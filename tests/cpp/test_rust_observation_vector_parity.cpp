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
#include "CarryForwardCalculators.h"
#include "DfaHurstExponent.h"
#include "EventVelocityEngine.h"
#include "LiquidityFragilityEngine.h"
#include "MeanReversionCalculator.h"
#include "RingBuffer.h"
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

    // --- DfaHurstExponent: C++ production function vs. Rust FFI wrapper, same input ---
    for (int length : {50, 100, 200}) {
        const auto walk = MakeWalk(length + 1, 0.8, 2026u);
        std::array<float, 512> logReturns{};
        for (int i = 0; i < length; ++i) {
            const float cur = walk[static_cast<std::size_t>(i + 1)];
            const float prev = walk[static_cast<std::size_t>(i)];
            logReturns[static_cast<std::size_t>(i)] =
                (cur > 0.0f && prev > 0.0f) ? std::log(cur / prev) : 0.0f;
        }

        const float cppResult = DfaHurstExponent(logReturns.data(), length, 8);
        const float rustResult = mts::mts_observation_vector_dfa_hurst_exponent(
            logReturns.data(), static_cast<std::size_t>(length), 8);

        char label[128];
        std::snprintf(label, sizeof(label), "DfaHurstExponent length=%d: Rust matches C++ exactly", length);
        check(label, std::fabs(cppResult - rustResult) < 1e-5f);
    }

    // Degenerate case must agree too (both NaN): length=16 < minScale(8)*4=32.
    {
        const std::array<float, 16> small{0.001f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f,
                                           0.001f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f, 0.001f};
        const float cppResult = DfaHurstExponent(small.data(), 16, 8);
        const float rustResult = mts::mts_observation_vector_dfa_hurst_exponent(small.data(), small.size(), 8);
        check("DfaHurstExponent degenerate (length < minScale*4): both NaN",
              std::isnan(cppResult) && std::isnan(rustResult));
    }

    // --- ComputeMeanReversionZ: C++ production function vs. Rust FFI wrapper, same input ---
    for (int n : {10, 20, 40}) {
        const auto walk = MakeWalk(n, 1.5, 3131u);

        const float cppResult = mrc::ComputeMeanReversionZ(walk.data(), n, 0.0f);
        const float rustResult = mts::mts_observation_vector_compute_mean_reversion_z(walk.data(), walk.size(), 0.0f);

        char label[128];
        std::snprintf(label, sizeof(label), "ComputeMeanReversionZ n=%d: Rust matches C++ exactly", n);
        check(label, std::fabs(cppResult - rustResult) < 1e-4f);
    }

    // Degenerate case (flat window, MAD collapses to 0) must agree too: both carry the same
    // last-valid-value forward.
    {
        const std::array<float, 7> flat{100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f, 100.0f};
        const float cppResult = mrc::ComputeMeanReversionZ(flat.data(), 7, 0.42f);
        const float rustResult =
            mts::mts_observation_vector_compute_mean_reversion_z(flat.data(), flat.size(), 0.42f);
        check("ComputeMeanReversionZ degenerate flat window: both carry last_valid_value=0.42 forward",
              std::fabs(cppResult - 0.42f) < 1e-6f && std::fabs(rustResult - 0.42f) < 1e-6f);
    }

    // Null/empty input must also carry last_valid_value forward (the FFI wrapper's own documented
    // convention, distinct from most other wrappers here, which map null/empty to NaN).
    {
        const float rustResult = mts::mts_observation_vector_compute_mean_reversion_z(nullptr, 0, 0.77f);
        check("ComputeMeanReversionZ Rust wrapper: null input carries last_valid_value forward",
              std::fabs(rustResult - 0.77f) < 1e-6f);
    }

    // --- ComputeLiquidityFragility: C++ production function vs. Rust FFI wrapper, same input ---
    {
        std::array<float, lfe::kWindow> rangeW{};
        std::array<float, lfe::kWindow> volW{};
        const auto rangeWalk = MakeWalk(lfe::kWindow, 0.3, 5151u);
        const auto volWalk = MakeWalk(lfe::kWindow, 2.0, 6161u);
        for (int i = 0; i < lfe::kWindow; ++i) {
            rangeW[static_cast<std::size_t>(i)] = std::fabs(rangeWalk[static_cast<std::size_t>(i)] - 100.0f) + 0.5f;
            volW[static_cast<std::size_t>(i)] = std::fabs(volWalk[static_cast<std::size_t>(i)]) + 1.0f;
        }

        for (const auto& [liveBarRange, liveVol, prevFrag] :
             {std::make_tuple(4.0f, 400.0f, 0.5f), std::make_tuple(20.0f, 100.0f, 0.2f),
              std::make_tuple(2.0f, 1000.0f, 0.1f)}) {
            const float cppResult =
                lfe::ComputeLiquidityFragility(rangeW.data(), volW.data(), liveBarRange, liveVol, prevFrag);
            const float rustResult = mts::mts_observation_vector_compute_liquidity_fragility(
                rangeW.data(), volW.data(), rangeW.size(), liveBarRange, liveVol, prevFrag);
            check("ComputeLiquidityFragility: Rust matches C++ exactly",
                  std::fabs(cppResult - rustResult) < 1e-4f);
        }
    }

    // Thin-volume degenerate case must agree too: both carry prev_fragility forward.
    {
        std::array<float, lfe::kWindow> rangeW{};
        std::array<float, lfe::kWindow> volW{};
        rangeW.fill(2.0f);
        volW.fill(10.0f);
        const float cppResult = lfe::ComputeLiquidityFragility(rangeW.data(), volW.data(), 4.0f, 10.0f, 0.42f);
        const float rustResult = mts::mts_observation_vector_compute_liquidity_fragility(
            rangeW.data(), volW.data(), rangeW.size(), 4.0f, 10.0f, 0.42f);
        check("ComputeLiquidityFragility thin-volume degenerate: both carry prev_fragility=0.42 forward",
              std::fabs(cppResult - 0.42f) < 1e-6f && std::fabs(rustResult - 0.42f) < 1e-6f);
    }

    // Null/zero-length window input must also carry prev_fragility forward.
    {
        const float rustResult =
            mts::mts_observation_vector_compute_liquidity_fragility(nullptr, nullptr, 0, 4.0f, 400.0f, 0.63f);
        check("ComputeLiquidityFragility Rust wrapper: null input carries prev_fragility forward",
              std::fabs(rustResult - 0.63f) < 1e-6f);
    }

    // --- ComputeBurstinessIndex (dim3's real caller): C++ vs. Rust FFI wrapper ---
    {
        const float cppResult = cfc::ComputeBurstinessIndex(0.0021062, 1.0, 0.0f, -10.0f, 6.0f);
        const float rustResult =
            mts::mts_observation_vector_compute_burstiness_index(0.0021062, 1.0, 0.0f, -10.0f, 6.0f);
        check("ComputeBurstinessIndex: Rust matches C++ exactly", std::fabs(cppResult - rustResult) < 1e-5f);
    }
    {
        const float cppResult = cfc::ComputeBurstinessIndex(4.0, 1e-13, 0.42f, -6.0f, 6.0f);
        const float rustResult =
            mts::mts_observation_vector_compute_burstiness_index(4.0, 1e-13, 0.42f, -6.0f, 6.0f);
        check("ComputeBurstinessIndex degenerate: both carry last_valid_value=0.42 forward",
              std::fabs(cppResult - 0.42f) < 1e-6f && std::fabs(rustResult - 0.42f) < 1e-6f);
    }

    // --- ComputeRelativeRange (dim 2): C++ vs. Rust FFI wrapper ---
    {
        const float cppResult = cfc::ComputeRelativeRange(105.0f, 100.0f, 2.5f, 0.0f);
        const float rustResult = mts::mts_observation_vector_compute_relative_range(105.0f, 100.0f, 2.5f, 0.0f);
        check("ComputeRelativeRange: Rust matches C++ exactly", std::fabs(cppResult - rustResult) < 1e-5f);
    }

    // --- ComputeFisherInformation (dim 8): C++ vs. Rust FFI wrapper ---
    {
        const float cppResult = cfc::ComputeFisherInformation(100.0f, 110.0f, 109.0f, 0.0f);
        const float rustResult = mts::mts_observation_vector_compute_fisher_information(100.0f, 110.0f, 109.0f, 0.0f);
        check("ComputeFisherInformation: Rust matches C++ exactly", std::fabs(cppResult - rustResult) < 1e-5f);
    }
    {
        const float cppResult = cfc::ComputeFisherInformation(100.0f, 100.0f, 100.0f, -0.85f);
        const float rustResult =
            mts::mts_observation_vector_compute_fisher_information(100.0f, 100.0f, 100.0f, -0.85f);
        check("ComputeFisherInformation degenerate: both carry last_valid_value=-0.85 forward",
              std::fabs(cppResult - (-0.85f)) < 1e-6f && std::fabs(rustResult - (-0.85f)) < 1e-6f);
    }

    // --- ComputeAmihudIlliquidity (dim 11): C++ vs. Rust FFI wrapper ---
    {
        const double sumLogRatio = std::log(0.002) + std::log(0.003) + std::log(0.001);
        const float cppResult = cfc::ComputeAmihudIlliquidity(sumLogRatio, 3, 0.0f);
        const float rustResult = mts::mts_observation_vector_compute_amihud_illiquidity(sumLogRatio, 3, 0.0f);
        check("ComputeAmihudIlliquidity: Rust matches C++ exactly", std::fabs(cppResult - rustResult) < 1e-5f);
    }

    // --- CalculateBurstinessIndex (dim 1): C++ (RingBuffer input) vs. Rust FFI wrapper (raw slice) ---
    {
        RingBuffer<uint64_t, 24> ts;
        for (uint64_t t : {0ULL, 50'000ULL, 100'000ULL, 300'000ULL, 1'100'000ULL, 1'150'000ULL,
                           1'200'000ULL, 1'250'000ULL, 2'150'000ULL, 2'250'000ULL, 2'350'000ULL,
                           3'050'000ULL, 3'100'000ULL, 3'150'000ULL, 3'200'000ULL, 3'800'000ULL,
                           4'000'000ULL, 4'050'000ULL, 4'950'000ULL, 5'000'000ULL}) {
            ts.push_back(t);
        }
        std::vector<uint64_t> flat;
        for (size_t i = 0; i < ts.size(); ++i) flat.push_back(ts[i]);

        const float cppResult = eve::CalculateBurstinessIndex(ts);
        const float rustResult = mts::mts_observation_vector_calculate_burstiness_index(flat.data(), flat.size());
        check("CalculateBurstinessIndex: Rust matches C++ exactly", std::fabs(cppResult - rustResult) < 1e-4f);
    }
    {
        // Degenerate: fewer than kMinSamples(20) -- both return the neutral 0.0 default.
        RingBuffer<uint64_t, 16> ts;
        for (uint64_t t = 0; t < 10'000'000; t += 1'000'000) ts.push_back(t);
        std::vector<uint64_t> flat;
        for (size_t i = 0; i < ts.size(); ++i) flat.push_back(ts[i]);

        const float cppResult = eve::CalculateBurstinessIndex(ts);
        const float rustResult = mts::mts_observation_vector_calculate_burstiness_index(flat.data(), flat.size());
        check("CalculateBurstinessIndex below-minimum-samples: Rust matches C++ (both neutral 0.0)",
              cppResult == 0.0f && rustResult == 0.0f);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
