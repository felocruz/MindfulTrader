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
            m_smoothedAlpha = 4.0;       // Task 4: clear stale EWMA state on reset too
            m_hasSmoothedAlpha = false;
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

            size_t validSize = m_sortBuffer.size();
            // Widen the sort to the largest k we might scan (up to 4x the configured
            // tail cutoff, capped at validSize-1) so the stability-region scan below
            // has a real range to search, not just the single configured k.
            size_t maxK = std::min(m_tailCutoff * 4, validSize - 1);
            if (maxK < 2) return 4.0;

            std::partial_sort(
                m_sortBuffer.begin(),
                m_sortBuffer.begin() + static_cast<std::ptrdiff_t>(maxK) + 1,
                m_sortBuffer.end(),
                std::greater<double>()
            );

            // 2. Resnick & Stărică (1997) Hill-plot stability-region selector: compute
            // Hill(k) across a k-range, find the widest window of k where consecutive
            // Hill(k) values stay within a fixed relative band, take its midpoint --
            // cheap (O(maxK), reuses the sort already paid for above), no bootstrap.
            std::vector<double> hillAtK;
            hillAtK.reserve(maxK);
            for (size_t k = 2; k <= maxK; ++k) {
                double threshold = m_sortBuffer[k];
                if (threshold <= 1e-9) { hillAtK.push_back(4.0); continue; }
                double logSum = 0.0;
                for (size_t i = 0; i < k; ++i) {
                    logSum += (std::log(m_sortBuffer[i]) - std::log(threshold));
                }
                hillAtK.push_back(logSum > 1e-9 ? static_cast<double>(k) / logSum : 4.0);
            }

            constexpr double kStabilityBandRelative = 0.15;  // 15% band, engineering choice within
                                                                // the practitioner "eyeball stability"
                                                                // convention -- no literature-prescribed
                                                                // exact figure found (2026-08-13 grounding pass).
            size_t bestStart = 0, bestLen = 1;
            size_t start = 0;
            for (size_t i = 1; i < hillAtK.size(); ++i) {
                const double relDiff = std::fabs(hillAtK[i] - hillAtK[start]) / std::max(hillAtK[start], 1e-6);
                if (relDiff > kStabilityBandRelative) {
                    start = i;
                }
                if (i - start + 1 > bestLen) {
                    bestLen = i - start + 1;
                    bestStart = start;
                }
            }
            const size_t midpointIdx = bestStart + bestLen / 2;
            const double rawAlpha = hillAtK[std::min(midpointIdx, hillAtK.size() - 1)];

            // 3. EWMA-smooth the alpha series itself (not the k-selection) -- Resnick &
            // Stărică (1997) show Hill is consistent under GARCH-type dependence, so
            // smoothing the output addresses finite-sample variance without
            // disturbing the underlying estimator. alpha=0.2 -> ~9-sample half-life,
            // an engineering choice matching this codebase's other EMA smoothing
            // conventions (e.g. FeatureScaler's CARRY_DECAY_HALFLIFE=200 samples at
            // a much higher tick-rate cadence -- this dim updates far less often).
            constexpr double kAlphaSmoothing = 0.2;
            if (!m_hasSmoothedAlpha) {
                m_smoothedAlpha = rawAlpha;
                m_hasSmoothedAlpha = true;
            } else {
                m_smoothedAlpha = kAlphaSmoothing * rawAlpha + (1.0 - kAlphaSmoothing) * m_smoothedAlpha;
            }
            return m_smoothedAlpha;
        }

    private:
        size_t m_windowSize;
        size_t m_tailCutoff; // K

        std::vector<double> m_buffer;
        size_t m_headIndex;
        bool m_isFull;

        // Pre-allocated scratchpad for calculation to avoid heap-alloc on hot path
        std::vector<double> m_sortBuffer;

        // EWMA state for the smoothed alpha output (Task 4)
        double m_smoothedAlpha = 4.0;
        bool m_hasSmoothedAlpha = false;
    };
}
