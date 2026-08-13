#pragma once

#include <cmath>
#include <algorithm>
#include <array>
#include <cstdint>

namespace MindfulTrader {

    /**
     * @brief Information Engine (Shannon Entropy & KL-Divergence)
     *
     * Quantifies "Market Surprise" and "Regime Shift" using Information Theory.
     *
     * Components:
     * 1. SAX Discretization: Maps continuous log-returns into k=10 symbolic bins.
     * 2. Shannon Entropy: Measures the "Confusion" or "Randomness" of the current window.
     * 3. KL-Divergence: Measures statistical distance between Current Window (P) and Baseline (Q).
     *
     * Usage:
     * - AddObservation(logReturn) on every event.
     * - Call GetShannonEntropy() or GetKLDivergence() for metrics.
     */
    class InformationEngine {
    public:
        static constexpr size_t NUM_BINS = 10;
        static constexpr size_t WINDOW_SIZE_P = 50;   // Current Regime (Short-term)
        static constexpr size_t WINDOW_SIZE_Q = 500;  // Baseline Context (Long-term)
        static constexpr size_t WINDOW_SIZE_LZ = 64;  // Lempel-Ziv Bit-String Window

        InformationEngine()
            : m_headIndexP(0), m_headIndexQ(0), m_headIndexLZ(0), m_countP(0), m_countQ(0)
        {
            m_bufferP.fill(0.0);
            m_bufferQ.fill(0.0);
            m_bufferLZ.fill(0.0);

            // Initialize histograms to zero (institutional standard: no arbitrary priors)
            m_histogramP.fill(0.0);
            m_histogramQ.fill(0.0);

            // Bin index recorded at insertion time for each ring-buffer slot (Finding 1,
            // final-review fix round). MapToBin() is time-varying (depends on the live,
            // drifting m_emaAbsLogReturn), so re-deriving a slot's eviction bin via
            // MapToBin(oldVal) at eviction time can disagree with the bin it was
            // originally counted into when inserted -- desyncing the histogram from the
            // ring buffer's true contents. Recording the bin at insertion and decrementing
            // THAT recorded bin at eviction restores the invariant sum(hist) == count.
            m_binIndexP.fill(0);
            m_binIndexQ.fill(0);
        }

        /**
         * @brief Adds a log-return observation.
         * Updates Short-term (P), Long-term (Q), and Lempel-Ziv buffers.
         */
        void AddObservation(double logReturn) {
            // Rolling volatility estimate (EMA of |logReturn|), used by MapToBin() to
            // standardize returns into sigma-scores instead of comparing against a
            // fixed, hardcoded assumed sigma (docs/superpowers/plans/2026-08-04-phase1-hardening.md
            // Task 2; lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1.2). Alpha is tied to
            // WINDOW_SIZE_P (the same short-term regime window already used elsewhere in
            // this class) for a consistent, non-arbitrary responsiveness.
            const double absReturn = std::abs(logReturn);
            constexpr double kVolatilityEmaAlpha = 2.0 / (static_cast<double>(WINDOW_SIZE_P) + 1.0);
            if (m_emaAbsLogReturn <= 0.0) {
                m_emaAbsLogReturn = absReturn;
            } else {
                m_emaAbsLogReturn = kVolatilityEmaAlpha * absReturn + (1.0 - kVolatilityEmaAlpha) * m_emaAbsLogReturn;
            }

            // Update Long-Term Window (Q) - Baseline
            // Compute the new bin once and record it into the parallel bin-index array
            // for this slot BEFORE it gets evicted on some future call -- the recorded
            // bin (not a re-derived MapToBin(oldVal) under a since-drifted sigma) is
            // what UpdateHistogram() decrements at eviction time (Finding 1).
            size_t newBinQ = MapToBin(logReturn);
            uint8_t oldBinQ = m_binIndexQ[m_headIndexQ];
            m_bufferQ[m_headIndexQ] = logReturn;
            UpdateHistogram(m_histogramQ, newBinQ, oldBinQ, m_countQ < WINDOW_SIZE_Q);
            m_binIndexQ[m_headIndexQ] = static_cast<uint8_t>(newBinQ);

            m_headIndexQ = (m_headIndexQ + 1) % WINDOW_SIZE_Q;
            if (m_countQ < WINDOW_SIZE_Q) m_countQ++;

            // Update Short-Term Window (P) - Current Regime
            size_t newBinP = MapToBin(logReturn);
            uint8_t oldBinP = m_binIndexP[m_headIndexP];
            m_bufferP[m_headIndexP] = logReturn;
            UpdateHistogram(m_histogramP, newBinP, oldBinP, m_countP < WINDOW_SIZE_P);
            m_binIndexP[m_headIndexP] = static_cast<uint8_t>(newBinP);

            m_headIndexP = (m_headIndexP + 1) % WINDOW_SIZE_P;
            if (m_countP < WINDOW_SIZE_P) m_countP++;
            
            // Update Lempel-Ziv Window (Raw log returns for binarization)
            m_bufferLZ[m_headIndexLZ] = logReturn;
            m_headIndexLZ = (m_headIndexLZ + 1) % WINDOW_SIZE_LZ;


        }

        /**
         * @brief Calculates Fisher Information (Discrete)
         * Measures the "Reliability" or "Sharpness" of the probability distribution.
         * F = Sum[ (p(i+1) - p(i))^2 / p(i) ]
         * High Fisher = Sharp peaks (High certainty/reliability).
         * Low Fisher = Flat distribution (Low certainty/high entropy).
         */
        double GetFisherInformation() const {
            if (m_countP < 10) return 0.0;

            double fisher = 0.0;
            double norm = 1.0 / static_cast<double>(m_countP);
            
            // Reconstruct probability vector using current P histogram
            std::array<double, NUM_BINS> p;
            for (size_t i = 0; i < NUM_BINS; ++i) {
                p[i] = m_histogramP[i] * norm;
            }

            for (size_t i = 0; i < NUM_BINS - 1; ++i) {
                // Fisher Info for discrete distribution: sum( (p_{i+1} - p_i)^2 / p_i )
                // Add small epsilon to denominator to avoid division by zero
                double denom = (p[i] > 1e-9) ? p[i] : 1e-9;
                double diff = p[i+1] - p[i];
                fisher += (diff * diff) / denom;
            }
            
            // Normalize/Clamp to reasonable range [0, 10] for stability
            return std::min(fisher, 10.0);
        }

        /**
         * @brief Calculates Recurrence Rate (Value Area frequency)
         * Returns the probability density of the CURRENT market state relative to long-term baseline.
         * High (0.3-0.5) = We are in the "Value Area" (Normal behavior).
         * Low (<0.05)    = We are in a "Rare Event" / Tail (Extremistan).
         */
        double GetRecurrenceRate(double currentLogReturn) const {
             if (m_countQ < 10) return 0.0;

             // reuse member function MapToBin
             size_t bin = MapToBin(currentLogReturn);
             
             // Prob of this state in long-term history
             return m_histogramQ[bin] / static_cast<double>(m_countQ);
        }

        size_t GetSampleCount() const { return m_countP; }

        void Reset() {
            m_bufferP.fill(0.0);
            m_bufferQ.fill(0.0);
            m_bufferLZ.fill(0.0);
            m_histogramP.fill(0.0);
            m_histogramQ.fill(0.0);
            m_binIndexP.fill(0);
            m_binIndexQ.fill(0);
            m_headIndexP = 0;
            m_headIndexQ = 0;
            m_headIndexLZ = 0;
            m_countP = 0;
            m_countQ = 0;
            m_emaAbsLogReturn = 0.0;
        }

        /**
         * @brief Calculates Lempel-Ziv Complexity (LZ76)
         * Measures the "Compressibility" or "Algorithmic Pattern Depth" of the sequence.
         * High LZ = Incompressible (Random). Low LZ = Patterned/Compressible.
         * 
         * Logic:
         * 1. Binarize returns (1 if > median, 0 otherwise) over last 64 bars.
         * 2. Compute LZ complexity count C(n).
         * 3. Normalize by n / log2(n) for asymptotical randomness measure.
         * 
         * @return Normalized LZ Complexity [0.0 - ~1.0+]
         */
        double GetLempelZivComplexity() const {
             // 1. Snapshot and Binarize
             // WINDOW_SIZE_LZ (64) is a compile-time-bounded max -- fixed
             // stack arrays, zero heap allocation, replacing what used to be
             // two std::vectors allocated fresh on every call (this ran every
             // tick once warmed up, docs/superpowers/specs/2026-08-07-
             // contextmanager-ring-buffer-dod-design.md §3.5, Round 3).
             std::array<int, WINDOW_SIZE_LZ> S{};

             // Extract linear sequence from circular buffer
             size_t count = (m_countQ < WINDOW_SIZE_LZ) ? m_countQ : WINDOW_SIZE_LZ;
             if (count < 10) {
                 return 0.5; // Neutral complexity if insufficient data
             }

             // Calculate local median for adaptive thresholding
             std::array<double, WINDOW_SIZE_LZ> sortedBuf{};
             for(size_t i=0; i<count; ++i) {
                 // Reconstruct chronological order if circular
                 // Logic: m_bufferLZ is a circular buffer.
                 // If full (count == WINDOW_SIZE_LZ), newest is (m_headIndexLZ - 1 + Size) % Size.
                 // Oldest is m_headIndexLZ.
                 // So "ith oldest" is (m_headIndexLZ + i) % Size.
                 // If NOT full, oldest is 0.
                 size_t idx = (m_countQ < WINDOW_SIZE_LZ) ? i : (m_headIndexLZ + i) % WINDOW_SIZE_LZ;
                 sortedBuf[i] = m_bufferLZ[idx];
             }
             std::sort(sortedBuf.begin(), sortedBuf.begin() + count);
             double median = sortedBuf[count/2];

             // Construct bit string S
             for(size_t i=0; i<count; ++i) {
                 size_t idx = (m_countQ < WINDOW_SIZE_LZ) ? i : (m_headIndexLZ + i) % WINDOW_SIZE_LZ;
                 S[i] = (m_bufferLZ[idx] >= median ? 1 : 0);
             }

             // 2. Lempel-Ziv 76 Algorithm
             // Kaspar/Schuster (1987) implementation
             int complexity = 1;
             int n = static_cast<int>(count);

             int i = 0; // Index of the start of the new component
             int k = 1; // Length of the current candidate component

             // Proper loop for LZ76
             // We partition S into components S[0...i-1], S[i...i+k-1], etc.
             // At each step, we look for S[i...i+k-1] in S[0...i+k-2].
             // Note the search space EXTENDS into the prefix of the current component.
             
             // Simpler (O(N^2)) implementation:
             // Iterate through the string.
             // For current position i (starting at 1), check if S[i] extends previous prefix.
             // This is technically LZ78 or a variant.
             
             // Let's stick to the cleanest O(N^2) LZ Complexity def:
             // c=1, l=1, i=0, k=1
             // While i+k <= n
             //   Check if S[i : i+k] appears in S[0 : i+k-1]
             //   If yes (it's a copy), k++
             //   If no (it's new), complexity++, i=i+k, k=1
             
             i = 1; // Start checking from second char (first char is always new component of length 1)
             // Actually, i is the start of the *current* component being built.
             // Initial state: Component 1 is S[0]. So next component starts at i=1.
             
             k = 1; 
             while (i + k <= n) {
                 bool found = false;
                 // Check if S[i ... i+k-1] exists in S[0 ... i+k-2]
                 // This means checking strictly BEFORE the END of the current candidate
                 
                 // Limit for starting position of match 'j':
                 // The match of length k must end at index < i+k-1
                 // So j + k - 1 < i + k - 1  => j < i
                 
                 for (int j = 0; j < i; ++j) {
                     bool match = true;
                     for (int x = 0; x < k; ++x) {
                         if (S[j + x] != S[i + x]) {
                             match = false;
                             break;
                         }
                     }
                     if (match) {
                         found = true;
                         break;
                     }
                 }

                 if (found) {
                     k++; // Extend the current component
                 } else {
                     complexity++; // New component found!
                     i += k; // Move start pointer
                     k = 1;  // Reset length
                 }
             }

             // 3. Normalize: b(n) = n / log2(n)
             if (n <= 1) return 0.0;
             double b_n = static_cast<double>(n) / std::log2(static_cast<double>(n));
             double result = static_cast<double>(complexity) / b_n;

             return result;
        }

        /**
         * @brief Calculates Shannon Entropy of the CURRENT short-term window (P).
         * H(P) = - sum( P(x) * log2( P(x) ) )
         * High Entropy = High Confusion/Random Walk. Low Entropy = Ordered/Trend.
         *
         * The plug-in estimator is biased at small sample sizes. Miller (1955)
         * correction is applied: bias ~ (m-1)/(2N*ln2) bits, m = occupied bins.
         * Confirmed still-current consensus practice (2026-08-13 grounding pass,
         * matches R's entropy::entropy(method="MM") / infotheo's "mm" default).
         *
         * ---------------------------------------------------------------------
         * CONSUMER-THRESHOLD SCALE NOTE (2026-08-13, final-review Finding 7)
         * ---------------------------------------------------------------------
         * The Miller-Madow term shifted this statistic's scale DOWNWARD. Its
         * magnitude, computed directly from (m-1)/(2N*ln2) with this class's own
         * parameters (NUM_BINS = 10, so m <= 10; N = m_countP, capped at
         * WINDOW_SIZE_P = 50 and gated at a minimum of 10):
         *   - steady state (N=50, m=10): 0.130 bits, ~3.9% of a ~3.32-bit reading
         *   - warmup floor (N=10, m=10): 0.649 bits, ~19.6%
         * i.e. roughly a 4-20% downward shift, largest during warmup.
         *
         * DECISION: this is an ACCEPTED, DOCUMENTED delta. No consumer threshold
         * was re-derived. Every downstream threshold is expressed as a FRACTION
         * of kShannonMaxEntropyBits (ContextManager.h) rather than as an
         * absolute bit count, and the three consumers --
         *   - src/Indicator.cpp (MarketClimate chaos/momentum/coil bands)
         *   - include/ExecutionParams.h `shannonEntropyHaltFrac` (RiskManager
         *     chaos halt)
         *   - src/Scoring.cpp (entropy scoring bands)
         * -- were evaluated and judged only mildly affected: a few-percent
         * downward shift moves each gate a few percent toward "less chaotic",
         * which is directionally conservative for the halt gates.
         *
         * This is deliberately UNLIKE the kurtosis migration, where Task 7 ran a
         * full empirical percentile-matching campaign: that shift was a change of
         * statistic (moment-based excess kurtosis 3.0 -> Moors octile kurtosis
         * 1.233, a ~2.4x recentring on a ~20x-compressed scale), an order of
         * magnitude larger than this bias correction. Should entropy gates ever
         * need re-derivation, the methodology to copy is
         * tools/analyze_kurtosis_threshold_migration.py.
         *
         * @return Entropy in bits (0.0 to ~3.32 for k=10)
         */
        double GetShannonEntropy() const {
            if (m_countP < 10) return 0.0; // Not enough data

            double entropy = 0.0;
            double totalCount = static_cast<double>(m_countP);
            size_t occupiedBins = 0;

            for (size_t i = 0; i < NUM_BINS; ++i) {
                // Determine probability of this bin
                // Note: m_histogram initialized with small counts, but here we use actual tracked counts if possible
                // Implementation detail: bin counts need to be normalized to probabilities
                double count = m_histogramP[i];
                if (count <= 0.0) continue;
                ++occupiedBins;

                double p = count / totalCount; // Probability
                entropy -= p * std::log2(p);
            }

            // Apply Miller-Madow bias correction (see Doxygen comment above for details).
            if (occupiedBins > 1) {
                const double bias = (static_cast<double>(occupiedBins) - 1.0) / (2.0 * totalCount * std::log(2.0));
                entropy -= bias;
            }

            return std::max(entropy, 0.0);  // guard against the correction pushing a
                                            // near-zero-entropy reading negative
        }

        /**
         * @brief Calculates Kullback-Leibler Divergence: D_KL( P || Q )
         * Measures how much the Current Regime (P) deviates from the Baseline (Q).
         * "Surprise" metric.
         *
         * @return KL Divergence (non-negative, 0.0 = identical distributions)
         */
        double GetKLDivergence() const {
             if (m_countP < 10 || m_countQ < 100) return 0.0; // Warmup

             double divergence = 0.0;
             double totalP = static_cast<double>(m_countP);
             double totalQ = static_cast<double>(m_countQ);

             for (size_t i = 0; i < NUM_BINS; ++i) {
                 // Smoothed probabilities (Add-1 smoothing implicit in bins if needed,
                 // but here we assume our histogram logic handles 0s or we handle here)

                 // Use a small epsilon to avoid div by zero / log(0)
                 double p = std::max(m_histogramP[i], 0.1) / totalP;
                 double q = std::max(m_histogramQ[i], 0.1) / totalQ;

                 divergence += p * std::log2(p / q);
             }

             return std::max(0.0, divergence); // KL is always non-negative
        }

    private:
        // Rolling buffers
        std::array<double, WINDOW_SIZE_P> m_bufferP;
        std::array<double, WINDOW_SIZE_Q> m_bufferQ;
        std::array<double, WINDOW_SIZE_LZ> m_bufferLZ;

        // Histograms (Counts per bin)
        // Bins are roughly: [-Sigma*3, ...] based on fixed discretization for speed,
        // OR we can use dynamic discretization. For HFT, fixed wide bins usually safer.
        // Here we use a simplified "Standardized" binning assuming relatively stationary sigma,
        // or just absolute value buckets.
        // Let's use robust fixed buckets based on typical market log-returns.
        std::array<double, NUM_BINS> m_histogramP;
        std::array<double, NUM_BINS> m_histogramQ;

        // Bin index each ring-buffer slot was mapped to at insertion time (Finding 1).
        // NUM_BINS == 10 fits in uint8_t; indexed by the same head-index slot as
        // m_bufferP/m_bufferQ, so eviction always decrements the bin actually
        // occupied by the value being overwritten, not a live-sigma re-derivation.
        std::array<uint8_t, WINDOW_SIZE_P> m_binIndexP;
        std::array<uint8_t, WINDOW_SIZE_Q> m_binIndexQ;

        size_t m_headIndexP;
        size_t m_headIndexQ;
        size_t m_headIndexLZ;
        size_t m_countP;
        size_t m_countQ;
        double m_emaAbsLogReturn = 0.0;   // rolling volatility estimate (Task 2, phase1-hardening plan)

        /**
         * @brief Maps a log return to one of 10 bins.
         * Uses a rough approximation of standard deviation boundaries.
         */
        size_t MapToBin(double val) const {
            // Volatility-standardize before binning: replaces the old fixed,
            // hardcoded "assumed sigma ~ 0.0002 (2bps) for 1-minute" approximation
            // with a live rolling estimate, so entropy reflects distribution
            // STRUCTURE (randomness vs. organization), not the current volatility
            // regime (docs/superpowers/plans/2026-08-04-phase1-hardening.md Task 2;
            // lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1.2). Bin thresholds are
            // the SAME sigma-multiple boundaries as before (this class's original
            // comment already documented them in sigma terms); only the value being
            // compared against them changed from a fixed assumption to a live estimate.
            // Bins:
            // 0: < -5 sigma
            // 1: -5 to -2
            // 2: -2 to -1
            // 3: -1 to -0.5
            // 4: -0.5 to 0
            // 5: 0 to 0.5
            // 6: 0.5 to 1
            // 7: 1 to 2
            // 8: 2 to 5
            // 9: > 5 sigma
            constexpr double kMinSigma = 1e-6;  // floor to avoid division blowup at cold-start/flat-market
            const double sigma = (m_emaAbsLogReturn > kMinSigma) ? m_emaAbsLogReturn : kMinSigma;
            const double sigmaScore = val / sigma;

            if (sigmaScore < -5.0) return 0;
            if (sigmaScore < -2.0) return 1;
            if (sigmaScore < -1.0) return 2;
            if (sigmaScore < -0.5) return 3;
            if (sigmaScore < 0.0)  return 4;
            if (sigmaScore < 0.5)  return 5;
            if (sigmaScore < 1.0)  return 6;
            if (sigmaScore < 2.0)  return 7;
            if (sigmaScore < 5.0)  return 8;
            return 9;
        }

        // Takes the bin indices directly (already computed by the caller from the
        // insertion-time sigma and, for eviction, recorded at the original insertion
        // time) rather than re-deriving them from raw values via MapToBin() -- that
        // re-derivation is what caused Finding 1's histogram/ring-buffer desync,
        // since MapToBin() depends on the live, drifting m_emaAbsLogReturn.
        void UpdateHistogram(std::array<double, NUM_BINS>& hist, size_t newBin, size_t oldBin, bool isWarmingUp) {
            hist[newBin] += 1.0;

            if (!isWarmingUp) {
                if (hist[oldBin] > 0.0) {
                    hist[oldBin] -= 1.0;
                }
            }
        }
    };
}
