#pragma once
#include <fstream>
#include <string>
#include <array>
#include <mutex>
#include "generated/mts_schema_generated.h"

// Singleton manager for FlatBuffer binary I/O (Alpha/Context streams)
class LBRFileManager {
public:
    static LBRFileManager& Instance();

    // Reserve next global sequence ID (monotonic across all streams).
    uint64_t ReserveSequenceId();

    // Open both streams, write magic header and FileMetadata
    bool Open(const std::string& path, const std::string& symbol);
    // Close both streams
    void Close();
    // Log alpha event as paired MarketObservation + SystemState records
    void LogAlpha(MTS::Training::TrainingEventT& event);
    // Log alpha event using caller-provided sequence_id (for stitched writes).
    void LogAlphaWithSequence(MTS::Training::TrainingEventT& event, uint64_t sequence_id);
    // Log context snapshot to Context stream as paired MarketObservation + SystemState records.
    // Optional risk_gate_context serializes the raw, unscaled RiskManager gate inputs
    // (Findings 19/20/21) onto the MarketObservation; nullptr leaves the field unset.
    void LogContext(
        const MTS::Schema::ObservationData& obs,
        const MTS::Schema::AsymmetryContext& ctx,
        uint64_t timestamp_us,
        float bars_since_last_update,
        const MTS::Schema::RiskGateContextT* risk_gate_context = nullptr
    );
    // Log context snapshot with caller-provided sequence_id (for stitched writes).
    void LogContextWithSequence(
        const MTS::Schema::ObservationData& obs,
        const MTS::Schema::AsymmetryContext& ctx,
        uint64_t timestamp_us,
        float bars_since_last_update,
        uint64_t sequence_id,
        const MTS::Schema::RiskGateContextT* risk_gate_context = nullptr
    );
    // Log an Imbalance Triple Screen context snapshot to the .imbalance.context
    // stream, opened alongside .context/.alpha by the same Open() call
    // (architecture spec §1.6: one Open() producing all streams together keeps
    // them trivially aligned to the same collection run/symbol by construction).
    // Single-record stream, not MO+SS paired -- no ImbalanceSystemState yet.
    // Optional risk_gate_context (§1.5a), same convention as LogContext: nullptr
    // leaves the field unset.
    void LogImbalanceContext(
        const MTS::Schema::ImbalanceObservationData& obs,
        uint64_t timestamp_us,
        const MTS::Schema::ImbalanceRiskGateContextT* risk_gate_context = nullptr
    );
    // Sequence-locked stitcher: write MarketObservation + SystemState + TrainingEvent under one shared sequence_id.
    // Optional risk_gate_context, same convention as LogContext/LogContextWithSequence: nullptr
    // leaves the field unset.
    void LogSynchronizedEvent(
        MTS::Training::TrainingEventT& event,
        const MTS::Schema::ObservationData& obs,
        const MTS::Schema::AsymmetryContext& ctx,
        uint64_t timestamp_us,
        float bars_since_last_update,
        const MTS::Schema::RiskGateContextT* risk_gate_context = nullptr
    );

private:
    LBRFileManager();
    ~LBRFileManager();
    LBRFileManager(const LBRFileManager&) = delete;
    LBRFileManager& operator=(const LBRFileManager&) = delete;

    // Close helper that assumes m_mutex is already held.
    void CloseUnlocked();
    void LogAlphaUnlocked(MTS::Training::TrainingEventT& event, uint64_t sequence_id);
    void LogImbalanceContextUnlocked(
        const MTS::Schema::ImbalanceObservationData& obs,
        uint64_t timestamp_us,
        uint64_t sequence_id,
        const MTS::Schema::ImbalanceRiskGateContextT* risk_gate_context = nullptr
    );
    void LogContextUnlocked(
        const MTS::Schema::ObservationData& obs,
        const MTS::Schema::AsymmetryContext& ctx,
        uint64_t timestamp_us,
        float bars_since_last_update,
        uint64_t sequence_id,
        const MTS::Schema::RiskGateContextT* risk_gate_context = nullptr
    );

    void WriteToStream(std::ofstream& stream, uint8_t* buf, size_t size);
    void FlushStreamIfNeeded(std::ofstream& stream, uint32_t& since_last_flush, bool force = false);
    void WriteMagicHeader(std::ofstream& stream);
    void WriteFileMetadata(std::ofstream& stream, const std::string& symbol, const std::string& timeframe);

    std::ofstream m_alphaStream;
    std::ofstream m_contextStream;
    std::ofstream m_imbalanceContextStream;
    flatbuffers::FlatBufferBuilder m_fbb{2048};
    std::mutex m_mutex;
    bool m_isOpen = false;

    static constexpr uint32_t kFlushEveryRecords = 512;
    uint32_t m_alphaRecordsSinceFlush = 0;
    uint32_t m_contextRecordsSinceFlush = 0;
    uint32_t m_imbalanceContextRecordsSinceFlush = 0;

    // Global atomic sequence ID for all log entries
    std::atomic<uint64_t> m_globalSequenceId{0};
};
