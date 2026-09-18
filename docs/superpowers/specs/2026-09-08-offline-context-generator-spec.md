# Offline `.context` Training-Data Generator — Architecture Spec

**Status, opened 2026-09-08**: re-created after the original spec-in-progress was lost to a VS
Code crash (grep over massive files in `MindfulTrader`/`lbrnet`/`MTS` triggered it). The supporting
code survived uncommitted in the working tree (`include/TickBarAggregator.h`,
`include/EasternTimeOffset.h`, `include/ObservationTriggerGate.h` + native tests) and has been
build/test-verified; this doc reconstructs the design around it from scratch, since the original
design notes were never saved to a file. Treat every "RESOLVED" below as this session's judgment
call, not a recovered decision — confirm before trusting it as settled.

## 0. Mandate

Build a standalone C++ CLI tool that reconstructs genuine `.context` training files (the same
binary format `LBRFileManager`'s `.context` stream writes today) directly from raw historical MES
tick data — **without running Sierra Chart in replay mode at all**. Sierra Chart replay is slow,
requires a licensed, running instance, and ties training-data generation to wall-clock replay
speed; every dependency this tool needs has already been shown to be pure, ACSIL-independent C++
(see §1) or a thin, replaceable ACSIL-array-gather shim (see §2).

Directly enabled by, and the concluding step of, the "Side thread — offline `.context` generator
feasibility" opened in
`docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` (its own final
words: *"every one of the 18 dims' real math, plus the Mahalanobis gate itself, is now provably
pure and independently reusable — the generator itself is not yet built, no dedicated spec written
yet"*). This doc is that dedicated spec.

**Priority clarification (operator directive, 2026-09-08): the current goal is training and
analyzing the HMM (Elite Feature Set Curation Phase 2, Feature Saliency EM) — emphasize the dims
that actually feed it, deprioritize everything that doesn't.** The schema's own comments settle
this precisely, not a judgment call: `ObservationData` is annotated *"Authoritative HMM Input"*
(`mts_schema.fbs:470`); `AsymmetryContext` is annotated *"Transformer-Specific 8D... NOT used by
HMM"* (`mts_schema.fbs:416-417`); `RiskGateContext` exists to let "the Python execution sim check
gates against the identical raw values C++ uses" (`mts_schema.fbs:434-436`) — a risk-management
parity concern, not an HMM training input either. Concretely, for the stated goal right now:
- **`ObservationData` (18D) construction is the only must-be-byte-accurate deliverable** — §2's
  per-dim table, §3a's per-tick order, and `TickBarAggregator.h`'s session-anchored bars are the
  critical path.
- **`AsymmetryContext` (§1b) can be a placeholder for now.** `LBRFileManager::LogContext()` takes
  it by `const&`, not a nullable pointer, so *some* value must be supplied — but since it's a
  Transformer input, not an HMM one, a default-constructed `MTS::Schema::AsymmetryContext{}` is
  acceptable until/unless a Transformer-training consumer of this tool's output is actually
  scoped. §1b's full construction trace stays valid, just deferred.
- **`RiskGateContext` can be omitted entirely.** Unlike `AsymmetryContext`, `LogContext()`'s
  `risk_gate_context` parameter is a nullable pointer (`const RiskGateContextT*`) —
  `context_to_parquet.cpp` already handles `nullptr` gracefully (writes its own documented
  defaults + a `risk_gate_context_available=false` column). Passing `nullptr` moots open question
  8's `regimeDuration`/live-HMM-dependency blocker entirely for the current goal — it only matters
  if/when a real execution-sim-parity consumer is scoped.

**Standing fidelity directive (operator, 2026-09-08): be as faithful as possible to a real Sierra
Chart replay — or better, to a real Sierra Chart live trading session.** This governs every design
choice where "simpler" and "what SC itself actually does" diverge, not just one open question: this
tool should behave as if it WERE the live/continuous chart, not an approximation of one. Concretely
already applied in §3 open question 1 (splice tick data continuously across contract rolls, exactly
matching SC's own front-month rollover convention, no per-contract state reset) — the same lens
should be applied to every future design choice this spec makes.

**Reusability mandate for a future `.alpha` generator (operator directive, 2026-09-08) — SUPERSEDED
2026-09-16, see `docs/superpowers/specs/2026-09-16-market-data-replay-alpha-generator-spec.md`.**
The one-shared-trigger framing below turned out not to match production once traced end-to-end:
`.context` (this spec) and `.alpha` are gated by two genuinely **independent** triggers in real
production (`ContextManager::CheckAndTriggerHMM`'s Mahalanobis gate vs. `EventDataCollectorStudy
.cpp`'s own Lock A-E + `HasSignificantChange()` gate) — not one trigger with a second consumer
bolted on. The new spec's design keeps this generator's per-tick observation-vector computation
fully reusable/unmodified (confirmed real production parity: `.alpha`'s observation field is
sourced from the exact same `m_latestScaledObs` buffer this generator already reconstructs) but
adds `.alpha` emission as a **separate, independently-gated, independently-toggleable** path
alongside it, rather than a shared-trigger extension. Original text, kept for history: The
offline `.alpha` (`TrainingEvent`) generator is a stated near-future follow-on, not speculative —
so per this repo's own explicit "single source of truth, not a parallel reimplementation"
convention (`CLAUDE.md`'s `IndicatorManager` Task 10 note; the `GetTrainingEventT`
`close_percentile` dead-write bug this convention exists to prevent), **the per-tick pipeline
built here must be one reusable core, not a `.context`-only port that gets a second,
independently-ported `.alpha` sibling later.** `EventDataCollectorStudy.cpp` is the existing proof
this is the right shape: it already runs ONE per-tick loop and conditionally emits to both
`.context` and `.alpha` from the same state, never two separate studies. Concretely: §3a's per-tick
steps (bar aggregation, `ActivityClockManager`/`ContextManager` feed, the `CheckAndTriggerHMM`
gate) should live in one core loop/class that a `.context`-only CLI invocation drives today, and a
later `.alpha`-inclusive invocation reuses unchanged — adding only the Lock A-E gate and
`GetTrainingEventT()` call as an additional consumer of the same tick-by-tick state, not a parallel
walk of the same ticks. **This reverses §3a step 2's "drop entirely" call**: `IndicatorManager::
UpdateBarContext(sc)` is genuinely unneeded for `.context` alone, but feeds `.alpha`'s Lock B
warm-up gate — so the reusable core keeps the call site (optional/feature-flagged), it just isn't
invoked by a `.context`-only run.

**Folder name — CONFIRMED 2026-09-08: `tools/market_data_replay/`.** Operator's own framing: this
tool doesn't just generate training data, it **replaces Sierra Chart's own replay mechanism with our
own tick-based replay** — "market data replay" is the standard institutional term for exactly this
(rebuilt from historical tick/quote data, replayed through the live processing pipeline for
research/backtesting, decoupled from the original venue/platform's own replay tooling): "market
data" is the standard noun for the raw tick/quote/trade feed itself (market data handler, market
data feed, market data normalization), and pairing it with "replay" names exactly what this tool
does. Operator's original recollection of `tools/training_data_preparation` is superseded — not
independently recoverable (no trace in any spec/plan/committed code) and undersold the "replaces SC
replay" framing. **All new files for this tool land under `tools/market_data_replay/`.**

## 1. Already-built, already-verified pure components (no ACSIL dependency)

All confirmed by direct inspection this session (file:line cited), not assumed:

| Component | File | Role | Purity evidence |
|---|---|---|---|
| Tick→bar aggregation | `include/TickBarAggregator.h` | Reconstructs TS1(240m)/TS2(60m)/TS3(15m) session-anchored bars from raw UTC tick timestamps, replicating Sierra Chart's `IBPT_DAYS_MINS_SECS` boundary convention | Header-only, `#include`s only `EasternTimeOffset.h` + STL. Native test `tests/cpp/test_tick_bar_aggregator.cpp`, 17/17 pass |
| DST-aware ET conversion | `include/EasternTimeOffset.h` | `GetEasternUtcOffsetSeconds()`, needed because ticks are UTC epoch-us but CME session start (18:00 ET) is defined in Eastern Time | Header-only, Howard Hinnant civil-calendar algorithm, no external deps. Native test `tests/cpp/test_eastern_time_offset.cpp`, 10/10 pass |
| Mahalanobis significant-change gate | `include/ObservationTriggerGate.h` | The exact gate `ContextManager::CheckAndTriggerHMM()` uses to decide whether an observation is novel enough to emit — energy/geometry split-channel robust z-score, velocity-adaptive epsilon/noise floor, sanitization clamps | Header-only, `ContextManager.h` now composes an instance of this class (`m_triggerGate`) rather than duplicating the logic — single source of truth, not a parallel reimplementation. Native test `tests/cpp/test_observation_trigger_gate.cpp`, 25/25 pass |
| `ObservationData`/`AsymmetryContext` conversion | `include/generated/mts_schema_contract_generated.h` (`MakeObservationData`/`ToObservationArray`) | Converts the pipeline's raw `std::array<float,18>` into the wire struct | `memcpy`-based, standard-layout-asserted. Native test `tests/cpp/test_mts_schema_contract.cpp`, 7/7 pass |
| `.context` binary writer | `include/LBRFileManager.h` / `src/LBRFileManager.cpp` | `Open()`/`LogContext()` — writes the exact size-prefixed `LBRN`-magic, `MarketObservation`+`SystemState` paired-record format the existing reader (`tools/context_pipeline/context_reader.h`) already consumes | **CORRECTED 2026-09-08 (Task 9 implementation)**: the earlier "confirmed pure" claim below only checked `LBRFileManager.h`'s own includes, not `src/LBRFileManager.cpp`'s — the `.cpp` transitively includes `MindfulTrader_Precompiled.h` → `sierrachart.h` → `windows.h` (genuinely Windows-only, confirmed by direct compile attempt), so it CANNOT be linked into a standalone Linux tool as originally claimed. Fixed by creating `tools/market_data_replay/ContextFileWriter.h`, a deliberate byte-faithful duplicate of the `.context`-only write path (not a reimplementation from scratch — every format detail copied verbatim). ~~`LBRFileManager.h` includes only `<fstream>`/`<mutex>`/the generated schema header; no `SCStudyInterfaceRef`/`sc.*` anywhere. This tool does not need to reimplement the writer — it can call the real singleton directly.~~ |
| Per-dim math (17 of 18 dims), extracted earlier | `BipowerVariation.h`, `CarryForwardCalculators.h`, `SevcikFractalDimension.h`, `DfaHurstExponent.h`, `OrderFlowAsymmetryEngine.h`, `EventVelocityEngine.h`, `RobustMoments.h`, `RecurrenceRateEngine.h`, `MeanReversionCalculator.h`, `LiquidityFragilityEngine.h` | Each dim's real production formula | Per `2026-08-31-elite-feature-set-curation-initiative.md`'s own Phase-1 audit — confirmed pure, natively tested individually |
| Tick-level engines | `include/TailRiskEngine.h` (Hill estimator → `tail_index`), `include/InformationEngine.h` (LZ76 → `lempel_ziv`) | Rolling, event-indexed (not bar-indexed) computations, members of `ContextManager` itself | **Confirmed pure this session** — zero `sc.*`/ACSIL references in either header |
| `FeatureScaler::UpdateAndNormalize` | `include/FeatureScaler.h` | Hybrid Soft-Log-Z (state dims) / Log-Z (energy dims) scaling — the exact transform between raw and wire-format observations | Confirmed pure in the 2026-09-07 audit (`2026-08-31-elite-feature-set-curation-initiative.md`) |
| Raw historical tick source | `lbrnet/data/raw/mes_ticks.parquet`, written by `tools/scid_processing/scid_to_ticks_parquet.cpp` | 471.9M real MES ticks, columnar (`timestamp_us`, `trade_price`, `ask_price`, `bid_price`, `spread`, `volume`, `bid_volume`, `ask_volume`, `trade_side`, `contract`) | Already the standard input for every other offline validation tool in this repo (`tools/observation_vector/*_eval.cpp`) |

**Prior art found this session — reference, not reusable as-is.**
`tools/observation_vector/whole_vector_redundancy_eval.cpp` (11 dims) and its predecessor
`observation_vector_recalibration.cpp` (9 dims) already stream `mes_ticks.parquet` and build
TS1/TS2/TS3 bars in-process, each formula's window size/simplification already validated against
real data — a genuinely useful per-dim reference for "what does production actually do, and what's
an acceptable approximation" (their own header comments document each deliberate simplification,
e.g. fixed vs. adaptive Hurst/Fisher windows). **But their bar boundaries are NOT session-anchored**
— both bucket with naive UTC-epoch division (`const long long bucket = ts / kTs1BarUs;`), not
`TickBarAggregator.h`'s 18:00-ET-anchored, DST-aware convention. Fine for a correlation/redundancy
audit (only bar-to-bar structure matters), **not safe to reuse directly** for a tool whose mandate
is byte-parity with real production `.context` files — this generator must build bars via
`TickBarAggregator.h`, porting these tools' per-dim formula choices, not their bucketing.

### 1a. Real `.context` wire content — a gap found against `../schema/mts_schema.fbs` (RESOLVED 2026-09-08)

§0-§3 above only traced `ObservationData` (18D). Checking the schema directly (per operator
request) against `LBRFileManager::LogContextUnlocked` (`src/LBRFileManager.cpp:166`) shows a
`.context` record is genuinely larger — **`AsymmetryContext` (8D) was never traced and is a real
gap**, plus two small items that turn out to be free:

- **`AsymmetryContext` (8D, schema `mts_schema.fbs:419`) — new requirement, not a subset of the
  18D vector.** Built by `ContextManager::GetAsymmetryContext()` (`src/ContextManager.cpp:536`)
  from `m_latestInstitutionalMetrics`: `shannon_entropy`/`shannon_efficiency` (`UpdateMarketPhysics`,
  `src/ContextManager.cpp:169-178`, from `InformationEngine::GetShannonEntropy()` — a value the
  generator's §3a step 3 already computes for `lempel_ziv`'s own engine, so this is nearly free),
  `taleb_kurtosis`/`taleb_skewness` (same call, `anchors.realizedKurtosis`/`skewnessIdx` — needs
  tracing `anchors`, not yet done), `taleb_cliff`/`pareto_rot` (= `elderChandelierATR`/`paretoRot`,
  already covered by §3a step 3b's `UpdatePriceStructure` trace), `raschke_burst` (`src/
  ContextManager.cpp:1035`, `CalculateBurstinessIndex(now_us)` inside `CheckAndTriggerHMM` itself —
  step 4), `session_quality_score` (= `elderImpulse`, the Close-Location-Value proxy, also §3a step
  3b). **Net: 6 of 8 fields piggyback on state §3a's existing steps already produce; only
  `anchors.realizedKurtosis`/`skewnessIdx`'s source needs its own trace before this is fully closed.**
- **`sequence_id` — free, no design needed.** `MarketObservation.sequence_id` is just
  `LBRFileManager::ReserveSequenceId()` (`src/LBRFileManager.cpp:24`, a mutex-guarded
  `m_globalSequenceId++`) — since §1 already established this tool calls the real
  `LBRFileManager` singleton directly, this comes for free, no independent counter needed.
- **`daily_bias_enum` — confirmed always 0 on this path, nothing to build.**
  `src/LBRFileManager.cpp:198`'s own comment: *"daily_bias_enum: unused on the .context path"* —
  hardcoded `0` in every call, live or offline. Do not spend effort computing `DailyBiasEnum` for
  this tool.
- **§3's open question 3 (`RiskGateContext` population) is now substantially de-risked, not fully
  new state.** Traced `m_localRiskContext`'s assignments (`src/ContextManager.cpp:429-524`) —
  5 of its fields are a straight copy of the same `m_latestInstitutionalMetrics` values
  `AsymmetryContext` needs anyway (`shannonFlowEntropy`/`shannonEfficiency`/`talebKurtosis`/
  `talebSkewness`/`raschkeBurst`), and 4 more are direct reads of the already-built raw
  `ObservationData` array (`amihudIlliquidity`←`obs[OBS_AMIHUD_ILLIQUIDITY]`,
  `spreadStress`←`obs[OBS_LIQ_FRAGILITY]`, `meanRevZ`←`obs[OBS_MEAN_REV_Z]`,
  `fisherInfo`←`obs[OBS_FISHER_INFO]`) or an engine the generator already runs
  (`paretoTailAlpha`←`m_cachedHillAlpha`, the `TailRiskEngine`'s Hill alpha; `isValid`←
  `m_featureScaler.warmedUp`). **Genuinely new state, not yet designed**: (a) `fractalDim` —
  NOT the same 400-bar HMM `fractal_dim`; it's a *separate, shorter-window* Sevcik fractal
  dimension (`src/TripleScreen2.cpp:301-313`'s `SetFractalDimShort()`, using an adaptive
  `slow_window_n` this tool hasn't traced yet — deliberately decoupled from the 400-bar HMM input,
  correlation ≈0.0115 between the two per that file's own comment, so one cannot substitute for
  the other); (b) `amihudPercentile` — the 21-day rolling RTH/overnight percentile pool
  (`ContextManager::PushAmihudSample()`, fed from `src/TripleScreen3.cpp:830`, gated on an
  RTH-vs-overnight session classification the generator can derive independently from
  `EasternTimeOffset.h` since it already needs ET conversion); (c) `regimeDuration` — set via a
  `SetRegimeDuration()` call this session did not yet trace to its source.

### 1b. `AsymmetryContext` (8D) — full construction trace (RESOLVED 2026-09-08, per operator request)

`ContextManager::GetAsymmetryContext()` (`src/ContextManager.cpp:536`) builds the 8 fields, in
wire order, from `m_latestInstitutionalMetrics`:

| Wire field | Source | Status |
|---|---|---|
| `shannon_entropy` | `m_latestInstitutionalMetrics.shannonFlowEntropy` ← `InformationEngine::GetShannonEntropy()`, set in `UpdateMarketPhysics()` (`src/ContextManager.cpp:169`) | **Free** — §3a step 3 already runs this engine for `lempel_ziv` |
| `shannon_efficiency` | `1 - min(shannonFlowEntropy/kMaxEntropy, 1)`, else neutral `0.5` (`src/ContextManager.cpp:176-178`) | **Free** — pure derived scalar once the above exists |
| `taleb_kurtosis` | `anchors.realizedKurtosis` — a `NormalizedAnchors` field fed via `ContextManager::SetNormalizedAnchors()` from `src/TripleScreen3.cpp:1381` (`Subgraph_RealizedKurtosis[signalBarIndex]`, computed by `CalculateRealizedKurtosis()`, `src/StudyHelperFunctions.cpp:2690`) | **New tracing needed** — see caveat below |
| `taleb_skewness` | `anchors.skewnessIdx`, same `NormalizedAnchors` path, `CalculateSkewness()` (`src/StudyHelperFunctions.cpp:2691`) | **New tracing needed** — same caveat |
| `taleb_cliff` | `= elderChandelierATR`, already traced in §3a step 3b (`UpdatePriceStructure`'s 22-bar Chandelier calc) | **Free** — no new work |
| `pareto_rot` | `= paretoRot = m_structureEngine.GetFractalDimension()`, also set inside `UpdatePriceStructure` (§3a step 3b) | **Free**, but see fractal-dim caveat below |
| `raschke_burst` | `CalculateBurstinessIndex(now_us)`, called inside `CheckAndTriggerHMM` itself (§3a step 4) | **Free — confirmed same value as `ObservationData.burstiness_index`** (`ContextManager::GetRaschkeBurst()`, `include/ContextManager.h:388`, is a pure getter over this same field, and `TripleScreen2.cpp:291` reads it for the `burstiness_index` dim) — one computation, two wire fields |
| `session_quality_score` | `= elderImpulse`, the Close-Location-Value proxy set in `UpdatePriceStructure` (§3a step 3b) | **Free** — no new work |

**Net: 6 of 8 fields cost nothing beyond what §3a's steps 3/3b/4 already compute** (2 are pure
derived scalars from step 3's own engine, 4 are direct reuse of already-traced values). Only
`taleb_kurtosis`/`taleb_skewness` require new work, and it's a targeted purity check, not a design
question: `CalculateRealizedKurtosis()`/`CalculateSkewness()` (`StudyHelperFunctions.cpp:2679-2691`)
are calendar-clock, TS3-bar-native Moors/Bowley estimators — **confirmed distinct from
`ObservationData`'s `skewness_idx`/`fast_taleb_kurtosis`**, which are activity-clock
(imbalance-bar-native). Do not conflate the two pairs; they are computed over different return
series and are expected to diverge.

**New finding: three independent fractal-dimension readings now exist in this system**, not two —
worth flagging so the generator doesn't accidentally collapse them into one: (1) the 400-bar HMM
`ObservationData.fractal_dim` (TS2, `SevcikFractalDimension.h`), (2) `RiskGateContext.fractalDim`'s
short-window sibling (`TripleScreen2.cpp`'s `SetFractalDimShort()`, §1a), and (3)
`AsymmetryContext.pareto_rot` = `m_structureEngine.GetFractalDimension()` — a third, `StructureEngine`-
internal fractal-dimension estimate, distinct from both. All three must stay independently computed.

**`regimeDuration` (§3 item 7/8, `RiskGateContext` only) — RESOLVED 2026-09-08, re-traced against
`src/Indicator.cpp`/`src/TripleScreen3.cpp:768-780`/`include/rc_enums.h`: production already has a
graceful no-HMM degradation path, and it's exactly what this tool needs, not an approximation.**
`SetRegimeDuration()` is called from `MarketClimateIndicator::UpdateContext(const LocalRiskContext&
ctx, HMMStateEnum hmmState)` (`src/Indicator.cpp:496`, NOT the `Update()` name originally assumed).
Its classification is two steps: **Step 1 (Physics Veto)** uses ONLY `talebKurtosis`/
`shannonFlowEntropy` — no HMM at all — and hard-overrides to `TALEBIAN_FRAGILE`
(`kurtosis > 1.4753`) or `SHANNON_CHAOS` (`entropy > 0.6 × kShannonMaxEntropyBits`) when tripped.
**Step 2 (Physics+HMM Hybrid)**, only reached if Step 1 doesn't fire: `isPhysicsMomentum`/
`isPhysicsCoil` are computed from pure physics (`hurst > 0.6 && entropy < 0.45×max` /
`hurst < 0.4 && entropy < 0.4×max`) and are alone SUFFICIENT to classify `PARETO_MOMENTUM`/
`COILED_SPRING` — the `isHmmMomentum`/`isHmmMeanRev` terms are only an additional `||`-branch, never
required. **Production's own designed sentinel for "no HMM state yet" is `HMM_NO_PRIOR = -1`**
(`include/rc_enums.h:59`, doc comment: *"Pipeline sentinel: HMM inference not yet received. Not a
trained model state"*) — `TripleScreen3.cpp:768` defaults `currentHmmState` to exactly this whenever
`InferenceManager::HmmState()` returns null, and passing it makes `isHmmMomentum`/`isHmmMeanRev`
both false (`-1` matches none of `HMMStateEnum`'s 4 real values 0-3), so the hybrid matrix cleanly
degrades to the pure-physics branch. **The generator should replicate this exact logic (all three
physics inputs — `talebKurtosis`, `shannonFlowEntropy`, `hurstExponent` — are already computed for
`ObservationData`/`AsymmetryContext`), always passing `HMM_NO_PRIOR`** — this is production's own
real fallback behavior (fires whenever the HMM is stale/disconnected/not-yet-warmed-up in live
trading too), not an invented simplification. Track `m_stateDuration` (bars where the classified
`MarketClimate` is unchanged) the same way to get `regimeDuration`. **No operator decision needed
after all — this was answerable from the code, not a genuine ambiguity.**

## 2. Per-dim ownership and what's NOT yet pure

Mapped this session by grepping each Triple Screen file's `obs->mutate_*` call sites (file:line):

| Dim (schema field) | Screen that owns it | Confirmed mutate call site |
|---|---|---|
| `log_scale_ratio` | TS1 (240m) | `src/TripleScreen1.cpp:628` |
| `hurst_exponent` | TS1 (240m) | `src/TripleScreen1.cpp:629` |
| `fisher_info` | TS1 (240m) | `src/TripleScreen1.cpp:631` |
| `relative_range` | TS2 (60m) | `src/TripleScreen2.cpp:334` |
| `burstiness_index` | TS2 (60m) | `src/TripleScreen2.cpp:335` (reads `ContextManager::GetRaschkeBurst()`, itself `EventVelocityEngine.h`-backed) |
| `fractal_dim` | TS2 (60m) | `src/TripleScreen2.cpp:339` (`SevcikFractalDimension.h`) |
| `log_scale_expansion_ratio` | TS2 (60m) | `src/TripleScreen2.cpp:361` (`BipowerVariation.h`) |
| `amihud_illiquidity` | TS3 (15m) | `src/TripleScreen3.cpp:807` (`CarryForwardCalculators.h`) |
| `liq_fragility` | TS3 (15m) | `src/TripleScreen3.cpp:808` (`LiquidityFragilityEngine.h`) |
| `mean_rev_z` | TS3 (15m) | `src/TripleScreen3.cpp:809` (`MeanReversionCalculator.h`) |
| `micro_asymmetry` | TS3 (15m) | `src/TripleScreen3.cpp:810` (`OrderFlowAsymmetryEngine.h`) |
| `lempel_ziv` | `ContextManager` directly (tick-level `InformationEngine`) | `ContextManager::BuildObservationVector()` |
| `tail_index` | `ContextManager` directly (tick-level `TailRiskEngine`) | `ContextManager::BuildObservationVector()` |
| `skewness_idx`, `fast_taleb_kurtosis`, `fast_hurst_exponent`, `recurrence_rate` | `ContextManager` directly, from `ActivityClockManager`'s imbalance-bar return buffer | `src/ContextManager.cpp:~430-475` (`RobustMoments.h`/`DfaHurstExponent.h`/`RecurrenceRateEngine.h`) |
| `fast_mean_rev_z` | **CONFIRMED 2026-09-08: NOT WIRED IN PRODUCTION** | `OBS_FAST_MEAN_REV_Z` (dim 17) is declared (`ContextManager.h:333`) but never assigned anywhere — grepped every `.cpp`/`.h` for the symbol, only the declaration and its `static_assert` exist. `BuildObservationVector()` leaves it at the array's zero-initialized default, always. `ActivityClockMeanReversion.h` is real, pure, and already reformulated to median/MAD (per `SCRATCHPAD.md`), but genuinely unused in the live path pending its own still-open wire-or-drop decision. **The generator must replicate this — leave dim 17 at 0.0, not compute a real value from the unwired header — or its output silently diverges from real production `.context` files for this one dim.** |

**Net finding**: every one of the 18 dims' real math already delegates to a pure header or a pure
tick-level engine class. The **only** non-pure part of the production path is (a) each Triple
Screen `.cpp`'s ACSIL array-gather (`sc.Close`/`sc.High`/`sc.Volume`/`sc.GetPersistentFloat`) and
(b) whatever raw-tick-to-bar/engine-feed plumbing currently lives inside the study that drives
`ContextManager` per tick (see below — corrected from an earlier draft of this section that
named the wrong study). Neither needs porting — this tool replaces both with its own tick source
(`mes_ticks.parquet`) and its own in-memory state (no `sc.GetPersistentFloat` carry-forward slots
needed; a plain local variable per dim suffices for a single-pass offline tool).

**Scope note — do not confuse with `ImbalanceContextManager`.** This entire spec is about the
calendar-clock `ContextManager`/`ObservationData` (18D) pipeline. A separate, already-built and
committed sibling exists for the activity-clock Imbalance Triple Screen migration —
`include/ImbalanceContextManager.h` (singleton, assembles the 4-dim `ImbalanceObservationData`
from `ImbalanceClockManager`'s TS1/TS2/TS3 return buffers), committed in `02f5b91`, designed in
`2026-09-06-imbalance-triple-screen-architecture-spec.md` §1.4. That component is out of scope
here — this tool does not need it, and nothing in §1-§3 above refers to it despite the similar
name.

**CORRECTED 2026-09-08 (operator correction) — the real `.context`-producing study is
`EventDataCollectorStudy.cpp`, NOT `SCStudies.cpp`.** `SCStudies.cpp` is the live-inference path,
and per its own comments (`EventDataCollectorStudy.cpp`'s Lock C note below) is **not yet deployed
— this system is not in production yet.** `EventDataCollectorStudy.cpp` is the Sierra-Chart-replay
study actually used today to produce training data. Its real per-tick order, traced this session:
`IndicatorManager::Instance().UpdateBarContext(sc)` → `ActivityClockManager::Instance().Update(sc)`
(feeds the imbalance-bar buffer that `skewness_idx`/`fast_taleb_kurtosis`/`fast_hurst_exponent`/
`recurrence_rate` read) → `ContextManager::Instance().UpdateMarketPhysics(logReturn)`, called ONLY
when `sc.Close[sc.Index]` differs from the last-seen price (feeds `TailRiskEngine`/
`InformationEngine`, i.e. `tail_index`/`lempel_ziv`) → `ContextManager::Instance()
.UpdatePriceStructure(sc, ...)` (feeds `StructureEngine`) → `ContextManager::Instance()
.CheckAndTriggerHMM(now_us, /*isDataCollection=*/true, syntheticVelocity, ...)`.
`TripleScreen1.cpp`/`TripleScreen2.cpp`/`TripleScreen3.cpp` remain separate `scsf_` studies on
their own chart timeframes, mutating `m_observationData` independently of this call sequence, per
this repo's own "all charts hot on every tick" rule (`CLAUDE.md`).

**Real, important distinction found while tracing this: `.context` and `.alpha` are gated
completely differently, and only one of them matters for this tool.** `.context` writing happens
entirely INSIDE `CheckAndTriggerHMM()` (gated only by that function's own TS1/TS2 freshness checks
+ the Mahalanobis significant-change trigger — see next paragraph). `.alpha` (the `TrainingEvent`
stream) is a SEPARATE write, later in `EventDataCollectorStudy.cpp`, gated behind its own 5-lock
sequence (Lock A: observation-saturation readiness; Lock B: indicator warm-up; Lock C: regime-
certainty Schmidt trigger — explicitly **telemetry-only during data collection**, per the source's
own comment: *"LockC is NOT enforced here because the data-collector must capture ALL market
regimes... will be enforced by the live-inference path (SCStudies.cpp) when deployed"*; Lock D/E:
TS1/TS2 freshness circuit-breakers). **This tool's mandate (§0) is `.context` only — none of Lock
A-E apply to it.** Don't over-build by replicating the `.alpha`-only lock sequence.

**Real gate the generator DOES need to replicate**: `CheckAndTriggerHMM()` hard-skips (returns
early, no emission at all) if `AreTs1DimsReady()`/`AreTs2StructuralDimsReady()` report stale/
missing TS1 or TS2 commits (each screen calls `MarkTs1MacroDimsFresh()`/
`MarkTs2StructuralDimsFresh()` on every successful write; `CheckAndTriggerHMM` checks `now_us`
against those timestamps with a max-staleness bound before proceeding). A generator assembling
dims from three independently-cadenced `TickBarAggregator` instances (240m/60m/15m) must reproduce
this same staleness gate, not just concatenate whatever values happen to exist at a given tick —
otherwise it would emit observations real production would have suppressed as stale.

### 2a. CRITICAL correction, found 2026-09-08 during Task 6 implementation: most "bar-close-cadence" dims are actually per-tick intra-bar-reactive in production — the offline generator was losing real intra-bar information

**RESOLVED 2026-09-08 (Task 6b, `docs/superpowers/plans/2026-09-08-market-data-replay-implementation.md`)**:
all 8 affected dims below now recompute every tick from `[closed-bar ring buffer] + [live bar]`,
mirroring TS3's pre-existing live-bar pattern. 66/66 native checks pass. The rest of this section
is kept as the historical record of the finding.

**Every dim below was previously (Tasks 3/4/4b, and Task 5's `mean_rev_z` specifically) implemented
in this tool as bar-GATED — computed once when a `TickBarAggregator` closes a bar, then held fixed
until the next close. Traced against the real production call sites this session: NONE of them are
actually bar-gated in production.** Each is called **unconditionally every tick** (no
`sc.Index != lastIndex`-style guard found anywhere in the surrounding code), and each internally
indexes its lookback window as `[sc.Index - length + 1 .. sc.Index]` — i.e. the window's most
recent point is `sc.Close[sc.Index]`/`sc.High[sc.Index]`/`sc.Low[sc.Index]`, **the current,
possibly still-forming bar**, not the last CLOSED bar. Confirmed by reading
`CalculateHurstExponent` (`src/StudyHelperFunctions.cpp:2393`) directly: `dataStartIndex =
sc.Index - length + 1`, `priceData[dataStartIndex + i]` up through `priceData[sc.Index]` — no
internal caching, genuinely recomputed from scratch on every call.

**Affected dims (all need to move from bar-gated to intra-bar/live-reactive):**
- `hurst_exponent`, `fisher_info`, `log_scale_ratio` (TS1) — `src/TripleScreen1.cpp:483/502/478`,
  called every tick, no gate.
- `relative_range`, `fractal_dim`, `log_scale_expansion_ratio`, and `bars_since_last_update`
  (regime tenure) (TS2) — `src/TripleScreen2.cpp:272-334` block, called every tick, no gate.
- `mean_rev_z` (TS3) — `src/TripleScreen3.cpp:801`, called every tick, no gate. **This corrects
  Task 5's own stated assumption** ("mean_rev_z stays bar-close cadence, `CalculateMeanReversion
  Speed`'s own real call site") — that assumption was never actually verified against the call
  site until now, and was wrong.

**NOT affected (already correctly intra-bar in this tool, or genuinely bar-gated in production
and correctly modeled as such):**
- `amihud_illiquidity`, `liq_fragility`, `micro_asymmetry` (TS3) — already fixed to be
  live-bar-reactive in Task 5, per `TripleScreen3.cpp`'s own 2026-08-29 live-reactivity change.
  This tool's existing "live bar" tracking pattern (`m_ts3LiveHigh`/`Low`/`Close`/`Volume`/
  `AskVolume`/`BidVolume`, updated every tick, read by `ComputeTs3LiveDims()`) is the **correct
  template to replicate** for TS1/TS2's affected dims above.
- `burstiness_index`, `tail_index`, `lempel_ziv` — already tick-level via `EventVelocityEngine`/
  `TailRiskEngine`/`InformationEngine`, unaffected.
- `skewness_idx`/`fast_taleb_kurtosis`/`fast_hurst_exponent`/`recurrence_rate` — already
  implemented intra-bar-reactive in Task 6 (recomputed every tick from `ImbalanceBarEngine`'s
  return buffer; only `recurrence_rate` caches at its own imbalance-bar-close cadence, which
  matches `ContextManager.cpp`'s real `completedBarCount`-gated caching exactly, not a fidelity gap).
- `fast_mean_rev_z` — stays at 0.0 always (dead field, unaffected, see §2's own table row).

**Fix approach**: for each affected TS1/TS2 dim, add live-forming-bar tracking (high/low/close
running for the current, not-yet-closed aggregator bar, exactly mirroring TS3's existing
`m_ts3Live*` members) and recompute the dim every tick from `[closed-bar window] + [live bar]`,
not only inside the `OnTsXBarClose()` callback. The existing bar-close callbacks still matter (they
advance the closed-bar ring buffers), but the actual `mutate_*()` call for each affected dim must
move to run every tick, reading the live bar as its own window's final point.

## 3. Proposed tool architecture (CANDIDATE — not yet built, not yet reviewed)

```
raw ticks (mes_ticks.parquet, one MES contract-window at a time)
    │
    ▼
TickBarAggregator × 3  (TS1=240m, TS2=60m, TS3=15m; session-anchored via EasternTimeOffset.h)
    │           │           │
    ▼           ▼           ▼
 TS1 dims    TS2 dims    TS3 dims      (each a direct call into the pure header/engine
(3 dims)    (4 dims)    (4 dims)        that screen owns today, per §2's table)
    │           │           │
    └─────┬─────┴─────┬─────┘
          ▼           ▼
   tick-level engines (TailRiskEngine, InformationEngine, ActivityClockManager's
   imbalance-bar buffer feeding skewness/kurtosis/fast_hurst/recurrence_rate)
          │
          ▼
   raw std::array<float,18>  (BuildObservationVector()'s output, replicated)
          │
          ▼
   FeatureScaler::UpdateAndNormalize()   → scaled std::array<float,18>
          │
          ▼
   ObservationTriggerGate::ComputeTriggerDecisionMetrics()  → significant_change?
          │
          ▼ (only if significant_change, matching production's own gate)
   MakeObservationData() + LBRFileManager::Instance().LogContext()
          │
          ▼
   genuine <symbol>.context file, byte-format-compatible with the existing
   tools/context_pipeline/context_reader.h
```

### 3a. Real per-tick order to replicate (RESOLVED 2026-09-08 — traced against `EventDataCollectorStudy.cpp`, operator directive: mimic it, don't invent a new shape)

The diagram above is the *data-flow* view; the diagram was never checked against the real
per-tick *control-flow* `EventDataCollectorStudy.cpp` actually executes. Traced this session,
file:line cited. The generator's main loop must reproduce this exact sequence, per real tick in
`mes_ticks.parquet`, not a parallel-branches shape:

0. **Market-closed gate** — skip Saturday / Sunday-pre-open / Friday-post-close ticks (DOW+HHMM
   check against session start/end). Trivial from the tick's own UTC `timestamp_us` +
   `EasternTimeOffset.h`.
1. **`now_us`** — the tick's own `timestamp_us` (replaces `GetReplaySafeNowUs(sc)`).
1b. **Synthetic velocity** — EDC derives `NumberOfTrades/SecondsPerBar` only because replay has no
   real tick cadence. The offline tool has real tick timestamps, so this step is replaced by a real
   per-tick event-rate measure — needs one more read of `CheckAndTriggerHMM`'s body to confirm
   whether the `.context` path actually consumes this parameter before finalizing.
2. **`IndicatorManager::UpdateBarContext(sc)`** — **not needed for `.context` alone, but do not
   delete the call site.** Traced this session (`src/IndicatorManager.cpp:649`): populates
   day-high/low anchors, warm-up status, trade-side dirty bits, regime tenure — none feed
   `ObservationData`'s 18 dims, so a `.context`-only run skips it. **Reversed 2026-09-08**: this
   call feeds `.alpha`'s Lock B warm-up gate (`IndicatorManager::IsWarmedUp()`), and the reusable
   core this tool is now scoped to build (§0) must retain the call site as an optional/
   feature-flagged step for that future consumer, not delete it outright.
2b. **`ActivityClockManager::Instance().Update(sc)`** — **needed, confirmed trivially pure this
   session.** Real body (`src/ActivityClockManager.cpp:17`): `m_engine.OnTickWithPrice(sc.Index,
   sc.AskVolume[Index], sc.BidVolume[Index], sc.Close[Index])` — three scalars per tick, a direct
   1:1 mapping to `mes_ticks.parquet`'s `ask_volume`/`bid_volume`/`trade_price` columns. Feeds the
   imbalance-bar buffer behind `skewness_idx`/`fast_taleb_kurtosis`/`fast_hurst_exponent`/
   `recurrence_rate`.
3. **`ContextManager::UpdateMarketPhysics(logReturn)`** — needed, fires only when the close price
   differs from the last-seen price (already noted in §1's table); trivial tick-to-tick
   `trade_price` comparison.
3b. **`ContextManager::UpdatePriceStructure(sc, high, low, close, isNewBar)`** — **DEFERRABLE given
   the 2026-09-08 priority pivot (§0): confirmed this step feeds only `AsymmetryContext`/
   `RiskGateContext`, never `ObservationData`.** Traced this session (`src/ContextManager.cpp:656`,
   re-confirmed `m_structureEngine`'s only 2 call sites are `Update()` and
   `GetFractalDimension()`→`paretoRot`): beyond `m_structureEngine.Update(...)`, it computes
   `elderChandelierATR` (`RiskGateContext`/`AsymmetryContext.taleb_cliff`) via a 22-bar
   `sc.High`/`sc.Low` lookback, branching on `PositionManager::Instance().GetOpenTrade()` (offline
   tool has none, always takes the "no position" branch). **Not required for the stated
   HMM-training/Feature-Saliency-EM goal** — skip this step entirely in a `.context`-only,
   placeholder-`AsymmetryContext`/`nullptr`-`RiskGateContext` build; only implement if a
   Transformer-training or execution-sim-parity consumer is later scoped.
4. **`ContextManager::CheckAndTriggerHMM(now_us, /*isDataCollection=*/true, syntheticVelocity,
   weekendGrace)`** — the actual `.context` writer. Internally: `BuildObservationVector()` → raw
   18D → `FeatureScaler::UpdateAndNormalize()` → scaled 18D → `ObservationTriggerGate` Mahalanobis
   gate → on significant change, `EmitTrainingContext()` → `LBRFileManager::LogContext()`
   (`src/ContextManager.cpp`, `EmitTrainingContext`). **Confirmed this session: `LogContext` always
   writes a `RiskGateContext` alongside `ObservationData` — it is not optional**, which hardens
   open question 3 below from "needs population" to "blocks first output, not deferrable."

**Resolves the pipeline-shape ambiguity**: single-pass over `mes_ticks.parquet`, per real tick, in
the literal order 0 → 1 → 1b → 2b → 3 → 3b → 4 (step 2 dropped, out of scope), checking
`LogContext` firing on step 4's gate — not the three-parallel-branches sketch the diagram above
suggests. The diagram remains a useful *data-flow* summary of where each dim's math lives; §3a is
the authoritative *control-flow* order.

**Open design questions (updated 2026-09-08 — items 1/4/5 resolved this session):**

1. ~~Contract-window handling.~~ — **RESOLVED 2026-09-08 (operator directive: be as faithful as
   possible to a Sierra Chart replay, or better, live trading).** Reuse
   `tools/observation_vector/market_data_io.h`'s existing `StreamTicksFullParquet()` unchanged — it
   already streams the ENTIRE tick history across ALL contract windows in one continuous
   chronological pass, emitting `isNewContract` exactly at each roll boundary. This is not a
   simplification, it's the faithful reconstruction: `tools/scid_processing/
   scid_contract_windows.h`'s per-contract windows are already derived byte-for-byte from Sierra
   Chart's OWN configured rollover rule (Method3, 4 days before expiry, ported from
   `mes_continuous.py`) — concatenating them in chronological order, with the natural unadjusted
   price gap at each roll, reproduces exactly what a live/replay SC chart following the front-month
   contract itself shows (SC does not back-adjust a live continuous contract either). **One
   invocation, one continuous pass across every contract — not one contract-window per invocation.**
   **Real design divergence from existing prior art, flagged explicitly**: `whole_vector_
   redundancy_eval.cpp` resets all rolling state (`ts1Bucket`/`ts2Bucket`/ring buffers/etc.) on
   `isNewContract` — correct for ITS goal (per-contract-independent redundancy statistics), but
   **wrong for this tool**: a real SC live/continuous chart never resets its indicators at a
   rollover, it just experiences a single-tick price gap flowing through the same continuous
   calculations. This generator should NOT reset `TickBarAggregator`/rolling-window state at
   `isNewContract` — only reset what a real SC session itself would reset (e.g. per-session daily
   anchors at the next session boundary, unrelated to the contract roll). Verify this concretely in
   the vertical slice (Next steps #6).
2. ~~Warm-up handling.~~ — **RESOLVED 2026-09-08 (operator decision): wait until every dim is
   warmed up before starting the `.context` stream at all — do not emit production's neutral
   placeholder values during warm-up.** Simpler and cleaner for an HMM-training-focused first
   version; downstream consumers never have to special-case placeholder-value rows.
   **Recorded for later, not actioned now**: this system is not in production yet (`CLAUDE.md`'s
   standing pre-production rule), so `EventDataCollectorStudy.cpp`'s own warm-up/Lock A-E gating
   may itself be worth revisiting for consistency with this decision at some future point — flagged
   here as a follow-up thread, out of scope for this tool.
3. ~~`RiskGateContext` population~~ — **DEFERRED 2026-09-08, no longer blocking.** §0's priority
   pivot established `RiskGateContext` can be passed as `nullptr` entirely (execution-sim-parity
   concern, not an HMM training input; `context_to_parquet.cpp` already handles `nullptr`
   gracefully). Combined with item 8's resolution (`regimeDuration` was the one piece that looked
   hard), there is now no remaining reason to populate this struct for the current goal — revisit
   only alongside a real execution-sim-parity consumer.
4. ~~Multi-threading / performance budget.~~ — **RESOLVED 2026-09-08 (operator decision):
   single-pass sequential**, matching item 1's "one continuous pass" fidelity requirement anyway
   (a spliced continuous-contract stream is inherently sequential, not independently
   parallelizable per-contract the way `scid_to_ticks_parquet.cpp`'s own decode step is). **DOD
   practices this repo already mandates for exactly this kind of tool** (`/memories/repo/
   cpp_tools_conventions.md`; `whole_vector_redundancy_eval.cpp`'s own header comment on the
   2026-09-03 OOM incident) — the single-pass design must still follow these, not just "be
   sequential":
   - **No heap allocation in the per-tick hot loop** — fixed-size `RingBuffer<T,N>`/`std::array`
     state only, matching every existing `tools/observation_vector/*_eval.cpp`.
   - **Bounded memory regardless of tick volume** — never retain a per-tick `std::vector` across
     the full 471.9M-tick stream (the exact OOM trap `whole_vector_redundancy_eval.cpp`'s own
     header comment documents avoiding).
   - **Reuse the real production classes directly, not reimplementations** — `ContextManager`,
     `FeatureScaler`, `ObservationTriggerGate`, `LBRFileManager`, `TickBarAggregator` (§1's
     "single source of truth" theme is itself a DOD discipline: one real data model, not a
     parallel one).
   - **`ToolProgressLogger::Log()`** for all output (never bare `printf`), with
     `CheckMemoryBudget()`/`--max-rss-mb` as the defense-in-depth safety net.
   - Native `check()`/`g_failures`/`ALL PASS` test harness; standalone bare-`g++` build, never
     added to `CMakeLists.txt`/`build_dll.sh`.
5. **Validation methodology — RESOLVED 2026-09-08: FlatBuffers' own struct contract makes
   bit-for-bit comparison cheap, not "strongest but most expensive" as originally framed.**
   The *write-decision criterion* was already settled (real `ObservationTriggerGate`, §3a step 4,
   no separate validation needed since it's identical code). For the residual question — are the
   raw `ObservationData` *values* correct — `ObservationData`/`AsymmetryContext` are FlatBuffers
   **structs**, not tables: `include/generated/mts_schema_contract_generated.h` already
   `static_assert`s `std::is_standard_layout<ObservationData>::value` and
   `sizeof(ObservationData) == kObservationDim * sizeof(float)` — fixed-layout, no vtable, no
   optional fields, no schema-evolution ambiguity. `tools/context_pipeline/context_reader.h`'s
   `mo->observation()` already hands back a raw `const ObservationData*` with zero further parsing.
   **This means direct per-field (or `memcmp`) comparison between a real Sierra-Chart-produced
   `.context` file's records and this generator's own output, matched on `timestamp_us`/
   `sequence_id`, is both valid and nearly free** — no bespoke comparison harness needed, reuse
   `context_reader.h` for both sides. **One caveat, not to be silently dropped**: literal `memcmp`
   can still be defeated by legitimate cross-compiler/cross-platform float ULP differences (this
   generator builds via `g++`/mamba on Linux; the production DLL builds via `clang-cl` cross-compiled
   for Windows) — so "bit-for-bit" here should mean exact struct layout + a tight per-field epsilon
   (e.g. relative 1e-5), not literal byte equality, to stay robust against transcendental-function
   ULP noise while still being a genuine strong check.
6. ~~`elderChandelierATR`'s inputs~~ — **DEFERRED 2026-09-08, see §3a step 3b**: this dim feeds only
   `AsymmetryContext`/`RiskGateContext`, neither required for the current HMM-training goal.
   Revisit only alongside a Transformer-training or execution-sim-parity consumer.
7. ~~`AsymmetryContext`'s `anchors` source~~ — **RESOLVED 2026-09-08, see §1b.** Full 8-field trace
   done; only `taleb_kurtosis`/`taleb_skewness`'s `CalculateRealizedKurtosis()`/`CalculateSkewness()`
   (`StudyHelperFunctions.cpp:2679-2691`) need a quick purity/window confirmation before
   implementation — not a design question, a verification task. §1b also surfaced a THIRD
   independent fractal-dimension reading (`StructureEngine::GetFractalDimension()`, feeding
   `pareto_rot`) alongside the two already known (§1a) — keep all three separate.
8. ~~`regimeDuration` genuinely cannot be reconstructed offline~~ — **RESOLVED 2026-09-08, see
   §1b.** Re-traced to the correct function name (`MarketClimateIndicator::UpdateContext()`, not
   `Update()`) and found production already has a real, named "no HMM yet" sentinel
   (`HMM_NO_PRIOR`, `include/rc_enums.h:59`) that degrades the classifier to a pure-physics branch —
   exactly this tool's situation, not a gap needing an invented workaround. No operator decision
   needed after all.

### 3b. Critical architecture correction, found this session: `ContextManager`/`ActivityClockManager` are NOT includable outside the SC DLL build

Every earlier section describing calling `ContextManager::Instance()`/`ActivityClockManager::
Instance()` "directly" was **wrong about the integration mechanism, even where the underlying
finding (a call site's inputs are simple/pure) stayed correct.** Checked this session:
`include/ContextManager.h:3` and `include/ActivityClockManager.h:3` both `#include "sierrachart.h"`
— Sierra Chart's own ACSIL SDK header — directly. Neither class can be included, let alone
instantiated, in this tool's standalone `bare g++`/mamba build (no ACSIL toolchain there). **This
also explains, retroactively, why every existing prior-art tool in `tools/observation_vector/`
(`whole_vector_redundancy_eval.cpp`, `observation_vector_recalibration.cpp`) always ports each
dim's own leaf-header math directly and never calls `ContextManager`/`ActivityClockManager`** — not
a stylistic preference, a hard technical necessity this spec hadn't stated plainly until now.

**Good news: this changes nothing for the current (`ObservationData`-only) goal, because every
leaf dependency actually needed is independently confirmed ACSIL-free** (checked this session,
`#include` lists read directly, no assumptions):

| Dependency | ACSIL-free? | Replaces |
|---|---|---|
| `include/TickBarAggregator.h` | Yes (§1) | TS1/TS2/TS3 bar construction |
| `include/EasternTimeOffset.h` | Yes (§1) | Session-anchoring for the above |
| `include/ImbalanceBarEngine.h` | **Confirmed this session** (`RingBuffer.h`+STL only) | `ActivityClockManager`'s wrapped engine — call `OnTickWithPrice(index, askVolume, bidVolume, price)` directly on a tool-owned instance, bypassing `ActivityClockManager` entirely |
| `include/TailRiskEngine.h`, `include/InformationEngine.h` | Yes (§1) | `tail_index`/`lempel_ziv` tick-level engines |
| `include/BipowerVariation.h`, `CarryForwardCalculators.h`, `SevcikFractalDimension.h`, `DfaHurstExponent.h`, `OrderFlowAsymmetryEngine.h`, `EventVelocityEngine.h`, `RobustMoments.h`, `RecurrenceRateEngine.h`, `MeanReversionCalculator.h`, `LiquidityFragilityEngine.h` | Yes (§1/§2) | Each TS1/TS2/TS3-owned dim's real formula |
| `include/FeatureScaler.h` | Yes (§1) | Raw→scaled transform |
| `include/ObservationTriggerGate.h` | Yes (§1) | Mahalanobis significant-change gate |
| `include/generated/mts_schema_contract_generated.h` (`MakeObservationData`) | Yes (§1) | Raw array → wire struct |
| `include/LBRFileManager.h` | **Confirmed this session** (`<fstream>`/`<mutex>`/generated schema header only) | `.context` writer, real singleton |
| `include/StructureEngine.h` | **Confirmed this session** (`Eigen`+`RingBuffer.h`+STL only) — moot for now, deferred with `AsymmetryContext`/`RiskGateContext` (§3a step 3b) | N/A for the current goal |

**Net implementation shape**: `MarketDataReplayEngine.h` (§3c) owns its own local instances of
`ImbalanceBarEngine`, `TailRiskEngine`, `InformationEngine`, `FeatureScaler`, and
`ObservationTriggerGate` — replicating `ContextManager`'s orchestration logic (already fully traced
in §3a/§2), never linking against `ContextManager`/`ActivityClockManager` themselves. This also
retroactively corrects §0's reusability-mandate wording ("`ActivityClockManager`/`ContextManager`
feed... should live in one core loop") — the *logic* those calls represent lives in the core loop;
the *classes* themselves are not callable and must not be included.

### 3c. Concrete file/class breakdown

Under `tools/market_data_replay/` (confirmed name, §0), matching this repo's established
pure-header-plus-thin-CLI-driver pattern (`tools/observation_vector/FeatureSaliencyEM.h`'s own
precedent, cited in its own spec §3: keep the numerical/orchestration core separate from I/O so it
stays independently unit-testable):

- **`MarketDataReplayEngine.h`** — the reusable per-tick pipeline core (§0's reusability
  mandate). PascalCase, matching this repo's `include/`-style logic-file convention
  (`TickBarAggregator.h`, `ImbalanceBarEngine.h`) rather than `tools/`'s usual snake_case, since
  this header is conceptually a pure, reusable production-adjacent component, not a one-off script.
  No parquet/Arrow dependency, no CLI parsing — takes raw scalars only, so it's testable
  on synthetic tick sequences without touching real data. Class `MarketDataReplayEngine`:
  - Owns 3× `tba::TickBarAggregator` (TS1=240m, TS2=60m, TS3=15m, `CME_ES_SESSION_START_SECS`),
    each constructed with a bar-close callback that computes that screen's owned dims (§2's table)
    via the pure per-dim headers and writes them via `mutate_*()` on a **tool-owned**
    `MTS::Schema::ObservationData` instance (mirrors `ContextManager::GetMutableObservation()`'s
    real access pattern, `include/ContextManager.h:137`/`src/TripleScreen1.cpp:628` — same
    mutation calls, tool-owned struct instead of the ACSIL-coupled singleton's).
  - Owns one `ImbalanceBarEngine`, `TailRiskEngine`, `InformationEngine` instance (tick-level, feeds
    `skewness_idx`/`fast_taleb_kurtosis`/`fast_hurst_exponent`/`recurrence_rate`/`tail_index`/
    `lempel_ziv` per §2's table) plus one `FeatureScaler` and one `ObservationTriggerGate` instance
    (replicating `CheckAndTriggerHMM()`'s own internal raw→scale→gate sequence, §3a step 4).
  - `bool OnTick(int64_t timestamp_us, double price, int64_t volume, int64_t askVolume, int64_t
    bidVolume)` — the single entry point, implementing §3a's steps 0/1/2b/3/4 literally (step 2
    skipped, step 3b deferred per §0): feeds all 3 aggregators + the tick-level engines, calls
    `UpdateMarketPhysics`-equivalent on genuine price change, computes/scales the raw 18D array,
    runs the trigger gate, and returns whether this tick's observation is significant (caller then
    writes it out). Explicitly does NOT call `LBRFileManager` itself — keeps the numerical core
    free of file I/O, matching the `FeatureSaliencyEM.h` precedent above.
  - Constructed with `AsymmetryContext` defaulted (`MTS::Schema::AsymmetryContext{}`) and no
    `RiskGateContext` member at all, per §0's priority pivot — not a stub to fill in later, a
    deliberate omission for this version.
- **`MarketDataReplay.cpp`** — thin CLI driver (§3d): parses flags, opens
  `LBRFileManager::Instance().Open(...)`, streams ticks via `market_data_io.h`'s
  `StreamTicksFullParquet()` (reused unchanged, §3a open Q1 resolution — one continuous pass, no
  per-contract reset), calls `engine.OnTick(...)` per tick, and on a significant-change return,
  calls `LBRFileManager::Instance().LogContext(obs, MTS::Schema::AsymmetryContext{}, timestamp_us,
  bars_since_last_update, /*risk_gate_context=*/nullptr)`. All progress/diagnostics through
  `ToolProgressLogger::Log()` (repo convention, `/memories/repo/cpp_tools_conventions.md`).
- **`test_market_data_replay_engine.cpp`** — native tests (§3f) against the engine header only, no
  parquet/CLI dependency, following this repo's `check()`/`g_failures`/`ALL PASS` convention.
  **Kept snake_case with the `test_` prefix despite the header above going PascalCase** — every
  existing test file in this repo (`tests/cpp/test_*.cpp`, every `tools/*/test_*.cpp`) uses this
  exact naming pattern with no exceptions; deviating here would break a more consistent, more
  load-bearing convention than the one this rename is fixing. Flagging this choice explicitly
  rather than silently applying PascalCase everywhere — correct if this wasn't the intent.

### 3d. CLI surface (proposed, matching `scid_to_ticks_parquet`'s own documented surface shape)

```
tools/bin/market_data_replay \
  --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet \
  --output <symbol>.context \
  [--max-rss-mb 3072]
```

- `--ticks-parquet`: required, the full multi-contract tick stream (§3a open Q1 — always the whole
  file, one continuous pass, never a single-contract slice).
- `--output`: required, the `.context` file path `LBRFileManager::Open()` writes to. `.alpha` output
  is intentionally never opened by this tool's `.context`-only CLI invocation (§0's reusability
  mandate — a future `.alpha`-inclusive invocation reuses the same engine, not this same flag set).
- `--max-rss-mb`: optional, `ToolProgressLogger::CheckMemoryBudget()`'s defense-in-depth convention
  (§3a open Q4), default matching sibling tools (e.g. 3072).
- Deliberately NOT included: a `--symbol`/`--contract` selector (§3a open Q1's resolution means
  there is nothing to select — always all contracts, one pass) or a warm-up-emission flag (§3a
  open Q2's resolution is unconditional: wait for full warm-up, always).

### 3e. Vertical slice — first concrete implementation target

Per this repo's "smallest real end-to-end slice" discipline (§4 Next steps #6): implement, in order,
just enough to prove the pipeline shape before wiring all 18 dims:
1. `TickBarAggregator` × 3, wired to `mes_ticks.parquet`, with **one dim per screen** —
   `log_scale_ratio` (TS1), `burstiness_index` (TS2, tick-level via `EventVelocityEngine.h`, not
   actually bar-gated — good first pick since it needs no bar-close callback at all), `mean_rev_z`
   (TS3) — plus `tail_index`/`lempel_ziv` (tick-level, no bar dependency either).
2. Wire `FeatureScaler` + `ObservationTriggerGate` against this partial vector (remaining 13 dims
   held at a fixed neutral placeholder inside the slice only, never in the real tool) to prove the
   scale→gate→significant-change sequence fires at a sane rate against real data.
3. Call `LBRFileManager::LogContext()` for real, open the resulting `.context` file with the
   existing `tools/context_pipeline/context_reader.h`, and eyeball the first N records.
4. Only then wire the remaining 14 dims (§2's table already has every formula/window traced) and
   remove the placeholder values.

### 3f. Error / edge-case handling (not yet in any prior section)

- **Non-monotonic/duplicate timestamps**: `scid_to_ticks_parquet.cpp`'s own spec documents a real,
  bounded benign-jitter case (`_MAX_BENIGN_TIMESTAMP_JITTER_US`, already resequenced at the parquet-
  build stage) — this tool reads already-resequenced output, so it should treat any remaining
  backwards jump as a hard error (data corruption), not silently reorder again.
- **Zero/negative price or volume**: skip the tick (mirrors `whole_vector_redundancy_eval.cpp`'s own
  `if (price <= 0.0) return;` guard) rather than let it corrupt a rolling window.
- **`TickBarAggregator::Flush()` at end-of-stream**: must be called once per aggregator after the
  last tick, or the final in-progress bar across all 3 timeframes is silently dropped (already
  covered by the class's own native test, `tests/cpp/test_tick_bar_aggregator.cpp`, but the CLI
  driver must remember to call it — easy to forget since it's not part of the per-tick loop).
- **Degenerate/NaN dim values**: every leaf header already has its own documented NaN-on-degenerate-
  window contract (carry-forward semantics, §2's table) — the engine must apply the SAME
  carry-forward convention production does (last valid value), not propagate NaN into
  `FeatureScaler`/`ObservationTriggerGate`, which is a real correctness bug class this repo has hit
  before (§1a's `LOGZ_WINSOR_SIGMA_OVERRIDE` misalignment bug, caught by native tests, not
  inspection).

### 3g. Testing plan

Following this repo's native `check()`/`g_failures`/`ALL PASS` convention (no GoogleTest/CMake),
mirroring `tests/cpp/test_tick_bar_aggregator.cpp`'s own style:
- `test_market_data_replay_engine.cpp`: feed a small, hand-constructed synthetic tick sequence
  (spanning a session boundary and at least one contract roll, per §3a open Q1) through
  `MarketDataReplayEngine::OnTick()` and assert specific dim values at specific ticks — the same
  characterization-test style `test_tick_bar_aggregator.cpp` already uses for bar boundaries.
- A small real-data smoke test (not a native `check()` test, a manual/documented run): process the
  first N contract-days of `mes_ticks.parquet`, confirm `.context` opens cleanly via
  `context_reader.h`, and confirm the significant-change rate is in a sane range (cross-check
  against `EventDataCollectorStudy.cpp`'s own historical run logs if available).
- Per §3's open question 5 resolution: a real validation pass comparing this tool's output against
  a genuine Sierra-Chart-produced `.context` file over the same historical window, matched on
  `timestamp_us`, per-field float comparison within a tight epsilon (not literal `memcmp`, per the
  cross-compiler ULP caveat already recorded there).

### 3h. Output files & the full `.context` → `.context.parquet` pipeline (found this session)

**`LBRFileManager::Open(path, symbol)` unconditionally creates THREE files, not one** —
`src/LBRFileManager.cpp:29`: `<path>.alpha`, `<path>.context`, AND `<path>.imbalance.context`, every
call, no flag to open `.context` only. Since this tool (§0) never calls `LogAlpha()`/
`LogImbalanceContext()` by design, the `.alpha` and `.imbalance.context` files it produces will
contain only the magic header and zero records — **expected, not a bug**, and worth a one-line log
message on close so it isn't mistaken for a broken run. `--output` (§3d) is therefore a **base path,
no extension** — `Open()` appends the three suffixes itself.

**This tool's mandate stops at `.context`** (§0) — it does not write `.context.parquet` itself.
That conversion is already solved: run the existing, unchanged `tools/context_pipeline/
context_to_parquet.cpp` against this tool's `<path>.context` output to get `<path>.context.parquet`
— the actual Feature Saliency EM input (per the operator's own stated goal, this thread's opening
context). Full pipeline, end to end:

```
mes_ticks.parquet
    │  (this tool, MarketDataReplay.cpp)
    ▼
<path>.context   (+ near-empty <path>.alpha, <path>.imbalance.context — expected)
    │  (existing tool, unchanged: tools/context_pipeline/context_to_parquet.cpp)
    ▼
<path>.context.parquet   ← Feature Saliency EM's real input
```

### 3i. Build command (proposed, matching `tools/observation_vector/*.cpp`'s own documented recipe)

```
mamba run -n mts g++ -O2 -std=c++17 -Iinclude \
  $(mamba run -n mts pkg-config --cflags arrow parquet) \
  tools/market_data_replay/MarketDataReplay.cpp \
  $(mamba run -n mts pkg-config --libs arrow parquet) \
  -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
  -o tools/bin/market_data_replay
```

`-Iinclude` is required (not present in some sibling tools' recipes) because `MarketDataReplayEngine.h`
`#include`s production headers directly (`TickBarAggregator.h`, `FeatureScaler.h`, etc.) rather than
tool-local copies — matches `whole_vector_redundancy_eval.cpp`'s own build recipe, which needs the
same flag for the same reason. `FeatureScaler.h`'s `<nlohmann/json.hpp>` dependency needs no extra
flag — already resolved for every other `tools/observation_vector/*.cpp` that includes it, so the
mamba env already provides it.

## 4. Next steps (cleaned up 2026-09-08 — superseded history moved out, this is now a current, actionable implementation checklist)

Every earlier iteration of this list has been resolved, deferred, or absorbed into §1a/§1b/§3a-§3i
(full history preserved there, not repeated here). What's actually left before real use:

1. Create `tools/market_data_replay/` (§0) and implement `MarketDataReplayEngine.h` per §3c's
   breakdown — steps 0/1/2b/3/4 of §3a literally, step 2/3b intentionally omitted per §0's priority
   pivot.
2. Build the vertical slice first (§3e) — prove the pipeline shape on a handful of dims before
   wiring all 18.
3. Write `test_market_data_replay_engine.cpp` alongside the slice (§3g), not after full wiring.
4. Wire the remaining 14 dims per §2's ownership table once the slice is verified.
5. Run the real validation pass (§3g/§3's open question 5): compare against a genuine
   Sierra-Chart-produced `.context` file, per-field, tight epsilon.
6. Convert output via the existing `tools/context_pipeline/context_to_parquet.cpp` (§3h) to get
   `.context.parquet`, then hand off to Feature Saliency EM (the stated goal, §0).
7. Verify Sierra-Chart cross-chart write-ordering (`sc.CalculationPrecedence`) for the
   calendar-clock TS1/TS2/TS3 screens, matching the already-documented Imbalance-side precedent
   (`2026-09-06-imbalance-triple-screen-architecture-spec.md` §1.2b) — the one item from the
   original list not yet touched by this session's work; still needs a look before trusting
   cross-screen write ordering in the replicated logic.

## 5. Cross-references

- `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` — originating
  "Side thread" section; the Phase-1 audit this tool's feasibility rests on.
- `docs/superpowers/specs/2026-09-06-imbalance-triple-screen-architecture-spec.md` — sibling
  activity-clock migration; NOT the same tool, but shares `ActivityClockManager`/imbalance-bar
  infrastructure this tool's `skewness_idx`/`fast_taleb_kurtosis`/`fast_hurst_exponent`/
  `recurrence_rate` dims depend on.
- `tools/context_pipeline/context_reader.h` — the existing reader this tool's output must stay
  byte-compatible with; ground truth for the wire format is `src/LBRFileManager.cpp` (the writer).
- `/home/rcruz/devel/VSCode/schema/mts_schema.fbs`'s `WIRE_SCHEMA_VERSION` marker — any schema
  drift between when this tool is built and when it's run must be caught, not silently tolerated
  (see `OpenContextFile()`'s existing hard-refusal-on-mismatch precedent).
