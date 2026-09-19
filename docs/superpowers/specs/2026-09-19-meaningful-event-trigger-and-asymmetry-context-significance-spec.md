# Meaningful Event-Trigger Definition + `AsymmetryContext` Transformer-Significance Gate — Spec

**Status**: design/brainstorm only, nothing implemented. Opened 2026-09-19 from a live-trading
fidelity audit (started as an offline-vs-live `.alpha` trigger comparison, escalated once the same
gap was confirmed to affect **live trading itself**, not just training-data density).

**Severity reframing, read this first**: this is not a training-data-quality nice-to-have. §2 below
confirms the exact same gap that starves `TRAP_*`/`EXIT_*` training labels also means the **live
Transformer can operate on a stale `AsymmetryContext` snapshot during real trading** — it only
receives a fresh one when an unrelated discrete indicator happens to transition on the same tick.

---

## 1. What "meaningful change" currently means, and where it's incomplete

Two live/offline mechanisms currently decide "does this tick warrant capturing a
training/live event" — both are keyed off `IndicatorManager::HasSignificantChange()`
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

## 2. Confirmed: this is a live-trading issue, not just a training-data issue

Checked `docs/ADR/risk_gate_context_wire_spec.md`: `asymmetry_context (AsymmetryContext) →
transformer embedding` — confirmed direct Transformer input, and confirmed **disjoint** from the
HMM's own input (`HMMClient.cpp:416`'s own comment: `asymmetry_context` is omitted from the
`MarketObservation` sent to the HMM — `"8D Context is Transformer-Only (via Event stream)"`).

Traced the live production call chain directly:
- `src/SCStudies.cpp:447` (the main live ACSIL entry point, every tick) calls
  `IndicatorManager::Instance().PublishEventOnChange(sc)`.
- `PublishEventOnChange()` (`IndicatorManager.cpp:984`): `if (!HasSignificantChange()) return false;`
  — then calls `SendEventFlatBuffer(sc, false)`.
- `SendEventFlatBuffer()` builds `auto asym_ctx = ContextManager::Instance().GetAsymmetryContext();`
  and serializes it directly into the live Event sent over port 5555 to Python for **real-time
  Transformer inference** — the actual trading decision path, not a training artifact.

**So `HasSignificantChange()` is a single, shared gate for three consumers**: (a) the live Event
publish to the Transformer (port 5555, real trading), (b) `EventDataCollectorStudy.cpp`'s
`.alpha`/`TrainingEvent` capture (via the same call, behind Locks A/B/D/E), and (c) nothing else —
the offline `market_data_replay` tool has its own separate, non-shared `ConsumePatternDirtyMask()`
analog. **A fix to `HasSignificantChange()` itself automatically fixes both (a) and (b)** — that's
the leverage point. The offline tool needs its own mirrored update afterward to stay in parity.

**Concrete consequence, live trading today**: if `AsymmetryContext` moves meaningfully (a real
kurtosis spike, a real entropy regime shift) but no discrete `IndicatorState` transition happens to
co-occur on that exact tick, the live Transformer keeps operating on a **stale** `AsymmetryContext`
snapshot until the next unrelated trigger fires — an unbounded, unmeasured staleness window.

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

A short-circuit OR at the top of `HasSignificantChange()` is the minimal-surface-area change —
both live (`SendEventFlatBuffer`) and training-collection (`EventDataCollectorStudy.cpp`) call
this one function, so neither call site needs its own new logic. The offline `market_data_replay`
tool's `ConsumePatternDirtyMask() != 0` check needs its own mirrored addition afterward (it does not
share this function at all today).

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

### 4d. Open questions, genuinely unresolved

1. Combined Mahalanobis-style distance over all 8 dims at once (one pass/fail decision, mirrors
   `ObservationData`'s own gate), or per-dim independent thresholds (mirrors the categorical
   trigger's per-key independence, easier to reason about per-dim but loses the "does the *whole*
   asymmetry picture look different" framing)? Not decided.
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
2. Design + calibrate Trigger 3 for `AsymmetryContext` (§4), reusing existing calibration where a
   genuine quantity overlap is confirmed, deriving fresh thresholds elsewhere.
3. Update the offline `market_data_replay` tool's alpha-emission gate to mirror both fixes — it does
   not share `HasSignificantChange()` today and needs its own parallel update.
4. Flag to `lbrnet`'s own coordination log once implemented — this changes the density/freshness
   characteristics of `asymmetry_context`, a value they already train the Transformer on; worth a
   heads-up before they draw conclusions from data collected under the old, gap-having behavior.

## 6. Institutional rollout plan (operator directive, 2026-09-19: decouple by risk, measure before/after)

The two fixes in this spec have different risk profiles and must not ship as one change.

**Phase 1 — `STRUCTURE_TEST` categorical fix (low-risk, well-understood idiom, ship first)**:
1. Decide the exact semantic before writing code: `FAILED_*`-only, or `FAILED_*` + `DECISIVE_*`
   (§ "Where I'd like your call" from the prior discussion — still open).
2. Golden-vector/parity test proving every existing trigger path is unaffected (this is an
   addition, not a refactor, but "should be unaffected" still gets verified, not assumed).
3. New, dedicated test for the actual new behavior.
4. Full rebuild, then measure on a real replay run (`tools/market_data_replay` or a backtest pass
   over real tick history) — quantify the actual event/`.alpha` rate change and whether `TRAP_*`
   label density moves the way the hypothesis predicts. Do not declare this done from compilation
   alone.

**Phase 2 — Trigger 3 (`AsymmetryContext` significance), separate initiative, not rushed**:
1. Measure real per-dim distributions on the existing 471.9M-tick dataset.
2. Reuse `FeatureScaler`'s existing calibration for the 3 confirmed-identical-quantity dims
   (§4c) — cheap, already real-data-validated.
3. Fresh EVT/GPD or percentile-matching derivation for the remaining dims — no invented constants.
4. Calibrate to a target base rate (this repo's ~10% precedent, `CandidateTriggerGate::
   kBaseEpsilon`'s chi-squared derivation), not a guessed threshold.
5. Implement, test, validate against real data — same empirical-close-the-loop discipline as
   Phase 1.

**Cross-cutting, both phases**:
- Offline `market_data_replay` parity is a follow-on after each live fix ships and is validated —
  not parallel work, to avoid re-drifting out of parity a second time before the live behavior
  itself is even settled.
- Flag to `lbrnet`'s coordination log both before (heads-up on the coming distributional shift in
  `asymmetry_context`/event density) and after (what actually changed) — they train on data whose
  statistical character this changes.
- `PRODUCTION_TRIAGE.md`'s `§1`/`§1.1`/`NORTH_STAR_STATUS` gets updated in the same edit that ships
  either phase, per the Triage Protocol — not as a follow-up.

## 7. Relationship to other open specs

- `docs/superpowers/specs/2026-09-16-market-data-replay-alpha-generator-spec.md` — the offline
  generator's own Locks D/E cold-start gap (found earlier the same session: `AllDimsReady()`
  requires ~17 trading days of window-fill before any `.alpha` record can be written at all, vs.
  live's freshness-only Locks D/E) is a **separate, independent** contributor to the same
  `TRAP_*`/`EXIT_*` under-representation symptom that motivated this whole investigation. Both need
  fixing; neither substitutes for the other.
- `docs/superpowers/specs/2026-09-18-predator-sniper-execution-architecture.md` §2 — the Transformer
  input pipeline this spec's Trigger 3 directly feeds; cross-reference once Trigger 3 is designed
  further.
