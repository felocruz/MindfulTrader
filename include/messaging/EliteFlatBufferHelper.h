/**
 * @file EliteFlatBufferHelper.h
 * @brief C++ FlatBuffer serialization helpers for elite protocol
 * @version v2.4 Elite
 * @date February 2, 2026
 *
 * Institutional-grade FlatBuffer builders and parsers for:
 * - PreFlightCheckRequest/Response (handshake)
 * - Heartbeat (liveness + model metrics)
 *
 * Zero-copy deserialization with type-safe routing.
 * Target latency: <100µs
 */

#pragma once

#include <zmq.hpp>
#include <string>
#include <optional>
#include <cstdint>
#include "generated/mts_schema_generated.h"
#include "generated/mts_schema_contract_generated.h"

namespace MTS {

/**
 * @struct HeartbeatData
 * All 11 Heartbeat fields for type-safe parsing
 */
struct HeartbeatData {
    uint64_t sequence_id;
    uint64_t timestamp_us;
    std::string sender;
    uint64_t uptime_ms;
    uint64_t message_count;
    float cpu_usage_pct;
    std::string model_status;       // "READY", "LOADING", "ERROR", "UNKNOWN"
    float last_inference_ms;        // Last inference latency
    float avg_inference_ms;         // 60s moving average latency
    int queue_depth;                // Pending inferences
    int error_count;                // Recent error count (60s window)
};

struct DiagnosticData {
    uint64_t timestamp_us;
    std::string sender;
    std::string message;
    float mahalanobis_distance;
    float observation_significance;
};

/**
 * @class EliteFlatBufferHelper
 * Server-side (C++) FlatBuffer protocol helpers
 */
class EliteFlatBufferHelper {
public:
    // ===== PREFLIGHT CHECK REQUEST (Python → C++) =====

    /**
     * @brief Parse PreFlightCheckRequest from ZMQ message
     * @param msg ZMQ message containing PreFlightCheckRequest FlatBuffer
     * @return Dictionary with fields: request_id, heartbeat_ms, validation_ms, sequence_id
     *         std::nullopt if parsing fails
     */
    static std::optional<std::map<std::string, std::string>> ParsePreFlightCheckRequest(
        const zmq::message_t& msg);

    /**
     * @brief Build PreFlightCheckResponse FlatBuffer
     * @param request_id Echo of original request_id for correlation
     * @param status PreFlightStatus enum value (READY, NOT_READY, or ERROR)
     * @param model_loaded Is AI model loaded?
     * @param system_ready Is system ready for trading?
     * @param reason Detailed reason (empty if READY)
     * @param cpp_state C++ state machine status
     * @param sequence_id Message sequence number
     * @return ZMQ message with serialized FlatBuffer
     */
    static zmq::message_t BuildPreFlightCheckResponse(
        const std::string& request_id,
        MTS::Schema::PreFlightStatus status,  // Typed enum (item 1 hardening)
        bool model_loaded,
        bool system_ready,
        const std::string& reason = "",
        const std::string& cpp_state = "READY",
        uint64_t sequence_id = 0);

    // ===== HEARTBEAT (C++ → Python) =====

    /**
     * @brief Parse Heartbeat FlatBuffer (all 11 fields)
     * Zero-copy deserialization using GetRoot() pattern
     * @param msg ZMQ message containing Heartbeat FlatBuffer
     * @return HeartbeatData with all fields, std::nullopt if parsing fails
     */
    static std::optional<HeartbeatData> ParseHeartbeat(const zmq::message_t& msg);

    /**
     * @brief Build Heartbeat FlatBuffer with all 11 fields
     * Sent by C++ every second to Python for liveness + model metrics
     * @param sender Component name ("MindfulTrader")
     * @param uptime_ms Process uptime
     * @param message_count Total messages sent
     * @param cpu_usage_pct CPU usage (0.0-100.0)
     * @param model_status "READY", "LOADING", "ERROR", "UNKNOWN"
     * @param last_inference_ms Last inference latency
     * @param avg_inference_ms 60s moving average latency
     * @param queue_depth Pending inferences
     * @param error_count Recent error count (60s window)
     * @param sequence_id Message sequence number
     * @return ZMQ message with serialized FlatBuffer
     */
    static zmq::message_t BuildHeartbeat(
        const std::string& sender,
        uint64_t uptime_ms,
        uint64_t message_count,
        float cpu_usage_pct,
        const std::string& model_status,
        float last_inference_ms,
        float avg_inference_ms,
        int queue_depth,
        int error_count,
        uint64_t sequence_id = 0);

    /**
     * @brief Build Diagnostic FlatBuffer envelope (C++ → Python GUI telemetry)
     * @param sender Component source identifier
     * @param message Diagnostic message payload
     * @param mahalanobis_distance Optional numeric metric channel
     * @param observation_significance Optional numeric metric channel
     * @return ZMQ message with serialized FlatBuffer envelope
     */
    static zmq::message_t BuildDiagnostic(
        const std::string& sender,
        const std::string& message,
        float mahalanobis_distance = 0.0f,
        float observation_significance = 0.0f);

    static zmq::message_t BuildDiagnosticWithGateEvent(
        const std::string& sender,
        const std::string& message,
        Schema::GateDirective directive,
        Schema::ReasonCode reason_code,
        int8_t action_id,
        float mahalanobis_distance = 0.0f,
        float observation_significance = 0.0f);

    static zmq::message_t BuildDiagnosticWithIntentEvent(
        const std::string& sender,
        const std::string& message,
        uint64_t intent_id,
        Schema::IntentState from_state,
        Schema::IntentState to_state,
        Schema::ReasonCode reason_code,
        int8_t action_id,
        uint64_t sequence_id,
        const std::string& source,
        float mahalanobis_distance = 0.0f,
        float observation_significance = 0.0f);

    static zmq::message_t BuildDiagnosticWithRecoveryEvent(
        const std::string& sender,
        const std::string& message,
        Schema::RecoveryState state,
        Schema::ReasonCode prior_reason_code,
        float mahalanobis_distance = 0.0f,
        float observation_significance = 0.0f);

    // ===== MESSAGE ENVELOPE UTILITIES =====

    /**
     * @brief Determine message type from MTS_Envelope
     * Type-safe routing without heuristics
     * @param msg ZMQ message containing MTS_Envelope
     * @return Message type enum, std::nullopt if parsing fails
     */
    static std::optional<Schema::Message> GetMessageType(const zmq::message_t& msg);

    /**
     * @brief Extract message payload from MTS_Envelope
     * @param msg ZMQ message containing MTS_Envelope
     * @return Message bytes (already deserialized for further parsing)
     */
    static std::optional<std::vector<uint8_t>> ExtractMessagePayload(const zmq::message_t& msg);

    // ===== SEQUENCE TRACKING =====

    /**
     * @brief Check for dropped messages in sequence
     * @param last_sequence Previous message sequence number
     * @param current_sequence Current message sequence number
     * @return Number of dropped messages (0 if no gaps)
     */
    static uint64_t GetDroppedMessageCount(uint64_t last_sequence, uint64_t current_sequence);
};

} // namespace MTS
