#pragma once

#include <zmq.hpp>
#include <thread>
#include <atomic>
#include <memory>
#include <cstdint>
#include <mutex>
#include <string>
#include "messaging/EliteFlatBufferHelper.h"

// Forward declaration
class AIConnectionMonitor;

// [Full class documentation above]

/**
 * @class AIHeartbeatMonitor
 * @brief Elite FlatBuffer SUB socket listener for AI heartbeat messages
 * 
 * Institutional-grade heartbeat monitoring:
 * - FlatBuffer-only protocol (zero JSON)
 * - Runs on separate thread (non-blocking trade execution)
 * - Parses 11-field Heartbeat with elite model metrics
 * - Updates AIConnectionMonitor + SystemOrchestrator
 * - Sequence tracking for dropped message detection
 * - Zero-copy deserialization using GetRoot() pattern
 * 
 * Target latency: <10ms per heartbeat received
 * 
 * Architecture Pattern: Separate monitoring channel from execution channel
 * Used by: Renaissance, Citadel, Two Sigma for AI system health monitoring
 * 
 * Port Allocation:
 * - 5557: MindfulSocketZMQ (PUB) - Real-time indicator data to GUI
 * - 5558: TradeExecutionServer (REP) - Critical trade execution requests
 * - 5559: AIHeartbeatMonitor (SUB) - Elite FlatBuffer heartbeat monitoring
 * 
 * @see docs/ELITE_PROTOCOL_SPEC.md Section 3
 */
class AIHeartbeatMonitor {
public:
    static AIHeartbeatMonitor& Instance();

    AIHeartbeatMonitor(const AIHeartbeatMonitor&) = delete;
    AIHeartbeatMonitor& operator=(const AIHeartbeatMonitor&) = delete;

    /**
     * Initialize heartbeat monitor with reference to connection monitor
     * @param connectionMonitor Reference to AIConnectionMonitor for timestamp updates
     */
    void Init(AIConnectionMonitor& connectionMonitor);
    
    /**
     * Start heartbeat subscriber thread.
     * Waits for SetEndpoint() before connecting and subscribing.
     */
    void Start();
    
    /**
     * Disconnect SUB socket (allows reconnection)
     */
    void Disconnect();
    
    /**
     * Shutdown subscriber thread and cleanup ZMQ resources
     */
    void Shutdown();
    
    /**
     * Check if heartbeat monitor is running
     */
    bool IsRunning() const { return m_isRunning.load(); }

    /**
     * Configure runtime endpoint for Python AI heartbeat publisher.
     * If monitor is running, it will reconnect on the next receive-cycle.
     */
    void SetEndpoint(const std::string& endpoint);
    
    /**
     * Get count of heartbeats received since start
     */
    int GetHeartbeatCount() const { return m_heartbeatCount.load(); }

    /**
     * Returns whether the latest heartbeat reports model readiness.
     */
    bool IsModelReady() const { return m_modelReady.load(std::memory_order_acquire); }

private:
    AIHeartbeatMonitor();
    ~AIHeartbeatMonitor();

    /**
     * Worker thread function - subscribes to FlatBuffer heartbeat messages
     * Runs in background, non-blocking, updates AIConnectionMonitor on receipt
     * 
     * Protocol: Receives FlatBuffer Heartbeat (11 fields) every 1 second
     * No JSON parsing, zero-copy deserialization
     */
    void WorkerFunction();
    
    /**
     * Process received FlatBuffer Heartbeat message
     * Extracts all 11 fields, updates connection monitor and system state
     * @param hb Parsed HeartbeatData with all elite metrics
     */
    void ProcessHeartbeatFlatBuffer(const MTS::HeartbeatData& hb);

    void EmitGhostDiagnostic(
        bool transport_stall,
        bool compute_stall,
        uint64_t transport_lag_us,
        const MTS::HeartbeatData& hb);

    // ZMQ members (uses shared context from ZMQContextManager)
    std::unique_ptr<zmq::socket_t> m_socket;
    std::thread m_workerThread;
    std::atomic<bool> m_stopThread{false};
    std::atomic<bool> m_isRunning{false};
    
    // Reference to connection monitor for timestamp updates
    AIConnectionMonitor* m_connectionMonitor{nullptr};
    
    // Statistics
    std::atomic<int> m_heartbeatCount{0};

    // Runtime-configured endpoint set from handshake/control-plane negotiation.
    mutable std::mutex m_endpointMutex;
    std::string m_endpoint;
    
    // Elite: Sequence tracking for dropped message detection
    std::atomic<uint64_t> m_lastSequenceNumber{0};
    std::atomic<uint64_t> m_droppedMessageCount{0};
    
    // Elite: Model health metrics from latest heartbeat
    std::atomic<float> m_lastInferenceMs{0.0f};
    std::atomic<float> m_avgInferenceMs{0.0f};
    std::atomic<int> m_queueDepth{0};
    std::atomic<int> m_errorCount{0};
    std::atomic<bool> m_modelReady{false};

    // Ghost protocol diagnostic emission controls
    std::atomic<int> m_lastGhostMode{0};  // 0=nominal,1=transport,2=compute,3=dual
    std::atomic<uint64_t> m_lastGhostDiagnosticUs{0};

    // Heartbeat status logging controls (suppress nominal spam, log transitions/degraded only)
    std::atomic<bool> m_lastHeartbeatNominal{true};
    std::atomic<uint64_t> m_lastHeartbeatStatusLogUs{0};
    
};
