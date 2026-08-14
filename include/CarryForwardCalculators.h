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

// Dim 1 (burstiness_index) / dim 3 (correction_action): log(recent-window
// realized-variance rate / reference-window rate), clamped to a data-error
// backstop range (NOT a statistical winsorization bound -- that job belongs
// to FeatureScaler's WIDE_STATE_WINSOR_SIGMA/DIM3_WIDE_WINSOR_SIGMA on the
// z-scored output, several orders of magnitude further out; see that file).
// Degenerate when the reference rate is below the numerical floor (near-flat
// range/return over that window) — carries the last valid value forward
// instead of a fabricated exact-zero "no change" reading.
//
// Default bound [-6,+6] is dim1's (CalculateBurstiness: disjoint recent-half
// vs older-half, symmetric by construction) -- verified on real 60-minute MES
// bars (mes_wave_60m.parquet, 19,592 bars, adaptive window range [10,40]):
// true range [-4.587, +5.082], 0/606,577 window/bar combinations clip.
//
// dim3 (CalculateRealizedVarianceRatio, StudyHelperFunctions.cpp) passes its
// own [-10,+6] -- its formula compares a recent-half window against the FULL
// window (recent is a subset of full), which makes the raw ratio structurally
// asymmetric: positive ratios are mechanically small (recent variance can't
// exceed full-window variance by much) while negative ratios are not (a quiet
// recent half against a volatile historical full window is comparatively
// unbounded). The shared default was already clipping real data: true range
// [-6.160, +0.787] on the same real-data sweep, with 3/606,577 combinations
// exceeding -6.0 -- not hypothetical, an already-occurring truncation of
// legitimate quiet-regime readings. [-10,+6] gives real margin below the
// observed -6.160 extreme.
inline float ComputeBurstinessIndex(double rvRecentRate, double rvOlderRate, float lastValidValue,
                                     float clampLow = -6.0f, float clampHigh = 6.0f) {
    constexpr double kFloor = 1e-12;
    if (rvOlderRate < kFloor) {
        return lastValidValue;
    }
    const float ratio = static_cast<float>(std::log(std::max(rvRecentRate, kFloor) / rvOlderRate));
    return std::clamp(ratio, clampLow, clampHigh);
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

// Dim 11 (amihud_illiquidity): mean(|log-return|/dollar-volume) over the
// valid samples in the lookback window. Degenerate when fewer than 2 valid
// samples were found (thin/illiquid lookback) — carries the last valid
// value forward instead of a fabricated exact-zero "perfectly liquid"
// reading.
inline float ComputeAmihudIlliquidity(double sum, int count, float lastValidValue) {
    if (count < 2) {
        return lastValidValue;
    }
    return static_cast<float>(sum / count);
}

}  // namespace cfc
