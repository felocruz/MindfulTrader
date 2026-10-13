//! observation_vector -- pure ports of this codebase's C++ observation-vector dim math.
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
    sorted.sort_by(f32::total_cmp);
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
    sorted.sort_by(f32::total_cmp);
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

/// Port of `LogScaleRatio.h`: log(short_BV / long_BV), a short-vs-long-horizon scale ratio
/// (HAR-RV/Corsi 2009-style multi-horizon volatility clustering). Deliberately takes the two
/// ALREADY-COMPUTED bipower-variation values (from `compute_bipower_variation`), not raw
/// log-return windows -- matches `LogScaleRatio.h`'s own division of labor, avoiding a duplicate
/// BV computation the caller already did. Clamped to `[-6.0, 6.0]`.
pub fn log_scale_ratio(short_bv: f64, long_bv: f64) -> f32 {
    const VAR_EPS: f64 = 1e-12;
    let log_ratio = ((short_bv + VAR_EPS) / (long_bv + VAR_EPS)).ln();
    (log_ratio as f32).clamp(-6.0, 6.0)
}

/// Port of `HillTailIndex.h`: the raw (pre-EWMA-smoothing) Hill estimator via the Resnick &
/// Stărică (1997) Hill-plot stability-region k-selector. `window` is sorted in PLACE (mirrors the
/// C++ original's own in-place `partial_sort` -- a full descending sort here, not a partial one,
/// since Rust's std has no `partial_sort` equivalent and a full sort produces an identical
/// top-`(max_k+1)` prefix, the only range this algorithm ever reads). `window` must already
/// contain absolute log-return magnitudes, in ANY order -- this algorithm only ever operates on
/// the sorted magnitude distribution, never on chronological position.
///
/// Returns `None` when the window cannot support an estimate yet, matching `ComputeRawAlpha`'s
/// own early-exit convention -- an explicit optional, not an implicit sentinel float, per this
/// crate's institutional NaN-handling principle (`calibration.rs`'s `Option<f64>` refactor).
///
/// See `HillTailIndex.h`'s own doc comment for the deliberate precondition simplification this
/// port inherits (`count < tail_cutoff + 1` alone, not the real engine's fuller
/// `!is_full && head_index < tail_cutoff + 1` gate -- behaviorally identical for every
/// configuration this codebase actually constructs).
pub fn hill_tail_index(window: &mut [f64], tail_cutoff: usize) -> Option<f64> {
    let count = window.len();
    if count < tail_cutoff + 1 {
        return None;
    }

    let max_k = (tail_cutoff * 4).min(count - 1);
    if max_k < 2 {
        return None;
    }

    const MIN_SCAN_K: usize = 10;
    let start_k = MIN_SCAN_K.min(max_k).max(2);

    // Descending sort -- matches std::greater<double>() ordering.
    window.sort_unstable_by(|a, b| b.total_cmp(a));

    let mut hill_at_k: Vec<f64> = Vec::with_capacity(max_k - start_k + 1);
    // Prime the prefix sum with the first `start_k` log-magnitudes, so running_log_sum ==
    // sum_{i<k} log(x_i) at the top of each iteration. hill_at_k index j corresponds to
    // k = start_k + j.
    let mut running_log_sum: f64 = window[..start_k].iter().map(|x| x.ln()).sum();
    for k in start_k..=max_k {
        if k > start_k {
            running_log_sum += window[k - 1].ln();
        }
        let threshold = window[k];
        if threshold <= 1e-9 {
            hill_at_k.push(4.0);
            continue;
        }
        let log_threshold = threshold.ln(); // hoisted: computed once per k, not per i
        let log_sum = running_log_sum - (k as f64) * log_threshold;
        hill_at_k.push(if log_sum > 1e-9 { k as f64 / log_sum } else { 4.0 });
    }

    // Hill-plot bias grows with k, so prefer the FIRST plateau reached scanning outward from
    // small k, not whichever plateau happens to be numerically widest anywhere in the range.
    const STABILITY_BAND_RELATIVE: f64 = 0.15; // 15% band
    const MIN_PLATEAU_WIDTH: usize = 5;
    let mut best_start = 0usize;
    let mut best_len = 1usize; // fallback: widest plateau found, if none meets MIN_PLATEAU_WIDTH
    let mut first_start = 0usize;
    let mut first_len = 0usize; // first plateau meeting MIN_PLATEAU_WIDTH (preferred)
    let mut start = 0usize;
    for i in 1..=hill_at_k.len() {
        let broke_or_end = i == hill_at_k.len()
            || (hill_at_k[i] - hill_at_k[start]).abs() / hill_at_k[start].max(1e-6) > STABILITY_BAND_RELATIVE;
        if broke_or_end {
            let run_len = i - start;
            if run_len > best_len {
                best_len = run_len;
                best_start = start;
            }
            if first_len == 0 && run_len >= MIN_PLATEAU_WIDTH {
                first_start = start;
                first_len = run_len;
            }
            start = i;
        }
    }
    let chosen_start = if first_len > 0 { first_start } else { best_start };
    let chosen_len = if first_len > 0 { first_len } else { best_len };
    if hill_at_k.is_empty() {
        return None;
    }
    let midpoint_idx = (chosen_start + chosen_len / 2).min(hill_at_k.len() - 1);
    Some(hill_at_k[midpoint_idx])
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
    scratch[..n].sort_by(f64::total_cmp);
    let price_mid = n / 2;
    let median_log_p = scratch[price_mid];

    let mut dev_scratch = [0.0f64; MEAN_REV_MAX_LOOKBACK];
    for i in 0..n {
        dev_scratch[i] = (log_prices[i] - median_log_p).abs();
    }
    dev_scratch[..n].sort_by(f64::total_cmp);
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
    ret_scratch[..m].sort_by(f64::total_cmp);
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
    range_scratch.sort_by(f32::total_cmp);
    let med_range = range_scratch[MID];

    let mut vol_scratch = [0.0f32; LIQ_FRAGILITY_WINDOW];
    vol_scratch.copy_from_slice(&sqrt_vol_window[..LIQ_FRAGILITY_WINDOW]);
    vol_scratch.sort_by(f32::total_cmp);
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

/// Port of CarryForwardCalculators.h's `ComputeBurstinessIndex`. Despite the name, this is dim 3's
/// (`log_scale_expansion_ratio`) real caller today, not dim 1's (`burstiness_index`, which moved to
/// `eve::CalculateBurstinessIndex`/`calculate_burstiness_index` below, 2026-08-29) -- a historical
/// artifact of the function name, not a current caller list; see the C++ header's own doc comment.
/// `rv_older_rate` below the numerical floor (near-flat reference window) carries `last_valid_value`
/// forward. `clamp_low`/`clamp_high`: the C++ original defaults to (-6.0, 6.0); callers needing that
/// default must pass it explicitly here (Rust has no default arguments).
pub fn compute_burstiness_index(
    rv_recent_rate: f64,
    rv_older_rate: f64,
    last_valid_value: f32,
    clamp_low: f32,
    clamp_high: f32,
) -> f32 {
    const FLOOR: f64 = 1e-12;
    if rv_older_rate < FLOOR {
        return last_valid_value;
    }
    let ratio = (rv_recent_rate.max(FLOOR) / rv_older_rate).ln() as f32;
    ratio.clamp(clamp_low, clamp_high)
}

/// Port of CarryForwardCalculators.h's `ComputeRelativeRange` (dim 2, `relative_range`):
/// (high - low) / atr. `atr` at/near zero (unpopulated ATR array) carries `last_valid_value` forward.
pub fn compute_relative_range(high: f32, low: f32, atr: f32, last_valid_value: f32) -> f32 {
    const ATR_FLOOR: f32 = 0.00001;
    if atr <= ATR_FLOOR {
        return last_valid_value;
    }
    (high - low) / atr
}

/// Port of CarryForwardCalculators.h's `ComputeFisherInformation` (dim 8, `fisher_info`): Fisher
/// transform of price position within its lookback range. A flat lookback range (`max_price <=
/// min_price`) carries `last_valid_value` forward.
pub fn compute_fisher_information(
    min_price: f32,
    max_price: f32,
    current_price: f32,
    last_valid_value: f32,
) -> f32 {
    if max_price <= min_price {
        return last_valid_value;
    }
    let raw_pos = (current_price - min_price) / (max_price - min_price);
    let x = (2.0 * (raw_pos - 0.5)).clamp(-0.99, 0.99);
    0.5 * ((1.0 + x) / (1.0 - x)).ln()
}

/// Port of CarryForwardCalculators.h's `ComputeAmihudIlliquidity` (dim 11, `amihud_illiquidity`):
/// geometric mean of the window's |log-return|/sqrt(dollar-volume) ratios, computed as
/// `exp(mean of logs)` -- `sum_log_ratio` is the accumulated SUM OF LOGS of each sample's ratio (the
/// caller accumulates `ln(ratio + eps)` per sample, not the raw ratios). Fewer than 2 valid samples
/// carries `last_valid_value` forward.
pub fn compute_amihud_illiquidity(sum_log_ratio: f64, count: i32, last_valid_value: f32) -> f32 {
    if count < 2 {
        return last_valid_value;
    }
    (sum_log_ratio / count as f64).exp() as f32
}

/// Burstiness Index (dim 1, `burstiness_index`): robust Index of Dispersion for Counts (IDC) over
/// K=10 fixed-width time sub-bins spanning the rolling event-timestamp window. Port of
/// EventVelocityEngine.h's `CalculateBurstinessIndex`. `timestamps` must be chronologically ordered
/// (oldest first); the C++ original reads this straight out of a `RingBuffer`, which iterates in that
/// same order.
///
/// Degenerate defaults: fewer than `MIN_SAMPLES` (20) timestamps returns 0.0 (Poisson-neutral,
/// insufficient data); `span == 0` (every tick in the same instant) or a zero median bin count both
/// return +1.0 (maximally bursty by definition, not "unknown"). Range is otherwise exactly [-1, +1]
/// via the Goh & Barabási (2008) bounded transform. See the C++ header's own doc comment for the full
/// derivation (Daley & Vere-Jones 2003 Index of Dispersion for Counts; the `1.58113883` consistency
/// constant is `sqrt(10)/2`, the exact Poisson(10) sigma/MAD ratio, not the standard-Normal `1.4826`).
pub fn calculate_burstiness_index(timestamps: &[u64]) -> f32 {
    const BINS: usize = 10;
    const MIN_SAMPLES: usize = 20;
    let n = timestamps.len();
    if n < MIN_SAMPLES {
        return 0.0;
    }

    let first = timestamps[0];
    let last = timestamps[n - 1];
    let span = last - first;
    if span == 0 {
        return 1.0;
    }

    let mut counts = [0u32; BINS];
    for &t in timestamps {
        let dt = t - first;
        let mut bin = ((dt as f64 / span as f64) * BINS as f64) as usize;
        if bin >= BINS {
            bin = BINS - 1;
        }
        counts[bin] += 1;
    }

    let mut vals = [0.0f32; BINS];
    for i in 0..BINS {
        vals[i] = counts[i] as f32;
    }

    // Median count -- matches this repo's own nth_element(mid=K/2) convention (FeatureScaler.h's
    // RobustLocation()), NOT the textbook averaged-middle-two for even n. A full sort gives the same
    // value at that index as nth_element, since only the single value at `mid` is read.
    let mut sorted = vals;
    const MID: usize = BINS / 2;
    sorted.sort_by(f32::total_cmp);
    let median = sorted[MID];
    if median <= 0.0 {
        return 1.0;
    }

    let mut abs_dev = [0.0f32; BINS];
    for i in 0..BINS {
        abs_dev[i] = (vals[i] - median).abs();
    }
    abs_dev.sort_by(f32::total_cmp);
    let mad = abs_dev[MID];

    const POISSON_CONSISTENCY: f32 = 1.581_138_8;
    let idc = (POISSON_CONSISTENCY * mad) * (POISSON_CONSISTENCY * mad) / median;
    (idc - 1.0) / (idc + 1.0)
}

/// Order-flow asymmetry in [-1, 1]: +1 = all buying (aggressor at ask), -1 = all selling
/// (aggressor at bid), 0 = balanced. Port of OrderFlowAsymmetryEngine.h's
/// `ComputeMicroAsymmetry` (dim 7, `micro_asymmetry`). A true no-trade bar (`ask_volume ==
/// bid_volume == 0`) carries `last_valid_value` forward instead of fabricating a misleading
/// exact-zero "balanced" reading.
pub fn micro_asymmetry(ask_volume: f32, bid_volume: f32, last_valid_value: f32) -> f32 {
    let total = ask_volume + bid_volume;
    if total <= 0.0 {
        return last_valid_value;
    }
    (ask_volume - bid_volume) / total
}

/// Largest window `select_epsilon_for_target_recurrence_rate` supports. Port of
/// RQAEpsilonSelector.h's `kRQASelectorMaxN`. Windows larger than this are clamped to the first
/// `RQA_SELECTOR_MAX_N` samples, matching the C++ original's truncation (`if (n >
/// kRQASelectorMaxN) n = kRQASelectorMaxN;`) rather than silently reading past it.
pub const RQA_SELECTOR_MAX_N: usize = 256;

/// Selects epsilon such that the recurrence rate over `prices` is as close as possible to
/// `target_rr`, via the Schinkel, Dimigen & Marwan (2008) fixed-recurrence-rate method: sort all
/// pairwise absolute distances, epsilon = the distance at the rank implied by the target. Port of
/// RQAEpsilonSelector.h's `SelectEpsilonForTargetRecurrenceRate`.
///
/// `target_rr` is the desired FULL-MATRIX recurrence rate, matching exactly what
/// `recurrence_rate` below measures: RR = recur_count / n^2 with the line of identity (LOI, the
/// i==j diagonal) included. Since this selector can only rank the n(n-1)/2 OFF-diagonal
/// distances, the off-diagonal target is back-solved from the desired full-matrix rate (see the
/// C++ header's own derivation comment for the full algebra). Infeasible targets (`target_rr <=
/// 1/n`, the LOI alone already meets or exceeds the requested rate) return the smallest
/// achievable epsilon (the minimum pairwise distance), matching production's strict `dist <
/// epsilon` test yielding `RR == 1/n` exactly.
pub fn select_epsilon_for_target_recurrence_rate(prices: &[f32], target_rr: f64) -> f64 {
    let n_full = prices.len();
    if n_full < 2 {
        return 1e-6;
    }
    let n = n_full.min(RQA_SELECTOR_MAX_N);
    let prices = &prices[..n];

    let mut distances: Vec<f32> = Vec::with_capacity(n * (n.saturating_sub(1)) / 2);
    for i in 0..n {
        for j in (i + 1)..n {
            distances.push((prices[i] - prices[j]).abs());
        }
    }
    if distances.is_empty() {
        return 1e-6;
    }
    distances.sort_by(f32::total_cmp);

    let nd = n as f64;
    let off_diag_target = (target_rr * nd * nd - nd) / (nd * (nd - 1.0));

    // Infeasible: the LOI alone already meets or exceeds the requested full-matrix rate. Negated
    // form (not `<= 0.0`) is deliberate -- matches the C++ original's `!(offDiagTarget > 0.0)`
    // bit-for-bit, including its NaN handling: a NaN `off_diag_target` (e.g. n==1, division by
    // zero in the n*(n-1) denominator) must also route into this defensive fallback branch, and
    // `NaN > 0.0` and `NaN <= 0.0` are NOT the same false here -- `!(NaN > 0.0)` is `true`,
    // `NaN <= 0.0` is `false`, so rewriting this as `<=` would silently change NaN behavior.
    #[allow(clippy::neg_cmp_op_on_partial_ord)]
    if !(off_diag_target > 0.0) {
        return (distances[0] as f64).max(1e-6);
    }

    let clamped_target = off_diag_target.min(1.0);
    let num_pairs = distances.len();
    // Round (not floor): distances[rank] leaves exactly `rank` distances strictly below epsilon
    // (ties aside), so `rank` IS the achieved M -- rounding puts M closest to the back-solved target.
    let rank_f = (clamped_target * num_pairs as f64 + 0.5).floor().max(0.0);
    let rank = (rank_f as usize).min(num_pairs - 1);

    (distances[rank] as f64).max(1e-6)
}

/// Largest closed-bar window `recurrence_rate` supports. Port of RecurrenceRateEngine.h's
/// `kMaxClosedBars`. Windows larger than this are clamped to the first
/// `RECURRENCE_RATE_MAX_CLOSED_BARS` samples, matching the C++ original's clamp.
pub const RECURRENCE_RATE_MAX_CLOSED_BARS: usize = 256;

/// Recurrence rate (dim 13, `recurrence_rate`): fraction of pairwise distances below `epsilon`
/// across `closed_bars` plus the live point, including the line-of-identity diagonal. Port of
/// RecurrenceRateEngine.h's `RebuildClosedBarWindow` + `ComputeRate`, combined into one stateless
/// call -- both real production call sites (`ContextManager.cpp`'s `s_recurrenceEngine`,
/// `ImbalanceContextManager.h`'s `ComputeImbalanceRecurrenceRate`) always call the two together,
/// gated on bar-close (`if (completedBarCount != s_lastRecurrenceBarCount)`), never streaming
/// across many ticks with the closed-bar window held fixed -- a fresh full-matrix computation is
/// therefore behaviorally identical to the incremental engine for every real input, not a
/// simplification that changes the answer (`RecurrenceRateEngine.h`'s own native test proves
/// exactly this equivalence against a brute-force n*n reference). Result is clamped to [0, 1].
pub fn recurrence_rate(closed_bars: &[f32], live_price: f32, epsilon: f32) -> f32 {
    let closed_count = closed_bars.len().min(RECURRENCE_RATE_MAX_CLOSED_BARS);
    let closed_bars = &closed_bars[..closed_count];
    let n = closed_count + 1;

    let mut closed_pair_count = 0i32;
    for i in 0..closed_count {
        for j in (i + 1)..closed_count {
            if (closed_bars[i] - closed_bars[j]).abs() < epsilon {
                closed_pair_count += 1;
            }
        }
    }

    let mut live_vs_closed = 0i32;
    for &c in closed_bars {
        if (live_price - c).abs() < epsilon {
            live_vs_closed += 1;
        }
    }

    let recur_count = n as i32 + 2 * closed_pair_count + 2 * live_vs_closed; // n = diagonal
    let total_count = (n * n) as f32;
    (recur_count as f32 / total_count).clamp(0.0, 1.0)
}

/// Largest window this estimator supports -- matches `InformationEngine::WINDOW_SIZE_LZ`/
/// `LempelZivComplexity.h`'s own `kLempelZivMaxWindow` exactly (a real wire/behavior contract,
/// not incidental; the C++ side ties them together via a `static_assert`).
pub const LEMPEL_ZIV_MAX_WINDOW: usize = 64;

/// Lempel-Ziv 76 complexity (Kaspar & Schuster 1987) over an adaptive-median-binarized window,
/// normalized by b(n) = n / log2(n). Port of `LempelZivComplexity.h` (itself extracted from
/// `InformationEngine::GetLempelZivComplexity()`, 2026-10-12). `window` must already be in
/// chronological order (oldest first, most recent last) -- the real call site
/// (`InformationEngine::GetLzWindowSnapshot`) does the circular-buffer-to-linear reconstruction
/// before calling this.
///
/// Returns `0.5` (neutral) for `window.len() < 10` -- insufficient data to binarize/estimate
/// meaningfully, matching the C++ original's own sentinel (not NaN: this is a genuinely defined
/// "don't know yet" answer, not a degenerate-input failure).
pub fn lempel_ziv_complexity(window: &[f64]) -> f64 {
    let count = window.len().min(LEMPEL_ZIV_MAX_WINDOW);
    if count < 10 {
        return 0.5;
    }
    let window = &window[..count];

    // 1. Adaptive median threshold.
    let mut sorted: Vec<f64> = window.to_vec();
    sorted.sort_by(f64::total_cmp);
    let median = sorted[count / 2];

    // 2. Construct bit string S via median thresholding.
    let s: Vec<u8> = window.iter().map(|&v| u8::from(v >= median)).collect();

    // 3. LZ76 complexity count: partition S into components S[0..i-1], S[i..i+k-1], ...; at each
    // step, check whether S[i..i+k-1] already appears somewhere in S[0..i+k-2] (the search space
    // extends into the current candidate's own prefix) -- if so it's a copy (extend k),
    // otherwise it's a genuinely new component (increment complexity, advance i, reset k).
    let mut complexity: i32 = 1;
    let n = count as i32;
    let mut i: i32 = 1; // Component 1 is S[0]; the next candidate component starts at i=1.
    let mut k: i32 = 1;
    while i + k <= n {
        let mut found = false;
        for j in 0..i {
            let mut matched = true;
            for x in 0..k {
                if s[(j + x) as usize] != s[(i + x) as usize] {
                    matched = false;
                    break;
                }
            }
            if matched {
                found = true;
                break;
            }
        }
        if found {
            k += 1;
        } else {
            complexity += 1;
            i += k;
            k = 1;
        }
    }

    // 4. Normalize: b(n) = n / log2(n).
    if n <= 1 {
        return 0.0;
    }
    let b_n = f64::from(n) / f64::from(n).log2();
    f64::from(complexity) / b_n
}

#[cfg(test)]
// Several tests assert exact sentinel/boundary return values (e.g. early-return 0.0 for
// below-minimum-samples, clamped -1.0/1.0 extremes) that the implementation guarantees bit-exactly,
// not the result of arithmetic subject to rounding -- strict equality is the correct check there.
#[allow(clippy::float_cmp)]
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
        assert!(sevcik_fractal_dimension(&[100.0f32; 41]).is_nan());
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
        assert!((compute_mean_reversion_z(&prices, 0.0) - 2.822_423).abs() <= 1e-3);
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
        assert!((result - 0.5).abs() <= 1e-3);
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

    // --- compute_burstiness_index: golden values from tests/cpp/test_carry_forward_calculators.cpp ---

    #[test]
    fn burstiness_normal_case_matches_log_ratio() {
        let result = compute_burstiness_index(4.0, 2.0, 0.0, -6.0, 6.0);
        assert!((result - (4.0f64 / 2.0).ln() as f32).abs() <= 1e-4);
    }

    #[test]
    fn burstiness_degenerate_older_rate_carries_forward() {
        assert!((compute_burstiness_index(4.0, 1e-13, 0.42, -6.0, 6.0) - 0.42).abs() <= 1e-4);
    }

    #[test]
    fn burstiness_clamps_extreme_ratio() {
        assert!((compute_burstiness_index(1e12, 1e-12, 0.0, -6.0, 6.0) - 6.0).abs() <= 1e-4);
    }

    #[test]
    fn burstiness_dim3_custom_bounds_preserve_real_extreme() {
        assert!((compute_burstiness_index(0.0021062, 1.0, 0.0, -10.0, 6.0) - (-6.16)).abs() <= 0.01);
    }

    // --- compute_relative_range: golden values from tests/cpp/test_carry_forward_calculators.cpp ---

    #[test]
    fn relative_range_normal_case() {
        assert!((compute_relative_range(105.0, 100.0, 2.5, 0.0) - 2.0).abs() <= 1e-4);
    }

    #[test]
    fn relative_range_degenerate_atr_carries_forward() {
        assert!((compute_relative_range(105.0, 100.0, 0.0, 1.3) - 1.3).abs() <= 1e-4);
    }

    // --- compute_fisher_information: golden values from tests/cpp/test_carry_forward_calculators.cpp ---

    #[test]
    fn fisher_info_midpoint_price_is_zero() {
        assert!((compute_fisher_information(100.0, 110.0, 105.0, 0.0)).abs() <= 1e-4);
    }

    #[test]
    fn fisher_info_near_high_is_positive() {
        let expected = 0.5 * (1.8f32 / 0.2).ln();
        assert!((compute_fisher_information(100.0, 110.0, 109.0, 0.0) - expected).abs() <= 1e-4);
    }

    #[test]
    fn fisher_info_degenerate_flat_price_carries_forward() {
        assert!((compute_fisher_information(100.0, 100.0, 100.0, -0.85) - (-0.85)).abs() <= 1e-4);
    }

    // --- compute_amihud_illiquidity: golden values from tests/cpp/test_carry_forward_calculators.cpp ---

    #[test]
    fn amihud_normal_case_is_geometric_mean() {
        let sum_log_ratio = 0.002f64.ln() + 0.003f64.ln() + 0.001f64.ln();
        let result = compute_amihud_illiquidity(sum_log_ratio, 3, 0.0);
        assert!((result - 0.0018171206).abs() <= 1e-6);
    }

    #[test]
    fn amihud_degenerate_count_carries_forward() {
        assert!((compute_amihud_illiquidity(0.006f64.ln(), 1, 0.42) - 0.42).abs() <= 1e-4);
    }

    // --- calculate_burstiness_index: golden values from tests/cpp/test_event_velocity_engine.cpp ---

    #[test]
    fn calc_burstiness_below_minimum_samples_is_neutral() {
        let ts: Vec<u64> = (0..10_000_000u64).step_by(1_000_000).collect();
        assert_eq!(calculate_burstiness_index(&ts), 0.0);
    }

    #[test]
    fn calc_burstiness_regular_spacing_is_minimum() {
        let ts: Vec<u64> = (0..20_000_000u64).step_by(1_000_000).collect();
        assert_eq!(calculate_burstiness_index(&ts), -1.0);
    }

    #[test]
    fn calc_burstiness_all_same_instant_is_maximum() {
        let ts = vec![5_000_000u64; 20];
        assert_eq!(calculate_burstiness_index(&ts), 1.0);
    }

    #[test]
    fn calc_burstiness_extreme_clustering_is_maximum() {
        let mut ts = vec![0u64; 19];
        ts.push(100_000_000);
        assert_eq!(calculate_burstiness_index(&ts), 1.0);
    }

    #[test]
    fn calc_burstiness_irregular_spacing_matches_verified_idc() {
        let ts: Vec<u64> = vec![
            0, 50_000, 100_000, 300_000, 1_100_000, 1_150_000, 1_200_000, 1_250_000, 2_150_000,
            2_250_000, 2_350_000, 3_050_000, 3_100_000, 3_150_000, 3_200_000, 3_800_000,
            4_000_000, 4_050_000, 4_950_000, 5_000_000,
        ];
        assert!((calculate_burstiness_index(&ts) - 0.6666667).abs() <= 1e-4);
    }

    // --- micro_asymmetry: golden values from tests/cpp/test_order_flow_asymmetry_engine.cpp ---

    #[test]
    fn micro_asymmetry_balanced_flow_is_zero() {
        assert!((micro_asymmetry(50.0, 50.0, 0.0) - 0.0).abs() <= 1e-6);
    }

    #[test]
    fn micro_asymmetry_all_buying_is_plus_one() {
        assert!((micro_asymmetry(100.0, 0.0, 0.0) - 1.0).abs() <= 1e-6);
    }

    #[test]
    fn micro_asymmetry_all_selling_is_minus_one() {
        assert!((micro_asymmetry(0.0, 100.0, 0.0) - (-1.0)).abs() <= 1e-6);
    }

    #[test]
    fn micro_asymmetry_partial_buy_skew() {
        assert!((micro_asymmetry(75.0, 25.0, 0.0) - 0.5).abs() <= 1e-6);
    }

    #[test]
    fn micro_asymmetry_no_trade_bar_with_no_prior_value_returns_neutral() {
        assert!((micro_asymmetry(0.0, 0.0, 0.0) - 0.0).abs() <= 1e-6);
    }

    #[test]
    fn micro_asymmetry_no_trade_bar_carries_forward_prior_value() {
        assert!((micro_asymmetry(0.0, 0.0, 0.7) - 0.7).abs() <= 1e-6);
    }

    #[test]
    fn micro_asymmetry_negative_no_trade_carry_forward() {
        assert!((micro_asymmetry(0.0, 0.0, -0.35) - (-0.35)).abs() <= 1e-6);
    }

    // --- select_epsilon_for_target_recurrence_rate / recurrence_rate: golden values from
    // tests/cpp/test_rqa_epsilon.cpp and tests/cpp/test_recurrence_rate_engine.cpp ---

    // Replicates production's full n*n matrix measurement verbatim (line of identity included,
    // strict `dist < epsilon`, symmetric pairs counted twice) -- the same independent reference
    // both C++ test files use to prove the selector/engine against, not the functions under test.
    fn measured_full_matrix_rr(prices: &[f32], epsilon: f64) -> f64 {
        let n = prices.len();
        let mut recur_count = n as i64;
        for i in 0..n {
            for j in (i + 1)..n {
                if ((prices[i] - prices[j]).abs() as f64) < epsilon {
                    recur_count += 2;
                }
            }
        }
        recur_count as f64 / (n * n) as f64
    }

    #[test]
    fn epsilon_selector_low_volatility_hits_target_rr() {
        const N: usize = 30;
        const TARGET: f64 = 0.05;
        const TOL: f64 = 0.008;
        let low_vol = make_walk(N, 0.05, 12345);
        let eps = select_epsilon_for_target_recurrence_rate(&low_vol, TARGET);
        let rr = measured_full_matrix_rr(&low_vol, eps);
        assert!((rr - TARGET).abs() < TOL);
        assert!(eps > 0.0);
    }

    #[test]
    fn epsilon_selector_high_volatility_hits_same_target_with_larger_epsilon() {
        const TARGET: f64 = 0.05;
        let low_vol = make_walk(30, 0.05, 12345);
        let eps_low = select_epsilon_for_target_recurrence_rate(&low_vol, TARGET);
        let high_vol = make_walk(30, 5.0, 987_654_321);
        let eps_high = select_epsilon_for_target_recurrence_rate(&high_vol, TARGET);
        let rr_high = measured_full_matrix_rr(&high_vol, eps_high);
        assert!((rr_high - TARGET).abs() < 0.008);
        assert!(eps_high > eps_low * 10.0);
    }

    #[test]
    fn epsilon_selector_is_scale_invariant() {
        const N: usize = 30;
        const TARGET: f64 = 0.05;
        let low_vol = make_walk(N, 0.05, 12345);
        let eps_low = select_epsilon_for_target_recurrence_rate(&low_vol, TARGET);
        let rr_low = measured_full_matrix_rr(&low_vol, eps_low);

        let scaled: Vec<f32> = low_vol.iter().map(|&p| 100.0 + (p - 100.0) * 250.0).collect();
        let eps_scaled = select_epsilon_for_target_recurrence_rate(&scaled, TARGET);
        let rr_scaled = measured_full_matrix_rr(&scaled, eps_scaled);
        assert!((rr_scaled - rr_low).abs() < 1e-12);
    }

    #[test]
    fn epsilon_selector_infeasible_target_falls_back_to_smallest_achievable_rr() {
        const N: usize = 30;
        let low_vol = make_walk(N, 0.05, 12345);
        let eps = select_epsilon_for_target_recurrence_rate(&low_vol, 0.02);
        let rr = measured_full_matrix_rr(&low_vol, eps);
        assert!((rr - 1.0 / N as f64).abs() < 1e-12);
        assert!(eps > 0.0);
    }

    #[test]
    fn epsilon_selector_n40_hits_target_rr() {
        const TARGET: f64 = 0.05;
        let wide = make_walk(40, 0.4, 42);
        let eps = select_epsilon_for_target_recurrence_rate(&wide, TARGET);
        let rr = measured_full_matrix_rr(&wide, eps);
        assert!((rr - TARGET).abs() < 0.008);
    }

    #[test]
    fn recurrence_rate_matches_brute_force_n30() {
        let walk = make_walk(30, 0.5, 12345);
        let closed = &walk[1..]; // 29 closed bars, walk[0] plays the live point
        let epsilon = 0.7f32;
        for &live_price in &[walk[0], walk[0] + 0.2, walk[0] - 1.3, walk[5], walk[10] + 0.05] {
            let mut full_window = vec![live_price];
            full_window.extend_from_slice(closed);
            let expected = measured_full_matrix_rr(&full_window, epsilon as f64);
            let actual = recurrence_rate(closed, live_price, epsilon);
            assert!((expected - actual as f64).abs() < 1e-6);
        }
    }

    #[test]
    fn recurrence_rate_matches_brute_force_n150() {
        let walk = make_walk(150, 1.2, 999);
        let closed = &walk[1..];
        let epsilon = 1.5f32;
        let mut full_window = vec![walk[0]];
        full_window.extend_from_slice(closed);
        let expected = measured_full_matrix_rr(&full_window, epsilon as f64);
        let actual = recurrence_rate(closed, walk[0], epsilon);
        assert!((expected - actual as f64).abs() < 1e-6);
    }

    #[test]
    fn recurrence_rate_oversized_window_is_clamped_not_overrun() {
        // All-identical values -- doesn't matter which elements survive the clamp, only that this
        // doesn't panic/overrun and returns a sane, fully-recurrent result (every pairwise distance
        // is 0.0, strictly below any positive epsilon).
        let big = vec![1.0f32; RECURRENCE_RATE_MAX_CLOSED_BARS + 10];
        let actual = recurrence_rate(&big, 1.0, 0.5);
        assert!((actual - 1.0).abs() <= 1e-6);
    }

    #[test]
    fn recurrence_rate_is_unit_agnostic_return_scale_reuse() {
        // Mirrors recurrence_rate's real activity-clock production call: a 100-element array of
        // return-scale (not price-scale) values, epsilon selected over all 100, first 99 as
        // "closed," the 100th as "live" (test_recurrence_rate_engine.cpp's own reuse check).
        let mut rng = Lcg(7);
        let returns: Vec<f32> = (0..100)
            .map(|_| (0.002 * (2.0 * rng.next_unit() - 1.0)) as f32)
            .collect();
        let epsilon = select_epsilon_for_target_recurrence_rate(&returns, 0.05);
        let actual = recurrence_rate(&returns[..99], returns[99], epsilon as f32);
        let expected = measured_full_matrix_rr(&returns, epsilon);
        assert!((expected - actual as f64).abs() < 1e-6);
        assert!(actual.is_finite() && (0.0..=1.0).contains(&actual));
    }

    #[test]
    fn lempel_ziv_complexity_below_ten_is_neutral() {
        for n in 0..10 {
            let window = vec![0.0f64; n];
            assert_eq!(lempel_ziv_complexity(&window), 0.5, "n={n}");
        }
    }

    #[test]
    fn lempel_ziv_complexity_constant_sequence_matches_closed_form() {
        let window = vec![5.0f64; 64];
        let n = 64.0_f64;
        let b_n = n / n.log2();
        let actual = lempel_ziv_complexity(&window);
        assert!((actual - 1.0 / b_n).abs() < 1e-12, "actual={actual}");
    }

    #[test]
    fn lempel_ziv_complexity_alternating_sequence_is_finite_and_positive() {
        let window: Vec<f64> = (0..64).map(|i| if i % 2 == 0 { 1.0 } else { -1.0 }).collect();
        let actual = lempel_ziv_complexity(&window);
        assert!(actual.is_finite() && actual > 0.0, "actual={actual}");
    }

    #[test]
    fn lempel_ziv_complexity_longer_than_max_window_is_clamped_not_overrun() {
        // Mirrors recurrence_rate_oversized_window_is_clamped_not_overrun's own precedent: an
        // over-long input must not panic/overrun, and must read only the first
        // LEMPEL_ZIV_MAX_WINDOW elements (matching the C++ original's fixed-capacity buffer).
        let mut exact = vec![0.0f64; LEMPEL_ZIV_MAX_WINDOW];
        for (i, v) in exact.iter_mut().enumerate() {
            *v = (i as f64 * 0.37).sin();
        }
        let mut oversized = exact.clone();
        oversized.push(999.0); // beyond the cap, must be ignored
        oversized.push(-999.0);
        assert_eq!(lempel_ziv_complexity(&exact), lempel_ziv_complexity(&oversized));
    }

    #[test]
    fn log_scale_ratio_equal_bvs_is_exactly_zero() {
        assert_eq!(log_scale_ratio(0.05, 0.05), 0.0);
    }

    #[test]
    fn log_scale_ratio_short_greater_is_positive_matches_closed_form() {
        let r = log_scale_ratio(0.20, 0.05);
        let expected = ((0.20_f64 + 1e-12) / (0.05 + 1e-12)).ln() as f32;
        assert!((r - expected).abs() < 1e-6, "r={r} expected={expected}");
    }

    #[test]
    fn log_scale_ratio_short_less_is_negative() {
        assert!(log_scale_ratio(0.01, 0.10) < 0.0);
    }

    #[test]
    fn log_scale_ratio_both_zero_is_exactly_zero() {
        assert_eq!(log_scale_ratio(0.0, 0.0), 0.0);
    }

    #[test]
    fn log_scale_ratio_extreme_values_clamp_to_bounds() {
        assert_eq!(log_scale_ratio(1e6, 1e-9), 6.0);
        assert_eq!(log_scale_ratio(1e-9, 1e6), -6.0);
    }

    fn make_positive_noise(n: usize, scale: f64, seed: u32) -> Vec<f64> {
        let mut rng = Lcg(seed);
        (0..n).map(|_| scale * (0.1 + rng.next_unit())).collect()
    }

    #[test]
    fn hill_tail_index_below_threshold_is_none() {
        let mut window = make_positive_noise(20, 0.01, 1);
        assert!(hill_tail_index(&mut window, 25).is_none());
    }

    #[test]
    fn hill_tail_index_real_production_config_produces_finite_positive_value() {
        let mut window = make_positive_noise(500, 0.001, 7);
        let result = hill_tail_index(&mut window, 25);
        assert!(result.is_some());
        let alpha = result.expect("checked is_some above");
        assert!(alpha.is_finite() && alpha > 0.0, "alpha={alpha}");
    }

    #[test]
    fn hill_tail_index_tiny_but_valid_window_produces_a_value() {
        // tail_cutoff=1 -> count(3) >= tail_cutoff+1(2) passes the first gate;
        // max_k=min(4,2)=2, which is NOT < 2, so this should still produce a value.
        let mut window = make_positive_noise(3, 0.01, 3);
        assert!(hill_tail_index(&mut window, 1).is_some());
    }

    #[test]
    fn hill_tail_index_is_deterministic() {
        let mut window_a = make_positive_noise(500, 0.002, 99);
        let mut window_b = window_a.clone();
        let result_a = hill_tail_index(&mut window_a, 25);
        let result_b = hill_tail_index(&mut window_b, 25);
        assert_eq!(result_a, result_b);
    }
}
