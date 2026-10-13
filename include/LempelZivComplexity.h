// LempelZivComplexity.h -- pure, header-only, ACSIL-independent Lempel-Ziv 76 complexity
// estimator (Kaspar & Schuster 1987), extracted from InformationEngine::GetLempelZivComplexity()
// so it can be natively unit-tested and ported to Rust independent of InformationEngine's own
// stateful ring-buffer/histogram bookkeeping -- same extraction precedent already established by
// MeanReversionCalculator.h/LiquidityFragilityEngine.h (both pulled out of ACSIL-coupled call
// sites for the same reason: a pure function is a Rust-portable, natively-testable unit; a
// stateful class method embedding the same math is neither).
//
// `window` must already be in chronological order (oldest first, most recent last) -- the
// circular-buffer-to-linear reconstruction stays InformationEngine's own responsibility,
// unchanged by this extraction (same division of labor DfaHurstExponent.h/MeanReversionCalculator.h
// already use: the ring buffer snapshot happens at the call site, the pure math takes a plain
// array).
#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>

namespace MindfulTrader {

// Matches InformationEngine::WINDOW_SIZE_LZ exactly -- a real wire/behavior contract, not
// incidental (see this header's own static_assert in InformationEngine.h, which ties the two
// together so they cannot silently drift apart).
constexpr std::size_t kLempelZivMaxWindow = 64;

// Lempel-Ziv 76 complexity over an adaptive-median-binarized window, normalized by b(n) = n /
// log2(n) (asymptotic complexity of a random binary string, Kaspar & Schuster 1987). Returns 0.5
// (neutral) for `count < 10` -- insufficient data to binarize/estimate meaningfully. `window` must
// point to at least `count` chronologically-ordered elements; `count` is clamped to
// `kLempelZivMaxWindow` by the caller's own fixed-capacity buffer, never bounds-checked here
// (matches DfaHurstExponent.h's fixed-capacity discipline: the caller's buffer size IS the bound).
inline double LempelZivComplexity(const double* window, std::size_t count) {
    if (count < 10) {
        return 0.5;  // Neutral complexity if insufficient data
    }

    // 1. Adaptive median threshold (institutional standard: no arbitrary fixed cutoff).
    std::array<double, kLempelZivMaxWindow> sortedBuf{};
    for (std::size_t i = 0; i < count; ++i) {
        sortedBuf[i] = window[i];
    }
    std::sort(sortedBuf.begin(), sortedBuf.begin() + static_cast<std::ptrdiff_t>(count));
    const double median = sortedBuf[count / 2];

    // 2. Construct bit string S via median thresholding.
    std::array<int, kLempelZivMaxWindow> S{};
    for (std::size_t i = 0; i < count; ++i) {
        S[i] = (window[i] >= median ? 1 : 0);
    }

    // 3. Lempel-Ziv 76 complexity count, O(n^2) cleanest definition: partition S into components
    // S[0..i-1], S[i..i+k-1], ...; at each step, check whether S[i..i+k-1] already appears
    // somewhere in S[0..i+k-2] (the search space extends into the current candidate's own
    // prefix) -- if so it's a copy (extend k), otherwise it's a genuinely new component
    // (increment complexity, advance i, reset k).
    int complexity = 1;
    const int n = static_cast<int>(count);
    int i = 1;  // Component 1 is S[0]; the next candidate component starts at i=1.
    int k = 1;
    while (i + k <= n) {
        bool found = false;
        // The match of length k must end strictly before index i+k-1, i.e. start j < i.
        for (int j = 0; j < i; ++j) {
            bool match = true;
            for (int x = 0; x < k; ++x) {
                if (S[static_cast<std::size_t>(j + x)] != S[static_cast<std::size_t>(i + x)]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                found = true;
                break;
            }
        }
        if (found) {
            ++k;  // Extend the current component.
        } else {
            ++complexity;  // New component found.
            i += k;
            k = 1;
        }
    }

    // 4. Normalize: b(n) = n / log2(n).
    if (n <= 1) {
        return 0.0;
    }
    const double b_n = static_cast<double>(n) / std::log2(static_cast<double>(n));
    return static_cast<double>(complexity) / b_n;
}

}  // namespace MindfulTrader
