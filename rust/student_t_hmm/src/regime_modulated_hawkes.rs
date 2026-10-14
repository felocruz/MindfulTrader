//! regime_modulated_hawkes.rs -- trade arrivals driven by a hidden regime AND by self-excitation
//! (rust-migration spec §6.2.8, D4 companion "Hawkes research": Model A).
//!
//! Arrival rate in regime k at time t:  lambda_k(t) = mu_k + g(t),
//!   g(t) = sum over earlier events t_j of  n * beta * e^{-beta (t - t_j)}.
//! mu_k is the regime's background rate (exogenous flow); g is ONE shared exponential echo of past
//! events (endogenous clustering); n is the branching ratio (expected events triggered per event),
//! beta the echo's decay rate. Regimes switch by a generator Q in continuous time.
//!
//! Why this form first: g depends only on OBSERVED event times, never on the hidden regime, so the
//! likelihood is exact (a derivation recorded in the spec, not found published -- accepted here on
//! known-answer tests). Between events no event occurs: factor e^{(Q - diag mu) dt} (the Markov-
//! modulated Poisson process construction, LaMa `tpm_ct(Q, timediff, rates)`); at an event:
//! diag(mu + g(t-)); the shared part e^{-int g} factors out of every regime path:
//!   L = initial * prod_i [ e^{(Q - diag mu) dt_i} diag(mu + g_i) ] * e^{(Q - diag mu)(T - t_N)} 1
//!       * exp(-int_0^T g),     int_0^T g = n sum_j (1 - e^{-beta (T - t_j)}).
//! Modelling regimes and excitation together is the remedy for the "false criticality" that a plain
//! Hawkes fit shows when the background rate shifts between regimes (Filimonov & Sornette 2015).
//!
//! Training (EM): regime posteriors at events by a scaled forward-backward over the gaps;
//! expected regime occupancy and switches from `StepStatistics::with_event_rates`; each event is an
//! "immigrant" (background) with probability mu_k / (mu_k + g) in regime k or an "offspring" of an
//! earlier event otherwise (the branching representation, Veen & Schoenberg 2008). Updates:
//! mu_k = expected immigrants in k / expected time in k; q_kl = expected switches k->l / time in k;
//! n and beta maximize the expected complete-data log-likelihood of the offspring
//! (O ln(n beta) - beta * expected parent lags - n sum_j (1 - e^{-beta (T - t_j)})), n in closed form
//! given beta and beta by golden section on a log scale.
//!
//! Exact simulation: the regime path from `regime_path_simulator`, events by Ogata (1981) thinning --
//! within a regime segment the intensity only decays between events, so mu_k + g(t) bounds it until
//! the next event or regime switch.
//!
//! Not handled here (recorded as open): trades sharing a timestamp (to be treated as one event with
//! a size mark at the data stage), power-law decay (a sum of exponentials keeps this form exact),
//! regime-dependent excitation (no longer exact).

use crate::elapsed_time_transitions::{ElapsedTimeError, StepStatistics, no_event_matrix};
use crate::init::SplitMix64;
use crate::regime_path_simulator::{Generator, GeneratorError, simulate_path};

#[derive(Debug, Clone, PartialEq)]
pub struct RegimeHawkes<const K: usize> {
    pub generator: Generator<K>,
    pub initial: [f64; K],
    /// Background event rate in each regime (mu_k >= 0).
    pub baseline: [f64; K],
    /// Expected events triggered per event (n in [0, 1) for a stationary process).
    pub branching_ratio: f64,
    /// Decay rate of the echo (beta > 0).
    pub decay: f64,
}

#[derive(Debug, Clone, PartialEq)]
pub enum HawkesError {
    ElapsedTime(ElapsedTimeError),
    Generator(GeneratorError),
    /// Event times must be ascending and inside [0, horizon].
    EventTimes,
    /// A regime received no expected time, so its parameters cannot be updated.
    EmptyRegime(usize),
    NoStationaryDistribution,
}

impl std::fmt::Display for HawkesError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::ElapsedTime(e) => write!(f, "{e}"),
            Self::Generator(e) => write!(f, "{e}"),
            Self::EventTimes => write!(f, "event times must be ascending and inside [0, horizon]"),
            Self::EmptyRegime(k) => write!(f, "regime {k} received no expected time"),
            Self::NoStationaryDistribution => write!(f, "generator has no unique stationary distribution"),
        }
    }
}

impl std::error::Error for HawkesError {}

impl From<ElapsedTimeError> for HawkesError {
    fn from(e: ElapsedTimeError) -> Self {
        Self::ElapsedTime(e)
    }
}

impl From<GeneratorError> for HawkesError {
    fn from(e: GeneratorError) -> Self {
        Self::Generator(e)
    }
}

/// One observation window: ascending event times in [0, horizon].
#[derive(Debug, Clone, PartialEq)]
pub struct EventWindow {
    pub times: Vec<f64>,
    pub horizon: f64,
}

fn check_window(w: &EventWindow) -> Result<(), HawkesError> {
    let ascending = w.times.windows(2).all(|p| p[0] <= p[1]);
    let inside = w.times.first().is_none_or(|&t| t >= 0.0) && w.times.last().is_none_or(|&t| t <= w.horizon);
    if ascending && inside && w.horizon.is_finite() { Ok(()) } else { Err(HawkesError::EventTimes) }
}

/// Excitation just before each event: g_i = e^{-beta dt} (g_{i-1} + n beta), g before the first = 0.
fn excitation_before_events(times: &[f64], n: f64, beta: f64) -> Vec<f64> {
    let mut g = Vec::with_capacity(times.len());
    let mut current = 0.0;
    let mut prev: Option<f64> = None;
    for &t in times {
        if let Some(p) = prev {
            current = (-beta * (t - p)).exp() * (current + n * beta);
        }
        g.push(current);
        prev = Some(t);
    }
    g
}

/// int_0^T g(s) ds = n sum_j (1 - e^{-beta (T - t_j)}).
fn excitation_integral(times: &[f64], horizon: f64, n: f64, beta: f64) -> f64 {
    n * times.iter().map(|&t| 1.0 - (-beta * (horizon - t)).exp()).sum::<f64>()
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

/// Forward pass products shared by the likelihood and the E-step.
struct Forward<const K: usize> {
    /// Gap factors: gaps[i] precedes event i; gaps[N] runs from the last event to the horizon.
    gaps: Vec<[[f64; K]; K]>,
    /// Event factors mu_k + g_i.
    event_factors: Vec<[f64; K]>,
    /// Normalized forward vectors after each event (alpha[0] = initial, alpha[i+1] after event i).
    alpha: Vec<[f64; K]>,
    /// Normalizers c_i of each event, and of the end gap last.
    scales: Vec<f64>,
    log_likelihood: f64,
    excitation: Vec<f64>,
}

impl<const K: usize> RegimeHawkes<K> {
    fn forward(&self, window: &EventWindow) -> Result<Forward<K>, HawkesError> {
        check_window(window)?;
        let times = &window.times;
        let excitation = excitation_before_events(times, self.branching_ratio, self.decay);
        let mut gaps = Vec::with_capacity(times.len() + 1);
        let mut prev = 0.0;
        for &t in times {
            gaps.push(no_event_matrix(&self.generator, &self.baseline, t - prev)?);
            prev = t;
        }
        gaps.push(no_event_matrix(&self.generator, &self.baseline, window.horizon - prev)?);
        let event_factors: Vec<[f64; K]> = excitation.iter().map(|&g| self.baseline.map(|mu| mu + g)).collect();

        let mut alpha = Vec::with_capacity(times.len() + 1);
        alpha.push(self.initial);
        let mut scales = Vec::with_capacity(times.len() + 1);
        let mut log_likelihood = 0.0;
        for (gap, e) in gaps.iter().zip(&event_factors) {
            let mut u = row_times(&alpha[alpha.len() - 1], gap);
            for (v, &f) in u.iter_mut().zip(e) {
                *v *= f;
            }
            let c: f64 = u.iter().sum();
            log_likelihood += c.ln();
            scales.push(c);
            alpha.push(u.map(|v| v / c));
        }
        let end = row_times(&alpha[alpha.len() - 1], &gaps[gaps.len() - 1]);
        let c_end: f64 = end.iter().sum();
        log_likelihood += c_end.ln();
        scales.push(c_end);
        log_likelihood -= excitation_integral(times, window.horizon, self.branching_ratio, self.decay);
        Ok(Forward { gaps, event_factors, alpha, scales, log_likelihood, excitation })
    }

    /// Exact log-likelihood of one window.
    pub fn log_likelihood(&self, window: &EventWindow) -> Result<f64, HawkesError> {
        Ok(self.forward(window)?.log_likelihood)
    }

    /// Regime probabilities just after each event given the events up to it.
    pub fn filtered_regimes(&self, window: &EventWindow) -> Result<Vec<[f64; K]>, HawkesError> {
        Ok(self.forward(window)?.alpha.split_off(1))
    }
}

/// Exact simulation on [0, horizon]: the regime path and the event times.
pub fn simulate<const K: usize>(model: &RegimeHawkes<K>, horizon: f64, rng: &mut SplitMix64) -> (Vec<usize>, Vec<f64>, EventWindow) {
    let path = simulate_path(&model.generator, &model.initial, horizon, rng);
    let jump_amount = model.branching_ratio * model.decay;
    let mut events = Vec::new();
    let mut regimes_at_events = Vec::new();
    let mut t = 0.0;
    let mut g = 0.0;
    for (seg, &state) in path.states.iter().enumerate() {
        let seg_end = path.jump_times.get(seg + 1).copied().unwrap_or(horizon);
        loop {
            let bound = model.baseline[state] + g;
            let wait = if bound > 0.0 { -(1.0 - rng.next_f64()).ln() / bound } else { f64::INFINITY };
            if t + wait >= seg_end {
                g *= (-model.decay * (seg_end - t)).exp();
                t = seg_end;
                break;
            }
            t += wait;
            g *= (-model.decay * wait).exp();
            if rng.next_f64() * bound <= model.baseline[state] + g {
                events.push(t);
                regimes_at_events.push(state);
                g += jump_amount;
            }
        }
    }
    (regimes_at_events, events.clone(), EventWindow { times: events, horizon })
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct HawkesFitSettings {
    pub max_iterations: usize,
    /// Stop when the log-likelihood improves by less than this (absolute).
    pub tolerance: f64,
}

impl Default for HawkesFitSettings {
    fn default() -> Self {
        Self { max_iterations: 500, tolerance: 1e-6 }
    }
}

#[derive(Debug, Clone, PartialEq)]
pub struct HawkesFitReport {
    pub log_likelihoods: Vec<f64>,
    pub converged: bool,
}

/// Golden-section maximization on [lo, hi] (unimodal objective).
fn golden_max(f: impl Fn(f64) -> f64, mut lo: f64, mut hi: f64) -> f64 {
    let ratio = (5f64.sqrt() - 1.0) / 2.0;
    let mut x1 = hi - ratio * (hi - lo);
    let mut x2 = lo + ratio * (hi - lo);
    let (mut f1, mut f2) = (f(x1), f(x2));
    for _ in 0..100 {
        if (hi - lo) < 1e-9 * (1.0 + lo.abs()) {
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

/// Sufficient statistics of one E-step, summed over windows.
struct Expectations<const K: usize> {
    occupancy: [f64; K],
    switches: [[f64; K]; K],
    immigrants: [f64; K],
    offspring: f64,
    /// sum_i P(offspring_i) * E[lag to parent | offspring_i]
    offspring_lag: f64,
    log_likelihood: f64,
}

fn e_step<const K: usize>(model: &RegimeHawkes<K>, windows: &[EventWindow]) -> Result<Expectations<K>, HawkesError> {
    let mut stats = StepStatistics::with_event_rates(&model.generator, &model.baseline);
    let mut ex = Expectations { occupancy: [0.0; K], switches: [[0.0; K]; K], immigrants: [0.0; K], offspring: 0.0, offspring_lag: 0.0, log_likelihood: 0.0 };
    for window in windows {
        let fw = model.forward(window)?;
        ex.log_likelihood += fw.log_likelihood;
        let n_events = window.times.len();
        // Backward: beta_end = gaps[N] 1 / c_end; beta_{i-1} = gaps[i] (e_i * beta_i) / c_i.
        let c_end = fw.scales[n_events];
        let last_gap = &fw.gaps[n_events];
        let mut beta: [f64; K] = std::array::from_fn(|k| last_gap[k].iter().sum::<f64>() / c_end);
        // End gap two-slice weights (w = xi / N): alpha_N(k) / c_end for every end regime.
        let a_last = fw.alpha[n_events];
        let w_end: [[f64; K]; K] = std::array::from_fn(|k| [a_last[k] / c_end; K]);
        stats.accumulate(window.horizon - window.times.last().copied().unwrap_or(0.0), &w_end)?;
        // Mean lag to parent: S0 = sum_j e^{-beta(t_i - t_j)}, S1 = sum_j (t_i - t_j) e^{...}.
        let mut lags = vec![0.0; n_events];
        {
            let (mut s0, mut s1) = (0.0f64, 0.0f64);
            for (lag, pair) in lags.iter_mut().skip(1).zip(window.times.windows(2)) {
                let dt = pair[1] - pair[0];
                let decay = (-model.decay * dt).exp();
                let a0 = s0 + 1.0;
                let a1 = s1;
                s0 = decay * a0;
                s1 = decay * (a1 + dt * a0);
                *lag = if s0 > 0.0 { s1 / s0 } else { 0.0 };
            }
        }
        for i in (0..n_events).rev() {
            let alpha_i = fw.alpha[i + 1]; // after event i
            let gamma: [f64; K] = std::array::from_fn(|k| alpha_i[k] * beta[k]);
            let s: f64 = gamma.iter().sum();
            let gamma = gamma.map(|v| v / s);
            let g = fw.excitation[i];
            for (k, (&gamma_k, &rate)) in gamma.iter().zip(&fw.event_factors[i]).enumerate() {
                if rate > 0.0 {
                    let p_imm = model.baseline[k] / rate;
                    ex.immigrants[k] += gamma_k * p_imm;
                    let p_off = gamma_k * g / rate;
                    ex.offspring += p_off;
                    ex.offspring_lag += p_off * lags[i];
                }
            }
            // Gap before event i: w = alpha_{i-1}(k) e_i(l) beta_i(l) / c_i.
            let alpha_prev = fw.alpha[i];
            let c = fw.scales[i];
            let e = fw.event_factors[i];
            let w: [[f64; K]; K] = std::array::from_fn(|k| std::array::from_fn(|l| alpha_prev[k] * e[l] * beta[l] / c));
            let gap_len = window.times[i] - if i == 0 { 0.0 } else { window.times[i - 1] };
            stats.accumulate(gap_len, &w)?;
            // beta_{i-1}
            let gap = &fw.gaps[i];
            beta = std::array::from_fn(|k| (0..K).map(|l| gap[k][l] * e[l] * beta[l]).sum::<f64>() / c);
        }
    }
    ex.occupancy = stats.occupancy;
    ex.switches = stats.jumps;
    Ok(ex)
}

/// Fits by EM from `start`. Every window starts with no excitation (no history before 0).
pub fn fit<const K: usize>(start: &RegimeHawkes<K>, windows: &[EventWindow], settings: HawkesFitSettings) -> Result<(RegimeHawkes<K>, HawkesFitReport), HawkesError> {
    let mut model = start.clone();
    let mut log_likelihoods = Vec::new();
    let mut converged = false;
    for _ in 0..settings.max_iterations {
        let ex = e_step(&model, windows)?;
        if let Some(&prev) = log_likelihoods.last()
            && ex.log_likelihood - prev < settings.tolerance
        {
            log_likelihoods.push(ex.log_likelihood);
            converged = true;
            break;
        }
        log_likelihoods.push(ex.log_likelihood);

        let mut q = [[0.0; K]; K];
        let mut baseline = [0.0; K];
        for (k, q_row) in q.iter_mut().enumerate() {
            let time = ex.occupancy[k];
            if time <= 0.0 {
                return Err(HawkesError::EmptyRegime(k));
            }
            baseline[k] = ex.immigrants[k] / time;
            let mut exit = 0.0;
            for l in (0..K).filter(|&l| l != k) {
                q_row[l] = ex.switches[k][l] / time;
                exit += q_row[l];
            }
            q_row[k] = -exit;
        }
        let generator = Generator::new(q)?;

        // Excitation: n = O / C(beta), beta maximizing O ln(n beta) - beta * lag_sum - n C(beta).
        let (branching_ratio, decay) = if ex.offspring > 1e-12 {
            let budget = |beta: f64| windows.iter().map(|w| w.times.iter().map(|&t| 1.0 - (-beta * (w.horizon - t)).exp()).sum::<f64>()).sum::<f64>();
            let objective = |log_beta: f64| {
                let beta = log_beta.exp();
                let c = budget(beta);
                let n = ex.offspring / c;
                ex.offspring * (n * beta).ln() - beta * ex.offspring_lag - n * c
            };
            let centre = model.decay.ln();
            let log_beta = golden_max(objective, centre - 5.0, centre + 5.0);
            let beta = log_beta.exp();
            (ex.offspring / budget(beta), beta)
        } else {
            (0.0, model.decay)
        };

        let initial = generator.stationary_distribution().ok_or(HawkesError::NoStationaryDistribution)?;
        model = RegimeHawkes { generator, initial, baseline, branching_ratio, decay };
    }
    Ok((model, HawkesFitReport { log_likelihoods, converged }))
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::elapsed_time_transitions::transition_matrix;

    fn two_regimes() -> Generator<2> {
        Generator::new([[-0.01, 0.01], [0.02, -0.02]]).unwrap()
    }

    #[test]
    fn no_event_matrix_known_answers() {
        let g = two_regimes();
        // Zero rates: plain transition probabilities.
        let p = transition_matrix(&g, 3.0).unwrap();
        let n0 = no_event_matrix(&g, &[0.0, 0.0], 3.0).unwrap();
        assert!(p.iter().flatten().zip(n0.iter().flatten()).all(|(a, b)| (a - b).abs() < 1e-14));
        // Equal rates r: diag(r) commutes with Q, so N(d) = e^{-r d} P(d).
        let r = 0.7;
        let nr = no_event_matrix(&g, &[r, r], 3.0).unwrap();
        assert!(p.iter().flatten().zip(nr.iter().flatten()).all(|(a, b)| (a * (-r * 3.0f64).exp() - b).abs() < 1e-14));
        // Rows sum to P(no event) < 1.
        let nm = no_event_matrix(&g, &[0.5, 3.0], 2.0).unwrap();
        assert!(nm.iter().all(|row| row.iter().all(|&v| v >= 0.0) && row.iter().sum::<f64>() < 1.0));
    }

    /// Independent check by simulation: with no self-excitation, the probability of no event in
    /// (0, d] and regime j at d, starting from regime i, matches N(d).
    #[test]
    fn no_event_matrix_matches_simulation() {
        let g = Generator::new([[-0.4, 0.4], [0.9, -0.9]]).unwrap();
        let model = RegimeHawkes { generator: g.clone(), initial: [1.0, 0.0], baseline: [0.3, 2.0], branching_ratio: 0.0, decay: 1.0 };
        let d = 1.5;
        let nm = no_event_matrix(&g, &model.baseline, d).unwrap();
        let mut rng = SplitMix64::new(5);
        for (start, nm_row) in nm.iter().enumerate() {
            let mut m = model.clone();
            m.initial = if start == 0 { [1.0, 0.0] } else { [0.0, 1.0] };
            let n = 200_000;
            let mut counts = [0usize; 2];
            for _ in 0..n {
                let path = simulate_path(&m.generator, &m.initial, d + 1.0, &mut rng);
                let (_, events, _) = simulate_from_path(&m, &path, d, &mut rng);
                if events.is_empty() {
                    counts[path.state_at(d)] += 1;
                }
            }
            for (j, (&count, &expected)) in counts.iter().zip(nm_row).enumerate() {
                let freq = count as f64 / n as f64;
                let se = (expected * (1.0 - expected) / n as f64).sqrt();
                assert!((freq - expected).abs() < 5.0 * se, "{start}->{j}: {freq} vs {expected}");
            }
        }
    }

    /// Thinning on a given regime path (test helper so a path can be shared).
    fn simulate_from_path<const K: usize>(model: &RegimeHawkes<K>, path: &crate::regime_path_simulator::CtPath, horizon: f64, rng: &mut SplitMix64) -> (Vec<usize>, Vec<f64>, EventWindow) {
        let mut events = Vec::new();
        let mut regimes = Vec::new();
        let mut t = 0.0;
        let mut g = 0.0;
        for (seg, &state) in path.states.iter().enumerate() {
            let seg_end = path.jump_times.get(seg + 1).copied().unwrap_or(f64::INFINITY).min(horizon);
            loop {
                let bound = model.baseline[state] + g;
                let wait = if bound > 0.0 { -(1.0 - rng.next_f64()).ln() / bound } else { f64::INFINITY };
                if t + wait >= seg_end {
                    g *= (-model.decay * (seg_end - t)).exp();
                    t = seg_end;
                    break;
                }
                t += wait;
                g *= (-model.decay * wait).exp();
                if rng.next_f64() * bound <= model.baseline[state] + g {
                    events.push(t);
                    regimes.push(state);
                    g += model.branching_ratio * model.decay;
                }
            }
            if t >= horizon {
                break;
            }
        }
        (regimes, events.clone(), EventWindow { times: events, horizon })
    }

    /// With equal background rates the regime is irrelevant: the likelihood is the textbook
    /// Hawkes log-likelihood  sum ln(mu + g_i) - mu T - int g,  whatever Q is.
    #[test]
    fn equal_baselines_give_the_textbook_hawkes_likelihood() {
        let times = vec![0.3, 0.35, 1.2, 4.0, 4.01, 4.02, 9.5];
        let window = EventWindow { times: times.clone(), horizon: 12.0 };
        let (mu, n, beta) = (0.8, 0.6, 3.0);
        let model = RegimeHawkes { generator: two_regimes(), initial: [0.3, 0.7], baseline: [mu, mu], branching_ratio: n, decay: beta };
        let g = excitation_before_events(&times, n, beta);
        // Brute-force excitation: sum over earlier events.
        for (i, &gi) in g.iter().enumerate() {
            let brute: f64 = times[..i].iter().map(|&tj| n * beta * (-beta * (times[i] - tj)).exp()).sum();
            assert!((gi - brute).abs() < 1e-12);
        }
        let textbook = g.iter().map(|gi| (mu + gi).ln()).sum::<f64>() - mu * 12.0 - excitation_integral(&times, 12.0, n, beta);
        assert!((model.log_likelihood(&window).unwrap() - textbook).abs() < 1e-11);
    }

    /// Event-by-event, the forward pass equals the direct matrix product (no scaling).
    #[test]
    fn scaled_forward_equals_the_direct_product() {
        let model = RegimeHawkes { generator: two_regimes(), initial: [0.6, 0.4], baseline: [0.2, 2.5], branching_ratio: 0.4, decay: 2.0 };
        let times = vec![0.5, 0.6, 2.0, 2.05, 7.0];
        let horizon = 10.0;
        let g = excitation_before_events(&times, 0.4, 2.0);
        let mut row = model.initial;
        let mut prev = 0.0;
        for (&t, &gi) in times.iter().zip(&g) {
            row = row_times(&row, &no_event_matrix(&model.generator, &model.baseline, t - prev).unwrap());
            for (v, &mu) in row.iter_mut().zip(&model.baseline) {
                *v *= mu + gi;
            }
            prev = t;
        }
        let end = row_times(&row, &no_event_matrix(&model.generator, &model.baseline, horizon - prev).unwrap());
        let direct = end.iter().sum::<f64>().ln() - excitation_integral(&times, horizon, 0.4, 2.0);
        let ll = model.log_likelihood(&EventWindow { times, horizon }).unwrap();
        assert!((ll - direct).abs() < 1e-12, "{ll} vs {direct}");
    }

    fn truth() -> RegimeHawkes<2> {
        let generator = Generator::new([[-0.004, 0.004], [0.008, -0.008]]).unwrap();
        let initial = generator.stationary_distribution().unwrap();
        RegimeHawkes { generator, initial, baseline: [0.3, 2.0], branching_ratio: 0.5, decay: 2.0 }
    }

    fn start() -> RegimeHawkes<2> {
        let generator = Generator::new([[-0.01, 0.01], [0.01, -0.01]]).unwrap();
        RegimeHawkes { generator, initial: [0.5, 0.5], baseline: [0.5, 1.5], branching_ratio: 0.3, decay: 1.0 }
    }

    #[test]
    fn em_never_decreases_the_likelihood() {
        let t = truth();
        let (_, _, window) = simulate(&t, 5_000.0, &mut SplitMix64::new(71));
        let (_, report) = fit(&start(), &[window], HawkesFitSettings { max_iterations: 30, tolerance: f64::NEG_INFINITY }).unwrap();
        for w in report.log_likelihoods.windows(2) {
            assert!(w[1] >= w[0] - 1e-7 * w[0].abs(), "decrease {} -> {}", w[0], w[1]);
        }
    }

    /// Known answer: regimes with background rates 0.3 and 2.0, branching ratio 0.5, decay 2.0.
    #[test]
    fn recovers_regimes_and_excitation_together() {
        let t = truth();
        let (_, _, window) = simulate(&t, 20_000.0, &mut SplitMix64::new(72));
        let (fitted, report) = fit(&start(), &[window], HawkesFitSettings { max_iterations: 400, tolerance: 1e-6 }).unwrap();
        assert!(report.converged);
        for k in 0..2 {
            assert!((fitted.baseline[k] / t.baseline[k] - 1.0).abs() < 0.1, "mu[{k}] {}", fitted.baseline[k]);
        }
        let (qt, qf) = (t.generator.matrix(), fitted.generator.matrix());
        assert!((qf[0][1] / qt[0][1] - 1.0).abs() < 0.3, "q01 {}", qf[0][1]);
        assert!((qf[1][0] / qt[1][0] - 1.0).abs() < 0.3, "q10 {}", qf[1][0]);
        assert!((fitted.branching_ratio - 0.5).abs() < 0.04, "n {}", fitted.branching_ratio);
        assert!((fitted.decay / 2.0 - 1.0).abs() < 0.12, "beta {}", fitted.decay);
    }

    /// The false-criticality guard (Filimonov & Sornette 2015): data with regime shifts and NO
    /// self-excitation must not be read as near-critical. Fit from a start of n = 0.5.
    #[test]
    fn regime_shifts_without_excitation_give_branching_ratio_near_zero() {
        let mut t = truth();
        t.branching_ratio = 0.0;
        let (_, _, window) = simulate(&t, 20_000.0, &mut SplitMix64::new(73));
        let mut s = start();
        s.branching_ratio = 0.5;
        let (fitted, _) = fit(&s, &[window], HawkesFitSettings { max_iterations: 400, tolerance: 1e-6 }).unwrap();
        assert!(fitted.branching_ratio < 0.03, "n {}", fitted.branching_ratio);
        for k in 0..2 {
            assert!((fitted.baseline[k] / t.baseline[k] - 1.0).abs() < 0.1, "mu[{k}] {}", fitted.baseline[k]);
        }
    }
}
