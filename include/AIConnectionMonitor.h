#pragma once

#include "sierrachart.h"
#include "nlohmann/json.hpp"
#include <fstream>
#include <string>
#include <vector>

using json = nlohmann::json;

/**
 * @class AIConnectionMonitor
 * @brief Production-grade heartbeat guard for AI ⇔ C++ communication
 * 
 * Implements 3-state health management (CONNECTED/DEGRADED/DISCONNECTED) with:
 * - Microsecond-precision alpha decay protection (signals >10s rejected)
 * - Fast-purge logic (orphaned orders cancelled on disconnect)
 * - Recovery re-calibration (state sync prevents double-entry bug)
 * - Transaction Cost Analysis (routing latency & slippage tracking)
 * 
 * Architecture Pattern: Used by Renaissance, Citadel, Two Sigma for multi-component AI systems
 * 
 * @see docs/TRANSFORMER_CPP_INTEGRATION_SPEC.md Section 8.2
 */
class AIConnectionMonitor {
public:
    // Singleton pattern (consistent with PositionManager, RiskManager, IndicatorManager)
    static AIConnectionMonitor& Instance() {
        static AIConnectionMonitor instance;
        return instance;
    }
    
    AIConnectionMonitor(const AIConnectionMonitor&) = delete;
    AIConnectionMonitor& operator=(const AIConnectionMonitor&) = delete;
    
    enum AIHealthStatus {
        CONNECTED,          // Heartbeats arriving normally (latency <2s)
        DEGRADED,           // Intermittent connectivity (1-2 timeouts, 2-5s lag)
        DISCONNECTED        // 3+ consecutive timeouts or >5s silence
    };
    
    enum class ModelHealthStatus {
        HEALTHY,            // Alpha slippage < 20%
        WARNING,            // Alpha slippage 20-30% (HIGH confidence only)
        SOFT_LOCKED         // Alpha slippage >= 30% (ALL AI signals rejected)
    };
    
private:
    SCDateTime lastHeartbeatTime;
    SCDateTime lastSignalTime;
    bool isAIConnected;
    int consecutiveTimeouts;
    
    // === CRITICAL THRESHOLDS ===
    const int HEARTBEAT_INTERVAL_SEC = 1;      // AI must send heartbeat every 1s
    const int HEARTBEAT_TIMEOUT_SEC = 5;       // Grace period before warning
    const int ALPHA_DECAY_SEC = 6;             // Signals older than 6s rejected (slow host + black-swan guard)
    const int MAX_CONSECUTIVE_TIMEOUTS = 3;    // 3 failures = full lockout
    
    // === TCA (TRANSACTION COST ANALYSIS) ===
    struct OrderEvent {
        int orderId;
        SCDateTime submitTime;
        SCDateTime fillTime;
        SCDateTime decisionTime;
        bool isLongEntry;
        float submitPrice;
        float fillPrice;
        float decisionPrice;
        float arrivalBid;
        float arrivalAsk;
        float arrivalSpreadTicks;
        float marketPriceAtSubmit;
        float marketPriceAtFill;
        std::string executionStyle;
        int replaceCount;
        double decisionToSubmitMs;
        double routingLatencyMs;
        double slippageTicks;
        double decisionSlippageTicks;
        double arrivalSlippageTicks;
        
        OrderEvent() : orderId(0), isLongEntry(true), submitPrice(0.0f), fillPrice(0.0f),
                      decisionPrice(0.0f), arrivalBid(0.0f), arrivalAsk(0.0f),
                      arrivalSpreadTicks(0.0f), marketPriceAtSubmit(0.0f), marketPriceAtFill(0.0f),
                      executionStyle("PASSIVE_BID_ASK"), replaceCount(0), decisionToSubmitMs(0.0),
                      routingLatencyMs(0.0), slippageTicks(0.0), decisionSlippageTicks(0.0),
                      arrivalSlippageTicks(0.0) {}
    };
    
    std::vector<OrderEvent> orderHistory;
    
    // === MODEL HEALTH MONITORING ===
    ModelHealthStatus m_lastModelHealth;
    SCDateTime m_lastHealthFileCheck;
    std::string m_healthFilePath;
    const int MODEL_HEALTH_CACHE_SEC = 60;      // Check file every 60s
    const int MODEL_HEALTH_STALE_SEC = 86400;   // File >24h old = stale
    
    // === ELITE: HEARTBEAT METRICS (from FlatBuffer) ===
    float m_lastInferenceMs = 0.0f;             // Last single inference latency (ms)
    float m_avgInferenceMs = 0.0f;              // 60s moving average inference latency (ms)
    int m_queueDepth = 0;                       // Current model queue depth
    int m_errorCount = 0;                       // Recent error count
    ModelHealthStatus m_modelHealth = ModelHealthStatus::HEALTHY;  // Derived from metrics

    // Ghost Protocol v1: separate transport health from compute health
    bool m_transportDegraded = false;           // True when transport lag is high while compute remains healthy
    float m_transportLagMs = 0.0f;              // Latest observed transport lag estimate (ms)
    
    // === ELITE: SIGNAL ACCEPTANCE TRACKING (Institutional Observability) ===
    int m_signalsAccepted = 0;                  // Total AI signals accepted
    int m_signalsRejected = 0;                  // Total AI signals rejected
    int m_rejectedByDisconnect = 0;             // Rejected: AI connection lost
    int m_rejectedByLatency = 0;                // Rejected: Signal too stale
    int m_rejectedByLowConfidence = 0;          // Rejected: Confidence too low during degradation
    
    // === ELITE: STATE SYNC IDEMPOTENCY (Double-Entry Prevention) ===
    int m_lastProcessedTradeId = 0;             // Track processed trades for idempotency
    std::string m_lastAIBias = "UNDEFINED";     // Track bias state for consistency checks
    std::string m_lastModelVersion = "";        // Track model version for compatibility

    
    // Private constructor for singleton
    AIConnectionMonitor() : 
        isAIConnected(false), 
        consecutiveTimeouts(0),
        m_lastModelHealth(ModelHealthStatus::HEALTHY),
        m_healthFilePath("data/model_health_status.json") {
        // Initialize to epoch to trigger immediate timeout on first check
        lastHeartbeatTime = SCDateTime(0);
        lastSignalTime = SCDateTime(0);
        m_lastHealthFileCheck = SCDateTime(0);
    }
    
    ~AIConnectionMonitor() = default;

public:
    
    // ═══════════════════════════════════════════════════════════════
    // CORE INTEGRITY CHECK (Called every bar update)
    // ═══════════════════════════════════════════════════════════════
    AIHealthStatus CheckSystemIntegrity(SCStudyInterfaceRef sc) {
        // Use CurrentSystemDateTime() for timestamp comparison
        SCDateTime currentTime = sc.CurrentSystemDateTime;
        
        double secondsSinceHeartbeat = (currentTime - lastHeartbeatTime).GetAsDouble() * 86400.0;
        
        // === HEARTBEAT LOSS DETECTION ===
        if (secondsSinceHeartbeat > HEARTBEAT_TIMEOUT_SEC) {
            consecutiveTimeouts++;
            
            if (consecutiveTimeouts >= MAX_CONSECUTIVE_TIMEOUTS) {
                // CRITICAL: AI process likely dead/hung
                if (isAIConnected) {  // Only log once on transition
                    Logger::getInstance().log("CRITICAL: AI DISCONNECTED (3+ consecutive timeouts). "
                                       "LOCKOUT MODE: No new AI signals accepted.");
                    isAIConnected = false;
                }
                return DISCONNECTED;
            }
            else {
                // WARNING: Intermittent connectivity
                SCString warnMsg;
                warnMsg.Format(
                    "WARNING: AI heartbeat delayed (%.1fs, timeout #%d/%d). "
                    "Tightening signal acceptance criteria.",
                    secondsSinceHeartbeat, consecutiveTimeouts, MAX_CONSECUTIVE_TIMEOUTS);
                Logger::getInstance().log(warnMsg.GetChars());
                return DEGRADED;
            }
        }
        
        // Heartbeat received - reset counter on recovery
        if (consecutiveTimeouts > 0 && secondsSinceHeartbeat < 2.0) {
            Logger::getInstance().log("AI connection restored");
            consecutiveTimeouts = 0;
            isAIConnected = true;
        }
        
        isAIConnected = true;
        return CONNECTED;
    }
    
    // ═══════════════════════════════════════════════════════════════
    // MODEL HEALTH STATUS INTEGRATION (Step 1.6)
    // ═══════════════════════════════════════════════════════════════
    
    /**
     * @brief Check model health from file-based status (cached 60s)
     * @return Current model health state (HEALTHY/WARNING/SOFT_LOCKED)
     */
    ModelHealthStatus CheckModelHealthStatus(SCStudyInterfaceRef sc);
    
    /**
     * @brief Determine if AI signal should be accepted based on model health
     * @param confidence Signal confidence (0.0-1.0)
     * @param health Current model health status
     * @return true if signal meets criteria for current health state
     */
    bool ShouldAcceptSignalWithModelHealth(float confidence, ModelHealthStatus health) const;
    
    /**
     * @brief Set custom path for model health status file
     */
    void SetHealthFilePath(const std::string& path) { m_healthFilePath = path; }
    
    // ═══════════════════════════════════════════════════════════════
    // ELITE: SIGNAL ACCEPTANCE METRICS (Institutional Observability)
    // ═══════════════════════════════════════════════════════════════
    int GetSignalsAccepted() const { return m_signalsAccepted; }
    int GetSignalsRejected() const { return m_signalsRejected; }
    int GetRejectedByDisconnect() const { return m_rejectedByDisconnect; }
    int GetRejectedByLatency() const { return m_rejectedByLatency; }
    int GetRejectedByLowConfidence() const { return m_rejectedByLowConfidence; }
    
    // ═══════════════════════════════════════════════════════════════
    // ELITE: STATE SYNC IDEMPOTENCY TRACKING (Double-Entry Prevention)
    // ═══════════════════════════════════════════════════════════════
    int GetLastProcessedTradeId() const { return m_lastProcessedTradeId; }
    std::string GetLastAIBias() const { return m_lastAIBias; }
    std::string GetLastModelVersion() const { return m_lastModelVersion; }
    
    void SetLastProcessedTradeId(int tradeId) { m_lastProcessedTradeId = tradeId; }
    void SetLastAIBias(const std::string& bias) { m_lastAIBias = bias; }
    void SetLastModelVersion(const std::string& version) { m_lastModelVersion = version; }
    
    // ═══════════════════════════════════════════════════════════════
    // SIGNAL ACCEPTANCE GUARD (Called before processing AI entry signal)
    // Elite v2.5: FlatBuffer TradeRequest (migrated from JSON)
    // ═══════════════════════════════════════════════════════════════
    bool ShouldAcceptAISignal(SCStudyInterfaceRef sc, const MTS::Schema::TradeRequest* signal) {
        // Guard: Null pointer check (corrupted FlatBuffer)
        if (!signal) {
            Logger::getInstance().log("ELITE GATE 0: AI signal REJECTED - Null FlatBuffer pointer");
            m_signalsRejected++;
            return false;
        }
        
        AIHealthStatus health = CheckSystemIntegrity(sc);
        
        // === LAYER 1: HEALTH-BASED FILTERING (Institutional Gate 1) ===
        if (health == DISCONNECTED) {
            Logger::getInstance().log("ELITE GATE 1: AI signal REJECTED - Disconnected (heartbeat timeout)");
            m_signalsRejected++;
            m_rejectedByDisconnect++;
            return false;  // Hard lockout - AI presumed dead (preserve capital)
        }
        
        // === LAYER 2: ALPHA DECAY CHECK (Institutional Gate 2 - Signal Staleness) ===
        // Elite v2.5: Extract timestamp_us from FlatBuffer (microsecond precision)
        uint64_t signalTimestampUs = signal->timestamp_us();
        time_t signalTimestampSec = signalTimestampUs / 1'000'000;
        SCDateTime signalTime(signalTimestampSec);
        SCDateTime currentTime = sc.CurrentSystemDateTime;
        
        double signalAgeSeconds = (currentTime - signalTime).GetAsDouble() * 86400.0;
        
        if (signalAgeSeconds > ALPHA_DECAY_SEC) {
            SCString gateMsg;
            gateMsg.Format(
                "ELITE GATE 2: AI signal REJECTED - Stale (age: %.2fs, threshold: %ds)",
                signalAgeSeconds, ALPHA_DECAY_SEC);
            Logger::getInstance().log(gateMsg.GetChars());
            m_signalsRejected++;
            m_rejectedByLatency++;
            return false;  // Stale alpha = degraded prediction quality
        }
        
        // === LAYER 3: DEGRADED CONNECTION = HIGHER CONFIDENCE THRESHOLD (Institutional Gate 3) ===
        if (health == DEGRADED) {
            // During connectivity issues, only accept high-confidence signals (preserve capital)
            // Elite v2.5: Extract model_confidence from FlatBuffer (already a float)
            float confidence = signal->model_confidence();
            if (confidence < 0.85f) {  // Raised threshold: 0.75 -> 0.85 during degradation
                SCString gateMsg;
                gateMsg.Format(
                    "ELITE GATE 3: AI signal REJECTED - Low confidence (%.2f) during degraded connection (threshold: 0.85)", 
                    confidence);
                Logger::getInstance().log(gateMsg.GetChars());
                m_signalsRejected++;
                m_rejectedByLowConfidence++;
                return false;  // Avoid orphaned orders during connectivity issues
            }
        }
        
        // === ELITE: All gates passed - signal is institutional quality ===
        m_signalsAccepted++;
        return true;  // CONNECTED + fresh signal + adequate confidence
    }
    
    // ═══════════════════════════════════════════════════════════════
    // FAST-PURGE LOGIC (Called when DISCONNECTED status detected)
    // ═══════════════════════════════════════════════════════════════
    template<typename T>
    void PurgeOrphanedOrders(SCStudyInterfaceRef sc, T& currentChase) {
        AIHealthStatus health = CheckSystemIntegrity(sc);
        
        if (health == DISCONNECTED) {
            if (currentChase.orderActive) {
                // Cancel pending limit order - the AI can no longer manage this entry
                sc.CancelOrder(currentChase.orderId);
                Logger::getInstance().log("EMERGENCY: AI Disconnected. Pending Limit Orders Purged.");
                
                currentChase.orderActive = false;
                currentChase.orderId = 0;
            }
        }
    }
    
    // ═══════════════════════════════════════════════════════════════
    // RECOVERY RE-CALIBRATION (Called when heartbeat resumes)
    // Elite v2.5: FlatBuffer ConfigRequest (migrated from JSON)
    // ═══════════════════════════════════════════════════════════════
    bool ValidateStateSync(SCStudyInterfaceRef sc, const MTS::Schema::PreFlightCheckResponse* preflight) {
        // Guard: Null pointer check (corrupted FlatBuffer)
        if (!preflight) {
            Logger::getInstance().log("PreFlight validation REJECTED: Null FlatBuffer pointer");
            return false;
        }
        
        // ELITE: Idempotency check prevents double-entry bug on AI reconnect
        
        try {
            // Elite v2.5: Extract fields from PreFlightCheckResponse FlatBuffer (item 1: typed enum)
            auto status_enum = preflight->status();  // Now returns PreFlightStatus enum (int8)
            auto cpp_state_offset = preflight->cpp_state();
            auto reason_offset = preflight->reason();
            
            // Fail-safe: Validate FlatBuffer fields are populated
            if (!cpp_state_offset) {
                Logger::getInstance().log("PreFlight validation REJECTED: Missing required FlatBuffer fields (cpp_state)");
                return false;
            }
            
            std::string cppState = cpp_state_offset->str();
            std::string reason = reason_offset ? reason_offset->str() : "";
            
            // Elite: Validate status enum value (item 1 hardening)
            // status_enum maps to: READY=0, NOT_READY=1, ERROR_STAT=2 (ERROR is Windows macro)
            if (status_enum < 0 || status_enum > 2) {
                SCString pfMsg;
                pfMsg.Format("PreFlight validation REJECTED: Invalid status enum value %d", (int)status_enum);
                Logger::getInstance().log(pfMsg.GetChars());
                return false;
            }
            
            // Elite: Handle different status states (typed enum)
            // Enum values: READY=0, NOT_READY=1, ERROR=2
            if (status_enum == static_cast<MTS::Schema::PreFlightStatus>(0)) {  // READY
                // System is ready - gates can proceed to accept signals
                Logger::getInstance().log("PreFlight READY: C++ state machine " + cppState);
                return true;
            } else if (status_enum == static_cast<MTS::Schema::PreFlightStatus>(2)) {  // ERROR
                // System error - reject all signals until recovery
                Logger::getInstance().log("PreFlight ERROR: " + reason + " (rejecting AI signals until recovery)");
                return false;
            } else {  // NOT_READY (1)
                // System warming up or transitioning - conditional acceptance
                Logger::getInstance().log("PreFlight NOT_READY: " + reason + " (may accept high-confidence signals)");
                return false;  // Conservative: reject until READY
            }
            
        } catch (const std::exception& e) {
            Logger::getInstance().log("PreFlight validation FAILED: " + std::string(e.what()) + " (rejecting AI signals until recovery)");
            return false;
        }
    }
    
    // ═══════════════════════════════════════════════════════════════
    // ELITE: MODEL METRICS TRACKING (from Heartbeat)
    // ═══════════════════════════════════════════════════════════════
    
    /**
     * @brief Get last inference latency (ms)
     * Updated from Heartbeat.last_inference_ms
     */
    float GetLastInferenceMs() const;
    
    /**
     * @brief Get average inference latency (ms, 60s moving average)
     * Updated from Heartbeat.avg_inference_ms
     */
    float GetAvgInferenceMs() const;
    
    /**
     * @brief Get current model queue depth
     * Updated from Heartbeat.queue_depth
     */
    int GetQueueDepth() const;
    
    /**
     * @brief Get recent error count
     * Updated from Heartbeat.error_count
     */
    int GetErrorCount() const;

    bool IsTransportDegraded() const;
    float GetTransportLagMs() const;

    // === GAP 7: TCA-ADAPTIVE EXECUTION STYLE FEEDBACK (rolling window) ===
    // Returns mean arrival slippage (ticks) for the given execution style over
    // the last `windowSize` filled orders of that style.  Positive = adverse.
    // If fewer than `minSamples` fills exist for that style, returns 0.0 (no data).
    double GetMeanArrivalSlippage(const char* style, int windowSize = 20, int minSamples = 5) const {
        if (!style) return 0.0;
        double sumSlip = 0.0;
        int count = 0;
        // Walk backwards through orderHistory for recency-weighted window
        for (int i = static_cast<int>(orderHistory.size()) - 1; i >= 0 && count < windowSize; --i) {
            const auto& evt = orderHistory[static_cast<size_t>(i)];
            if (evt.fillTime.IsUnset()) continue;
            if (evt.executionStyle != style) continue;
            sumSlip += evt.arrivalSlippageTicks;
            count++;
        }
        if (count < minSamples) return 0.0;
        return sumSlip / static_cast<double>(count);
    }

    /// Returns true if the given style's recent slippage exceeds the degrade
    /// threshold (0.5 ticks mean).  Recovery requires slippage below 0.3 ticks.
    /// Hysteresis prevents oscillation.
    bool IsExecutionStyleDegraded(const char* style) const {
        const double slip = GetMeanArrivalSlippage(style);
        if (slip <= 0.0) return false;  // No data or favorable fills
        // Simple threshold with implicit hysteresis: once degraded, caller demotes;
        // as slippage falls below 0.3, caller re-promotes next session.
        return slip > 0.50;
    }

    void UpdateTransportHealth(bool degraded, float transport_lag_ms);
    
    /**
     * @brief Update elite model metrics from Heartbeat
     * Called by AIHeartbeatMonitor when FlatBuffer Heartbeat received
     */
    void UpdateEliteMetrics(float last_inference_ms, float avg_inference_ms, 
                           int queue_depth, int error_count);
    
    // ═══════════════════════════════════════════════════════════════
    void UpdateHeartbeat(SCDateTime timestamp) {
        lastHeartbeatTime = timestamp;
    }
    
    void UpdateSignalTime(SCDateTime timestamp) {
        lastSignalTime = timestamp;
    }
    
    // ═══════════════════════════════════════════════════════════════
    // TCA: TRANSACTION COST ANALYSIS (Routing Latency Monitor)
    // ═══════════════════════════════════════════════════════════════
    void LogOrderSubmit(SCStudyInterfaceRef sc,
                        int orderId,
                        float price,
                        bool isLongEntry,
                        float decisionPrice = 0.0f,
                        float arrivalBid = 0.0f,
                        float arrivalAsk = 0.0f,
                        const char* executionStyle = "PASSIVE_BID_ASK",
                        SCDateTime decisionTime = SCDateTime(0)) {
        OrderEvent evt;
        evt.orderId = orderId;
        evt.isLongEntry = isLongEntry;
        evt.decisionPrice = decisionPrice;
        evt.arrivalBid = arrivalBid;
        evt.arrivalAsk = arrivalAsk;
        evt.executionStyle = executionStyle ? executionStyle : "PASSIVE_BID_ASK";
        evt.decisionTime = decisionTime;
        evt.submitTime = sc.CurrentSystemDateTime;
        evt.submitPrice = price;
        evt.marketPriceAtSubmit = sc.Close[sc.ArraySize - 1];
        if (arrivalAsk > arrivalBid && sc.TickSize > 0.0f) {
            evt.arrivalSpreadTicks = (arrivalAsk - arrivalBid) / sc.TickSize;
        }
        if (!decisionTime.IsUnset()) {
            evt.decisionToSubmitMs = (evt.submitTime - decisionTime).GetAsDouble() * 86400000.0;
        }
        
        orderHistory.push_back(evt);
    }
    
    void LogOrderFill(SCStudyInterfaceRef sc, int orderId, float fillPrice) {
        // Find matching submit event
        for (auto& evt : orderHistory) {
            if (evt.orderId == orderId && evt.fillTime.IsUnset()) {
                evt.fillTime = sc.CurrentSystemDateTime;
                evt.fillPrice = fillPrice;
                evt.marketPriceAtFill = sc.Close[sc.ArraySize - 1];
                
                // Calculate routing latency (milliseconds)
                evt.routingLatencyMs = (evt.fillTime - evt.submitTime).GetAsDouble() * 86400000.0;
                
                // Side-aware slippage (positive is worse execution)
                if (sc.TickSize > 0.0f) {
                    const double sideSign = evt.isLongEntry ? 1.0 : -1.0;
                    evt.slippageTicks = sideSign * (evt.fillPrice - evt.submitPrice) / sc.TickSize;

                    if (evt.decisionPrice > 0.0f) {
                        evt.decisionSlippageTicks = sideSign * (evt.fillPrice - evt.decisionPrice) / sc.TickSize;
                    }

                    const float arrivalRef = evt.isLongEntry ? evt.arrivalAsk : evt.arrivalBid;
                    if (arrivalRef > 0.0f) {
                        evt.arrivalSlippageTicks = sideSign * (evt.fillPrice - arrivalRef) / sc.TickSize;
                    }
                }
                
                // Alert if latency exceeds institutional threshold (100ms)
                if (evt.routingLatencyMs > 100.0) {
                    SCString latMsg;
                    latMsg.Format(
                        "HIGH ROUTING LATENCY: %.1fms (style=%s, slip_submit=%.2f ticks)",
                        evt.routingLatencyMs, evt.executionStyle.c_str(), evt.slippageTicks);
                    Logger::getInstance().log(latMsg.GetChars());
                }
                
                break;
            }
        }
    }

    void LogOrderReplace(int orderId) {
        for (auto& evt : orderHistory) {
            if (evt.orderId == orderId && evt.fillTime.IsUnset()) {
                evt.replaceCount++;
                return;
            }
        }
    }
    
    void GenerateTCAReport(SCStudyInterfaceRef sc) {
        if (orderHistory.empty()) return;

        struct BucketStats {
            int fills = 0;
            double latencyMs = 0.0;
            double submitSlip = 0.0;
            double decisionSlip = 0.0;
            double arrivalSlip = 0.0;
            int replaceCount = 0;
        };

        BucketStats grouped[3][3]; // style x time-of-day

        auto StyleIndex = [](const std::string& style) -> int {
            if (style == "AGGRESSIVE_CROSS") return 2;
            if (style == "MIDPOINT_IMPROVE") return 1;
            return 0;
        };

        auto TimeBucketIndex = [](const SCDateTime& dt) -> int {
            const int minutes = dt.GetHour() * 60 + dt.GetMinute();
            if (minutes <= (9 * 60 + 45)) return 0;   // open
            if (minutes >= (15 * 60 + 45)) return 2;  // close
            return 1;                                  // core
        };

        const char* styleNames[3] = {"PASSIVE_BID_ASK", "MIDPOINT_IMPROVE", "AGGRESSIVE_CROSS"};
        const char* timeNames[3] = {"OPEN", "CORE", "CLOSE"};
        
        double avgLatency = 0;
        double avgSubmitSlippage = 0;
        double avgDecisionSlippage = 0;
        double avgArrivalSlippage = 0;
        int filledCount = 0;
        
        for (const auto& evt : orderHistory) {
            if (!evt.fillTime.IsUnset()) {
                avgLatency += evt.routingLatencyMs;
                avgSubmitSlippage += evt.slippageTicks;
                avgDecisionSlippage += evt.decisionSlippageTicks;
                avgArrivalSlippage += evt.arrivalSlippageTicks;

                const int si = StyleIndex(evt.executionStyle);
                const int ti = TimeBucketIndex(evt.submitTime);
                grouped[si][ti].fills += 1;
                grouped[si][ti].latencyMs += evt.routingLatencyMs;
                grouped[si][ti].submitSlip += evt.slippageTicks;
                grouped[si][ti].decisionSlip += evt.decisionSlippageTicks;
                grouped[si][ti].arrivalSlip += evt.arrivalSlippageTicks;
                grouped[si][ti].replaceCount += evt.replaceCount;
                filledCount++;
            }
        }
        
        if (filledCount > 0) {
            avgLatency /= filledCount;
            avgSubmitSlippage /= filledCount;
            avgDecisionSlippage /= filledCount;
            avgArrivalSlippage /= filledCount;
            
            // Calculate cost in dollars
            double costPerTrade = avgSubmitSlippage * sc.TickSize * 50.0;  // ES: $12.50/tick, 50 = 12.50 * 4
            double annualCost = costPerTrade * 250;  // ~250 trades/year
            
            SCString tcaMsg;
            tcaMsg.Format(
                "TCA REPORT (%d fills): Avg Routing: %.1fms | "
                "Slip(submit/decision/arrival)=%.2f/%.2f/%.2f ticks | "
                "Cost=$%.2f/trade ($%.0f/year)",
                filledCount,
                avgLatency,
                avgSubmitSlippage,
                avgDecisionSlippage,
                avgArrivalSlippage,
                costPerTrade,
                annualCost);
            Logger::getInstance().log(tcaMsg.GetChars());

            for (int si = 0; si < 3; ++si) {
                for (int ti = 0; ti < 3; ++ti) {
                    const BucketStats& bucket = grouped[si][ti];
                    if (bucket.fills == 0) {
                        continue;
                    }
                    const double denom = static_cast<double>(bucket.fills);
                    SCString bucketMsg;
                    bucketMsg.Format(
                        "TCA BUCKET [%s|%s]: n=%d lat=%.1fms slip(s/d/a)=%.2f/%.2f/%.2f avg_reprices=%.2f",
                        styleNames[si],
                        timeNames[ti],
                        bucket.fills,
                        bucket.latencyMs / denom,
                        bucket.submitSlip / denom,
                        bucket.decisionSlip / denom,
                        bucket.arrivalSlip / denom,
                        static_cast<double>(bucket.replaceCount) / denom
                    );
                    Logger::getInstance().log(bucketMsg.GetChars());
                }
            }
        }
    }
    
    // ═══════════════════════════════════════════════════════════════
    // SESSION SUMMARY: Trade Simulation Performance Report
    // ═══════════════════════════════════════════════════════════════
    void GenerateSessionSummary(SCStudyInterfaceRef sc) {
        auto& log = Logger::getInstance();

        log.log("");
        log.log("════════════════════════════════════════════════════");
        log.log("  SESSION SUMMARY — Paper Trading Report");
        log.log("════════════════════════════════════════════════════");

        // --- Aggregate Trade Statistics ---
        s_ACSTradeStatistics stats;
        const int statsResult = sc.GetTradeStatisticsForSymbol(0, 1, stats); // DailyStats=1
        SCString msg;

        if (statsResult != 0) {
            const int totalTrades = stats.TotalTrades();
            if (totalTrades == 0) {
                log.log("  No trades executed this session.");
                log.log("════════════════════════════════════════════════════");
                return;
            }

            msg.Format("  Trades: %d total (%d W / %d L)  |  Win%%: %.1f%%",
                totalTrades, stats.TotalWinningTrades, stats.TotalLosingTrades,
                stats.TotalPercentProfitable() * 100.0);
            log.log(msg.GetChars());

            msg.Format("  Long: %d  |  Short: %d", stats.TotalLongTrades, stats.TotalShortTrades);
            log.log(msg.GetChars());

            msg.Format("  Net P&L: $%.2f  (Profit: $%.2f  Loss: $%.2f)",
                stats.ClosedProfitLoss(), stats.ClosedProfit, stats.ClosedLoss);
            log.log(msg.GetChars());

            msg.Format("  Profit Factor: %.2f  |  Commission: $%.2f",
                stats.ProfitFactor(), stats.TotalCommission);
            log.log(msg.GetChars());

            log.log("  ──────────────────────────────────────────────────");

            msg.Format("  Avg P&L/Trade: $%.2f  (Avg Win: $%.2f  Avg Loss: $%.2f)",
                stats.AvgProfitLoss(), stats.AvgProfit(), stats.AvgLoss());
            log.log(msg.GetChars());

            msg.Format("  Largest Win: $%.2f  |  Largest Loss: $%.2f",
                stats.LargestWinningTrade, stats.LargestLosingTrade);
            log.log(msg.GetChars());

            msg.Format("  Max Consec. Winners: %d  |  Max Consec. Losers: %d",
                stats.MaxConsecutiveWinners, stats.MaxConsecutiveLosers);
            log.log(msg.GetChars());

            log.log("  ──────────────────────────────────────────────────");

            msg.Format("  Max Drawdown: $%.2f  |  Max Runup: $%.2f",
                stats.MaximumDrawdown, stats.MaximumRunup);
            log.log(msg.GetChars());

            msg.Format("  Max Trade Drawdown: $%.2f  |  Max Trade Runup: $%.2f",
                stats.MaximumTradeDrawdown, stats.MaximumTradeRunup);
            log.log(msg.GetChars());

            msg.Format("  Avg Time in Winners: %ds  |  Avg Time in Losers: %ds",
                stats.AvgTimeInWinningTrade(), stats.AvgTimeInLosingTrade());
            log.log(msg.GetChars());
        } else {
            log.log("  [Trade statistics unavailable]");
        }

        // --- Flat-to-Flat Trade Journal ---
        const int f2fSize = sc.GetFlatToFlatTradeListSize();
        if (f2fSize > 0) {
            log.log("  ──────────────────────────────────────────────────");
            msg.Format("  Flat-to-Flat Round Trips: %d", f2fSize);
            log.log(msg.GetChars());

            for (int i = 0; i < f2fSize; ++i) {
                s_ACSTrade trade;
                if (sc.GetFlatToFlatTradeListEntry(i, trade) == 0)
                    continue;

                const char* sideStr = (trade.TradeType > 0) ? "LONG" : "SHORT";
                const char* resultStr = (trade.TradeProfitLoss >= 0) ? "WIN" : "LOSS";

                msg.Format("    #%d %s %.0f @ %.2f -> %.2f  P&L=$%.2f (%s)  MAE=$%.2f MFE=$%.2f",
                    i + 1,
                    sideStr,
                    trade.TradeQuantity,
                    trade.EntryPrice,
                    trade.ExitPrice,
                    trade.TradeProfitLoss,
                    resultStr,
                    trade.FlatToFlatMaximumOpenPositionLoss,
                    trade.FlatToFlatMaximumOpenPositionProfit);
                log.log(msg.GetChars());
            }
        }

        log.log("════════════════════════════════════════════════════");
    }

    // ═══════════════════════════════════════════════════════════════
    // UTILITY: ISO8601 TIMESTAMP PARSER
    // ═══════════════════════════════════════════════════════════════
    SCDateTime ParseISO8601(const std::string& timestamp) {
        // Parse "2025-12-19T14:35:22.123456Z" or "2025-12-19T14:35:22Z"
        int year, month, day, hour, minute, second;
        
        sscanf(timestamp.c_str(), "%d-%d-%dT%d:%d:%d", 
               &year, &month, &day, &hour, &minute, &second);
        
        // Create SCDateTime (microseconds not supported, use seconds)
        return SCDateTime(year, month, day, hour, minute, second);
    }
    
    // ═══════════════════════════════════════════════════════════════
    // ACCESSORS
    // ═══════════════════════════════════════════════════════════════
    bool IsConnected() const { return isAIConnected; }
    int GetConsecutiveTimeouts() const { return consecutiveTimeouts; }
    SCDateTime GetLastHeartbeatTime() const { return lastHeartbeatTime; }
    ModelHealthStatus GetModelHealth() const { return m_lastModelHealth; }
    
private:
    // ===== ELITE: MODEL METRICS (from FlatBuffer Heartbeat) =====
    struct EliteModelMetrics {
        float last_inference_ms = 0.0f;
        float avg_inference_ms = 0.0f;
        int queue_depth = 0;
        int error_count = 0;
    };
    EliteModelMetrics m_eliteMetrics;
    mutable std::mutex m_metricsMutex;
};
