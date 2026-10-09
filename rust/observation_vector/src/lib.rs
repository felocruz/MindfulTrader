//! mts_observation_vector -- pure ports of this codebase's C++ observation-vector dim math.
//! Zero ACSIL/zmq/pyo3 dependency (infrastructure guide §2.1 "pure core, thin edges").
//!
//! Every function here is a byte-for-byte port of an existing, natively-tested C++ header; the
//! ported-from file is named in each function's own doc comment. Parity with C++ is proven two ways:
//! (1) this crate's own unit tests, mirroring each C++ test file's structure; (2) a separate C++ test
//! (tests/cpp/test_rust_observation_vector_parity.cpp) that calls both the original C++ function and
//! this crate's FFI wrapper (via rust/ffi) on the *same* input and asserts equality -- the real,
//! authoritative cross-language proof, not just "ported independently and hoping".

/// Linear-interpolation empirical quantile (R's default "Type 7" method) over an already-sorted
/// slice. Port of RobustMoments.h's `EmpiricalQuantile<N>`.
pub fn empirical_quantile(sorted: &[f32], p: f64) -> f32 {
    let n = sorted.len();
    let idx = p * (n - 1) as f64;
    let lo = idx.floor() as usize;
    let hi = idx.ceil() as usize;
    if lo == hi {
        return sorted[lo];
    }
    let frac = idx - lo as f64;
    (sorted[lo] as f64 + frac * (sorted[hi] as f64 - sorted[lo] as f64)) as f32
}

/// Bowley (1920) quartile skewness: (Q3 - 2*Q2 + Q1) / (Q3 - Q1). Port of RobustMoments.h's
/// `BowleySkewness`. Returns NaN if Q3==Q1 (degenerate window) -- caller must carry-forward, same
/// convention as the C++ original.
pub fn bowley_skewness(returns: &[f32]) -> f32 {
    let mut sorted: Vec<f32> = returns.to_vec();
    sorted.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let q1 = empirical_quantile(&sorted, 0.25);
    let q2 = empirical_quantile(&sorted, 0.50);
    let q3 = empirical_quantile(&sorted, 0.75);
    let denom = q3 - q1;
    if denom.abs() < 1e-10 {
        return f32::NAN;
    }
    (q3 - 2.0 * q2 + q1) / denom
}

/// Moors (1988) octile kurtosis: [Q(7/8)-Q(5/8)+Q(3/8)-Q(1/8)] / [Q(6/8)-Q(2/8)]. Port of
/// RobustMoments.h's `MoorsKurtosis`. Returns NaN if Q(6/8)==Q(2/8) (degenerate window).
pub fn moors_kurtosis(returns: &[f32]) -> f32 {
    let mut sorted: Vec<f32> = returns.to_vec();
    sorted.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let q1_8 = empirical_quantile(&sorted, 1.0 / 8.0);
    let q2_8 = empirical_quantile(&sorted, 2.0 / 8.0);
    let q3_8 = empirical_quantile(&sorted, 3.0 / 8.0);
    let q5_8 = empirical_quantile(&sorted, 5.0 / 8.0);
    let q6_8 = empirical_quantile(&sorted, 6.0 / 8.0);
    let q7_8 = empirical_quantile(&sorted, 7.0 / 8.0);
    let denom = q6_8 - q2_8;
    if denom.abs() < 1e-10 {
        return f32::NAN;
    }
    (q7_8 - q5_8 + q3_8 - q1_8) / denom
}

/// Sevcik (1998) fractal-dimension estimator: D = 1 + ln(L) / ln(2*N), L = sum of normalized
/// Euclidean segment lengths. Port of SevcikFractalDimension.h -- same exact (asymmetric) windowing
/// as the C++ original (see that header's own comment for the one-bar-offset convention this
/// replicates, not corrects). `prices` must hold `lookback_n + 1` chronological points;
/// `prices[0]` = oldest, `prices[lookback_n]` = the live/current bar.
pub fn sevcik_fractal_dimension(prices: &[f32]) -> f32 {
    let lookback_n = prices.len() as isize - 1;
    if lookback_n < 2 {
        return f32::NAN;
    }
    let lookback_n = lookback_n as usize;

    let mut min_p = prices[1];
    let mut max_p = prices[1];
    for &p in &prices[2..=lookback_n] {
        if p < min_p {
            min_p = p;
        }
        if p > max_p {
            max_p = p;
        }
    }
    if max_p <= min_p {
        return f32::NAN;
    }

    let segments = lookback_n - 1;
    if segments == 0 {
        return f32::NAN;
    }

    let mut length = 0.0f64;
    let price_range = max_p as f64 - min_p as f64;
    for i in 1..lookback_n {
        let dy = (prices[i] as f64 - prices[i - 1] as f64) / price_range;
        let dx = 1.0 / segments as f64;
        length += (dx * dx + dy * dy).sqrt();
    }
    if length <= 0.0 {
        return f32::NAN;
    }

    let dim = (1.0 + length.ln() / (2.0 * segments as f64).ln()) as f32;
    dim.clamp(1.0, 2.0)
}

/// Barndorff-Nielsen & Shephard (2004, 2006) bipower variation: a jump-robust estimator of the
/// continuous-time variance component of a return series. Port of BipowerVariation.h's
/// `ComputeBipowerVariation`. BV = (pi/2) * sum_{k=1}^{n-1} |returns[k-1]| * |returns[k]|.
/// Undefined for n < 2 (no adjacent pair exists); returns 0.0 in that case, matching the C++
/// original's neutral-value convention.
pub fn compute_bipower_variation(returns: &[f64]) -> f64 {
    const HALF_PI: f64 = std::f64::consts::FRAC_PI_2;
    if returns.len() < 2 {
        return 0.0;
    }
    let mut bv_sum = 0.0;
    for k in 1..returns.len() {
        bv_sum += returns[k - 1].abs() * returns[k].abs();
    }
    HALF_PI * bv_sum
}

#[cfg(test)]
mod tests {
    use super::*;

    // Deterministic pseudo-random walk (no external RNG crate), same LCG convention as
    // tests/cpp/test_sevcik_fractal_dimension.cpp -- so this and the C++ test can share identical
    // generated inputs when compared directly in the cross-language parity test.
    struct Lcg(u32);
    impl Lcg {
        fn next_unit(&mut self) -> f64 {
            self.0 = self.0.wrapping_mul(1664525).wrapping_add(1013904223);
            (self.0 >> 8) as f64 / 16_777_216.0
        }
    }

    fn make_walk(n: usize, sigma: f64, seed: u32) -> Vec<f32> {
        let mut rng = Lcg(seed);
        let mut price = 100.0f64;
        let mut out = Vec::with_capacity(n);
        for _ in 0..n {
            let u = rng.next_unit();
            price += sigma * (2.0 * u - 1.0);
            out.push(price as f32);
        }
        out
    }

    // Brute-force reference matching the C++ original's production formula (and
    // test_sevcik_fractal_dimension.cpp's own `BruteForceFractalDim`), operating directly on the
    // chronological `prices` slice this crate's own function already uses (no index-convention
    // translation needed here, unlike the C++ test, which must bridge an sc.Index-relative view).
    fn brute_force_fractal_dim(prices: &[f32]) -> f64 {
        let lookback_n = prices.len() - 1;
        let mut min_p = f32::MAX;
        let mut max_p = f32::MIN;
        for &p in &prices[1..=lookback_n] {
            if p < min_p {
                min_p = p;
            }
            if p > max_p {
                max_p = p;
            }
        }
        if max_p <= min_p {
            return f64::NAN;
        }
        let segments = lookback_n - 1;
        if segments == 0 {
            return f64::NAN;
        }
        let mut length = 0.0f64;
        let price_range = max_p as f64 - min_p as f64;
        for i in 1..lookback_n {
            let dy = (prices[i] as f64 - prices[i - 1] as f64) / price_range;
            let dx = 1.0 / segments as f64;
            length += (dx * dx + dy * dy).sqrt();
        }
        if length <= 0.0 {
            return f64::NAN;
        }
        1.0 + length.ln() / (2.0 * segments as f64).ln()
    }

    #[test]
    fn sevcik_degenerate_flat_window_is_nan() {
        assert!(sevcik_fractal_dimension(&vec![100.0f32; 41]).is_nan());
    }

    #[test]
    fn sevcik_lookback_too_short_is_nan() {
        assert!(sevcik_fractal_dimension(&[100.0, 101.0]).is_nan());
    }

    #[test]
    fn sevcik_matches_brute_force_reference_across_window_sizes() {
        for &lookback_n in &[30usize, 40, 150, 400] {
            let walk = make_walk(lookback_n + 1, 0.8, 4242);
            let expected = brute_force_fractal_dim(&walk);
            let actual = sevcik_fractal_dimension(&walk);
            assert!(
                (expected - actual as f64).abs() < 1e-4,
                "n={lookback_n}: expected {expected}, got {actual}"
            );
            assert!((1.0..=2.0).contains(&actual), "n={lookback_n}: out of contract range: {actual}");
        }
    }

    // Approximate Gaussian via Box-Muller over the same LCG -- good enough for the statistical-
    // property assertions below (mirroring test_robust_moments.cpp's own tolerances), not meant to
    // bit-match C++'s std::normal_distribution (implementation-defined, not portable across
    // languages; the cross-language parity test instead shares literal input arrays, not RNG streams).
    fn make_gaussian(n: usize, seed: u32) -> Vec<f32> {
        let mut rng = Lcg(seed);
        let mut out = Vec::with_capacity(n);
        while out.len() < n {
            let u1 = rng.next_unit().max(1e-12);
            let u2 = rng.next_unit();
            let r = (-2.0 * u1.ln()).sqrt();
            let theta = 2.0 * std::f64::consts::PI * u2;
            out.push((r * theta.cos()) as f32);
            if out.len() < n {
                out.push((r * theta.sin()) as f32);
            }
        }
        out
    }

    #[test]
    fn moors_kurtosis_on_gaussian_sample_is_close_to_reference_value() {
        let sample = make_gaussian(100, 11);
        let k = moors_kurtosis(&sample);
        assert!((k - 1.23).abs() < 0.35, "got {k}");
    }

    #[test]
    fn bowley_skewness_on_symmetric_gaussian_sample_is_near_zero() {
        let sample = make_gaussian(100, 11);
        let s = bowley_skewness(&sample);
        assert!(s.abs() < 0.15, "got {s}");
    }

    #[test]
    fn moors_kurtosis_is_not_dominated_by_a_single_extreme_outlier() {
        let mut sample = make_gaussian(100, 11);
        sample[0] = 50.0; // 50-sigma outlier
        let k = moors_kurtosis(&sample);
        assert!(k < 5.0, "got {k}");
    }

    // --- compute_bipower_variation: mirrors tests/cpp/test_bipower_variation.cpp exactly ---

    #[test]
    fn bipower_variation_n_below_2_returns_zero() {
        assert_eq!(compute_bipower_variation(&[]), 0.0);
        assert_eq!(compute_bipower_variation(&[0.01]), 0.0);
    }

    #[test]
    fn bipower_variation_hand_computed_three_element_window() {
        // r = {0.01, -0.02, 0.03}; BV = (pi/2) * (|0.01|*|-0.02| + |-0.02|*|0.03|)
        //                             = (pi/2) * 0.0008
        let r = [0.01, -0.02, 0.03];
        let expected = std::f64::consts::FRAC_PI_2 * 0.0008;
        let actual = compute_bipower_variation(&r);
        assert!((actual - expected).abs() <= 1e-9 * expected.abs().max(1.0), "got {actual}, expected {expected}");
    }

    #[test]
    fn bipower_variation_single_tick_jump_inflates_rv_far_more_than_bv() {
        let calm = [0.001, -0.0012, 0.0009, -0.0011, 0.0010];
        let rv_calm: f64 = calm.iter().map(|x| x * x).sum();
        let bv_calm = compute_bipower_variation(&calm);

        let mut jump = calm;
        jump[2] = 0.05; // one 50-sigma-scale outlier tick
        let rv_jump: f64 = jump.iter().map(|x| x * x).sum();
        let bv_jump = compute_bipower_variation(&jump);

        let rv_inflation = rv_jump / rv_calm;
        let bv_inflation = bv_jump / bv_calm;
        assert!(rv_inflation > 100.0, "got {rv_inflation}");
        assert!(bv_inflation < rv_inflation / 10.0, "rv_inflation={rv_inflation} bv_inflation={bv_inflation}");
    }
}
