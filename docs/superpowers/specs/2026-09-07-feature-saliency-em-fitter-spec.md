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

### 2.4 MML pruning — open item, NOT to be guessed at implementation time

Under Minimum Message Length regularization, the saliency update becomes a soft-thresholding
operator: `φ_j = max(0, (Σ_i,k w_{i,k}u_{i,k,j} - c/2) / N)`, where `c` is the number of free
parameters saved by pruning feature j to exactly zero saliency (collapsing its per-state
`θ_{k,j}` back to the single shared background `λ_j`). **`c`'s exact value was not derived or
verified this session** — `CLAUDE_BRIEF_120_REPLY` confirmed the *form* of the closed-form update
but did not confirm the specific parameter-count constant. Before implementing MML pruning: derive
`c` directly from Law-Figueiredo-Jain (2004)'s own paper (plausibly `c ≈ 2K`, one mean + one
variance per state made redundant, but this must be confirmed against the source, not assumed).
Until then, Phase 2a can ship WITHOUT hard pruning — report raw `φ_j` per feature and let a human
(or Phase 4's own mRMR combination step) apply a judgment threshold, same posture as Phase 1's own
correlation-matrix output (report the numbers, don't auto-decide).

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

- `tools/observation_vector/feature_saliency_em.h` — the EM loop itself (E-step, M-step per §2.2/2.3),
  operating on an in-memory `std::vector<std::array<double, D>>` (or a caller-provided flat buffer)
  of observations — deliberately NOT reading Parquet itself, so the fitting math stays independently
  unit-testable on synthetic data (mirrors this session's own `streaming_correlation_matrix.h`
  precedent: separate the numerical core from the I/O).
- `tools/observation_vector/feature_saliency_eval.cpp` — CLI driver: streams the real tick data via
  the SAME `market_data_io.h`/`StreamTicksFullParquet` this session's Phase 1 tool already uses,
  reusing whichever of the 11 dims' compute logic Phase 1 already validated (extract the shared
  per-dim compute helpers rather than re-deriving them a third time — this would be the "second real
  use" moment for `whole_vector_redundancy_eval.cpp`'s own dim-compute code, per this repo's own
  extraction convention), buffers the resulting per-bar-close snapshot vectors (bounded: this is a
  FIT, not a streaming accumulator — unlike Phase 1, the EM loop genuinely needs random-access
  passes over the full observation set, so SOME in-memory materialization is unavoidable here,
  unlike Phase 1's O(D²)-only design; bound it explicitly via a `--max-observations`/subsampling
  flag matching this tool family's own established `--max-rss-mb` safety-net convention, not left
  unbounded).
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
- **Convergence**: relative log-likelihood plateau (`_check_convergence`'s own formula,
  `student_t_hmm.py`), one-sided (only a non-negative relative delta counts as convergence,
  matching the ascent-guaranteed EM theory this repo's own code comment already cites — McLachlan &
  Krishnan; Wu 1983) — reuse this exact convergence CRITERION (not the code, which is
  Python/numba-specific), since it's already a real, considered design decision documented in
  production, not something to re-derive.

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

## 7. Known caveats / open questions (honest, not resolved here)

- **Student-t vs. Gaussian saliency tension (RESEARCH_RESPONSE_005 §2, unresolved)**: Student-t's
  own bounded-influence robustness (the entire reason production uses it) may dampen exactly the
  tail-driven saliency signal a rare-but-extreme feature would otherwise register under Gaussian
  emissions. Phase 2a's Gaussian result should be read as "does this feature discriminate states
  under a non-robust emission family" — not yet the final institutional answer; Phase 2b (once
  derived) is how this gets checked, not a redundant afterthought.
- **MML's `c` constant (§2.4)**: not yet derived/verified — do not implement hard pruning against a
  guessed value; report raw `φ_j` and defer the pruning threshold to human judgment or Phase 4.
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
- `lbrnet/logs/rc_gemini.log` `RESEARCH_RESPONSE_005` — the unresolved Student-t-vs-Gaussian
  saliency-sensitivity tension (§7).
- `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` §3/§4 — the
  originating design decision (Phase 2 scope, C++ language override) this spec elaborates.
