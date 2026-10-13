// Unit tests for HillTailIndex (pure Resnick & Stărică 1997 Hill-plot stability-region Hill
// estimator, extracted from TailRiskEngine::ComputeRawAlpha).
//
// Build & run natively (no Sierra Chart deps, header-only):
//   g++ -std=c++17 -I include tests/cpp/test_hill_tail_index.cpp -o /tmp/hti_test && /tmp/hti_test

#include "HillTailIndex.h"
#include "TailRiskEngine.h"

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

// Same LCG convention as rust/observation_vector's own #[cfg(test)] -- deliberately NOT
// std::mt19937/<random> (implementation-defined, not portable across languages).
std::vector<double> MakePositiveNoise(int n, double scale, uint32_t seed) {
    std::vector<double> v(static_cast<std::size_t>(n));
    uint32_t s = seed;
    for (int i = 0; i < n; ++i) {
        s = s * 1664525u + 1013904223u;
        const double u = static_cast<double>(s >> 8) / 16777216.0;
        v[static_cast<std::size_t>(i)] = scale * (0.1 + u);  // strictly positive magnitudes
    }
    return v;
}
}  // namespace

int main() {
    // --- Below the tail_cutoff+1 threshold: nullopt ---
    {
        auto window = MakePositiveNoise(20, 0.01, 1u);
        check("count below tail_cutoff+1 returns nullopt",
              !HillTailIndex(window.data(), 20, 25).has_value());
    }

    // --- Exactly at the real production config (windowSize=500, tailPercent=0.05 -> cutoff=25):
    // a real estimate should come back, finite and within the documented sane Hill-alpha range ---
    {
        auto window = MakePositiveNoise(500, 0.001, 7u);
        auto result = HillTailIndex(window.data(), 500, 25);
        check("real production config (n=500, cutoff=25) produces a value", result.has_value());
        if (result.has_value()) {
            check("result is finite and positive", std::isfinite(*result) && *result > 0.0);
        }
    }

    // --- maxK < 2 degenerate case: count just barely above tail_cutoff+1, but 4*cutoff capped at
    // count-1 can still be too small (e.g. count=26, tail_cutoff=25 -> maxK=min(100,25)=25, fine;
    // but a tiny tail_cutoff=1 with count=3 -> maxK=min(4,2)=2, boundary) ---
    {
        auto window = MakePositiveNoise(3, 0.01, 3u);
        // tail_cutoff=1 -> count(3) >= tail_cutoff+1(2) passes the first gate; maxK=min(4,2)=2,
        // which is NOT < 2, so this should still produce a value, not nullopt.
        auto result = HillTailIndex(window.data(), 3, 1);
        check("tiny-but-valid window (n=3, cutoff=1) still produces a value", result.has_value());
    }

    // --- Determinism: same input (independent copies) gives the same output ---
    {
        auto windowA = MakePositiveNoise(500, 0.002, 99u);
        auto windowB = windowA;  // independent copy -- HillTailIndex mutates its argument in place
        const auto resultA = HillTailIndex(windowA.data(), windowA.size(), 25);
        const auto resultB = HillTailIndex(windowB.data(), windowB.size(), 25);
        check("deterministic: same input gives the same output",
              resultA.has_value() && resultB.has_value() && *resultA == *resultB);
    }

    // --- Regression: TailRiskEngine's own raw (pre-EWMA) computation, independently recomputed
    // via GetWindowSnapshot()+HillTailIndex() on the exact same window, matches the FIRST
    // raw-alpha value folded into the smoothed output (the only point where smoothed == raw,
    // since EWMA starts by taking rawAlpha directly on its very first successful computation) ---
    {
        MindfulTrader::TailRiskEngine tre;
        auto driver = MakePositiveNoise(100, 0.001, 55u);
        // Feed observations until just past the warmup threshold (tail_cutoff+1=26), so the
        // engine's smoothed alpha has just been seeded directly from its own first raw value.
        int fed = 0;
        for (double v : driver) {
            tre.AddObservation(v);
            ++fed;
            if (fed == 26) break;  // first tick where ComputeRawAlpha can succeed
        }
        const double smoothedAfterFirstSuccess = tre.GetHillAlpha();

        std::vector<double> snapshot;
        const std::size_t count = tre.GetWindowSnapshot(snapshot);
        const auto independentResult = HillTailIndex(snapshot.data(), count, tre.GetTailCutoff());
        check("TailRiskEngine's first smoothed alpha matches an independent HillTailIndex call on "
              "the same window (smoothed == raw on the very first successful computation)",
              independentResult.has_value() && *independentResult == smoothedAfterFirstSuccess);
    }

    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
