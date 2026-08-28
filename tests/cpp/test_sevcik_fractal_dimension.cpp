// Unit tests for SevcikFractalDimension (pure Sevcik fractal-dimension
// estimator, extracted from StudyHelperFunctions.cpp's
// CalculateFractalDimension).
//
// Build & run natively (no Sierra Chart deps, header-only):
//   g++ -std=c++17 -I include tests/cpp/test_sevcik_fractal_dimension.cpp -o /tmp/sfd_test && /tmp/sfd_test

#include "SevcikFractalDimension.h"

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
// CalculateFractalDimension exactly, including its asymmetric windowing
// (minP/maxP over the live bar's window, path length one bar further back,
// excluding the live bar) -- operates on a simulated sc.Index-relative array
// where `bars[k]` represents `sc.BaseData[SC_LAST][sc.Index - k]` (k=0 is the
// live/current bar, increasing k moves further into history).
double BruteForceFractalDim(const std::vector<float>& bars, int lookback_n) {
    float minP = 1e30f, maxP = -1e30f;
    for (int i = 0; i < lookback_n; ++i) {
        const float p = bars[static_cast<std::size_t>(i)];  // sc.Index - i
        if (p < minP) minP = p;
        if (p > maxP) maxP = p;
    }
    if (maxP <= minP) return std::nan("");
    const int segments = lookback_n - 1;
    if (segments <= 0) return std::nan("");
    double length = 0.0;
    const double priceRange = static_cast<double>(maxP) - static_cast<double>(minP);
    // idx = sc.Index - lookback_n + i, in terms of `bars[k]` (k = sc.Index - idx):
    // p1 = bars[lookback_n - i + 1], p2 = bars[lookback_n - i], for i in [1, lookback_n).
    for (int i = 1; i < lookback_n; ++i) {
        const float p1 = bars[static_cast<std::size_t>(lookback_n - i + 1)];
        const float p2 = bars[static_cast<std::size_t>(lookback_n - i)];
        const double dy = (static_cast<double>(p2) - static_cast<double>(p1)) / priceRange;
        const double dx = 1.0 / static_cast<double>(segments);
        length += std::sqrt(dx * dx + dy * dy);
    }
    if (length <= 0.0) return std::nan("");
    return 1.0 + std::log(length) / std::log(2.0 * static_cast<double>(segments));
}

// Deterministic pseudo-random walk (no <random>), same LCG convention as
// this codebase's other native test fixtures.
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

// Convert a chronological walk (oldest first) into the sc.Index-relative
// `bars[k]` convention (k=0 = newest/live) that BruteForceFractalDim expects,
// and into the prices[0..lookback_n] chronological array
// SevcikFractalDimension expects, for the same underlying window.
void BuildBothViews(const std::vector<float>& walk, int endIdxExclusive, int lookback_n,
                     std::vector<float>* barsRelative, std::vector<float>* pricesChrono) {
    // walk[endIdxExclusive - 1] is "sc.Index" (the live bar).
    barsRelative->resize(static_cast<std::size_t>(lookback_n) + 1);
    for (int k = 0; k <= lookback_n; ++k) {
        (*barsRelative)[static_cast<std::size_t>(k)] = walk[static_cast<std::size_t>(endIdxExclusive - 1 - k)];
    }
    // pricesChrono[i] = sc.Index - lookback_n + i, i.e. reverse of barsRelative.
    pricesChrono->resize(static_cast<std::size_t>(lookback_n) + 1);
    for (int i = 0; i <= lookback_n; ++i) {
        (*pricesChrono)[static_cast<std::size_t>(i)] = (*barsRelative)[static_cast<std::size_t>(lookback_n - i)];
    }
}
}  // namespace

int main() {
    check("degenerate (flat) window returns NaN",
          std::isnan(SevcikFractalDimension(std::vector<float>(41, 100.0f).data(), 40)));
    check("lookback_n < 2 returns NaN",
          std::isnan(SevcikFractalDimension(std::vector<float>{100.0f, 101.0f}.data(), 1)));

    for (int lookback_n : {30, 40, 150, 400}) {
        const auto walk = MakeWalk(lookback_n + 200, 0.8, 4242u);
        std::vector<float> barsRelative, pricesChrono;
        BuildBothViews(walk, static_cast<int>(walk.size()), lookback_n, &barsRelative, &pricesChrono);

        const double expected = BruteForceFractalDim(barsRelative, lookback_n);
        const float actual = SevcikFractalDimension(pricesChrono.data(), lookback_n);
        char label[128];
        std::snprintf(label, sizeof(label), "n=%d: pure extraction matches brute-force original", lookback_n);
        check(label, std::fabs(expected - static_cast<double>(actual)) < 1e-4);
        check("result is within contract [1.0, 2.0]", actual >= 1.0f && actual <= 2.0f);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
