// MarketDataReplay.cpp -- CLI driver for the offline .context generator
// (docs/superpowers/plans/2026-09-08-market-data-replay-implementation.md,
// Task 9; docs/superpowers/specs/2026-09-08-offline-context-generator-spec.md
// §3d). Streams the full multi-contract mes_ticks.parquet through
// MarketDataReplayEngine and writes .context records via LBRFileManager,
// exactly the same real gate ContextManager::CheckAndTriggerHMM() uses
// (docs/superpowers/specs/2026-09-08-context-emission-gate-quality-over-
// quantity-spec.md).
//
// Usage:
//   tools/bin/market_data_replay \
//     --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet \
//     --output <symbol>.context \
//     [--max-rss-mb 3072]
//
// Build: mamba run -n mts g++ -O2 -std=c++17 -Iinclude \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   tools/market_data_replay/MarketDataReplay.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/market_data_replay
// (mamba run -n mts is required -- FeatureScaler.h needs <nlohmann/json.hpp>,
// only resolvable via that env, same as every tools/observation_vector/*.cpp
// that includes it.)

#include "MarketDataReplayEngine.h"
#include "../observation_vector/market_data_io.h"
#include "../ToolProgressLogger.h"
#include "ContextFileWriter.h"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    std::string ticksPath;
    std::string outputPath;
    std::size_t maxRssMB = 3072;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks-parquet") == 0 && i + 1 < argc) {
            ticksPath = argv[++i];
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (std::strcmp(argv[i], "--max-rss-mb") == 0 && i + 1 < argc) {
            maxRssMB = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        }
    }
    if (ticksPath.empty() || outputPath.empty()) {
        std::fprintf(stderr,
                      "usage: %s --ticks-parquet PATH --output PATH.context [--max-rss-mb 3072]\n",
                      argv[0]);
        return 1;
    }

    ToolProgressLogger progress("market_data_replay");
    progress.Log("streaming from " + ticksPath + " -> " + outputPath +
                  " (max-rss-mb=" + std::to_string(maxRssMB) + ")");
    progress.Log(".alpha/.imbalance.context will be near-empty by design -- this tool only "
                 "populates .context (spec §3h)");

    ContextFileWriter contextWriter;
    if (!contextWriter.Open(outputPath, "ES")) {
        progress.Log("FATAL: ContextFileWriter::Open failed for " + outputPath);
        return 1;
    }

    MarketDataReplayEngine engine;

    constexpr std::size_t kProgressEveryNTicks = 5'000'000;
    constexpr std::size_t kInterimReportEveryNTicks = 50'000'000;
    std::size_t ticksProcessed = 0;
    std::size_t recordsWritten = 0;
    bool sawFirstWarmup = false;

    try {
        StreamTicksFullParquet(
            ticksPath,
            [&](std::int64_t ts, double price, std::int64_t volume, std::int64_t askVol,
                std::int64_t bidVol, bool isNewContract) {
                ++ticksProcessed;

                // Faithful-to-live-trading directive (spec §3a open Q1): a real
                // continuous chart never resets its indicators at a contract
                // roll, it just sees a single-tick price gap flow through the
                // same continuous calculations -- do NOT reset engine state
                // here, unlike whole_vector_redundancy_eval.cpp's own (correct
                // for its different goal) per-contract reset.
                if (isNewContract) {
                    progress.Log("contract roll at tick " + std::to_string(ticksProcessed) +
                                 " (timestamp_us=" + std::to_string(ts) + ") -- engine state NOT reset");
                }

                if (price <= 0.0) return;  // spec §3f: skip degenerate ticks, don't corrupt windows

                const bool wasWarmedUp = sawFirstWarmup;
                const bool significant = engine.OnTick(ts, price, volume, askVol, bidVol);

                if (significant) {
                    contextWriter.LogContext(
                        engine.GetObservation(), MTS::Schema::AsymmetryContext{},
                        static_cast<uint64_t>(ts), engine.GetBarsSinceLastUpdate(),
                        /*risk_gate_context=*/nullptr);
                    ++recordsWritten;
                    if (!wasWarmedUp) {
                        sawFirstWarmup = true;
                        progress.Log("warm-up complete, first .context record written at tick " +
                                     std::to_string(ticksProcessed));
                    }
                }

                if (ticksProcessed % kProgressEveryNTicks == 0) {
                    progress.LogProgress(ticksProcessed, 0);
                    progress.CheckMemoryBudget(maxRssMB);
                }
                if (ticksProcessed % kInterimReportEveryNTicks == 0) {
                    progress.Log("interim records written: " + std::to_string(recordsWritten));
                }
            });
    } catch (const std::exception& e) {
        progress.Log(std::string("FATAL: ") + e.what());
        contextWriter.Close();
        return 1;
    }

    engine.Flush();  // spec §3f: drops the final in-progress bar across all 3 timeframes if skipped
    contextWriter.Close();

    progress.Log("=== SUMMARY: ticks processed=" + std::to_string(ticksProcessed) +
                 " .context records written=" + std::to_string(recordsWritten) +
                 " significant-change rate=" +
                 std::to_string(ticksProcessed > 0
                                     ? static_cast<double>(recordsWritten) / static_cast<double>(ticksProcessed)
                                     : 0.0) +
                 " ===");
    return 0;
}
