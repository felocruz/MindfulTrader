// tools/observation_vector/imbalance_clock_manager_ratio_eval.cpp -- empirical validation for
// the ImbalanceClockManager K2/K1 bars-of-bars aggregation ratio (docs/superpowers/specs/
// 2026-09-06-imbalance-triple-screen-architecture-spec.md §1.2a/§1.2b Next Steps item 12's
// remaining open follow-on: "5/5 is literature-plausible, not yet data-derived for this
// instrument").
//
// Two literature-grounded candidates are compared head-to-head on the SAME real tick stream:
//   - K2=K1=4: matches THIS repo's own existing calendar-clock precedent exactly
//     (TS1:TS2:TS3 = 240:60:15 min = a clean 4:1 ratio at each hop, confirmed via
//     src/TripleScreen1/2/3.cpp's own bar-period constants).
//   - K2=K1=5: matches the "25:5:1" architecture-spec mandate and the upper end of the
//     multifractal-cascade scale-separation range (m≈4-5, Mandelbrot 1997; Calvet & Fisher 2002).
//
// Empirical criterion: subordination theory (Clark 1973; Ané & Geman 2000) predicts that
// information-time-sampled returns are closer to i.i.d. than calendar-time returns, and that
// this effect should be MORE pronounced at coarser (more heavily-aggregated) scales, not less --
// i.e., lag-1 autocorrelation of returns should trend toward zero (not away from it) IS3 -> IS2
// -> IS1. This is not a claim that either K is "correct" in some absolute sense -- it's a
// falsifiable, data-groundable signal for whether one candidate ratio produces a cleaner,
// more monotonic decorrelation trend than the other on THIS instrument's real tick data, which
// is the same kind of "let real data adjudicate between literature-plausible candidates"
// discipline already used for fractal_dim's window and burstiness_index's reformulation.
//
// Both candidates use ImbalanceClockManager::EnableIs3AdaptiveThreshold() (not an uncalibrated
// fixed constant) -- same choice already validated in the sibling tool,
// imbalance_screen1_hurst_eval.cpp.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   -Iinclude tools/observation_vector/imbalance_clock_manager_ratio_eval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/imbalance_clock_manager_ratio_eval
// Usage: ./tools/bin/imbalance_clock_manager_ratio_eval \
//   --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet \
//   [--adaptive-alpha 0.01] [--max-rss-mb 4096]

#include "ImbalanceClockManager.h"
#include "market_data_io.h"
#include "../ToolProgressLogger.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>

namespace {

constexpr std::size_t kTotalTicksEstimate = 471'930'891;
constexpr float kFixedFallbackThreshold = 700.0f;  // seed value before the adaptive estimate engages

// Online (streaming, O(1)-memory) lag-1 autocorrelation -- no need to retain the full return
// history, matching this codebase's no-heap/bounded-memory hot-path discipline even in tools/.
struct AutocorrTracker {
    double sumX = 0.0, sumX2 = 0.0, sumCross = 0.0;
    double prevX = 0.0;
    bool hasPrev = false;
    std::size_t n = 0, nCross = 0;

    void Update(double x) {
        if (!std::isfinite(x)) return;
        if (hasPrev) { sumCross += prevX * x; ++nCross; }
        sumX += x;
        sumX2 += x * x;
        ++n;
        prevX = x;
        hasPrev = true;
    }

    double Lag1Autocorr() const {
        if (n < 2 || nCross < 1) return 0.0;
        const double mean = sumX / static_cast<double>(n);
        const double var = sumX2 / static_cast<double>(n) - mean * mean;
        if (var <= 0.0) return 0.0;
        const double crossMean = sumCross / static_cast<double>(nCross);
        return (crossMean - mean * mean) / var;
    }
};

struct RatioPipeline {
    ImbalanceClockManager mgr;
    std::size_t is3Before = 0, is2Before = 0, is1Before = 0;
    AutocorrTracker is3Auto, is2Auto, is1Auto;

    void Configure(std::size_t k, float adaptiveAlpha) {
        mgr.SetAggregationCounts(k, k);
        mgr.ConfigureIs3Threshold(kFixedFallbackThreshold);
        mgr.EnableIs3AdaptiveThreshold(adaptiveAlpha);
    }

    void OnTick(int idx, float askVol, float bidVol, float price) {
        mgr.OnTick(idx, askVol, bidVol, price);

        const std::size_t is3After = mgr.GetIs3CompletedBarCount();
        if (is3After != is3Before) {
            is3Before = is3After;
            float v = 0.0f;
            mgr.GetIs3Returns(1, &v);
            is3Auto.Update(v);
        }
        const std::size_t is2After = mgr.GetIs2CompletedBarCount();
        if (is2After != is2Before) {
            is2Before = is2After;
            float v = 0.0f;
            mgr.GetIs2Returns(1, &v);
            is2Auto.Update(v);
        }
        const std::size_t is1After = mgr.GetIs1CompletedBarCount();
        if (is1After != is1Before) {
            is1Before = is1After;
            float v = 0.0f;
            mgr.GetIs1Returns(1, &v);
            is1Auto.Update(v);
        }
    }
};

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --ticks-parquet PATH [--adaptive-alpha 0.01] [--max-rss-mb 4096]\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticksPath;
    float adaptiveAlpha = 0.01f;
    std::size_t maxRssMB = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--ticks-parquet") ticksPath = next("--ticks-parquet");
        else if (arg == "--adaptive-alpha") adaptiveAlpha = std::stof(next("--adaptive-alpha"));
        else if (arg == "--max-rss-mb") maxRssMB = std::stoull(next("--max-rss-mb"));
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); PrintUsage(argv[0]); return 1; }
    }
    if (ticksPath.empty()) { PrintUsage(argv[0]); return 1; }

    ToolProgressLogger progress("imbalance_clock_manager_ratio_eval");
    progress.SetScope("K2=K1=4 (calendar-clock precedent) vs K2=K1=5 (25:5:1 mandate), both IS3-adaptive alpha=" +
                       std::to_string(adaptiveAlpha));
    progress.Log("streaming from " + ticksPath);
    constexpr std::size_t kProgressEveryNTicks = 5'000'000;

    RatioPipeline k4;
    k4.Configure(4, adaptiveAlpha);
    RatioPipeline k5;
    k5.Configure(5, adaptiveAlpha);

    long long tickIndex = 0;
    std::size_t ticksProcessed = 0;

    try {
        StreamTicksFullParquet(ticksPath, [&](std::int64_t /*ts*/, double price, std::int64_t /*volume*/,
                                               std::int64_t askVolume, std::int64_t bidVolume,
                                               bool /*isNewContract*/) {
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
            k4.OnTick(idx, ask, bid, p);
            k5.OnTick(idx, ask, bid, p);
        });
    } catch (const std::exception& e) {
        progress.Log(std::string("aborted: ") + e.what());
        return 1;
    }

    progress.Log("streaming done (" + std::to_string(ticksProcessed) + " ticks)");

    char line[320];
    auto report = [&](const char* label, const RatioPipeline& p) {
        std::snprintf(line, sizeof(line), "\n=== %s ===", label);
        std::puts(line); progress.Log(line);
        std::snprintf(line, sizeof(line),
            "  IS3: bars=%zu  lag1_autocorr=%.4f", p.is3Before, p.is3Auto.Lag1Autocorr());
        std::puts(line); progress.Log(line);
        std::snprintf(line, sizeof(line),
            "  IS2: bars=%zu  lag1_autocorr=%.4f", p.is2Before, p.is2Auto.Lag1Autocorr());
        std::puts(line); progress.Log(line);
        std::snprintf(line, sizeof(line),
            "  IS1: bars=%zu  lag1_autocorr=%.4f", p.is1Before, p.is1Auto.Lag1Autocorr());
        std::puts(line); progress.Log(line);

        const double a3 = std::abs(p.is3Auto.Lag1Autocorr());
        const double a2 = std::abs(p.is2Auto.Lag1Autocorr());
        const double a1 = std::abs(p.is1Auto.Lag1Autocorr());
        const bool monotonicDecorrelation = (a2 <= a3) && (a1 <= a2);
        std::snprintf(line, sizeof(line),
            "  |autocorr| monotonically decreasing IS3->IS2->IS1: %s",
            monotonicDecorrelation ? "YES" : "NO");
        std::puts(line); progress.Log(line);
    };
    report("K2=K1=4 (calendar-clock precedent)", k4);
    report("K2=K1=5 (25:5:1 mandate / literature upper bound)", k5);

    std::puts("\n(this adjudicates between two literature-plausible K candidates via real-data");
    std::puts(" decorrelation trend -- it is not a claim that either K is uniquely 'correct';");
    std::puts(" see architecture spec §1.2a/§1.2b Next Steps item 12 for the full context)");

    return 0;
}
