#include "PositionManager.h"
#include <algorithm>
#include <array>
#include <cmath>
#include "HMMClient.h"
#include "IndicatorManager.h"
#include "RiskManager.h"
#include "Scoring.h"
#include "RiskManager.h"

// ====================================================================
// ELITE v3.2: UNIFIED HARD GATE ENFORCEMENT
// Three-state dispatch based on position state:
//   1) FLAT, no working orders → block (silent, entries rejected later)
//   2) FLAT, working orders    → cancel all working orders
//   3) IN POSITION             → emergency flatten
// Returns true if gates passed, false if violation triggered action.
// ====================================================================

bool PositionManager::EnforceHardGates(SCStudyInterfaceRef sc) {
    const auto& ctx = ContextManager::Instance().GetLocalRiskContext();
    auto result = RiskManager::Instance().EvaluateHardGates(ctx);

    if (result.isOk()) {
        return true; // All gates passed
    }

    const std::string& reason = result.error();

    if (IsFlat()) {
        // Check for working orders via O(1) position data query (not O(n) order iteration)
        s_SCPositionData posData;
        sc.GetTradePosition(posData);
        const bool hasWorkingOrders = (posData.WorkingOrdersExist != 0);

        if (hasWorkingOrders) {
            // State 2: FLAT with working orders → cancel them
            CancelAllWorkingOrders(sc);
            Logger::getInstance().log("HARD_GATE ENFORCED (flat+orders): " + reason);
        }
        // State 1: FLAT, no orders → silent block (ProcessPendingPrediction will check gates)
        return false;
    }

    // State 3: IN POSITION → emergency flatten
    Logger::getInstance().log("HARD_GATE ENFORCED (in-position): " + reason);
    EmergencyFlattenPosition(sc, reason.c_str());
    return false;
}

// ====================================================================
// Elite v2.5: Context-Aware Position Management Helpers
// ====================================================================

// Copies the live HMM + MarketClimate indicator values into m_previous*/m_current*.
// Pure state sync — no side effects. Called at the top of Update() every tick so
// all in-position consumers (UpdateTradeGradeProtection, EvaluateRegimeDefense) read
// fresh regime. Indicators are refreshed by ContextManager::CheckAndTriggerHMM(),
// which precedes Update() in both SCStudies and BackTesterStudy.
void PositionManager::SyncRegimeState() {
    if (auto* hmmInd = InferenceManager::Instance().HmmState()) {
        m_previousHMMState = m_currentHMMState;
        m_currentHMMState  = hmmInd->Value();
    }
    if (auto* climateInd = InferenceManager::Instance().MarketClimate()) {
        m_previousClimate = m_currentClimate;
        m_currentClimate  = climateInd->Value();
    }
}

// Event-driven entry point (SystemOrchestrator on regime change). Retained so an
// out-of-band regime event can force an immediate defense pass; shares the one
// refresh implementation. NOT called from the per-tick Update() path (that uses
// SyncRegimeState() directly to avoid a double EvaluateRegimeDefense).
void PositionManager::UpdateContext(SCStudyInterfaceRef sc) {
    SyncRegimeState();
    if (!IsFlat()) {
        EvaluateRegimeDefense(sc);
    }
}

void PositionManager::EvaluateRegimeDefense(SCStudyInterfaceRef sc) {
    if (IsFlat()) return;

    const bool isLong = IsLong();

    // === GAP 8: HMM REGIME AS INDEPENDENT EXIT SIGNAL (Taleb — asymmetric cost) ===
    // A regime flip to a directly hostile regime is a hard exit gate independent of
    // pattern scoring.  The cost of being wrong in a hostile regime is asymmetrically
    // worse than the cost of exiting a still-valid position.
    //   Long position + regime → GAUSSIAN_FRAGILE (short-side institutional flow) → flatten
    //   Short position + regime → PARETO_MOMENTUM (momentum breakout upward) → flatten
    if (m_currentHMMState != m_previousHMMState && m_previousHMMState != HMM_NO_PRIOR) {
        if (InferenceManager::IsHostileRegimeChange(m_previousHMMState, m_currentHMMState, isLong)) {
            SCString logMsg;
            logMsg.Format("HOSTILE REGIME EXIT: %s position vs regime %d (prev=%d -> new=%d)",
                isLong ? "LONG" : "SHORT", static_cast<int>(m_currentHMMState),
                static_cast<int>(m_previousHMMState), static_cast<int>(m_currentHMMState));
            Logger::getInstance().log(logMsg.GetChars());
            EmergencyFlattenPosition(sc, "HOSTILE_REGIME_EXIT");
            return;
        }
    }

    // === GAP 5: CLIMATE-SHIFT MID-TRADE HARD GATE (Taleb — phase transition) ===
    // Climate shifts are rare binary events. Previously this force-activated Chandelier
    // trailing; that actuator was removed in the first-touch cutover. The static barrier
    // now holds through the shift; escalation to a regime-invalidation FLATTEN is a
    // flagged follow-up (tbe::Resolution::REGIME_INVALIDATION). Telemetry only.
    if (InferenceManager::IsCriticalClimateShift(m_currentClimate, m_previousClimate)) {
        SCString logMsg;
        logMsg.Format("CLIMATE-SHIFT [telemetry]: %s",
            m_currentClimate == MarketClimate::TALEBIAN_FRAGILE ? "TALEBIAN_FRAGILE" : "SHANNON_CHAOS");
        Logger::getInstance().log(logMsg.GetChars());
    }

    // Convert open trade pattern name string to PatternType enum
    PatternType pattern = Scoring::Instance().StringToPatternType(m_openTrade.GetPatternName());
    if (pattern == PatternType::Unknown) return;

    // Calculate Multipliers
    double hmmMult = Scoring::Instance().GetHMMMultiplier(pattern, m_currentHMMState);
    double climateMult = Scoring::Instance().GetClimateMultiplier(pattern, m_currentClimate);

    // ELITE v3.2: Deep Context from ContextManager's unified LocalRiskContext
    double deepContextMult = 1.0;
    const auto& riskCtx = ContextManager::Instance().GetLocalRiskContext();
    if (riskCtx.isValid) {
        deepContextMult = Scoring::Instance().GetDeepContextMultiplier(pattern, riskCtx);
    }

    // Composite Health Score for HOLDING (fixed max of 4 factors: hmm, climate,
    // deep context, and an optional regime-stability factor below).
    std::array<double, 4> holdingFactors{};
    size_t holdingFactorCount = 0;
    holdingFactors[holdingFactorCount++] = hmmMult;
    holdingFactors[holdingFactorCount++] = climateMult;
    holdingFactors[holdingFactorCount++] = deepContextMult; // Add deep context multiplier

    // Regime stability factor: short expected_duration → lower holding score
    // Logic lives in HmmStateIndicator::HoldingStabilityFactor().
    auto* hmmDurInd = InferenceManager::Instance().HmmState();
    if (hmmDurInd) {
        const double stabilityFactor = hmmDurInd->HoldingStabilityFactor();
        if (stabilityFactor < 1.0) {
            holdingFactors[holdingFactorCount++] = stabilityFactor;
        }
    }

    double holdingScore = Scoring::Instance().CalculateEliteCompositeMultiplier(holdingFactors.data(), holdingFactorCount);
    const float toxicThreshold = RiskManager::Instance().GetLiveToxicScoreThreshold();
    const float hostileThreshold = RiskManager::Instance().GetLiveHostileScoreThreshold();

    // === DEFENSE LOGIC ===

    // Level 1: Toxic Environment -> EMERGENCY EXIT
    if (holdingScore < toxicThreshold) {
        SCString logMsg;
        logMsg.Format("TOXIC EXIT: Regime/Climate incompatible with %s | HMM=%d (%.2fx) | Climate=%d (%.2fx) | DeepContext=%.2fx | Score=%.4f | threshold=%.2f",
            m_openTrade.GetPatternName().c_str(), static_cast<int>(m_currentHMMState), hmmMult,
            static_cast<int>(m_currentClimate), climateMult, deepContextMult, holdingScore, toxicThreshold);

        Logger::getInstance().log(logMsg.GetChars());
        EmergencyFlattenPosition(sc, "TOXIC_ENV_EXIT");
        return;
    }

    // Level 2: Hostile Environment -> TELEMETRY ONLY (was: force Chandelier trailing)
    // First-touch cutover removed trailing; the static barrier holds. Escalation to a
    // regime-invalidation flatten is a flagged follow-up (tbe::Resolution::REGIME_INVALIDATION).
    if (holdingScore < hostileThreshold) {
        SCString logMsg;
        logMsg.Format("HOSTILE ENV [telemetry]: DeepContext=%.2fx | Score=%.4f | threshold=%.2f",
            deepContextMult, holdingScore, hostileThreshold);
        Logger::getInstance().log(logMsg.GetChars());
    }
}

// ====================================================================
// PATTERN-BASED PRICE CALCULATION HELPERS
// ====================================================================

bool PositionManager::CalculateTacticalTriggerPrices(SCStudyInterfaceRef sc, const int patternId, const bool /*isLong*/,
                                                      const float atr, float& entryPrice, float& stopPrice,
                                                      float& targetPrice) const {
    const auto trigger = static_cast<RaschkeTacticalTrigger>(patternId);

    const int idx = sc.Index;
    const float close = sc.Close[idx];
    const float high = sc.High[idx];
    const float low = sc.Low[idx];

    // Pattern-specific stop/target multipliers (constexpr for compile-time optimization)
    constexpr float TURTLE_SOUP_STOP_BUFFER = 0.5f;
    constexpr float PINBALL_STOP_MULTIPLIER = 0.4f;
    constexpr int TURTLE_SOUP_LOOKBACK = 4;
    constexpr int PINBALL_SWING_LOOKBACK = 10;

    switch (trigger) {

        // ===== TURTLE SOUP PATTERNS =====

        case RaschkeTacticalTrigger::TURTLE_SOUP_BUY:
        {
            // Price broke below 4-day low then closed back above it (failed breakout)
            entryPrice = close;
            stopPrice = low - (TURTLE_SOUP_STOP_BUFFER * atr);

            // Calculate 4-day high using efficient lookup
            const int lookbackStart = std::max(0, idx - TURTLE_SOUP_LOOKBACK);
            targetPrice = *std::max_element(&sc.High[lookbackStart], &sc.High[idx + 1]);

            return true;
        }

        case RaschkeTacticalTrigger::TURTLE_SOUP_SELL:
        {
            entryPrice = close;
            stopPrice = high + (TURTLE_SOUP_STOP_BUFFER * atr);

            // Calculate 4-day low using efficient lookup
            const int lookbackStart = std::max(0, idx - TURTLE_SOUP_LOOKBACK);
            targetPrice = *std::min_element(&sc.Low[lookbackStart], &sc.Low[idx + 1]);

            return true;
        }

        // ===== MOMENTUM PINBALL PATTERNS =====

        case RaschkeTacticalTrigger::MOMENTUM_PINBALL_BUY:
        {
            // Extreme oversold - tight stop per Raschke methodology
            entryPrice = close;
            stopPrice = low - (PINBALL_STOP_MULTIPLIER * atr);

            // Find previous swing high using STL algorithm
            const int lookbackStart = std::max(0, idx - PINBALL_SWING_LOOKBACK);
            targetPrice = *std::max_element(&sc.High[lookbackStart], &sc.High[idx + 1]);

            return true;
        }

        case RaschkeTacticalTrigger::MOMENTUM_PINBALL_SELL:
        {
            entryPrice = close;
            stopPrice = high + (PINBALL_STOP_MULTIPLIER * atr);

            // Find previous swing low using STL algorithm
            const int lookbackStart = std::max(0, idx - PINBALL_SWING_LOOKBACK);
            targetPrice = *std::min_element(&sc.Low[lookbackStart], &sc.Low[idx + 1]);

            return true;
        }

        // ===== ELDER BREAKOUT PATTERNS =====

        case RaschkeTacticalTrigger::ELDER_BREAKOUT_BUY:
        {
            // Triple Screen System - breakout above yesterday's high after pullback
            if (idx < 1) [[unlikely]] return false;

            // 1.5R initial target — parity with tbe::kElderTargetR (Triple-Barrier engine,
            // triple_barrier_exit_engine_spec.md Reading B). Trail with 13-EMA + MACD-H later.
            constexpr float ELDER_TARGET_R_MULTIPLE = 1.5f;
            const float prevHigh = sc.High[idx - 1];

            // Determine entry: use close if already broken out, else breakout level
            entryPrice = (high > prevHigh) ? close : prevHigh + sc.TickSize;

            // Stop: Below low of past 2 bars (tight per Elder methodology)
            stopPrice = std::min(low, sc.Low[idx - 1]) - sc.TickSize;

            // Target: 1.5R initial (trail with 13-EMA + MACD-H later)
            const float riskAmount = entryPrice - stopPrice;
            targetPrice = entryPrice + (ELDER_TARGET_R_MULTIPLE * riskAmount);

            return true;
        }

        case RaschkeTacticalTrigger::ELDER_BREAKOUT_SELL:
        {
            if (idx < 1) [[unlikely]] return false;

            // 1.5R initial target — parity with tbe::kElderTargetR (Triple-Barrier engine,
            // triple_barrier_exit_engine_spec.md Reading B).
            constexpr float ELDER_TARGET_R_MULTIPLE = 1.5f;
            const float prevLow = sc.Low[idx - 1];

            entryPrice = (low < prevLow) ? close : prevLow - sc.TickSize;

            // Stop: Above high of past 2 bars
            stopPrice = std::max(high, sc.High[idx - 1]) + sc.TickSize;

            // Target: 1.5R initial
            const float riskAmount = stopPrice - entryPrice;
            targetPrice = entryPrice - (ELDER_TARGET_R_MULTIPLE * riskAmount);

            return true;
        }

        default:
            return false;  // Unknown pattern ID
    }
}

bool PositionManager::CalculateStrategySetupPrices(SCStudyInterfaceRef sc, const int patternId, const bool isLong,
                                                    const float atr, float& entryPrice, float& stopPrice,
                                                    float& targetPrice) const {
    const auto setup = static_cast<RaschkeStrategySetup>(patternId);

    const int idx = sc.Index;
    const float close = sc.Close[idx];
    const float high = sc.High[idx];
    const float low = sc.Low[idx];

    // Pattern-specific constants
    constexpr float COMPRESSION_STOP_MULTIPLIER = 0.5f;
    constexpr int NR7_LOOKBACK = 7;
    constexpr int SWING_LOOKBACK = 10;
    constexpr int HOLY_GRAIL_LOOKBACK = 20;

    switch (setup) {

        case RaschkeStrategySetup::THREE_BAR_TRIANGLE:
        case RaschkeStrategySetup::NR4:
        case RaschkeStrategySetup::NR7:
        case RaschkeStrategySetup::IDNR4:
        {
            // Compression patterns - enter on breakout from consolidation
            // Calculate compression height efficiently using transform_reduce pattern
            const int lookbackStart = std::max(0, idx - NR7_LOOKBACK);
            const int lookbackEnd = idx + 1;

            float compressionHeight = 0.0f;
            for (int i = lookbackStart; i < lookbackEnd; ++i) {
                compressionHeight = std::max(compressionHeight, sc.High[i] - sc.Low[i]);
            }

            if (isLong) {
                entryPrice = high + sc.TickSize;
                stopPrice = low - (COMPRESSION_STOP_MULTIPLIER * atr);
                targetPrice = entryPrice + compressionHeight;
            } else {
                entryPrice = low - sc.TickSize;
                stopPrice = high + (COMPRESSION_STOP_MULTIPLIER * atr);
                targetPrice = entryPrice - compressionHeight;
            }
            return true;
        }

        case RaschkeStrategySetup::TWO_B_REVERSAL:
        {
            // Failed new high/low reversal pattern
            constexpr float TWO_B_STOP_MULTIPLIER = 0.5f;
            const int lookbackStart = std::max(0, idx - SWING_LOOKBACK);

            if (isLong) {
                entryPrice = close;
                stopPrice = low - (TWO_B_STOP_MULTIPLIER * atr);

                // Target: Previous swing high using STL algorithm
                targetPrice = *std::max_element(&sc.High[lookbackStart], &sc.High[idx + 1]);
            } else {
                entryPrice = close;
                stopPrice = high + (TWO_B_STOP_MULTIPLIER * atr);

                // Target: Previous swing low using STL algorithm
                targetPrice = *std::min_element(&sc.Low[lookbackStart], &sc.Low[idx + 1]);
            }
            return true;
        }

        case RaschkeStrategySetup::HOLY_GRAIL_BUY:
        {
            // Pullback to EMA after trend established
            constexpr float HOLY_GRAIL_STOP_MULTIPLIER = 0.6f;
            const int lookbackStart = std::max(0, idx - HOLY_GRAIL_LOOKBACK);

            entryPrice = close;
            stopPrice = low - (HOLY_GRAIL_STOP_MULTIPLIER * atr);

            // Target: Previous high + 1 ATR extension
            const float prevHigh = *std::max_element(&sc.High[lookbackStart], &sc.High[idx + 1]);
            targetPrice = prevHigh + atr;

            return true;
        }

        case RaschkeStrategySetup::HOLY_GRAIL_SELL:
        {
            constexpr float HOLY_GRAIL_STOP_MULTIPLIER = 0.6f;
            const int lookbackStart = std::max(0, idx - HOLY_GRAIL_LOOKBACK);

            entryPrice = close;
            stopPrice = high + (HOLY_GRAIL_STOP_MULTIPLIER * atr);

            // Target: Previous low - 1 ATR extension
            const float prevLow = *std::min_element(&sc.Low[lookbackStart], &sc.Low[idx + 1]);
            targetPrice = prevLow - atr;

            return true;
        }

        case RaschkeStrategySetup::DOUBLE_REPO:
        {
            // Two consecutive reversal bars - tight stop, 2R target
            constexpr float DOUBLE_REPO_STOP_MULTIPLIER = 0.4f;
            constexpr float DOUBLE_REPO_TARGET_R = 2.0f;

            entryPrice = close;

            if (isLong) {
                stopPrice = low - (DOUBLE_REPO_STOP_MULTIPLIER * atr);
                const float riskAmount = entryPrice - stopPrice;
                targetPrice = entryPrice + (DOUBLE_REPO_TARGET_R * riskAmount);
            } else {
                stopPrice = high + (DOUBLE_REPO_STOP_MULTIPLIER * atr);
                const float riskAmount = stopPrice - entryPrice;
                targetPrice = entryPrice - (DOUBLE_REPO_TARGET_R * riskAmount);
            }
            return true;
        }

        case RaschkeStrategySetup::DOUBLE_REPO_FAILURE:
        {
            // Failed reversal = strong trend continuation
            constexpr float REPO_FAILURE_STOP_MULTIPLIER = 0.5f;
            constexpr float REPO_FAILURE_TARGET_R = 3.0f;

            entryPrice = close;

            if (isLong) {
                stopPrice = low - (REPO_FAILURE_STOP_MULTIPLIER * atr);
                const float riskAmount = entryPrice - stopPrice;
                targetPrice = entryPrice + (REPO_FAILURE_TARGET_R * riskAmount);
            } else {
                stopPrice = high + (REPO_FAILURE_STOP_MULTIPLIER * atr);
                const float riskAmount = stopPrice - entryPrice;
                targetPrice = entryPrice - (REPO_FAILURE_TARGET_R * riskAmount);
            }
            return true;
        }

        // Default handler for other patterns
        default:
        {
            // Generic setup: Enter on close, stop at swing point, target 2R
            constexpr float DEFAULT_STOP_MULTIPLIER = 0.5f;
            constexpr float DEFAULT_TARGET_R = 2.0f;

            entryPrice = close;

            if (isLong) {
                stopPrice = low - (DEFAULT_STOP_MULTIPLIER * atr);
                const float riskAmount = entryPrice - stopPrice;
                targetPrice = entryPrice + (DEFAULT_TARGET_R * riskAmount);
            } else {
                stopPrice = high + (DEFAULT_STOP_MULTIPLIER * atr);
                const float riskAmount = stopPrice - entryPrice;
                targetPrice = entryPrice - (DEFAULT_TARGET_R * riskAmount);
            }
            return true;
        }
    }
}
