#pragma once

// IndicatorComputations.h — pure, header-only compute functions extracted from
// the old virtual-dispatch `Indicator<T>` leaf classes (indicator-manager-dod-soa
// plan, design spec §3.5's "stateful compute extraction" pattern, mirroring the
// VwapState/ComputeVwap convention). Growing home for every subsequent extracted
// indicator's compute function across Tasks 6/8/9 — one file, not one per
// indicator, matching how StudyHelperFunctions.h's free functions are already
// organized in this codebase.
//
// No Sierra Chart / ACSIL types, natively unit-testable
// (tests/cpp/test_indicator_computations.cpp), same convention as
// VolumeProfileEngine.h / DailyBiasEngine.h / EventVelocityEngine.h.
//
// MacdEnum lives here (not in Indicator.h, where it used to be declared)
// for the same reason IndicatorKey was extracted to its own ACSIL-independent
// header (see IndicatorKey.h's comment at its include site in Indicator.h):
// ComputeMacd's return type must be the real MacdEnum, and this header must
// stay includable with just `-I include` (no sierrachart.h on the path).

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstddef>
#include <vector>

// Elite v2.3: Kinetic Spectrum [-6, +6]
// Architecture: Symmetric scale where:
// - Positive = Bullish momentum (building → peak → consolidation)
// - Negative = Bearish momentum (building → trough → consolidation)
// - Magnitude hierarchy: CROSS(6) > PEAK/TROUGH(5) > ZERO_CROSS(4) > SEASON(3) > TICK(2) > FLAT(1)
enum class MacdEnum : int8_t
{
    // Equilibrium
    AT_ZERO = 0,              // Exactly at zero line (neutral)

    // Bullish Spectrum (Positive)
    BULLISH_CROSS = 6,        // Signal line cross up (max conviction)
    SUMMER = 5,               // Histogram peak (max thrust)
    ZERO_FROM_BELOW = 4,      // Zero cross up (bear back broken)
    SPRING = 3,               // Rising histogram (momentum building)
    NEG_TICK_UP = 2,          // Early spring (ticking up below zero)
    POSITIVE_FLAT = 1,        // Consolidation above zero (resting)

    // Bearish Spectrum (Negative)
    NEGATIVE_FLAT = -1,       // Consolidation below zero
    POS_TICK_DOWN = -2,       // Early fall (ticking down above zero)
    FALL = -3,                // Falling histogram (momentum decaying)
    ZERO_FROM_ABOVE = -4,     // Zero cross down (bull back broken)
    WINTER = -5,              // Histogram trough (max bearish pressure)
    BEARISH_CROSS = -6        // Signal line cross down (max conviction)
};

// ============================================================================
// Macd: pure classification helpers.
//
// Transcribed verbatim from Macd::IsSpring/IsSummer/IsFall/IsWinter/
// IsPositiveTickDown/IsNegativeTickUp/IsZeroFromBelow/IsZeroFromAbove
// (src/Indicator.cpp:175-221 as of the indicator-manager-dod-soa Task 6 audit),
// with MACD_Diff[Index]/[Index-1]/[Index-2] renamed to
// diffCurrent/diffPrev1/diffPrev2 -- same logic, decoupled from
// SCSubgraphRef/Index so these stay standalone-testable like every other pure
// engine in this codebase.
// ============================================================================

inline bool MacdIsSpring(double diffCurrent, double diffPrev1, double diffPrev2) {
    return ((diffCurrent < 0.0) && (diffPrev1 < 0.0) && (diffPrev2 < 0.0) &&
        (diffCurrent > diffPrev1) &&
        (diffPrev1 >= diffPrev2));
}

inline bool MacdIsSummer(double diffCurrent, double diffPrev1, double diffPrev2) {
    return ((diffCurrent > 0.0) && (diffPrev1 > 0.0) && (diffPrev2 > 0.0) &&
        (diffCurrent > diffPrev1) &&
        (diffPrev1 >= diffPrev2));
}

inline bool MacdIsFall(double diffCurrent, double diffPrev1, double diffPrev2) {
    return ((diffCurrent > 0.0) && (diffPrev1 > 0.0) && (diffPrev2 > 0.0) &&
        (diffCurrent < diffPrev1) &&
        (diffPrev1 <= diffPrev2));
}

inline bool MacdIsWinter(double diffCurrent, double diffPrev1, double diffPrev2) {
    return ((diffCurrent < 0.0) && (diffPrev1 < 0.0) && (diffPrev2 < 0.0) &&
        (diffCurrent < diffPrev1) &&
        (diffPrev1 <= diffPrev2));
}

inline bool MacdIsPositiveTickDown(double diffCurrent, double diffPrev1, double diffPrev2) {
    return ((diffCurrent > 0.0) && (diffPrev1 > 0.0) && (diffPrev2 > 0.0) &&
        (diffCurrent < diffPrev1) &&
        (diffPrev1 > diffPrev2));
}

inline bool MacdIsNegativeTickUp(double diffCurrent, double diffPrev1, double diffPrev2) {
    return ((diffCurrent < 0.0) && (diffPrev1 < 0.0) && (diffPrev2 < 0.0) &&
        (diffCurrent > diffPrev1) &&
        (diffPrev1 < diffPrev2));
}

inline bool MacdIsZeroFromBelow(double diffCurrent, double diffPrev1, double diffPrev2) {
    return ((diffCurrent >= 0.0) && (diffPrev1 < 0.0) && (diffPrev2 < 0.0) &&
        (diffCurrent > diffPrev1) &&
        (diffPrev1 >= diffPrev2));
}

inline bool MacdIsZeroFromAbove(double diffCurrent, double diffPrev1, double diffPrev2) {
    return ((diffCurrent <= 0.0) && (diffPrev1 > 0.0) && (diffPrev2 > 0.0) &&
        (diffCurrent < diffPrev1) &&
        (diffPrev1 <= diffPrev2));
}

// ============================================================================
// Macd: stateful robust z-score (median/MAD, Taleb fat-tail safe) + signal
// classification. Transcribed verbatim from Macd::SetFromChart
// (src/Indicator.cpp:72-172 as of the Task 6 audit). kLookback/kMADConsistency
// are Macd's former private static constexpr members, moved here unchanged.
// ============================================================================

struct MacdState {
    static constexpr int kLookback = 50;  // 50-bar lookback for 60-min TS2 ≈ 3 trading days
    static constexpr double kMADConsistency = 1.4826;

    std::array<double, kLookback> zScoreHistory{};
    int historyIdx = 0;
    int historyCount = 0;
};

struct MacdResult {
    MacdEnum signal;
    float zScore;
};

// diffPrev1/diffPrev2 are only meaningful when barsAvailable >= 1 / >= 2
// respectively (mirrors Macd::SetFromChart's own `if (Index >= 2) ... else if
// (Index >= 1) ...` bar-count gating -- the caller passes however many prior
// bars it actually has, clamped to [0,2], NOT Sierra Chart's raw Index value
// itself, so this function stays fully decoupled from ACSIL indexing
// semantics).
inline MacdResult ComputeMacd(MacdState& state, double diffCurrent, double diffPrev1, double diffPrev2, int barsAvailable) {
    // ── Robust z-score (median/MAD) — Taleb fat-tail safe ──
    // Mirrors FI2Signal::setFromChart normalization.
    // MACD histogram is unbounded (Extremistan) → standard z is forbidden.
    state.zScoreHistory[static_cast<size_t>(state.historyIdx)] = diffCurrent;
    state.historyIdx = (state.historyIdx + 1) % MacdState::kLookback;
    if (state.historyCount < MacdState::kLookback) ++state.historyCount;

    float zScore = 0.0f;
    if (state.historyCount >= 5) {
        std::array<double, MacdState::kLookback> scratch;
        std::copy_n(state.zScoreHistory.begin(), state.historyCount, scratch.begin());
        const int n = state.historyCount;
        const int mid = n / 2;

        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
        double median = scratch[mid];

        for (int i = 0; i < n; ++i)
            scratch[static_cast<size_t>(i)] = std::abs(state.zScoreHistory[static_cast<size_t>(i)] - median);
        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
        double mad = scratch[mid];

        double denom = mad * MacdState::kMADConsistency;
        zScore = (denom > 1e-12)
            ? static_cast<float>((diffCurrent - median) / denom)
            : 0.0f;
    }

    MacdEnum newValue = MacdEnum::AT_ZERO;

    // Check for zero-line crossings (highest priority)
    // Check specific patterns first (ZERO_FROM_BELOW/ABOVE require 3 bars + momentum)
    // Then evaluate simple crosses (BULLISH/BEARISH_CROSS require 2 bars only)
    if (barsAvailable >= 2) {
        // ZERO_FROM_BELOW: Crossing to positive with momentum (3-bar pattern)
        if (MacdIsZeroFromBelow(diffCurrent, diffPrev1, diffPrev2)) {
            newValue = MacdEnum::ZERO_FROM_BELOW;
        }
        // ZERO_FROM_ABOVE: Crossing to negative with momentum (3-bar pattern)
        else if (MacdIsZeroFromAbove(diffCurrent, diffPrev1, diffPrev2)) {
            newValue = MacdEnum::ZERO_FROM_ABOVE;
        }
        // Bullish cross: Simple cross from negative to positive (2-bar pattern)
        else if (diffCurrent > 0.0 && diffPrev1 <= 0.0) {
            newValue = MacdEnum::BULLISH_CROSS;
        }
        // Bearish cross: Simple cross from positive to negative (2-bar pattern)
        else if (diffCurrent < 0.0 && diffPrev1 >= 0.0) {
            newValue = MacdEnum::BEARISH_CROSS;
        }
        // MACD is positive
        else if (diffCurrent > 0.0) {
            if (MacdIsSummer(diffCurrent, diffPrev1, diffPrev2))
                newValue = MacdEnum::SUMMER;
            else if (MacdIsFall(diffCurrent, diffPrev1, diffPrev2))
                newValue = MacdEnum::FALL;
            else if (MacdIsPositiveTickDown(diffCurrent, diffPrev1, diffPrev2))
                newValue = MacdEnum::POS_TICK_DOWN;
            else if (diffCurrent > diffPrev1)
                newValue = MacdEnum::SUMMER;  // Positive and rising (simple case)
            else if (diffCurrent < diffPrev1)
                newValue = MacdEnum::FALL;     // Positive and falling (simple case)
            else
                newValue = MacdEnum::POSITIVE_FLAT;  // Above zero, consolidating
        }
        // MACD is negative
        else if (diffCurrent < 0.0) {
            if (MacdIsWinter(diffCurrent, diffPrev1, diffPrev2))
                newValue = MacdEnum::WINTER;
            else if (MacdIsSpring(diffCurrent, diffPrev1, diffPrev2))
                newValue = MacdEnum::SPRING;
            else if (MacdIsNegativeTickUp(diffCurrent, diffPrev1, diffPrev2))
                newValue = MacdEnum::NEG_TICK_UP;
            else if (diffCurrent < diffPrev1)
                newValue = MacdEnum::WINTER;   // Negative and falling (simple case)
            else if (diffCurrent > diffPrev1)
                newValue = MacdEnum::SPRING;    // Negative and rising (simple case)
            else
                newValue = MacdEnum::NEGATIVE_FLAT;  // Below zero, consolidating
        }
        // MACD is exactly zero (no additional pattern - already checked crosses above)
        else {
            newValue = MacdEnum::AT_ZERO;
        }
    }
    // barsAvailable < 2: use simplified 2-bar cross detection while history is building
    else if (barsAvailable >= 1) {
        // Simple 2-bar crosses when we don't have 3 bars of history yet
        if (diffCurrent > 0.0 && diffPrev1 <= 0.0) {
            newValue = MacdEnum::BULLISH_CROSS;
        }
        else if (diffCurrent < 0.0 && diffPrev1 >= 0.0) {
            newValue = MacdEnum::BEARISH_CROSS;
        }
    }

    return MacdResult{ newValue, zScore };
}

// ============================================================================
// Impulse (indicator-manager-dod-soa plan, Task 7): pure classification +
// derived-metrics computation, extracted from Impulse::SetFromColor
// (src/Indicator.cpp, pre-Task-7). ImpulseEnum moves here for the same
// ACSIL-independence reason MacdEnum did (Task 6) — this header must stay
// includable with just `-I include`.
//
// GREEN/RED/BLUE are NOT duplicated here: Indicator.h already defines them as
// plain `const int` (CreateRGB(...) results) BEFORE it includes this header,
// so redeclaring them here would collide. ComputeImpulse instead takes them
// as explicit int parameters (greenColor/redColor/blueColor) — the caller
// (Impulse::SetFromColor) passes the real GREEN/RED/BLUE constants; the pure
// function itself stays opaque to what those values actually are, exactly
// like it already only takes `color`/`prevColor` as plain ints.
// ============================================================================

// Elite v2.3: Symmetric Scale [-4, +4]
// Positive = Bullish momentum, Negative = Bearish momentum, Magnitude = Urgency/Strength
// v5.1: BLUE_BULL/BLUE_BEAR split — fixes the "Blue Bug" where two opposite market states
//       (EMA↑ MACD↓ vs EMA↓ MACD↑) mapped to the same neutral embedding index.
enum class ImpulseEnum : int8_t
{
    // Bullish Spectrum (Positive)
    GREEN_TO_BLUE_BEAR = 5,  // Green fading, maDiff reversed bearish — early short hint (floatValue: 0.10)
    BLUE_BULL = 4,           // Bullish Blue: EMA rising, MACD falling (floatValue: 0.15)
    GREEN = 3,               // Full Bullish Flow: MACD & EMA rising (floatValue: 1.0)
    BLUE_TO_GREEN = 2,       // Bullish Ignition: High-scoring early entry (floatValue: 0.66)
    GREEN_TO_BLUE_BULL = 1,  // Green fading, maDiff still bullish — structure intact (floatValue: 0.40)

    // Equilibrium
    BLUE = 0,                // True Neutral: maDiff ≈ 0 or legacy data (floatValue: 0.0)
    UNDEFINED = 7,           // Not enough data (maps to 0.0)

    // Bearish Spectrum (Negative)
    RED_TO_BLUE_BEAR = -1,   // Red fading, maDiff still bearish — structure intact (floatValue: -0.40)
    BLUE_TO_RED = -2,        // Bearish Ignition: High-scoring early entry (floatValue: -0.66)
    RED = -3,                // Full Bearish Flow: MACD & EMA falling (floatValue: -1.0)
    BLUE_BEAR = -4,          // Bearish Blue: EMA falling, MACD rising (floatValue: -0.15)
    RED_TO_BLUE_BULL = -5    // Red fading, maDiff reversed bullish — early long hint (floatValue: -0.10)
};

// Running state carried between ticks. magnitude/runLength/colorHistory are
// all feedback into the next call (magnitude -> next call's "previous
// magnitude" for fatigue; runLength increments/resets; colorHistory is the
// 16-bar bit-packed transition window) — Impulse::SetFromColor's former
// m_magnitude/m_runLength/m_colorHistory private members, moved verbatim.
struct ImpulseState {
    float    magnitude     = 0.0f;
    uint8_t  runLength     = 0;
    uint16_t colorHistory  = 0;
};

struct ImpulseResult {
    ImpulseEnum signal;
    uint8_t     runLength;
    float       magnitude;
    float       fatigue;
    float       transitionRate;
};

// Transcribed verbatim from Impulse::SetFromColor (src/Indicator.cpp, pre-Task-7).
inline ImpulseResult ComputeImpulse(ImpulseState& state, int color, int prevColor,
                                     float maDiff, float macdDiff, float atr,
                                     int greenColor, int redColor, int blueColor) {
    ImpulseEnum newValue = ImpulseEnum::UNDEFINED;

    if (color == greenColor) {
        newValue = (prevColor == blueColor) ? ImpulseEnum::BLUE_TO_GREEN : ImpulseEnum::GREEN;
    } else if (color == redColor) {
        newValue = (prevColor == blueColor) ? ImpulseEnum::BLUE_TO_RED : ImpulseEnum::RED;
    } else if (color == blueColor) {
        if (prevColor == greenColor) {
            // v5.3 Event-driven split: maDiff polarity known at this tick,
            // no need to wait for bar close.
            newValue = (maDiff > 0.0f) ? ImpulseEnum::GREEN_TO_BLUE_BULL
                                       : ImpulseEnum::GREEN_TO_BLUE_BEAR;
        } else if (prevColor == redColor) {
            newValue = (maDiff < 0.0f) ? ImpulseEnum::RED_TO_BLUE_BEAR
                                       : ImpulseEnum::RED_TO_BLUE_BULL;
        } else {
            // v5.1 Blue Bug Fix: Determine blue polarity from EMA direction.
            if (maDiff > 0.0f)
                newValue = ImpulseEnum::BLUE_BULL;
            else if (maDiff < 0.0f)
                newValue = ImpulseEnum::BLUE_BEAR;
            else
                newValue = ImpulseEnum::BLUE;  // True neutral (maDiff exactly 0, rare)
        }
    }

    // --- v5.2 Institutional-grade derived metrics ---

    // 1. Magnitude: ATR-normalized momentum strength, clamped to [-1, +1].
    //    Combines EMA slope and MACD-H slope into one continuous signal.
    const float prevMagnitude = state.magnitude;
    float magnitude;
    if (atr > 0.00001f) {
        const float maComponent = maDiff / atr;
        const float macdComponent = macdDiff / atr;
        magnitude = std::clamp((maComponent + macdComponent) * 0.5f, -1.0f, 1.0f);
    } else {
        magnitude = 0.0f;
    }
    state.magnitude = magnitude;

    // 2. Fatigue: Δ(magnitude).  Positive = accelerating, negative = fading.
    const float fatigue = magnitude - prevMagnitude;

    // 3. Run length: consecutive bars in the same color bucket.
    //    We bucket by raw color (GREEN/RED/BLUE), not by the refined enum.
    const bool sameColor = (color == prevColor);
    if (sameColor && state.runLength < 255) {
        ++state.runLength;
    } else if (!sameColor) {
        state.runLength = 1;
    }

    // 4. Transition rate: fraction of recent 16 bars with a color change.
    //    Shift history left, set bit-0 if color changed this bar.
    state.colorHistory = static_cast<uint16_t>((state.colorHistory << 1) | (sameColor ? 0u : 1u));
    // popcount via compiler intrinsic for the 16-bit window
    const float transitionRate = static_cast<float>(__builtin_popcount(state.colorHistory)) / 16.0f;

    return ImpulseResult{ newValue, state.runLength, magnitude, fatigue, transitionRate };
}

// ============================================================================
// VolumeIndicator (indicator-manager-dod-soa plan, Task 7): pure log-volume
// robust z-score (session-segregated) + self-classification + order-flow
// imbalance, extracted from VolumeIndicator::SampleBarVolume/UpdateVolume and
// the anonymous-namespace RobustLogZ() helper (src/Indicator.cpp, pre-Task-7).
// VolumeEnum moves here for the same ACSIL-independence reason as
// MacdEnum/ImpulseEnum above.
// ============================================================================

enum class VolumeEnum : int8_t
{
    NORMAL = 0,
    HIGH = 1,
    VERY_HIGH = 2,
    LOW = 3,
    VERY_LOW = 4,
    HIGH_BUY_VOLUME = 5,
    HIGH_SELL_VOLUME = 6
};

// Two deseasonalized baselines routed by session (RTH vs overnight) — 50
// same-session bars ≈ recent volume regime (fast-adaptive, unlike the 21-day
// Amihud window — volume regime shifts quickly). volumeZScore is the only
// value carried forward into UpdateVolume's per-tick self-classification.
struct VolumeState {
    static constexpr int kLookback = 50;

    std::array<double, kLookback> logVolRth{};
    int   logVolRthCount = 0;
    int   logVolRthIdx = 0;
    std::array<double, kLookback> logVolOvn{};
    int   logVolOvnCount = 0;
    int   logVolOvnIdx = 0;
    float volumeZScore = 0.0f;
};

// Robust z-score of a value's log within a session pool: median/MAD in
// log-space. MAD (50% breakdown) + log transform are the fat-tail-appropriate
// treatment for volume (lognormal under the Mixture-of-Distributions
// Hypothesis). Transcribed verbatim from the anonymous-namespace RobustLogZ()
// (src/Indicator.cpp, pre-Task-7).
inline float ComputeRobustLogZ(const double* buf, int count, double logVol) {
    if (count < 5) return 0.0f;  // need >=5 samples for a stable MAD
    std::array<double, VolumeState::kLookback> scratch;
    std::copy_n(buf, count, scratch.begin());
    const int mid = count / 2;

    std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + count);
    const double median = scratch[mid];

    for (int i = 0; i < count; ++i) scratch[static_cast<size_t>(i)] = std::abs(buf[i] - median);
    std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + count);
    const double mad = scratch[mid];

    const double denom = mad * 1.4826;  // MAD -> sigma consistency under normality
    return (denom > 1e-12) ? static_cast<float>((logVol - median) / denom) : 0.0f;
}

// Per closed 15-min bar: route the completed bar into its session pool
// (deseasonalization) and recompute the robust log-vol z-score. RTH and
// overnight volume distributions differ materially (intraday U-shape/MDH), so
// a pooled baseline is bimodal and miscalibrates the z. No-op guard mirrors
// the original's log-of-nonpositive protection.
inline void ComputeVolumeBarSample(VolumeState& state, float completedBarVolume, bool isRTH) {
    if (completedBarVolume <= 0.0f) return;
    const double logVol = std::log(static_cast<double>(completedBarVolume));

    double* buf = isRTH ? state.logVolRth.data() : state.logVolOvn.data();
    int&    cnt = isRTH ? state.logVolRthCount    : state.logVolOvnCount;
    int&    idx = isRTH ? state.logVolRthIdx       : state.logVolOvnIdx;

    buf[idx] = logVol;
    idx = (idx + 1) % VolumeState::kLookback;
    if (cnt < VolumeState::kLookback) ++cnt;

    state.volumeZScore = ComputeRobustLogZ(buf, cnt, logVol);
}

struct VolumeClassification {
    VolumeEnum signal;
    float      imbalance;      // (askVol - bidVol) / totalVol, bounded [-1, +1]
    float      totalVolume;    // bid+ask contracts behind the imbalance
};

// Per-tick: self-classify VolumeEnum against the current (per-bar,
// session-aware) z-score baseline, folding in the live order-flow imbalance
// for the buy/sell split, and refresh the imbalance itself. Pure function of
// the already-updated volumeZScore (read-only here; ComputeVolumeBarSample
// owns writing it).
inline VolumeClassification ComputeVolumeClassification(float volumeZScore, float bidVolume, float askVolume) {
    VolumeEnum classified = VolumeEnum::NORMAL;
    if (volumeZScore < -2.0f) {
        classified = VolumeEnum::VERY_LOW;
    } else if (volumeZScore < -1.0f) {
        classified = VolumeEnum::LOW;
    } else if (volumeZScore > 2.0f) {
        classified = VolumeEnum::VERY_HIGH;
    } else if (volumeZScore > 1.0f) {
        if (askVolume > bidVolume) {
            classified = VolumeEnum::HIGH_BUY_VOLUME;
        } else if (bidVolume > askVolume) {
            classified = VolumeEnum::HIGH_SELL_VOLUME;
        } else {
            classified = VolumeEnum::HIGH;
        }
    }

    const float totalVol = bidVolume + askVolume;
    const float imbalance = (totalVol > 0.0f) ? (askVolume - bidVolume) / totalVol : 0.0f;

    return VolumeClassification{ classified, imbalance, totalVol };
}

// ============================================================================
// Oscillator310 (indicator-manager-dod-soa plan, Task 7): pure crossover
// detection, extracted from Oscillator310::DetectCross (Indicator.h,
// pre-Task-7). Oscillator310CrossEnum moves here for the same
// ACSIL-independence reason as MacdEnum/ImpulseEnum/VolumeEnum above.
// Stateless — genuinely only needs its four arguments (design spec §3.5's
// "stateless compute extraction" case), so no state struct is needed; the
// caller (Oscillator310::UpdateOscillator) already owns the fastLine/
// slowLine/prevFastLine/prevSlowLine history itself.
// ============================================================================

enum class Oscillator310CrossEnum : int8_t {
    NEUTRAL = 0,
    BULLISH_CROSS = 1,
    BEARISH_CROSS = 2
};

inline Oscillator310CrossEnum ComputeOscillator310Cross(float fastLine, float slowLine,
                                                          float prevFastLine, float prevSlowLine) {
    const bool fastAboveNow = (fastLine > slowLine);
    const bool fastAbovePrev = (prevFastLine > prevSlowLine);

    if (fastAboveNow && !fastAbovePrev) return Oscillator310CrossEnum::BULLISH_CROSS;
    if (!fastAboveNow && fastAbovePrev) return Oscillator310CrossEnum::BEARISH_CROSS;
    return Oscillator310CrossEnum::NEUTRAL;
}

// ============================================================================
// KangarooTail / TurtleSoup / MomentumPinball / ElderBreakout / NR7
// (indicator-manager-dod-soa plan, Task 8): pure pattern-detection functions,
// extracted verbatim from StudyHelperFunctions.cpp's DetectKangarooTail/
// DetectTurtleSoup/DetectMomentumPinball/DetectElderBreakout/DetectNR7 (which
// already took only primitive float/int/double/bool arguments — no ACSIL
// dependency in the bodies, only in the header they used to be declared in,
// include/StudyHelperFunctions.h, which includes sierrachart.h). The five
// enums move here for the same ACSIL-independence reason as MacdEnum/
// ImpulseEnum/VolumeEnum/Oscillator310CrossEnum above.
//
// Working-state exception (design spec §3.4): each pattern's leaf class in
// Indicator.h (KangarooTail/TurtleSoup/MomentumPinball/ElderBreakout/NR7)
// also carries small internal-only context fields set via a separate
// SetContext(...) call from TripleScreenX.cpp (e.g. atSupportLevel, hurst,
// screenAligned) that are NOT computed by these Detect* functions and NOT
// read back anywhere outside the leaf class itself (confirmed via full-repo
// grep at Task 8 time) — only the published enum + quality score are packed.
// No struct parameter is introduced for that context here because no compute
// function actually consumes it as an argument; see each leaf class's comment
// in Indicator.h for the authoritative field list.
// ============================================================================

/**
 * KangarooTailEnum: Elder's price-action reversal pattern (pattern detection layer).
 *
 * Kangaroo tail = long shadow (wick) showing rejection of price extreme.
 *
 * Bullish: Long lower tail, close near high
 * - Bears pushed price down, buyers aggressively rejected and closed high
 * - "Stop-loss hunters flushed out, now reverse"
 * - Most powerful at support levels or after MACD divergence
 *
 * Bearish: Long upper tail, close near low
 * - Bulls pushed price up, sellers aggressively rejected and closed low
 * - Most powerful at resistance levels
 *
 * Elder's Criteria:
 * - Tail ≥ 2× body size (2.5-4× = strong, >4× = extreme)
 * - Tail ≥ 0.5× ATR (must be meaningful relative to volatility)
 * - Close in upper 75% of range (bullish) or lower 25% (bearish)
 * - Screen3 (15-min) primary use: Precise entry confirmation
 *
 * Integration with RaschkeTacticalTrigger:
 * - KangarooTailEnum = Pattern detection (WEAK/STRONG/EXTREME granularity)
 * - RaschkeTacticalTrigger = Entry decision (KANGAROO_TAIL_BUY/SELL action)
 * - Entry logic: if (tail == STRONG/EXTREME + context valid) → tactical trigger
 */
enum class KangarooTailEnum : int8_t
{
    NONE = 0,

    // Bullish patterns (long lower tail)
    BULLISH_WEAK = 1,          // Tail 2.0-2.5× body, moderate buyer rejection
    BULLISH_STRONG = 2,        // Tail 2.5-4× body, strong buyer rejection
    BULLISH_EXTREME = 3,       // Tail >4× body + >1× ATR, extreme buyer power

    // Bearish patterns (long upper tail)
    BEARISH_WEAK = -1,         // Tail 2.0-2.5× body, moderate seller rejection
    BEARISH_STRONG = -2,       // Tail 2.5-4× body, strong seller rejection
    BEARISH_EXTREME = -3       // Tail >4× body + >1× ATR, extreme seller power
};

/**
 * DetectKangarooTail: Implements Elder's Kangaroo tail price-action pattern.
 *
 * Elder's Definition: Long shadow (tail) showing aggressive rejection of price extreme.
 * - Bullish: Long lower tail, close near high (buyers rejected lower prices)
 * - Bearish: Long upper tail, close near low (sellers rejected higher prices)
 *
 * Elder's Criteria (from "Trading for a Living" and "Come Into My Trading Room"):
 * - Tail ≥ 2× body size (shows strong rejection)
 * - Tail ≥ 0.5× ATR (must be meaningful vs volatility, not just noise)
 * - Close in upper 75% (bullish) or lower 25% (bearish) of bar range
 *
 * Strength Levels:
 * - WEAK: Tail 2.0-2.5× body, tail ≥0.5× ATR
 * - STRONG: Tail 2.5-4× body, tail ≥0.5× ATR
 * - EXTREME: Tail >4× body AND tail ≥1.0× ATR
 *
 * Linda Raschke adds: "Most reliable at support/resistance levels and after divergences"
 */
inline KangarooTailEnum DetectKangarooTail(
    float open,
    float high,
    float low,
    float close,
    float atr,
    float& tailToBodyRatio,
    float& tailToATR,
    float& closePosition,
    float& qualityScore
) {
    // Initialize output parameters
    tailToBodyRatio = 0.0f;
    tailToATR = 0.0f;
    closePosition = 0.0f;
    qualityScore = 0.0f;

    // Validate inputs
    if (high <= low || atr <= 0.0f) {
        return KangarooTailEnum::NONE;
    }

    // Calculate bar components
    const float bodySize = std::abs(close - open);
    const float minBodySize = 0.001f;  // Prevent division by zero for doji bars
    const float effectiveBodySize = std::max(bodySize, minBodySize);

    const float lowerTail = std::min(open, close) - low;
    const float upperTail = high - std::max(open, close);
    const float barRange = high - low;

    // Calculate tail-to-body ratios
    const float lowerTailToBody = lowerTail / effectiveBodySize;
    const float upperTailToBody = upperTail / effectiveBodySize;

    // Calculate tail-to-ATR ratios (Elder's volatility context)
    const float lowerTailToATR = lowerTail / atr;
    const float upperTailToATR = upperTail / atr;

    // Calculate close position in bar range (0.0 = at low, 1.0 = at high)
    closePosition = (barRange > 0.001f) ? ((close - low) / barRange) : 0.5f;

    // --- Bullish Kangaroo Tail Detection ---
    // Requirements:
    // 1. Long lower tail (≥1.5× body)
    // 2. Tail is meaningful vs volatility (≥0.3× ATR)
    // 3. Close near high (upper 60% of bar range)
    if (lowerTailToBody >= 1.5f && lowerTailToATR >= 0.3f && closePosition >= 0.60f) {
        tailToBodyRatio = lowerTailToBody;
        tailToATR = lowerTailToATR;

        // Calculate quality score (0.0-1.0)
        float baseScore = 0.0f;

        // Determine strength level and base score
        KangarooTailEnum result = KangarooTailEnum::NONE;
        if (lowerTailToBody >= 4.0f && lowerTailToATR >= 1.0f) {
            result = KangarooTailEnum::BULLISH_EXTREME;
            baseScore = 0.6f;  // Extreme strength (raised from 0.4)
        } else if (lowerTailToBody >= 2.5f) {
            result = KangarooTailEnum::BULLISH_STRONG;
            baseScore = 0.4f;  // Strong (raised from 0.25)
        } else {
            result = KangarooTailEnum::BULLISH_WEAK;
            baseScore = 0.2f;  // Weak but valid (raised from 0.15)
        }

        // Add ATR significance bonus (0.0-0.3)
        float atrBonus = 0.0f;
        if (lowerTailToATR >= 1.5f) {
            atrBonus = 0.3f;  // Very significant vs volatility
        } else if (lowerTailToATR >= 1.0f) {
            atrBonus = 0.2f;  // Significant
        } else {
            atrBonus = 0.1f;  // Meaningful
        }

        // Add close position bonus (how close to high)
        float closeBonus = (closePosition - 0.60f) / 0.40f * 0.1f;  // 0.0-0.1 based on how far into upper 40%

        qualityScore = std::min(1.0f, baseScore + atrBonus + closeBonus);
        return result;
    }

    // --- Bearish Kangaroo Tail Detection ---
    // Requirements:
    // 1. Long upper tail (≥1.5× body)
    // 2. Tail is meaningful vs volatility (≥0.3× ATR)
    // 3. Close near low (lower 40% of bar range)
    if (upperTailToBody >= 1.5f && upperTailToATR >= 0.3f && closePosition <= 0.40f) {
        tailToBodyRatio = upperTailToBody;
        tailToATR = upperTailToATR;

        // Calculate quality score (0.0-1.0)
        float baseScore = 0.0f;

        // Determine strength level and base score
        KangarooTailEnum result = KangarooTailEnum::NONE;
        if (upperTailToBody >= 4.0f && upperTailToATR >= 1.0f) {
            result = KangarooTailEnum::BEARISH_EXTREME;
            baseScore = 0.6f;  // Extreme strength (raised from 0.4)
        } else if (upperTailToBody >= 2.5f) {
            result = KangarooTailEnum::BEARISH_STRONG;
            baseScore = 0.4f;  // Strong (raised from 0.25)
        } else {
            result = KangarooTailEnum::BEARISH_WEAK;
            baseScore = 0.2f;  // Weak but valid (raised from 0.15)
        }

        // Add ATR significance bonus (0.0-0.3)
        float atrBonus = 0.0f;
        if (upperTailToATR >= 1.5f) {
            atrBonus = 0.3f;  // Very significant vs volatility
        } else if (upperTailToATR >= 1.0f) {
            atrBonus = 0.2f;  // Significant
        } else {
            atrBonus = 0.1f;  // Meaningful
        }

        // Add close position bonus (how close to low)
        float closeBonus = (0.40f - closePosition) / 0.40f * 0.1f;  // 0.0-0.1 based on how far into lower 40%

        qualityScore = std::min(1.0f, baseScore + atrBonus + closeBonus);
        return result;
    }

    // No Kangaroo tail pattern detected
    return KangarooTailEnum::NONE;
}

/**
 * TurtleSoupEnum: Linda Raschke's false breakout reversal pattern (pattern detection layer).
 *
 * Turtle Soup = Price breaks recent extreme (4-day high/low), then closes back inside range.
 * This is a "stop hunt" or "false breakout" pattern where professionals trap amateurs.
 *
 * Bullish: Price breaks below 4-day low, closes back above it
 * - Amateurs stopped out at breakdown, professionals enter long
 * - "Soup" = Turtles (breakout traders) get eaten by sharks (counter-trend traders)
 * - Most powerful when 4-day low is near daily/weekly support
 *
 * Bearish: Price breaks above 4-day high, closes back below it
 * - Amateurs stopped out at breakout, professionals enter short
 * - Most powerful when 4-day high is near daily/weekly resistance
 *
 * Raschke's Criteria:
 * - Penetration: Price must break 4-day extreme by meaningful distance (≥0.1× ATR)
 * - Close-back: Close must return inside range by meaningful distance (≥0.1× ATR)
 * - WEAK: Penetration 0.1-0.3× ATR, moderate false breakout
 * - STRONG: Penetration 0.3-0.5× ATR, close near opposite extreme (≥40% of bar range)
 * - EXTREME: Penetration >0.5× ATR, close at bar extreme (≥80% of range), often with volume spike
 * - Screen3 (15-min) primary use: Precise entry timing
 *
 * Integration with RaschkeTacticalTrigger:
 * - TurtleSoupEnum = Pattern detection (WEAK/STRONG/EXTREME granularity)
 * - RaschkeTacticalTrigger = Entry decision (TURTLE_SOUP_BUY/SELL action)
 * - Entry logic: if (soup == STRONG/EXTREME + quality ≥0.6 + at daily high/low) → tactical trigger
 *
 * Source: "Street Smarts" by Linda Raschke (1995), Chapter 8: The Turtle Soup Pattern
 * Defined in: include/Indicator.h
 * Computed by: StudyHelperFunctions::DetectTurtleSoup()
 * Used by: TripleScreen3.cpp::scsf_Screen3_TurtleSoup() (15-min bars)
 */
enum class TurtleSoupEnum : int8_t
{
    NONE = 0,

    // Bullish patterns (break below 4-day low, close back inside)
    BULLISH_WEAK = 1,          // Penetration 0.1-0.3× ATR, moderate stop hunt
    BULLISH_STRONG = 2,        // Penetration 0.3-0.5× ATR, close near high (strong reversal)
    BULLISH_EXTREME = 3,       // Penetration >0.5× ATR, close ≥80% of range (professional trap)

    // Bearish patterns (break above 4-day high, close back inside)
    BEARISH_WEAK = -1,         // Penetration 0.1-0.3× ATR, moderate stop hunt
    BEARISH_STRONG = -2,       // Penetration 0.3-0.5× ATR, close near low (strong reversal)
    BEARISH_EXTREME = -3       // Penetration >0.5× ATR, close ≤20% of range (professional trap)
};

inline TurtleSoupEnum DetectTurtleSoup(
    float /* open */,
    float high,
    float low,
    float close,
    float fourDayHigh,
    float fourDayLow,
    float atr,
    float& penetrationDistance,
    float& closeDistance,
    float& closePosition,
    float& qualityScore
) {
    // Step 1: Validate inputs
    if (high <= low || atr <= 0.0f || fourDayHigh <= fourDayLow) {
        return TurtleSoupEnum::NONE;
    }

    const float MIN_PENETRATION_ATR = 0.05f;  // Minimum 0.05× ATR (lowered from 0.1 for 15-min timeframes)
    const float barRange = high - low;

    // Step 2: Calculate close position in bar range (0.0-1.0)
    closePosition = (barRange > 0.001f) ? ((close - low) / barRange) : 0.5f;

    // Step 3: Bullish Turtle Soup Detection (break below 4-day low, close back inside)
    // Requirements:
    // 1. Low breaks below 4-day low (amateur stops triggered)
    // 2. Close is back above 4-day low (professionals enter)
    // 3. Penetration meaningful vs volatility (≥0.1× ATR)
    if (low < fourDayLow && close > fourDayLow) {
        // Calculate penetration distance (how far price broke beyond extreme)
        const float penetration = fourDayLow - low;  // Positive value
        penetrationDistance = penetration / atr;

        // Verify minimum penetration (filter noise)
        if (penetrationDistance < MIN_PENETRATION_ATR) {
            return TurtleSoupEnum::NONE;
        }

        // Calculate close-back-inside distance (how far close recovered)
        const float closeBack = close - fourDayLow;  // Positive value
        closeDistance = closeBack / atr;

        // Determine strength classification and calculate quality score
        TurtleSoupEnum result;
        float baseScore = 0.0f;

        // EXTREME: Deep penetration (>0.5× ATR) + close near high (≥80% of range)
        if (penetrationDistance > 0.5f && closePosition >= 0.80f) {
            result = TurtleSoupEnum::BULLISH_EXTREME;
            baseScore = 0.6f;  // Extreme strength: panic selling exhausted (raised from 0.4)
        }
        // STRONG: Moderate penetration (0.3-0.5× ATR) + close in upper half (≥60%)
        else if (penetrationDistance >= 0.3f && closePosition >= 0.60f) {
            result = TurtleSoupEnum::BULLISH_STRONG;
            baseScore = 0.4f;  // Strong rejection of breakdown (raised from 0.25)
        }
        // WEAK: Light penetration (0.05-0.3× ATR) - lowered threshold
        else if (penetrationDistance >= MIN_PENETRATION_ATR) {
            result = TurtleSoupEnum::BULLISH_WEAK;
            baseScore = 0.2f;  // Marginal stop hunt (raised from 0.15)
        }
        else {
            return TurtleSoupEnum::NONE;  // Too small to matter
        }

        // Add close-back bonus (how strongly price recovered)
        // More recovery inside = stronger reversal conviction
        float closeBackBonus = 0.0f;
        if (closeDistance >= 0.5f) {
            closeBackBonus = 0.3f;  // Deep recovery (>0.5× ATR inside)
        } else if (closeDistance >= 0.3f) {
            closeBackBonus = 0.2f;  // Moderate recovery
        } else {
            closeBackBonus = 0.1f;  // Light recovery
        }

        // Add close position bonus (where in bar range close occurred)
        // Higher close = more bullish conviction
        float closePositionBonus = 0.0f;
        if (closePosition >= 0.80f) {
            closePositionBonus = 0.1f;  // Close at/near high (strongest)
        } else if (closePosition >= 0.60f) {
            closePositionBonus = 0.05f;  // Close upper half
        }

        qualityScore = std::min(1.0f, baseScore + closeBackBonus + closePositionBonus);
        return result;
    }

    // Step 4: Bearish Turtle Soup Detection (break above 4-day high, close back inside)
    // Requirements:
    // 1. High breaks above 4-day high (amateur stops triggered)
    // 2. Close is back below 4-day high (professionals enter short)
    // 3. Penetration meaningful vs volatility (≥0.1× ATR)
    if (high > fourDayHigh && close < fourDayHigh) {
        // Calculate penetration distance (how far price broke beyond extreme)
        const float penetration = high - fourDayHigh;  // Positive value
        penetrationDistance = penetration / atr;

        // Verify minimum penetration (filter noise)
        if (penetrationDistance < MIN_PENETRATION_ATR) {
            return TurtleSoupEnum::NONE;
        }

        // Calculate close-back-inside distance (how far close recovered)
        const float closeBack = fourDayHigh - close;  // Positive value
        closeDistance = closeBack / atr;

        // Determine strength classification and calculate quality score
        TurtleSoupEnum result;
        float baseScore = 0.0f;

        // EXTREME: Deep penetration (>0.5× ATR) + close near low (≤20% of range)
        if (penetrationDistance > 0.5f && closePosition <= 0.20f) {
            result = TurtleSoupEnum::BEARISH_EXTREME;
            baseScore = 0.6f;  // Extreme strength: panic buying exhausted (raised from 0.4)
        }
        // STRONG: Moderate penetration (0.3-0.5× ATR) + close in lower half (≤40%)
        else if (penetrationDistance >= 0.3f && closePosition <= 0.40f) {
            result = TurtleSoupEnum::BEARISH_STRONG;
            baseScore = 0.4f;  // Strong rejection of breakout (raised from 0.25)
        }
        // WEAK: Light penetration (0.05-0.3× ATR) - lowered threshold
        else if (penetrationDistance >= MIN_PENETRATION_ATR) {
            result = TurtleSoupEnum::BEARISH_WEAK;
            baseScore = 0.2f;  // Marginal stop hunt (raised from 0.15)
        }
        else {
            return TurtleSoupEnum::NONE;  // Too small to matter
        }

        // Add close-back bonus (how strongly price recovered)
        float closeBackBonus = 0.0f;
        if (closeDistance >= 0.5f) {
            closeBackBonus = 0.3f;  // Deep recovery (>0.5× ATR inside)
        } else if (closeDistance >= 0.3f) {
            closeBackBonus = 0.2f;  // Moderate recovery
        } else {
            closeBackBonus = 0.1f;  // Light recovery
        }

        // Add close position bonus (where in bar range close occurred)
        // Lower close = more bearish conviction
        float closePositionBonus = 0.0f;
        if (closePosition <= 0.20f) {
            closePositionBonus = 0.1f;  // Close at/near low (strongest)
        } else if (closePosition <= 0.40f) {
            closePositionBonus = 0.05f;  // Close lower half
        }

        qualityScore = std::min(1.0f, baseScore + closeBackBonus + closePositionBonus);
        return result;
    }

    // No Turtle Soup pattern detected
    return TurtleSoupEnum::NONE;
}

/**
 * MomentumPinballEnum: Linda Raschke's momentum-based mean-reversion pattern (pattern detection layer).
 *
 * Momentum Pinball = RSI cross + Stochastic extreme = early reversal signal
 * Named "Pinball" because price bounces off extreme (like pinball off bumper)
 *
 * Bullish: RSI3 crosses above RSI10 + Stochastic oversold (<20)
 * - Momentum shifts from down to up (RSI cross)
 * - Price at extreme (Stochastic <20)
 * - "Catch the bounce off support" — Raschke
 * - Most powerful after FI2 pullback + fresh Impulse green
 *
 * Bearish: RSI3 crosses below RSI10 + Stochastic overbought (>80)
 * - Momentum shifts from up to down (RSI cross)
 * - Price at extreme (Stochastic >80)
 * - Most powerful after FI2 rally + fresh Impulse red
 *
 * Raschke's Strength Classification:
 * - WEAK: Fresh cross, stoch barely extreme (15-20 or 80-85), no volume
 * - STRONG: Strong RSI delta (≥5 pts), stoch deep (10-15 or 85-90), FI2 aligned
 * - EXTREME: Fresh Impulse change + deep stoch (<10 or >90) + volume spike (≥1.5× avg)
 *
 * Integration with RaschkeTacticalTrigger:
 * - MomentumPinballEnum = Pattern detection (WEAK/STRONG/EXTREME granularity)
 * - RaschkeTacticalTrigger = Entry decision (MOMENTUM_PINBALL_BUY/SELL action)
 * - Entry logic: if (pinball == STRONG/EXTREME + quality ≥0.6) → tactical trigger
 */
enum class MomentumPinballEnum : int8_t
{
    NONE = 0,

    // Bullish patterns (RSI3 > RSI10 + Stochastic oversold)
    BULLISH_WEAK = 1,          // Fresh cross, stoch 15-20, marginal setup
    BULLISH_STRONG = 2,        // RSI delta ≥5, stoch 10-15, FI2 pullback
    BULLISH_EXTREME = 3,       // Fresh Impulse green + stoch <10 + volume spike

    // Bearish patterns (RSI3 < RSI10 + Stochastic overbought)
    BEARISH_WEAK = -1,         // Fresh cross, stoch 80-85, marginal setup
    BEARISH_STRONG = -2,       // RSI delta ≤-5, stoch 85-90, FI2 rally
    BEARISH_EXTREME = -3       // Fresh Impulse red + stoch >90 + volume spike
};

/**
 * DetectMomentumPinball: Raschke's momentum-based mean-reversion pattern.
 *
 * Pattern combines:
 * 1. RSI3/RSI10 cross = momentum shift
 * 2. Stochastic extreme = price stretched
 * 3. Impulse color change = fresh trend shift (EXTREME only)
 * 4. Volume spike = institutional participation (EXTREME only)
 *
 * Classification:
 * - WEAK: Fresh cross, stoch barely extreme (15-20 or 80-85)
 * - STRONG: RSI delta ≥5, stoch deep (10-15 or 85-90), FI2 aligned
 * - EXTREME: Fresh Impulse + very deep stoch (<10 or >90) + volume ≥1.5× avg
 */
inline MomentumPinballEnum DetectMomentumPinball(
    float rsi3, float rsi10, float prevRSI3, float prevRSI10,
    float stochK, int impulseColor, int prevImpulseColor,
    double volume, double avgVolume,
    float& rsiDelta, float& stochDepth, bool& impulseJustChanged,
    float& volumeSpike, float& qualityScore
) {
    // Initialize output parameters
    rsiDelta = rsi3 - rsi10;
    stochDepth = stochK;
    impulseJustChanged = (impulseColor != prevImpulseColor);
    volumeSpike = (avgVolume > 0.001) ? static_cast<float>(volume / avgVolume) : 1.0f;
    qualityScore = 0.0f;

    // Validate inputs
    if (rsi3 < 0.0f || rsi3 > 100.0f || rsi10 < 0.0f || rsi10 > 100.0f ||
        stochK < 0.0f || stochK > 100.0f) {
        return MomentumPinballEnum::NONE;
    }

    // Detect RSI cross direction
    float prevRSIDelta = prevRSI3 - prevRSI10;
    bool freshBullishCross = (prevRSIDelta <= 0.0f && rsiDelta > 0.0f);  // RSI3 crosses above RSI10
    bool freshBearishCross = (prevRSIDelta >= 0.0f && rsiDelta < 0.0f);  // RSI3 crosses below RSI10

    // If no cross, no pattern
    if (!freshBullishCross && !freshBearishCross) {
        return MomentumPinballEnum::NONE;
    }

    // --- Bullish Momentum Pinball Detection ---
    // Requirements:
    // 1. RSI3 crosses above RSI10 (momentum shift bullish)
    // 2. Stochastic < 20 (oversold, ready to bounce)
    if (freshBullishCross && stochK < 20.0f) {
        // Determine strength classification
        MomentumPinballEnum result;
        float baseScore = 0.0f;

        // Calculate absolute RSI delta strength
        float absRSIDelta = std::abs(rsiDelta);

        // EXTREME: Fresh Impulse green + very deep oversold + volume spike
        // Impulse green = 1 (from GetImpulse function)
        const int COLOR_GREEN = 1;
        if (impulseJustChanged && impulseColor == COLOR_GREEN &&
            stochK < 10.0f && volumeSpike >= 1.5f) {
            result = MomentumPinballEnum::BULLISH_EXTREME;
            baseScore = 0.6f;  // Highest base quality (raised from 0.4)
        }
        // STRONG: Strong RSI delta (≥5) + deep oversold (10-15)
        else if (absRSIDelta >= 5.0f && stochK >= 10.0f && stochK <= 15.0f) {
            result = MomentumPinballEnum::BULLISH_STRONG;
            baseScore = 0.4f;  // Base quality for STRONG (raised from 0.25)
        }
        // WEAK: Marginal cross + barely oversold (15-20)
        else if (stochK >= 15.0f && stochK < 20.0f) {
            result = MomentumPinballEnum::BULLISH_WEAK;
            baseScore = 0.2f;  // Base quality for WEAK (raised from 0.15)
        }
        // Edge case: Very deep oversold (<10) but no fresh Impulse/volume
        else if (stochK < 10.0f) {
            result = MomentumPinballEnum::BULLISH_STRONG;  // Still strong pattern
            baseScore = 0.4f;  // Raised from 0.25
        }
        else {
            return MomentumPinballEnum::NONE;  // Doesn't meet criteria
        }

        // Quality Score Calculation
        // Base: 0.15 (WEAK), 0.25 (STRONG), 0.4 (EXTREME)

        // RSI Delta Bonus (0.0-0.2): Stronger cross = higher quality
        float rsiDeltaBonus = 0.0f;
        if (absRSIDelta >= 10.0f) {
            rsiDeltaBonus = 0.2f;  // Very strong momentum shift
        } else if (absRSIDelta >= 7.0f) {
            rsiDeltaBonus = 0.15f;  // Strong momentum shift
        } else if (absRSIDelta >= 5.0f) {
            rsiDeltaBonus = 0.1f;  // Moderate momentum shift
        } else if (absRSIDelta >= 3.0f) {
            rsiDeltaBonus = 0.05f;  // Light momentum shift
        }

        // Stochastic Depth Bonus (0.0-0.2): Deeper oversold = higher quality
        float stochBonus = 0.0f;
        if (stochK < 5.0f) {
            stochBonus = 0.2f;  // Extreme oversold (panic selling)
        } else if (stochK < 10.0f) {
            stochBonus = 0.15f;  // Very deep oversold
        } else if (stochK < 15.0f) {
            stochBonus = 0.1f;  // Deep oversold
        } else {
            stochBonus = 0.05f;  // Barely oversold
        }

        // Volume Bonus (0.0-0.1): Volume spike confirms institutional interest
        float volumeBonus = 0.0f;
        if (volumeSpike >= 2.0f) {
            volumeBonus = 0.1f;  // Major volume spike
        } else if (volumeSpike >= 1.5f) {
            volumeBonus = 0.05f;  // Moderate volume spike
        }

        qualityScore = std::min(1.0f, baseScore + rsiDeltaBonus + stochBonus + volumeBonus);
        return result;
    }

    // --- Bearish Momentum Pinball Detection ---
    // Requirements:
    // 1. RSI3 crosses below RSI10 (momentum shift bearish)
    // 2. Stochastic > 80 (overbought, ready to drop)
    if (freshBearishCross && stochK > 80.0f) {
        // Determine strength classification
        MomentumPinballEnum result;
        float baseScore = 0.0f;

        // Calculate absolute RSI delta strength
        float absRSIDelta = std::abs(rsiDelta);

        // EXTREME: Fresh Impulse red + very deep overbought + volume spike
        // Impulse red = -1 (from GetImpulse function)
        const int COLOR_RED = -1;
        if (impulseJustChanged && impulseColor == COLOR_RED &&
            stochK > 90.0f && volumeSpike >= 1.5f) {
            result = MomentumPinballEnum::BEARISH_EXTREME;
            baseScore = 0.6f;  // Raised from 0.4 to match bullish
        }
        // STRONG: Strong RSI delta (≥5) + deep overbought (85-90)
        else if (absRSIDelta >= 5.0f && stochK >= 85.0f && stochK <= 90.0f) {
            result = MomentumPinballEnum::BEARISH_STRONG;
            baseScore = 0.4f;  // Raised from 0.25 to match bullish
        }
        // WEAK: Marginal cross + barely overbought (80-85)
        else if (stochK > 80.0f && stochK <= 85.0f) {
            result = MomentumPinballEnum::BEARISH_WEAK;
            baseScore = 0.2f;  // Raised from 0.15 to match bullish
        }
        // Edge case: Very deep overbought (>90) but no fresh Impulse/volume
        else if (stochK > 90.0f) {
            result = MomentumPinballEnum::BEARISH_STRONG;
            baseScore = 0.4f;  // Raised from 0.25 to match bullish
        }
        else {
            return MomentumPinballEnum::NONE;
        }

        // Quality Score Calculation (mirror of bullish logic)

        // RSI Delta Bonus
        float rsiDeltaBonus = 0.0f;
        if (absRSIDelta >= 10.0f) {
            rsiDeltaBonus = 0.2f;
        } else if (absRSIDelta >= 7.0f) {
            rsiDeltaBonus = 0.15f;
        } else if (absRSIDelta >= 5.0f) {
            rsiDeltaBonus = 0.1f;
        } else if (absRSIDelta >= 3.0f) {
            rsiDeltaBonus = 0.05f;
        }

        // Stochastic Depth Bonus (inverted for overbought)
        float stochBonus = 0.0f;
        if (stochK > 95.0f) {
            stochBonus = 0.2f;  // Extreme overbought (panic buying)
        } else if (stochK > 90.0f) {
            stochBonus = 0.15f;  // Very deep overbought
        } else if (stochK > 85.0f) {
            stochBonus = 0.1f;  // Deep overbought
        } else {
            stochBonus = 0.05f;  // Barely overbought
        }

        // Volume Bonus
        float volumeBonus = 0.0f;
        if (volumeSpike >= 2.0f) {
            volumeBonus = 0.1f;
        } else if (volumeSpike >= 1.5f) {
            volumeBonus = 0.05f;
        }

        qualityScore = std::min(1.0f, baseScore + rsiDeltaBonus + stochBonus + volumeBonus);
        return result;
    }

    return MomentumPinballEnum::NONE;
}

/**
 * ElderBreakoutEnum: Dr. Elder/Linda Raschke Keltner Channel breakout pattern (pattern detection layer).
 *
 * Elder Breakout = Price closes beyond Keltner Channel band = volatility expansion signal
 * Named after Dr. Elder's use of volatility channels (Keltner = 20-period EMA ± 2.5× ATR)
 *
 * Bullish: Close above upper Keltner band
 * - Price breaks out of normal volatility range (upside expansion)
 * - Most powerful after consolidation at upper band (5+ bars)
 * - "Channel squeeze then expansion" — Elder/Raschke
 * - Best when Impulse GREEN + Hurst rising + Screen1 bullish
 *
 * Bearish: Close below lower Keltner band
 * - Price breaks out of normal volatility range (downside expansion)
 * - Most powerful after consolidation at lower band
 * - Best when Impulse RED + Hurst rising + Screen1 bearish
 *
 * Raschke's Strength Classification:
 * - WEAK: Barely beyond band (0.1-0.5× ATR), marginal breakout
 * - STRONG: Clear breakout (>0.5× ATR) + volume 1.5× avg + Hurst >0.55
 * - EXTREME: Large breakout (>1× ATR or gap) + 2-3× volume + after 5+ bar consolidation
 *
 * Context Enhancement:
 * - Channel squeeze: Bands narrowing (ATR compression) before breakout
 * - Impulse alignment: Impulse color matches breakout direction
 * - Screen1 support: Weekly trend confirms breakout direction
 *
 * Integration with RaschkeTacticalTrigger:
 * - ElderBreakoutEnum = Pattern detection (WEAK/STRONG/EXTREME granularity)
 * - RaschkeTacticalTrigger = Entry decision (ELDER_BREAKOUT_BUY/SELL action)
 * - Entry logic: if (breakout == STRONG/EXTREME + quality ≥0.6) → tactical trigger
 */
enum class ElderBreakoutEnum : int8_t
{
    NONE = 0,

    // Bullish patterns (close above upper Keltner band)
    BULLISH_WEAK = 1,          // Barely beyond band (0.1-0.5× ATR)
    BULLISH_STRONG = 2,        // Clear breakout (>0.5× ATR) + volume + Hurst >0.55
    BULLISH_EXTREME = 3,       // Large breakout (>1× ATR or gap) + surge volume + consolidation

    // Bearish patterns (close below lower Keltner band)
    BEARISH_WEAK = -1,         // Barely beyond band (0.1-0.5× ATR)
    BEARISH_STRONG = -2,       // Clear breakout (>0.5× ATR) + volume + Hurst >0.55
    BEARISH_EXTREME = -3       // Large breakout (>1× ATR or gap) + surge volume + consolidation
};

/**
 * DetectElderBreakout: Dr. Elder/Linda Raschke Keltner Channel breakout detection.
 *
 * Pattern: Close beyond Keltner band = volatility expansion
 * Bullish: Close > upperBand (upside breakout)
 * Bearish: Close < lowerBand (downside breakout)
 *
 * Strength based on:
 * - Distance beyond band (× ATR): Further = stronger
 * - Hurst: >0.55 = persistent, >0.65 = strong persistence
 * - Volume spike: ≥1.5× avg = participation, ≥2× = surge
 * - Consolidation: 5+ bars near band = compression before expansion
 * - Gap: Price gaps beyond band = very strong
 */
inline ElderBreakoutEnum DetectElderBreakout(
    float close, float upperBand, float lowerBand,
    float atr, float hurst,
    double volume, double avgVolume,
    int consolidationBars, bool isGap,
    float& breakoutDistance, float& hurstOut, float& volumeSpike,
    int& consolidationBarsOut, bool& isGapOut, float& qualityScore
)
{
    // Initialize outputs
    breakoutDistance = 0.0f;
    hurstOut = hurst;
    volumeSpike = 0.0f;
    consolidationBarsOut = consolidationBars;
    isGapOut = isGap;
    qualityScore = 0.0f;

    ElderBreakoutEnum breakoutEnum = ElderBreakoutEnum::NONE;

    // Input validation
    if (atr <= 0.0f || upperBand <= 0.0f || lowerBand <= 0.0f) {
        return ElderBreakoutEnum::NONE;
    }

    // Calculate volume spike
    if (avgVolume > 0.0) {
        volumeSpike = static_cast<float>(volume / avgVolume);
    } else {
        volumeSpike = 1.0f;  // Neutral if no average
    }

    // Check for bullish breakout (close above upper band)
    if (close > upperBand) {
        breakoutDistance = (close - upperBand) / atr;  // Distance in ATR units

        // Classify strength
        if (isGap && breakoutDistance > 1.0f && volumeSpike >= 2.0f && consolidationBars >= 5) {
            // EXTREME: Gap + >1× ATR + surge volume + consolidation
            breakoutEnum = ElderBreakoutEnum::BULLISH_EXTREME;
            qualityScore = 0.6f;  // Base quality for EXTREME (raised from 0.4)
        }
        else if (breakoutDistance > 0.5f && volumeSpike >= 1.5f && hurst > 0.55f) {
            // STRONG: Clear breakout + volume + persistent trend
            breakoutEnum = ElderBreakoutEnum::BULLISH_STRONG;
            qualityScore = 0.4f;  // Base quality for STRONG (raised from 0.25)
        }
        else if (breakoutDistance > 0.1f) {
            // WEAK: Marginal breakout
            breakoutEnum = ElderBreakoutEnum::BULLISH_WEAK;
            qualityScore = 0.2f;  // Base quality for WEAK (raised from 0.15)
        }
    }
    // Check for bearish breakout (close below lower band)
    else if (close < lowerBand) {
        breakoutDistance = (lowerBand - close) / atr;  // Distance in ATR units (positive)

        // Classify strength
        if (isGap && breakoutDistance > 1.0f && volumeSpike >= 2.0f && consolidationBars >= 5) {
            // EXTREME: Gap + >1× ATR + surge volume + consolidation
            breakoutEnum = ElderBreakoutEnum::BEARISH_EXTREME;
            qualityScore = 0.6f;  // Base quality for EXTREME (raised from 0.4)
        }
        else if (breakoutDistance > 0.5f && volumeSpike >= 1.5f && hurst > 0.55f) {
            // STRONG: Clear breakout + volume + persistent trend
            breakoutEnum = ElderBreakoutEnum::BEARISH_STRONG;
            qualityScore = 0.4f;  // Base quality for STRONG (raised from 0.25)
        }
        else if (breakoutDistance > 0.1f) {
            // WEAK: Marginal breakout
            breakoutEnum = ElderBreakoutEnum::BEARISH_WEAK;
            qualityScore = 0.2f;  // Base quality for WEAK (raised from 0.15)
        }
    }

    // If no pattern, return early
    if (breakoutEnum == ElderBreakoutEnum::NONE) {
        return ElderBreakoutEnum::NONE;
    }

    // Quality enhancements based on breakout characteristics

    // Bonus 1: Breakout Distance (further = better)
    if (breakoutDistance >= 1.5f) {
        qualityScore += 0.2f;  // Very far beyond band
    }
    else if (breakoutDistance >= 1.0f) {
        qualityScore += 0.15f;  // Far beyond band
    }
    else if (breakoutDistance >= 0.7f) {
        qualityScore += 0.1f;  // Moderate distance
    }
    else if (breakoutDistance >= 0.5f) {
        qualityScore += 0.05f;  // Slight distance
    }

    // Bonus 2: Hurst (trend persistence)
    if (hurst >= 0.70f) {
        qualityScore += 0.15f;  // Strong persistence (trending regime)
    }
    else if (hurst >= 0.65f) {
        qualityScore += 0.1f;  // Good persistence
    }
    else if (hurst >= 0.60f) {
        qualityScore += 0.05f;  // Emerging persistence
    }

    // Bonus 3: Volume spike
    if (volumeSpike >= 3.0f) {
        qualityScore += 0.15f;  // Massive volume
    }
    else if (volumeSpike >= 2.0f) {
        qualityScore += 0.1f;  // Surge volume
    }
    else if (volumeSpike >= 1.5f) {
        qualityScore += 0.05f;  // Elevated volume
    }

    // Bonus 4: Consolidation (compression before expansion)
    if (consolidationBars >= 10) {
        qualityScore += 0.15f;  // Extended consolidation
    }
    else if (consolidationBars >= 7) {
        qualityScore += 0.1f;  // Good consolidation
    }
    else if (consolidationBars >= 5) {
        qualityScore += 0.05f;  // Moderate consolidation
    }

    // Bonus 5: Gap (strongest signal)
    if (isGap) {
        qualityScore += 0.1f;
    }

    // Cap quality at 1.0
    qualityScore = std::min(1.0f, qualityScore);

    return breakoutEnum;
}

/**
 * NR7Enum: Narrow Range 7 - Compression pattern where bar range is smallest over past 7 bars.
 *
 * Named after Linda Raschke's compression breakout pattern.
 * Range = High - Low; NR7 = bar with smallest range over 7-bar lookback.
 *
 * Theory: "Volatility compression precedes expansion. When price consolidates in a tight range,
 *         the spring coils tighter. When it breaks, the expansion follows." — Raschke
 *
 * WEAK: Range 95-100% of 7-bar average (barely narrowest)
 * - Close to the 7-bar average, barely qualifies as compression
 * - High probability of failure or fizzle
 * - Base quality: 0.15
 *
 * STRONG: Range 85-95% of 7-bar average + volume declining (good compression signal)
 * - Solid compression with volume drying up
 * - Best when after multi-bar consolidation
 * - Base quality: 0.25
 *
 * EXTREME: Range <80% of 7-bar average + volume very low + consolidation (nuclear setup)
 * - Extremely tight range (compression extreme)
 * - Volume drying up completely (low participation)
 * - After 3+ bars at band (spring fully coiled)
 * - Base quality: 0.4
 *
 * Context Enhancement (Raschke's additions):
 * - Volume decline (+0.2): Volume drying up confirms compression
 * - Impulse aligned (+0.1): Impulse color will confirm breakout direction
 * - Screen1 aligned (+0.1): Weekly trend predicts breakout direction
 *
 * Quote: "NR7 is the compression pattern I use most. It's selective enough to filter
 *        whipsaws but common enough to trade regularly." — Linda Raschke
 */
enum class NR7Enum : int8_t
{
    NONE = 0,
    WEAK = 1,               // Range 95-100% of 7-bar average
    STRONG = 2,             // Range 85-95% + volume declining
    EXTREME = 3             // Range <80% + volume dry + consolidation
};

// ============================================================================
// TickCompanionValues (indicator-manager-dod-soa plan, Task 10)
// ============================================================================
// One per-tick snapshot of the companion values that both the live Event path
// (EventSerializer.cpp) and the training TrainingEvent path
// (IndicatorManager::GetTrainingEventT) need. Before this task, each path
// independently called GetIndicator<T>()->GetX() on the same underlying
// indicator objects to gather these same conceptual values -- two unrelated,
// independently-maintained blocks of code. That duplication is exactly how
// GetTrainingEventT's close_percentile dead-write bug (two disagreeing
// implementations, one of them recomputed from raw OHLC) was possible.
// IndicatorManager::GetTickCompanionValues() is the single gather site now;
// this struct only carries values already computed exactly once elsewhere --
// it does not recompute anything itself. Mirrors
// EventRootSharedSlice/TrainingRootSharedSlice's field list exactly (those two
// are confirmed identical to each other already; this struct is the shared
// READ-side counterpart of their shared WRITE-side contract).
struct TickCompanionValues {
    int8_t side = 0;
    int8_t marketSymbol = 0;
    int8_t overnightExit = 0;
    float nhNlDaily = 0.0f;
    float prevHigh = 0.0f;
    float prevLow = 0.0f;
    float prevDayHigh = 0.0f;
    float prevDayLow = 0.0f;
    float prevFourBarHigh = 0.0f;
    float prevFourBarLow = 0.0f;
    float closePercentile = 0.0f;
    float volumeRatioPercent = 0.0f;
    float volumeImbalance = 0.0f;
    float atr10 = 0.0f;
};
