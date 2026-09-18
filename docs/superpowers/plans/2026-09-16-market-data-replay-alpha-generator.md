# `.alpha` (TrainingEvent) Generator — Verification Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resolve every open question in
`docs/superpowers/specs/2026-09-16-market-data-replay-alpha-generator-spec.md` §5 via direct
source verification (real reads, real bare-compiles — not guesses), so an implementation plan can
be written with a concrete, correct task breakdown. **Verification phase COMPLETE (2026-09-16) —
all 6 tasks resolved, both scope decisions APPROVED by the operator (spec §7).** A follow-on
implementation plan is the next artifact to write.

**Spec:** `docs/superpowers/specs/2026-09-16-market-data-replay-alpha-generator-spec.md` — read in
full before starting; every task below maps 1:1 to one of its §5 open questions.

**Why verification-first**: this repo's own established convention (the HMM Regime Manager
spec/plan pair, `docs/superpowers/specs/2026-09-08-offline-context-generator-spec.md`'s own Task
1-12 breakdown) blocks implementation on resolved open questions, not the other way around — an
implementation plan written against unresolved unknowns (especially Task 3 below, flagged in the
spec as "the single biggest open risk") would have to be rewritten once the real answer changes
its scope. Cheaper to verify first.

**STATUS UPDATE (2026-09-16): all 6 verification tasks done, both decisions APPROVED.**
- **Scope (spec §7 item 1)**: proceed with `PRIMARY_TRIGGER_MASK` (17 keys) as a first slice;
  `SECONDARY_TRIGGER_MASK` explicitly deferred.
- **CLI entry point (spec §7 item 2)**: restore `.context`+`.alpha` writing as its own,
  separately-named CLI (e.g. `MarketDataReplayContext.cpp`), reusing the existing engine core;
  `MarketDataReplay.cpp`'s Parquet exporter stays untouched. `tools/README.md` gets corrected as
  part of implementation.

**This verification plan is now closed. A new implementation plan is the next artifact** — should
cover: the ~5 TA-primitive reimplementations (RSI, ATR, MACD/Impulse, Stochastic, EMA/SMA) +
their empirical validation, `NR7` extraction, bounded multi-bar history in the engine, the 17
`PRIMARY_TRIGGER_MASK` detector assembly, `AlphaFileWriter.h`, the new `MarketDataReplayContext.cpp`
CLI with `--emit-context`/`--emit-alpha` flags and single `--output` base path, and the
`tools/README.md` fix.

## Global constraints

- Every task is read-only source investigation (or, where noted, a scoped standalone bare-compile
  check) against real files — no edits to any production `.cpp`/`.h` under this plan.
- Apply the spec's §1d three-way classification (borrow the real vendored header where genuinely
  portable / mirror-with-citation for small stable constants / extract-to-pure-function otherwise)
  to every finding — don't re-derive a new disposition from scratch per task.
- Record findings directly in the spec (§1/§3/§5, matching each item's existing structure) as they
  resolve, not just in this plan — the spec is the single source of truth for the design; this plan
  tracks task status only.

---

### Task 1 (spec §5 item 1): Trace Lock A/B's exact readiness source

**Files:** `src/EventDataCollectorStudy.cpp` (read-only)

- [x] **Step 1:** Traced Lock A: `ContextManager::IsObservationSaturated()` →
  `m_featureScaler.warmedUp`, `sampleCount` vs `FeatureScaler::RANK_WINDOW`=500. Already resolved
  in a prior session (`SCRATCHPAD.md`'s "LockA audit — resolved, don't re-investigate") and
  already free for this initiative — `FeatureScaler` is already a dependency of the replay
  engine's existing 18D observation-vector work.
- [x] **Step 2:** Traced Lock B: `IndicatorManager::IsWarmedUp()` → `m_isWarmedUp`, set by
  `CheckWarmupStatus()` (called once per TS3 bar-close) once (a) a 200-bar counter is reached
  (trivial), (b) `GetValue<IndicatorKey::RSI>() != 0` (**the same RSI-reimplementation dependency
  already identified in Task 2** — not independent), and (c) `HmmState() != nullptr` (confirmed
  dead/always-true in practice — `HmmState()` returns the address of an always-constructed
  member, never actually null).
- [x] **Step 3:** Recorded in spec §3 item 1 and §5 item 1. Institutional recommendation: don't
  scope Task 1 as its own separate effort — fold it into Task 2's scope decision, since Lock B's
  only real cost (RSI) is identical to Task 2's already-identified cost.


### Task 2 (spec §5 item 2): Trace `HasSignificantChange()`'s real mechanism

**Files:** `include/IndicatorManager.h`, `src/IndicatorManager.cpp` (read-only)

- [x] **Step 1:** Confirmed: `HasSignificantChange()` (`src/IndicatorManager.cpp:1134`) is a
  per-`IndicatorKey` dirty-bit mask (`m_dirty_mask`), genuinely distinct from
  `ObservationTriggerGate`'s Mahalanobis-distance test — two tiers, `PRIMARY_TRIGGER_MASK` (17
  keys, fires unconditionally on any dirty bit) and `SECONDARY_TRIGGER_MASK` (remaining keys up
  to `MAX_INDICATORS`=55, also gated by `Scoring::IsIndicatorEventSignificant()`).
- [x] **Step 2:** Traced what the mask actually covers: the full Raschke/Elder pattern-detector
  indicator suite (Turtle Soup, Momentum Pinball, Elder Breakout, Kangaroo Tail, NR7, Raschke
  strategy/tactical setups, structure tests, MACD/impulse variants, oscillators, daily bias,
  ATR/EMA proximity, correlations — up to 55 `IndicatorKey` slots), computed by
  `TripleScreen1/2/3.cpp`. **None of this is currently reconstructed by
  `tools/market_data_replay/`** — the existing engine only computes the 18D HMM observation
  vector, a categorically smaller, separate set of computations. Reconstructing `.alpha`'s real
  trigger offline would require porting a large, currently-unscoped set of additional indicator
  computations.
- [x] **Step 3:** Recorded in spec §3 item 2 (full breakdown) and §5 item 2 (decision needed).
  **Not a mechanical follow-up task** — this is a scope/priority decision for the operator, not
  something to resolve unilaterally in this plan.
- [x] **Step 4 (added, feasibility assessment):** Investigated how hard porting
  `PRIMARY_TRIGGER_MASK`'s 17 keys actually would be, to turn the abstract scope decision into a
  concrete one. Findings (full detail in spec §3 item 2): the pattern-detector functions
  themselves (`DetectKangarooTail`/`DetectTurtleSoup`/`DetectMomentumPinball`/
  `DetectElderBreakout`) are already pure/portable in `include/IndicatorComputations.h`, zero new
  work; `NR7` needs extraction from `TripleScreen3.cpp` but its logic is simple (7-bar range
  comparison, no TA-library dependency); the real cost is that several detectors' upstream inputs
  (RSI, MACD/Impulse, ATR, Stochastic, EMA) are computed via genuine Sierra Chart BUILT-IN
  functions (`sc.RSI()`/`sc.MACD()`/`sc.ATR()`/`sc.Stochastic()`/`sc.MovingAverage()`) with no
  extractable source — confirmed via repo-wide grep that zero pure reimplementation of these
  exists yet. This is new work (reimplement + empirically validate ~5 standard TA formulas), but
  bounded and reusable across most of the 17 keys, not 17 independent efforts. The replay engine
  also needs a new bounded multi-bar history ring buffer (currently tracks only the current bar).
  Verdict: feasible at a real, moderate-to-significant, but now concretely scoped cost — recorded
  in spec §3 item 2 and §5 item 2's revised decision framing.

### Task 3 (spec §5 item 3, flagged in the spec as the single biggest open risk): Full audit of `GetTrainingEventT()`'s `sc`-coupled surface

**Files:** `src/IndicatorManager.cpp` (`GetTrainingEventT`, full body — read-only)

- [x] **Step 1:** Line-by-line read of the entire function (not sampled) — listed every `sc.*`
  reference, and every function it calls that itself might touch `sc.*` transitively
  (`PopulateIndicatorState()`, `ContextManager::AddToTrainingEventFB()`,
  `InferenceManager::AddToTrainingEventFB()`, `GetTickCompanionValues()`,
  `SyncFeatureVector()`, `WriteTrainingRootSharedFields()`).
- [x] **Step 2:** Classified every reference against spec §1d — result: only 4 genuine `sc`
  touches in the whole call graph (`bar_index`, `timestamp_us`, OHLC, volume), all
  extract-to-pure; every other call (`PopulateIndicatorState`, `GetTickCompanionValues`,
  `InferenceManager::AddToTrainingEventFB`, `SyncFeatureVector`,
  `WriteTrainingRootSharedFields`) is already `sc`-free, and `ContextManager::
  AddToTrainingEventFB`'s `sc` parameter is dead (name commented out, unused in the body).
  `SCDateTime` borrowing turned out unnecessary here — the replay engine already has the raw
  epoch-microsecond timestamp directly.
- [x] **Step 3:** Recorded the full per-field disposition table in spec §3 item 3; §5 item 3
  marked resolved.

### Task 4 (spec §5 item 4): Confirm `model_confidence` placeholder decision

**Files:** `src/EventDataCollectorStudy.cpp`, `src/TradeSignalManager.cpp` (read-only)

- [x] **Step 1:** Confirmed — and stronger than expected: `TradeSignalManager::SetTradeSignal()`
  (the only writer of `m_hasFreshSignal`) has zero callers anywhere in `src/`/`include/` (verified
  by grep). `HasFreshSignal()` returns `false` unconditionally today, in production too, not just
  offline. `0.0f` is therefore not an offline approximation — it's the only value this field ever
  takes in the current system.
- [x] **Step 2:** Recorded in spec §5 item 4 — decision: use `0.0f` unconditionally, no caveat
  needed.

### Task 5 (spec §5 item 5): Audit `BuildRiskGateContext()`'s portability

**Files:** `src/ContextManager.cpp` (`BuildRiskGateContext`, read-only)

- [x] **Step 1:** Read the full function body (`src/ContextManager.cpp:851`) — confirmed already
  pure: `const`, zero-parameter, just a 17-field copy from `m_localRiskContext` into
  `RiskGateContextT`. No `sc`/ACSIL dependency.
- [x] **Step 2:** One real, small gap found (not a portability risk): the replay engine currently
  computes each raw pre-scaling value as a transient local (e.g. `meanRevZ` at
  `MarketDataReplayEngine.h:728`) and discards it after writing the scaled observation — it
  doesn't retain them in a persistent struct yet. Recorded in spec §5 item 5 as a small "retain
  what's already computed" plumbing item, not new computation.

### Task 6 (spec §5 item 6): Decide output file naming/pairing

**Files:** none — design decision only, informed by Tasks 1-5's findings

- [x] **Step 1:** DECIDED: single `--output <path>` base-path argument, no separate
  `--alpha-output`. The tool derives `<path>.context`/`<path>.alpha` internally, mirroring
  `LBRFileManager::Open()`'s own convention. Grounded in real evidence: `lbrnet/lbrnet/scripts/
  build_directional_alpha.py` already auto-detects the sibling `.context` file this exact way
  (same-stem, swapped extension) — a separate argument would work against that existing logic.
  Recorded in spec §5 item 6.
- [x] **Step 2 (added, discrepancy found):** `tools/market_data_replay/MarketDataReplay.cpp`
  currently writes directly to Parquet (2026-09-09 dim-selection pivot), not `.context` —
  `ContextFileWriter.h` has zero includers in the current source tree despite `tools/README.md`
  still describing this file as the `.context` generator's CLI driver. A separate, older compiled
  binary (`tools/bin/market_data_replay_full`) is the last artifact of the original `.context`-
  writing CLI. Flagged in spec §5 item 6 as needing resolution during implementation (which entry
  point does this initiative actually extend?) — not resolved in this verification-only plan.

---

## Not yet scoped (deferred until this plan closes)

- This plan is now CLOSED — all 6 tasks done, both decisions approved (spec §7).
- **Implementation plan written**: `docs/superpowers/plans/2026-09-16-market-data-replay-alpha-
  generator-implementation.md` — 13 tasks, covering the TA-primitive reimplementations, `NR7`
  extraction, bounded multi-bar history, the 17 `PRIMARY_TRIGGER_MASK` detector assembly,
  `AlphaFileWriter.h`, the new `MarketDataReplayContext.cpp` CLI, validation (partially blocked —
  needs a fresh current-schema SC-collected file), and the `tools/README.md` fix.
