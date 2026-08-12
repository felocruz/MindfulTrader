// OrderFlowAsymmetryEngine.h — pure, header-only order-flow asymmetry from
// Sierra-Chart-maintained per-bar bid/ask volume. Extracted so it can be
// natively unit-tested (tests/cpp/test_order_flow_asymmetry_engine.cpp) —
// same rationale as FeatureScaler.h/VolumeProfileEngine.h: zero Sierra
// Chart/ACSIL dependency.
//
// Replaces the removed CalculateMicroAsymmetry/CalculateMicroAsymmetryFromTimeAndSales
// (StudyHelperFunctions.cpp), which manually rescanned sc.GetTimeAndSales() with
// hand-rolled sequence tracking and returned NaN (sanitized to a misleading exact
// 0.0) on ~45% of bars — a replay-mechanics reliability gap, not a data-richness
// one (docs/superpowers/plans/2026-08-12-denali-data-feed-proxy-audit.md Part 3).
// sc.BidVolume/sc.AskVolume are the same per-bar arrays VolumeIndicator already
// reads successfully every bar in the identical replay sessions where the T&S
// scan failed.

#pragma once

namespace ofae {

// Order-flow asymmetry in [-1, 1]: +1 = all buying (aggressor at ask),
// -1 = all selling (aggressor at bid), 0 = balanced.
// On a true no-trade bar (askVolume == bidVolume == 0 — confirmed by direct
// .scid byte inspection to be essentially unseen intraday for ES/MES, but
// free to guard), carries the last valid reading forward instead of
// fabricating a misleading exact-zero "balanced" reading.
inline float ComputeMicroAsymmetry(float askVolume, float bidVolume, float lastValidValue) {
    const float total = askVolume + bidVolume;
    if (total <= 0.0f) {
        return lastValidValue;
    }
    return (askVolume - bidVolume) / total;
}

}  // namespace ofae
