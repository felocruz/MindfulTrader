#include "MindfulTrader_Precompiled.h"
#include "LBRFileManager.h"
#include "Logger.h"
#include <iostream>
#include <cstring>
#include <limits>
#include <errno.h>

LBRFileManager& LBRFileManager::Instance() {
    static LBRFileManager instance;
    return instance;
}

LBRFileManager::LBRFileManager() : m_isOpen(false), m_globalSequenceId(0) {
}

LBRFileManager::~LBRFileManager() {
    if (m_isOpen) {
        Close();
    }
}

uint64_t LBRFileManager::ReserveSequenceId() {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_globalSequenceId++;
}

bool LBRFileManager::Open(const std::string& path, const std::string& symbol) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (m_isOpen) {
        Logger::getInstance().log("WARNING: LBRFileManager::Open called but already open, closing first");
        CloseUnlocked();
    }

    // Clear any prior stream error flags before reopening.
    m_alphaStream.clear();
    m_contextStream.clear();

    // Open alpha stream (all training events)
    m_alphaStream.open(path + ".alpha", std::ios::binary | std::ios::out | std::ios::trunc);
    if (!m_alphaStream.is_open()) {
        Logger::getInstance().log("ERROR: LBRFileManager::Open failed to open alpha stream: " + path + ".alpha");
        return false;
    }

    // Open context stream (context vectors)
    m_contextStream.open(path + ".context", std::ios::binary | std::ios::out | std::ios::trunc);
    if (!m_contextStream.is_open()) {
        Logger::getInstance().log("ERROR: LBRFileManager::Open failed to open context stream: " + path + ".context");
        m_alphaStream.close();
        return false;
    }

    // Write magic headers to both streams
    WriteMagicHeader(m_alphaStream);
    WriteMagicHeader(m_contextStream);

    // Write file metadata
    WriteFileMetadata(m_alphaStream, symbol, "ALPHA_v2.5_TRAINING_EVENT");
    WriteFileMetadata(m_contextStream, symbol, "CONTEXT_v2.5_MO_SS");

    m_isOpen = true;
    m_globalSequenceId = 0;
    m_alphaRecordsSinceFlush = 0;
    m_contextRecordsSinceFlush = 0;
    Logger::getInstance().log("LBRFileManager::Open successful: " + path);

    return true;
}

void LBRFileManager::Close() {
    std::lock_guard<std::mutex> lock(m_mutex);
    CloseUnlocked();
}

void LBRFileManager::CloseUnlocked() {
    if (!m_isOpen && !m_alphaStream.is_open() && !m_contextStream.is_open()) {
        return;
    }

    if (m_alphaStream.is_open()) {
        m_alphaStream.flush();
        m_alphaStream.close();
    }

    if (m_contextStream.is_open()) {
        m_contextStream.flush();
        m_contextStream.close();
    }

    m_isOpen = false;
    m_alphaRecordsSinceFlush = 0;
    m_contextRecordsSinceFlush = 0;
    Logger::getInstance().log("LBRFileManager::Close: Streams closed successfully");
}

void LBRFileManager::LogAlpha(MTS::Training::TrainingEventT& event) {
    const uint64_t sequence_id = ReserveSequenceId();
    LogAlphaWithSequence(event, sequence_id);
}

void LBRFileManager::LogAlphaWithSequence(MTS::Training::TrainingEventT& event, uint64_t sequence_id) {
    std::lock_guard<std::mutex> lock(m_mutex);

    LogAlphaUnlocked(event, sequence_id);
}

void LBRFileManager::LogAlphaUnlocked(MTS::Training::TrainingEventT& event, uint64_t sequence_id) {
    if (!m_isOpen || !m_alphaStream.is_open()) {
        return;
    }

    event.sequence_id = sequence_id;

    m_fbb.Clear();
    auto training_event_offset = MTS::Training::CreateTrainingEvent(m_fbb, &event);
    m_fbb.Finish(training_event_offset);

    WriteToStream(m_alphaStream, m_fbb.GetBufferPointer(), m_fbb.GetSize());
}

void LBRFileManager::LogContext(
    const MTS::Schema::ObservationData& obs,
    const MTS::Schema::AsymmetryContext& ctx,
    uint64_t timestamp_us,
    float bars_since_last_update,
    const MTS::Schema::RiskGateContextT* risk_gate_context) {
    const uint64_t sequence_id = ReserveSequenceId();
    LogContextWithSequence(obs, ctx, timestamp_us, bars_since_last_update, sequence_id, risk_gate_context);
}

void LBRFileManager::LogContextWithSequence(
    const MTS::Schema::ObservationData& obs,
    const MTS::Schema::AsymmetryContext& ctx,
    uint64_t timestamp_us,
    float bars_since_last_update,
    uint64_t sequence_id,
    const MTS::Schema::RiskGateContextT* risk_gate_context) {
    std::lock_guard<std::mutex> lock(m_mutex);

    LogContextUnlocked(obs, ctx, timestamp_us, bars_since_last_update, sequence_id, risk_gate_context);
}

void LBRFileManager::LogContextUnlocked(
    const MTS::Schema::ObservationData& obs,
    const MTS::Schema::AsymmetryContext& ctx,
    uint64_t timestamp_us,
    float bars_since_last_update,
    uint64_t sequence_id,
    const MTS::Schema::RiskGateContextT* risk_gate_context) {
    if (!m_isOpen || !m_contextStream.is_open()) {
        return;
    }

    // MO+SS only contract: context stream is paired records with shared sequence_id.
    // Combined write: build both messages, then write as single I/O operation.

    // 1) MarketObservation
    m_fbb.Clear();

    // Raw, unscaled risk-gate inputs (Findings 19/20/21 wire parity). Built before
    // the MarketObservation table starts, per the FlatBuffers nested-table rule.
    // When the caller passes nullptr (live / synchronized paths) the field is left
    // unset and serializes zero bytes.
    ::flatbuffers::Offset<MTS::Schema::RiskGateContext> rgc_offset = 0;
    if (risk_gate_context != nullptr) {
        rgc_offset = MTS::Schema::CreateRiskGateContext(m_fbb, risk_gate_context);
    }

    auto mo_loc = MTS::Schema::CreateMarketObservation(
        m_fbb,
        static_cast<int64_t>(timestamp_us),
        sequence_id,
        &obs,
        &ctx,
        0,           // daily_bias_enum: unused on the .context path
        rgc_offset
    );
    m_fbb.Finish(mo_loc);

    const uint32_t mo_size = static_cast<uint32_t>(m_fbb.GetSize());
    const uint8_t* mo_ptr = m_fbb.GetBufferPointer();

    // Stage MO bytes into local buffer (size-prefix + payload)
    // Typical MO+SS combined: ~200 bytes, well within stack allocation.
    const size_t mo_total = sizeof(uint32_t) + mo_size;
    uint8_t staged[512];  // Stack buffer for both records
    std::memcpy(staged, &mo_size, sizeof(uint32_t));
    std::memcpy(staged + sizeof(uint32_t), mo_ptr, mo_size);

    // 2) SystemState
    m_fbb.Clear();
    auto ss_loc = MTS::Schema::CreateSystemState(
        m_fbb,
        static_cast<int64_t>(timestamp_us),
        sequence_id,
        bars_since_last_update
    );
    m_fbb.Finish(ss_loc);

    const uint32_t ss_size = static_cast<uint32_t>(m_fbb.GetSize());
    const uint8_t* ss_ptr = m_fbb.GetBufferPointer();
    const size_t ss_total = sizeof(uint32_t) + ss_size;
    const size_t combined_size = mo_total + ss_total;

    if (combined_size <= sizeof(staged)) {
        // Fast path: both records fit in stack buffer — single write
        std::memcpy(staged + mo_total, &ss_size, sizeof(uint32_t));
        std::memcpy(staged + mo_total + sizeof(uint32_t), ss_ptr, ss_size);
        m_contextStream.write(reinterpret_cast<const char*>(staged), combined_size);
    } else {
        // Fallback: records too large for stack buffer — two writes
        m_contextStream.write(reinterpret_cast<const char*>(staged), mo_total);
        uint32_t ss_len = ss_size;
        m_contextStream.write(reinterpret_cast<const char*>(&ss_len), sizeof(ss_len));
        m_contextStream.write(reinterpret_cast<const char*>(ss_ptr), ss_size);
    }

    // Count both MO and SS records toward flush threshold
    m_contextRecordsSinceFlush += 2;
    if (m_contextRecordsSinceFlush >= kFlushEveryRecords) {
        m_contextStream.flush();
        m_contextRecordsSinceFlush = 0;
    }
}

void LBRFileManager::LogSynchronizedEvent(
    MTS::Training::TrainingEventT& event,
    const MTS::Schema::ObservationData& obs,
    const MTS::Schema::AsymmetryContext& ctx,
    uint64_t timestamp_us,
    float bars_since_last_update) {
    std::lock_guard<std::mutex> lock(m_mutex);

    if (!m_isOpen || !m_alphaStream.is_open() || !m_contextStream.is_open()) {
        return;
    }

    const uint64_t sequence_id = m_globalSequenceId++;

    LogContextUnlocked(obs, ctx, timestamp_us, bars_since_last_update, sequence_id);
    LogAlphaUnlocked(event, sequence_id);
}

void LBRFileManager::WriteToStream(std::ofstream& stream, uint8_t* buf, size_t size) {
    if (!stream.is_open()) {
        return;
    }

    if (size > std::numeric_limits<uint32_t>::max()) {
        return;
    }

    // Combined write: 4-byte LE length prefix + binary payload in one call.
    // Reduces 2 stream.write() calls to 1 for better I/O throughput.
    uint32_t length = static_cast<uint32_t>(size);
    const size_t total = sizeof(length) + size;

    if (total <= 8192) {
        // Fast path: stack-buffer combined write (typical TrainingEvents are ~2KB)
        uint8_t combined[8192];
        std::memcpy(combined, &length, sizeof(length));
        std::memcpy(combined + sizeof(length), buf, size);
        stream.write(reinterpret_cast<const char*>(combined), total);
    } else {
        // Fallback: two writes for very large messages
        stream.write(reinterpret_cast<const char*>(&length), sizeof(length));
        stream.write(reinterpret_cast<const char*>(buf), size);
    }

    if (!stream.good()) {
        return;
    }

    if (&stream == &m_alphaStream) {
        FlushStreamIfNeeded(stream, m_alphaRecordsSinceFlush);
    } else if (&stream == &m_contextStream) {
        FlushStreamIfNeeded(stream, m_contextRecordsSinceFlush);
    } else {
        Logger::getInstance().log("ERROR: LBRFileManager::WriteToStream: Unknown stream reference");
        return;
    }
}

void LBRFileManager::FlushStreamIfNeeded(std::ofstream& stream, uint32_t& since_last_flush, bool force) {
    if (!stream.is_open()) {
        return;
    }

    if (!force) {
        ++since_last_flush;
        if (since_last_flush < kFlushEveryRecords) {
            return;
        }
    }

    stream.flush();
    if (!stream.good()) {
        Logger::getInstance().log("ERROR: LBRFileManager::WriteToStream: Failed to flush (errno=" + std::to_string(errno) + ")");
        return;
    }

    since_last_flush = 0;
}

void LBRFileManager::WriteMagicHeader(std::ofstream& stream) {
    if (!stream.is_open()) {
        return;
    }

    // Magic header: "LBRN" (Little-Big-R-N framework)
    const char magic[4] = {'L', 'B', 'R', 'N'};
    stream.write(magic, 4);
}

void LBRFileManager::WriteFileMetadata(std::ofstream& stream, const std::string& symbol, const std::string& timeframe) {
    if (!stream.is_open()) {
        return;
    }

    // Create FileMetadata FlatBuffer (Elite v2.3 format)
    flatbuffers::FlatBufferBuilder fbb(1024);

    auto symbol_offset = fbb.CreateString(symbol);
    auto timeframe_offset = fbb.CreateString(timeframe);

    // Use generated FileMetadata builder
    MTS::Training::FileMetadataBuilder metadata_builder(fbb);
    metadata_builder.add_symbol(symbol_offset);
    metadata_builder.add_timeframe(timeframe_offset);
    metadata_builder.add_schema_version(230);  // v2.3.0
    auto metadata_offset = metadata_builder.Finish();

    fbb.Finish(metadata_offset);

    // Write size-prefixed FlatBuffer: [4-byte LE size][FlatBuffer data]
    uint32_t size = static_cast<uint32_t>(fbb.GetSize());
    stream.write(reinterpret_cast<const char*>(&size), sizeof(size));
    stream.write(reinterpret_cast<const char*>(fbb.GetBufferPointer()), size);
}
