// test_triple_barrier_parity.cpp — C++ half of the golden-vector parity proof.
//
// Asserts tbe::ComputeBarriers() reproduces every case in the shared contract
// fixture: tests/fixtures/triple_barrier_golden_vectors.json. The 7 cases are
// embedded here (values copied from that JSON, which remains the canonical
// cross-repo source) — mirroring how the Python side embeds the same numbers in
// tests/test_triple_barrier_scanner.py. This is the C++ counterpart of that
// suite; both green == the barrier definition is provably consistent (§5.2).
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I include tests/cpp/test_triple_barrier_parity.cpp -o /tmp/tbe_test && /tmp/tbe_test

#include "TripleBarrierEngine.h"

#include <cmath>
#include <cstdio>

using namespace tbe;

namespace {

int g_failures = 0;

bool approx(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

struct Expected {
    double seed_stop;
    double seed_target;
    double stop;
    double target;
    int    max_bars;
    double risk;
    double reward_at_target;
    bool   structural_stop_bound;
    bool   structural_target_bound;
};

void check(const char* name, const BarrierInputs& in, const Expected& e) {
    const Barriers b = ComputeBarriers(in);
    bool ok = approx(b.seed_stop, e.seed_stop)
           && approx(b.seed_target, e.seed_target)
           && approx(b.stop, e.stop)
           && approx(b.target, e.target)
           && b.max_bars == e.max_bars
           && approx(b.risk, e.risk)
           && approx(b.reward_at_target, e.reward_at_target)
           && b.structural_stop_bound == e.structural_stop_bound
           && b.structural_target_bound == e.structural_target_bound;

    if (ok) {
        std::printf("  PASS  %s\n", name);
    } else {
        ++g_failures;
        std::printf("  FAIL  %s\n", name);
        std::printf("        stop    got %.6f  exp %.6f\n", b.stop, e.stop);
        std::printf("        target  got %.6f  exp %.6f\n", b.target, e.target);
        std::printf("        maxbars got %d      exp %d\n", b.max_bars, e.max_bars);
        std::printf("        risk    got %.6f  exp %.6f\n", b.risk, e.risk);
        std::printf("        reward  got %.6f  exp %.6f\n", b.reward_at_target, e.reward_at_target);
        std::printf("        s_bound got %d      exp %d\n", b.structural_stop_bound, e.structural_stop_bound);
        std::printf("        t_bound got %d      exp %d\n", b.structural_target_bound, e.structural_target_bound);
    }
}

}  // namespace

int main() {
    std::printf("Triple-Barrier golden-vector parity (C++ half)\n");

    // Case 1 — turtle_soup_buy_caps_nonbinding
    check("turtle_soup_buy_caps_nonbinding",
          BarrierInputs{ 3, true, 5000.00, 5000.50, 4999.00, 0.0, 0.0,
                         5.00, 1.0, 1.0, 5008.00, 0.0, 5015.00, 4990.00,
                         Regime::GAUSSIAN_STABLE, 0.25 },
          Expected{ 4996.50, 5008.00, 4996.50, 5008.00, 25, 3.50, 8.00, false, false });

    // Case 2 — turtle_soup_buy_caps_binding
    check("turtle_soup_buy_caps_binding",
          BarrierInputs{ 3, true, 5000.00, 5000.50, 4999.00, 0.0, 0.0,
                         5.00, 1.0, 1.0, 5012.00, 0.0, 5006.00, 4998.00,
                         Regime::GAUSSIAN_STABLE, 0.25 },
          Expected{ 4996.50, 5012.00, 4998.00, 5006.00, 25, 2.00, 6.00, true, true });

    // Case 3 — turtle_soup_sell_caps_nonbinding
    check("turtle_soup_sell_caps_nonbinding",
          BarrierInputs{ 4, false, 5000.00, 5001.00, 4999.50, 0.0, 0.0,
                         5.00, 1.0, 1.0, 0.0, 4992.00, 5006.00, 4988.00,
                         Regime::GAUSSIAN_STABLE, 0.25 },
          Expected{ 5003.50, 4992.00, 5003.50, 4992.00, 25, 3.50, 8.00, false, false });

    // Case 4 — momentum_pinball_sell_dof_scaled  (dof 1.2 -> eff_atr 6.00)
    check("momentum_pinball_sell_dof_scaled",
          BarrierInputs{ 6, false, 5000.00, 5001.00, 4999.50, 0.0, 0.0,
                         5.00, 1.2, 1.0, 0.0, 4990.00, 5010.00, 4985.00,
                         Regime::PARETO_MOMENTUM, 0.25 },
          Expected{ 5003.40, 4990.00, 5003.40, 4990.00, 12, 3.40, 10.00, false, false });

    // Case 5 — elder_breakout_buy_hybrid_1p5R
    check("elder_breakout_buy_hybrid_1p5R",
          BarrierInputs{ 7, true, 5000.00, 5001.00, 4998.00, 4999.50, 4997.50,
                         5.00, 1.0, 1.0, 0.0, 0.0, 5010.00, 4990.00,
                         Regime::COILED_SPRING, 0.25 },
          Expected{ 4997.25, 5004.125, 4997.25, 5004.125, 40, 2.75, 4.125, false, false });

    // Case 6 — nr7_breakout_buy_low_generic
    check("nr7_breakout_buy_low_generic",
          BarrierInputs{ 9, true, 5000.00, 5000.50, 4999.50, 0.0, 0.0,
                         5.00, 1.0, 1.0, 0.0, 0.0, 5015.00, 4990.00,
                         Regime::GAUSSIAN_FRAGILE, 0.25 },
          Expected{ 4997.50, 5005.00, 4997.50, 5005.00, 15, 2.50, 5.00, false, false });

    // Case 7 — itr_fade_sell_low_caps_binding
    check("itr_fade_sell_low_caps_binding",
          BarrierInputs{ 14, false, 5000.00, 5000.50, 4999.50, 0.0, 0.0,
                         5.00, 1.0, 1.0, 0.0, 0.0, 5001.50, 4998.00,
                         Regime::GAUSSIAN_STABLE, 0.25 },
          Expected{ 5002.50, 4997.50, 5001.50, 4998.00, 25, 1.50, 2.00, true, true });

    if (g_failures == 0) {
        std::printf("ALL 7 GOLDEN VECTORS PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
