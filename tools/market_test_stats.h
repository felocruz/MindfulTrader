// tools/market_test_stats.h
// Shared model-independent market-outcome testing utilities, extracted from
// tools/drift_location_stats.h on its second real use (tools/jump_ratio_eval.cpp).
// No Arrow dependency -- fully natively testable.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <random>
#include <vector>

// Below this size, resample bootstraps exactly (multinomial, with
// replacement); at or above it, switch to the O(n)-sequential weighted/
// exchangeable-bootstrap path. Used by ComputeBootstrapMedianGapCI (the
// sibling mean-based version was deleted 2026-08-30 -- zero real callers,
// and mean(|x|) is the wrong default for this system's fat-tailed data;
// see market_test_stats.h's git history / the brainstorm doc §10.9 for why).
constexpr std::size_t kExactResampleThreshold = 20'000;

// Percentile via linear interpolation over an ALREADY SORTED array (matches
// numpy's default `interpolation='linear'`). Extracted on its 2nd/3rd real
// use (ComputeBootstrapMedianGapCI and tools/jump_ratio_eval.cpp's own
// Percentile(), which sorts its own copy of an unsorted array first) -- this
// plan's own Task 1 established the "extract on 2nd/3rd real use"
// convention; this dedup applies it to itself.
inline double PercentileFromSorted(const std::vector<double>& sorted_values, double p) {
    const double idx = p / 100.0 * static_cast<double>(sorted_values.size() - 1);
    const std::size_t lo_idx = static_cast<std::size_t>(std::floor(idx));
    const std::size_t hi_idx = static_cast<std::size_t>(std::ceil(idx));
    if (lo_idx == hi_idx) return sorted_values[lo_idx];
    const double frac = idx - static_cast<double>(lo_idx);
    return sorted_values[lo_idx] * (1.0 - frac) + sorted_values[hi_idx] * frac;
}

// Mirrors tools/dim_acceptance_eval.py's compute_forward_returns() semantics
// exactly (lines 189-200: forward log-return from signal_price at signal_ts to
// the first available price at or after signal_ts + horizon, rejected if that
// target is beyond the series or the actual gap exceeds 3x the horizon), but
// as an O(n) two-pointer merge instead of Python's O(n log n) np.searchsorted.
// REQUIRES both signal_ts and timestamps to be sorted ascending (verified via
// a real Polars is_sorted() check against the actual production Parquet
// file for the series this was first used against). This isn't a
// micro-optimization -- an earlier binary-search-per-signal version was
// measured taking 100+ seconds (timed out) on a real 38.5M-row series across
// 4 horizons: each independent std::lower_bound probe jumps unpredictably
// through a ~308MB array that doesn't fit in cache, thrashing it on every
// level of the search. A single forward-only pointer never revisits memory,
// so this stays cache-friendly at that scale.
inline std::vector<double> ComputeForwardReturns(
    const std::vector<std::int64_t>& signal_ts, const std::vector<double>& signal_price,
    const std::vector<std::int64_t>& timestamps, const std::vector<double>& prices,
    int horizon_minutes) {
    const std::int64_t horizon_us = static_cast<std::int64_t>(horizon_minutes) * 60 * 1'000'000LL;
    std::vector<double> fwd(signal_ts.size(), std::numeric_limits<double>::quiet_NaN());
    const std::size_t m = timestamps.size();
    std::size_t idx = 0;
    for (std::size_t i = 0; i < signal_ts.size(); ++i) {
        const std::int64_t target_ts = signal_ts[i] + horizon_us;
        // idx only ever advances across the whole loop: target_ts is
        // non-decreasing in i (signal_ts is sorted, horizon_us is constant),
        // so the first timestamps[idx] >= target_ts is also non-decreasing.
        while (idx < m && timestamps[idx] < target_ts) {
            ++idx;
        }
        if (idx >= m) {
            break;  // every later target_ts is >= this one -- none can match either.
        }
        if ((timestamps[idx] - signal_ts[i]) > horizon_us * 3) {
            continue;
        }
        fwd[i] = std::log(prices[idx] / signal_price[i]);
    }
    return fwd;
}

struct WilsonInterval {
    double lo;
    double hi;
};

// Mirrors tools/dim_acceptance_eval.py's wilson_ci() exactly (lines 171-178).
inline WilsonInterval ComputeWilsonCI(std::size_t k, std::size_t n, double z = 1.96) {
    const double nd = static_cast<double>(n);
    const double phat = static_cast<double>(k) / nd;
    const double denom = 1.0 + z * z / nd;
    const double center = (phat + z * z / (2.0 * nd)) / denom;
    const double half = (z / denom) * std::sqrt(phat * (1.0 - phat) / nd + z * z / (4.0 * nd * nd));
    return {center - half, center + half};
}

// Matches numpy's np.sign() exactly: -1, 0, or +1.
inline int Sign(double x) {
    if (x > 0.0) return 1;
    if (x < 0.0) return -1;
    return 0;
}

struct HitRateResult {
    std::size_t n = 0;
    std::size_t k = 0;
    double hit_rate = 0.0;
    double ci_lo = 0.0;
    double ci_hi = 0.0;
    double z_stat = 0.0;
    double p_value = 1.0;
};

// Mirrors tools/dim_acceptance_eval.py's predictive_power_directional() hit-rate
// test exactly (lines 203-231): tests the SAME-SIGN (continuation) hypothesis --
// candidate and forward return agreeing in sign. This is the right test for a
// signed, directional candidate like drift/location's momentum z-score; it is
// NOT mean_rev_z_variant_comparison.py's negated-sign (reversion) test, and it
// is not a fit at all for a non-negative, non-directional candidate (e.g. a
// jump ratio), which has no sign to test against and needs a magnitude-style
// test instead. Callers must confirm which hypothesis applies to their
// candidate before reusing this function.
inline HitRateResult ComputeHitRate(
    const std::vector<double>& forward_returns, const std::vector<double>& candidate_values) {
    HitRateResult result;
    std::size_t n = 0, k = 0;
    for (std::size_t i = 0; i < forward_returns.size(); ++i) {
        if (!std::isfinite(forward_returns[i])) continue;
        if (candidate_values[i] == 0.0 || !std::isfinite(candidate_values[i])) continue;
        ++n;
        if (Sign(forward_returns[i]) == Sign(candidate_values[i])) {
            ++k;
        }
    }
    result.n = n;
    result.k = k;
    if (n == 0) {
        return result;
    }
    result.hit_rate = static_cast<double>(k) / static_cast<double>(n);
    const double se_null = std::sqrt(0.25 / static_cast<double>(n));
    result.z_stat = (result.hit_rate - 0.5) / se_null;
    result.p_value = 2.0 * (1.0 - 0.5 * (1.0 + std::erf(std::fabs(result.z_stat) / std::sqrt(2.0))));
    const auto ci = ComputeWilsonCI(k, n);
    result.ci_lo = ci.lo;
    result.ci_hi = ci.hi;
    return result;
}

struct MedianGapResult {
    double gap;
    double ci_lo;
    double ci_hi;
};

// Median of |values|. Returns 0.0 for an empty input -- unreachable in this
// codebase's current callers, which all pre-filter with a >=30-sample floor,
// but this is shared, reusable infrastructure and an empty vector must not
// read out of bounds.
// A NaN element gives an unspecified result (std::nth_element's ordering
// with NaN is unspecified by the standard) rather than a defined NaN
// propagation -- safe for this codebase's current callers, which all
// pre-filter with std::isfinite before calling, but not a general-purpose
// guarantee.
inline double MedianAbs(std::vector<double> v) {
    if (v.empty()) return 0.0;
    for (auto& x : v) x = std::fabs(x);
    const std::size_t mid = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + mid, v.end());
    double m = v[mid];
    if (v.size() % 2 == 0) {
        std::nth_element(v.begin(), v.begin() + mid - 1, v.begin() + mid);
        m = (m + v[mid - 1]) / 2.0;
    }
    return m;
}

// PRECONDITION: weights.size() == sorted_abs.size() -- both of this
// function's callers (ComputeBootstrapMedianGapCI's weighted-median path)
// satisfy this by construction, but it is not checked here. A shorter
// weights vector reads out of bounds; a longer one silently ignores the
// extra weights (the loop bound is sorted_abs.size()).
inline double WeightedMedianOfSortedAbs(
    const std::vector<double>& sorted_abs, const std::vector<double>& weights) {
    double total = 0.0;
    for (double w : weights) total += w;
    const double half = total / 2.0;
    double cum = 0.0;
    for (std::size_t i = 0; i < sorted_abs.size(); ++i) {
        cum += weights[i];
        if (cum >= half) return sorted_abs[i];
    }
    return sorted_abs.back();  // unreachable except fp rounding at the last element
}

// Robust magnitude/bootstrap-gap test using MEDIAN, not mean, as the central-
// tendency statistic. This is this codebase's own established convention for
// fat-tailed data, not an alternative choice: FeatureScaler.h's
// RobustLocation() computes "median and MAD x 1.4826 (Taleb-consistent)" for
// every observation-vector dimension, precisely because Kim & White (2004)
// show that moment-based statistics (mean, variance, skewness, kurtosis) are
// most unreliable exactly under the fat-tailed conditions they exist to
// detect -- a handful of extreme values can dominate them. This codebase
// already replaced moment-based skewness/kurtosis with Bowley/Moors robust
// quantile estimators for exactly this reason (see docs/superpowers/specs/
// 2026-08-12-gang-literature-grounding-spec.md). A mean(|x|)-based sibling of
// this function existed briefly (matching dim_acceptance_eval.py's own
// bootstrap_mean_gap_ci() precedent) but was deleted 2026-08-30: it had zero
// real callers, and mean(|x|) is exactly the kind of statistic the critique
// above warns against for a system built around fat-tailed data -- keeping a
// fully-tested, equally-convenient wrong-by-this-codebase's-own-standard
// sibling around was a standing hazard, not a safety net. This function is
// new, native-only infrastructure (the Python reference has no median
// counterpart to port from), not a straight port.
//
// Point estimate: median(|top|) - median(|bottom|), no randomness. Below
// kExactResampleThreshold: exact multinomial resample, one std::nth_element
// selection per resample per group (mirrors FeatureScaler::RobustLocation's
// own nth_element convention for small windows). At or above the threshold:
// a weighted/exchangeable-bootstrap median (Praestgaard & Wellner 1993's
// general theory covers M-estimators, including the median, not only the
// mean). Sort |values| ONCE per group (O(n log n), amortized across all
// n_boot resamples -- negligible next to the O(n_boot*n) work below); for
// each resample, assign every SORTED position an i.i.d. Exponential(1)
// weight (weights are exchangeable, so alignment to sorted order rather than
// original index is valid) and do two linear passes over the fixed sorted
// array: one to sum total weight, one to find the sorted position where
// cumulative weight first reaches half of the total -- that position's value
// is this resample's weighted median. O(n) sequential per resample (two
// passes, memory-bandwidth-bound), never a random gather, and never a
// per-resample re-sort/re-selection (the O(n) SELECTION cost a naive
// per-resample nth_element would add on top of the weight generation).
//
// KNOWN LIMITATION, not yet addressed here: this function resamples
// individual elements as if they were i.i.d., but jump_ratio_eval.cpp's real
// callers pass forward-return signals with heavy autocorrelation/overlap (a
// 240-minute forward return computed per-tick over a 38.5M-row series
// overlaps thousands of neighboring signals) -- textbook i.i.d. bootstrap
// understates the true CI width under this much overlap (by a large,
// unquantified-here factor). This codebase already has a measured block-
// length precedent for exactly this kind of dependent-data problem (Politis-
// White circular block length ~404.82 on real MES data, see CLAUDE.md's
// fractal_dim window-widening entry) that a proper block bootstrap here
// should reuse -- flagged as a real, not-yet-implemented gap in the standing
// §10.7 methodology (shared by drift_location_eval.cpp's already-accepted
// OUT verdict too, not unique to this candidate), not something to patch ad
// hoc for one candidate's test.
//
// Default n_boot=1000 -- the commonly-cited Efron & Tibshirani (1993)
// minimum replicate count for a percentile bootstrap CI, chosen over the
// more conservative 2000 given this function's real measured cost
// (dominated by Exponential(1) generation) against a caller
// (jump_ratio_eval.cpp) explicitly choosing to run at the
// cheaper floor rather than the more conservative 2000.
inline MedianGapResult ComputeBootstrapMedianGapCI(
    const std::vector<double>& top, const std::vector<double>& bottom,
    std::size_t n_boot = 1000, std::uint64_t seed = 0) {
    const double point_gap = MedianAbs(top) - MedianAbs(bottom);

    if (n_boot == 0) {
        return {point_gap, std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
    }

    std::mt19937_64 rng(seed);
    std::vector<double> gaps(n_boot);
    const std::size_t top_n = top.size();
    const std::size_t bottom_n = bottom.size();

    if (top_n < kExactResampleThreshold && bottom_n < kExactResampleThreshold) {
        std::vector<double> top_abs(top_n), bottom_abs(bottom_n);
        for (std::size_t i = 0; i < top_n; ++i) top_abs[i] = std::fabs(top[i]);
        for (std::size_t i = 0; i < bottom_n; ++i) bottom_abs[i] = std::fabs(bottom[i]);
        std::vector<double> rs_top(top_n), rs_bottom(bottom_n);
        for (std::size_t b = 0; b < n_boot; ++b) {
            for (std::size_t i = 0; i < top_n; ++i) rs_top[i] = top_abs[rng() % top_n];
            for (std::size_t i = 0; i < bottom_n; ++i) rs_bottom[i] = bottom_abs[rng() % bottom_n];
            // rs_top/rs_bottom already hold |value| (gathered from top_abs/
            // bottom_abs), so MedianAbs's internal fabs() is a no-op here --
            // reused rather than duplicating the selection logic a third time.
            gaps[b] = MedianAbs(rs_top) - MedianAbs(rs_bottom);
        }
    } else {
        std::vector<double> top_sorted(top_n), bottom_sorted(bottom_n);
        for (std::size_t i = 0; i < top_n; ++i) top_sorted[i] = std::fabs(top[i]);
        for (std::size_t i = 0; i < bottom_n; ++i) bottom_sorted[i] = std::fabs(bottom[i]);
        std::sort(top_sorted.begin(), top_sorted.end());
        std::sort(bottom_sorted.begin(), bottom_sorted.end());

        // WeightedMedianOfSortedAbs's precondition (weights.size() ==
        // sorted_abs.size()) holds here: w_top/w_bottom are sized to top_n/
        // bottom_n, matching top_sorted/bottom_sorted exactly.
        std::exponential_distribution<double> w_dist(1.0);
        std::vector<double> w_top(top_n), w_bottom(bottom_n);
        for (std::size_t b = 0; b < n_boot; ++b) {
            for (std::size_t i = 0; i < top_n; ++i) w_top[i] = w_dist(rng);
            for (std::size_t i = 0; i < bottom_n; ++i) w_bottom[i] = w_dist(rng);
            // Returns the lower crossing element, unlike MedianAbs/the exact
            // path above (which averages the two middle elements for even
            // n) -- an undocumented difference between the two branches, but
            // immaterial in practice: at this path's n>=20,000 scale the gap
            // between adjacent order statistics is orders of magnitude
            // smaller than the bootstrap CI width itself.
            gaps[b] = WeightedMedianOfSortedAbs(top_sorted, w_top) -
                      WeightedMedianOfSortedAbs(bottom_sorted, w_bottom);
        }
    }

    std::sort(gaps.begin(), gaps.end());
    MedianGapResult result;
    result.gap = point_gap;
    result.ci_lo = PercentileFromSorted(gaps, 2.5);
    result.ci_hi = PercentileFromSorted(gaps, 97.5);
    return result;
}
