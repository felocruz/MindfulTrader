// Unit tests for volatility_dim_stats.h -- the old/new log_scale_ratio and
// current correction_action offline ports used by
// tools/observation_vector/volatility_dim_redundancy_eval.cpp.
//
// Build & run natively (no Sierra Chart deps, header-only):
//   g++ -std=c++17 -I include -I tools/observation_vector tools/observation_vector/test_volatility_dim_stats.cpp -o /tmp/vds_test && /tmp/vds_test

#include "volatility_dim_stats.h"

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
    std::printf("=== volatility_dim_stats ===\n");

    // Not enough history yet -> NaN, for all three.
    {
        std::vector<double> r(10, 0.001);
        check("old: NaN before long_n history available", std::isnan(ComputeLogScaleRatioOld(r, 5, 20)));
        check("new: NaN before long_n history available", std::isnan(ComputeLogScaleRatioNew(r, 5, 20)));
        check("log_scale_expansion_ratio OLD: NaN before lookback_n history available",
              std::isnan(ComputeLogScaleExpansionRatioOld(r, 5, 20)));
        check("log_scale_expansion_ratio NEW: NaN before lookback_n history available",
              std::isnan(ComputeLogScaleExpansionRatioNew(r, 5, 20)));
    }

    // Jump robustness, same property BipowerVariation.h's own test already
    // established for the primitive -- verify it survives being embedded in
    // the full short/long log-ratio formula. Both windows are trailing and
    // include idx itself by construction, so a jump at idx necessarily
    // affects both short and long windows -- there's no way to confine it
    // to "short only". Using a large long_n (2000) dilutes the jump's
    // relative effect on the long window close to zero while still leaving
    // a meaningful relative effect on the short window (500), isolating the
    // comparison this test actually cares about: OLD's short-window
    // variance estimate is far more jump-sensitive than NEW's short-window
    // BV estimate.
    {
        std::vector<double> r(2001, 0.0005);
        for (std::size_t i = 0; i < r.size(); i += 2) r[i] = -r[i];
        std::vector<double> r_jump = r;
        r_jump[2000] = 0.05;  // most recent tick is an extreme outlier

        const int long_n = 2000;
        const double old_calm = ComputeLogScaleRatioOld(r, 2000, long_n);
        const double old_jump = ComputeLogScaleRatioOld(r_jump, 2000, long_n);
        const double new_calm = ComputeLogScaleRatioNew(r, 2000, long_n);
        const double new_jump = ComputeLogScaleRatioNew(r_jump, 2000, long_n);

        std::printf("  [info] old: calm=%.4f jump=%.4f (delta=%.4f)\n", old_calm, old_jump, old_jump - old_calm);
        std::printf("  [info] new: calm=%.4f jump=%.4f (delta=%.4f)\n", new_calm, new_jump, new_jump - new_calm);
        check("single-tick jump moves OLD formula far more than NEW formula",
              std::fabs(old_jump - old_calm) > std::fabs(new_jump - new_calm) * 3.0);
    }

    // log_scale_expansion_ratio OLD: hand-computed reference. lookback_n=4, half=2.
    // returns (most recent last): r[idx-3..idx] = {0.01, -0.01, 0.02, -0.02}.
    // rv_full = 0.0001+0.0001+0.0004+0.0004 = 0.0010, rate = 0.0010/4 = 0.00025.
    // recent (i<2, i.e. idx and idx-1) = {-0.02, 0.02} -> rv_recent=0.0008, rate=0.0004.
    // ratio = log(0.0004/0.00025) = log(1.6).
    {
        std::vector<double> r = {0.01, -0.01, 0.02, -0.02};
        const double expected = std::log(1.6);
        check("log_scale_expansion_ratio OLD hand-computed reference",
              Close(ComputeLogScaleExpansionRatioOld(r, 3, 4), expected, 1e-6));
    }

    // log_scale_expansion_ratio NEW: jump robustness, same isolation
    // technique as log_scale_ratio's own test above (large lookback_n
    // dilutes the jump's effect on the full window, isolating the
    // recent-half window's own jump sensitivity).
    {
        std::vector<double> r(2001, 0.0005);
        for (std::size_t i = 0; i < r.size(); i += 2) r[i] = -r[i];
        std::vector<double> r_jump = r;
        r_jump[2000] = 0.05;

        const int lookback_n = 2000;
        const double old_calm = ComputeLogScaleExpansionRatioOld(r, 2000, lookback_n);
        const double old_jump = ComputeLogScaleExpansionRatioOld(r_jump, 2000, lookback_n);
        const double new_calm = ComputeLogScaleExpansionRatioNew(r, 2000, lookback_n);
        const double new_jump = ComputeLogScaleExpansionRatioNew(r_jump, 2000, lookback_n);
        std::printf("  [info] expansion old: calm=%.4f jump=%.4f (delta=%.4f)\n", old_calm, old_jump, old_jump - old_calm);
        std::printf("  [info] expansion new: calm=%.4f jump=%.4f (delta=%.4f)\n", new_calm, new_jump, new_jump - new_calm);
        check("log_scale_expansion_ratio: single-tick jump moves OLD far more than NEW",
              std::fabs(old_jump - old_calm) > std::fabs(new_jump - new_calm) * 3.0);
    }

    // Pearson correlation: perfectly correlated, perfectly anti-correlated,
    // and a NaN-warmup-entry filter check.
    {
        std::vector<double> a = {1, 2, 3, 4, 5};
        std::vector<double> b = {2, 4, 6, 8, 10};
        std::vector<double> c = {5, 4, 3, 2, 1};
        check("Pearson: perfectly correlated == 1.0", Close(PearsonCorrelation(a, b), 1.0));
        check("Pearson: perfectly anti-correlated == -1.0", Close(PearsonCorrelation(a, c), -1.0));

        std::vector<double> a_nan = {std::nan(""), 2, 3, 4, 5};
        std::vector<double> b_nan = {2, 4, 6, 8, 10};
        check("Pearson: ignores NaN entries", Close(PearsonCorrelation(a_nan, b_nan), 1.0));
    }

    // Symmetric3x3Eigenvalues: diagonal matrix -> eigenvalues are just the
    // diagonal entries, sorted ascending.
    {
        const auto eig = Symmetric3x3Eigenvalues(5.0, 0.0, 0.0, 1.0, 0.0, 3.0);
        check("diagonal matrix eigenvalues sorted ascending",
              Close(eig.eig1, 1.0) && Close(eig.eig2, 3.0) && Close(eig.eig3, 5.0));
    }
    // Identity correlation matrix (no collinearity at all) -> all eigenvalues == 1.
    {
        const auto eig = Symmetric3x3Eigenvalues(1.0, 0.0, 0.0, 1.0, 0.0, 1.0);
        check("identity matrix: all eigenvalues == 1 (condition number 1, no collinearity)",
              Close(eig.eig1, 1.0) && Close(eig.eig2, 1.0) && Close(eig.eig3, 1.0));
    }
    // Near-singular correlation matrix: two variables correlated at 0.99,
    // third independent -> smallest eigenvalue should be small (~0.01-ish
    // scale), condition number large.
    {
        const auto eig = Symmetric3x3Eigenvalues(1.0, 0.99, 0.0, 1.0, 0.0, 1.0);
        const double condition_number = eig.eig3 / eig.eig1;
        std::printf("  [info] near-collinear (rho=0.99) condition number: %.2f\n", condition_number);
        check("near-collinear pair produces a large condition number (>50)", condition_number > 50.0);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
