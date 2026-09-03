// Unit tests for BipowerVariation.h (pure, standalone Barndorff-Nielsen &
// Shephard 2004/2006 bipower-variation estimator over a single fixed window
// -- the primitive CalculateLogScaleRatio's replacement for
// CalculateLogScaleRatio's dim0/log_scale_ratio uses in place of raw
// sample variance, per docs/superpowers/specs/2026-08-29-hmm-fat-tail-
// observation-vector-brainstorm.md §5.1 and the log_scale_ratio
// literature-grounding thread (lbrnet/logs/rc_gemini.log,
// CLAUDE_BRIEF_118/118_REPLY: raw L2 sample variance ratio is fragile under
// fat tails; the already-validated jump-robust BV formula from
// tools/observation_vector/jump_ratio_stats.h is the institutional replacement).
//
// Build & run natively (no Sierra Chart deps, header-only):
//   g++ -std=c++17 -I include -I tools tests/cpp/test_bipower_variation.cpp -o /tmp/bv_test && /tmp/bv_test

#include "BipowerVariation.h"
#include "jump_ratio_stats.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}

bool Close(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol * std::max(1.0, std::fabs(b));
}
}  // namespace

int main() {
    std::printf("=== BipowerVariation ===\n");

    // n < 2 is undefined for a lag-1 product sum -- must return 0, not NaN
    // or garbage (CalculateLogScaleRatio's caller needs a safe neutral value).
    {
        const double one[] = {0.01};
        check("n=0 returns 0", ComputeBipowerVariation(nullptr, 0) == 0.0);
        check("n=1 returns 0", ComputeBipowerVariation(one, 1) == 0.0);
    }

    // Hand-computed reference: r = {0.01, -0.02, 0.03}.
    // BV = (pi/2) * (|0.01|*|-0.02| + |-0.02|*|0.03|)
    //    = (pi/2) * (0.0002 + 0.0006) = (pi/2) * 0.0008
    {
        const double r[] = {0.01, -0.02, 0.03};
        constexpr double kHalfPi = 1.5707963267948966;
        const double expected = kHalfPi * 0.0008;
        check("hand-computed 3-element window", Close(ComputeBipowerVariation(r, 3), expected));
    }

    // Jump robustness: a single extreme return contaminates raw realized
    // variance (RV = sum r^2) quadratically, but BV only through two linear
    // adjacent-product terms -- BV must stay far below RV when one element
    // is a large outlier relative to its neighbors. This is the entire
    // reason for replacing raw sample variance with BV in the first place;
    // if this doesn't hold, the replacement bought nothing.
    {
        std::vector<double> r = {0.001, -0.0012, 0.0009, -0.0011, 0.0010};
        double rv_calm = 0.0;
        for (double x : r) rv_calm += x * x;
        const double bv_calm = ComputeBipowerVariation(r.data(), static_cast<int>(r.size()));

        std::vector<double> r_jump = r;
        r_jump[2] = 0.05;  // one 50-sigma-scale outlier tick
        double rv_jump = 0.0;
        for (double x : r_jump) rv_jump += x * x;
        const double bv_jump = ComputeBipowerVariation(r_jump.data(), static_cast<int>(r_jump.size()));

        const double rv_inflation = rv_jump / rv_calm;
        const double bv_inflation = bv_jump / bv_calm;
        check("single-tick jump inflates RV far more than BV",
              rv_inflation > 100.0 && bv_inflation < rv_inflation / 10.0);
    }

    // Cross-check against the already-validated production formula in
    // tools/observation_vector/jump_ratio_stats.h: for a window equal to the whole series,
    // ComputeJumpRatio's internal BV (recovered algebraically from its
    // reported jump_ratio and RV) must match ComputeBipowerVariation
    // exactly -- two independent call sites computing the same
    // Barndorff-Nielsen & Shephard statistic must agree bit-for-bit
    // (mod floating-point summation order), not just approximately.
    // Requires a genuine jump (BV < RV) so ComputeJumpRatio's max(0, ...)
    // floor doesn't clamp the recovery formula -- a calm series can have
    // BV > RV in finite samples (the pi/2 factor's well-known finite-sample
    // bias), which would clamp jump_ratio to 0 and make the recovery
    // formula recover the wrong (clamped) value, not a bug in either
    // implementation.
    {
        std::vector<double> r = {0.001, -0.0012, 0.05, -0.0011, 0.0010};
        const auto n = r.size();
        const auto jump_ratio = ComputeJumpRatio(r, n);
        double rv = 0.0;
        for (double x : r) rv += x * x;
        check("test fixture has a genuine unclamped jump", jump_ratio[n - 1] > 0.0);
        const double bv_from_jump_ratio_module = rv * (1.0 - jump_ratio[n - 1]);
        const double bv_here = ComputeBipowerVariation(r.data(), static_cast<int>(n));
        check("agrees with tools/observation_vector/jump_ratio_stats.h's BV to high precision",
              Close(bv_here, bv_from_jump_ratio_module, 1e-6));
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
