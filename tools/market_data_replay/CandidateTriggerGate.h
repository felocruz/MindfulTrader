// CandidateTriggerGate.h — tool-local copy of ObservationTriggerGate's
// median/MAD Mahalanobis significant-change logic, sized to the EVOLVING
// candidate dim count (CandidateObservationDims.h), not the fixed 18D
// ObservationData space. Deliberately NOT shared with
// include/ObservationTriggerGate.h -- the dim-selection spec (§2) requires
// that live ContextManager.cpp's gate stay untouched until dim selection
// concludes; this is a genuinely separate copy, not a parameterization of
// the shared header.
//
// docs/superpowers/specs/2026-09-09-market-data-replay-dim-selection-spec.md §3.

#pragma once

#include "CandidateObservationDims.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace mdr {

struct CandidateTriggerMetrics {
    float mahalanobis_distance = 0.0f;
    bool significant_change = false;
};

// Operates on a caller-provided candidate-only vector (kCandidateDimCount
// floats, already extracted/scaled by the caller) -- this class has no
// knowledge of the full 18D ObservationData layout at all.
class CandidateTriggerGate {
public:
    static constexpr std::size_t kMinSamples = 40;
    static constexpr float kVarEps = 1e-6f;
    static constexpr float kTriggerMult = 1.0f;
    static constexpr float kBaseEpsilon = 4.0f;

    void PushObservation(const std::array<float, kCandidateDimCount>& obs) {
        for (std::size_t dim = 0; dim < kCandidateDimCount; ++dim) {
            m_history[dim].push_back(obs[dim]);
            if (m_history[dim].size() > kMinSamples) {
                m_history[dim].pop_front();
            }
        }
    }

    std::size_t SampleCount() const { return m_history[0].size(); }
    bool HasBaseline() const { return m_hasBaseline; }
    void SetBaseline(const std::array<float, kCandidateDimCount>& obs) {
        m_baseline = obs;
        m_hasBaseline = true;
    }
    void Reset() {
        for (auto& h : m_history) h.clear();
        m_hasBaseline = false;
    }

    CandidateTriggerMetrics ComputeTriggerDecisionMetrics(
        const std::array<float, kCandidateDimCount>& currentObs) const {
        CandidateTriggerMetrics metrics;
        if (m_history[0].size() < kMinSamples) return metrics;

        float distance_sq = 0.0f;
        const int n = static_cast<int>(m_history[0].size());
        const int mid = n / 2;

        for (std::size_t dim = 0; dim < kCandidateDimCount; ++dim) {
            std::array<float, kMinSamples + 1> scratch{};
            for (int k = 0; k < n; ++k) {
                scratch[static_cast<std::size_t>(k)] = m_history[dim][static_cast<std::size_t>(k)];
            }
            std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
            const float median = scratch[static_cast<std::size_t>(mid)];

            for (int k = 0; k < n; ++k) {
                scratch[static_cast<std::size_t>(k)] = std::abs(
                    m_history[dim][static_cast<std::size_t>(k)] - median);
            }
            std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
            const float madScale = scratch[static_cast<std::size_t>(mid)] * 1.4826f;

            const float safe_variance = std::max(madScale * madScale, kVarEps);
            const float z = (currentObs[dim] - median) / std::sqrt(safe_variance);
            distance_sq += z * z;
        }

        metrics.mahalanobis_distance = std::sqrt(std::max(distance_sq, 0.0f));
        metrics.significant_change = metrics.mahalanobis_distance >= (kBaseEpsilon * kTriggerMult);
        return metrics;
    }

private:
    // Minimal fixed-capacity deque substitute -- avoids pulling in
    // include/RingBuffer.h (a live-execution-path header) for this tool-local
    // copy; capacity is kMinSamples, push_back+pop_front only.
    struct RingBufferLike {
        std::array<float, kMinSamples> buf{};
        std::size_t count = 0;
        std::size_t head = 0;

        std::size_t size() const { return count; }
        void push_back(float v) {
            buf[(head + count) % kMinSamples] = v;
            if (count < kMinSamples) ++count;
            else head = (head + 1) % kMinSamples;
        }
        void pop_front() {
            if (count == 0) return;
            head = (head + 1) % kMinSamples;
            --count;
        }
        void clear() { count = 0; head = 0; }
        float operator[](std::size_t i) const { return buf[(head + i) % kMinSamples]; }
    };

    std::array<RingBufferLike, kCandidateDimCount> m_history{};
    std::array<float, kCandidateDimCount> m_baseline{};
    bool m_hasBaseline = false;
};

}  // namespace mdr
