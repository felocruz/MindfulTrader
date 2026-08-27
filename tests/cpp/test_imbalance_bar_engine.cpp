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
        engine.SetImbalanceThresholdForTesting(50.0f);
        engine.OnTickWithPrice(1, 20.0f, 0.0f, 100.0f);
        check("bar not yet closed below threshold", engine.GetCompletedBarCount() == 0);
        engine.OnTickWithPrice(1, 60.0f, 0.0f, 100.5f);
        check("bar closes once cumulative imbalance crosses threshold",
              engine.GetCompletedBarCount() == 1);
    }
    {
        ImbalanceBarEngine engine;
        engine.SetImbalanceThresholdForTesting(10.0f);
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
        engine.SetImbalanceThresholdForTesting(2.0f);
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

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
