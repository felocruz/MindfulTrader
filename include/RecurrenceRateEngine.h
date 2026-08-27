// RecurrenceRateEngine.h -- pure, header-only, ACSIL-independent incremental
// RQA recurrence-rate engine. Same extraction rationale/precedent as
// RQAEpsilonSelector.h/TailRiskEngine.h/InformationEngine.h.
//
// Standard technique for real-time/streaming RQA: recomputing the full n x n
// pairwise distance matrix every sample is the naive approach. Production's
// window has exactly one point that changes every tick -- the live,
// still-forming current bar -- while the other n-1 points (closed bars) are
// fixed for the whole bar's duration. This engine caches the O((n-1)^2)
// pairwise recurrence count among the closed bars, rebuilt only when the
// window shifts (once per bar close, RebuildClosedBarWindow), and evaluates
// the live point against that fixed set in O(n-1) every tick (ComputeRate).
// Reduces the per-tick hot-path cost from O(n^2) to O(n), matching this
// codebase's existing convention of paying expensive work only at a coarser
// cadence (RQA_EPSILON_RECALIBRATION_BARS) and cheap work every tick.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

class RecurrenceRateEngine {
public:
    // Sized with headroom over the observation-vector-institutional-hardening
    // spec's proposed ~150-bar target (docs/superpowers/specs/2026-08-25-
    // observation-vector-institutional-hardening-spec.md Section 5) -- the
    // final window size is still pending that spec's own autocorrelation-time
    // derivation, tracked separately from this algorithmic change.
    static constexpr int kMaxClosedBars = 256;

    void Reset() {
        m_closedCount = 0;
        m_closedPairCount = 0;
    }

    // Called once per bar close (window shift), NOT every tick. closedPrices
    // holds the (up to kMaxClosedBars) closed/historical bar prices currently
    // in the window. O(count^2) -- acceptable at bar-close cadence.
    void RebuildClosedBarWindow(const float* closedPrices, int count, float epsilon) {
        count = std::clamp(count, 0, kMaxClosedBars);
        m_closedCount = count;
        for (int i = 0; i < count; ++i) {
            m_closedPrices[static_cast<std::size_t>(i)] = closedPrices[i];
        }
        int pairCount = 0;
        for (int i = 0; i < count; ++i) {
            for (int j = i + 1; j < count; ++j) {
                if (std::fabs(m_closedPrices[static_cast<std::size_t>(i)] -
                              m_closedPrices[static_cast<std::size_t>(j)]) < epsilon) {
                    ++pairCount;
                }
            }
        }
        m_closedPairCount = pairCount;
    }

    // Called every tick with the live (still-forming) bar's current price.
    // O(closedCount) -- cheap. n = closedCount + 1 (the live point included).
    float ComputeRate(float currentPrice, float epsilon) const {
        const int n = m_closedCount + 1;
        if (n <= 0) return 0.0f;
        int liveVsClosed = 0;
        for (int i = 0; i < m_closedCount; ++i) {
            if (std::fabs(currentPrice - m_closedPrices[static_cast<std::size_t>(i)]) < epsilon) {
                ++liveVsClosed;
            }
        }
        const int recurCount = n + 2 * m_closedPairCount + 2 * liveVsClosed;  // n = diagonal
        const float totalCount = static_cast<float>(n) * static_cast<float>(n);
        return std::clamp(static_cast<float>(recurCount) / totalCount, 0.0f, 1.0f);
    }

    int GetClosedCount() const { return m_closedCount; }

private:
    std::array<float, kMaxClosedBars> m_closedPrices{};
    int m_closedCount = 0;
    int m_closedPairCount = 0;
};
