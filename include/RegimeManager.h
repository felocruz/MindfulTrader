// RegimeManager.h — discrete, human-auditable HMM-regime decision API.
//
// Cross-repo design doc (lbrnet, sibling repo):
// docs/superpowers/specs/2026-09-16-hmm-regime-manager-institutional-api-spec.md §4.3/§4.4.
//
// This is a NEW, ADDITIVE facade sitting atop the existing HmmStateIndicator/
// HMMStateEnum plumbing (InferenceManager, Scoring, TripleBarrierExitManager) —
// it does not replace any existing call site. Every method below is a thin,
// verified-equivalent wrapper around logic that already exists and is already
// live; no new regime-classification behavior is introduced. The 8-file
// migration of PositionManager/RiskManager/Scoring/TripleScreen3/Indicator/
// InferenceManager/Trade/TripleBarrierExitManager onto this class (spec §4.6)
// is a separate, not-yet-approved follow-on — this class currently has zero
// live callers.
//
// Python-side counterpart (2026-09-18): `lbrnet/lbrnet/models/regime_manager.py`,
// built for the labeler/data-collection pipeline, not a port of this file. Keep
// the two in sync — if the momentum/mean-reversion state groupings or the
// hostility-veto convention change here, update the Python side too (and vice
// versa); see `docs/HMM_REGIME_MANAGER_COORDINATION.md` for the handoff log.

#pragma once

#include "InferenceManager.h"
#include "Scoring.h"          // PatternType
#include "TripleBarrierEngine.h"  // tbe::Regime (pure header, no SC dependency)

#include <string>
#include <type_traits>

// RegimeSnapshot — opaque Memento token (spec §4.4).
//
// Trivially-copyable POD, zero heap allocation, safe to pass by value on the
// execution hot path. Never exposes the raw HMMStateEnum to callers — code
// that needs a decision re-evaluated later must pass the snapshot back into
// RegimeManager (e.g. `regimeManager.IsHostileRegimeChange(snapshot, isLong)`),
// never branch on it directly.
class RegimeSnapshot {
public:
    RegimeSnapshot() = default;

    bool operator==(const RegimeSnapshot& other) const { return m_state == other.m_state; }
    bool operator!=(const RegimeSnapshot& other) const { return !(*this == other); }

    /// Opaque, loggable identifier for backtest/analytics persistence
    /// (BackTesterStudy.cpp, EventSerializer.cpp) — never the raw enum name/ordinal.
    std::string ToOpaqueString() const;

private:
    friend class RegimeManager;
    explicit RegimeSnapshot(HMMStateEnum state) : m_state(state) {}
    HMMStateEnum m_state = HMM_NO_PRIOR;
};

static_assert(std::is_trivially_copyable<RegimeSnapshot>::value,
              "RegimeSnapshot must stay a zero-allocation POD (execution hot path)");
static_assert(sizeof(RegimeSnapshot) <= 16,
              "RegimeSnapshot must stay within the Memento-token size budget (spec §4.4)");

class RegimeManager {
public:
    static RegimeManager& Instance();

    // 1. DIRECTIONAL AXIS (Indicator.cpp's MarketClimateIndicator::UpdateContext
    //    isHmmMomentum/isHmmMeanRev sets: {PARETO_MOMENTUM, GAUSSIAN_STABLE} vs
    //    {COILED_SPRING, GAUSSIAN_FRAGILE}). The direction hostility veto reuses
    //    InferenceManager::IsHostileRegimeChange's existing long-vs-FRAGILE /
    //    short-vs-PARETO_MOMENTUM convention — the one place this codebase
    //    already signs a direction onto a state.
    [[nodiscard]] bool IsMomentumAligned(bool isBullish) const;
    [[nodiscard]] bool IsMeanReversionAligned(bool isBullish) const;

    // 2. TAIL-HEAVINESS / VOLATILITY AXIS — delegates to Scoring's existing,
    //    real per-pattern multiplier tables (verified intentional, not
    //    accidental — spec §3 Task 2).
    [[nodiscard]] double GetPatternMultiplier(PatternType pattern) const;

    // 3. EXECUTION / RISK OVERRIDES (PositionManager.cpp's regimeForcePassive/momentumBoost)
    [[nodiscard]] bool RequiresPassiveExecution() const;   // == GAUSSIAN_FRAGILE
    [[nodiscard]] bool GrantsMomentumBoost() const;        // == PARETO_MOMENTUM

    // 4. BESPOKE PER-STATE LOGIC (InferenceManager's existing hand-tuned constants)
    [[nodiscard]] int GetStaleFishThreshold(int barsHeld, float earlyProfitAtr = 0.0f) const;
    [[nodiscard]] InferenceManager::GradeThresholds GetRegimeGradeThresholds() const;

    // 5. TRIPLE-BARRIER EXIT DECOUPLING — delegates to TripleBarrierExitManager's
    //    existing ToRegime() (already owns the K→params mapping correctly,
    //    including the HMM_NO_PRIOR fallback); the 8-file call-site migration
    //    itself remains deferred (spec §4.6).
    [[nodiscard]] tbe::Regime GetTripleBarrierExitParams() const;

    // 6. LIFECYCLE & PERSISTENCE
    [[nodiscard]] RegimeSnapshot GetCurrentSnapshot() const;
    [[nodiscard]] bool IsHostileRegimeChange(const RegimeSnapshot& entry, bool isLong) const;
    /// Replaces Finding 3's degenerate NR7 all-4-states OR (a tautology) with
    /// the real check it was standing in for: has a live HMM state arrived yet?
    [[nodiscard]] bool IsBurnInComplete() const;

private:
    RegimeManager() = default;
    [[nodiscard]] HMMStateEnum CurrentState() const;
    [[nodiscard]] ::MarketClimate CurrentClimate() const;
};
