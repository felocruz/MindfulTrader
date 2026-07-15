#pragma once

#include <array>
#include <cstddef>

namespace mts::schema_contract {

enum class WireClass {
    shared_wire,
    live_only_wire,
    training_only_wire,
    non_wire_internal,
};

enum class FieldSink {
    indicator_state,
    event_root,
    training_root,
    event_root_and_training_root,
    none,
};

struct IndicatorBindingPolicyRow {
    const char* indicator_key;
    WireClass wire_class;
    FieldSink sink;
    const char* field_type;
    bool has_live_writer;
    bool has_training_writer;
};

inline constexpr std::size_t kExpectedManagedIndicatorKeyCount = 41;
inline constexpr std::size_t kExpectedSharedWireCount = 37;
inline constexpr std::size_t kExpectedNonWireInternalCount = 4;

inline constexpr std::array<const char*, kExpectedManagedIndicatorKeyCount> kExpectedManagedIndicatorKeys = {
    "LONG_MACD",
    "LONG_FI13_SIGNAL",
    "LONG_MACD_DIVERGENCE",
    "LONG_IMP",
    "LONG_MKT_ACTION",
    "INTERM_STOCHASTIC",
    "RASCHKE_STRATEGY_SETUP",
    "RASCHKE_TACTICAL_TRIGGER",
    "RSI",
    "INTERM_FI2_SIGNAL",
    "EMA_PROXIMITY",
    "PRICE_METRICS",
    "INTERM_MACD_DIVERGENCE",
    "INTERM_IMP",
    "INTERM_MACD",
    "STRUCTURE_TEST",
    "VOLUME_SIGNAL",
    "ATR_PROXIMITY",
    "DAILY_BIAS",
    "KANGAROO_TAIL",
    "TURTLE_SOUP",
    "MOMENTUM_PINBALL",
    "ELDER_BREAKOUT",
    "NR7",
    "SHORT_MKT_ACTION",
    "OSCILLATOR_310",
    "VWAP",
    "SIDE",
    "MARKET_SYMBOL",
    "TIME_OF_DAY",
    "OVERNIGHT_EXIT",
    "HURST_EXPONENT",
    "NH_NL_SIGNAL",
    "CORR_ES_ZN",
    "CORR_ES_DX",
    "ZN_TREND",
    "DX_TREND",
    "CORR_ES_ZN_DELTA",
    "CORR_ES_ZN_ACCEL",
    "CORR_ES_DX_DELTA",
    "CORR_ES_DX_ACCEL",
};

inline constexpr std::array<IndicatorBindingPolicyRow, kExpectedManagedIndicatorKeyCount> kIndicatorBindingPolicyRows = {{
    {"LONG_MACD", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"LONG_FI13_SIGNAL", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"LONG_MACD_DIVERGENCE", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"LONG_IMP", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"LONG_MKT_ACTION", WireClass::non_wire_internal, FieldSink::none, "special", false, false},
    {"INTERM_STOCHASTIC", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"RASCHKE_STRATEGY_SETUP", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"RASCHKE_TACTICAL_TRIGGER", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"RSI", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"INTERM_FI2_SIGNAL", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"EMA_PROXIMITY", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"PRICE_METRICS", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"INTERM_MACD_DIVERGENCE", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"INTERM_IMP", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"INTERM_MACD", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"STRUCTURE_TEST", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"VOLUME_SIGNAL", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"ATR_PROXIMITY", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"DAILY_BIAS", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"KANGAROO_TAIL", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"TURTLE_SOUP", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"MOMENTUM_PINBALL", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"ELDER_BREAKOUT", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"NR7", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"SHORT_MKT_ACTION", WireClass::non_wire_internal, FieldSink::none, "special", false, false},
    {"OSCILLATOR_310", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"VWAP", WireClass::non_wire_internal, FieldSink::none, "special", false, false},
    {"SIDE", WireClass::shared_wire, FieldSink::event_root_and_training_root, "int8 enum", true, true},
    {"MARKET_SYMBOL", WireClass::shared_wire, FieldSink::event_root_and_training_root, "int8 enum", true, true},
    {"TIME_OF_DAY", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"OVERNIGHT_EXIT", WireClass::shared_wire, FieldSink::event_root_and_training_root, "int8 enum", true, true},
    {"HURST_EXPONENT", WireClass::non_wire_internal, FieldSink::none, "float", false, false},
    {"NH_NL_SIGNAL", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"CORR_ES_ZN", WireClass::shared_wire, FieldSink::indicator_state, "float", true, true},
    {"CORR_ES_DX", WireClass::shared_wire, FieldSink::indicator_state, "float", true, true},
    {"ZN_TREND", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"DX_TREND", WireClass::shared_wire, FieldSink::indicator_state, "int8 enum", true, true},
    {"CORR_ES_ZN_DELTA", WireClass::shared_wire, FieldSink::indicator_state, "float", true, true},
    {"CORR_ES_ZN_ACCEL", WireClass::shared_wire, FieldSink::indicator_state, "float", true, true},
    {"CORR_ES_DX_DELTA", WireClass::shared_wire, FieldSink::indicator_state, "float", true, true},
    {"CORR_ES_DX_ACCEL", WireClass::shared_wire, FieldSink::indicator_state, "float", true, true},
}};

inline constexpr std::size_t kIndicatorBindingPolicyRowCount = kIndicatorBindingPolicyRows.size();

constexpr bool CStrEq(const char* lhs, const char* rhs) {
    if (lhs == nullptr || rhs == nullptr) {
        return false;
    }
    while (*lhs != '\0' && *rhs != '\0') {
        if (*lhs != *rhs) {
            return false;
        }
        ++lhs;
        ++rhs;
    }
    return *lhs == *rhs;
}

constexpr bool HasKey(const char* key) {
    for (const auto& row : kIndicatorBindingPolicyRows) {
        if (CStrEq(row.indicator_key, key)) {
            return true;
        }
    }
    return false;
}

constexpr bool HasUniqueKeys() {
    for (std::size_t i = 0; i < kIndicatorBindingPolicyRows.size(); ++i) {
        for (std::size_t j = i + 1; j < kIndicatorBindingPolicyRows.size(); ++j) {
            if (CStrEq(kIndicatorBindingPolicyRows[i].indicator_key,
                       kIndicatorBindingPolicyRows[j].indicator_key)) {
                return false;
            }
        }
    }
    return true;
}

constexpr std::size_t CountWireClass(WireClass wire_class) {
    std::size_t count = 0;
    for (const auto& row : kIndicatorBindingPolicyRows) {
        if (row.wire_class == wire_class) {
            ++count;
        }
    }
    return count;
}

constexpr bool SharedRowsHaveDualWriters() {
    for (const auto& row : kIndicatorBindingPolicyRows) {
        if (row.wire_class == WireClass::shared_wire) {
            if (!row.has_live_writer || !row.has_training_writer) {
                return false;
            }
            if (row.sink == FieldSink::none) {
                return false;
            }
        }
    }
    return true;
}

constexpr bool NonWireRowsAreExplicitInternal() {
    for (const auto& row : kIndicatorBindingPolicyRows) {
        if (row.wire_class == WireClass::non_wire_internal) {
            if (row.sink != FieldSink::none || row.has_live_writer || row.has_training_writer) {
                return false;
            }
        }
    }
    return true;
}

constexpr bool HasAllExpectedKeys() {
    for (const auto* key : kExpectedManagedIndicatorKeys) {
        if (!HasKey(key)) {
            return false;
        }
    }
    return true;
}

inline constexpr char kIndicatorBindingPolicySchemaSha256[] = "152a692483ca13b3b85ff745ddcf2e1d8aa4bebeda40183138adb262acd51bc6";
inline constexpr char kIndicatorBindingPolicyGeneratedUtc[] = "2026-07-15T11:15:22Z";

static_assert(kIndicatorBindingPolicyRowCount == kExpectedManagedIndicatorKeyCount,
              "Indicator binding policy row count drifted from expected managed IndicatorKey count");
static_assert(HasUniqueKeys(),
              "Indicator binding policy contains duplicate IndicatorKey rows");
static_assert(HasAllExpectedKeys(),
              "Indicator binding policy missing one or more expected IndicatorKey rows");
static_assert(CountWireClass(WireClass::shared_wire) == kExpectedSharedWireCount,
              "Indicator binding policy shared_wire row count drifted");
static_assert(CountWireClass(WireClass::non_wire_internal) == kExpectedNonWireInternalCount,
              "Indicator binding policy non_wire_internal row count drifted");
static_assert(SharedRowsHaveDualWriters(),
              "Indicator binding policy shared_wire rows must declare both live and training writers");
static_assert(NonWireRowsAreExplicitInternal(),
              "Indicator binding policy non_wire_internal rows must map to sink=none and no writers");

}  // namespace mts::schema_contract
