#pragma once

#include "MessageType.h"
#include <flatbuffers/flatbuffers.h>
#include <string>
#include <cstdint>
#include <memory>
#include "generated/mts_schema_contract_generated.h"

// Forward declarations (generated FlatBuffer types)
namespace MTS {
namespace Schema {
class Heartbeat;
}
}

namespace MTS {
namespace Messaging {

/**
 * @struct HeartbeatPayload
 * @brief In-memory representation of heartbeat data
 * 
 * Decoupled from FlatBuffer types for application-level use.
 * Safe to pass between threads without serialization overhead.
 */
struct HeartbeatPayload {
    uint64_t sequence_id;       // Sequential counter (for detecting drops)
    uint64_t timestamp_us;      // When heartbeat was created
    std::string sender;         // "C++" or "Python"
    uint64_t uptime_ms;         // Process uptime
    uint64_t message_count;     // Total messages sent by sender
    float cpu_usage_pct;        // CPU usage (optional)
};

/**
 * @class HeartbeatBuilder
 * @brief Serialize HeartbeatPayload → FlatBuffer binary
 * 
 * Ultra-simple starter message for validating transport/messaging layers.
 * Provides zero-copy building with FlatBuffer Object API.
 */
class HeartbeatBuilder {
public:
    /**
     * @brief Build heartbeat message from payload
     * @param builder FlatBuffer builder
     * @param payload In-memory heartbeat data
     * @return Offset of built message (pass to builder.Finish())
     */
    static flatbuffers::Offset<MTS::Schema::Heartbeat> Build(
        flatbuffers::FlatBufferBuilder& builder,
        const HeartbeatPayload& payload
    );

    /**
     * @brief Serialize complete heartbeat to buffer
     * @param payload In-memory data
    * @param out_buffer Output buffer (allocated by caller, should allow at least
    *        MTS::Schema::Contract::kBuilderCapacityHeartbeat bytes)
     * @return Number of bytes written
     */
    static size_t Serialize(const HeartbeatPayload& payload, uint8_t* out_buffer, size_t max_size);

    /**
     * @brief Convenience: Create default heartbeat (C++ sender, current time)
     * @return Populated HeartbeatPayload with reasonable defaults
     */
    static HeartbeatPayload CreateDefault();
};

/**
 * @class HeartbeatParser
 * @brief Deserialize FlatBuffer binary → HeartbeatPayload
 * 
 * Zero-copy parsing for performance.
 * Validates heartbeat format before extracting data.
 */
class HeartbeatParser {
public:
    /**
     * @brief Parse FlatBuffer heartbeat
     * @param data Serialized data pointer
     * @param size Data size in bytes
     * @return Parsed HeartbeatPayload, or empty if parsing failed
     */
    static HeartbeatPayload Parse(const uint8_t* data, size_t size);

    /**
     * @brief Extract metadata for routing
     * @param data Serialized data pointer
     * @param size Data size in bytes
     * @return MessageMetadata with HEARTBEAT type and extracted values
     */
    static MessageMetadata ExtractMetadata(const uint8_t* data, size_t size);

    /**
     * @brief Validate heartbeat structure
     * @param data Serialized data pointer
     * @param size Data size in bytes
     * @return true if structure is valid and complete
     */
    static bool Validate(const uint8_t* data, size_t size);
};

} // namespace Messaging
} // namespace MTS
