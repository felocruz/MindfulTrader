// CarryForwardCalculators.h — pure, header-only degenerate-guard formulas for
// three observation dims (2 relative_range, 3 log_scale_expansion_ratio, 8
// fisher_info) that were returning a fixed 0.0f sentinel on routine,
// non-warmup degenerate input (flat range, zero ATR, flat price) instead of
// carrying the last valid physics reading forward. That fixed sentinel became
// frequent enough to anchor FeatureScaler's rolling median, collapsing the
// exported, scaled observation to an exact 0.0
// (docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md).
// Dim 1 (burstiness_index) used ComputeBurstinessIndex below too until the
// 2026-08-29 redirect to raschkeBurst (EventVelocityEngine.h) -- it no longer
// consumes this file at all; the function name is a historical artifact of
// that original use, not a current caller list.
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

// Dim 3 only (CalculateLogScaleExpansionRatio, StudyHelperFunctions.cpp) as of
// 2026-09-02 -- dim 1 (burstiness_index) redirected to raschkeBurst
// (EventVelocityEngine.h::CalculateBurstinessIndex) 2026-08-29 and no longer
// calls this function; its own [-6,+6] default below is a historical
// leftover from when it did, kept as the function's default only because
// dim3's call site always passes its own explicit [-10,+6] override anyway.
//
// log(recent-window realized-variance rate / reference-window rate), clamped
// to a data-error backstop range (NOT a statistical winsorization bound --
// that job belongs to FeatureScaler's WIDE_STATE_WINSOR_SIGMA/
// DIM3_WIDE_WINSOR_SIGMA on the z-scored output, several orders of magnitude
// further out; see that file). Degenerate when the reference rate is below
// the numerical floor (near-flat range/return over that window) — carries
// the last valid value forward instead of a fabricated exact-zero "no
// change" reading.
//
// Default bound [-6,+6] was dim1's own (CalculateBurstiness: disjoint
// recent-half vs older-half, symmetric by construction) -- verified on real
// 60-minute MES bars (mes_wave_60m.parquet, 19,592 bars, adaptive window
// range [10,40]): true range [-4.587, +5.082], 0/606,577 window/bar
// combinations clip. No longer load-bearing now that dim1 doesn't call this
// function, but left unchanged rather than churned for its own sake.
//
// dim3 (CalculateLogScaleExpansionRatio, StudyHelperFunctions.cpp) passes its
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

// Dim 11 (amihud_illiquidity): geometric mean of |log-return|/sqrt(dollar-volume)
// over the valid samples in the lookback window, computed as exp(mean of logs)
// -- `sumLogRatio` is the accumulated SUM OF LOGS of each sample's ratio, not
// the accumulated raw ratios (see the call site for the log accumulation and
// its epsilon floor before ln()). REFORMULATED 2026-09-03 from Amihud (2002)'s
// literal linear-ratio arithmetic mean, two independent real-data-validated
// fixes to the same underlying defect (a thin-volume bar's ratio blows up and
// dominates a plain arithmetic mean):
//   1. sqrt(dollar-volume) replaces linear dollar-volume in the denominator --
//      Kyle & Obizhaeva (2016, "Market Microstructure Invariance", Econometrica)
//      and Lillo, Farmer & Mantegna (2003, "Master curve for price-impact
//      function", Nature): price impact empirically scales as a concave,
//      roughly square-root function of volume, not linearly as Amihud assumed.
//      Verified directly on real MES data (2026-09-03): OLS-fitting
//      log|r_t| = gamma*log(V_t) gives gamma=0.512 (R^2=0.354), matching the
//      theoretical 0.5 almost exactly; the resulting ratio's coefficient of
//      variation and skewness both drop ~14-16% vs. the linear formula.
//   2. Geometric mean (exp of the mean of logs) replaces the arithmetic mean --
//      Hasbrouck (2009, "Trading Costs and Returns for US Equities", Journal
//      of Finance)'s log-transform convention for exactly this class of ratio.
//      Verified empirically on real MES rolling 20-bar windows (2026-09-03):
//      removing the single worst reading from a window shifts a geometric-mean
//      aggregate by only 0.55% on average, vs. 5.0% for a median and 8.6% for
//      a raw arithmetic mean -- an order-of-magnitude difference in
//      outlier-robustness, not a marginal one. The resulting rolling series is
//      also far more stable (CV 0.027 vs. 0.38-0.43 for median/mean).
//   Kept in raw (positive-ratio) units rather than exposing the log value
//   directly (unlike Hasbrouck's own regression convention) so every existing
//   percentile-based/floor-based downstream consumer (RiskGateContext's
//   amihud_percentile, FeatureScaler's AMIHUD_ABSOLUTE_FLOOR) keeps working
//   unchanged -- percentile rank is invariant under a monotonic transform, so
//   this reformulation only requires FeatureScaler's own dim11 calibration to
//   be re-derived against the new formula's real scale, not a redesign of
//   every gate that reads this dim.
// Degenerate when fewer than 2 valid samples were found (thin/illiquid
// lookback) -- carries the last valid value forward instead of a fabricated
// exact-zero "perfectly liquid" reading.
inline float ComputeAmihudIlliquidity(double sumLogRatio, int count, float lastValidValue) {
    if (count < 2) {
        return lastValidValue;
    }
    return static_cast<float>(std::exp(sumLogRatio / count));
}

}  // namespace cfc
