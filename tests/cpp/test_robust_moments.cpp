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

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
