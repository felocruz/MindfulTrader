// streaming_correlation_matrix.h -- generic, header-only, O(D^2)-memory
// online multivariate Pearson correlation accumulator. Built for Elite
// Feature Set Curation Phase 1 (docs/superpowers/specs/2026-08-31-elite-
// feature-set-curation-initiative.md Phase 1: whole-vector redundancy audit)
// specifically to avoid the OOM failure mode this tool family has already
// hit once for real: observation_vector_recalibration.cpp's own header
// comment records that 4 CONCURRENT dim-group passes over the same
// 471.9M-row mes_ticks.parquet exhausted RAM (no swap configured) and took
// down the whole session, 2026-09-03 -- the root cause there was concurrent
// process count, not a single pass materializing full-length arrays, but
// this initiative's own Phase 1 (whole-vector pairwise correlation across
// ~D dims) has a SEPARATE, additional single-pass risk if implemented
// naively: retaining a full-length std::vector<double> PER DIM (the
// approach volatility_dim_redundancy_eval.cpp already takes for 4 series)
// would cost D * 471.9M * 8 bytes -- at D=12-18 dims that is 45-68GB for the
// per-dim series alone, a real OOM regardless of concurrency. This header
// avoids that entirely: never retains a single observation, in any dim, at
// any point -- only a fixed D+D^2-sized set of running accumulators, exactly
// like this tool family's own existing CorrTracker (observation_vector_
// recalibration.cpp) already does for ONE pair; this generalizes that same,
// already-proven idea to a full D x D matrix in one accumulator object.
//
// Numerically stable (not the naive "sum of squares/products" formula
// CorrTracker itself uses, which suffers catastrophic cancellation at large
// N when subtracting two large nearly-equal sums -- a real risk at this
// project's real scale, N ~ 10^5-10^8 depending on the sampling cadence
// chosen): implements Welford's (1962) single-pass mean/variance update
// generalized to the multivariate case per West, D.H.D. (1979), "Updating
// Mean and Variance Estimates: An Improved Method," Communications of the
// ACM 22(9):532-535 -- the standard, textbook citation for exactly this
// online-covariance-matrix construction. Verified in
// test_streaming_correlation_matrix.cpp against a naive two-pass batch
// Pearson computation (must agree, small-N case) AND against a
// large-constant-offset stress case designed to break the naive
// sum-of-squares formula specifically (must still agree, proving the
// numerical-stability claim is real, not just cited).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <string>
#include <utility>
#include <vector>

#include "../ToolProgressLogger.h"

template <std::size_t D>
class StreamingCorrelationMatrix {
public:
    static_assert(D >= 2, "StreamingCorrelationMatrix requires at least 2 dims to correlate");

    void Reset() {
        n_ = 0;
        mean_.fill(0.0);
        for (auto& row : c_) row.fill(0.0);
    }

    // Call once per observation (a full D-wide snapshot, carry-forward
    // already applied by the caller for any dim that hasn't refreshed this
    // observation -- matches FeatureScaler's own "the vector always has a
    // value for every dim" model). O(D^2) time, O(1) additional memory --
    // no observation is ever stored.
    void Update(const std::array<double, D>& x) {
        ++n_;
        std::array<double, D> delta;
        for (std::size_t i = 0; i < D; ++i) {
            delta[i] = x[i] - mean_[i];
            mean_[i] += delta[i] / static_cast<double>(n_);
        }
        // West (1979)'s incremental co-moment update: C_n(i,j) = C_{n-1}(i,j)
        // + delta_i_old * (x_j - mean_j_new). Only the upper triangle
        // (including the diagonal, which is the per-dim variance
        // accumulator) is maintained -- correlation is symmetric.
        for (std::size_t i = 0; i < D; ++i) {
            for (std::size_t j = i; j < D; ++j) {
                c_[i][j] += delta[i] * (x[j] - mean_[j]);
            }
        }
    }

    std::size_t Count() const { return n_; }

    // Sample covariance (Bessel-corrected, n-1 denominator).
    double Covariance(std::size_t i, std::size_t j) const {
        if (n_ < 2) return 0.0;
        const std::size_t lo = i < j ? i : j;
        const std::size_t hi = i < j ? j : i;
        return c_[lo][hi] / static_cast<double>(n_ - 1);
    }

    double Variance(std::size_t i) const { return Covariance(i, i); }

    // Pearson correlation coefficient. Returns 0.0 (not NaN) for a
    // degenerate (zero-variance) dim -- matches this codebase's own
    // established "degenerate input -> neutral, not a propagated NaN"
    // convention (RobustMoments.h/DfaHurstExponent.h's own contract).
    double Correlation(std::size_t i, std::size_t j) const {
        if (i == j) return 1.0;
        const double vi = Variance(i);
        const double vj = Variance(j);
        if (!(vi > 0.0) || !(vj > 0.0)) return 0.0;
        return Covariance(i, j) / std::sqrt(vi * vj);
    }

    // Prints the full lower-triangular correlation matrix plus every pair
    // sorted by |correlation| descending -- the actual redundancy signal
    // Phase 1 exists to surface (high |r| = double-counted evidence under
    // this system's diagonal-covariance HMM, per the initiative doc's own
    // governing constraint, not a numerical-conditioning concern).
    //
    // Routes every line through ToolProgressLogger (2026-09-07 fix, real
    // incident: an earlier version of this method used bare std::printf --
    // this repo's own top-level directive requires every tools/ result to
    // go through the logger so it survives in tools/output/'s permanent
    // archive even if the terminal session closes before being read; a real
    // ~32-minute run's only output was lost to exactly this before the fix).
    void Print(const std::array<const char*, D>& names, ToolProgressLogger& logger) const {
        char line[512];
        std::snprintf(line, sizeof(line),
                      "=== Streaming correlation matrix (n=%zu observations, O(%zu) accumulator state) ===",
                      n_, D * D);
        std::puts(line); logger.Log(line);

        std::string header = std::string(28, ' ');
        for (std::size_t j = 0; j < D; ++j) {
            std::snprintf(line, sizeof(line), "%8s", names[j]);
            header += line;
        }
        std::puts(header.c_str()); logger.Log(header);

        for (std::size_t i = 0; i < D; ++i) {
            std::string row;
            char cell[32];
            std::snprintf(cell, sizeof(cell), "%-28s", names[i]);
            row += cell;
            for (std::size_t j = 0; j < D; ++j) {
                std::snprintf(cell, sizeof(cell), "%+8.4f", Correlation(i, j));
                row += cell;
            }
            std::puts(row.c_str()); logger.Log(row);
        }

        std::puts("=== Pairs sorted by |correlation| descending ==="); logger.Log("=== Pairs sorted by |correlation| descending ===");
        const std::size_t pairCount = D * (D - 1) / 2;
        std::vector<std::pair<double, std::pair<std::size_t, std::size_t>>> pairs;
        pairs.reserve(pairCount);
        for (std::size_t i = 0; i < D; ++i) {
            for (std::size_t j = i + 1; j < D; ++j) {
                pairs.push_back({std::fabs(Correlation(i, j)), {i, j}});
            }
        }
        std::sort(pairs.begin(), pairs.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (const auto& p : pairs) {
            const auto [i, j] = p.second;
            std::snprintf(line, sizeof(line), "  |r|=%.4f  corr(%s, %s) = %+.4f",
                          p.first, names[i], names[j], Correlation(i, j));
            std::puts(line); logger.Log(line);
        }
    }

private:
    std::size_t n_ = 0;
    std::array<double, D> mean_{};
    std::array<std::array<double, D>, D> c_{};
};
