---
domain: cpp/statistics
intent: Why raw (unregularized) Feature-Saliency EM cannot perform feature selection at all, why a continuous MML soft-threshold penalty doesn't fix it, and the working hard MDL/BIC compare-before-prune gate that does
scope: global
tags: [feature-saliency, EM, mixture-model, MML, MDL, BIC, minimum-message-length, saliency, phi, Law-Figueiredo-Jain, overfitting, model-selection]
source_files:
  - tools/observation_vector/FeatureSaliencyEM.h
  - tools/observation_vector/test_feature_saliency_em.cpp
last_verified: 2026-09-09
dependencies: []
---

# Feature-Saliency EM requires a hard MDL/BIC pruning gate, not a continuous penalty, to do feature selection at all

## Why This Exists

While implementing Law, Figueiredo & Jain (2004)'s Feature Saliency EM (a Gaussian mixture where
each feature has a scalar saliency `φ_j ∈ [0,1]` deciding whether it's state-dependent or drawn from
a shared background distribution), a native C++ port that deliberately deferred MML (Minimum
Message Length) pruning as "not yet needed, ship raw `φ_j`" was empirically tested against a
planted-irrelevant-feature fixture (D=2, one genuinely state-dependent feature, one drawn from the
same distribution regardless of state). The irrelevant feature's `φ_j` did NOT converge toward 0 —
it converged to ~0.998, indistinguishable from the genuinely salient feature, **and this got WORSE,
not better, as sample size grew** (N=40 per cluster → φ=0.77; N=20,000 per cluster → φ=0.998). This
rules out finite-sample noise/overfitting as the explanation (that would shrink with more data) and
confirms a structural property of the algorithm itself.

## The Invariant / Contract

**Without MML (or an equivalent complexity penalty), raw likelihood-maximizing EM for a
feature-saliency mixture has NO mechanism that can ever push `φ_j` toward 0 for a genuinely
irrelevant feature.** The reason is structural, not a bug: the "salient" branch for feature `j`
fits `K` independent `(mean, variance)` pairs (one per state); the "background" branch fits a single
shared `(mean, variance)` pair. The salient branch is strictly more flexible (more free parameters),
so it can never achieve a *worse* log-likelihood than the background branch — likelihood-only EM
therefore always has at least a weak preference for `φ_j → 1`, regardless of whether the feature
actually discriminates between states. Worse: once the per-state parameters for a truly
irrelevant feature converge near the shared background parameters (the CORRECT fit for such a
feature), the salient and background densities become numerically indistinguishable
(`f(x|θ_kj) ≈ q(x|λ_j)`), which makes the E-step's own responsibility split
`u_{i,k,j} = φ_j·f / (φ_j·f + (1-φ_j)·q) ≈ φ_j` regardless of value — the M-step's `φ_j` update
(`mean of w·u`) then just reproduces whatever `φ_j` currently is. **This is a fixed point, not
convergence toward truth**: `φ_j` gets frozen near its initialization once salient/background
densities coincide, never pulled toward 0 by evidence.

**A complexity penalty is required to do feature selection at all** — likelihood alone cannot. Two
approaches were tried; only the second is the actual fix (kept in `FeatureSaliencyEM.h` today):

## Attempt 1 (SUPERSEDED): continuous MML soft-thresholding on φ_j every M-step

The first attempt subtracted a soft-threshold penalty from `φ_j` every M-step:
`φ_j = max(0, sum_u_j - c/2) / max(eps, N - c/2)` with `c ≈ 2K-2` (Figueiredo & Jain 2002's MML
mixture-pruning convention, extended to feature saliency). An independent Gemini CLI read-only
consult (2026-09-09) fixed a real bug in this formula (the original numerator-only form
`(sum/N) - c/(2N)` omitted the necessary denominator shrinkage) and a real convergence bug
(MML-penalized M-steps optimize the *penalized* objective, so gating the loop's convergence check
on *raw* log-likelihood ascent — as the original `CheckConvergence` did — misreads legitimate,
penalty-induced raw-LL decreases as instability, causing oscillation/iteration-cap hits; fixed by
switching to parameter-delta convergence tracking).

**Both fixes were real and verified, but the approach itself was ultimately abandoned**: a second
independent Gemini CLI consult diagnosed the deeper issue as structural, not fixable by further
constant-tuning — continuous soft-thresholding (an `O(1)` penalty) can never overcome an `O(N)`
responsibility-mass advantage that a genuinely irrelevant feature's more-flexible `K`-Gaussian
salient branch accrues merely by overfitting noise. A broad sweep (N=40 to N=10,000 per cluster)
confirmed this empirically: `φ_1` (irrelevant) never approached 0 at ANY tested sample size
(stayed in the 0.25–0.76 range), and behavior was erratic/non-monotonic in N (one sample size
failed to converge at all within 2000 iterations; another over-pruned the genuinely salient
feature too). True MDL/BIC structural (L0) model selection needs a penalty that scales with
`ln(N)`, evaluated as an explicit compare-before-prune decision, not continuous per-iteration
shrinkage.

## Attempt 2 (CURRENT, WORKING): hard MDL/BIC compare-before-prune gate, applied once after EM converges

`ApplyHardSaliencyPruning()` replaces the continuous approach entirely. The EM loop itself is now
**plain, unmodified textbook EM** (no MML entanglement during iteration — φ_j updates via
`ComputeMStepUpdate`'s raw formula only), so its convergence criterion reverted to the original
raw-log-likelihood ascent check (`CheckConvergence`/`RelativeDelta`, matching `student_t_hmm.py`'s
own convention) — the objective-function mismatch from Attempt 1 no longer exists, because MML
no longer runs inside the loop at all.

Once the loop converges, a **one-shot hard decision per feature** is made: compare two nested
models for feature `j`'s marginal density using the converged state responsibilities `w_{i,k}`:

```
LL_salient_j   = Σ_i log( Σ_k w_{i,k} · f(x_{i,j} | θ_{k,j}) )        (K (mean,var) pairs)
LL_background_j = Σ_i log( q(x_{i,j} | GLOBAL mean/var for feature j) ) (1 (mean,var) pair)

if LL_salient_j - LL_background_j < (K-1)·ln(N):  φ_j := 0   (pruned)
else:                                              φ_j stays at its raw M-step value
```

This is the standard BIC decision rule (`2·ΔLL > Δparams·ln(N)` with `Δparams = 2K-2`, simplifying
to the `(K-1)·ln(N)` threshold above) — grounded in Wilks' theorem: under the null "feature j is
truly background-only," the log-likelihood gain from the more-flexible K-Gaussian fit is
chi-squared-distributed with `K-1` degrees of freedom, so its expectation is `O(1)` regardless of
`N`, while the `ln(N)` threshold grows without bound — guaranteeing the gate eventually overwhelms
pure-noise overfitting at any sample size, unlike Attempt 1's flat constant.

**A second real bug was found and fixed while implementing this**: the background null-hypothesis
density must be the **global, unweighted** mean/variance for feature `j` (computed once over all
`N` observations, already available as `FitFeatureSaliencyEM`'s own init-time `globalMean`/
`globalVar`) — NOT `params.bgMean`/`bgVar` (the EM-fitted background, weighted by `(1-u_{i,k,j})`
every M-step). The EM-fitted background is entangled with `φ_j` itself: as `φ_j` rises during the
raw EM phase, `(1-u)` collapses toward near-zero weight, and the "background" parameters degrade/
narrow alongside it (observed directly: `bgVar` collapsing to ~0.02 for a feature whose true
generating variance was 1.0) — no longer a fair null hypothesis. Using the phi-independent global
mean/variance instead fixed this immediately.

**Validated**: all 19/19 native tests pass (including both previously-failing Task 5 checks). A
broader robustness sweep (N=40 to N=10,000 per cluster × 4 seeds each, 28 total runs) shows 26/28
(93%) correctly classify both the planted-salient (`φ_0 > 0.8`) and planted-irrelevant (`φ_1 <
0.2`) feature. The 2 misses are a **feature-label swap** (both misses show `φ_0≈0.00, φ_1≈0.85-1.0`
— i.e. the fit found a real 2-cluster solution but converged with the roles reversed), a well-known
EM local-optima/initialization-sensitivity artifact, not a flaw in the pruning gate's own math —
mitigated in practice via multiple-restart/best-of-N fitting, out of scope for this fix.

## How It Works

Diagnostic evidence (D=2, K=2, feature 0 planted salient at ±5 separation, feature 1 drawn from
`N(0,1)` regardless of assigned cluster):

| N per cluster | φ_0 (salient) | φ_1 (irrelevant) |
|---|---|---|
| 40 | 0.923 | 0.768 |
| 200 | 1.000 | 0.999 |
| 1,000 | 1.000 | 0.999 |
| 5,000 | 0.998 | 0.998 |
| 20,000 | 0.998 | 0.998 |

φ_1 tracks φ_0 almost exactly at every sample size instead of separating toward 0 — confirms this
is not noise, it's the absence of a restoring force.

A secondary, related trap: **never initialize `φ_j` to EXACTLY 1.0** (even though "start assuming
every feature is salient" is the correct qualitative intent, per Law-Figueiredo-Jain's own
recommended init). At `φ_j=1.0` exactly, `(1-φ_j)=0`, making `u_{i,k,j} = f/(f+0) = 1` identically
for every observation regardless of the true likelihood ratio — an exact absorbing boundary the
M-step can never escape (φ recomputes to exactly 1.0 forever). Initialize to something just inside
the open interval, e.g. `1.0 - 1e-3`, so genuine likelihood evidence can still move it.

## Failure Modes

- **Reporting raw `φ_j` to a human/downstream selection step without MML** (e.g. "let a human apply
  a judgment threshold") **doesn't degrade gracefully to a merely-noisy signal — it removes the
  signal entirely.** Every feature will report `φ_j` near 1.0 regardless of true relevance, at any
  sample size. There is no threshold a human could pick that would recover meaningful ranking from
  this output.
- Assuming "more data will sharpen the separation" is a valid mitigation under raw likelihood-only
  EM -- it is not; the empirical table above shows the opposite trend across a 500x sample-size
  range. (This is specifically about the ABSENCE of a complexity penalty -- the hard BIC gate that
  fixes it explicitly DOES get stronger, not weaker, with more data, since its threshold scales
  with `ln(N)`.)
- Initializing any EM parameter at an exact 0/1 (or otherwise exact-boundary-of-support) value when
  that parameter's own update formula has a `(1-value)` or similar term in a denominator/numerator
  that can zero out -- check for this class of absorbing fixed point before trusting an
  initialization scheme copied from a paper's stated qualitative intent.
- Injecting a continuous post-hoc parameter correction (like the superseded soft-thresholding MML
  attempt) into an M-step without also changing the loop's own convergence criterion away from raw
  log-likelihood ascent -- the two objectives (raw vs. penalized log-likelihood) diverge once the
  penalty is active mid-loop. This entire class of bug is why the final design moved the penalty
  OUT of the loop (applied once, after plain EM converges) rather than fixing the convergence check
  to tolerate it.
- Using an EM-fitted "background" parameter (weighted by `(1-u)`, itself entangled with the very
  saliency value being tested) as the null-hypothesis density in a compare-before-prune decision --
  it degrades/narrows as saliency rises, silently biasing the comparison. Use a saliency-independent
  estimate (e.g. the global, unweighted per-feature mean/variance) for any such null-hypothesis
  density.
- Treating ANY single passing synthetic fixture as proof a general statistical mechanism is fully
  validated -- always sweep sample size AND seed before trusting a fix; this is what surfaced both
  Attempt 1's structural failure and Attempt 2's remaining EM-local-optima limitation.

## References

- Law, M.H.C., Figueiredo, M.A.T., & Jain, A.K. (2004), "Simultaneous Feature Selection and
  Clustering Using Mixture Models," IEEE TPAMI 26(9):1154-1166 — the primary source; the final
  hard-gate mechanism is a BIC/Wilks'-theorem derivation grounded in general MDL/MML mixture-model
  literature, not a formula transcribed directly from this paper — still not independently verified
  against its own primary text.
- Figueiredo, M.A.T. & Jain, A.K. (2002), "Unsupervised Learning of Finite Mixture Models," IEEE
  TPAMI 24(3):381-396 — the MML mixture-component-pruning machinery Attempt 1 (superseded) was
  extending; motivates why a complexity penalty is needed at all, even though its own continuous
  soft-thresholding mechanism did not transfer cleanly to per-feature saliency.
- Schwarz, G. (1978), "Estimating the Dimension of a Model," Annals of Statistics — BIC's own
  `ln(N)`-scaled complexity penalty, the form the final hard gate uses.
- Wilks, S.S. (1938), "The Large-Sample Distribution of the Likelihood Ratio for Testing Composite
  Hypotheses," Annals of Mathematical Statistics — the chi-squared expected-log-likelihood-gain
  argument for why an `O(1)` null-hypothesis LL gain is overwhelmed by an `ln(N)`-scaled threshold.
- `docs/superpowers/specs/2026-09-07-feature-saliency-em-fitter-spec.md` §2.4 — this repo's own
  spec, updated 2026-09-09 to reflect the final hard MDL/BIC gate design.
- `tools/observation_vector/FeatureSaliencyEM.h` -- the C++ port; `test_feature_saliency_em.cpp`'s
  Task 5 degenerate-input tests are what surfaced this finding (19/19 pass with the final design).
- Independent Gemini CLI literature/code-review consults, 2026-09-09 (`gemini --approval-mode plan
  -p "..."`, explicitly read-only, verified via `git status` afterward -- made no edits, twice):
  first consult diagnosed Attempt 1's objective-function-mismatch bug and denominator-shrinkage
  fix; second consult diagnosed Attempt 1 as structurally unfixable and recommended the hard
  MDL/BIC compare-before-prune gate that Attempt 2 implements. Not a primary literature source
  itself -- treat as a secondary, unverified-against-primary-sources review.
