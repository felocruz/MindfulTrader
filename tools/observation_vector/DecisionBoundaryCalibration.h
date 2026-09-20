// tools/observation_vector/DecisionBoundaryCalibration.h — generic, dim-agnostic decision-boundary
// calibration core, introduced by docs/superpowers/plans/2026-09-19-decision-boundary-calibration-
// tool-implementation.md (spec: docs/superpowers/specs/2026-09-19-decision-boundary-calibration-
// tool-spec.md). Replaces the prior one-bespoke-script-per-migration-event pattern
// (analyze_kurtosis_threshold_migration.py, fractal_dim_threshold_migration.py, etc.) with three
// reusable modes. Pure, header-only, no Arrow/Sierra Chart dependency -- natively unit-testable
// (test_decision_boundary_calibration.cpp).
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <vector>

#include "market_test_stats.h"  // PercentileFromSorted

// ---------------------------------------------------------------------------------------------
// Mode 1: empirical-percentile -- "what value does this dim exceed exactly `targetRate` of the
// time", the fresh-gate case with no legacy threshold to anchor against (spec §4 mode 2).
// upperTail=true (default) treats targetRate as an exceedance rate (returns the (1-targetRate)
// percentile); upperTail=false treats it as a plain percentile in [0,1].
// ---------------------------------------------------------------------------------------------
inline double EmpiricalPercentileThreshold(const std::vector<double>& sortedValues,
                                            double targetRate,
                                            bool upperTail = false) {
    const double p = (upperTail ? (1.0 - targetRate) : targetRate) * 100.0;
    return PercentileFromSorted(sortedValues, p);
}

// ---------------------------------------------------------------------------------------------
// Mode 2: percentile-match -- ports analyze_kurtosis_threshold_migration.py's exact algorithm
// (the only existing threshold migration in this repo with a real, saved, rerunnable pipeline):
// find the old threshold's ECDF percentile within the old-scale sample (fraction of old samples
// <= oldThreshold), then return the value at that SAME percentile within the new-scale sample.
// Both samples are paired observations of the same underlying events under the old vs. new
// formula (spec §4 mode 1 -- for scale-migration events, not fresh gates).
// ---------------------------------------------------------------------------------------------
inline double PercentileMatchThreshold(const std::vector<double>& oldSample,
                                        const std::vector<double>& newSample,
                                        double oldThreshold) {
    std::size_t countLE = 0;
    for (double v : oldSample) {
        if (v <= oldThreshold) ++countLE;
    }
    const double percentile = oldSample.empty()
        ? 0.0
        : (static_cast<double>(countLE) / static_cast<double>(oldSample.size())) * 100.0;

    std::vector<double> sortedNew = newSample;
    std::sort(sortedNew.begin(), sortedNew.end());
    return PercentileFromSorted(sortedNew, percentile);
}

// ---------------------------------------------------------------------------------------------
// Mode 3: EVT/GPD -- Peaks-Over-Threshold fit, Method of Moments (Hosking & Wallis 1987), for
// dims with a genuine heavy/unbounded tail (spec §4 mode 3). Extracted verbatim (not rewritten)
// from tools/observation_vector/observation_vector_recalibration.cpp's FitGPD() -- its 2nd real
// use, per market_test_stats.h's own "extract on 2nd/3rd real use" convention. That file's own
// call site is left untouched; this is an additive extraction, not a migration.
// ---------------------------------------------------------------------------------------------
struct GPDFit {
    double u = 0.0;
    std::size_t nTail = 0;
    std::size_t nSample = 0;
    std::size_t totalObserved = 0;
    double xi = 0.0;
    double sigma = 0.0;
    double returnLevel = 0.0;
    bool valid = false;
};

inline GPDFit FitGPD(std::vector<float> values, std::size_t totalObservedCount) {
    GPDFit fit;
    fit.nSample = values.size();
    fit.totalObserved = totalObservedCount;
    if (fit.nSample < 200) return fit;  // too few samples for a trustworthy tail fit

    std::sort(values.begin(), values.end());
    const std::size_t n = values.size();
    const std::size_t p99Idx = std::min(static_cast<std::size_t>(0.99 * static_cast<double>(n)), n - 1);
    fit.u = static_cast<double>(values[p99Idx]);

    std::vector<double> exceedances;
    exceedances.reserve(n - p99Idx);
    for (std::size_t i = p99Idx; i < n; ++i) {
        const double y = static_cast<double>(values[i]) - fit.u;
        if (y > 0.0) exceedances.push_back(y);
    }
    fit.nTail = exceedances.size();
    if (fit.nTail < 20) return fit;  // too few exceedances for a trustworthy MOM fit

    double sum = 0.0, sumSq = 0.0;
    for (double y : exceedances) { sum += y; sumSq += y * y; }
    const double mean = sum / static_cast<double>(fit.nTail);
    const double var = (sumSq / static_cast<double>(fit.nTail)) - mean * mean;
    if (var <= 0.0 || mean <= 0.0) return fit;

    // Hosking & Wallis (1987) Method of Moments estimators.
    fit.xi = 0.5 * (1.0 - (mean * mean) / var);
    fit.sigma = 0.5 * mean * (1.0 + (mean * mean) / var);

    // p = 1/N return level: N = totalObservedCount (this dim's real observation count), matching
    // FeatureScaler.h's own convention throughout.
    const double zetaU = static_cast<double>(fit.nTail) / static_cast<double>(n);  // P(X > u) within the retained sample
    const double m = static_cast<double>(fit.totalObserved) * zetaU;
    if (std::fabs(fit.xi) < 1e-6) {
        fit.returnLevel = fit.u + fit.sigma * std::log(std::max(m, 1.0));
    } else {
        fit.returnLevel = fit.u + (fit.sigma / fit.xi) * (std::pow(std::max(m, 1.0), fit.xi) - 1.0);
    }
    fit.valid = true;
    return fit;
}
