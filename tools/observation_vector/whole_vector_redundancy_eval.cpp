// whole_vector_redundancy_eval.cpp -- Elite Feature Set Curation Phase 1
// (docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md
// Phase 1): whole-vector, model-independent pairwise Pearson correlation
// audit across the ObservationData dims that have a verified, already-traced
// production formula, in ONE single streaming pass over real MES tick data.
//
// Rock-solid DOD, built specifically to avoid an OOM incident this tool
// FAMILY has already hit for real: observation_vector_recalibration.cpp's own
// header comment records that launching 4 CONCURRENT dim-group passes over
// the same 471.9M-row mes_ticks.parquet exhausted RAM (no swap configured)
// and took the whole session down, 2026-09-03. Two lessons applied here,
// directly:
//   1. ONE unified tool, not several tools meant to be run concurrently --
//      avoids the concurrent-process trap by construction, not by convention.
//   2. Whole-vector pairwise correlation has its OWN, additional single-pass
//      memory risk if done naively: retaining one std::vector<double> PER
//      DIM across the full 471.9M-tick history (volatility_dim_redundancy_
//      eval.cpp's own existing pattern, extended to ~10+ dims) would cost
//      D * 471.9M * 8 bytes -- 37+GB for just the raw series, a real OOM
//      regardless of concurrency. Avoided entirely via
//      streaming_correlation_matrix.h's O(D^2) online accumulator (Welford
//      1962 / West 1979) -- no observation, in any dim, is ever retained.
//   --max-rss-mb + ToolProgressLogger::CheckMemoryBudget() kept anyway as a
//   defense-in-depth safety net (this tool's own per-dim state -- ring
//   buffers, ImbalanceBarEngine, etc. -- is still bounded but non-trivial;
//   this catches any future regression that accidentally reintroduces
//   unbounded retention, same convention as observation_vector_
//   recalibration.cpp's own budget check).
//
// Scope, 2026-09-07, CORRECTED same day (operator directive: remove any dim already
// sourced from the activity/imbalance clock -- mixing them into a CALENDAR-clock
// redundancy audit conflates two observation vectors this system's two-HMM design
// (docs/superpowers/specs/2026-09-18-predator-sniper-execution-architecture.md
// §1.4) already decided must stay independent):
//   INCLUDED (11 dims, every formula traced to its real, live production call site,
//   ALL genuinely calendar-clock-native -- none read ImbalanceBarEngine/activity-
//   clock returns): log_scale_ratio(0), burstiness_index(1),
//   log_scale_expansion_ratio(3), amihud_illiquidity(11), liq_fragility(12),
//   mean_rev_z(16), relative_range(2), lempel_ziv(4), hurst_exponent(5),
//   fisher_info(7), fractal_dim(15) [ADDED 2026-09-07, see below].
//   REMOVED 2026-09-07 (were included in the first pass, then correctly identified
//   as activity-clock-sourced, not calendar-clock): fast_hurst_exponent(8),
//   skewness_idx(10), fast_taleb_kurtosis(13), recurrence_rate(14) -- all four read
//   the SAME ImbalanceBarEngine imbalance-bar-returns buffer (confirmed via
//   TripleScreen2.cpp's own comment for recurrence_rate: moved off TS2 to that
//   buffer 2026-08-28). Removing them also deletes this tool's own
//   ImbalanceBarEngine/RecurrenceRateEngine dependency entirely -- simpler and
//   faster, not just more correctly scoped.
//
//   5 dims ADDED 2026-09-07 (formulas fixed against their real, live production
//   call sites, per operator instruction "fix them, then wire them"):
//   - relative_range (TS2): `cfc::ComputeRelativeRange(high,low,atr,lastValid)`,
//     ATR = SMA(True Range, 14) -- confirmed via TripleScreen2.cpp:245
//     (`sc.ATR(..., 14, MOVAVGTYPE_SIMPLE)` -- SIMPLE, NOT Wilder's, a real detail
//     that would have been wrong to assume).
//   - lempel_ziv: `InformationEngine::GetLempelZivComplexity()` (LZ76, WINDOW_SIZE_LZ=64
//     median-binarized returns), fed tick-level on genuine price CHANGES only
//     (matches `ContextManager.cpp`'s own `UpdateMarketPhysics()` gate).
//   - hurst_exponent (TS1): `DfaHurstExponent(returns,100,8)` -- production's real
//     window is ADAPTIVE (`macro_window_n`, market-speed/coherence-driven with a
//     5-bar hysteresis confirm, `CalculateAdaptiveObservationWindow`), simplified
//     here to a FIXED 100-bar window (same as `fast_hurst_exponent`'s own
//     convention) -- a documented, deliberate approximation, not a silent one:
//     porting the full hysteresis state machine adds real complexity for a
//     redundancy audit that only needs the correlation STRUCTURE, not exact
//     value parity.
//   - fisher_info (TS1): `cfc::ComputeFisherInformation(min,max,current,lastValid)`
//     over a fixed 100-bar close window (same adaptive-window simplification as
//     hurst_exponent, for the same reason -- production's real window is also
//     adaptive, `fisher_window_n`).
//   - fractal_dim (TS2): `SevcikFractalDimension.h`, 400-bar window (confirmed via
//     TripleScreen2.cpp's own `kFractalDimHmmWindow=400` comment, Politis-White-
//     derived) over TS2 closes; uses the last 401 CLOSED-bar closes as its own
//     401-point window (production's exact asymmetric live-vs-closed indexing,
//     SevcikFractalDimension.h's own documented contract, is approximated here by
//     treating the just-closed bar as the "live" slot -- a boundary-only
//     simplification, consistent with this tool's bar-close-cadence convention
//     for every other dim).
//   All 5 computed at bar-close cadence (TS1/TS2, matching this tool's own existing
//   convention for log_scale_ratio/log_scale_expansion_ratio) rather than production's
//   genuine per-tick intra-bar reactivity for hurst_exponent/fisher_info specifically
//   -- a deliberate, documented simplification, not an oversight.
//
//   EXCLUDED, structurally (not a scope gap, a real reason): micro_asymmetry(6)
//   [StudyHelperFunctions.cpp's own comment: must update every tick, not
//   bar-gated like the rest of this vector -- a genuinely different
//   accumulation cadence this tool's bar-close snapshot model doesn't fit];
//   fast_mean_rev_z(17) [operator-decided DROP, not wired into production
//   (docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md
//   row 19) -- correlating a dead field would be meaningless]; fast_hurst_exponent(8),
//   skewness_idx(10), fast_taleb_kurtosis(13), recurrence_rate(14) [these 4
//   activity-clock dims belong in a SEPARATE redundancy audit against
//   `ImbalanceObservationData`'s own vector, not here].
//
// Snapshot cadence: once per TS3 (15-min) bar close, the finest bar-level
// cadence among the included dims -- matches FeatureScaler's own conceptual
// model (the vector always has SOME value for every dim; slower-refreshing
// dims are carried forward from their last computed value, exactly
// mirroring production's own carry-forward semantics, per RobustMoments.h/
// DfaHurstExponent.h's documented NaN-on-degenerate-window contract).
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   -Iinclude tools/observation_vector/whole_vector_redundancy_eval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/whole_vector_redundancy_eval
// Usage: ./tools/bin/whole_vector_redundancy_eval \
//   --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet [--max-rss-mb 3072]

#include "BipowerVariation.h"
#include "CarryForwardCalculators.h"
#include "DfaHurstExponent.h"
#include "EventVelocityEngine.h"
#include "InformationEngine.h"
#include "RingBuffer.h"
#include "SevcikFractalDimension.h"
#include "market_data_io.h"
#include "streaming_correlation_matrix.h"
#include "../ToolProgressLogger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr std::size_t kTotalTicksEstimate = 471'930'891;

// ---------------------------------------------------------------------
// Small pure helpers duplicated (not #included) from
// observation_vector_recalibration.cpp, deliberately: refactoring that
// already-validated, previously-run tool to share a header carries real
// regression risk for no benefit to it (its own scope, GPD/TailStats
// reporting, is unrelated to this tool's correlation-matrix goal) -- these
// are faithful copies of already-validated logic, not re-derivations. Each
// is a thin port of the exact ACSIL formula it's named after (StudyHelper
// Functions.cpp), confirmed against the live call site before writing.
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
    if (scale_log_p < 1e-6) return 0.0f;
    const double current_log_p = log_prices[static_cast<std::size_t>(n - 1)];
    const double abs_z_price = std::abs((current_log_p - median_log_p) / scale_log_p);
    const int m = n - 1;
    if (m < 3) return std::clamp(static_cast<float>(abs_z_price), 0.0f, 5.0f);
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

float LogScaleRatioFromReturns(const double* returns, int available, int long_n) {
    const int short_n = std::max(8, long_n / 4);
    if (available < long_n) return 0.0f;
    const double longBV = ComputeBipowerVariation(returns + (available - long_n), long_n);
    const double shortBV = ComputeBipowerVariation(returns + (available - short_n), short_n);
    constexpr double kEps = 1e-12;
    const double ratio = std::log((shortBV + kEps) / (longBV + kEps));
    return static_cast<float>(std::clamp(ratio, -6.0, 6.0));
}

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

}  // namespace

// Dim order matches this tool's own StreamingCorrelationMatrix indices --
// NOT ObservationData's own field order (this is a subset, per the scope
// note above), documented here once so Update() call sites and Print()
// labels can never drift apart.
enum RedundancyDim : std::size_t {
    kLogScaleRatio = 0,
    kBurstinessIndex,
    kLogScaleExpansionRatio,
    kAmihudIlliquidity,
    kLiqFragility,
    kMeanRevZ,
    kRelativeRange,
    kLempelZiv,
    kHurstExponent,
    kFisherInfo,
    kFractalDim,
    kRedundancyDimCount
};

constexpr std::array<const char*, kRedundancyDimCount> kDimNames = {
    "log_scale_ratio", "burstiness_idx", "log_scale_exp_r",
    "amihud_illiq", "liq_fragility", "mean_rev_z",
    "relative_range", "lempel_ziv", "hurst_exponent", "fisher_info", "fractal_dim"
};

int main(int argc, char** argv) {
    std::string ticksPath;
    std::size_t maxRssMB = 3072;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks-parquet") == 0 && i + 1 < argc) {
            ticksPath = argv[++i];
        } else if (std::strcmp(argv[i], "--max-rss-mb") == 0 && i + 1 < argc) {
            maxRssMB = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        }
    }
    if (ticksPath.empty()) {
        std::fprintf(stderr, "usage: %s --ticks-parquet PATH [--max-rss-mb 3072]\n", argv[0]);
        return 1;
    }

    ToolProgressLogger progress("whole_vector_redundancy_eval");
    progress.Log("streaming from " + ticksPath + " (max-rss-mb=" + std::to_string(maxRssMB) + ")");
    constexpr std::size_t kProgressEveryNTicks = 5'000'000;

    // --- TS1 (240min) / TS2 (60min) close-bar aggregation + return history ---
    constexpr long long kTs1BarUs = 240LL * 60 * 1'000'000;
    constexpr long long kTs2BarUs = 60LL * 60 * 1'000'000;
    constexpr int kTs1LongN = 20;
    constexpr int kTs2LongN = 20;
    // Bumped 64->100 (2026-09-07) so the SAME TS1 return history also covers
    // hurst_exponent's 100-bar DfaHurstExponent window -- log_scale_ratio only
    // ever reads the last kTs1LongN=20, so the larger cap is pure headroom, not
    // a behavior change for it.
    constexpr std::size_t kReturnHistCap = 100;
    long long ts1Bucket = -1, ts2Bucket = -1;
    float ts1CurClose = 0.0f, ts2CurClose = 0.0f;
    RingBuffer<double, kReturnHistCap> ts1Returns, ts2Returns;
    float ts1PrevClose = 0.0f, ts2PrevClose = 0.0f;
    bool ts1HavePrev = false, ts2HavePrev = false;

    // --- fisher_info (TS1, 100-bar fixed-window simplification): raw TS1 closes,
    // not returns -- Fisher transform needs min/max PRICE, not log-return history. ---
    constexpr std::size_t kFisherWindow = 100;
    RingBuffer<float, kFisherWindow> ts1ClosesForFisher;

    // --- relative_range (TS2): High/Low tracking + a 14-bar SMA(True Range) ATR,
    // confirmed via TripleScreen2.cpp:245 (MOVAVGTYPE_SIMPLE, not Wilder's). ---
    constexpr std::size_t kAtrPeriod = 14;
    float ts2CurHigh = -1e30f, ts2CurLow = 1e30f;
    float ts2PrevCloseForTR = 0.0f;
    bool ts2HavePrevForTR = false;
    RingBuffer<float, kAtrPeriod> ts2TrueRanges;

    // --- fractal_dim (TS2, 400-bar window): raw TS2 closes, +1 slot for the
    // "live bar" per SevcikFractalDimension's own asymmetric-window contract
    // (approximated here as the just-closed bar -- see scope note above). ---
    constexpr std::size_t kFractalDimWindow = 400;
    RingBuffer<float, kFractalDimWindow + 1> ts2ClosesForFractal;

    // --- lempel_ziv: tick-level InformationEngine, fed only on genuine price
    // CHANGES (matches ContextManager.cpp's own UpdateMarketPhysics() gate). ---
    MindfulTrader::InformationEngine lzEngine;
    double lzLastPrice = 0.0;
    bool lzHaveLastPrice = false;

    // --- TS3 (15min) bar aggregation: mean_rev_z, amihud/liq_fragility bar history ---
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

    // --- Carry-forward snapshot (matches production's own carry-forward contract:
    // NaN on a degenerate window -> reuse last valid value) + the correlation matrix
    // itself (the ONLY thing this tool ever accumulates per-observation into; O(D^2)
    // fixed state, never grows with tick count -- see streaming_correlation_matrix.h). ---
    std::array<float, kRedundancyDimCount> lastValid{
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.5f, 0.0f, 1.5f
    };
    StreamingCorrelationMatrix<kRedundancyDimCount> corrMatrix;

    auto applyCarryForward = [&](RedundancyDim dim, float raw) {
        if (std::isfinite(raw)) lastValid[dim] = raw;
        return lastValid[dim];
    };

    std::size_t ticksProcessed = 0;
    std::size_t snapshotsRecorded = 0;
    constexpr std::size_t kInterimReportEveryNTicks = 50'000'000;

    try {
        StreamTicksFullParquet(ticksPath, [&](std::int64_t ts, double price, std::int64_t volume,
                                              std::int64_t askVol, std::int64_t bidVol, bool isNewContract) {
            ++ticksProcessed;
            if (isNewContract) {
                ts1Bucket = ts2Bucket = ts3Bucket = -1;
                ts1Returns.clear(); ts2Returns.clear(); ts3Closes.clear();
                ts1HavePrev = ts2HavePrev = false;
                tickTimestamps.clear();
                amihudBarHistory.clear();
                liqFragMedRange = liqFragMedSqrtVol = 0.0f;
                amihudHistSumLogRatio = 0.0;
                amihudHistCount = 0;
                liqFragPrev = 0.0f;
                ts3BarsClosed = 0;
                ts1ClosesForFisher.clear();
                ts2TrueRanges.clear();
                ts2HavePrevForTR = false;
                ts2ClosesForFractal.clear();
                lzEngine.Reset();
                lzHaveLastPrice = false;
            }
            if (ticksProcessed % kProgressEveryNTicks == 0) {
                progress.LogProgress(ticksProcessed, kTotalTicksEstimate);
                progress.CheckMemoryBudget(maxRssMB);
            }
            if (ticksProcessed % kInterimReportEveryNTicks == 0) {
                progress.Log("interim snapshot count: " + std::to_string(snapshotsRecorded));
            }
            if (price <= 0.0) return;
            const float p = static_cast<float>(price);

            // --- burstiness_index (dim1): tick-level, always fresh ---
            tickTimestamps.push_back(static_cast<uint64_t>(ts));
            if (tickTimestamps.size() == 100) tickTimestamps.pop_front();
            lastValid[kBurstinessIndex] = eve::CalculateBurstinessIndex(tickTimestamps);

            // --- lempel_ziv: tick-level, fed only on a genuine price CHANGE
            // (matches ContextManager.cpp's own UpdateMarketPhysics() gate). ---
            if (!lzHaveLastPrice || price != lzLastPrice) {
                if (lzHaveLastPrice && lzLastPrice > 0.0) {
                    lzEngine.AddObservation(std::log(price / lzLastPrice));
                    lastValid[kLempelZiv] = static_cast<float>(lzEngine.GetLempelZivComplexity());
                }
                lzLastPrice = price;
                lzHaveLastPrice = true;
            }

            // --- TS1 240-min bar: log_scale_ratio (dim0), hurst_exponent (100-bar
            // fixed-window simplification), fisher_info (100-bar close window) ---
            {
                const long long bucket = ts / kTs1BarUs;
                if (ts1Bucket == -1) { ts1Bucket = bucket; ts1CurClose = p; }
                if (bucket != ts1Bucket) {
                    if (ts1HavePrev && ts1PrevClose > 0.0f) {
                        if (ts1Returns.size() == kReturnHistCap) ts1Returns.pop_front();
                        ts1Returns.push_back(std::log(static_cast<double>(ts1CurClose) / ts1PrevClose));
                    }
                    if (ts1ClosesForFisher.size() == kFisherWindow) ts1ClosesForFisher.pop_front();
                    ts1ClosesForFisher.push_back(ts1CurClose);
                    ts1PrevClose = ts1CurClose;
                    ts1HavePrev = true;
                    ts1Bucket = bucket;
                    if (ts1Returns.size() >= static_cast<std::size_t>(kTs1LongN)) {
                        std::array<double, kReturnHistCap> buf{};
                        for (std::size_t i = 0; i < ts1Returns.size(); ++i) buf[i] = ts1Returns[i];
                        lastValid[kLogScaleRatio] = applyCarryForward(kLogScaleRatio,
                            LogScaleRatioFromReturns(buf.data(), static_cast<int>(ts1Returns.size()), kTs1LongN));
                        if (ts1Returns.size() >= kFisherWindow) {
                            std::array<float, kReturnHistCap> hurstBuf{};
                            for (std::size_t i = 0; i < ts1Returns.size(); ++i) hurstBuf[i] = static_cast<float>(buf[i]);
                            lastValid[kHurstExponent] = applyCarryForward(kHurstExponent,
                                DfaHurstExponent(hurstBuf.data() + (ts1Returns.size() - kFisherWindow),
                                                  static_cast<int>(kFisherWindow), 8));
                        }
                    }
                    if (ts1ClosesForFisher.size() >= kFisherWindow) {
                        float minP = ts1ClosesForFisher[0], maxP = ts1ClosesForFisher[0];
                        for (std::size_t i = 1; i < ts1ClosesForFisher.size(); ++i) {
                            minP = std::min(minP, ts1ClosesForFisher[i]);
                            maxP = std::max(maxP, ts1ClosesForFisher[i]);
                        }
                        lastValid[kFisherInfo] = cfc::ComputeFisherInformation(minP, maxP, ts1CurClose, lastValid[kFisherInfo]);
                    }
                }
                ts1CurClose = p;
            }

            // --- TS2 60-min bar: log_scale_expansion_ratio (dim3), relative_range
            // (dim2, ATR=SMA(TrueRange,14)), fractal_dim (dim15, 400-bar window) ---
            {
                const long long bucket = ts / kTs2BarUs;
                if (ts2Bucket == -1) {
                    ts2Bucket = bucket;
                    ts2CurClose = p;
                    ts2CurHigh = ts2CurLow = p;
                }
                if (bucket != ts2Bucket) {
                    if (ts2HavePrev && ts2PrevClose > 0.0f) {
                        if (ts2Returns.size() == kReturnHistCap) ts2Returns.pop_front();
                        ts2Returns.push_back(std::log(static_cast<double>(ts2CurClose) / ts2PrevClose));
                    }

                    // relative_range's ATR: SMA(True Range, 14) -- True Range needs the
                    // PRIOR bar's close, tracked separately from ts2PrevClose (that one only
                    // updates when ts2HavePrev, this needs the raw previous close regardless).
                    if (ts2HavePrevForTR) {
                        const float tr = std::max({ts2CurHigh - ts2CurLow,
                                                    std::fabs(ts2CurHigh - ts2PrevCloseForTR),
                                                    std::fabs(ts2CurLow - ts2PrevCloseForTR)});
                        if (ts2TrueRanges.size() == kAtrPeriod) ts2TrueRanges.pop_front();
                        ts2TrueRanges.push_back(tr);
                    }
                    ts2PrevCloseForTR = ts2CurClose;
                    ts2HavePrevForTR = true;
                    if (ts2TrueRanges.size() >= kAtrPeriod) {
                        float sumTr = 0.0f;
                        for (std::size_t i = 0; i < ts2TrueRanges.size(); ++i) sumTr += ts2TrueRanges[i];
                        const float atr = sumTr / static_cast<float>(ts2TrueRanges.size());
                        lastValid[kRelativeRange] = cfc::ComputeRelativeRange(ts2CurHigh, ts2CurLow, atr, lastValid[kRelativeRange]);
                    }

                    if (ts2ClosesForFractal.size() == kFractalDimWindow + 1) ts2ClosesForFractal.pop_front();
                    ts2ClosesForFractal.push_back(ts2CurClose);
                    if (ts2ClosesForFractal.size() >= kFractalDimWindow + 1) {
                        std::array<float, kFractalDimWindow + 1> fdBuf{};
                        for (std::size_t i = 0; i < ts2ClosesForFractal.size(); ++i) fdBuf[i] = ts2ClosesForFractal[i];
                        const float fd = SevcikFractalDimension(fdBuf.data(), static_cast<int>(kFractalDimWindow));
                        if (std::isfinite(fd)) lastValid[kFractalDim] = fd;
                    }

                    ts2PrevClose = ts2CurClose;
                    ts2HavePrev = true;
                    ts2Bucket = bucket;
                    ts2CurHigh = ts2CurLow = p;
                    if (ts2Returns.size() >= static_cast<std::size_t>(kTs2LongN)) {
                        std::array<double, kReturnHistCap> buf{};
                        for (std::size_t i = 0; i < ts2Returns.size(); ++i) buf[i] = ts2Returns[i];
                        lastValid[kLogScaleExpansionRatio] = applyCarryForward(kLogScaleExpansionRatio,
                            LogScaleRatioFromReturns(buf.data(), static_cast<int>(ts2Returns.size()), kTs2LongN));
                    }
                }
                ts2CurClose = p;
                ts2CurHigh = std::max(ts2CurHigh, p);
                ts2CurLow = std::min(ts2CurLow, p);
            }

            // --- TS3 15-min bar: mean_rev_z, amihud/liq_fragility bar history ---
            bool ts3BarJustClosed = false;
            {
                const long long bucket = ts / kTs3BarUs;
                if (ts3Bucket == -1) {
                    ts3Bucket = bucket;
                    ts3CurClose = p;
                    ts3CurHigh = ts3CurLow = p;
                    ts3CurVolume = 0.0f;
                } else if (bucket != ts3Bucket) {
                    ts3BarJustClosed = true;
                    if (ts3Closes.size() == kCloseBarCap) ts3Closes.pop_front();
                    ts3Closes.push_back(ts3CurClose);
                    if (ts3Closes.size() >= static_cast<std::size_t>(kMeanRevLookback)) {
                        std::array<float, kCloseBarCap> buf{};
                        for (std::size_t i = 0; i < ts3Closes.size(); ++i) buf[i] = ts3Closes[i];
                        const std::size_t nCloses = ts3Closes.size();
                        lastValid[kMeanRevZ] = ComputeMeanRevZ(buf.data() + (nCloses - kMeanRevLookback), kMeanRevLookback);
                    }
                    if (amihudBarHistory.size() == kBarHistoryCapacity) amihudBarHistory.pop_front();
                    amihudBarHistory.push_back({ts3CurClose, ts3CurVolume, ts3CurHigh - ts3CurLow});
                    ++ts3BarsClosed;

                    // Recompute Amihud historical sum + liq_fragility scale reference,
                    // same convention as observation_vector_recalibration.cpp.
                    amihudHistSumLogRatio = 0.0;
                    amihudHistCount = 0;
                    const std::size_t n = amihudBarHistory.size();
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
                    const std::size_t w = std::min<std::size_t>(kLiqFragWindow, n);
                    if (w == 0) {
                        liqFragMedRange = liqFragMedSqrtVol = 0.0f;
                    } else {
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
                    }

                    ts3Bucket = bucket;
                    ts3CurHigh = ts3CurLow = p;
                    ts3CurVolume = 0.0f;
                }
                ts3CurClose = p;
                ts3CurHigh = std::max(ts3CurHigh, p);
                ts3CurLow = std::min(ts3CurLow, p);
                ts3CurVolume += static_cast<float>(volume);

                if (ts3BarsClosed >= 100) {
                    const float prevBarClose = amihudBarHistory.empty() ? ts3CurClose : amihudBarHistory.back().close;
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
                    if (count >= 2) lastValid[kAmihudIlliquidity] = static_cast<float>(std::exp(sumLogRatio / count));
                    liqFragPrev = LiqFragilityLive(ts3CurHigh, ts3CurLow, ts3CurVolume, liqFragMedRange,
                                                    liqFragMedSqrtVol, liqFragPrev);
                    lastValid[kLiqFragility] = liqFragPrev;
                }
            }

            // --- Snapshot into the O(D^2) correlation matrix, once per TS3 bar close ---
            if (ts3BarJustClosed) {
                std::array<double, kRedundancyDimCount> snapshot;
                for (std::size_t i = 0; i < kRedundancyDimCount; ++i) {
                    snapshot[i] = static_cast<double>(lastValid[i]);
                }
                corrMatrix.Update(snapshot);
                ++snapshotsRecorded;
            }
        });
    } catch (const std::exception& e) {
        progress.Log(std::string("aborted: ") + e.what());
        return 1;
    }

    progress.Log("streaming done (" + std::to_string(ticksProcessed) + " ticks, " +
                 std::to_string(snapshotsRecorded) + " TS3-bar-close snapshots recorded)");
    corrMatrix.Print(kDimNames, progress);

    return 0;
}
