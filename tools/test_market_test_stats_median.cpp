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
        // exact; CI bounds must be NaN, not a garbage read (the guard
        // this reference itself needs -- gaps.size()-1 would wrap on an
        // empty vector without it).
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
    {
        // variance_inflation default (1.0) must reproduce the exact same
        // (gap, ci_lo, ci_hi) as calling with the parameter omitted --
        // proves the new parameter is additive, not a behavior change.
        std::vector<double> top = {3.0, -5.0, 8.0};
        std::vector<double> bottom = {1.0, -1.0, 2.0, 10.0};
        auto baseline = ComputeBootstrapMedianGapCI(top, bottom, 1000, 42);
        auto explicit_one = ComputeBootstrapMedianGapCI(top, bottom, 1000, 42, /*variance_inflation=*/1.0);
        check("variance_inflation=1.0 matches the 4-argument call exactly",
              close(explicit_one.gap, baseline.gap, 1e-12) &&
              close(explicit_one.ci_lo, baseline.ci_lo, 1e-12) &&
              close(explicit_one.ci_hi, baseline.ci_hi, 1e-12));
    }
    {
        // Known-factor widening: same seed -> same underlying bootstrap
        // distribution, so the widened CI must be an EXACT deterministic
        // function of the unwidened one: new_ci_lo = gap - (gap-ci_lo)*scale,
        // new_ci_hi = gap + (ci_hi-gap)*scale, scale = sqrt(variance_inflation).
        // No external reference values needed -- this is a self-consistency
        // property of the post-hoc widening transform, not a new statistical
        // claim, so it's verified against the function's own unwidened output
        // rather than a hand- or Python-derived number.
        std::vector<double> top(50000);
        std::vector<double> bottom(50000);
        std::mt19937_64 gen(7);
        std::normal_distribution<double> nd(0.0, 1.0);
        for (auto& x : top) x = nd(gen);
        for (auto& x : bottom) x = nd(gen) + 0.02;
        auto baseline = ComputeBootstrapMedianGapCI(top, bottom, 1000, 99);
        auto widened = ComputeBootstrapMedianGapCI(top, bottom, 1000, 99, /*variance_inflation=*/4.0);
        check("gap is unaffected by variance_inflation",
              close(widened.gap, baseline.gap, 1e-12));
        const double scale = 2.0;  // sqrt(4.0)
        const double expected_lo = baseline.gap - (baseline.gap - baseline.ci_lo) * scale;
        const double expected_hi = baseline.gap + (baseline.ci_hi - baseline.gap) * scale;
        check("ci_lo widens by exactly sqrt(variance_inflation) around gap",
              close(widened.ci_lo, expected_lo, 1e-9));
        check("ci_hi widens by exactly sqrt(variance_inflation) around gap",
              close(widened.ci_hi, expected_hi, 1e-9));
        check("widened CI is strictly wider than baseline",
              (widened.ci_hi - widened.ci_lo) > (baseline.ci_hi - baseline.ci_lo));
    }
    {
        // n_boot=0 still returns NaN CI bounds regardless of
        // variance_inflation (NaN * anything is NaN, gap stays exact) --
        // confirms the widening transform doesn't crash or silently
        // produce a finite value out of the existing NaN-CI guard.
        std::vector<double> top = {3.0, -5.0, 8.0};
        std::vector<double> bottom = {1.0, -1.0, 2.0, 10.0};
        auto result = ComputeBootstrapMedianGapCI(top, bottom, /*n_boot=*/0, /*seed=*/0, /*variance_inflation=*/4.0);
        check("n_boot=0 point estimate is still exact under variance_inflation",
              close(result.gap, 3.5, 1e-9));
        check("n_boot=0 CI bounds are still NaN under variance_inflation",
              std::isnan(result.ci_lo) && std::isnan(result.ci_hi));
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
