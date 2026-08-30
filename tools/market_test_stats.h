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

struct BootstrapGapResult {
    double gap;
    double ci_lo;
    double ci_hi;
};

// Mirrors tools/dim_acceptance_eval.py's bootstrap_mean_gap_ci() methodology
// (lines 179-186): point estimate is the exact gap between the two groups'
// mean |value| (no randomness); the CI comes from n_boot resamples (with
// replacement) of each group independently, taking the 2.5th/97.5th
// percentile of the resampled gap distribution. NOT bit-matched to Python's
// PCG64 bitstream (impractical and unnecessary for a stochastic CI estimate;
// see this plan's Global Constraints) -- verified instead via the
// deterministic zero-variance case, where the bootstrap distribution
// collapses to a single point regardless of which PRNG algorithm is used.
//
// CALLER NOTE on the n_boot=2000 default at large n: this function is fast
// for typical group sizes, but at multi-million-element scale (this repo's
// real usage against full-file signal series -- see the in-body comments
// below for the full incident), even the O(n) sequential path this switches
// to above 20,000 elements measures ~255ms/resample. At the default
// n_boot=2000, that's ~8.5 minutes for ONE call -- jump_ratio_eval.cpp calls
// this once per horizon (4 calls per run), so an unmodified default would
// cost ~34 minutes end to end. jump_ratio_eval.cpp explicitly overrides to
// n_boot=200 for exactly this reason (see its call site's comment for the
// arithmetic). Any future caller operating at similar scale (multi-million-
// element decile/quantile groups from a full tick-series signal) should
// benchmark and consider the same override -- don't rediscover this by
// running the real file and waiting.
inline BootstrapGapResult ComputeBootstrapMeanGapCI(
    const std::vector<double>& top, const std::vector<double>& bottom,
    std::size_t n_boot = 2000, std::uint64_t seed = 0) {
    auto mean_abs = [](const std::vector<double>& v) {
        double sum = 0.0;
        for (double x : v) sum += std::fabs(x);
        return sum / static_cast<double>(v.size());
    };
    const double point_gap = mean_abs(top) - mean_abs(bottom);

    if (n_boot == 0) {
        // No resamples -> no bootstrap distribution to read a percentile from.
        // Without this guard, gaps.size() - 1 wraps std::size_t to SIZE_MAX
        // below and indexes far past an empty vector.
        return {point_gap, std::numeric_limits<double>::quiet_NaN(),
                std::numeric_limits<double>::quiet_NaN()};
    }

    // Precompute |value| once: every resample only ever needs the absolute
    // value, never the signed original.
    std::vector<double> top_abs(top.size());
    for (std::size_t i = 0; i < top.size(); ++i) top_abs[i] = std::fabs(top[i]);
    std::vector<double> bottom_abs(bottom.size());
    for (std::size_t i = 0; i < bottom.size(); ++i) bottom_abs[i] = std::fabs(bottom[i]);

    std::mt19937_64 rng(seed);
    std::vector<double> gaps(n_boot);

    // Exact multinomial resampling (draw n indices with replacement) costs
    // one random *memory gather* per drawn element. Measured directly on
    // this platform against a ~3.85M-element array (this tool's real decile
    // group size against the full 38.5M-row production file): 728.8ms per
    // resample, i.e. ~95ns/gather -- the array doesn't fit L2/L3, so this is
    // memory-LATENCY-bound, not throughput-bound, and no amount of loop
    // fusion or faster modulo fixes that (both were tried and measured; the
    // first attempt only removed a redundant second pass, still ~95ns/gather
    // dominated). At n_boot=2000 that's ~24 minutes per horizon, ~97 minutes
    // for a real 4-horizon run -- confirmed empirically, not estimated: an
    // actual run against the production file was still on horizon 2/4 after
    // 61 minutes before being killed.
    //
    // Below kExactResampleThreshold, this stays the exact multinomial
    // resample (matches dim_acceptance_eval.py's literal bootstrap). Note:
    // the RNG-to-index mapping here (raw rng() % n against precomputed abs
    // arrays) is NOT byte-identical to the pre-rewrite version (which used
    // std::uniform_int_distribution against the signed arrays) -- an earlier
    // version of this comment incorrectly claimed the code was unchanged.
    // What's actually unchanged, and what was verified, is that both
    // pre-existing native test suites (test_market_test_stats_magnitude.cpp,
    // test_drift_location_stats.cpp) still pass with identical assertions --
    // point-estimate exactness, CI ordering, and the zero-variance collapse,
    // none of which pin an exact stochastic value. Plain modulo bias against
    // mt19937_64 at n<20,000 is statistically negligible.
    //
    // At or above the threshold, switch to a weighted/"exchangeable"
    // bootstrap (general theory: Praestgaard & Wellner 1993, "Exchangeably
    // Weighted Bootstraps of the General Empirical Process"): assign each
    // element an i.i.d. weight and take the weighted mean
    // sum(w_i*|x_i|)/sum(w_i). Critically, the weight distribution's
    // VARIANCE must match the multinomial resample's implied per-element
    // variance (=1, e.g. Poisson(1) or Exponential(1)) for the result to be
    // correctly scaled -- an earlier version of this code used Uniform[0,2]
    // weights (variance 1/3) purely because they were cheap to generate,
    // without checking this, and a review caught that it produces a CI
    // systematically ~1.7-1.8x too narrow (confirmed via delta-method
    // derivation and an empirical side-by-side rerun against identical
    // synthetic data: exact-bootstrap CI width 0.010902 vs. Uniform[0,2]
    // 0.005944 vs. Exponential(1) 0.010121 -- matching the exact bootstrap).
    // Exponential(1) (Rubin 1981's Bayesian-bootstrap weight) has variance 1
    // and is used here. Dividing by the realized sum(w_i) rather than n keeps
    // a constant array collapsing to that exact constant regardless of the
    // weight draw (a weighted average of a constant is always that
    // constant), so this doesn't reintroduce a zero-variance edge case. This
    // is O(n) *sequential* per resample: a memory-bandwidth-bound streaming
    // pass instead of a memory-latency-bound random gather -- the actual
    // fix for the original performance emergency (a ~3.85M-element decile
    // group measured at ~95ns/gather, ~728.8ms/resample, ~97 minutes for a
    // real 4-horizon run). Two faster-but-wrong-variance alternatives were
    // tried and rejected before this: Poisson(1) (Chamandy, Muralidharan,
    // Najmi & Naidu 2012, "Estimating Uncertainty for Massive Data Streams")
    // has the correct variance but std::poisson_distribution measured
    // ~73ns/draw, almost as expensive as the gather it replaced; Uniform[0,2]
    // measured ~35ns/draw but has the wrong variance (the bug above).
    // Exponential(1) measured ~28.5ns/draw -- both fast and correctly scaled.
    constexpr std::size_t kExactResampleThreshold = 20'000;
    const std::size_t top_n = top_abs.size();
    const std::size_t bottom_n = bottom_abs.size();
    if (top_n < kExactResampleThreshold && bottom_n < kExactResampleThreshold) {
        for (std::size_t b = 0; b < n_boot; ++b) {
            double top_sum = 0.0;
            for (std::size_t i = 0; i < top_n; ++i) {
                top_sum += top_abs[rng() % top_n];
            }
            double bottom_sum = 0.0;
            for (std::size_t i = 0; i < bottom_n; ++i) {
                bottom_sum += bottom_abs[rng() % bottom_n];
            }
            gaps[b] = (top_sum / static_cast<double>(top_n)) - (bottom_sum / static_cast<double>(bottom_n));
        }
    } else {
        std::exponential_distribution<double> w_dist(1.0);
        for (std::size_t b = 0; b < n_boot; ++b) {
            double top_wsum = 0.0, top_wtotal = 0.0;
            for (std::size_t i = 0; i < top_n; ++i) {
                const double w = w_dist(rng);
                top_wsum += w * top_abs[i];
                top_wtotal += w;
            }
            double bottom_wsum = 0.0, bottom_wtotal = 0.0;
            for (std::size_t i = 0; i < bottom_n; ++i) {
                const double w = w_dist(rng);
                bottom_wsum += w * bottom_abs[i];
                bottom_wtotal += w;
            }
            gaps[b] = (top_wsum / top_wtotal) - (bottom_wsum / bottom_wtotal);
        }
    }

    std::sort(gaps.begin(), gaps.end());
    auto percentile = [&](double p) {
        const double idx = p / 100.0 * static_cast<double>(gaps.size() - 1);
        const std::size_t lo_idx = static_cast<std::size_t>(std::floor(idx));
        const std::size_t hi_idx = static_cast<std::size_t>(std::ceil(idx));
        if (lo_idx == hi_idx) return gaps[lo_idx];
        const double frac = idx - static_cast<double>(lo_idx);
        return gaps[lo_idx] * (1.0 - frac) + gaps[hi_idx] * frac;
    };

    BootstrapGapResult result;
    result.gap = point_gap;
    result.ci_lo = percentile(2.5);
    result.ci_hi = percentile(97.5);
    return result;
}

struct MedianGapResult {
    double gap;
    double ci_lo;
    double ci_hi;
};

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
// 2026-08-12-gang-literature-grounding-spec.md). mean(|x|) (the
// ComputeBootstrapMeanGapCI function above, which this mirrors in structure)
// is exactly the kind of statistic that critique warns against when
// evaluating a candidate meant to capture jump/tail behavior; that function
// is kept for other callers/parity with dim_acceptance_eval.py's own
// bootstrap_mean_gap_ci(), which has no median counterpart in the Python
// reference -- this is new, native-only infrastructure, not a straight port.
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
// passes, same complexity class and same memory-bandwidth-bound profile as
// ComputeBootstrapMeanGapCI's one pass), never a random gather, and never a
// per-resample re-sort/re-selection (the O(n) SELECTION cost a naive
// per-resample nth_element would add on top of the weight generation).
inline double MedianAbs(std::vector<double> v) {
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
    constexpr std::size_t kExactResampleThreshold = 20'000;
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

        std::exponential_distribution<double> w_dist(1.0);
        std::vector<double> w_top(top_n), w_bottom(bottom_n);
        for (std::size_t b = 0; b < n_boot; ++b) {
            for (std::size_t i = 0; i < top_n; ++i) w_top[i] = w_dist(rng);
            for (std::size_t i = 0; i < bottom_n; ++i) w_bottom[i] = w_dist(rng);
            gaps[b] = WeightedMedianOfSortedAbs(top_sorted, w_top) -
                      WeightedMedianOfSortedAbs(bottom_sorted, w_bottom);
        }
    }

    std::sort(gaps.begin(), gaps.end());
    auto percentile = [&](double p) {
        const double idx = p / 100.0 * static_cast<double>(gaps.size() - 1);
        const std::size_t lo_idx = static_cast<std::size_t>(std::floor(idx));
        const std::size_t hi_idx = static_cast<std::size_t>(std::ceil(idx));
        if (lo_idx == hi_idx) return gaps[lo_idx];
        const double frac = idx - static_cast<double>(lo_idx);
        return gaps[lo_idx] * (1.0 - frac) + gaps[hi_idx] * frac;
    };

    MedianGapResult result;
    result.gap = point_gap;
    result.ci_lo = percentile(2.5);
    result.ci_hi = percentile(97.5);
    return result;
}
