// test_task5_bar_history_wiring.cpp — smoke test confirming Task 5's
// NR7/TurtleSoup bounded history rings are correctly wired into
// MarketDataReplayEngine's OnTs3BarClose() (not a full pattern-detector
// test -- that's Task 7).
//
// Build: mamba run -n mts g++ -std=c++17 -Wall -Wextra -Iinclude -Itools/market_data_replay \
//   -I$(mamba run -n mts python -c "import nlohmann" 2>/dev/null; echo include) \
//   tools/market_data_replay/test_task5_bar_history_wiring.cpp -o /tmp/t_task5 && /tmp/t_task5

#include "MarketDataReplayEngine.h"

#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}  // namespace

int main() {
    std::printf("Task 5 bar-history wiring smoke test\n");

    MarketDataReplayEngine engine;
    check("nr7_ring_empty_initially", engine.GetNr7Ring().Count() == 0);
    check("turtlesoup_ring_empty_initially", engine.GetTurtleSoupRing().Count() == 0);

    // Feed ticks spanning several TS3 (15-min) bars -- session-anchored at
    // 18:00 ET, so start well after that to avoid edge effects.
    int64_t t = 1000000000000000LL;  // arbitrary epoch-us anchor, mid-session
    double price = 100.0;
    for (int bar = 0; bar < 9; ++bar) {
        for (int tick = 0; tick < 5; ++tick) {
            engine.OnTick(t, price + tick, 1, 1, 1);
            t += 60'000'000LL;  // 60s apart, 5 ticks/bar < 15 min bar period
        }
        t += 15 * 60 * 1'000'000LL;  // jump to next bar
        price += 1.0;
    }
    engine.Flush();

    check("ts3_bars_closed_at_least_8", engine.GetTs3BarsClosed() >= 8);
    check("nr7_ring_reflects_bars_closed",
          engine.GetNr7Ring().Count() == std::min(7, engine.GetTs3BarsClosed()));
    check("turtlesoup_ring_reflects_bars_closed",
          engine.GetTurtleSoupRing().Count() == std::min(20, engine.GetTs3BarsClosed()));
    check("nr7_ring_full_after_8_bars", engine.GetNr7Ring().IsFull());
    check("turtlesoup_ring_not_yet_full_after_8_bars", !engine.GetTurtleSoupRing().IsFull());

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
