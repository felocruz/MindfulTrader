// tools/test_market_test_stats_median.cpp
// Build & run: mamba run -n mts g++ -std=c++17 tools/test_market_test_stats_median.cpp \
//   -o /tmp/market_test_median_test && /tmp/market_test_median_test
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
        // Empty input must not read out of bounds (a real segfault, found in
        // review: the even-length branch computed v[mid-1] with mid=0 -> read
        // v[SIZE_MAX]). Unreachable in jump_ratio_eval.cpp's real callers
        // (pre-filtered by a >=30-sample floor), but this is shared, reusable
        // infrastructure.
        check("MedianAbs({}) returns 0.0 instead of reading out of bounds",
              close(MedianAbs(std::vector<double>{}), 0.0));
    }
    {
        // Point estimate (not the CI, which is stochastic) must exactly match
        // median(|top|) - median(|bottom|) -- no randomness at all. Verified
        // via a real mamba run -n mts python3 execution (np.median(np.abs(.))):
        // top={3,-5,8} (odd count) -> median|top|=5.0;
        // bottom={1,-1,2,10} (even count) -> median|bottom|=1.5; gap=3.5.
        std::vector<double> top = {3.0, -5.0, 8.0};
        std::vector<double> bottom = {1.0, -1.0, 2.0, 10.0};
        auto result = ComputeBootstrapMedianGapCI(top, bottom);
        check("point estimate is exactly median(|top|) - median(|bottom|), odd/even counts",
              close(result.gap, 3.5, 1e-9));
        check("CI bounds are finite and ordered (lo <= hi)",
              std::isfinite(result.ci_lo) && std::isfinite(result.ci_hi) && result.ci_lo <= result.ci_hi);
    }
    {
        // Zero-variance groups (exact-resample path, n below threshold):
        // median of a constant-valued array's resample is always that
        // constant, regardless of which indices get drawn.
        std::vector<double> top(50, 10.0);
        std::vector<double> bottom(50, 1.0);
        auto result = ComputeBootstrapMedianGapCI(top, bottom);
        check("zero-variance gap matches exactly (exact-resample path)", close(result.gap, 9.0));
        check("zero-variance CI collapses to a point (exact-resample path)",
              close(result.ci_lo, 9.0) && close(result.ci_hi, 9.0));
    }
    {
        // n_boot == 0: point estimate has no randomness and must remain
        // exact; CI bounds must be NaN, not a garbage read (mirrors the
        // n_boot=0 guard already proven necessary for ComputeBootstrapMeanGapCI).
        std::vector<double> top = {3.0, -5.0, 8.0};
        std::vector<double> bottom = {1.0, -1.0, 2.0, 10.0};
        auto result = ComputeBootstrapMedianGapCI(top, bottom, /*n_boot=*/0);
        check("n_boot=0 point estimate is still exact", close(result.gap, 3.5, 1e-9));
        check("n_boot=0 CI bounds are NaN", std::isnan(result.ci_lo) && std::isnan(result.ci_hi));
    }
    {
        // Regression coverage for the >=20,000-element weighted-median path
        // (Praestgaard & Wellner 1993's general exchangeable-bootstrap theory
        // covers M-estimators, including the median, not only the mean).
        // Analytic reference: for X ~ N(0,1), the median of |X| is
        // Phi^-1(0.75) = 0.6744897501960817, with density 2*phi(median) =
        // 0.635553145368214 -- both verified via a real mamba run -n mts
        // python3 (scipy.stats.norm) execution, not hand arithmetic. The
        // asymptotic variance of a sample median is 1/(4*n*f(m)^2); for the
        // GAP of two independent same-size medians, Var(gap) = 2x that.
        // 95% CI width at n=25,000, verified: 0.02758348860572742.
        std::mt19937_64 gen(123);
        std::normal_distribution<double> nd(0.0, 1.0);
        const std::size_t n = 25000;  // above kExactResampleThreshold=20,000
        std::vector<double> top(n), bottom(n);
        for (auto& x : top) x = nd(gen);
        for (auto& x : bottom) x = nd(gen);
        auto result = ComputeBootstrapMedianGapCI(top, bottom, 2000, 0);
        constexpr double kTheoryWidth = 0.02758348860572742;
        const double actual_width = result.ci_hi - result.ci_lo;
        const double ratio = actual_width / kTheoryWidth;
        check("weighted-median-path (n=25000) CI width matches analytic theory within 20%",
              ratio > 0.80 && ratio < 1.20);
        check("weighted-median-path point estimate is near zero (both groups drawn from the same distribution)",
              std::fabs(result.gap) < 0.05);
    }
    {
        // Weighted-median-path zero-variance collapse (constant arrays, n
        // above the threshold): a weighted median of a constant array is
        // that constant for ANY nonzero weight configuration, since every
        // sorted value is identical -- the cumulative-weight crossing point
        // always lands on the same value.
        std::vector<double> top(25000, 10.0);
        std::vector<double> bottom(25000, 1.0);
        auto result = ComputeBootstrapMedianGapCI(top, bottom, 500, 0);
        check("weighted-median-path (n=25000) zero-variance collapses exactly",
              close(result.gap, 9.0) && close(result.ci_lo, 9.0) && close(result.ci_hi, 9.0));
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
