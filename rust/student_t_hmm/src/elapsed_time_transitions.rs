//! elapsed_time_transitions.rs -- regime-to-regime transition probabilities over ANY elapsed
//! time: P(d) = expm(Q d) for a validated generator Q (rust-migration spec §6.2.8 D4 rulings: the
//! regime model is a continuous-time Student-t HMM whose steps have unequal lengths d).
//!
//! P(d)[i][j] = probability of being in regime j after elapsed time d, given regime i now.
//!
//! Method: uniformization (Jensen 1953; standard for continuous-time Markov chains) with scaling
//! and squaring. With lambda >= max exit rate, U = I + Q / lambda is a stochastic matrix and
//!   P(d) = sum_n e^{-lambda d} (lambda d)^n / n! U^n.
//! Every term is non-negative, so there is no cancellation: entries stay in [0, 1] and rows sum to
//! 1 up to rounding -- unlike a generic matrix exponential (eigendecomposition or Pade), which can
//! return small negative probabilities or lose accuracy when Q has nearly repeated or complex
//! eigenvalues (the hazards flagged by the msm audit). For a long elapsed time the series is
//! evaluated at d / 2^s with lambda d / 2^s <= 1 and squared s times (P(2t) = P(t)^2, the
//! semigroup property), which keeps the number of terms small and bounded.
//!
//! The series is truncated when the remaining Poisson mass is below `TAIL_TOL`, so the truncation
//! error per entry is below that bound before squaring.
//!
//! Cost: O(K^3) per term and per squaring; for K <= 8 this is a few thousand flops per call.

use crate::regime_path_simulator::Generator;

/// Remaining Poisson mass below which the uniformization series stops.
const TAIL_TOL: f64 = 1e-17;

/// Largest lambda * d evaluated directly; longer times are halved until below this, then squared.
const MAX_DIRECT_LAMBDA_D: f64 = 1.0;

/// Why a transition matrix could not be computed.
#[derive(Debug, Clone, PartialEq)]
pub enum ElapsedTimeError {
    /// The elapsed time is negative, NaN or infinite.
    InvalidElapsedTime(f64),
}

impl std::fmt::Display for ElapsedTimeError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::InvalidElapsedTime(d) => write!(f, "elapsed time {d} is not a finite non-negative number"),
        }
    }
}

impl std::error::Error for ElapsedTimeError {}

fn identity<const K: usize>() -> [[f64; K]; K] {
    let mut m = [[0.0; K]; K];
    for (i, row) in m.iter_mut().enumerate() {
        row[i] = 1.0;
    }
    m
}

fn mat_mul<const K: usize>(a: &[[f64; K]; K], b: &[[f64; K]; K]) -> [[f64; K]; K] {
    let mut c = [[0.0; K]; K];
    for (c_row, a_row) in c.iter_mut().zip(a) {
        for (&a_ik, b_row) in a_row.iter().zip(b) {
            for (c_ij, &b_kj) in c_row.iter_mut().zip(b_row) {
                *c_ij += a_ik * b_kj;
            }
        }
    }
    c
}

/// Transition probabilities over elapsed time `d` (>= 0) for generator `generator`.
pub fn transition_matrix<const K: usize>(generator: &Generator<K>, d: f64) -> Result<[[f64; K]; K], ElapsedTimeError> {
    if !(d.is_finite() && d >= 0.0) {
        return Err(ElapsedTimeError::InvalidElapsedTime(d));
    }
    if d == 0.0 {
        return Ok(identity());
    }
    let q = generator.matrix();
    let lambda = (0..K).map(|i| generator.exit_rate(i)).fold(0.0, f64::max);

    // Halve the time until lambda * t <= MAX_DIRECT_LAMBDA_D.
    let mut squarings = 0u32;
    let mut t = d;
    while lambda * t > MAX_DIRECT_LAMBDA_D {
        t *= 0.5;
        squarings += 1;
    }

    // U = I + Q / lambda (stochastic).
    let mut u = identity::<K>();
    for (u_row, q_row) in u.iter_mut().zip(q) {
        for (u_ij, &q_ij) in u_row.iter_mut().zip(q_row) {
            *u_ij += q_ij / lambda;
        }
    }

    // sum_n w_n U^n, w_n = e^{-x} x^n / n!, x = lambda t <= 1.
    let x = lambda * t;
    let mut weight = (-x).exp();
    let mut accumulated_mass = weight;
    let mut power = identity::<K>();
    let mut p = [[0.0; K]; K];
    for (p_row, pw_row) in p.iter_mut().zip(&power) {
        for (p_ij, &pw) in p_row.iter_mut().zip(pw_row) {
            *p_ij = weight * pw;
        }
    }
    let mut n = 0u32;
    while 1.0 - accumulated_mass > TAIL_TOL && n < 64 {
        n += 1;
        power = mat_mul(&power, &u);
        weight *= x / f64::from(n);
        accumulated_mass += weight;
        for (p_row, pw_row) in p.iter_mut().zip(&power) {
            for (p_ij, &pw) in p_row.iter_mut().zip(pw_row) {
                *p_ij += weight * pw;
            }
        }
    }

    // Each squaring adds rounding error of order K * epsilon to every row sum; over the ~20+
    // squarings a long time with fast regimes needs, that drift reaches ~1e-11. The exact result
    // is stochastic, so each row is rescaled to sum to 1 after every squaring -- restoring an
    // invariant lost only to rounding, without changing the ratios between entries.
    for _ in 0..squarings {
        p = mat_mul(&p, &p);
        for row in &mut p {
            let sum: f64 = row.iter().sum();
            for v in row.iter_mut() {
                *v /= sum;
            }
        }
    }
    Ok(p)
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::init::SplitMix64;
    use crate::regime_path_simulator::simulate_path;

    fn three_state() -> Generator<3> {
        Generator::new([[-0.5, 0.3, 0.2], [0.1, -0.4, 0.3], [0.6, 0.2, -0.8]]).unwrap()
    }

    fn assert_stochastic<const K: usize>(p: &[[f64; K]; K], tol: f64) {
        for row in p {
            assert!(row.iter().all(|&v| (0.0..=1.0).contains(&v)), "entry outside [0,1]: {row:?}");
            assert!((row.iter().sum::<f64>() - 1.0).abs() < tol, "row sum {}", row.iter().sum::<f64>());
        }
    }

    fn max_abs_diff<const K: usize>(a: &[[f64; K]; K], b: &[[f64; K]; K]) -> f64 {
        a.iter().flatten().zip(b.iter().flatten()).map(|(x, y)| (x - y).abs()).fold(0.0, f64::max)
    }

    #[test]
    fn invalid_elapsed_times_are_rejected_and_zero_is_identity() {
        let g = three_state();
        for d in [-1.0, f64::NAN, f64::INFINITY] {
            assert!(transition_matrix(&g, d).is_err());
        }
        assert_eq!(transition_matrix(&g, 0.0).unwrap(), identity());
    }

    /// Two regimes, rates a (0->1) and b (1->0): P_00(d) = pi_0 + pi_1 e^{-(a+b)d},
    /// P_11(d) = pi_1 + pi_0 e^{-(a+b)d} -- exact, across nine orders of magnitude of d.
    #[test]
    fn two_regimes_match_the_closed_form() {
        let (a, b) = (0.4, 0.9);
        let g = Generator::new([[-a, a], [b, -b]]).unwrap();
        let (pi0, pi1) = (b / (a + b), a / (a + b));
        for d in [1e-6, 1e-3, 0.1, 0.5, 1.0, 3.0, 10.0, 100.0, 1000.0] {
            let p = transition_matrix(&g, d).unwrap();
            let e = (-(a + b) * d).exp();
            let exact = [[pi0 + pi1 * e, pi1 - pi1 * e], [pi0 - pi0 * e, pi1 + pi0 * e]];
            assert!(max_abs_diff(&p, &exact) < 1e-13, "d={d}: {p:?} vs {exact:?}");
            assert_stochastic(&p, 1e-13);
        }
    }

    /// P(s + t) = P(s) P(t): moving forward s then t equals moving forward s + t.
    #[test]
    fn composing_two_elapsed_times_equals_their_sum() {
        let g = three_state();
        for (s, t) in [(0.3, 0.7), (1.0, 2.5), (0.01, 40.0)] {
            let lhs = transition_matrix(&g, s + t).unwrap();
            let rhs = mat_mul(&transition_matrix(&g, s).unwrap(), &transition_matrix(&g, t).unwrap());
            assert!(max_abs_diff(&lhs, &rhs) < 1e-13, "s={s}, t={t}");
        }
    }

    /// Short times: P(d) = I + Q d + O(d^2). Long times: every row tends to the stationary
    /// distribution (the starting regime is forgotten).
    #[test]
    fn short_and_long_time_limits() {
        let g = three_state();
        let d = 1e-7;
        let p = transition_matrix(&g, d).unwrap();
        for (i, (p_row, q_row)) in p.iter().zip(g.matrix()).enumerate() {
            for (j, (&p_ij, &q_ij)) in p_row.iter().zip(q_row).enumerate() {
                let first_order = if i == j { 1.0 } else { 0.0 } + q_ij * d;
                assert!((p_ij - first_order).abs() < 1e-12, "[{i}][{j}]");
            }
        }
        let pi = g.stationary_distribution().unwrap();
        let p = transition_matrix(&g, 500.0).unwrap();
        for row in &p {
            assert!(row.iter().zip(&pi).all(|(&p_ij, &pi_j)| (p_ij - pi_j).abs() < 1e-12), "{row:?} vs {pi:?}");
        }
    }

    /// Fast and slow regimes together (rates 10_000x apart -- a burst regime that flips in
    /// milliseconds next to a calm regime that lasts hours): still a valid probability matrix and
    /// still composable.
    #[test]
    fn widely_separated_rates_stay_valid() {
        let g = Generator::new([[-2000.0, 1000.0, 1000.0], [0.05, -0.1, 0.05], [0.1, 0.1, -0.2]]).unwrap();
        for d in [1e-5, 0.01, 1.0, 60.0, 3600.0] {
            let p = transition_matrix(&g, d).unwrap();
            assert_stochastic(&p, 1e-12);
        }
        let lhs = transition_matrix(&g, 1.5).unwrap();
        let rhs = mat_mul(&transition_matrix(&g, 0.5).unwrap(), &transition_matrix(&g, 1.0).unwrap());
        assert!(max_abs_diff(&lhs, &rhs) < 1e-12);
    }

    /// The independent check: the simulator never computes a matrix exponential. Many paths are
    /// started in each regime and read at time d; the observed frequencies must match P(d).
    #[test]
    fn matches_the_event_by_event_simulator() {
        let g = three_state();
        let mut rng = SplitMix64::new(41);
        let n = 100_000usize;
        for d in [0.2, 1.5, 6.0] {
            let p = transition_matrix(&g, d).unwrap();
            for start in 0..3 {
                let mut initial = [0.0; 3];
                initial[start] = 1.0;
                let mut counts = [0usize; 3];
                for _ in 0..n {
                    counts[simulate_path(&g, &initial, d + 1.0, &mut rng).state_at(d)] += 1;
                }
                for j in 0..3 {
                    let freq = counts[j] as f64 / n as f64;
                    let se = (p[start][j] * (1.0 - p[start][j]) / n as f64).sqrt().max(1e-6);
                    assert!((freq - p[start][j]).abs() <= 5.0 * se, "d={d} {start}->{j}: simulated {freq} vs computed {}", p[start][j]);
                }
            }
        }
    }
}
