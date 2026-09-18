// test_bounded_bar_ring.cpp — unit tests for BoundedBarRing.h
// (docs/superpowers/plans/2026-09-16-market-data-replay-alpha-generator-implementation.md Task 5)
//
// Build: g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_bounded_bar_ring.cpp -o /tmp/t_ring && /tmp/t_ring

#include "BoundedBarRing.h"

#include <cmath>
#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
bool near(float a, float b, float tol = 1e-4f) { return std::fabs(a - b) < tol; }
}  // namespace

int main() {
    std::printf("BoundedBarRing tests\n");

    // --- Warm-up: not full until `capacity` bars pushed --------------------
    {
        BoundedBarRing ring(3);
        check("not_full_initially", !ring.IsFull());
        ring.OnBarClose(10.0f, 8.0f);
        check("not_full_after_1", !ring.IsFull());
        ring.OnBarClose(12.0f, 9.0f);
        check("not_full_after_2", !ring.IsFull());
        ring.OnBarClose(11.0f, 7.0f);
        check("full_after_3", ring.IsFull());
        check("count_is_3", ring.Count() == 3);
    }

    // --- Chronological access + aggregates, before and after eviction ------
    {
        BoundedBarRing ring(3);
        ring.OnBarClose(10.0f, 8.0f);   // bar1
        ring.OnBarClose(12.0f, 9.0f);   // bar2
        ring.OnBarClose(11.0f, 7.0f);   // bar3
        check("chrono0_is_bar1_high", near(ring.HighAt(0), 10.0f));
        check("chrono1_is_bar2_high", near(ring.HighAt(1), 12.0f));
        check("chrono2_is_bar3_high", near(ring.HighAt(2), 11.0f));
        check("range_at_1_is_3", near(ring.RangeAt(1), 3.0f));
        check("highest_high_is_12", near(ring.HighestHigh(), 12.0f));
        check("lowest_low_is_7", near(ring.LowestLow(), 7.0f));

        // Push a 4th bar -- evicts bar1 (oldest); chronological order shifts.
        ring.OnBarClose(15.0f, 6.0f);   // bar4
        check("still_full_after_eviction", ring.IsFull());
        check("chrono0_is_now_bar2", near(ring.HighAt(0), 12.0f));
        check("chrono1_is_now_bar3", near(ring.HighAt(1), 11.0f));
        check("chrono2_is_now_bar4", near(ring.HighAt(2), 15.0f));
        check("highest_high_after_eviction_is_15", near(ring.HighestHigh(), 15.0f));
        check("lowest_low_after_eviction_is_6", near(ring.LowestLow(), 6.0f));
    }

    // --- NR7-style usage: is the current bar's range the narrowest in the
    // retained window? --------------------------------------------------
    {
        BoundedBarRing ring(7);
        const float highs[] = {10, 11, 12, 10, 13, 11, 12};
        const float lows[]  = {8,  9,  9,  8,  10, 9,  9};
        for (int i = 0; i < 7; ++i) ring.OnBarClose(highs[i], lows[i]);
        check("nr7_ring_full", ring.IsFull());

        const float currentRange = 1.0f;  // narrower than every stored bar's range (>= 2 each)
        bool isNarrowest = true;
        for (int i = 0; i < ring.Count(); ++i) {
            if (ring.RangeAt(i) <= currentRange) { isNarrowest = false; break; }
        }
        check("nr7_current_bar_is_narrowest", isNarrowest);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
