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
    constexpr float kLiveBarMinVolume = 50.0f;
    constexpr float kEps = 1e-6f;
    constexpr int kMid = kWindow / 2;

    std::array<float, kWindow> rangeScratch{};
    std::copy_n(rangeWindow, kWindow, rangeScratch.begin());
    std::nth_element(rangeScratch.begin(), rangeScratch.begin() + kMid, rangeScratch.end());
    const float medRange = rangeScratch[kMid];

    std::array<float, kWindow> volScratch{};
    std::copy_n(sqrtVolWindow, kWindow, volScratch.begin());
    std::nth_element(volScratch.begin(), volScratch.begin() + kMid, volScratch.end());
    const float medSqrtVol = volScratch[kMid];
    const float scaleRef = medRange / (medSqrtVol + kEps);

    if (liveVolumeSoFar < kLiveBarMinVolume || scaleRef < kEps) {
        return std::clamp(prev_fragility, 0.0f, 1.0f);
    }

    float barRange = liveBarRange;
    if (barRange < 0.00001f) barRange = 0.00001f;  // avoid div-by-zero on doji

    const float eta = barRange / (std::sqrt(liveVolumeSoFar) + kEps);
    const float fRaw = eta / scaleRef;
    const float logF = std::log(std::max(fRaw, 1e-6f));
    const float fragilityRaw = 1.0f / (1.0f + std::exp(-2.0f * logF));

    const float alpha = (fragilityRaw > prev_fragility) ? 0.30f : 0.15f;
    return std::clamp(alpha * fragilityRaw + (1.0f - alpha) * prev_fragility, 0.0f, 1.0f);
}

}  // namespace lfe
