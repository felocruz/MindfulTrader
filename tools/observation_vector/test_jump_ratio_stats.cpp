// tools/observation_vector/test_jump_ratio_stats.cpp
// Build & run: mamba run -n mts g++ -std=c++17 tools/observation_vector/test_jump_ratio_stats.cpp \
//   -o /tmp/jump_ratio_stats_test && /tmp/jump_ratio_stats_test
#include "jump_ratio_stats.h"
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
        // Reference values verified via a real mamba run -n mts python3 execution
        // (not hand arithmetic -- an initial hand computation was wrong and caught
        // before being embedded here): returns = [0.01, 0.01, 0.5, 0.01], window=4:
        //   RV = 0.01^2 + 0.01^2 + 0.5^2 + 0.01^2 = 0.25029999999999997
        //   BV = (pi/2) * (|0.01*0.01| + |0.01*0.5| + |0.5*0.01|) = 0.015865042900628457
        //   jump_ratio = max(0, (RV-BV)/RV) = 0.9366158893302898
        std::vector<double> returns = {0.01, 0.01, 0.5, 0.01};
        auto jr = ComputeJumpRatio(returns, 4);
        check("jump ratio has same length as input", jr.size() == 4);
        check("first 3 entries are NaN (warmup, window=4)",
              std::isnan(jr[0]) && std::isnan(jr[1]) && std::isnan(jr[2]));
        check("jr[3] matches Python reference (0.9366158893302898)",
              close(jr[3], 0.9366158893302898, 1e-9));
    }
    {
        // Pure diffusion (no jumps): all returns similar magnitude -> BV should be
        // close to RV -> jump_ratio close to 0 (or floored at 0 if BV slightly > RV
        // due to finite-sample noise, matching the max(0, ...) floor's whole purpose).
        std::vector<double> returns = {0.02, 0.02, 0.02, 0.02};
        auto jr = ComputeJumpRatio(returns, 4);
        check("pure-diffusion-like series has near-zero (or floored-zero) jump ratio",
              jr[3] >= 0.0 && jr[3] < 0.1);
    }
    {
        // RV == 0 (all-zero returns) -> undefined ratio -> NaN, not a divide-by-zero crash.
        std::vector<double> zero_returns = {0.0, 0.0, 0.0, 0.0};
        auto jr = ComputeJumpRatio(zero_returns, 4);
        check("all-zero returns yield NaN (RV=0, undefined ratio), not a crash",
              std::isnan(jr[3]));
    }
    {
        check("window larger than input yields all-NaN",
              std::isnan(ComputeJumpRatio({0.01, 0.02}, 5)[0]));
    }
    {
        // Sliding window sanity: a second window position over the same series
        // shifts by one element -- verify the incremental slide logic against
        // two independently-computed windows. returns = [0.1, 0.2, 0.05],
        // window=2. Reference values from a real mamba run -n mts python3
        // execution (not hand arithmetic -- an initial hand computation here
        // was off in the 7th decimal place, caught before being embedded):
        //   i=1: RV=0.05000000000000001 BV=0.031415926535897934 jump_ratio=0.37168146928204143
        //   i=2: RV=0.04250000000000001 BV=0.015707963267948967 jump_ratio=0.6304008642835538
        std::vector<double> returns = {0.1, 0.2, 0.05};
        auto jr = ComputeJumpRatio(returns, 2);
        check("sliding window i=1 matches Python reference", close(jr[1], 0.37168146928204143, 1e-9));
        check("sliding window i=2 matches Python reference", close(jr[2], 0.6304008642835538, 1e-9));
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
