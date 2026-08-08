#pragma once

#include <sdkddkver.h>

#include <string>
#include <string_view>
#include <vector>
#include <memory>
#include <map>
#include <array>

#include "MindfulTraderConstants.h"
#include "sierrachart.h"
#include "Indicator.h"
#include "IndicatorLayout.h"
#include "IndicatorPackedState.h"
#include "RingBuffer.h"

// DataCollector removed (Jan 24, 2026 - event pipeline now handled by EventDataCollectorStudy)

class IndicatorManager {
public:
    template <typename T>
    T* GetIndicator(IndicatorKey key) const;

    void InitializeHotPathCache();
    void Reset();
    void UpdateBarContext(SCStudyInterfaceRef sc);

    /// Refresh the daily cache (prevDayHigh/prevDayLow, real Value Area) for the current trading
    /// day. Idempotent by construction (day-gated internally: a call after the trading day has
    /// already been refreshed this bar is a cheap no-op), so it is safe to call from more than one
    /// study on the same chart. Public (docs/superpowers/plans/2026-08-04-volume-profile-daily-bias.md
    /// final-review fix wave, round 2): TripleScreen3.cpp calls this directly, immediately before its
    /// own CalculateDailyBias(...) read, rather than relying on inferred cross-study
    /// CalculationPrecedence ordering against SCStudies.cpp's call inside UpdateBarContext().
    void UpdateDailyCache(SCStudyInterfaceRef sc);

    bool HasSignificantChange();
    void ClearDirtyMask() { m_dirty_mask = 0; }
    std::string getScreen1EntryText();
    std::string getScreen2EntryText();
    std::string getScreen3EntryText();
    static IndicatorManager& Instance();

    // --- Elite v2.3+ FlatBuffer API ---
    // Tier 2 (docs/superpowers/specs/2026-08-07-training-event-export-dod-
    // design.md): returns a non-owning pointer to the pooled
    // m_trainingEventScratch member (previously a fresh std::make_unique
    // per call) -- always non-null today, same as before this change; the
    // one caller (EventDataCollectorStudy.cpp) already treats the result as
    // "check for null, then use via ->/*", which means the same thing for a
    // raw pointer as it did for a unique_ptr.
    MTS::Training::TrainingEventT* GetTrainingEventT(SCStudyInterfaceRef sc);
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

    // Short-term price extremes: previous completed 15-min bar high/low, 3-bar
    // max/min swing extremes, and prior 20-bar-window Turtle Soup lookback extremes.
    // Extracted from ShortMarketAction (docs/superpowers/specs/2026-08-06-indicator-
    // orphan-cleanup-design.md §3.2): that class's own PriceActionEnum classification
    // was dead (no live/training writer), but its object doubled as storage for these
    // six genuinely-live values. Not tied to any IndicatorKey.
    void SetShortTermPriceExtremes(float prevHigh, float prevLow, float maxHigh, float minLow) {
        m_shortTermExtremes.prevHigh = prevHigh;
        m_shortTermExtremes.prevLow = prevLow;
        m_shortTermExtremes.maxHigh = maxHigh;
        m_shortTermExtremes.minLow = minLow;
    }
    void SetPrevFourBarExtremes(float prevFourBarHigh, float prevFourBarLow) {
        m_shortTermExtremes.prevFourBarHigh = prevFourBarHigh;
        m_shortTermExtremes.prevFourBarLow = prevFourBarLow;
    }
    float GetPrevHigh() const { return m_shortTermExtremes.prevHigh; }
    float GetPrevLow() const { return m_shortTermExtremes.prevLow; }
    float GetMaxHigh() const { return m_shortTermExtremes.maxHigh; }
    float GetMinLow() const { return m_shortTermExtremes.minLow; }
    float GetPrevFourBarHigh() const { return m_shortTermExtremes.prevFourBarHigh; }
    float GetPrevFourBarLow() const { return m_shortTermExtremes.prevFourBarLow; }

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
    // Stale-comment fix (indicator-manager-dod-soa plan, Task 9): this no
    // longer needs to precede PopulateIndicatorState. Task 9 rewrote
    // PopulateIndicatorState to read m_packed directly (GetValue<Key>()) with
    // no dirty-clearing side effect at all, unlike the old ExtractInt8AndClearDirty()-
    // based loop this comment used to warn about. Callers still snapshot the
    // mask before publish for clarity, but the ordering is no longer load-bearing.
    [[nodiscard]] uint64_t GetDirtyMask() const { return m_dirty_mask; }

    // Populate the Zero-Copy IndicatorState struct
    void PopulateIndicatorState(MTS::Schema::IndicatorState& state) const;

    /// One canonical per-tick gather of companion values (Task 10,
    /// indicator-manager-dod-soa plan) shared by the live Event path
    /// (EventSerializer.cpp), the training TrainingEvent path
    /// (GetTrainingEventT), and BackTesterStudy.cpp's entry-context capture.
    /// Replaces three independent GetIndicator<T>()->GetX() call sites with
    /// one read, fixing the close_percentile dead-write bug at the source.
    /// `side` is sourced directly from PositionManager::GetTradeSide() — see
    /// the definition's comment for why.
    TickCompanionValues GetTickCompanionValues() const;

    // --- indicator-manager-dod-soa plan, Task 5: compile-time packed-array
    // accessors (design spec §3.3). Two forms:
    //   - Single-row form (the common case): resolves via
    //     mts::UniqueDescriptorFor(Key), which only succeeds for keys with
    //     exactly one packed row. Keys with zero or two rows fail the
    //     static_assert below, directing the caller to the explicit-block form.
    //   - Explicit-block form: required for the ~15 keys that contribute two
    //     packed rows (one Int8, one Float32 companion) — Task 6 (Macd) is
    //     this overload's first real caller.
    template <IndicatorKey Key>
    auto GetValue() const {
        constexpr auto desc = mts::UniqueDescriptorFor(Key);
        static_assert(desc.block != mts::StorageBlock::NotPacked,
                      "IndicatorKey has zero or two rows — use GetValue<Key, Block>() instead");
        if constexpr (desc.block == mts::StorageBlock::Int8) {
            return m_packed.GetI8(desc.position);
        } else {
            return m_packed.GetF32(desc.position);
        }
    }

    template <IndicatorKey Key, typename V>
    void SetValue(V value) {
        constexpr auto desc = mts::UniqueDescriptorFor(Key);
        static_assert(desc.block != mts::StorageBlock::NotPacked,
                      "IndicatorKey has zero or two rows — use SetValue<Key, Block>() instead");
        constexpr uint64_t keyBit = 1ULL << static_cast<uint64_t>(Key);
        if constexpr (desc.block == mts::StorageBlock::Int8) {
            m_packed.SetI8(desc.position, static_cast<int8_t>(value), keyBit);
        } else {
            m_packed.SetF32(desc.position, static_cast<float>(value), keyBit);
        }
    }

    template <IndicatorKey Key, mts::StorageBlock Block>
    auto GetValue() const {
        constexpr auto desc = mts::DescriptorFor(Key, Block);
        static_assert(desc.block != mts::StorageBlock::NotPacked, "no row for this (key, block) pair");
        if constexpr (Block == mts::StorageBlock::Int8) {
            return m_packed.GetI8(desc.position);
        } else {
            return m_packed.GetF32(desc.position);
        }
    }

    template <IndicatorKey Key, mts::StorageBlock Block, typename V>
    void SetValue(V value) {
        constexpr auto desc = mts::DescriptorFor(Key, Block);
        static_assert(desc.block != mts::StorageBlock::NotPacked, "no row for this (key, block) pair");
        constexpr uint64_t keyBit = 1ULL << static_cast<uint64_t>(Key);
        if constexpr (Block == mts::StorageBlock::Int8) {
            m_packed.SetI8(desc.position, static_cast<int8_t>(value), keyBit);
        } else {
            m_packed.SetF32(desc.position, static_cast<float>(value), keyBit);
        }
    }

private:
    IndicatorManager();
    ~IndicatorManager() = default;

    // Helper: Build Event FlatBuffer and send via pubQueue with size prefix
    // Returns true if sent, false if pubQueue is null or error
    bool SendEventFlatBuffer(SCStudyInterfaceRef sc, bool isSnapshot);

    // Phase 1.2: Static Metaprogramming Dispatcher (Devirtualized Trigger Check)
    // Uses a compile-time switch to call concrete .ShouldTrigger() on m_store members
    // Bypasses vtable lookup for hot-path dirty checking
    bool CheckTrigger(size_t index) const;

#ifndef NDEBUG
    // Debug-only safety net for the Task 4 dual-write (indicator-manager-dod-soa
    // plan): verifies every Int8-block primary value in m_packed matches the
    // legacy m_store/m_indicators path. Called once per tick after that tick's
    // indicator updates have settled. Should never fire — nothing depends on
    // m_packed's correctness yet except this assertion itself.
    void AssertPackedStateParity() const;
#endif

    // Phase 1.1: Flat Heterogeneous Store (Avoids allocation/vtable indirection)
    struct IndicatorStore {
        // Screen 1
        Macd long_macd{IndicatorKey::LONG_MACD};
        FI13Signal long_fi13{IndicatorKey::LONG_FI13_SIGNAL};
        MACDDivergence long_macd_div{IndicatorKey::LONG_MACD_DIVERGENCE};
        Impulse long_imp{IndicatorKey::LONG_IMP};

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
        // Own object/key -- previously wrongly aliased onto ShortMarketAction's
        // storage via GetIndicator<IntermediateMarketAction>(SHORT_MKT_ACTION).
        IntermediateMarketAction interm_mkt_action{IndicatorKey::INTERM_MKT_ACTION};
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

    // Phase II DOD/SoA migration (indicator-manager-dod-soa plan, Task 4):
    // dual-write target alongside m_store above. Nothing reads through this
    // yet — purely additive until a later task migrates callers off m_store.
    mts::IndicatorPackedState<mts::kIndicatorLayoutI8Count, mts::kIndicatorLayoutF32Count> m_packed;

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

    // Short-term price extremes -- decoupled companion storage extracted from
    // ShortMarketAction (docs/superpowers/specs/2026-08-06-indicator-orphan-cleanup-
    // design.md §3.2): that class's own PriceActionEnum classification was dead (no
    // live/training writer), but its object doubled as storage for these six
    // genuinely-live values (Event schema prev_high/prev_low/prev_four_bar_high/
    // prev_four_bar_low, TradeExecutionServer's swing-high/low market context).
    // Not tied to any IndicatorKey. All six fields are written exclusively from
    // TripleScreen3.cpp (TS3, the 15-minute chart) -- this is TS3-only data, not a
    // cross-timeframe aggregate.
    struct ShortTermPriceExtremes {
        float prevHigh = 0.0f;         // Previous completed 15-min bar high
        float prevLow = 0.0f;          // Previous completed 15-min bar low
        float maxHigh = 0.0f;          // Max of last 3 completed 15-min bars (swing-high proxy)
        float minLow = 0.0f;           // Min of last 3 completed 15-min bars (swing-low proxy)
        float prevFourBarHigh = 0.0f;  // Highest of prior 20-bar (15-min) window (Turtle Soup lookback)
        float prevFourBarLow = 0.0f;   // Lowest of prior 20-bar (15-min) window (Turtle Soup lookback)
    };

    ShortTermPriceExtremes m_shortTermExtremes;

    // m_regimeTenure, m_lastHmmState → InferenceManager (Mar 2026)

    // Elite v2.3: Warmup tracking (Zero-Trap prevention)
    bool m_isWarmedUp = false;          // True after 200 bars + validation
    int m_warmupBarCount = 0;           // Bars processed since initialization
    bool m_warmupLoggedOnce = false;    // Prevent log spam

    // Phase 3.1: Global bitset dirty tracking (53 indicators fit in 64 bits)
    uint64_t m_dirty_mask = 0;

    // indicator-manager-dod-soa plan, whole-branch-review fix: tracks the
    // trade side as of the last UpdateBarContext() tick so a flip can be
    // detected and mirrored into m_dirty_mask's SIDE bit (see UpdateBarContext
    // in IndicatorManager.cpp for the writer).
    TradeSideEnum m_lastKnownSide = TradeSideEnum::FLAT;

    // Training temporal physics cache (for delta_t_log/tau_100_log parity).
    // Fixed-capacity ring buffer (docs/superpowers/specs/2026-08-07-training-
    // event-export-dod-design.md, Tier 1) -- zero heap allocation, replacing
    // std::deque's ongoing chunk churn as the window slides on every
    // significant-change event. Capacity = kTrainingTauWindowSize + 1 for
    // headroom (push_back-then-conditionally-pop_front transiently overshoots
    // by one, same convention as every other RingBuffer conversion this repo
    // has made).
    int64_t m_lastTrainingEventTimestampUs = 0;
    static constexpr size_t kTrainingTauWindowSize = 100;
    RingBuffer<int64_t, kTrainingTauWindowSize + 1> m_recentTrainingDeltaUs;

    // Tier 2 (docs/superpowers/specs/2026-08-07-training-event-export-dod-
    // design.md): pooled TrainingEventT scratch object, reused across every
    // GetTrainingEventT() call instead of freshly std::make_unique'd -- its
    // three nested heap-owning fields (indicators/observation/
    // asymmetry_context) are allocated once (lazily, on first use) and never
    // freed until IndicatorManager itself is destroyed. GetTrainingEventT()
    // returns a non-owning pointer to this member; every scalar field is
    // confirmed unconditionally overwritten on every call (see the spec's
    // §3.2 write-site audit) before the pointer is handed to the caller, so
    // no data from a previous call is ever visible.
    MTS::Training::TrainingEventT m_trainingEventScratch;

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
