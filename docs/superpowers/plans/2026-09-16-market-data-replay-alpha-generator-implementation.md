# `.alpha` (TrainingEvent) Generator — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Extend `tools/market_data_replay/` with a new, separately-named CLI
(`MarketDataReplayContext.cpp`) that emits real, byte-compatible `.context`+`.alpha` file pairs
from raw historical ticks — no Sierra Chart replay needed — scoped to `PRIMARY_TRIGGER_MASK`'s 17
pattern-detector indicators as a first slice (per the approved scope decision).

**Spec:** `docs/superpowers/specs/2026-09-16-market-data-replay-alpha-generator-spec.md` — every
task below cites the spec section it implements. **Verification plan (closed, both decisions
approved):** `docs/superpowers/plans/2026-09-16-market-data-replay-alpha-generator.md`.

**Approved scope (spec §7, operator sign-off 2026-09-16):**
1. `PRIMARY_TRIGGER_MASK` (17 `IndicatorKey`s) only — `SECONDARY_TRIGGER_MASK`'s ~38 remaining
   keys explicitly deferred to a separately-justified follow-on, not part of this plan.
2. A new, separately-named CLI entry point (`MarketDataReplayContext.cpp`), reusing
   `MarketDataReplayEngine.h`'s existing tick/bar core unmodified — `MarketDataReplay.cpp` (the
   dim-selection Parquet exporter) is not touched by any task below.

**Architecture:** Vertical-slice-first, matching this tool family's own established discipline
(the original `.context` generator's plan, Tasks 1-11). Each task is independently buildable and
natively testable before the next depends on it. Tasks 1-4 build shared TA primitives (used by
multiple pattern detectors, built once); Task 5 adds bounded bar history; Task 6 extracts `NR7`;
Task 7 assembles the 17 detectors; Tasks 8-10 build the `.alpha`-specific write path; Task 11 is
the new CLI driver; Task 12 is empirical validation (partially blocked, see its own note); Task 13
fixes the stale `tools/README.md` entry.

## Institutional DOD Mandate — inherited from the `.context` generator plan, applies identically here

Same non-negotiable rules as `docs/superpowers/plans/2026-09-08-market-data-replay-implementation.md`
(this tool once took down a session with a real OOM incident from violating exactly these):

1. **Zero heap allocation in the per-tick hot path** (`OnTick()` and everything it calls). Every
   new piece of state (RSI/ATR/MACD/Stochastic internals, the new bar-history ring buffer) must be
   a fixed-size `RingBuffer<T,N>`/`std::array`/plain scalar, sized at compile time — never a
   `std::vector`/heap allocation inside a per-tick call.
2. **Bounded memory regardless of tick volume.** The new bar-history buffer (Task 5) must be a
   fixed-size ring (7-8 bars max, per NR7/TurtleSoup's real lookback needs), never growing with
   stream length.
3. **Reuse real production leaf classes — never re-derive a formula that already exists.** The 4
   already-pure `Detect*` functions (`IndicatorComputations.h`) must be called as-is, not
   reimplemented. Only the genuinely-missing TA primitives (Tasks 1-4) are new code.
4. **`ToolProgressLogger::CheckMemoryBudget()`/`--max-rss-mb`** wired into the new CLI (Task 11),
   matching this tool family's established convention.

---

### Task 1 (spec §3 item 2): Reimplement + natively test Wilder's ATR

**Files:** `include/WilderAtrEngine.h` (new, pure, header-only), `tests/cpp/test_wilder_atr_engine.cpp` (new)

**Why first:** ATR is the single most-reused primitive — needed by `DetectKangarooTail`,
`DetectTurtleSoup`, `DetectElderBreakout`, and `ATR_PROXIMITY` directly. Building it first
unblocks the most downstream work.

- [x] **Step 1:** Implemented (`include/WilderAtrEngine.h`). Confirmed real period from source:
  `TripleScreen2.cpp:483` uses N=14; `TripleScreen3.cpp:529` uses N=10 ("institutional standard,
  hardcoded") and is the one that actually feeds `DetectElderBreakout`'s `atr`/Keltner-band
  inputs (`TripleScreen3.cpp:1032`) — period is a runtime constructor parameter, not hardcoded,
  since both are genuinely used. Fixed-size state only (running value + small seed accumulator).
- [x] **Step 2:** Native test (`tests/cpp/test_wilder_atr_engine.cpp`) — hand-computed 5-bar
  reference sequence, warm-up/seeding behavior, flat-bar edge case. **ALL PASS.**
- [ ] **Step 3 (validation, may be deferred — see Task 12's blocker note):** If a current-schema
  real `.alpha`/`.context` file becomes available, cross-check this implementation's ATR output
  against the real `sc.ATR()`-derived values it contains for the same bars. Not blocking to merge
  this task — Wilder's ATR is a fully standard, unambiguous formula — but should be done before
  this initiative is considered fully validated.

### Task 2 (spec §3 item 2): Reimplement + natively test RSI

**Files:** `include/RsiEngine.h` (new, pure, header-only), `tests/cpp/test_rsi_engine.cpp` (new)

- [x] **Step 1:** Implemented (`include/RsiEngine.h`). Confirmed from source: TWO genuinely
  different real configurations exist, not one — `TripleScreen2.cpp:385-386` (`IndicatorKey::RSI`,
  feeds Lock B) uses period=2, `MOVAVGTYPE_WILDERS` ("the correct smoothing method for RSI");
  `TripleScreen3.cpp:715-716` (`DetectMomentumPinball`'s rsi3/rsi10) uses periods 3/10,
  `MOVAVGTYPE_SIMPLE`, which is the standard, named "Cutler's RSI" variant (true rolling SMA of
  gain/loss, not Wilder's recursive smoothing) — a legitimate documented formulation, not a guess.
  Smoothing mode is a runtime constructor parameter so one engine serves both real configs.
- [x] **Step 2:** Native test (`tests/cpp/test_rsi_engine.cpp`) — hand-computed sequences for both
  Wilder's and Cutler's modes, warm-up behavior, all-gains saturation edge case. **ALL PASS.**
- [ ] **Step 3 (validation, same deferral note as Task 1 Step 3).**

### Task 3 (spec §3 item 2): Reimplement + natively test Stochastic %K

**Files:** `include/StochasticEngine.h` (new, pure, header-only), `tests/cpp/test_stochastic_engine.cpp` (new)

- [x] **Step 1:** Implemented (`include/StochasticEngine.h`). Confirmed real signature from
  `sierra_chart_dependencies/sierrachart.h:584`: `Stochastic(BaseDataIn, Out, FastKLength,
  FastDLength, SlowDLength, MAType)` → `InternalStochastic(..., Out.Arrays[0] /*FastK*/,
  Out.Arrays[1] /*FastD*/, ...)` — a standard "Slow Stochastic" cascade (RawK → FastD → SlowD),
  `TripleScreen3.cpp:717` uses (3,3,3, SIMPLE). **Not confirmable from source**: whether
  `DetectMomentumPinball`'s `stochK` input reads FastK (raw) or FastD (once-smoothed) — SC's
  internal array indexing isn't in the vendored headers. Engine exposes all three stages
  (`RawK()`/`FastD()`/`SlowD()`); `Value()` defaults to FastD, flagged for Task 12 validation.
  **Bug found and fixed during implementation**: the initial rolling-window logic (both the
  fastK high/low window and `SmaWindow`) had an avoidable off-by-one warm-up delay — traced by
  hand, confirmed via the native test, fixed before merging (not shipped with the bug).
- [x] **Step 2:** Native test (`tests/cpp/test_stochastic_engine.cpp`) — hand-computed RawK/FastD
  sequence, warm-up timing, flat-range divide-by-zero guard. **ALL PASS.**
- [ ] **Step 3 (validation, same deferral note).**

### Task 4 (spec §3 item 2): Reimplement + natively test EMA/SMA and the Elder Impulse "color"

**Files:** `include/EmaEngine.h` (new, pure, header-only), `tests/cpp/test_ema_engine.cpp` (new)

- [x] **Step 1:** Implemented (`include/EmaEngine.h`). Confirmed from source, and corrected an
  initial wrong assumption from this plan: the MACD at `TripleScreen3.cpp:1546` feeds
  `Oscillator310` (overnight-exit signal), **not** the Elder Impulse color — the real impulse
  source is `GetImpulse(maDiff, macdDiff)` (`src/StudyHelperFunctions.cpp:671-682`, transcribed
  exactly: GREEN if both>0, RED if both<0, else BLUE), fed by `sc.ExponentialMovAvg(...,
  Subgraph_ImpulseEMA, 13)` (`TripleScreen1.cpp:249`, classic Elder EMA(13)) and a MACD histogram
  from a chart-attached study referenced via `sc.GetStudyArrayUsingID` (`TripleScreen1.cpp:203-
  220`) — **its fast/slow/signal lengths are operator chart configuration, not in source**;
  `MacdEngine` defaults to industry-standard MACD(12,26,9), flagged for Task 12 validation.
  `ComputeImpulse()` itself (`include/IndicatorComputations.h:303`) was found ALREADY pure and
  portable (takes `greenColor`/`redColor`/`blueColor` as opaque caller-supplied ints) — reused
  directly, not reimplemented; only the EMA/MACD inputs feeding it needed building. Keltner center
  line confirmed separately: `sc.MovingAverage(..., MOVAVGTYPE_EXPONENTIAL, length=10)` of
  `SC_OHLC_AVG` (`TripleScreen3.cpp:446-458,523-526`).
- [x] **Step 2:** Native test (`tests/cpp/test_ema_engine.cpp`) — hand-computed EMA and MACD
  sequences, Elder Impulse color classification (all sign combinations), EMA convergence sanity
  check. **ALL PASS.**
- [ ] **Step 3 (validation, same deferral note).**

### Task 5 (spec §3 item 2's feasibility assessment): Add bounded multi-bar history to the replay engine

**Files:** `include/BoundedBarRing.h` (new), `tests/cpp/test_bounded_bar_ring.cpp` (new),
`tools/market_data_replay/MarketDataReplayEngine.h` (modify),
`tools/market_data_replay/test_task5_bar_history_wiring.cpp` (new)

- [x] **Step 1:** Implemented `BoundedBarRing` (fixed max capacity 32, DOD-bounded). **Corrected
  the plan's own wrong assumption**: `TurtleSoup`'s lookback is NOT "4 days" — confirmed via
  source (`TripleScreen3.cpp:539-540`, `sc.Highest(sc.High,...,20)`/`sc.Lowest(sc.Low,...,20)`) it
  is a genuine **20-bar** (TS3/15-min) rolling window, evaluated as of the last CLOSED bar only
  (`TripleScreen3.cpp:1195-1202`, "Turtle Soup must use PREVIOUS lookback window"). The real
  function name `SetPrevFourBarExtremes()` is itself a pre-existing, misleading naming mismatch in
  the live codebase (not introduced here). `ElderBreakout`'s consolidation-bar counter deferred to
  Task 7 (pattern-specific, not a shared/generic history need).
- [x] **Step 2:** Wired into `MarketDataReplayEngine.h`: two `BoundedBarRing` members
  (`m_nr7Ring`=7, `m_turtleSoupRing`=20), pushed in `OnTs3BarClose()`, exposed via
  `GetNr7Ring()`/`GetTurtleSoupRing()`.
- [x] **Step 3:** `tests/cpp/test_bounded_bar_ring.cpp` (pure ring-buffer unit tests: warm-up,
  chronological access, eviction, NR7-style narrowest-range usage) — **ALL PASS**.
  `tools/market_data_replay/test_task5_bar_history_wiring.cpp` (engine-integration smoke test:
  feeds real ticks across 9 TS3 bars, confirms both rings fill/evict correctly) — **ALL PASS**.
  Full existing `test_market_data_replay_engine.cpp` suite re-run — **no regression, all pass**.

### Task 6 (spec §3 item 2): Extract `NR7` into a pure function — DONE

**Files:** `include/IndicatorComputations.h` (add `DetectNR7`), `tests/cpp/test_indicator_computations.cpp` (extend), `src/TripleScreen3.cpp` (thin wrapper call site, replacing the inline block — read-modify, not a behavior change)

- [x] **Step 1:** Extracted the real inline logic into `inline NR7Enum DetectNR7(...)`
  (`include/IndicatorComputations.h`, immediately after `NR7Enum`'s own definition). Confirmed a
  real discrepancy while reading source: `NR7Enum`'s own doc comment describes a richer tiered
  WEAK(95-100%)/STRONG(85-95%)/EXTREME(<80%) design, but the LIVE call site's own comment says
  "simplified from WEAK/STRONG/EXTREME" and unconditionally classifies any qualifying bar as
  `STRONG` — `DetectNR7()` deliberately matches the real live simplified behavior (extraction, not
  a silent enhancement). Takes `isChaosClimateBlocked` (bool) instead of a `MarketClimate` enum
  parameter (`Indicator.h`'s `MarketClimate` isn't includable here without pulling in ACSIL headers
  transitively) — caller evaluates `currentClimate == MarketClimate::SHANNON_CHAOS` and passes the
  bool, same convention as the other `Detect*` functions. Accepts the 7 prior bars' ranges via
  `const float*`/count (unordered-safe, designed to be fed from `BoundedBarRing::RangeAt()`).
  One accepted, documented precision difference: production's `sc.FormattedEvaluate(...,
  GREATER_OPERATOR, ...)` is display-format-rounding-tolerant; the pure function uses a plain `<=`
  float comparison — flagged in the function's own doc comment for Task 12's validation, not
  silently assumed identical.
- [x] **Step 2:** Rewired `TripleScreen3.cpp`'s live call site: replaced the inline 7-bar loop +
  climate gate + metrics-calculation block with a call to `DetectNR7()`, gathering the 7 prior
  ranges into a local array and forwarding `sc.Volume[sc.Index]`/`Subgraph_AvgVolume[sc.Index]`.
  Removed a now-redundant duplicate `qualityScore` recomputation left over from the original block
  (same formula, now computed once inside `DetectNR7`). All downstream code (indicator `Update`/
  `SetMetrics` calls, context flags, breakout visualization) left untouched. Full clean
  `./build_dll.sh` (required — editing `IndicatorComputations.h`, which is part of the PCH, staled
  it) succeeded with zero errors, 1.72 MB DLL.
- [x] **Step 3:** Native test added to `test_indicator_computations.cpp` — 3 cases (STRONG when
  narrowest with hand-computed avg/percentile/volumeSpike/qualityScore; NONE + verified-zeroed
  defaults when one prior bar isn't wider; NONE when chaos-climate-blocked regardless of ranges).
  Full suite re-run: ALL PASS, no regressions (22 pre-existing checks + 8 new).

### Task 7 (spec §3 item 2): Assemble the 17 `PRIMARY_TRIGGER_MASK` detectors in the replay engine


**Files:** `tools/market_data_replay/MarketDataReplayEngine.h` (extend), `tools/market_data_replay/test_market_data_replay_engine.cpp` (extend), `include/StochasticEngine.h` (doc comment resolved)

- [x] **Step 1 — DONE:** Wired `DetectKangarooTail`/`DetectTurtleSoup`/`DetectMomentumPinball`/
  `DetectElderBreakout`/`DetectNR7` into `OnTs3BarClose()`'s per-bar-close path. Added shared
  compute engines as new members: `WilderAtr m_atr10` (10, matches `Subgraph_AtrTemp3`),
  `RsiEngine m_rsi3`/`m_rsi10` (3/10, SIMPLE — "Cutler's RSI", confirmed real defaults),
  `StochasticEngine m_stoch` (3,3,3 SIMPLE), `Ema m_ts3KeltnerEma10` (EMA10 of TS3 OHLC-avg —
  Elder Breakout's Keltner center), `SmaWindow m_avgVolume14` (shared avg-volume input for
  NR7/MomentumPinball/ElderBreakout, confirmed `Input_AvgLength` real default = 14), plus a
  TS2-side `Ema m_ts2ImpulseEma13` + `MacdEngine m_ts2Macd` (12,26,9) feeding `INTERM_IMP` (see
  below). **Real findings confirmed while wiring (not assumed):**
  - `DetectMomentumPinball`'s `stochK` reads `Subgraph_StochK[sc.Index]` directly — i.e.
    `sc.Stochastic()`'s Arrays[0]/FastK/RawK, NOT the separately-extracted `Subgraph_StochD`
    (FastD) — resolves `StochasticEngine.h`'s own previously-flagged open question; its header
    comment updated in place (`RawK()` is the call site to use, not `Value()`/`FastD()`).
  - `INTERM_IMP` (the Impulse color `MomentumPinball` reads) is **TS2's own** Impulse
    computation, a genuine cross-timeframe read from a TS3 pattern — traced fully through
    `src/TripleScreen2.cpp`: `maDiff` = EMA(13) of TS2's OHLC-avg delta; `macdDiff` = a MACD
    histogram delta from `scsf_Screen2_MACD`, a **repo-owned custom study** (not a third-party
    chart reference) with real, confirmed-from-source (12,26,9) EMA-on-close defaults — higher
    confidence than `EmaEngine.h`'s own pre-existing TS1/`LONG_IMP` finding, which remains
    genuinely chart-configurable. Production stores its own per-TS3-bar impulse-color snapshot
    (`Subgraph_PrevImpulseColor[sc.Index] = currentImpulseColor`, confirmed at
    `TripleScreen3.cpp:994`) rather than reading TS2's bar cadence directly — replicated via
    `m_ts3PrevImpulseColorSnapshot`.
  - Elder Breakout's "consolidation bars" heuristic (`ELDER_CONSOLIDATION_LOOKBACK=5`,
    `ELDER_MIN_CONSOLIDATION_BARS=3`) counts, over the trailing 5 bars **including the just-
    closed one**, how many had their own close within 1× their own concurrent ATR(10) of their
    own concurrent Keltner band — confirmed via `TripleScreen3.cpp`'s real loop bound
    (`idx <= sc.Index`), opposite order from NR7/TurtleSoup (whose windows explicitly *exclude*
    the just-closed bar) — implemented via 4 parallel `RingBuffer`s (close/topBand/bottomBand/
    atr), pushed before counting, not after.
  - NR7/TurtleSoup's ring reads happen **before** `BoundedBarRing::OnBarClose()` runs for the
    current bar (their real "previous window" semantics, per `BoundedBarRing.h`'s own citation) —
    `OnTs3BarClose()` was reordered so ring reads precede ring writes.
  - Real `IndicatorKey` enum (`include/IndicatorKey.h`, already ACSIL-independent) reused
    directly for dirty-bit positions — genuine parity with production's own
    `PRIMARY_TRIGGER_MASK` bit convention, not an independent replay-tool-local numbering.
  - Climate gate (`MarketClimate::SHANNON_CHAOS` blocking NR7) is **not** ported — `Indicator.h`'s
    `MarketClimate` isn't includable in this ACSIL-free tool; `DetectNR7()`'s
    `isChaosClimateBlocked` is hardcoded `false` here, documented as an accepted scope gap (the
    replay tool has no live regime/climate signal at all yet, a larger pre-existing gap, not
    specific to NR7).
  - Full clean rebuild not required (all changes confined to `tools/market_data_replay/` +
    `include/StochasticEngine.h`'s doc-only comment, neither part of `CMakeLists.txt`'s DLL
    target) — confirmed via `grep` that no `src/` file includes any of the 4 engine headers.
- [x] **Step 2 (partial) — DONE for 4 of the 12: `RSI`, `INTERM_IMP` (its own dirty bit),
  `ATR_PROXIMITY`, `STRUCTURE_TEST`.** Source-traced via a dedicated `Explore` subagent pass then
  independently re-verified by direct `grep`/`read_file` (the subagent's report contained at
  least 2 real errors, both caught before wiring — see below), matching this plan's own
  discipline requirement.
  - **`RSI`** (a top-level TS2 indicator, distinct from `MomentumPinball`'s internal rsi3/rsi10):
    real config is period=2, `MOVAVGTYPE_WILDERS`, on TS2's own close
    (`src/TripleScreen2.cpp:385-386`, inside `scsf_Screen2_MACD` despite the function's name).
    Classified via `DetectRSI()` (>70 OVERBOUGHT, <30 OVERSOLD, else NORMAL) — already pure,
    just needed relocating (see extraction note below). Wired as `m_rsiTop` (`RsiEngine(2,
    WILDERS)`), fed on TS2 bar close.
  - **`INTERM_IMP`** now gets its own dirty bit (`m_lastEmittedTs2ImpulseColor` tracks the last
    value a dirty bit was emitted for, separate from `m_ts3PrevImpulseColorSnapshot` which tracks
    MomentumPinball's own cross-timeframe snapshot — two different "previous" concepts that both
    read the same `m_currentImpulseColor`, now correctly kept independent).
  - **`ATR_PROXIMITY`**: real formula confirmed at `src/StudyHelperFunctions.cpp:1528` — but its
    real signature takes `SCStudyInterfaceRef sc` directly (the subagent's report incorrectly
    called it "pure" — caught by reading the actual declaration in `StudyHelperFunctions.h`,
    which itself `#include`s `sierrachart.h`). A new pure `ClassifyATRProximity(high, low,
    close, atr)` was extracted (bar-range vs 1.0×/2.5×ATR thresholds, then close-position
    tie-break for the >2.5× case), and the original `DetectATRProximity(sc, atr)` now forwards
    to it.
  - **`STRUCTURE_TEST`**: `ClassifyStructure(float...)` was ALREADY pure-signature (unlike
    `DetectATRProximity`) but lived in the same ACSIL-coupled `StudyHelperFunctions.h`/`.cpp` —
    moved verbatim to `IndicatorComputations.h` (no logic change), `DetectStructure(sc,...)` now
    forwards to it. Needs the 20-bar lookback high/low **inclusive of the current bar**
    (`sc.Highest/Lowest(...,20)`'s real semantic) — confirmed genuinely different from
    TurtleSoup's own *exclusive*-of-current window, despite both nominally being "20-bar" —
    reused `m_turtleSoupRing.HighestHigh()/LowestLow()` but read AFTER the ring's own
    `OnBarClose()` update (opposite timing from TurtleSoup's own read earlier in the same
    function), plus a new `m_prevTs3High`/`m_prevTs3Low` scalar snapshot for the immediately-
    prior bar's own high/low.
  - **Real production-file extraction** (mirrors Task 6's `DetectNR7` precedent exactly): `enum
    class RSI`/`StructureTest`/`ATRProximityEnum` and `DetectRSI()`/`ClassifyStructure()` moved
    from `include/Indicator.h`/`src/StudyHelperFunctions.cpp` into `include/IndicatorComputations.h`
    (single source of truth, no duplication — `StudyHelperFunctions.h`'s declarations updated,
    its `.cpp` now has thin `sc`-argument-forwarding wrappers only). Full clean `./build_dll.sh`
    verified zero errors, 1.72 MB DLL, before any replay-tool wiring was attempted.
  - **2 subagent errors caught by independent re-verification** (documented since this is exactly
    the "verify, don't assume" failure mode this session's discipline exists to catch): (1) the
    subagent claimed `DetectATRProximity`/`DetectEmaProximity` were "pure" when their real
    signatures take `SCStudyInterfaceRef sc` directly; (2) the subagent claimed
    `INTERM_STOCHASTIC` (`scsf_Screen2_StochasticCrossover`, `TripleScreen2.cpp:1092`) reads TS3
    data — re-verified via direct `grep`+`read_file`: it is genuinely its own TS2-native
    Stochastic(10,3,3) SIMPLE with `sc.CrossOver`-based crossover detection, a different config
    from TS3's (3,3,3) one, not TS3 data at all.
  - Native tests added to `test_indicator_computations.cpp` (`detect_rsi_*`,
    `classify_structure_*`, `classify_atr_proximity_*`, 14 new hand-verified cases) and
    `test_market_data_replay_engine.cpp` (`task7_atr_proximity_*`/`task7_structure_test_*`, a
    fresh-engine 21-bar integration test with a hand-verified `DECISIVE_BREAKOUT_HIGH` — including
    catching and fixing one of the plan's OWN wrong assumptions: the ATR_PROXIMITY dirty bit
    does NOT fire on bar #21 because the 20 filler bars already settle to `HIGH_MOVE` once
    ATR(10) seeds, so bar #21's also-`HIGH_MOVE` result is correctly not a "change"). Full suite:
    ALL PASS, 87 checks, no regressions.
  - **`LONG_IMP` (TS1's own Elder Impulse) — DONE, same session, right after the above.**
    Confirmed via direct source read (not the earlier subagent pass): `Input_InputData` for TS1's
    Impulse EMA defaults to `SC_LAST` (close) — genuinely DIFFERENT from TS2/TS3's Keltner-style
    EMAs, which use `SC_OHLC_AVG` — a real, easy-to-miss distinction, not assumed symmetric.
    `scsf_Screen1_MACD` (`src/TripleScreen1.cpp:688`) is a repo-owned custom study, same pattern
    as TS2's `scsf_Screen2_MACD`, with real confirmed defaults MACD(12,26,9) EXPONENTIAL on
    close — this RESOLVES `EmaEngine.h`'s own previously-flagged "NOT confirmable from source"
    caveat about `LONG_IMP`'s MACD study (its header comment updated in place). Wired as
    `m_ts1ImpulseEma13`/`m_ts1Macd`, feeding a new `m_currentTs1ImpulseColor` +
    `IndicatorKey::LONG_IMP` dirty bit in `OnTs1BarClose()`. Native test: a 60-bar sustained-
    uptrend smoke test (full hand-computed EMA(13)+MACD(12,26,9) trace across many 240-min bars
    isn't practical by hand, unlike the single/few-bar cases above) confirms the color settles to
    a well-defined value and the dirty bit fires at least once. Full suite: ALL PASS, 89 checks
    total, no regressions.
  - **`INTERM_STOCHASTIC` — DONE.** Real config confirmed via `scsf_Screen2_StochasticCrossover`
    (`src/TripleScreen2.cpp`): TS2's OWN Stochastic(10,3,3) SIMPLE — a genuinely different config
    from TS3's own (3,3,3) MomentumPinball stochastic, despite the "INTERM" name suggesting
    otherwise. Real naming trap confirmed: that study's own local variables `Subgraph_SlowK`/
    `Subgraph_SlowD` are actually bound to `sc.Stochastic()`'s Arrays[0]/Arrays[1] (RawK/FastD),
    NOT a genuine Slow-K/Slow-D third smoothing stage — the crossover is really RawK-vs-FastD.
    Divergence detection (a separate, disabled-by-default regression-slope branch) is NOT ported,
    matching this repo's own default config. `StochasticEnum` migrated from `Indicator.h` to
    `IndicatorComputations.h` (same ACSIL-independence rationale as `RSI`/`StructureTest`/
    `ATRProximityEnum`) alongside a new `ClassifyStochasticCrossover()` pure function (a genuine
    extraction of previously-inline crossover logic) — the live `TripleScreen2.cpp` call site was
    deliberately NOT rewired to call it (unlike the RSI/StructureTest/ATRProximity precedent),
    since that live logic has an extra runtime toggle (`Input_UseBuySell.GetYesNo()`) the pure
    function doesn't model; rewiring risked a live behavior change for a toggle this initiative
    doesn't need. Wired a new `StochasticEngine(10,3,3)` instance (`m_ts2Stoch`) + previous-bar
    RawK/FastD tracking into `OnTs2BarClose()`. Native test: a V-shaped (decline-then-rally) TS2
    price smoke test (full hand-computed 10-bar-SMA-cascade crossover trace not practical by hand,
    same tradeoff as `LONG_IMP`) confirms a well-defined classification and at least one dirty-bit
    fire. Full suite: ALL PASS, 96 checks total, no regressions.
  - **`EMA_PROXIMITY` — DONE.** Real config confirmed via `scsf_Screen2_KeltnerChannel`
    (`src/TripleScreen2.cpp`): `ema` = the SAME EMA(13)-of-`SC_OHLC_AVG` line already computed for
    `INTERM_IMP` (`m_ts2ImpulseEma13`, confirmed identical formula); `stdDev` = a rolling 13-bar
    stddev of THAT LINE'S OWN VALUES (`sc.StdDeviation(Subgraph_KeltnerAverage, ..., 13)`), NOT of
    price. `EmaProximity` migrated from `Indicator.h` to `IndicatorComputations.h` (same rationale
    as `RSI`/`StructureTest`/`ATRProximityEnum`/`StochasticEnum`) alongside a new
    `ClassifyEmaProximity()` pure extraction; the live `DetectEmaProximity(sc,...)` call site WAS
    rewired (unlike `INTERM_STOCHASTIC`'s crossover, this one had no extra runtime toggle
    complicating a faithful match) to forward to it. Same-session finding, analogous to `NR7Enum`:
    `EmaProximity::BELOW_STRONG` has ZERO real producers (grep-verified) — `ABOVE_STRONG` is only
    ever the cold-start default/unreachable dead code, never a live "strong" classification despite
    the enum's own naming implying otherwise; documented inline, not silently left unexplained.
    Population vs. sample stddev denominator for `sc.StdDeviation` is unconfirmable from the
    vendored ACSIL headers (closed-source engine) — population (n) implemented, flagged for Task
    12. Native tests: 7 hand-computed `ClassifyEmaProximity` cases (all 2 cross directions + AT_EMA
    + both TOUCH tiers + both PRICE_*_EMA tiers) in `test_indicator_computations.cpp`; a sustained-
    uptrend integration smoke test in the engine suite (rolling-stddev-of-a-lagging-EMA hand-trace
    not practical, same tradeoff as `LONG_IMP`/`INTERM_STOCHASTIC`) confirms `PRICE_ABOVE_EMA`/
    `ABOVE_TOUCH`/`CROSS_ABOVE` and a dirty-bit fire. Full clean `./build_dll.sh` + both native
    suites re-verified: ALL PASS, zero regressions (98 engine-suite checks total).
  - **`VOLUME_SIGNAL` — DONE, and much cheaper than expected.** `VolumeState`/
    `ComputeVolumeBarSample`/`ComputeVolumeClassification`/`VolumeEnum` were ALREADY fully pure
    (`include/IndicatorComputations.h`, extracted from `VolumeIndicator` in an earlier,
    pre-this-session pass) — zero extraction needed, just wiring. Real session routing (`isRTH`)
    confirmed via `TimeOfDayEnum`'s own real value comments (`include/Indicator.h`): RTH =
    `OPENING_HOUR`..`PM_RUN_ENTRY` = 09:30-16:00 ET. New `IsRthSession(timestampUs)` helper reuses
    the same `ete::GetEasternUtcOffsetSeconds` DST-aware conversion already trusted by
    `TickBarAggregator`. Production samples the bar-magnitude baseline once per closed bar and
    self-classifies per-tick using the live forming bar's own bid/ask volume; this engine's
    already-established bar-close-only cadence uses the just-closed bar's own aggregated bid/ask
    volume as its per-bar proxy for that per-tick read (consistent simplification, not a new one).
    Native test: a stable-but-varied 20-bar RTH volume baseline (needed for a non-degenerate
    MAD) followed by a genuine 10x volume spike confirms a HIGH-tier classification and a
    dirty-bit fire. Full clean `./build_dll.sh` + both native suites re-verified: ALL PASS, zero
    regressions (102 engine-suite checks total).
  - **`SIDE` — DONE, per operator directive: always FLAT.** Real value is
    `PositionManager::GetTradeSide()` — execution-layer state, not a market-observation signal at
    all; this offline replay tool has no execution/position state to read by design. Operator
    directive (2026-09-16): "by convention, SIDE is always FLAT." `GetSideResult()` returns the
    constant `0` (`TradeSideEnum::FLAT`); no dirty-bit wiring added since a truly constant value
    can never fire one (matches what a real, permanently-flat account would also produce).
    `TradeSideEnum` itself was NOT migrated to `IndicatorComputations.h` (unlike the other extracted
    enums) — it's a core execution-layer concept used pervasively elsewhere (`PositionManager`,
    `TripleScreen3.cpp`, `BackTesterStudy.cpp`), not confined to one `Detect`/`Classify` function,
    so migrating it would be scope creep for a single constant. Native test: confirms
    `GetSideResult()==0` and the dirty bit never fires across 30 bars.
  - **`DAILY_BIAS` — DONE, and much cheaper than expected (same pattern as `VOLUME_SIGNAL`).**
    `include/DailyBiasEngine.h`'s `dbe::ComputeDailyBias()` was ALREADY fully pure — zero
    extraction needed. Real call site (`src/TripleScreen3.cpp`) passes the SAME TS3-local Hurst
    already computed for `ElderBreakout` (`m_lastElderBreakoutTs3Hurst`) and its own real
    `prevDayHigh`/`prevDayLow` (now available via `m_dailyBars`, wired for `STRUCTURE_TEST`).
    Two real findings resolved the "needs Value Area + PathEfficiencySNR" gap entirely: (1) the
    real `CalculateDailyBias()` wrapper's `entropy`/`PathEfficiencySNR` parameter is a commented-out,
    entirely unused pass-through (`float /*entropy*/`, confirmed by direct read) — never a real
    gap; (2) `DailyBiasEngine.h`'s own header comment documents `valueAreaLow`/`valueAreaHigh`
    `0.0f` as an intentional, DESIGNED sentinel ("fall back to the naive 15%/85% range-split proxy
    this engine replaces"), not an invented approximation — passing `0.0f`/`0.0f` exercises a real,
    intentional fallback path in the engine itself. Native test: hand-verified that `ComputeDailyBias`'s
    own first-checked Hurst gate ((0.45,0.55) -> `PHYSICS_VETO_RANDOM_WALK`) unconditionally
    dominates during this engine's own 101-bar DFA warmup (Hurst stays at its documented 0.5
    cold-start default) — true even with a real day-0 session seeded, proving the Hurst gate, not
    missing day data, controls the classification during warmup. Full clean `./build_dll.sh` + both
    native suites re-verified: ALL PASS, zero regressions (107 engine-suite checks total).
  - **`RASCHKE_TACTICAL_TRIGGER` — DONE (2026-09-16, "institutional fix" session).** Full 5-writer,
    last-write-wins precedence chain implemented (`src/TripleScreen3.cpp`'s real ascending-line-
    number order): base classifier (`DetectRaschkeTacticalTrigger`'s real RSI-failure-swing +
    Stochastic-pop + ITR-breakout/fade logic, `src/StudyHelperFunctions.cpp`, `PatternConstants`
    values confirmed by direct grep, not assumed) → `KangarooTail`'s own quality/support-resistance
    gate → `ElderBreakout`'s own STRONG/EXTREME gate → `TurtleSoup`'s own STRONG/EXTREME +
    at-daily-extreme gate → NR7-breakout's own 3-bar marker-history gate (highest precedence).
    `RaschkeTacticalTrigger` (the enum itself, zero ACSIL dependency) migrated from `Indicator.h`
    to `IndicatorComputations.h` — same single-source pattern as `KangarooTailEnum`/`TurtleSoupEnum`/
    etc., `Indicator.h` re-exposes it via its existing `#include "IndicatorComputations.h"`. New
    state: a 5-bar RSI-swing high/low ring, per-ET-calendar-day ITR high/low/breakout/breakdown
    persistent state (`GetEasternTimeParts()`, a new static ET hour/minute/day-number helper
    alongside the existing `IsRthSession()`), a 3-bar NR7 marker-history ring, and
    `m_lastKangarooTailQuality` (needed by KangarooTail's own gate, not previously retained outside
    its own detection block). `MOMENTUM_PINBALL_BUY/SELL` confirmed dead (writer removed
    2026-09-04, `src/StudyHelperFunctions.cpp`'s own comment) — correctly never produced. Native
    test: a long varied-price/varied-volume 400-bar walk spanning multiple ET calendar days
    (smoke test, not exact-value — replicating the full 5-writer cascade by hand in the test itself
    would just re-derive the same logic under test, the same rationale already used for
    `task7_interm_stochastic`/`task7_long_impulse` above) confirms the wiring is genuinely live:
    the dirty bit fires and a non-`NONE` result is produced at least once. Full clean
    `./build_dll.sh` + both native suites re-verified: ALL PASS, zero regressions (104 engine-suite
    checks total). See "Institutional fix" section below for the TurtleSoup separation-filter gap
    this same investigation surfaced and fixed first.
  - **`RASCHKE_STRATEGY_SETUP` — DONE (2026-09-16, same "institutional fix" session, Gemini-
    consulted design pass, CLAUDE_BRIEF_147/148).** All 13 sub-patterns ported
    (`DetectRaschkeStrategySetup`, `src/StudyHelperFunctions.cpp`): Holy Grail Buy/Sell/
    Continuation, Double Repo Failure, Double Repo, Bread and Butter, Anti, Slingshot, Ghost,
    Two-B Reversal, Whiplash, Three Bar Triangle, NR4/NR7/IDNR4/NR4_NR7_Volume_Spike, First Cross —
    single monolithic `OnTs2BarClose()`-integrated cascade mirroring production's own early-return
    precedence order exactly (Gemini's own explicit recommendation over splitting into free
    functions, given the early-return-halts-the-cascade semantics). `RaschkeStrategySetup` (the
    enum itself, zero ACSIL dependency) migrated from `Indicator.h` to `IndicatorComputations.h`,
    same single-source pattern as `RaschkeTacticalTrigger`'s own earlier move. New engines/state: a
    TS2-local 100-bar DFA(minScale=8) Hurst (`m_ts2HurstCloses` — a THIRD independent instance of
    the same fixed formula, after TS1's canonical dim and TS3's ElderBreakout dim), a 21-period EMA
    of close (`m_ts2Ema21`), a dedicated Stochastic(7,4,10) for ANTI (`m_ts2AntiStoch` + 3-deep
    FastD/SlowD history rings), an independent 2-deep MACD-diff shift register feeding the already-
    pure `ComputeMacd()` (`IndicatorComputations.h` — confirmed already ACSIL-free, needed only
    wiring, not extraction), and a 30-bar `BoundedBarRing` (`m_ts2PatternRing`, sized for GHOST's own
    deepest swing-confirmed divergence lookback) with parallel close/volume rings. A new
    `IsSwingHighAt()`/`IsSwingLowAt()` pair replicates Sierra Chart's real, symmetric,
    look-ahead-required swing definition (verbatim-sourced from `sierrachart.h`) directly against
    the ring, returning `false` (not a swing) whenever either flank would read outside the ring's
    real, closed-bar range — deliberately never reading a not-yet-closed future bar, unlike two
    real gaps found in the live code during this port (see "Institutional fix" section below: a
    small-lookback edge case in `DOUBLE_REPO`/`DOUBLE_REPO_FAILURE`, and a much larger, previously-
    unknown structural dead-code bug in `GHOST` — both Gemini-confirmed, the `GHOST` one fixed live
    too). `TWO_B_LOOKBACK`'s real value (5, not the stale "20-bar" text in its own section's
    comments) and the `BREAD_AND_BUTTER` `ema`/`ema21` self-comparison wiring bug (also fixed live,
    `src/TripleScreen2.cpp`) were the other 2 real findings from this same investigation. Overall
    cascade gated on the 30-bar ring being full — a single, deliberately conservative blanket
    warm-up gate rather than replicating each sub-pattern's own smaller individual threshold
    (documented simplification, not a correctness gap). Native tests: a hand-constructed
    `THREE_BAR_TRIANGLE` fixture (27 flat filler bars that can never register as a swing point,
    ruling out every earlier-precedence pattern without needing to hand-verify each one's own full
    cascade) plus a smoke test (long varied-price/volume 200-bar walk confirms the dirty bit fires
    and a non-`NONE` result is produced at least once — same rationale as `RASCHKE_TACTICAL_TRIGGER`'s
    own smoke test, hand-verifying one exact 13-writer precedence outcome end-to-end would just
    re-derive the cascade under test). Full clean `./build_dll.sh` + both native suites
    re-verified: ALL PASS, zero regressions (107 engine-suite checks total).

**All 17 of 17 `PRIMARY_TRIGGER_MASK` keys are now wired.** Task 7 is functionally complete.

- [x] **Step 3 — DONE for all 17 of 17 wired keys** (the original 5 + RSI/ATR_PROXIMITY/
  STRUCTURE_TEST/LONG_IMP/INTERM_STOCHASTIC/EMA_PROXIMITY/VOLUME_SIGNAL/SIDE/DAILY_BIAS/
  RASCHKE_TACTICAL_TRIGGER/RASCHKE_STRATEGY_SETUP, plus INTERM_IMP's own separate bit):
  `ConsumePatternDirtyMask()` implemented, using real
  `IndicatorKey` bit positions (`IndicatorKeyBit()` helper), set when a wired detector's
  classification differs from its own immediately-prior value — matches `PRIMARY_TRIGGER_MASK`'s
  "any dirty bit fires unconditionally" semantics for all 17 keys (`SIDE` is a documented
  exception — constant by convention, so its bit can never fire).
- [x] **Step 4 (partial) — DONE for NR7, TurtleSoup, KangarooTail, ATR_PROXIMITY,
  STRUCTURE_TEST, and LONG_IMP (6 of 9 wired-with-tests keys):** New test blocks in
  `test_market_data_replay_engine.cpp` (`task7_*`): 20 hand-controlled uniform filler bars
  (range=4) fill both the NR7 and TurtleSoup rings identically, then a deliberately narrow bar
  (range=1) is fed and its `NR7Enum::STRONG` classification + dirty-bit-set + dirty-mask-clears-
  after-consume are all hand-verified; `TurtleSoupRing`'s `IsFull()`/`HighestHigh()`/`LowestLow()`
  are cross-checked against the known filler values (confirms the engine reads PRE-update ring
  state, not stale/empty). A follow-up bar with a genuine long-lower-tail shape is hand-verified
  against `DetectKangarooTail`'s real STRONG-tier boundary. A separate fresh-engine test
  hand-verifies `ATR_PROXIMITY::HIGH_MOVE` and `StructureTest::DECISIVE_BREAKOUT_HIGH` on a
  constructed breakout bar, including the 20-bar-window's inclusive-of-current-bar semantic. A
  60-bar sustained-uptrend smoke test confirms `LONG_IMP`'s color settles to a well-defined value
  and its dirty bit fires at least once (full hand-computed EMA/MACD trace not practical by hand
  for a 240-min-bar dim). Full suite re-run: ALL PASS, no regressions (89 total checks).
  `RSI`/`INTERM_IMP`'s own dirty bits are wired but not yet given a dedicated hand-verified test
  (need a multi-bar WILDERS-RSI crossing sequence, more involved than a single-bar case);
  MomentumPinball/ElderBreakout also still lack dedicated tests — flagged as a remaining Step 4
  gap for a future session.

### Post-implementation audit (2026-09-16, same session) — 2 real findings, both resolved

A full re-read of every changed file against the real call sites in this repo's own current C++
implementation (`src/*.cpp` — this is dev-only code, never deployed anywhere; not an external
"production" system to be treated as an unquestionable authority) surfaced 2 real issues in the
Step 1/2 wiring above, before any further work continued — exactly the kind of check this plan's
own "verify, don't assume" discipline exists to catch. Terminology note: this plan (and its spec)
previously used "production" loosely to mean "the current live C++ code path" — corrected here;
that code is dev-only and any real bug found in it is fixed, not treated as ground truth to
faithfully reproduce warts and all.

1. **FIXED — `STRUCTURE_TEST`'s `prev_high`/`prev_low` were wrong.** The implementation used
   `m_prevTs3High`/`m_prevTs3Low` (the immediately-prior TS3 bar's own high/low), but the real
   call site (`src/TripleScreen3.cpp:614`) passes **`prevDayHigh`/`prevDayLow`** — the PREVIOUS
   COMPLETED TRADING DAY's own high/low (`IndicatorManager::GetCachedPrevDayHigh/Low()`, itself
   sourced from `sc.GetOHLCForDate()` with a `DailyHighLowLoader` CSV override) — a variable-
   naming trap (`ClassifyStructure`'s generic `prev_high`/`prev_low` parameter names read like
   "prior bar" but the real call site binds them to daily data). **Fix**: added a 4th
   `tba::TickBarAggregator` (`m_dailyBars`, 24h period, same 18:00 ET session-start-aligned
   convention already trusted for TS1/TS2/TS3) that fires `OnDailyBarClose()` once per completed
   CME trading session and populates `m_prevDayHigh`/`m_prevDayLow` — reproduces "trading day"
   (not calendar day) boundaries without needing `sc.GetTradingDayDate()`'s exact ACSIL semantics,
   since ticks never flow across a weekend/holiday close. Deliberately NOT flushed at replay-end
   (a partial final day must never be promoted to "yesterday"). Removed the now-dead
   `m_prevTs3High`/`m_prevTs3Low`/`m_hasPrevTs3Bar` members entirely (not left as unused dead
   code). Test rewritten to seed a genuine "day 0" session (known high=110/low=90) before
   asserting `STRUCTURE_TEST`'s classification — old test was asserting the WRONG semantic and
   only appeared to pass by coincidence of the specific numbers chosen. Full suite re-verified:
   ALL PASS, no regressions.
2. **FIXED — `ElderBreakout`'s `hurst` input was the wrong dim.** The implementation fed
   `m_obs.hurst_exponent()` (the canonical TS1-based, 100-bar-fixed-window HMM-vector dim). The
   real `ElderBreakout` (`src/TripleScreen3.cpp:1027`) reads `Subgraph_HurstExponent[sc.Index]`,
   populated by `UpdateObservationVectorSubgraphs()` → `CalculateHurstExponent(sc)`. Deeper trace
   (not stopping at the first plausible-looking explanation) found this zero-arg overload
   **hardcodes** `CalculateHurstExponent(sc, 100, 8)` — i.e. the `observation_window_n`/
   `CalculateAdaptiveObservationWindow` machinery computed right above it in the real call site is
   entirely UNUSED by the Hurst line (only `CalculatePathEfficiencySNR` in that same function
   consumes it) — so no `LONG_MACD`/`INTERM_MACD`/adaptive-window porting is actually needed at
   all. The real difference is simpler and entirely about the calling context: this function
   operates on WHATEVER `sc` refers to in the caller's timeframe, and since the caller is TS3, the
   real `ElderBreakout` hurst is a **100-bar DFA(minScale=8) Hurst over TS3's OWN 15-minute bar
   closes** — the exact same formula as the canonical TS1 dim, just fed a different bar series.
   **Sanity-checked with Gemini before implementing** (read-only consult, `gemini --approval-mode
   plan`, Gemini given zero file-edit capability — see `lbrnet/logs/rc_gemini.log`
   `CLAUDE_BRIEF_145`/`_REPLY`): Gemini independently confirmed the diagnosis (`observation_window_n`
   is dead for the Hurst line, confirmed via its own direct read) and confirmed DFA is scale-
   agnostic (no soundness concern applying the same `DfaHurstExponent(..., 100, 8)` call to a
   15-minute series instead of a 240-minute one — the two measure genuinely different persistence
   horizons, ~3.8 trading days vs. ~61 trading days, which is exactly why the original mismatch
   mattered). **Fix implemented**: added a dedicated `m_ts3HurstCloses` ring (101 retained TS3
   closes, matching `ComputeTs1LiveDims`'s own "closed+live = window+1 prices" convention for the
   identical DFA(100,8) formula), fed at TS3 bar-close (the current bar's own close included as
   the window's newest point, matching the real call's own same-bar semantic); computes 100
   log-returns and calls the SAME `DfaHurstExponent()` this engine already uses for the canonical
   TS1 dim. New diagnostic accessor `GetElderBreakoutTs3Hurst()` + 2 new tests confirm the value
   is finite/in `[0,1]` after warmup AND genuinely independent of the canonical TS1
   `hurst_exponent` dim (not silently re-aliasing it). Full suite re-verified: ALL PASS, no
   regressions (94 total checks, `test_market_data_replay_engine.cpp`).

### Task 7b (spec §9): Live-code hardening — findings surfaced by Task 7's own tracing

**Files:** `src/TripleScreen3.cpp`, `src/StudyHelperFunctions.cpp`, `include/IndicatorComputations.h`

Operator directive: this initiative is also a forcing function for hardening the live C++ path,
not just the replay tool. 3 real findings so far, each investigated then either fixed live or
explicitly resolved with a documented reason (not left as a silent replay-tool-only caveat):

- [x] **Finding 1 — RESOLVED, not a bug.** Sanity-checked with Gemini (read-only,
  `lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_146`/`_REPLY`, VERY HIGH confidence): using
  `adaptive_window_n` (clamped `[10,40]`) for DFA-based Hurst would mostly violate DFA's own
  `minScale*4>=32` floor (`CalculateHurstExponent`'s own guard), silently degrading to stale
  carry-forward values across most of that range; DFA's OLS scaling-law regression also needs a
  wide, stable block (this repo's own DFA bias Monte Carlo shows a wide 95% CI even at N=100).
  TS1's own adaptive `macro_window_n` avoids this via a ×5 multiplier (clamped `[50,200]`,
  safely above the floor) — TS3/TS2's local Hurst call sites have no such multiplier, so the
  fixed (100,8) window is the only mathematically viable choice there, not an incomplete
  migration. Fix: added a clarifying comment to `UpdateObservationVectorSubgraphs`
  (`src/StudyHelperFunctions.cpp`) explaining why `adaptive_window_n` is deliberately not passed
  to `CalculateHurstExponent` — no behavior change. Full DLL rebuild verified clean.
- [x] **Finding 2 — RESOLVED.** Grep-verified `NR7Enum::WEAK`/`EXTREME` have ZERO real callers
  anywhere in this codebase — the continuous `qualityScore` this same detector already computes
  conveys the severity gradation the 3 discrete tiers would have added, so implementing them would
  add redundant complexity, not fix a gap. Updated `NR7Enum`'s own doc comment
  (`include/IndicatorComputations.h`) and the live call site's comment (`src/TripleScreen3.cpp`) to
  state this explicitly, including marking `WEAK`/`EXTREME` "NOT LIVE" inline — kept, not deleted,
  to preserve wire compatibility and design history for any future decision to revive them. No
  behavior change. Full clean `./build_dll.sh` verified (required — `IndicatorComputations.h` is
  part of the PCH, doc-only size changes still invalidate it).
- [x] **Finding 3 — RESOLVED.** `vscode_renameSymbol` had no rename provider for this standalone
  header (no language server attached), so renamed manually: `ClassifyStructure()`
  (`include/IndicatorComputations.h`) and `DetectStructure()`'s forwarding declaration/definition
  (`include/StudyHelperFunctions.h`/`src/StudyHelperFunctions.cpp`) both renamed `prev_high`/
  `prev_low` → `prevDayHigh`/`prevDayLow`. Confirmed via a repo-wide grep that this codebase
  overwhelmingly uses `prev_high`/`prev_low` to mean "the immediately-prior bar" elsewhere
  (`TripleBarrierEngine.h`, `PositionManager.cpp`, the `Event`/`TrainingEvent` schema's own
  separate `prev_day_high`/`prev_day_low` fields) — `ClassifyStructure`'s prior naming was the
  true anomaly, not the norm, strengthening the case for this rename. Full clean `./build_dll.sh`
  + both native test suites (`test_indicator_computations.cpp`,
  `test_market_data_replay_engine.cpp`) re-verified: zero behavior change, all pass.
- [x] **Finding 4 ("institutional fix", 2026-09-16) — RESOLVED, TurtleSoup's separation filter was
  missing entirely.** Discovered while tracing `RASCHKE_TACTICAL_TRIGGER`'s true 5-writer
  semantics (see Step 2's own entry above): `src/TripleScreen3.cpp` (~lines 1179-1266) applies a
  "Street Smarts separation" filter to `TurtleSoup`'s own classification that the earlier Task 7
  Step 1 wiring never ported — a real fidelity gap in already-shipped code, not a new key.
  `TURTLE_SOUP_LENGTH=20`/`TURTLE_SOUP_MIN_SEPARATION=4` (both confirmed via direct grep). Real
  algorithm: scans the 19 bars immediately before the prior-window's own newest bar (i.e.
  EXCLUDING it) for the bar-INDEX (not just value) of that sub-window's own highest-high/
  lowest-low, ties resolving to the OLDEST bar (production's own `>=`/`<=` + descending-index-loop
  semantics); if the resulting `barsSincePriorLow`/`barsSincePriorHigh` is `<4`, a qualifying
  BULLISH/BEARISH `TurtleSoupEnum` is reset to `NONE` before it ever reaches
  `RASCHKE_TACTICAL_TRIGGER`'s own gate. **Fix**: added a bar-index scan over
  `m_turtleSoupRing`'s own PRE-update state (ring indices 0..`kTurtleSoupLookbackBars-2`, i.e.
  excluding the newest/index-19 bar), computed `barsSincePriorHigh`/`barsSincePriorLow`, applied
  the `<4` reset to `TurtleSoup`'s own `result` before the dirty-bit comparison. 2 new native
  tests (`task7_turtle_soup_separation_filter_resets_recent_extreme_to_none`/
  `_allows_distant_extreme`) construct a deep-low filler bar at ring index 18 (2 bars before the
  signal bar, must be filtered) vs. ring index 0 (20 bars before, must NOT be filtered) — both
  pass, confirming the filter fires in both directions. Full clean `./build_dll.sh` + both native
  suites re-verified: ALL PASS, zero regressions.
- [x] **Finding 5 (RASCHKE_STRATEGY_SETUP port, 2026-09-16) — RESOLVED, `BREAD_AND_BUTTER`'s
  `ema`/`ema21` self-comparison was dead code.** Gemini-confirmed (`lbrnet/logs/rc_gemini.log`
  `CLAUDE_BRIEF_147` Q3). `DetectRaschkeStrategySetup` receives `ema21` (`Subgraph_EMA21`, a
  21-period EMA of close) as a direct parameter, and ALSO reads `intermMarketAction->ema()` as a
  separate local `ema`. Tracing `IntermediateMarketAction::setEma()`'s real call site
  (`src/TripleScreen2.cpp:825`, same tick, just before `DetectRaschkeStrategySetup` runs) found it
  was ALSO being fed `Subgraph_EMA21[sc.Index]` — the identical series, read twice. `ema > ema21`/
  `ema < ema21` both reduce to `X > X`/`X < X` (always false), making both of `BREAD_AND_BUTTER`'s
  branches permanently unreachable. `IntermediateMarketAction` already separately maintains a
  genuinely shorter-period `fastEma` (`Subgraph_KeltnerAverage`, EMA 13) that was computed but never
  read by this function at all — exactly the "Short EMA vs Long EMA" comparand the pattern's own
  comments describe. **Fixed live** (`src/TripleScreen2.cpp`'s `setEma()` call now passes
  `Subgraph_KeltnerAverage[sc.Index]` instead) and in the port (uses `impulseEmaValue`, this
  engine's own already-computed identical TS2 Keltner EMA(13) series, from the start — never carried
  the bug). Confirmed `ema()`/`prevEma()` have exactly one real caller each (this function), so the
  fix is fully contained. Full clean `./build_dll.sh` verified.
- [x] **Finding 6 (RASCHKE_STRATEGY_SETUP port, 2026-09-16) — RESOLVED, `GHOST` was structurally
  100% dead code in every real execution context, a larger and more consequential finding than
  Finding 5.** Gemini-confirmed after independent scrutiny (`lbrnet/logs/rc_gemini.log`
  `CLAUDE_BRIEF_148`, given the severity a dedicated follow-up consult beyond `CLAUDE_BRIEF_147`).
  `GHOST`'s real divergence check calls `sc.IsSwingLow/High(*, sc.Index, SWING_LENGTH=3)` —
  checking whether the CURRENT (just-closed/still-forming) bar is itself a swing point. Sierra
  Chart's real `IsSwingHigh`/`IsSwingLow` (verbatim from `sierrachart.h`) requires `Length` bars on
  BOTH sides — so this specific call needs `sc.Low/High[sc.Index+1..+3]`, bars that do not exist yet
  in ANY real execution context this system uses (live `AutoLoop=1`, this project's own
  `BackTesterStudy.cpp` pipeline reusing live semantics, or Sierra Chart's native Chart Replay —
  all three deliberately withhold future bars). That read deterministically evaluates as "not a
  swing" on the very first offset checked (a real price is never `<=`/`>=` an uninitialized/zero
  future read the wrong direction), making BOTH of `GHOST`'s branches permanently unreachable —
  100% of the time, not an edge case like `DOUBLE_REPO`'s own small-lookback gap (Q2 of the same
  consult). **Fix (Gemini's own explicit recommendation):** the most recent bar that CAN legitimately
  be confirmed as a swing point right now is `sc.Index - SWING_LENGTH` (today's own bar closing is
  exactly what newly confirms it) — anchor the divergence check there instead of at `sc.Index`, and
  search further back from that anchor for the prior swing. **Fixed live**
  (`src/StudyHelperFunctions.cpp`'s GHOST section) and in the port identically. This is a genuine
  live-code BEHAVIOR CHANGE (a previously-always-dead signal becomes reachable), not a narrow
  dead-code/comment fix like Findings 2/4/5 — Gemini explicitly flagged this as warranting its own
  scrutiny and downstream empirical validation (via this same initiative's own planned Task 12
  validation pass) before being treated as fully vetted, given `RaschkeStrategySetup` feeds
  scoring/risk logic downstream. Recorded here explicitly so that validation isn't skipped.
  Full clean `./build_dll.sh` verified; new engine-suite smoke test confirms the port's own cascade
  (which now includes this fix from the start) is genuinely live.



### Task 8 (spec §3 item 4): `AlphaFileWriter.h`

**Files:** `tools/market_data_replay/AlphaFileWriter.h` (new), `tools/market_data_replay/test_alpha_file_writer.cpp` (new)

- [x] **Step 1:** Mirrored `ContextFileWriter.h`'s established precedent exactly — a deliberate,
  byte-faithful standalone duplicate of `LBRFileManager::LogAlphaUnlocked()`'s real wire-format
  write path (`src/LBRFileManager.cpp`, confirmed by direct read, not assumed): "LBRN" magic
  header, a size-prefixed `FileMetadata` record (`symbol`, `timeframe="ALPHA_v2.5_TRAINING_EVENT"`
  — confirmed identical to production's own real `Open()` call, `MTS::Schema::Contract::kSchemaVersion`),
  then one size-prefixed `TrainingEvent` FlatBuffer record per `LogAlpha()` call (8192-byte
  stack-buffer combined-write fast path + flush-every-512-records cadence, both copied verbatim
  from `WriteToStream()`/`FlushStreamIfNeeded()`). `LBRFileManager.cpp` still cannot link into this
  standalone tool (`windows.h` via `MindfulTrader_Precompiled.h` → `sierrachart.h`, same blocker
  `ContextFileWriter.h` already documented).
- [x] **Step 2:** Native test (`test_alpha_file_writer.cpp`, a "new minimal reader" per this
  step's own alternative wording — `tools/context_pipeline/context_reader.h` is `.context`-MO+SS-
  specific and doesn't apply to a single-table `.alpha` stream): writes a `TrainingEventT` with
  hand-set `bar_index`/`timestamp_us`/`open`/`high`/`low`/`close`/`volume`/`hmm_state`, reads it
  back via `flatbuffers::GetRoot<MTS::Training::TrainingEvent>()` after manually skipping the
  magic header + `FileMetadata` record, and confirms every field round-trips byte-faithfully.
  Also confirms `sequence_id` is writer-owned and monotonic across 2 records in the same run
  (matching `LogAlphaUnlocked()`'s own `event.sequence_id = sequence_id` overwrite contract), and
  that `LogAlpha()` before `Open()`/after `Close()` is a safe no-op (matching production's own
  `m_isOpen` guard). 14/14 checks pass. Full clean `./build_dll.sh --no-clean` confirmed unaffected
  (new file lives only under `tools/`, not part of `CMakeLists.txt`/`build_dll.sh`, per this tool
  family's own standing convention).

### Task 9 (spec §3 item 3): Wire the ported `GetTrainingEventT()` subset

**Files:** `tools/market_data_replay/MarketDataReplayEngine.h` (extend) or a new `AlphaEventBuilder.h`

**Scope clarification (2026-09-17, direct read of `../schema/mts_schema.fbs` + every real
`AddToTrainingEventFB`/`GetTickCompanionValues` body, not inferred from the spec table's own
shorthand):** the spec §3 item 3 table's "already portable, call as-is" language describes those
calls' *signatures* (no `sc` parameter) — it does NOT mean `IndicatorManager`/`ContextManager`/
`InferenceManager` can literally be linked into this standalone tool. Confirmed by direct probe:
`#include "IndicatorManager.h"` alone fails to compile standalone (`fatal error: sdkddkver.h: No
such file or directory`) — these are live ACSIL singletons, not portable classes. This task
therefore builds an EQUIVALENT `TrainingEventT` assembly using this engine's OWN already-computed
state, mapped into the same schema fields — not a literal reuse of the live call graph. Fields
genuinely in scope vs. explicitly out of scope for this `PRIMARY_TRIGGER_MASK`-only initiative:

- **In scope (real values from this engine's own state):** `bar_index`/`timestamp_us`/`open`/
  `high`/`low`/`close`/`volume` (already tracked); all 17 `PRIMARY_TRIGGER_MASK` `IndicatorState`
  fields (`kangaroo_tail`, `turtle_soup`, `momentum_pinball`, `elder_breakout`, `nr7`, `long_imp`,
  `interm_imp`, `rsi`, `interm_stochastic`, `atr_proximity`, `ema_proximity`,
  `raschke_strategy_setup`, `raschke_tactical_trigger`, `structure_test`, `volume_signal`,
  `daily_bias` — via this engine's own `Get*Result()` accessors from Task 7); top-level `side`
  (always FLAT, per operator directive); `event.observation` (this engine's own `m_obs`, a direct,
  genuine reuse — `ContextManager::AddToTrainingEventFB`'s real body confirms `event.observation`
  is populated from nothing but the same scaled 18D vector this tool already builds); `prev_day_high`/
  `prev_day_low` (already tracked, `m_prevDayHigh`/`m_prevDayLow`); `prev_high`/`prev_low` (the
  immediately-prior TS3 bar's own high/low — already tracked, `m_prevTs3BarHigh`/`m_prevTs3BarLow`,
  confirmed via `GetTickCompanionValues()`'s own comment: *"prevHigh/prevLow = previous completed
  15-minute bar extremes"*); `model_confidence = 0.0f` (spec §5 item 4's resolved decision).
- **Needs retention (real math already done, just discarded today — same "residual plumbing gap"
  class as Task 10's own item 5):** `prev_four_bar_high`/`prev_four_bar_low` map to TurtleSoup's
  own `fourDayHigh`/`fourDayLow` locals in `OnTs3BarClose()` (confirmed via `GetTickCompanionValues()`:
  `prevFourBarHigh`/`Low` = `m_shortTermExtremes.prevFourBarHigh/Low`, itself populated by the same
  `SetPrevFourBarExtremes()` `BoundedBarRing.h` already cites for TurtleSoup's real 20-bar window)
  — currently a transient local, must become a retained member.
- **Out of scope, explicit documented sentinel required (genuinely requires live-only
  subsystems this tool does not and should not replicate):** `hmm_state`/`market_climate`/all
  `regime_prob_*`/`regime_confidence`/`regime_entropy`/`regime_transition_risk`/
  `regime_expected_duration`/`regime_variance_ratio`/`regime_duration_ratio` (real source:
  `InferenceManager::Instance().AddToTrainingEventFB`, requires a live-trained HMM inference this
  offline tool has no equivalent of); `event.asymmetry_context`/`dist_day_high`/`dist_day_low`/
  `dist_four_bar_high`/`dist_four_bar_low`/`dist_ema_13`/`volatility`/`efficiency`/`rel_range`/
  `velocity`/`regime_tenure` (real source: `ContextManager::AddToTrainingEventFB`, requires
  `StructureEngine`/`m_latestInstitutionalMetrics`/`PositionManager`-derived state, none of which
  this tool replicates); `event.features` (real source: `SyncFeatureVector`, reads
  `ContextManager::Instance().GetStatisticalContext()`/`GetNormalizedAnchors()`); all
  `SECONDARY_TRIGGER_MASK` `IndicatorState` fields (`corr_es_*`, `long_macd*`, `price_metrics`,
  `oscillator_310`, `zn_trend`/`dx_trend`, `daily_bias_enum`, `time_of_day`, `nh_nl_signal`, etc.);
  `close_percentile`/`volume_ratio_percent`/`volume_imbalance` (real sources —
  `PriceMetricsIndicator`/`VolumeIndicator` — not yet ported, and not part of the 17-key scope);
  Section 12-15 outcome/label fields (training-labeler-only, never populated by `GetTrainingEventT()`
  itself in production either).

**Institutional DOD requirements (spec §11 — apply from the start, not as a later cleanup pass):**
pool a single `TrainingEventT` scratch member (+ its nested `indicators`/`observation`
`unique_ptr`s, each allocated once and reused) — never construct a fresh `TrainingEventT` per
emission; every out-of-scope field must be explicitly (re-)assigned its documented sentinel on
every call, since a pooled object silently carries forward a prior call's value otherwise —
exactly the hazard production's own `GetTrainingEventT()` comment warns about.

- [x] **Step 0 (retention, prerequisite for `prev_four_bar_high/low`):** Retained TurtleSoup's
  `fourDayHigh`/`fourDayLow` locals (`OnTs3BarClose()`) as persistent `float` members
  (`m_prevFourBarHigh`/`m_prevFourBarLow`), updated the same tick they're computed today (once
  `turtleSoupWindowReady`).
- [x] **Step 1:** Implemented the 4 extract-to-pure substitutions (`barIndex`, `timestamp_us`
  epoch-microseconds, current-bar OHLC, volume) as explicit parameters of the new
  `BuildTrainingEventT()` method — the caller (Task 11's CLI, not yet built) supplies them from its
  own tick-processing loop, matching spec §3 item 3's own framing exactly.
- [x] **Step 2:** Implemented `BuildTrainingEventT()` (`MarketDataReplayEngine.h`) per the in-scope/
  out-of-scope split above: all 17 `PRIMARY_TRIGGER_MASK` `IndicatorState` fields from this
  engine's own `Get*Result()` accessors, `event.observation` set via direct struct-copy from
  `m_obs`, `WriteTrainingRootSharedFields()` called with the real retained fields (`side`,
  `prev_high`/`prev_low` from `m_prevTs3BarHigh`/`Low`, `prev_day_high`/`low` from
  `m_prevDayHigh`/`Low`, `prev_four_bar_high`/`low` from Step 0's new members) plus explicit `0`/
  `0.0f` sentinels for the 3 genuinely out-of-scope shared-root fields (`market_symbol`,
  `overnight_exit`, `nh_nl_daily`), every other out-of-scope field (HMM/regime, `asymmetry_context`,
  `dist_*`, `features`, all `SECONDARY_TRIGGER_MASK` `IndicatorState` fields) left untouched at
  `TrainingEventT`'s own true zero-init default (verified never touched anywhere else in this
  method, so never stale from a prior pooled call either).
  **New tracked gap, discovered during this step (not silently left unstated):** `long_imp`/
  `interm_imp`/`impulse_run_length` need the FULL `ImpulseEnum` from `ComputeImpulse()`
  (`IndicatorComputations.h`), which itself needs a dedicated "Impulse ATR" per timeframe
  (`TripleScreen1.cpp`'s `Subgraph_ATR`/`TripleScreen2.cpp`'s `Array_ImpulseATR` — confirmed via
  direct read of both real call sites — neither is this engine's existing `atr10`/relative-range
  ATR) that this engine does not yet track; using the raw 3-color bucket this engine already has
  (`GetLongImpulseColor()`/`GetIntermImpulseColor()`) directly would silently misrepresent the
  field (color ≠ the refined 8-state enum the wire field expects), so both fields are explicitly
  set to `ImpulseEnum::UNDEFINED` (its own documented "not enough data" sentinel) instead —
  honest, not a guessed mapping. Follow-on work, not blocking this task: add a dedicated
  TS1/TS2 "Impulse ATR" `WilderAtr` instance each and call `ComputeImpulse()` properly.
- [x] **Step 3:** Set `model_confidence = 0.0f` unconditionally (spec §5 item 4's resolved
  decision — this is the only value production itself ever produces today).
- [x] **Step 4:** Native test (`test_market_data_replay_engine.cpp`, `task9_*`): warms up the
  engine (same 200-bar varied-walk fixture as the `RASCHKE_STRATEGY_SETUP` smoke test), calls
  `BuildTrainingEventT()`, and confirms: the 4 extract-to-pure fields round-trip exactly; all 12
  spot-checked `IndicatorState` fields match the engine's own live `Get*Result()` accessors;
  `event.observation` matches `m_obs`; `side`/`model_confidence` hold their documented values;
  `prev_day_high/low`/`prev_high/low`/`prev_four_bar_high/low` are genuinely non-zero after
  warmup (proving real retention, not a default); every out-of-scope int/float field holds its
  `0`/`0.0f` sentinel; `long_imp`/`interm_imp`/`impulse_run_length` hold their documented
  `UNDEFINED`/`0` sentinel; a second `BuildTrainingEventT()` call reuses the SAME
  `indicators`/`observation` pointer (proving the DOD pooling requirement — spec §11 item 1 — is
  real, not just asserted) while still correctly overwriting `bar_index`. 24 new checks, all pass
  (135 total in this suite). Full clean `./build_dll.sh` re-verified (new
  `#include "generated/training_shared_writers_generated.h"` — no live-code files touched).

### Task 10 (spec §5 item 5): Retain raw pre-scaling values + build `RiskGateContext`-equivalent

**Files:** `tools/market_data_replay/MarketDataReplayEngine.h` (extend)

**Scope correction (2026-09-17, direct read of `ContextManager::BuildRiskGateContext()`
(`src/ContextManager.cpp:851`) + every one of its 17 fields' own real assignment site, not
assumed from the spec's own earlier pass):** the spec's "residual plumbing gap... does not
currently retain them afterward" framing turns out to overstate the work needed. Confirmed via
direct read: `LocalRiskContext.hurstExponent`/`meanRevZ`/`fisherInfo`/`amihudIlliquidity`/
`spreadStress` are all assigned from `obs[OBS_X] = m_observationData.x()` in production — i.e.
the RAW (pre-`FeatureScaler`), not the scaled, `ObservationData`. This engine's own `m_obs` is
*already* that same raw vector (confirmed: `m_featureScaler.UpdateAndNormalize(rawObs)` produces
a separate, transient `currentObs` used ONLY for the Mahalanobis gate check — never written back
into `m_obs`). Since every one of those 5 dims is written into `m_obs` via `m_obs.mutate_x(...)`
*before* the enclosing computation block's local variable goes out of scope, **`m_obs` already
retains exactly what `BuildRiskGateContext()`'s equivalent needs — no new member is needed for
these 5 fields, just a read of `m_obs` at build time.** Likewise `pareto_tail_alpha` is literally
a cached copy of the same Hill-alpha value this engine already computes as `tail_index`
(confirmed: production's own `m_cachedHillAlpha.store(obs[OBS_TAIL_INDEX], ...)`) — another
direct `m_obs` read, not a new computation. This shrinks Task 10 to: 6 direct `m_obs` reads, 1
`FeatureScaler.warmedUp` read (already a member), the current tick's own timestamp, 1 genuine new
gap (`fractal_dim`'s short-window decoupling), and 8 fields with no equivalent computation in this
engine at all (out of scope, explicit sentinel).

- **In scope, zero new computation (read directly from `m_obs`/`m_featureScaler` at build time):**
  `hurst_exponent`, `mean_rev_z`, `fisher_info`, `amihud_illiquidity`, `spread_stress` (=
  this engine's `liq_fragility`), `pareto_tail_alpha` (= this engine's `tail_index`), `is_valid`
  (= `m_featureScaler.warmedUp`), `snapshot_timestamp_us` (the current tick's own timestamp —
  production's real wall-clock `system_clock::now()` has no offline equivalent; the tick
  timestamp is this tool's own established substitute elsewhere).
- **Genuine new gap, NOT silently faked (same "documented sentinel over a plausible-looking wrong
  value" principle as Task 9's `ImpulseEnum::UNDEFINED` decision):** `fractal_dim` — production's
  real `LocalRiskContext.fractalDim` is `m_fractalDimShortRaw`, a SEPARATE, deliberately
  DECOUPLED short-window Sevcik computation (`ContextManager.cpp`'s own comment: *"that dim is now
  the HMM-bound 400-bar window... the gate this feeds... stays on the original short window"*) —
  NOT the same quantity as this engine's own `fractal_dim` dim (already the 400-bar HMM-bound
  window, per the `2026-08-28` window-widening work). Using this engine's `m_obs.fractal_dim()`
  here would silently substitute the wrong window. Left at `RiskGateContextT`'s own documented
  default (`1.5f`) instead, with a follow-up noted: add a dedicated short-window
  `SevcikFractalDimension` call, decoupled from the existing 400-bar one, mirroring
  `ContextManager`'s own `SetFractalDimShort()` split.
- **Out of scope, explicit documented sentinel required (genuinely requires `StructureEngine`/
  `TailRiskEngine`/`MarketClimateIndicator`/the Layer-B rolling-percentile subsystem, none
  replicated by this engine):** `shannon_flow_entropy`(`0.0f`)/`shannon_efficiency`(`0.5f`)/
  `taleb_kurtosis`(`0.0f`, NOT the same quantity as this engine's own `fast_taleb_kurtosis`)/
  `taleb_skewness`(`0.0f`)/`elder_chandelier_atr`(`0.0f`)/`regime_duration`(`0`)/
  `amihud_percentile`(`0.5f`, a session-aware rolling percentile of `amihud_illiquidity` this
  engine doesn't track) — all `LocalRiskContext.h`'s own documented struct defaults, not arbitrary
  zeros. **`raschke_burst` needs care**: `LocalRiskContext.h`'s own documented neutral is `0.0f`
  (Poisson-neutral, Goh-Barabási bounded scale) but `RiskGateContextT`'s OWN generated default
  member initializer is `1.0f` (a stale mismatch between the two structs, never surfaced in
  production because `BuildRiskGateContext()` always explicitly assigns it) — must be set
  explicitly to `0.0f`, not left at the wire struct's own default, to match the semantically
  correct neutral value.

- [x] **Step 1:** Implemented `BuildRiskGateContextT(int64_t timestampUs)` (`MarketDataReplayEngine.h`,
  pooled via `m_riskGateContextScratch`, same DOD rationale as `BuildTrainingEventT()`) reading the
  6 in-scope `m_obs`/`m_featureScaler` fields directly (no new member needed, per the scope
  correction above), `fractal_dim` left at its documented `1.5f` default (genuine gap, not faked),
  and every out-of-scope field set to its own `LocalRiskContext.h`-documented default explicitly
  (including the `raschke_burst` correction — `0.0f`, not `RiskGateContextT`'s own mismatched
  `1.0f` wire default).
- [x] **Step 2:** Native tests (`test_market_data_replay_engine.cpp`, `task10_*`): confirm all 6
  in-scope fields match `m_obs`'s own live accessors for the same warmed-up tick;
  `snapshot_timestamp_us` matches the passed-in timestamp; `fractal_dim` holds its documented
  sentinel (not this engine's own 400-bar `m_obs.fractal_dim()`, which would silently be the wrong
  window); every out-of-scope field (including the `raschke_burst` wire-default mismatch) holds
  `LocalRiskContext.h`'s own documented default; a separate fresh-engine test confirms `is_valid`
  is genuinely `false` before `FeatureScaler` warms up (not hardcoded true). 11 new checks, all
  pass (146 total in this suite). Full clean `./build_dll.sh` re-verified.

### Task 11 (spec §7 item 2): New CLI driver `MarketDataReplayContext.cpp`

**Files:** `tools/market_data_replay/MarketDataReplayContext.cpp` (new)

- [x] **Step 1:** CLI flags implemented: `--ticks-parquet PATH`, `--output PATH` (single base
  path — this tool derives `<path>.context`/`<path>.alpha` internally, matching
  `LBRFileManager::Open()`'s own convention, spec §5 item 6's resolved decision), `--symbol`
  (default `MES`), `--emit-context`/`--no-emit-context` (default on), `--emit-alpha`/
  `--no-emit-alpha` (default off), `--max-rss-mb` (default 3072), `--max-ticks` (default
  unlimited). Refuses to run if both `--emit-context`/`--emit-alpha` end up off.
- [x] **Step 2:** Implemented `AlphaLocksPass()`: Lock A (new public `IsFeatureScalerWarmedUp()`
  accessor), Lock B (`GetTs3BarsClosed() >= 200`, matching production's real
  `m_warmupBarCount>=200`, + `GetRsiTopResult() != RSI::UNDEFINED`, matching production's real
  `GetValue<IndicatorKey::RSI>() != 0`), Lock D/E (new public `IsAllDimsReady()` accessor —
  **documented simplification**: production's real Lock D/E are TIME-based staleness checks
  (6h/3h max age since last update), meaningless for a continuous, gap-free offline tick replay
  where no live disconnect can occur; this tool's own substitute is the engine's existing
  per-timeframe bar-count sufficiency check, already relied on for trigger 1 — a deliberate,
  explicitly-commented simplification, not a silent gap). Lock C is entirely absent (never
  enforced, matching production's own explicit rationale). Trigger 2 fires when all 4 locks pass
  AND `ConsumePatternDirtyMask() != 0`.
  **New engine additions needed for this step**: `MarketDataReplayEngine.h` had no live (still-
  forming) bar OPEN price tracked (only high/low/close/volume) — added `m_ts3LiveOpen` + public
  `GetTs3LiveOpen/High/Low/Close/Volume()` accessors (TS3 is the timeframe all 17
  `PRIMARY_TRIGGER_MASK` detectors are evaluated against, the direct equivalent of whichever chart
  `EventDataCollectorStudy.cpp` is really attached to). `BuildTrainingEventT()`'s return type
  changed from `const TrainingEventT&` to `TrainingEventT&` (non-const) so
  `AlphaFileWriter::LogAlpha()` can mutate `sequence_id` on the pooled object directly, matching
  production's own `LogAlphaUnlocked(TrainingEventT&)` contract.
- [x] **Step 3:** `--emit-context`+trigger-1-pass writes via `ContextFileWriter::LogContext()`
  (engine's own `ObservationData`, an empty/zero-init `AsymmetryContext` — out of scope per spec
  §3 item 3's disposition table, `BuildRiskGateContextT()`'s own real risk-gate context); `--emit-
  alpha`+trigger-2-pass writes via `AlphaFileWriter::LogAlpha()` (`BuildTrainingEventT()`'s pooled
  event). Both triggers evaluated independently every tick, never unified, per spec §2.
- [x] **Step 4:** `ToolProgressLogger` wired (`"market_data_replay_context"`), progress logged
  every 5M ticks + memory-budget check, contract-roll events logged, final summary logs ticks
  processed + both output record counts.

**Build + smoke test (2026-09-17):** compiles cleanly
(`mamba run -n mts g++ -O2 -std=c++17 ... -o tools/bin/market_data_replay_context`). A 2M-real-tick
smoke test ran clean (29.6s, exit 0) but produced 0 records in either file — expected, not a bug:
`AllDimsReady()`'s largest requirement (TS2's 400-bar fractal_dim window) needs ~400 hours of real
session time, far more than 2M ticks covers at real MES tick density. A 100M-tick run was launched
to confirm non-zero output eventually appears (in progress at last check: 10M ticks/238s, RSS
138-162MB, well within the 3072MB budget) — full result to be recorded once complete; this is
empirical confirmation (Task 12's own domain), not a blocker for Task 11's own completion, since
every component this CLI wires (`BuildTrainingEventT`/`BuildRiskGateContextT`/`AlphaFileWriter`/
`ContextFileWriter`) already has passing unit-level native test coverage (146 checks total).

### Task 12 (spec §0): Empirical validation — partially blocked

**Files:** none yet

- [x] **Step 1:** Ran to completion against the real 471.9M-tick `mes_ticks.parquet` (capped at
  100M ticks, ~50 min, RSS bounded 242MB→633MB, well under `--max-rss-mb`'s 3072 default). Produced
  well-formed, non-trivial output: 5,483,891 `.context` records, 21,362 `.alpha` records (log:
  `tools/log/market_data_replay_context.log`, archive:
  `tools/output/market_data_replay_context_20260917_091253.txt`). `build_directional_alpha.py`
  consumption checked directly: it parsed the `.alpha` file, loaded the HMM model, and warmed up
  the regime engine without error, then stopped at `_verify_hmm_model_provenance()` demanding a
  `<file>.alpha.manifest.json` stamp from `materialize_hmm_features.py` — a real, orthogonal
  pipeline prerequisite (not a defect in this tool's output), not yet run. Full drop-in parity
  therefore still open pending that separate step. Output files handed off to `lbrnet` for their
  own code-path/refactoring use (not as a correctness oracle, see spec §13):
  `lbrnet/data/raw/offline_replay_smoke_100m.context`/`.alpha`.
- [ ] **Step 2 — BLOCKED, same blocker as the original `.context` generator's own Task 12**: byte/
  numeric-parity validation of the new RSI/ATR/MACD/Stochastic implementations against genuine
  SC-computed values requires a **current-schema (v240) real SC-collected `.alpha`/`.context`
  file** — none exists on this machine today (checked this session: no `.alpha`/`.context` files
  found anywhere under `lbrnet/data/raw/`; the prior known file,
  `lbrnet/data/raw/event_data.context`, is schema_version 230 and is hard-rejected by the current
  reader). **Cannot be completed until a fresh SC collection run produces a current-schema
  comparison file** — flag this dependency early, don't discover it late.

### Task 13 (spec §7 item 2): Fix `tools/README.md`

**Files:** `tools/README.md`

- [x] **Step 1:** Corrected the `market_data_replay/` section to describe `MarketDataReplay.cpp`'s
  real current behavior (direct-to-Parquet dim-selection export) and added
  `MarketDataReplayContext.cpp` as its own entry, distinguishing the two tools' purposes clearly.

---

## Not yet scoped (deferred, per the approved scope decision)

- `SECONDARY_TRIGGER_MASK`'s ~38 remaining `IndicatorKey`s — explicitly deferred (spec §7 item 1).
- The 8-file `HMMStateEnum` → `RegimeManager` call-site migration and `ModelManifest` control-plane
  broadcast — unrelated initiative, already tracked in `docs/HMM_REGIME_MANAGER_COORDINATION.md`.
- ~~The `EventDataCollectorStudy.cpp:786` `model_confidence` live-system bug~~ — **FIXED
  2026-09-17** (`docs/HMM_REGIME_MANAGER_COORDINATION.md` Entry 9; spec §10 finding 5). Now reads
  `InferenceManager::Instance().Prediction()->Confidence()` instead of the dead `TradeSignalManager`
  path.
