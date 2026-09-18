// diagnose_real_data_trigger.cpp -- ad hoc diagnostic (not part of the CLI/
// build), investigating why the push-on-change fix (dim-selection spec §3a)
// did not lower the real-data significant-change rate (still ~71% on a
// 50M-tick real run, vs. 69.6% before the fix). Streams real ticks through
// MarketDataReplayEngine and aggregates per-dim z-score statistics via
// GetLastTriggerMetrics().perDimZ, to find which dim(s) are still driving
// near-continuous triggering.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 -Iinclude -Iinclude/generated \
//   -Itools/market_data_replay -I/home/rcruz/anaconda3/envs/mts/include \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   tools/market_data_replay/diagnose_real_data_trigger.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o /tmp/diagnose_real_data_trigger

#include "MarketDataReplayEngine.h"
#include "CandidateObservationDims.h"
#include "../observation_vector/market_data_io.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    std::string ticksPath;
    std::size_t maxTicks = 10'000'000;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks-parquet") == 0 && i + 1 < argc) {
            ticksPath = argv[++i];
        } else if (std::strcmp(argv[i], "--max-ticks") == 0 && i + 1 < argc) {
            maxTicks = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        }
    }
    if (ticksPath.empty()) {
        std::fprintf(stderr, "usage: %s --ticks-parquet PATH [--max-ticks N]\n", argv[0]);
        return 1;
    }

    MarketDataReplayEngine engine;
    std::size_t ticksProcessed = 0;
    std::size_t decisionsSeen = 0;
    std::size_t significantCount = 0;
    std::size_t directSignificantCount = 0;  // Gemini hypothesis: no double-normalization
    bool fatalError = false;

    // Per-dim aggregate stats, only over ticks where a real decision was made
    // (AllDimsWarmedUp()==true).
    std::array<double, mdr::kCandidateDimCount> sumAbsZ{};
    std::array<double, mdr::kCandidateDimCount> maxAbsZ{};
    std::array<std::size_t, mdr::kCandidateDimCount> exceeds2 {};  // |z|>=2.0
    std::array<std::size_t, mdr::kCandidateDimCount> exceeds4 {};  // |z|>=4.0 (own-dim already over threshold)

    // Direct-metric (Gemini hypothesis) per-dim stats: |currentObs[dim]|
    // straight from FeatureScaler, no second rolling-window re-normalization.
    std::array<double, mdr::kCandidateDimCount> sumAbsDirect{};
    std::array<double, mdr::kCandidateDimCount> maxAbsDirect{};

    constexpr float kChiSq10P90 = 15.987f;  // chi-squared(10) 90th percentile

    const char* names[] = {"log_scale_ratio", "burstiness_index", "relative_range", "lempel_ziv",
                            "hurst_exponent", "fisher_info", "amihud_illiquidity", "liq_fragility",
                            "fractal_dim", "mean_rev_z"};

    try {
        StreamTicksFullParquet(
            ticksPath,
            [&](std::int64_t ts, double price, std::int64_t volume, std::int64_t askVol,
                std::int64_t bidVol, bool isNewContract) {
                if (fatalError) return;
                if (ticksProcessed >= maxTicks) return;
                ++ticksProcessed;
                if (price <= 0.0) return;

                const bool significant = engine.OnTick(ts, price, volume, askVol, bidVol);
                const auto& metrics = engine.GetLastTriggerMetrics();
                // GetLastTriggerMetrics() default-constructs (all-zero) when
                // no decision was made this tick (not warmed up yet) -- use
                // mahalanobis_distance>0 as a cheap "a real decision happened"
                // proxy (a genuine post-warmup tick with all-zero z across
                // every dim is a measure-zero coincidence).
                if (metrics.mahalanobis_distance <= 0.0f && !significant) return;

                ++decisionsSeen;
                if (significant) ++significantCount;
                for (std::size_t d = 0; d < mdr::kCandidateDimCount; ++d) {
                    const double az = std::abs(static_cast<double>(metrics.perDimZ[d]));
                    sumAbsZ[d] += az;
                    if (az > maxAbsZ[d]) maxAbsZ[d] = az;
                    if (az >= 2.0) ++exceeds2[d];
                    if (az >= 4.0) ++exceeds4[d];
                }

                // Gemini hypothesis test: compute the chi-squared distance
                // DIRECTLY from FeatureScaler's already-scaled output, with NO
                // second rolling-window re-normalization inside the gate.
                const auto& scaledObs = engine.GetLastScaledCandidateObs();
                float directDistSq = 0.0f;
                for (std::size_t d = 0; d < mdr::kCandidateDimCount; ++d) {
                    const double av = std::abs(static_cast<double>(scaledObs[d]));
                    sumAbsDirect[d] += av;
                    if (av > maxAbsDirect[d]) maxAbsDirect[d] = av;
                    directDistSq += scaledObs[d] * scaledObs[d];
                }
                if (directDistSq >= kChiSq10P90) ++directSignificantCount;

                if (ticksProcessed % 1'000'000 == 0) {
                    std::fprintf(stderr, "processed %zu ticks, decisions=%zu, significant=%zu (%.2f%%), "
                                 "direct_significant=%zu (%.2f%%)\n",
                                 ticksProcessed, decisionsSeen, significantCount,
                                 decisionsSeen > 0 ? 100.0 * significantCount / decisionsSeen : 0.0,
                                 directSignificantCount,
                                 decisionsSeen > 0 ? 100.0 * directSignificantCount / decisionsSeen : 0.0);
                }
            });
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FATAL: %s\n", e.what());
        return 1;
    }

    std::printf("=== FINAL: ticks=%zu decisions=%zu significant=%zu rate=%.4f "
               "direct_significant=%zu direct_rate=%.4f ===\n",
                ticksProcessed, decisionsSeen, significantCount,
                decisionsSeen > 0 ? static_cast<double>(significantCount) / decisionsSeen : 0.0,
                directSignificantCount,
                decisionsSeen > 0 ? static_cast<double>(directSignificantCount) / decisionsSeen : 0.0);
    std::printf("%-20s %10s %10s %12s %12s %10s %10s\n", "dim", "mean|z|", "max|z|", "frac|z|>=2",
               "frac|z|>=4", "mean|dir|", "max|dir|");
    for (std::size_t d = 0; d < mdr::kCandidateDimCount; ++d) {
        std::printf("%-20s %10.4f %10.4f %12.4f %12.4f %10.4f %10.4f\n", names[d],
                    decisionsSeen > 0 ? sumAbsZ[d] / decisionsSeen : 0.0, maxAbsZ[d],
                    decisionsSeen > 0 ? static_cast<double>(exceeds2[d]) / decisionsSeen : 0.0,
                    decisionsSeen > 0 ? static_cast<double>(exceeds4[d]) / decisionsSeen : 0.0,
                    decisionsSeen > 0 ? sumAbsDirect[d] / decisionsSeen : 0.0, maxAbsDirect[d]);
    }
    return 0;
}
