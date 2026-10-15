// MeanReversionCalculator.h — pure, header-only mean-reversion elasticity score
// (dim 16, mean_rev_z), extracted from StudyHelperFunctions.cpp's
// CalculateMeanReversionSpeed so it can be natively unit-tested
// (tests/cpp/test_mean_reversion_calculator.cpp) and reused by an offline,
// non-Sierra-Chart tool building the same 18D ObservationData vector directly
// from tick data. Same rationale/precedent as CarryForwardCalculators.h/
// OrderFlowAsymmetryEngine.h/SevcikFractalDimension.h.
//
// The caller (StudyHelperFunctions.cpp) owns the ACSIL price pull (gathering
// the chronological price window ending at the current bar) and the
// persistent-state carry-forward; this file owns only the math: median/MAD
// (Kim & White 2004) price z-score, suppressed by lag-1 return
// autocorrelation elasticity gating.

#pragma once

#include "generated/mts_core.h"  // the formulas below are implemented in Rust (spec decision #42)

#include <algorithm>
#include <array>
#include <cmath>

namespace mrc {

// Matches the [10,40] adaptive observation window contract's upper bound
// (CalculateAdaptiveObservationWindow's own std::clamp(..., 10, 40)) --
// defensive upper bound so the fixed-capacity scratch buffers below can
// never be written out of range.
constexpr int kMaxLookback = 40;

// prices[0..n-1]: chronological price window, prices[n-1] == current
// (still-forming) bar's price. n in [4, kMaxLookback] (n<4 gives m<3,
// skipping the autocorrelation term entirely -- degenerate but not invalid).
// Degenerate (flat window, MAD collapses below the numerical floor) carries
// lastValidValue forward instead of a fabricated exact-zero "no stretch"
// reading. Contract: result >= 0, unbounded above (not clamped -- spec B12).
inline float ComputeMeanReversionZ(const float* prices, int n, float lastValidValue) {
    return mts::observation_vector_compute_mean_reversion_z(prices, n < 0 ? 0 : static_cast<std::size_t>(n), lastValidValue);
}

}  // namespace mrc
