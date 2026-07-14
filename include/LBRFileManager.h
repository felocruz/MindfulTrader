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
    void LogContext(
        const MTS::Schema::ObservationData& obs,
        const MTS::Schema::AsymmetryContext& ctx,
        uint64_t timestamp_us,
        float bars_since_last_update
    );
    // Log context snapshot with caller-provided sequence_id (for stitched writes).
    void LogContextWithSequence(
        const MTS::Schema::ObservationData& obs,
        const MTS::Schema::AsymmetryContext& ctx,
        uint64_t timestamp_us,
        float bars_since_last_update,
        uint64_t sequence_id
    );
    // Sequence-locked stitcher: write MarketObservation + SystemState + TrainingEvent under one shared sequence_id.
    void LogSynchronizedEvent(
        MTS::Training::TrainingEventT& event,
        const MTS::Schema::ObservationData& obs,
        const MTS::Schema::AsymmetryContext& ctx,
        uint64_t timestamp_us,
        float bars_since_last_update
    );

private:
    LBRFileManager();
    ~LBRFileManager();
    LBRFileManager(const LBRFileManager&) = delete;
    LBRFileManager& operator=(const LBRFileManager&) = delete;

    // Close helper that assumes m_mutex is already held.
    void CloseUnlocked();
    void LogAlphaUnlocked(MTS::Training::TrainingEventT& event, uint64_t sequence_id);
    void LogContextUnlocked(
        const MTS::Schema::ObservationData& obs,
        const MTS::Schema::AsymmetryContext& ctx,
        uint64_t timestamp_us,
        float bars_since_last_update,
        uint64_t sequence_id
    );

    void WriteToStream(std::ofstream& stream, uint8_t* buf, size_t size);
    void FlushStreamIfNeeded(std::ofstream& stream, uint32_t& since_last_flush, bool force = false);
    void WriteMagicHeader(std::ofstream& stream);
    void WriteFileMetadata(std::ofstream& stream, const std::string& symbol, const std::string& timeframe);

    std::ofstream m_alphaStream;
    std::ofstream m_contextStream;
    flatbuffers::FlatBufferBuilder m_fbb{2048};
    std::mutex m_mutex;
    bool m_isOpen = false;

    static constexpr uint32_t kFlushEveryRecords = 512;
    uint32_t m_alphaRecordsSinceFlush = 0;
    uint32_t m_contextRecordsSinceFlush = 0;

    // Global atomic sequence ID for all log entries
    std::atomic<uint64_t> m_globalSequenceId{0};
};
