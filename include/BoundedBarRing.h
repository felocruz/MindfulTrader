// BoundedBarRing.h — fixed-size, no-heap-allocation ring buffer of completed
// bars' {high, low} — the shared building block for `NR7`'s 7-bar range
// comparison and `TurtleSoup`'s 20-bar prior-window extremes (both real,
// confirmed requirements, not assumed):
//   - NR7: `sc.High[sc.Index-i] - sc.Low[sc.Index-i]` for i in 1..7
//     (`src/TripleScreen3.cpp`, real inline logic) — needs each of the last
//     7 completed bars' own range, not just an aggregate.
//   - TurtleSoup: `Subgraph_HighestHigh20Period`/`Subgraph_LowestLow20Period`
//     (`sc.Highest(sc.High, ..., 20)`/`sc.Lowest(sc.Low, ..., 20)`,
//     `TripleScreen3.cpp:539-540`) — a genuine 20-bar rolling window, NOT
//     "4 days" despite the misleading real function name
//     `SetPrevFourBarExtremes()` (a pre-existing naming mismatch in the live
//     codebase, not introduced here) — confirmed via the real call site
//     (`TripleScreen3.cpp:1197-1202`): evaluated as of the last CLOSED bar,
//     excluding the currently-forming bar ("Turtle Soup must use PREVIOUS
//     lookback window").
//
// Pure, header-only, no ACSIL types, natively unit-testable
// (tests/cpp/test_bounded_bar_ring.cpp). DOD: fixed max capacity (32, ample
// headroom over the 7/20 real requirements), no heap allocation, bounded
// memory regardless of stream length.
//
// docs/superpowers/plans/2026-09-16-market-data-replay-alpha-generator-implementation.md Task 5.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>

constexpr int kBoundedBarRingMaxCapacity = 32;

class BoundedBarRing {
public:
    // `capacity` is the number of completed bars retained (e.g. 7 for NR7,
    // 20 for TurtleSoup) — must be <= kBoundedBarRingMaxCapacity.
    explicit BoundedBarRing(int capacity) : m_capacity(capacity) {}

    // Call once per completed bar (bar-close), not per tick.
    void OnBarClose(float high, float low) {
        m_high[m_pos] = high;
        m_low[m_pos] = low;
        m_pos = (m_pos + 1) % m_capacity;
        if (m_count < m_capacity) ++m_count;
    }

    [[nodiscard]] bool IsFull() const { return m_count >= m_capacity; }
    [[nodiscard]] int Count() const { return m_count; }

    // Chronological access: index 0 = oldest retained bar, Count()-1 = most
    // recently closed bar. Undefined for index >= Count() (caller's
    // responsibility, matching this codebase's other bounded-buffer
    // conventions — no exceptions on a hot path).
    [[nodiscard]] float HighAt(int chronologicalIndex) const {
        return m_high[LogicalToRingIndex(chronologicalIndex)];
    }
    [[nodiscard]] float LowAt(int chronologicalIndex) const {
        return m_low[LogicalToRingIndex(chronologicalIndex)];
    }
    [[nodiscard]] float RangeAt(int chronologicalIndex) const {
        return HighAt(chronologicalIndex) - LowAt(chronologicalIndex);
    }

    // Aggregate over the full retained window (0.0f/undefined if not yet full
    // — callers must check IsFull() first, same convention as WilderAtr's
    // "0.0f during warm-up").
    [[nodiscard]] float HighestHigh() const {
        float hh = m_high[0];
        for (int i = 1; i < m_count; ++i) hh = std::max(hh, m_high[i]);
        return hh;
    }
    [[nodiscard]] float LowestLow() const {
        float ll = m_low[0];
        for (int i = 1; i < m_count; ++i) ll = std::min(ll, m_low[i]);
        return ll;
    }

private:
    [[nodiscard]] int LogicalToRingIndex(int chronologicalIndex) const {
        // Oldest entry lives at m_pos when the ring is full (next write
        // slot); when not yet full, oldest is simply index 0.
        const int base = IsFull() ? m_pos : 0;
        return (base + chronologicalIndex) % m_capacity;
    }

    int m_capacity;
    std::array<float, kBoundedBarRingMaxCapacity> m_high{};
    std::array<float, kBoundedBarRingMaxCapacity> m_low{};
    int m_pos = 0;
    int m_count = 0;
};
