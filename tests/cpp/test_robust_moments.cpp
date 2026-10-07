// tests/cpp/test_robust_moments.cpp
#include "RobustMoments.h"
#include <cmath>
#include <cstdio>
#include <random>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
bool approx(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
}

int main() {
    // Gaussian reference: Moors kurtosis normalizes to ~1.23 under N(0,1)
    // (the value the spec cites) -- verify on a large synthetic Gaussian sample.
    std::mt19937 rng(11);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    std::array<float, 100> gaussianReturns{};
    for (auto& r : gaussianReturns) r = gauss(rng);

    check("Moors kurtosis on N(0,1) sample is close to the ~1.23 reference value",
          approx(MoorsKurtosis(gaussianReturns), 1.23f, 0.35f));  // wide tolerance -- N=100 single draw

    // Symmetric distribution -> Bowley skewness near 0.
    check("Bowley skewness on symmetric N(0,1) sample is near 0",
          approx(BowleySkewness(gaussianReturns), 0.0f, 0.15f));

    // Outlier robustness: the whole point of Kim & White's replacement --
    // a single extreme value must NOT blow up either statistic the way the
    // old moment-based formula did.
    std::array<float, 100> withOutlier = gaussianReturns;
    withOutlier[0] = 50.0f;  // 50-sigma outlier
    check("Moors kurtosis is not dominated by a single 50-sigma outlier",
          MoorsKurtosis(withOutlier) < 5.0f);  // moment-based kurtosis would spike to hundreds here

    // --- ComputeRealizedKurtosis / ComputeRealizedSkewness (2026-09-20 pure ports of
    // StudyHelperFunctions.cpp's CalculateRealizedKurtosis/CalculateSkewness) ---
    {
        // Degenerate case: all closes identical -> zero variance -> carry-forward/neutral.
        std::array<float, 101> flatCloses{};
        flatCloses.fill(100.0f);
        std::array<float, 20> flatAtr{};
        flatAtr.fill(1.0f);
        check("degenerate flat-price kurtosis falls back to neutral baseline (no prior)",
              approx(ComputeRealizedKurtosis(flatCloses, flatAtr, 0.0f, false), 1.23f, 1e-6f));
        check("degenerate flat-price kurtosis carries forward a valid prior",
              approx(ComputeRealizedKurtosis(flatCloses, flatAtr, 2.0f, true), 2.0f, 1e-6f));
        check("degenerate flat-price skewness carries forward the last valid value",
              approx(ComputeRealizedSkewness(flatCloses, flatAtr, 0.42f), 0.42f, 1e-6f));
    }
    {
        // Real varying series -> finite, in-range kurtosis/skewness (exact value not asserted --
        // MoorsKurtosis/BowleySkewness's own correctness is covered above; this just proves the
        // window-building + regime-multiplier wiring produces a sane, non-degenerate result).
        std::mt19937 rng2(7);
        std::normal_distribution<float> gauss2(0.0f, 0.01f);
        std::array<float, 101> varyingCloses{};
        varyingCloses[0] = 100.0f;
        for (size_t i = 1; i < varyingCloses.size(); ++i) {
            varyingCloses[i] = varyingCloses[i - 1] / (1.0f + gauss2(rng2));
        }
        std::array<float, 20> atr{};
        atr.fill(0.5f);
        const float kurt = ComputeRealizedKurtosis(varyingCloses, atr, 0.0f, false);
        const float skew = ComputeRealizedSkewness(varyingCloses, atr, 0.0f);
        check("realized kurtosis on a varying series is finite and in [0,5]",
              std::isfinite(kurt) && kurt >= 0.0f && kurt <= 5.0f);
        check("realized skewness on a varying series is finite and in [-1.5,1.5]",
              std::isfinite(skew) && skew >= -1.5f && skew <= 1.5f);

        // High vol-ratio (current ATR far above the 20-bar average) must scale kurtosis UP
        // relative to a matched low vol-ratio case (the regime-multiplier's own documented intent).
        std::array<float, 20> highVolAtr = atr;
        highVolAtr[0] = 5.0f;  // current >> 20-bar average -> regime_mult = 1.25x
        std::array<float, 20> lowVolAtr = atr;
        lowVolAtr[0] = 0.1f;   // current << 20-bar average -> regime_mult = 0.75x
        const float kurtHighVol = ComputeRealizedKurtosis(varyingCloses, highVolAtr, 0.0f, false);
        const float kurtLowVol = ComputeRealizedKurtosis(varyingCloses, lowVolAtr, 0.0f, false);
        check("high vol-ratio regime multiplier scales kurtosis up vs. low vol-ratio",
              kurtHighVol > kurtLowVol);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
