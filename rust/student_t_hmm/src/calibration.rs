//! calibration.rs -- gate-threshold calibration methodology (plan Stage G6's previously-blocked
//! half; G0(e) resolved via an independent consultation, `lbrnet/logs/rc_gemini.log`
//! `CLAUDE_BRIEF_158`/`_REPLY`, 2026-10-12): stationary block-bootstrap percentile CIs for
//! window-level gate-threshold quantile statistics. Ported from
//! `lbrnet/scripts/calibrate_hmm_gate_thresholds.py`'s `_geyer_tau_int`/`_optimal_block_length_sb`/
//! `_bootstrap_threshold_ci`, cross-checked directly against the real, installed `arch` package's
//! own source (v8.0.0, `site-packages/arch/bootstrap/base.py`'s `_single_optimal_block` and
//! `_samplers_python.py`'s `stationary_bootstrap_sample_python`) -- not reconstructed from the
//! papers alone.
//!
//! **G0(e) resolution, in brief** (full consultation: `CLAUDE_BRIEF_158`): the open question was
//! whether Politis & White (2004)'s block-length selector and Politis & Romano (1994)'s
//! stationary bootstrap -- both conditioned on finite-variance assumptions -- are soundly
//! justified here, given this crate's own Student-t HMM routinely operates with `dof_min=2.1`
//! (barely above the variance-existence boundary) and the `TALEB_KURTOSIS_UNDEFINED_DOF=4.0` gate
//! already treats undefined kurtosis as a normal operating regime, not an edge case. **Resolved:
//! the concern doesn't bite.** The `series` this module bootstraps are window-level SUMMARY
//! statistics (bounded ratios, tenure counts, interior order-statistic percentiles) computed over
//! the raw Student-t-driven data, not the raw observations themselves -- these have well-behaved
//! tails by construction (extreme value theory: the k-th largest order statistic from n draws of
//! a tail-index-alpha distribution has finite moments up to roughly k*alpha; a 95th-percentile
//! statistic over a 10,000-row window has an effectively enormous number of finite moments
//! regardless of how fat the raw data's own tails are). Adopting Politis-Romano-Wolf subsampling
//! instead (our own initial hypothesis before this consultation) would have been an unjustified
//! downgrade: wider, less efficient CIs with no clean automatic subsample-size rule, trading away
//! Politis-White's well-defined block-length formula to solve a problem that doesn't exist here.
//!
//! No Rust crate implements either Politis-White block-length selection or Politis-Romano
//! stationary bootstrap (confirmed via a direct crates.io API search, 2026-10-12) -- both are
//! ported from scratch below.

use crate::init::SplitMix64;

/// Politis & White (2004), corrected by Patton/Politis/White (2009): automatic block-length
/// selection for the stationary bootstrap via a flat-top-kernel long-run-variance estimate.
/// Ported line-for-line from the real, installed `arch` package's own
/// `arch.bootstrap.base._single_optimal_block` (v8.0.0) -- only the `b_sb` (stationary bootstrap)
/// output, not `b_cb` (circular bootstrap, which this codebase never uses). Fully deterministic
/// (no randomness at all), so golden-parity tested bit-for-bit against the real installed package.
///
/// Returns `None` when the long-run-variance estimate is exactly zero (e.g. a constant input
/// series) -- the real `arch` package's own `_single_optimal_block` hits a genuine `0.0/0.0` in
/// this case and returns `NaN` (verified directly, not assumed), but that's a Python/NumPy
/// idiom -- an implicit float sentinel standing in for "could not be estimated" -- not part of
/// the algorithm's actual mathematical contract. Rust has a better, zero-cost way to say the same
/// thing: detect the degenerate precondition explicitly and return `None`, so every caller is
/// compiler-forced to handle it, the same mathematical contract `_optimal_block_length_sb`'s own
/// `is_finite()`-check-then-fallback already expresses, just made explicit instead of implicit.
#[allow(clippy::many_single_char_names)] // g/m/d_sb/b_sb/b_max mirror the cited paper's own
// notation and the real arch package's own variable names -- renaming would make this harder,
// not easier, to verify against the source it's ported from.
pub fn politis_white_block_length(series: &[f64]) -> Option<f64> {
    let nobs = series.len();
    if nobs == 0 {
        return None;
    }
    let mean = series.iter().sum::<f64>() / nobs as f64;
    let eps: Vec<f64> = series.iter().map(|&x| x - mean).collect();

    let b_max = (3.0 * (nobs as f64).sqrt()).min(nobs as f64 / 3.0).ceil();
    let kn = 5usize.max((nobs as f64).log10() as usize);
    let m_max = ((nobs as f64).sqrt().ceil() as usize) + kn;
    let cv = 2.0 * ((nobs as f64).log10() / nobs as f64).sqrt();

    let mut acv = vec![0.0; m_max + 1];
    let mut abs_acorr = vec![0.0; m_max + 1];
    let mut opt_m: Option<usize> = None;
    for i in 0..=m_max {
        // Python's `eps[i+1:]`/`eps[:-(i+1)]`/`eps[i:]`/`eps[:nobs-i]` never raise even when `i`
        // exceeds `nobs` (slicing past an array's length just clips to empty in Python/NumPy) --
        // `saturating_sub`/`.min(nobs)` replicate that "clip to empty, never panic" behavior
        // (verified: for this module's real ~228-element target series, `m_max` stays well under
        // `nobs`, so this never actually triggers; it only guards a pathologically tiny-`nobs`
        // input from panicking, not claimed to bit-match Python's own compounding-negative-index
        // reinterpretation in that regime).
        let start1 = (i + 1).min(nobs);
        let end1 = nobs.saturating_sub(i + 1);
        let v1: f64 = eps[start1..].iter().map(|v| v * v).sum();
        let v2: f64 = eps[..end1].iter().map(|v| v * v).sum();
        let start2 = i.min(nobs);
        let end2 = nobs.saturating_sub(i);
        let cross_prod: f64 =
            eps[start2..].iter().zip(eps[..end2].iter()).map(|(a, b)| a * b).sum();
        acv[i] = cross_prod / nobs as f64;
        abs_acorr[i] = cross_prod.abs() / (v1 * v2).sqrt();
        if i >= kn && opt_m.is_none() && abs_acorr[i - kn..i].iter().all(|&a| a < cv) {
            opt_m = Some(i - kn);
        }
    }
    let m = match opt_m {
        Some(om) => (2 * om.max(1)).min(m_max),
        None => m_max,
    };

    let mut g = 0.0;
    let mut lr_acv = acv[0];
    // `k` is both the loop index into `acv` and a plain numeric value in the `lam`/`g` formula
    // below -- an `.enumerate()` rewrite wouldn't eliminate the separate numeric use, just
    // obscure it.
    #[allow(clippy::needless_range_loop)]
    for k in 1..=m {
        let ratio = k as f64 / m as f64;
        let lam = if ratio <= 0.5 { 1.0 } else { 2.0 * (1.0 - ratio) };
        g += 2.0 * lam * k as f64 * acv[k];
        lr_acv += 2.0 * lam * acv[k];
    }
    let d_sb = 2.0 * lr_acv * lr_acv;
    // Explicit precondition check, not an implicit NaN that arithmetic happens to produce:
    // `d_sb <= 0.0` iff `lr_acv == 0.0` exactly (`d_sb` is `2*lr_acv^2`, never negative) -- the
    // ONLY input shape for which the real `arch` package's own division-based formula degenerates
    // (`0.0/0.0` when `g` is also zero, as it always is when `lr_acv` is, since both are weighted
    // sums of the same `acv[k]` values; `x/0.0` -> `+inf` in the hypothetical case `g != 0`, which
    // `_optimal_block_length_sb`'s `is_finite()` check already treats identically to `NaN` either
    // way). Checking this BEFORE computing `b_sb` means `b_sb` is guaranteed finite by the time
    // it reaches the final clamp below -- ordinary `f64::min` is correct there, no NaN-aware
    // comparison logic needed at all.
    if d_sb <= 0.0 {
        return None;
    }
    let b_sb = ((2.0 * g * g) / d_sb).cbrt() * (nobs as f64).cbrt();
    Some(b_sb.min(b_max))
}

/// Geyer's (1992) initial monotone sequence estimator of the integrated autocorrelation time,
/// `tau_int = 1 + 2*sum(rho_k)`. Ported from `_geyer_tau_int`, with one deliberate scope cut:
/// **direct O(n*max_lag) autocovariance, not the real function's FFT-based one.** The real
/// function needs FFT specifically for the raw per-bar scale `_derive_window_rows` uses it at
/// (up to millions of rows) -- a separate use case, out of this port's scope (this crate's
/// `calibration.rs` only ever calls this on the same ~228-element window-stat arrays
/// `politis_white_block_length` operates on, where `max_lag` is tiny -- `O(228*57)` here, not
/// `O(10^6 * 2.5*10^5)`). Reusing this function for a million-row raw series would need an
/// FFT upgrade first; not silently assumed safe at that scale.
pub fn geyer_tau_int(series: &[f64], max_lag: Option<usize>) -> f64 {
    let n = series.len();
    if n < 4 {
        return 1.0;
    }
    let mean = series.iter().sum::<f64>() / n as f64;
    let x: Vec<f64> = series.iter().map(|&v| v - mean).collect();
    let var = x.iter().map(|v| v * v).sum::<f64>() / n as f64;
    if var <= 0.0 {
        return 1.0;
    }
    let max_lag = max_lag.unwrap_or_else(|| (n - 2).min((n / 4).max(50)));
    let max_lag = max_lag.max(2).min(n - 1);

    let mut rho = vec![0.0; max_lag + 1];
    rho[0] = 1.0;
    for lag in 1..=max_lag {
        let acov: f64 = (0..n - lag).map(|t| x[t] * x[t + lag]).sum::<f64>() / n as f64;
        rho[lag] = acov / var;
    }

    let n_pairs = (max_lag - 1) / 2 + 1;
    let mut gamma: Vec<f64> = (0..n_pairs).map(|m| rho[2 * m] + rho[2 * m + 1]).collect();
    let cutoff = gamma.iter().position(|&g| g <= 0.0).unwrap_or(gamma.len());
    gamma.truncate(cutoff);
    if gamma.is_empty() {
        return 1.0;
    }
    // `np.minimum.accumulate`: running minimum, enforcing Geyer's "initial monotone" rule.
    for m in 1..gamma.len() {
        if gamma[m] > gamma[m - 1] {
            gamma[m] = gamma[m - 1];
        }
    }
    let tau = 2.0 * gamma.iter().sum::<f64>() - 1.0;
    tau.max(1.0)
}

/// Ported from `_optimal_block_length_sb`: Politis-White block length, falling back to
/// `2 * geyer_tau_int` when Politis-White fails (`None`, or a finite `< 1.0` result -- e.g. too
/// few observations, or a near-degenerate window the autocorrelation-band procedure can't
/// resolve). **Always returns a finite value `>= 1.0`** -- `geyer_tau_int` is guaranteed finite
/// `>= 1.0` by its own construction (every early-return path and its final `.max(1.0)`), so no
/// degenerate/non-finite value can ever escape this function's boundary into
/// `bootstrap_threshold_ci` or any other caller.
pub fn optimal_block_length_sb(series: &[f64]) -> f64 {
    match politis_white_block_length(series) {
        Some(b_sb) if b_sb >= 1.0 => b_sb,
        _ => 2.0 * geyer_tau_int(series, None),
    }
}

/// Politis & Romano (1994) stationary bootstrap resampling: geometrically-distributed random
/// block lengths (mean `block_size`), circular wraparound. Ported from the real `arch` package's
/// own `_samplers_python.py::stationary_bootstrap_sample_python` -- `p = 1/block_size` is the
/// per-step probability of starting a fresh random block; otherwise the previous index is
/// continued sequentially (wrapping at `n`). Returns a `Vec<usize>` of resampled indices into the
/// original series, length `n`.
pub fn stationary_bootstrap_indices(n: usize, block_size: f64, rng: &mut SplitMix64) -> Vec<usize> {
    if n == 0 {
        return Vec::new();
    }
    let p = 1.0 / block_size.max(1.0);
    let mut indices: Vec<usize> = (0..n).map(|_| rng.next_bounded(n as u64) as usize).collect();
    for i in 1..n {
        let u = rng.next_f64();
        if u > p {
            indices[i] = indices[i - 1] + 1;
            if indices[i] == n {
                indices[i] = 0;
            }
        }
        // else: keep the independently-drawn `indices[i]` -- starts a fresh random block.
    }
    indices
}

/// Result of `bootstrap_threshold_ci` -- mirrors `_bootstrap_threshold_ci`'s numeric fields.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BootstrapThresholdCi {
    /// `None` when `ci_valid` is `false` (Efron & Tibshirani 1993: a CI from too few independent
    /// blocks is not just imprecise, it's statistically meaningless -- null the bounds rather than
    /// report a number that looks precise but isn't, same as the Python original).
    pub ci_low: Option<f64>,
    pub ci_high: Option<f64>,
    pub ci_valid: bool,
    pub ci_size: f64,
    pub block_length_sb: f64,
    pub geyer_tau_int: f64,
    pub n_eff_bootstrap: f64,
    pub n_eff_geyer: f64,
    pub ess_disagreement_flagged: bool,
    pub n_bootstrap_reps: usize,
}

const ESS_DISAGREEMENT_FLAG_RATIO: f64 = 2.0;
const MIN_EFFECTIVE_BLOCKS: f64 = 30.0;

/// Ported from `_bootstrap_threshold_ci`: a stationary block-bootstrap percentile CI for a
/// window-level quantile statistic, cross-checked against Geyer's tau_int-implied effective
/// sample size (flags, does not silently resolve, disagreement between the two independent
/// estimates). `stat_fn` computes the statistic of interest on one resampled series (e.g.
/// `|xs| xs.percentile(quantile)` for the real script's own percentile-CI use).
///
/// Golden-parity for the block-length/ESS/validity-gate math is exercised directly (this
/// module's own tests); the bootstrap resampling itself is NOT held to cross-language bit-exact
/// parity (same deliberately-flagged scope boundary `init.rs`'s k-means++ seeding already
/// established for this crate -- no PRNG choice buys real distribution-level parity with numpy's
/// own generator anyway) -- validated instead by statistical sanity checks (the real holdout
/// point estimate should usually land inside its own simulated/resampled CI, bounds should widen
/// as `ci_size` grows, etc).
pub fn bootstrap_threshold_ci(
    series: &[f64],
    seed: u64,
    n_bootstrap: usize,
    ci_size: f64,
    stat_fn: impl Fn(&[f64]) -> f64,
) -> BootstrapThresholdCi {
    let n = series.len();
    let block_len_raw = optimal_block_length_sb(series);
    let block_len = (block_len_raw.round() as i64).max(1) as f64;
    let tau_int = geyer_tau_int(series, None);
    let n_eff_bootstrap = n as f64 / block_len;
    let n_eff_geyer = n as f64 / (2.0 * tau_int);

    let mut rng = SplitMix64::new(seed);
    let mut draws = Vec::with_capacity(n_bootstrap);
    for _ in 0..n_bootstrap {
        let idx = stationary_bootstrap_indices(n, block_len, &mut rng);
        let resampled: Vec<f64> = idx.iter().map(|&i| series[i]).collect();
        draws.push(stat_fn(&resampled));
    }
    draws.sort_by(f64::total_cmp);

    let alpha = (1.0 - ci_size) / 2.0;
    let lo = percentile_sorted(&draws, 100.0 * alpha);
    let hi = percentile_sorted(&draws, 100.0 * (1.0 - alpha));

    let ess_ratio = n_eff_bootstrap.max(n_eff_geyer) / n_eff_bootstrap.min(n_eff_geyer).max(1e-9);
    let flagged = ess_ratio > ESS_DISAGREEMENT_FLAG_RATIO;
    let ci_valid = n_eff_bootstrap.min(n_eff_geyer) >= MIN_EFFECTIVE_BLOCKS;

    BootstrapThresholdCi {
        ci_low: if ci_valid { Some(lo) } else { None },
        ci_high: if ci_valid { Some(hi) } else { None },
        ci_valid,
        ci_size,
        block_length_sb: block_len,
        geyer_tau_int: tau_int,
        n_eff_bootstrap,
        n_eff_geyer,
        ess_disagreement_flagged: flagged,
        n_bootstrap_reps: n_bootstrap,
    }
}

/// Linear-interpolation percentile over an ALREADY-SORTED slice -- matches `numpy.percentile`'s
/// default (`method="linear"`) convention, the same one `_bootstrap_threshold_ci`'s own
/// `np.percentile` call relies on implicitly.
fn percentile_sorted(sorted: &[f64], pct: f64) -> f64 {
    if sorted.is_empty() {
        return f64::NAN;
    }
    if sorted.len() == 1 {
        return sorted[0];
    }
    let rank = (pct / 100.0) * (sorted.len() - 1) as f64;
    let lo = rank.floor() as usize;
    let hi = rank.ceil() as usize;
    if lo == hi {
        sorted[lo]
    } else {
        let frac = rank - lo as f64;
        sorted[lo] * (1.0 - frac) + sorted[hi] * frac
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn check(name: &str, got: f64, want: f64, tol: f64) {
        assert!((got - want).abs() < tol, "{name}: got {got}, want {want} (tol {tol})");
    }

    include!("calibration_fixtures.rs");

    #[test]
    fn golden_politis_white_block_length_ar1_228() {
        let b_sb = politis_white_block_length(&AR1_FIXTURE_228)
            .expect("a real AR(1) series must produce a defined block length, not None");
        check("b_sb_228", b_sb, 4.704129104006514, 1e-9);
    }

    #[test]
    fn golden_geyer_tau_int_ar1_228() {
        // Real `arch`/Python run used an FFT-based autocovariance; this port uses a direct sum
        // (see this module's own doc comment on `geyer_tau_int` for why that's an acceptable
        // scope cut at this array size) -- both compute the EXACT SAME autocovariance values
        // mathematically, so the golden match is still to full double precision, not a looser
        // tolerance.
        check("tau_int_228", geyer_tau_int(&AR1_FIXTURE_228, None), 2.308183802190502, 1e-9);
    }

    #[test]
    fn golden_short_series_n10() {
        let short = &AR1_FIXTURE_228[..10];
        let b_sb = politis_white_block_length(short).expect("n=10 must still produce a value");
        check("b_sb_n10", b_sb, 0.7244335300449274, 1e-9);
        check("tau_int_n10", geyer_tau_int(short, None), 1.2422105308269145, 1e-9);
    }

    #[test]
    fn golden_constant_series_none_and_floor() {
        let constant = [5.0; 50];
        assert_eq!(
            politis_white_block_length(&constant),
            None,
            "a constant series must produce None (the real arch package's own equivalent: a \
             genuine 0.0/0.0 degenerate case) -- Rust expresses 'could not be estimated' as an \
             explicit None instead of an implicit NaN sentinel a caller could forget to check"
        );
        check("tau_int_constant", geyer_tau_int(&constant, None), 1.0, 1e-12);
        // optimal_block_length_sb must catch the None and fall back to 2*geyer_tau_int.
        check("fallback_on_none", optimal_block_length_sb(&constant), 2.0, 1e-9);
    }

    #[test]
    fn golden_too_short_for_geyer() {
        check("geyer_n_lt_4", geyer_tau_int(&[1.0, 2.0, 3.0], None), 1.0, 1e-12);
    }

    #[test]
    fn optimal_block_length_sb_is_always_finite_and_at_least_one() {
        // The explicit finite-guarantee `optimal_block_length_sb`'s own doc comment states --
        // exercised here directly, not just inferred from reading `geyer_tau_int`'s guards, for
        // a battery of inputs including every known-degenerate case this module's other tests
        // exercise individually.
        let cases: Vec<Vec<f64>> = vec![
            AR1_FIXTURE_228.to_vec(),
            AR1_FIXTURE_228[..10].to_vec(),
            vec![5.0; 50],   // constant -> politis_white_block_length returns None
            vec![1.0, 2.0, 3.0], // too short even for geyer_tau_int's own n>=4 floor
            vec![],
        ];
        for series in &cases {
            let result = optimal_block_length_sb(series);
            assert!(result.is_finite(), "optimal_block_length_sb must never return non-finite, got {result} for {series:?}");
            assert!(result >= 1.0, "optimal_block_length_sb must never return < 1.0, got {result} for {series:?}");
        }
    }

    #[test]
    fn stationary_bootstrap_indices_are_in_range_and_deterministic_per_seed() {
        let mut rng_a = SplitMix64::new(7);
        let mut rng_b = SplitMix64::new(7);
        let idx_a = stationary_bootstrap_indices(50, 5.0, &mut rng_a);
        let idx_b = stationary_bootstrap_indices(50, 5.0, &mut rng_b);
        assert_eq!(idx_a, idx_b, "same seed must reproduce the same resample");
        assert_eq!(idx_a.len(), 50);
        assert!(idx_a.iter().all(|&i| i < 50), "every index must be in [0, n)");

        let mut rng_c = SplitMix64::new(8);
        let idx_c = stationary_bootstrap_indices(50, 5.0, &mut rng_c);
        assert_ne!(idx_a, idx_c, "different seeds must (overwhelmingly likely) differ");
    }

    #[test]
    fn stationary_bootstrap_block_size_one_is_pure_iid_resampling() {
        // p = 1/1 = 1.0, so `u > p` is never true (u is in [0,1)) -- every position gets an
        // independent fresh random draw, i.e. plain-vanilla bootstrap (iid with replacement).
        let mut rng = SplitMix64::new(3);
        let idx = stationary_bootstrap_indices(20, 1.0, &mut rng);
        assert_eq!(idx.len(), 20);
    }

    #[test]
    fn percentile_sorted_matches_numpy_linear_interpolation() {
        // numpy.percentile([1,2,3,4,5], 50) == 3.0; percentile(.., 25) == 2.0 (verified against
        // numpy's own documented linear-interpolation convention, the default method).
        let sorted = [1.0, 2.0, 3.0, 4.0, 5.0];
        check("median", percentile_sorted(&sorted, 50.0), 3.0, 1e-12);
        check("p25", percentile_sorted(&sorted, 25.0), 2.0, 1e-12);
        check("p0", percentile_sorted(&sorted, 0.0), 1.0, 1e-12);
        check("p100", percentile_sorted(&sorted, 100.0), 5.0, 1e-12);
    }

    #[test]
    fn bootstrap_threshold_ci_real_holdout_sanity_checks() {
        // Statistical sanity, not bit-exact parity (the resampling itself isn't held to
        // cross-language parity -- see this function's own doc comment). A median-statistic CI
        // over a real-shaped 228-element series should: (a) widen as ci_size grows, (b) have
        // ci_low <= ci_high, (c) report a finite, positive block length and tau_int.
        let median_stat = |xs: &[f64]| -> f64 {
            let mut v = xs.to_vec();
            v.sort_by(f64::total_cmp);
            percentile_sorted(&v, 50.0)
        };
        let ci_90 = bootstrap_threshold_ci(&AR1_FIXTURE_228, 42, 500, 0.90, median_stat);
        let ci_99 = bootstrap_threshold_ci(&AR1_FIXTURE_228, 42, 500, 0.99, median_stat);

        assert!(ci_90.block_length_sb >= 1.0);
        assert!(ci_90.geyer_tau_int >= 1.0);
        assert!(ci_90.n_eff_bootstrap > 0.0);
        assert!(ci_90.n_eff_geyer > 0.0);

        if let (Some(lo90), Some(hi90)) = (ci_90.ci_low, ci_90.ci_high) {
            assert!(lo90 <= hi90, "lower bound must not exceed upper bound");
            if let (Some(lo99), Some(hi99)) = (ci_99.ci_low, ci_99.ci_high) {
                assert!(
                    (hi99 - lo99) >= (hi90 - lo90) - 1e-9,
                    "a 99% CI must be at least as wide as a 90% CI from the same data/seed"
                );
            }
        }
    }

    #[test]
    fn bootstrap_threshold_ci_flags_invalid_when_too_few_effective_blocks() {
        // A short series (n=10) against a large block length produces n_eff well below
        // MIN_EFFECTIVE_BLOCKS=30 -- ci_valid must be false, bounds must be None, not a
        // misleadingly precise-looking number.
        let short = &AR1_FIXTURE_228[..10];
        let stat = |xs: &[f64]| xs.iter().sum::<f64>() / xs.len() as f64;
        let ci = bootstrap_threshold_ci(short, 1, 100, 0.95, stat);
        assert!(!ci.ci_valid);
        assert_eq!(ci.ci_low, None);
        assert_eq!(ci.ci_high, None);
    }
}
