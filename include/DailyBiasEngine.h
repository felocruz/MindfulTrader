// DailyBiasEngine.h — pure, header-only Daily Bias classification core
// (docs/ADR/sierra_chart_data_feed_setup.md "genuine low-hanging fruit").
//
// SCOPE: Deterministic Raschke 80%-Rule / gap-analysis classification only —
// no Sierra Chart types, no I/O — so it is natively unit-testable
// (tests/cpp/test_daily_bias_engine.cpp) without the ACSIL SDK. Live code
// (StudyHelperFunctions.cpp's CalculateDailyBias()) wraps ComputeDailyBias()
// and maps dbe::Bias -> DailyBiasEnum (Indicator.h) by identical underlying
// values, keeping this header free of engine headers for native testability
// (same pattern as TripleBarrierEngine.h).
//
// valueAreaLow/valueAreaHigh are the REAL, volume-weighted Value Area,
// computed by VolumeProfileEngine.h from Sierra Chart's own Volume-at-Price
// data (Task 5). Pass 0.0f for either (the sentinel "not available") to fall
// back to the naive 15%/85% range-split proxy this engine replaces.

#pragma once

namespace dbe {

enum class Bias : int {
    PHYSICS_VETO_RANDOM_WALK = 0,
    BULLISH_TREND_PERSISTENT = 1,
    BULLISH_MEAN_REVERSION = 2,
    PHYSICS_VETO_HIGH_ENTROPY = 3,
    VALUE_AREA_ROTATION = 4,
    BULLISH_VOLATILITY_TRAP = 5,
    BEARISH_TREND_PERSISTENT = -1,
    BEARISH_MEAN_REVERSION = -2,
    BEARISH_VOLATILITY_TRAP = -5,
};

struct DailyBiasInputs {
    float lastPrice = 0.0f;
    float prevDayHigh = 0.0f;
    float prevDayLow = 0.0f;
    float hurstExponent = 0.0f;
    float valueAreaLow = 0.0f;   // Real Volume Profile VAL; 0.0f = use proxy
    float valueAreaHigh = 0.0f;  // Real Volume Profile VAH; 0.0f = use proxy
};

inline Bias ComputeDailyBias(const DailyBiasInputs& in) {
    if (in.hurstExponent > 0.45f && in.hurstExponent < 0.55f) {
        return Bias::PHYSICS_VETO_RANDOM_WALK;
    }

    if (in.prevDayHigh <= 0.0f || in.prevDayLow <= 0.0f || in.prevDayHigh <= in.prevDayLow) {
        return Bias::PHYSICS_VETO_RANDOM_WALK;
    }

    const float range = in.prevDayHigh - in.prevDayLow;

    // Real Volume Profile Value Area takes precedence over the naive
    // range-split proxy. Require BOTH bounds to avoid mixing a real bound
    // with a mismatched proxy bound.
    const bool haveRealValueArea = in.valueAreaLow > 0.0f && in.valueAreaHigh > 0.0f;
    const float val = haveRealValueArea ? in.valueAreaLow  : in.prevDayLow + (range * 0.15f);
    const float vah = haveRealValueArea ? in.valueAreaHigh : in.prevDayLow + (range * 0.85f);

    if (in.lastPrice > in.prevDayHigh) {
        if (in.hurstExponent > 0.0f && in.hurstExponent < 0.4f) {
            return Bias::BEARISH_VOLATILITY_TRAP;
        }
        return Bias::BULLISH_TREND_PERSISTENT;
    }
    if (in.lastPrice < in.prevDayLow) {
        if (in.hurstExponent > 0.0f && in.hurstExponent < 0.4f) {
            return Bias::BULLISH_VOLATILITY_TRAP;
        }
        return Bias::BEARISH_TREND_PERSISTENT;
    }
    if (in.lastPrice > in.prevDayLow && in.lastPrice < val) {
        return Bias::BULLISH_MEAN_REVERSION;
    }
    if (in.lastPrice < in.prevDayHigh && in.lastPrice > vah) {
        return Bias::BEARISH_MEAN_REVERSION;
    }
    return Bias::VALUE_AREA_ROTATION;
}

}  // namespace dbe
