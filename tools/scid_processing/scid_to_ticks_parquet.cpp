// tools/scid_processing/scid_to_ticks_parquet.cpp
// CLI tool: syncs the local .scid mirror from the live Sierra Chart directory
// (Task 4), discovers contract windows (Task 2), decodes true per-tick data --
// no aggregation of any kind -- per contract in parallel (Task 1), and writes
// lbrnet/data/raw/mes_ticks.parquet. Per-contract incremental state lives in
// {data-dir}/.parts/{label}.parquet. See
// docs/superpowers/specs/2026-09-02-scid-tick-parquet-cpp-tool-spec.md.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   tools/scid_processing/scid_to_ticks_parquet.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/scid_to_ticks_parquet
#include "scid_contract_windows.h"
#include "scid_mirror_sync.h"
#include "scid_part_action.h"
#include "scid_reader.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/api/reader.h>
#include <parquet/arrow/reader.h>
#include <parquet/arrow/writer.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

constexpr std::size_t kChunkRows = 2'000'000;  // matches context_to_parquet.cpp's convention

// Serializes stdout progress lines across the parallel decode threads --
// std::printf itself isn't guaranteed atomic across threads, and multi-GB
// per-contract decodes can otherwise run silently for minutes with zero
// visible output.
std::mutex g_print_mutex;
void LogProgress(const std::string& line) {
    std::lock_guard<std::mutex> lock(g_print_mutex);
    std::printf("%s\n", line.c_str());
    std::fflush(stdout);
}

// Columnar accumulation buffer matching the output schema exactly -- reused
// across chunks (Reserve once, Clear between chunks), never a fresh
// heap-allocated buffer per chunk (context_to_parquet.cpp's ChunkBuffers pattern).
struct TickChunkBuilders {
    std::vector<std::int64_t> timestamp_us;
    std::vector<double> trade_price, ask_price, bid_price, spread;
    std::vector<std::int64_t> volume, bid_volume, ask_volume, trade_side;
    std::vector<std::string> contract;

    void Reserve(std::size_t n) {
        timestamp_us.reserve(n);
        trade_price.reserve(n);
        ask_price.reserve(n);
        bid_price.reserve(n);
        spread.reserve(n);
        volume.reserve(n);
        bid_volume.reserve(n);
        ask_volume.reserve(n);
        trade_side.reserve(n);
        contract.reserve(n);
    }
    void Clear() {
        timestamp_us.clear();
        trade_price.clear();
        ask_price.clear();
        bid_price.clear();
        spread.clear();
        volume.clear();
        bid_volume.clear();
        ask_volume.clear();
        trade_side.clear();
        contract.clear();
    }
    std::size_t Rows() const { return timestamp_us.size(); }
};

std::shared_ptr<arrow::Schema> BuildTickSchema() {
    // Column order is load-bearing: index 0 (timestamp_us) is relied on by
    // ReadPartSummary()'s row-group statistics lookup below.
    return arrow::schema({
        arrow::field("timestamp_us", arrow::int64()),
        arrow::field("trade_price", arrow::float64()),
        arrow::field("ask_price", arrow::float64()),
        arrow::field("bid_price", arrow::float64()),
        arrow::field("spread", arrow::float64()),
        arrow::field("volume", arrow::int64()),
        arrow::field("bid_volume", arrow::int64()),
        arrow::field("ask_volume", arrow::int64()),
        arrow::field("trade_side", arrow::int64()),
        arrow::field("contract", arrow::utf8()),
    });
}

arrow::Status BuildRecordBatch(const TickChunkBuilders& buf, const std::shared_ptr<arrow::Schema>& schema,
                                std::shared_ptr<arrow::RecordBatch>* out) {
    std::vector<std::shared_ptr<arrow::Array>> columns;
    auto append_i64 = [&](const std::vector<std::int64_t>& col) -> arrow::Status {
        arrow::Int64Builder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(col));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
        return arrow::Status::OK();
    };
    auto append_f64 = [&](const std::vector<double>& col) -> arrow::Status {
        arrow::DoubleBuilder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(col));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
        return arrow::Status::OK();
    };
    ARROW_RETURN_NOT_OK(append_i64(buf.timestamp_us));
    ARROW_RETURN_NOT_OK(append_f64(buf.trade_price));
    ARROW_RETURN_NOT_OK(append_f64(buf.ask_price));
    ARROW_RETURN_NOT_OK(append_f64(buf.bid_price));
    ARROW_RETURN_NOT_OK(append_f64(buf.spread));
    ARROW_RETURN_NOT_OK(append_i64(buf.volume));
    ARROW_RETURN_NOT_OK(append_i64(buf.bid_volume));
    ARROW_RETURN_NOT_OK(append_i64(buf.ask_volume));
    ARROW_RETURN_NOT_OK(append_i64(buf.trade_side));
    {
        arrow::StringBuilder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(buf.contract));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    *out = arrow::RecordBatch::Make(schema, static_cast<std::int64_t>(buf.Rows()), columns);
    return arrow::Status::OK();
}

// Wraps a parquet::arrow::FileWriter with this tool's fixed writer properties
// (zstd level 1 -- this is a repeatedly-regenerated incremental cache, not a
// cold-archival file, so write throughput matters more than ratio; dictionary
// encoding for the low-cardinality `contract` column) and the chunked
// NewBufferedRowGroup()+WriteRecordBatch() write pattern.
class PartWriter {
public:
    static std::unique_ptr<PartWriter> Open(const std::string& path,
                                             const std::shared_ptr<arrow::Schema>& schema) {
        auto writer_props = parquet::WriterProperties::Builder()
                                 .compression(parquet::Compression::ZSTD)
                                 ->compression_level(1)
                                 ->enable_dictionary("contract")
                                 ->build();
        auto arrow_props = parquet::default_arrow_writer_properties();
        auto sink_result = arrow::io::FileOutputStream::Open(path);
        if (!sink_result.ok()) {
            std::fprintf(stderr, "cannot open output %s: %s\n", path.c_str(),
                         sink_result.status().ToString().c_str());
            return nullptr;
        }
        auto writer_result = parquet::arrow::FileWriter::Open(
            *schema, arrow::default_memory_pool(), *sink_result, writer_props, arrow_props);
        if (!writer_result.ok()) {
            std::fprintf(stderr, "cannot open parquet writer for %s: %s\n", path.c_str(),
                         writer_result.status().ToString().c_str());
            return nullptr;
        }
        auto pw = std::make_unique<PartWriter>();
        pw->writer_ = std::move(*writer_result);
        pw->schema_ = schema;
        return pw;
    }

    bool WriteBatch(const std::shared_ptr<arrow::RecordBatch>& batch) {
        if (batch->num_rows() == 0) return true;
        auto s1 = writer_->NewBufferedRowGroup();
        if (!s1.ok()) { std::fprintf(stderr, "NewBufferedRowGroup: %s\n", s1.ToString().c_str()); return false; }
        auto s2 = writer_->WriteRecordBatch(*batch);
        if (!s2.ok()) { std::fprintf(stderr, "WriteRecordBatch: %s\n", s2.ToString().c_str()); return false; }
        return true;
    }

    bool FlushBuilders(TickChunkBuilders& buf) {
        if (buf.Rows() == 0) return true;
        std::shared_ptr<arrow::RecordBatch> batch;
        auto status = BuildRecordBatch(buf, schema_, &batch);
        if (!status.ok()) { std::fprintf(stderr, "BuildRecordBatch: %s\n", status.ToString().c_str()); return false; }
        const bool ok = WriteBatch(batch);
        buf.Clear();
        return ok;
    }

    bool Close() {
        auto s = writer_->Close();
        if (!s.ok()) { std::fprintf(stderr, "Close: %s\n", s.ToString().c_str()); return false; }
        return true;
    }

private:
    std::unique_ptr<parquet::arrow::FileWriter> writer_;
    std::shared_ptr<arrow::Schema> schema_;
};

// Appends one decoded chunk's rows into `out`, skipping any row with
// timestamp_us <= min_ts_exclusive (used by resume-append to avoid
// duplicating rows already present in the existing part), flushing to
// `writer` whenever the buffer reaches kChunkRows.
void AppendFilteredChunk(const scid::DecodedTickChunk& chunk, const std::string& contract_label,
                          std::optional<std::int64_t> min_ts_exclusive, TickChunkBuilders& out,
                          PartWriter& writer, bool* ok) {
    for (std::size_t i = 0; i < chunk.Rows(); ++i) {
        if (min_ts_exclusive.has_value() && chunk.timestamp_us[i] <= *min_ts_exclusive) continue;
        out.timestamp_us.push_back(chunk.timestamp_us[i]);
        out.trade_price.push_back(chunk.trade_price[i]);
        out.ask_price.push_back(chunk.ask_price[i]);
        out.bid_price.push_back(chunk.bid_price[i]);
        out.spread.push_back(chunk.spread[i]);
        out.volume.push_back(chunk.volume[i]);
        out.bid_volume.push_back(chunk.bid_volume[i]);
        out.ask_volume.push_back(chunk.ask_volume[i]);
        out.trade_side.push_back(chunk.trade_side[i]);
        out.contract.push_back(contract_label);
        if (out.Rows() >= kChunkRows) {
            if (!writer.FlushBuilders(out)) { *ok = false; return; }
        }
    }
}

struct PartSummary {
    std::int64_t rows = 0;
    std::optional<std::int64_t> ts_min;
    std::optional<std::int64_t> ts_max;
};

// Reads row count and min/max timestamp_us from an existing part's own
// row-group statistics -- never re-reads the actual tick data. Valid because
// every part is written in strictly chronological row-group order (matches
// this tool's own write pattern): row group 0's min is the file's global min,
// the last row group's max is the file's global max.
std::optional<PartSummary> ReadPartSummary(const std::string& path) {
    if (!std::filesystem::exists(path)) return std::nullopt;
    try {
        auto file_reader = parquet::ParquetFileReader::OpenFile(path, false);
        auto metadata = file_reader->metadata();
        const int n_rg = metadata->num_row_groups();
        if (n_rg == 0) return std::nullopt;
        PartSummary summary;
        for (int i = 0; i < n_rg; ++i) {
            summary.rows += metadata->RowGroup(i)->num_rows();
        }
        auto first_stats = metadata->RowGroup(0)->ColumnChunk(0)->statistics();
        if (first_stats && first_stats->HasMinMax()) {
            summary.ts_min = std::static_pointer_cast<parquet::Int64Statistics>(first_stats)->min();
        }
        auto last_stats = metadata->RowGroup(n_rg - 1)->ColumnChunk(0)->statistics();
        if (last_stats && last_stats->HasMinMax()) {
            summary.ts_max = std::static_pointer_cast<parquet::Int64Statistics>(last_stats)->max();
        }
        return summary;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "ReadPartSummary(%s): %s\n", path.c_str(), e.what());
        return std::nullopt;
    }
}

// Streams every row batch of an existing part file directly into `writer`,
// unmodified -- bounded peak memory (one batch at a time) regardless of the
// part's total size.
bool StreamExistingPartInto(const std::string& path, PartWriter& writer) {
    auto infile_result = arrow::io::ReadableFile::Open(path);
    if (!infile_result.ok()) {
        std::fprintf(stderr, "open %s: %s\n", path.c_str(), infile_result.status().ToString().c_str());
        return false;
    }
    parquet::arrow::FileReaderBuilder builder;
    auto open_status = builder.Open(*infile_result);
    if (!open_status.ok()) {
        std::fprintf(stderr, "FileReaderBuilder::Open %s: %s\n", path.c_str(), open_status.ToString().c_str());
        return false;
    }
    std::unique_ptr<parquet::arrow::FileReader> reader;
    auto build_status = builder.Build(&reader);
    if (!build_status.ok()) {
        std::fprintf(stderr, "FileReaderBuilder::Build %s: %s\n", path.c_str(), build_status.ToString().c_str());
        return false;
    }
    auto batch_reader_result = reader->GetRecordBatchReader();
    if (!batch_reader_result.ok()) {
        std::fprintf(stderr, "GetRecordBatchReader %s: %s\n", path.c_str(),
                     batch_reader_result.status().ToString().c_str());
        return false;
    }
    auto batch_reader = std::move(*batch_reader_result);
    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        auto next_status = batch_reader->ReadNext(&batch);
        if (!next_status.ok()) {
            std::fprintf(stderr, "ReadNext %s: %s\n", path.c_str(), next_status.ToString().c_str());
            return false;
        }
        if (batch == nullptr) break;
        if (!writer.WriteBatch(batch)) return false;
    }
    return true;
}

// Like StreamExistingPartInto, but drops any row with timestamp_us >=
// ts_cutoff_exclusive (rollover trim: the sibling contract that used to be
// open-ended has since closed, so any of its rows at/after the new boundary
// belong to the next contract, not this one).
bool StreamExistingPartFiltered(const std::string& path, std::int64_t ts_cutoff_exclusive, PartWriter& writer) {
    auto infile_result = arrow::io::ReadableFile::Open(path);
    if (!infile_result.ok()) {
        std::fprintf(stderr, "open %s: %s\n", path.c_str(), infile_result.status().ToString().c_str());
        return false;
    }
    parquet::arrow::FileReaderBuilder builder;
    auto open_status = builder.Open(*infile_result);
    if (!open_status.ok()) {
        std::fprintf(stderr, "FileReaderBuilder::Open %s: %s\n", path.c_str(), open_status.ToString().c_str());
        return false;
    }
    std::unique_ptr<parquet::arrow::FileReader> reader;
    auto build_status = builder.Build(&reader);
    if (!build_status.ok()) {
        std::fprintf(stderr, "FileReaderBuilder::Build %s: %s\n", path.c_str(), build_status.ToString().c_str());
        return false;
    }
    auto batch_reader_result = reader->GetRecordBatchReader();
    if (!batch_reader_result.ok()) {
        std::fprintf(stderr, "GetRecordBatchReader %s: %s\n", path.c_str(),
                     batch_reader_result.status().ToString().c_str());
        return false;
    }
    auto batch_reader = std::move(*batch_reader_result);
    TickChunkBuilders buf;
    buf.Reserve(kChunkRows);
    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        auto next_status = batch_reader->ReadNext(&batch);
        if (!next_status.ok()) {
            std::fprintf(stderr, "ReadNext %s: %s\n", path.c_str(), next_status.ToString().c_str());
            return false;
        }
        if (batch == nullptr) break;
        auto ts = std::static_pointer_cast<arrow::Int64Array>(batch->column(0));
        auto trade_price = std::static_pointer_cast<arrow::DoubleArray>(batch->column(1));
        auto ask_price = std::static_pointer_cast<arrow::DoubleArray>(batch->column(2));
        auto bid_price = std::static_pointer_cast<arrow::DoubleArray>(batch->column(3));
        auto spread = std::static_pointer_cast<arrow::DoubleArray>(batch->column(4));
        auto volume = std::static_pointer_cast<arrow::Int64Array>(batch->column(5));
        auto bid_volume = std::static_pointer_cast<arrow::Int64Array>(batch->column(6));
        auto ask_volume = std::static_pointer_cast<arrow::Int64Array>(batch->column(7));
        auto trade_side = std::static_pointer_cast<arrow::Int64Array>(batch->column(8));
        auto contract = std::static_pointer_cast<arrow::StringArray>(batch->column(9));
        for (std::int64_t i = 0; i < batch->num_rows(); ++i) {
            if (ts->Value(i) >= ts_cutoff_exclusive) continue;
            buf.timestamp_us.push_back(ts->Value(i));
            buf.trade_price.push_back(trade_price->Value(i));
            buf.ask_price.push_back(ask_price->Value(i));
            buf.bid_price.push_back(bid_price->Value(i));
            buf.spread.push_back(spread->Value(i));
            buf.volume.push_back(volume->Value(i));
            buf.bid_volume.push_back(bid_volume->Value(i));
            buf.ask_volume.push_back(ask_volume->Value(i));
            buf.trade_side.push_back(trade_side->Value(i));
            buf.contract.emplace_back(contract->GetView(i));
            if (buf.Rows() >= kChunkRows) {
                if (!writer.FlushBuilders(buf)) return false;
            }
        }
    }
    return writer.FlushBuilders(buf);
}

struct BuildResult {
    std::string contract_label;
    bool ok = true;
};

BuildResult FullDecodeContract(const scid::ContractWindow& window, const std::string& part_path) {
    BuildResult result;
    result.contract_label = scid::ContractLabel(window.month_code, window.year);
    const std::string tmp_path = part_path + ".tmp";
    auto schema = BuildTickSchema();
    auto writer = PartWriter::Open(tmp_path, schema);
    if (!writer) { result.ok = false; return result; }

    bool ok = true;
    try {
        scid::ScidFileView file(window.path);
        scid::DecodedTickChunk chunk;
        chunk.Reserve(kChunkRows);
        TickChunkBuilders builders;
        builders.Reserve(kChunkRows);
        scid::DecodeScidTicks(file, window.active_start_us, window.active_end_us, kChunkRows, chunk,
                               [&](const scid::DecodedTickChunk& c) {
                                   if (!ok) return;
                                   AppendFilteredChunk(c, result.contract_label, std::nullopt, builders,
                                                        *writer, &ok);
                               });
        if (ok && !writer->FlushBuilders(builders)) ok = false;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "FullDecodeContract(%s): %s\n", result.contract_label.c_str(), e.what());
        ok = false;
    }
    if (!writer->Close()) ok = false;
    if (ok) std::filesystem::rename(tmp_path, part_path);
    result.ok = ok;
    return result;
}

BuildResult ResumeAppendContract(const scid::ContractWindow& window, const std::string& part_path) {
    BuildResult result;
    result.contract_label = scid::ContractLabel(window.month_code, window.year);

    auto existing = ReadPartSummary(part_path);
    if (!existing.has_value() || !existing->ts_max.has_value()) {
        // Can't establish a safe resume point from this part's own metadata --
        // force a full decode rather than risk silently missing rows.
        return FullDecodeContract(window, part_path);
    }
    const std::int64_t existing_max = *existing->ts_max;

    const std::string tmp_path = part_path + ".tmp";
    auto schema = BuildTickSchema();
    auto writer = PartWriter::Open(tmp_path, schema);
    if (!writer) { result.ok = false; return result; }

    bool ok = StreamExistingPartInto(part_path, *writer);
    if (ok) {
        try {
            scid::ScidFileView file(window.path);
            scid::DecodedTickChunk chunk;
            chunk.Reserve(kChunkRows);
            TickChunkBuilders builders;
            builders.Reserve(kChunkRows);
            // Start slightly before existing_max+1 to safely re-capture any
            // benign jitter straddling the old/new boundary; AppendFilteredChunk's
            // min_ts_exclusive=existing_max then drops the true duplicates.
            const std::int64_t safe_start =
                std::max(window.active_start_us, existing_max + 1 - scid::kMaxBenignTimestampJitterUs);
            scid::DecodeScidTicks(file, safe_start, window.active_end_us, kChunkRows, chunk,
                                   [&](const scid::DecodedTickChunk& c) {
                                       if (!ok) return;
                                       AppendFilteredChunk(c, result.contract_label, existing_max, builders,
                                                            *writer, &ok);
                                   });
            if (ok && !writer->FlushBuilders(builders)) ok = false;
        } catch (const std::exception& e) {
            std::fprintf(stderr, "ResumeAppendContract(%s): %s\n", result.contract_label.c_str(), e.what());
            ok = false;
        }
    }
    if (!writer->Close()) ok = false;
    if (ok) std::filesystem::rename(tmp_path, part_path);
    result.ok = ok;
    return result;
}

BuildResult TrimContract(const scid::ContractWindow& window, const std::string& part_path) {
    BuildResult result;
    result.contract_label = scid::ContractLabel(window.month_code, window.year);
    if (!window.active_end_us.has_value()) { result.ok = true; return result; }  // defensive, shouldn't happen

    const std::string tmp_path = part_path + ".tmp";
    auto schema = BuildTickSchema();
    auto writer = PartWriter::Open(tmp_path, schema);
    if (!writer) { result.ok = false; return result; }

    bool ok = StreamExistingPartFiltered(part_path, *window.active_end_us, *writer);
    if (!writer->Close()) ok = false;
    if (ok) std::filesystem::rename(tmp_path, part_path);
    result.ok = ok;
    return result;
}

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s [--data-dir DIR] [--data-dir-live DIR] [--output PATH] "
        "[--sync-only] [--no-sync] [--full-rebuild]\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
    // Absolute by default -- matches this codebase's own cross-repo path
    // convention (e.g. WORKSPACE = Path("/home/rcruz/devel/VSCode") in the
    // observation_vector Python recalibration tools). A bare relative default
    // here would resolve against whatever the caller's cwd happens to be
    // (e.g. "MindfulTrader/lbrnet/..." if invoked from MindfulTrader), not
    // the intended sibling lbrnet checkout -- confirmed the hard way against
    // real data before this fix.
    std::string data_dir = "/home/rcruz/devel/VSCode/lbrnet/data/scid";
    std::string data_dir_live = "/mnt/c/SierraChart2/Data";
    std::string output_path = "/home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet";
    bool sync_only = false, no_sync = false, full_rebuild = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--data-dir") data_dir = next("--data-dir");
        else if (arg == "--data-dir-live") data_dir_live = next("--data-dir-live");
        else if (arg == "--output") output_path = next("--output");
        else if (arg == "--sync-only") sync_only = true;
        else if (arg == "--no-sync") no_sync = true;
        else if (arg == "--full-rebuild") full_rebuild = true;
        else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            PrintUsage(argv[0]);
            return 1;
        }
    }

    std::map<std::string, scid::MirrorSyncAction> sync_actions;
    if (!no_sync) {
        std::printf("syncing mirror from %s...\n", data_dir_live.c_str());
        std::fflush(stdout);
        auto results = scid::SyncScidMirror(data_dir_live, data_dir);
        for (const auto& r : results) {
            sync_actions[r.contract_filename] = r.action;
            const char* action_name = r.action == scid::MirrorSyncAction::kNew          ? "new"
                                       : r.action == scid::MirrorSyncAction::kUnchanged  ? "unchanged"
                                       : r.action == scid::MirrorSyncAction::kGrown      ? "grown"
                                                                                          : "shrunk/changed";
            std::printf("  sync %s: %s\n", r.contract_filename.c_str(), action_name);
        }
        std::fflush(stdout);
        if (sync_only) {
            int n_new = 0, n_unchanged = 0, n_grown = 0, n_changed = 0;
            for (const auto& r : results) {
                switch (r.action) {
                    case scid::MirrorSyncAction::kNew: ++n_new; break;
                    case scid::MirrorSyncAction::kUnchanged: ++n_unchanged; break;
                    case scid::MirrorSyncAction::kGrown: ++n_grown; break;
                    case scid::MirrorSyncAction::kShrunkOrChanged: ++n_changed; break;
                }
            }
            std::printf("sync summary: %d new, %d unchanged, %d grown, %d shrunk/changed\n",
                        n_new, n_unchanged, n_grown, n_changed);
            return 0;
        }
    }

    std::vector<scid::ContractWindow> windows;
    try {
        windows = scid::DiscoverContracts(data_dir);
    } catch (const std::exception& e) {
        std::fprintf(stderr, "DiscoverContracts failed: %s\n", e.what());
        return 1;
    }

    const std::string parts_dir = data_dir + "/.parts";
    std::filesystem::create_directories(parts_dir);

    struct ContractPlan {
        scid::ContractWindow window;
        std::string part_path;
        scid::PartAction action;
    };
    std::vector<ContractPlan> plans;
    plans.reserve(windows.size());
    for (const auto& window : windows) {
        const std::string filename = std::filesystem::path(window.path).filename().string();
        const std::string label = scid::ContractLabel(window.month_code, window.year);
        const std::string part_path = parts_dir + "/" + label + ".parquet";

        scid::MirrorSyncAction sync_action = scid::MirrorSyncAction::kUnchanged;
        if (!no_sync) {
            auto it = sync_actions.find(filename);
            if (it != sync_actions.end()) sync_action = it->second;
        }
        const bool has_existing_part = std::filesystem::exists(part_path);
        bool needs_trim = false;
        if (window.active_end_us.has_value() && has_existing_part) {
            auto summary = ReadPartSummary(part_path);
            needs_trim = summary.has_value() && summary->ts_max.has_value() &&
                         *summary->ts_max >= *window.active_end_us;
        }
        const auto action = scid::DecidePartAction(sync_action, has_existing_part, full_rebuild, needs_trim);
        plans.push_back({window, part_path, action});
    }

    // Parallel decode phase: one thread per contract needing decode, capped at
    // hardware_concurrency (spec §4's single biggest speed lever -- decoding
    // ~13 independent multi-GB files sequentially otherwise dominates wall-clock time).
    std::vector<std::size_t> decode_indices;
    for (std::size_t i = 0; i < plans.size(); ++i) {
        if (plans[i].action == scid::PartAction::kFullDecode ||
            plans[i].action == scid::PartAction::kResumeAppend) {
            decode_indices.push_back(i);
        }
    }
    std::vector<BuildResult> decode_results(decode_indices.size());
    if (!decode_indices.empty()) {
        std::atomic<std::size_t> next_idx{0};
        const unsigned n_threads = std::max(
            1u, std::min<unsigned>(std::thread::hardware_concurrency(),
                                    static_cast<unsigned>(decode_indices.size())));
        std::vector<std::thread> workers;
        workers.reserve(n_threads);
        for (unsigned t = 0; t < n_threads; ++t) {
            workers.emplace_back([&]() {
                std::size_t idx;
                while ((idx = next_idx.fetch_add(1)) < decode_indices.size()) {
                    const auto& plan = plans[decode_indices[idx]];
                    const std::string label = scid::ContractLabel(plan.window.month_code, plan.window.year);
                    const bool is_full = plan.action == scid::PartAction::kFullDecode;
                    LogProgress("  starting " + std::string(is_full ? "full-decode" : "resume-append") +
                                " " + label + "...");
                    decode_results[idx] = is_full ? FullDecodeContract(plan.window, plan.part_path)
                                                   : ResumeAppendContract(plan.window, plan.part_path);
                }
            });
        }
        for (auto& w : workers) w.join();
    }
    bool any_failed = false;
    for (std::size_t i = 0; i < decode_results.size(); ++i) {
        const auto& r = decode_results[i];
        const char* kind =
            plans[decode_indices[i]].action == scid::PartAction::kFullDecode ? "full-decode" : "resume-append";
        std::printf("%s %s: %s\n", kind, r.contract_label.c_str(), r.ok ? "ok" : "FAILED");
        if (!r.ok) any_failed = true;
    }
    if (any_failed) {
        std::fprintf(stderr, "one or more contracts failed to decode; aborting before merge\n");
        return 1;
    }

    // Trim phase: sequential -- cheap (filters an existing part, no fresh
    // decode) and rare (only fires when a sibling contract's window just closed).
    for (auto& plan : plans) {
        if (plan.action == scid::PartAction::kTrim) {
            auto r = TrimContract(plan.window, plan.part_path);
            std::printf("trim %s: %s\n", r.contract_label.c_str(), r.ok ? "ok" : "FAILED");
            if (!r.ok) {
                std::fprintf(stderr, "trim failed for %s\n", r.contract_label.c_str());
                return 1;
            }
        }
    }

    // Final merge: every part is already sorted and contracts are
    // non-overlapping by construction, so this is a linear concatenation in
    // contract order, not a genuine multi-way sort -- streamed row-batch by
    // row-batch, never loading two contracts' full tick data into RAM at once.
    auto schema = BuildTickSchema();
    auto writer = PartWriter::Open(output_path, schema);
    if (!writer) {
        std::fprintf(stderr, "cannot open final output writer %s\n", output_path.c_str());
        return 1;
    }
    std::int64_t total_rows = 0;
    std::optional<std::int64_t> ts_min, ts_max;
    for (const auto& plan : plans) {
        auto summary = ReadPartSummary(plan.part_path);
        if (!summary.has_value()) continue;  // e.g. a brand-new contract with zero ticks in its window
        if (!StreamExistingPartInto(plan.part_path, *writer)) {
            std::fprintf(stderr, "failed to merge part %s\n", plan.part_path.c_str());
            return 1;
        }
        total_rows += summary->rows;
        if (summary->ts_min.has_value() && (!ts_min.has_value() || *summary->ts_min < *ts_min)) {
            ts_min = summary->ts_min;
        }
        if (summary->ts_max.has_value() && (!ts_max.has_value() || *summary->ts_max > *ts_max)) {
            ts_max = summary->ts_max;
        }
    }
    if (!writer->Close()) {
        std::fprintf(stderr, "failed to close final output %s\n", output_path.c_str());
        return 1;
    }

    std::printf("done: %lld total rows, %zu contracts, ts_min=%lld, ts_max=%lld, output=%s\n",
                static_cast<long long>(total_rows), plans.size(),
                static_cast<long long>(ts_min.value_or(0)), static_cast<long long>(ts_max.value_or(0)),
                output_path.c_str());
    return 0;
}
