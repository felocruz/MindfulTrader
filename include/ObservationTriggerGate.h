// ObservationTriggerGate.h — pure, header-only replica of ContextManager's
// Mahalanobis significant-change trigger gate (energy/geometry split-channel
// robust z-score, velocity-adaptive noise floor/epsilon, sanitization
// clamps). Extracted so it is reusable by a non-Sierra-Chart offline tool
// (the training-data generator) using the EXACT SAME code production runs,
// not a re-derived copy -- ContextManager composes an instance of this class
// rather than duplicating its logic inline (see ContextManager.h's
// m_triggerGate member).
//
// No Sierra Chart/ACSIL dependency.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>

#include "generated/mts_schema_contract_generated.h"
#include "RingBuffer.h"

namespace otg {

constexpr size_t kObservationDim = MTS::Schema::Contract::kObservationDim;

// Per-dim inclusive sanitization clamp bounds -- byte-identical to
// ContextManager.cpp's kObsLowerBounds/kObsUpperBounds (dim order matches
// mts_schema_contract_generated.h's kObs* field indices).
inline constexpr std::array<float, kObservationDim> kLowerBounds = {
    -6.0f, -6.0f, 0.0f, -6.0f, 0.0f, 0.0f, -1.0f, -6.0f,
    0.0f, 0.5f, -2.5f, 0.0f, 0.0f, 0.5f, 0.0f, 1.0f, 0.0f, 0.0f,
};
inline constexpr std::array<float, kObservationDim> kUpperBounds = {
    6.0f, 6.0f, 25.0f, 6.0f, 1.0f, 1.5f, 1.0f, 6.0f,
    1.5f, 8.0f, 2.5f, 100.0f, 1.0f, 8.0f, 1.0f, 2.0f, 5.0f, 5.0f,
};

// True for the 5 "energy"/magnitude channels that use a lower Mahalanobis
// trigger multiplier -- byte-identical set to ContextManager.cpp's
// IsEnergyObservationDim().
inline bool IsEnergyObservationDim(size_t dim) {
    return dim == MTS::Schema::Contract::kObsLogScaleRatio
        || dim == MTS::Schema::Contract::kObsRelativeRange
        || dim == MTS::Schema::Contract::kObsTailIndex
        || dim == MTS::Schema::Contract::kObsLiqFragility
        || dim == MTS::Schema::Contract::kObsFastTalebKurtosis;
}

inline std::array<float, kObservationDim> SanitizeObservationVector(
    const std::array<float, kObservationDim>& input,
    uint64_t& non_finite_count,
    uint64_t& clamped_count) {
    auto out = input;
    for (size_t i = 0; i < kObservationDim; ++i) {
        if (!std::isfinite(out[i])) {
            out[i] = 0.0f;
            ++non_finite_count;
        }
        const float clamped = std::clamp(out[i], kLowerBounds[i], kUpperBounds[i]);
        if (clamped != out[i]) {
            out[i] = clamped;
            ++clamped_count;
        }
    }
    return out;
}

struct TriggerDecisionMetrics {
    float mahalanobis_distance = 0.0f;
    float mahalanobis_epsilon = 0.0f;
    float energy_mahalanobis = 0.0f;
    float geometry_mahalanobis = 0.0f;
    float energy_contribution_share = 0.0f;
    bool energy_fast_track = false;
    bool significant_change = false;
};

// Faithful replica of ContextManager's rolling-history-based Mahalanobis
// significant-change gate. Owns its own per-dim rolling history and the
// baseline observation used for the L1-distance diagnostic -- byte-identical
// state semantics to ContextManager's own m_observationHistory/m_hmmObservation.
class ObservationTriggerGate {
public:
    static constexpr size_t kMinSamples = 40;
    static constexpr float kVarEps = 1e-6f;
    static constexpr float kEnergyFastTrackZ = 3.0f;
    static constexpr float kEnergyTriggerMult = 0.90f;
    static constexpr float kGeometryTriggerMult = 1.30f;
    static constexpr float kNoiseFloor = 0.005f;
    static constexpr float kNoiseFloorLowVelocityMult = 1.5f;
    static constexpr float kNoiseFloorHighVelocityMult = 0.85f;
    static constexpr float kVelocityLow = 3.0f;
    static constexpr float kVelocityHigh = 8.0f;

    // Pushes one already-scaled observation into the rolling history (call
    // once per accepted tick, matching ContextManager::CheckAndTriggerHMM's
    // Phase 2C).
    void PushObservation(const std::array<float, kObservationDim>& scaledObs) {
        for (size_t dim = 0; dim < kObservationDim; ++dim) {
            m_history[dim].push_back(scaledObs[dim]);
            if (m_history[dim].size() > kMinSamples) {
                m_history[dim].pop_front();
            }
        }
    }

    size_t SampleCount() const { return m_history[0].size(); }

    TriggerDecisionMetrics ComputeTriggerDecisionMetrics(
        const std::array<float, kObservationDim>& currentObs,
        float event_velocity) const {
        TriggerDecisionMetrics metrics;
        if (m_history[0].size() < kMinSamples) {
            return metrics;
        }

        float distance_sq = 0.0f;
        float energy_sq = 0.0f;
        float geometry_sq = 0.0f;
        float max_energy_abs_z = 0.0f;
        const int n = static_cast<int>(m_history[0].size());
        const int mid = n / 2;

        for (size_t dim = 0; dim < kObservationDim; ++dim) {
            std::array<float, kMinSamples + 1> scratch;
            for (int k = 0; k < n; ++k) {
                scratch[static_cast<size_t>(k)] = m_history[dim][static_cast<size_t>(k)];
            }
            std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
            const float median = scratch[static_cast<size_t>(mid)];

            for (int k = 0; k < n; ++k) {
                scratch[static_cast<size_t>(k)] = std::abs(
                    m_history[dim][static_cast<size_t>(k)] - median);
            }
            std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
            const float madScale = scratch[static_cast<size_t>(mid)] * 1.4826f;

            const float safe_variance = std::max(madScale * madScale, kVarEps);
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
            (distance_sq > kVarEps) ? (energy_sq / distance_sq) : 0.0f;
        metrics.energy_fast_track = (max_energy_abs_z >= kEnergyFastTrackZ);

        // NOTE: replicated exactly as production computes it -- the epsilon
        // call's 2nd/3rd args are currentObs[kObsLempelZiv]/[kObsTailIndex],
        // not path-efficiency-SNR/kurtosis despite the callee's parameter
        // names (a pre-existing production naming artifact, out of scope to
        // "fix" here; this file's job is bit-faithful replication).
        metrics.mahalanobis_epsilon = GetAdaptiveMahalanobisEpsilon(
            event_velocity,
            currentObs[MTS::Schema::Contract::kObsLempelZiv],
            currentObs[MTS::Schema::Contract::kObsTailIndex]);

        const bool energy_significant =
            metrics.energy_mahalanobis >= (metrics.mahalanobis_epsilon * kEnergyTriggerMult);
        const bool geometry_significant =
            metrics.geometry_mahalanobis >= (metrics.mahalanobis_epsilon * kGeometryTriggerMult);

        metrics.significant_change =
            metrics.energy_fast_track || energy_significant || geometry_significant;
        return metrics;
    }

    void SetBaseline(const std::array<float, kObservationDim>& obs) {
        m_baseline = obs;
        m_baselineInitialized = true;
    }

    bool HasBaseline() const { return m_baselineInitialized; }

    // Cheap reset: only clears bookkeeping (RingBuffer head/count, the
    // baseline flag) -- does NOT re-zero the float payloads, which are
    // already inert once size()==0/HasBaseline()==false (never read out of
    // bounds). Matches ContextManager's original m_observationHistory[i].
    // clear() loop's exact cost profile; a naive `*this = {}` reassignment
    // would instead value-reinitialize every RingBuffer's full backing
    // std::array (RingBuffer.h's own `std::array<T, Capacity> m_data{};`),
    // wasted work for data no read ever reaches post-reset.
    void Reset() {
        for (auto& buf : m_history) buf.clear();
        m_baselineInitialized = false;
    }

    float ComputeL1DistanceFromBaseline(const std::array<float, kObservationDim>& currentObs) const {
        if (!m_baselineInitialized) {
            return 0.0f;
        }
        float l1 = 0.0f;
        for (size_t i = 0; i < kObservationDim; ++i) {
            l1 += std::abs(currentObs[i] - m_baseline[i]);
        }
        return l1;
    }

    static float GetAdaptiveNoiseFloor(float event_velocity) {
        if (event_velocity < kVelocityLow) {
            return kNoiseFloor * kNoiseFloorLowVelocityMult;
        } else if (event_velocity > kVelocityHigh) {
            return kNoiseFloor * kNoiseFloorHighVelocityMult;
        }
        const float t = (event_velocity - kVelocityLow) / (kVelocityHigh - kVelocityLow);
        const float scale = kNoiseFloorLowVelocityMult -
            (t * (kNoiseFloorLowVelocityMult - kNoiseFloorHighVelocityMult));
        return kNoiseFloor * scale;
    }

    static float GetAdaptiveMahalanobisEpsilon(float event_velocity, float path_efficiency_snr,
                                                float realized_kurtosis) {
        constexpr float base_epsilon = 4.0f;
        float velocity_multiplier = 1.0f;
        if (event_velocity < kVelocityLow) {
            velocity_multiplier = 1.15f;
        } else if (event_velocity > kVelocityHigh) {
            velocity_multiplier = 0.85f;
        } else {
            const float t = (event_velocity - kVelocityLow) / (kVelocityHigh - kVelocityLow);
            velocity_multiplier = 1.15f - (0.30f * t);
        }

        const float entropy_clamped = std::clamp(path_efficiency_snr, 0.0f, 1.0f);
        const float entropy_multiplier = 1.0f + (0.35f * entropy_clamped);

        constexpr float KURT_FRAGILITY_RAMP_START = 1.3809f;
        constexpr float KURT_FRAGILITY_SLOPE = 0.6609f;
        const float kurtosis_excess = std::max(realized_kurtosis - KURT_FRAGILITY_RAMP_START, 0.0f);
        const float kurtosis_multiplier = 1.0f + std::min(kurtosis_excess * KURT_FRAGILITY_SLOPE, 0.25f);

        return base_epsilon * velocity_multiplier * entropy_multiplier * kurtosis_multiplier;
    }

private:
    std::array<RingBuffer<float, kMinSamples + 1>, kObservationDim> m_history;
    std::array<float, kObservationDim> m_baseline{};
    bool m_baselineInitialized = false;
};

}  // namespace otg
