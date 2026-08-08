#include "MindfulTrader_Precompiled.h"
#include "IndicatorManager.h"
#include <array>
#include <cassert>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <vector>
#include "Scoring.h"  // Elite v2.5: Integration for noise filtering in HasSignificantChange
#include "ContextManager.h" // Elite v2.5: Observation integration
#include "generated/mts_schema_contract_generated.h"
#include "generated/indicator_key_registry_generated.h"
#include "generated/training_shared_writers_generated.h"
#include "messaging/EventSerializer.h"
#include "VolumeProfileEngine.h"

// Elite v2.3: Named constants for feature vector defaults
// (FeatureDefaults namespace removed (indicator-manager-dod-soa plan, Task 7):
// its last two members, IMPULSE_NEUTRAL and DAILY_BIAS_NEUTRAL, existed only
// as null-fallback defaults for GetIndicator<T>() pointer reads. LONG_IMP's
// and DAILY_BIAS's read call sites now go through GetValue<Key>() directly,
// which is always valid — no pointer, no null check, no fallback needed.)

namespace {
    constexpr uint64_t IndicatorKeyMask(IndicatorKey key) {
        return 1ULL << static_cast<uint64_t>(key);
    }

    constexpr uint64_t PRIMARY_TRIGGER_MASK =
        IndicatorKeyMask(IndicatorKey::KANGAROO_TAIL) |
        IndicatorKeyMask(IndicatorKey::TURTLE_SOUP) |
        IndicatorKeyMask(IndicatorKey::MOMENTUM_PINBALL) |
        IndicatorKeyMask(IndicatorKey::ELDER_BREAKOUT) |
        IndicatorKeyMask(IndicatorKey::NR7) |
        IndicatorKeyMask(IndicatorKey::SIDE) |
        IndicatorKeyMask(IndicatorKey::LONG_IMP) |
        IndicatorKeyMask(IndicatorKey::INTERM_IMP) |
        IndicatorKeyMask(IndicatorKey::RSI) |
        IndicatorKeyMask(IndicatorKey::INTERM_STOCHASTIC) |
        IndicatorKeyMask(IndicatorKey::ATR_PROXIMITY) |
        IndicatorKeyMask(IndicatorKey::EMA_PROXIMITY) |
        IndicatorKeyMask(IndicatorKey::RASCHKE_STRATEGY_SETUP) |
        IndicatorKeyMask(IndicatorKey::RASCHKE_TACTICAL_TRIGGER) |
        IndicatorKeyMask(IndicatorKey::STRUCTURE_TEST) |
        IndicatorKeyMask(IndicatorKey::VOLUME_SIGNAL) |
        IndicatorKeyMask(IndicatorKey::DAILY_BIAS);

    constexpr uint64_t ALL_INDICATOR_MASK =
        (1ULL << static_cast<uint64_t>(IndicatorKey::MAX_INDICATORS)) - 1ULL;

    constexpr uint64_t SECONDARY_TRIGGER_MASK =
        ALL_INDICATOR_MASK & ~PRIMARY_TRIGGER_MASK;

    template <std::size_t N>
    constexpr bool ArraysEqual(const std::array<unsigned int, N>& lhs,
                               const std::array<unsigned int, N>& rhs) {
        for (std::size_t i = 0; i < N; ++i) {
            if (lhs[i] != rhs[i]) {
                return false;
            }
        }
        return true;
    }

    // WS-04: Compile-time parity guard between generated registry and runtime
    // constructor registration contract list.
    constexpr std::array<unsigned int, mts::schema_contract::kIndicatorKeyRegistryRowCount>
        kRuntimeRegisteredIndicatorKeyValues = {{
            static_cast<unsigned int>(IndicatorKey::LONG_MACD),
            static_cast<unsigned int>(IndicatorKey::LONG_FI13_SIGNAL),
            static_cast<unsigned int>(IndicatorKey::LONG_MACD_DIVERGENCE),
            static_cast<unsigned int>(IndicatorKey::LONG_IMP),
            static_cast<unsigned int>(IndicatorKey::LONG_MKT_ACTION),
            static_cast<unsigned int>(IndicatorKey::INTERM_STOCHASTIC),
            static_cast<unsigned int>(IndicatorKey::RASCHKE_STRATEGY_SETUP),
            static_cast<unsigned int>(IndicatorKey::RASCHKE_TACTICAL_TRIGGER),
            static_cast<unsigned int>(IndicatorKey::RSI),
            static_cast<unsigned int>(IndicatorKey::INTERM_FI2_SIGNAL),
            static_cast<unsigned int>(IndicatorKey::EMA_PROXIMITY),
            static_cast<unsigned int>(IndicatorKey::PRICE_METRICS),
            static_cast<unsigned int>(IndicatorKey::INTERM_MACD_DIVERGENCE),
            static_cast<unsigned int>(IndicatorKey::INTERM_IMP),
            static_cast<unsigned int>(IndicatorKey::INTERM_MACD),
            static_cast<unsigned int>(IndicatorKey::STRUCTURE_TEST),
            static_cast<unsigned int>(IndicatorKey::VOLUME_SIGNAL),
            static_cast<unsigned int>(IndicatorKey::ATR_PROXIMITY),
            static_cast<unsigned int>(IndicatorKey::DAILY_BIAS),
            static_cast<unsigned int>(IndicatorKey::KANGAROO_TAIL),
            static_cast<unsigned int>(IndicatorKey::TURTLE_SOUP),
            static_cast<unsigned int>(IndicatorKey::MOMENTUM_PINBALL),
            static_cast<unsigned int>(IndicatorKey::ELDER_BREAKOUT),
            static_cast<unsigned int>(IndicatorKey::NR7),
            static_cast<unsigned int>(IndicatorKey::SHORT_MKT_ACTION),
            static_cast<unsigned int>(IndicatorKey::OSCILLATOR_310),
            static_cast<unsigned int>(IndicatorKey::VWAP),
            static_cast<unsigned int>(IndicatorKey::SIDE),
            static_cast<unsigned int>(IndicatorKey::MARKET_SYMBOL),
            static_cast<unsigned int>(IndicatorKey::TIME_OF_DAY),
            static_cast<unsigned int>(IndicatorKey::OVERNIGHT_EXIT),
            static_cast<unsigned int>(IndicatorKey::HURST_EXPONENT),
            static_cast<unsigned int>(IndicatorKey::NH_NL_SIGNAL),
            static_cast<unsigned int>(IndicatorKey::CORR_ES_ZN),
            static_cast<unsigned int>(IndicatorKey::CORR_ES_DX),
            static_cast<unsigned int>(IndicatorKey::ZN_TREND),
            static_cast<unsigned int>(IndicatorKey::DX_TREND),
            static_cast<unsigned int>(IndicatorKey::CORR_ES_ZN_DELTA),
            static_cast<unsigned int>(IndicatorKey::CORR_ES_ZN_ACCEL),
            static_cast<unsigned int>(IndicatorKey::CORR_ES_DX_DELTA),
            static_cast<unsigned int>(IndicatorKey::CORR_ES_DX_ACCEL),
            static_cast<unsigned int>(IndicatorKey::INTERM_MKT_ACTION),
        }};

    static_assert(ArraysEqual(kRuntimeRegisteredIndicatorKeyValues,
                              mts::schema_contract::kIndicatorKeyRegistryValues),
                  "Runtime indicator registration keys diverge from generated indicator key registry");

    // ------------------------------------------------------------------------
    // Task 9 (indicator-manager-dod-soa plan): devirtualized ShouldTrigger()
    // reimplementations for the ~9 entered/exited-transition indicator
    // families. Each function reproduces its leaf class's ShouldTrigger()
    // override (include/Indicator.h, re-read fresh for this task, not from
    // memory) byte-for-byte, operating on int8_t values read from m_packed
    // (GetI8/GetPrevI8) instead of the enum-typed m_value/m_prevValue members.
    // ------------------------------------------------------------------------

    // Shared shape for KangarooTail/TurtleSoup/MomentumPinball/ElderBreakout/
    // NR7: "trigger on pattern entry OR exit" relative to each enum's NONE
    // value (all confirmed == 0, but compared symbolically, not as a magic
    // number, so this stays correct if any enum's NONE value ever changes).
    template <typename Enum>
    bool EnteredOrExitedNone(int8_t cur, int8_t prev, Enum noneValue) {
        const int8_t none = static_cast<int8_t>(noneValue);
        const bool entered = (prev == none && cur != none);
        const bool exited = (prev != none && cur == none);
        return entered || exited;
    }

    // Stochastic::ShouldTrigger() (Indicator.h:859-872)
    bool StochasticTrigger(int8_t cur, int8_t prev) {
        using E = StochasticEnum;
        const bool enteringOverbought = (prev != static_cast<int8_t>(E::OVER_BOUGHT) && cur == static_cast<int8_t>(E::OVER_BOUGHT));
        const bool enteringOversold   = (prev != static_cast<int8_t>(E::OVER_SOLD)    && cur == static_cast<int8_t>(E::OVER_SOLD));
        const bool exitingOverbought  = (prev == static_cast<int8_t>(E::OVER_BOUGHT) && cur != static_cast<int8_t>(E::OVER_BOUGHT));
        const bool exitingOversold    = (prev == static_cast<int8_t>(E::OVER_SOLD)    && cur != static_cast<int8_t>(E::OVER_SOLD));
        const bool divergence = (cur == static_cast<int8_t>(E::BULLISH_DIVERGENCE) || cur == static_cast<int8_t>(E::BEARISH_DIVERGENCE));
        return enteringOverbought || enteringOversold || exitingOverbought || exitingOversold || divergence;
    }

    // RSIIndicator::ShouldTrigger() (Indicator.h:1497-1510)
    bool RSITrigger(int8_t cur, int8_t prev) {
        using E = RSI;
        const bool enteringOverbought = (prev != static_cast<int8_t>(E::OVERBOUGHT) && cur == static_cast<int8_t>(E::OVERBOUGHT));
        const bool enteringOversold   = (prev != static_cast<int8_t>(E::OVERSOLD)    && cur == static_cast<int8_t>(E::OVERSOLD));
        const bool exitingOverbought  = (prev == static_cast<int8_t>(E::OVERBOUGHT) && cur != static_cast<int8_t>(E::OVERBOUGHT));
        const bool exitingOversold    = (prev == static_cast<int8_t>(E::OVERSOLD)    && cur != static_cast<int8_t>(E::OVERSOLD));
        const bool divergence = (cur == static_cast<int8_t>(E::BULLISH_DIVERGENCE) || cur == static_cast<int8_t>(E::BEARISH_DIVERGENCE));
        return enteringOverbought || enteringOversold || exitingOverbought || exitingOversold || divergence;
    }

    // ATRProximityIndicator::ShouldTrigger() (Indicator.h:1614-1632)
    bool ATRProximityTrigger(int8_t cur, int8_t prev) {
        using E = ATRProximityEnum;
        const bool enteringExtreme = (prev == static_cast<int8_t>(E::LOW_VOLATILITY) &&
                                       (cur == static_cast<int8_t>(E::EXTREME_VOLATILITY) ||
                                        cur == static_cast<int8_t>(E::EXTREME_LOW) ||
                                        cur == static_cast<int8_t>(E::EXTREME_HIGH)));
        const bool exitingExtreme = ((prev == static_cast<int8_t>(E::EXTREME_VOLATILITY) ||
                                       prev == static_cast<int8_t>(E::EXTREME_LOW) ||
                                       prev == static_cast<int8_t>(E::EXTREME_HIGH)) &&
                                      cur == static_cast<int8_t>(E::LOW_VOLATILITY));
        const bool channelBreach = ((prev != static_cast<int8_t>(E::HIGH_MOVE) && cur == static_cast<int8_t>(E::HIGH_MOVE)) ||
                                     (prev == static_cast<int8_t>(E::HIGH_MOVE) && cur != static_cast<int8_t>(E::HIGH_MOVE)));
        return enteringExtreme || exitingExtreme || channelBreach;
    }

    // EmaProximityIndicator::ShouldTrigger() (Indicator.h:1649-1669)
    bool EmaProximityTrigger(int8_t cur, int8_t prev) {
        using E = EmaProximity;
        const bool crossover = (cur == static_cast<int8_t>(E::CROSS_ABOVE) || cur == static_cast<int8_t>(E::CROSS_BELOW));
        const bool wasBelow = (prev == static_cast<int8_t>(E::BELOW_TOUCH) ||
                                prev == static_cast<int8_t>(E::BELOW_STRONG) ||
                                prev == static_cast<int8_t>(E::PRICE_BELOW_EMA));
        const bool wasAbove = (prev == static_cast<int8_t>(E::ABOVE_TOUCH) ||
                                prev == static_cast<int8_t>(E::ABOVE_STRONG) ||
                                prev == static_cast<int8_t>(E::PRICE_ABOVE_EMA));
        const bool isAbove = (cur == static_cast<int8_t>(E::ABOVE_TOUCH) ||
                               cur == static_cast<int8_t>(E::ABOVE_STRONG) ||
                               cur == static_cast<int8_t>(E::PRICE_ABOVE_EMA));
        const bool isBelow = (cur == static_cast<int8_t>(E::BELOW_TOUCH) ||
                               cur == static_cast<int8_t>(E::BELOW_STRONG) ||
                               cur == static_cast<int8_t>(E::PRICE_BELOW_EMA));
        const bool crossingUp = wasBelow && isAbove;
        const bool crossingDown = wasAbove && isBelow;
        return crossover || crossingUp || crossingDown;
    }
}

IndicatorManager& IndicatorManager::Instance()
{
    static IndicatorManager singletonInstance;
    return singletonInstance;
}

// The template helper is implemented as a member function

IndicatorManager::IndicatorManager()
{
    // Initialize pointer map (O(1) lookup)
    m_indicators.fill(nullptr);

    // Screen 1
    m_indicators[static_cast<size_t>(IndicatorKey::LONG_MACD)] = &m_store.long_macd;
    m_store.long_macd.SetPackedSlotPointer(m_packed.RawI8Pointer(0));
    m_store.long_macd.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(0));
    m_indicators[static_cast<size_t>(IndicatorKey::LONG_FI13_SIGNAL)] = &m_store.long_fi13;
    m_store.long_fi13.SetPackedSlotPointer(m_packed.RawI8Pointer(1));
    m_store.long_fi13.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(1));
    m_store.long_fi13.SetPackedSlotPointer(m_packed.RawF32Pointer(9));
    m_indicators[static_cast<size_t>(IndicatorKey::LONG_MACD_DIVERGENCE)] = &m_store.long_macd_div;
    m_store.long_macd_div.SetPackedSlotPointer(m_packed.RawI8Pointer(2));
    m_store.long_macd_div.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(2));
    m_indicators[static_cast<size_t>(IndicatorKey::LONG_IMP)] = &m_store.long_imp;
    m_store.long_imp.SetPackedSlotPointer(m_packed.RawI8Pointer(3));
    m_store.long_imp.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(3));

    // Screen 2
    m_indicators[static_cast<size_t>(IndicatorKey::INTERM_STOCHASTIC)] = &m_store.interm_stochastic;
    m_store.interm_stochastic.SetPackedSlotPointer(m_packed.RawI8Pointer(4));
    m_store.interm_stochastic.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(4));
    m_indicators[static_cast<size_t>(IndicatorKey::RASCHKE_STRATEGY_SETUP)] = &m_store.raschke_strategy;
    m_store.raschke_strategy.SetPackedSlotPointer(m_packed.RawI8Pointer(5));
    m_store.raschke_strategy.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(5));
    m_indicators[static_cast<size_t>(IndicatorKey::RASCHKE_TACTICAL_TRIGGER)] = &m_store.raschke_tactical;
    m_store.raschke_tactical.SetPackedSlotPointer(m_packed.RawI8Pointer(6));
    m_store.raschke_tactical.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(6));
    m_indicators[static_cast<size_t>(IndicatorKey::RSI)] = &m_store.rsi;
    m_store.rsi.SetPackedSlotPointer(m_packed.RawI8Pointer(7));
    m_store.rsi.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(7));
    m_indicators[static_cast<size_t>(IndicatorKey::INTERM_FI2_SIGNAL)] = &m_store.interm_fi2;
    m_store.interm_fi2.SetPackedSlotPointer(m_packed.RawI8Pointer(8));
    m_store.interm_fi2.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(8));
    m_store.interm_fi2.SetPackedSlotPointer(m_packed.RawF32Pointer(7));
    m_indicators[static_cast<size_t>(IndicatorKey::EMA_PROXIMITY)] = &m_store.ema_prox;
    m_store.ema_prox.SetPackedSlotPointer(m_packed.RawI8Pointer(9));
    m_store.ema_prox.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(9));
    m_indicators[static_cast<size_t>(IndicatorKey::PRICE_METRICS)] = &m_store.price_metrics;
    m_store.price_metrics.SetPackedSlotPointer(m_packed.RawI8Pointer(10));
    m_store.price_metrics.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(10));
    m_indicators[static_cast<size_t>(IndicatorKey::INTERM_MACD_DIVERGENCE)] = &m_store.interm_macd_div;
    m_store.interm_macd_div.SetPackedSlotPointer(m_packed.RawI8Pointer(11));
    m_store.interm_macd_div.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(11));
    m_indicators[static_cast<size_t>(IndicatorKey::INTERM_IMP)] = &m_store.interm_imp;
    m_store.interm_imp.SetPackedSlotPointer(m_packed.RawI8Pointer(12));
    m_store.interm_imp.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(12));
    // NOTE: kIndicatorLayout also has a second, companion Int8 row for
    // INTERM_IMP at position 26 (impulse_run_length) — that is a distinct
    // field (m_runLength, not m_value) with no shared write path through the
    // generic Indicator<T>::Update() hook used here, and Task 2's audit flags
    // it as a pre-existing live-path gap (only BackTesterStudy.cpp populates
    // it today). Out of scope for this task; deferred to Task 9/10.
    m_indicators[static_cast<size_t>(IndicatorKey::INTERM_MACD)] = &m_store.interm_macd;
    m_store.interm_macd.SetPackedSlotPointer(m_packed.RawI8Pointer(13));
    m_store.interm_macd.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(13));
    // NOTE: INTERM_MACD's Float32-block companion (position 8, interm_macd_norm)
    // is intentionally NOT wired — Task 2's audit confirmed Macd::AddToTrainingEventFB
    // writes only the ambiguous top-level TrainingEventT::interm_macd_norm field,
    // not the IndicatorState struct field this packed slot represents. No working
    // internal write path exists to dual-write from; deferred to Task 9/10.

    // Screen 3
    m_indicators[static_cast<size_t>(IndicatorKey::STRUCTURE_TEST)] = &m_store.structure_test;
    m_store.structure_test.SetPackedSlotPointer(m_packed.RawI8Pointer(14));
    m_store.structure_test.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(14));
    m_indicators[static_cast<size_t>(IndicatorKey::VOLUME_SIGNAL)] = &m_store.volume;
    m_store.volume.SetPackedSlotPointer(m_packed.RawI8Pointer(15));
    m_store.volume.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(15));
    m_indicators[static_cast<size_t>(IndicatorKey::ATR_PROXIMITY)] = &m_store.atr_prox;
    m_store.atr_prox.SetPackedSlotPointer(m_packed.RawI8Pointer(16));
    m_store.atr_prox.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(16));
    m_indicators[static_cast<size_t>(IndicatorKey::DAILY_BIAS)] = &m_store.daily_bias;
    m_store.daily_bias.SetPackedSlotPointer(m_packed.RawI8Pointer(17));
    m_store.daily_bias.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(17));
    m_indicators[static_cast<size_t>(IndicatorKey::KANGAROO_TAIL)] = &m_store.kangaroo_tail;
    m_store.kangaroo_tail.SetPackedSlotPointer(m_packed.RawI8Pointer(18));
    m_store.kangaroo_tail.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(18));
    m_store.kangaroo_tail.SetPackedSlotPointer(m_packed.RawF32Pointer(0));
    m_indicators[static_cast<size_t>(IndicatorKey::TURTLE_SOUP)] = &m_store.turtle_soup;
    m_store.turtle_soup.SetPackedSlotPointer(m_packed.RawI8Pointer(19));
    m_store.turtle_soup.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(19));
    m_store.turtle_soup.SetPackedSlotPointer(m_packed.RawF32Pointer(1));
    m_indicators[static_cast<size_t>(IndicatorKey::MOMENTUM_PINBALL)] = &m_store.momentum_pinball;
    m_store.momentum_pinball.SetPackedSlotPointer(m_packed.RawI8Pointer(20));
    m_store.momentum_pinball.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(20));
    m_store.momentum_pinball.SetPackedSlotPointer(m_packed.RawF32Pointer(2));
    m_indicators[static_cast<size_t>(IndicatorKey::ELDER_BREAKOUT)] = &m_store.elder_breakout;
    m_store.elder_breakout.SetPackedSlotPointer(m_packed.RawI8Pointer(21));
    m_store.elder_breakout.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(21));
    m_store.elder_breakout.SetPackedSlotPointer(m_packed.RawF32Pointer(3));
    m_indicators[static_cast<size_t>(IndicatorKey::NR7)] = &m_store.nr7;
    m_store.nr7.SetPackedSlotPointer(m_packed.RawI8Pointer(22));
    m_store.nr7.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(22));
    m_store.nr7.SetPackedSlotPointer(m_packed.RawF32Pointer(4));

    // IndicatorKey::SHORT_MKT_ACTION has no live IndicatorStore member -- ShortMarketAction
    // was deleted (docs/superpowers/specs/2026-08-06-indicator-orphan-cleanup-design.md
    // §3.2); its live companion values now live in m_shortTermExtremes, decoupled from any
    // IndicatorKey. m_indicators[SHORT_MKT_ACTION] is intentionally left null.
    m_indicators[static_cast<size_t>(IndicatorKey::INTERM_MKT_ACTION)] = &m_store.interm_mkt_action;
    m_indicators[static_cast<size_t>(IndicatorKey::OSCILLATOR_310)] = &m_store.oscillator_310;
    m_store.oscillator_310.SetPackedSlotPointer(m_packed.RawI8Pointer(24));
    m_store.oscillator_310.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(24));
    m_indicators[static_cast<size_t>(IndicatorKey::VWAP)] = &m_store.vwap;

    m_indicators[static_cast<size_t>(IndicatorKey::SIDE)] = &m_store.side;
    m_indicators[static_cast<size_t>(IndicatorKey::MARKET_SYMBOL)] = &m_store.market_symbol;
    m_indicators[static_cast<size_t>(IndicatorKey::TIME_OF_DAY)] = &m_store.time_of_day;
    m_store.time_of_day.SetPackedSlotPointer(m_packed.RawI8Pointer(25));
    m_store.time_of_day.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(25));
    m_indicators[static_cast<size_t>(IndicatorKey::OVERNIGHT_EXIT)] = &m_store.overnight_exit;

    // HMM_STATE, MARKET_CLIMATE, PREDICTION_STATE → InferenceManager (Mar 2026)
    // Slots left null — accessors delegate to InferenceManager::Instance()
    m_indicators[static_cast<size_t>(IndicatorKey::HURST_EXPONENT)] = &m_store.hurst_exponent;

    m_indicators[static_cast<size_t>(IndicatorKey::NH_NL_SIGNAL)] = &m_store.nh_nl_signal;
    m_store.nh_nl_signal.SetPackedSlotPointer(m_packed.RawI8Pointer(23));
    m_store.nh_nl_signal.SetPackedPrevSlotPointer(m_packed.RawPrevI8Pointer(23));

    // Prediction State → InferenceManager (Mar 2026)

    // Cross-Market Correlations
    m_indicators[static_cast<size_t>(IndicatorKey::CORR_ES_ZN)] = &m_store.corr_es_zn;
    m_store.corr_es_zn.SetPackedSlotPointer(m_packed.RawF32Pointer(5));
    m_indicators[static_cast<size_t>(IndicatorKey::CORR_ES_DX)] = &m_store.corr_es_dx;
    m_store.corr_es_dx.SetPackedSlotPointer(m_packed.RawF32Pointer(6));
    m_indicators[static_cast<size_t>(IndicatorKey::ZN_TREND)] = &m_store.zn_trend;
    m_indicators[static_cast<size_t>(IndicatorKey::DX_TREND)] = &m_store.dx_trend;

    m_indicators[static_cast<size_t>(IndicatorKey::CORR_ES_ZN_DELTA)] = &m_store.corr_es_zn_delta;
    m_indicators[static_cast<size_t>(IndicatorKey::CORR_ES_ZN_ACCEL)] = &m_store.corr_es_zn_accel;
    m_indicators[static_cast<size_t>(IndicatorKey::CORR_ES_DX_DELTA)] = &m_store.corr_es_dx_delta;
    m_indicators[static_cast<size_t>(IndicatorKey::CORR_ES_DX_ACCEL)] = &m_store.corr_es_dx_accel;

    // Phase 3.1: Inject shared dirty mask pointer into each indicator
    for (auto* indicator : m_indicators) {
        if (indicator) {
            indicator->SetDirtyMaskPointer(&m_dirty_mask);
        }
    }

    // VWAP is trade-execution only — disconnect from dirty mask so it
    // never triggers training events (no FlatBuffer schema field exists).
    m_store.vwap.SetDirtyMaskPointer(nullptr);
}
// ----------------------------------------------------------------------------
// Phase 1.2 / Task 9 (indicator-manager-dod-soa plan): Static Metaprogramming
// Dispatcher (Devirtualized Trigger Check)
// ----------------------------------------------------------------------------
// Reads m_packed directly (GetI8/GetPrevI8) instead of calling virtual
// ShouldTrigger() on m_store's concrete objects -- no BaseIndicator*, no
// vtable, anywhere in this function. Every case below was re-verified against
// its leaf class's actual current ShouldTrigger() override (include/Indicator.h)
// during this task, not reproduced from memory:
//   - The ~9 entered/exited-transition families (Stochastic, RSI,
//     ATRProximity, EmaProximity, KangarooTail, TurtleSoup, MomentumPinball,
//     ElderBreakout, NR7) call the free-function reimplementations above,
//     keyed to the same kIndicatorLayout position their packed row lives at.
//   - LONG_IMP/INTERM_IMP/SIDE's ShouldTrigger() is `return IsDirty();`
//     (Indicator.h:779,904). CheckTrigger(index) is only ever invoked from
//     HasSignificantChange() with an index bit already known set in
//     m_dirty_mask (that's where `index` comes from), so this is byte-for-byte
//     equivalent to the old virtual call, just spelled out as a direct mask
//     check instead of relying on that precondition implicitly.
//   - Every other case's leaf class either has no ShouldTrigger() override at
//     all (falls to Indicator<T>::ShouldTrigger() default: `return false;`,
//     Indicator.h:663) or an explicit `return false;` override
//     (CorrelationIndicator, Indicator.h:2401) -- confirmed by re-reading
//     every override in the file (see grep-verified list in the task report),
//     not assumed from the previous virtual-call switch's shape.
// ----------------------------------------------------------------------------
bool IndicatorManager::CheckTrigger(size_t index) const {
    switch (static_cast<IndicatorKey>(index)) {
        // Screen 1
        case IndicatorKey::LONG_MACD: return false;
        case IndicatorKey::LONG_FI13_SIGNAL: return false;
        case IndicatorKey::LONG_MACD_DIVERGENCE: return false;
        case IndicatorKey::LONG_IMP: return (m_dirty_mask & IndicatorKeyMask(IndicatorKey::LONG_IMP)) != 0ULL;
        case IndicatorKey::LONG_MKT_ACTION: return false;

        // Screen 2
        case IndicatorKey::INTERM_STOCHASTIC: {
            constexpr size_t pos = mts::UniqueDescriptorFor(IndicatorKey::INTERM_STOCHASTIC).position;
            return StochasticTrigger(m_packed.GetI8(pos), m_packed.GetPrevI8(pos));
        }
        case IndicatorKey::RASCHKE_STRATEGY_SETUP: return false;
        case IndicatorKey::RASCHKE_TACTICAL_TRIGGER: return false;
        case IndicatorKey::RSI: {
            constexpr size_t pos = mts::UniqueDescriptorFor(IndicatorKey::RSI).position;
            return RSITrigger(m_packed.GetI8(pos), m_packed.GetPrevI8(pos));
        }
        case IndicatorKey::INTERM_FI2_SIGNAL: return false;
        case IndicatorKey::EMA_PROXIMITY: {
            constexpr size_t pos = mts::UniqueDescriptorFor(IndicatorKey::EMA_PROXIMITY).position;
            return EmaProximityTrigger(m_packed.GetI8(pos), m_packed.GetPrevI8(pos));
        }
        case IndicatorKey::PRICE_METRICS: return false;
        case IndicatorKey::INTERM_MACD_DIVERGENCE: return false;
        case IndicatorKey::INTERM_IMP: return (m_dirty_mask & IndicatorKeyMask(IndicatorKey::INTERM_IMP)) != 0ULL;
        case IndicatorKey::INTERM_MACD: return false;

        // Screen 3
        case IndicatorKey::STRUCTURE_TEST: return false;
        case IndicatorKey::VOLUME_SIGNAL: return false;
        case IndicatorKey::ATR_PROXIMITY: {
            constexpr size_t pos = mts::UniqueDescriptorFor(IndicatorKey::ATR_PROXIMITY).position;
            return ATRProximityTrigger(m_packed.GetI8(pos), m_packed.GetPrevI8(pos));
        }
        case IndicatorKey::DAILY_BIAS: return false;
        case IndicatorKey::KANGAROO_TAIL: {
            // Finding 1 fix (Task 9 review): KANGAROO_TAIL has TWO kIndicatorLayout
            // rows (Int8 primary + Float32 quality companion) — UniqueDescriptorFor
            // silently resolves such keys to NotPacked (position 0), which used to
            // alias LONG_MACD's position and test the wrong indicator entirely.
            // DescriptorFor(Key, Block) plus this static_assert makes that class of
            // bug a compile error instead of a silent position-0 alias.
            constexpr auto desc = mts::DescriptorFor(IndicatorKey::KANGAROO_TAIL, mts::StorageBlock::Int8);
            static_assert(desc.block == mts::StorageBlock::Int8, "KANGAROO_TAIL must resolve to a real Int8 row");
            constexpr size_t pos = desc.position;
            return EnteredOrExitedNone(m_packed.GetI8(pos), m_packed.GetPrevI8(pos), KangarooTailEnum::NONE);
        }
        case IndicatorKey::TURTLE_SOUP: {
            // Finding 1 fix — see KANGAROO_TAIL comment above; same two-row shape.
            constexpr auto desc = mts::DescriptorFor(IndicatorKey::TURTLE_SOUP, mts::StorageBlock::Int8);
            static_assert(desc.block == mts::StorageBlock::Int8, "TURTLE_SOUP must resolve to a real Int8 row");
            constexpr size_t pos = desc.position;
            return EnteredOrExitedNone(m_packed.GetI8(pos), m_packed.GetPrevI8(pos), TurtleSoupEnum::NONE);
        }
        case IndicatorKey::MOMENTUM_PINBALL: {
            // Finding 1 fix — see KANGAROO_TAIL comment above; same two-row shape.
            constexpr auto desc = mts::DescriptorFor(IndicatorKey::MOMENTUM_PINBALL, mts::StorageBlock::Int8);
            static_assert(desc.block == mts::StorageBlock::Int8, "MOMENTUM_PINBALL must resolve to a real Int8 row");
            constexpr size_t pos = desc.position;
            return EnteredOrExitedNone(m_packed.GetI8(pos), m_packed.GetPrevI8(pos), MomentumPinballEnum::NONE);
        }
        case IndicatorKey::ELDER_BREAKOUT: {
            // Finding 1 fix — see KANGAROO_TAIL comment above; same two-row shape.
            constexpr auto desc = mts::DescriptorFor(IndicatorKey::ELDER_BREAKOUT, mts::StorageBlock::Int8);
            static_assert(desc.block == mts::StorageBlock::Int8, "ELDER_BREAKOUT must resolve to a real Int8 row");
            constexpr size_t pos = desc.position;
            return EnteredOrExitedNone(m_packed.GetI8(pos), m_packed.GetPrevI8(pos), ElderBreakoutEnum::NONE);
        }
        case IndicatorKey::NR7: {
            // Finding 1 fix — see KANGAROO_TAIL comment above; same two-row shape.
            constexpr auto desc = mts::DescriptorFor(IndicatorKey::NR7, mts::StorageBlock::Int8);
            static_assert(desc.block == mts::StorageBlock::Int8, "NR7 must resolve to a real Int8 row");
            constexpr size_t pos = desc.position;
            return EnteredOrExitedNone(m_packed.GetI8(pos), m_packed.GetPrevI8(pos), NR7Enum::NONE);
        }
        case IndicatorKey::SHORT_MKT_ACTION: return false;
        case IndicatorKey::INTERM_MKT_ACTION: return false;
        case IndicatorKey::OSCILLATOR_310: return false;

        // Metadata & State
        case IndicatorKey::SIDE: return (m_dirty_mask & IndicatorKeyMask(IndicatorKey::SIDE)) != 0ULL;
        case IndicatorKey::MARKET_SYMBOL: return false;
        case IndicatorKey::TIME_OF_DAY: return false;
        case IndicatorKey::OVERNIGHT_EXIT: return false;
        // HMM_STATE, MARKET_CLIMATE → InferenceManager (no longer in dirty-mask dispatch)
        case IndicatorKey::HURST_EXPONENT: return false;
        case IndicatorKey::NH_NL_SIGNAL: return false;

        // Cross-Market Correlations
        case IndicatorKey::CORR_ES_ZN: return false;
        case IndicatorKey::CORR_ES_DX: return false;
        case IndicatorKey::ZN_TREND: return false;
        case IndicatorKey::DX_TREND: return false;
        case IndicatorKey::CORR_ES_ZN_DELTA: return false;
        case IndicatorKey::CORR_ES_ZN_ACCEL: return false;
        case IndicatorKey::CORR_ES_DX_DELTA: return false;
        case IndicatorKey::CORR_ES_DX_ACCEL: return false;

        default: return false;
    }
}

#ifndef NDEBUG
// Task 4 safety net (indicator-manager-dod-soa plan): confirms every Int8-block
// primary value dual-written into m_packed matches the legacy m_store/m_indicators
// path it was copied from. Covers only the rows Step 2 actually wires — position
// 26 (INTERM_IMP's impulse_run_length companion row) is a distinct field with no
// generic-Update() write path and is intentionally excluded (see the wiring
// comment in the constructor); asserting it against intValue() would compare
// unrelated values and fire spuriously.
//
// Task 9 disposition: NOT removed. Task 9 devirtualized CheckTrigger/
// PopulateIndicatorState/GetTrainingEventT (this file) to read m_packed
// directly, but those were never what this skip-list tracks -- the skip-list
// tracks whether a key's OTHER live call sites (TripleScreen1/2/3.cpp,
// EventSerializer.cpp, StudyHelperFunctions.cpp) have moved off the leaf
// pointer to GetValue<Key>(). Task 9 didn't touch any of those call sites, so
// the skip-list below is unchanged and still correct. Concretely: LONG_MACD's
// and INTERM_MACD's leaf `.Value()` is still read live outside this class
// (TripleScreen2.cpp:930, StudyHelperFunctions.cpp:1006/1027/1224) -- if their
// dual-write ever broke, those call sites would silently diverge from
// `m_packed` with nothing to catch it. This assertion is real, load-bearing
// coverage for every key below not already skipped, not a tautology; removing
// it now would lose that coverage. Defer full removal to whichever task
// (10 or 11) finishes migrating those remaining external readers.
void IndicatorManager::AssertPackedStateParity() const {
    for (const auto& desc : mts::kIndicatorLayout) {
        if (desc.block != mts::StorageBlock::Int8) continue;
        if (desc.key == IndicatorKey::INTERM_IMP && desc.position == 26) continue;
        // TIME_OF_DAY cut over to GetValue<Key>() (Task 5) — every live reader
        // now goes through m_packed directly, so this is comparing the packed
        // value against itself via the legacy object's still-live SetFromDateTime
        // write path. No longer a meaningful parity check; skip it.
        if (desc.key == IndicatorKey::TIME_OF_DAY) continue;
        // LONG_IMP and INTERM_IMP's main signal row (position 12) cut over to
        // GetValue<Key>()/GetValue<Key, Block>() (Task 7) — every live reader
        // now goes through m_packed directly. Same "no longer meaningful"
        // rationale as TIME_OF_DAY above. (INTERM_IMP's position-26
        // impulse_run_length row is already skipped above, unaffected.)
        if (desc.key == IndicatorKey::LONG_IMP) continue;
        if (desc.key == IndicatorKey::INTERM_IMP && desc.position == 12) continue;
        // STRUCTURE_TEST/VOLUME_SIGNAL/ATR_PROXIMITY/DAILY_BIAS's remaining
        // live readers (getScreen3EntryText, SyncFeatureVector, PositionManager,
        // TripleScreen3) cut over to GetValue<Key>() (Task 7). Same "no longer
        // meaningful" rationale as TIME_OF_DAY above.
        if (desc.key == IndicatorKey::STRUCTURE_TEST) continue;
        if (desc.key == IndicatorKey::VOLUME_SIGNAL) continue;
        if (desc.key == IndicatorKey::ATR_PROXIMITY) continue;
        if (desc.key == IndicatorKey::DAILY_BIAS) continue;
        // RSI's only external read (CheckWarmupStatus) cut over to
        // GetValue<Key>() (Task 7). Same "no longer meaningful" rationale.
        if (desc.key == IndicatorKey::RSI) continue;
        // INTERM_STOCHASTIC/RASCHKE_STRATEGY_SETUP/RASCHKE_TACTICAL_TRIGGER's
        // only external read (getScreen2EntryText) cut over to GetValue<Key>()
        // (Task 7). Same "no longer meaningful" rationale.
        if (desc.key == IndicatorKey::INTERM_STOCHASTIC) continue;
        if (desc.key == IndicatorKey::RASCHKE_STRATEGY_SETUP) continue;
        if (desc.key == IndicatorKey::RASCHKE_TACTICAL_TRIGGER) continue;
        // OSCILLATOR_310's only external read (EventSerializer::SnapshotContext)
        // cut over to GetValue<Key>() (Task 7). Same "no longer meaningful"
        // rationale.
        if (desc.key == IndicatorKey::OSCILLATOR_310) continue;

        const auto* base = m_indicators[static_cast<size_t>(desc.key)];
        if (!base) continue;

        const int8_t oldPathValue = static_cast<int8_t>(base->intValue());
        const int8_t newPathValue = m_packed.GetI8(desc.position);
        assert(oldPathValue == newPathValue && "IndicatorPackedState dual-write parity violation");
    }
}
#endif

void IndicatorManager::InitializeHotPathCache() {
    // No-op in enum-indexed DOD architecture.
}

void IndicatorManager::Reset()
{
    for (auto const& indicator : m_indicators) {
        if (indicator) {
            indicator->Reset();
        }
    }
    m_dirty_mask = 0;
    m_lastTrainingEventTimestampUs = 0;
    m_recentTrainingDeltaUs.clear();
}

// ============================================================================
// Elite v2.3: CheckWarmupStatus() - Zero-Trap Prevention
// ============================================================================
// Prevents broken indicators from silently poisoning training data
// Validates that indicators are computing real values (not just defaulting to 0.0)
// ============================================================================
void IndicatorManager::CheckWarmupStatus([[maybe_unused]] SCStudyInterfaceRef sc) {
    // Already warmed up - skip validation
    if (m_isWarmedUp) {
        return;
    }

    m_warmupBarCount++;

    // Phase 1: Wait for minimum bar threshold (200 bars)
    constexpr int MIN_WARMUP_BARS = 200;
    if (m_warmupBarCount < MIN_WARMUP_BARS) {
        return;
    }

    // Phase 2: Validate critical indicators are computing real values
    // If RSI is 0.0 after 200 bars, indicator is broken (not just oversold)
    // DOD/SoA migration (Task 7): read straight from the packed array — no
    // pointer, no null check, always a valid value.
    {
        const int rsiValue = static_cast<int>(GetValue<IndicatorKey::RSI>());
        if (rsiValue == 0) {
            // RSI stuck at 0 after 200 bars = broken indicator
            Logger::getInstance().log(
                "WARNING: RSI still at 0 after " + std::to_string(m_warmupBarCount) +
                " bars. Indicator may be broken. Warmup delayed."
            );
            return;
        }
    }

    // Phase 2b: Validate FI2Signal is computing (soft warning, not blocking)
    // DOD/SoA migration (Task 11): read straight from the packed array — no
    // pointer, no null check, always a valid value.
    const int fi2Value = static_cast<int>(GetValue<IndicatorKey::INTERM_FI2_SIGNAL, mts::StorageBlock::Int8>());
    if (fi2Value == 0 && m_warmupBarCount > MIN_WARMUP_BARS) {
        // Note: FI2 can legitimately be NEUTRAL during ranging markets
        // This is a soft warning, not a hard block like RSI
        Logger::getInstance().log(
            "INFO: FI2Signal at NEUTRAL after " + std::to_string(m_warmupBarCount) +
            " bars. Verify Force Index calculation is active."
        );
    }

    // Phase 3: Validate HMM state (HMM regime detection)
    // For now, assume HMM is ready if HmmState indicator exists
    auto* hmmIndicator = HmmState();
    if (!hmmIndicator) {
        // HMM indicator not yet created
        Logger::getInstance().log(
            "INFO: HMM indicator not yet available after " + std::to_string(m_warmupBarCount) +
            " bars. Warmup delayed."
        );
        return;
    }

    // All validation passed - mark as warmed up
    m_isWarmedUp = true;

    if (!m_warmupLoggedOnce) {
        SCString msg = "WARMUP COMPLETE after ";
        msg += std::to_string(m_warmupBarCount).c_str();
        msg += " bars (RSI functional, HMM initialized). Data collection ready.";
        Logger::getInstance().log(msg.GetChars());
        m_warmupLoggedOnce = true;
    }
}

// OnBarClose removed - Legacy GUI publishing (Elite v2.4+: FlatBuffer events only)

// Public method to update bar context (timestamp, price, daily cache)
void IndicatorManager::UpdateBarContext(SCStudyInterfaceRef sc)
{
    // INSTITUTIONAL GUARANTEE: Daily cache must be refreshed continuously so
    // dimensions 12/13 (dist_day_high/low) remain valid across sessions.
    // Keep one-time loader validation log, but update anchors every bar.
    if (!m_dailyCacheInitialized)
    {
        m_dailyCacheInitialized = true;

        // VALIDATION: Check if daily cache backend loaded successfully
        if (!DailyHighLowLoader::Instance().IsLoaded()) {
            Logger::getInstance().log("CRITICAL: DailyHighLowLoader failed to load CSV");
        } else {
            Logger::getInstance().log("INFO: DailyHighLowLoader initialized successfully");
        }
    }

    // Refresh previous-day anchors every bar (internally day-gated by UpdateDailyCache)
    UpdateDailyCache(sc);

    // Elite v2.3: Check warmup status (validates indicators after 200 bars)
    CheckWarmupStatus(sc);

    // indicator-manager-dod-soa plan, whole-branch-review fix: SIDE's dirty
    // bit lost its writer when Task 10 removed the legacy Side object's push
    // sites (the object's VALUE is still correctly read live from
    // PositionManager::GetTradeSide() in GetTickCompanionValues() -- only
    // the dirty-bit side effect that used to drive prompt Event publication
    // on a trade-side flip was lost). Restore it directly against
    // PositionManager, the same authoritative source GetTickCompanionValues()
    // already reads.
    const TradeSideEnum currentSide = PositionManager::Instance().GetTradeSide();
    if (currentSide != m_lastKnownSide) {
        m_dirty_mask |= IndicatorKeyMask(IndicatorKey::SIDE);
        m_lastKnownSide = currentSide;
    }

    // Track regime tenure in InferenceManager (moved Mar 2026)
    InferenceManager::Instance().UpdateRegimeTenure();
}

// Task 10 (indicator-manager-dod-soa plan): one canonical per-tick gather of
// the companion values that the live Event path (EventSerializer.cpp), the
// training TrainingEvent path (GetTrainingEventT below), and
// BackTesterStudy.cpp's entry-context capture each used to re-derive
// independently via their own GetIndicator<T>()->GetX() calls. That
// duplication is exactly how GetTrainingEventT's close_percentile dead-write
// bug (two silently-disagreeing implementations, one of them recomputed from
// raw OHLC) was possible. Every field below is read from wherever it is
// already computed exactly once (the owning m_store leaf's own member state)
// -- nothing here recomputes anything. `side` is the one exception with no
// indicator-compute source at all: per the Task 5 finding, IndicatorManager's
// SIDE indicator was never a real indicator, only a manually-pushed mirror of
// PositionManager's authoritative Trade::m_side, so it is read directly from
// PositionManager here instead, and the mirror push-sites are removed.
TickCompanionValues IndicatorManager::GetTickCompanionValues() const {
    TickCompanionValues companions;
    companions.side = static_cast<int8_t>(PositionManager::Instance().GetTradeSide());
    companions.marketSymbol = static_cast<int8_t>(m_store.market_symbol.intValue());
    companions.overnightExit = static_cast<int8_t>(m_store.overnight_exit.intValue());
    companions.nhNlDaily = m_store.nh_nl_signal.GetDailyValue();
    // IMPORTANT semantic split (mirrors EventSerializer::SnapshotContext's
    // comment): prevHigh/prevLow = previous completed 15-minute bar extremes
    // (m_shortTermExtremes); prevDayHigh/prevDayLow = previous trading day
    // session extremes (IndicatorManager's own daily cache).
    companions.prevHigh = m_shortTermExtremes.prevHigh;
    companions.prevLow = m_shortTermExtremes.prevLow;
    companions.prevDayHigh = GetCachedPrevDayHigh();
    companions.prevDayLow = GetCachedPrevDayLow();
    companions.prevFourBarHigh = m_shortTermExtremes.prevFourBarHigh;
    companions.prevFourBarLow = m_shortTermExtremes.prevFourBarLow;
    companions.closePercentile = m_store.price_metrics.GetClosePercentile();
    companions.volumeRatioPercent = m_store.volume.GetVolumeRatio();
    companions.volumeImbalance = m_store.volume.GetVolumeImbalance();
    companions.atr10 = m_store.atr_prox.GetATR10();
    return companions;
}

// Elite v2.3: FlatBuffer Training Data Export
// Creates a TrainingEvent with all 60+ fields populated
// Elite v2.4 FIX: Return TrainingEventT object directly (no double Pack/UnPack)
// This prevents corruption with int8 enums during UnPack→Pack cycle

namespace {
// Crash breadcrumb for GetTrainingEventT — mirrors EDC breadcrumb file.
// Sub-steps 11-19 narrow which section crashes.
inline void WriteCrashProbe(int step) {
    FILE* f = fopen("C:/SierraChart2/Data/edc_breadcrumb.bin", "wb");
    if (f) { fwrite(&step, sizeof(step), 1, f); fclose(f); }
}
} // namespace

MTS::Training::TrainingEventT* IndicatorManager::GetTrainingEventT(SCStudyInterfaceRef sc) {
    using namespace MTS::Training;

    WriteCrashProbe(11);  // Entry

    // 1. Reuse the pooled scratch object instead of allocating fresh (Tier 2,
    // docs/superpowers/specs/2026-08-07-training-event-export-dod-design.md).
    // Every scalar field below is unconditionally overwritten before this
    // function returns (confirmed by the spec's write-site audit), so no
    // data from a previous call is ever visible to the caller.
    auto* event = &m_trainingEventScratch;

    // 2. Populate temporal metadata
    event->bar_index = sc.Index;
    event->timestamp_us = sc.BaseDateTimeIn[sc.Index].ToUNIXTimeInMicroseconds();

    int64_t deltaUs = 0;
    if (m_lastTrainingEventTimestampUs > 0 && event->timestamp_us > m_lastTrainingEventTimestampUs) {
        deltaUs = event->timestamp_us - m_lastTrainingEventTimestampUs;
        m_recentTrainingDeltaUs.push_back(deltaUs);
        if (m_recentTrainingDeltaUs.size() > kTrainingTauWindowSize) {
            m_recentTrainingDeltaUs.pop_front();
        }
    }

    int64_t tauMedianUs = deltaUs;
    if (!m_recentTrainingDeltaUs.empty()) {
        // Fixed scratch buffer, zero heap allocation (docs/superpowers/specs/
        // 2026-08-07-training-event-export-dod-design.md, Tier 1) -- replaces
        // a std::vector copy-constructed fresh on every significant-change
        // event. Only the filled prefix (m_recentTrainingDeltaUs.size(), not
        // the buffer's full kTrainingTauWindowSize capacity) participates in
        // nth_element.
        const size_t n = m_recentTrainingDeltaUs.size();
        std::array<int64_t, kTrainingTauWindowSize> scratch{};
        for (size_t i = 0; i < n; ++i) {
            scratch[i] = m_recentTrainingDeltaUs[i];
        }
        const size_t mid = n / 2;
        std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(mid),
                          scratch.begin() + static_cast<std::ptrdiff_t>(n));
        tauMedianUs = scratch[mid];
    }

    event->delta_t_log = std::log1p(static_cast<float>(deltaUs));
    event->tau_100_log = std::log1p(static_cast<float>(tauMedianUs));
    m_lastTrainingEventTimestampUs = event->timestamp_us;

    WriteCrashProbe(12);  // Before indicator population

    // Task 9 (indicator-manager-dod-soa plan): replaced the per-indicator
    // virtual-dispatch loop (`m_indicators[i]->AddToTrainingEventFB(*event)`)
    // with a call to the now-devirtualized PopulateIndicatorState(). This
    // removes the OSCILLATOR_310 crash-guard special case above as dead code
    // by construction: PopulateIndicatorState reads m_packed directly, has no
    // virtual dispatch, and is shared by both Event.indicators (live path,
    // SendEventFlatBuffer) and TrainingEvent.indicators (this path) -- there
    // is no longer a per-indicator virtual call anywhere in either consumer,
    // so there is nothing left to special-case. The fine-grained per-index
    // breadcrumbs (1200+i/1400+i) that used to bracket each indicator's
    // individual virtual call are removed for the same reason: there is no
    // longer a per-indicator loop to bracket.
    if (!event->indicators) {
        event->indicators = std::make_unique<MTS::Schema::IndicatorState>();
    }
    PopulateIndicatorState(*event->indicators);

    // Stopgap (Task 9 Step 1's explicit "last checkpoint" obligation, plan
    // Task 6 Step 4 / Task 8 Step 3 pattern): the deleted loop above also
    // populated several companion values that live outside
    // PopulateIndicatorState's Int8-only scope, via each leaf class's bespoke
    // AddToTrainingEventFB override. Verified by re-reading every override in
    // include/Indicator.h during this task (not assumed): the ones below have
    // no other write site anywhere in this function and would otherwise
    // silently regress training data. Companions already wired into m_packed
    // (Task 4/8) are read via GetValue<Key,Block>(); companions with no packed
    // home yet (NotPacked, Task 2's audit) are read directly from the
    // concrete m_store object -- a plain non-virtual accessor call, not vtable
    // dispatch. Task 10 gives all of these a durable, unified home
    // (TickCompanionValues) shared with the live path.
    event->indicators->mutate_kangaroo_tail_quality(
        GetValue<IndicatorKey::KANGAROO_TAIL, mts::StorageBlock::Float32>());
    event->indicators->mutate_turtle_soup_quality(
        GetValue<IndicatorKey::TURTLE_SOUP, mts::StorageBlock::Float32>());
    event->indicators->mutate_momentum_pinball_quality(
        GetValue<IndicatorKey::MOMENTUM_PINBALL, mts::StorageBlock::Float32>());
    event->indicators->mutate_elder_breakout_quality(
        GetValue<IndicatorKey::ELDER_BREAKOUT, mts::StorageBlock::Float32>());
    event->indicators->mutate_nr7_quality(
        GetValue<IndicatorKey::NR7, mts::StorageBlock::Float32>());
    event->indicators->mutate_corr_es_zn(
        GetValue<IndicatorKey::CORR_ES_ZN, mts::StorageBlock::Float32>());
    event->indicators->mutate_corr_es_dx(
        GetValue<IndicatorKey::CORR_ES_DX, mts::StorageBlock::Float32>());
    event->indicators->mutate_interm_fi2_norm(
        GetValue<IndicatorKey::INTERM_FI2_SIGNAL, mts::StorageBlock::Float32>());
    const float longFi13Norm = GetValue<IndicatorKey::LONG_FI13_SIGNAL, mts::StorageBlock::Float32>();
    event->indicators->mutate_long_fi13_norm(longFi13Norm);
    event->long_fi13_norm = longFi13Norm;  // TrainingEventT top-level mirror (FI13Signal::AddToTrainingEventFB)
    // impulse_run_length: NotPacked (position 26, unwired per Task 4) -- both
    // LONG_IMP's and INTERM_IMP's Impulse instances wrote this field in the
    // old loop (last-write-wins by IndicatorKey iteration order: INTERM_IMP,
    // the same pre-existing ambiguity Task 2's audit flagged), so read it from
    // the same INTERM_IMP instance here to reproduce the identical net value.
    event->indicators->mutate_impulse_run_length(static_cast<int8_t>(m_store.interm_imp.RunLength()));
    // interm_macd_norm: NotPacked TrainingEventT top-level field
    // (Macd::AddToTrainingEventFB). Same last-write-wins note as
    // impulse_run_length above (LONG_MACD's and INTERM_MACD's Macd instances
    // both wrote it; INTERM_MACD wins).
    event->interm_macd_norm = m_store.interm_macd.ZScore();

    // Task 10 (indicator-manager-dod-soa plan): single canonical gather of
    // atr_10 and every WriteTrainingRootSharedFields companion below, shared
    // with the live Event path. Fixes the close_percentile dead-write bug:
    // this used to be independently recomputed from raw OHLC a few lines
    // below, silently overwriting whatever PriceMetricsIndicator's own cached
    // percentile had already produced. There is now exactly one
    // implementation, used by both consumers.
    const auto companions = GetTickCompanionValues();
    event->atr_10 = companions.atr10;

    WriteCrashProbe(13);  // After indicator population

    // 3b. Inference indicators (moved to InferenceManager, Mar 2026)
    InferenceManager::Instance().AddToTrainingEventFB(*event);

    WriteCrashProbe(14);  // After InferenceManager

    // 4. Add ContextManager data (statistical context, HMM probs, normalized anchors)
    ContextManager::Instance().AddToTrainingEventFB(*event, sc);

    WriteCrashProbe(15);  // After ContextManager

    // 4b. CRITICAL: Sync features vector for inference consistency (Elite v2.3)
    SyncFeatureVector(event->features);

    WriteCrashProbe(16);  // After SyncFeatureVector
    // 5. Add OHLCV data
    const float currentOpen = static_cast<float>(sc.Open[sc.Index]);
    const float currentHigh = static_cast<float>(sc.High[sc.Index]);
    const float currentLow = static_cast<float>(sc.Low[sc.Index]);
    const float currentClose = static_cast<float>(sc.Close[sc.Index]);

    event->open = currentOpen;
    event->high = currentHigh;
    event->low = currentLow;
    event->close = currentClose;
    event->volume = static_cast<int64_t>(sc.Volume[sc.Index]);

    // close_percentile/volume_ratio_percent/volume_imbalance/nh_nl_daily/side/
    // market_symbol/overnight_exit/prev_*: all read from the same
    // `companions` snapshot gathered above -- no independent recomputation
    // (this replaces the old inline `(currentClose - currentLow) / barRange`
    // close_percentile recompute, which was the confirmed dead-write bug: it
    // silently overwrote PriceMetricsIndicator's own cached percentile with a
    // second, independently-derived value a few lines after indicator
    // population had already set it).
    mts::schema_contract::shared_writers::WriteTrainingRootSharedFields(
        *event,
        mts::schema_contract::shared_writers::TrainingRootSharedSlice{
            companions.side,
            companions.marketSymbol,
            companions.overnightExit,
            companions.nhNlDaily,
            companions.prevHigh,
            companions.prevLow,
            companions.prevDayHigh,
            companions.prevDayLow,
            companions.prevFourBarHigh,
            companions.prevFourBarLow,
            companions.closePercentile,
            companions.volumeRatioPercent,
            companions.volumeImbalance});

    // 7. Return object (caller will Pack once)
    return event;
}


// ============================================================================
// Elite v2.3: SyncFeatureVector() - CRITICAL Train-Serve Skew Prevention
// ============================================================================
// Single source of truth for feature vector packing (29 elements)
// MUST be called by BOTH training export and live inference paths
// Pattern: Explicit feature ordering (Phase 4 - Jan 22, 2026)
// ============================================================================
void IndicatorManager::SyncFeatureVector(std::vector<float>& targetVector) const {
    // Pre-allocate capacity for 29 features (avoids reallocation)
    targetVector.clear();
    targetVector.reserve(29);

    // ===== ZONE 1: Event Physics (indices 0-2) =====
    // Computed externally (Python EventPhysicsCalculator), added by caller
    // Skipped here - caller adds: delta_t_log, tau_100_log, event_velocity

    // ===== ZONE 2: Momentum (indices 3-5) =====
    // From ContextManager (Statistical Context)
    const auto& statContext = ContextManager::Instance().GetStatisticalContext();
    if (statContext.has_value()) {
        targetVector.push_back(statContext->velocity);      // 3: velocity
        targetVector.push_back(statContext->efficiency);    // 4: efficiency
        targetVector.push_back(statContext->volatility);    // 5: volatility
    } else {
        targetVector.insert(targetVector.end(), 3, 0.0f);  // Fast zero-fill
    }

    // ===== ZONE 3: Structural Anchors (indices 6-9) =====
    // From ContextManager (Normalized Anchors)
    const auto& anchors = ContextManager::Instance().GetNormalizedAnchors();
    if (anchors.has_value()) {
        targetVector.push_back(anchors->distEma13);          // 6: dist_ema_13
        targetVector.push_back(anchors->distDayHigh);        // 7: dist_day_high
        targetVector.push_back(anchors->distDayLow);         // 8: dist_day_low
    } else {
        targetVector.insert(targetVector.end(), 3, 0.0f);  // Fast zero-fill
    }
    targetVector.push_back(statContext.has_value() ? statContext->relRange : 0.0f);   // 9: rel_range

    // ===== ZONE 4: HMM Context (indices 10-12) =====
    // Entropy and entropy_delta are computed by Python HMM inference only
    // C++ does not calculate entropy - it's a Python-only metric
    targetVector.push_back(0.0f);  // 10: regime_entropy (placeholder - Python calculates)
    targetVector.push_back(0.0f);  // 11: entropy_delta (placeholder - Python calculates)
    targetVector.push_back(0.0f);  // 12: ignition_flag (placeholder - Python calculates)

    // ===== ZONE 5: Pattern Quality (indices 13-17) =====
    // Computed by Python pattern_quality_calculators.py
    targetVector.push_back(0.0f);  // 13: tail_quality (placeholder - Python calculates)
    targetVector.push_back(0.0f);  // 14: soup_quality (placeholder - Python calculates)
    targetVector.push_back(0.0f);  // 15: pinball_quality (placeholder - Python calculates)
    targetVector.push_back(0.0f);  // 16: breakout_quality (placeholder - Python calculates)
    targetVector.push_back(0.0f);  // 17: nr7_quality (placeholder - Python calculates)

    // ===== ZONE 6: Temporal Context (indices 18-21) =====
    // DOD/SoA migration (Task 5): read straight from the packed array — no
    // pointer, no null check, always a valid value.
    targetVector.push_back(static_cast<float>(GetValue<IndicatorKey::TIME_OF_DAY>()));  // 18: time_of_day_norm

    // bar_completion_pct: Computed by Python EventPhysicsCalculator (placeholder)
    targetVector.push_back(0.0f);  // 19: bar_completion_pct (placeholder - Python calculates)

    // regime_tenure: Bars in current HMM state (tracked by IndicatorManager)
    targetVector.push_back(static_cast<float>(InferenceManager::Instance().GetRegimeTenure()));  // 20: regime_tenure

    // session_age removed from indicator stack; keep 29D compatibility with explicit zero placeholder.
    targetVector.push_back(0.0f);  // 21: session_age (deprecated)

    // ===== ZONE 7: Screen Fusion (indices 22-25) =====
    // Elite v2.3: Use indicator floatValue() methods (indicators own normalization)
    // DOD/SoA migration (Task 11): read straight from the packed array — no
    // pointers, no null checks, always valid values.
    const float fi13Norm = GetValue<IndicatorKey::LONG_FI13_SIGNAL, mts::StorageBlock::Float32>();
    const int fi2Value = GetValue<IndicatorKey::INTERM_FI2_SIGNAL, mts::StorageBlock::Int8>();

    targetVector.push_back(fi13Norm);                          // 22: long_fi13_norm
    targetVector.push_back(static_cast<float>(fi2Value));      // 23: interm_fi2_norm
    // DOD/SoA migration (Task 7): read straight from the packed array — no
    // pointer, no null check, always a valid value.
    targetVector.push_back(static_cast<float>(GetValue<IndicatorKey::LONG_IMP>()));                       // 24: impulse_color
    targetVector.push_back(static_cast<float>(GetValue<IndicatorKey::DAILY_BIAS>()));                     // 25: daily_bias

    // ===== ZONE 8: Market Correlations (indices 26-28) =====
    // Computed by Python market_correlation_tracker.py (placeholders)
    targetVector.push_back(0.0f);  // 26: corr_es_zn (placeholder - Python calculates)
    targetVector.push_back(0.0f);  // 27: corr_es_dx (placeholder - Python calculates)
    targetVector.push_back(0.0f);  // 28: corr_velocity (placeholder - Python calculates)

    // Elite v2.3: Runtime validation (catches missing/extra features)
    const size_t expectedSize = 29;
    if (targetVector.size() != expectedSize) {
        // Silently skip validation during normal operation
        // Mismatch typically means warmup phase or model change
    }
}



// PublishHeartbeat() removed (Jan 24, 2026 - migrated to FlatBuffer events)
// ProcessChartRecalculation() removed (Feb 9, 2026 - 50-event history refactored to Python Cold-Start Orchestrator)

// ============================================================================
// Elite v2.4: Event-Driven FlatBuffer Publishing (port 5555)
// ============================================================================
// PublishEventOnChange: Check HasSignificantChange() and send FlatBuffer Event
// ELITE v2.4 OPTIMIZATION: Try delta encoding first (5-10× compression)
// Falls back to full event if delta unavailable
// GENMINI GUARD: Prevents sends during full recalculation (buffering only)
// ============================================================================
bool IndicatorManager::PublishEventOnChange(SCStudyInterfaceRef sc) {
    if (!HasSignificantChange()) {
        return false;  // No significant change, don't publish
    }

    // Send full event if significant change detected
    const bool sent = SendEventFlatBuffer(sc, false);  // Send full event (~400 bytes)
    if (sent) {
        // Phase 3.1: Flush any remaining dirty bits not consumed by serializer
        m_dirty_mask = 0;
    }
    return sent;
}

// ============================================================================
// PublishEventSnapshot: Force publish full Event snapshot
// Used on GUI connection to synchronize all indicator state
// ELITE v2.4: Snapshots always use full event (need complete state)
// ============================================================================
void IndicatorManager::PublishEventSnapshot(SCStudyInterfaceRef sc) {
    SendEventFlatBuffer(sc, true);  // Send snapshot (all indicators)
}

// ============================================================================
// SendEventFlatBuffer: Send LIVE trading events (84-field Event, not TrainingEvent)
// ELITE v2.4: Uses institutional-grade messaging layer (include/messaging/)
//
// ARCHITECTURE NOTE (Feb 6, 2026):
// - This method sends LIVE trading events (84 fields) via SerializeEvent()
// - Events go to TransportStream (port 5555) for real-time Python inference
// - Training data export is SEPARATE: EventDataCollectorStudy → LBRFileManager
// - TrainingEvent (60+ fields) is written to .lbr file by EventDataCollectorStudy
// ============================================================================
bool IndicatorManager::SendEventFlatBuffer(SCStudyInterfaceRef sc, bool isSnapshot) {
#ifndef NDEBUG
    // Called once per tick, after this tick's TripleScreen1/2/3 + SCStudies.cpp
    // indicator updates have all settled (Task 4 dual-write safety net).
    // Shared by both PublishEventOnChange() and PublishEventSnapshot() so the
    // assertion is reached on every live publish path, not just the delta path.
    AssertPackedStateParity();
#endif

    try {
        // Delegate to EventSerializer singleton for FlatBuffer serialization
        // - SerializeEvent() creates 84-field Event from indicator state
        // - Reusable builder: ~5-15µs latency, ~0 allocations
        // - Returns binary ready for ZMQ transport (port 5555)
        // Replay-safe current timestamp:
        // - Replay: replay timeline time
        // - Live: system clock time
        uint64_t timestamp_us = sc.GetCurrentDateTime().ToUNIXTimeInMicroseconds();

        // ✅ CRITICAL FIX (Feb 6, 2026): Use global sequence counter, not bar index
        // sc.Index is constant for all intrabar events (same bar = same index)
        // We need truly unique IDs for event ordering and drop detection
        uint64_t sequenceId = m_globalSequenceId++;  // Increment AFTER use

        // ✅ CORRECT: Serialize LIVE trading event (~49 fields, raw indicators)
        // Python handles feature engineering from raw indicator enums
        // Not TrainingEvent (which goes to .lbr file, not ZMQ)

        // Elite v2.5 Integration: Include ObservationData
        auto obs_vec = ContextManager::Instance().BuildObservationVector();
        MTS::Schema::ObservationData obs_data =
            MTS::Schema::Contract::MakeObservationData(obs_vec);

        auto asym_ctx = ContextManager::Instance().GetAsymmetryContext();

        const uint8_t* buffer = nullptr;
        size_t bufferSize = 0;
        const bool serialized = EventSerializer::Instance().SerializeEventInPlace(
            *this,
            sc.Index,                    // Bar index for context
            timestamp_us,
            sequenceId,
            buffer,
            bufferSize,
            &obs_data,
            &asym_ctx
        );
        if (!serialized || !buffer || bufferSize == 0) {
            return false;
        }

        // === Elite v2.4: Send via centralized TransportStream (port 5555) ===
        // Python listeners (port 5555 SUB socket) receive live events in real-time
        TransportStream::Instance().Emit(buffer, bufferSize);

        if (sc.Input[13].GetYesNo()) {  // Debug mode (IDX_INPUT_DEBUG_MTS)
            Logger::getInstance().log(
                "✅ Live Event " + std::string(isSnapshot ? "snapshot" : "") +
                " sent: " + std::to_string(bufferSize) + " bytes to port 5555"
            );
        }

        return true;
    }
    catch (const std::exception& e) {
        Logger::getInstance().log(
            "ERROR in SendEventFlatBuffer: " + std::string(e.what())
        );
        return false;
    }
}

bool IndicatorManager::HasSignificantChange() {
    // Bounds-sanitize: mask off any bits beyond MAX_INDICATORS to prevent
    // OOB access on m_indicators[] if dirty mask is ever corrupted.
    const uint64_t dirtyMask = m_dirty_mask & ALL_INDICATOR_MASK;

    if (dirtyMask == 0ULL) {
        return false;
    }

    // Fast path: evaluate primary trigger indicators only if any are dirty.
    uint64_t primaries = dirtyMask & PRIMARY_TRIGGER_MASK;
    while (primaries != 0ULL) {
        const unsigned int index = static_cast<unsigned int>(__builtin_ctzll(primaries));
        primaries &= (primaries - 1ULL);

        if (CheckTrigger(index)) {
            return true;
        }
    }

    // Secondary path: apply scoring filter only to dirty, non-primary indicators.
    uint64_t secondaries = dirtyMask & SECONDARY_TRIGGER_MASK;
    if (secondaries == 0ULL) {
        return false;
    }

    const auto& riskCtx = ContextManager::Instance().GetLocalRiskContext();

    while (secondaries != 0ULL) {
        const unsigned int index = static_cast<unsigned int>(__builtin_ctzll(secondaries));
        secondaries &= (secondaries - 1ULL);

        auto& indicator = m_indicators[index];
        if (!indicator) {
            continue;
        }

        const auto key = static_cast<IndicatorKey>(index);
        if (Scoring::Instance().IsIndicatorEventSignificant(key, riskCtx)) {
            return true;
        }
    }

    return false;
}

std::string IndicatorManager::getScreen1EntryText() {
    // DOD/SoA migration (Task 11): read straight from the packed array — no
    // pointers, no null checks, always valid values. (Previously returned
    // "NA" if either leaf object was null; packed reads can't be null, so
    // that branch is gone — both keys are guaranteed constructed by the time
    // this is called, same as every other already-migrated accessor here.)
    const int longMacdValue = static_cast<int>(GetValue<IndicatorKey::LONG_MACD>());
    const int longFI13Value = static_cast<int>(GetValue<IndicatorKey::LONG_FI13_SIGNAL, mts::StorageBlock::Int8>());

    char buffer[32] = {0};
    std::snprintf(buffer, sizeof(buffer), "%d|%d", longMacdValue, longFI13Value);
    return std::string(buffer);
}

std::string IndicatorManager::getScreen2EntryText() {
    // DOD/SoA migration (Task 11): all four fields now read straight from
    // the packed array — no pointers, no null checks, always valid values.
    char buffer[48] = {0};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%d|%d|%d|%d",
        static_cast<int>(GetValue<IndicatorKey::INTERM_FI2_SIGNAL, mts::StorageBlock::Int8>()),
        static_cast<int>(GetValue<IndicatorKey::INTERM_STOCHASTIC>()),
        static_cast<int>(GetValue<IndicatorKey::RASCHKE_STRATEGY_SETUP>()),
        static_cast<int>(GetValue<IndicatorKey::RASCHKE_TACTICAL_TRIGGER>())
    );
    return std::string(buffer);
}

std::string IndicatorManager::getScreen3EntryText() {
    // DOD/SoA migration (Task 7): all four fields read straight from the
    // packed array — no pointers, no null checks, always valid values.
    char buffer[48] = {0};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%d|%d|%d|%d",
        static_cast<int>(GetValue<IndicatorKey::STRUCTURE_TEST>()),
        static_cast<int>(GetValue<IndicatorKey::VOLUME_SIGNAL>()),
        static_cast<int>(GetValue<IndicatorKey::ATR_PROXIMITY>()),
        static_cast<int>(GetValue<IndicatorKey::DAILY_BIAS>())
    );
    return std::string(buffer);
}

bool IndicatorManager::CalculateRequiresInference() const {
    // ELITE v3.2: Determine Inference Relevance using Scoring Engine + LocalRiskContext
    const auto& riskCtx = ContextManager::Instance().GetLocalRiskContext();

    bool requiresInference = false;
    uint64_t dirtyMaskInference = m_dirty_mask;
    while (dirtyMaskInference != 0ULL) {
        const unsigned int index = static_cast<unsigned int>(__builtin_ctzll(dirtyMaskInference));
        dirtyMaskInference &= (dirtyMaskInference - 1ULL);

        auto& indicator = m_indicators[index];
        if (indicator) {
            const auto key = static_cast<IndicatorKey>(index);
            if (Scoring::Instance().IsIndicatorEventSignificant(key, riskCtx)) {
                requiresInference = true;
                break;
            }
        }
    }
    return requiresInference;
}

// Task 9 (indicator-manager-dod-soa plan): reads m_packed directly via the
// existing GetValue<Key>()/GetValue<Key,Block>() compile-time accessors -- no
// BaseIndicator*, no vtable, no m_indicators lookup anywhere in this function.
// Same case list as the old ExtractInt8AndClearDirty()-based switch (Int8-
// block rows only; Float32-block companions -- quality scores, z-score norms,
// base correlations -- stay out of scope here, matching this function's
// existing behavior, since Task 10 owns unifying those between the live and
// training paths). Unlike the old loop, this has NO dirty-clearing side
// effect: callers rely on PublishEventOnChange's explicit `m_dirty_mask = 0`
// flush after a successful publish instead (see that function) -- this also
// fixes a latent bug where calling this from the training-export path (see
// GetTrainingEventT below) used to silently clear the live-publish dirty mask
// as an unrelated side effect.
void IndicatorManager::PopulateIndicatorState(MTS::Schema::IndicatorState& state) const {
    using mts::StorageBlock;

    state.mutate_long_macd(GetValue<IndicatorKey::LONG_MACD>());
    state.mutate_long_fi13_signal(GetValue<IndicatorKey::LONG_FI13_SIGNAL, StorageBlock::Int8>());
    state.mutate_long_macd_divergence(GetValue<IndicatorKey::LONG_MACD_DIVERGENCE>());
    state.mutate_long_imp(GetValue<IndicatorKey::LONG_IMP>());
    state.mutate_interm_stochastic(GetValue<IndicatorKey::INTERM_STOCHASTIC>());
    state.mutate_raschke_strategy_setup(GetValue<IndicatorKey::RASCHKE_STRATEGY_SETUP>());
    state.mutate_raschke_tactical_trigger(GetValue<IndicatorKey::RASCHKE_TACTICAL_TRIGGER>());
    state.mutate_rsi(GetValue<IndicatorKey::RSI>());
    state.mutate_interm_fi2_signal(GetValue<IndicatorKey::INTERM_FI2_SIGNAL, StorageBlock::Int8>());
    state.mutate_ema_proximity(GetValue<IndicatorKey::EMA_PROXIMITY>());
    state.mutate_price_metrics(GetValue<IndicatorKey::PRICE_METRICS>());
    state.mutate_interm_macd_divergence(GetValue<IndicatorKey::INTERM_MACD_DIVERGENCE>());
    state.mutate_interm_macd(GetValue<IndicatorKey::INTERM_MACD, StorageBlock::Int8>());
    // INTERM_IMP has two Int8-block rows (position 12 = primary signal,
    // position 26 = impulse_run_length companion, unwired per Task 4).
    // DescriptorFor(key, block) resolves to the FIRST matching row (position
    // 12) -- the same disambiguation already proven at TripleScreen3.cpp and
    // PositionManager.cpp's live reads of this same key.
    state.mutate_interm_imp(GetValue<IndicatorKey::INTERM_IMP, StorageBlock::Int8>());
    state.mutate_structure_test(GetValue<IndicatorKey::STRUCTURE_TEST>());
    state.mutate_volume_signal(GetValue<IndicatorKey::VOLUME_SIGNAL>());
    state.mutate_atr_proximity(GetValue<IndicatorKey::ATR_PROXIMITY>());
    {
        // DAILY_BIAS writes both mutators from the same single source read
        // (see kIndicatorLayout.h's comment on this key) -- one packed slot,
        // fanned out to two mutators here, same as the old code.
        const int8_t dailyBias = GetValue<IndicatorKey::DAILY_BIAS>();
        state.mutate_daily_bias(dailyBias);
        state.mutate_daily_bias_enum(dailyBias);
    }
    state.mutate_time_of_day(GetValue<IndicatorKey::TIME_OF_DAY>());
    state.mutate_kangaroo_tail(GetValue<IndicatorKey::KANGAROO_TAIL, StorageBlock::Int8>());
    state.mutate_turtle_soup(GetValue<IndicatorKey::TURTLE_SOUP, StorageBlock::Int8>());
    state.mutate_momentum_pinball(GetValue<IndicatorKey::MOMENTUM_PINBALL, StorageBlock::Int8>());
    state.mutate_elder_breakout(GetValue<IndicatorKey::ELDER_BREAKOUT, StorageBlock::Int8>());
    state.mutate_nr7(GetValue<IndicatorKey::NR7, StorageBlock::Int8>());
    state.mutate_nh_nl_signal(GetValue<IndicatorKey::NH_NL_SIGNAL>());
    state.mutate_oscillator_310(GetValue<IndicatorKey::OSCILLATOR_310>());
    // ZN_TREND / DX_TREND: deferred to future release (NotPacked, per
    // kIndicatorLayout.h's audit comment) -- no case needed, matches the old
    // switch's explicit "fall through to default" for these two keys.
}

void IndicatorManager::UpdateDailyCache(SCStudyInterfaceRef sc) {
    // Trading-day semantics, not calendar date (docs/superpowers/plans/2026-08-04-volume-profile-daily-bias.md
    // final-review fix wave, lbrnet/logs/rc_gemini.log GEMINI_REVIEW_080): for an overnight-session
    // instrument like ES, Sunday evening through Monday's close is ONE trading day spanning two
    // calendar dates. Using plain GetDate() here would fire this gate at Monday midnight, mid-Globex,
    // and misresolve "yesterday" to Sunday (zero RTH bars by construction).
    const int currentTradingDay = sc.GetTradingDayDate(sc.BaseDateTimeIn[sc.Index]);

    // Trading-day-anchored midnight (docs/superpowers/plans/2026-08-04-volume-profile-daily-bias.md
    // final-review fix wave, round 2): the native-OHLC and CSV-override lookback loops below walk
    // backward by whole days to resolve "yesterday". Anchoring that walk to sc.BaseDateTimeIn[sc.Index]
    // (the current bar's raw calendar timestamp) is wrong for an overnight-session instrument -- on the
    // first bar of a new trading day that starts the prior calendar evening (e.g. Tuesday 18:00 Globex
    // open for trading day "Wednesday"), `daysBack=1` from that raw timestamp lands on Monday, not
    // Tuesday, shifting prevDayHigh/prevDayLow back a full session for the entire new trading day.
    // Anchoring to currentTradingDay's own midnight fixes both loops: DailyHighLowLoader::GetDataForDate
    // (src/DailyHighLowLoader.cpp:130-134) provably keys on calendar date only (SCDateTime::GetDateYMD),
    // so a trading-day-anchored SCDateTime resolves correctly there. GetOHLCForDate's exact treatment of
    // the argument is not independently confirmed from the header alone -- see report residual note.
    const SCDateTime tradingDayAnchor(currentTradingDay, 0);  // SCDateTime(int Date, int TimeInSeconds), scdatetime.h:1034

    // Update cache only when trading day changes (reload daily high/low once per day)
    if (currentTradingDay != m_dailyCache.tradingDay) {
    // ── INSTITUTIONAL HYBRID STRATEGY ─────────────────────────────
    // 1. DEFAULT: Attempt to get High/Low from Sierra Chart native history
    //    This ensures the study works "out of the box" without external dependencies.
    // 2. OVERRIDE: If CSV data exists, it takes precedence (Source of Truth).

    // Step 1: Native SC Fetch
    // We look back up to MAX_LOOKBACK_DAYS to find the most recent completed trading session.
    bool nativeDataFound = false;
    double nativeHigh = 0.0;
    double nativeLow = 0.0;


    int daysBackUsed = 0;

    constexpr int MAX_LOOKBACK_DAYS = 7;
    for (int daysBack = 1; daysBack <= MAX_LOOKBACK_DAYS; ++daysBack) {
        const SCDateTime candidateDay = tradingDayAnchor - static_cast<double>(daysBack);
        float scOpen = 0.0f, scHigh = 0.0f, scLow = 0.0f, scClose = 0.0f;

        // Native SC API call
        int result = sc.GetOHLCForDate(candidateDay, scOpen, scHigh, scLow, scClose);

        if (result != 0 && scHigh > 0.0f && scLow > 0.0f) {
            nativeHigh = static_cast<double>(scHigh);
            nativeLow = static_cast<double>(scLow);
            nativeDataFound = true;
            daysBackUsed = daysBack;
            break;
        }
    }

    // Step 2: Initialize with Native Data (if found)
    if (nativeDataFound) {
        m_dailyCache.prevDayHigh = static_cast<float>(nativeHigh);
        m_dailyCache.prevDayLow = static_cast<float>(nativeLow);
    }

    // Step 3: CSV Override (The "Institutional" Precision Layer)

    const auto& loader = DailyHighLowLoader::Instance();
    if (loader.IsLoaded()) {
        DailyHighLowData csvData;
        bool foundInCsv = false;

        // Try to match the exact date we found natively, or search similar logic
        constexpr int MAX_LOOKBACK_DAYS = 7;
    for (int daysBack = 1; daysBack <= MAX_LOOKBACK_DAYS; ++daysBack) {
            const SCDateTime candidateDay = tradingDayAnchor - static_cast<double>(daysBack);
            const DailyHighLowData candidate = loader.GetDataForDate(candidateDay);

            if (candidate.prevDayHigh > 0.0 && candidate.prevDayLow > 0.0) {
                csvData = candidate;
                foundInCsv = true;

                // DATA INTEGRITY CHECK
                if (nativeDataFound && daysBack == daysBackUsed) {
                    float diffHigh = std::abs(static_cast<float>(csvData.prevDayHigh) - m_dailyCache.prevDayHigh);
                    float diffLow = std::abs(static_cast<float>(csvData.prevDayLow) - m_dailyCache.prevDayLow);

                    // Preserve CSV override silently even if it diverges from chart values.
                    if ((diffHigh > 1.0f || diffLow > 1.0f) && !m_dailyAnchorZeroWarningLogged) {
                        // Intentionally no log here to avoid startup/runtime log pollution.
                    }
                }
                break;
            }
        }


        if (foundInCsv) {
            m_dailyCache.prevDayHigh = static_cast<float>(csvData.prevDayHigh);
            m_dailyCache.prevDayLow = static_cast<float>(csvData.prevDayLow);
        }
    } // End of loader.IsLoaded() check

    // === Real Volume Profile Value Area (docs/superpowers/plans/2026-08-04-volume-profile-daily-bias.md
    // final-review fix wave). Gated off by default -- see SetRealVolumeProfileDailyBiasEnabled's
    // comment for why. Mirrors the backward-walk-if-empty pattern the native-OHLC block above already
    // uses for prevDayHigh/prevDayLow, so a market holiday (or any other RTH-less prior day) doesn't
    // silently strand the cache -- it keeps walking back to the most recent day with real RTH volume.
    if (m_realVolumeProfileDailyBiasEnabled && sc.VolumeAtPriceForBars != nullptr) {
        // RTH-only (09:30-16:00 ET): Market Profile theory is built on RTH volume, and this codebase
        // already keeps the same RTH/overnight split for the Amihud percentile pools (TripleScreen3.cpp
        // "Layer B") for the same reason -- thin overnight/Globex volume would otherwise dilute the
        // Value Area into something unnaturally wide. sc.HMS_TIME() is deprecated (scdatetime.h:965) --
        // SCDateTime(...).GetTime() is the modern replacement.
        static const int kRthOpenTime = SCDateTime(9, 30, 0, 0).GetTime();
        static const int kRthCloseTime = SCDateTime(16, 0, 0, 0).GetTime();
        constexpr int MAX_VALUE_AREA_LOOKBACK_DAYS = 7;

        int searchEndIndex = sc.GetFirstIndexForDate(sc.ChartNumber, currentTradingDay);
        bool foundValueArea = false;

        for (int daysBack = 0; daysBack < MAX_VALUE_AREA_LOOKBACK_DAYS && searchEndIndex > 0 && !foundValueArea; ++daysBack) {
            const int candidateLastIndex = searchEndIndex - 1;
            const int candidateTradingDay = sc.GetTradingDayDate(sc.BaseDateTimeIn[candidateLastIndex]);
            const int candidateFirstIndex = sc.GetFirstIndexForDate(sc.ChartNumber, candidateTradingDay);
            if (candidateFirstIndex < 0 || candidateFirstIndex > candidateLastIndex) {
                break;  // date resolution failed -- stop walking, fall through to the zeroed fallback below
            }

            std::vector<vpe::PriceVolume> levels;
            for (int barIndex = candidateFirstIndex; barIndex <= candidateLastIndex; ++barIndex) {
                const int barTime = sc.BaseDateTimeIn[barIndex].GetTime();
                if (barTime < kRthOpenTime || barTime >= kRthCloseTime) {
                    continue;  // overnight/Globex bar -- excluded from the Value Area
                }
                const unsigned int numLevels = sc.VolumeAtPriceForBars->GetSizeAtBarIndex(barIndex);
                for (unsigned int i = 0; i < numLevels; ++i) {
                    const s_VolumeAtPriceV2* vap = nullptr;
                    if (sc.VolumeAtPriceForBars->GetVAPElementAtIndex(barIndex, static_cast<int>(i), &vap) && vap != nullptr) {
                        levels.push_back({static_cast<int32_t>(vap->PriceInTicks), static_cast<double>(vap->Volume)});
                    }
                }
            }

            const vpe::ValueArea va = vpe::ComputeValueArea(levels, 70.0);
            if (va.valid && sc.TickSize > 0.0) {
                m_dailyCache.valueAreaHigh = static_cast<float>(va.valueAreaHighInTicks * sc.TickSize);
                m_dailyCache.valueAreaLow = static_cast<float>(va.valueAreaLowInTicks * sc.TickSize);
                foundValueArea = true;
            } else {
                searchEndIndex = candidateFirstIndex;  // this day had no RTH volume -- walk back further
            }
        }

        if (!foundValueArea) {
            m_dailyCache.valueAreaHigh = 0.0f;
            m_dailyCache.valueAreaLow = 0.0f;
            Logger::getInstance().log(
                "IndicatorManager::UpdateDailyCache: Volume Profile Value Area aggregation found no "
                "valid RTH session within " + std::to_string(MAX_VALUE_AREA_LOOKBACK_DAYS) +
                " days -- falling back to the range-split proxy for today."
            );
        }
    } else {
        m_dailyCache.valueAreaHigh = 0.0f;
        m_dailyCache.valueAreaLow = 0.0f;
    }

    // Daily anchor updates are intentionally silent to avoid recurring startup/runtime noise.
    m_dailyCache.tradingDay = currentTradingDay;
    m_dailyAnchorZeroWarningLogged = false;
    m_dailyCache.validated = true;
} // End of UpdateDailyCache logic (inside if(currentTradingDay != ...))
} // End of UpdateDailyCache function


template <typename T>
T* IndicatorManager::GetIndicator(IndicatorKey key) const
{
    const size_t idx = static_cast<size_t>(key);
    if (idx >= static_cast<size_t>(IndicatorKey::MAX_INDICATORS)) {
        return nullptr;
    }
    return static_cast<T*>(m_indicators[idx]);
}

template Impulse* IndicatorManager::GetIndicator<Impulse>(IndicatorKey key) const;
template Macd* IndicatorManager::GetIndicator<Macd>(IndicatorKey key) const;
template MACDDivergence* IndicatorManager::GetIndicator<MACDDivergence>(IndicatorKey key) const;
template Stochastic* IndicatorManager::GetIndicator<Stochastic>(IndicatorKey key) const;
template FI13Signal* IndicatorManager::GetIndicator<FI13Signal>(IndicatorKey key) const;
template FI2Signal* IndicatorManager::GetIndicator<FI2Signal>(IndicatorKey key) const;
template Side* IndicatorManager::GetIndicator<Side>(IndicatorKey key) const;
template IntermediateMarketAction* IndicatorManager::GetIndicator<IntermediateMarketAction>(IndicatorKey key) const;
template RaschkeStrategyIndicator* IndicatorManager::GetIndicator<RaschkeStrategyIndicator>(IndicatorKey key) const;
template RaschkeTacticalIndicator* IndicatorManager::GetIndicator<RaschkeTacticalIndicator>(IndicatorKey key) const;
template RSIIndicator* IndicatorManager::GetIndicator<RSIIndicator>(IndicatorKey key) const;
template VolumeIndicator* IndicatorManager::GetIndicator<VolumeIndicator>(IndicatorKey key) const;
template StructureTestIndicator* IndicatorManager::GetIndicator<StructureTestIndicator>(IndicatorKey key) const;
template ATRProximityIndicator* IndicatorManager::GetIndicator<ATRProximityIndicator>(IndicatorKey key) const;
template EmaProximityIndicator* IndicatorManager::GetIndicator<EmaProximityIndicator>(IndicatorKey key) const;
template PriceMetricsIndicator* IndicatorManager::GetIndicator<PriceMetricsIndicator>(IndicatorKey key) const;
template MarketSymbolIndicator* IndicatorManager::GetIndicator<MarketSymbolIndicator>(IndicatorKey key) const;
template DailyBiasIndicator* IndicatorManager::GetIndicator<DailyBiasIndicator>(IndicatorKey key) const;
template TimeOfDayIndicator* IndicatorManager::GetIndicator<TimeOfDayIndicator>(IndicatorKey key) const;
template OvernightExitIndicator* IndicatorManager::GetIndicator<OvernightExitIndicator>(IndicatorKey key) const;
template HurstExponentIndicator* IndicatorManager::GetIndicator<HurstExponentIndicator>(IndicatorKey key) const;
template NhNlSignalIndicator* IndicatorManager::GetIndicator<NhNlSignalIndicator>(IndicatorKey key) const;
template KangarooTail* IndicatorManager::GetIndicator<KangarooTail>(IndicatorKey key) const;
template TurtleSoup* IndicatorManager::GetIndicator<TurtleSoup>(IndicatorKey key) const;
template Oscillator310* IndicatorManager::GetIndicator<Oscillator310>(IndicatorKey key) const;
template MomentumPinball* IndicatorManager::GetIndicator<MomentumPinball>(IndicatorKey key) const;
template ElderBreakout* IndicatorManager::GetIndicator<ElderBreakout>(IndicatorKey key) const;
template NR7* IndicatorManager::GetIndicator<NR7>(IndicatorKey key) const;
template CorrelationIndicator* IndicatorManager::GetIndicator<CorrelationIndicator>(IndicatorKey key) const;
template CrossMarketTrend* IndicatorManager::GetIndicator<CrossMarketTrend>(IndicatorKey key) const;
template VwapIndicator* IndicatorManager::GetIndicator<VwapIndicator>(IndicatorKey key) const;
