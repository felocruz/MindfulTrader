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

    std::printf("\nIndicatorComputations (Oscillator310) tests\n");
    check("oscillator_bullish_cross",
          ComputeOscillator310Cross(/*fastLine=*/1.0f, /*slowLine=*/0.5f,
                                     /*prevFastLine=*/0.5f, /*prevSlowLine=*/1.0f) == Oscillator310CrossEnum::BULLISH_CROSS);
    check("oscillator_bearish_cross",
          ComputeOscillator310Cross(/*fastLine=*/0.5f, /*slowLine=*/1.0f,
                                     /*prevFastLine=*/1.0f, /*prevSlowLine=*/0.5f) == Oscillator310CrossEnum::BEARISH_CROSS);
    check("oscillator_neutral_no_cross",
          ComputeOscillator310Cross(/*fastLine=*/1.0f, /*slowLine=*/0.5f,
                                     /*prevFastLine=*/1.0f, /*prevSlowLine=*/0.5f) == Oscillator310CrossEnum::NEUTRAL);

    std::printf("\nIndicatorComputations (KangarooTail/TurtleSoup/MomentumPinball/"
                "ElderBreakout/NR7) tests\n");

    // --- DetectKangarooTail --------------------------------------------
    {
        // Bullish: long lower tail (>=1.5x body), tail >=0.3x ATR, close in
        // upper 60% of range. open=100, close=101 (body=1), low=95 (lower
        // tail=5, ratio=5.0 -> >=4.0 body-ratio tier), high=101.2, atr=2.0
        // (tailToATR = 5/2 = 2.5 -> >=1.0, so both EXTREME conditions hold).
        float ratio = 0.0f, tailAtr = 0.0f, closePos = 0.0f, quality = 0.0f;
        KangarooTailEnum e = DetectKangarooTail(100.0f, 101.2f, 95.0f, 101.0f, 2.0f,
                                                  ratio, tailAtr, closePos, quality);
        check("kangaroo_tail_bullish_extreme", e == KangarooTailEnum::BULLISH_EXTREME);
        check("kangaroo_tail_quality_in_range", quality > 0.0f && quality <= 1.0f);
    }
    {
        // No pattern: doji-ish bar, no meaningful tail.
        float ratio = 0.0f, tailAtr = 0.0f, closePos = 0.0f, quality = 0.0f;
        KangarooTailEnum e = DetectKangarooTail(100.0f, 100.5f, 99.5f, 100.1f, 2.0f,
                                                  ratio, tailAtr, closePos, quality);
        check("kangaroo_tail_none_when_flat", e == KangarooTailEnum::NONE);
    }
    {
        // Invalid input (high <= low) -> NONE, outputs zeroed.
        float ratio = 1.0f, tailAtr = 1.0f, closePos = 1.0f, quality = 1.0f;
        KangarooTailEnum e = DetectKangarooTail(100.0f, 99.0f, 99.0f, 99.0f, 2.0f,
                                                  ratio, tailAtr, closePos, quality);
        check("kangaroo_tail_invalid_range_is_none", e == KangarooTailEnum::NONE);
        check("kangaroo_tail_invalid_range_zeroes_quality", quality == 0.0f);
    }

    // --- DetectTurtleSoup ------------------------------------------------
    {
        // Bullish: low breaks below 4-day low, close recovers back above it.
        // fourDayLow=100, low=98 (penetration=2/4=0.5x ATR exactly, not >0.5
        // so this lands STRONG via the >=0.3 leg, not EXTREME), close=103
        // (closeBack=3/4=0.75x ATR -> deep recovery bonus), high=103.5 ->
        // closePosition=(103-98)/5.5=0.909 (upper, STRONG needs >=0.60).
        // fourDayHigh=110 (well away, so the bearish branch can't fire too).
        float pen = 0.0f, closeDist = 0.0f, closePos = 0.0f, quality = 0.0f;
        TurtleSoupEnum e = DetectTurtleSoup(100.0f, 103.5f, 98.0f, 103.0f,
                                              110.0f, 100.0f, 4.0f,
                                              pen, closeDist, closePos, quality);
        check("turtle_soup_bullish_pattern_detected",
              e == TurtleSoupEnum::BULLISH_STRONG || e == TurtleSoupEnum::BULLISH_EXTREME);
        check("turtle_soup_quality_in_range", quality > 0.0f && quality <= 1.0f);
    }
    {
        // No breakout at all -> NONE.
        float pen = 0.0f, closeDist = 0.0f, closePos = 0.0f, quality = 0.0f;
        TurtleSoupEnum e = DetectTurtleSoup(100.0f, 101.0f, 99.0f, 100.5f,
                                              105.0f, 95.0f, 4.0f,
                                              pen, closeDist, closePos, quality);
        check("turtle_soup_none_when_inside_range", e == TurtleSoupEnum::NONE);
    }

    // --- DetectMomentumPinball -------------------------------------------
    {
        // Fresh bullish RSI cross (prev RSI3<=RSI10, now RSI3>RSI10) with
        // stoch < 20 (oversold) -> bullish pinball.
        float rsiDelta = 0.0f, stochDepth = 0.0f, volSpike = 0.0f, quality = 0.0f;
        bool impulseChanged = false;
        MomentumPinballEnum e = DetectMomentumPinball(
            /*rsi3=*/25.0f, /*rsi10=*/20.0f, /*prevRSI3=*/18.0f, /*prevRSI10=*/20.0f,
            /*stochK=*/12.0f, /*impulseColor=*/1, /*prevImpulseColor=*/1,
            /*volume=*/100.0, /*avgVolume=*/100.0,
            rsiDelta, stochDepth, impulseChanged, volSpike, quality);
        check("momentum_pinball_bullish_on_fresh_cross_oversold",
              e == MomentumPinballEnum::BULLISH_STRONG || e == MomentumPinballEnum::BULLISH_WEAK ||
              e == MomentumPinballEnum::BULLISH_EXTREME);
        check("momentum_pinball_rsi_delta_matches", near(rsiDelta, 5.0f));
    }
    {
        // No cross (RSI3/RSI10 relationship unchanged) -> NONE regardless of
        // stochastic extremity.
        float rsiDelta = 0.0f, stochDepth = 0.0f, volSpike = 0.0f, quality = 0.0f;
        bool impulseChanged = false;
        MomentumPinballEnum e = DetectMomentumPinball(
            25.0f, 20.0f, 24.0f, 19.0f,
            12.0f, 1, 1,
            100.0, 100.0,
            rsiDelta, stochDepth, impulseChanged, volSpike, quality);
        check("momentum_pinball_none_without_fresh_cross", e == MomentumPinballEnum::NONE);
    }

    // --- DetectElderBreakout ---------------------------------------------
    {
        // Bullish breakout: close clears upper band by 0.6x ATR, volume
        // spike 1.6x, hurst 0.6 (persistent) -> STRONG tier.
        float dist = 0.0f, hurstOut = 0.0f, volSpike = 0.0f, quality = 0.0f;
        int consolOut = 0;
        bool gapOut = false;
        ElderBreakoutEnum e = DetectElderBreakout(
            /*close=*/106.0f, /*upperBand=*/105.0f, /*lowerBand=*/95.0f,
            /*atr=*/1.67f, /*hurst=*/0.6f,
            /*volume=*/160.0, /*avgVolume=*/100.0,
            /*consolidationBars=*/3, /*isGap=*/false,
            dist, hurstOut, volSpike, consolOut, gapOut, quality);
        check("elder_breakout_bullish_strong", e == ElderBreakoutEnum::BULLISH_STRONG);
        check("elder_breakout_quality_in_range", quality > 0.0f && quality <= 1.0f);
    }
    {
        // Close inside both bands -> NONE.
        float dist = 0.0f, hurstOut = 0.0f, volSpike = 0.0f, quality = 0.0f;
        int consolOut = 0;
        bool gapOut = false;
        ElderBreakoutEnum e = DetectElderBreakout(
            100.0f, 105.0f, 95.0f, 1.0f, 0.5f, 100.0, 100.0, 0, false,
            dist, hurstOut, volSpike, consolOut, gapOut, quality);
        check("elder_breakout_none_when_inside_bands", e == ElderBreakoutEnum::NONE);
    }

    // --- DetectNR7 ---------------------------------------------------------
    {
        // Current range (1.0) narrower than all of the last 7 bars' ranges
        // (avg 2.0) -> narrowest, percentile 0.5 -> EXTREME tier (<0.80).
        std::vector<float> ranges(7, 2.0f);
        float curRange = 0.0f, avgRange = 0.0f, pct = 0.0f, volSpike = 0.0f, quality = 0.0f;
        NR7Enum e = DetectNR7(/*currentHigh=*/101.0f, /*currentLow=*/100.0f, ranges,
                                /*volume=*/100.0, /*avgVolume=*/100.0,
                                /*consolidationBars=*/2,
                                curRange, avgRange, pct, volSpike, quality);
        check("nr7_extreme_on_deep_compression", e == NR7Enum::EXTREME);
        check("nr7_quality_in_range", quality > 0.0f && quality <= 1.0f);
    }
    {
        // Current range NOT narrowest (some prior bar was tighter) -> NONE.
        std::vector<float> ranges = {0.5f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f, 2.0f};
        float curRange = 0.0f, avgRange = 0.0f, pct = 0.0f, volSpike = 0.0f, quality = 0.0f;
        NR7Enum e = DetectNR7(101.0f, 100.0f, ranges, 100.0, 100.0, 2,
                                curRange, avgRange, pct, volSpike, quality);
        check("nr7_none_when_not_narrowest", e == NR7Enum::NONE);
    }
    {
        // Fewer than 7 range samples -> NONE (insufficient lookback).
        std::vector<float> ranges = {1.0f, 1.0f, 1.0f};
        float curRange = 0.0f, avgRange = 0.0f, pct = 0.0f, volSpike = 0.0f, quality = 0.0f;
        NR7Enum e = DetectNR7(101.0f, 100.0f, ranges, 100.0, 100.0, 2,
                                curRange, avgRange, pct, volSpike, quality);
        check("nr7_none_when_insufficient_lookback", e == NR7Enum::NONE);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
