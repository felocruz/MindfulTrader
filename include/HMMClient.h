#pragma once

#include "MindfulTrader_Precompiled.h"
#include "generated/mts_schema_contract_generated.h"
#include <thread>
#include <atomic>
#include <array>
#include <chrono>
#include <vector>
#include <zmq.h>

// Forward declarations
class HMMRegimeIndicator;

/**
 * @brief Elite DEALER/ROUTER async HMM client
 *
 * Architecture:
 * - Study thread sends observation via RequestUpdateAsync() (non-blocking)
 * - Worker thread polls DEALER socket and inproc signal
 * - Responses update HMMRegimeIndicator atomically (thread-safe)
 * - Study thread reads cached values from indicator (never blocks)
 *
 * Port: 5561 (DEALER -> Python ROUTER)
 * Pattern: Fire observation, continue immediately, use cached values
 */
class HMMClient {
public:
    using ObservationArray = MTS::Schema::Contract::ObservationArray;

    // Singleton pattern
    static HMMClient& Instance();

    // Delete copy/move
    HMMClient(const HMMClient&) = delete;
    HMMClient& operator=(const HMMClient&) = delete;

    /**
     * @brief Initialize DEALER socket and start worker thread
        * @param host Python HMM Server host (from negotiated runtime config)
    * @param port HMM Server port (from negotiated runtime config)
     */
    void Init(const std::string& host = "", int port = 0);

    /**
     * @brief Shutdown worker thread and close socket
     */
    void Shutdown();

    /**
    * @brief Request HMM update (HMM 16D)
    * @param observation 16D MarketObservation vector (Strict HMM Physics)
     * @param bars_since_last_update SystemState bars_since_last_update value
     * @param sequence_id Global sequence ID for correlation
     * @param timestamp_us Observation timestamp (UNIX microseconds)
     */
    void RequestUpdateAsync(
        const ObservationArray& observation,
        float bars_since_last_update,
        uint64_t sequence_id,
        uint64_t timestamp_us
    );

    /**
     * @brief Check if worker thread is running
     */
    bool IsRunning() const { return m_running.load(); }

    // Student-t tail diagnostics now live in HmmStateIndicator (single source of truth).
    // Access via: InferenceManager::Instance().HmmState()->Dof() etc.

private:
    HMMClient();
    ~HMMClient();

    // Worker thread main loop
    void WorkerThread();

    // Rebuild DEALER socket against the current active host candidate.
    bool ReconnectDealerSocket(const std::string& reason);

    // Rotate to next host candidate after repeated write-not-ready failures.
    bool AttemptHostFailover(const std::string& reason);

    // Send FlatBuffer binary request to HMM server
    void SendBinaryRequest();

    // Handle binary response from HMM server
    void HandleBinaryResponse(const uint8_t* data, size_t size);
    void MaybeLogValidationSummary(bool force);

    // ZMQ context and sockets
    void* m_context;
    void* m_dealer;        // DEALER socket (async req/rep with Python)
    void* m_inproc_signal; // Inproc PAIR for study->worker communication

    // Threading
    std::thread m_worker;
    std::atomic<bool> m_running;

    // Connection info
    std::string m_endpoint;
    std::vector<std::string> m_hostCandidates;
    size_t m_activeHostIndex{0};
    int m_resolvedPort{5561};
    uint32_t m_consecutiveWriteNotReady{0};
    std::chrono::steady_clock::time_point m_lastFailoverAttempt{};

    // Latest observation (read by worker thread)
    ObservationArray m_latestObservation;
    float m_latestBarsSinceLastUpdate;
    std::atomic<bool> m_hasNewObservation;
    std::mutex m_obsMutex;
    uint64_t m_latestSequenceId;
    uint64_t m_latestTimestampUs;

    // Sequence ID that was actually transmitted on the wire (worker thread only).
    // Separated from m_latestSequenceId (which advances on every queued observation)
    // to prevent false-stale rejection of in-flight responses.
    std::atomic<uint64_t> m_lastSentSequenceId{0};

    // Student-t tail diagnostics removed — now stored in HmmStateIndicator.
    // See Indicator.h: HmmStateIndicator::Dof(), Mahalanobis(), TailWeight(), ExpectedDuration().

    // Validation observability counters
    std::atomic<uint64_t> m_responseCount;
    std::atomic<uint64_t> m_staleSequenceRejectCount;
    std::atomic<uint64_t> m_timestampRejectCount;
    std::atomic<uint64_t> m_probabilityRejectCount;
    std::atomic<uint64_t> m_probabilityClipNormalizeCount;

    // Send-path resilience counters for warning throttling.
    std::atomic<uint64_t> m_sendEagainCount{0};
    std::atomic<uint64_t> m_sendHardFailCount{0};
    std::chrono::steady_clock::time_point m_lastSendWarningLog{};
};
