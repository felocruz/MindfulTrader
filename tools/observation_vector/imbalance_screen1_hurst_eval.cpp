// tools/observation_vector/imbalance_screen1_hurst_eval.cpp -- standalone validation for
// ImbalanceScreen1.cpp's proposed IS1 dim (docs/superpowers/specs/2026-09-06-imbalance-triple-
// screen-architecture-spec.md §1.1a): hurst_exponent computed on a DEDICATED, adaptively-
// thresholded imbalance-bar engine, vs. today's shipped fast_hurst_exponent (a single shared
// engine, ContextManager.cpp:607, DfaHurstExponent(returnsArray.data(), 100, 8) over a fixed-
// threshold-700 engine per this session's own prior sweep work).
//
// This does NOT attempt the eventual IS1:IS2:IS3 25:5:1 ratio (architecture spec Next Steps
// item 12, a real, deliberately deferred gap) -- IS1 is being validated alone here, nothing for
// a ratio to be relative to yet. EnableAdaptiveThreshold() is used specifically to avoid
// inventing an uncalibrated fixed constant for this first pass.
//
// Methodology: stream real MES ticks once, maintain TWO independent ImbalanceBarEngine instances
// side by side -- (a) "shipped": fixed threshold=700 (matching this session's own prior sweep
// choice for the single shared production engine), (b) "IS1 candidate": EnableAdaptiveThreshold()
// -- compute hurst_exponent (DfaHurstExponent, length=100/minScale=8, byte-for-byte matching
// ContextManager.cpp:607's own call) over each engine's own last-100-imbalance-bar-returns window
// whenever ITS OWN bar count advances, and report: (1) how the two engines' bar-formation rates
// differ (adaptive vs. fixed-700), (2) basic distributional stats for each hurst series (mean,
// std, min/max) so a human can judge whether the adaptive engine's IS1 candidate looks like a
// meaningfully different (hopefully more macro-stable) persistence signal, not just noise.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   -Iinclude tools/observation_vector/imbalance_screen1_hurst_eval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/imbalance_screen1_hurst_eval
// Usage: ./tools/bin/imbalance_screen1_hurst_eval \
//   --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet \
//   [--adaptive-alpha 0.01] [--max-rss-mb 4096]

#include "DfaHurstExponent.h"
#include "ImbalanceBarEngine.h"
#include "market_data_io.h"
#include "../ToolProgressLogger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kTotalTicksEstimate = 471'930'891;
constexpr float kFixedShippedThreshold = 700.0f;  // matches this session's own prior sweep choice

struct SeriesStats {
    std::vector<double> values;
    void Record(double v) { if (std::isfinite(v)) values.push_back(v); }
    double Mean() const {
        if (values.empty()) return 0.0;
        double sum = 0.0;
        for (double v : values) sum += v;
        return sum / static_cast<double>(values.size());
    }
    double StdDev() const {
        if (values.size() < 2) return 0.0;
        const double mean = Mean();
        double sq = 0.0;
        for (double v : values) sq += (v - mean) * (v - mean);
        return std::sqrt(sq / static_cast<double>(values.size() - 1));
    }
    double Min() const { return values.empty() ? 0.0 : *std::min_element(values.begin(), values.end()); }
    double Max() const { return values.empty() ? 0.0 : *std::max_element(values.begin(), values.end()); }
};

// One engine + its own hurst-tracking state -- shared logic for the "shipped" (fixed-700) and
// "IS1 candidate" (adaptive) pipelines.
struct HurstPipeline {
    ImbalanceBarEngine engine;
    std::size_t barsClosedBefore = 0;
    SeriesStats hurstStats;

    void OnTick(int tickIndex, float askVolume, float bidVolume, float price) {
        engine.OnTickWithPrice(tickIndex, askVolume, bidVolume, price);
        const std::size_t barsClosedNow = engine.GetCompletedBarCount();
        if (barsClosedNow == barsClosedBefore) return;
        barsClosedBefore = barsClosedNow;

        float returns[100];
        const std::size_t count = engine.GetImbalanceBarReturns(100, returns);
        if (count < 100) return;  // matches ContextManager.cpp:602's own warm-up gate (count >= 100)

        // Byte-for-byte the same call ContextManager.cpp:607 makes for the shipped fast_hurst_exponent.
        const float hurst = DfaHurstExponent(returns, 100, 8);
        hurstStats.Record(hurst);
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

    ToolProgressLogger progress("imbalance_screen1_hurst_eval");
    progress.SetScope("shipped(fixed=700) vs IS1-candidate(adaptive alpha=" + std::to_string(adaptiveAlpha) + ")");
    progress.Log("streaming from " + ticksPath);
    constexpr std::size_t kProgressEveryNTicks = 5'000'000;

    HurstPipeline shipped;
    shipped.engine.SetImbalanceThreshold(kFixedShippedThreshold);

    HurstPipeline candidate;
    candidate.engine.SetImbalanceThreshold(kFixedShippedThreshold);  // fixed fallback until seeded
    candidate.engine.EnableAdaptiveThreshold(adaptiveAlpha);

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
            shipped.OnTick(idx, static_cast<float>(askVolume), static_cast<float>(bidVolume), p);
            candidate.OnTick(idx, static_cast<float>(askVolume), static_cast<float>(bidVolume), p);
        });
    } catch (const std::exception& e) {
        progress.Log(std::string("aborted: ") + e.what());
        return 1;
    }

    progress.Log("streaming done (" + std::to_string(ticksProcessed) + " ticks)");

    char line[256];
    auto report = [&](const char* label, const HurstPipeline& p) {
        std::snprintf(line, sizeof(line), "\n=== %s ===  bars_closed=%zu  hurst_samples=%zu",
                      label, p.barsClosedBefore, p.hurstStats.values.size());
        std::puts(line); progress.Log(line);
        std::snprintf(line, sizeof(line), "  hurst_exponent: mean=%.4f std=%.4f min=%.4f max=%.4f",
                      p.hurstStats.Mean(), p.hurstStats.StdDev(), p.hurstStats.Min(), p.hurstStats.Max());
        std::puts(line); progress.Log(line);
    };
    report("shipped (fixed threshold=700, matches today's single shared engine)", shipped);
    report("IS1 candidate (EnableAdaptiveThreshold)", candidate);

    std::puts("\n(this validates IS1 in isolation only -- the IS1:IS2:IS3 25:5:1 ratio question");
    std::puts(" is a separate, deferred follow-on, see architecture spec Next Steps item 12)");

    return 0;
}
