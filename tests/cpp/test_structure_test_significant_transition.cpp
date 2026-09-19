// test_structure_test_significant_transition.cpp — unit tests for
// IsStructureTestSignificantTransition() (IndicatorComputations.h), the pure
// "meaningful change" test for StructureTest introduced 2026-09-19 (docs/
// superpowers/specs/2026-09-19-meaningful-event-trigger-and-asymmetry-
// context-significance-spec.md, Phase 1).
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I include tests/cpp/test_structure_test_significant_transition.cpp \
//     -o /tmp/st_trigger_test && /tmp/st_trigger_test

#include "IndicatorComputations.h"

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
    using ST = StructureTest;
    std::printf("IsStructureTestSignificantTransition unit tests\n");

    // Entering an actionable (TRAP) state from neutral -- must trigger.
    check("none_to_failed_low_close_inside",
          IsStructureTestSignificantTransition(ST::NONE, ST::FAILED_LOW_CLOSE_INSIDE), true);
    check("none_to_failed_high_strong_reversal",
          IsStructureTestSignificantTransition(ST::NONE, ST::FAILED_HIGH_STRONG_REVERSAL), true);

    // Exiting an actionable (TRAP) state back to neutral -- must trigger.
    check("failed_low_strong_reversal_to_none",
          IsStructureTestSignificantTransition(ST::FAILED_LOW_STRONG_REVERSAL, ST::NONE), true);

    // Entering/exiting an actionable REGIME_INVALIDATION (DECISIVE_*) state -- must trigger,
    // same as TRAP (both are "actionable" per the ADR split, not just FAILED_*).
    check("none_to_decisive_breakout_high",
          IsStructureTestSignificantTransition(ST::NONE, ST::DECISIVE_BREAKOUT_HIGH), true);
    check("decisive_breakdown_low_to_none",
          IsStructureTestSignificantTransition(ST::DECISIVE_BREAKDOWN_LOW, ST::NONE), true);

    // Direct actionable-to-different-actionable transition -- must trigger (unlike the
    // patterns' entered/exited-NONE idiom, StructureTest's non-neutral values are distinct
    // events, not gradations of one signal).
    check("failed_high_strong_reversal_to_decisive_breakdown_low",
          IsStructureTestSignificantTransition(ST::FAILED_HIGH_STRONG_REVERSAL, ST::DECISIVE_BREAKDOWN_LOW),
          true);
    check("decisive_breakout_high_to_failed_low_close_inside",
          IsStructureTestSignificantTransition(ST::DECISIVE_BREAKOUT_HIGH, ST::FAILED_LOW_CLOSE_INSIDE),
          true);

    // Wobbling among the neutral set only -- must NOT trigger.
    check("none_to_inside_bar", IsStructureTestSignificantTransition(ST::NONE, ST::INSIDE_BAR), false);
    check("inside_bar_to_outside_bar",
          IsStructureTestSignificantTransition(ST::INSIDE_BAR, ST::OUTSIDE_BAR), false);
    check("outside_bar_to_none", IsStructureTestSignificantTransition(ST::OUTSIDE_BAR, ST::NONE), false);

    // No real change at all -- must NOT trigger.
    check("same_value_failed_low_close_inside",
          IsStructureTestSignificantTransition(ST::FAILED_LOW_CLOSE_INSIDE, ST::FAILED_LOW_CLOSE_INSIDE),
          false);
    check("same_value_none", IsStructureTestSignificantTransition(ST::NONE, ST::NONE), false);

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
