// tests/cpp/TestTurtleSoupFusion.cpp — Option A geometric heuristic + fusion tests.
//
// Build: g++ -std=c++17 -Wall -Wextra -I include tests/cpp/TestTurtleSoupFusion.cpp -o /tmp/t_ts && /tmp/t_ts

#include "TurtleSoupFusion.h"
#include "ClassifierParams.h"

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
    std::printf("Turtle Soup Option A fusion tests\n");

    // --- Bullish setup: price penetrated below the 20-bar low, then recovered above it ---
    // (EvaluateTurtleSoupOptionA is pure and takes no PredatorContext -- confirmed no
    // consumer needs one; removed an unused local left over from an earlier draft.)
    {
        TurtleSoupMicroSignal sig = EvaluateTurtleSoupOptionA(
            /*low=*/98.0f, /*high=*/101.0f, /*closeSoFar=*/100.5f,
            /*twentyBarLow=*/99.0f, /*twentyBarHigh=*/105.0f,
            /*atr=*/1.0f, /*elapsedFraction=*/0.6f
        );
        check("bullish penetrate-then-recover -> positive score", sig.score > 0.0f);
        check("isValid true given a real bar range", sig.isValid == true);
        check("earliness matches elapsedFraction", std::fabs(sig.earliness - 0.6f) < 1e-4f);
    }

    // --- Bearish setup: price penetrated above the 20-bar high, then rejected back below it ---
    // closeSoFar=101.25 puts closePosition at 0.3 within the bar's own [99, 106.5] range --
    // corrected from an earlier draft's 104.5 (closePosition 0.73, the WRONG side of the
    // 0.45 threshold, which would never have triggered the bearish branch at all; caught
    // while running this test for the first time, same class of self-authored scenario bug
    // as Task 3's FuseTauStar test).
    {
        TurtleSoupMicroSignal sig = EvaluateTurtleSoupOptionA(
            /*low=*/99.0f, /*high=*/106.5f, /*closeSoFar=*/101.25f,
            /*twentyBarLow=*/95.0f, /*twentyBarHigh=*/106.0f,
            /*atr=*/1.0f, /*elapsedFraction=*/0.4f
        );
        check("bearish penetrate-then-reject -> negative score", sig.score < 0.0f);
    }

    // --- Direction-discrimination proof (the shape that would have caught Elder Breakout's bug):
    //     a bullish setup and a bearish setup with mirrored geometry must produce OPPOSITE-signed
    //     scores, not the same result regardless of direction. ---
    {
        TurtleSoupMicroSignal bull = EvaluateTurtleSoupOptionA(
            98.0f, 101.0f, 100.5f, 99.0f, 105.0f, 1.0f, 0.5f);
        TurtleSoupMicroSignal bear = EvaluateTurtleSoupOptionA(
            99.0f, 106.5f, 101.25f, 95.0f, 106.0f, 1.0f, 0.5f);
        check("bullish and bearish setups produce opposite-signed scores, not identical",
              (bull.score > 0.0f) != (bear.score > 0.0f));
    }

    // --- No penetration at all -> invalid/neutral signal ---
    {
        TurtleSoupMicroSignal sig = EvaluateTurtleSoupOptionA(
            /*low=*/100.0f, /*high=*/102.0f, /*closeSoFar=*/101.0f,
            /*twentyBarLow=*/95.0f, /*twentyBarHigh=*/106.0f,
            /*atr=*/1.0f, /*elapsedFraction=*/0.5f
        );
        check("no extreme penetration -> isValid false (no setup forming)", sig.isValid == false);
    }

    // --- Option B: hand-crafted logistic regression inference ---
    {
        // Synthetic test weights (not a real trained model -- proves the inference math only).
        ClassifierParams params{};
        params.weights = {2.0f, -1.0f, 0.5f};  // [penetration_atr, close_position, elapsed_fraction]
        params.bias = 0.0f;
        params.isLoaded = true;

        // Strong positive penetration, high close position -> should score clearly bullish
        TurtleSoupMicroSignal sig = EvaluateTurtleSoupOptionB(params, /*penetrationAtr=*/1.0f, /*closePosition=*/0.2f, /*elapsedFraction=*/0.5f);
        check("Option B: isValid true when params.isLoaded", sig.isValid == true);
        check("Option B: positive score for strong bullish features",
              sig.score > 0.0f);

        // Unloaded params (inert default) -> isValid false, never fires
        ClassifierParams unloaded{};
        TurtleSoupMicroSignal inert = EvaluateTurtleSoupOptionB(unloaded, 1.0f, 0.2f, 0.5f);
        check("Option B: unloaded params -> isValid false (inert until real model exists)",
              inert.isValid == false);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
