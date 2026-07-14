#pragma once

#include <string>
#include <mutex>
#include <memory>
#include <thread>
#include <atomic>
#include <zmq.hpp>
#include "sierrachart.h"
#include "messaging/MessageRouter.h"    // Phase 5: MessageRouter integration
#include "messaging/MessageType.h"      // Phase 5: MessageMetadata struct

namespace MTS { namespace Schema { struct TradeRequest; } }

/**
 * @brief ZMQ REP server that receives trade commands from Python GUI
 *
 * Single-slot prediction model: Python sends one ModelPrediction at a time.
 * ZMQ worker thread writes into m_currentPrediction (mutex-guarded).
 * Study thread consumes via ConsumePendingPrediction() each update cycle.
 *
 * Handles message types:
 * - FlatBuffer: TradeRequest → TradeResponse, ModelPrediction → single-slot
 *
 * Runs on port 5558 (REP socket, receives FlatBuffer TRADE_COMMAND)
 */
class TradeExecutionServer {
public:
    static TradeExecutionServer& Instance();

    TradeExecutionServer(const TradeExecutionServer&) = delete;
    TradeExecutionServer& operator=(const TradeExecutionServer&) = delete;

    // Dedicated trade RPC endpoint lifecycle (port 5558 REP)
    bool Initialize();

    // Update market context (called from study every bar)
    void UpdateMarketContext(SCStudyInterfaceRef sc);

    // Shutdown (called during study cleanup)
    void Shutdown();

    // Shannon conviction accessors (thread-safe, read under prediction mutex)
    float GetActionEntropy() const;
    float GetTop2Margin() const;

    // Global entry directive (control-plane): blocks all NEW entries when false.
    bool IsNewEntriesAllowed() const;
    void SetAllowNewEntries(bool allowed, const char* source = "unknown");

    // Single-slot prediction consumption (called from study thread)
    // Returns true if a prediction was pending and populates the output fields.
    // Clears the slot after consumption (one-shot).
    struct PredictionSlot {
        uint8_t actionId = 0;
        float confidence = 0.0f;
        std::string patternName;
        float thesisStrength = 0.0f;
        uint64_t timestampUs = 0;
        uint64_t sequenceId = 0;
        long inferenceLatencyUs = -1;
        long transformerLatencyUs = -1;
        long regimeLatencyUs = -1;
        float actionEntropy = 0.0f;
        float top2Margin = 0.0f;
        bool modelReady = false;
        int hmmStateId = -1;            // HMM regime state at prediction generation
    };
    bool ConsumePendingPrediction(PredictionSlot& out);

    // Single-slot manual command consumption (called from study thread)
    // Returns true if a manual command was pending and populates output fields
    struct ManualCommandSlot {
        bool isLong = false;
        float entryPrice = 0.0f;
        float stopPrice = 0.0f;
        float targetPrice = 0.0f;
        uint32_t quantity = 0;
        std::string patternName;
        uint64_t intentId = 0;
        uint64_t sequenceId = 0;
        uint64_t timestampUs = 0;
    };
    bool ConsumePendingManualCommand(ManualCommandSlot& out);

    // PHASE 5: MessageRouter integration
    // Register this server's handlers with the global MessageRouter
    void RegisterWithMessageRouter();

    // Runtime bridge helper: process FlatBuffer TradeRequest payload received on control-plane REP.
    // Accepts either raw TradeRequest bytes or envelope-wrapped TradeRequest bytes.
    bool ProcessTradeCommandPayload(const uint8_t* data, size_t size);

    // Runtime bridge fast-path: process an already-decoded TradeRequest from SystemOrchestrator.
    bool ProcessDecodedTradeRequest(const MTS::Schema::TradeRequest* request, size_t source_size_bytes);

private:
    TradeExecutionServer();
    ~TradeExecutionServer();

    void TradeRpcWorkerLoop();

    // === FlatBuffer Message Handlers (Modern, Phase 5+) ===
    // PHASE 5: Implement FlatBuffer handlers
    // Process incoming FlatBuffer TradeRequest and generate TradeResponse
    bool HandleTradeCommand(
        const uint8_t* data,
        size_t size,
        const MTS::Messaging::MessageMetadata& metadata
    );

    bool HandleDecodedTradeRequest(const MTS::Schema::TradeRequest& request);

    // PHASE 5: Implement trade close handler
    // Process incoming FlatBuffer TradeClose and generate TradeCloseResponse
    bool HandleTradeClose(
        const uint8_t* data,
        size_t size,
        const MTS::Messaging::MessageMetadata& metadata
    );

    // === INSTITUTIONAL GRADE: Python ML Prediction Handler ===
    // PHASE 5.2: Implements 7-layer prediction validation pipeline:
    // Layer 1: Confidence Gate (RiskManager dynamic threshold based on model health)
    // Layer 2: Pattern Validation (re-verify pattern assumptions with live data)
    // Layer 3: Regime Scaling (PositionManager HMM-based position sizing multiplier)
    // Layer 4: Risk Validation (RiskManager approve safe position size)
    // Layer 5: Price Calculation (pattern-specific entry/stop/target from current market)
    // Layer 6: Order Submission (Sierra Chart bracket order execution)
    // Layer 7: Position Tracking (PositionManager register open position)
    bool HandlePythonPrediction(
        const uint8_t* data,
        size_t size,
        const MTS::Messaging::MessageMetadata& metadata
    );

    // Helper for pattern validation (Layer 2)
    bool ValidatePatternWithLiveData(
        const std::string& patternName,
        int actionId,
        SCStudyInterfaceRef sc
    );

    // Helper for price calculation (Layer 5)
    struct OrderPrices {
        float entry;
        float stop;
        float target;
    };

    OrderPrices CalculateOrderPrices(
        const std::string& patternName,
        int actionId,
        float currentPrice
    );

    // === Helper Methods ===
    // Validation helper - checks if action is valid for current position state
    std::string ValidateActionAgainstPosition(int actionInt) const;

    // PHASE 5: Static callback methods for MessageRouter
    // MessageRouter stores function pointers, so we need static methods
    // These forward to the singleton instance
    static bool StaticHandleTradeCommand(
        const uint8_t* data,
        size_t size,
        const MTS::Messaging::MessageMetadata& metadata
    );

    static bool StaticHandleTradeClose(
        const uint8_t* data,
        size_t size,
        const MTS::Messaging::MessageMetadata& metadata
    );

    // PHASE 5.2: Static handler for Python prediction messages
    static bool StaticHandlePythonPrediction(
        const uint8_t* data,
        size_t size,
        const MTS::Messaging::MessageMetadata& metadata
    );

    // Helper to calculate market data
    struct MarketContext {
        float currentPrice;
        float atr;
        float lastSwingHigh;
        float lastSwingLow;
        float tickSize;
        float currencyValuePerTick;
    };

    // Market context (updated from study thread, read by worker thread)
    MarketContext m_marketContext;
    mutable std::mutex m_marketMutex;

    // PHASE 5.2: Current prediction state (for 7-layer validation pipeline)
    struct CurrentPrediction {
        uint8_t actionId = 0;           // TradeActionEnum ID (STAND_ASIDE..TRAP_SHORT)
        float confidence = 0.0f;        // Model confidence (0.0-1.0)
        std::string patternName;        // Pattern that triggered prediction
        float thesisStrength = 0.0f;    // Conviction decay (0.0-1.0)
        uint64_t timestamp_us = 0;      // Prediction timestamp
        int sequenceId = 0;             // Monotonic counter
        long inferenceLatencyUs = -1;   // End-to-end model latency (µs)
        long transformerLatencyUs = -1; // Transformer latency (µs)
        long regimeLatencyUs = -1;      // RegimeEngine latency (µs)
        float actionEntropy = 0.0f;     // Shannon entropy of 9-class softmax
        float top2Margin = 0.0f;        // p_best - p_runner_up
        bool modelReady = false;        // AI heartbeat readiness snapshot at receipt time
        bool isValid = false;           // Set to true after Layer 2 validation passes
        int hmmStateId = -1;            // HMM regime at prediction generation (for coherence gate)
    };
    CurrentPrediction m_currentPrediction;
    mutable std::mutex m_predictionMutex;
    std::atomic<bool> m_predictionReady{false};  // Fast lock-free check before taking mutex

    // PHASE 5.3: Manual trade command storage (queued from message router thread)
    // Follows same single-slot pattern as predictions for thread safety
    struct CurrentManualCommand {
        bool isLong = false;
        float entryPrice = 0.0f;
        float stopPrice = 0.0f;
        float targetPrice = 0.0f;
        uint32_t quantity = 0;
        std::string patternName;
        uint64_t intentId = 0;
        uint64_t sequenceId = 0;
        uint64_t timestamp_us = 0;
    };
    CurrentManualCommand m_currentManualCommand;
    mutable std::mutex m_manualMutex;
    std::atomic<bool> m_manualCommandReady{false};  // Fast lock-free check before taking mutex

    // Global entry directive state (process-wide execution policy)
    std::atomic<bool> m_allowNewEntries{true};

    // PHASE 5: MessageRouter reference
    MTS::Messaging::MessageRouter* m_messageRouter{nullptr};

    // Dedicated trade RPC server (REQ/REP) for Python trade requests
    std::unique_ptr<zmq::socket_t> m_tradeRpcSocket;
    std::thread m_tradeRpcThread;
    std::atomic<bool> m_tradeRpcRunning{false};
    static constexpr const char* ZMQ_TRADE_RPC_ENDPOINT = "tcp://*:5558";
};
