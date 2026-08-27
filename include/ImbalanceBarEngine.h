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

        if (std::abs(m_cumulativeImbalance) >= m_imbalanceThreshold) {
            const float barReturn = m_barOpenPrice > 0.0f
                ? std::log(price / m_barOpenPrice)
                : 0.0f;
            if (m_completedBarReturns.size() == kImbalanceBarBufferCapacity) {
                m_completedBarReturns.pop_front();
            }
            m_completedBarReturns.push_back(barReturn);
            ++m_completedBarCount;
            m_cumulativeImbalance = 0.0f;
            m_haveBarOpenPrice = false;
        }
    }

    void SetImbalanceThresholdForTesting(float threshold) {
        m_imbalanceThreshold = threshold;
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
};
