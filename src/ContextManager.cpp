#include "MindfulTrader_Precompiled.h"
#include "ContextManager.h"
#include "LBRFileManager.h"
#include "HMMClient.h"
#include "Logger.h"
#include "IndicatorManager.h"
#include "ChandelierStopManager.h"
#include "PositionManager.h"
#include <cmath>
#include <algorithm>
#include <chrono>

namespace {
constexpr uint64_t kInputRejectLogEvery = 128;
constexpr uint64_t kInvalidObservationLogEvery = 64;
constexpr uint64_t kSanitizedObservationLogEvery = 5000;
constexpr uint64_t kTs1MacroRejectLogEvery = 4096;
constexpr uint64_t kTs2StructuralRejectLogEvery = 4096;
constexpr uint64_t kTs1MacroMaxAgeUs = 6ULL * 60ULL * 60ULL * 1000000ULL;
constexpr uint64_t kTs2StructuralMaxAgeUs = 3ULL * 60ULL * 60ULL * 1000000ULL;

uint64_t GetSteadyNowUs() {
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count());
}

constexpr std::array<float, ContextManager::OBSERVATION_VECTOR_SIZE> kObsLowerBounds = {
    -6.0f,   // OBS_LOG_VARIANCE_RATIO
   -6.0f,   // OBS_BURSTINESS_INDEX (half-window variance ratio)
    0.0f,   // OBS_REL_RANGE
   -6.0f,   // OBS_CORRECTION_ACTION (realized variance ratio)
    0.0f,   // OBS_VOL_CONVEXITY
    0.0f,   // OBS_LEMPEL_ZIV
    0.0f,   // OBS_HURST_EXPONENT
   -1.0f,   // OBS_MICRO_ASYMMETRY
   -6.0f,   // OBS_FISHER_INFO
    0.5f,   // OBS_TAIL_INDEX
   -2.5f,   // OBS_SKEWNESS
    0.0f,   // OBS_VPIN_TOXICITY (Amihud illiquidity)
    0.0f,   // OBS_LIQ_FRAGILITY
    0.0f,   // OBS_RECURRENCE_RATE
    1.0f,   // OBS_FRACTAL_DIM
    0.0f    // OBS_MEAN_REV_Z
};

constexpr std::array<float, ContextManager::OBSERVATION_VECTOR_SIZE> kObsUpperBounds = {
    6.0f,    // OBS_LOG_VARIANCE_RATIO
    6.0f,    // OBS_BURSTINESS_INDEX (half-window variance ratio)
    25.0f,   // OBS_REL_RANGE
    6.0f,    // OBS_CORRECTION_ACTION (realized variance ratio)
    25.0f,   // OBS_VOL_CONVEXITY
    1.0f,    // OBS_LEMPEL_ZIV
    1.5f,    // OBS_HURST_EXPONENT
    1.0f,    // OBS_MICRO_ASYMMETRY
    6.0f,    // OBS_FISHER_INFO
    8.0f,    // OBS_TAIL_INDEX
    2.5f,    // OBS_SKEWNESS
    100.0f,  // OBS_VPIN_TOXICITY (Amihud illiquidity)
    1.0f,    // OBS_LIQ_FRAGILITY
    1.0f,    // OBS_RECURRENCE_RATE
    2.0f,    // OBS_FRACTAL_DIM
    5.0f     // OBS_MEAN_REV_Z
};

inline bool ShouldSampleLog(uint64_t count, uint64_t firstN, uint64_t everyN) {
    if (count <= firstN) {
        return true;
    }
    return everyN > 0 && (count % everyN) == 0;
}

inline bool IsEnergyObservationDim(size_t dim) {
    switch (dim) {
        case ContextManager::OBS_LOG_VARIANCE_RATIO:
        case ContextManager::OBS_REL_RANGE:
        case ContextManager::OBS_VOL_CONVEXITY:
        case ContextManager::OBS_TAIL_INDEX:
        case ContextManager::OBS_LIQ_FRAGILITY:
            return true;
        default:
            return false;
    }
}

inline bool ShouldTriggerHMM(
    bool hmm_initialized,
    bool significant_change,
    bool is_data_collection,
    bool any_observation_changed) {
    if (!is_data_collection) {
        return !hmm_initialized || significant_change;
    }
    // Collection narrative contract:
    // emit first sample and when any dimension changes.
    return !hmm_initialized || any_observation_changed;
}

inline std::array<float, ContextManager::OBSERVATION_VECTOR_SIZE> SanitizeObservationVector(
    const std::array<float, ContextManager::OBSERVATION_VECTOR_SIZE>& input,
    uint64_t& non_finite_count,
    uint64_t& clamped_count) {
    auto out = input;
    for (size_t i = 0; i < ContextManager::OBSERVATION_VECTOR_SIZE; ++i) {
        if (!std::isfinite(out[i])) {
            out[i] = 0.0f;
            ++non_finite_count;
        }

        const float clamped = std::clamp(out[i], kObsLowerBounds[i], kObsUpperBounds[i]);
        if (clamped != out[i]) {
            out[i] = clamped;
            ++clamped_count;
        }
    }
    return out;
}

template <size_t N>
inline int CountStaleDims(
    const std::array<uint64_t, ContextManager::OBSERVATION_VECTOR_SIZE>& stale_run,
    const std::array<size_t, N>& dims,
    uint64_t stale_threshold) {
    int stale = 0;
    for (size_t idx : dims) {
        if (stale_run[idx] >= stale_threshold) {
            ++stale;
        }
    }
    return stale;
}
}  // namespace

// Singleton Instance
ContextManager& ContextManager::Instance() {
    static ContextManager instance;
    return instance;
}

// State Setters
void ContextManager::SetWaveContext(StatisticalContext&& ctx) {
    if (!std::isfinite(ctx.volatility) || !std::isfinite(ctx.efficiency)) {
        ++m_telemetryCounters.waveRejectNonFinite;
        if (ShouldSampleLog(m_telemetryCounters.waveRejectNonFinite, 3, kInputRejectLogEvery)) {
            Logger::getInstance().log(
                "ContextManager::SetWaveContext rejected non-finite input "
                "(count=" + std::to_string(m_telemetryCounters.waveRejectNonFinite) + ")"
            );
        }
        return;
    }

    // Keep efficiency bounded in case upstream sends slight numeric overshoots.
    ctx.efficiency = std::clamp(ctx.efficiency, 0.0f, 1.0f);

    // Institutional-grade regime tenure tracking
    // Detect regime changes by monitoring efficiency and volatility shifts
    // If either changes significantly (>15%), reset tenure to 1 (new regime started)
    if (m_hasWaveContext) {
        // Calculate percentage changes
        float eff_change = std::abs(ctx.efficiency - m_prevEfficiency) / std::max(m_prevEfficiency, 0.1f);
        float vol_change = std::abs(ctx.volatility - m_prevVolatility) / std::max(m_prevVolatility, 0.0001f);

        // If significant change detected, regime likely shifted
        if (eff_change > REGIME_CHANGE_THRESHOLD || vol_change > REGIME_CHANGE_THRESHOLD) {
            m_regimeTenureCounter = 1;  // Reset: new regime started
        } else {
            m_regimeTenureCounter++;     // Continue: same regime
        }

    } else {
        m_regimeTenureCounter = 1;  // First update
    }

    // Store current values for next comparison
    m_prevEfficiency = ctx.efficiency;
    m_prevVolatility = ctx.volatility;

    // Single canonical tenure value for live + training exports.
    ctx.regimeTenure = m_regimeTenureCounter;

    // Wave path owns volatility/efficiency and may carry TS2 oscillator velocity.
    m_waveContext.volatility = ctx.volatility;
    m_waveContext.efficiency = ctx.efficiency;
    m_waveContext.relRange = 0.0f;
    m_waveContext.velocity = std::isfinite(ctx.velocity) ? ctx.velocity : 0.0f;
    m_waveContext.lastUpdated = ctx.lastUpdated;
    m_waveContext.regimeTenure = ctx.regimeTenure;
    m_hasWaveContext = true;
}

void ContextManager::SetRippleContext(StatisticalContext&& ctx) {
    if (!std::isfinite(ctx.relRange) || !std::isfinite(ctx.velocity)) {
        ++m_telemetryCounters.rippleRejectNonFinite;
        if (ShouldSampleLog(m_telemetryCounters.rippleRejectNonFinite, 3, kInputRejectLogEvery)) {
            Logger::getInstance().log(
                "ContextManager::SetRippleContext rejected non-finite input "
                "(count=" + std::to_string(m_telemetryCounters.rippleRejectNonFinite) + ")"
            );
        }
        return;
    }

    // Strict ownership: ripple path owns relRange/velocity and may cache ATR proxy
    // (in volatility slot) for downstream normalization tasks.
    m_rippleContext.volatility = (std::isfinite(ctx.volatility) && ctx.volatility > 0.0f)
        ? ctx.volatility
        : 0.0f;
    m_rippleContext.efficiency = 0.0f;
    m_rippleContext.relRange = ctx.relRange;
    m_rippleContext.velocity = ctx.velocity;
    m_rippleContext.lastUpdated = ctx.lastUpdated;
    m_rippleContext.regimeTenure = m_regimeTenureCounter;
    m_hasRippleContext = true;
}

// Phase 2/3: Feed raw market returns to Engines
void ContextManager::UpdateMarketPhysics(float logReturn) {
    m_lastLogReturn = logReturn;  // Cache for Recurrence Rate calculation

    m_infoEngine.AddObservation(logReturn);
    m_tailRiskEngine.AddObservation(logReturn);

    // [NEW] Elite v3.0: Populate Institutional Metrics Cache from Internal Engines
    m_latestInstitutionalMetrics.shannonFlowEntropy = static_cast<float>(m_infoEngine.GetShannonEntropy());
    // Shannon efficiency: 1 - H/Hmax where Hmax = log2(NUM_BINS) for equiprobable bins.
    // NUM_BINS = 10 → Hmax ≈ 3.322 bits.
    constexpr float kMaxEntropy = 3.321928f; // std::log2(10.0f)
    static_assert(MindfulTrader::InformationEngine::NUM_BINS == 10,
                  "kMaxEntropy must match log2(InformationEngine::NUM_BINS)");
    if (m_latestInstitutionalMetrics.shannonFlowEntropy > 0.0f) {
        m_latestInstitutionalMetrics.shannonEfficiency = 1.0f - std::min(m_latestInstitutionalMetrics.shannonFlowEntropy / kMaxEntropy, 1.0f);
    } else {
        m_latestInstitutionalMetrics.shannonEfficiency = 0.5f; // Neutral
    }

    // TailRiskEngine is the unconditional sole authority for dim 9 (tail_index).
    // No bar-based fallback writes to m_observationData here;
    // BuildObservationVector() reads TRE directly.

    // Compressor is updated later with the full vector
}

void ContextManager::SetNormalizedAnchors(NormalizedAnchors&& anchors) {
    // [NEW] Elite v3.0: Populate Institutional Metrics Cache from Anchors
    m_latestInstitutionalMetrics.talebKurtosis = anchors.realizedKurtosis;
    m_latestInstitutionalMetrics.talebSkewness = anchors.skewnessIdx;

    // Move into member variable
    m_anchors = std::move(anchors);
}

void ContextManager::SetDailyCache(const DailyCache& cache) {
    m_dailyCache = cache;
}

// Getters
std::optional<StatisticalContext> ContextManager::GetStatisticalContext() const {
    if (!m_hasWaveContext && !m_hasRippleContext) {
        return std::nullopt;
    }

    StatisticalContext merged{};
    if (m_hasWaveContext) {
        merged = m_waveContext;
    }
    if (m_hasRippleContext) {
        merged.relRange = m_rippleContext.relRange;
        merged.velocity = m_rippleContext.velocity;
        if (!m_hasWaveContext || m_rippleContext.lastUpdated > merged.lastUpdated) {
            merged.lastUpdated = m_rippleContext.lastUpdated;
        }
    }
    return merged;
}

std::optional<NormalizedAnchors> ContextManager::GetNormalizedAnchors() const {
    return m_anchors;
}

std::array<float, ContextManager::OBSERVATION_VECTOR_SIZE> ContextManager::GetLatestScaledObservation() const {
    return m_latestScaledObs;
}

float ContextManager::GetLastEventVelocityPerSec() const {
    return m_lastEventVelocityPerSec;
}

// Utility: Get adaptive noise floor based on market velocity
float ContextManager::GetAdaptiveNoiseFloor(float event_velocity) const {
    if (event_velocity < VELOCITY_LOW) {
        // Low velocity (< 3 events/sec): Conservative during lunch dead zone or overnight
        // Require 1.5× baseline confirmation to filter false signals
        return NOISE_FLOOR * NOISE_FLOOR_LOW_VELOCITY_MULT;
    } else if (event_velocity > VELOCITY_HIGH) {
        // High velocity (> 8 events/sec): Reactive during active trading sessions
        // Allow 0.85× baseline to capture fast-moving regimes
        return NOISE_FLOOR * NOISE_FLOOR_HIGH_VELOCITY_MULT;
    } else {
        // Normal velocity (3-8 events/sec): Linear interpolation
        // Smoothly blend from conservative to reactive across the range
        float t = (event_velocity - VELOCITY_LOW) / (VELOCITY_HIGH - VELOCITY_LOW);  // Normalize to [0,1]
        float scale = NOISE_FLOOR_LOW_VELOCITY_MULT - (t * (NOISE_FLOOR_LOW_VELOCITY_MULT - NOISE_FLOOR_HIGH_VELOCITY_MULT));
        return NOISE_FLOOR * scale;
    }
}

float ContextManager::GetAdaptiveMahalanobisEpsilon(
    float event_velocity,
    float path_efficiency_snr,
    float realized_kurtosis) const {
    // Base epsilon tuned for 16D diagonal Mahalanobis distance under stable observations.
    constexpr float base_epsilon = 4.0f;

    // Velocity term: lower epsilon in fast markets to capture real regime transitions quickly.
    float velocity_multiplier = 1.0f;
    if (event_velocity < VELOCITY_LOW) {
        velocity_multiplier = 1.15f;
    } else if (event_velocity > VELOCITY_HIGH) {
        velocity_multiplier = 0.85f;
    } else {
        const float t = (event_velocity - VELOCITY_LOW) / (VELOCITY_HIGH - VELOCITY_LOW);
        velocity_multiplier = 1.15f - (0.30f * t);
    }

    // Noise term: low path efficiency (high noise) should be harder to trigger.
    const float entropy_clamped = std::clamp(path_efficiency_snr, 0.0f, 1.0f);
    const float entropy_multiplier = 1.0f + (0.35f * entropy_clamped);

    // Fragility term: elevate threshold in fat-tail stress unless movement is truly structural.
    const float kurtosis_excess = std::max(realized_kurtosis - 3.0f, 0.0f);
    const float kurtosis_multiplier = 1.0f + std::min(kurtosis_excess * 0.05f, 0.25f);

    return base_epsilon * velocity_multiplier * entropy_multiplier * kurtosis_multiplier;
}

// Get diagnostics from most recent HMM trigger decision
std::optional<HMMTriggerDiagnostics> ContextManager::GetLastTriggerDiagnostics() const {
    return m_lastTriggerDiagnostics;
}

bool ContextManager::IsObservationSaturated() const {
    return m_featureScaler.warmedUp;
}

size_t ContextManager::GetObservationSampleCount() const {
    return m_featureScaler.sampleCount;
}

void ContextManager::MarkTs1MacroDimsFresh(uint64_t timestamp_us) {
    m_ts1MacroLastWriteUs.store(timestamp_us, std::memory_order_relaxed);
    m_ts1MacroLastSteadyUs.store(GetSteadyNowUs(), std::memory_order_relaxed);
    m_ts1SeenAfterReset.store(true, std::memory_order_relaxed);
}

bool ContextManager::AreTs1DimsReady(uint64_t now_us, uint64_t max_age_us) const {
    if (!m_ts1SeenAfterReset.load(std::memory_order_relaxed)) {
        return false;
    }

    const float dim0 = m_observationData.log_variance_ratio();
    const float dim6 = m_observationData.hurst_exponent();
    const float dim8 = m_observationData.fisher_info();
    // Dim9 (tail_index) authority is TailRiskEngine via cached Hill alpha.
    // ObservationData::tail_index is not the canonical hot-path source here.
    const float dim9 = m_cachedHillAlpha.load(std::memory_order_relaxed);

    const bool finite = std::isfinite(dim0) &&
                        std::isfinite(dim6) &&
                        std::isfinite(dim8) &&
                        std::isfinite(dim9);
    if (!finite) {
        return false;
    }

    if (dim9 <= 0.5f) {
        return false;
    }

    const uint64_t lastWriteUs = m_ts1MacroLastWriteUs.load(std::memory_order_relaxed);
    if (lastWriteUs == 0) {
        return false;
    }

    if (now_us >= lastWriteUs) {
        return (now_us - lastWriteUs) <= max_age_us;
    }

    // Defensive for replay seeks or chart timeline jumps.
    return (lastWriteUs - now_us) <= max_age_us;
}

uint64_t ContextManager::GetTs1MacroAgeUs(uint64_t now_us) const {
    const uint64_t lastWriteUs = m_ts1MacroLastWriteUs.load(std::memory_order_relaxed);
    if (lastWriteUs == 0) {
        return 0;
    }

    if (now_us >= lastWriteUs) {
        return now_us - lastWriteUs;
    }

    return 0;
}

uint64_t ContextManager::GetTs1MacroLastWriteUs() const {
    return m_ts1MacroLastWriteUs.load(std::memory_order_relaxed);
}

void ContextManager::MarkTs2StructuralDimsFresh(uint64_t timestamp_us) {
    m_ts2StructuralLastWriteUs.store(timestamp_us, std::memory_order_relaxed);
    m_ts2StructuralLastSteadyUs.store(GetSteadyNowUs(), std::memory_order_relaxed);
    m_ts2SeenAfterReset.store(true, std::memory_order_relaxed);
}

bool ContextManager::AreTs2StructuralDimsReady(uint64_t now_us, uint64_t max_age_us) const {
    if (!m_ts2SeenAfterReset.load(std::memory_order_relaxed)) {
        return false;
    }

    const float recurrence = m_observationData.recurrence_rate();
    const float fractal = m_observationData.fractal_dim();
    const bool finite = std::isfinite(recurrence) && std::isfinite(fractal);
    if (!finite) {
        return false;
    }

    const bool inContract = recurrence >= 0.0f && recurrence <= 1.0f &&
                            fractal >= 1.0f && fractal <= 2.0f;
    if (!inContract) {
        return false;
    }

    const uint64_t lastWriteUs = m_ts2StructuralLastWriteUs.load(std::memory_order_relaxed);
    if (lastWriteUs == 0) {
        return false;
    }

    if (now_us >= lastWriteUs) {
        return (now_us - lastWriteUs) <= max_age_us;
    }

    return (lastWriteUs - now_us) <= max_age_us;
}

uint64_t ContextManager::GetTs2StructuralAgeUs(uint64_t now_us) const {
    const uint64_t lastWriteUs = m_ts2StructuralLastWriteUs.load(std::memory_order_relaxed);
    if (lastWriteUs == 0) {
        return 0;
    }

    if (now_us >= lastWriteUs) {
        return now_us - lastWriteUs;
    }

    return 0;
}

uint64_t ContextManager::GetTs2StructuralLastWriteUs() const {
    return m_ts2StructuralLastWriteUs.load(std::memory_order_relaxed);
}

// Utility: Build 16D observation vector (Elite v3.1: Institutional Physics)
// Single source of truth for observation vector construction
// Used by both AddToTrainingEventFB() and CheckAndTriggerHMM()
// Returns strictly 16D vector for HMM Physics Core
std::array<float, ContextManager::OBSERVATION_VECTOR_SIZE> ContextManager::BuildObservationVector() {
    std::array<float, OBSERVATION_VECTOR_SIZE> obs = {};

    // 0. Log-Variance Ratio (Ergodicity)
    // Directly from struct storage
    obs[OBS_LOG_VARIANCE_RATIO] = m_observationData.log_variance_ratio();

    // 1. Burstiness Index (Shannon-Pareto Flow)
    obs[OBS_BURSTINESS_INDEX] = m_observationData.burstiness_index();

    // 2. Relative Range
    obs[OBS_REL_RANGE] = m_observationData.relative_range();

    // 3. Realized Variance Ratio (replaces Correction Action)
    // Now driven by TripleScreen2 — stateless, no internal accumulation.
    obs[OBS_CORRECTION_ACTION] = m_observationData.correction_action();

    // === QUADRANT II (Information): Indices 5-8 - Market Quality & Confidence ===
    // Dim 5 (LZ): InformationEngine is the unconditional sole authority.
    // IE computes LZ76 over event-level log returns with adaptive median
    // binarization, fed by UpdateMarketPhysics() in both live and collection
    // modes.  No bar-based CalculateLempelZiv() writes to m_observationData.
    {
        const size_t ieSamples = m_infoEngine.GetSampleCount();
        if (ieSamples >= 50) {
            double rawLZ = m_infoEngine.GetLempelZivComplexity();
            obs[OBS_LEMPEL_ZIV] = static_cast<float>(rawLZ);
        } else {
            obs[OBS_LEMPEL_ZIV] = 0.0f;  // Warmup: IE not yet primed
        }
    }
    obs[OBS_HURST_EXPONENT] = m_observationData.hurst_exponent();
    obs[OBS_MICRO_ASYMMETRY] = m_observationData.micro_asymmetry();
    obs[OBS_FISHER_INFO] = m_observationData.fisher_info();

    // === QUADRANT III (Fragility): Indices 9-12 - Taleb Risk Metrics ===
    // Dim 9 (tail_index): TailRiskEngine is the unconditional sole authority.
    // TRE computes Hill alpha over event-level log returns (500-sample window,
    // top 5% tail mass), fed by UpdateMarketPhysics() in both live and
    // collection modes.  No bar-based CalculateHillEstimator writes here.
    if (m_tailRiskEngine.GetSampleCount() >= 50) {
        const float alpha = static_cast<float>(m_tailRiskEngine.GetHillAlpha());
        obs[OBS_TAIL_INDEX] = (std::isfinite(alpha) && alpha >= 1.1f && alpha <= 8.0f)
                              ? alpha : 2.5f;  // 2.5 = neutral if pathological
        m_cachedHillAlpha.store(obs[OBS_TAIL_INDEX], std::memory_order_relaxed);  // P1.4: cache for direct gate
    } else {
        obs[OBS_TAIL_INDEX] = 0.0f;  // Warmup: TRE not yet primed
        m_cachedHillAlpha.store(4.0f, std::memory_order_relaxed);  // P1.4: assume safe during warmup
    }
    obs[OBS_SKEWNESS] = m_observationData.skewness_idx();
    obs[OBS_VPIN_TOXICITY] = m_observationData.vpin_toxicity();
    obs[OBS_LIQ_FRAGILITY] = m_observationData.liq_fragility();

    // === QUADRANT IV (Structure): Indices 13-15 - Topological Stability ===
    obs[OBS_RECURRENCE_RATE] = m_observationData.recurrence_rate();
    obs[OBS_FRACTAL_DIM] = m_observationData.fractal_dim();
    obs[OBS_MEAN_REV_Z] = m_observationData.mean_rev_z();

    // 4. Vol Convexity (Q1 derivative check)
    obs[OBS_VOL_CONVEXITY] = m_observationData.vol_convexity();

    // === Elite v3.2: Refresh LocalRiskContext from all engines ===
    // Single write point — all engines already updated by callers before BuildObservationVector().
    m_localRiskContext.shannonFlowEntropy = m_latestInstitutionalMetrics.shannonFlowEntropy;
    m_localRiskContext.shannonEfficiency = m_latestInstitutionalMetrics.shannonEfficiency;
    m_localRiskContext.talebKurtosis = m_latestInstitutionalMetrics.talebKurtosis;
    m_localRiskContext.talebSkewness = m_latestInstitutionalMetrics.talebSkewness;
    m_localRiskContext.elderChandelierATR = m_latestInstitutionalMetrics.elderChandelierATR;
    m_localRiskContext.paretoTailAlpha = m_cachedHillAlpha.load(std::memory_order_relaxed);
    m_localRiskContext.vpin = obs[OBS_VPIN_TOXICITY];
    m_localRiskContext.spreadStress = obs[OBS_LIQ_FRAGILITY];
    m_localRiskContext.hurstExponent = obs[OBS_HURST_EXPONENT];
    m_localRiskContext.fractalDim = obs[OBS_FRACTAL_DIM];
    m_localRiskContext.meanRevZ = obs[OBS_MEAN_REV_Z];
    m_localRiskContext.raschkeBurst = m_latestInstitutionalMetrics.raschkeBurst;
    m_localRiskContext.fisherInfo = obs[OBS_FISHER_INFO];
    m_localRiskContext.isValid = m_featureScaler.warmedUp;
    m_localRiskContext.snapshotTimestampUs = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()
        ).count()
    );
    // Note: regimeDuration set externally via SetRegimeDuration()
    // ADX retired March 2026 — Hurst exponent provides trend persistence

    return obs;
}

// Helper: Build Asymmetry Context (8D)
MTS::Schema::AsymmetryContext ContextManager::GetAsymmetryContext() const {
    return MTS::Schema::Contract::MakeAsymmetryContext({
        m_latestInstitutionalMetrics.shannonFlowEntropy,
        m_latestInstitutionalMetrics.shannonEfficiency,
        m_latestInstitutionalMetrics.talebKurtosis,
        m_latestInstitutionalMetrics.talebSkewness,
        m_latestInstitutionalMetrics.elderChandelierATR,
        m_latestInstitutionalMetrics.paretoRot,
        m_latestInstitutionalMetrics.raschkeBurst,
        m_latestInstitutionalMetrics.elderImpulse,
    });
}

// Utility: Calculate event velocity from timestamp history
float ContextManager::CalculateEventVelocity(uint64_t now_us) {
    if (m_eventTimestampsUS.size() >= EVENT_VELOCITY_MAX) {
        m_eventTimestampsUS.pop_front();
    }
    m_eventTimestampsUS.push_back(now_us);

    if (m_eventTimestampsUS.empty()) {
        return 0.0f;
    }

    uint64_t window_size_us = static_cast<uint64_t>(EVENT_VELOCITY_WINDOW_SEC) * 1000000ULL;
    uint64_t window_start = (now_us > window_size_us) ? (now_us - window_size_us) : 0;

    size_t count = 0;
    for (auto it = m_eventTimestampsUS.rbegin(); it != m_eventTimestampsUS.rend(); ++it) {
        if (*it < window_start) break;
        ++count;
    }

    return static_cast<float>(count) / EVENT_VELOCITY_WINDOW_SEC;
}

// Populate TrainingEvent with environmental context (SECTION 11)
// SECTION 11 contains: volatility, efficiency, rel_range, velocity, regime_tenure,
// normalized anchors (dist_day_high/low, dist_four_bar_high/low, dist_ema_13),
// and the 16D observation vector for HMM inference
void ContextManager::AddToTrainingEventFB(MTS::Training::TrainingEventT& event, SCStudyInterfaceRef /*sc*/) const {
    // ELITE 3.0: All 8D AsymmetryContext dims now read from m_latestInstitutionalMetrics
    // (populated by UpdateMarketPhysics, UpdatePriceStructure, SetNormalizedAnchors,
    //  CheckAndTriggerHMM). This guarantees .alpha/.context parity.

    // SECTION 11: Statistical Context (Screen2 Environmental Data)
    // These fields represent market conditions: volatility, efficiency, range, velocity
    const StatisticalContext* wave = m_hasWaveContext ? &m_waveContext : nullptr;
    const StatisticalContext* ripple = m_hasRippleContext ? &m_rippleContext : nullptr;

    if (wave) {
        event.volatility = wave->volatility;          ///< Std dev of log returns
        event.efficiency = wave->efficiency;          ///< Net change / sum of absolute changes [0,1]
        event.regime_tenure = wave->regimeTenure;  ///< TS2 bar-closes in current regime
    }
    event.rel_range = ripple ? ripple->relRange : 0.0f;
    // Export TS2 oscillator velocity when available; fall back to ripple velocity.
    event.velocity = wave ? wave->velocity : (ripple ? ripple->velocity : 0.0f);
    // Emit the raw log1p transformed velocity for the Transformer
    event.log_event_velocity = std::log1p(m_lastEventVelocityPerSec);

    // SECTION 11: Normalized Anchors (Screen3 Technical Levels)
    // These fields represent price distance to key support/resistance levels
    // Positive = above level, Negative = below level (all measured in points)
    if (m_anchors) {
        event.dist_day_high = m_anchors->distDayHigh;          ///< Distance to previous day high
        event.dist_day_low = m_anchors->distDayLow;            ///< Distance to previous day low
        event.dist_four_bar_high = m_anchors->distFourBarHigh; ///< Distance to 4-bar swing high
        event.dist_four_bar_low = m_anchors->distFourBarLow;   ///< Distance to 4-bar swing low
        event.dist_ema_13 = m_anchors->distEma13;              ///< Distance to EMA(13)
    }

    // SECTION 11: Observation Vector (16D context for HMM inference)
    // Use the post-FeatureScaler hybrid observation:
    // - state dims: topology-preserving Soft-Log-Z values
    // - energy dims: 6-sigma winsorized Log-Z values
    // This guarantees representational parity across .alpha, .context, live inference,
    // and training pipelines.
    // m_latestScaledObs is updated once per CheckAndTriggerHMM() call.
    event.observation = std::make_unique<MTS::Schema::ObservationData>(
        MTS::Schema::Contract::MakeObservationData(m_latestScaledObs));

    // SECTION 12: Asymmetry Context (8D Transformer Input)
    // All 8 dims sourced from m_latestInstitutionalMetrics for .alpha/.context parity.
    event.asymmetry_context = std::make_unique<MTS::Schema::AsymmetryContext>(
        MTS::Schema::Contract::MakeAsymmetryContext({
            m_latestInstitutionalMetrics.shannonFlowEntropy,
            m_latestInstitutionalMetrics.shannonEfficiency,
            m_latestInstitutionalMetrics.talebKurtosis,
            m_latestInstitutionalMetrics.talebSkewness,
            m_latestInstitutionalMetrics.elderChandelierATR,
            m_latestInstitutionalMetrics.paretoRot,
            m_latestInstitutionalMetrics.raschkeBurst,
            m_latestInstitutionalMetrics.elderImpulse,
        }));
}

// ============================================================================
// Elite v3.0: Structure Engine Integration
// ============================================================================
void ContextManager::UpdatePriceStructure(SCStudyInterfaceRef sc, float high, float low, float close, bool isNewBar) {
    m_structureEngine.Update(high, low, close, isNewBar);

    // [NEW] Elite v3.0: Populate Institutional Metrics Cache
    m_latestInstitutionalMetrics.paretoRot = m_structureEngine.GetFractalDimension();

    // Elder Impulse Proxy: Close Location Value
    if (high > low) {
        float clv = ((close - low) - (high - close)) / (high - low);
        m_latestInstitutionalMetrics.elderImpulse = clv;
    }

    // Taleb Cliff: Chandelier Stop distance normalized by ATR
    const float atr = (m_hasRippleContext && m_rippleContext.volatility > 0.0f)
        ? m_rippleContext.volatility
        : sc.TickSize;

    float distToCliff = 0.0f;
    const Trade& trade = PositionManager::Instance().GetOpenTrade();
    if (trade.GetStatus() == TradeStatusEnum::OPEN && trade.GetStop() > 0.0) {
        // With position: distance from price to actual stop
        float diff = static_cast<float>(close - trade.GetStop());
        distToCliff = (atr > 0.00001f) ? (diff / atr) : 0.0f;
    } else {
        // Without position: theoretical Chandelier = High(22) - 3*ATR / Low(22) + 3*ATR
        int lookbar = 22;
        int limit = std::min(lookbar, sc.Index);
        if (limit > 0) {
            float hh = sc.High[sc.Index];
            float ll = sc.Low[sc.Index];
            for (int i = 1; i < limit; ++i) {
                float h = sc.High[sc.Index - i];
                float l = sc.Low[sc.Index - i];
                if (h > hh) hh = h;
                if (l < ll) ll = l;
            }
            float longStop = hh - (3.0f * atr);
            float shortStop = ll + (3.0f * atr);
            float distLong = (close - longStop);
            float distShort = (shortStop - close);
            float nearestSafeDist = std::min(distLong, distShort);
            distToCliff = (atr > 0.00001f) ? (nearestSafeDist / atr) : 0.0f;
        }
    }
    m_latestInstitutionalMetrics.elderChandelierATR = distToCliff;
}

bool ContextManager::ValidateObservationVector(
    const std::array<float, OBSERVATION_VECTOR_SIZE>& currentObs) {
    for (size_t i = 0; i < OBSERVATION_VECTOR_SIZE; ++i) {
        if (!std::isfinite(currentObs[i])) {
            ++m_telemetryCounters.invalidObservationReject;
            if (ShouldSampleLog(m_telemetryCounters.invalidObservationReject, 3, kInvalidObservationLogEvery)) {
                Logger::getInstance().log(
                    "ContextManager::CheckAndTriggerHMM SKIP: invalid observation value at idx=" +
                    std::to_string(i) + " value=" + std::to_string(currentObs[i]) +
                    " (count=" + std::to_string(m_telemetryCounters.invalidObservationReject) + ")"
                );
            }
            return false;
        }
    }
    return true;
}

bool ContextManager::UpdateCollectionObservationTelemetry(
    const std::array<float, OBSERVATION_VECTOR_SIZE>& rawObs,
    const std::array<float, OBSERVATION_VECTOR_SIZE>& currentObs) {
    ++m_collectionObservationCount;

    bool any_observation_changed = false;
    if (!m_hasPrevCollectionObservation) {
        any_observation_changed = true;
        m_hasPrevCollectionObservation = true;
    } else {
        for (size_t i = 0; i < OBSERVATION_VECTOR_SIZE; ++i) {
            const float delta = std::abs(currentObs[i] - m_prevCollectionObservation[i]);
            const float change_eps = IsEnergyObservationDim(i)
                ? OBS_CHANGE_EPS
                : (OBS_CHANGE_EPS * OBS_GEOMETRY_CHANGE_MULT);
            const bool changed = delta > change_eps;
            if (changed) {
                any_observation_changed = true;
                ++m_changeCount[i];
                m_staleRunLength[i] = 0;
            } else {
                ++m_staleRunLength[i];
            }
        }
    }

    m_prevCollectionObservation = currentObs;

    if ((m_collectionObservationCount % OBS_STALENESS_LOG_INTERVAL) == 0) {
        std::string stale_dims;
        int stale_count = 0;

        for (size_t i = 0; i < OBSERVATION_VECTOR_SIZE; ++i) {
            if (m_staleRunLength[i] >= OBS_STALENESS_ALERT_RUN) {
                if (!stale_dims.empty()) {
                    stale_dims += ", ";
                }
                stale_dims += "dim" + std::to_string(i) +
                    "(value=" + std::to_string(currentObs[i]) +
                    ", stale_run=" + std::to_string(m_staleRunLength[i]) +
                    ", changes=" + std::to_string(m_changeCount[i]) + ")";
                ++stale_count;
            }
        }

        if (stale_count > 0) {
            Logger::getInstance().log(
                "ContextManager::ObservationStaleness ALERT samples=" + std::to_string(m_collectionObservationCount) +
                " stale_dims=" + std::to_string(stale_count) + " [" + stale_dims + "]"
            );
        }
    }

    if ((m_collectionObservationCount % OBS_FRESHNESS_DIGEST_INTERVAL) == 0) {
        static constexpr std::array<size_t, 4> kTs1Dims = {
            OBS_LOG_VARIANCE_RATIO,
            OBS_HURST_EXPONENT,
            OBS_FISHER_INFO,
            OBS_TAIL_INDEX
        };
        static constexpr std::array<size_t, 5> kTs2Dims = {
            OBS_BURSTINESS_INDEX,
            OBS_REL_RANGE,
            OBS_LEMPEL_ZIV,
            OBS_RECURRENCE_RATE,
            OBS_FRACTAL_DIM
        };
        static constexpr std::array<size_t, 6> kTs3Dims = {
            OBS_VOL_CONVEXITY,
            OBS_MICRO_ASYMMETRY,
            OBS_SKEWNESS,
            OBS_VPIN_TOXICITY,
            OBS_LIQ_FRAGILITY,
            OBS_MEAN_REV_Z
        };

        const int ts1_stale_dims = CountStaleDims(
            m_staleRunLength, kTs1Dims, OBS_STALENESS_ALERT_RUN);
        const int ts2_stale_dims = CountStaleDims(
            m_staleRunLength, kTs2Dims, OBS_STALENESS_ALERT_RUN);
        const int ts3_stale_dims = CountStaleDims(
            m_staleRunLength, kTs3Dims, OBS_STALENESS_ALERT_RUN);
        const int cm_stale =
            (m_staleRunLength[OBS_CORRECTION_ACTION] >= OBS_STALENESS_ALERT_RUN) ? 1 : 0;

        ++m_telemetryCounters.freshnessDigestEmitted;
        Logger::getInstance().log(
            "ContextManager::FreshnessDigest samples=" +
            std::to_string(m_collectionObservationCount) +
            " digest_seq=" + std::to_string(m_telemetryCounters.freshnessDigestEmitted) +
            " has_wave=" + std::to_string(m_hasWaveContext ? 1 : 0) +
            " has_ripple=" + std::to_string(m_hasRippleContext ? 1 : 0) +
            " ts1_stale_dims=" + std::to_string(ts1_stale_dims) +
            " ts2_stale_dims=" + std::to_string(ts2_stale_dims) +
            " ts3_stale_dims=" + std::to_string(ts3_stale_dims) +
            " cm_stale_dims=" + std::to_string(cm_stale) +
            " reject_wave_nonfinite=" +
            std::to_string(m_telemetryCounters.waveRejectNonFinite) +
            " reject_ripple_nonfinite=" +
            std::to_string(m_telemetryCounters.rippleRejectNonFinite) +
            " reject_invalid_obs=" +
            std::to_string(m_telemetryCounters.invalidObservationReject) +
            " sanitize_nonfinite=" +
            std::to_string(m_telemetryCounters.sanitizedNonFinite) +
            " sanitize_clamped=" +
            std::to_string(m_telemetryCounters.sanitizedClamped)
        );

        // Focused zero-trap diagnostics (dims 0/6/8) emitted at digest cadence.
        Logger::getInstance().log(
            "ContextManager::ZeroTrapDigest samples=" +
            std::to_string(m_collectionObservationCount) +
            " raw(dim0=" + std::to_string(rawObs[OBS_LOG_VARIANCE_RATIO]) +
            ",dim6=" + std::to_string(rawObs[OBS_HURST_EXPONENT]) +
            ",dim8=" + std::to_string(rawObs[OBS_FISHER_INFO]) + ")" +
            " scaled(dim0=" + std::to_string(currentObs[OBS_LOG_VARIANCE_RATIO]) +
            ",dim6=" + std::to_string(currentObs[OBS_HURST_EXPONENT]) +
            ",dim8=" + std::to_string(currentObs[OBS_FISHER_INFO]) + ")"
        );
    }

    return any_observation_changed;
}

bool ContextManager::EmitTrainingContext(
    const std::array<float, OBSERVATION_VECTOR_SIZE>& currentObs,
    const MTS::Schema::AsymmetryContext& asymContext,
    uint64_t now_us) {
    try {
        LBRFileManager& lbr_mgr = LBRFileManager::Instance();

        const MTS::Schema::ObservationData obs_data =
            MTS::Schema::Contract::MakeObservationData(currentObs);

        // Canonical semantics shared with live path: regime tenure in TS2 bars.
        const float bars_since_last_update =
            m_hasWaveContext ? static_cast<float>(m_waveContext.regimeTenure) : 0.0f;

        // Raw gate-input twin (Findings 19/20/21): serialize the exact, UNSCALED
        // LocalRiskContext values the RiskManager gates evaluate, so the Python
        // execution-sim reads the same signals C++ gates on (no scaled proxies).
        const LocalRiskContext& lrc = m_localRiskContext;
        MTS::Schema::RiskGateContextT rgc;
        rgc.shannon_flow_entropy  = lrc.shannonFlowEntropy;
        rgc.shannon_efficiency    = lrc.shannonEfficiency;
        rgc.taleb_kurtosis        = lrc.talebKurtosis;
        rgc.taleb_skewness        = lrc.talebSkewness;
        rgc.elder_chandelier_atr  = lrc.elderChandelierATR;
        rgc.pareto_tail_alpha     = lrc.paretoTailAlpha;
        rgc.amihud_illiquidity    = lrc.vpin;   // PC-03: legacy 'vpin' field carries Amihud illiquidity
        rgc.spread_stress         = lrc.spreadStress;
        rgc.hurst_exponent        = lrc.hurstExponent;
        rgc.fractal_dim           = lrc.fractalDim;
        rgc.mean_rev_z            = lrc.meanRevZ;
        rgc.raschke_burst         = lrc.raschkeBurst;
        rgc.fisher_info           = lrc.fisherInfo;
        rgc.regime_duration       = lrc.regimeDuration;
        rgc.is_valid              = lrc.isValid;
        rgc.snapshot_timestamp_us = static_cast<int64_t>(lrc.snapshotTimestampUs);
        rgc.amihud_percentile     = lrc.amihudPercentile;   // Layer B: session-aware rolling percentile (the gate input)

        // Log both Physics (16D) and Asymmetry (8D) + raw gate context.
        lbr_mgr.LogContext(obs_data, asymContext, now_us, bars_since_last_update, &rgc);
        return true;
    } catch (const std::exception& e) {
        Logger::getInstance().log(
            "ERROR ContextManager::CheckAndTriggerHMM [PATH 1 - TRAINING]: " +
            std::string(e.what())
        );
        return false;
    }
}

void ContextManager::PushAmihudSample(float rawAmihud, bool isRTH) {
    // Skip warmup / degenerate values — keep the last trusted percentile rather than
    // polluting the pool with zeros/NaNs (canonical Amihud is strictly positive).
    if (!std::isfinite(rawAmihud) || rawAmihud <= 0.0f) {
        return;
    }

    // Route into the session pool (deep-liquidity RTH vs thin overnight/globex).
    if (isRTH) {
        m_amihudRthPool[static_cast<size_t>(m_amihudRthHead)] = rawAmihud;
        m_amihudRthHead = (m_amihudRthHead + 1) % kAmihudRthWindow;
        if (m_amihudRthCount < kAmihudRthWindow) ++m_amihudRthCount;
    } else {
        m_amihudOvernightPool[static_cast<size_t>(m_amihudOvernightHead)] = rawAmihud;
        m_amihudOvernightHead = (m_amihudOvernightHead + 1) % kAmihudOvernightWindow;
        if (m_amihudOvernightCount < kAmihudOvernightWindow) ++m_amihudOvernightCount;
    }

    const float* pool = isRTH ? m_amihudRthPool.data() : m_amihudOvernightPool.data();
    const int n = isRTH ? m_amihudRthCount : m_amihudOvernightCount;

    // Until the pool holds enough samples for a meaningful tail estimate, report a
    // neutral 0.5 so the p90/p75 gate cannot fire on thin history (fail-open on data,
    // not on risk — the fixed sibling gates still apply).
    if (n < kAmihudMinSamples) {
        m_localRiskContext.amihudPercentile = 0.5f;
        return;
    }

    // Empirical percentile of the current value within its session pool:
    // fraction of samples <= rawAmihud. O(n) once per 15-min bar (negligible).
    int leq = 0;
    for (int i = 0; i < n; ++i) {
        if (pool[i] <= rawAmihud) ++leq;
    }
    m_localRiskContext.amihudPercentile = static_cast<float>(leq) / static_cast<float>(n);
}

float ContextManager::ComputeL1DistanceFromBaseline(
    const std::array<float, OBSERVATION_VECTOR_SIZE>& currentObs) const {
    if (!m_hmmInitialized) {
        return 0.0f;
    }

    float l1_accumulation = 0.0f;
    for (size_t i = 0; i < OBSERVATION_VECTOR_SIZE; ++i) {
        l1_accumulation += std::abs(currentObs[i] - m_hmmObservation[i]);
    }
    return l1_accumulation;
}

bool ContextManager::EmitLiveContext(
    const std::array<float, OBSERVATION_VECTOR_SIZE>& currentObs,
    uint64_t sequence_id,
    uint64_t now_us) {
    try {
        // Send Dual-Stream Input:
        // 1. Scaled Physics (16D) -> HMM
        // 2. Transformer data handled via SCStudies/EventStream (Port 5555)
        HMMClient::Instance().RequestUpdateAsync(
            currentObs,
            m_hasWaveContext ? static_cast<float>(m_waveContext.regimeTenure) : 0.0f,
            sequence_id,
            now_us
        );

        // Intentionally no success-path log here; live trigger rate can be high
        // and would generate avoidable log noise.
        return true;
    } catch (const std::exception& e) {
        Logger::getInstance().log(
            "ERROR ContextManager::CheckAndTriggerHMM [PATH 2 - LIVE]: " +
            std::string(e.what())
        );
        return false;
    }
}

ContextManager::TriggerDecisionMetrics ContextManager::ComputeTriggerDecisionMetrics(
    const std::array<float, OBSERVATION_VECTOR_SIZE>& currentObs,
    float event_velocity) const {
    TriggerDecisionMetrics metrics;

    if (m_observationHistory.size() < OBS_SATURATION_MIN_SAMPLES) {
        return metrics;
    }

    float distance_sq = 0.0f;
    float energy_sq = 0.0f;
    float geometry_sq = 0.0f;
    float max_energy_abs_z = 0.0f;
    const float epsilon_floor = OBS_SATURATION_VAR_EPS;
    const int n = static_cast<int>(m_observationHistory.size());
    const int mid = n / 2;

    for (size_t dim = 0; dim < OBSERVATION_VECTOR_SIZE; ++dim) {
        // Extract per-dim values into stack scratch
        std::array<float, OBS_SATURATION_MIN_SAMPLES + 1> scratch;
        for (int k = 0; k < n; ++k) {
            scratch[static_cast<size_t>(k)] = m_observationHistory[static_cast<size_t>(k)][dim];
        }

        // Robust median
        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
        const float median = scratch[static_cast<size_t>(mid)];

        // Robust MAD
        for (int k = 0; k < n; ++k) {
            scratch[static_cast<size_t>(k)] = std::abs(
                m_observationHistory[static_cast<size_t>(k)][dim] - median);
        }
        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
        const float madScale = scratch[static_cast<size_t>(mid)] * 1.4826f;

        const float safe_variance = std::max(madScale * madScale, epsilon_floor);
        const float centered = currentObs[dim] - median;
        const float z = centered / std::sqrt(safe_variance);
        const float contribution = z * z;

        distance_sq += contribution;
        if (IsEnergyObservationDim(dim)) {
            energy_sq += contribution;
            max_energy_abs_z = std::max(max_energy_abs_z, std::abs(z));
        } else {
            geometry_sq += contribution;
        }
    }

    metrics.mahalanobis_distance = std::sqrt(std::max(distance_sq, 0.0f));
    metrics.energy_mahalanobis = std::sqrt(std::max(energy_sq, 0.0f));
    metrics.geometry_mahalanobis = std::sqrt(std::max(geometry_sq, 0.0f));
    metrics.energy_contribution_share =
        (distance_sq > OBS_SATURATION_VAR_EPS) ? (energy_sq / distance_sq) : 0.0f;
    metrics.energy_fast_track = (max_energy_abs_z >= ENERGY_FAST_TRACK_Z);

    metrics.mahalanobis_epsilon = GetAdaptiveMahalanobisEpsilon(
        event_velocity,
        currentObs[OBS_LEMPEL_ZIV],
        currentObs[OBS_TAIL_INDEX]
    );

    const bool energy_significant =
        metrics.energy_mahalanobis >= (metrics.mahalanobis_epsilon * ENERGY_TRIGGER_MULT);
    const bool geometry_significant =
        metrics.geometry_mahalanobis >= (metrics.mahalanobis_epsilon * GEOMETRY_TRIGGER_MULT);

    metrics.significant_change =
        metrics.energy_fast_track || energy_significant || geometry_significant;
    return metrics;
}

HMMTriggerDiagnostics ContextManager::BuildTriggerDiagnostics(
    uint64_t now_us,
    bool isDataCollection,
    float event_velocity,
    float l1_accumulation,
    float adaptive_noise_floor,
    const TriggerDecisionMetrics& trigger_metrics) const {
    HMMTriggerDiagnostics diag;
    diag.l1_norm = l1_accumulation;
    diag.noise_floor_used = adaptive_noise_floor;
    diag.mahalanobis_distance = trigger_metrics.mahalanobis_distance;
    diag.energy_mahalanobis = trigger_metrics.energy_mahalanobis;
    diag.geometry_mahalanobis = trigger_metrics.geometry_mahalanobis;
    diag.energy_contribution_share = trigger_metrics.energy_contribution_share;
    diag.mahalanobis_epsilon = trigger_metrics.mahalanobis_epsilon;
    diag.event_velocity_per_sec = event_velocity;
    diag.significant_change = trigger_metrics.significant_change;
    diag.energy_fast_track = trigger_metrics.energy_fast_track;
    diag.training_forced = isDataCollection;
    diag.trigger_time_us = now_us;
    diag.last_update_age_us =
        (m_lastHMMUpdateTimeUS > 0) ? (now_us - m_lastHMMUpdateTimeUS) : 0;
    diag.hmm_success = false;
    return diag;
}

// ============================================================================
// MAIN: CheckAndTriggerHMM - Dual-Path Institutional HMM Pipeline
// ============================================================================
// ELITE ARCHITECTURE: Unified trigger condition with asymmetric output routing
//
// TRIGGER DECISION (identical for both paths):
//   • Trigger IF: Mahalanobis significant change OR first initialization
//   • Collection mode: trigger only on init or meaningful observation change
//   • Adaptive noise floor: Velocity-based (conservative lunch, reactive hot market)
//
// PATH 1 - TRAINING CONTEXT (isDataCollection=true):
//   • Route triggered observation to .context stream
//   • Write to LBRFileManager.m_contextStream (.context binary file)
//   • FlatBuffer size-prefixed format (institutional standard)
//   • Used by: training data analysis and lookahead labeling
//
// PATH 2 - LIVE TRADING CONTEXT (isDataCollection=false):
//   • Route triggered observation to ZMQ serialization
//   • Serialize to MarketObservation + SystemState FlatBuffers
//   • Send to Python HMM server (port 5561, DEALER/ROUTER async)
//   • Response updates regime probabilities in PositionManager/RiskManager
//
// INSTITUTIONAL QUALITY GATES:
//   • L1-norm + Mahalanobis change detection for robust regime identification
//   • Comprehensive diagnostics: Every trigger logged for production observability
// ============================================================================
void ContextManager::CheckAndTriggerHMM(uint64_t now_us, bool isDataCollection, float syntheticVelocity) {
    // ========================================================================
    // PHASE 1: Calculate Event Velocity (Events Per Second)
    // ========================================================================
    // If caller provides a synthetic velocity (>= 0), use it directly.
    // Data-collection / replay passes NumberOfTrades/SecondsPerBar (EMA-smoothed)
    // because sub-bar timestamps are clamped to bar-open during replay.
    // Live path (SCStudies) omits the parameter → default -1 → timestamp-based.
    float event_velocity = (syntheticVelocity >= 0.0f)
        ? syntheticVelocity
        : CalculateEventVelocity(now_us);
    m_lastEventVelocityPerSec = event_velocity;

    // [NEW] Elite v3.0: Update Burstiness Loop
    m_latestInstitutionalMetrics.raschkeBurst = CalculateBurstinessIndex(now_us);

    const bool ts1MacroReady = AreTs1DimsReady(now_us, kTs1MacroMaxAgeUs);
    if (!ts1MacroReady) {
        ++m_telemetryCounters.ts1MacroReject;
        if (ShouldSampleLog(m_telemetryCounters.ts1MacroReject, 3, kTs1MacroRejectLogEvery)) {
            Logger::getInstance().log(
                "ContextManager::CheckAndTriggerHMM SKIP: TS1 macro dims unavailable/stale "
                "(count=" + std::to_string(m_telemetryCounters.ts1MacroReject) +
                ", reset_generation=" + std::to_string(GetResetGeneration()) +
                ", seen_after_reset=" + std::to_string(m_ts1SeenAfterReset.load(std::memory_order_relaxed) ? 1 : 0) +
                ", age_us=" + std::to_string(GetTs1MacroAgeUs(now_us)) +
                ", last_write_us=" + std::to_string(GetTs1MacroLastWriteUs()) +
                ", dim0=" + std::to_string(m_observationData.log_variance_ratio()) +
                ", dim6=" + std::to_string(m_observationData.hurst_exponent()) +
                ", dim8=" + std::to_string(m_observationData.fisher_info()) + ")"
            );
        }
        return;
    }

    const bool ts2StructuralReady = AreTs2StructuralDimsReady(now_us, kTs2StructuralMaxAgeUs);
    if (!ts2StructuralReady) {
        ++m_telemetryCounters.ts2StructuralReject;
        if (ShouldSampleLog(m_telemetryCounters.ts2StructuralReject, 3, kTs2StructuralRejectLogEvery)) {
            Logger::getInstance().log(
                "ContextManager::CheckAndTriggerHMM SKIP: TS2 structural dims unavailable/stale "
                "(count=" + std::to_string(m_telemetryCounters.ts2StructuralReject) +
                ", reset_generation=" + std::to_string(GetResetGeneration()) +
                ", seen_after_reset=" + std::to_string(HasTs2SeenAfterReset() ? 1 : 0) +
                ", age_us=" + std::to_string(GetTs2StructuralAgeUs(now_us)) +
                ", last_write_us=" + std::to_string(GetTs2StructuralLastWriteUs()) +
                ", recurrence=" + std::to_string(m_observationData.recurrence_rate()) +
                ", fractal=" + std::to_string(m_observationData.fractal_dim()) + ")"
            );
        }
        return;
    }

    // ========================================================================
    // PHASE 2: Build 16D Observation Vector + 8D Asymmetry Context
    // ========================================================================
    // Build physics core (16D)
    auto rawObs = BuildObservationVector();
    uint64_t non_finite_count = 0;
    uint64_t clamped_count = 0;
    rawObs = SanitizeObservationVector(rawObs, non_finite_count, clamped_count);
    m_telemetryCounters.sanitizedNonFinite += non_finite_count;
    m_telemetryCounters.sanitizedClamped += clamped_count;

    const uint64_t sanitized_total = non_finite_count + clamped_count;
    if (sanitized_total > 0 &&
        ShouldSampleLog(m_telemetryCounters.sanitizedClamped + m_telemetryCounters.sanitizedNonFinite,
                        3,
                        kSanitizedObservationLogEvery)) {
        Logger::getInstance().log(
            "ContextManager::ObservationSanitized non_finite=" + std::to_string(non_finite_count) +
            " clamped=" + std::to_string(clamped_count) +
            " totals(non_finite=" + std::to_string(m_telemetryCounters.sanitizedNonFinite) +
            ", clamped=" + std::to_string(m_telemetryCounters.sanitizedClamped) + ")"
        );
    }

    // Guard scaler state from contamination by non-finite raw inputs.
    if (!ValidateObservationVector(rawObs)) {
        return;
    }

    // Build context gate (8D)
    auto asymContext = GetAsymmetryContext();

    // ========================================================================
    // PHASE 2A: Hybrid Feature Scaling (Soft-Log-Z + Log-Z)
    // ========================================================================
    // State geometry dims use rolling z-score with symmetric soft-log compression.
    // Energy magnitude dims use rolling Log-Z with 6-sigma winsorization.
    // Note: AsymmetryContext 8D is NOT scaled here. It is used as raw embedding lookup.
    auto currentObs = m_featureScaler.UpdateAndNormalize(rawObs);
    m_latestScaledObs = currentObs;  // Store for LockC and external consumers

    // Feature Scaler Warmup Guard: suppress ALL downstream processing until
    // the 500-sample hybrid buffers are fully primed.
    // During warmup, scaled values are unstable and would contaminate:
    //   - Observation history (Mahalanobis distance baseline)
    //   - .context file records (training data quality)
    //   - Live HMM regime detection (inference quality)
    if (!m_featureScaler.warmedUp) {
        if (m_featureScaler.sampleCount == 1) {
            Logger::getInstance().log(
                "ContextManager::FeatureScaler WARMUP: accumulating " +
                std::to_string(FeatureScaler::RANK_WINDOW) +
                " observations before emission (dim0_raw=" +
                std::to_string(rawObs[OBS_LOG_VARIANCE_RATIO]) +
                ", log_dim0=" +
                std::to_string(FeatureScaler::ToLogEnergy(rawObs[OBS_LOG_VARIANCE_RATIO])) +
                ")"
            );
        }
        return;
    }

    // One-shot diagnostic: log first hybrid-scaled observation after warmup
    if (m_featureScaler.sampleCount == FeatureScaler::RANK_WINDOW) {
        Logger::getInstance().log(
            "ContextManager::FeatureScaler READY: hybrid scaler active (softlogz+logz). "
            "dim0_logz=" + std::to_string(currentObs[OBS_LOG_VARIANCE_RATIO]) +
            " dim9_logz=" + std::to_string(currentObs[OBS_TAIL_INDEX]) +
            " dim12_logz=" + std::to_string(currentObs[OBS_LIQ_FRAGILITY]) +
            " dim13_softlogz=" + std::to_string(currentObs[OBS_RECURRENCE_RATE]) +
            " after " + std::to_string(m_featureScaler.sampleCount) + " samples"
        );
    }

    // ========================================================================
    // PHASE 2B: Single Validation Boundary (ContextManager Contract)
    // ========================================================================
    // Contract: If HMM update is emitted from ContextManager, observation is valid.
    // Downstream components (HMMClient/.context writer) can trust this precondition.
    if (!ValidateObservationVector(currentObs)) {
        return;
    }

    // ========================================================================
    // PHASE 2B.1: Institutional Observation Staleness Telemetry
    // ========================================================================
    bool any_observation_changed = false;
    if (isDataCollection) {
        any_observation_changed = UpdateCollectionObservationTelemetry(rawObs, currentObs);
    }

    // ========================================================================
    // PHASE 2C: Observation History (Mahalanobis baseline)
    // ========================================================================
    // Maintain rolling window for Mahalanobis distance calculation.
    // Saturation is determined solely by FeatureScaler warmup (500 samples),
    // which is the single authority for data-readiness.
    m_observationHistory.push_back(currentObs);
    if (m_observationHistory.size() > OBS_SATURATION_MIN_SAMPLES) {
        m_observationHistory.pop_front();
    }

    // ========================================================================
    // PHASE 3: Calculate L1-norm Distance (Change Detection)
    // ========================================================================
    const float l1_accumulation = ComputeL1DistanceFromBaseline(currentObs);

    // ========================================================================
    // PHASE 4: Get Adaptive Noise Floor (Velocity-Dependent Threshold)
    // ========================================================================
    float adaptive_noise_floor = GetAdaptiveNoiseFloor(event_velocity);

    // ========================================================================
    // PHASE 5: Evaluate Trigger Conditions
    // ========================================================================
    const TriggerDecisionMetrics trigger_metrics =
        ComputeTriggerDecisionMetrics(currentObs, event_velocity);

    const bool should_trigger = ShouldTriggerHMM(
        m_hmmInitialized,
        trigger_metrics.significant_change,
        isDataCollection,
        any_observation_changed);

    // ========================================================================
    // PHASE 6: Populate Diagnostics (Institutional Observability)
    // ========================================================================
    HMMTriggerDiagnostics diag = BuildTriggerDiagnostics(
        now_us,
        isDataCollection,
        event_velocity,
        l1_accumulation,
        adaptive_noise_floor,
        trigger_metrics);

    // Store diagnostics even for skipped triggers (useful for production monitoring)
    if (!should_trigger) {
        diag.sequence_id = m_lastSequenceId;
        m_lastTriggerDiagnostics = diag;
        return;  // Efficiency gate: no update needed
    }

    // ========================================================================
    // PHASE 7: DUAL-PATH LOGIC - Training vs Live Trading
    // ========================================================================
    m_lastSequenceId++;
    diag.sequence_id = m_lastSequenceId;

    if (isDataCollection) {
        diag.hmm_success = EmitTrainingContext(currentObs, asymContext, now_us);
    } else {
        diag.hmm_success = EmitLiveContext(currentObs, m_lastSequenceId, now_us);
    }

    // ========================================================================
    // PHASE 8: Update State & Store Diagnostics
    // ========================================================================
    m_hmmObservation = currentObs;
    m_hmmInitialized = true;
    m_lastHMMUpdateTimeUS = now_us;
    m_lastTriggerDiagnostics = diag;
}

void ContextManager::Reset(uint64_t reset_reference_time_us) {
    const uint64_t generation = m_resetGeneration.fetch_add(1, std::memory_order_relaxed) + 1;

    // Preserve valid cross-chart ownership snapshots across epoch reset.
    // TS1/TS2 studies own dims that may not rewrite immediately after arm.
    const float savedDim0 = m_observationData.log_variance_ratio();
    const float savedDim1 = m_observationData.burstiness_index();
    const float savedDim2 = m_observationData.relative_range();
    const float savedDim3 = m_observationData.correction_action();
    const float savedDim6 = m_observationData.hurst_exponent();
    const float savedDim8 = m_observationData.fisher_info();
    const float savedDim13 = m_observationData.recurrence_rate();
    const float savedDim14 = m_observationData.fractal_dim();

    const uint64_t savedTs1WriteUs = m_ts1MacroLastWriteUs.load(std::memory_order_relaxed);
    const bool savedTs1Seen = m_ts1SeenAfterReset.load(std::memory_order_relaxed);
    const uint64_t savedTs2WriteUs = m_ts2StructuralLastWriteUs.load(std::memory_order_relaxed);
    const bool savedTs2Seen = m_ts2SeenAfterReset.load(std::memory_order_relaxed);

    const bool ts1SnapshotValid =
        savedTs1Seen && savedTs1WriteUs > 0 &&
        std::isfinite(savedDim0) && std::isfinite(savedDim6) && std::isfinite(savedDim8);

    const bool ts2SnapshotValid =
        savedTs2Seen && savedTs2WriteUs > 0 &&
        std::isfinite(savedDim1) && std::isfinite(savedDim2) && std::isfinite(savedDim3) &&
        std::isfinite(savedDim13) && std::isfinite(savedDim14) &&
        savedDim13 >= 0.0f && savedDim13 <= 1.0f &&
        savedDim14 >= 1.0f && savedDim14 <= 2.0f;

    m_waveContext = StatisticalContext{};
    m_rippleContext = StatisticalContext{};
    m_hasWaveContext = false;
    m_hasRippleContext = false;
    m_anchors.reset();
    m_dailyCache = {};
    m_observationData = MTS::Schema::ObservationData();
    m_asymmetryContext = MTS::Schema::AsymmetryContext();
    m_hmmInitialized = false;
    m_eventTimestampsUS.clear();
    m_observationHistory.clear();
    m_lastSequenceId = 0;
    m_hmmObservation.fill(0.0f);
    m_prevCollectionObservation.fill(0.0f);
    m_hasPrevCollectionObservation = false;
    m_staleRunLength.fill(0);
    m_changeCount.fill(0);
    m_collectionObservationCount = 0;
    m_telemetryCounters = {};
    m_lastHMMUpdateTimeUS = 0;
    m_lastEventVelocityPerSec = 0.0f;
    m_regimeTenureCounter = 0;
    m_structureEngine.Reset();
    m_featureScaler.Reset();
    m_latestScaledObs.fill(0.0f);
    m_ts1MacroLastWriteUs.store(0, std::memory_order_relaxed);
    m_ts1MacroLastSteadyUs.store(0, std::memory_order_relaxed);
    m_ts2StructuralLastWriteUs.store(0, std::memory_order_relaxed);
    m_ts2StructuralLastSteadyUs.store(0, std::memory_order_relaxed);
    m_ts1SeenAfterReset.store(false, std::memory_order_relaxed);
    m_ts2SeenAfterReset.store(false, std::memory_order_relaxed);

    // D1 fix: reset members previously missed
    m_latestInstitutionalMetrics = InstitutionalMetrics{};
    m_localRiskContext = LocalRiskContext{};
    m_cachedHillAlpha.store(4.0f, std::memory_order_relaxed);
    m_lastLogReturn = 0.0f;
    m_lastTriggerDiagnostics.reset();
    m_prevEfficiency = 0.0f;
    m_prevVolatility = 0.0f;

    // D2 fix: reset physics engines to clear stale circular buffers
    m_infoEngine.Reset();
    m_tailRiskEngine.Reset();

    const uint64_t ts1AnchorWriteUs = (reset_reference_time_us > 0)
        ? reset_reference_time_us
        : savedTs1WriteUs;
    const uint64_t ts2AnchorWriteUs = (reset_reference_time_us > 0)
        ? reset_reference_time_us
        : savedTs2WriteUs;

    if (ts1SnapshotValid) {
        m_observationData.mutate_log_variance_ratio(savedDim0);
        m_observationData.mutate_hurst_exponent(savedDim6);
        m_observationData.mutate_fisher_info(savedDim8);
        m_ts1MacroLastWriteUs.store(ts1AnchorWriteUs, std::memory_order_relaxed);
        m_ts1MacroLastSteadyUs.store(GetSteadyNowUs(), std::memory_order_relaxed);
        m_ts1SeenAfterReset.store(true, std::memory_order_relaxed);
    }

    if (ts2SnapshotValid) {
        m_observationData.mutate_burstiness_index(savedDim1);
        m_observationData.mutate_relative_range(savedDim2);
        m_observationData.mutate_correction_action(savedDim3);
        m_observationData.mutate_recurrence_rate(savedDim13);
        m_observationData.mutate_fractal_dim(savedDim14);
        m_ts2StructuralLastWriteUs.store(ts2AnchorWriteUs, std::memory_order_relaxed);
        m_ts2StructuralLastSteadyUs.store(GetSteadyNowUs(), std::memory_order_relaxed);
        m_ts2SeenAfterReset.store(true, std::memory_order_relaxed);
    }

    Logger::getInstance().log(
        "ContextManager::Reset HARD generation=" + std::to_string(generation) +
        " instance=" + std::to_string(reinterpret_cast<uint64_t>(this)) +
        " ts1_snapshot=" + std::to_string(ts1SnapshotValid ? 1 : 0) +
        " ts2_snapshot=" + std::to_string(ts2SnapshotValid ? 1 : 0) +
        " reset_reference_time_us=" + std::to_string(reset_reference_time_us)
    );

}

// Utility: Burstiness Index (Shannon-Pareto Flow Analysis)
// Coefficients of Variation of Inter-Arrival Times (IAT)
// CV = StdDev(IAT) / Mean(IAT)
// CV ~ 1.0 -> Poisson (Random/Efficient)
// CV > 1.0 -> Pareto (Clustered/Informed)
// CV < 1.0 -> Regular (Periodic/Algorithmic)
float ContextManager::CalculateBurstinessIndex(uint64_t now_us) {
    (void)now_us; // Unused, uses internal history
    if (m_eventTimestampsUS.size() < 4) return 1.0f; // Need samples for variance

    // Convert timestamps to IATs — stack-allocated (max 99 IATs, ~400 bytes)
    std::array<float, EVENT_VELOCITY_MAX> iats;
    size_t iatCount = 0;

    uint64_t prev = 0;
    bool first = true;
    // Iterate from oldest to newest
    for (const auto& ts : m_eventTimestampsUS) {
        if (first) {
            prev = ts;
            first = false;
            continue;
        }
        // IAT in milliseconds for numerical stability
        float iat_ms = static_cast<float>(ts - prev) / 1000.0f;
        if (iat_ms < 0.001f) iat_ms = 0.001f; // Clamp zero IATs
        iats[iatCount++] = iat_ms;
        prev = ts;
    }

    if (iatCount == 0) return 1.0f; // Default to Poisson

    // Calculate Mean IAT
    float sum = 0.0f;
    for (size_t i = 0; i < iatCount; ++i) sum += iats[i];
    float mean = sum / static_cast<float>(iatCount);

    // Calculate StdDev IAT
    float sumSq = 0.0f;
    for (size_t i = 0; i < iatCount; ++i) {
        const float d = iats[i] - mean;
        sumSq += d * d;
    }
    float stdDev = std::sqrt(sumSq / (iatCount > 1 ? static_cast<float>(iatCount - 1) : 1.0f));

    if (mean < 0.0001f) return 1.0f; // Avoid division by zero

    return stdDev / mean;
}


// 1200
