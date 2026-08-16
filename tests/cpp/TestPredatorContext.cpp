// tests/cpp/TestPredatorContext.cpp — standalone unit tests for PredatorContext assembly.
//
// Build: g++ -std=c++17 -Wall -Wextra -I include tests/cpp/TestPredatorContext.cpp -o /tmp/t_pc && /tmp/t_pc

#include "PredatorContext.h"

#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}  // namespace

int main() {
    std::printf("PredatorContext tests\n");

    // --- Default-constructed PredatorContext is safe (zero-initialized, not garbage) ---
    {
        PredatorContext ctx{};
        check("default gang.isValid is false", ctx.gang.isValid == false);
        check("default regime is COILED_SPRING (enum 0)", ctx.regime == HMMStateEnum::COILED_SPRING);
        check("default inPosition is false", ctx.inPosition == false);
        check("default applicabilityMask is zero", ctx.applicabilityMask == 0ULL);
    }

    // --- Struct is trivially copyable (DOD requirement: no heap, no vtable) ---
    {
        PredatorContext a{};
        a.gang.hurstExponent = 0.72f;
        a.regime = HMMStateEnum::PARETO_MOMENTUM;
        a.inPosition = true;
        PredatorContext b = a;  // plain copy, must compile and preserve values
        check("copy preserves hurstExponent", b.gang.hurstExponent == 0.72f);
        check("copy preserves regime", b.regime == HMMStateEnum::PARETO_MOMENTUM);
        check("copy preserves inPosition", b.inPosition == true);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
