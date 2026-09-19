// structure_test_trigger_impact_eval.cpp -- targeted, cheap measurement of the
// 2026-09-19 STRUCTURE_TEST significant-transition trigger fix's real effect
// (docs/superpowers/specs/2026-09-19-meaningful-event-trigger-and-asymmetry-
// context-significance-spec.md, Phase 1), replacing the earlier 30M-tick A/B
// file-diff approach that came back inconclusive (byte-identical output --
// too small/quiet a slice to exercise a rare event, not evidence the fix is a
// no-op). Streams the FULL real tick dataset once, tracking aggregate counts
// instead of writing multi-GB output files -- answers the actual question
// ("how often does this fix add a net-new alpha capture event across all real
// history") directly and cheaply, in one pass.
//
// Key measurement: an "isolated significant transition" is a tick where
// StructureTest crosses into/out of/between its actionable (FAILED_*/
// DECISIVE_*) states per IsStructureTestSignificantTransition(), AND no other
// PRIMARY_TRIGGER_MASK-tracked field was also dirty on that same tick. Before
// today's fix, STRUCTURE_TEST's CheckTrigger() always returned false (no
// ShouldTrigger() override existed), so every one of these ticks would have
// produced ZERO alpha capture in the ACSIL-coupled production path -- this
// count is exactly how many net-new capture events the fix adds.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 -Iinclude \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   tools/market_data_replay/structure_test_trigger_impact_eval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/structure_test_trigger_impact_eval
// Usage: ./tools/bin/structure_test_trigger_impact_eval \
//   --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet \
//   [--max-ticks N] [--max-rss-mb 3072]

#include "MarketDataReplayEngine.h"
#include "../observation_vector/market_data_io.h"
#include "../ToolProgressLogger.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace {

constexpr std::size_t kTotalTicksEstimate = 471'930'891;
constexpr std::size_t kProgressEveryNTicks = 20'000'000;

bool IsTrap(StructureTest v) {
    return v == StructureTest::FAILED_LOW_CLOSE_INSIDE || v == StructureTest::FAILED_LOW_STRONG_REVERSAL ||
           v == StructureTest::FAILED_HIGH_CLOSE_INSIDE || v == StructureTest::FAILED_HIGH_STRONG_REVERSAL;
}

bool IsRegimeInvalidation(StructureTest v) {
    return v == StructureTest::DECISIVE_BREAKOUT_HIGH || v == StructureTest::DECISIVE_BREAKDOWN_LOW;
}

// Matches MarketDataReplayEngine's own private IndicatorKeyBit() -- duplicated
// here rather than exposed, this tool only needs the one bit.
constexpr uint64_t kStructureTestBit = 1ULL << static_cast<uint64_t>(IndicatorKey::STRUCTURE_TEST);

}  // namespace

int main(int argc, char** argv) {
    std::string ticksPath;
    std::size_t maxTicks = 0;  // 0 = unlimited
    std::size_t maxRssMB = 3072;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks-parquet") == 0 && i + 1 < argc) {
            ticksPath = argv[++i];
        } else if (std::strcmp(argv[i], "--max-ticks") == 0 && i + 1 < argc) {
            maxTicks = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--max-rss-mb") == 0 && i + 1 < argc) {
            maxRssMB = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        }
    }
    if (ticksPath.empty()) {
        std::fprintf(stderr, "usage: %s --ticks-parquet PATH [--max-ticks N] [--max-rss-mb 3072]\n", argv[0]);
        return 1;
    }

    ToolProgressLogger progress("structure_test_trigger_impact_eval");
    progress.SetScope("2026-09-19 STRUCTURE_TEST significant-transition trigger fix -- real-data impact count");

    MarketDataReplayEngine engine;
    StructureTest prevValue = StructureTest::NONE;
    std::size_t ticksProcessed = 0;
    std::size_t anyChangeCount = 0;
    std::size_t newSignificantCount = 0;
    std::size_t isolatedSignificantCount = 0;
    std::size_t isolatedTrapCount = 0;
    std::size_t isolatedRegimeInvalidationCount = 0;

    try {
        StreamTicksFullParquet(
            ticksPath,
            [&](std::int64_t ts, double price, std::int64_t volume, std::int64_t askVol,
                std::int64_t bidVol, bool /*isNewContract*/) {
                if (maxTicks != 0 && ticksProcessed >= maxTicks) return;
                ++ticksProcessed;
                if (price <= 0.0) return;

                const bool significantBefore = engine.OnTick(ts, price, volume, askVol, bidVol);
                (void)significantBefore;  // Trigger 1 (.context) gate -- not this tool's concern

                const StructureTest currentValue = engine.GetStructureTestResult();
                const uint64_t dirtyMask = engine.ConsumePatternDirtyMask();

                if (currentValue != prevValue) ++anyChangeCount;
                if (IsStructureTestSignificantTransition(prevValue, currentValue)) {
                    ++newSignificantCount;
                    const bool isolated = dirtyMask == kStructureTestBit;
                    if (isolated) {
                        ++isolatedSignificantCount;
                        if (IsTrap(currentValue)) ++isolatedTrapCount;
                        if (IsRegimeInvalidation(currentValue)) ++isolatedRegimeInvalidationCount;
                    }
                }
                prevValue = currentValue;

                if (ticksProcessed % kProgressEveryNTicks == 0) {
                    progress.LogProgress(ticksProcessed, kTotalTicksEstimate);
                    progress.CheckMemoryBudget(maxRssMB);
                }
            });
    } catch (const std::exception& e) {
        progress.Log(std::string("FATAL: ") + e.what());
        return 1;
    }

    progress.Log("=== RESULTS ===");
    progress.Log("ticks_processed=" + std::to_string(ticksProcessed));
    progress.Log("any_change_count=" + std::to_string(anyChangeCount) +
                 " (old offline-tool semantics: any value change)");
    progress.Log("new_significant_count=" + std::to_string(newSignificantCount) +
                 " (2026-09-19 fix's own significance test: FAILED_*/DECISIVE_* entered/exited/switched)");
    progress.Log("isolated_significant_count=" + std::to_string(isolatedSignificantCount) +
                 " (net-new alpha-capture events the fix adds -- STRUCTURE_TEST was the ONLY dirty "
                 "primary-masked field, so CheckTrigger() always returned false here before today's "
                 "fix, in the ACSIL-coupled production path)");
    progress.Log("  of which isolated_trap_count=" + std::to_string(isolatedTrapCount));
    progress.Log("  of which isolated_regime_invalidation_count=" + std::to_string(isolatedRegimeInvalidationCount));
    return 0;
}
