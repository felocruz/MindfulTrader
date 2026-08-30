// tools/test_drift_location_stats.cpp
// Build & run: mamba run -n mts g++ -std=c++17 tools/test_drift_location_stats.cpp \
//   -o /tmp/drift_location_stats_test && /tmp/drift_location_stats_test
#include "drift_location_stats.h"
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
        // prices = [1, 2, 4] -> log returns = [log(2), log(2)] (doubling each step)
        std::vector<double> prices = {1.0, 2.0, 4.0};
        auto returns = ComputeLogReturns(prices);
        check("2 returns from 3 prices", returns.size() == 2);
        check("both returns equal log(2)", close(returns[0], std::log(2.0)) && close(returns[1], std::log(2.0)));
    }
    {
        check("empty vector for <2 prices", ComputeLogReturns({1.0}).empty());
    }
    {
        // Hand-computed rolling z-score, window=2, over returns = [1, 3, 2, 4]:
        //   i=1: window=[1,3] mean=2 var=(1+9)/2-4=1 std=1 z=2.0
        //   i=2: window=[3,2] mean=2.5 var=(9+4)/2-6.25=0.25 std=0.5 z=5.0
        //   i=3: window=[2,4] mean=3 var=(4+16)/2-9=1 std=1 z=3.0
        std::vector<double> returns = {1.0, 3.0, 2.0, 4.0};
        auto z = ComputeDriftZScore(returns, 2);
        check("z has same length as input", z.size() == 4);
        check("z[0] is NaN (warmup)", std::isnan(z[0]));
        check("z[1] == 2.0", close(z[1], 2.0));
        check("z[2] == 5.0", close(z[2], 5.0));
        check("z[3] == 3.0", close(z[3], 3.0));
    }
    {
        // Constant returns -> zero variance -> z defined as 0.0, not NaN/Inf.
        std::vector<double> constant_returns = {0.5, 0.5, 0.5};
        auto z = ComputeDriftZScore(constant_returns, 2);
        check("zero-variance window yields z=0.0, not NaN/Inf", close(z[1], 0.0) && close(z[2], 0.0));
    }
    {
        check("window larger than input yields all-NaN",
              std::isnan(ComputeDriftZScore({1.0, 2.0}, 5)[0]));
    }
    {
        // timestamps in microseconds: t=0, t=60s, t=120s; prices 100, 110, 121.
        // Signal at t=0, price=100, horizon=1 minute (60s) -> target_ts=60_000_000,
        // exact match at index 1 -> fwd = log(110/100).
        std::vector<std::int64_t> timestamps = {0, 60'000'000, 120'000'000};
        std::vector<double> prices = {100.0, 110.0, 121.0};
        auto fwd = ComputeForwardReturns({0}, {100.0}, timestamps, prices, /*horizon_minutes=*/1);
        check("forward return at exact horizon match", close(fwd[0], std::log(110.0 / 100.0)));

        // Horizon far beyond available data -> no valid target -> NaN.
        auto fwd_oob = ComputeForwardReturns({0}, {100.0}, timestamps, prices, /*horizon_minutes=*/1000);
        check("forward return out of bounds is NaN", std::isnan(fwd_oob[0]));

        // Gap guard: signal at t=0 but next available timestamp is way beyond
        // 3x the horizon -- matches dim_acceptance_eval.py's own gap_ok rule.
        std::vector<std::int64_t> gappy_ts = {0, 10'000'000'000};  // huge gap
        std::vector<double> gappy_px = {100.0, 999.0};
        auto fwd_gap = ComputeForwardReturns({0}, {100.0}, gappy_ts, gappy_px, /*horizon_minutes=*/1);
        check("forward return beyond 3x horizon gap is NaN (guard)", std::isnan(fwd_gap[0]));
    }
    {
        // Reference values computed directly from tools/dim_acceptance_eval.py's
        // own wilson_ci() (verified via a real mamba run -n mts python3 invocation,
        // not hand-arithmetic): wilson_ci(50, 100) = (0.40382982859014716, 0.5961701714098528)
        auto ci = ComputeWilsonCI(50, 100);
        check("WilsonCI(50,100) lo matches Python reference", close(ci.lo, 0.40382982859014716, 1e-9));
        check("WilsonCI(50,100) hi matches Python reference", close(ci.hi, 0.5961701714098528, 1e-9));

        // wilson_ci(70, 100) = (0.6041496156718804, 0.7810524613377188)
        auto ci2 = ComputeWilsonCI(70, 100);
        check("WilsonCI(70,100) lo matches Python reference", close(ci2.lo, 0.6041496156718804, 1e-9));
        check("WilsonCI(70,100) hi matches Python reference", close(ci2.hi, 0.7810524613377188, 1e-9));
    }
    {
        check("Sign(5.0) == 1", Sign(5.0) == 1);
        check("Sign(-5.0) == -1", Sign(-5.0) == -1);
        check("Sign(0.0) == 0", Sign(0.0) == 0);
    }
    {
        // 70 hits out of 100, all candidate values nonzero and finite.
        std::vector<double> forward_returns(100, 1.0);   // all positive forward returns
        std::vector<double> candidate_values(100, 1.0);  // all positive candidate -> hit
        for (int i = 0; i < 30; ++i) candidate_values[i] = -1.0;  // 30 misses (opposite sign)
        auto result = ComputeHitRate(forward_returns, candidate_values);
        check("n == 100", result.n == 100);
        check("k == 70", result.k == 70);
        check("hit_rate == 0.70", close(result.hit_rate, 0.70));
        // Reference values from a real mamba run -n mts python3 computation:
        // hit_rate=0.70, n=100 -> z=3.999999999999999, p=6.334248366623996e-05
        check("z_stat matches Python reference", close(result.z_stat, 3.999999999999999, 1e-6));
        check("p_value matches Python reference", close(result.p_value, 6.334248366623996e-05, 1e-9));
    }
    {
        // Zero candidate values are excluded from n (matches dim_acceptance_eval.py's
        // `mask = np.isfinite(fwd) & (cand_val != 0)`).
        std::vector<double> forward_returns = {1.0, 1.0, 1.0};
        std::vector<double> candidate_values = {1.0, 0.0, 1.0};
        auto result = ComputeHitRate(forward_returns, candidate_values);
        check("zero candidate values excluded from n", result.n == 2);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
