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

// Fills an engine with 500 draws from a Type I Pareto tail of known alpha=3.0
// (X = (1-U)^(-1/alpha)), scaled to log-return-like magnitudes.
void FillPareto(MindfulTrader::TailRiskEngine& engine, unsigned seed, int n = 500) {
    std::mt19937 rng(seed);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    for (int i = 0; i < n; ++i) {
        const double u = uniform(rng);
        const double x = std::pow(1.0 - u, -1.0 / 3.0) - 1.0;  // shifted to center near 0
        engine.AddObservation(x * 0.001);                       // log-return-like magnitude
    }
}
}  // namespace

int main() {
    using namespace MindfulTrader;

    TailRiskEngine engine(500, 0.05);
    FillPareto(engine, 42);
    const double alpha = engine.GetHillAlpha();
    std::printf("  alpha (alpha=3.0 Pareto, k-scan in [10,100]) = %.4f\n", alpha);
    check("Hill alpha on synthetic alpha=3.0 Pareto data lands in [1.5, 5.0]",
          alpha >= 1.5 && alpha <= 5.0);

    // --- EWMA smoothing suppresses single-observation noise -----------------
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
    // 2026-08-13 post-review k-selector fix and the Finding 6/10 fixes).
    const double alphaBefore = engine.GetHillAlpha();
    engine.AddObservation(0.05);  // one genuine outlier (~50x median tail magnitude)
    const double alphaAfter = engine.GetHillAlpha();
    std::printf("  outlier swing: %.4f -> %.4f (%.2f%%)\n", alphaBefore, alphaAfter,
                100.0 * std::fabs(alphaAfter - alphaBefore) / alphaBefore);
    check("EWMA-smoothed alpha changes by less than 15% on a single outlier observation",
          std::fabs(alphaAfter - alphaBefore) / alphaBefore < 0.15);
    check("a single outlier still MOVES the smoothed alpha (smoothing damps, does not freeze)",
          std::fabs(alphaAfter - alphaBefore) > 1e-9);

    // --- Finding 6: read cadence must not affect the smoothed series --------
    //
    // This is the property the fix exists to guarantee. When the EWMA advanced
    // inside GetHillAlpha(), N extra reads between two real observations drove
    // m_smoothedAlpha geometrically toward the raw estimate, so the *effective*
    // smoothing constant depended on how often callers happened to read --
    // weakest exactly when publish rates spike (high volatility). Two engines
    // fed the IDENTICAL observation sequence but read at wildly different rates
    // must now end up bit-identical.
    //
    // The fixture is a deliberate volatility-regime shift, because that is when
    // raw alpha moves fast and the two cadences pull apart most: a stationary
    // series makes raw ~= smoothed and hides the defect. Verified against the
    // pre-fix implementation, which diverges by 42.3% here (the burst-read
    // engine has effectively no smoothing left and tracks raw alpha), so this
    // check genuinely discriminates the corrected design.
    {
        // normalCadence models the ordinary caller (one read per observation --
        // ContextManager's own pattern); burstCadence models a publish-rate
        // spike (25 reads per observation). Both see the identical data.
        TailRiskEngine normalCadence(500, 0.05);
        TailRiskEngine burstCadence(500, 0.05);
        std::mt19937 rng(7);
        std::uniform_real_distribution<double> uniform(0.0, 1.0);
        // Stop 6 observations after the shift -- ~0.8^6 = 26% of the alpha jump
        // is still un-absorbed by a correctly-smoothed engine, while a
        // getter-smoothed engine (6*26 = 156 EWMA steps) has fully converged to
        // raw. Running to convergence instead would hide the defect: both
        // engines end at the same fixed point.
        for (int i = 0; i < 506; ++i) {
            const double u = uniform(rng);
            // Regime shift at i=500: tail exponent 3.0 -> 1.5 (much fatter) and
            // magnitude 20x. Raw alpha drops sharply across the boundary.
            const double tailExp = (i < 500) ? 3.0 : 1.5;
            const double scale = (i < 500) ? 0.001 : 0.02;
            const double x = (std::pow(1.0 - u, -1.0 / tailExp) - 1.0) * scale;
            normalCadence.AddObservation(x);
            burstCadence.AddObservation(x);
            (void)normalCadence.GetHillAlpha();
            for (int r = 0; r < 25; ++r) (void)burstCadence.GetHillAlpha();
        }
        const double a1 = normalCadence.GetHillAlpha();
        const double a2 = burstCadence.GetHillAlpha();
        std::printf("  cadence independence: normal=%.10f burst=%.10f (delta %.4f%%)\n",
                    a1, a2, 100.0 * std::fabs(a1 - a2) / a1);
        check("smoothed alpha is independent of read cadence (25x reads change nothing)",
              a1 == a2);
        check("cadence-independence fixture actually produced a real estimate (not the 4.0 default)",
              std::fabs(a1 - 4.0) > 1e-9);
    }

    // --- GetHillAlpha() is a pure read: repeated calls are idempotent -------
    {
        TailRiskEngine e(500, 0.05);
        FillPareto(e, 123);
        const double first = e.GetHillAlpha();
        for (int i = 0; i < 100; ++i) (void)e.GetHillAlpha();
        check("100 consecutive GetHillAlpha() calls return the identical value",
              e.GetHillAlpha() == first);
    }

    // --- Cold start / Reset semantics ---------------------------------------
    {
        TailRiskEngine e(500, 0.05);
        check("cold-start alpha is the 4.0 Gaussian-safe default", e.GetHillAlpha() == 4.0);
        for (int i = 0; i < 10; ++i) e.AddObservation(0.001);
        check("below the tail-cutoff warmup gate, alpha stays at the 4.0 default",
              e.GetHillAlpha() == 4.0);

        FillPareto(e, 99);
        check("after warmup, alpha has moved off the default", std::fabs(e.GetHillAlpha() - 4.0) > 1e-9);
        e.Reset();
        check("Reset() restores the 4.0 default", e.GetHillAlpha() == 4.0);
    }

    // --- Finding 10: the k-scan starts at k=10, never at k=2 ----------------
    // Indirect but real: k in [2,9] Hill values are computed from 2-9 order
    // statistics and are wildly noisy. With the default config the scan range
    // is exactly [10, 100]. A degenerate window whose top 9 magnitudes are all
    // identical produces log-sum ~ 0 for every k < 10 (Hill -> the 4.0
    // sentinel), which a k=2-anchored scan would immediately lock onto as a
    // "plateau" at 4.0; a k>=10 scan sees the real tail instead.
    {
        TailRiskEngine e(500, 0.05);
        std::mt19937 rng(2024);
        std::uniform_real_distribution<double> uniform(0.0, 1.0);
        for (int i = 0; i < 491; ++i) {
            const double u = uniform(rng);
            e.AddObservation((std::pow(1.0 - u, -1.0 / 3.0) - 1.0) * 0.001);
        }
        for (int i = 0; i < 9; ++i) e.AddObservation(0.5);  // 9 identical extreme magnitudes
        const double a = e.GetHillAlpha();
        std::printf("  9-identical-extremes window alpha = %.4f\n", a);
        check("a 9-identical-extremes window does not lock the estimate onto the 4.0 "
              "small-k sentinel (k-scan starts at 10)",
              std::fabs(a - 4.0) > 1e-6);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
