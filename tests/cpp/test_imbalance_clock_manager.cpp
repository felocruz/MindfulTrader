// test_imbalance_clock_manager.cpp — unit tests for ImbalanceClockManager.
// Build & run natively:
//   g++ -std=c++17 -I include tests/cpp/test_imbalance_clock_manager.cpp -o /tmp/icm_test && /tmp/icm_test

#include "ImbalanceClockManager.h"
#include <cmath>
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

bool nearlyEqual(float a, float b, float eps = 1e-4f) {
    return std::abs(a - b) <= eps;
}
}  // namespace

int main() {
    {
        ImbalanceClockManager mgr;
        check("fresh manager has zero completed bars at all 3 levels",
              mgr.GetIs3CompletedBarCount() == 0 &&
              mgr.GetIs2CompletedBarCount() == 0 &&
              mgr.GetIs1CompletedBarCount() == 0);
    }

    {
        ImbalanceClockManager mgr;
        mgr.ConfigureIs3Threshold(10.0f);
        mgr.SetAggregationCounts(/*k2=*/2, /*k1=*/2);

        // First IS3 bar: open at 100, tick closes it with a single large imbalance burst.
        mgr.OnTick(0, 11.0f, 0.0f, 100.0f);
        check("one completed IS3 bar closes on threshold crossing", mgr.GetIs3CompletedBarCount() == 1);
        check("IS2/IS1 have not completed yet (K2=2 needs a second IS3 bar)",
              mgr.GetIs2CompletedBarCount() == 0 && mgr.GetIs1CompletedBarCount() == 0);

        // Second IS3 bar on a new bar index -- completes IS2's first bar (K2=2).
        mgr.OnTick(1, 11.0f, 0.0f, 100.0f);
        check("two completed IS3 bars", mgr.GetIs3CompletedBarCount() == 2);
        check("one completed IS2 bar after K2=2 IS3 bars", mgr.GetIs2CompletedBarCount() == 1);
        check("IS1 still not completed (K1=2 needs a second IS2 bar)", mgr.GetIs1CompletedBarCount() == 0);

        // Two more IS3 bars -> second IS2 bar -> first IS1 bar (K1=2).
        mgr.OnTick(2, 11.0f, 0.0f, 100.0f);
        mgr.OnTick(3, 11.0f, 0.0f, 100.0f);
        check("four completed IS3 bars", mgr.GetIs3CompletedBarCount() == 4);
        check("two completed IS2 bars", mgr.GetIs2CompletedBarCount() == 2);
        check("one completed IS1 bar after K1=2 IS2 bars", mgr.GetIs1CompletedBarCount() == 1);
    }

    {
        // Exact-sum property: an IS2 bar's return must equal the sum of its K2 constituent
        // IS3 bar returns (log(P_end/P_start) telescopes exactly under summed log-returns).
        ImbalanceClockManager mgr;
        mgr.ConfigureIs3Threshold(10.0f);
        mgr.SetAggregationCounts(/*k2=*/3, /*k1=*/99);

        // Each IS3 bar uses 2 ticks with CUMULATIVE ask volume (matching ImbalanceBarEngine's
        // own contract): first tick (below threshold) opens the bar; second tick (cumulative
        // total crossing the 10.0 threshold) closes it at a different price, giving a genuinely
        // non-zero per-bar return instead of a same-tick open==close no-op.
        float opens[3]  = {100.0f, 105.0f,  98.0f};
        float closes[3] = {105.0f,  98.0f, 102.0f};
        float expectedIs3Returns[3];
        for (int i = 0; i < 3; ++i) {
            expectedIs3Returns[i] = std::log(closes[i] / opens[i]);
            mgr.OnTick(i, 3.0f, 0.0f, opens[i]);    // cumulative imbalance 3 < 10, opens the bar
            mgr.OnTick(i, 11.0f, 0.0f, closes[i]);  // cumulative imbalance 11 >= 10, closes it
        }
        check("three IS3 bars completed", mgr.GetIs3CompletedBarCount() == 3);
        check("one IS2 bar completed (K2=3)", mgr.GetIs2CompletedBarCount() == 1);

        float is3Returns[3];
        mgr.GetIs3Returns(3, is3Returns);
        float is2Returns[1];
        mgr.GetIs2Returns(1, is2Returns);

        const float expectedIs2Return = expectedIs3Returns[0] + expectedIs3Returns[1] + expectedIs3Returns[2];
        check("IS2 bar return equals the sum of its 3 constituent IS3 returns",
              nearlyEqual(is2Returns[0], expectedIs2Return));
    }

    {
        ImbalanceClockManager mgr;
        mgr.ConfigureIs3Threshold(10.0f);
        mgr.SetAggregationCounts(2, 2);
        mgr.OnTick(0, 11.0f, 0.0f, 100.0f);
        mgr.OnTick(1, 11.0f, 0.0f, 100.0f);
        mgr.Reset();
        check("Reset clears completed-bar counts at all 3 levels",
              mgr.GetIs3CompletedBarCount() == 0 &&
              mgr.GetIs2CompletedBarCount() == 0 &&
              mgr.GetIs1CompletedBarCount() == 0);
    }

    {
        ImbalanceClockManager mgr;
        mgr.SetAggregationCounts(0, 0);
        // Guard against a degenerate K of 0 causing an infinite/zero-progress aggregation loop --
        // treated as K=1 (every completed finer bar immediately becomes one coarser bar).
        mgr.ConfigureIs3Threshold(10.0f);
        mgr.OnTick(0, 11.0f, 0.0f, 100.0f);
        check("K2/K1=0 is guarded up to 1, not left at a degenerate zero",
              mgr.GetIs2CompletedBarCount() == 1 && mgr.GetIs1CompletedBarCount() == 1);
    }

    {
        ImbalanceClockManager mgr;
        check("Instance() returns the same singleton on repeated calls",
              &ImbalanceClockManager::Instance() == &ImbalanceClockManager::Instance());
        (void)mgr;
    }

    if (g_failures == 0) {
        std::printf("ALL PASS\n");
        return 0;
    }
    std::printf("%d FAILURE(S)\n", g_failures);
    return 1;
}
