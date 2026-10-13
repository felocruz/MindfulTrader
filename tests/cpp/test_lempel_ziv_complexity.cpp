// Unit tests for LempelZivComplexity (pure Lempel-Ziv 76 complexity estimator, extracted from
// InformationEngine::GetLempelZivComplexity).
//
// Build & run natively (no Sierra Chart deps, header-only):
//   g++ -std=c++17 -I include tests/cpp/test_lempel_ziv_complexity.cpp -o /tmp/lz_test && /tmp/lz_test

#include "LempelZivComplexity.h"
#include "InformationEngine.h"

#include <array>
#include <cmath>
#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}  // namespace

int main() {
    // --- Warmup: fewer than 10 samples returns the neutral 0.5 sentinel ---
    {
        std::array<double, MindfulTrader::kLempelZivMaxWindow> window{};
        for (std::size_t n = 0; n < 10; ++n) {
            char label[64];
            std::snprintf(label, sizeof(label), "count=%zu returns neutral 0.5", n);
            check(label, MindfulTrader::LempelZivComplexity(window.data(), n) == 0.5);
        }
    }

    // --- Perfectly alternating sequence: highly "new" at every step (near-maximal complexity
    // for a 2-symbol alphabet), since every bit flips relative to the running median threshold ---
    {
        std::array<double, MindfulTrader::kLempelZivMaxWindow> window{};
        for (std::size_t i = 0; i < 64; ++i) {
            window[i] = (i % 2 == 0) ? 1.0 : -1.0;
        }
        const double c = MindfulTrader::LempelZivComplexity(window.data(), 64);
        check("alternating sequence: complexity is finite and positive", std::isfinite(c) && c > 0.0);
    }

    // --- Constant sequence (all identical values): median splits exactly at that value, so
    // every element binarizes to 1 (>= median) -- a single repeated symbol, minimal complexity ---
    {
        std::array<double, MindfulTrader::kLempelZivMaxWindow> window{};
        window.fill(5.0);
        const double c = MindfulTrader::LempelZivComplexity(window.data(), 64);
        const double n = 64.0;
        const double b_n = n / std::log2(n);
        // All-1s bit string: LZ76 complexity count is 1 (one giant repeated component) in the
        // classic Kaspar-Schuster convention for this edge case -- verify against the closed
        // form directly, not just "some small value".
        check("constant sequence: complexity matches closed form 1/b(n)", std::fabs(c - 1.0 / b_n) < 1e-12);
    }

    // --- Count at the fixed-capacity cap behaves identically whether window is exactly full
    // or has trailing garbage beyond `count` (the function must only read the first `count`
    // elements) ---
    {
        std::array<double, MindfulTrader::kLempelZivMaxWindow> windowA{};
        std::array<double, MindfulTrader::kLempelZivMaxWindow> windowB{};
        for (std::size_t i = 0; i < 40; ++i) {
            const double v = std::sin(static_cast<double>(i) * 0.3);
            windowA[i] = v;
            windowB[i] = v;
        }
        windowB.back() = 999.0;  // garbage beyond count=40, must be ignored
        const double a = MindfulTrader::LempelZivComplexity(windowA.data(), 40);
        const double b = MindfulTrader::LempelZivComplexity(windowB.data(), 40);
        check("trailing garbage beyond count is ignored", a == b);
    }

    // --- Regression: InformationEngine::GetLempelZivComplexity() still matches this pure
    // function exactly after the 2026-10-12 extraction refactor (same math, moved not changed) ---
    {
        MindfulTrader::InformationEngine ie;
        uint32_t seed = 12345u;
        for (int i = 0; i < 200; ++i) {
            seed = seed * 1664525u + 1013904223u;
            const double u = static_cast<double>(seed >> 8) / 16777216.0;
            ie.AddObservation(2.0 * u - 1.0);
        }
        std::array<double, MindfulTrader::kLempelZivMaxWindow> snapshot{};
        const std::size_t count = ie.GetLzWindowSnapshot(snapshot);
        const double viaClass = ie.GetLempelZivComplexity();
        const double viaPureFn = MindfulTrader::LempelZivComplexity(snapshot.data(), count);
        check("InformationEngine delegates to the pure function exactly", viaClass == viaPureFn);
    }

    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
