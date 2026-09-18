#include "MindfulTrader_Precompiled.h"
#include "RegimeManager.h"
#include "TripleBarrierExitManager.h"  // ToRegime() (SC-dependent wrapper, .cpp-only)

#include <cstdio>

std::string RegimeSnapshot::ToOpaqueString() const {
    // Opaque on purpose: a numeric token, not the enum's name — callers must
    // route decisions back through RegimeManager, never parse this string.
    char buf[24];
    std::snprintf(buf, sizeof(buf), "REGIME_SNAPSHOT_%d", static_cast<int>(m_state));
    return std::string(buf);
}

RegimeManager& RegimeManager::Instance() {
    static RegimeManager singletonInstance;
    return singletonInstance;
}

HMMStateEnum RegimeManager::CurrentState() const {
    const auto* hmmInd = InferenceManager::Instance().HmmState();
    return hmmInd ? hmmInd->Value() : HMM_NO_PRIOR;
}

::MarketClimate RegimeManager::CurrentClimate() const {
    const auto* climateInd = InferenceManager::Instance().MarketClimate();
    return climateInd ? climateInd->Value() : ::MarketClimate::GAUSSIAN_STABLE;
}

bool RegimeManager::IsMomentumAligned(bool isBullish) const {
    const HMMStateEnum state = CurrentState();
    const bool inMomentumSet = (state == HMMStateEnum::PARETO_MOMENTUM ||
                                 state == HMMStateEnum::GAUSSIAN_STABLE);
    if (!inMomentumSet) return false;
    // Short-vs-PARETO_MOMENTUM hostility (InferenceManager::IsHostileRegimeChange's
    // own convention): a bearish thesis isn't momentum-aligned in a thrust regime.
    if (!isBullish && state == HMMStateEnum::PARETO_MOMENTUM) return false;
    return true;
}

bool RegimeManager::IsMeanReversionAligned(bool isBullish) const {
    const HMMStateEnum state = CurrentState();
    const bool inMeanRevSet = (state == HMMStateEnum::COILED_SPRING ||
                                state == HMMStateEnum::GAUSSIAN_FRAGILE);
    if (!inMeanRevSet) return false;
    // Long-vs-GAUSSIAN_FRAGILE hostility (same convention, mirrored).
    if (isBullish && state == HMMStateEnum::GAUSSIAN_FRAGILE) return false;
    return true;
}

double RegimeManager::GetPatternMultiplier(PatternType pattern) const {
    return Scoring::Instance().GetHMMMultiplier(pattern, CurrentState());
}

bool RegimeManager::RequiresPassiveExecution() const {
    return CurrentState() == HMMStateEnum::GAUSSIAN_FRAGILE;
}

bool RegimeManager::GrantsMomentumBoost() const {
    return CurrentState() == HMMStateEnum::PARETO_MOMENTUM;
}

int RegimeManager::GetStaleFishThreshold(int barsHeld, float earlyProfitAtr) const {
    return InferenceManager::GetStaleFishBarThreshold(CurrentState(), barsHeld, earlyProfitAtr);
}

InferenceManager::GradeThresholds RegimeManager::GetRegimeGradeThresholds() const {
    return InferenceManager::GetRegimeGradeThresholds(CurrentState(), CurrentClimate());
}

tbe::Regime RegimeManager::GetTripleBarrierExitParams() const {
    return TripleBarrierExitManager::ToRegime(CurrentState());
}

RegimeSnapshot RegimeManager::GetCurrentSnapshot() const {
    return RegimeSnapshot(CurrentState());
}

bool RegimeManager::IsHostileRegimeChange(const RegimeSnapshot& entry, bool isLong) const {
    return InferenceManager::IsHostileRegimeChange(entry.m_state, CurrentState(), isLong);
}

bool RegimeManager::IsBurnInComplete() const {
    return CurrentState() != HMM_NO_PRIOR;
}
