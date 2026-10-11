//! model.rs -- the Student-t HMM's pure-math kernels: emission density, the numerically-stable
//! Hamilton forward filter, and Viterbi decode. No training, no artifact I/O, no FFI -- the
//! "golden-independent slice" (plan §4 Stage G1/G2) that doesn't need Stage A/P9's golden harness
//! to be hand/synthetic-tested, per the already-decided "pure math is hand/synthetic-testable now;
//! golden-parity validation deferred to whenever P9 lands" note.
//!
//! Ported from `lbrnet/lbrnet/models/student_t_hmm.py`'s `_log_emission_and_delta()` -- NOT a copy
//! of Atratus's simpler unweighted `sensor_core/src/hmm.rs` (checked side by side, 2026-10-11):
//! MindfulTrader's own emission density has three things Atratus's doesn't need at all --
//! per-feature weights on the Mahalanobis delta, the Jacobian correction term that weighting
//! requires, and the "true dimension `D`, not the weighted sum" fix from this repo's own
//! documented "L_MAP decline" bug chain (density-normalization bug, item 2). Porting Atratus's
//! formula verbatim would silently reproduce that exact bug.
//!
//! `D`/`K` are const generics, not `ndarray`'s runtime-sized arrays -- deliberate, 2026-10-11: the
//! dimension count is still being decided (Elite Feature Set Curation's Phase 2 saliency finding),
//! but every change in this codebase already goes through a full rebuild cycle regardless, so a
//! const change costs nothing extra; it does not justify a heap-allocating dependency this
//! codebase's own `observation_vector` crate deliberately avoids. Const generics also let tests
//! instantiate small `D`/`K` configurations (matching Atratus's own varying-dimension test style)
//! while production code commits to one concrete type alias.

use std::f64::consts::PI;

/// Variance floor -- matches `student_t_hmm.py`'s own `min_covar` default (1e-12), itself the fix
/// for the documented `min_covar`/`global_var`-anchor miscalibration (L_MAP bug chain item 6:
/// a flat 0.1 floor bound continuously on real low-variance dims). Unlike `dof_min`/`dof_max`
/// below, `min_covar` is NOT CLI-configurable anywhere in `train_student_t_hmm.py` (no
/// `--min-covar` flag exists) -- confirmed 2026-10-11 while tracing where `gate_metrics.rs`'s own
/// `dof_min`/`dof_max` parameters actually come from. Genuinely a fixed constant, correctly a
/// module `const`.
pub const MIN_COVAR: f64 = 1e-12;
/// Default degrees-of-freedom clamp bounds -- match `student_t_hmm.py`'s own constructor
/// defaults. **Not** module `const`s used directly in the math (corrected 2026-10-11): the real
/// system's `--dof-min`/`--dof-max` CLI flags feed `trainer.config.dof_min`/`dof_max`, which
/// become `model.dof_min`/`model.dof_max` -- genuinely per-training-run configuration every real
/// `gate_metrics.rs` call site reads off the model object, not a hardcoded default. `StudentTHmm`
/// carries `dof_min`/`dof_max` as its own fields (below); these two constants exist only so a
/// caller constructing a model with the ordinary bounds doesn't need to spell out the magic
/// numbers `2.1`/`100.0` themselves.
pub const DEFAULT_DOF_MIN: f64 = 2.1;
pub const DEFAULT_DOF_MAX: f64 = 100.0;
/// Sentinel log-probability returned when a numerical result is non-finite, instead of
/// propagating NaN/Inf into a posterior. Matches Atratus's own `LOG_PROB_PENALTY` convention
/// (same defensive idea, independently useful, not formula-specific -- safe to reuse the pattern).
const LOG_PROB_PENALTY: f64 = -1e10;

/// Production dimension count (current 18D observation vector) and state count (K=4,
/// production-locked per the runbook's own Smart Tuner V1 frozen contract). A single point of
/// change if either moves -- see this crate's own Cargo.toml header for why that's an acceptable,
/// already-routine cost, not a reason to avoid const generics.
pub const PRODUCTION_D: usize = 18;
pub const PRODUCTION_K: usize = 4;

/// Diagonal-covariance, per-feature-weighted Student-t Hidden Markov Model.
///
/// `D` = observation-vector dimension, `K` = number of regime states, both compile-time.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct StudentTHmm<const D: usize, const K: usize> {
    /// Per-state means, shape (K, D).
    pub means: [[f64; D]; K],
    /// Per-state diagonal covariances, shape (K, D). Floored at `MIN_COVAR` on read, not on
    /// construction, so a freshly-deserialized artifact never needs a repair pass.
    pub covars: [[f64; D]; K],
    /// Row-stochastic state-transition matrix, shape (K, K).
    pub transmat: [[f64; K]; K],
    /// Initial-state distribution, shape (K,).
    pub startprob: [f64; K],
    /// Per-state Student-t degrees of freedom (nu), shape (K,).
    pub dof: [f64; K],
    /// Degrees-of-freedom clamp bounds -- per-model, not a module constant (see `DEFAULT_DOF_MIN`/
    /// `DEFAULT_DOF_MAX`'s own doc comment for why: the real system's `--dof-min`/`--dof-max` CLI
    /// flags make these genuinely vary per training run).
    pub dof_min: f64,
    pub dof_max: f64,
    /// Per-feature weights applied to the Mahalanobis delta ("bullet dims" weighting). All-1.0
    /// is the unweighted case; `log_emission_prob`'s Jacobian-correction term is a no-op then.
    pub feature_weights: [f64; D],
}

impl<const D: usize, const K: usize> StudentTHmm<D, K> {
    #[allow(clippy::too_many_arguments)] // one named field per real model parameter; a builder
    // would hide which fields are actually required, not simplify anything for this struct's
    // genuinely-flat parameter set.
    pub fn new(
        means: [[f64; D]; K],
        covars: [[f64; D]; K],
        transmat: [[f64; K]; K],
        startprob: [f64; K],
        dof: [f64; K],
        dof_min: f64,
        dof_max: f64,
        feature_weights: [f64; D],
    ) -> Self {
        Self { means, covars, transmat, startprob, dof, dof_min, dof_max, feature_weights }
    }

    /// Unweighted convenience constructor (`feature_weights` all 1.0). `dof_min`/`dof_max` are
    /// still required explicitly, not defaulted -- see their own field doc comment for why
    /// silently defaulting them would be the exact inconsistency this constructor was fixed to
    /// avoid.
    #[allow(clippy::too_many_arguments)]
    pub fn new_unweighted(
        means: [[f64; D]; K],
        covars: [[f64; D]; K],
        transmat: [[f64; K]; K],
        startprob: [f64; K],
        dof: [f64; K],
        dof_min: f64,
        dof_max: f64,
    ) -> Self {
        Self::new(means, covars, transmat, startprob, dof, dof_min, dof_max, [1.0; D])
    }

    /// Weighted squared Mahalanobis distance: `delta = sum(w_j * (x_j - mu_j)^2 / var_j)`.
    /// Shared by `log_emission_prob` and `compute_tail_weight` so the two can never disagree on
    /// what "delta" means -- a real risk the Python original's duplicated inline computation
    /// didn't have to guard against (one function, `_log_emission_and_delta`, computes both).
    pub(crate) fn weighted_delta(&self, k: usize, x: &[f64; D]) -> f64 {
        let mean = &self.means[k];
        let var = &self.covars[k];
        let mut delta = 0.0;
        for j in 0..D {
            let v = var[j].max(MIN_COVAR);
            let diff = x[j] - mean[j];
            delta += self.feature_weights[j] * (diff * diff) / v;
        }
        delta
    }

    /// Log-density of the diagonal-covariance, per-feature-weighted multivariate Student-t:
    ///
    /// ln p(x|mu,sigma^2,nu,w) = lnGamma((nu+D)/2) - lnGamma(nu/2) - (D/2)ln(nu*pi) - 0.5*log_det
    ///                           + 0.5*sum(ln w_j) - ((nu+D)/2)*ln1p(delta/nu)
    ///
    /// `D` here is the *true* dimension count, never `sum(w_j)` -- the fix for the density-
    /// normalization bug (L_MAP chain item 2): a linear rescaling `y_d = sqrt(w_d)*(x_d-mu_d)/sigma_d`
    /// only ever changes the Jacobian/determinant term (`0.5*sum(ln w_j)` below), never the ambient
    /// dimension being integrated over. Verified against `student_t_hmm.py`'s own documented
    /// numerical check (D=2, w=[1.4, 1.0]) in this module's tests.
    pub fn log_emission_prob(&self, k: usize, x: &[f64; D]) -> f64 {
        let nu = self.dof[k].clamp(self.dof_min, self.dof_max);
        let d = D as f64;
        let var = &self.covars[k];

        let delta = self.weighted_delta(k, x);
        let log_det: f64 = var.iter().map(|&v| v.max(MIN_COVAR).ln()).sum();
        let log_weight_sum: f64 = self.feature_weights.iter().map(|&w| w.max(1e-300).ln()).sum();

        let term_coeff = ln_gamma((nu + d) * 0.5) - ln_gamma(nu * 0.5)
            - 0.5 * (d * (nu * PI).ln() + log_det)
            + 0.5 * log_weight_sum;
        // log1p(delta/nu), not ln(1.0 + delta/nu) -- matches student_t_hmm.py's own
        // np.log1p(delta/nu) for numerical precision when delta/nu is small (Atratus's hmm.rs
        // uses the less precise plain-ln form; not copied here).
        let result = term_coeff - 0.5 * (nu + d) * (delta / nu).ln_1p();

        if result.is_nan() || result.is_infinite() {
            LOG_PROB_PENALTY
        } else {
            result
        }
    }

    /// Student-t tail weight: `w = (nu + D) / (nu + delta)`. Low values mean `x` is a tail
    /// observation relative to state `k`; matches `compute_tail_diagnostics`'s own formula.
    pub fn compute_tail_weight(&self, k: usize, x: &[f64; D]) -> f64 {
        let nu = self.dof[k].clamp(self.dof_min, self.dof_max);
        let delta = self.weighted_delta(k, x);
        (nu + D as f64) / (nu + delta)
    }

    /// Numerically-stable emission update: `posterior[k] ∝ prior[k] * exp(log_b[k] - max_log_b)`.
    /// The log-sum-exp trick (subtract the max log-emission before exponentiating) guarantees at
    /// least one `exp(...) == 1.0`, so an extreme-magnitude anomaly tick can't underflow every
    /// state to 0 and collapse the filter -- same pattern Atratus's own `stable_posterior` uses,
    /// rewritten natively here (fixed-size arrays, not `ndarray`), stress-tested the same way
    /// (`test_predict_stable_under_extreme_anomaly`, this module's own tests).
    // `needless_range_loop` fires on these index loops, but the suggested `.enumerate()` rewrite
    // doesn't fit: each iteration either calls `log_emission_prob(k, ...)` (needs the raw index,
    // not just an array element) or walks two/three differently-shaped arrays (`prior`, `log_b`,
    // `out`) in lockstep -- a loop index is clearer here than a nested `.zip()` chain.
    #[allow(clippy::needless_range_loop)]
    fn stable_posterior(&self, prior: &[f64; K], x: &[f64; D]) -> [f64; K] {
        let mut log_b = [0.0; K];
        let mut max_log_b = f64::NEG_INFINITY;
        for k in 0..K {
            let lb = self.log_emission_prob(k, x);
            log_b[k] = lb;
            if lb > max_log_b {
                max_log_b = lb;
            }
        }
        if !max_log_b.is_finite() {
            max_log_b = 0.0;
        }

        let mut out = [0.0; K];
        let mut sum = 0.0;
        for k in 0..K {
            let w = prior[k] * (log_b[k] - max_log_b).exp();
            out[k] = w;
            sum += w;
        }
        if sum > 0.0 && sum.is_finite() {
            for k in 0..K {
                out[k] /= sum;
            }
        } else {
            // Extremely unlikely given the max-subtraction; fall back to the (assumed
            // normalized) prior rather than ever emitting a NaN-laced posterior.
            out = *prior;
        }
        out
    }

    /// One recursive step of the Hamilton filter: predict via the transition matrix, then update
    /// with the current observation's (numerically stable) emission.
    // `transmat[i][j]` is a genuine 2D matrix access jointly indexed by both loop variables --
    // not a case `.enumerate()` over one array naturally covers.
    #[allow(clippy::needless_range_loop)]
    pub fn forward_step(&self, alpha_prev: &[f64; K], x: &[f64; D]) -> [f64; K] {
        let mut alpha_pred = [0.0; K];
        for j in 0..K {
            let mut s = 0.0;
            for i in 0..K {
                s += alpha_prev[i] * self.transmat[i][j];
            }
            alpha_pred[j] = s;
        }
        let pred_sum: f64 = alpha_pred.iter().sum();
        if pred_sum > 0.0 && pred_sum.is_finite() {
            for v in &mut alpha_pred {
                *v /= pred_sum;
            }
        } else {
            alpha_pred = [1.0 / K as f64; K];
        }
        self.stable_posterior(&alpha_pred, x)
    }

    /// Sequential state-posterior prediction over an observation sequence (a slice of fixed-size
    /// observation vectors, chronologically ordered). Returns the final posterior only, matching
    /// `student_t_hmm.py`'s own `predict()` contract -- the live caller advances one tick at a
    /// time via `forward_step` directly; this is the bulk/test-time convenience path.
    pub fn predict(&self, observations: &[[f64; D]]) -> [f64; K] {
        if observations.is_empty() {
            return self.startprob;
        }

        let mut alpha = self.startprob;
        let s0: f64 = alpha.iter().sum();
        if s0 > 0.0 && s0.is_finite() {
            for v in &mut alpha {
                *v /= s0;
            }
        } else {
            alpha = [1.0 / K as f64; K];
        }

        // First observation: emission update only, no transition (matches the Python original).
        alpha = self.stable_posterior(&alpha, &observations[0]);
        for obs in &observations[1..] {
            alpha = self.forward_step(&alpha, obs);
        }
        alpha
    }

    /// Viterbi decode: the single most likely state sequence for an observation sequence.
    // Same reasoning as `forward_step`/`stable_posterior`: `v[t][k]`/`ptr[t][k]` are 2D, and
    // `transmat[i][j]` is jointly indexed by two loop variables -- a loop index, not `.enumerate()`.
    #[allow(clippy::needless_range_loop)]
    pub fn decode(&self, observations: &[[f64; D]]) -> Vec<usize> {
        let n = observations.len();
        if n == 0 {
            return Vec::new();
        }

        let mut v = vec![[f64::NEG_INFINITY; K]; n];
        let mut ptr = vec![[0usize; K]; n];

        for k in 0..K {
            v[0][k] = self.startprob[k].ln() + self.log_emission_prob(k, &observations[0]);
        }

        for t in 1..n {
            for j in 0..K {
                let mut best_val = f64::NEG_INFINITY;
                let mut best_prev = 0usize;
                for i in 0..K {
                    let val = v[t - 1][i] + self.transmat[i][j].ln();
                    if val > best_val {
                        best_val = val;
                        best_prev = i;
                    }
                }
                v[t][j] = best_val + self.log_emission_prob(j, &observations[t]);
                ptr[t][j] = best_prev;
            }
        }

        let mut best_last = 0usize;
        let mut best_final = f64::NEG_INFINITY;
        for k in 0..K {
            if v[n - 1][k] > best_final {
                best_final = v[n - 1][k];
                best_last = k;
            }
        }

        let mut path = vec![0usize; n];
        path[n - 1] = best_last;
        for t in (1..n).rev() {
            path[t - 1] = ptr[t][path[t]];
        }
        path
    }
}

/// Log of the gamma function via the Lanczos approximation (g=7, 9-term series) -- hand-rolled,
/// not a crate dependency (`f64::ln_gamma` is nightly-only `float_gamma`; adding a crate like
/// `special` purely for this one function would be a dependency this codebase's own
/// `observation_vector` crate deliberately avoids). Accurate to ~15 significant digits across the
/// domain this HMM actually evaluates it on (`nu/2`, `(nu+D)/2` for `nu` in the model's own
/// `[dof_min, dof_max]` clamp range, `D` up to a few dozen -- always comfortably positive, the
/// reflection branch below exists for completeness/correctness, not because this HMM is expected
/// to hit it).
fn ln_gamma(x: f64) -> f64 {
    const G: f64 = 7.0;
    const COEF: [f64; 9] = [
        0.999_999_999_999_809_9,
        676.520_368_121_885_1,
        -1_259.139_216_722_402_8,
        771.323_428_777_653,
        -176.615_029_162_140_6,
        12.507_343_278_686_905,
        -0.138_571_095_265_720_12,
        9.984_369_578_019_572e-6,
        1.505_632_735_149_312e-7,
    ];

    if x < 0.5 {
        // Reflection formula: Gamma(x)*Gamma(1-x) = pi/sin(pi*x).
        (PI / (PI * x).sin()).ln() - ln_gamma(1.0 - x)
    } else {
        let x = x - 1.0;
        let mut a = COEF[0];
        for (i, &c) in COEF.iter().enumerate().skip(1) {
            a += c / (x + i as f64);
        }
        let t = x + G + 0.5;
        0.5 * (2.0 * PI).ln() + (x + 0.5) * t.ln() - t + a.ln()
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn assert_close(a: f64, b: f64, tol: f64) {
        assert!((a - b).abs() < tol, "expected {b}, got {a} (tol {tol})");
    }

    // --- ln_gamma correctness, against exact known values ---

    #[test]
    fn ln_gamma_known_values() {
        assert_close(ln_gamma(1.0), 0.0, 1e-12); // Gamma(1) = 1
        assert_close(ln_gamma(2.0), 0.0, 1e-12); // Gamma(2) = 1
        assert_close(ln_gamma(5.0), 24.0_f64.ln(), 1e-10); // Gamma(5) = 4! = 24
        assert_close(ln_gamma(0.5), 0.5 * PI.ln(), 1e-10); // Gamma(0.5) = sqrt(pi)
        assert_close(ln_gamma(10.0), 362_880.0_f64.ln(), 1e-8); // Gamma(10) = 9!
    }

    // --- emission density: must integrate to 1 (L_MAP bug chain item 2 regression) ---

    /// 1D unweighted Student-t: numerically integrate `exp(log_emission_prob)` over a wide range
    /// via Simpson's rule and assert it equals 1.0. This is the base case the weighted test below
    /// builds on.
    #[test]
    fn emission_density_integrates_to_one_1d_unweighted() {
        let hmm = StudentTHmm::<1, 1>::new_unweighted(
            [[0.0]],
            [[1.0]],
            [[1.0]],
            [1.0],
            [8.0],
            DEFAULT_DOF_MIN,
            DEFAULT_DOF_MAX,
        );
        let integral = simpson_integrate(-50.0, 50.0, 200_000, |x| {
            hmm.log_emission_prob(0, &[x]).exp()
        });
        assert_close(integral, 1.0, 1e-4);
    }

    /// 2D *weighted* Student-t, D=2, w=[1.4, 1.0] -- the exact numerical case
    /// `student_t_hmm.py`'s own source comment documents as the verification that using the true
    /// dimension `D` (not `sum(w_j)`) in every gamma-function argument and normalizing power is
    /// what makes the weighted density integrate to 1. A pre-fix version using `sum(w_j)` instead
    /// of `D` would integrate to a different constant here, not 1.0 -- this test would have caught
    /// that exact historical bug.
    #[test]
    fn emission_density_integrates_to_one_2d_weighted() {
        let hmm = StudentTHmm::<2, 1>::new(
            [[0.0, 0.0]],
            [[1.0, 1.0]],
            [[1.0]],
            [1.0],
            [8.0],
            DEFAULT_DOF_MIN,
            DEFAULT_DOF_MAX,
            [1.4, 1.0],
        );
        // Double integral via nested Simpson's rule over a wide-enough box; coarser grid than the
        // 1D case to keep this fast (O(n^2) evaluations).
        let integral = simpson_integrate_2d(-30.0, 30.0, -30.0, 30.0, 600, |x0, x1| {
            hmm.log_emission_prob(0, &[x0, x1]).exp()
        });
        assert_close(integral, 1.0, 2e-3);
    }

    // --- numerical stability under an extreme anomaly (Atratus-inspired stress test, rewritten
    // natively against MindfulTrader's own weighted formula, not copied) ---

    #[test]
    fn predict_stable_under_extreme_anomaly() {
        // Two well-separated states; feed an observation far from both, at a magnitude that would
        // underflow every emission to 0 in a naive (non-log-space) linear filter.
        let hmm = StudentTHmm::<3, 2>::new_unweighted(
            [[-5.0, -5.0, -5.0], [5.0, 5.0, 5.0]],
            [[1.0, 1.0, 1.0], [1.0, 1.0, 1.0]],
            [[0.9, 0.1], [0.1, 0.9]],
            [0.5, 0.5],
            [4.0, 4.0],
            DEFAULT_DOF_MIN,
            DEFAULT_DOF_MAX,
        );
        let observations = [[1.0e9, -1.0e9, 1.0e9]];
        let posterior = hmm.predict(&observations);

        assert!(posterior.iter().all(|p| p.is_finite()), "posterior must stay finite");
        let sum: f64 = posterior.iter().sum();
        assert_close(sum, 1.0, 1e-9);
        assert!(posterior.iter().all(|&p| (0.0..=1.0).contains(&p)));
    }

    #[test]
    fn forward_step_normalization() {
        let hmm = StudentTHmm::<1, 2>::new_unweighted(
            [[0.0], [0.0]],
            [[1.0], [1.0]],
            [[0.9, 0.1], [0.1, 0.9]],
            [0.5, 0.5],
            [300.0, 300.0],
            DEFAULT_DOF_MIN,
            DEFAULT_DOF_MAX,
        );
        let alpha_prev = [0.5, 0.5];
        let alpha_curr = hmm.forward_step(&alpha_prev, &[0.0]);
        assert_close(alpha_curr.iter().sum(), 1.0, 1e-10);
    }

    #[test]
    fn predict_state_persistence() {
        // High self-transition probability + observations near state 1's mean should land the
        // posterior decisively on state 1.
        let hmm = StudentTHmm::<1, 2>::new_unweighted(
            [[-5.0], [5.0]],
            [[1.0], [1.0]],
            [[0.99, 0.01], [0.01, 0.99]],
            [0.5, 0.5],
            [300.0, 300.0],
            DEFAULT_DOF_MIN,
            DEFAULT_DOF_MAX,
        );
        let observations = [[4.9], [5.1], [5.0]];
        let posterior = hmm.predict(&observations);
        assert!(posterior[1] > 0.9, "expected state 1 dominant, got {posterior:?}");
    }

    #[test]
    fn decode_recovers_obvious_regime_switch() {
        // A sharp, unambiguous mean shift halfway through the sequence; Viterbi should recover it.
        let hmm = StudentTHmm::<1, 2>::new_unweighted(
            [[-5.0], [5.0]],
            [[0.25], [0.25]],
            [[0.95, 0.05], [0.05, 0.95]],
            [0.5, 0.5],
            [300.0, 300.0],
            DEFAULT_DOF_MIN,
            DEFAULT_DOF_MAX,
        );
        let observations = [[-5.1], [-4.9], [-5.0], [5.0], [5.1], [4.9]];
        let path = hmm.decode(&observations);
        assert_eq!(path, vec![0, 0, 0, 1, 1, 1]);
    }

    #[test]
    fn tail_weight_outlier_is_low() {
        let hmm = StudentTHmm::<2, 1>::new_unweighted(
            [[0.0, 0.0]],
            [[1.0, 1.0]],
            [[1.0]],
            [1.0],
            [4.0],
            DEFAULT_DOF_MIN,
            DEFAULT_DOF_MAX,
        );
        let w_center = hmm.compute_tail_weight(0, &[0.0, 0.0]);
        assert_close(w_center, 1.5, 1e-9); // (nu+D)/(nu+0) = (4+2)/4 = 1.5

        let w_outlier = hmm.compute_tail_weight(0, &[10.0, 10.0]);
        assert!(w_outlier < 0.1, "expected a low tail weight for a gross outlier, got {w_outlier}");
    }

    /// Two models differing *only* in their `dof_min`/`dof_max` clamp bounds -- not module
    /// constants, per-instance fields -- must clamp the same raw `dof` differently. This is the
    /// regression test for the fix itself: before it, `DOF_MIN`/`DOF_MAX` were crate-wide
    /// constants and no two `StudentTHmm` instances could ever disagree on clamp bounds, which is
    /// wrong given the real system's `--dof-min`/`--dof-max` CLI flags make this genuinely
    /// per-training-run configuration (confirmed by tracing every real `gate_metrics.rs` call
    /// site, which reads `dof_min`/`dof_max` off the model object, not a hardcoded default).
    #[test]
    fn dof_clamp_bounds_are_per_model_not_global() {
        let raw_dof = 150.0; // above both configurations' dof_min, above only one's dof_max
        let narrow = StudentTHmm::<1, 1>::new_unweighted(
            [[0.0]], [[1.0]], [[1.0]], [1.0], [raw_dof], 2.1, 50.0,
        );
        let wide = StudentTHmm::<1, 1>::new_unweighted(
            [[0.0]], [[1.0]], [[1.0]], [1.0], [raw_dof], 2.1, 200.0,
        );
        // Same raw `dof`, different `dof_max` -- the clamped value used by the emission density
        // must differ between the two models. Probe it indirectly via log_emission_prob, since
        // the clamped `nu` isn't itself exposed.
        let lp_narrow = narrow.log_emission_prob(0, &[0.0]);
        let lp_wide = wide.log_emission_prob(0, &[0.0]);
        assert!(
            (lp_narrow - lp_wide).abs() > 1e-9,
            "models with different dof_max clamp bounds must disagree on log_emission_prob when \
             raw dof exceeds the narrower bound, got narrow={lp_narrow} wide={lp_wide}"
        );
    }

    /// 1D composite Simpson's rule over `[a, b]` with `n` (must be even) intervals.
    fn simpson_integrate(a: f64, b: f64, n: usize, f: impl Fn(f64) -> f64) -> f64 {
        assert!(n.is_multiple_of(2));
        let h = (b - a) / n as f64;
        let mut sum = f(a) + f(b);
        for i in 1..n {
            let x = a + i as f64 * h;
            sum += if i.is_multiple_of(2) { 2.0 * f(x) } else { 4.0 * f(x) };
        }
        sum * h / 3.0
    }

    /// 2D composite Simpson's rule (tensor product of two 1D rules) over `[a0,b0] x [a1,b1]`.
    fn simpson_integrate_2d(
        a0: f64,
        b0: f64,
        a1: f64,
        b1: f64,
        n: usize,
        f: impl Fn(f64, f64) -> f64,
    ) -> f64 {
        assert!(n.is_multiple_of(2));
        let h1 = (b1 - a1) / n as f64;
        let mut outer_sum = 0.0;
        let weight = |i: usize| -> f64 {
            if i == 0 || i == n {
                1.0
            } else if i.is_multiple_of(2) {
                2.0
            } else {
                4.0
            }
        };
        for i in 0..=n {
            let x1 = a1 + i as f64 * h1;
            let inner = simpson_integrate(a0, b0, n, |x0| f(x0, x1));
            outer_sum += weight(i) * inner;
        }
        outer_sum * h1 / 3.0
    }
}
