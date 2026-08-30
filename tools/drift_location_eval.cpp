// tools/drift_location_eval.cpp
// Offline, model-independent prototype validator for the §5.0 drift/location
// observation-vector candidate. Reads lbrnet/data/raw/mes_continuous_ticks.parquet
// DIRECTLY via Arrow -- the first C++ tool in this codebase to read Parquet
// (every prior tool, e.g. context_to_parquet.cpp, only writes). This removes
// the old mean_rev_z_variant_comparison.py pattern's Python round-trip
// (polars-read -> custom-binary-export -> C++-read -> CSV -> Python-scores).
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   tools/drift_location_eval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/drift_location_eval
#include "drift_location_stats.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

struct TickSeries {
    std::vector<std::int64_t> timestamp_us;
    std::vector<double> close;
};

namespace {

// Flattens a Table column's chunks into a contiguous std::vector -- a Table
// column is an arrow::ChunkedArray (may span multiple row-group chunks), not
// a flat array, so this can't just be a single raw_values() call.
template <typename ArrowArrayType, typename ValueType>
std::vector<ValueType> FlattenColumn(const std::shared_ptr<arrow::ChunkedArray>& col) {
    std::vector<ValueType> out;
    out.reserve(static_cast<std::size_t>(col->length()));
    for (const auto& chunk : col->chunks()) {
        auto typed = std::static_pointer_cast<ArrowArrayType>(chunk);
        const ValueType* raw = typed->raw_values();
        out.insert(out.end(), raw, raw + typed->length());
    }
    return out;
}

}  // namespace

inline TickSeries ReadTicksParquet(const std::string& path) {
    auto infile_result = arrow::io::ReadableFile::Open(path);
    if (!infile_result.ok()) {
        throw std::runtime_error("ReadTicksParquet: cannot open " + path + ": " +
                                  infile_result.status().ToString());
    }

    parquet::arrow::FileReaderBuilder builder;
    auto open_status = builder.Open(*infile_result);
    if (!open_status.ok()) {
        throw std::runtime_error("ReadTicksParquet: FileReaderBuilder::Open failed: " +
                                  open_status.ToString());
    }
    std::unique_ptr<parquet::arrow::FileReader> reader;
    auto build_status = builder.Build(&reader);
    if (!build_status.ok()) {
        throw std::runtime_error("ReadTicksParquet: FileReaderBuilder::Build failed: " +
                                  build_status.ToString());
    }

    // Column projection: read ONLY timestamp_us/close, not all 12 columns
    // (including a string `contract` column and several unneeded doubles) --
    // reading the full table was measured taking 5+ minutes on this 38.5M-row
    // file with zero output, versus seconds with projection. Indices are
    // resolved from the real schema, never hardcoded, so a future column
    // reorder in the source Parquet can't silently read the wrong data.
    std::shared_ptr<arrow::Schema> schema;
    auto schema_status = reader->GetSchema(&schema);
    if (!schema_status.ok()) {
        throw std::runtime_error("ReadTicksParquet: GetSchema failed: " + schema_status.ToString());
    }
    const int ts_idx = schema->GetFieldIndex("timestamp_us");
    const int close_idx = schema->GetFieldIndex("close");
    if (ts_idx < 0 || close_idx < 0) {
        throw std::runtime_error("ReadTicksParquet: missing timestamp_us or close column in " + path);
    }

    std::shared_ptr<arrow::Table> table;
    auto read_status = reader->ReadTable({ts_idx, close_idx}, &table);
    if (!read_status.ok()) {
        throw std::runtime_error("ReadTicksParquet: ReadTable failed: " + read_status.ToString());
    }

    auto ts_col = table->GetColumnByName("timestamp_us");
    auto close_col = table->GetColumnByName("close");
    if (!ts_col || !close_col) {
        throw std::runtime_error("ReadTicksParquet: missing timestamp_us or close column in projected table");
    }

    TickSeries series;
    series.timestamp_us = FlattenColumn<arrow::Int64Array, std::int64_t>(ts_col);
    series.close = FlattenColumn<arrow::DoubleArray, double>(close_col);
    return series;
}

namespace {

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --ticks-parquet PATH [--window N] [--horizons 30,60,120,240] "
        "[--report-json PATH]\n", argv0);
}

std::vector<int> ParseHorizons(const std::string& csv) {
    std::vector<int> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        out.push_back(std::stoi(item));
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticks_path, report_json_path;
    std::size_t window = 100;
    std::string horizons_csv = "30,60,120,240";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--ticks-parquet") ticks_path = next("--ticks-parquet");
        else if (arg == "--window") window = std::stoull(next("--window"));
        else if (arg == "--horizons") horizons_csv = next("--horizons");
        else if (arg == "--report-json") report_json_path = next("--report-json");
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); PrintUsage(argv[0]); return 1; }
    }
    if (ticks_path.empty()) {
        PrintUsage(argv[0]);
        return 1;
    }

    TickSeries series;
    try {
        series = ReadTicksParquet(ticks_path);
    } catch (const std::runtime_error& e) {
        std::fprintf(stderr, "❌ %s\n", e.what());
        return 1;
    }
    std::printf("Loaded %zu rows from %s\n", series.timestamp_us.size(), ticks_path.c_str());
    std::fflush(stdout);

    const auto log_returns = ComputeLogReturns(series.close);
    const auto z = ComputeDriftZScore(log_returns, window);
    // z[] is indexed against log_returns (length n-1), which is itself offset
    // by 1 from series.close/timestamp_us -- signal at return-index i corresponds
    // to price-series index i+1 (the price AFTER the return that produced z[i]).
    std::vector<std::int64_t> signal_ts;
    std::vector<double> signal_price, signal_z;
    for (std::size_t i = 0; i < z.size(); ++i) {
        if (std::isnan(z[i])) continue;
        signal_ts.push_back(series.timestamp_us[i + 1]);
        signal_price.push_back(series.close[i + 1]);
        signal_z.push_back(z[i]);
    }
    std::printf("%zu non-warmup drift/location signals (window=%zu)\n", signal_ts.size(), window);
    std::fflush(stdout);

    const auto horizons = ParseHorizons(horizons_csv);
    const double bonferroni_alpha = 0.05 / static_cast<double>(horizons.size());

    std::printf("\n=== Predictive power (directional, continuation): forward-return sign vs. drift z-score sign ===\n");
    std::ofstream json_out;
    if (!report_json_path.empty()) {
        json_out.open(report_json_path);
        json_out << "{\n  \"window\": " << window << ",\n  \"horizons\": [\n";
    }
    for (std::size_t h_idx = 0; h_idx < horizons.size(); ++h_idx) {
        const int h = horizons[h_idx];
        std::fprintf(stderr, "[progress] computing horizon=%dmin (%zu/%zu)...\n", h, h_idx + 1, horizons.size());
        const auto fwd = ComputeForwardReturns(signal_ts, signal_price, series.timestamp_us, series.close, h);
        const auto result = ComputeHitRate(fwd, signal_z);
        const bool survives = result.p_value < bonferroni_alpha;
        std::printf("  %4dmin: n=%-8zu hit_rate=%.4f 95%%CI=[%.4f,%.4f] p=%.4f (%s, alpha=%.5f for %zu tests)\n",
                    h, result.n, result.hit_rate, result.ci_lo, result.ci_hi, result.p_value,
                    survives ? "SURVIVES Bonferroni" : "does not survive Bonferroni",
                    bonferroni_alpha, horizons.size());
        if (json_out.is_open()) {
            json_out << "    {\"horizon_minutes\": " << h << ", \"n\": " << result.n
                      << ", \"hit_rate\": " << result.hit_rate
                      << ", \"ci_lo\": " << result.ci_lo << ", \"ci_hi\": " << result.ci_hi
                      << ", \"p_value\": " << result.p_value
                      << ", \"survives_bonferroni\": " << (survives ? "true" : "false") << "}"
                      << (h_idx + 1 < horizons.size() ? ",\n" : "\n");
        }
    }
    if (json_out.is_open()) {
        json_out << "  ]\n}\n";
        std::printf("\nWrote JSON report: %s\n", report_json_path.c_str());
    }
    return 0;
}
