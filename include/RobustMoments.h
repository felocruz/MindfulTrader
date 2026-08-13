// RobustMoments.h — pure, header-only Bowley skewness / Moors kurtosis over a
// fixed-size returns window. Zero ACSIL dependency, natively testable
// (tests/cpp/test_robust_moments.cpp), same extraction rationale as
// FeatureScaler.h/TailRiskEngine.h/InformationEngine.h. Implements Kim &
// White (2004)'s recommended robust replacement for moment-based
// skewness/kurtosis, per docs/superpowers/specs/2026-08-13-observation-
// vector-institutional-elevation-spec.md Unit 3.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

/// Linear-interpolation empirical quantile (R's default "Type 7" method) over
/// an already-sorted array.
template <size_t N>
float EmpiricalQuantile(const std::array<float, N>& sorted, double p) {
    const double idx = p * static_cast<double>(N - 1);
    const size_t lo = static_cast<size_t>(std::floor(idx));
    const size_t hi = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) return sorted[lo];
    const double frac = idx - static_cast<double>(lo);
    return static_cast<float>(static_cast<double>(sorted[lo]) +
                               frac * (static_cast<double>(sorted[hi]) - static_cast<double>(sorted[lo])));
}

/// Bowley (1920) quartile skewness: (Q3 - 2*Q2 + Q1) / (Q3 - Q1).
/// Returns NaN if Q3==Q1 (degenerate window) -- caller must carry-forward,
/// same convention as the existing SKEW_VARIANCE_EPS guard it replaces.
inline float BowleySkewness(std::array<float, 100> returns) {
    std::sort(returns.begin(), returns.end());
    const float q1 = EmpiricalQuantile(returns, 0.25);
    const float q2 = EmpiricalQuantile(returns, 0.50);
    const float q3 = EmpiricalQuantile(returns, 0.75);
    const float denom = q3 - q1;
    if (std::fabs(denom) < 1e-10f) return std::numeric_limits<float>::quiet_NaN();
    return (q3 - 2.0f * q2 + q1) / denom;
}

/// Moors (1988) octile kurtosis: [Q(7/8)-Q(5/8)+Q(3/8)-Q(1/8)] / [Q(6/8)-Q(2/8)].
/// Returns NaN if Q(6/8)==Q(2/8) (degenerate window) -- caller must carry-forward.
inline float MoorsKurtosis(std::array<float, 100> returns) {
    std::sort(returns.begin(), returns.end());
    const float q1_8 = EmpiricalQuantile(returns, 1.0 / 8.0);
    const float q2_8 = EmpiricalQuantile(returns, 2.0 / 8.0);
    const float q3_8 = EmpiricalQuantile(returns, 3.0 / 8.0);
    const float q5_8 = EmpiricalQuantile(returns, 5.0 / 8.0);
    const float q6_8 = EmpiricalQuantile(returns, 6.0 / 8.0);
    const float q7_8 = EmpiricalQuantile(returns, 7.0 / 8.0);
    const float denom = q6_8 - q2_8;
    if (std::fabs(denom) < 1e-10f) return std::numeric_limits<float>::quiet_NaN();
    return (q7_8 - q5_8 + q3_8 - q1_8) / denom;
}
