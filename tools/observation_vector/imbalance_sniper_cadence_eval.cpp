// tools/observation_vector/imbalance_sniper_cadence_eval.cpp -- real-data validation for the
// Sniper's imbalance-clock exit-monitoring cadence (docs/superpowers/specs/2026-09-19-imbalance-
// clock-hotpath-and-standalone-backtester-spec.md §4).
//
// Drives ImbalanceClockManager::OnTick() + ImbalanceContextManager::Update() unconditionally,
// every tick, over the real MES tick stream -- the exact wiring SniperContext.is3BarClosed will
// eventually need in SCStudies.cpp/BackTesterStudy.cpp/the new standalone backtester (§5's
// convergence principle: pure state-management, no external dependency, so validating it here
// first is representative of all three contexts).
//
// Answers two real, previously-unmeasured questions:
//   1. IS3 bar-close cadence: how often does "is3BarClosed" actually fire, in real tick counts
//      and wall-clock-equivalent terms -- is it "blazingly fast" in practice, or does it degenerate
//      to noise/near-continuous firing at real tick density (the exact failure class already found
//      for burstiness_index and sub-second imbalance bars in prior sessions)?
//   2. Gang-math value sanity at IS3 closes: do hurst_exponent/recurrence_rate/skewness_idx/
//      taleb_kurtosis/shannon_flow_entropy/shannon_efficiency take plausible, varying values when
//      sampled at this cadence, or do they collapse/saturate?
//
// This does NOT touch MarketDataReplayEngine.h (which deliberately stays walled off from
// ImbalanceContextManager per the standing calendar-clock/imbalance-clock boundary rule) -- this
// is a new, separate, standalone tool, same convention as imbalance_clock_manager_ratio_eval.cpp.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   -Iinclude tools/observation_vector/imbalance_sniper_cadence_eval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/imbalance_sniper_cadence_eval
// Usage: ./tools/bin/imbalance_sniper_cadence_eval \
//   --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet \
//   [--max-ticks N] [--max-rss-mb 4096]

#include "ImbalanceClockManager.h"
#include "ImbalanceContextManager.h"
#include "market_data_io.h"
#include "../ToolProgressLogger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kTotalTicksEstimate = 471'930'891;
constexpr float kFixedFallbackThreshold = 700.0f;  // same shipped default as ImbalanceScreen1.cpp

// Simple running stats -- count/min/max/mean, no heap growth, matches this codebase's
// bounded-memory tools/ convention.
struct RunningStats {
    std::size_t n = 0;
    double sum = 0.0;
    float lo = std::numeric_limits<float>::infinity();
    float hi = -std::numeric_limits<float>::infinity();

    void Update(float v) {
        if (!std::isfinite(v)) return;
        ++n;
        sum += v;
        lo = std::min(lo, v);
        hi = std::max(hi, v);
    }
    double Mean() const { return n > 0 ? sum / static_cast<double>(n) : 0.0; }
};

void PrintUsage(const char* argv0) {
    std::fprintf(stderr, "usage: %s --ticks-parquet PATH [--max-ticks N] [--max-rss-mb 4096]\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticksPath;
    std::size_t maxTicks = 0;
    std::size_t maxRssMB = 0;
    bool adaptiveEnabled = true;
    float threshold = kFixedFallbackThreshold;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--ticks-parquet") ticksPath = next("--ticks-parquet");
        else if (arg == "--max-ticks") maxTicks = std::stoull(next("--max-ticks"));
        else if (arg == "--max-rss-mb") maxRssMB = std::stoull(next("--max-rss-mb"));
        else if (arg == "--no-adaptive") adaptiveEnabled = false;
        else if (arg == "--threshold") threshold = std::stof(next("--threshold"));
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); PrintUsage(argv[0]); return 1; }
    }
    if (ticksPath.empty()) { PrintUsage(argv[0]); return 1; }

    ToolProgressLogger progress("imbalance_sniper_cadence_eval");
    progress.SetScope("SniperContext.is3BarClosed cadence + Gang-math sanity at real IS3 bar closes");
    progress.Log("streaming from " + ticksPath + (adaptiveEnabled ? " (adaptive threshold)" :
        (" (FIXED threshold=" + std::to_string(threshold) + ", adaptive disabled)")));
    constexpr std::size_t kProgressEveryNTicks = 5'000'000;

    ImbalanceClockManager::Instance().Reset();
    ImbalanceClockManager::Instance().ConfigureIs3Threshold(threshold);
    if (adaptiveEnabled) ImbalanceClockManager::Instance().EnableIs3AdaptiveThreshold();
    ImbalanceContextManager::Instance().Reset();

    std::size_t is3Before = 0;
    std::size_t is3Closes = 0;
    RunningStats hurst, recurrence, skew, kurt, entropy, efficiency;

    long long tickIndex = 0;
    std::size_t ticksProcessed = 0;
    std::size_t ticksSinceLastClose = 0;
    std::vector<std::size_t> gapsBetweenCloses;  // ticks between consecutive IS3 closes

    try {
        StreamTicksFullParquet(ticksPath, [&](std::int64_t /*ts*/, double price, std::int64_t /*volume*/,
                                               std::int64_t askVolume, std::int64_t bidVolume,
                                               bool /*isNewContract*/) {
            if (maxTicks != 0 && ticksProcessed >= maxTicks) return;
            ++ticksProcessed;
            if (ticksProcessed % kProgressEveryNTicks == 0) {
                progress.LogProgress(ticksProcessed, kTotalTicksEstimate);
                if (maxRssMB > 0) progress.CheckMemoryBudget(maxRssMB);
            }
            if (price <= 0.0) return;

            const int idx = static_cast<int>(tickIndex++);
            const float p = static_cast<float>(price);
            const float ask = static_cast<float>(askVolume);
            const float bid = static_cast<float>(bidVolume);

            // The exact wiring proposed for SCStudies.cpp/BackTesterStudy.cpp -- unconditional,
            // every tick, right next to where ActivityClockManager::Instance().Update(sc) already
            // sits (same field set: barIndex/askVolume/bidVolume/price).
            ImbalanceClockManager::Instance().OnTick(idx, ask, bid, p);
            ImbalanceContextManager::Instance().Update();

            ++ticksSinceLastClose;
            const std::size_t is3After = ImbalanceClockManager::Instance().GetIs3CompletedBarCount();
            if (is3After != is3Before) {
                is3Before = is3After;
                ++is3Closes;
                gapsBetweenCloses.push_back(ticksSinceLastClose);
                ticksSinceLastClose = 0;

                // SniperContext.is3BarClosed == true this instant -- sample the Gang-math values
                // a Sniper reading SniperContext at this exact cadence would see.
                hurst.Update(ImbalanceContextManager::Instance()
                    .GetDim<ImbalanceContextManager::IMBALANCE_OBS_HURST_EXPONENT>());
                recurrence.Update(ImbalanceContextManager::Instance()
                    .GetDim<ImbalanceContextManager::IMBALANCE_OBS_RECURRENCE_RATE>());
                skew.Update(ImbalanceContextManager::Instance()
                    .GetDim<ImbalanceContextManager::IMBALANCE_OBS_SKEWNESS_IDX>());
                kurt.Update(ImbalanceContextManager::Instance()
                    .GetDim<ImbalanceContextManager::IMBALANCE_OBS_TALEB_KURTOSIS>());
                entropy.Update(ImbalanceContextManager::Instance().GetShannonFlowEntropy());
                efficiency.Update(ImbalanceContextManager::Instance().GetShannonEfficiency());
            }
        });
    } catch (const std::exception& e) {
        progress.Log(std::string("aborted: ") + e.what());
        return 1;
    }

    progress.Log("streaming done (" + std::to_string(ticksProcessed) + " ticks)");

    char line[320];
    auto p = [&](const char* fmt, auto... rest) {
        std::snprintf(line, sizeof(line), fmt, rest...);
        std::puts(line);
        progress.Log(line);
    };

    p("\n=== IS3 bar-close cadence ===");
    p("  ticks processed=%zu  IS3 closes=%zu  ticks/close (mean)=%.1f",
      ticksProcessed, is3Closes, is3Closes > 0 ? static_cast<double>(ticksProcessed) / is3Closes : 0.0);

    if (!gapsBetweenCloses.empty()) {
        std::vector<std::size_t> sortedGaps = gapsBetweenCloses;
        std::sort(sortedGaps.begin(), sortedGaps.end());
        const auto pct = [&](double q) -> std::size_t {
            const std::size_t idx2 = static_cast<std::size_t>(q * static_cast<double>(sortedGaps.size() - 1));
            return sortedGaps[idx2];
        };
        p("  gap-between-closes (ticks): p10=%zu p50=%zu p90=%zu p99=%zu min=%zu max=%zu",
          pct(0.10), pct(0.50), pct(0.90), pct(0.99), sortedGaps.front(), sortedGaps.back());
    }

    p("\n=== Gang-math values sampled at IS3 closes (n=%zu) ===", hurst.n);
    p("  hurst_exponent:      min=%.4f mean=%.4f max=%.4f", hurst.lo, hurst.Mean(), hurst.hi);
    p("  recurrence_rate:     min=%.4f mean=%.4f max=%.4f", recurrence.lo, recurrence.Mean(), recurrence.hi);
    p("  skewness_idx:        min=%.4f mean=%.4f max=%.4f", skew.lo, skew.Mean(), skew.hi);
    p("  taleb_kurtosis:      min=%.4f mean=%.4f max=%.4f", kurt.lo, kurt.Mean(), kurt.hi);
    p("  shannon_flow_entropy: min=%.4f mean=%.4f max=%.4f", entropy.lo, entropy.Mean(), entropy.hi);
    p("  shannon_efficiency:   min=%.4f mean=%.4f max=%.4f", efficiency.lo, efficiency.Mean(), efficiency.hi);

    std::puts("\n(this validates the imbalance-clock cascade wiring pattern + is3BarClosed cadence");
    std::puts(" against real tick data -- it is not yet a claim about trade-outcome value, see");
    std::puts(" docs/superpowers/specs/2026-09-19-imbalance-clock-hotpath-and-standalone-backtester-spec.md §4/§5)");

    return 0;
}
