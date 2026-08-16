// tests/cpp/TestPredatorFusionDispatch.cpp — applicability-mask dispatch tests.
//
// Build: g++ -std=c++17 -Wall -Wextra -I include tests/cpp/TestPredatorFusionDispatch.cpp -o /tmp/t_pfd && /tmp/t_pfd

#include "PredatorFusion.h"
#include "PredatorContext.h"

#include <cmath>
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

    // --- FuseTauStar: Elkan (2001) cost-sensitive threshold, tau* = C_FP / (C_FP + C_FN) ---
    // C_FP = distance to target (cost of exiting too early, forfeiting the remaining move)
    // C_FN = distance to stop (cost of not exiting, i.e. the loss if wrong)
    {
        PredatorContext ctx{};
        ctx.inPosition = true;
        ctx.regime = HMMStateEnum::GAUSSIAN_STABLE;

        // Symmetric case: target and stop equidistant -> tau* = 0.5
        TauStarFusionResult r1 = FuseTauStar(ctx, /*distanceToTarget=*/2.0, /*distanceToStop=*/2.0, /*modelConfidence=*/0.6);
        check("symmetric distances -> tau* == 0.5", std::fabs(r1.effectiveThreshold - 0.5f) < 1e-4f);
        check("confidence 0.6 < tau* 0.5 is FALSE so shouldExit is true (0.6 >= 0.5)", r1.shouldExit == true);

        // Asymmetric: target much closer than stop -> C_FP small, C_FN large -> tau* low
        // (failing to exit is far costlier here -- the stop is distant -- so the bar to
        // trigger an exit should be lower; verified against Elkan's derivation directly:
        // p* = C_FP/(C_FP+C_FN) = 1/(1+8) = 0.111, not the reverse scenario originally
        // written here, which computed 8/(8+1) = 0.889 -- an error in this test's own
        // scenario, not in FuseTauStar's implementation, caught while running Task 3).
        TauStarFusionResult r2 = FuseTauStar(ctx, /*distanceToTarget=*/1.0, /*distanceToStop=*/8.0, /*modelConfidence=*/0.2);
        check("target much closer than stop -> tau* < 0.5", r2.effectiveThreshold < 0.5f);

        // Confidence below threshold -> should not exit
        TauStarFusionResult r3 = FuseTauStar(ctx, /*distanceToTarget=*/2.0, /*distanceToStop=*/2.0, /*modelConfidence=*/0.1);
        check("low confidence below tau* -> shouldExit false", r3.shouldExit == false);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
