#pragma once

#include <vector>
#include <cstddef>
#include <cstdint>
#include <atomic>
#include <memory>
#include <deque>
#include "flatbuffers/flatbuffers.h"
#include "generated/mts_schema_generated.h"

// Forward declarations
class IndicatorManager;

/**
 * EventSerializer: Strict FlatBuffer Event Emitter
 *
 * Purpose:
 * - Serialize live trading Event payloads using FlatBuffer Event table
 * - Emit deterministic, fixed-schema packets from typed indicator/context state
 * - Preserve sequence ordering and low-latency publish path
 *
 * Architecture:
 * - Singleton pattern (one serializer per process)
 * - Reuses FlatBufferBuilder for efficiency
 * - Populates schema fields from IndicatorManager + context snapshot
 * - No string-key transport or dynamic schema fields in hot path
 *
 * Performance:
 * - Serialization: 5-15µs per event
 * - Binary size: 500-700 bytes
 * - Memory: ~2KB for builder + buffers
 *
 * Integration:
 * - Called from SendEventFlatBuffer() when IsDirty()
 * - Returns binary ready for ZmqPublisher (port 5555)
 * - Python consumers deserialize via Event.GetRootAsEvent()
 *
 * Features:
 * - Sequence ID counter for unique event identification
 * - Full Event table emission per current schema contract
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
     *   observation: Optional pointer to ObservationData struct (added Elite v2.5)
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
        uint64_t sequence_id,
        const MTS::Schema::ObservationData* observation = nullptr,
        const MTS::Schema::AsymmetryContext* asymmetry_context = nullptr
    );

    /**
     * SerializeEventInPlace: Hot-path API exposing builder-owned buffer directly.
     *
     * The returned pointer remains valid until the next serialization call.
     * Caller must copy or enqueue data before subsequent SerializeEvent* invocation.
     */
    bool SerializeEventInPlace(
        const IndicatorManager& manager,
        int32_t bar_index,
        uint64_t timestamp_us,
        uint64_t sequence_id,
        const uint8_t*& out_buffer,
        size_t& out_size,
        const MTS::Schema::ObservationData* observation = nullptr,
        const MTS::Schema::AsymmetryContext* asymmetry_context = nullptr
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
    EventSerializer();
    ~EventSerializer();

    // ELITE: Context manager snapshot (avoid repeated singleton calls)
    struct ContextSnapshot {
        float log_event_velocity;
        float delta_t_log;
        float tau_100_log;
        int8_t hmm_state;
        int8_t market_climate;
        int8_t oscillator_310;
        float corr_es_zn;
        float corr_es_dx;
        int8_t zn_trend;
        int8_t dx_trend;
        float corr_es_zn_delta;
        float corr_es_zn_accel;
        float corr_es_dx_delta;
        float corr_es_dx_accel;
        // prev_high/prev_low/prev_day_high/prev_day_low/prev_four_bar_high/
        // prev_four_bar_low removed (Task 10, indicator-manager-dod-soa plan):
        // these duplicated IndicatorManager::GetTickCompanionValues()'s own
        // ShortMarketAction/daily-cache reads. SerializeEventInPlace now reads
        // them once from GetTickCompanionValues() instead of twice.
    };
    ContextSnapshot SnapshotContext(const IndicatorManager& manager, uint64_t timestamp_us);
    void UpdateTemporalPhysics(uint64_t timestamp_us, float& out_delta_t_log, float& out_tau_100_log);

    // Reusable FlatBufferBuilder for efficiency
    std::unique_ptr<::flatbuffers::FlatBufferBuilder> m_fbb;

    // Statistics (atomic for thread-safe reads)
    std::atomic<uint64_t> m_eventCount{0};
    std::atomic<int32_t> m_lastSerializationTimeUs{0};
    std::atomic<int32_t> m_lastEventSizeBytes{0};

    // Event-physics cache used to compute delta_t_log and tau_100_log on the live stream.
    uint64_t m_lastEventTimestampUs = 0;
    std::deque<uint64_t> m_recentDeltaUs;

    static constexpr size_t kTauWindowSize = 100;
};
