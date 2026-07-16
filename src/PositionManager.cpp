#include "MindfulTrader_Precompiled.h"
#include "Scoring.h"
#include "TripleBarrierExitManager.h"
#include "TradeExecutionServer.h"
#include "execution/ExecutionGate.h"
#include "messaging/EliteFlatBufferHelper.h"
#include "transport/TransportStream.h"

//
// PositionManager Implementation

namespace {
uint64_t WallClockNowUs() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
}

MTS::Schema::ReasonCode ToSchemaReasonCode(PositionManager::ReasonCode reasonCode) {
    if (reasonCode == PositionManager::ReasonCode::NA) {
        return MTS::Schema::ReasonCode_UNKNOWN;
    }
    return static_cast<MTS::Schema::ReasonCode>(reasonCode);
}

MTS::Schema::IntentState ToSchemaIntentState(const std::string& state) {
    if (state == "OPEN") {
        return MTS::Schema::IntentState_OPEN;
    }
    if (state == "PENDING") {
        return MTS::Schema::IntentState_PENDING;
    }
    if (state == "EXECUTED") {
        return MTS::Schema::IntentState_EXECUTED;
    }
    if (state == "SUPERSEDED") {
        return MTS::Schema::IntentState_SUPERSEDED;
    }
    if (state == "REJECTED") {
        return MTS::Schema::IntentState_REJECTED;
    }
    if (state == "CLOSED") {
        return MTS::Schema::IntentState_CLOSED;
    }
    return MTS::Schema::IntentState_INACTIVE;
}

ExecutionGate::ReasonCode ToGateConnectivityReason(PositionManager::ReasonCode reasonCode) {
    switch (reasonCode) {
        case PositionManager::ReasonCode::IbDisconnected:
            return ExecutionGate::ReasonCode::IbDisconnected;
        case PositionManager::ReasonCode::OrderAckTimeout:
            return ExecutionGate::ReasonCode::OrderAckTimeout;
        case PositionManager::ReasonCode::ScDisconnected:
        case PositionManager::ReasonCode::NA:
        default:
            return ExecutionGate::ReasonCode::ScDisconnected;
    }
}

float ComputeParetoTopStateRatioProxy(const LocalRiskContext& ctx) {
    const float alpha = ctx.paretoTailAlpha;
    if (!std::isfinite(alpha) || alpha <= 1e-6f) {
        return 1.0f;
    }
    return std::clamp(1.0f / alpha, 0.0f, 1.0f);
}

PositionManager::ReasonCode ToPositionReasonCode(ExecutionGate::ReasonCode reasonCode) {
    switch (reasonCode) {
        case ExecutionGate::ReasonCode::ContextStale:
            return PositionManager::ReasonCode::ContextStale;
        case ExecutionGate::ReasonCode::EntryDisabledGlobal:
            return PositionManager::ReasonCode::EntryDisabledGlobal;
        case ExecutionGate::ReasonCode::HardSafetyViolation:
            return PositionManager::ReasonCode::HardSafetyViolation;
        case ExecutionGate::ReasonCode::IbDisconnected:
            return PositionManager::ReasonCode::IbDisconnected;
        case ExecutionGate::ReasonCode::IntentSuperseded:
            return PositionManager::ReasonCode::IntentSuperseded;
        case ExecutionGate::ReasonCode::ModelNotReady:
            return PositionManager::ReasonCode::ModelNotReady;
        case ExecutionGate::ReasonCode::OrderAckTimeout:
            return PositionManager::ReasonCode::OrderAckTimeout;
        case ExecutionGate::ReasonCode::PredictionStale:
            return PositionManager::ReasonCode::PredictionStale;
        case ExecutionGate::ReasonCode::RiskDailyLimit:
            return PositionManager::ReasonCode::RiskDailyLimit;
        case ExecutionGate::ReasonCode::ScDisconnected:
            return PositionManager::ReasonCode::ScDisconnected;
        case ExecutionGate::ReasonCode::HmmRegimeGateParetoBreach:
            return PositionManager::ReasonCode::HmmRegimeGateParetoBreach;
        case ExecutionGate::ReasonCode::HmmRegimeGateShannonBreach:
            return PositionManager::ReasonCode::HmmRegimeGateShannonBreach;
        case ExecutionGate::ReasonCode::HmmRegimeGateTalebBreach:
            return PositionManager::ReasonCode::HmmRegimeGateTalebBreach;
        case ExecutionGate::ReasonCode::PolicyEvalErrorFailClosed:
            return PositionManager::ReasonCode::PolicyEvalErrorFailClosed;
        case ExecutionGate::ReasonCode::Allow:
        default:
            return PositionManager::ReasonCode::NA;
    }
}

bool ShouldEmitGateTelemetry(PositionManager::ReasonCode reasonCode) {
    return reasonCode != PositionManager::ReasonCode::ScDisconnected &&
           reasonCode != PositionManager::ReasonCode::IbDisconnected &&
           reasonCode != PositionManager::ReasonCode::OrderAckTimeout &&
           reasonCode != PositionManager::ReasonCode::IntentSuperseded &&
           reasonCode != PositionManager::ReasonCode::RiskDailyLimit &&
           reasonCode != PositionManager::ReasonCode::PolicyEvalErrorFailClosed;
}

const char* Ts2HurstStateToString(HurstExponentEnum state) {
    switch (state) {
        case HurstExponentEnum::ANTI_PERSISTENT_STRONG:
            return "ANTI_PERSISTENT_STRONG";
        case HurstExponentEnum::ANTI_PERSISTENT_WEAK:
            return "ANTI_PERSISTENT_WEAK";
        case HurstExponentEnum::PERSISTENT_WEAK:
            return "PERSISTENT_WEAK";
        case HurstExponentEnum::PERSISTENT_STRONG:
            return "PERSISTENT_STRONG";
        case HurstExponentEnum::RANDOM_WALK:
        default:
            return "RANDOM_WALK";
    }
}

struct Ts2HurstRegimeDecision {
    bool allow;
    float confidenceMultiplier;
};

Ts2HurstRegimeDecision EvaluateTs2HurstRegime(HurstExponentEnum state) {
    switch (state) {
        case HurstExponentEnum::ANTI_PERSISTENT_STRONG:
            // Hard block in strongly mean-reverting wave regime.
            return {false, 0.0f};
        case HurstExponentEnum::ANTI_PERSISTENT_WEAK:
            return {true, 0.70f};
        case HurstExponentEnum::RANDOM_WALK:
            return {true, 0.85f};
        case HurstExponentEnum::PERSISTENT_STRONG:
            return {true, 1.05f};
        case HurstExponentEnum::PERSISTENT_WEAK:
        default:
            return {true, 1.00f};
    }
}
}

PositionManager& PositionManager::Instance() {
    static PositionManager singletonInstance;
    return singletonInstance;
}

void PositionManager::Init(SCStudyInterfaceRef sc,
                           std::shared_ptr<ThreadSafeQueue<TradeRequest>> req,
                           std::shared_ptr<ThreadSafeQueue<TradeReply>> rep) {
    m_requestQueue = req;
    m_replyQueue = rep;
    m_lastFillArraySize = sc.GetOrderFillArraySize();
}

void PositionManager::Reset(SCStudyInterfaceRef sc) {
    m_openTrade.Reset(sc);
    m_prevState = {};
    m_pendingEntryOrder = {};
    m_intentTicket = {};
    m_connectivityDownUntilUs = 0;
    m_connectivityDownReason = ReasonCode::NA;
    m_connectivityRecoveryPending = false;
    m_prevQuoteBid = 0.0;
    m_prevQuoteAsk = 0.0;
    m_prevQuoteSampleUs = 0;
    m_quoteChurnEmaPerSec = 0.0;
}

void PositionManager::Update(SCStudyInterfaceRef sc) {
    CachePreviousState(sc);

    // === Finding 1 fix (regime_state_wiring_fix_spec.md) ===
    // Refresh regime state every tick BEFORE any in-position consumer
    // (UpdateTradeGradeProtection, EvaluateRegimeDefense). The HMM/climate
    // indicators are already refreshed by ContextManager::CheckAndTriggerHMM(),
    // which precedes Update() in both SCStudies and BackTesterStudy. Pure sync —
    // EvaluateRegimeDefense remains the single per-tick defense call below.
    SyncRegimeState();

    // === ELITE v3.2: UNIFIED HARD GATE — first thing on every tick ===
    // Checks LocalRiskContext physics gates and dispatches:
    //   FLAT, no orders  → silent block (entries rejected below)
    //   FLAT + orders    → cancel working orders
    //   IN POSITION      → emergency flatten
    [[maybe_unused]] const bool hardGatesPass = EnforceHardGates(sc);

    // Process pending manual commands from TradeExecutionServer single-slot (UI/API directives)
    // Applies identical gate chain as Transformer predictions for risk parity
    {
        TradeExecutionServer::ManualCommandSlot manualCmd;
        if (TradeExecutionServer::Instance().ConsumePendingManualCommand(manualCmd)) {
            ProcessManualTradeCommand(
                sc,
                manualCmd.isLong,
                manualCmd.entryPrice,
                manualCmd.stopPrice,
                manualCmd.targetPrice,
                manualCmd.quantity,
                manualCmd.patternName,
                manualCmd.intentId,
                manualCmd.sequenceId,
                manualCmd.timestampUs
            );
        }
    }

    // Process pending prediction from TradeExecutionServer single-slot
    // (will be rejected inside if hardGatesPass == false)
    ProcessPendingPrediction(sc);

    // Manage working entry order lifecycle in C++ (bounded cancel/replace discipline)
    ManageWorkingEntryOrder(sc);

    HandleReplies();
    HandleFills(sc);

    if (!IsFlat()) {
        m_exitSubmittedThisTick = false;
        UpdateAttachedOrders(sc);

        auto intermPriceAction = IndicatorManager::Instance().GetIndicator<IntermediateMarketAction>(IndicatorKey::SHORT_MKT_ACTION);
        if (intermPriceAction)
            m_openTrade.SetChannel(intermPriceAction->Channel());

        // Update trade object (calculates grades)
        m_openTrade.Update(sc);

        // === TRAP — PRIORITY #1 native reactive floor (completed-bar StructureTest) ===
        // Ranks ahead of stop/target/time (matches the labeler first-hit ordering).
        // Deterministic, model-independent; may exit immediately. If it fires it sets
        // m_exitSubmittedThisTick, so the defense/time layers below stand down (no double-exit).
        EvaluateNativeTrapFloor(sc);

        // NEW: Check for Elder grade-based profit protection
        if (!m_exitSubmittedThisTick) {
            UpdateTradeGradeProtection(sc);
        }

        // Elite v2.5: Context-Aware Defensive Management
        if (!m_exitSubmittedThisTick) {
            EvaluateRegimeDefense(sc);
        }

        // Triple-Barrier vertical (time) barrier — deterministic max-hold close.
        // First-hit ordering: TRAP -> regime (EvaluateRegimeDefense, above) -> time -> stop/target
        // (stop/target are handled by the SC bracket in this phase). Uses the entry-latched
        // bracket so maxBars is fixed at the entry regime (train/live parity). Guarded by
        // m_exitSubmittedThisTick so an earlier deterministic exit this tick wins (no double-exit).
        if (!m_exitSubmittedThisTick) {
            auto& tbMgr = TripleBarrierExitManager::getInstance();
            const auto& tbBr = tbMgr.Current();
            if (tbBr.active && (sc.Index - tbBr.entryBarIndex) >= tbBr.maxBars) {
                Logger::getInstance().log("[TB] vertical/time barrier reached -> closing at market (TIME_STOP)");
                ClosePositionAtMarket(sc, "TIME_STOP");
                tbMgr.Close();
            }
        }
    }

    // After all state changes, publish FlatBuffer position update
    PublishSnapshot(sc);
}

void PositionManager::HandleFills(SCStudyInterfaceRef sc) {
    const int currentFillArraySize = sc.GetOrderFillArraySize();
    if (currentFillArraySize <= m_lastFillArraySize)
        return;

    s_SCOrderFillData latestFill;
    sc.GetOrderFillEntry(currentFillArraySize - 1, latestFill);

    s_SCPositionData pos;
    sc.GetTradePosition(pos);

    const bool wasFlat = m_prevState.status == TradeStatusEnum::NO_TRADE;
    const bool isNowFlat = pos.PositionQuantity == 0;

    if (wasFlat && !isNowFlat) { // Opening a new position
        s_SCTradeOrder order;
        sc.GetOrderByOrderID(latestFill.InternalOrderID, order);

        // === TCA: LOG ORDER FILL (Step 1.7) ===
        AIConnectionMonitor::Instance().LogOrderFill(sc, latestFill.InternalOrderID, latestFill.FillPrice);

        m_openTrade.Open(sc, order.InternalOrderID, pos.PositionQuantity, latestFill.FillPrice, sc.BaseDateTimeIn[sc.Index].GetAsDouble(), order);

        const int openedQuantity = std::abs(pos.PositionQuantity);
        const bool hasPartialRemainder =
            m_pendingEntryOrder.active &&
            m_pendingEntryOrder.requestedQuantity > 0 &&
            openedQuantity > 0 &&
            openedQuantity < m_pendingEntryOrder.requestedQuantity;

        // === GAP 19: INTELLIGENT PARTIAL FILL REBASING (Raschke — capture close fills) ===
        // If the partial fill price is within 0.5 ticks of decision price, the remainder
        // is very close to completing — grant a brief grace window via working order TTL
        // instead of immediately cancelling.
        bool partialGraceActive = false;
        if (hasPartialRemainder) {
            const float actualFillPrice = latestFill.FillPrice;
            const float slippageTicks = std::fabs(actualFillPrice - m_pendingEntryOrder.decisionPrice)
                                        / sc.TickSize;

            if (slippageTicks > 0.5f) {
                // Fill is far from decision price — cancel remainder immediately
                DetectBrokerCancelFault(sc.CancelOrder(m_pendingEntryOrder.orderId));
                EmitGateEventTelemetry(ReasonCode::PartialRemainderCancelled, m_pendingEntryOrder.isLong ? TradeActionEnum::ENTER_LONG : TradeActionEnum::ENTER_SHORT);
                ClearIntentTicket(ReasonCode::PartialRemainderCancelled);
            } else {
                // Fill is close — grant grace window; working order TTL handles expiry
                m_pendingEntryOrder.workingOrderActiveUs = WallClockNowUs();
                partialGraceActive = true;
            }
        }

        if (!partialGraceActive) {
            m_pendingEntryOrder = {};
            ClearIntentTicket(ReasonCode::IntentFilled);
        }

        // Non-negotiable partial-fill safety: if stop protection cannot be confirmed,
        // flatten immediately to avoid unprotected residual exposure.
        if (!HasProtectiveCoverage(sc)) {
            EmitGateEventTelemetry(ReasonCode::HardSafetyViolation, pos.PositionQuantity > 0 ? TradeActionEnum::ENTER_LONG : TradeActionEnum::ENTER_SHORT);
            EmergencyFlattenPosition(sc, ToReasonCodeString(ReasonCode::HardSafetyViolation));
            m_lastFillArraySize = currentFillArraySize;
            return;
        }

        // Reset grade tracking for new position
        m_lastTradeGradeAction = 0;

        // === ELITE GAP 2: FILL FEEDBACK TO GUI ===
        PublishSnapshot(sc);  // Immediate position update

        // === ELITE GAP 6: TCA METRICS ON FILL ===
        // TCA metrics exported via EventSerializer + TrainingEvent FlatBuffers via TransportStream

        // Notify RiskManager that position opened (cache unrealized P&L tracking)
        RiskManager::Instance().OnPositionOpened(sc);

        // === Triple-Barrier shadow (Phase 1, step 1a — NON-DESTRUCTIVE A/B) ===
        // Compute engine barriers alongside the live pattern/Chandelier stop+target
        // and log the comparison. Drives NO orders (Chandelier remains authoritative);
        // validates the D1 live-source mapping numerically on replay data before the
        // engine-driven cutover. See docs/ADR/triple_barrier_cutover_phase1_plan.md.
        try {
            const int tbRawPattern = m_openTrade.GetPatternId();
            const bool tbValid = (tbRawPattern >= 1 &&
                                  tbRawPattern < static_cast<int>(tbe::kPatternTable.size()));
            const int tbPattern = tbValid ? tbRawPattern : 0;  // D2 fail-closed: safe LOW-tier default
            const tbe::Tier tbTier = tbe::kPatternTable[static_cast<std::size_t>(tbPattern)].tier;
            const bool tbLong = (pos.PositionQuantity > 0);

            const auto* tbHmm = InferenceManager::Instance().HmmState();
            const tbe::Regime tbRegime = tbHmm
                ? TripleBarrierExitManager::ToRegime(tbHmm->Value())
                : tbe::Regime::GAUSSIAN_STABLE;
            const double tbDofScale = tbHmm ? tbHmm->DofStopScale() : 1.5;

            float tbSwingHigh = 0.0f, tbSwingLow = 0.0f;
            if (auto tbIma = IndicatorManager::Instance().GetIndicator<IntermediateMarketAction>(IndicatorKey::SHORT_MKT_ACTION)) {
                tbSwingHigh = tbIma->swingHigh();
                tbSwingLow  = tbIma->swingLow();
            }

            // For HIGH-tier patterns the pattern target IS the N-bar structural extreme
            // (Turtle Soup 4-bar high / Momentum Pinball swing); absent for other tiers.
            const double tbStructTarget = m_openTrade.GetTarget();
            const double tbNbarHigh = (tbTier == tbe::Tier::HIGH_DEDICATED &&  tbLong) ? tbStructTarget : 0.0;
            const double tbNbarLow  = (tbTier == tbe::Tier::HIGH_DEDICATED && !tbLong) ? tbStructTarget : 0.0;

            tbe::BarrierInputs tbIn{};
            tbIn.pattern_id              = tbPattern;
            tbIn.is_long                 = tbLong;
            tbIn.entry                   = static_cast<double>(latestFill.FillPrice);
            tbIn.bar_high                = static_cast<double>(sc.High[sc.Index]);
            tbIn.bar_low                 = static_cast<double>(sc.Low[sc.Index]);
            tbIn.prev_high               = (sc.Index >= 1) ? static_cast<double>(sc.High[sc.Index - 1]) : tbIn.bar_high;
            tbIn.prev_low                = (sc.Index >= 1) ? static_cast<double>(sc.Low[sc.Index - 1]) : tbIn.bar_low;
            tbIn.atr10                   = static_cast<double>(m_cachedATR10);
            tbIn.dof_stop_scale          = tbDofScale;
            tbIn.regime_stop_width_scale = 1.0;   // Phase 1 identity
            tbIn.nbar_extreme_high       = tbNbarHigh;
            tbIn.nbar_extreme_low        = tbNbarLow;
            tbIn.swing_high              = static_cast<double>(tbSwingHigh);
            tbIn.swing_low               = static_cast<double>(tbSwingLow);
            tbIn.regime                  = tbRegime;
            tbIn.tick_size               = sc.TickSize;

            const tbe::Barriers tbB = tbe::ComputeBarriers(tbIn);
            const double tbRR = (tbB.risk > 0.0) ? (tbB.reward_at_target / tbB.risk) : 0.0;

            SCString tbLog;
            tbLog.Format(
                "[TB-SHADOW] pat=%d%s tier=%d long=%d entry=%.2f | ENGINE stop=%.2f tgt=%.2f maxBars=%d rr=%.2f%s%s"
                " || LIVE stop=%.2f tgt=%.2f | dStop=%.2f dTgt=%.2f",
                tbRawPattern, (tbValid ? "" : "!INVALID->LOW"), static_cast<int>(tbTier), tbLong ? 1 : 0,
                tbIn.entry, tbB.stop, tbB.target, tbB.max_bars, tbRR,
                tbB.structural_stop_bound ? " capS" : "", tbB.structural_target_bound ? " capT" : "",
                m_openTrade.GetStop(), m_openTrade.GetTarget(),
                tbB.stop - m_openTrade.GetStop(), tbB.target - m_openTrade.GetTarget());
            Logger::getInstance().log(tbLog.GetChars());

            // Latch the immutable bracket (fixes stop/target/maxBars at entry) so the
            // per-tick vertical/time barrier in Update() uses entry-regime maxBars
            // (train/live parity). Non-destructive: only the time barrier reads it;
            // stop/target are still driven by the SC bracket in this phase.
            TripleBarrierExitManager::getInstance().OpenBracket(order.InternalOrderID, sc.Index, tbIn);
        } catch (const std::exception& tbEx) {
            Logger::getInstance().log(std::string("[TB-SHADOW] exception: ") + tbEx.what());
        }

        if (m_requestQueue) {
        	m_requestQueue->push({TradeRequest::Type::ENTER_LONG, m_openTrade.CreateTradeRequestFlatBuffer()});
        }
    } else if (!wasFlat && !isNowFlat) {
        // Position size changed but still open - could be scale-in or scale-out
        float oldQty = m_openTrade.GetSize();
        float newQty = fabs(pos.PositionQuantity);

        if (newQty > oldQty) {
            // Scale-in: adding to position
            m_openTrade.ScaleIn(pos.AveragePrice, newQty);
        } else if (newQty < oldQty) {
            // Partial target fill — update tracked size only. Single-stage first-touch
            // has no trailing activation; the remaining size keeps the same static
            // stop/target bracket (Phase 1 cutover).
            m_openTrade.ScaleIn(pos.AveragePrice, newQty);  // ScaleIn handles size updates
        }
    } else if (!wasFlat && isNowFlat) {
        // === TCA: LOG ORDER FILL (Step 1.7) ===
        AIConnectionMonitor::Instance().LogOrderFill(sc, latestFill.InternalOrderID, latestFill.FillPrice);

        m_openTrade.Close(sc, latestFill.FillPrice, latestFill);
        m_pendingEntryOrder = {};
        ClearIntentTicket(ReasonCode::IntentClosed);

        // Triple-Barrier: release the latched bracket on position close.
        TripleBarrierExitManager::getInstance().Close();

        // Reset grade tracking for next trade
        m_lastTradeGradeAction = 0;

        // Update risk tracking with final P&L
        double finalPnL = m_openTrade.GetRealizedPnL();
        RiskManager::Instance().OnTradeClose(sc, finalPnL);

        // === ELITE GAP 2: EXIT FILL FEEDBACK TO GUI ===
        PublishSnapshot(sc);  // Show FLAT status with realized P&L

        // === ELITE GAP 6: EXIT TCA METRICS ===
        // Event publishing via TransportStream + EventSerializer (FlatBuffers)

        const double close_mae_ticks = m_openTrade.GetMAETicks();
        const double close_mfe_ticks = m_openTrade.GetMFETicks();
        const std::vector<uint8_t> exit_position_fb = m_openTrade.CreateTradeCloseFlatBuffer();

        if (m_requestQueue) {
            m_requestQueue->push({TradeRequest::Type::EXIT_POSITION, exit_position_fb});
        }

        {
            SCString logMsg;
            logMsg.Format("PositionManager: EXIT_POSITION sent (order_id=%d, mae_ticks=%.2f, mfe_ticks=%.2f)",
                m_openTrade.GetParentOrderId(), close_mae_ticks, close_mfe_ticks);
            Logger::getInstance().log(logMsg.GetChars());
        }

        // Store closed trade before reset (for analytics, backtesting, GUI)
        m_lastClosedTrade = m_openTrade;

        m_openTrade.Reset(sc);
    }
    m_lastFillArraySize = currentFillArraySize;
}

void PositionManager::ManageWorkingEntryOrder(SCStudyInterfaceRef sc) {
    if (!m_pendingEntryOrder.active || !IsFlat()) {
        return;
    }

    s_SCTradeOrder order;
    if (DetectSierraLookupFault(sc.GetOrderByOrderID(m_pendingEntryOrder.orderId, order))) {
        m_pendingEntryOrder = {};
        ClearIntentTicket(ReasonCode::ScDisconnected);
        return;
    }

    if (order.OrderStatusCode != SCT_OSC_OPEN) {
        m_pendingEntryOrder = {};
        ClearIntentTicket(ReasonCode::IntentCancelled);
        return;
    }

    const SCDateTime now = sc.CurrentSystemDateTime;
    const uint64_t nowUs = WallClockNowUs();

    // Stamp the moment the order first becomes working; independent of submit time.
    if (m_pendingEntryOrder.workingOrderActiveUs == 0) {
        m_pendingEntryOrder.workingOrderActiveUs = nowUs;
    }

    if (IsIntentTicketExpired(nowUs)) {
        DetectBrokerCancelFault(sc.CancelOrder(m_pendingEntryOrder.orderId));
        EmitGateEventTelemetry(ReasonCode::IntentExpired, m_intentTicket.action);
        m_pendingEntryOrder = {};
        ClearIntentTicket(ReasonCode::IntentExpired);
        return;
    }

    // Independent working-order TTL: separate from intent TTL; starts at ORDER_WORKING.
    // Prevents zombie working orders that survive even if the intent ticket is not yet expired.
    {
        const uint64_t workingTtlUs = static_cast<uint64_t>(RiskManager::Instance().GetWorkingOrderTtlMs()) * 1000ULL;
        if (m_pendingEntryOrder.workingOrderActiveUs > 0 &&
            (nowUs - m_pendingEntryOrder.workingOrderActiveUs) > workingTtlUs) {
            DetectBrokerCancelFault(sc.CancelOrder(m_pendingEntryOrder.orderId));
            EmitGateEventTelemetry(ReasonCode::WorkingOrderTtlExceeded, m_intentTicket.action);
            m_pendingEntryOrder = {};
            ClearIntentTicket(ReasonCode::WorkingOrderTtlExceeded);
            return;
        }
    }

    // === GAP 21: HMM TRANSITION RISK → WORKING ORDER EARLY CANCEL (Taleb — regime instability) ===
    // TransitionRisk measures the probability of an imminent regime change.
    // A prediction made in GAUSSIAN_STABLE is worthless if the regime is about to flip
    // to TALEBIAN_FRAGILE while the order is working.  Don't wait for TTL.
    if (InferenceManager::Instance().IsHighTransitionRisk()) {
        DetectBrokerCancelFault(sc.CancelOrder(m_pendingEntryOrder.orderId));
        EmitGateEventTelemetry(ReasonCode::ClimateShiftExit, m_intentTicket.action);
        m_pendingEntryOrder = {};
        ClearIntentTicket(ReasonCode::ClimateShiftExit);
        return;
    }

    // === GAP 23: FISHER INFORMATION → TTL TUNING (Shannon — estimate sharpness) ===
    // fisherInfo measures how sharply the HMM regime estimate is localized.
    // High Fisher = regime is clear; if you're not filled fast, the market disagrees.
    // Low Fisher = regime boundary is fuzzy; give the order more runway.
    float fisherTtlScale = 1.0f;
    {
        const auto& lrc = ContextManager::Instance().GetLocalRiskContext();
        if (lrc.isValid) {
            if (lrc.fisherInfo > 0.60f) {
                fisherTtlScale = 0.75f;   // Sharp estimate → tighten TTL by 25%
            } else if (lrc.fisherInfo < 0.20f) {
                fisherTtlScale = 1.50f;   // Fuzzy estimate → extend TTL by 50%
            }
        }
    }

    int executionBudgetMs = GetSessionExecutionBudgetMs();
    const float maxSpreadTicks = GetSessionSpreadLimit();

    // Policy-owned watchdog budget: caps working-order wait before ACK-timeout handling.
    executionBudgetMs = std::min(executionBudgetMs, RiskManager::Instance().GetOrderAckTimeoutMs());

    // Gap 23: Apply Fisher information TTL scaling
    executionBudgetMs = static_cast<int>(static_cast<float>(executionBudgetMs) * fisherTtlScale);

    // === GAP 17: REGIME-ADAPTIVE CHASE TIME CAP (Elder — market structure awareness) ===
    // SC has no native chase time limit field; enforce via executionBudgetMs cap.
    // In fast regimes, a stale chase walks into adverse flow — cap the budget.
    if (auto* climateInd = InferenceManager::Instance().MarketClimate()) {
        executionBudgetMs = std::min(executionBudgetMs,
                                     InferenceManager::GetRegimeExecutionBudgetCap(climateInd->Value()));
    }

    const double ageMs = (now - m_pendingEntryOrder.submitTime).GetAsDouble() * 86400000.0;
    const bool ackTimedOut = (ageMs > static_cast<double>(executionBudgetMs));
    const bool injectAckTimeout = RiskManager::Instance().ShouldInjectOrderAckTimeout();
    if (ackTimedOut || injectAckTimeout) {
        const int cancelResult = sc.CancelOrder(m_pendingEntryOrder.orderId);
        if (DetectOrderAckTimeoutFault(true, cancelResult)) {
            m_pendingEntryOrder = {};
            ClearIntentTicket(ReasonCode::OrderAckTimeout);
            return;
        }
    }

    const double bid = static_cast<double>(sc.Bid);
    const double ask = static_cast<double>(sc.Ask);
    const bool hasInside = (ask > bid) && (sc.TickSize > 0.0f);
    if (!hasInside) {
        Logger::getInstance().log(
            "PositionManager::ManagePendingEntry: skipped — invalid market quote "
            "(bid=" + std::to_string(bid) + " ask=" + std::to_string(ask) + ")");
        return;
    }

    const double spreadTicks = (ask - bid) / static_cast<double>(sc.TickSize);
    if (spreadTicks > static_cast<double>(maxSpreadTicks)) {
        DetectBrokerCancelFault(sc.CancelOrder(m_pendingEntryOrder.orderId));
        m_pendingEntryOrder = {};
        ClearIntentTicket(ReasonCode::SpreadBreach);
        return;
    }

    // Toxic flow gate: read from VolumeIndicator (single source of truth, TS3 15-min context).
    const auto* volInd = IndicatorManager::Instance().GetIndicator<VolumeIndicator>(IndicatorKey::VOLUME_SIGNAL);
    const double imbalance = volInd ? static_cast<double>(volInd->GetVolumeImbalance()) : 0.0;
    if ((m_pendingEntryOrder.isLong && imbalance < -0.35) ||
        (!m_pendingEntryOrder.isLong && imbalance > 0.35)) {
        DetectBrokerCancelFault(sc.CancelOrder(m_pendingEntryOrder.orderId));
        m_pendingEntryOrder = {};
        ClearIntentTicket(ReasonCode::ToxicFlowBreach);
        return;
    }

    if (IsQuoteChurnBreached(sc, bid, ask)) {
        DetectBrokerCancelFault(sc.CancelOrder(m_pendingEntryOrder.orderId));
        m_pendingEntryOrder = {};
        ClearIntentTicket(ReasonCode::QuoteChurnBreach);
        return;
    }

    // === GAP 1: PREDICTION DIRECTION COHERENCE (Taleb — regime-triggered reinference) ===
    // After the reinfer_action_id atomic update, PredictionState may now point the
    // opposite direction from the working order.  Cancel immediately rather than let
    // a stale order fill into a regime that has already flipped.
    {
        const auto* pred = InferenceManager::Instance().Prediction();
        if (pred && InferenceManager::HasDirectionConflict(m_pendingEntryOrder.isLong, pred->Action())) {
            DetectBrokerCancelFault(sc.CancelOrder(m_pendingEntryOrder.orderId));
            EmitGateEventTelemetry(ReasonCode::PredictionDirectionBreach, m_intentTicket.action);
            m_pendingEntryOrder = {};
            ClearIntentTicket(ReasonCode::PredictionDirectionBreach);
            return;
        }
    }

    // === GAP 2: PREDICTION STALENESS DECAY (Semantic + Shannon backstop) ===
    // Primary: event-count semantic freshness (correct for event-driven architecture).
    // Backstop: wall-clock Shannon decay (catches stalled pipeline / hung inference).
    {
        const auto* pred = InferenceManager::Instance().Prediction();
        const double semanticFreshness = pred ? pred->SemanticFreshnessDiscount(
            IndicatorManager::Instance().GlobalSequenceId()) : 1.0;
        const double wallClockFreshness = pred ? pred->FreshnessDiscount(nowUs) : 1.0;
        // Take the minimum — if either dimension says the prediction is dead, cancel.
        const double effectiveFreshness = std::min(semanticFreshness, wallClockFreshness);
        if (effectiveFreshness <= 0.0) {
            DetectBrokerCancelFault(sc.CancelOrder(m_pendingEntryOrder.orderId));
            EmitGateEventTelemetry(ReasonCode::PredictionStaleDecay, m_intentTicket.action);
            m_pendingEntryOrder = {};
            ClearIntentTicket(ReasonCode::PredictionStaleDecay);
            return;
        }
    }

    // NOTE: Repricing is now handled natively by SC Limit Chase order engine at zero latency.
    // ManageWorkingEntryOrder retains monitoring-only responsibility: spread, toxic flow, quote churn,
    // intent expiry, and working-order TTL cancellation are all still enforced above.
}

void PositionManager::UpdateAttachedOrders(SCStudyInterfaceRef sc) {
    if (IsFlat()) return;
    int stopId = 0, targetId = 0;
    sc.GetAttachedOrderIDsForParentOrder(m_openTrade.GetParentOrderId(), targetId, stopId);
    m_openTrade.SetAttachedOrderIds(stopId, targetId);

    s_SCTradeOrder attachedOrder;
    if (stopId != 0 && sc.GetOrderByOrderID(stopId, attachedOrder) != SCTRADING_ORDER_ERROR) {
        m_openTrade.SetStop(attachedOrder.Price1);
    }
    if (targetId != 0 && sc.GetOrderByOrderID(targetId, attachedOrder) != SCTRADING_ORDER_ERROR) {
        m_openTrade.SetTarget(attachedOrder.Price1);
    }
}

void PositionManager::HandleReplies() {
    // Elite v2.4: Handle FlatBuffer trade responses from Python trade_server
    // Correlates trade confirmations with open position for Firestore doc ID linking

    if (!m_replyQueue) return;

    TradeReply reply;
    while (m_replyQueue->try_pop(reply)) {
        // Handle TradeResponse: Entry confirmation with firestore_doc_id
        if (reply.flatbuffer_data.empty()) {
            continue;
        }

        // Deserialize TradeResponse from FlatBuffer (zero-copy)
        const MTS::Schema::TradeResponse* trade_response =
            flatbuffers::GetRoot<MTS::Schema::TradeResponse>(reply.flatbuffer_data.data());

        if (!trade_response) {
            Logger::getInstance().log("PositionManager::HandleReplies - Failed to deserialize TradeResponse FlatBuffer");
            continue;
        }

        // Extract fields from FlatBuffer
        uint32_t response_order_id = trade_response->order_id();
        auto firestore_doc_id_fb = trade_response->firestore_doc_id();
        auto error_message_fb = trade_response->error_message();

        // Correlate reply with open trade using order_id
        if (!IsFlat() && static_cast<uint32_t>(m_openTrade.GetParentOrderId()) == response_order_id) {
            // Response matches open trade - process based on type
            if (reply.type == TradeReply::Type::FILLED) {
                // Python successfully submitted order to Interactive Brokers
                if (firestore_doc_id_fb) {
                    m_openTrade.SetFirestoreDocId(firestore_doc_id_fb->str());
                    Logger::getInstance().log("PositionManager: Trade FILLED - Firestore doc ID stored for correlation");
                }
            } else if (reply.type == TradeReply::Type::REJECTED) {
                // Python rejected the trade request (validation failed)
                std::string reason = error_message_fb ? error_message_fb->str() : "Unknown";
                Logger::getInstance().log("PositionManager: Trade REJECTED - " + reason);
            } else if (reply.type == TradeReply::Type::FAILURE) {
                // IB submission failed
                std::string reason = error_message_fb ? error_message_fb->str() : "IB submission failed";
                Logger::getInstance().log("PositionManager: Trade FAILURE - " + reason);
            }
        }
    }
}

void PositionManager::PublishSnapshot(SCStudyInterfaceRef sc) {
    // SAFETY: Only attempt to publish if TransportStream is running
    try {
        // Send PositionUpdate FlatBuffer to Python GUI
        const uint8_t* positionUpdateBuffer = nullptr;
        size_t positionUpdateSize = 0;
        if (CreatePositionUpdateFlatBuffer(sc, positionUpdateBuffer, positionUpdateSize) &&
            positionUpdateBuffer && positionUpdateSize > 0) {
            TransportStream::Instance().Emit(positionUpdateBuffer, positionUpdateSize);
        }
    } catch (const std::exception& e) {
        Logger::getInstance().log("ERROR: PublishSnapshot - " + std::string(e.what()));
    } catch (...) {
        Logger::getInstance().log("ERROR: PublishSnapshot - Unknown exception");
    }
}

// === ELITE REFINEMENT 2: POSITION STATE SYNC ON RECONNECT ===
void PositionManager::PublishPositionSync(SCStudyInterfaceRef sc) {
    // Send explicit position state to GUI on reconnect
    // This ensures GUI knows current position after EmergencyFlattenPosition during disconnect

    // Elite v2.4: Send PositionUpdate FlatBuffer to Python GUI
    const uint8_t* positionUpdateBuffer = nullptr;
    size_t positionUpdateSize = 0;
    if (CreatePositionUpdateFlatBuffer(sc, positionUpdateBuffer, positionUpdateSize) &&
        positionUpdateBuffer && positionUpdateSize > 0) {
        TransportStream::Instance().Emit(positionUpdateBuffer, positionUpdateSize);
        Logger::getInstance().log("Position sync sent to GUI on reconnect");
    }
}

// Publish() deprecated - use PublishSnapshot() for FlatBuffer-based PositionUpdate


// === ELITE v2.4: PositionUpdate FlatBuffer Serialization ===
bool PositionManager::CreatePositionUpdateFlatBuffer(
    SCStudyInterfaceRef sc,
    const uint8_t*& outBuffer,
    size_t& outSize
) const {
    outBuffer = nullptr;
    outSize = 0;
    m_positionUpdateBuilder.Clear();

    // Get current position state
    s_SCPositionData pos;
    sc.GetTradePosition(pos);

    const bool isFlat = (pos.PositionQuantity == 0);
    const bool isLong = (pos.PositionQuantity > 0);

    // Build strings - safe even if Trade is uninitialized (returns empty string)
    auto pattern_offset = m_positionUpdateBuilder.CreateString(m_openTrade.GetPatternName());

    // Calculate MAE/MFE in ticks (instrument-agnostic via sc.TickSize)
    // SAFETY: Only calculate if position is actually open
    float mae_ticks = 0.0f;
    float mfe_ticks = 0.0f;

    if (!isFlat && m_openTrade.GetStatus() == TradeStatusEnum::OPEN && sc.TickSize > 0.0f) {
        const float tick = static_cast<float>(sc.TickSize);
        float entry_price = static_cast<float>(m_openTrade.GetEntryPrice());

        // MAE: Maximum Adverse Excursion
        mae_ticks = isLong
            ? static_cast<float>((entry_price - m_openTrade.GetEntryLow()) / tick)
            : static_cast<float>((m_openTrade.GetEntryHigh() - entry_price) / tick);

        // MFE: Maximum Favorable Excursion
        mfe_ticks = isLong
            ? static_cast<float>((m_openTrade.GetHighestPrice() - entry_price) / tick)
            : static_cast<float>((entry_price - m_openTrade.GetLowestPrice()) / tick);
    }

    // Build PositionUpdate table and wrap it in the canonical envelope.
    try {
        MTS::Schema::PositionUpdateBuilder pos_builder(m_positionUpdateBuilder);

        // Add fields in reverse order of definition (FlatBuffers requirement)
        pos_builder.add_side(isFlat ? 0 : (isLong ? 1 : -1));
        pos_builder.add_pattern(pattern_offset);
        pos_builder.add_daily_pnl(static_cast<float>(pos.DailyProfitLoss));
        pos_builder.add_open_pnl(static_cast<float>(pos.OpenProfitLoss));
        pos_builder.add_confidence(isFlat ? 0.0f : m_openTrade.GetConfidence());
        pos_builder.add_mfe_ticks(mfe_ticks);
        pos_builder.add_mae_ticks(mae_ticks);
        pos_builder.add_unrealized_pnl(static_cast<float>(pos.OpenProfitLoss));

        // SAFETY: Check array bounds for sc.Close
        const float current_price = (sc.Index >= 0) ? static_cast<float>(sc.Close[sc.Index]) : 0.0f;
        pos_builder.add_current_price(current_price);

        // SAFETY: Only use Trade prices if position is open
        const float target = !isFlat ? static_cast<float>(m_openTrade.GetTarget()) : 0.0f;
        const float stop = !isFlat ? static_cast<float>(m_openTrade.GetStop()) : 0.0f;
        const float entry = !isFlat ? static_cast<float>(m_openTrade.GetEntryPrice()) : 0.0f;
        pos_builder.add_target_price(target);
        pos_builder.add_stop_price(stop);
        pos_builder.add_entry_price(entry);

        // SAFETY: Calculate bars_held only if Trade is open and entry_index is valid
        uint32_t bars_held = 0;
        if (!isFlat && m_openTrade.GetEntryIndex() >= 0) {
            bars_held = static_cast<uint32_t>(sc.Index - m_openTrade.GetEntryIndex());
        }
        pos_builder.add_bars_held(bars_held);
        pos_builder.add_quantity(static_cast<uint32_t>(fabs(pos.PositionQuantity)));
        pos_builder.add_order_id(isFlat ? 0 : static_cast<uint32_t>(m_openTrade.GetParentOrderId()));
        pos_builder.add_timestamp_us(sc.GetCurrentDateTime().ToUNIXTimeInMicroseconds());

        auto pos_update = pos_builder.Finish();
        auto envelope = MTS::Schema::Contract::BuildEnvelope(
            m_positionUpdateBuilder,
            MTS::Schema::Contract::kEnvelopePositionUpdate,
            pos_update.Union()
        );
        m_positionUpdateBuilder.Finish(envelope);

        outBuffer = m_positionUpdateBuilder.GetBufferPointer();
        outSize = m_positionUpdateBuilder.GetSize();
        return outBuffer != nullptr && outSize > 0;
    } catch (const std::exception& e) {
        Logger::getInstance().log("ERROR: CreatePositionUpdateFlatBuffer - " + std::string(e.what()));
        return false;
    } catch (...) {
        Logger::getInstance().log("ERROR: CreatePositionUpdateFlatBuffer - Unknown exception");
        return false;
    }
}

void PositionManager::CachePreviousState(SCStudyInterfaceRef sc) {
    m_prevState.status = m_openTrade.GetStatus();
    if (!IsFlat()) {
        m_prevState.last = sc.Close[sc.Index];
        m_prevState.stop = m_openTrade.GetStop();
        m_prevState.target = m_openTrade.GetTarget();
        m_prevState.trade_grade = m_openTrade.GetTradeGrade();
    }
}

bool PositionManager::IsDirty(SCStudyInterfaceRef sc) const {
    if (m_openTrade.GetStatus() != m_prevState.status) return true;
    if (IsFlat()) return false;

    return std::fabs(sc.Close[sc.Index] - m_prevState.last) > 1.0 ||
           m_openTrade.GetStop() != m_prevState.stop ||
           m_openTrade.GetTarget() != m_prevState.target ||
           m_openTrade.GetTradeGrade() != m_prevState.trade_grade;
}

TradeSideEnum PositionManager::GetTradeSide() const {
    return m_openTrade.GetSide();
}

const Trade& PositionManager::GetTrade() const {
    return m_openTrade;
}

bool PositionManager::IsFlat() const {
    auto status = m_openTrade.GetStatus();
    return status == TradeStatusEnum::NO_TRADE || status == TradeStatusEnum::CLOSE;
}

bool PositionManager::IsLong() const { return m_openTrade.GetSide() == TradeSideEnum::LONG; }

bool PositionManager::IsShort() const { return m_openTrade.GetSide() == TradeSideEnum::SHORT; }

const char* PositionManager::ToReasonCodeString(ReasonCode reasonCode) {
    switch (reasonCode) {
        case ReasonCode::ContextStale: return "RC_CONTEXT_STALE";
        case ReasonCode::EntryDisabledGlobal: return "RC_ENTRY_DISABLED_GLOBAL";
        case ReasonCode::HardSafetyViolation: return "RC_HARD_SAFETY_VIOLATION";
        case ReasonCode::IbDisconnected: return "RC_IB_DISCONNECTED";
        case ReasonCode::IntentCancelled: return "RC_INTENT_CANCELLED";
        case ReasonCode::IntentClosed: return "RC_INTENT_CLOSED";
        case ReasonCode::IntentExpired: return "RC_INTENT_EXPIRED";
        case ReasonCode::IntentFilled: return "RC_INTENT_FILLED";
        case ReasonCode::IntentSuperseded: return "RC_INTENT_SUPERSEDED";
        case ReasonCode::ModelNotReady: return "RC_MODEL_NOT_READY";
        case ReasonCode::OrderAckTimeout: return "RC_ORDER_ACK_TIMEOUT";
        case ReasonCode::PartialRemainderCancelled: return "RC_PARTIAL_REMAINDER_CANCELLED";
        case ReasonCode::PatternInvalidated: return "RC_PATTERN_INVALIDATED";
        case ReasonCode::PatternQualityFail: return "RC_PATTERN_QUALITY_FAIL";
        case ReasonCode::PolicyEvalErrorFailClosed: return "RC_POLICY_EVAL_ERROR_FAIL_CLOSED";
        case ReasonCode::PredictionStale: return "RC_PREDICTION_STALE";
        case ReasonCode::RepriceBudgetExceeded: return "RC_REPRICE_BUDGET_EXCEEDED";
        case ReasonCode::RiskDailyLimit: return "RC_RISK_DAILY_LIMIT";
        case ReasonCode::ScDisconnected: return "RC_SC_DISCONNECTED";
        case ReasonCode::SpreadBreach: return "RC_SPREAD_BREACH";
        case ReasonCode::ToxicFlowBreach: return "RC_TOXIC_FLOW_BREACH";
        case ReasonCode::QuoteChurnBreach: return "RC_QUOTE_CHURN_BREACH";
        case ReasonCode::WorkingOrderTtlExceeded: return "RC_WORKING_ORDER_TTL_EXCEEDED";
        case ReasonCode::EntropyBreach: return "RC_ENTROPY_BREACH";
        case ReasonCode::PredictionDirectionBreach: return "RC_PREDICTION_DIRECTION_BREACH";
        case ReasonCode::PredictionStaleDecay: return "RC_PREDICTION_STALE_DECAY";
        case ReasonCode::ClimateShiftExit: return "RC_CLIMATE_SHIFT_EXIT";
        case ReasonCode::HostileRegimeExit: return "RC_HOSTILE_REGIME_EXIT";
        case ReasonCode::ClusteringBreach: return "RC_CLUSTERING_BREACH";
        case ReasonCode::NA:
        default:
            return "N/A";
    }
}

bool PositionManager::IsQuoteChurnBreached(SCStudyInterfaceRef sc, double bid, double ask) {
    const uint64_t nowUs = WallClockNowUs();
    if (m_prevQuoteSampleUs == 0) {
        m_prevQuoteBid = bid;
        m_prevQuoteAsk = ask;
        m_prevQuoteSampleUs = nowUs;
        return false;
    }

    const bool quoteChanged =
        (std::fabs(bid - m_prevQuoteBid) > 1e-9) ||
        (std::fabs(ask - m_prevQuoteAsk) > 1e-9);
    const uint64_t deltaUs = (nowUs > m_prevQuoteSampleUs) ? (nowUs - m_prevQuoteSampleUs) : 1ULL;
    const double instantaneousPerSec = quoteChanged
        ? std::min(1000.0, 1000000.0 / static_cast<double>(deltaUs))
        : 0.0;

    // EMA smooths bursty tick-to-tick updates while preserving sustained churn signals.
    constexpr double kEmaAlpha = 0.2;
    m_quoteChurnEmaPerSec = (kEmaAlpha * instantaneousPerSec) + ((1.0 - kEmaAlpha) * m_quoteChurnEmaPerSec);

    m_prevQuoteBid = bid;
    m_prevQuoteAsk = ask;
    m_prevQuoteSampleUs = nowUs;

    const int currentMinutes = sc.CurrentSystemDateTime.GetHour() * 60 + sc.CurrentSystemDateTime.GetMinute();
    double maxQuoteChurnPerSec = 8.0;
    if (currentMinutes <= (9 * 60 + 45) || currentMinutes >= (15 * 60 + 45)) {
        maxQuoteChurnPerSec = 6.0;
    }
    return m_quoteChurnEmaPerSec > maxQuoteChurnPerSec;
}

// ── Session-Aware Microstructure Helpers ──
// Centralizes time-of-day → spread/budget mapping that was previously
// copy-pasted at 3 call sites (working-entry, prediction entry, manual entry).
// Reads TimeOfDayIndicator (updated by TS3 earlier in study chain) so the
// mapping is always consistent with the indicator's classification.

float PositionManager::GetSessionSpreadLimit() {
    const auto* tod = IndicatorManager::Instance().GetIndicator<TimeOfDayIndicator>(IndicatorKey::TIME_OF_DAY);
    if (!tod) return ENTRY_MAX_SPREAD_TICKS;

    switch (tod->Value()) {
        case TimeOfDayEnum::PRE_MARKET_HOOK:   // 08:30–09:00 ET
        case TimeOfDayEnum::PRE_MARKET:        // 09:00–09:30 ET
        case TimeOfDayEnum::OPENING_HOUR:      // 09:30–10:30 ET (first 15 min widens)
            return 1.5f;                       // Opening: wider spreads tolerated

        case TimeOfDayEnum::FINAL_HOUR:        // 15:00–15:45 ET
        case TimeOfDayEnum::PM_RUN_ENTRY:      // 15:45–16:00 ET
        case TimeOfDayEnum::AFTER_HOURS:       // 16:00–18:00 ET
            return 1.0f;                       // Close: tighter spread required

        default:
            return ENTRY_MAX_SPREAD_TICKS;     // Core session: 2.0 ticks
    }
}

int PositionManager::GetSessionExecutionBudgetMs() {
    const auto* tod = IndicatorManager::Instance().GetIndicator<TimeOfDayIndicator>(IndicatorKey::TIME_OF_DAY);
    if (!tod) return ENTRY_EXECUTION_BUDGET_MS;

    switch (tod->Value()) {
        case TimeOfDayEnum::PRE_MARKET_HOOK:
        case TimeOfDayEnum::PRE_MARKET:
        case TimeOfDayEnum::OPENING_HOUR:
            return 900;                        // Opening: wider budget (volatile fills)

        case TimeOfDayEnum::FINAL_HOUR:
        case TimeOfDayEnum::PM_RUN_ENTRY:
        case TimeOfDayEnum::AFTER_HOURS:
            return 800;                        // Close: tighter budget

        default:
            return ENTRY_EXECUTION_BUDGET_MS;  // Core session: 1200 ms
    }
}

void PositionManager::LogOrderFailure(
    const char* source,
    TradeActionEnum action,
    ReasonCode reasonCode,
    const char* stage,
    const std::string& detail,
    uint64_t nowUs,
    uint64_t contextSnapshotUs,
    const ExecutionGate::GateContext* gateCtx
) const {
    const char* reason = ToReasonCodeString(reasonCode);
    const uint64_t contextAgeUs =
        (contextSnapshotUs > 0 && nowUs > contextSnapshotUs)
            ? (nowUs - contextSnapshotUs)
            : 0ULL;

    std::string message =
        std::string("ORDER_FAIL|source=") + (source ? source : "UNKNOWN") +
        "|stage=" + (stage ? stage : "UNSPECIFIED") +
        "|reason_code=" + reason +
        "|reason_id=" + std::to_string(static_cast<int>(reasonCode)) +
        "|action_id=" + std::to_string(static_cast<int>(action)) +
        "|intent_active=" + std::to_string(m_intentTicket.active ? 1 : 0) +
        "|intent_id=" + std::to_string(m_intentTicket.intentId) +
        "|sequence_id=" + std::to_string(m_intentTicket.sequenceId) +
        "|context_snapshot_us=" + std::to_string(contextSnapshotUs) +
        "|context_age_us=" + std::to_string(contextAgeUs);

    if (gateCtx) {
        message +=
            "|gate_model_ready=" + std::to_string(gateCtx->modelReady ? 1 : 0) +
            "|gate_prediction_fresh=" + std::to_string(gateCtx->predictionFresh ? 1 : 0) +
            "|gate_context_fresh=" + std::to_string(gateCtx->contextFresh ? 1 : 0) +
            "|gate_allow_new_entries=" + std::to_string(gateCtx->allowNewEntries ? 1 : 0) +
            "|gate_hard_gates_pass=" + std::to_string(gateCtx->hardGatesPass ? 1 : 0) +
            "|gate_is_flat=" + std::to_string(gateCtx->isFlat ? 1 : 0) +
            "|gate_has_pending_entry=" + std::to_string(gateCtx->hasPendingEntryOrder ? 1 : 0) +
            "|gate_trading_halted=" + std::to_string(gateCtx->tradingHalted ? 1 : 0);
    }

    if (!detail.empty()) {
        message += "|detail=" + detail;
    }

    Logger::getInstance().log(message);
}

void PositionManager::EmitGateEventTelemetry(ReasonCode reasonCode, TradeActionEnum action) {
    if (reasonCode == ReasonCode::NA) {
        return;
    }

    const uint64_t nowUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()
        ).count()
    );

    if (reasonCode == m_lastGateEventCode && (nowUs - m_lastGateEventEmitUs) < GATE_EVENT_MIN_EMIT_INTERVAL_US) {
        return;
    }

    m_lastGateEventCode = reasonCode;
    m_lastGateEventEmitUs = nowUs;
    const char* reason = ToReasonCodeString(reasonCode);
    const int reasonId = static_cast<int>(reasonCode);

    const std::string message =
        std::string("GATE_EVENT|directive=DENY|reason_code=") + reason +
        "|reason_id=" + std::to_string(reasonId) +
        "|action_id=" + std::to_string(static_cast<int>(action));

    auto diagnostic = MTS::EliteFlatBufferHelper::BuildDiagnosticWithGateEvent(
        "PositionManager",
        message,
        MTS::Schema::GateDirective_DENY,
        ToSchemaReasonCode(reasonCode),
        static_cast<int8_t>(action),
        0.0f,
        0.0f);
    TransportStream::Instance().Emit(
        static_cast<const uint8_t*>(diagnostic.data()),
        diagnostic.size()
    );
}

void PositionManager::MarkConnectivityFault(ReasonCode reasonCode, TradeActionEnum action) {
    if (reasonCode == ReasonCode::NA) {
        return;
    }

    const uint64_t holdUs = RiskManager::Instance().GetConnectivityFailClosedHoldUs();
    m_connectivityDownUntilUs = WallClockNowUs() + ((holdUs > 0) ? holdUs : CONNECTIVITY_FAIL_CLOSED_HOLD_US);
    m_connectivityDownReason = reasonCode;
    m_connectivityRecoveryPending = true;
    EmitGateEventTelemetry(reasonCode, action);
}

void PositionManager::EmitConnectivityRecoveryEvent() {
    const char* priorReason =
        (m_connectivityDownReason == ReasonCode::NA)
            ? "UNKNOWN"
            : ToReasonCodeString(m_connectivityDownReason);

    const std::string message =
        std::string("RECOVERY_EVENT|state=RECOVERY_OK|prior_reason=") +
        priorReason;

    auto diagnostic = MTS::EliteFlatBufferHelper::BuildDiagnosticWithRecoveryEvent(
        "PositionManager",
        message,
        MTS::Schema::RecoveryState_RECOVERY_OK,
        ToSchemaReasonCode(m_connectivityDownReason),
        0.0f,
        0.0f);
    TransportStream::Instance().Emit(
        static_cast<const uint8_t*>(diagnostic.data()),
        diagnostic.size()
    );
}

bool PositionManager::HasProtectiveCoverage(SCStudyInterfaceRef sc) const {
    if (IsFlat()) {
        return true;
    }

    const int parentOrderId = m_openTrade.GetParentOrderId();
    if (parentOrderId <= 0) {
        return false;
    }

    int stopId = 0;
    int targetId = 0;
    sc.GetAttachedOrderIDsForParentOrder(parentOrderId, targetId, stopId);
    if (stopId == 0) {
        return false;
    }

    s_SCTradeOrder stopOrder;
    if (sc.GetOrderByOrderID(stopId, stopOrder) == SCTRADING_ORDER_ERROR) {
        return false;
    }

    return stopOrder.OrderStatusCode == SCT_OSC_OPEN;
}

bool PositionManager::IsRestartReconciliationReady(SCStudyInterfaceRef sc) const {
    if (IsFlat()) {
        return true;
    }

    if (!HasProtectiveCoverage(sc)) {
        return false;
    }

    return !m_openTrade.GetFirestoreDocId().empty();
}

bool PositionManager::IsConnectivityRecoveryReady(SCStudyInterfaceRef sc) const {
    if (!SystemOrchestrator::Instance().IsHandshakeComplete()) {
        return false;
    }
    if (!TradeExecutionServer::Instance().IsNewEntriesAllowed()) {
        return false;
    }
    if (RiskManager::Instance().IsTradingHalted(sc)) {
        return false;
    }
    if (!IsRestartReconciliationReady(sc)) {
        return false;
    }
    return true;
}

bool PositionManager::IsConnectivityFailClosed(SCStudyInterfaceRef sc, TradeActionEnum action) {
    const uint64_t nowUs = WallClockNowUs();
    if (nowUs < m_connectivityDownUntilUs) {
        const ReasonCode reason =
            (m_connectivityDownReason == ReasonCode::NA) ? ReasonCode::ScDisconnected : m_connectivityDownReason;
        EmitGateEventTelemetry(reason, action);
        return true;
    }

    if (!m_connectivityRecoveryPending) {
        return false;
    }

    if (!IsConnectivityRecoveryReady(sc)) {
        const ReasonCode reason =
            (m_connectivityDownReason == ReasonCode::NA) ? ReasonCode::ScDisconnected : m_connectivityDownReason;
        EmitGateEventTelemetry(reason, action);
        return true;
    }

    EmitConnectivityRecoveryEvent();
    m_connectivityRecoveryPending = false;
    m_connectivityDownUntilUs = 0;
    m_connectivityDownReason = ReasonCode::NA;
    return false;
}

TradeActionEnum PositionManager::PendingEntryAction() const {
    return m_pendingEntryOrder.isLong ? TradeActionEnum::ENTER_LONG : TradeActionEnum::ENTER_SHORT;
}

bool PositionManager::DetectSierraLookupFault(int orderLookupResult) {
    if (orderLookupResult != SCTRADING_ORDER_ERROR && !RiskManager::Instance().ShouldInjectScDisconnect()) {
        return false;
    }

    MarkConnectivityFault(ReasonCode::ScDisconnected, PendingEntryAction());
    return true;
}

bool PositionManager::DetectBrokerCancelFault(int cancelResult) {
    if (cancelResult > 0) {
        return false;
    }

    MarkConnectivityFault(ReasonCode::IbDisconnected, PendingEntryAction());
    return true;
}

bool PositionManager::DetectOrderAckTimeoutFault(bool timedOut, int cancelResult) {
    if (!timedOut && !RiskManager::Instance().ShouldInjectOrderAckTimeout()) {
        return false;
    }

    MarkConnectivityFault(ReasonCode::OrderAckTimeout, PendingEntryAction());
    DetectBrokerCancelFault(cancelResult);
    return true;
}

bool PositionManager::DetectBrokerSubmitFault(int orderResult, TradeActionEnum action) {
    if (orderResult > 0 && !RiskManager::Instance().ShouldInjectIbDisconnect()) {
        return false;
    }

    MarkConnectivityFault(ReasonCode::IbDisconnected, action);
    return true;
}

void PositionManager::TransitionIntentTicket(const char* nextState, ReasonCode reasonCode) {
    if (!m_intentTicket.active || nextState == nullptr || nextState[0] == '\0') {
        return;
    }

    const std::string fromState = m_intentTicket.state;
    m_intentTicket.state = nextState;

    // Keep hot-path logs quiet: only emit high-signal terminal transitions.
    const bool terminalTransition =
        (std::strcmp(nextState, "SUPERSEDED") == 0) ||
        (std::strcmp(nextState, "REJECTED") == 0) ||
        (std::strcmp(nextState, "EXECUTED") == 0) ||
        (std::strcmp(nextState, "CLOSED") == 0);
    if (!terminalTransition) {
        return;
    }

    // Avoid duplicate chatter: REJECTED/EXECUTED/SUPERSEDED already fully describe
    // closure, so do not emit a second CLOSED log immediately afterward.
    if (std::strcmp(nextState, "CLOSED") == 0 &&
        (fromState == "REJECTED" || fromState == "EXECUTED" || fromState == "SUPERSEDED")) {
        return;
    }

    SCString evt;
    evt.Format(
        "INTENT_EVENT|intent_id=%llu|from_state=%s|to_state=%s|reason_code=%s|reason_id=%d|action=%d|sequence_id=%llu|source=%s",
        static_cast<unsigned long long>(m_intentTicket.intentId),
        fromState.c_str(),
        m_intentTicket.state.c_str(),
        ToReasonCodeString(reasonCode),
        static_cast<int>(reasonCode),
        static_cast<int>(m_intentTicket.action),
        static_cast<unsigned long long>(m_intentTicket.sequenceId),
        m_intentTicket.source.c_str()
    );
    Logger::getInstance().log(evt.GetChars());

    auto diagnostic = MTS::EliteFlatBufferHelper::BuildDiagnosticWithIntentEvent(
        "PositionManager",
        evt.GetChars(),
        m_intentTicket.intentId,
        ToSchemaIntentState(fromState),
        ToSchemaIntentState(m_intentTicket.state),
        ToSchemaReasonCode(reasonCode),
        static_cast<int8_t>(m_intentTicket.action),
        m_intentTicket.sequenceId,
        m_intentTicket.source,
        0.0f,
        0.0f);
    TransportStream::Instance().Emit(
        static_cast<const uint8_t*>(diagnostic.data()),
        diagnostic.size());
}

void PositionManager::ClearIntentTicket(ReasonCode reasonCode) {
    if (m_intentTicket.active) {
        TransitionIntentTicket("CLOSED", reasonCode);
    }
    m_intentTicket = {};
}

bool PositionManager::BeginIntentTicket(
    SCStudyInterfaceRef sc,
    uint64_t intentId,
    uint64_t sequenceId,
    uint64_t createdUs,
    uint64_t ttlUs,
    TradeActionEnum action,
    const char* source
) {
    if (intentId == 0 || ttlUs == 0) {
        return false;
    }

    if (m_intentTicket.active) {
        if (m_intentTicket.intentId == intentId) {
            return false;
        }

        TransitionIntentTicket("SUPERSEDED", ReasonCode::IntentSuperseded);
        if (m_pendingEntryOrder.active && IsFlat()) {
            DetectBrokerCancelFault(sc.CancelOrder(m_pendingEntryOrder.orderId));
            m_pendingEntryOrder = {};
        }
        m_intentTicket = {};
    }

    m_intentTicket.active = true;
    m_intentTicket.intentId = intentId;
    m_intentTicket.sequenceId = sequenceId;
    m_intentTicket.createdUs = createdUs;
    m_intentTicket.validUntilUs = createdUs + ttlUs;
    m_intentTicket.action = action;
    m_intentTicket.source = source ? source : "UNKNOWN";
    m_intentTicket.state = "CREATED";
    TransitionIntentTicket("ACKED", ReasonCode::NA);
    return true;
}

void PositionManager::RejectIntentTicket(TradeActionEnum action, ReasonCode reasonCode, bool emitTelemetry) {
    if (emitTelemetry) {
        EmitGateEventTelemetry(reasonCode, action);
    }

    if (!m_intentTicket.active) {
        return;
    }

    const ReasonCode finalReason =
        (reasonCode == ReasonCode::NA) ? ReasonCode::PolicyEvalErrorFailClosed : reasonCode;
    EmitPendingPredictionAckRejected(finalReason);
    TransitionIntentTicket("REJECTED", finalReason);
    ClearIntentTicket(finalReason);
}

bool PositionManager::IsIntentTicketExpired(uint64_t nowUs) const {
    return m_intentTicket.active && m_intentTicket.validUntilUs > 0 && nowUs > m_intentTicket.validUntilUs;
}

void PositionManager::SetPredictionAckSink(PredictionAckSink sink) {
    m_predictionAckSink = std::move(sink);
}

void PositionManager::BeginPendingPredictionAck(
    const TradeExecutionServer::PredictionSlot& prediction,
    TradeActionEnum action) {
    m_pendingPredictionAck = {};
    m_pendingPredictionAck.active = true;
    m_pendingPredictionAck.event.accepted = false;
    m_pendingPredictionAck.event.action = action;
    m_pendingPredictionAck.event.confidence = prediction.confidence;
    m_pendingPredictionAck.event.patternName = prediction.patternName;
    m_pendingPredictionAck.event.sequenceId = prediction.sequenceId;
    m_pendingPredictionAck.event.timestampUs = prediction.timestampUs;
    m_pendingPredictionAck.event.inferenceLatencyUs = prediction.inferenceLatencyUs;
    m_pendingPredictionAck.event.transformerLatencyUs = prediction.transformerLatencyUs;
    m_pendingPredictionAck.event.regimeLatencyUs = prediction.regimeLatencyUs;
}

void PositionManager::EmitPendingPredictionAckAccepted(float entryPrice, float stopPrice, float targetPrice) {
    if (!m_pendingPredictionAck.active) {
        return;
    }

    m_pendingPredictionAck.event.accepted = true;
    m_pendingPredictionAck.event.reasonCode = ReasonCode::NA;
    m_pendingPredictionAck.event.entryPrice = entryPrice;
    m_pendingPredictionAck.event.stopPrice = stopPrice;
    m_pendingPredictionAck.event.targetPrice = targetPrice;

    if (m_predictionAckSink) {
        m_predictionAckSink(m_pendingPredictionAck.event);
    }
    m_pendingPredictionAck = {};
}

void PositionManager::EmitPendingPredictionAckRejected(ReasonCode reasonCode, const std::string& detail) {
    if (!m_pendingPredictionAck.active) {
        return;
    }

    m_pendingPredictionAck.event.accepted = false;
    m_pendingPredictionAck.event.reasonCode = reasonCode;
    m_pendingPredictionAck.event.reasonDetail = detail;

    if (m_predictionAckSink) {
        m_predictionAckSink(m_pendingPredictionAck.event);
    }
    m_pendingPredictionAck = {};
}

void PositionManager::ClearPendingPredictionAck() {
    m_pendingPredictionAck = {};
}

namespace {
// Triple-Barrier input builder (Phase 1 cutover, Step C). Single source of truth
// for the immutable first-touch bracket, shared by the prediction-path order
// submission and the HandleFills shadow/latch. Resolves tier/regime/dof/swing
// internally so callers only supply entry-time price context.
// See docs/ADR/triple_barrier_cutover_phase1_plan.md.
tbe::BarrierInputs BuildBarrierInputs(SCStudyInterfaceRef sc,
                                      int patternId,
                                      bool isLong,
                                      double entry,
                                      double structuralTarget,
                                      double atr10) {
    const bool valid = (patternId >= 1 &&
                        patternId < static_cast<int>(tbe::kPatternTable.size()));
    const int pat = valid ? patternId : 0;  // D2 fail-closed: safe LOW-tier default
    const tbe::Tier tier = tbe::kPatternTable[static_cast<std::size_t>(pat)].tier;

    const auto* hmm = InferenceManager::Instance().HmmState();
    const tbe::Regime regime = hmm
        ? TripleBarrierExitManager::ToRegime(hmm->Value())
        : tbe::Regime::GAUSSIAN_STABLE;
    const double dofScale = hmm ? hmm->DofStopScale() : 1.5;

    float swingHigh = 0.0f, swingLow = 0.0f;
    if (auto ima = IndicatorManager::Instance().GetIndicator<IntermediateMarketAction>(IndicatorKey::SHORT_MKT_ACTION)) {
        swingHigh = ima->swingHigh();
        swingLow  = ima->swingLow();
    }

    // HIGH-tier pattern target IS the N-bar structural extreme (Turtle Soup 4-bar
    // high / Momentum Pinball swing); absent for other tiers.
    const double nbarHigh = (tier == tbe::Tier::HIGH_DEDICATED &&  isLong) ? structuralTarget : 0.0;
    const double nbarLow  = (tier == tbe::Tier::HIGH_DEDICATED && !isLong) ? structuralTarget : 0.0;

    tbe::BarrierInputs in{};
    in.pattern_id              = pat;
    in.is_long                 = isLong;
    in.entry                   = entry;
    in.bar_high                = static_cast<double>(sc.High[sc.Index]);
    in.bar_low                 = static_cast<double>(sc.Low[sc.Index]);
    in.prev_high               = (sc.Index >= 1) ? static_cast<double>(sc.High[sc.Index - 1]) : in.bar_high;
    in.prev_low                = (sc.Index >= 1) ? static_cast<double>(sc.Low[sc.Index - 1]) : in.bar_low;
    in.atr10                   = atr10;
    in.dof_stop_scale          = dofScale;
    in.regime_stop_width_scale = 1.0;   // Phase 1 identity
    in.nbar_extreme_high       = nbarHigh;
    in.nbar_extreme_low        = nbarLow;
    in.swing_high              = static_cast<double>(swingHigh);
    in.swing_low               = static_cast<double>(swingLow);
    in.regime                  = regime;
    in.tick_size               = sc.TickSize;
    return in;
}
}  // namespace

void PositionManager::ProcessPendingPrediction(SCStudyInterfaceRef sc) {
    // === SEMANTIC STALENESS: Pipeline Latency Budget ===
    // In an event-driven system, predictions arrive when indicators change — NOT on a clock.
    // Wall-clock age measures PIPELINE HEALTH (transport + inference), not world-staleness.
    // A prediction is semantically valid until the next event fires (nothing the model saw changed).
    // 500ms budget: generous for Transformer forward pass + HMM inference + ZMQ round-trip.
    constexpr uint64_t kMaxPipelineLatencyUs = 500000ULL;  // 500 ms pipeline health budget

    // Single-slot consumption: read from TradeExecutionServer (replaces queue drain)
    TradeExecutionServer::PredictionSlot prediction;
    if (!TradeExecutionServer::Instance().ConsumePendingPrediction(prediction)) {
        return; // No pending prediction
    }

    constexpr uint8_t kMinTradeActionId = static_cast<uint8_t>(TradeActionEnum::STAND_ASIDE);
    constexpr uint8_t kMaxTradeActionId = static_cast<uint8_t>(TradeActionEnum::TRAP_SHORT);
    if (prediction.actionId < kMinTradeActionId || prediction.actionId > kMaxTradeActionId) {
        LogOrderFailure(
            "AUTOMATIC",
            TradeActionEnum::STAND_ASIDE,
            ReasonCode::PolicyEvalErrorFailClosed,
            "ACTION_DECODE",
            "invalid_action_id=" + std::to_string(static_cast<int>(prediction.actionId)),
            WallClockNowUs());
        return;
    }

    TradeActionEnum action = static_cast<TradeActionEnum>(prediction.actionId);
    ClearPendingPredictionAck();

    // === ELITE v3.2: HARD GATE PRE-CHECK (fail-fast for entries) ===
    // Exit actions always allowed through (safety takes priority over gates).
    // Entry actions rejected immediately if physics gates are violated.
    if (IsEntryAction(action)) {
        BeginPendingPredictionAck(prediction, action);
        const uint64_t nowUs = WallClockNowUs();
        const uint64_t intentId = prediction.sequenceId != 0 ? prediction.sequenceId : prediction.timestampUs;
        const uint64_t createdUs = prediction.timestampUs > 0 ? prediction.timestampUs : nowUs;

        if (!BeginIntentTicket(
                sc,
                intentId,
                prediction.sequenceId,
                createdUs,
                AUTOMATIC_INTENT_TTL_US,
                action,
                "AUTOMATIC")) {
            LogOrderFailure(
                "AUTOMATIC",
                action,
                ReasonCode::IntentSuperseded,
                "INTENT_TICKET",
                "begin_intent_ticket_failed",
                nowUs);
            EmitPendingPredictionAckRejected(ReasonCode::IntentSuperseded, "begin_intent_ticket_failed");
            return;
        }

        const bool connectivityFailClosed = IsConnectivityFailClosed(sc, action);

        // Layer 1: Pipeline latency gate — is the inference pipeline healthy?
        // Measures transport + inference time, NOT world-staleness.
        const bool predictionFresh =
            prediction.timestampUs != 0 &&
            nowUs > prediction.timestampUs &&
            (nowUs - prediction.timestampUs) <= kMaxPipelineLatencyUs;

        // Layer 2: Regime coherence gate — is the HMM regime the same as when
        // the Transformer generated this prediction?  The Transformer's action is
        // conditioned on a specific regime embedding; if the regime has since
        // shifted, the prediction's premise is invalidated.
        // Note: regime transitions ALSO trigger re-inference (reinfer_action_id),
        // so this gate primarily catches the race where C++ tries to act on a
        // stale prediction BEFORE the re-inference response arrives.
        if (prediction.hmmStateId >= 0) {
            const auto* hmmNow = InferenceManager::Instance().HmmState();
            const int currentHmmId = hmmNow ? static_cast<int>(hmmNow->Value()) : -1;
            if (currentHmmId >= 0 && currentHmmId != prediction.hmmStateId) {
                LogOrderFailure(
                    "AUTOMATIC",
                    action,
                    ReasonCode::PredictionStale,
                    "REGIME_COHERENCE",
                    "pred_regime=" + std::to_string(prediction.hmmStateId) +
                    "|current_regime=" + std::to_string(currentHmmId),
                    nowUs);
                RejectIntentTicket(action, ReasonCode::PredictionStale);
                return;
            }
        }

        // D.6: Context freshness gate — automatic entries require context_age <= 200ms
        constexpr uint64_t kMaxContextAgeUs_Auto = 200000ULL;
        const auto& ctx = ContextManager::Instance().GetLocalRiskContext();
        const bool contextFresh =
            ctx.snapshotTimestampUs != 0 &&
            nowUs > ctx.snapshotTimestampUs &&
            (nowUs - ctx.snapshotTimestampUs) <= kMaxContextAgeUs_Auto;

        auto hardGateResult = RiskManager::Instance().EvaluateHardGates(ctx);

        ExecutionGate::GateContext gateCtx;
        gateCtx.source = ExecutionGate::EntrySource::Automatic;
        gateCtx.isEntryAction = true;
        gateCtx.connectivityFailClosed = connectivityFailClosed;
        gateCtx.connectivityReason = ToGateConnectivityReason(
            (m_connectivityDownReason == ReasonCode::NA) ? ReasonCode::ScDisconnected : m_connectivityDownReason);
        gateCtx.modelReady = prediction.modelReady;
        gateCtx.predictionFresh = predictionFresh;
        gateCtx.contextFresh = contextFresh;
        gateCtx.allowNewEntries = TradeExecutionServer::Instance().IsNewEntriesAllowed();
        gateCtx.hardGatesPass = hardGateResult.isOk();
        gateCtx.isFlat = IsFlat();
        gateCtx.hasPendingEntryOrder = m_pendingEntryOrder.active;
        gateCtx.tradingHalted = RiskManager::Instance().IsTradingHalted();
        gateCtx.hmmMetricsValid = ctx.isValid;
        gateCtx.paretoTopStateRatio = ComputeParetoTopStateRatioProxy(ctx);
        gateCtx.shannonTenureBars = static_cast<float>(std::max(0, ctx.regimeDuration));
        gateCtx.talebSignalSigma = std::max(0.0f, ctx.talebKurtosis);
        gateCtx.paretoTopStateRatioMax = RiskManager::Instance().GetParetoTopStateRatioMax();
        gateCtx.shannonMinTenureBars = RiskManager::Instance().GetShannonMinTenureBars();
        gateCtx.talebSignalSigmaThreshold = RiskManager::Instance().GetTalebSignalSigmaThreshold();

        const auto decision = ExecutionGate::EvaluateEntry(gateCtx);
        if (decision.outcome == ExecutionGate::Outcome::Ignore) {
            return;
        }
        if (decision.outcome == ExecutionGate::Outcome::Deny) {
            const ReasonCode denyReason = ToPositionReasonCode(decision.reason);
            const std::string hardGateDetail = hardGateResult.isOk() ? std::string() : ("hard_gate_error=" + hardGateResult.error());
            LogOrderFailure(
                "AUTOMATIC",
                action,
                denyReason,
                "ENTRY_GATE",
                hardGateDetail,
                nowUs,
                ctx.snapshotTimestampUs,
                &gateCtx);
            RejectIntentTicket(action, denyReason, ShouldEmitGateTelemetry(denyReason));
            return;
        }

        TransitionIntentTicket("ELIGIBLE", ReasonCode::NA);
    }

    // === PHASE 1: EXIT LOGIC ===
    if (IsExitAction(action)) {
        if (IsFlat()) {
            return;
        }

        bool isLong = IsLong();
        bool exitValid = (action == TradeActionEnum::EXIT_LONG && isLong) ||
                         (action == TradeActionEnum::EXIT_SHORT && IsShort());

        if (!exitValid) {
            SCString mismatchMsg;
            mismatchMsg.Format("PositionManager: EXIT direction mismatch action_id=%d position=%s; ignoring",
                static_cast<int>(action), isLong ? "LONG" : "SHORT");
            Logger::getInstance().log(mismatchMsg.GetChars());
            EmitGateEventTelemetry(ReasonCode::PatternInvalidated, action);
            return;
        }

        int result = sc.FlattenAndCancelAllOrders();
        if (result >= 0) {
            SCString logMsg;
            logMsg.Format("PositionManager: Flattened %s position via model exit signal", isLong ? "LONG" : "SHORT");
            Logger::getInstance().log(logMsg.GetChars());
        } else {
            Logger::getInstance().log("PositionManager: Failed to flatten position");
        }
        return;
    }

    // === PHASE 2: ENTRY PRE-FLIGHT ===
    if (!IsEntryAction(action)) {
        if (action == TradeActionEnum::HOLD_LONG || action == TradeActionEnum::HOLD_SHORT ||
            action == TradeActionEnum::TRAP_LONG || action == TradeActionEnum::TRAP_SHORT) {
            SCString holdMsg;
            holdMsg.Format("PositionManager: action_id=%d received — no order action (position maintained)",
                static_cast<int>(action));
            Logger::getInstance().log(holdMsg.GetChars());
        }
        return;
    }

    // Entry preflight (flat/pending/halted) is centralized in ExecutionGate.

    // === PHASE 2b: SCORING VALIDATION (Daily Bias & Market Structure) ===
    auto* dailyBiasInd = IndicatorManager::Instance().GetIndicator<DailyBiasIndicator>(IndicatorKey::DAILY_BIAS);
    DailyBiasEnum currentBias = dailyBiasInd ? dailyBiasInd->Value() : DailyBiasEnum::PHYSICS_VETO_RANDOM_WALK;

    PatternType patternType = PatternType::Unknown;

    bool isLongTrade = (action == TradeActionEnum::ENTER_LONG);

    Scoring::BiasFilterResult biasResult = Scoring::Instance().ApplyDailyBiasFilter(patternType, isLongTrade, currentBias);

    if (!biasResult.allowed) {
        LogOrderFailure(
            "AUTOMATIC",
            action,
            ReasonCode::PatternQualityFail,
            "BIAS_FILTER",
            "bias_reason=" + biasResult.reason,
            WallClockNowUs());
        RejectIntentTicket(action, ReasonCode::PatternQualityFail, false);
        return;
    }

    // === PHASE 2c: IMPULSE CENSORSHIP (matching Python labeler) ===
    auto* impulseInd = IndicatorManager::Instance().GetIndicator<Impulse>(IndicatorKey::INTERM_IMP);
    ImpulseEnum currentImpulse = impulseInd ? impulseInd->Value() : ImpulseEnum::UNDEFINED;

    if ((isLongTrade && currentImpulse == ImpulseEnum::RED) ||
        (!isLongTrade && currentImpulse == ImpulseEnum::GREEN)) {
        LogOrderFailure(
            "AUTOMATIC",
            action,
            ReasonCode::PatternQualityFail,
            "IMPULSE_CENSORSHIP",
            "impulse=" + std::to_string(static_cast<int>(currentImpulse)),
            WallClockNowUs());
        RejectIntentTicket(action, ReasonCode::PatternQualityFail, false);
        return;
    }

    // === PHASE 2c.1: TS2 HURST REGIME FILTER (60m wave persistence guard) ===
    auto* ts2HurstInd = IndicatorManager::Instance().GetIndicator<HurstExponentIndicator>(IndicatorKey::HURST_EXPONENT);
    const HurstExponentEnum ts2HurstState =
        ts2HurstInd ? ts2HurstInd->Value() : HurstExponentEnum::RANDOM_WALK;
    const Ts2HurstRegimeDecision ts2HurstDecision = EvaluateTs2HurstRegime(ts2HurstState);

    if (!ts2HurstDecision.allow) {
        LogOrderFailure(
            "AUTOMATIC",
            action,
            ReasonCode::PatternQualityFail,
            "TS2_HURST_GUARD",
            "state=" + std::string(Ts2HurstStateToString(ts2HurstState)),
            WallClockNowUs());
        RejectIntentTicket(action, ReasonCode::PatternQualityFail, false);
        return;
    }

    // === ELITE: ENTROPY GATE (Shannon — model confusion filter) ===
    // action_entropy measures Shannon entropy of the 9-class softmax distribution.
    // High entropy → model can't distinguish actions → refuse entry.
    // Threshold: ln(9) ≈ 2.197; entropy > 1.5 means top action gets < ~22% probability.
    // §8.2: Cliff at 1.8 removed. Continuous discount [1.0, 2.0] → [1.0×, 0.0×] in
    // CalculateSafePositionSize handles sizing. TDE hard veto at 2.0 is the true ceiling.
    const float actionEntropy = prediction.actionEntropy;

    // Apply confidence boost/penalty from bias structure
    float confidence = prediction.confidence;
    if (biasResult.multiplier_adjustment != 1.0) {
        confidence = std::clamp(confidence * (float)biasResult.multiplier_adjustment, 0.0f, 1.0f);
    }
    if (ts2HurstDecision.confidenceMultiplier != 1.0f) {
        confidence = std::clamp(confidence * ts2HurstDecision.confidenceMultiplier, 0.0f, 1.0f);
    }

    // === GAP 9: SHANNON EFFICIENCY TRUST GATE (Shannon — signal-to-noise ratio) ===
    // shannonEfficiency = 1 - H/Hmax measures how much real signal exists in the market.
    // A model predicting with 92% confidence in a 90%-noise market is less trustworthy
    // than the same 92% in a clean signal regime.  Discount confidence by noise level.
    {
        const auto& lrc = ContextManager::Instance().GetLocalRiskContext();
        if (lrc.isValid) {
            const float eff = std::clamp(lrc.shannonEfficiency, 0.0f, 1.0f);
            // effectiveConfidence = confidence × (0.5 + 0.5 × efficiency)
            // At efficiency=1.0 (pure signal): full confidence.
            // At efficiency=0.0 (pure noise): confidence halved.
            const float trustMultiplier = 0.5f + 0.5f * eff;
            confidence = std::clamp(confidence * trustMultiplier, 0.0f, 1.0f);
        }
    }

    // === PHASE 2d: SESSION QUALITY MULTIPLIER (matching Python scoring.py) ===
    auto* timeOfDayInd = IndicatorManager::Instance().GetIndicator<TimeOfDayIndicator>(IndicatorKey::TIME_OF_DAY);
    TimeOfDayEnum currentSession = TimeOfDayEnum::OVERNIGHT_HOLD;
    if (timeOfDayInd) {
        currentSession = timeOfDayInd->Value();
        float sessionMult = 1.0f;
        switch (currentSession) {
            case TimeOfDayEnum::SWEET_SPOT:         sessionMult = 1.15f; break;
            case TimeOfDayEnum::OPENING_HOUR:       sessionMult = 1.10f; break;
            case TimeOfDayEnum::AFTERNOON_SESSION:  sessionMult = 1.00f; break;
            case TimeOfDayEnum::LUNCH_DEAD_ZONE:    sessionMult = 0.75f; break;
            case TimeOfDayEnum::FINAL_HOUR:         sessionMult = 0.60f; break;
            case TimeOfDayEnum::PRE_MARKET:
            case TimeOfDayEnum::AFTER_HOURS:
            case TimeOfDayEnum::OVERNIGHT_HOLD:     sessionMult = 0.50f; break;
            default:                                sessionMult = 1.00f; break;
        }
        if (sessionMult != 1.0f) {
            float oldConf = confidence;
            confidence = std::clamp(confidence * sessionMult, 0.0f, 1.0f);

            SCString sessionLog;
            sessionLog.Format("PositionManager: Session Quality | %.2f -> %.2f (%.2fx) | Session=%d",
                              oldConf, confidence, sessionMult, (int)currentSession);
            Logger::getInstance().log(sessionLog.GetChars());
        }
    }

    // Live-convergence parity: low-confidence directional predictions in afternoon
    // are flattened/rejected using the runtime-configured confidence cap.
    if (currentSession == TimeOfDayEnum::AFTERNOON_SESSION) {
        const float confidenceCap = RiskManager::Instance().GetLiveForceFlatConfidenceCap();
        if (confidence < confidenceCap) {
            LogOrderFailure(
                "AUTOMATIC",
                action,
                ReasonCode::PatternQualityFail,
                "LATE_SESSION_CONFIDENCE_CAP",
                "confidence=" + std::to_string(confidence) +
                    " threshold=" + std::to_string(confidenceCap),
                WallClockNowUs());
            RejectIntentTicket(action, ReasonCode::PatternQualityFail, false);
            return;
        }
    }

    // === GAP 10: TALEB SKEWNESS DIRECTIONAL ASYMMETRY GATE (Taleb — convexity alignment) ===
    // talebSkewness tells you which tail is fatter.  Negative skewness + long entry =
    // you face asymmetric downside (the left tail is longer).  Positive skewness + short
    // entry = same problem in reverse.  Force passive execution when fighting the tail.
    bool skewnessForcePassive = false;
    {
        const auto& lrc = ContextManager::Instance().GetLocalRiskContext();
        if (lrc.isValid) {
            const bool longAgainstLeftTail  = (isLongTrade && lrc.talebSkewness < -1.0f);
            const bool shortAgainstRightTail = (!isLongTrade && lrc.talebSkewness > 1.0f);
            if (longAgainstLeftTail || shortAgainstRightTail) {
                skewnessForcePassive = true;
            }
        }
    }

    // === GAP 12: RASCHKE BURST CLUSTERING ENTRY GUARD (Raschke — event dynamics) ===
    // raschkeBurst (CV of inter-arrival times) measures event clustering.
    // High clustering = volatile microstructure, stale quotes, whipsaw risk.
    // Submitting during a cluster storm is walking into a minefield.
    bool burstForcePassive = false;
    {
        const auto& lrc = ContextManager::Instance().GetLocalRiskContext();
        if (lrc.isValid && lrc.raschkeBurst > 2.0f) {
            if (lrc.raschkeBurst > 3.0f) {
                LogOrderFailure("AUTOMATIC", action, ReasonCode::ClusteringBreach,
                    "BURST_CLUSTERING",
                    "raschke_burst=" + std::to_string(lrc.raschkeBurst),
                    WallClockNowUs());
                RejectIntentTicket(action, ReasonCode::ClusteringBreach, false);
                return;
            }
            burstForcePassive = true;  // 2.0-3.0 → force passive
        }
    }

    // === GAP 22: OBSERVATION QUALITY GATE (Mandelbrot — extrapolation detection) ===
    // The Transformer's prediction quality depends on observation quality.  If ≥ 4 of
    // the 16D scaled observation features are at extreme quantiles (robust |z| > 3.0),
    // the market state is unusual — the model may be extrapolating, not interpolating.
    // Force passive execution + sizing discount for never-seen-before states.
    //
    // Thresholds are in each scaler's output space:
    //   SOFTLOGZ: sign(z)*log1p(|z|) → |output| > log1p(3.0) ≈ 1.386
    //   LOGZ:     clamped robust z    → |output| > 3.0
    bool obsQualityForcePassive = false;
    {
        const auto obs = ContextManager::Instance().GetLatestScaledObservation();
        int extremeCount = 0;
        constexpr float kSoftLogZThreshold = 1.386f;  // log1p(3.0) — robust z=3.0 in SoftLogZ space
        constexpr float kLogZThreshold = 3.0f;        // direct robust z threshold
        for (size_t i = 0; i < obs.size(); ++i) {
            const float threshold =
                (FeatureScaler::SCALE_MODE_MAP[i] == FeatureScaler::ScaleMode::SOFTLOGZ)
                    ? kSoftLogZThreshold : kLogZThreshold;
            if (std::fabs(obs[i]) > threshold || !std::isfinite(obs[i])) {
                extremeCount++;
            }
        }
        if (extremeCount >= 4) {
            obsQualityForcePassive = true;
            // Discount confidence for out-of-distribution states
            confidence = std::clamp(confidence * 0.85f, 0.0f, 1.0f);
        }
    }

    // === PHASE 3: PATTERN-BASED PRICE CALCULATION ===
    float entryPrice = 0.0f;
    float stopPrice = 0.0f;
    float targetPrice = 0.0f;
    bool isLong = (action == TradeActionEnum::ENTER_LONG);

    // Gap 1: Use cached ATR from TripleScreen1
    float atr = m_cachedATR14;
    if (atr <= 0.0f) {
        Logger::getInstance().log("Prediction rejected: Cached ATR14 not available");
        LogOrderFailure(
            "AUTOMATIC",
            action,
            ReasonCode::PolicyEvalErrorFailClosed,
            "ATR_CACHE",
            "cached_atr14_unavailable",
            WallClockNowUs());
        RejectIntentTicket(action, ReasonCode::PolicyEvalErrorFailClosed, false);
        return;
    }

    // Gap 2: DOF-scale ATR for pattern stops — logic in HmmStateIndicator::DofStopScale().
    auto* hmmPatternInd = InferenceManager::Instance().HmmState();
    const double dofScale = hmmPatternInd ? hmmPatternInd->DofStopScale() : 1.5;
    const float dofScaledAtr = atr * static_cast<float>(dofScale);

    // actionId maps to pattern — try tactical first, then strategy
    int patternId = static_cast<int>(prediction.actionId);
    bool priceCalcOk = CalculateTacticalTriggerPrices(sc, patternId, isLong, dofScaledAtr, entryPrice, stopPrice, targetPrice);
    if (!priceCalcOk) {
        priceCalcOk = CalculateStrategySetupPrices(sc, patternId, isLong, dofScaledAtr, entryPrice, stopPrice, targetPrice);
    }

    if (!priceCalcOk) {
        Logger::getInstance().log("Prediction rejected: Failed to calculate pattern prices for action " +
                                  std::to_string(prediction.actionId));
        LogOrderFailure(
            "AUTOMATIC",
            action,
            ReasonCode::PatternInvalidated,
            "PRICE_CALC",
            "pattern_price_calc_failed action_id=" + std::to_string(static_cast<int>(prediction.actionId)),
            WallClockNowUs());
        RejectIntentTicket(action, ReasonCode::PatternInvalidated, false);
        return;
    }

    // === GAP B/D: STRUCTURAL ENTRY-QUALITY ASSESSMENT (Institutional) ===
    // Entry-quality flags only steer EXECUTION STYLE (passive vs aggressive) — they
    // no longer mutate the stop.  Stop/target come solely from the Triple-Barrier
    // engine (first-touch parity with the labeler).  The former GAP C 60-min swing
    // stop-tightening was a live-only divergence from the labeler and has been
    // removed (Phase 1 cutover, Step C).
    bool structEntryIntoResistance = false;
    bool structEntryAtSupport = false;
    {
        auto ima = IndicatorManager::Instance().GetIndicator<IntermediateMarketAction>(IndicatorKey::SHORT_MKT_ACTION);
        if (ima) {
            const float swHigh = ima->swingHigh();
            const float swLow  = ima->swingLow();

            constexpr float STRUCT_PROXIMITY_ATR = 0.75f; // within 0.75 ATR = "near structural level"
            const float proximityThreshold = STRUCT_PROXIMITY_ATR * atr;

            if (isLong) {
                // GAP B: Entry quality — entering near 60-min swing high = resistance = poor quality
                if (swHigh > 0.0f && swHigh > entryPrice && (swHigh - entryPrice) < proximityThreshold) {
                    structEntryIntoResistance = true;
                }
                // Entering near 60-min swing low = support = structural confluence
                if (swLow > 0.0f && swLow < entryPrice && (entryPrice - swLow) < proximityThreshold) {
                    structEntryAtSupport = true;
                }
            } else {
                // GAP B (short): Entering near swing low (support) = into structure
                if (swLow > 0.0f && swLow < entryPrice && (entryPrice - swLow) < proximityThreshold) {
                    structEntryIntoResistance = true;
                }
                // Near swing high = structural resistance = confluence for shorts
                if (swHigh > 0.0f && swHigh > entryPrice && (swHigh - entryPrice) < proximityThreshold) {
                    structEntryAtSupport = true;
                }
            }

            // GAP D: Keltner rejection against trade direction = entering into failed move
            if (isLong && ima->upperRejected()) {
                structEntryIntoResistance = true;
            }
            if (!isLong && ima->lowerRejected()) {
                structEntryIntoResistance = true;
            }
        }
    }

    // === STEP C: TRIPLE-BARRIER ENGINE BARRIERS (single source of truth) ===
    // Overwrite the pattern stop/target with the immutable first-touch engine
    // barriers so position sizing (Phase 4) and the SC bracket (Phase 5) both use
    // the exact levels the labeler reproduces.  targetPrice from Phase 3 is passed
    // as the structural (N-bar) target for HIGH-tier patterns.
    {
        const tbe::BarrierInputs tbIn = BuildBarrierInputs(
            sc, patternId, isLong, static_cast<double>(entryPrice),
            static_cast<double>(targetPrice), static_cast<double>(m_cachedATR10));
        const tbe::Barriers tbB = tbe::ComputeBarriers(tbIn);
        stopPrice   = static_cast<float>(tbB.stop);
        targetPrice = static_cast<float>(tbB.target);
    }

    // === PHASE 3b: MICROSTRUCTURE ADMISSION + EXECUTION STYLE MAPPING ===
    // Deterministic styles: PASSIVE_BID_ASK, MIDPOINT_IMPROVE, AGGRESSIVE_CROSS
    const float decisionPrice = entryPrice;
    const double arrivalBid = static_cast<double>(sc.Bid);
    const double arrivalAsk = static_cast<double>(sc.Ask);
    const bool hasInsideQuote = (arrivalAsk > arrivalBid) && (sc.TickSize > 0.0f);
    const double spreadTicks = hasInsideQuote ? ((arrivalAsk - arrivalBid) / static_cast<double>(sc.TickSize)) : 0.0;

    float allowedSpreadTicks = GetSessionSpreadLimit();

    // === GAP 15: ADAPTIVE SPREAD GATE (Pareto + Raschke — dynamic microstructure threshold) ===
    // Static spread gates miss regime-dependent fill quality.  A 1.5-tick spread during
    // TALEBIAN_FRAGILE + high clustering is far more dangerous than 1.5 ticks in GAUSSIAN_STABLE.
    // Tighten the gate when liquidity fragility or event clustering are elevated.
    {
        const auto& lrc = ContextManager::Instance().GetLocalRiskContext();
        if (lrc.isValid) {
            // Liquidity fragility: >0.50 → tighten gate proportionally (up to 40% tighter)
            if (lrc.spreadStress > 0.50f) {
                const float liqPenalty = 1.0f - 0.4f * std::clamp((lrc.spreadStress - 0.50f) / 0.50f, 0.0f, 1.0f);
                allowedSpreadTicks *= liqPenalty;
            }
            // Burst clustering >2.0 → tighten gate by 25%
            if (lrc.raschkeBurst > 2.0f) {
                allowedSpreadTicks *= 0.75f;
            }
        }
    }

    if (hasInsideQuote && spreadTicks > static_cast<double>(allowedSpreadTicks)) {
        SCString logMsg;
        logMsg.Format("Order rejected [SPREAD_BREACH]: spread=%.2f ticks (limit=%.2f)", spreadTicks, allowedSpreadTicks);
        Logger::getInstance().log(logMsg.GetChars());
        LogOrderFailure(
            "AUTOMATIC",
            action,
            ReasonCode::SpreadBreach,
            "MICROSTRUCTURE_SPREAD",
            "spread_ticks=" + std::to_string(spreadTicks) + " limit_ticks=" + std::to_string(allowedSpreadTicks),
            WallClockNowUs());
        RejectIntentTicket(action, ReasonCode::SpreadBreach, false);
        return;
    }

    const auto* volInd = IndicatorManager::Instance().GetIndicator<VolumeIndicator>(IndicatorKey::VOLUME_SIGNAL);
    const double imbalance = volInd ? static_cast<double>(volInd->GetVolumeImbalance()) : 0.0;

    // Min-volume guard (#4, gate session-awareness audit): the imbalance (ask-bid)/(ask+bid)
    // is a sample proportion whose noise ~ 1/sqrt(N_contracts). In a thin book (overnight
    // especially) a handful of contracts can push it past ±0.35 spuriously, causing FALSE
    // toxic-flow rejections. For the ±0.35 band to sit ~2 SE from zero we need N ≳ 33
    // contracts (0.35 > 2/sqrt(N)); use a modest floor so the gate only acts on a
    // statistically meaningful sample. Thin-book illiquidity itself is covered by the
    // spread / Amihud gates — not this directional-toxicity gate.
    constexpr double kMinImbalanceVolume = 50.0;  // contracts; proportion-SE sample floor
    const double imbalanceSampleVol = volInd ? static_cast<double>(volInd->GetLastTotalVolume()) : 0.0;
    const bool imbalanceReliable = (imbalanceSampleVol >= kMinImbalanceVolume);

    if (imbalanceReliable && ((isLong && imbalance < -0.35) || (!isLong && imbalance > 0.35))) {
        SCString logMsg;
        logMsg.Format("Order rejected [TOXIC_FLOW_BREACH]: imbalance=%.3f (vol=%.0f)", imbalance, imbalanceSampleVol);
        Logger::getInstance().log(logMsg.GetChars());
        LogOrderFailure(
            "AUTOMATIC",
            action,
            ReasonCode::ToxicFlowBreach,
            "MICROSTRUCTURE_FLOW",
            "imbalance=" + std::to_string(imbalance),
            WallClockNowUs());
        RejectIntentTicket(action, ReasonCode::ToxicFlowBreach, false);
        return;
    }

    if (hasInsideQuote && IsQuoteChurnBreached(sc, arrivalBid, arrivalAsk)) {
        SCString logMsg;
        logMsg.Format("Order rejected [QUOTE_CHURN_BREACH]: churn=%.2f events/sec", m_quoteChurnEmaPerSec);
        Logger::getInstance().log(logMsg.GetChars());
        LogOrderFailure(
            "AUTOMATIC",
            action,
            ReasonCode::QuoteChurnBreach,
            "MICROSTRUCTURE_CHURN",
            "quote_churn_ema_per_sec=" + std::to_string(m_quoteChurnEmaPerSec),
            WallClockNowUs());
        RejectIntentTicket(action, ReasonCode::QuoteChurnBreach, false);
        return;
    }

    // === ELITE: ENTROPY-AWARE EXECUTION STYLE (Shannon) ===
    // High entropy (model uncertain) → force passive execution regardless of confidence.
    // This prevents aggressive crossing when the model is confused but still above
    // the hard reject threshold.
    constexpr float kEntropyPassiveForce = 1.5f;
    const bool entropyForcePassive = (actionEntropy > kEntropyPassiveForce);

    // === GAP 3: REGIME-CONDITIONAL EXECUTION STYLE (Taleb + Pareto) ===
    // Microstructure quality varies dramatically by HMM regime.  Use regime as a
    // second axis alongside confidence/entropy to select execution style.
    const auto* hmmExecInd = InferenceManager::Instance().HmmState();
    const HMMStateEnum execRegime = hmmExecInd ? hmmExecInd->Value() : HMM_NO_PRIOR;

    // GAUSSIAN_FRAGILE: institutional flow is toxic — force passive regardless of confidence.
    // Fading institutional sweep with aggressive execution is how retail loses.
    const bool regimeForcePassive = (execRegime == HMMStateEnum::GAUSSIAN_FRAGILE);

    // PARETO_MOMENTUM: momentum is your ally — allow aggressive crossing at lower
    // confidence threshold (0.85 vs 0.90) and wider spread (2 ticks vs 1).
    const bool momentumBoost = (execRegime == HMMStateEnum::PARETO_MOMENTUM);

    // === GAP 11: FRACTAL DIMENSION → EXECUTION ROUGHNESS DETECTOR (Mandelbrot) ===
    // fractalDim near 1.0 = smooth/trending (directed price action).
    // Near 2.0 = rough/random (Brownian noise).  Aggressive execution in rough markets
    // causes jitter-through fills and TCA degradation.  Smooth markets reward speed.
    bool fractalForcePassive = false;
    bool fractalAllowAggressive = false;
    {
        const auto& lrc = ContextManager::Instance().GetLocalRiskContext();
        if (lrc.isValid) {
            if (lrc.fractalDim > 1.6f) {
                fractalForcePassive = true;   // Rough market → passive only
            } else if (lrc.fractalDim < 1.3f) {
                fractalAllowAggressive = true; // Smooth market → lower aggressive threshold
            }
        }
    }

    // === DOF-ADAPTIVE CONFIDENCE THRESHOLDS ===
    // Let the Student-t distribution set the threshold: fat tails → demand more evidence.
    // Base thresholds (Gaussian-regime defaults) are scaled upward by DOF penalty.
    const auto* hmmDofInd = InferenceManager::Instance().HmmState();
    const float threshMomentum   = hmmDofInd ? hmmDofInd->DofConfidenceThreshold(0.82f) : 0.87f;  // PARETO_MOMENTUM base
    const float threshSupport    = hmmDofInd ? hmmDofInd->DofConfidenceThreshold(0.78f) : 0.83f;  // Structural confluence base
    const float threshAggressive = hmmDofInd ? hmmDofInd->DofConfidenceThreshold(fractalAllowAggressive ? 0.83f : 0.88f)
                                             : (fractalAllowAggressive ? 0.88f : 0.93f);          // Generic aggression base
    const float threshMidpoint   = hmmDofInd ? hmmDofInd->DofConfidenceThreshold(0.78f) : 0.83f;  // Midpoint improve base

    std::string executionStyle = "PASSIVE_BID_ASK";
    if (entropyForcePassive || regimeForcePassive || skewnessForcePassive || burstForcePassive || fractalForcePassive || obsQualityForcePassive) {
        // Any force-passive override: stay passive.
        executionStyle = "PASSIVE_BID_ASK";
    } else if (structEntryIntoResistance) {
        // GAP B/D: Entering into 60-min resistance or against Keltner rejection —
        // institutional desks never cross the spread into adverse structure.
        executionStyle = "PASSIVE_BID_ASK";
    } else if (momentumBoost && confidence >= threshMomentum && spreadTicks <= 2.0 && prediction.regimeLatencyUs <= 12000) {
        // Momentum-boosted aggressive: PARETO_MOMENTUM + DOF-adaptive confidence → cross even at 2-tick spread.
        executionStyle = "AGGRESSIVE_CROSS";
    } else if (structEntryAtSupport && confidence >= threshSupport && spreadTicks <= 2.0 && prediction.regimeLatencyUs <= 12000) {
        // GAP B: Structural confluence — entering at 60-min support/resistance level.
        // DOF-adaptive: Gaussian regime allows aggression at ~0.78; fat tails push to ~0.88.
        executionStyle = "AGGRESSIVE_CROSS";
    } else if (confidence >= threshAggressive && (!hasInsideQuote || spreadTicks <= 1.0) && prediction.regimeLatencyUs <= 12000) {
        // Generic aggression: fractal + DOF jointly determine threshold.
        executionStyle = "AGGRESSIVE_CROSS";
    } else if (confidence >= threshMidpoint && hasInsideQuote && spreadTicks <= 2.0) {
        executionStyle = "MIDPOINT_IMPROVE";
    }

    // === GAP 7: TCA FEEDBACK — ADAPTIVE EXECUTION STYLE (Institutional) ===
    // If the chosen style's recent arrival slippage exceeds 0.5 ticks (mean over
    // last 20 fills), demote to the next-safer style.  This prevents persistent
    // adverse selection in a style that used to work but no longer does.
    {
        const auto& tca = AIConnectionMonitor::Instance();
        if (executionStyle == "AGGRESSIVE_CROSS" && tca.IsExecutionStyleDegraded("AGGRESSIVE_CROSS")) {
            executionStyle = "MIDPOINT_IMPROVE";
        }
        if (executionStyle == "MIDPOINT_IMPROVE" && tca.IsExecutionStyleDegraded("MIDPOINT_IMPROVE")) {
            executionStyle = "PASSIVE_BID_ASK";
        }
    }

    if (hasInsideQuote) {
        if (executionStyle == "PASSIVE_BID_ASK") {
            entryPrice = isLong
                ? std::min(entryPrice, static_cast<float>(arrivalBid))
                : std::max(entryPrice, static_cast<float>(arrivalAsk));
        } else if (executionStyle == "MIDPOINT_IMPROVE") {
            const float improvedLong = static_cast<float>(arrivalBid + static_cast<double>(sc.TickSize));
            const float improvedShort = static_cast<float>(arrivalAsk - static_cast<double>(sc.TickSize));
            entryPrice = isLong ? std::min(entryPrice, improvedLong) : std::max(entryPrice, improvedShort);
        } else { // AGGRESSIVE_CROSS
            if (spreadTicks > 1.0) {
                SCString logMsg;
                logMsg.Format("Order rejected [SPREAD_BREACH]: aggressive-cross blocked, spread=%.2f ticks", spreadTicks);
                Logger::getInstance().log(logMsg.GetChars());
                LogOrderFailure(
                    "AUTOMATIC",
                    action,
                    ReasonCode::SpreadBreach,
                    "EXECUTION_STYLE",
                    "aggressive_cross_spread_ticks=" + std::to_string(spreadTicks),
                    WallClockNowUs());
                RejectIntentTicket(action, ReasonCode::SpreadBreach, false);
                return;
            }
            entryPrice = isLong
                ? std::max(entryPrice, static_cast<float>(arrivalAsk))
                : std::min(entryPrice, static_cast<float>(arrivalBid));
        }
    }

    // === PHASE 4: ELITE POSITION SIZING & VALIDATION ===
    auto positionSizeResult = RiskManager::Instance().CalculateSafePositionSize(sc, entryPrice, stopPrice, confidence);

    if (!positionSizeResult.isOk()) {
        Logger::getInstance().log("Prediction rejected: RiskManager error: " + positionSizeResult.error());
        LogOrderFailure(
            "AUTOMATIC",
            action,
            ReasonCode::PolicyEvalErrorFailClosed,
            "POSITION_SIZING",
            positionSizeResult.error(),
            WallClockNowUs());
        RejectIntentTicket(action, ReasonCode::PolicyEvalErrorFailClosed, false);
        return;
    }

    int adjustedQuantity = positionSizeResult.unwrap();

    if (adjustedQuantity <= 0) {
        Logger::getInstance().log("PositionManager: Order suppressed (Adjusted Qty = 0)");
        LogOrderFailure(
            "AUTOMATIC",
            action,
            ReasonCode::PolicyEvalErrorFailClosed,
            "POSITION_SIZING",
            "adjusted_quantity<=0",
            WallClockNowUs());
        RejectIntentTicket(action, ReasonCode::PolicyEvalErrorFailClosed, false);
        return;
    }

    // Final Safety Gate
    auto validationResult = RiskManager::Instance().ValidateOrder(
        sc,
        entryPrice,
        stopPrice,
        adjustedQuantity,
        prediction.inferenceLatencyUs,
        prediction.transformerLatencyUs,
        prediction.regimeLatencyUs
    );
    if (!validationResult.isOk()) {
        Logger::getInstance().log("Order rejected: " + validationResult.error());
        LogOrderFailure(
            "AUTOMATIC",
            action,
            ReasonCode::PolicyEvalErrorFailClosed,
            "FINAL_VALIDATION",
            validationResult.error(),
            WallClockNowUs());
        RejectIntentTicket(action, ReasonCode::PolicyEvalErrorFailClosed, false);
        return;
    }

    // === PHASE 5: ORDER SUBMISSION (single-stage first-touch bracket) ===
    s_SCNewOrder order;
    order.OrderQuantity = adjustedQuantity;
    order.Price1 = entryPrice;
    order.OrderType = SCT_ORDERTYPE_LIMIT_CHASE;
    order.TimeInForce = SCT_TIF_DAY;

    // Limit Chase: SC engine reprices at zero latency; MaximumChaseAsPrice caps slippage per style.
    if (executionStyle == "AGGRESSIVE_CROSS") {
        order.MaximumChaseAsPrice = 1.0 * sc.TickSize;  // Already crossing, minimal additional chase
    } else {
        order.MaximumChaseAsPrice = 2.0 * sc.TickSize;  // PASSIVE/MIDPOINT: allow 2 ticks of chase
    }

    // === GAP 20: INFERENCE LATENCY → CHASE AGGRESSIVENESS (Shannon — information freshness) ===
    // A 50ms inference is 10× staler than 5ms — the market has moved further from the
    // decision price.  Widen chase for stale predictions, tighten for fresh ones.
    if (prediction.inferenceLatencyUs > 8000) {
        order.MaximumChaseAsPrice += 1.0 * sc.TickSize;  // Stale: market likely moved, chase further
    } else if (prediction.inferenceLatencyUs > 0 && prediction.inferenceLatencyUs < 3000) {
        // Fresh prediction: price is still close to where model saw it — tighten chase
        order.MaximumChaseAsPrice = std::max(0.0, order.MaximumChaseAsPrice - 1.0 * sc.TickSize);
    }

    // === GAP 25: FAT-TAIL CHASE CAP (Taleb — refuse to chase in crash regimes) ===
    // When tails are extreme (DOF ≤ 4 / kurtosis > 10), price discovery is unreliable.
    // Chasing into a falling knife amplifies adverse selection.  Cap chase to zero
    // (pure limit) so we only fill at our stated price or not at all.
    {
        const auto& lrcChase = ContextManager::Instance().GetLocalRiskContext();
        const auto* hmmChase = InferenceManager::Instance().HmmState();
        const float chaseDof = hmmChase ? hmmChase->Dof() : 30.0f;
        const float chaseKurtosis = lrcChase.isValid ? lrcChase.talebKurtosis : 3.0f;
        if (chaseDof <= 4.0f || chaseKurtosis > 10.0f) {
            order.MaximumChaseAsPrice = 0.0;  // Pure limit — no chase in crash regime
        }
    }

    // === GAP 17: REGIME-ADAPTIVE CHASE TIME CAP ===
    // SC lacks a native chase time limit field on s_SCNewOrder.
    // Regime-based chase time capping is enforced in ManageWorkingEntryOrder
    // via executionBudgetMs (see Gap 17 cap after Gap 23 Fisher scaling).

    // TextTag: intent ID + pattern name for SC Trade Activity Log identification
    {
        SCString tag;
        tag.Format("I:%llu|%s", static_cast<unsigned long long>(m_intentTicket.intentId), prediction.patternName.c_str());
        order.TextTag = tag;
    }

    // Bracket Stop — regime-aware stop TYPE selection (STATIC — no trailing)
    // === STEP C: REGIME-AWARE STATIC STOP TYPE (Taleb — crash resilience) ===
    //
    // Phase 1 first-touch: the stop is IMMUTABLE at the engine barrier.  Regime only
    // selects the stop ORDER TYPE for fill quality — never a trailing/breakeven
    // adjustment (naive profit-protection permanently rejected; see
    // triple_barrier_profit_protection_ruling.md):
    //   1. CRASH REGIME (DOF ≤ 4 / kurtosis > 10 / Amihud pctile > p90):
    //      → market stop (STOP_WITH_BID_ASK_TRIGGERING).  Guaranteed fill; in a
    //        flash crash a limit stop can gap through — exit certainty > price control.
    //   2. NORMAL / ORDERLY-BUT-TOXIC:
    //      → static stop-limit with 2-tick offset.  Protective fill control.
    {
        const auto& lrc = ContextManager::Instance().GetLocalRiskContext();
        const auto* hmmStop = InferenceManager::Instance().HmmState();
        const float dof = hmmStop ? hmmStop->Dof() : 30.0f;
        const float kurtosis = lrc.isValid ? lrc.talebKurtosis : 3.0f;

        const bool crashRegime = (dof <= 4.0f) || (kurtosis > 10.0f) ||
                                 (lrc.isValid && lrc.amihudPercentile > 0.90f);

        if (crashRegime) {
            // Tier 1: guaranteed exit — market stop fires immediately at trigger price
            order.AttachedOrderStop1Type = SCT_ORDERTYPE_STOP_WITH_BID_ASK_TRIGGERING;
        } else {
            // Tier 2: static stop-limit — protective fill control, no trailing
            order.AttachedOrderStop1Type = SCT_ORDERTYPE_STOP_LIMIT;
            order.StopLimitOrderLimitOffsetForAttachedOrders = 2.0 * sc.TickSize;
        }
    }
    order.Stop1Price = stopPrice;

    // === STEP C: SINGLE FULL-SIZE FIRST-TOUCH TARGET ===
    // One immutable target for the whole position at the engine barrier.  No
    // scale-out, no runner, no trailing, no move-to-breakeven — all removed in the
    // Phase 1 cutover (see triple_barrier_cutover_phase1_plan.md).  Winner give-back
    // is accepted as the control-group baseline; differentiated exit objectives
    // (breakout trend-scanning / meta-label exit) are a later, data-gated layer.
    order.AttachedOrderTarget1Type = SCT_ORDERTYPE_LIMIT_CHASE;
    order.Target1Price = targetPrice;
    order.AttachedOrderMaximumChase = 2.0 * sc.TickSize;

    int orderResult = isLong ? static_cast<int>(sc.BuyOrder(order)) : static_cast<int>(sc.SellOrder(order));

    if (orderResult > 0) {
        SCDateTime decisionTime = SCDateTime(static_cast<time_t>(prediction.timestampUs / 1000000ULL));
        AIConnectionMonitor::Instance().LogOrderSubmit(
            sc,
            orderResult,
            entryPrice,
            isLong,
            decisionPrice,
            static_cast<float>(arrivalBid),
            static_cast<float>(arrivalAsk),
            executionStyle.c_str(),
            decisionTime
        );
        m_pendingEntryOrder.active = true;
        m_pendingEntryOrder.orderId = orderResult;
        m_pendingEntryOrder.requestedQuantity = adjustedQuantity;
        m_pendingEntryOrder.isLong = isLong;
        m_pendingEntryOrder.decisionPrice = decisionPrice;
        m_pendingEntryOrder.submitPrice = entryPrice;
        m_pendingEntryOrder.executionStyle = executionStyle;
        m_pendingEntryOrder.repriceCount = 0;
        m_pendingEntryOrder.submitTime = sc.CurrentSystemDateTime;
        m_pendingEntryOrder.lastRepriceTime = sc.CurrentSystemDateTime;
        m_openTrade.SetPattern("EventTransformer", patternId, prediction.patternName);
        m_openTrade.SetConfidence(confidence);
        TransitionIntentTicket("EXECUTED", ReasonCode::NA);
        EmitPendingPredictionAckAccepted(entryPrice, stopPrice, targetPrice);

        SCString successMsg;
        successMsg.Format("Order SUBMITTED: ID=%d, Pattern=%s, Qty=%d, Entry=%.2f, Stop=%.2f, Style=%s, Spread=%.2f",
                          orderResult, prediction.patternName.c_str(), adjustedQuantity, entryPrice, stopPrice,
                          executionStyle.c_str(), spreadTicks);
        Logger::getInstance().log(successMsg.GetChars());
    } else {
        DetectBrokerSubmitFault(orderResult, action);
        Logger::getInstance().log("PositionManager: Sierra Chart order submission failed");
        LogOrderFailure(
            "AUTOMATIC",
            action,
            ReasonCode::IbDisconnected,
            "ORDER_SUBMIT",
            "sc_order_submission_failed order_result=" + std::to_string(orderResult),
            WallClockNowUs());
        RejectIntentTicket(action, ReasonCode::IbDisconnected, false);
    }
}

// ============================================================
// Manual Trade Command Processing
// Applies identical gate chain to Transformer predictions for risk parity
// ============================================================

void PositionManager::ProcessManualTradeCommand(
    SCStudyInterfaceRef sc,
    bool isLong,
    float entryPrice,
    float stopPrice,
    float targetPrice,
    uint32_t quantity,
    const std::string& patternName,
    uint64_t intentId,
    uint64_t sequenceId,
    uint64_t intentTimestampUs
) {
    // Manual directives must pass the SAME gates as Transformer predictions
    // This ensures uniform risk governance across all entry paths

    const TradeActionEnum manualAction = isLong ? TradeActionEnum::ENTER_LONG : TradeActionEnum::ENTER_SHORT;
    const uint64_t nowUs = WallClockNowUs();
    const uint64_t resolvedIntentId = intentId != 0 ? intentId : (intentTimestampUs != 0 ? intentTimestampUs : nowUs);
    const uint64_t resolvedCreatedUs = intentTimestampUs != 0 ? intentTimestampUs : nowUs;

    if (!BeginIntentTicket(
            sc,
            resolvedIntentId,
            sequenceId,
            resolvedCreatedUs,
            MANUAL_INTENT_TTL_US,
            manualAction,
            "MANUAL")) {
        LogOrderFailure(
            "MANUAL",
            manualAction,
            ReasonCode::IntentSuperseded,
            "INTENT_TICKET",
            "begin_intent_ticket_failed",
            nowUs);
        return;
    }

    const bool connectivityFailClosed = IsConnectivityFailClosed(sc, manualAction);

    // D.6: Context freshness gate — manual entries require context_age <= 400ms
    constexpr uint64_t kMaxContextAgeUs_Manual = 400000ULL;
    const auto& ctx = ContextManager::Instance().GetLocalRiskContext();
    const bool contextFresh =
        ctx.snapshotTimestampUs != 0 &&
        nowUs > ctx.snapshotTimestampUs &&
        (nowUs - ctx.snapshotTimestampUs) <= kMaxContextAgeUs_Manual;

    auto hardGateResult = RiskManager::Instance().EvaluateHardGates(ctx);

    ExecutionGate::GateContext gateCtx;
    gateCtx.source = ExecutionGate::EntrySource::Manual;
    gateCtx.isEntryAction = true;
    gateCtx.connectivityFailClosed = connectivityFailClosed;
    gateCtx.connectivityReason = ToGateConnectivityReason(
        (m_connectivityDownReason == ReasonCode::NA) ? ReasonCode::ScDisconnected : m_connectivityDownReason);
    gateCtx.contextFresh = contextFresh;
    gateCtx.allowNewEntries = TradeExecutionServer::Instance().IsNewEntriesAllowed();
    gateCtx.hardGatesPass = hardGateResult.isOk();
    gateCtx.isFlat = IsFlat();
    gateCtx.hasPendingEntryOrder = m_pendingEntryOrder.active;
    gateCtx.tradingHalted = RiskManager::Instance().IsTradingHalted();
    gateCtx.hmmMetricsValid = ctx.isValid;
    gateCtx.paretoTopStateRatio = ComputeParetoTopStateRatioProxy(ctx);
    gateCtx.shannonTenureBars = static_cast<float>(std::max(0, ctx.regimeDuration));
    gateCtx.talebSignalSigma = std::max(0.0f, ctx.talebKurtosis);
    gateCtx.paretoTopStateRatioMax = RiskManager::Instance().GetParetoTopStateRatioMax();
    gateCtx.shannonMinTenureBars = RiskManager::Instance().GetShannonMinTenureBars();
    gateCtx.talebSignalSigmaThreshold = RiskManager::Instance().GetTalebSignalSigmaThreshold();

    const auto manualDecision = ExecutionGate::EvaluateEntry(gateCtx);
    if (manualDecision.outcome == ExecutionGate::Outcome::Ignore) {
        return;
    }
    if (manualDecision.outcome == ExecutionGate::Outcome::Deny) {
        const ReasonCode denyReason = ToPositionReasonCode(manualDecision.reason);
        const std::string hardGateDetail = hardGateResult.isOk() ? std::string() : ("hard_gate_error=" + hardGateResult.error());
        LogOrderFailure(
            "MANUAL",
            manualAction,
            denyReason,
            "ENTRY_GATE",
            hardGateDetail,
            nowUs,
            ctx.snapshotTimestampUs,
            &gateCtx);
        if (denyReason == ReasonCode::HardSafetyViolation && !hardGateResult.isOk()) {
            Logger::getInstance().log("Manual entry REJECTED (hard gate): " + hardGateResult.error());
        }
        if (denyReason == ReasonCode::PolicyEvalErrorFailClosed) {
            Logger::getInstance().log("Manual entry rejected: Already in a position (must be flat to enter)");
        } else if (denyReason == ReasonCode::IntentSuperseded) {
            Logger::getInstance().log("Manual entry rejected: Existing working entry order still active");
        } else if (denyReason == ReasonCode::RiskDailyLimit) {
            Logger::getInstance().log("Manual entry rejected: Trading halted");
        }
        RejectIntentTicket(manualAction, denyReason, ShouldEmitGateTelemetry(denyReason));
        return;
    }

    TransitionIntentTicket("ELIGIBLE", ReasonCode::NA);

    // === PHASE 2b: BIAS FILTER ===
    auto* dailyBiasInd = IndicatorManager::Instance().GetIndicator<DailyBiasIndicator>(IndicatorKey::DAILY_BIAS);
    DailyBiasEnum currentBias = dailyBiasInd ? dailyBiasInd->Value() : DailyBiasEnum::PHYSICS_VETO_RANDOM_WALK;

    Scoring::BiasFilterResult biasResult = Scoring::Instance().ApplyDailyBiasFilter(PatternType::Unknown, isLong, currentBias);

    if (!biasResult.allowed) {
        SCString logMsg;
        logMsg.Format("Manual entry VETOED | Bias=%d | Reason=%s",
                      (int)currentBias, biasResult.reason.c_str());
        Logger::getInstance().log(logMsg.GetChars());
        LogOrderFailure(
            "MANUAL",
            manualAction,
            ReasonCode::PatternQualityFail,
            "BIAS_FILTER",
            "bias_reason=" + biasResult.reason,
            WallClockNowUs());
        RejectIntentTicket(manualAction, ReasonCode::PatternQualityFail, false);
        return;
    }

    // === PHASE 2c: IMPULSE CENSORSHIP ===
    auto* impulseInd = IndicatorManager::Instance().GetIndicator<Impulse>(IndicatorKey::INTERM_IMP);
    ImpulseEnum currentImpulse = impulseInd ? impulseInd->Value() : ImpulseEnum::UNDEFINED;

    if ((isLong && currentImpulse == ImpulseEnum::RED) ||
        (!isLong && currentImpulse == ImpulseEnum::GREEN)) {
        SCString impulseLog;
        impulseLog.Format("Manual entry rejected | Impulse Censorship | Impulse=%d", (int)currentImpulse);
        Logger::getInstance().log(impulseLog.GetChars());
        LogOrderFailure(
            "MANUAL",
            manualAction,
            ReasonCode::PatternQualityFail,
            "IMPULSE_CENSORSHIP",
            "impulse=" + std::to_string(static_cast<int>(currentImpulse)),
            WallClockNowUs());
        RejectIntentTicket(manualAction, ReasonCode::PatternQualityFail, false);
        return;
    }

    // === PHASE 2c.1: TS2 HURST REGIME FILTER (60m wave persistence guard) ===
    auto* ts2HurstInd = IndicatorManager::Instance().GetIndicator<HurstExponentIndicator>(IndicatorKey::HURST_EXPONENT);
    const HurstExponentEnum ts2HurstState =
        ts2HurstInd ? ts2HurstInd->Value() : HurstExponentEnum::RANDOM_WALK;
    const Ts2HurstRegimeDecision ts2HurstDecision = EvaluateTs2HurstRegime(ts2HurstState);

    if (!ts2HurstDecision.allow) {
        SCString hurstLog;
        hurstLog.Format("Manual entry rejected | TS2 Hurst Guard | State=%s",
                        Ts2HurstStateToString(ts2HurstState));
        Logger::getInstance().log(hurstLog.GetChars());
        LogOrderFailure(
            "MANUAL",
            manualAction,
            ReasonCode::PatternQualityFail,
            "TS2_HURST_GUARD",
            "state=" + std::string(Ts2HurstStateToString(ts2HurstState)),
            WallClockNowUs());
        RejectIntentTicket(manualAction, ReasonCode::PatternQualityFail, false);
        return;
    }

    // Manual entries use full confidence (operator override)
    float confidence = 1.0f;
    if (biasResult.multiplier_adjustment != 1.0) {
        confidence = std::clamp(confidence * (float)biasResult.multiplier_adjustment, 0.0f, 1.0f);
    }
    if (ts2HurstDecision.confidenceMultiplier != 1.0f) {
        confidence = std::clamp(confidence * ts2HurstDecision.confidenceMultiplier, 0.0f, 1.0f);
    }

    // === PHASE 5: MICROSTRUCTURE ADMISSION GATE ===
    const double bid = static_cast<double>(sc.Bid);
    const double ask = static_cast<double>(sc.Ask);
    const bool hasInside = (ask > bid) && (sc.TickSize > 0.0f);

    if (!hasInside) {
        Logger::getInstance().log("Manual entry rejected: No valid market (bid/ask invalid)");
        LogOrderFailure(
            "MANUAL",
            manualAction,
            ReasonCode::PolicyEvalErrorFailClosed,
            "MICROSTRUCTURE_MARKET",
            "invalid_bid_ask",
            WallClockNowUs());
        RejectIntentTicket(manualAction, ReasonCode::PolicyEvalErrorFailClosed, false);
        return;
    }

    const double spreadTicks = (ask - bid) / static_cast<double>(sc.TickSize);

    // Session-aware spread gate (delegated to helper)
    const float maxSpreadTicks = GetSessionSpreadLimit();

    if (spreadTicks > static_cast<double>(maxSpreadTicks)) {
        SCString logMsg;
        logMsg.Format("Manual entry rejected: Spread too wide | %.2f ticks > %.1f ticks max",
                      spreadTicks, maxSpreadTicks);
        Logger::getInstance().log(logMsg.GetChars());
        LogOrderFailure(
            "MANUAL",
            manualAction,
            ReasonCode::SpreadBreach,
            "MICROSTRUCTURE_SPREAD",
            "spread_ticks=" + std::to_string(spreadTicks) + " limit_ticks=" + std::to_string(maxSpreadTicks),
            WallClockNowUs());
        RejectIntentTicket(manualAction, ReasonCode::SpreadBreach, false);
        return;
    }

    // Toxic flow gate (delegated to VolumeIndicator)
    const auto* volInd = IndicatorManager::Instance().GetIndicator<VolumeIndicator>(IndicatorKey::VOLUME_SIGNAL);
    const double imbalance = volInd ? static_cast<double>(volInd->GetVolumeImbalance()) : 0.0;

    if ((isLong && imbalance < -0.35) || (!isLong && imbalance > 0.35)) {
        SCString logMsg;
        logMsg.Format("Manual entry rejected: Toxic flow imbalance | %.2f", imbalance);
        Logger::getInstance().log(logMsg.GetChars());
        LogOrderFailure(
            "MANUAL",
            manualAction,
            ReasonCode::ToxicFlowBreach,
            "MICROSTRUCTURE_FLOW",
            "imbalance=" + std::to_string(imbalance),
            WallClockNowUs());
        RejectIntentTicket(manualAction, ReasonCode::ToxicFlowBreach, false);
        return;
    }

    if (IsQuoteChurnBreached(sc, bid, ask)) {
        SCString logMsg;
        logMsg.Format("Manual entry rejected: Quote churn breach | %.2f events/sec", m_quoteChurnEmaPerSec);
        Logger::getInstance().log(logMsg.GetChars());
        LogOrderFailure(
            "MANUAL",
            manualAction,
            ReasonCode::QuoteChurnBreach,
            "MICROSTRUCTURE_CHURN",
            "quote_churn_ema_per_sec=" + std::to_string(m_quoteChurnEmaPerSec),
            WallClockNowUs());
        RejectIntentTicket(manualAction, ReasonCode::QuoteChurnBreach, false);
        return;
    }

    // === PHASE 6: POSITION SIZING (RiskManager size calculator) ===
    // Operator requested quantity is treated as a preference, but RiskManager's calculation is authoritative
    auto positionSizeResult = RiskManager::Instance().CalculateSafePositionSize(sc, entryPrice, stopPrice, confidence);

    if (!positionSizeResult.isOk()) {
        Logger::getInstance().log("Manual entry rejected: " + positionSizeResult.error());
        LogOrderFailure(
            "MANUAL",
            manualAction,
            ReasonCode::PolicyEvalErrorFailClosed,
            "POSITION_SIZING",
            positionSizeResult.error(),
            WallClockNowUs());
        RejectIntentTicket(manualAction, ReasonCode::PolicyEvalErrorFailClosed, false);
        return;
    }

    int adjustedQuantity = positionSizeResult.unwrap();

    if (adjustedQuantity <= 0) {
        Logger::getInstance().log("Manual entry: Order size suppressed (adjusted qty = 0)");
        LogOrderFailure(
            "MANUAL",
            manualAction,
            ReasonCode::PolicyEvalErrorFailClosed,
            "POSITION_SIZING",
            "adjusted_quantity<=0",
            WallClockNowUs());
        RejectIntentTicket(manualAction, ReasonCode::PolicyEvalErrorFailClosed, false);
        return;
    }

    // Log sizing decision for transparency (requested vs approved)
    if (static_cast<uint32_t>(adjustedQuantity) != quantity) {
        SCString logMsg;
        logMsg.Format("PositionManager: Manual order size adjusted | requested=%d -> approved=%d",
                      quantity, adjustedQuantity);
        Logger::getInstance().log(logMsg.GetChars());
    }

    // === PHASE 7: FINAL VALIDATION (RiskManager validator chain) ===
    auto validationResult = RiskManager::Instance().ValidateOrder(
        sc,
        entryPrice,
        stopPrice,
        adjustedQuantity,
        0,  // No transformer latency
        0,  // No regime latency
        0   // No inference latency
    );

    if (!validationResult.isOk()) {
        Logger::getInstance().log("Manual entry rejected: " + validationResult.error());
        LogOrderFailure(
            "MANUAL",
            manualAction,
            ReasonCode::PolicyEvalErrorFailClosed,
            "FINAL_VALIDATION",
            validationResult.error(),
            WallClockNowUs());
        RejectIntentTicket(manualAction, ReasonCode::PolicyEvalErrorFailClosed, false);
        return;
    }

    // === PHASE 8: ORDER SUBMISSION (Bracket order with TCA logging) ===
    s_SCNewOrder order;
    order.OrderQuantity = adjustedQuantity;
    order.Price1 = entryPrice;
    order.OrderType = SCT_ORDERTYPE_LIMIT_CHASE;
    order.MaximumChaseAsPrice = 2.0 * sc.TickSize;
    order.TimeInForce = SCT_TIF_DAY;

    // GAP 25: fat-tail chase cap (mirrors automatic path)
    {
        const auto& lrcChase = ContextManager::Instance().GetLocalRiskContext();
        const auto* hmmChase = InferenceManager::Instance().HmmState();
        const float chaseDof = hmmChase ? hmmChase->Dof() : 30.0f;
        const float chaseKurtosis = lrcChase.isValid ? lrcChase.talebKurtosis : 3.0f;
        if (chaseDof <= 4.0f || chaseKurtosis > 10.0f) {
            order.MaximumChaseAsPrice = 0.0;  // Pure limit — no chase in crash regime
        }
    }

    // TextTag: intent ID + pattern name for SC Trade Activity Log identification
    {
        SCString tag;
        tag.Format("I:%llu|MANUAL|%s", static_cast<unsigned long long>(intentId), patternName.c_str());
        order.TextTag = tag;
    }

    // Bracket Stop — regime-aware STATIC stop type (mirrors automatic path Step C)
    {
        const auto& lrc = ContextManager::Instance().GetLocalRiskContext();
        const auto* hmmStop = InferenceManager::Instance().HmmState();
        const float dof = hmmStop ? hmmStop->Dof() : 30.0f;
        const float kurtosis = lrc.isValid ? lrc.talebKurtosis : 3.0f;

        const bool crashRegime = (dof <= 4.0f) || (kurtosis > 10.0f) ||
                                 (lrc.isValid && lrc.amihudPercentile > 0.90f);

        if (crashRegime) {
            order.AttachedOrderStop1Type = SCT_ORDERTYPE_STOP_WITH_BID_ASK_TRIGGERING;
        } else {
            order.AttachedOrderStop1Type = SCT_ORDERTYPE_STOP_LIMIT;
            order.StopLimitOrderLimitOffsetForAttachedOrders = 2.0 * sc.TickSize;
        }
    }
    order.Stop1Price = stopPrice;

    // Bracket Target — single full-size first-touch (mirrors automatic path Step C).
    // No trailing runner, no move-to-breakeven (removed in the Phase 1 cutover).
    order.AttachedOrderTarget1Type = SCT_ORDERTYPE_LIMIT_CHASE;
    order.Target1Price = targetPrice;
    order.AttachedOrderMaximumChase = 2.0 * sc.TickSize;

    int orderResult = isLong ? static_cast<int>(sc.BuyOrder(order)) : static_cast<int>(sc.SellOrder(order));

    if (orderResult > 0) {
        // Populate working order tracking
        m_pendingEntryOrder.active = true;
        m_pendingEntryOrder.orderId = orderResult;
        m_pendingEntryOrder.requestedQuantity = adjustedQuantity;
        m_pendingEntryOrder.isLong = isLong;
        m_pendingEntryOrder.submitPrice = entryPrice;
        m_pendingEntryOrder.executionStyle = "MANUAL";  // Manual directives tracked separately
        m_pendingEntryOrder.repriceCount = 0;
        m_pendingEntryOrder.submitTime = sc.CurrentSystemDateTime;
        m_pendingEntryOrder.lastRepriceTime = sc.CurrentSystemDateTime;

        m_openTrade.SetPattern("Manual", 0, patternName);
        m_openTrade.SetConfidence(confidence);
        TransitionIntentTicket("EXECUTED", ReasonCode::NA);

        // === TCA: LOG ORDER SUBMIT for manual entry ===
        AIConnectionMonitor::Instance().LogOrderSubmit(
            sc,
            orderResult,
            entryPrice,
            isLong,
            entryPrice,         // decisionPrice = entry price for manual
            static_cast<float>(bid),
            static_cast<float>(ask),
            "MANUAL"            // executionStyle
        );

        SCString successMsg;
        successMsg.Format("Manual Order SUBMITTED: ID=%d, Pattern=%s, Qty=%d, Entry=%.2f, Stop=%.2f, Target=%.2f, Spread=%.2f",
                          orderResult, patternName.c_str(), adjustedQuantity, entryPrice, stopPrice, targetPrice, spreadTicks);
        Logger::getInstance().log(successMsg.GetChars());
    } else {
        DetectBrokerSubmitFault(orderResult, manualAction);
        Logger::getInstance().log("PositionManager: Manual order submission failed");
        LogOrderFailure(
            "MANUAL",
            manualAction,
            ReasonCode::IbDisconnected,
            "ORDER_SUBMIT",
            "sc_order_submission_failed order_result=" + std::to_string(orderResult),
            WallClockNowUs());
        RejectIntentTicket(manualAction, ReasonCode::IbDisconnected, false);
    }
}

// ============================================================

void PositionManager::CancelAllWorkingOrders(SCStudyInterfaceRef sc) {
    // AI disconnect fast-purge: Cancel all working orders
    // This prevents orphaned limit orders from being filled after AI fails

    // Sierra Chart's CancelAllOrders() cancels ALL working orders in the chart
    // This is aggressive but safe - prevents any orphaned fills
    int cancelResult = sc.CancelAllOrders();
    m_pendingEntryOrder = {};
    ClearIntentTicket(ReasonCode::IntentCancelled);

    if (cancelResult > 0) {
        SCString msg;
        msg.Format("AI DISCONNECT: Cancelled %d working order(s) (fast-purge)",
                   cancelResult);
        Logger::getInstance().log(msg.GetChars());

        // If we're flat, ensure m_openTrade state is clean
        // (order may have been submitted but not filled, leaving stale pattern data)
        if (IsFlat()) {
            m_openTrade.Reset(sc);
        }
    } else {
        // Result could be 0 (no orders to cancel) or negative (error)
        SCString msg;
        msg.Format("AI DISCONNECT: CancelAllOrders completed, result=%d (0=no orders, <0=error)",
                   cancelResult);
        Logger::getInstance().log(msg.GetChars());
    }
}

// ============================================================
// Elite GAP 5: Emergency Flatten Position
// ============================================================

void PositionManager::EmergencyFlattenPosition(SCStudyInterfaceRef sc, const char* reason) {
    // EMERGENCY PROTOCOL: Cancel all orders AND flatten any open position
    // This is the nuclear option for critical system failures (AI disconnect, model soft-lock)

    // First-hit guard: a deterministic exit is being submitted this tick.
    m_exitSubmittedThisTick = true;

    SCString logMsg;
    logMsg.Format("EMERGENCY FLATTEN TRIGGERED: %s", reason);
    Logger::getInstance().log(logMsg.GetChars());

    // Taleb: Immediately invalidate PredictionState to prevent stale predictions
    // from re-entering after the flatten completes.  Direction determined by
    // current position; if flat, ForceStandAside prevents any new entry.
    auto* pred = InferenceManager::Instance().MutablePrediction();
    s_SCPositionData posCheck;
    sc.GetTradePosition(posCheck);
    if (pred) {
        if (posCheck.PositionQuantity != 0) {
            pred->ForceExit(posCheck.PositionQuantity > 0);
        } else {
            pred->ForceStandAside();
        }
    }

    // Step 1: Cancel ALL working orders (prevents orphaned fills)
    int cancelResult = sc.CancelAllOrders();
    m_pendingEntryOrder = {};
    ClearIntentTicket(ReasonCode::HardSafetyViolation);
    if (cancelResult > 0) {
        logMsg.Format("   ├─ Cancelled %d working order(s)", cancelResult);
        Logger::getInstance().log(logMsg.GetChars());
    }

    // Step 2: Check if we have an open position to flatten
    s_SCPositionData pos;
    sc.GetTradePosition(pos);

    if (pos.PositionQuantity != 0) {
        // Position exists - flatten it immediately
        int flattenResult = sc.FlattenAndCancelAllOrders();

        if (flattenResult > 0) {
            logMsg.Format("   ├─ Flattened %s position (%d contracts)",
                         pos.PositionQuantity > 0 ? "LONG" : "SHORT",
                         abs(pos.PositionQuantity));
            Logger::getInstance().log(logMsg.GetChars());

            // Reset trade state after flatten
            m_openTrade.Reset(sc);
        } else {
            // C3 fix: FlattenAndCancelAllOrders failed — attempt explicit market exit
            logMsg.Format("   +-- FlattenAndCancelAllOrders failed (result=%d), attempting explicit exit", flattenResult);
            Logger::getInstance().log(logMsg.GetChars());

            s_SCNewOrder exitOrder;
            exitOrder.OrderQuantity = abs(pos.PositionQuantity);
            exitOrder.OrderType = SCT_ORDERTYPE_MARKET;
            exitOrder.TimeInForce = SCT_TIF_GOOD_TILL_CANCELED;

            int exitResult = (pos.PositionQuantity > 0)
                ? static_cast<int>(sc.SellOrder(exitOrder))
                : static_cast<int>(sc.BuyOrder(exitOrder));

            if (exitResult > 0) {
                logMsg.Format("   ├─ Explicit market exit submitted (order=%d)", exitResult);
                Logger::getInstance().log(logMsg.GetChars());
                m_openTrade.Reset(sc);
            } else {
                // Both flatten methods failed — escalate to RiskManager halt
                logMsg.Format("   +-- BOTH FLATTEN METHODS FAILED -- escalating to EmergencyHalt");
                Logger::getInstance().log(logMsg.GetChars());
                RiskManager::Instance().EmergencyHalt(sc, "EmergencyFlatten: all exit methods failed");
            }
        }
    } else {
        Logger::getInstance().log("   ├─ No position to flatten (already flat)");
    }

    // C15 fix: Halt trading after emergency flatten to prevent re-entry into dangerous conditions
    RiskManager::Instance().EmergencyHalt(sc, reason);

    Logger::getInstance().log("   └─ Emergency flatten complete, trading halted");

    // Play loud audible alert (5 repetitions - cannot be ignored)
    for (int i = 0; i < 5; i++) {
        sc.PlaySound(9);  // Alert sound 9 = loud alert
    }
}

// Native TRAP floor (priority #1): completed-bar StructureTest reversal against the
// open position -> immediate neutral market close. Deterministic, model-independent
// (no Python/inference dependency), the parity anchor with the labeler's TRAP outcome.
// TRAP = sprung-trap REVERSAL only (Q0 split, ruling 2026-07-15): FAILED_* against the
// position side. An adverse DECISIVE_* counter-break is REGIME_INVALIDATION, NOT TRAP,
// and is intentionally excluded here. Completed-bar (sc.Index-1) for train/live parity;
// evaluated at most once per completed bar and only for bars strictly after entry.
void PositionManager::EvaluateNativeTrapFloor(SCStudyInterfaceRef sc) {
    if (IsFlat() || m_exitSubmittedThisTick) {
        return;
    }
    // Fire at most once per completed bar.
    if (sc.Index == m_lastTrapEvalBarIndex) {
        return;
    }
    m_lastTrapEvalBarIndex = sc.Index;

    // Only consider bars that completed strictly AFTER the entry bar (the labeler's
    // lookahead scan starts forward from entry; the entry bar and earlier are excluded).
    if ((sc.Index - 1) <= m_openTrade.GetEntryIndex()) {
        return;
    }

    auto* stInd = IndicatorManager::Instance().StructureTest();
    if (!stInd) {
        return;
    }
    const StructureTest st = stInd->CompletedValue();
    const bool isLong = IsLong();

    const bool adverse =
        isLong ? (st == StructureTest::FAILED_HIGH_CLOSE_INSIDE ||
                  st == StructureTest::FAILED_HIGH_STRONG_REVERSAL)
               : (st == StructureTest::FAILED_LOW_CLOSE_INSIDE ||
                  st == StructureTest::FAILED_LOW_STRONG_REVERSAL);
    if (!adverse) {
        return;
    }

    SCString msg;
    msg.Format("[TRAP] native floor: completed-bar StructureTest=%d adverse to %s position -> close at market (priority #1)",
               static_cast<int>(st), isLong ? "LONG" : "SHORT");
    Logger::getInstance().log(msg.GetChars());

    ClosePositionAtMarket(sc, "TRAP");
    TripleBarrierExitManager::getInstance().Close();
}

// Neutral deterministic market close for Triple-Barrier resolutions (e.g. the
// vertical/time barrier). Unlike EmergencyFlattenPosition this carries NO emergency
// semantics: no trading halt, no alarms, no prediction force-exit, and it does NOT
// reset the trade — the resulting fill flows through HandleFills' normal close path
// so the trade is recorded with the explicit exit-reason tag (not price-inferred).
void PositionManager::ClosePositionAtMarket(SCStudyInterfaceRef sc, const char* exitTag) {    s_SCPositionData pos;
    sc.GetTradePosition(pos);
    if (pos.PositionQuantity == 0) {
        return;  // already flat
    }

    m_exitSubmittedThisTick = true;

    // Tag the reason BEFORE the exit fill is processed, so the .btst TradeRecord
    // records it deterministically instead of inferring from exit price.
    m_openTrade.SetExitReasonTag(exitTag ? exitTag : "");

    SCString logMsg;
    logMsg.Format("TRIPLE-BARRIER CLOSE [%s]: flatten %s position (%d contracts)",
                  exitTag ? exitTag : "", pos.PositionQuantity > 0 ? "LONG" : "SHORT",
                  abs(pos.PositionQuantity));
    Logger::getInstance().log(logMsg.GetChars());

    int flattenResult = sc.FlattenAndCancelAllOrders();
    if (flattenResult <= 0) {
        // Fallback: explicit market exit (mirrors EmergencyFlattenPosition's fallback).
        s_SCNewOrder exitOrder;
        exitOrder.OrderQuantity = abs(pos.PositionQuantity);
        exitOrder.OrderType = SCT_ORDERTYPE_MARKET;
        exitOrder.TimeInForce = SCT_TIF_GOOD_TILL_CANCELED;
        int exitResult = (pos.PositionQuantity > 0)
            ? static_cast<int>(sc.SellOrder(exitOrder))
            : static_cast<int>(sc.BuyOrder(exitOrder));
        if (exitResult <= 0) {
            logMsg.Format("   +-- Triple-Barrier close FAILED (flatten=%d, market=%d) -- escalating to emergency",
                          flattenResult, exitResult);
            Logger::getInstance().log(logMsg.GetChars());
            EmergencyFlattenPosition(sc, "TripleBarrierClose: exit methods failed");
        }
    }
}

// ============================================================
// Elder Grade-Based Profit Protection
// ============================================================

void PositionManager::UpdateTradeGradeProtection(SCStudyInterfaceRef sc) {
    // === ELITE GAP: TIME - STALE FISH EXIT (Raschke/Shannon) ===
    // "Information creates value. Time destroys information."
    if (m_openTrade.GetStatus() == TradeStatusEnum::OPEN && m_openTrade.GetEntryIndex() > 0) {
        // Fetch HMM state once for all checks in this function
        auto* hmmStateInd = InferenceManager::Instance().HmmState();

        // -------------------------------------------------------------
        // P1.2: PER-BAR MAHALANOBIS EMERGENCY FLATTEN (Taleb+Elder)
        // Logic lives in HmmStateIndicator::IsOutlierEmergency().
        // -------------------------------------------------------------
        if (hmmStateInd && hmmStateInd->IsOutlierEmergency()) {
            SCString logMsg;
            logMsg.Format("P1.2 MAHALANOBIS EMERGENCY: mahal=%.2f, dof=%.1f. FLATTEN!",
                          hmmStateInd->Mahalanobis(), hmmStateInd->Dof());
            Logger::getInstance().log(logMsg.GetChars());
            EmergencyFlattenPosition(sc, "Mahalanobis Outlier — Mid-Trade Emergency");
            return;
        }

        // -------------------------------------------------------------
        // ELITE: TAIL RISK / FLASH CRASH PROTECTION (Taleb)
        // C18 fix: Check every bar, not just entry bar.
        // Attached stop provides primary protection, but gaps/limit-down
        // can blow through stops. This is the backup gate.
        // -------------------------------------------------------------
        int barsHeld = sc.Index - m_openTrade.GetEntryIndex();
        {
            const float atr = m_cachedATR14;
            if (atr > 0.0f) {
                double currentPrice = sc.Close[sc.Index];
                double entryPrice = m_openTrade.GetEntryPrice();
                double adverseMove = (m_openTrade.GetSide() == TradeSideEnum::LONG)
                                     ? (entryPrice - currentPrice)
                                     : (currentPrice - entryPrice);

                // 3.0 ATR adverse move = extreme dislocation (distribution-free, no Gaussian assumption).
                if (adverseMove > (3.0 * atr)) {
                    SCString logMsg;
                    logMsg.Format("🚨 TAIL RISK EVENT: >3 ATR (%.2f vs %.2f ATR) bar %d. EMERGENCY FLATTEN!",
                                  adverseMove, 3.0 * atr, barsHeld);
                    Logger::getInstance().log(logMsg.GetChars());

                    EmergencyFlattenPosition(sc, "Tail Risk / Flash Crash Detected");
                    return;
                }
            }
        }

        // === STALE-FISH TIME EXIT — REMOVED (Phase 1 first-touch cutover) ===
        // The Triple-Barrier VERTICAL barrier (maxBars, latched at entry via
        // TripleBarrierExitManager::OpenBracket and enforced in Update()) is now the
        // single, labeler-reproducible time exit. A second, differently-thresholded
        // time exit here would race it and break train/live parity. The "cut trades
        // going nowhere" intent belongs in the labeler's vertical-barrier definition
        // (or a future meta-label feature), not as a live-only heuristic.
        // The Mahalanobis + >3·ATR tail-risk flattens above are retained safety gates.
    }

    int currentTradeGrade = m_openTrade.GetTradeGrade();

    // Only act on grade improvements (don't trigger multiple times for same grade level)
    if (currentTradeGrade <= m_lastTradeGradeAction) {
        return;
    }

    // === C3: REGIME-CONDITIONAL ELDER GRADE THRESHOLDS (Taleb+Elder) ===
    // Momentum regimes raise thresholds (patient holding, let winners run).
    // Fragile/chaos regimes lower them (exit faster, asymmetric cost of holding).
    const auto gt = InferenceManager::GetRegimeGradeThresholds(m_currentHMMState, m_currentClimate);

    // === A-GRADE: Telemetry Only (no in-trade action) ===
    // Rationale: Grade A scale-out conflicts with bracket targets (T1/T2/T3) and
    // Chandelier trailing — two profit-measurement systems (R-multiples vs channel-%)
    // racing each other. Demoted to post-trade attribution via TradeClose FlatBuffer.
    if (currentTradeGrade >= gt.gradeA && m_lastTradeGradeAction < gt.gradeA) {
        SCString logMsg;
        logMsg.Format("TRADE GRADE A (%d%%): Captured %d%%+ of channel [telemetry]",
                      currentTradeGrade, gt.gradeA);
        Logger::getInstance().log(logMsg.GetChars());
        m_lastTradeGradeAction = gt.gradeA;
    }

    // === B-GRADE: Telemetry Only (no in-trade action) ===
    // Rationale: BE+1R stop move conflicts with Chandelier regime-aware trailing.
    // Both systems move stops in different units; letting both race creates inconsistency.
    // Demoted to post-trade attribution via TradeClose FlatBuffer.
    else if (currentTradeGrade >= gt.gradeB && m_lastTradeGradeAction < gt.gradeB) {
        SCString logMsg;
        logMsg.Format("TRADE GRADE B (%d%%): Captured %d%%+ of channel [telemetry]",
                      currentTradeGrade, gt.gradeB);
        Logger::getInstance().log(logMsg.GetChars());
        m_lastTradeGradeAction = gt.gradeB;
    }

    // === C-GRADE: Telemetry Only (breakeven stop-move REMOVED — Phase 1 cutover) ===
    // Moving the stop to breakeven is a profit-lock heuristic permanently rejected by
    // the profit-protection ruling (whipsaw trap on tight fade stops; see
    // triple_barrier_profit_protection_ruling.md). The stop is immutable at the engine
    // barrier; winner give-back is the accepted control-group baseline.
    else if (currentTradeGrade >= gt.gradeC && m_lastTradeGradeAction < gt.gradeC) {
        SCString logMsg;
        logMsg.Format("TRADE GRADE C (%d%%): Captured %d%%+ of channel [telemetry]",
                      currentTradeGrade, gt.gradeC);
        Logger::getInstance().log(logMsg.GetChars());
        m_lastTradeGradeAction = gt.gradeC;
    }
}




// 3040
