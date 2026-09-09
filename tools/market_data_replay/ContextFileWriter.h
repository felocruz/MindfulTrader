// ContextFileWriter.h -- standalone, ACSIL-free `.context` file writer for
// tools/market_data_replay/. LBRFileManager (include/LBRFileManager.h,
// src/LBRFileManager.cpp) cannot be linked into a standalone Linux tool: its
// .cpp transitively includes MindfulTrader_Precompiled.h -> sierrachart.h ->
// windows.h (confirmed 2026-09-08 -- genuinely Windows-only, not just a
// missing include path). This is a deliberate, faithful duplicate of
// LBRFileManager's `.context`-only write path (Open/LogContext/Close), not a
// reimplementation from scratch -- every byte-format detail (magic header,
// FileMetadata, MO+SS size-prefixed pairing, flush cadence) is copied
// verbatim from src/LBRFileManager.cpp so output stays byte-compatible with
// tools/context_pipeline/context_reader.h. Only the `.alpha`/
// `.imbalance.context` streams are omitted -- this tool is `.context`-only
// by design (docs/superpowers/specs/2026-09-08-offline-context-generator-
// spec.md §0/§3d).
//
// KEEP IN SYNC with src/LBRFileManager.cpp's Open()/LogContextUnlocked()/
// WriteToStream()/WriteMagicHeader()/WriteFileMetadata() if that file's wire
// format ever changes -- there is no automated check tying these together.

#pragma once

#include "generated/mts_schema_contract_generated.h"
#include "generated/mts_schema_generated.h"
#include "flatbuffers/flatbuffers.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>

class ContextFileWriter {
public:
    // path + ".context" is opened, matching LBRFileManager::Open()'s own
    // path+".context" convention (so tools/context_pipeline/context_reader.h
    // needs no special-casing for offline-generated files).
    bool Open(const std::string& path, const std::string& symbol) {
        m_contextStream.close();
        m_contextStream.clear();
        m_contextStream.open(path + ".context", std::ios::binary | std::ios::out | std::ios::trunc);
        if (!m_contextStream.is_open()) {
            return false;
        }
        WriteMagicHeader();
        WriteFileMetadata(symbol, "CONTEXT_v2.5_MO_SS");
        m_isOpen = true;
        m_sequenceId = 0;
        m_recordsSinceFlush = 0;
        return true;
    }

    void Close() {
        if (m_contextStream.is_open()) {
            m_contextStream.flush();
            m_contextStream.close();
        }
        m_isOpen = false;
        m_recordsSinceFlush = 0;
    }

    // Byte-identical to LBRFileManager::LogContextUnlocked()'s MO+SS
    // combined-write path.
    void LogContext(
        const MTS::Schema::ObservationData& obs,
        const MTS::Schema::AsymmetryContext& ctx,
        uint64_t timestamp_us,
        float bars_since_last_update,
        const MTS::Schema::RiskGateContextT* risk_gate_context = nullptr) {
        if (!m_isOpen || !m_contextStream.is_open()) {
            return;
        }
        const uint64_t sequence_id = m_sequenceId++;

        m_fbb.Clear();
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
            0,  // daily_bias_enum: unused on the .context path
            rgc_offset);
        m_fbb.Finish(mo_loc);

        const uint32_t mo_size = static_cast<uint32_t>(m_fbb.GetSize());
        const uint8_t* mo_ptr = m_fbb.GetBufferPointer();
        const size_t mo_total = sizeof(uint32_t) + mo_size;
        uint8_t staged[512];
        std::memcpy(staged, &mo_size, sizeof(uint32_t));
        std::memcpy(staged + sizeof(uint32_t), mo_ptr, mo_size);

        m_fbb.Clear();
        auto ss_loc = MTS::Schema::CreateSystemState(
            m_fbb, static_cast<int64_t>(timestamp_us), sequence_id, bars_since_last_update);
        m_fbb.Finish(ss_loc);

        const uint32_t ss_size = static_cast<uint32_t>(m_fbb.GetSize());
        const uint8_t* ss_ptr = m_fbb.GetBufferPointer();
        const size_t ss_total = sizeof(uint32_t) + ss_size;
        const size_t combined_size = mo_total + ss_total;

        if (combined_size <= sizeof(staged)) {
            std::memcpy(staged + mo_total, &ss_size, sizeof(uint32_t));
            std::memcpy(staged + mo_total + sizeof(uint32_t), ss_ptr, ss_size);
            m_contextStream.write(reinterpret_cast<const char*>(staged), combined_size);
        } else {
            m_contextStream.write(reinterpret_cast<const char*>(staged), mo_total);
            uint32_t ss_len = ss_size;
            m_contextStream.write(reinterpret_cast<const char*>(&ss_len), sizeof(ss_len));
            m_contextStream.write(reinterpret_cast<const char*>(ss_ptr), ss_size);
        }

        m_recordsSinceFlush += 2;
        if (m_recordsSinceFlush >= kFlushEveryRecords) {
            m_contextStream.flush();
            m_recordsSinceFlush = 0;
        }
    }

private:
    void WriteMagicHeader() {
        const char magic[4] = {'L', 'B', 'R', 'N'};
        m_contextStream.write(magic, 4);
    }

    void WriteFileMetadata(const std::string& symbol, const std::string& timeframe) {
        flatbuffers::FlatBufferBuilder fbb(1024);
        auto symbol_offset = fbb.CreateString(symbol);
        auto timeframe_offset = fbb.CreateString(timeframe);
        MTS::Training::FileMetadataBuilder metadata_builder(fbb);
        metadata_builder.add_symbol(symbol_offset);
        metadata_builder.add_timeframe(timeframe_offset);
        metadata_builder.add_schema_version(MTS::Schema::Contract::kSchemaVersion);
        auto metadata_offset = metadata_builder.Finish();
        fbb.Finish(metadata_offset);

        const uint32_t size = static_cast<uint32_t>(fbb.GetSize());
        m_contextStream.write(reinterpret_cast<const char*>(&size), sizeof(size));
        m_contextStream.write(reinterpret_cast<const char*>(fbb.GetBufferPointer()), size);
    }

    static constexpr uint32_t kFlushEveryRecords = 512;

    std::ofstream m_contextStream;
    flatbuffers::FlatBufferBuilder m_fbb{2048};
    bool m_isOpen = false;
    uint64_t m_sequenceId = 0;
    uint32_t m_recordsSinceFlush = 0;
};
