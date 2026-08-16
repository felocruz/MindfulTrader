// tests/cpp/test_predator_fusion_dispatch.cpp — applicability-mask dispatch tests.
//
// Build: g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_predator_fusion_dispatch.cpp -o /tmp/t_pfd && /tmp/t_pfd

#include "PredatorFusion.h"

#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}  // namespace

int main() {
    std::printf("PredatorFusion dispatch tests\n");

    // --- Contract element #5, structural: entry-fusion bits can never be set while in a position ---
    {
        const uint64_t mask = ComputeApplicabilityMask(/*inPosition=*/true, HMMStateEnum::GAUSSIAN_STABLE);
        check("TAU_STAR (exit-side) bit set while in position",
              (mask & FusionKeyMask(FusionKey::TAU_STAR)) != 0ULL);
        check("TURTLE_SOUP_OPTION_A (entry-side) bit CLEAR while in position — structural safety guarantee",
              (mask & FusionKeyMask(FusionKey::TURTLE_SOUP_OPTION_A)) == 0ULL);
    }

    // --- Flat: entry-fusion bits set, exit-fusion bits clear ---
    {
        const uint64_t mask = ComputeApplicabilityMask(/*inPosition=*/false, HMMStateEnum::PARETO_MOMENTUM);
        check("TURTLE_SOUP_OPTION_A bit set while flat",
              (mask & FusionKeyMask(FusionKey::TURTLE_SOUP_OPTION_A)) != 0ULL);
        check("TAU_STAR bit CLEAR while flat (no position to exit)",
              (mask & FusionKeyMask(FusionKey::TAU_STAR)) == 0ULL);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
