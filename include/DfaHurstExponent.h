// DfaHurstExponent.h -- pure, header-only, ACSIL-independent Detrended
// Fluctuation Analysis (DFA) Hurst-exponent estimator. Extracted from
// StudyHelperFunctions.cpp's CalculateHurstExponent so it can be natively
// tested and driven by ActivityClockManager's imbalance-bar returns buffer
// directly (same extraction rationale/precedent as SevcikFractalDimension.h/
// RQAEpsilonSelector.h/RecurrenceRateEngine.h).
//
// Despite variable/comment names elsewhere in this codebase saying "R/S",
// this is Detrended Fluctuation Analysis, not classic rescaled-range: a
// de-meaned cumulative profile of log-returns is segmented at each scale s,
// each segment is linearly detrended, and the Hurst exponent is the
// log(fluctuation)-vs-log(scale) regression slope.
//
// `logReturns` must already be log-returns (not prices), oldest first, most
// recent last -- exactly ImbalanceBarEngine::GetImbalanceBarReturns()'s own
// ordering, and exactly what CalculateHurstExponent's own internal
// price-to-return loop produces before this math begins.

#pragma once

#include "generated/mts_core.h"  // the formulas below are implemented in Rust (spec decision #42)

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

/// Largest window this estimator supports (matches CalculateHurstExponent's
/// own capacity constant -- fixed-capacity stack buffers, no heap allocation).
inline constexpr int kDfaMaxWindow = 512;

/// Returns NaN for a degenerate/insufficient-data window -- callers own the
/// carry-forward/cold-start fallback decision (this function holds no
/// persistent state, unlike CalculateHurstExponent).
inline float DfaHurstExponent(const float* logReturns, int length, int minScale) {
    return mts::observation_vector_dfa_hurst_exponent(logReturns, length < 0 ? 0 : static_cast<std::size_t>(length), minScale);
}
