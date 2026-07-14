#pragma once

#include "Indicator.h"
#include <chrono>

/**
 * InferenceManager — Single authority for ML inference state on the C++ side.
 *
 * Owns:
 *   - HmmStateIndicator      (Student-t HMM regime state, risk metrics)
 *   - PredictionState         (Transformer action, confidence, latency)
 *   - MarketClimateIndicator  (Derived macro climate from HMM + physics)
 *   - Regime tenure tracking  (bars since HMM state changed)
 *   - HMM freshness tracking  (microsecond wallclock of last HMM update)
 *
 * ## Threading Contract
 *
 * Writers (ZMQ worker threads):
 *   - HMMClient::HandleBinaryResponse     → MutableHmmState(), MutableClimate(), MutablePrediction()
 *   - TradeExecutionServer::HandlePrediction → MutablePrediction()
 *
 * Writers (SC UI thread):
 *   - TripleScreen3::StudyCallback         → MutableClimate()->UpdateContext()
 *   - PositionManager::EmergencyFlatten    → MutablePrediction()->ForceExit/ForceStandAside()
 *
 * Readers (SC UI thread — const access only):
 *   - PositionManager, RiskManager, ChandelierStopManager, Scoring, EventSerializer
 *
 * All indicator fields use std::atomic with relaxed ordering (single-writer-per-field,
 * readers tolerate one-tick staleness).  The regime tenure counter is updated on the
 * UI thread only (no cross-thread mutation).
 *
 * ## Accessor Design
 *
 * Default accessors return const pointers — callers cannot accidentally mutate inference
 * state.  The Mutable*() family returns non-const and is restricted to known writer
 * call-sites.  If you need Mutable access from a new location, update this header's
 * threading contract comment first.
 *
 * Moved from IndicatorManager (Mar 2026): These are ML inference results,
 * not market indicators.  Separation clarifies the architectural boundary.
 */
class InferenceManager {
public:
    static InferenceManager& Instance();

    // ── Read-Only Accessors (default — all readers use these) ──
    [[nodiscard]] const HmmStateIndicator*      HmmState()      const { return &m_hmmState; }
    [[nodiscard]] const PredictionState*         Prediction()    const { return &m_prediction; }
    [[nodiscard]] const MarketClimateIndicator*  MarketClimate() const { return &m_climate; }

    // ── Mutable Accessors (writers only — see threading contract above) ──
    HmmStateIndicator*      MutableHmmState()   { return &m_hmmState; }
    PredictionState*        MutablePrediction()  { return &m_prediction; }
    MarketClimateIndicator* MutableClimate()     { return &m_climate; }

    // ── Regime Tenure (bars since HMM state last changed, UI-thread only) ──
    [[nodiscard]] int GetRegimeTenure() const { return m_regimeTenure; }

    /// Call once per bar (from IndicatorManager::UpdateBarContext, UI thread only)
    /// to track how many bars the current HMM state has persisted.
    /// Snapshots HMM state atomically and updates tenure counter.
    void UpdateRegimeTenure();

    // ── HMM Freshness ──

    /// Returns microseconds since the last HMM state update from Python.
    /// Returns UINT64_MAX if no update has ever been received.
    [[nodiscard]] uint64_t HmmStateAgeUs(uint64_t nowUs) const;

    /// True when last HMM update is older than maxAgeUs (default 5s = 5'000'000µs).
    /// Safe-closed: returns true (stale) if no update has ever arrived.
    [[nodiscard]] bool IsHmmStateStale(uint64_t nowUs, uint64_t maxAgeUs = 5'000'000ULL) const;

    /// Called by HMMClient after writing HMM state to stamp freshness.
    void MarkHmmStateUpdated(uint64_t nowUs);

    // ── Consolidated Inference Policy Methods ──

    /// Amber gate: TransitionRisk > defensive threshold (policy-driven, default 0.30).
    /// True → tighten stops, reduce sizing.  Clamps risk to [0,1].
    /// Safe-closed: returns true if HMM state is stale.
    [[nodiscard]] bool IsInDefensiveMode() const;

    /// Red gate: TransitionRisk > critical threshold (policy-driven, default 0.60).
    /// True → cancel working orders immediately (imminent regime flip).
    [[nodiscard]] bool IsHighTransitionRisk() const;

    /// Long-vs-GAUSSIAN_FRAGILE or Short-vs-PARETO_MOMENTUM after a genuine state change.
    /// Pure inference-domain check; no chart/position data needed beyond direction.
    [[nodiscard]] static bool IsHostileRegimeChange(HMMStateEnum prev, HMMStateEnum curr, bool isLong);

    /// Transition INTO TALEBIAN_FRAGILE or SHANNON_CHAOS from a non-default climate.
    [[nodiscard]] static bool IsCriticalClimateShift(::MarketClimate curr, ::MarketClimate prev);

    /// Prediction direction conflicts with order direction → should cancel.
    [[nodiscard]] static bool HasDirectionConflict(bool orderIsLong, TradeActionEnum predAction);

    /// Climate → execution budget cap in ms.
    /// Tighter in high-volatility regimes (100ms TALEBIAN) to avoid chasing into adverse flow;
    /// wider in stable regimes (500ms GAUSSIAN) where execution is less time-critical.
    [[nodiscard]] static int GetRegimeExecutionBudgetCap(::MarketClimate climate);

    /// Climate → target width multiplier for T2/T3.
    /// ≥ 1.0: extreme regimes produce wider price moves worth capturing.
    /// T1 is never widened (capital preservation).
    [[nodiscard]] static float GetRegimeTargetWidthScale(::MarketClimate climate);

    /// Regime-conditional stale-fish bar threshold.
    /// earlyProfitAtr: profit at bar 4 expressed in ATR units (pass 0 if unavailable).
    /// P3.3: PARETO_MOMENTUM with early profit > 0.5 ATR extends to 48 bars (momentum ride).
    [[nodiscard]] static int GetStaleFishBarThreshold(HMMStateEnum state, int barsHeld, float earlyProfitAtr);

    /// Regime-conditional Elder trade-grade thresholds for profit protection.
    /// Momentum regimes raise thresholds (patient holding); fragile/chaos lower them (faster exits).
    struct GradeThresholds { int gradeA; int gradeB; int gradeC; };
    [[nodiscard]] static GradeThresholds GetRegimeGradeThresholds(HMMStateEnum state, ::MarketClimate climate);

    /// Convenience wrapper: is the current prediction fresh enough?
    [[nodiscard]] bool IsPredictionFresh(uint64_t nowUs, uint64_t maxAgeUs) const;

    // ── Training Event Export ──
    /// Populate training event fields for the three inference indicators.
    /// Called by IndicatorManager::GetTrainingEventT after its own indicator loop.
    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const;

private:
    InferenceManager();
    ~InferenceManager() = default;

    InferenceManager(const InferenceManager&) = delete;
    InferenceManager& operator=(const InferenceManager&) = delete;
    InferenceManager(InferenceManager&&) = delete;
    InferenceManager& operator=(InferenceManager&&) = delete;

    // ── ML Inference State ──
    HmmStateIndicator           m_hmmState{IndicatorKey::HMM_STATE};
    PredictionState             m_prediction{IndicatorKey::PREDICTION_STATE};
    MarketClimateIndicator      m_climate{IndicatorKey::MARKET_CLIMATE};

    // ── Temporal Tracking (UI-thread only — no cross-thread mutation) ──
    int m_regimeTenure   = 0;
    int m_lastHmmStateId = -1;   // Snapshot of HMM state enum for tenure tracking

    // ── HMM Freshness (written by ZMQ worker, read by UI thread) ──
    std::atomic<uint64_t> m_hmmLastUpdateUs{0};
};
