// ═══════════════════════════════════════════════════════════════════════════
// MessagingFramework.cpp - Elite v2.4 Institutional Message Context
// ═══════════════════════════════════════════════════════════════════════════
// Purpose: C++ implementation of unified message context and request tracking
//          for cohesive Python-C++ FlatBuffer messaging patterns.
//
// Architecture:
// - MessageContext: Carries metadata (sequence_id, request_id, priority, etc.)
// - RequestTracker: Maps pending requests for response correlation
// - BuilderPool: Object pool for reusable FlatBuffer builders
// - MessageErrorCode: Structured error handling
//
// Design Pattern: RAII with automatic resource cleanup
// Thread-Safety: std::mutex for RequestTracker, atomic for sequence counters
// ═══════════════════════════════════════════════════════════════════════════

#include "MessagingFramework.h"
#include "../Logger.h"
#include <chrono>
#include <algorithm>

// ═══════════════════════════════════════════════════════════════════════════
// MessageContext Implementation
// ═══════════════════════════════════════════════════════════════════════════

// Static sequence counter (shared across all contexts)
std::atomic<uint64_t> MessageContext::s_nextSequenceId(1);

MessageContext MessageContext::Create(
    uint32_t request_id,
    uint8_t priority)
{
    MessageContext ctx;
    ctx.sequence_id = s_nextSequenceId.fetch_add(1);
    ctx.request_id = request_id;
    ctx.sender = "SystemOrchestrator";  // Default sender
    ctx.timestamp_us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    ctx.priority = priority;
    ctx.checksum = 0;
    ctx.expiration_us = ctx.timestamp_us + (5000000);  // 5 second TTL
    return ctx;
}

MessageContext MessageContext::CreateWithTTL(
    uint32_t request_id,
    uint8_t priority,
    uint64_t ttl_us)
{
    MessageContext ctx = Create(request_id, priority);
    ctx.expiration_us = ctx.timestamp_us + ttl_us;
    return ctx;
}

bool MessageContext::IsExpired() const
{
    uint64_t now = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return now > expiration_us;
}

std::string MessageContext::ToString() const
{
    return "MessageContext(seq=" + std::to_string(sequence_id) +
           ", req=" + std::to_string(request_id) +
           ", priority=" + std::to_string(static_cast<int>(priority)) +
           ", sender=" + sender + ")";
}

// ═══════════════════════════════════════════════════════════════════════════
// RequestTracker Implementation
// ═══════════════════════════════════════════════════════════════════════════

RequestTracker::RequestTracker()
    : m_cleanupIntervalMs(5000), m_lastCleanupTime(std::chrono::steady_clock::now())
{
}

void RequestTracker::RegisterRequest(
    uint64_t sequence_id,
    uint32_t request_id,
    const std::string& message_type,
    uint32_t timeout_ms)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    PendingRequest req;
    req.request_id = request_id;
    req.sent_time = std::chrono::steady_clock::now();
    req.timeout_ms = timeout_ms;
    req.message_type = message_type;

    m_pendingRequests[sequence_id] = req;

    Logger::getInstance().log("[RequestTracker] Registered request: "
        "seq=" + std::to_string(sequence_id) +
        ", req=" + std::to_string(request_id) +
        ", type=" + message_type +
        ", timeout=" + std::to_string(timeout_ms) + "ms");

    // Periodically clean up expired requests (every 5 seconds)
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - m_lastCleanupTime).count();

    if (elapsed > m_cleanupIntervalMs) {
        CleanupExpired();
        m_lastCleanupTime = now;
    }
}

bool RequestTracker::ValidateResponse(
    uint64_t sequence_id,
    const std::string& message_type)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    auto it = m_pendingRequests.find(sequence_id);
    if (it == m_pendingRequests.end()) {
        Logger::getInstance().log("[RequestTracker] WARN: No pending request for seq=" +
            std::to_string(sequence_id));
        return false;
    }

    const PendingRequest& pending = it->second;

    // Check message type matches
    if (pending.message_type + "_RESPONSE" != message_type &&
        pending.message_type + "_ACK" != message_type &&
        pending.message_type != message_type) {
        Logger::getInstance().log("[RequestTracker] ERROR: Message type mismatch: "
            "pending=" + pending.message_type +
            ", received=" + message_type);
        return false;
    }

    // Check timeout
    auto now = std::chrono::steady_clock::now();
    auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        now - pending.sent_time).count();

    if (elapsed > static_cast<int64_t>(pending.timeout_ms)) {
        Logger::getInstance().log("[RequestTracker] ERROR: Request timeout: "
            "seq=" + std::to_string(sequence_id) +
            ", elapsed=" + std::to_string(elapsed) + "ms");
        m_pendingRequests.erase(it);
        return false;
    }

    Logger::getInstance().log("[RequestTracker] ✅ Response validated: "
        "seq=" + std::to_string(sequence_id) +
        ", latency=" + std::to_string(elapsed) + "ms");

    // Remove from pending (successful response received)
    m_pendingRequests.erase(it);
    return true;
}

void RequestTracker::CleanupExpired()
{
    auto now = std::chrono::steady_clock::now();
    int cleanup_count = 0;

    auto it = m_pendingRequests.begin();
    while (it != m_pendingRequests.end()) {
        auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            now - it->second.sent_time).count();

        if (elapsed > static_cast<int64_t>(it->second.timeout_ms)) {
            Logger::getInstance().log("[RequestTracker] Cleaning up expired request: "
                "seq=" + std::to_string(it->first) +
                ", elapsed=" + std::to_string(elapsed) + "ms");

            it = m_pendingRequests.erase(it);
            cleanup_count++;
        } else {
            ++it;
        }
    }

    if (cleanup_count > 0) {
        Logger::getInstance().log("[RequestTracker] Cleanup completed: " +
            std::to_string(cleanup_count) + " requests removed");
    }
}

size_t RequestTracker::PendingCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_pendingRequests.size();
}

std::vector<uint64_t> RequestTracker::GetPendingSequenceIds() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    std::vector<uint64_t> ids;
    for (const auto& pair : m_pendingRequests) {
        ids.push_back(pair.first);
    }
    return ids;
}

// ═══════════════════════════════════════════════════════════════════════════
// MessageErrorCode Implementation
// ═══════════════════════════════════════════════════════════════════════════

std::string MessageErrorCode::ToString(ErrorCode code)
{
    switch (code) {
        case ErrorCode::OK: return "OK";
        case ErrorCode::ENQUEUE_FAILED: return "ENQUEUE_FAILED";
        case ErrorCode::PARSE_ERROR: return "PARSE_ERROR";
        case ErrorCode::VALIDATION_FAILED: return "VALIDATION_FAILED";
        case ErrorCode::SEQUENCE_MISMATCH: return "SEQUENCE_MISMATCH";
        case ErrorCode::TIMEOUT: return "TIMEOUT";
        case ErrorCode::SOCKET_ERROR: return "SOCKET_ERROR";
        case ErrorCode::BUFFER_OVERFLOW: return "BUFFER_OVERFLOW";
        case ErrorCode::UNIMPLEMENTED: return "UNIMPLEMENTED";
        default: return "UNKNOWN";
    }
}

// ═══════════════════════════════════════════════════════════════════════════
// BuilderPool Implementation
// ═══════════════════════════════════════════════════════════════════════════

BuilderPool::BuilderPool(size_t initial_size, size_t builder_initial_capacity)
    : m_initialCapacity(builder_initial_capacity), m_maxSize(initial_size)
{
    // Pre-allocate builders
    for (size_t i = 0; i < initial_size; ++i) {
        m_builders.push_back(
            std::make_unique<flatbuffers::FlatBufferBuilder>(builder_initial_capacity)
        );
    }

    Logger::getInstance().log("[BuilderPool] Initialized with " +
        std::to_string(initial_size) + " builders (capacity: " +
        std::to_string(builder_initial_capacity) + " bytes each)");
}

PooledBuilder BuilderPool::Acquire()
{
    std::lock_guard<std::mutex> lock(m_mutex);

    flatbuffers::FlatBufferBuilder* builder = nullptr;

    if (!m_builders.empty()) {
        builder = m_builders.back().get();
        m_builders.pop_back();
    } else {
        // Pool exhausted - create new builder (will be discarded after use)
        Logger::getInstance().log("[BuilderPool] WARN: Pool exhausted, creating temporary builder");
        builder = new flatbuffers::FlatBufferBuilder(m_initialCapacity);
    }

    return PooledBuilder(builder, this);
}

void BuilderPool::Release(flatbuffers::FlatBufferBuilder* builder)
{
    std::lock_guard<std::mutex> lock(m_mutex);

    // Clear builder state for reuse
    builder->Clear();

    // Return to pool if not exceeding max size
    if (m_builders.size() < m_maxSize) {
        m_builders.push_back(
            std::unique_ptr<flatbuffers::FlatBufferBuilder>(builder)
        );
    } else {
        // Delete if pool is full
        delete builder;
    }
}

size_t BuilderPool::AvailableCount() const
{
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_builders.size();
}

// ═══════════════════════════════════════════════════════════════════════════
// PooledBuilder Implementation (RAII)
// ═══════════════════════════════════════════════════════════════════════════

PooledBuilder::PooledBuilder(
    flatbuffers::FlatBufferBuilder* builder,
    BuilderPool* pool)
    : m_builder(builder), m_pool(pool)
{
}

PooledBuilder::~PooledBuilder()
{
    if (m_pool && m_builder) {
        m_pool->Release(m_builder);
    }
}

flatbuffers::FlatBufferBuilder* PooledBuilder::operator->()
{
    return m_builder;
}

flatbuffers::FlatBufferBuilder& PooledBuilder::operator*()
{
    return *m_builder;
}

flatbuffers::FlatBufferBuilder* PooledBuilder::Get()
{
    return m_builder;
}

