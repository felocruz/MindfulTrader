# Market Data Replay (`.context` Generator) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build `tools/market_data_replay/`, a standalone C++ CLI tool that reconstructs genuine
`.context` training files directly from `lbrnet/data/raw/mes_ticks.parquet`, without running Sierra
Chart in replay mode — enabling Feature Saliency EM (Elite Feature Set Curation Phase 2) without a
multi-day SC replay.

**Architecture:** Vertical-slice-first, one dim-group per task, each independently committable and
natively testable, matching this repo's own "smallest real end-to-end slice" discipline. Tasks 1-7
(incl. 4b) build the raw 18D `ObservationData` construction plus `SystemState`'s
`bars_since_last_update`; Task 8 wires the real `FeatureScaler`/`ObservationTriggerGate` gate; Task 9
is the CLI driver; Tasks 10-12 are validation, warm-up gating, and edge-case hardening. No task
calls `ContextManager`/`ActivityClockManager` (both transitively `#include "sierrachart.h"`,
confirmed not includable in this standalone build) — every task calls the pure leaf header/engine
class directly.

**Tech Stack:** C++17, native `check()`/`g_failures`/`ALL PASS` unit tests (no GoogleTest/CMake),
Arrow/Parquet (`mamba run -n mts`), FlatBuffers-generated headers (read-only — no schema changes).

**Spec:** `docs/superpowers/specs/2026-09-08-offline-context-generator-spec.md` — read in full before
starting; every design decision below traces back to a section there (cited per task).

## Institutional DOD (Data-Oriented Design) Mandate — non-negotiable, applies to every task

This tool processes 471.9M+ real ticks per run. Every task in this plan must follow the DOD
practices this repo already established and validated the hard way — `whole_vector_redundancy_
eval.cpp`'s own header comment records that an earlier tool in this same family took down a session
with a real OOM incident from violating exactly these rules (2026-09-03). **This is not a style
preference — a violation here is a correctness/reliability bug, not a nitpick, and a task is not
done just because its test passes if it violates one of these:**

1. **Zero heap allocation in the per-tick hot loop** (`OnTick()` and everything it calls,
   transitively). Every piece of state must be a fixed-size `RingBuffer<T,N>`/`std::array`/plain
   scalar, sized at compile time or reserved once at construction — never a
   `std::vector`/`std::string`/heap allocation inside a function called once per tick.
2. **Bounded memory regardless of tick volume.** No task may retain a per-tick value across the
   full stream (the exact trap behind the 2026-09-03 OOM: `D × N × 8` bytes for `D` dims over
   `N=471.9M` ticks is 37+GB for raw series alone). State size must be a function of window length,
   never of stream length.
3. **Reuse the real production leaf classes/headers directly — never re-derive a formula.** Every
   dim's math already lives in a natively-tested pure header (spec §1/§2); copying a formula instead
   of including the header is both a production-drift risk and, in its own right, usually a DOD
   violation — these headers are already written to the fixed-window/SoA discipline this mandate
   requires.
4. **Columnar/SoA construction wherever batching is unavoidable** (any future Arrow/Parquet path
   this tool's output feeds) — one typed builder/buffer per column, never a row-of-structs
   intermediate, matching `context_to_parquet.cpp`'s/`scid_to_ticks_parquet.cpp`'s own established
   `ChunkBuffers` pattern.
5. **`ToolProgressLogger::CheckMemoryBudget()`/`--max-rss-mb` is mandatory, not optional** — even a
   design that's bounded by construction (rule 2) must still be instrumented to catch a future
   regression, per this tool family's own established convention.

**Tasks 1, 2, 3, 4, 4b, 5, 6, 8, 10, and 11 all touch `OnTick()` and must satisfy this checklist
before being considered done, in addition to each task's own listed test.** Verify rules 1-2 by
reading the diff, not by trusting a green test alone — a test can pass while still allocating on
every tick.

## FlatBuffers Generated-Header Policy — use them as-is, fix upstream if inefficient

Every task that touches the wire types (`MTS::Schema::ObservationData`, `AsymmetryContext`,
`ToObservationArray()`, `MakeObservationData()`) must use the existing generated headers exactly as
they are — `include/generated/mts_schema_generated.h` (raw `flatc` output) and `include/generated/
mts_schema_contract_generated.h` (the reflection-derived contract helpers, produced by `schema/
scripts/generate_contract_header.py`, invoked from `schema/regenerate_schema.sh`). Both are already
confirmed pure/standard-layout/fixed-size (spec §1) — this plan needs no new schema fields and no
new generated-header content, only consumption.

**Never hand-edit either generated file directly** — both are regenerated on demand and any local
patch is silently lost the next time `bash /home/rcruz/devel/VSCode/scripts/regenerate_schema.sh`
runs (the canonical entrypoint per `CLAUDE.md`'s hard requirement — a thin wrapper around `schema/
regenerate_schema.sh`, which calls `flatc` for the raw schema headers and `generate_contract_header.py`
for the contract header).

**If any task finds a generated header genuinely inefficient or missing a needed institutional
helper** (e.g. an accessor pattern that isn't fixed-size/branch-free where this tool's hot loop needs
one to be, or a contract helper this tool needs that doesn't exist yet) — **the fix belongs upstream,
not in this repo**: `../schema/mts_schema.fbs` (schema source), `../schema/regenerate_schema.sh`
(`flatc` invocation + deploy step), and/or `../schema/scripts/generate_contract_header.py` (the
contract-header generator itself). Do not work around an inefficient generated header with a
MindfulTrader-local shim or a hand-patched copy — that is exactly the "two silently-disagreeing
implementations" failure class this repo's own conventions exist to prevent (spec §0's reusability
mandate cites the same class of bug: the `GetTrainingEventT` `close_percentile` dead-write incident).
Escalate as a new, separately-tracked spec/plan under `schema/`'s own governance, not as an
in-flight fix inside this plan's tasks.

## Logging Policy — smart, not flooded, never empty

A 471.9M-tick run must produce a log an operator can actually read (not scroll past) AND must never
end with zero evidence of what happened. Three concrete, already-established patterns to reuse, not
invent from scratch:

1. **Streaming progress, bounded line count** — mirror `whole_vector_redundancy_eval.cpp`'s own
   convention exactly: a progress line every `kProgressEveryNTicks` (5,000,000) via
   `ToolProgressLogger::LogProgress(ticksProcessed, kTotalTicksEstimate)` (also where
   `CheckMemoryBudget()` gets called, mandate rule 5), plus a coarser interim summary every
   `kInterimReportEveryNTicks` (50,000,000). Over the full 471.9M-tick stream this is ~94 progress
   lines + ~9 interim summaries — enough to prove liveness and catch a stall, not a flood.
2. **Rate-limited logging for repeated/noisy events** — port `ContextManager.cpp:32`'s
   `ShouldSampleLog(count, firstN, everyN)` verbatim (trivial, 3 lines, zero dependencies: log
   always for the first `firstN` occurrences of a thing, then every `everyN`-th after that). Use it
   for anything that can repeat thousands of times in a row and would otherwise flood the log:
   skipped bad ticks (Task 11), warm-up-not-ready checks (Task 10), NaN carry-forward events. Do not
   invent a different rate-limiting scheme — this one is already the established convention
   (`ContextManager.cpp`'s own 5 call sites) and it's small enough to just copy.
3. **Always-log, never-suppress events (the anti-"empty log" guarantee)**: file open (with the
   `.alpha`/`.imbalance.context`-will-be-near-empty note, spec §3h), each contract-roll boundary
   (`isNewContract`, rare and meaningful — never rate-limited), warm-up completion (once, the
   transition from "not ready" to "ready"), and — **mandatory, every run, success or failure** — a
   final summary line on close: ticks processed, `.context` records written, significant-change
   rate, and (once Task 10/11 exist) warm-up/edge-case-guard trigger counts. Mirrors
   `EventDataCollectorStudy.cpp`'s own disarm-time diagnostic dump (event count, lock block counts,
   breadcrumb) — a run that produces zero output lines on exit is itself a bug to fix, not an
   acceptable quiet success.

Every log call still routes through `ToolProgressLogger::Log()` (Global Constraints, unchanged) —
this section governs *cadence*, not the mechanism.

## Global Constraints

- Work directly on `master`, no feature branches or worktrees (standing repo convention).
- TDD throughout: write the failing test first, watch it fail, then write the minimum code to pass.
- Never add this tool to `CMakeLists.txt`/`build_dll.sh` — standalone `tools/` build only (spec §3i):
  ```bash
  mamba run -n mts g++ -O2 -std=c++17 -Iinclude \
    $(mamba run -n mts pkg-config --cflags arrow parquet) \
    tools/market_data_replay/MarketDataReplay.cpp \
    $(mamba run -n mts pkg-config --libs arrow parquet) \
    -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
    -o tools/bin/market_data_replay
  ```
- Native tests build/run via:
  ```bash
  g++ -std=c++17 -Iinclude -Iinclude/generated tools/market_data_replay/test_market_data_replay_engine.cpp -o /tmp/mdr_test && /tmp/mdr_test
  ```
- `MarketDataReplayEngine.h` must stay parquet/Arrow/CLI-free (spec §3c) — testable on synthetic
  tick sequences alone. All parquet I/O lives in `MarketDataReplay.cpp` only.
- Per spec §0's priority pivot: `AsymmetryContext` is a placeholder (`MTS::Schema::
  AsymmetryContext{}`), `RiskGateContext` is `nullptr` — do not wire either in this plan. A future
  plan handles them if/when a Transformer-training or execution-sim-parity consumer is scoped.
- Per spec §2: `fast_mean_rev_z` (dim 17) must stay at its zero sentinel — do not wire
  `ActivityClockMeanReversion.h` into it under any task here.
- Route all tool output through `ToolProgressLogger::Log()` (repo convention) — never bare `printf`.

---

### Task 1: `MarketDataReplayEngine` skeleton — 3× `TickBarAggregator` wiring only

**Files:**
- Create: `tools/market_data_replay/MarketDataReplayEngine.h`
- Create: `tools/market_data_replay/test_market_data_replay_engine.cpp`

**Interfaces:**
- Consumes: `include/TickBarAggregator.h` (`tba::TickBarAggregator`, unchanged), `include/
  generated/mts_schema_generated.h` (`MTS::Schema::ObservationData`, tool-owned instance).
- Produces: `class MarketDataReplayEngine` with `bool OnTick(int64_t timestamp_us, double price,
  int64_t volume, int64_t askVolume, int64_t bidVolume)` (returns `false` always in this task — no
  gate yet), `void Flush()`, and `const MTS::Schema::ObservationData& GetObservation() const`
  (returns `m_obs` by const ref — Task 9's CLI driver needs this to pass into `LogContext()`; not
  optional scaffolding, the CLI cannot read the built vector without it).

- [x] **Step 1: Write the failing test** — construct the engine, feed a synthetic tick sequence
  spanning one TS3 (15-min) bar boundary, assert (via a test-only accessor or logged callback) that
  each of the 3 internal `TickBarAggregator` instances receives every tick and closes bars at the
  expected boundaries (mirrors `tests/cpp/test_tick_bar_aggregator.cpp`'s own fixtures/style).
- [x] **Step 2: Run to verify it fails** (class doesn't exist yet / bar-close hooks unwired).
- [x] **Step 3: Implement** — `MarketDataReplayEngine` owns 3× `tba::TickBarAggregator` (240m/60m/15m,
  `CME_ES_SESSION_START_SECS`), one tool-owned `MTS::Schema::ObservationData m_obs{}`; `OnTick()`
  forwards to all 3 aggregators' `OnTick()`, `Flush()` forwards to all 3 `Flush()`. No dim math yet.
  **DOD checklist applies from this task onward** (see mandate above) — `m_obs` is a fixed-size
  struct, the 3 aggregators are fixed members, nothing heap-allocated per tick.
- [x] **Step 4: Run to verify it passes.** — 11/11 checks pass, `ALL PASS`.

---

### Task 2: Tick-level dims — `burstiness_index`, `tail_index`, `lempel_ziv`

**Files:** Modify `MarketDataReplayEngine.h`, append to the test file.

**Interfaces:**
- Consumes: `include/EventVelocityEngine.h` (`CalculateBurstinessIndex`), `include/TailRiskEngine.h`,
  `include/InformationEngine.h` — all confirmed pure (spec §1/§3b).

- [x] **Step 1: Write the failing test** — synthetic tick sequence with a known burst pattern (a
  long quiet run then a rapid cluster) and a known price-change sequence; assert `m_obs.burstiness_
  index()`/`tail_index()`/`lempel_ziv()` move in the expected direction after enough samples.
- [x] **Step 2: Run to verify it fails.**
- [x] **Step 3: Implement** — own a `RingBuffer<uint64_t,100>` (tick timestamps) +
  one `TailRiskEngine` + one `InformationEngine` instance inside the engine; in `OnTick()`, push the
  timestamp and call `CalculateBurstinessIndex`, feed **both** `TailRiskEngine` and
  `InformationEngine` only on a genuine price change (corrected from an earlier draft of this task
  that assumed `TailRiskEngine` was fed every tick — re-verified against `src/ContextManager.cpp:
  165-166` and confirmed institutionally correct independent of what the existing code does: a
  repeated-price tick has a `log(1)=0` return, and feeding null returns into a Hill-estimator/
  entropy/LZ-complexity calculator contaminates the tail/information estimate — standard event-time
  sampling practice, not a "production convention" to imitate) — write results via
  `m_obs.mutate_burstiness_index(...)` etc.
  **DOD checklist**: `RingBuffer<uint64_t,100>` is fixed-size by construction — confirm
  `TailRiskEngine`/`InformationEngine`'s own internal state is likewise fixed-size (both already
  natively tested as such, spec §1 — this is a re-confirmation, not new design).
- [x] **Step 4: Run to verify it passes.** — 16/16 checks pass, `ALL PASS`. (Two initial test
  failures were the test fixture's own wrong assumptions about the Goh-Barabási `[-1,+1]` bounded
  scale and MAD's breakdown-point robustness at 10 bins, not engine bugs — fixed the tests, not the
  engine; see the test file's own comments.)

---

### Task 3: TS1-owned dims — `log_scale_ratio`, `hurst_exponent`, `fisher_info`

**⚠️ CADENCE FIDELITY BUG FOUND 2026-09-08 (during Task 6): all 3 dims here are bar-GATED
(computed only in the TS1 bar-close callback), but production recomputes them EVERY TICK from a
window ending at the current, still-forming bar (confirmed against `CalculateHurstExponent`'s real
ACSIL indexing, `src/StudyHelperFunctions.cpp:2393`) — no bar-close gate exists in production at
all. This tool is silently losing real intra-bar information for these 3 dims. See spec §2a for
full evidence; remediation tracked as Task 6b below.**

**Files:** Modify `MarketDataReplayEngine.h`, append to the test file.

**Interfaces:** `include/BipowerVariation.h`, `include/DfaHurstExponent.h`, `include/
CarryForwardCalculators.h` (`ComputeFisherInformation`) — per spec §2's ownership table
(`src/TripleScreen1.cpp:628-631`'s real call sites, ported not reinvented).

- [x] **Step 1: Write the failing test** — synthetic TS1 (240m) bar-close sequence with known
  returns; assert the 3 dims match hand-computed expected values within a tight epsilon.
- [x] **Step 2: Run to verify it fails.**
- [x] **Step 3: Implement** — inside the TS1 `TickBarAggregator`'s bar-close callback (Task 1's
  skeleton), maintain the same rolling-return/close-price buffers `TripleScreen1.cpp` does (ported,
  per spec §1a's "prior art" caveat: reuse the *formula*, not `whole_vector_redundancy_eval.cpp`'s
  own non-session-anchored bucketing), call each header's function, write via `mutate_*()`.
  **Institutional-correctness fix made proactively**: `hurst_exponent`'s true "no data yet" neutral
  value is 0.5 (random walk), not `ObservationData`'s zero-initialized 0.0 — matches
  `CalculateHurstExponent`'s own fallback; set explicitly in the constructor rather than left wrong.
  **Documented simplification, not a silent shortcut**: all 3 dims use a FIXED 100-bar window here
  vs. production's adaptive `macro_window_n`/`fisher_window_n` (both driven by ACSIL-coupled
  adaptive-window helpers not yet ported) — matches `whole_vector_redundancy_eval.cpp`'s own
  precedent for this exact simplification; `hurst_exponent`'s 100-bar/minScale=8 pair specifically
  matches `CalculateHurstExponent(sc)`'s own "legacy... standard institutional settings" fixed
  default, not an invented number. Flagged as a real follow-up (porting the adaptive-window state
  machine), not closed out here.
- [x] **Step 4: Run to verify it passes.** — 21/21 checks pass, `ALL PASS`. Exact hand-computed BV/
  DFA-Hurst arithmetic proved impractically tedious to derive by hand — used a bounds/warm-up
  characterization test instead (finite, correctly-clamped, warms up at the right tick count),
  same posture as Task 2's own characterization-style tests.
- [x] **Step 5 (found and fixed while designing Task 4, re-deriving the same ring-buffer pattern
  more carefully): a real off-by-one bug.** The pop condition (`size() == Capacity` with
  `Capacity == targetWindow+1`) popped one element too early on every bar, so the buffer could
  never actually stabilize at the needed window size — `ComputeTs1Dims()`'s warm-up guard never
  passed, and all 3 TS1 dims silently stayed at their cold-start defaults forever. The original
  24 checks still passed because their "finite and in-bounds" assertions are trivially true for the
  defaults too — caught only by hand-tracing the push/pop sequence while designing Task 4, not by
  the tests. **Fixed**: `Capacity = targetWindow + 1` (headroom) with pop firing when
  `size() > targetWindow`, matching `RingBuffer.h`'s own documented convention for this call shape.
  **Added 3 new regression checks** that assert values moved OFF their construction-time defaults
  after warm-up (the bounds checks alone can't distinguish "computed" from "never ran"). All 24
  checks pass post-fix, confirmed via the new regression checks that real computation is now
  happening, not just falling through to defaults.

---

### Task 4: TS2-owned dims — `relative_range`, `fractal_dim`, `log_scale_expansion_ratio`

**⚠️ CADENCE FIDELITY BUG FOUND 2026-09-08 (during Task 6): same bug as Task 3 — all 3 dims here
are bar-GATED but production recomputes them every tick from a window ending at the current,
still-forming TS2 bar (confirmed against `TripleScreen2.cpp:272-334`'s unconditional per-tick
block). See spec §2a; remediation tracked as Task 6b below.**

**Files:** Modify `MarketDataReplayEngine.h`, append to the test file.

**Interfaces:** `include/CarryForwardCalculators.h` (`ComputeRelativeRange`), `include/
SevcikFractalDimension.h` (400-bar window, per `TripleScreen2.cpp`'s own `kFractalDimHmmWindow`
comment), `include/BipowerVariation.h` — per spec §2 (`src/TripleScreen2.cpp:334-361`).

- [x] **Step 1: Write the failing test** — synthetic TS2 (60m) bar-close sequence, assert against
  hand-computed values: `relative_range` = `cfc::ComputeRelativeRange(high, low, atr14, lastValid)`
  where `atr14` is a 14-bar SMA of True Range (confirmed `MOVAVGTYPE_SIMPLE`, not Wilder's, per
  `TripleScreen2.cpp:245`); `fractal_dim` = `SevcikFractalDimension.h` over the last 401 closed-bar
  closes (400-bar window, `kFractalDimHmmWindow`); `log_scale_expansion_ratio` = `BipowerVariation.h`
  bipower-variation log-ratio, same construct as Task 3's `log_scale_ratio` but over TS2's own
  short/long windows.
- [x] **Step 2: Run to verify it fails.**
- [x] **Step 3: Implement** — TS2 bar-close callback, same porting discipline as Task 3: maintain a
  fixed-size 14-bar True Range ring buffer (for the ATR feeding `relative_range`) and a fixed-size
  401-slot close-price ring buffer (for `fractal_dim`), call each header's function, write via
  `mutate_*()`. `log_scale_expansion_ratio` uses a fixed `observation_window_n=20` — matches
  `CalculateAdaptiveObservationWindow`'s own seed value (clamp range `[10,40]`), a real sanctioned
  default, not an invented number, same posture as Task 3's fixed-window simplifications.
  **Institutional-correctness fix made proactively (same class as Task 3's hurst fix)**:
  `fractal_dim`'s true cold-start "Brownian guess" is 1.5, not `ObservationData`'s zero-initialized
  0.0 — and 0.0 isn't even inside `fractal_dim`'s valid `[1.0,2.0]` contract, so this was a real bug
  risk, not just a cosmetic default; set explicitly in the constructor.
  **Applied the Task 3 ring-buffer lesson from the start**: `Capacity = targetWindow + 1` with pop
  firing when `size() > targetWindow`, not `size() == Capacity` — got the pattern right the first
  time this task, no off-by-one.
- [x] **Step 4: Run to verify it passes.** — 31/31 checks pass, `ALL PASS`. Two rounds of test-fixture
  bugs found and fixed (not engine bugs, both instructive): (1) a "gap-only" True Range fixture using
  single-tick bars is fundamentally insufficient — `high==low` always for a one-tick bar, and the
  first bar close has no prior close for the gap term either, so True Range is 0 on bar #1
  specifically no matter what; (2) even after adding a second bar, `relative_range=(high-low)/ATR`
  stays exactly 0 forever with single-tick bars, since the numerator `(high-low)` is always 0
  regardless of ATR — required giving a bar genuine intrabar range (multiple ticks inside one TS2
  bucket before crossing into the next) to actually exercise the dim at all.

---

### Task 4b: `bars_since_last_update` (`SystemState`'s regime-tenure field — previously undefined, resolved this session)

**⚠️ CADENCE FIDELITY BUG FOUND 2026-09-08: same bug as Task 3/4 — regime tenure is bar-GATED here
but its own inputs (`volatility`/`efficiency`) come from the same per-tick-reactive TS2 block
(`TripleScreen2.cpp:751-817`). See spec §2a; remediation tracked as Task 6b below.**

**Files:** Modify `MarketDataReplayEngine.h`, append to the test file.

**Interfaces:** No new header needed — pure arithmetic, ported from `src/TripleScreen2.cpp:751-817`
/`include/ContextManager.h:469`. **This closes a real gap**: `LBRFileManager::LogContext()`'s 4th
  parameter (`bars_since_last_update`) was referenced in Task 9 without a defined source anywhere
  in this plan — it is NOT covered by the `AsymmetryContext`/`RiskGateContext` deferral (spec §0),
  since `SystemState` is a third, separate, non-optional parameter concatenated with
  `MarketObservation` at inference time (schema's own comment).

- [x] **Step 1: Write the failing test** — synthetic TS2 bar-close sequence with a known
  volatility/efficiency step change; assert regime tenure resets to 1 on the step and increments by
  1 per unchanged bar otherwise, matching hand-computed values.
- [x] **Step 2: Run to verify it fails.**
- [x] **Step 3: Implement**, entirely physics-based, no HMM dependency: on each TS2 bar close,
  compute `volatility` (20-period rolling stddev of log returns — a fixed-size `RingBuffer<float,20>`
  of log returns, not `sc.StdDeviation`, which isn't available offline) and `efficiency` (Kaufman
  Efficiency Ratio: `|close[t] - close[t-20]| / Σ|close[i] - close[i-1]|` over the same 20-bar
  window, clamped to `[0,1]`); track the previous bar's `volatility`/`efficiency`; if either changes
  by more than `REGIME_CHANGE_THRESHOLD = 0.15f` (ported constant, `ContextManager.h:469` — cannot
  include `ContextManager.h` itself, spec §3b), reset the tenure counter to 1, else increment.
  Expose the running counter (as `float`) via `GetBarsSinceLastUpdate()` for `MarketDataReplay.cpp`
  to pass into `LogContext()`. Shares `log_scale_expansion_ratio`'s own 20-bar window/warm-up
  threshold (Task 4) — no separate buffer needed.
- [x] **Step 4: Run to verify it passes.** — 34/34 checks pass, `ALL PASS`. **Same lesson as Task 4's
  `relative_range` fixture, hit again**: a shock tick only *opens* a new bar; the bar's data isn't
  visible to `ComputeTs2Dims()` until a SECOND tick closes it. The first test attempt sent one shock
  tick and asserted an immediate reset, which failed — root-caused with temporary debug prints
  (confirmed efficiency/volatility were completely unchanged at the shock tick, not just close),
  fixed by sending a second tick to actually close the shocked bar. Not an engine bug.

---

### Task 5: TS3-owned dims — `amihud_illiquidity`, `liq_fragility`, `mean_rev_z`, `micro_asymmetry`

**⚠️ CORRECTION 2026-09-08: this task's own text below wrongly assumed `mean_rev_z` is bar-close
cadence in production ("stays bar-close cadence, `CalculateMeanReversionSpeed`'s own real call
site") — that assumption was never actually verified against the call site until Task 6. Confirmed
now (`TripleScreen3.cpp:801`): `mean_rev_z` is ALSO called every tick, no gate, same bug class as
Tasks 3/4/4b. `amihud_illiquidity`/`liq_fragility`/`micro_asymmetry` below are correctly intra-bar
already — only `mean_rev_z` needs rework. See spec §2a; remediation tracked as Task 6b below.**

**Files:** Modify `MarketDataReplayEngine.h`, append to the test file.

**Interfaces:** `include/CarryForwardCalculators.h`, `include/LiquidityFragilityEngine.h`, `include/
MeanReversionCalculator.h`, `include/OrderFlowAsymmetryEngine.h` — per spec §2
(`src/TripleScreen3.cpp:807-810`). Note `micro_asymmetry` is genuinely tick-reactive, not
bar-close-gated (spec §2/`whole_vector_redundancy_eval.cpp`'s own exclusion note) — implement its
update inside `OnTick()` directly, not the TS3 bar-close callback.

- [x] **Step 1: Write the failing test** — synthetic TS3 (15m) bar-close/tick sequence with known
  values; assert against hand-computed results: `amihud_illiquidity` and `liq_fragility` are
  live-reactive intra-bar (per `StudyHelperFunctions.cpp`'s own live-reactivity fix — update on
  every tick within the forming bar, not just at bar close), `mean_rev_z` is the median/MAD
  price-stretch z-score plus median-centered lag-1 return autocorrelation (`MeanReversionCalculator.h`,
  Kim & White 2004 reformulation, spec §2), and `micro_asymmetry` is the tick-level order-flow
  imbalance (`OrderFlowAsymmetryEngine.h`).
- [x] **Step 2: Run to verify it fails.**
- [x] **Step 3: Implement** — TS3 bar-close callback for `amihud_illiquidity`/`liq_fragility`/
  `mean_rev_z` (each maintaining its own small fixed-size rolling window per its header's own
  contract), but `micro_asymmetry` updates inside `OnTick()` directly, every tick, per the note
  above — do not gate it on the TS3 bar-close callback.
- [x] **Step 4: Run to verify it passes.**

**Findings:** Two real test-fixture bugs found via debug instrumentation (temporary
`std::printf`/`std::fprintf`, removed after diagnosis), not engine bugs:
1. `liq_fragility` measures whether the *current still-forming* bar's range-per-unit-volume
   deviates from the historical baseline. A fixture giving every bar (including the live one) an
   identical range/volume pattern has genuinely zero elasticity anomaly to detect — `0.0` is the
   metric's real, correct neutral output there, not evidence of a stuck default.
2. `ComputeLiquidityFragility` (`include/LiquidityFragilityEngine.h`) has a `kLiveBarMinVolume =
   50.0f` guard: below that cumulative live-bar volume, it returns `prev_fragility` unchanged
   rather than compute anything (avoids reacting to a noisy, barely-started bar). The fixture's
   live bar only accumulated volume=2, silently never clearing the gate. Fixed by giving the final
   (still-forming) bar both a deliberately anomalous range and volume ≥ 50.
`amihud_illiquidity` and `mean_rev_z` needed no fixture changes beyond the shared 2-tick-per-bar
intrabar-range pattern already established in Tasks 4/4b. All ~44 checks pass (`ALL PASS`).

---

### Task 6: Activity-clock dims — `skewness_idx`, `fast_taleb_kurtosis`, `fast_hurst_exponent`, `recurrence_rate`

**Files:** Modify `MarketDataReplayEngine.h`, append to the test file.

**Interfaces:** `include/ImbalanceBarEngine.h` directly (confirmed pure, spec §3b) — **do not**
`#include "ActivityClockManager.h"` (drags in `sierrachart.h`). `include/RobustMoments.h`, `include/
DfaHurstExponent.h`, `include/RecurrenceRateEngine.h`.

- [x] **Step 1: Write the failing test** — synthetic ask/bid-volume-imbalanced tick sequence,
  assert the imbalance-bar buffer accumulates and the 4 dims compute against hand-checked values.
- [x] **Step 2: Run to verify it fails.**
- [x] **Step 3: Implement** — own one `ImbalanceBarEngine m_imbalanceEngine;`; call
  `m_imbalanceEngine.OnTickWithPrice(index, askVolume, bidVolume, price)` every tick (replicates
  `ActivityClockManager::Update(sc)`'s exact real body, spec §3a step 2b, without the class it lives
  in); feed the resulting imbalance-bar return buffer into the 4 dims' formulas.
  **DOD checklist**: `ImbalanceBarEngine`'s own internal return buffer must stay fixed-window (it
  already is, per spec §3b's purity confirmation) — do not wrap it in any additional unbounded
  container here. **`barIndex` resolution**: a plain ever-incrementing per-tick counter, not tied to
  any calendar-clock bar — confirmed `sc.AskVolume[sc.Index]`/`BidVolume[sc.Index]` in production
  are this study's PER-TICK footprint volumes (1:1 with `mes_ticks.parquet`'s own per-tick columns),
  so `ImbalanceBarEngine::OnTick()`'s "barIndex != m_lastBarIndex" reset correctly fires every tick.
  **Institutional-correctness fix made proactively (same class as Tasks 3/4)**: `fast_taleb_
  kurtosis`'s true cold-start neutral value is 1.23f (`ContextManager.cpp`'s own carry-forward
  seed), and `fast_hurst_exponent`'s is 0.5f (shares `hurst_exponent`'s random-walk neutral) — both
  set explicitly in the constructor rather than left at FlatBuffers' zero-init.
- [x] **Step 4: Run to verify it passes.** — 20/20 new checks pass, 64/64 total, `ALL PASS`. One
  real test-fixture bug found and fixed (not an engine bug): the first fixture used a periodic
  price formula (`100 + 0.001*i + 0.02*((i%7)-3)`) that was fully deterministic and, though not
  literally periodic against the imbalance engine's own 5-tick bar-closing cadence, was still
  regular enough to produce pathological quantile behavior — `skewness_idx` landed at 0.998 (right
  at `BowleySkewness`'s own `[-1,1]` mathematical bound) and `fast_taleb_kurtosis` at 0.0025
  (outside its `[0.5,8.0]` contract). Root-caused via a standalone debug harness printing the real
  computed values (same "verify empirically" discipline as Task 5's `liq_fragility` debugging), not
  by re-guessing the formula. Fixed by switching the fixture to a simple LCG-driven pseudo-random
  price walk, which produced values comfortably inside every dim's contract range.

---

### Task 6b: Intra-bar reactivity remediation for TS1/TS2 dims + `mean_rev_z` (fixes the cadence bug found during Task 6)

**Files:** Modify `MarketDataReplayEngine.h`, append to the test file.

**Interfaces:** No new headers — reuses the same formula headers Tasks 3/4/5 already wired
(`BipowerVariation.h`, `DfaHurstExponent.h`, `CarryForwardCalculators.h`, `SevcikFractalDimension.h`,
`MeanReversionCalculator.h`).

**Background:** spec §2a. `hurst_exponent`, `fisher_info`, `log_scale_ratio` (TS1),
`relative_range`, `fractal_dim`, `log_scale_expansion_ratio`, `bars_since_last_update` (TS2), and
`mean_rev_z` (TS3) were all implemented bar-gated (Tasks 3/4/4b/5), but production recomputes every
one of them EVERY TICK from a window ending at the current, still-forming bar (confirmed against
real ACSIL indexing in `CalculateHurstExponent`/`TripleScreen2.cpp`/`TripleScreen3.cpp` — no
bar-close gate exists anywhere in production for these dims). This tool has been silently losing
real intra-bar information for 8 of its 18 dims.

- [x] **Step 1: Write the failing test** — for each affected dim, assert the value changes
  mid-bar (after a tick that does NOT close the underlying `TickBarAggregator` bar), not only at
  bar close. A bar-gated implementation will fail this (value frozen until the next close).
- [x] **Step 2: Run to verify it fails** (current implementation is bar-gated for all 8).
- [x] **Step 3: Implement** — extend the existing TS3 "live bar" tracking pattern
  (`m_ts3LiveHigh`/`Low`/`Close`/`Volume`, updated every tick, read by `ComputeTs3LiveDims()`) to
  TS1 and TS2: add `m_ts1LiveClose`/`m_ts2LiveHigh`/`Low`/`Close` (updated every tick from the
  current in-progress bar), and move the `mutate_*()` call for each affected dim out of the
  `OnTsXBarClose()` callback into a per-tick `ComputeTs1LiveDims()`/`ComputeTs2LiveDims()` (mirroring
  `ComputeTs3LiveDims()`), called from `OnTick()` every tick — reading `[closed-bar ring buffer] +
  [live bar]` as the window, with the live bar's current state as the window's final point (matches
  `sc.Close[sc.Index]` always being the live/forming bar in the existing codebase). The bar-close
  callbacks still matter for advancing the closed-bar ring buffers, but no longer own the
  `mutate_*()` call for these 8 dims. `mean_rev_z` moves from `OnTs3BarClose()` to
  `ComputeTs3LiveDims()`, alongside `amihud_illiquidity`/`liq_fragility`. Buffer capacities were
  shrunk by one CLOSED slot everywhere a live point now fills the window's final slot (TS1:
  100 closed, down from 101; TS2 fractal: 400 closed, down from 401; TS2 ATR: 13 closed, down from
  14; TS2 obs-window: 20 closed unchanged, live appended as the 21st).
- [x] **Step 4: Run to verify it passes.** — 8/8 new checks pass, 66/66 total, `ALL PASS`.

**Real finding while implementing regime tenure specifically**: `ContextManager::SetWaveContext()`'s
real source shows its "previous efficiency/volatility" comparison state is committed EVERY TICK, not
just at bar close — regime tenure is not merely fed live INPUTS on a bar-committed comparison, the
comparison itself is tick-granular (confirmed by direct read of `ContextManager.cpp`, not assumed).
Implemented to match.

**Three existing tests broke and were fixed (test-fixture issues, not engine bugs), all following
this session's established "verify empirically" discipline**:
1. `ts1_fisher_info_moved_off_zero_default_after_warmup` — the periodic test fixture's 102nd tick
   happened to land exactly on the fixture's own symmetric midpoint price (100.0), and
   `fisher_info`'s Fisher-transform formula is exactly 0.0 at that midpoint by construction —
   coincided with the dim's own zero-init default. Fixed by extending to 103 ticks.
2. `ts2_relative_range_moves_off_zero_with_real_intrabar_range` — checked the value right after
   bar #1 closed into bar #2, but `relative_range` now reads the LIVE (bar #2, a fresh single
   point) bar's own high/low, not bar #1's. Fixed by checking while bar #1 is still forming.
3. `ts2_regime_tenure_resets_to_one_after_a_genuine_shock` — the shock now fires on the tick that
   opens it, not two ticks later; updated the assertion timing to match.

**One Task 6b test itself needed a fixture fix, found via a standalone debug harness (not
re-guessed)**: `task6b_bars_since_last_update_reacts_mid_bar` initially failed because the shared
noisy warm-up fixture kept resetting regime tenure to 1 on nearly every tick already, so "before"
and "after" the mid-bar price jump both landed on the same floor value (1==1) — a false negative,
not a sign the mechanism wasn't reacting. Fixed by adding a 30-bar calm tail before the jump so
tenure climbs above 1 first, making the reset to 1 genuinely detectable.

---

### Task 7: `fast_mean_rev_z` sentinel regression test

**Files:** Append to the test file only — no production code change.

- [x] **Step 1: Write the test** — after any sequence of ticks, assert `m_obs.fast_mean_rev_z() ==
  0.0f` exactly, always. This is a guard, not a feature: per spec §2, this dim is decided-DROP in
  the existing codebase and must never silently acquire a real value here.
- [x] **Step 2: Run to verify it passes immediately** (Tasks 1-6/6b never touch this field —
  confirmed: 67/67 checks pass, `ALL PASS`, reusing Task 6b's own fully-warmed-up engine as the
  strongest test of "nothing accidentally wires this field").

---

### Task 8: `FeatureScaler` + the real collection-mode change gate (the real gate)

**Files:** Modify `MarketDataReplayEngine.h`, append to the test file.

**Interfaces:** `include/FeatureScaler.h` (confirmed pure, spec §1), used as-is, not re-derived.
`include/generated/mts_schema_contract_generated.h`'s `ToObservationArray()`. `include/
ObservationTriggerGate.h`'s free functions `SanitizeObservationVector()`/`IsEnergyObservationDim()`
only — **NOT** its `ObservationTriggerGate` class/`ComputeTriggerDecisionMetrics()`.

**CORRECTION 2026-09-08 (found while implementing, by reading `ContextManager.cpp::
CheckAndTriggerHMM` directly): the Mahalanobis `ObservationTriggerGate::ComputeTriggerDecisionMetrics()`
gate this task originally planned to wire is NOT what actually governs `.context` emission.**
`ShouldTriggerHMM(hmm_initialized, significant_change, is_data_collection, any_observation_changed)`
(`src/ContextManager.cpp:39`) branches on `is_data_collection` — for `isDataCollection=true` (always
true for `EventDataCollectorStudy.cpp`, this tool's own target, per spec §3a), the decision is
`return !hmm_initialized || any_observation_changed;` — the Mahalanobis `significant_change` value
is computed but **never consulted** on this branch. The real gate is `UpdateCollectionObservationTelemetry()`
(`src/ContextManager.cpp:727`): per-dim absolute delta on the SCALED observation vs. the previous
call's scaled observation (updated every call, unconditionally, regardless of whether a trigger
fires), threshold `OBS_CHANGE_EPS=1e-5f` for energy dims (`otg::IsEnergyObservationDim()`) or
`OBS_CHANGE_EPS * OBS_GEOMETRY_CHANGE_MULT=3.0f` for the rest (`ContextManager.h:274-275` — both
plain `constexpr float`s, replicated directly since `ContextManager.h` itself isn't includable,
spec §3b). `hmm_initialized` = "has this tool ever successfully emitted before" (mirrors
`m_triggerGate.HasBaseline()`, which only flips true after a successful emission).
`ObservationTriggerGate`'s Mahalanobis machinery is real and used for the LIVE-trading branch
(`isDataCollection=false`), genuinely out of scope for a `.context`-only generator.

- [x] **Step 1: Write the failing test** — feed a long quiet synthetic sequence where the scaled
  vector barely moves after the first sample (gate should mostly stay quiet after the first
  emission) followed by an injected regime shift (gate should fire again); assert `OnTick()`'s
  return value matches, and that the very first post-warmup tick always fires (first-ever emission).
- [x] **Step 2: Run to verify it fails** (`OnTick()` still always returns `false` from Task 1).
- [x] **Step 3: Implement** — own one `FeatureScaler m_featureScaler;` + one `otg::
  ObservationTriggerGate m_triggerGate;` + one `eve::VelocityState m_velocityState;`. At the end of
  `OnTick()`: `rawObs = ToObservationArray(m_obs)` → `otg::SanitizeObservationVector(...)` → validate
  all-finite → `currentObs = m_featureScaler.UpdateAndNormalize(rawObs)` → if `!warmedUp` return
  false → validate all-finite again → `m_triggerGate.PushObservation(currentObs)` → real
  timestamp-based `event_velocity` (`eve::UpdateAndGetVelocity`, matching `CalculateEventVelocity`'s
  own EMA-of-inter-arrival-time formula, `tauUs = EVENT_VELOCITY_WINDOW_SEC(2)*1e6`) →
  `ComputeTriggerDecisionMetrics` → `should_trigger = !HasBaseline() || significant_change` → if
  true, `SetBaseline(currentObs)`.
  **DOD checklist**: `ToObservationArray()`/`FeatureScaler`/`ObservationTriggerGate` all fixed-size,
  no allocation.
- [x] **Step 4: Run to verify it passes.** — 3/3 new checks pass, 70/70 total, `ALL PASS`.

**Build note**: `FeatureScaler.h` needs `<nlohmann/json.hpp>` (a vcpkg dependency) — plain `g++`
fails with `fatal error: nlohmann/json.hpp: No such file or directory`. Build via
`mamba run -n mts g++ ...` instead (the `mts` conda env already provides it, same as every
`tools/observation_vector/*.cpp` that includes `FeatureScaler.h` today, per spec §3i's own note).

**Real fixture lesson, found via a standalone debug harness before committing to the test**: a
second-scale tick fixture (ticks 1 second apart) leaves almost every TS1/TS2/TS3 bar-based dim
frozen at its cold-start default for the entire run (not enough elapsed session time to close any
bars — TS1 alone needs 100+ 240-minute bars) — the resulting "observation vector" is nearly
constant regardless of price jumps, so the Mahalanobis gate correctly never fires even on an
extreme injected jump (not a bug — there was nothing for the gate to detect). Fixed by reusing Task
6b's hour-scale warm-up pattern instead, which genuinely varies all 18 dims. Real observed gate
behavior on that fixture, all 471 ticks: exactly 1 trigger before/at `FeatureScaler`'s 500-sample
warmup boundary (the mandatory first-ever emission), ~12% trigger rate during continued noisy-but-
quiet ticks after warmup (vs. the old gate's near-100% rate), and a reliable trigger on an injected
$400 price jump.

---

### Task 9: CLI driver — `MarketDataReplay.cpp`

**Files:**
- Create: `tools/market_data_replay/MarketDataReplay.cpp`
- Create: `tools/market_data_replay/ContextFileWriter.h`

**Interfaces:** `tools/observation_vector/market_data_io.h` (`StreamTicksFullParquet`, unchanged),
`tools/ToolProgressLogger.h`.

**CORRECTION 2026-09-08 (found while implementing): `LBRFileManager` cannot actually be linked into
a standalone Linux tool.** Its `.cpp` transitively includes `MindfulTrader_Precompiled.h` →
`sierrachart.h` → `windows.h` — genuinely Windows-only (confirmed by direct compile attempt, not
assumed), not just a missing include path. Per operator directive, created
`ContextFileWriter.h` instead: a deliberate, faithful duplicate of `LBRFileManager`'s `.context`-only
write path (`Open`/`LogContext`/`Close`) — every byte-format detail (magic header, `FileMetadata`,
MO+SS size-prefixed pairing, flush cadence) copied verbatim from `src/LBRFileManager.cpp`, not
reimplemented from scratch, so output stays byte-compatible with `tools/context_pipeline/
context_reader.h`. `src/LBRFileManager.cpp` itself is left untouched (an initial attempt swapped its
`MindfulTrader_Precompiled.h` include for explicit ones and confirmed it isolated cleanly — zero
real `sc.`/ACSIL usage anywhere in that file, only `<atomic>` was missing — but the operator asked
for a separate class instead of touching the shared file, so that change was reverted).

- [x] **Step 1: Parse flags** per spec §3d (`--ticks-parquet`, `--output`, `--max-rss-mb`).
- [x] **Step 2: Open output** — `ContextFileWriter::Open(outputBasePath, "ES")`; logs that
  `.alpha`/`.imbalance.context` are intentionally never produced (spec §3h — this tool is
  `.context`-only, unlike the real `LBRFileManager` which always opens all 3 streams).
- [x] **Step 3: Stream and drive the engine** — `StreamTicksFullParquet(path, [&](ts, price, vol,
  askVol, bidVol, isNewContract) { ... })`; per spec §3a open Q1's resolution, does **not** reset
  engine state on `isNewContract` (faithful-to-live-trading directive, spec §0) — just calls
  `engine.OnTick(...)`, and on `true`, calls `contextWriter.LogContext(engine.GetObservation(),
  MTS::Schema::AsymmetryContext{}, timestamp_us, engine.GetBarsSinceLastUpdate(),
  /*risk_gate_context=*/nullptr)`.
- [x] **Step 4: Call `engine.Flush()` at end of stream** (spec §3f).
- [x] **Step 4b: Wire `ToolProgressLogger::CheckMemoryBudget(maxRssMb)`** at the same progress-log
  cadence every sibling tool uses.
- [x] **Step 4c: Wire the Logging Policy** — progress every 5,000,000 ticks, interim summary every
  50,000,000 ticks, contract-roll and warm-up-completion transitions logged unconditionally (never
  rate-limited), mandatory final summary line (ticks processed, `.context` records written,
  significant-change rate).
- [x] **Step 5: Build** — `mamba run -n mts g++ -O2 -std=c++17 -Iinclude $(mamba run -n mts
  pkg-config --cflags arrow parquet) tools/market_data_replay/MarketDataReplay.cpp $(mamba run -n
  mts pkg-config --libs arrow parquet) -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib -o
  tools/bin/market_data_replay` — clean.
- [x] **Step 6: Smoke test** — ran against the real, full `mes_ticks.parquet` (killed early, not run
  to completion — not needed for a smoke test), then verified the resulting partial `.context` file
  with a standalone harness using the real `tools/context_pipeline/context_reader.h`: opened
  cleanly, **335,147 aligned MO+SS pairs, zero sequence mismatches**. Real observed behavior: first
  `.context` record written at exactly tick 500 (`FeatureScaler::RANK_WINDOW`), contract-roll
  correctly detected and logged without resetting engine state, and the first few records correctly
  show cold-start defaults (`hurst_exponent=0.5`, `fast_taleb_kurtosis=1.23`, `bars_since=0.0`) since
  500 real ticks is nowhere near enough elapsed session time for TS1's 100+ 240-minute bars to close.

---

### Task 10: Warm-up gating (spec §3a open Q2)

**Files:** Modify `MarketDataReplayEngine.h`, append to the test file.

- [x] **Step 1: Write the failing test** — feed a short tick sequence (not enough to warm up every
  dim, e.g. `TailRiskEngine`'s own ≥50-sample floor); assert `OnTick()` never returns `true` during
  this window even if the gate's raw math would otherwise fire.
- [x] **Step 2: Run to verify it fails** — confirmed via a standalone debug harness before writing
  the test: 26 spurious emissions over 700 one-second ticks, all with TS1/TS2 bar-based dims still
  frozen at cold-start defaults (not enough elapsed session time for any bar to close).
- [x] **Step 3: Implement** — `AllDimsReady()`, checking the single LARGEST per-timeframe buffer
  requirement (TS1: `m_ts1Closes.size() >= 100`; TS2: `m_ts2Closes.size() >= 400`, fractal_dim's
  window dominates TS2's own smaller needs; TS3: `m_ts3ClosedRanges.size() >= 30`, liq_fragility's
  window dominates; activity-clock: `m_imbalanceEngine.GetCompletedBarCount() >= 100`; tail_index:
  `m_tailRiskEngine.GetSampleCount() >= 50`) as the very first check in `ComputeShouldEmit()` —
  `OnTick()` returns `false` unconditionally until every one of these is satisfied, no
  placeholder-value emission (spec §3a open Q2 resolution).
- [x] **Step 4: Run to verify it passes.** — 1/1 new check passes, 71/71 total, `ALL PASS`.

**Real, significant side effect found while fixing Task 8's own test to accommodate this change**:
gating `ComputeShouldEmit()`'s very first line means `FeatureScaler`'s own 500-sample warmup
counter does not even START incrementing until `AllDimsReady()` first passes — the two warm-ups
are now genuinely SEQUENTIAL (bar-based dims warm up over ~400 hours of elapsed session time, THEN
500 more ticks for `FeatureScaler`), not concurrent as Task 8's original test fixture assumed. This
is the institutionally correct behavior (matches this initiative's own quality-over-quantity
standard — `FeatureScaler` should not build its rolling scale statistics from a period when
bar-based dims were still emitting cold-start-default placeholder values), but it meant Task 8's
440-iteration fixture no longer left enough runway for `FeatureScaler` to warm up at all within the
test (found via a standalone debug harness: every single tick returned `false`, not just the ones
during warmup). Fixed by extending Task 8's fixture to 750 iterations and moving its warmup/quiet
split from a fixed tick count to the TS1-warmup boundary itself (iteration 400).

---

### Task 11: Edge-case hardening (spec §3f)

**Files:** Modify `MarketDataReplayEngine.h`/`MarketDataReplay.cpp`, append to the test file.

- [x] **Step 1: Write failing tests** for each: (a) zero/negative price tick is skipped, not fed to
  any engine; (b) a timestamp that goes backwards beyond benign jitter raises a hard error, not a
  silent reorder; (c) NaN from any leaf header's degenerate-window path carries forward the last
  valid value into `m_obs`, never propagates into `FeatureScaler`.
- [x] **Step 2: Run to verify they fail.** — (a)/(b) failed as expected (no guard existed yet);
  (c) passed immediately — the `if (std::isfinite(x)) mutate_x(x);` carry-forward pattern already
  used throughout every dim (Tasks 3-6) already satisfies this, confirmed as a regression guard, not
  a new gap.
- [x] **Step 3: Implement each guard** — (a) `if (price <= 0.0) return false;` as `OnTick()`'s first
  line (matches `whole_vector_redundancy_eval.cpp`'s own precedent); (b) track `m_lastTimestampUs`,
  throw `std::runtime_error` on any backwards jump — no benign-jitter tolerance here (unlike
  `scid_to_ticks_parquet.cpp`'s own `kMaxBenignTimestampJitterUs`), since this tool's input is
  already resequenced upstream, so ANY remaining backwards jump is real data corruption per spec
  §3f. (c) no code change needed, already correct.
- [x] **Step 4: Run to verify they pass.** — 5/5 new checks pass, 75/75 total, `ALL PASS`. CLI
  driver (`MarketDataReplay.cpp`) rebuilds clean against the updated engine.

---

### Task 12: Real validation pass (spec §3g / §3 open question 5)

**Status: BLOCKED, 2026-09-08 (operator directive: defer).** The only genuine Sierra-Chart-produced
`.context` file found on disk (`/home/rcruz/devel/VSCode/lbrnet/data/raw/event_data.context`,
14.8GB) is stamped `schema_version=230`; the current schema is `240` — `context_reader.h`'s own
hard-refuse gate correctly blocks reading it (exists precisely to prevent silently misinterpreting
incompatible `ObservationData` bytes across a schema change, per the brainstorm doc's §10.2
versioning contract). No current-schema comparison file exists yet. This task needs a freshly
SC-collected `.context` file at `schema_version=240` before it can proceed — not something
producible from this environment (requires actually running Sierra Chart). Revisit once one exists.

**Files:** New standalone comparison tool or script, not part of the engine/CLI proper — exact
location TBD when this task starts (likely `tools/market_data_replay/validate_against_sc_context.cpp`
or a short Python script using `context_reader.h`-equivalent parsing).

- [ ] **Step 1:** Obtain (or generate, if none exists yet) a genuine Sierra-Chart-produced `.context`
  file over a historical window this tool can also process.
- [ ] **Step 2:** Run this tool over the same window, matching records by `timestamp_us`.
- [ ] **Step 3:** Per-field float comparison within a tight relative epsilon (e.g. 1e-5) — not
  literal `memcmp` (spec §3's open question 5 cross-compiler ULP caveat).
- [ ] **Step 4:** Document the result (pass/fail per dim) in the spec's own status line before this
  tool is trusted for real Feature Saliency EM training data.
