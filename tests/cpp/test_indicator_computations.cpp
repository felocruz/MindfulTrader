// tests/cpp/test_indicator_computations.cpp — standalone unit tests for the
// pure compute functions extracted from the old Indicator<T> leaf classes
// (indicator-manager-dod-soa plan). This is the growing home for all such
// extracted compute functions across Tasks 6/8/9 — one file, not one per
// indicator, matching how StudyHelperFunctions.h's free functions are
// already organized in this codebase.
//
// Build: g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_indicator_computations.cpp -o /tmp/t1 && /tmp/t1

#include "IndicatorComputations.h"

#include <cmath>
#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
bool near(float a, float b, float tol = 1e-3f) { return std::fabs(a - b) < tol; }
}  // namespace

int main() {
    std::printf("IndicatorComputations (Macd) tests\n");

    // --- ComputeMacd: cold start ---------------------------------------
    {
        MacdState state;
        MacdResult result = ComputeMacd(state, 5.0, 0.0, 0.0, /*barsAvailable=*/0);
        check("cold_start_signal_is_at_zero", result.signal == MacdEnum::AT_ZERO);
        check("cold_start_zscore_is_zero", result.zScore == 0.0f);
    }

    // --- ComputeMacd: z-score path --------------------------------------
    {
        // Fewer than 5 history samples -> zScore stays 0 regardless of signal.
        MacdState state;
        bool allZeroBeforeFive = true;
        for (int i = 1; i <= 4; ++i) {
            MacdResult r = ComputeMacd(state, static_cast<double>(i), static_cast<double>(i - 1), 0.0, 2);
            if (r.zScore != 0.0f) allZeroBeforeFive = false;
        }
        check("zscore_zero_below_five_samples", allZeroBeforeFive);

        // 5th sample: median/MAD hand-computed over {1,2,3,4,5} -> median=3,
        // MAD=1, denom=1.4826, zScore=(5-3)/1.4826 ≈ 1.34898.
        MacdResult r5 = ComputeMacd(state, 5.0, 4.0, 3.0, 2);
        check("zscore_matches_hand_computed_median_mad", near(r5.zScore, 1.34898f));
    }

    // --- ComputeMacd: sign-change simple-cross transitions ---------------
    {
        MacdState state;
        // barsAvailable >= 2: plain bullish cross (not the 3-bar ZERO_FROM_BELOW
        // pattern -- diffPrev1 < diffPrev2 here, so IsZeroFromBelow's
        // diffPrev1 >= diffPrev2 leg is false).
        MacdResult r = ComputeMacd(state, /*diffCurrent=*/1.0, /*diffPrev1=*/-1.0, /*diffPrev2=*/-0.5, 2);
        check("bullish_cross_barsAvailable_2", r.signal == MacdEnum::BULLISH_CROSS);
    }
    {
        MacdState state;
        MacdResult r = ComputeMacd(state, /*diffCurrent=*/-1.0, /*diffPrev1=*/1.0, /*diffPrev2=*/0.5, 2);
        check("bearish_cross_barsAvailable_2", r.signal == MacdEnum::BEARISH_CROSS);
    }
    {
        // barsAvailable == 1: simplified 2-bar cross path (history still building).
        MacdState state;
        MacdResult r = ComputeMacd(state, /*diffCurrent=*/1.0, /*diffPrev1=*/-1.0, /*diffPrev2=*/0.0, 1);
        check("bullish_cross_barsAvailable_1", r.signal == MacdEnum::BULLISH_CROSS);
    }
    {
        MacdState state;
        MacdResult r = ComputeMacd(state, /*diffCurrent=*/-1.0, /*diffPrev1=*/1.0, /*diffPrev2=*/0.0, 1);
        check("bearish_cross_barsAvailable_1", r.signal == MacdEnum::BEARISH_CROSS);
    }

    // --- 8 classification helpers: one exercising test each ---------------
    // (diffPrev2, diffPrev1, diffCurrent) windows chosen to satisfy exactly
    // one helper's predicate, verified against src/Indicator.cpp:175-221's
    // transcribed logic.
    check("macd_is_spring", MacdIsSpring(/*diffCurrent=*/-2.0, /*diffPrev1=*/-4.0, /*diffPrev2=*/-5.0));
    check("macd_is_summer", MacdIsSummer(/*diffCurrent=*/5.0, /*diffPrev1=*/3.0, /*diffPrev2=*/2.0));
    check("macd_is_fall", MacdIsFall(/*diffCurrent=*/3.0, /*diffPrev1=*/5.0, /*diffPrev2=*/5.0));
    check("macd_is_winter", MacdIsWinter(/*diffCurrent=*/-5.0, /*diffPrev1=*/-3.0, /*diffPrev2=*/-3.0));
    check("macd_is_positive_tick_down", MacdIsPositiveTickDown(/*diffCurrent=*/3.0, /*diffPrev1=*/4.0, /*diffPrev2=*/2.0));
    check("macd_is_negative_tick_up", MacdIsNegativeTickUp(/*diffCurrent=*/-3.0, /*diffPrev1=*/-4.0, /*diffPrev2=*/-2.0));
    check("macd_is_zero_from_below", MacdIsZeroFromBelow(/*diffCurrent=*/0.0, /*diffPrev1=*/-2.0, /*diffPrev2=*/-3.0));
    check("macd_is_zero_from_above", MacdIsZeroFromAbove(/*diffCurrent=*/0.0, /*diffPrev1=*/2.0, /*diffPrev2=*/3.0));

    // Each helper should NOT fire on a window built for a different helper
    // (spot check against the Spring window, to catch an over-broad predicate).
    check("macd_is_summer_excludes_spring_window", !MacdIsSummer(-2.0, -4.0, -5.0));
    check("macd_is_winter_excludes_spring_window", !MacdIsWinter(-2.0, -4.0, -5.0));

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
