#pragma once

#include <zmq.hpp>
#include <memory>
#include <mutex>

/**
 * @brief Singleton manager for ZMQ context - CRITICAL for thread safety
 * 
 * ZMQ RULE: One context per process, shared by all sockets
 * Multiple contexts cause undefined behavior and crashes
 */
class ZMQContextManager {
public:
    static ZMQContextManager& Instance() {
        static ZMQContextManager instance;
        return instance;
    }
    
    // Get the shared ZMQ context
    zmq::context_t& GetContext() {
        return m_context;
    }
    
    // Deleted copy/move constructors
    ZMQContextManager(const ZMQContextManager&) = delete;
    ZMQContextManager& operator=(const ZMQContextManager&) = delete;
    ZMQContextManager(ZMQContextManager&&) = delete;
    ZMQContextManager& operator=(ZMQContextManager&&) = delete;
    
private:
    ZMQContextManager() : m_context(1) {}
    ~ZMQContextManager() = default;
    
    zmq::context_t m_context;  // Single shared context for entire process
};
