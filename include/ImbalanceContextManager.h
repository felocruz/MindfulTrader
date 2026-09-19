// ImbalanceContextManager.h -- centralized computer for the 4 activity-clock
// dims consumed by IS1/IS2/IS3 (architecture spec docs/superpowers/specs/
// 2026-09-18-predator-sniper-execution-architecture.md §1.1a/§1.2c/§1.2d/
// §1.4/§1.5). Mirrors ContextManager's ROLE (assembling an observation struct
// for the .context/.imbalance.context write) but deliberately NOT its full
// machinery -- no Mahalanobis triggers, no Lock A-E readiness gates, no HMM
// routing. A full ImbalanceEventDataCollectorStudy.cpp with its own readiness
// gates (§1.6) is separate, later work.
//
// DOD from day one (operator directive, 2026-09-07): flat SoA array + index
// enum + compile-time-devirtualized template accessors, mirroring this repo's
// own established IndicatorManager::SetValue<Key>() idiom
// (include/IndicatorManager.h) and ContextManager's own OBSERVATION_VECTOR_SIZE/
// OBS_* index convention (include/ContextManager.h) -- not 4 named float
// members + 4 named bools, even though the vector is small today.
//
// Centralized computation (§1.2c, RESOLVED 2026-09-07, SUPERSEDES the earlier
// per-screen-computes-its-own-dim + cascade-generation-stamp freshness design):
// Update() is called directly by the cascade (from ImbalanceScreen1.cpp's tick
// handler, immediately after ImbalanceClockManager::OnTick()) and computes ALL
// 4 dims itself, in one deterministic sequential function. IS1/IS2/IS3 no
// longer compute anything -- they become pure, precedence-agnostic DISPLAY
// studies that only read this manager's already-computed vector. This makes
// same-tick freshness structural, not merely detectable: there is no window
// where "some dims are this tick's, some are last tick's" can occur, since all
// 4 are computed in one function, one call, sequential statements, with no
// other code path able to interleave.
//
// Individual pure compute functions (§1.2d): each takes explicit array+count
// inputs and returns a value -- no hidden singleton reads, no side effects,
// mirroring this repo's own existing math headers (DfaHurstExponent.h,
// RobustMoments.h, RecurrenceRateEngine.h) exactly. Only Update() itself
// touches ImbalanceClockManager::Instance() to pull the 3 returns buffers --
// every compute function stays fully unit-testable with synthetic arrays.
// Carry-forward (NaN on a degenerate window -> caller must reuse the last
// valid value) is centralized and generic via ApplyWithCarryForward<Index>(),
// a real DRY win only possible once computation is centralized -- the
// contract is identical across all 4 dims.
//
// Pure, ACSIL-independent (no SCStudyInterfaceRef dependency) -- only depends
// on the generated FlatBuffers schema struct, ImbalanceClockManager, and this
// repo's existing pure math headers, all themselves ACSIL-independent.
#pragma once

#include "generated/mts_schema_generated.h"
#include "ImbalanceClockManager.h"
#include "DfaHurstExponent.h"
#include "RobustMoments.h"
#include "RecurrenceRateEngine.h"
#include "RQAEpsilonSelector.h"
#include "InformationEngine.h"

#include <array>
#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>

// Shannon flow entropy is measured in BITS (range 0 .. log2(NUM_BINS)) -- same convention as
// ContextManager.h's kShannonMaxEntropyBits, duplicated here (not included) to preserve this
// header's ACSIL independence (ContextManager.h pulls in sierrachart.h).
inline constexpr float kImbalanceShannonMaxEntropyBits = 3.321928f;  // log2(10)
static_assert(MindfulTrader::InformationEngine::NUM_BINS == 10,
              "kImbalanceShannonMaxEntropyBits must equal log2(InformationEngine::NUM_BINS)");

// hurst_exponent -> IS1: same DfaHurstExponent call convention as
// ContextManager.cpp's fast_hurst_exponent (length=100, minScale=8).
inline float ComputeImbalanceHurstExponent(const float* returns, std::size_t count) {
    if (count < 100) return std::numeric_limits<float>::quiet_NaN();
    return DfaHurstExponent(returns, 100, 8);
}

// recurrence_rate -> IS2: epsilon selected via SelectEpsilonForTargetRecurrenceRate
// (target RR=0.05), O(n^2) window rebuild only when a NEW IS2 bar has closed
// (NaN otherwise -- carried forward by the caller via ApplyWithCarryForward,
// same "cached until next bar close" behavior as the original per-screen code,
// just expressed through the generic carry-forward mechanism instead of a
// dedicated cache variable).
inline float ComputeImbalanceRecurrenceRate(const float* returns, std::size_t count,
                                             std::size_t completedBarCount,
                                             RecurrenceRateEngine& engine,
                                             std::size_t& lastBarCount) {
    if (count < 100) return std::numeric_limits<float>::quiet_NaN();
    if (completedBarCount == lastBarCount) return std::numeric_limits<float>::quiet_NaN();
    const float epsilon = static_cast<float>(SelectEpsilonForTargetRecurrenceRate(returns, 100, 0.05));
    engine.RebuildClosedBarWindow(returns, 99, epsilon);
    lastBarCount = completedBarCount;
    return engine.ComputeRate(returns[99], epsilon);
}

// skewness_idx + taleb_kurtosis -> IS3: same BowleySkewness/MoorsKurtosis
// convention as ContextManager.cpp's own skewness_idx/fast_taleb_kurtosis.
struct ImbalanceSkewKurt {
    float skewness;
    float kurtosis;
};

inline ImbalanceSkewKurt ComputeImbalanceSkewnessKurtosis(const float* returns, std::size_t count) {
    if (count < 100) {
        return {std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::quiet_NaN()};
    }
    std::array<float, 100> window;
    std::copy(returns, returns + 100, window.begin());
    return {BowleySkewness(window), MoorsKurtosis(window)};
}

class ImbalanceContextManager {
public:
    // Index order matches ImbalanceObservationData's own field order
    // (schema/mts_schema.fbs) -- keep both in lockstep if either changes.
    enum ImbalanceObsIndex : std::size_t {
        IMBALANCE_OBS_HURST_EXPONENT = 0,   // IS1
        IMBALANCE_OBS_RECURRENCE_RATE = 1,  // IS2
        IMBALANCE_OBS_SKEWNESS_IDX = 2,     // IS3
        IMBALANCE_OBS_TALEB_KURTOSIS = 3,   // IS3
        IMBALANCE_OBSERVATION_VECTOR_SIZE = 4
    };

    static ImbalanceContextManager& Instance() {
        static ImbalanceContextManager instance;
        return instance;
    }

    void Reset() {
        m_values = kDefaultValues;
        m_lastValid = kDefaultValues;
        m_recurrenceEngine.Reset();
        m_recurrenceLastBarCount = 0;
        m_entropyEngine.Reset();
        m_entropyLastBarCount = 0;
        m_shannonFlowEntropy = 0.0f;
        m_shannonEfficiency = 0.5f;
        m_updated = false;
    }

    // Called once per tick, directly by the cascade (IS1's tick handler, right after
    // ImbalanceClockManager::OnTick()) -- computes all 4 dims in one deterministic,
    // sequential pass: hurst_exponent (IS1 returns) -> recurrence_rate (IS2 returns)
    // -> skewness_idx + taleb_kurtosis (IS3 returns), per §1.2c.
    void Update() {
        float is1Returns[ImbalanceClockManager::kAggregateBufferCapacity];
        const std::size_t is1Count = ImbalanceClockManager::Instance().GetIs1Returns(100, is1Returns);
        ApplyWithCarryForward<IMBALANCE_OBS_HURST_EXPONENT>(
            ComputeImbalanceHurstExponent(is1Returns, is1Count));

        float is2Returns[ImbalanceClockManager::kAggregateBufferCapacity];
        const std::size_t is2Count = ImbalanceClockManager::Instance().GetIs2Returns(100, is2Returns);
        const std::size_t is2CompletedBarCount = ImbalanceClockManager::Instance().GetIs2CompletedBarCount();
        ApplyWithCarryForward<IMBALANCE_OBS_RECURRENCE_RATE>(
            ComputeImbalanceRecurrenceRate(is2Returns, is2Count, is2CompletedBarCount,
                                           m_recurrenceEngine, m_recurrenceLastBarCount));

        float is3Returns[ImbalanceClockManager::kAggregateBufferCapacity];
        const std::size_t is3Count = ImbalanceClockManager::Instance().GetIs3Returns(100, is3Returns);
        const ImbalanceSkewKurt skewKurt = ComputeImbalanceSkewnessKurtosis(is3Returns, is3Count);
        ApplyWithCarryForward<IMBALANCE_OBS_SKEWNESS_IDX>(skewKurt.skewness);
        ApplyWithCarryForward<IMBALANCE_OBS_TALEB_KURTOSIS>(skewKurt.kurtosis);

        // shannon_flow_entropy/shannon_efficiency -> IS2 (§1.3b): InformationEngine is
        // inherently INCREMENTAL (EMA-based rolling sigma + ring-buffer histograms that advance
        // on every AddObservation() call), unlike the 4 dims above which recompute from scratch
        // over a fresh snapshot every tick -- feeding it the same historical return repeatedly
        // would corrupt its state, so it's fed exactly ONE new observation per newly-completed
        // IS2 bar, same completedBarCount-gated pattern as recurrence_rate above.
        if (is2CompletedBarCount != m_entropyLastBarCount) {
            float newestIs2Return[1];
            ImbalanceClockManager::Instance().GetIs2Returns(1, newestIs2Return);
            m_entropyEngine.AddObservation(static_cast<double>(newestIs2Return[0]));
            m_entropyLastBarCount = is2CompletedBarCount;

            m_shannonFlowEntropy = static_cast<float>(m_entropyEngine.GetShannonEntropy());
            m_shannonEfficiency = (m_shannonFlowEntropy > 0.0f)
                ? (1.0f - std::min(m_shannonFlowEntropy / kImbalanceShannonMaxEntropyBits, 1.0f))
                : 0.5f;
        }

        m_updated = true;
    }

    // Read-only accessor for the pure-display screens (IS1/IS2/IS3 no longer compute
    // anything themselves per §1.2c) -- compile-time-devirtualized, same idiom as
    // IndicatorManager::GetValue<Key>().
    template <ImbalanceObsIndex Index>
    float GetDim() const {
        static_assert(Index < IMBALANCE_OBSERVATION_VECTOR_SIZE, "ImbalanceObsIndex out of range");
        return m_values[Index];
    }

    // True once Update() has been called at least once -- freshness is now structural
    // (§1.2c), not per-dim-tracked: by the time any LOW_PREC_LEVEL reader (IS2/IS3/the
    // collector) runs, IS1's STD_PREC_LEVEL Update() call has already completed this
    // exact tick, so a plain "ever updated" flag is sufficient and correct.
    bool AllDimsReady() const { return m_updated; }

    MTS::Schema::ImbalanceObservationData BuildObservation() const {
        return MTS::Schema::ImbalanceObservationData(
            m_values[IMBALANCE_OBS_HURST_EXPONENT],
            m_values[IMBALANCE_OBS_RECURRENCE_RATE],
            m_values[IMBALANCE_OBS_SKEWNESS_IDX],
            m_values[IMBALANCE_OBS_TALEB_KURTOSIS]);
    }

    // shannon_flow_entropy in bits, shannon_efficiency in [0,1] -- IS2-owned (§1.3b), read by
    // the Gang-MACD/Phase-Coherence indicator (§1.3a) for its dH_norm/dτ input.
    float GetShannonFlowEntropy() const { return m_shannonFlowEntropy; }
    float GetShannonEfficiency() const { return m_shannonEfficiency; }

    // Raw, unscaled activity-clock risk-gate inputs (§1.5a) -- mirrors RiskGateContext's own
    // table-not-struct, populate-on-write convention.
    MTS::Schema::ImbalanceRiskGateContextT BuildRiskGateContext(uint64_t timestamp_us) const {
        MTS::Schema::ImbalanceRiskGateContextT ctx;
        ctx.shannon_flow_entropy = m_shannonFlowEntropy;
        ctx.shannon_efficiency = m_shannonEfficiency;
        ctx.hurst_exponent = m_values[IMBALANCE_OBS_HURST_EXPONENT];
        ctx.skewness_idx = m_values[IMBALANCE_OBS_SKEWNESS_IDX];
        ctx.taleb_kurtosis = m_values[IMBALANCE_OBS_TALEB_KURTOSIS];
        ctx.is_valid = m_updated;
        ctx.snapshot_timestamp_us = static_cast<int64_t>(timestamp_us);
        return ctx;
    }

private:
    ImbalanceContextManager() { Reset(); }

    // Generic carry-forward (§1.2d): NaN on a degenerate/insufficient-data window ->
    // reuse the last valid value; identical contract across all 4 dims, so this is a
    // real DRY win only possible once computation is centralized.
    template <ImbalanceObsIndex Index>
    void ApplyWithCarryForward(float raw) {
        static_assert(Index < IMBALANCE_OBSERVATION_VECTOR_SIZE, "ImbalanceObsIndex out of range");
        if (std::isfinite(raw)) {
            m_values[Index] = raw;
            m_lastValid[Index] = raw;
        } else {
            m_values[Index] = m_lastValid[Index];
        }
    }

    // Neutral seeds, matching ContextManager.cpp's own carry-forward defaults
    // for the same dims (0.5f hurst, 1.23f taleb_kurtosis).
    static constexpr std::array<float, IMBALANCE_OBSERVATION_VECTOR_SIZE> kDefaultValues = {
        0.5f, 0.0f, 0.0f, 1.23f
    };

    std::array<float, IMBALANCE_OBSERVATION_VECTOR_SIZE> m_values = kDefaultValues;
    std::array<float, IMBALANCE_OBSERVATION_VECTOR_SIZE> m_lastValid = kDefaultValues;

    // recurrence_rate's own extra state (§1.2d): a genuinely different-in-kind concern
    // (performance caching, not NaN-handling) -- stays a dedicated member, not folded
    // into the generic arrays above.
    RecurrenceRateEngine m_recurrenceEngine;
    std::size_t m_recurrenceLastBarCount = 0;

    // shannon_flow_entropy/shannon_efficiency's own state (§1.3b): InformationEngine is a
    // genuinely stateful, non-trivial-sized engine (parallel P/Q histograms, ring buffers),
    // not a one-line pure function -- same "dedicated member, not generalized" precedent as
    // m_recurrenceEngine above.
    MindfulTrader::InformationEngine m_entropyEngine;
    std::size_t m_entropyLastBarCount = 0;
    float m_shannonFlowEntropy = 0.0f;
    float m_shannonEfficiency = 0.5f;

    bool m_updated = false;
};

