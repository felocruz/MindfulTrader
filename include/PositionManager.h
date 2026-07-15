#pragma once

#include <memory>
#include <functional>
#include <cstddef>
#include <string>
#include <cstdint>
#include <deque>
#include <mutex>
#include "flatbuffers/flatbuffers.h"
#include "sierrachart.h"
#include "Trade.h"
#include "Indicator.h"
#include "TradeExecutionServer.h"
#include "execution/ExecutionGate.h"

// Forward declarations
template<typename T> class ThreadSafeQueue;
struct TradeRequest;
struct TradeReply;

// The PositionManager holds a collection of the new self-contained Trade objects.
// Its responsibility is to act as a bridge between the Sierra Chart environment and the Trade objects.
// NOTE: Elite v2.4 removed SocketMessage queue (deprecated MindfulSocketZMQ infrastructure)
// Event publishing now uses: TransportStream::Instance().Emit() + EventSerializer
class PositionManager {
public:
    enum class ReasonCode : uint8_t {
        NA = 0,
        ContextStale = 1,
        EntryDisabledGlobal = 2,
        HardSafetyViolation = 3,
        IbDisconnected = 4,
        IntentCancelled = 5,
        IntentClosed = 6,
        IntentExpired = 7,
        IntentFilled = 8,
        IntentSuperseded = 9,
        ModelNotReady = 10,
        OrderAckTimeout = 11,
        PartialRemainderCancelled = 12,
        PatternInvalidated = 13,
        PatternQualityFail = 14,
        PolicyEvalErrorFailClosed = 15,
        PredictionStale = 16,
        RepriceBudgetExceeded = 17,
        RiskDailyLimit = 18,
        ScDisconnected = 19,
        SpreadBreach = 20,
        ToxicFlowBreach = 21,
        QuoteChurnBreach = 22,
        WorkingOrderTtlExceeded = 23,
        EntropyBreach = 24,
        PredictionDirectionBreach = 25,
        PredictionStaleDecay = 26,
        ClimateShiftExit = 27,
        HostileRegimeExit = 28,
        ClusteringBreach = 29,
        HmmRegimeGateParetoBreach = 30,
        HmmRegimeGateShannonBreach = 31,
        HmmRegimeGateTalebBreach = 32,
    };

    struct PredictionAckEvent {
        bool accepted = false;
        TradeActionEnum action = TradeActionEnum::STAND_ASIDE;
        float confidence = 0.0f;
        std::string patternName;
        uint64_t sequenceId = 0;
        uint64_t timestampUs = 0;
        long inferenceLatencyUs = -1;
        long transformerLatencyUs = -1;
        long regimeLatencyUs = -1;
        ReasonCode reasonCode = ReasonCode::NA;
        std::string reasonDetail;
        float entryPrice = 0.0f;
        float stopPrice = 0.0f;
        float targetPrice = 0.0f;
    };
    using PredictionAckSink = std::function<void(const PredictionAckEvent&)>;

    static PositionManager& Instance();

    PositionManager(const PositionManager&) = delete;
    PositionManager& operator=(const PositionManager&) = delete;

    void Init(SCStudyInterfaceRef sc,
              std::shared_ptr<ThreadSafeQueue<TradeRequest>> req,
              std::shared_ptr<ThreadSafeQueue<TradeReply>> rep);
    void Update(SCStudyInterfaceRef sc);
    void Reset(SCStudyInterfaceRef sc);
    void PublishSnapshot(SCStudyInterfaceRef sc);
    void PublishPositionSync(SCStudyInterfaceRef sc);  // Elite REFINEMENT 2: Position sync on reconnect

    // Elite v2.5: Context-Aware Position Management (Scoring.cpp integration)
    // Event-driven entry point (SystemOrchestrator on regime change): refresh +
    // immediate defense. The per-tick refresh is done by SyncRegimeState() inside
    // Update() to avoid a double EvaluateRegimeDefense.
    void UpdateContext(SCStudyInterfaceRef sc);
    void EvaluateRegimeDefense(SCStudyInterfaceRef sc); // Analyzes scoring multipliers to tighten stops or exit

    // Single-slot prediction processing (consumes from TradeExecutionServer)
    void ProcessPendingPrediction(SCStudyInterfaceRef sc);
    void SetPredictionAckSink(PredictionAckSink sink);

    // Manual trade command processing (from UI/API with user-specified entry/stop/target)
    // Applies identical gate chain to Transformer predictions for risk parity
    void ProcessManualTradeCommand(
        SCStudyInterfaceRef sc,
        bool isLong,
        float entryPrice,
        float stopPrice,
        float targetPrice,
        uint32_t quantity,
        const std::string& patternName = "MANUAL",
        uint64_t intentId = 0,
        uint64_t sequenceId = 0,
        uint64_t intentTimestampUs = 0
    );

    // === ELITE v3.2: UNIFIED HARD GATE ENFORCEMENT ===
    // Evaluates LocalRiskContext hard gates and dispatches defensive action
    // based on current position state:
    //   FLAT, no orders  → block (entries rejected by gate check)
    //   FLAT, working orders → cancel all working orders
    //   IN POSITION → emergency flatten
    // Returns true if gates passed, false if violation triggered defensive action.
    bool EnforceHardGates(SCStudyInterfaceRef sc);

    // Accessors
    TradeSideEnum GetTradeSide() const;
    const Trade& GetOpenTrade() const { return m_openTrade; }
    const Trade& GetLastClosedTrade() const { return m_lastClosedTrade; }
    const Trade& GetTrade() const;

    // Gap 1 closure: TripleScreen producers push ATR directly into PositionManager.
    void SetCachedATR14(float value) { m_cachedATR14 = value; }
    void SetCachedATR10(float value) { m_cachedATR10 = value; }

    // State checks
    bool IsFlat() const;
    bool IsLong() const;
    bool IsShort() const;
    void CancelAllWorkingOrders(SCStudyInterfaceRef sc);   // AI disconnect fast-purge
    void EmergencyFlattenPosition(SCStudyInterfaceRef sc, const char* reason);  // Elite GAP 5: Emergency flatten
    // Triple-Barrier: neutral deterministic market close (e.g. vertical/time barrier).
    // No emergency semantics (no halt/alarm/force-exit); tags the trade's exit reason
    // and lets the fill flow through HandleFills' normal close path.
    void ClosePositionAtMarket(SCStudyInterfaceRef sc, const char* exitTag);

private:
    PositionManager() = default;
    ~PositionManager() = default;

    void HandleFills(SCStudyInterfaceRef sc);
    void HandleReplies(void);
    void UpdateAttachedOrders(SCStudyInterfaceRef sc);
    void UpdateTradeGradeProtection(SCStudyInterfaceRef sc);  // NEW: Elder grade-based exits
    void ManageWorkingEntryOrder(SCStudyInterfaceRef sc);

    // Elite v2.4+: FlatBuffer serialization for PositionUpdate (single-copy transport path)
    bool CreatePositionUpdateFlatBuffer(
        SCStudyInterfaceRef sc,
        const uint8_t*& outBuffer,
        size_t& outSize
    ) const;

    // Pattern-based price calculation helpers
    bool CalculateTacticalTriggerPrices(SCStudyInterfaceRef sc, int patternId, bool isLong, float atr,
                                        float& entryPrice, float& stopPrice, float& targetPrice) const;
    bool CalculateStrategySetupPrices(SCStudyInterfaceRef sc, int patternId, bool isLong, float atr,
                                      float& entryPrice, float& stopPrice, float& targetPrice) const;

    // Scale-out target calculation (50/30/20 split) - uses pattern enum directly, no strings
    void CalculateScaleOutTargets(RaschkeTacticalTrigger patternTrigger, float entryPrice, float stopPrice,
                                  bool isLong, float& target1, float& target2, float& target3) const;

    bool IsDirty(SCStudyInterfaceRef sc) const;
    void CachePreviousState(SCStudyInterfaceRef sc);
    // Copies live HMM + MarketClimate indicator values into m_previous*/m_current*.
    // Pure state sync (no side effects). Called at the top of Update() every tick so
    // in-position consumers (UpdateTradeGradeProtection, EvaluateRegimeDefense) read
    // fresh regime. See docs/ADR/regime_state_wiring_fix_spec.md (Finding 1).
    void SyncRegimeState();
    void EmitGateEventTelemetry(ReasonCode reasonCode, TradeActionEnum action);
    void LogOrderFailure(
        const char* source,
        TradeActionEnum action,
        ReasonCode reasonCode,
        const char* stage,
        const std::string& detail,
        uint64_t nowUs,
        uint64_t contextSnapshotUs = 0,
        const ExecutionGate::GateContext* gateCtx = nullptr
    ) const;
    void EmitConnectivityRecoveryEvent();
    void MarkConnectivityFault(ReasonCode reasonCode, TradeActionEnum action);
    bool HasProtectiveCoverage(SCStudyInterfaceRef sc) const;
    bool IsRestartReconciliationReady(SCStudyInterfaceRef sc) const;
    bool IsConnectivityRecoveryReady(SCStudyInterfaceRef sc) const;
    bool IsConnectivityFailClosed(SCStudyInterfaceRef sc, TradeActionEnum action);
    TradeActionEnum PendingEntryAction() const;
    bool DetectSierraLookupFault(int orderLookupResult);
    bool DetectBrokerCancelFault(int cancelResult);
    bool DetectOrderAckTimeoutFault(bool timedOut, int cancelResult);
    bool DetectBrokerSubmitFault(int orderResult, TradeActionEnum action);
    bool BeginIntentTicket(
        SCStudyInterfaceRef sc,
        uint64_t intentId,
        uint64_t sequenceId,
        uint64_t createdUs,
        uint64_t ttlUs,
        TradeActionEnum action,
        const char* source
    );
    void TransitionIntentTicket(const char* nextState, ReasonCode reasonCode);
    void ClearIntentTicket(ReasonCode reasonCode = ReasonCode::NA);
    void RejectIntentTicket(TradeActionEnum action, ReasonCode reasonCode, bool emitTelemetry = true);
    bool IsIntentTicketExpired(uint64_t nowUs) const;
    bool IsQuoteChurnBreached(SCStudyInterfaceRef sc, double bid, double ask);
    void BeginPendingPredictionAck(const TradeExecutionServer::PredictionSlot& prediction, TradeActionEnum action);
    void EmitPendingPredictionAckAccepted(float entryPrice, float stopPrice, float targetPrice);
    void EmitPendingPredictionAckRejected(ReasonCode reasonCode, const std::string& detail = "");
    void ClearPendingPredictionAck();

    /// Session-aware spread threshold.  Returns the max allowed spread in ticks
    /// based on TimeOfDayIndicator (open/close sessions get tighter limits).
    /// Centralizes the logic that was previously inlined at 3 call sites.
    static float GetSessionSpreadLimit();

    /// Session-aware execution budget for working-entry TTL (milliseconds).
    /// Opening → wider (900ms), close → tighter (800ms), core → default.
    static int GetSessionExecutionBudgetMs();

    static const char* ToReasonCodeString(ReasonCode reasonCode);

    // Member variables
    Trade m_openTrade;
    Trade m_lastClosedTrade;  // Stores most recently closed trade for analytics/logging
    int m_lastFillArraySize{ -1 };

    TradeStatusEnum m_tradeStatus = TradeStatusEnum::NO_TRADE;

    // Previous state for IsDirty check
    struct PrevState {
        TradeStatusEnum status = TradeStatusEnum::NO_TRADE;
        double last = 0.0;
        double stop = 0.0;
        double target = 0.0;
        int trade_grade = 0;
    } m_prevState;

    // Trade grade protection tracking (Elder methodology)
    int m_lastTradeGradeAction{0};  // Highest grade level that triggered action (10, 20, or 30)

    // Transition-aware management tracking
    bool m_lastDefensiveTransitionMode{false};

    // Gate telemetry throttling (prevents GUI/transport flood on repetitive denials)
    ReasonCode m_lastGateEventCode{ReasonCode::NA};
    uint64_t m_lastGateEventEmitUs{0};

    // Connectivity sentinel latch: once a venue/connectivity fault is observed,
    // deny new entries for a short hold window while exits remain available.
    uint64_t m_connectivityDownUntilUs{0};
    ReasonCode m_connectivityDownReason{ReasonCode::NA};
    bool m_connectivityRecoveryPending{false};

    // Immutable entry intent ticketing (Phase A critical): one active ticket
    // per symbol/strategy in this manager scope with deterministic lifecycle.
    struct IntentTicketState {
        bool active = false;
        uint64_t intentId = 0;
        uint64_t sequenceId = 0;
        uint64_t createdUs = 0;
        uint64_t validUntilUs = 0;
        TradeActionEnum action = TradeActionEnum::STAND_ASIDE;
        std::string source;
        std::string state = "NONE";
    };
    IntentTicketState m_intentTicket;

    struct PendingPredictionAckState {
        bool active = false;
        PredictionAckEvent event;
    };
    PendingPredictionAckState m_pendingPredictionAck;
    PredictionAckSink m_predictionAckSink;

    // Quote churn tracking for microstructure admission gate.
    double m_prevQuoteBid = 0.0;
    double m_prevQuoteAsk = 0.0;
    uint64_t m_prevQuoteSampleUs = 0;
    double m_quoteChurnEmaPerSec = 0.0;

    // Gap 1: Cached ATR from TripleScreen producers (eliminates redundant per-tick TR loops)
    float m_cachedATR14 = 0.0f;
    float m_cachedATR10 = 0.0f;

    // Triple-Barrier: set when a deterministic exit (regime flatten or time barrier)
    // submits a close this tick, so the first-hit ordering (regime -> time -> stop/target)
    // is honored and no double-exit fires. Reset each tick in the in-position block.
    bool m_exitSubmittedThisTick = false;

    // Queues for communication
    std::shared_ptr<ThreadSafeQueue<TradeRequest>> m_requestQueue;
    std::shared_ptr<ThreadSafeQueue<TradeReply>> m_replyQueue;

    // Current Regime State (for Context-Aware Management)
    HMMStateEnum m_currentHMMState{HMM_NO_PRIOR};
    MarketClimate m_currentClimate{MarketClimate::GAUSSIAN_STABLE};

    // Previous regime/climate state (Gap 5 & 8: detect mid-trade transitions)
    HMMStateEnum m_previousHMMState{HMM_NO_PRIOR};
    MarketClimate m_previousClimate{MarketClimate::GAUSSIAN_STABLE};

    // Phase 3.2.1: Reusable FlatBufferBuilder for high-frequency PositionUpdate serialization.
    mutable flatbuffers::FlatBufferBuilder m_positionUpdateBuilder{256};

    struct PendingEntryOrderState {
        bool active = false;
        int orderId = 0;
        int requestedQuantity = 0;
        bool isLong = true;
        float decisionPrice = 0.0f;
        float submitPrice = 0.0f;
        std::string executionStyle;
        int repriceCount = 0;
        SCDateTime submitTime;
        SCDateTime lastRepriceTime;
        uint64_t workingOrderActiveUs = 0; // wall-clock µs when ORDER_WORKING first confirmed; 0 = not yet working
    };
    PendingEntryOrderState m_pendingEntryOrder;

    static constexpr int ENTRY_EXECUTION_BUDGET_MS = 1200;
    // NOTE: PASSIVE_HOLD, REPRICE_COOLDOWN, MAX_REPRICES removed — SC Limit Chase handles repricing natively.
    static constexpr float ENTRY_MAX_SPREAD_TICKS = 2.0f;
    static constexpr uint64_t GATE_EVENT_MIN_EMIT_INTERVAL_US = 1000000ULL; // 1s
    static constexpr uint64_t CONNECTIVITY_FAIL_CLOSED_HOLD_US = 5000000ULL; // 5s
    static constexpr uint64_t MANUAL_INTENT_TTL_US = 3000000ULL; // 3000 ms
    static constexpr uint64_t AUTOMATIC_INTENT_TTL_US = 1500000ULL; // 1500 ms
};
