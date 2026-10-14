//! continuous_time_student_t.rs -- the continuous-time Student-t regime model (rust-migration spec
//! §6.2.8 D4 rulings): regimes switch by a generator Q in continuous time; each step carries its
//! elapsed time d and the inputs observed at it; an input with no new value at a step is absent
//! and leaves that step's likelihood (emission factor over the observed inputs only).
//!
//! Emissions: the diagonal-scale, feature-weighted multivariate Student-t of `model.rs`, one dof
//! per regime. Any subset of a multivariate t is multivariate t with the same dof (the shared
//! chi-square factor), so the density over the observed inputs is exact: the observed dimension
//! count D_obs, the observed Mahalanobis term and the observed log-scales and weights. With every
//! input observed it equals `StudentTHmm::log_emission_prob` (tested). With none observed it is 0
//! (factor 1) -- the step only lets time pass. This is the rule in `msm` (`lik.c`
//! `GetOutcomeProb`), in LaMa's worked example and in Zucchini, MacDonald & Langrock (2016).
//!
//! Filtering: scaled forward recursion, alpha_t = alpha_{t-1} P(d_t) diag(b(x_t)), normalized each
//! step with the log of the normalizer added to the log-likelihood; emissions are shifted by their
//! maximum before exponentiating so no regime underflows.
//!
//! Training (EM): the E-step is a scaled forward-backward giving the regime posteriors and the
//! two-slice weights xi_t / P(d_t); the switching-rate statistics (expected time in each regime and
//! expected jumps) come from `elapsed_time_transitions::StepStatistics` and give the closed-form
//! update q_kl = jumps_kl / occupancy_k (Liu et al. 2015). Emission updates are the Student-t
//! mixture updates with the latent scale weight u = (nu + D_obs) / (nu + delta_obs) over the
//! observed inputs; each regime's dof maximizes the observed-data likelihood given the other
//! parameters (ECME, Liu & Rubin 1995) by golden-section search on [dof_min, dof_max]. The initial
//! distribution is the stationary distribution of Q (one long sequence does not identify a
//! separate initial distribution).

use crate::elapsed_time_transitions::{ElapsedTimeError, StepStatistics, transition_matrix};
use crate::model::{MIN_COVAR, ln_gamma};
use crate::regime_path_simulator::{Generator, GeneratorError};
use std::f64::consts::PI;

/// One step: elapsed time since the previous step, the input values, and which inputs carry a new
/// value at this step. Values of absent inputs are ignored.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Step<const D: usize> {
    pub elapsed: f64,
    pub values: [f64; D],
    pub present: [bool; D],
}

impl<const D: usize> Step<D> {
    pub fn all_present(elapsed: f64, values: [f64; D]) -> Self {
        Self { elapsed, values, present: [true; D] }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum FitError {
    ElapsedTime(ElapsedTimeError),
    Generator(GeneratorError),
    /// A regime received no posterior weight, so its parameters cannot be updated.
    EmptyRegime(usize),
    /// The generator has no unique stationary distribution.
    NoStationaryDistribution,
}

impl std::fmt::Display for FitError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::ElapsedTime(e) => write!(f, "{e}"),
            Self::Generator(e) => write!(f, "{e}"),
            Self::EmptyRegime(k) => write!(f, "regime {k} received no posterior weight"),
            Self::NoStationaryDistribution => write!(f, "generator has no unique stationary distribution"),
        }
    }
}

impl std::error::Error for FitError {}

impl From<ElapsedTimeError> for FitError {
    fn from(e: ElapsedTimeError) -> Self {
        Self::ElapsedTime(e)
    }
}

impl From<GeneratorError> for FitError {
    fn from(e: GeneratorError) -> Self {
        Self::Generator(e)
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct ContinuousTimeStudentT<const D: usize, const K: usize> {
    pub generator: Generator<K>,
    /// Regime probabilities at the first step.
    pub initial: [f64; K],
    pub means: [[f64; D]; K],
    /// Diagonal scales (variances of the Gaussian part), floored at `MIN_COVAR` when read.
    pub covars: [[f64; D]; K],
    pub dof: [f64; K],
    pub dof_min: f64,
    pub dof_max: f64,
    /// Fixed per-input weights on the Mahalanobis term (1.0 = unweighted).
    pub feature_weights: [f64; D],
}

/// Per-step observed Mahalanobis term and observed dimension count for one regime.
fn observed_delta<const D: usize>(mean: &[f64; D], var: &[f64; D], weights: &[f64; D], step: &Step<D>) -> (f64, usize) {
    let mut delta = 0.0;
    let mut n_obs = 0;
    for j in 0..D {
        if step.present[j] {
            let diff = step.values[j] - mean[j];
            delta += weights[j] * diff * diff / var[j].max(MIN_COVAR);
            n_obs += 1;
        }
    }
    (delta, n_obs)
}

/// Log-density of the observed inputs under one regime's multivariate t, given the observed
/// Mahalanobis term `delta` and count `n_obs` (the per-input constant terms are summed here).
fn log_t_density<const D: usize>(nu: f64, delta: f64, n_obs: usize, var: &[f64; D], weights: &[f64; D], present: &[bool; D]) -> f64 {
    if n_obs == 0 {
        return 0.0;
    }
    let d = n_obs as f64;
    let mut log_det = 0.0;
    let mut log_w = 0.0;
    for j in 0..D {
        if present[j] {
            log_det += var[j].max(MIN_COVAR).ln();
            log_w += weights[j].max(1e-300).ln();
        }
    }
    ln_gamma((nu + d) * 0.5) - ln_gamma(nu * 0.5) - 0.5 * (d * (nu * PI).ln() + log_det) + 0.5 * log_w
        - 0.5 * (nu + d) * (delta / nu).ln_1p()
}

/// Result of filtering a sequence.
#[derive(Debug, Clone, PartialEq)]
pub struct FilterResult<const K: usize> {
    /// P(regime at step t | steps up to t).
    pub posteriors: Vec<[f64; K]>,
    pub log_likelihood: f64,
}

impl<const D: usize, const K: usize> ContinuousTimeStudentT<D, K> {
    pub(crate) fn nu(&self, k: usize) -> f64 {
        self.dof[k].clamp(self.dof_min, self.dof_max)
    }

    /// Log-density of the inputs observed at `step` under regime `k` (0 when none is observed).
    pub fn log_emission(&self, k: usize, step: &Step<D>) -> f64 {
        let (delta, n_obs) = observed_delta(&self.means[k], &self.covars[k], &self.feature_weights, step);
        log_t_density(self.nu(k), delta, n_obs, &self.covars[k], &self.feature_weights, &step.present)
    }

    /// Emission factors shifted by their maximum: returns (b_k / max_k b_k, ln max_k b_k).
    pub(crate) fn scaled_emissions(&self, step: &Step<D>) -> ([f64; K], f64) {
        let mut log_b = [0.0; K];
        for (k, lb) in log_b.iter_mut().enumerate() {
            *lb = self.log_emission(k, step);
        }
        let max = log_b.iter().copied().fold(f64::NEG_INFINITY, f64::max);
        (log_b.map(|lb| (lb - max).exp()), max)
    }

    /// Regime probabilities after each step and the exact log-likelihood of the sequence.
    pub fn filter(&self, steps: &[Step<D>]) -> Result<FilterResult<K>, ElapsedTimeError> {
        let mut posteriors = Vec::with_capacity(steps.len());
        let mut log_likelihood = 0.0;
        let mut alpha = self.initial;
        for (t, step) in steps.iter().enumerate() {
            let prior = if t == 0 { self.initial } else { row_times_matrix(&alpha, &transition_matrix(&self.generator, step.elapsed)?) };
            let (b, shift) = self.scaled_emissions(step);
            let mut unnorm = [0.0; K];
            for k in 0..K {
                unnorm[k] = prior[k] * b[k];
            }
            let norm: f64 = unnorm.iter().sum();
            log_likelihood += norm.ln() + shift;
            alpha = unnorm.map(|v| v / norm);
            posteriors.push(alpha);
        }
        Ok(FilterResult { posteriors, log_likelihood })
    }
}

fn row_times_matrix<const K: usize>(row: &[f64; K], m: &[[f64; K]; K]) -> [f64; K] {
    let mut out = [0.0; K];
    for (&r_i, m_row) in row.iter().zip(m) {
        for (o_j, &m_ij) in out.iter_mut().zip(m_row) {
            *o_j += r_i * m_ij;
        }
    }
    out
}

/// Settings for `fit`.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct FitSettings {
    pub max_iterations: usize,
    /// Stop when the log-likelihood improves by less than this (absolute).
    pub tolerance: f64,
    /// Estimate the dof (ECME step); when false the dof stay fixed.
    pub estimate_dof: bool,
}

impl Default for FitSettings {
    fn default() -> Self {
        Self { max_iterations: 500, tolerance: 1e-6, estimate_dof: true }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct FitReport {
    /// Log-likelihood after each E-step (the model at the start of each iteration).
    pub log_likelihoods: Vec<f64>,
    pub converged: bool,
}

/// Golden-section maximization of a unimodal function on [lo, hi].
fn golden_max(f: impl Fn(f64) -> f64, mut lo: f64, mut hi: f64) -> f64 {
    let ratio = (5f64.sqrt() - 1.0) / 2.0;
    let mut x1 = hi - ratio * (hi - lo);
    let mut x2 = lo + ratio * (hi - lo);
    let (mut f1, mut f2) = (f(x1), f(x2));
    for _ in 0..80 {
        if (hi - lo) < 1e-6 * (1.0 + lo.abs()) {
            break;
        }
        if f1 < f2 {
            lo = x1;
            x1 = x2;
            f1 = f2;
            x2 = lo + ratio * (hi - lo);
            f2 = f(x2);
        } else {
            hi = x2;
            x2 = x1;
            f2 = f1;
            x1 = hi - ratio * (hi - lo);
            f1 = f(x1);
        }
    }
    0.5 * (lo + hi)
}

/// One sequence's E-step: regime posteriors gamma_t, switching-rate statistics (added to
/// `stats`), and the log-likelihood.
fn e_step<const D: usize, const K: usize>(
    model: &ContinuousTimeStudentT<D, K>,
    steps: &[Step<D>],
    stats: &mut StepStatistics<K>,
) -> Result<(Vec<[f64; K]>, f64), FitError> {
    let n = steps.len();
    let mut transitions = Vec::with_capacity(n);
    let mut emissions = Vec::with_capacity(n);
    for (t, step) in steps.iter().enumerate() {
        transitions.push(if t == 0 { None } else { Some(transition_matrix(&model.generator, step.elapsed)?) });
        emissions.push(model.scaled_emissions(step));
    }
    // Forward, scaled: alpha_t sums to 1; c_t = normalizer.
    let mut alpha = vec![[0.0; K]; n];
    let mut scale = vec![0.0; n];
    let mut log_likelihood = 0.0;
    for t in 0..n {
        let prior = match &transitions[t] {
            None => model.initial,
            Some(p) => row_times_matrix(&alpha[t - 1], p),
        };
        let (b, shift) = emissions[t];
        let mut unnorm = [0.0; K];
        for k in 0..K {
            unnorm[k] = prior[k] * b[k];
        }
        let c: f64 = unnorm.iter().sum();
        scale[t] = c;
        log_likelihood += c.ln() + shift;
        alpha[t] = unnorm.map(|v| v / c);
    }
    // Backward, with the same scaling: beta_T = 1; beta_t = P (b * beta_{t+1}) / c_{t+1}.
    let mut beta = vec![[1.0; K]; n];
    for t in (0..n.saturating_sub(1)).rev() {
        let p = transitions[t + 1].as_ref().ok_or(FitError::NoStationaryDistribution)?;
        let (b, _) = emissions[t + 1];
        let mut out = [0.0; K];
        for (i, out_i) in out.iter_mut().enumerate() {
            let mut s = 0.0;
            for j in 0..K {
                s += p[i][j] * b[j] * beta[t + 1][j];
            }
            *out_i = s / scale[t + 1];
        }
        beta[t] = out;
        // Two-slice weights divided by P: w_ij = alpha_t(i) b_j beta_{t+1}(j) / c_{t+1}.
        let mut w = [[0.0; K]; K];
        for (i, w_row) in w.iter_mut().enumerate() {
            for (j, w_ij) in w_row.iter_mut().enumerate() {
                *w_ij = alpha[t][i] * b[j] * beta[t + 1][j] / scale[t + 1];
            }
        }
        stats.accumulate(steps[t + 1].elapsed, &w)?;
    }
    let gamma = alpha
        .iter()
        .zip(&beta)
        .map(|(a, b)| {
            let mut g = [0.0; K];
            for k in 0..K {
                g[k] = a[k] * b[k];
            }
            let s: f64 = g.iter().sum();
            g.map(|v| v / s)
        })
        .collect();
    Ok((gamma, log_likelihood))
}

/// Emission M-step shared by every model with Student-t observations: Student-t mixture updates
/// over the observed inputs (latent scale weight u = (nu + D_obs) / (nu + delta_obs)), then, if
/// `estimate_dof`, each regime's dof maximizing the observed-data likelihood given the new means and
/// scales (ECME). `pairs` yields (step, regime posterior) and is called once per pass.
pub(crate) fn update_emissions<'a, const D: usize, const K: usize, I>(
    model: &ContinuousTimeStudentT<D, K>,
    pairs: impl Fn() -> I,
    estimate_dof: bool,
) -> ([[f64; D]; K], [[f64; D]; K], [f64; K])
where
    I: Iterator<Item = (&'a Step<D>, &'a [f64; K])>,
{
    let mut means = model.means;
    let mut covars = model.covars;
    for k in 0..K {
        let nu = model.nu(k);
        let mut sum_gu = [0.0; D];
        let mut sum_gux = [0.0; D];
        for (step, g) in pairs() {
            let (delta, n_obs) = observed_delta(&model.means[k], &model.covars[k], &model.feature_weights, step);
            if n_obs == 0 {
                continue;
            }
            let u = (nu + n_obs as f64) / (nu + delta);
            for j in (0..D).filter(|&j| step.present[j]) {
                sum_gu[j] += g[k] * u;
                sum_gux[j] += g[k] * u * step.values[j];
            }
        }
        for j in 0..D {
            if sum_gu[j] > 0.0 {
                means[k][j] = sum_gux[j] / sum_gu[j];
            }
        }
        let mut sum_g = [0.0; D];
        let mut sum_gu_dev2 = [0.0; D];
        for (step, g) in pairs() {
            let (delta, n_obs) = observed_delta(&model.means[k], &model.covars[k], &model.feature_weights, step);
            if n_obs == 0 {
                continue;
            }
            let u = (nu + n_obs as f64) / (nu + delta);
            for j in (0..D).filter(|&j| step.present[j]) {
                let dev = step.values[j] - means[k][j];
                sum_g[j] += g[k];
                sum_gu_dev2[j] += g[k] * u * dev * dev;
            }
        }
        for j in 0..D {
            if sum_g[j] > 0.0 {
                covars[k][j] = (sum_gu_dev2[j] / sum_g[j]).max(MIN_COVAR);
            }
        }
    }
    let mut dof = model.dof;
    if estimate_dof {
        for k in 0..K {
            let cached: Vec<(f64, f64, usize, [bool; D])> = pairs()
                .filter_map(|(step, g)| {
                    let (delta, n_obs) = observed_delta(&means[k], &covars[k], &model.feature_weights, step);
                    (n_obs > 0 && g[k] > 0.0).then_some((g[k], delta, n_obs, step.present))
                })
                .collect();
            let objective = |nu: f64| {
                cached.iter().map(|&(g, delta, n_obs, present)| g * log_t_density(nu, delta, n_obs, &covars[k], &model.feature_weights, &present)).sum::<f64>()
            };
            dof[k] = golden_max(objective, model.dof_min, model.dof_max);
        }
    }
    (means, covars, dof)
}

/// Fits the model to one or more sequences by EM, starting from `model`. Returns the fitted model
/// and the log-likelihood path (non-decreasing up to rounding, tested).
pub fn fit<const D: usize, const K: usize>(
    start: &ContinuousTimeStudentT<D, K>,
    sequences: &[Vec<Step<D>>],
    settings: FitSettings,
) -> Result<(ContinuousTimeStudentT<D, K>, FitReport), FitError> {
    let mut model = start.clone();
    let mut log_likelihoods = Vec::new();
    let mut converged = false;
    for _ in 0..settings.max_iterations {
        // ---- E-step ----
        let mut stats = StepStatistics::new(&model.generator);
        let mut gammas = Vec::with_capacity(sequences.len());
        let mut total_ll = 0.0;
        for seq in sequences {
            let (gamma, ll) = e_step(&model, seq, &mut stats)?;
            gammas.push(gamma);
            total_ll += ll;
        }
        if let Some(&prev) = log_likelihoods.last()
            && total_ll - prev < settings.tolerance
        {
            log_likelihoods.push(total_ll);
            converged = true;
            break;
        }
        log_likelihoods.push(total_ll);

        // ---- M-step: switching rates ----
        let mut q = [[0.0; K]; K];
        for (k, q_row) in q.iter_mut().enumerate() {
            let occupancy = stats.occupancy[k];
            if occupancy <= 0.0 {
                return Err(FitError::EmptyRegime(k));
            }
            let mut exit = 0.0;
            for l in (0..K).filter(|&l| l != k) {
                q_row[l] = stats.jumps[k][l] / occupancy;
                exit += q_row[l];
            }
            q_row[k] = -exit;
        }
        let generator = Generator::new(q)?;

        // ---- M-step: emissions; ECME dof ----
        let (means, covars, dof) =
            update_emissions(&model, || sequences.iter().zip(&gammas).flat_map(|(seq, gamma)| seq.iter().zip(gamma)), settings.estimate_dof);

        let initial = generator.stationary_distribution().ok_or(FitError::NoStationaryDistribution)?;
        model = ContinuousTimeStudentT { generator, initial, means, covars, dof, ..model };
    }
    Ok((model, FitReport { log_likelihoods, converged }))
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::init::SplitMix64;
    use crate::model::StudentTHmm;
    use crate::regime_path_simulator::{poisson_times, simulate_emissions, simulate_path};

    fn two_regime_model() -> ContinuousTimeStudentT<2, 2> {
        ContinuousTimeStudentT {
            generator: Generator::new([[-0.3, 0.3], [0.6, -0.6]]).unwrap(),
            initial: [0.5, 0.5],
            means: [[0.0, 1.0], [2.0, -1.0]],
            covars: [[1.0, 0.5], [2.0, 1.5]],
            dof: [5.0, 9.0],
            dof_min: 2.1,
            dof_max: 100.0,
            feature_weights: [1.0, 1.4],
        }
    }

    #[test]
    fn full_observation_matches_the_discrete_model_density() {
        let m = two_regime_model();
        let dm = StudentTHmm::new(m.means, m.covars, [[0.9, 0.1], [0.2, 0.8]], [0.5, 0.5], m.dof, m.dof_min, m.dof_max, m.feature_weights);
        for x in [[0.3, -0.2], [5.0, 4.0], [-3.0, 0.1]] {
            let step = Step::all_present(1.0, x);
            for k in 0..2 {
                assert!((m.log_emission(k, &step) - dm.log_emission_prob(k, &x)).abs() < 1e-12);
            }
        }
        assert!(m.log_emission(0, &Step { elapsed: 1.0, values: [9.9, 9.9], present: [false, false] }).abs() < 1e-15);
    }

    /// The density of the observed input equals the full density integrated over the absent one
    /// (the subset property, checked by quadrature).
    #[test]
    fn absent_input_density_is_the_marginal() {
        let mut m = two_regime_model();
        m.feature_weights = [1.0, 1.0];
        let x0 = 0.7;
        let marginal = m.log_emission(1, &Step { elapsed: 1.0, values: [x0, 0.0], present: [true, false] }).exp();
        // Integrate the full 2-D density over x1 with a wide Simpson grid (t tails decay as |x|^-(nu+2)).
        let (lo, hi, n) = (-400.0, 400.0, 400_000usize);
        let h = (hi - lo) / n as f64;
        let integral: f64 = (0..=n)
            .map(|i| {
                let x1 = lo + i as f64 * h;
                let w = if i == 0 || i == n { 1.0 } else if i % 2 == 1 { 4.0 } else { 2.0 };
                w * m.log_emission(1, &Step::all_present(1.0, [x0, x1])).exp()
            })
            .sum::<f64>()
            * h
            / 3.0;
        assert!((integral / marginal - 1.0).abs() < 1e-6, "{integral} vs {marginal}");
    }

    /// With every step one time unit apart, the continuous-time filter equals the discrete filter
    /// whose transition matrix is P(1).
    #[test]
    fn equal_steps_match_the_discrete_filter() {
        let m = two_regime_model();
        let p1 = transition_matrix(&m.generator, 1.0).unwrap();
        let dm = StudentTHmm::new(m.means, m.covars, p1, m.initial, m.dof, m.dof_min, m.dof_max, m.feature_weights);
        let xs = [[0.1, 0.9], [2.2, -1.1], [1.9, -0.8], [0.0, 1.2], [3.0, 3.0]];
        let steps: Vec<Step<2>> = xs.iter().map(|&x| Step::all_present(1.0, x)).collect();
        let ct = m.filter(&steps).unwrap();
        // Discrete filter: first step is the prior updated by the emission, then predict+update.
        let mut alpha = m.initial;
        for (t, x) in xs.iter().enumerate() {
            alpha = if t == 0 {
                let b: Vec<f64> = (0..2).map(|k| dm.log_emission_prob(k, x).exp()).collect();
                let u = [alpha[0] * b[0], alpha[1] * b[1]];
                let s = u[0] + u[1];
                [u[0] / s, u[1] / s]
            } else {
                dm.forward_step(&alpha, x)
            };
            assert!(ct.posteriors[t].iter().zip(&alpha).all(|(c, d)| (c - d).abs() < 1e-12), "step {t}");
        }
    }

    /// The log-likelihood equals brute-force summation over every regime path (2^6 paths), with
    /// unequal steps and an absent input.
    #[test]
    fn log_likelihood_matches_brute_force_over_all_paths() {
        let m = two_regime_model();
        let steps = [
            Step::all_present(0.0, [0.2, 0.8]),
            Step::all_present(0.4, [2.1, -0.9]),
            Step { elapsed: 3.0, values: [1.8, 0.0], present: [true, false] },
            Step::all_present(0.05, [0.3, 1.1]),
            Step { elapsed: 1.2, values: [0.0, 0.0], present: [false, false] },
            Step::all_present(7.0, [2.5, -1.5]),
        ];
        let n = steps.len();
        let mut total = 0.0;
        for path in 0..(1u32 << n) {
            let s = |t: usize| ((path >> t) & 1) as usize;
            let mut p = m.initial[s(0)] * m.log_emission(s(0), &steps[0]).exp();
            for (t, step) in steps.iter().enumerate().skip(1) {
                p *= transition_matrix(&m.generator, step.elapsed).unwrap()[s(t - 1)][s(t)] * m.log_emission(s(t), step).exp();
            }
            total += p;
        }
        let ll = m.filter(&steps).unwrap().log_likelihood;
        assert!((ll - total.ln()).abs() < 1e-11, "{ll} vs {}", total.ln());
    }

    /// Simulate from a known 3-regime model with irregular steps and absent inputs.
    fn simulate_dataset(truth: &ContinuousTimeStudentT<2, 3>, horizon: f64, seed: u64) -> Vec<Step<2>> {
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
                let present = [rng.next_f64() > 0.2, rng.next_f64() > 0.2];
                let step = Step { elapsed: t - prev, values: x, present };
                prev = t;
                step
            })
            .collect()
    }

    fn truth() -> ContinuousTimeStudentT<2, 3> {
        let generator = Generator::new([[-0.02, 0.015, 0.005], [0.01, -0.03, 0.02], [0.03, 0.01, -0.04]]).unwrap();
        let initial = generator.stationary_distribution().unwrap();
        ContinuousTimeStudentT {
            generator,
            initial,
            means: [[-3.0, 0.0], [0.0, 3.0], [3.0, -3.0]],
            covars: [[1.0, 1.0], [0.5, 2.0], [2.0, 0.5]],
            dof: [4.0, 8.0, 5.0],
            dof_min: 2.1,
            dof_max: 100.0,
            feature_weights: [1.0, 1.0],
        }
    }

    fn perturbed_start(t: &ContinuousTimeStudentT<2, 3>) -> ContinuousTimeStudentT<2, 3> {
        let generator = Generator::new([[-0.04, 0.02, 0.02], [0.02, -0.04, 0.02], [0.02, 0.02, -0.04]]).unwrap();
        ContinuousTimeStudentT {
            generator,
            initial: [1.0 / 3.0; 3],
            means: [[-2.0, 0.5], [0.5, 2.0], [2.0, -2.0]],
            covars: [[2.0, 2.0]; 3],
            dof: [10.0; 3],
            ..t.clone()
        }
    }

    #[test]
    fn em_never_decreases_the_likelihood() {
        let t = truth();
        let data = vec![simulate_dataset(&t, 5_000.0, 101)];
        let (_, report) = fit(&perturbed_start(&t), &data, FitSettings { max_iterations: 25, tolerance: 0.0, estimate_dof: true }).unwrap();
        for w in report.log_likelihoods.windows(2) {
            assert!(w[1] >= w[0] - 1e-6 * w[0].abs().max(1.0), "decrease: {} -> {}", w[0], w[1]);
        }
    }

    /// The known-answer test: simulate ~60,000 irregular steps with 20% of inputs absent, fit from
    /// a perturbed start, recover the switching rates, means, scales and dof.
    #[test]
    fn recovers_known_parameters_from_simulated_data() {
        let t = truth();
        let data = vec![simulate_dataset(&t, 60_000.0, 202)];
        let (fitted, report) = fit(&perturbed_start(&t), &data, FitSettings { max_iterations: 300, tolerance: 1e-7, estimate_dof: true }).unwrap();
        assert!(report.converged, "did not converge in {} iterations", report.log_likelihoods.len());
        let q_true = t.generator.matrix();
        let q_fit = fitted.generator.matrix();
        for i in 0..3 {
            for j in (0..3).filter(|&j| j != i) {
                let rel = (q_fit[i][j] - q_true[i][j]).abs() / q_true[i][j];
                assert!(rel < 0.25, "q[{i}][{j}] = {} vs {}", q_fit[i][j], q_true[i][j]);
            }
            for d in 0..2 {
                assert!((fitted.means[i][d] - t.means[i][d]).abs() < 0.08, "mean[{i}][{d}] {}", fitted.means[i][d]);
                assert!((fitted.covars[i][d] / t.covars[i][d] - 1.0).abs() < 0.12, "covar[{i}][{d}] {}", fitted.covars[i][d]);
            }
            assert!((fitted.dof[i] - t.dof[i]).abs() < 0.35 * t.dof[i], "dof[{i}] {} vs {}", fitted.dof[i], t.dof[i]);
        }
    }
}
