// WilderAtrEngine.h — pure, header-only reimplementation of Sierra Chart's
// sc.ATR(..., MOVAVGTYPE_WILDERS) — no ACSIL types, natively unit-testable
// (tests/cpp/test_wilder_atr_engine.cpp), matching IndicatorComputations.h's
// established "No Sierra Chart / ACSIL types" convention.
//
// Real production call sites confirmed this session (not assumed):
//   - src/TripleScreen2.cpp:483  sc.ATR(sc.BaseDataIn, Subgraph_ATR, 14, MOVAVGTYPE_WILDERS)
//   - src/TripleScreen3.cpp:529  sc.ATR(sc.BaseDataIn, Subgraph_AtrTemp3, 10, MOVAVGTYPE_WILDERS)
//     ("ATR(10) Wilder's smoothing — institutional standard (hardcoded, not
//     configurable)") — this is the ATR that feeds DetectElderBreakout's
//     `atr`/Keltner-band inputs directly (TripleScreen3.cpp:1032-1035).
// Two different periods are genuinely used in production — period is a
// runtime constructor parameter here, not hardcoded to one value.
//
// docs/superpowers/plans/2026-09-16-market-data-replay-alpha-generator-implementation.md
// Task 1. Numeric parity against real SC-computed ATR values is deferred to
// that plan's Task 12 (blocked pending a fresh current-schema SC-collected
// comparison file) — the formula below is Wilder's own published smoothing,
// not a guess.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>

// Wilder's Average True Range: TR = max(high-low, |high-prevClose|,
// |low-prevClose|); seeded by a simple average of the first `period` TR
// values, then recursively smoothed: ATR_t = ATR_{t-1} + (TR_t - ATR_{t-1})/period.
// Fixed-size internal state only (DOD rule: no per-tick heap allocation, no
// unbounded history) — a small seed accumulator, never a growing buffer.
class WilderAtr {
public:
    explicit WilderAtr(int period) : m_period(period) {}

    // Feed one new bar's OHLC. Returns the current ATR value (0.0f until the
    // seed window fills, matching Wilder's own "undefined during warm-up"
    // convention — callers must treat 0.0f as "not yet valid", same
    // requirement `DetectKangarooTail`/`DetectTurtleSoup`/etc. already impose
    // on their own `atr` parameter, `atr <= 0.0f` guard).
    float OnBar(float high, float low, float close) {
        float tr;
        if (!m_havePrevClose) {
            tr = high - low;
        } else {
            tr = std::max({high - low, std::fabs(high - m_prevClose), std::fabs(low - m_prevClose)});
        }
        m_prevClose = close;
        m_havePrevClose = true;

        if (!m_seeded) {
            m_seedSum += tr;
            ++m_seedCount;
            if (m_seedCount < m_period) {
                return 0.0f;
            }
            m_atr = m_seedSum / static_cast<float>(m_period);
            m_seeded = true;
            return m_atr;
        }

        m_atr = m_atr + (tr - m_atr) / static_cast<float>(m_period);
        return m_atr;
    }

    [[nodiscard]] float Value() const { return m_seeded ? m_atr : 0.0f; }
    [[nodiscard]] bool IsSeeded() const { return m_seeded; }

private:
    int m_period;
    bool m_havePrevClose = false;
    float m_prevClose = 0.0f;
    bool m_seeded = false;
    float m_seedSum = 0.0f;
    int m_seedCount = 0;
    float m_atr = 0.0f;
};
