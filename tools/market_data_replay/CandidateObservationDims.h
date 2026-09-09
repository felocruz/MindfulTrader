// CandidateObservationDims.h — the single, evolving source of truth for which
// ObservationData dims this dim-selection phase currently treats as candidates
// (docs/superpowers/specs/2026-09-09-market-data-replay-dim-selection-spec.md
// §3). Starting point: the 10 dims carrying an explicit "IN*" status in
// docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md
// §7. Edit this list, and only this list, as the candidate set evolves --
// every other tool-local file (CandidateTriggerGate.h, the engine, the CLI's
// Parquet writer) derives its dimensionality from kCandidateDims, never a
// hardcoded count.
//
// Scope discipline (dim-selection spec §2): this file is confined to
// tools/market_data_replay/. It does not modify, and is not used by,
// include/ObservationTriggerGate.h or any live-execution-path file.

#pragma once

#include "generated/mts_schema_contract_generated.h"

#include <array>
#include <cstddef>

namespace mdr {

// Indices into MTS::Schema::Contract's kObservationDim=18 space -- one entry
// per current candidate dim, ledger rows 1/2/3/6/7/9/13/14/17/18.
inline constexpr std::array<std::size_t, 10> kCandidateDims = {
    MTS::Schema::Contract::kObsLogScaleRatio,       // ledger row 1
    MTS::Schema::Contract::kObsBurstinessIndex,     // ledger row 2
    MTS::Schema::Contract::kObsRelativeRange,       // ledger row 3
    MTS::Schema::Contract::kObsLempelZiv,           // ledger row 6
    MTS::Schema::Contract::kObsHurstExponent,       // ledger row 7
    MTS::Schema::Contract::kObsFisherInfo,          // ledger row 9
    MTS::Schema::Contract::kObsAmihudIlliquidity,   // ledger row 13
    MTS::Schema::Contract::kObsLiqFragility,        // ledger row 14
    MTS::Schema::Contract::kObsFractalDim,          // ledger row 17
    MTS::Schema::Contract::kObsMeanRevZ,            // ledger row 18
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
