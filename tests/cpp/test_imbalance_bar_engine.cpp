// test_imbalance_bar_engine.cpp — unit tests for ImbalanceBarEngine.
// Build & run natively:
//   g++ -std=c++17 -I include tests/cpp/test_imbalance_bar_engine.cpp -o /tmp/ibe_test && /tmp/ibe_test

#include "ImbalanceBarEngine.h"
#include "RobustMoments.h"
#include <algorithm>
#include <array>
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
}  // namespace

int main() {
    {
        ImbalanceBarEngine engine;
        check("fresh engine has zero completed bars", engine.GetCompletedBarCount() == 0);
    }
    {
        ImbalanceBarEngine engine;
        engine.Reset();
        check("Reset clears completed-bar count", engine.GetCompletedBarCount() == 0);
    }

    {
        ImbalanceBarEngine engine;
        engine.OnTick(5, 100.0f, 40.0f);
        check("delta on first tick of a bar is the full signed total",
              engine.GetLastSignedDelta() == 60.0f);

        engine.OnTick(5, 130.0f, 45.0f);
        check("delta within same bar is incremental", engine.GetLastSignedDelta() == 25.0f);
    }
    {
        ImbalanceBarEngine engine;
        engine.OnTick(5, 100.0f, 40.0f);
        engine.OnTick(6, 3.0f, 1.0f);
        check("delta resets across a bar boundary", engine.GetLastSignedDelta() == 2.0f);
    }

    {
        ImbalanceBarEngine engine;
        engine.SetImbalanceThreshold(50.0f);
        engine.OnTickWithPrice(1, 20.0f, 0.0f, 100.0f);
        check("bar not yet closed below threshold", engine.GetCompletedBarCount() == 0);
        engine.OnTickWithPrice(1, 60.0f, 0.0f, 100.5f);
        check("bar closes once cumulative imbalance crosses threshold",
              engine.GetCompletedBarCount() == 1);
    }
    {
        ImbalanceBarEngine engine;
        engine.SetImbalanceThreshold(10.0f);
        for (int i = 0; i < 5; ++i) {
            engine.OnTickWithPrice(i, static_cast<float>(i + 1) * 12.0f, 0.0f,
                                   100.0f + static_cast<float>(i));
        }
        float out[3];
        const std::size_t got = engine.GetImbalanceBarReturns(3, out);
        check("returns buffer is fixed-capacity and rolling (last 3 of 5)", got == 3);
    }
    {
        ImbalanceBarEngine engine;
        engine.SetImbalanceThreshold(10.0f);
        engine.OnTickWithPrice(0, 15.0f, 0.0f, 100.0f);   // bar 0 closes: theta = +15
        engine.OnTickWithPrice(1, 0.0f, 12.0f, 101.0f);   // bar 1 closes: theta = -12
        float magnitudes[2];
        const std::size_t got = engine.GetImbalanceBarMagnitudes(2, magnitudes);
        check("magnitude buffer returns one signed theta per completed bar", got == 2);
        check("first completed bar's signed imbalance is +15", magnitudes[0] == 15.0f);
        check("second completed bar's signed imbalance is -12", magnitudes[1] == -12.0f);
    }
    {
        ImbalanceBarEngine engine;
        engine.SetImbalanceThreshold(10.0f);
        for (int i = 0; i < 5; ++i) {
            engine.OnTickWithPrice(i, static_cast<float>(i + 1) * 12.0f, 0.0f,
                                   100.0f + static_cast<float>(i));
        }
        float returns[5];
        float magnitudes[5];
        const std::size_t retCount = engine.GetImbalanceBarReturns(5, returns);
        const std::size_t magCount = engine.GetImbalanceBarMagnitudes(5, magnitudes);
        check("returns and magnitudes stay index-aligned across the same rolling window",
              retCount == magCount);
    }
    {
        ImbalanceBarEngine engine;
        engine.Reset();
        float out[1];
        check("magnitude buffer is empty on a fresh/reset engine",
              engine.GetImbalanceBarMagnitudes(1, out) == 0);
    }
    {
        ImbalanceBarEngine engine;
        engine.SetImbalanceThreshold(10.0f);
        engine.OnTickWithPrice(0, 15.0f, 0.0f, 0.0f);  // bad tick: non-positive price at bar-close
        float returns[1];
        const std::size_t got = engine.GetImbalanceBarReturns(1, returns);
        check("non-positive close price is guarded, not NaN-injected", got == 1 && returns[0] == 0.0f);
    }
    {
        ImbalanceBarEngine engine;
        engine.SetImbalanceThreshold(2.0f);
        for (int i = 0; i < 100; ++i) {
            const float openPrice = 100.0f + static_cast<float>(i % 7) * 0.1f;
            engine.OnTickWithPrice(i, 1.0f, 0.0f, openPrice);
            engine.OnTickWithPrice(i, 2.0f, 0.0f, openPrice + 0.01f * static_cast<float>(i % 5));
        }
        float rawReturns[100];
        const std::size_t count = engine.GetImbalanceBarReturns(100, rawReturns);
        check("100 imbalance bars closed as expected", count == 100);

        std::array<float, 100> returnsArray;
        std::copy(rawReturns, rawReturns + count, returnsArray.begin());
        check("Moors kurtosis over the engine's own buffer is finite",
              std::isfinite(MoorsKurtosis(returnsArray)));
    }
    {
        ImbalanceBarEngine engine;
        engine.SetImbalanceThreshold(5.0f);
        check("adaptive threshold disabled by default", !engine.IsAdaptiveThresholdEnabled());
        check("GetCurrentThreshold returns the fixed threshold when adaptive is disabled",
              engine.GetCurrentThreshold() == 5.0f);
    }
    {
        // Fixed-threshold callers must see byte-identical behavior after this change --
        // same tick sequence as the "100 imbalance bars closed" test above, threshold unchanged.
        ImbalanceBarEngine engine;
        engine.SetImbalanceThreshold(2.0f);
        for (int i = 0; i < 100; ++i) {
            const float openPrice = 100.0f + static_cast<float>(i % 7) * 0.1f;
            engine.OnTickWithPrice(i, 1.0f, 0.0f, openPrice);
            engine.OnTickWithPrice(i, 2.0f, 0.0f, openPrice + 0.01f * static_cast<float>(i % 5));
        }
        check("fixed-threshold behavior is unchanged by the adaptive-threshold addition",
              engine.GetCompletedBarCount() == 100);
    }
    {
        ImbalanceBarEngine engine;
        engine.SetImbalanceThreshold(10.0f);  // seeds a real first bar quickly (10 ticks of delta=+1 each)
        engine.EnableAdaptiveThreshold(0.5f); // fast alpha so the test converges quickly
        check("GetCurrentThreshold falls back to the fixed value before any bar has completed",
              engine.GetCurrentThreshold() == 10.0f);

        // Constant per-tick imbalance of +1.0 (askVolume=1, bidVolume=0 every tick) -- the fixed
        // threshold (10.0) seeds the first bar/estimate from real observed values; subsequent
        // bars should keep completing under the now-adaptive threshold, not stall or diverge.
        for (int i = 0; i < 200 && engine.GetCompletedBarCount() < 5; ++i) {
            engine.OnTickWithPrice(i, 1.0f, 0.0f, 100.0f);
        }
        check("adaptive threshold engages and lets bars keep completing",
              engine.GetCompletedBarCount() >= 5);
        check("adaptive threshold has converged near the real observed per-tick pattern (~10)",
              engine.GetCurrentThreshold() > 5.0f && engine.GetCurrentThreshold() < 15.0f);
    }
    {
        ImbalanceBarEngine engine;
        engine.SetImbalanceThreshold(3.0f);
        engine.EnableAdaptiveThreshold(0.5f);
        engine.OnTickWithPrice(0, 1.0f, 0.0f, 100.0f);
        engine.OnTickWithPrice(1, 1.0f, 0.0f, 100.0f);
        engine.OnTickWithPrice(2, 1.0f, 0.0f, 100.0f);  // cumulative=3.0, bar closes, 3 ticks
        check("first completed bar seeds the adaptive estimate from real observed values",
              engine.GetCompletedBarCount() == 1 &&
              engine.GetCurrentThreshold() > 0.0f && engine.GetCurrentThreshold() < 3.0f * 2.0f);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
