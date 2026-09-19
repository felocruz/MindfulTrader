// test_entered_or_exited_neutral.cpp — unit tests for EnteredOrExitedNeutral()
// (IndicatorComputations.h), the shared enum-typed generalization of
// IndicatorManager.cpp's int8_t-based EnteredOrExitedNone, introduced
// 2026-09-19 (docs/superpowers/specs/2026-09-19-meaningful-event-trigger-and-
// asymmetry-context-significance-spec.md) to fix RASCHKE_STRATEGY_SETUP,
// RASCHKE_TACTICAL_TRIGGER, VOLUME_SIGNAL, and DAILY_BIAS -- the same
// "always false" dead-trigger bug class as STRUCTURE_TEST.
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I include tests/cpp/test_entered_or_exited_neutral.cpp \
//     -o /tmp/eoen_test && /tmp/eoen_test

#include "IndicatorComputations.h"
#include "DailyBiasEngine.h"

#include <cstdio>

namespace {

int g_failures = 0;

void check(const char* name, bool got, bool expected) {
    if (got == expected) {
        std::printf("  PASS  %s\n", name);
    } else {
        ++g_failures;
        std::printf("  FAIL  %s  got=%d exp=%d\n", name, got, expected);
    }
}

}  // namespace

int main() {
    std::printf("EnteredOrExitedNeutral unit tests\n");

    // Generic shape, using RaschkeStrategySetup as the representative enum.
    using RSS = RaschkeStrategySetup;
    check("stays_at_neutral_no_trigger",
          EnteredOrExitedNeutral(RSS::NONE, RSS::NONE, RSS::NONE), false);
    check("neutral_to_actionable_triggers",
          EnteredOrExitedNeutral(RSS::NONE, RSS::WHIPLASH, RSS::NONE), true);
    check("actionable_to_neutral_triggers",
          EnteredOrExitedNeutral(RSS::WHIPLASH, RSS::NONE, RSS::NONE), true);
    check("actionable_to_different_actionable_does_not_trigger",
          EnteredOrExitedNeutral(RSS::WHIPLASH, RSS::GHOST, RSS::NONE), false);
    check("stays_at_same_actionable_no_trigger",
          EnteredOrExitedNeutral(RSS::GHOST, RSS::GHOST, RSS::NONE), false);

    // RASCHKE_TACTICAL_TRIGGER: same idiom, different enum.
    using RTT = RaschkeTacticalTrigger;
    check("tactical_neutral_to_itr_breakout_triggers",
          EnteredOrExitedNeutral(RTT::NONE, RTT::ITR_BREAKOUT_BUY, RTT::NONE), true);
    check("tactical_itr_fade_to_neutral_triggers",
          EnteredOrExitedNeutral(RTT::ITR_FADE_SELL, RTT::NONE, RTT::NONE), true);
    check("tactical_actionable_to_different_actionable_does_not_trigger",
          EnteredOrExitedNeutral(RTT::ITR_BREAKOUT_BUY, RTT::ITR_FADE_SELL, RTT::NONE), false);

    // VOLUME_SIGNAL: NORMAL is the neutral value, not NONE.
    using VE = VolumeEnum;
    check("volume_normal_to_high_triggers",
          EnteredOrExitedNeutral(VE::NORMAL, VE::HIGH, VE::NORMAL), true);
    check("volume_high_to_normal_triggers",
          EnteredOrExitedNeutral(VE::HIGH, VE::NORMAL, VE::NORMAL), true);
    check("volume_high_to_very_high_does_not_trigger",
          EnteredOrExitedNeutral(VE::HIGH, VE::VERY_HIGH, VE::NORMAL), false);
    check("volume_stays_normal_no_trigger",
          EnteredOrExitedNeutral(VE::NORMAL, VE::NORMAL, VE::NORMAL), false);

    // DAILY_BIAS: PHYSICS_VETO_RANDOM_WALK is the neutral value (dbe::Bias,
    // the ACSIL-independent offline-path enum -- DailyBiasEnum, the
    // ACSIL-coupled Indicator.h twin, is exercised separately by the DLL build).
    using Bias = dbe::Bias;
    check("daily_bias_veto_to_bullish_trend_triggers",
          EnteredOrExitedNeutral(Bias::PHYSICS_VETO_RANDOM_WALK, Bias::BULLISH_TREND_PERSISTENT,
                                  Bias::PHYSICS_VETO_RANDOM_WALK), true);
    check("daily_bias_bearish_to_veto_triggers",
          EnteredOrExitedNeutral(Bias::BEARISH_MEAN_REVERSION, Bias::PHYSICS_VETO_RANDOM_WALK,
                                  Bias::PHYSICS_VETO_RANDOM_WALK), true);
    check("daily_bias_bullish_to_bearish_does_not_trigger",
          EnteredOrExitedNeutral(Bias::BULLISH_TREND_PERSISTENT, Bias::BEARISH_TREND_PERSISTENT,
                                  Bias::PHYSICS_VETO_RANDOM_WALK), false);

    std::printf("%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "SOME FAILED",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
