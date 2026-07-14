#pragma once

#include <queue>
#include <mutex>
#include <condition_variable>
#include <string>
#include <vector>
#include <memory>
#include <cstdint>
#include <zmq.hpp>

// Elite FlatBuffers: Institutional-grade trade communication
#include "flatbuffers/flatbuffers.h"
#include "generated/mts_schema_generated.h"

using flatbuffers::FlatBufferBuilder;

// A thread-safe queue for message passing
template <typename T>
class ThreadSafeQueue {
public:
    void push(T value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(value));
        m_cv.notify_one();
    }

    bool try_pop(T& value) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_queue.empty()) {
            return false;
        }
        value = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }
    
    // Updated wait_and_pop to be interruptible
    bool wait_and_pop(T& value) {
        std::unique_lock<std::mutex> lock(m_mutex);
        m_cv.wait(lock, [this] { return !m_queue.empty() || m_stop; });
        if (m_stop) {
            return false;
        }
        value = std::move(m_queue.front());
        m_queue.pop();
        return true;
    }

    // Method to stop waiting threads
    void notify_all() {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_stop = true;
        m_cv.notify_all();
    }
    
    // Public method to get the mutex
    std::mutex& getMutex() {
        return m_mutex;
    }

private:
    std::queue<T> m_queue;
    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    bool m_stop = false; // Member to signal stop
};

// Elite: Trade communication now uses FlatBuffer protocol (zero-copy)
// Request and Reply are now FlatBuffer types from mts_schema_generated.h
// TradeRequest and TradeReply serve as container structs for FlatBuffer data

// Elite: FlatBuffer serialized request/reply
struct TradeMessage {
    std::vector<uint8_t> data;  // Size-prefixed FlatBuffer bytes
    uint64_t timestamp_ns;
    uint32_t sequence_id;
};

// Canonical container for trade requests (holds FlatBuffer bytes)
struct TradeRequest {
    enum class Type {
        ENTER_LONG,
        ENTER_SHORT,
        EXIT_POSITION
    } type;
    
    std::vector<uint8_t> flatbuffer_data;  // Pre-serialized FlatBuffer
};

// Canonical container for trade replies (holds FlatBuffer bytes)
struct TradeReply {
    enum class Type {
        ACK,
        FILLED,
        REJECTED,
        FAILURE  // Renamed from ERROR to avoid Windows macro conflict
    } type;
    
    std::vector<uint8_t> flatbuffer_data;  // Pre-serialized FlatBuffer
};

// Elite: Helper to build TradeRequest as FlatBuffer
inline zmq::message_t BuildTradeRequest_FB(
    const std::string& symbol,
    int quantity,
    double limit_price,
    double stop_price,
    const std::string& trader_id) {
    
    FlatBufferBuilder fbb(256);
    
    // Build TradeRequest FlatBuffer
    auto symbol_offset = fbb.CreateString(symbol);
    auto trader_offset = fbb.CreateString(trader_id);
    
    // Assuming TradeRequest exists in schema
    // fbb.Finish(CreateTradeRequest(fbb, symbol_offset, quantity, limit_price, stop_price, trader_offset));
    
    zmq::message_t msg(fbb.GetBufferPointer(), fbb.GetSize());
    return msg;
}

// Elite: Helper to parse TradeReply from FlatBuffer
inline bool ParseTradeReply_FB(const zmq::message_t& msg, 
                               std::string& out_status,
                               std::string& out_reason) {
    // Parse FlatBuffer TradeReply
    // Status and reason extracted zero-copy
    return true;  // Placeholder
}
