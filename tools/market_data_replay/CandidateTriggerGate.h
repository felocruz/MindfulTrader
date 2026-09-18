// CandidateTriggerGate.h — Mahalanobis significant-change decision over the
// EVOLVING candidate dim count (CandidateObservationDims.h), computed
// DIRECTLY from FeatureScaler's already-scaled candidate vector. Deliberately
// NOT shared with include/ObservationTriggerGate.h -- the dim-selection spec
// (§2) requires that live ContextManager.cpp's gate stay untouched until dim
// selection concludes; this is a genuinely separate copy, not a
// parameterization of the shared header.
//
// docs/superpowers/specs/2026-09-09-market-data-replay-dim-selection-spec.md
// §3/§3c (the push-on-change/rolling-window design in §3a/§3b was tried,
// native-tested, and then found empirically NOT to fix the real-data
// emission rate -- 73.2% vs. an expected ~10% -- because it still ran a
// SECOND, independent median/MAD re-normalization over FeatureScaler's
// OWN already-scaled output. §3c replaces that design entirely: no rolling
// window, no re-normalization, no per-dim warm-up concept of its own --
// FeatureScaler already produces properly-scaled, institutional-grade
// z-scores (rolling robust Soft-Log-Z/Log-Z, shrinkage-corrected, its own
// 500-sample warmup already gates candidateObs's validity upstream in
// MarketDataReplayEngine::ComputeShouldEmit()). Re-normalizing that output
// through a SECOND small rolling window destroyed FeatureScaler's own
// calibration and reintroduced the exact MAD-collapse failure mode §3a/§3b
// were trying to fix, just one layer up (diagnosed independently by Gemini
// CLI, `docs/Gemini.md`, then empirically confirmed: a direct
// sum-of-squares metric on the same real data gave 7.1%, matching the
// chi-squared(10) 90th-percentile prediction almost exactly).

#pragma once

#include "CandidateObservationDims.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace mdr {

struct CandidateTriggerMetrics {
    float mahalanobis_distance = 0.0f;
    bool significant_change = false;
    // Per-dim breakdown (diagnostics) -- now literally just currentObs itself
    // (FeatureScaler's own scaled output), squared-summed to
    // mahalanobis_distance^2. Kept as its own field so a future investigation
    // can attribute a trigger to specific dims without extra plumbing.
    std::array<float, kCandidateDimCount> perDimZ{};
};

// Operates on a caller-provided candidate-only vector (kCandidateDimCount
// floats, already extracted + FeatureScaler-scaled by the caller) -- this
// class has no knowledge of the full 18D ObservationData layout, and no
// internal state of its own beyond "has a baseline ever been set" (used only
// to force the very first post-warmup tick to emit unconditionally, matching
// live ContextManager.cpp's ShouldTriggerHMM(hmm_initialized,
// significant_change) precedent).
class CandidateTriggerGate {
public:
    // kBaseEpsilon=5.1: re-derived 2026-09-16 for kCandidateDimCount=18 (all
    // 18 schema dims, operator directive -- this gate is now a pure row-
    // thinning heuristic, not a dim-selection mechanism; lbrnet's own
    // Student-t HMM training does dim selection). For 18 independent
    // standard-normal z's, sum(z_i^2) ~ chi-squared(18); the 90th percentile
    // of chi-squared(18) is 25.99 (Wilson-Hilferty approximation, cross-
    // checked against the standard chi-squared table), and sqrt(25.99) =
    // 5.098 ≈ 5.1 -- same ~10% base (false-positive) trigger rate this
    // threshold has always targeted, just re-derived for the new dimension
    // count (was 4.0 for kCandidateDimCount=10, chi-squared(10) 90th
    // percentile 15.987, sqrt=3.998). Re-derive again if kCandidateDimCount
    // changes.
    static constexpr float kBaseEpsilon = 5.1f;

    bool HasBaseline() const { return m_hasBaseline; }
    void SetBaseline(const std::array<float, kCandidateDimCount>&) { m_hasBaseline = true; }
    void Reset() { m_hasBaseline = false; }

    CandidateTriggerMetrics ComputeTriggerDecisionMetrics(
        const std::array<float, kCandidateDimCount>& currentObs) const {
        CandidateTriggerMetrics metrics;
        float distance_sq = 0.0f;
        for (std::size_t dim = 0; dim < kCandidateDimCount; ++dim) {
            metrics.perDimZ[dim] = currentObs[dim];
            distance_sq += currentObs[dim] * currentObs[dim];
        }
        metrics.mahalanobis_distance = std::sqrt(std::max(distance_sq, 0.0f));
        metrics.significant_change = metrics.mahalanobis_distance >= kBaseEpsilon;
        return metrics;
    }

private:
    bool m_hasBaseline = false;
};

}  // namespace mdr
