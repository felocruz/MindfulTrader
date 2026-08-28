# Spec: Observation-Vector Estimator Hardening -- Dead-Code Candidates and Window Widening

**Status**: Drafted 2026-08-25, companion to `lbrnet/docs/superpowers/specs/2026-08-25-vol-
convexity-removal-spec.md` (which this spec is downstream of -- read that one first for the full
evidence chain). **Not yet implemented.** Grounded in a literature search (Student-t HMM / fat-tail
regime-switching literature) plus a direct window/timeframe audit of every candidate feature's
actual C++ computation, run against the real source in this repo, not assumed from the Python side.

**MAJOR REVISION, 2026-08-27 — Section 5's "widen the time-bar window" framing was incomplete for
2 of its 3 dims, corrected via a real literature-grounding pass (Clark 1973 → Ané & Geman 2000 →
AFML ch. 2), not assumption.** `mean_rev_z` and `hurst_exponent` move to **activity-clock windowing**
(the same `ActivityClockManager` mechanism already shipped for kurtosis/`skewness_idx`), not just a
longer time-bar window — see the new Section 5a below, which supersedes part of Section 5 and all of
Section 6 for these two dims specifically. `recurrence_rate`/`fractal_dim` remain pure window-widening
candidates (Section 5 unchanged) — literature search found **no** direct precedent either way for RQA
or Sevcik fractal dimension under information-driven bars; that's silence, not a ruling, and this
spec does not extend the AFML/Clark argument to them by unsupported analogy. Read Section 5a before
touching `mean_rev_z`'s or `hurst_exponent`'s C++ computation under the old Section 5/6 framing.

## 1. Purpose

lbrnet's Student-t HMM is dropping 4 of its 16 model-input dimensions (`vol_convexity`,
`tail_index`, `skewness_idx`, `micro_asymmetry`) because they contribute no measurable cross-state
discriminative power to the fitted model. This spec addresses the MindfulTrader-side consequences
and a separate, related finding from the same investigation: **3 more dimensions that remain in
the model are genuinely undersampled and should have their C++ computation windows widened**, and
**2 more that look undersampled at a glance are not** -- their near-zero discrimination is more
likely a pre-existing data-quality artifact this project has already partially documented, and
widening their window would not fix it.

## 2. What does NOT change here

- **`vol_convexity` (`CalculateVolConvexity`, `StudyHelperFunctions.cpp:3322`) and `tail_index`
  (`TailRiskEngine`, `TailRiskEngine.h`) computations are unchanged.** Both remain load-bearing for
  live, non-HMM consumers: `vol_convexity` feeds backtest barrier-width modulation (mirrored on the
  lbrnet side by `_apply_context_barrier_modulation()`); `tail_index` feeds `PositionManager.cpp`'s
  own `1/tail_index` sizing logic (mirrored by `backtest_runner.py::_hill_alpha_factor()`). lbrnet
  training simply stops selecting these two as HMM model inputs -- nothing changes about how or how
  often MindfulTrader computes or transmits them.
- **`mts_schema.fbs` is unchanged.** No field is added, removed, or resized by this spec -- see the
  companion `schema/` spec for the full reasoning (field-shape is orthogonal to which fields the
  HMM happens to select as training input, and orthogonal to window-length changes on the C++ side,
  which affect only what value a `float` field carries, not its wire shape).

## 3. Dead-code candidates (not yet acted on -- MindfulTrader's own timing decision)

`skewness_idx` (`CalculateSkewness` -> `BowleySkewness`) and `micro_asymmetry`
(`ofae::ComputeMicroAsymmetry`, `OrderFlowAsymmetryEngine.h:26`) have **no consumer found anywhere
in lbrnet outside the HMM's own model input and the regime-taxonomy files that read the model's
fitted output** (grepped `lbrnet/backtest`, `lbrnet/core`, `lbrnet/data` -- zero hits, unlike
`vol_convexity`/`tail_index` which both have real other consumers, Section 2). Once lbrnet's
companion spec lands and confirms no downstream consumer materializes, these two C++ computations
become genuine dead-code-removal candidates under this project's own standing mandate (delete
superseded code outright, no back-compat shims). **Not acted on in this spec** -- removing a live
per-tick/per-bar computation is a real MindfulTrader decision with its own timing (e.g. bundling
with the next observation-vector schema pass), not a mechanical consequence of a Python-side
training-input change landing first. Tracked here so it isn't lost, not executed here.

## 4. Window audit -- verified against the actual C++ source, 2026-08-25

| Feature | Function (file:line) | Screen/timeframe | Current window | Real-world span |
|---|---|---|---|---|
| `hurst_exponent` | `CalculateHurstExponent(sc, length, 8)`, `StudyHelperFunctions.cpp:2489` (feeds the observation vector via `TripleScreen1.cpp:483` -- the `:2639` overload is a legacy display-only wrapper, unaffected either way) | TS1, 240min | `length = clamp(observation_window_n*5, 50, 200)` | 50-200 bars = **8.3-33.3 days** |
| `fisher_info` | `CalculateFisherInformation(sc, lookback_n)` -> `cfc::ComputeFisherInformation`, `StudyHelperFunctions.cpp:3055` | TS1, 240min | `lookback_n = clamp(fisher_obs_window_n*3, 30, 120)`, independently adaptive from Hurst's own window | 30-120 bars = **5-20 days** |
| `recurrence_rate` | `CalculateRecurrenceRate(sc, lookback_n)`, `StudyHelperFunctions.cpp:3369` | TS2, 60min | `lookback_n = max(30, observation_window_n)`, clamped `[2,40]` -> effectively 30-40 | 30-40 bars = **1.25-1.67 days** |
| `fractal_dim` | `CalculateFractalDimension(sc, lookback_n)`, `StudyHelperFunctions.cpp:3177` (Sevcik path-length method) | TS2, 60min | Same `max(30, observation_window_n)` as `recurrence_rate` | 30-40 bars = **1.25-1.67 days** |
| `mean_rev_z` | `CalculateMeanReversionSpeed(sc, lookback_n)`, `StudyHelperFunctions.cpp:3238` | TS3, 15min | Outer z-score `n = clamp(lookback_n, 5, 40)`; inner lag-1 autocorrelation `rho` uses `m=n-1`, **the same window**, not independently parameterized | 10-40 bars = **2.5-10 hours** |

Reference point: this HMM's own fitted mean regime tenure, on the current production model, is
~589 bars at TS3/15min granularity (`Shannon mean tenure bars: 588.8`, 2026-08-25 sign-off run) --
**approximately 6.1 real days**.

## 5. Widen the window: `recurrence_rate`, `fractal_dim`, `mean_rev_z`

All three currently span well under a day (`recurrence_rate`/`fractal_dim`: 1.25-1.67 days;
`mean_rev_z`: 2.5-10 hours) against a ~6.1-day regime-persistence reference. This project's own
literature search (2026-08-25) supports widening rather than dropping: RQA in financial
applications commonly uses 100-252-*day* windows in practice (though "no strict theoretical
minimum" is sometimes claimed formally); rescaled-range/DFA-family estimators generally need more
observations, not fewer, to stabilize.

**Proposed targets, sized to be comparable to the regime-tenure reference, not an arbitrary round
number**:
- `recurrence_rate`/`fractal_dim` (currently `max(30, observation_window_n)` bars on TS2/60min):
  widen toward **~150 bars** (≈6.25 days at 60min) -- matches the regime-tenure reference at the
  same order of magnitude a genuinely regime-persistent statistic should need to resolve one full
  regime cycle, not just a fraction of one.
- `mean_rev_z` (currently `clamp(lookback_n, 5, 40)` bars on TS3/15min): widen toward **~600 bars**
  (≈6.25 days at 15min) for the same reason. **Additionally**: decouple the inner `rho`
  autocorrelation estimate from the outer z-score's window -- autocorrelation at lag 1 is its own
  distinct statistic with its own sample-size behavior, and sharing one lookback for both was never
  a deliberate design choice per the source (no comment justifying it), just an implementation
  shortcut. Give `rho` its own explicit, likely-longer lookback.

**These were proposed targets for discussion, not finalized magic numbers** -- matching this
project's own standing rule against inventing round numbers without derivation (`CLAUDE.md`'s
"ungrounded utility constants" pattern). The ~150/~600 figures were sized only to match the
regime-tenure reference at the same order of magnitude, pending a rigorous derivation. **For
`recurrence_rate`/`fractal_dim`, that derivation is now done -- see Section 5b immediately below,
which supersedes the ~150-bar figure with a measured 400. For `mean_rev_z`, the ~600-bar figure is
moot -- see Section 5a, which moves it to activity-clock treatment instead of a wider time-bar
window.**

## 5b. `recurrence_rate`/`fractal_dim` final window, derived 2026-08-27 -- **400 bars, not ~150**

`mean_rev_z` moved out of this window-widening path entirely (Section 5a, activity-clock treatment
instead) -- this section covers `recurrence_rate`/`fractal_dim` only, the two dims Section 5a
explicitly leaves on the pure time-bar-widening path.

**Method**: Politis & White (2004) / Patton, Politis & White (2009) automatic optimal block-length
selection (`arch.bootstrap.optimal_block_length`) -- the same tool this project already depends on
(`arch>=8.0`) for the HMM gate-threshold calibration's own `--window-rows` derivation
(`docs/superpowers/specs/2026-08-24-hmm-gate-threshold-calibration-institutional-grade-spec.md`
Section 5b), applied here to a different series for a different (but related) purpose. Run against
real MES data (`lbrnet/data/raw/mes_wave_60m.parquet`, TS2/60min, 2023-06-04..2026-08-18, 19,727
bars) via `tools/window_autocorrelation_diagnostic.py`, on `|log-returns|` (volatility clustering,
Cont 2001) -- raw log-returns themselves decorrelate in ~3 bars (consistent with weak-form market
efficiency) and are not the relevant proxy for regime/structural persistence; `|returns|` is.

**Measured**: `stationary ≈ 353.6` bars, `circular ≈ 404.8` bars. Internally consistent with
Politis-White's own tuning-constant ratio (`b_circular/b_stationary = (2/(4/3))^(1/3) = 1.1447`;
measured `404.82/353.64 = 1.1447` exactly) -- confirms the numbers are correctly computed, not
noise. Contract-roll-affected bars (~12 across the series, real but unadjusted-splice price jumps)
were checked and found to change nothing (353.57/404.74 with them excluded) -- negligible at this
cadence, confirmed rather than assumed.

**Which estimator, and why**: `optimal_block_length` returns two numbers calibrated for two
*different* bootstrap resampling schemes, not two candidate answers to one question -- stationary
(`b_sb`) is the *mean* of a geometrically-distributed random block length (for the stationary
bootstrap); circular (`b_cb`) is a *fixed*-length block (for the circular block bootstrap). This
spec isn't bootstrapping -- it's picking one fixed rolling-window length for a point estimator
(RQA/Sevcik). **Circular is the structurally correct analogy** (a fixed number, calibrated for
fixed-length blocks), not stationary (the mean of a distribution repurposed as a literal window
size). This is not a contradiction with the HMM calibration spec's own choice of `stationary` --
that spec feeds the result directly into `StationaryBootstrap` (its literal designed use case);
this is a different application with a different correct answer from the same tool.

**Final target: 400 bars** (rounded from the measured circular value 404.82 -- a ~1% convenience
rounding for config readability, not itself a derivation; the derived value is 404.82).

**The real finding, worth keeping independent of the final number**: 400 bars at TS2/60min is
**~16.7 days** -- longer than both the original ~150-bar guess (Section 5) and the ~6.1-day
regime-tenure reference that motivated proposing it. The measured decorrelation time for volatility
clustering on this instrument is real information, materially larger than either prior guess.

Sources: Patton, Politis & White (2009 correction), *Econometric Reviews* 28(4); Politis & White
(2004), *Journal of Business & Economic Statistics* 22(2); `arch.bootstrap.optimal_block_length`
documentation (confirms the `b_sb`/`b_cb` distinction and per-bootstrap-type usage guidance).

**Not yet done**: implementing this 400-bar target in `CalculateRecurrenceRate`/
`CalculateFractalDimension` (`StudyHelperFunctions.cpp`), `RecurrenceRateEngine::kMaxClosedBars`/
`RQAEpsilonSelector.h`'s `kRQASelectorMaxN` (currently 256, sized for the old ~150 target -- must be
bumped again for 400), and `TripleScreen2.cpp`'s adaptive-window clamp. Native test coverage for
`fractal_dim` (no pure-header extraction exists yet, unlike `recurrence_rate`'s
`RecurrenceRateEngine.h`) still needs writing per Section 8's acceptance gate.

**Cross-reference, 2026-08-27, so this 400-bar derivation doesn't read as contradicting existing
Gang-doc history**: `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`'s Sevcik
fractal-dimension row and RQA-epsilon row are both marked `validated` (2026-08-13) — that verdict
covers *formula choice* (Sevcik vs. Higuchi/Katz) and *epsilon-selection methodology* only, a
different axis from *lookback-horizon length for regime relevance*, which is what this section
derives. Both Gang-doc rows now carry an explicit 2026-08-27 scope note saying so — read them, this
400-bar figure doesn't undo either "validated" verdict, it answers a question neither one asked.

## 5a. Literature-grounded reframe, 2026-08-27: `mean_rev_z` and `hurst_exponent` move to
activity-clock windowing, not time-bar widening

**Origin of this section**: prompted by a user question during the Section 5 window-widening
handoff ("is there institutional literature that lets us decide which dims can genuinely move to
activity-clock windows?") — answered with a real literature pass (web search, not assumption),
summarized here so the decision is auditable rather than a one-off chat answer.

**The root citation, predating and grounding AFML itself**: Clark (1973), *A Subordinated
Stochastic Process Model with Finite Variance for Speculative Prices*, *Econometrica* 41:135-156 --
models price as a process subordinated to cumulative trading volume, arguing markets advance in
"transaction time" / "business time," not calendar time, and that much of the apparent
non-normality of returns is a clock-choice artifact, not a distributional fact about the underlying
process. Ané & Geman (2000, *Journal of Finance* -- already cited in `2026-08-26-activity-clock-
tail-risk-and-decay-spec.md` §3 for kurtosis specifically) is the direct modern extension. López de
Prado's AFML ch. 2 (information-driven bars) is the engineering realization of this same,
50-year-old theoretical claim -- not an independent finding. **AFML's own critique of time bars
names three defects, not one**: serial correlation, heteroskedasticity, and non-normality. This
project's existing activity-clock work (kurtosis, `skewness_idx`) has so far only acted on the
non-normality leg (moments). The other two legs are directly relevant to two dims in this spec:

- **`mean_rev_z`** is built on lag-1 return autocorrelation (`rho`) -- i.e. it *is* a serial-
  correlation statistic, the first defect AFML names. Separate literature on market microstructure
  (non-synchronous trading, bid-ask bounce) independently establishes that **calendar-time sampling
  is itself a source of *spurious* serial correlation** -- autocorrelation that appears even when
  the true underlying price process has none. That is precisely the artifact `mean_rev_z`'s
  momentum-vs-reversion classification would be vulnerable to if computed on TS3 15-min time bars.
  This is not an analogy borrowed from kurtosis's justification -- it is a separate, direct
  literature result for autocorrelation-based estimators specifically.
- **`hurst_exponent`** (long-memory/persistence estimation): direct literature support that
  **trading time -- cumulative trades executed -- is the more natural timescale for long-memory
  estimation, reducing biases introduced by regular (calendar-time) sampling.** This *reverses* an
  earlier, unresearched claim made mid-conversation (2026-08-27, before this literature pass) that
  Hurst was "genuinely time-based and resistant to clock conversion" because long-range dependence
  is inherently about self-similarity across time lags -- that reasoning was never checked against
  the actual literature before being stated, and the literature says the opposite. Corrected here,
  not silently dropped.

**What this means concretely, and what it does NOT mean**:
- `mean_rev_z`'s outer z-score AND inner `rho` autocorrelation should be recomputed over
  `ActivityClockManager`'s imbalance-bar return buffer, mirroring the kurtosis precedent -- exact
  additive-vs-replacement choice (dual-clock twin like kurtosis, or in-place replacement like
  `skewness_idx`) depends on whether `mean_rev_z` has live gate consumers to protect: **confirmed
  directly, it does** (`Scoring.cpp:305`, `isMeanReversionPattern && ctx.meanRevZ > 2.0f`) -- so this
  should follow kurtosis's **additive dual-clock** pattern, not `skewness_idx`'s replacement pattern,
  unless that gate is being re-calibrated in the same pass.
- `hurst_exponent` similarly needs an activity-clock twin design, not a replacement decision made
  yet -- its own live consumers (`StudyHelperFunctions.cpp:623`, `TripleScreen3.cpp` regime
  thresholds) are calibrated on the existing TS1/240-min value and should be checked the same way
  before deciding additive-vs-replace.
- **`recurrence_rate` and `fractal_dim` are explicitly NOT reframed by this section.** The literature
  search found no direct precedent -- for or against -- testing RQA or Sevcik fractal dimension
  under information-driven vs. time-bar sampling. Extending the Clark/AFML argument to them would be
  an unsupported analogy, not a literature-grounded decision like the two above. They remain pure
  window-widening candidates per Section 5, on TS2 time bars, pending either (a) a future literature
  finding that actually covers RQA/fractal-dimension estimators under alternative clocks, or (b) an
  explicit, flagged engineering decision to extend the pattern by analogy anyway -- not silently.
- **Real stakes beyond this spec's own scope, named explicitly by the user**: `hurst_exponent` is
  independently known to be the HMM's single worst cross-state discriminator (exactly `0.0000`,
  Section 6 below) and to carry the worst scale-collapse data-quality artifact of any of the 16
  original dims. If clock choice is a genuine contributor to that failure (not certain, but now
  literature-plausible in a way it wasn't before this pass), this connects directly to row 1's
  Student-t HMM sign-off problem, not just to this spec's narrower window-sizing question --
  `PRODUCTION_TRIAGE.md` row 1 should reflect this connection, not just this spec.

**Not decided by this section, left for a future implementation plan**: exact design (additive twin
field vs. replacement, matching row 14's now-demonstrated struct-in-place-edit pattern), whether the
existing time-bar `hurst_exponent`/`mean_rev_z` values keep their current calibrated gates untouched
while a fast twin is added (kurtosis's pattern), and sequencing against the still-open `fisher_info`/
`recurrence_rate`/`fractal_dim` work below.

## 6. `fisher_info`: still out of scope, unresearched for this question -- do not conflate with `hurst_exponent` above

`fisher_info` already runs on a window comparable to the ~6.1-day regime-tenure reference (5-20
days) -- widening further has no clear justification from the sample-size argument that motivates
Section 5, same as `hurst_exponent`. **Unlike `hurst_exponent`, this spec has NOT researched whether
Clark/AFML-style clock-conversion literature applies to `fisher_info`** (Ehlers' Fisher Transform --
a technical-analysis price transform, not a moment/autocorrelation/long-memory statistic in the same
family as the three dims above, so the same literature may not transfer by analogy either). It ranks
third-worst in the HMM's cross-state discrimination (0.0011). **This project's own prior audit**
(`lbrnet/docs/superpowers/specs/2026-08-14-observation-vector-full-institutional-coverage-spec.md`
row 6) already found `hurst_exponent`'s near-zero discrimination plausibly explained by its
scale-collapse data-quality artifact (24.85% pre-shrinkage `|z|>=6` rate) independent of window
length or clock choice -- `fisher_info` has not yet had the equivalent tail-conditional noise
decomposition run against it.

**Recommended next step, not part of this spec's own implementation scope**: run
`fisher_info` through the same tail-conditional noise-decomposition technique already validated for
`hurst_exponent` (`hmm_feature_selection.md`'s Phase 4 audit) before deciding whether it needs a
data-quality fix, a drop, a clock-conversion literature check of its own, or is genuinely fine as-is.
Do not widen its window as a first move, and do not assume Section 5a's `hurst_exponent` finding
transfers to it without checking.

## 7. Non-goals

- Not removing `skewness_idx`/`micro_asymmetry`'s C++ computation in this spec (Section 3) --
  timing is MindfulTrader's own call, tracked but not executed here.
- Not touching `vol_convexity`/`tail_index` C++ computation at all (Section 2).
- Not deriving the exact widened window sizes with full rigor (Section 5) -- proposed targets only,
  pending an autocorrelation-time diagnostic. Applies only to `recurrence_rate`/`fractal_dim` now --
  `mean_rev_z` moved to Section 5a's activity-clock design instead of a time-bar target number.
- Not designing `mean_rev_z`/`hurst_exponent`'s activity-clock twins in implementation detail
  (Section 5a) -- the literature grounding and additive-vs-replace framing are decided; the concrete
  C++ design (field/state shape, gate integration) is a future implementation plan's job, same as
  kurtosis went through its own plan before code was written.
- Not fixing `fisher_info`'s scale-collapse/discrimination question or researching whether Clark/AFML
  literature applies to it (Section 6) -- flagged as the right next step, not attempted here.
- Not touching `mts_schema.fbs` -- see companion `schema/` spec. (`mean_rev_z`/`hurst_exponent`'s
  eventual activity-clock twins would need a schema entry when implemented, same as
  `fast_taleb_kurtosis` -- not yet filed, this spec is design-grounding only.)

## 8. Acceptance gates

- Widened-window changes for `recurrence_rate`/`fractal_dim` are backed by an explicit
  autocorrelation-time or equivalent derivation for the final window size, not the Section 5
  proposed-target numbers taken as final without that check.
- `mean_rev_z`'s `rho` autocorrelation, when its activity-clock twin is implemented (Section 5a),
  gets its own, separately-justified imbalance-bar lookback, not its outer z-score's window by
  default -- same principle Section 5 established for the time-bar version, carried forward rather
  than dropped when the clock changed.
- Existing MindfulTrader unit test coverage for `recurrence_rate`/`fractal_dim`
  (`test_indicator_computations.cpp` or equivalent) updated to reflect new window bounds, and a
  regression test confirms the widened computation still respects the "historical-bars-only, never
  reads the live forming bar" contract these adaptive-window functions document for themselves --
  **flagged for direct verification before relying on it**: a 2026-08-27 code read of
  `CalculateMeanReversionSpeed`/`CalculateRecurrenceRate` found both explicitly reference
  `sc.BaseData[SC_LAST][sc.Index]` (the live, still-forming bar) as their current-point term, which
  appears to be in tension with this stated contract -- resolve which is actually true (the contract
  wording, or the current-point behavior) before writing a regression test that assumes either.
- `hurst_exponent` and `mean_rev_z` are NOT implemented by this spec (Section 5a is a design/
  literature-grounding decision, not an implementation) -- confirmed via diff review, not just
  stated intent, same discipline as this section already applied to the old Section 6.
- `fisher_info` is NOT touched by this spec's own implementation -- confirmed via diff review.
- Cross-referenced from `lbrnet`'s companion spec and `schema/PENDING_SCHEMA_CHANGES.md` (if either
  needs an entry -- Section 2 concludes neither does for this spec's own scope; a future
  `mean_rev_z`/`hurst_exponent` activity-clock plan will need its own schema entry, tracked there,
  not retrofitted into this spec).

## 9. Residual risk

- Widening `recurrence_rate`/`fractal_dim`'s windows changes their live, currently-
  transmitted values for every consumer, not just the HMM training path -- confirm no other live
  C++ consumer (routing, sizing, display subgraphs) depends on the *current* short-window behavior
  before widening, the same class of check Section 3 already applies to the dead-code candidates.
- `recurrence_rate`/`fractal_dim`'s window derivation is now done (Section 5b, `400` bars, measured
  not guessed) -- the ~150-bar figure this bullet used to warn against is superseded, not still a
  live risk. Residual risk now is implementation-stage only: the two `RQAEpsilonSelector.h`/
  `RecurrenceRateEngine::kMaxClosedBars` capacity constants and `fractal_dim`'s missing native test
  coverage, both named in Section 5b's own "Not yet done" list.
- Section 5a's `mean_rev_z`/`hurst_exponent` activity-clock reframe both have live gate consumers
  calibrated on their current (time-bar) values (`Scoring.cpp:305`; `StudyHelperFunctions.cpp:623`/
  `TripleScreen3.cpp` regime thresholds) -- an eventual implementation plan must protect those
  exactly like kurtosis's own plan did, not silently disturb them by treating this as a like-for-like
  swap.
- Section 5a's literature grounding (Clark 1973; Ané & Geman 2000; AFML ch. 2) establishes that
  calendar-time sampling is *a* source of spurious serial correlation / long-memory estimation bias
  -- it does not establish that it is *the dominant* source for this system's specific data, or that
  an activity-clock twin will empirically outperform the existing time-bar value once built. Same
  epistemic caution this project already applies elsewhere (e.g. the retracted "divergence between
  clocks is itself informative" claim, `2026-08-26-activity-clock-tail-risk-and-decay-spec.md` §4
  item 5) -- an empirical backtest comparison remains the real test, not the literature alone.
