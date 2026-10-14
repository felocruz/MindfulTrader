//! bootstrap.rs -- parametric bootstrap for Student-t HMM gate-threshold CIs (plan Stage G3).
//! Ported from `lbrnet/models/hmm_parametric_bootstrap.py` (2026-09-21): simulates independent
//! regime-state paths and 2D Student-t emissions directly from a FITTED model's own
//! `transmat`/`startprob`/`means`/`covars`/`dof` (rather than resampling limited real history),
//! giving B=500-1000 genuinely independent draws of a window-level gate statistic. This
//! sidesteps the `N_eff >= 30` ceiling real market regime persistence (decorrelation length
//! ~5,000-10,000 bars) imposes on `calibration.rs`'s own nonparametric block bootstrap, which
//! even an 8-10M-row holdout lands right at (Zucchini, MacDonald & Langrock 2016, "Hidden Markov
//! Models for Time Series", Ch. 3.6; grounded via `lbrnet/logs/rc_gemini.log`
//! `CLAUDE_BRIEF_153`/`_REPLY`, 2026-09-21). Each simulated sequence is a genuinely independent
//! draw (no autocorrelation-driven effective-sample-size erosion), so B sequences give a real
//! `N_eff` of B, not ~8-30.
//!
//! Multivariate Student-t marginal property (why simulating only the 2 raw dims a statistic needs
//! is exact, not an approximation): a marginal of ANY subset of a multivariate Student-t's
//! dimensions is itself multivariate Student-t with the SAME degrees of freedom -- the shared
//! chi-square scaling factor is common across the whole vector, unlike a Gaussian, so
//! marginalizing never changes nu. `dim_a`/`dim_b` must not be a bullet-weighted
//! (`feature_weights != 1.0`) dim for this simple two-column form to stay exact -- a documented
//! precondition carried over from the real Python's own docstring, not re-checked here.
//!
//! Shares `gate_metrics::mean_tenure_by_state` and `calibration::percentile_sorted` instead of
//! duplicating either -- the real Python module's own flagged pre-existing duplication
//! (`_mean_tenure_by_state` copy-pasted from `regime_gate_metrics.py`) is fixed in this port, per
//! this crate's established "share, don't duplicate" principle.
//!
//! **Not bit-exact cross-language parity** (the same deliberately-flagged scope boundary
//! `init.rs`'s k-means++ seeding and `calibration.rs`'s stationary bootstrap already
//! established): the Markov-chain/Student-t-emission simulation here is statistically equivalent
//! to the real Python, not reproducible value-for-value from a shared seed -- no Rust crate or
//! hand-rolled generator buys real distribution-level parity with numpy's own generator anyway.
//! Validated instead via moment-matching (the gamma/chi-squared sampler's sample mean and
//! variance converge to the closed-form theoretical values across a large draw count) and
//! structural checks (state paths always land in `[0, K)`, CI bounds widen as `ci_size` grows,
//! the misspecification p-value is close to 1.0 when the "real" estimate is drawn from the same
//! simulated distribution, etc).

use crate::gate_metrics::mean_tenure_by_state;
use crate::init::SplitMix64;

/// Box-Muller (1958) standard normal draw. Discards the paired sine-branch value for simplicity
/// -- this module runs offline during calibration, not a live hot path, so redrawing two uniforms
/// per normal is an accepted, deliberate inefficiency, not an oversight.
pub(crate) fn standard_normal(rng: &mut SplitMix64) -> f64 {
    let u1 = rng.next_f64().max(f64::MIN_POSITIVE); // guard ln(0.0) = -inf at the u1=0.0 edge
    let u2 = rng.next_f64();
    (-2.0 * u1.ln()).sqrt() * (2.0 * std::f64::consts::PI * u2).cos()
}

/// Marsaglia & Tsang (2000) `Gamma(shape, scale=1)` sampler, valid for `shape >= 1.0`.
#[allow(clippy::many_single_char_names)] // d/c/x/v mirror the cited paper's own notation.
fn marsaglia_tsang_gamma(shape: f64, rng: &mut SplitMix64) -> f64 {
    debug_assert!(
        shape >= 1.0,
        "marsaglia_tsang_gamma requires shape >= 1.0; see gamma_sample's Ahrens-Dieter boost for shape < 1"
    );
    let d = shape - 1.0 / 3.0;
    let c = 1.0 / (9.0 * d).sqrt();
    loop {
        let x = standard_normal(rng);
        let v_cbrt = 1.0 + c * x;
        if v_cbrt <= 0.0 {
            continue;
        }
        let v = v_cbrt * v_cbrt * v_cbrt;
        let u = rng.next_f64();
        let x2 = x * x;
        if u < 1.0 - 0.0331 * x2 * x2 {
            return d * v;
        }
        if u.ln() < 0.5 * x2 + d * (1.0 - v + v.ln()) {
            return d * v;
        }
    }
}

/// `Gamma(shape, scale=1)` for any `shape > 0.0` -- boosts sub-1 shapes via Ahrens & Dieter
/// (1974): `Gamma(shape) = Gamma(shape+1) * U^(1/shape)`, `U ~ Uniform(0,1)`. Not strictly
/// required by this crate's own `dof_min=2.1` floor (`shape = dof/2 >= 1.05` always in practice),
/// included anyway so this sampler is correct over its full documented domain rather than
/// silently assuming a caller-side precondition it doesn't enforce.
fn gamma_sample(shape: f64, rng: &mut SplitMix64) -> f64 {
    if shape >= 1.0 {
        marsaglia_tsang_gamma(shape, rng)
    } else {
        let g = marsaglia_tsang_gamma(shape + 1.0, rng);
        let u = rng.next_f64().max(f64::MIN_POSITIVE);
        g * u.powf(1.0 / shape)
    }
}

/// Chi-squared(df) = 2 * Gamma(df/2, scale=1). Matches `numpy.random.Generator.chisquare`'s own
/// distributional definition (not its bit stream, see module doc).
pub(crate) fn chi_square_sample(df: f64, rng: &mut SplitMix64) -> f64 {
    2.0 * gamma_sample(df / 2.0, rng)
}

/// `searchsorted(cum, u, side="right")`-equivalent for a cumulative-probability array: the first
/// index `k` such that `cum[k] >= u`. Ported carefully from the real Python's own
/// `(u[:, None] > row_cum).sum(axis=1)` formula (a direct count of cumulative entries strictly
/// less than `u`, NOT `numpy.searchsorted`'s named convention despite the docstring borrowing
/// that name) -- the two differ at the exact-tie edge (`u` landing precisely on a cumulative
/// boundary), where the count formula keeps the LOWER state. A naive `u < cum[k]` (strict) loop
/// gives the wrong state at that edge; this uses `u <= cum[k]` to match.
fn sample_from_cumulative<const K: usize>(cum: &[f64; K], u: f64) -> usize {
    for (k, &c) in cum.iter().enumerate() {
        if u <= c {
            return k;
        }
    }
    K - 1
}

/// Simulate one independent discrete-time Markov chain state path of length `n_bars`
/// (post-burn-in). Ported from `simulate_state_path` -- the real Python vectorizes across B
/// sequences at once purely for NumPy performance, a detail invisible to the math; this Rust
/// port simulates ONE sequence per call, and callers wanting B independent sequences call this B
/// times with a shared, advancing `rng`.
///
/// Burn-in: draws the initial state from `startprob` then discards the first `burn_in` bars, so
/// the chain reaches its true stationary behavior before measurement -- `startprob` from a real
/// fit can be a training-window-specific artifact (observed directly on this project's own real
/// data: `startprob_[1]=4.3e-29`), not the genuine long-run stationary distribution.
pub fn simulate_state_path<const K: usize>(
    transmat: &[[f64; K]; K],
    startprob: &[f64; K],
    n_bars: usize,
    burn_in: usize,
    rng: &mut SplitMix64,
) -> Vec<usize> {
    let total_bars = burn_in + n_bars;
    let mut cum_start = [0.0; K];
    let mut running = 0.0;
    for k in 0..K {
        running += startprob[k];
        cum_start[k] = running;
    }

    let mut cum_trans = [[0.0; K]; K];
    for i in 0..K {
        let mut r = 0.0;
        for j in 0..K {
            r += transmat[i][j];
            cum_trans[i][j] = r;
        }
    }

    let mut states = Vec::with_capacity(total_bars);
    states.push(sample_from_cumulative(&cum_start, rng.next_f64()));
    for _ in 1..total_bars {
        let prev = *states.last().expect("just pushed the initial state above");
        states.push(sample_from_cumulative(&cum_trans[prev], rng.next_f64()));
    }

    states.split_off(burn_in)
}

/// Simulate 2 raw-feature columns' emissions conditioned on a given state path, from the fitted
/// diagonal-covariance multivariate Student-t (matches `model.rs`'s own emission density: shared
/// per-timestep chi-square scaling `W ~ Chi2(nu_state)`, independent standard-normal draws per
/// dim, `x_d = mean_d + sqrt(var_d) * Z_d * sqrt(nu/W)`). The `1e-300` covariance floor below is
/// the real Python's own (a bare "never sqrt a non-positive number" guard, not
/// `model::MIN_COVAR`'s `1e-12` numerical-stability floor used elsewhere in this crate) --
/// deliberately kept as the file being ported actually wrote it, not harmonized with `model.rs`'s
/// own convention.
#[allow(clippy::too_many_arguments)] // one named parameter per real model field/argument this
// function needs; a struct wrapper would hide which fields are actually required, not simplify
// anything (same rationale `model.rs`'s `StudentTHmm::new` already documents for itself).
pub fn simulate_student_t_emissions_2d<const D: usize, const K: usize>(
    states: &[usize],
    dim_a: usize,
    dim_b: usize,
    means: &[[f64; D]; K],
    covars: &[[f64; D]; K],
    dof: &[f64; K],
    dof_min: f64,
    dof_max: f64,
    rng: &mut SplitMix64,
) -> (Vec<f64>, Vec<f64>) {
    let n = states.len();
    let mut vals_a = Vec::with_capacity(n);
    let mut vals_b = Vec::with_capacity(n);
    for &s in states {
        let nu = dof[s].clamp(dof_min, dof_max);
        let w = chi_square_sample(nu, rng);
        let scale_factor = (nu / w).sqrt();
        let std_a = covars[s][dim_a].max(1e-300).sqrt();
        let std_b = covars[s][dim_b].max(1e-300).sqrt();
        let z_a = standard_normal(rng);
        let z_b = standard_normal(rng);
        vals_a.push(means[s][dim_a] + std_a * z_a * scale_factor);
        vals_b.push(means[s][dim_b] + std_b * z_b * scale_factor);
    }
    (vals_a, vals_b)
}

/// Replicates `_windowed_pareto_top_state_ratio`'s exact per-window statistic: per-state window
/// OCCUPANCY COUNT weighted by that state's fitted (fixed, model-level, not per-window) `|mean
/// relative_range|` -- NOT a flat unweighted occupancy share -- then how many top states (by this
/// weighted contribution) are needed to reach 70% of the total, divided by `K`. `rel_range_means`
/// is the model's own `means[:, rel_range_idx]` (a length-`K` constant vector, matching the real
/// `extract_means_by_state(model)[:, rel_range_idx]` exactly), not something simulated per-window.
pub fn windowed_pareto_top_state_ratio<const K: usize>(
    states: &[usize],
    window_rows: usize,
    rel_range_means: &[f64; K],
    quantile: f64,
) -> f64 {
    let n_windows = states.len() / window_rows;
    if n_windows == 0 {
        return f64::NAN;
    }
    let mut ratios = Vec::with_capacity(n_windows);
    for w in 0..n_windows {
        let seg = &states[w * window_rows..(w + 1) * window_rows];
        let mut counts = [0.0f64; K];
        for &s in seg {
            counts[s] += 1.0;
        }
        let contrib: [f64; K] = std::array::from_fn(|k| counts[k] * rel_range_means[k].abs());
        let total: f64 = contrib.iter().sum();
        if total <= 0.0 {
            ratios.push(1.0);
            continue;
        }
        let mut shares: Vec<f64> = contrib.iter().map(|&c| c / total).collect();
        shares.sort_by(|a, b| b.total_cmp(a));
        let mut running = 0.0;
        let mut top_states_for_70 = K;
        for (i, share) in shares.iter().enumerate() {
            running += share;
            if running >= 0.70 {
                top_states_for_70 = i + 1;
                break;
            }
        }
        ratios.push(top_states_for_70 as f64 / (K.max(1) as f64));
    }
    ratios.sort_by(f64::total_cmp);
    crate::calibration::percentile_sorted(&ratios, quantile)
}

/// Replicates `_windowed_shannon_mean_tenure`'s exact per-window statistic: `mean_tenure_by_state`
/// (per-state mean run-length) averaged ACROSS STATES (a simple mean of the `n_states`-length
/// vector, not weighted by frequency) -- then the target quantile across windows.
pub fn windowed_mean_tenure_quantile(
    states: &[usize],
    window_rows: usize,
    n_states: usize,
    quantile: f64,
) -> f64 {
    let n_windows = states.len() / window_rows;
    if n_windows == 0 {
        return f64::NAN;
    }
    let mut tenures = Vec::with_capacity(n_windows);
    for w in 0..n_windows {
        let seg: Vec<i32> = states[w * window_rows..(w + 1) * window_rows]
            .iter()
            .map(|&s| s as i32)
            .collect();
        let per_state = mean_tenure_by_state(&seg, n_states);
        let mean_across_states = if per_state.is_empty() {
            0.0
        } else {
            per_state.iter().sum::<f64>() / per_state.len() as f64
        };
        tenures.push(mean_across_states);
    }
    tenures.sort_by(f64::total_cmp);
    crate::calibration::percentile_sorted(&tenures, quantile)
}

/// Replicates `_windowed_taleb_signal_sigma`'s exact per-window statistic: `combined_signal =
/// max(robust_z(vol_conv), robust_z(tail))` per row, then `taleb_signal_sigma` = max OVER STATES
/// of (95th percentile of `combined_signal` restricted to that state's rows within the window) --
/// NOT a flat quantile over all rows in the window. Uses the SAME real-data robust location/scale
/// references (`ref_a`/`ref_b`, each `(center, scale)`) as the real calibration run -- the
/// z-score's absolute scale must not shift between real and simulated scoring, or the
/// misspecification comparison would be meaningless.
#[allow(clippy::too_many_arguments)]
pub fn windowed_taleb_signal_sigma(
    vals_a: &[f64],
    vals_b: &[f64],
    states: &[usize],
    window_rows: usize,
    n_states: usize,
    quantile: f64,
    ref_a: (f64, f64),
    ref_b: (f64, f64),
) -> f64 {
    let n_windows = vals_a.len() / window_rows;
    if n_windows == 0 {
        return f64::NAN;
    }
    let mut sigmas = Vec::with_capacity(n_windows);
    for w in 0..n_windows {
        let start = w * window_rows;
        let end = start + window_rows;
        let combined: Vec<f64> = (start..end)
            .map(|i| {
                let z_a = (vals_a[i] - ref_a.0) / ref_a.1;
                let z_b = (vals_b[i] - ref_b.0) / ref_b.1;
                z_a.max(z_b)
            })
            .collect();
        let seg_states = &states[start..end];
        let mut best = f64::NEG_INFINITY;
        for sid in 0..n_states {
            let mut masked: Vec<f64> = seg_states
                .iter()
                .zip(combined.iter())
                .filter(|&(&s, _)| s == sid)
                .map(|(_, &v)| v)
                .collect();
            if masked.is_empty() {
                continue;
            }
            masked.sort_by(f64::total_cmp);
            best = best.max(crate::calibration::percentile_sorted(&masked, 95.0));
        }
        sigmas.push(best);
    }
    sigmas.sort_by(f64::total_cmp);
    crate::calibration::percentile_sorted(&sigmas, quantile)
}

/// Orchestrates B independent draws of a scalar gate statistic into a percentile CI, plus the
/// misspecification sanity check (Gemini's recommendation, `CLAUDE_BRIEF_153`): where does the
/// REAL holdout's own point estimate fall in the simulated distribution -- a frequentist
/// posterior-predictive-style p-value, not just an assumption. Large p means the real value sits
/// comfortably inside the model-implied distribution (model structurally consistent with reality
/// for this statistic); small p means the fitted HMM fails to capture whatever produced the real
/// value.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ParametricBootstrapGateCi {
    pub ci_low: f64,
    pub ci_high: f64,
    pub ci_size: f64,
    pub n_sequences: usize,
    pub mean: f64,
    pub std: f64,
    pub real_point_estimate: Option<f64>,
    pub misspecification_pvalue: Option<f64>,
    pub misspecification_flagged: Option<bool>,
}

pub fn parametric_bootstrap_gate_ci(
    draws: &[f64],
    ci_size: f64,
    real_point_estimate: Option<f64>,
) -> ParametricBootstrapGateCi {
    let n = draws.len();
    let mut sorted = draws.to_vec();
    sorted.sort_by(f64::total_cmp);

    let alpha = (1.0 - ci_size) / 2.0;
    let ci_low = crate::calibration::percentile_sorted(&sorted, 100.0 * alpha);
    let ci_high = crate::calibration::percentile_sorted(&sorted, 100.0 * (1.0 - alpha));

    let mean = draws.iter().sum::<f64>() / n as f64;
    // Population variance (ddof=0), matching `numpy.std`'s own default.
    let variance = draws.iter().map(|&x| (x - mean).powi(2)).sum::<f64>() / n as f64;
    let std = variance.sqrt();

    let (misspecification_pvalue, misspecification_flagged) = match real_point_estimate {
        Some(real) => {
            let real_dev = (real - mean).abs();
            let count = draws.iter().filter(|&&x| (x - mean).abs() >= real_dev).count();
            let p = count as f64 / n as f64;
            (Some(p), Some(p < 0.05))
        }
        None => (None, None),
    };

    ParametricBootstrapGateCi {
        ci_low,
        ci_high,
        ci_size,
        n_sequences: n,
        mean,
        std,
        real_point_estimate,
        misspecification_pvalue,
        misspecification_flagged,
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // --- RNG primitives: moment-matching, not golden-parity (see module doc) ---

    #[test]
    fn standard_normal_moments_converge() {
        let mut rng = SplitMix64::new(42);
        let n = 200_000;
        let draws: Vec<f64> = (0..n).map(|_| standard_normal(&mut rng)).collect();
        let mean = draws.iter().sum::<f64>() / n as f64;
        let var = draws.iter().map(|&x| (x - mean).powi(2)).sum::<f64>() / n as f64;
        assert!(mean.abs() < 0.02, "mean={mean}");
        assert!((var - 1.0).abs() < 0.03, "var={var}");
    }

    #[test]
    fn chi_square_moments_converge_for_several_dof() {
        for &df in &[2.1, 4.0, 10.0, 30.0] {
            let mut rng = SplitMix64::new(7);
            let n = 200_000;
            let draws: Vec<f64> = (0..n).map(|_| chi_square_sample(df, &mut rng)).collect();
            let mean = draws.iter().sum::<f64>() / n as f64;
            let var = draws.iter().map(|&x| (x - mean).powi(2)).sum::<f64>() / n as f64;
            // Chi2(df): E[X]=df, Var[X]=2*df.
            let rel_mean_err = (mean - df).abs() / df;
            let rel_var_err = (var - 2.0 * df).abs() / (2.0 * df);
            assert!(rel_mean_err < 0.02, "df={df} mean={mean} expected={df}");
            assert!(rel_var_err < 0.05, "df={df} var={var} expected={}", 2.0 * df);
        }
    }

    #[test]
    fn gamma_sample_sub_one_shape_moments_converge() {
        // Exercises the Ahrens-Dieter boost path (shape < 1), not reachable via chi_square_sample
        // at this crate's own dof_min=2.1 floor (shape=dof/2>=1.05) -- tested directly so the
        // boost path isn't silently unverified dead code.
        let shape = 0.4;
        let mut rng = SplitMix64::new(99);
        let n = 200_000;
        let draws: Vec<f64> = (0..n).map(|_| gamma_sample(shape, &mut rng)).collect();
        let mean = draws.iter().sum::<f64>() / n as f64;
        let var = draws.iter().map(|&x| (x - mean).powi(2)).sum::<f64>() / n as f64;
        // Gamma(shape, scale=1): E[X]=shape, Var[X]=shape.
        assert!((mean - shape).abs() / shape < 0.05, "mean={mean}");
        assert!((var - shape).abs() / shape < 0.10, "var={var}");
    }

    // --- Markov chain simulation: structural correctness ---

    #[test]
    fn simulate_state_path_always_in_range_and_correct_length() {
        let transmat = [[0.9, 0.1], [0.2, 0.8]];
        let startprob = [0.5, 0.5];
        let mut rng = SplitMix64::new(1);
        let path = simulate_state_path(&transmat, &startprob, 1_000, 50, &mut rng);
        assert_eq!(path.len(), 1_000);
        assert!(path.iter().all(|&s| s < 2));
    }

    #[test]
    fn simulate_state_path_absorbing_chain_stays_put() {
        // An absorbing state (row [0,1] means state 0 always transitions to state 1, then state
        // 1 always stays at state 1) exercises sample_from_cumulative's exact-boundary behavior
        // deterministically, without relying on a specific RNG draw landing exactly on a tie.
        let transmat = [[0.0, 1.0], [0.0, 1.0]];
        let startprob = [1.0, 0.0];
        let mut rng = SplitMix64::new(3);
        let path = simulate_state_path(&transmat, &startprob, 20, 1, &mut rng);
        // After burn-in (>=1 transition), the chain must have reached the absorbing state 1.
        assert!(path.iter().all(|&s| s == 1), "path={path:?}");
    }

    #[test]
    fn sample_from_cumulative_exact_tie_keeps_lower_state() {
        let cum = [0.2, 0.5, 1.0];
        assert_eq!(sample_from_cumulative(&cum, 0.2), 0);
        assert_eq!(sample_from_cumulative(&cum, 0.2000001), 1);
        assert_eq!(sample_from_cumulative(&cum, 1.0), 2);
    }

    // --- Student-t emission simulation ---

    #[test]
    fn simulate_student_t_emissions_2d_moments_converge_to_state_means() {
        let means = [[0.0, 10.0], [5.0, -3.0]];
        let covars = [[1.0, 4.0], [2.0, 0.5]];
        let dof = [30.0, 30.0]; // high dof: Student-t approx Gaussian, variance ~ finite and tame
        let mut rng = SplitMix64::new(11);
        let states: Vec<usize> = vec![0; 50_000];
        let (vals_a, vals_b) =
            simulate_student_t_emissions_2d(&states, 0, 1, &means, &covars, &dof, 2.1, 100.0, &mut rng);
        let mean_a = vals_a.iter().sum::<f64>() / vals_a.len() as f64;
        let mean_b = vals_b.iter().sum::<f64>() / vals_b.len() as f64;
        assert!((mean_a - 0.0).abs() < 0.1, "mean_a={mean_a}");
        assert!((mean_b - 10.0).abs() < 0.1, "mean_b={mean_b}");
    }

    #[test]
    fn simulate_student_t_emissions_2d_handles_zero_covar() {
        let means = [[1.0, 2.0]];
        let covars = [[0.0, 0.0]]; // floored at 1e-300, must not produce NaN
        let dof = [2.1];
        let mut rng = SplitMix64::new(5);
        let states = vec![0usize; 10];
        let (vals_a, vals_b) =
            simulate_student_t_emissions_2d(&states, 0, 1, &means, &covars, &dof, 2.1, 100.0, &mut rng);
        assert!(vals_a.iter().all(|v| v.is_finite()));
        assert!(vals_b.iter().all(|v| v.is_finite()));
    }

    // --- Windowed statistics ---

    #[test]
    fn windowed_pareto_top_state_ratio_single_dominant_state_is_small() {
        // All rows in state 0 -> one state carries 100% of the weighted contribution -> ratio
        // should be the minimum possible, 1/K.
        let states = vec![0usize; 1_000];
        let rel_range_means = [1.0, 1.0, 1.0, 1.0];
        let ratio = windowed_pareto_top_state_ratio(&states, 100, &rel_range_means, 50.0);
        assert!((ratio - 0.25).abs() < 1e-9, "ratio={ratio}");
    }

    #[test]
    fn windowed_pareto_top_state_ratio_too_short_for_one_window_is_nan() {
        let states = vec![0usize; 5];
        let rel_range_means = [1.0, 1.0];
        let ratio = windowed_pareto_top_state_ratio(&states, 100, &rel_range_means, 50.0);
        assert!(ratio.is_nan());
    }

    #[test]
    fn windowed_mean_tenure_quantile_long_runs_give_high_tenure() {
        // One run of length 1000 per window -> mean_tenure_by_state's only active state has
        // tenure == window_rows itself (a single run spanning the whole segment).
        let states = vec![0usize; 1_000];
        let tenure = windowed_mean_tenure_quantile(&states, 1_000, 2, 50.0);
        // n_states=2, only state 0 occupied (tenure=1000), state 1 untouched (tenure=0) ->
        // mean across states = 500.0.
        assert!((tenure - 500.0).abs() < 1e-9, "tenure={tenure}");
    }

    #[test]
    fn windowed_taleb_signal_sigma_higher_z_state_wins() {
        // Two states: state 0's rows cluster near ref (low z), state 1's rows are far away
        // (high z) -- the max-over-states result must reflect state 1's higher sigma.
        let n = 200;
        let states: Vec<usize> = (0..n).map(|i| if i < n / 2 { 0 } else { 1 }).collect();
        let vals_a: Vec<f64> = (0..n)
            .map(|i| if i < n / 2 { 0.0 } else { 20.0 })
            .collect();
        let vals_b = vec![0.0f64; n];
        let sigma = windowed_taleb_signal_sigma(&vals_a, &vals_b, &states, n, 2, 50.0, (0.0, 1.0), (0.0, 1.0));
        assert!(sigma > 10.0, "sigma={sigma}");
    }

    // --- CI orchestration ---

    #[test]
    fn parametric_bootstrap_gate_ci_widens_with_larger_ci_size() {
        let mut rng = SplitMix64::new(21);
        let draws: Vec<f64> = (0..5_000).map(|_| standard_normal(&mut rng)).collect();
        let ci_90 = parametric_bootstrap_gate_ci(&draws, 0.90, None);
        let ci_99 = parametric_bootstrap_gate_ci(&draws, 0.99, None);
        assert!(ci_99.ci_high - ci_99.ci_low > ci_90.ci_high - ci_90.ci_low);
    }

    #[test]
    fn parametric_bootstrap_gate_ci_misspecification_pvalue_high_for_typical_real_estimate() {
        let mut rng = SplitMix64::new(33);
        let draws: Vec<f64> = (0..5_000).map(|_| standard_normal(&mut rng)).collect();
        let mean = draws.iter().sum::<f64>() / draws.len() as f64;
        // The real estimate IS the simulated mean -- the most "typical" possible value, so the
        // p-value must be high (not flagged as model misspecification).
        let result = parametric_bootstrap_gate_ci(&draws, 0.95, Some(mean));
        assert!(result.misspecification_pvalue.expect("Some(mean) supplied") > 0.9);
        assert_eq!(result.misspecification_flagged, Some(false));
    }

    #[test]
    fn parametric_bootstrap_gate_ci_misspecification_pvalue_low_for_extreme_real_estimate() {
        let mut rng = SplitMix64::new(33);
        let draws: Vec<f64> = (0..5_000).map(|_| standard_normal(&mut rng)).collect();
        // A far-outlier "real" estimate must be flagged.
        let result = parametric_bootstrap_gate_ci(&draws, 0.95, Some(50.0));
        assert!(result.misspecification_pvalue.expect("Some(50.0) supplied") < 0.05);
        assert_eq!(result.misspecification_flagged, Some(true));
    }

    #[test]
    fn parametric_bootstrap_gate_ci_no_real_estimate_leaves_misspecification_none() {
        let draws = vec![1.0, 2.0, 3.0, 4.0, 5.0];
        let result = parametric_bootstrap_gate_ci(&draws, 0.95, None);
        assert!(result.misspecification_pvalue.is_none());
        assert!(result.misspecification_flagged.is_none());
    }
}
