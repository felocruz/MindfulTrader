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

- [ ] Accepts `--dim <name>`, `--mode {empirical-percentile|percentile-match|evt-gpd}`, a real-data
  input source (initially: a plain newline-delimited float file or CSV column, generic — not fused
  to any one dim's own data-sourcing mechanism), and mode-specific parameters (`--target-rate` /
  `--old-threshold --old-sample --new-sample` / `--pot-threshold-percentile`). Dispatches to Task
  1/2's core functions. Emits results via `ToolProgressLogger`.
- [ ] Manual smoke test: rerun `analyze_kurtosis_threshold_migration.py`'s existing paired CSV
  through `--mode percentile-match` for at least 2 of its `OLD_THRESHOLDS` entries and confirm the
  C++ tool's output matches the already-published Python output to a tight tolerance — the
  concrete form of Task 2's "known-good answer" validation, now end-to-end through the CLI.

### Task 4: Real-data sourcing for `roughness_ratio` (spec §8 item 1)

**Files:** likely a small new standalone driver under `tools/observation_vector/` (or a mode added
to an existing `market_data_replay` binary if that proves less invasive once inspected) feeding
`StructureEngine::Update()`/`GetRoughnessRatio()` (`include/StructureEngine.h`, confirmed
ACSIL-independent, no `sc.`/`SCStudyInterfaceRef` dependency) from real TS3-cadence OHLC bars
reconstructed from `mes_ticks.parquet` — reuse `tools/market_data_replay/MarketDataReplayEngine.h`'s
existing TS3 `TickBarAggregator` rather than building a new one.

- [ ] Confirm which chart/bar interval production's `ContextManager::UpdatePriceStructure()` is
  actually fed from (`SCStudies.cpp`'s call site) before assuming TS3 — do not assume from memory.
- [ ] Emit a real `roughness_ratio` time series across the full tick dataset, `ToolProgressLogger`-
  reported, archived to `tools/output/`.

### Task 5: Real-data sourcing for `session_quality_score` (spec §8 item 2)

**Files:** likely a small pure `timestamp -> TimeOfDayEnum` classifier extracted from wherever the
live classification logic lives (`StudyHelperFunctions.cpp`, to be located, not assumed), mirroring
`MarketDataReplayEngine.h`'s existing `IsRthSession()` offline precedent, feeding
`ComputeSessionQualityScore()` (`include/Indicator.h`, already pure).

- [ ] Locate the live `TimeOfDayEnum` classification logic and confirm it reduces to pure
  timestamp/session-calendar arithmetic (no other ACSIL dependency) before porting.
- [ ] Emit a real `session_quality_score` time series across the full tick dataset.

### Task 6: Calibrate both dims via empirical-percentile mode against the full real dataset (spec §9 item 2)

- [ ] Run Task 3's CLI against Task 4/5's real time series in `--mode empirical-percentile`, at a
  target rate consistent with this repo's existing base-rate precedent (~10%, per
  `CandidateTriggerGate::kBaseEpsilon`'s chi-squared derivation, cited in the parent spec's §6 item
  4) unless a dim-specific rate is separately justified.
- [ ] Record results in `tools/RECALIBRATION_LEDGER.md` (automatic via `ToolProgressLogger`) and
  cross-reference from the parent Trigger 3 spec's §4c once thresholds exist.

---

## Explicitly deferred (not this plan)

- Wiring the resulting thresholds into `ContextManager::HasAsymmetryContextMovedSignificantly()`
  and the `changed_mask` wire bits — belongs to Trigger 3's own implementation plan (parent spec §5
  items 2-3), not this tool's plan.
- The retroactive audit queue (spec §7) — separate follow-up.
