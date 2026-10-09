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

/// Largest window this estimator supports (matches DfaHurstExponent.h's own capacity constant --
/// fixed-capacity stack buffers, no heap allocation, same hot-path discipline as the C++ original).
pub const DFA_MAX_WINDOW: usize = 512;

/// Detrended Fluctuation Analysis (DFA) Hurst-exponent estimator. Port of DfaHurstExponent.h.
/// `log_returns` must already be log-returns (not prices), oldest first, most recent last -- exactly
/// `ImbalanceBarEngine::GetImbalanceBarReturns()`'s own ordering.
///
/// Returns NaN for a degenerate/insufficient-data window -- callers own the carry-forward/cold-start
/// fallback decision (this function holds no persistent state, same as the C++ original).
///
/// Note on the sub-16-element case: the C++ original does `length = std::clamp(length, 16,
/// kDfaMaxWindow)`, which, if `length` is requested smaller than 16 against an *already
/// fixed-capacity-512 buffer*, deliberately reads up to 16 elements regardless. The real call site
/// (`CalculateHurstExponent`) already pre-clamps to >= 16 before calling, so this never fires in
/// practice. A Rust slice has no capacity beyond its own length, so reading "up to 16" from a
/// shorter slice would be unsound; this port instead treats a slice shorter than 16 as insufficient
/// data (NaN) -- behaviorally identical for every reachable real input, safe for the unreachable one.
pub fn dfa_hurst_exponent(log_returns: &[f32], min_scale: i32) -> f32 {
    let min_scale = min_scale.clamp(4, 64) as usize;
    if log_returns.len() < 16 {
        return f32::NAN;
    }
    let length = log_returns.len().min(DFA_MAX_WINDOW);
    if length < min_scale * 4 {
        return f32::NAN;
    }
    let log_returns = &log_returns[..length];

    let mut profile = [0.0f64; DFA_MAX_WINDOW];
    let mut log_scales = [0.0f64; DFA_MAX_WINDOW];
    let mut log_fluctuations = [0.0f64; DFA_MAX_WINDOW];

    let sum_returns: f64 = log_returns.iter().map(|&x| x as f64).sum();
    let mean_return = sum_returns / length as f64;

    let mut cumulative = 0.0f64;
    for i in 0..length {
        cumulative += log_returns[i] as f64 - mean_return;
        profile[i] = cumulative;
    }

    let max_scale = length / 4;
    if max_scale <= min_scale {
        return f32::NAN;
    }

    let step = if max_scale - min_scale > 50 { 2 } else { 1 };
    let mut valid_scale_count = 0usize;

    let mut s = min_scale;
    while s <= max_scale {
        let num_segments = length / s;
        if num_segments < 1 {
            s += step;
            continue;
        }

        let mut total_variance = 0.0f64;
        let mut used_segments = 0usize;

        for v in 0..num_segments {
            let start_index = v * s;
            let n = s as f64;
            let sum_x = n * (n - 1.0) * 0.5;
            let sum_x2 = n * (n - 1.0) * (2.0 * n - 1.0) / 6.0;
            let denom = n * sum_x2 - sum_x * sum_x;
            if denom.abs() < 1e-12 {
                continue;
            }

            let mut sum_y = 0.0f64;
            let mut sum_xy = 0.0f64;
            for k in 0..s {
                let y = profile[start_index + k];
                sum_y += y;
                sum_xy += k as f64 * y;
            }

            let slope = (n * sum_xy - sum_x * sum_y) / denom;
            let intercept = (sum_y - slope * sum_x) / n;

            let mut ssr = 0.0f64;
            for k in 0..s {
                let trend = slope * k as f64 + intercept;
                let diff = profile[start_index + k] - trend;
                ssr += diff * diff;
            }

            total_variance += ssr / n;
            used_segments += 1;
        }

        if used_segments > 0 {
            let f_s = (total_variance / used_segments as f64).sqrt();
            if f_s > 1e-12 && valid_scale_count < DFA_MAX_WINDOW {
                log_scales[valid_scale_count] = (s as f64).ln();
                log_fluctuations[valid_scale_count] = f_s.ln();
                valid_scale_count += 1;
            }
        }

        s += step;
    }

    if valid_scale_count < 2 {
        return f32::NAN;
    }

    let n = valid_scale_count as f64;
    let mut sum_x = 0.0f64;
    let mut sum_y = 0.0f64;
    let mut sum_xy = 0.0f64;
    let mut sum_x2 = 0.0f64;
    for i in 0..valid_scale_count {
        let x = log_scales[i];
        let y = log_fluctuations[i];
        sum_x += x;
        sum_y += y;
        sum_xy += x * y;
        sum_x2 += x * x;
    }

    let regression_denom = n * sum_x2 - sum_x * sum_x;
    if regression_denom.abs() < 1e-12 {
        return f32::NAN;
    }

    let mut hurst = ((n * sum_xy - sum_x * sum_y) / regression_denom) as f32;
    hurst = hurst.clamp(0.0, 1.5);
    if !hurst.is_finite() {
        return f32::NAN;
    }
    hurst
}

/// Matches `MeanReversionCalculator.h`'s `kMaxLookback` -- the adaptive observation window's upper
/// bound ([10, 40] clamp), so the stack arrays below can never be written out of range.
pub const MEAN_REV_MAX_LOOKBACK: usize = 40;

/// Median/MAD (Kim & White 2004) price z-score, suppressed by lag-1 log-return autocorrelation
/// elasticity gating (dim 16, mean_rev_z). Port of MeanReversionCalculator.h's
/// `ComputeMeanReversionZ`. `prices` is the chronological price window ending at the current
/// (still-forming) bar, i.e. `prices[prices.len() - 1]` is "now".
///
/// Degenerate (flat window, MAD collapses below the numerical floor) returns `last_valid_value`
/// carried forward, matching the C++ original's carry-forward convention -- not a fabricated
/// exact-zero "no stretch" reading. Contract: result in [0, 5] for the non-degenerate path.
///
/// The C++ original takes a precondition on its caller ("n in [4, kMaxLookback]") that it does not
/// enforce itself -- its fixed `std::array<double, kMaxLookback>` scratch buffers would be written
/// out of bounds if ever called with more elements, which never happens in practice (the real call
/// site, `CalculateMeanReversionSpeed`, always sizes its window within that bound). A Rust slice
/// can't be written past its own bounds either way, so this port stays sound regardless by
/// keeping only the most recent `MEAN_REV_MAX_LOOKBACK` prices (anchored at the end, since the
/// *current* bar -- the last element -- must never be dropped) -- behaviorally identical to the
/// C++ original for every reachable real input.
pub fn compute_mean_reversion_z(prices: &[f32], last_valid_value: f32) -> f32 {
    const MAD_CONSISTENCY: f64 = 1.4826;
    const PRICE_EPS: f64 = 1e-6;

    if prices.is_empty() {
        return last_valid_value;
    }
    let n = prices.len().min(MEAN_REV_MAX_LOOKBACK);
    let prices = &prices[prices.len() - n..];

    let mut log_prices = [0.0f64; MEAN_REV_MAX_LOOKBACK];
    for i in 0..n {
        let p = (prices[i] as f64).max(PRICE_EPS);
        log_prices[i] = p.ln();
    }

    let mut scratch = log_prices;
    scratch[..n].sort_by(|a, b| a.partial_cmp(b).unwrap());
    let price_mid = n / 2;
    let median_log_p = scratch[price_mid];

    let mut dev_scratch = [0.0f64; MEAN_REV_MAX_LOOKBACK];
    for i in 0..n {
        dev_scratch[i] = (log_prices[i] - median_log_p).abs();
    }
    dev_scratch[..n].sort_by(|a, b| a.partial_cmp(b).unwrap());
    let mad_log_p = dev_scratch[price_mid];
    let scale_log_p = mad_log_p * MAD_CONSISTENCY;

    if scale_log_p < 1e-6 {
        return last_valid_value;
    }

    let current_log_p = log_prices[n - 1];
    let abs_z_price = ((current_log_p - median_log_p) / scale_log_p).abs();

    // Lag-1 autocorrelation on log-returns: positive rho => momentum, negative => reversion.
    let m = n - 1;
    if m < 3 {
        // Too few samples for the autocorrelation term -- genuinely computed, not degenerate, just
        // skips the elasticity gate.
        return (abs_z_price as f32).clamp(0.0, 5.0);
    }

    let mut returns = [0.0f64; MEAN_REV_MAX_LOOKBACK];
    for i in 0..m {
        let p = (prices[i + 1] as f64).max(PRICE_EPS);
        let p_prev = (prices[i] as f64).max(PRICE_EPS);
        returns[i] = (p / p_prev).ln();
    }

    let mut ret_scratch = returns;
    ret_scratch[..m].sort_by(|a, b| a.partial_cmp(b).unwrap());
    let ret_mid = m / 2;
    let median_r = ret_scratch[ret_mid];

    let mut num = 0.0f64;
    let mut den = 0.0f64;
    for t in 1..m {
        let r_t = returns[t] - median_r;
        let r_prev = returns[t - 1] - median_r;
        num += r_t * r_prev;
        den += r_prev * r_prev;
    }

    let rho = if den > 1e-12 { num / den } else { 0.0 };
    let elasticity_gate = (1.0 - rho.max(0.0)).clamp(0.0, 1.0);
    let score = abs_z_price * elasticity_gate;

    (score as f32).clamp(0.0, 5.0)
}

/// Window size for the liquidity-fragility microstructure elasticity ratio. Port of
/// LiquidityFragilityEngine.h's `kWindow` -- a *fixed* window, not an adaptive one (unlike
/// `compute_mean_reversion_z`/`dfa_hurst_exponent` above), so the real call site always supplies
/// exactly this many elements for both `range_window` and `sqrt_vol_window`.
pub const LIQ_FRAGILITY_WINDOW: usize = 30;

/// Microstructure elasticity ratio (Foucault, Kadan & Kandel 2005; Morris & Shin 2004), mapped
/// through a bounded sigmoid and EMA-blended against the previous reading (dim 12, liq_fragility).
/// Port of LiquidityFragilityEngine.h's `ComputeLiquidityFragility`.
///
/// `range_window`/`sqrt_vol_window`: `LIQ_FRAGILITY_WINDOW` closed bars' (high-low) and
/// sqrt(volume) respectively (order doesn't matter -- only the median is used). `live_bar_range`/
/// `live_volume_so_far` describe the still-forming current bar. Degenerate (thin live volume, a
/// collapsed scale reference, or -- unreachable in practice, see below -- a too-short window)
/// carries `prev_fragility` forward. Contract: result in [0, 1].
///
/// The C++ original takes raw pointers with no length parameter at all -- it unconditionally reads
/// exactly `kWindow` elements from each, a *stronger* precondition than `mean_rev_z`'s "n in [4,
/// 40]" (there is no partial-window case at all; every real call site sizes both arrays to exactly
/// 30). A Rust slice shorter than `LIQ_FRAGILITY_WINDOW` would be unsound to read that way, so this
/// port treats that case as a degenerate carry-forward too -- unreachable for every real input,
/// safe for the unreachable one.
pub fn compute_liquidity_fragility(
    range_window: &[f32],
    sqrt_vol_window: &[f32],
    live_bar_range: f32,
    live_volume_so_far: f32,
    prev_fragility: f32,
) -> f32 {
    const LIVE_BAR_MIN_VOLUME: f32 = 50.0;
    const EPS: f32 = 1e-6;
    const MID: usize = LIQ_FRAGILITY_WINDOW / 2;

    if range_window.len() < LIQ_FRAGILITY_WINDOW || sqrt_vol_window.len() < LIQ_FRAGILITY_WINDOW {
        return prev_fragility.clamp(0.0, 1.0);
    }

    let mut range_scratch = [0.0f32; LIQ_FRAGILITY_WINDOW];
    range_scratch.copy_from_slice(&range_window[..LIQ_FRAGILITY_WINDOW]);
    range_scratch.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let med_range = range_scratch[MID];

    let mut vol_scratch = [0.0f32; LIQ_FRAGILITY_WINDOW];
    vol_scratch.copy_from_slice(&sqrt_vol_window[..LIQ_FRAGILITY_WINDOW]);
    vol_scratch.sort_by(|a, b| a.partial_cmp(b).unwrap());
    let med_sqrt_vol = vol_scratch[MID];
    let scale_ref = med_range / (med_sqrt_vol + EPS);

    if live_volume_so_far < LIVE_BAR_MIN_VOLUME || scale_ref < EPS {
        return prev_fragility.clamp(0.0, 1.0);
    }

    let bar_range = if live_bar_range < 0.00001 { 0.00001 } else { live_bar_range };

    let eta = bar_range / (live_volume_so_far.sqrt() + EPS);
    let f_raw = eta / scale_ref;
    let log_f = f_raw.max(1e-6).ln();
    let fragility_raw = 1.0 / (1.0 + (-2.0 * log_f).exp());

    let alpha = if fragility_raw > prev_fragility { 0.30 } else { 0.15 };
    (alpha * fragility_raw + (1.0 - alpha) * prev_fragility).clamp(0.0, 1.0)
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

    // --- dfa_hurst_exponent: mirrors tests/cpp/test_dfa_hurst_exponent.cpp exactly ---

    // Converts a chronological price walk of length+1 points into the log-returns array
    // dfa_hurst_exponent expects (oldest first, most recent last) -- same conversion as the C++
    // test's own `ToLogReturns`.
    fn to_log_returns(prices: &[f32], length: usize) -> Vec<f32> {
        (0..length)
            .map(|i| {
                let cur = prices[i + 1] as f64;
                let prev = prices[i] as f64;
                if cur > 0.0 && prev > 0.0 { (cur / prev).ln() as f32 } else { 0.0 }
            })
            .collect()
    }

    // Independent brute-force reference, a second implementation of the same DFA math (not calling
    // dfa_hurst_exponent itself), matching tests/cpp/test_dfa_hurst_exponent.cpp's own
    // `BruteForceHurst` line for line -- operating on the chronological `prices` slice directly,
    // same price-to-return conversion, same DFA math, using Vec since this is a test-only reference,
    // not the hot-path production function.
    fn brute_force_hurst(prices: &[f32], length: usize, min_scale: usize) -> f64 {
        let min_scale = min_scale.clamp(4, 64);
        if length < min_scale * 4 {
            return f64::NAN;
        }
        let log_returns = to_log_returns(prices, length);
        let sum_returns: f64 = log_returns.iter().map(|&x| x as f64).sum();
        let mean_return = sum_returns / length as f64;

        let mut profile = vec![0.0f64; length];
        let mut cumulative = 0.0f64;
        for i in 0..length {
            cumulative += log_returns[i] as f64 - mean_return;
            profile[i] = cumulative;
        }

        let max_scale = length / 4;
        if max_scale <= min_scale {
            return f64::NAN;
        }
        let step = if max_scale - min_scale > 50 { 2 } else { 1 };

        let mut log_scales = Vec::new();
        let mut log_fluctuations = Vec::new();
        let mut s = min_scale;
        while s <= max_scale {
            let num_segments = length / s;
            if num_segments < 1 {
                s += step;
                continue;
            }
            let mut total_variance = 0.0f64;
            let mut used_segments = 0usize;
            for v in 0..num_segments {
                let start_index = v * s;
                let n = s as f64;
                let sum_x = n * (n - 1.0) * 0.5;
                let sum_x2 = n * (n - 1.0) * (2.0 * n - 1.0) / 6.0;
                let denom = n * sum_x2 - sum_x * sum_x;
                if denom.abs() < 1e-12 {
                    continue;
                }
                let mut sum_y = 0.0f64;
                let mut sum_xy = 0.0f64;
                for k in 0..s {
                    let y = profile[start_index + k];
                    sum_y += y;
                    sum_xy += k as f64 * y;
                }
                let slope = (n * sum_xy - sum_x * sum_y) / denom;
                let intercept = (sum_y - slope * sum_x) / n;
                let mut ssr = 0.0f64;
                for k in 0..s {
                    let trend = slope * k as f64 + intercept;
                    let diff = profile[start_index + k] - trend;
                    ssr += diff * diff;
                }
                total_variance += ssr / n;
                used_segments += 1;
            }
            if used_segments > 0 {
                let f_s = (total_variance / used_segments as f64).sqrt();
                if f_s > 1e-12 {
                    log_scales.push((s as f64).ln());
                    log_fluctuations.push(f_s.ln());
                }
            }
            s += step;
        }
        if log_scales.len() < 2 {
            return f64::NAN;
        }

        let n = log_scales.len() as f64;
        let mut sum_x = 0.0f64;
        let mut sum_y = 0.0f64;
        let mut sum_xy = 0.0f64;
        let mut sum_x2 = 0.0f64;
        for i in 0..log_scales.len() {
            sum_x += log_scales[i];
            sum_y += log_fluctuations[i];
            sum_xy += log_scales[i] * log_fluctuations[i];
            sum_x2 += log_scales[i] * log_scales[i];
        }
        let denom = n * sum_x2 - sum_x * sum_x;
        if denom.abs() < 1e-12 {
            return f64::NAN;
        }
        let hurst = (n * sum_xy - sum_x * sum_y) / denom;
        hurst.clamp(0.0, 1.5)
    }

    #[test]
    fn dfa_hurst_lookback_too_short_for_min_scale_times_4_is_nan() {
        let flat = [0.001f32; 16];
        assert!(dfa_hurst_exponent(&flat, 8).is_nan());
    }

    #[test]
    fn dfa_hurst_matches_brute_force_reference_across_window_sizes() {
        for &length in &[50usize, 100, 200] {
            let walk = make_walk(length + 1, 0.8, 2026);
            let log_returns = to_log_returns(&walk, length);

            let expected = brute_force_hurst(&walk, length, 8);
            let actual = dfa_hurst_exponent(&log_returns, 8);

            assert!(
                (expected - actual as f64).abs() < 1e-4,
                "length={length}: expected {expected}, got {actual}"
            );
            assert!((0.0..=1.5).contains(&actual), "length={length}: out of contract range: {actual}");
        }
    }

    #[test]
    fn dfa_hurst_random_walk_estimate_is_finite_and_in_plausible_mid_range() {
        // A pure random walk (no persistence) should land near H=0.5, not at either extreme --
        // sanity check that the estimator actually discriminates, not just "doesn't crash".
        let walk = make_walk(201, 1.0, 555);
        let log_returns = to_log_returns(&walk, 200);
        let hurst = dfa_hurst_exponent(&log_returns, 8);
        assert!(hurst.is_finite() && hurst > 0.2 && hurst < 0.9, "got {hurst}");
    }

    // --- compute_mean_reversion_z: golden values from tests/cpp/test_mean_reversion_calculator.cpp ---

    #[test]
    fn mean_rev_z_flat_window_carries_last_valid_forward() {
        let prices = [100.0f32; 7];
        assert!((compute_mean_reversion_z(&prices, 0.33) - 0.33).abs() <= 1e-4);
    }

    #[test]
    fn mean_rev_z_n7_full_path_matches_reference() {
        let prices = [100.0f32, 101.0, 99.0, 102.0, 98.0, 103.0, 110.0];
        assert!((compute_mean_reversion_z(&prices, 0.0) - 2.8224230).abs() <= 1e-3);
    }

    #[test]
    fn mean_rev_z_n4_full_path_matches_reference() {
        let prices = [100.0f32, 102.0, 101.0, 105.0];
        assert!((compute_mean_reversion_z(&prices, 0.0) - 0.9873349).abs() <= 1e-3);
    }

    #[test]
    fn mean_rev_z_n3_skips_autocorrelation_matches_reference() {
        let prices = [100.0f32, 102.0, 108.0];
        assert!((compute_mean_reversion_z(&prices, 0.0) - 1.9468539).abs() <= 1e-3);
    }

    #[test]
    fn mean_rev_z_result_stays_within_contract_bounds() {
        let prices = [100.0f32, 100.0, 100.0, 100.0, 100000.0];
        let result = compute_mean_reversion_z(&prices, 0.0);
        assert!((0.0..=5.0).contains(&result), "got {result}");
    }

    // --- compute_liquidity_fragility: golden values from tests/cpp/test_liquidity_fragility_engine.cpp ---

    #[test]
    fn liq_fragility_neutral_case_matches_reference() {
        let range_w = [2.0f32; LIQ_FRAGILITY_WINDOW];
        let vol_w = [10.0f32; LIQ_FRAGILITY_WINDOW];
        let result = compute_liquidity_fragility(&range_w, &vol_w, 4.0, 400.0, 0.5);
        assert!((result - 0.5000000).abs() <= 1e-3);
    }

    #[test]
    fn liq_fragility_fragile_case_matches_reference() {
        let range_w = [2.0f32; LIQ_FRAGILITY_WINDOW];
        let vol_w = [10.0f32; LIQ_FRAGILITY_WINDOW];
        let result = compute_liquidity_fragility(&range_w, &vol_w, 20.0, 100.0, 0.2);
        assert!((result - 0.4370297).abs() <= 1e-3);
    }

    #[test]
    fn liq_fragility_thin_volume_guard_carries_forward() {
        let range_w = [2.0f32; LIQ_FRAGILITY_WINDOW];
        let vol_w = [10.0f32; LIQ_FRAGILITY_WINDOW];
        let result = compute_liquidity_fragility(&range_w, &vol_w, 4.0, 10.0, 0.42);
        assert!((result - 0.42).abs() <= 1e-4);
    }

    #[test]
    fn liq_fragility_result_stays_within_contract_bounds() {
        let range_w = [0.5f32; LIQ_FRAGILITY_WINDOW];
        let vol_w = [20.0f32; LIQ_FRAGILITY_WINDOW];
        let result = compute_liquidity_fragility(&range_w, &vol_w, 500.0, 10000.0, 0.9);
        assert!((0.0..=1.0).contains(&result), "got {result}");
    }

    #[test]
    fn liq_fragility_too_short_window_carries_forward() {
        let range_w = [2.0f32; 10]; // shorter than LIQ_FRAGILITY_WINDOW -- unreachable in practice
        let vol_w = [10.0f32; 10];
        let result = compute_liquidity_fragility(&range_w, &vol_w, 4.0, 400.0, 0.37);
        assert!((result - 0.37).abs() <= 1e-6);
    }
}
