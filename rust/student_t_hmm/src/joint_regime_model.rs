//! joint_regime_model.rs -- the regime model reading BOTH the trades and the observation steps, in
//! market time (rust-migration spec §6.2.8 D4; step-length proposal approved 2026-10-14).
//!
//! One hidden continuous-time regime drives
//!   * the trade arrival rate: mu_k * s_b + g(t) -- a regime background level times a time-of-day
//!     factor s_b (b = the intraday bucket), plus one shared echo of earlier trades
//!     (`regime_modulated_hawkes`); and
//!   * the Student-t inputs read at each observation step (`continuous_time_student_t`).
//!
//! **Market time.** Each window carries its timeline: open periods (each in one time-of-day bucket)
//! and closed periods (weekends, the daily halt, holidays). Regimes switch at the generator's rates
//! in open time and at `closed_weight` times them in closed time (learned, in [0, 1]); information
//! arrives mostly while trading (French & Roll 1986). No trades can occur while closed, so a closed
//! period is NOT evidence of a calm regime: it only lets regimes switch at the closed weight. The
//! echo decays in calendar time and only accumulates intensity while open.
//!
//! **Activity is evidence, not the clock.** Trades enter through the arrival likelihood (busy regimes
//! trade more and cluster more); they are not also a time deformation, which would use the same
//! information twice. Regime-dependent timing is modelled jointly (Lange, Hubbard, Inoue & Minin
//! 2015), as Gruger, Kay & Schumacher (1991) require for informative observation times.
//!
//! **Time of day.** Trading intensity follows a strong daily rhythm; a background rate that depended
//! only on the regime would make the regimes learn the clock (Andersen & Bollerslev 1997; the LaMa
//! review's caution). With the multiplicative factor s_b (normalized to average 1 over open time) the
//! regimes capture departures from the normal rhythm.
//!
//! Likelihood: the product, in time order, of e^{(Q - diag(mu s_b)) dt} for open gaps (with the
//! smallest rate's decay factored out as a log scale, `no_event_matrix_scaled`, so long gaps with a
//! busy regime do not underflow),
//! e^{closed_weight Q dt} for closed gaps, diag(mu s_b + g(t-)) at a trade and diag(b(x)) at an
//! observation step, times e^{-int g over open time}. With no trades and mu = 0, n = 0 on an all-open
//! timeline it is exactly the continuous-time Student-t model; with no observation steps it is
//! exactly the regime-modulated Hawkes model (both tested).
//!
//! Training (EM / ECM): one scaled forward-backward over all items; expected regime occupancy and
//! switches per open bucket and for closed time (`StepStatistics`); immigrant / offspring
//! responsibilities at trades; regime posteriors at observation steps for the Student-t updates.
//! M-step, each block maximizing the expected complete-data log-likelihood given the others:
//!   q_kl = (N_open_kl + N_closed_kl) / (T_open_k + w T_closed_k),  w = sum N_closed / sum_k exit_k T_closed_k
//!   mu_k = sum_b I_kb / sum_b s_b T_kb,  s_b = sum_k I_kb / sum_k mu_k T_kb  (alternated, s normalized)
//!   n, beta: offspring count over the open-time echo budget; beta by golden section.

use crate::continuous_time_student_t::{ContinuousTimeStudentT, Step, update_emissions};
use crate::elapsed_time_transitions::{ElapsedTimeError, StepStatistics, no_event_matrix_scaled, shifted_rates, weighted_transition_matrix};
use crate::regime_modulated_hawkes::golden_max;
use crate::regime_path_simulator::{Generator, GeneratorError};

/// One period of a window's timeline.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Piece {
    /// Market open, in time-of-day bucket `bucket`.
    Open { duration: f64, bucket: usize },
    /// Market closed.
    Closed { duration: f64 },
}

impl Piece {
    fn duration(&self) -> f64 {
        match self {
            Self::Open { duration, .. } | Self::Closed { duration } => *duration,
        }
    }
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

/// One observation window: time-ordered points on a timeline starting at time 0.
#[derive(Debug, Clone, PartialEq)]
pub struct JointWindow<const D: usize> {
    pub points: Vec<Point<D>>,
    pub timeline: Vec<Piece>,
}

impl<const D: usize> JointWindow<D> {
    /// Merges trades and observation steps into one stream (trades first at equal times).
    pub fn new(trades: &[f64], observations: &[(f64, [f64; D], [bool; D])], timeline: Vec<Piece>) -> Self {
        let mut points: Vec<Point<D>> = trades.iter().map(|&time| Point::Trade { time }).collect();
        points.extend(observations.iter().map(|&(time, values, present)| Point::Observation { time, values, present }));
        points.sort_by(|a, b| {
            a.time().total_cmp(&b.time()).then_with(|| matches!(a, Point::Observation { .. }).cmp(&matches!(b, Point::Observation { .. })))
        });
        Self { points, timeline }
    }

    /// A window that is open throughout, in one bucket.
    pub fn all_open(trades: &[f64], observations: &[(f64, [f64; D], [bool; D])], horizon: f64) -> Self {
        Self::new(trades, observations, vec![Piece::Open { duration: horizon, bucket: 0 }])
    }

    pub fn horizon(&self) -> f64 {
        self.timeline.iter().map(Piece::duration).sum()
    }
}

#[derive(Debug, Clone, PartialEq)]
pub enum JointError {
    ElapsedTime(ElapsedTimeError),
    Generator(GeneratorError),
    /// Points must be ascending, inside the timeline, and only trades/steps while open.
    PointTimes,
    /// A piece's bucket is outside the model's time-of-day factors.
    Bucket(usize),
    EmptyRegime(usize),
    NoStationaryDistribution,
    /// A present input value is not finite: training stops rather than learn from it.
    InvalidInput { window: usize, point: usize, input: usize },
}

impl std::fmt::Display for JointError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::ElapsedTime(e) => write!(f, "{e}"),
            Self::Generator(e) => write!(f, "{e}"),
            Self::PointTimes => write!(f, "points must be ascending and lie in open periods of the timeline"),
            Self::Bucket(b) => write!(f, "time-of-day bucket {b} has no factor"),
            Self::EmptyRegime(k) => write!(f, "regime {k} received no expected time"),
            Self::NoStationaryDistribution => write!(f, "generator has no unique stationary distribution"),
            Self::InvalidInput { window, point, input } => {
                write!(f, "invalid (non-finite) value in window {window}, point {point}, input {input}; refusing to train")
            }
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
/// Student-t inputs; the remaining fields describe market time and the trade arrivals.
#[derive(Debug, Clone, PartialEq)]
pub struct JointRegimeModel<const D: usize, const K: usize> {
    pub observation: ContinuousTimeStudentT<D, K>,
    /// Regime background trade levels mu_k.
    pub baseline: [f64; K],
    /// Time-of-day factors s_b (one per bucket; average 1 over open time after fitting).
    pub seasonal: Vec<f64>,
    /// Regime switching rate while closed, as a fraction of the open rate (in [0, 1]).
    pub closed_weight: f64,
    pub branching_ratio: f64,
    pub decay: f64,
}

/// The timeline walked against the points: what happens next.
#[derive(Debug, Clone, Copy, PartialEq)]
enum Item {
    /// Time passing: open in a bucket, or closed (`None`).
    Gap { duration: f64, bucket: Option<usize> },
    /// The point with this index, during an open period in this bucket.
    At { point: usize, bucket: usize },
}

fn walk<const D: usize>(window: &JointWindow<D>) -> Result<Vec<Item>, JointError> {
    let mut items = Vec::with_capacity(window.points.len() * 2 + window.timeline.len());
    let mut next = 0usize;
    let mut cursor = 0.0;
    let mut start = 0.0;
    for piece in &window.timeline {
        let end = start + piece.duration();
        match *piece {
            Piece::Open { bucket, .. } => {
                while next < window.points.len() && window.points[next].time() <= end {
                    let t = window.points[next].time();
                    if t < cursor {
                        return Err(JointError::PointTimes);
                    }
                    items.push(Item::Gap { duration: t - cursor, bucket: Some(bucket) });
                    items.push(Item::At { point: next, bucket });
                    cursor = t;
                    next += 1;
                }
                items.push(Item::Gap { duration: end - cursor, bucket: Some(bucket) });
            }
            Piece::Closed { .. } => {
                if next < window.points.len() && window.points[next].time() < end {
                    return Err(JointError::PointTimes);
                }
                items.push(Item::Gap { duration: end - cursor, bucket: None });
            }
        }
        cursor = end;
        start = end;
    }
    if next < window.points.len() {
        return Err(JointError::PointTimes);
    }
    Ok(items)
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

fn as_step<const D: usize>(values: [f64; D], present: [bool; D]) -> Step<D> {
    Step { elapsed: 0.0, values, present }
}

/// Per item: the operator (a matrix for gaps, a diagonal for points).
#[derive(Debug, Clone)]
enum Operator<const K: usize> {
    Matrix([[f64; K]; K]),
    Diagonal([f64; K]),
}

struct Forward<const K: usize> {
    items: Vec<Item>,
    operators: Vec<Operator<K>>,
    /// Excitation just before each item (only used at trades).
    excitation: Vec<f64>,
    /// alpha[0] = initial; alpha[i + 1] after item i (normalized).
    alpha: Vec<[f64; K]>,
    scales: Vec<f64>,
    log_likelihood: f64,
}

impl<const D: usize, const K: usize> JointRegimeModel<D, K> {
    fn arrival_rates(&self, bucket: usize) -> Result<[f64; K], JointError> {
        let s = *self.seasonal.get(bucket).ok_or(JointError::Bucket(bucket))?;
        Ok(self.baseline.map(|mu| mu * s))
    }

    fn forward(&self, window: &JointWindow<D>) -> Result<Forward<K>, JointError> {
        let items = walk(window)?;
        let generator = &self.observation.generator;
        let jump = self.branching_ratio * self.decay;
        let mut operators = Vec::with_capacity(items.len());
        let mut excitation = Vec::with_capacity(items.len());
        let mut alpha = Vec::with_capacity(items.len() + 1);
        alpha.push(self.observation.initial);
        let mut scales = Vec::with_capacity(items.len());
        let mut log_likelihood = 0.0;
        let mut g = 0.0;
        for item in &items {
            excitation.push(g);
            let (op, shift) = match *item {
                Item::Gap { duration, bucket } => {
                    let decay = (-self.decay * duration).exp();
                    let (m, log_scale) = match bucket {
                        Some(b) => {
                            // Open: compensator of the echo over this gap.
                            log_likelihood -= g * (1.0 - decay) / self.decay;
                            no_event_matrix_scaled(generator, &self.arrival_rates(b)?, duration)?
                        }
                        None => (weighted_transition_matrix(generator, self.closed_weight, duration)?, 0.0),
                    };
                    g *= decay;
                    (Operator::Matrix(m), log_scale)
                }
                Item::At { point, bucket } => match &window.points[point] {
                    Point::Trade { .. } => {
                        let rates = self.arrival_rates(bucket)?;
                        let f = rates.map(|r| r + g);
                        g += jump;
                        (Operator::Diagonal(f), 0.0)
                    }
                    Point::Observation { values, present, .. } => {
                        let (b, shift) = self.observation.scaled_emissions(&as_step(*values, *present));
                        (Operator::Diagonal(b), shift)
                    }
                },
            };
            let prev = alpha[alpha.len() - 1];
            let u = match &op {
                Operator::Matrix(m) => row_times(&prev, m),
                Operator::Diagonal(f) => std::array::from_fn(|k| prev[k] * f[k]),
            };
            let c: f64 = u.iter().sum();
            log_likelihood += c.ln() + shift;
            scales.push(c);
            alpha.push(u.map(|v| v / c));
            operators.push(op);
        }
        Ok(Forward { items, operators, excitation, alpha, scales, log_likelihood })
    }

    /// Exact log-likelihood of one window.
    pub fn log_likelihood(&self, window: &JointWindow<D>) -> Result<f64, JointError> {
        Ok(self.forward(window)?.log_likelihood)
    }

    /// Regime probabilities just after each point, given everything up to it.
    pub fn filtered_regimes(&self, window: &JointWindow<D>) -> Result<Vec<[f64; K]>, JointError> {
        let fw = self.forward(window)?;
        Ok(fw.items.iter().enumerate().filter(|(_, it)| matches!(it, Item::At { .. })).map(|(i, _)| fw.alpha[i + 1]).collect())
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

/// Expected sufficient statistics of one E-step.
struct Expectations<const D: usize, const K: usize> {
    /// Per bucket: occupancy and switches while open.
    open: Vec<StepStatistics<K>>,
    closed: StepStatistics<K>,
    /// Per bucket: expected immigrants per regime.
    immigrants: Vec<[f64; K]>,
    offspring: f64,
    offspring_lag: f64,
    /// sum over open gaps of S0(gap start) (1 - e^{-beta d}), as a function of beta: stored as
    /// (gap duration, time since each earlier trade) is too large, so the budget is recomputed from
    /// the windows when beta changes (see `echo_budget`).
    observed: Vec<(Step<D>, [f64; K])>,
    log_likelihood: f64,
}

/// sum over open gaps of sum_{earlier trades j} e^{-beta (gap start - t_j)} (1 - e^{-beta d}):
/// the echo's open-time exposure, so that int g over open time = n * budget.
fn echo_budget<const D: usize>(windows: &[JointWindow<D>], items: &[Vec<Item>], beta: f64) -> f64 {
    let mut total = 0.0;
    for (window, items) in windows.iter().zip(items) {
        let mut s0 = 0.0f64;
        for item in items {
            match *item {
                Item::Gap { duration, bucket } => {
                    let decay = (-beta * duration).exp();
                    if bucket.is_some() {
                        total += s0 * (1.0 - decay);
                    }
                    s0 *= decay;
                }
                Item::At { point, .. } => {
                    if matches!(window.points[point], Point::Trade { .. }) {
                        s0 += 1.0;
                    }
                }
            }
        }
    }
    total
}

fn e_step<const D: usize, const K: usize>(
    model: &JointRegimeModel<D, K>,
    windows: &[JointWindow<D>],
    all_items: &mut Vec<Vec<Item>>,
) -> Result<Expectations<D, K>, JointError> {
    let generator = &model.observation.generator;
    let buckets = model.seasonal.len();
    let mut open: Vec<StepStatistics<K>> =
        (0..buckets).map(|b| model.arrival_rates(b).map(|r| StepStatistics::with_event_rates(generator, &shifted_rates(&r)))).collect::<Result<_, _>>()?;
    let mut closed = StepStatistics::with_rate_weight(generator, model.closed_weight);
    let mut ex = Expectations {
        open: Vec::new(),
        closed: StepStatistics::with_rate_weight(generator, model.closed_weight),
        immigrants: vec![[0.0; K]; buckets],
        offspring: 0.0,
        offspring_lag: 0.0,
        observed: Vec::new(),
        log_likelihood: 0.0,
    };
    all_items.clear();
    for window in windows {
        let fw = model.forward(window)?;
        ex.log_likelihood += fw.log_likelihood;
        let n = fw.items.len();
        // Mean lag to the parent of each trade (calendar time), by item index.
        let mut lag_at = vec![0.0; n];
        {
            let (mut s0, mut s1) = (0.0f64, 0.0f64);
            for (i, item) in fw.items.iter().enumerate() {
                match *item {
                    Item::Gap { duration, .. } => {
                        let decay = (-model.decay * duration).exp();
                        s1 = decay * (s1 + duration * s0);
                        s0 *= decay;
                    }
                    Item::At { point, .. } => {
                        if matches!(window.points[point], Point::Trade { .. }) {
                            lag_at[i] = if s0 > 0.0 { s1 / s0 } else { 0.0 };
                            s0 += 1.0;
                        }
                    }
                }
            }
        }
        // Backward: beta_last = 1; beta_{i-1} = O_i beta_i / c_i.
        let mut beta = [1.0; K];
        for i in (0..n).rev() {
            let alpha_prev = fw.alpha[i];
            let c = fw.scales[i];
            match (&fw.items[i], &fw.operators[i]) {
                (Item::Gap { duration, bucket }, Operator::Matrix(m)) => {
                    let w: [[f64; K]; K] = std::array::from_fn(|k| std::array::from_fn(|l| alpha_prev[k] * beta[l] / c));
                    match bucket {
                        Some(b) => open[*b].accumulate(*duration, &w)?,
                        None => closed.accumulate(*duration, &w)?,
                    }
                    beta = std::array::from_fn(|k| (0..K).map(|l| m[k][l] * beta[l]).sum::<f64>() / c);
                }
                (Item::At { point, bucket }, Operator::Diagonal(f)) => {
                    let after = fw.alpha[i + 1];
                    let gamma: [f64; K] = std::array::from_fn(|k| after[k] * beta[k]);
                    let s: f64 = gamma.iter().sum();
                    let gamma = gamma.map(|v| v / s);
                    match &window.points[*point] {
                        Point::Trade { .. } => {
                            let g = fw.excitation[i];
                            for (k, (&gamma_k, &rate)) in gamma.iter().zip(f).enumerate() {
                                if rate > 0.0 {
                                    ex.immigrants[*bucket][k] += gamma_k * (rate - g) / rate;
                                    let p_off = gamma_k * g / rate;
                                    ex.offspring += p_off;
                                    ex.offspring_lag += p_off * lag_at[i];
                                }
                            }
                        }
                        Point::Observation { values, present, .. } => ex.observed.push((as_step(*values, *present), gamma)),
                    }
                    beta = std::array::from_fn(|k| f[k] * beta[k] / c);
                }
                _ => unreachable!("gaps carry matrices, points carry diagonals"),
            }
        }
        all_items.push(fw.items);
    }
    ex.open = open;
    ex.closed = closed;
    Ok(ex)
}

/// Fits by EM from `start`. Every window starts with no excitation.
pub fn fit<const D: usize, const K: usize>(
    start: &JointRegimeModel<D, K>,
    windows: &[JointWindow<D>],
    settings: JointFitSettings,
) -> Result<(JointRegimeModel<D, K>, JointFitReport), JointError> {
    for (w, window) in windows.iter().enumerate() {
        for (p, point) in window.points.iter().enumerate() {
            if let Point::Observation { values, present, .. } = point
                && let Some(j) = (0..D).find(|&j| present[j] && !values[j].is_finite())
            {
                return Err(JointError::InvalidInput { window: w, point: p, input: j });
            }
        }
    }
    let mut model = start.clone();
    let mut log_likelihoods = Vec::new();
    let mut converged = false;
    let mut items = Vec::new();
    for _ in 0..settings.max_iterations {
        let ex = e_step(&model, windows, &mut items)?;
        if let Some(&prev) = log_likelihoods.last()
            && ex.log_likelihood - prev < settings.tolerance
        {
            log_likelihoods.push(ex.log_likelihood);
            converged = true;
            break;
        }
        log_likelihoods.push(ex.log_likelihood);
        let buckets = model.seasonal.len();

        // ---- Switching rates and the closed-time weight (alternated) ----
        let mut open_time = [0.0; K];
        let mut open_jumps = [[0.0; K]; K];
        for st in &ex.open {
            for k in 0..K {
                open_time[k] += st.occupancy[k];
                for (total, &n) in open_jumps[k].iter_mut().zip(&st.jumps[k]) {
                    *total += n;
                }
            }
        }
        let (closed_time, closed_jumps) = (ex.closed.occupancy, ex.closed.jumps);
        let total_closed_jumps: f64 = closed_jumps.iter().flatten().sum();
        let any_closed = closed_time.iter().any(|&t| t > 0.0);
        let mut weight = model.closed_weight;
        let mut q = [[0.0; K]; K];
        for _ in 0..50 {
            for k in 0..K {
                let exposure = open_time[k] + weight * closed_time[k];
                if exposure <= 0.0 {
                    return Err(JointError::EmptyRegime(k));
                }
                let mut exit = 0.0;
                for l in (0..K).filter(|&l| l != k) {
                    q[k][l] = (open_jumps[k][l] + closed_jumps[k][l]) / exposure;
                    exit += q[k][l];
                }
                q[k][k] = -exit;
            }
            if !any_closed {
                break;
            }
            let denominator: f64 = (0..K).map(|k| -q[k][k] * closed_time[k]).sum();
            weight = if denominator > 0.0 { (total_closed_jumps / denominator).clamp(0.0, 1.0) } else { 0.0 };
        }
        let generator = Generator::new(q)?;

        // ---- Background levels and time-of-day factors (alternated, factors normalized) ----
        let exposure: Vec<[f64; K]> = ex.open.iter().map(|st| st.occupancy).collect();
        let mut baseline = model.baseline;
        let mut seasonal = model.seasonal.clone();
        for _ in 0..50 {
            for k in 0..K {
                let num: f64 = (0..buckets).map(|b| ex.immigrants[b][k]).sum();
                let den: f64 = (0..buckets).map(|b| seasonal[b] * exposure[b][k]).sum();
                baseline[k] = if den > 0.0 { num / den } else { 0.0 };
            }
            for b in 0..buckets {
                let num: f64 = ex.immigrants[b].iter().sum();
                let den: f64 = (0..K).map(|k| baseline[k] * exposure[b][k]).sum();
                seasonal[b] = if den > 0.0 { num / den } else { 1.0 };
            }
            let open_by_bucket: Vec<f64> = exposure.iter().map(|e| e.iter().sum()).collect();
            let total_open: f64 = open_by_bucket.iter().sum();
            let mean: f64 = seasonal.iter().zip(&open_by_bucket).map(|(s, w)| s * w).sum::<f64>() / total_open;
            if mean > 0.0 {
                for s in &mut seasonal {
                    *s /= mean;
                }
                for mu in &mut baseline {
                    *mu *= mean;
                }
            }
        }

        // ---- Echo: n = O / budget(beta), beta by golden section ----
        let (branching_ratio, decay) = if ex.offspring > 1e-12 {
            let objective = |log_beta: f64| {
                let beta = log_beta.exp();
                let c = echo_budget(windows, &items, beta);
                let n = ex.offspring / c;
                ex.offspring * (n * beta).ln() - beta * ex.offspring_lag - n * c
            };
            let centre = model.decay.ln();
            let beta = golden_max(objective, centre - 5.0, centre + 5.0).exp();
            (ex.offspring / echo_budget(windows, &items, beta), beta)
        } else {
            (0.0, model.decay)
        };

        // ---- Student-t inputs ----
        let (means, covars, dof) = update_emissions(&model.observation, || ex.observed.iter().map(|(s, g)| (s, g)), settings.estimate_dof);

        let initial = generator.stationary_distribution().ok_or(JointError::NoStationaryDistribution)?;
        model = JointRegimeModel {
            observation: ContinuousTimeStudentT { generator, initial, means, covars, dof, ..model.observation },
            baseline,
            seasonal,
            closed_weight: weight,
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
    use crate::regime_modulated_hawkes::{EventWindow, RegimeHawkes};
    use crate::regime_path_simulator::simulate_emissions;

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

    fn plain(observation: ContinuousTimeStudentT<2, 2>, baseline: [f64; 2], n: f64, beta: f64) -> JointRegimeModel<2, 2> {
        JointRegimeModel { observation, baseline, seasonal: vec![1.0], closed_weight: 1.0, branching_ratio: n, decay: beta }
    }

    /// All open, no trades, mu = 0, n = 0: exactly the continuous-time Student-t model.
    #[test]
    fn without_trades_it_is_the_continuous_time_student_t_model() {
        let obs = observation_model();
        let joint = plain(obs.clone(), [0.0, 0.0], 0.0, 1.0);
        let data = [(0.0, [-1.8, 0.7], [true, true]), (3.0, [2.2, 0.0], [true, false]), (3.5, [1.9, -1.3], [true, true]), (40.0, [0.0, 0.0], [false, false])];
        let window = JointWindow::all_open(&[], &data, 50.0);
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

    /// All open, no observation steps: exactly the regime-modulated Hawkes model.
    #[test]
    fn without_observations_it_is_the_regime_modulated_hawkes_model() {
        let obs = observation_model();
        let joint = plain(obs.clone(), [0.3, 2.0], 0.4, 2.5);
        let trades = vec![0.2, 0.25, 1.0, 6.0, 6.01, 30.0];
        let window = JointWindow::all_open(&trades, &[], 40.0);
        let hawkes = RegimeHawkes { generator: obs.generator.clone(), initial: obs.initial, baseline: [0.3, 2.0], branching_ratio: 0.4, decay: 2.5 };
        let expected = hawkes.log_likelihood(&EventWindow { times: trades, horizon: 40.0 }).unwrap();
        assert!((joint.log_likelihood(&window).unwrap() - expected).abs() < 1e-11);
    }

    /// A closed period with weight 0 changes nothing about the regime: the regime probabilities
    /// after it equal those without it, and a long weekend without trades is not evidence of calm.
    #[test]
    fn a_closed_period_is_not_evidence_of_a_calm_regime() {
        let mut joint = plain(observation_model(), [0.3, 3.0], 0.0, 1.0);
        joint.closed_weight = 0.0;
        let trades_before = [1.0, 1.5, 2.0, 2.2, 2.4];
        let open_only = JointWindow::all_open(&trades_before, &[], 3.0);
        let mut timeline = vec![Piece::Open { duration: 3.0, bucket: 0 }, Piece::Closed { duration: 5_000.0 }];
        let with_weekend = JointWindow::new(&trades_before, &[], timeline.clone());
        let a = joint.forward(&open_only).unwrap();
        let b = joint.forward(&with_weekend).unwrap();
        assert!((a.log_likelihood - b.log_likelihood).abs() < 1e-12);
        let (pa, pb) = (a.alpha[a.alpha.len() - 1], b.alpha[b.alpha.len() - 1]);
        assert!(pa.iter().zip(&pb).all(|(x, y)| (x - y).abs() < 1e-12));
        // The same 5,000 time units OPEN without a trade would be strong evidence for the calm regime.
        timeline[1] = Piece::Open { duration: 5_000.0, bucket: 0 };
        let open_weekend = joint.forward(&JointWindow::new(&trades_before, &[], timeline)).unwrap();
        let calm = open_weekend.alpha[open_weekend.alpha.len() - 1][0];
        assert!(calm > 0.99, "{calm}");
    }

    /// The scaled forward pass equals a direct product over a mixed timeline (open buckets with
    /// different time-of-day factors, a closed period at weight 0.3, trades and an observation).
    #[test]
    fn scaled_forward_equals_the_direct_product_on_a_mixed_timeline() {
        let mut joint = plain(observation_model(), [0.5, 2.0], 0.3, 1.5);
        joint.seasonal = vec![1.4, 0.6];
        joint.closed_weight = 0.3;
        let timeline = vec![Piece::Open { duration: 2.0, bucket: 0 }, Piece::Closed { duration: 10.0 }, Piece::Open { duration: 3.0, bucket: 1 }];
        let trades = [0.5, 0.6, 1.9, 12.5, 14.0];
        let obs = [(1.0, [-1.5, 0.8], [true, true]), (13.0, [2.0, 0.0], [true, false])];
        let window = JointWindow::new(&trades, &obs, timeline);
        // Direct: walk by hand.
        let generator = &joint.observation.generator;
        let (n, beta) = (0.3, 1.5);
        let mut row = joint.observation.initial;
        let mut g = 0.0;
        let mut compensator = 0.0;
        let mut t = 0.0;
        let open_gap = |row: &mut [f64; 2], g: &mut f64, comp: &mut f64, d: f64, s: f64| {
            *row = row_times(row, &crate::elapsed_time_transitions::no_event_matrix(generator, &[0.5 * s, 2.0 * s], d).unwrap());
            *comp += *g * (1.0 - (-beta * d).exp()) / beta;
            *g *= (-beta * d).exp();
        };
        let events: [(f64, Option<usize>); 7] = [(0.5, None), (0.6, None), (1.0, Some(0)), (1.9, None), (12.5, None), (13.0, Some(1)), (14.0, None)];
        for &(time, obs_index) in &events {
            let s = if time < 2.0 { 1.4 } else { 0.6 };
            if t < 2.0 && time > 2.0 {
                open_gap(&mut row, &mut g, &mut compensator, 2.0 - t, 1.4);
                row = row_times(&row, &weighted_transition_matrix(generator, 0.3, 10.0).unwrap());
                g *= (-beta * 10.0f64).exp();
                t = 12.0;
            }
            open_gap(&mut row, &mut g, &mut compensator, time - t, s);
            t = time;
            match obs_index {
                None => {
                    for (k, v) in row.iter_mut().enumerate() {
                        *v *= [0.5, 2.0][k] * s + g;
                    }
                    g += n * beta;
                }
                Some(i) => {
                    let st = as_step(obs[i].1, obs[i].2);
                    for (k, v) in row.iter_mut().enumerate() {
                        *v *= joint.observation.log_emission(k, &st).exp();
                    }
                }
            }
        }
        open_gap(&mut row, &mut g, &mut compensator, 15.0 - t, 0.6);
        let direct = row.iter().sum::<f64>().ln() - compensator;
        let ll = joint.log_likelihood(&window).unwrap();
        assert!((ll - direct).abs() < 1e-10, "{ll} vs {direct}");
    }

    /// Synthetic calendar: `days` repetitions of [open `open_len` split into the buckets, closed
    /// `closed_len`].
    fn calendar(days: usize, open_len: f64, closed_len: f64, buckets: usize) -> Vec<Piece> {
        let mut t = Vec::new();
        for _ in 0..days {
            for b in 0..buckets {
                t.push(Piece::Open { duration: open_len / buckets as f64, bucket: b });
            }
            t.push(Piece::Closed { duration: closed_len });
        }
        t
    }

    /// Exact simulation on a timeline: regimes switch at Q while open and at w Q while closed;
    /// trades arrive while open at mu_k s_b + g (thinning); the echo decays in calendar time.
    /// Observations at every `every`-th trade.
    fn simulate(truth: &JointRegimeModel<2, 2>, timeline: &[Piece], every: usize, seed: u64) -> JointWindow<2> {
        let mut rng = SplitMix64::new(seed);
        let obs = &truth.observation;
        let q = obs.generator.matrix();
        let mut state = if rng.next_f64() < obs.initial[0] { 0 } else { 1 };
        let (mut t, mut g) = (0.0, 0.0);
        let mut trades = Vec::new();
        let mut states_at_obs = Vec::new();
        let mut obs_times = Vec::new();
        for piece in timeline {
            let end = t + piece.duration();
            let (rate_mult, bucket) = match *piece {
                Piece::Open { bucket, .. } => (1.0, Some(bucket)),
                Piece::Closed { .. } => (truth.closed_weight, None),
            };
            while t < end {
                let exit = -q[state][state] * rate_mult;
                let arrival_bound = bucket.map_or(0.0, |b| truth.baseline[state] * truth.seasonal[b] + g);
                let total = exit + arrival_bound;
                let wait = if total > 0.0 { -(1.0 - rng.next_f64()).ln() / total } else { f64::INFINITY };
                if t + wait >= end {
                    g *= (-truth.decay * (end - t)).exp();
                    t = end;
                    break;
                }
                t += wait;
                g *= (-truth.decay * wait).exp();
                let u = rng.next_f64() * total;
                if u < exit {
                    state = 1 - state;
                } else if let Some(b) = bucket
                    && u - exit < truth.baseline[state] * truth.seasonal[b] + g
                {
                    trades.push(t);
                    g += truth.branching_ratio * truth.decay;
                    if every > 0 && trades.len() % every == 0 {
                        obs_times.push(t);
                        states_at_obs.push(state);
                    }
                }
            }
        }
        let xs = simulate_emissions(&states_at_obs, &obs.means, &obs.covars, &obs.dof, &mut rng);
        let observations: Vec<(f64, [f64; 2], [bool; 2])> = obs_times.iter().zip(xs).map(|(&t, x)| (t, x, [rng.next_f64() > 0.2, rng.next_f64() > 0.2])).collect();
        JointWindow::new(&trades, &observations, timeline.to_vec())
    }

    fn truth() -> JointRegimeModel<2, 2> {
        JointRegimeModel { observation: observation_model(), baseline: [0.3, 2.0], seasonal: vec![1.6, 0.5, 0.9], closed_weight: 0.1, branching_ratio: 0.5, decay: 2.0 }
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
        JointRegimeModel { observation, baseline: [0.5, 1.5], seasonal: vec![1.0; 3], closed_weight: 0.5, branching_ratio: 0.3, decay: 1.0 }
    }

    #[test]
    fn fit_stops_on_an_invalid_present_input() {
        let mut window = simulate(&truth(), &calendar(30, 100.0, 40.0, 3), 20, 91);
        let p = window.points.iter().position(|pt| matches!(pt, Point::Observation { .. })).expect("an observation");
        if let Point::Observation { values, present, .. } = &mut window.points[p] {
            values[0] = f64::NAN;
            present[0] = true;
        }
        let settings = JointFitSettings { max_iterations: 2, tolerance: f64::NEG_INFINITY, estimate_dof: true };
        match fit(&start(), &[window], settings) {
            Err(JointError::InvalidInput { window: 0, point, input: 0 }) => assert_eq!(point, p),
            other => panic!("expected InvalidInput, got {:?}", other.map(|_| ())),
        }
    }

    #[test]
    fn em_never_decreases_the_likelihood() {
        let window = simulate(&truth(), &calendar(30, 100.0, 40.0, 3), 20, 91);
        let (_, report) = fit(&start(), &[window], JointFitSettings { max_iterations: 20, tolerance: f64::NEG_INFINITY, estimate_dof: true }).unwrap();
        for w in report.log_likelihoods.windows(2) {
            assert!(w[1] >= w[0] - 1e-7 * w[0].abs(), "decrease {} -> {}", w[0], w[1]);
        }
    }

    /// The full known-answer test: open/closed calendar with three time-of-day buckets, closed-time
    /// weight 0.1, regimes, echo and Student-t inputs with 20% absent.
    #[test]
    fn recovers_market_time_seasonality_and_every_parameter() {
        let t = truth();
        let window = simulate(&t, &calendar(150, 100.0, 40.0, 3), 20, 92);
        let (f, report) = fit(&start(), &[window], JointFitSettings { max_iterations: 400, tolerance: 1e-6, estimate_dof: true }).unwrap();
        assert!(report.converged);
        let (qt, qf) = (t.observation.generator.matrix(), f.observation.generator.matrix());
        assert!((qf[0][1] / qt[0][1] - 1.0).abs() < 0.35, "q01 {}", qf[0][1]);
        assert!((qf[1][0] / qt[1][0] - 1.0).abs() < 0.35, "q10 {}", qf[1][0]);
        assert!((f.closed_weight - 0.1).abs() < 0.1, "closed weight {}", f.closed_weight);
        // Seasonal factors as truth normalized the same way (open time is equal across buckets).
        let mean_truth: f64 = t.seasonal.iter().sum::<f64>() / 3.0;
        for b in 0..3 {
            let truth_b = t.seasonal[b] / mean_truth;
            assert!((f.seasonal[b] / truth_b - 1.0).abs() < 0.08, "s[{b}] {} vs {truth_b}", f.seasonal[b]);
        }
        for k in 0..2 {
            let truth_k = t.baseline[k] * mean_truth;
            assert!((f.baseline[k] / truth_k - 1.0).abs() < 0.12, "mu[{k}] {} vs {truth_k}", f.baseline[k]);
            for d in 0..2 {
                assert!((f.observation.means[k][d] - t.observation.means[k][d]).abs() < 0.15, "mean[{k}][{d}]");
            }
        }
        assert!((f.branching_ratio - 0.5).abs() < 0.05, "n {}", f.branching_ratio);
        assert!((f.decay / 2.0 - 1.0).abs() < 0.15, "beta {}", f.decay);
    }

    /// The decisive test: data with ONE regime and a strong daily rhythm. Without the time-of-day
    /// factor the model invents two regimes (busy vs quiet hours); with it, the regimes collapse.
    #[test]
    fn without_the_daily_rhythm_regimes_learn_the_clock() {
        let mut truth = truth();
        truth.baseline = [1.0, 1.0]; // identical regimes: effectively one
        truth.seasonal = vec![3.0, 0.3, 1.0];
        truth.branching_ratio = 0.0;
        let window = simulate(&truth, &calendar(100, 100.0, 40.0, 3), 0, 93);
        let settings = JointFitSettings { max_iterations: 300, tolerance: 1e-6, estimate_dof: false };
        let mut no_rhythm = start();
        no_rhythm.branching_ratio = 0.0;
        no_rhythm.seasonal = vec![1.0];
        // One bucket: relabel every open piece as bucket 0.
        let mut one_bucket = window.clone();
        for p in &mut one_bucket.timeline {
            if let Piece::Open { bucket, .. } = p {
                *bucket = 0;
            }
        }
        let (fitted_plain, _) = fit(&no_rhythm, &[one_bucket], settings).unwrap();
        let spread_plain = fitted_plain.baseline[1].max(fitted_plain.baseline[0]) / fitted_plain.baseline[1].min(fitted_plain.baseline[0]);
        let mut with_rhythm = start();
        with_rhythm.branching_ratio = 0.0;
        let (fitted, _) = fit(&with_rhythm, &[window], settings).unwrap();
        let spread = fitted.baseline[1].max(fitted.baseline[0]) / fitted.baseline[1].min(fitted.baseline[0]);
        assert!(spread_plain > 2.0, "without the rhythm the regimes should split: ratio {spread_plain}");
        assert!(spread < 1.2, "with the rhythm the regimes should coincide: ratio {spread}");
    }
}
