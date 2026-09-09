# Market Data Replay Dim-Selection Spec

**Status: OPENED 2026-09-09.** Scope: `tools/market_data_replay/` only. No changes to `../schema/
mts_schema.fbs`, `include/generated/`, `src/ContextManager.cpp`, `src/EventDataCollectorStudy.cpp`,
or any other live-execution-path file until dim selection concludes (see §4).

## 0. Why this exists

A real full-dataset run of `market_data_replay` (471.9M ticks) showed a much higher
significant-change rate than expected, traced to the shared `ObservationTriggerGate`'s Mahalanobis
computation running over all 18 `ObservationData` dims, including several the
`docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` §7 ledger has not
endorsed (`OUT-HMM`/`DROP` rows, and dims absent from the ledger entirely). A shared-header fix was
attempted, then reverted (`git checkout -- include/ObservationTriggerGate.h`) — it changed live
`ContextManager.cpp` behavior, which is out of scope until Feature Saliency EM / HMM training has
actually concluded which dims belong in the final vector.

## 1. Goal

Select the best `ObservationData` dims via Feature Saliency EM + Student-t HMM training, using
`tools/market_data_replay/` as the sole experimentation vehicle. The candidate dim set is expected
to **evolve** over the course of this work — starting from the ledger's 10 explicit `IN*` dims, but
not necessarily ending there. Iterate until HMM training performance is judged reasonable.

## 2. Scope discipline (non-negotiable per operator directive, 2026-09-09)

- **Only `tools/market_data_replay/` changes.** No edits to `../schema/mts_schema.fbs`,
  `include/generated/*_generated.h`, `include/ObservationTriggerGate.h`, `src/ContextManager.cpp`,
  `src/EventDataCollectorStudy.cpp`, `src/TripleScreen*.cpp`, `src/StudyHelperFunctions.cpp`, or any
  other live-execution-path file — not even to skip computation there. All 18 dims' real
  computation stays fully intact and untouched in the live codebase throughout this entire effort.
- **No computation code is removed anywhere** until dim selection concludes (§4) — this spec's own
  tool-local engine may *skip calling* a compute path for a non-candidate dim (for speed, §3), but
  the underlying calculator headers/functions themselves (`DfaHurstExponent`, `RecurrenceRateEngine`,
  etc.) are not touched, deleted, or modified.
- **A local, tool-scoped copy of the significant-change gate** — sized to the evolving candidate
  dim count, not the fixed 18 — replaces this tool's use of the shared
  `include/ObservationTriggerGate.h`. The shared header itself is not modified.

## 3. Design

- **`CandidateObservationDims.h`** (new, `tools/market_data_replay/`): defines the current candidate
  dim set as an explicit, named list (starting point: the 10 ledger `IN*` dims — `log_scale_ratio`,
  `burstiness_index`, `relative_range`, `lempel_ziv`, `hurst_exponent`, `fisher_info`,
  `amihud_illiquidity`, `liq_fragility`, `fractal_dim`, `mean_rev_z`). This is the single place the
  candidate set is edited as it evolves — every other tool-local piece derives its dimensionality
  from this list, not a hardcoded `10`.
- **`MarketDataReplayEngine`** only computes the dims in the current candidate list — skips calling
  the compute path entirely for non-candidate dims (not just discarding the result), for the real
  speedup a full 471.9M-tick run benefits from.
- **`CandidateTriggerGate.h`** (new, tool-local): a copy of `ObservationTriggerGate`'s
  median/MAD Mahalanobis significant-change logic, sized to the candidate dim count from
  `CandidateObservationDims.h` (not a fixed 18). Not shared with `include/ObservationTriggerGate.h`.
- **Output: direct-to-Parquet, no `.context` intermediate.** This codebase already writes Parquet
  from C++ (`tools/context_pipeline/context_to_parquet.cpp`, `tools/scid_processing/
  scid_to_ticks_parquet.cpp`, both using `parquet::arrow::FileWriter`; Arrow/Parquet 22.0.0 already
  in the `mts` mamba env) — no new library work needed. The market_data_replay CLI writes a flat
  N-column (N = candidate dim count) Parquet file directly, using `context_to_parquet.cpp`'s
  columnar-builder + chunked-write pattern, skipping `ContextFileWriter`/the `.context` binary
  format entirely for this experimentation phase.

## 4. Closing contract (the reason this spec exists, not just the implementation plan)

**Once Feature Saliency EM + HMM training conclude which dims belong in the final vector**, this
becomes a live-code refactor task — explicitly out of scope until then, tracked here so it isn't
forgotten once dim-selection experimentation itself feels "done":

- [ ] **Task: refactor the live codebase to match the final selected dim set.** Not started, not
  startable until dim selection concludes. Concretely:
  - [ ] `../schema/mts_schema.fbs`'s `ObservationData` struct is pruned to the final selected dims
    (run `regenerate_schema.sh` after, per `CLAUDE.md`'s hard requirement — never hand-edit
    generated headers).
  - [ ] `include/ObservationTriggerGate.h` (the shared, live gate) is updated to match
    `CandidateTriggerGate.h`'s final dim set.
  - [ ] `src/ContextManager.cpp` is updated: stop computing any dropped dim **unless** it has a
    real non-observation-vector consumer (see the audit item below) — this is where computation
    code actually gets removed, not before.
  - [ ] `src/EventDataCollectorStudy.cpp` is checked and updated if needed (its own `.context`/
    `.alpha` write path may reference dropped fields directly, separately from `ContextManager`).
  - [ ] `src/TripleScreen1.cpp`/`TripleScreen2.cpp`/`TripleScreen3.cpp` are updated: remove any
    dropped dim's UI subgraph/computation that has no other consumer.
  - [ ] `FeatureScaler.h`'s positional arrays (`DIM_WINSOR_SIGMA_OVERRIDE`,
    `LOGZ_WINSOR_SIGMA_OVERRIDE`, `SHRINKAGE_SCALE_MIN`, `SCALE_MODE_MAP`, etc.) are re-derived for
    the new, smaller layout — this is the exact index-shift-bug risk class
    `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` has already hit
    twice; budget real care for it, not a mechanical resize.
  - [ ] **Per-dropped-dim non-observation-vector-consumer audit** (must happen before any
    computation is removed, not after): grep every dropped dim's real name/accessor across
    `src/PositionManager.cpp`/`RiskManager.cpp`/`Scoring.cpp`/`TradeDecisionEngine.h` (and any
    other live consumer) — a dim dropped from `ObservationData` is not automatically safe to stop
    computing if something else still reads it. Known real cases already found by this spec's own
    investigation, to re-verify at refactor time (may have changed by then):
    - `tail_index` → `PositionManager.cpp` position sizing (`ComputeHillTailIndexProxy`,
      `RiskManager::GetHillTailIndexProxyMax`).
    - `fast_taleb_kurtosis` → `RiskManager`'s kurtosis emergency-halt gate, `Scoring.cpp`,
      `PositionManager`'s chase logic (`LocalRiskContext.fastTalebKurtosis`).
  - [ ] Full rebuild (`./build_dll.sh`, not `--no-clean`, given the schema/PCH sensitivity already
    seen this session) + native test suite pass before considering this task done.

## 5. Cross-references

- `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` §7 — the ledger,
  starting point for the candidate dim list (§3).
- `docs/superpowers/plans/2026-09-08-market-data-replay-implementation.md` — the original
  implementation plan (Tasks 1-12); this spec's tasks are appended there as a new phase, not a
  fork.
- `docs/superpowers/specs/2026-09-08-offline-context-generator-spec.md` — the original tool design
  this dim-selection phase builds on top of (engine, tick aggregation, `FeatureScaler` reuse).
- `tools/context_pipeline/context_to_parquet.cpp` — the Parquet-write pattern this phase's CLI
  output adapts.
