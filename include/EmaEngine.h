// EmaEngine.h — pure, header-only EMA/SMA/MACD reimplementation of Sierra
// Chart's built-in moving-average/MACD studies — no ACSIL types, natively
// unit-testable (tests/cpp/test_ema_engine.cpp).
//
// Real production facts confirmed this session (not assumed):
//   - Elder Impulse EMA: `sc.ExponentialMovAvg(sc.BaseDataIn[...],
//     Subgraph_ImpulseEMA, 13)` (`src/TripleScreen1.cpp:249`) — classic
//     Elder EMA(13) of close.
//   - Elder Impulse color classification: `GetImpulse(maDiff, macdDiff)`
//     (`src/StudyHelperFunctions.cpp:671-682`) — GREEN if both > 0, RED if
//     both < 0, else BLUE. Transcribed exactly, not approximated.
//   - `ComputeImpulse()` (`include/IndicatorComputations.h:303`) is ALREADY
//     pure/portable and takes `greenColor`/`redColor`/`blueColor` as opaque
//     caller-supplied ints (equality-compared only) — this engine reuses it
//     directly, no need to know Sierra Chart's real internal color values.
//   - Keltner center line (feeds `DetectElderBreakout`'s bands):
//     `sc.MovingAverage(..., Input_KeltnerMAType=MOVAVGTYPE_EXPONENTIAL,
//     Input_KeltnerMALength=10)` of `SC_OHLC_AVG` (average of O/H/L/C, not
//     just close) — `src/TripleScreen3.cpp:446-458,523-526`.
//   - **RESOLVED (Task 7, market-data-replay-alpha-generator-implementation
//     plan)**: the MACD study feeding `macdDiff` (`MACDDiffSubgraphArray`) is
//     referenced by `sc.GetStudyArrayUsingID` from a chart-attached study
//     instance (`src/TripleScreen1.cpp:203-220`), but the STUDY FUNCTION
//     ITSELF is `scsf_Screen1_MACD` (`src/TripleScreen1.cpp:688`) — a
//     repo-owned custom study (same pattern as TS2's `scsf_Screen2_MACD`),
//     with real, source-confirmed defaults: MACD(12,26,9), EXPONENTIAL, on
//     `SC_LAST` (close). `MacdEngine`'s (12,26,9) default below matches this
//     exactly. The chart could theoretically be reconfigured away from these
//     defaults (genuine chart-attachment, not hardcoded), but the SHIPPED
//     defaults are now a confirmed fact, not a guess — Task 12's real
//     validation gap is only about whether the LIVE chart config matches
//     the study's own shipped defaults, not about the formula itself.

#pragma once

#include <cmath>

// Standard EMA: α = 2/(N+1), seeded by the first input value (Sierra
// Chart's own convention — sc.ExponentialMovAvg seeds from the first bar,
// not a simple-average warm-up window, unlike Wilder's ATR/RSI above).
class Ema {
public:
    explicit Ema(int period) : m_alpha(2.0f / (static_cast<float>(period) + 1.0f)) {}

    float OnValue(float value) {
        if (!m_seeded) {
            m_value = value;
            m_seeded = true;
        } else {
            m_value = m_value + m_alpha * (value - m_value);
        }
        return m_value;
    }

    [[nodiscard]] float Value() const { return m_value; }
    [[nodiscard]] bool IsSeeded() const { return m_seeded; }

private:
    float m_alpha;
    bool m_seeded = false;
    float m_value = 0.0f;
};

// Standard MACD: line = EMA(fast) - EMA(slow); signal = EMA(signalLength)
// of the line; histogram = line - signal. Defaults to MACD(12,26,9) — see
// header note on why the exact chart-configured values aren't in source.
class MacdEngine {
public:
    MacdEngine(int fastPeriod = 12, int slowPeriod = 26, int signalPeriod = 9)
        : m_fast(fastPeriod), m_slow(slowPeriod), m_signal(signalPeriod) {}

    float OnValue(float value) {
        const float fast = m_fast.OnValue(value);
        const float slow = m_slow.OnValue(value);
        m_line = fast - slow;
        m_signalValue = m_signal.OnValue(m_line);
        m_histogram = m_line - m_signalValue;
        return m_histogram;
    }

    [[nodiscard]] float Line() const { return m_line; }
    [[nodiscard]] float Signal() const { return m_signalValue; }
    [[nodiscard]] float Histogram() const { return m_histogram; }
    [[nodiscard]] bool IsSeeded() const { return m_fast.IsSeeded() && m_slow.IsSeeded() && m_signal.IsSeeded(); }

private:
    Ema m_fast;
    Ema m_slow;
    Ema m_signal;
    float m_line = 0.0f;
    float m_signalValue = 0.0f;
    float m_histogram = 0.0f;
};

// Elder Impulse color constants — arbitrary but consistent (ComputeImpulse
// is opaque to their actual values, equality-compared only; see header note).
constexpr int kImpulseGreen = 1;
constexpr int kImpulseRed = -1;
constexpr int kImpulseBlue = 0;

// Transcribed exactly from the real GetImpulse() (src/StudyHelperFunctions.cpp:671-682).
inline int GetImpulseColorLike(float maDiff, float macdDiff) {
    if (maDiff > 0.0f && macdDiff > 0.0f) return kImpulseGreen;
    if (maDiff < 0.0f && macdDiff < 0.0f) return kImpulseRed;
    return kImpulseBlue;
}
