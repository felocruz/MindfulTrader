# Decision-Boundary Calibration Tool Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement the generic, parameterized decision-boundary calibration tool proposed in
`docs/superpowers/specs/2026-09-19-decision-boundary-calibration-tool-spec.md`, then apply it to
that spec's prospective queue (§8: `roughness_ratio`, `session_quality_score`) — the two
`AsymmetryContext` dims that currently block Trigger 3
(`docs/superpowers/specs/2026-09-19-meaningful-event-trigger-and-asymmetry-context-significance-
spec.md` §4c) from having any real threshold at all. The retroactive audit queue (spec §7) is
explicitly out of scope for this plan (spec §9 item 3 — separate follow-up, its own review).

**Architecture:** Pure numerical core (`tools/observation_vector/DecisionBoundaryCalibration.h`, no
I/O, natively unit-testable) + thin CLI/data-sourcing drivers per dim — mirrors this repo's own
established `FeatureSaliencyEM.h`/`FeatureSaliencyEval.cpp` and `market_test_stats.h` split. No
`CMakeLists.txt`/`build_dll.sh` changes — standalone `tools/` convention throughout. Every
executable step routes results through `ToolProgressLogger` (auto-archived, auto-ledgered) per this
repo's top-level directive.

**Tech stack:** C++17, native `check()`/`g_failures`/`ALL PASS` unit tests. No new external
numerical dependencies — the GPD/EVT mode extracts and generalizes the Method-of-Moments fit
already proven in `tools/observation_vector/observation_vector_recalibration.cpp`'s `FitGPD()`
(Hosking & Wallis 1987), and the percentile-match mode ports
`analyze_kurtosis_threshold_migration.py`'s exact ECDF-then-percentile algorithm to C++ rather than
re-deriving it.

**Spec:** `docs/superpowers/specs/2026-09-19-decision-boundary-calibration-tool-spec.md` — read in
full before starting; every task below cites the section it implements.

## Institutional discipline (carried from the spec's own standing rules)

- **Validate the tool against a known-good answer before trusting it for anything new** (spec §9
  item 1) — Task 2's correctness check must reproduce `taleb_kurtosis`'s already-published
  percentile-match results before the tool is used on `roughness_ratio`/`session_quality_score`.
- **Real tick data only, no synthetic placeholders** for the final calibration runs (Tasks 5-6) —
  matches this project's standing rule, already the subject of the parent spec's own finding.
- **Do not touch the retroactive-queue gates** (`shannon_entropy` bands, `taleb_cliff`,
  `raschke_burst`'s legacy basis, `taleb_skewness`'s citation) in this plan — explicitly deferred
  (spec §5, §9 item 3).
- **`ToolProgressLogger` required** for every executable step that reads the full tick dataset —
  this repo's own top-level directive exists because a prior long run's only report was lost to
  terminal scrollback.

---

### Task 1: Core calibration math header — empirical-percentile + percentile-match modes (spec §6, §4)

**Files:** New `tools/observation_vector/DecisionBoundaryCalibration.h`, new
`tools/observation_vector/test_decision_boundary_calibration.cpp`.

- [x] **Step 1: Write the failing test** — for empirical-percentile mode: a small hand-constructed
  sorted sample, assert the value at a known target rate matches a hand-computed percentile. For
  percentile-match mode: a small hand-constructed paired (old, new) sample where the old
  threshold's ECDF percentile is easy to compute by hand, assert the mapped new-scale value matches.
- [x] **Step 2: Run to verify it fails** (no implementation exists yet).
- [x] **Step 3: Implement**:
  - `EmpiricalPercentileThreshold(sortedValues, targetRate) -> double` — thin wrapper reusing
    `market_test_stats.h`'s `PercentileFromSorted` (do not duplicate it).
  - `PercentileMatchThreshold(oldSample, newSample, oldThreshold) -> double` — port
    `analyze_kurtosis_threshold_migration.py`'s exact algorithm: `percentile = (oldSample <=
    oldThreshold).mean() * 100`, then `PercentileFromSorted(sorted(newSample), percentile)`.
- [x] **Step 4: Run to verify it passes.**

### Task 2: Core calibration math header — EVT/GPD mode + correctness check against `taleb_kurtosis` (spec §6, §9 item 1)

**Files:** Modify `DecisionBoundaryCalibration.h`, append to the test file.

- [x] **Step 1: Write the failing test** — extract `observation_vector_recalibration.cpp`'s
  `GPDFit`/`FitGPD` into this header unchanged (byte-for-byte logic port, not a rewrite); assert the
  ported function reproduces one of that file's own already-published, real-data-derived results
  (e.g. `liq_fragility`'s `u=5.1727, n_tail=93475, xi=+0.3147, sigma=2.7671, returnLevel=1100.7906`
  from `docs/superpowers/specs`/`CLAUDE.md`'s cited figures) on a synthetic sample engineered to hit
  those exact intermediate values, OR (simpler, preferred) assert against a hand-computable
  synthetic GPD-distributed sample with a known analytic answer.
- [x] **Step 2: Run to verify it fails.**
- [x] **Step 3: Implement** — move `GPDFit`/`FitGPD` into the shared header; have
  `observation_vector_recalibration.cpp` continue working unmodified (out of scope to refactor its
  call site in this plan — extraction only, per `market_test_stats.h`'s own "extract on 2nd/3rd
  real use, don't force-migrate existing callers" precedent unless a quick follow-up confirms it's
  a copy of the very same code with no behavior change).
- [x] **Step 4: Run to verify it passes.**

### Task 3: CLI driver skeleton (spec §6)

**Files:** New `tools/observation_vector/DecisionBoundaryCalibrationEval.cpp`.

- [x] Accepts `--mode {empirical-percentile|percentile-match|evt-gpd}`, a plain newline-delimited
  float file input (generic — not fused to any one dim's own data-sourcing mechanism), and
  mode-specific parameters. Dispatches to Task 1/2's core functions. Emits results via
  `ToolProgressLogger`.
- [x] Smoke test: **the originally-planned real-data reproduction of
  `analyze_kurtosis_threshold_migration.py`'s published numbers was BLOCKED** -- the `.scid` source
  files it needs (`/mnt/c/SierraChart2/Data/`) are not mounted in this environment (checked
  directly), and no paired-sample CSV artifact survives on disk. Substituted a synthetic
  hand-computable CLI-level smoke test instead (confirms argument parsing/file I/O/dispatch;
  Task 1/2's own unit tests already prove the algorithms themselves are correct ports) --
  documented here as a real, not-silently-skipped gap, not treated as equivalent to the original
  plan.

### Task 4: Real-data sourcing for `roughness_ratio` (spec §8 item 1) -- DONE

**Files:** New `tools/observation_vector/asymmetry_context_dim_extractor.cpp`, new shared
`ClassifyTimeOfDay()`/`TimeOfDayEnum`/`ComputeSessionQualityScore()` relocated to
`include/IndicatorComputations.h` (see Task 5 note -- one combined tool covers both dims).

- [x] Confirmed which chart/bar interval production's `ContextManager::UpdatePriceStructure()` is
  actually fed from: `SCStudies.cpp`'s own comment states explicitly "Runs on the TS3 (15-minute)
  chart ... All ContextManager updates (physics, structure, HMM) operate in 15-min context" --
  verified directly, not assumed. `StructureEngine`/`TickBarAggregator` are both confirmed
  ACSIL-independent (no `sc.`/`SCStudyInterfaceRef`), reused as-is.
- [x] **Parallelized across the Puget machine's cores** (32 hardware threads; machine load checked
  via `free -h`/`ps aux` first, per `/memories/user/shared_hardware_concurrency.md` -- only one
  other light single-core job running, 52GB free): `mes_ticks.parquet`'s row groups are
  single-contract and chronologically ordered (existing, already-verified property), so
  `[0,num_row_groups)` was split into N contiguous shards, each with its own
  `TickBarAggregator`/`StructureEngine` instance (no shared/locked state) -- same "one independent
  unit of work per thread" shape as `scid_to_ticks_parquet.cpp`'s existing per-contract worker
  pool. Full 471.9M-tick, 7,282-row-group run: **~2 seconds wall clock with 16 threads** (a 200-
  row-group timed sub-run extrapolated to ~29s single-threaded; DOD-style per-tick work in
  `TickBarAggregator`/`StructureEngine` was already cheap/allocation-light, so the real lever here
  was I/O parallelism, not micro-level compute restructuring).
- [x] Emitted a real `roughness_ratio` time series across the full tick dataset: **77,426 real
  samples** (p10=1.695 p50=2.464 p90=3.490 p99=4.458), archived via `ToolProgressLogger`
  (`tools/output/asymmetry_context_dim_extractor_20260919_202326.txt`, ledger-reviewed).

### Task 5: Real-data sourcing for `session_quality_score` (spec §8 item 2) -- DONE

- [x] Located the live `TimeOfDayEnum` classification logic (`src/Indicator.cpp`'s
  `TimeOfDayIndicator::SetFromDateTime()`) and confirmed it reduces to pure ET-hour/minute
  arithmetic (its only ACSIL touch is `SCDateTime::GetTimeHMS()` for extracting hour/minute, not
  the classification itself). Extracted the boundary logic into a new pure
  `ClassifyTimeOfDay(hour, minute, hasOpenPosition)` in `include/IndicatorComputations.h`, moved
  `TimeOfDayEnum`/`ComputeSessionQualityScore` there alongside it (same "zero-ACSIL-dependency,
  single-source, re-exposed via the `#include` in `Indicator.h`" pattern already used for
  `RaschkeStrategySetup`/`RaschkeTacticalTrigger`) -- `SetFromDateTime()` now delegates to the pure
  function instead of duplicating the boundary logic. Full clean `./build_dll.sh` passes; offline
  `market_data_replay` test suite (75+ tests) re-run clean, confirming no regression from the move.
- [x] Emitted a real `session_quality_score` time series (same tool/run as Task 4, ET hour/minute
  derived from each TS3 bar's own close timestamp via `EasternTimeOffset.h`'s existing
  `GetEasternUtcOffsetSeconds()`): **77,890 real samples** (discrete-valued as expected --
  `ComputeSessionQualityScore()`'s fixed per-session constants -- corrected values below).

**Correction, 2026-09-19 (same day, post-user-review)**: `ComputeSessionQualityScore()`'s first
version invented its own [-1,+1] scale instead of matching the canonical, already-established
`symmetric_val` per `TimeOfDayEnum` member in `lbrnet/lbrnet/core/rc_enums.py` ("Elite v2.3:
Quality-Based Symmetric Mapping") -- every other Transformer-facing categorical field in this
system already has exactly one canonical mapping there, and this function should have matched it
from the start rather than deriving a fresh one. Fixed to transcribe the real values
(SWEET_SPOT=1.0, OPENING_HOUR/AFTERNOON_SESSION=0.66, PRE_MARKET_HOOK/PM_RUN_ENTRY=0.33,
ASIAN_SESSION/PRE_MARKET/OVERNIGHT_HOLD=0.0, LONDON_WINDOW/LONDON_TO_PREMARKET/AFTER_HOURS=-0.33,
FINAL_HOUR=-0.66, LUNCH_DEAD_ZONE=-1.0), corrected value set: p10=-0.66 p50=0.0 p90=0.66 p99=1.0.
Extraction and calibration rerun against the corrected scale (see Task 6 below).

### Task 6: Calibrate both dims via empirical-percentile mode against the full real dataset (spec §9 item 2) -- DONE

- [x] Ran Task 3's CLI against Task 4/5's real time series at this repo's ~10% base-rate precedent
  (`CandidateTriggerGate::kBaseEpsilon`'s chi-squared derivation):
  - `roughness_ratio` empirical-percentile (10% upper-tail): **threshold = 3.4898**.
  - `roughness_ratio` EVT-GPD (POT, u=p99): u=4.4583, n_tail=771, xi=-0.0948 (Weibull/bounded,
    genuine finite endpoint), sigma=0.3652, p=1/N return level (N=77,426) = **6.2597**.
  - `session_quality_score` empirical-percentile (10% lower-tail, **corrected scale**):
    **threshold = -0.66** (supersedes a stale -0.8 computed before the symmetric_val fix above).
- [x] Recorded in `tools/RECALIBRATION_LEDGER.md` (superseded row + corrected rerun rows marked
  REVIEWED). Cross-referencing from the parent Trigger 3 spec's §4c is the natural next step,
  left to that spec's own implementation work (out of this plan's scope per "Explicitly deferred"
  below).

---

## Explicitly deferred (not this plan)

- Wiring the resulting thresholds into `ContextManager::HasAsymmetryContextMovedSignificantly()`
  and the `changed_mask` wire bits — belongs to Trigger 3's own implementation plan (parent spec §5
  items 2-3), not this tool's plan.
- The retroactive audit queue (spec §7) — separate follow-up.
