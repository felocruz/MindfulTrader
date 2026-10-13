// LogScaleRatio.h -- pure, header-only, ACSIL-independent port of the ratio-of-bipower-variations
// math inside StudyHelperFunctions.cpp's CalculateLogScaleRatio.
//
// Canonical metric: log(short_BV / long_BV) -- a short-vs-long-horizon scale ratio (HAR-RV/Corsi
// 2009-style multi-horizon volatility clustering), using Barndorff-Nielsen & Shephard (2004, 2006)
// bipower variation (BipowerVariation.h's ComputeBipowerVariation, already its own ported/
// shadow-wired primitive) instead of raw sample variance for each window -- raw sample variance is
// fragile under this system's fat-tailed tick data, a single-tick jump dominates the sum of
// squares quadratically (Mandelbrot 1963; see lbrnet/logs/rc_gemini.log CLAUDE_BRIEF_118/118_REPLY
// for the full literature-grounding).
//
// Deliberately takes the two ALREADY-COMPUTED bipower-variation values, not raw log-return
// windows -- `CalculateLogScaleRatio` itself already computes `short_bv`/`long_bv` via
// `ComputeBipowerVariation` (independently shadow-checked at that call site), so this header only
// needs to own the ratio+clamp step; recomputing BV again in here would duplicate work the caller
// already did and already validated.
#pragma once

#include <algorithm>
#include <cmath>

inline float LogScaleRatio(double short_bv, double long_bv) {
    constexpr double kVarEps = 1e-12;
    const double log_ratio = std::log((short_bv + kVarEps) / (long_bv + kVarEps));
    return std::clamp(static_cast<float>(log_ratio), -6.0f, 6.0f);
}
