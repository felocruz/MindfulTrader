// test_liquidity_fragility_engine.cpp — characterization tests for
// LiquidityFragilityEngine's pure microstructure elasticity formula (dim 12,
// liq_fragility), extracted so it can be natively unit-tested without Sierra
// Chart/ACSIL deps, same pattern as test_carry_forward_calculators.cpp.
//
// Expected values independently computed via an exact Python port of the
// same algorithm.
//
// Build & run natively:
//   g++ -std=c++17 -I include tests/cpp/test_liquidity_fragility_engine.cpp -o /tmp/lfe_test && /tmp/lfe_test

#include "LiquidityFragilityEngine.h"

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
    std::printf("LiquidityFragilityEngine unit tests\n");

    // Neutral case: live bar's range/volume ratio matches the historical
    // median exactly (F_raw ~= 1.0) -- fragility should sit at ~0.5.
    {
        float rangeW[lfe::kWindow];
        float volW[lfe::kWindow];
        for (int i = 0; i < lfe::kWindow; ++i) { rangeW[i] = 2.0f; volW[i] = 10.0f; }
        const float result = lfe::ComputeLiquidityFragility(rangeW, volW, 4.0f, 400.0f, 0.5f);
        check("neutral_case_matches_reference", approx(result, 0.5000000f, 1e-3f));
    }

    // Fragile case: live range expands much faster than volume explains
    // (F_raw >> 1) -- fragility rises above the previous reading.
    {
        float rangeW[lfe::kWindow];
        float volW[lfe::kWindow];
        for (int i = 0; i < lfe::kWindow; ++i) { rangeW[i] = 2.0f; volW[i] = 10.0f; }
        const float result = lfe::ComputeLiquidityFragility(rangeW, volW, 20.0f, 100.0f, 0.2f);
        check("fragile_case_matches_reference", approx(result, 0.4370297f, 1e-3f));
    }

    // Thin live volume guard: below kLiveBarMinVolume (50) -- carries
    // prev_fragility forward unchanged rather than manufacturing a reading
    // from a near-empty denominator.
    {
        float rangeW[lfe::kWindow];
        float volW[lfe::kWindow];
        for (int i = 0; i < lfe::kWindow; ++i) { rangeW[i] = 2.0f; volW[i] = 10.0f; }
        const float result = lfe::ComputeLiquidityFragility(rangeW, volW, 4.0f, 10.0f, 0.42f);
        check("thin_volume_guard_carries_forward", approx(result, 0.42f));
    }

    // Contract: result always clamped to [0, 1].
    {
        float rangeW[lfe::kWindow];
        float volW[lfe::kWindow];
        for (int i = 0; i < lfe::kWindow; ++i) { rangeW[i] = 0.5f; volW[i] = 20.0f; }
        const float result = lfe::ComputeLiquidityFragility(rangeW, volW, 500.0f, 10000.0f, 0.9f);
        check("result_stays_within_contract_bounds", result >= 0.0f && result <= 1.0f);
    }

    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
