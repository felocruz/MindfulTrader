// tools/observation_vector/test_drift_location_stats.cpp
// Build & run: mamba run -n mts g++ -std=c++17 tools/observation_vector/test_drift_location_stats.cpp \
//   -o /tmp/drift_location_stats_test && /tmp/drift_location_stats_test
#include "drift_location_stats.h"
#include <cmath>
#include <cstdio>
#include <deque>

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
        // Reference values computed directly from tools/observation_vector/dim_acceptance_eval.py's
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
    {
        // variance_inflation default (1.0) must reproduce the existing
        // 70/100 fixture's exact values -- proves the new parameter is
        // additive, not a behavior change, when unset.
        std::vector<double> forward_returns(100, 1.0);
        std::vector<double> candidate_values(100, 1.0);
        for (int i = 0; i < 30; ++i) candidate_values[i] = -1.0;
        auto baseline = ComputeHitRate(forward_returns, candidate_values);
        auto explicit_one = ComputeHitRate(forward_returns, candidate_values, /*variance_inflation=*/1.0);
        check("variance_inflation=1.0 matches the no-argument default exactly",
              close(explicit_one.z_stat, baseline.z_stat, 1e-12) &&
              close(explicit_one.p_value, baseline.p_value, 1e-12) &&
              close(explicit_one.ci_lo, baseline.ci_lo, 1e-12) &&
              close(explicit_one.ci_hi, baseline.ci_hi, 1e-12));
    }
    {
        // Effective-sample-size substitution, verified against a real
        // mamba run -n mts python3 computation (not hand arithmetic):
        // n=10000, k=5200 (hit_rate=0.52), variance_inflation=4.0 ->
        // n_eff=2500, k_eff=1300.
        //   baseline (DEFF=1.0): z=4.000000000000004 p=6.334248366623996e-05
        //     ci=(0.510202040212211, 0.529782599288679)
        //   DEFF=4.0: z=2.000000000000002 p=4.550026389635820e-02
        //     ci=(0.500400006272198, 0.539538622433388)
        // z_stat scales by EXACTLY 1/sqrt(DEFF) (pure algebraic
        // substitution into se_null). The Wilson CI width does NOT scale
        // by exactly sqrt(DEFF) -- Wilson's own small-sample nonlinear
        // correction terms mean the ratio is only asymptotically exact,
        // converging to it as n_eff grows (verified: 1.9147 at n_eff=25,
        // 1.9991 at n_eff=2500 -- checked via real Python before choosing
        // n=10000 for this test specifically so the approximation is tight).
        std::vector<double> forward_returns(10000, 1.0);
        std::vector<double> candidate_values(10000, 1.0);
        for (int i = 0; i < 4800; ++i) candidate_values[i] = -1.0;  // 5200 hits / 10000
        auto baseline = ComputeHitRate(forward_returns, candidate_values);
        auto corrected = ComputeHitRate(forward_returns, candidate_values, /*variance_inflation=*/4.0);
        check("n/k still report raw observed counts under correction",
              corrected.n == 10000 && corrected.k == 5200);
        check("hit_rate is unaffected by variance_inflation",
              close(corrected.hit_rate, baseline.hit_rate, 1e-12));
        check("z_stat matches Python reference under DEFF=4.0",
              close(corrected.z_stat, 2.000000000000002, 1e-9));
        check("z_stat scales by exactly 1/sqrt(DEFF)",
              close(baseline.z_stat / corrected.z_stat, 2.0, 1e-9));
        check("p_value matches Python reference under DEFF=4.0",
              close(corrected.p_value, 4.550026389635820e-02, 1e-9));
        check("Wilson CI matches Python reference under DEFF=4.0",
              close(corrected.ci_lo, 0.500400006272198, 1e-9) &&
              close(corrected.ci_hi, 0.539538622433388, 1e-9));
        const double baseline_width = baseline.ci_hi - baseline.ci_lo;
        const double corrected_width = corrected.ci_hi - corrected.ci_lo;
        check("CI width scales by approximately sqrt(DEFF) at this n (within 1%)",
              corrected_width / baseline_width > 1.98 && corrected_width / baseline_width < 2.02);
    }
    {
        // CI/p-value consistency: both must tell the same significance
        // story under a nontrivial correction (both are derived from the
        // same n_eff/k_eff substitution, so this holds by construction --
        // this test guards against a future edit breaking that shared
        // derivation, e.g. if someone "optimizes" one path without the other).
        std::vector<double> forward_returns(10000, 1.0);
        std::vector<double> candidate_values(10000, 1.0);
        for (int i = 0; i < 4800; ++i) candidate_values[i] = -1.0;
        auto corrected = ComputeHitRate(forward_returns, candidate_values, /*variance_inflation=*/4.0);
        const bool ci_significant = corrected.ci_lo > 0.5 || corrected.ci_hi < 0.5;
        const bool p_significant = corrected.p_value < 0.05;
        check("CI and p-value agree on significance under a nontrivial correction",
              ci_significant == p_significant);
    }
    {
        // StreamingHitRateAccumulator equivalence: a synthetic tick series
        // (irregular spacing, one deliberate large gap to exercise the
        // 3x-horizon staleness reject, long enough that the tail signals
        // near the end never get resolved before the stream ends) is fed
        // through BOTH the batch path (ComputeLogReturns+ComputeDriftZScore+
        // ComputeForwardReturns+ComputeHitRate, exactly mirroring
        // drift_location_eval.cpp's own driver loop) and the streaming
        // accumulator, per horizon. Both must agree exactly -- this is a
        // self-consistency check (both implementations must produce the
        // same answer on the same input), not a hand-verified fixture.
        constexpr std::size_t kWindow = 50;
        constexpr std::size_t kN = 3000;
        constexpr std::size_t kGapAtIndex = 1500;  // one large jump to trigger a stale-gap reject
        std::vector<std::int64_t> ts(kN);
        std::vector<double> price(kN);
        std::int64_t t = 1'000'000;
        for (std::size_t i = 0; i < kN; ++i) {
            t += (i == kGapAtIndex) ? 100'000'000LL : 100'000LL;  // ~100ms spacing, one 100s gap
            ts[i] = t;
            price[i] = 100.0 + 0.01 * std::sin(static_cast<double>(i) * 0.1) + 0.0001 * static_cast<double>(i);
        }
        const std::vector<int> horizons = {1, 2};  // minutes

        // -- Batch path (mirrors drift_location_eval.cpp's driver exactly) --
        const auto log_returns = ComputeLogReturns(price);
        const auto z = ComputeDriftZScore(log_returns, kWindow);
        std::vector<std::int64_t> signal_ts;
        std::vector<double> signal_price, signal_z;
        for (std::size_t i = 0; i < z.size(); ++i) {
            if (std::isnan(z[i])) continue;
            signal_ts.push_back(ts[i + 1]);
            signal_price.push_back(price[i + 1]);
            signal_z.push_back(z[i]);
        }
        std::vector<HitRateResult> batch_results;
        for (int h : horizons) {
            const auto fwd = ComputeForwardReturns(signal_ts, signal_price, ts, price, h);
            batch_results.push_back(ComputeHitRate(fwd, signal_z));
        }

        // -- Streaming path --
        StreamingHitRateAccumulator acc(horizons);
        double prev_price = 0.0;
        bool has_prev = false;
        std::deque<double> ret_window;
        double sum = 0.0, sum_sq = 0.0;
        std::size_t returns_seen = 0;
        for (std::size_t i = 0; i < kN; ++i) {
            acc.Advance(ts[i], price[i]);
            if (has_prev) {
                const double log_ret = std::log(price[i] / prev_price);
                ret_window.push_back(log_ret);
                sum += log_ret;
                sum_sq += log_ret * log_ret;
                ++returns_seen;
                if (ret_window.size() > kWindow) {
                    const double oldest = ret_window.front();
                    ret_window.pop_front();
                    sum -= oldest;
                    sum_sq -= oldest * oldest;
                }
                if (returns_seen >= kWindow) {
                    const double mean = sum / static_cast<double>(kWindow);
                    const double variance = sum_sq / static_cast<double>(kWindow) - mean * mean;
                    const double stddev = std::sqrt(std::max(0.0, variance));
                    const double zval = (stddev > 0.0) ? (mean / stddev) : 0.0;
                    acc.PushSignal(ts[i], price[i], zval);
                }
            }
            prev_price = price[i];
            has_prev = true;
        }

        bool any_unresolved_at_end = false;  // sanity: the test data should actually exercise this edge case
        for (std::size_t h = 0; h < horizons.size(); ++h) {
            const auto streaming_result = acc.Result(h);
            const auto& batch_result = batch_results[h];
            char label[128];
            std::snprintf(label, sizeof(label), "streaming vs batch agree exactly (horizon=%dmin): n", horizons[h]);
            check(label, streaming_result.n == batch_result.n);
            std::snprintf(label, sizeof(label), "streaming vs batch agree exactly (horizon=%dmin): k", horizons[h]);
            check(label, streaming_result.k == batch_result.k);
            std::snprintf(label, sizeof(label), "streaming vs batch agree exactly (horizon=%dmin): hit_rate/ci/p", horizons[h]);
            check(label, close(streaming_result.hit_rate, batch_result.hit_rate) &&
                             close(streaming_result.ci_lo, batch_result.ci_lo) &&
                             close(streaming_result.ci_hi, batch_result.ci_hi) &&
                             close(streaming_result.p_value, batch_result.p_value));
            if (streaming_result.n < signal_ts.size()) any_unresolved_at_end = true;
        }
        check("test data actually exercises the never-resolved-at-end edge case",
              any_unresolved_at_end);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
