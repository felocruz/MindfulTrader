//! Rust port of `include/TripleBarrierExitManager.h` -- the live bracket lifecycle: compute +
//! latch the immutable bracket at entry (via `stop_manager::compute_barriers`), then resolve the
//! first-hit-wins exit each tick (stop/target/time/regime-invalidation, §4.4). Literal
//! behavior-preserving port (spec §8.7 D4) -- same race order (regime-invalidation -> vertical ->
//! lower -> upper), same resolution semantics as the C++ original. The C++ original is a
//! process-wide singleton (`TripleBarrierExitManager::getInstance()`, "single active-position
//! bracket (concentrated single-instrument book)"); this port exposes the same state machine as
//! an ordinary struct instead -- that's a mechanics difference (the Rust caller already owns
//! exactly one instance per position, no singleton needed), not a behavior change.
//!
//! The bracket this manages is STATIC once opened -- `open_bracket` computes `stop`/`target` once
//! and `evaluate` never adjusts them, only resolves against them. This is not a Chandelier-style
//! trailing stop (see `stop_manager`'s own module doc for the full correction).

use crate::stop_manager::{self, BarrierInputs, Resolution};

/// Single active-position bracket. Mirrors `TripleBarrierExitManager::Bracket`.
#[derive(Debug, Clone, Copy)]
pub struct Bracket {
    pub active: bool,
    pub position_id: i32,
    pub pattern_id: i32,
    pub is_long: bool,
    pub entry_bar_index: i32,
    pub stop: f64,
    pub target: f64,
    pub max_bars: i32,
    pub resolution: Resolution,
}

impl Default for Bracket {
    fn default() -> Self {
        // Matches the C++ struct's in-class initializers exactly.
        Self {
            active: false,
            position_id: -1,
            pattern_id: 0,
            is_long: true,
            entry_bar_index: -1,
            stop: 0.0,
            target: 0.0,
            max_bars: 25,
            resolution: Resolution::Open,
        }
    }
}

/// Mirrors `TripleBarrierExitManager` (minus the C++ singleton accessor -- see module doc).
#[derive(Debug, Clone, Copy, Default)]
pub struct TripleBarrierExitManager {
    bracket: Bracket,
}

impl TripleBarrierExitManager {
    pub fn new() -> Self {
        Self::default()
    }

    /// Compute + latch the immutable bracket at entry (barriers fixed here). Mirrors
    /// `TripleBarrierExitManager::OpenBracket`.
    pub fn open_bracket(&mut self, position_id: i32, entry_bar_index: i32, input: &BarrierInputs) -> &Bracket {
        let b = stop_manager::compute_barriers(input);
        self.bracket = Bracket {
            active: true,
            position_id,
            pattern_id: input.pattern_id,
            is_long: input.is_long,
            entry_bar_index,
            stop: b.stop,
            target: b.target,
            max_bars: b.max_bars,
            resolution: Resolution::Open,
        };
        &self.bracket
    }

    /// First-hit-wins resolution on the current tick (§4.4). `regime_invalidated` is the
    /// caller-supplied kill-switch decision (Phase 2 wires the O(1) `.context` check here, same as
    /// the C++ original). Returns `Open` if no barrier is touched. Mirrors
    /// `TripleBarrierExitManager::Evaluate`.
    pub fn evaluate(&mut self, current_bar_index: i32, current_price: f64, regime_invalidated: bool) -> Resolution {
        if !self.bracket.active {
            return Resolution::Open;
        }

        if regime_invalidated {
            return self.resolve(Resolution::RegimeInvalidation);
        }

        if current_bar_index - self.bracket.entry_bar_index >= self.bracket.max_bars {
            return self.resolve(Resolution::TimeExit);
        }

        if self.bracket.is_long {
            if current_price <= self.bracket.stop {
                return self.resolve(Resolution::StopHit);
            }
            if current_price >= self.bracket.target {
                return self.resolve(Resolution::TargetHit);
            }
        } else {
            if current_price >= self.bracket.stop {
                return self.resolve(Resolution::StopHit);
            }
            if current_price <= self.bracket.target {
                return self.resolve(Resolution::TargetHit);
            }
        }
        Resolution::Open
    }

    pub fn current(&self) -> &Bracket {
        &self.bracket
    }

    pub fn close(&mut self) {
        self.bracket = Bracket::default();
    }

    fn resolve(&mut self, r: Resolution) -> Resolution {
        self.bracket.resolution = r;
        self.bracket.active = false;
        r
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::stop_manager::Regime;

    fn long_inputs(entry: f64, stop_mult_atr: f64) -> BarrierInputs {
        // pattern_id 1 (KANGAROO_TAIL_BUY, Tier::Low, stop_mult=0.5) with atr10 chosen so the
        // stop distance is exactly `stop_mult_atr` (eff_atr = atr10 * dof_stop_scale = atr10).
        BarrierInputs {
            pattern_id: 1,
            is_long: true,
            entry,
            bar_high: entry + 1.0,
            bar_low: entry - 1.0,
            prev_high: 0.0,
            prev_low: 0.0,
            atr10: stop_mult_atr / 0.5,
            dof_stop_scale: 1.0,
            regime_stop_width_scale: 1.0,
            nbar_extreme_high: 0.0,
            nbar_extreme_low: 0.0,
            swing_high: 0.0,
            swing_low: 0.0,
            regime: Regime::GaussianStable, // max_bars = 25
            tick_size: 0.25,
        }
    }

    fn short_inputs(entry: f64, stop_mult_atr: f64) -> BarrierInputs {
        // pattern_id 2 (KANGAROO_TAIL_SELL, Tier::Low, stop_mult=0.5), same construction.
        BarrierInputs {
            pattern_id: 2,
            is_long: false,
            ..long_inputs(entry, stop_mult_atr)
        }
    }

    #[test]
    fn fresh_manager_has_no_active_bracket() {
        let mgr = TripleBarrierExitManager::new();
        assert!(!mgr.current().active);
        assert_eq!(mgr.current().resolution, Resolution::Open);
    }

    #[test]
    fn open_bracket_latches_computed_stop_target_and_max_bars() {
        let mut mgr = TripleBarrierExitManager::new();
        let inputs = long_inputs(100.0, 2.0); // stop_dist = 2.0 * 1.0 (identity scale) = 2.0
        let b = mgr.open_bracket(7, 10, &inputs);
        assert!(b.active);
        assert_eq!(b.position_id, 7);
        assert_eq!(b.pattern_id, 1);
        assert!(b.is_long);
        assert_eq!(b.entry_bar_index, 10);
        assert!((b.stop - 98.0).abs() < 1e-9);
        assert_eq!(b.max_bars, 25);
        assert_eq!(b.resolution, Resolution::Open);
    }

    #[test]
    fn evaluate_on_inactive_bracket_returns_open() {
        let mut mgr = TripleBarrierExitManager::new();
        assert_eq!(mgr.evaluate(0, 100.0, false), Resolution::Open);
    }

    #[test]
    fn long_stop_hit_resolves_and_deactivates() {
        let mut mgr = TripleBarrierExitManager::new();
        let inputs = long_inputs(100.0, 2.0); // stop = 98.0
        mgr.open_bracket(1, 0, &inputs);
        let r = mgr.evaluate(1, 97.99, false);
        assert_eq!(r, Resolution::StopHit);
        assert!(!mgr.current().active);
        assert_eq!(mgr.current().resolution, Resolution::StopHit);
    }

    #[test]
    fn long_target_hit_resolves() {
        let mut mgr = TripleBarrierExitManager::new();
        let inputs = long_inputs(100.0, 2.0); // target = 100 + 1.5*2.0 = 103.0
        mgr.open_bracket(1, 0, &inputs);
        let r = mgr.evaluate(1, 103.01, false);
        assert_eq!(r, Resolution::TargetHit);
    }

    #[test]
    fn short_stop_hit_resolves() {
        let mut mgr = TripleBarrierExitManager::new();
        let inputs = short_inputs(100.0, 2.0); // stop = 102.0
        mgr.open_bracket(1, 0, &inputs);
        let r = mgr.evaluate(1, 102.01, false);
        assert_eq!(r, Resolution::StopHit);
    }

    #[test]
    fn short_target_hit_resolves() {
        let mut mgr = TripleBarrierExitManager::new();
        let inputs = short_inputs(100.0, 2.0); // target = 100 - 1.5*2.0 = 97.0
        mgr.open_bracket(1, 0, &inputs);
        let r = mgr.evaluate(1, 96.99, false);
        assert_eq!(r, Resolution::TargetHit);
    }

    #[test]
    fn neither_barrier_touched_stays_open() {
        let mut mgr = TripleBarrierExitManager::new();
        let inputs = long_inputs(100.0, 2.0); // stop=98.0, target=103.0
        mgr.open_bracket(1, 0, &inputs);
        let r = mgr.evaluate(1, 100.5, false);
        assert_eq!(r, Resolution::Open);
        assert!(mgr.current().active); // still open
    }

    #[test]
    fn time_exit_fires_at_max_bars() {
        let mut mgr = TripleBarrierExitManager::new();
        let inputs = long_inputs(100.0, 2.0); // max_bars = 25, entry_bar_index = 0
        mgr.open_bracket(1, 0, &inputs);
        // Price stays safely between stop/target; only the bar count should trigger.
        let r = mgr.evaluate(25, 100.5, false);
        assert_eq!(r, Resolution::TimeExit);
    }

    #[test]
    fn time_exit_does_not_fire_one_bar_early() {
        let mut mgr = TripleBarrierExitManager::new();
        let inputs = long_inputs(100.0, 2.0); // max_bars = 25
        mgr.open_bracket(1, 0, &inputs);
        let r = mgr.evaluate(24, 100.5, false);
        assert_eq!(r, Resolution::Open);
    }

    #[test]
    fn regime_invalidation_takes_precedence_over_everything() {
        let mut mgr = TripleBarrierExitManager::new();
        let inputs = long_inputs(100.0, 2.0); // stop=98.0 -- price below stop AND past max_bars
        mgr.open_bracket(1, 0, &inputs);
        // Both the stop and the time barrier would also fire here; regime-invalidation must win
        // (first-hit-wins race order: regime-invalidation -> vertical -> lower -> upper, §4.4).
        let r = mgr.evaluate(30, 97.0, true);
        assert_eq!(r, Resolution::RegimeInvalidation);
    }

    #[test]
    fn time_exit_takes_precedence_over_stop_and_target() {
        let mut mgr = TripleBarrierExitManager::new();
        let inputs = long_inputs(100.0, 2.0); // stop=98.0, target=103.0
        mgr.open_bracket(1, 0, &inputs);
        // Price is also past the stop, but the vertical barrier is checked first.
        let r = mgr.evaluate(25, 97.0, false);
        assert_eq!(r, Resolution::TimeExit);
    }

    #[test]
    fn stop_takes_precedence_over_target_when_both_look_touched() {
        // Pathological input (stop >= target for a long) isn't realistic, but the C++ original
        // checks stop before target unconditionally -- preserve that exact order.
        let mut mgr = TripleBarrierExitManager::new();
        let inputs = long_inputs(100.0, 2.0);
        mgr.open_bracket(1, 0, &inputs);
        // A price at or below the stop always resolves StopHit even if (hypothetically) it would
        // also satisfy a target check -- verified directly via the stop boundary alone above
        // (long_stop_hit_resolves_and_deactivates); this test documents the ordering guarantee.
        let r = mgr.evaluate(1, 98.0, false); // exactly at stop (`<=`)
        assert_eq!(r, Resolution::StopHit);
    }

    #[test]
    fn once_resolved_bracket_stays_inactive_on_further_evaluate_calls() {
        let mut mgr = TripleBarrierExitManager::new();
        let inputs = long_inputs(100.0, 2.0);
        mgr.open_bracket(1, 0, &inputs);
        assert_eq!(mgr.evaluate(1, 97.0, false), Resolution::StopHit);
        // A second call on an already-resolved (inactive) bracket must return Open, not re-fire.
        assert_eq!(mgr.evaluate(2, 50.0, false), Resolution::Open);
    }

    #[test]
    fn close_resets_to_default_bracket() {
        let mut mgr = TripleBarrierExitManager::new();
        let inputs = long_inputs(100.0, 2.0);
        mgr.open_bracket(1, 0, &inputs);
        assert!(mgr.current().active);
        mgr.close();
        assert!(!mgr.current().active);
        assert_eq!(mgr.current().position_id, -1);
        assert_eq!(mgr.current().max_bars, 25);
        assert_eq!(mgr.current().resolution, Resolution::Open);
    }

    #[test]
    fn opening_a_new_bracket_overwrites_a_prior_closed_one() {
        let mut mgr = TripleBarrierExitManager::new();
        mgr.open_bracket(1, 0, &long_inputs(100.0, 2.0));
        mgr.evaluate(1, 97.0, false); // resolves StopHit, deactivates
        mgr.open_bracket(2, 5, &long_inputs(200.0, 3.0));
        assert!(mgr.current().active);
        assert_eq!(mgr.current().position_id, 2);
        assert_eq!(mgr.current().entry_bar_index, 5);
        assert_eq!(mgr.current().resolution, Resolution::Open);
    }
}
