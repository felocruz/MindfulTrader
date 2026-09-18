// test_wilder_atr_engine.cpp — unit tests for WilderAtrEngine.h
// (docs/superpowers/plans/2026-09-16-market-data-replay-alpha-generator-implementation.md Task 1)
//
// Build: g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_wilder_atr_engine.cpp -o /tmp/t_atr && /tmp/t_atr

#include "WilderAtrEngine.h"

#include <cmath>
#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
bool near(float a, float b, float tol = 1e-2f) { return std::fabs(a - b) < tol; }
}  // namespace

int main() {
    std::printf("WilderAtrEngine tests\n");

    // --- Warm-up: undefined (0.0f) until the seed window fills ----------
    {
        WilderAtr atr(3);
        check("bar1_not_seeded", !atr.IsSeeded());
        check("bar1_value_zero", atr.OnBar(/*h=*/10.0f, /*l=*/8.0f, /*c=*/9.0f) == 0.0f);
        check("bar2_still_warming", atr.OnBar(11.0f, 9.0f, 10.0f) == 0.0f);
    }

    // --- Hand-computed reference sequence, period=3 ----------------------
    // Bar1 H=10 L=8  C=9    (no prevClose)              TR1=2
    // Bar2 H=11 L=9  C=10   prevClose=9                 TR2=max(2,2,0)=2
    // Bar3 H=12 L=10 C=11   prevClose=10                TR3=max(2,2,0)=2
    //   seed ATR3 = (2+2+2)/3 = 2.0
    // Bar4 H=13 L=9  C=12   prevClose=11                TR4=max(4,2,2)=4
    //   ATR4 = 2.0 + (4-2.0)/3 = 2.6667
    // Bar5 H=14 L=13 C=13.5 prevClose=12                TR5=max(1,2,1)=2
    //   ATR5 = 2.6667 + (2-2.6667)/3 = 2.4444
    {
        WilderAtr atr(3);
        atr.OnBar(10.0f, 8.0f, 9.0f);
        atr.OnBar(11.0f, 9.0f, 10.0f);
        float seeded = atr.OnBar(12.0f, 10.0f, 11.0f);
        check("seed_atr_is_2_0", near(seeded, 2.0f));
        check("is_seeded_after_period_bars", atr.IsSeeded());

        float atr4 = atr.OnBar(13.0f, 9.0f, 12.0f);
        check("atr4_is_2_6667", near(atr4, 2.6667f));

        float atr5 = atr.OnBar(14.0f, 13.0f, 13.5f);
        check("atr5_is_2_4444", near(atr5, 2.4444f));

        check("value_matches_last_onbar_return", near(atr.Value(), atr5));
    }

    // --- Zero-range bar (high==low==close, no movement) doesn't corrupt state
    {
        WilderAtr atr(2);
        atr.OnBar(100.0f, 100.0f, 100.0f);
        float seeded = atr.OnBar(100.0f, 100.0f, 100.0f);
        check("flat_bars_produce_zero_atr", near(seeded, 0.0f));
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
