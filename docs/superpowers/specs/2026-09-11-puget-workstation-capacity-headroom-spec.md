# Puget Workstation Capacity Headroom: Revisiting Old-Machine-Tuned Defaults

## 1. Origin

`docs/NEW_MACHINE_WSL_SETUP.md` §0b (added 2026-09-11, once the Puget Workstation's factory
benchmark run confirmed healthy hardware) measured the real headroom jump vs. the outgoing Dell
box: Intel Xeon E5-1603 v3 (4 cores / 4 threads, 2014-era Haswell-EP) → AMD Ryzen 9 9950X (16
cores / 32 threads), 15GiB RAM / 0B swap → 96GB RAM, no discrete GPU → RTX 5080 (16GB VRAM).
Roughly **6.4× RAM, 4× cores, 8× threads**. That section explicitly flagged a list of RAM/CPU-
scarcity-driven defaults as "not yet acted on — flagged for a deliberate decision, not silently
changed" pending a dedicated pass. This spec is that pass.

## 2. Problem

Several `tools/` utilities hardcode memory ceilings, sampling caps, and single-threaded/single-
restart algorithm choices that were reasonable, deliberate engineering decisions against the old
machine's real constraints (in particular a real 2026-09-03 OOM incident — 4 concurrent
`observation_vector_recalibration.cpp` passes over the same 471.9M-row file exhausted RAM with no
swap configured and took down the whole VS Code/WSL session). Carrying those exact numbers forward
onto a 6.4×-RAM machine either leaves free headroom on the table (overly conservative
`--max-rss-mb` ceilings that could never legitimately trigger there) or leaves a real correctness
question open (the Feature Saliency EM fitter's single-restart k-means++ initialization, already
self-flagged as a known local-optima risk whose mitigation was previously deferred for compute-cost
reasons that mostly no longer apply).

Two categories, requiring different treatment:

- **Pure safety-net ceilings** (`--max-rss-mb`): raising these is a mechanical, output-neutral
  change. The flag is a fail-fast circuit breaker (`ToolProgressLogger::CheckMemoryBudget()`
  throws before an OS-level OOM-kill) — it does not change what a tool computes, only how much
  headroom it's given before aborting. Safe to bump broadly.
- **Statistical/algorithmic caps** (`FeatureSaliencyEval.cpp`'s `--max-observations` reservoir
  cap; `FeatureSaliencyEM.h`'s single-restart k-means++ seeding): these change *what gets computed*
  (a larger sample, or a best-of-N restart policy), not just how much RAM is allowed. Any change
  here requires acknowledging that previously-recorded results
  (`tools/RECALIBRATION_LEDGER.md`'s 2026-09-11 09:48 `feature_saliency_eval` row) may no longer
  be reproduced bit-for-bit, and a fresh validation run is required before the new default is
  trusted.

## 3. Inventory (current state, verified by direct file read, 2026-09-11)

| File | Mechanism | Current default | Category |
|---|---|---|---|
| `tools/observation_vector/observation_vector_recalibration.cpp` | `--max-rss-mb` | `3072` (hardcoded, always enforced) | Safety-net ceiling |
| `tools/market_data_replay/MarketDataReplay.cpp` | `--max-rss-mb` | `3072` (hardcoded, always enforced) | Safety-net ceiling |
| `tools/observation_vector/whole_vector_redundancy_eval.cpp` | `--max-rss-mb` | `3072` (hardcoded, always enforced) | Safety-net ceiling |
| `tools/observation_vector/activity_clock_bv_comparison.cpp` | `--max-rss-mb` | `0` (opt-in; help text suggests `4096`) | Safety-net ceiling |
| `tools/observation_vector/imbalance_clock_manager_ratio_eval.cpp` | `--max-rss-mb` | `0` (opt-in; help text suggests `4096`) | Safety-net ceiling |
| `tools/observation_vector/imbalance_screen1_hurst_eval.cpp` | `--max-rss-mb` | `0` (opt-in; help text suggests `4096`) | Safety-net ceiling |
| `tools/observation_vector/imbalance_work_rate_eval.cpp` | `--max-rss-mb` | `0` (opt-in; help text suggests `4096`) | Safety-net ceiling |
| `tools/observation_vector/ImbalanceEntropyDivergenceEval.cpp` | `--max-rss-mb` | `0` (opt-in; help text suggests `4096`) | Safety-net ceiling |
| `tools/observation_vector/FeatureSaliencyEval.cpp` | `--max-observations` reservoir cap | `500000` (hardcoded) | Statistical/algorithmic cap |
| `tools/observation_vector/FeatureSaliencyEM.h` | k-means++ initialization | single restart, no best-of-N | Statistical/algorithmic choice |

Also relevant, not code: 0B swap on the old machine was an unconfigured default, not a deliberate
choice (§0b), and is a Puget OS-level decision, not a repo change.

Explicitly **not** in this inventory (out of scope, see §5): the RTX 5080 / `lbrnet` GPU-dispatch
gap (§0c/§0b's own note — no `device='cuda'`-style dispatch exists in `lbrnet` training scripts;
that repo owns ML training logic per this repo's own boundary rule) and the Feature Saliency EM's
own zero-`phi` dim-drop question (`tools/RECALIBRATION_LEDGER.md`'s 2026-09-11 09:48 row — a
scientific/selection decision belonging to the Elite Feature Set Curation initiative, not a
capacity question).

## 4. Decision

### 4a. `--max-rss-mb` ceilings — RAISE, mechanical, all 8 files

**Sizing rationale.** The original `3072`/`4096` values were sized, per
`observation_vector_recalibration.cpp`'s own comment, for "3-4 concurrent passes sharing a ~15GB
box" (≈3.75–5GB/pass). Puget's 96GB affords the same concurrency-safety philosophy at a
proportionally higher ceiling — but the right number is **not** a naive 6.4× multiply of the old
value, because on a dev workstation (unlike a possibly-dedicated old box) some RAM is legitimately
reserved for Sierra Chart, VS Code/WSL, and other concurrent work, not 100% available to `tools/`
batch jobs. Adopt **8192MB (8GB) per pass** as the new shared default across all 8 files: preserves
headroom for 4 concurrent passes (32GB, one third of 96GB) while leaving the remaining ~64GB free
for everything else running on the box, and is still a >2× increase over every existing default
(safety headroom growing, not just matching the old ratio). This is a starting default, not a hard
ceiling — the flag remains fully overridable per the existing convention.

**Action per category:**
- The 3 hardcoded-and-always-enforced defaults (`observation_vector_recalibration.cpp`,
  `MarketDataReplay.cpp`, `whole_vector_redundancy_eval.cpp`): change `3072` → `8192`, and update
  each file's own comment that cites "a ~15GB box" to record the Puget headroom and the new
  rationale (so the comment doesn't silently go stale the way `docs/NEW_MACHINE_WSL_SETUP.md` §0b
  already flagged it as doing).
- The 5 opt-in-default (`0`) files: change their `--help`/usage-string suggested value from `4096`
  to `8192` for consistency (cosmetic — the enforced behavior doesn't change unless the operator
  passes the flag either way, since these tools don't enforce a budget by default at all). No
  functional risk.

**Why this is safe (not just convenient):** raising a fail-fast ceiling can only ever make a
previously-aborting run now succeed, or leave an already-succeeding run's behavior completely
unchanged — it cannot silently corrupt output, mask a real leak below the new (still-finite)
ceiling, or change any computed statistic. `ToolProgressLogger::CheckMemoryBudget()`'s own
docstring already frames this as fail-fast defense-in-depth, not a resource-shaping mechanism.

### 4b. `FeatureSaliencyEval.cpp`'s `--max-observations` reservoir cap — RAISE, but gated on re-validation

The `500000` cap was a deliberate bound (spec §7: "a genuine EM fit needs random-access passes over
the full observation set", so the whole 274.9M-row `mes_candidates.parquet` can't be materialized
directly — reservoir sampling is still required regardless of RAM). Puget's headroom changes what
a *safe* reservoir size is, not whether reservoir sampling itself is still needed. Raise the default
to **2,000,000** (4× increase; back-of-envelope: 2M observations × 10 D × 8 bytes (double) ≈ 160MB
for the reservoir array itself, trivially inside the new 8GB ceiling with enormous margin — the
constraint was never actually RAM-bound at 500K, it was a deliberately conservative choice, so this
is genuinely free headroom, not a risky push against a real wall).

**Required companion step, not optional:** re-run `feature_saliency_eval` at the new cap and record
a fresh `tools/RECALIBRATION_LEDGER.md` row comparing its `phi`/`mu`/`var` output against the
2026-09-11 09:48 row (the current `500000`-cap baseline: 5 of 10 dims salient — `mean_rev_z`,
`fisher_info`, `relative_range`, `lempel_ziv`, `log_scale_ratio`). If the salient/non-salient split
changes, that is real new information for the Elite Feature Set Curation initiative and must be
flagged as such, not silently overwritten.

### 4c. `FeatureSaliencyEM.h`'s single-restart k-means++ — IMPLEMENT best-of-N restart

`docs/superpowers/specs/2026-09-07-feature-saliency-em-fitter-spec.md` (§ "EM local-optima
sensitivity") already identifies this as a known, real limitation — "the current single-restart
k-means++ init can [land in a local optimum]... consider multiple-restart (best-of-N
log-likelihood) fitting" — previously left unmitigated, plausibly for CPU-cost reasons that no
longer hold at 8×-the-threads, and each individual restart is cheap (the existing synthetic
recovery/robustness sweep already fits repeatedly in seconds on the *old* hardware).

**Design:** additive, not a rewrite of the existing (tested, working) single-fit path.
`FitFeatureSaliencyEM<D>()` keeps its exact current signature and behavior. A new
`FitFeatureSaliencyEMBestOf<D>(observations, params, numRestarts, baseSeed)` wrapper runs
`FitFeatureSaliencyEM` `numRestarts` times (varying only the k-means++ seed derived from
`baseSeed`), and returns the `FitResult<D>` with the highest final log-likelihood
(`ComputeLogLikelihood`, already computed as part of each fit's own convergence check — no new
metric to invent). Existing single-fit tests are untouched; new tests cover: (a) best-of-N never
returns a result worse (by log-likelihood) than a fixed single restart on the same seed set, (b) on
the existing known-local-optima-prone synthetic fixture (the 2/28 robustness-sweep misses cited in
the plan for `2026-09-07-feature-saliency-em-fitter-spec.md`), best-of-N with a modest restart
count recovers the correct labeling where a single restart sometimes doesn't.

`FeatureSaliencyEval.cpp`'s CLI gains a `--restarts N` flag (default a conservative but real value,
e.g. `8` — cheap given the new core count, and callers needing the old single-restart behavior for
exact reproducibility can still pass `--restarts 1`).

### 4d. Swap configuration — operational recommendation, not a code change

`docs/NEW_MACHINE_WSL_SETUP.md` §0b already raises this ("worth deciding deliberately whether
Puget should have swap as a safety net"). This spec's recommendation: configure a modest swap file
(16–32GB) inside the WSL2 instance purely as a last-resort safety net against a genuine runaway
process, not as a performance requirement — 96GB is already far more than any current `tools/`
workload needs, and swap should never be relied on for real throughput given its `.wslconfig`-level
configuration. This is an operator action on the Puget machine itself (`.wslconfig`'s `memory=`/
`swap=` keys), not a change tracked by this repo; recorded here only so it isn't lost as a decided
recommendation.

## 5. Explicitly out of scope (do not touch as part of this initiative)

1. **RTX 5080 / GPU-accelerated training dispatch** — owned by `lbrnet` (this repo's own boundary
   rule: "no ML training logic — belongs in lbrnet"). `docs/NEW_MACHINE_WSL_SETUP.md` §0c already
   has its own GPU bring-up checklist; any CUDA dispatch code change is a separate `lbrnet`-side
   initiative, not this one.
2. **Whether the Feature Saliency EM's zero-`phi` dims should be dropped from the observation
   vector** — a scientific/selection decision under the Elite Feature Set Curation initiative
   (`docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md`), independent of
   how large a sample or how many restarts fit them. This spec only changes the *inputs* to that
   still-open decision (a larger, more-restart-robust fit), never the decision itself.
3. **Parallelizing `FeatureSaliencyEM.h`'s E-step across observations** — `docs/NEW_MACHINE_WSL_
   SETUP.md` §0b flags this as "embarrassingly parallel... a cheap win if fit time becomes a
   bottleneck on Puget" but explicitly not yet needed (a 22-minute single-threaded fit is not
   currently blocking anything). Deferred until it is actually a bottleneck, to avoid adding
   concurrency-correctness risk for no measured benefit.
4. **`--max-rss-mb` values for any tool not listed in §3** — this spec's inventory is exhaustive
   for the files `docs/NEW_MACHINE_WSL_SETUP.md` §0b actually named; if other tools are found later
   with their own old-machine-tuned ceiling, they should get their own follow-up, not be silently
   swept in here.

## 6. Acceptance / done criteria

- All 8 files in §3's `--max-rss-mb` inventory updated per §4a; native build
  (`mamba run -n mts g++ ...`, per each tool's own build-command comment) succeeds for each
  changed file.
- `FeatureSaliencyEval.cpp`'s new `--max-observations 2000000` default re-run against
  `mes_candidates.parquet`, producing a new `tools/RECALIBRATION_LEDGER.md` row that explicitly
  compares against the 2026-09-11 09:48 baseline row.
- `FeatureSaliencyEM.h` gains `FitFeatureSaliencyEMBestOf`, with native tests per §4c's two
  required cases, all passing alongside the existing 19/19 (or current count) test suite with zero
  regressions.
- `FeatureSaliencyEval.cpp`'s `--restarts` flag wired end-to-end and documented in its own usage
  string.
- No change to any file/mechanism listed in §5.
