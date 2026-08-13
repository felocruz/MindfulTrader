#include <cmath>
#include <cstdio>
#include <cstdint>
#include <vector>
#include <algorithm>
#include "RQAEpsilonSelector.h"

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}

// Replicates src/StudyHelperFunctions.cpp's CalculateRecurrenceRate measurement
// VERBATIM: the full n*n matrix with the line of identity included
// (recurCount seeded with n), strict `dist < epsilon`, symmetric pairs counted
// twice. If this ever diverges from production the test is measuring the wrong
// thing -- keep the two in lockstep.
double MeasuredFullMatrixRR(const std::vector<float>& prices, double epsilon) {
    const int n = static_cast<int>(prices.size());
    int recurCount = n;  // diagonal (i==i) always recurs
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            if (std::fabs(prices[static_cast<size_t>(i)] - prices[static_cast<size_t>(j)]) < epsilon) {
                recurCount += 2;
            }
        }
    }
    return static_cast<double>(recurCount) / (static_cast<double>(n) * static_cast<double>(n));
}

// Deterministic pseudo-random walk (no <random>, no platform-dependent engines)
// so the fixture is byte-identical on every toolchain. `sigma` sets the
// per-step volatility; the scale-invariance property under test is that the
// achieved recurrence rate does NOT depend on it.
std::vector<float> MakeWalk(int n, double sigma, uint32_t seed) {
    std::vector<float> p(static_cast<size_t>(n));
    uint32_t s = seed;
    double price = 100.0;
    for (int i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;                             // Numerical Recipes LCG
        const double u = static_cast<double>(s >> 8) / 16777216.0;  // [0,1)
        price += sigma * (2.0 * u - 1.0);
        p[static_cast<size_t>(i)] = static_cast<float>(price);
    }
    return p;
}
}  // namespace

// Declared here (at global scope, NOT inside the anonymous namespace above)
// to match the production signature this step will add to
// StudyHelperFunctions.h/.cpp -- copy the real implementation in Step 3,
// do not reimplement it differently for the test. Note: a forward
// declaration placed inside an anonymous namespace mangles to a
// TU-local symbol and can never link against (or unambiguously coexist
// with) an externally-defined or header-included global-scope function of
// the same name -- verified empirically while implementing this test.
double SelectEpsilonForTargetRecurrenceRate(const float* prices, int n, double targetRR);

int main() {
    constexpr int kN = 30;                 // TripleScreen2's slow_window_n floor
    constexpr double kTarget = 0.05;       // production RQA_TARGET_RECURRENCE_RATE
    // Achieved RR is quantised to 2/n^2 = 0.00222 steps (M is an integer number
    // of off-diagonal pairs), so the tolerance must admit +/-3 pairs of tie/
    // rounding slack. It is deliberately far tighter than the 1/n = 0.0333 the
    // line of identity contributes: the pre-2026-08-13 implementation, which
    // applied targetRR directly to the off-diagonal ranking, lands at
    // kTarget + 1/n = 0.0833 and FAILS every one of the target checks below.
    // (Verified by reverting the fix: all three target assertions fail.)
    constexpr double kTol = 0.008;

    // --- Case 1: low-volatility series -------------------------------------
    const std::vector<float> lowVol = MakeWalk(kN, 0.05, 12345u);
    const double epsLow = SelectEpsilonForTargetRecurrenceRate(lowVol.data(), kN, kTarget);
    const double rrLow = MeasuredFullMatrixRR(lowVol, epsLow);
    std::printf("  [low-vol]  eps=%.8f  measuredRR=%.5f\n", epsLow, rrLow);
    check("low-volatility series: achieved full-matrix RR hits the 0.05 target",
          std::fabs(rrLow - kTarget) < kTol);
    check("low-volatility series: epsilon is strictly positive", epsLow > 0.0);

    // --- Case 2: high-volatility series (spec Unit 2b's missing second case) -
    // 100x the per-step volatility AND a different seed, so this is a
    // genuinely different price path, not just a rescaling of Case 1.
    const std::vector<float> highVol = MakeWalk(kN, 5.0, 987654321u);
    const double epsHigh = SelectEpsilonForTargetRecurrenceRate(highVol.data(), kN, kTarget);
    const double rrHigh = MeasuredFullMatrixRR(highVol, epsHigh);
    std::printf("  [high-vol] eps=%.8f  measuredRR=%.5f\n", epsHigh, rrHigh);
    check("high-volatility series: achieved full-matrix RR hits the same 0.05 target",
          std::fabs(rrHigh - kTarget) < kTol);
    check("high-volatility series selects a much larger epsilon than the low-vol one "
          "(epsilon adapts to the series' scale)",
          epsHigh > epsLow * 10.0);

    // --- Case 3: exact scale invariance -------------------------------------
    // Multiplying every deviation-from-start by a constant multiplies every
    // pairwise distance by that constant, so the rank-based selection -- and
    // therefore the achieved recurrence rate -- must be identical.
    std::vector<float> scaled(lowVol.size());
    for (size_t i = 0; i < lowVol.size(); ++i) {
        scaled[i] = 100.0f + (lowVol[i] - 100.0f) * 250.0f;
    }
    const double epsScaled = SelectEpsilonForTargetRecurrenceRate(scaled.data(), kN, kTarget);
    const double rrScaled = MeasuredFullMatrixRR(scaled, epsScaled);
    check("scale invariance: 250x-rescaled series achieves the identical recurrence rate",
          std::fabs(rrScaled - rrLow) < 1e-12);

    // --- Case 4: line-of-identity accounting regression guard ----------------
    // The pre-fix implementation overshot by exactly the LOI's own 1/n
    // contribution. Assert we are nowhere near that, independently of kTol.
    const double preFixRR = kTarget + 1.0 / static_cast<double>(kN);
    check("achieved RR is not the pre-fix target+1/n overshoot (low-vol)",
          std::fabs(rrLow - preFixRR) > 0.02);
    check("achieved RR is not the pre-fix target+1/n overshoot (high-vol)",
          std::fabs(rrHigh - preFixRR) > 0.02);

    // --- Case 5: infeasible target -------------------------------------------
    // targetRR < 1/n cannot be reached: the diagonal alone is 1/n = 0.0333 at
    // n=30. The selector must fall back to the smallest achievable epsilon,
    // which counts zero off-diagonal pairs and yields exactly 1/n.
    const double epsInfeasible = SelectEpsilonForTargetRecurrenceRate(lowVol.data(), kN, 0.02);
    const double rrInfeasible = MeasuredFullMatrixRR(lowVol, epsInfeasible);
    check("infeasible target (< 1/n) falls back to the smallest achievable RR = 1/n",
          std::fabs(rrInfeasible - 1.0 / static_cast<double>(kN)) < 1e-12);
    check("infeasible-target epsilon is still strictly positive", epsInfeasible > 0.0);

    // --- Case 6: n=40 (the other end of the production window range) ---------
    const std::vector<float> wide = MakeWalk(40, 0.4, 42u);
    const double epsWide = SelectEpsilonForTargetRecurrenceRate(wide.data(), 40, kTarget);
    const double rrWide = MeasuredFullMatrixRR(wide, epsWide);
    std::printf("  [n=40]     eps=%.8f  measuredRR=%.5f\n", epsWide, rrWide);
    check("n=40 window: achieved full-matrix RR hits the 0.05 target",
          std::fabs(rrWide - kTarget) < kTol);

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
