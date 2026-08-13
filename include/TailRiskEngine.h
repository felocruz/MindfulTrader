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

            // Pre-allocated scratchpad for the stability-region Hill(k) scan (Task 4
            // fix, 2026-08-13): reserved once here, reused every GetHillAlpha() call,
            // never reallocates since its size is capped at windowSize - 1.
            m_hillAtK.reserve(windowSize);

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
            // Hill(k) across a k-range, find a stable window of k where consecutive
            // Hill(k) values stay within a fixed relative band of the window's anchor,
            // take its midpoint. Hill(k) is computed via a running prefix log-sum
            // (cumLogSum[k] = cumLogSum[k-1] + log(x_{k-1})) so this whole scan is
            // true O(maxK) (~maxK std::log() calls total) -- the naive per-k resum
            // from i=0 is O(maxK^2) and was measured at ~10,000 log() calls for
            // maxK=100 (fixed 2026-08-13 post-review). m_hillAtK is a pre-allocated
            // member (reserved once in the constructor, like m_sortBuffer) so this
            // scan does zero heap allocation on the hot path.
            m_hillAtK.clear();
            double runningLogSum = std::log(m_sortBuffer[0]) + std::log(m_sortBuffer[1]);
            for (size_t k = 2; k <= maxK; ++k) {
                if (k > 2) {
                    runningLogSum += std::log(m_sortBuffer[k - 1]);
                }
                double threshold = m_sortBuffer[k];
                if (threshold <= 1e-9) { m_hillAtK.push_back(4.0); continue; }
                const double logThreshold = std::log(threshold);  // hoisted: computed once per k, not per i
                double logSum = runningLogSum - static_cast<double>(k) * logThreshold;
                m_hillAtK.push_back(logSum > 1e-9 ? static_cast<double>(k) / logSum : 4.0);
            }

            constexpr double kStabilityBandRelative = 0.15;  // 15% band, engineering choice within
                                                                // the practitioner "eyeball stability"
                                                                // convention -- no literature-prescribed
                                                                // exact figure found (2026-08-13 grounding pass).
            // Hill-plot bias grows with k (more of the bulk distribution, further from
            // the true tail), so prefer the FIRST plateau reached scanning outward from
            // small k, not whichever plateau happens to be numerically widest anywhere
            // in the range -- a review on 2026-08-13 found the widest-anywhere rule
            // picks a k=56-100 bulk-distribution plateau over a k=13-30 near-tail one on
            // synthetic alpha=3.0 Pareto data, producing alpha=1.55 (48% error) instead
            // of alpha=2.54 (15% error). kMinPlateauWidth=5 is an engineering choice
            // (stable across 5-10 on the validation case) requiring a plateau to span at
            // least 5 k-values before it's trusted as real rather than sampling noise.
            constexpr size_t kMinPlateauWidth = 5;
            size_t bestStart = 0, bestLen = 1;      // fallback: widest plateau found, if none meets kMinPlateauWidth
            size_t firstStart = 0, firstLen = 0;    // first plateau meeting kMinPlateauWidth (preferred)
            size_t start = 0;
            for (size_t i = 1; i <= m_hillAtK.size(); ++i) {
                const bool brokeOrEnd = (i == m_hillAtK.size()) ||
                    (std::fabs(m_hillAtK[i] - m_hillAtK[start]) / std::max(m_hillAtK[start], 1e-6) > kStabilityBandRelative);
                if (brokeOrEnd) {
                    const size_t runLen = i - start;
                    if (runLen > bestLen) { bestLen = runLen; bestStart = start; }
                    if (firstLen == 0 && runLen >= kMinPlateauWidth) { firstStart = start; firstLen = runLen; }
                    start = i;
                }
            }
            const size_t chosenStart = (firstLen > 0) ? firstStart : bestStart;
            const size_t chosenLen = (firstLen > 0) ? firstLen : bestLen;
            const size_t midpointIdx = chosenStart + chosenLen / 2;
            const double rawAlpha = m_hillAtK[std::min(midpointIdx, m_hillAtK.size() - 1)];

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

        // Pre-allocated scratchpad for the stability-region Hill(k) scan (Task 4)
        std::vector<double> m_hillAtK;

        // EWMA state for the smoothed alpha output (Task 4)
        double m_smoothedAlpha = 4.0;
        bool m_hasSmoothedAlpha = false;
    };
}
