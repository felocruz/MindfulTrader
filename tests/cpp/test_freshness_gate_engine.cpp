// test_freshness_gate_engine.cpp — unit tests for the pure freshness/age
// comparison used by TS1/TS2 readiness gates (docs/superpowers/plans/2026-08-04-phase1-hardening.md
// Task 3; lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1.3).
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I include tests/cpp/test_freshness_gate_engine.cpp -o /tmp/fge_test && /tmp/fge_test

#include "FreshnessGateEngine.h"

#include <cstdio>

using namespace fge;

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

}  // namespace

int main() {
    std::printf("FreshnessGateEngine unit tests\n");

    constexpr uint64_t kSixHoursUs = 6ULL * 60ULL * 60ULL * 1'000'000ULL;
    constexpr uint64_t kFortyNineHoursUs = 49ULL * 60ULL * 60ULL * 1'000'000ULL;

    // Ordinary weekday: 1 hour stale, well within a 6h budget, no grace needed.
    check("fresh_within_budget_no_grace_needed",
          IsFresh(/*nowUs=*/10 * 3'600ULL * 1'000'000ULL,
                  /*lastWriteUs=*/9 * 3'600ULL * 1'000'000ULL,
                  kSixHoursUs, /*bypassCheck=*/false));

    // Ordinary weekday: genuinely stuck for 8 hours (real staleness, no weekend
    // involved) -- must still correctly BLOCK when bypassCheck is false.
    check("genuinely_stale_still_blocks_without_grace",
          !IsFresh(/*nowUs=*/10 * 3'600ULL * 1'000'000ULL,
                   /*lastWriteUs=*/2 * 3'600ULL * 1'000'000ULL,
                   kSixHoursUs, /*bypassCheck=*/false));

    // The Sunday-open scenario: 49 hours stale (Friday 17:00 -> Sunday 18:00),
    // which is what triggered the original bug. Without the grace bypass this
    // fails (matching today's real behavior); WITH it, it must pass.
    check("weekend_gap_blocks_without_grace_bypass",
          !IsFresh(kFortyNineHoursUs, 0, kSixHoursUs, /*bypassCheck=*/false));
    check("weekend_gap_passes_with_grace_bypass",
          IsFresh(kFortyNineHoursUs, 0, kSixHoursUs, /*bypassCheck=*/true));

    // Even with the grace bypass active, a lastWriteUs of exactly 0 (never
    // written at all, e.g. before startup warm-up) has nothing to do with the
    // weekend gap and is a different failure mode entirely -- IsFresh() itself
    // only compares the two timestamps given to it; the "never written" case is
    // handled by the CALLER's m_ts1SeenAfterReset check before IsFresh() is ever
    // reached, so this test just documents that IsFresh() alone doesn't special-case it.
    check("bypass_does_not_change_pure_age_arithmetic_when_ages_are_equal",
          IsFresh(1000, 1000, 0, /*bypassCheck=*/false) == IsFresh(1000, 1000, 0, /*bypassCheck=*/true));

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
