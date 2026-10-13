// RecurrenceRateEngine.h -- holds the closed-bar window for the RQA recurrence rate (dims 13 and
// the activity-clock twin). The rate itself is computed in Rust
// (`observation_vector::recurrence_rate`, spec decision #42): the closed bars' pairwise
// recurrences plus the live point, including the diagonal. That is O(n^2) per ComputeRate call.
// Every caller (ContextManager, ImbalanceContextManager, MarketDataReplayEngine) rebuilds and
// computes together only when a new bar closes, never per tick, so the C++ original's per-tick
// O(n) caching of the closed-pair count bought nothing on these paths and was dropped.

#pragma once

#include "generated/mts_core.h"  // the formulas below are implemented in Rust (spec decision #42)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

class RecurrenceRateEngine {
public:
    static constexpr int kMaxClosedBars = 256;

    void Reset() { m_closedCount = 0; }

    /// Stores the closed-bar window (clamped to kMaxClosedBars). `epsilon` is accepted for API
    /// stability; the rate is computed against the epsilon passed to ComputeRate.
    void RebuildClosedBarWindow(const float* closedPrices, int count, float /*epsilon*/) {
        m_closedCount = std::clamp(count, 0, kMaxClosedBars);
        for (int i = 0; i < m_closedCount; ++i) {
            m_closedPrices[static_cast<std::size_t>(i)] = closedPrices[i];
        }
    }

    /// Recurrence rate of the stored closed bars plus the live point (Rust; O(n^2) per call --
    /// every caller computes once per newly closed bar, never per tick).
    float ComputeRate(float currentPrice, float epsilon) const {
        return mts::observation_vector_recurrence_rate(m_closedPrices.data(), static_cast<std::size_t>(m_closedCount),
                                                       currentPrice, epsilon);
    }

    int GetClosedCount() const { return m_closedCount; }

private:
    std::array<float, kMaxClosedBars> m_closedPrices{};
    int m_closedCount = 0;
};
