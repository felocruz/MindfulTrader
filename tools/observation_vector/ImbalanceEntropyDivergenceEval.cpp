// ImbalanceEntropyDivergenceEval.cpp -- empirical validation for the Gang-MACD/Phase-Coherence
// entropy engine (docs/superpowers/specs/2026-09-06-imbalance-triple-screen-architecture-spec.md
// §1.3a/§1.3b) BEFORE wiring its divergence sub-case ("Thermodynamic Exhaustion") into the native
// TRAP/StructureTest framework -- per an independent Gemini CLI read-only literature consult,
// 2026-09-09: validate dH_norm/dtau is a stable, sensible signal on real imbalance-bar data first.
//
// Reuses the REAL production code path, not a reimplementation: streams real ticks through
// ImbalanceClockManager::OnTick() -> ImbalanceContextManager::Update() (feeds the entropy engine)
// -> ImbalanceIndicatorManager::Update() (the actual shipped Gang-MACD/Phase-Coherence signal via
// ComputeGangMacdPhaseCoherence()). Only the NOT-yet-wired divergence condition itself is
// reconstructed here (phaseVelocity/entropyTrend aren't exposed by ImbalanceIndicatorManager's own
// accessors, which only return the final signal+quality) -- exactly matching the spec's own
// structural definition:
//   P(tau_2) > P(tau_1)  while  v_phase(tau_2) < v_phase(tau_1)  and  dH_norm/dtau > 0
// operationalized on two CONSECUTIVE completed IS2 bars (tau_1, tau_2), since v_phase(tau) IS the
// bar's own log return by construction (P(tau_2) > P(tau_1) <=> v_phase(tau_2) > 0).
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   -Iinclude tools/observation_vector/ImbalanceEntropyDivergenceEval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/imbalance_entropy_divergence_eval
// Usage: ./tools/bin/imbalance_entropy_divergence_eval \
//   --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet \
//   [--adaptive-alpha 0.01] [--max-ticks N] [--max-rss-mb 4096]

#include "ImbalanceClockManager.h"
#include "ImbalanceContextManager.h"
#include "ImbalanceIndicatorManager.h"
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
constexpr float kFixedFallbackThreshold = 700.0f;  // seed value before the adaptive estimate engages

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

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --ticks-parquet PATH [--adaptive-alpha 0.01] [--max-ticks N] [--max-rss-mb 4096]\n",
        argv0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticksPath;
    float adaptiveAlpha = 0.01f;
    std::size_t maxTicks = 0;  // 0 = unbounded
    std::size_t maxRssMB = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--ticks-parquet") ticksPath = next("--ticks-parquet");
        else if (arg == "--adaptive-alpha") adaptiveAlpha = std::stof(next("--adaptive-alpha"));
        else if (arg == "--max-ticks") maxTicks = static_cast<std::size_t>(std::stoull(next("--max-ticks")));
        else if (arg == "--max-rss-mb") maxRssMB = static_cast<std::size_t>(std::stoull(next("--max-rss-mb")));
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); PrintUsage(argv[0]); return 1; }
    }
    if (ticksPath.empty()) { PrintUsage(argv[0]); return 1; }

    ToolProgressLogger progress("imbalance_entropy_divergence_eval");
    progress.SetScope("adaptive-alpha=" + std::to_string(adaptiveAlpha) +
                       (maxTicks > 0 ? " max-ticks=" + std::to_string(maxTicks) : ""));
    progress.Log("streaming from " + ticksPath);
    constexpr std::size_t kProgressEveryNTicks = 5'000'000;

    ImbalanceClockManager::Instance().ConfigureIs3Threshold(kFixedFallbackThreshold);
    ImbalanceClockManager::Instance().EnableIs3AdaptiveThreshold(adaptiveAlpha);

    std::size_t is2BarsBefore = 0;
    bool hasPrevPhaseVelocity = false;
    float prevPhaseVelocity = 0.0f;
    bool hasPrevNormalizedEntropy = false;
    float prevNormalizedEntropy = 0.5f;

    SeriesStats entropyTrendStats;
    std::size_t redCount = 0, blueCount = 0, greenCount = 0;
    std::size_t divergenceFireCount = 0;
    std::size_t is2BarsWithEntropySample = 0;

    long long tickIndex = 0;
    std::size_t ticksProcessed = 0;
    bool stopEarly = false;

    try {
        StreamTicksFullParquet(ticksPath, [&](std::int64_t /*ts*/, double price, std::int64_t /*volume*/,
                                               std::int64_t askVolume, std::int64_t bidVolume,
                                               bool /*isNewContract*/) {
            if (stopEarly) return;
            ++ticksProcessed;
            if (ticksProcessed % kProgressEveryNTicks == 0) {
                progress.LogProgress(ticksProcessed, kTotalTicksEstimate);
                if (maxRssMB > 0) progress.CheckMemoryBudget(maxRssMB);
            }
            if (maxTicks > 0 && ticksProcessed >= maxTicks) stopEarly = true;
            if (price <= 0.0) return;

            const int idx = static_cast<int>(tickIndex++);
            ImbalanceClockManager::Instance().OnTick(
                idx, static_cast<float>(askVolume), static_cast<float>(bidVolume), static_cast<float>(price));
            ImbalanceContextManager::Instance().Update();
            ImbalanceIndicatorManager::Instance().Update();

            const std::size_t is2BarsAfter = ImbalanceClockManager::Instance().GetIs2CompletedBarCount();
            if (is2BarsAfter == is2BarsBefore) return;
            is2BarsBefore = is2BarsAfter;

            float phaseVelocity = 0.0f;
            ImbalanceClockManager::Instance().GetIs2Returns(1, &phaseVelocity);
            const float normalizedEntropy = 1.0f - ImbalanceContextManager::Instance().GetShannonEfficiency();

            const int8_t signal = ImbalanceIndicatorManager::Instance().GetSignal<
                ImbalanceIndicatorManager::IMBALANCE_IND_GANG_MACD_PHASE_COHERENCE>();
            if (signal > 0) ++greenCount;
            else if (signal < 0) ++redCount;
            else ++blueCount;

            if (hasPrevNormalizedEntropy) {
                const float entropyTrend = normalizedEntropy - prevNormalizedEntropy;
                entropyTrendStats.Record(entropyTrend);
                ++is2BarsWithEntropySample;

                if (hasPrevPhaseVelocity) {
                    const bool priceHigher = phaseVelocity > 0.0f;
                    const bool decelerating = phaseVelocity < prevPhaseVelocity;
                    const bool entropyRising = entropyTrend > 0.0f;
                    if (priceHigher && decelerating && entropyRising) ++divergenceFireCount;
                }
            }

            prevPhaseVelocity = phaseVelocity;
            hasPrevPhaseVelocity = true;
            prevNormalizedEntropy = normalizedEntropy;
            hasPrevNormalizedEntropy = true;
        });
    } catch (const std::exception& e) {
        progress.Log(std::string("aborted: ") + e.what());
        return 1;
    }

    progress.Log("streaming done (" + std::to_string(ticksProcessed) + " ticks, " +
                 std::to_string(is2BarsBefore) + " IS2 bars closed)");

    char line[256];
    std::snprintf(line, sizeof(line),
        "dH_norm/dtau: n=%zu mean=%.6f std=%.6f min=%.6f max=%.6f",
        entropyTrendStats.values.size(), entropyTrendStats.Mean(), entropyTrendStats.StdDev(),
        entropyTrendStats.Min(), entropyTrendStats.Max());
    std::puts(line); progress.Log(line);

    const std::size_t totalSignals = redCount + blueCount + greenCount;
    std::snprintf(line, sizeof(line),
        "Gang-MACD/Phase-Coherence signal counts: RED=%zu BLUE=%zu GREEN=%zu (rates %.4f/%.4f/%.4f)",
        redCount, blueCount, greenCount,
        totalSignals ? static_cast<double>(redCount) / totalSignals : 0.0,
        totalSignals ? static_cast<double>(blueCount) / totalSignals : 0.0,
        totalSignals ? static_cast<double>(greenCount) / totalSignals : 0.0);
    std::puts(line); progress.Log(line);

    std::snprintf(line, sizeof(line),
        "Thermodynamic Exhaustion divergence (price up + decelerating + entropy rising, "
        "consecutive IS2 bars): fired=%zu / eligible=%zu (rate=%.6f)",
        divergenceFireCount, is2BarsWithEntropySample,
        is2BarsWithEntropySample ? static_cast<double>(divergenceFireCount) / is2BarsWithEntropySample : 0.0);
    std::puts(line); progress.Log(line);

    return 0;
}
