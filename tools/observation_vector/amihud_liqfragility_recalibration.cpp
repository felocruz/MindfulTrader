// amihud_liqfragility_recalibration.cpp -- tick-level replica for the
// FeatureScaler.h dim11 (amihud_illiquidity) recalibration required after the
// 2026-09-03 sqrt-law + geometric-mean reformulation (CalculateAmihudIlliquidity,
// StudyHelperFunctions.cpp / CarryForwardCalculators.h). The formula's entire
// numeric scale and behavior changed (sqrt(dollar-volume) instead of linear,
// geometric mean instead of arithmetic) -- the OLD calibration
// (AMIHUD_ABSOLUTE_FLOOR etc.) was tuned against the pre-2026-09-03
// distribution and must be re-verified, not assumed to still apply.
//
// REBUILT 2026-09-03 to stream genuine per-tick data (StreamTicksWithVolumeParquet,
// lbrnet/data/raw/mes_ticks.parquet, 471.9M real ticks) instead of the prior
// 1-second-bar-aggregated ticks_1s.bin -- the same "coarse proxy hid the real
// defect" lesson learned from burstiness_index applies here: this dim's live
// intra-bar term is called every real tick in production (TripleScreen3.cpp),
// not once per second, so a 1-second-bar replica under-samples true
// live-reactivity the same way the old burstiness tool did.
//
// Builds 15-min bars (ATR(10,Wilder)/VolumeSMA(21) reference series, exactly
// as TripleScreen3.cpp computes them) and the live intra-bar Amihud/
// liq_fragility reactivity in a SINGLE streaming pass, bounded memory
// (RingBuffer-backed bar history, no full-file materialization) -- same DOD
// architecture as tools/observation_vector/burstiness_recalibration.cpp.
// Every raw per-tick value is fed through the REAL FeatureScaler
// (include/FeatureScaler.h, unmodified) via UpdateAndNormalize(), so the
// z-score/exceedance-rate statistics reported are against the actual
// production scaling code, not a re-derivation of it.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   -Iinclude tools/observation_vector/amihud_liqfragility_recalibration.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/amihud_liqfragility_recalibration
// Usage: ./tools/bin/amihud_liqfragility_recalibration \
//   --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet

#include "FeatureScaler.h"
#include "RingBuffer.h"
#include "market_data_io.h"
#include "../ToolProgressLogger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr int kLookbackN = 20;                       // representative mid-point of the adaptive [10,40] range
constexpr long long kBarUs = 15LL * 60 * 1'000'000;  // 15-min bar bucket width in microseconds
constexpr size_t kLiqFragWindow = 30;                 // matches CalculateLiquidityFragility's kWindow
constexpr float kLiveBarMinVolume = 50.0f;           // matches production's kLiveBarMinVolume
constexpr size_t kBarHistoryCapacity = 32;            // >= max(kLookbackN, kLiqFragWindow) + headroom
constexpr double kRatioEps = 1e-20;                   // matches production's floor before ln()

struct Bar {
    float close = 0.0f;
    float volume = 0.0f;
    float range = 0.0f;
};

// Faithful port of the NEW (2026-09-03) CalculateLiquidityFragility --
// dedicated microstructure elasticity ratio (Gemini literature grounding,
// lbrnet/logs/rc_gemini.log CLAUDE_BRIEF_124), replacing the prior
// ATR/volume-SMA composite. medRange/medSqrtVol are this dim's own dedicated
// median-based scale reference over the last kLiqFragWindow CLOSED bars --
// see CalculateLiquidityFragility's own doc comment (StudyHelperFunctions.cpp)
// for the full derivation.
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

struct ZStats {
    std::size_t n = 0;
    double sumAbsZ = 0.0;
    float maxAbsZ = 0.0f;
    float madAtMaxZ = 0.0f;
    std::size_t hits6 = 0, hits25 = 0, hits45 = 0;
    std::vector<float> sample;
    std::vector<float> madSample;

    void Record(float z, float localMad) {
        ++n;
        const float az = std::fabs(z);
        sumAbsZ += az;
        if (az > maxAbsZ) {
            maxAbsZ = az;
            madAtMaxZ = localMad;
        }
        if (az >= 6.0f) ++hits6;
        if (az >= 25.0f) ++hits25;
        if (az >= 45.0f) ++hits45;
        if (n % 50 == 0) {
            sample.push_back(az);
            madSample.push_back(localMad);
        }
    }

    double RateAt(float threshold) const {
        if (sample.empty()) return 0.0;
        std::size_t hits = 0;
        for (float az : sample) {
            if (az >= threshold) ++hits;
        }
        return 100.0 * hits / sample.size();
    }

    void Report(const char* name, float productionBound) const {
        std::vector<float> sorted = sample;
        std::sort(sorted.begin(), sorted.end());
        auto pct = [&](double p) -> float {
            if (sorted.empty()) return 0.0f;
            std::size_t idx = static_cast<std::size_t>(p * (sorted.size() - 1));
            return sorted[idx];
        };
        std::printf("%s: n=%zu  mean|z|=%.4f  max|z|=%.2f  p50=%.3f p90=%.3f p99=%.3f p99.9=%.3f\n",
                    name, n, sumAbsZ / std::max<std::size_t>(n, 1), maxAbsZ,
                    pct(0.50), pct(0.90), pct(0.99), pct(0.999));
        std::printf("%s: |z|>=6 rate=%.4f%%  |z|>=25 rate=%.4f%%  |z|>=45 rate=%.4f%%\n",
                    name, 100.0 * hits6 / std::max<std::size_t>(n, 1),
                    100.0 * hits25 / std::max<std::size_t>(n, 1),
                    100.0 * hits45 / std::max<std::size_t>(n, 1));
        std::printf("%s: CURRENT PRODUCTION BOUND=%.1f  rate-at-bound=%.4f%%\n",
                    name, productionBound, RateAt(productionBound));

        if (!madSample.empty()) {
            std::vector<float> madSorted = madSample;
            std::sort(madSorted.begin(), madSorted.end());
            auto madPct = [&](double p) -> float {
                std::size_t idx = static_cast<std::size_t>(p * (madSorted.size() - 1));
                return madSorted[idx];
            };
            double sumMad = 0, sumZ = 0;
            for (std::size_t k = 0; k < sample.size(); ++k) { sumMad += madSample[k]; sumZ += sample[k]; }
            const double meanMad = sumMad / sample.size(), meanZ = sumZ / sample.size();
            double cov = 0, varMad = 0, varZ = 0;
            for (std::size_t k = 0; k < sample.size(); ++k) {
                const double dm = madSample[k] - meanMad, dz = sample[k] - meanZ;
                cov += dm * dz; varMad += dm * dm; varZ += dz * dz;
            }
            const double corr = (varMad > 0 && varZ > 0) ? cov / std::sqrt(varMad * varZ) : 0.0;
            std::vector<std::size_t> idxSorted(sample.size());
            for (std::size_t k = 0; k < idxSorted.size(); ++k) idxSorted[k] = k;
            std::sort(idxSorted.begin(), idxSorted.end(), [&](std::size_t a, std::size_t b) { return sample[a] > sample[b]; });
            const std::size_t topN = std::max<std::size_t>(1, sample.size() / 100);
            double sumMadTop = 0;
            for (std::size_t k = 0; k < topN; ++k) sumMadTop += madSample[idxSorted[k]];
            const double meanMadTop = sumMadTop / topN;
            std::printf("%s: [shrinkage audit] corr(localMAD,|z|)=%.4f (negative = collapse signature)  "
                        "median_MAD=%.6g  mean_MAD_top1pct|z|=%.6g (ratio=%.4f)  MAD_at_max|z|=%.6g\n",
                        name, corr, madPct(0.50), meanMadTop, meanMadTop / std::max(madPct(0.50), 1e-30f), madAtMaxZ);
            std::printf("%s: [MAD percentiles, candidate SHRINKAGE_SCALE_MIN floor points] "
                        "p0.1=%.6g p1=%.6g p5=%.6g p10=%.6g p50=%.6g\n",
                        name, madPct(0.001), madPct(0.01), madPct(0.05), madPct(0.10), madPct(0.50));
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string ticksPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks-parquet") == 0 && i + 1 < argc) {
            ticksPath = argv[++i];
        }
    }
    if (ticksPath.empty()) {
        std::fprintf(stderr, "usage: %s --ticks-parquet PATH\n", argv[0]);
        return 1;
    }

    FeatureScaler fs;
    ZStats amihudStats, liqFragStats;
    ToolProgressLogger progress("amihud_liqfragility_recalibration");
    progress.Log("streaming from " + ticksPath);
    constexpr std::size_t kProgressEveryNTicks = 5'000'000;

    RingBuffer<Bar, kBarHistoryCapacity> barHistory;
    float liqFragMedRange = 0.0f;
    float liqFragMedSqrtVol = 0.0f;

    long long curBucket = -1;
    float curHigh = -1e30f, curLow = 1e30f, curClose = 0.0f, curVolume = 0.0f;
    double historicalSumLogRatio = 0.0;
    int historicalCount = 0;
    float liqFragPrev = 0.0f;
    std::size_t barsClosed = 0;
    std::size_t ticksProcessed = 0;

    auto recomputeHistorical = [&]() {
        historicalSumLogRatio = 0.0;
        historicalCount = 0;
        const std::size_t n = barHistory.size();
        if (n < 1) return;
        const std::size_t maxPairs = std::min<std::size_t>(kLookbackN, n - 1);
        for (std::size_t k = 0; k < maxPairs; ++k) {
            const std::size_t curIdx = n - 1 - k;
            const std::size_t prevIdx = curIdx - 1;
            const Bar& cur = barHistory[curIdx];
            const Bar& prev = barHistory[prevIdx];
            if (cur.volume < 1.0f || cur.close <= 0.0f || prev.close <= 0.0f) continue;
            const double dollarVol = static_cast<double>(cur.close) * static_cast<double>(cur.volume);
            if (dollarVol < 1.0) continue;
            const double logRet = std::fabs(std::log(static_cast<double>(cur.close) / prev.close));
            const double ratio = logRet / std::sqrt(dollarVol);
            historicalSumLogRatio += std::log(ratio + kRatioEps);
            ++historicalCount;
        }
    };

    auto onNewContractOrStart = [&]() {
        barHistory.clear();
        liqFragMedRange = 0.0f;
        liqFragMedSqrtVol = 0.0f;
        curBucket = -1;
        historicalSumLogRatio = 0.0;
        historicalCount = 0;
        liqFragPrev = 0.0f;
    };

    auto recomputeLiqFragScaleRef = [&]() {
        const std::size_t n = barHistory.size();
        const std::size_t w = std::min<std::size_t>(kLiqFragWindow, n);
        if (w == 0) {
            liqFragMedRange = 0.0f;
            liqFragMedSqrtVol = 0.0f;
            return;
        }
        std::array<float, kLiqFragWindow> rangeBuf{};
        std::array<float, kLiqFragWindow> sqrtVolBuf{};
        for (std::size_t k = 0; k < w; ++k) {
            const Bar& b = barHistory[n - w + k];
            rangeBuf[k] = b.range;
            sqrtVolBuf[k] = std::sqrt(std::max(b.volume, 0.0f));
        }
        const std::size_t mid = w / 2;
        std::array<float, kLiqFragWindow> rangeScratch = rangeBuf;
        std::nth_element(rangeScratch.begin(), rangeScratch.begin() + mid, rangeScratch.begin() + w);
        liqFragMedRange = rangeScratch[mid];
        std::array<float, kLiqFragWindow> volScratch = sqrtVolBuf;
        std::nth_element(volScratch.begin(), volScratch.begin() + mid, volScratch.begin() + w);
        liqFragMedSqrtVol = volScratch[mid];
    };

    auto finalizeBar = [&]() {
        if (barHistory.size() == kBarHistoryCapacity) barHistory.pop_front();
        barHistory.push_back({curClose, curVolume, curHigh - curLow});
        ++barsClosed;

        recomputeHistorical();
        recomputeLiqFragScaleRef();
    };

    StreamTicksWithVolumeParquet(
        ticksPath,
        [&](std::int64_t ts, double price, std::int64_t volume, bool isNewContract) {
            ++ticksProcessed;
            if (ticksProcessed % kProgressEveryNTicks == 0) {
                progress.LogProgress(ticksProcessed, 471'930'891);
            }
            if (isNewContract) onNewContractOrStart();

            const long long bucket = ts / kBarUs;
            if (curBucket == -1) {
                curBucket = bucket;
                curHigh = curLow = curClose = static_cast<float>(price);
                curVolume = 0.0f;
            } else if (bucket != curBucket) {
                finalizeBar();
                curBucket = bucket;
                curHigh = curLow = curClose = static_cast<float>(price);
                curVolume = 0.0f;
            }

            curHigh = std::max(curHigh, static_cast<float>(price));
            curLow = std::min(curLow, static_cast<float>(price));
            curClose = static_cast<float>(price);
            curVolume += static_cast<float>(volume);

            // Warmup: need enough closed-bar history for a meaningful Amihud/liq_fragility reading.
            if (barsClosed < 100) return;

            const float prevBarClose = barHistory.empty() ? curClose : barHistory.back().close;
            float amihud = 0.0f;
            {
                double sumLogRatio = historicalSumLogRatio;
                int count = historicalCount;
                if (curVolume >= kLiveBarMinVolume && curClose > 0.0f && prevBarClose > 0.0f) {
                    const double dollarVol = static_cast<double>(curClose) * static_cast<double>(curVolume);
                    if (dollarVol >= 1.0) {
                        const double logRet = std::fabs(std::log(static_cast<double>(curClose) / prevBarClose));
                        const double ratio = logRet / std::sqrt(dollarVol);
                        sumLogRatio += std::log(ratio + kRatioEps);
                        ++count;
                    }
                }
                if (count >= 2) amihud = static_cast<float>(std::exp(sumLogRatio / count));
            }

            const float liqFrag = LiqFragilityLive(curHigh, curLow, curVolume, liqFragMedRange, liqFragMedSqrtVol, liqFragPrev);

            std::array<float, FeatureScaler::N_DIMS> obs{};
            obs[11] = amihud;
            obs[12] = liqFrag;
            fs.UpdateAndNormalize(obs);
            amihudStats.Record(fs.lastRawZ[11], fs.lastLocalMad[11]);
            liqFragStats.Record(fs.lastRawZ[12], fs.latestLogScale[12]);
        });

    // Finalize the last still-forming bar's fragility EMA anchor for completeness (not required for reporting).
    (void)liqFragPrev;

    progress.Log("streaming done, writing final report");
    std::printf("processed %zu real ticks, %zu closed 15-min bars\n", ticksProcessed, barsClosed);
    std::printf("\n=== amihud_illiquidity (dim 11, SOFTLOGZ) ===\n");
    amihudStats.Report("amihud", FeatureScaler::STATE_WINSOR_SIGMA);
    std::printf("\n=== liq_fragility (dim 12, LOGZ) ===\n");
    liqFragStats.Report("liq_fragility", FeatureScaler::LOGZ_WINSOR_SIGMA_OVERRIDE[12]);

    return 0;
}

