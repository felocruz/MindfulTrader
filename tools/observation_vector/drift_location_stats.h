// tools/observation_vector/drift_location_stats.h
// Pure numeric functions for the §5.0 drift/location offline prototype.
// Forward-return/Wilson-CI/hit-rate functions moved to market_test_stats.h
// (2026-08-30, extracted on their second real use by jump_ratio_eval.cpp) --
// included here so existing callers of this header see no symbol change.
#pragma once

#include "market_test_stats.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
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
