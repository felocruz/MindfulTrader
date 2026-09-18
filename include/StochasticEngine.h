// StochasticEngine.h — pure, header-only reimplementation of Sierra Chart's
// sc.Stochastic() — no ACSIL types, natively unit-testable
// (tests/cpp/test_stochastic_engine.cpp).
//
// Real production call site confirmed this session:
//   src/TripleScreen3.cpp:717  sc.Stochastic(sc.BaseDataIn, Subgraph_StochK,
//                                            3, 3, 3, MOVAVGTYPE_SIMPLE)
// Real signature (sierra_chart_dependencies/sierrachart.h:584):
//   Stochastic(ChartBaseDataIn, Out, FastKLength, FastDLength, SlowDLength,
//              MovingAverageType) -> InternalStochastic(..., Out.Arrays[0]
//              /*FastK*/, Out.Arrays[1] /*FastD*/, ...)
// i.e. this is the standard "Slow Stochastic" cascade: RawK (FastKLength
// lookback) -> FastD (SMA over FastDLength) -> SlowD (SMA over SlowDLength).
//
// RESOLVED (Task 7, docs/superpowers/plans/2026-09-16-market-data-replay-
// alpha-generator-implementation.md): `DetectMomentumPinball`'s `stochK`
// reads `Subgraph_StochK[sc.Index]` directly (src/TripleScreen3.cpp:934) --
// the array `sc.Stochastic()` itself was called into, i.e. Out.Arrays[0] /
// FastK / RawK, NOT the separately-extracted `Subgraph_StochD` (FastD).
// This engine's `RawK()` is the one to feed callers reproducing that call
// site; `Value()`/`FastD()` remain available for any future consumer of the
// smoothed reading.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

constexpr int kStochasticMaxLength = 32;

// Bounded rolling SMA over a fixed-size window (DOD: no heap allocation).
class SmaWindow {
public:
    explicit SmaWindow(int length) : m_length(length) {}

    float Push(float value) {
        m_ring[m_pos] = value;
        m_pos = (m_pos + 1) % m_length;
        if (m_count < m_length) {
            ++m_count;
        }
        if (m_count < m_length) {
            return 0.0f;  // warm-up, not yet a full window
        }
        float sum = 0.0f;
        for (int i = 0; i < m_length; ++i) sum += m_ring[i];
        m_value = sum / static_cast<float>(m_length);
        return m_value;
    }

    [[nodiscard]] bool IsSeeded() const { return m_count >= m_length; }
    [[nodiscard]] float Value() const { return IsSeeded() ? m_value : 0.0f; }

private:
    int m_length;
    std::array<float, kStochasticMaxLength> m_ring{};
    int m_pos = 0;
    int m_count = 0;
    float m_value = 0.0f;
};

class StochasticEngine {
public:
    StochasticEngine(int fastKLength, int fastDLength, int slowDLength)
        : m_fastKLength(fastKLength), m_fastD(fastDLength), m_slowD(slowDLength) {}

    // Feed one new bar's high/low/close. Returns FastD (see header note).
    float OnBar(float high, float low, float close) {
        m_highRing[m_pos] = high;
        m_lowRing[m_pos] = low;
        m_pos = (m_pos + 1) % m_fastKLength;
        if (m_count < m_fastKLength) {
            ++m_count;
        }
        if (m_count < m_fastKLength) {
            return 0.0f;
        }
        float hh = m_highRing[0], ll = m_lowRing[0];
        for (int i = 1; i < m_fastKLength; ++i) {
            hh = std::max(hh, m_highRing[i]);
            ll = std::min(ll, m_lowRing[i]);
        }
        const float range = hh - ll;
        m_rawK = (range > 1e-9f) ? (100.0f * (close - ll) / range) : 50.0f;
        const float fastD = m_fastD.Push(m_rawK);
        m_slowD.Push(fastD);
        return fastD;
    }

    [[nodiscard]] float RawK() const { return m_rawK; }
    [[nodiscard]] float FastD() const { return m_fastD.Value(); }
    [[nodiscard]] float SlowD() const { return m_slowD.Value(); }
    [[nodiscard]] float Value() const { return FastD(); }
    [[nodiscard]] bool IsSeeded() const { return m_fastD.IsSeeded(); }

private:
    int m_fastKLength;
    std::array<float, kStochasticMaxLength> m_highRing{};
    std::array<float, kStochasticMaxLength> m_lowRing{};
    int m_pos = 0;
    int m_count = 0;
    float m_rawK = 0.0f;
    SmaWindow m_fastD;
    SmaWindow m_slowD;
};
