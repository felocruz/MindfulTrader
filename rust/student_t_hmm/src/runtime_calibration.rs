//! runtime_calibration.rs -- temporal half-life calibration from a trained transition matrix
//! (plan Stage G6's unblocked half; `calibration.rs`'s Geyer tau_int / stationary block-bootstrap
//! methodology is the other, still blocked on G0(e)'s `arch`-crate spike). Ported from
//! `lbrnet/analytics/regime_calibration.py`'s `expected_state_durations_steps`/
//! `calibrate_temporal_half_life` -- the pure, once-per-training-run math only. Deliberately NOT
//! ported: `estimate_context_step_seconds`/`load_hmm_transition_matrix` (real `.context`-file I/O,
//! a separate concern -- `step_seconds` is a parameter here, supplied by whichever caller already
//! has it) and `write_agent_state_bin`/`read_agent_state_half_life` (FlatBuffer `Diagnostic`
//! artifact persistence -- belongs with `artifact.rs`'s own manifest-writing territory if/when
//! this crate needs to persist a calibration result, not duplicated here).
//!
//! Golden-parity validated against a real invocation of the actual Python
//! `calibrate_temporal_half_life`/`expected_state_durations_steps` (mts conda env, 2026-10-12) --
//! see this module's own tests.

/// `E[duration in state k] = 1 / (1 - P(k -> k))` for every state, clamping the self-transition
/// probability to `[0, 0.999999]` first (an exact `1.0` self-transition would make this infinite)
/// and the resulting denominator to a `1e-6` floor. Ported verbatim from
/// `expected_state_durations_steps`.
pub fn expected_state_durations_steps<const K: usize>(transmat: &[[f64; K]; K]) -> [f64; K] {
    let mut out = [0.0; K];
    for k in 0..K {
        let diag = transmat[k][k].clamp(0.0, 0.999999);
        out[k] = 1.0 / (1.0 - diag).max(1e-6);
    }
    out
}

/// Result of `calibrate_temporal_half_life` -- the numeric fields `regime_calibration.py`'s own
/// dict carries, minus provenance/debug fields this crate doesn't need (`transition_matrix` is
/// the caller's own input, already in hand; `context_path`/`hmm_model_path`/`calibrated_at_us` are
/// either redundant or non-deterministic/wall-clock-derived, same reasoning spec §5.5 already
/// applies to `cognitive_load`).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TemporalHalfLifeCalibration {
    pub half_life_seconds: f64,
    pub mean_duration_steps: f64,
    pub mean_duration_seconds: f64,
    pub step_seconds: f64,
}

/// Ported verbatim from `calibrate_temporal_half_life`: a single scalar "how fast should this
/// model's signal be treated as decaying" calibrated from the trained transition matrix's own
/// average state persistence, in real seconds (via the caller-supplied `step_seconds` -- the
/// average wall-clock time between consecutive trigger-gated observations, estimated from real
/// `.context` data elsewhere, not here). Floors at `0.1` seconds; falls back to
/// `default_half_life_sec` if the result is ever non-finite (e.g. a non-finite `step_seconds`).
pub fn calibrate_temporal_half_life<const K: usize>(
    transmat: &[[f64; K]; K],
    step_seconds: f64,
    default_half_life_sec: f64,
) -> TemporalHalfLifeCalibration {
    let durations_steps = expected_state_durations_steps(transmat);
    let mean_duration_steps = durations_steps.iter().sum::<f64>() / K as f64;
    let mean_duration_seconds = mean_duration_steps * step_seconds;
    let mut half_life_seconds = (mean_duration_seconds / 2.0).max(0.1);
    if !half_life_seconds.is_finite() {
        half_life_seconds = default_half_life_sec;
    }
    TemporalHalfLifeCalibration {
        half_life_seconds,
        mean_duration_steps,
        mean_duration_seconds,
        step_seconds,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn check(name: &str, got: f64, want: f64, tol: f64) {
        assert!((got - want).abs() < tol, "{name}: got {got}, want {want} (tol {tol})");
    }

    /// Golden fixture: real `expected_state_durations_steps`/`calibrate_temporal_half_life`
    /// invocation (mts conda env, 2026-10-12) against a hand-built 4-state transition matrix.
    const TRANSMAT: [[f64; 4]; 4] = [
        [0.90, 0.05, 0.03, 0.02],
        [0.10, 0.80, 0.05, 0.05],
        [0.20, 0.10, 0.60, 0.10],
        [0.01, 0.01, 0.01, 0.97],
    ];

    #[test]
    fn golden_expected_state_durations_steps() {
        let durations = expected_state_durations_steps(&TRANSMAT);
        let want = [10.000000000000002, 5.000000000000001, 2.5, 33.33333333333331];
        for k in 0..4 {
            check(&format!("duration[{k}]"), durations[k], want[k], 1e-9);
        }
    }

    #[test]
    fn golden_calibrate_temporal_half_life_varies_with_step_seconds() {
        // mean_duration_steps = 12.708333333333329 (hand-verified against the real Python run)
        for (step_seconds, want_mean_duration_seconds, want_half_life) in [
            (1.0, 12.708333333333329, 6.354166666666664),
            (2.5, 31.77083333333332, 15.88541666666666),
            (60.0, 762.4999999999998, 381.2499999999999),
        ] {
            let cal = calibrate_temporal_half_life(&TRANSMAT, step_seconds, 5.0);
            check("mean_duration_steps", cal.mean_duration_steps, 12.708333333333329, 1e-9);
            check("mean_duration_seconds", cal.mean_duration_seconds, want_mean_duration_seconds, 1e-6);
            check("half_life_seconds", cal.half_life_seconds, want_half_life, 1e-6);
            check("step_seconds_roundtrip", cal.step_seconds, step_seconds, 1e-12);
        }
    }

    #[test]
    fn golden_clipped_diagonal_near_one_and_floor_behavior() {
        // A near-certain self-transition (0.9999999, clipped to 0.999999) paired with a fast-mixing
        // state -- matches the real Python run's `clipped-diagonal durations_steps` fixture.
        let transmat: [[f64; 2]; 2] = [[0.9999999, 0.0000001], [0.5, 0.5]];
        let durations = expected_state_durations_steps(&transmat);
        check("duration[0]_near_clip_bound", durations[0], 999999.9999712444, 1e-3);
        check("duration[1]", durations[1], 2.0, 1e-9);

        // Tiny step_seconds still floors half_life at 0.1, never below.
        let cal = calibrate_temporal_half_life(&transmat, 1e-9, 5.0);
        check("floored_half_life", cal.half_life_seconds, 0.1, 1e-9);
    }

    #[test]
    fn nan_step_seconds_is_laundered_to_the_floor_not_the_default() {
        // Real, verified-against-Python behavior, not a bug: `f64::max`/Python's built-in `max()`
        // both treat NaN as "the other operand wins" (neither is IEEE-754 minNum/maxNum), so
        // `(NaN).max(0.1)` resolves to `0.1` -- already finite -- before the `is_finite()` check
        // ever runs. The `default_half_life_sec` fallback branch is therefore NEVER reachable via
        // a NaN `step_seconds`; only a genuine `+Infinity` propagates past the floor as non-finite
        // (next test). Confirmed identical in real Python (mts conda env, 2026-10-12): NaN
        // `step_seconds` also silently returns `0.1` there, not the default.
        let cal = calibrate_temporal_half_life(&TRANSMAT, f64::NAN, 5.0);
        check("nan_lands_on_floor", cal.half_life_seconds, 0.1, 1e-12);
    }

    #[test]
    fn infinite_step_seconds_falls_back_to_default() {
        // Unlike NaN (previous test), `+Infinity` is a genuine ordering winner over `0.1` under
        // `f64::max`, so it survives to the `is_finite()` check and triggers the real fallback --
        // verified against real Python (mts conda env, 2026-10-12).
        let cal = calibrate_temporal_half_life(&TRANSMAT, f64::INFINITY, 5.0);
        check("falls_back_to_default", cal.half_life_seconds, 5.0, 1e-12);
    }

    #[test]
    fn single_state_self_transition_one_clamps_without_panicking() {
        // A degenerate 1-state model with a literal 1.0 self-transition -- the clamp to 0.999999
        // plus the 1e-6 denominator floor must prevent a divide-by-zero/infinite result.
        let transmat: [[f64; 1]; 1] = [[1.0]];
        let durations = expected_state_durations_steps(&transmat);
        assert!(durations[0].is_finite());
        check("single_state_duration", durations[0], 1.0 / 0.000001, 1e-3);
    }
}
