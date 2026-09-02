// tools/context_pipeline/context_validate_stats.h
// Pure numeric per-dim statistics + row-level finite filtering + inter-feature
// correlation, shared by tools/context_pipeline/context_validate.cpp. Split from
// context_validate.cpp itself so this pure-math layer is natively testable
// without pulling in CLI/context_reader.h concerns -- mirrors this codebase's
// established pure-engine/thin-glue split.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

struct DimStats {
    std::size_t nan_inf_count = 0;
    std::size_t finite_row_count = 0;
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double std_dev = 0.0;
    double zero_ratio = 0.0;
    double one_ratio = 0.0;
    double constant_ratio = 0.0;
};

// A row is finite only if EVERY column is finite at that row -- matches
// context_preflight.py's np.isfinite(ctx.observations).all(axis=1) exactly,
// not an independent per-column filter (needed so mean/std/correlation are all
// computed over the identical row set across every dimension).
inline std::vector<bool> ComputeRowFiniteMask(
    const std::vector<std::vector<float>>& columns, std::size_t n_rows) {
    std::vector<bool> mask(n_rows, true);
    for (const auto& col : columns) {
        for (std::size_t r = 0; r < n_rows; ++r) {
            if (!std::isfinite(col[r])) {
                mask[r] = false;
            }
        }
    }
    return mask;
}

inline std::vector<DimStats> ComputeDimStats(const std::vector<std::vector<float>>& columns) {
    if (columns.empty()) {
        return {};
    }
    const std::size_t n_rows = columns[0].size();
    const auto mask = ComputeRowFiniteMask(columns, n_rows);

    std::vector<DimStats> stats(columns.size());
    for (std::size_t d = 0; d < columns.size(); ++d) {
        const auto& col = columns[d];
        auto& s = stats[d];

        // Raw NaN/Inf count over ALL rows, unfiltered -- this is the signal
        // validate_lbr_file.py's --check-nulls reports as a violation.
        for (std::size_t r = 0; r < n_rows; ++r) {
            if (!std::isfinite(col[r])) {
                ++s.nan_inf_count;
            }
        }

        double sum = 0.0, sum_sq = 0.0, zero_count = 0.0, one_count = 0.0;
        double min_v = std::numeric_limits<double>::infinity();
        double max_v = -std::numeric_limits<double>::infinity();
        std::size_t kept = 0;
        for (std::size_t r = 0; r < n_rows; ++r) {
            if (!mask[r]) continue;
            const double v = static_cast<double>(col[r]);
            sum += v;
            sum_sq += v * v;
            if (v == 0.0) zero_count += 1.0;
            if (v == 1.0) one_count += 1.0;
            min_v = std::min(min_v, v);
            max_v = std::max(max_v, v);
            ++kept;
        }
        s.finite_row_count = kept;
        if (kept > 0) {
            s.mean = sum / static_cast<double>(kept);
            const double variance = sum_sq / static_cast<double>(kept) - s.mean * s.mean;
            s.std_dev = std::sqrt(std::max(0.0, variance));
            s.min = min_v;
            s.max = max_v;
            s.zero_ratio = zero_count / static_cast<double>(kept);
            s.one_ratio = one_count / static_cast<double>(kept);
            s.constant_ratio = std::max(s.zero_ratio, s.one_ratio);
        }
    }
    return stats;
}

inline std::vector<std::vector<double>> ComputeCorrelationMatrix(
    const std::vector<std::vector<float>>& columns) {
    const std::size_t d = columns.size();
    if (d == 0) {
        return {};
    }
    const std::size_t n_rows = columns[0].size();
    const auto mask = ComputeRowFiniteMask(columns, n_rows);

    std::vector<double> sum(d, 0.0), sum_sq(d, 0.0);
    std::size_t n = 0;
    for (std::size_t r = 0; r < n_rows; ++r) {
        if (!mask[r]) continue;
        ++n;
        for (std::size_t i = 0; i < d; ++i) {
            const double v = static_cast<double>(columns[i][r]);
            sum[i] += v;
            sum_sq[i] += v * v;
        }
    }

    std::vector<std::vector<double>> sum_products(d, std::vector<double>(d, 0.0));
    for (std::size_t r = 0; r < n_rows; ++r) {
        if (!mask[r]) continue;
        for (std::size_t i = 0; i < d; ++i) {
            const double vi = static_cast<double>(columns[i][r]);
            for (std::size_t j = i; j < d; ++j) {
                sum_products[i][j] += vi * static_cast<double>(columns[j][r]);
            }
        }
    }

    // Sum-of-products Pearson formula: r = (n*Sxy - Sx*Sy) / sqrt((n*Sxx-Sx^2)*(n*Syy-Sy^2)).
    // Mathematically identical to np.corrcoef -- the population-vs-sample (N vs N-1)
    // normalization cancels out in the ratio, so this needs no separate divisor choice.
    std::vector<std::vector<double>> corr(d, std::vector<double>(d, 0.0));
    const double nd = static_cast<double>(n);
    for (std::size_t i = 0; i < d; ++i) {
        for (std::size_t j = i; j < d; ++j) {
            const double num = nd * sum_products[i][j] - sum[i] * sum[j];
            const double denom_i = nd * sum_sq[i] - sum[i] * sum[i];
            const double denom_j = nd * sum_sq[j] - sum[j] * sum[j];
            const double denom = std::sqrt(std::max(0.0, denom_i) * std::max(0.0, denom_j));
            const double value = (denom > 0.0) ? (num / denom) : std::numeric_limits<double>::quiet_NaN();
            corr[i][j] = value;
            corr[j][i] = value;
        }
    }
    return corr;
}

struct CorrelationPair {
    std::size_t dim_a;
    std::size_t dim_b;
    double corr;
    double abs_corr;
};

inline std::vector<CorrelationPair> AllFiniteCorrelationPairs(
    const std::vector<std::vector<double>>& corr) {
    std::vector<CorrelationPair> pairs;
    const std::size_t d = corr.size();
    for (std::size_t i = 0; i < d; ++i) {
        for (std::size_t j = i + 1; j < d; ++j) {
            if (std::isfinite(corr[i][j])) {
                pairs.push_back({i, j, corr[i][j], std::fabs(corr[i][j])});
            }
        }
    }
    return pairs;
}

inline std::vector<CorrelationPair> TopCorrelationPairs(
    const std::vector<std::vector<double>>& corr, std::size_t top_n) {
    auto pairs = AllFiniteCorrelationPairs(corr);
    std::sort(pairs.begin(), pairs.end(),
              [](const CorrelationPair& a, const CorrelationPair& b) { return a.abs_corr > b.abs_corr; });
    if (pairs.size() > top_n) {
        pairs.resize(top_n);
    }
    return pairs;
}

inline std::vector<CorrelationPair> PairsAboveThreshold(
    const std::vector<std::vector<double>>& corr, double threshold) {
    auto pairs = AllFiniteCorrelationPairs(corr);
    std::vector<CorrelationPair> above;
    for (const auto& p : pairs) {
        if (p.abs_corr > threshold) {
            above.push_back(p);
        }
    }
    return above;
}
