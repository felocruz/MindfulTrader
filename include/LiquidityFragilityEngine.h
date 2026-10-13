// LiquidityFragilityEngine.h — pure, header-only liquidity fragility score
// (dim 12, liq_fragility), extracted from StudyHelperFunctions.cpp's
// CalculateLiquidityFragility so it can be natively unit-tested
// (tests/cpp/test_liquidity_fragility_engine.cpp) and reused by an offline,
// non-Sierra-Chart tool building the same 18D ObservationData vector directly
// from tick data. Same rationale/precedent as CarryForwardCalculators.h/
// OrderFlowAsymmetryEngine.h/MeanReversionCalculator.h.
//
// The caller (StudyHelperFunctions.cpp) owns the ACSIL bar-array pull
// (gathering the kWindow closed-bar range/sqrt-volume history plus the
// still-forming live bar's own range/volume-so-far); this file owns only the
// math: F_raw = eta_t / ScaleRef_W microstructure elasticity ratio (Foucault,
// Kadan & Kandel 2005; Morris & Shin 2004), mapped through a bounded sigmoid
// and EMA-blended against the previous reading. Full derivation lives in the
// wrapper's own doc comment (StudyHelperFunctions.cpp), preserved there since
// it explains WHY this formula exists, not just what it computes.

#pragma once

#include "generated/mts_core.h"  // the formulas below are implemented in Rust (spec decision #42)

#include <algorithm>
#include <array>
#include <cmath>

namespace lfe {

constexpr int kWindow = 30;

// rangeWindow/sqrtVolWindow: kWindow CLOSED bars' (high-low) and sqrt(volume)
// respectively (order doesn't matter -- only the median is used). liveBarRange/
// liveVolumeSoFar describe the still-forming current bar. Degenerate (thin
// live volume or a collapsed scale reference) carries prev_fragility forward.
// Contract: result in [0, 1].
inline float ComputeLiquidityFragility(const float* rangeWindow, const float* sqrtVolWindow,
                                        float liveBarRange, float liveVolumeSoFar,
                                        float prev_fragility) {
    return mts::observation_vector_compute_liquidity_fragility(rangeWindow, sqrtVolWindow, static_cast<std::size_t>(kWindow),
                                                               liveBarRange, liveVolumeSoFar, prev_fragility);
}

}  // namespace lfe
