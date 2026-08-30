// tools/test_market_test_stats_magnitude.cpp
// Build & run: mamba run -n mts g++ -std=c++17 tools/test_market_test_stats_magnitude.cpp \
//   -o /tmp/market_test_magnitude_test && /tmp/market_test_magnitude_test
#include "market_test_stats.h"
#include <cmath>
#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
bool close(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }
}  // namespace

int main() {
    {
        // Zero-variance groups: every bootstrap resample of a constant-valued
        // array is that same constant, so the bootstrap distribution collapses
        // to a single point -- this is RNG-independent by construction, unlike
        // a general bootstrap CI, and matches a real mamba run -n mts python3
        // execution of the actual Python bootstrap_mean_gap_ci() exactly:
        // top=[10.0]*50, bottom=[1.0]*50 -> gap=lo=hi=9.0.
        std::vector<double> top(50, 10.0);
        std::vector<double> bottom(50, 1.0);
        auto result = ComputeBootstrapMeanGapCI(top, bottom);
        check("zero-variance gap matches Python reference exactly", close(result.gap, 9.0));
        check("zero-variance CI collapses to a point (lo == hi == gap)",
              close(result.ci_lo, 9.0) && close(result.ci_hi, 9.0));
    }
    {
        // Point estimate (not the CI, which is stochastic) must exactly match
        // mean(|top|) - mean(|bottom|) -- this part of the formula has no
        // randomness at all, matches tools/dim_acceptance_eval.py's own
        // bootstrap_mean_gap_ci() line: `float(np.mean(np.abs(top)) - np.mean(np.abs(bottom)))`.
        std::vector<double> top = {3.0, -5.0, 4.0};   // mean |x| = (3+5+4)/3 = 4.0
        std::vector<double> bottom = {1.0, -1.0, 2.0}; // mean |x| = (1+1+2)/3 = 4.0/3
        auto result = ComputeBootstrapMeanGapCI(top, bottom);
        check("point estimate is exactly mean(|top|) - mean(|bottom|)",
              close(result.gap, 4.0 - (4.0 / 3.0), 1e-9));
        check("CI bounds are finite and ordered (lo <= hi)",
              std::isfinite(result.ci_lo) && std::isfinite(result.ci_hi) && result.ci_lo <= result.ci_hi);
    }
    {
        // n_boot == 0: no resamples means no bootstrap distribution to read a
        // percentile from -- must return NaN CI bounds, not index an empty
        // gaps vector out of bounds (a real latent bug, found in review,
        // fixed directly: gaps.size() - 1 on an empty vector wraps
        // std::size_t to SIZE_MAX). The point estimate still has no
        // randomness in it, so it must remain exact even with zero resamples.
        std::vector<double> top = {3.0, -5.0, 4.0};
        std::vector<double> bottom = {1.0, -1.0, 2.0};
        auto result = ComputeBootstrapMeanGapCI(top, bottom, /*n_boot=*/0);
        check("n_boot=0 point estimate is still exact", close(result.gap, 4.0 - (4.0 / 3.0), 1e-9));
        check("n_boot=0 CI bounds are NaN, not garbage from an empty-vector index",
              std::isnan(result.ci_lo) && std::isnan(result.ci_hi));
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
