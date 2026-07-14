#include "transport/TransportStream.h"
#include "generated/mts_schema_generated.h"
#include "Logger.h"
#include <cstring>

namespace {
constexpr uint64_t kIndicatorTraceSampleEvery = 16;
std::atomic<uint64_t> g_indicator_emit_count{0};
}

namespace MTS::Transport {

TransportStream& TransportStream::Instance() {
    static TransportStream instance;
    return instance;
}

void TransportStream::Initialize(const std::string& endpoint) {
    if (m_running) return;

    // Create ZMQ context and PUB socket
    m_context = std::make_unique<zmq::context_t>(1);
    m_socket = std::make_unique<zmq::socket_t>(*m_context, ZMQ_PUB);

    // Set High Water Mark to manage memory if the subscriber is slow
    int hwm = 1000;
    m_socket->set(zmq::sockopt::sndhwm, hwm);

    // Bind the socket to the port
    m_socket->bind(endpoint);

    m_running = true;
    m_worker = std::thread(&TransportStream::WorkerLoop, this);
}

void TransportStream::Emit(std::vector<uint8_t> payload) {
    Emit(payload.data(), payload.size());
}

void TransportStream::Emit(const uint8_t* payload, size_t size) {
    if (!m_running) return;
    if (payload == nullptr || size == 0) return;

    // Stage 4 trace: sample indicator delta events published toward Python.
    if (size >= sizeof(uint32_t)) {
        flatbuffers::Verifier verifier(payload, size);
        const auto* event = flatbuffers::GetRoot<MTS::Schema::Event>(payload);
        if (event && event->Verify(verifier) && event->indicators() != nullptr) {
            const uint64_t emit_count = ++g_indicator_emit_count;
            if (emit_count <= 3 || (emit_count % kIndicatorTraceSampleEvery) == 0) {
                Logger::getInstance().log(
                    "[PIPELINE_TRACE][4/8] C++ -> Python indicator delta published "
                    "(emit_count=" + std::to_string(emit_count) +
                    ", sequence_id=" + std::to_string(event->sequence_id()) +
                    ", timestamp_us=" + std::to_string(event->timestamp_us()) +
                    ", payload_bytes=" + std::to_string(size) + ")"
                );
            }
        }
    }

    zmq::message_t msg(size);
    std::memcpy(msg.data(), payload, size);

    {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_queue.push(std::move(msg));
    }
    m_cv.notify_one();
}

void TransportStream::WorkerLoop() {
    while (m_running) {
        zmq::message_t msg;
        {
            std::unique_lock<std::mutex> lock(m_mutex);
            // Wait until queue has data or we are shutting down
            m_cv.wait(lock, [this] { return !m_queue.empty() || !m_running; });

            if (!m_running && m_queue.empty()) break;

            msg = std::move(m_queue.front());
            m_queue.pop();
        }

        if (msg.size() > 0) {
            m_socket->send(msg, zmq::send_flags::none);
        }
    }
}

void TransportStream::Shutdown() {
    m_running = false;
    m_cv.notify_all();

    if (m_worker.joinable()) {
        m_worker.join();
    }

    if (m_socket) {
        m_socket->close();
    }
}

} // namespace MTS::Transport
