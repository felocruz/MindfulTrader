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

#include <vector>
#include <cmath>
#include <algorithm>
#include <cstddef>

/// Selects epsilon such that the recurrence rate over `prices[0..n)` is as
/// close as possible to `targetRR`, via the Schinkel, Dimigen & Marwan
/// (2008) fixed-recurrence-rate method: sort all pairwise absolute
/// distances, epsilon = the distance at rank floor(targetRR * numPairs).
/// O(n^2 log n) -- called only at recalibration cadence (Task 3's
/// RQA_EPSILON_RECALIBRATION_BARS), never per-tick.
inline double SelectEpsilonForTargetRecurrenceRate(const float* prices, int n, double targetRR) {
    if (n < 2) return 1e-6;

    std::vector<float> distances;
    distances.reserve(static_cast<size_t>(n) * static_cast<size_t>(n - 1) / 2);
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            distances.push_back(std::fabs(prices[i] - prices[j]));
        }
    }
    if (distances.empty()) return 1e-6;

    std::sort(distances.begin(), distances.end());

    const size_t numPairs = distances.size();
    size_t rank = static_cast<size_t>(targetRR * static_cast<double>(numPairs));
    rank = std::min(rank, numPairs - 1);

    return std::max(static_cast<double>(distances[rank]), 1e-6);
}
