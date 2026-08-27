// Unit tests for RecurrenceRateEngine (tick-delta incremental RQA recurrence
// rate, O(n) per tick / O(n^2) only on bar close).
//
// Build & run natively (no Sierra Chart deps, header-only):
//   g++ -std=c++17 -I include tests/cpp/test_recurrence_rate_engine.cpp -o /tmp/rre_test && /tmp/rre_test

#include "RecurrenceRateEngine.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}

// Verbatim brute-force reference matching src/StudyHelperFunctions.cpp's
// CalculateRecurrenceRate measurement: full n*n matrix, LOI included (diagonal
// seeded with n), strict `dist < epsilon`, symmetric pairs counted twice.
// `prices[0]` is treated as the live point, prices[1..n) as closed bars --
// matching production's `sc.BaseData[SC_LAST][sc.Index - i]` convention
// where i=0 is the live/current bar.
double BruteForceRR(const std::vector<float>& prices, double epsilon) {
    const int n = static_cast<int>(prices.size());
    int recurCount = n;
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            if (std::fabs(prices[static_cast<std::size_t>(i)] - prices[static_cast<std::size_t>(j)]) < epsilon) {
                recurCount += 2;
            }
        }
    }
    return static_cast<double>(recurCount) / (static_cast<double>(n) * static_cast<double>(n));
}

// Deterministic pseudo-random walk (no <random>), same LCG convention as
// test_rqa_epsilon.cpp's MakeWalk, for byte-identical fixtures across toolchains.
std::vector<float> MakeWalk(int n, double sigma, uint32_t seed) {
    std::vector<float> p(static_cast<std::size_t>(n));
    uint32_t s = seed;
    double price = 100.0;
    for (int i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        const double u = static_cast<double>(s >> 8) / 16777216.0;
        price += sigma * (2.0 * u - 1.0);
        p[static_cast<std::size_t>(i)] = static_cast<float>(price);
    }
    return p;
}
}  // namespace

int main() {
    {
        RecurrenceRateEngine engine;
        check("fresh engine has zero closed bars", engine.GetClosedCount() == 0);
    }
    {
        // n=30 window: prices[0]=live, prices[1..29]=closed. Cross-check the
        // incremental engine's ComputeRate against the brute-force n*n matrix
        // across several live-price values within the same closed-bar window
        // (mirrors intra-bar ticks: closed bars fixed, only the live price moves).
        const auto walk = MakeWalk(30, 0.5, 12345u);
        const std::vector<float> closed(walk.begin() + 1, walk.end());  // 29 closed bars
        const double epsilon = 0.7;

        RecurrenceRateEngine engine;
        engine.RebuildClosedBarWindow(closed.data(), static_cast<int>(closed.size()), static_cast<float>(epsilon));

        for (float livePrice : {walk[0], walk[0] + 0.2f, walk[0] - 1.3f, walk[5], walk[10] + 0.05f}) {
            std::vector<float> fullWindow;
            fullWindow.push_back(livePrice);
            fullWindow.insert(fullWindow.end(), closed.begin(), closed.end());

            const double expected = BruteForceRR(fullWindow, epsilon);
            const double actual = engine.ComputeRate(livePrice, static_cast<float>(epsilon));
            check("incremental ComputeRate matches brute-force n*n matrix",
                  std::fabs(expected - actual) < 1e-6);
        }
    }
    {
        // Larger window (n=150, the spec's proposed target) -- same equivalence
        // check, confirming the engine scales past the old kMaxLookback=40 cap.
        const auto walk = MakeWalk(150, 1.2, 999u);
        const std::vector<float> closed(walk.begin() + 1, walk.end());
        const double epsilon = 1.5;

        RecurrenceRateEngine engine;
        engine.RebuildClosedBarWindow(closed.data(), static_cast<int>(closed.size()), static_cast<float>(epsilon));

        std::vector<float> fullWindow;
        fullWindow.push_back(walk[0]);
        fullWindow.insert(fullWindow.end(), closed.begin(), closed.end());
        const double expected = BruteForceRR(fullWindow, epsilon);
        const double actual = engine.ComputeRate(walk[0], static_cast<float>(epsilon));
        check("n=150 window: incremental matches brute-force", std::fabs(expected - actual) < 1e-6);
    }
    {
        // Reset clears closed-bar state.
        RecurrenceRateEngine engine;
        std::vector<float> closed = {1.0f, 2.0f, 3.0f};
        engine.RebuildClosedBarWindow(closed.data(), 3, 0.5f);
        check("window populated before reset", engine.GetClosedCount() == 3);
        engine.Reset();
        check("Reset clears closed-bar count", engine.GetClosedCount() == 0);
    }
    {
        // count exceeding kMaxClosedBars is clamped, not overrun.
        RecurrenceRateEngine engine;
        std::vector<float> big(RecurrenceRateEngine::kMaxClosedBars + 10, 1.0f);
        engine.RebuildClosedBarWindow(big.data(), static_cast<int>(big.size()), 0.5f);
        check("oversized window is clamped to kMaxClosedBars",
              engine.GetClosedCount() == RecurrenceRateEngine::kMaxClosedBars);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
