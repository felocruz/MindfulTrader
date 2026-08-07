#pragma once

#include "sierrachart.h"
#include <optional>
#include <array>
#include <deque>
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

/// Hybrid Feature Scaler: Soft-Log-Z state channels + winsorized Log-Z energy channels.
///
/// Institutional rationale (Topology Preservation for Student-t HMM):
/// - State/geometry channels keep relative outlier distance via symmetric Soft-Log-Z.
/// - Energy/magnitude channels keep amplitude via rolling Log-Z with 6-sigma winsorization.
///
/// Architecture:
///   LOGZ dims:      2, 4, 12
///   SOFTLOGZ dims:  0, 1, 3, 5, 6, 7, 8, 9, 10, 11, 13, 14, 15
///   Warmup:         500 samples for stable rolling windows
///
/// After warmup, output is model-ready hybrid space:
/// - energy dims are winsorized z-space
/// - state dims are sign(z) * log1p(|z|)
/// No Python-side scaling needed: load .context and hit model.train().
struct FeatureScaler {
    enum class ScaleMode : uint8_t { SOFTLOGZ = 0, LOGZ = 1 };

    static constexpr size_t N_DIMS = MTS::Schema::Contract::kObservationDim;
    static constexpr float ABSOLUTE_FLOOR = 1e-8f;              ///< Numerical safety floor (per-dim minimum)
    static constexpr float RELATIVE_FLOOR_FRACTION = 0.01f;     ///< 1% of burn-in median stddev per dim

    /// Dim 11 (amihud_illiquidity): ABSOLUTE_FLOOR (1e-8) permanently zero-trapped
    /// this dimension. Amihud = |log-return| / dollar-volume; on a liquid index
    /// future its rolling MAD is ~1e-11 to 1e-10 -- two to three orders of
    /// magnitude below the floor -- so madScale < minScaleFloor fired on every
    /// single observation and the carry-forward death spiral (never leaving
    /// hasValidScaled=false) returned 0.0f forever. Unlike DIM_LZ_INDEX/
    /// DIM_RECURRENCE_INDEX/DIM_FRACTAL_INDEX below, Amihud's raw scale is NOT
    /// instrument-invariant (it scales with an instrument's price level and
    /// dollar volume, unlike those three bounded structural indices), so a
    /// static center/scale would silently break if this study ever ran against
    /// a different instrument -- lowering the floor instead preserves the
    /// existing adaptive rolling median/MAD calibration for this dim, just lets
    /// it actually clear the gate. See logs/rc_gemini.log CLAUDE_BRIEF_086/087
    /// and GEMINI_BRIEF_087_RESPONSE for the full derivation.
    static constexpr size_t DIM_AMIHUD_INDEX = 11;               ///< == OBS_AMIHUD_ILLIQUIDITY
    static constexpr float AMIHUD_ABSOLUTE_FLOOR = 1e-16f;       ///< Negligible vs. ABSOLUTE_FLOOR; still divide-by-zero-safe
    static constexpr float LOG_BPS = 10000.0f;                  ///< Basis-point multiplier inside log transform
    static constexpr float LOG_EPS = 1e-8f;                     ///< Non-zero floor before log transform
    static constexpr float ENERGY_WINSOR_SIGMA = 6.0f;          ///< 6-sigma winsorization for energy channels
    static constexpr float STATE_WINSOR_SIGMA = 6.0f;           ///< 6-sigma winsorization before Soft-Log map
    static constexpr size_t RANK_WINDOW = 500;                   ///< Max rolling window (scratch array sizing + warmup gate)
    static constexpr size_t EXPECTED_LOGZ_DIMS = 3;
    static constexpr size_t RECALIBRATION_INTERVAL = 5000;       ///< Re-examine adaptive floors every N samples
    static constexpr size_t CARRY_DECAY_HALFLIFE = 200;          ///< Carry-forward exponential decay half-life (samples)

    /// Dim 5 (lempel_ziv) static scaler — bypasses rolling MAD.
    /// LZ76 on n=64 binary string produces ~10 discrete values (step ≈ 0.094).
    /// MAD(identical values) = 0.0 → guaranteed carry-forward death spiral.
    /// Static center/scale keeps the signal alive without zero-denominator risk.
    static constexpr size_t DIM_LZ_INDEX = 5;
    static constexpr float LZ_STATIC_CENTER = 0.5f;              ///< Theoretical mean of LZ76 for n=64
    static constexpr float LZ_STATIC_SCALE = 0.25f;              ///< Maps [0,1] → [-2,+2] z-score range

    /// Dim 13/14 (recurrence/fractal): bounded structural channels.
    /// Use static bounded scaling to avoid MAD-floor carry-forward collapse
    /// on slow/discrete regimes while preserving topology via Soft-Log-Z.
    static constexpr size_t DIM_RECURRENCE_INDEX = 13;
    static constexpr size_t DIM_FRACTAL_INDEX = 14;
    /// Recalibrated 2026-07-23 against real event_data.context (500k-sample pull):
    /// original constants assumed symmetric use of the theoretical contract range,
    /// but real data centers well off that assumption and only spans a narrow band.
    /// Recentered to empirical raw median; rescaled to 1.4826*empirical raw MAD
    /// (the same Taleb-consistent convention RobustLocation() uses for adaptive
    /// dims below), so real variation spans the intended [-2,+2] ToSoftLogZ range
    /// instead of a narrow off-center sliver. See docs/hmm/STUDENT_T_HMM_RUNBOOK.md
    /// (lbrnet) item 4b and logs/rc_gemini.log CLAUDE_BRIEF_024/025 for derivation.
    static constexpr float RECURRENCE_STATIC_CENTER = 0.329f;    ///< empirical median (was 0.5)
    static constexpr float RECURRENCE_STATIC_SCALE = 0.083f;     ///< 1.4826*raw MAD (was 0.25)
    static constexpr float FRACTAL_STATIC_CENTER = 1.289f;       ///< empirical median (was 1.5)
    static constexpr float FRACTAL_STATIC_SCALE = 0.101f;        ///< 1.4826*raw MAD (was 0.25)

    /// Per-dim rolling window depths — tuned to decorrelation characteristics.
    /// Short-memory features (bounded, fast-decorrelating) use shorter windows.
    /// Long-memory features (structural, slow-decorrelating) use longer windows.
    inline static constexpr std::array<size_t, N_DIMS> DIM_WINDOW_SIZE = {
        500,  //  0  log_variance_ratio    medium memory
        300,  //  1  burstiness_index      bursty, shorter
        500,  //  2  relative_range        LOGZ, stable
        300,  //  3  correction_action     regime-reactive
        500,  //  4  vol_convexity         LOGZ, stable
        150,  //  5  lempel_ziv            bounded [0,1], static scaler (MAD-immune)
        200,  //  6  hurst_exponent        bounded ~[0,1], moderate
        300,  //  7  micro_asymmetry       order flow, bursty
        500,  //  8  fisher_info           geometry, stable
        500,  //  9  tail_index            Hill needs depth
        300,  // 10  skewness              regime-reactive
        300,  // 11  amihud_illiquidity    regime-sensitive
        500,  // 12  liq_fragility         LOGZ, stable
        200,  // 13  recurrence_rate       structural, periodic
        200,  // 14  fractal_dim           structural, slow
        300,  // 15  mean_rev_z            medium memory
    };

    /// Per-dimension adaptive scaling calibration.
    /// Calibrated at warmup and periodically recalibrated every RECALIBRATION_INTERVAL.
    struct DimCalibration {
        float minScaleFloor = ABSOLUTE_FLOOR; ///< Adaptive MAD-scale floor (robust)
        float lastValidScaled = 0.0f;         ///< Carry-forward cache
        bool  hasValidScaled = false;         ///< Guard: no carry-forward before first real value
        size_t carryForwardCount = 0;         ///< Consecutive carry-forward samples (Shannon decay)
    };

    inline static constexpr std::array<ScaleMode, N_DIMS> SCALE_MODE_MAP = {
        ScaleMode::SOFTLOGZ, // 0
        ScaleMode::SOFTLOGZ, // 1
        ScaleMode::LOGZ, // 2
        ScaleMode::SOFTLOGZ, // 3
        ScaleMode::LOGZ, // 4
        ScaleMode::SOFTLOGZ, // 5
        ScaleMode::SOFTLOGZ, // 6
        ScaleMode::SOFTLOGZ, // 7
        ScaleMode::SOFTLOGZ, // 8
        ScaleMode::SOFTLOGZ, // 9
        ScaleMode::SOFTLOGZ, // 10
        ScaleMode::SOFTLOGZ, // 11
        ScaleMode::LOGZ, // 12
        ScaleMode::SOFTLOGZ, // 13
        ScaleMode::SOFTLOGZ, // 14
        ScaleMode::SOFTLOGZ  // 15
    };

    // Rolling FIFO buffers by mode.
    std::array<std::deque<float>, N_DIMS> stateBuffers; // SOFTLOGZ dims use raw rolling z-score
    std::array<std::deque<float>, N_DIMS> logBuffers;
    // Latest rolling robust stats for diagnostics/telemetry.
    std::array<float, N_DIMS> latestLogMedian = {};
    std::array<float, N_DIMS> latestLogScale = {};  ///< MAD × 1.4826

    std::array<DimCalibration, N_DIMS> calibration = {};  ///< Per-dim adaptive state
    bool calibrated = false;                               ///< Set once at warmup completion

    bool modeMapValidated = false;
    bool modeMapIsValid = false;
    size_t sampleCount = 0;
    bool warmedUp = false;

    bool EnsureModeMapValid() {
        if (modeMapValidated) {
            return modeMapIsValid;
        }

        size_t stateCount = 0;
        size_t logzCount = 0;
        for (ScaleMode mode : SCALE_MODE_MAP) {
            if (mode == ScaleMode::SOFTLOGZ) {
                ++stateCount;
            } else if (mode == ScaleMode::LOGZ) {
                ++logzCount;
            }
        }

        modeMapIsValid = ((stateCount + logzCount) == N_DIMS) && (logzCount == EXPECTED_LOGZ_DIMS);
        modeMapValidated = true;
        assert(modeMapIsValid && "FeatureScaler SCALE_MODE_MAP must match expected LOGZ/SOFTLOGZ partition");
        return modeMapIsValid;
    }

    static float ToLogEnergy(float rawValue) {
        const float magnitude = std::max(std::abs(rawValue), LOG_EPS);
        return std::log1p(magnitude * LOG_BPS);
    }

    static float ToSoftLogZ(float z) {
        const float zw = std::clamp(z, -STATE_WINSOR_SIGMA, STATE_WINSOR_SIGMA);
        return std::copysign(std::log1p(std::abs(zw)), zw);
    }

    /// Robust location + scale: median and MAD × 1.4826 (Taleb-consistent).
    /// Uses std::nth_element on stack scratch — O(n) average, zero heap allocation.
    /// Returns {median, madScale}. If fewer than 5 samples, returns {0, 0}.
    static std::pair<float, float> RobustLocation(const std::deque<float>& buf) {
        const int n = static_cast<int>(buf.size());
        if (n < 5) return {0.0f, 0.0f};

        std::array<float, RANK_WINDOW> scratch;  // Sized to max window (500)
        for (int j = 0; j < n; ++j) scratch[static_cast<size_t>(j)] = buf[static_cast<size_t>(j)];
        const int mid = n / 2;

        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
        const float median = scratch[static_cast<size_t>(mid)];

        for (int j = 0; j < n; ++j) {
            scratch[static_cast<size_t>(j)] = std::abs(buf[static_cast<size_t>(j)] - median);
        }
        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
        const float madScale = scratch[static_cast<size_t>(mid)] * 1.4826f;

        return {median, madScale};
    }

    /// Feed raw observation, return hybrid-scaled values.
    /// Safe from bar 1: returns neutral defaults until warmup.
    std::array<float, N_DIMS> UpdateAndNormalize(const std::array<float, N_DIMS>& raw) {
        std::array<float, N_DIMS> result;
        result.fill(0.0f);  // Neutral for signed topology-preserving space.

        if (!EnsureModeMapValid()) {
            return result;
        }

        // ── Step 1: Update rolling buffers by mode (per-dim window depth) ──
        for (size_t i = 0; i < N_DIMS; ++i) {
            const size_t winSize = DIM_WINDOW_SIZE[i];
            if (SCALE_MODE_MAP[i] == ScaleMode::SOFTLOGZ) {
                stateBuffers[i].push_back(raw[i]);
                if (stateBuffers[i].size() > winSize) {
                    stateBuffers[i].pop_front();
                }
            } else {
                const float logValue = ToLogEnergy(raw[i]);
                logBuffers[i].push_back(logValue);
                if (logBuffers[i].size() > winSize) {
                    logBuffers[i].pop_front();
                }
            }
        }

        ++sampleCount;

        if (sampleCount == 1) {
            latestLogMedian.fill(0.0f);
            latestLogScale.fill(0.0f);
            return result;
        }

        // ── Step 2: Scale by mode (SOFTLOGZ or LOGZ) ─────────────────────
        for (size_t i = 0; i < N_DIMS; ++i) {
            if (SCALE_MODE_MAP[i] == ScaleMode::SOFTLOGZ) {
                const auto& stateBuf = stateBuffers[i];
                if (stateBuf.empty()) {
                    result[i] = 0.0f;
                    continue;
                }

                const float current = stateBuf.back();

                // ── Dim 5 (lempel_ziv): Static global scaler ──────────────
                // LZ76 output is discrete (~10 levels for n=64).  MAD of
                // identical discrete values is exactly 0.0, which triggers
                // the carry-forward death spiral.  Static center/scale
                // keeps the signal alive without zero-denominator risk.
                if (i == DIM_LZ_INDEX) {
                    const float z = (current - LZ_STATIC_CENTER) / LZ_STATIC_SCALE;
                    result[i] = ToSoftLogZ(z);
                    calibration[i].lastValidScaled = result[i];
                    calibration[i].hasValidScaled = true;
                    calibration[i].carryForwardCount = 0;
                    continue;
                }

                // Structural bounded channels use static bounded scaling.
                if (i == DIM_RECURRENCE_INDEX) {
                    const float z = (current - RECURRENCE_STATIC_CENTER) / RECURRENCE_STATIC_SCALE;
                    result[i] = ToSoftLogZ(z);
                    calibration[i].lastValidScaled = result[i];
                    calibration[i].hasValidScaled = true;
                    calibration[i].carryForwardCount = 0;
                    continue;
                }

                if (i == DIM_FRACTAL_INDEX) {
                    const float z = (current - FRACTAL_STATIC_CENTER) / FRACTAL_STATIC_SCALE;
                    result[i] = ToSoftLogZ(z);
                    calibration[i].lastValidScaled = result[i];
                    calibration[i].hasValidScaled = true;
                    calibration[i].carryForwardCount = 0;
                    continue;
                }

                const auto [median, madScale] = RobustLocation(stateBuf);

                if (madScale < calibration[i].minScaleFloor) {
                    // Shannon decay: carry-forward value decays toward neutral
                    calibration[i].carryForwardCount++;
                    if (calibration[i].hasValidScaled) {
                        const float decay = std::exp2f(
                            -static_cast<float>(calibration[i].carryForwardCount)
                            / static_cast<float>(CARRY_DECAY_HALFLIFE));
                        result[i] = calibration[i].lastValidScaled * decay;
                    } else {
                        result[i] = 0.0f;
                    }
                    continue;
                }

                calibration[i].carryForwardCount = 0;  // Reset on live signal
                const float z = (current - median) / madScale;
                result[i] = ToSoftLogZ(z);
                calibration[i].lastValidScaled = result[i];
                calibration[i].hasValidScaled = true;
                continue;
            }

            const auto& logBuf = logBuffers[i];
            if (logBuf.empty()) {
                result[i] = 0.0f;
                latestLogMedian[i] = 0.0f;
                latestLogScale[i] = 0.0f;
                continue;
            }

            const float currentLog = logBuf.back();
            const auto [median, madScale] = RobustLocation(logBuf);

            latestLogMedian[i] = median;
            latestLogScale[i] = madScale;

            if (madScale < calibration[i].minScaleFloor) {
                // Shannon decay: carry-forward value decays toward neutral
                calibration[i].carryForwardCount++;
                if (calibration[i].hasValidScaled) {
                    const float decay = std::exp2f(
                        -static_cast<float>(calibration[i].carryForwardCount)
                        / static_cast<float>(CARRY_DECAY_HALFLIFE));
                    result[i] = calibration[i].lastValidScaled * decay;
                } else {
                    result[i] = 0.0f;
                }
                continue;
            }

            calibration[i].carryForwardCount = 0;  // Reset on live signal
            const float zLog = (currentLog - median) / madScale;
            result[i] = std::clamp(zLog, -ENERGY_WINSOR_SIGMA, ENERGY_WINSOR_SIGMA);
            calibration[i].lastValidScaled = result[i];
            calibration[i].hasValidScaled = true;
        }

        // Warmup gate: RANK_WINDOW samples for full rolling buffer depth
        if (!warmedUp && sampleCount >= RANK_WINDOW) {
            warmedUp = true;
            if (!calibrated) {
                Calibrate();
            }
        }

        // Rolling recalibration: update adaptive floors periodically (Mandelbrot regime fix)
        if (warmedUp && (sampleCount % RECALIBRATION_INTERVAL == 0)) {
            Recalibrate();
        }

        return result;
    }

    /// Snapshot per-dim MAD-scale from burn-in buffers → adaptive floors.
    /// Called once when warmedUp flips to true.
    void Calibrate() {
        for (size_t i = 0; i < N_DIMS; ++i) {
            float bufScale = 0.0f;
            if (SCALE_MODE_MAP[i] == ScaleMode::SOFTLOGZ) {
                const auto& buf = stateBuffers[i];
                if (buf.size() >= 5) {
                    bufScale = RobustLocation(buf).second;
                }
            } else {
                const auto& buf = logBuffers[i];
                if (buf.size() >= 5) {
                    bufScale = RobustLocation(buf).second;
                }
            }
            const float floorToUse = (i == DIM_AMIHUD_INDEX) ? AMIHUD_ABSOLUTE_FLOOR : ABSOLUTE_FLOOR;
            calibration[i].minScaleFloor = std::max(floorToUse, bufScale * RELATIVE_FLOOR_FRACTION);
        }
        calibrated = true;
    }

    /// Periodic recalibration: EMA-blend current MAD-scale into adaptive floors.
    /// Prevents the "Great Moderation" trap — floors track regime changes.
    /// Called every RECALIBRATION_INTERVAL samples after warmup.
    void Recalibrate() {
        for (size_t i = 0; i < N_DIMS; ++i) {
            float bufScale = 0.0f;
            if (SCALE_MODE_MAP[i] == ScaleMode::SOFTLOGZ) {
                const auto& buf = stateBuffers[i];
                if (buf.size() >= 5) {
                    bufScale = RobustLocation(buf).second;
                }
            } else {
                const auto& buf = logBuffers[i];
                if (buf.size() >= 5) {
                    bufScale = RobustLocation(buf).second;
                }
            }
            const float floorToUse = (i == DIM_AMIHUD_INDEX) ? AMIHUD_ABSOLUTE_FLOOR : ABSOLUTE_FLOOR;
            const float newFloor = std::max(floorToUse, bufScale * RELATIVE_FLOOR_FRACTION);
            // EMA blend: 70% old + 30% new — conservative, prevents floor whiplash
            calibration[i].minScaleFloor = 0.7f * calibration[i].minScaleFloor + 0.3f * newFloor;
        }
    }

    void Reset() {
        for (auto& buf : stateBuffers) buf.clear();
        for (auto& buf : logBuffers) buf.clear();
        latestLogMedian.fill(0.0f);
        latestLogScale.fill(0.0f);
        calibration = {};
        calibrated = false;
        modeMapValidated = false;
        modeMapIsValid = false;
        sampleCount = 0;
        warmedUp = false;
    }
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
    std::deque<uint64_t> m_eventTimestampsUS;       // Event-arrival timestamps (us); shared by CalculateEventVelocity() and CalculateBurstinessIndex()
    eve::VelocityState m_velocityState;              // EMA-based event velocity (Task 1, phase1-hardening plan)
    std::deque<std::array<float, OBSERVATION_VECTOR_SIZE>> m_observationHistory;
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
