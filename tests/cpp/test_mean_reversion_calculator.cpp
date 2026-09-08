// test_mean_reversion_calculator.cpp — characterization tests for
// MeanReversionCalculator's pure mean-reversion elasticity formula (dim 16,
// mean_rev_z), extracted so it can be natively unit-tested without Sierra
// Chart/ACSIL deps, same pattern as test_carry_forward_calculators.cpp.
//
// Expected values independently computed via an exact Python port of the
// same algorithm (nth_element semantics == sorted array at index n/2, not
// the textbook averaged-middle-two for even n).
//
// Build & run natively:
//   g++ -std=c++17 -I include tests/cpp/test_mean_reversion_calculator.cpp -o /tmp/mrc_test && /tmp/mrc_test

#include "MeanReversionCalculator.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void check(const char* name, bool ok) {
    if (ok) {
        std::printf("  PASS  %s\n", name);
    } else {
        ++g_failures;
        std::printf("  FAIL  %s\n", name);
    }
}

bool approx(float a, float b, float tol = 1e-4f) {
    return std::fabs(a - b) <= tol;
}

}  // namespace

int main() {
    std::printf("MeanReversionCalculator unit tests\n");

    // Flat window: MAD collapses to exactly 0 -- degenerate, carries the
    // last valid value forward instead of a fabricated exact-zero reading.
    {
        float prices[7] = {100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f};
        check("flat_window_carries_last_valid_forward",
              approx(mrc::ComputeMeanReversionZ(prices, 7, 0.33f), 0.33f));
    }

    // n=7, jump at the end: full path (median/MAD z-score + autocorrelation
    // elasticity gate). Independently computed via Python reference port.
    {
        float prices[7] = {100.0f, 101.0f, 99.0f, 102.0f, 98.0f, 103.0f, 110.0f};
        check("n7_full_path_matches_reference",
              approx(mrc::ComputeMeanReversionZ(prices, 7, 0.0f), 2.8224230f, 1e-3f));
    }

    // n=4 (m=3, still full path -- m<3 threshold not yet crossed).
    {
        float prices[4] = {100.0f, 102.0f, 101.0f, 105.0f};
        check("n4_full_path_matches_reference",
              approx(mrc::ComputeMeanReversionZ(prices, 4, 0.0f), 0.9873349f, 1e-3f));
    }

    // n=3 (m=2 < 3): autocorrelation term skipped, pure |z| returned.
    {
        float prices[3] = {100.0f, 102.0f, 108.0f};
        check("n3_skips_autocorrelation_matches_reference",
              approx(mrc::ComputeMeanReversionZ(prices, 3, 0.0f), 1.9468539f, 1e-3f));
    }

    // Contract: result is always clamped to [0, 5] even for an extreme jump.
    {
        float prices[5] = {100.0f, 100.0f, 100.0f, 100.0f, 100000.0f};
        const float result = mrc::ComputeMeanReversionZ(prices, 5, 0.0f);
        check("result_stays_within_contract_bounds", result >= 0.0f && result <= 5.0f);
    }

    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
