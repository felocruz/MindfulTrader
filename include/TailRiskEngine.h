#pragma once

#include <vector>
#include <cmath>
#include <algorithm>
#include <numeric>
#include <limits>

namespace MindfulTrader {

    /**
     * @brief Tail Risk Engine (Hill Estimator)
     *
     * Calculates the Pareto Tail Index (Alpha) of the distribution of returns
     * to detect "Lévy Flight" (Fat Tail) regimes vs. Gaussian (Thin Tail) regimes.
     *
     * Usage:
     * 1. Push every event's log-return using AddObservation().
     * 2. Call GetHillAlpha() to get the current regime metric.
     *
     * Interpretation:
     * - Alpha > 2.0: Gaussian/Safe (Thin Tails). Trends are likely stable.
     * - Alpha < 1.7: Lévy/Fragile (Fat Tails). Variance is infinite. Expect gaps.
     */
    class TailRiskEngine {
    public:
        /**
         * @param windowSize Number of most recent events to consider (e.g., 500).
         * @param tailPercent Percentage of observations to treat as "Tail" (e.g., 0.05 for top 5%).
         */
        TailRiskEngine(size_t windowSize = 500, double tailPercent = 0.05)
            : m_windowSize(windowSize)
            , m_tailCutoff(static_cast<size_t>(windowSize * tailPercent))
            , m_headIndex(0)
            , m_isFull(false)
        {
            // Reserve memory once to prevent reallocations
            m_buffer.resize(windowSize, 0.0);

            // Temporary buffer for sorting (avoid allocating in the hot path)
            m_sortBuffer.reserve(windowSize);

            // Safety check for tail size
            if (m_tailCutoff < 2) m_tailCutoff = 2;
        }

        /**
         * @brief Adds a new Log Return observation (Event-Driven update).
         * Complexity: O(1)
         * @param logReturn Natural log return: ln(price_t / price_{t-1})
         */
        void AddObservation(double logReturn) {
            // We only care about magnitude for tail risk (absolute deviation)
            // Filter extremely small noise (epsilon) to avoid log(0) issues later
            double absRet = std::abs(logReturn);
            if (absRet < 1e-9) absRet = 1e-9;

            m_buffer[m_headIndex] = absRet;

            m_headIndex++;
            if (m_headIndex >= m_windowSize) {
                m_headIndex = 0;
                m_isFull = true;
            }
        }

        size_t GetSampleCount() const {
            return m_isFull ? m_windowSize : m_headIndex;
        }

        void Reset() {
            std::fill(m_buffer.begin(), m_buffer.end(), 0.0);
            m_sortBuffer.clear();
            m_headIndex = 0;
            m_isFull = false;
        }

        /**
         * @brief Calculates the Hill Estimator (Alpha) for the current window.
         * Complexity: O(N * log K) using partial_sort.
         *
         * Formula: alpha = [ (1/k) * sum( ln(x_i / x_{k+1}) ) ] ^ -1
         * Where x_i are the sorted top k absolute returns.
         *
         * @return Hill Alpha (lower = fatter tails/higher risk).
         *         Returns 4.0 (very safe) if not enough data.
         */
        double GetHillAlpha() {
            if (!m_isFull && m_headIndex < m_tailCutoff + 1) {
                return 4.0; // Not enough data yet, assume Gaussian safety
            }

            // 1. Copy active window to sort buffer
            // Since it's a circular buffer, we copy everything.
            // If strictly partially filled, only copy what we have.
            m_sortBuffer.clear();
            if (m_isFull) {
                m_sortBuffer.assign(m_buffer.begin(), m_buffer.end()); // Copy all -- .assign() avoids
                                                                        // reallocation given the pre-reserved
                                                                        // capacity, unlike operator= (Task 4).
            } else {
                m_sortBuffer.insert(m_sortBuffer.end(), m_buffer.begin(), m_buffer.begin() + m_headIndex);
            }

            // 2. Sort the top K elements (Largest Absolute Returns)
            // We need the largest elements at the beginning.
            // std::partial_sort sorts the range [first, middle) such that it contains the smallest elements
            // if using default comparator. We used std::greater to get LARGEST elements.
            size_t validSize = m_sortBuffer.size();
            size_t k = std::min(m_tailCutoff, validSize - 1);

            if (k < 2) return 4.0;

            std::partial_sort(
                m_sortBuffer.begin(),
                m_sortBuffer.begin() + k + 1, // We need k+1 to find the threshold X_(k+1)
                m_sortBuffer.end(),
                std::greater<double>()
            );

            // 3. Apply Hill Formula
            // x_(k+1) is the threshold return
            double threshold = m_sortBuffer[k];

            // If threshold is basically zero, the tail is undefined (everything is flat).
            // Return high alpha (safe).
            if (threshold <= 1e-9) return 4.0;

            double logSum = 0.0;
            for (size_t i = 0; i < k; ++i) {
                // ln( x_(i) / x_(k+1) )
                // = ln(x_i) - ln(threshold)
                logSum += (std::log(m_sortBuffer[i]) - std::log(threshold));
            }

            if (logSum <= 1e-9) return 4.0; // Avoid divide by zero

            // Hill Alpha = (1/k * Sum)^-1  =>  k / Sum
            double alpha = static_cast<double>(k) / logSum;

            return alpha;
        }

    private:
        size_t m_windowSize;
        size_t m_tailCutoff; // K

        std::vector<double> m_buffer;
        size_t m_headIndex;
        bool m_isFull;

        // Pre-allocated scratchpad for calculation to avoid heap-alloc on hot path
        std::vector<double> m_sortBuffer;
    };
}
