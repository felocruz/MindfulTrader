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

    std::printf("\nIndicatorComputations (Impulse) tests\n");

    // Opaque color constants for the tests -- ComputeImpulse doesn't care what
    // the real values are, only that they're distinct (mirrors GREEN/RED/BLUE
    // being passed through from Indicator.h's real CreateRGB(...) constants).
    constexpr int kGreen = 1;
    constexpr int kRed = 2;
    constexpr int kBlue = 3;

    // --- Color classification (one exercising test per branch) -----------
    {
        ImpulseState state;
        ImpulseResult r = ComputeImpulse(state, kGreen, kBlue, 0.0f, 0.0f, 1.0f, kGreen, kRed, kBlue);
        check("blue_to_green", r.signal == ImpulseEnum::BLUE_TO_GREEN);
    }
    {
        ImpulseState state;
        ImpulseResult r = ComputeImpulse(state, kGreen, kGreen, 0.0f, 0.0f, 1.0f, kGreen, kRed, kBlue);
        check("green_plain", r.signal == ImpulseEnum::GREEN);
    }
    {
        ImpulseState state;
        ImpulseResult r = ComputeImpulse(state, kRed, kBlue, 0.0f, 0.0f, 1.0f, kGreen, kRed, kBlue);
        check("blue_to_red", r.signal == ImpulseEnum::BLUE_TO_RED);
    }
    {
        ImpulseState state;
        ImpulseResult r = ComputeImpulse(state, kBlue, kGreen, 0.5f, 0.0f, 1.0f, kGreen, kRed, kBlue);
        check("green_to_blue_bull", r.signal == ImpulseEnum::GREEN_TO_BLUE_BULL);
    }
    {
        ImpulseState state;
        ImpulseResult r = ComputeImpulse(state, kBlue, kGreen, -0.5f, 0.0f, 1.0f, kGreen, kRed, kBlue);
        check("green_to_blue_bear", r.signal == ImpulseEnum::GREEN_TO_BLUE_BEAR);
    }
    {
        ImpulseState state;
        ImpulseResult r = ComputeImpulse(state, kBlue, kRed, -0.5f, 0.0f, 1.0f, kGreen, kRed, kBlue);
        check("red_to_blue_bear", r.signal == ImpulseEnum::RED_TO_BLUE_BEAR);
    }
    {
        ImpulseState state;
        ImpulseResult r = ComputeImpulse(state, kBlue, kRed, 0.5f, 0.0f, 1.0f, kGreen, kRed, kBlue);
        check("red_to_blue_bull", r.signal == ImpulseEnum::RED_TO_BLUE_BULL);
    }
    {
        ImpulseState state;
        ImpulseResult r = ComputeImpulse(state, kBlue, kBlue, 0.5f, 0.0f, 1.0f, kGreen, kRed, kBlue);
        check("blue_bull_no_prior_color", r.signal == ImpulseEnum::BLUE_BULL);
    }
    {
        ImpulseState state;
        ImpulseResult r = ComputeImpulse(state, kBlue, kBlue, -0.5f, 0.0f, 1.0f, kGreen, kRed, kBlue);
        check("blue_bear_no_prior_color", r.signal == ImpulseEnum::BLUE_BEAR);
    }
    {
        ImpulseState state;
        ImpulseResult r = ComputeImpulse(state, kBlue, kBlue, 0.0f, 0.0f, 1.0f, kGreen, kRed, kBlue);
        check("blue_true_neutral", r.signal == ImpulseEnum::BLUE);
    }

    // --- Derived metrics: magnitude/fatigue/runLength/transitionRate -----
    {
        // ATR <= 0 -> magnitude clamps to 0, regardless of maDiff/macdDiff.
        ImpulseState state;
        ImpulseResult r = ComputeImpulse(state, kGreen, kGreen, 5.0f, 5.0f, 0.0f, kGreen, kRed, kBlue);
        check("zero_atr_zero_magnitude", r.magnitude == 0.0f);
    }
    {
        // magnitude = clamp(((maDiff/atr) + (macdDiff/atr)) * 0.5, -1, 1)
        // maDiff=2, macdDiff=2, atr=1 -> ((2+2)*0.5) = 2, clamped to 1.0.
        ImpulseState state;
        ImpulseResult r = ComputeImpulse(state, kGreen, kGreen, 2.0f, 2.0f, 1.0f, kGreen, kRed, kBlue);
        check("magnitude_clamped_to_one", near(r.magnitude, 1.0f));
        // fatigue = magnitude - prevMagnitude(0 on cold start) = 1.0
        check("fatigue_from_cold_start", near(r.fatigue, 1.0f));
    }
    {
        // Run length increments while color stays the same, resets to 1 on change.
        ImpulseState state;
        ImpulseResult r1 = ComputeImpulse(state, kGreen, kGreen, 0.0f, 0.0f, 1.0f, kGreen, kRed, kBlue);
        ImpulseResult r2 = ComputeImpulse(state, kGreen, kGreen, 0.0f, 0.0f, 1.0f, kGreen, kRed, kBlue);
        ImpulseResult r3 = ComputeImpulse(state, kRed, kGreen, 0.0f, 0.0f, 1.0f, kGreen, kRed, kBlue);
        check("run_length_increments", r1.runLength == 1 && r2.runLength == 2);
        check("run_length_resets_on_color_change", r3.runLength == 1);
    }
    {
        // Transition rate: fraction of last 16 bars with a color change.
        // One change out of one bar -> 1/16.
        ImpulseState state;
        ImpulseResult r = ComputeImpulse(state, kGreen, kRed, 0.0f, 0.0f, 1.0f, kGreen, kRed, kBlue);
        check("transition_rate_one_change", near(r.transitionRate, 1.0f / 16.0f));
    }

    std::printf("\nIndicatorComputations (Volume) tests\n");

    // --- ComputeVolumeBarSample: cold start + guard ------------------------
    {
        VolumeState state;
        ComputeVolumeBarSample(state, /*completedBarVolume=*/0.0f, /*isRTH=*/true);
        check("zero_volume_is_noop", state.logVolRthCount == 0 && state.volumeZScore == 0.0f);
    }
    {
        VolumeState state;
        ComputeVolumeBarSample(state, /*completedBarVolume=*/-5.0f, /*isRTH=*/true);
        check("negative_volume_is_noop", state.logVolRthCount == 0);
    }
    {
        // Fewer than 5 samples -> zScore stays 0 regardless of session.
        VolumeState state;
        bool allZeroBeforeFive = true;
        for (int i = 1; i <= 4; ++i) {
            ComputeVolumeBarSample(state, static_cast<float>(i * 100), true);
            if (state.volumeZScore != 0.0f) allZeroBeforeFive = false;
        }
        check("zscore_zero_below_five_samples", allZeroBeforeFive);
        check("rth_count_tracks_samples", state.logVolRthCount == 4);
    }
    {
        // RTH and overnight pools are independent (session-segregated).
        VolumeState state;
        ComputeVolumeBarSample(state, 100.0f, /*isRTH=*/true);
        ComputeVolumeBarSample(state, 200.0f, /*isRTH=*/false);
        check("rth_and_overnight_pools_independent", state.logVolRthCount == 1 && state.logVolOvnCount == 1);
    }

    // --- ComputeVolumeClassification: threshold boundaries -----------------
    {
        VolumeClassification c = ComputeVolumeClassification(/*volumeZScore=*/0.0f, 10.0f, 10.0f);
        check("normal_classification", c.signal == VolumeEnum::NORMAL);
    }
    {
        VolumeClassification c = ComputeVolumeClassification(-2.5f, 10.0f, 10.0f);
        check("very_low_classification", c.signal == VolumeEnum::VERY_LOW);
    }
    {
        VolumeClassification c = ComputeVolumeClassification(-1.5f, 10.0f, 10.0f);
        check("low_classification", c.signal == VolumeEnum::LOW);
    }
    {
        VolumeClassification c = ComputeVolumeClassification(2.5f, 10.0f, 10.0f);
        check("very_high_classification", c.signal == VolumeEnum::VERY_HIGH);
    }
    {
        VolumeClassification c = ComputeVolumeClassification(1.5f, 5.0f, 15.0f);
        check("high_buy_volume_classification", c.signal == VolumeEnum::HIGH_BUY_VOLUME);
    }
    {
        VolumeClassification c = ComputeVolumeClassification(1.5f, 15.0f, 5.0f);
        check("high_sell_volume_classification", c.signal == VolumeEnum::HIGH_SELL_VOLUME);
    }
    {
        VolumeClassification c = ComputeVolumeClassification(1.5f, 10.0f, 10.0f);
        check("high_classification_balanced", c.signal == VolumeEnum::HIGH);
    }
    {
        // imbalance = (ask - bid) / (ask + bid); totalVolume = ask + bid.
        VolumeClassification c = ComputeVolumeClassification(0.0f, 30.0f, 70.0f);
        check("imbalance_matches_hand_computed", near(c.imbalance, 0.4f));
        check("total_volume_matches_hand_computed", near(c.totalVolume, 100.0f));
    }
    {
        // Zero total volume -> imbalance defaults to 0 (no divide-by-zero).
        VolumeClassification c = ComputeVolumeClassification(0.0f, 0.0f, 0.0f);
        check("zero_total_volume_zero_imbalance", c.imbalance == 0.0f);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
