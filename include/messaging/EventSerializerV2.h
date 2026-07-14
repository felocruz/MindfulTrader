#pragma once

#include <vector>
#include <cstdint>
#include <atomic>
#include <memory>
#include <optional>
#include "flatbuffers/flatbuffers.h"
#include "generated/mts_schema_generated.h"

// Forward declarations
class IndicatorManager;

/**
 * EventSerializer: Elite v2.4 Event Table Serialization
 *
 * Purpose:
 * - Serialize live trading Events (84 fields) using FlatBuffer Event table
 * - Each Event represents a single significant indicator change (intrabar frequency)
 * - Includes global sequence_id for event ordering and drop detection
 * - Generates ~500-700 byte binary for real-time ZMQ streaming
 *
 * Architecture:
 * - Singleton pattern (one serializer per process)
 * - Reuses FlatBufferBuilder for efficiency
 * - Populates all 84 Event fields from IndicatorManager state
 * - No outcome fields (training labels added by Python CHL labeler)
 *
 * Performance:
 * - Serialization: 5-15µs per event
 * - Binary size: 500-700 bytes
 * - Memory: ~2KB for builder + buffers
 *
 * Integration:
 * - Called from SendEventFlatBuffer() when IsDirty()
 * - Returns binary ready for ZmqPublisher (port 5555)
 * - Python deserializes via Event.GetRootAsEvent()
 * - Training pipeline extends Event → TrainingEvent (add outcomes)
 *
 * Features:
 * - Sequence ID counter for unique event identification
 * - Full 84-field Event table (no outcomes yet)
 * - Circular buffer support for disconnect recovery and replay
 * - Elite Protocol integration (heartbeat + message routing)
 */
class EventSerializer {
public:
    static EventSerializer& Instance();

    /**
     * SerializeEvent: Main serialization for live trading events
     *
     * Params:
     *   manager: IndicatorManager with current indicator state
     *   bar_index: Current bar index (for bar_index field)
     *   timestamp_us: Microsecond precision timestamp
     *   sequence_id: Global monotonic counter (must increment per call)
     *
     * Returns:
     *   Binary FlatBuffer Event (500-700 bytes), ready for ZMQ PUB
     *
     * Throws:
     *   std::exception on serialization failure
     */
    std::vector<uint8_t> SerializeEvent(
        const IndicatorManager& manager,
        int32_t bar_index,
        uint64_t timestamp_us,
        uint64_t sequence_id
    );

    /**
     * ValidateEventBinary: Check if binary is valid Event
     *
     * Returns:
     *   true if binary is well-formed FlatBuffer Event
     *   false if corrupt/invalid
     */
    bool ValidateEventBinary(const std::vector<uint8_t>& binary) const;

    // Statistics for monitoring
    uint64_t GetEventCount() const { return m_eventCount.load(); }
    int32_t GetLastSerializationTimeUs() const { return m_lastSerializationTimeUs.load(); }
    int32_t GetLastEventSizeBytes() const { return m_lastEventSizeBytes.load(); }

private:
    EventSerializerV2();
    ~EventSerializerV2();

    // Populate Event fields from IndicatorManager
    void PopulateIndicatorEnums(
        MTS::Schema::EventBuilder& builder,
        const IndicatorManager& manager
    );

    void PopulateContextFields(
        MTS::Schema::EventBuilder& builder,
        const IndicatorManager& manager
    );

    void PopulateFeatureVectors(
        MTS::Schema::EventBuilder& builder,
        const IndicatorManager& manager
    );

    void PopulateNormalizedFields(
        MTS::Schema::EventBuilder& builder,
        const IndicatorManager& manager
    );

    // Temporary buffers for vector fields (changed_keys, features)
    std::vector<::flatbuffers::Offset<::flatbuffers::String>> m_changedKeysOffsets;
    std::vector<float> m_featuresBuffer;
    std::vector<float> m_observationBuffer;

    // Reusable FlatBufferBuilder for efficiency
    std::unique_ptr<::flatbuffers::FlatBufferBuilder> m_fbb;

    // Statistics (atomic for thread-safe reads)
    std::atomic<uint64_t> m_eventCount{0};
    std::atomic<int32_t> m_lastSerializationTimeUs{0};
    std::atomic<int32_t> m_lastEventSizeBytes{0};
};
