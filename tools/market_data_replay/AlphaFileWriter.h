// AlphaFileWriter.h -- standalone, ACSIL-free `.alpha` file writer for
// tools/market_data_replay/. LBRFileManager (include/LBRFileManager.h,
// src/LBRFileManager.cpp) cannot be linked into a standalone Linux tool: its
// .cpp transitively includes MindfulTrader_Precompiled.h -> sierrachart.h ->
// windows.h (same genuinely-Windows-only blocker ContextFileWriter.h already
// worked around for `.context`). This is a deliberate, faithful duplicate of
// LBRFileManager's `.alpha`-only write path (Open/LogAlpha/Close), not a
// reimplementation from scratch -- every byte-format detail (magic header,
// FileMetadata, size-prefixed TrainingEvent records, flush cadence) is
// copied verbatim from src/LBRFileManager.cpp so output stays byte-
// compatible with any real `.alpha` reader. Only the `.context`/
// `.imbalance.context` streams are omitted -- this writer is `.alpha`-only
// by design (docs/superpowers/specs/2026-09-16-market-data-replay-alpha-
// generator-spec.md §3 item 4).
//
// KEEP IN SYNC with src/LBRFileManager.cpp's Open()/LogAlphaUnlocked()/
// WriteToStream()/WriteMagicHeader()/WriteFileMetadata() if that file's wire
// format ever changes -- there is no automated check tying these together
// (same caveat ContextFileWriter.h already carries).

#pragma once

#include "generated/mts_schema_contract_generated.h"
#include "generated/mts_schema_generated.h"
#include "flatbuffers/flatbuffers.h"

#include <cstdint>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>

class AlphaFileWriter {
public:
    // path + ".alpha" is opened, matching LBRFileManager::Open()'s own
    // path+".alpha" convention.
    bool Open(const std::string& path, const std::string& symbol) {
        m_alphaStream.close();
        m_alphaStream.clear();
        m_alphaStream.open(path + ".alpha", std::ios::binary | std::ios::out | std::ios::trunc);
        if (!m_alphaStream.is_open()) {
            return false;
        }
        WriteMagicHeader();
        WriteFileMetadata(symbol, "ALPHA_v2.5_TRAINING_EVENT");
        m_isOpen = true;
        m_sequenceId = 0;
        m_recordsSinceFlush = 0;
        return true;
    }

    void Close() {
        if (m_alphaStream.is_open()) {
            m_alphaStream.flush();
            m_alphaStream.close();
        }
        m_isOpen = false;
        m_recordsSinceFlush = 0;
    }

    // Byte-identical to LBRFileManager::LogAlphaUnlocked()'s write path.
    // `event.sequence_id` is overwritten here, matching production's own
    // "caller-supplied event, writer owns the sequence id" contract.
    void LogAlpha(MTS::Training::TrainingEventT& event) {
        if (!m_isOpen || !m_alphaStream.is_open()) {
            return;
        }
        event.sequence_id = m_sequenceId++;

        m_fbb.Clear();
        auto training_event_offset = MTS::Training::CreateTrainingEvent(m_fbb, &event);
        m_fbb.Finish(training_event_offset);

        WriteToStream(m_fbb.GetBufferPointer(), m_fbb.GetSize());
    }

private:
    void WriteMagicHeader() {
        const char magic[4] = {'L', 'B', 'R', 'N'};
        m_alphaStream.write(magic, 4);
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
        m_alphaStream.write(reinterpret_cast<const char*>(&size), sizeof(size));
        m_alphaStream.write(reinterpret_cast<const char*>(fbb.GetBufferPointer()), size);
    }

    // Mirrors LBRFileManager::WriteToStream()'s own combined-write/flush-
    // cadence logic exactly (8192-byte stack-buffer fast path, else 2 writes).
    void WriteToStream(uint8_t* buf, size_t size) {
        if (size > std::numeric_limits<uint32_t>::max()) {
            return;
        }
        const uint32_t length = static_cast<uint32_t>(size);
        const size_t total = sizeof(length) + size;

        if (total <= 8192) {
            uint8_t combined[8192];
            std::memcpy(combined, &length, sizeof(length));
            std::memcpy(combined + sizeof(length), buf, size);
            m_alphaStream.write(reinterpret_cast<const char*>(combined), total);
        } else {
            m_alphaStream.write(reinterpret_cast<const char*>(&length), sizeof(length));
            m_alphaStream.write(reinterpret_cast<const char*>(buf), size);
        }

        if (!m_alphaStream.good()) {
            return;
        }

        ++m_recordsSinceFlush;
        if (m_recordsSinceFlush >= kFlushEveryRecords) {
            m_alphaStream.flush();
            m_recordsSinceFlush = 0;
        }
    }

    static constexpr uint32_t kFlushEveryRecords = 512;

    std::ofstream m_alphaStream;
    flatbuffers::FlatBufferBuilder m_fbb{2048};
    bool m_isOpen = false;
    uint64_t m_sequenceId = 0;
    uint32_t m_recordsSinceFlush = 0;
};
