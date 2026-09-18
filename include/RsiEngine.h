// RsiEngine.h — pure, header-only reimplementation of Sierra Chart's
// sc.RSI() — no ACSIL types, natively unit-testable
// (tests/cpp/test_rsi_engine.cpp), matching IndicatorComputations.h's
// established "No Sierra Chart / ACSIL types" convention.
//
// Real production call sites confirmed this session (not assumed) — TWO
// genuinely different configurations exist, both needed:
//   - src/TripleScreen2.cpp:385-386,545: `RSI_LENGTH=2`, `MOVAVGTYPE_WILDERS`
//     ("the correct smoothing method for RSI") — feeds IndicatorKey::RSI,
//     which Lock B's warm-up check (`GetValue<IndicatorKey::RSI>() != 0`)
//     depends on directly.
//   - src/TripleScreen3.cpp:715-716: `sc.RSI(sc.Close, Subgraph_RSI3,
//     MOVAVGTYPE_SIMPLE, 3)` / `sc.RSI(sc.Close, Subgraph_RSI10,
//     MOVAVGTYPE_SIMPLE, 10)` — feeds DetectMomentumPinball's
//     rsi3/rsi10/prevRSI3/prevRSI10 inputs.
// MOVAVGTYPE_SIMPLE here is the standard, well-documented "Cutler's RSI"
// variant (average gain/loss smoothed via a true rolling SMA, not Wilder's
// recursive smoothing) — a legitimate, named alternative formulation, not a
// guess. Smoothing mode is a runtime constructor parameter so both real
// configurations are served by one engine.
//
// docs/superpowers/plans/2026-09-16-market-data-replay-alpha-generator-implementation.md
// Task 2. Numeric parity against real SC-computed RSI values is deferred to
// that plan's Task 12 (blocked pending a fresh current-schema comparison file).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

enum class RsiSmoothing { SIMPLE, WILDERS };

// Bounded ring buffer (fixed max capacity, DOD rule: no heap allocation).
// All real periods in this codebase are small (2, 3, 10) — 32 is generous
// headroom, not a magic number chosen to fit a specific case.
constexpr int kRsiMaxPeriod = 32;

class RsiEngine {
public:
    RsiEngine(int period, RsiSmoothing smoothing)
        : m_period(period), m_smoothing(smoothing) {}

    // Feed one new bar's close. Returns the current RSI in [0,100], or 0.0f
    // during warm-up (matching this codebase's own `rsiValue == 0` "not yet
    // computing real values" convention, IndicatorManager.cpp:596-604).
    float OnClose(float close) {
        if (!m_havePrevClose) {
            m_prevClose = close;
            m_havePrevClose = true;
            return 0.0f;
        }
        const float delta = close - m_prevClose;
        m_prevClose = close;
        const float gain = delta > 0.0f ? delta : 0.0f;
        const float loss = delta < 0.0f ? -delta : 0.0f;

        float avgGain, avgLoss;
        if (m_smoothing == RsiSmoothing::WILDERS) {
            if (!m_seeded) {
                m_seedGainSum += gain;
                m_seedLossSum += loss;
                ++m_seedCount;
                if (m_seedCount < m_period) {
                    return 0.0f;
                }
                m_avgGain = m_seedGainSum / static_cast<float>(m_period);
                m_avgLoss = m_seedLossSum / static_cast<float>(m_period);
                m_seeded = true;
            } else {
                m_avgGain = m_avgGain + (gain - m_avgGain) / static_cast<float>(m_period);
                m_avgLoss = m_avgLoss + (loss - m_avgLoss) / static_cast<float>(m_period);
            }
            avgGain = m_avgGain;
            avgLoss = m_avgLoss;
        } else {
            // Cutler's RSI: true rolling SMA of gain/loss over `period`.
            m_gainRing[m_ringPos] = gain;
            m_lossRing[m_ringPos] = loss;
            m_ringPos = (m_ringPos + 1) % m_period;
            if (m_ringCount < m_period) {
                ++m_ringCount;
                return 0.0f;
            }
            float gainSum = 0.0f, lossSum = 0.0f;
            for (int i = 0; i < m_period; ++i) {
                gainSum += m_gainRing[i];
                lossSum += m_lossRing[i];
            }
            avgGain = gainSum / static_cast<float>(m_period);
            avgLoss = lossSum / static_cast<float>(m_period);
            m_seeded = true;
        }

        if (avgLoss <= 1e-9f) {
            m_rsi = 100.0f;
        } else {
            const float rs = avgGain / avgLoss;
            m_rsi = 100.0f - (100.0f / (1.0f + rs));
        }
        return m_rsi;
    }

    [[nodiscard]] float Value() const { return m_seeded ? m_rsi : 0.0f; }
    [[nodiscard]] bool IsSeeded() const { return m_seeded; }

private:
    int m_period;
    RsiSmoothing m_smoothing;

    bool m_havePrevClose = false;
    float m_prevClose = 0.0f;
    bool m_seeded = false;
    float m_rsi = 0.0f;

    // Wilder's-mode state.
    float m_seedGainSum = 0.0f;
    float m_seedLossSum = 0.0f;
    int m_seedCount = 0;
    float m_avgGain = 0.0f;
    float m_avgLoss = 0.0f;

    // SIMPLE (Cutler's)-mode state — bounded ring buffer.
    std::array<float, kRsiMaxPeriod> m_gainRing{};
    std::array<float, kRsiMaxPeriod> m_lossRing{};
    int m_ringPos = 0;
    int m_ringCount = 0;
};
