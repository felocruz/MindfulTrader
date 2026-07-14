#pragma once

#include <array>
#include <cstddef>

namespace mts::schema_contract {

struct IndicatorKeyRegistryRow {
    const char* indicator_key;
    unsigned int key_value;
};

inline constexpr std::array<IndicatorKeyRegistryRow, 41> kIndicatorKeyRegistryRows = {{
    {"LONG_MACD", 1},
    {"LONG_FI13_SIGNAL", 2},
    {"LONG_MACD_DIVERGENCE", 3},
    {"LONG_IMP", 4},
    {"LONG_MKT_ACTION", 5},
    {"INTERM_STOCHASTIC", 6},
    {"RASCHKE_STRATEGY_SETUP", 7},
    {"RASCHKE_TACTICAL_TRIGGER", 8},
    {"RSI", 9},
    {"INTERM_FI2_SIGNAL", 10},
    {"EMA_PROXIMITY", 11},
    {"PRICE_METRICS", 12},
    {"INTERM_MACD_DIVERGENCE", 13},
    {"INTERM_IMP", 14},
    {"INTERM_MACD", 15},
    {"STRUCTURE_TEST", 16},
    {"VOLUME_SIGNAL", 17},
    {"ATR_PROXIMITY", 18},
    {"DAILY_BIAS", 19},
    {"KANGAROO_TAIL", 21},
    {"TURTLE_SOUP", 22},
    {"MOMENTUM_PINBALL", 23},
    {"ELDER_BREAKOUT", 24},
    {"NR7", 25},
    {"SHORT_MKT_ACTION", 20},
    {"OSCILLATOR_310", 26},
    {"VWAP", 53},
    {"SIDE", 27},
    {"MARKET_SYMBOL", 28},
    {"TIME_OF_DAY", 29},
    {"OVERNIGHT_EXIT", 31},
    {"HURST_EXPONENT", 34},
    {"NH_NL_SIGNAL", 35},
    {"CORR_ES_ZN", 45},
    {"CORR_ES_DX", 46},
    {"ZN_TREND", 47},
    {"DX_TREND", 48},
    {"CORR_ES_ZN_DELTA", 49},
    {"CORR_ES_ZN_ACCEL", 50},
    {"CORR_ES_DX_DELTA", 51},
    {"CORR_ES_DX_ACCEL", 52},
}};

inline constexpr std::size_t kIndicatorKeyRegistryRowCount = kIndicatorKeyRegistryRows.size();

inline constexpr std::array<unsigned int, kIndicatorKeyRegistryRowCount> kIndicatorKeyRegistryValues = {{
    1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
    21, 22, 23, 24, 25, 20, 26, 53, 27, 28, 29, 31, 34, 35, 45, 46,
    47, 48, 49, 50, 51, 52,
}};

constexpr bool IndicatorRegistryHasUniqueValues() {
    for (std::size_t i = 0; i < kIndicatorKeyRegistryValues.size(); ++i) {
        for (std::size_t j = i + 1; j < kIndicatorKeyRegistryValues.size(); ++j) {
            if (kIndicatorKeyRegistryValues[i] == kIndicatorKeyRegistryValues[j]) {
                return false;
            }
        }
    }
    return true;
}

static_assert(IndicatorRegistryHasUniqueValues(),
              "Indicator key registry contains duplicate key values");

}  // namespace mts::schema_contract
