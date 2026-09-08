// ImbalanceIndicatorManager.h -- centralized computer for activity-clock
// trading indicators (architecture spec docs/superpowers/specs/2026-09-06-
// imbalance-triple-screen-architecture-spec.md §1.3/§1.3a). Mirrors
// ImbalanceContextManager's own shape exactly (§1.2c/§1.2d): a single
// Update() orchestrator, invoked from the SAME cascade call site (IS1's tick
// handler, right after ImbalanceContextManager::Update()), computing every
// indicator in one deterministic pass -- packed int8/float arrays, not a
// heterogeneous per-indicator OOP store (mirrors IndicatorPackedState, the
// DOD read-side IndicatorManager is migrating TOWARD, explicitly NOT
// IndicatorStore/BaseIndicator/Indicator<T>, the OOP write-side it's
// migrating AWAY from).
//
// First real indicator (§1.3a): Gang-MACD / Phase Coherence, a Gemini
// literature-consult reframing (lbrnet/logs/rc_gemini.log, line 7515) of
// MACD/Impulse-System as physical operators (Phase Velocity, Thermodynamic
// Dissipation) on the imbalance clock. Lives at IS2 (matches calendar-clock
// MACD's own TS2/"INTERM_MACD" ownership, TripleScreen2.cpp) -- dP/d\u03c4 and
// dH_norm/d\u03c4 must share the same \u03c4, so this indicator's own clock is IS2,
// the same level ImbalanceContextManager's entropy engine (\u00a71.3b) samples at.
//
// Only the general-purpose 3-state classification is implemented here. The
// divergence sub-case (Thermodynamic Exhaustion -> TRAP_LONG, \u00a71.3a) is a
// separate, NOT YET WIRED follow-on: it requires cross-referencing the native
// TRAP/StructureTest framework (CLAUDE.md), out of scope for this pass.
//
// Pure, ACSIL-independent (no SCStudyInterfaceRef dependency) -- only depends
// on ImbalanceClockManager/ImbalanceContextManager, both themselves
// ACSIL-independent.
#pragma once

#include "ImbalanceClockManager.h"
#include "ImbalanceContextManager.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

// 3-state Gang-MACD/Phase-Coherence signal (§1.3a). Entropy checked FIRST --
// overrides price direction, not a bolt-on filter on top of it:
//   dH_norm/d\u03c4 > 0                    -> BLUE  (turbulent/dissipation, 0)
//   dH_norm/d\u03c4 <= 0 and dP/d\u03c4 > 0    -> GREEN (coherent bullish, +1)
//   dH_norm/d\u03c4 <= 0 and dP/d\u03c4 < 0    -> RED   (coherent bearish, -1)
//   dH_norm/d\u03c4 <= 0 and dP/d\u03c4 == 0   -> BLUE  (no direction to be coherent about)
enum class GangMacdSignal : int8_t { RED = -1, BLUE = 0, GREEN = 1 };

struct ImbalanceIndicatorSignalQuality {
    int8_t signal;
    float quality;
};

// Pure compute function (§1.2d discipline extended to indicators): explicit
// scalar inputs, no hidden singleton reads, no side effects. Quality is the
// magnitude of the phase velocity -- exact formula not finalized per §1.3a,
// this is the simplest non-invented choice, secondary to the signal logic.
inline ImbalanceIndicatorSignalQuality ComputeGangMacdPhaseCoherence(float phaseVelocity,
                                                                      float entropyTrend) {
    int8_t signal;
    if (entropyTrend > 0.0f) {
        signal = static_cast<int8_t>(GangMacdSignal::BLUE);
    } else if (phaseVelocity > 0.0f) {
        signal = static_cast<int8_t>(GangMacdSignal::GREEN);
    } else if (phaseVelocity < 0.0f) {
        signal = static_cast<int8_t>(GangMacdSignal::RED);
    } else {
        signal = static_cast<int8_t>(GangMacdSignal::BLUE);
    }
    return {signal, std::fabs(phaseVelocity)};
}

class ImbalanceIndicatorManager {
public:
    enum ImbalanceIndicatorKey : std::size_t {
        IMBALANCE_IND_GANG_MACD_PHASE_COHERENCE = 0,  // IS2
        IMBALANCE_INDICATOR_VECTOR_SIZE = 1
    };

    static ImbalanceIndicatorManager& Instance() {
        static ImbalanceIndicatorManager instance;
        return instance;
    }

    void Reset() {
        m_signals.fill(static_cast<int8_t>(GangMacdSignal::BLUE));
        m_quality.fill(0.0f);
        m_lastEntropyBarCount = 0;
        m_lastNormalizedEntropy = 0.5f;  // matches ImbalanceContextManager's own neutral
                                         // shannon_efficiency seed (H_norm = 1 - efficiency)
    }

    // Called once per tick, directly by the cascade (IS1's tick handler, right after
    // ImbalanceContextManager::Update()) -- computes every activity-clock indicator in one
    // deterministic pass, same shape as ImbalanceContextManager::Update() (§1.2c/§1.3).
    void Update() {
        // dP/d\u03c4: the latest completed IS2 bar's own log return IS the discrete phase
        // velocity by construction (return = log(P_end/P_start) for that bar, one bar = one
        // unit of \u03c4) -- no separate price-level tracking needed. GetIs2Returns() is a
        // non-destructive read, safe to call every tick regardless of whether a new bar closed.
        float latestIs2Return[1] = {0.0f};
        ImbalanceClockManager::Instance().GetIs2Returns(1, latestIs2Return);
        const float phaseVelocity = latestIs2Return[0];

        // dH_norm/d\u03c4: first difference of normalized entropy (H_norm = H/Hmax = 1 -
        // shannon_efficiency) across consecutive IS2 bar closes -- 0.0 (mapping to the spec's
        // own "<= 0" branch) on any tick where no new bar has closed, i.e. nothing new to
        // report, not an approximation of the true trend.
        const std::size_t is2CompletedBarCount = ImbalanceClockManager::Instance().GetIs2CompletedBarCount();
        float entropyTrend = 0.0f;
        const float normalizedEntropy = 1.0f - ImbalanceContextManager::Instance().GetShannonEfficiency();
        if (is2CompletedBarCount != m_lastEntropyBarCount) {
            entropyTrend = normalizedEntropy - m_lastNormalizedEntropy;
            m_lastNormalizedEntropy = normalizedEntropy;
            m_lastEntropyBarCount = is2CompletedBarCount;
        }

        SetSignalQuality<IMBALANCE_IND_GANG_MACD_PHASE_COHERENCE>(
            ComputeGangMacdPhaseCoherence(phaseVelocity, entropyTrend));
    }

    // Read-only accessors for the pure-display screens -- compile-time-devirtualized, same
    // idiom as ImbalanceContextManager::GetDim<Index>().
    template <ImbalanceIndicatorKey Key>
    int8_t GetSignal() const {
        static_assert(Key < IMBALANCE_INDICATOR_VECTOR_SIZE, "ImbalanceIndicatorKey out of range");
        return m_signals[Key];
    }
    template <ImbalanceIndicatorKey Key>
    float GetQuality() const {
        static_assert(Key < IMBALANCE_INDICATOR_VECTOR_SIZE, "ImbalanceIndicatorKey out of range");
        return m_quality[Key];
    }

private:
    ImbalanceIndicatorManager() { Reset(); }

    template <ImbalanceIndicatorKey Key>
    void SetSignalQuality(const ImbalanceIndicatorSignalQuality& sq) {
        static_assert(Key < IMBALANCE_INDICATOR_VECTOR_SIZE, "ImbalanceIndicatorKey out of range");
        m_signals[Key] = sq.signal;
        m_quality[Key] = sq.quality;
    }

    std::array<int8_t, IMBALANCE_INDICATOR_VECTOR_SIZE> m_signals{};
    std::array<float, IMBALANCE_INDICATOR_VECTOR_SIZE> m_quality{};

    // Gang-MACD/Phase-Coherence's own entropy-trend state -- a genuinely different-in-kind
    // concern (first-difference tracking, not NaN-handling), same "dedicated member, not
    // generalized" precedent as ImbalanceContextManager's m_recurrenceEngine/m_entropyEngine.
    std::size_t m_lastEntropyBarCount = 0;
    float m_lastNormalizedEntropy = 0.5f;
};
