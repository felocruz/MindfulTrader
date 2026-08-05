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
namespace FeatureDefaults {
    // NOTE: MAX_HMM_ENTROPY removed - now handled by HmmStateIndicator
    constexpr float IMPULSE_NEUTRAL = 0.5f;       // Midpoint of [-1.0,1.0] range
    constexpr float DAILY_BIAS_NEUTRAL = 0.5f;    // Midpoint when bias unknown
}

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
        }};

    static_assert(ArraysEqual(kRuntimeRegisteredIndicatorKeyValues,
                              mts::schema_contract::kIndicatorKeyRegistryValues),
                  "Runtime indicator registration keys diverge from generated indicator key registry");
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
    m_indicators[static_cast<size_t>(IndicatorKey::LONG_FI13_SIGNAL)] = &m_store.long_fi13;
    m_store.long_fi13.SetPackedSlotPointer(m_packed.RawI8Pointer(1));
    m_store.long_fi13.SetPackedSlotPointer(m_packed.RawF32Pointer(9));
    m_indicators[static_cast<size_t>(IndicatorKey::LONG_MACD_DIVERGENCE)] = &m_store.long_macd_div;
    m_store.long_macd_div.SetPackedSlotPointer(m_packed.RawI8Pointer(2));
    m_indicators[static_cast<size_t>(IndicatorKey::LONG_IMP)] = &m_store.long_imp;
    m_store.long_imp.SetPackedSlotPointer(m_packed.RawI8Pointer(3));
    m_indicators[static_cast<size_t>(IndicatorKey::LONG_MKT_ACTION)] = &m_store.long_mkt_action;

    // Screen 2
    m_indicators[static_cast<size_t>(IndicatorKey::INTERM_STOCHASTIC)] = &m_store.interm_stochastic;
    m_store.interm_stochastic.SetPackedSlotPointer(m_packed.RawI8Pointer(4));
    m_indicators[static_cast<size_t>(IndicatorKey::RASCHKE_STRATEGY_SETUP)] = &m_store.raschke_strategy;
    m_store.raschke_strategy.SetPackedSlotPointer(m_packed.RawI8Pointer(5));
    m_indicators[static_cast<size_t>(IndicatorKey::RASCHKE_TACTICAL_TRIGGER)] = &m_store.raschke_tactical;
    m_store.raschke_tactical.SetPackedSlotPointer(m_packed.RawI8Pointer(6));
    m_indicators[static_cast<size_t>(IndicatorKey::RSI)] = &m_store.rsi;
    m_store.rsi.SetPackedSlotPointer(m_packed.RawI8Pointer(7));
    m_indicators[static_cast<size_t>(IndicatorKey::INTERM_FI2_SIGNAL)] = &m_store.interm_fi2;
    m_store.interm_fi2.SetPackedSlotPointer(m_packed.RawI8Pointer(8));
    m_store.interm_fi2.SetPackedSlotPointer(m_packed.RawF32Pointer(7));
    m_indicators[static_cast<size_t>(IndicatorKey::EMA_PROXIMITY)] = &m_store.ema_prox;
    m_store.ema_prox.SetPackedSlotPointer(m_packed.RawI8Pointer(9));
    m_indicators[static_cast<size_t>(IndicatorKey::PRICE_METRICS)] = &m_store.price_metrics;
    m_store.price_metrics.SetPackedSlotPointer(m_packed.RawI8Pointer(10));
    m_indicators[static_cast<size_t>(IndicatorKey::INTERM_MACD_DIVERGENCE)] = &m_store.interm_macd_div;
    m_store.interm_macd_div.SetPackedSlotPointer(m_packed.RawI8Pointer(11));
    m_indicators[static_cast<size_t>(IndicatorKey::INTERM_IMP)] = &m_store.interm_imp;
    m_store.interm_imp.SetPackedSlotPointer(m_packed.RawI8Pointer(12));
    // NOTE: kIndicatorLayout also has a second, companion Int8 row for
    // INTERM_IMP at position 26 (impulse_run_length) — that is a distinct
    // field (m_runLength, not m_value) with no shared write path through the
    // generic Indicator<T>::Update() hook used here, and Task 2's audit flags
    // it as a pre-existing live-path gap (only BackTesterStudy.cpp populates
    // it today). Out of scope for this task; deferred to Task 9/10.
    m_indicators[static_cast<size_t>(IndicatorKey::INTERM_MACD)] = &m_store.interm_macd;
    m_store.interm_macd.SetPackedSlotPointer(m_packed.RawI8Pointer(13));
    // NOTE: INTERM_MACD's Float32-block companion (position 8, interm_macd_norm)
    // is intentionally NOT wired — Task 2's audit confirmed Macd::AddToTrainingEventFB
    // writes only the ambiguous top-level TrainingEventT::interm_macd_norm field,
    // not the IndicatorState struct field this packed slot represents. No working
    // internal write path exists to dual-write from; deferred to Task 9/10.

    // Screen 3
    m_indicators[static_cast<size_t>(IndicatorKey::STRUCTURE_TEST)] = &m_store.structure_test;
    m_store.structure_test.SetPackedSlotPointer(m_packed.RawI8Pointer(14));
    m_indicators[static_cast<size_t>(IndicatorKey::VOLUME_SIGNAL)] = &m_store.volume;
    m_store.volume.SetPackedSlotPointer(m_packed.RawI8Pointer(15));
    m_indicators[static_cast<size_t>(IndicatorKey::ATR_PROXIMITY)] = &m_store.atr_prox;
    m_store.atr_prox.SetPackedSlotPointer(m_packed.RawI8Pointer(16));
    m_indicators[static_cast<size_t>(IndicatorKey::DAILY_BIAS)] = &m_store.daily_bias;
    m_store.daily_bias.SetPackedSlotPointer(m_packed.RawI8Pointer(17));
    m_indicators[static_cast<size_t>(IndicatorKey::KANGAROO_TAIL)] = &m_store.kangaroo_tail;
    m_store.kangaroo_tail.SetPackedSlotPointer(m_packed.RawI8Pointer(18));
    m_store.kangaroo_tail.SetPackedSlotPointer(m_packed.RawF32Pointer(0));
    m_indicators[static_cast<size_t>(IndicatorKey::TURTLE_SOUP)] = &m_store.turtle_soup;
    m_store.turtle_soup.SetPackedSlotPointer(m_packed.RawI8Pointer(19));
    m_store.turtle_soup.SetPackedSlotPointer(m_packed.RawF32Pointer(1));
    m_indicators[static_cast<size_t>(IndicatorKey::MOMENTUM_PINBALL)] = &m_store.momentum_pinball;
    m_store.momentum_pinball.SetPackedSlotPointer(m_packed.RawI8Pointer(20));
    m_store.momentum_pinball.SetPackedSlotPointer(m_packed.RawF32Pointer(2));
    m_indicators[static_cast<size_t>(IndicatorKey::ELDER_BREAKOUT)] = &m_store.elder_breakout;
    m_store.elder_breakout.SetPackedSlotPointer(m_packed.RawI8Pointer(21));
    m_store.elder_breakout.SetPackedSlotPointer(m_packed.RawF32Pointer(3));
    m_indicators[static_cast<size_t>(IndicatorKey::NR7)] = &m_store.nr7;
    m_store.nr7.SetPackedSlotPointer(m_packed.RawI8Pointer(22));
    m_store.nr7.SetPackedSlotPointer(m_packed.RawF32Pointer(4));

    m_indicators[static_cast<size_t>(IndicatorKey::SHORT_MKT_ACTION)] = &m_store.short_mkt_action;
    m_indicators[static_cast<size_t>(IndicatorKey::OSCILLATOR_310)] = &m_store.oscillator_310;
    m_store.oscillator_310.SetPackedSlotPointer(m_packed.RawI8Pointer(24));
    m_indicators[static_cast<size_t>(IndicatorKey::VWAP)] = &m_store.vwap;

    m_indicators[static_cast<size_t>(IndicatorKey::SIDE)] = &m_store.side;
    m_indicators[static_cast<size_t>(IndicatorKey::MARKET_SYMBOL)] = &m_store.market_symbol;
    m_indicators[static_cast<size_t>(IndicatorKey::TIME_OF_DAY)] = &m_store.time_of_day;
    m_store.time_of_day.SetPackedSlotPointer(m_packed.RawI8Pointer(25));
    m_indicators[static_cast<size_t>(IndicatorKey::OVERNIGHT_EXIT)] = &m_store.overnight_exit;

    // HMM_STATE, MARKET_CLIMATE, PREDICTION_STATE → InferenceManager (Mar 2026)
    // Slots left null — accessors delegate to InferenceManager::Instance()
    m_indicators[static_cast<size_t>(IndicatorKey::HURST_EXPONENT)] = &m_store.hurst_exponent;

    m_indicators[static_cast<size_t>(IndicatorKey::NH_NL_SIGNAL)] = &m_store.nh_nl_signal;
    m_store.nh_nl_signal.SetPackedSlotPointer(m_packed.RawI8Pointer(23));

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
// Phase 1.2: Static Metaprogramming Dispatcher (Devirtualized Trigger Check)
// ----------------------------------------------------------------------------
// This function dispatches dirty-bit checks directly to the concrete types in m_store.
// The compiler can inline ShouldTrigger() and optimize away the vtable lookup.
// ----------------------------------------------------------------------------
bool IndicatorManager::CheckTrigger(size_t index) const {
    switch (static_cast<IndicatorKey>(index)) {
        // Screen 1
        case IndicatorKey::LONG_MACD: return m_store.long_macd.ShouldTrigger();
        case IndicatorKey::LONG_FI13_SIGNAL: return m_store.long_fi13.ShouldTrigger();
        case IndicatorKey::LONG_MACD_DIVERGENCE: return m_store.long_macd_div.ShouldTrigger();
        case IndicatorKey::LONG_IMP: return m_store.long_imp.ShouldTrigger();
        case IndicatorKey::LONG_MKT_ACTION: return m_store.long_mkt_action.ShouldTrigger();

        // Screen 2
        case IndicatorKey::INTERM_STOCHASTIC: return m_store.interm_stochastic.ShouldTrigger();
        case IndicatorKey::RASCHKE_STRATEGY_SETUP: return m_store.raschke_strategy.ShouldTrigger();
        case IndicatorKey::RASCHKE_TACTICAL_TRIGGER: return m_store.raschke_tactical.ShouldTrigger();
        case IndicatorKey::RSI: return m_store.rsi.ShouldTrigger();
        case IndicatorKey::INTERM_FI2_SIGNAL: return m_store.interm_fi2.ShouldTrigger();
        case IndicatorKey::EMA_PROXIMITY: return m_store.ema_prox.ShouldTrigger();
        case IndicatorKey::PRICE_METRICS: return m_store.price_metrics.ShouldTrigger();
        case IndicatorKey::INTERM_MACD_DIVERGENCE: return m_store.interm_macd_div.ShouldTrigger();
        case IndicatorKey::INTERM_IMP: return m_store.interm_imp.ShouldTrigger();
        case IndicatorKey::INTERM_MACD: return m_store.interm_macd.ShouldTrigger();

        // Screen 3
        case IndicatorKey::STRUCTURE_TEST: return m_store.structure_test.ShouldTrigger();
        case IndicatorKey::VOLUME_SIGNAL: return m_store.volume.ShouldTrigger();
        case IndicatorKey::ATR_PROXIMITY: return m_store.atr_prox.ShouldTrigger();
        case IndicatorKey::DAILY_BIAS: return m_store.daily_bias.ShouldTrigger();
        case IndicatorKey::KANGAROO_TAIL: return m_store.kangaroo_tail.ShouldTrigger();
        case IndicatorKey::TURTLE_SOUP: return m_store.turtle_soup.ShouldTrigger();
        case IndicatorKey::MOMENTUM_PINBALL: return m_store.momentum_pinball.ShouldTrigger();
        case IndicatorKey::ELDER_BREAKOUT: return m_store.elder_breakout.ShouldTrigger();
        case IndicatorKey::NR7: return m_store.nr7.ShouldTrigger();
        case IndicatorKey::SHORT_MKT_ACTION: return m_store.short_mkt_action.ShouldTrigger();
        case IndicatorKey::OSCILLATOR_310: return m_store.oscillator_310.ShouldTrigger();

        // Metadata & State
        case IndicatorKey::SIDE: return m_store.side.ShouldTrigger();
        case IndicatorKey::MARKET_SYMBOL: return m_store.market_symbol.ShouldTrigger();
        case IndicatorKey::TIME_OF_DAY: return m_store.time_of_day.ShouldTrigger();
        case IndicatorKey::OVERNIGHT_EXIT: return m_store.overnight_exit.ShouldTrigger();
        // HMM_STATE, MARKET_CLIMATE → InferenceManager (no longer in dirty-mask dispatch)
        case IndicatorKey::HURST_EXPONENT: return m_store.hurst_exponent.ShouldTrigger();
        case IndicatorKey::NH_NL_SIGNAL: return m_store.nh_nl_signal.ShouldTrigger();

        // Cross-Market Correlations
        case IndicatorKey::CORR_ES_ZN: return m_store.corr_es_zn.ShouldTrigger();
        case IndicatorKey::CORR_ES_DX: return m_store.corr_es_dx.ShouldTrigger();
        case IndicatorKey::ZN_TREND: return m_store.zn_trend.ShouldTrigger();
        case IndicatorKey::DX_TREND: return m_store.dx_trend.ShouldTrigger();
        case IndicatorKey::CORR_ES_ZN_DELTA: return m_store.corr_es_zn_delta.ShouldTrigger();
        case IndicatorKey::CORR_ES_ZN_ACCEL: return m_store.corr_es_zn_accel.ShouldTrigger();
        case IndicatorKey::CORR_ES_DX_DELTA: return m_store.corr_es_dx_delta.ShouldTrigger();
        case IndicatorKey::CORR_ES_DX_ACCEL: return m_store.corr_es_dx_accel.ShouldTrigger();

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
void IndicatorManager::AssertPackedStateParity() const {
    for (const auto& desc : mts::kIndicatorLayout) {
        if (desc.block != mts::StorageBlock::Int8) continue;
        if (desc.key == IndicatorKey::INTERM_IMP && desc.position == 26) continue;

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
    const auto rsiIndicator = GetIndicator<RSIIndicator>(IndicatorKey::RSI);
    if (rsiIndicator) {
        const int rsiValue = rsiIndicator->intValue();
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
    const auto fi2Indicator = GetIndicator<FI2Signal>(IndicatorKey::INTERM_FI2_SIGNAL);
    if (fi2Indicator && fi2Indicator->intValue() == 0 && m_warmupBarCount > MIN_WARMUP_BARS) {
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

    // Track regime tenure in InferenceManager (moved Mar 2026)
    InferenceManager::Instance().UpdateRegimeTenure();
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

std::unique_ptr<MTS::Training::TrainingEventT> IndicatorManager::GetTrainingEventT(SCStudyInterfaceRef sc) {
    using namespace MTS::Training;

    WriteCrashProbe(11);  // Entry

    // 1. Create native C++ object using Object API
    auto event = std::make_unique<TrainingEventT>();

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
        std::vector<int64_t> scratch(m_recentTrainingDeltaUs.begin(), m_recentTrainingDeltaUs.end());
        const size_t mid = scratch.size() / 2;
        std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(mid), scratch.end());
        tauMedianUs = scratch[mid];
    }

    event->delta_t_log = std::log1p(static_cast<float>(deltaUs));
    event->tau_100_log = std::log1p(static_cast<float>(tauMedianUs));
    m_lastTrainingEventTimestampUs = event->timestamp_us;

    WriteCrashProbe(12);  // Before indicator loop

    // 3. Iterate all indicators and populate their fields
    for (size_t i = 0; i < m_indicators.size(); ++i) {
        if (m_indicators[i]) {
            // Write indicator index to breadcrumb so crash reveals which one
            // Pre-call probe: 1200+i, Post-call probe: 1400+i
            WriteCrashProbe(1200 + static_cast<int>(i));

            // Crash guard: production evidence repeatedly points to a fault
            // during virtual dispatch at OSCILLATOR_310 (index 26).
            // Emit the same training field directly from owned storage.
            if (i == static_cast<size_t>(IndicatorKey::OSCILLATOR_310)) {
                if (!event->indicators) {
                    event->indicators = std::make_unique<MTS::Schema::IndicatorState>();
                }
                event->indicators->mutate_oscillator_310(
                    static_cast<int8_t>(m_store.oscillator_310.intValue()));
                m_dirty_mask &= ~(1ULL << static_cast<uint64_t>(IndicatorKey::OSCILLATOR_310));
                WriteCrashProbe(1400 + static_cast<int>(i));
                continue;
            }

            m_indicators[i]->AddToTrainingEventFB(*event);
            WriteCrashProbe(1400 + static_cast<int>(i));
        }
    }

    WriteCrashProbe(13);  // After indicator loop

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

    // 6. Calculate bar metrics (rel_range = bar range / ATR10)
    float barRange = currentHigh - currentLow;

    float closePercentile = 0.0f;
    if (barRange > 0.0f) {
        closePercentile = (currentClose - currentLow) / barRange;
    }

    // v5.6: VolumeIndicator owns volume_ratio_percent via AddToTrainingEventFB
    // (robust log-volume z-score, Taleb/Mandelbrot consistent)

    const auto* sideIndicator = GetIndicator<Side>(IndicatorKey::SIDE);
    const auto* marketSymbolIndicator = GetIndicator<MarketSymbolIndicator>(IndicatorKey::MARKET_SYMBOL);
    const auto* overnightExitIndicator = GetIndicator<OvernightExitIndicator>(IndicatorKey::OVERNIGHT_EXIT);
    const auto* nhnlIndicator = GetIndicator<NhNlSignalIndicator>(IndicatorKey::NH_NL_SIGNAL);
    const auto* volumeIndicator = GetIndicator<VolumeIndicator>(IndicatorKey::VOLUME_SIGNAL);
    const auto* shortActionIndicator = GetIndicator<ShortMarketAction>(IndicatorKey::SHORT_MKT_ACTION);

    const int8_t side = sideIndicator ? static_cast<int8_t>(sideIndicator->intValue()) : 0;
    const int8_t marketSymbol = marketSymbolIndicator ? static_cast<int8_t>(marketSymbolIndicator->intValue()) : 0;
    const int8_t overnightExit = overnightExitIndicator ? static_cast<int8_t>(overnightExitIndicator->intValue()) : 0;
    const float nhNlDaily = nhnlIndicator ? nhnlIndicator->GetDailyValue() : 0.0f;
    const float volumeRatioPercent = volumeIndicator ? volumeIndicator->GetVolumeRatio() : 0.0f;
    const float volumeImbalance = volumeIndicator ? volumeIndicator->GetVolumeImbalance() : 0.0f;

    // IMPORTANT semantic split:
    // - prevHigh/prevLow = previous completed 15-minute bar extremes (ShortMarketAction)
    // - prevDayHigh/prevDayLow = previous trading day session extremes (daily cache)
    const float prevHigh = shortActionIndicator ? shortActionIndicator->PrevHigh() : 0.0f;
    const float prevLow = shortActionIndicator ? shortActionIndicator->PrevLow() : 0.0f;
    const float prevDayHigh = GetCachedPrevDayHigh();
    const float prevDayLow = GetCachedPrevDayLow();
    const float prevFourBarHigh = shortActionIndicator ? shortActionIndicator->PrevFourBarHigh() : 0.0f;
    const float prevFourBarLow = shortActionIndicator ? shortActionIndicator->PrevFourBarLow() : 0.0f;

    mts::schema_contract::shared_writers::WriteTrainingRootSharedFields(
        *event,
        mts::schema_contract::shared_writers::TrainingRootSharedSlice{
            side,
            marketSymbol,
            overnightExit,
            nhNlDaily,
            prevHigh,
            prevLow,
            prevDayHigh,
            prevDayLow,
            prevFourBarHigh,
            prevFourBarLow,
            closePercentile,
            volumeRatioPercent,
            volumeImbalance});

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
    // Elite v2.3: Use indicator intValue() methods for enum-based indicators
    const auto timeOfDayIndicator = GetIndicator<TimeOfDayIndicator>(IndicatorKey::TIME_OF_DAY);
    targetVector.push_back(timeOfDayIndicator ? static_cast<float>(timeOfDayIndicator->intValue()) : 0.0f);  // 18: time_of_day_norm

    // bar_completion_pct: Computed by Python EventPhysicsCalculator (placeholder)
    targetVector.push_back(0.0f);  // 19: bar_completion_pct (placeholder - Python calculates)

    // regime_tenure: Bars in current HMM state (tracked by IndicatorManager)
    targetVector.push_back(static_cast<float>(InferenceManager::Instance().GetRegimeTenure()));  // 20: regime_tenure

    // session_age removed from indicator stack; keep 29D compatibility with explicit zero placeholder.
    targetVector.push_back(0.0f);  // 21: session_age (deprecated)

    // ===== ZONE 7: Screen Fusion (indices 22-25) =====
    // Elite v2.3: Use indicator floatValue() methods (indicators own normalization)
    const auto fi13Indicator = GetIndicator<FI13Signal>(IndicatorKey::LONG_FI13_SIGNAL);
    const auto fi2Indicator = GetIndicator<FI2Signal>(IndicatorKey::INTERM_FI2_SIGNAL);
    const auto longImpulseIndicator = GetIndicator<Impulse>(IndicatorKey::LONG_IMP);
    const auto dailyBiasIndicator = GetIndicator<DailyBiasIndicator>(IndicatorKey::DAILY_BIAS);

    targetVector.push_back(fi13Indicator ? fi13Indicator->ZScore() : 0.0f);                              // 22: long_fi13_norm
    targetVector.push_back(fi2Indicator ? static_cast<float>(fi2Indicator->intValue()) : 0.0f);            // 23: interm_fi2_norm
    targetVector.push_back(longImpulseIndicator ? static_cast<float>(longImpulseIndicator->intValue()) : FeatureDefaults::IMPULSE_NEUTRAL);  // 24: impulse_color
    targetVector.push_back(dailyBiasIndicator ? static_cast<float>(dailyBiasIndicator->intValue()) : FeatureDefaults::DAILY_BIAS_NEUTRAL);  // 25: daily_bias

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
    auto longMacd = GetIndicator<Macd>(IndicatorKey::LONG_MACD);
    auto longFI13Signal = GetIndicator<FI13Signal>(IndicatorKey::LONG_FI13_SIGNAL);

    if (!longMacd || !longFI13Signal) {
        return "NA";
    }

    char buffer[32] = {0};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%d|%d",
        longMacd->intValue(),
        longFI13Signal->intValue()
    );
    return std::string(buffer);
}

std::string IndicatorManager::getScreen2EntryText() {
    auto intermFI2Signal = GetIndicator<FI2Signal>(IndicatorKey::INTERM_FI2_SIGNAL);
    auto intermStochastic = GetIndicator<Stochastic>(IndicatorKey::INTERM_STOCHASTIC);
    auto raschkeStrategySetup = GetIndicator<RaschkeStrategyIndicator>(IndicatorKey::RASCHKE_STRATEGY_SETUP);
    auto raschkeTacticalTrigger = GetIndicator<RaschkeTacticalIndicator>(IndicatorKey::RASCHKE_TACTICAL_TRIGGER);

    if (!intermFI2Signal || !intermStochastic || !raschkeStrategySetup || !raschkeTacticalTrigger) {
        return "NA";
    }

    char buffer[48] = {0};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%d|%d|%d|%d",
        intermFI2Signal->intValue(),
        intermStochastic->intValue(),
        static_cast<int>(raschkeStrategySetup->Value()),
        static_cast<int>(raschkeTacticalTrigger->Value())
    );
    return std::string(buffer);
}

std::string IndicatorManager::getScreen3EntryText() {
    auto structureTest = GetIndicator<StructureTestIndicator>(IndicatorKey::STRUCTURE_TEST);
    auto volume = GetIndicator<VolumeIndicator>(IndicatorKey::VOLUME_SIGNAL);
    auto atrProximity = GetIndicator<ATRProximityIndicator>(IndicatorKey::ATR_PROXIMITY);
    auto dailyBias = GetIndicator<DailyBiasIndicator>(IndicatorKey::DAILY_BIAS);

    if (!structureTest || !volume || !atrProximity || !dailyBias) {
        return "NA";
    }

    char buffer[48] = {0};
    std::snprintf(
        buffer,
        sizeof(buffer),
        "%d|%d|%d|%d",
        structureTest->intValue(),
        volume->intValue(),
        atrProximity->intValue(),
        static_cast<int>(dailyBias->Value())
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

void IndicatorManager::PopulateIndicatorState(MTS::Schema::IndicatorState& state) const {

    // Iterate all indicators and send FULL SNAPSHOT
    for (size_t i = 0; i < m_indicators.size(); ++i) {
        auto* indicator = m_indicators[i];
        if (!indicator) continue;

        // Force extraction of current value using ExtractInt8AndClearDirty()
        // This clears dirty flag as side effect, fulfilling contract
        int8_t value = indicator->ExtractInt8AndClearDirty();

        switch (static_cast<IndicatorKey>(i)) {
            case IndicatorKey::LONG_MACD: state.mutate_long_macd(value); break;
            case IndicatorKey::LONG_FI13_SIGNAL: state.mutate_long_fi13_signal(value); break;
            case IndicatorKey::LONG_MACD_DIVERGENCE: state.mutate_long_macd_divergence(value); break;
            case IndicatorKey::LONG_IMP: state.mutate_long_imp(value); break;
            case IndicatorKey::INTERM_STOCHASTIC: state.mutate_interm_stochastic(value); break;
            case IndicatorKey::RASCHKE_STRATEGY_SETUP: state.mutate_raschke_strategy_setup(value); break;
            case IndicatorKey::RASCHKE_TACTICAL_TRIGGER: state.mutate_raschke_tactical_trigger(value); break;
            case IndicatorKey::RSI: state.mutate_rsi(value); break;
            case IndicatorKey::INTERM_FI2_SIGNAL: state.mutate_interm_fi2_signal(value); break;
            case IndicatorKey::EMA_PROXIMITY: state.mutate_ema_proximity(value); break;
            case IndicatorKey::PRICE_METRICS: state.mutate_price_metrics(value); break;
            case IndicatorKey::INTERM_MACD_DIVERGENCE: state.mutate_interm_macd_divergence(value); break;
            case IndicatorKey::INTERM_MACD: state.mutate_interm_macd(value); break;
            case IndicatorKey::INTERM_IMP: state.mutate_interm_imp(value); break;
            case IndicatorKey::STRUCTURE_TEST: state.mutate_structure_test(value); break;
            case IndicatorKey::VOLUME_SIGNAL: state.mutate_volume_signal(value); break;
            case IndicatorKey::ATR_PROXIMITY: state.mutate_atr_proximity(value); break;
            case IndicatorKey::DAILY_BIAS:
                state.mutate_daily_bias(value);
                state.mutate_daily_bias_enum(value);
                break;
            case IndicatorKey::TIME_OF_DAY: state.mutate_time_of_day(value); break;
            case IndicatorKey::KANGAROO_TAIL: state.mutate_kangaroo_tail(value); break;
            case IndicatorKey::TURTLE_SOUP: state.mutate_turtle_soup(value); break;
            case IndicatorKey::MOMENTUM_PINBALL: state.mutate_momentum_pinball(value); break;
            case IndicatorKey::ELDER_BREAKOUT: state.mutate_elder_breakout(value); break;
            case IndicatorKey::NR7: state.mutate_nr7(value); break;
            case IndicatorKey::NH_NL_SIGNAL: state.mutate_nh_nl_signal(value); break;
            case IndicatorKey::OSCILLATOR_310: state.mutate_oscillator_310(value); break;
            // ZN_TREND / DX_TREND: deferred to future release.
            // Training path (MapIndicatorKeyToTrainingEvent) already skips these.
            // Live path now consistent: fall through to default.
            default: break;
        }
    }
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
template Ema* IndicatorManager::GetIndicator<Ema>(IndicatorKey key) const;
template Macd* IndicatorManager::GetIndicator<Macd>(IndicatorKey key) const;
template MACDDivergence* IndicatorManager::GetIndicator<MACDDivergence>(IndicatorKey key) const;
template Stochastic* IndicatorManager::GetIndicator<Stochastic>(IndicatorKey key) const;
template FI13Signal* IndicatorManager::GetIndicator<FI13Signal>(IndicatorKey key) const;
template FI2Signal* IndicatorManager::GetIndicator<FI2Signal>(IndicatorKey key) const;
template Side* IndicatorManager::GetIndicator<Side>(IndicatorKey key) const;
template IntermediateMarketAction* IndicatorManager::GetIndicator<IntermediateMarketAction>(IndicatorKey key) const;
template LongMarketAction* IndicatorManager::GetIndicator<LongMarketAction>(IndicatorKey key) const;
template ShortMarketAction* IndicatorManager::GetIndicator<ShortMarketAction>(IndicatorKey key) const;
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
template HmmStateIndicator* IndicatorManager::GetIndicator<HmmStateIndicator>(IndicatorKey key) const;
template MarketClimateIndicator* IndicatorManager::GetIndicator<MarketClimateIndicator>(IndicatorKey key) const;
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
template AdxIndicator* IndicatorManager::GetIndicator<AdxIndicator>(IndicatorKey key) const;
template VwapIndicator* IndicatorManager::GetIndicator<VwapIndicator>(IndicatorKey key) const;
