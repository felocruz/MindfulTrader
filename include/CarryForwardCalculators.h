// CarryForwardCalculators.h — pure, header-only degenerate-guard formulas for
// three observation dims (1 burstiness_index, 2 relative_range, 8 fisher_info)
// that were returning a fixed 0.0f sentinel on routine, non-warmup degenerate
// input (flat range, zero ATR, flat price) instead of carrying the last valid
// physics reading forward. That fixed sentinel became frequent enough to anchor
// FeatureScaler's rolling median, collapsing the exported, scaled observation to
// an exact 0.0 (docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md).
//
// Extracted so the guard logic can be natively unit-tested
// (tests/cpp/test_carry_forward_calculators.cpp) — same rationale as
// FeatureScaler.h/OrderFlowAsymmetryEngine.h: zero Sierra Chart/ACSIL dependency.
// Callers (StudyHelperFunctions.cpp/TripleScreen2.cpp) own the persistent
// last-valid-value state and the window-scanning/history-lookback that feeds
// these pure formulas; only the true cold-start branch (insufficient bar
// history for the lookback window) is unaffected and stays 0.0f at the caller.

#pragma once

#include <algorithm>
#include <cmath>

namespace cfc {

// Dim 1 (burstiness_index): log(recent-half realized-range rate / older-half
// realized-range rate), clamped to [-6, 6]. Degenerate when the older half's
// rate is below the numerical floor (near-flat range over that half) — carries
// the last valid value forward instead of a fabricated exact-zero "no change"
// reading.
inline float ComputeBurstinessIndex(double rvRecentRate, double rvOlderRate, float lastValidValue) {
    constexpr double kFloor = 1e-12;
    if (rvOlderRate < kFloor) {
        return lastValidValue;
    }
    const float ratio = static_cast<float>(std::log(std::max(rvRecentRate, kFloor) / rvOlderRate));
    return std::clamp(ratio, -6.0f, 6.0f);
}

// Dim 2 (relative_range): (high - low) / atr. Degenerate when atr is at/near
// zero (unpopulated ATR array) — carries the last valid value forward instead
// of a fabricated exact-zero "no range" reading.
inline float ComputeRelativeRange(float high, float low, float atr, float lastValidValue) {
    constexpr float kAtrFloor = 0.00001f;
    if (atr <= kAtrFloor) {
        return lastValidValue;
    }
    return (high - low) / atr;
}

// Dim 8 (fisher_info): Fisher transform of price position within its lookback
// range, 0.5*ln((1+x)/(1-x)) with x = 2*((price-min)/(max-min) - 0.5), clamped
// away from the +-1 singularity. Degenerate when the lookback range is flat
// (max <= min) — carries the last valid value forward instead of a fabricated
// exact-zero "midpoint" reading.
inline float ComputeFisherInformation(float minPrice, float maxPrice, float currentPrice, float lastValidValue) {
    if (maxPrice <= minPrice) {
        return lastValidValue;
    }
    const float rawPos = (currentPrice - minPrice) / (maxPrice - minPrice);
    float x = 2.0f * (rawPos - 0.5f);
    x = std::clamp(x, -0.99f, 0.99f);
    return 0.5f * std::log((1.0f + x) / (1.0f - x));
}

}  // namespace cfc
