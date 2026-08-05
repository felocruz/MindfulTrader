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
