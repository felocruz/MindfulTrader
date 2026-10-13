// HillTailIndex.h -- pure, header-only, ACSIL-independent port of TailRiskEngine::ComputeRawAlpha
// -- the raw (pre-EWMA-smoothing) Hill estimator via the Resnick & Stărică (1997) Hill-plot
// stability-region k-selector, extracted so it can be natively unit-tested and ported to Rust
// independent of TailRiskEngine's own stateful ring-buffer/EWMA-smoothing bookkeeping (same
// extraction precedent as LempelZivComplexity.h/MeanReversionCalculator.h).
//
// `window` must contain the engine's own absolute-log-return values (TailRiskEngine's own
// AddObservation floors magnitude at 1e-9 before storing -- the caller's responsibility, same
// division of labor established elsewhere), in ANY order -- unlike LempelZivComplexity.h, this
// algorithm only ever operates on the window's sorted MAGNITUDE distribution (order statistics),
// never on chronological position, so the circular-buffer-to-linear reconstruction the real
// `TailRiskEngine::GetWindowSnapshot` performs doesn't need to preserve chronological order for
// this function's own correctness -- only completeness (every live value present once) matters.
// `tail_cutoff` is the engine's own `m_tailCutoff` (windowSize * tailPercent, floored at 2).
//
// `window` is a MUTABLE buffer this function partial_sorts in place (not `const`) -- matching
// `ComputeRawAlpha`'s own zero-extra-allocation design exactly: the caller already owns a
// pre-allocated scratch buffer (`TailRiskEngine::m_sortBuffer`) it copies the live ring buffer
// into once; this function reorders that same buffer rather than taking an immutable `const`
// view and allocating a second copy internally just to preserve purity-by-convention.
//
// Returns `std::nullopt` when the window cannot support an estimate yet (mirrors
// `ComputeRawAlpha`'s own `return false` early-exit, which leaves the caller's EWMA state
// untouched) -- an explicit optional, not an implicit sentinel float, per this crate's own
// institutional NaN-handling principle (2026-10-12, `calibration.rs`'s `Option<f64>` refactor).
//
// **Deliberate precondition simplification, documented not silently assumed**: the real
// `ComputeRawAlpha`'s not-enough-data gate is `!m_isFull && m_headIndex < m_tailCutoff + 1`
// (i.e. never reject once the ring buffer is full, regardless of count). This header instead
// gates on `count < tail_cutoff + 1` alone, which is behaviorally identical for every
// configuration this codebase actually constructs (the sole real instantiation,
// `ContextManager.h`'s default-constructed `TailRiskEngine` --
// `windowSize=500`/`tailPercent=0.05` -> `tail_cutoff=25` -- always has `windowSize >
// tail_cutoff + 1`, so "full" already implies "enough data" under the simpler gate too); a
// hypothetical config with `tail_cutoff >= windowSize` would behave differently here, but no
// such config exists anywhere in this codebase.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

inline std::optional<double> HillTailIndex(double* window, std::size_t count, std::size_t tail_cutoff) {
    if (count < tail_cutoff + 1) {
        return std::nullopt;
    }

    // Widen the sort to the largest k we might scan (up to 4x the configured tail cutoff, capped
    // at count-1) so the stability-region scan below has a real range to search, not just the
    // single configured k.
    const std::size_t maxK = std::min(tail_cutoff * 4, count - 1);
    if (maxK < 2) {
        return std::nullopt;
    }

    // Scan k in [10, maxK] -- very small k (2-6) yields extremely noisy per-k Hill estimates,
    // and the first-plateau-wins selector below is most exposed to locking onto a noise-driven
    // "plateau" precisely where k is smallest and variance is highest. Starting at k=10 removes
    // that exposure.
    constexpr std::size_t kMinScanK = 10;
    std::size_t startK = std::min(kMinScanK, maxK);
    if (startK < 2) {
        startK = 2;  // degenerate tiny-window configs only
    }

    std::partial_sort(window, window + static_cast<std::ptrdiff_t>(maxK) + 1, window + count,
                       std::greater<double>());

    // 2. Resnick & Stărică (1997) Hill-plot stability-region selector: compute Hill(k) across a
    // k-range via a running prefix log-sum (true O(maxK), not O(maxK^2)), find a stable window of
    // k where consecutive Hill(k) values stay within a fixed relative band of the window's
    // anchor, take its midpoint.
    std::vector<double> hillAtK;
    hillAtK.reserve(maxK - startK + 1);
    // Prime the prefix sum with the first `startK` log-magnitudes, so runningLogSum ==
    // sum_{i<k} log(x_i) at the top of each iteration. hillAtK index j corresponds to k = startK + j.
    double runningLogSum = 0.0;
    for (std::size_t i = 0; i < startK; ++i) {
        runningLogSum += std::log(window[i]);
    }
    for (std::size_t k = startK; k <= maxK; ++k) {
        if (k > startK) {
            runningLogSum += std::log(window[k - 1]);
        }
        const double threshold = window[k];
        if (threshold <= 1e-9) {
            hillAtK.push_back(4.0);
            continue;
        }
        const double logThreshold = std::log(threshold);  // hoisted: computed once per k, not per i
        const double logSum = runningLogSum - static_cast<double>(k) * logThreshold;
        hillAtK.push_back(logSum > 1e-9 ? static_cast<double>(k) / logSum : 4.0);
    }

    // Hill-plot bias grows with k (more of the bulk distribution, further from the true tail), so
    // prefer the FIRST plateau reached scanning outward from small k, not whichever plateau
    // happens to be numerically widest anywhere in the range. kMinPlateauWidth=5 requires a
    // plateau to span at least 5 k-values before it's trusted as real rather than sampling noise.
    constexpr double kStabilityBandRelative = 0.15;  // 15% band
    constexpr std::size_t kMinPlateauWidth = 5;
    std::size_t bestStart = 0, bestLen = 1;   // fallback: widest plateau found, if none meets kMinPlateauWidth
    std::size_t firstStart = 0, firstLen = 0; // first plateau meeting kMinPlateauWidth (preferred)
    std::size_t start = 0;
    for (std::size_t i = 1; i <= hillAtK.size(); ++i) {
        const bool brokeOrEnd = (i == hillAtK.size()) ||
            (std::fabs(hillAtK[i] - hillAtK[start]) / std::max(hillAtK[start], 1e-6) > kStabilityBandRelative);
        if (brokeOrEnd) {
            const std::size_t runLen = i - start;
            if (runLen > bestLen) {
                bestLen = runLen;
                bestStart = start;
            }
            if (firstLen == 0 && runLen >= kMinPlateauWidth) {
                firstStart = start;
                firstLen = runLen;
            }
            start = i;
        }
    }
    const std::size_t chosenStart = (firstLen > 0) ? firstStart : bestStart;
    const std::size_t chosenLen = (firstLen > 0) ? firstLen : bestLen;
    if (hillAtK.empty()) {
        return std::nullopt;
    }
    const std::size_t midpointIdx = chosenStart + chosenLen / 2;
    return hillAtK[std::min(midpointIdx, hillAtK.size() - 1)];
}
