// test_asymmetry_context_buckets.cpp — unit tests for the AsymmetryContext regime-bucket
// classifiers (IndicatorComputations.h), introduced 2026-09-19 per spec 2026-09-19-meaningful-
// event-trigger-and-asymmetry-context-significance-spec.md §8e-8h.
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I include tests/cpp/test_asymmetry_context_buckets.cpp \
//     -o /tmp/asym_bucket_test && /tmp/asym_bucket_test

#include "IndicatorComputations.h"

#include <cstdio>

namespace {

int g_failures = 0;

void check(const char* name, int got, int expected) {
    if (got == expected) {
        std::printf("  PASS  %s\n", name);
    } else {
        ++g_failures;
        std::printf("  FAIL  %s  got=%d exp=%d\n", name, got, expected);
    }
}

}  // namespace

int main() {
    std::printf("AsymmetryContext bucket classifier unit tests\n");

    // --- taleb_kurtosis: bounds {1.3248, 1.3809, 1.5650, 1.6414, 1.7592, 2.0064} -> 7 buckets ---
    check("kurtosis_below_all_bounds_bucket0", ClassifyKurtosisBucket(1.0f), 0);
    check("kurtosis_at_first_bound_still_bucket0", ClassifyKurtosisBucket(1.3248f), 0);
    check("kurtosis_just_above_first_bound_bucket1", ClassifyKurtosisBucket(1.35f), 1);
    check("kurtosis_between_bound2_and_3_bucket2", ClassifyKurtosisBucket(1.5f), 2);
    check("kurtosis_between_bound3_and_4_bucket3", ClassifyKurtosisBucket(1.6f), 3);
    check("kurtosis_between_bound4_and_5_bucket4", ClassifyKurtosisBucket(1.7f), 4);
    check("kurtosis_between_bound5_and_6_bucket5", ClassifyKurtosisBucket(1.9f), 5);
    check("kurtosis_above_all_bounds_bucket6", ClassifyKurtosisBucket(5.0f), 6);

    // --- taleb_skewness: +/-0.1544 -> 3 buckets (-1/0/1) ---
    check("skewness_very_negative", ClassifySkewnessBucket(-0.5f), -1);
    check("skewness_neutral_zero", ClassifySkewnessBucket(0.0f), 0);
    check("skewness_at_positive_bound_still_neutral", ClassifySkewnessBucket(0.1544f), 0);
    check("skewness_very_positive", ClassifySkewnessBucket(0.5f), 1);

    // --- taleb_cliff: 0.50 -> 2 buckets ---
    check("cliff_below_bound", ClassifyCliffBucket(0.2f), 0);
    check("cliff_at_bound_is_upper_bucket", ClassifyCliffBucket(0.50f), 1);
    check("cliff_above_bound", ClassifyCliffBucket(1.0f), 1);

    // --- roughness_ratio: 3.33 -> 2 buckets (trending/choppy) ---
    check("roughness_trending", ClassifyRoughnessBucket(2.0f), 0);
    check("roughness_choppy", ClassifyRoughnessBucket(4.0f), 1);

    // --- raschke_burst: 1/3, 1/2 -> 3 buckets ---
    check("burst_normal", ClassifyBurstBucket(0.0f), 0);
    check("burst_caution", ClassifyBurstBucket(0.4f), 1);
    check("burst_deny", ClassifyBurstBucket(0.6f), 2);

    // --- shannon_entropy fraction: {0.45, 0.60, 0.80, 0.90} -> 5 buckets ---
    check("entropy_lowest_bucket", ClassifyEntropyFractionBucket(0.2f), 0);
    check("entropy_second_bucket", ClassifyEntropyFractionBucket(0.5f), 1);
    check("entropy_third_bucket", ClassifyEntropyFractionBucket(0.7f), 2);
    check("entropy_fourth_bucket", ClassifyEntropyFractionBucket(0.85f), 3);
    check("entropy_highest_bucket", ClassifyEntropyFractionBucket(0.95f), 4);

    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "SOME FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
