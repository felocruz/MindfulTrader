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

## 0a. Status update, 2026-09-15 — Task 13/14 confirmed unwired; real clock-mismatch root cause found

A `full_fidelity_smoke` run (`lbrnet/data/raw/full_fidelity_smoke.context` → `.parquet`, 34.8M
ticks, 2026-09-14) reproduced the same high significant-change rate this spec was written to fix
(58.5% on the full 476.7M-tick run, `tools/output/market_data_replay_20260914_084354.txt`; 69.6% on
a 50M-tick smoke sample, `tools/output/market_data_replay_20260914_214750.txt`) — despite the
implementation plan's Task 13/14 checklist marking the `CandidateTriggerGate`/direct-Parquet wiring
`[x]` done. Direct code inspection (not the checklist) shows what actually shipped:

- `MarketDataReplayEngine.h` still declares `otg::ObservationTriggerGate m_triggerGate` (the full
  18-dim shared gate) plus a second `otg::ObservationTriggerGate m_correctedOrderGate` and a
  `TriggerDiagnostics` counter block, all clearly added *after* Task 13 to re-investigate this same
  problem on the full dataset — `CandidateTriggerGate`/`CandidateObservationDims.h` are `#include`d
  by nothing except their own standalone unit test.
- The CLI (`MarketDataReplay.cpp`) still writes through `ContextFileWriter` (the full `.context`
  binary format, all 18 `ObservationData` + 8 `AsymmetryContext` + `RiskGateContext` fields),
  confirmed directly against the real Parquet schema — not the flat N-candidate-column direct-Parquet
  writer Task 14 describes.
- **RESOLVED, 2026-09-15: confirmed via `git log --follow` + `git show`, not left an open question.**
  `ea752c4` (`git log --follow -- tools/market_data_replay/MarketDataReplayEngine.h`, the only
  commit besides the original `82b4010` to touch this file) **already correctly wired
  `mdr::CandidateTriggerGate` + the direct-Parquet writer** — verified via `git show
  ea752c4:tools/market_data_replay/MarketDataReplayEngine.h` (`mdr::CandidateTriggerGate
  m_candidateTriggerGate;` present) and `git show ea752c4:tools/market_data_replay/
  MarketDataReplay.cpp` (`parquet::arrow::FileWriter::Open` present). The full-gate/`.context`
  version was **uncommitted working-tree changes** (`git status` showed `MM` against both files) —
  a genuine revert-for-diagnosis, never committed, still sitting in the working tree as of
  2026-09-15. Preserved, not discarded: `git stash push -m "diagnostic revert to full
  ObservationTriggerGate for root-cause investigation..."` (`stash@{0}`) before restoring the
  clean, correctly-wired `ea752c4` state and building the fix (§3a) on top of it. Net: Task 16
  (below) turned out to be "restore the committed state", not "re-wire from scratch".

**Root cause, confirmed empirically against the real `full_fidelity_smoke.parquet`** (not just
theorized): `ObservationTriggerGate`'s 40-tick rolling window assumes every pushed sample is a
roughly-independent draw from a stationary distribution. Several dims violate this badly — they
update at bar-close / price-change-gated / N-sample-window cadence, not per tick, so they sit frozen
at one identical value for far longer than 40 ticks:

| dim | frac. unchanged tick-to-tick | longest frozen run (ticks) |
|---|---|---|
| `relative_range` | 99.7% | up to 35,119 |
| `fast_taleb_kurtosis` | 99.3% | up to 4,086 |
| `tail_index` | ~83-88% | up to 1,883 |
| `log_scale_ratio` | ~82-85% | up to 184 |
| `liq_fragility` | ~0.7% (changes almost every tick — not degenerate) | 43 |

Whenever one of the frozen dims finally steps to a new value, its trailing 40-sample window is
(almost) entirely the old frozen value → median/MAD collapse to ~0 → the code's variance floor
(`kVarEps=1e-6f`) produces a z-score amplified roughly 1000x for that one tick, trivially crossing
`kEnergyFastTrackZ=3.0`. This is the same failure class as the earlier `burstiness_index`
point-mass degeneracy (`CLAUDE.md`'s own account) — a tick-clock rolling window applied to an
update-clock (event-indexed) signal. **Two of the four affected dims (`tail_index`,
`fast_taleb_kurtosis`) are already excluded from the 10-dim candidate list below, but two are not**
(`log_scale_ratio`, `relative_range`) — so simply re-wiring `CandidateTriggerGate` (§3) is necessary
but not sufficient; the gate's own history-sampling design needs the fix in §3a.

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

## 3a. Fix attempt #1: event-indexed (push-on-change) candidate history, 2026-09-15 —
SUPERSEDED by §3c (implemented, native-tested, then found empirically insufficient on real data)

**Status: implemented and native-tested, 2026-09-15**, then **superseded the same day** once a
real-data validation run showed it did not fix the actual problem (see §3c) — kept here, not
deleted, because the underlying diagnosis (dims 0/2 freezing for tens of thousands of ticks) was
real and correct; the fix for it just wasn't sufficient on its own. Two changes, tool-local only
per §2's scope discipline:

1. **Push-on-change, not push-per-tick.** `PushObservation()` now appends a new sample into a
   dim's history **only when that dim's value differs from the last value pushed for it by more
   than `kFreezeEpsilon=1e-6f`** (one `m_lastPushed`/`m_hasLastPushed` entry per dim). This makes
   the median/MAD estimate reflect genuine update-to-update variability for low-cadence dims,
   instead of being dominated by hundreds/thousands of duplicate entries between updates.
   High-cadence dims (e.g. `liq_fragility`, already changing on ~99% of ticks in real data) are
   unaffected in practice.
2. **Compute-before-push ordering.** `MarketDataReplayEngine::ComputeShouldEmit()` now calls
   `ComputeTriggerDecisionMetrics()` *before* `PushObservation()` for the same tick — evaluating
   against history that does not yet include the point under test, removing the self-referential
   contamination of comparing a value to a distribution that already contains it. Deliberately
   **not** ported back to `include/ObservationTriggerGate.h` yet (§2/§4).
3. **Empirical re-validation against real data**: see §3b below for the resolved target-rate
   question and the actual measured result.

### 3b. Design questions raised against fix attempt #1 — RESOLVED, 2026-09-15 (moot after §3c)

These were answered as asked, but the underlying gate they describe was itself replaced by §3c
the same day — kept for the record, not because they still apply to the current design.

- **Per-dim independent buffer lengths — RESOLVED, implemented.** `CandidateTriggerGate` no longer
  assumes lockstep filling. Added `AllDimsWarmedUp()` (true only once every dim independently holds
  `kMinSamples` distinct pushed values), `PerDimSampleCounts()` (diagnostic, one count per dim), and
  redefined `SampleCount()` to report the minimum across dims (the true warm-up bottleneck) rather
  than a single proxy dim's count. `ComputeTriggerDecisionMetrics()` now uses each dim's own
  `m_history[dim].size()` in its loop instead of one shared `n`. A real test-fixture consequence
  surfaced while validating this (not a gate bug): the pre-existing Task 8 engine-test fixture used
  a perfectly regular hourly tick cadence with no intrabar range, which made `burstiness_index`
  (measures inter-*cluster* arrival-time regularity) and `liq_fragility` (needs real intrabar range
  + ≥50 cumulative live-bar volume) permanently stuck at 1-2 samples — never a real-world condition
  (confirmed via `full_fidelity_smoke.parquet`, where `liq_fragility` changes on ~99% of real
  ticks), but the fixture needed genuine per-tick and per-cluster randomization to reach warm-up.
  Fixed in `test_market_data_replay_engine.cpp`'s own Task 8 block (clustered intra-bucket ticks
  with randomized timing/volume/spacing) — all 10 candidate dims now warm up and the fixture's own
  regime-shift assertion passes.
- **Target rate — RESOLVED: not a fixed target, an empirical measurement.** Confirmed the
  ~12-14% figure was indeed a short-sample artifact of the old push-per-tick gate, not a validated
  ground truth — abandoned as an acceptance gate. The actual measured rate post-fix, against real
  tick data, is recorded in Task 18's own log entry (see implementation plan) rather than compared
  against this stale number.
- **`kBaseEpsilon=4.0` — RESOLVED: re-derived and confirmed correct, not changed.** For
  `kCandidateDimCount=10` independent standard-normal z's, `sum(z_i^2) ~ chi-squared(10)`. The 90th
  percentile of chi-squared(10) is 15.987, and `sqrt(15.987) = 3.998 ≈ 4.0` — i.e. the existing
  constant already corresponds to a principled ~10% base (noise-only) trigger rate at this
  dimensionality, not an arbitrary carry-over. This derivation is now written directly into
  `CandidateTriggerGate.h` as a code comment, not left implicit. Left unchanged pending the real
  measured rate (previous bullet) — the theoretical derivation assumes near-independent unit-normal
  z's, an assumption the push-on-change fix makes approximately true in practice for the first time
  (it was badly violated before: MAD collapse produced z's in the hundreds/thousands). Re-derive if
  `kCandidateDimCount` changes.

## 3c. The real fix: no double-normalization — compute the distance directly from
FeatureScaler's already-scaled output, 2026-09-15 — IMPLEMENTED and empirically confirmed

**Status: implemented and native-tested** (`tools/market_data_replay/CandidateTriggerGate.h`
rewritten, `tools/market_data_replay/test_candidate_trigger_gate.cpp` rewritten, 11/11 pass; 75/75
pre-existing engine tests still pass unchanged).

**What went wrong with §3a/§3b**: a real 50M-tick full-density run with that fix applied still
showed a **71.0% significant-change rate** — barely different from the 69.6% measured before the
fix, despite the frozen-value degeneracy being definitively gone (confirmed by unit test). An
independent second opinion was sought — **Gemini CLI, invoked directly and read-only**
(`--approval-mode plan`, structurally incapable of editing files), analysis captured verbatim in
`docs/Gemini.md` — which correctly diagnosed the real problem: `CandidateTriggerGate` was running
its own separate median/MAD rolling-window re-normalization **on top of** `FeatureScaler`'s output,
which is *already* a properly-scaled, robust, institutional-grade z-score (rolling Soft-Log-Z/
Log-Z, shrinkage-corrected). Several candidate dims (`amihud_illiquidity`, `liq_fragility`,
`hurst_exponent`, `mean_rev_z`) are computed reactively on essentially every tick (confirmed:
`liq_fragility` changes on ~99% of real ticks) — so push-on-change never skipped them at all, and
the gate's own 40-sample window re-normalized a continuously-drifting, already-scaled sequence,
independently reproducing the same MAD-collapse failure mode one layer up. A per-dim diagnostic
(`tools/market_data_replay/diagnose_real_data_trigger.cpp`) confirmed this empirically on real data:
`amihud_illiquidity` (mean|z|=31.6, max|z|=11,107) and `liq_fragility` (mean|z|=12.4,
max|z|=71,821) were wildly disproportionate vs. the other 8 dims (mean|z| ~1-3) — exactly the
continuously-reactive dims the hypothesis predicted.

**The fix**: `CandidateTriggerGate` no longer maintains any rolling window, median, or MAD of its
own. It is now a stateless function of the current tick's already-scaled candidate vector:
`distance = sqrt(sum(currentObs[dim]^2))`, `significant = distance >= kBaseEpsilon` (kBaseEpsilon=
4.0 unchanged — the chi-squared(10) derivation is now genuinely applicable, since `currentObs` IS
FeatureScaler's own properly-scaled output, not a re-derived quantity). The only remaining state is
`HasBaseline()`/`SetBaseline()`, unrelated to the distance computation — it just forces the very
first post-warmup tick to emit unconditionally (matches live `ContextManager.cpp`'s
`ShouldTriggerHMM(hmm_initialized, significant_change)` precedent). `push-on-change`/
`AllDimsWarmedUp()`/per-dim rolling history are gone entirely — no longer needed, since
`FeatureScaler`'s own 500-sample warmup (already checked upstream in `ComputeShouldEmit()`) is the
only warm-up gate the candidate vector needs.

**Empirical confirmation, before implementing the real gate change**: the diagnostic tool computed
both metrics side-by-side on the same 8,000,000 real ticks — the OLD (double-normalized) gate gave
73.17%, a **direct** sum-of-squares metric on the same data gave **7.14%**, matching the
chi-squared(10) 90th-percentile prediction (~10%) closely. Only then was the real gate rewritten to
match. **Full real-dataset validation, completed**: the entire 476,745,947-tick `mes_ticks.parquet`
run with the real fix applied gave a final **5.07% significant-change rate** (24,170,802 records
written, `tools/log/market_data_replay.log`) — sane, in the same order of magnitude as both the
8M-tick sample and the chi-squared(10) theoretical prediction, and a training-data volume that is
actually usable (vs. the ~340M records the pre-fix 71-73% rate would have produced).

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
  - [ ] `include/ObservationTriggerGate.h` also needs §3c's real fix ported over — the shared gate
    has the exact same double-normalization structure (its own energy/geometry median/MAD channels
    computed over an already-scaled `currentObs`) that §3c found and fixed here. Until this lands,
    live `ContextManager.cpp` keeps the same double-normalization exposure for any dim reactive on
    most ticks (`amihud_illiquidity`/`liq_fragility` confirmed as the worst offenders here — likely
    true there too, not yet independently confirmed against live data).
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

## 0b. Status update, 2026-09-16 — Gaussian dim-selection superseded; candidate set now ALL 18 dims

**Operator directive, 2026-09-16**: this tool's own Gaussian-based dim-selection machinery
(`CandidateTriggerGate`'s chi-squared gate, the never-implemented Feature Saliency EM path) was
the wrong tool for selecting inputs to a **Student-t** (fat-tailed) HMM — a Gaussian selection
criterion biases toward dims that look well-behaved under a Gaussian assumption, not dims that
carry real information for a fat-tailed model. Dim selection is now lbrnet's job: train the actual
Student-t HMM on the full 18D vector and test per-dim relevance from there, not a pre-filter here.
The schema stays at 18D permanently either way (already decided, §2/§4 unchanged).

Consequently:
- `CandidateObservationDims.h`'s `kCandidateDims` now holds all 18 schema dims (was the 10 ledger
  `IN*` dims) — `fast_mean_rev_z` included only as the already-decided-DROP zero-sentinel (never
  wired, per `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` row 19).
- `MarketDataReplayEngine.h`'s 7 previously compute-skipped dims (`tail_index`,
  `log_scale_expansion_ratio`, `micro_asymmetry`, `skewness_idx`, `fast_taleb_kurtosis`,
  `fast_hurst_exponent`, `recurrence_rate`) are now genuinely computed every tick/bar, ported
  directly from their real production formulas (`TailRiskEngine::GetHillAlpha()`,
  `StudyHelperFunctions.cpp`'s bipower-variation construct, `OrderFlowAsymmetryEngine.h`,
  `RobustMoments.h`'s `MoorsKurtosis`/`BowleySkewness`, `DfaHurstExponent`,
  `RecurrenceRateEngine`/`RQAEpsilonSelector.h`) — not placeholder/cold-start values.
- `CandidateTriggerGate`'s row-thinning role is unchanged (still a pure sampling-cadence heuristic,
  §3c's no-double-normalization fix still applies) but now runs over all 18 scaled dims instead of
  10 — `kBaseEpsilon` re-derived from chi-squared(18)'s 90th percentile: 4.0 → 5.1.
- 75/75 engine native tests + 11/11 gate native tests pass (several rewritten from "stays at
  frozen default, non-candidate" to "moves off default, finite, in-contract" for the 7 re-enabled
  dims). A bounded real-data smoke run (4M ticks, `mes_ticks.parquet`) confirmed the Parquet output
  now has all 18 named dim columns.
- `tools/bin/market_data_replay` rebuilt with this change.

## 0c. IN/OUT convergence workflow adopted, 2026-09-16 — mechanism built, no dim moved OUT yet

**Operator directive, 2026-09-16**: rather than shrinking the schema/output per dim decision,
dims move through an explicit IN/OUT lifecycle inside this tool only, converging on the final set
before any schema change:
- **IN** (default, currently all 18): computed for real every tick, included in
  `CandidateTriggerGate`'s Mahalanobis-style row-thinning distance.
- **OUT**: `MarketDataReplayEngine::ApplyOutDimZeroing()` forces a hard `0.0f` every tick
  (unconditionally, even pre-warmup); every per-dim compute site is individually guarded on
  `mdr::IsCandidateDim(...)` and skips its real math entirely when OUT (the actual "stop
  computing" savings); `CandidateTriggerGate` excludes it from the distance sum (it's simply not
  in `kCandidateDims`). lbrnet is expected to maintain its own ignore-list of OUT column names.
- **Output stays fixed at 18 columns always**, decoupled from the IN/OUT list —
  `MarketDataReplay.cpp`'s writer now iterates all `MTS::Schema::Contract::kObservationDim` (18)
  dims directly, not `mdr::kCandidateDims`, so moving a dim OUT never changes the Parquet schema.
- Only once the full set converges does this become a live-code refactor per §4's closing
  contract (schema prune + regen, `ObservationTriggerGate.h`/`ContextManager.cpp` updates).

**Status as of 2026-09-16: mechanism implemented and tested (75/75 engine + 11/11 gate native
checks pass), but no dim has actually been moved to OUT yet** — `CandidateObservationDims.h` still
lists all 18 dims IN (including `fast_mean_rev_z`, despite it being a separately-decided-DROP
dim per the elite-feature-set-curation ledger) pending the first real verdict from lbrnet's
Student-t HMM training/testing. Moving a dim from IN to OUT going forward is a one-line edit to
`kCandidateDims` in `CandidateObservationDims.h`; `CandidateTriggerGate`'s `kBaseEpsilon` must be
re-derived (chi-squared(new count) 90th percentile) whenever the IN count changes, per its own
header comment.

## 0d. Sibling `.alpha` generator thread — Task 11/12 shipped and smoke-tested, 2026-09-17

Genuinely separate tool/thread from this spec's own dim-selection scope (new CLI
`MarketDataReplayContext.cpp`, not `MarketDataReplay.cpp`), tracked here per the "kept fully
up to date" coordination convention rather than in a third doc. Full detail lives in
`docs/superpowers/specs/2026-09-16-market-data-replay-alpha-generator-spec.md` §13 — condensed
pointer:

- Ran clean against a real 100M-tick slice of `mes_ticks.parquet`: 5,483,891 `.context` records,
  21,362 `.alpha` records, RSS bounded 242MB→633MB, ~50 min. Both independent triggers (§2 of
  that spec) confirmed firing repeatedly against real data, not just in unit tests.
- `lbrnet`'s `build_directional_alpha.py` parsed the `.alpha` output and got through HMM-model
  load/warmup before stopping at a `materialize_hmm_features.py`-stamped provenance-manifest
  requirement — a real, separate lbrnet-side pipeline prerequisite, not a defect in this tool.
- Output files handed off to `lbrnet`: `lbrnet/data/raw/offline_replay_smoke_100m.context`/
  `.alpha` — deliberately non-production-named, **not** validated for numeric correctness against
  genuine SC-collected ground truth (that byte/value-parity check is still blocked on the missing
  current-schema v240 comparison file), safe for `lbrnet`-side code-path/refactoring use only, not
  as a correctness oracle.
- A full 471.9M-tick run (~4h projected, ~7GB `.context`) was offered but not yet launched.

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
