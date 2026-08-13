#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>
#include "RQAEpsilonSelector.h"

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}

// Declared here (at global scope, NOT inside the anonymous namespace above)
// to match the production signature this step will add to
// StudyHelperFunctions.h/.cpp -- copy the real implementation in Step 3,
// do not reimplement it differently for the test. Note: a forward
// declaration placed inside an anonymous namespace mangles to a
// TU-local symbol and can never link against (or unambiguously coexist
// with) an externally-defined or header-included global-scope function of
// the same name -- verified empirically while implementing this test.
double SelectEpsilonForTargetRecurrenceRate(const float* prices, int n, double targetRR);

int main() {
    // 30 prices linearly spaced 100.0 to 103.0 -- deterministic, known pairwise
    // distance distribution.
    std::vector<float> prices(30);
    for (int i = 0; i < 30; ++i) prices[static_cast<size_t>(i)] = 100.0f + 0.1f * static_cast<float>(i);

    const double eps = SelectEpsilonForTargetRecurrenceRate(prices.data(), 30, 0.03);

    // Verify: using this epsilon, the actual measured recurrence rate over
    // these prices is within a small tolerance of the 0.03 target (the
    // defining property of the fixed-RR selection method).
    int recurCount = 30;  // diagonal (i==i) always recurs
    for (int i = 0; i < 30; ++i) {
        for (int j = i + 1; j < 30; ++j) {
            if (std::fabs(prices[static_cast<size_t>(i)] - prices[static_cast<size_t>(j)]) < eps) {
                recurCount += 2;
            }
        }
    }
    const double measuredRR = static_cast<double>(recurCount) / (30.0 * 30.0);
    check("selected epsilon achieves recurrence rate within 0.01 of the 0.03 target",
          std::fabs(measuredRR - 0.03) < 0.01);
    check("epsilon is strictly positive", eps > 0.0);

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
