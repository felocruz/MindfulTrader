// observation_vector_recalibration.cpp -- single-pass, real-tick-data
// FeatureScaler recalibration for every dim FeatureScaler.h currently flags
// as disabled-pending-audit or never-calibrated (audit performed 2026-09-03):
// dim0 (log_scale_ratio), dim1 (burstiness_index, SHRINKAGE_SCALE_MIN only --
// STATE_WINSOR_SIGMA was already re-validated separately), dim3
// (log_scale_expansion_ratio), dim8 (fast_hurst_exponent), dim10
// (skewness_idx), dim11 (amihud_illiquidity), dim12 (liq_fragility), dim13
// (fast_taleb_kurtosis), dim16 (mean_rev_z).
//
// Consolidates what would otherwise be ~9 separate tools each independently
// re-streaming the same 471.9M-row mes_ticks.parquet (~1h43m each) into ONE
// pass: every tick feeds five independent "clocks"/consumers simultaneously
// (mirroring how ContextManager itself processes ticks once per production
// tick, not per-dim):
//   - TS1 240-min bars  -> log_scale_ratio (dim0)
//   - TS2  60-min bars  -> log_scale_expansion_ratio (dim3)
//   - TS3  15-min bars  -> mean_rev_z (dim16), amihud_illiquidity (dim11,
//     tick-reactive intra-bar per StudyHelperFunctions.cpp's live-reactivity
//     fix), liq_fragility (dim12, same)
//   - tick-level, no bar -> burstiness_index (dim1)
//   - activity-clock (imbalance bars) -> fast_hurst_exponent (dim8),
//     skewness_idx (dim10), fast_taleb_kurtosis (dim13)
//
// GPD (Generalized Pareto Distribution) tail fit, ADDED 2026-09-03 after a
// real gap was found: an earlier, never-committed revision of this tool's
// amihud/liq_fragility-only predecessor did compute a proper Peaks-Over-
// Threshold GPD fit (u=p99, Method of Moments, p=1/N return level -- the
// same "GPD-derived, p=1/N return level" convention FeatureScaler.h's own
// comments use throughout), but that code was lost across this session's
// rebuilds and never re-added -- this tool only reported raw percentiles
// until that gap was caught. FitGPD() below restores it, generically, for
// all 9 dims in one pass -- see FitGPD()'s own comment for the exact
// methodology (Hosking & Wallis 1987).
//
// Every underlying math primitive reused as-is, zero porting needed (all
// already sc-free, confirmed by inspection before writing this file):
// include/BipowerVariation.h, include/RobustMoments.h (BowleySkewness/
// MoorsKurtosis), include/DfaHurstExponent.h, include/EventVelocityEngine.h
// (CalculateBurstinessIndex), include/ImbalanceBarEngine.h. mean_rev_z's
// median/MAD formula and amihud_illiquidity/liq_fragility's live-reactive
// formulas have no standalone header yet, so they're faithfully ported/
// reused inline below (the liq_fragility/amihud logic is a direct port of
// tools/observation_vector/amihud_liqfragility_recalibration.cpp's own
// already-validated bar/tick logic, not a re-derivation).
//
// Every raw value is fed through the REAL FeatureScaler (include/
// FeatureScaler.h, unmodified) via UpdateAndNormalize(), so the z-score/
// exceedance-rate statistics reported are against the actual production
// scaling code, not a re-derivation of it.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   -Iinclude tools/observation_vector/observation_vector_recalibration.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/observation_vector_recalibration
// Usage: ./tools/bin/observation_vector_recalibration \
//   --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet

#include "BipowerVariation.h"
#include "DfaHurstExponent.h"
#include "EventVelocityEngine.h"
#include "FeatureScaler.h"
#include "ImbalanceBarEngine.h"
#include "RingBuffer.h"
#include "RobustMoments.h"
#include "market_data_io.h"
#include "../ToolProgressLogger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kTotalTicksEstimate = 471'930'891;

// ---------------------------------------------------------------------
// Peaks-Over-Threshold GPD fit, Method of Moments (Hosking & Wallis 1987) --
// restores the methodology an earlier, never-committed tool revision had
// (see file header). threshold u = 99th percentile of the retained sample;
// exceedances y_i = x_i - u for every x_i > u; return level is the value
// expected to be exceeded once per `totalObservedCount` real observations
// of this dim (matches FeatureScaler.h's own "p=1/N return level" phrasing).
// ---------------------------------------------------------------------
struct GPDFit {
    double u = 0.0;
    std::size_t nTail = 0;
    std::size_t nSample = 0;
    std::size_t totalObserved = 0;
    double xi = 0.0;
    double sigma = 0.0;
    double returnLevel = 0.0;
    bool valid = false;
};

GPDFit FitGPD(std::vector<float> values, std::size_t totalObservedCount) {
    GPDFit fit;
    fit.nSample = values.size();
    fit.totalObserved = totalObservedCount;
    if (fit.nSample < 200) return fit;  // too few samples for a trustworthy tail fit

    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    const std::size_t p99Idx = std::min(static_cast<std::size_t>(0.99 * static_cast<double>(n)), n - 1);
    fit.u = static_cast<double>(values[p99Idx]);

    std::vector<double> exceedances;
    exceedances.reserve(n - p99Idx);
    for (std::size_t i = p99Idx; i < n; ++i) {
        const double y = static_cast<double>(values[i]) - fit.u;
        if (y > 0.0) exceedances.push_back(y);
    }
    fit.nTail = exceedances.size();
    if (fit.nTail < 20) return fit;  // too few exceedances for a trustworthy MOM fit

    double sum = 0.0, sumSq = 0.0;
    for (double y : exceedances) { sum += y; sumSq += y * y; }
    const double mean = sum / static_cast<double>(fit.nTail);
    const double var = (sumSq / static_cast<double>(fit.nTail)) - mean * mean;
    if (var <= 0.0 || mean <= 0.0) return fit;

    // Hosking & Wallis (1987) Method of Moments estimators.
    fit.xi = 0.5 * (1.0 - (mean * mean) / var);
    fit.sigma = 0.5 * mean * (1.0 + (mean * mean) / var);

    // p = 1/N return level: N = totalObservedCount (this dim's real
    // observation count -- bar closes or, for burstiness, ticks), matching
    // FeatureScaler.h's own convention throughout.
    const double zetaU = static_cast<double>(fit.nTail) / static_cast<double>(n);  // P(X > u) within the retained sample
    const double m = static_cast<double>(fit.totalObserved) * zetaU;
    if (std::fabs(fit.xi) < 1e-6) {
        fit.returnLevel = fit.u + fit.sigma * std::log(std::max(m, 1.0));
    } else {
        fit.returnLevel = fit.u + (fit.sigma / fit.xi) * (std::pow(std::max(m, 1.0), fit.xi) - 1.0);
    }
    fit.valid = true;
    return fit;
}

// ---------------------------------------------------------------------
// Per-dim collector: same mean|z|/max|z|/rate-at-bound reporting as before,
// PLUS full retention of every |z| sample for the GPD fit above -- feasible
// here because every dim except burstiness_index fires at most once per
// bar close (thousands-to-low-hundred-thousands of samples over the whole
// 471.9M-tick history, not per-tick volume). Burstiness_index (tick-level)
// subsamples 1-in-50, same as this tool family's established convention.
// ---------------------------------------------------------------------
struct TailStats {
    std::size_t n = 0;
    double sumAbsZ = 0.0;
    float maxAbsZ = 0.0f;
    std::vector<float> retained;
    std::size_t subsampleEveryN = 1;  // 1 = retain every sample; >1 = retain every Nth

    void Record(float z) {
        ++n;
        const float az = std::fabs(z);
        sumAbsZ += az;
        if (az > maxAbsZ) maxAbsZ = az;
        if (subsampleEveryN <= 1 || (n % subsampleEveryN) == 0) retained.push_back(az);
    }

    double RateAt(float threshold) const {
        if (retained.empty()) return 0.0;
        std::size_t hits = 0;
        for (float az : retained) if (az >= threshold) ++hits;
        return 100.0 * hits / retained.size();
    }

    void Report(const char* name, float currentBound, ToolProgressLogger& logger) const {
        char line[512];
        if (n == 0) {
            std::snprintf(line, sizeof(line), "%s: NO SAMPLES (dim never left warmup/degenerate guard)", name);
            std::puts(line); logger.Log(line);
            return;
        }
        std::snprintf(line, sizeof(line),
                      "%s: n=%zu mean|z|=%.4f max|z|=%.2f rate-at-bound(%.1f)=%.4f%%",
                      name, n, sumAbsZ / static_cast<double>(n), maxAbsZ, currentBound, RateAt(currentBound));
        std::puts(line); logger.Log(line);

        const GPDFit fit = FitGPD(retained, n);
        if (!fit.valid) {
            std::snprintf(line, sizeof(line), "%s: GPD fit SKIPPED (insufficient tail samples: nSample=%zu nTail=%zu)",
                          name, fit.nSample, fit.nTail);
            std::puts(line); logger.Log(line);
            return;
        }
        const char* domain = (fit.xi > 0.05) ? "Frechet/unbounded" : (fit.xi < -0.05 ? "Weibull/bounded" : "Gumbel/borderline");
        std::snprintf(line, sizeof(line),
                      "%s: GPD fit u=p99=%.4f n_tail=%zu xi=%+.4f (%s) sigma=%.4f  p=1/N return level (N=%zu) = %.4f  [CURRENT BOUND=%.1f]",
                      name, fit.u, fit.nTail, fit.xi, domain, fit.sigma, fit.totalObserved, fit.returnLevel, currentBound);
        std::puts(line); logger.Log(line);
    }
};

// ---------------------------------------------------------------------
// Faithful port of CalculateMeanReversionSpeed's median/MAD formula
// (StudyHelperFunctions.cpp) -- no existing sc-free header for this one.
// Operates on a trailing window of CLOSED-bar closing prices, oldest-first;
// n must be >= 5 (matches production's std::clamp(...,5,40)).
// ---------------------------------------------------------------------
constexpr int kMeanRevMaxLookback = 40;
constexpr double kMadConsistency = 1.4826;

float ComputeMeanRevZ(const float* closes, int n) {
    n = std::clamp(n, 5, kMeanRevMaxLookback);
    constexpr double kPriceEps = 1e-6;

    std::array<double, kMeanRevMaxLookback> log_prices{};
    for (int i = 0; i < n; ++i) {
        log_prices[static_cast<std::size_t>(i)] = std::log(std::max(static_cast<double>(closes[i]), kPriceEps));
    }

    std::array<double, kMeanRevMaxLookback> scratch{};
    std::copy_n(log_prices.begin(), n, scratch.begin());
    const int priceMid = n / 2;
    std::nth_element(scratch.begin(), scratch.begin() + priceMid, scratch.begin() + n);
    const double median_log_p = scratch[static_cast<std::size_t>(priceMid)];

    for (int i = 0; i < n; ++i) {
        scratch[static_cast<std::size_t>(i)] = std::abs(log_prices[static_cast<std::size_t>(i)] - median_log_p);
    }
    std::nth_element(scratch.begin(), scratch.begin() + priceMid, scratch.begin() + n);
    const double mad_log_p = scratch[static_cast<std::size_t>(priceMid)];
    const double scale_log_p = mad_log_p * kMadConsistency;
    if (scale_log_p < 1e-6) return 0.0f;  // degenerate: caller carries forward, this port has no persistent state

    const double current_log_p = log_prices[static_cast<std::size_t>(n - 1)];
    const double abs_z_price = std::abs((current_log_p - median_log_p) / scale_log_p);

    const int m = n - 1;
    if (m < 3) {
        return std::clamp(static_cast<float>(abs_z_price), 0.0f, 5.0f);
    }

    std::array<double, kMeanRevMaxLookback> returns{};
    for (int i = 0; i < m; ++i) {
        returns[static_cast<std::size_t>(i)] = log_prices[static_cast<std::size_t>(i + 1)] - log_prices[static_cast<std::size_t>(i)];
    }
    std::array<double, kMeanRevMaxLookback> retScratch{};
    std::copy_n(returns.begin(), m, retScratch.begin());
    const int retMid = m / 2;
    std::nth_element(retScratch.begin(), retScratch.begin() + retMid, retScratch.begin() + m);
    const double median_r = retScratch[static_cast<std::size_t>(retMid)];

    double num = 0.0, den = 0.0;
    for (int t = 1; t < m; ++t) {
        const double r_t = returns[static_cast<std::size_t>(t)] - median_r;
        const double r_prev = returns[static_cast<std::size_t>(t - 1)] - median_r;
        num += r_t * r_prev;
        den += r_prev * r_prev;
    }
    const double rho = (den > 1e-12) ? (num / den) : 0.0;
    const double elasticity_gate = std::clamp(1.0 - std::max(rho, 0.0), 0.0, 1.0);
    return std::clamp(static_cast<float>(abs_z_price * elasticity_gate), 0.0f, 5.0f);
}

// log(short_BV/long_BV) over a trailing return window -- shared by both
// log_scale_ratio (TS1) and log_scale_expansion_ratio (TS2), differing only
// in bar period and long_n. short_n = max(8, long_n/4), matching production's
// CalculateLogScaleRatio derivation.
float LogScaleRatioFromReturns(const double* returns, int available, int long_n) {
    const int short_n = std::max(8, long_n / 4);
    if (available < long_n) return 0.0f;
    const double longBV = ComputeBipowerVariation(returns + (available - long_n), long_n);
    const double shortBV = ComputeBipowerVariation(returns + (available - short_n), short_n);
    constexpr double kEps = 1e-12;
    const double ratio = std::log((shortBV + kEps) / (longBV + kEps));
    return static_cast<float>(std::clamp(ratio, -6.0, 6.0));
}

// ---------------------------------------------------------------------
// amihud_illiquidity / liq_fragility -- direct port of
// amihud_liqfragility_recalibration.cpp's own already-validated bar/tick
// logic (2026-09-03 reformulations), not a re-derivation.
// ---------------------------------------------------------------------
constexpr int kAmihudLookbackN = 20;
constexpr std::size_t kLiqFragWindow = 30;
constexpr float kLiveBarMinVolume = 50.0f;
constexpr std::size_t kBarHistoryCapacity = 32;
constexpr double kRatioEps = 1e-20;

struct AmihudBar {
    float close = 0.0f;
    float volume = 0.0f;
    float range = 0.0f;
};

float LiqFragilityLive(float liveHigh, float liveLow, float liveVolumeSoFar, float medRange, float medSqrtVol,
                        float prevFragility) {
    constexpr float kEps = 1e-6f;
    const float scaleRef = medRange / (medSqrtVol + kEps);
    if (liveVolumeSoFar < kLiveBarMinVolume || scaleRef < kEps) {
        return std::clamp(prevFragility, 0.0f, 1.0f);
    }
    float barRange = liveHigh - liveLow;
    if (barRange < 0.00001f) barRange = 0.00001f;
    const float eta = barRange / (std::sqrt(liveVolumeSoFar) + kEps);
    const float fRaw = eta / scaleRef;
    const float logF = std::log(std::max(fRaw, 1e-6f));
    const float fragilityRaw = 1.0f / (1.0f + std::exp(-2.0f * logF));
    const float alpha = (fragilityRaw > prevFragility) ? 0.30f : 0.15f;
    return std::clamp(alpha * fragilityRaw + (1.0f - alpha) * prevFragility, 0.0f, 1.0f);
}

// --dims= group gating -- lets this tool be run as several cheaper,
// parallel passes over the SAME full, untruncated tick stream instead of
// one expensive all-in-one pass. Each group only pays the per-tick cost of
// the clocks its own dims actually need (e.g. "logscale" skips the
// per-tick burstiness ring-buffer AND the per-tick imbalance-bar engine
// entirely -- both real, measured costs, not assumed).
enum class DimGroup { kAll, kBurstiness, kLogScale, kAmihudMeanRevZ, kActivity };

DimGroup ParseDimGroup(const std::string& s) {
    if (s == "burstiness") return DimGroup::kBurstiness;
    if (s == "logscale") return DimGroup::kLogScale;
    if (s == "amihud") return DimGroup::kAmihudMeanRevZ;
    if (s == "activity") return DimGroup::kActivity;
    return DimGroup::kAll;
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticksPath;
    std::string dimsFlag = "all";
    // 2026-09-03: this tool's own real crash -- 4 dim-group passes launched
    // concurrently over the same 471.9M-row file exhausted RAM (no swap
    // configured) and took the whole VS Code/WSL session down with them,
    // with the log showing only progress percentages and no memory trend
    // to explain why. Default budget assumes 3-4 concurrent passes sharing
    // a ~15GB box; override via --max-rss-mb for a different concurrency.
    std::size_t maxRssMB = 3072;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks-parquet") == 0 && i + 1 < argc) {
            ticksPath = argv[++i];
        } else if (std::strcmp(argv[i], "--dims") == 0 && i + 1 < argc) {
            dimsFlag = argv[++i];
        } else if (std::strcmp(argv[i], "--max-rss-mb") == 0 && i + 1 < argc) {
            maxRssMB = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        }
    }
    if (ticksPath.empty()) {
        std::fprintf(stderr, "usage: %s --ticks-parquet PATH [--dims all|burstiness|logscale|amihud|activity] [--max-rss-mb N]\n", argv[0]);
        return 1;
    }
    const DimGroup group = ParseDimGroup(dimsFlag);
    const bool wantBurstiness = (group == DimGroup::kAll || group == DimGroup::kBurstiness);
    const bool wantLogScale = (group == DimGroup::kAll || group == DimGroup::kLogScale);
    const bool wantAmihudMeanRevZ = (group == DimGroup::kAll || group == DimGroup::kAmihudMeanRevZ);
    const bool wantActivity = (group == DimGroup::kAll || group == DimGroup::kActivity);

    FeatureScaler fs;
    ToolProgressLogger progress("observation_vector_recalibration_" + dimsFlag);
    progress.Log("streaming from " + ticksPath + " (dims=" + dimsFlag + ", max-rss-mb=" + std::to_string(maxRssMB) + ")");
    constexpr std::size_t kProgressEveryNTicks = 5'000'000;

    TailStats zLogScaleRatio, zLogScaleExpansion, zBurstiness, zMeanRevZ,
              zFastHurst, zSkewness, zFastKurtosis, zAmihud, zLiqFrag;
    zBurstiness.subsampleEveryN = 50;  // tick-level volume -- same subsampling this tool family already uses
    // amihud_illiquidity/liq_fragility are ALSO tick-reactive intra-bar (see file header),
    // not bar-close-only like the other TailStats dims -- missing this subsample (found
    // 2026-09-04: unbounded retained-vector growth hit the RSS budget guard at ~4.1GB,
    // 63% through a real run) let them retain one float per tick same as burstiness needs.
    zAmihud.subsampleEveryN = 50;
    zLiqFrag.subsampleEveryN = 50;

    // --- TS1 (240min) / TS2 (60min) close-bar aggregation + return history ---
    constexpr long long kTs1BarUs = 240LL * 60 * 1'000'000;
    constexpr long long kTs2BarUs = 60LL * 60 * 1'000'000;
    constexpr int kTs1LongN = 20;
    constexpr int kTs2LongN = 20;
    constexpr std::size_t kReturnHistCap = 64;

    long long ts1Bucket = -1, ts2Bucket = -1;
    float ts1CurClose = 0.0f, ts2CurClose = 0.0f;
    RingBuffer<double, kReturnHistCap> ts1Returns, ts2Returns;
    float ts1PrevClose = 0.0f, ts2PrevClose = 0.0f;
    bool ts1HavePrev = false, ts2HavePrev = false;

    // --- TS3 (15min) bar aggregation: mean_rev_z, amihud_illiquidity, liq_fragility ---
    constexpr long long kTs3BarUs = 15LL * 60 * 1'000'000;
    constexpr int kMeanRevLookback = 20;
    constexpr std::size_t kCloseBarCap = 48;
    long long ts3Bucket = -1;
    float ts3CurClose = 0.0f;
    RingBuffer<float, kCloseBarCap> ts3Closes;

    RingBuffer<AmihudBar, kBarHistoryCapacity> amihudBarHistory;
    float liqFragMedRange = 0.0f, liqFragMedSqrtVol = 0.0f;
    float ts3CurHigh = -1e30f, ts3CurLow = 1e30f, ts3CurVolume = 0.0f;
    double amihudHistSumLogRatio = 0.0;
    int amihudHistCount = 0;
    float liqFragPrev = 0.0f;
    std::size_t ts3BarsClosed = 0;

    // --- burstiness_index: tick-level timestamp ring buffer ---
    RingBuffer<uint64_t, 100> tickTimestamps;

    // --- Activity-clock trio: shared ImbalanceBarEngine + 100-return buffer ---
    ImbalanceBarEngine imbalanceEngine;
    long long ts3BarIndexForImbalance = 0;
    long long lastImbalanceBucket = -1;
    long long askVolBarSum = 0, bidVolBarSum = 0;
    std::size_t lastImbalanceBarCount = 0;

    // Single persistent observation vector, matching production's own calling
    // convention exactly (ContextManager.cpp: ONE UpdateAndNormalize() call
    // per tick against a continuously-updated array, not a fresh zero-filled
    // array per dim per call). Declared here (not per-tick) so it survives
    // across ticks; dims only get overwritten when their own clock actually
    // produces a new reading -- FeatureScaler's dedupe-at-ingestion then
    // correctly treats an unchanged value as carrying zero new information.
    std::array<float, FeatureScaler::N_DIMS> rawObs{};

    auto recomputeAmihudHistorical = [&]() {
        amihudHistSumLogRatio = 0.0;
        amihudHistCount = 0;
        const std::size_t n = amihudBarHistory.size();
        if (n < 1) return;
        const std::size_t maxPairs = std::min<std::size_t>(kAmihudLookbackN, n - 1);
        for (std::size_t k = 0; k < maxPairs; ++k) {
            const std::size_t curIdx = n - 1 - k;
            const std::size_t prevIdx = curIdx - 1;
            const AmihudBar& cur = amihudBarHistory[curIdx];
            const AmihudBar& prev = amihudBarHistory[prevIdx];
            if (cur.volume < 1.0f || cur.close <= 0.0f || prev.close <= 0.0f) continue;
            const double dollarVol = static_cast<double>(cur.close) * static_cast<double>(cur.volume);
            if (dollarVol < 1.0) continue;
            const double logRet = std::fabs(std::log(static_cast<double>(cur.close) / prev.close));
            const double ratio = logRet / std::sqrt(dollarVol);
            amihudHistSumLogRatio += std::log(ratio + kRatioEps);
            ++amihudHistCount;
        }
    };

    auto recomputeLiqFragScaleRef = [&]() {
        const std::size_t n = amihudBarHistory.size();
        const std::size_t w = std::min<std::size_t>(kLiqFragWindow, n);
        if (w == 0) { liqFragMedRange = 0.0f; liqFragMedSqrtVol = 0.0f; return; }
        std::array<float, kLiqFragWindow> rangeBuf{}, sqrtVolBuf{};
        for (std::size_t k = 0; k < w; ++k) {
            const AmihudBar& b = amihudBarHistory[n - w + k];
            rangeBuf[k] = b.range;
            sqrtVolBuf[k] = std::sqrt(std::max(b.volume, 0.0f));
        }
        const std::size_t mid = w / 2;
        auto rangeScratch = rangeBuf;
        std::nth_element(rangeScratch.begin(), rangeScratch.begin() + mid, rangeScratch.begin() + w);
        liqFragMedRange = rangeScratch[mid];
        auto volScratch = sqrtVolBuf;
        std::nth_element(volScratch.begin(), volScratch.begin() + mid, volScratch.begin() + w);
        liqFragMedSqrtVol = volScratch[mid];
    };

    auto onNewContractOrStart = [&]() {
        ts1Bucket = ts2Bucket = ts3Bucket = -1;
        ts1Returns.clear(); ts2Returns.clear(); ts3Closes.clear();
        ts1HavePrev = ts2HavePrev = false;
        tickTimestamps.clear();
        imbalanceEngine.Reset();
        ts3BarIndexForImbalance = 0;
        lastImbalanceBucket = -1;
        askVolBarSum = bidVolBarSum = 0;
        lastImbalanceBarCount = 0;
        amihudBarHistory.clear();
        liqFragMedRange = liqFragMedSqrtVol = 0.0f;
        amihudHistSumLogRatio = 0.0;
        amihudHistCount = 0;
        liqFragPrev = 0.0f;
        ts3BarsClosed = 0;
        rawObs.fill(0.0f);
    };

    std::size_t ticksProcessed = 0;
    constexpr std::size_t kInterimReportEveryNTicks = 50'000'000;

    // Same report a full run only ever printed once at the very end (~3hr away) --
    // 2026-09-03: a real correctness bug (recording post-transform instead of raw z)
    // wasn't visible until then. Calling this every kInterimReportEveryNTicks makes a
    // wrong fix visible in ~10-15min instead of hours, and leaves a real correctness
    // signal in the log even if the run is killed early.
    //
    // "currentBound" reference for each dim (2026-09-04 fix): must reflect the REAL
    // configured FeatureScaler bound, not a hardcoded guess -- dim11's report used a
    // stale hardcoded 6.0f even after DIM_WINSOR_SIGMA_OVERRIDE[11] was recalibrated to
    // 3036.0f, silently misreporting rate-at-bound/[CURRENT BOUND=] for that dim only.
    // Mirrors FeatureScaler::UpdateAndNormalize's own override-else-generic-default
    // fallback so this can never drift out of sync with production again.
    auto currentBoundFor = [](std::size_t dim) {
        return (FeatureScaler::DIM_WINSOR_SIGMA_OVERRIDE[dim] > 0.0f)
            ? FeatureScaler::DIM_WINSOR_SIGMA_OVERRIDE[dim] : FeatureScaler::STATE_WINSOR_SIGMA;
    };
    auto writeReport = [&](const char* label) {
        progress.Log(std::string(label) + " (" + std::to_string(ticksProcessed) + " ticks so far)");
        zLogScaleRatio.Report(     "dim0  log_scale_ratio          ", currentBoundFor(MTS::Schema::Contract::kObsLogScaleRatio), progress);
        zBurstiness.Report(        "dim1  burstiness_index (shrink)", currentBoundFor(MTS::Schema::Contract::kObsBurstinessIndex), progress);
        zLogScaleExpansion.Report( "dim3  log_scale_expansion_ratio", currentBoundFor(MTS::Schema::Contract::kObsLogScaleExpansionRatio), progress);
        zFastHurst.Report(         "dim8  fast_hurst_exponent      ", currentBoundFor(MTS::Schema::Contract::kObsFastHurstExponent), progress);
        zSkewness.Report(          "dim10 skewness_idx             ", currentBoundFor(MTS::Schema::Contract::kObsSkewnessIdx), progress);
        zAmihud.Report(            "dim11 amihud_illiquidity       ", currentBoundFor(MTS::Schema::Contract::kObsAmihudIlliquidity), progress);
        zLiqFrag.Report(           "dim12 liq_fragility            ", 21.26f, progress);  // LOGZ_WINSOR_SIGMA_OVERRIDE, a separate array -- unaffected by this fix
        zFastKurtosis.Report(      "dim13 fast_taleb_kurtosis       ", currentBoundFor(MTS::Schema::Contract::kObsFastTalebKurtosis), progress);
        zMeanRevZ.Report(          "dim16 mean_rev_z               ", currentBoundFor(MTS::Schema::Contract::kObsMeanRevZ), progress);
    };

    try {
    StreamTicksFullParquet(ticksPath, [&](std::int64_t ts, double price, std::int64_t volume,
                                          std::int64_t askVol, std::int64_t bidVol, bool isNewContract) {
        ++ticksProcessed;
        if (isNewContract) onNewContractOrStart();
        if (ticksProcessed % kProgressEveryNTicks == 0) {
            progress.LogProgress(ticksProcessed, kTotalTicksEstimate);
            progress.CheckMemoryBudget(maxRssMB);
        }
        if (ticksProcessed % kInterimReportEveryNTicks == 0) {
            writeReport("interim report");
        }
        if (price <= 0.0) return;
        const float p = static_cast<float>(price);

        bool freshAmihudLiqFrag = false;
        bool freshMeanRevZ = false;
        bool freshLogScaleRatio = false;
        bool freshLogScaleExpansion = false;
        bool freshHurstSkewKurt = false;

        // --- burstiness_index (dim1): tick-level, no bar -- always fresh ---
        if (wantBurstiness) {
            tickTimestamps.push_back(static_cast<uint64_t>(ts));
            if (tickTimestamps.size() == 100) tickTimestamps.pop_front();
            rawObs[MTS::Schema::Contract::kObsBurstinessIndex] = eve::CalculateBurstinessIndex(tickTimestamps);
        }

        // --- TS1 240-min bar: log_scale_ratio (dim0) ---
        if (wantLogScale) {
            const long long bucket = ts / kTs1BarUs;
            if (ts1Bucket == -1) { ts1Bucket = bucket; ts1CurClose = p; }
            if (bucket != ts1Bucket) {
                if (ts1HavePrev && ts1PrevClose > 0.0f) {
                    if (ts1Returns.size() == kReturnHistCap) ts1Returns.pop_front();
                    ts1Returns.push_back(std::log(static_cast<double>(ts1CurClose) / ts1PrevClose));
                }
                ts1PrevClose = ts1CurClose;
                ts1HavePrev = true;
                ts1Bucket = bucket;

                if (ts1Returns.size() >= static_cast<std::size_t>(kTs1LongN)) {
                    std::array<double, kReturnHistCap> buf{};
                    for (std::size_t i = 0; i < ts1Returns.size(); ++i) buf[i] = ts1Returns[i];
                    rawObs[MTS::Schema::Contract::kObsLogScaleRatio] =
                        LogScaleRatioFromReturns(buf.data(), static_cast<int>(ts1Returns.size()), kTs1LongN);
                    freshLogScaleRatio = true;
                }
            }
            ts1CurClose = p;
        }

        // --- TS2 60-min bar: log_scale_expansion_ratio (dim3) ---
        if (wantLogScale) {
            const long long bucket = ts / kTs2BarUs;
            if (ts2Bucket == -1) { ts2Bucket = bucket; ts2CurClose = p; }
            if (bucket != ts2Bucket) {
                if (ts2HavePrev && ts2PrevClose > 0.0f) {
                    if (ts2Returns.size() == kReturnHistCap) ts2Returns.pop_front();
                    ts2Returns.push_back(std::log(static_cast<double>(ts2CurClose) / ts2PrevClose));
                }
                ts2PrevClose = ts2CurClose;
                ts2HavePrev = true;
                ts2Bucket = bucket;

                if (ts2Returns.size() >= static_cast<std::size_t>(kTs2LongN)) {
                    std::array<double, kReturnHistCap> buf{};
                    for (std::size_t i = 0; i < ts2Returns.size(); ++i) buf[i] = ts2Returns[i];
                    rawObs[MTS::Schema::Contract::kObsLogScaleExpansionRatio] =
                        LogScaleRatioFromReturns(buf.data(), static_cast<int>(ts2Returns.size()), kTs2LongN);
                    freshLogScaleExpansion = true;
                }
            }
            ts2CurClose = p;
        }

        // --- TS3 15-min bar: mean_rev_z + amihud_illiquidity + liq_fragility ---
        // Bucket/high/low/volume tracking is shared by both the amihud/mean_rev_z
        // group and the activity-clock group (which needs the bar index + volume
        // sums for ImbalanceBarEngine) -- only the group-specific inner blocks below
        // are individually gated.
        if (wantAmihudMeanRevZ || wantActivity) {
            const long long bucket = ts / kTs3BarUs;
            if (ts3Bucket == -1) {
                ts3Bucket = bucket;
                ts3CurClose = p;
                ts3CurHigh = ts3CurLow = p;
                ts3CurVolume = 0.0f;
            } else if (bucket != ts3Bucket) {
                if (wantAmihudMeanRevZ) {
                    if (ts3Closes.size() == kCloseBarCap) ts3Closes.pop_front();
                    ts3Closes.push_back(ts3CurClose);
                    if (ts3Closes.size() >= static_cast<std::size_t>(kMeanRevLookback)) {
                        std::array<float, kCloseBarCap> buf{};
                        for (std::size_t i = 0; i < ts3Closes.size(); ++i) buf[i] = ts3Closes[i];
                        const std::size_t nCloses = ts3Closes.size();
                        rawObs[MTS::Schema::Contract::kObsMeanRevZ] =
                            ComputeMeanRevZ(buf.data() + (nCloses - kMeanRevLookback), kMeanRevLookback);
                        freshMeanRevZ = true;
                    }

                    if (amihudBarHistory.size() == kBarHistoryCapacity) amihudBarHistory.pop_front();
                    amihudBarHistory.push_back({ts3CurClose, ts3CurVolume, ts3CurHigh - ts3CurLow});
                    ++ts3BarsClosed;
                    recomputeAmihudHistorical();
                    recomputeLiqFragScaleRef();
                }

                ts3Bucket = bucket;
                ts3CurHigh = ts3CurLow = p;
                ts3CurVolume = 0.0f;
                ++ts3BarIndexForImbalance;
            }
            ts3CurClose = p;
            ts3CurHigh = std::max(ts3CurHigh, p);
            ts3CurLow = std::min(ts3CurLow, p);
            ts3CurVolume += static_cast<float>(volume);

            if (wantAmihudMeanRevZ && ts3BarsClosed >= 100) {
                const float prevBarClose = amihudBarHistory.empty() ? ts3CurClose : amihudBarHistory.back().close;
                float amihud = 0.0f;
                {
                    double sumLogRatio = amihudHistSumLogRatio;
                    int count = amihudHistCount;
                    if (ts3CurVolume >= kLiveBarMinVolume && ts3CurClose > 0.0f && prevBarClose > 0.0f) {
                        const double dollarVol = static_cast<double>(ts3CurClose) * static_cast<double>(ts3CurVolume);
                        if (dollarVol >= 1.0) {
                            const double logRet = std::fabs(std::log(static_cast<double>(ts3CurClose) / prevBarClose));
                            const double ratio = logRet / std::sqrt(dollarVol);
                            sumLogRatio += std::log(ratio + kRatioEps);
                            ++count;
                        }
                    }
                    if (count >= 2) amihud = static_cast<float>(std::exp(sumLogRatio / count));
                }
                const float liqFrag = LiqFragilityLive(ts3CurHigh, ts3CurLow, ts3CurVolume, liqFragMedRange,
                                                        liqFragMedSqrtVol, liqFragPrev);
                liqFragPrev = liqFrag;
                rawObs[MTS::Schema::Contract::kObsAmihudIlliquidity] = amihud;
                rawObs[MTS::Schema::Contract::kObsLiqFragility] = liqFrag;
                freshAmihudLiqFrag = true;
            }

            if (wantActivity) {
                if (lastImbalanceBucket != ts3Bucket) {
                    askVolBarSum = bidVolBarSum = 0;
                    lastImbalanceBucket = ts3Bucket;
                }
                askVolBarSum += askVol;
                bidVolBarSum += bidVol;
                imbalanceEngine.OnTickWithPrice(static_cast<int>(ts3BarIndexForImbalance),
                                                 static_cast<float>(askVolBarSum),
                                                 static_cast<float>(bidVolBarSum), p);
            }
        }

        // --- Activity-clock trio: fast_hurst_exponent/skewness_idx/fast_taleb_kurtosis ---
        if (wantActivity) {
            const std::size_t barCount = imbalanceEngine.GetCompletedBarCount();
            if (barCount != lastImbalanceBarCount && barCount >= 100) {
                lastImbalanceBarCount = barCount;
                std::array<float, 100> returns{};
                imbalanceEngine.GetImbalanceBarReturns(100, returns.data());

                const float hurstRaw = DfaHurstExponent(returns.data(), 100, 8);
                if (std::isfinite(hurstRaw)) {
                    rawObs[MTS::Schema::Contract::kObsFastHurstExponent] = hurstRaw;
                }
                // BowleySkewness()/MoorsKurtosis() return NaN on a degenerate window
                // (RobustMoments.h's documented contract) -- mirrors the carry-forward
                // guard just added to ContextManager.cpp's own production call site
                // (found via this exact tool's "activity" run showing mean|z|=nan for
                // both dims from the first checkpoint onward, 2026-09-04).
                static float s_lastValidSkewnessIdx = 0.0f;
                const float skewnessRaw = BowleySkewness(returns);
                if (std::isfinite(skewnessRaw)) {
                    rawObs[MTS::Schema::Contract::kObsSkewnessIdx] = skewnessRaw;
                    s_lastValidSkewnessIdx = skewnessRaw;
                } else {
                    rawObs[MTS::Schema::Contract::kObsSkewnessIdx] = s_lastValidSkewnessIdx;
                }
                static float s_lastValidFastTalebKurtosis = 1.23f;
                const float kurtosisRaw = MoorsKurtosis(returns);
                if (std::isfinite(kurtosisRaw)) {
                    rawObs[MTS::Schema::Contract::kObsFastTalebKurtosis] = kurtosisRaw;
                    s_lastValidFastTalebKurtosis = kurtosisRaw;
                } else {
                    rawObs[MTS::Schema::Contract::kObsFastTalebKurtosis] = s_lastValidFastTalebKurtosis;
                }
                freshHurstSkewKurt = true;
            }
        }

        // --- Single UpdateAndNormalize() call per tick, matching production exactly ---
        // NOTE (bug found + fixed 2026-09-03): this must record fs.lastRawZ[dim], the
        // PRE-ToSoftLogZ/pre-clamp raw z-score -- NOT UpdateAndNormalize()'s own return
        // value, which is already winsorized/shrinkage-compressed production output.
        // Recording the post-transform value here was circular (measuring a signal
        // against bounds already baked into that same signal) and produced degenerate,
        // wrong recalibration numbers for every dim in this file (verified: dim1's own
        // dedicated run showed max|z|=1.95 here vs the already real-data-validated
        // max|z|=49.28 in tools/observation_vector/burstiness_recalibration.cpp, which
        // correctly records fs.lastRawZ[1]). Also fixed: zBurstiness.Record() was
        // unconditional (ungated by wantBurstiness), so every non-burstiness dims= run
        // recorded 471M meaningless all-zero samples into a report line that looked real.
        fs.UpdateAndNormalize(rawObs);
        if (!fs.warmedUp) return;

        if (wantBurstiness) zBurstiness.Record(fs.lastRawZ[MTS::Schema::Contract::kObsBurstinessIndex]);
        if (freshLogScaleRatio) zLogScaleRatio.Record(fs.lastRawZ[MTS::Schema::Contract::kObsLogScaleRatio]);
        if (freshLogScaleExpansion) zLogScaleExpansion.Record(fs.lastRawZ[MTS::Schema::Contract::kObsLogScaleExpansionRatio]);
        if (freshMeanRevZ) zMeanRevZ.Record(fs.lastRawZ[MTS::Schema::Contract::kObsMeanRevZ]);
        if (freshAmihudLiqFrag) {
            zAmihud.Record(fs.lastRawZ[MTS::Schema::Contract::kObsAmihudIlliquidity]);
            zLiqFrag.Record(fs.lastRawZ[MTS::Schema::Contract::kObsLiqFragility]);
        }
        if (freshHurstSkewKurt) {
            zFastHurst.Record(fs.lastRawZ[MTS::Schema::Contract::kObsFastHurstExponent]);
            zSkewness.Record(fs.lastRawZ[MTS::Schema::Contract::kObsSkewnessIdx]);
            zFastKurtosis.Record(fs.lastRawZ[MTS::Schema::Contract::kObsFastTalebKurtosis]);
        }
    });
    } catch (const std::exception& e) {
        progress.Log(std::string("aborted: ") + e.what());
        return 1;
    }

    writeReport("streaming done, final report");

    return 0;
}

