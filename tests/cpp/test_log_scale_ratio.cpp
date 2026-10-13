// Unit tests for LogScaleRatio (pure ratio-of-bipower-variations math, extracted from
// StudyHelperFunctions.cpp's CalculateLogScaleRatio).
//
// Build & run natively (no Sierra Chart deps, header-only):
//   g++ -std=c++17 -I include tests/cpp/test_log_scale_ratio.cpp -o /tmp/lsr_test && /tmp/lsr_test

#include "LogScaleRatio.h"

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
    // --- Equal BVs: ratio is exactly 0.0 (log(1) = 0) ---
    {
        const float r = LogScaleRatio(0.05, 0.05);
        check("equal BVs: ratio is exactly 0.0", r == 0.0f);
    }

    // --- short_bv > long_bv: positive ratio (expanding volatility) ---
    {
        const float r = LogScaleRatio(0.20, 0.05);
        const double expected = std::log((0.20 + 1e-12) / (0.05 + 1e-12));
        check("short_bv > long_bv: positive ratio matches closed form",
              std::fabs(static_cast<double>(r) - expected) < 1e-6);
    }

    // --- short_bv < long_bv: negative ratio (contracting volatility) ---
    {
        const float r = LogScaleRatio(0.01, 0.10);
        check("short_bv < long_bv: negative ratio", r < 0.0f);
    }

    // --- Both zero (degenerate, too-short windows on both sides): log(eps/eps) = log(1) = 0.0 ---
    {
        const float r = LogScaleRatio(0.0, 0.0);
        check("both BVs zero: ratio is exactly 0.0 (epsilon cancels)", r == 0.0f);
    }

    // --- Extreme ratio clamps to [-6.0, 6.0] ---
    {
        const float rHigh = LogScaleRatio(1e6, 1e-9);
        const float rLow = LogScaleRatio(1e-9, 1e6);
        check("extreme high ratio clamps to +6.0", rHigh == 6.0f);
        check("extreme low ratio clamps to -6.0", rLow == -6.0f);
    }

    std::printf("\n%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
