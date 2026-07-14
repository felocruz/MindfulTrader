// TripleBarrierEngine.h — pure, header-only barrier-computation core for the
// Triple-Barrier Exit Engine (docs/ADR/triple_barrier_exit_engine_spec.md §4, §5.3, §5.6).
//
// SCOPE (Phase 0/1): This header is the *deterministic barrier math* only — no
// Sierra Chart types, no heap, no I/O — so it is natively unit-testable against the
// shared golden-vector fixture (tests/fixtures/triple_barrier_golden_vectors.json)
// AND inlinable on the hot path. The live `TripleBarrierExitManager` singleton
// (Phase 1) will wrap `ComputeBarriers()` to own per-position state, place the
// static OCO bracket, and enforce the vertical barrier + regime-invalidation
// kill-switch each tick (§5.1). Deleting ChandelierStopManager and rewiring
// PositionManager are later Phase-1 steps, intentionally NOT in this file.
//
// Design: Reading B composition (2026-07-14, Claude + Gemini CLI code-verified) +
// the §5.6 4-step Adaptive Barrier Protocol. Pattern ids mirror
// RaschkeTacticalTrigger (Indicator.h:88-110) exactly.

#pragma once

#include <array>
#include <cmath>
#include <cstdint>

namespace tbe {

// ---------------------------------------------------------------------------
// Enums (self-contained mirrors; live code maps RaschkeTacticalTrigger/HMM ->
// these, keeping this header free of engine headers for native testability).
// ---------------------------------------------------------------------------

// Per-pattern barrier tier (Reading B, §5.3).
enum class Tier : std::uint8_t {
    LOW = 0,             // generic: entry-anchored ATR stop, R-multiple target
    HIGH_DEDICATED = 1,  // Turtle Soup / Momentum Pinball: bar-anchored stop, N-bar structural target
    ELDER = 2            // Elder Breakout: 2-bar structural stop, fixed 1.5R target
};

// HMM regime at entry -> vertical-barrier horizon (§4.3).
enum class Regime : std::uint8_t {
    COILED_SPRING = 0,
    GAUSSIAN_STABLE = 1,   // also the absent/default
    GAUSSIAN_FRAGILE = 2,
    PARETO_MOMENTUM = 3
};

// First-hit-wins resolution outcome (§4.4, §5.1).
enum class Resolution : std::uint8_t {
    OPEN = 0,
    STOP_HIT = 1,
    TARGET_HIT = 2,
    TIME_EXIT = 3,
    REGIME_INVALIDATION = 4
};

// ---------------------------------------------------------------------------
// Reconciled 9-pattern parameter table (§5.3). Indexed by RaschkeTacticalTrigger
// id (0..18). `stop_mult` is the ATR buffer (HIGH/LOW); `target_r_mult` is the
// ladder-t1 R-multiple (drives LOW targets and Elder's 1.5R; informational for
// HIGH, whose target is the structural extreme per Reading B).
// ---------------------------------------------------------------------------
struct PatternParams {
    Tier   tier;
    double stop_mult;
    double target_r_mult;
};

inline constexpr std::array<PatternParams, 19> kPatternTable = {{
    /* 0  NONE                    */ { Tier::LOW,            0.5, 1.5 },
    /* 1  KANGAROO_TAIL_BUY       */ { Tier::LOW,            0.5, 1.5 },
    /* 2  KANGAROO_TAIL_SELL      */ { Tier::LOW,            0.5, 1.5 },
    /* 3  TURTLE_SOUP_BUY         */ { Tier::HIGH_DEDICATED, 0.5, 1.0 },
    /* 4  TURTLE_SOUP_SELL        */ { Tier::HIGH_DEDICATED, 0.5, 1.0 },
    /* 5  MOMENTUM_PINBALL_BUY    */ { Tier::HIGH_DEDICATED, 0.4, 1.0 },
    /* 6  MOMENTUM_PINBALL_SELL   */ { Tier::HIGH_DEDICATED, 0.4, 1.0 },
    /* 7  ELDER_BREAKOUT_BUY      */ { Tier::ELDER,          0.0, 1.5 },
    /* 8  ELDER_BREAKOUT_SELL     */ { Tier::ELDER,          0.0, 1.5 },
    /* 9  NR7_BREAKOUT_BUY        */ { Tier::LOW,            0.5, 2.0 },
    /* 10 NR7_BREAKOUT_SELL       */ { Tier::LOW,            0.5, 2.0 },
    /* 11 ITR_BREAKOUT_BUY        */ { Tier::LOW,            0.5, 1.5 },
    /* 12 ITR_BREAKOUT_SELL       */ { Tier::LOW,            0.5, 1.5 },
    /* 13 ITR_FADE_BUY            */ { Tier::LOW,            0.5, 1.0 },
    /* 14 ITR_FADE_SELL           */ { Tier::LOW,            0.5, 1.0 },
    /* 15 RSI_FAILURE_SWING_BUY   */ { Tier::LOW,            0.5, 1.5 },
    /* 16 RSI_FAILURE_SWING_SELL  */ { Tier::LOW,            0.5, 1.5 },
    /* 17 STOCHASTIC_POP_BUY      */ { Tier::LOW,            0.5, 1.0 },
    /* 18 STOCHASTIC_POP_SELL     */ { Tier::LOW,            0.5, 1.0 },
}};

// Fixed 1.5R target for Elder Breakout (2026-07-14 lock; §5.3 note).
inline constexpr double kElderTargetR = 1.5;

// Regime-conditioned vertical barrier (bars, TS3 15-min clock; §4.3).
// Phase 1: min(regime_cap, pattern_cap) with pattern_cap == +inf, so == regime_cap.
inline constexpr int MaxBarsForRegime(Regime r) noexcept {
    switch (r) {
        case Regime::COILED_SPRING:    return 40;
        case Regime::GAUSSIAN_STABLE:  return 25;
        case Regime::GAUSSIAN_FRAGILE: return 15;
        case Regime::PARETO_MOMENTUM:  return 12;
    }
    return 25;  // absent/default
}

// ---------------------------------------------------------------------------
// Barrier computation I/O (POD; matches the golden-vector fixture schema).
// ---------------------------------------------------------------------------
struct BarrierInputs {
    int    pattern_id;               // RaschkeTacticalTrigger value 1..18
    bool   is_long;
    double entry;                    // resolved fill price
    double bar_high;                 // current bar high (HIGH-tier / Elder stop)
    double bar_low;                  // current bar low  (HIGH-tier / Elder stop)
    double prev_high;                // prior bar high (Elder 2-bar stop)
    double prev_low;                 // prior bar low  (Elder 2-bar stop)
    double atr10;
    double dof_stop_scale;           // effective_atr = atr10 * dof_stop_scale
    double regime_stop_width_scale;  // Phase 1 = 1.0 (identity); bounded [0.5,2.0] in Phase 3
    double nbar_extreme_high;        // structural target for HIGH long (0 => absent)
    double nbar_extreme_low;         // structural target for HIGH short (0 => absent)
    double swing_high;               // universal 60-min swing cap (0 => absent)
    double swing_low;                // universal 60-min swing cap (0 => absent)
    Regime regime;
    double tick_size;
};

struct Barriers {
    double seed_stop;
    double seed_target;
    double stop;
    double target;
    int    max_bars;
    double risk;                     // |entry - final stop|
    double reward_at_target;         // |final target - entry|
    bool   structural_stop_bound;    // true if the swing cap moved the stop
    bool   structural_target_bound;  // true if the swing cap moved the target
};

// Clamp helper (regime stop-width scale bound, §5.4 co-evolution lesson).
inline constexpr double clampd(double v, double lo, double hi) noexcept {
    return v < lo ? lo : (v > hi ? hi : v);
}

// ---------------------------------------------------------------------------
// ComputeBarriers — the §5.6 4-step Adaptive Barrier Protocol (Reading B).
//   1. Seed (per-tier)  2. Regime stop-width scale  3./4. Structural cap (last).
// Deterministic, allocation-free. Returns raw (unrounded) prices; the order
// layer rounds to tick downstream (fixture convention).
// ---------------------------------------------------------------------------
inline Barriers ComputeBarriers(const BarrierInputs& in) noexcept {
    const int idx = (in.pattern_id >= 0 && in.pattern_id < 19) ? in.pattern_id : 0;
    const PatternParams p = kPatternTable[static_cast<std::size_t>(idx)];
    const double eff_atr = in.atr10 * in.dof_stop_scale;
    const double dir = in.is_long ? 1.0 : -1.0;

    // --- Step 1: seed stop + target (Reading B, per tier) ---
    double seed_stop = 0.0;
    double seed_target = 0.0;

    if (p.tier == Tier::HIGH_DEDICATED) {
        // Bar-anchored stop (PC-22): low - buffer*atr (long) / high + buffer*atr (short).
        seed_stop = in.is_long ? (in.bar_low - p.stop_mult * eff_atr)
                               : (in.bar_high + p.stop_mult * eff_atr);
        // Target = raw N-bar structural extreme, directly.
        seed_target = in.is_long ? in.nbar_extreme_high : in.nbar_extreme_low;
    } else if (p.tier == Tier::ELDER) {
        // 2-bar structural stop: min(low, prev_low) - tick (long) / max(high, prev_high) + tick (short).
        seed_stop = in.is_long ? (std::fmin(in.bar_low, in.prev_low) - in.tick_size)
                               : (std::fmax(in.bar_high, in.prev_high) + in.tick_size);
        const double risk0 = std::fabs(in.entry - seed_stop);
        seed_target = in.entry + dir * (kElderTargetR * risk0);
    } else {  // Tier::LOW — generic entry-anchored
        seed_stop = in.entry - dir * (p.stop_mult * eff_atr);
        const double risk0 = std::fabs(in.entry - seed_stop);
        seed_target = in.entry + dir * (p.target_r_mult * risk0);
    }

    // --- Step 2: regime stop-width scale (bounded [0.5,2.0]; Phase 1 = 1.0 identity) ---
    const double scale = clampd(in.regime_stop_width_scale, 0.5, 2.0);
    double stop_dist = std::fabs(in.entry - seed_stop) * scale;
    double stop = in.entry - dir * stop_dist;
    double target = seed_target;
    // R-based tiers re-derive target off the scaled stop so R:R is preserved.
    if (p.tier == Tier::ELDER) {
        target = in.entry + dir * (kElderTargetR * stop_dist);
    } else if (p.tier == Tier::LOW) {
        target = in.entry + dir * (p.target_r_mult * stop_dist);
    }
    // (HIGH structural target is scale-invariant by construction.)

    // --- Steps 3+4: universal 60-min structural cap, tightening-only (applied last) ---
    bool stop_bound = false;
    bool target_bound = false;
    if (in.is_long) {
        if (in.swing_low > 0.0 && in.swing_low < in.entry) {
            const double t = std::fmax(stop, in.swing_low);   // tighten stop up
            stop_bound = (t != stop);
            stop = t;
        }
        if (in.swing_high > 0.0 && in.swing_high > in.entry) {
            const double t = std::fmin(target, in.swing_high); // cap target nearer
            target_bound = (t != target);
            target = t;
        }
    } else {
        if (in.swing_high > 0.0 && in.swing_high > in.entry) {
            const double t = std::fmin(stop, in.swing_high);   // tighten stop down
            stop_bound = (t != stop);
            stop = t;
        }
        if (in.swing_low > 0.0 && in.swing_low < in.entry) {
            const double t = std::fmax(target, in.swing_low);  // cap target nearer
            target_bound = (t != target);
            target = t;
        }
    }

    Barriers out{};
    out.seed_stop = seed_stop;
    out.seed_target = seed_target;
    out.stop = stop;
    out.target = target;
    out.max_bars = MaxBarsForRegime(in.regime);
    out.risk = std::fabs(in.entry - stop);
    out.reward_at_target = std::fabs(target - in.entry);
    out.structural_stop_bound = stop_bound;
    out.structural_target_bound = target_bound;
    return out;
}

}  // namespace tbe
