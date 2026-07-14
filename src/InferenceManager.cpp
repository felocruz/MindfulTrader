#include "MindfulTrader_Precompiled.h"
#include "InferenceManager.h"
#include "RiskManager.h"

InferenceManager& InferenceManager::Instance() {
    static InferenceManager singletonInstance;
    return singletonInstance;
}

InferenceManager::InferenceManager() = default;

// ── Regime Tenure (UI-thread only) ──

void InferenceManager::UpdateRegimeTenure() {
    // Snapshot the atomic HMM state once to avoid TOCTOU within this method.
    const int snapshotState = static_cast<int>(m_hmmState.Value());

    if (m_lastHmmStateId == -1) {
        m_lastHmmStateId = snapshotState;
        m_regimeTenure = 0;
    } else if (snapshotState != m_lastHmmStateId) {
        m_lastHmmStateId = snapshotState;
        m_regimeTenure = 0;
    } else {
        ++m_regimeTenure;
    }
}

// ── HMM Freshness ──

void InferenceManager::MarkHmmStateUpdated(uint64_t nowUs) {
    m_hmmLastUpdateUs.store(nowUs, std::memory_order_relaxed);
}

uint64_t InferenceManager::HmmStateAgeUs(uint64_t nowUs) const {
    const uint64_t lastUpdate = m_hmmLastUpdateUs.load(std::memory_order_relaxed);
    if (lastUpdate == 0 || nowUs <= lastUpdate) return UINT64_MAX;
    return nowUs - lastUpdate;
}

bool InferenceManager::IsHmmStateStale(uint64_t nowUs, uint64_t maxAgeUs) const {
    return HmmStateAgeUs(nowUs) > maxAgeUs;
}

// ── Training Event Export ──

void InferenceManager::AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const {
    m_hmmState.AddToTrainingEventFB(event);
    m_prediction.AddToTrainingEventFB(event);
    m_climate.AddToTrainingEventFB(event);
}

// ── Consolidated Inference Policy Methods ──

bool InferenceManager::IsInDefensiveMode() const {
    const float risk = std::clamp(m_hmmState.TransitionRisk(), 0.0f, 1.0f);
    const float threshold = RiskManager::Instance().GetTransitionRiskDefensiveThreshold();
    return risk > threshold;
}

bool InferenceManager::IsHighTransitionRisk() const {
    const float risk = std::clamp(m_hmmState.TransitionRisk(), 0.0f, 1.0f);
    const float threshold = RiskManager::Instance().GetTransitionRiskCriticalThreshold();
    return risk > threshold;
}

bool InferenceManager::IsHostileRegimeChange(HMMStateEnum prev, HMMStateEnum curr, bool isLong) {
    if (prev == curr || prev == HMM_NO_PRIOR) return false;
    return (isLong  && curr == HMMStateEnum::GAUSSIAN_FRAGILE) ||
           (!isLong && curr == HMMStateEnum::PARETO_MOMENTUM);
}

bool InferenceManager::IsCriticalClimateShift(::MarketClimate curr, ::MarketClimate prev) {
    if (curr == prev) return false;
    return curr == ::MarketClimate::TALEBIAN_FRAGILE ||
           curr == ::MarketClimate::SHANNON_CHAOS;
}

bool InferenceManager::HasDirectionConflict(bool orderIsLong, TradeActionEnum predAction) {
    const bool predLong  = (predAction == TradeActionEnum::ENTER_LONG  ||
                            predAction == TradeActionEnum::HOLD_LONG);
    const bool predShort = (predAction == TradeActionEnum::ENTER_SHORT ||
                            predAction == TradeActionEnum::HOLD_SHORT);
    const bool predNeutral = IsExitAction(predAction) ||
                             predAction == TradeActionEnum::STAND_ASIDE;

    return (orderIsLong  && (predShort || predNeutral)) ||
           (!orderIsLong && (predLong  || predNeutral));
}

// Budget caps are compile-time constants tied to regime semantics, not operator-tunable
// policy.  TALEBIAN_FRAGILE (100ms) reflects the empirical observation that fat-tail
// regimes resolve adverse fills within ~80ms on ES.  These belong in code, not JSON,
// because changing them requires understanding the microstructure argument.
int InferenceManager::GetRegimeExecutionBudgetCap(::MarketClimate climate) {
    switch (climate) {
        case ::MarketClimate::TALEBIAN_FRAGILE: return 100;
        case ::MarketClimate::SHANNON_CHAOS:    return 150;
        case ::MarketClimate::PARETO_MOMENTUM:  return 350;
        case ::MarketClimate::COILED_SPRING:    return 400;
        case ::MarketClimate::GAUSSIAN_STABLE:
        default:                                return 500;
    }
}

// Target width rationale: extreme regimes produce wider directional moves worth
// capturing on T2/T3.  SHANNON_CHAOS gets the widest (1.6×) because entries are
// rare and high-conviction.  Values derived from 2024-25 backtest P&L attribution.
float InferenceManager::GetRegimeTargetWidthScale(::MarketClimate climate) {
    switch (climate) {
        case ::MarketClimate::PARETO_MOMENTUM:  return 1.3f;
        case ::MarketClimate::TALEBIAN_FRAGILE: return 1.5f;
        case ::MarketClimate::SHANNON_CHAOS:    return 1.6f;
        case ::MarketClimate::COILED_SPRING:    return 1.2f;
        case ::MarketClimate::GAUSSIAN_STABLE:
        default:                                return 1.0f;
    }
}

int InferenceManager::GetStaleFishBarThreshold(HMMStateEnum state, int barsHeld, float earlyProfitAtr) {
    // Defaults: 24 bars for most regimes (Raschke "stale fish" heuristic applied to
    // 15-min bars = 6 hours of holding time before time-decay concern).
    int staleFishBars = 24;
    switch (state) {
        case HMMStateEnum::COILED_SPRING:    staleFishBars = 16; break; // compression decays faster
        case HMMStateEnum::GAUSSIAN_FRAGILE: staleFishBars = 12; break; // fragile/toxic resolves fast
        default: break;
    }
    // P3.3: Momentum continuation extension — PARETO_MOMENTUM with demonstrated
    // early profit (>0.5 ATR in first 4 bars) is genuine trend, not noise.
    // Extend to 48 bars (12 hours) to ride the flush.
    if (state == HMMStateEnum::PARETO_MOMENTUM && barsHeld > 4 && earlyProfitAtr > 0.5f) {
        staleFishBars = 48;
    }
    return staleFishBars;
}

InferenceManager::GradeThresholds InferenceManager::GetRegimeGradeThresholds(HMMStateEnum state, ::MarketClimate climate) {
    // Elder canonical defaults: A=30, B=20, C=10 (channel-percentage thresholds).
    // Regime shifts the thresholds:
    //   Momentum (PARETO_MOMENTUM): raise — let winners run.
    //   Fragile/Chaos (TALEBIAN_FRAGILE, SHANNON_CHAOS, GAUSSIAN_FRAGILE): lower — exit faster.
    // HMM state overrides climate when they conflict (HMM is higher-frequency signal).

    // Start with climate-level adjustment
    int a = 30, b = 20, c = 10;
    switch (climate) {
        case ::MarketClimate::PARETO_MOMENTUM:
            a = 35; b = 25; c = 15;
            break;
        case ::MarketClimate::TALEBIAN_FRAGILE:
            a = 22; b = 15; c = 8;
            break;
        case ::MarketClimate::SHANNON_CHAOS:
            a = 20; b = 12; c = 7;
            break;
        default:
            break;
    }

    // HMM state override: finer-grained than climate
    switch (state) {
        case HMMStateEnum::PARETO_MOMENTUM:
            // Momentum flush — be patient, raise thresholds
            a = std::max(a, 35); b = std::max(b, 25); c = std::max(c, 15);
            break;
        case HMMStateEnum::GAUSSIAN_FRAGILE:
            // Institutional flow — resolve quickly, lower thresholds
            a = std::min(a, 22); b = std::min(b, 15); c = std::min(c, 8);
            break;
        default:
            break;
    }

    return {a, b, c};
}

bool InferenceManager::IsPredictionFresh(uint64_t nowUs, uint64_t maxAgeUs) const {
    return m_prediction.IsFresh(nowUs, maxAgeUs);
}
