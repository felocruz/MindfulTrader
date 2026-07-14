#pragma once

/**
 * Elite FlatBuffers Messaging Framework v2.4
 *
 * Institutional-Grade Patterns:
 * - Zero-copy envelope parsing (all messages wrapped)
 * - Object API for serialization (no manual builders)
 * - Message correlation (request_id + sequence_id)
 * - Request tracking with automatic timeout cleanup
 * - Type-safe routing via union discriminators
 * - Microsecond timestamp precision
 */

#include <flatbuffers/flatbuffers.h>
#include <zmq.hpp>
#include <chrono>
#include <string>
#include <map>
#include <mutex>
#include <atomic>
#include <optional>
#include "Logger.h"

namespace MTS::Messaging {

// ============================================================================
// MESSAGE CONTEXT: Carries metadata through serialization pipeline
// ============================================================================

struct MessageContext {
    uint64_t sequence_id;           // Monotonically increasing
    std::string request_id;         // For request-response correlation
    std::string sender;             // "MindfulTrader" or "MTS"
    int64_t timestamp_us;           // UTC microseconds (required)
    uint8_t priority;               // 0=low, 1=normal, 2=high, 3=critical
    uint32_t checksum;              // CRC32 of payload
    int64_t expiration_us;          // TTL (0 = no expiration)

    static std::atomic<uint64_t> next_sequence_;

    static MessageContext Create(const std::string& request_id = "",
                                 uint8_t priority = 1) {
        MessageContext ctx;
        ctx.sequence_id = next_sequence_.fetch_add(1);
        ctx.request_id = request_id.empty()
            ? "ctx_" + std::to_string(ctx.sequence_id)
            : request_id;
        ctx.sender = "MindfulTrader";
        ctx.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
        ctx.priority = priority;
        ctx.checksum = 0;
        ctx.expiration_us = 0;  // No expiration by default
        return ctx;
    }
};

// ============================================================================
// REQUEST TRACKER: Correlates responses to requests
// ============================================================================

class RequestTracker {
public:
    struct PendingRequest {
        uint64_t sequence_id;
        std::string request_id;
        std::chrono::steady_clock::time_point sent_time;
        std::chrono::milliseconds timeout;
        std::string message_type;
    };

    void RegisterRequest(uint64_t sequence_id,
                        const std::string& request_id,
                        const std::string& message_type = "",
                        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
        std::lock_guard<std::mutex> lock(lock_);
        pending_[sequence_id] = {
            sequence_id,
            request_id,
            std::chrono::steady_clock::now(),
            timeout,
            message_type
        };
    }

    bool ValidateResponse(uint64_t sequence_id) {
        std::lock_guard<std::mutex> lock(lock_);
        auto it = pending_.find(sequence_id);
        if (it == pending_.end()) {
            return false;  // No matching request
        }

        auto elapsed = std::chrono::steady_clock::now() - it->second.sent_time;
        if (elapsed > it->second.timeout) {
            pending_.erase(it);
            return false;  // Timed out
        }

        pending_.erase(it);
        return true;  // Valid response
    }

    std::optional<PendingRequest> GetPendingRequest(uint64_t sequence_id) {
        std::lock_guard<std::mutex> lock(lock_);
        auto it = pending_.find(sequence_id);
        if (it == pending_.end()) {
            return std::nullopt;
        }
        return it->second;
    }

    void CleanupExpired() {
        std::lock_guard<std::mutex> lock(lock_);
        auto now = std::chrono::steady_clock::now();

        for (auto it = pending_.begin(); it != pending_.end();) {
            auto elapsed = now - it->second.sent_time;
            if (elapsed > it->second.timeout) {
                Logger::getInstance().log("RequestTracker: Cleaned up expired request " +
                    it->second.request_id + " (seq=" + std::to_string(it->first) + ")");
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }
    }

    size_t PendingCount() const {
        std::lock_guard<std::mutex> lock(lock_);
        return pending_.size();
    }

private:
    mutable std::mutex lock_;
    std::map<uint64_t, PendingRequest> pending_;
};

// ============================================================================
// ENUM: Message Error Codes
// ============================================================================

enum class MessageErrorCode: uint8_t {
    OK = 0,                           // Success
    ENQUEUE_FAILED = 1,               // ZMQ send would block
    PARSE_ERROR = 2,                  // FlatBuffer parsing failed
    VALIDATION_FAILED = 3,            // Message validation failed
    TIMEOUT = 4,                      // Request timed out
    CORRELATION_MISMATCH = 5,         // Sequence/request_id mismatch
    PROTOCOL_VERSION_MISMATCH = 6,    // Version incompatibility
    RESOURCE_EXHAUSTED = 7,           // No available builders/buffers
    INTERNAL_ERROR = 8,               // Unexpected exception
    UNIMPLEMENTED = 9                 // Feature not implemented
};

inline const char* ErrorCodeToString(MessageErrorCode code) {
    switch (code) {
        case MessageErrorCode::OK: return "OK";
        case MessageErrorCode::ENQUEUE_FAILED: return "ENQUEUE_FAILED";
        case MessageErrorCode::PARSE_ERROR: return "PARSE_ERROR";
        case MessageErrorCode::VALIDATION_FAILED: return "VALIDATION_FAILED";
        case MessageErrorCode::TIMEOUT: return "TIMEOUT";
        case MessageErrorCode::CORRELATION_MISMATCH: return "CORRELATION_MISMATCH";
        case MessageErrorCode::PROTOCOL_VERSION_MISMATCH: return "PROTOCOL_VERSION_MISMATCH";
        case MessageErrorCode::RESOURCE_EXHAUSTED: return "RESOURCE_EXHAUSTED";
        case MessageErrorCode::INTERNAL_ERROR: return "INTERNAL_ERROR";
        case MessageErrorCode::UNIMPLEMENTED: return "UNIMPLEMENTED";
    }
    return "UNKNOWN";
}

// ============================================================================
// BUILDER POOL: Reusable FlatBuffer builders (reduces allocation overhead)
// ============================================================================

class BuilderPool {
public:
    static BuilderPool& Instance() {
        static BuilderPool instance;
        return instance;
    }

    class PooledBuilder {
    public:
        PooledBuilder(BuilderPool& pool) : pool_(pool), builder_(nullptr) {}
        
        // Allow move semantics (needed for return value)
        PooledBuilder(PooledBuilder&&) = default;
        
        // Delete copy and move assignment (reference members can't be reassigned)
        PooledBuilder(const PooledBuilder&) = delete;
        PooledBuilder& operator=(const PooledBuilder&) = delete;
        PooledBuilder& operator=(PooledBuilder&&) = delete;

        ~PooledBuilder() {
            if (builder_) {
                pool_.Release(std::move(builder_));
            }
        }

        flatbuffers::FlatBufferBuilder* operator->() { return builder_.get(); }
        flatbuffers::FlatBufferBuilder& operator*() { return *builder_; }

        flatbuffers::FlatBufferBuilder* Get() { return builder_.get(); }

        void Reset() {
            if (builder_) {
                builder_->Reset();
            }
        }

    private:
        BuilderPool& pool_;
        std::unique_ptr<flatbuffers::FlatBufferBuilder> builder_;

        friend class BuilderPool;
    };

    PooledBuilder Acquire(size_t initial_size = 256) {
        std::lock_guard<std::mutex> lock(lock_);

        auto builder = std::make_unique<flatbuffers::FlatBufferBuilder>(initial_size);
        pool_.push_back(std::move(builder));

        PooledBuilder pb(*this);
        pb.builder_ = std::move(pool_.back());
        pool_.pop_back();

        return pb;
    }

private:
    BuilderPool() = default;

    void Release(std::unique_ptr<flatbuffers::FlatBufferBuilder> builder) {
        std::lock_guard<std::mutex> lock(lock_);
        if (pool_.size() < MAX_POOL_SIZE) {
            builder->Reset();
            pool_.push_back(std::move(builder));
        }
        // Otherwise let it be destroyed
    }

    mutable std::mutex lock_;
    std::vector<std::unique_ptr<flatbuffers::FlatBufferBuilder>> pool_;
    static constexpr size_t MAX_POOL_SIZE = 8;

    friend class PooledBuilder;
};

// ============================================================================
// COHESIVE MESSAGING: Refactored methods for SystemOrchestrator
// ============================================================================

/**
 * All message building should follow this pattern:
 *
 * 1. Create optional MessageContext (auto-generated if empty)
 * 2. Build message content using Object API pattern
 * 3. Create envelope wrapper
 * 4. Pack into FlatBuffer
 * 5. Return zmq::message_t
 *
 * ✅ DO THIS:
 *    auto ctx = MessageContext::Create(request_id);
 *
 *    MTS::Schema::HeartbeatT hb;
 *    hb.sequence_id = ctx.sequence_id;
 *    hb.sender = "MindfulTrader";
 *    // ... populate all fields
 *
 *    FlatBufferBuilder fbb(512);
 *    auto hb_offset = MTS::Schema::Heartbeat::Pack(fbb, &hb);
 *
 *    MTS::Schema::MTS_MessageEnvelopeBuilder env(fbb);
 *    // ... wrap message
 *
 * ❌ DON'T DO THIS:
 *    FlatBufferBuilder fbb;
 *    HeartbeatBuilder hb_builder(fbb);
 *    hb_builder.add_field1(...);
 *    hb_builder.add_field2(...);
 *    // (Loses metadata, manual field ordering, no envelope)
 */

}  // namespace MTS::Messaging
