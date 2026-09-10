# Feature Saliency Mixture Fitter — Spec (Elite Feature Set Curation Phase 2)

**Status: DESIGN, NOT YET IMPLEMENTED.** Written 2026-09-07 per operator request ("Do we have the
spec / plan for the Feature Saliency fitting?" → "Yes"). Companion to
`docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` §4 Phase 2, which
states the design *decision* (what/why/where) but not the concrete math derivation, architecture,
or validation plan — this doc fills that gap before any code is written, per this project's own
"design before implementation" discipline.

## 0. Origin, scope, and the language decision (recorded accurately)

Phase 1 (`docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md`, same doc)
found essentially zero pairwise redundancy across the 11 genuinely calendar-clock-native
`ObservationData` dims (max |r|=0.34). That answers "are any two dims saying the same thing twice"
— it does **not** answer "does each dim actually help discriminate regimes at all." Phase 2 answers
the second question via **Feature Saliency** (Vaithyanathan & Dom 1999 → Law, Figueiredo & Jain
2004, IEEE TPAMI 26(9):1154-1166 → Fons, Dawson, Zeng, Keane & Iosifidis 2020's HMM/finance
extension): each candidate feature gets an explicit saliency weight, estimated *jointly* with the
mixture fit via EM, not a post-hoc score. Features whose saliency converges near zero are pruned.

**Why a fresh, standalone fit, not the production model**: `models/hmm_model.pkl`'s own K=4 state
boundaries were learned from a vector containing dims with known-broken formulas (the contamination
manifest, initiative doc §5a) — measuring saliency against its state assignments would be circular.
This tool fits its own small, purpose-built mixture from scratch on the corrected/current vector.

**Language decision, recorded honestly (not a unanimous verdict)**: an independent Gemini consult
(`lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_120`/`_REPLY`) recommended **against** a native C++
implementation — verdict: "Extend the existing Python `student_t_hmm.py` implementation," citing
1-2 days effort vs. 4-6 days for C++, and that an offline research tool gets zero benefit from C++'s
runtime-latency advantage. **The operator explicitly overrode this recommendation** (initiative doc
§3): `CLAUDE.md`'s "no ML training logic belongs in `lbrnet`" boundary was written for the
*production* model's live serving/retraining cadence, not a small standalone diagnostic fit with no
dependency on production code; Gemini's own effort estimates were judged unverified guesses (same
category as an earlier Gemini latency estimate later measured 200x wrong); every needed numerical
primitive was independently confirmed to have a real C++ equivalent (below). This doc treats "C++,
in `tools/`" as the standing decision, not open for re-litigation here — but records the dissenting
recommendation plainly rather than omitting it.

## 1. What is actually being built — scope narrowed on purpose, in two slices

**Phase 2a (this doc's actual near-term deliverable): Gaussian mixture + Feature Saliency**, matching
Law-Figueiredo-Jain (2004)'s own original formulation exactly — diagonal covariance, K components,
no transition matrix (treats observations as i.i.d., not a true time-ordered HMM), no sticky-Dirichlet
prior, no MAP priors, no ECME degrees-of-freedom refinement. This is the well-established, fully
closed-form version of the method (confirmed via `CLAUDE_BRIEF_120_REPLY`, §2 below).

**Phase 2b (explicitly deferred, NOT scoped by this doc): Student-t emissions**, matching this
system's real production architecture (`lbrnet/lbrnet/models/student_t_hmm.py`, diagonal covariance,
per-state degrees-of-freedom `ν_k`). **Why deferred, not just "later"**: combining Feature Saliency's
own latent variable (per-feature/per-state "is this the salient or background distribution"
assignment, `z_{i,j}`) with the Student-t mixture's own latent variable (per-observation/per-state
scale-mixture weight, the quantity `student_t_hmm.py`'s own DOF profile equation is built on) is a
genuine three-latent-variable joint E-step that neither `CLAUDE_BRIEF_120_REPLY` nor this session's
own literature search has independently derived or verified. Inventing that derivation from scratch
here would be exactly the kind of unverified-but-confident math this project's own discipline exists
to catch — see §5's open question. Phase 2a is deliberately scoped to the well-grounded, fully
closed-form Gaussian case first; Phase 2b needs its own targeted derivation/literature consult before
implementation, not a guess.

**Practical consequence of running Gaussian first**: Gaussian mixtures are not robust to outliers
the way Student-t is — a rare, extreme observation can disproportionately swing a Gaussian
component's mean/variance and, by extension, its apparent saliency for that feature. This is a real,
flagged limitation of Phase 2a's own result, not silently accepted: Phase 2b (once derived) is
partly *how* this gets checked, not merely a nicety — see §5's caveat on whether Student-t's own
bounded-influence robustness might dampen exactly the tail-driven saliency signal Feature Saliency
exists to detect (a real, unresolved tension raised independently in
`lbrnet/logs/rc_gemini.log` `RESEARCH_RESPONSE_005` §2).

## 2. Math (Phase 2a, Gaussian mixture + Feature Saliency)

### 2.1 Emission model

K components (states), diagonal covariance, D features (the 11 dims from Phase 1, or whatever
subset Phase 4 is evaluating). Component k's density for feature j:

```
f(x_j | θ_{k,j}) = Normal(x_j; μ_{k,j}, σ²_{k,j})
```

Background (shared, state-independent) density for feature j:

```
q(x_j | λ_j) = Normal(x_j; μ_{λ,j}, σ²_{λ,j})
```

Per Law-Figueiredo-Jain (2004), each feature has a scalar saliency `φ_j ∈ [0,1]` — the probability
mass on "this feature's value came from the state-dependent (salient) distribution" versus "came
from the shared background distribution regardless of state." Component k's density for the FULL
D-dimensional observation:

```
p(x | k) = Π_{j=1}^{D} [ φ_j · f(x_j | θ_{k,j}) + (1 - φ_j) · q(x_j | λ_j) ]
```

Mixture weights `π_k` (component priors) combine as usual: `p(x) = Σ_k π_k · p(x | k)`.

### 2.2 E-step

Two responsibility quantities per observation `i`:

- **State responsibility** `w_{i,k} = P(s_i = k | x_i)`, standard Bayes:
  `w_{i,k} = π_k p(x_i|k) / Σ_{k'} π_{k'} p(x_i|k')`.
- **Feature-level responsibility** `u_{i,k,j} = P(z_{i,j}=1 | x_i, s_i=k)` (was this feature's value
  generated by the salient or background branch, given the observation is in state k):
  `u_{i,k,j} = [φ_j f(x_{i,j}|θ_{k,j})] / [φ_j f(x_{i,j}|θ_{k,j}) + (1-φ_j) q(x_{i,j}|λ_j)]`.

### 2.3 M-step — confirmed closed-form (no new numerical optimization)

Per-state, per-feature parameters, weighted by BOTH responsibilities (only the "salient" fraction of
an observation's feature value should inform the state-specific parameter):

```
μ_{k,j}  = Σ_i w_{i,k} u_{i,k,j} x_{i,j}              / Σ_i w_{i,k} u_{i,k,j}
σ²_{k,j} = Σ_i w_{i,k} u_{i,k,j} (x_{i,j} - μ_{k,j})²  / Σ_i w_{i,k} u_{i,k,j}
```

Background (shared across states, weighted by the COMPLEMENT of feature-level responsibility):

```
μ_{λ,j}  = Σ_i Σ_k w_{i,k} (1-u_{i,k,j}) x_{i,j}             / Σ_i Σ_k w_{i,k} (1-u_{i,k,j})
σ²_{λ,j} = Σ_i Σ_k w_{i,k} (1-u_{i,k,j}) (x_{i,j}-μ_{λ,j})²  / Σ_i Σ_k w_{i,k} (1-u_{i,k,j})
```

Feature saliency itself — **confirmed 100% closed-form, verified via `CLAUDE_BRIEF_120_REPLY`**
(no root-find, no gradient step):

```
φ_j = (1/N) Σ_i Σ_k w_{i,k} u_{i,k,j}
```

Mixture weights: `π_k = (1/N) Σ_i w_{i,k}` (standard).

### 2.4 Saliency pruning — CONFIRMED REQUIRED, NOT optional; final design is a hard MDL/BIC gate (updated 2026-09-09)

**Superseded finding, recorded 2026-09-09**: this section originally deferred pruning as an open
item ("ship raw `φ_j`, let a human threshold it"). Empirical testing during implementation (Task
5's degenerate-input tests) proved that framing wrong: raw likelihood-only EM has NO mechanism
that can ever push a genuinely irrelevant feature's `φ_j` toward 0 — the salient branch (K
per-state parameter pairs) can never fit *worse* than the shared background branch (1 pair), so
likelihood alone always weakly prefers `φ_j → 1` regardless of true relevance, at every sample
size tested (N=40 to N=20,000 per cluster). Reporting raw `φ_j` for a human to threshold therefore
doesn't degrade to a merely-noisy signal — it removes the signal entirely.

**A first fix attempt (continuous MML soft-thresholding every M-step) was tried and ABANDONED,
not just refined.** It subtracted a penalty from `φ_j` every M-step
(`φ_j = max(0, sum_u - c/2) / max(eps, N - c/2)`, `c ≈ 2K-2`, per Figueiredo & Jain 2002's
MML mixture-pruning convention). Two real bugs were found and fixed in this form (denominator
shrinkage; switching the loop's convergence check to parameter-delta tracking since MML-penalized
M-steps no longer guarantee raw log-likelihood ascent) — both fixes were genuine and verified, but
a second independent Gemini CLI read-only literature consult then diagnosed the whole *approach*
as structurally unfixable: a flat, `O(1)` continuous penalty can never overcome the `O(N)`
responsibility-mass advantage a genuinely irrelevant feature's more-flexible K-Gaussian branch
accrues merely by overfitting noise. A broad empirical sweep (N=40 to N=10,000 per cluster)
confirmed this: `φ_j` for the irrelevant feature never approached 0 at ANY tested sample size, and
behavior was erratic/non-monotonic in N. True MDL/BIC structural (L0) model selection needs a
penalty that scales with `ln(N)`, evaluated as an explicit compare-before-prune decision — not
continuous per-iteration shrinkage. Full narrative:
`knowledge/global/cpp/feature_saliency_em_mml_pruning.md`.

**Final, working design: a hard MDL/BIC compare-before-prune gate, applied ONCE after the EM loop
converges** — not during it. The EM loop itself reverts to plain, unmodified textbook EM (no
penalty entangled in its M-step), so §5's ORIGINAL raw-log-likelihood-ascent convergence check is
valid again (the objective-function mismatch that motivated the parameter-delta detour no longer
applies, since nothing but the raw M-step touches `φ_j` during the loop). Once converged, each
feature `j` is evaluated via a standard BIC nested-model comparison:

```
LL_salient_j    = Σ_i log( Σ_k w_{i,k} · f(x_{i,j} | θ_{k,j}) )          (K (mean,var) pairs)
LL_background_j = Σ_i log( q(x_{i,j} | GLOBAL, unweighted mean/var) )     (1 (mean,var) pair)

if LL_salient_j - LL_background_j < (K-1)·ln(N):  φ_j := 0     (pruned to background-only)
else:                                              φ_j keeps its raw M-step value
```

This is the standard BIC decision rule (`2·ΔLL > Δparams·ln(N)`, `Δparams=2K-2`, simplifying to
the `(K-1)·ln(N)` threshold above), grounded in Wilks' theorem (Wilks 1938) and Schwarz's BIC
(1978): under the null "feature j is truly background-only," the K-Gaussian fit's log-likelihood
gain is chi-squared-distributed with `K-1` degrees of freedom (expectation `O(1)`, independent of
`N`), while the `ln(N)` threshold grows without bound — guaranteeing the gate eventually
overwhelms pure-noise overfitting at any sample size.

**A second real bug was found and fixed while implementing this**: the "background" null-hypothesis
density must be the **global, unweighted** per-feature mean/variance (computed once over all N
observations at EM init time), NOT the EM-fitted `bgMean`/`bgVar` (which are themselves weighted
by `(1-u_{i,k,j})` and degrade/narrow as `φ_j` rises during the EM phase — no longer a fair null
hypothesis once entangled with the very quantity being tested).

**Validated (2026-09-09)**: all 19/19 native tests pass, including both previously-failing Task 5
checks. A broader robustness sweep (N=40 to N=10,000 per cluster × 4 seeds, 28 runs) shows 26/28
(93%) correctly classify both the planted-salient and planted-irrelevant features. The 2 misses are
a feature-label swap (a well-known EM local-optima/initialization-sensitivity artifact, mitigated
in practice via multiple-restart/best-of-N fitting) — not a flaw in the gate's own math.

### 2.5 Explicitly NOT included in Phase 2a, and why

- **No transition matrix / no forward-backward** — this is a static mixture (i.i.d. observations),
  not a true HMM. Feature Saliency's own EM machinery only needs the static clustering question
  ("which features discriminate between states"); the time-ordering/persistence question is
  already answered by other dims (`hurst_exponent` etc.) and Phase 1's own audit.
- **No sticky-Dirichlet prior, no MAP priors on mean/covariance, no ECME DOF refinement** — all
  three are production-serving mechanisms (`student_t_hmm.py`'s own `mean_prior_kappa`/
  `covar_prior_eta`/`sticky_diag_alpha`) tuned for a live, continuously-retrained regime-detection
  model. None of them bear on "does this feature discriminate states in a static fit," and each
  adds real implementation/validation surface for zero benefit to this question.

## 3. Architecture

New standalone native tool, matching `include/BipowerVariation.h` / `tools/observation_vector/
volatility_dim_redundancy_eval.cpp`'s established pattern (pure header + thin CLI driver, own native
tests, never added to `CMakeLists.txt`/`build_dll.sh`):

- `tools/observation_vector/FeatureSaliencyEM.h` — the EM loop itself (E-step, M-step per §2.2/2.3),
  operating on an in-memory `std::vector<std::array<double, D>>` (or a caller-provided flat buffer)
  of observations — deliberately NOT reading Parquet itself, so the fitting math stays independently
  unit-testable on synthetic data (mirrors this session's own `streaming_correlation_matrix.h`
  precedent: separate the numerical core from the I/O).
- `tools/observation_vector/FeatureSaliencyEval.cpp` — CLI driver: reads `mes_candidates.parquet`
  directly (2026-09-09 update, §6 — the dim-selection pipeline's own already-materialized 10-dim
  candidate table, via `parquet::arrow::FileReader`, the same read pattern already proven in
  `tools/context_pipeline/context_to_parquet.cpp`'s reader side), buffers the resulting observation
  vectors (bounded: this is a FIT, not a streaming accumulator — unlike Phase 1, the EM loop
  genuinely needs random-access passes over the full observation set, so SOME in-memory
  materialization is unavoidable here, unlike Phase 1's O(D²)-only design; bound it explicitly via
  a `--max-observations`/subsampling flag matching this tool family's own established
  `--max-rss-mb` safety-net convention, not left unbounded).
- `tools/observation_vector/test_feature_saliency_em.cpp` — native tests (§4).

## 4. Building blocks (confirmed real, `CLAUDE_BRIEF_120_REPLY`)

Phase 2a (Gaussian) needs none of the Student-t-specific special functions — the M-step is pure
arithmetic (weighted means/variances), no root-finding. Recorded here for Phase 2b's eventual use,
so this doc remains the single reference when that phase is picked up:

| Need | C++ equivalent | Header |
|---|---|---|
| `scipy.special.digamma` | `boost::math::digamma<double>(x)` | `<boost/math/special_functions/digamma.hpp>` |
| `scipy.special.gammaln` | `std::lgamma(x)` | `<cmath>` (zero external dependency) |
| `scipy.optimize.brentq` | `boost::math::tools::toms748_solve` | `<boost/math/tools/roots.hpp>` (TOMS 748, Alefeld/Potra/Shi 1995 — confirmed better-behaved than Brent's for bracketed root-finding) |
| NumPy broadcasting | `Eigen::ArrayXXd` | `<Eigen/Dense>` |

No existing C++ library implements mixture EM with integrated feature saliency (`mlpack::gmm::GMM`
is Gaussian-only with no saliency mechanism; `dlib` has no Student-t profile likelihood or saliency
parameters) — hand-rolling the EM loop is required in either language, confirmed via
`CLAUDE_BRIEF_120_REPLY` §(c).

## 5. Initialization & convergence

- **Initialization**: k-means++ seeding (Arthur & Vassilvitskii 2007) on the raw feature values,
  matching `student_t_hmm.py`'s own `_kmeans_plusplus_seed_indices` precedent — reuse the SAME
  algorithm (not a different one) for consistency with this repo's own established practice, ported
  to C++ (straightforward: no Python-specific dependency). Saliency `φ_j` initialized to 1.0 for
  every feature (start assuming every candidate feature is salient; let the data pull genuinely
  irrelevant ones toward 0, not the reverse).
- **Convergence (reverted to original design, 2026-09-09 — see §2.4's own note)**: relative
  log-likelihood plateau (`_check_convergence`'s own formula, `student_t_hmm.py`), one-sided (only
  a non-negative relative delta counts as convergence, matching the ascent-guaranteed EM theory —
  McLachlan & Krishnan; Wu 1983). A brief detour (parameter-delta tracking) was introduced while a
  continuous MML penalty was entangled in the loop's own M-step, then reverted once that approach
  was abandoned for the final hard MDL/BIC gate (§2.4) — since the gate is now applied ONCE, after
  the loop converges, the loop itself is plain unmodified EM again and this original criterion is
  valid without qualification.

## 6. Validation plan — synthetic ground truth FIRST, real data second

Matches this repo's own standing discipline (never trust a new statistical tool against real data
before proving it recovers a KNOWN answer on synthetic data first — same discipline
`streaming_correlation_matrix.h`'s own test suite already applied to the correlation-matrix
primitive this session):

1. **Synthetic recovery test**: generate synthetic data with K=3-4 known components, D=6-8
   features, where a KNOWN SUBSET of features are genuinely state-dependent (different mean/
   variance per component) and the REST are drawn from a single shared distribution regardless of
   component (planted "irrelevant" features). Fit Feature Saliency EM; assert the recovered `φ_j`
   is high (>0.8) for the planted-salient features and low (<0.2) for the planted-irrelevant ones.
   This is the actual correctness proof — a real, checkable ground truth, not just "it runs."
2. **Degenerate-input tests**: a feature that is IDENTICAL across all components (zero true
   saliency) should converge `φ_j → 0`; a feature with an extreme, fully-separating difference
   across components should converge `φ_j → 1`.
3. **Only then, real data**: run against the 11-dim vector Phase 1 already validated
   (`tools/output/whole_vector_redundancy_eval_20260907_201445.txt`'s own underlying per-bar-close
   snapshots — either re-derive them via the same streaming tool with in-memory buffering added, or
   extract the shared compute logic per §3), for a real K matching this system's own production
   target (K=4, per `PRODUCTION_TRIAGE.md` row 1's decided production target).

**Data source update, 2026-09-09**: the dim-selection experimentation pipeline
(`docs/superpowers/specs/2026-09-09-market-data-replay-dim-selection-spec.md`) now produces
`mes_candidates.parquet` directly — a flat, already-materialized 10-candidate-dim table
(`sequence_id`/`timestamp_us`/`bars_since_last_update` + the 10 ledger `IN*` dims), generated by
`tools/market_data_replay/`'s own significant-change gate over the real 471.9M-tick dataset. This
is now the preferred real-data input for step 3 above — no snapshot re-derivation needed, and its
10-dim candidate set (not the 11-dim Phase 1 set) is the actual current dim-selection question this
fitter needs to answer. Read it directly via Arrow/Parquet (same pattern as
`tools/context_pipeline/context_to_parquet.cpp`'s reader side), one row per already-significant
observation.

## 7. Known caveats / open questions (honest, not resolved here)

- **Student-t vs. Gaussian saliency tension (RESEARCH_RESPONSE_005 §2, unresolved)**: Student-t's
  own bounded-influence robustness (the entire reason production uses it) may dampen exactly the
  tail-driven saliency signal a rare-but-extreme feature would otherwise register under Gaussian
  emissions. Phase 2a's Gaussian result should be read as "does this feature discriminate states
  under a non-robust emission family" — not yet the final institutional answer; Phase 2b (once
  derived) is how this gets checked, not a redundant afterthought.
- **Saliency pruning mechanism (§2.4)**: RESOLVED 2026-09-09 -- pruning is confirmed REQUIRED
  (not optional) and the final hard MDL/BIC compare-before-prune gate is validated: 19/19 native
  tests pass, and a broader robustness sweep (28 runs across N=40-10,000/cluster × 4 seeds) shows
  26/28 (93%) correct classification. The 2 misses are a feature-label swap -- a well-known EM
  local-optima/initialization-sensitivity artifact (mitigated in practice via multiple-restart/
  best-of-N fitting, not yet implemented here) -- not a flaw in the gate's own math. See
  `knowledge/global/cpp/feature_saliency_em_mml_pruning.md` for the full narrative including the
  abandoned continuous-MML first attempt.
- **EM local-optima sensitivity (new, 2026-09-09)**: the current single-restart k-means++ init can
  occasionally converge to a feature-label-swapped local optimum (2/28 in the robustness sweep
  above) -- flagged as a real, known limitation, not yet mitigated. Consider multiple-restart
  (best-of-N log-likelihood) fitting before Task 6/7/8's real-data runs if this proves material at
  larger K/D.
- **In-memory materialization**: unlike every other tool this session built (Phase 1's streaming
  O(D²) design, deliberately memory-bounded), a genuine EM fit needs random-access passes over the
  full observation set — this is NOT avoidable via streaming the way Phase 1's correlation matrix
  was. Must be explicitly bounded (subsampling / `--max-rss-mb`), not silently unbounded, to avoid
  repeating this session's own two real near-miss OOM incidents.
- **Phase 2b's own combined derivation** (§1) is a real prerequisite, not a formality, before
  Student-t emissions can be added — flagged, not scoped by this doc.

## 8. References

- Vaithyanathan, S. & Dom, B. (1999) — origin of feature relevance in mixture models.
- Law, M.H., Figueiredo, M.A.T. & Jain, A.K. (2004), "Simultaneous Feature Selection and Clustering
  Using Mixture Models," IEEE TPAMI 26(9):1154-1166 — the actual EM+MML formalization this spec's
  §2 is derived from.
- Fons, E., Dawson, P., Zeng, X., Keane, J. & Iosifidis, A. (2020), "A novel dynamic asset
  allocation system using Feature Saliency Hidden Markov models for smart beta investing" — the
  HMM/finance extension motivating this initiative; its own combined Student-t+saliency+HMM
  derivation is Phase 2b's real prerequisite, not yet independently obtained by this project.
- Arthur, D. & Vassilvitskii, S. (2007), "k-means++: The Advantages of Careful Seeding" —
  initialization, already this repo's own production precedent.
- McLachlan, G.J. & Krishnan, T. (2008), *The EM Algorithm and Extensions* — ascent-guaranteed
  convergence theory underlying the one-sided plateau check.
- `lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_120`/`CLAUDE_BRIEF_120_REPLY` — the C++ feasibility
  consult this spec's §0/§2/§4 are grounded in.
- Schwarz, G. (1978), "Estimating the Dimension of a Model," Annals of Statistics -- BIC's own
  `ln(N)`-scaled complexity penalty, the form the final hard-gate design (§2.4) uses.
- Wilks, S.S. (1938), "The Large-Sample Distribution of the Likelihood Ratio for Testing Composite
  Hypotheses," Annals of Mathematical Statistics -- the chi-squared expected-log-likelihood-gain
  argument underlying the hard gate's threshold.
- Gemini CLI, two independent read-only literature/code-review consults, 2026-09-09
  (`gemini --approval-mode plan`, verified via `git status` afterward to have made no edits): first
  consult diagnosed the objective-function-mismatch bug in an abandoned continuous-MML first
  attempt and supplied its denominator-shrinkage correction; second consult diagnosed that whole
  approach as structurally unfixable and recommended the final hard MDL/BIC gate (§2.4). Not a
  primary literature source itself -- treat as a secondary, unverified-against-primary-sources
  review.
- `lbrnet/logs/rc_gemini.log` `RESEARCH_RESPONSE_005` — the unresolved Student-t-vs-Gaussian
  saliency-sensitivity tension (§7).
- `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` §3/§4 — the
  originating design decision (Phase 2 scope, C++ language override) this spec elaborates.
