// test_daily_bias_engine.cpp — unit tests for the pure Daily Bias classification core.
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I include tests/cpp/test_daily_bias_engine.cpp -o /tmp/dbe_test && /tmp/dbe_test

#include "DailyBiasEngine.h"

#include <cstdio>

using namespace dbe;

namespace {

int g_failures = 0;

void check(const char* name, Bias got, Bias expected) {
    if (got == expected) {
        std::printf("  PASS  %s\n", name);
    } else {
        ++g_failures;
        std::printf("  FAIL  %s  got=%d exp=%d\n", name,
                     static_cast<int>(got), static_cast<int>(expected));
    }
}

}  // namespace

int main() {
    std::printf("DailyBiasEngine unit tests\n");

    // Zone A: breakout above prevDayHigh, Hurst trending -> persistent
    check("breakout_up_trending_persistent",
          ComputeDailyBias({5150.0f, 5100.0f, 5000.0f, 0.6f}),
          Bias::BULLISH_TREND_PERSISTENT);

    // Zone A: breakout above prevDayHigh, Hurst mean-reverting -> trap
    check("breakout_up_mean_reverting_trap",
          ComputeDailyBias({5150.0f, 5100.0f, 5000.0f, 0.3f}),
          Bias::BEARISH_VOLATILITY_TRAP);

    // Zone B: breakdown below prevDayLow, Hurst trending -> persistent
    check("breakdown_trending_persistent",
          ComputeDailyBias({4950.0f, 5100.0f, 5000.0f, 0.6f}),
          Bias::BEARISH_TREND_PERSISTENT);

    // Physics veto: Hurst ~0.5 -> random walk veto regardless of price
    check("hurst_random_walk_veto",
          ComputeDailyBias({5150.0f, 5100.0f, 5000.0f, 0.5f}),
          Bias::PHYSICS_VETO_RANDOM_WALK);

    // Invalid prevDay data -> random walk veto
    check("invalid_prev_day_veto",
          ComputeDailyBias({5020.0f, 0.0f, 0.0f, 0.6f}),
          Bias::PHYSICS_VETO_RANDOM_WALK);

    // No real Value Area supplied (defaults 0.0f) -> falls back to the
    // 15%/85% range-split proxy. range=100, proxy val=5015, vah=5085.
    // lastPrice=5020 is between val and vah -> Zone E rotation under the proxy.
    check("fallback_proxy_rotation_when_no_real_value_area",
          ComputeDailyBias({5020.0f, 5100.0f, 5000.0f, 0.6f}),
          Bias::VALUE_AREA_ROTATION);

    // Real Value Area supplied and DIFFERENT from the proxy: real VAL=5022 >
    // proxy VAL=5015, moving lastPrice=5020 from "rotation" (under the proxy)
    // into "bullish mean reversion" (under the real, volume-weighted VAL).
    // This is the behavior this task exists to change.
    check("real_value_area_overrides_proxy_classification",
          ComputeDailyBias({5020.0f, 5100.0f, 5000.0f, 0.6f, /*valueAreaLow=*/5022.0f, /*valueAreaHigh=*/5085.0f}),
          Bias::BULLISH_MEAN_REVERSION);

    // Only one of the two real bounds supplied -> still falls back to proxy
    // (haveRealValueArea requires BOTH bounds to avoid a mismatched pair).
    check("partial_real_value_area_still_falls_back",
          ComputeDailyBias({5020.0f, 5100.0f, 5000.0f, 0.6f, /*valueAreaLow=*/5022.0f, /*valueAreaHigh=*/0.0f}),
          Bias::VALUE_AREA_ROTATION);

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
