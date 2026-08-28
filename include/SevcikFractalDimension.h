// SevcikFractalDimension.h -- pure, header-only, ACSIL-independent Sevcik
// (1998) fractal-dimension estimator. Extracted from StudyHelperFunctions.cpp's
// CalculateFractalDimension so it can be natively tested and driven by
// standalone tools (same extraction rationale/precedent as RobustMoments.h,
// RQAEpsilonSelector.h, TailRiskEngine.h).
//
// D = 1 + ln(L) / ln(2*N), L = sum of normalized Euclidean segment lengths.
//
// Replicates production's exact (asymmetric) windowing, not a "cleaned up"
// version of it: `prices` must hold lookback_n+1 chronological points,
// prices[0] = sc.Index-lookback_n .. prices[lookback_n] = sc.Index (the live,
// still-forming bar). minP/maxP range over prices[1..lookback_n] (INCLUDES
// the live bar); the path-length trace ranges over prices[0..lookback_n-1]
// (EXCLUDES the live bar, includes one bar further back than the min/max
// range does). This one-bar offset between the two ranges exists in the
// original CalculateFractalDimension and is preserved here exactly, not
// corrected -- this header's job is an exact port, not a redesign.

#pragma once

#include <algorithm>
#include <cmath>
#include <limits>

/// Returns NaN for a degenerate (flat, maxP<=minP) or otherwise ill-defined
/// window -- callers own the carry-forward/cold-start fallback decision (this
/// function holds no persistent state, unlike CalculateFractalDimension).
inline float SevcikFractalDimension(const float* prices, int lookback_n) {
    if (lookback_n < 2) return std::numeric_limits<float>::quiet_NaN();

    float minP = prices[1], maxP = prices[1];
    for (int i = 2; i <= lookback_n; ++i) {
        if (prices[i] < minP) minP = prices[i];
        if (prices[i] > maxP) maxP = prices[i];
    }
    if (maxP <= minP) return std::numeric_limits<float>::quiet_NaN();

    const int segments = lookback_n - 1;
    if (segments <= 0) return std::numeric_limits<float>::quiet_NaN();

    double length = 0.0;
    const double priceRange = static_cast<double>(maxP) - static_cast<double>(minP);
    for (int i = 1; i < lookback_n; ++i) {
        const double dy = (static_cast<double>(prices[i]) - static_cast<double>(prices[i - 1])) / priceRange;
        const double dx = 1.0 / static_cast<double>(segments);
        length += std::sqrt(dx * dx + dy * dy);
    }
    if (length <= 0.0) return std::numeric_limits<float>::quiet_NaN();

    const float dim = static_cast<float>(1.0 + std::log(length) / std::log(2.0 * static_cast<double>(segments)));
    return std::clamp(dim, 1.0f, 2.0f);
}
