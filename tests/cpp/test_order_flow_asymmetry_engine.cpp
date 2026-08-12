// test_order_flow_asymmetry_engine.cpp — characterization tests for
// OrderFlowAsymmetryEngine's pure (ask,bid) -> asymmetry formula, extracted
// so it can be natively unit-tested without Sierra Chart/ACSIL deps
// (docs/superpowers/specs/2026-08-12-tick-native-toxicity-illiquidity-design.md).
//
// Build & run natively:
//   g++ -std=c++17 -I include tests/cpp/test_order_flow_asymmetry_engine.cpp -o /tmp/ofae_test && /tmp/ofae_test

#include "OrderFlowAsymmetryEngine.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void check(const char* name, bool ok) {
    if (ok) {
        std::printf("  PASS  %s\n", name);
    } else {
        ++g_failures;
        std::printf("  FAIL  %s\n", name);
    }
}

bool approx(float a, float b, float tol = 1e-6f) {
    return std::fabs(a - b) <= tol;
}

}  // namespace

int main() {
    std::printf("OrderFlowAsymmetryEngine unit tests\n");

    check("balanced_flow_is_zero",
          approx(ofae::ComputeMicroAsymmetry(50.0f, 50.0f, 0.0f), 0.0f));

    check("all_buying_is_plus_one",
          approx(ofae::ComputeMicroAsymmetry(100.0f, 0.0f, 0.0f), 1.0f));

    check("all_selling_is_minus_one",
          approx(ofae::ComputeMicroAsymmetry(0.0f, 100.0f, 0.0f), -1.0f));

    check("partial_buy_skew",
          approx(ofae::ComputeMicroAsymmetry(75.0f, 25.0f, 0.0f), 0.5f));

    check("no_trade_bar_with_no_prior_value_returns_neutral",
          approx(ofae::ComputeMicroAsymmetry(0.0f, 0.0f, 0.0f), 0.0f));

    check("no_trade_bar_carries_forward_prior_value",
          approx(ofae::ComputeMicroAsymmetry(0.0f, 0.0f, 0.7f), 0.7f));

    check("negative_no_trade_carry_forward",
          approx(ofae::ComputeMicroAsymmetry(0.0f, 0.0f, -0.35f), -0.35f));

    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
