// MarketDataReplay.cpp -- CLI driver for the dim-selection experimentation
// pipeline (docs/superpowers/specs/2026-09-09-market-data-replay-dim-
// selection-spec.md). Streams the full multi-contract mes_ticks.parquet
// through MarketDataReplayEngine and writes a flat Parquet file (candidate
// observation dims + sequence_id/timestamp_us/bars_since_last_update) direct
// from C++ -- no .context intermediate, per the dim-selection spec §3.
//
// Usage:
//   tools/bin/market_data_replay \
//     --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet \
//     --output <path>.parquet \
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
#include "CandidateObservationDims.h"
#include "../observation_vector/market_data_io.h"
#include "../ToolProgressLogger.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kChunkRows = 2'000'000;  // matches context_to_parquet.cpp's own convention

// Flat, columnar accumulation buffer -- one std::vector<float> per
// ObservationData dim (ALWAYS all 18, schema order, regardless of the
// current candidate/IN gate set -- operator directive, 2026-09-16: a dim
// marked OUT still gets its own column, just hard-zeroed by the engine, so
// lbrnet's ignore-list -- not a shrinking column count -- is what excludes
// it), plus sequence_id/timestamp_us/bars_since_last_update. Reused across
// chunks (cleared, not reallocated) to avoid per-chunk heap churn (DOD).
struct ChunkBuffers {
    std::array<std::vector<float>, MTS::Schema::Contract::kObservationDim> obs_cols;
    std::vector<std::uint64_t> sequence_id;
    std::vector<std::int64_t> timestamp_us;
    std::vector<float> bars_since_last_update;

    void Reserve(std::size_t n) {
        for (auto& col : obs_cols) col.reserve(n);
        sequence_id.reserve(n);
        timestamp_us.reserve(n);
        bars_since_last_update.reserve(n);
    }
    void Clear() {
        for (auto& col : obs_cols) col.clear();
        sequence_id.clear();
        timestamp_us.clear();
        bars_since_last_update.clear();
    }
    std::size_t Rows() const { return sequence_id.size(); }
};

arrow::Status BuildArrowSchema(std::shared_ptr<arrow::Schema>* out) {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    for (std::size_t dim = 0; dim < MTS::Schema::Contract::kObservationDim; ++dim) {
        fields.push_back(arrow::field(
            MTS::Schema::Contract::kObservationFieldNames[dim], arrow::float32()));
    }
    fields.push_back(arrow::field("sequence_id", arrow::uint64()));
    fields.push_back(arrow::field("timestamp_us", arrow::int64()));
    fields.push_back(arrow::field("bars_since_last_update", arrow::float32()));
    *out = arrow::schema(fields);
    return arrow::Status::OK();
}

arrow::Status BuildRecordBatch(
    const ChunkBuffers& buf, const std::shared_ptr<arrow::Schema>& schema,
    std::shared_ptr<arrow::RecordBatch>* out) {
    std::vector<std::shared_ptr<arrow::Array>> columns;
    for (const auto& col : buf.obs_cols) {
        arrow::FloatBuilder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(col));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    {
        arrow::UInt64Builder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(buf.sequence_id));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    {
        arrow::Int64Builder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(buf.timestamp_us));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    {
        arrow::FloatBuilder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(buf.bars_since_last_update));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    *out = arrow::RecordBatch::Make(schema, static_cast<std::int64_t>(buf.Rows()), columns);
    return arrow::Status::OK();
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticksPath;
    std::string outputPath;
    std::size_t maxRssMB = 3072;
    std::size_t maxTicks = 0;  // 0 = unlimited; bounds runs for smoke-testing
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks-parquet") == 0 && i + 1 < argc) {
            ticksPath = argv[++i];
        } else if (std::strcmp(argv[i], "--output") == 0 && i + 1 < argc) {
            outputPath = argv[++i];
        } else if (std::strcmp(argv[i], "--max-rss-mb") == 0 && i + 1 < argc) {
            maxRssMB = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--max-ticks") == 0 && i + 1 < argc) {
            maxTicks = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        }
    }
    if (ticksPath.empty() || outputPath.empty()) {
        std::fprintf(stderr,
                      "usage: %s --ticks-parquet PATH --output PATH.parquet [--max-rss-mb 3072] "
                      "[--max-ticks N]\n",
                      argv[0]);
        return 1;
    }

    ToolProgressLogger progress("market_data_replay");
    progress.Log("streaming from " + ticksPath + " -> " + outputPath +
                  " (max-rss-mb=" + std::to_string(maxRssMB) + ", " +
                  std::to_string(MTS::Schema::Contract::kObservationDim) + " dims written (fixed), " +
                  std::to_string(mdr::kCandidateDimCount) + " currently IN the Mahalanobis gate, " +
                  "direct-to-Parquet)");

    std::shared_ptr<arrow::Schema> schema;
    auto schemaStatus = BuildArrowSchema(&schema);
    if (!schemaStatus.ok()) {
        progress.Log("FATAL: BuildArrowSchema failed: " + schemaStatus.ToString());
        return 1;
    }
    auto sinkResult = arrow::io::FileOutputStream::Open(outputPath);
    if (!sinkResult.ok()) {
        progress.Log("FATAL: cannot open output file: " + sinkResult.status().ToString());
        return 1;
    }
    auto writerResult = parquet::arrow::FileWriter::Open(*schema, arrow::default_memory_pool(), *sinkResult);
    if (!writerResult.ok()) {
        progress.Log("FATAL: cannot open Parquet writer: " + writerResult.status().ToString());
        return 1;
    }
    auto writer = std::move(*writerResult);

    ChunkBuffers buffers;
    buffers.Reserve(kChunkRows);

    auto flushChunk = [&]() -> bool {
        if (buffers.Rows() == 0) return true;
        auto s1 = writer->NewBufferedRowGroup();
        if (!s1.ok()) { progress.Log("FATAL: NewBufferedRowGroup: " + s1.ToString()); return false; }
        std::shared_ptr<arrow::RecordBatch> batch;
        auto s2 = BuildRecordBatch(buffers, schema, &batch);
        if (!s2.ok()) { progress.Log("FATAL: BuildRecordBatch: " + s2.ToString()); return false; }
        auto s3 = writer->WriteRecordBatch(*batch);
        if (!s3.ok()) { progress.Log("FATAL: WriteRecordBatch: " + s3.ToString()); return false; }
        buffers.Clear();
        return true;
    };

    MarketDataReplayEngine engine;

    constexpr std::size_t kProgressEveryNTicks = 5'000'000;
    constexpr std::size_t kInterimReportEveryNTicks = 50'000'000;
    std::size_t ticksProcessed = 0;
    std::size_t recordsWritten = 0;
    std::uint64_t nextSequenceId = 0;
    bool sawFirstWarmup = false;
    bool fatalError = false;

    try {
        StreamTicksFullParquet(
            ticksPath,
            [&](std::int64_t ts, double price, std::int64_t volume, std::int64_t askVol,
                std::int64_t bidVol, bool isNewContract) {
                if (fatalError) return;
                if (maxTicks != 0 && ticksProcessed >= maxTicks) return;
                ++ticksProcessed;

                // Faithful-to-live-trading directive (spec §3a open Q1): a real
                // continuous chart never resets its indicators at a contract
                // roll -- do NOT reset engine state here.
                if (isNewContract) {
                    progress.Log("contract roll at tick " + std::to_string(ticksProcessed) +
                                 " (timestamp_us=" + std::to_string(ts) + ") -- engine state NOT reset");
                }

                if (price <= 0.0) return;  // skip degenerate ticks, don't corrupt windows

                const bool wasWarmedUp = sawFirstWarmup;
                const bool significant = engine.OnTick(ts, price, volume, askVol, bidVol);

                if (significant) {
                    const auto& obs = engine.GetObservation();
                    const auto rawObs = MTS::Schema::Contract::ToObservationArray(obs);
                    for (std::size_t i = 0; i < MTS::Schema::Contract::kObservationDim; ++i) {
                        buffers.obs_cols[i].push_back(rawObs[i]);
                    }
                    buffers.sequence_id.push_back(nextSequenceId++);
                    buffers.timestamp_us.push_back(ts);
                    buffers.bars_since_last_update.push_back(engine.GetBarsSinceLastUpdate());
                    ++recordsWritten;
                    if (!wasWarmedUp) {
                        sawFirstWarmup = true;
                        progress.Log("warm-up complete, first record written at tick " +
                                     std::to_string(ticksProcessed));
                    }
                    if (buffers.Rows() >= kChunkRows) {
                        if (!flushChunk()) fatalError = true;
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
        flushChunk();
        return 1;
    }

    if (fatalError) return 1;

    engine.Flush();  // drops the final in-progress bar across all 3 timeframes if skipped
    if (!flushChunk()) return 1;

    auto closeStatus = writer->Close();
    if (!closeStatus.ok()) {
        progress.Log("FATAL: writer Close failed: " + closeStatus.ToString());
        return 1;
    }

    progress.Log("=== SUMMARY: ticks processed=" + std::to_string(ticksProcessed) +
                 " records written=" + std::to_string(recordsWritten) +
                 " significant-change rate=" +
                 std::to_string(ticksProcessed > 0
                                     ? static_cast<double>(recordsWritten) / static_cast<double>(ticksProcessed)
                                     : 0.0) +
                 " ===");
    return 0;
}

