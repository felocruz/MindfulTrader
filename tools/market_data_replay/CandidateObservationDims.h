// CandidateObservationDims.h — the single source of truth for which
// ObservationData dims this tool exports to Parquet.
//
// STATUS CHANGE (operator directive, 2026-09-16): dim selection is no longer
// done via Gaussian-based metrics in this tool (chi-squared gating / Feature
// Saliency EM) -- the HMM this vector feeds is a Student-t (fat-tailed) model,
// and a Gaussian selection criterion is the wrong tool for choosing its
// inputs. This list held ALL 18 schema dims, unfiltered, from 2026-09-16 --
// lbrnet does the real dim-selection work by training the actual Student-t
// HMM on the full vector and testing which dims don't belong. The first real
// verdict landed 2026-09-18 (fast_mean_rev_z moved OUT, see below); the 10-dim
// subset used during 2026-09-09..2026-09-15 (docs/superpowers/specs/
// 2026-09-09-market-data-replay-dim-selection-spec.md) remains superseded,
// not deleted from history -- that spec's own §3c fix (no double-
// normalization) still applies, it just now runs over 17 dims (see
// CandidateTriggerGate.h).
//
// Every other tool-local file (CandidateTriggerGate.h, the engine, the CLI's
// Parquet writer) derives its dimensionality from kCandidateDims, never a
// hardcoded count -- so this remains the one place to edit if the exported
// set ever needs to change again.
//
// Scope discipline (dim-selection spec §2, still honored): this file is
// confined to tools/market_data_replay/. It does not modify, and is not used
// by, include/ObservationTriggerGate.h or any live-execution-path file --
// the schema itself stays at 18D permanently (operator directive, 2026-09-16).

#pragma once

#include "generated/mts_schema_contract_generated.h"

#include <array>
#include <cstddef>

namespace mdr {

// Indices into MTS::Schema::Contract's kObservationDim=18 space -- 17 of 18,
// in schema field order (operator directive, 2026-09-16: let lbrnet's own
// Student-t HMM training decide which dims to drop, not a Gaussian gate here).
// Size is explicit (not kObservationDim) -- std::array does NOT infer its
// size from the initializer list here, so it MUST match the element count
// below exactly or the array silently pads with zero-valued (duplicate
// dim-0) entries.
inline constexpr std::array<std::size_t, 17> kCandidateDims = {
    MTS::Schema::Contract::kObsLogScaleRatio,
    MTS::Schema::Contract::kObsBurstinessIndex,
    MTS::Schema::Contract::kObsRelativeRange,
    MTS::Schema::Contract::kObsLogScaleExpansionRatio,
    MTS::Schema::Contract::kObsLempelZiv,
    MTS::Schema::Contract::kObsHurstExponent,
    MTS::Schema::Contract::kObsMicroAsymmetry,
    MTS::Schema::Contract::kObsFisherInfo,
    MTS::Schema::Contract::kObsFastHurstExponent,
    MTS::Schema::Contract::kObsTailIndex,
    MTS::Schema::Contract::kObsSkewnessIdx,
    MTS::Schema::Contract::kObsAmihudIlliquidity,
    MTS::Schema::Contract::kObsLiqFragility,
    MTS::Schema::Contract::kObsFastTalebKurtosis,
    MTS::Schema::Contract::kObsRecurrenceRate,
    MTS::Schema::Contract::kObsFractalDim,
    MTS::Schema::Contract::kObsMeanRevZ,
    // kObsFastMeanRevZ moved OUT 2026-09-18: the measurement this dim was
    // wired in for (2026-09-17) concluded -- lbrnet's Student-t HMM training
    // found it collapses one state to 0.39% occupancy (Celeux & Durand
    // pathology, a genuinely-varying but non-predictive dim fragmenting a
    // spurious state), vs. healthy occupancy across all 4 states at 17D.
    // Reconfirms the 2026-08-31/2026-09-04 DROP decision this dim already
    // carried. Live src/ContextManager.cpp keeps computing/emitting it
    // (out of this tool's scope, and other consumers may still want it) --
    // this only removes it from the HMM-training candidate view.
};

inline constexpr std::size_t kCandidateDimCount = kCandidateDims.size();

// True iff `dim` (an index into the full 18D ObservationData space) is a
// current candidate. Linear scan is fine -- called O(18) times per tick at
// most, never in a hot inner loop.
inline bool IsCandidateDim(std::size_t dim) {
    for (std::size_t c : kCandidateDims) {
        if (c == dim) return true;
    }
    return false;
}

}  // namespace mdr
