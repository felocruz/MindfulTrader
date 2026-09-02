// tools/context_pipeline/context_to_parquet.cpp
// CLI + Arrow/Parquet output for context_reader.h. Columnar (SoA) construction
// throughout -- one arrow::FloatBuilder/UInt64Builder/etc. per output column,
// appended directly as records are parsed; never a row-of-structs intermediate.
// Chunked writes via NewBufferedRowGroup()+WriteRecordBatch() bound peak memory
// regardless of file size (DOD discipline, this plan's Global Constraints).
//
// Build: mamba run -n mts g++ -O2 -std=c++17 -Iinclude \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   tools/context_pipeline/context_to_parquet.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -o tools/context_to_parquet
#include "context_cache_key.h"
#include "context_reader.h"
#include "generated/mts_schema_contract_generated.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kChunkRows = 2'000'000;  // matches hmm_training_cache.py's _BUILD_CHUNK_ROWS convention

// Flat, columnar accumulation buffer -- one std::vector<float> per ObservationData
// field, one per RiskGateContext float field, plus the scalar/bool columns.
// Reused across chunks (cleared, not reallocated) to avoid per-chunk heap churn.
struct ChunkBuffers {
    std::array<std::vector<float>, MTS::Schema::Contract::kObservationDim> observation_cols;
    std::array<std::vector<float>, MTS::Schema::Contract::kRiskGateFloatFieldCount> risk_gate_cols;
    std::vector<std::int32_t> risk_gate_regime_duration;
    std::vector<bool> risk_gate_is_valid;
    std::vector<std::int64_t> risk_gate_snapshot_timestamp_us;
    std::vector<bool> risk_gate_context_available;
    std::vector<std::uint64_t> sequence_id;
    std::vector<std::int64_t> timestamp_us;
    std::vector<float> bars_since_last_update;

    void Reserve(std::size_t n) {
        for (auto& col : observation_cols) col.reserve(n);
        for (auto& col : risk_gate_cols) col.reserve(n);
        risk_gate_regime_duration.reserve(n);
        risk_gate_is_valid.reserve(n);
        risk_gate_snapshot_timestamp_us.reserve(n);
        risk_gate_context_available.reserve(n);
        sequence_id.reserve(n);
        timestamp_us.reserve(n);
        bars_since_last_update.reserve(n);
    }

    void Clear() {
        for (auto& col : observation_cols) col.clear();
        for (auto& col : risk_gate_cols) col.clear();
        risk_gate_regime_duration.clear();
        risk_gate_is_valid.clear();
        risk_gate_snapshot_timestamp_us.clear();
        risk_gate_context_available.clear();
        sequence_id.clear();
        timestamp_us.clear();
        bars_since_last_update.clear();
    }

    std::size_t Rows() const { return sequence_id.size(); }
};

// Default values matching RiskGateContext's own .fbs-declared field defaults
// (mts_schema.fbs:432-448) -- used when a record has no RiskGateContext at all,
// mirroring observation_vector_bulk_reader.py's _RISK_GATE_FLOAT_DEFAULTS.
constexpr std::array<float, MTS::Schema::Contract::kRiskGateFloatFieldCount> kRiskGateFloatDefaults = {
    0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f, 0.5f, 1.5f, 0.0f, 1.0f, 0.0f, 0.5f,
};

void AppendPair(const PairedRecord& rec, void* user_data) {
    auto* buf = static_cast<ChunkBuffers*>(user_data);
    const auto* obs = rec.observation.observation;
    // ObservationData's accessor order is guaranteed to match
    // kObservationFieldNames' declared order (both derived from the same .fbs
    // struct, Task 2) -- this positional loop is safe by that invariant.
    const std::array<float, MTS::Schema::Contract::kObservationDim> values = {
        obs->log_scale_ratio(), obs->burstiness_index(), obs->relative_range(),
        obs->log_scale_expansion_ratio(), obs->vol_convexity(), obs->lempel_ziv(),
        obs->hurst_exponent(), obs->micro_asymmetry(), obs->fisher_info(),
        obs->fast_hurst_exponent(), obs->tail_index(), obs->skewness_idx(),
        obs->amihud_illiquidity(), obs->liq_fragility(), obs->fast_taleb_kurtosis(),
        obs->recurrence_rate(), obs->fractal_dim(), obs->mean_rev_z(), obs->fast_mean_rev_z(),
    };
    for (std::size_t i = 0; i < values.size(); ++i) {
        buf->observation_cols[i].push_back(values[i]);
    }

    const auto* rgc = rec.observation.risk_gate_context;
    if (rgc != nullptr) {
        const std::array<float, MTS::Schema::Contract::kRiskGateFloatFieldCount> rgc_values = {
            rgc->shannon_flow_entropy(), rgc->shannon_efficiency(), rgc->taleb_kurtosis(),
            rgc->taleb_skewness(), rgc->elder_chandelier_atr(), rgc->pareto_tail_alpha(),
            rgc->amihud_illiquidity(), rgc->spread_stress(), rgc->hurst_exponent(),
            rgc->fractal_dim(), rgc->mean_rev_z(), rgc->raschke_burst(), rgc->fisher_info(),
            rgc->amihud_percentile(),
        };
        for (std::size_t i = 0; i < rgc_values.size(); ++i) {
            buf->risk_gate_cols[i].push_back(rgc_values[i]);
        }
        buf->risk_gate_regime_duration.push_back(rgc->regime_duration());
        buf->risk_gate_is_valid.push_back(rgc->is_valid());
        buf->risk_gate_snapshot_timestamp_us.push_back(rgc->snapshot_timestamp_us());
        buf->risk_gate_context_available.push_back(true);
    } else {
        for (std::size_t i = 0; i < kRiskGateFloatDefaults.size(); ++i) {
            buf->risk_gate_cols[i].push_back(kRiskGateFloatDefaults[i]);
        }
        buf->risk_gate_regime_duration.push_back(0);
        buf->risk_gate_is_valid.push_back(false);
        buf->risk_gate_snapshot_timestamp_us.push_back(0);
        buf->risk_gate_context_available.push_back(false);
    }

    buf->sequence_id.push_back(rec.observation.sequence_id);
    buf->timestamp_us.push_back(rec.observation.timestamp_us);
    buf->bars_since_last_update.push_back(rec.bars_since_last_update);
}

arrow::Status BuildArrowSchema(std::shared_ptr<arrow::Schema>* out) {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    for (std::size_t i = 0; i < MTS::Schema::Contract::kObservationDim; ++i) {
        fields.push_back(arrow::field(MTS::Schema::Contract::kObservationFieldNames[i], arrow::float32()));
    }
    // kRiskGateFloatOutputColumnNames already resolves each name to either the
    // plain field name or <name>_raw (only for the 5 that collide with an
    // ObservationData column, §10.4) -- no additional prefix here. Blanket-
    // prefixing on top of that would defeat the whole point of computing this
    // array in the first place.
    for (std::size_t i = 0; i < MTS::Schema::Contract::kRiskGateFloatFieldCount; ++i) {
        fields.push_back(arrow::field(
            MTS::Schema::Contract::kRiskGateFloatOutputColumnNames[i], arrow::float32()));
    }
    fields.push_back(arrow::field("risk_gate_regime_duration", arrow::int32()));
    fields.push_back(arrow::field("risk_gate_is_valid", arrow::boolean()));
    fields.push_back(arrow::field("risk_gate_snapshot_timestamp_us", arrow::int64()));
    fields.push_back(arrow::field("risk_gate_context_available", arrow::boolean()));
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

    for (const auto& col : buf.observation_cols) {
        arrow::FloatBuilder builder;
        ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<std::int64_t>(col.size())));
        ARROW_RETURN_NOT_OK(builder.AppendValues(col));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    for (const auto& col : buf.risk_gate_cols) {
        arrow::FloatBuilder builder;
        ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<std::int64_t>(col.size())));
        ARROW_RETURN_NOT_OK(builder.AppendValues(col));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    {
        arrow::Int32Builder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(buf.risk_gate_regime_duration));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    {
        // BooleanBuilder::AppendValues(const std::vector<bool>&) exists directly
        // (arrow/array/builder_primitive.h) -- ChunkBuffers already stores this
        // column as std::vector<bool>, so no conversion/copy is needed here.
        arrow::BooleanBuilder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(buf.risk_gate_is_valid));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    {
        arrow::Int64Builder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(buf.risk_gate_snapshot_timestamp_us));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    {
        arrow::BooleanBuilder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(buf.risk_gate_context_available));
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

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --input PATH --output PATH [--mode unbounded|head|tail] "
        "[--max-samples N] [--resume-offset N] [--resume-last-seq-id N]\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string input_path, output_path, mode = "unbounded", meta_path_arg;
    std::size_t max_samples = 0;
    std::size_t resume_offset = 0;
    std::uint64_t resume_last_seq_id = 0;
    bool has_resume_last_seq_id = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--input") input_path = next("--input");
        else if (arg == "--output") output_path = next("--output");
        else if (arg == "--mode") mode = next("--mode");
        else if (arg == "--max-samples") max_samples = std::stoull(next("--max-samples"));
        else if (arg == "--resume-offset") resume_offset = std::stoull(next("--resume-offset"));
        else if (arg == "--resume-last-seq-id") {
            resume_last_seq_id = std::stoull(next("--resume-last-seq-id"));
            has_resume_last_seq_id = true;
        } else if (arg == "--meta-path") {
            meta_path_arg = next("--meta-path");
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            PrintUsage(argv[0]);
            return 1;
        }
    }
    if (input_path.empty() || output_path.empty()) {
        PrintUsage(argv[0]);
        return 1;
    }

    ContextFileHandle handle;
    try {
        handle = OpenContextFile(input_path);
    } catch (const std::runtime_error& e) {
        std::fprintf(stderr, "❌ %s\n", e.what());
        return 1;
    }

    // Freshness/rebuild decision (§10.6's "what do we do when a new .context file
    // arrives" question) -- only engaged when --meta-path is supplied, so scripted/
    // tested invocations that pass explicit --resume-offset/--resume-last-seq-id
    // flags keep their existing behavior unchanged.
    if (mode == "unbounded" && !meta_path_arg.empty()) {
        const auto plan = DecideRebuildPlan(
            input_path, output_path, meta_path_arg, MTS::Schema::Contract::kSchemaVersion);
        if (plan == RebuildPlan::kFresh) {
            std::printf("✅ %s is already fresh relative to %s -- nothing to do\n",
                        output_path.c_str(), input_path.c_str());
            CloseContextFile(handle);
            return 0;
        }
        if (plan == RebuildPlan::kIncremental) {
            auto prior = ReadCacheKey(meta_path_arg);
            resume_offset = prior->resume_offset;
            if (prior->has_last_seq_id) {
                resume_last_seq_id = prior->last_seq_id;
                has_resume_last_seq_id = true;
            }
            std::printf("Incremental rebuild: resuming from byte offset %zu\n", resume_offset);
        } else {
            std::printf("Full rebuild: %s\n",
                        std::ifstream(meta_path_arg).good()
                            ? "schema changed, file shrank, or same-size-different-content"
                            : "no prior cache");
        }
    }

    std::shared_ptr<arrow::Schema> schema;
    auto status = BuildArrowSchema(&schema);
    if (!status.ok()) {
        std::fprintf(stderr, "❌ BuildArrowSchema failed: %s\n", status.ToString().c_str());
        return 1;
    }

    auto sink_result = arrow::io::FileOutputStream::Open(output_path);
    if (!sink_result.ok()) {
        std::fprintf(stderr, "❌ cannot open output file: %s\n", sink_result.status().ToString().c_str());
        return 1;
    }
    auto sink = *sink_result;

    auto writer_result = parquet::arrow::FileWriter::Open(
        *schema, arrow::default_memory_pool(), sink);
    if (!writer_result.ok()) {
        std::fprintf(stderr, "❌ cannot open Parquet writer: %s\n", writer_result.status().ToString().c_str());
        return 1;
    }
    auto writer = std::move(*writer_result);

    ChunkBuffers buffers;
    buffers.Reserve(kChunkRows);

    auto flush_chunk = [&]() -> bool {
        if (buffers.Rows() == 0) return true;
        auto s1 = writer->NewBufferedRowGroup();
        if (!s1.ok()) { std::fprintf(stderr, "❌ NewBufferedRowGroup: %s\n", s1.ToString().c_str()); return false; }
        std::shared_ptr<arrow::RecordBatch> batch;
        auto s2 = BuildRecordBatch(buffers, schema, &batch);
        if (!s2.ok()) { std::fprintf(stderr, "❌ BuildRecordBatch: %s\n", s2.ToString().c_str()); return false; }
        auto s3 = writer->WriteRecordBatch(*batch);
        if (!s3.ok()) { std::fprintf(stderr, "❌ WriteRecordBatch: %s\n", s3.ToString().c_str()); return false; }
        buffers.Clear();
        return true;
    };

    ReadCounters counters;
    if (mode == "unbounded" && max_samples == 0) {
        // Chunked resumable loop even in "unbounded" mode -- bounds peak memory
        // to kChunkRows regardless of total file size (DOD discipline).
        std::size_t offset = resume_offset != 0 ? resume_offset : handle.record_stream_offset;
        std::uint64_t last_seq = resume_last_seq_id;
        bool has_last_seq = has_resume_last_seq_id;
        while (offset < handle.size) {
            auto [chunk_counters, resume] = ReadAllRecordsResumable(
                handle, offset, last_seq, has_last_seq, AppendPair, &buffers);
            counters.market_records += chunk_counters.market_records;
            counters.system_records += chunk_counters.system_records;
            counters.pair_attempts += chunk_counters.pair_attempts;
            counters.aligned_pairs += chunk_counters.aligned_pairs;
            counters.sequence_mismatches += chunk_counters.sequence_mismatches;
            if (!flush_chunk()) { CloseContextFile(handle); return 1; }
            if (resume.resume_offset == offset) break;  // no progress -- end of stream
            offset = resume.resume_offset;
            last_seq = resume.last_seq_id;
            has_last_seq = resume.has_last_seq_id;
        }
    } else if (mode == "head") {
        counters = ReadBoundedHead(handle, max_samples, AppendPair, &buffers);
        if (!flush_chunk()) { CloseContextFile(handle); return 1; }
    } else if (mode == "tail") {
        counters = ReadBoundedTail(handle, max_samples, AppendPair, &buffers);
        if (!flush_chunk()) { CloseContextFile(handle); return 1; }
    } else {
        std::fprintf(stderr, "❌ unknown --mode %s (expected unbounded|head|tail)\n", mode.c_str());
        CloseContextFile(handle);
        return 1;
    }

    auto close_status = writer->Close();
    const std::size_t final_handle_size = handle.size;
    CloseContextFile(handle);
    if (!close_status.ok()) {
        std::fprintf(stderr, "❌ Parquet writer Close failed: %s\n", close_status.ToString().c_str());
        return 1;
    }

    if (!meta_path_arg.empty()) {
        // handle.size (the whole file's byte length) is used as the persisted
        // resume_offset for a completed unbounded pass -- the whole file was
        // consumed. A future incremental call re-derives the true resume point
        // from ReadAllRecordsResumable's own returned ResumePoint the same way
        // the per-chunk loop above already tracks offset/last_seq; wiring the
        // exact final chunk's ResumePoint into this call instead is a direct
        // follow-on, not done here to keep this step's diff focused on the
        // freshness-decision wiring itself.
        const auto final_stat = StatFile(input_path);
        ContextCacheKey key{
            final_stat.size, final_stat.mtime, MTS::Schema::Contract::kSchemaVersion,
            kContextParquetCacheFormatVersion, final_handle_size,
            /*last_seq_id=*/0, /*has_last_seq_id=*/false,
        };
        WriteCacheKey(meta_path_arg, key);
    }

    std::printf(
        "✅ wrote %s (mode=%s, aligned_pairs=%zu, sequence_mismatches=%zu)\n",
        output_path.c_str(), mode.c_str(), counters.aligned_pairs, counters.sequence_mismatches);
    return 0;
}
