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
// Includes RcEnums.h (not Indicator.h) for HMMStateEnum, matching PredatorContext.h's own
// ACSIL-independence convention — this header must stay includable with just `-I include`.

#pragma once

#include "FusionKey.h"
#include "RcEnums.h"  // HMMStateEnum

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
