# Feature Saliency EM Fitter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development
> (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use
> checkbox (`- [ ]`) syntax for tracking.

**Goal:** Implement Phase 2a of the Elite Feature Set Curation initiative — a Gaussian mixture +
Feature Saliency EM fitter, native C++, per
`docs/superpowers/specs/2026-09-07-feature-saliency-em-fitter-spec.md`. Proves each candidate
`ObservationData` dim's actual regime-discriminative value (not just pairwise non-redundancy,
already answered by Phase 1) against synthetic ground truth first, then against real data from
`mes_candidates.parquet` (the dim-selection pipeline's own output).

**Architecture:** Pure numerical core (`FeatureSaliencyEM.h`, no I/O, unit-testable on synthetic
data) + a thin CLI driver (`FeatureSaliencyEval.cpp`, Parquet I/O only) — mirrors this repo's own
established `streaming_correlation_matrix.h`/`whole_vector_redundancy_eval.cpp` split. No
`CMakeLists.txt`/`build_dll.sh` changes — standalone `tools/` convention throughout.

**Tech stack:** C++17, native `check()`/`g_failures`/`ALL PASS` unit tests (no GoogleTest/CMake).
Phase 2a needs zero external numerical libraries (spec §4 — pure arithmetic, no digamma/root-find);
Eigen/Boost stay out of scope until Phase 2b.

**Spec:** `docs/superpowers/specs/2026-09-07-feature-saliency-em-fitter-spec.md` — read in full
before starting; every task below cites the section it implements.

## Institutional discipline (non-negotiable, carried from the spec's own standing rules)

- **Synthetic ground truth before real data** (spec §6) — Task 6 (synthetic recovery test) is the
  actual correctness proof; no task after it may run against real data until it passes.
- **Saliency pruning REQUIRED, not optional (supersedes spec §2.4's original deferral, found
  2026-09-09 via Task 5)** — raw likelihood-only EM cannot ever push a genuinely irrelevant
  feature's `φ_j` toward 0 (structural property, not a bug: the salient branch can never fit worse
  than background). Final design (`ApplyHardSaliencyPruning`, RESOLVED): a hard MDL/BIC
  compare-before-prune gate applied ONCE after plain EM converges (an earlier continuous
  soft-thresholding attempt, `ApplyMMLPruning`, was tried, partially fixed via an independent
  Gemini CLI consult, then ABANDONED as structurally unfixable per a second consult -- see
  `knowledge/global/cpp/feature_saliency_em_mml_pruning.md` for the full narrative). Validated:
  19/19 native tests pass; a broader robustness sweep (28 runs) shows 26/28 (93%) correct, with
  the 2 misses being a known EM local-optima/label-swap artifact, not a gate-math flaw.
- **No Phase 2b (Student-t) work** — explicitly deferred (spec §1); this plan is Gaussian-only.
- **Bounded memory** (spec §3/§7) — the CLI driver task must include an explicit
  `--max-observations` cap; unbounded in-memory materialization is the two-near-miss-OOM mistake
  this repo has already made twice this initiative.

---

### Task 1: Core data structures + k-means++ seeding (spec §5)

**Files:** New `tools/observation_vector/FeatureSaliencyEM.h`, new
`tools/observation_vector/test_feature_saliency_em.cpp`.

**Interfaces:** Fixed-D `std::array<double, D>` observations (template on `D`, matching this repo's
own `otg::kObservationDim`-style compile-time-sized convention), a plain `std::vector` of
observations as input (no Arrow dependency in this header at all, per spec §3).

- [x] **Step 1: Write the failing test** — k-means++ seeding on a small synthetic 2-cluster,
  1D dataset (well-separated clusters, e.g. centered at -5 and +5): assert the K=2 seeds picked are
  one from each cluster (not both from the same one) across a fixed-seed deterministic RNG, several
  repeated draws.
- [x] **Step 2: Run to verify it fails** (no implementation exists yet).
- [x] **Step 3: Implement** `KMeansPlusPlusSeed(observations, K, rngSeed) -> std::vector<size_t>`
  (returns the K seed indices) — D²-distance-weighted sampling per Arthur & Vassilvitskii (2007),
  matching `student_t_hmm.py`'s own `_kmeans_plusplus_seed_indices` algorithm (ported, not
  re-derived from scratch).
- [x] **Step 4: Run to verify it passes.** 3/3 pass (`kmeans_pp_picks_one_seed_per_cluster_across_
  repeated_draws`, `kmeans_pp_empty_input_returns_no_seeds`, `kmeans_pp_k_zero_returns_no_seeds`).

### Task 2: E-step (spec §2.2)

**Files:** Modify `FeatureSaliencyEM.h`, append to the test file.

- [x] **Step 1: Write the failing test** — hand-computable 2-observation, 1-feature, K=2 fixture:
  compute `w_{i,k}` and `u_{i,k,j}` by hand for a case where one observation clearly belongs to
  state 0 and the salient branch, the other to state 1 and the background branch; assert the
  computed responsibilities match within a tight epsilon.
- [x] **Step 2: Run to verify it fails.** (no `ComputeStateResponsibilities`/
  `ComputeFeatureResponsibilities` existed yet).
- [x] **Step 3: Implement** `ComputeStateResponsibilities(...)` (`w_{i,k}`, standard Bayes over
  `π_k · p(x_i|k)`) and `ComputeFeatureResponsibilities(...)` (`u_{i,k,j}`, the salient-vs-background
  branch split) exactly per spec §2.2's formulas — `p(x|k)` itself is the product over features of
  `φ_j·f(x_j|θ_{k,j}) + (1-φ_j)·q(x_j|λ_j)` (spec §2.1). Also added `FitParams<D>` (the shared
  per-component/per-feature parameter struct) and `GaussianDensity`/`ComponentDensity` helpers.
- [x] **Step 4: Run to verify it passes.** 5/5 new checks pass (all 8/8 total), hand-derived fixture
  (state0 mean=0/var=1, state1 mean=4/var=25, background mean=4/var=1 — deliberately centered at
  state1's own mean so background density exceeds state1's there — φ=0.6): obs A (x=0) gives
  w=0.873 (clearly state0), u=0.9998 (clearly salient); obs B (x=4) gives w=0.565 (state1, less
  dramatically) and u=0.231 (background-leaning), matching the two contrasting cases the task asked
  for.

### Task 3: M-step (spec §2.3)

**Files:** Modify `FeatureSaliencyEM.h`, append to the test file.

- [x] **Step 1: Write the failing test** — same hand-computable fixture as Task 2, extended: given
  known `w`/`u` responsibilities, assert the M-step's closed-form updates
  (`μ_{k,j}`/`σ²_{k,j}`/`μ_{λ,j}`/`σ²_{λ,j}`/`φ_j`/`π_k`) match hand-computed weighted-mean/-variance
  values within a tight epsilon.
- [x] **Step 2: Run to verify it fails.** (`ComputeMStepUpdate` didn't exist yet).
- [x] **Step 3: Implement** each closed-form update exactly per spec §2.3's formulas (no
  approximation, no root-finding — pure weighted arithmetic). Guard the shared variance floor the
  same way every other robust-scale calculator in this repo does (`kVarEps`-style, avoid
  divide-by-near-zero when a component's responsibility mass collapses).
- [x] **Step 4: Run to verify it passes.** 8/8 new checks pass (16/16 total). Used a simpler,
  purpose-built D=1/K=2/N=4 fixture rather than reusing Task 2's E-step output directly (given
  `w`/`u` as known inputs per the task's own framing) — hard state assignment (2 obs per state) +
  uniform `u=0.5` gives clean, exactly-verifiable numbers for both the state-specific AND background
  weighted stats simultaneously: `mu_{0,0}=1.5`/`var_{0,0}=0.25`, `mu_{1,0}=3.5`/`var_{1,0}=0.25`,
  `mu_bg=2.5`/`var_bg=1.25`, `phi_0=0.5`, `pi=[0.5,0.5]` — matched to 1e-9.

### Task 4: Convergence check + EM loop orchestration (spec §5)

**Files:** Modify `FeatureSaliencyEM.h`, append to the test file.

- [x] **Step 1: Write the failing test** — run the full E-step/M-step loop on a trivial fixture
  (e.g. two obviously-separated 1D clusters) for a fixed max-iteration count; assert log-likelihood
  is monotonically non-decreasing across iterations (the ascent-guarantee property McLachlan &
  Krishnan's cited theory promises) and that the loop terminates before hitting the iteration cap.
- [x] **Step 2: Run to verify it fails.** (`FitFeatureSaliencyEM` didn't exist yet).
- [x] **Step 3: Implement** `FitFeatureSaliencyEM(observations, K, maxIterations, tol) -> FitResult`
  (`FitResult` holds `π_k`, `μ_{k,j}`/`σ²_{k,j}`, `μ_{λ,j}`/`σ²_{λ,j}`, `φ_j`, final log-likelihood,
  iterations run) — one-sided relative log-likelihood plateau convergence, matching
  `_check_convergence`'s own formula (spec §5), k-means++ init from Task 1, `φ_j` initialized to
  1.0 for every feature (spec §5). Ported `_relative_delta`/`_check_convergence` from
  `student_t_hmm.py` verbatim (formula, not code). State/background variance initialized to the
  whole dataset's own per-feature variance (a reasonable, undocumented-by-spec GMM init default).
- [x] **Step 4: Run to verify it passes.** 3/3 new checks pass (19/19 total): converges at
  iteration 2 of a 50-iteration cap on the well-separated fixture, and log-likelihood is
  monotonically non-decreasing across every completed iteration (checked directly via a new
  `logLikelihoodHistory` field on `FitResult`, not just the final value).

### Task 5: Degenerate-input tests (spec §6, item 2)

**Files:** Append to the test file only, no new implementation expected (revised: a real
implementation gap was found, per the task's own "if either fails, that's a real bug" instruction).

- [x] **Step 1: Write the tests** -- (a) a feature identical across all planted components (zero true
  saliency) converges `φ_j` near 0; (b) a feature with an extreme, fully-separating difference
  across components converges `φ_j` near 1. D=2/K=2, 40+40 observations, feature0 planted salient
  (±5 separation), feature1 planted irrelevant (`N(0,1)` regardless of cluster), rngSeed=13.
- [x] **Step 2: Run -- found a REAL bug, not a new-code gap.** Both checks initially failed. Root
  cause #1 (fixed): `φ_j` initialized to exactly 1.0 is a mathematical absorbing fixed point of the
  E-step formula (`(1-φ)=0` makes `u=f/f=1` identically) -- fixed by initializing to `1.0-1e-3`.
  Root cause #2 (deeper, the actual reason this task exists): even after the init fix, an empirical
  sweep (N=40 to N=20,000 per cluster) showed raw likelihood-only EM has NO mechanism that can ever
  push an irrelevant feature's `φ_j` toward 0 -- it converges to ~0.998 regardless of sample size,
  structurally indistinguishable from the salient feature. This is a literature-grounded finding
  (MML pruning is required, not optional, per Law-Figueiredo-Jain 2004 extending Figueiredo & Jain
  2002) -- recorded in `knowledge/global/cpp/feature_saliency_em_mml_pruning.md` BEFORE
  implementing the fix, per operator instruction.
- [x] **Step 3: Implement `ApplyMMLPruning`** -- first attempt (numerator-only soft-threshold,
  applied every M-step) was UNSTABLE (a sweep across N=40..3000/several seeds showed frequent
  non-convergence and inconsistent salient-vs-irrelevant ranking) -- a real regression, not just
  miscalibration. An independent, read-only Gemini CLI literature/code-review consult
  (`gemini --approval-mode plan`, verified via `git status` to have made zero edits) diagnosed the
  root cause as an objective-function mismatch (MML-penalized M-steps optimize a penalized
  objective, but the convergence check still gated on RAW log-likelihood ascent, which is no
  longer guaranteed once pruning is active) and supplied two concrete fixes, both implemented: (a)
  `ApplyMMLPruning` corrected to shrink the denominator too (`max(0,sum-c/2)/max(eps,N-c/2)`, not
  just the numerator), (b) the EM loop's convergence gate switched from raw log-likelihood ascent
  to parameter-delta tracking (new `MaxAbsParamDelta` helper; `RelativeDelta`/`CheckConvergence`
  kept for parity/future non-MML consumers but no longer the loop's own gate).
- [x] **Step 4: Run -- confirmed both fixes real** (no more oscillation, previously-regressed
  salient-feature check passes again), but **`degenerate_planted_irrelevant_feature_converges_
  phi_near_zero` STILL FAILED** (`φ_1` stably converged to ≈0.764 at N=40, not <0.2). A broader
  sweep (N=40 to N=10,000/cluster) showed the separation was unreliable across sample size --
  `φ_1` never approached 0 at ANY tested N, and behavior was non-monotonic (one N failed to
  converge at all; another over-pruned the salient feature too). This was reported honestly rather
  than tuned further.
- [x] **Step 5: Second independent Gemini CLI consult (read-only, `--approval-mode plan`, verified
  via `git status` to have made zero edits)**, given the exact sweep table above. Diagnosis: the
  whole continuous-soft-thresholding approach is structurally unfixable, not miscalibrated -- a
  flat `O(1)` penalty can never overcome the `O(N)` responsibility-mass advantage a genuinely
  irrelevant feature's more-flexible K-Gaussian branch gets merely by overfitting noise. True MDL/
  BIC structural (L0) model selection needs a penalty scaling with `ln(N)`, evaluated as an
  explicit compare-before-prune decision. Concrete recommendation: replace `ApplyMMLPruning`
  entirely with a hard BIC gate applied once after plain EM converges (raw M-step for `φ_j`, no
  penalty during the loop at all; compare `LL_salient_j` vs `LL_background_j` per feature; prune
  to 0 iff `LL_salient_j - LL_background_j < (K-1)·ln(N)`, per Wilks'/Schwarz's BIC theory).
- [x] **Step 6: Implement `ApplyHardSaliencyPruning`** per the recommendation above --
  `ApplyMMLPruning` removed entirely; the EM loop reverted to plain, unmodified textbook EM (no
  penalty entangled in the M-step), restoring the ORIGINAL raw-log-likelihood-ascent convergence
  check (`MaxAbsParamDelta`/parameter-delta tracking removed as no longer needed -- dead code once
  the objective-function mismatch it existed to work around no longer applies). The hard gate is
  applied ONCE after the loop converges. **A second real bug was found and fixed during this
  implementation**: the background null-hypothesis density must be the GLOBAL, unweighted
  per-feature mean/variance (already computed at EM init time), NOT `params.bgMean`/`bgVar` --
  those are themselves `(1-u)`-weighted and degrade/narrow as `φ_j` rises, corrupting the null
  hypothesis (observed directly: `bgVar` collapsing to ~0.02 for a feature whose true variance was
  1.0).
- [x] **Step 7: Run to verify -- ALL 19/19 tests pass**, both previously-failing Task 5 checks
  included. A broader robustness sweep (N=40 to N=10,000/cluster × 4 seeds, 28 runs) shows 26/28
  (93%) correctly classify both planted-salient and planted-irrelevant features. **Task 5 is now
  FULLY COMPLETE.** The 2 sweep misses are a feature-label swap (both show `φ_0≈0, φ_1≈0.85-1.0`)
  -- a well-known EM local-optima/initialization-sensitivity artifact (multiple-restart/best-of-N
  fitting would mitigate it), not a flaw in the pruning gate's own math; flagged as a real, known,
  NOT-yet-mitigated limitation for Task 6/7/8, not silently accepted.

### Task 6: Synthetic recovery test — THE correctness proof (spec §6, item 1)

**Files:** Append to the test file only.

- [x] **Step 1: Write the test** — K=4 known components, D=8 features: dims 0-3 planted salient
  (a distinct ±5 sign pattern per state across all 4 salient dims simultaneously — e.g. state0
  `[+,+,+,+]`, state1 `[+,-,+,-]`, state2 `[-,+,-,+]`, state3 `[-,-,-,-]`, giving pairwise Euclidean
  separation ≥14 at jitter ±0.3, well beyond Task 5's single-dim design), dims 4-7 planted
  irrelevant (`N(0,1)`-equivalent regardless of assigned state, same jitter convention as Task 5's
  own background dims). 100 observations per state (400 total). Fixed-seed deterministic RNG
  (data-generating seed=2026, EM init `rngSeed=13`).
- [x] **Step 2: Run — PASSED on first attempt**, no implementation bug surfaced this time (Tasks
  1-5's fixes already cover the relevant mechanics). Both checks green: `φ_j > 0.8` for all 4
  planted-salient dims, `φ_j < 0.2` for all 4 planted-irrelevant dims (converged at iteration 202
  of a 300-iteration cap, `φ = [1,1,1,1 | 0,0,0,0]` exactly).
- [x] **Step 3: No real bug found — nothing to fix.** Per the task's own instruction, did NOT stop
  at one passing seed: ran a 10-seed EM-init robustness sweep (same data, `rngSeed ∈ {13,1,2,3,7,
  42,99,123,555,777}`) to rule out a lucky single-seed fluke. Result: **9/10 (90%) pass** — the
  single miss (`rngSeed=1`) shows `φ=[1,0,1,0 | 0,0,0,0]`, i.e. 2 of the 4 planted-salient dims
  pruned to 0. This is the SAME feature-label-swap / local-optima class Task 5's own 28-run
  robustness sweep already found and documented (26/28, 93%) — at K=4 with a single k-means++
  restart, some seeds land on a degenerate/merged-cluster local optimum where a subset of the truly
  discriminating dims look unnecessary for whatever reduced clustering was found. Not a new gate
  bug; consistent with the already-known, already-flagged limitation (multiple-restart/best-of-N
  fitting would mitigate it, not yet implemented).
- [x] **Step 4: Run to verify it passes with the real thresholds.** Committed test uses
  `rngSeed=13` (in the passing set) — 21/21 native checks pass (`ALL PASS`), thresholds exactly
  per spec §6 item 1 (`φ_j > 0.8` salient, `φ_j < 0.2` irrelevant), not weakened.

### Task 7: CLI driver — real data (spec §3, §6 item 3)

**Files:** New `tools/observation_vector/FeatureSaliencyEval.cpp`.

**Interfaces:** `parquet::arrow::FileReader` reading `mes_candidates.parquet`'s 10 candidate-dim
columns (the dim-selection pipeline's own output, spec §6's 2026-09-09 update) — column names via
`MTS::Schema::Contract::kObservationFieldNames[mdr::kCandidateDims[i]]`, same naming convention
`MarketDataReplay.cpp` already writes. `tools/ToolProgressLogger.h` for output.

- [ ] **Step 1:** Parse flags: `--input PATH.parquet`, `--k N` (component count, default 4 per
  `PRODUCTION_TRIAGE.md` row 1's production target), `--max-observations N` (bounded materialization
  cap, spec §7 — no unbounded default), `--max-iterations N`, `--seed N`.
- [ ] **Step 2:** Read the Parquet file's 10 candidate columns into an in-memory
  `std::vector<std::array<double, 10>>`, capped at `--max-observations` (subsample if the file has
  more rows than the cap — deterministic subsampling via the same seed, not arbitrary truncation).
- [ ] **Step 3:** Call `FitFeatureSaliencyEM(...)` (Task 4), report per-feature `φ_j` (with the real
  candidate dim NAME, not just an index) sorted descending, plus per-state `μ_{k,j}`/`σ²_{k,j}` for
  context, plus final log-likelihood/iteration count — via `ToolProgressLogger`, never bare
  `std::printf` (this repo's own standing rule for any tool that can run more than a few seconds).
- [ ] **Step 4:** Build via the standard `mamba run -n mts g++ ...` recipe (needs Arrow/Parquet for
  the reader only — `FeatureSaliencyEM.h` itself stays dependency-free per Task 1-6). Smoke-test
  against a bounded real sample before treating results as trustworthy.

### Task 8: Real-data run and reporting (spec §6 item 3, closing this phase)

**Files:** None — this is a run + a doc update, not new code.

- [ ] Run Task 7's CLI against `mes_candidates.parquet` (once the full 471.9M-tick generation run
  finishes) at K=4.
- [ ] Record the per-dim `φ_j` results in
  `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` (the curation
  ledger) as real evidence for each candidate dim's regime-discriminative value — per this
  project's own "report the numbers, don't auto-decide" posture (spec §2.4), this is input to a
  human/Phase-4 decision, not itself a pruning action.
- [ ] Flag the Gaussian-vs-Student-t caveat (spec §7) explicitly alongside the results — this is
  Phase 2a's result, not yet the final institutional answer.
