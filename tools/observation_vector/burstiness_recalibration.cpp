// tools/observation_vector/burstiness_recalibration.cpp
// Tick-level replica for FeatureScaler.h dim1 (burstiness_index)'s
// recalibration, required after two stacked, not-yet-re-audited fixes:
//   (a) the wire redirect from the old bar-cadence proxy to raschkeBurst (2026-08-29)
//   (b) raschkeBurst's own reformulation from plain CV to robust CV
//       (MAD/median x 1.4404199, EventVelocityEngine.h, 2026-08-31)
// DIM_WINSOR_SIGMA_OVERRIDE[1] is currently disabled (0.0f -> falls back to
// STATE_WINSOR_SIGMA=6.0) -- the historical 45.0 bound it replaced was fit
// against neither of the above; it described the ORIGINAL bar-cadence,
// plain-CV proxy's distribution.
//
// KNOWN, NAMED LIMITATION -- read before trusting this tool's output as
// final: production's raschkeBurst is computed on REAL PER-TICK arrival
// timestamps (ContextManager::m_eventTimestampsUS, pushed unconditionally on
// every incoming trade inside CheckAndTriggerHMM -- CLAUDE.md's AutoLoop=1
// tick cadence). The only real historical MES data available in this
// workspace (lbrnet/data/raw/mes_continuous_ticks.parquet) is
// 1-SECOND-BAR-AGGREGATED -- verified directly, 2026-08-31 (every
// consecutive timestamp delta in a real 2M-row sample is an exact multiple
// of 1,000,000us; zero sub-second gaps found). This tool therefore
// replicates burstiness of PER-ACTIVE-SECOND bar-formation events, not
// production's real per-tick arrival cadence -- a real, non-fabricated
// measurement, but of a genuinely coarser event stream than what production
// actually feeds CalculateBurstinessIndex(). Treat the resulting bound as a
// real improvement over the current disabled placeholder, not a final
// calibration -- re-derive once genuine sub-second tick data exists.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   -Iinclude -I/mnt/c/Users/rcruz/vcpkg/installed/x64-windows/include \
//   tools/observation_vector/burstiness_recalibration.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/burstiness_recalibration
// Usage: ./tools/bin/burstiness_recalibration \
//   --ticks-parquet lbrnet/data/raw/mes_continuous_ticks.parquet \
//   [--zsample-dump /tmp/burstiness_zsamples.csv]

#include "EventVelocityEngine.h"
#include "FeatureScaler.h"
#include "RingBuffer.h"
#include "market_data_io.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Mirrors ContextManager::EVENT_VELOCITY_MAX (ContextManager.h) -- that
// header cannot be #included standalone (pulls in sierrachart.h), same
// constraint noted by every other *_recalibration.cpp tool in this directory.
constexpr size_t kEventVelocityMax = 100;

// Same reporting shape as tools/observation_vector/amihud_liqfragility_recalibration.cpp's
// ZStats -- kept self-contained here rather than extracted into a shared
// header on this, its second real use: the two tools' Record()/Report() call
// sites differ enough (single formula here vs. multi-candidate guard sweep
// there) that a premature shared abstraction would cost more than it saves.
struct ZStats {
    std::size_t n = 0;
    double sumAbsZ = 0.0;
    float maxAbsZ = 0.0f;
    float madAtMaxZ = 0.0f;  // shrinkage-audit: local MAD scale at the single most extreme |z| event
    std::size_t hits6 = 0, hits25 = 0, hits45 = 0;
    std::vector<float> sample;    // downsampled |z| values, for percentile/GPD reporting
    std::vector<float> madSample; // paired 1:1 with `sample` -- local MAD scale at that same tick

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
        if (n % 10 == 0) {
            sample.push_back(az);
            madSample.push_back(localMad);
        }
    }

    // Rate at an arbitrary threshold -- reuses the downsampled `sample`
    // (already |z| values), same statistical validity as the percentile calc.
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
        std::printf("%s: CURRENT PRODUCTION BOUND=%.1f (STATE_WINSOR_SIGMA default, override disabled)  "
                    "rate-at-bound=%.4f%%\n",
                    name, productionBound, RateAt(productionBound));

        if (madSample.empty()) return;

        // Shrinkage-collapse audit: a genuine collapse signature (the
        // pattern that made dims 3/6/7/9/12 need SHRINKAGE_SCALE_MIN) looks
        // like the most extreme |z| events clustering at anomalously SMALL
        // local MAD relative to its own typical value -- not a large raw
        // deviation on a normal-scale MAD (a real tail event, which needs a
        // wider winsorization bound, not shrinkage).
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
    }
};

void PrintUsage(const char* prog) {
    std::fprintf(stderr,
        "usage: %s --ticks-parquet PATH [--zsample-dump PATH]\n"
        "  --ticks-parquet PATH  real MES 1-second-bar parquet (timestamp_us column required)\n"
        "  --zsample-dump PATH   optional CSV of downsampled |z| values for a GPD/EVT tail fit\n",
        prog);
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticksPath, dumpPath;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--ticks-parquet") ticksPath = next("--ticks-parquet");
        else if (arg == "--zsample-dump") dumpPath = next("--zsample-dump");
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); PrintUsage(argv[0]); return 1; }
    }
    if (ticksPath.empty()) {
        PrintUsage(argv[0]);
        return 1;
    }

    TickSeries series;
    try {
        series = ReadTicksParquet(ticksPath);
    } catch (const std::runtime_error& e) {
        std::fprintf(stderr, "FAILED: %s\n", e.what());
        return 1;
    }
    std::printf("Loaded %zu real MES 1-second-bar timestamps from %s\n",
                series.timestamp_us.size(), ticksPath.c_str());
    std::fflush(stdout);

    RingBuffer<uint64_t, kEventVelocityMax + 1> ring;
    FeatureScaler fs;
    ZStats stats;

    for (const std::int64_t ts : series.timestamp_us) {
        // Mirrors ContextManager.cpp's own push/pop shape exactly (pop_front
        // once at the logical window size, THEN push_back) --
        // ContextManager.cpp:677-680.
        if (ring.size() >= kEventVelocityMax) ring.pop_front();
        ring.push_back(static_cast<uint64_t>(ts));

        const float burstiness = eve::CalculateBurstinessIndex(ring);

        // Every other dim left at its zero default -- each dim's rolling
        // median/MAD window is independent, same approach already used by
        // tools/observation_vector/amihud_liqfragility_recalibration.cpp for dims 12/13.
        std::array<float, FeatureScaler::N_DIMS> obs{};
        obs[1] = burstiness;
        fs.UpdateAndNormalize(obs);
        stats.Record(fs.lastRawZ[1], fs.lastLocalMad[1]);
    }

    std::printf("\n=== burstiness_index (dim 1, SOFTLOGZ) ===\n");
    stats.Report("burstiness_index", FeatureScaler::STATE_WINSOR_SIGMA);
    std::printf("burstiness_index: for reference, the OLD (pre-2026-08-31, now-disabled) bound was 45.0 -- "
                "|z|>=45.0 rate=%.4f%%\n",
                stats.RateAt(45.0f));

    if (!dumpPath.empty()) {
        std::ofstream out(dumpPath);
        out << "dim,abs_z\n";
        for (const float az : stats.sample) out << "burstiness_index," << az << "\n";
        std::printf("\ndumped %zu |z| samples to %s\n", stats.sample.size(), dumpPath.c_str());
    }
    return 0;
}
