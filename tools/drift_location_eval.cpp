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

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-mes_continuous_ticks.parquet>\n", argv[0]);
        return 1;
    }
    try {
        TickSeries series = ReadTicksParquet(argv[1]);
        std::printf("✅ read %zu rows, timestamp range [%lld, %lld], first close=%.4f, last close=%.4f\n",
                    series.timestamp_us.size(),
                    static_cast<long long>(series.timestamp_us.front()),
                    static_cast<long long>(series.timestamp_us.back()),
                    series.close.front(), series.close.back());
    } catch (const std::runtime_error& e) {
        std::fprintf(stderr, "❌ %s\n", e.what());
        return 1;
    }
    return 0;
}
