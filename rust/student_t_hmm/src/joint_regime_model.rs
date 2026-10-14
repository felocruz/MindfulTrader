//! joint_regime_model.rs -- the regime model reading BOTH the trades and the observation steps
//! (rust-migration spec §6.2.8 D4): one hidden continuous-time regime drives
//!   * the trade arrival rate, mu_k + g(t) -- a regime background rate plus one shared echo of
//!     earlier trades (`regime_modulated_hawkes`), and
//!   * the Student-t inputs read at each observation step (`continuous_time_student_t`).
//!
//! Why: the steps are triggered by trades and volume (`step_times`), so in a busy regime they come
//! faster. Fitting the regime model to the steps while ignoring the trades treats regime-dependent
//! timing as uninformative, which is the biased case (Gruger, Kay & Schumacher 1991; Lange et al.
//! 2015). Here the trades are part of the likelihood, so the timing is evidence, not bias.
//!
//! Data: one time-ordered stream of points (a trade, or an observation step with its inputs and
//! which of them are present) on [0, horizon]. At equal times trades come first.
//! Likelihood:
//!   L = initial * prod_p [ e^{(Q - diag mu) dt_p} F_p ] * e^{(Q - diag mu)(T - t_last)} 1 * e^{-int g}
//! with F_p = diag(mu + g(t_p-)) at a trade and diag(b(x_p)) (Student-t density over the present
//! inputs) at an observation step. With no trades and mu = 0, n = 0 it is exactly the continuous-
//! time Student-t model; with no observation steps it is exactly the regime-modulated Hawkes model
//! (both tested).
//!
//! Training (EM): one scaled forward-backward over all points; expected regime occupancy and
//! switches (`StepStatistics::with_event_rates`); immigrant / offspring responsibilities at trades;
//! regime posteriors at observation steps for the Student-t updates (shared `update_emissions`).

use crate::continuous_time_student_t::{ContinuousTimeStudentT, Step, update_emissions};
use crate::elapsed_time_transitions::{ElapsedTimeError, StepStatistics, no_event_matrix};
use crate::regime_modulated_hawkes::{excitation_before_events, excitation_integral, golden_max};
use crate::regime_path_simulator::{Generator, GeneratorError};

/// The agreed step trigger: a step fires when, since the previous step, `trades` trades OR `volume`
/// contracts have arrived (at the trade that reaches either), or `floor` time has passed with
/// neither (a time-only step at exactly previous + floor).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct StepTrigger {
    pub trades: u32,
    pub volume: u64,
    pub floor: f64,
}

/// Step times on [0, horizon] for ascending trade times and their sizes.
pub fn step_times(trade_times: &[f64], sizes: &[u64], trigger: StepTrigger, horizon: f64) -> Vec<f64> {
    let mut steps = Vec::new();
    let mut last = 0.0;
    let (mut count, mut volume) = (0u32, 0u64);
    for (&t, &size) in trade_times.iter().zip(sizes) {
        while t - last >= trigger.floor {
            last += trigger.floor;
            steps.push(last);
            count = 0;
            volume = 0;
        }
        count += 1;
        volume += size;
        if count >= trigger.trades || volume >= trigger.volume {
            steps.push(t);
            last = t;
            count = 0;
            volume = 0;
        }
    }
    while last + trigger.floor <= horizon {
        last += trigger.floor;
        steps.push(last);
    }
    steps
}

/// One point of the joint stream.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Point<const D: usize> {
    Trade { time: f64 },
    Observation { time: f64, values: [f64; D], present: [bool; D] },
}

impl<const D: usize> Point<D> {
    pub fn time(&self) -> f64 {
        match self {
            Self::Trade { time } | Self::Observation { time, .. } => *time,
        }
    }
}

/// One observation window: time-ordered points in [0, horizon].
#[derive(Debug, Clone, PartialEq)]
pub struct JointWindow<const D: usize> {
    pub points: Vec<Point<D>>,
    pub horizon: f64,
}

impl<const D: usize> JointWindow<D> {
    /// Merges trades and observation steps into one stream (trades first at equal times).
    pub fn merge(trades: &[f64], observations: &[(f64, [f64; D], [bool; D])], horizon: f64) -> Self {
        let mut points: Vec<Point<D>> = trades.iter().map(|&time| Point::Trade { time }).collect();
        points.extend(observations.iter().map(|&(time, values, present)| Point::Observation { time, values, present }));
        points.sort_by(|a, b| {
            a.time().total_cmp(&b.time()).then_with(|| matches!(a, Point::Observation { .. }).cmp(&matches!(b, Point::Observation { .. })))
        });
        Self { points, horizon }
    }

    fn trade_times(&self) -> Vec<f64> {
        self.points.iter().filter_map(|p| if let Point::Trade { time } = p { Some(*time) } else { None }).collect()
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum JointError {
    ElapsedTime(ElapsedTimeError),
    Generator(GeneratorError),
    /// Points must be ascending and inside [0, horizon].
    PointTimes,
    EmptyRegime(usize),
    NoStationaryDistribution,
}

impl std::fmt::Display for JointError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::ElapsedTime(e) => write!(f, "{e}"),
            Self::Generator(e) => write!(f, "{e}"),
            Self::PointTimes => write!(f, "points must be ascending and inside [0, horizon]"),
            Self::EmptyRegime(k) => write!(f, "regime {k} received no expected time"),
            Self::NoStationaryDistribution => write!(f, "generator has no unique stationary distribution"),
        }
    }
}

impl std::error::Error for JointError {}

impl From<ElapsedTimeError> for JointError {
    fn from(e: ElapsedTimeError) -> Self {
        Self::ElapsedTime(e)
    }
}

impl From<GeneratorError> for JointError {
    fn from(e: GeneratorError) -> Self {
        Self::Generator(e)
    }
}

/// The joint model. `observation` carries the generator, the initial distribution and the
/// Student-t inputs; the remaining fields describe the trade arrivals.
#[derive(Debug, Clone, PartialEq)]
pub struct JointRegimeModel<const D: usize, const K: usize> {
    pub observation: ContinuousTimeStudentT<D, K>,
    pub baseline: [f64; K],
    pub branching_ratio: f64,
    pub decay: f64,
}

fn row_times<const K: usize>(row: &[f64; K], m: &[[f64; K]; K]) -> [f64; K] {
    let mut out = [0.0; K];
    for (&r, m_row) in row.iter().zip(m) {
        for (o, &v) in out.iter_mut().zip(m_row) {
            *o += r * v;
        }
    }
    out
}

struct Forward<const K: usize> {
    /// gaps[p] precedes point p; gaps[P] runs to the horizon.
    gaps: Vec<[[f64; K]; K]>,
    factors: Vec<[f64; K]>,
    /// alpha[0] = initial; alpha[p + 1] after point p (normalized).
    alpha: Vec<[f64; K]>,
    /// c_p for each point, then c_end.
    scales: Vec<f64>,
    /// Excitation just before each trade, by trade order.
    excitation: Vec<f64>,
    log_likelihood: f64,
}

fn as_step<const D: usize>(values: [f64; D], present: [bool; D]) -> Step<D> {
    Step { elapsed: 0.0, values, present }
}

impl<const D: usize, const K: usize> JointRegimeModel<D, K> {
    fn forward(&self, window: &JointWindow<D>) -> Result<Forward<K>, JointError> {
        let ascending = window.points.windows(2).all(|w| w[0].time() <= w[1].time());
        let inside = window.points.first().is_none_or(|p| p.time() >= 0.0) && window.points.last().is_none_or(|p| p.time() <= window.horizon);
        if !(ascending && inside && window.horizon.is_finite()) {
            return Err(JointError::PointTimes);
        }
        let generator = &self.observation.generator;
        let trades = window.trade_times();
        let excitation = excitation_before_events(&trades, self.branching_ratio, self.decay);
        let mut gaps = Vec::with_capacity(window.points.len() + 1);
        let mut factors = Vec::with_capacity(window.points.len());
        let mut alpha = Vec::with_capacity(window.points.len() + 1);
        alpha.push(self.observation.initial);
        let mut scales = Vec::with_capacity(window.points.len() + 1);
        let mut log_likelihood = 0.0;
        let mut prev = 0.0;
        let mut trade_index = 0;
        for point in &window.points {
            let gap = no_event_matrix(generator, &self.baseline, point.time() - prev)?;
            prev = point.time();
            let (factor, shift) = match point {
                Point::Trade { .. } => {
                    let g = excitation[trade_index];
                    trade_index += 1;
                    (self.baseline.map(|mu| mu + g), 0.0)
                }
                Point::Observation { values, present, .. } => self.observation.scaled_emissions(&as_step(*values, *present)),
            };
            let mut u = row_times(&alpha[alpha.len() - 1], &gap);
            for (v, &f) in u.iter_mut().zip(&factor) {
                *v *= f;
            }
            let c: f64 = u.iter().sum();
            log_likelihood += c.ln() + shift;
            scales.push(c);
            alpha.push(u.map(|v| v / c));
            gaps.push(gap);
            factors.push(factor);
        }
        let end_gap = no_event_matrix(generator, &self.baseline, window.horizon - prev)?;
        let c_end: f64 = row_times(&alpha[alpha.len() - 1], &end_gap).iter().sum();
        log_likelihood += c_end.ln();
        scales.push(c_end);
        gaps.push(end_gap);
        log_likelihood -= excitation_integral(&trades, window.horizon, self.branching_ratio, self.decay);
        Ok(Forward { gaps, factors, alpha, scales, excitation, log_likelihood })
    }

    /// Exact log-likelihood of one window.
    pub fn log_likelihood(&self, window: &JointWindow<D>) -> Result<f64, JointError> {
        Ok(self.forward(window)?.log_likelihood)
    }

    /// Regime probabilities just after each point, given the points up to it.
    pub fn filtered_regimes(&self, window: &JointWindow<D>) -> Result<Vec<[f64; K]>, JointError> {
        Ok(self.forward(window)?.alpha.split_off(1))
    }
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct JointFitSettings {
    pub max_iterations: usize,
    pub tolerance: f64,
    pub estimate_dof: bool,
}

impl Default for JointFitSettings {
    fn default() -> Self {
        Self { max_iterations: 500, tolerance: 1e-6, estimate_dof: true }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct JointFitReport {
    pub log_likelihoods: Vec<f64>,
    pub converged: bool,
}

/// Fits by EM from `start`. Every window starts with no excitation.
pub fn fit<const D: usize, const K: usize>(
    start: &JointRegimeModel<D, K>,
    windows: &[JointWindow<D>],
    settings: JointFitSettings,
) -> Result<(JointRegimeModel<D, K>, JointFitReport), JointError> {
    let mut model = start.clone();
    let mut log_likelihoods = Vec::new();
    let mut converged = false;
    for _ in 0..settings.max_iterations {
        // ---- E-step ----
        let mut stats = StepStatistics::with_event_rates(&model.observation.generator, &model.baseline);
        let mut immigrants = [0.0; K];
        let (mut offspring, mut offspring_lag) = (0.0, 0.0);
        let mut observed: Vec<(Step<D>, [f64; K])> = Vec::new();
        let mut total_ll = 0.0;
        for window in windows {
            let fw = model.forward(window)?;
            total_ll += fw.log_likelihood;
            let n_points = window.points.len();
            let trades = window.trade_times();
            // Mean lag to the parent for each trade (recursive sums over earlier trades).
            let mut lags = vec![0.0; trades.len()];
            let (mut s0, mut s1) = (0.0f64, 0.0f64);
            for (lag, pair) in lags.iter_mut().skip(1).zip(trades.windows(2)) {
                let dt = pair[1] - pair[0];
                let decay = (-model.decay * dt).exp();
                let a0 = s0 + 1.0;
                s1 = decay * (s1 + dt * a0);
                s0 = decay * a0;
                *lag = if s0 > 0.0 { s1 / s0 } else { 0.0 };
            }
            // Backward pass.
            let c_end = fw.scales[n_points];
            let end_gap = &fw.gaps[n_points];
            let mut beta: [f64; K] = std::array::from_fn(|k| end_gap[k].iter().sum::<f64>() / c_end);
            let a_last = fw.alpha[n_points];
            let w_end: [[f64; K]; K] = std::array::from_fn(|k| [a_last[k] / c_end; K]);
            stats.accumulate(window.horizon - window.points.last().map_or(0.0, Point::time), &w_end)?;
            let mut trade_index = trades.len();
            for p in (0..n_points).rev() {
                let alpha_p = fw.alpha[p + 1];
                let gamma: [f64; K] = std::array::from_fn(|k| alpha_p[k] * beta[k]);
                let s: f64 = gamma.iter().sum();
                let gamma = gamma.map(|v| v / s);
                match &window.points[p] {
                    Point::Trade { .. } => {
                        trade_index -= 1;
                        let g = fw.excitation[trade_index];
                        for (k, (&gamma_k, &rate)) in gamma.iter().zip(&fw.factors[p]).enumerate() {
                            if rate > 0.0 {
                                immigrants[k] += gamma_k * model.baseline[k] / rate;
                                let p_off = gamma_k * g / rate;
                                offspring += p_off;
                                offspring_lag += p_off * lags[trade_index];
                            }
                        }
                    }
                    Point::Observation { values, present, .. } => observed.push((as_step(*values, *present), gamma)),
                }
                let alpha_prev = fw.alpha[p];
                let c = fw.scales[p];
                let f = fw.factors[p];
                let w: [[f64; K]; K] = std::array::from_fn(|k| std::array::from_fn(|l| alpha_prev[k] * f[l] * beta[l] / c));
                let gap_len = window.points[p].time() - if p == 0 { 0.0 } else { window.points[p - 1].time() };
                stats.accumulate(gap_len, &w)?;
                let gap = &fw.gaps[p];
                beta = std::array::from_fn(|k| (0..K).map(|l| gap[k][l] * f[l] * beta[l]).sum::<f64>() / c);
            }
        }
        if let Some(&prev) = log_likelihoods.last()
            && total_ll - prev < settings.tolerance
        {
            log_likelihoods.push(total_ll);
            converged = true;
            break;
        }
        log_likelihoods.push(total_ll);

        // ---- M-step: switching rates and background rates ----
        let mut q = [[0.0; K]; K];
        let mut baseline = [0.0; K];
        for (k, q_row) in q.iter_mut().enumerate() {
            let time = stats.occupancy[k];
            if time <= 0.0 {
                return Err(JointError::EmptyRegime(k));
            }
            baseline[k] = immigrants[k] / time;
            let mut exit = 0.0;
            for l in (0..K).filter(|&l| l != k) {
                q_row[l] = stats.jumps[k][l] / time;
                exit += q_row[l];
            }
            q_row[k] = -exit;
        }
        let generator = Generator::new(q)?;

        // ---- M-step: excitation ----
        let (branching_ratio, decay) = if offspring > 1e-12 {
            let budget = |beta: f64| {
                windows
                    .iter()
                    .map(|w| w.trade_times().iter().map(|&t| 1.0 - (-beta * (w.horizon - t)).exp()).sum::<f64>())
                    .sum::<f64>()
            };
            let objective = |log_beta: f64| {
                let beta = log_beta.exp();
                let c = budget(beta);
                let n = offspring / c;
                offspring * (n * beta).ln() - beta * offspring_lag - n * c
            };
            let centre = model.decay.ln();
            let beta = golden_max(objective, centre - 5.0, centre + 5.0).exp();
            (offspring / budget(beta), beta)
        } else {
            (0.0, model.decay)
        };

        // ---- M-step: Student-t inputs ----
        let (means, covars, dof) = update_emissions(&model.observation, || observed.iter().map(|(s, g)| (s, g)), settings.estimate_dof);

        let initial = generator.stationary_distribution().ok_or(JointError::NoStationaryDistribution)?;
        model = JointRegimeModel {
            observation: ContinuousTimeStudentT { generator, initial, means, covars, dof, ..model.observation },
            baseline,
            branching_ratio,
            decay,
        };
    }
    Ok((model, JointFitReport { log_likelihoods, converged }))
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::init::SplitMix64;
    use crate::regime_modulated_hawkes::{EventWindow, RegimeHawkes, simulate_events_on_path};
    use crate::regime_path_simulator::{simulate_emissions, simulate_path};

    fn observation_model() -> ContinuousTimeStudentT<2, 2> {
        let generator = Generator::new([[-0.004, 0.004], [0.008, -0.008]]).unwrap();
        let initial = generator.stationary_distribution().unwrap();
        ContinuousTimeStudentT {
            generator,
            initial,
            means: [[-2.0, 1.0], [2.0, -1.0]],
            covars: [[1.0, 0.5], [0.8, 1.5]],
            dof: [5.0, 8.0],
            dof_min: 2.1,
            dof_max: 100.0,
            feature_weights: [1.0, 1.0],
        }
    }

    #[test]
    fn step_trigger_known_answers() {
        let trigger = StepTrigger { trades: 3, volume: 10, floor: 5.0 };
        // Trades at 1, 2, 3 -> 3rd trade fires at 3. Trade at 4 (size 12) -> volume fires at 4.
        // Then nothing until 9 -> floor step at 9 (4 + 5). Trade at 9.5, then floor at 14.
        let steps = step_times(&[1.0, 2.0, 3.0, 4.0, 9.5], &[1, 1, 1, 12, 1], trigger, 15.0);
        assert_eq!(steps, vec![3.0, 4.0, 9.0, 14.0]);
        // No trades: floor steps only.
        assert_eq!(step_times(&[], &[], trigger, 12.0), vec![5.0, 10.0]);
    }

    /// No trades, zero background rate, no excitation: exactly the continuous-time Student-t model.
    #[test]
    fn without_trades_it_is_the_continuous_time_student_t_model() {
        let obs = observation_model();
        let joint = JointRegimeModel { observation: obs.clone(), baseline: [0.0, 0.0], branching_ratio: 0.0, decay: 1.0 };
        let data = [(0.0, [-1.8, 0.7], [true, true]), (3.0, [2.2, 0.0], [true, false]), (3.5, [1.9, -1.3], [true, true]), (40.0, [0.0, 0.0], [false, false])];
        let window = JointWindow::merge(&[], &data, 50.0);
        let mut prev = 0.0;
        let steps: Vec<Step<2>> = data
            .iter()
            .map(|&(t, values, present)| {
                let s = Step { elapsed: t - prev, values, present };
                prev = t;
                s
            })
            .collect();
        let expected = obs.filter(&steps).unwrap().log_likelihood;
        assert!((joint.log_likelihood(&window).unwrap() - expected).abs() < 1e-11);
    }

    /// No observation steps: exactly the regime-modulated Hawkes model.
    #[test]
    fn without_observations_it_is_the_regime_modulated_hawkes_model() {
        let obs = observation_model();
        let joint = JointRegimeModel { observation: obs.clone(), baseline: [0.3, 2.0], branching_ratio: 0.4, decay: 2.5 };
        let trades = vec![0.2, 0.25, 1.0, 6.0, 6.01, 30.0];
        let window = JointWindow::merge(&trades, &[], 40.0);
        let hawkes = RegimeHawkes { generator: obs.generator.clone(), initial: obs.initial, baseline: [0.3, 2.0], branching_ratio: 0.4, decay: 2.5 };
        let expected = hawkes.log_likelihood(&EventWindow { times: trades, horizon: 40.0 }).unwrap();
        assert!((joint.log_likelihood(&window).unwrap() - expected).abs() < 1e-11);
    }

    fn simulate(truth: &JointRegimeModel<2, 2>, trigger: StepTrigger, horizon: f64, seed: u64) -> JointWindow<2> {
        let mut rng = SplitMix64::new(seed);
        let obs = &truth.observation;
        let path = simulate_path(&obs.generator, &obs.initial, horizon, &mut rng);
        let hawkes = RegimeHawkes { generator: obs.generator.clone(), initial: obs.initial, baseline: truth.baseline, branching_ratio: truth.branching_ratio, decay: truth.decay };
        let (_, trades) = simulate_events_on_path(&hawkes, &path, &mut rng);
        let sizes = vec![1u64; trades.len()];
        let times = step_times(&trades, &sizes, trigger, horizon);
        let states = path.sample_at(&times);
        let xs = simulate_emissions(&states, &obs.means, &obs.covars, &obs.dof, &mut rng);
        let observations: Vec<(f64, [f64; 2], [bool; 2])> = times.iter().zip(xs).map(|(&t, x)| (t, x, [rng.next_f64() > 0.2, rng.next_f64() > 0.2])).collect();
        JointWindow::merge(&trades, &observations, horizon)
    }

    fn truth() -> JointRegimeModel<2, 2> {
        JointRegimeModel { observation: observation_model(), baseline: [0.3, 2.0], branching_ratio: 0.5, decay: 2.0 }
    }

    fn start() -> JointRegimeModel<2, 2> {
        let generator = Generator::new([[-0.01, 0.01], [0.01, -0.01]]).unwrap();
        let observation = ContinuousTimeStudentT {
            generator,
            initial: [0.5, 0.5],
            means: [[-1.0, 0.5], [1.0, -0.5]],
            covars: [[2.0, 2.0], [2.0, 2.0]],
            dof: [10.0, 10.0],
            ..observation_model()
        };
        JointRegimeModel { observation, baseline: [0.5, 1.5], branching_ratio: 0.3, decay: 1.0 }
    }

    const TRIGGER: StepTrigger = StepTrigger { trades: 20, volume: 1_000_000, floor: 30.0 };

    #[test]
    fn em_never_decreases_the_likelihood() {
        let window = simulate(&truth(), TRIGGER, 4_000.0, 91);
        let (_, report) = fit(&start(), &[window], JointFitSettings { max_iterations: 20, tolerance: f64::NEG_INFINITY, estimate_dof: true }).unwrap();
        for w in report.log_likelihoods.windows(2) {
            assert!(w[1] >= w[0] - 1e-7 * w[0].abs(), "decrease {} -> {}", w[0], w[1]);
        }
    }

    /// The full known-answer test: trades with regime background rates and an echo, observation
    /// steps fired by the real trigger (20 trades or 30 time units), Student-t inputs with 20% absent.
    #[test]
    fn recovers_every_parameter_from_trades_and_steps() {
        let t = truth();
        let window = simulate(&t, TRIGGER, 20_000.0, 92);
        let (f, report) = fit(&start(), &[window], JointFitSettings { max_iterations: 400, tolerance: 1e-6, estimate_dof: true }).unwrap();
        assert!(report.converged);
        let (qt, qf) = (t.observation.generator.matrix(), f.observation.generator.matrix());
        assert!((qf[0][1] / qt[0][1] - 1.0).abs() < 0.3, "q01 {}", qf[0][1]);
        assert!((qf[1][0] / qt[1][0] - 1.0).abs() < 0.3, "q10 {}", qf[1][0]);
        for k in 0..2 {
            assert!((f.baseline[k] / t.baseline[k] - 1.0).abs() < 0.1, "mu[{k}] {}", f.baseline[k]);
            for d in 0..2 {
                assert!((f.observation.means[k][d] - t.observation.means[k][d]).abs() < 0.1, "mean[{k}][{d}] {}", f.observation.means[k][d]);
                assert!((f.observation.covars[k][d] / t.observation.covars[k][d] - 1.0).abs() < 0.15, "covar[{k}][{d}] {}", f.observation.covars[k][d]);
            }
            assert!((f.observation.dof[k] - t.observation.dof[k]).abs() < 0.5 * t.observation.dof[k], "dof[{k}] {}", f.observation.dof[k]);
        }
        assert!((f.branching_ratio - 0.5).abs() < 0.04, "n {}", f.branching_ratio);
        assert!((f.decay / 2.0 - 1.0).abs() < 0.12, "beta {}", f.decay);
    }
}
