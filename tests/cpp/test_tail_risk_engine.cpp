// test_tail_risk_engine.cpp — unit tests for TailRiskEngine (Hill estimator,
// stability-region k-selection + EWMA-smoothed alpha)
//
// Build & run natively (no Sierra Chart deps, header-only):
//   g++ -std=c++17 -I include tests/cpp/test_tail_risk_engine.cpp -o /tmp/tre_test && /tmp/tre_test

#include "TailRiskEngine.h"
#include <cmath>
#include <cstdio>
#include <random>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}

int main() {
    using namespace MindfulTrader;
    // Synthetic Pareto-tailed data with known alpha=3.0 (Type I Pareto:
    // X = (1-U)^(-1/alpha), U~Uniform(0,1)).
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    TailRiskEngine engine(500, 0.05);
    for (int i = 0; i < 500; ++i) {
        const double u = uniform(rng);
        const double x = std::pow(1.0 - u, -1.0 / 3.0) - 1.0;  // shifted to center near 0
        engine.AddObservation(x * 0.001);  // scaled to a log-return-like magnitude
    }
    const double alpha = engine.GetHillAlpha();
    check("Hill alpha on synthetic alpha=3.0 Pareto data lands in [1.5, 5.0]",
          alpha >= 1.5 && alpha <= 5.0);

    // EWMA smoothing: two consecutive reads after adding one more observation
    // apart should not swing wildly (smoothing suppresses single-sample noise).
    //
    // NOTE: a genuinely *discriminating* perturbation must land inside the
    // scanned top-k order statistics, or a fixed-cutoff Hill computation with
    // no smoothing produces a *bit-identical* alpha (0% change) regardless of
    // whether smoothing exists -- verified empirically: overwriting the
    // evicted slot with a tiny unremarkable value (e.g. 0.0001) never enters
    // the top ~100 of 500 tail magnitudes here, so it is not discriminating.
    // A genuine outlier (~50x the median tail magnitude) reliably enters the
    // scanned k-range and swings the old fixed-k=25/no-smoothing computation
    // by ~25%, while the stability-region + EWMA-smoothed computation moves
    // by ~6% (confirmed empirically against both the pre-fix and post-fix
    // implementations before finalizing this test, and re-verified after the
    // 2026-08-13 post-review k-selector fix).
    const double alphaBefore = engine.GetHillAlpha();
    engine.AddObservation(0.05);  // one genuine outlier (~50x median tail magnitude)
    const double alphaAfter = engine.GetHillAlpha();
    check("EWMA-smoothed alpha changes by less than 15% on a single outlier observation",
          std::fabs(alphaAfter - alphaBefore) / alphaBefore < 0.15);

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
