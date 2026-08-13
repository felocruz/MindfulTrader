// RQAEpsilonSelector.h — pure, header-only RQA (Recurrence Quantification
// Analysis) epsilon selector. Extracted from StudyHelperFunctions.cpp
// (Task 3, docs/superpowers/sdd/2026-08-13-observation-vector-institutional-
// elevation) so it can be natively unit-tested
// (tests/cpp/test_rqa_epsilon.cpp) -- it has zero Sierra Chart/ACSIL
// dependency, but StudyHelperFunctions.cpp itself pulls in sierrachart.h via
// MindfulTrader_Precompiled.h and cannot be compiled standalone. Same
// rationale/precedent as FeatureScaler.h/TailRiskEngine.h/InformationEngine.h
// already being extracted this way.

#pragma once

#include <array>
#include <cmath>
#include <algorithm>
#include <cstddef>

/// Largest window this selector supports. Production's caller
/// (CalculateRecurrenceRate, via TripleScreen2's
/// slow_window_n = max(30, clamp(adaptiveWindow, 10, 40))) never exceeds 40;
/// 64 leaves headroom while keeping the fixed-capacity pair buffer below at
/// 64*63/2 = 2016 floats (~8 KB of stack), so this function performs ZERO heap
/// allocation -- it is reachable from the per-tick ACSIL path (only at
/// recalibration cadence, but CLAUDE.md's no-heap-on-hot-path rule still
/// applies). Windows larger than this are clamped to the most recent
/// kMaxSelectorN samples rather than silently overrunning.
inline constexpr int kRQASelectorMaxN = 64;
inline constexpr std::size_t kRQASelectorMaxPairs =
    static_cast<std::size_t>(kRQASelectorMaxN) * static_cast<std::size_t>(kRQASelectorMaxN - 1) / 2;

/// Selects epsilon such that the recurrence rate over `prices[0..n)` is as
/// close as possible to `targetRR`, via the Schinkel, Dimigen & Marwan
/// (2008) fixed-recurrence-rate method: sort all pairwise absolute
/// distances, epsilon = the distance at the rank implied by the target.
/// O(n^2 log n) -- called only at recalibration cadence (Task 3's
/// RQA_EPSILON_RECALIBRATION_BARS, gated on bar advancement), never per-tick.
///
/// `targetRR` is the desired FULL-MATRIX recurrence rate, matching exactly what
/// production's CalculateRecurrenceRate measures: RR = recurCount / n^2 with
/// the line of identity (LOI, the i==j diagonal) included --
/// `recurCount += lookback_n` seeds the diagonal before the off-diagonal loop.
///
/// This selector can only rank the n(n-1)/2 OFF-diagonal distances, so the
/// off-diagonal target must be back-solved from the desired full-matrix rate
/// (fix 2026-08-13: before this correction the raw targetRR was applied
/// directly to the off-diagonal ranking, so the LOI's own 1/n contribution --
/// 3.3% at n=30, already exceeding a 3% target on its own -- was pure
/// overshoot and achieved RR came out ~2x the intended target):
///
///     RR_full     = (n + 2*M) / n^2      , M = # unordered off-diagonal pairs < eps
///     M           = (RR_full*n^2 - n) / 2
///     RR_offdiag  = M / (n(n-1)/2) = (RR_full*n^2 - n) / (n*(n-1))
///
/// Feasibility: RR_full is bounded below by the LOI alone, 1/n. If
/// targetRR <= 1/n the target is unreachable at this window size; the selector
/// then returns the smallest achievable epsilon (the minimum pairwise
/// distance, which under production's strict `dist < epsilon` test yields
/// M = 0 and therefore RR = 1/n, the closest achievable value to the target).
inline double SelectEpsilonForTargetRecurrenceRate(const float* prices, int n, double targetRR) {
    if (n < 2) return 1e-6;
    if (n > kRQASelectorMaxN) n = kRQASelectorMaxN;

    std::array<float, kRQASelectorMaxPairs> distances{};
    std::size_t numPairs = 0;
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            distances[numPairs++] = std::fabs(prices[i] - prices[j]);
        }
    }
    if (numPairs == 0) return 1e-6;

    std::sort(distances.begin(), distances.begin() + static_cast<std::ptrdiff_t>(numPairs));

    const double nd = static_cast<double>(n);
    const double offDiagTarget = (targetRR * nd * nd - nd) / (nd * (nd - 1.0));

    // Infeasible (targetRR <= 1/n): the LOI alone already meets or exceeds the
    // requested full-matrix rate. Smallest achievable epsilon == the minimum
    // pairwise distance (strict `<` in the production test means no
    // off-diagonal pair is counted at that epsilon), giving RR == 1/n exactly.
    if (!(offDiagTarget > 0.0)) {
        return std::max(static_cast<double>(distances[0]), 1e-6);
    }

    const double clampedTarget = std::min(offDiagTarget, 1.0);
    // Round (not floor): distances[rank] leaves exactly `rank` distances
    // strictly below epsilon (ties aside), so `rank` IS the achieved M --
    // rounding puts M closest to the back-solved target.
    double rankF = std::floor(clampedTarget * static_cast<double>(numPairs) + 0.5);
    if (rankF < 0.0) rankF = 0.0;
    std::size_t rank = static_cast<std::size_t>(rankF);
    rank = std::min(rank, numPairs - 1);

    return std::max(static_cast<double>(distances[rank]), 1e-6);
}
