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

/// Largest lambda * d handled in one uniformization pass by `accumulate_step_statistics`; longer
/// steps are split in half (exactly -- see there).
const MAX_STATISTICS_LAMBDA_D: f64 = 8.0;

/// Expected time spent in each regime and expected jumps between regimes, summed over the steps
/// of a sequence -- the sufficient statistics of the switching rates in continuous-time EM
/// (Liu, Li, Li, Song & Rehg 2015; the endpoint-conditioned expectations of Hobolth & Jensen
/// 2011, computed here by uniformization so every term is non-negative).
///
/// For a step of length d with posterior two-slice weights xi[i][j] = P(regime i at the start,
/// regime j at the end | all data), the step contributes
///   occupancy[k] += sum_ij xi_ij * E[time in k | i -> j over d]
///   jumps[k][l]  += sum_ij xi_ij * E[jumps k -> l | i -> j over d]
/// with E[time in k | i, j] = I_kk(d)_ij / P_ij(d), E[jumps k -> l | i, j] = q_kl I_kl(d)_ij / P_ij(d),
/// I_kl(d) = int_0^d e^{Q s} e_k e_l^T e^{Q (d - s)} ds. The caller passes w = xi / P(d) (so the
/// division by P happens where xi is formed, without a separate pass).
///
/// Uniformization (lambda >= every exit rate, R = I + Q / lambda, R stochastic):
///   I_kl(d) = (1/lambda) sum_n Pois(n + 1; lambda d) sum_{m=0}^{n} R^m e_k e_l^T R^{n-m},
/// so sum_ij w_ij I_kl(d)_ij = [A]_kl with
///   A = sum_a (R^a)^T w B_a,   B_a = sum_b c_{a+b} (R^b)^T,   c_n = Pois(n + 1; lambda d) / lambda.
/// The powers of R depend only on Q, so they are computed once per generator and reused.
#[derive(Debug, Clone)]
pub struct StepStatistics<const K: usize> {
    generator: Generator<K>,
    lambda: f64,
    /// (R^n)^T for n = 0, 1, ...
    powers_t: Vec<[[f64; K]; K]>,
    pub occupancy: [f64; K],
    pub jumps: [[f64; K]; K],
}

fn transpose<const K: usize>(m: &[[f64; K]; K]) -> [[f64; K]; K] {
    let mut t = [[0.0; K]; K];
    for (i, row) in m.iter().enumerate() {
        for (j, &v) in row.iter().enumerate() {
            t[j][i] = v;
        }
    }
    t
}

impl<const K: usize> StepStatistics<K> {
    pub fn new(generator: &Generator<K>) -> Self {
        let lambda = (0..K).map(|i| generator.exit_rate(i)).fold(0.0, f64::max);
        Self { generator: generator.clone(), lambda, powers_t: vec![identity()], occupancy: [0.0; K], jumps: [[0.0; K]; K] }
    }

    fn ensure_powers(&mut self, n: usize) {
        if self.powers_t.len() > n {
            return;
        }
        let mut r = identity::<K>();
        for (r_row, q_row) in r.iter_mut().zip(self.generator.matrix()) {
            for (r_ij, &q_ij) in r_row.iter_mut().zip(q_row) {
                *r_ij += q_ij / self.lambda;
            }
        }
        let r_t = transpose(&r);
        while self.powers_t.len() <= n {
            let last = self.powers_t[self.powers_t.len() - 1];
            // (R^{n+1})^T = R^T (R^n)^T
            self.powers_t.push(mat_mul(&r_t, &last));
        }
    }

    /// Adds one step of length `d` with weights `w[i][j] = xi_ij / P_ij(d)`.
    pub fn accumulate(&mut self, d: f64, w: &[[f64; K]; K]) -> Result<(), ElapsedTimeError> {
        if !(d.is_finite() && d >= 0.0) {
            return Err(ElapsedTimeError::InvalidElapsedTime(d));
        }
        if d == 0.0 {
            return Ok(());
        }
        let x = self.lambda * d;
        if x > MAX_STATISTICS_LAMBDA_D {
            // Split exactly: I(d1 + d2) = I(d1) P(d2) + P(d1) I(d2), so
            // sum_ij w_ij I(d)_ij = [first half with w P(d2)^T] + [second half with P(d1)^T w].
            let half = 0.5 * d;
            let p_half = transition_matrix(&self.generator, half)?;
            let w_first = mat_mul(w, &transpose(&p_half));
            let w_second = mat_mul(&transpose(&p_half), w);
            self.accumulate(half, &w_first)?;
            return self.accumulate(half, &w_second);
        }
        // c_n = Pois(n + 1; x) / lambda until the remaining Poisson mass is negligible.
        let mut c = Vec::new();
        let mut pois = (-x).exp(); // Pois(0; x)
        let mut cumulative = pois;
        let mut n = 0usize;
        loop {
            pois *= x / (n + 1) as f64; // Pois(n + 1; x)
            cumulative += pois;
            c.push(pois / self.lambda);
            n += 1;
            if (1.0 - cumulative) < TAIL_TOL && (n as f64) > x || n >= 256 {
                break;
            }
        }
        let n_max = c.len() - 1;
        self.ensure_powers(n_max);
        let mut a = [[0.0; K]; K];
        for (ai, power_a) in self.powers_t.iter().take(n_max + 1).enumerate() {
            // B_a = sum_b c_{a+b} (R^b)^T
            let mut b_mat = [[0.0; K]; K];
            for (bi, power_b) in self.powers_t.iter().take(n_max + 1 - ai).enumerate() {
                let coeff = c[ai + bi];
                for (b_row, p_row) in b_mat.iter_mut().zip(power_b) {
                    for (b_v, &p_v) in b_row.iter_mut().zip(p_row) {
                        *b_v += coeff * p_v;
                    }
                }
            }
            let term = mat_mul(&mat_mul(power_a, w), &b_mat);
            for (a_row, t_row) in a.iter_mut().zip(&term) {
                for (a_v, &t_v) in a_row.iter_mut().zip(t_row) {
                    *a_v += t_v;
                }
            }
        }
        let q = *self.generator.matrix();
        for k in 0..K {
            self.occupancy[k] += a[k][k];
            for l in (0..K).filter(|&l| l != k) {
                self.jumps[k][l] += q[k][l] * a[k][l];
            }
        }
        Ok(())
    }
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

    /// I_kl(d)_ij by composite Simpson quadrature of [P(s)]_ik [P(d - s)]_lj -- an independent
    /// route to the same integral.
    fn integral_by_quadrature<const K: usize>(g: &Generator<K>, d: f64, i: usize, j: usize, k: usize, l: usize) -> f64 {
        let n = 2000; // even
        let h = d / n as f64;
        (0..=n)
            .map(|m| {
                let s = m as f64 * h;
                let weight = if m == 0 || m == n { 1.0 } else if m % 2 == 1 { 4.0 } else { 2.0 };
                weight * transition_matrix(g, s).unwrap()[i][k] * transition_matrix(g, d - s).unwrap()[l][j]
            })
            .sum::<f64>()
            * h
            / 3.0
    }

    /// Occupancy and jumps for a single endpoint pair (w = e_i e_j^T) match quadrature, and the
    /// expected occupancies over all regimes add up to the whole step.
    #[test]
    fn step_statistics_match_quadrature_and_add_up() {
        let g = three_state();
        for d in [0.3, 2.0, 25.0] {
            let p = transition_matrix(&g, d).unwrap();
            for (i, j) in [(0, 0), (0, 2), (2, 1)] {
                let mut w = [[0.0; 3]; 3];
                w[i][j] = 1.0;
                let mut stats = StepStatistics::new(&g);
                stats.accumulate(d, &w).unwrap();
                for k in 0..3 {
                    let quad = integral_by_quadrature(&g, d, i, j, k, k);
                    assert!((stats.occupancy[k] - quad).abs() < 1e-9 * d.max(1.0), "d={d} {i}->{j} occupancy[{k}] {} vs {quad}", stats.occupancy[k]);
                    for l in (0..3).filter(|&l| l != k) {
                        let quad = g.matrix()[k][l] * integral_by_quadrature(&g, d, i, j, k, l);
                        assert!((stats.jumps[k][l] - quad).abs() < 1e-9 * d.max(1.0), "d={d} {i}->{j} jumps[{k}][{l}]");
                    }
                }
                // sum_k E[time in k | i, j] = d, i.e. sum_k I_kk(d)_ij = d P_ij(d).
                let total: f64 = stats.occupancy.iter().sum();
                assert!((total - d * p[i][j]).abs() < 1e-11 * d.max(1.0), "d={d}: {total} vs {}", d * p[i][j]);
            }
        }
    }

    /// The exact split for long steps agrees with a direct pass, and weights are linear.
    #[test]
    fn long_steps_are_split_exactly() {
        let g = Generator::new([[-2.0, 1.5, 0.5], [0.2, -0.4, 0.2], [1.0, 1.0, -2.0]]).unwrap();
        let w = [[0.3, 0.1, 0.0], [0.0, 0.5, 0.2], [0.1, 0.0, 0.4]];
        let d = 30.0; // lambda d = 60 -> split
        let mut split = StepStatistics::new(&g);
        split.accumulate(d, &w).unwrap();
        // Same step as two halves composed by hand.
        let half = d / 2.0;
        let p_half = transition_matrix(&g, half).unwrap();
        let mut manual = StepStatistics::new(&g);
        manual.accumulate(half, &mat_mul(&w, &transpose(&p_half))).unwrap();
        manual.accumulate(half, &mat_mul(&transpose(&p_half), &w)).unwrap();
        for k in 0..3 {
            assert!((split.occupancy[k] - manual.occupancy[k]).abs() < 1e-9);
        }
        // Total occupancy = d * sum_ij w_ij P_ij(d).
        let p = transition_matrix(&g, d).unwrap();
        let expected: f64 = (0..3).flat_map(|i| (0..3).map(move |j| (i, j))).map(|(i, j)| w[i][j] * p[i][j] * d).sum();
        assert!((split.occupancy.iter().sum::<f64>() - expected).abs() < 1e-9 * expected);
    }
}
