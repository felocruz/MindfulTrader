#pragma once

#include <string>
#include <cstdint>
#include <optional>
#include <functional>
#include <map>
#include "generated/mts_schema_contract_generated.h"

namespace MTS {
namespace Messaging {

/**
 * @enum MessageType
 * @brief Central message type registry for routing and dispatch
 * 
 * Canonical values are anchored to FlatBuffer MessageType enum constants
 * from mts_schema.fbs via Contract helpers. Additional values represent
 * local-only categories not present in the schema routing enum.
 */
enum class MessageType : uint8_t {
    UNKNOWN = 255,
    HEARTBEAT = static_cast<uint8_t>(MTS::Schema::Contract::kRoutingHeartbeat),
    OBSERVATION = static_cast<uint8_t>(MTS::Schema::Contract::kRoutingObservation),
    HMM_COMMAND = static_cast<uint8_t>(MTS::Schema::Contract::kRoutingHmmCommand),
    DIAGNOSTIC = static_cast<uint8_t>(MTS::Schema::Contract::kRoutingDiagnostic),

    // Local-only message categories (not encoded in Schema::MessageType).
    TRADE_COMMAND = 100,
    PYTHON_PREDICTION = 101,
};

static_assert(
    static_cast<uint8_t>(MessageType::HEARTBEAT) ==
    static_cast<uint8_t>(MTS::Schema::Contract::kRoutingHeartbeat),
    "MessageType::HEARTBEAT must match schema routing enum");
static_assert(
    static_cast<uint8_t>(MessageType::OBSERVATION) ==
    static_cast<uint8_t>(MTS::Schema::Contract::kRoutingObservation),
    "MessageType::OBSERVATION must match schema routing enum");
static_assert(
    static_cast<uint8_t>(MessageType::HMM_COMMAND) ==
    static_cast<uint8_t>(MTS::Schema::Contract::kRoutingHmmCommand),
    "MessageType::HMM_COMMAND must match schema routing enum");
static_assert(
    static_cast<uint8_t>(MessageType::DIAGNOSTIC) ==
    static_cast<uint8_t>(MTS::Schema::Contract::kRoutingDiagnostic),
    "MessageType::DIAGNOSTIC must match schema routing enum");

/**
 * @brief Convert MessageType to string
 * @param type Message type
 * @return Human-readable name ("HEARTBEAT", "OBSERVATION", etc.)
 */
inline std::string MessageTypeToString(MessageType type) {
    switch (type) {
        case MessageType::UNKNOWN:          return "UNKNOWN";
        case MessageType::HEARTBEAT:        return "HEARTBEAT";
        case MessageType::OBSERVATION:      return "OBSERVATION";
        case MessageType::HMM_COMMAND:      return "HMM_COMMAND";
        case MessageType::TRADE_COMMAND:    return "TRADE_COMMAND";
        case MessageType::DIAGNOSTIC:       return "DIAGNOSTIC";
        case MessageType::PYTHON_PREDICTION: return "PYTHON_PREDICTION";
        default:                            return "UNKNOWN";
    }
}

/**
 * @brief Parse string to MessageType
 * @param str String representation
 * @return MessageType, or UNKNOWN if not recognized
 */
inline MessageType StringToMessageType(const std::string& str) {
    if (str == "HEARTBEAT")        return MessageType::HEARTBEAT;
    if (str == "OBSERVATION")      return MessageType::OBSERVATION;
    if (str == "HMM_COMMAND")      return MessageType::HMM_COMMAND;
    if (str == "TRADE_COMMAND")    return MessageType::TRADE_COMMAND;
    if (str == "DIAGNOSTIC")       return MessageType::DIAGNOSTIC;
    if (str == "PYTHON_PREDICTION") return MessageType::PYTHON_PREDICTION;
    return MessageType::UNKNOWN;
}

inline std::optional<MTS::Schema::Contract::RoutingMessageType> ToSchemaMessageType(MessageType type) {
    switch (type) {
        case MessageType::HEARTBEAT: return MTS::Schema::Contract::kRoutingHeartbeat;
        case MessageType::OBSERVATION: return MTS::Schema::Contract::kRoutingObservation;
        case MessageType::HMM_COMMAND: return MTS::Schema::Contract::kRoutingHmmCommand;
        case MessageType::DIAGNOSTIC: return MTS::Schema::Contract::kRoutingDiagnostic;
        default: return std::nullopt;
    }
}

inline MessageType FromSchemaMessageType(MTS::Schema::Contract::RoutingMessageType type) {
    switch (type) {
        case MTS::Schema::Contract::kRoutingHeartbeat: return MessageType::HEARTBEAT;
        case MTS::Schema::Contract::kRoutingObservation: return MessageType::OBSERVATION;
        case MTS::Schema::Contract::kRoutingHmmCommand: return MessageType::HMM_COMMAND;
        case MTS::Schema::Contract::kRoutingDiagnostic: return MessageType::DIAGNOSTIC;
        default: return MessageType::UNKNOWN;
    }
}

/**
 * @struct MessageMetadata
 * @brief Common metadata attached to all messages
 * 
 * Used for routing decisions, latency measurement, and diagnostics.
 * Extracted during deserialization before routing to handler.
 */
struct MessageMetadata {
    MessageType type;              // Message type (enum)
    uint64_t timestamp_us;         // When message was created (microseconds)
    uint64_t sequence_id;          // Message sequence number
    std::string sender;            // Originating sender ("C++", "Python", etc.)
    uint64_t size_bytes;           // Serialized message size
    uint64_t latency_us;           // Time from creation to now (microseconds)
    
    std::string ToString() const;
};

} // namespace Messaging
} // namespace MTS
