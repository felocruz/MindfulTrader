// tests/cpp/TestClassifierParams.cpp — config-driven classifier parameter loading.
//
// Build: g++ -std=c++17 -Wall -Wextra -I include -I /mnt/c/Users/rcruz/vcpkg/installed/x64-windows/include tests/cpp/TestClassifierParams.cpp -o /tmp/t_cp && /tmp/t_cp

#include "ClassifierParams.h"

#include <cmath>
#include <cstdio>
#include <fstream>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}  // namespace

int main() {
    std::printf("ClassifierParams tests\n");

    // --- Safe default when config file is missing: all-zero weights, isLoaded false ---
    {
        ClassifierParams params = ClassifierParams::LoadConfig(
            "turtle_soup_option_b", "/tmp/does_not_exist_classifier_params.json");
        check("missing file -> isLoaded false", params.isLoaded == false);
        check("missing file -> empty weights (neutral/inert)", params.weights.empty());
    }

    // --- Loads real weights from a well-formed test config file ---
    {
        const std::string testPath = "/tmp/test_classifier_params.json";
        std::ofstream f(testPath);
        f << R"({
  "turtle_soup_option_b": {
    "model_type": "logistic_regression",
    "weights": [0.1, -0.2, 0.3],
    "bias": 0.05,
    "feature_names": ["penetration_atr", "close_position", "elapsed_fraction"]
  }
})";
        f.close();

        ClassifierParams params = ClassifierParams::LoadConfig("turtle_soup_option_b", testPath);
        check("real config -> isLoaded true", params.isLoaded == true);
        check("real config -> 3 weights loaded", params.weights.size() == 3);
        check("real config -> weights[0] correct", std::fabs(params.weights[0] - 0.1f) < 1e-5f);
        check("real config -> weights[1] correct (negative)", std::fabs(params.weights[1] - (-0.2f)) < 1e-5f);
        check("real config -> bias correct", std::fabs(params.bias - 0.05f) < 1e-5f);
    }

    // --- Unknown consumer key in a well-formed file -> safe default, not a crash ---
    {
        ClassifierParams params = ClassifierParams::LoadConfig(
            "nonexistent_consumer", "/tmp/test_classifier_params.json");
        check("unknown consumer key -> isLoaded false, no crash", params.isLoaded == false);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
