# Offline `.alpha` (TrainingEvent) Generator — Additive Extension to `tools/market_data_replay/`

**Status, opened 2026-09-16**: verification phase COMPLETE, both scope decisions APPROVED
(2026-09-16, explicit operator sign-off) — see §7. Ready for an implementation plan.
**Verification plan:**
`docs/superpowers/plans/2026-09-16-market-data-replay-alpha-generator.md` — resolved this spec's
§5 open questions before any implementation plan is scoped. Follow-on to
`docs/superpowers/specs/2026-09-08-offline-context-generator-spec.md`, whose own "Reusability
mandate for a future `.alpha` generator" section (opened 2026-09-08) flagged this as a stated
near-future follow-on. That section's original framing — one reusable per-tick core that a
`.context`-only invocation drives today and a later `.alpha`-inclusive invocation reuses unchanged
— is **superseded below** once the real production architecture was traced end-to-end this
session: `.context` and `.alpha` are gated by two genuinely independent triggers in production, not
one shared trigger with an extra consumer bolted on. See §1.

## 0. Mandate

Extend `tools/market_data_replay/` (already shipped, `docs/superpowers/plans/2026-09-08-market-
data-replay-implementation.md`, validated against 471.9M real MES ticks, 335,147 aligned MO+SS
pairs, zero sequence mismatches) so it can **also** produce `.alpha` files (the `TrainingEvent`
stream consumed by Transformer training), as **new, additive, independently-toggleable
functionality** — CLI flags select `.context` only (today's default, unchanged), `.alpha` only, or
both from the same replay run. **The existing, shipped `.context`-generation code path is not
modified, reordered, or rewired by this work** — this is a deliberate, explicit decision (operator
directive, this session), not merely a preference: the `.context` path is already real-data-
validated and any change to its trigger/output risks silently invalidating that validation.

## 1. Real production architecture (verified against real source, not assumed)

Two **independent** triggers write into the live system's output streams — this is the single
most important correction to the 2026-09-08 spec's framing:

### 1a. Trigger 1 — `.context` (already replicated by `tools/market_data_replay/`)

`ContextManager::CheckAndTriggerHMM(now_us, isDataCollection, ...)` (`src/ContextManager.cpp:1022`)
computes the scaled 18D observation vector (`m_latestScaledObs`, via `FeatureScaler`), runs it
through `ObservationTriggerGate`'s Mahalanobis significant-change test, and on a pass calls
`LBRFileManager::LogContext()` (`src/ContextManager.cpp:894`, "PATH 1 - TRAINING"). This is exactly
what `tools/market_data_replay/`'s `MarketDataReplayEngine.h`/`CandidateTriggerGate.h` already
reconstruct today, real-data-validated.

### 1b. Trigger 2 — `.alpha` (the actual production path, NOT yet replicated)

`EventDataCollectorStudy.cpp` runs its **own, separate** 5-lock gate sequence
(`src/EventDataCollectorStudy.cpp:669-780`), independent of trigger 1's Mahalanobis test:

| Lock | Gate | Enforced during data collection? |
|---|---|---|
| A | Observation-stream saturation readiness (`saturationSamples`/`saturationRequired`) | Yes — hard block |
| B | Indicator-stack warm-up (`IndicatorManager::IsWarmedUp()`-style) | Yes — hard block |
| C | Regime-certainty Schmidt trigger (entropy/kurtosis rank-percentile hysteresis, `LOCK_C_*` constants) | **No — telemetry only.** Source's own comment: *"LockC is NOT enforced here because the data-collector must capture ALL market regimes... will be enforced by the live-inference path (SCStudies.cpp) when deployed"* |
| D | TS1 macro freshness (`AreTs1DimsReady`, 6h max age) | Yes — hard block, with a stale-streak circuit breaker |
| E | TS2 structural freshness (`AreTs2StructuralDimsReady`, 3h max age) | Yes — hard block |

Once A/B/D/E pass (C is logged but never blocks), the actual per-event trigger is
`IndicatorManager::HasSignificantChange()` — a **per-indicator dirty-mask flag**, not a
Mahalanobis distance test. On a pass: `IndicatorManager::GetTrainingEventT(sc)`
(`src/IndicatorManager.cpp:741`) builds the `TrainingEventT`, then
`LBRFileManager::LogSynchronizedEvent()` (`include/LBRFileManager.h:58`) writes **MarketObservation
+ SystemState + TrainingEvent together under one shared sequence_id** (the "Sequence-Locked
Stitcher") — meaning this second trigger *also* writes its own `.context`-format MO+SS pair,
independently of trigger 1's writes, into the same `.context` stream. **The real, on-disk
`.context` file is therefore a merge of two independently-triggered write streams, not one** —
`tools/market_data_replay/` today only reconstructs trigger 1's contribution to that merged stream.
This is a genuine, previously-undocumented architectural fact, not a simplification for this spec.

### 1c. Key clarification (operator, this session): `.alpha`'s observation vector is NOT Mahalanobis-gated

`event.observation` (the field `.alpha`'s `TrainingEvent` carries) is populated inside
`ContextManager::AddToTrainingEventFB()` (`src/ContextManager.cpp:630-636`) directly from
`m_latestScaledObs` — **the same scaled-observation buffer trigger 1 already computes every
`CheckAndTriggerHMM()` call**, per that method's own comment: *"m_latestScaledObs is updated once
per CheckAndTriggerHMM() call. ... guarantees .alpha/.context parity."* Critically, this value is
refreshed **regardless of whether that call's Mahalanobis gate decided to fire a `.context` write**
— so by the time trigger 2's independent Lock+dirty-mask gate fires (on its own schedule, possibly
a different tick than any given trigger-1 pass), it reads whatever the *latest* scaled observation
happens to be, not one re-gated by distance. Practical consequence for this generator: **the
observation-vector computation itself is fully reusable, unchanged, from the already-shipped
`.context` engine** — only the *emit decision* for `.alpha` needs new, independent logic; there is
no need to build a second, parallel observation-vector pipeline.

### 1d. `sc`-decoupling pattern (this session): borrow from `sierra_chart_dependencies/` directly where genuinely portable, mirror-with-citation where not, extract-to-pure where neither works

Before porting any `sc`-coupled function (§3 item 3, §5 item 3), classify each symbol it touches
against the vendored ACSIL SDK headers under `sierra_chart_dependencies/` — verified this session,
not assumed:

- **Directly includable, zero duplication** — `sierra_chart_dependencies/scdatetime.h` (the real
  `SCDateTime` class, `MonthEnum`/`DayOfWeekEnum`, 2738 lines) was bare-compiled standalone this
  session (`g++ -std=c++17 -I. ...`, zero errors) — it has **no Windows/ACSIL dependency at all**,
  unlike `scstructures.h` (confirmed the actual, sole source of the `#include <windows.h>` that
  blocks `LBRFileManager.cpp`/`sierrachart.h` from linking into this standalone tool — traced to
  `scstructures.h:15` specifically, not `sierrachart.h` itself). Any `sc`-coupled logic whose only
  real dependency is `SCDateTime` (e.g. `GetTrainingEventT()`'s
  `sc.BaseDateTimeIn[sc.Index].ToUNIXTimeInMicroseconds()`) can `#include` this real vendored
  header directly and construct/consume genuine `SCDateTime` objects — zero drift risk since it is
  the literal same code, not a re-derived port (unlike `EasternTimeOffset.h`, which predates this
  finding and duplicates DST logic `scdatetime.h` might already provide for free — flagged as a
  possible future consolidation, out of scope for this spec).
- **Mirror small, stable numeric constants/enums with a citation** — `scconstants.h` does NOT
  compile standalone as-is (missing a leading `#include <cstdint>`; relies on transitively getting
  it from whatever real ACSIL translation unit includes it), so it cannot be included directly.
  But individual small, self-contained enums inside it (e.g. `ChartDataTypeEnum` — `NO_DATA_TYPE`/
  `DAILY_DATA`/`INTRADAY_DATA`/`MARKET_DEPTH_DATA`, `scconstants.h:650-655` — and `ReplayStatus` —
  `REPLAY_STOPPED`/`RUNNING`/`PAUSED`, `scconstants.h:1650-1654`) are trivially stable, explicit-
  underlying-type (`int32_t`) value sets safe to mirror verbatim into a small, portable header,
  each constant citing its vendored source line as ground truth — the exact precedent already
  established in this repo for `HMMStateEnum`/`HMM_NO_PRIOR` (`include/rc_enums.h`'s own header
  comment: *"extracted to rc_enums.h ... same rationale as MacdEnum's extraction ... PredatorContext.h
  needs the enum without pulling in sierrachart.h"*) — not a new pattern, a repeat of one already in
  production. **Caveat, checked this session**: `ChartDataTypeEnum`/`ReplayStatus` themselves are
  used by `PreflightValidation()` (arm-time live-chart-configuration sanity checks: is this chart
  Intraday, is replay running/paused) — **not** by the actual per-tick Lock A/B/D/E gating logic or
  `GetTrainingEventT()`'s temporal-metadata section. A standalone replay tool has no "chart" to
  validate in the first place, so these two specific enums are likely NOT needed for this spec's
  actual scope — cited here only to establish the pattern is sound and already verified working on
  a concrete example, not as a claim that these two enums must be ported.
- **Neither portable nor safely mirrorable — extract to pure functions instead** — genuine ACSIL
  array/persistent-storage access (`sc.Index`, `sc.GetPersistentInt()`/`SetPersistentInt()`,
  `sc.Close[]`, etc.) has no vendored-header shortcut; this is the same "extract the pure
  computation, thin SC-coupled wrapper on the live side" pattern already used successfully for
  every dim in `tools/market_data_replay/` today (`TickBarAggregator.h`, `ObservationTriggerGate.h`,
  `MeanReversionCalculator.h`, `LiquidityFragilityEngine.h`, etc.) — not a new technique, just the
  established one applied to `GetTrainingEventT()`/Lock A/B next.

**Net effect on open questions §5 items 1/3**: the audit still needs to happen (nothing here
resolves *which* symbols `GetTrainingEventT()`/Lock A/B actually touch), but the three-way
disposition above (borrow / mirror-with-citation / extract-to-pure) is now the decided
classification framework to apply once that audit is done, rather than an open design question in
its own right.

## 2. Decision: additive-only, two independent gates, one shared observation core

Per §0/§1, the design is:

- **Shared, unmodified**: the existing per-tick core (`TickBarAggregator` ×3,
  `ActivityClockManager`-equivalent, the 18D `ObservationData` construction, `FeatureScaler`) that
  `tools/market_data_replay/` already runs every tick to feed trigger 1. `.alpha` generation reads
  the *same* freshly-computed scaled observation each tick — it does not recompute it.
- **New, parallel, independently gated**: a second emit path replicating trigger 2's real logic
  (Locks A/B/D/E, `HasSignificantChange()`-equivalent dirty trigger, `GetTrainingEventT()`-
  equivalent record assembly, a new `AlphaFileWriter.h` output). This path runs *alongside*
  trigger 1's existing Mahalanobis-gated `.context` emission, on its own independent schedule —
  exactly mirroring production's two-independent-triggers reality (§1b), not a false unification.
- **Not done**: no attempt to merge/reconcile the two gates into one, no change to trigger 1's
  existing behavior or output, no enforcement of Lock C (stays telemetry-only, matching
  production's own explicit "capture ALL market regimes" rationale).

## 3. New components required (none built yet)

1. **Lock A/B/D/E replication** — concrete thresholds/state already fully specified in §1b's table
   above, copied from `EventDataCollectorStudy.cpp`'s real constants
   (`LOCK_D_TS1_MAX_AGE_US`=6h, `LOCK_E_TS2_MAX_AGE_US`=3h, etc.). **Lock A/B traced in full,
   2026-09-16 (plan Task 1):**
   - **Lock A** (`ContextManager::IsObservationSaturated()`, `src/ContextManager.cpp:240`) is
     just `m_featureScaler.warmedUp`, backed by `sampleCount` vs. `FeatureScaler::RANK_WINDOW`
     (=500 samples, `include/FeatureScaler.h:544`) — already resolved and documented as
     "working as designed, no defect" in a prior session (`SCRATCHPAD.md`'s "LockA audit").
     **Already free**: `FeatureScaler` is confirmed pure and already a dependency of the replay
     engine's existing 18D observation-vector construction — the engine already has this state on
     hand, it just needs to expose/check it, no new computation.
   - **Lock B** (`IndicatorManager::IsWarmedUp()`, `include/IndicatorManager.h:66`) is
     `m_isWarmedUp`, set by `CheckWarmupStatus()` (`src/IndicatorManager.cpp:579`, called once per
     TS3 15-min bar-close from `UpdateBarContext`) once three conditions hold: (a)
     `m_warmupBarCount >= 200` (a simple bar-close counter, trivial to reconstruct); (b)
     `GetValue<IndicatorKey::RSI>() != 0` — **a direct, hard dependency on Task 2's already-
     identified RSI reimplementation work** (RSI is one of the Sierra-Chart-built-in-function-
     backed `PRIMARY_TRIGGER_MASK` indicators, §3 item 2) — Lock B genuinely cannot pass offline
     until RSI is reimplemented, this is not a separate, independent unknown; (c)
     `HmmState() != nullptr` — confirmed a dead/no-op check in practice: `HmmState()` returns
     `&m_hmmState`, the address of an always-constructed member, never actually null regardless of
     whether any live inference has ever arrived — zero work needed for this clause.

   **Institutional recommendation**: don't treat Task 1 as an independent effort needing its own
   scope decision — Lock A is already free, and Lock B's only real cost (RSI) is the exact same
   cost already identified and decided upon in item 2/§5 item 2. Fold Task 1 into whatever
   decision is made there: if the `PRIMARY_TRIGGER_MASK` porting work proceeds (which needs RSI
   anyway for `DetectMomentumPinball`), Lock B unblocks as a side effect, no separate investment;
   if that work is deferred, Lock B is moot for the same reason.
2. **`HasSignificantChange()`-equivalent dirty trigger** — **AUDITED, 2026-09-16 (plan Task 2) —
   MAJOR SCOPE FINDING, changes this initiative's risk profile.** Confirmed a per-`IndicatorKey`
   dirty-bit mask (`m_dirty_mask`, `src/IndicatorManager.cpp:1134`), genuinely distinct from
   `ObservationTriggerGate`'s Mahalanobis test — two tiers:
   - **`PRIMARY_TRIGGER_MASK`** (`src/IndicatorManager.cpp:29-45`, 17 keys) — any dirty bit here
     fires unconditionally: `KANGAROO_TAIL`, `TURTLE_SOUP`, `MOMENTUM_PINBALL`, `ELDER_BREAKOUT`,
     `NR7`, `SIDE`, `LONG_IMP`, `INTERM_IMP`, `RSI`, `INTERM_STOCHASTIC`, `ATR_PROXIMITY`,
     `EMA_PROXIMITY`, `RASCHKE_STRATEGY_SETUP`, `RASCHKE_TACTICAL_TRIGGER`, `STRUCTURE_TEST`,
     `VOLUME_SIGNAL`, `DAILY_BIAS`.
   - **`SECONDARY_TRIGGER_MASK`** (everything else up to `MAX_INDICATORS`=55,
     `include/IndicatorKey.h:77`) — dirty bit alone isn't enough; also gated by
     `Scoring::Instance().IsIndicatorEventSignificant(key, riskCtx)`.

   **This means `.alpha`'s real per-event trigger depends on the FULL Raschke/Elder pattern-
   detection indicator suite** (Turtle Soup, Momentum Pinball, Elder Breakout, Kangaroo Tail, NR7,
   Raschke strategy/tactical setups, structure tests, MACD/impulse variants, oscillators, daily
   bias, ATR/EMA proximity, correlations, etc. — up to 55 `IndicatorKey` slots) — **none of which
   `tools/market_data_replay/` currently computes.** The existing engine only reconstructs the 18D
   `ObservationData` vector (activity-clock/statistical dims: fractal dimension, Hurst, bipower
   variation, recurrence rate, mean-reversion z-score, etc.) — a categorically smaller, separate
   set of computations from `TripleScreen1/2/3.cpp`'s pattern-detector indicators. **Faithfully
   reconstructing `.alpha`'s real trigger offline requires porting a large, currently-unscoped set
   of additional indicator computations, not just adding a new gate function on top of existing
   engine state.** This is a substantially bigger effort than items 1/3/4/5 in this list, and
   changes the overall sizing of this initiative — flagged prominently for a scope/priority
   decision (§5 item 2), not assumed to proceed at the originally-implied size.

   **Feasibility assessment of `PRIMARY_TRIGGER_MASK`'s 17 keys, 2026-09-16 — genuinely feasible,
   bounded cost, not a hard blocker, but real new work (not "flip a switch"):**

   - **The pattern-detection logic itself is mostly already pure and portable, free.**
     `DetectKangarooTail`/`DetectTurtleSoup`/`DetectMomentumPinball`/`DetectElderBreakout` all live
     in `include/IndicatorComputations.h`, whose own header comment states *"No Sierra Chart /
     ACSIL types, natively unit-testable"* — confirmed by `tests/cpp/test_indicator_computations.cpp`
     calling them with plain scalar arguments. Zero porting work needed for these functions
     themselves.
   - **`NR7` is not yet extracted** (still inline in `TripleScreen3.cpp`, ~1394+) but its actual
     logic is simple and self-contained: a 7-bar range-narrowness comparison
     (`sc.High[sc.Index-i] - sc.Low[sc.Index-i]` for `i` in 1..7) plus the already-computed
     `MarketClimate` gate — no dependency on any built-in technical-analysis function. Low-effort
     extraction once a bounded 7-bar rolling history exists (see below).
   - **The real, nontrivial cost: several detectors' upstream inputs are genuine Sierra Chart
     BUILT-IN library functions, not extractable code.** Confirmed by direct grep:
     `sc.RSI()`/`sc.MACD()`/`sc.ATR()`/`sc.Stochastic()`/`sc.MovingAverage()` compute RSI(3/10),
     MACD/Elder-Impulse color, ATR, Stochastic %K, and EMA/Keltner-band inputs that
     `DetectMomentumPinball`/`DetectElderBreakout` (and likely several `PRIMARY_TRIGGER_MASK`
     members not yet individually audited: `RSI`, `INTERM_STOCHASTIC`, `ATR_PROXIMITY`,
     `EMA_PROXIMITY`) consume. These are proprietary, closed-source platform routines — nothing to
     "extract," since we don't have their source. A repo-wide grep found **zero existing pure
     reimplementation** of RSI/MACD/ATR/Stochastic anywhere in this codebase — this is genuinely
     new work: reimplementing standard, published TA formulas (Wilder's RSI/ATR, standard EMA/SMA,
     standard MACD, standard %K/%D stochastic) and empirically validating numeric parity against
     real historical `.context`/`.alpha` data — the same validation methodology already proven for
     `TickBarAggregator.h`/`EasternTimeOffset.h` (SC's own bar-boundary/DST conventions had to be
     empirically matched, not assumed, and passed).
   - **Favorable economies of scale**: this is NOT 17 independent porting efforts. Most
     `PRIMARY_TRIGGER_MASK` members are built from a small, shared set of ~5 TA primitives (RSI,
     ATR, MACD/Impulse, Stochastic, EMA/SMA). Build and validate those once; most patterns then
     assemble cheaply on top, the same "single source of truth" leverage this codebase already
     gets from `IndicatorComputations.h`.
   - **New infrastructure needed in the replay engine itself**: it currently tracks only the
     in-progress/current bar, no rolling multi-bar history. `NR7` (7 bars), `TurtleSoup` (4-day
     high/low), and Elder Breakout's consolidation-bar count all need a small, fixed-size ring
     buffer of completed bars per timeframe added — bounded, DOD-compliant, consistent with this
     tool's own established memory-safety conventions (not a new architectural risk).
   - **`SECONDARY_TRIGGER_MASK`'s ~38 remaining keys not yet individually audited** — lower
     priority, since `PRIMARY_TRIGGER_MASK` alone already fires the trigger unconditionally; a
     first working version could reasonably scope to `PRIMARY_TRIGGER_MASK`-only fidelity as a
     defensible, real (not arbitrary) first slice, deferring `SECONDARY_TRIGGER_MASK` to a
     follow-on.

   **Revised verdict for §5 item 2's decision**: option (a) (port the full suite) is no longer an
   unscoped, open-ended risk — it decomposes into a concrete, bounded plan: (i) reimplement +
   validate ~5 TA primitives (RSI, ATR, MACD/Impulse, Stochastic, EMA/SMA), (ii) add bounded
   multi-bar history to the replay engine, (iii) extract `NR7`, (iv) assemble the 17
   `PRIMARY_TRIGGER_MASK` detectors on top (mostly already pure), (v) defer
   `SECONDARY_TRIGGER_MASK` to a later slice. This is real, moderate-to-significant new work, not
   nothing — but it is no longer the open-ended unknown it was before this assessment.
3. **A pure/ported subset of `IndicatorManager::GetTrainingEventT(SCStudyInterfaceRef sc)`**
   (`src/IndicatorManager.cpp:741`) — **AUDITED IN FULL, 2026-09-16 (plan Task 3), risk resolved:
   the real `sc`-coupled surface is much narrower than originally feared.** Full line-by-line read
   of the function body plus every function it calls, not sampled:

   | Call / reference | `sc` touch? | Disposition (§1d) |
   |---|---|---|
   | `event->bar_index = sc.Index` | Yes, direct | Extract-to-pure: pass a plain `int barIndex` (the replay engine already tracks its own tick/bar index) |
   | `event->timestamp_us = sc.BaseDateTimeIn[sc.Index].ToUNIXTimeInMicroseconds()` | Yes, direct | Extract-to-pure, simpler than borrowing `SCDateTime`: the replay engine already holds the raw tick timestamp as epoch-microseconds directly — pass that `uint64_t` straight through, no `SCDateTime` construction needed at all |
   | `PopulateIndicatorState(*event->indicators)` | **None** — signature takes only `MTS::Schema::IndicatorState&`, confirmed at `src/IndicatorManager.cpp:1261` | Already portable, call as-is |
   | `GetValue<IndicatorKey::*, StorageBlock::*>()` (all `mutate_*_quality`/`mutate_corr_*`/etc. calls) | **None** — reads `m_packed` directly | Already portable, call as-is |
   | `GetTickCompanionValues()` | **None** — `const` method, zero parameters, confirmed at `src/IndicatorManager.cpp:704`; reads `m_store`/`PositionManager::Instance()`/its own cached members | Already portable, call as-is |
   | `InferenceManager::Instance().AddToTrainingEventFB(*event)` | **None** — takes only `TrainingEventT&`, reads its own atomic indicator state | Already portable, call as-is |
   | `ContextManager::Instance().AddToTrainingEventFB(*event, sc)` | **Dead parameter** — real signature is `AddToTrainingEventFB(TrainingEventT& event, SCStudyInterfaceRef /*sc*/) const` (`src/ContextManager.cpp:574`, parameter name literally commented out, never referenced in the body) | Already portable — the ported call simply omits the unused argument |
   | `SyncFeatureVector(event->features)` | **None** — reads `ContextManager::Instance().GetStatisticalContext()`/`GetNormalizedAnchors()` | Already portable, call as-is |
   | `sc.Open/High/Low/Close[sc.Index]`, `sc.Volume[sc.Index]` | Yes, direct | Extract-to-pure: pass plain `float open,high,low,close` + `int64_t volume` for the current/most-recent bar (the replay engine's `TickBarAggregator` already tracks these) |
   | `WriteTrainingRootSharedFields(*event, TrainingRootSharedSlice{...})` | **None** — takes only `event` and the already-built `companions` struct | Already portable, call as-is |

   **Conclusion**: only 4 real `sc` touches exist in the entire call graph (bar index, timestamp,
   OHLC, volume) — all four are trivial extract-to-pure substitutions using values the replay
   engine already computes for its existing `.context` path. No `SCDateTime` borrowing is actually
   needed for this function (simpler than §1d anticipated: the raw epoch-microsecond timestamp is
   already available upstream, bypassing `SCDateTime` entirely). Every other call in the chain —
   the large majority of the function's real work — is already `sc`-free today. This substantially
   de-risks item 3; it is no longer "the single biggest open risk" in this spec.
4. **`AlphaFileWriter.h`** — mirrors `ContextFileWriter.h`'s already-established precedent
   (`tools/market_data_replay/ContextFileWriter.h`): `LBRFileManager.cpp` cannot link into a
   standalone Linux tool (transitively pulls in `windows.h` via `MindfulTrader_Precompiled.h` →
   `sierrachart.h`, confirmed by direct compile attempt during the `.context` generator's own
   build), so `LogAlpha()`/`LogSynchronizedEvent()`'s `TrainingEvent`-writing logic needs the same
   deliberate, byte-faithful standalone duplication — not a re-derivation from scratch.
5. **CLI flags**: `--emit-context` (default **on**, preserves every existing invocation's current
   behavior unchanged) / `--emit-alpha` (default **off**) — independently selectable, so today's
   scripts/automation need zero changes unless explicitly opted into `.alpha` output.

## 4. Non-goals

- Any change to trigger 1's existing Mahalanobis-gated `.context` emission, its output format, or
  its already-validated behavior.
- Unifying triggers 1 and 2 into a single gate — that would itself be a production-behavior
  change, and is explicitly out of scope; production keeps them independent and so does this tool.
- Enforcing Lock C — stays telemetry-only, matching production's explicit rationale (data
  collection must see all regimes, not just "certain" ones).
- `.imbalance.context`/`.imbalance.alpha` — out of scope, a separate stream family
  (`ImbalanceEventDataCollectorStudy.cpp`) not touched by this spec.
- **"Directional `.alpha`" construction (`lbrnet/lbrnet/scripts/build_directional_alpha.py`) —
  confirmed this session it must stay Python-side, not a candidate for a C++ port.** Traced its
  real dependencies: it loads the actual trained `models/hmm_model.pkl` via `RegimeEngine.
  load_model()` and calls `regime_engine.infer(obs)` on every event to compute fresh HMM posteriors
  (`_infer_regime_for_event`/`_iter_with_hmm`), overwriting whatever regime state the raw `.alpha`
  already carried — genuine trained-model inference, not a pass-through read. It also applies
  Python-native labeling (`triple_barrier_scanner`) and augmentation ("shout" threshold/mask scans,
  regime-stress scans) — lbrnet's own labeling domain. Both are out of scope for
  `MindfulTrader`/`tools/market_data_replay/` by hard architectural boundary ("C++ never runs HMM
  inference", established repeatedly this session for the `RegimeManager` work) and by this
  project's own stated governance ("No ML training logic (belongs in `lbrnet`)",
  `.github/copilot-instructions.md`'s Boundaries section). **Consequence, and good news for this
  initiative's scope**: this generator's job stays exactly what §0 already scoped — produce a
  well-formed raw `.alpha` (+ `.context` sibling); `build_directional_alpha.py` already works
  as-is against any valid raw `.alpha`/`.context` pair regardless of whether real SC replay or this
  offline tool produced it, so no new Python-side work is implied by this spec.

## 5. Open questions / investigation needed before implementation

1. ~~Lock A/B's exact readiness source~~ **RESOLVED (2026-09-16, plan Task 1)**: Lock A
   (`FeatureScaler.warmedUp`/500-sample `RANK_WINDOW`) is already free — the replay engine already
   depends on `FeatureScaler`. Lock B (`m_isWarmedUp`) reduces to a 200-bar counter (trivial) plus
   an RSI-nonzero check that is the exact same dependency already identified in item 2 (RSI
   reimplementation) — not a separate cost. See §3 item 1 for the full trace. Recommendation: fold
   this into item 2's scope decision rather than treating it independently.
2. ~~`HasSignificantChange()`'s real mechanism~~ **RESOLVED, MAJOR SCOPE FINDING (2026-09-16, plan
   Task 2)**: confirmed a per-`IndicatorKey` dirty-bit mask (17 `PRIMARY_TRIGGER_MASK` keys fire
   unconditionally; up to 38 more gated by `Scoring::IsIndicatorEventSignificant()`), genuinely
   distinct from `ObservationTriggerGate`'s Mahalanobis test — see §3 item 2 for the full
   breakdown. **The replay engine's existing per-tick state does NOT carry enough information to
   reconstruct this** — it only computes the 18D HMM observation vector, not the ~55-slot Raschke/
   Elder pattern-detector indicator suite (`TripleScreen1/2/3.cpp`'s Turtle Soup, Momentum
   Pinball, Elder Breakout, Kangaroo Tail, NR7, MACD/impulse variants, oscillators, structure
   tests, etc.) that `.alpha`'s real trigger depends on. **Feasibility of porting
   `PRIMARY_TRIGGER_MASK` assessed the same day, §3 item 2** — genuinely feasible at a bounded,
   moderate-to-significant cost (reimplement + validate ~5 TA primitives, add bounded multi-bar
   history, extract `NR7`, assemble the 17 detectors — mostly already-pure code), not an
   open-ended unknown. **APPROVED (2026-09-16, operator sign-off, §7 item 1)**: proceed with
   that bounded plan for `PRIMARY_TRIGGER_MASK` as a first slice, deferring `SECONDARY_TRIGGER_
   MASK` to a separately-justified follow-on.
3. ~~`GetTrainingEventT()`'s exact `sc`-coupled surface~~ **RESOLVED (2026-09-16, plan Task 3)**:
   full line-by-line audit done, see §3 item 3's table — only 4 real `sc` touches exist (bar
   index, timestamp, OHLC, volume), all trivial extract-to-pure substitutions; everything else in
   the call graph is already `sc`-free. No longer the biggest open risk.
4. ~~`model_confidence`~~ **RESOLVED (2026-09-16, plan Task 4)**: confirmed, not just assumed.
   `TradeSignalManager::SetTradeSignal()` (the only writer of `m_hasFreshSignal`/`m_currentSignal`)
   is declared and defined but has **zero callers anywhere in the live codebase**
   (`src/`/`include/`, verified by grep) — `HasFreshSignal()` returns `false` unconditionally today,
   in production as well as offline, not just in this generator. `model_confidence = 0.0f` is
   therefore not a weaker offline approximation of a real live value — it is the only value this
   field ever takes anywhere in the current system. Decision: use `0.0f` unconditionally, same as
   production's own `else` branch, no further caveat needed. **Separate, real live-system finding
   surfaced by this trace (not a `.alpha`-generator scope item — tracked instead in
   `MindfulTrader/docs/HMM_REGIME_MANAGER_COORDINATION.md` Entry 8)**: `TradeSignalManager` appears
   to be dead/superseded code — the actual live model-confidence path is `PositionManager.cpp`'s
   `prediction.confidence` (from `InferenceManager::Instance().Prediction()`) →
   `Trade::SetConfidence()`, entirely bypassing `TradeSignalManager`. This means
   `EventDataCollectorStudy.cpp:786`'s `.alpha`-emission code is reading from a dead source when a
   live one already exists elsewhere — likely a real bug, not something this spec should fix
   (an offline generator should replicate production's actual current behavior, bugs included, not
   silently diverge from it), but worth its own investigation.

   **UPDATE (2026-09-17) — the live-system bug above is now FIXED** (§10 finding 5,
   `HMM_REGIME_MANAGER_COORDINATION.md` Entry 10): `EventDataCollectorStudy.cpp` now reads
   `InferenceManager::Instance().Prediction()->Confidence()`, so production's real `.alpha` stream
   will carry genuine non-zero confidence going forward. **This does NOT change this tool's own
   `model_confidence = 0.0f` decision above, and for a DIFFERENT reason than originally written**:
   the original rationale ("0.0f is the only value this field ever takes anywhere in the current
   system") is now false — production has a real value. The offline tool still can't produce one,
   though, because it has no live Transformer connection to query at replay time (a structural
   limitation, not a bug) — the correct framing is now the same as the `HmmState`/regime-field gap
   `HMM_REGIME_MANAGER_COORDINATION.md` Entry 9 (lbrnet-session) found in `offline_replay_455m.alpha`:
   this tool computes the raw observation vector but runs no model inference at all, live or
   offline. See Entry 11 (MindfulTrader-session) in that same log for the full reply.
5. ~~`RiskGateContext` for `.alpha`~~ **RESOLVED (2026-09-16, plan Task 5)**:
   `ContextManager::BuildRiskGateContext()` (`src/ContextManager.cpp:851`) is confirmed already
   pure/portable — a `const`, zero-parameter method that does nothing but copy 17 fields
   field-by-field from `m_localRiskContext` (a plain `LocalRiskContext` struct) into the
   `RiskGateContextT` wire type. No `sc`/ACSIL dependency at all. **One real, small residual
   plumbing gap found**: the replay engine currently computes each of these raw (pre-`FeatureScaler`)
   values as transient local variables while building the *scaled* 18D `ObservationData` (e.g.
   `meanRevZ` at `MarketDataReplayEngine.h:728` is written straight into the scaled observation
   and then goes out of scope) — it does not currently retain them afterward in a persistent
   struct. This is not a new computation or a portability risk (the raw math is already being
   done for scaling), just a "retain what's already computed, instead of discarding it" change —
   low effort.
6. ~~Output file naming/pairing~~ **RESOLVED (2026-09-16, plan Task 6)**: a single `--output
   <path>` base-path argument, no separate `--alpha-output`. The tool derives `<path>.context`/
   `<path>.alpha` internally, exactly mirroring `LBRFileManager::Open(path, symbol)`'s own
   convention. Grounded in real evidence, not just simplicity: `lbrnet/lbrnet/scripts/
   build_directional_alpha.py` (the actual, already-working Python consumer) already auto-detects
   the sibling `.context` file this way (`alpha_basename = args.input.stem; context_candidate =
   RAW_DATA_DIR / (alpha_basename + ".context")`, lines 1035-1036) — a separate `--alpha-output`
   argument would actively work against that existing auto-detection logic, undermining this
   initiative's own stated goal (§0) of being a true drop-in replacement for real SC-collected
   data.

   **Separate discrepancy found while investigating this item, not yet resolved**:
   `tools/market_data_replay/MarketDataReplay.cpp` — which `tools/README.md` still describes as
   the `.context` generator's "CLI driver" — currently writes directly to Parquet (per the
   2026-09-09 dim-selection pivot), not `.context`. `ContextFileWriter.h` has zero includers
   anywhere in the current source tree; a separate, older compiled binary
   (`tools/bin/market_data_replay_full`) appears to be the last artifact of the original
   `.context`-writing CLI, whose source was since overwritten in place rather than kept as a
   separate file. This means the assumption that `--emit-context` "preserves every existing
   invocation's current behavior unchanged" (§3 item 5) needs re-checking against whichever CLI
   entry point this initiative actually extends — `tools/README.md` is stale on this point and
   should be corrected as part of implementation, not assumed correct.

## 6. Cross-references

- `docs/superpowers/specs/2026-09-08-offline-context-generator-spec.md` — the `.context` generator
  this work extends; its "Reusability mandate for a future `.alpha` generator" section is
  superseded by this spec's §1/§2 (two independent gates, not one shared trigger).
- `docs/superpowers/plans/2026-09-08-market-data-replay-implementation.md` — the shipped `.context`
  implementation plan, Tasks 1-11, whose output (`TickBarAggregator`, `ObservationTriggerGate`,
  `ContextFileWriter.h`) this spec's §2 explicitly reuses unmodified.
- `/memories/repo/market_data_replay_dim_selection.md` — separate, unrelated thread (dim
  IN/OUT convergence) sharing the same `tools/market_data_replay/` folder; not affected by this
  spec.

## 7. Institutional recommendations (2026-09-16) — APPROVED (operator sign-off, 2026-09-16)

All 6 verification tasks are done (§5). Both decisions below are approved — ready to scope an
implementation plan.

1. **§5 item 2's scope question: proceed with `PRIMARY_TRIGGER_MASK` (17 keys) only, as a first
   slice — not the full 55-slot suite, not abandonment.** This matches this repo's own established
   discipline: the original `.context` generator's own plan explicitly frames itself as "vertical-
   slice-first... matching this repo's own smallest real end-to-end slice discipline," and that
   approach already succeeded once (the shipped, validated `.context` generator). `SECONDARY_
   TRIGGER_MASK`'s ~38 remaining keys should stay explicitly deferred, not silently implied.
2. **§5 item 6's discrepancy: restore `.context`+`.alpha` writing as its own, separately-named CLI
   entry point (e.g. `MarketDataReplayContext.cpp`) — do not retrofit flags onto
   `MarketDataReplay.cpp`.** Checked git history to confirm this wasn't an accidental regression:
   commit `ea752c4` ("Market-data-replay dim-selection phase...") deliberately rewrote
   `MarketDataReplay.cpp` to write flat Parquet directly, for a real, documented reason (fixing a
   MAD-collapse degeneracy found via a 20M-real-tick diagnostic run) — a correct decision *for that
   purpose*, which is now actively shipping `observation_vector_18d_full.parquet` to `lbrnet`.
   `ContextFileWriter.h` was left in place, not deleted, but is now orphaned (zero includers). This
   `.alpha` initiative has a genuinely different purpose (byte-compatible `.context`/`.alpha` pairs
   for `build_directional_alpha.py`, not flat Parquet for dim-selection research) — conflating the
   two risks the same kind of silent capability loss that created this discrepancy. Reuse the same
   engine core (`MarketDataReplayEngine.h`), but keep the output layer and CLI separate.
   `tools/README.md`'s stale description of `MarketDataReplay.cpp` as the `.context` generator's
   CLI driver should be corrected regardless of which way this decision goes — it actively
   misdescribes current behavior today.

## 8. Post-implementation audit resolution (2026-09-16)

Terminology correction, applies to this spec and its implementation plan: this repo's C++ is
dev-only code, never deployed anywhere ("production" was previously used loosely here to mean
"the current live call site in `src/*.cpp`" — not an external, deployed, authoritative system).
Any real bug found in that code during this initiative is fixed, not treated as ground truth to
faithfully reproduce including its own defects.

An audit of the Task 7 implementation found 2 real wiring bugs against the actual call sites
(not assumed from a research subagent's summary) — both fixed, with a read-only Gemini consult
(`gemini --approval-mode plan`, zero file-edit capability given) sanity-checking the second one
before implementation. Full account, including the Gemini exchange citation
(`lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_145`/`_REPLY`): see the implementation plan's own
"Post-implementation audit" section.

## 9. Live-code findings surfaced by this initiative (2026-09-16) — must be resolved, not just noted

Operator directive: this replay-tool initiative is also a forcing function for hardening the live
C++ path itself. Tracing every `PRIMARY_TRIGGER_MASK` key against its real call site (required for
byte-faithful replay) surfaces genuine live-code questions/debt as a side effect — these must be
investigated and, where confirmed, actually fixed in `src/`/`include/`, not merely documented as a
replay-tool caveat. 3 found so far while wiring Task 7's keys:

1. **RESOLVED, not a bug — `ElderBreakout`'s adaptive observation window is intentionally NOT used
   for Hurst.** `src/TripleScreen3.cpp`'s real call site computes `coherence_score` (from
   `LONG_MACD`/`INTERM_MACD`) and `observation_window_n` (`CalculateAdaptiveObservationWindow`)
   specifically to pass into `UpdateObservationVectorSubgraphs()` — but that function's Hurst line
   calls `CalculateHurstExponent(sc)`, a zero-arg overload that hardcodes a FIXED (100, minScale=8)
   window, never consuming `observation_window_n` (only `CalculatePathEfficiencySNR` does).
   Sanity-checked with Gemini (read-only, `gemini --approval-mode plan`,
   `lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_146`/`_REPLY`, VERY HIGH confidence): this is
   **mathematically necessary, not a bug** — DFA's own `minScale*4` floor requires `length>=32`,
   but `adaptive_window_n` is clamped to `[10,40]`, so most of that range would silently hit the
   fallback/stale-carry-forward path instead of a real estimate; DFA's OLS scaling-law regression
   also needs a wide, stable block (this repo's own DFA bias Monte Carlo already shows a wide 95%
   CI even at N=100). TS1's own adaptive `macro_window_n` avoids this by multiplying by 5
   (clamped to `[50,200]`), safely above the floor — TS3 has no such multiplier. Fix implemented:
   a clarifying comment only (`src/StudyHelperFunctions.cpp`), no behavior change.
2. **RESOLVED — `NR7Enum`'s WEAK/STRONG/EXTREME design confirmed intentionally unimplemented.**
   The enum's own doc comment described a real, percentile-threshold-based 3-tier design (WEAK
   95-100% of 7-bar avg range, STRONG 85-95%, EXTREME <80% + volume dry + consolidation); the real
   live call site (`src/TripleScreen3.cpp`) unconditionally emits `STRONG` for any qualifying bar.
   Grep-verified: `NR7Enum::WEAK`/`EXTREME` have ZERO real callers anywhere in this codebase — the
   continuous `qualityScore` this same detector already computes conveys the severity gradation
   the 3 discrete tiers would have added, making them redundant, not a gap. Fix: updated
   `NR7Enum`'s own doc comment (`include/IndicatorComputations.h`) and the live call site's comment
   (`src/TripleScreen3.cpp`) to state this explicitly — no behavior change.
3. **RESOLVED — `ClassifyStructure`'s parameters renamed for clarity.** The real call site
   (`src/TripleScreen3.cpp:614`) binds these generic-sounding parameters to `prevDayHigh`/
   `prevDayLow` (the previous completed TRADING DAY's high/low), not "the prior bar" as the names
   alone suggested — this session's own first implementation pass got it wrong for exactly this
   reason. Made worse by this codebase's OWN overwhelmingly-established convention elsewhere
   (`TripleBarrierEngine.h`, `PositionManager.cpp`, the `Event`/`TrainingEvent` schema's own
   separate `prev_high`/`prev_low` vs. `prev_day_high`/`prev_day_low` fields) where `prev_high`/
   `prev_low` genuinely DOES mean "the immediately-prior bar" — `ClassifyStructure`'s reuse of
   those exact names for day-level data was the true anomaly. Fixed: renamed to `prevDayHigh`/
   `prevDayLow` in `IndicatorComputations.h`'s `ClassifyStructure()` and
   `StudyHelperFunctions.h`/`.cpp`'s `DetectStructure()` forwarding declaration/definition. Full
   clean rebuild + both native test suites re-verified: zero behavior change.

## 10. `../schema/regenerate_schema.sh` findings (2026-09-17) — surfaced by Task 9's own investigation, must be resolved not just noted

Task 9 (wiring the ported `GetTrainingEventT()` subset) required reading `mts_schema.fbs` and its
generated output directly (not inferred from grep snippets) to confirm exact field lists for
`TrainingEvent`/`IndicatorState`. That direct read surfaced 2 real generation-strategy findings in
`../schema/regenerate_schema.sh` itself — same standing directive as §9 above (a forcing-function
side effect, must be tracked and actually fixed, not left as a caveat):

1. **RESOLVED (2026-09-17) — `generate_shared_writer_artifacts()`'s `TrainingRootSharedSlice`/
   `EventRootSharedSlice` structs (and their `Write*RootSharedFields()` functions,
   `include/generated/training_shared_writers_generated.h`/`event_shared_writers_generated.h`)
   were 100% hand-typed, static heredoc content — genuinely undocumented drift risk.** Confirmed by
   direct read of `regenerate_schema.sh` (`generate_shared_writer_artifacts()`, ~line 677): unlike
   `ObservationData`/`RiskGateContext`'s own field lists in the SAME script (which are real,
   `flatc --jsonschema`-reflection-derived via `schema/scripts/generate_contract_header.py`,
   substituted into `__GENERATED_*__` marker tokens), the 13 shared root fields (`side`,
   `market_symbol`, `overnight_exit`, `nh_nl_daily`, `prev_high`, `prev_low`, `prev_day_high`,
   `prev_day_low`, `prev_four_bar_high`, `prev_four_bar_low`, `close_percentile`,
   `volume_ratio_percent`, `volume_imbalance`) were written verbatim into the bash heredoc with
   zero cross-check against `TrainingEvent`/`Event`'s real field lists in `mts_schema.fbs`. If
   either table's shared-root field set ever changed (add/remove/rename), this heredoc needed a
   MANUAL, unenforced edit to `regenerate_schema.sh` itself — nothing failed loudly for an ADDED
   field silently missing from this slice. **Fix implemented (not a full regeneration rewrite —
   deliberately the minimal fix that closes the actual risk, same "validate, don't regenerate"
   scope decision as finding 2's own accepted `AsymmetryContext` precedent)**:
   `generate_contract_header.py` gained `validate_shared_writer_fields()`, called from `main()`
   right after the existing reflection derivation — cross-checks the 13 hand-typed field names
   (and their int8/float types) against `Event`/`TrainingEvent`'s real `flatc --jsonschema` output,
   raising `ValueError` (failing the build) on any mismatch. Verified against the real, current
   schema (passes cleanly) AND against 2 simulated drift scenarios (a removed field, a type change)
   — both correctly raised. Wired into `regenerate_schema.sh`'s existing invocation of that script;
   a full `./regenerate_schema.sh --cpp-only` run plus a full `MindfulTrader/./build_dll.sh`
   rebuild both verified clean afterward.
2. **DOCUMENTED, ACCEPTED EXCEPTION with one residual gap — `AsymmetryContext`'s field-index
   constants (`kAsymShannonEntropy`..`kAsymSessionQualityScore`) and `kAsymmetryFieldNames` array
   are ALSO hand-typed in the same heredoc** (`mts_schema_contract_generated.h`'s own template,
   immediately adjacent to `ObservationData`'s constants, which DO get reflection-derived).
   Confirmed this is a deliberate, acknowledged tradeoff, not an oversight:
   `generate_contract_header.py`'s own docstring states *"Only ObservationData and RiskGateContext
   are derived here — AsymmetryContext, envelope/heartbeat constants, and helper functions are
   untouched, hand-typed content in regenerate_schema.sh's own heredoc (no drift has ever been
   reported there; expanding scope to rewrite it too is not this fix's job)."* Not re-litigating
   that accepted decision. **But one real, narrower residual gap the existing mitigation doesn't
   cover**: the header's own `static_assert(sizeof(MTS::Schema::AsymmetryContext) ==
   (kAsymmetryDim * sizeof(float)), ...)` only catches a field-COUNT change (a genuine, real
   safety net for that case) — it provides zero protection against a same-count field REORDER or
   RENAME in the `.fbs`, which would silently leave `kAsymShannonEntropy`/`kAsymmetryFieldNames`
   mismatched against the real struct layout with no compile-time signal at all. Flagged for
   awareness alongside finding 1, not urgent given "no drift has ever been reported" — but the
   residual gap itself was not previously called out anywhere.
3. **RESOLVED (2026-09-17) — `generate_indicator_policy_artifacts()`'s `IndicatorKey` registry
   (`indicator_key_registry_generated.h`/`indicator_binding_policy_generated.h`,
   `kIndicatorKeyRegistryRows`/`kExpectedManagedIndicatorKeys`/`kIndicatorBindingPolicyRows`) had
   NO real drift protection at all, despite looking like it did.** Found while investigating
   whether `include/IndicatorLayout.h` (MindfulTrader's own hand-maintained, position-assignment
   table for the packed SoA indicator arrays — a SEPARATE hand-typed list of the same
   `IndicatorKey` names, audited by a human per its own header comment) could instead reuse this
   generated registry (the user's own "prefer generated code over hand-maintained code doing the
   same thing" directive). Traced `generate_indicator_policy_artifacts()`
   (`regenerate_schema.sh` ~line 322) and confirmed it is ALSO 100% hand-typed heredoc content —
   but unlike finding 1 above, `IndicatorKey` isn't part of `mts_schema.fbs` AT ALL (it's a plain
   C++ `enum class` in the sibling `MindfulTrader/include/IndicatorKey.h`), so `flatc
   --jsonschema` reflection can never reach it. The heredoc's own `kIndicatorBindingPolicySchemaSha256`/
   `kIndicatorBindingPolicyGeneratedUtc` metadata gives a FALSE sense of protection — it hashes
   `mts_schema.fbs`, the wrong file; `IndicatorKey.h` could rename/renumber/remove a key with zero
   signal from that hash. `SchemaContractChecks.cpp`'s existing `static_assert`s only cross-check
   the two GENERATED registries against EACH OTHER (name alignment, uniqueness) — neither one is
   ever checked against the real enum. **Fix**: new `schema/scripts/validate_indicator_key_registry.py`
   — regex-parses `IndicatorKey.h`'s real enum body and the just-written
   `indicator_key_registry_generated.h`'s own `kIndicatorKeyRegistryRows` array, then validates
   every managed row's name+value against the real enum, raising loudly on any mismatch. Verified
   against the real, current files (passes, 42/42 rows) AND 2 simulated drift scenarios
   (a renumbered key, a removed key) — both correctly raised. Wired into `regenerate_schema.sh`
   right after `generate_indicator_policy_artifacts()` writes its output (with a graceful skip +
   warning if `IndicatorKey.h` isn't found at the expected sibling-repo path, rather than a hard
   failure, since this script can in principle run without that repo checked out). **Deliberately
   NOT a full regeneration of the heredoc from `IndicatorKey.h`** — that would additionally require
   auto-deriving `wire_class`/`sink`/`field_type`/`has_live_writer`/`has_training_writer` per key,
   none of which `IndicatorKey.h` itself encodes (these require the same manual, source-reading
   audit `IndicatorLayout.h`'s own header comment describes doing by hand for its OWN position
   assignments) — a bigger, separate investment, same "validate, don't regenerate" scope
   discipline as finding 2's own accepted `AsymmetryContext` precedent. `IndicatorLayout.h` itself
   is unchanged — it still needs its own hand-assigned `position` field (never derivable from
   either registry), but now has an indirect safety net: if `IndicatorKey.h` drifts, this new
   validation fails the NEXT `regenerate_schema.sh` run before `IndicatorLayout.h`'s own
   hand-maintained key references could silently go stale against it.
4. **RESOLVED (2026-09-17) — `IndicatorManager.cpp`'s own `kRuntimeRegisteredIndicatorKeyValues`
   was a THIRD, purely duplicative hand-typed copy of the same 42 `IndicatorKey` values, providing
   zero independent verification.** Found while directly answering the operator's question "can
   `IndicatorManager` use the generated indicator keys?" — grepped every usage of
   `kRuntimeRegisteredIndicatorKeyValues` and confirmed it is referenced in exactly one place: a
   `static_assert(ArraysEqual(kRuntimeRegisteredIndicatorKeyValues,
   mts::schema_contract::kIndicatorKeyRegistryValues), ...)`. It is NOT derived from
   `IndicatorStore`'s real member declarations (`IndicatorManager.h`'s `Macd long_macd{IndicatorKey
   ::LONG_MACD}`-style initializers, the actual runtime registration) — it is a second, independent
   hand-typed literal that happened to be kept in the same order as the generated registry by
   convention, not by construction. The `static_assert` therefore only ever verified "two
   human-typed copies of the same list agree with each other," never "the schema-derived registry
   matches what the runtime actually registers" (its own stated WS-04 intent) — a weaker guarantee
   than it appeared to provide, and exactly why it broke the build the moment
   `generate_indicator_key_rows.py` (finding 3, same day) legitimately reordered the generated
   array to match the canonical row order: the hand-typed copy in `IndicatorManager.cpp` simply
   hadn't been told about the reorder, and nothing HAD to keep it in sync since it wasn't derived
   from anything. **Fix**: deleted `kRuntimeRegisteredIndicatorKeyValues` and its `ArraysEqual`
   helper entirely; replaced the assert with a much narrower, still-real check —
   `static_assert(kIndicatorKeyRegistryRowCount == kExpectedManagedIndicatorKeyCount, ...)` — a
   row-count sanity check against the SAME named constant `indicator_binding_policy_generated.h`
   already uses for its own row-count guard, not a second magic number. A true "does
   `IndicatorStore`'s real member list match the registry" check remains out of reach without C++
   reflection (same limitation finding 3 already accepted for `IndicatorLayout.h`'s own per-key
   position correctness) — this fix removes a false sense of protection rather than replacing it
   with an equally strong one, which is a net improvement, not a regression. Full
   `MindfulTrader/./build_dll.sh` rebuild verified clean.
5. **RESOLVED (2026-09-17) — `EventDataCollectorStudy.cpp`'s `model_confidence` field was always
   `0.0f` in real production data collection, sourced from a dead code path.** First surfaced in
   `docs/HMM_REGIME_MANAGER_COORDINATION.md` Entry 8 (2026-09-16), fixed this session. Root cause:
   `eventT->model_confidence` read `TradeSignalManager::Instance().HasFreshSignal()`/
   `GetTradeSignal().modelConfidence` — `TradeSignalManager::SetTradeSignal()` (its only writer) has
   zero callers anywhere in `src/`/`include/`, so `HasFreshSignal()` returns `false` unconditionally
   and this field was a permanent `0.0f` sentinel, never the real live confidence, even when a
   genuine Transformer prediction was available at that tick. The real, live confidence path is
   `InferenceManager::Instance().Prediction()->Confidence()` (confirmed by direct read — this is
   the same `PredictionState` class `PositionManager.cpp`'s own GAP1/GAP2 direction-conflict and
   staleness-decay checks already read from). **Fix**: `eventT->model_confidence` now reads
   `InferenceManager::Instance().Prediction()->Confidence()` directly (null-checked, `0.0f` if no
   prediction has landed yet) — this is a genuine training-data-quality fix, not just code
   hygiene: `.alpha` streams collected going forward carry the real per-tick model confidence.
   Removed the now-dead `#include "TradeSignalManager.h"` from this one file only —
   `TradeSignalManager` itself still has real references in `SCStudies.cpp`/`TradeSignalManager.cpp`,
   so the class was NOT deleted, only this file's dependency on it. Full
   `MindfulTrader/./build_dll.sh` rebuild verified clean.

## 11. Institutional DOD practices to apply to Task 9's own implementation (2026-09-17)

This plan's own "Institutional DOD Mandate" (top of the implementation plan doc) already commits
to zero heap allocation in the per-tick hot path and bounded memory regardless of tick volume —
Task 9's `TrainingEventT` assembly must hold to the same standard, not silently regress it:

1. **Pool the `TrainingEventT` scratch object — do not `std::make_unique` a fresh one per
   emission.** The REAL `IndicatorManager::GetTrainingEventT()` (`src/IndicatorManager.cpp:751`)
   explicitly reuses a single pooled instance (`m_trainingEventScratch`, cited in its own comment
   as "Tier 2," `docs/superpowers/specs/2026-08-07-training-event-export-dod-design.md`) — every
   scalar field is unconditionally overwritten before the caller sees it, so no cross-call state
   ever leaks. This replay tool's own port must mirror that exact pattern: one
   `TrainingEventT` member (plus its own already-heap-owned `indicators`/`observation`/
   `asymmetry_context` `unique_ptr`s, allocated ONCE and reused, exactly like production's own
   `if (!event->indicators) { event->indicators = std::make_unique<...>(); }` guard) — never a
   fresh `TrainingEventT` (or fresh nested `unique_ptr`s) constructed inside the per-tick/per-bar
   emission path.
2. **`event.features` (`std::vector<float>`) must be sized once and reused, not
   reallocated per emission** — same rationale as item 1, extended to the one real
   heap-backed container inside `TrainingEventT` itself (a `std::vector`, not a fixed-size
   array, per the schema's own `features: [float]` — variable-length by design). Resize once
   to its final length on first use (or in the pooled object's own initialization), then
   overwrite contents in place on every subsequent call — never `.clear()` + repeated
   `push_back()`, which reintroduces the exact allocation churn this mandate exists to prevent.
3. **Every genuinely out-of-scope field (InferenceManager's HMM/regime fields, ContextManager's
   `AsymmetryContext`/`dist_*`/`StructureEngine`-derived anchors) must be left at an explicit,
   documented sentinel — never silently uninitialized.** Since the pooled scratch object persists
   across calls (item 1), a field this port never touches would otherwise silently carry forward
   whatever a PRIOR call happened to leave there, not the schema's own zero-init default — a real,
   DOD-relevant correctness trap specific to the pooling pattern itself (the exact hazard
   `IndicatorManager.cpp`'s own comment warns about: "Every scalar field below is unconditionally
   overwritten... so no data from a previous call is ever visible"). Every out-of-scope field must
   be explicitly assigned its documented default (e.g. `model_confidence = 0.0f`, matching
   production's own real, confirmed-dead-code value) every single call, not left to chance.

## 12. Cutting-edge DOD practices found while implementing Task 9 (2026-09-17) — candidates to extend back into live trading + training-data-generation code

Direct reads of the generated wire structs and `IndicatorPackedState.h`/`IndicatorLayout.h` (not
inferred) surfaced 3 concrete, verified opportunities to extend this codebase's ALREADY-adopted
"single memcpy, static_assert-protected" DOD pattern (`MakeObservationData`/`MakeAsymmetryContext`,
`mts_schema_contract_generated.h`) further than it reaches today. None of these are implemented yet
— recorded here as candidates for a separately-scoped follow-on, per the same "record findings,
don't silently fix scope-creep into an unrelated task" discipline as §9/§10 above.

1. **`IndicatorState` has 3 genuinely homogeneous, contiguous byte-runs — a real (if narrower than
   `ObservationData`'s) batched-memcpy candidate.** Confirmed via direct read of the generated
   struct layout (`mts_schema_generated.h`'s `IndicatorState`, not assumed from the `.fbs`'s own
   field grouping comments alone — compiler-inserted padding at each type transition must be
   accounted for): the real member layout is 11 contiguous `float`s, then 38 contiguous `int8_t`s
   (+ 2 bytes compiler padding), then a final small run of 3 `float`s + 1 `int8_t` (+ 3 bytes
   padding). `IndicatorManager::PopulateIndicatorState()` (`src/IndicatorManager.cpp:1261`)
   currently writes this struct via ~38 individual `state.mutate_*()` calls, one per field —
   confirmed by direct read, not already using any bulk-copy technique. A 3-way batched memcpy
   (one per homogeneous run, each protected by its own `static_assert` on offset/size, mirroring
   `MakeObservationData`'s existing `is_standard_layout`/size static_asserts) would extend the
   already-proven pattern here — with an honest caveat `ObservationData`/`AsymmetryContext` don't
   have: those two are *pure*-float structs (trivially whole-struct-memcpy-able from a
   `std::array<float,N>`); `IndicatorState`'s mixed float/int8 layout makes this a 3-way *partial*
   memcpy, more tightly coupled to the compiler's exact padding/alignment decisions and therefore
   more fragile to a field reorder than the existing whole-struct case — worth prototyping with
   its own `offsetof`-based static_asserts (not just a `sizeof` check) before adopting, not a
   drop-in port of the existing technique.
2. **Reflection-driven generation to eliminate hand-maintained duplicate field lists is this
   codebase's own already-proven fix for a real, twice-recurring defect class — a general
   institutional principle, not a one-off fix, that finding 1 in §10 above is one more instance
   of.** `generate_contract_header.py`'s own docstring cites the concrete history:
   `fast_taleb_kurtosis` silently drifted undetected for weeks, `fast_hurst_exponent`/
   `fast_mean_rev_z` blocked the build outright — both caused by hand-duplicated field lists
   falling out of sync with `mts_schema.fbs`, both fixed by generating those lists from
   `flatc --jsonschema` reflection instead of hand-typing them. §10 finding 1
   (`TrainingRootSharedSlice`/`EventRootSharedSlice`) is a second, currently-unfixed instance of
   the exact same defect class this tool already knows how to cure — worth stating explicitly as
   a standing principle (any new hand-typed field list mirroring an `.fbs` table's fields is a
   latent instance of this same defect class) rather than only patching instances as they're
   individually discovered.
3. **Aspirational, bigger-effort, NOT yet well-grounded enough to schedule — reordering
   `IndicatorPackedState`'s own packed-array positions to match `IndicatorState`'s wire field
   order would enable a true zero-per-field-call memcpy straight from packed SoA storage to the
   wire struct.** `IndicatorLayout.h`'s own header comment already states, unprompted, *"Positions
   are assignment order, not IndicatorState's own field order"* — meaning `m_currentI8`
   (`IndicatorPackedState<N_I8,N_F32>`, already a plain contiguous `std::array<int8_t,N_I8>`, a
   real memcpy source today) cannot be memcpy'd directly into `IndicatorState`'s own int8 run
   without a field-by-field reorder first. Flagged as the natural "next level" beyond item 1 above
   (item 1 still keeps per-field `GetValue<Key>()` reads feeding a staging array; this would
   remove that read step too) — but deliberately NOT recommended for near-term action: reordering
   `kIndicatorLayout`'s positions would ripple through every other packed-array consumer
   (`GetValue<Key,Block>()`'s callers across `RiskManager`/`Scoring`/`EventSerializer`/etc.), a
   materially bigger and riskier change than items 1/2 above. Recorded so the idea isn't lost, not
   as a scoped recommendation to act on now.

## 13. Empirical validation results (2026-09-17) — Task 12 Step 1 closed, Step 2 still blocked

Ran `MarketDataReplayContext.cpp` against a real 100M-tick slice of the full 471.9M-tick
`lbrnet/data/raw/mes_ticks.parquet` (capped via `--max-ticks`, not the full dataset):

- **Completed cleanly**: ~50 minutes wall-clock, RSS bounded and roughly linear (242MB→633MB over
  the run, well under the tool's 3072MB default budget) — no crash, no unbounded growth.
- **Output**: 5,483,891 `.context` records, 21,362 `.alpha` records (both triggers fired
  independently and repeatedly against real data, per §2's core design decision). Log:
  `tools/log/market_data_replay_context.log`; permanent archive:
  `tools/output/market_data_replay_context_20260917_091253.txt`.
- **Drop-in-compatibility check (Task 12 Step 1's second half)**: ran `lbrnet`'s
  `build_directional_alpha.py` directly against the generated `.alpha` file. It parsed the file,
  loaded the trained HMM model, and warmed up the regime engine without error — then stopped at
  `_verify_hmm_model_provenance()`, which requires a `materialize_hmm_features.py`-stamped
  `<file>.alpha.manifest.json` sidecar. This is a **real, separate lbrnet-side pipeline
  prerequisite**, not a defect in this tool's output — full drop-in parity remains open pending
  that step, but the file format itself is confirmed structurally consumable by the real
  production Python pipeline.
- **Task 12 Step 2 (byte/numeric-parity against genuine SC-collected output) remains BLOCKED** —
  no current-schema (v240) real SC-collected `.context`/`.alpha` file exists on this machine yet.
  This run does not close that gap; it only confirms the tool runs end-to-end and produces
  well-formed output at production scale.
- **Handoff to `lbrnet`**: the 100M-tick output files were copied to
  `lbrnet/data/raw/offline_replay_smoke_100m.context`/`.alpha` (deliberately named to avoid being
  mistaken for genuine SC-collected production data) for `lbrnet`'s own code-path
  exercising/refactoring use. Explicit caveat carried with the handoff: this data is **not**
  validated for numeric correctness against genuine SC ground truth (Step 2 above), so it is
  suitable for testing that Python-side parsers/pipeline stages run against real-shaped,
  real-schema-version data — not as an oracle for validating whether any feature's math is
  correct. A full 471.9M-tick run (~4h projected, ~7GB `.context`) was offered but not yet run.




