// ActivityClockMeanReversion.h -- pure, header-only, ACSIL-independent
// activity-clock mean-reversion elasticity score. A PARALLEL implementation
// to StudyHelperFunctions.cpp's CalculateMeanReversionSpeed, NOT a shared
// extraction (unlike SevcikFractalDimension.h/DfaHurstExponent.h) --
// CalculateMeanReversionSpeed's z-score operates on real log-prices from
// sc.BaseData; ImbalanceBarEngine only ever holds per-bar log-RETURNS, so
// there is no shared price-shaped interface to delegate through. Do not go
// looking for a delegation call between the two; there isn't one.
//
// score = |z(cumulative log-return path)| * clamp(1 - max(rho, 0), 0, 1),
// clamped to [0, 5] -- same elasticity-gated z-score shape as
// CalculateMeanReversionSpeed, computed over a different (activity) clock:
// - z-score: reconstructs a within-window log-price-like path via the
//   cumulative sum of the buffered log-returns (log-price is definitionally
//   the cumulative sum of log-returns) and z-scores that path. This is
//   RELATIVE TO THE START OF THE BUFFERED WINDOW, not an absolute price
//   level, since the buffer only ever holds a rolling tail -- harmless for
//   a z-score (which only depends on the window's own median/scale), but a
//   real distinction worth knowing before debugging this.
// - rho: lag-1 autocorrelation of the log-returns directly, identical
//   formula to the original (positive rho => momentum, dampens the score;
//   negative/zero rho => full elasticity).
//
// REFORMULATED 2026-09-02 to median/MAD (Kim & White 2004), matching
// CalculateMeanReversionSpeed's own 2026-08-31 reformulation
// (StudyHelperFunctions.cpp) -- this file was never updated when its sibling
// was, so it was about to become production's only remaining
// mean/std-based (Gaussian-moment) mean-reversion formula the moment it got
// wired in (docs/superpowers/specs/2026-08-31-elite-feature-set-curation-
// initiative.md row 19, "confirmed unwired... needs its own wire-or-drop
// decision"). Fixed before that wiring happens, not after: both the
// cumulative-path z-score AND the lag-1 autocorrelation's centering now use
// this repo's own established nth_element(mid=n/2) median/MAD convention
// (FeatureScaler.h's RobustLocation(), EventVelocityEngine.h's
// CalculateBurstinessIndex) instead of mean/std -- zero live consumers
// exist yet, so this is a pure formula correction, not a behavior-changing
// migration of anything already running. Verified via real python3
// execution against the existing test fixtures below (not hand arithmetic):
// momentum fixture 0.0398->0.0397, mean-reverting fixture 1.502->1.141 --
// both finite, both preserve the qualitative ordering
// (mean-reverting > momentum) this header exists to encode.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

/// Largest window this estimator supports -- matches
/// ImbalanceBarEngine::kImbalanceBarBufferCapacity (include/ImbalanceBarEngine.h),
/// the only real caller of this function, same fixed-capacity-scratch-buffer
/// convention as DfaHurstExponent.h's kDfaMaxWindow.
inline constexpr int kActivityClockMeanRevMaxWindow = 500;

/// Returns NaN for a degenerate window (flat cumulative path, or n too
/// small) -- caller owns the carry-forward/cold-start fallback decision,
/// same convention as SevcikFractalDimension.h/DfaHurstExponent.h.
inline float ActivityClockMeanRevZ(const float* logReturns, int n) {
    if (n < 5 || n > kActivityClockMeanRevMaxWindow) return std::numeric_limits<float>::quiet_NaN();

    constexpr double kMadConsistency = 1.4826;

    // 1. Cumulative log-return path (n+1 points, path[0] = 0).
    std::array<double, kActivityClockMeanRevMaxWindow + 1> path{};
    double cumsum = 0.0;
    path[0] = 0.0;
    for (int i = 0; i < n; ++i) {
        cumsum += static_cast<double>(logReturns[i]);
        path[static_cast<std::size_t>(i + 1)] = cumsum;
    }
    const int count = n + 1;
    const int pathMid = count / 2;

    // Median of the path -- nth_element(mid=count/2), NOT the textbook
    // averaged-middle-two for even count (this repo's own established
    // convention, see FeatureScaler.h's RobustLocation()).
    std::array<double, kActivityClockMeanRevMaxWindow + 1> scratch = path;
    std::nth_element(scratch.begin(), scratch.begin() + pathMid, scratch.begin() + count);
    const double medianPath = scratch[static_cast<std::size_t>(pathMid)];

    for (int i = 0; i < count; ++i) {
        scratch[static_cast<std::size_t>(i)] = std::fabs(path[static_cast<std::size_t>(i)] - medianPath);
    }
    std::nth_element(scratch.begin(), scratch.begin() + pathMid, scratch.begin() + count);
    const double madPath = scratch[static_cast<std::size_t>(pathMid)];
    const double scalePath = madPath * kMadConsistency;
    if (scalePath < 1e-9) return std::numeric_limits<float>::quiet_NaN();

    const double absZPrice = std::fabs((cumsum - medianPath) / scalePath);  // cumsum == path's last point

    // 2. Lag-1 autocorrelation of the log-returns directly, centered on
    // their median (not mean).
    const int m = n - 1;
    if (m < 3) {
        return std::clamp(static_cast<float>(absZPrice), 0.0f, 5.0f);
    }

    std::array<double, kActivityClockMeanRevMaxWindow> returns{};
    for (int i = 0; i < n; ++i) returns[static_cast<std::size_t>(i)] = static_cast<double>(logReturns[i]);
    std::array<double, kActivityClockMeanRevMaxWindow> returnScratch = returns;
    const int retMid = n / 2;
    std::nth_element(returnScratch.begin(), returnScratch.begin() + retMid, returnScratch.begin() + n);
    const double medianR = returnScratch[static_cast<std::size_t>(retMid)];

    double num = 0.0, den = 0.0;
    for (int t = 1; t < n; ++t) {
        const double rt = returns[static_cast<std::size_t>(t)] - medianR;
        const double rPrev = returns[static_cast<std::size_t>(t - 1)] - medianR;
        num += rt * rPrev;
        den += rPrev * rPrev;
    }
    const double rho = (den > 1e-12) ? (num / den) : 0.0;
    const double elasticityGate = std::clamp(1.0 - std::max(rho, 0.0), 0.0, 1.0);
    const double score = absZPrice * elasticityGate;

    return std::clamp(static_cast<float>(score), 0.0f, 5.0f);
}
