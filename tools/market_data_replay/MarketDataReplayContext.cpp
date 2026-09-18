// MarketDataReplayContext.cpp -- CLI driver for the `.context`/`.alpha`
// generator (docs/superpowers/specs/2026-09-16-market-data-replay-alpha-
// generator-spec.md, plan Task 11). A separately-named entry point from
// MarketDataReplay.cpp (the dim-selection flat-Parquet exporter, untouched
// by this initiative, per spec §7 item 2's own approved decision) --
// reuses MarketDataReplayEngine.h's tick/bar core unmodified. Writes
// `.context.parquet` directly via ContextParquetWriter.h (operator
// directive, 2026-09-17: saves lbrnet from having to run
// tools/context_pipeline/context_to_parquet.cpp by hand on every new file)
// and native `.alpha` via AlphaFileWriter.h (kept byte-compatible with
// lbrnet's build_directional_alpha.py, which requires that exact format).
//
// Two independent triggers (spec §2), never unified:
//   Trigger 1 (`.context`): engine.OnTick()'s own existing Mahalanobis gate
//     (ComputeShouldEmit/ObservationTriggerGate) -- unchanged from the
//     already-shipped `.context` generator.
//   Trigger 2 (`.alpha`): PRIMARY_TRIGGER_MASK's per-tick dirty-bit mask
//     (Task 7), gated by Locks A/B/D/E (spec §1b/§3 item 1) -- Lock C stays
//     telemetry-only (never enforced), matching production exactly.
//
// Usage:
//   tools/bin/market_data_replay_context \
//     --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet \
//     --output <path> [--symbol MES] \
//     [--emit-context] [--emit-alpha] [--max-rss-mb 3072] [--max-ticks N]
// (`--output <path>` is a single base path, no extension -- this tool derives
// <path>.context.parquet/<path>.alpha internally, matching LBRFileManager::
// Open()'s own real `<path>.<ext>` convention, spec §5 item 6's resolved
// decision, just with `.context.parquet` instead of `.context`.)
//
// Build: mamba run -n mts g++ -O2 -std=c++17 -Iinclude \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   tools/market_data_replay/MarketDataReplayContext.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/market_data_replay_context

#include "MarketDataReplayEngine.h"
#include "ContextParquetWriter.h"
#include "AlphaFileWriter.h"
#include "../observation_vector/market_data_io.h"
#include "../ToolProgressLogger.h"

#include <cstdio>
#include <cstring>
#include <string>

namespace {

// Lock B: production's real m_warmupBarCount threshold
// (IndicatorManager::CheckWarmupStatus, src/IndicatorManager.cpp:579).
constexpr int kLockBBarCountThreshold = 200;

// Locks A/B/D/E (spec §1b/§3 item 1) -- Lock C is deliberately absent, never
// enforced, matching production's own explicit "data-collector must capture
// ALL market regimes" rationale (EventDataCollectorStudy.cpp's own comment).
bool AlphaLocksPass(const MarketDataReplayEngine& engine) {
    // Lock A: FeatureScaler warm-up (500-sample RANK_WINDOW).
    if (!engine.IsFeatureScalerWarmedUp()) return false;
    // Lock B: 200-bar counter + RSI producing a real (non-UNDEFINED) value
    // (src/IndicatorManager.cpp's own m_warmupBarCount>=200 && RSI!=0 pair).
    if (engine.GetTs3BarsClosed() < kLockBBarCountThreshold) return false;
    if (engine.GetRsiTopResult() == RSI::UNDEFINED) return false;
    // Locks D/E: TS1 macro / TS2 structural freshness. Production's real
    // gates are time-since-last-update staleness checks (6h/3h max age) --
    // meaningless for a continuous, gap-free offline tick replay (no live
    // disconnect can occur), so this tool's own documented substitute is
    // engine.IsAllDimsReady()'s existing per-timeframe bar-count sufficiency
    // check (already the "are TS1/TS2/TS3's own windows genuinely filled"
    // gate this engine relies on for trigger 1) -- a deliberate, documented
    // simplification, not a silent gap.
    if (!engine.IsAllDimsReady()) return false;
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticksPath;
    std::string outputPath;
    std::string symbol = "MES";
    bool emitContext = true;
    bool emitAlpha = false;
    std::size_t maxRssMB = 3072;
    std::size_t maxTicks = 0;  // 0 = unlimited
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks-parquet") == 0 && i + 1 < argc) {
            ticksPath = argv[++i];
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (std::strcmp(argv[i], "--symbol") == 0 && i + 1 < argc) {
            symbol = argv[++i];
        } else if (std::strcmp(argv[i], "--emit-context") == 0) {
            emitContext = true;
        } else if (std::strcmp(argv[i], "--no-emit-context") == 0) {
            emitContext = false;
        } else if (std::strcmp(argv[i], "--emit-alpha") == 0) {
            emitAlpha = true;
        } else if (std::strcmp(argv[i], "--no-emit-alpha") == 0) {
            emitAlpha = false;
        } else if (std::strcmp(argv[i], "--max-rss-mb") == 0 && i + 1 < argc) {
            maxRssMB = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--max-ticks") == 0 && i + 1 < argc) {
            maxTicks = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        }
    }
    if (ticksPath.empty() || outputPath.empty()) {
        std::fprintf(stderr,
                      "usage: %s --ticks-parquet PATH --output PATH [--symbol MES] "
                      "[--emit-context] [--no-emit-context] [--emit-alpha] [--no-emit-alpha] "
                      "[--max-rss-mb 3072] [--max-ticks N]\n",
                      argv[0]);
        return 1;
    }
    if (!emitContext && !emitAlpha) {
        std::fprintf(stderr, "nothing to do: both --emit-context and --emit-alpha are off\n");
        return 1;
    }

    ToolProgressLogger progress("market_data_replay_context");
    progress.Log("streaming from " + ticksPath + " -> " + outputPath +
                  ".context.parquet/.alpha (symbol=" + symbol +
                  ", emit-context=" + (emitContext ? "on" : "off") +
                  ", emit-alpha=" + (emitAlpha ? "on" : "off") +
                  ", max-rss-mb=" + std::to_string(maxRssMB) + ")");

    ContextParquetWriter contextWriter;
    AlphaFileWriter alphaWriter;
    if (emitContext && !contextWriter.Open(outputPath, symbol)) {
        progress.Log("FATAL: ContextParquetWriter::Open failed for " + outputPath + ".context.parquet");
        return 1;
    }
    if (emitAlpha && !alphaWriter.Open(outputPath, symbol)) {
        progress.Log("FATAL: AlphaFileWriter::Open failed for " + outputPath + ".alpha");
        return 1;
    }

    MarketDataReplayEngine engine;

    constexpr std::size_t kProgressEveryNTicks = 5'000'000;
    std::size_t ticksProcessed = 0;
    std::size_t contextRecordsWritten = 0;
    std::size_t alphaRecordsWritten = 0;
    bool fatalError = false;

    try {
        StreamTicksFullParquet(
            ticksPath,
            [&](std::int64_t ts, double price, std::int64_t volume, std::int64_t askVol,
                std::int64_t bidVol, bool isNewContract) {
                if (fatalError) return;
                if (maxTicks != 0 && ticksProcessed >= maxTicks) return;
                ++ticksProcessed;

                // Faithful-to-live-trading directive (same as MarketDataReplay.cpp's
                // own precedent, spec §3a open Q1): never reset engine state on a
                // contract roll -- a real continuous chart never does either.
                if (isNewContract) {
                    progress.Log("contract roll at tick " + std::to_string(ticksProcessed) +
                                 " -- engine state NOT reset");
                }
                if (price <= 0.0) return;  // skip degenerate ticks

                // Trigger 1 (`.context`): engine's own existing Mahalanobis gate.
                const bool significantContext = engine.OnTick(ts, price, volume, askVol, bidVol);
                if (emitContext && significantContext) {
                    const auto rgc = engine.BuildRiskGateContextT(ts);
                    // asymmetry_context: out of scope (spec §3 item 3's own
                    // disposition table -- requires StructureEngine/
                    // PositionManager this tool doesn't replicate) --
                    // documented zero-init sentinel, not a guessed value.
                    const MTS::Schema::AsymmetryContext emptyAsymmetry{};
                    contextWriter.LogContext(
                        engine.GetObservation(), emptyAsymmetry, static_cast<uint64_t>(ts),
                        engine.GetBarsSinceLastUpdate(), &rgc);
                    ++contextRecordsWritten;
                }

                // Trigger 2 (`.alpha`): independent PRIMARY_TRIGGER_MASK dirty-bit
                // schedule, gated by Locks A/B/D/E -- never unified with trigger 1
                // (spec §2's own core design decision).
                const uint64_t dirtyMask = engine.ConsumePatternDirtyMask();
                if (emitAlpha && dirtyMask != 0 && AlphaLocksPass(engine)) {
                    auto& event = engine.BuildTrainingEventT(
                        engine.GetTs3BarsClosed(), ts,
                        engine.GetTs3LiveOpen(), engine.GetTs3LiveHigh(),
                        engine.GetTs3LiveLow(), engine.GetTs3LiveClose(),
                        engine.GetTs3LiveVolume());
                    alphaWriter.LogAlpha(event);
                    ++alphaRecordsWritten;
                }

                if (ticksProcessed % kProgressEveryNTicks == 0) {
                    progress.LogProgress(ticksProcessed, 0);
                    progress.CheckMemoryBudget(maxRssMB);
                }
            });
    } catch (const std::exception& e) {
        progress.Log(std::string("FATAL: ") + e.what());
        // Close on ANY abort (not just a clean finish) -- otherwise whatever
        // rows were accumulated in ContextParquetWriter's in-memory chunk
        // buffer since the last flush (up to kChunkRows) are silently lost,
        // even though the file itself still looks valid (Arrow's FileWriter
        // destructor does a best-effort close either way). Found 2026-09-17:
        // a real --max-rss-mb abort left the .context.parquet file readable
        // but missing its last unflushed chunk because this path used to
        // `return 1` without calling Close() at all.
        try {
            contextWriter.Close();
        } catch (const std::exception& closeErr) {
            progress.Log(std::string("WARNING: contextWriter.Close() after abort also failed: ") + closeErr.what());
        }
        alphaWriter.Close();
        return 1;
    }

    if (fatalError) return 1;

    engine.Flush();  // drops the final in-progress bar across all 3 timeframes if skipped
    contextWriter.Close();
    alphaWriter.Close();

    progress.Log("=== SUMMARY: ticks processed=" + std::to_string(ticksProcessed) +
                 " .context records=" + std::to_string(contextRecordsWritten) +
                 " .alpha records=" + std::to_string(alphaRecordsWritten) + " ===");
    return 0;
}
