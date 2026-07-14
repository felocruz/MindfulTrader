#include "transport/AIHeartbeatMonitor.h"
#include "AIConnectionMonitor.h"
#include "SystemOrchestrator.h"
#include "ZMQContextManager.h"
#include "messaging/EliteFlatBufferHelper.h"
#include "transport/TransportStream.h"
#include "generated/mts_schema_contract_generated.h"
#include "Logger.h"
#include "sierrachart.h"
#include <chrono>
#include <thread>

namespace {
constexpr uint64_t kGhostDiagnosticHeartbeatUs = 5'000'000;  // 5 seconds
constexpr uint64_t kTransportStallLagUs = 40'000;            // 40ms stall threshold (Windows+WSL)
constexpr float kComputeHealthyLastInferenceMs = 12.0f;
constexpr float kComputeHealthyAvgInferenceMs = 40.0f;
constexpr float kComputeStallAvgInferenceMs = 120.0f;
constexpr int kComputeStallQueueDepth = 40;
static_assert(MTS::Schema::Contract::kHeartbeatFieldCount == 11,
              "Heartbeat contract changed: update monitor parsing assumptions");
}

AIHeartbeatMonitor& AIHeartbeatMonitor::Instance() {
    static AIHeartbeatMonitor instance;
    return instance;
}

AIHeartbeatMonitor::AIHeartbeatMonitor() {
}

AIHeartbeatMonitor::~AIHeartbeatMonitor() {
    Shutdown();
}

void AIHeartbeatMonitor::SetEndpoint(const std::string& endpoint) {
    if (endpoint.empty()) {
        return;
    }

    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(m_endpointMutex);
        if (m_endpoint != endpoint) {
            m_endpoint = endpoint;
            changed = true;
        }
    }

    if (changed) {
        Logger::getInstance().log("AIHeartbeatMonitor endpoint set to " + endpoint);
        if (m_isRunning.load()) {
            // Force receive loop to reconnect using the updated endpoint.
            Disconnect();
        }
    }
}

void AIHeartbeatMonitor::Init(AIConnectionMonitor& connectionMonitor) {
    m_connectionMonitor = &connectionMonitor;
    Logger::getInstance().log("✅ AIHeartbeatMonitor initialized (Elite FlatBuffer protocol)");
}

void AIHeartbeatMonitor::Start() {
    // Guard against double-start: check both the runtime flag AND thread joinability.
    // m_isRunning is set inside WorkerFunction (not before std::thread construction),
    // so a second call on a different chart's UpdateStartIndex==0 can race past the flag
    // and attempt to assign a new thread to an already-joinable m_workerThread,
    // which calls std::terminate(). Checking joinable() closes that window.
    if (m_isRunning.load() || m_workerThread.joinable()) {
        Logger::getInstance().log("AIHeartbeatMonitor already running (double-start guard: isRunning=" +
            std::to_string(m_isRunning.load()) + " joinable=" +
            std::to_string(m_workerThread.joinable()) + ")");
        return;
    }

    if (!m_connectionMonitor) {
        Logger::getInstance().log("ERROR: AIHeartbeatMonitor not initialized - call Init() first");
        return;
    }

    m_stopThread.store(false);
    m_workerThread = std::thread(&AIHeartbeatMonitor::WorkerFunction, this);
    Logger::getInstance().log("🚀 AIHeartbeatMonitor FlatBuffer subscriber started (awaiting runtime endpoint)");
}

void AIHeartbeatMonitor::Disconnect() {
    if (m_socket) {
        try {
            m_socket->close();
            Logger::getInstance().log("AIHeartbeatMonitor socket disconnected");
        } catch (const zmq::error_t& e) {
            Logger::getInstance().log(std::string("AIHeartbeatMonitor disconnect error: ") + e.what());
        }
    }
}

void AIHeartbeatMonitor::Shutdown() {
    // Elite: Atomic check-and-clear prevents double-shutdown
    if (!m_isRunning.exchange(false)) {
        return;
    }

    Logger::getInstance().log("🛑 AIHeartbeatMonitor: Initiating shutdown...");

    m_stopThread.store(true);

    // Disconnect socket to unblock receive
    Disconnect();

    // Wait for thread to exit (with timeout)
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }

    m_isRunning.store(false);
    Logger::getInstance().log("✅ AIHeartbeatMonitor: Shutdown complete");
}

void AIHeartbeatMonitor::WorkerFunction() {
    // ===== ELITE: FLATBUFFER-ONLY HEARTBEAT MONITOR =====
    // Zero JSON, type-safe parsing using FlatBuffer schema
    // Auto-reconnect with exponential backoff (1s → 8s → cap at 30s)

    int reconnect_ms = 1000;  // Start at 1s
    const int MAX_RECONNECT_MS = 30000;  // Cap at 30s
    const int BACKOFF_MULTIPLIER = 2;  // Double each attempt

    while (!m_stopThread.load()) {
        try {
            // Create SUB socket (shared ZMQ context - CRITICAL for thread safety)
            auto& context = ZMQContextManager::Instance().GetContext();
            m_socket = std::make_unique<zmq::socket_t>(context, zmq::socket_type::sub);

            // Subscribe to all messages (empty subscription = all)
            m_socket->set(zmq::sockopt::subscribe, "");

            // Connection settings (using new zmq::set API)
            m_socket->set(zmq::sockopt::rcvtimeo, 5000);  // 5s timeout per receive
            m_socket->set(zmq::sockopt::linger, 0);       // Don't linger on close

            std::string endpoint;
            {
                std::lock_guard<std::mutex> lock(m_endpointMutex);
                endpoint = m_endpoint;
            }

            if (endpoint.empty()) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                continue;
            }

            // Connect to publisher using runtime-configured endpoint.
            m_socket->connect(endpoint);
            Logger::getInstance().log("✅ AIHeartbeatMonitor connected to " + endpoint);
            reconnect_ms = 1000;  // Reset backoff on successful connect

            m_isRunning.store(true);

            // ===== RECEIVE LOOP: FLATBUFFER ONLY =====
            while (!m_stopThread.load()) {
                try {
                    zmq::message_t msg;
                    zmq::recv_result_t result = m_socket->recv(msg, zmq::recv_flags::none);

                    if (!result) {
                        // Timeout or error - continue listening
                        continue;
                    }

                    // Guard: Don't process if shutting down
                    if (m_stopThread.load()) break;

                    // ===== PARSE FLATBUFFER: ZERO-COPY DESERIALIZATION =====
                    // Elite FlatBuffer type-safe parsing with sequence tracking
                    auto hb_opt = MTS::EliteFlatBufferHelper::ParseHeartbeat(msg);

                    if (!hb_opt) {
                        Logger::getInstance().log("⚠️ Failed to parse Heartbeat FlatBuffer - skipping");
                        continue;
                    }

                    // Successfully parsed - process heartbeat with sequence tracking and model metrics
                    ProcessHeartbeatFlatBuffer(hb_opt.value());

                } catch (const zmq::error_t& e) {
                    if (e.num() != EAGAIN) {  // EAGAIN = timeout, ignore
                        Logger::getInstance().log(std::string("AIHeartbeatMonitor receive error: ") + e.what());
                        throw;  // Reconnect on non-timeout errors
                    }
                }
            }

            Logger::getInstance().log("AIHeartbeatMonitor: Receive loop exiting");
            m_isRunning.store(false);
            break;  // Graceful shutdown, don't reconnect

        } catch (const zmq::error_t& e) {
            m_isRunning.store(false);

            // Elite: Exponential backoff on ZMQ errors
            if (!m_stopThread.load()) {
                Logger::getInstance().log(std::string("⚠️ AIHeartbeatMonitor ZMQ error: ") + e.what() +
                                         " | Reconnecting in " + std::to_string(reconnect_ms) + "ms");
                std::this_thread::sleep_for(std::chrono::milliseconds(reconnect_ms));
                reconnect_ms = std::min(MAX_RECONNECT_MS, reconnect_ms * BACKOFF_MULTIPLIER);
            }
        } catch (const std::exception& e) {
            m_isRunning.store(false);
            Logger::getInstance().log(std::string("ERROR: AIHeartbeatMonitor worker failed: ") + e.what());
            break;
        }
    }

    m_isRunning.store(false);
}

void AIHeartbeatMonitor::ProcessHeartbeatFlatBuffer(const MTS::HeartbeatData& hb) {
    // Guard: Don't process if we're shutting down
    if (m_stopThread.load()) return;

    if (!m_connectionMonitor) return;

    try {
        // Ghost Protocol v1: compute transport lag from heartbeat timestamp.
        const uint64_t now_us = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());

        uint64_t transport_lag_us = 0;
        if (hb.timestamp_us > 0 && now_us > hb.timestamp_us) {
            transport_lag_us = now_us - hb.timestamp_us;
        }

        const bool compute_fast = (
            hb.last_inference_ms < kComputeHealthyLastInferenceMs &&
            hb.avg_inference_ms < kComputeHealthyAvgInferenceMs
        );
        const bool transport_stall = (transport_lag_us > kTransportStallLagUs && compute_fast);
        const bool compute_stall = (
            hb.model_status != "READY" ||
            hb.avg_inference_ms > kComputeStallAvgInferenceMs ||
            hb.queue_depth > kComputeStallQueueDepth ||
            hb.error_count > 0
        );

        AIConnectionMonitor::Instance().UpdateTransportHealth(
            transport_stall,
            static_cast<float>(transport_lag_us) / 1000.0f
        );

        EmitGhostDiagnostic(transport_stall, compute_stall, transport_lag_us, hb);

        if (transport_stall) {
            Logger::getInstance().log(
                std::string("⚠️ GHOST v1: TRANSPORT_STALL detected (lag=") +
                std::to_string(transport_lag_us) + "us, compute=healthy)"
            );
        }

        // ===== ELITE: MODEL READINESS VALIDATION GATE =====
        // Reject metrics if model is not ready (prevents stale data)
        bool model_ready = hb.model_status == "READY";
        m_modelReady.store(model_ready);

        if (!model_ready) {
            Logger::getInstance().log(std::string("⚠️ Model not ready: ") + hb.model_status +
                                     " - skipping metrics update");
            m_connectionMonitor->UpdateHeartbeat(SCDateTime(0));
            return;  // Skip metric propagation until model ready
        }

        // ===== ELITE SEQUENCE TRACKING: DETECT DROPPED MESSAGES =====
        // Institutional-grade network reliability monitoring
        // Detects lost FlatBuffer messages between C++ and Python
        // Used for:
        //   1. Alerting operator of connection quality degradation
        //   2. Triggering reconnection logic if threshold exceeded
        //   3. Monitoring infrastructure health (ZMQ reliability)
        uint64_t last_seq = m_lastSequenceNumber.load();
        uint64_t dropped = MTS::EliteFlatBufferHelper::GetDroppedMessageCount(last_seq, hb.sequence_id);

        if (dropped > 0) {
            m_droppedMessageCount.fetch_add(dropped);
            uint64_t total_dropped = m_droppedMessageCount.load();

            // Log dropped messages (throttle at every 5th gap to avoid log spam)
            if (total_dropped % 5 == 0 || dropped > 100) {
                Logger::getInstance().log(
                    std::string("⚠️ ELITE: Dropped ") + std::to_string(dropped) +
                    std::string(" heartbeat FlatBuffer messages (seq ") +
                    std::to_string(last_seq) + std::string(" → ") + std::to_string(hb.sequence_id) +
                    std::string(") | Total dropped: ") + std::to_string(total_dropped)
                );

                // CRITICAL: If >100 consecutive drops, notify SystemOrchestrator
                if (dropped > 100) {
                    Logger::getInstance().log(std::string("🚨 CRITICAL: FlatBuffer sequence gap of ") +
                                             std::to_string(dropped) + " dropped heartbeats!");
                    SystemOrchestrator::Instance().SetAIHeartbeatHealth(false);
                }
            }
        }

        m_lastSequenceNumber.store(hb.sequence_id);

        // ===== ELITE: CONVERT timestamp_us (microseconds) TO SCDateTime =====
        // Elite: SCDateTime uses seconds precision (microseconds from FlatBuffer discarded)
        // Note: Sierra Chart SCDateTime does not support microsecond precision
        const double heartbeat_seconds = static_cast<double>(hb.timestamp_us) / 1'000'000.0;
        SCDateTime heartbeat_time(heartbeat_seconds);
        m_connectionMonitor->UpdateHeartbeat(heartbeat_time);

        // ===== UPDATE SYSTEM ORCHESTRATOR =====
        SystemOrchestrator::Instance().UpdateHeartbeat();
        SystemOrchestrator::Instance().SetAIHeartbeatHealth(true);
        SystemOrchestrator::Instance().ResetStrikeCount();

        // ===== STORE ELITE METRICS FOR EXTERNAL ACCESS =====
        // Elite: queue_depth semantics - Python ZMQ send queue depth
        // Thresholds: <10 = healthy, 10-50 = warning, >50 = critical
        m_lastInferenceMs.store(hb.last_inference_ms);
        m_avgInferenceMs.store(hb.avg_inference_ms);
        m_queueDepth.store(hb.queue_depth);
        m_errorCount.store(hb.error_count);

        // ===== ELITE: UPDATE AIConnectionMonitor WITH MODEL METRICS =====
        // Forward elite metrics to connection monitor for use by RiskManager, PositionManager
        AIConnectionMonitor::Instance().UpdateEliteMetrics(
            hb.last_inference_ms,
            hb.avg_inference_ms,
            hb.queue_depth,
            hb.error_count
        );

        // ===== INCREMENT COUNTER =====
        m_heartbeatCount.fetch_add(1);

        // ===== LOG ONLY ON STATUS TRANSITION / DEGRADED STATE =====
        // Suppress recurring nominal heartbeat logs; keep operator signal focused.
        const int count = m_heartbeatCount.load();
        const bool nominal = (
            hb.model_status == "READY" &&
            hb.queue_depth == 0 &&
            hb.error_count == 0
        );
        const bool was_nominal = m_lastHeartbeatNominal.exchange(nominal);
        constexpr uint64_t kDegradedHeartbeatLogIntervalUs = 60'000'000;  // 60s

        if (nominal) {
            if (!was_nominal) {
                Logger::getInstance().log(
                    std::string("✅ Heartbeat RECOVERED #") + std::to_string(count) +
                    std::string(" - Model: ") + hb.model_status +
                    std::string(" | Inference: ") + std::to_string(hb.last_inference_ms) + "ms" +
                    std::string(" | Queue: ") + std::to_string(hb.queue_depth) +
                    std::string(" | Errors: ") + std::to_string(hb.error_count)
                );
            }
        } else {
            const uint64_t last_log_us = m_lastHeartbeatStatusLogUs.load();
            if (was_nominal ||
                (now_us > last_log_us && (now_us - last_log_us) >= kDegradedHeartbeatLogIntervalUs)) {
                Logger::getInstance().log(
                    std::string("⚠️ Heartbeat DEGRADED #") + std::to_string(count) +
                    std::string(" - Model: ") + hb.model_status +
                    std::string(" | Inference: ") + std::to_string(hb.last_inference_ms) + "ms" +
                    std::string(" | Queue: ") + std::to_string(hb.queue_depth) +
                    std::string(" | Errors: ") + std::to_string(hb.error_count)
                );
                m_lastHeartbeatStatusLogUs.store(now_us);
            }
        }

    } catch (const std::exception& e) {
        Logger::getInstance().log(std::string("ERROR: ProcessHeartbeatFlatBuffer failed: ") + e.what());
    }
}

void AIHeartbeatMonitor::EmitGhostDiagnostic(
    bool transport_stall,
    bool compute_stall,
    uint64_t transport_lag_us,
    const MTS::HeartbeatData& hb)
{
    const uint64_t now_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());

    int mode = 0;
    if (transport_stall && compute_stall) {
        mode = 3;
    } else if (transport_stall) {
        mode = 1;
    } else if (compute_stall) {
        mode = 2;
    }

    const int last_mode = m_lastGhostMode.load();
    const uint64_t last_emit_us = m_lastGhostDiagnosticUs.load();
    const bool mode_changed = (mode != last_mode);
    const bool periodic_degraded = (mode != 0) && (now_us > last_emit_us) && ((now_us - last_emit_us) >= kGhostDiagnosticHeartbeatUs);

    if (!mode_changed && !periodic_degraded) {
        return;
    }

    std::string message;
    if (mode == 3) {
        message = "GHOST_DUAL_STALL";
    } else if (mode == 1) {
        message = "GHOST_TRANSPORT_STALL";
    } else if (mode == 2) {
        message = "GHOST_COMPUTE_STALL";
    } else {
        message = "GHOST_RECOVERED";
    }

    message += "|lag_us=" + std::to_string(transport_lag_us);
    message += "|avg_inf_ms=" + std::to_string(hb.avg_inference_ms);
    message += "|queue=" + std::to_string(hb.queue_depth);
    message += "|errors=" + std::to_string(hb.error_count);
    message += "|model=" + hb.model_status;

    const auto diagnostic_msg = MTS::EliteFlatBufferHelper::BuildDiagnostic(
        "C++_AIHeartbeatMonitor",
        message,
        static_cast<float>(transport_lag_us) / 1000.0f,
        hb.avg_inference_ms
    );

    std::vector<uint8_t> payload(
        static_cast<const uint8_t*>(diagnostic_msg.data()),
        static_cast<const uint8_t*>(diagnostic_msg.data()) + diagnostic_msg.size());
    MTS::Transport::TransportStream::Instance().Emit(std::move(payload));

    m_lastGhostMode.store(mode);
    m_lastGhostDiagnosticUs.store(now_us);
}
