// tools/observation_vector/test_decision_boundary_calibration.cpp — unit tests for
// DecisionBoundaryCalibration.h, introduced by docs/superpowers/plans/2026-09-19-decision-
// boundary-calibration-tool-implementation.md.
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I tools/observation_vector tools/observation_vector/test_decision_boundary_calibration.cpp \
//     -o /tmp/dbc_test && /tmp/dbc_test

#include "DecisionBoundaryCalibration.h"

#include <cstdio>
#include <cmath>
#include <random>

namespace {

int g_failures = 0;

void check(const char* name, bool got, bool expected) {
    if (got == expected) {
        std::printf("  PASS  %s\n", name);
    } else {
        ++g_failures;
        std::printf("  FAIL  %s  got=%d exp=%d\n", name, got, expected);
    }
}

void checkNear(const char* name, double got, double expected, double eps) {
    const bool ok = std::fabs(got - expected) <= eps;
    if (ok) {
        std::printf("  PASS  %s (got=%.6f exp=%.6f)\n", name, got, expected);
    } else {
        ++g_failures;
        std::printf("  FAIL  %s  got=%.6f exp=%.6f (eps=%.6f)\n", name, got, expected, eps);
    }
}

}  // namespace

int main() {
    std::printf("DecisionBoundaryCalibration unit tests\n");

    // --- Task 1a: EmpiricalPercentileThreshold -------------------------------------------------
    {
        // 0..10 inclusive, 11 values -> PercentileFromSorted's linear-interpolation convention
        // (matches numpy default): P50 = 5.0 exactly, P90 = 9.0 exactly.
        std::vector<double> sorted = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
        checkNear("empirical_p50_is_median", EmpiricalPercentileThreshold(sorted, 0.50), 5.0, 1e-9);
        checkNear("empirical_p90_matches_hand_computed", EmpiricalPercentileThreshold(sorted, 0.90), 9.0, 1e-9);
        checkNear("empirical_target_rate_is_upper_tail_rate",
                   EmpiricalPercentileThreshold(sorted, 0.10 /* target rate */, /*upperTail=*/true),
                   9.0, 1e-9);
    }

    // --- Task 1b: PercentileMatchThreshold -----------------------------------------------------
    {
        // Old sample: 0..9 (10 values). Old threshold = 7 -> ECDF percentile = (values <= 7).mean()*100
        // = 8/10*100 = 80.0 (matches analyze_kurtosis_threshold_migration.py's exact `<=` convention).
        std::vector<double> oldSample = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9};
        // New sample: 0..99 in steps of 10 (10 values): 0,10,20,...,90. P80 (linear interp over
        // this 10-value sorted array) = value at index 0.80*9=7.2 -> interpolate between idx7=70
        // and idx8=80 -> 70 + 0.2*10 = 72.0.
        std::vector<double> newSample = {0, 10, 20, 30, 40, 50, 60, 70, 80, 90};
        checkNear("percentile_match_reproduces_hand_computed_mapping",
                   PercentileMatchThreshold(oldSample, newSample, 7.0), 72.0, 1e-9);
    }
    {
        // Threshold below every old sample value -> percentile 0 -> maps to new sample's minimum.
        std::vector<double> oldSample = {5, 6, 7, 8, 9};
        std::vector<double> newSample = {100, 200, 300, 400, 500};
        checkNear("percentile_match_threshold_below_all_maps_to_new_min",
                   PercentileMatchThreshold(oldSample, newSample, 0.0), 100.0, 1e-9);
    }

    // --- Task 2: GPDFit / FitGPD -----------------------------------------------------------------
    {
        // Synthetic GPD-distributed exceedances with known xi/sigma via inverse-CDF sampling:
        // X = sigma/xi * ((1-U)^-xi - 1), U~Uniform(0,1). Use xi=0.3, sigma=2.0 (Frechet/unbounded,
        // matches this repo's own common real-data finding, e.g. liq_fragility's xi=+0.3147).
        std::mt19937 rng(42);
        std::uniform_real_distribution<double> unif(0.0, 1.0);
        const double trueXi = 0.30;
        const double trueSigma = 2.0;
        const double threshold = 10.0;
        std::vector<float> values;
        values.reserve(50000);
        // Bulk of the sample sits below the threshold (values in [0, threshold)) so the p99 cut
        // lands near `threshold`; only the exceedance TAIL needs to follow the exact GPD law.
        for (int i = 0; i < 49000; ++i) {
            values.push_back(static_cast<float>(unif(rng) * threshold));
        }
        for (int i = 0; i < 1000; ++i) {
            const double u = unif(rng);
            const double exceedance = (trueSigma / trueXi) * (std::pow(1.0 - u, -trueXi) - 1.0);
            values.push_back(static_cast<float>(threshold + exceedance));
        }
        const GPDFit fit = FitGPD(values, values.size());
        check("gpd_fit_valid_with_enough_tail_samples", fit.valid, true);
        checkNear("gpd_fit_recovers_xi_within_tolerance", fit.xi, trueXi, 0.15);
        checkNear("gpd_fit_recovers_sigma_within_tolerance", fit.sigma, trueSigma, 1.0);
    }
    {
        // Too few samples -> not valid, no crash.
        std::vector<float> tiny = {1.0f, 2.0f, 3.0f};
        const GPDFit fit = FitGPD(tiny, tiny.size());
        check("gpd_fit_invalid_on_tiny_sample", fit.valid, false);
    }

    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "SOME FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
