// Unit tests for ActivityClockMeanReversion.h (pure, standalone mean-
// reversion elasticity score over activity-clock log-returns -- a PARALLEL
// implementation to CalculateMeanReversionSpeed, not a shared extraction;
// tested on its own merits, not against the original).
//
// Build & run natively (no Sierra Chart deps, header-only):
//   g++ -std=c++17 -I include tests/cpp/test_activity_clock_mean_reversion.cpp -o /tmp/acmr_test && /tmp/acmr_test

#include "ActivityClockMeanReversion.h"

#include <cmath>
#include <cstdio>
#include <vector>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}

// Independent lag-1 autocorrelation reference (same formula the header
// itself uses), to confirm the two synthetic fixtures below actually have
// the sign of serial correlation their names claim -- not assumed.
double ReferenceRho(const std::vector<float>& r) {
    const int n = static_cast<int>(r.size());
    double meanR = 0.0;
    for (float v : r) meanR += v;
    meanR /= n;
    double num = 0.0, den = 0.0;
    for (int t = 1; t < n; ++t) {
        const double rt = r[static_cast<std::size_t>(t)] - meanR;
        const double rPrev = r[static_cast<std::size_t>(t - 1)] - meanR;
        num += rt * rPrev;
        den += rPrev * rPrev;
    }
    return (den > 1e-12) ? (num / den) : 0.0;
}
}  // namespace

int main() {
    check("flat (zero) returns -> NaN (degenerate cumulative path)",
          std::isnan(ActivityClockMeanRevZ(std::vector<float>(100, 0.0f).data(), 100)));
    check("n < 5 -> NaN",
          std::isnan(ActivityClockMeanRevZ(std::vector<float>{0.01f, 0.02f, -0.01f}.data(), 3)));

    // Momentum fixture: a smooth two-level "regime step" -- returns are
    // near-constant within each half, giving strong positive lag-1
    // autocorrelation (consecutive returns are almost always similar),
    // while still producing a real, non-degenerate cumulative drift.
    std::vector<float> momentum(100);
    for (int i = 0; i < 100; ++i) {
        momentum[static_cast<std::size_t>(i)] = (i < 50) ? 0.002f : 0.004f;
    }
    const double momentumRho = ReferenceRho(momentum);
    check("momentum fixture: reference rho is strongly positive", momentumRho > 0.5);
    const float momentumScore = ActivityClockMeanRevZ(momentum.data(), 100);
    check("momentum fixture: score is finite", std::isfinite(momentumScore));

    // Mean-reverting fixture: asymmetric alternation -- returns flip sign
    // every step (classic anti-persistence signature, strong negative
    // lag-1 autocorrelation), with unequal magnitudes so the cumulative
    // path doesn't stay exactly centered on its own mean.
    std::vector<float> reverting(100);
    for (int i = 0; i < 100; ++i) {
        reverting[static_cast<std::size_t>(i)] = (i % 2 == 0) ? 0.003f : -0.0025f;
    }
    const double revertingRho = ReferenceRho(reverting);
    check("mean-reverting fixture: reference rho is strongly negative", revertingRho < -0.5);
    const float revertingScore = ActivityClockMeanRevZ(reverting.data(), 100);
    check("mean-reverting fixture: score is finite", std::isfinite(revertingScore));

    // The core claim this header exists to encode: momentum (positive rho)
    // dampens the score via the elasticity gate; mean-reversion (negative
    // rho) passes the full |z| through undamped. Both fixtures produce a
    // real, comparable cumulative excursion, so the score difference is
    // attributable to the elasticity gate, not just differing |z| inputs.
    check("mean-reverting score exceeds momentum score (elasticity gate dampens momentum, not reversion)",
          revertingScore > momentumScore);

    check("scores stay within contract [0, 5]",
          momentumScore >= 0.0f && momentumScore <= 5.0f &&
          revertingScore >= 0.0f && revertingScore <= 5.0f);

    // Exact regression values for the median/MAD reformulation (2026-09-02),
    // verified via real python3 execution against this exact algorithm (not
    // hand arithmetic) -- locks in the reformulation's real output, not just
    // its qualitative shape.
    check("momentum fixture matches the verified reformulated value",
          std::fabs(momentumScore - 0.039676f) < 1e-4f);
    check("mean-reverting fixture matches the verified reformulated value",
          std::fabs(revertingScore - 1.141446f) < 1e-4f);

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
