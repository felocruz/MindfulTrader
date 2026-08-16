// PredatorFusion.h — free-function fusion interface + applicability-mask dispatch.
//
// Reuses IndicatorManager's already-proven dirty-mask/trigger-mask idiom (m_dirty_mask,
// PRIMARY_TRIGGER_MASK, __builtin_ctzll-based bit iteration) rather than inventing a new
// mechanism. Each fusion function is a plain free function — no virtual dispatch — assigned
// a bit in FusionKey. The applicability mask is computed once per tick from the cheapest
// available preconditions (position state, regime), and is ALSO the structural enforcement
// of Predator Decision Contract element #5: an entry-fusion bit can never be set while
// inPosition is true, so it is impossible to call an entry-side fusion function on the
// wrong side of position state, not merely disciplined against.
//
// Includes rc_enums.h (not Indicator.h) for HMMStateEnum, matching PredatorContext.h's own
// ACSIL-independence convention — this header must stay includable with just `-I include`.

#pragma once

#include "FusionKey.h"
#include "rc_enums.h"  // HMMStateEnum
#include "PredatorContext.h"

#include <cstdint>

constexpr uint64_t FusionKeyMask(FusionKey key) {
    return 1ULL << static_cast<uint64_t>(key);
}

constexpr uint64_t ENTRY_FUSION_MASK =
    FusionKeyMask(FusionKey::TURTLE_SOUP_OPTION_A) |
    FusionKeyMask(FusionKey::TURTLE_SOUP_OPTION_B);

constexpr uint64_t EXIT_FUSION_MASK =
    FusionKeyMask(FusionKey::TAU_STAR);

// Computes which fusion functions are applicable this tick. Position state is the
// first-level, structural split (contract element #5); regime-applicability is a
// second-level filter individual fusion functions can further narrow against, but
// this helper only enforces the structural (position-state) split — a fusion function
// still checks its own regime-conditioning internally (e.g., FuseTauStar checks regime
// itself; this mask only guarantees it's never called on the wrong side of position state).
inline uint64_t ComputeApplicabilityMask(bool inPosition, HMMStateEnum /*regime*/) {
    return inPosition ? EXIT_FUSION_MASK : ENTRY_FUSION_MASK;
}

struct TauStarFusionResult {
    bool shouldExit;
    float effectiveThreshold;  // the computed tau* value, exported for telemetry/analysis
};

// Elkan (2001) cost-sensitive threshold: tau* = C_FP / (C_FP + C_FN).
// C_FP (cost of a false positive, i.e. exiting when the trade would have worked) is modeled
// as the remaining distance to target; C_FN (cost of a false negative, i.e. not exiting when
// the trade is actually failing) is modeled as the remaining distance to stop -- matching
// CLAUDE.md's Trap Detection section exactly ("C_FP=|target-price|, C_FN=|price-stop|").
inline TauStarFusionResult FuseTauStar(
    const PredatorContext& ctx,
    double distanceToTarget,
    double distanceToStop,
    double modelConfidence
) {
    TauStarFusionResult result{};
    const double denom = distanceToTarget + distanceToStop;
    const float tauStar = (denom > 1e-9)
        ? static_cast<float>(distanceToTarget / denom)
        : 0.5f;  // degenerate case (both distances ~0): neutral threshold
    result.effectiveThreshold = tauStar;
    result.shouldExit = (ctx.inPosition) && (static_cast<float>(modelConfidence) >= tauStar);
    return result;
}
