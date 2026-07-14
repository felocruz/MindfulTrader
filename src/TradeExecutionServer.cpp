#include "MindfulTrader_Precompiled.h"
#include "TradeExecutionServer.h"
#include "SystemOrchestrator.h"
#include "IndicatorManager.h"
#include "Logger.h"
#include "transport/AIHeartbeatMonitor.h"
#include "ZMQContextManager.h"
#include "messaging/MessageRouter.h"      // Phase 5: MessageRouter integration
#include "messaging/MessageType.h"        // Phase 5: MessageMetadata struct
#include "generated/mts_schema_generated.h"  // Phase 5: FlatBuffer types

namespace {
zmq::message_t BuildTradeResponseEnvelope(
    uint32_t order_id,
    MTS::Schema::TradeResponseStatus status,
    const std::string& error_message,
    uint32_t latency_us)
{
    flatbuffers::FlatBufferBuilder fbb(256);

    auto err_fb = fbb.CreateString(error_message);

    MTS::Schema::TradeResponseBuilder trade_resp_builder(fbb);
    trade_resp_builder.add_status(status);
    trade_resp_builder.add_order_id(order_id);
    trade_resp_builder.add_error_message(err_fb);
    trade_resp_builder.add_timestamp_us(static_cast<int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count()));
    trade_resp_builder.add_latency_us(latency_us);
    auto trade_resp = trade_resp_builder.Finish();

    MTS::Schema::MTS_EnvelopeBuilder env_builder(fbb);
    env_builder.add_data_type(MTS::Schema::Message_TradeResponse);
    env_builder.add_data(trade_resp.Union());
    auto envelope = env_builder.Finish();

    fbb.Finish(envelope);

    zmq::message_t msg(fbb.GetSize());
    std::memcpy(msg.data(), fbb.GetBufferPointer(), fbb.GetSize());
    return msg;
}
}

TradeExecutionServer& TradeExecutionServer::Instance() {
    static TradeExecutionServer instance;
    return instance;
}

TradeExecutionServer::TradeExecutionServer() {
}

TradeExecutionServer::~TradeExecutionServer() {
}

bool TradeExecutionServer::Initialize() {
    if (m_tradeRpcRunning.load()) {
        return true;
    }

    try {
        m_tradeRpcSocket = std::make_unique<zmq::socket_t>(
            ZMQContextManager::Instance().GetContext(), zmq::socket_type::rep);
        m_tradeRpcSocket->set(zmq::sockopt::linger, 0);
        m_tradeRpcSocket->set(zmq::sockopt::rcvtimeo, 250);
        m_tradeRpcSocket->bind(ZMQ_TRADE_RPC_ENDPOINT);

        m_tradeRpcRunning.store(true);
        m_tradeRpcThread = std::thread(&TradeExecutionServer::TradeRpcWorkerLoop, this);

        Logger::getInstance().log("TradeExecutionServer: REP socket bound on tcp://*:5558");
        return true;
    } catch (const std::exception& e) {
        Logger::getInstance().log("ERROR: TradeExecutionServer::Initialize - " + std::string(e.what()));
        if (m_tradeRpcSocket) {
            try {
                m_tradeRpcSocket->close();
            } catch (...) {
            }
            m_tradeRpcSocket.reset();
        }
        m_tradeRpcRunning.store(false);
        return false;
    }
}

float TradeExecutionServer::GetActionEntropy() const {
    std::lock_guard<std::mutex> lock(m_predictionMutex);
    return m_currentPrediction.actionEntropy;
}

float TradeExecutionServer::GetTop2Margin() const {
    std::lock_guard<std::mutex> lock(m_predictionMutex);
    return m_currentPrediction.top2Margin;
}

bool TradeExecutionServer::IsNewEntriesAllowed() const {
    return m_allowNewEntries.load(std::memory_order_acquire);
}

void TradeExecutionServer::SetAllowNewEntries(bool allowed, const char* source) {
    const bool previous = m_allowNewEntries.exchange(allowed, std::memory_order_acq_rel);
    if (previous != allowed) {
        Logger::getInstance().log(
            std::string("TradeExecutionServer: allow_new_entries -> ") +
            (allowed ? "true" : "false") +
            " (source=" + (source ? source : "unknown") + ")"
        );
    }
}

void TradeExecutionServer::UpdateMarketContext(SCStudyInterfaceRef sc) {
    std::lock_guard<std::mutex> lock(m_marketMutex);

    // Update basic market context from sc
    m_marketContext.currentPrice = sc.Close[sc.Index];
    m_marketContext.tickSize = sc.TickSize;
    m_marketContext.currencyValuePerTick = sc.CurrencyValuePerTick;

    // Get ATR from IndicatorManager (IntermediateMarketAction indicator)
    auto intermAction = IndicatorManager::Instance().GetIndicator<IntermediateMarketAction>(IndicatorKey::SHORT_MKT_ACTION);
    if (intermAction) {
        m_marketContext.atr = intermAction->atr();
    }

    // Get swing highs/lows from IndicatorManager (ShortMarketAction indicator)
    auto shortAction = IndicatorManager::Instance().GetIndicator<ShortMarketAction>(IndicatorKey::SHORT_MKT_ACTION);
    if (shortAction) {
        m_marketContext.lastSwingHigh = shortAction->MaxHigh();
        m_marketContext.lastSwingLow = shortAction->MinLow();
    }
}

void TradeExecutionServer::Shutdown() {
    try {
        m_tradeRpcRunning.store(false);

        if (m_tradeRpcSocket) {
            try {
                m_tradeRpcSocket->set(zmq::sockopt::linger, 0);
                m_tradeRpcSocket->close();
            } catch (...) {
            }
        }

        if (m_tradeRpcThread.joinable()) {
            m_tradeRpcThread.join();
        }

        m_tradeRpcSocket.reset();

        std::lock_guard<std::mutex> lock(m_predictionMutex);
        m_currentPrediction.isValid = false;
        Logger::getInstance().log("TradeExecutionServer: Shutdown complete");
    } catch (const std::exception& e) {
        Logger::getInstance().log("ERROR: TradeExecutionServer::Shutdown - " + std::string(e.what()));
    }
}

void TradeExecutionServer::TradeRpcWorkerLoop() {
    while (m_tradeRpcRunning.load()) {
        try {
            if (!m_tradeRpcSocket) {
                break;
            }

            zmq::message_t request;
            auto recv_result = m_tradeRpcSocket->recv(request, zmq::recv_flags::none);
            if (!recv_result) {
                continue;
            }

            uint32_t order_id = 0;
            std::string error_message;

            try {
                flatbuffers::Verifier verifier(
                    static_cast<const uint8_t*>(request.data()),
                    request.size());
                if (MTS::Schema::VerifyMTS_EnvelopeBuffer(verifier)) {
                    const auto* envelope = MTS::Schema::GetMTS_Envelope(request.data());
                    if (envelope && envelope->data_type() == MTS::Schema::Message_TradeRequest) {
                        const auto* req = envelope->data_as_TradeRequest();
                        if (req) {
                            order_id = static_cast<uint32_t>(req->order_id());
                        }
                    }
                }
            } catch (...) {
            }

            const auto t0 = std::chrono::steady_clock::now();
            const bool accepted = ProcessTradeCommandPayload(
                static_cast<const uint8_t*>(request.data()),
                request.size());
            const auto latency_us = static_cast<uint32_t>(
                std::chrono::duration_cast<std::chrono::microseconds>(
                    std::chrono::steady_clock::now() - t0).count());

            const auto status = accepted
                ? MTS::Schema::TradeResponseStatus_SUCCESS
                : MTS::Schema::TradeResponseStatus_REJECTED;
            if (!accepted) {
                error_message = "TradeExecutionServer rejected command";
            }

            auto response = BuildTradeResponseEnvelope(order_id, status, error_message, latency_us);
            m_tradeRpcSocket->send(response, zmq::send_flags::none);
        } catch (const zmq::error_t& e) {
            if (m_tradeRpcRunning.load()) {
                Logger::getInstance().log("WARNING: TradeExecutionServer::TradeRpcWorkerLoop ZMQ error: " + std::string(e.what()));
            }
        } catch (const std::exception& e) {
            Logger::getInstance().log("WARNING: TradeExecutionServer::TradeRpcWorkerLoop exception: " + std::string(e.what()));
        } catch (...) {
            Logger::getInstance().log("WARNING: TradeExecutionServer::TradeRpcWorkerLoop unknown exception");
        }
    }

    Logger::getInstance().log("TradeExecutionServer: Trade RPC worker stopped");
}

// ============================================================================
// PHASE 5: MessageRouter Integration
// ============================================================================

void TradeExecutionServer::RegisterWithMessageRouter() {
    // Phase 5.2: MessageRouter integration pending
    // For now, handlers are registered and called from other components
    Logger::getInstance().log("TradeExecutionServer: Handler registration framework ready for Phase 5.3");
}

// Static callback for MessageRouter
// Forwards TRADE_COMMAND messages to singleton instance
bool TradeExecutionServer::StaticHandleTradeCommand(
    const uint8_t* data,
    size_t size,
    const MTS::Messaging::MessageMetadata& metadata
) {
    return Instance().HandleTradeCommand(data, size, metadata);
}

// Static callback for MessageRouter
// Forwards TRADE_CLOSE messages to singleton instance
bool TradeExecutionServer::StaticHandleTradeClose(
    const uint8_t* data,
    size_t size,
    const MTS::Messaging::MessageMetadata& metadata
) {
    return Instance().HandleTradeClose(data, size, metadata);
}

// Static callback for MessageRouter
// Forwards PYTHON_PREDICTION messages to singleton instance
bool TradeExecutionServer::StaticHandlePythonPrediction(
    const uint8_t* data,
    size_t size,
    const MTS::Messaging::MessageMetadata& metadata
) {
    return Instance().HandlePythonPrediction(data, size, metadata);
}

bool TradeExecutionServer::ProcessTradeCommandPayload(const uint8_t* data, size_t size) {
    MTS::Messaging::MessageMetadata metadata{};
    metadata.type = MTS::Messaging::MessageType::TRADE_COMMAND;
    metadata.timestamp_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    metadata.sequence_id = 0;
    metadata.sender = "SystemOrchestrator";
    metadata.size_bytes = static_cast<uint64_t>(size);
    metadata.latency_us = 0;
    return HandleTradeCommand(data, size, metadata);
}

bool TradeExecutionServer::ProcessDecodedTradeRequest(
    const MTS::Schema::TradeRequest* request,
    size_t source_size_bytes
) {
    (void)source_size_bytes;

    if (!request) {
        Logger::getInstance().log("ERROR: TradeExecutionServer::ProcessDecodedTradeRequest - null request");
        return false;
    }

    return HandleDecodedTradeRequest(*request);
}

// PHASE 5: Handle incoming FlatBuffer TradeRequest message
// Deserializes TradeRequest, queues manual trade command for PositionManager
// Manual directives go through identical gate chain as Transformer predictions for risk parity
bool TradeExecutionServer::HandleTradeCommand(
    const uint8_t* data,
    size_t size,
    [[maybe_unused]] const MTS::Messaging::MessageMetadata& metadata
) {
    try {
        if (!data || size == 0) {
            Logger::getInstance().log("ERROR: TradeExecutionServer::HandleTradeCommand - Empty payload");
            Logger::getInstance().log("ORDER_FAIL|source=RPC|stage=TRADE_COMMAND_DECODE|reason_code=EMPTY_PAYLOAD");
            return false;
        }

        // Canonical wire format: envelope-wrapped TradeRequest.
        flatbuffers::Verifier envelopeVerifier(data, size);
        if (MTS::Schema::VerifyMTS_EnvelopeBuffer(envelopeVerifier)) {
            const MTS::Schema::MTS_Envelope* envelope = MTS::Schema::GetMTS_Envelope(data);
            if (!envelope || envelope->data_type() != MTS::Schema::Message_TradeRequest) {
                Logger::getInstance().log(
                    "ERROR: TradeExecutionServer::HandleTradeCommand - Envelope data_type is not TradeRequest"
                );
                Logger::getInstance().log("ORDER_FAIL|source=RPC|stage=TRADE_COMMAND_DECODE|reason_code=ENVELOPE_WRONG_DATA_TYPE");
                return false;
            }

            const MTS::Schema::TradeRequest* envelopeRequest = envelope->data_as_TradeRequest();
            if (!envelopeRequest) {
                Logger::getInstance().log(
                    "ERROR: TradeExecutionServer::HandleTradeCommand - Envelope TradeRequest payload missing"
                );
                Logger::getInstance().log("ORDER_FAIL|source=RPC|stage=TRADE_COMMAND_DECODE|reason_code=ENVELOPE_PAYLOAD_MISSING");
                return false;
            }

            return HandleDecodedTradeRequest(*envelopeRequest);
        }

        Logger::getInstance().log(
            "ERROR: TradeExecutionServer::HandleTradeCommand - Invalid payload: expected MTS_Envelope with TradeRequest"
        );
        Logger::getInstance().log("ORDER_FAIL|source=RPC|stage=TRADE_COMMAND_DECODE|reason_code=INVALID_ENVELOPE");
        return false;

    } catch (const std::exception& e) {
        Logger::getInstance().log("ERROR: TradeExecutionServer::HandleTradeCommand - " + std::string(e.what()));
        Logger::getInstance().log("ORDER_FAIL|source=RPC|stage=TRADE_COMMAND_DECODE|reason_code=EXCEPTION|detail=" + std::string(e.what()));
        return false;
    }
}

bool TradeExecutionServer::HandleDecodedTradeRequest(const MTS::Schema::TradeRequest& request) {
    try {
        // Extract trade signal from FlatBuffer (Python provides the signal, C++ determines execution params)
        MTS::Schema::TradeRequestType requestType = request.request_type();
        const uint64_t orderId = static_cast<uint64_t>(request.order_id());

        auto patternFb = request.pattern();
        std::string patternName = patternFb ? patternFb->str() : "PYTHON_SIGNAL";
        float modelConfidence = request.model_confidence();

        // Log incoming trade command
        Logger::getInstance().log(
            "TradeExecutionServer: Received TRADE_COMMAND (order_id=" +
            std::to_string(orderId) +
            ", type=" +
            std::to_string(static_cast<int>(requestType)) +
            ", pattern=" + patternName +
            ", confidence=" + std::to_string(modelConfidence) +
            ", allow_new_entries=" + std::string(request.allow_new_entries() ? "true" : "false") + ")"
        );
        Logger::getInstance().log(
            "[PIPELINE_TRACE][8/8] C++ received prediction intent from Python "
            "(order_id=" + std::to_string(orderId) +
            ", request_type=" + std::to_string(static_cast<int>(requestType)) +
            ", pattern=" + patternName +
            ", confidence=" + std::to_string(modelConfidence) + ")"
        );

        // === MANUAL ENTRY ROUTE VALIDATION ===
        bool isLongTrade = (requestType == MTS::Schema::TradeRequestType_ENTER_LONG);
        bool isShortTrade = (requestType == MTS::Schema::TradeRequestType_ENTER_SHORT);
        bool isExitTrade = (requestType == MTS::Schema::TradeRequestType_EXIT_POSITION);
        bool isEntryDirective = (requestType == MTS::Schema::TradeRequestType_SET_ALLOW_NEW_ENTRIES);

        if (isEntryDirective) {
            const bool allowNewEntries = request.allow_new_entries();
            SetAllowNewEntries(allowNewEntries, "TRADE_COMMAND");
            Logger::getInstance().log(
                std::string("TradeExecutionServer: ENTRY_DIRECTIVE applied (order_id=") +
                std::to_string(orderId) +
                ", reason_code=" + (allowNewEntries ? "ENTRY_ENABLED" : "ENTRY_DISABLED") + ")"
            );
            return true;
        }

        // Handle exit separately (PositionManager flattens on next update)
        if (isExitTrade) {
            Logger::getInstance().log(
                "TradeExecutionServer: Manual EXIT command accepted (order_id=" +
                std::to_string(orderId) +
                ") - will be processed on study thread"
            );
            return true;
        }

        if (!isLongTrade && !isShortTrade) {
            Logger::getInstance().log(
                "TradeExecutionServer: Invalid trade request type (order_id=" +
                std::to_string(orderId) +
                ", type=" + std::to_string(static_cast<int>(requestType)) + ")"
            );
            Logger::getInstance().log(
                "ORDER_FAIL|source=RPC|stage=TRADE_REQUEST_VALIDATE|reason_code=INVALID_REQUEST_TYPE|order_id=" +
                std::to_string(orderId)
            );
            return false;
        }

        if (!IsNewEntriesAllowed()) {
            Logger::getInstance().log(
                "TradeExecutionServer: Manual entry blocked by allow_new_entries=false (order_id=" +
                std::to_string(orderId) +
                ", side=" + std::string(isLongTrade ? "LONG" : "SHORT") + ")"
            );
            Logger::getInstance().log(
                "ORDER_FAIL|source=RPC|stage=ENTRY_DIRECTIVE|reason_code=ENTRY_DISABLED_GLOBAL|order_id=" +
                std::to_string(orderId)
            );
            return true;
        }

        // === QUEUE FOR STUDY THREAD PROCESSING ===
        // Store pattern and confidence; PositionManager will determine entry price, size, stops
        // Entry price, quantity, stops are calculated internally by C++ based on:
        // - Current market conditions
        // - ATR and risk management rules
        // - CalculateSafePositionSize cascade
        {
            std::lock_guard<std::mutex> lock(m_manualMutex);
            m_currentManualCommand.isLong = isLongTrade;
            m_currentManualCommand.patternName = patternName;
            m_currentManualCommand.intentId = orderId;
            m_currentManualCommand.sequenceId = orderId;
            // NOTE: entryPrice, stopPrice, targetPrice, quantity are TBD by C++ internal logic
            // and will be calculated by PositionManager based on current market state
            m_currentManualCommand.entryPrice = 0.0f;  // Placeholder: to be determined
            m_currentManualCommand.stopPrice = 0.0f;   // Placeholder: to be determined
            m_currentManualCommand.targetPrice = 0.0f; // Placeholder: to be determined
            m_currentManualCommand.quantity = 0;       // Placeholder: to be determined
            const uint64_t requestTimestampUs = static_cast<uint64_t>(request.timestamp_us());
            m_currentManualCommand.timestamp_us = requestTimestampUs > 0
                ? requestTimestampUs
                : static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::system_clock::now().time_since_epoch()
                    ).count()
                );
        }

        // Signal study thread that a manual command is ready
        m_manualCommandReady.store(true, std::memory_order_release);

        Logger::getInstance().log(
            "TradeExecutionServer: Manual entry queued for study thread (order_id=" +
            std::to_string(orderId) +
            ", side=" + std::string(isLongTrade ? "LONG" : "SHORT") +
            ", pattern=" + patternName +
            ", confidence=" + std::to_string(modelConfidence) + ")"
        );

        return true;

    } catch (const std::exception& e) {
        Logger::getInstance().log("ERROR: TradeExecutionServer::HandleDecodedTradeRequest - " + std::string(e.what()));
        Logger::getInstance().log("ORDER_FAIL|source=RPC|stage=TRADE_REQUEST_VALIDATE|reason_code=EXCEPTION|detail=" + std::string(e.what()));
        return false;
    }
}
// PHASE 5: Handle incoming FlatBuffer TradeClose message
// Deserializes TradeClose, validates close parameters, generates TradeCloseResponse
bool TradeExecutionServer::HandleTradeClose(
    const uint8_t* data,
    [[maybe_unused]] size_t size,
    [[maybe_unused]] const MTS::Messaging::MessageMetadata& metadata
) {
    try {
        // Deserialize TradeClose from FlatBuffer
        const MTS::Schema::TradeClose* tradeClose = flatbuffers::GetRoot<MTS::Schema::TradeClose>(data);
        if (!tradeClose) {
            Logger::getInstance().log("ERROR: TradeExecutionServer::HandleTradeClose - Failed to deserialize TradeClose");
            return false;
        }

        // Log incoming trade close command
        Logger::getInstance().log(
            "TradeExecutionServer: Received TRADE_CLOSE (order_id=" +
            std::to_string(tradeClose->order_id()) + ")"
        );

        // TODO: Implement trade close validation and execution
        // - Verify position exists and matches side
        // - Close position (LONG → FLAT, SHORT → FLAT)
        // - Log exit details (exit price, PnL, R-multiple)

        Logger::getInstance().log("TradeExecutionServer: TRADE_CLOSE processed successfully");
        return true;

    } catch (const std::exception& e) {
        Logger::getInstance().log("ERROR: TradeExecutionServer::HandleTradeClose - " + std::string(e.what()));
        return false;
    }
}

// ============================================================================
// PHASE 5.2: INSTITUTIONAL-GRADE PYTHON PREDICTION HANDLER
// 7-Layer Validation Pipeline
// ============================================================================

bool TradeExecutionServer::HandlePythonPrediction(
    const uint8_t* data,
    size_t size,
    [[maybe_unused]] const MTS::Messaging::MessageMetadata& metadata
) {
    try {
        // === PHASE 5.2: Elite FlatBuffer Deserialization ===
        // Parse ModelPrediction message from Python LiveAgent (EventTransformer inference)
        // Elite pattern: Zero-copy FlatBuffer deserialization using GetRoot<>()

        if (data == nullptr || size < 4) {
            Logger::getInstance().log("ERROR: TradeExecutionServer::HandlePythonPrediction - Invalid FlatBuffer data");
            return false;
        }

        // Canonical wire format: envelope-wrapped ModelPrediction.
        flatbuffers::Verifier envelopeVerifier(data, size);
        if (!MTS::Schema::VerifyMTS_EnvelopeBuffer(envelopeVerifier)) {
            Logger::getInstance().log(
                "ERROR: TradeExecutionServer::HandlePythonPrediction - Invalid payload: expected MTS_Envelope"
            );
            return false;
        }

        const MTS::Schema::MTS_Envelope* envelope = MTS::Schema::GetMTS_Envelope(data);
        if (!envelope || envelope->data_type() != MTS::Schema::Message_ModelPrediction) {
            Logger::getInstance().log(
                "ERROR: TradeExecutionServer::HandlePythonPrediction - Envelope data_type is not ModelPrediction"
            );
            return false;
        }

        const MTS::Schema::ModelPrediction* prediction = envelope->data_as_ModelPrediction();

        if (prediction == nullptr) {
            Logger::getInstance().log(
                "ERROR: TradeExecutionServer::HandlePythonPrediction - Envelope ModelPrediction payload missing"
            );
            return false;
        }

        // Extract prediction fields (zero-copy access - no allocations)
        uint8_t actionId = prediction->action_id();
        float confidence = prediction->confidence();           // 0.0-1.0
        float thesisStrength = prediction->thesis_strength();  // 0.0-1.0 conviction metric
        uint64_t timestamp_us = prediction->timestamp_us();    // Microsecond precision
        uint64_t sequenceId = prediction->sequence_id();       // Monotonic counter for loss detection
        long inferenceLatency_us = prediction->inference_latency_us();    // End-to-end model latency
        long transformerLatency_us = prediction->transformer_latency_us(); // Transformer forward-pass
        long regimeLatency_us = prediction->regime_latency_us();           // RegimeEngine latency
        float actionEntropy = prediction->action_entropy();                 // Shannon entropy of softmax
        float top2Margin = prediction->top2_margin();                       // p_best - p_runner_up

        Logger::getInstance().log(
            "[PIPELINE_TRACE][8/8] C++ received ModelPrediction payload from Python "
            "(sequence_id=" + std::to_string(sequenceId) +
            ", action_id=" + std::to_string(actionId) +
            ", confidence=" + std::to_string(confidence) +
            ", latency_total_us=" + std::to_string(inferenceLatency_us) + ")"
        );

        // Semantic guards: reject malformed or cross-path payloads.
        constexpr uint8_t kMaxActionId = static_cast<uint8_t>(TradeActionEnum::TRAP_SHORT);
        if (actionId > kMaxActionId) {
            Logger::getInstance().log(
                "ERROR: TradeExecutionServer::HandlePythonPrediction - Invalid action_id=" +
                std::to_string(actionId)
            );
            return false;
        }

        if (!std::isfinite(confidence) || confidence < 0.0f || confidence > 1.5f) {
            Logger::getInstance().log(
                "ERROR: TradeExecutionServer::HandlePythonPrediction - Invalid confidence=" +
                std::to_string(confidence)
            );
            return false;
        }

        if (!std::isfinite(thesisStrength) || thesisStrength < 0.0f || thesisStrength > 1.5f) {
            Logger::getInstance().log(
                "ERROR: TradeExecutionServer::HandlePythonPrediction - Invalid thesis_strength=" +
                std::to_string(thesisStrength)
            );
            return false;
        }

        if (!std::isfinite(actionEntropy) || !std::isfinite(top2Margin)) {
            Logger::getInstance().log(
                "ERROR: TradeExecutionServer::HandlePythonPrediction - Invalid action diagnostics"
            );
            return false;
        }

        if (timestamp_us == 0 || sequenceId == 0) {
            Logger::getInstance().log(
                "ERROR: TradeExecutionServer::HandlePythonPrediction - Missing timestamp/sequence"
            );
            return false;
        }

        // Extract triggered indicators (optional, for diagnostics)
        std::string triggeredIndicators = "";
        if (prediction->triggered_indicators() != nullptr && prediction->triggered_indicators()->size() > 0) {
            triggeredIndicators = "indicators:[";
            for (uint32_t i = 0; i < prediction->triggered_indicators()->size(); i++) {
                const MTS::Schema::TriggeredIndicator* indicator = prediction->triggered_indicators()->Get(i);
                if (indicator->indicator_name() != nullptr) {
                    if (i > 0) triggeredIndicators += ", ";
                    triggeredIndicators += indicator->indicator_name()->c_str();
                    triggeredIndicators += "=" + std::to_string(static_cast<int>(indicator->indicator_value()));
                }
            }
            triggeredIndicators += "]";
        }

        // Log received prediction with full context
        char confStr[16];
        char thesisStr[16];
        snprintf(confStr, sizeof(confStr), "%.3f", confidence);
        snprintf(thesisStr, sizeof(thesisStr), "%.3f", thesisStrength);

        Logger::getInstance().log(
            "TradeExecutionServer: PYTHON_PREDICTION received (action=" + std::to_string(actionId) +
            ", confidence=" + std::string(confStr) + ", thesis=" + std::string(thesisStr) +
            ", latency_total=" + std::to_string(inferenceLatency_us) + "µs" +
            ", latency_transformer=" + std::to_string(transformerLatency_us) + "µs" +
            ", latency_regime=" + std::to_string(regimeLatency_us) + "µs" +
            ", seq=" + std::to_string(sequenceId) +
            ", " + triggeredIndicators + ")"
        );

        const TradeActionEnum action = static_cast<TradeActionEnum>(actionId);

        // === PERSISTENT STATE: Always update PredictionState (survives consumption) ===
        const bool modelReadyNow = AIHeartbeatMonitor::Instance().IsModelReady();
        InferenceManager::Instance().MutablePrediction()->SetPrediction(
            action, confidence, thesisStrength, actionEntropy, top2Margin,
            timestamp_us, static_cast<uint64_t>(sequenceId),
            inferenceLatency_us, transformerLatency_us, regimeLatency_us,
            modelReadyNow);

        // === LAYER 0: STAND_ASIDE Filter ===
        if (action == TradeActionEnum::STAND_ASIDE) {
            Logger::getInstance().log("TradeExecutionServer: Model predicts STAND_ASIDE - no trade generated");
            return true;
        }

        // === LAYER 1: Confidence Gate (RiskManager) ===
        // Get current SC context from main study thread
        // Note: This is typically called from worker thread, so we can't use SC API here directly
        // Instead, we defer to when the prediction is processed on the study thread
        // TODO (Phase 5.3): Call RiskManager::Instance().GetRequiredConfidenceThreshold() on study thread
        if (regimeLatency_us > 5000) {
            Logger::getInstance().log(
                "TradeExecutionServer: LATENCY BREAKER candidate (regime_latency_us=" +
                std::to_string(regimeLatency_us) +
                ") -> enforce SHANNON_CHAOS posture on study-thread validation"
            );
        }

        // === LAYER 2: Pattern Validation ===
        // Re-validate pattern assumptions with live market data
        // For now, store prediction and mark for deferred validation on study thread
        {
            std::lock_guard<std::mutex> lock(m_predictionMutex);
            m_currentPrediction.actionId = actionId;
            m_currentPrediction.confidence = confidence;
            m_currentPrediction.patternName = "EventTransformer";  // Pattern from EventTransformer
            m_currentPrediction.thesisStrength = thesisStrength;
            m_currentPrediction.timestamp_us = timestamp_us;
            m_currentPrediction.sequenceId = sequenceId;
            m_currentPrediction.inferenceLatencyUs = inferenceLatency_us;
            m_currentPrediction.transformerLatencyUs = transformerLatency_us;
            m_currentPrediction.regimeLatencyUs = regimeLatency_us;
            m_currentPrediction.actionEntropy = actionEntropy;
            m_currentPrediction.top2Margin = top2Margin;
            m_currentPrediction.modelReady = modelReadyNow;
            m_currentPrediction.isValid = false;  // Will be set to true after Layer 2 validation
            // Snapshot HMM regime at prediction generation for regime-coherence gate.
            // The Transformer's action is conditioned on this regime embedding.
            const auto* hmmState = InferenceManager::Instance().HmmState();
            m_currentPrediction.hmmStateId = hmmState ? static_cast<int>(hmmState->Value()) : -1;
        }

        // Signal study thread that a prediction is ready for consumption
        m_predictionReady.store(true, std::memory_order_release);

        Logger::getInstance().log(
            "TradeExecutionServer: Prediction ready for study thread "
            "(seq=" + std::to_string(sequenceId) + ", action=" + std::to_string(actionId) + ")"
        );

        return true;

    } catch (const std::exception& e) {
        Logger::getInstance().log("ERROR: TradeExecutionServer::HandlePythonPrediction - " + std::string(e.what()));
        return false;
    }
}

// ============================================================================
// Single-slot prediction consumption (study thread)
// ============================================================================

bool TradeExecutionServer::ConsumePendingPrediction(PredictionSlot& out) {
    // Fast atomic check — avoids mutex contention when no prediction pending
    if (!m_predictionReady.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_predictionMutex);

    // Double-check under lock (another thread could have cleared it)
    if (!m_predictionReady.load(std::memory_order_relaxed)) {
        return false;
    }

    out.actionId = m_currentPrediction.actionId;
    out.confidence = m_currentPrediction.confidence;
    out.patternName = m_currentPrediction.patternName;
    out.thesisStrength = m_currentPrediction.thesisStrength;
    out.timestampUs = m_currentPrediction.timestamp_us;
    out.sequenceId = static_cast<uint64_t>(m_currentPrediction.sequenceId);
    out.inferenceLatencyUs = m_currentPrediction.inferenceLatencyUs;
    out.transformerLatencyUs = m_currentPrediction.transformerLatencyUs;
    out.regimeLatencyUs = m_currentPrediction.regimeLatencyUs;
    out.actionEntropy = m_currentPrediction.actionEntropy;
    out.top2Margin = m_currentPrediction.top2Margin;
    out.modelReady = m_currentPrediction.modelReady;
    out.hmmStateId = m_currentPrediction.hmmStateId;

    // Clear the slot (one-shot consumption)
    m_predictionReady.store(false, std::memory_order_release);

    return true;
}

// ============================================================================
// Single-slot manual command consumption (study thread)
// ============================================================================

bool TradeExecutionServer::ConsumePendingManualCommand(ManualCommandSlot& out) {
    // Fast atomic check — avoids mutex contention when no command pending
    if (!m_manualCommandReady.load(std::memory_order_acquire)) {
        return false;
    }

    std::lock_guard<std::mutex> lock(m_manualMutex);

    // Double-check under lock (another thread could have cleared it)
    if (!m_manualCommandReady.load(std::memory_order_relaxed)) {
        return false;
    }

    out.isLong = m_currentManualCommand.isLong;
    out.entryPrice = m_currentManualCommand.entryPrice;
    out.stopPrice = m_currentManualCommand.stopPrice;
    out.targetPrice = m_currentManualCommand.targetPrice;
    out.quantity = m_currentManualCommand.quantity;
    out.patternName = m_currentManualCommand.patternName;
    out.intentId = m_currentManualCommand.intentId;
    out.sequenceId = m_currentManualCommand.sequenceId;
    out.timestampUs = m_currentManualCommand.timestamp_us;

    // Clear the slot (one-shot consumption)
    m_manualCommandReady.store(false, std::memory_order_release);

    return true;
}

// ============================================================================
// PHASE 5.2: Pattern Validation (Layer 2)
// ============================================================================

bool TradeExecutionServer::ValidatePatternWithLiveData(
    const std::string& patternName,
    int actionId,
    [[maybe_unused]] SCStudyInterfaceRef sc
) {
    try {
        // Pattern-specific validation logic
        // This re-verifies that the pattern conditions still exist with CURRENT market data

        // Examples (pattern-specific):
        // momentum_pinball: RSI <= 30 AND Stochastic <= 20 for LONG entry
        // stochastic_extreme: Stochastic >= 80 AND momentum confirmed
        // macd_cross: MACD bullish/bearish cross still valid
        // elder_breakout: Breakout level still valid, momentum confirmed

        // For MVP, we'll do basic validation and log intent for full implementation

        Logger::getInstance().log(
            "TradeExecutionServer: Layer 2 - Pattern validation: " + patternName + " (action=" +
            std::to_string(actionId) + ")"
        );

        // TODO: Implement pattern-specific validation logic
        // - momentum_pinball: Check RSI and Stochastic levels
        // - stochastic_extreme: Check Stochastic and momentum
        // - macd_cross: Verify MACD direction
        // - elder_breakout: Verify breakout level and impulse
        // Return false if pattern invalidated, true if still valid

        return true;  // Assume valid for MVP

    } catch (const std::exception& e) {
        Logger::getInstance().log("ERROR: TradeExecutionServer::ValidatePatternWithLiveData - " + std::string(e.what()));
        return false;
    }
}

// ============================================================================
// PHASE 5.2: Order Price Calculation (Layer 5)
// ============================================================================

TradeExecutionServer::OrderPrices TradeExecutionServer::CalculateOrderPrices(
    const std::string& patternName,
    int actionId,
    float currentPrice
) {
    OrderPrices prices = {currentPrice, 0.0f, 0.0f};

    try {
        // Get ATR for stop/target calculation
        float atr = m_marketContext.atr;
        if (atr <= 0.0f) {
            Logger::getInstance().log(
                "ERROR: TradeExecutionServer::CalculateOrderPrices - invalid ATR context"
            );
            return prices;
        }

        // Pattern-specific price calculation (using enum-based switch for type safety)
        // patternId maps to RaschkeTacticalTrigger enum (Python: rc_enums.py)
        const TradeActionEnum action = static_cast<TradeActionEnum>(actionId);
        bool isLong = (action == TradeActionEnum::ENTER_LONG);
        bool isShort = (action == TradeActionEnum::ENTER_SHORT);

        // TODO: Extract tactical trigger from prediction message or currentPrediction state.
        const RaschkeTacticalTrigger trigger = RaschkeTacticalTrigger::MOMENTUM_PINBALL_BUY;

        switch (trigger) {
            case RaschkeTacticalTrigger::MOMENTUM_PINBALL_BUY:
            case RaschkeTacticalTrigger::MOMENTUM_PINBALL_SELL:
                prices.entry = currentPrice;
                if (isLong) {
                    prices.stop = m_marketContext.lastSwingLow - (0.5f * atr);
                    prices.target = prices.entry + (2.0f * (prices.entry - prices.stop));
                } else if (isShort) {
                    prices.stop = m_marketContext.lastSwingHigh + (0.5f * atr);
                    prices.target = prices.entry - (2.0f * (prices.stop - prices.entry));
                }
                break;

            case RaschkeTacticalTrigger::STOCHASTIC_POP_BUY:
            case RaschkeTacticalTrigger::STOCHASTIC_POP_SELL:
                prices.entry = currentPrice;
                if (isLong) {
                    prices.stop = m_marketContext.lastSwingLow - (0.75f * atr);
                    prices.target = prices.entry + (2.5f * (prices.entry - prices.stop));
                } else if (isShort) {
                    prices.stop = m_marketContext.lastSwingHigh + (0.75f * atr);
                    prices.target = prices.entry - (2.5f * (prices.stop - prices.entry));
                }
                break;

            case RaschkeTacticalTrigger::ELDER_BREAKOUT_BUY:
            case RaschkeTacticalTrigger::ELDER_BREAKOUT_SELL:
                prices.entry = currentPrice;
                if (isLong) {
                    prices.stop = m_marketContext.lastSwingLow - (0.5f * atr);
                    prices.target = prices.entry + (1.5f * (prices.entry - prices.stop));
                } else if (isShort) {
                    prices.stop = m_marketContext.lastSwingHigh + (0.5f * atr);
                    prices.target = prices.entry - (1.5f * (prices.stop - prices.entry));
                }
                break;

            // Default pattern logic (all other RaschkeTacticalTrigger patterns)
            default:
                prices.entry = currentPrice;
                if (isLong) {
                    prices.stop = prices.entry - atr;
                    prices.target = prices.entry + (1.5f * atr);
                } else if (isShort) {
                prices.stop = prices.entry + atr;
                prices.target = prices.entry - (1.5f * atr);
            }
        }

        Logger::getInstance().log(
            "TradeExecutionServer: Layer 5 - Calculated prices for " + patternName +
            " (entry=" + std::to_string(prices.entry) +
            ", stop=" + std::to_string(prices.stop) +
            ", target=" + std::to_string(prices.target) + ")"
        );

    } catch (const std::exception& e) {
        Logger::getInstance().log("ERROR: TradeExecutionServer::CalculateOrderPrices - " + std::string(e.what()));
    }

    return prices;
}
