#pragma once

#include "sierrachart.h"
#include <optional>
#include <array>
#include <cmath>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include "generated/mts_schema_contract_generated.h"
#include "InformationEngine.h"
#include "TailRiskEngine.h"
#include "StructureEngine.h"
#include "EventVelocityEngine.h"
#include "FreshnessGateEngine.h"
#include "FeatureScaler.h"
#include "RingBuffer.h"

// Shannon flow entropy is measured in BITS (range 0 .. log2(NUM_BINS)). Several gates
// were authored with thresholds that read like normalized [0,1] ratios of H/Hmax but
// compared the RAW bits value (Finding 18) — mis-firing. Multiply a normalized threshold
// by this constant to compare it correctly against the bits value. Hmax = log2(10).
inline constexpr float kShannonMaxEntropyBits = 3.321928f;
static_assert(MindfulTrader::InformationEngine::NUM_BINS == 10,
              "kShannonMaxEntropyBits must equal log2(InformationEngine::NUM_BINS)");

struct StatisticalContext {
    float volatility = 0.0f;              ///< Std dev of log returns (volatility measure)
    float efficiency = 0.0f;              ///< Efficiency ratio [0-1] (trending vs ranging)
    float relRange = 0.0f;                ///< Bar range / ATR (dimensionless volatility)
    float velocity = 0.0f;                ///< Oscillator momentum (price action acceleration)
    SCDateTime lastUpdated;
    int regimeTenure = 0;                 ///< TS2 bar-closes in current regime (exported as regime_tenure / bars_since_last_update)
};

struct NormalizedAnchors {
    // Screen 3 statistical anchors feeding ContextManager's canonical ObservationData mutators.

    // Information metrics.
    float pathEfficiencySNR = 0.0f;     ///< Path directness signal-to-noise proxy (Elder's ER²).
    float hurstExponent = 0.0f;         ///< Multi-scale persistence estimate.
    float microAsymmetry = 0.0f;        ///< Normalized order-flow imbalance.

    // Fragility metrics.
    float realizedKurtosis = 0.0f;      ///< 4th moment (fat-tail detector).
    float skewnessIdx = 0.0f;           ///< 3rd moment directional asymmetry.
    float amihudIlliquidity = 0.0f;      ///< Amihud illiquidity (|r_t|/V_t).
    float spreadStress = 0.0f;           ///< Spread-stress fragility estimate.

    // Structural anchors.
    float distDayHigh = 0.0f;           ///< Distance to previous day high (ATR-normalized).
    float distDayLow = 0.0f;            ///< Distance to previous day low (ATR-normalized).
    float distFourBarHigh = 0.0f;       ///< Distance to local swing high (ATR-normalized).
    float distFourBarLow = 0.0f;        ///< Distance to local swing low (ATR-normalized).
    float distEma13 = 0.0f;             ///< Distance to EMA13 (ATR-normalized).

    // Regime context carried on Event/TrainingEvent top-level fields.
    float nhNlDaily = 0.0f;             ///< New highs minus new lows index.
    float dailyBias = 0.0f;             ///< Directional bias proxy in [-1, 1].

    // Supplemental Internal Features
    float logRelativeVolume = 0.0f;     ///< Log-normalized relative volume (participation)

    SCDateTime lastUpdated;
};

struct DailyCache {
    float prevDayHigh = 0.0f;
    float prevDayLow = 0.0f;
    int tradingDay = 0;
    bool validated = false;
};

/// Elite v3.2: Unified Local Risk Context
/// Single struct exposing all locally-computed Gang (Shannon/Taleb/Pareto) intelligence
/// to RiskManager, PositionManager, and Scoring without Python round-trip.
/// Replaces the fragmented ClimateMetrics ↔ InstitutionalMetrics ↔ NormalizedAnchors paths.
struct LocalRiskContext {
    // Shannon (Information Theory) — from InformationEngine
    float shannonFlowEntropy = 0.0f;   // bits — flow entropy (disambiguated from schema-level Shannon)
    float shannonEfficiency = 0.5f;    // 1 - H/Hmax — signal-to-noise ratio

    // Taleb (Tail Risk) — from NormalizedAnchors + TailRiskEngine
    float talebKurtosis = 0.0f;        // realized 4th moment
    float talebSkewness = 0.0f;        // realized 3rd moment
    float elderChandelierATR = 0.0f;   // Elder's Chandelier: (price - stop) / ATR distance
    float paretoTailAlpha = 4.0f;      // Pareto tail index via Hill estimator (4.0 = safe default)

    // Pareto (Structure/Flow) — from StructureEngine + ObservationData
    float amihudIlliquidity = 0.0f;    // Amihud illiquidity (canonical: mean |log-ret| / dollar-volume; formerly 'vpin')
    float amihudPercentile = 0.5f;     // Layer B: session-aware rolling percentile [0,1] of amihudIlliquidity — the actual gate input (p90 normal / p75 fat-tail)
    float spreadStress = 0.0f;         // spread-stress fragility
    float hurstExponent = 0.5f;        // persistence (>0.5 trending, <0.5 mean-reverting)
    float fractalDim = 1.5f;           // roughness 1.0-2.0
    float meanRevZ = 0.0f;             // distance from mean in sigma

    // Raschke (Event Dynamics)
    float raschkeBurst = 1.0f;         // CV of inter-arrival times (clustering)

    // Fisher (Observation Reliability)
    float fisherInfo = 0.0f;           // sharpness of current estimate

    // Regime Duration (from MarketClimateIndicator)
    int regimeDuration = 0;            // bars in current climate state

    bool isValid = false;              // true after warmup
    uint64_t snapshotTimestampUs = 0;  // D.6: Wall-clock time (system_clock, µs) when context was last refreshed
};

/// Diagnostics for HMM trigger decisions (institutional-grade observability)
struct HMMTriggerDiagnostics {
    float l1_norm;                      ///< L1-norm distance from baseline
    float noise_floor_used;             ///< Adaptive noise floor applied (velocity-adjusted)
    float mahalanobis_distance;         ///< Diagonal Mahalanobis distance from rolling observation distribution
    float energy_mahalanobis;           ///< Mahalanobis contribution from energy channels only
    float geometry_mahalanobis;         ///< Mahalanobis contribution from geometry channels only
    float energy_contribution_share;    ///< Energy contribution share in [0,1] of Mahalanobis distance-squared
    float mahalanobis_epsilon;          ///< Adaptive epsilon threshold used for Mahalanobis trigger
    float event_velocity_per_sec;       ///< Events per second in observation window
    bool significant_change;            ///< True if Mahalanobis distance >= adaptive epsilon
    bool energy_fast_track;             ///< True when energy channel z-move bypassed normal gate
    bool training_forced;               ///< True if isDataCollection flag forced update
    bool hmm_success;                   ///< True if HMMClient request succeeded
    uint64_t trigger_time_us;           ///< Timestamp of trigger decision (UNIX microseconds)
    uint64_t last_update_age_us;        ///< Time since last successful HMM update
    uint32_t sequence_id;               ///< Sequence number of HMM request

    HMMTriggerDiagnostics()
        : l1_norm(0.0f),
          noise_floor_used(0.0f),
                    mahalanobis_distance(0.0f),
                                        energy_mahalanobis(0.0f),
                                        geometry_mahalanobis(0.0f),
                                        energy_contribution_share(0.0f),
                    mahalanobis_epsilon(0.0f),
          event_velocity_per_sec(0.0f),
          significant_change(false),
                    energy_fast_track(false),
          training_forced(false),
          hmm_success(false),
          trigger_time_us(0),
          last_update_age_us(0),
          sequence_id(0)
    {}
};

class ContextManager {
public:
    // Institutional-grade constants (declared early for use in return types)
    static constexpr size_t OBSERVATION_VECTOR_SIZE = MTS::Schema::Contract::kObservationDim;
    static constexpr size_t ASYMMETRY_CONTEXT_SIZE = MTS::Schema::Contract::kAsymmetryDim;

    static ContextManager& Instance();

    /// Set Wave context (TS2 ownership): `volatility` and `efficiency` as primary fields.
    void SetWaveContext(StatisticalContext&& ctx);

    /// Set Ripple context (TS3 ownership): `relRange` and `velocity` as primary fields.
    void SetRippleContext(StatisticalContext&& ctx);

    // [New] Dimensional Physics Accessors (Directly to ObservationData)
    MTS::Schema::ObservationData* GetMutableObservation() { return &m_observationData; }
    const MTS::Schema::ObservationData& GetObservationData() const { return m_observationData; }

    /// Feeds real-time event log-return into Phase 2/3 Engines (Information + Tail)
    /// Validates and compresses observations via Phase 1 Engine (PCA)
    void UpdateMarketPhysics(float logReturn);

    /// Set normalized anchors (price distances to support/resistance levels) from Screen3 (15-min bars)
    void SetNormalizedAnchors(NormalizedAnchors&& anchors);

    /// Elite v3.0: Update StructureEngine + structural risk metrics on bar updates.
    /// TS2 remains sole writer for ObservationData structural dims (fractal/recurrence).
    /// Also populates elderChandelierATR (Chandelier Stop distance) from sc bar history.
    /// @param isNewBar: true if this is a new closed bar to commit
    void UpdatePriceStructure(SCStudyInterfaceRef sc, float high, float low, float close, bool isNewBar);

    /// Set daily cache (previous day's high/low for normalized anchor calculations)
    void SetDailyCache(const DailyCache& cache);

    /// Get current statistical context (volatility, efficiency, etc.)
    std::optional<StatisticalContext> GetStatisticalContext() const;

    /// Get current normalized anchors (price distances to technical levels)
    std::optional<NormalizedAnchors> GetNormalizedAnchors() const;

    /// Get the latest hybrid-scaled observation vector.
    /// State/geometry dims are Soft-Log-Z and energy dims are bounded Log-Z.
    std::array<float, OBSERVATION_VECTOR_SIZE> GetLatestScaledObservation() const;

    /// Returns the 8D Asymmetry Context (Transformer Gate)
    /// NOT scaled - these are raw metrics for embedding lookup.
    MTS::Schema::AsymmetryContext GetAsymmetryContext() const;

    /// Latest computed event velocity (events/sec) from CheckAndTriggerHMM.
    float GetLastEventVelocityPerSec() const;

    /// Check if HMM update is needed and trigger on init/significant state change
    /// @param now_us Current time in microseconds (UNIX timestamp)
    /// @param isDataCollection If true, force HMM updates during data collection to diversify training labels
    /// @param syntheticVelocity If >= 0, use this as event velocity instead of
    ///        timestamp-based calculation. Used by replay/data-collection to inject
    ///        bar-derived trade rate (NumberOfTrades / SecondsPerBar).
    /// @param isPostWeekendReopenGrace If true, bypasses the TS1/TS2 staleness check
    ///        (see AreTs1DimsReady/AreTs2StructuralDimsReady) for the post-weekend-reopen
    ///        grace window, since the market was closed and stale dims are the best
    ///        available approximation. Default false preserves today's strict behavior.
    void CheckAndTriggerHMM(uint64_t now_us, bool isDataCollection, float syntheticVelocity = -1.0f,
                             bool isPostWeekendReopenGrace = false);

    /// Get diagnostics from the last HMM trigger decision (for observability and debugging)
    /// Useful for understanding label generation patterns, monitoring HMM health
    std::optional<HMMTriggerDiagnostics> GetLastTriggerDiagnostics() const;

    /// True when observation stream has reached event-driven saturation readiness.
    /// Use this as deterministic gate for collection/export paths.
    bool IsObservationSaturated() const;

    /// Current number of observation samples accumulated toward saturation.
    size_t GetObservationSampleCount() const;

    /// Mark TS1 macro dims (0/6/8/9) as freshly committed at replay timestamp
    /// and process-monotonic timestamp.
    /// Called by TS1 writer only after atomic finite commit.
    void MarkTs1MacroDimsFresh(uint64_t timestamp_us);

    /// Returns true when TS1 macro dims are finite, non-degenerate, and fresh.
    /// max_age_us bounds accepted staleness from the last TS1 macro commit.
    /// bypassCheck (post-weekend-reopen grace) skips the staleness check only.
    bool AreTs1DimsReady(uint64_t now_us, uint64_t max_age_us, bool bypassCheck = false) const;

    /// Age of last TS1 macro commit using replay/live chart timeline (microseconds).
    uint64_t GetTs1MacroAgeUs(uint64_t now_us) const;

    /// Last replay timestamp when TS1 macro dims were atomically committed.
    uint64_t GetTs1MacroLastWriteUs() const;

    /// Mark TS2 structural dims (recurrence/fractal) as freshly committed at replay timestamp
    /// and process-monotonic timestamp.
    /// Called by TS2 writer only after finite/range-valid commit.
    void MarkTs2StructuralDimsFresh(uint64_t timestamp_us);

    /// Returns true when TS2 structural dims are finite, in-contract bounds, and fresh.
    /// max_age_us bounds accepted staleness from the last TS2 structural commit.
    /// bypassCheck (post-weekend-reopen grace) skips the staleness check only.
    bool AreTs2StructuralDimsReady(uint64_t now_us, uint64_t max_age_us, bool bypassCheck = false) const;

    /// Age of last TS2 structural commit using replay/live chart timeline (microseconds).
    uint64_t GetTs2StructuralAgeUs(uint64_t now_us) const;

    /// Last replay timestamp when TS2 structural dims were atomically committed.
    uint64_t GetTs2StructuralLastWriteUs() const;

    /// True once TS2 has produced at least one post-reset structural write.
    bool HasTs2SeenAfterReset() const {
        return m_ts2SeenAfterReset.load(std::memory_order_relaxed);
    }

    /// Monotonic reset generation for cross-study epoch tracing.
    uint64_t GetResetGeneration() const {
        return m_resetGeneration.load(std::memory_order_relaxed);
    }

    /// Required sample count threshold for saturation readiness.
    /// Single authority: FeatureScaler RANK_WINDOW (500 observations).
    static constexpr size_t GetObservationSaturationRequired() { return FeatureScaler::RANK_WINDOW; }

    /// Populate training event with environmental context (SECTION 11: volatility, anchors, observation vector)
    /// Validates and ensures consistent 16D observation payload with BuildObservationVector
    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event, SCStudyInterfaceRef sc) const;

    /// Build 16D observation vector from current environmental state (Institutional Physics-Based)
    /// Used by both live inference and training data collection
    std::array<float, OBSERVATION_VECTOR_SIZE> BuildObservationVector();

private:
    ContextManager() = default;
    ~ContextManager() = default;

public:
    /// Reset all state (hard epoch boundary for chart-derived data).
    /// After reset, TS1/TS2 readiness requires fresh post-reset producer writes.
    /// reset_reference_time_us should be the replay/live chart timestamp at arm-time.
    /// If provided, preserved TS1/TS2 ownership snapshots are anchored to this epoch.
    void Reset(uint64_t reset_reference_time_us = 0);

public:
    // Institutional-grade constants
    static constexpr float NOISE_FLOOR = 0.005f;                    ///< Base L1-norm threshold
    static constexpr float NOISE_FLOOR_LOW_VELOCITY_MULT = 1.5f;    ///< Conservative multiplier (<3 evt/s)
    static constexpr float NOISE_FLOOR_HIGH_VELOCITY_MULT = 0.85f;  ///< Reactive multiplier (>8 evt/s)
    static constexpr float VELOCITY_LOW = 3.0f;                     ///< Low velocity threshold (evt/sec)
    static constexpr float VELOCITY_HIGH = 8.0f;                    ///< High velocity threshold (evt/sec)
    static constexpr int EVENT_VELOCITY_WINDOW_SEC = 2;              ///< 2-second window for event counting
    static constexpr size_t EVENT_VELOCITY_MAX = 100;                ///< Max events to track in deque
    static constexpr float ANCHOR_CLAMP_MIN = -50.0f;                ///< Min clamp for anchor values (points)
    static constexpr float ANCHOR_CLAMP_MAX = 50.0f;                 ///< Max clamp for anchor values (points)
    static constexpr size_t OBS_SATURATION_MIN_SAMPLES = 40;         ///< Event-driven warm-up: minimum observations for covariance stability
    static constexpr float OBS_SATURATION_VAR_EPS = 1e-6f;           ///< Minimum per-dimension variance to treat stream as non-degenerate
    static constexpr float OBS_CHANGE_EPS = 1e-5f;                   ///< Absolute delta threshold to classify a dimension as changed
    static constexpr float OBS_GEOMETRY_CHANGE_MULT = 3.0f;          ///< Geometry dims require stricter change threshold in collection mode
    static constexpr uint64_t OBS_STALENESS_LOG_INTERVAL = 5000;      ///< Emit staleness telemetry every N collection observations
    static constexpr uint64_t OBS_STALENESS_ALERT_RUN = 120;          ///< Consecutive unchanged observations to flag as stale
    static constexpr uint64_t OBS_FRESHNESS_DIGEST_INTERVAL = 10000;  ///< Emit owner-group freshness digest every N collection observations
    static constexpr float ENERGY_FAST_TRACK_Z = 3.0f;                ///< Fast-track trigger z-threshold for energy channels
    static constexpr float ENERGY_TRIGGER_MULT = 0.90f;               ///< Energy channels trigger with lower Mahalanobis multiplier
    static constexpr float GEOMETRY_TRIGGER_MULT = 1.30f;             ///< Geometry channels require higher Mahalanobis multiplier

    /**
    * Observation vector indices for HMM inference (Elite v3.1: 16D Statistical Mechanics Spine).
     *
     * QUADRANT I (Physics): Indices 0-4 (Market Mechanics)
     *   - Volatility, velocity, efficiency, relative range from Screen2 environmental data
     *   - Regime tenure: bars in current trend/regime state
     *
     * QUADRANT II (Information): Indices 5-7 (Market Quality & Confidence)
     *   - Path Efficiency SNR: Signal purity (Elder's ER²) → gates Transformer confidence
     *   - Hurst Exponent: Persistence vs mean-reversion bias
     *   - Microstructure Asymmetry: Buy/sell volume delta → informed flow lead indicator
     *
     * QUADRANT III (Fragility): Indices 8-11 (Taleb Risk Metrics)
     *   - Realized Kurtosis: Fat tail probability (>6.0 = GUARDRAIL: tighten stops)
     *   - Skewness: Directional panic detection (panic liquidation vs accumulation)
     *   - Amihud Illiquidity: |return|/volume ratio (adverse selection proxy)
     *   - Liquidity Fragility: Bid-ask spread inflation (order book thinness stress)
     *
    * QUADRANT IV (Structure): Indices 12-15
    *   - Recurrence, fractal roughness, and mean-reversion elasticity
     *
     * Institutional Strategy: Python FeatureSpine gates predictions based on information quality (Entropy, Hurst, Kurtosis)
     * High-entropy markets rely on Transformer alpha; High-Kurtosis markets lock down position sizing; Skew detects panic flow
     */
    // === Institutional 16D Vector Map (Elite v3.1) ===
    // Aligned with mts_schema.fbs ObservationData struct
    // Quadrant I: Market Mechanics (Energy & Ergodicity)
    static constexpr size_t OBS_LOG_VARIANCE_RATIO = MTS::Schema::Contract::kObsLogVarianceRatio;
    static constexpr size_t OBS_BURSTINESS_INDEX = MTS::Schema::Contract::kObsBurstinessIndex;
    static constexpr size_t OBS_REL_RANGE = MTS::Schema::Contract::kObsRelativeRange;
    static constexpr size_t OBS_CORRECTION_ACTION = MTS::Schema::Contract::kObsCorrectionAction;
    static constexpr size_t OBS_VOL_CONVEXITY = MTS::Schema::Contract::kObsVolConvexity;

    // Quadrant II: Information Theory
    static constexpr size_t OBS_LEMPEL_ZIV = MTS::Schema::Contract::kObsLempelZiv;
    static constexpr size_t OBS_HURST_EXPONENT = MTS::Schema::Contract::kObsHurstExponent;
    static constexpr size_t OBS_MICRO_ASYMMETRY = MTS::Schema::Contract::kObsMicroAsymmetry;
    static constexpr size_t OBS_FISHER_INFO = MTS::Schema::Contract::kObsFisherInfo;

    // Quadrant III: Tail Risk (Fragility)
    static constexpr size_t OBS_TAIL_INDEX = MTS::Schema::Contract::kObsTailIndex;
    static constexpr size_t OBS_SKEWNESS = MTS::Schema::Contract::kObsSkewnessIdx;
    static constexpr size_t OBS_AMIHUD_ILLIQUIDITY = MTS::Schema::Contract::kObsAmihudIlliquidity;
    static constexpr size_t OBS_LIQ_FRAGILITY = MTS::Schema::Contract::kObsLiqFragility;

    // Quadrant IV: Structure (Topological Stability)
    static constexpr size_t OBS_RECURRENCE_RATE = MTS::Schema::Contract::kObsRecurrenceRate;
    static constexpr size_t OBS_FRACTAL_DIM = MTS::Schema::Contract::kObsFractalDim;
    static constexpr size_t OBS_MEAN_REV_Z = MTS::Schema::Contract::kObsMeanRevZ;

    static_assert(OBS_MEAN_REV_Z + 1 == OBSERVATION_VECTOR_SIZE,
                  "ContextManager observation index map must cover canonical ObservationData fields");

    /// P1.4: Expose cached Hill alpha for direct trading gate (not just HMM input).
    /// Updated every BuildObservationVector() call — no hot-path recomputation.
    float GetCachedHillAlpha() const { return m_cachedHillAlpha.load(std::memory_order_relaxed); }

    /// Elite v3.2: Unified local risk context — single source of truth for all
    /// locally-computed Gang intelligence (Shannon/Taleb/Pareto/Raschke/Fisher).
    /// Updated every BuildObservationVector() call.
    const LocalRiskContext& GetLocalRiskContext() const { return m_localRiskContext; }

    /// Update regime duration from MarketClimateIndicator
    void SetRegimeDuration(int bars) { m_localRiskContext.regimeDuration = bars; }

    /// Layer B: push one raw Amihud sample (once per closed 15-min bar) into its
    /// session pool (RTH vs overnight) and refresh the cached percentile stored on
    /// m_localRiskContext.amihudPercentile. isRTH routes the sample; the thin
    /// overnight distribution is kept separate so it does not contaminate the RTH gate.
    void PushAmihudSample(float rawAmihud, bool isRTH);

    /// Layer B: current session-aware Amihud percentile [0,1] (the risk-gate input).
    float GetAmihudPercentile() const { return m_localRiskContext.amihudPercentile; }

private:
    // === Institutional-Grade Physics Engines (Elite v2.5 / v3.0) ===
    MindfulTrader::TailRiskEngine m_tailRiskEngine;       ///< Hill Estimator (Fat Tail detection)
    MindfulTrader::InformationEngine m_infoEngine;        ///< LZ Complexity & KL Divergence
    MindfulTrader::StructureEngine m_structureEngine;     ///< [NEW] Topological Stability (Fractal/Recurrence)

    // Internal State
    float m_lastLogReturn = 0.0f;                         ///< Cached for delayed observation construction (Recurrence Rate)
    std::atomic<float> m_cachedHillAlpha{4.0f};              ///< P1.4: Cached Hill alpha from last BuildObservationVector()
    std::atomic<uint64_t> m_ts1MacroLastSteadyUs{0};         ///< Process-monotonic TS1 commit time (us)
    std::atomic<uint64_t> m_ts2StructuralLastSteadyUs{0};    ///< Process-monotonic TS2 structural commit time (us)
    std::atomic<bool> m_ts1SeenAfterReset{false};            ///< TS1 producer has written since last hard reset
    std::atomic<bool> m_ts2SeenAfterReset{false};            ///< TS2 producer has written since last hard reset
    std::atomic<uint64_t> m_resetGeneration{0};              ///< Monotonic epoch id incremented by Reset()

    // [NEW] Elite v3.0: Institutional metrics cache for observation/context exports
    /// InstitutionalMetrics: high-order mechanics used by 16D observation + 8D asymmetry exports.
    /// Populated inline by UpdateMarketPhysics(), SetNormalizedAnchors(), UpdatePriceStructure().
    struct InstitutionalMetrics {
        float shannonFlowEntropy = 0.0f;
        float shannonEfficiency = 0.5f;
        float talebKurtosis = 0.0f;
        float talebSkewness = 0.0f;
        float elderChandelierATR = 0.0f;
        float paretoRot = 0.0f;
        float raschkeBurst = 0.0f;
        float elderImpulse = 0.0f;
    };
    InstitutionalMetrics m_latestInstitutionalMetrics;

    // Elite v3.2: Unified local risk context (public via GetLocalRiskContext())
    LocalRiskContext m_localRiskContext;

    // === Layer B: session-aware Amihud rolling-percentile estimator ===
    // Two trailing pools so the structurally-higher thin-session (overnight/globex)
    // Amihud does not contaminate the deep-liquidity RTH gate. Sampled once per
    // closed 15-min bar; percentile recomputed on each push and cached on
    // m_localRiskContext.amihudPercentile (gate reads O(1)). Continuous ring
    // (no daily reset). Windows ≈ 21 trading days per session type.
    static constexpr int kAmihudRthWindow = 546;         ///< ~21 RTH days @ 26 15-min bars
    static constexpr int kAmihudOvernightWindow = 1600;  ///< ~21 globex days (thin-session bars)
    static constexpr int kAmihudMinSamples = 30;         ///< min pool size before the gate trusts the percentile
    std::array<float, kAmihudRthWindow> m_amihudRthPool{};
    std::array<float, kAmihudOvernightWindow> m_amihudOvernightPool{};
    int m_amihudRthCount = 0;
    int m_amihudRthHead = 0;
    int m_amihudOvernightCount = 0;
    int m_amihudOvernightHead = 0;

    StatisticalContext m_waveContext;                   ///< Wave context (TS2)
    StatisticalContext m_rippleContext;                 ///< Ripple context (TS3)
    bool m_hasWaveContext = false;
    bool m_hasRippleContext = false;
    std::optional<NormalizedAnchors> m_anchors;
    // [New] Direct FlatBuffers Struct Storage (Single Source of Truth)
    MTS::Schema::ObservationData m_observationData;

    DailyCache m_dailyCache;

    // Regime tenure tracking (institutional-grade)
    int m_regimeTenureCounter = 0;       ///< Incremented each update; reflects bars in current regime state
    float m_prevEfficiency = 0.0f;       ///< Previous efficiency for regime change detection
    float m_prevVolatility = 0.0f;       ///< Previous volatility for regime change detection
    static constexpr float REGIME_CHANGE_THRESHOLD = 0.15f;  ///< Efficiency/volatility % change to reset tenure

    std::array<float, OBSERVATION_VECTOR_SIZE> m_hmmObservation = {};
    MTS::Schema::AsymmetryContext m_asymmetryContext;
    bool m_hmmInitialized = false;
    uint64_t m_lastSequenceId = 0;
    uint64_t m_lastHMMUpdateTimeUS = 0;
    float m_lastEventVelocityPerSec = 0.0f;
    // Fixed-capacity ring buffer (docs/superpowers/specs/2026-08-07-
    // contextmanager-ring-buffer-dod-design.md §3.3, Round 2) -- zero heap
    // allocation, replacing std::deque's ongoing chunk churn as the window
    // slides every tick. Capacity = EVENT_VELOCITY_MAX + 1 for headroom
    // (push_back-then-conditionally-pop_front transiently overshoots by one).
    RingBuffer<uint64_t, EVENT_VELOCITY_MAX + 1> m_eventTimestampsUS;       // Event-arrival timestamps (us); shared by CalculateEventVelocity() and CalculateBurstinessIndex()
    eve::VelocityState m_velocityState;              // EMA-based event velocity (Task 1, phase1-hardening plan)
    // Structure-of-arrays: one fixed-capacity ring buffer per dimension,
    // mirroring FeatureScaler's own stateBuffers/logBuffers layout
    // (docs/superpowers/specs/2026-08-07-contextmanager-ring-buffer-dod-design.md
    // §3.4, Round 2) -- was a single std::deque<std::array<float,16>> (AoS),
    // read every tick via a transposed per-dim stride in
    // ComputeTriggerDecisionMetrics(); this closes both the deque's ongoing
    // chunk churn and the transposed-access inefficiency. All 16 buffers are
    // always pushed/popped together (single call site, once per tick), so
    // they never fall out of sync -- size() on any one of them is a valid
    // stand-in for "how many samples do we have."
    std::array<RingBuffer<float, OBS_SATURATION_MIN_SAMPLES + 1>, OBSERVATION_VECTOR_SIZE> m_observationHistory;
    std::optional<HMMTriggerDiagnostics> m_lastTriggerDiagnostics;  ///< Diagnostics from most recent trigger
    std::array<float, OBSERVATION_VECTOR_SIZE> m_prevCollectionObservation = {};
    bool m_hasPrevCollectionObservation = false;
    std::array<uint64_t, OBSERVATION_VECTOR_SIZE> m_staleRunLength = {};
    std::array<uint64_t, OBSERVATION_VECTOR_SIZE> m_changeCount = {};
    uint64_t m_collectionObservationCount = 0;

    struct TelemetryCounters {
        uint64_t waveRejectNonFinite = 0;
        uint64_t rippleRejectNonFinite = 0;
        uint64_t invalidObservationReject = 0;
        uint64_t sanitizedNonFinite = 0;
        uint64_t sanitizedClamped = 0;
        uint64_t freshnessDigestEmitted = 0;
        uint64_t ts1MacroReject = 0;
        uint64_t ts2StructuralReject = 0;
    };
    TelemetryCounters m_telemetryCounters = {};

    /// Hybrid feature scaler for observation vector (SOFTLOGZ state + LOGZ energy)
    /// Updated once per CheckAndTriggerHMM call, NOT from AddToTrainingEventFB
    FeatureScaler m_featureScaler;

    /// Latest post-FeatureScaler observation for all 16 dims in hybrid space.
    /// Stored after UpdateAndNormalize() in CheckAndTriggerHMM().
    /// LockC and other downstream consumers read this instead of raw anchors.
    std::array<float, OBSERVATION_VECTOR_SIZE> m_latestScaledObs = {};

    /// Replay-safe timestamp of the most recent TS1 macro commit (dims 0/6/8/9).
    std::atomic<uint64_t> m_ts1MacroLastWriteUs{0};

    /// Replay-safe timestamp of the most recent TS2 structural commit (dims 13/14).
    std::atomic<uint64_t> m_ts2StructuralLastWriteUs{0};

    /// Calculate event velocity from recent timestamp history
    /// Non-const because it maintains rolling deque of timestamps
    /// @param now_us Current time in microseconds
    /// @return Events per second in the EVENT_VELOCITY_WINDOW_SEC window
    float CalculateEventVelocity(uint64_t now_us);

    /// Calculate Burstiness Index (CV of Inter-Arrival Times)
    /// Measures non-Poisson clustering (Pareto flow)
    /// @return Dimensionless CV ratio (StdDev/Mean of IATs)
    float CalculateBurstinessIndex(uint64_t now_us);

    /// Apply velocity-based adaptive noise floor threshold for robust HMM triggering

    /// Adapts threshold based on market activity:
    /// - Low velocity (< 3 evt/s): 1.5x baseline (conservative during lunch/overnight)
    /// - Normal velocity (3-8 evt/s): Linear interpolation between extremes
    /// - High velocity (> 8 evt/s): 0.85x baseline (reactive during active sessions)
    /// @param event_velocity Events per second
    /// @return Adaptive noise floor threshold
    float GetAdaptiveNoiseFloor(float event_velocity) const;

    /// Adaptive Mahalanobis epsilon to filter noise and retain structural physics shifts.
    /// Raises threshold in high-noise/fragile states; lowers in coherent high-velocity moves.
    float GetAdaptiveMahalanobisEpsilon(float event_velocity, float path_efficiency_snr, float realized_kurtosis) const;

    struct TriggerDecisionMetrics {
        float mahalanobis_distance = 0.0f;
        float mahalanobis_epsilon = 0.0f;
        float energy_mahalanobis = 0.0f;
        float geometry_mahalanobis = 0.0f;
        float energy_contribution_share = 0.0f;
        bool energy_fast_track = false;
        bool significant_change = false;
    };

    /// Compute Mahalanobis trigger metrics for current observation snapshot.
    TriggerDecisionMetrics ComputeTriggerDecisionMetrics(
        const std::array<float, OBSERVATION_VECTOR_SIZE>& currentObs,
        float event_velocity) const;

    /// Compute L1 distance from last emitted HMM observation baseline.
    float ComputeL1DistanceFromBaseline(
        const std::array<float, OBSERVATION_VECTOR_SIZE>& currentObs) const;

    /// Build base trigger diagnostics payload before emission result is known.
    HMMTriggerDiagnostics BuildTriggerDiagnostics(
        uint64_t now_us,
        bool isDataCollection,
        float event_velocity,
        float l1_accumulation,
        float adaptive_noise_floor,
        const TriggerDecisionMetrics& trigger_metrics) const;

    /// Validate observation vector at ContextManager emission boundary.
    /// Returns false if any value is non-finite and records sampled diagnostics.
    bool ValidateObservationVector(const std::array<float, OBSERVATION_VECTOR_SIZE>& currentObs);

    /// Update collection-only change/staleness telemetry and emit periodic digest.
    /// Returns true when at least one dimension changed above OBS_CHANGE_EPS.
    bool UpdateCollectionObservationTelemetry(
        const std::array<float, OBSERVATION_VECTOR_SIZE>& rawObs,
        const std::array<float, OBSERVATION_VECTOR_SIZE>& currentObs);

    /// Emit training-context payload to .context stream.
    /// Returns true on successful write, false on exception.
    bool EmitTrainingContext(
        const std::array<float, OBSERVATION_VECTOR_SIZE>& currentObs,
        const MTS::Schema::AsymmetryContext& asymContext,
        uint64_t now_us);

    /// Emit live-context payload to HMM client.
    /// Returns true on successful send, false on exception.
    bool EmitLiveContext(
        const std::array<float, OBSERVATION_VECTOR_SIZE>& currentObs,
        uint64_t sequence_id,
        uint64_t now_us);
};
