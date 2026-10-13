// Barndorff-Nielsen & Shephard (2004, 2006) bipower variation: a jump-robust
// estimator of the continuous-time variance component of a return series.
// Raw realized variance (sum of squared returns) lets a single outlier tick
// dominate the whole sum quadratically; BV only ever lets an outlier enter
// through two adjacent linear |return| products, so it stays far more
// stable when the underlying series is fat-tailed and jump-contaminated --
// see docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-
// brainstorm.md §5.1 and the log_scale_ratio literature-grounding thread
// (lbrnet/logs/rc_gemini.log, CLAUDE_BRIEF_118/118_REPLY) for the full
// grounding of why this replaces raw sample variance in
// CalculateLogScaleRatio. Same formula as tools/observation_vector/jump_ratio_stats.h's
// ComputeJumpRatio (validated against 38.5M real MES rows, 2026-08-30) --
// this header extracts the single-fixed-window primitive that formula
// embeds, for call sites (like a live per-tick short/long variance ratio)
// that need one window's BV value rather than a whole-series sliding scan.
#pragma once

#include "generated/mts_core.h"  // the formulas below are implemented in Rust (spec decision #42)

#include <cmath>

// BV = (pi/2) * sum_{k=1}^{n-1} |returns[k-1]| * |returns[k]|
// Undefined for n < 2 (no adjacent pair exists); returns 0.0 in that case,
// the same neutral value CalculateLogScaleRatio's raw-variance windows return
// for a degenerate (too-short) window.
inline double ComputeBipowerVariation(const double* returns, int n) {
    return mts::observation_vector_compute_bipower_variation(returns, n < 0 ? 0 : static_cast<std::size_t>(n));
}
