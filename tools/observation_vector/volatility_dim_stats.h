// tools/observation_vector/volatility_dim_stats.h
// Offline, sc-free ports of the three "volatility level" observation-vector
// calculators under redundancy investigation (lbrnet/logs/rc_gemini.log,
// CLAUDE_BRIEF_118/119 and reply): log_scale_ratio's OLD (pre-2026-08-31,
// raw centered sample variance) and NEW (bipower variation) formulas, plus
// correction_action's CURRENT (still raw, uncentered realized variance)
// formula. relative_range is intentionally NOT ported here -- it needs
// High/Low bar data this tick-close-only pipeline (market_data_io.h) does
// not carry; the redundancy check below is scoped to the two dims this
// thread's actual open question is about, not a full 3-way test.
//
// These are close ports of the production math (StudyHelperFunctions.cpp),
// not byte-identical: production is bar-indexed (sc.Index, sc.Close) with
// per-tick carry-forward/degenerate-input handling; these operate on a
// plain tick-to-tick log-return vector with no persistent state, matching
// this tool family's own established convention (jump_ratio_stats.h,
// drift_location_stats.h) of a tick-indexed proxy window rather than
// reconstructing exact bar boundaries offline. O(n*window) per series, not
// sliding-window-optimized: at this tool's real window sizes (hundreds of
// ticks, not tens of thousands), that's a few seconds over a 38.5M-row
// series -- not worth the bug surface of a hand-rolled incremental
// accumulator for a one-off offline diagnostic.
#pragma once

#include "BipowerVariation.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

namespace volatility_dim_stats_detail {
constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();
constexpr double kVarEps = 1e-12;

// short_n = max(8, long_n/4) -- identical derivation to production's
// CalculateLogScaleRatio (StudyHelperFunctions.cpp).
inline int ShortWindowFor(int long_n) {
    return std::max(8, long_n / 4);
}
}  // namespace volatility_dim_stats_detail

// OLD log_variance_ratio (pre-2026-08-31): log(short_var/long_var) using
// raw, MEAN-SUBTRACTED sample variance over each window -- the formula
// deleted from CalculateLogVariance for letting single-tick jumps dominate
// the sum of squares (Mandelbrot 1963). Windows are trailing, ending at and
// including index `idx`. Returns NaN if idx doesn't have long_n prior
// returns available.
inline double ComputeLogScaleRatioOld(const std::vector<double>& returns, std::size_t idx, int long_n) {
    using namespace volatility_dim_stats_detail;
    const int short_n = ShortWindowFor(long_n);
    if (idx + 1 < static_cast<std::size_t>(long_n)) return kNaN;

    auto window_variance = [&](int n) -> double {
        double sum = 0.0, sum_sq = 0.0;
        for (int i = 0; i < n; ++i) {
            const double r = returns[idx - static_cast<std::size_t>(i)];
            sum += r;
            sum_sq += r * r;
        }
        const double mean = sum / n;
        return std::max(sum_sq / n - mean * mean, 0.0);
    };

    const double short_var = window_variance(short_n);
    const double long_var = window_variance(long_n);
    const double log_ratio = std::log((short_var + kVarEps) / (long_var + kVarEps));
    return std::clamp(log_ratio, -6.0, 6.0);
}

// NEW log_scale_ratio (2026-08-31+): log(short_BV/long_BV), Barndorff-
// Nielsen & Shephard bipower variation in place of raw variance -- same
// window derivation as the OLD version above, so the two are directly
// comparable on identical windows.
//
// kMaxWindow=4096 bounds a fixed on-stack buffer (matches the same pattern
// StudyHelperFunctions.cpp's production CalculateLogScaleRatio uses,
// kMaxWindow=256, scaled up since this offline tool's --long-window can be
// set arbitrarily large): an earlier version allocated a std::vector per
// call, measured to make a real 38.5M-row pass not finish within a 280s
// timeout -- 77M heap allocations (2 windows x 2 calls x 38.5M indices)
// dominates cost regardless of window size at this row count.
inline double ComputeLogScaleRatioNew(const std::vector<double>& returns, std::size_t idx, int long_n) {
    using namespace volatility_dim_stats_detail;
    constexpr int kMaxWindow = 4096;
    const int short_n = ShortWindowFor(long_n);
    if (idx + 1 < static_cast<std::size_t>(long_n)) return kNaN;
    if (long_n > kMaxWindow) return kNaN;

    auto window_bv = [&](int n) -> double {
        double window[kMaxWindow];
        for (int i = 0; i < n; ++i) {
            window[n - 1 - i] = returns[idx - static_cast<std::size_t>(i)];
        }
        return ComputeBipowerVariation(window, n);
    };

    const double short_bv = window_bv(short_n);
    const double long_bv = window_bv(long_n);
    const double log_ratio = std::log((short_bv + kVarEps) / (long_bv + kVarEps));
    return std::clamp(log_ratio, -6.0, 6.0);
}

// OLD log_scale_expansion_ratio (pre-2026-08-31, under the name
// correction_action): log(RV_recent_half_rate / RV_full_rate), raw
// UNCENTERED sum-of-squared returns (RV = sum(r^2), no demeaning -- the
// classic realized-variance convention, distinct from OLD log_scale_ratio's
// mean-subtracted sample variance). Skips the production carry-forward-on-
// degenerate-input behavior (not meaningful for a bulk offline pass over
// already-valid historical data); returns NaN instead when the full-window
// rate is degenerate.
inline double ComputeLogScaleExpansionRatioOld(const std::vector<double>& returns, std::size_t idx, int lookback_n) {
    using namespace volatility_dim_stats_detail;
    const int half = lookback_n / 2;
    if (half < 2 || idx + 1 < static_cast<std::size_t>(lookback_n)) return kNaN;

    double rv_full = 0.0, rv_recent = 0.0;
    for (int i = 0; i < lookback_n; ++i) {
        const double r = returns[idx - static_cast<std::size_t>(i)];
        const double r2 = r * r;
        rv_full += r2;
        if (i < half) rv_recent += r2;
    }
    const double rv_full_rate = rv_full / lookback_n;
    const double rv_recent_rate = rv_recent / half;
    if (rv_full_rate < kVarEps) return kNaN;

    const double log_ratio = std::log(std::max(rv_recent_rate, kVarEps) / rv_full_rate);
    return std::clamp(log_ratio, -10.0, 6.0);
}

// NEW log_scale_expansion_ratio (2026-08-31+): log(BV_recent_half_rate /
// BV_full_rate) -- Barndorff-Nielsen & Shephard bipower variation in place
// of raw uncentered RV, same window structure as OLD above so the two are
// directly comparable. Matches production's CalculateLogScaleExpansionRatio.
inline double ComputeLogScaleExpansionRatioNew(const std::vector<double>& returns, std::size_t idx, int lookback_n) {
    using namespace volatility_dim_stats_detail;
    constexpr int kMaxWindow = 4096;
    const int half = lookback_n / 2;
    if (half < 2 || idx + 1 < static_cast<std::size_t>(lookback_n)) return kNaN;
    if (lookback_n > kMaxWindow) return kNaN;

    auto window_bv = [&](int n) -> double {
        double window[kMaxWindow];
        for (int i = 0; i < n; ++i) {
            window[n - 1 - i] = returns[idx - static_cast<std::size_t>(i)];
        }
        return ComputeBipowerVariation(window, n);
    };

    const double bv_full = window_bv(lookback_n);
    const double bv_recent = window_bv(half);
    const double bv_full_rate = bv_full / lookback_n;
    const double bv_recent_rate = bv_recent / half;
    if (bv_full_rate < kVarEps) return kNaN;

    const double log_ratio = std::log(std::max(bv_recent_rate, kVarEps) / bv_full_rate);
    return std::clamp(log_ratio, -10.0, 6.0);
}

// Pearson correlation between two equal-length series, ignoring any index
// where either value is non-finite (NaN warmup entries).
inline double PearsonCorrelation(const std::vector<double>& a, const std::vector<double>& b) {
    double sum_a = 0.0, sum_b = 0.0;
    std::size_t n = 0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) continue;
        sum_a += a[i];
        sum_b += b[i];
        ++n;
    }
    if (n < 2) return volatility_dim_stats_detail::kNaN;
    const double mean_a = sum_a / static_cast<double>(n);
    const double mean_b = sum_b / static_cast<double>(n);

    double cov = 0.0, var_a = 0.0, var_b = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (!std::isfinite(a[i]) || !std::isfinite(b[i])) continue;
        const double da = a[i] - mean_a;
        const double db = b[i] - mean_b;
        cov += da * db;
        var_a += da * da;
        var_b += db * db;
    }
    const double denom = std::sqrt(var_a * var_b);
    return denom > 0.0 ? cov / denom : volatility_dim_stats_detail::kNaN;
}

struct Eigen3x3Result {
    double eig1, eig2, eig3;  // ascending order
};

// Closed-form eigenvalues of a real symmetric 3x3 matrix (Smith 1961 /
// the standard trigonometric closed form -- see e.g. "Eigenvalue
// algorithm" for the 3x3 symmetric case). Matrix given as its upper
// triangle: {a00, a01, a02, a11, a12, a22}.
inline Eigen3x3Result Symmetric3x3Eigenvalues(double a00, double a01, double a02,
                                               double a11, double a12, double a22) {
    const double p1 = a01 * a01 + a02 * a02 + a12 * a12;
    if (p1 == 0.0) {
        double lo = std::min({a00, a11, a22});
        double hi = std::max({a00, a11, a22});
        double mid = a00 + a11 + a22 - lo - hi;
        return {lo, mid, hi};
    }
    const double q = (a00 + a11 + a22) / 3.0;
    const double p2 = (a00 - q) * (a00 - q) + (a11 - q) * (a11 - q) + (a22 - q) * (a22 - q) + 2.0 * p1;
    const double p = std::sqrt(p2 / 6.0);
    const double b00 = (a00 - q) / p, b01 = a01 / p, b02 = a02 / p;
    const double b11 = (a11 - q) / p, b12 = a12 / p, b22 = (a22 - q) / p;
    // det(B) for symmetric B
    const double det_b = b00 * (b11 * b22 - b12 * b12) - b01 * (b01 * b22 - b12 * b02) +
                          b02 * (b01 * b12 - b11 * b02);
    double r = std::clamp(det_b / 2.0, -1.0, 1.0);
    const double phi = std::acos(r) / 3.0;
    constexpr double kTwoPiOver3 = 2.0943951023931953;
    const double eig_max = q + 2.0 * p * std::cos(phi);
    const double eig_min = q + 2.0 * p * std::cos(phi + kTwoPiOver3);
    const double eig_mid = 3.0 * q - eig_max - eig_min;
    return {eig_min, eig_mid, eig_max};
}
