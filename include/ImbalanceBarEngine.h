// Pure, ACSIL-independent imbalance-bar accumulator.
#pragma once

#include "RingBuffer.h"

#include <cmath>
#include <cstddef>

class ImbalanceBarEngine {
public:
    static constexpr std::size_t kImbalanceBarBufferCapacity = 500;

    void Reset() {
        m_completedBarCount = 0;
        m_lastBarIndex = -1;
        m_lastAskVolume = 0.0f;
        m_lastBidVolume = 0.0f;
        m_lastSignedDelta = 0.0f;
        m_cumulativeImbalance = 0.0f;
        m_barOpenPrice = 0.0f;
        m_haveBarOpenPrice = false;
        m_completedBarReturns.clear();
        m_completedBarImbalances.clear();
        m_ticksInCurrentBar = 0;
        m_ewmaExpectedTicksPerBar = 0.0f;
        m_ewmaExpectedImbalancePerTick = 0.0f;
        m_haveAdaptiveEstimate = false;
    }

    void OnTick(int barIndex, float askVolume, float bidVolume) {
        if (barIndex != m_lastBarIndex) {
            m_lastBarIndex = barIndex;
            m_lastAskVolume = 0.0f;
            m_lastBidVolume = 0.0f;
        }

        const float currentSigned = askVolume - bidVolume;
        const float previousSigned = m_lastAskVolume - m_lastBidVolume;
        m_lastSignedDelta = currentSigned - previousSigned;
        m_lastAskVolume = askVolume;
        m_lastBidVolume = bidVolume;
    }

    void OnTickWithPrice(int barIndex, float askVolume, float bidVolume, float price) {
        OnTick(barIndex, askVolume, bidVolume);
        if (!m_haveBarOpenPrice) {
            m_barOpenPrice = price;
            m_haveBarOpenPrice = true;
        }
        m_cumulativeImbalance += m_lastSignedDelta;
        ++m_ticksInCurrentBar;

        if (std::abs(m_cumulativeImbalance) >= GetCurrentThreshold()) {
            // Guard both operands: a non-positive price (bad tick data) would otherwise inject
            // NaN/-inf into log(), the same degenerate-input class already fixed elsewhere in
            // this codebase (ContextManager.cpp's activity-clock skewness/kurtosis NaN guard).
            const float barReturn = (m_barOpenPrice > 0.0f && price > 0.0f)
                ? std::log(price / m_barOpenPrice)
                : 0.0f;
            if (m_completedBarReturns.size() == kImbalanceBarBufferCapacity) {
                m_completedBarReturns.pop_front();
            }
            m_completedBarReturns.push_back(barReturn);
            // Signed cumulative imbalance at the moment this bar completed (theta_tau) --
            // captured before the reset below discards it. Pushed/evicted in lockstep with
            // m_completedBarReturns, so the two buffers always stay index-aligned.
            if (m_completedBarImbalances.size() == kImbalanceBarBufferCapacity) {
                m_completedBarImbalances.pop_front();
            }
            m_completedBarImbalances.push_back(m_cumulativeImbalance);
            ++m_completedBarCount;

            // AFML Ch.2 information-driven-bars EWMA update (López de Prado 2018; the brainstorm's
            // own b_target = E[T]*|E[b_k*v_k]| formula is a direct match to this, literature-
            // grounded 2026-09-06, docs/superpowers/specs/2026-09-06-labeling-data-augmentation-
            // gang-statistical-reformulation-initiative.md §2.1). Only advances when adaptive mode
            // is enabled -- opt-in, existing fixed-threshold callers are unaffected.
            if (m_adaptiveThresholdEnabled && m_ticksInCurrentBar > 0) {
                const float perTickImbalance = m_cumulativeImbalance / static_cast<float>(m_ticksInCurrentBar);
                if (!m_haveAdaptiveEstimate) {
                    // Seed with the first real completed bar's own values -- same "seed with first
                    // sample, don't fabricate a zero" convention already used throughout this
                    // codebase's other EWMA state (e.g. EventDataCollectorStudy.cpp's velocity EMA).
                    m_ewmaExpectedTicksPerBar = static_cast<float>(m_ticksInCurrentBar);
                    m_ewmaExpectedImbalancePerTick = perTickImbalance;
                    m_haveAdaptiveEstimate = true;
                } else {
                    m_ewmaExpectedTicksPerBar +=
                        m_adaptiveAlpha * (static_cast<float>(m_ticksInCurrentBar) - m_ewmaExpectedTicksPerBar);
                    m_ewmaExpectedImbalancePerTick +=
                        m_adaptiveAlpha * (perTickImbalance - m_ewmaExpectedImbalancePerTick);
                }
            }

            m_cumulativeImbalance = 0.0f;
            m_haveBarOpenPrice = false;
            m_ticksInCurrentBar = 0;
        }
    }

    // Real production API -- each future IS1/IS2/IS3 instance is configured with its own
    // bucket-size threshold at init time, not just from tests/offline tools.
    void SetImbalanceThreshold(float threshold) {
        m_imbalanceThreshold = threshold;
    }

    // AFML Ch.2 (López de Prado 2018) EWMA-adaptive threshold: b_target = E[T]*|E[b_k*v_k]|,
    // both EWMA-updated after every completed bar. Opt-in -- disabled by default, so existing
    // fixed-threshold callers (SetImbalanceThreshold alone) are unaffected. alpha=0.01 matches
    // the source brainstorm's own pseudocode (docs/superpowers/specs/2026-09-06-labeling-data-
    // augmentation-gang-statistical-reformulation-initiative.md §2.1), not independently derived.
    void EnableAdaptiveThreshold(float alpha = 0.01f) {
        m_adaptiveThresholdEnabled = true;
        m_adaptiveAlpha = alpha;
    }

    bool IsAdaptiveThresholdEnabled() const { return m_adaptiveThresholdEnabled; }

    // The threshold actually gating bar completion right now: the adaptive EWMA estimate once
    // enabled AND at least one real bar has completed (seeding it), otherwise the fixed
    // SetImbalanceThreshold value -- never a fabricated zero/undefined threshold.
    float GetCurrentThreshold() const {
        if (m_adaptiveThresholdEnabled && m_haveAdaptiveEstimate) {
            return m_ewmaExpectedTicksPerBar * std::abs(m_ewmaExpectedImbalancePerTick);
        }
        return m_imbalanceThreshold;
    }

    std::size_t GetCompletedBarCount() const { return m_completedBarCount; }
    float GetLastSignedDelta() const { return m_lastSignedDelta; }

    std::size_t GetImbalanceBarReturns(std::size_t n, float* out) const {
        const std::size_t available = m_completedBarReturns.size();
        const std::size_t count = n < available ? n : available;
        const std::size_t start = available - count;
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = m_completedBarReturns[start + i];
        }
        return count;
    }

    // Signed cumulative imbalance (theta_tau) at each completed bar's close, same rolling
    // window/eviction policy as GetImbalanceBarReturns -- index i here corresponds to the
    // same bar as index i from that call, as long as both are queried with the same n.
    std::size_t GetImbalanceBarMagnitudes(std::size_t n, float* out) const {
        const std::size_t available = m_completedBarImbalances.size();
        const std::size_t count = n < available ? n : available;
        const std::size_t start = available - count;
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = m_completedBarImbalances[start + i];
        }
        return count;
    }

private:
    std::size_t m_completedBarCount = 0;
    int m_lastBarIndex = -1;
    float m_lastAskVolume = 0.0f;
    float m_lastBidVolume = 0.0f;
    float m_lastSignedDelta = 0.0f;
    float m_cumulativeImbalance = 0.0f;
    float m_imbalanceThreshold = 50.0f;
    float m_barOpenPrice = 0.0f;
    bool m_haveBarOpenPrice = false;
    RingBuffer<float, kImbalanceBarBufferCapacity> m_completedBarReturns;
    RingBuffer<float, kImbalanceBarBufferCapacity> m_completedBarImbalances;

    // AFML Ch.2 EWMA-adaptive threshold state -- all zero/disabled until EnableAdaptiveThreshold()
    // is called, so a plain SetImbalanceThreshold()-only caller sees no behavior change at all.
    std::size_t m_ticksInCurrentBar = 0;
    bool m_adaptiveThresholdEnabled = false;
    float m_adaptiveAlpha = 0.01f;
    bool m_haveAdaptiveEstimate = false;
    float m_ewmaExpectedTicksPerBar = 0.0f;
    float m_ewmaExpectedImbalancePerTick = 0.0f;
};
