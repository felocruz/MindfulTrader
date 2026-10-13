#pragma once

#include <vector>
#include <cmath>
#include <algorithm>

#include "HillTailIndex.h"

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
         * @brief Adds a new Log Return observation (Event-Driven update) and
         * advances the EWMA-smoothed alpha by exactly one step.
         * Complexity: O(1) buffer write + O(N log K) estimation (the work that
         * used to sit in GetHillAlpha(); see the note below).
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

            // The EWMA advances HERE -- once per genuine new observation -- not
            // in GetHillAlpha(). Before 2026-08-13 the smoothing step lived in
            // the getter, so the effective smoothing constant depended on how
            // often callers happened to read: N reads between two real
            // observations drove m_smoothedAlpha geometrically toward rawAlpha,
            // weakening the smoothing exactly when read/publish rates spike
            // (i.e. in high volatility, the regime where stable smoothing
            // matters most). With the update anchored to AddObservation() the
            // alpha series is a deterministic function of the observation
            // sequence alone, independent of read cadence, and GetHillAlpha()
            // is a pure O(1) read. Net hot-path cost is unchanged: the same
            // O(N log K) scan runs once per observation instead of once per
            // read (final-review Finding 6).
            double rawAlpha = 0.0;
            if (!ComputeRawAlpha(rawAlpha)) return;

            // EWMA-smooth the alpha series itself (not the k-selection) -- Resnick &
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
         * @brief Current EWMA-smoothed Hill Estimator (Alpha).
         * Complexity: O(1) -- a pure read. All estimation work happens in
         * AddObservation(), so calling this repeatedly between observations is
         * free AND cannot perturb the smoothed state (final-review Finding 6).
         *
         * Formula (in AddObservation -> ComputeRawAlpha):
         *   alpha = [ (1/k) * sum( ln(x_i / x_{k+1}) ) ] ^ -1
         * Where x_i are the sorted top k absolute returns.
         *
         * @return Hill Alpha (lower = fatter tails/higher risk).
         *         Returns 4.0 (very safe) if not enough data.
         */
        double GetHillAlpha() const {
            return m_smoothedAlpha;
        }

        /**
         * @brief Snapshots the current window (absolute log-returns, ANY order -- see
         * HillTailIndex.h's own doc comment for why order doesn't matter here) and the engine's
         * own `m_tailCutoff`, so callers (tests) can independently recompute the raw (pre-EWMA) Hill
         * alpha via the same `HillTailIndex()` this class delegates to internally, without needing
         * their own parallel EWMA state.
         * @return Number of valid elements written to `out` (may be < windowSize pre-warmup).
         */
        size_t GetWindowSnapshot(std::vector<double>& out) const {
            const size_t count = m_isFull ? m_windowSize : m_headIndex;
            out.assign(m_buffer.begin(), m_buffer.begin() + static_cast<std::ptrdiff_t>(count));
            return count;
        }

        size_t GetTailCutoff() const {
            return m_tailCutoff;
        }

    private:
        /**
         * @brief Computes the un-smoothed (raw) Hill alpha over the current
         * window via the Resnick & Stărică (1997) stability-region k-selector.
         * Complexity: O(N * log K) using partial_sort + an O(maxK) scan.
         * Math extracted to HillTailIndex.h (2026-10-12) so it can be natively unit-tested and
         * ported to Rust independent of this class's own stateful ring-buffer/EWMA bookkeeping --
         * same extraction precedent as LempelZivComplexity.h. This method is now a thin
         * snapshot-then-delegate wrapper; the math itself is unchanged.
         * @param outAlpha receives the raw alpha on success.
         * @return false when the window cannot support an estimate yet (caller
         *         must leave the smoothed state untouched).
         */
        bool ComputeRawAlpha(double& outAlpha) {
            m_sortBuffer.clear();
            if (m_isFull) {
                m_sortBuffer.assign(m_buffer.begin(), m_buffer.end()); // Copy all -- .assign() avoids
                                                                        // reallocation given the pre-reserved
                                                                        // capacity, unlike operator= (Task 4).
            } else {
                m_sortBuffer.insert(m_sortBuffer.end(), m_buffer.begin(), m_buffer.begin() + m_headIndex);
            }

            const auto result = HillTailIndex(m_sortBuffer.data(), m_sortBuffer.size(), m_tailCutoff);
            if (!result.has_value()) {
                return false;
            }
            outAlpha = *result;
            return true;
        }

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
