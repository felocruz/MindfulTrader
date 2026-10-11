//! Rust port of `include/TripleBarrierEngine.h` -- pure, allocation-free barrier-computation core
//! for the Triple-Barrier Exit Engine (`docs/ADR/triple_barrier_exit_engine_spec.md` §4, §5.3,
//! §5.6). Literal behavior-preserving port (spec §8.7 D4) -- same formulas, same pattern table,
//! same regime-vertical-barrier mapping as the C++ original; only mechanics changed
//! (snake_case field/enum-variant names, a Rust `enum`/`struct` instead of a C++ `enum class`/POD
//! struct). The bracket this computes is a STATIC, entry-anchored stop/target pair, latched once
//! and never adjusted thereafter -- not a Chandelier-style continuously-trailing stop (that
//! mechanism, `ChandelierStopManager`, was deleted from this codebase during a documented cutover
//! before this Rust initiative began; see spec §8.3's `stop_manager.rs` entry for the full
//! correction).

/// Per-pattern barrier tier (Reading B, §5.3). Mirrors `tbe::Tier`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Tier {
    /// Generic: entry-anchored ATR stop, R-multiple target.
    Low,
    /// Turtle Soup / Momentum Pinball: bar-anchored stop, N-bar structural target.
    HighDedicated,
    /// Elder Breakout: 2-bar structural stop, fixed 1.5R target.
    Elder,
}

/// HMM regime at entry -> vertical-barrier horizon (§4.3). Mirrors `tbe::Regime`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Regime {
    CoiledSpring,
    /// Also the absent/default (mirrors the C++ original's `HMM_NO_PRIOR` fold-through, done by
    /// the caller before this core is reached -- see `TripleBarrierExitManager`'s `ToRegime`).
    GaussianStable,
    GaussianFragile,
    ParetoMomentum,
}

/// First-hit-wins resolution outcome (§4.4, §5.1). Mirrors `tbe::Resolution`. Also used by
/// `trap_exit`'s live bracket state machine (same namespace split as the C++ original, where
/// `TripleBarrierExitManager.h` reuses `tbe::Resolution` from this file rather than redefining it).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Resolution {
    Open,
    StopHit,
    TargetHit,
    TimeExit,
    RegimeInvalidation,
}

/// Reconciled 9-pattern parameter table (§5.3). Indexed by `RaschkeTacticalTrigger` id (0..18).
/// `stop_mult` is the ATR buffer (HIGH/LOW); `target_r_mult` is the ladder-t1 R-multiple (drives
/// LOW targets and Elder's 1.5R; informational for HIGH, whose target is the structural extreme
/// per Reading B). Mirrors `tbe::PatternParams`/`tbe::kPatternTable`.
#[derive(Debug, Clone, Copy)]
pub struct PatternParams {
    pub tier: Tier,
    pub stop_mult: f64,
    pub target_r_mult: f64,
}

pub const PATTERN_TABLE: [PatternParams; 19] = [
    /* 0  NONE                    */ PatternParams { tier: Tier::Low, stop_mult: 0.5, target_r_mult: 1.5 },
    /* 1  KANGAROO_TAIL_BUY       */ PatternParams { tier: Tier::Low, stop_mult: 0.5, target_r_mult: 1.5 },
    /* 2  KANGAROO_TAIL_SELL      */ PatternParams { tier: Tier::Low, stop_mult: 0.5, target_r_mult: 1.5 },
    /* 3  TURTLE_SOUP_BUY         */ PatternParams { tier: Tier::HighDedicated, stop_mult: 0.5, target_r_mult: 1.0 },
    /* 4  TURTLE_SOUP_SELL        */ PatternParams { tier: Tier::HighDedicated, stop_mult: 0.5, target_r_mult: 1.0 },
    /* 5  MOMENTUM_PINBALL_BUY    */ PatternParams { tier: Tier::HighDedicated, stop_mult: 0.4, target_r_mult: 1.0 },
    /* 6  MOMENTUM_PINBALL_SELL   */ PatternParams { tier: Tier::HighDedicated, stop_mult: 0.4, target_r_mult: 1.0 },
    /* 7  ELDER_BREAKOUT_BUY      */ PatternParams { tier: Tier::Elder, stop_mult: 0.0, target_r_mult: 1.5 },
    /* 8  ELDER_BREAKOUT_SELL     */ PatternParams { tier: Tier::Elder, stop_mult: 0.0, target_r_mult: 1.5 },
    /* 9  NR7_BREAKOUT_BUY        */ PatternParams { tier: Tier::Low, stop_mult: 0.5, target_r_mult: 2.0 },
    /* 10 NR7_BREAKOUT_SELL       */ PatternParams { tier: Tier::Low, stop_mult: 0.5, target_r_mult: 2.0 },
    /* 11 ITR_BREAKOUT_BUY        */ PatternParams { tier: Tier::Low, stop_mult: 0.5, target_r_mult: 1.5 },
    /* 12 ITR_BREAKOUT_SELL       */ PatternParams { tier: Tier::Low, stop_mult: 0.5, target_r_mult: 1.5 },
    /* 13 ITR_FADE_BUY            */ PatternParams { tier: Tier::Low, stop_mult: 0.5, target_r_mult: 1.0 },
    /* 14 ITR_FADE_SELL           */ PatternParams { tier: Tier::Low, stop_mult: 0.5, target_r_mult: 1.0 },
    /* 15 RSI_FAILURE_SWING_BUY   */ PatternParams { tier: Tier::Low, stop_mult: 0.5, target_r_mult: 1.5 },
    /* 16 RSI_FAILURE_SWING_SELL  */ PatternParams { tier: Tier::Low, stop_mult: 0.5, target_r_mult: 1.5 },
    /* 17 STOCHASTIC_POP_BUY      */ PatternParams { tier: Tier::Low, stop_mult: 0.5, target_r_mult: 1.0 },
    /* 18 STOCHASTIC_POP_SELL     */ PatternParams { tier: Tier::Low, stop_mult: 0.5, target_r_mult: 1.0 },
];

/// Fixed 1.5R target for Elder Breakout (2026-07-14 lock; §5.3 note). Mirrors `tbe::kElderTargetR`.
pub const ELDER_TARGET_R: f64 = 1.5;

/// Regime-conditioned vertical barrier (bars, TS3 15-min clock; §4.3). Phase 1:
/// `min(regime_cap, pattern_cap)` with `pattern_cap == +inf`, so == `regime_cap`. Mirrors
/// `tbe::MaxBarsForRegime`.
pub fn max_bars_for_regime(r: Regime) -> i32 {
    match r {
        Regime::CoiledSpring => 40,
        Regime::GaussianStable => 25,
        Regime::GaussianFragile => 15,
        Regime::ParetoMomentum => 12,
    }
}

/// Barrier computation inputs (matches the golden-vector fixture schema). Mirrors
/// `tbe::BarrierInputs`.
#[derive(Debug, Clone, Copy)]
pub struct BarrierInputs {
    /// `RaschkeTacticalTrigger` value 1..18.
    pub pattern_id: i32,
    pub is_long: bool,
    /// Resolved fill price.
    pub entry: f64,
    /// Current bar high (HIGH-tier / Elder stop).
    pub bar_high: f64,
    /// Current bar low (HIGH-tier / Elder stop).
    pub bar_low: f64,
    /// Prior bar high (Elder 2-bar stop).
    pub prev_high: f64,
    /// Prior bar low (Elder 2-bar stop).
    pub prev_low: f64,
    pub atr10: f64,
    /// `effective_atr = atr10 * dof_stop_scale`.
    pub dof_stop_scale: f64,
    /// Phase 1 = 1.0 (identity); bounded `[0.5, 2.0]` in Phase 3.
    pub regime_stop_width_scale: f64,
    /// Structural target for HIGH long (0 => absent).
    pub nbar_extreme_high: f64,
    /// Structural target for HIGH short (0 => absent).
    pub nbar_extreme_low: f64,
    /// Universal 60-min swing cap (0 => absent).
    pub swing_high: f64,
    /// Universal 60-min swing cap (0 => absent).
    pub swing_low: f64,
    pub regime: Regime,
    pub tick_size: f64,
}

/// Barrier computation outputs. Mirrors `tbe::Barriers`.
#[derive(Debug, Clone, Copy)]
pub struct Barriers {
    pub seed_stop: f64,
    pub seed_target: f64,
    pub stop: f64,
    pub target: f64,
    pub max_bars: i32,
    /// `|entry - final stop|`.
    pub risk: f64,
    /// `|final target - entry|`.
    pub reward_at_target: f64,
    /// True if the swing cap moved the stop.
    pub structural_stop_bound: bool,
    /// True if the swing cap moved the target.
    pub structural_target_bound: bool,
}

/// `ComputeBarriers` -- the §5.6 4-step Adaptive Barrier Protocol (Reading B): 1. Seed (per-tier)
/// 2. Regime stop-width scale 3./4. Structural cap (last). Deterministic, allocation-free. Returns
/// raw (unrounded) prices; the order layer rounds to tick downstream (fixture convention). Mirrors
/// `tbe::ComputeBarriers`.
// The structural-cap step's `t != stop`/`t != target` checks are deliberate exact comparisons
// (matching the C++ original's `stop_bound = (t != stop);` bit-for-bit): they ask "did
// `.max()`/`.min()` just change the value at all," not "is the result numerically close to the
// old one" -- no rounding/accumulated-error concern exists since `t` is either literally
// `stop`/`target` unchanged (the branch not taken) or literally the swing cap value substituted
// in whole.
#[allow(clippy::float_cmp)]
pub fn compute_barriers(input: &BarrierInputs) -> Barriers {
    let idx = if input.pattern_id >= 0 && input.pattern_id < 19 {
        input.pattern_id as usize
    } else {
        0
    };
    let p = PATTERN_TABLE[idx];
    let eff_atr = input.atr10 * input.dof_stop_scale;
    let dir = if input.is_long { 1.0 } else { -1.0 };

    // --- Step 1: seed stop + target (Reading B, per tier) ---
    let (seed_stop, seed_target) = match p.tier {
        Tier::HighDedicated => {
            // Bar-anchored stop (PC-22): low - buffer*atr (long) / high + buffer*atr (short).
            let seed_stop = if input.is_long {
                input.bar_low - p.stop_mult * eff_atr
            } else {
                input.bar_high + p.stop_mult * eff_atr
            };
            // Target = raw N-bar structural extreme, directly.
            let seed_target = if input.is_long { input.nbar_extreme_high } else { input.nbar_extreme_low };
            (seed_stop, seed_target)
        }
        Tier::Elder => {
            // 2-bar structural stop: min(low, prev_low) - tick (long) / max(high, prev_high) + tick (short).
            let seed_stop = if input.is_long {
                input.bar_low.min(input.prev_low) - input.tick_size
            } else {
                input.bar_high.max(input.prev_high) + input.tick_size
            };
            let risk0 = (input.entry - seed_stop).abs();
            let seed_target = input.entry + dir * (ELDER_TARGET_R * risk0);
            (seed_stop, seed_target)
        }
        Tier::Low => {
            // Generic entry-anchored.
            let seed_stop = input.entry - dir * (p.stop_mult * eff_atr);
            let risk0 = (input.entry - seed_stop).abs();
            let seed_target = input.entry + dir * (p.target_r_mult * risk0);
            (seed_stop, seed_target)
        }
    };

    // --- Step 2: regime stop-width scale (bounded [0.5,2.0]; Phase 1 = 1.0 identity) ---
    let scale = input.regime_stop_width_scale.clamp(0.5, 2.0);
    let stop_dist = (input.entry - seed_stop).abs() * scale;
    let mut stop = input.entry - dir * stop_dist;
    let mut target = seed_target;
    // R-based tiers re-derive target off the scaled stop so R:R is preserved.
    match p.tier {
        Tier::Elder => target = input.entry + dir * (ELDER_TARGET_R * stop_dist),
        Tier::Low => target = input.entry + dir * (p.target_r_mult * stop_dist),
        Tier::HighDedicated => {} // structural target is scale-invariant by construction.
    }

    // --- Steps 3+4: universal 60-min structural cap, tightening-only (applied last) ---
    let mut stop_bound = false;
    let mut target_bound = false;
    if input.is_long {
        if input.swing_low > 0.0 && input.swing_low < input.entry {
            let t = stop.max(input.swing_low); // tighten stop up
            stop_bound = t != stop;
            stop = t;
        }
        if input.swing_high > 0.0 && input.swing_high > input.entry {
            let t = target.min(input.swing_high); // cap target nearer
            target_bound = t != target;
            target = t;
        }
    } else {
        if input.swing_high > 0.0 && input.swing_high > input.entry {
            let t = stop.min(input.swing_high); // tighten stop down
            stop_bound = t != stop;
            stop = t;
        }
        if input.swing_low > 0.0 && input.swing_low < input.entry {
            let t = target.max(input.swing_low);
            target_bound = t != target;
            target = t;
        }
    }

    Barriers {
        seed_stop,
        seed_target,
        stop,
        target,
        max_bars: max_bars_for_regime(input.regime),
        risk: (input.entry - stop).abs(),
        reward_at_target: (target - input.entry).abs(),
        structural_stop_bound: stop_bound,
        structural_target_bound: target_bound,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn approx(a: f64, b: f64) -> bool {
        (a - b).abs() <= 1e-6
    }

    struct Expected {
        seed_stop: f64,
        seed_target: f64,
        stop: f64,
        target: f64,
        max_bars: i32,
        risk: f64,
        reward_at_target: f64,
        structural_stop_bound: bool,
        structural_target_bound: bool,
    }

    fn check(input: &BarrierInputs, expected: &Expected) {
        let b = compute_barriers(input);
        assert!(approx(b.seed_stop, expected.seed_stop), "seed_stop: got {} exp {}", b.seed_stop, expected.seed_stop);
        assert!(approx(b.seed_target, expected.seed_target), "seed_target: got {} exp {}", b.seed_target, expected.seed_target);
        assert!(approx(b.stop, expected.stop), "stop: got {} exp {}", b.stop, expected.stop);
        assert!(approx(b.target, expected.target), "target: got {} exp {}", b.target, expected.target);
        assert_eq!(b.max_bars, expected.max_bars);
        assert!(approx(b.risk, expected.risk), "risk: got {} exp {}", b.risk, expected.risk);
        assert!(
            approx(b.reward_at_target, expected.reward_at_target),
            "reward_at_target: got {} exp {}",
            b.reward_at_target,
            expected.reward_at_target
        );
        assert_eq!(b.structural_stop_bound, expected.structural_stop_bound);
        assert_eq!(b.structural_target_bound, expected.structural_target_bound);
    }

    // The 7 cases below mirror tests/cpp/test_triple_barrier_parity.cpp /
    // tests/fixtures/triple_barrier_golden_vectors.json exactly -- same canonical cross-repo
    // fixture, same names, same numbers.

    #[test]
    fn turtle_soup_buy_caps_nonbinding() {
        check(
            &BarrierInputs {
                pattern_id: 3,
                is_long: true,
                entry: 5000.00,
                bar_high: 5000.50,
                bar_low: 4999.00,
                prev_high: 0.0,
                prev_low: 0.0,
                atr10: 5.00,
                dof_stop_scale: 1.0,
                regime_stop_width_scale: 1.0,
                nbar_extreme_high: 5008.00,
                nbar_extreme_low: 0.0,
                swing_high: 5015.00,
                swing_low: 4990.00,
                regime: Regime::GaussianStable,
                tick_size: 0.25,
            },
            &Expected {
                seed_stop: 4996.50,
                seed_target: 5008.00,
                stop: 4996.50,
                target: 5008.00,
                max_bars: 25,
                risk: 3.50,
                reward_at_target: 8.00,
                structural_stop_bound: false,
                structural_target_bound: false,
            },
        );
    }

    #[test]
    fn turtle_soup_buy_caps_binding() {
        check(
            &BarrierInputs {
                pattern_id: 3,
                is_long: true,
                entry: 5000.00,
                bar_high: 5000.50,
                bar_low: 4999.00,
                prev_high: 0.0,
                prev_low: 0.0,
                atr10: 5.00,
                dof_stop_scale: 1.0,
                regime_stop_width_scale: 1.0,
                nbar_extreme_high: 5012.00,
                nbar_extreme_low: 0.0,
                swing_high: 5006.00,
                swing_low: 4998.00,
                regime: Regime::GaussianStable,
                tick_size: 0.25,
            },
            &Expected {
                seed_stop: 4996.50,
                seed_target: 5012.00,
                stop: 4998.00,
                target: 5006.00,
                max_bars: 25,
                risk: 2.00,
                reward_at_target: 6.00,
                structural_stop_bound: true,
                structural_target_bound: true,
            },
        );
    }

    #[test]
    fn turtle_soup_sell_caps_nonbinding() {
        check(
            &BarrierInputs {
                pattern_id: 4,
                is_long: false,
                entry: 5000.00,
                bar_high: 5001.00,
                bar_low: 4999.50,
                prev_high: 0.0,
                prev_low: 0.0,
                atr10: 5.00,
                dof_stop_scale: 1.0,
                regime_stop_width_scale: 1.0,
                nbar_extreme_high: 0.0,
                nbar_extreme_low: 4992.00,
                swing_high: 5006.00,
                swing_low: 4988.00,
                regime: Regime::GaussianStable,
                tick_size: 0.25,
            },
            &Expected {
                seed_stop: 5003.50,
                seed_target: 4992.00,
                stop: 5003.50,
                target: 4992.00,
                max_bars: 25,
                risk: 3.50,
                reward_at_target: 8.00,
                structural_stop_bound: false,
                structural_target_bound: false,
            },
        );
    }

    #[test]
    fn momentum_pinball_sell_dof_scaled() {
        // dof 1.2 -> eff_atr 6.00
        check(
            &BarrierInputs {
                pattern_id: 6,
                is_long: false,
                entry: 5000.00,
                bar_high: 5001.00,
                bar_low: 4999.50,
                prev_high: 0.0,
                prev_low: 0.0,
                atr10: 5.00,
                dof_stop_scale: 1.2,
                regime_stop_width_scale: 1.0,
                nbar_extreme_high: 0.0,
                nbar_extreme_low: 4990.00,
                swing_high: 5010.00,
                swing_low: 4985.00,
                regime: Regime::ParetoMomentum,
                tick_size: 0.25,
            },
            &Expected {
                seed_stop: 5003.40,
                seed_target: 4990.00,
                stop: 5003.40,
                target: 4990.00,
                max_bars: 12,
                risk: 3.40,
                reward_at_target: 10.00,
                structural_stop_bound: false,
                structural_target_bound: false,
            },
        );
    }

    #[test]
    fn elder_breakout_buy_hybrid_1p5r() {
        check(
            &BarrierInputs {
                pattern_id: 7,
                is_long: true,
                entry: 5000.00,
                bar_high: 5001.00,
                bar_low: 4998.00,
                prev_high: 4999.50,
                prev_low: 4997.50,
                atr10: 5.00,
                dof_stop_scale: 1.0,
                regime_stop_width_scale: 1.0,
                nbar_extreme_high: 0.0,
                nbar_extreme_low: 0.0,
                swing_high: 5010.00,
                swing_low: 4990.00,
                regime: Regime::CoiledSpring,
                tick_size: 0.25,
            },
            &Expected {
                seed_stop: 4997.25,
                seed_target: 5004.125,
                stop: 4997.25,
                target: 5004.125,
                max_bars: 40,
                risk: 2.75,
                reward_at_target: 4.125,
                structural_stop_bound: false,
                structural_target_bound: false,
            },
        );
    }

    #[test]
    fn nr7_breakout_buy_low_generic() {
        check(
            &BarrierInputs {
                pattern_id: 9,
                is_long: true,
                entry: 5000.00,
                bar_high: 5000.50,
                bar_low: 4999.50,
                prev_high: 0.0,
                prev_low: 0.0,
                atr10: 5.00,
                dof_stop_scale: 1.0,
                regime_stop_width_scale: 1.0,
                nbar_extreme_high: 0.0,
                nbar_extreme_low: 0.0,
                swing_high: 5015.00,
                swing_low: 4990.00,
                regime: Regime::GaussianFragile,
                tick_size: 0.25,
            },
            &Expected {
                seed_stop: 4997.50,
                seed_target: 5005.00,
                stop: 4997.50,
                target: 5005.00,
                max_bars: 15,
                risk: 2.50,
                reward_at_target: 5.00,
                structural_stop_bound: false,
                structural_target_bound: false,
            },
        );
    }

    #[test]
    fn itr_fade_sell_low_caps_binding() {
        check(
            &BarrierInputs {
                pattern_id: 14,
                is_long: false,
                entry: 5000.00,
                bar_high: 5000.50,
                bar_low: 4999.50,
                prev_high: 0.0,
                prev_low: 0.0,
                atr10: 5.00,
                dof_stop_scale: 1.0,
                regime_stop_width_scale: 1.0,
                nbar_extreme_high: 0.0,
                nbar_extreme_low: 0.0,
                swing_high: 5001.50,
                swing_low: 4998.00,
                regime: Regime::GaussianStable,
                tick_size: 0.25,
            },
            &Expected {
                seed_stop: 5002.50,
                seed_target: 4997.50,
                stop: 5001.50,
                target: 4998.00,
                max_bars: 25,
                risk: 1.50,
                reward_at_target: 2.00,
                structural_stop_bound: true,
                structural_target_bound: true,
            },
        );
    }

    #[test]
    fn max_bars_for_regime_matches_table() {
        assert_eq!(max_bars_for_regime(Regime::CoiledSpring), 40);
        assert_eq!(max_bars_for_regime(Regime::GaussianStable), 25);
        assert_eq!(max_bars_for_regime(Regime::GaussianFragile), 15);
        assert_eq!(max_bars_for_regime(Regime::ParetoMomentum), 12);
    }

    #[test]
    fn out_of_range_pattern_id_falls_back_to_index_zero() {
        // Mirrors the C++ original's `(pattern_id >= 0 && pattern_id < 19) ? pattern_id : 0`
        // defensive clamp -- an out-of-range id must not panic/index-out-of-bounds, it silently
        // falls back to pattern 0 (NONE, Tier::Low).
        let base = BarrierInputs {
            pattern_id: 0,
            is_long: true,
            entry: 100.0,
            bar_high: 100.5,
            bar_low: 99.5,
            prev_high: 0.0,
            prev_low: 0.0,
            atr10: 1.0,
            dof_stop_scale: 1.0,
            regime_stop_width_scale: 1.0,
            nbar_extreme_high: 0.0,
            nbar_extreme_low: 0.0,
            swing_high: 0.0,
            swing_low: 0.0,
            regime: Regime::GaussianStable,
            tick_size: 0.25,
        };
        let expected = compute_barriers(&base);

        let out_of_range = BarrierInputs { pattern_id: 999, ..base };
        let actual = compute_barriers(&out_of_range);
        assert!(approx(actual.stop, expected.stop));
        assert!(approx(actual.target, expected.target));

        let negative = BarrierInputs { pattern_id: -1, ..base };
        let actual_neg = compute_barriers(&negative);
        assert!(approx(actual_neg.stop, expected.stop));
        assert!(approx(actual_neg.target, expected.target));
    }
}
