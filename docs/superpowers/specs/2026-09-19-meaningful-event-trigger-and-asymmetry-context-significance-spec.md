# Meaningful Event-Trigger Definition + `AsymmetryContext` Transformer-Significance Gate — Spec

**Status**: design/brainstorm only, nothing implemented. Opened 2026-09-19 from an offline-vs-
ACSIL-path `.alpha`/event trigger comparison, escalated once the same gap was confirmed to exist in
**both** C++ code paths this repo has, not just the offline generator.

**Status correction (operator, 2026-09-19): this system has never been deployed to production, at
any point.** Nothing here is being framed against "live trading risk" — there is no real trading
to risk. `SCStudies.cpp`'s ACSIL-coupled call chain is **not a source of truth** just because it's
the code path Sierra Chart would eventually run; it is exactly as much a work-in-progress as the
offline `market_data_replay` tool, and both currently share the same design gap. The offline path
is, if anything, the **better** place to design and validate the fix first — it replays real MES
tick history at full scale, fast, with no Sierra Chart/ZMQ counterpart required — and the validated
result then gets ported into the ACSIL-coupled path as the implementation eventually destined for
production, not the other way around.

**Why this still matters, reframed**: §2 below confirms the same gap (`AsymmetryContext` has zero
dirty-mask integration, so its own changes never independently cause an event/`.alpha` write) exists
in the ACSIL-coupled code path's call chain too — meaning whichever version of this code eventually
ships to production would inherit this same gap unless it's fixed now, in whichever path is fastest
to validate it in (the offline one).

---

## 1. What "meaningful change" currently means, and where it's incomplete

Both C++ code paths currently decide "does this tick warrant capturing a training/inference event"
off the same underlying design (the ACSIL-coupled path literally shares one function;
the offline path has its own separate analog) — see `IndicatorManager::HasSignificantChange()`
(`src/IndicatorManager.cpp:1089`):

- **Categorical/enum `IndicatorState` fields** (patterns, RSI/Stochastic/ATR/EMA proximity):
  correctly implement "meaningful = a real state transition" via `EnteredOrExitedNone()`/
  threshold-crossing logic in each leaf class's `ShouldTrigger()` override.
- **5 `PRIMARY_TRIGGER_MASK` keys that were never given a real `ShouldTrigger()` override**
  (`STRUCTURE_TEST`, `RASCHKE_STRATEGY_SETUP`, `RASCHKE_TACTICAL_TRIGGER`, `VOLUME_SIGNAL`,
  `DAILY_BIAS`) — confirmed via direct read of `include/Indicator.h`: `StructureTestIndicator`,
  `RaschkeStrategyIndicator`, `RaschkeTacticalIndicator`, `VolumeIndicator`, `DailyBiasIndicator`
  all silently inherit the base class's `ShouldTrigger() { return false; }`. Being dirty (value
  changed) is necessary but **never sufficient** for these 5 — verified NOT a DOD-refactor
  regression (Task 9's devirtualized `CheckTrigger()` is a faithful, confirmed-correct port of this
  pre-existing behavior). **`STRUCTURE_TEST`'s `FAILED_*` values are this system's own definition of
  TRAP** — meaning a TRAP moment has never, in any version of this codebase, been independently
  capable of causing a training/live event write. It only gets captured when it coincides with some
  other indicator's real trigger on the same tick — a coincidence, not a mechanism.
- **`AsymmetryContext` (the Transformer's own direct 8D input) has ZERO dirty-mask integration at
  all** — worse than `STRUCTURE_TEST`. `ContextManager::GetAsymmetryContext()` just snapshots
  `m_latestInstitutionalMetrics` fresh into whatever event happens to be built; its own changes were
  never even considered as a trigger candidate, structurally, not as an oversight-within-the-mask
  system.

## 2. Confirmed: the gap exists in the ACSIL-coupled code path too, not just the offline generator

Checked `docs/ADR/risk_gate_context_wire_spec.md`: `asymmetry_context (AsymmetryContext) →
transformer embedding` — confirmed direct Transformer input, and confirmed **disjoint** from the
HMM's own input (`HMMClient.cpp:416`'s own comment: `asymmetry_context` is omitted from the
`MarketObservation` sent to the HMM — `"8D Context is Transformer-Only (via Event stream)"`).

Traced the ACSIL-coupled call chain directly (this is the code path Sierra Chart would run — not
currently deployed anywhere, but still worth tracing precisely since it's what the fix eventually
needs to land in):
- `src/SCStudies.cpp:447` (the main ACSIL entry point, every tick) calls
  `IndicatorManager::Instance().PublishEventOnChange(sc)`.
- `PublishEventOnChange()` (`IndicatorManager.cpp:984`): `if (!HasSignificantChange()) return false;`
  — then calls `SendEventFlatBuffer(sc, false)`.
- `SendEventFlatBuffer()` builds `auto asym_ctx = ContextManager::Instance().GetAsymmetryContext();`
  and serializes it directly into the Event sent over port 5555 — the path intended to eventually
  carry real-time Transformer inference, whenever this system is deployed.

**So `HasSignificantChange()` is a single, shared gate for two consumers today**: (a) the
ACSIL-coupled Event publish (port 5555, the eventual real-time-inference path), and (b)
`EventDataCollectorStudy.cpp`'s `.alpha`/`TrainingEvent` capture (via the same call, behind Locks
A/B/D/E). The offline `market_data_replay` tool has its own separate, non-shared
`ConsumePatternDirtyMask()` analog. **A fix to `HasSignificantChange()` itself automatically fixes
both (a) and (b)** — that's the leverage point on the ACSIL-coupled side. But per the status
correction above, the better order of operations is to prototype and validate the fix in the
offline path first (fast, real-data-validated, no Sierra Chart needed), then port the validated
logic into `HasSignificantChange()` for the ACSIL-coupled path.

**Concrete consequence, once this system is eventually deployed**: if `AsymmetryContext` moves
meaningfully (a real kurtosis spike, a real entropy regime shift) but no discrete `IndicatorState`
transition happens to co-occur on that exact tick, the Transformer would keep operating on a
**stale** `AsymmetryContext` snapshot until the next unrelated trigger fires — an unbounded,
unmeasured staleness window. Worth fixing now, before deployment, precisely because it's cheap to
fix in the offline path today and expensive to discover after the fact.

## 3. The refined principle (operator's proposed test, refined for continuous fields)

"A meaningful change is a change in a value that feeds the Transformer" is the right axis, but taken
completely literally it re-creates the exact failure mode this repo already diagnosed and fixed
once for a different vector — the 2026-09-08 "quality over quantity" correction to
`ContextManager::ShouldTriggerHMM()`, which replaced "any dim moved by ~1e-5" with a real
Mahalanobis significant-change threshold specifically because near-continuous emission on
continuous-valued dims drowns the signal (Rydén/Teräsvirta/Åsbrink's own cited argument for
lower-frequency, information-rich sampling over HMM-style state models).

Refined, by field type:
- **Discrete/categorical Transformer inputs** (patterns, `STRUCTURE_TEST`, MACD-cross/trend enums,
  correlation-direction enums): meaningful = a real state transition. Same idiom already correct
  for the 5 patterns/RSI/Stochastic/ATR/EMA — `STRUCTURE_TEST` (and probably the other 4 dead keys)
  just need the same treatment they were apparently never given.
- **Continuous Transformer inputs** (`AsymmetryContext`'s 8 dims): meaningful = a magnitude-based
  significant change, analogous to `ObservationData`'s existing Mahalanobis gate — not "changed,"
  but "changed enough to matter." This is a **new, third trigger axis**, not a tweak to the
  existing categorical one.

## 4. Brainstorm: designing Trigger 3 (`AsymmetryContext` magnitude-significance)

### 4a. Where it plugs in

```
HasSignificantChange() =
    TriggerViaIndicatorState()          // existing: PRIMARY/SECONDARY dirty-mask + Scoring path
    OR TriggerViaAsymmetryContextMagnitude()   // NEW: Trigger 3
```

A short-circuit OR at the top of `HasSignificantChange()` is the minimal-surface-area change for
the ACSIL-coupled path — both its Event publish (`SendEventFlatBuffer`) and
`EventDataCollectorStudy.cpp`'s training-collection call this one function, so neither call site
needs its own new logic. The offline `market_data_replay` tool's `ConsumePatternDirtyMask() != 0`
check needs its own mirrored addition — and per §6 below, is actually the recommended place to
prototype this logic FIRST, before porting it into `HasSignificantChange()`.

### 4b. Baseline/reset semantics

Needs its own "last published `AsymmetryContext`" baseline (mirrors `ObservationData`'s Mahalanobis
gate's own `SetBaseline()`-after-accepted-trigger convention) — compare the *current* tick's
`AsymmetryContext` against the snapshot as of the **last successful publish** (from either trigger
axis, since a fresh full snapshot goes out either way), not against the previous tick's value (which
would let slow drift escape detection indefinitely) and not against a fixed reference (which would
never adapt to genuine regime baseline shifts).

Ownership question, not yet decided: this baseline plausibly belongs on `ContextManager` (which
already owns `m_latestInstitutionalMetrics`/`GetAsymmetryContext()`), with `IndicatorManager::
HasSignificantChange()` calling a new `ContextManager::HasAsymmetryContextMovedSignificantly()`
query — mirrors the existing pattern where `HasSignificantChange()`'s secondary path already calls
out to `ContextManager::Instance().GetLocalRiskContext()` for `Scoring::IsIndicatorEventSignificant()`.

### 4c. Threshold calibration — real-data-derived, not invented (per this project's standing rule)

Three of the 8 dims are plausible reuse candidates for *already-calibrated* thresholds, because
they trace to the same underlying quantities as existing, already-calibrated `ObservationData`/
`RiskGateContext` dims (verified via direct code reads, not assumed):
- `shannonFlowEntropy`/`shannonEfficiency` — same `m_latestInstitutionalMetrics` fields already
  feeding `RiskGateContext`'s calibrated gates.
- `talebKurtosis` — same live-computed calendar-clock kurtosis already feeding `RiskGateContext`'s
  5 kurtosis checks (Kim & White 2004 Moors-kurtosis-based, already percentile-calibrated on real
  data).
- `raschkeBurst` — confirmed (`ContextManager.cpp:1075`) to literally be
  `CalculateBurstinessIndex(now_us)`, the **same function** that produces `ObservationData`'s
  `burstiness_index` dim (`FeatureScaler`-covered, real-data-calibrated, Daley & Vere-Jones
  Index-of-Dispersion-for-Counts formulation, 2026-09-02 fix).

The other dims (`talebSkewness`, `elderChandelierATR`, `paretoRot`, `elderImpulse`) either have a
weaker/no direct overlap with an existing calibrated dim, or (per `ContextManager.h`'s own
naming-mismatch note) are a known Mandelbrot-pillar value sitting in a Pareto-named field —
`paretoRot` specifically needs its own fresh look before assuming any existing calibration applies.

**Recommendation, not yet decided**: reuse/adapt existing `FeatureScaler` calibration for the 3
confirmed-identical-quantity dims (cheap, already real-data-validated); derive fresh thresholds for
the remaining dims via the same EVT/GPD or percentile-matching discipline already standard in this
repo (`docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`'s own methodology) —
**do not invent placeholder constants**, per this project's own standing rule.

### 4d. Cross-repo finding, 2026-09-19: Trigger 3 must export a per-field wire bitmask, not just gate internally

`lbrnet`'s own session (working the Transformer's `hints` mechanism, `2026-09-19-asymmetry-
context-change-hints-spec.md`) independently identified a load-bearing assumption: their generic
"did this field change between published events" hint (`col[1:] != col[:-1]`) is only meaningful
if C++ holds `AsymmetryContext` bit-identical between real recomputes. **Verified false, not just
unverified** — traced all 7 live wire fields to their C++ source call sites
(`docs/HMM_REGIME_MANAGER_COORDINATION.md` Entry 22 has the full per-field table): every one is
recomputed every tick or every bar-close, unconditionally, fully decoupled from whether
`HasSignificantChange()` ever fires. Two consecutive *published* events' `AsymmetryContext`
snapshots will differ from ordinary drift almost every time regardless of causal relevance — the
naive hint is structurally degenerate (near-100% "changed"), not merely noisy.

**Design consequence**: Trigger 3's per-field significance decision (§4a-4d above) needs to be
exported on the wire as an explicit per-field bitmask/flags, not just consumed internally by
`HasSignificantChange()` — this is the same artifact `lbrnet`'s Option A needs for `hints`. One
shared deliverable, computed once in C++, not two independently-derived (and driftable)
thresholds on either side of the wire. This adds a schema-field requirement to §4's design that
wasn't present before this cross-repo exchange.

### 4e. Concrete design, 2026-09-19: extend `changed_mask: uint64` with `AsymmetryContext` bits (PSC-05, schema repo)

**Corrected same day — narrower than first proposed, most of the original scope already existed.**
The `IndicatorState`-bits half of this design turned out to already be live: `Event.changed_mask`
(`schema/mts_schema.fbs`) already existed before this proposal was written — bits are exactly
`IndicatorKey`'s enum values, populated from `IndicatorManager::GetDirtyMask()` in
`EventSerializer.cpp`, missed on first pass. `TrainingEvent` lacked the same field entirely — a
real, separate gap (since `lbrnet`'s `hints` mechanism operates on `TrainingEvent`/`.alpha` data,
not the live `Event` stream) — **added and wired same day**: `TrainingEvent.changed_mask: uint64`
(schema), populated via `GetDirtyMask()` in `GetTrainingEventT()` (`IndicatorManager.cpp`), full
clean build passes.

**The "snapshot before clear" prerequisite was also verified unneeded, not just deferred**: traced
the actual call chain (`HasSignificantChange()` → `SendEventFlatBuffer()`/`GetTrainingEventT()` →
`PublishEventOnChange()`'s `m_dirty_mask = 0` flush) and confirmed nothing in between mutates
`m_dirty_mask` — Task 9's devirtualized `PopulateIndicatorState()` has no dirty-clearing side
effect (unlike the old per-indicator `AddToTrainingEventFB()`/`ExtractInt8AndClearDirty()` calls
this spec originally assumed were still in the hot path — they aren't; those methods still exist
but are only called by objects outside the `m_dirty_mask` system entirely, e.g.
`InferenceManager`'s HMM/prediction/climate state). `m_dirty_mask` is already stable at the exact
moment `HasSignificantChange()` confirms true, all the way through to the explicit flush.

**Only genuinely remaining scope**: extend `changed_mask` (both tables, now real on both) with 8
new high bits, one per `AsymmetryContext` field in wire-declaration order: `shannon_entropy`=55,
`shannon_efficiency`=56, `taleb_kurtosis`=57, `taleb_skewness`=58, `taleb_cliff`=59,
`roughness_ratio`=60, `raschke_burst`=61, `session_quality_score`=62 (bit 63 reserved) — set only
when Trigger 3's per-field significance test fires for that dim. Still fully blocked on Trigger 3
(§4a-4d above), which remains design-only, not calibrated.

### 4f. Open questions, genuinely unresolved

1. **RESOLVED by §4e's bitmask design**: per-dim independent thresholds, not a combined
   Mahalanobis distance — `changed_fields_mask` needs to know *which* of the 8 fields moved, which
   a single combined pass/fail distance cannot express.
2. Update cadence mismatch: `m_latestInstitutionalMetrics`'s 8 source fields are updated at
   different cadences internally (some per-tick, some per-bar via `UpdatePriceStructure`/
   `UpdateMarketPhysics`/`SetNormalizedAnchors`/`CheckAndTriggerHMM`) — does the significance check
   need to account for "this dim hasn't actually refreshed since the last check" the way
   `ObservationData`'s carry-forward convention does, or is comparing raw snapshot-to-snapshot
   sufficient? Not decided.
3. Exact ownership/call shape (§4b) — `ContextManager` vs. `IndicatorManager` — not decided.
4. Does fixing `STRUCTURE_TEST`'s categorical trigger (§1) reduce Trigger 3's own necessary scope
   at all (i.e., do TRAP moments already tend to co-occur with a genuine `AsymmetryContext` swing,
   making the two fixes partially redundant in practice), or are they fully independent gaps? Not
   measured — real data would answer this, not assumed.

## 5. Recommended sequencing (not yet started)

1. Decide and implement a real, transition-based `ShouldTrigger()` for `STRUCTURE_TEST` at minimum
   (narrowest, most directly TRAP-relevant fix; the other 4 dead keys are a secondary decision).
   Prototype in the offline `market_data_replay` tool first (§6), not the ACSIL-coupled path.
2. Design + calibrate Trigger 3 for `AsymmetryContext` (§4), reusing existing calibration where a
   genuine quantity overlap is confirmed, deriving fresh thresholds elsewhere. Same offline-first
   discipline.
3. Port both validated fixes into the ACSIL-coupled path's `HasSignificantChange()` — the code path
   Sierra Chart would eventually run, not currently deployed anywhere.
4. Flag to `lbrnet`'s own coordination log once implemented — this changes the density/freshness
   characteristics of `asymmetry_context`, a value they already train the Transformer on; worth a
   heads-up before they draw conclusions from data collected under the old, gap-having behavior.

## 6. Rollout plan (operator directive, 2026-09-19: prototype offline first, decouple by risk)

The two fixes in this spec have different risk/effort profiles and must not ship as one change.
Given the system has never been deployed to production, "rollout" here means "which C++ code path
to prototype and validate in first, before porting to the ACSIL-coupled path" — not production
deployment risk management.

**Phase 1 — `STRUCTURE_TEST` categorical fix (low-risk, well-understood idiom) — DECIDED AND
PROTOTYPED, 2026-09-19**:
1. **Semantic decided**: both `FAILED_*` (TRAP) and `DECISIVE_*` (REGIME_INVALIDATION) count as
   "actionable" — both are named, ADR-governed, labeler-relevant outcomes (the ADR splits them
   into two label classes, it doesn't say only one matters). `NONE`/`INSIDE_BAR`/`OUTSIDE_BAR` are
   the "neutral" set. Trigger fires unless BOTH endpoints of a transition are neutral — covers
   entering/exiting an actionable state (mirrors the existing pattern idiom) AND a direct
   actionable-to-different-actionable transition (unlike the patterns' gradation-of-one-signal
   enums, `StructureTest`'s non-neutral values are structurally distinct events, so suppressing
   actionable-to-actionable transitions the way `EnteredOrExitedNone` does for patterns would hide
   a real regime change). Implemented as `IsStructureTestSignificantTransition()`
   (`include/IndicatorComputations.h`), 12/12 new native tests pass
   (`tests/cpp/test_structure_test_significant_transition.cpp`).
2. **Prototyped in the offline `market_data_replay` tool first**, per the rollout philosophy above
   — wired into `MarketDataReplayEngine.h`'s `STRUCTURE_TEST` dirty-bit condition. Full existing
   engine test suite re-run clean (all pre-existing tests still pass, confirming no regression).
3. **Real-data A/B measurement: inconclusive, not a validation failure.** A 30M-tick slice of
   `mes_ticks.parquet` produced byte-identical `.alpha` output pre-fix vs. post-fix — the file
   contains real records (confirmed via hex inspection, not an empty-header artifact), so this
   means no isolated `FAILED_*`/`DECISIVE_*` transition happened to occur (without a co-occurring
   pattern trigger) in that particular slice, not that the fix has no effect. A larger or
   differently-positioned sample is needed for a real measured effect size — not yet done.
4. **Ported into the ACSIL-coupled path, 2026-09-19**: `IndicatorManager::CheckTrigger()`'s
   `STRUCTURE_TEST` case (the real devirtualized dispatch path) now calls
   `IsStructureTestSignificantTransition()` directly, matching the offline-prototyped logic
   exactly. `StructureTestIndicator::ShouldTrigger()` (`include/Indicator.h`) also updated for
   consistency, for any non-devirtualized caller. Full clean `./build_dll.sh` passes. **Phase 1 is
   now complete** on both code paths — only the real-data effect-size measurement (item 3) remains
   open, as a measurement task, not an implementation one.

**Phase 2 — Trigger 3 (`AsymmetryContext` significance), separate initiative, not rushed**:
1. Measure real per-dim distributions on the existing 471.9M-tick dataset.
2. Reuse `FeatureScaler`'s existing calibration for the 3 confirmed-identical-quantity dims
   (§4c) — cheap, already real-data-validated.
3. Fresh EVT/GPD or percentile-matching derivation for the remaining dims — no invented constants.
4. Calibrate to a target base rate (this repo's ~10% precedent, `CandidateTriggerGate::
   kBaseEpsilon`'s chi-squared derivation), not a guessed threshold.
5. Prototype and validate in the offline tool first, same as Phase 1; port into the ACSIL-coupled
   path only once validated.

**Cross-cutting, both phases**:
- Flag to `lbrnet`'s coordination log both before (heads-up on the coming distributional shift in
  `asymmetry_context`/event density) and after (what actually changed) — they train on data whose
  statistical character this changes.
- `PRODUCTION_TRIAGE.md`'s `§1`/`§1.1`/`NORTH_STAR_STATUS` gets updated in the same edit that ships
  either phase, per the Triage Protocol, as a readiness/correctness-finding record — not framed as
  managing real trading risk, since none exists yet.

## 7. Relationship to other open specs

- `docs/superpowers/specs/2026-09-16-market-data-replay-alpha-generator-spec.md` — the offline
  generator's own Locks D/E cold-start gap (found earlier the same session: `AllDimsReady()`
  required ~17 trading days of window-fill before any `.alpha` record could be written at all, vs.
  live's freshness-only Locks D/E) was a **separate, independent** contributor to the same
  `TRAP_*`/`EXIT_*` under-representation symptom that motivated this whole investigation. **Fixed
  2026-09-19**: `AlphaLocksPass()` now uses a new `IsTs1Ts2FreshForAlpha()` accessor ("has TS1/TS2
  closed at least one real bar," matching live's real staleness-based semantic) instead of
  `AllDimsReady()`'s full-window-population check — `AllDimsReady()` itself is unchanged and still
  correctly gates Trigger 1 (`.context`/HMM significant-change), which genuinely needs mature
  windows. New tests prove the two accessors now diverge as intended (fresh-for-alpha goes true
  almost 17 days before all-dims-ready does). This was independent of, not a substitute for, the
  `STRUCTURE_TEST`/Trigger-3 fixes above — both needed fixing, neither covers the other.
- `docs/superpowers/specs/2026-09-18-predator-sniper-execution-architecture.md` §2 — the Transformer
  input pipeline this spec's Trigger 3 directly feeds; cross-reference once Trigger 3 is designed
  further.
