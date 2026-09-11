# Puget Workstation Capacity Headroom Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Execute `docs/superpowers/specs/2026-09-11-puget-workstation-capacity-headroom-spec.md`
§4 — raise the 8 old-machine-tuned `--max-rss-mb` safety-net ceilings, raise
`FeatureSaliencyEval.cpp`'s reservoir-sampling cap with a mandatory re-validation run, and
implement best-of-N restart for `FeatureSaliencyEM.h`'s k-means++ initialization.

**Spec:** `docs/superpowers/specs/2026-09-11-puget-workstation-capacity-headroom-spec.md` — read in
full before starting; every task below cites the section it implements.

## Institutional discipline (carried from the spec's own standing rules)

- **§4a is purely mechanical** — a safety-net ceiling raise cannot change any tool's computed
  output; do not add any other logic while touching these files.
- **§4b is NOT purely mechanical** — raising `--max-observations` changes the actual sample the EM
  fitter sees. Task 4's re-validation run is mandatory, not optional, before the new default is
  trusted for any downstream decision.
- **§4c is additive only** — `FitFeatureSaliencyEM<D>()`'s existing signature, behavior, and tests
  must be untouched. The best-of-N wrapper is new code alongside it, never a modification in place.
- **Out of scope, per spec §5**: no GPU/CUDA work, no Feature Saliency dim-drop decision, no
  E-step parallelization, no `--max-rss-mb` changes to files outside the §3 inventory.

---

### Task 1: Raise `--max-rss-mb` defaults in the 3 hardcoded-and-enforced files (spec §4a)

**Files:** `tools/observation_vector/observation_vector_recalibration.cpp`,
`tools/market_data_replay/MarketDataReplay.cpp`,
`tools/observation_vector/whole_vector_redundancy_eval.cpp`.

- [ ] In each file, change `std::size_t maxRssMB = 3072;` → `std::size_t maxRssMB = 8192;`.
- [ ] Update each file's own header/inline comment that cites "a ~15GB box" (all three currently
  reference the 2026-09-03 OOM incident) to record the new rationale: Puget's 96GB affords an
  8192MB/pass default (still supports 4 concurrent passes at 32GB total, leaving ~64GB free for
  concurrent Sierra Chart/VS Code/WSL work), fully overridable via `--max-rss-mb`.
- [ ] Rebuild each tool via its own file-header build-command comment (`mamba run -n mts g++ -O2
  -std=c++17 ...`) and confirm a clean compile — no functional test needed (pure constant change),
  but a compile failure would indicate a copy-paste mistake.

### Task 2: Raise `--max-rss-mb` suggested value in the 5 opt-in-default files (spec §4a)

**Files:** `tools/observation_vector/activity_clock_bv_comparison.cpp`,
`tools/observation_vector/imbalance_clock_manager_ratio_eval.cpp`,
`tools/observation_vector/imbalance_screen1_hurst_eval.cpp`,
`tools/observation_vector/imbalance_work_rate_eval.cpp`,
`tools/observation_vector/ImbalanceEntropyDivergenceEval.cpp`.

- [ ] In each file, update every occurrence of `4096` in the usage-string/comment context
  (`[--max-rss-mb 4096]`) to `8192`. Do **not** change the underlying default variable (`maxRssMB =
  0`, opt-in-only) — this task is cosmetic consistency with Task 1's new convention, not a
  behavior change.
- [ ] Rebuild each tool to confirm a clean compile.

### Task 3: Raise `FeatureSaliencyEval.cpp`'s `--max-observations` default (spec §4b)

**Files:** `tools/observation_vector/FeatureSaliencyEval.cpp`.

- [ ] Change `std::size_t maxObservations = 500'000;` → `std::size_t maxObservations = 2'000'000;`.
  Update the adjacent comment (currently "bounded default, spec §7 -- never unbounded") to note the
  Puget-headroom rationale from spec §4b (≈160MB reservoir array at the new cap, trivially inside
  the new 8GB ceiling).
- [ ] Rebuild via the file's own build-command comment; confirm clean compile.
- [ ] **Do not** run this against real data yet — that happens in Task 4, after Task 5/6's
  best-of-N restart support lands, so the re-validation run in Task 4 can optionally also exercise
  `--restarts` if useful. (If Task 4 is run before Tasks 5-6 for any reason, `--restarts` simply
  isn't available yet and the run defaults to single-restart — still valid, just record which mode
  was used in the ledger row.)

### Task 4: Re-validate Feature Saliency EM output at the new sample size (spec §4b)

**Files:** none (execution task) — writes a new row to `tools/RECALIBRATION_LEDGER.md` (via
`ToolProgressLogger`'s existing auto-append mechanism, not a manual edit).

- [ ] Run `feature_saliency_eval` against the full `mes_candidates.parquet`
  (274,893,510 rows) with the new `--max-observations 2000000` default (same `k=4`,
  `--max-iterations 500`, `--seed 13` as the 2026-09-11 09:48 baseline run, for a controlled
  comparison — vary only the sample size in this task).
- [ ] Compare the resulting `phi`/`mu`/`var` per dim against the 2026-09-11 09:48 ledger row's
  values (baseline: `mean_rev_z`, `fisher_info`, `relative_range`, `lempel_ziv`, `log_scale_ratio`
  salient; `burstiness_index`, `hurst_exponent`, `amihud_illiquidity`, `liq_fragility`,
  `fractal_dim` zero-`phi`).
- [ ] Record the outcome explicitly in `tools/RECALIBRATION_LEDGER.md`'s auto-appended row's
  Status column (hand-edited after the run, per the ledger's own convention): either "REVIEWED --
  consistent with 500K-sample baseline, no salient/non-salient split change" or, if any dim's
  `phi` crossed the zero/non-zero boundary, flag it explicitly as new information for the Elite
  Feature Set Curation initiative (do not silently treat it as replacing the old baseline without
  this flag).

### Task 5: Implement `FitFeatureSaliencyEMBestOf` (spec §4c)

**Files:** `tools/observation_vector/FeatureSaliencyEM.h`, its existing test file
(`tools/observation_vector/test_feature_saliency_em.cpp`).

- [ ] **Step 1: Write the failing test** — using the existing synthetic 2-cluster fixture(s)
  already in the test file, assert `FitFeatureSaliencyEMBestOf(observations, params, numRestarts=8,
  baseSeed=13)` returns a `FitResult<D>` whose final log-likelihood is `>=` every individual
  single-restart `FitFeatureSaliencyEM` call across the same 8 derived seeds (never worse than the
  best of the restarts it ran).
- [ ] **Step 2: Add a second test** using the known local-optima-prone fixture referenced in
  `docs/superpowers/specs/2026-09-07-feature-saliency-em-fitter-spec.md`'s robustness-sweep
  discussion (2/28 misses) — assert best-of-N with a modest restart count (e.g. 8) recovers the
  correct cluster labeling on a case where a fixed single-seed restart is known to mislabel.
- [ ] **Step 3: Run to verify both fail** (function doesn't exist yet).
- [ ] **Step 4: Implement** `FitFeatureSaliencyEMBestOf<D>(observations, params, numRestarts,
  baseSeed)`: loop `i` in `[0, numRestarts)`, call the existing unmodified
  `FitFeatureSaliencyEM<D>(observations, params, /*seed=*/baseSeed + i)`, track the result with the
  highest `ComputeLogLikelihood` (already computed internally per fit — reuse, don't recompute),
  return it. No changes to `FitFeatureSaliencyEM` itself, `ComputeStateResponsibilities`,
  `ComputeFeatureResponsibilities`, or `ApplyHardSaliencyPruning`.
- [ ] **Step 5: Run to verify both new tests pass, and the full existing suite still passes with
  zero regressions** (all prior native checks, e.g. the 19/19 baseline cited in the 2026-09-09
  plan, unchanged).

### Task 6: Wire `--restarts` into `FeatureSaliencyEval.cpp` (spec §4c)

**Files:** `tools/observation_vector/FeatureSaliencyEval.cpp`.

- [ ] Add `--restarts N` CLI flag (default `8`), parsed alongside the existing flags.
- [ ] Call `FitFeatureSaliencyEMBestOf` instead of the current single-fit call when `--restarts >
  1`; when `--restarts == 1`, call the existing `FitFeatureSaliencyEM` directly (exact old
  behavior, zero-cost escape hatch for reproducing prior runs bit-for-bit).
- [ ] Update the usage string to document the new flag and its default.
- [ ] Rebuild; confirm clean compile.

### Task 7: Final verification

- [ ] Re-run the full native test suite for `tools/observation_vector/` (whatever the existing
  invocation convention is — check `test_feature_saliency_em.cpp`'s own build/run comment) and
  confirm `ALL PASS`.
- [ ] Confirm no file outside this plan's Tasks 1-6 was touched (`git diff --stat`).
- [ ] Update `tools/RECALIBRATION_LEDGER.md` is left in a consistent state (Task 4's row correctly
  annotated, no other rows disturbed).
