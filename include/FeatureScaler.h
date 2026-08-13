// FeatureScaler.h — pure, header-only hybrid Soft-Log-Z/Log-Z scaler for
// ContextManager's 16D observation vector. Extracted from ContextManager.h
// (docs/superpowers/specs/2026-08-07-contextmanager-ring-buffer-dod-design.md)
// so it can be natively unit-tested (tests/cpp/test_feature_scaler.cpp) —
// it has zero Sierra Chart/ACSIL dependency and was only ever unreachable by
// a standalone test because ContextManager.h itself pulls in sierrachart.h.
// Same rationale/precedent as InformationEngine.h/TailRiskEngine.h/
// StructureEngine.h already being extracted this way.

#pragma once

#include <array>
#include <cmath>
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <utility>
#include "generated/mts_schema_contract_generated.h"
#include "RingBuffer.h"

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
    /// Dim 13 RE-derived 2026-08-13 (final-review Finding 1). The 2026-07-23
    /// constants (center 0.329, scale 0.083) were calibrated against the OLD
    /// RQA epsilon heuristic max(range*0.1, std*0.5). That heuristic was
    /// replaced by a fixed-target-recurrence-rate selector (RQAEpsilonSelector.h;
    /// target 0.05 full-matrix, line-of-identity included) and the raw dim moved
    /// to a ~0.033-0.15 band, so the old constants mapped every production value
    /// to z ~= -3.2 -- a Soft-Log-Z p01..p99 span of only 0.215, i.e. the
    /// dimension-collapse pathology this initiative exists to remove.
    /// Methodology (same as Task 7's kurtosis re-derivation: real data, not
    /// synthetic): tools/rqa_recurrence_calibration.cpp runs the shipped
    /// CalculateRecurrenceRate logic verbatim (n=30 -- TripleScreen2's
    /// slow_window_n is deterministically 30; bar-gated 200-bar epsilon
    /// recalibration) over 18,742 real 60-minute MES bars resampled from
    /// lbrnet/data/raw/mes_ripple_15m.parquet (2023-06-04 to 2026-08-10).
    /// Result: median 0.04667, MAD 0.00667 -> 1.4826*MAD = 0.00988. Stable
    /// across split-halves and 3 yearly sub-periods (median 0.0444-0.0489,
    /// scale 0.0099-0.0132). Verified non-degenerate: Soft-Log-Z p01..p99 span
    /// widens from 0.215 (old) to 2.80 (new), 34 distinct scaled values.
    static constexpr float RECURRENCE_STATIC_CENTER = 0.0467f;   ///< empirical median (was 0.329)
    static constexpr float RECURRENCE_STATIC_SCALE = 0.0099f;    ///< 1.4826*raw MAD (was 0.083)
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
    // Fixed-capacity ring buffers (docs/superpowers/specs/2026-08-07-
    // contextmanager-ring-buffer-dod-design.md) -- zero heap allocation for
    // the buffer's lifetime, replacing std::deque's ongoing chunk churn as
    // the window slides. Capacity = RANK_WINDOW + 1 for headroom: push_back
    // then conditionally pop_front (below) transiently holds one more
    // element than the per-dim logical window between those two statements.
    std::array<RingBuffer<float, RANK_WINDOW + 1>, N_DIMS> stateBuffers; // SOFTLOGZ dims use raw rolling z-score
    std::array<RingBuffer<float, RANK_WINDOW + 1>, N_DIMS> logBuffers;
    // Latest rolling robust stats for diagnostics/telemetry.
    std::array<float, N_DIMS> latestLogMedian = {};
    std::array<float, N_DIMS> latestLogScale = {};  ///< MAD × 1.4826

    std::array<DimCalibration, N_DIMS> calibration = {};  ///< Per-dim adaptive state

    /// Per-dim result of the last Calibrate()/Recalibrate() dominance check:
    /// fraction of the rolling window occupied by its single most-frequent
    /// exact value. Exposed (not logged here -- this header has zero
    /// ACSIL/Logger dependency by design) so a caller with Logger access can
    /// flag a sentinel-collapse signature
    /// (docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md D2).
    ///
    /// SEMANTIC SHIFT (2026-08-13, final-review pass): Task 1's consecutive-identical
    /// dedupe (`3d5f9af`) drops repeats BEFORE they reach the rolling buffer this
    /// diagnostic scans, so this ratio now measures dominance among DISTINCT
    /// observations, no longer fraction-of-time-held. The 0.30 ALERT threshold
    /// documented in that spec was calibrated on the OLD (pre-dedupe) meaning and
    /// is therefore harder to trip now; reconsider it if it starts firing
    /// unexpectedly post-dedupe (a post-dedupe hit is a stronger signal than a
    /// pre-dedupe one -- it means genuinely distinct samples keep landing on the
    /// same exact value, not merely that a held value repeated).
    std::array<float, N_DIMS> dominanceRatio = {};

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
    static std::pair<float, float> RobustLocation(const RingBuffer<float, RANK_WINDOW + 1>& buf) {
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

    /// Fraction of `buf` occupied by its single most-frequent exact value.
    /// O(n log n) via sort-then-scan-longest-equal-run on the same stack
    /// scratch RobustLocation() already uses -- only called from
    /// Calibrate()/Recalibrate() (warmup completion + every
    /// RECALIBRATION_INTERVAL samples), never per-tick. Pure, native-testable.
    static float ComputeValueDominance(const RingBuffer<float, RANK_WINDOW + 1>& buf) {
        const int n = static_cast<int>(buf.size());
        if (n == 0) return 0.0f;
        std::array<float, RANK_WINDOW> scratch;
        for (int j = 0; j < n; ++j) scratch[static_cast<size_t>(j)] = buf[static_cast<size_t>(j)];
        std::sort(scratch.begin(), scratch.begin() + n);
        int maxRun = 1;
        int currentRun = 1;
        for (int j = 1; j < n; ++j) {
            if (scratch[static_cast<size_t>(j)] == scratch[static_cast<size_t>(j - 1)]) {
                ++currentRun;
                maxRun = std::max(maxRun, currentRun);
            } else {
                currentRun = 1;
            }
        }
        return static_cast<float>(maxRun) / static_cast<float>(n);
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
        // Dedupe-at-ingestion: a value identical to the window's most recent entry
        // carries zero marginal Shannon information (H(X_t|X_{t-1})=0 for an exact
        // repeat) and must not be treated as a fresh independent draw by the
        // median/MAD estimator below -- see docs/superpowers/specs/
        // 2026-08-13-observation-vector-institutional-elevation-spec.md Unit 1.
        for (size_t i = 0; i < N_DIMS; ++i) {
            const size_t winSize = DIM_WINDOW_SIZE[i];
            if (SCALE_MODE_MAP[i] == ScaleMode::SOFTLOGZ) {
                auto& buf = stateBuffers[i];
                if (buf.empty() || buf.back() != raw[i]) {
                    buf.push_back(raw[i]);
                    if (buf.size() > winSize) {
                        buf.pop_front();
                    }
                }
            } else {
                const float logValue = ToLogEnergy(raw[i]);
                auto& buf = logBuffers[i];
                if (buf.empty() || buf.back() != logValue) {
                    buf.push_back(logValue);
                    if (buf.size() > winSize) {
                        buf.pop_front();
                    }
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
                dominanceRatio[i] = ComputeValueDominance(buf);
            } else {
                const auto& buf = logBuffers[i];
                if (buf.size() >= 5) {
                    bufScale = RobustLocation(buf).second;
                }
                dominanceRatio[i] = ComputeValueDominance(buf);
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
                dominanceRatio[i] = ComputeValueDominance(buf);
            } else {
                const auto& buf = logBuffers[i];
                if (buf.size() >= 5) {
                    bufScale = RobustLocation(buf).second;
                }
                dominanceRatio[i] = ComputeValueDominance(buf);
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
        dominanceRatio.fill(0.0f);
        calibrated = false;
        modeMapValidated = false;
        modeMapIsValid = false;
        sampleCount = 0;
        warmedUp = false;
    }
};
