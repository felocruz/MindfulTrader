# Spec: Institutional Elevation of the 16D Observation Vector & FeatureScaler

Date: 2026-08-13
Owner: C++ execution layer (MindfulTrader)
Scope: `FeatureScaler.h`, `InformationEngine.h`, `StudyHelperFunctions.cpp`, `TailRiskEngine.h`,
`ContextManager.cpp`, `StructureEngine.h/.cpp`
Prerequisite reading: `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md` (source of
every citation below — this spec implements that doc's 2026-08-13 consensus-practice findings),
`docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md` (D1/D2 background —
the defect Unit 1 below finishes fixing).

## Purpose

The 16D observation vector trains a Student-t HMM on fat-tailed ES/MES data. Every calculator and the
shared scaling layer (`FeatureScaler`) should reflect consensus institutional practice for fat-tailed
financial time series (the Shannon/Mandelbrot/Taleb/Pareto "Gang"), not just internally-consistent
engineering choices. This spec turns the 2026-08-13 literature-grounding pass into six independently
landable units of work, each with its own scope, test plan, and acceptance criteria — same pattern as
the hardening spec's D1/D2/D3 split, extended repo-wide.

**Sequencing:** priority order (Unit 1 first — it's actively corrupting live training data), but no
unit blocks another. Each unit is its own commit, following this repo's established convention (direct
to `master`, no worktrees — confirmed standing default).

---

## Unit 1 (Tier 1): `FeatureScaler` dedupe-at-ingestion

### Problem

`FeatureScaler`'s median+MAD rolling estimator (`FeatureScaler.h:205`, correct choice for fat-tailed
data, not in question) collapses to an exact `z=0` when a value dominates its rolling window. D1's
carry-forward fix (raw calculators hold the last valid physics reading instead of fabricating a
sentinel) is correct but has a side effect: a carried-forward value repeats in the window many times in
a row, and the estimator legitimately converges its median onto whatever dominates its input. Measured
in live `.context` data (200k-pair sample, cross-verified by two independent readers, 2026-08-13): dims
1 (`burstiness_index`) and 2 (`relative_range`) land at exact zero in 71.4% and 61.4% of rows
respectively; four more dims (4, 10, 11, 12) in 31-47%.

A repeated value carries zero marginal Shannon information (`H(X_t | X_{t-1}) = 0`) and shouldn't be
treated as a fresh independent draw by a location/scale estimator. Per the Gang spec's Cross-Pillar
section, this is a *principled synthesis* of market-microstructure stale-tick-exclusion practice and
HMM missing-data-marginalization practice — not an implementation of one named technique, and should be
represented that way, not overclaimed.

### Design

In `FeatureScaler::UpdateAndNormalize()`'s Step 1 buffer-update loop (`FeatureScaler.h`, currently
~lines 262-277), skip `push_back` when the value about to be pushed equals `buf.back()`:

- SOFTLOGZ dims: compare `raw[i]` against `stateBuffers[i].back()` before pushing.
- LOGZ dims: compare the post-`ToLogEnergy()` value against `logBuffers[i].back()` before pushing (the
  transform is a monotonic function of `|raw[i]|`, so this is equivalent to deduping on the raw value).
- Guard the comparison on `!buf.empty()` (always push the first sample).

`RingBuffer<T,N>::back()` already exists (`include/RingBuffer.h:45`) — no new data structure needed.
Confined entirely to `FeatureScaler.h`; no `ContextManager.cpp` change (matches the file's existing
"zero ACSIL/Logger dependency by design" comment).

**Effect on warmup/calibration:** `RobustLocation()` already handles variable buffer occupancy
(`n<5 → {0,0}`, otherwise operates on `buf.size()` as-is) — the `sampleCount >= RANK_WINDOW` warmup gate
needs no change. During a repeat-heavy stretch, affected dims' buffers grow more slowly than
`sampleCount` — intended (a window of *informative* observations, not raw ticks) but worth an explicit
test.

**D2 diagnostic as acceptance signal:** `ComputeValueDominance()` (`FeatureScaler.h:233`) needs no
change — post-fix, dominance-ratio ALERTs for dims 1/2/4/10/11/12 should drop toward the baseline rate
already seen on clean dims, since duplicates no longer enter the sorted window it scans.

### Test Plan (write failing first, per `superpowers:test-driven-development`)

1. Unit test: feed `FeatureScaler` a synthetic per-dim series with an injected run of N identical
   values (simulating a carry-forward stretch) followed by resumed variation; assert the *held* value's
   emitted z-score is nonzero and the window's `size()` stops growing during the repeat run.
2. Unit test: assert `ComputeValueDominance()` no longer reports high dominance for a window built
   through the deduped ingestion path, given the same synthetic repeat-injection fixture the hardening
   spec's own test plan proposed.
3. Regression test: existing `FeatureScaler` warmup/calibration tests continue passing unmodified (no
   behavior change for streams with no repeats).

### Acceptance Criteria

1. Re-run `context_preflight.py` against a freshly-collected `.context` file post-deploy: dims 1/2's
   `zero_ratio` drops to a level consistent with genuine cold-start only (matches the hardening spec's
   original Acceptance Criterion #1, still unmet — this unit is what finally satisfies it).
2. Live log shows `FeatureScaler dominance ALERT` frequency for dims 1/2/4/10/11/12 drop to baseline.
3. No change in `.context` sequence continuity, NaN/Inf counts, or record throughput (structural
   integrity, already verified clean, must stay clean).

### Rollback

Single-file, single-condition change — revert `FeatureScaler.h` if a regression surfaces.

---

## Unit 2 (Tier 2): three independent literature-concrete swaps

Each item below is its own commit; none depends on the others.

### 2a. Shannon — Miller-Madow entropy bias correction

**File:** `InformationEngine.h:294-311` (`GetShannonEntropy()`), constants at lines 28-29
(`NUM_BINS=10`, `WINDOW_SIZE_P=50`).
**Change:** entropy is already computed in bits (`log2`) via a plug-in histogram estimator. Add the
Miller-Madow correction — subtract `bias = (m-1) / (2·N·ln2)` bits, where `m` = number of bins with
nonzero count (not `NUM_BINS` unconditionally — the classic Miller-Madow correction uses the count of
*occupied* bins) and `N = m_countP`. Confirmed still-current consensus practice (2026-08-13 grounding
pass), not superseded for an O(bins) hot-path constraint.
**Test:** unit test with a known small-N histogram, assert corrected entropy matches the closed-form
bias-subtracted value; assert correction vanishes as N grows large (consistency check).

### 2b. Mandelbrot — RQA epsilon via target recurrence rate

**File:** `StudyHelperFunctions.cpp:3297-3298`.
**Change:** replace `epsilon = max(range*0.1, std*0.5)` with a small bisection search over epsilon that
targets a fixed recurrence rate (RR≈0.02-0.05, per Schinkel et al. 2008), reusing the same recurrence
matrix already computed — no new O(n²) cost, just a different threshold-selection rule.
**Test:** unit test asserting the resulting RR stays within tolerance of the target across both a
low-volatility and a high-volatility synthetic price series (the exact scale-invariance property this
replaces the old heuristic for).

### 2c. Pareto — Hill-plot stability-region k-selection + EWMA-smoothed alpha

**Files:** `TailRiskEngine.h:31` (windowSize/tailPercent), `ContextManager.cpp:512` (warmup gate,
unaffected).
**Change:** replace the fixed `tailPercent=0.05` with a Hill-plot stability-region selector — compute
Hill(k) across a k-range (e.g. 10-100), find the widest window where values stay within a fixed band,
take its midpoint as k. EWMA-smooth the resulting alpha series before it's used as the HMM feature
(honestly labeled in the Gang spec as a practitioner-precedented engineering choice, not literature-
prescribed).
**Test:** unit test against a synthetic Pareto-tailed series with known alpha, assert the selector
recovers a k in a sane stability region; unit test for the EWMA smoothing's convergence behavior.

### 2d. (Deferred to Unit 4) — no separate 2d; kept as three items per the grounding doc.

---

## Unit 3 (Tier 3): Bowley skewness / Moors kurtosis — direct replacement, no Winsorize interim

**Files:** `StudyHelperFunctions.cpp:2639-2701` (`CalculateRealizedKurtosis`, `KURT_WINDOW=100`),
`StudyHelperFunctions.cpp:2709-2770` (`CalculateSkewness`, `SKEW_WINDOW=100`).

### Problem

Moment-based skewness/kurtosis are outlier-sensitive exactly under the fat-tailed conditions they exist
to detect (Kim & White 2004) — already escalated to `pending-replacement` in the Gang spec. This dim
also feeds a **live** risk gate (`Indicator.cpp:505`'s `>4.0 = Fat Tail Risk`, `Scoring.cpp`'s fragility
sigmoid, `RiskManager.cpp`'s cascade-count checks), so correctness here has direct trading consequences,
not just noisier ML input.

### Design

Replace both calculators' final formula with their Kim & White-recommended robust alternative:

- **Skewness → Bowley (1920) quartile skewness:** `SK_B = (Q3 − 2·Q2 + Q1) / (Q3 − Q1)`.
- **Kurtosis → Moors (1988) octile kurtosis:**
  `Ku_M = [Q(7/8) − Q(5/8) + Q(3/8) − Q(1/8)] / [Q(6/8) − Q(2/8)]`.

Both need order statistics (quantiles) over the rolling 100-bar window, not a running moment sum — a
materially different accumulator shape than the current sum-based formula.

**First task, before either formula lands:** benchmark a plain full re-sort (`std::nth_element`/
`std::sort` over the 100-element window each 15-min bar) against a t-digest (Dunning & Ertl 2019,
production-standard for rolling-window quantiles) for the actual per-bar CPU cost on this hot path.
Per your direction, this benchmark decides the *implementation structure*, not *whether* to make the
swap — Bowley/Moors ships either way, no Winsorize interim step.
**Decision rule:** if full re-sort profiles cheap enough at 15-min cadence (expected, given N=100 is
small), use it — simplest, no new data structure. Only reach for t-digest if profiling shows the sort
is a measurable cost on this hot path.

Live risk-gate thresholds (`>4.0` Fat Tail Risk, fragility sigmoid breakpoints, cascade-count checks)
must be re-validated against the new statistic's empirical distribution before deploy — Moors kurtosis
normalizes to ≈1.23 under N(0,1), a different scale than excess-kurtosis's 0 baseline, so a naive
threshold carry-over would be wrong.

### Test Plan

1. Unit test: quantile-window benchmark harness (informs the structure decision, not a pass/fail gate).
2. Unit test: Bowley/Moors formulas against known reference distributions (e.g. verify Moors≈1.23 on a
   synthetic Gaussian window).
3. Unit test: outlier-injection fixture — assert a single extreme tick no longer produces the spurious
   kurtosis spike the moment-based formula was vulnerable to (the exact failure mode Kim & White
   describe).
4. Re-derive and document the new `>4.0`-equivalent threshold and any dependent multiplier/sigmoid
   breakpoints against the new statistic's distribution — do not carry over old thresholds unchanged.

### Acceptance Criteria

1. `CalculateRealizedKurtosis`/`CalculateSkewness` emit Moors/Bowley values.
2. All three live risk-gate consumers (`Indicator.cpp`, `Scoring.cpp`, `RiskManager.cpp`) re-validated
   against the new statistic's distribution, thresholds updated with documented rationale.
3. Existing kurtosis/skewness carry-forward behavior (D1, already shipped) is preserved — the
   degenerate-input carry-forward state now holds the last valid *Moors/Bowley* reading, not a moment-
   based one.

### Rollback

Two independent calculator functions — revert either independently if its threshold re-validation
surfaces a live-trading regression.

---

## Unit 4 (Tier 4): citation and mislabeling hygiene

Zero functional risk, no tests required beyond existing coverage staying green.

1. `StudyHelperFunctions.cpp:2676-2686` — credit kurtosis bias-correction to Joanes & Gill (1998), not
   "Sierra Chart's approach."
2. `ContextManager.cpp:693-725` — credit "Taleb Cliff" Chandelier parameters to LeBeau, not an implied
   Taleb citation.
3. `StudyHelperFunctions.cpp:2333` (`CalculatePathEfficiencySNR`) — correct attribution to Kaufman
   (1998) Efficiency Ratio; drop the "Spectral Entropy"/Elder framing (it isn't a Shannon-entropy
   measure).
4. `StudyHelperFunctions.cpp:2748` — drop the unsupported "Wyckoff-aligned" label on the skewness
   regime multiplier.
5. `StructureEngine.cpp:85-109` — fix the comment claiming Sevcik's method (it's an unlogged
   path-length/displacement ratio); either rename to stop citing Sevcik, or reimplement as true Sevcik
   if the log-log version is preferred (default: fix the comment, smaller change).
6. `ContextManager.cpp:685` — `paretoRot` field holds a Mandelbrot-pillar metric (fractal roughness from
   `StructureEngine::GetFractalDimension()`); rename field or reroute the correct Pareto-pillar metric
   into it (default: rename, since the metric itself is validated and just misplaced).
7. Remove three confirmed-dead functions (`StructureEngine::GetRecurrenceRate()`,
   `InformationEngine::GetFisherInformation()`, `InformationEngine::GetRecurrenceRate()`) — confirm zero
   call sites immediately before deletion (per repo's symbol-removal safety rule).

---

## Unit 5 (Tier 5): D3 `context_preflight.py` chronic-zero gate — deferred

Out of scope for this repo. `lbrnet/scripts/context_preflight.py` needs a `chronic_zero_threshold`
(default 0.15) that promotes `zero_ratio` to a `violations` entry — fully specified already in the
hardening spec's D3 section. Per your explicit instruction, this is `lbrnet`-session work, not
implemented here. This unit exists in this spec only as a pointer so it isn't lost.

---

## Unit 6 (Tier 6): two scoped research spikes

Investigation only — output is a finding recorded in the Gang spec's Changelog, not necessarily
production code. Neither blocks Units 1-4.

### 6a. DFA/Hurst empirical bias/CI correction

**Question:** is a Kristoufek (2010)-style empirical/percentile bias correction cheaply derivable (a
precomputed lookup table or closed-form curve) for this codebase's N≈100 DFA window, as an alternative
to widening the window (which trades off regime responsiveness)?
**Method:** Monte Carlo the DFA estimator's bias/variance at N≈100 on synthetic fractional Brownian
motion at known Hurst values (matching Kristoufek's methodology), see if a stable correction curve
emerges.
**Output:** either a concrete correction ready for a follow-on Unit-2-style implementation, or a
documented "still under-powered, no cheap fix found — window-widening is the only lever" conclusion.

### 6b. Hill estimator intraday seasonality

**Question:** does the Hill tail-index reading vary systematically by session time-of-day (open/midday/
close U-shape) in a way that biases the HMM, given no classical literature (built on daily data)
addresses 15-minute futures bars?
**Method:** compute Hill-plot stability across time-of-day buckets on existing collected `.context`/
historical replay data (sample via `lbrnet/scripts/`, per established tooling convention).
**Output:** either a deseasonalization adjustment recommendation for a follow-on unit, or a documented
"not a material effect at this cadence" conclusion.

---

## Cross-Unit Notes

- **TDD applies throughout** (per this repo's Guardrail 4): each unit's tests are written first and
  must fail against the pre-fix code before the production change lands.
- **No unit touches `lbrnet`** except as read-only sampling via existing `lbrnet/scripts/` tooling
  (Unit 6b) — matches the standing repo-boundary convention.
- **Documentation Sync Contract:** none of these units touch `CLAUDE.md`/`README-AI.md`/
  `.github/copilot-instructions.md`/`GEMINI.md` directly, so the four-mirror pre-commit gate shouldn't
  fire — flag if that assumption breaks during implementation.
