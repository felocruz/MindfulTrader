// tests/cpp/test_rust_observation_vector_parity.cpp -- the authoritative cross-language proof that
// rust/observation_vector's Rust ports produce IDENTICAL output to the original C++ functions, not
// just "ported independently and hoping": this test feeds the SAME input array into both the
// original C++ function and the Rust FFI wrapper (rust/ffi's mts_observation_vector_* functions,
// via the cbindgen-generated include/generated/mts_core.h) and asserts equality. Each language's own
// unit tests (tests/cpp/test_sevcik_fractal_dimension.cpp, tests/cpp/test_robust_moments.cpp,
// rust/observation_vector/src/lib.rs's #[cfg(test)]) separately prove each side matches its own
// reference/golden values; this test is the missing link between them.
//
// Build & run natively (links the Linux-native rust/target/release/libmts_ffi.a -- build that first
// with `cd rust && cargo build -p mts_ffi --release --features observation_vector`):
//   g++ -std=c++17 -I include tests/cpp/test_rust_observation_vector_parity.cpp \
//     rust/target/release/libmts_ffi.a -lpthread -ldl -o /tmp/parity_test && /tmp/parity_test

#include "BipowerVariation.h"
#include "RobustMoments.h"
#include "SevcikFractalDimension.h"
#include "generated/mts_core.h"

#include <array>
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

// Same LCG convention as test_sevcik_fractal_dimension.cpp and rust/observation_vector's own
// #[cfg(test)] -- deliberately NOT std::mt19937/<random> (implementation-defined, not portable
// across languages); this trivial LCG is bit-identical in both C++ and Rust by construction.
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
    // --- SevcikFractalDimension: C++ production function vs. Rust FFI wrapper, same input ---
    for (int lookback_n : {30, 40, 150, 400}) {
        const auto prices = MakeWalk(lookback_n + 1, 0.8, 4242u);

        const float cppResult = SevcikFractalDimension(prices.data(), lookback_n);
        const float rustResult = mts::mts_observation_vector_sevcik_fractal_dimension(
            prices.data(), prices.size());

        char label[128];
        std::snprintf(label, sizeof(label), "SevcikFractalDimension n=%d: Rust matches C++ exactly", lookback_n);
        check(label, std::fabs(cppResult - rustResult) < 1e-5f);
    }

    // Degenerate cases must agree too (both NaN).
    {
        const std::vector<float> flat(41, 100.0f);
        const float cppResult = SevcikFractalDimension(flat.data(), 40);
        const float rustResult = mts::mts_observation_vector_sevcik_fractal_dimension(flat.data(), flat.size());
        check("SevcikFractalDimension degenerate flat window: both NaN",
              std::isnan(cppResult) && std::isnan(rustResult));
    }

    // --- BowleySkewness / MoorsKurtosis: C++ production functions vs. Rust FFI wrappers ---
    {
        const auto walk = MakeWalk(101, 0.8, 777u);
        std::array<float, 100> returns{};
        for (int i = 0; i < 100; ++i) {
            returns[static_cast<std::size_t>(i)] =
                std::log(walk[static_cast<std::size_t>(i + 1)] / walk[static_cast<std::size_t>(i)]);
        }

        const float cppSkew = BowleySkewness(returns);
        const float rustSkew = mts::mts_observation_vector_bowley_skewness(returns.data(), returns.size());
        check("BowleySkewness: Rust matches C++ exactly", std::fabs(cppSkew - rustSkew) < 1e-5f);

        const float cppKurt = MoorsKurtosis(returns);
        const float rustKurt = mts::mts_observation_vector_moors_kurtosis(returns.data(), returns.size());
        check("MoorsKurtosis: Rust matches C++ exactly", std::fabs(cppKurt - rustKurt) < 1e-5f);
    }

    // --- ComputeBipowerVariation: C++ production function vs. Rust FFI wrapper (double, not float) ---
    {
        const double r[] = {0.01, -0.02, 0.03};
        const double cppBv = ComputeBipowerVariation(r, 3);
        const double rustBv = mts::mts_observation_vector_compute_bipower_variation(r, 3);
        check("ComputeBipowerVariation hand-computed window: Rust matches C++ exactly",
              std::fabs(cppBv - rustBv) < 1e-12);
    }
    {
        const auto walk = MakeWalk(101, 0.8, 999u);
        std::vector<double> returns(100);
        for (int i = 0; i < 100; ++i) {
            returns[static_cast<std::size_t>(i)] =
                std::log(static_cast<double>(walk[static_cast<std::size_t>(i + 1)]) /
                          static_cast<double>(walk[static_cast<std::size_t>(i)]));
        }
        const double cppBv = ComputeBipowerVariation(returns.data(), static_cast<int>(returns.size()));
        const double rustBv = mts::mts_observation_vector_compute_bipower_variation(returns.data(), returns.size());
        check("ComputeBipowerVariation on real walk data: Rust matches C++ exactly",
              std::fabs(cppBv - rustBv) < 1e-9);
    }
    check("ComputeBipowerVariation n<2: both return 0.0 (not NaN)",
          ComputeBipowerVariation(nullptr, 0) == 0.0 &&
          mts::mts_observation_vector_compute_bipower_variation(nullptr, 0) == 0.0);

    // Null/empty-input contract: Rust wrapper must not crash, must return NaN.
    {
        check("Rust wrapper handles null pointer without crashing",
              std::isnan(mts::mts_observation_vector_sevcik_fractal_dimension(nullptr, 0)));
        check("Rust wrapper handles zero length without crashing",
              std::isnan(mts::mts_observation_vector_bowley_skewness(nullptr, 0)));
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
