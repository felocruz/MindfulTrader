// tools/observation_vector/asymmetry_context_dim_extractor.cpp -- real-data extraction of
// AsymmetryContext's two uncalibrated dims (roughness_ratio, session_quality_score), Tasks 4/5
// of docs/superpowers/plans/2026-09-19-decision-boundary-calibration-tool-implementation.md.
//
// Parallelized across the Puget machine's cores (32 hardware threads confirmed available,
// headroom checked via free -h/ps aux before launch, per /memories/user/shared_hardware_
// concurrency.md): mes_ticks.parquet's row groups are each single-contract and chronologically
// ordered (verified 2026-09-02, market_data_io.h's own comment), so splitting [0,num_row_groups)
// into N contiguous, non-overlapping shards preserves per-shard chronological order -- required
// for correct TickBarAggregator/StructureEngine state, which a work-stealing/arbitrary-order
// split would corrupt. Same "one independent unit of work per thread" shape as
// scid_to_ticks_parquet.cpp's per-contract worker pool, adapted to row-group ranges since this
// tool needs one continuous stream, not N independent output files.
//
// Usage:
//   tools/bin/asymmetry_context_dim_extractor --ticks-parquet PATH \
//     [--threads N] [--out-roughness PATH] [--out-session-quality PATH]
//
// Build: mamba run -n mts g++ -O2 -std=c++17 -Iinclude \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   tools/observation_vector/asymmetry_context_dim_extractor.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -lpthread -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/asymmetry_context_dim_extractor

#include "IndicatorComputations.h"  // TimeOfDayEnum, ClassifyTimeOfDay, ComputeSessionQualityScore
#include "StructureEngine.h"
#include "TickBarAggregator.h"
#include "EasternTimeOffset.h"
#include "../ToolProgressLogger.h"
#include "DecisionBoundaryCalibration.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr int64_t kTs3BarPeriodSeconds = 15 * 60;
constexpr int kCmeEsSessionStartSecondsET = 18 * 3600;  // matches EventDataCollectorStudy.cpp

struct ShardResult {
    std::vector<double> roughnessRatio;
    std::vector<double> sessionQualityScore;
};

// One thread's independent work unit: stream row groups [firstRowGroup, lastRowGroup) of the
// same file through its own FileReader/TickBarAggregator/StructureEngine -- no state shared with
// any other thread, so no locking needed on the hot path.
ShardResult ProcessShard(const std::string& path, int firstRowGroup, int lastRowGroup) {
    ShardResult result;

    auto infile_result = arrow::io::ReadableFile::Open(path);
    if (!infile_result.ok()) {
        throw std::runtime_error("ProcessShard: cannot open " + path);
    }
    parquet::arrow::FileReaderBuilder builder;
    if (!builder.Open(*infile_result).ok()) {
        throw std::runtime_error("ProcessShard: FileReaderBuilder::Open failed");
    }
    std::unique_ptr<parquet::arrow::FileReader> reader;
    if (!builder.Build(&reader).ok()) {
        throw std::runtime_error("ProcessShard: FileReaderBuilder::Build failed");
    }

    std::shared_ptr<arrow::Schema> schema;
    if (!reader->GetSchema(&schema).ok()) {
        throw std::runtime_error("ProcessShard: GetSchema failed");
    }
    const int ts_idx = schema->GetFieldIndex("timestamp_us");
    const int price_idx = schema->GetFieldIndex("trade_price");
    if (ts_idx < 0 || price_idx < 0) {
        throw std::runtime_error("ProcessShard: missing timestamp_us/trade_price column");
    }

    std::vector<int> row_groups;
    row_groups.reserve(static_cast<std::size_t>(lastRowGroup - firstRowGroup));
    for (int i = firstRowGroup; i < lastRowGroup; ++i) row_groups.push_back(i);

    auto batch_reader_result = reader->GetRecordBatchReader(row_groups, {ts_idx, price_idx});
    if (!batch_reader_result.ok()) {
        throw std::runtime_error("ProcessShard: GetRecordBatchReader failed");
    }
    auto batch_reader = std::move(*batch_reader_result);

    MindfulTrader::StructureEngine structureEngine;

    auto onBarClose = [&](const tba::Bar& bar) {
        structureEngine.Update(static_cast<float>(bar.high), static_cast<float>(bar.low),
                                static_cast<float>(bar.close), /*isNewBar=*/true);
        if (structureEngine.IsReady()) {
            result.roughnessRatio.push_back(static_cast<double>(structureEngine.GetRoughnessRatio()));
        }

        // ET hour/minute from the bar's own close timestamp -- session_quality_score is a
        // per-bar-cadence sample here (matches roughness_ratio's own cadence), not per-tick;
        // the underlying TimeOfDayEnum only transitions at session boundaries far coarser than
        // tick frequency, so bar-close sampling loses nothing real.
        const int64_t utcSeconds = bar.closeTimeUs / 1'000'000LL;
        const int etOffsetSeconds = ete::GetEasternUtcOffsetSeconds(utcSeconds);
        const int64_t etSeconds = utcSeconds + etOffsetSeconds;
        const int hour = static_cast<int>((etSeconds / 3600) % 24);
        const int minute = static_cast<int>((etSeconds / 60) % 60);
        const TimeOfDayEnum tod = ClassifyTimeOfDay(hour, minute, /*hasOpenPosition=*/false);
        result.sessionQualityScore.push_back(static_cast<double>(ComputeSessionQualityScore(tod)));
    };

    tba::TickBarAggregator ts3(kTs3BarPeriodSeconds, kCmeEsSessionStartSecondsET, onBarClose);

    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        if (!batch_reader->ReadNext(&batch).ok()) {
            throw std::runtime_error("ProcessShard: ReadNext failed");
        }
        if (batch == nullptr) break;
        auto ts_arr = std::static_pointer_cast<arrow::Int64Array>(batch->column(0));
        auto price_arr = std::static_pointer_cast<arrow::DoubleArray>(batch->column(1));
        for (int64_t i = 0; i < batch->num_rows(); ++i) {
            ts3.OnTick(ts_arr->Value(i), price_arr->Value(i), /*volume=*/0, /*askVolume=*/0, /*bidVolume=*/0);
        }
    }
    ts3.Flush();

    return result;
}

void WriteValues(const std::string& path, const std::vector<double>& values) {
    if (path.empty()) return;
    std::ofstream out(path);
    for (double v : values) out << v << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticksPath;
    std::string outRoughnessPath;
    std::string outSessionQualityPath;
    unsigned requestedThreads = 0;  // 0 = auto (hardware_concurrency)
    int maxRowGroups = 0;  // 0 = all -- smoke-test knob only, NOT for a real calibration run

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks-parquet") == 0 && i + 1 < argc) {
            ticksPath = argv[++i];
        } else if (std::strcmp(argv[i], "--threads") == 0 && i + 1 < argc) {
            requestedThreads = static_cast<unsigned>(std::atoi(argv[++i]));
        } else if (std::strcmp(argv[i], "--max-row-groups") == 0 && i + 1 < argc) {
            maxRowGroups = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--out-roughness") == 0 && i + 1 < argc) {
            outRoughnessPath = argv[++i];
        } else if (std::strcmp(argv[i], "--out-session-quality") == 0 && i + 1 < argc) {
            outSessionQualityPath = argv[++i];
        }
    }
    if (ticksPath.empty()) {
        std::fprintf(stderr,
                      "usage: %s --ticks-parquet PATH [--threads N] "
                      "[--out-roughness PATH] [--out-session-quality PATH]\n",
                      argv[0]);
        return 1;
    }

    ToolProgressLogger logger("asymmetry_context_dim_extractor");

    int numRowGroups = 0;
    {
        auto infile_result = arrow::io::ReadableFile::Open(ticksPath);
        if (!infile_result.ok()) {
            logger.Log("ERROR: cannot open " + ticksPath);
            return 1;
        }
        parquet::arrow::FileReaderBuilder builder;
        if (!builder.Open(*infile_result).ok()) {
            logger.Log("ERROR: FileReaderBuilder::Open failed");
            return 1;
        }
        std::unique_ptr<parquet::arrow::FileReader> reader;
        if (!builder.Build(&reader).ok()) {
            logger.Log("ERROR: FileReaderBuilder::Build failed");
            return 1;
        }
        numRowGroups = reader->num_row_groups();
    }
    if (maxRowGroups > 0) {
        numRowGroups = std::min(numRowGroups, maxRowGroups);
    }

    const unsigned hwThreads = std::max(1u, std::thread::hardware_concurrency());
    const unsigned nThreads = std::max(
        1u, std::min({requestedThreads == 0 ? hwThreads : requestedThreads, hwThreads,
                       static_cast<unsigned>(numRowGroups)}));

    logger.Log("row_groups=" + std::to_string(numRowGroups) + " threads=" + std::to_string(nThreads));

    std::vector<ShardResult> shardResults(nThreads);
    std::vector<std::thread> workers;
    workers.reserve(nThreads);
    const int baseChunk = numRowGroups / static_cast<int>(nThreads);
    const int remainder = numRowGroups % static_cast<int>(nThreads);
    int nextStart = 0;
    std::mutex logMutex;
    for (unsigned t = 0; t < nThreads; ++t) {
        const int chunkSize = baseChunk + (static_cast<int>(t) < remainder ? 1 : 0);
        const int first = nextStart;
        const int last = first + chunkSize;
        nextStart = last;
        workers.emplace_back([&, t, first, last]() {
            ShardResult r = ProcessShard(ticksPath, first, last);
            {
                std::lock_guard<std::mutex> lock(logMutex);
                logger.Log("shard " + std::to_string(t) + " [" + std::to_string(first) + "," +
                            std::to_string(last) + ") -> " + std::to_string(r.roughnessRatio.size()) +
                            " bar samples");
            }
            shardResults[t] = std::move(r);
        });
    }
    for (auto& w : workers) w.join();

    std::vector<double> roughness;
    std::vector<double> sessionQuality;
    for (const auto& r : shardResults) {
        roughness.insert(roughness.end(), r.roughnessRatio.begin(), r.roughnessRatio.end());
        sessionQuality.insert(sessionQuality.end(), r.sessionQualityScore.begin(), r.sessionQualityScore.end());
    }

    logger.Log("total roughness_ratio samples=" + std::to_string(roughness.size()));
    logger.Log("total session_quality_score samples=" + std::to_string(sessionQuality.size()));

    if (!roughness.empty()) {
        std::vector<double> sorted = roughness;
        std::sort(sorted.begin(), sorted.end());
        char line[256];
        std::snprintf(line, sizeof(line),
                      "roughness_ratio: p10=%.6f p50=%.6f p90=%.6f p99=%.6f",
                      PercentileFromSorted(sorted, 10.0), PercentileFromSorted(sorted, 50.0),
                      PercentileFromSorted(sorted, 90.0), PercentileFromSorted(sorted, 99.0));
        logger.Log(line);
    }
    if (!sessionQuality.empty()) {
        std::vector<double> sorted = sessionQuality;
        std::sort(sorted.begin(), sorted.end());
        char line[256];
        std::snprintf(line, sizeof(line),
                      "session_quality_score: p10=%.6f p50=%.6f p90=%.6f p99=%.6f",
                      PercentileFromSorted(sorted, 10.0), PercentileFromSorted(sorted, 50.0),
                      PercentileFromSorted(sorted, 90.0), PercentileFromSorted(sorted, 99.0));
        logger.Log(line);
    }

    WriteValues(outRoughnessPath, roughness);
    WriteValues(outSessionQualityPath, sessionQuality);

    return 0;
}
