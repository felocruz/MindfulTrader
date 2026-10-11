//! train.rs -- the EM/M-step training core: sequence-level forward-backward, the weighted
//! M-step (startprob/transmat/means/covars), and the ECME degrees-of-freedom update (Brent's
//! method, not Atratus's weaker unbracketed Newton's-method fallback -- see this module's own
//! `brent_root_find` doc comment).
//!
//! Ported from `lbrnet/lbrnet/models/student_t_hmm.py`'s `fit_streaming()` M-step (lines ~1211-
//! 1502) -- the single most bug-prone region in the whole Python HMM per this repo's own
//! documented "L_MAP decline" 8-bug forensic history (spec F3). Every fix in that history is
//! reproduced here deliberately, not rediscovered: the mean-weighted/covariance-UNweighted
//! denominator split (item in that history; independently cross-validated against Atratus's own
//! `rust/src/trainer.rs` M-step, which uses the exact same split despite being a wholly separate
//! implementation -- `combined_weight_sum` for the mean, unweighted `resp_sum` for the variance),
//! the true-`D`-not-weighted-sum DOF profile dimension (matches `model.rs`'s own emission-density
//! fix), and the ECME-staleness-correct two-pass DOF update (Liu & Rubin 1995: a conditional
//! maximizer must condition on the LATEST values of parameters already updated earlier in the same
//! M-step, so DOF's Mahalanobis deltas must be recomputed under the just-updated mean/covariance,
//! not left at their pre-M-step, E-step-time values -- confirmed NOT present in Atratus's own
//! trainer.rs, which only pays this cost if explicitly ported from here, not inherited "for free").
//!
//! Golden-parity tests at the bottom of this file reproduce two real invocations of
//! `student_t_hmm.py`'s `StudentTHMM.fit_streaming()` (mts conda env, 2026-10-11) for a tiny,
//! fully-deterministic 8-row/D=2/K=2 synthetic dataset -- one with every MAP prior disabled, one
//! with every prior (`mean_prior_kappa`/`covar_prior_eta`/`trans_prior_alpha`/`sticky_diag_alpha`/
//! `bullet_weight`) active, specifically to exercise the cross-term-heavy covariance-prior formula
//! this bug chain centers on. Bit-for-bit (to the printed `f64` precision) cross-language parity,
//! not independently-rederived "looks plausible" test data.

use crate::model::StudentTHmm;

/// Hand-rolled digamma (psi) function -- same "no crate dependency for one special function"
/// convention as `model.rs`'s own `ln_gamma` (no `special`/`statrs` crate pulled in just for
/// this). Recurrence `psi(x) = psi(x+1) - 1/x` shifts the argument up to the asymptotic series'
/// accurate regime (>= 6.0, matching the standard reference implementations' own threshold),
/// then the series (Abramowitz & Stegun 6.3.18, carried to the `x^-10` term for ~1e-13 relative
/// accuracy at the shift boundary). This HMM only ever evaluates digamma at `nu/2`/`(nu+D)/2` for
/// `nu` inside the model's own `[dof_min, dof_max]` clamp range -- always comfortably positive, so
/// no reflection-formula branch for negative arguments is needed (unlike `ln_gamma`'s, kept for
/// completeness there but genuinely unreachable here too).
fn digamma(x: f64) -> f64 {
    let mut x = x;
    let mut result = 0.0;
    while x < 6.0 {
        result -= 1.0 / x;
        x += 1.0;
    }
    let inv = 1.0 / x;
    let inv2 = inv * inv;
    result += x.ln() - 0.5 * inv
        - inv2 * (1.0 / 12.0 - inv2 * (1.0 / 120.0 - inv2 * (1.0 / 252.0 - inv2 * (1.0 / 240.0 - inv2 / 132.0))));
    result
}

/// ECME profile-likelihood score equation for the Student-t degrees of freedom (Liu & Rubin 1995;
/// Kent-Tyler-Vardi fixed point) -- ported verbatim from `student_t_hmm.py`'s
/// `_dof_profile_equation`. `dim` must be the TRUE observation dimension (never a feature-weighted
/// pseudo-dimension -- the same "true `D`" fix `model.rs`'s own emission density already applies).
/// Monotonically decreasing in `nu`: `f(dof_min) < 0` means even the heaviest allowed tail isn't
/// heavy enough (true root below `dof_min`); `f(dof_max) > 0` is the mirror near-Gaussian case --
/// `update_dof_ecme` below clamps to the bound directly in either case rather than root-finding.
pub fn dof_profile_equation(nu: f64, delta_k: f64, dim: f64) -> f64 {
    -digamma(nu / 2.0) + (nu / 2.0).ln() + 1.0 + delta_k + digamma((nu + dim) / 2.0) - ((nu + dim) / 2.0).ln()
}

/// Brent's method root finder on a bracketed interval `[a, b]` with `f(a)` and `f(b)` of opposite
/// sign (caller's responsibility -- `update_dof_ecme` below only calls this after checking that
/// invariant itself). Replaces `scipy.optimize.brentq` (the real production solver `
/// _update_dof_ecme` uses) -- deliberately NOT Atratus's own `solve_dof`, a fixed-10-iteration,
/// unbracketed Newton's-method-with-finite-difference-derivative that can diverge outside a
/// well-behaved region; a real root-bracketing method matches what production training actually
/// runs. Standard algorithm (Brent 1973; Press et al., *Numerical Recipes* §9.3) combining
/// bisection, secant, and inverse quadratic interpolation with a guaranteed-convergence fallback.
#[allow(clippy::too_many_arguments)]
// `fa != fc`/`fb != fc` below are genuine exact-equality checks from the real Brent's-method
// algorithm (deciding inverse-quadratic-interpolation eligibility), not an approximate-equality
// mistake -- a tolerance-based comparison here would silently change which interpolation branch
// the algorithm takes, not just its numerical precision.
#[allow(clippy::float_cmp)]
pub fn brent_root_find(f: impl Fn(f64) -> f64, a0: f64, b0: f64, tol: f64, max_iter: usize) -> f64 {
    let mut a = a0;
    let mut b = b0;
    let mut fa = f(a);
    let mut fb = f(b);
    if fa.abs() < fb.abs() {
        std::mem::swap(&mut a, &mut b);
        std::mem::swap(&mut fa, &mut fb);
    }
    let mut c = a;
    let mut fc = fa;
    let mut mflag = true;
    let mut d = c; // only read after the first iteration, when mflag is false
    let mut s = b;

    for _ in 0..max_iter {
        if fb == 0.0 || (b - a).abs() < tol {
            return b;
        }
        s = if fa != fc && fb != fc {
            // Inverse quadratic interpolation.
            a * fb * fc / ((fa - fb) * (fa - fc))
                + b * fa * fc / ((fb - fa) * (fb - fc))
                + c * fa * fb / ((fc - fa) * (fc - fb))
        } else {
            // Secant method.
            b - fb * (b - a) / (fb - fa)
        };

        let cond1 = !((3.0 * a + b) / 4.0..=b).contains(&s) && !(b..=(3.0 * a + b) / 4.0).contains(&s);
        let cond2 = mflag && (s - b).abs() >= (b - c).abs() / 2.0;
        let cond3 = !mflag && (s - b).abs() >= (c - d).abs() / 2.0;
        let cond4 = mflag && (b - c).abs() < tol;
        let cond5 = !mflag && (c - d).abs() < tol;
        if cond1 || cond2 || cond3 || cond4 || cond5 {
            s = (a + b) / 2.0;
            mflag = true;
        } else {
            mflag = false;
        }

        let fs = f(s);
        d = c;
        c = b;
        fc = fb;
        if fa * fs < 0.0 {
            b = s;
            fb = fs;
        } else {
            a = s;
            fa = fs;
        }
        if fa.abs() < fb.abs() {
            std::mem::swap(&mut a, &mut b);
            std::mem::swap(&mut fa, &mut fb);
        }
    }
    s
}

/// ECME conditional-maximization update for the per-state degrees of freedom, applied directly
/// (no learning-rate blend -- `dof_learning_rate` is confirmed dead code in the Python original,
/// decision #26, dropped from this port entirely: see this crate's own migration spec §5.1a).
/// `gamma_sum` is the FIRST E-step pass's occupancy (shared with the covariance update's
/// denominator); `gammaw_sum_post`/`gamma_logw_sum_post` must come from the SECOND,
/// staleness-corrected pass (`recompute_post_mstep_dof_stats` below) -- passing first-pass values
/// here would silently reintroduce the ECME-staleness bug this file's own module doc warns about.
pub fn update_dof_ecme<const K: usize>(
    dof: &mut [f64; K],
    dof_min: f64,
    dof_max: f64,
    true_dim: f64,
    gamma_sum: &[f64; K],
    gammaw_sum_post: &[f64; K],
    gamma_logw_sum_post: &[f64; K],
) {
    let dim = true_dim.max(1e-6);
    for sid in 0..K {
        let nk = gamma_sum[sid];
        if nk <= 1e-9 {
            continue;
        }
        let delta_k = (gamma_logw_sum_post[sid] - gammaw_sum_post[sid]) / nk;
        if !delta_k.is_finite() {
            continue;
        }

        let f_min = dof_profile_equation(dof_min, delta_k, dim);
        let f_max = dof_profile_equation(dof_max, delta_k, dim);
        let target = if f_min < 0.0 {
            dof_min
        } else if f_max > 0.0 {
            dof_max
        } else {
            brent_root_find(|nu| dof_profile_equation(nu, delta_k, dim), dof_min, dof_max, 1e-8, 100)
        };
        dof[sid] = target.clamp(dof_min, dof_max);
    }
}

/// Shared single source of truth for the relative-delta formula, ported from
/// `student_t_hmm.py`'s own `_relative_delta` (its docstring there: kept as one function so a
/// displayed per-iteration log line can never drift from the actual gating quantity -- the same
/// reasoning applies here).
pub fn relative_delta(current_val: f64, prev_val: f64) -> f64 {
    (current_val - prev_val) / prev_val.abs().max(1e-12)
}

/// One-sided relative-plateau convergence check, ported from `student_t_hmm.py`'s
/// `_check_convergence`. Deliberately one-sided (`0.0 <= rel_delta < tol`, not
/// `rel_delta.abs() < tol`): the canonical EM/ECM/ECME convergence criterion (McLachlan &
/// Krishnan; Wu 1983) is stated as `L(t+1) - L(t) < eps`, which assumes a non-negative delta
/// under the ascent guarantee -- treating a persistent one-directional DECLINE the same as genuine
/// plateau noise is exactly how a real production run "converged" while sliding into an inferior
/// fixed point (documented history, not a hypothetical). Caller must pass `map_ll` (L_MAP), not
/// raw observed-data log-likelihood -- only L_MAP carries the ascent guarantee under MAP-EM/ECME.
pub fn check_convergence(current_val: f64, prev_val: Option<f64>, tol: f64, iter_count: usize, min_iter: usize) -> bool {
    match prev_val {
        None => false,
        Some(_) if iter_count < min_iter => false,
        Some(p) => {
            let rel = relative_delta(current_val, p);
            (0.0..tol).contains(&rel)
        }
    }
}

/// Sequence-level, log-space, max-subtraction-stabilized forward-backward -- ported from
/// `student_t_hmm.py`'s numba-jitted `_forward_backward_numba`. Returns `(log_alpha, log_beta,
/// gamma, log_likelihood)`; `gamma` is already the normalized posterior (`exp(log_gamma)`), not
/// the raw log form, since every M-step consumer needs the linear-space value.
#[allow(clippy::needless_range_loop, clippy::type_complexity)]
pub fn forward_backward<const D: usize, const K: usize>(
    model: &StudentTHmm<D, K>,
    observations: &[[f64; D]],
) -> (Vec<[f64; K]>, Vec<[f64; K]>, Vec<[f64; K]>, f64) {
    let n = observations.len();
    assert!(n > 0, "forward_backward requires at least one observation");

    let log_start: [f64; K] = std::array::from_fn(|k| model.startprob[k].max(1e-300).ln());
    let log_trans: [[f64; K]; K] =
        std::array::from_fn(|i| std::array::from_fn(|j| model.transmat[i][j].max(1e-300).ln()));
    let log_b: Vec<[f64; K]> =
        observations.iter().map(|x| std::array::from_fn(|k| model.log_emission_prob(k, x))).collect();

    let mut log_alpha = vec![[0.0; K]; n];
    let mut log_beta = vec![[0.0; K]; n];

    for k in 0..K {
        log_alpha[0][k] = log_start[k] + log_b[0][k];
    }
    for t in 1..n {
        for j in 0..K {
            let mut m = f64::NEG_INFINITY;
            for i in 0..K {
                let v = log_alpha[t - 1][i] + log_trans[i][j];
                if v > m {
                    m = v;
                }
            }
            if !m.is_finite() {
                log_alpha[t][j] = f64::NEG_INFINITY;
                continue;
            }
            let mut s = 0.0;
            for i in 0..K {
                s += (log_alpha[t - 1][i] + log_trans[i][j] - m).exp();
            }
            log_alpha[t][j] = log_b[t][j] + m + s.ln();
        }
    }

    let mut m_final = f64::NEG_INFINITY;
    for k in 0..K {
        if log_alpha[n - 1][k] > m_final {
            m_final = log_alpha[n - 1][k];
        }
    }
    let mut s_final = 0.0;
    for k in 0..K {
        s_final += (log_alpha[n - 1][k] - m_final).exp();
    }
    let log_likelihood = m_final + s_final.ln();

    for k in 0..K {
        log_beta[n - 1][k] = 0.0;
    }
    for t in (0..n - 1).rev() {
        for i in 0..K {
            let mut m = f64::NEG_INFINITY;
            for j in 0..K {
                let v = log_trans[i][j] + log_b[t + 1][j] + log_beta[t + 1][j];
                if v > m {
                    m = v;
                }
            }
            if !m.is_finite() {
                log_beta[t][i] = f64::NEG_INFINITY;
                continue;
            }
            let mut s = 0.0;
            for j in 0..K {
                s += (log_trans[i][j] + log_b[t + 1][j] + log_beta[t + 1][j] - m).exp();
            }
            log_beta[t][i] = m + s.ln();
        }
    }

    let mut gamma = vec![[0.0; K]; n];
    for t in 0..n {
        let mut log_gamma_t: [f64; K] = std::array::from_fn(|k| log_alpha[t][k] + log_beta[t][k]);
        let mut m = f64::NEG_INFINITY;
        for k in 0..K {
            if log_gamma_t[k] > m {
                m = log_gamma_t[k];
            }
        }
        let mut s = 0.0;
        for k in 0..K {
            s += (log_gamma_t[k] - m).exp();
        }
        let norm = m + s.ln();
        for k in 0..K {
            log_gamma_t[k] -= norm;
            gamma[t][k] = log_gamma_t[k].exp();
        }
    }

    (log_alpha, log_beta, gamma, log_likelihood)
}

/// Accumulated first-E-step-pass sufficient statistics for one EM iteration, summed across every
/// independent segment ("sequence") in the training set -- ported from `fit_streaming()`'s
/// per-chunk accumulators (`start_acc`/`xi_acc`/`gamma_sum`/`gammaw_sum`/`gamma_logw_sum`/
/// `weighted_x_sum`/`weighted_x2_sum`).
#[derive(Debug, Clone, Copy)]
pub struct EmAccumulators<const D: usize, const K: usize> {
    pub start_acc: [f64; K],
    pub xi_acc: [[f64; K]; K],
    pub gamma_sum: [f64; K],
    pub gammaw_sum: [f64; K],
    pub gamma_logw_sum: [f64; K],
    pub weighted_x_sum: [[f64; D]; K],
    pub weighted_x2_sum: [[f64; D]; K],
    pub total_loglik: f64,
}

impl<const D: usize, const K: usize> Default for EmAccumulators<D, K> {
    fn default() -> Self {
        Self {
            start_acc: [0.0; K],
            xi_acc: [[0.0; K]; K],
            gamma_sum: [0.0; K],
            gammaw_sum: [0.0; K],
            gamma_logw_sum: [0.0; K],
            weighted_x_sum: [[0.0; D]; K],
            weighted_x2_sum: [[0.0; D]; K],
            total_loglik: 0.0,
        }
    }
}

impl<const D: usize, const K: usize> EmAccumulators<D, K> {
    /// Runs the E-step (forward-backward + Student-t tail-weight computation) for one segment and
    /// folds its contribution into `self`. Returns the segment's own `gamma` (needed, unmodified,
    /// by the later ECME-staleness-correction pass -- `fit_one_iteration`'s own doc comment).
    // `transmat[i][j]`/`xi`'s 2D indexing is jointly keyed by two loop variables throughout this
    // function -- a loop index, not `.enumerate()`, matches `model.rs`'s own established allows.
    #[allow(clippy::needless_range_loop)]
    pub fn accumulate_segment(&mut self, model: &StudentTHmm<D, K>, observations: &[[f64; D]]) -> Vec<[f64; K]> {
        let n = observations.len();
        let (log_alpha, log_beta, gamma, seg_loglik) = forward_backward(model, observations);
        self.total_loglik += seg_loglik;

        for k in 0..K {
            self.gamma_sum[k] += gamma.iter().map(|g| g[k]).sum::<f64>();
            self.start_acc[k] += gamma[0][k];
        }

        if n > 1 {
            let log_trans: [[f64; K]; K] =
                std::array::from_fn(|i| std::array::from_fn(|j| model.transmat[i][j].max(1e-300).ln()));
            for t in 0..n - 1 {
                let log_b_next: [f64; K] = std::array::from_fn(|k| model.log_emission_prob(k, &observations[t + 1]));
                for i in 0..K {
                    for j in 0..K {
                        let log_xi = log_alpha[t][i] + log_trans[i][j] + log_b_next[j] + log_beta[t + 1][j] - seg_loglik;
                        self.xi_acc[i][j] += log_xi.exp();
                    }
                }
            }
        }

        let nu: [f64; K] = std::array::from_fn(|k| model.dof[k].clamp(model.dof_min, model.dof_max));
        let true_dim = D as f64;
        for t in 0..n {
            let x = &observations[t];
            for k in 0..K {
                let delta = model.weighted_delta(k, x);
                let w = (nu[k] + true_dim) / (nu[k] + delta).max(1e-12);
                let gw = gamma[t][k] * w;
                self.gammaw_sum[k] += gw;
                self.gamma_logw_sum[k] += gamma[t][k] * w.ln();
                for j in 0..D {
                    self.weighted_x_sum[k][j] += gw * x[j];
                    self.weighted_x2_sum[k][j] += gw * x[j] * x[j];
                }
            }
        }

        gamma
    }
}

/// MAP-prior configuration for one M-step call -- ported from `student_t_hmm.py`'s own
/// constructor-level prior fields. `covar_prior_eta_effective` must already be resolved by the
/// caller (`max(covar_prior_eta, covar_prior_min_share * effective_n)`, computed once at
/// initialization time in Python, not recomputed here) -- this struct carries the resolved value,
/// not the raw `covar_prior_eta`/`covar_prior_min_share` pair.
#[derive(Debug, Clone, Copy, Default)]
pub struct EmPriors {
    pub mean_prior_kappa: f64,
    pub covar_prior_eta_effective: f64,
    pub trans_prior_alpha: f64,
    pub sticky_diag_alpha: f64,
    pub min_covar: f64,
}

/// One segment's observations paired with that segment's own `gamma` (first-pass E-step
/// posterior, held fixed across the M-step) -- named alias purely to satisfy clippy's
/// `type_complexity` lint, not a semantically new type.
type SegmentGammaPair<'a, const D: usize, const K: usize> = (&'a [[f64; D]], &'a [[f64; K]]);

/// One M-step update of `model`'s `startprob`/`transmat`/`means`/`covars` in place, given the
/// accumulated first-pass E-step statistics. Ported verbatim from `fit_streaming()`'s M-step
/// (lines ~1422-1472) -- see this module's own doc comment for why the covariance denominator is
/// deliberately `gamma_sum` (unweighted), not `gammaw_sum`, unlike the mean's.
// Every matrix here (`xi_smoothed`/`transmat`/`means`/`covars`) is jointly indexed by two loop
// variables (state x state, or state x feature) -- a loop index is clearer throughout this
// function than threading multiple `.iter_mut().enumerate()` chains in lockstep, matching
// `model.rs`'s own established convention for the same shape of code.
#[allow(clippy::needless_range_loop)]
pub fn m_step<const D: usize, const K: usize>(
    model: &mut StudentTHmm<D, K>,
    acc: &EmAccumulators<D, K>,
    global_mean: &[f64; D],
    global_var: &[f64; D],
    priors: &EmPriors,
) {
    // Startprob.
    let start_sum: f64 = acc.start_acc.iter().sum::<f64>().max(1e-12);
    for k in 0..K {
        model.startprob[k] = acc.start_acc[k] / start_sum;
    }

    // Transmat: Dirichlet prior (`trans_prior_alpha`, every entry) + sticky diagonal prior
    // (`sticky_diag_alpha`, diagonal only), both independent, additive pseudo-counts on `xi_acc`.
    let use_prior = priors.trans_prior_alpha > 0.0 || priors.sticky_diag_alpha > 0.0;
    let mut xi_smoothed = acc.xi_acc;
    if use_prior {
        for i in 0..K {
            for j in 0..K {
                xi_smoothed[i][j] += priors.trans_prior_alpha;
            }
            xi_smoothed[i][i] += priors.sticky_diag_alpha;
        }
    }
    for i in 0..K {
        let row_sum: f64 = xi_smoothed[i].iter().sum();
        if row_sum > 1e-12 {
            for j in 0..K {
                model.transmat[i][j] = xi_smoothed[i][j] / row_sum;
            }
        }
    }
    // Defensive re-normalization (matches `_normalize_rows` -- applied unconditionally even
    // though the loop above already normalizes valid rows, exactly mirroring the Python original).
    for i in 0..K {
        let row_sum: f64 = model.transmat[i].iter().sum::<f64>().max(1e-12);
        for j in 0..K {
            model.transmat[i][j] /= row_sum;
        }
    }

    // Mean: weighted (`gammaw_sum` denominator), optionally shrunk toward `global_mean` by a
    // Normal(m0, Sigma/kappa) prior.
    let safe_gammaw: [f64; K] = std::array::from_fn(|k| acc.gammaw_sum[k].max(1e-12));
    let ml_means: [[f64; D]; K] =
        std::array::from_fn(|k| std::array::from_fn(|j| acc.weighted_x_sum[k][j] / safe_gammaw[k]));
    let new_means: [[f64; D]; K] = if priors.mean_prior_kappa > 0.0 {
        let kappa = priors.mean_prior_kappa;
        std::array::from_fn(|k| {
            std::array::from_fn(|j| (acc.weighted_x_sum[k][j] + kappa * global_mean[j]) / (safe_gammaw[k] + kappa))
        })
    } else {
        ml_means
    };
    model.means = new_means;

    // Covariance: deliberately the UNweighted `gamma_sum` denominator (L_MAP bug-chain fix,
    // "base denominator... from gammaw_sum to the unweighted gamma_sum"; independently matched by
    // Atratus's own `resp_sum`-denominator M-step -- this module's own doc comment).
    let safe_gamma_sum: [f64; K] = std::array::from_fn(|k| acc.gamma_sum[k].max(1e-12));
    for k in 0..K {
        let mut numerator = [0.0; D];
        let mut denom = safe_gamma_sum[k];
        for j in 0..D {
            let tau_hat_ml = acc.weighted_x2_sum[k][j] / safe_gammaw[k] - ml_means[k][j] * ml_means[k][j];
            let raw_var_ml = model.feature_weights[j] * tau_hat_ml;
            numerator[j] = safe_gammaw[k] * raw_var_ml;
        }
        if priors.mean_prior_kappa > 0.0 {
            let kappa = priors.mean_prior_kappa;
            for j in 0..D {
                let centering_correction = model.feature_weights[j] * (ml_means[k][j] - new_means[k][j]).powi(2);
                let cross_term = kappa * (new_means[k][j] - global_mean[j]).powi(2);
                numerator[j] += safe_gammaw[k] * centering_correction + cross_term;
            }
            denom += 1.0;
        }
        if priors.covar_prior_eta_effective > 0.0 {
            let eta = priors.covar_prior_eta_effective;
            for j in 0..D {
                numerator[j] += eta * global_var[j];
            }
            denom += eta;
        }
        for j in 0..D {
            model.covars[k][j] = (numerator[j] / denom).max(priors.min_covar);
        }
    }
}

/// Second-pass, forward-backward-free recomputation of the DOF-update sufficient statistics under
/// the JUST-UPDATED mean/covariance -- the ECME-staleness fix (Liu & Rubin 1995) this module's own
/// doc comment describes. `gamma` is reused unchanged from the first pass (a genuine E-step
/// quantity, fixed for the whole M-step); only the Mahalanobis deltas (and therefore the
/// Student-t tail weights derived from them) are recomputed, under `model`'s NEW parameters.
pub fn recompute_post_mstep_dof_stats<const D: usize, const K: usize>(
    model: &StudentTHmm<D, K>,
    segments: &[SegmentGammaPair<D, K>],
) -> ([f64; K], [f64; K]) {
    let mut gammaw_sum_post = [0.0; K];
    let mut gamma_logw_sum_post = [0.0; K];
    let true_dim = D as f64;
    let nu: [f64; K] = std::array::from_fn(|k| model.dof[k].clamp(model.dof_min, model.dof_max));

    for &(observations, gamma) in segments {
        for (t, x) in observations.iter().enumerate() {
            for k in 0..K {
                let delta = model.weighted_delta(k, x);
                let w = (nu[k] + true_dim) / (nu[k] + delta).max(1e-12);
                gammaw_sum_post[k] += gamma[t][k] * w;
                gamma_logw_sum_post[k] += gamma[t][k] * w.ln();
            }
        }
    }
    (gammaw_sum_post, gamma_logw_sum_post)
}

/// Orchestrates one full EM iteration (E-step across every segment, M-step, and -- if
/// `update_dof` -- the ECME-staleness-correct two-pass DOF update) in place on `model`. Returns
/// the first-pass total log-likelihood (the raw observed-data log-likelihood, matching Python's
/// `total_loglik` -- NOT `L_MAP`; a caller tracking convergence via `check_convergence` must add
/// `model`'s own log-prior term itself, mirroring `fit_streaming()`'s own `map_ll = total_loglik +
/// self._compute_log_prior()` composition, not duplicated inside this function since the log-prior
/// computation only depends on `model`'s post-M-step parameters, not on anything this function
/// alone owns).
pub fn fit_one_iteration<const D: usize, const K: usize>(
    model: &mut StudentTHmm<D, K>,
    segments: &[&[[f64; D]]],
    global_mean: &[f64; D],
    global_var: &[f64; D],
    priors: &EmPriors,
    update_dof: bool,
) -> f64 {
    let mut acc = EmAccumulators::<D, K>::default();
    let mut segment_gammas: Vec<Vec<[f64; K]>> = Vec::with_capacity(segments.len());
    for seg in segments {
        let gamma = acc.accumulate_segment(model, seg);
        segment_gammas.push(gamma);
    }
    let gamma_sum = acc.gamma_sum;
    let total_loglik = acc.total_loglik;

    m_step(model, &acc, global_mean, global_var, priors);

    if update_dof {
        let pairs: Vec<SegmentGammaPair<D, K>> =
            segments.iter().zip(segment_gammas.iter()).map(|(&s, g)| (s, g.as_slice())).collect();
        let (gammaw_sum_post, gamma_logw_sum_post) = recompute_post_mstep_dof_stats(model, &pairs);
        update_dof_ecme(
            &mut model.dof,
            model.dof_min,
            model.dof_max,
            D as f64,
            &gamma_sum,
            &gammaw_sum_post,
            &gamma_logw_sum_post,
        );
    }

    total_loglik
}

/// MAP-EM log-prior kernel -- ported verbatim from `student_t_hmm.py`'s own `_compute_log_prior()`
/// (Green 1990: MAP-EM guarantees ascent of `L_MAP = L_raw + log p(theta)`, not raw LL alone).
/// Additive normalizing constants that don't vary across iterations are omitted (irrelevant for
/// both of this value's real uses -- `argmax_t` comparison for checkpoint-restore, and relative-
/// delta convergence checks -- matching the Python docstring's own justification). Must be called
/// with `model`'s CURRENT (post-M-step) parameters, since `fit()` below scores each iteration's
/// L_MAP after that iteration's own M-step has already run.
// Mixed 1D (`global_mean`/`global_var`) and 2D (`model.means`/`model.covars`/`model.transmat`)
// indexing in the same loop bodies below -- a loop index, not `.enumerate()`, same convention as
// `model.rs`'s/this file's own `m_step`.
#[allow(clippy::needless_range_loop)]
pub fn compute_log_prior<const D: usize, const K: usize>(
    model: &StudentTHmm<D, K>,
    global_mean: &[f64; D],
    global_var: &[f64; D],
    priors: &EmPriors,
) -> f64 {
    let mut log_prior = 0.0;

    if priors.mean_prior_kappa > 0.0 {
        let kappa = priors.mean_prior_kappa;
        let mut sum = 0.0;
        for k in 0..K {
            for j in 0..D {
                let diff2 = (model.means[k][j] - global_mean[j]).powi(2);
                sum += diff2 / model.covars[k][j];
            }
        }
        log_prior += -0.5 * kappa * sum;
    }

    if priors.covar_prior_eta_effective > 0.0 {
        let eta = priors.covar_prior_eta_effective;
        let mut sum = 0.0;
        for k in 0..K {
            for j in 0..D {
                sum += model.covars[k][j].ln() + global_var[j] / model.covars[k][j];
            }
        }
        log_prior += -0.5 * eta * sum;
    }

    if priors.trans_prior_alpha > 0.0 || priors.sticky_diag_alpha > 0.0 {
        let mut sum = 0.0;
        for i in 0..K {
            for j in 0..K {
                let alpha_minus_1 =
                    priors.trans_prior_alpha + if i == j { priors.sticky_diag_alpha } else { 0.0 };
                let safe_trans = model.transmat[i][j].max(1e-300);
                sum += alpha_minus_1 * safe_trans.ln();
            }
        }
        log_prior += sum;
    }

    log_prior
}

/// Best-training-checkpoint snapshot of the parameters this crate actually trains (mirrors
/// `fit_streaming()`'s own `best_train_params` dict -- `feature_weights`/`dof_min`/`dof_max` are
/// never touched by the M-step, so they don't need snapshotting here).
#[derive(Debug, Clone, Copy)]
struct Checkpoint<const D: usize, const K: usize> {
    means: [[f64; D]; K],
    covars: [[f64; D]; K],
    transmat: [[f64; K]; K],
    startprob: [f64; K],
    dof: [f64; K],
    map_ll: f64,
    raw_ll: f64,
    iter: usize,
}

impl<const D: usize, const K: usize> Checkpoint<D, K> {
    fn capture(model: &StudentTHmm<D, K>, map_ll: f64, raw_ll: f64, iter: usize) -> Self {
        Self {
            means: model.means,
            covars: model.covars,
            transmat: model.transmat,
            startprob: model.startprob,
            dof: model.dof,
            map_ll,
            raw_ll,
            iter,
        }
    }

    fn restore(&self, model: &mut StudentTHmm<D, K>) {
        model.means = self.means;
        model.covars = self.covars;
        model.transmat = self.transmat;
        model.startprob = self.startprob;
        model.dof = self.dof;
    }
}

/// Diagnostics from a full `fit()` run -- mirrors the subset of `fit_streaming()`'s own
/// post-training attributes (`map_ll_history_`/`monitor_.converged`/`checkpoint_restored_`/
/// `checkpoint_restore_info_`) this crate's own test/calling code actually needs.
#[derive(Debug, Clone)]
pub struct FitResult {
    pub map_ll_history: Vec<f64>,
    pub converged: bool,
    pub iterations_run: usize,
    pub checkpoint_restored: bool,
    pub best_train_iter: usize,
    pub best_train_map_ll: f64,
    pub best_train_raw_ll: f64,
}

/// Runs EM to convergence (or `n_iter` exhaustion), scoring each iteration by
/// `L_MAP = total_loglik + compute_log_prior(...)` and restoring the best-observed checkpoint if the final iteration's
/// L_MAP ends up worse than an earlier one -- ported verbatim from `fit_streaming()`'s own
/// iteration loop (lines ~1518-1583): `best_train_map_ll` is updated on a STRICT `>` (first-found
/// wins on ties, matching Python), `_check_convergence` is one-sided (declines never "converge" --
/// see this module's own `check_convergence` doc comment), and the post-loop restore-if-worse
/// check runs unconditionally, independent of whether the loop exited by convergence or by
/// exhausting `n_iter`. Deliberately does NOT port `fit_streaming()`'s validation-based early-
/// stopping path (`early_stopping_patience`/`X_val`) -- a separate, orthogonal generalization
/// mechanism, out of scope for this orchestration slice; also does not initialize `model` --
/// k-means++ seeding needs a real PRNG with no numpy-`default_rng`-bit-exact Rust equivalent, a
/// separate open question not silently folded in here -- callers must pass an already-initialized
/// model, exactly as this file's own golden-parity tests already do.
#[allow(clippy::too_many_arguments)] // one named field per real EM-loop parameter; matches
// `StudentTHmm::new`'s own established "a builder would hide which fields are actually required"
// rationale.
pub fn fit<const D: usize, const K: usize>(
    model: &mut StudentTHmm<D, K>,
    segments: &[&[[f64; D]]],
    global_mean: &[f64; D],
    global_var: &[f64; D],
    priors: &EmPriors,
    update_dof: bool,
    n_iter: usize,
    min_iter: usize,
    tol: f64,
) -> FitResult {
    let mut map_ll_history = Vec::with_capacity(n_iter);
    let mut prev_map_ll: Option<f64> = None;
    let mut best: Option<Checkpoint<D, K>> = None;
    let mut converged = false;
    let mut iterations_run = 0;
    let mut final_map_ll = f64::NEG_INFINITY;

    for em_iter in 0..n_iter {
        iterations_run = em_iter + 1;
        let total_loglik = fit_one_iteration(model, segments, global_mean, global_var, priors, update_dof);
        let log_prior = compute_log_prior(model, global_mean, global_var, priors);
        let map_ll = total_loglik + log_prior;
        map_ll_history.push(map_ll);
        final_map_ll = map_ll;

        let is_new_best = match &best {
            None => true,
            Some(b) => map_ll > b.map_ll,
        };
        if is_new_best {
            best = Some(Checkpoint::capture(model, map_ll, total_loglik, em_iter));
        }

        if check_convergence(map_ll, prev_map_ll, tol, iterations_run, min_iter) {
            converged = true;
            break;
        }
        prev_map_ll = Some(map_ll);
    }

    // `best` is always `Some` after at least one iteration (seeded on the very first pass above).
    let best = best.expect("fit() requires n_iter >= 1 and at least one segment");
    let (checkpoint_restored, best_train_iter, best_train_map_ll, best_train_raw_ll) =
        (final_map_ll < best.map_ll, best.iter, best.map_ll, best.raw_ll);
    if checkpoint_restored {
        best.restore(model);
    }

    FitResult {
        map_ll_history,
        converged,
        iterations_run,
        checkpoint_restored,
        best_train_iter,
        best_train_map_ll,
        best_train_raw_ll,
    }
}

#[cfg(test)]
#[allow(clippy::unwrap_used, clippy::needless_range_loop)]
mod tests {
    use super::*;

    const EPS: f64 = 1e-9;

    fn assert_close(label: &str, got: f64, want: f64, eps: f64) {
        assert!(
            (got - want).abs() <= eps * want.abs().max(1.0),
            "{label}: got {got}, want {want} (diff {})",
            (got - want).abs()
        );
    }

    #[test]
    fn digamma_matches_known_values() {
        // Known closed forms (Abramowitz & Stegun 6.3.2-6.3.3): psi(1) = -gamma_euler,
        // psi(1/2) = -2ln2 - gamma_euler, psi(2) = 1 - gamma_euler.
        const EULER_GAMMA: f64 = 0.5772156649015329;
        assert_close("digamma(1)", digamma(1.0), -EULER_GAMMA, 1e-10);
        assert_close("digamma(0.5)", digamma(0.5), -2.0 * 2f64.ln() - EULER_GAMMA, 1e-10);
        assert_close("digamma(2)", digamma(2.0), 1.0 - EULER_GAMMA, 1e-10);
        assert_close("digamma(10)", digamma(10.0), 2.251752589066721, 1e-10);
    }

    #[test]
    fn brent_finds_known_root() {
        // f(x) = x^2 - 2, root = sqrt(2), bracketed [0, 2].
        let root = brent_root_find(|x| x * x - 2.0, 0.0, 2.0, 1e-12, 200);
        assert_close("brent sqrt(2)", root, 2f64.sqrt(), 1e-9);
    }

    #[test]
    fn relative_delta_and_convergence_one_sided() {
        // A positive-but-tiny improvement under tol, past min_iter: converged.
        assert!(check_convergence(100.0001, Some(100.0), 1e-3, 5, 2));
        // A negative delta (decline), however small: never treated as convergence.
        assert!(!check_convergence(99.9999, Some(100.0), 1e-3, 5, 2));
        // Before min_iter: never converged regardless of delta.
        assert!(!check_convergence(100.0001, Some(100.0), 1e-3, 1, 2));
        // No previous value yet: never converged.
        assert!(!check_convergence(100.0, None, 1e-3, 5, 2));
    }

    /// 8-row/D=2/K=2 synthetic dataset shared by both golden-parity tests below -- identical to
    /// the fixture used to generate the golden values from the real Python `StudentTHMM` (mts
    /// conda env, 2026-10-11).
    fn golden_observations() -> Vec<[f64; 2]> {
        vec![
            [0.10, 0.20],
            [0.15, 0.25],
            [0.05, 0.18],
            [3.00, 3.10],
            [3.20, 2.90],
            [2.95, 3.05],
            [0.12, 0.22],
            [3.10, 3.00],
        ]
    }

    #[test]
    fn golden_parity_m_step_no_priors() {
        // Exact Python invocation (mts env): StudentTHMM(n_components=2, n_iter=1, min_covar=1e-6,
        // dof=6.0, update_dof=True, dof_min=2.1, dof_max=100.0, all priors=0, bullet_weight=1.0),
        // seeded with means=[[0.1,0.2],[3.0,3.0]], covars=[[0.05,0.05],[0.05,0.05]],
        // transmat=[[0.9,0.1],[0.1,0.9]], startprob=[0.5,0.5], dof=[6.0,6.0], fit_streaming() over
        // one single-segment chunk containing all 8 rows.
        let mut model = StudentTHmm::<2, 2>::new_unweighted(
            [[0.1, 0.2], [3.0, 3.0]],
            [[0.05, 0.05], [0.05, 0.05]],
            [[0.9, 0.1], [0.1, 0.9]],
            [0.5, 0.5],
            [6.0, 6.0],
            2.1,
            100.0,
        );
        let obs = golden_observations();
        let global_mean: [f64; 2] = {
            let n = obs.len() as f64;
            std::array::from_fn(|j| obs.iter().map(|x| x[j]).sum::<f64>() / n)
        };
        let global_var = [0.0; 2]; // unused: both priors disabled below
        let priors = EmPriors { min_covar: 1e-6, ..Default::default() };

        fit_one_iteration(&mut model, &[obs.as_slice()], &global_mean, &global_var, &priors, true);

        let want_means = [[0.10493677377723291, 0.21241852920248008], [3.0580000992731518, 3.015952338686337]];
        let want_covars = [[0.0017458066687712531, 0.0008806089107920058], [0.01124007449793242, 0.00658273353812226]];
        let want_transmat = [[0.5000014212033276, 0.49999857879667237], [0.333329199030099, 0.666670800969901]];
        let want_startprob = [0.9999999880036102, 1.1996389792796462e-08];
        let want_dof = [9.278677210584084, 10.490399909935109];

        for k in 0..2 {
            for j in 0..2 {
                assert_close(&format!("means[{k}][{j}]"), model.means[k][j], want_means[k][j], EPS);
                assert_close(&format!("covars[{k}][{j}]"), model.covars[k][j], want_covars[k][j], EPS);
                assert_close(&format!("transmat[{k}][{j}]"), model.transmat[k][j], want_transmat[k][j], EPS);
            }
            assert_close(&format!("startprob[{k}]"), model.startprob[k], want_startprob[k], EPS);
            assert_close(&format!("dof[{k}]"), model.dof[k], want_dof[k], 1e-6);
        }
    }

    #[test]
    fn golden_parity_m_step_with_all_priors() {
        // Same fixture, but every MAP prior active: mean_prior_kappa=2.0, covar_prior_eta=3.0
        // (covar_prior_min_share=0.03 -> effective_n=8 -> min_share floor 0.24 < 3.0, so
        // covar_prior_eta_effective stays 3.0), trans_prior_alpha=0.5, sticky_diag_alpha=1.0,
        // bullet_dims=(1,) with bullet_weight=1.4 (feature_weights=[1.0, 1.4]) -- deliberately
        // exercises the cross-term-heavy covariance-prior formula the L_MAP bug chain centers on.
        let mut model = StudentTHmm::<2, 2>::new(
            [[0.1, 0.2], [3.0, 3.0]],
            [[0.05, 0.05], [0.05, 0.05]],
            [[0.9, 0.1], [0.1, 0.9]],
            [0.5, 0.5],
            [6.0, 6.0],
            2.1,
            100.0,
            [1.0, 1.4],
        );
        let obs = golden_observations();
        let global_mean: [f64; 2] = {
            let n = obs.len() as f64;
            std::array::from_fn(|j| obs.iter().map(|x| x[j]).sum::<f64>() / n)
        };
        let global_var: [f64; 2] = {
            let n = obs.len() as f64;
            std::array::from_fn(|j| {
                let mean = obs.iter().map(|x| x[j]).sum::<f64>() / n;
                obs.iter().map(|x| (x[j] - mean).powi(2)).sum::<f64>() / n
            })
        };
        let priors = EmPriors {
            mean_prior_kappa: 2.0,
            covar_prior_eta_effective: 3.0,
            trans_prior_alpha: 0.5,
            sticky_diag_alpha: 1.0,
            min_covar: 1e-6,
        };

        fit_one_iteration(&mut model, &[obs.as_slice()], &global_mean, &global_var, &priors, true);

        let want_means = [[0.5106537898957514, 0.5965369995982484], [2.6368068465361483, 2.615038477321767]];
        let want_covars = [[1.2195977893738783, 1.1314090004965514], [1.2156589368156812, 1.132651179298951]];
        let want_transmat = [[0.5833338735929231, 0.4166661264070769], [0.2999987886919256, 0.7000012113080745]];
        let want_startprob = [0.999999994007437, 5.992563056215892e-09];
        let want_dof = [7.513956807341204, 7.7457042471788915];

        for k in 0..2 {
            for j in 0..2 {
                assert_close(&format!("means[{k}][{j}]"), model.means[k][j], want_means[k][j], EPS);
                assert_close(&format!("covars[{k}][{j}]"), model.covars[k][j], want_covars[k][j], EPS);
                assert_close(&format!("transmat[{k}][{j}]"), model.transmat[k][j], want_transmat[k][j], EPS);
            }
            assert_close(&format!("startprob[{k}]"), model.startprob[k], want_startprob[k], EPS);
            assert_close(&format!("dof[{k}]"), model.dof[k], want_dof[k], 1e-6);
        }
    }

    #[test]
    fn golden_parity_fit_ascending_no_restore() {
        // Same no-priors fixture as `golden_parity_m_step_no_priors`, but n_iter=5/min_iter=1/
        // tol=1e-9 (real Python invocation, mts env, 2026-10-11): map_ll strictly ascends every
        // iteration here, so the loop runs all 5 iterations without converging early (tol is too
        // tight to trip) and without ever needing a checkpoint restore -- the common/healthy path.
        let mut model = StudentTHmm::<2, 2>::new_unweighted(
            [[0.1, 0.2], [3.0, 3.0]],
            [[0.05, 0.05], [0.05, 0.05]],
            [[0.9, 0.1], [0.1, 0.9]],
            [0.5, 0.5],
            [6.0, 6.0],
            2.1,
            100.0,
        );
        let obs = golden_observations();
        let global_mean: [f64; 2] = {
            let n = obs.len() as f64;
            std::array::from_fn(|j| obs.iter().map(|x| x[j]).sum::<f64>() / n)
        };
        let global_var = [0.0; 2];
        let priors = EmPriors { min_covar: 1e-6, ..Default::default() };

        let result = fit(&mut model, &[obs.as_slice()], &global_mean, &global_var, &priors, true, 5, 1, 1e-9);

        let want_map_ll_history =
            [0.180218619413351, 19.648454784379755, 20.08657779393678, 20.12970400101858, 20.15377884825991];
        assert_eq!(result.iterations_run, 5);
        assert!(!result.converged, "tol=1e-9 should not trip in exactly 5 iterations here");
        assert!(!result.checkpoint_restored, "map_ll ascends monotonically in this fixture");
        assert_eq!(result.map_ll_history.len(), 5);
        for (i, (&got, &want)) in result.map_ll_history.iter().zip(want_map_ll_history.iter()).enumerate() {
            assert_close(&format!("map_ll_history[{i}]"), got, want, EPS);
        }

        let want_means = [[0.10580231279435258, 0.2123714949525511], [3.0591387017280667, 3.0154223664902404]];
        let want_covars = [[0.0011822415057624075, 0.0006038456330330049], [0.00882305195613155, 0.005171370255533211]];
        let want_dof = [16.797609840992884, 32.3743235968432];
        for k in 0..2 {
            for j in 0..2 {
                assert_close(&format!("means[{k}][{j}]"), model.means[k][j], want_means[k][j], EPS);
                assert_close(&format!("covars[{k}][{j}]"), model.covars[k][j], want_covars[k][j], EPS);
            }
            assert_close(&format!("dof[{k}]"), model.dof[k], want_dof[k], 1e-6);
        }
    }

    #[test]
    fn golden_parity_fit_declining_triggers_restore() {
        // K=3/D=2, every prior active (mean_prior_kappa=5.0, covar_prior_eta_effective=8.0 --
        // covar_prior_min_share=0.5 * n=8 = 4.0 < 8.0, so eta itself wins -- trans_prior_alpha=0.1,
        // sticky_diag_alpha=2.0, bullet_dims=(1,)/bullet_weight=1.4), n_iter=15/min_iter=1/
        // tol=1e-12, deliberately adversarial starting point (3 states crammed onto an 8-row/
        // 2-cluster dataset). Real Python invocation (mts env, 2026-10-11) genuinely declines
        // after iteration 0 and never recovers -- `checkpoint_restored_=True`, restoring
        // iteration 0's own parameters. This is the one golden fixture that exercises the actual
        // restore branch, not just the "ascended, no restore needed" common path above.
        let mut model = StudentTHmm::<2, 3>::new(
            [[0.1, 0.2], [3.0, 3.0], [1.5, 1.5]],
            [[0.05, 0.05], [0.05, 0.05], [0.05, 0.05]],
            [[0.8, 0.1, 0.1], [0.1, 0.8, 0.1], [0.1, 0.1, 0.8]],
            [0.34, 0.33, 0.33],
            [6.0, 6.0, 6.0],
            2.1,
            100.0,
            [1.0, 1.4],
        );
        let obs = golden_observations();
        let global_mean: [f64; 2] = {
            let n = obs.len() as f64;
            std::array::from_fn(|j| obs.iter().map(|x| x[j]).sum::<f64>() / n)
        };
        let global_var: [f64; 2] = {
            let n = obs.len() as f64;
            std::array::from_fn(|j| {
                let mean = obs.iter().map(|x| x[j]).sum::<f64>() / n;
                obs.iter().map(|x| (x[j] - mean).powi(2)).sum::<f64>() / n
            })
        };
        let priors = EmPriors {
            mean_prior_kappa: 5.0,
            covar_prior_eta_effective: 8.0,
            trans_prior_alpha: 0.1,
            sticky_diag_alpha: 2.0,
            min_covar: 1e-6,
        };

        let result = fit(&mut model, &[obs.as_slice()], &global_mean, &global_var, &priors, true, 15, 1, 1e-12);

        assert_eq!(result.iterations_run, 15);
        assert!(!result.converged, "real Python trace runs all 15 iterations without tripping tol");
        assert!(result.checkpoint_restored, "real Python trace genuinely declines and restores");
        assert_eq!(result.best_train_iter, 0);
        assert_close("best_train_map_ll", result.best_train_map_ll, -47.75961273352509, EPS);
        assert_close("best_train_raw_ll", result.best_train_raw_ll, 0.5421763962281108, EPS);

        let want_means = [
            [0.8235281753599829, 0.8927535098083917],
            [2.3209312519420693, 2.314316427999073],
            [1.583749545886866, 1.612499566428662],
        ];
        let want_covars = [
            [1.7818506509682766, 1.671347006983636],
            [1.7703101126628777, 1.6654345099104966],
            [1.9484142232944837, 1.7449443220762553],
        ];
        let want_transmat = [
            [0.650795864496576, 0.33332942135629856, 0.015874714147125404],
            [0.2075424434813158, 0.7735856320128301, 0.01887192450585412],
            [0.043479295678761645, 0.04348776506791534, 0.913032939253323],
        ];
        let want_startprob = [0.9999978475171508, 6.543354241535294e-09, 2.145939495040652e-06];
        let want_dof = [10.092464825871533, 10.593527140770762, 23.871273412170524];

        for k in 0..3 {
            for j in 0..2 {
                assert_close(&format!("means[{k}][{j}]"), model.means[k][j], want_means[k][j], EPS);
                assert_close(&format!("covars[{k}][{j}]"), model.covars[k][j], want_covars[k][j], EPS);
            }
            for j in 0..3 {
                assert_close(&format!("transmat[{k}][{j}]"), model.transmat[k][j], want_transmat[k][j], EPS);
            }
            assert_close(&format!("startprob[{k}]"), model.startprob[k], want_startprob[k], EPS);
            assert_close(&format!("dof[{k}]"), model.dof[k], want_dof[k], 1e-6);
        }
    }
}
