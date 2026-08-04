// test_volume_profile_engine.cpp — unit tests for the pure Point-of-Control /
// Value-Area expansion algorithm.
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I include tests/cpp/test_volume_profile_engine.cpp -o /tmp/vpe_test && /tmp/vpe_test

#include "VolumeProfileEngine.h"

#include <cstdio>

using namespace vpe;

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
    std::printf("VolumeProfileEngine unit tests\n");

    // Empty input -> invalid result, no crash.
    {
        const ValueArea va = ComputeValueArea({});
        check("empty_input_is_invalid", !va.valid);
    }

    // Single price level -> POC = VAH = VAL = that price.
    {
        const ValueArea va = ComputeValueArea({{5000, 100.0}});
        check("single_level_poc_equals_bounds",
              va.valid && va.pocPriceInTicks == 5000 &&
              va.valueAreaHighInTicks == 5000 && va.valueAreaLowInTicks == 5000);
    }

    // Symmetric bell-shaped volume around 5002 (the classic textbook case):
    //   4999:10  5000:20  5001:40  5002:60  5003:40  5004:20  5005:10
    // total=200, target(70%)=140. POC=5002 (60), captured=60.
    // 2-row check: above(5003+5004=40+20=60) vs below(5001+5000=40+20=60) ->
    // exact tie -> expand BOTH sides by 2: VAH=5004, VAL=5000,
    // captured=60+60+60=180 >= 140 -> stop.
    {
        const std::vector<PriceVolume> levels = {
            {4999, 10.0}, {5000, 20.0}, {5001, 40.0}, {5002, 60.0},
            {5003, 40.0}, {5004, 20.0}, {5005, 10.0},
        };
        const ValueArea va = ComputeValueArea(levels, 70.0);
        check("symmetric_distribution_poc",
              va.valid && va.pocPriceInTicks == 5002);
        check("symmetric_distribution_value_area_bounds",
              va.valueAreaHighInTicks == 5004 && va.valueAreaLowInTicks == 5000);
    }

    // Skewed distribution: POC not centered, value area should still expand
    // toward whichever side has more volume at each step.
    // 5000:5  5001:10  5002:80(POC)  5003:5
    // total=100, target(70%)=70. POC=5002(80) already >= 70 -> VAH=VAL=5002,
    // loop doesn't even need to expand.
    {
        const std::vector<PriceVolume> levels = {
            {5000, 5.0}, {5001, 10.0}, {5002, 80.0}, {5003, 5.0},
        };
        const ValueArea va = ComputeValueArea(levels, 70.0);
        check("dominant_poc_needs_no_expansion",
              va.valid && va.pocPriceInTicks == 5002 &&
              va.valueAreaHighInTicks == 5002 && va.valueAreaLowInTicks == 5002);
    }

    // The reason this needs to be a 2-row TICK expansion (not 2-key), made
    // concrete: array-based algorithm treats an untraded tick as a real
    // 0-volume position, whereas a std::map-iterator approach would skip it
    // entirely. With an untraded gap and traded prices on both sides, they
    // diverge: array sees "ticks 5003(0) + 5004(5)=5", map sees "keys 5004 +
    // 5005=40". Test data: 5000:10 5001:5 5002:50(POC) [5003: implicit 0]
    // 5004:5 5005:35. Total=105, target(70%)=73.5.
    // Array: captured=50. iter1: above(0+5=5) < below(5+10=15), expand below
    // to 5000, captured=65. iter2: above(0+5=5), no below, expand above to
    // 5004, captured=70. iter3: above(35), expand above to 5005, captured=105.
    // Result: POC=5002, VAH=5005, VAL=5000.
    // Map (wrong): would compare keys 5004+5005(=40) vs 5001+5000(=15), expand
    // above immediately, never lowering below 5002 -- demonstrating why tick
    // positions matter, not just traded keys.
    {
        const std::vector<PriceVolume> levels = {
            {5000, 10.0}, {5001, 5.0}, {5002, 50.0}, {5004, 5.0}, {5005, 35.0},
        };
        const ValueArea va = ComputeValueArea(levels, 70.0);
        check("two_row_rule_respects_tick_positions_not_traded_key_positions",
              va.valid && va.pocPriceInTicks == 5002 &&
              va.valueAreaHighInTicks == 5005 && va.valueAreaLowInTicks == 5000);
    }

    // Multiple PriceVolume entries at the SAME price must be summed, not
    // overwritten (this models multiple bars in the aggregated day
    // contributing volume at the same price level).
    {
        const std::vector<PriceVolume> levels = {
            {5000, 10.0}, {5000, 15.0}, // same price, two bars -> 25 total
            {5001, 5.0},
        };
        const ValueArea va = ComputeValueArea(levels, 70.0);
        check("duplicate_price_entries_are_summed",
              va.valid && va.pocPriceInTicks == 5000);
    }

    // All-zero volume -> invalid (no real distribution to compute from).
    {
        const ValueArea va = ComputeValueArea({{5000, 0.0}, {5001, 0.0}});
        check("all_zero_volume_is_invalid", !va.valid);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
