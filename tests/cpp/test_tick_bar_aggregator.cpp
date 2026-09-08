// test_tick_bar_aggregator.cpp — characterization tests for
// TickBarAggregator's session-anchored bar-boundary logic, no Sierra
// Chart/ACSIL dependency.
//
// Build & run natively:
//   g++ -std=c++17 -I include tests/cpp/test_tick_bar_aggregator.cpp -o /tmp/tba_test && /tmp/tba_test

#include "TickBarAggregator.h"

#include <cmath>
#include <cstdio>
#include <vector>

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

bool approx(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

constexpr int kCmeEsSessionStartSecondsET = 18 * 3600;  // matches EventDataCollectorStudy.cpp

// 2024-01-15 (winter, EST, UTC-5): 18:00:00 ET = 23:00:00 UTC.
constexpr int64_t kBar1OpenUs = 1705359600LL * 1'000'000LL;       // 18:00:00 ET
constexpr int64_t kBar1MidUs = 1705360050LL * 1'000'000LL;        // 18:07:30 ET
constexpr int64_t kBar1LastTickUs = 1705360499LL * 1'000'000LL;   // 18:14:59 ET
constexpr int64_t kBar2OpenUs = 1705360500LL * 1'000'000LL;       // 18:15:00 ET

}  // namespace

int main() {
    std::printf("TickBarAggregator unit tests\n");

    // Three ticks within the same 15-min bar aggregate into one bar with
    // correct OHLCV; the fourth tick, at the exact next boundary, closes it.
    {
        std::vector<tba::Bar> closedBars;
        tba::TickBarAggregator agg(900, kCmeEsSessionStartSecondsET,
                                    [&](const tba::Bar& b) { closedBars.push_back(b); });

        agg.OnTick(kBar1OpenUs, 100.0, 10, 6, 4);
        agg.OnTick(kBar1MidUs, 105.0, 20, 12, 8);
        agg.OnTick(kBar1LastTickUs, 98.0, 5, 2, 3);
        check("no_bar_closed_yet_within_same_bucket", closedBars.empty());

        agg.OnTick(kBar2OpenUs, 102.0, 7, 4, 3);
        check("bar_closed_exactly_at_next_boundary", closedBars.size() == 1);

        const tba::Bar& bar1 = closedBars[0];
        check("bar1_open_is_first_tick_price", approx(bar1.open, 100.0));
        check("bar1_high_is_max_of_ticks", approx(bar1.high, 105.0));
        check("bar1_low_is_min_of_ticks", approx(bar1.low, 98.0));
        check("bar1_close_is_last_tick_before_boundary", approx(bar1.close, 98.0));
        check("bar1_volume_summed", bar1.volume == 35);
        check("bar1_ask_volume_summed", bar1.askVolume == 20);
        check("bar1_bid_volume_summed", bar1.bidVolume == 15);
        check("bar1_num_trades_is_3", bar1.numTrades == 3);
        check("bar1_open_time_matches_first_tick", bar1.openTimeUs == kBar1OpenUs);
        check("bar1_close_time_matches_last_tick", bar1.closeTimeUs == kBar1LastTickUs);

        agg.Flush();
        check("flush_closes_the_still_open_second_bar", closedBars.size() == 2);
        check("bar2_open_is_boundary_tick_price", approx(closedBars[1].open, 102.0));
    }

    // Flush() on an aggregator that never received a tick is a safe no-op.
    {
        std::vector<tba::Bar> closedBars;
        tba::TickBarAggregator agg(900, kCmeEsSessionStartSecondsET,
                                    [&](const tba::Bar& b) { closedBars.push_back(b); });
        agg.Flush();
        check("flush_with_no_ticks_emits_nothing", closedBars.empty());
    }

    // A different bar period (60 min = TS2) on the same tick stream produces
    // exactly one bar (all 4 ticks fall within the same 18:00-19:00 ET hour).
    {
        std::vector<tba::Bar> closedBars;
        tba::TickBarAggregator agg(3600, kCmeEsSessionStartSecondsET,
                                    [&](const tba::Bar& b) { closedBars.push_back(b); });
        agg.OnTick(kBar1OpenUs, 100.0, 10, 6, 4);
        agg.OnTick(kBar1MidUs, 105.0, 20, 12, 8);
        agg.OnTick(kBar2OpenUs, 102.0, 7, 4, 3);
        agg.Flush();
        check("ts2_60min_period_keeps_all_ticks_in_one_bar", closedBars.size() == 1);
        check("ts2_bar_volume_summed_across_all_ticks", closedBars[0].volume == 37);
    }

    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
