// RobustMoments.h — pure, header-only Bowley skewness / Moors kurtosis over a
// fixed-size returns window. Zero ACSIL dependency, natively testable
// (tests/cpp/test_robust_moments.cpp), same extraction rationale as
// FeatureScaler.h/TailRiskEngine.h/InformationEngine.h. Implements Kim &
// White (2004)'s recommended robust replacement for moment-based
// skewness/kurtosis, per docs/superpowers/specs/2026-08-13-observation-
// vector-institutional-elevation-spec.md Unit 3.

#pragma once

#include "generated/mts_core.h"  // the formulas below are implemented in Rust (spec decision #42)

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

/// Bowley (1920) quartile skewness: (Q3 - 2*Q2 + Q1) / (Q3 - Q1).
/// Returns NaN if Q3==Q1 (degenerate window) -- caller must carry-forward,
/// same convention as the existing SKEW_VARIANCE_EPS guard it replaces.
inline float BowleySkewness(const std::array<float, 100>& returns) {
    return mts::observation_vector_bowley_skewness(returns.data(), returns.size());
}

/// Moors (1988) octile kurtosis: [Q(7/8)-Q(5/8)+Q(3/8)-Q(1/8)] / [Q(6/8)-Q(2/8)].
/// Returns NaN if Q(6/8)==Q(2/8) (degenerate window) -- caller must carry-forward.
inline float MoorsKurtosis(const std::array<float, 100>& returns) {
    return mts::observation_vector_moors_kurtosis(returns.data(), returns.size());
}

// ---------------------------------------------------------------------------------------------
// Pure ports of StudyHelperFunctions.cpp's CalculateRealizedKurtosis()/CalculateSkewness(),
// decoupled from SCStudyInterfaceRef (2026-09-20, offline/ACSIL-coupled AsymmetryContext parity
// work) -- so tools/market_data_replay/MarketDataReplayEngine.h can compute the same real
// values, not just the ACSIL-coupled path. Byte-for-byte the same math; the ACSIL-coupled
// StudyHelperFunctions.cpp functions now delegate here. Callers own the warmup gate (100-bar
// history required) -- these assume valid, sufficient input, same precondition style as
// StructureEngine::IsReady().
// ---------------------------------------------------------------------------------------------

/// closesNewestFirst[0]=current bar close .. [100]=100 bars ago (101 consecutive closes, newest
/// first). atrNewestFirst[0]=current ATR .. [19]=19 bars ago (20 consecutive ATR readings).
inline float ComputeRealizedKurtosis(const std::array<float, 101>& closesNewestFirst,
                                      const std::array<float, 20>& atrNewestFirst,
                                      float prevKurtosis, bool hasPrevValid) {
    constexpr float kNeutralBaseline = 1.23f;
    constexpr float kClampLo = 0.0f;
    constexpr float kClampHi = 5.0f;
    constexpr float kVarianceEps = 1e-10f;

    std::array<float, 100> returns{};
    for (int i = 0; i < 100; ++i) {
        returns[static_cast<size_t>(i)] =
            std::log(closesNewestFirst[static_cast<size_t>(i)] /
                      std::max(closesNewestFirst[static_cast<size_t>(i + 1)], 0.001f));
    }

    float meanRet = 0.0f;
    for (float r : returns) meanRet += r;
    meanRet /= 100.0f;

    float variance = 0.0f;
    for (float ret : returns) { const float diff = ret - meanRet; variance += diff * diff; }
    variance /= 100.0f;

    const float carriedKurtosis = hasPrevValid ? std::clamp(prevKurtosis, kClampLo, kClampHi) : kNeutralBaseline;
    if (variance < kVarianceEps) return carriedKurtosis;

    const float moorsKurtosis = MoorsKurtosis(returns);
    float kurtosis;
    if (std::isnan(moorsKurtosis)) {
        if (hasPrevValid) return carriedKurtosis;
        kurtosis = kNeutralBaseline;
    } else {
        kurtosis = moorsKurtosis;
    }

    const float atrCurrent = atrNewestFirst[0];
    float atrAvg = 0.0f;
    for (float a : atrNewestFirst) atrAvg += a;
    atrAvg /= 20.0f;
    const float volRatio = atrCurrent / std::max(atrAvg, 0.0001f);
    float regimeMult = 1.0f;
    if (volRatio > 1.3f) regimeMult = 1.25f;
    if (volRatio < 0.7f) regimeMult = 0.75f;
    kurtosis *= regimeMult;

    return std::clamp(kurtosis, kClampLo, kClampHi);
}

/// Same input shape as ComputeRealizedKurtosis. lastValidSkewness mirrors the ACSIL-coupled
/// function's sc.GetPersistentFloat() carry-forward state -- caller owns that storage.
inline float ComputeRealizedSkewness(const std::array<float, 101>& closesNewestFirst,
                                      const std::array<float, 20>& atrNewestFirst,
                                      float lastValidSkewness) {
    constexpr float kVarianceEps = 1e-10f;

    std::array<float, 100> returns{};
    for (int i = 0; i < 100; ++i) {
        returns[static_cast<size_t>(i)] =
            std::log(closesNewestFirst[static_cast<size_t>(i)] /
                      std::max(closesNewestFirst[static_cast<size_t>(i + 1)], 0.001f));
    }

    float meanRet = 0.0f;
    for (float r : returns) meanRet += r;
    meanRet /= 100.0f;

    float variance = 0.0f;
    for (float ret : returns) { const float diff = ret - meanRet; variance += diff * diff; }
    variance /= 100.0f;

    if (variance < kVarianceEps) return lastValidSkewness;

    const float bowleySkewness = BowleySkewness(returns);
    if (std::isnan(bowleySkewness)) return lastValidSkewness;
    float skewness = bowleySkewness;

    const float atrCurrent = atrNewestFirst[0];
    float atrAvg = 0.0f;
    for (float a : atrNewestFirst) atrAvg += a;
    atrAvg /= 20.0f;
    const float volRatio = atrCurrent / std::max(atrAvg, 0.0001f);
    float regimeMult = 1.0f;
    if (volRatio > 1.2f) regimeMult = 1.30f;
    if (volRatio < 0.8f) regimeMult = 0.80f;
    skewness *= regimeMult;

    return std::clamp(skewness, -1.5f, 1.5f);
}
