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

**Folder name**: operator recalls settling on something like `tools/training_data_preparation` —
not independently recoverable (no trace of this decision in any spec, plan, or committed code was
found). Proposing `tools/training_data_preparation/` to match that recollection and this repo's
existing function-based subfolder convention (`tools/observation_vector/`, `tools/context_pipeline/`,
`tools/scid_processing/`) — confirm or correct before any file lands there.

## 1. Already-built, already-verified pure components (no ACSIL dependency)

All confirmed by direct inspection this session (file:line cited), not assumed:

| Component | File | Role | Purity evidence |
|---|---|---|---|
| Tick→bar aggregation | `include/TickBarAggregator.h` | Reconstructs TS1(240m)/TS2(60m)/TS3(15m) session-anchored bars from raw UTC tick timestamps, replicating Sierra Chart's `IBPT_DAYS_MINS_SECS` boundary convention | Header-only, `#include`s only `EasternTimeOffset.h` + STL. Native test `tests/cpp/test_tick_bar_aggregator.cpp`, 17/17 pass |
| DST-aware ET conversion | `include/EasternTimeOffset.h` | `GetEasternUtcOffsetSeconds()`, needed because ticks are UTC epoch-us but CME session start (18:00 ET) is defined in Eastern Time | Header-only, Howard Hinnant civil-calendar algorithm, no external deps. Native test `tests/cpp/test_eastern_time_offset.cpp`, 10/10 pass |
| Mahalanobis significant-change gate | `include/ObservationTriggerGate.h` | The exact gate `ContextManager::CheckAndTriggerHMM()` uses to decide whether an observation is novel enough to emit — energy/geometry split-channel robust z-score, velocity-adaptive epsilon/noise floor, sanitization clamps | Header-only, `ContextManager.h` now composes an instance of this class (`m_triggerGate`) rather than duplicating the logic — single source of truth, not a parallel reimplementation. Native test `tests/cpp/test_observation_trigger_gate.cpp`, 25/25 pass |
| `ObservationData`/`AsymmetryContext` conversion | `include/generated/mts_schema_contract_generated.h` (`MakeObservationData`/`ToObservationArray`) | Converts the pipeline's raw `std::array<float,18>` into the wire struct | `memcpy`-based, standard-layout-asserted. Native test `tests/cpp/test_mts_schema_contract.cpp`, 7/7 pass |
| `.context` binary writer | `include/LBRFileManager.h` / `src/LBRFileManager.cpp` | `Open()`/`LogContext()` — writes the exact size-prefixed `LBRN`-magic, `MarketObservation`+`SystemState` paired-record format the existing reader (`tools/context_pipeline/context_reader.h`) already consumes | **Confirmed pure this session** — `LBRFileManager.h` includes only `<fstream>`/`<mutex>`/the generated schema header; no `SCStudyInterfaceRef`/`sc.*` anywhere. **This tool does not need to reimplement the writer — it can call the real singleton directly.** |
| Per-dim math (17 of 18 dims), extracted earlier | `BipowerVariation.h`, `CarryForwardCalculators.h`, `SevcikFractalDimension.h`, `DfaHurstExponent.h`, `OrderFlowAsymmetryEngine.h`, `EventVelocityEngine.h`, `RobustMoments.h`, `RecurrenceRateEngine.h`, `MeanReversionCalculator.h`, `LiquidityFragilityEngine.h` | Each dim's real production formula | Per `2026-08-31-elite-feature-set-curation-initiative.md`'s own Phase-1 audit — confirmed pure, natively tested individually |
| Tick-level engines | `include/TailRiskEngine.h` (Hill estimator → `tail_index`), `include/InformationEngine.h` (LZ76 → `lempel_ziv`) | Rolling, event-indexed (not bar-indexed) computations, members of `ContextManager` itself | **Confirmed pure this session** — zero `sc.*`/ACSIL references in either header |
| `FeatureScaler::UpdateAndNormalize` | `include/FeatureScaler.h` | Hybrid Soft-Log-Z (state dims) / Log-Z (energy dims) scaling — the exact transform between raw and wire-format observations | Confirmed pure in the 2026-09-07 audit (`2026-08-31-elite-feature-set-curation-initiative.md`) |
| Raw historical tick source | `lbrnet/data/raw/mes_ticks.parquet`, written by `tools/scid_processing/scid_to_ticks_parquet.cpp` | 471.9M real MES ticks, columnar (`timestamp_us`, `trade_price`, `ask_price`, `bid_price`, `spread`, `volume`, `bid_volume`, `ask_volume`, `trade_side`, `contract`) | Already the standard input for every other offline validation tool in this repo (`tools/observation_vector/*_eval.cpp`) |

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

**Open design questions (none resolved yet):**

1. **Contract-window handling.** `mes_ticks.parquet`'s `contract` column carries multiple MES
   contract-month windows (per `scid_to_ticks_parquet.cpp`'s own multi-contract decode). Does this
   tool process one contract-window per invocation (simplest, matches
   `scid_to_ticks_parquet.cpp`'s own per-contract parallelism), or splice a continuous front-month
   series across rolls? Splicing introduces a real price-gap-at-roll question this doc does not
   yet answer.
2. **Warm-up handling.** Production's own dims have real warm-up floors (e.g. `tail_index` needs
   ≥50 samples in `TailRiskEngine`, `ObservationTriggerGate` needs ≥40 rolling samples before it
   can fire). Does the offline tool emit anything during warm-up (as production does, with neutral
   placeholder values), or start the `.context` stream only once every dim is warmed up? Affects
   whether downstream consumers must replicate production's warm-up-value conventions.
3. **`RiskGateContext` population.** `ContextManager::BuildRiskGateContext()` reads several
   `LocalRiskContext` fields (e.g. `amihudPercentile`, sourced from a 21-trading-day rolling pool —
   `ContextManager.h`'s `kAmihudRthWindow`/`kAmihudOvernightWindow`) that this tool would need to
   independently reconstruct with matching session-pool semantics (RTH vs. overnight), not yet
   designed.
4. **Multi-threading / performance budget.** `scid_to_ticks_parquet.cpp` decodes contracts in
   parallel; should this tool similarly parallelize across contract-windows, or is a single-pass
   sequential read (simpler, correctness-first) acceptable for a first version given the tick
   volumes involved (hundreds of millions)?
5. **Validation methodology.** How is correctness actually established — bit-for-bit comparison
   against a real Sierra-Chart-produced `.context` file over the same historical window (strongest,
   most expensive), or a weaker statistical-parity check (distribution comparison per dim)? Given
   this tool's entire reason for existing is trustworthy training data, this is not a minor
   afterthought — needs an explicit answer before the tool is trusted for real training runs.

## 4. Next steps (not started)

1. Confirm the `tools/` subfolder name (§0) with the operator before creating any file there.
2. ~~Trace `SCStudies.cpp`'s real per-tick dispatch order end-to-end~~ — **CORRECTED AND RESOLVED
   2026-09-08 (operator correction): the real study is `EventDataCollectorStudy.cpp`, not
   `SCStudies.cpp`** (the latter is the live-inference path, itself not yet deployed — this system
   is not in production yet). Full per-tick trace + the `.context`-vs-`.alpha` gating distinction
   now recorded in §2.
3. Independently re-confirm `fast_mean_rev_z`'s source header/purity — **RESOLVED 2026-09-08: NOT
   wired in production at all** (§2).
4. Resolve §3's 5 open design questions — at minimum, pick a default answer for each and record
   the rationale here, even if provisional.
5. Design `RiskGateContext` reconstruction (§3 item 3) — likely needs its own short sub-spec given
   the session-pool state involved.
6. Build a minimal end-to-end vertical slice first (one contract-window, one dim per screen,
   verify the pipeline shape) before wiring all 18 dims — matches this repo's own established
   "smallest real end-to-end slice" discipline (e.g. `ImbalanceScreen1.cpp`'s own scoping, see
   `2026-09-06-imbalance-triple-screen-architecture-spec.md` §1.1a).
7. Decide the validation methodology (§3 item 5) before generating any training data intended for
   real use.
8. Verify Sierra-Chart cross-chart write-ordering (`sc.CalculationPrecedence`) for the
   calendar-clock TS1/TS2/TS3 screens, matching the already-documented Imbalance-side precedent
   (`2026-09-06-imbalance-triple-screen-architecture-spec.md` §1.2b) — not yet done for this side.

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
