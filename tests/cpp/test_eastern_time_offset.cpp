// test_eastern_time_offset.cpp — characterization tests for EasternTimeOffset.h
// against publicly documented real US DST transition dates (Energy Policy Act
// of 2005 rule: 2nd Sunday in March / 1st Sunday in November, both 2:00 AM
// local), independently computed via Python's calendar.timegm.
//
// Build & run natively:
//   g++ -std=c++17 -I include tests/cpp/test_eastern_time_offset.cpp -o /tmp/eto_test && /tmp/eto_test

#include "EasternTimeOffset.h"

#include <cstdio>

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
    std::printf("EasternTimeOffset unit tests\n");

    // 2024 March transition: 2024-03-10 07:00:00 UTC (2am EST -> 3am EDT).
    check("2024_mar_just_before_is_est", ete::GetEasternUtcOffsetSeconds(1710053940) == -18000);
    check("2024_mar_just_after_is_edt", ete::GetEasternUtcOffsetSeconds(1710054060) == -14400);

    // 2024 November transition: 2024-11-03 06:00:00 UTC (2am EDT -> 1am EST).
    check("2024_nov_just_before_is_edt", ete::GetEasternUtcOffsetSeconds(1730613540) == -14400);
    check("2024_nov_just_after_is_est", ete::GetEasternUtcOffsetSeconds(1730613660) == -18000);

    // 2023 (DST started March 12, ended November 5) -- confirms year-generality.
    check("2023_mar_transition_is_edt", ete::GetEasternUtcOffsetSeconds(1678604400) == -14400);
    check("2023_nov_transition_is_est", ete::GetEasternUtcOffsetSeconds(1699164000) == -18000);

    // 2025 (DST started March 9, ended November 2).
    check("2025_mar_transition_is_edt", ete::GetEasternUtcOffsetSeconds(1741503600) == -14400);
    check("2025_nov_transition_is_est", ete::GetEasternUtcOffsetSeconds(1762063200) == -18000);

    // Mid-summer / mid-winter sanity checks, far from any transition.
    check("mid_summer_2024_is_edt", ete::GetEasternUtcOffsetSeconds(1719835200) == -14400);
    check("mid_winter_2024_is_est", ete::GetEasternUtcOffsetSeconds(1705320000) == -18000);

    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
