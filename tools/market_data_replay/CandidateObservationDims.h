// CandidateObservationDims.h — the single source of truth for which
// ObservationData dims this tool exports to Parquet.
//
// STATUS CHANGE (operator directive, 2026-09-16): dim selection is no longer
// done via Gaussian-based metrics in this tool (chi-squared gating / Feature
// Saliency EM) -- the HMM this vector feeds is a Student-t (fat-tailed) model,
// and a Gaussian selection criterion is the wrong tool for choosing its
// inputs. This list now holds ALL 18 schema dims, unfiltered -- lbrnet does
// the real dim-selection work by training the actual Student-t HMM on the
// full vector and testing which dims don't belong. The 10-dim subset used
// during 2026-09-09..2026-09-15 (docs/superpowers/specs/2026-09-09-market-
// data-replay-dim-selection-spec.md) is superseded, not deleted from history
// -- that spec's own §3c fix (no double-normalization) still applies, it
// just now runs over 18 dims instead of 10 (see CandidateTriggerGate.h).
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

// Indices into MTS::Schema::Contract's kObservationDim=18 space -- all 18,
// in schema field order (operator directive, 2026-09-16: let lbrnet's own
// Student-t HMM training decide which dims to drop, not a Gaussian gate here).
inline constexpr std::array<std::size_t, MTS::Schema::Contract::kObservationDim> kCandidateDims = {
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
    MTS::Schema::Contract::kObsFastMeanRevZ,  // wired 2026-09-17 to enable the previously-blocked
                                               // HMM cross-state discrimination test -- the
                                               // 2026-09-04 "do not wire on raw predictive power"
                                               // decision stands and is not being re-litigated
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
