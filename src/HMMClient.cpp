#include "MindfulTrader_Precompiled.h"
#include "HMMClient.h"
#include "IndicatorManager.h"
#include "ContextManager.h"
#include <algorithm>
#include <array>
#include <cerrno>
#include <cinttypes>
#include <cmath>
#include <chrono>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include "RiskManager.h"
#include <fstream>
#include <iomanip>
#include <sstream>
#include <unordered_set>
#include <vector>
#include "nlohmann/json.hpp"

namespace {
constexpr std::chrono::seconds kSendWarningThrottleWindow(2);
constexpr std::chrono::seconds kFailoverCooldownWindow(2);
constexpr uint32_t kFailoverWriteNotReadyThreshold = 9;
constexpr uint64_t kPipelineTraceSampleEvery = 8;
constexpr uint64_t kObsDigestFnvOffset = 1469598103934665603ULL;
constexpr uint64_t kObsDigestFnvPrime = 1099511628211ULL;

uint64_t ComputeObservationDigest(const HMMClient::ObservationArray& obs) {
    uint64_t digest = kObsDigestFnvOffset;
    for (float value : obs) {
        uint32_t bits = 0;
        std::memcpy(&bits, &value, sizeof(bits));
        for (int byte_idx = 0; byte_idx < 4; ++byte_idx) {
            const uint8_t b = static_cast<uint8_t>((bits >> (byte_idx * 8)) & 0xFFu);
            digest ^= b;
            digest *= kObsDigestFnvPrime;
        }
    }
    return digest;
}

std::vector<std::string> ResolveHmmHostCandidates(const std::string& host_arg) {
    std::vector<std::string> ordered;
    std::unordered_set<std::string> seen;

    const auto add_if_new = [&ordered, &seen](const std::string& host) {
        if (host.empty()) {
            return;
        }
        if (seen.insert(host).second) {
            ordered.push_back(host);
        }
    };

    add_if_new(host_arg);

    const char* env_host = std::getenv("MTS_HMM_HOST");
    if (env_host && *env_host) {
        add_if_new(std::string(env_host));
    }

    return ordered;
}

int ResolveHmmPort(int port_arg) {
    if (port_arg > 0) {
        return port_arg;
    }
    const char* env_port = std::getenv("MTS_HMM_PORT");
    if (env_port && *env_port) {
        try {
            const int parsed = std::stoi(env_port);
            if (parsed > 0 && parsed <= 65535) {
                return parsed;
            }
        } catch (...) {
            Logger::getInstance().log("HMMClient::Init WARNING: ignoring invalid MTS_HMM_PORT value");
        }
    }
    return 0;
}
}
HMMClient::HMMClient()
    : m_context(nullptr)
    , m_dealer(nullptr)
    , m_inproc_signal(nullptr)
    , m_running(false)
    , m_latestBarsSinceLastUpdate(0.0f)
    , m_hasNewObservation(false)
    , m_latestSequenceId(0)
    , m_latestTimestampUs(0)
    , m_responseCount(0)
    , m_staleSequenceRejectCount(0)
    , m_timestampRejectCount(0)
    , m_probabilityRejectCount(0)
    , m_probabilityClipNormalizeCount(0)
{
    m_latestObservation.fill(0.0f);
}

HMMClient::~HMMClient() {
    Shutdown();
}

HMMClient& HMMClient::Instance() {
    static HMMClient instance;
    return instance;
}

void HMMClient::Init(const std::string& host, int port) {
    if (m_running.load()) return;

    m_hostCandidates = ResolveHmmHostCandidates(host);
    m_activeHostIndex = 0;
    m_resolvedPort = ResolveHmmPort(port);
    m_consecutiveWriteNotReady = 0;
    m_lastFailoverAttempt = std::chrono::steady_clock::now() - kFailoverCooldownWindow;

    if (m_hostCandidates.empty()) {
        Logger::getInstance().log("HMMClient::Init ERROR: No valid HMM host candidates resolved");
        return;
    }

    if (m_resolvedPort <= 0 || m_resolvedPort > 65535) {
        Logger::getInstance().log("HMMClient::Init ERROR: No valid HMM port resolved");
        return;
    }

    m_context = zmq_ctx_new();

    if (!m_context) {
        Logger::getInstance().log("HMMClient::Init ERROR: Failed to create ZMQ context");
        return;
    }

    m_lastSendWarningLog = std::chrono::steady_clock::now() - kSendWarningThrottleWindow;
    m_sendEagainCount.store(0, std::memory_order_relaxed);
    m_sendHardFailCount.store(0, std::memory_order_relaxed);

    if (!ReconnectDealerSocket("initialization")) {
        if (m_context) {
            zmq_ctx_destroy(m_context);
            m_context = nullptr;
        }
        return;
    }

    std::ostringstream host_list;
    for (size_t i = 0; i < m_hostCandidates.size(); ++i) {
        if (i > 0) {
            host_list << ",";
        }
        host_list << m_hostCandidates[i];
    }
    Logger::getInstance().log("HMMClient::Init: Host candidates=" + host_list.str());

    m_inproc_signal = zmq_socket(m_context, ZMQ_PAIR);
    if (!m_inproc_signal) {
        Logger::getInstance().log("HMMClient::Init ERROR: Failed to create inproc signal socket");
        zmq_close(m_dealer);
        m_dealer = nullptr;
        zmq_ctx_destroy(m_context);
        m_context = nullptr;
        return;
    }

    if (zmq_bind(m_inproc_signal, "inproc://hmm_signal") != 0) {
        int err = zmq_errno();
        Logger::getInstance().log("HMMClient::Init ERROR: Failed to bind inproc signal (" + std::string(zmq_strerror(err)) + ")");
        zmq_close(m_inproc_signal);
        zmq_close(m_dealer);
        m_inproc_signal = nullptr;
        m_dealer = nullptr;
        zmq_ctx_destroy(m_context);
        m_context = nullptr;
        return;
    }

    m_running.store(true);
    m_worker = std::thread(&HMMClient::WorkerThread, this);
    Logger::getInstance().log("HMMClient::Init: Worker thread started successfully");
}

bool HMMClient::ReconnectDealerSocket(const std::string& reason) {
    if (!m_context) {
        Logger::getInstance().log("HMMClient::ReconnectDealerSocket ERROR: null context");
        return false;
    }

    if (m_activeHostIndex >= m_hostCandidates.size()) {
        Logger::getInstance().log("HMMClient::ReconnectDealerSocket ERROR: active host index out of range");
        return false;
    }

    if (m_dealer) {
        zmq_close(m_dealer);
        m_dealer = nullptr;
    }

    m_dealer = zmq_socket(m_context, ZMQ_DEALER);
    if (!m_dealer) {
        Logger::getInstance().log("HMMClient::ReconnectDealerSocket ERROR: failed to create DEALER socket");
        return false;
    }

    int hwm = 1;
    zmq_setsockopt(m_dealer, ZMQ_SNDHWM, &hwm, sizeof(hwm));
    zmq_setsockopt(m_dealer, ZMQ_RCVHWM, &hwm, sizeof(hwm));

    int linger = 0;
    zmq_setsockopt(m_dealer, ZMQ_LINGER, &linger, sizeof(linger));

    int immediate = 1;
    zmq_setsockopt(m_dealer, ZMQ_IMMEDIATE, &immediate, sizeof(immediate));

    m_endpoint = "tcp://" + m_hostCandidates[m_activeHostIndex] + ":" + std::to_string(m_resolvedPort);
    if (zmq_connect(m_dealer, m_endpoint.c_str()) != 0) {
        int err = zmq_errno();
        Logger::getInstance().log(
            "HMMClient::ReconnectDealerSocket ERROR: Failed to connect to " + m_endpoint +
            " (reason=" + reason + ", errno=" + std::to_string(err) + ": " + zmq_strerror(err) + ")"
        );
        zmq_close(m_dealer);
        m_dealer = nullptr;
        return false;
    }

    Logger::getInstance().log(
        "HMMClient::ReconnectDealerSocket: Connected to " + m_endpoint +
        " (reason=" + reason + ")"
    );
    return true;
}

bool HMMClient::AttemptHostFailover(const std::string& reason) {
    if (m_hostCandidates.size() <= 1) {
        return false;
    }

    const auto now = std::chrono::steady_clock::now();
    if ((now - m_lastFailoverAttempt) < kFailoverCooldownWindow) {
        return false;
    }

    const size_t prior_index = m_activeHostIndex;
    const size_t next_index = (m_activeHostIndex + 1) % m_hostCandidates.size();
    if (next_index == prior_index) {
        return false;
    }

    m_activeHostIndex = next_index;
    m_lastFailoverAttempt = now;

    Logger::getInstance().log(
        "HMMClient::AttemptHostFailover: rotating endpoint " +
        m_hostCandidates[prior_index] + " -> " + m_hostCandidates[next_index] +
        " (reason=" + reason + ")"
    );

    if (!ReconnectDealerSocket("failover:" + reason)) {
        m_activeHostIndex = prior_index;
        ReconnectDealerSocket("failover-revert");
        return false;
    }

    m_consecutiveWriteNotReady = 0;
    return true;
}

void HMMClient::Shutdown() {
    if (!m_running.load()) {
        Logger::getInstance().log("HMMClient::Shutdown: Already shutdown or not initialized");
        return;
    }
    Logger::getInstance().log("HMMClient::Shutdown: Initiating shutdown...");
    MaybeLogValidationSummary(true);
    m_running.store(false);

    if (m_inproc_signal) {
        const char* msg = "QUIT";
        zmq_send(m_inproc_signal, msg, 4, ZMQ_DONTWAIT);
    }

    if (m_worker.joinable()) m_worker.join();

    if (m_inproc_signal) zmq_close(m_inproc_signal);
    if (m_dealer) zmq_close(m_dealer);
    if (m_context) zmq_ctx_destroy(m_context);

    m_inproc_signal = nullptr;
    m_dealer = nullptr;
    m_context = nullptr;
}

void HMMClient::RequestUpdateAsync(
    const ObservationArray& observation,
    float bars_since_last_update,
    uint64_t sequence_id,
    uint64_t timestamp_us) {
    if (!m_running.load()) {
        Logger::getInstance().log("HMMClient::RequestUpdateAsync WARNING: Called while not running (seq=" + std::to_string(sequence_id) + ")");
        return;
    }

    // Validation boundary is ContextManager::CheckAndTriggerHMM.
    // If RequestUpdateAsync is called, observation payload is already trusted-valid.

    {
        std::lock_guard<std::mutex> lock(m_obsMutex);
        m_latestObservation = observation;
        m_latestBarsSinceLastUpdate = bars_since_last_update;
        m_latestSequenceId = sequence_id;
        m_latestTimestampUs = timestamp_us;
        m_hasNewObservation.store(true);
    }

    if (m_inproc_signal) {
        const char* msg = "GO";
        zmq_send(m_inproc_signal, msg, 2, ZMQ_DONTWAIT);
    }
}

void HMMClient::WorkerThread() {
    void* worker_signal = zmq_socket(m_context, ZMQ_PAIR);
    zmq_connect(worker_signal, "inproc://hmm_signal");

    zmq_pollitem_t items[] = {
        { m_dealer, 0, ZMQ_POLLIN, 0 },      // Feedback from Python (Inference)
        { worker_signal, 0, ZMQ_POLLIN, 0 }  // Signal from Study (Observation)
    };

    while (m_running.load()) {
        int rc = zmq_poll(items, 2, 500);
        if (rc < 0) {
            int err = zmq_errno();
            Logger::getInstance().log("HMMClient::WorkerThread ERROR: zmq_poll failed (errno=" + std::to_string(err) + ": " + zmq_strerror(err) + ")");
            break;
        }

        // 1. Outgoing: Send 16D Observation to Python
        if (items[1].revents & ZMQ_POLLIN) {
            char cmd[8];
            int size = zmq_recv(worker_signal, cmd, sizeof(cmd), ZMQ_DONTWAIT);
            if (size > 0) {
                // Priority 1: Handle Shutdown Signal immediately
                if (size == 4 && memcmp(cmd, "QUIT", 4) == 0) break;

                // Priority 2: Handle New Observation
                if (m_hasNewObservation.load()) {
                    SendBinaryRequest();
                    m_hasNewObservation.store(false);
                }
            }
        }

        // 2. Incoming: Process Integer Regime IDs from Python
        if (items[0].revents & ZMQ_POLLIN) {
            zmq_msg_t msg;
            zmq_msg_init(&msg);
            int size = zmq_msg_recv(&msg, m_dealer, ZMQ_DONTWAIT);
            if (size > 0) {
                HandleBinaryResponse(static_cast<const uint8_t*>(zmq_msg_data(&msg)), size);
            }
            zmq_msg_close(&msg);
        }
    }
    zmq_close(worker_signal);
}

void HMMClient::SendBinaryRequest() {
    uint64_t seq;
    uint64_t ts_us;
    float bars_since_last_update;
    ObservationArray obs;

    {
        std::lock_guard<std::mutex> lock(m_obsMutex);
        seq = m_latestSequenceId;
        ts_us = m_latestTimestampUs;
        bars_since_last_update = m_latestBarsSinceLastUpdate;
        obs = m_latestObservation;
    }

    const uint64_t obs_digest = ComputeObservationDigest(obs);

    if (seq <= 3 || (seq % kPipelineTraceSampleEvery) == 0) {
        char buf[320];
        std::snprintf(buf, sizeof(buf),
            "[PIPELINE_TRACE][1/8] C++ -> Python HMM request "
            "(topic_pair=MARKET_OBSERVATION+SYSTEM_STATE"
            ", seq=%" PRIu64 ", ts_us=%" PRIu64
            ", bars_since_last_update=%.2f"
            ", obs0=%.4f, obs1=%.4f, obs2=%.4f, obs_dim=16"
            ", obs_digest=0x%016" PRIx64 ")",
            seq, ts_us, bars_since_last_update, obs[0], obs[1], obs[2], obs_digest);
        Logger::getInstance().log(buf);
    }

    // Build both payloads first to enforce paired-send contract with shared sequence_id.
    std::vector<uint8_t> market_payload;
    std::vector<uint8_t> system_payload;

    // ========================================================================
    // MARKET OBSERVATION (Institutional Physics-Based 16D)
    // ========================================================================
    {
        flatbuffers::FlatBufferBuilder builder(512);

        // 1. Construct canonical ObservationData using generated contract mapping.
        const MTS::Schema::ObservationData obs_data = MTS::Schema::Contract::MakeObservationData(obs);

        // 2. Assemble Table
        // MarketObservationBuilder takes pointers to the structs
        MTS::Schema::MarketObservationBuilder mob(builder);
        mob.add_timestamp_us(static_cast<int64_t>(ts_us));
        mob.add_sequence_id(seq);
        mob.add_observation(&obs_data);
        // mob.add_asymmetry_context(&asym); // Removed: 8D Context is Transformer-Only (via Event stream)

        auto mo = mob.Finish();
        builder.Finish(mo);
        market_payload.assign(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
    }

    {
        flatbuffers::FlatBufferBuilder builder(256);
        MTS::Schema::SystemStateBuilder ssb(builder);
        ssb.add_timestamp_us(static_cast<int64_t>(ts_us));
        ssb.add_sequence_id(seq);
        ssb.add_bars_since_last_update(bars_since_last_update);
        auto ss = ssb.Finish();
        builder.Finish(ss);
        system_payload.assign(builder.GetBufferPointer(), builder.GetBufferPointer() + builder.GetSize());
    }

    constexpr const char* kTopicMarketObservation = "MARKET_OBSERVATION";
    constexpr const char* kTopicSystemState = "SYSTEM_STATE";

    // Atomic 4-frame multipart: [MO_topic | MO_payload | SS_topic | SS_payload]
    // SNDMORE on first 3 frames guarantees ZMQ delivers all 4 atomically,
    // eliminating the half-pair condition where MO arrives but SS is lost.
    auto send_atomic_pair = [this](
        const char* mo_topic, const std::vector<uint8_t>& mo_payload,
        const char* ss_topic, const std::vector<uint8_t>& ss_payload) -> bool {
        if (zmq_send(m_dealer, mo_topic, std::strlen(mo_topic), ZMQ_SNDMORE | ZMQ_DONTWAIT) < 0)
            return false;
        if (zmq_send(m_dealer, mo_payload.data(), mo_payload.size(), ZMQ_SNDMORE | ZMQ_DONTWAIT) < 0)
            return false;
        if (zmq_send(m_dealer, ss_topic, std::strlen(ss_topic), ZMQ_SNDMORE | ZMQ_DONTWAIT) < 0)
            return false;
        if (zmq_send(m_dealer, ss_payload.data(), ss_payload.size(), ZMQ_DONTWAIT) < 0)
            return false;
        return true;
    };

    constexpr int kMaxPairSendAttempts = 3;
    bool pair_sent = false;

    zmq_pollitem_t out_item[] = {
        {m_dealer, 0, ZMQ_POLLOUT, 0}
    };

    auto maybe_log_send_warning = [this](auto&& msg_fn) {
        const auto now = std::chrono::steady_clock::now();
        if (now - m_lastSendWarningLog >= kSendWarningThrottleWindow) {
            const uint64_t eagain = m_sendEagainCount.load(std::memory_order_relaxed);
            const uint64_t hard_fail = m_sendHardFailCount.load(std::memory_order_relaxed);
            char suffix[80];
            std::snprintf(suffix, sizeof(suffix),
                " [send_eagain_total=%" PRIu64 ", send_hard_fail_total=%" PRIu64 "]",
                eagain, hard_fail);
            Logger::getInstance().log(std::string(msg_fn()) + suffix);
            m_lastSendWarningLog = now;
        }
    };

    for (int attempt = 1; attempt <= kMaxPairSendAttempts; ++attempt) {
        const int poll_rc = zmq_poll(out_item, 1, 5);
        if (poll_rc <= 0 || !(out_item[0].revents & ZMQ_POLLOUT)) {
            m_sendEagainCount.fetch_add(1, std::memory_order_relaxed);
            ++m_consecutiveWriteNotReady;
            maybe_log_send_warning([&]() {
                char buf[160];
                std::snprintf(buf, sizeof(buf),
                    "HMMClient::SendBinaryRequest WARNING: dealer not write-ready "
                    "(seq=%" PRIu64 ", attempt=%d/%d)",
                    seq, attempt, kMaxPairSendAttempts);
                return std::string(buf);
            });
            if (m_consecutiveWriteNotReady >= kFailoverWriteNotReadyThreshold) {
                AttemptHostFailover("write_not_ready_threshold");
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
            continue;
        }

        m_consecutiveWriteNotReady = 0;

        if (send_atomic_pair(kTopicMarketObservation, market_payload,
                             kTopicSystemState, system_payload)) {
            m_lastSentSequenceId.store(seq, std::memory_order_release);
            pair_sent = true;
            break;
        }

        const int err = zmq_errno();
        if (err == EAGAIN) {
            m_sendEagainCount.fetch_add(1, std::memory_order_relaxed);
            ++m_consecutiveWriteNotReady;
        } else {
            m_sendHardFailCount.fetch_add(1, std::memory_order_relaxed);
            ++m_consecutiveWriteNotReady;
        }
        maybe_log_send_warning([&]() {
            char buf[192];
            std::snprintf(buf, sizeof(buf),
                "HMMClient::SendBinaryRequest WARNING: paired send incomplete "
                "(seq=%" PRIu64 ", attempt=%d/%d, errno=%d: %s)",
                seq, attempt, kMaxPairSendAttempts, err, zmq_strerror(err));
            return std::string(buf);
        });

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    if (!pair_sent) {
        m_sendHardFailCount.fetch_add(1, std::memory_order_relaxed);
        maybe_log_send_warning([&]() {
            char buf[160];
            std::snprintf(buf, sizeof(buf),
                "HMMClient::SendBinaryRequest ERROR: failed to send aligned "
                "MARKET_OBSERVATION+SYSTEM_STATE pair after retries (seq=%" PRIu64 ")", seq);
            return std::string(buf);
        });
    }
}

void HMMClient::HandleBinaryResponse(const uint8_t* data, size_t size) {
    constexpr uint64_t kMaxStaleTimestampDriftUs = 250000;  // 250ms
    constexpr uint64_t kMaxFutureTimestampLeadUs = 50000;   // 50ms

    m_responseCount.fetch_add(1, std::memory_order_relaxed);

    // Validate buffer size
    if (size < sizeof(MTS::Schema::MTS_Envelope)) {
        Logger::getInstance().log("HMMClient::HandleBinaryResponse ERROR: Buffer too small (" + std::to_string(size) + " bytes)");
        return;
    }

    flatbuffers::Verifier envelopeVerifier(data, size);
    if (!MTS::Schema::VerifyMTS_EnvelopeBuffer(envelopeVerifier)) {
        Logger::getInstance().log("HMMClient::HandleBinaryResponse ERROR: Invalid MTS_Envelope buffer");
        return;
    }

    auto envelope = ::flatbuffers::GetRoot<MTS::Schema::MTS_Envelope>(data);
    if (!envelope) {
        Logger::getInstance().log("HMMClient::HandleBinaryResponse ERROR: Failed to deserialize envelope");
        return;
    }

    if (envelope->data_type() != MTS::Schema::Message_RiskStateUpdate) {
        Logger::getInstance().log("HMMClient::HandleBinaryResponse WARNING: Received non-RiskStateUpdate message (type=" +
                                std::to_string(envelope->data_type()) + ")");
        return;
    }

    auto regimeUpdate = envelope->data_as_RiskStateUpdate();
    if (!regimeUpdate) {
        Logger::getInstance().log("HMMClient::HandleBinaryResponse ERROR: Failed to cast to RiskStateUpdate");
        return;
    }

    // --- 1. Handshake Check ---
    // Compare against the sequence ID that was actually *sent* on the wire,
    // not m_latestSequenceId (which advances whenever a new observation is queued).
    // This eliminates false-stale rejections when a new observation arrives
    // while Python is still processing the previous one.
    const uint64_t lastSentSeq = m_lastSentSequenceId.load(std::memory_order_acquire);
    uint64_t latestTimestampUsSnapshot = 0;
    {
        std::lock_guard<std::mutex> lock(m_obsMutex);
        latestTimestampUsSnapshot = m_latestTimestampUs;
    }

    uint64_t returnedSeq = regimeUpdate->sequence_id();
    if (returnedSeq < lastSentSeq) {
        m_staleSequenceRejectCount.fetch_add(1, std::memory_order_relaxed);
        char buf[128];
        std::snprintf(buf, sizeof(buf),
            "HMMClient::HandleBinaryResponse WARNING: Stale inference "
            "(received=%" PRIu64 ", expected>=%" PRIu64 ")",
            returnedSeq, lastSentSeq);
        Logger::getInstance().log(buf);
        MaybeLogValidationSummary(false);
        return;
    }

    const int64_t responseTimestampRaw = regimeUpdate->timestamp_us();
    if (responseTimestampRaw <= 0) {
        m_timestampRejectCount.fetch_add(1, std::memory_order_relaxed);
        char buf[96];
        std::snprintf(buf, sizeof(buf),
            "HMMClient::HandleBinaryResponse WARNING: Invalid response timestamp_us (%" PRId64 ")",
            responseTimestampRaw);
        Logger::getInstance().log(buf);
        MaybeLogValidationSummary(false);
        return;
    }

    const uint64_t responseTimestampUs = static_cast<uint64_t>(responseTimestampRaw);
    const uint64_t staleBoundary = (latestTimestampUsSnapshot > kMaxStaleTimestampDriftUs)
        ? (latestTimestampUsSnapshot - kMaxStaleTimestampDriftUs)
        : 0;
    const uint64_t futureBoundary = latestTimestampUsSnapshot + kMaxFutureTimestampLeadUs;

    if (responseTimestampUs < staleBoundary || responseTimestampUs > futureBoundary) {
        m_timestampRejectCount.fetch_add(1, std::memory_order_relaxed);
        char buf[224];
        std::snprintf(buf, sizeof(buf),
            "HMMClient::HandleBinaryResponse WARNING: Rejected out-of-budget response timestamp "
            "(response_ts_us=%" PRIu64 ", latest_request_ts_us=%" PRIu64
            ", stale_budget_us=%" PRIu64 ", future_budget_us=%" PRIu64 ")",
            responseTimestampUs, latestTimestampUsSnapshot,
            kMaxStaleTimestampDriftUs, kMaxFutureTimestampLeadUs);
        Logger::getInstance().log(buf);
        MaybeLogValidationSummary(false);
        return;
    }

    const auto* hmmRisk = regimeUpdate->hmm_risk();
    if (!hmmRisk) {
        Logger::getInstance().log("HMMClient::HandleBinaryResponse ERROR: Missing hmm_risk payload");
        return;
    }

    constexpr int kModelStateCount = 4;
    constexpr int kUnknownDisplayState = 4;

    int state_id = hmmRisk->primary_state();
    bool unknown_posture = false;

    int climate_id = regimeUpdate->climate();
    if (climate_id < 0 || climate_id > 4) {
        Logger::getInstance().log("HMMClient::HandleBinaryResponse ERROR: Invalid market climate ID (" + std::to_string(climate_id) + ")");
        return;
    }

    std::array<float, kModelStateCount> sanitized_probs{};
    const float* probs_data = nullptr;
    size_t probs_count = 0;

    const auto* probs = hmmRisk->probability_vector();
    const size_t raw_prob_count = probs ? probs->size() : 0;

    if (state_id < 0 || state_id >= kModelStateCount) {
        unknown_posture = true;
    }

    if (!probs || raw_prob_count != static_cast<size_t>(kModelStateCount)) {
        m_probabilityRejectCount.fetch_add(1, std::memory_order_relaxed);
        Logger::getInstance().log(
            "HMMClient::HandleBinaryResponse ERROR: Invalid probability_vector size "
            "(expected=4, got=" + std::to_string(raw_prob_count) + ")"
        );
        MaybeLogValidationSummary(false);
        return;
    } else {
        float sum = 0.0f;
        bool clipped_values = false;
        bool has_non_finite = false;

        sanitized_probs.fill(0.0f);
        for (size_t i = 0; i < raw_prob_count; ++i) {
            const float raw = probs->Get(i);
            if (!std::isfinite(raw)) {
                has_non_finite = true;
                break;
            }

            const float clipped = std::max(0.0f, std::min(raw, 1.0f));
            if (clipped != raw) {
                clipped_values = true;
            }
            const int mapped_idx = static_cast<int>(i);

            if (mapped_idx < 0 || mapped_idx >= kModelStateCount) {
                has_non_finite = true;
                break;
            }

            sanitized_probs[static_cast<size_t>(mapped_idx)] += clipped;
            sum += clipped;
        }

        if (has_non_finite || sum <= 1e-6f) {
            m_probabilityRejectCount.fetch_add(1, std::memory_order_relaxed);
            Logger::getInstance().log(
                std::string("HMMClient::HandleBinaryResponse ERROR: Rejected invalid probability_vector ") +
                "(reason=" + (has_non_finite ? "non_finite" : "sum_zero") + ")"
            );
            MaybeLogValidationSummary(false);
            return;
        } else {
            if (std::fabs(sum - 1.0f) > 0.01f) {
                clipped_values = true;
                for (float& value : sanitized_probs) {
                    value /= sum;
                }
            }

            probs_data = sanitized_probs.data();
            probs_count = sanitized_probs.size();

            if (clipped_values) {
                m_probabilityClipNormalizeCount.fetch_add(1, std::memory_order_relaxed);
                Logger::getInstance().log(
                    "HMMClient::HandleBinaryResponse WARNING: Clipped/normalized probability_vector before sizing"
                );
            }
        }
    }

    // Student-t tail diagnostics are now stored in HmmStateIndicator (single source of truth).
    // No local atomic storage — pass through to SetState() below.

    const float risk_multiplier = RiskManager::Instance().ComputeInstitutionalRiskMultiplier(
        state_id,
        probs_data,
        probs_count,
        hmmRisk->entropy(),
        hmmRisk->transition_risk(),
        hmmRisk->dof(),
        unknown_posture);

    // --- 2. State Injection ---
    auto* stateInd = InferenceManager::Instance().MutableHmmState();
    auto* climateInd = InferenceManager::Instance().MutableClimate();

    if (!stateInd) {
        Logger::getInstance().log("HMMClient::HandleBinaryResponse ERROR: HmmState not found");
        return;
    }

    if (!climateInd) {
        Logger::getInstance().log("HMMClient::HandleBinaryResponse ERROR: MarketClimate not found");
        return;
    }

    const int display_state_id = unknown_posture ? kUnknownDisplayState : state_id;

    // === ATOMIC REGIME-TRIGGERED RE-INFERENCE ===
    // When the HMM regime changes, Python runs a Transformer re-inference and
    // piggybacks the resulting action_id in the same RiskStateUpdate message.
    // Writing PredictionState here — on the same worker thread, in the same
    // message handler — eliminates the coherence gap between HMM state and
    // Transformer prediction.  -1 means no re-inference occurred.
    {
        const int8_t reinferAction = regimeUpdate->reinfer_action_id();
        if (reinferAction >= 0 && reinferAction <= 8) {
            InferenceManager::Instance().MutablePrediction()->Update(
                static_cast<TradeActionEnum>(reinferAction));
        }
    }

    stateInd->SetState(display_state_id, risk_multiplier, hmmRisk->transition_risk(),
                       hmmRisk->dof(), hmmRisk->mahalanobis_distance(),
                       hmmRisk->tail_weight(), hmmRisk->expected_duration(),
                       hmmRisk->entropy());

    // Stamp HMM freshness immediately after state injection so UI-thread
    // staleness checks reflect the most recent Python round-trip.
    {
        const auto now = std::chrono::steady_clock::now();
        const uint64_t nowUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                now.time_since_epoch()).count());
        InferenceManager::Instance().MarkHmmStateUpdated(nowUs);
    }

    climateInd->SetClimate(climate_id);

    if (returnedSeq <= 3 || (returnedSeq % kPipelineTraceSampleEvery) == 0) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "[PIPELINE_TRACE][3/8] C++ received HMM state from Python "
            "(seq=%" PRIu64 ", state=%d, climate=%d"
            ", entropy=%.4f, transition_risk=%.4f, dof=%.2f)",
            returnedSeq, state_id, climate_id,
            hmmRisk->entropy(), hmmRisk->transition_risk(), hmmRisk->dof());
        Logger::getInstance().log(buf);
    }

    MaybeLogValidationSummary(false);
}

void HMMClient::MaybeLogValidationSummary(bool force) {
    constexpr uint64_t kSummaryIntervalResponses = 200;

    const uint64_t total = m_responseCount.load(std::memory_order_relaxed);
    if (total == 0) {
        return;
    }

    if (!force && (total % kSummaryIntervalResponses) != 0) {
        return;
    }

    const uint64_t stale_seq = m_staleSequenceRejectCount.load(std::memory_order_relaxed);
    const uint64_t ts_reject = m_timestampRejectCount.load(std::memory_order_relaxed);
    const uint64_t prob_reject = m_probabilityRejectCount.load(std::memory_order_relaxed);
    const uint64_t prob_clip_norm = m_probabilityClipNormalizeCount.load(std::memory_order_relaxed);
    const uint64_t anomaly_total = stale_seq + ts_reject + prob_reject + prob_clip_norm;
    const double anomaly_pct = (total > 0)
        ? (100.0 * static_cast<double>(anomaly_total) / static_cast<double>(total))
        : 0.0;

    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2)
        << "HMMClient::ValidationSummary"
        << " | responses=" << total
        << " | anomaly_rate_pct=" << anomaly_pct
        << " | stale_seq_rejects=" << stale_seq
        << " | timestamp_rejects=" << ts_reject
        << " | prob_rejects=" << prob_reject
        << " | prob_clip_normalize=" << prob_clip_norm;
    Logger::getInstance().log(oss.str());
}
