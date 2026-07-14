#pragma once
#include <zmq.hpp>
#include <vector>
#include <string>
#include <thread>
#include <atomic>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <memory>
#include <cstddef>

namespace MTS::Transport {

/**
 * @brief Thread-safe, non-blocking ZMQ PUB (Publisher) class.
 * Uses an internal queue and background thread to ensure network
 * latency never affects the main trading thread.
 */
class TransportStream {
public:
    static TransportStream& Instance();

    // Initializes the ZMQ PUB socket (e.g., "tcp://*:5555")
    void Initialize(const std::string& endpoint);

    // Pushes binary data to the background queue for broadcast
    void Emit(std::vector<uint8_t> payload);
    void Emit(const uint8_t* payload, size_t size);

    void Shutdown();

private:
    TransportStream() : m_running(false) {}
    ~TransportStream() { Shutdown(); }

    void WorkerLoop();

    std::unique_ptr<zmq::context_t> m_context;
    std::unique_ptr<zmq::socket_t> m_socket;

    std::thread m_worker;
    std::atomic<bool> m_running;

    std::queue<zmq::message_t> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cv;
};

} // namespace MTS::Transport
