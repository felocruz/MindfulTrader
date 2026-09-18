// test_ema_engine.cpp — unit tests for EmaEngine.h
// (docs/superpowers/plans/2026-09-16-market-data-replay-alpha-generator-implementation.md Task 4)
//
// Build: g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_ema_engine.cpp -o /tmp/t_ema && /tmp/t_ema

#include "EmaEngine.h"

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
    std::printf("EmaEngine tests\n");

    // --- Ema(period=2): seeds on first value, alpha=2/3 thereafter ---------
    {
        Ema ema(2);
        check("ema_seeds_on_first_value", near(ema.OnValue(10.0f), 10.0f));
        check("ema_is_seeded", ema.IsSeeded());
        check("ema_bar2_is_11_3333", near(ema.OnValue(12.0f), 11.3333f));
        check("ema_bar3_is_11_1111", near(ema.OnValue(11.0f), 11.1111f));
    }

    // --- MacdEngine(fast=1, slow=2, signal=2) — hand-computed sequence ------
    // Bar1 v=10: fast=10,slow=10 -> line=0; signal seeds=0 -> histogram=0
    // Bar2 v=12: fast=12,slow=11.3333 -> line=0.6667; signal=0.4444 -> hist=0.2222
    // Bar3 v=11: fast=11,slow=11.1111 -> line=-0.1111; signal=0.0741 -> hist=-0.1852
    {
        MacdEngine macd(1, 2, 2);
        float h1 = macd.OnValue(10.0f);
        check("macd_bar1_histogram_zero", near(h1, 0.0f));
        float h2 = macd.OnValue(12.0f);
        check("macd_bar2_line_is_0_6667", near(macd.Line(), 0.6667f));
        check("macd_bar2_signal_is_0_4444", near(macd.Signal(), 0.4444f));
        check("macd_bar2_histogram_is_0_2222", near(h2, 0.2222f));
        float h3 = macd.OnValue(11.0f);
        check("macd_bar3_line_is_neg_0_1111", near(macd.Line(), -0.1111f));
        check("macd_bar3_histogram_is_neg_0_1852", near(h3, -0.1852f));
    }

    // --- Elder Impulse color classification (transcribed from the real
    // GetImpulse(), src/StudyHelperFunctions.cpp:671-682) -------------------
    {
        check("both_positive_is_green", GetImpulseColorLike(0.5f, 0.1f) == kImpulseGreen);
        check("both_negative_is_red", GetImpulseColorLike(-0.5f, -0.1f) == kImpulseRed);
        check("mixed_signs_is_blue_pos_neg", GetImpulseColorLike(0.5f, -0.1f) == kImpulseBlue);
        check("mixed_signs_is_blue_neg_pos", GetImpulseColorLike(-0.5f, 0.1f) == kImpulseBlue);
        check("zero_zero_is_blue", GetImpulseColorLike(0.0f, 0.0f) == kImpulseBlue);
    }

    // --- Sanity: EMA converges to a constant input after enough bars -------
    {
        Ema ema(10);
        float last = 0.0f;
        for (int i = 0; i < 200; ++i) last = ema.OnValue(50.0f);
        check("ema_converges_to_constant_input", near(last, 50.0f, 1e-3f));
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
