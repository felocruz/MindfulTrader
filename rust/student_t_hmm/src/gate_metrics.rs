//! gate_metrics.rs -- shared regime quality-gate statistics.
//!
//! Port of `lbrnet/lbrnet/models/regime_gate_metrics.py` (spec §5.1a: "the cleanest, most-reused,
//! lowest-risk file in the whole stack... the intentional first real code in this crate," plan
//! Stage G1). The Python original's own docstring explains why this module exists at all: both
//! `regime_registry.py`'s offline taxonomy builder and `train_student_t_hmm.py`'s online sign-off
//! gate independently computed the same DOF-fragility/Pareto-thrust-share/mean-tenure-by-state
//! formulas before it existed -- one shared implementation is what keeps a Rust port's eventual
//! registry/training consumers from silently diverging the same way.
//!
//! Operates on plain state-sequence/per-state arrays (`&[f64]`, `&[i32]`), not a `StudentTHmm<D,K>`
//! instance -- these are statistics over a model's *output* (decoded states, fitted DOF, sample
//! shares), not its internals, so this module has no dependency on model.rs at all. Plan §4
//! Stage G1: no model-dependency, hand/synthetic-testable, doesn't need Stage A/P9.
//!
//! One function from the Python original is deliberately NOT ported: `extract_means_by_state()`
//! unwraps a Python model's `means_` array, which may be shaped `(K, D)` or `(K, 1, D)` depending
//! on which hmmlearn-style API produced it -- an ambiguity that doesn't exist for
//! `StudentTHmm<D, K>`'s own `means` field, which is always exactly `[[f64; D]; K]`.

/// Student-t degrees-of-freedom below which the 4th moment (kurtosis) is undefined/infinite --
/// Kotz & Nadarajah (2004), "Multivariate t Distributions and Their Applications," ch. 1. A
/// structural distributional-theory boundary, not a calibrated/tuned quantile.
pub const TALEB_KURTOSIS_UNDEFINED_DOF: f64 = 4.0;

/// Default boundary-pinning tolerance for `dof_boundary_check`/`eligible_fragile_states`/
/// `taleb_gate_pass`, matching the Python original's own default.
pub const DEFAULT_EPSILON: f64 = 0.05;
/// Default minimum-occupancy floor for `state_occupancy_balance_check`/`eligible_fragile_states`/
/// `taleb_gate_pass`, matching the Python original's own default.
pub const DEFAULT_MIN_SHARE_FLOOR: f64 = 0.03;

fn clip01(value: f64) -> f64 {
    value.clamp(0.0, 1.0)
}

/// Result of `dof_boundary_check`.
#[derive(Debug, Clone, PartialEq)]
pub struct DofBoundaryCheck {
    pub dof_by_state: Vec<f64>,
    pub pinned_states: Vec<usize>,
    pub no_state_pinned_at_bounds: bool,
}

/// Flags states whose fitted DOF sits within `epsilon` of either configured clamp bound -- the
/// canonical symptom of the (now-replaced) moment-matching DOF estimator's structural
/// instability: drift to `dof_max` regardless of true tail-heaviness, or collapse to `dof_min`.
pub fn dof_boundary_check(dof_by_state: &[f64], dof_min: f64, dof_max: f64, epsilon: f64) -> DofBoundaryCheck {
    let pinned_states: Vec<usize> = dof_by_state
        .iter()
        .enumerate()
        .filter(|&(_, &d)| d <= dof_min + epsilon || d >= dof_max - epsilon)
        .map(|(i, _)| i)
        .collect();
    let no_state_pinned_at_bounds = pinned_states.is_empty();
    DofBoundaryCheck { dof_by_state: dof_by_state.to_vec(), pinned_states, no_state_pinned_at_bounds }
}

/// Result of `state_occupancy_balance_check`.
#[derive(Debug, Clone, PartialEq)]
pub struct StateOccupancyBalanceCheck {
    pub sample_shares: Vec<f64>,
    pub min_sample_share: f64,
    pub no_state_degenerately_empty: bool,
}

/// Flags degenerate near-empty states -- a real symptom of a bad init/fit, distinct from genuine
/// minority regimes, which should still carry a meaningful non-trivial share of occupancy
/// (`min_share_floor`, default 3%, well below a uniform 1/K share but non-trivial).
pub fn state_occupancy_balance_check(sample_shares: &[f64], min_share_floor: f64) -> StateOccupancyBalanceCheck {
    let min_sample_share = sample_shares.iter().copied().fold(f64::INFINITY, f64::min);
    let min_sample_share = if sample_shares.is_empty() { 0.0 } else { min_sample_share };
    StateOccupancyBalanceCheck {
        sample_shares: sample_shares.to_vec(),
        min_sample_share,
        no_state_degenerately_empty: min_sample_share >= min_share_floor,
    }
}

/// State indices whose DOF estimate is trustworthy evidence of fragility -- not boundary-pinned
/// (`dof_boundary_check`), not NaN, and not a degenerately-empty state (below `min_share_floor`).
/// Both `taleb_gate_pass` callers filter through this before applying the
/// `dof < TALEB_KURTOSIS_UNDEFINED_DOF` test.
pub fn eligible_fragile_states(
    dof_by_state: &[f64],
    sample_shares: &[f64],
    dof_min: f64,
    dof_max: f64,
    epsilon: f64,
    min_share_floor: f64,
) -> Vec<usize> {
    let pinned = dof_boundary_check(dof_by_state, dof_min, dof_max, epsilon).pinned_states;
    dof_by_state
        .iter()
        .enumerate()
        .filter(|&(i, &d)| {
            if d.is_nan() || pinned.contains(&i) {
                return false;
            }
            let share = sample_shares.get(i).copied().unwrap_or(0.0);
            share >= min_share_floor
        })
        .map(|(i, _)| i)
        .collect()
}

/// Continuous `[0,1]` fragility score: `clip01((THRESH - dof) / THRESH)`.
pub fn dof_fragility_score(state_dof: f64) -> f64 {
    if state_dof.is_nan() {
        return 0.0;
    }
    clip01((TALEB_KURTOSIS_UNDEFINED_DOF - state_dof) / TALEB_KURTOSIS_UNDEFINED_DOF)
}

/// Result of `taleb_gate_pass`.
#[derive(Debug, Clone, PartialEq)]
pub struct TalebGatePass {
    pub pass: bool,
    pub eligible_states: Vec<usize>,
    pub fragile_states: Vec<usize>,
    /// `NaN` if `eligible_states` is empty (no eligible state to take a minimum over) --
    /// matches the Python original's `float("nan")` fallback exactly, not an `Option`, so a
    /// caller's own NaN-propagation/display logic doesn't need two different code paths.
    pub dof_min_eligible: f64,
}

/// The one function both `train_student_t_hmm.py`'s gang-metrics computation and
/// `regime_registry.py`'s taxonomy builder call for a pass/fail + diagnostic read on
/// Taleb-fragility. `pass == true` iff at least one eligible (not boundary-pinned, not
/// degenerately-empty) state has `dof < TALEB_KURTOSIS_UNDEFINED_DOF`.
pub fn taleb_gate_pass(
    dof_by_state: &[f64],
    sample_shares: &[f64],
    dof_min: f64,
    dof_max: f64,
    epsilon: f64,
    min_share_floor: f64,
) -> TalebGatePass {
    let eligible = eligible_fragile_states(dof_by_state, sample_shares, dof_min, dof_max, epsilon, min_share_floor);
    let fragile_states: Vec<usize> =
        eligible.iter().copied().filter(|&i| dof_by_state[i] < TALEB_KURTOSIS_UNDEFINED_DOF).collect();
    let eligible_dof: Vec<f64> = eligible.iter().map(|&i| dof_by_state[i]).collect();
    let dof_min_eligible =
        eligible_dof.iter().copied().fold(f64::INFINITY, f64::min);
    let dof_min_eligible = if eligible_dof.is_empty() { f64::NAN } else { dof_min_eligible };
    TalebGatePass { pass: !fragile_states.is_empty(), eligible_states: eligible, fragile_states, dof_min_eligible }
}

/// Per-state share of `counts[i] * |rel_range_component[i]|`, normalized to sum to 1 (all zero if
/// the total is non-positive). Diverges from the Python original's `(counts, means,
/// idx_rel_range)` signature deliberately: takes the already-extracted per-state
/// `relative_range` component directly, since this crate doesn't embed a dimension-index
/// convention the way the Python caller's `idx_rel_range` argument does -- the caller already
/// knows which column that is from whatever schema-derived constant source it's using.
pub fn compute_pareto_thrust_shares(counts: &[f64], rel_range_component: &[f64]) -> Vec<f64> {
    let contrib: Vec<f64> =
        counts.iter().zip(rel_range_component.iter()).map(|(&c, &m)| c * m.abs()).collect();
    let total: f64 = contrib.iter().sum();
    if total <= 0.0 {
        return vec![0.0; contrib.len()];
    }
    contrib.iter().map(|&c| c / total).collect()
}

/// Vectorized (single-pass) run-length encoding: the mean run-length ("tenure") of each state
/// across a decoded state sequence. Boundary detection via direct inequality (not a numeric
/// difference) -- same DOD lesson the Python original's own comment documents (comparing for
/// equality, not subtracting, means the boundary-detection step never needs to care what numeric
/// type the state labels happen to be, and never materializes an intermediate array one byte
/// wider than a bool would be). `states` may contain a `-1` "no prior" sentinel or any other
/// out-of-range id; runs with an invalid state are simply excluded from every state's totals.
pub fn mean_tenure_by_state(states: &[i32], n_states: usize) -> Vec<f64> {
    let mut means = vec![0.0; n_states];
    let n = states.len();
    if n == 0 {
        return means;
    }

    let mut state_totals = vec![0.0; n_states];
    let mut state_counts = vec![0u64; n_states];

    let mut run_start = 0usize;
    // One pass over `states`, closing out the current run the instant a boundary is found, plus
    // one final close-out after the loop for the run ending at the last element.
    for i in 1..n {
        if states[i] != states[i - 1] {
            let run_state = states[run_start];
            let run_len = (i - run_start) as f64;
            if run_state >= 0 && (run_state as usize) < n_states {
                state_totals[run_state as usize] += run_len;
                state_counts[run_state as usize] += 1;
            }
            run_start = i;
        }
    }
    let run_state = states[run_start];
    let run_len = (n - run_start) as f64;
    if run_state >= 0 && (run_state as usize) < n_states {
        state_totals[run_state as usize] += run_len;
        state_counts[run_state as usize] += 1;
    }

    for s in 0..n_states {
        if state_counts[s] > 0 {
            means[s] = state_totals[s] / state_counts[s] as f64;
        }
    }
    means
}

#[cfg(test)]
mod tests {
    use super::*;

    fn assert_close(a: f64, b: f64, tol: f64) {
        assert!((a - b).abs() < tol, "expected {b}, got {a} (tol {tol})");
    }

    // --- dof_boundary_check ---

    #[test]
    fn dof_boundary_check_flags_pinned_states() {
        let result = dof_boundary_check(&[2.11, 50.0, 99.98, 10.0], 2.1, 100.0, 0.05);
        assert_eq!(result.pinned_states, vec![0, 2]);
        assert!(!result.no_state_pinned_at_bounds);
    }

    #[test]
    fn dof_boundary_check_no_pinning() {
        let result = dof_boundary_check(&[5.0, 10.0, 50.0], 2.1, 100.0, 0.05);
        assert!(result.pinned_states.is_empty());
        assert!(result.no_state_pinned_at_bounds);
    }

    // --- state_occupancy_balance_check ---

    #[test]
    fn state_occupancy_balance_detects_degenerate_state() {
        let result = state_occupancy_balance_check(&[0.5, 0.48, 0.01, 0.01], 0.03);
        assert_close(result.min_sample_share, 0.01, 1e-12);
        assert!(!result.no_state_degenerately_empty);
    }

    #[test]
    fn state_occupancy_balance_all_healthy() {
        let result = state_occupancy_balance_check(&[0.3, 0.3, 0.2, 0.2], 0.03);
        assert!(result.no_state_degenerately_empty);
    }

    #[test]
    fn state_occupancy_balance_empty_input() {
        let result = state_occupancy_balance_check(&[], 0.03);
        assert_close(result.min_sample_share, 0.0, 1e-12);
    }

    // --- eligible_fragile_states ---

    #[test]
    fn eligible_fragile_states_excludes_pinned_and_degenerate_and_nan() {
        // state 0: pinned at dof_min; state 1: degenerate occupancy; state 2: NaN dof;
        // state 3: genuinely eligible.
        let dof = [2.1, 8.0, f64::NAN, 3.0];
        let shares = [0.3, 0.01, 0.3, 0.39];
        let eligible = eligible_fragile_states(&dof, &shares, 2.1, 100.0, 0.05, 0.03);
        assert_eq!(eligible, vec![3]);
    }

    // --- dof_fragility_score ---

    #[test]
    fn dof_fragility_score_boundaries() {
        assert_close(dof_fragility_score(0.0), 1.0, 1e-12); // max fragility at dof=0
        assert_close(dof_fragility_score(TALEB_KURTOSIS_UNDEFINED_DOF), 0.0, 1e-12);
        assert_close(dof_fragility_score(100.0), 0.0, 1e-12); // clipped, not negative
        assert_close(dof_fragility_score(f64::NAN), 0.0, 1e-12);
    }

    // --- taleb_gate_pass ---

    #[test]
    fn taleb_gate_passes_when_a_fragile_eligible_state_exists() {
        let dof = [2.1, 3.5, 50.0];
        let shares = [0.1, 0.4, 0.5];
        let result = taleb_gate_pass(&dof, &shares, 2.1, 100.0, 0.05, 0.03);
        assert!(result.pass);
        assert_eq!(result.fragile_states, vec![1]);
        assert_close(result.dof_min_eligible, 3.5, 1e-12);
    }

    #[test]
    fn taleb_gate_fails_when_no_eligible_state_is_fragile() {
        let dof = [50.0, 60.0];
        let shares = [0.5, 0.5];
        let result = taleb_gate_pass(&dof, &shares, 2.1, 100.0, 0.05, 0.03);
        assert!(!result.pass);
        assert!(result.fragile_states.is_empty());
    }

    #[test]
    fn taleb_gate_dof_min_eligible_is_nan_when_nothing_eligible() {
        let dof = [2.1, 2.09]; // both pinned at dof_min
        let shares = [0.5, 0.5];
        let result = taleb_gate_pass(&dof, &shares, 2.1, 100.0, 0.05, 0.03);
        assert!(result.eligible_states.is_empty());
        assert!(result.dof_min_eligible.is_nan());
    }

    // --- compute_pareto_thrust_shares ---

    #[test]
    fn pareto_thrust_shares_normalize_to_one() {
        let counts = [100.0, 200.0, 50.0];
        let rel_range = [0.5, -1.0, 2.0]; // sign shouldn't matter -- abs() is applied
        let shares = compute_pareto_thrust_shares(&counts, &rel_range);
        let sum: f64 = shares.iter().sum();
        assert_close(sum, 1.0, 1e-9);
        // state 1 has the largest |contribution| (200*1.0=200), should get the largest share.
        assert!(shares[1] > shares[0] && shares[1] > shares[2]);
    }

    #[test]
    fn pareto_thrust_shares_zero_when_total_non_positive() {
        let shares = compute_pareto_thrust_shares(&[0.0, 0.0], &[0.0, 0.0]);
        assert_eq!(shares, vec![0.0, 0.0]);
    }

    // --- mean_tenure_by_state ---

    #[test]
    fn mean_tenure_by_state_basic_runs() {
        // State 0: one run of length 3; state 1: one run of length 2. Means: 3.0 and 2.0.
        let states = [0, 0, 0, 1, 1];
        let means = mean_tenure_by_state(&states, 2);
        assert_close(means[0], 3.0, 1e-12);
        assert_close(means[1], 2.0, 1e-12);
    }

    #[test]
    fn mean_tenure_by_state_multiple_runs_averages_correctly() {
        // State 0: runs of length 2 and 4 -> mean 3.0. State 1: one run of length 1.
        let states = [0, 0, 1, 0, 0, 0, 0];
        let means = mean_tenure_by_state(&states, 2);
        assert_close(means[0], 3.0, 1e-12);
        assert_close(means[1], 1.0, 1e-12);
    }

    #[test]
    fn mean_tenure_by_state_ignores_invalid_sentinel() {
        // -1 ("no prior") runs are excluded from every state's totals.
        let states = [-1, -1, 0, 0, 0];
        let means = mean_tenure_by_state(&states, 2);
        assert_close(means[0], 3.0, 1e-12);
        assert_close(means[1], 0.0, 1e-12);
    }

    #[test]
    fn mean_tenure_by_state_empty_input() {
        let means = mean_tenure_by_state(&[], 3);
        assert_eq!(means, vec![0.0, 0.0, 0.0]);
    }

    #[test]
    fn mean_tenure_by_state_single_run_whole_sequence() {
        let states = [2, 2, 2, 2];
        let means = mean_tenure_by_state(&states, 3);
        assert_close(means[2], 4.0, 1e-12);
        assert_close(means[0], 0.0, 1e-12);
    }
}
