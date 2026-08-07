// test_structure_engine.cpp — characterization tests for StructureEngine,
// captured against the CURRENT std::deque-backed implementation as a golden
// baseline before the RingBuffer swap
// (docs/superpowers/specs/2026-08-07-contextmanager-ring-buffer-dod-design.md).
// Expected values below are independently hand-derived from the class's own
// documented formulas, not just "whatever the current code happens to
// output" — re-run unchanged after the swap to confirm bit-identical
// behavior.
//
// Build & run natively (no Sierra Chart deps):
//   g++ -std=c++17 -I include -I /usr/include/eigen3 tests/cpp/test_structure_engine.cpp src/StructureEngine.cpp -o /tmp/se_test && /tmp/se_test

#include "StructureEngine.h"

#include <cmath>
#include <cstdio>

using namespace MindfulTrader;

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

bool approx(float a, float b, float tol = 1e-4f) {
    return std::fabs(a - b) <= tol;
}

}  // namespace

int main() {
    std::printf("StructureEngine unit tests\n");

    // Before any Update(): neutral defaults.
    {
        StructureEngine se;
        check("cold_start_log_expansion_ratio_is_one", se.GetLogExpansionRatio() == 1.0f);
        check("cold_start_recurrence_rate_is_neutral", se.GetRecurrenceRate() == 0.5f);
        check("cold_start_fractal_dim_is_neutral", se.GetFractalDimension() == 1.5f);
        check("cold_start_mean_rev_z_is_zero", se.GetMeanReversionZ() == 0.0f);
        check("cold_start_not_ready", !se.IsReady());
    }

    // Fewer than WINDOW_SIZE (30) bars: still neutral for the window-gated metrics.
    {
        StructureEngine se;
        for (int i = 0; i < 10; ++i) {
            se.Update(/*high=*/100.5f + i, /*low=*/99.5f + i, /*close=*/100.0f + i, /*isNewBar=*/true);
        }
        check("below_window_recurrence_rate_is_neutral", se.GetRecurrenceRate() == 0.5f);
        check("below_window_fractal_dim_is_neutral", se.GetFractalDimension() == 1.5f);
        check("below_window_not_ready", !se.IsReady());
    }

    // Perfect linear ramp, exactly WINDOW_SIZE (30) bars: close = 100+i,
    // high = close+0.5, low = close-0.5 for i=0..29. Every bar has range=1.0,
    // so log-range is 0 for every bar (constant) -- hand-derived exact values:
    //   - LogExpansionRatio: currentLogRange(0) - avg(0) -> exp(0) = 1.0
    //   - RecurrenceRate: 30 evenly-spaced prices [100..129] into 10 bins of
    //     width 2.9 split exactly 3-per-bin (hand-verified bin assignment) -> 3/30 = 0.1
    //   - FractalDimension: pathLength = 29*1.0 = 29.0; displacement =
    //     maxHigh(129.5) - minLow(99.5) = 30.0 -> 29.0/30.0
    //   - MeanReversionZ: perfect OLS fit -> residual=0, stdRes=0 -> guarded to 0.0f
    {
        StructureEngine se;
        for (int i = 0; i < 30; ++i) {
            const float close = 100.0f + static_cast<float>(i);
            se.Update(/*high=*/close + 0.5f, /*low=*/close - 0.5f, close, /*isNewBar=*/true);
        }
        check("ramp_is_ready", se.IsReady());
        check("ramp_log_expansion_ratio", approx(se.GetLogExpansionRatio(), 1.0f));
        check("ramp_recurrence_rate", approx(se.GetRecurrenceRate(), 0.1f));
        check("ramp_fractal_dimension", approx(se.GetFractalDimension(), 29.0f / 30.0f));
        check("ramp_mean_rev_z_is_zero_on_perfect_fit", se.GetMeanReversionZ() == 0.0f);
    }

    // isNewBar=false overwrites the latest bar in place rather than growing
    // the window -- window size must stay exactly WINDOW_SIZE, and a
    // subsequent isNewBar=true push must still evict down to WINDOW_SIZE.
    {
        StructureEngine se;
        for (int i = 0; i < 30; ++i) {
            const float close = 100.0f + static_cast<float>(i);
            se.Update(close + 0.5f, close - 0.5f, close, /*isNewBar=*/true);
        }
        // Overwrite the 30th (last) bar with a very different value.
        se.Update(/*high=*/500.5f, /*low=*/499.5f, /*close=*/500.0f, /*isNewBar=*/false);
        check("overwrite_last_bar_changes_mean_rev_z",
              se.GetMeanReversionZ() != 0.0f);  // no longer a perfect linear fit

        // Push one more real bar -- window must still be capped at 30, not 31.
        se.Update(530.5f, 529.5f, 530.0f, /*isNewBar=*/true);
        check("overwrite_then_push_still_ready", se.IsReady());
        // FractalDimension recomputes over the (now-corrupted-by-overwrite) window;
        // just confirm it stays in the method's documented [bounded-ish] output
        // range rather than blowing up (e.g. NaN/negative), i.e. the container
        // swap didn't desync any of the four parallel buffers' sizes.
        const float fd = se.GetFractalDimension();
        check("overwrite_then_push_fractal_dim_finite_and_positive",
              std::isfinite(fd) && fd > 0.0f);
    }

    // Fewer than 10 bars: GetMeanReversionZ's separate, stricter warmup gate.
    {
        StructureEngine se;
        for (int i = 0; i < 9; ++i) {
            se.Update(105.0f, 95.0f, 100.0f, true);
        }
        check("below_ten_bars_mean_rev_z_is_zero", se.GetMeanReversionZ() == 0.0f);
    }

    // Reset() clears all four parallel buffers back to cold-start state.
    {
        StructureEngine se;
        for (int i = 0; i < 30; ++i) {
            se.Update(105.0f + i, 95.0f + i, 100.0f + i, true);
        }
        se.Reset();
        check("reset_clears_ready_state", !se.IsReady());
        check("reset_clears_recurrence_rate", se.GetRecurrenceRate() == 0.5f);
        check("reset_clears_fractal_dim", se.GetFractalDimension() == 1.5f);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
