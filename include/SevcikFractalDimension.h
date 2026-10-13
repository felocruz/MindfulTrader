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

#include "generated/mts_core.h"  // the formulas below are implemented in Rust (spec decision #42)

#include <algorithm>
#include <cmath>
#include <limits>

/// Returns NaN for a degenerate (flat, maxP<=minP) or otherwise ill-defined
/// window -- callers own the carry-forward/cold-start fallback decision (this
/// function holds no persistent state, unlike CalculateFractalDimension).
inline float SevcikFractalDimension(const float* prices, int lookback_n) {
    // The Rust function takes the point count (lookback_n + 1), not the lookback.
    return mts::observation_vector_sevcik_fractal_dimension(prices, lookback_n < 0 ? 0 : static_cast<std::size_t>(lookback_n) + 1);
}
