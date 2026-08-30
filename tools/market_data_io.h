// tools/market_data_io.h
// Shared Arrow Parquet reading utility for MES tick data, extracted from
// tools/drift_location_eval.cpp on its second real use (tools/jump_ratio_eval.cpp)
// -- the right moment for this extraction, not premature. This codebase's
// first direct Parquet read remains context-preserved in git history; every
// prior tool (context_to_parquet.cpp) only writes.
#pragma once

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

struct TickSeries {
    std::vector<std::int64_t> timestamp_us;
    std::vector<double> close;
};

namespace market_data_io_detail {

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

}  // namespace market_data_io_detail

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
    // reading the full table was measured at 5+ minutes with zero output on
    // this 38.5M-row file; projected, it's 7.5s. Indices resolved from the
    // real schema, never hardcoded.
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
    series.timestamp_us = market_data_io_detail::FlattenColumn<arrow::Int64Array, std::int64_t>(ts_col);
    series.close = market_data_io_detail::FlattenColumn<arrow::DoubleArray, double>(close_col);
    return series;
}
