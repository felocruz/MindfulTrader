# Elite Feature Set Curation for the Student-t HMM Observation Vector — Institutional Methodology

**Status, updated 2026-08-31 (later same day): Phase 0 done bar 3 ambiguous decisions +
`fast_mean_rev_z`'s fate (see §4 Phase 0 for the closed-out detail). Phase 1 not yet started. A new
cross-cutting finding (§5) closes off one entire line of "did live-reactivity help" investigation as
circular given the current model's training-data contamination — see §5's new bullet before
attempting anything like it again. Supersedes the pairwise, ad hoc treatment of individual dim
redundancy questions — from here forward, redundancy/relevance is a whole-vector question, not a
one-off pairwise fix.**

## 0. Origin and mandate

This initiative was triggered by a narrow investigation (`log_scale_ratio`'s bipower-variation
fix, then the discovery that its sibling `log_scale_expansion_ratio` (was `correction_action`) shares the
same jump-fragility bug and correlates with it at **0.81** once both are made fat-tail-compliant —
`lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_118`/`119` and replies, full derivation there). That
investigation surfaced two things bigger than the two dims it started with:

1. **This system's Student-t HMM uses hard-enforced diagonal covariance**
   (`lbrnet/lbrnet/models/student_t_hmm.py`: `if covariance_type != "diag": raise ValueError(...)`).
   The curse-of-dimensionality failure mode this implies is **not** covariance-matrix
   ill-conditioning (impossible under diagonal covariance — there is no cross-dimensional Σ_k term to
   become singular) — it is **double-counted evidence under a violated conditional-independence
   assumption** (the classical naive-Bayes overconfidence problem): two correlated dims modeled as
   independent push posterior state probabilities harder than their real combined information
   content justifies.
2. **The operator's explicit mandate, 2026-08-31**: think institutional, elite, whole-vector — not a
   pairwise patch. Reactivity is a first-class design goal for the HMM (not secondary to statistical
   precision), and the correct question is which **elite, minimally-redundant set** of dims best
   informs state discrimination, evaluated across all ~19 dims, not just the volatility trio that
   started this.

## 1. Literature grounding

Every citation below was independently verified against source during this thread (web search or
direct code/doc inspection), not asserted — see `CLAUDE_BRIEF_118`/`119` replies for the verification
trail on the ones marked (verified in-thread).

- **Kish (1965)**, *Survey Sampling* — origin of the Design Effect (DEFF) concept; established but
  **not the relevant diagnostic here** (DEFF/condition-number analysis targets full-covariance
  ill-conditioning, which this architecture cannot exhibit — kept as a documented dead end, not a
  live method).
- **Bouveyron & Brunet-Saumard (2014)**, *Model-based clustering of high-dimensional data: a review*
  (verified in-thread) — general curse-of-dimensionality mechanics for mixture models; the specific
  Σ_k-ill-conditioning consequence does not transfer to diagonal covariance, but the general
  redundant-features-inflate-variance-without-adding-separation argument does.
- **Peng, Long & Ding (2005)**, mRMR (Minimum Redundancy Maximum Relevance) (verified in-thread) —
  the standard supervised-learning framework for exactly this tradeoff; adapted here for an
  unsupervised HMM context by substituting state-assignment-conditioned relevance for a labeled
  target.
- **Fons, Dawson, Zeng, Keane & Iosifidis (2020)**, Feature Saliency HMM lineage — the actual
  methodology already informally in use in this codebase: the between-state-variance /
  mean-within-state-variance ratio that produced the historical 0.6373/0.1823/0.0221 discrimination
  scores (confirmed via `lbrnet/knowledge/global/training/hmm_feature_selection.md` and
  `docs/superpowers/specs/2026-08-25-vol-convexity-removal-spec.md`, both read directly this
  session) is a cheap approximation of this framework. **This is the anchor methodology for Phase 2
  below** — not a new import, a formalization of what this codebase already does ad hoc.
- **Andersen, Bollerslev, Diebold & Labys**, realized-volatility estimation theory (verified
  in-thread) — estimator precision for a realized-variance/bipower-variation-type statistic scales
  with the number of return observations in the window, independent of jump-robustness. Directly
  informed the `log_scale_ratio` vs. `log_scale_expansion_ratio` estimator-precision argument.
- **Spearman (1904)**, correction for attenuation — **empirically validated, not just cited, this
  session**: measured correlation between `log_scale_ratio` and `log_scale_expansion_ratio` rose from
  0.7638 (both raw/noisy) to 0.8085 (both bipower-variation-based) against 38.5M real MES ticks,
  confirming the predicted direction (de-noising both sides reveals a higher true correlation, not a
  lower one).
- **Ang & Timmermann (2012)**, *Regime Changes and Financial Markets* (verified in-thread) — regimes
  are strongly persistent, and risk properties follow a term structure by investment horizon; informs
  how "elite" relevance should be judged (a dim's information content is horizon-dependent, not just
  a scalar score).
- **Rydén, Teräsvirta & Åsbrink (1998)** (verified in-thread) — foundational financial-HMM precedent
  built on daily (not intraday-tick) data; supports treating regime detection as a lower-frequency
  phenomenon than raw microstructure noise, tempering (not resolving) the reactivity-vs-precision
  tradeoff.
- **Shiryaev/Wald quickest-detection theory, Dachraoui (2015), Mori (2017)** — already the cited basis
  for this system's own TRAP τ* per-tick timing (`CLAUDE.md`'s Trap Detection section). Proposed here
  (Phase 3) as the correct framework for redesigning reactive volatility-shift features, rather than
  choosing between two ad hoc fixed-window-length dims.
- **Peel & McLachlan (2000)** / **McLachlan & Peel (2000)** (verified in-thread) — describes the
  actual Student-t mixture EM/ECME machinery this system's HMM implements; the ground truth for what
  "the model" actually does with whatever feature set Phases 1-4 below select.

## 2. Governing constraint for every decision in this initiative

**Judge every redundancy/relevance call against the double-counted-evidence failure mode, not
covariance ill-conditioning.** A condition-number or eigenvalue diagnostic on a correlation matrix is
not wrong math, but it answers a question this architecture's diagonal covariance makes moot. The
directly relevant quantity is pairwise (and where practical, higher-order) correlation among
candidate-kept dims — high correlation under conditional independence means the model is being told
the same thing twice with unwarranted confidence, not that a matrix will fail to invert.

## 3. Repo/language division of labor — decided 2026-08-31, corrected same day (not as clean-cut as
first written — flagged directly by the operator, real tension, not a false alarm)

**MindfulTrader (C++) produces the best possible vector; `lbrnet` (Python) trains against it.** This
isn't a convenience split, it's this project's own already-documented architecture boundary
(`MindfulTrader/CLAUDE.md`'s Project Overview: "no ML training logic (belongs in `lbrnet`)"). Feature
Saliency EM fitting is training logic, full stop.

**Correction to this section's own first draft**: "produce the best possible vector" cannot fully
exclude relevance/informativeness judgments — a construction-correct, mutually-uncorrelated vector
full of dims that don't discriminate anything isn't "best" either. The resolution is not "C++ decides
nothing about relevance," it's that there are **two different kinds** of "which dim is more
informative" judgment, and only one of them requires fitting a model:

1. **Proxy relevance arguments, model-independent, squarely C++'s job**: literature-grounded
   informativeness calls that don't require fitting anything — e.g. the ABDL estimator-precision
   argument already used to prefer `log_scale_ratio` over `log_scale_expansion_ratio` (more
   observations feeding a windowed estimator -> lower sampling variance -> less-attenuated
   correlation with the true underlying signal), or a persistence-timescale argument (does this dim's
   native cadence even match how long regimes actually last, per Ang & Timmermann 2012). These are
   real informativeness judgments, not just redundancy bookkeeping, and C++ should make them wherever
   the literature supports a decisive call.
2. **Formal relevance determination**: Feature Saliency's actual saliency weight, jointly estimated
   with the mixture fit (Phase 2). This genuinely requires fitting something -- the line that actually
   separates C++'s scope from `lbrnet`'s.

So: C++ owns construction fixes (Phase 0), redundancy mapping (Phase 1), redesign (Phase 3), **and
every relevance call winnable on model-independent literature grounds** -- not nothing. Phase 4's mRMR
combination step already assumed a two-source combination (redundancy + relevance) -- this section's
first draft just overclaimed C++'s independence in a way the phase structure itself never did.

**Further decision, same day, operator's explicit call**: Phase 2 (Feature Saliency fitting) stays in
`MindfulTrader`/C++ too, not `lbrnet`/Python. Reasoning: `CLAUDE.md`'s "no ML training logic" boundary
was written for the *production* regime-detection model -- its live serving and retraining cadence --
not for a small, standalone, throwaway-scale diagnostic fit run purely to decide candidate-dim
inclusion/exclusion before any production model exists. That's architecturally closer to this
session's own established offline-tool pattern (`jump_ratio_eval.cpp`, `drift_location_eval.cpp`,
`volatility_dim_redundancy_eval.cpp` -- all real statistical estimation, none of them "training") than
to training the live model. `CLAUDE_BRIEF_120_REPLY` already confirmed technical viability: every
Python primitive this needs has a real C++ equivalent (`boost::math::digamma`, `std::lgamma`,
`boost::math::tools::toms748_solve` for the DOF root-find, Eigen for the linear algebra) -- Gemini's
own effort estimates favoring Python were unverified guesses (same category as its earlier `<15ns`
estimate that was later measured 200x wrong), not a reason to override this. Built as a new,
natively-tested standalone tool extending the `tools/` pattern, not touching `lbrnet` at all.

## 4. Phased methodology

### Phase 0 — Gaussian-moment-construct audit, whole vector (`MindfulTrader`, C++, CLOSED OUT bar 4 open items below)

Every dim's *raw* computation (before `FeatureScaler.h`'s downstream median/MAD normalization layer,
which is separate and already compliant) audited for reliance on Gaussian-moment statistics (mean,
variance, standard deviation, or a z-score built from them) instead of fat-tail-proven constructs
(median, MAD, order statistics, quantile ratios). Sequenced **before** Phase 1 deliberately — this
session measured directly that redundancy/correlation reads taken on still-noisy (Gaussian-moment)
dims are unreliable in both directions (see `log_scale_ratio`/`log_scale_expansion_ratio`'s
0.76-both-noisy vs. 0.81-both-clean vs. 0.62-0.64-mixed result) — fixing construction before auditing
relationships isn't just cleaner, it's the only way to get a trustworthy Phase 1 result.

**Real audit results, 2026-08-31**:
- **Confirmed Gaussian-moment, needed the same bipower-variation/robust treatment already applied to
  `log_scale_ratio`/`log_scale_expansion_ratio`**: `burstiness_index` (`EventVelocityEngine.h:124-138`,
  literal `stddev/mean` of inter-arrival times), `vol_convexity` (`StudyHelperFunctions.cpp:3231-3241`,
  literal `stddev/mean` of True Range), `mean_rev_z` (`StudyHelperFunctions.cpp:3133-3155`, an explicit
  textbook z-score — the most literal instance of the pattern in the vector).
- **Ambiguous, needs a decision not a mechanical fix — STILL OPEN**: `hurst_exponent`/`fast_hurst_exponent` (DFA's
  RMS/q=2 fluctuation function assumes finite second moment; a q=1 MFDFA variant would be more
  fat-tail-consistent, but standard DFA methodology itself isn't wrong, just a real tradeoff to weigh);
  `amihud_illiquidity` (arithmetic mean is Amihud's own canonical 2002 definition, not an accident —
  overriding a named literature construct needs its own justification, not a mechanical swap);
  `relative_range`/`liq_fragility` (own formulas are clean, but both consume an externally-computed
  ATR, itself a rolling mean, as their scale reference).
- **Separate defect, not a construct issue — STILL OPEN**: `fast_mean_rev_z` is completely unwired —
  declared, never computed, silently sitting at its schema default. Needs its own decision (wire it,
  presumably via the already-built but never-plumbed-in `ActivityClockMeanReversion.h`, or drop it).
- **Confirmed already robust**: `log_scale_ratio`, `log_scale_expansion_ratio` (bipower variation —
  jump-robust, note: not literally median/MAD, a real nuance), `skewness_idx` (Bowley), `fast_taleb_
  kurtosis` (Moors octile), `tail_index` (Hill), `lempel_ziv`, `micro_asymmetry`, `fisher_info`,
  `recurrence_rate`, `fractal_dim`.

**Fixes applied and build-verified, 2026-08-31 (later same day)**:
- **`burstiness_index`** — reformulated to a robust CV: `MAD/median × 1.4404199`
  (`EventVelocityEngine.h`'s `CalculateBurstinessIndex`). The 1.4404199 consistency constant is
  newly-derived here, NOT the standard 1.4826 — solved so `MAD/median × constant = 1.0` exactly for a
  true Poisson process (`median(X)=ln(2)/rate`, `MAD(X)` solves `sinh(d)=1/2` ⇒ `constant =
  ln(2)/arcsinh(1/2) ≈ 1.4404199`, scale-invariant, rate cancels). Empirically verified against a real
  4095-sample `Exponential(mean=1s)` draw converging to 0.9733 (`test_event_velocity_engine.cpp`,
  passing).
- **`vol_convexity`** — REMOVED from the schema entirely (19D→18D,
  `../schema/mts_schema.fbs`), not reformulated — this was already decided 2026-08-25
  (weakest discriminator, rank 13/16 at/below no-enrichment baseline; independently measuring the
  wrong thing structurally, an options-implied-vol concept on a futures-only, options-data-free
  system) but never implemented until this session executed it. Binary-incompatible with any
  historical `.context`/`.alpha` file spanning this change — accepted per this project's standing
  pre-production "default to deletion" rule. Full cleanup across every call site
  (`StudyHelperFunctions.cpp`/`.h`, `TripleScreen3.cpp`, `ContextManager.cpp`/`.h`, `FeatureScaler.h`'s
  five positional arrays and hardcoded index constants, `tests/cpp/test_feature_scaler.cpp`'s dim4
  test block and its now-dead `fixtures_dim4_raw.h` fixture file), not a partial/shimmed removal.
- **`mean_rev_z`** — reformulated to median/MAD (Kim & White 2004): the price-stretch z-score now
  uses `(current_log_p − median_log_p) / (MAD_log_p × 1.4826)` in place of mean/std, and the lag-1
  return-autocorrelation term now centers on the median return instead of the mean
  (`StudyHelperFunctions.cpp`'s `CalculateMeanReversionSpeed`). Same `nth_element`-based
  median/MAD(×1.4826) pattern already used by this codebase's other robust z-scores
  (`IndicatorComputations.h`'s `ComputeMacd`, `EventVelocityEngine.h`'s `CalculateBurstinessIndex`).
  `FeatureScaler.h`'s dim16 (`mean_rev_z`) `DIM_WINSOR_SIGMA_OVERRIDE` disabled pending re-audit against
  the new formula's real distribution, same posture as dim0/dim1/dim3.

**Real bug found and fixed as a side effect of the `vol_convexity` schema shrink**: `FeatureScaler.h`'s
`LOGZ_WINSOR_SIGMA_OVERRIDE` array literal was missing one element (17 literals for an 18-slot array),
silently misaligning every dim from `fisher_info` onward by one position — `liq_fragility`'s
calibrated `21.26f` bound was sitting at array index 11 instead of 12. Caught by
`test_feature_scaler.cpp`'s `dim12` LOGZ-rate assertion failing after the reindex, not by inspection —
this is exactly the failure class `DIM_RECURRENCE_INDEX`/`DIM_FRACTAL_INDEX`'s own comments already
warned about from the 2026-08-28 `fast_taleb_kurtosis`/`fast_hurst_exponent` insertion. Fixed by
rewriting the array with one explicit literal per dim (18 entries, each individually commented) rather
than the previous grouped/uncommented-run style that made the miscount possible in the first place.
All 18 dims' `LOGZ_WINSOR_SIGMA_OVERRIDE`/`DIM_WINSOR_SIGMA_OVERRIDE`/`DIM_WINDOW_SIZE`/`SCALE_MODE_MAP`
positions cross-checked against each other after the fix; `EXPECTED_LOGZ_DIMS` updated 3→2.

**Verification**: `test_feature_scaler.cpp`, `test_event_velocity_engine.cpp`, `test_bipower_variation.cpp`
all pass (0 failures); `./build_dll.sh --no-clean` builds clean. **Nothing from this batch is committed
yet** — pending explicit commit instruction (large batch: schema change + 5+ source files + test
fixture deletion).

**Still open (unchanged from the original audit, not touched by this fix pass)**: the 3 ambiguous
cases (`hurst_exponent`/`fast_hurst_exponent`, `amihud_illiquidity`, `relative_range`/`liq_fragility`)
still need a decision, not a mechanical fix; `fast_mean_rev_z`'s wire-it-or-drop-it decision is still
open. Phase 0 is otherwise closed.

### Phase 1 — Model-independent redundancy audit, whole vector (`MindfulTrader`, C++, available once Phase 0 lands)

Extend the pattern already built and validated for the volatility trio
(`tools/observation_vector/volatility_dim_stats.h`, `tools/observation_vector/volatility_dim_redundancy_eval.cpp`,
`include/BipowerVariation.h`) to every dim in the current ~19D vector: pure, `sc`-free ports of each
calculator operating on real historical MES tick data, pairwise Pearson correlation across all pairs
(not just within one conceptual axis — cross-axis redundancy is unverified, not assumed absent).
Organize by the existing axis groupings in
`docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md` (Scale/dispersion,
Asymmetry, Tail weight, Persistence, etc.) as a starting structure, but do not assume redundancy is
confined within an axis. **Sequencing note**: run this per-dim only after that dim clears Phase 0 —
running it earlier on a still-Gaussian-moment dim reproduces the unreliable-correlation problem Phase
0's own ordering exists to avoid.

### Phase 2 — Feature Saliency fitting (`MindfulTrader`, C++, standalone native tool, unblocked)

**Named method, not a placeholder**: Vaithyanathan & Dom (1999) → Law, Figueiredo & Jain (2004, IEEE
TPAMI 26(9):1154-1166, the actual EM+MML mixture formalization) → Fons et al. (2020)'s HMM extension —
all verified this thread (`CLAUDE_BRIEF_119`/`120`). Per-feature saliency weight estimated jointly
with the mixture via EM; near-zero-saliency features pruned via the MML criterion. **Not the same
"blocked" as originally scoped**: reusing the actual production `models/hmm_model.pkl` would be
circular (its own state boundaries were learned from the pre-fix vector), but a **fresh, standalone,
purpose-built mixture fit** — diagonal covariance (matching the real architecture), no transition
matrix/sticky prior/MAP priors/ECME nuances — has no dependency on that contaminated model and is not
circular.

**Built in C++, per §3's corrected division of labor**, as a new standalone native tool (own header +
CLI, matching the `BipowerVariation.h`/`volatility_dim_stats.h`/`volatility_dim_redundancy_eval.cpp`
pattern), not by extending `lbrnet/lbrnet/models/student_t_hmm.py`. Real building blocks confirmed by
`CLAUDE_BRIEF_120_REPLY`: `boost::math::digamma`/`std::lgamma` for the special functions the DOF
profile equation needs, `boost::math::tools::toms748_solve` for its 1D root-find (Boost's nearest
real equivalent to `scipy.optimize.brentq` -- not identical, confirmed better-behaved for bracketed
root-finding), Eigen for the linear algebra. The saliency M-step itself is closed-form per
Law-Figueiredo-Jain's own derivation (confirmed, no new numerical-optimization step beyond the
existing DOF solver) -- no off-the-shelf library in either language already implements this (`mlpack`
confirmed Gaussian-only with no saliency mechanism), so this is genuinely new code regardless of
language; building it in C++ keeps it in the same tested, native, dependency-light tradition as every
other offline validation tool this initiative has produced. Runnable once Phase 0/1 produce a clean
candidate feature set to fit against -- does not need the data-quality fix or a production retrain
first.

### Phase 3 — Reactivity-vs-precision redesign for flagged dims (`MindfulTrader`, C++)

For any dim (or redundant cluster) Phase 1/2 flags as forced into an unnecessary
reactivity-vs-precision tradeoff by construction (the `log_scale_ratio`/`log_scale_expansion_ratio`
pair is the known first instance), evaluate EWMA-weighted or sequential quickest-detection
(CUSUM/Shiryaev-Roberts) reformulation instead of defaulting to "keep the longer window." The goal is
a strictly better estimator (precision and reactivity both), not a compromise between two existing ad
hoc window lengths.

### Phase 4 — mRMR-style elite-set selection

Combine Phase 1 (redundancy) and Phase 2 (relevance, once available) into an explicit
minimum-redundancy-maximum-relevance selection pass across the full vector — not a series of
independent pairwise verdicts. Candidates dropped from the HMM are re-evaluated for fit elsewhere
(the soft/gate classifier's still-undecided feature set — `docs/superpowers/specs/
2026-08-24-two-classifier-cpp-deployment-spec.md` — is the known first candidate destination,
per its own explicit design requirement that its features be independent of the HMM's).

**Side note, 2026-08-31**: an EWMA-weighted variant of `log_scale_expansion_ratio` was contributed as
a candidate feature for the soft/gate classifier specifically (`lbrnet`'s
`2026-08-24-two-classifier-risk-sizing-architecture-spec.md` §2) — not decided, not benchmarked, not
trained against. Recorded there rather than duplicated in full here; this is the pointer.

## 5. Known blockers, explicit — corrected 2026-08-31 per `CLAUDE_BRIEF_120`

- **No longer a Phase 2 blocker**: the training-data-quality problem (faulty/missing) blocks any
  measurement against the *actual production* `models/hmm_model.pkl`, but Phase 2's fresh, standalone
  Feature Saliency fit doesn't touch that model at all — it's not blocked by this. The data-quality
  fix and eventual production retrain remain necessary for `PRODUCTION_TRIAGE.md` row 1's own sign-off
  gate, just not for this initiative's Phase 2 specifically. Don't conflate the two.
- **Real Phase 2 prerequisite**: Phase 0 (Gaussian-moment fixes) and Phase 1 (redundancy audit) need
  to produce the candidate feature set Phase 2 fits against — sequencing dependency, not a data-quality
  one.
- **NEW, 2026-08-31 (later same day) — any "did X change help the HMM" question against the
  *existing* `models/hmm_model.pkl` is circular, full stop, not just a data-quality caveat.** Surfaced
  when scoping a re-measurement of `amihud_illiquidity`/`liq_fragility`'s cross-state discrimination
  ratio now that both are live-reactive (2026-08-29, see the 2026-08-29 brainstorm doc §1.11): that
  model's state boundaries were themselves learned from the pre-Phase-0 vector (`burstiness_index`'s
  plain-CV bug, `log_scale_ratio`/`log_scale_expansion_ratio`'s raw-variance-ratio bug, `mean_rev_z`'s
  mean/std bug, `vol_convexity` still present) — using its labels as a yardstick to judge a fix to
  those exact defects scores the fix against contaminated ground truth. This is the mirror image of
  Phase 2's own resolved circularity question (§4 Phase 2: a *fresh* standalone fit is NOT circular)
  — reusing the *existing* stale model's state labels for anything **is** circular. Two honest paths
  forward for any such question: (a) defer until a model is retrained on the corrected/elite vector,
  or (b) answer a model-independent version instead (e.g. does the live-reactive signal lead a real
  market-stress/jump proxy, `jump_ratio_eval.cpp`/`drift_location_eval.cpp`'s own lead-time
  methodology, no HMM required).

  **DECIDED, 2026-08-31 (same day): defer, option (a).** Considered building the model-independent
  lead-time tool (option b) — it's cheap, methodologically sound, and fits squarely in C++'s own
  "proxy relevance" category per §3 — but rejected on priority grounds, not technical merit: it would
  answer a *different* question (lead-time-to-a-stress-proxy) than the one originally posed
  ("more important to the HMM"), and doesn't move row 1's actual blocking gate
  (`PRODUCTION_TRIAGE.md` row 1's own 2026-08-31 entry: "Real next action, not more feature curation:
  scope and fix the training-data quality problem"). Building it now would be more feature-vector
  investigation on the exact row already flagged for that pattern. **No tool built. Re-open this only
  after a retrain exists on the corrected/elite vector**, at which point the original discrimination-
  ratio methodology (§1's Fons et al. 2020 anchor) can be re-run directly and non-circularly.

## 6. Immediate next action

Phase 0 (Gaussian-moment audit) is closed out bar 3 ambiguous decisions (`hurst_exponent`/
`fast_hurst_exponent`, `amihud_illiquidity`, `relative_range`/`liq_fragility`) and `fast_mean_rev_z`'s
wire-or-drop decision — none of the four require further investigation, only a decision. `burstiness_
index`, `vol_convexity` (removed), `mean_rev_z` are all fixed and build-verified. Phase 1 (whole-vector
correlation audit) is unblocked and ready to scope now. Separately, §5's new circularity finding means
the Amihud/liq_fragility live-reactivity question needs its own explicit path choice (defer to retrain,
or build a model-independent lead-time tool) before any further work on that specific question.

**Updated 2026-09-02**: the real-tick-data blocker on row #2 (`burstiness_index`) is closed —
`tools/scid_processing/scid_to_ticks_parquet.cpp` shipped and `lbrnet/data/raw/mes_ticks.parquet`
(471.9M real rows) now exists. Re-running `burstiness_recalibration.cpp` against it found the
production `STATE_WINSOR_SIGMA=6.0` default clipping ~74% of real readings (rate-at-bound(6.0)=
73.7749%) — a genuinely broken bound, not a minor miscalibration. Deriving the correct bound (this
codebase's own GPD/return-level methodology, per `amihud_illiquidity`/`liq_fragility`'s precedent) is
the immediate next action.

## 7. Per-dim decision ledger (carried over from the 2026-08-29 brainstorm doc, canonical here going forward)

**Origin note**: this ledger was originally built and maintained as §9 of
`docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md`. Moved here
2026-08-31 because this initiative now owns whole-vector redundancy/relevance decisions (§0); the
brainstorm doc's own copy is left in place for history but points here for the current state — don't
update both independently, this is the copy of record. Field count is now **18** (`vol_convexity`
removed, §4 Phase 0), not 19.

Status vocabulary (unchanged from the brainstorm doc): **IN** (settled, stays as-is) · **IN-WEAK**
(stays, weak, no better alternative identified) · **IN-CONTINGENT** (stays now, flagged future
redundancy risk) · **IN-PENDING-FIX** (stays, a decided implementation change not yet done) ·
**IN-UNMEASURED** (shipped, never tested against anything) · **OUT-HMM** (dropped from HMM
model-input selection only; C++ computation and non-HMM consumers unaffected) · **PAUSED**
(groundwork exists, explicitly do not proceed without new evidence) · **CANDIDATE** (proposed, not
yet in the vector) · **CANDIDATE-VALIDATED** (offline model-independent test survives, not yet an
actual schema field) · **CANDIDATE-DEFERRED** (proposed, explicitly pushed to a later window).

| # | Dim | Status | Clock | Why (one line, updated 2026-08-31) | Last verified |
|---|---|---|---|---|---|
| 1 | `log_scale_ratio` | IN-WEAK | Live (TS1) | Reformulated to bipower variation 2026-08-31 (Barndorff-Nielsen & Shephard) — see brainstorm doc §9 row 1 for full derivation; unchanged since | 2026-08-31 |
| 2 | `burstiness_index` | IN-PENDING-FIX | Event-driven (`raschkeBurst`) | Two fixes stacked: (a) redirect to `raschkeBurst` landed 2026-08-29; (b) **Phase 0 fix, 2026-08-31**: reformulated to robust CV `MAD/median × 1.4404199` (newly-derived Poisson-neutrality constant, not the standard 1.4826), build-verified (`test_event_velocity_engine.cpp` passing). **Real-tick-data gap CLOSED 2026-09-02**: `tools/scid_processing/scid_to_ticks_parquet.cpp` shipped, decoding genuine per-tick `.scid` data (no aggregation) into `lbrnet/data/raw/mes_ticks.parquet` (471.9M real rows) -- the "not yet sourced" blocker below no longer applies. `tools/observation_vector/burstiness_recalibration.cpp` rewritten to a bounded single-pass streaming architecture (`StreamTicksParquet`, ~250MB RSS regardless of row count -- the prior full-materialization design would have peaked near this machine's physical RAM at this row count) and re-run against the real file: **rate-at-current-bound(6.0)=73.7749%** (n=471,930,891, mean\|z\|=27828.54, max\|z\|=2,037,487.00, p50=1460.62, p90=111531.42, p99=339241.09, p99.9=339241.41, corr(localMAD,\|z\|)=-0.0780). This is a dramatically different, far more severe finding than the earlier bar-file-based estimate (rate-at-bound(6.0)=0.0663%) -- confirms that estimate was never trustworthy at production scale, exactly as this row's prior entry already flagged. **The production `STATE_WINSOR_SIGMA=6.0` default is clipping ~74% of real readings, not a rare tail** -- a genuinely broken bound, not a minor miscalibration. Correct-bound derivation (matching this codebase's own GPD/return-level methodology already used for `amihud_illiquidity`/`liq_fragility`) is now the immediate next action, in progress | 2026-09-02 |
| 3 | `relative_range` | IN | Live (TS2) | #1 discriminator (0.6373), volatility-level axis, unchanged | 2026-08-29 |
| 4 | `log_scale_expansion_ratio` | IN-WEAK | TS2, live-vs-bar-gated unchecked | Reformulated to bipower variation 2026-08-31, correlates with `log_scale_ratio` at 0.8085 post-fix — redundancy call owned by this doc's Phase 1, not yet run | 2026-08-31 |
| 5 | `vol_convexity` | **REMOVED FROM SCHEMA** | N/A | Executed 2026-08-31 (19D→18D) — full cleanup across all call sites, `FeatureScaler.h`'s five positional arrays, and `test_feature_scaler.cpp`; not reformulated, independently weak on two separate measures, correctly not worth further investment (§4 Phase 0) | 2026-08-31 |
| 6 | `lempel_ziv` | IN, not for fat-tail use | Event-native | Complexity axis rep, unchanged, already confirmed robust construct (Phase 0 audit) | 2026-08-23 |
| 7 | `hurst_exponent` | IN-WEAK | Live (TS1), diluted weight | **Phase 0 flagged AMBIGUOUS 2026-08-31, still open**: DFA's RMS/q=2 fluctuation function assumes finite second moment; a q=1 MFDFA variant would be more fat-tail-consistent, but standard DFA isn't wrong — a real tradeoff to weigh, not a mechanical fix. Needs a decision | 2026-08-31 |
| 8 | `micro_asymmetry` | OUT-HMM | TS3, unchecked | Weakest overall (0.0001), already dropped from HMM selection, unchanged | 2026-08-25 |
| 9 | `fisher_info` | IN-WEAK | Live (TS1) | Second-worst (0.0011), already confirmed robust construct (Phase 0 audit), unchanged | 2026-08-29 |
| 10 | `fast_hurst_exponent` | IN-UNMEASURED | Activity-clock | Shipped but cross-state ratio never measured, alone or crossed with `relative_range`; unchanged | 2026-08-29 |
| 11 | `tail_index` | OUT-HMM | Event-native | Structurally redundant with the model's own native ν_k; already confirmed robust construct (Phase 0 audit), unchanged | 2026-08-25 |
| 12 | `skewness_idx` | IN-CONTINGENT | Activity-clock | Asymmetry axis rep, contingently redundant against a future skewed-Student-t emission; already confirmed robust construct (Bowley, Phase 0 audit), unchanged | 2026-08-27 |
| 13 | `amihud_illiquidity` | IN | Live-reactive, guarded `kLiveBarMinVolume=50.0` | **Phase 0 flagged AMBIGUOUS 2026-08-31, still open**: arithmetic mean is Amihud's own canonical 2002 definition — overriding it needs its own literature justification, not a mechanical swap. Separately (§5): re-measuring its cross-state discrimination ratio against the *current* `models/hmm_model.pkl` is **confirmed circular** (that model's states were learned from the pre-Phase-0 vector) — **decided 2026-08-31: defer to a post-retrain re-measurement**, no model-independent lead-time tool built (priority decision, not technical) | 2026-08-31 |
| 14 | `liq_fragility` | IN | Live-reactive, same guard as row 13 | **Phase 0 flagged AMBIGUOUS 2026-08-31, still open**: own formula is clean but consumes an externally-computed ATR (itself a rolling mean) as its scale reference. Same circularity/deferral decision as row 13 | 2026-08-31 |
| 15 | `fast_taleb_kurtosis` | IN-UNMEASURED, **top-priority action** | Activity-clock | First-ever kurtosis dim; must be selected into `lbrnet`'s `HMM_KEEP_DIMS` and retrained — unchanged, still the single highest-priority action across both this doc and the brainstorm doc | 2026-08-29 |
| 16 | `recurrence_rate` | IN, orthogonal to this doc's goal | Activity-clock | Already confirmed robust construct (Phase 0 audit), unchanged | 2026-08-28 |
| 17 | `fractal_dim` | IN | Time-bar (TS2), by design | Already confirmed robust construct (Phase 0 audit), unchanged | 2026-08-28 |
| 18 | `mean_rev_z` | IN-WEAK, **reformulated** | Live (TS3) | **Phase 0 CONFIRMED Gaussian-moment** (literal textbook z-score, the most literal instance of the pattern in the vector) **and REFORMULATED 2026-08-31** to median/MAD (Kim & White 2004): both the price-stretch z-score and the lag-1 autocorrelation term now center on the median, build-verified. `FeatureScaler.h` dim16 winsor override disabled pending re-audit against the new formula's real distribution. Its prior "empirically null" verdict (`mean_rev_z_variant_comparison.py`) was measured against the **old** mean/std formula — not yet retested against this new one | 2026-08-31 |
| 19 | `fast_mean_rev_z` | PAUSED, **confirmed unwired** | Activity-clock | **Phase 0 confirmed 2026-08-31** (stronger than "wiring paused"): completely unwired — declared, never computed, silently sitting at its schema default. Needs its own wire-or-drop decision (presumably via the already-built but never-plumbed-in `ActivityClockMeanReversion.h`), still open | 2026-08-31 |
| 20 | Drift/location (return z-score) | **OUT, tested and rejected** | N/A | Offline prototype (`tools/observation_vector/drift_location_eval.cpp`) found hit_rate below 0.5 at every horizon; rejected before any schema/C++ commitment. Unchanged | 2026-08-30 |
| 21 | Jump/bipower-variation ratio | **CANDIDATE-VALIDATED** | N/A | Offline prototype (`tools/observation_vector/jump_ratio_eval.cpp`) survives — real, substantial, opposite-of-naive-hypothesis effect. Not yet promoted to a schema field; promotion decision now sits behind this doc's own Phase 1 (redundancy audit), not yet run | 2026-08-30 |
| 22 | Hurst × volatility-level cross-term | CANDIDATE | N/A | Feature-engineering only, no new data; unchanged | 2026-08-29 |
| 23 | Self-exciting jump clustering (Hawkes intensity) | CANDIDATE-DEFERRED | N/A | Deferred to post-workstation window; unchanged | 2026-08-29 |
| 24 | Realized semi-variance decomposition | CANDIDATE-DEFERRED | N/A | Logged, no priority assigned yet; unchanged | 2026-08-29 |
| 25 | Recovery time-since-last-extreme-event construct | CANDIDATE-DEFERRED | N/A | Not yet designed, not just unprototyped; unchanged | 2026-08-29 |
