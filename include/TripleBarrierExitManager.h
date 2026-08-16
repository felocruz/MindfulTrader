// TripleBarrierExitManager.h — Phase 1 live-integration glue for the
// Triple-Barrier Exit Engine (docs/ADR/triple_barrier_exit_engine_spec.md §5.1).
//
// This is the thin, SC-dependent wrapper around the pure, native-tested core
// (`tbe::ComputeBarriers`, TripleBarrierEngine.h). It maps live engine enums
// (RaschkeTacticalTrigger, HMMStateEnum) into the core's plain inputs, owns the
// single active position's immutable bracket, and resolves the first-hit-wins
// race each tick (regime-invalidation → vertical → lower → upper, §4.4).
//
// STATUS (post Phase 1 cutover): wired into PositionManager and authoritative —
// `PositionManager.cpp`'s "STEP C" block unconditionally overwrites the pattern
// stop/target with this engine's output before the SC bracket order is built.
// `ChandelierStopManager` was deleted entirely as part of this cutover (see
// docs/ADR/triple_barrier_cutover_phase1_plan.md). The barrier MATH lives in the
// pure core and is verified natively against the golden-vector fixture; this
// layer is validated at integration level (build + replay), per the
// cross-compilation testing posture.

#pragma once

#include "sierrachart.h"          // build/PCH consistency
#include "Indicator.h"            // RaschkeTacticalTrigger, HMMStateEnum, HMM_NO_PRIOR
#include "TripleBarrierEngine.h"  // pure core (SC-free)

#include <cstdint>

class TripleBarrierExitManager {
public:
    static TripleBarrierExitManager& getInstance();

    // --- Live-enum → pure-core mappings (see §5.2 / HMMStateEnum note) ---

    /// HMMStateEnum → tbe::Regime. Values align 1:1 (0/1/2/3); the HMM_NO_PRIOR
    /// (-1) sentinel folds to GAUSSIAN_STABLE (the documented absent/default →
    /// 25-bar horizon, §4.3).
    static tbe::Regime ToRegime(HMMStateEnum s) noexcept {
        if (s == HMM_NO_PRIOR) return tbe::Regime::GAUSSIAN_STABLE;
        return static_cast<tbe::Regime>(static_cast<std::int8_t>(s));
    }

    /// RaschkeTacticalTrigger → pattern id (1..18); values match kPatternTable indices.
    static int ToPatternId(RaschkeTacticalTrigger t) noexcept {
        return static_cast<int>(t);
    }

    /// Single active-position bracket (concentrated single-instrument book).
    struct Bracket {
        bool            active        = false;
        int             positionId    = -1;
        int             patternId     = 0;
        bool            isLong        = true;
        int             entryBarIndex = -1;
        double          stop          = 0.0;
        double          target        = 0.0;
        int             maxBars       = 25;
        tbe::Resolution resolution    = tbe::Resolution::OPEN;
    };

    /// Compute + latch the immutable bracket at entry (barriers fixed here).
    const Bracket& OpenBracket(int positionId, int entryBarIndex,
                               const tbe::BarrierInputs& in) noexcept {
        const tbe::Barriers b = tbe::ComputeBarriers(in);
        m_bracket = Bracket{};
        m_bracket.active        = true;
        m_bracket.positionId    = positionId;
        m_bracket.patternId     = in.pattern_id;
        m_bracket.isLong        = in.is_long;
        m_bracket.entryBarIndex = entryBarIndex;
        m_bracket.stop          = b.stop;
        m_bracket.target        = b.target;
        m_bracket.maxBars       = b.max_bars;
        m_bracket.resolution    = tbe::Resolution::OPEN;
        return m_bracket;
    }

    /// First-hit-wins resolution on the current tick (§4.4). `regimeInvalidated`
    /// is the caller-supplied kill-switch decision (Phase 2 wires the O(1)
    /// `.context` check here). Returns OPEN if no barrier is touched.
    tbe::Resolution Evaluate(int currentBarIndex, double currentPrice,
                             bool regimeInvalidated) noexcept {
        if (!m_bracket.active) return tbe::Resolution::OPEN;

        if (regimeInvalidated)
            return Resolve(tbe::Resolution::REGIME_INVALIDATION);

        if (currentBarIndex - m_bracket.entryBarIndex >= m_bracket.maxBars)
            return Resolve(tbe::Resolution::TIME_EXIT);

        if (m_bracket.isLong) {
            if (currentPrice <= m_bracket.stop)   return Resolve(tbe::Resolution::STOP_HIT);
            if (currentPrice >= m_bracket.target) return Resolve(tbe::Resolution::TARGET_HIT);
        } else {
            if (currentPrice >= m_bracket.stop)   return Resolve(tbe::Resolution::STOP_HIT);
            if (currentPrice <= m_bracket.target) return Resolve(tbe::Resolution::TARGET_HIT);
        }
        return tbe::Resolution::OPEN;
    }

    const Bracket& Current() const noexcept { return m_bracket; }
    void Close() noexcept { m_bracket = Bracket{}; }

private:
    TripleBarrierExitManager() = default;
    ~TripleBarrierExitManager() = default;
    TripleBarrierExitManager(const TripleBarrierExitManager&) = delete;
    TripleBarrierExitManager& operator=(const TripleBarrierExitManager&) = delete;

    tbe::Resolution Resolve(tbe::Resolution r) noexcept {
        m_bracket.resolution = r;
        m_bracket.active = false;
        return r;
    }

    Bracket m_bracket;
};
