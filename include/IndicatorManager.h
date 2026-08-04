#pragma once

#include <sdkddkver.h>

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <map>
#include <array>
#include <deque>

#include "MindfulTraderConstants.h"
#include "sierrachart.h"
#include "Indicator.h"

// DataCollector removed (Jan 24, 2026 - event pipeline now handled by EventDataCollectorStudy)

class IndicatorManager {
public:
    template <typename T>
    T* GetIndicator(IndicatorKey key) const;

    void InitializeHotPathCache();
    void Reset();
    void UpdateBarContext(SCStudyInterfaceRef sc);

    bool HasSignificantChange();
    void ClearDirtyMask() { m_dirty_mask = 0; }
    std::string getScreen1EntryText();
    std::string getScreen2EntryText();
    std::string getScreen3EntryText();
    static IndicatorManager& Instance();

    // --- Elite v2.3+ FlatBuffer API ---
    std::unique_ptr<MTS::Training::TrainingEventT> GetTrainingEventT(SCStudyInterfaceRef sc);  // Elite v2.4: Return object directly
    void SyncFeatureVector(std::vector<float>& targetVector) const;

    // --- Elite v2.4: Event-Driven FlatBuffer Publishing (SCStudies.cpp migration) ---
    // PublishEventOnChange: Check HasSignificantChange() and publish FlatBuffer Event if triggered
    // Returns true if event was published, false otherwise
    bool PublishEventOnChange(SCStudyInterfaceRef sc);

    // PublishEventSnapshot: Force publish full Event snapshot regardless of change detection
    // Used on GUI connection to synchronize state
    void PublishEventSnapshot(SCStudyInterfaceRef sc);

    bool IsWarmedUp() const { return m_isWarmedUp; }
    void CheckWarmupStatus(SCStudyInterfaceRef sc);  // Called by UpdateBarContext

    float GetCachedPrevDayHigh() const { return m_dailyCache.prevDayHigh; }
    float GetCachedPrevDayLow() const { return m_dailyCache.prevDayLow; }
    float GetCachedValueAreaHigh() const { return m_dailyCache.valueAreaHigh; }
    float GetCachedValueAreaLow() const { return m_dailyCache.valueAreaLow; }

    /// Train/live parity gate (docs/superpowers/plans/2026-08-04-volume-profile-daily-bias.md
    /// final-review fix wave): the real Volume Profile Value Area must stay OFF by default
    /// because the currently-deployed HMM was fitted on the old proxy's semantics. Only the
    /// operator flipping the "Enable Real Volume Profile Daily Bias" ACSIL input (SCStudies.cpp)
    /// turns this on; UpdateDailyCache() checks it before running the real aggregation.
    void SetRealVolumeProfileDailyBiasEnabled(bool enabled) { m_realVolumeProfileDailyBiasEnabled = enabled; }

    // ...existing code...
    Oscillator310* Oscillator310Ptr() const {
        return static_cast<Oscillator310*>(m_indicators[static_cast<size_t>(IndicatorKey::OSCILLATOR_310)]);
    }
    ShortMarketAction* ShortMktAction() const {
        return static_cast<ShortMarketAction*>(m_indicators[static_cast<size_t>(IndicatorKey::SHORT_MKT_ACTION)]);
    }
    StructureTestIndicator* StructureTest() const {
        return static_cast<StructureTestIndicator*>(m_indicators[static_cast<size_t>(IndicatorKey::STRUCTURE_TEST)]);
    }
    VolumeIndicator* Volume() const {
        return static_cast<VolumeIndicator*>(m_indicators[static_cast<size_t>(IndicatorKey::VOLUME_SIGNAL)]);
    }
    PriceMetricsIndicator* PriceMetrics() const {
        return static_cast<PriceMetricsIndicator*>(m_indicators[static_cast<size_t>(IndicatorKey::PRICE_METRICS)]);
    }
    RaschkeTacticalIndicator* RaschkeTactical() const {
        return static_cast<RaschkeTacticalIndicator*>(m_indicators[static_cast<size_t>(IndicatorKey::RASCHKE_TACTICAL_TRIGGER)]);
    }
    DailyBiasIndicator* DailyBias() const {
        return static_cast<DailyBiasIndicator*>(m_indicators[static_cast<size_t>(IndicatorKey::DAILY_BIAS)]);
    }
    const HmmStateIndicator* HmmState() const {
        return InferenceManager::Instance().HmmState();
    }
    const PredictionState* Prediction() const {
        return InferenceManager::Instance().Prediction();
    }
    KangarooTail* KangarooTailPtr() const {
        return static_cast<KangarooTail*>(m_indicators[static_cast<size_t>(IndicatorKey::KANGAROO_TAIL)]);
    }
    MomentumPinball* MomentumPinballPtr() const {
        return static_cast<MomentumPinball*>(m_indicators[static_cast<size_t>(IndicatorKey::MOMENTUM_PINBALL)]);
    }
    ElderBreakout* ElderBreakoutPtr() const {
        return static_cast<ElderBreakout*>(m_indicators[static_cast<size_t>(IndicatorKey::ELDER_BREAKOUT)]);
    }

    const MarketClimateIndicator* MarketClimate() const {
        return InferenceManager::Instance().MarketClimate();
    }

    // Elite v2.4: Populate Event builder with indicator fields (data-driven, schema-ordered)
    // Iterates through indicators in schema field order and maps each to Event builder field
    // Called by EventSerializer to populate all 22 indicator fields
    // Check if inference is required
    bool CalculateRequiresInference() const;

    // Snapshot current dirty bitmask for FlatBuffer changed_mask field.
    // MUST be called BEFORE PopulateIndicatorState (which clears dirty bits).
    [[nodiscard]] uint64_t GetDirtyMask() const { return m_dirty_mask; }

    // Populate the Zero-Copy IndicatorState struct
    void PopulateIndicatorState(MTS::Schema::IndicatorState& state) const;

private:
    IndicatorManager();
    void UpdateDailyCache(SCStudyInterfaceRef sc);
    ~IndicatorManager() = default;

    // Helper: Build Event FlatBuffer and send via pubQueue with size prefix
    // Returns true if sent, false if pubQueue is null or error
    bool SendEventFlatBuffer(SCStudyInterfaceRef sc, bool isSnapshot);

    // Phase 1.2: Static Metaprogramming Dispatcher (Devirtualized Trigger Check)
    // Uses a compile-time switch to call concrete .ShouldTrigger() on m_store members
    // Bypasses vtable lookup for hot-path dirty checking
    bool CheckTrigger(size_t index) const;

    // Phase 1.1: Flat Heterogeneous Store (Avoids allocation/vtable indirection)
    struct IndicatorStore {
        // Screen 1
        Macd long_macd{IndicatorKey::LONG_MACD};
        FI13Signal long_fi13{IndicatorKey::LONG_FI13_SIGNAL};
        MACDDivergence long_macd_div{IndicatorKey::LONG_MACD_DIVERGENCE};
        Impulse long_imp{IndicatorKey::LONG_IMP};
        LongMarketAction long_mkt_action{IndicatorKey::LONG_MKT_ACTION};

        // Screen 2
        Stochastic interm_stochastic{IndicatorKey::INTERM_STOCHASTIC};
        RaschkeStrategyIndicator raschke_strategy{IndicatorKey::RASCHKE_STRATEGY_SETUP};
        RaschkeTacticalIndicator raschke_tactical{IndicatorKey::RASCHKE_TACTICAL_TRIGGER};
        RSIIndicator rsi{IndicatorKey::RSI};
        FI2Signal interm_fi2{IndicatorKey::INTERM_FI2_SIGNAL};
        EmaProximityIndicator ema_prox{IndicatorKey::EMA_PROXIMITY};
        PriceMetricsIndicator price_metrics{IndicatorKey::PRICE_METRICS};
        MACDDivergence interm_macd_div{IndicatorKey::INTERM_MACD_DIVERGENCE};
        Impulse interm_imp{IndicatorKey::INTERM_IMP};
        Macd interm_macd{IndicatorKey::INTERM_MACD};

        // Screen 3
        StructureTestIndicator structure_test{IndicatorKey::STRUCTURE_TEST};
        VolumeIndicator volume{IndicatorKey::VOLUME_SIGNAL};
        VwapIndicator vwap{IndicatorKey::VWAP};
        ATRProximityIndicator atr_prox{IndicatorKey::ATR_PROXIMITY};
        DailyBiasIndicator daily_bias{IndicatorKey::DAILY_BIAS};
        KangarooTail kangaroo_tail{IndicatorKey::KANGAROO_TAIL};
        TurtleSoup turtle_soup{IndicatorKey::TURTLE_SOUP};
        MomentumPinball momentum_pinball{IndicatorKey::MOMENTUM_PINBALL};
        ElderBreakout elder_breakout{IndicatorKey::ELDER_BREAKOUT};
        NR7 nr7{IndicatorKey::NR7};
        ShortMarketAction short_mkt_action{IndicatorKey::SHORT_MKT_ACTION};
        Oscillator310 oscillator_310{IndicatorKey::OSCILLATOR_310};

        // Metadata & State
        Side side{IndicatorKey::SIDE};
        MarketSymbolIndicator market_symbol{IndicatorKey::MARKET_SYMBOL};
        TimeOfDayIndicator time_of_day{IndicatorKey::TIME_OF_DAY};
        OvernightExitIndicator overnight_exit{IndicatorKey::OVERNIGHT_EXIT};
        // HmmStateIndicator, MarketClimateIndicator, PredictionState → moved to InferenceManager (Mar 2026)
        HurstExponentIndicator hurst_exponent{IndicatorKey::HURST_EXPONENT};
        NhNlSignalIndicator nh_nl_signal{IndicatorKey::NH_NL_SIGNAL};

        // Cross-Market Correlations
        CorrelationIndicator corr_es_zn{IndicatorKey::CORR_ES_ZN};
        CorrelationIndicator corr_es_dx{IndicatorKey::CORR_ES_DX};
        CrossMarketTrend zn_trend{IndicatorKey::ZN_TREND};
        CrossMarketTrend dx_trend{IndicatorKey::DX_TREND};

        // Feature Engineering (Derivatives)
        CorrelationIndicator corr_es_zn_delta{IndicatorKey::CORR_ES_ZN_DELTA};
        CorrelationIndicator corr_es_zn_accel{IndicatorKey::CORR_ES_ZN_ACCEL};
        CorrelationIndicator corr_es_dx_delta{IndicatorKey::CORR_ES_DX_DELTA};
        CorrelationIndicator corr_es_dx_accel{IndicatorKey::CORR_ES_DX_ACCEL};
    } m_store;

    // Fast O(1) Lookup Table (Raw Pointers to m_store members, no ownership)
    std::array<BaseIndicator*, static_cast<size_t>(IndicatorKey::MAX_INDICATORS)> m_indicators;

    // Central daily cache (previous trading day's session high/low)
    struct DailyCache {
        int tradingDay = -1;        // Cached trading day (Julian date)
        float prevDayHigh = 0.0f;   // Previous day's session high
        float prevDayLow = 0.0f;    // Previous day's session low
        float valueAreaHigh = 0.0f; // Previous day's real Volume Profile VAH (0.0f = unavailable)
        float valueAreaLow = 0.0f;  // Previous day's real Volume Profile VAL (0.0f = unavailable)
        bool validated = false;     // Validation done once at startup
    };

    DailyCache m_dailyCache;
    bool m_realVolumeProfileDailyBiasEnabled = false;
    bool m_dailyCacheInitialized = false;
    bool m_dailyAnchorZeroWarningLogged = false;

    // m_regimeTenure, m_lastHmmState → InferenceManager (Mar 2026)

    // Elite v2.3: Warmup tracking (Zero-Trap prevention)
    bool m_isWarmedUp = false;          // True after 200 bars + validation
    int m_warmupBarCount = 0;           // Bars processed since initialization
    bool m_warmupLoggedOnce = false;    // Prevent log spam

    // Phase 3.1: Global bitset dirty tracking (53 indicators fit in 64 bits)
    uint64_t m_dirty_mask = 0;

    // Training temporal physics cache (for delta_t_log/tau_100_log parity)
    int64_t m_lastTrainingEventTimestampUs = 0;
    std::deque<int64_t> m_recentTrainingDeltaUs;
    static constexpr size_t kTrainingTauWindowSize = 100;

    // Elite v2.4: Global Event Sequence Counter
    // CRITICAL: Monotonically increasing counter for event ordering
    // Starts at 1, increments for EVERY event (including intrabar)
    // Python uses this to detect dropped events and reconstruct order
    // Unlike sc.Index (bar level), this is truly unique per event
    // Single producer pattern: Sierra Chart UI thread only (no mutex)
    uint64_t m_globalSequenceId = 1;  // Start at 1, 0 reserved for initialization

public:
    /// Current global event sequence counter (monotonically increasing).
    /// Used by semantic staleness: eventsSincePrediction = GlobalSequenceId() - prediction.sequenceId.
    [[nodiscard]] uint64_t GlobalSequenceId() const { return m_globalSequenceId; }
};
