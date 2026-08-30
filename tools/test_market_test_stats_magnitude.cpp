// tools/test_market_test_stats_magnitude.cpp
// Build & run: mamba run -n mts g++ -std=c++17 tools/test_market_test_stats_magnitude.cpp \
//   -o /tmp/market_test_magnitude_test && /tmp/market_test_magnitude_test
#include "market_test_stats.h"
#include <cmath>
#include <cstdio>
#include <random>

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
    {
        // Regression coverage for the >=20,000-element weighted-bootstrap
        // path (Praestgaard & Wellner 1993 exchangeable bootstrap) -- until
        // now this path was exercised ONLY by an expensive real 38.5M-row
        // production run, meaning a future weight-distribution regression
        // (like the Uniform[0,2] variance-1/3 bug this exact path once had)
        // would only be caught by another such run, or not at all. Analytic
        // reference: for X ~ N(0,1), Var(|X|) = 1 - 2/pi (verified via a
        // real mamba run -n mts python3 execution: 0.3633802276324186), so
        // for top/bottom i.i.d. |N(0,1)| samples of size n each, the 95% CI
        // half-width for mean(|top|)-mean(|bottom|) is
        // 1.96*sqrt(2*(1-2/pi)/n) -- verified at n=25,000: 0.021135460...
        // This is exactly the check that would have caught the Uniform[0,2]
        // regression: that variant measured ~1.7-1.8x narrower than this
        // theory value on the same kind of data.
        std::mt19937_64 gen(123);
        std::normal_distribution<double> nd(0.0, 1.0);
        const std::size_t n = 25000;  // above kExactResampleThreshold=20,000
        std::vector<double> top(n), bottom(n);
        for (auto& x : top) x = nd(gen);
        for (auto& x : bottom) x = nd(gen);
        auto result = ComputeBootstrapMeanGapCI(top, bottom, 2000, 0);
        constexpr double kPi = 3.14159265358979323846;
        const double theory_width = 1.96 * 2.0 * std::sqrt(2.0 * (1.0 - 2.0 / kPi) / static_cast<double>(n));
        const double actual_width = result.ci_hi - result.ci_lo;
        const double ratio = actual_width / theory_width;
        check("weighted-bootstrap-path (n=25000) CI width matches analytic theory within 15%",
              ratio > 0.85 && ratio < 1.15);
        check("weighted-bootstrap-path point estimate is near zero (both groups drawn from the same distribution)",
              std::fabs(result.gap) < 0.05);
    }
    {
        // Weighted-path zero-variance collapse (constant arrays, n above the
        // threshold) -- confirms dividing by the realized sum(w_i) rather
        // than n still collapses exactly for a constant array on this path,
        // not just on the small-n exact-resample path already covered above.
        std::vector<double> top(25000, 10.0);
        std::vector<double> bottom(25000, 1.0);
        auto result = ComputeBootstrapMeanGapCI(top, bottom, 500, 0);
        check("weighted-path (n=25000) zero-variance collapses exactly",
              close(result.gap, 9.0) && close(result.ci_lo, 9.0) && close(result.ci_hi, 9.0));
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
