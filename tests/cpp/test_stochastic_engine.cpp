// test_stochastic_engine.cpp — unit tests for StochasticEngine.h
// (docs/superpowers/plans/2026-09-16-market-data-replay-alpha-generator-implementation.md Task 3)
//
// Build: g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_stochastic_engine.cpp -o /tmp/t_stoch && /tmp/t_stoch

#include "StochasticEngine.h"

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
    std::printf("StochasticEngine tests\n");

    // --- SmaWindow: length-2 window produces its first average on the
    // 2nd push (no extra off-by-one delay) ---------------------------------
    {
        SmaWindow w(2);
        check("sma_bar1_warming", w.Push(10.0f) == 0.0f);
        float bar2 = w.Push(20.0f);
        check("sma_bar2_is_avg_of_10_20", near(bar2, 15.0f));
        check("sma_seeded_at_bar2", w.IsSeeded());
        float bar3 = w.Push(30.0f);
        check("sma_bar3_is_avg_of_20_30", near(bar3, 25.0f));
    }

    // --- StochasticEngine(fastK=2, fastD=2, slowD=2) -----------------------
    // Bar1 H=10 L=8  C=9    -> fastK window warming (1/2)         -> 0
    // Bar2 H=11 L=9  C=10.5 -> fastK window full: HH=11,LL=8      -> RawK=100*(10.5-8)/3=83.333
    //                          fastD warming (1/2)                -> OnBar returns 0
    // Bar3 H=10 L=9  C=9.5  -> fastK window (bar2,bar3): HH=11,LL=9 -> RawK=100*(9.5-9)/2=25.0
    //                          fastD full: avg(83.333,25.0)=54.1665 -> OnBar returns 54.1665
    {
        StochasticEngine stoch(2, 2, 2);
        check("stoch_bar1_warming", stoch.OnBar(10.0f, 8.0f, 9.0f) == 0.0f);

        float bar2 = stoch.OnBar(11.0f, 9.0f, 10.5f);
        check("stoch_bar2_fastd_still_warming", bar2 == 0.0f);
        check("stoch_bar2_rawk_is_83_333", near(stoch.RawK(), 83.333f, 0.05f));
        check("stoch_bar2_not_seeded_yet", !stoch.IsSeeded());

        float bar3 = stoch.OnBar(10.0f, 9.0f, 9.5f);
        check("stoch_bar3_rawk_is_25", near(stoch.RawK(), 25.0f));
        check("stoch_bar3_fastd_is_54_1665", near(bar3, 54.1665f, 0.05f));
        check("stoch_bar3_seeded", stoch.IsSeeded());
        check("stoch_value_matches_fastd", near(stoch.Value(), stoch.FastD()));
    }

    // --- Flat range (high==low across the window) doesn't divide by zero --
    {
        StochasticEngine stoch(2, 2, 2);
        stoch.OnBar(100.0f, 100.0f, 100.0f);
        float bar2 = stoch.OnBar(100.0f, 100.0f, 100.0f);
        (void)bar2;
        check("flat_range_rawk_is_neutral_50", near(stoch.RawK(), 50.0f));
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
