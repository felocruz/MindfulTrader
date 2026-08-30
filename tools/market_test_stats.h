// tools/market_test_stats.h
// Shared model-independent market-outcome testing utilities, extracted from
// tools/drift_location_stats.h on its second real use (tools/jump_ratio_eval.cpp).
// No Arrow dependency -- fully natively testable.
#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
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
