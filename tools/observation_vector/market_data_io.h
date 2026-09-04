// tools/observation_vector/market_data_io.h
// Shared Arrow Parquet reading utility for MES tick data, extracted from
// tools/observation_vector/drift_location_eval.cpp on its second real use (tools/observation_vector/jump_ratio_eval.cpp)
// -- the right moment for this extraction, not premature. This codebase's
// first direct Parquet read remains context-preserved in git history; every
// prior tool (context_to_parquet.cpp) only writes.
#pragma once

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

struct TickSeries {
    std::vector<std::int64_t> timestamp_us;
    std::vector<double> trade_price;
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

    // Column projection: read ONLY timestamp_us/trade_price, not all columns
    // (e.g. a string `contract` column and several unneeded doubles) --
    // reading the full table was measured at 5+ minutes with zero output on
    // a 38.5M-row file; projected, it's 7.5s. Indices resolved from the real
    // schema, never hardcoded.
    std::shared_ptr<arrow::Schema> schema;
    auto schema_status = reader->GetSchema(&schema);
    if (!schema_status.ok()) {
        throw std::runtime_error("ReadTicksParquet: GetSchema failed: " + schema_status.ToString());
    }
    const int ts_idx = schema->GetFieldIndex("timestamp_us");
    const int price_idx = schema->GetFieldIndex("trade_price");
    if (ts_idx < 0 || price_idx < 0) {
        throw std::runtime_error("ReadTicksParquet: missing timestamp_us or trade_price column in " + path);
    }

    std::shared_ptr<arrow::Table> table;
    auto read_status = reader->ReadTable({ts_idx, price_idx}, &table);
    if (!read_status.ok()) {
        throw std::runtime_error("ReadTicksParquet: ReadTable failed: " + read_status.ToString());
    }

    auto ts_col = table->GetColumnByName("timestamp_us");
    auto price_col = table->GetColumnByName("trade_price");
    if (!ts_col || !price_col) {
        throw std::runtime_error("ReadTicksParquet: missing timestamp_us or trade_price column in projected table");
    }

    TickSeries series;
    series.timestamp_us = market_data_io_detail::FlattenColumn<arrow::Int64Array, std::int64_t>(ts_col);
    series.trade_price = market_data_io_detail::FlattenColumn<arrow::DoubleArray, double>(price_col);
    return series;
}

// DOD, bounded-memory alternative to ReadTicksParquet() for single-pass,
// no-lookback consumers (e.g. an online/ring-buffer statistic that only ever
// needs "the next timestamp/price", never random access to the whole
// series). ReadTicksParquet()'s full materialization is only appropriate for
// algorithms that genuinely need the whole series in memory at once (e.g.
// forward-return two-pointer merges, percentile computations) -- at this
// file's real production scale (471.9M rows), that's ~7.5GB for the two
// std::vectors alone, plus Arrow's own transient decode buffers held
// simultaneously during the copy: a real OOM risk on a memory-constrained
// machine that a single-pass consumer doesn't need to take on at all.
//
// Streams one Parquet row group at a time (this file's own row-group size,
// tens of MB) via GetRecordBatchReader's column-projected, all-row-groups
// overload -- never holds more than one batch in memory regardless of total
// row count.
inline void StreamTicksParquet(const std::string& path,
                                const std::function<void(std::int64_t timestamp_us, double trade_price)>& on_row) {
    auto infile_result = arrow::io::ReadableFile::Open(path);
    if (!infile_result.ok()) {
        throw std::runtime_error("StreamTicksParquet: cannot open " + path + ": " +
                                  infile_result.status().ToString());
    }
    parquet::arrow::FileReaderBuilder builder;
    auto open_status = builder.Open(*infile_result);
    if (!open_status.ok()) {
        throw std::runtime_error("StreamTicksParquet: FileReaderBuilder::Open failed: " + open_status.ToString());
    }
    std::unique_ptr<parquet::arrow::FileReader> reader;
    auto build_status = builder.Build(&reader);
    if (!build_status.ok()) {
        throw std::runtime_error("StreamTicksParquet: FileReaderBuilder::Build failed: " + build_status.ToString());
    }

    std::shared_ptr<arrow::Schema> schema;
    auto schema_status = reader->GetSchema(&schema);
    if (!schema_status.ok()) {
        throw std::runtime_error("StreamTicksParquet: GetSchema failed: " + schema_status.ToString());
    }
    const int ts_idx = schema->GetFieldIndex("timestamp_us");
    const int price_idx = schema->GetFieldIndex("trade_price");
    if (ts_idx < 0 || price_idx < 0) {
        throw std::runtime_error("StreamTicksParquet: missing timestamp_us or trade_price column in " + path);
    }

    std::vector<int> row_groups(static_cast<std::size_t>(reader->num_row_groups()));
    for (std::size_t i = 0; i < row_groups.size(); ++i) row_groups[i] = static_cast<int>(i);

    auto batch_reader_result = reader->GetRecordBatchReader(row_groups, {ts_idx, price_idx});
    if (!batch_reader_result.ok()) {
        throw std::runtime_error("StreamTicksParquet: GetRecordBatchReader failed: " +
                                  batch_reader_result.status().ToString());
    }
    auto batch_reader = std::move(*batch_reader_result);

    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        auto next_status = batch_reader->ReadNext(&batch);
        if (!next_status.ok()) {
            throw std::runtime_error("StreamTicksParquet: ReadNext failed: " + next_status.ToString());
        }
        if (batch == nullptr) break;
        auto ts_arr = std::static_pointer_cast<arrow::Int64Array>(batch->column(0));
        auto price_arr = std::static_pointer_cast<arrow::DoubleArray>(batch->column(1));
        for (std::int64_t i = 0; i < batch->num_rows(); ++i) {
            on_row(ts_arr->Value(i), price_arr->Value(i));
        }
    }
}

// Same DOD streaming architecture as StreamTicksParquet, extended with
// `volume` (needed for Amihud-style illiquidity/bar-aggregation work) and a
// `isNewContract` flag (true only for the very first row of each contract).
// Cheap to detect: every row group in mes_ticks.parquet belongs to exactly
// one contract (verified 2026-09-02 via row-group min/max statistics on the
// `contract` column, all 7208 row groups) -- so this only needs to decode
// the dictionary-encoded `contract` column ONCE per batch (not per row) and
// compare against the previous batch's value.
inline void StreamTicksWithVolumeParquet(
    const std::string& path,
    const std::function<void(std::int64_t timestamp_us, double trade_price, std::int64_t volume, bool isNewContract)>& on_row) {
    auto infile_result = arrow::io::ReadableFile::Open(path);
    if (!infile_result.ok()) {
        throw std::runtime_error("StreamTicksWithVolumeParquet: cannot open " + path + ": " +
                                  infile_result.status().ToString());
    }
    parquet::arrow::FileReaderBuilder builder;
    auto open_status = builder.Open(*infile_result);
    if (!open_status.ok()) {
        throw std::runtime_error("StreamTicksWithVolumeParquet: FileReaderBuilder::Open failed: " + open_status.ToString());
    }
    std::unique_ptr<parquet::arrow::FileReader> reader;
    auto build_status = builder.Build(&reader);
    if (!build_status.ok()) {
        throw std::runtime_error("StreamTicksWithVolumeParquet: FileReaderBuilder::Build failed: " + build_status.ToString());
    }

    std::shared_ptr<arrow::Schema> schema;
    auto schema_status = reader->GetSchema(&schema);
    if (!schema_status.ok()) {
        throw std::runtime_error("StreamTicksWithVolumeParquet: GetSchema failed: " + schema_status.ToString());
    }
    const int ts_idx = schema->GetFieldIndex("timestamp_us");
    const int price_idx = schema->GetFieldIndex("trade_price");
    const int vol_idx = schema->GetFieldIndex("volume");
    const int contract_idx = schema->GetFieldIndex("contract");
    if (ts_idx < 0 || price_idx < 0 || vol_idx < 0 || contract_idx < 0) {
        throw std::runtime_error("StreamTicksWithVolumeParquet: missing a required column in " + path);
    }

    std::vector<int> row_groups(static_cast<std::size_t>(reader->num_row_groups()));
    for (std::size_t i = 0; i < row_groups.size(); ++i) row_groups[i] = static_cast<int>(i);

    auto batch_reader_result = reader->GetRecordBatchReader(row_groups, {ts_idx, price_idx, vol_idx, contract_idx});
    if (!batch_reader_result.ok()) {
        throw std::runtime_error("StreamTicksWithVolumeParquet: GetRecordBatchReader failed: " +
                                  batch_reader_result.status().ToString());
    }
    auto batch_reader = std::move(*batch_reader_result);

    std::string prevContract;
    bool haveContract = false;

    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        auto next_status = batch_reader->ReadNext(&batch);
        if (!next_status.ok()) {
            throw std::runtime_error("StreamTicksWithVolumeParquet: ReadNext failed: " + next_status.ToString());
        }
        if (batch == nullptr) break;
        if (batch->num_rows() == 0) continue;

        auto ts_arr = std::static_pointer_cast<arrow::Int64Array>(batch->column(0));
        auto price_arr = std::static_pointer_cast<arrow::DoubleArray>(batch->column(1));
        auto vol_arr = std::static_pointer_cast<arrow::Int64Array>(batch->column(2));

        // Contract is constant within a batch (one row group == one contract,
        // verified) -- decode it once, not per row.
        auto contract_col = batch->column(3);
        std::string currentContract;
        if (contract_col->type_id() == arrow::Type::DICTIONARY) {
            auto dict_arr = std::static_pointer_cast<arrow::DictionaryArray>(contract_col);
            auto dict_values = std::static_pointer_cast<arrow::StringArray>(dict_arr->dictionary());
            auto indices = std::static_pointer_cast<arrow::Int32Array>(dict_arr->indices());
            currentContract = dict_values->GetString(indices->Value(0));
        } else {
            auto str_arr = std::static_pointer_cast<arrow::StringArray>(contract_col);
            currentContract = str_arr->GetString(0);
        }
        const bool isNewContract = !haveContract || (currentContract != prevContract);
        prevContract = currentContract;
        haveContract = true;

        for (std::int64_t i = 0; i < batch->num_rows(); ++i) {
            on_row(ts_arr->Value(i), price_arr->Value(i), vol_arr->Value(i), isNewContract && i == 0);
        }
    }
}

// Same DOD/streaming pattern as StreamTicksWithVolumeParquet, additionally
// projecting ask_volume/bid_volume for consumers that need per-tick
// order-flow classification (e.g. ImbalanceBarEngine's askVolume/bidVolume
// bar-scoped running sums) -- observation_vector_recalibration.cpp's first
// user. ask_volume/bid_volume are this dataset's own per-tick trade-side-
// classified volumes (confirmed via schema: separate ask_volume/bid_volume/
// trade_side columns alongside the aggregate volume column), not a
// cumulative-since-bar-open running total -- callers wanting the latter
// (ImbalanceBarEngine's own expected semantic) must accumulate these
// per-tick values into their own bar-scoped running sum, resetting at each
// bar boundary, exactly as StreamTicksWithVolumeParquet's callers already do
// for close/high/low/volume bar aggregation.
inline void StreamTicksFullParquet(
    const std::string& path,
    const std::function<void(std::int64_t timestamp_us, double trade_price, std::int64_t volume,
                              std::int64_t askVolume, std::int64_t bidVolume, bool isNewContract)>& on_row) {
    auto infile_result = arrow::io::ReadableFile::Open(path);
    if (!infile_result.ok()) {
        throw std::runtime_error("StreamTicksFullParquet: cannot open " + path + ": " +
                                  infile_result.status().ToString());
    }
    parquet::arrow::FileReaderBuilder builder;
    auto open_status = builder.Open(*infile_result);
    if (!open_status.ok()) {
        throw std::runtime_error("StreamTicksFullParquet: FileReaderBuilder::Open failed: " + open_status.ToString());
    }
    std::unique_ptr<parquet::arrow::FileReader> reader;
    auto build_status = builder.Build(&reader);
    if (!build_status.ok()) {
        throw std::runtime_error("StreamTicksFullParquet: FileReaderBuilder::Build failed: " + build_status.ToString());
    }

    std::shared_ptr<arrow::Schema> schema;
    auto schema_status = reader->GetSchema(&schema);
    if (!schema_status.ok()) {
        throw std::runtime_error("StreamTicksFullParquet: GetSchema failed: " + schema_status.ToString());
    }
    const int ts_idx = schema->GetFieldIndex("timestamp_us");
    const int price_idx = schema->GetFieldIndex("trade_price");
    const int vol_idx = schema->GetFieldIndex("volume");
    const int ask_vol_idx = schema->GetFieldIndex("ask_volume");
    const int bid_vol_idx = schema->GetFieldIndex("bid_volume");
    const int contract_idx = schema->GetFieldIndex("contract");
    if (ts_idx < 0 || price_idx < 0 || vol_idx < 0 || ask_vol_idx < 0 || bid_vol_idx < 0 || contract_idx < 0) {
        throw std::runtime_error("StreamTicksFullParquet: missing a required column in " + path);
    }

    std::vector<int> row_groups(static_cast<std::size_t>(reader->num_row_groups()));
    for (std::size_t i = 0; i < row_groups.size(); ++i) row_groups[i] = static_cast<int>(i);

    auto batch_reader_result =
        reader->GetRecordBatchReader(row_groups, {ts_idx, price_idx, vol_idx, ask_vol_idx, bid_vol_idx, contract_idx});
    if (!batch_reader_result.ok()) {
        throw std::runtime_error("StreamTicksFullParquet: GetRecordBatchReader failed: " +
                                  batch_reader_result.status().ToString());
    }
    auto batch_reader = std::move(*batch_reader_result);

    std::string prevContract;
    bool haveContract = false;

    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        auto next_status = batch_reader->ReadNext(&batch);
        if (!next_status.ok()) {
            throw std::runtime_error("StreamTicksFullParquet: ReadNext failed: " + next_status.ToString());
        }
        if (batch == nullptr) break;
        if (batch->num_rows() == 0) continue;

        auto ts_arr = std::static_pointer_cast<arrow::Int64Array>(batch->column(0));
        auto price_arr = std::static_pointer_cast<arrow::DoubleArray>(batch->column(1));
        auto vol_arr = std::static_pointer_cast<arrow::Int64Array>(batch->column(2));
        auto ask_vol_arr = std::static_pointer_cast<arrow::Int64Array>(batch->column(3));
        auto bid_vol_arr = std::static_pointer_cast<arrow::Int64Array>(batch->column(4));

        auto contract_col = batch->column(5);
        std::string currentContract;
        if (contract_col->type_id() == arrow::Type::DICTIONARY) {
            auto dict_arr = std::static_pointer_cast<arrow::DictionaryArray>(contract_col);
            auto dict_values = std::static_pointer_cast<arrow::StringArray>(dict_arr->dictionary());
            auto indices = std::static_pointer_cast<arrow::Int32Array>(dict_arr->indices());
            currentContract = dict_values->GetString(indices->Value(0));
        } else {
            auto str_arr = std::static_pointer_cast<arrow::StringArray>(contract_col);
            currentContract = str_arr->GetString(0);
        }
        const bool isNewContract = !haveContract || (currentContract != prevContract);
        prevContract = currentContract;
        haveContract = true;

        for (std::int64_t i = 0; i < batch->num_rows(); ++i) {
            on_row(ts_arr->Value(i), price_arr->Value(i), vol_arr->Value(i),
                   ask_vol_arr->Value(i), bid_vol_arr->Value(i), isNewContract && i == 0);
        }
    }
}

