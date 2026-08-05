#pragma once

#include <cstdint>

// Dense index for every indicator tracked by IndicatorManager's IndicatorStore.
//
// Extracted verbatim from Indicator.h (2026-08-05, Task 2 of the
// indicator-manager-dod-soa plan: docs/superpowers/sdd/2026-08-05-indicator-manager-dod-soa/)
// so that headers needing only the enum -- IndicatorLayout.h today, future
// IndicatorPackedState.h -- can be included and unit-tested with a plain
// standalone compiler (no "sierrachart.h" / Windows SDK ACSIL dependency).
// Indicator.h includes this header for the enum; no behavior change.
enum class IndicatorKey : uint8_t {
    UNKNOWN = 0,
    LONG_MACD = 1,
    LONG_FI13_SIGNAL = 2,
    LONG_MACD_DIVERGENCE = 3,
    LONG_IMP = 4,
    LONG_MKT_ACTION = 5,
    INTERM_STOCHASTIC = 6,
    RASCHKE_STRATEGY_SETUP = 7,
    RASCHKE_TACTICAL_TRIGGER = 8,
    RSI = 9,
    INTERM_FI2_SIGNAL = 10,
    EMA_PROXIMITY = 11,
    PRICE_METRICS = 12,
    INTERM_MACD_DIVERGENCE = 13,
    INTERM_IMP = 14,
    INTERM_MACD = 15,
    STRUCTURE_TEST = 16,
    VOLUME_SIGNAL = 17,
    ATR_PROXIMITY = 18,
    DAILY_BIAS = 19,
    SHORT_MKT_ACTION = 20,
    KANGAROO_TAIL = 21,
    TURTLE_SOUP = 22,
    MOMENTUM_PINBALL = 23,
    ELDER_BREAKOUT = 24,
    NR7 = 25,
    OSCILLATOR_310 = 26,
    SIDE = 27,
    MARKET_SYMBOL = 28,
    TIME_OF_DAY = 29,
    // 30 reserved (SESSION_AGE removed)
    OVERNIGHT_EXIT = 31,
    HMM_STATE = 32,
    MARKET_CLIMATE = 33,
    HURST_EXPONENT = 34,
    NH_NL_SIGNAL = 35,
    PREV_HIGH_KEY = 36,
    PREV_LOW_KEY = 37,
    PREV_DAY_HIGH_KEY = 38,
    PREV_DAY_LOW_KEY = 39,
    PREV_FOUR_BAR_HIGH_KEY = 40,
    PREV_FOUR_BAR_LOW_KEY = 41,
    THREE_LINE_OSCILLATOR = 42,
    THREE_LINE_OSCILLATOR_PREV = 43,
    PREDICTION_STATE = 44,
    CORR_ES_ZN = 45,
    CORR_ES_DX = 46,
    ZN_TREND = 47,
    DX_TREND = 48,
    CORR_ES_ZN_DELTA = 49,
    CORR_ES_ZN_ACCEL = 50,
    CORR_ES_DX_DELTA = 51,
    CORR_ES_DX_ACCEL = 52,
    VWAP = 53,
    MAX_INDICATORS = 54
};
