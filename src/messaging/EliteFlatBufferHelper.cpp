/**
 * @file EliteFlatBufferHelper.cpp
 * @brief C++ FlatBuffer parsing implementation
 * @version v2.4 Elite
 */

#include "messaging/EliteFlatBufferHelper.h"
#include "Logger.h"
#include <cstring>

namespace MTS {

// ===== PREFLIGHT CHECK REQUEST PARSING (Python → C++) =====

std::optional<std::map<std::string, std::string>> EliteFlatBufferHelper::ParsePreFlightCheckRequest(
    const zmq::message_t& msg)
{
    try {
        const Schema::MTS_Envelope* envelope =
            Schema::Contract::GetVerifiedEnvelope(msg.data(), msg.size());
        if (!envelope) {
            Logger::getInstance().log("ERROR: EliteFlatBufferHelper: Invalid envelope");
            return std::nullopt;
        }

        // Check message type
        if (envelope->data_type() != Schema::Contract::kEnvelopePreFlightCheckRequest) {
            Logger::getInstance().log("ERROR: EliteFlatBufferHelper: Expected PreFlightCheckRequest");
            return std::nullopt;
        }

        // Generated union accessor validates expected table type.
        const Schema::PreFlightCheckRequest* request = envelope->data_as_PreFlightCheckRequest();

        if (!request) {
            Logger::getInstance().log("ERROR: EliteFlatBufferHelper: Failed to cast PreFlightCheckRequest");
            return std::nullopt;
        }

        // Extract fields
        std::map<std::string, std::string> result;
        result["request_id"] = request->request_id() ? request->request_id()->str() : "";
        result["heartbeat_ms"] = std::to_string(request->heartbeat_ms());
        result["validation_ms"] = std::to_string(request->validation_ms());
        result["sequence_id"] = std::to_string(request->sequence_id());
        result["version"] = request->version() ? request->version()->str() : "2.4";

        return result;
    }
    catch (const std::exception& e) {
        Logger::getInstance().log(std::string("ERROR: PreFlightCheckRequest parse failed: ") + e.what());
        return std::nullopt;
    }
}

// ===== PREFLIGHT CHECK RESPONSE BUILDING (C++ → Python) =====

zmq::message_t EliteFlatBufferHelper::BuildPreFlightCheckResponse(
    const std::string& request_id,
    MTS::Schema::PreFlightStatus status,  // Typed enum (from item 1 hardening)
    bool model_loaded,
    bool system_ready,
    const std::string& reason,
    const std::string& cpp_state,
    uint64_t sequence_id)
{
    flatbuffers::FlatBufferBuilder fbb(Schema::Contract::kBuilderCapacityPreFlightResponse);

    // Serialize string fields
    auto request_id_offset = fbb.CreateString(request_id);
    auto reason_offset = fbb.CreateString(reason);
    auto cpp_state_offset = fbb.CreateString(cpp_state);

    // Build PreFlightCheckResponse table
    Schema::PreFlightCheckResponseBuilder resp_builder(fbb);
    resp_builder.add_sequence_id(sequence_id);
    resp_builder.add_request_id(request_id_offset);
    resp_builder.add_timestamp_us(Schema::Contract::NowTimestampUs());
    resp_builder.add_status(status);  // Typed enum (not string)
    resp_builder.add_cpp_state(cpp_state_offset);
    resp_builder.add_reason(reason_offset);
    resp_builder.add_model_loaded(model_loaded);
    resp_builder.add_system_ready(system_ready);

    auto response = resp_builder.Finish();

    // Wrap in MTS_Envelope with proper union handling
    // The envelope.data field holds the serialized response
    // The envelope.data_type field holds Message_PreFlightCheckResponse
    auto envelope = Schema::Contract::BuildEnvelope(
        fbb,
        Schema::Contract::kEnvelopePreFlightCheckResponse,
        response.Union());

    fbb.Finish(envelope);

    // Copy to ZMQ message
    const uint8_t* buf = fbb.GetBufferPointer();
    size_t size = fbb.GetSize();
    zmq::message_t result(size);
    std::memcpy(result.data(), buf, size);

    return result;
}

// ===== HEARTBEAT PARSING (receive from C++ or Python) =====

std::optional<HeartbeatData> EliteFlatBufferHelper::ParseHeartbeat(const zmq::message_t& msg)
{
    try {
        const Schema::MTS_Envelope* envelope =
            Schema::Contract::GetVerifiedEnvelope(msg.data(), msg.size());
        if (!envelope) {
            Logger::getInstance().log("ERROR: EliteFlatBufferHelper: Invalid Heartbeat envelope");
            return std::nullopt;
        }

        // Check message type
        if (envelope->data_type() != Schema::Contract::kEnvelopeHeartbeat) {
            Logger::getInstance().log("ERROR: EliteFlatBufferHelper: Expected Heartbeat message");
            return std::nullopt;
        }

        const Schema::Heartbeat* heartbeat = envelope->data_as_Heartbeat();

        if (!heartbeat) {
            Logger::getInstance().log("ERROR: EliteFlatBufferHelper: Failed to cast Heartbeat");
            return std::nullopt;
        }

        // Extract all schema-defined heartbeat fields (zero-copy).
        HeartbeatData data;
        data.sequence_id = heartbeat->sequence_id();
        data.timestamp_us = heartbeat->timestamp_us();
        data.sender = heartbeat->sender() ? heartbeat->sender()->str() : "UNKNOWN";
        data.uptime_ms = heartbeat->uptime_ms();
        data.message_count = heartbeat->message_count();
        data.cpu_usage_pct = heartbeat->cpu_usage_pct();
        data.model_status = heartbeat->model_status() ? heartbeat->model_status()->str() : "UNKNOWN";
        data.last_inference_ms = heartbeat->last_inference_ms();
        data.avg_inference_ms = heartbeat->avg_inference_ms();
        data.queue_depth = heartbeat->queue_depth();
        data.error_count = heartbeat->error_count();

        return data;
    }
    catch (const std::exception& e) {
        Logger::getInstance().log(std::string("ERROR: Heartbeat parse failed: ") + e.what());
        return std::nullopt;
    }
}

// ===== HEARTBEAT BUILDING (C++ sends to Python) =====

zmq::message_t EliteFlatBufferHelper::BuildHeartbeat(
    const std::string& sender,
    uint64_t uptime_ms,
    uint64_t message_count,
    float cpu_usage_pct,
    const std::string& model_status,
    float last_inference_ms,
    float avg_inference_ms,
    int queue_depth,
    int error_count,
    uint64_t sequence_id)
{
    flatbuffers::FlatBufferBuilder fbb(Schema::Contract::kBuilderCapacityHeartbeat);

    // Serialize strings
    auto sender_offset = fbb.CreateString(sender);
    auto model_status_offset = fbb.CreateString(model_status);

    // Build Heartbeat table with all 11 fields
    Schema::HeartbeatBuilder hb_builder(fbb);
    hb_builder.add_sequence_id(sequence_id);
    hb_builder.add_timestamp_us(Schema::Contract::NowTimestampUs());
    hb_builder.add_sender(sender_offset);
    hb_builder.add_uptime_ms(static_cast<int64_t>(uptime_ms));
    hb_builder.add_message_count(message_count);
    hb_builder.add_cpu_usage_pct(cpu_usage_pct);
    hb_builder.add_model_status(model_status_offset);
    hb_builder.add_last_inference_ms(last_inference_ms);
    hb_builder.add_avg_inference_ms(avg_inference_ms);
    hb_builder.add_queue_depth(queue_depth);
    hb_builder.add_error_count(error_count);

    auto heartbeat = hb_builder.Finish();

    // Wrap in MTS_Envelope with proper union handling
    auto envelope = Schema::Contract::BuildEnvelope(
        fbb,
        Schema::Contract::kEnvelopeHeartbeat,
        heartbeat.Union());

    fbb.Finish(envelope);

    // Copy to ZMQ message
    const uint8_t* buf = fbb.GetBufferPointer();
    size_t size = fbb.GetSize();
    zmq::message_t result(size);
    std::memcpy(result.data(), buf, size);

    return result;
}

zmq::message_t EliteFlatBufferHelper::BuildDiagnostic(
    const std::string& sender,
    const std::string& message,
    float mahalanobis_distance,
    float observation_significance)
{
    flatbuffers::FlatBufferBuilder fbb(Schema::Contract::kBuilderCapacityDiagnostic);

    auto sender_offset = fbb.CreateString(sender);
    auto message_offset = fbb.CreateString(message);
    const auto timestamp_us = Schema::Contract::NowTimestampUs();

    Schema::DiagnosticBuilder diagnostic_builder(fbb);
    diagnostic_builder.add_timestamp_us(timestamp_us);
    diagnostic_builder.add_sender(sender_offset);
    diagnostic_builder.add_message(message_offset);
    diagnostic_builder.add_mahalanobis_distance(mahalanobis_distance);
    diagnostic_builder.add_observation_significance(observation_significance);
    auto diagnostic = diagnostic_builder.Finish();

    auto envelope = Schema::Contract::BuildEnvelope(
        fbb,
        Schema::Contract::kEnvelopeDiagnostic,
        diagnostic.Union());

    fbb.Finish(envelope);

    const uint8_t* buf = fbb.GetBufferPointer();
    const size_t size = fbb.GetSize();
    zmq::message_t result(size);
    std::memcpy(result.data(), buf, size);

    return result;
}

zmq::message_t EliteFlatBufferHelper::BuildDiagnosticWithGateEvent(
    const std::string& sender,
    const std::string& message,
    Schema::GateDirective directive,
    Schema::ReasonCode reason_code,
    int8_t action_id,
    float mahalanobis_distance,
    float observation_significance)
{
    flatbuffers::FlatBufferBuilder fbb(Schema::Contract::kBuilderCapacityDiagnostic);

    auto sender_offset = fbb.CreateString(sender);
    auto message_offset = fbb.CreateString(message);
    const auto timestamp_us = Schema::Contract::NowTimestampUs();

    const auto gate_event = Schema::CreateGateEvent(
        fbb,
        timestamp_us,
        sender_offset,
        directive,
        reason_code,
        action_id);

    Schema::DiagnosticBuilder diagnostic_builder(fbb);
    diagnostic_builder.add_timestamp_us(timestamp_us);
    diagnostic_builder.add_sender(sender_offset);
    diagnostic_builder.add_message(message_offset);
    diagnostic_builder.add_mahalanobis_distance(mahalanobis_distance);
    diagnostic_builder.add_observation_significance(observation_significance);
    diagnostic_builder.add_gate_event(gate_event);
    auto diagnostic = diagnostic_builder.Finish();

    auto envelope = Schema::Contract::BuildEnvelope(
        fbb,
        Schema::Contract::kEnvelopeDiagnostic,
        diagnostic.Union());

    fbb.Finish(envelope);

    const uint8_t* buf = fbb.GetBufferPointer();
    const size_t size = fbb.GetSize();
    zmq::message_t result(size);
    std::memcpy(result.data(), buf, size);

    return result;
}

zmq::message_t EliteFlatBufferHelper::BuildDiagnosticWithIntentEvent(
    const std::string& sender,
    const std::string& message,
    uint64_t intent_id,
    Schema::IntentState from_state,
    Schema::IntentState to_state,
    Schema::ReasonCode reason_code,
    int8_t action_id,
    uint64_t sequence_id,
    const std::string& source,
    float mahalanobis_distance,
    float observation_significance)
{
    flatbuffers::FlatBufferBuilder fbb(Schema::Contract::kBuilderCapacityDiagnostic);

    auto sender_offset = fbb.CreateString(sender);
    auto message_offset = fbb.CreateString(message);
    auto source_offset = fbb.CreateString(source);
    const auto timestamp_us = Schema::Contract::NowTimestampUs();

    const auto intent_event = Schema::CreateIntentEvent(
        fbb,
        timestamp_us,
        sender_offset,
        intent_id,
        from_state,
        to_state,
        reason_code,
        action_id,
        sequence_id,
        source_offset);

    Schema::DiagnosticBuilder diagnostic_builder(fbb);
    diagnostic_builder.add_timestamp_us(timestamp_us);
    diagnostic_builder.add_sender(sender_offset);
    diagnostic_builder.add_message(message_offset);
    diagnostic_builder.add_mahalanobis_distance(mahalanobis_distance);
    diagnostic_builder.add_observation_significance(observation_significance);
    diagnostic_builder.add_intent_event(intent_event);
    auto diagnostic = diagnostic_builder.Finish();

    auto envelope = Schema::Contract::BuildEnvelope(
        fbb,
        Schema::Contract::kEnvelopeDiagnostic,
        diagnostic.Union());

    fbb.Finish(envelope);

    const uint8_t* buf = fbb.GetBufferPointer();
    const size_t size = fbb.GetSize();
    zmq::message_t result(size);
    std::memcpy(result.data(), buf, size);

    return result;
}

zmq::message_t EliteFlatBufferHelper::BuildDiagnosticWithRecoveryEvent(
    const std::string& sender,
    const std::string& message,
    Schema::RecoveryState state,
    Schema::ReasonCode prior_reason_code,
    float mahalanobis_distance,
    float observation_significance)
{
    flatbuffers::FlatBufferBuilder fbb(Schema::Contract::kBuilderCapacityDiagnostic);

    auto sender_offset = fbb.CreateString(sender);
    auto message_offset = fbb.CreateString(message);
    const auto timestamp_us = Schema::Contract::NowTimestampUs();

    const auto recovery_event = Schema::CreateRecoveryEvent(
        fbb,
        timestamp_us,
        sender_offset,
        state,
        prior_reason_code);

    Schema::DiagnosticBuilder diagnostic_builder(fbb);
    diagnostic_builder.add_timestamp_us(timestamp_us);
    diagnostic_builder.add_sender(sender_offset);
    diagnostic_builder.add_message(message_offset);
    diagnostic_builder.add_mahalanobis_distance(mahalanobis_distance);
    diagnostic_builder.add_observation_significance(observation_significance);
    diagnostic_builder.add_recovery_event(recovery_event);
    auto diagnostic = diagnostic_builder.Finish();

    auto envelope = Schema::Contract::BuildEnvelope(
        fbb,
        Schema::Contract::kEnvelopeDiagnostic,
        diagnostic.Union());

    fbb.Finish(envelope);

    const uint8_t* buf = fbb.GetBufferPointer();
    const size_t size = fbb.GetSize();
    zmq::message_t result(size);
    std::memcpy(result.data(), buf, size);

    return result;
}

// ===== MESSAGE ENVELOPE UTILITIES =====

std::optional<Schema::Message> EliteFlatBufferHelper::GetMessageType(const zmq::message_t& msg)
{
    try {
        const Schema::MTS_Envelope* envelope =
            Schema::Contract::GetVerifiedEnvelope(msg.data(), msg.size());
        if (!envelope) {
            return std::nullopt;
        }

        return envelope->data_type();
    }
    catch (...) {
        return std::nullopt;
    }
}

std::optional<std::vector<uint8_t>> EliteFlatBufferHelper::ExtractMessagePayload(
    const zmq::message_t& msg)
{
    try {
        const Schema::MTS_Envelope* envelope =
            Schema::Contract::GetVerifiedEnvelope(msg.data(), msg.size());
        if (!envelope) {
            return std::nullopt;
        }

        // Return full message bytes (caller will deserialize based on type)
        std::vector<uint8_t> payload(
            static_cast<const uint8_t*>(msg.data()),
            static_cast<const uint8_t*>(msg.data()) + msg.size());

        return payload;
    }
    catch (...) {
        return std::nullopt;
    }
}

// ===== SEQUENCE TRACKING =====

uint64_t EliteFlatBufferHelper::GetDroppedMessageCount(
    uint64_t last_sequence,
    uint64_t current_sequence)
{
    if (last_sequence == 0) {
        return 0;  // First message
    }

    uint64_t expected = last_sequence + 1;
    if (current_sequence >= expected) {
        return current_sequence - expected;
    }

    // Sequence wrapped around (uint64_t overflow)
    return 0;
}

} // namespace MTS
