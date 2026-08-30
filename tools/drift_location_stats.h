// tools/drift_location_stats.h
// Pure numeric functions for the §5.0 drift/location offline prototype
// (docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-
// brainstorm.md). No Arrow dependency -- fully natively testable. Mirrors
// tools/context_validate_stats.h's pure-math/thin-glue split for the same
// reason: keep CLI/Arrow concerns out of the natively-tested layer.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

inline std::vector<double> ComputeLogReturns(const std::vector<double>& prices) {
    std::vector<double> returns;
    if (prices.size() < 2) {
        return returns;
    }
    returns.reserve(prices.size() - 1);
    for (std::size_t i = 1; i < prices.size(); ++i) {
        returns.push_back(std::log(prices[i] / prices[i - 1]));
    }
    return returns;
}

// Volatility-normalized rolling return z-score: z[i] = mean(w) / std(w), where
// w is the `window`-wide trailing slice of log_returns ending at i. O(n) via a
// sliding sum/sum-of-squares accumulator, not O(n*window) -- DOD discipline,
// matters at this tool's real 38.5M-row scale.
inline std::vector<double> ComputeDriftZScore(
    const std::vector<double>& log_returns, std::size_t window) {
    const std::size_t n = log_returns.size();
    std::vector<double> z(n, std::numeric_limits<double>::quiet_NaN());
    if (window == 0 || n < window) {
        return z;
    }

    double sum = 0.0, sum_sq = 0.0;
    for (std::size_t i = 0; i < window; ++i) {
        sum += log_returns[i];
        sum_sq += log_returns[i] * log_returns[i];
    }
    auto write_z = [&](std::size_t idx) {
        const double mean = sum / static_cast<double>(window);
        const double variance = sum_sq / static_cast<double>(window) - mean * mean;
        const double stddev = std::sqrt(std::max(0.0, variance));
        z[idx] = (stddev > 0.0) ? (mean / stddev) : 0.0;
    };
    write_z(window - 1);
    for (std::size_t i = window; i < n; ++i) {
        sum += log_returns[i] - log_returns[i - window];
        sum_sq += log_returns[i] * log_returns[i] - log_returns[i - window] * log_returns[i - window];
        write_z(i);
    }
    return z;
}

// Mirrors tools/dim_acceptance_eval.py's compute_forward_returns() exactly
// (lines 189-200): forward log-return from signal_price at signal_ts to the
// first available price at or after signal_ts + horizon, rejected if that
// target is beyond the series or the actual gap exceeds 3x the horizon
// (guards against overnight/weekend gaps swamping the horizon).
inline std::vector<double> ComputeForwardReturns(
    const std::vector<std::int64_t>& signal_ts, const std::vector<double>& signal_price,
    const std::vector<std::int64_t>& timestamps, const std::vector<double>& prices,
    int horizon_minutes) {
    const std::int64_t horizon_us = static_cast<std::int64_t>(horizon_minutes) * 60 * 1'000'000LL;
    std::vector<double> fwd(signal_ts.size(), std::numeric_limits<double>::quiet_NaN());
    for (std::size_t i = 0; i < signal_ts.size(); ++i) {
        const std::int64_t target_ts = signal_ts[i] + horizon_us;
        auto it = std::lower_bound(timestamps.begin(), timestamps.end(), target_ts);
        if (it == timestamps.end()) {
            continue;
        }
        const std::size_t idx = static_cast<std::size_t>(it - timestamps.begin());
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
// test exactly (lines 203-231): SAME-SIGN (continuation) hypothesis, the correct
// one for a drift/momentum candidate -- NOT mean_rev_z_variant_comparison.py's
// negated-sign reversion test, which is specific to that different candidate.
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
