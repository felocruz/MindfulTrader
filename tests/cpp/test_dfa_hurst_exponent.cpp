// Unit tests for DfaHurstExponent (pure Detrended Fluctuation Analysis Hurst
// estimator, extracted from src/StudyHelperFunctions.cpp's
// CalculateHurstExponent).
//
// Build & run natively (no Sierra Chart deps, header-only):
//   g++ -std=c++17 -I include tests/cpp/test_dfa_hurst_exponent.cpp -o /tmp/dfa_test && /tmp/dfa_test

#include "DfaHurstExponent.h"

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

// Verbatim brute-force reference matching src/StudyHelperFunctions.cpp's
// CalculateHurstExponent(sc, length, minScale) exactly, operating on a
// chronological (oldest-first) price array instead of sc.BaseData -- same
// price-to-return conversion, same DFA math.
double BruteForceHurst(const std::vector<float>& prices, int length, int minScale) {
    length = std::clamp(length, 16, 512);
    minScale = std::clamp(minScale, 4, 64);
    if (length < minScale * 4) return std::nan("");

    std::vector<double> logReturns(static_cast<std::size_t>(length));
    double sumReturns = 0.0;
    // prices holds length+1 chronological points; logReturns[i] = log(prices[i+1]/prices[i]).
    for (int i = 0; i < length; ++i) {
        const double cur = prices[static_cast<std::size_t>(i + 1)];
        const double prev = prices[static_cast<std::size_t>(i)];
        const double r = (cur > 0.0 && prev > 0.0) ? std::log(cur / prev) : 0.0;
        logReturns[static_cast<std::size_t>(i)] = r;
        sumReturns += r;
    }
    const double meanReturn = sumReturns / static_cast<double>(length);

    std::vector<double> profile(static_cast<std::size_t>(length));
    double cumulative = 0.0;
    for (int i = 0; i < length; ++i) {
        cumulative += (logReturns[static_cast<std::size_t>(i)] - meanReturn);
        profile[static_cast<std::size_t>(i)] = cumulative;
    }

    const int maxScale = length / 4;
    if (maxScale <= minScale) return std::nan("");
    const int step = (maxScale - minScale > 50) ? 2 : 1;

    std::vector<double> logScales, logFluctuations;
    for (int s = minScale; s <= maxScale; s += step) {
        const int numSegments = length / s;
        if (numSegments < 1) continue;
        double totalVariance = 0.0;
        int usedSegments = 0;
        for (int v = 0; v < numSegments; ++v) {
            const int startIndex = v * s;
            const double n = static_cast<double>(s);
            const double sumX = n * (n - 1.0) * 0.5;
            const double sumX2 = n * (n - 1.0) * (2.0 * n - 1.0) / 6.0;
            const double denom = n * sumX2 - sumX * sumX;
            if (std::fabs(denom) < 1e-12) continue;
            double sumY = 0.0, sumXY = 0.0;
            for (int k = 0; k < s; ++k) {
                const double y = profile[static_cast<std::size_t>(startIndex + k)];
                sumY += y;
                sumXY += static_cast<double>(k) * y;
            }
            const double slope = (n * sumXY - sumX * sumY) / denom;
            const double intercept = (sumY - slope * sumX) / n;
            double ssr = 0.0;
            for (int k = 0; k < s; ++k) {
                const double trend = slope * static_cast<double>(k) + intercept;
                const double diff = profile[static_cast<std::size_t>(startIndex + k)] - trend;
                ssr += diff * diff;
            }
            totalVariance += (ssr / n);
            ++usedSegments;
        }
        if (usedSegments == 0) continue;
        const double f_s = std::sqrt(totalVariance / static_cast<double>(usedSegments));
        if (f_s > 1e-12) {
            logScales.push_back(std::log(static_cast<double>(s)));
            logFluctuations.push_back(std::log(f_s));
        }
    }
    if (logScales.size() < 2) return std::nan("");

    const double n = static_cast<double>(logScales.size());
    double sumX = 0.0, sumY = 0.0, sumXY = 0.0, sumX2 = 0.0;
    for (std::size_t i = 0; i < logScales.size(); ++i) {
        sumX += logScales[i];
        sumY += logFluctuations[i];
        sumXY += logScales[i] * logFluctuations[i];
        sumX2 += logScales[i] * logScales[i];
    }
    const double denom = n * sumX2 - sumX * sumX;
    if (std::fabs(denom) < 1e-12) return std::nan("");
    double hurst = (n * sumXY - sumX * sumY) / denom;
    hurst = std::clamp(hurst, 0.0, 1.5);
    return hurst;
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

// Converts a chronological price walk of length+1 points into the log-returns
// array DfaHurstExponent expects (oldest first, most recent last).
std::vector<float> ToLogReturns(const std::vector<float>& prices, int length) {
    std::vector<float> r(static_cast<std::size_t>(length));
    for (int i = 0; i < length; ++i) {
        const double cur = prices[static_cast<std::size_t>(i + 1)];
        const double prev = prices[static_cast<std::size_t>(i)];
        r[static_cast<std::size_t>(i)] = (cur > 0.0 && prev > 0.0)
            ? static_cast<float>(std::log(cur / prev)) : 0.0f;
    }
    return r;
}
}  // namespace

int main() {
    check("lookback too short for minScale*4 returns NaN",
          std::isnan(DfaHurstExponent(std::vector<float>(16, 0.001f).data(), 16, 8)));

    for (int length : {50, 100, 200}) {
        const auto walk = MakeWalk(length + 1, 0.8, 2026u);
        const auto logReturns = ToLogReturns(walk, length);

        const double expected = BruteForceHurst(walk, length, 8);
        const float actual = DfaHurstExponent(logReturns.data(), length, 8);

        char label[128];
        std::snprintf(label, sizeof(label), "length=%d: pure extraction matches brute-force original", length);
        check(label, std::fabs(expected - static_cast<double>(actual)) < 1e-4);
        check("result is within contract [0.0, 1.5]", actual >= 0.0f && actual <= 1.5f);
    }

    {
        // A pure random walk (no persistence) should land near H=0.5, not at either extreme --
        // sanity check that the estimator actually discriminates, not just "doesn't crash."
        const auto walk = MakeWalk(201, 1.0, 555u);
        const auto logReturns = ToLogReturns(walk, 200);
        const float hurst = DfaHurstExponent(logReturns.data(), 200, 8);
        check("random walk: Hurst estimate is finite and in a plausible mid-range",
              std::isfinite(hurst) && hurst > 0.2f && hurst < 0.9f);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
