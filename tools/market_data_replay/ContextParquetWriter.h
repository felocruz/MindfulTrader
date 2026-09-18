// ContextParquetWriter.h -- streaming Parquet writer for
// tools/market_data_replay/, replacing native `.context` FlatBuffer output.
//
// Same public interface as ContextFileWriter.h (Open/LogContext/Close) so
// MarketDataReplayContext.cpp's call sites don't change -- only the on-disk
// format does. Reason for this: lbrnet was previously expected to run
// tools/context_pipeline/context_to_parquet.cpp by hand on every new offline
// `.context` file it received; this writer does that conversion inline,
// during replay, so lbrnet always receives a ready-to-read `.context.parquet`
// file directly (operator directive, 2026-09-17).
//
// Column set and Arrow/Parquet write mechanics (schema, chunked
// NewBufferedRowGroup()+WriteRecordBatch(), kChunkRows) are a deliberate,
// close port of context_to_parquet.cpp's own already-proven pattern -- same
// columns (ObservationData's 18 floats + RiskGateContext's 14 floats/
// regime_duration/is_valid/snapshot_timestamp_us/context_available +
// sequence_id/timestamp_us/bars_since_last_update), same bounded-memory
// chunking discipline. AsymmetryContext is deliberately NOT a column here,
// matching context_to_parquet.cpp's own existing decision -- this tool never
// populates it with real values (see MarketDataReplayContext.cpp's own
// documented empty-sentinel comment), so it would add no real information.
//
// KEEP IN SYNC with context_to_parquet.cpp's BuildArrowSchema()/AppendPair()
// if either one's column set changes -- there is no automated check tying
// these two together (same caveat ContextFileWriter.h's own header documents
// for its relationship to LBRFileManager.cpp).

#pragma once

#include "generated/mts_schema_contract_generated.h"
#include "generated/mts_schema_generated.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>

#include <array>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

class ContextParquetWriter {
public:
    // path + ".context.parquet" is opened -- mirrors ContextFileWriter::Open()'s
    // path+".context" convention, just with the new extension.
    bool Open(const std::string& path, const std::string& /*symbol*/) {
        Close();
        m_buffers.Reserve(kChunkRows);

        auto schema_status = BuildSchema(&m_schema);
        if (!schema_status.ok()) {
            return false;
        }
        auto sink_result = arrow::io::FileOutputStream::Open(path + ".context.parquet");
        if (!sink_result.ok()) {
            return false;
        }
        auto writer_result = parquet::arrow::FileWriter::Open(
            *m_schema, arrow::default_memory_pool(), *sink_result);
        if (!writer_result.ok()) {
            return false;
        }
        m_writer = std::move(*writer_result);
        m_isOpen = true;
        return true;
    }

    void Close() {
        if (!m_isOpen) {
            return;
        }
        FlushChunk();
        auto status = m_writer->Close();
        (void)status;  // best-effort on close; nothing actionable left to do here
        m_writer.reset();
        m_isOpen = false;
    }

    // Same signature as ContextFileWriter::LogContext() -- `ctx` (AsymmetryContext)
    // is accepted for call-site compatibility but not written (see header comment).
    void LogContext(
        const MTS::Schema::ObservationData& obs,
        const MTS::Schema::AsymmetryContext& /*ctx*/,
        uint64_t timestamp_us,
        float bars_since_last_update,
        const MTS::Schema::RiskGateContextT* risk_gate_context = nullptr) {
        if (!m_isOpen) {
            return;
        }
        const std::array<float, MTS::Schema::Contract::kObservationDim> obs_values =
            MTS::Schema::Contract::ToObservationArray(obs);
        for (std::size_t i = 0; i < obs_values.size(); ++i) {
            m_buffers.observation_cols[i].push_back(obs_values[i]);
        }

        if (risk_gate_context != nullptr) {
            const std::array<float, MTS::Schema::Contract::kRiskGateFloatFieldCount> rgc_values = {
                risk_gate_context->shannon_flow_entropy, risk_gate_context->shannon_efficiency,
                risk_gate_context->taleb_kurtosis, risk_gate_context->taleb_skewness,
                risk_gate_context->elder_chandelier_atr, risk_gate_context->pareto_tail_alpha,
                risk_gate_context->amihud_illiquidity, risk_gate_context->spread_stress,
                risk_gate_context->hurst_exponent, risk_gate_context->fractal_dim,
                risk_gate_context->mean_rev_z, risk_gate_context->raschke_burst,
                risk_gate_context->fisher_info, risk_gate_context->amihud_percentile,
            };
            for (std::size_t i = 0; i < rgc_values.size(); ++i) {
                m_buffers.risk_gate_cols[i].push_back(rgc_values[i]);
            }
            m_buffers.risk_gate_regime_duration.push_back(risk_gate_context->regime_duration);
            m_buffers.risk_gate_is_valid.push_back(risk_gate_context->is_valid);
            m_buffers.risk_gate_snapshot_timestamp_us.push_back(risk_gate_context->snapshot_timestamp_us);
            m_buffers.risk_gate_context_available.push_back(true);
        } else {
            for (std::size_t i = 0; i < kRiskGateFloatDefaults.size(); ++i) {
                m_buffers.risk_gate_cols[i].push_back(kRiskGateFloatDefaults[i]);
            }
            m_buffers.risk_gate_regime_duration.push_back(0);
            m_buffers.risk_gate_is_valid.push_back(false);
            m_buffers.risk_gate_snapshot_timestamp_us.push_back(0);
            m_buffers.risk_gate_context_available.push_back(false);
        }

        m_buffers.sequence_id.push_back(m_sequenceId++);
        m_buffers.timestamp_us.push_back(static_cast<std::int64_t>(timestamp_us));
        m_buffers.bars_since_last_update.push_back(bars_since_last_update);

        if (m_buffers.Rows() >= kChunkRows) {
            FlushChunk();
        }
    }

private:
    // Matches RiskGateContext's own .fbs-declared field defaults (mts_schema.fbs),
    // same values context_to_parquet.cpp's kRiskGateFloatDefaults already uses.
    static constexpr std::array<float, MTS::Schema::Contract::kRiskGateFloatFieldCount> kRiskGateFloatDefaults = {
        0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f, 0.5f, 1.5f, 0.0f, 1.0f, 0.0f, 0.5f,
    };
    static constexpr std::size_t kChunkRows = 2'000'000;  // matches context_to_parquet.cpp's own convention

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

    static arrow::Status BuildSchema(std::shared_ptr<arrow::Schema>* out) {
        std::vector<std::shared_ptr<arrow::Field>> fields;
        for (std::size_t i = 0; i < MTS::Schema::Contract::kObservationDim; ++i) {
            fields.push_back(arrow::field(MTS::Schema::Contract::kObservationFieldNames[i], arrow::float32()));
        }
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

    arrow::Status BuildRecordBatch(std::shared_ptr<arrow::RecordBatch>* out) const {
        std::vector<std::shared_ptr<arrow::Array>> columns;
        auto append_float_col = [&](const std::vector<float>& col) -> arrow::Status {
            arrow::FloatBuilder builder;
            ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<std::int64_t>(col.size())));
            ARROW_RETURN_NOT_OK(builder.AppendValues(col));
            std::shared_ptr<arrow::Array> arr;
            ARROW_RETURN_NOT_OK(builder.Finish(&arr));
            columns.push_back(arr);
            return arrow::Status::OK();
        };
        for (const auto& col : m_buffers.observation_cols) ARROW_RETURN_NOT_OK(append_float_col(col));
        for (const auto& col : m_buffers.risk_gate_cols) ARROW_RETURN_NOT_OK(append_float_col(col));
        {
            arrow::Int32Builder builder;
            ARROW_RETURN_NOT_OK(builder.AppendValues(m_buffers.risk_gate_regime_duration));
            std::shared_ptr<arrow::Array> arr;
            ARROW_RETURN_NOT_OK(builder.Finish(&arr));
            columns.push_back(arr);
        }
        {
            arrow::BooleanBuilder builder;
            ARROW_RETURN_NOT_OK(builder.AppendValues(m_buffers.risk_gate_is_valid));
            std::shared_ptr<arrow::Array> arr;
            ARROW_RETURN_NOT_OK(builder.Finish(&arr));
            columns.push_back(arr);
        }
        {
            arrow::Int64Builder builder;
            ARROW_RETURN_NOT_OK(builder.AppendValues(m_buffers.risk_gate_snapshot_timestamp_us));
            std::shared_ptr<arrow::Array> arr;
            ARROW_RETURN_NOT_OK(builder.Finish(&arr));
            columns.push_back(arr);
        }
        {
            arrow::BooleanBuilder builder;
            ARROW_RETURN_NOT_OK(builder.AppendValues(m_buffers.risk_gate_context_available));
            std::shared_ptr<arrow::Array> arr;
            ARROW_RETURN_NOT_OK(builder.Finish(&arr));
            columns.push_back(arr);
        }
        {
            arrow::UInt64Builder builder;
            ARROW_RETURN_NOT_OK(builder.AppendValues(m_buffers.sequence_id));
            std::shared_ptr<arrow::Array> arr;
            ARROW_RETURN_NOT_OK(builder.Finish(&arr));
            columns.push_back(arr);
        }
        {
            arrow::Int64Builder builder;
            ARROW_RETURN_NOT_OK(builder.AppendValues(m_buffers.timestamp_us));
            std::shared_ptr<arrow::Array> arr;
            ARROW_RETURN_NOT_OK(builder.Finish(&arr));
            columns.push_back(arr);
        }
        ARROW_RETURN_NOT_OK(append_float_col(m_buffers.bars_since_last_update));

        *out = arrow::RecordBatch::Make(m_schema, static_cast<std::int64_t>(m_buffers.Rows()), columns);
        return arrow::Status::OK();
    }

    void FlushChunk() {
        if (m_buffers.Rows() == 0) {
            return;
        }
        auto s1 = m_writer->NewBufferedRowGroup();
        if (!s1.ok()) {
            throw std::runtime_error("ContextParquetWriter: NewBufferedRowGroup failed: " + s1.ToString());
        }
        std::shared_ptr<arrow::RecordBatch> batch;
        auto s2 = BuildRecordBatch(&batch);
        if (!s2.ok()) {
            throw std::runtime_error("ContextParquetWriter: BuildRecordBatch failed: " + s2.ToString());
        }
        auto s3 = m_writer->WriteRecordBatch(*batch);
        if (!s3.ok()) {
            throw std::runtime_error("ContextParquetWriter: WriteRecordBatch failed: " + s3.ToString());
        }
        m_buffers.Clear();
    }

    std::shared_ptr<arrow::Schema> m_schema;
    std::unique_ptr<parquet::arrow::FileWriter> m_writer;
    ChunkBuffers m_buffers;
    bool m_isOpen = false;
    std::uint64_t m_sequenceId = 0;
};
