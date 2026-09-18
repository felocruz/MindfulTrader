// test_rsi_engine.cpp — unit tests for RsiEngine.h
// (docs/superpowers/plans/2026-09-16-market-data-replay-alpha-generator-implementation.md Task 2)
//
// Build: g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_rsi_engine.cpp -o /tmp/t_rsi && /tmp/t_rsi

#include "RsiEngine.h"

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
    std::printf("RsiEngine tests\n");

    // --- Wilder's mode, period=2 (matches IndicatorKey::RSI's real config) --
    // Closes: 10, 11, 10, 12, 11.5
    // Bar1 c=10: setup only -> 0
    // Bar2 c=11: gain=1,loss=0 -> seedCount=1<2 -> 0
    // Bar3 c=10: gain=0,loss=1 -> seedCount=2==2 -> avgGain=0.5,avgLoss=0.5 -> RSI=50
    // Bar4 c=12: gain=2,loss=0 -> avgGain=0.5+(2-0.5)/2=1.25, avgLoss=0.5+(0-0.5)/2=0.25 -> RS=5 -> RSI=83.333
    // Bar5 c=11.5: gain=0,loss=0.5 -> avgGain=1.25+(0-1.25)/2=0.625, avgLoss=0.25+(0.5-0.25)/2=0.375 -> RS=1.6667 -> RSI=62.5
    {
        RsiEngine rsi(2, RsiSmoothing::WILDERS);
        check("wilders_bar1_zero", rsi.OnClose(10.0f) == 0.0f);
        check("wilders_bar2_still_seeding", rsi.OnClose(11.0f) == 0.0f);
        float bar3 = rsi.OnClose(10.0f);
        check("wilders_bar3_is_50", near(bar3, 50.0f));
        check("wilders_seeded_at_bar3", rsi.IsSeeded());
        float bar4 = rsi.OnClose(12.0f);
        check("wilders_bar4_is_83_333", near(bar4, 83.333f, 0.05f));
        float bar5 = rsi.OnClose(11.5f);
        check("wilders_bar5_is_62_5", near(bar5, 62.5f, 0.05f));
    }

    // --- Cutler's (SIMPLE) mode, period=2 (matches DetectMomentumPinball's
    // rsi3/rsi10 config: sc.RSI(..., MOVAVGTYPE_SIMPLE, N)) -----------------
    // Closes: 10, 12, 11, 13, 11
    // Bar4 (first full window over bars 3,4): avgGain=1.0, avgLoss=0.5 -> RSI=66.667
    // Bar5 (window over bars 4,5): avgGain=1.0, avgLoss=1.0 -> RSI=50.0
    {
        RsiEngine rsi(2, RsiSmoothing::SIMPLE);
        rsi.OnClose(10.0f);
        rsi.OnClose(12.0f);
        check("simple_still_warming_at_bar3", rsi.OnClose(11.0f) == 0.0f);
        float bar4 = rsi.OnClose(13.0f);
        check("simple_bar4_is_66_667", near(bar4, 66.667f, 0.05f));
        check("simple_seeded_at_bar4", rsi.IsSeeded());
        float bar5 = rsi.OnClose(11.0f);
        check("simple_bar5_is_50", near(bar5, 50.0f, 0.05f));
    }

    // --- All-gains sequence saturates to RSI=100 (avgLoss==0 guard) --------
    {
        RsiEngine rsi(2, RsiSmoothing::WILDERS);
        rsi.OnClose(10.0f);
        rsi.OnClose(11.0f);
        float bar3 = rsi.OnClose(12.0f);
        check("all_gains_saturates_to_100", near(bar3, 100.0f));
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
