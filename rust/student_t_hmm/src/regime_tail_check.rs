//! regime_tail_check.rs -- does each regime's fitted tail heaviness match the data's?
//! (rust-migration spec §6.2.13, fat-tail tools item 1: the HMM's never-passed fat-tail sign-off.)
//!
//! Under regime k the model says the inputs are multivariate Student-t with nu_k degrees of
//! freedom. Then the radius r = sqrt(delta) (delta = the weighted Mahalanobis term over the present
//! inputs) has a Pareto-type tail with index nu_k: P(r > x) ~ x^{-nu_k}, extreme value index
//! gamma = 1 / nu_k, whatever the number of inputs, their scales, fixed weights or which are absent.
//! So the check estimates gamma from the radii of the steps assigned to regime k and compares it
//! with the same estimate on radii simulated from the fitted regime (see below for why not with
//! 1 / nu_k directly).
//!
//! Estimator (all formulas checked against Caeiro, Gomes & Pestana 2005, "Direct reduction of bias
//! of the classical Hill estimator", REVSTAT 3(2)): with X_{n-i+1:n} the i-th largest of n
//! positive values,
//!   Hill        H(k) = (1/k) sum_{i=1..k} ln X_{n-i+1:n} - ln X_{n-k:n}
//!   corrected   Hbar(k) = H(k) (1 - beta/(1 - rho) (n/k)^rho)            (their (1.13))
//! with the second-order parameters estimated at the larger level k1 = min(n - 1, [2n / ln ln n]):
//!   rho (Fraga Alves, Gomes & de Haan 2003), tau = 0:
//!     M_j(k) = (1/k) sum (ln X_{n-i+1:n} - ln X_{n-k:n})^j,
//!     T = (ln M_1 - ln(M_2/2)/2) / (ln(M_2/2)/2 - ln(M_3/6)/3),  rho = -|3 (T - 1)/(T - 3)|;
//!   beta (Gomes & Martins 2002), scaled log-spacings U_i = i (ln X_{n-i+1:n} - ln X_{n-i:n}):
//!     beta = (k/n)^rho [d(rho) D(0) - D(rho)] / [d(rho) D(rho) - D(2 rho)],
//!     d(a) = (1/k) sum (i/k)^{-a},  D(a) = (1/k) sum (i/k)^{-a} U_i.
//! tau = 0 is the paper's advice for rho in [-1, 0); a Student-t tail has rho = -2/nu, so this
//! holds for nu >= 2 -- the model's whole dof range.
//!
//! Level k: the MSE-optimal Hill level k0 = ((1 - rho)^2 n^{-2 rho} / (-2 rho beta^2))^{1/(1 - 2 rho)}
//! (their (1.12)), clamped to [10, n - 1].
//!
//! Why the check does NOT compare 1/gamma with nu_k directly: tail estimators are biased for
//! thinner tails because the second-order parameter (rho = -2/nu for a Student-t) is estimated very
//! slowly as rho -> 0. Measured here on exact multivariate-t data (150,000 radii): true nu 3 / 4 / 6
//! / 8 / 12 read as 3.0 / 3.85 / 5.4 / 6.2 / 7.5. A direct comparison would accuse a correct model.
//! So the check compares like with like (a Monte Carlo test): the same estimator is applied to
//! radii simulated from the fitted regime, one per assigned step with that step's present-input
//! mask (so weights and absent inputs match), repeated to get the estimator's spread under the
//! model; the estimator's bias is shared and cancels. Verdict: z = (gamma_data - mean gamma_model) /
//! sqrt(sd_data^2 + sd_model^2), where sd_data comes from a moving-block bootstrap (market data
//! cluster; IID errors are far too optimistic, Kearns & Pagan 1997).
//!
//! A second, estimator-free view: the fraction of the data's radii above the model's own 99% and
//! 99.9% radius quantiles, against the expected 1% and 0.1% (exceedance ratios; 1 = calibrated,
//! > 1 = the data's tails are heavier than the model's).
//!
//! Caveat recorded with every result: steps are assigned to a regime by a posterior threshold;
//! ambiguous steps are left out, and extreme steps tend to be assigned to the heaviest-tailed
//! regime, so the check compares like with like only when regimes are well separated.

use crate::continuous_time_student_t::{ContinuousTimeStudentT, Step};
use crate::init::SplitMix64;
use crate::model::MIN_COVAR;
use crate::regime_path_simulator::student_t_draw;

/// Second-order parameters (rho < 0, beta) of a Hall-class tail.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SecondOrder {
    pub rho: f64,
    pub beta: f64,
}

/// Positive values sorted in DEScending order (largest first).
fn descending(values: &[f64]) -> Vec<f64> {
    let mut v: Vec<f64> = values.iter().copied().filter(|x| x.is_finite() && *x > 0.0).collect();
    v.sort_by(|a, b| b.total_cmp(a));
    v
}

/// Hill estimate of gamma from the k largest of `sorted_desc` (needs k < len).
fn hill(sorted_desc: &[f64], k: usize) -> f64 {
    let threshold = sorted_desc[k].ln();
    sorted_desc[..k].iter().map(|x| x.ln() - threshold).sum::<f64>() / k as f64
}

/// The level k1 = min(n - 1, [2n / ln ln n]) for the second-order parameters.
fn level_k1(n: usize) -> usize {
    let nf = n as f64;
    let k = (2.0 * nf / nf.ln().ln()).floor() as usize;
    k.min(n - 1)
}

/// rho (tau = 0) and beta at level k (Fraga Alves et al. 2003; Gomes & Martins 2002).
fn second_order_at(sorted_desc: &[f64], k: usize) -> Option<SecondOrder> {
    let n = sorted_desc.len();
    if k < 3 || k >= n {
        return None;
    }
    let threshold = sorted_desc[k].ln();
    let (mut m1, mut m2, mut m3) = (0.0, 0.0, 0.0);
    for x in &sorted_desc[..k] {
        let l = x.ln() - threshold;
        m1 += l;
        m2 += l * l;
        m3 += l * l * l;
    }
    let kf = k as f64;
    let (m1, m2, m3) = (m1 / kf, m2 / kf, m3 / kf);
    let a = (m2 / 2.0).ln() / 2.0;
    let t = (m1.ln() - a) / (a - (m3 / 6.0).ln() / 3.0);
    let rho = -(3.0 * (t - 1.0) / (t - 3.0)).abs();
    if !rho.is_finite() || rho >= 0.0 {
        return None;
    }
    // Scaled log-spacings U_i = i (ln X_{n-i+1} - ln X_{n-i}), i = 1..k (1-based on the descending list).
    let (mut d_rho, mut big_d0, mut big_d_rho, mut big_d_2rho) = (0.0, 0.0, 0.0, 0.0);
    for i in 1..=k {
        let u = i as f64 * (sorted_desc[i - 1].ln() - sorted_desc[i].ln());
        let ratio = i as f64 / kf;
        let w = ratio.powf(-rho);
        d_rho += w;
        big_d0 += u;
        big_d_rho += w * u;
        big_d_2rho += ratio.powf(-2.0 * rho) * u;
    }
    let (d_rho, big_d0, big_d_rho, big_d_2rho) = (d_rho / kf, big_d0 / kf, big_d_rho / kf, big_d_2rho / kf);
    let beta = (kf / n as f64).powf(rho) * (d_rho * big_d0 - big_d_rho) / (d_rho * big_d_rho - big_d_2rho);
    beta.is_finite().then_some(SecondOrder { rho, beta })
}

/// One tail estimate.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TailEstimate {
    pub n: usize,
    pub k: usize,
    pub hill_gamma: f64,
    /// Bias-corrected gamma (Caeiro, Gomes & Pestana 2005, (1.13)).
    pub corrected_gamma: f64,
    pub second_order: SecondOrder,
}

/// Bias-corrected tail index of positive `values` at level `k`, or at the Hill-optimal level when
/// `k` is `None`. `None` if fewer than 50 positive values or the second-order fit fails.
pub fn tail_estimate(values: &[f64], k: Option<usize>) -> Option<TailEstimate> {
    let sorted = descending(values);
    let n = sorted.len();
    if n < 50 {
        return None;
    }
    let so = second_order_at(&sorted, level_k1(n))?;
    let k = match k {
        Some(k) => k.clamp(10, n - 1),
        None => {
            let (rho, beta) = (so.rho, so.beta);
            let k0 = ((1.0 - rho).powi(2) * (n as f64).powf(-2.0 * rho) / (-2.0 * rho * beta * beta)).powf(1.0 / (1.0 - 2.0 * rho));
            if k0.is_finite() { (k0.floor() as usize).clamp(10, n - 1) } else { (n / 10).max(10) }
        }
    };
    let h = hill(&sorted, k);
    let corrected = h * (1.0 - so.beta / (1.0 - so.rho) * (n as f64 / k as f64).powf(so.rho));
    Some(TailEstimate { n, k, hill_gamma: h, corrected_gamma: corrected, second_order: so })
}

/// Settings for the per-regime check.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TailCheckSettings {
    /// A step counts for regime k when its posterior probability of k is at least this.
    pub min_posterior: f64,
    /// Block-bootstrap replicates of the data's estimate.
    pub bootstrap_replicates: usize,
    /// Moving-block length in steps (dependence span).
    pub block_length: usize,
    /// Model-simulated replicates for the reference spread.
    pub model_replicates: usize,
    /// |z| above this marks the regime inconsistent.
    pub z_threshold: f64,
    pub seed: u64,
}

impl Default for TailCheckSettings {
    fn default() -> Self {
        Self { min_posterior: 0.9, bootstrap_replicates: 200, block_length: 50, model_replicates: 100, z_threshold: 3.0, seed: 1 }
    }
}

/// The verdict for one regime.
#[derive(Debug, Clone, PartialEq)]
pub struct RegimeTailCheck {
    pub regime: usize,
    /// The model's fitted degrees of freedom.
    pub model_dof: f64,
    /// Number of steps assigned to the regime.
    pub n: usize,
    /// Bias-corrected tail index of the data's radii (1/gamma is a biased dof for thin tails).
    pub data_gamma: f64,
    pub data_gamma_sd: f64,
    /// The same estimator on model-simulated radii: mean and spread.
    pub model_gamma_mean: f64,
    pub model_gamma_sd: f64,
    /// (data - model mean) / combined spread.
    pub z: f64,
    /// Observed / expected fraction of radii above the model's 99% and 99.9% quantiles.
    pub exceedance_ratio_99: f64,
    pub exceedance_ratio_999: f64,
    pub consistent: bool,
}

fn radius_of<const D: usize, const K: usize>(model: &ContinuousTimeStudentT<D, K>, k: usize, values: &[f64; D], present: &[bool; D]) -> Option<f64> {
    let mut delta = 0.0;
    let mut any = false;
    for j in (0..D).filter(|&j| present[j]) {
        let diff = values[j] - model.means[k][j];
        delta += model.feature_weights[j] * diff * diff / model.covars[k][j].max(MIN_COVAR);
        any = true;
    }
    any.then(|| delta.sqrt())
}

/// One radius drawn from regime k's emission with a given present-input mask.
fn simulated_radius<const D: usize, const K: usize>(model: &ContinuousTimeStudentT<D, K>, k: usize, present: &[bool; D], rng: &mut SplitMix64) -> Option<f64> {
    let covars = model.covars[k].map(|v| v.max(MIN_COVAR));
    let x = student_t_draw(&model.means[k], &covars, model.nu(k), rng);
    radius_of(model, k, &x, present)
}

fn mean_sd(xs: &[f64]) -> (f64, f64) {
    let n = xs.len() as f64;
    let mean = xs.iter().sum::<f64>() / n;
    let var = xs.iter().map(|x| (x - mean).powi(2)).sum::<f64>() / (n - 1.0).max(1.0);
    (mean, var.sqrt())
}

/// Per-regime tail check: steps (time order) with their regime posteriors (e.g. smoothed, from the
/// fit). Regimes with too few confidently assigned steps are skipped.
pub fn check_regime_tails<const D: usize, const K: usize>(
    model: &ContinuousTimeStudentT<D, K>,
    steps: &[Step<D>],
    posteriors: &[[f64; K]],
    settings: TailCheckSettings,
) -> Vec<RegimeTailCheck> {
    let mut rng = SplitMix64::new(settings.seed);
    let mut out = Vec::new();
    for k in 0..K {
        let assigned: Vec<(f64, [bool; D])> = steps
            .iter()
            .zip(posteriors)
            .filter(|(_, p)| p[k] >= settings.min_posterior)
            .filter_map(|(s, _)| radius_of(model, k, &s.values, &s.present).map(|r| (r, s.present)))
            .collect();
        let radii: Vec<f64> = assigned.iter().map(|&(r, _)| r).collect();
        let Some(estimate) = tail_estimate(&radii, None) else { continue };

        // Data spread: moving-block bootstrap.
        let block = settings.block_length.clamp(1, radii.len());
        let mut boot = Vec::with_capacity(settings.bootstrap_replicates);
        let mut sample = Vec::with_capacity(radii.len());
        for _ in 0..settings.bootstrap_replicates {
            sample.clear();
            while sample.len() < radii.len() {
                let start = rng.next_bounded((radii.len() - block + 1) as u64) as usize;
                sample.extend_from_slice(&radii[start..start + block]);
            }
            sample.truncate(radii.len());
            if let Some(e) = tail_estimate(&sample, None) {
                boot.push(e.corrected_gamma);
            }
        }
        // Model reference: same masks, radii simulated from regime k.
        let mut reference = Vec::with_capacity(settings.model_replicates);
        let mut pooled = Vec::new();
        for _ in 0..settings.model_replicates {
            let sim: Vec<f64> = assigned.iter().filter_map(|(_, mask)| simulated_radius(model, k, mask, &mut rng)).collect();
            if let Some(e) = tail_estimate(&sim, None) {
                reference.push(e.corrected_gamma);
            }
            if pooled.len() < 2_000_000 {
                pooled.extend_from_slice(&sim);
            }
        }
        if boot.len() < 10 || reference.len() < 10 {
            continue;
        }
        let (_, data_sd) = mean_sd(&boot);
        let (model_mean, model_sd) = mean_sd(&reference);
        let z = (estimate.corrected_gamma - model_mean) / (data_sd * data_sd + model_sd * model_sd).sqrt();
        pooled.sort_by(f64::total_cmp);
        let quantile = |p: f64| pooled[((pooled.len() - 1) as f64 * p).round() as usize];
        let (q99, q999) = (quantile(0.99), quantile(0.999));
        let n = radii.len() as f64;
        let above = |q: f64| radii.iter().filter(|&&r| r > q).count() as f64 / n;
        out.push(RegimeTailCheck {
            regime: k,
            model_dof: model.nu(k),
            n: radii.len(),
            data_gamma: estimate.corrected_gamma,
            data_gamma_sd: data_sd,
            model_gamma_mean: model_mean,
            model_gamma_sd: model_sd,
            z,
            exceedance_ratio_99: above(q99) / 0.01,
            exceedance_ratio_999: above(q999) / 0.001,
            consistent: z.abs() <= settings.z_threshold,
        });
    }
    out
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::regime_path_simulator::{Generator, poisson_times, simulate_emissions, simulate_path};

    fn student_t_abs(nu: f64, n: usize, seed: u64) -> Vec<f64> {
        let mut rng = SplitMix64::new(seed);
        (0..n).map(|_| student_t_draw(&[0.0], &[1.0], nu, &mut rng)[0].abs()).collect()
    }

    /// Exact Pareto: Hill is unbiased, so it must hit gamma within sampling error at any k.
    #[test]
    fn hill_recovers_an_exact_pareto_tail() {
        let mut rng = SplitMix64::new(3);
        let gamma = 0.5;
        let xs: Vec<f64> = (0..100_000).map(|_| (1.0 - rng.next_f64()).powf(-gamma)).collect();
        let sorted = descending(&xs);
        for k in [500, 2_000, 10_000] {
            let h = hill(&sorted, k);
            assert!((h - gamma).abs() < 4.0 * gamma / (k as f64).sqrt(), "k={k}: {h}");
        }
    }

    /// Student-t tails have second-order parameter rho = -2/nu (Hall class).
    #[test]
    fn rho_matches_the_student_t_second_order_parameter() {
        // Only the heavy-tailed case: rho estimation converges very slowly as rho -> 0 (thinner
        // tails), which is why the regime check compares against model-simulated data.
        let nu = 3.0;
        let xs = student_t_abs(nu, 400_000, 11);
        let sorted = descending(&xs);
        let so = second_order_at(&sorted, level_k1(sorted.len())).unwrap();
        assert!((so.rho - (-2.0 / nu)).abs() < 0.15, "rho {}", so.rho);
    }

    /// The correction removes the bias plain Hill shows on Student-t data at a large k.
    #[test]
    fn corrected_hill_beats_hill_on_student_t() {
        for (nu, seed) in [(3.0, 21), (4.0, 22), (6.0, 23)] {
            let xs = student_t_abs(nu, 200_000, seed);
            let e = tail_estimate(&xs, Some(10_000)).unwrap();
            let truth = 1.0 / nu;
            let (err_h, err_c) = ((e.hill_gamma - truth).abs(), (e.corrected_gamma - truth).abs());
            assert!(err_c < err_h, "nu={nu}: corrected {} vs hill {} (truth {truth})", e.corrected_gamma, e.hill_gamma);
        }
        // At its own level, the corrected estimator is within sampling error (sd gamma/sqrt(k),
        // the paper's asymptotic variance) for heavy tails.
        for (nu, seed) in [(3.0, 24), (4.0, 25)] {
            let e = tail_estimate(&student_t_abs(nu, 200_000, seed), None).unwrap();
            let truth = 1.0 / nu;
            assert!((e.corrected_gamma - truth).abs() < 4.0 * truth / (e.k as f64).sqrt(), "nu={nu}: {} at k={}", e.corrected_gamma, e.k);
        }
    }

    /// The radius of a multivariate t with any number of dimensions has tail index nu.
    #[test]
    fn multivariate_radius_has_tail_index_nu() {
        let mut rng = SplitMix64::new(31);
        let nu = 4.0;
        let radii: Vec<f64> = (0..200_000)
            .map(|_| {
                let x = student_t_draw(&[0.0, 0.0, 0.0], &[1.0, 4.0, 0.25], nu, &mut rng);
                (x[0] * x[0] + x[1] * x[1] / 4.0 + x[2] * x[2] / 0.25).sqrt()
            })
            .collect();
        let e = tail_estimate(&radii, None).unwrap();
        assert!((1.0 / e.corrected_gamma - nu).abs() < 0.6, "dof {}", 1.0 / e.corrected_gamma);
    }

    fn two_regime_model(dof: [f64; 2]) -> ContinuousTimeStudentT<2, 2> {
        let generator = Generator::new([[-0.002, 0.002], [0.004, -0.004]]).unwrap();
        let initial = generator.stationary_distribution().unwrap();
        ContinuousTimeStudentT {
            generator,
            initial,
            means: [[-6.0, 6.0], [6.0, -6.0]],
            covars: [[1.0, 1.0], [1.0, 1.0]],
            dof,
            dof_min: 2.1,
            dof_max: 100.0,
            feature_weights: [1.0, 1.0],
        }
    }

    fn simulate(truth: &ContinuousTimeStudentT<2, 2>, horizon: f64, seed: u64) -> Vec<Step<2>> {
        let mut rng = SplitMix64::new(seed);
        let path = simulate_path(&truth.generator, &truth.initial, horizon, &mut rng);
        let times = poisson_times(1.0, horizon, &mut rng);
        let states = path.sample_at(&times);
        let xs = simulate_emissions(&states, &truth.means, &truth.covars, &truth.dof, &mut rng);
        let mut prev = 0.0;
        times
            .iter()
            .zip(xs)
            .map(|(&t, x)| {
                let s = Step { elapsed: t - prev, values: x, present: [true, true] };
                prev = t;
                s
            })
            .collect()
    }

    /// A correct model passes; the same data judged by a model claiming thin tails fails for the
    /// heavy-tailed regime.
    #[test]
    fn passes_a_correct_model_and_flags_a_thin_tailed_one() {
        let truth = two_regime_model([3.0, 8.0]);
        let steps = simulate(&truth, 150_000.0, 41);
        let posteriors = truth.filter(&steps).unwrap().posteriors;
        let settings = TailCheckSettings { bootstrap_replicates: 100, model_replicates: 60, ..TailCheckSettings::default() };
        let checks = check_regime_tails(&truth, &steps, &posteriors, settings);
        assert_eq!(checks.len(), 2);
        for c in &checks {
            assert!(c.consistent, "regime {}: z {} ({c:?})", c.regime, c.z);
            assert!((c.exceedance_ratio_99 - 1.0).abs() < 0.2, "regime {}: 99% ratio {}", c.regime, c.exceedance_ratio_99);
        }
        // Same data, a model that claims nu = 30 everywhere (near-Gaussian tails).
        let wrong = two_regime_model([30.0, 30.0]);
        let checks = check_regime_tails(&wrong, &steps, &posteriors, settings);
        let heavy = checks.iter().find(|c| c.regime == 0).unwrap();
        assert!(!heavy.consistent, "heavy regime not flagged: z {}", heavy.z);
        assert!(heavy.exceedance_ratio_999 > 3.0, "99.9% ratio {}", heavy.exceedance_ratio_999);
    }
}
