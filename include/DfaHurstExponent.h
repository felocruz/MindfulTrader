// DfaHurstExponent.h -- pure, header-only, ACSIL-independent Detrended
// Fluctuation Analysis (DFA) Hurst-exponent estimator. Extracted from
// StudyHelperFunctions.cpp's CalculateHurstExponent so it can be natively
// tested and driven by ActivityClockManager's imbalance-bar returns buffer
// directly (same extraction rationale/precedent as SevcikFractalDimension.h/
// RQAEpsilonSelector.h/RecurrenceRateEngine.h).
//
// Despite variable/comment names elsewhere in this codebase saying "R/S",
// this is Detrended Fluctuation Analysis, not classic rescaled-range: a
// de-meaned cumulative profile of log-returns is segmented at each scale s,
// each segment is linearly detrended, and the Hurst exponent is the
// log(fluctuation)-vs-log(scale) regression slope.
//
// `logReturns` must already be log-returns (not prices), oldest first, most
// recent last -- exactly ImbalanceBarEngine::GetImbalanceBarReturns()'s own
// ordering, and exactly what CalculateHurstExponent's own internal
// price-to-return loop produces before this math begins.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

/// Largest window this estimator supports (matches CalculateHurstExponent's
/// own capacity constant -- fixed-capacity stack buffers, no heap allocation).
inline constexpr int kDfaMaxWindow = 512;

/// Returns NaN for a degenerate/insufficient-data window -- callers own the
/// carry-forward/cold-start fallback decision (this function holds no
/// persistent state, unlike CalculateHurstExponent).
inline float DfaHurstExponent(const float* logReturns, int length, int minScale) {
    length = std::clamp(length, 16, kDfaMaxWindow);
    minScale = std::clamp(minScale, 4, 64);
    if (length < minScale * 4) return std::numeric_limits<float>::quiet_NaN();

    std::array<double, kDfaMaxWindow> profile{};
    std::array<double, kDfaMaxWindow> logScales{};
    std::array<double, kDfaMaxWindow> logFluctuations{};

    double sumReturns = 0.0;
    for (int i = 0; i < length; ++i) {
        sumReturns += static_cast<double>(logReturns[i]);
    }
    const double meanReturn = sumReturns / static_cast<double>(length);

    double cumulative = 0.0;
    for (int i = 0; i < length; ++i) {
        cumulative += (static_cast<double>(logReturns[i]) - meanReturn);
        profile[static_cast<std::size_t>(i)] = cumulative;
    }

    const int maxScale = length / 4;
    if (maxScale <= minScale) return std::numeric_limits<float>::quiet_NaN();

    const int step = (maxScale - minScale > 50) ? 2 : 1;
    int validScaleCount = 0;

    for (int s = minScale; s <= maxScale; s += step) {
        const int numSegments = length / s;
        if (numSegments < 1) continue;

        double totalVariance = 0.0;
        int usedSegments = 0;

        for (int v = 0; v < numSegments; ++v) {
            const int startIndex = v * s;
            const double n = static_cast<double>(s);
            const double sumX = n * (n - 1.0) * 0.5;
            const double sumX2 = n * (n - 1.0) * (2.0 * n - 1.0) / 6.0;
            const double denom = n * sumX2 - sumX * sumX;
            if (std::fabs(denom) < 1e-12) continue;

            double sumY = 0.0;
            double sumXY = 0.0;
            for (int k = 0; k < s; ++k) {
                const double y = profile[static_cast<std::size_t>(startIndex + k)];
                sumY += y;
                sumXY += static_cast<double>(k) * y;
            }

            const double slope = (n * sumXY - sumX * sumY) / denom;
            const double intercept = (sumY - slope * sumX) / n;

            double ssr = 0.0;
            for (int k = 0; k < s; ++k) {
                const double trend = slope * static_cast<double>(k) + intercept;
                const double diff = profile[static_cast<std::size_t>(startIndex + k)] - trend;
                ssr += diff * diff;
            }

            totalVariance += (ssr / n);
            ++usedSegments;
        }

        if (usedSegments == 0) continue;

        const double f_s = std::sqrt(totalVariance / static_cast<double>(usedSegments));
        if (f_s > 1e-12 && validScaleCount < kDfaMaxWindow) {
            logScales[static_cast<std::size_t>(validScaleCount)] = std::log(static_cast<double>(s));
            logFluctuations[static_cast<std::size_t>(validScaleCount)] = std::log(f_s);
            ++validScaleCount;
        }
    }

    if (validScaleCount < 2) return std::numeric_limits<float>::quiet_NaN();

    const double n = static_cast<double>(validScaleCount);
    double sumX = 0.0, sumY = 0.0, sumXY = 0.0, sumX2 = 0.0;
    for (int i = 0; i < validScaleCount; ++i) {
        const double x = logScales[static_cast<std::size_t>(i)];
        const double y = logFluctuations[static_cast<std::size_t>(i)];
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumX2 += x * x;
    }

    const double regressionDenom = n * sumX2 - sumX * sumX;
    if (std::fabs(regressionDenom) < 1e-12) return std::numeric_limits<float>::quiet_NaN();

    float hurst = static_cast<float>((n * sumXY - sumX * sumY) / regressionDenom);
    hurst = std::clamp(hurst, 0.0f, 1.5f);
    if (!std::isfinite(hurst)) return std::numeric_limits<float>::quiet_NaN();

    return hurst;
}
