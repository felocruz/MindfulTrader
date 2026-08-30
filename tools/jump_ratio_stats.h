// tools/jump_ratio_stats.h
// Realized variance / bipower variation / jump ratio for the §5.1 jump-ratio
// offline prototype (docs/superpowers/specs/2026-08-29-hmm-fat-tail-
// observation-vector-brainstorm.md §5.1, Barndorff-Nielsen & Shephard
// 2004/2006). No Arrow dependency -- fully natively testable.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

// jump_ratio[i] = max(0, (RV[i] - BV[i]) / RV[i]) over the `window`-wide
// trailing slice of log_returns ending at i, where RV = sum(r^2) and
// BV = (pi/2) * sum(|r_{k-1}| * |r_k|) (Barndorff-Nielsen & Shephard's
// jump-robust continuous-variance estimator). The max(0, ...) floor is
// standard institutional practice (Andersen-Bollerslev-Diebold 2007) --
// finite-sample noise can make BV exceed RV, which would otherwise yield a
// nonsensical negative jump share. O(n) via sliding accumulators for RV's
// sum-of-squares and BV's sum-of-adjacent-products, matching
// drift_location_stats.h's ComputeDriftZScore precedent -- not O(n*window).
inline std::vector<double> ComputeJumpRatio(
    const std::vector<double>& log_returns, std::size_t window) {
    const std::size_t n = log_returns.size();
    std::vector<double> jump_ratio(n, std::numeric_limits<double>::quiet_NaN());
    if (window < 2 || n < window) {
        return jump_ratio;
    }

    constexpr double kHalfPi = 1.5707963267948966;  // pi/2

    auto compute_at = [&](std::size_t window_start) {
        const std::size_t window_end = window_start + window;  // exclusive
        double rv = 0.0;
        for (std::size_t k = window_start; k < window_end; ++k) {
            rv += log_returns[k] * log_returns[k];
        }
        double bv_sum = 0.0;
        for (std::size_t k = window_start + 1; k < window_end; ++k) {
            bv_sum += std::fabs(log_returns[k - 1]) * std::fabs(log_returns[k]);
        }
        const double bv = kHalfPi * bv_sum;
        if (rv <= 0.0) {
            return;  // leaves jump_ratio[window_end - 1] as NaN
        }
        jump_ratio[window_end - 1] = std::max(0.0, (rv - bv) / rv);
    };

    // First window computed directly (O(window)); every subsequent window
    // slides by one element via incremental sum updates (O(1) per step).
    compute_at(0);
    double rv = 0.0, bv_sum = 0.0;
    for (std::size_t k = 0; k < window; ++k) {
        rv += log_returns[k] * log_returns[k];
    }
    for (std::size_t k = 1; k < window; ++k) {
        bv_sum += std::fabs(log_returns[k - 1]) * std::fabs(log_returns[k]);
    }
    for (std::size_t window_start = 1; window_start + window <= n; ++window_start) {
        const std::size_t old_idx = window_start - 1;
        const std::size_t new_idx = window_start + window - 1;
        rv += log_returns[new_idx] * log_returns[new_idx] - log_returns[old_idx] * log_returns[old_idx];
        // BV's adjacent-pair sum: drop the pair that fell out of the window
        // (old_idx, old_idx+1), add the pair that entered (new_idx-1, new_idx).
        bv_sum -= std::fabs(log_returns[old_idx]) * std::fabs(log_returns[old_idx + 1]);
        bv_sum += std::fabs(log_returns[new_idx - 1]) * std::fabs(log_returns[new_idx]);
        const double bv = kHalfPi * bv_sum;
        if (rv > 0.0) {
            jump_ratio[new_idx] = std::max(0.0, (rv - bv) / rv);
        }
    }
    return jump_ratio;
}
