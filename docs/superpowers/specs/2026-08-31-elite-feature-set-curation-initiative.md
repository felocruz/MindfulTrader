# Elite Feature Set Curation for the Student-t HMM Observation Vector — Institutional Methodology

**Status, updated 2026-09-14: Phase 0 fully CLOSED (`relative_range`, row 3, resolved via real-data
validation — the candidate median-range reformulation is MORE fat-tailed than the current ATR(14),
kept as-is). Phase 1 (whole-vector redundancy audit) DONE for the calendar-clock vector — no
redundancy found; `tail_index`'s clock/window also verified against its live call site 2026-09-14.
Phase 2 (Feature Saliency EM) is BUILT and has a first real-data result (2026-09-11, old machine):
5 of 10 candidate dims got zero saliency (`burstiness_index`/`hurst_exponent`/`amihud_illiquidity`/
`liq_fragility`/`fractal_dim`) — see §4 Phase 2's own results subsection; a fresh re-run against
Puget's larger `mes_ticks.parquet` is in progress. A side thread (§4, after Phase 2) opened
2026-09-07: every dim's real math, plus the Mahalanobis significant-change gate, is now confirmed
pure C++ and reusable outside Sierra Chart. A cross-cutting finding (§5) closes off one entire line
of "did live-reactivity help" investigation as circular given the current model's training-data
contamination — see §5's bullet before attempting anything like it again. Supersedes the pairwise,
ad hoc treatment of individual dim redundancy questions — from here forward, redundancy/relevance is
a whole-vector question, not a
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
- **Ambiguous cases**: `hurst_exponent`/`fast_hurst_exponent` (kept q=2 DFA, resolved 2026-09-02,
  row 7); `amihud_illiquidity` (reformulated to sqrt-law + geometric mean, resolved 2026-09-03, row
  13); `liq_fragility` (reformulated off ATR to a dedicated median-range reference, 2026-09-03, row
  14); `relative_range` (**RESOLVED 2026-09-14** -- a real-data validation found the same style of
  median-range reformulation actually makes this dim's distribution MORE fat-tailed, not less;
  decision: keep ATR(14)-SMA, no reformulation; see row 3 for the full account).
- **Separate defect, RESOLVED**: `fast_mean_rev_z` was completely unwired. Decision (2026-09-04, row
  19): DROP, not wire -- real-data forward-return/hit-rate test found no predictive power over
  `mean_rev_z` (statistically indistinguishable from a coin flip).
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

### Phase 1 — Model-independent redundancy audit, whole vector (`MindfulTrader`, C++, PARTIAL RESULT 2026-09-07)

Extend the pattern already built and validated for the volatility trio
(`tools/observation_vector/volatility_dim_stats.h`, `tools/observation_vector/volatility_dim_redundancy_eval.cpp`,
`include/BipowerVariation.h`) to every dim in the current ~19D vector: pure, `sc`-free ports of each
calculator operating on real historical MES tick data, pairwise Pearson correlation across all pairs
(not just within one conceptual axis — cross-axis redundancy is unverified, not assumed absent).
Organize by the axis groupings in
`docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`'s "Market regime state
taxonomy and orthogonal-axis decomposition" section (Scale/dispersion, Asymmetry, Tail weight,
Persistence, etc.) as a starting structure, but do not assume redundancy is
confined within an axis. **Sequencing note**: run this per-dim only after that dim clears Phase 0 —
running it earlier on a still-Gaussian-moment dim reproduces the unreliable-correlation problem Phase
0's own ordering exists to avoid.

**Rock-solid DOD implementation, 2026-09-07** (`tools/observation_vector/streaming_correlation_matrix.h`
+ `tools/observation_vector/whole_vector_redundancy_eval.cpp`), built specifically to avoid a repeat
of the 2026-09-03 OOM incident (4 concurrent `observation_vector_recalibration.cpp` passes exhausted
RAM): a single streaming pass (`StreamTicksFullParquet`) feeding a generic O(D²)-memory online
multivariate Pearson correlation accumulator (Welford 1962 / West, D.H.D. 1979, "Updating Mean and
Variance Estimates: An Improved Method," CACM 22(9):532-535) — no per-tick observation is ever
retained, in any dim, at any point (17/17 native tests pass,
`tools/observation_vector/test_streaming_correlation_matrix.cpp`, including a stress test proving
this beats the naive sum-of-squares formula `CorrTracker` itself uses under catastrophic
cancellation). Snapshot cadence: once per TS3 (15-min) bar close, with carry-forward for
slower-refreshing dims (matches FeatureScaler's own conceptual model).

**Real incident during this work, self-inflicted, worth recording as its own lesson (see
`/memories/repo/cpp_tools_conventions.md`)**: the first ~32-minute run's `Print()` helper used bare
`std::printf` instead of routing through `ToolProgressLogger`, and its entire correlation-matrix
result was lost when the terminal session closed before being read. Fixed (`Print()` now takes a
`ToolProgressLogger&` and logs every line) and re-run.

**Scope correction, same day (operator directive): removed `fast_hurst_exponent`, `skewness_idx`,
`fast_taleb_kurtosis`, `recurrence_rate` from this audit entirely** — all four read the SAME
`ImbalanceBarEngine` activity-clock returns buffer (confirmed via `TripleScreen2.cpp`'s own comment
for `recurrence_rate`: moved off TS2 to that buffer 2026-08-28), so they are NOT genuinely
calendar-clock-native dims — mixing them into this CALENDAR-clock redundancy audit conflates two
observation vectors the two-HMM design (`docs/superpowers/specs/2026-09-06-imbalance-triple-screen-
architecture-spec.md` §1.4) already decided must stay independent. Removing them also deleted this
tool's own `ImbalanceBarEngine`/`RecurrenceRateEngine` dependency entirely — simpler and faster, not
just more correctly scoped. These 4 dims' own redundancy question belongs in a SEPARATE audit against
`ImbalanceObservationData`'s own (currently 4-field) vector, not here.

**Result, n=76,411 TS3-bar-close snapshots, PARTIAL, CORRECTED SCOPE (6 of ~12 genuinely
calendar-clock-native real dims)** — included: `log_scale_ratio`, `burstiness_index`,
`log_scale_expansion_ratio`, `amihud_illiquidity`, `liq_fragility`, `mean_rev_z` (full
inclusion/exclusion rationale in the tool's own header comment). **Max |r| = 0.0901**
(`log_scale_ratio` × `amihud_illiquidity`) — every one of the 15 pairs is |r| < 0.10, essentially
ZERO pairwise redundancy among these 6 dims, well below the ~0.8 double-counted-evidence threshold
this same session found for `log_scale_ratio`/`log_scale_expansion_ratio` earlier (§0). Extracted
directly from the original 10-dim run's already-computed matrix (`tools/output/
whole_vector_redundancy_eval_20260907_191934.txt`) by dropping the 4 removed dims' rows/columns —
Pearson correlation between two fixed series is unaffected by which OTHER dims share the same
matrix, so no re-run was needed to get this corrected-scope number (a re-run was started anyway,
then recognized as unnecessary and killed before completion — no fresh archive exists or is needed).
The tool itself was still fixed in code (§ above) so any FUTURE run is correctly scoped from the
start, without requiring this same manual extraction step again.

**Not yet done, real follow-on**: `relative_range` (needs a ported ATR), `lempel_ziv`/`tail_index`
(clock/window choice not yet re-verified against a live call site), `hurst_exponent`/`fisher_info`/
`fractal_dim` (each needs its own additional window plumbing) — adding these completes Phase 1's
whole-vector scope; `micro_asymmetry` (per-tick cadence) and `fast_mean_rev_z` (decided dead) are
structurally excluded, not deferred.

**5 remaining dims fixed and wired, 2026-09-07 (operator directive: "fix them, then wire them")**
— every formula verified against its real, live production call site before porting, per this
session's own standing discipline:
- `relative_range` (TS2): `cfc::ComputeRelativeRange(high,low,atr,lastValid)`, ATR = **SMA(True
  Range, 14)** — confirmed via `TripleScreen2.cpp:245` (`sc.ATR(..., 14, MOVAVGTYPE_SIMPLE)` — a
  real, easy-to-miss detail: SIMPLE moving average, NOT Wilder's, unlike TS1's own ATR(14) used for
  `RiskManager` sizing).
- `lempel_ziv`: `InformationEngine::GetLempelZivComplexity()` (LZ76, `WINDOW_SIZE_LZ=64`
  median-binarized returns), fed tick-level on genuine price CHANGES only (matches
  `ContextManager.cpp`'s own `UpdateMarketPhysics()` gate).
- `hurst_exponent` (TS1): `DfaHurstExponent(returns,100,8)`. **Documented simplification, not a
  silent one**: production's real window is ADAPTIVE (`macro_window_n`, market-speed/coherence-
  driven with a 5-bar hysteresis confirm via `CalculateAdaptiveObservationWindow`/
  `AdaptiveWindowParams::UpdateWindows()`) — simplified here to the same FIXED 100-bar window
  `fast_hurst_exponent` already uses. Porting the full hysteresis state machine was judged not
  worth the added complexity/bug-surface for a redundancy audit that only needs the correlation
  STRUCTURE, not exact value parity.
- `fisher_info` (TS1): `cfc::ComputeFisherInformation(min,max,current,lastValid)` over a fixed
  100-bar close window — same adaptive-window simplification as `hurst_exponent`, same reason
  (production's real window, `fisher_window_n`, is also adaptive).
- `fractal_dim` (TS2): `SevcikFractalDimension.h`, 400-bar window (confirmed via
  `TripleScreen2.cpp`'s own `kFractalDimHmmWindow=400` comment, Politis-White-derived) over TS2
  closes — the last 401 CLOSED-bar closes approximate production's own asymmetric live-vs-closed
  401-point window (the just-closed bar stands in for the "live" slot), a boundary-only
  simplification.

All 5 computed at bar-close cadence (matching this tool's own existing convention for
`log_scale_ratio`/`log_scale_expansion_ratio`), not production's genuine per-tick intra-bar
reactivity for `hurst_exponent`/`fisher_info` specifically. Compiles clean, smoke-tested (20s run,
no crash, RSS bounded) before committing to the full pass.

**FINAL RESULT, 2026-09-07, n=76,411 TS3-bar-close snapshots, FULL SCOPE (11 of ~12 genuinely
calendar-clock-native real dims)** — completes Phase 1's whole-vector scope. Max RSS 2558MB (well
within the 4096MB budget), no `CheckMemoryBudget()` trip. **Max |r| = 0.3414** (`mean_rev_z` ×
`relative_range`) — the strongest pair in the entire vector, still nowhere near the ~0.8
double-counted-evidence threshold this session found for `log_scale_ratio`/`log_scale_expansion_ratio`
(§0). Next-strongest pairs: `fisher_info`×`fractal_dim` = -0.2838, `log_scale_expansion_ratio`×
`relative_range` = +0.2671, `amihud_illiquidity`×`fisher_info` = -0.2519,
`liq_fragility`×`relative_range` = -0.2393 — all real, interpretable (e.g. `relative_range`
correlating with the two volatility-expansion/liquidity dims makes structural sense — range
normalization and volatility/liquidity share a common driver) but none rising to redundancy by this
initiative's own governing standard (§2). Every one of the 55 pairs is |r| < 0.35. **Conclusion: no
redundancy problem found across the full genuinely-calendar-clock-native observation vector.** Full
matrix + sorted pairs: `tools/output/whole_vector_redundancy_eval_20260907_201445.txt`,
`tools/RECALIBRATION_LEDGER.md`.

**Phase 1 status: DONE for the calendar-clock vector.** Real, deliberate remaining gaps: `tail_index`
(clock/window verified against its live call site 2026-09-14 — event-driven, windowSize=500,
tailPercent=0.05, EWMA α=0.2 — but its OUT-HMM status rests on conceptual redundancy with the
model's own ν_k, not a Phase-1-style correlation measurement, so it was not folded into the
redundancy-matrix tool); `micro_asymmetry`
(structurally excluded, per-tick cadence incompatible with this tool's bar-close snapshot model);
the 4 activity-clock dims (`fast_hurst_exponent`/`skewness_idx`/`fast_taleb_kurtosis`/
`recurrence_rate`) need their own separate audit against `ImbalanceObservationData`, deliberately
postponed per operator directive to focus on `ObservationData`. Next per the initiative's own phased
order: Phase 2 (Feature Saliency fitting).

### Phase 2 — Feature Saliency fitting (`MindfulTrader`, C++, standalone native tool, unblocked)

**Dedicated implementation spec written 2026-09-07**:
`docs/superpowers/specs/2026-09-07-feature-saliency-em-fitter-spec.md` — full E-step/M-step math
derivation, architecture, validation plan, and honestly-flagged open questions (MML pruning
constant not yet derived; Student-t extension deferred to its own Phase 2b pending a targeted
derivation of how its latent scale-mixture weight combines with the saliency responsibility). This
section (below) remains the design-decision summary; read the dedicated spec before implementing.

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

**Phase 2 real-data results (Task 8 of `docs/superpowers/plans/2026-09-09-feature-saliency-em-
fitter-implementation.md`, run 2026-09-11 on the old machine, recorded here 2026-09-14 — this
transcription was the plan's own last open item, the run/code itself was already done)**:

`tools/bin/feature_saliency_eval` against the old machine's `mes_candidates.parquet` (274,893,510
rows, 10 candidate dims, K=4, seed=13, 500K reservoir sample, 337 EM iterations to convergence):

| Candidate dim | Ledger row | φ_j (saliency) | Verdict |
|---|---|---|---|
| `mean_rev_z` | 18 | 0.9600 | Salient |
| `fisher_info` | 9 | 0.9476 | Salient |
| `relative_range` | 3 | 0.8901 | Salient |
| `lempel_ziv` | 6 | 0.8135 | Salient |
| `log_scale_ratio` | 1 | 0.6983 | Salient |
| `burstiness_index` | 2 | 0.0000 | **Zero saliency** |
| `hurst_exponent` | 7 | 0.0000 | **Zero saliency** |
| `amihud_illiquidity` | 13 | 0.0000 | **Zero saliency** |
| `liq_fragility` | 14 | 0.0000 | **Zero saliency** |
| `fractal_dim` | 17 | 0.0000 | **Zero saliency** |

Exactly 5 of 10 candidate dims got `φ_j=0` (not just low) — their state-conditional means barely
separate across the K=4 clusters fit here. **This is real evidence for Phase 4's mRMR selection
step, not itself a pruning decision** (per spec §2.4's "report the numbers, don't auto-decide"
posture) — whether to actually drop these 5 from the HMM's final vector is still open.

**Gaussian-vs-Student-t caveat (spec §7), flagged explicitly per Task 8's own requirement**: this
fit is Phase 2a — a Gaussian mixture + Feature Saliency EM, not the production Student-t HMM. A
dim that looks non-salient under a Gaussian cluster-mean-separation test could still carry real
tail-behavior-specific information a Student-t model's own per-state ν would pick up on
differently. **Do not treat these φ_j=0 verdicts as a final answer** until Phase 2b (the Student-t
extension, explicitly deferred, spec §1) exists — this result is Phase 2a's honest output, not the
institutional final word.

**Not yet done**: this result is against the *old* machine's `mes_candidates.parquet` (271.9M
ticks' worth, generated before the Puget migration). A fresh regeneration from Puget's own
476.7M-tick `mes_ticks.parquet` was launched 2026-09-14 (`docs/PUGET_SETUP_COORDINATION.md` Entry
17) — once it completes, re-run `feature_saliency_eval` against it and record the fresh φ_j
alongside this table to confirm the same 5-zero/5-salient pattern holds on the larger, fresher
dataset.

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

### Side thread — offline `.context` generator feasibility, opened 2026-09-07

Not a new phase of this initiative, but directly enabled by it: Phase 1's tool proved the whole
calendar-clock vector's real math is already reachable without Sierra Chart. Follow-up question —
could a standalone, non-Sierra-Chart tool reconstruct TS1/TS2/TS3 from raw tick data, compute the
real 18D vector via the exact production formulas, and replicate the real Mahalanobis
significant-change gate (not a simplified bar-close-cadence substitute) to write a genuine
`.context` file? Traced `ContextManager::BuildObservationVector()`/`CheckAndTriggerHMM()` in full:
`ComputeTriggerDecisionMetrics` (the Mahalanobis gate itself) and `FeatureScaler::UpdateAndNormalize`
were confirmed **already pure C++**, operating only on `std::array<float,18>` + internal rolling
history — no `sc.*` dependency at all. Of the 18 dims, only `mean_rev_z` and `liq_fragility` still had
their real math inlined directly against `sc.*` arrays (every other dim already delegated to a pure
header: `BipowerVariation.h`, `CarryForwardCalculators.h`, `SevcikFractalDimension.h`,
`DfaHurstExponent.h`, `OrderFlowAsymmetryEngine.h`, `EventVelocityEngine.h`, `RobustMoments.h`,
`RecurrenceRateEngine.h`). Extracted both same-day: `include/MeanReversionCalculator.h`
(`mrc::ComputeMeanReversionZ`) and `include/LiquidityFragilityEngine.h`
(`lfe::ComputeLiquidityFragility`), each verified bit-faithful against an independent Python port of
the original formula (native test suites `tests/cpp/test_mean_reversion_calculator.cpp`/
`test_liquidity_fragility_engine.cpp`, 9/9 checks pass) before being wired back into
`StudyHelperFunctions.cpp`'s production wrappers, which now do nothing but the ACSIL array-gather and
persistent-state carry-forward. `./build_dll.sh --no-clean` clean, `test_feature_scaler.cpp`
regression-clean. **Result: every one of the 18 dims' real math, plus the Mahalanobis gate itself, is
now provably pure and independently reusable — the generator itself is not yet built, no dedicated
spec written yet.**

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

- **Same trap, caught before acting on it this time, 2026-09-02**: scoping `fast_hurst_exponent`
  (row 10, IN-UNMEASURED)'s cross-state discrimination ratio hit the identical circularity — both
  the *existing* `models/hmm_model.pkl` (trained before this session's Phase 0 fixes existed) and the
  *existing* exported `.context` training-cache files (not yet regenerated with the fixed/removed/new
  dims either) are equally untrustworthy yardsticks right now. Neither is a new circularity mode —
  it's the same one above, just almost applied to a different dim without re-checking first. No
  measurement attempted; deferred alongside `amihud_illiquidity`/`liq_fragility` until both (a) a
  retrain on the corrected/elite vector exists, AND (b) `.context` exports are regenerated from it.

## 5a. Model contamination manifest — READ BEFORE citing any HMM-state-dependent conclusion

**Standing rule (operator directive, 2026-09-03): any finding — in this doc, any other doc, or a
casual claim — that relies on `models/hmm_model.pkl`'s state assignments, or on any retrain that
predates the fixes below, is provisional evidence about a contaminated model, not a settled
conclusion about the current system. This applies retroactively — a documented instance of this trap
being missed was found and corrected in `docs/ADR/burstiness_index_misnomer.md` (its "ECME-fixed K=4
retrain" empirical claim relied on a `burstiness_index` formula since found broken and reformulated
twice over). Check every doc citing HMM cross-state ratios, spread analyses, or per-state statistics
against the list below before trusting it; if it predates a listed fix, flag it the same way, don't
silently treat it as current-state fact.**

Production model per `docs/superpowers/specs/2026-08-26-activity-clock-lbrnet-handoff.md`: `K=4,
feature_dim=16`, trained 2026-08-25. Every fix below post-dates that training run and is therefore
**invisible to it** (either the model learned state boundaries from the pre-fix value, or the
dimension didn't exist in the trained vector at all):

| Dim | Contamination type | What the trained model actually saw | Fixed |
|---|---|---|---|
| `burstiness_index` | Invalid (formula wrong, twice) | Plain-CV bug (pre-2026-08-31), then a still-broken robust-CV variant (73.77% clip rate at real tick density, found 2026-09-02) | `1.58113883`-constant Index of Dispersion for Counts, 2026-09-02 |
| `log_scale_ratio` / `log_scale_expansion_ratio` | Invalid (formula wrong) | Raw-variance-ratio bug | Bipower variation (Barndorff-Nielsen & Shephard), 2026-08-31 |
| `mean_rev_z` | Invalid (formula wrong) | Mean/std z-score (non-robust) | Median/MAD (Kim & White 2004), committed `d2ab57c` 2026-09-02 |
| `amihud_illiquidity` | Invalid (formula wrong) | Linear ratio (`|log-ret|/dollarVolume`) | Sqrt-law + geometric-mean (Kyle & Obizhaeva 2016; Hasbrouck 2009), committed `a6d0630` 2026-09-03 |
| `liq_fragility` | Invalid (formula wrong) | ATR/volume-SMA composite | Dedicated median-based elasticity ratio, committed `a76ec00` 2026-09-03 |
| `vol_convexity` | Structural (dim removed) | Present as the model's 19th dim | Removed from schema entirely (19D→18D), 2026-08-31 |
| `fast_taleb_kurtosis` | Missing (never selected) | Not in training vector at all — first kurtosis dim ever added, never in `HMM_KEEP_DIMS` | Still not selected/retrained on, as of 2026-09-03 |
| `skewness_idx` | Source changed (value discontinuity) | TS3 time-bar cadence (stale, once-per-15-min) | Replaced with activity-clock (tick-native) source, `7c51f33`, 2026-08-27 |

**Conclusion, stated plainly**: at least 5 of 18 currently-shipped dims have a formula the deployed
model never saw, 1 dim the model was trained with no longer exists, 3 dims that would materially
matter for a fat-tail state (kurtosis, persistence, mean-reversion, all in activity-clock form) are
entirely absent from training, and 1 more had a source-level discontinuity. `PRODUCTION_TRIAGE.md`
row 1's own K=4/fat-tail-state sign-off question cannot be answered by this model — it needs a clean
retrain on the corrected/elite vector first, full stop.

## 6. Immediate next action

**Stale as of 2026-09-06 -- corrected**: Phase 0 (Gaussian-moment audit) is closed bar ONE item, not
the original four -- three were resolved between 2026-09-02 and 2026-09-04 (see §7's own per-row
entries: `hurst_exponent` row 7, `amihud_illiquidity` row 13, `liq_fragility` row 14,
`fast_mean_rev_z` row 19). **`relative_range` (row 3) remains genuinely open** -- a same-fix attempt
(median-range reference, mirroring `liq_fragility`) was implemented and reverted same day, 2026-09-06,
once the robust-ATR spec's finding (production ATR already intra-bar-reactive) undercut the premise;
still kept on ATR(14), no real-data validation run either way. Every OTHER dim's `FeatureScaler.h`
winsorization bound is GPD-recalibrated against real tick data as of 2026-09-04
(`tools/RECALIBRATION_LEDGER.md`) -- `relative_range`'s bound was never touched (correctly, since its
formula reverted to the original).
**Real next action: Phase 1 (whole-vector correlation audit) is unblocked and has NOT been started.**
Separately, §5's circularity finding (any HMM-state-dependent measurement is invalid against the
current contaminated model) still stands and blocks nothing about Phase 1 itself (Phase 1 is a
model-independent redundancy audit, not an HMM-state measurement).

**Updated 2026-09-02**: the real-tick-data blocker on row #2 (`burstiness_index`) is closed —
`tools/scid_processing/scid_to_ticks_parquet.cpp` shipped and `lbrnet/data/raw/mes_ticks.parquet`
(471.9M real rows) now exists. Re-running `burstiness_recalibration.cpp` against it found the
production `STATE_WINSOR_SIGMA=6.0` default clipping ~74% of real readings (rate-at-bound(6.0)=
73.7749%) — a genuinely broken bound, not a minor miscalibration.

**RESOLVED 2026-09-02 (same day, later)**: my first attempted fix (bounding the existing IAT-median/
MAD ratio via the Goh & Barabási (2008) transform) **FAILED real-data validation** — re-run against
the same 471.9M rows, rate-at-bound(6.0) was **unchanged at 73.7787%** and mean\|z\| roughly
*doubled* (27,828→67,899). Root cause (confirmed via Gemini literature review,
`lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_121`/`122`): any statistic built from inter-arrival TIMES
is structurally tied at real tick density (73.9% of real 100-tick windows have median IAT collapsed
to the timestamp field's own 1us resolution floor) — a bounding transform applied afterward just
relabels the same point-mass to a different constant, it cannot repair the underlying degeneracy.
**Real fix**: abandoned inter-arrival times entirely, reformulated to a robust Index of Dispersion
for Counts (Daley & Vere-Jones 2003) over K=10 fixed-width TIME sub-bins (bin width = window span/10,
self-scaling to local tick rate, not a fixed absolute width) — a tick count is always a well-defined
integer regardless of same-microsecond ties, so the degeneracy cannot occur structurally. Consistency
constant `1.58113883` (=√10/2, Poisson(N/K=10)'s true sigma/MAD ratio, not the standard-Normal
`1.4826`) verified both analytically and via 500K-trial Monte Carlo matching this repo's own
nth_element median convention. **Full real-data re-validation (all 471,930,891 rows,
`include/EventVelocityEngine.h`'s final formula): mean\|z\|=1.1356, max\|z\|=49.28, p50=0.682,
p90=2.541, p99=7.959, p99.9=15.870, rate-at-bound(6.0)=1.9605%** — a normal, sane winsorization rate.
The existing `STATE_WINSOR_SIGMA=6.0` default needs no further recalibration; the indicator's own
construction was the defect, not the bound.

## 7. Per-dim decision ledger (carried over from the 2026-08-29 brainstorm doc, canonical here going forward)

**Origin note**: this ledger was originally built and maintained as §9 of the now-deleted
2026-08-29 brainstorm doc. Moved here 2026-08-31 because this initiative now owns whole-vector
redundancy/relevance decisions (§0); the origin doc was removed 2026-09-09 (its still-relevant
content migrated: axis taxonomy → `2026-08-12-gang-literature-grounding-spec.md`, lead-time
criterion and bootstrap methodology debt → §8/§9 below). This ledger is the sole current source of
truth for per-dim status — a dim's absence from it means it has no current curation-initiative
decision, not that one should be inferred or imported from elsewhere.

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
| 1 | `log_scale_ratio` | IN, DECIDED (queued for next retrain) | Live (TS1) | **Assessed through the Vision lens, 2026-09-03**: same construct as row 4 (`log(short_BV/long_BV)`, Barndorff-Nielsen & Shephard bipower variation), just TS1/macro vs. TS2/tactical window. Correlation with row 4 rose 0.7638→0.8085 once both were de-noised (Spearman 1904 attenuation-correction logic) — confirms one shared latent signal, not independent information; under this HMM's hard diagonal covariance, keeping both is real double-counted evidence (Bouveyron & Brunet-Saumard 2014), not hypothetical. **Decision: keep this one (TS1/macro) as the vector's sole representative** — matches Elder's Triple Screen philosophy that Screen 1 sets the regime context the HMM is meant to track. Not actioned against the current contaminated model (per Vision section, `PRODUCTION_TRIAGE.md`) — queued for the next clean retrain alongside every other §5a contamination-manifest item, not a standalone lbrnet change | 2026-09-03 |
| 2 | `burstiness_index` | **IN, FIXED** | Event-driven (`raschkeBurst`) | Full history: (a) redirect to `raschkeBurst` landed 2026-08-29; (b) Phase 0 reformulated to robust CV `MAD/median × 1.4404199`; (c) real-tick-data validation (2026-09-02) found this STILL broken at production tick density (rate-at-bound(6.0)=73.77%); (d) first attempted fix (Goh-Barabási (2008) bounded transform on the same IAT-ratio) **FAILED real-data re-validation** (rate unchanged at 73.78%, mean\|z\| roughly doubled) — diagnosed with Gemini (`rc_gemini.log` `CLAUDE_BRIEF_121`/`122`) as a point-mass-degeneracy problem no post-hoc bounding transform can fix; (e) **REAL FIX, 2026-09-02**: reformulated entirely to a robust Index of Dispersion for Counts (Daley & Vere-Jones 2003) over K=10 self-scaling time sub-bins, consistency constant `1.58113883`=√10/2 (Poisson(10)'s exact sigma/MAD, verified analytically + 500K-trial Monte Carlo), same Goh-Barabási bounding device retained on the new ratio. **Full real-data re-validation, all 471.9M rows: mean\|z\|=1.1356, max\|z\|=49.28, rate-at-bound(6.0)=1.9605%** — normal, sane clip rate, existing `STATE_WINSOR_SIGMA=6.0` needs no recalibration. `test_event_velocity_engine.cpp` passing, `./build_dll.sh` clean | 2026-09-02 |
| 3 | `relative_range` | **IN, DECIDED** | Live (TS2) | #1 discriminator (0.6373), volatility-level axis, **kept as ATR(14)-based -- reformulation attempted and REVERTED, 2026-09-06**. Gemini's `CLAUDE_BRIEF_126` (2026-09-02/03) recommended replacing the shared ATR(14) denominator with a dedicated median-range reference (same fix as `liq_fragility`, row 14), on Kim & White (2004) robustness grounds. Implemented 2026-09-06 (`CalculateRelativeRange`, `StudyHelperFunctions.{h,cpp}`, wired into `TripleScreen2.cpp`), then reverted the same day once `docs/superpowers/specs/2026-09-05-robust-atr-reformulation-spec.md` §9's empirical finding came to light: production ATR is already intra-bar-reactive, so the fat-tail-contamination effect motivating the original fix is weaker than assumed. **RESOLVED 2026-09-14, real-data validation run (the original attempt was never committed, so this is a faithful reconstruction, not a recovery)**: built a candidate `(high-low)/median(range, last N closed bars)` variant (mirroring `liq_fragility`'s own proven reformulation) and compared it against the current `(high-low)/ATR(14,SMA)` on real TS2-equivalent 60-min bars from a single clean contract (`H26`, 1,409 valid bars, `lbrnet/data/raw/mes_ticks.parquet`). **Result: keep ATR(14) — the candidate is empirically worse, not better.** High correlation with the current formula (r=0.96 at N=14, r=0.92 at N=30) confirms no material new information (consistent with §9.1's ATR-already-reactive finding), but the candidate's own distribution is *more* fat-tailed (kurtosis 7.97/6.91 vs. current 3.95; skew 2.30/2.13 vs. 1.73; higher max/p99) — a shorter-memory median reference is less smooth than SMA(14) ATR and occasionally collapses small, spiking the ratio. Opposite of the robustness improvement `liq_fragility` actually got from the same style of fix. **Decision: no reformulation — current ATR(14)-SMA-based formula stays** | 2026-09-14 |
| 4 | `log_scale_expansion_ratio` | **OUT-HMM (decided, queued for next retrain)** | **RESOLVED 2026-09-08: Live (TS2), confirmed intra-bar-reactive** — `TripleScreen2.cpp:272-334`'s block is called unconditionally every tick with no bar-close gate found anywhere nearby, same as `relative_range`/`hurst_exponent`/`fisher_info`/`mean_rev_z` (row 3/7/9/18) — traced during the offline `.context` generator's Task 6 (`docs/superpowers/specs/2026-09-08-offline-context-generator-spec.md` §2a). This dim's own "unchecked" status is no longer open | **Assessed through the Vision lens, 2026-09-03**: same redundancy finding as row 1 (0.8085 correlation, genuine shared signal per attenuation-correction). **Decision: drop from `lbrnet`'s `HMM_KEEP_DIMS`** at the next clean retrain (matches `tail_index`/`micro_asymmetry`'s existing OUT-HMM precedent — C++ computation and non-HMM consumers unaffected, only HMM training selection changes). **Not wasted**: its TS2/tactical (faster-reacting) reading is a real, currently-missing candidate for a new trade-execution/risk-management gate — see `docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md` §2 item 4. Per the Vision's own standing rule, that new gate must be built stationarity-checked/percentile-based (Amihud-gate precedent), not a raw fixed threshold — the old, non-institutional pattern this whole initiative exists to move away from | 2026-09-08 |
| 5 | `vol_convexity` | **REMOVED FROM SCHEMA** | N/A | Executed 2026-08-31 (19D→18D) — full cleanup across all call sites, `FeatureScaler.h`'s five positional arrays, and `test_feature_scaler.cpp`; not reformulated, independently weak on two separate measures, correctly not worth further investment (§4 Phase 0) | 2026-08-31 |
| 6 | `lempel_ziv` | IN, not for fat-tail use | Event-native | Complexity axis rep, unchanged, already confirmed robust construct (Phase 0 audit) | 2026-08-23 |
| 7 | `hurst_exponent` | **IN, DECIDED** | Live (TS1), diluted weight | **RESOLVED 2026-09-02**: the ambiguous DFA q=2 vs MFDFA q=1 question was tested empirically, not assumed — `tools/observation_vector/dfa_vs_mfdfa_q1_montecarlo.py` extends the existing Kristoufek (2010) Monte Carlo methodology (exact Davies-Harte fGn simulation, production's exact N=100/minScale=8 window) with a paired q=1-vs-q=2 comparison, both on clean fGn and on fGn contaminated with realistic Student-t(3) fat-tail spikes. **Result contradicts the literature's general claim**: under contamination, q=2's bias flips sign across true-H (+0.19 at H=0.3 to -0.14 at H=0.7) while q=1's bias is uniformly positive and *worse* in magnitude at low H (+0.34 vs q2's +0.19 at H=0.3) — verified robust across 3 contamination severities and 2 seeds. **Decision: keep q=2 (standard DFA), do not adopt MFDFA q=1** — it doesn't survive direct empirical testing on this codebase's own exact algorithm. The real, separately-documented problem is window size (N=100 gives std≈0.13-0.19 regardless of q, "under-powered" per Weron/Kristoufek) — that's the actual lever, see row 10 | 2026-09-02 |
| 8 | `micro_asymmetry` | OUT-HMM | TS3, unchecked | Weakest overall (0.0001), already dropped from HMM selection, unchanged | 2026-08-25 |
| 9 | `fisher_info` | IN-WEAK | Live (TS1) | Second-worst (0.0011), already confirmed robust construct (Phase 0 audit), unchanged | 2026-08-29 |
| 11 | `tail_index` | OUT-HMM | Event-native | Structurally redundant with the model's own native ν_k; already confirmed robust construct (Phase 0 audit), unchanged. **Clock/window verified against the live call site, 2026-09-14**: genuinely event-driven (`SCStudies.cpp` gates `UpdateMarketPhysics()` on `currentPrice != s_lastPhysicsPrice`, real tick-native price changes only, same in `EventDataCollectorStudy.cpp`/`BackTesterStudy.cpp`); `TailRiskEngine` is default-constructed in `ContextManager.h` with no override anywhere — production actually runs `windowSize=500`, `tailPercent=0.05` (top 25 largest \|returns\|), EWMA-smoothed (α=0.2, ~9-observation half-life). Does not change the OUT-HMM decision (conceptual redundancy with ν_k, not a correlation question) — closes Phase 1's own flagged verification gap | 2026-09-14 |
| 13 | `amihud_illiquidity` | **IN, REFORMULATED** | Live-reactive, guarded `kLiveBarMinVolume=50.0` | **RESOLVED 2026-09-03**: reformulated to sqrt-law volume scaling (Kyle & Obizhaeva 2016 *Market Microstructure Invariance*; Lillo, Farmer & Mantegna 2003 — real-data-fitted impact exponent γ=0.512 on 2,138 real adjacent MES bar pairs, matching the theoretical 0.5) + geometric-mean aggregation (Hasbrouck 2009 — real-data leave-one-out test: 0.55% shift vs 5.0%/8.6% for median/raw-mean, an order of magnitude more outlier-robust). An activity-clock (dollar-volume-bar) alternative was tested and REJECTED — empirically worse (CV=1.175/skew=7.51 vs calendar-time sqrt-law's CV=0.918/skew=4.33), unlike the other dims where activity-clock treatment helped (bars span highly variable real time, concentrating undiluted return exposure). Committed `a6d0630`; kept in raw positive-ratio units so percentile-based gates (`amihud_percentile`) keep working unchanged (percentile rank is invariant under this monotonic reformulation). Full literature thread: `lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_123`. **Winsor-bound follow-up RESOLVED 2026-09-04, second pass, same day as a first attempt**: the first attempt (3036.0f) rested on a real bug — its "~5,000x pathological extrapolation" reasoning computed ζ_u as nTail/N_true_ticks, uncorrected for this dim's own 1-in-50 tick subsampling, undercounting the true per-tick exceedance probability by exactly that 50x factor. The correctly subsampling-unbiased ζ_u=nTail/nSample_retained=0.01 shows u=p99 is a genuine 1-in-100 event here — the same ordinary resolution as dims 3/8/10/13/16, not a uniquely pathological case. The direct "p=1/N return level" convention already used for those dims applies here too (the tool's own internal computation was correct all along; only the header's manual side-derivation was wrong). `DIM_WINSOR_SIGMA_OVERRIDE[11]` corrected to **191703.9** (tool's own report: N=467,376,145, return level=191,703.8870). `AMIHUD_ABSOLUTE_FLOOR` (the separate 1e-16 divide-by-zero floor, not the winsor bound) was re-checked and remains fine — it's negligibly small by design and was never tied to the old linear-ratio scale's magnitude. Separately (§5, unaffected by either fix): cross-state discrimination re-measurement against the *current* `models/hmm_model.pkl` remains circular — still deferred to a post-retrain re-measurement | 2026-09-04 |
| 14 | `liq_fragility` | **IN, REFORMULATED** | Live-reactive, same guard as row 13 | **RESOLVED 2026-09-02/03**: the Phase 0 ambiguity (own formula clean but consumed an externally-computed ATR/volume-SMA as its scale reference) is closed — reformulated to a dedicated median-based elasticity ratio (Foucault, Kadan & Kandel 2005 resilience concept; Morris & Shin 2004 liquidity-black-holes signature; Rousseeuw & Croux 1993 for the 50%-breakdown median/MAD-style scale estimators), no longer depends on ATR/volume-SMA at all — computes its own 30-bar median-range/median-sqrt-volume scale reference internally. Committed `a76ec00`. Real Python sample (2,211 bars) validated no collapse (`ScaleRef_W` min=0.042), `F_raw` median=1.0031 matching the theoretical neutral point, no blowups (max=6.63); correlation with the reformulated `amihud_illiquidity` on real data = 0.024 (confirms genuinely distinct axes). **Winsor-bound follow-up RESOLVED 2026-09-04**: full tick-level validation (471.9M ticks) has since completed. `FeatureScaler.h`'s `LOGZ_WINSOR_SIGMA_OVERRIDE[12]` (21.26f) was stale on two independent grounds — computed from a partial 38.5M-tick sample, and against the pre-`a76ec00` still-ATR-coupled formula. Fresh GPD refit against the current formula on the full 467,376,145-tick stream: u=p99=5.1727, n_tail=93,475, xi=+0.3147 (Frechet/unbounded), sigma=2.7671; same ζ_u-correction reasoning as row 13 confirms this is an ordinary ~100x-resolution case, direct p=1/N applies (tool's report: return level=1100.7906). Recalibrated to **1100.8** | 2026-09-04 |
| 17 | `fractal_dim` | IN | Time-bar (TS2), by design | Already confirmed robust construct (Phase 0 audit), unchanged | 2026-08-28 |
| 18 | `mean_rev_z` | IN-WEAK, **reformulated, committed** | Live (TS3) | **Phase 0 CONFIRMED Gaussian-moment** (literal textbook z-score, the most literal instance of the pattern in the vector) **and REFORMULATED 2026-08-31** to median/MAD (Kim & White 2004): both the price-stretch z-score and the lag-1 autocorrelation term now center on the median, build-verified, **committed 2026-09-02 (`d2ab57c`)** after sitting uncommitted for days. **Both follow-ups below RESOLVED 2026-09-04 (this row was stale)**: `FeatureScaler.h`'s `DIM_WINSOR_SIGMA_OVERRIDE[16]` recalibrated to 7.8 (real GPD fit, n=76,164); its prior "empirically null" verdict WAS retested against the new formula (`mean_rev_z_variant_comparison.py`, real data) and remains null (hit_rate=0.4988, p=0.790, n=12,716) -- see row 19's own fuller account of the same retest | 2026-09-04 |
| 20 | Drift/location (return z-score) | **OUT, tested and rejected** | N/A | Offline prototype (`tools/observation_vector/drift_location_eval.cpp`) found hit_rate below 0.5 at every horizon; rejected before any schema/C++ commitment. Unchanged | 2026-08-30 |
| 21 | Jump/bipower-variation ratio | **CANDIDATE-VALIDATED** | N/A | Offline prototype (`tools/observation_vector/jump_ratio_eval.cpp`) survives — real, substantial, opposite-of-naive-hypothesis effect. Not yet promoted to a schema field; promotion decision now sits behind this doc's own Phase 1 (redundancy audit), not yet run | 2026-08-30 |
| 22 | Hurst × volatility-level cross-term | CANDIDATE | N/A | Feature-engineering only, no new data; unchanged | 2026-08-29 |
| 23 | Self-exciting jump clustering (Hawkes intensity) | CANDIDATE-DEFERRED | N/A | Deferred to post-workstation window; unchanged | 2026-08-29 |
| 24 | Realized semi-variance decomposition | CANDIDATE-DEFERRED | N/A | Logged, no priority assigned yet; unchanged | 2026-08-29 |
| 25 | Recovery time-since-last-extreme-event construct | CANDIDATE-DEFERRED | N/A | Not yet designed, not just unprototyped; unchanged | 2026-08-29 |

## 8. Governing evaluation criterion: lead-time, not just membership (migrated from the
2026-08-29 brainstorm doc §1.10, that doc removed 2026-09-09)

This system has two real-money consumers of any fat-tail-relevant candidate, and both share a
timing requirement worth stating explicitly for every future candidate evaluated by this doc:
- **Offense — Atratus** (a separate options-trading app): buying cheap out-of-the-money options
  ahead of a tail event captures convexity. A confirmation that arrives after the market has
  already repriced the option is worthless to this consumer.
- **Defense — this system's own risk gates** (the TRAP-detection framework, `CLAUDE.md`'s "Trap
  Detection" section): exiting before catastrophic loss has the identical timing requirement from
  the other side. A flag that arrives after the drawdown has already happened is a post-mortem, not
  a risk control.

**The resulting criterion**: does a candidate detect *approach to* a fat-tail state, or only
*membership in* one, once it's already underway? A dim can show good cross-state discrimination in
an offline audit while being nearly useless to either consumer, if it only separates cleanly in the
middle of the event rather than ahead of it. `ν_k` (the model's native tail-heaviness parameter) is
structurally a steady-state descriptor by this criterion — necessary for classification, not
sufficient for either consumer's timing need. Apply this lens to any new tail-relevant candidate
this doc evaluates going forward, not just cross-state-ratio/redundancy.

## 9. Known methodology debt: i.i.d. bootstrap on overlapping forward-return signals (migrated
from the 2026-08-29 brainstorm doc §10.9, that doc removed 2026-09-09)

`tools/observation_vector/drift_location_eval.cpp` and `tools/observation_vector/jump_ratio_eval.cpp`
(rows 20/21 above) both resample individual per-tick forward-return signals as if independent, but
consecutive signals share most of their underlying tick data once the forward horizon (30-240
minutes) is much larger than the per-tick sampling interval — this overstates effective sample size
and understates every reported CI/p-value. Not yet quantified precisely or fixed for either tool.
Neither existing verdict changes (both effects are far enough from a naively-calibrated zero to
survive even a much wider correctly-calibrated CI), but a future *marginal* candidate tested on the
same apparatus should not be trusted without fixing this first. A real fix should reuse this
codebase's own already-measured Politis-White circular block-length (≈404.82, from the
`fractal_dim` window-widening work, `72ab967`) rather than re-derive one — e.g. a block bootstrap
applied uniformly across this whole candidate-validation tool family, not patched into one
candidate's test ad hoc.
