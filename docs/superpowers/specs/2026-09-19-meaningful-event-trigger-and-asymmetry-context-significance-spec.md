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

**Correction, 2026-09-19 (same day, before implementation started): the "reuse" plan above is
blocked on verifying the reused thresholds are actually calibrated, not just labeled as such.**
Auditing each dim's real derivation found `taleb_kurtosis`'s thresholds are the only ones with a
saved, rerunnable, real-data pipeline; `taleb_skewness`'s citation is unverifiable (no script
survives), `raschke_burst`'s bounded-scale thresholds are an algebraic carry-forward of an
undocumented legacy constant, and `shannon_entropy`'s bands/halt-frac and `taleb_cliff`'s gate have
no derivation on record at all. See the new spin-off spec
`docs/superpowers/specs/2026-09-19-decision-boundary-calibration-tool-spec.md` for the full
per-field audit, assessment, and the coordinated calibration tool it proposes — Trigger 3's own
threshold work (this dim's `roughness_ratio`/`session_quality_score` gap included) is now sequenced
behind that tool's prospective-queue item, not directly behind this section's original plan.

**Prospective queue closed out, 2026-09-19 (same day)**: the calibration tool
(`docs/superpowers/plans/2026-09-19-decision-boundary-calibration-tool-implementation.md`) was
built and run against the full 471.9M-tick real dataset (16-thread row-group-sharded, ~2s wall
clock). Both previously-uncalibrated dims now have real, data-derived candidate thresholds:
`roughness_ratio` empirical-percentile (10% upper-tail) = **3.4898**, EVT-GPD p=1/N return level =
**6.2597** (Weibull/bounded, genuine finite endpoint, xi=-0.0948); `session_quality_score`
empirical-percentile (10% lower-tail) = **-0.66** (corrected same day -- see below; lands on a
known discrete session-quality level, expected given the dim's construction). Full derivation,
sample counts, and ledger references: that plan's Tasks 4-6. The 6 dims with pre-existing
production thresholds (§4c above) still need the retroactive audit (spec §7) before Trigger 3 can
trust them, per that spec's own sequencing.

**Real bug found and fixed, 2026-09-19 (same day, post-user-review)**: `ComputeSessionQualityScore()`
had invented its own `[-1,+1]` scale for `TimeOfDayEnum` instead of matching the canonical,
already-established `symmetric_val` per member already defined in `lbrnet/lbrnet/core/rc_enums.py`
("Elite v2.3: Quality-Based Symmetric Mapping") -- every other Transformer-facing categorical field
in this system already has exactly one canonical mapping there. Fixed to transcribe the real
values; the -0.8 threshold above was stale (didn't even exist on the corrected 7-value discrete
set) and has been superseded by -0.66. See `docs/superpowers/plans/2026-09-19-decision-boundary-
calibration-tool-implementation.md`'s own correction note and `tools/RECALIBRATION_LEDGER.md` for
the full before/after. **Bigger open question this raised, not yet resolved**: given `symmetric_val`
is this system's own established pattern for feeding categorical state to the Transformer, several
of `AsymmetryContext`'s other continuous fields (kurtosis, entropy, burst, skewness, roughness) may
be better redesigned as categorical enums with their own `symmetric_val` rather than requiring
Trigger 3's bespoke continuous magnitude-significance calibration at all -- see operator discussion,
not yet spec'd.

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

**Phase 1b — gap-hunt across the other 3 remaining `PRIMARY_TRIGGER_MASK` "always false" keys,
DECIDED AND IMPLEMENTED, 2026-09-19** (`RASCHKE_STRATEGY_SETUP`/`RASCHKE_TACTICAL_TRIGGER` were the
4th/5th; `DAILY_BIAS`/`VOLUME_SIGNAL` complete the set — same class of bug as `STRUCTURE_TEST`,
found by auditing every remaining `CheckTrigger()` case that still read `return false;` with no
corresponding leaf-class `ShouldTrigger()` override):
1. **`RASCHKE_TACTICAL_TRIGGER`** — the highest-stakes of the four: several of its 18 non-`NONE`
   states (`ITR_BREAKOUT_BUY/SELL`, `ITR_FADE_BUY/SELL`) have no other `IndicatorKey`
   representation anywhere in the system, meaning these setups could never independently cause a
   training/live event capture. Same finding for **`RASCHKE_STRATEGY_SETUP`** (~17 non-`NONE`
   states, e.g. `WHIPLASH`/`GHOST`/`SLINGSHOT`/`FIRST_CROSS`, none represented elsewhere).
   `VOLUME_SIGNAL` (`NORMAL`=neutral) and `DAILY_BIAS` (`PHYSICS_VETO_RANDOM_WALK`=neutral) are the
   same shape but lower stakes (context/gating signals, not standalone entry setups).
2. **Cheaper fix than `STRUCTURE_TEST`**: unlike `StructureTest`'s three-state neutral set, all four
   have a single clean neutral value, so no new custom function was needed — reused the existing
   `EnteredOrExitedNone` idiom. That idiom was generalized to `EnteredOrExitedNeutral<Enum>(prev,
   cur, neutralValue)` (`include/IndicatorComputations.h`, enum-typed, no packed-array/int8_t
   dependency) so both the offline (`MarketDataReplayEngine.h`, enum-typed values) and
   ACSIL-coupled (`IndicatorManager::CheckTrigger()`, int8_t from `m_packed`) paths share one
   implementation; `IndicatorManager.cpp`'s original int8_t-based `EnteredOrExitedNone` now
   delegates to it instead of duplicating the entered/exited logic.
3. **Both code paths fixed together** (not offline-first-then-port, since the idiom was already
   proven live for 5 other patterns — no new semantic to validate offline first): `CheckTrigger()`'s
   4 dead cases now call `EnteredOrExitedNone()`; the 4 leaf classes
   (`RaschkeStrategyIndicator`/`RaschkeTacticalIndicator`/`VolumeIndicator`/`DailyBiasIndicator`,
   `include/Indicator.h`) gained real `ShouldTrigger()` overrides; `MarketDataReplayEngine.h`'s 4
   dirty-bit-setting sites switched from a plain "any change" comparison to
   `EnteredOrExitedNeutral()`. Note this is a **narrowing** for the offline path (previously more
   permissive than the correct semantic, unlike `STRUCTURE_TEST` which was a widening) —
   `RASCHKE_TACTICAL_TRIGGER`'s 5-writer cascade's own `tacticalDirty` flag (monotonic OR across
   writer stages, so a writer that reverted the value back to its prior state still counted as
   dirty) was replaced with a check against the cascade's final resolved value.
4. **Tests**: 15 new native tests (`tests/cpp/test_entered_or_exited_neutral.cpp`) for the shared
   function across all 4 enum types; full existing `market_data_replay` engine test suite (75+
   tests) re-run clean; full clean `./build_dll.sh` passes.
5. **Not yet done**: a real-data effect-size measurement (same open item as Phase 1's item 3) for
   these 4 keys specifically.

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

## 8. Design synthesis, 2026-09-19 evening (operator discussion) — Trigger 3 reframed, nothing implemented yet

A long design discussion (not yet acted on beyond the `session_quality_score` bug fix already
committed) resolved several open questions from §4f and reframed Trigger 3's actual shape. Recorded
in full so none of it is lost before the next session picks this up.

### 8a. Two separate questions were being conflated under "Trigger 3"

1. **What should the Transformer's training payload be** for each `AsymmetryContext` field — raw
   continuous value, or a discretized/categorical proxy?
2. **When should a fresh observation get captured at all**, and separately, **which fields should
   carry a "this specific field just moved meaningfully" hint bit** on whatever row does get
   captured (for whatever reason)?

These don't need the same answer, and conflating them is what produced the "raw level vs. delta"
confusion earlier in this doc.

### 8b. Resolution for (1): keep the payload continuous, except where the field is already categorical

- `taleb_kurtosis`, `taleb_skewness`, `shannon_entropy`, `roughness_ratio`, `raschke_burst`,
  `taleb_cliff` are genuinely continuous-natured statistics with real fine-grained variation.
  Collapsing them into a handful of enum buckets for the *training payload* throws away exactly the
  kind of signal a high-capacity model can otherwise learn nonlinear thresholds over itself. This
  also matches this repo's own established precedent for its *other* continuous vector (the 18D
  HMM observation vector, `FeatureScaler.h`): scale/winsorize robustly, keep continuous — the
  Student-t HMM's own EM weighting (Peel & McLachlan 2000, already cited in `FeatureScaler.h`)
  explicitly wants continuous magnitude to downweight tails, not a coarse bucket ID.
- `session_quality_score` is the one genuine exception: it isn't a continuous quantity being
  discretized, it's a continuous *representation of an already-categorical thing* (`TimeOfDayEnum`'s
  13 mutually-exclusive states) — there's no "0.5 of Sweet Spot" in between states. It should just
  carry `TimeOfDayEnum`'s own canonical `symmetric_val` directly, which is what the 2026-09-19
  bug fix (§ below) already made it do.

### 8c. Real bug found and fixed, 2026-09-19: `ComputeSessionQualityScore()` didn't match the canonical Python scale

`lbrnet/lbrnet/core/rc_enums.py`'s `TimeOfDayEnum(BaseIntEnum)` already defines a canonical
`symmetric_val` per member ("Elite v2.3: Quality-Based Symmetric Mapping") — the same mechanism
every other Transformer-facing categorical field in this system already uses. This function's first
version (added earlier the same session) invented its own different `[-1,+1]` scale instead of
matching it (e.g. `OPENING_HOUR=0.6` vs. canonical `0.66`, `ASIAN_SESSION=-0.6` vs. canonical `0.0`).
Neither scale is more "scientific" than the other in any empirical/literature sense (both are
hand-picked discretizations of the same Raschke/Taylor qualitative ranking — see 8d) — but having
the C++ producer and the Python training consumer silently disagree about what `SWEET_SPOT` means
numerically is a real correctness bug regardless. **Fixed**: `IndicatorComputations.h`'s
`ComputeSessionQualityScore()` now transcribes the real `rc_enums.py` values directly. Full
before/after and the corrected real-data extraction (`session_quality_score` empirical-percentile
threshold corrected from a stale -0.8 to -0.66): `tools/RECALIBRATION_LEDGER.md`,
`docs/superpowers/plans/2026-09-19-decision-boundary-calibration-tool-implementation.md`'s own
correction note.

### 8d. Provenance of the `TimeOfDayEnum` qualitative ranking itself (for the record)

Both the C++ and Python source comments explicitly attribute the *qualitative* session-quality
ranking (which periods are good/bad, roughly why) to two named sources, split by which part of the
24-hour session each state covers: **Linda Raschke** ("Street Smarts", 1995) grounds the RTH/day
states (Opening Hour, Sweet Spot, Lunch Dead Zone, Afternoon Session, Final Hour, PM Run Entry);
**George Taylor**'s overnight/Globex trading-day methodology grounds the overnight states (Asian
Session, London Window, London-to-Premarket, Pre-Market Hook, After Hours, Overnight Hold). This
attribution is real and well-cited. What is **not** attributable to either source — confirmed by
Gemini's own literature check (`CLAUDE_BRIEF_149`) — is the specific numeric encoding
(`1.0/0.66/0.33/0.0/-0.33/-0.66/-1.0`, an evenly-spaced sevenths-of-the-range ordinal scale) or any
exact tied-ranking within it; that's an engineering discretization layered on top of the qualitative
framework, not a literature-derived quantity.

### 8e. Resolution for (2): separate "capture a new row" from "tag this field as having moved meaningfully"

`lbrnet`'s own "hints" mechanism (§4d) needs a real, non-degenerate per-field "did this change
meaningfully" signal — not "did the raw value change at all" (always true, useless, per §4d's
already-diagnosed finding). The right shape for that signal is a **bucket-transition test**
(reusing the exact `EnteredOrExitedNeutral` idiom already implemented and tested this session for
`RASCHKE_STRATEGY_SETUP`/etc.) over each field's own *existing, already-consequential* regime
boundaries — not a fresh percentile/GPD threshold computed on the raw value's level or delta
distribution (which is what Tasks 4-6 of the calibration-tool plan actually did, and which this
discussion now recognizes was answering a related-but-different question than what the hint
mechanism needs). Concretely: the bucket boundaries become the trigger-classifier's edges; the
*value recorded* in the payload stays the raw continuous magnitude (per 8b) — the two are decoupled,
not the same number reused for two jobs.

**Whether a field should *also* be able to independently cause a new row to be captured** (not just
tag hint bits on rows already captured for some other reason) is reframed as an **empirical
question, not a design assumption** — see 8g.

### 8f. Six candidate fields already have C++-consequential regime boundaries (real, not invented for this discussion)

| Field | Existing boundary(ies) | What crossing it does in C++ today |
|---|---|---|
| `taleb_kurtosis` | 1.3248 / 1.6414 / 1.5650 / 1.3809 / 2.0064 / 1.7592 | Fragility penalty, crisis enter/exit, halt, Amihud fat-tail tightening |
| `raschke_burst` | 1/3, 1/2 | Caution → deny (force passive / reject entry) |
| `shannon_entropy` | 0.45 / 0.60 / 0.80 / 0.90 × Hmax | Pattern-multiplier bands, chaos halt |
| `roughness_ratio` | 3.33 (Kaufman ER-inverted, see 8h) | Not yet wired to any gate, but a real trending/choppy regime distinction |
| `taleb_skewness` | ±0.1544 | Directional-asymmetry force-passive |
| `taleb_cliff` | 0.50 | Proximity-to-invalidation hard gate |

If C++ already treats crossing one of these as consequential enough to change trading behavior,
that's a real, literature-independent argument for the Transformer needing to learn about that exact
moment too — not just whenever some unrelated pattern happens to also fire on the same tick.

### 8g. Proposed empirical test (not yet built): measure isolated-transition rate per candidate field

Generalizes the diagnostic already built for `STRUCTURE_TEST`
(`tools/market_data_replay/structure_test_trigger_impact_eval.cpp`, currently still running
single-threaded against the full 471.9M-tick dataset — confirmed alive via `ps` (`R` state, 99.9%
CPU, 3h14m elapsed at last check), not hung, just slow; a parallelized rebuild using today's
`asymmetry_context_dim_extractor.cpp` row-group-sharding approach would very likely finish in
seconds rather than hours, same lesson learned this session). For each of the 6 fields/boundaries
in 8f, measure across the real tick dataset:
1. How often does the field cross that boundary at all (enter/exit the "notable" region)?
2. Of those crossings, what fraction are **isolated** — no other pattern/indicator trigger fires on
   the same tick/bar? That fraction is the actual, measured case for giving that specific field
   independent triggering power. Near-zero isolated rate = the crossing is already captured
   incidentally by other triggers, no new mechanism needed. High isolated rate = a real, currently
   invisible gap, the same class of finding `STRUCTURE_TEST` turned out to be.

This also directly answers §4f open question #4 (does fixing `STRUCTURE_TEST` reduce Trigger 3's
own necessary scope), generalized from just `STRUCTURE_TEST` to all 6 candidates here.

**Not yet built. Next session's natural starting point once resumed.**

### 8h. Per-field literature-computability status (from `CLAUDE_BRIEF_149`, for the record)

Distinguishing fields with a *real, explicit method* for computing a defensible boundary from those
with none:

- **`taleb_kurtosis`**: already has a real, saved, rerunnable percentile-matching pipeline
  (`analyze_kurtosis_threshold_migration.py`) — the best-grounded of the six already.
- **`taleb_skewness`**: Bowley (1920) gives an explicit, citable band convention
  (`|S_B|<0.10` negligible, `0.10-0.30` mild, `>0.30` moderate-to-strong) — a real, computable rule;
  the existing `0.1544` lands in "mild," consistent-but-not-uniquely-validated by the convention
  (the band doesn't pin one exact number within it).
- **`roughness_ratio`**: Kaufman's Efficiency Ratio convention (`ER>0.30` = trending, "Smarter
  Trading", 1995) gives an explicit, real, literature-sourced number, inverting to
  `roughness_ratio<3.33` — usable now, not just a reference point.
- **`shannon_entropy`**: no universal *fixed* band exists in the literature, but there IS a real,
  citable *method*: identify regime shifts via local maxima/minima or structural breaks in rolling
  entropy (a change-point-detection approach), rather than a static percentile cutoff. This is a
  bigger methodology lift than a constant, not yet attempted.
- **`raschke_burst`** (Goh & Barabási 2008): no universal gating threshold, but a real theoretical
  anchor exists (`B=0` is exactly Poisson-neutral; empirically-observed human-activity processes
  cluster in `B∈(0.5,0.9)`) — useful context, not a ready-made trigger threshold.
  Also: this dim's *own current threshold pair* (1/3, 1/2) is an algebraic carry-forward of an
  undocumented older constant (2.0/3.0 on the pre-bounded scale) — the original pair's own
  provenance is still unknown, a retroactive-queue item (spec §7).
- **`taleb_cliff`** (Chandelier Exit distance): LeBeau's own convention (2.5-3.0× ATR) is for the
  *stop distance itself*, not a secondary "how close to that stop is alarming" early-warning gate —
  no literature precedent found for that secondary layer at all. This field has the weakest
  grounding of the six.
- **General EVT/GPD methodology correction** (Coles 2001): decision thresholds should be read
  directly as a quantile of the fitted GPD at a target risk probability — not a secondary
  arbitrary percentage layered on top of an already-computed return level. Flags this plan's
  `roughness_ratio` EVT-GPD figure (6.2597) for a methodology revisit before treating it as final —
  not yet redone.

### 8i. Immediate next actions for whoever resumes this thread

1. Build the parallelized isolated-transition-rate measurement tool (8g) for the 6 candidate
   fields/boundaries in 8f — the concrete, data-driven way to decide which (if any) need
   independent Trigger-3 power, rather than assuming all or none do.
2. Revisit `roughness_ratio`'s EVT-GPD return level (6.2597) per the Coles (2001) correction in 8h.
3. Decide `shannon_entropy`'s boundary via the structural-break/local-extrema method (8h), not a
   fixed percentile — a materially different, bigger task than the other fields.
4. `raschke_burst`'s and `taleb_cliff`'s original threshold provenance remain open retroactive-queue
   items (spec §7) independent of this discussion.
5. Once isolated-transition rates are known, design the actual bucket-transition trigger
   (`EnteredOrExitedNeutral`-style) for whichever fields warrant it, plus the per-field hint-bit
   export (8e) for `lbrnet`'s consumption regardless of which fields get independent triggering
   power.

