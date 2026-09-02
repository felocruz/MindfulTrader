// tools/observation_vector/jump_ratio_stats.h
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

    // Computes RV/BV directly (O(window)) for the window starting at
    // window_start -- used once, to seed the sliding accumulators below.
    // Every subsequent window reuses these accumulators via O(1) incremental
    // updates rather than recomputing from scratch.
    auto compute_window = [&](std::size_t window_start) {
        double rv = 0.0;
        for (std::size_t k = window_start; k < window_start + window; ++k) {
            rv += log_returns[k] * log_returns[k];
        }
        double bv_sum = 0.0;
        for (std::size_t k = window_start + 1; k < window_start + window; ++k) {
            bv_sum += std::fabs(log_returns[k - 1]) * std::fabs(log_returns[k]);
        }
        return std::make_pair(rv, bv_sum);
    };
    auto write_jump_ratio = [&](std::size_t window_end_idx, double rv, double bv_sum) {
        if (rv <= 0.0) return;  // leaves jump_ratio[window_end_idx] as NaN
        jump_ratio[window_end_idx] = std::max(0.0, (rv - kHalfPi * bv_sum) / rv);
    };

    auto [rv, bv_sum] = compute_window(0);
    write_jump_ratio(window - 1, rv, bv_sum);
    for (std::size_t window_start = 1; window_start + window <= n; ++window_start) {
        const std::size_t old_idx = window_start - 1;
        const std::size_t new_idx = window_start + window - 1;
        rv += log_returns[new_idx] * log_returns[new_idx] - log_returns[old_idx] * log_returns[old_idx];
        // BV's adjacent-pair sum: drop the pair that fell out of the window
        // (old_idx, old_idx+1), add the pair that entered (new_idx-1, new_idx).
        bv_sum -= std::fabs(log_returns[old_idx]) * std::fabs(log_returns[old_idx + 1]);
        bv_sum += std::fabs(log_returns[new_idx - 1]) * std::fabs(log_returns[new_idx]);
        write_jump_ratio(new_idx, rv, bv_sum);
    }
    return jump_ratio;
}
