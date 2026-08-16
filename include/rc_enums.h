#pragma once

// RcEnums.h — canonical, ACSIL-independent home for enums that must mirror
// lbrnet's Python core/rc_enums.py for cross-language parity (name chosen to match
// that module directly). Extracted from Indicator.h so headers composing them
// (e.g. PredatorContext.h) stay includable with just `-I include` (no sierrachart.h
// on the path) — same rationale as MacdEnum's extraction into IndicatorComputations.h.
//
// Only HMMStateEnum lives here today. MacdEnum/KangarooTailEnum/TurtleSoupEnum/
// MomentumPinballEnum/ElderBreakoutEnum/NR7Enum remain in IndicatorComputations.h
// (already ACSIL-independent, already have live call sites) — moving them here is
// a separate, non-surgical consolidation left for its own future change, not bundled
// into this one. Any new cross-language-parity enum should land here, not spawn
// another single-enum header.

#include <cstdint>

/**
 * HMMStateEnum mirrors Python rc_enums.HMMStateEnum for cross-language parity.
 *
 * IDs are a fixed canonical convention, not a raw EM output: EM state indices
 * are arbitrary per-fit (label-switching) and are NOT guaranteed to match
 * this convention on their own. The Python training pipeline's state-alignment
 * step (lbrnet/docs/architecture/HMM_STATE_ALIGNMENT_SPEC.md) reorders each
 * freshly-fit model's raw arrays before saving so that raw_id matches these
 * ids by construction -- this side never runs independent HMM inference, it
 * only deserialises the already-resolved hmm_state_id Python sends over the
 * wire (HMMClient::HandleBinaryResponse -> MutableHmmState()).
 * No UNKNOWN member — use HMM_NO_PRIOR constexpr for uninitialised state.
 *
 * Identified by RULE, not by fixed numeric centroids (any specific centroid
 * values are retrain-specific and go stale the moment the feature pipeline
 * or training data changes -- this bit us once already: an earlier version
 * of this comment cited z-score-era centroid values from before the
 * redundant Python-side normalization step was removed, 2026-08-02):
 *   COILED_SPRING:    quiet (low vol_convexity/relative_range) AND fleeting
 *                     (low tenure/occupancy) -- rare, short-lived compression
 *   GAUSSIAN_STABLE:  quiet AND persistent (high tenure/occupancy) -- the
 *                     common, long-lived baseline regime
 *   GAUSSIAN_FRAGILE: DOF < 4.0 (Student-t kurtosis-undefined threshold,
 *                     Kotz & Nadarajah 2004) OR low-tenure/high-entropy
 *                     chaos -- the "doesn't look like a clean archetype"
 *                     bucket; also absorbs what MarketClimate labels
 *                     SHANNON_CHAOS below, since HMMStateEnum has no chaos
 *                     slot of its own
 *   PARETO_MOMENTUM:  high burstiness AND positive relative_range --
 *                     directional thrust
 * See lbrnet.models.regime_registry.compute_hmmstate_affinity() (Python) for
 * the exact, executable form of these rules.
 */
enum class HMMStateEnum : int8_t {
    COILED_SPRING    = 0,
    GAUSSIAN_STABLE  = 1,
    GAUSSIAN_FRAGILE = 2,
    PARETO_MOMENTUM  = 3
};

/// Pipeline sentinel: HMM inference not yet received. Not a trained model state.
constexpr HMMStateEnum HMM_NO_PRIOR = static_cast<HMMStateEnum>(-1);
