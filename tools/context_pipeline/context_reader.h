// tools/context_pipeline/context_reader.h
// Pure, ACSIL-independent .context binary-format reader. Zero-copy: every read
// is a pointer-cast over an mmap'd region, never a heap-allocated copy (DOD
// discipline). Replaces lbrnet's Python observation_vector_bulk_reader.py --
// ground truth for the wire format is src/LBRFileManager.cpp (the writer). See
// docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md §10.
#pragma once

#include "generated/mts_schema_generated.h"
#include "generated/mts_schema_contract_generated.h"
#include "flatbuffers/flatbuffers.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct ContextFileHandle {
    int fd = -1;
    const std::uint8_t* base = nullptr;
    std::size_t size = 0;
    std::size_t record_stream_offset = 0;  // first byte after magic + FileMetadata
    std::uint16_t schema_version = 0;
};

inline ContextFileHandle OpenContextFile(const std::string& path) {
    ContextFileHandle handle;
    handle.fd = ::open(path.c_str(), O_RDONLY);
    if (handle.fd < 0) {
        throw std::runtime_error("OpenContextFile: cannot open " + path);
    }
    struct stat st{};
    if (::fstat(handle.fd, &st) != 0 || st.st_size < 8) {
        ::close(handle.fd);
        throw std::runtime_error("OpenContextFile: cannot stat or file too small: " + path);
    }
    handle.size = static_cast<std::size_t>(st.st_size);
    void* mapped = ::mmap(nullptr, handle.size, PROT_READ, MAP_PRIVATE, handle.fd, 0);
    if (mapped == MAP_FAILED) {
        ::close(handle.fd);
        throw std::runtime_error("OpenContextFile: mmap failed for " + path);
    }
    handle.base = static_cast<const std::uint8_t*>(mapped);

    if (std::memcmp(handle.base, "LBRN", 4) != 0) {
        ::munmap(mapped, handle.size);
        ::close(handle.fd);
        throw std::runtime_error("OpenContextFile: bad magic header (expected 'LBRN'): " + path);
    }

    std::uint32_t meta_size = 0;
    std::memcpy(&meta_size, handle.base + 4, sizeof(meta_size));
    const std::size_t meta_offset = 8;
    if (meta_size == 0 || meta_offset + meta_size > handle.size) {
        ::munmap(mapped, handle.size);
        ::close(handle.fd);
        throw std::runtime_error("OpenContextFile: truncated or missing FileMetadata: " + path);
    }

    const auto* meta = flatbuffers::GetRoot<MTS::Training::FileMetadata>(handle.base + meta_offset);
    handle.schema_version = meta->schema_version();
    if (handle.schema_version != MTS::Schema::Contract::kSchemaVersion) {
        ::munmap(mapped, handle.size);
        ::close(handle.fd);
        throw std::runtime_error(
            "OpenContextFile: schema_version mismatch for " + path + " -- file stamped " +
            std::to_string(handle.schema_version) + ", this tool compiled for " +
            std::to_string(MTS::Schema::Contract::kSchemaVersion) +
            ". Refusing to reinterpret potentially incompatible ObservationData bytes "
            "(brainstorm doc §10.2) -- re-collect this file or write a dedicated migration, "
            "never force-process it.");
    }

    handle.record_stream_offset = meta_offset + meta_size;
    return handle;
}

inline void CloseContextFile(ContextFileHandle& handle) {
    if (handle.base != nullptr) {
        ::munmap(const_cast<std::uint8_t*>(handle.base), handle.size);
        handle.base = nullptr;
    }
    if (handle.fd >= 0) {
        ::close(handle.fd);
        handle.fd = -1;
    }
}

struct ParsedObservation {
    const MTS::Schema::ObservationData* observation;
    const MTS::Schema::RiskGateContext* risk_gate_context;  // nullptr if not present in this record
    std::int64_t timestamp_us;
    std::uint64_t sequence_id;
};

// record_payload must point at the start of a size-prefixed record's FlatBuffer
// payload (i.e. 4 bytes past that record's own length prefix). Zero-copy: every
// field below is a pointer/value read directly off the mmap'd buffer.
inline ParsedObservation ReadMarketObservation(const std::uint8_t* record_payload) {
    const auto* mo = flatbuffers::GetRoot<MTS::Schema::MarketObservation>(record_payload);
    ParsedObservation parsed;
    parsed.observation = mo->observation();
    parsed.risk_gate_context = mo->risk_gate_context();
    parsed.timestamp_us = mo->timestamp_us();
    parsed.sequence_id = mo->sequence_id();
    return parsed;
}

struct PairingState {
    const std::uint8_t* pending_obs_record = nullptr;
    std::uint64_t pending_seq = 0;
    bool has_pending = false;
    std::uint64_t last_seq_id = 0;
    bool has_last_seq_id = false;
};

struct ReadCounters {
    std::size_t market_records = 0;
    std::size_t system_records = 0;
    std::size_t pair_attempts = 0;
    std::size_t aligned_pairs = 0;
    std::size_t sequence_mismatches = 0;
    std::size_t sequence_regressions = 0;
    std::size_t unpaired_market_records = 0;
    std::size_t unpaired_system_records = 0;
};

struct PairedRecord {
    ParsedObservation observation;
    float bars_since_last_update;
};

// Mirrors observation_vector_bulk_reader.py's _process_one_record() exactly:
// even record_index => MarketObservation (staged as "pending"), odd => SystemState
// (attempts to complete the pending pair). Flat POD state only, no allocation.
inline bool ProcessOneRecord(
    std::size_t record_index, const std::uint8_t* payload, PairingState& state,
    ReadCounters& counters, PairedRecord* out) {
    if (record_index % 2 == 0) {
        ++counters.market_records;
        state.pending_obs_record = payload;
        const auto* mo = flatbuffers::GetRoot<MTS::Schema::MarketObservation>(payload);
        state.pending_seq = mo->sequence_id();
        state.has_pending = true;
        return false;
    }

    ++counters.system_records;
    ++counters.pair_attempts;
    const auto* ss = flatbuffers::GetRoot<MTS::Schema::SystemState>(payload);
    const std::uint64_t ss_seq = ss->sequence_id();

    if (!state.has_pending || state.pending_seq != ss_seq) {
        ++counters.sequence_mismatches;
        ++counters.unpaired_market_records;
        state.has_pending = false;
        return false;
    }

    ++counters.aligned_pairs;
    if (state.has_last_seq_id && state.pending_seq <= state.last_seq_id) {
        ++counters.sequence_regressions;
    }
    state.last_seq_id = state.pending_seq;
    state.has_last_seq_id = true;

    out->observation = ReadMarketObservation(state.pending_obs_record);
    out->bars_since_last_update = ss->bars_since_last_update();
    state.has_pending = false;
    return true;
}

// Unbounded full-file read. Calls on_pair once per aligned pair, in stream
// order -- caller owns accumulation (context_to_parquet.cpp's Arrow columnar
// builders, context_validate.cpp's plain column buffers), keeping this header
// free of any Arrow dependency.
inline ReadCounters ReadAllRecords(
    const ContextFileHandle& handle,
    void (*on_pair)(const PairedRecord&, void* user_data), void* user_data) {
    ReadCounters counters;
    PairingState state;
    std::size_t offset = handle.record_stream_offset;
    std::size_t record_index = 0;

    while (offset + 4 <= handle.size) {
        std::uint32_t payload_size = 0;
        std::memcpy(&payload_size, handle.base + offset, sizeof(payload_size));
        offset += 4;
        if (payload_size == 0 || offset + payload_size > handle.size) {
            break;  // torn/end-of-stream record, matches the Python reader's own truncation handling
        }
        const std::uint8_t* payload = handle.base + offset;
        offset += payload_size;

        PairedRecord rec;
        if (ProcessOneRecord(record_index, payload, state, counters, &rec)) {
            on_pair(rec, user_data);
        }
        ++record_index;
    }
    return counters;
}

inline ReadCounters ReadBoundedHead(
    const ContextFileHandle& handle, std::size_t max_pairs,
    void (*on_pair)(const PairedRecord&, void*), void* user_data) {
    ReadCounters counters;
    PairingState state;
    std::size_t offset = handle.record_stream_offset;
    std::size_t record_index = 0;
    std::size_t emitted = 0;

    while (offset + 4 <= handle.size && emitted < max_pairs) {
        std::uint32_t payload_size = 0;
        std::memcpy(&payload_size, handle.base + offset, sizeof(payload_size));
        offset += 4;
        if (payload_size == 0 || offset + payload_size > handle.size) {
            break;
        }
        const std::uint8_t* payload = handle.base + offset;
        offset += payload_size;

        PairedRecord rec;
        if (ProcessOneRecord(record_index, payload, state, counters, &rec)) {
            on_pair(rec, user_data);
            ++emitted;
        }
        ++record_index;
    }
    return counters;
}

// Two-phase, matching observation_vector_bulk_reader.py's _read_bounded_tail:
// Phase 1 is a cheap header-only byte scan (no FlatBuffer parsing) locating the
// last 2*max_pairs candidate record offsets; Phase 2 parses only those.
inline ReadCounters ReadBoundedTail(
    const ContextFileHandle& handle, std::size_t max_pairs,
    void (*on_pair)(const PairedRecord&, void*), void* user_data) {
    struct Candidate { std::size_t record_index; const std::uint8_t* payload; };

    std::vector<Candidate> ring;
    ring.reserve(max_pairs * 2);
    std::size_t offset = handle.record_stream_offset;
    std::size_t record_index = 0;
    std::size_t ring_head = 0;  // next write slot, wraps -- avoids per-record allocation

    while (offset + 4 <= handle.size) {
        std::uint32_t payload_size = 0;
        std::memcpy(&payload_size, handle.base + offset, sizeof(payload_size));
        offset += 4;
        if (payload_size == 0 || offset + payload_size > handle.size) {
            break;
        }
        Candidate c{record_index, handle.base + offset};
        if (ring.size() < max_pairs * 2) {
            ring.push_back(c);
        } else {
            ring[ring_head] = c;
            ring_head = (ring_head + 1) % ring.size();
        }
        offset += payload_size;
        ++record_index;
    }

    // Re-order the ring buffer back into stream order before Phase 2.
    std::vector<Candidate> ordered;
    ordered.reserve(ring.size());
    for (std::size_t i = 0; i < ring.size(); ++i) {
        ordered.push_back(ring[(ring_head + i) % ring.size()]);
    }

    ReadCounters counters;
    PairingState state;
    std::vector<PairedRecord> tail_ring;
    tail_ring.reserve(max_pairs);
    std::size_t tail_head = 0;
    std::size_t emitted = 0;

    for (const auto& c : ordered) {
        PairedRecord rec;
        if (ProcessOneRecord(c.record_index, c.payload, state, counters, &rec)) {
            if (tail_ring.size() < max_pairs) {
                tail_ring.push_back(rec);
            } else {
                tail_ring[tail_head] = rec;
                tail_head = (tail_head + 1) % max_pairs;
            }
            ++emitted;
        }
    }

    const std::size_t n = tail_ring.size();
    for (std::size_t i = 0; i < n; ++i) {
        on_pair(tail_ring[(tail_head + i) % (n == 0 ? 1 : n)], user_data);
    }
    return counters;
}

struct ResumePoint {
    std::size_t resume_offset;
    std::uint64_t last_seq_id;
    bool has_last_seq_id;
};

// Like ReadAllRecords, but starts at an arbitrary byte offset (mid-stream resume)
// and reports where the caller should resume from next time. resume_offset only
// ever advances to a position immediately after a COMPLETE record -- a torn
// trailing record at end-of-file is never counted as consumed, matching
// read_context_observations_chunked's documented guarantee (no duplication or
// drop on the next call once that record is completed).
inline std::pair<ReadCounters, ResumePoint> ReadAllRecordsResumable(
    const ContextFileHandle& handle, std::size_t start_offset,
    std::uint64_t start_last_seq_id, bool has_start_last_seq_id,
    void (*on_pair)(const PairedRecord&, void*), void* user_data) {
    ReadCounters counters;
    PairingState state;
    state.last_seq_id = start_last_seq_id;
    state.has_last_seq_id = has_start_last_seq_id;

    std::size_t offset = start_offset;
    std::size_t record_index = 0;
    std::size_t resume_offset = start_offset;

    while (offset + 4 <= handle.size) {
        std::uint32_t payload_size = 0;
        std::memcpy(&payload_size, handle.base + offset, sizeof(payload_size));
        if (payload_size == 0 || offset + 4 + payload_size > handle.size) {
            break;  // torn/end-of-stream -- resume_offset stays at this record's start
        }
        const std::uint8_t* payload = handle.base + offset + 4;
        offset += 4 + payload_size;

        PairedRecord rec;
        if (ProcessOneRecord(record_index, payload, state, counters, &rec)) {
            on_pair(rec, user_data);
        }
        resume_offset = offset;
        ++record_index;
    }

    ResumePoint resume{resume_offset, state.last_seq_id, state.has_last_seq_id};
    return {counters, resume};
}
