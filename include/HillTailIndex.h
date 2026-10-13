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

#include "generated/mts_core.h"  // the formulas below are implemented in Rust (spec decision #42)

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <optional>
#include <vector>

inline std::optional<double> HillTailIndex(double* window, std::size_t count, std::size_t tail_cutoff) {
    // Rust reports "no estimate" as NaN; the C++ API keeps std::optional. Sorts `window` in place.
    const double alpha = mts::observation_vector_hill_tail_index(window, count, tail_cutoff);
    if (std::isnan(alpha)) return std::nullopt;
    return alpha;
}
