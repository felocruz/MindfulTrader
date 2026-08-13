// tests/cpp/bench_quantile_window.cpp — throwaway timing harness, not part of
// the regular test suite. Determines whether Task 6 uses a plain full
// re-sort or needs a t-digest, per docs/superpowers/specs/2026-08-13-
// observation-vector-institutional-elevation-spec.md Unit 3's decision rule.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <random>

int main() {
    std::mt19937 rng(7);
    std::normal_distribution<float> dist(0.0f, 1.0f);
    std::array<float, 100> returns{};
    for (auto& r : returns) r = dist(rng);

    constexpr int kIterations = 100000;  // far more than the 15-min-bar-cadence
                                          // call rate would ever produce in a
                                          // comparable wall-clock span; a
                                          // conservative stress test.
    const auto start = std::chrono::steady_clock::now();
    volatile float sink = 0.0f;  // prevent the optimizer from eliding the sort
    for (int iter = 0; iter < kIterations; ++iter) {
        std::array<float, 100> scratch = returns;
        std::sort(scratch.begin(), scratch.end());
        sink += scratch[50];
    }
    const auto end = std::chrono::steady_clock::now();
    const double totalMicros = std::chrono::duration<double, std::micro>(end - start).count();
    const double perCallMicros = totalMicros / kIterations;

    std::printf("Full re-sort of N=100: %.4f microseconds/call (%d iterations)\n",
                perCallMicros, kIterations);
    std::printf("At 15-minute bar cadence this runs at most once per 900 seconds --\n");
    std::printf("verdict: %s\n",
                 perCallMicros < 10.0 ? "PASS -- full re-sort is trivially cheap enough, use it directly"
                                      : "FAIL -- investigate t-digest (Dunning & Ertl 2019)");
    return 0;
}
