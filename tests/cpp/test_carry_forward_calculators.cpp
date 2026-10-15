// test_carry_forward_calculators.cpp — characterization tests for
// CarryForwardCalculators' pure degenerate-guard formulas (dims 2, 3, 8:
// relative_range, log_scale_expansion_ratio, fisher_info), extracted so they
// can be natively unit-tested without Sierra Chart/ACSIL deps, following the
// same pattern as OrderFlowAsymmetryEngine.h (dim 7)
// (docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md D1).
// ComputeBurstinessIndex below is exercised generically (it's dim3's real
// caller now, not dim1's -- see CarryForwardCalculators.h's own header comment).
//
// Build & run natively:
//   g++ -std=c++17 -I include tests/cpp/test_carry_forward_calculators.cpp -o /tmp/cfc_test && /tmp/cfc_test

#include "CarryForwardCalculators.h"

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
    std::printf("CarryForwardCalculators unit tests\n");

    // --- ComputeBurstinessIndex (dim3's real caller; generic formula test) ---
    check("burstiness_normal_case_matches_log_ratio",
          approx(cfc::ComputeBurstinessIndex(4.0, 2.0, 0.0f),
                 static_cast<float>(std::log(4.0 / 2.0))));

    check("burstiness_degenerate_older_rate_carries_forward",
          approx(cfc::ComputeBurstinessIndex(4.0, 1e-13, 0.42f), 0.42f));

    check("burstiness_degenerate_no_prior_value_returns_neutral",
          approx(cfc::ComputeBurstinessIndex(4.0, 1e-13, 0.0f), 0.0f));

    // Unbounded log ratio: not clamped (spec B12). ln(1e24) = 55.26; the real
    // dim3 quiet-regime extreme ln(0.0021062) = -6.16 passes through intact.
    check("burstiness_extreme_ratio_not_clamped",
          approx(cfc::ComputeBurstinessIndex(1e12, 1e-12, 0.0f), 55.26204f, 1e-3f));
    check("burstiness_extreme_negative_ratio_not_clamped",
          approx(cfc::ComputeBurstinessIndex(1e-12, 1e12, 0.0f), -55.26204f, 1e-3f));
    check("burstiness_real_dim3_extreme_preserved",
          approx(cfc::ComputeBurstinessIndex(0.0021062, 1.0, 0.0f), -6.16f, 0.01f));

    // --- ComputeRelativeRange (dim 2) ---
    check("relative_range_normal_case",
          approx(cfc::ComputeRelativeRange(105.0f, 100.0f, 2.5f, 0.0f), 2.0f));

    check("relative_range_degenerate_atr_carries_forward",
          approx(cfc::ComputeRelativeRange(105.0f, 100.0f, 0.0f, 1.3f), 1.3f));

    check("relative_range_degenerate_no_prior_value_returns_neutral",
          approx(cfc::ComputeRelativeRange(105.0f, 100.0f, 0.0f, 0.0f), 0.0f));

    // --- ComputeFisherInformation (dim 8) ---
    check("fisher_info_midpoint_price_is_zero",
          approx(cfc::ComputeFisherInformation(100.0f, 110.0f, 105.0f, 0.0f), 0.0f));

    check("fisher_info_near_high_is_positive",
          approx(cfc::ComputeFisherInformation(100.0f, 110.0f, 109.0f, 0.0f),
                 0.5f * static_cast<float>(std::log(1.8 / 0.2))));

    check("fisher_info_degenerate_flat_price_carries_forward",
          approx(cfc::ComputeFisherInformation(100.0f, 100.0f, 100.0f, -0.85f), -0.85f));

    check("fisher_info_degenerate_no_prior_value_returns_neutral",
          approx(cfc::ComputeFisherInformation(100.0f, 100.0f, 100.0f, 0.0f), 0.0f));

    // --- ComputeAmihudIlliquidity (dim 11) -- geometric mean of log-ratios,
    // reformulated 2026-09-03 (sqrt-law volume scaling + log-space
    // aggregation, see CarryForwardCalculators.h). `sumLogRatio` is the
    // accumulated SUM OF LOGS of each sample's ratio (the call site
    // accumulates ln(ratio + eps) per sample, not the raw ratios).
    {
        const double sumLogRatio = std::log(0.002) + std::log(0.003) + std::log(0.001);
        check("amihud_normal_case_is_geometric_mean",
              approx(cfc::ComputeAmihudIlliquidity(sumLogRatio, 3, 0.0f), 0.0018171206f, 1e-6f));
    }

    check("amihud_degenerate_count_carries_forward",
          approx(cfc::ComputeAmihudIlliquidity(std::log(0.006), 1, 0.42f), 0.42f));

    check("amihud_degenerate_zero_count_carries_forward",
          approx(cfc::ComputeAmihudIlliquidity(0.0, 0, 0.42f), 0.42f));

    check("amihud_degenerate_no_prior_value_returns_neutral",
          approx(cfc::ComputeAmihudIlliquidity(std::log(0.006), 1, 0.0f), 0.0f));

    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
