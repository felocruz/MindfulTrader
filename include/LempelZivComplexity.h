// LempelZivComplexity.h -- pure, header-only, ACSIL-independent Lempel-Ziv 76 complexity
// estimator (Kaspar & Schuster 1987), extracted from InformationEngine::GetLempelZivComplexity()
// so it can be natively unit-tested and ported to Rust independent of InformationEngine's own
// stateful ring-buffer/histogram bookkeeping -- same extraction precedent already established by
// MeanReversionCalculator.h/LiquidityFragilityEngine.h (both pulled out of ACSIL-coupled call
// sites for the same reason: a pure function is a Rust-portable, natively-testable unit; a
// stateful class method embedding the same math is neither).
//
// `window` must already be in chronological order (oldest first, most recent last) -- the
// circular-buffer-to-linear reconstruction stays InformationEngine's own responsibility,
// unchanged by this extraction (same division of labor DfaHurstExponent.h/MeanReversionCalculator.h
// already use: the ring buffer snapshot happens at the call site, the pure math takes a plain
// array).
#pragma once

#include "generated/mts_core.h"  // the formulas below are implemented in Rust (spec decision #42)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace MindfulTrader {

// Matches InformationEngine::WINDOW_SIZE_LZ exactly -- a real wire/behavior contract, not
// incidental (see this header's own static_assert in InformationEngine.h, which ties the two
// together so they cannot silently drift apart).
constexpr std::size_t kLempelZivMaxWindow = 64;

// Lempel-Ziv 76 complexity over an adaptive-median-binarized window, normalized by b(n) = n /
// log2(n) (asymptotic complexity of a random binary string, Kaspar & Schuster 1987). Returns 0.5
// (neutral) for `count < 10` -- insufficient data to binarize/estimate meaningfully. `window` must
// point to at least `count` chronologically-ordered elements; `count` is clamped to
// `kLempelZivMaxWindow` by the caller's own fixed-capacity buffer, never bounds-checked here
// (matches DfaHurstExponent.h's fixed-capacity discipline: the caller's buffer size IS the bound).
inline double LempelZivComplexity(const double* window, std::size_t count) {
    return mts::observation_vector_lempel_ziv_complexity(window, count);
}

}  // namespace MindfulTrader
