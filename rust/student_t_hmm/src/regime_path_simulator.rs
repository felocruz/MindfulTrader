//! regime_path_simulator.rs -- exact simulation of a continuous-time regime process with Student-t observations
//! (rust-migration spec §6.2.8, D4 rulings 1-2: the regime model becomes a continuous-time
//! Student-t HMM stepping at irregular times; stage 1 of its build).
//!
//! Purpose: known-answer tests. Every later stage (the transition matrix for a step of length d,
//! filtering with step lengths, rate estimation by EM) is accepted by simulating from KNOWN
//! switching rates and checking the answer -- never by fitting today's market data.
//!
//! Independence: the path is simulated event by event (Gillespie 1977): the process stays in
//! state i for an exponential holding time with rate q_i = -Q[i][i], then jumps to j != i with
//! probability Q[i][j] / q_i. This never computes a matrix exponential, so it is an independent
//! check of the expm(Q d) computation built in stage 2.
//!
//! Observations: at caller-chosen (irregular) times the state is read off the path and a
//! diagonal-scale multivariate Student-t vector is drawn -- the same construction as
//! `bootstrap.rs` and `model.rs`'s density: one chi-square factor W ~ Chi2(nu) shared by every
//! dimension, x_d = mean_d + sqrt(var_d) * Z_d * sqrt(nu / W). The shared factor is what makes the
//! tails joint (all dimensions are large together), unlike independent per-dimension draws.
//!
//! Time is in the model's own step-length unit (whatever d measures once the step length is
//! defined); this module does not assume seconds.

use crate::bootstrap::{chi_square_sample, standard_normal};
use crate::init::SplitMix64;

/// Tolerance on a generator row sum (exactly 0 in exact arithmetic).
const ROW_SUM_TOL: f64 = 1e-9;

/// Why a matrix is not a valid generator for this simulator.
#[derive(Debug, Clone, PartialEq)]
pub enum GeneratorError {
    /// An entry is NaN or infinite.
    NotFinite { row: usize, col: usize },
    /// An off-diagonal rate is negative.
    NegativeRate { row: usize, col: usize },
    /// A row does not sum to zero.
    RowSum { row: usize, sum: f64 },
    /// A state can never be left (zero exit rate). Regimes are not absorbing in this model.
    Absorbing { row: usize },
}

impl std::fmt::Display for GeneratorError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::NotFinite { row, col } => write!(f, "generator entry [{row}][{col}] is not finite"),
            Self::NegativeRate { row, col } => write!(f, "generator off-diagonal [{row}][{col}] is negative"),
            Self::RowSum { row, sum } => write!(f, "generator row {row} sums to {sum}, not 0"),
            Self::Absorbing { row } => write!(f, "state {row} has zero exit rate (absorbing)"),
        }
    }
}

impl std::error::Error for GeneratorError {}

/// A validated continuous-time Markov generator: off-diagonal rates >= 0, each row summing to 0,
/// every state with a positive exit rate.
#[derive(Debug, Clone, PartialEq)]
pub struct Generator<const K: usize> {
    q: [[f64; K]; K],
}

impl<const K: usize> Generator<K> {
    pub fn new(q: [[f64; K]; K]) -> Result<Self, GeneratorError> {
        for (i, row) in q.iter().enumerate() {
            let mut sum = 0.0;
            for (j, &v) in row.iter().enumerate() {
                if !v.is_finite() {
                    return Err(GeneratorError::NotFinite { row: i, col: j });
                }
                if i != j && v < 0.0 {
                    return Err(GeneratorError::NegativeRate { row: i, col: j });
                }
                sum += v;
            }
            let scale = row.iter().map(|v| v.abs()).fold(1.0, f64::max);
            if sum.abs() > ROW_SUM_TOL * scale {
                return Err(GeneratorError::RowSum { row: i, sum });
            }
            if row[i] >= 0.0 {
                return Err(GeneratorError::Absorbing { row: i });
            }
        }
        Ok(Self { q })
    }

    pub fn matrix(&self) -> &[[f64; K]; K] {
        &self.q
    }

    /// Total rate of leaving state i, q_i = -Q[i][i] (> 0 by construction).
    pub fn exit_rate(&self, i: usize) -> f64 {
        -self.q[i][i]
    }

    /// The stationary distribution pi (pi Q = 0, sum pi = 1), by Gaussian elimination with partial
    /// pivoting on Q^T with the last equation replaced by the normalization. `None` if the system
    /// is singular (a reducible chain without a unique stationary distribution).
    pub fn stationary_distribution(&self) -> Option<[f64; K]> {
        // a[r][c] = Q[c][r] (transpose), last row = all ones; rhs = e_last.
        let mut a = [[0.0; K]; K];
        let mut b = [0.0; K];
        for (r, a_row) in a.iter_mut().enumerate() {
            for (c, a_rc) in a_row.iter_mut().enumerate() {
                *a_rc = if r == K - 1 { 1.0 } else { self.q[c][r] };
            }
        }
        b[K - 1] = 1.0;
        for col in 0..K {
            let pivot = (col..K).max_by(|&x, &y| a[x][col].abs().total_cmp(&a[y][col].abs()))?;
            if a[pivot][col].abs() < 1e-300 {
                return None;
            }
            a.swap(col, pivot);
            b.swap(col, pivot);
            let pivot_row = a[col];
            for r in (col + 1)..K {
                let f = a[r][col] / pivot_row[col];
                for (x, p) in a[r][col..].iter_mut().zip(&pivot_row[col..]) {
                    *x -= f * p;
                }
                b[r] -= f * b[col];
            }
        }
        let mut pi = [0.0; K];
        for r in (0..K).rev() {
            let mut s = b[r];
            for c in (r + 1)..K {
                s -= a[r][c] * pi[c];
            }
            pi[r] = s / a[r][r];
        }
        pi.iter().all(|p| p.is_finite() && *p >= -1e-12).then_some(pi.map(|p| p.max(0.0)))
    }
}

/// One simulated path: `states[k]` holds on `[jump_times[k], jump_times[k + 1])`; the last state
/// holds until `horizon`. `jump_times[0]` is the start time 0.
#[derive(Debug, Clone, PartialEq)]
pub struct CtPath {
    pub jump_times: Vec<f64>,
    pub states: Vec<usize>,
    pub horizon: f64,
}

impl CtPath {
    /// The state at time `t` (right-continuous: at a jump time, the NEW state). `t` is clamped to
    /// `[0, horizon]`.
    pub fn state_at(&self, t: f64) -> usize {
        // Index of the last jump time <= t.
        let k = self.jump_times.partition_point(|&s| s <= t);
        self.states[k.saturating_sub(1)]
    }

    /// The states at each of `times` (any order; each looked up independently).
    pub fn sample_at(&self, times: &[f64]) -> Vec<usize> {
        times.iter().map(|&t| self.state_at(t)).collect()
    }

    /// Completed holding times per state: the durations of every visit that ended with a jump
    /// before `horizon` (the final, censored visit is excluded).
    pub fn completed_holding_times(&self) -> Vec<(usize, f64)> {
        self.states
            .iter()
            .zip(self.jump_times.windows(2))
            .map(|(&s, w)| (s, w[1] - w[0]))
            .collect()
    }
}

/// Exponential(rate) draw by inversion; `1 - u` lies in (0, 1], so the log is finite.
fn exponential(rate: f64, rng: &mut SplitMix64) -> f64 {
    -(1.0 - rng.next_f64()).ln() / rate
}

/// Draws an index with probability proportional to `weights` (non-negative, positive total).
fn categorical<const K: usize>(weights: &[f64; K], rng: &mut SplitMix64) -> usize {
    let total: f64 = weights.iter().sum();
    let u = rng.next_f64() * total;
    let mut acc = 0.0;
    let mut last_positive = 0;
    for (k, &w) in weights.iter().enumerate() {
        if w > 0.0 {
            last_positive = k;
            acc += w;
            if u < acc {
                return k;
            }
        }
    }
    last_positive
}

/// Simulates one path on `[0, horizon]` starting from a state drawn from `initial` (non-negative
/// weights, positive total; e.g. the stationary distribution).
pub fn simulate_path<const K: usize>(
    generator: &Generator<K>,
    initial: &[f64; K],
    horizon: f64,
    rng: &mut SplitMix64,
) -> CtPath {
    let mut state = categorical(initial, rng);
    let mut t = 0.0;
    let mut jump_times = vec![0.0];
    let mut states = vec![state];
    loop {
        t += exponential(generator.exit_rate(state), rng);
        if t >= horizon {
            break;
        }
        let mut weights = generator.q[state];
        weights[state] = 0.0;
        state = categorical(&weights, rng);
        jump_times.push(t);
        states.push(state);
    }
    CtPath { jump_times, states, horizon }
}

/// Observation times of a Poisson process with the given rate on `(0, horizon)`, ascending --
/// irregular spacing with no relation to the path.
pub fn poisson_times(rate: f64, horizon: f64, rng: &mut SplitMix64) -> Vec<f64> {
    let mut times = Vec::new();
    let mut t = exponential(rate, rng);
    while t < horizon {
        times.push(t);
        t += exponential(rate, rng);
    }
    times
}

/// One diagonal-scale multivariate Student-t draw with a shared chi-square factor.
pub fn student_t_draw<const D: usize>(mean: &[f64; D], var: &[f64; D], nu: f64, rng: &mut SplitMix64) -> [f64; D] {
    let w = chi_square_sample(nu, rng);
    let scale = (nu / w).sqrt();
    let mut x = [0.0; D];
    for d in 0..D {
        x[d] = mean[d] + var[d].sqrt() * standard_normal(rng) * scale;
    }
    x
}

/// Student-t observations for a sequence of states (`means[k]`, `vars[k]`, `dof[k]` per state).
pub fn simulate_emissions<const D: usize, const K: usize>(
    states: &[usize],
    means: &[[f64; D]; K],
    vars: &[[f64; D]; K],
    dof: &[f64; K],
    rng: &mut SplitMix64,
) -> Vec<[f64; D]> {
    states.iter().map(|&s| student_t_draw(&means[s], &vars[s], dof[s], rng)).collect()
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    /// 2-state generator with rates a (0 -> 1) and b (1 -> 0).
    fn two_state(a: f64, b: f64) -> Generator<2> {
        Generator::new([[-a, a], [b, -b]]).unwrap()
    }

    fn three_state() -> Generator<3> {
        Generator::new([[-0.5, 0.3, 0.2], [0.1, -0.4, 0.3], [0.6, 0.2, -0.8]]).unwrap()
    }

    /// |x - expected| within `k` standard errors.
    fn within(x: f64, expected: f64, se: f64, k: f64) -> bool {
        (x - expected).abs() <= k * se
    }

    #[test]
    fn invalid_generators_are_rejected() {
        assert_eq!(Generator::new([[-1.0, 1.0], [f64::NAN, 0.0]]), Err(GeneratorError::NotFinite { row: 1, col: 0 }));
        assert_eq!(Generator::new([[1.0, -1.0], [1.0, -1.0]]), Err(GeneratorError::NegativeRate { row: 0, col: 1 }));
        assert!(matches!(Generator::new([[-1.0, 0.5], [1.0, -1.0]]), Err(GeneratorError::RowSum { row: 0, .. })));
        assert_eq!(Generator::new([[0.0, 0.0], [1.0, -1.0]]), Err(GeneratorError::Absorbing { row: 0 }));
        assert!(Generator::new([[-1.0, 1.0], [2.0, -2.0]]).is_ok());
    }

    #[test]
    fn stationary_distribution_matches_closed_forms() {
        // 2 states: pi_0 = b / (a + b).
        let pi = two_state(0.3, 0.7).stationary_distribution().unwrap();
        assert!((pi[0] - 0.7).abs() < 1e-12 && (pi[1] - 0.3).abs() < 1e-12);
        // 3 states: pi Q = 0 and sums to 1.
        let g = three_state();
        let pi = g.stationary_distribution().unwrap();
        assert!((pi.iter().sum::<f64>() - 1.0).abs() < 1e-12);
        for j in 0..3 {
            let flow: f64 = (0..3).map(|i| pi[i] * g.matrix()[i][j]).sum();
            assert!(flow.abs() < 1e-12, "column {j}: {flow}");
        }
    }

    #[test]
    fn right_continuous_state_lookup() {
        let path = CtPath { jump_times: vec![0.0, 1.0, 2.5], states: vec![0, 1, 0], horizon: 4.0 };
        assert_eq!(path.sample_at(&[0.0, 0.999, 1.0, 2.4, 2.5, 4.0]), vec![0, 0, 1, 1, 0, 0]);
        assert_eq!(path.completed_holding_times(), vec![(0, 1.0), (1, 1.5)]);
    }

    #[test]
    fn same_seed_same_path() {
        let g = three_state();
        let p1 = simulate_path(&g, &[1.0, 0.0, 0.0], 100.0, &mut SplitMix64::new(7));
        let p2 = simulate_path(&g, &[1.0, 0.0, 0.0], 100.0, &mut SplitMix64::new(7));
        assert_eq!(p1, p2);
        assert!(p1.jump_times.windows(2).all(|w| w[0] < w[1]));
        assert!(p1.states.windows(2).all(|w| w[0] != w[1]), "a jump always changes state");
        assert!(p1.jump_times.last().is_some_and(|&t| t < 100.0));
    }

    /// Holding times in state i are Exponential(q_i): mean 1/q_i and P(hold > mean) = e^-1.
    #[test]
    fn holding_times_are_exponential_with_the_exit_rate() {
        let g = three_state();
        let path = simulate_path(&g, &[1.0, 0.0, 0.0], 400_000.0, &mut SplitMix64::new(11));
        for i in 0..3 {
            let holds: Vec<f64> = path.completed_holding_times().into_iter().filter(|&(s, _)| s == i).map(|(_, h)| h).collect();
            let n = holds.len() as f64;
            assert!(n > 10_000.0, "state {i}: only {n} visits");
            let mean_true = 1.0 / g.exit_rate(i);
            let mean = holds.iter().sum::<f64>() / n;
            assert!(within(mean, mean_true, mean_true / n.sqrt(), 5.0), "state {i}: mean {mean} vs {mean_true}");
            let p_tail = holds.iter().filter(|&&h| h > mean_true).count() as f64 / n;
            let p_true = (-1.0f64).exp();
            assert!(within(p_tail, p_true, (p_true * (1.0 - p_true) / n).sqrt(), 5.0), "state {i}: tail {p_tail}");
        }
    }

    /// From state i the next state is j with probability Q[i][j] / q_i.
    #[test]
    fn jump_destinations_follow_the_rates() {
        let g = three_state();
        let path = simulate_path(&g, &[1.0, 0.0, 0.0], 400_000.0, &mut SplitMix64::new(13));
        for i in 0..3 {
            let next: Vec<usize> = path.states.windows(2).filter(|w| w[0] == i).map(|w| w[1]).collect();
            let n = next.len() as f64;
            for j in (0..3).filter(|&j| j != i) {
                let p_true = g.matrix()[i][j] / g.exit_rate(i);
                let p = next.iter().filter(|&&s| s == j).count() as f64 / n;
                assert!(within(p, p_true, (p_true * (1.0 - p_true) / n).sqrt(), 5.0), "{i}->{j}: {p} vs {p_true}");
            }
        }
    }

    /// Long-run fraction of time in each state equals the stationary distribution.
    #[test]
    fn time_occupancy_converges_to_the_stationary_distribution() {
        let g = three_state();
        let pi = g.stationary_distribution().unwrap();
        let horizon = 400_000.0;
        let path = simulate_path(&g, &[1.0, 0.0, 0.0], horizon, &mut SplitMix64::new(17));
        let mut occupancy = [0.0; 3];
        for (k, &s) in path.states.iter().enumerate() {
            let end = path.jump_times.get(k + 1).copied().unwrap_or(horizon);
            occupancy[s] += end - path.jump_times[k];
        }
        for i in 0..3 {
            assert!((occupancy[i] / horizon - pi[i]).abs() < 0.01, "state {i}: {} vs {}", occupancy[i] / horizon, pi[i]);
        }
    }

    /// The independent check stage 2 needs: for 2 states, P_00(d) = pi_0 + pi_1 e^{-(a+b) d}
    /// (closed form). Many independent paths started in state 0, each read at time d.
    #[test]
    fn transition_probability_over_a_lag_matches_the_closed_form() {
        let (a, b) = (0.4, 0.9);
        let g = two_state(a, b);
        let (pi0, pi1) = (b / (a + b), a / (a + b));
        let mut rng = SplitMix64::new(19);
        let n = 200_000;
        for d in [0.1, 0.5, 1.0, 3.0] {
            let stayed = (0..n).filter(|_| simulate_path(&g, &[1.0, 0.0], d + 1.0, &mut rng).state_at(d) == 0).count();
            let p = stayed as f64 / n as f64;
            let p_true = pi0 + pi1 * (-(a + b) * d).exp();
            assert!(within(p, p_true, (p_true * (1.0 - p_true) / n as f64).sqrt(), 5.0), "d={d}: {p} vs {p_true}");
        }
    }

    #[test]
    fn poisson_times_are_ascending_with_the_right_rate() {
        let times = poisson_times(2.0, 100_000.0, &mut SplitMix64::new(23));
        assert!(times.windows(2).all(|w| w[0] < w[1]) && times.iter().all(|&t| t > 0.0 && t < 100_000.0));
        let n = times.len() as f64;
        assert!(within(n, 200_000.0, 200_000f64.sqrt(), 5.0), "count {n}");
    }

    /// Student-t moments: mean mu, variance var * nu / (nu - 2); the shared chi-square factor
    /// makes squares positively correlated across dimensions (joint tails) even though the
    /// dimensions are uncorrelated.
    #[test]
    fn student_t_draws_have_the_known_moments_and_joint_tails() {
        let (mean, var, nu) = ([1.0, -2.0], [4.0, 0.25], 6.0);
        let mut rng = SplitMix64::new(29);
        let n = 400_000;
        let xs: Vec<[f64; 2]> = (0..n).map(|_| student_t_draw(&mean, &var, nu, &mut rng)).collect();
        let nf = n as f64;
        for d in 0..2 {
            let m = xs.iter().map(|x| x[d]).sum::<f64>() / nf;
            let v_true = var[d] * nu / (nu - 2.0);
            assert!(within(m, mean[d], (v_true / nf).sqrt(), 5.0), "dim {d}: mean {m}");
            let v = xs.iter().map(|x| (x[d] - mean[d]).powi(2)).sum::<f64>() / nf;
            assert!((v / v_true - 1.0).abs() < 0.03, "dim {d}: var {v} vs {v_true}");
        }
        // Cross-moment of centred squares: E[z1^2 z2^2] = nu^2 E[1/W^2] = nu^2 / ((nu-2)(nu-4))
        // per unit variance, vs (nu/(nu-2))^2 if the dimensions were independent.
        let s = xs.iter().map(|x| (x[0] - mean[0]).powi(2) / var[0] * (x[1] - mean[1]).powi(2) / var[1]).sum::<f64>() / nf;
        let joint = nu * nu / ((nu - 2.0) * (nu - 4.0));
        let independent = (nu / (nu - 2.0)).powi(2);
        assert!((s / joint - 1.0).abs() < 0.1, "cross-moment {s} vs {joint}");
        assert!(s > 1.5 * independent, "joint tails missing: {s} vs independent {independent}");
    }

    #[test]
    fn emissions_follow_the_state_sequence() {
        let means = [[0.0], [100.0]];
        let vars = [[1.0], [1.0]];
        let xs = simulate_emissions(&[0, 1, 1, 0], &means, &vars, &[30.0, 30.0], &mut SplitMix64::new(31));
        assert!(xs[0][0].abs() < 20.0 && xs[3][0].abs() < 20.0);
        assert!((xs[1][0] - 100.0).abs() < 20.0 && (xs[2][0] - 100.0).abs() < 20.0);
    }
}
