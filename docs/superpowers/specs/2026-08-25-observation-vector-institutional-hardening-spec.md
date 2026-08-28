# Spec: Observation-Vector Estimator Hardening -- Dead-Code Candidates and Window Widening

**Status**: Drafted 2026-08-25, companion to `lbrnet/docs/superpowers/specs/2026-08-25-vol-
convexity-removal-spec.md` (which this spec is downstream of -- read that one first for the full
evidence chain). **Not yet implemented.** Grounded in a literature search (Student-t HMM / fat-tail
regime-switching literature) plus a direct window/timeframe audit of every candidate feature's
actual C++ computation, run against the real source in this repo, not assumed from the Python side.

**MAJOR REVISION, 2026-08-27, TWO PASSES SAME DAY — Section 5's "widen the time-bar window" framing
was incomplete for 3 of its 4 candidate dims, corrected via real literature-grounding passes (Clark
1973 → Ané & Geman 2000 → AFML ch. 2 for the first pass; RQA-on-event-indexed-sequences precedent
for the second), not assumption.** `mean_rev_z`, `hurst_exponent`, and (second pass) `recurrence_rate`
all move to **activity-clock treatment** (the same `ActivityClockManager` mechanism already shipped
for kurtosis/`skewness_idx`) instead of a longer time-bar window — see Section 5a below, which
supersedes part of Section 5 and all of Section 6 for these three dims. **`fractal_dim` alone stays
on the pure time-bar-widening path** (Section 5, now derived to 400 bars in Section 5b) — a second,
targeted literature search specifically for the adjacent case (fractal-dimension/path-length methods
under event-indexed sampling, not just finance-specific tests) still found no precedent, confirming
rather than reversing the original silence. `recurrence_rate` and `fractal_dim` are NOT symmetric —
treating them as a pair was this spec's own earlier error. Read Section 5a in full before touching
any of these four dims' C++ computation under the old Section 5/6 framing.

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

## 5b. `fractal_dim` final window, derived 2026-08-27 -- **400 bars, not ~150** (`recurrence_rate`
SUPERSEDED here by Section 5a's second pass -- read that note before treating this section's
`recurrence_rate` measurements as still applying to it)

**Correction, second literature pass, same day**: this section was originally written for
`recurrence_rate`/`fractal_dim` together (they share one window constant today,
`slow_window_n` in `TripleScreen2.cpp`). Section 5a's second pass since found real cross-domain
literature grounding for `recurrence_rate` to move to activity-clock treatment (like `mean_rev_z`),
the same way `mean_rev_z` was superseded out of this section in the first pass. **This section's
400-bar measurement and final target now apply to `fractal_dim` only.** The underlying measurement
itself (TS2/60min `|log-returns|` volatility-clustering decorrelation time) is unaffected and
remains valid for whichever dim(s) end up on the time-bar path -- only the *scope* of dims it
applies to changed. **New engineering consequence, not yet implemented**: `recurrence_rate` and
`fractal_dim` currently share one window constant in the C++ source -- if `recurrence_rate` moves to
activity-clock and `fractal_dim` doesn't, they need decoupling into two independently-parameterized
windows, the same class of fix Section 5's original text already required for `mean_rev_z`'s inner
`rho` vs. its outer z-score.

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
(Sevcik, now that `recurrence_rate` is superseded out above). **Circular is the structurally correct analogy** (a fixed number, calibrated for
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

**Not yet done**: implementing this 400-bar target in `CalculateFractalDimension`
(`StudyHelperFunctions.cpp`) and `TripleScreen2.cpp`'s adaptive-window clamp, decoupled from
`recurrence_rate`'s own window per the engineering consequence noted above. Native test coverage for
`fractal_dim` (no pure-header extraction exists yet, unlike `recurrence_rate`'s
`RecurrenceRateEngine.h`) still needs writing per Section 8's acceptance gate. **`RecurrenceRateEngine::
kMaxClosedBars`/`RQAEpsilonSelector.h`'s `kRQASelectorMaxN` bump for 400 is now moot** unless
`recurrence_rate` ends up staying on the time-bar path after all (Section 5a's activity-clock move
is a literature-grounded direction, not yet a locked implementation decision) -- don't bump that
capacity constant until that's actually decided.

**Concrete coupling point that must be broken, verified by reading the actual code 2026-08-28 --
this is the one line that silently defeats the whole two-window decision if missed**:
`ContextManager.cpp:589` currently reads `m_localRiskContext.fractalDim = obs[OBS_FRACTAL_DIM];` --
i.e. `LocalRiskContext.fractalDim` (what `PositionManager.cpp` GAP 11's gate reads) is copied
straight from the same `ObservationData` slot the HMM vector uses. Today that's a no-op distinction
because both sides compute from the same `slow_window_n`-windowed call
(`TripleScreen2.cpp:290`, `float fractalDim = CalculateFractalDimension(sc, slow_window_n);`) --
one value, written to both places. Once `TripleScreen2.cpp` widens the HMM-bound computation to
400 bars, `obs[OBS_FRACTAL_DIM]` becomes the 400-bar value, and this line would carry that straight
into `LocalRiskContext.fractalDim` too -- silently re-coupling the two windows the split was
designed to separate, with no compiler error and no test failure to catch it (the gate already
never fires, so a regression here would be invisible in current test coverage). **Implementation
requirement, not optional**: `TripleScreen2.cpp` must compute a second, independent short-window
`fractalDim` value (its own local, at the current/unwidened window) and thread *that* into
`m_localRiskContext.fractalDim` -- `ContextManager.cpp:589` must stop sourcing it from
`obs[OBS_FRACTAL_DIM]` once that slot is 400-bar. This is the actual mechanism of "decoupled from
the HMM's" stated throughout this section and Section 9 -- naming it here so it isn't the one
line rediscovered mid-implementation instead of planned for up front.

**IMPLEMENTED, 2026-08-28**: done exactly as specified above, verified by full build + native
tests, not just read-through. `include/SevcikFractalDimension.h` (pure, header-only Sevcik math,
already existed from the threshold-migration tooling) is now the single source of truth --
`StudyHelperFunctions.cpp`'s `CalculateFractalDimension` was refactored to delegate to it instead
of carrying its own duplicate inline copy, eliminating the drift risk between the two. Its
signature gained a `persistentVarIndex` parameter (default preserves the original single-call
behavior) because the split design calls it *twice per tick* with two different `lookback_n`
values, and the degenerate-window carry-forward state is genuinely per-call, not shared --
`FRACTAL_DIM_SHORT_LAST_VALID_VALUE` (new persistent var, index 42) was added alongside the
existing `FRACTAL_DIM_LAST_VALID_VALUE` so the two calls can't corrupt each other's fallback
state. `TripleScreen2.cpp` now computes `fractalDim` at the new 400-bar window (feeds
`obs->mutate_fractal_dim`, unchanged `recurrenceRate`/`slow_window_n` wiring otherwise) and a
separate `fractalDimShort` at the original `slow_window_n`, pushed directly into `ContextManager`
via a new `SetFractalDimShort()` setter (same "push directly" precedent as `SetRegimeDuration()`).
`ContextManager.cpp:589` now reads `m_localRiskContext.fractalDim = m_fractalDimShortRaw;` instead
of `obs[OBS_FRACTAL_DIM]` -- the coupling this note warned about is broken. New native test
`tests/cpp/test_sevcik_fractal_dimension.cpp` verifies the pure extraction against a brute-force
reference replicating the original's exact asymmetric windowing at n=30/40/150/400 (all pass).
Full `./build_dll.sh --no-clean` succeeds; `test_recurrence_rate_engine`, `test_rqa_epsilon`,
`test_feature_scaler` all regression-pass. No `PositionManager.cpp` change (confirmed unneeded
above -- its gate is dead code and stays inert, tracked via `PRODUCTION_TRIAGE.md` row 11/15).

**Cross-reference, 2026-08-27, so this 400-bar derivation doesn't read as contradicting existing
Gang-doc history**: `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`'s Sevcik
fractal-dimension row and RQA-epsilon row are both marked `validated` (2026-08-13) — that verdict
covers *formula choice* (Sevcik vs. Higuchi/Katz) and *epsilon-selection methodology* only, a
different axis from *lookback-horizon length for regime relevance*, which is what this section
derives. Both Gang-doc rows now carry an explicit 2026-08-27 scope note saying so — read them, this
400-bar figure doesn't undo either "validated" verdict, it answers a question neither one asked.

### `fractal_dim`'s live gate consumer (`PositionManager.cpp` GAP 11) — two-window decision, plus a
separately-broken threshold found while investigating it, 2026-08-27/28

**The additive-vs-replace question this spec's own Section 9 flagged for `fractal_dim`'s gate is now
answered, and it changed based on real measurement, not assumption.** `tools/fractal_dim_threshold_
migration.cpp`/`.py` (built to percentile-match the live gate's `1.6f`/`1.3f` thresholds against the
new 400-bar distribution, same methodology as Task 7's kurtosis/skewness migration) produced a
paired `(fractal_dim@30, fractal_dim@400)` sample over 19,327 real MES TS2/60min bars and found
**`correlation(fractal_dim@30, fractal_dim@400) = 0.0115`** — essentially zero, not "positive but
attenuated" as the tool's own docstring expected. **This is real evidence the two windows measure
largely independent information, not the same signal at different noise levels**: `PositionManager.
cpp`'s GAP 11 gate asks "is the market rough *right now*, for this order's immediate routing" (a
short-horizon execution question); the 400-bar window was derived from a *6.1-day HMM regime-tenure*
reference (a structural persistence question). Given that, **decision: split, not shared** —
`PositionManager.cpp`'s gate stays on a short window (its own, decoupled from the HMM's), the HMM
vector gets the 400-bar value. This reverses an earlier position in this same investigation (split
was initially rejected for lacking a complementary-information rationale, the same reasoning trap
already named and retracted once for kurtosis's own design) — the near-zero correlation is the real,
measured rationale that was missing before, not "avoid recalibration effort."

**Separately, and found only because the migration tool was built**: `PositionManager.cpp:2144`'s
`lrc.fractalDim > 1.6f` branch (`fractalForcePassive`, "rough market → passive only") **has fired
zero times across all 19,327 paired 30-bar readings** — the observed maximum is well below `1.6`
(mean 1.29, p90 1.37). This is a live, currently-shipped risk-gate branch that has been effectively
dead code in production, independent of the window-widening question entirely — it would be broken
on the *current* 30-bar window with or without any of this spec's work. **Naive percentile-mapping
makes this worse, not better, if applied blindly**: `(fractal_dim@30 <= 1.6).mean()` is already
`100%`, so quantile-mapping onto the 400-bar distribution maps to `np.percentile(new, 100.0)` —
literally the sample maximum (`1.4393` measured) — a threshold that would *also* never fire, just
under a new number. Confirmed by actually running the migration script, not assumed.

**SUPERSEDED, 2026-08-28 — no hand-derived replacement threshold needed, decided by the user.**
The original plan here was to derive `1.6f`'s replacement from a real percentile of the observed
`fractal_dim@30` distribution (the same domain-grounded exercise Task 7 did for kurtosis/skewness's
gates). That plan is now superseded, not merely deferred: `fractal_dim@30` (the exact `lrc.
fractalDim` value this gate reads, already wire-transmitted via `RiskGateContext.fractal_dim`,
`../schema/mts_schema.fbs:442`, zero new C++/schema work needed) is proposed as a raw feature input
to the soft/gate danger classifier (`PRODUCTION_TRIAGE.md` row 11 — see that row's 2026-08-28
addendum). A **learned** classifier finding the real relationship between raw `fractal_dim@30` and
danger (possibly nonlinear, possibly interacting with other features) is a stronger fix than hand-
picking a single linear cutoff a second time — especially given the first hand-picked cutoff (`1.6f`)
turned out to be wrong. **What this means concretely for `PositionManager.cpp`'s GAP 11 hard gate
specifically**: no new threshold-derivation work is needed as an interim fix. The gate is *already*
non-functional in production (fires zero times) — leaving it inert costs nothing further while the
soft classifier is built, the same "don't retire/replace the old mechanism before its replacement is
proven" discipline this project already applies to Predator Fusion's own C++ retirement (row 10) —
except here the "old mechanism" is already provably not doing anything, so there's no live behavior
at risk either way. Once the soft classifier is live and validated, GAP 11's hard-coded branch
becomes a genuine simplification/retirement candidate, not before. **Not yet done, and no longer
this spec's own action item** — tracked from here via row 11, not this row.

## 5a. Literature-grounded reframe, 2026-08-27: `mean_rev_z`, `hurst_exponent`, and `recurrence_rate`
move to activity-clock windowing, not time-bar widening — `fractal_dim` does NOT, still ungrounded

**Updated same day, second pass**: this section originally treated `recurrence_rate` and
`fractal_dim` identically ("no direct precedent either way"). A user question ("ground your answer
on the literature") prompted a deeper, more targeted search specifically for the adjacent case
(fractal/RQA methods under event-indexed, non-uniform-time sampling, not just finance-specific
tests) — it found real grounding for `recurrence_rate` and confirmed the silence for `fractal_dim`.
They are NOT symmetric; treating them as a pair was the earlier error, corrected below.

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
- **`recurrence_rate` (RQA) -- added in the second pass, 2026-08-27, real cross-domain precedent,
  not finance-specific.** Heart-rate-variability research routinely applies RQA directly to
  beat-to-beat RR-interval sequences -- a large, well-established literature (RQA-detected
  ventilatory thresholds; symbolic RQA on RR intervals for atrial-fibrillation detection). RR
  intervals are indexed by *beat number*, not fixed clock time -- each value is the duration between
  consecutive heartbeats, and RQA's phase-space embedding operates on that sequence directly, no
  resampling to a uniform time grid. That is structurally the same move as computing RQA over a
  sequence of imbalance-bar values indexed by bar count instead of time-bar count: the recurrence-
  plot/embedding machinery is about the *order and value* of a sequence, not the real-time spacing
  between its elements. **This is the same evidentiary standard this project's own Gang doc already
  accepts** -- Sevcik's own formula choice is grounded in Esteller et al. (2001), an EEG comparison
  study, not a finance paper; cross-domain biomedical grounding is not a lower standard here, it's
  the established one.

**`fractal_dim` (Sevcik) -- explicitly separated out, still NOT reframed, second pass confirms the
silence rather than reversing it.** The nearest adjacent literature (multifractal/DFA analysis of
inter-spike-interval sequences in neuroscience) analyzes the *intervals themselves* as the signal
(closer to this spec's own "bar-formation rate" speculative idea than to `fractal_dim`'s actual
job), not a price-like signal *sampled at* event times, which is what `fractal_dim` would need.
Sevcik's formula is, by construction, order-based rather than clock-based (a fixed `dx` per step
regardless of what real interval that step spans) -- the same underlying argument for portability
plausibly applies -- but that is this spec's own inference from reading the formula, not a
literature finding, and is explicitly labeled as such rather than blurred with `recurrence_rate`'s
real citation above. Sevcik is also independently documented as sensitive to the sampling-rate/
time-period of its input (different multipliers result if "the amplitude range and/or time periods
of signals to be compared are not identical") -- meaning even if moved, its resulting values would
need their own `FeatureScaler` recalibration from scratch, not a drop-in continuation. `fractal_dim`
remains a pure window-widening candidate per Section 5b (400 bars, derived), on TS2 time bars,
pending either (a) a future literature finding that actually covers Sevcik/path-length fractal
dimension under event-indexed sampling, or (b) an explicit, flagged engineering decision to extend
the pattern by unsupported analogy anyway -- not silently.

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
- **`recurrence_rate` -- checked directly, has ZERO live gate consumers** (grepped
  `RiskManager.cpp`/`Scoring.cpp`/`PositionManager.cpp`/`TradeDecisionEngine.h`, no hits;
  `ContextManager.cpp:452-460`'s `AreTs2StructuralDimsReady()` only checks the value is finite and
  within its natural `[0,1]` contract range, a data-quality/freshness gate that stays valid
  regardless of source, not a calibrated decision threshold that a source change would disturb) --
  so `recurrence_rate` should follow `skewness_idx`'s **replacement** pattern, not kurtosis's
  additive one. Nothing calibrated to protect.
- **`fractal_dim` remains explicitly NOT reframed** (separate box above) -- do not extend this
  section's treatment to it by assuming symmetry with `recurrence_rate` just because they share a
  window/screen today.
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
- Not deriving the exact widened window size with full rigor for its own sake (Section 5) -- now
  done for `fractal_dim` specifically (Section 5b, 400 bars, measured). `recurrence_rate` and
  `mean_rev_z` both moved to Section 5a's activity-clock design instead of a time-bar target number.
- Not designing `mean_rev_z`/`hurst_exponent`/`recurrence_rate`'s activity-clock twins (`recurrence_rate`
  is a replacement, not a twin, per Section 5a) in implementation detail -- the literature grounding
  and additive-vs-replace framing are decided; the concrete C++ design (field/state shape, gate
  integration) is a future implementation plan's job, same as kurtosis went through its own plan
  before code was written.
- Not fixing `fisher_info`'s scale-collapse/discrimination question or researching whether Clark/AFML
  literature applies to it (Section 6) -- flagged as the right next step, not attempted here.
- Not touching `mts_schema.fbs` -- see companion `schema/` spec. (`mean_rev_z`/`hurst_exponent`'s
  eventual activity-clock twins would need a schema entry when implemented, same as
  `fast_taleb_kurtosis` -- not yet filed, this spec is design-grounding only.)

## 8. Acceptance gates

- `fractal_dim`'s widened window is backed by an explicit autocorrelation-time derivation (Section
  5b, 400 bars, measured) -- done, not the Section 5 proposed-target number taken as final.
- `mean_rev_z`'s `rho` autocorrelation and `recurrence_rate` itself, when their activity-clock
  treatments are implemented (Section 5a), get their own, separately-justified imbalance-bar
  lookback/design, not a carried-over time-bar window by default -- same principle Section 5
  established for the time-bar version, carried forward rather than dropped when the clock changed.
- Existing MindfulTrader unit test coverage for `fractal_dim`
  (`test_indicator_computations.cpp` or equivalent) updated to reflect the new window bound, and a
  regression test confirms the widened computation still respects the "historical-bars-only, never
  reads the live forming bar" contract these adaptive-window functions document for themselves --
  **flagged for direct verification before relying on it**: a 2026-08-27 code read of
  `CalculateMeanReversionSpeed`/`CalculateRecurrenceRate` found both explicitly reference
  `sc.BaseData[SC_LAST][sc.Index]` (the live, still-forming bar) as their current-point term, which
  appears to be in tension with this stated contract -- resolve which is actually true (the contract
  wording, or the current-point behavior) before writing a regression test that assumes either.
- `hurst_exponent`, `mean_rev_z`, and `recurrence_rate` are NOT implemented by this spec (Section 5a
  is a design/literature-grounding decision, not an implementation) -- confirmed via diff review,
  not just stated intent, same discipline as this section already applied to the old Section 6.
- `fisher_info` is NOT touched by this spec's own implementation -- confirmed via diff review.
- Cross-referenced from `lbrnet`'s companion spec and `schema/PENDING_SCHEMA_CHANGES.md` (if either
  needs an entry -- Section 2 concludes neither does for this spec's own scope; a future
  `mean_rev_z`/`hurst_exponent`/`recurrence_rate` activity-clock plan will need its own schema entry
  if any of them go additive, tracked there, not retrofitted into this spec).

## 9. Residual risk

- **CORRECTED, 2026-08-27/28**: widening `fractal_dim`'s window to 400 bars does NOT change
  `PositionManager.cpp`'s live gate's behavior — decided (Section 5b's new subsection): the gate
  stays on its own short window, decoupled from the HMM's 400-bar value, because a real measurement
  (correlation ≈ 0.0115) showed the two windows capture largely independent information, not the
  same signal at different smoothing levels. The residual risk this bullet originally warned about
  (re-validating the gate against the widened distribution) is now moot — there's no shared
  distribution to re-validate against, by design.
- **A separate, independently-discovered risk, found while building the migration tooling for the
  above**: `PositionManager.cpp`'s `fractalDim>1.6f` branch has fired **zero times** in 2.5 years of
  real MES data — broken on the *current* 30-bar window, unrelated to whether it's widened or split.
  Naive percentile-mapping of this threshold is a real trap (maps to the new distribution's sample
  maximum, `1.4393` — also non-functional). **SUPERSEDED, 2026-08-28**: no hand-derived replacement
  threshold is being pursued — `fractal_dim@30` is instead proposed as a raw feature into the soft/
  gate danger classifier (`PRODUCTION_TRIAGE.md` row 11), where a learned model finds the real
  relationship instead of a second hand-picked cutoff. The gate stays inert (already non-functional,
  so no live behavior is at risk) until the classifier is live and validated, then becomes a
  retirement/simplification candidate — same discipline as row 10's Predator Fusion retirement.
- `fractal_dim`'s window derivation is now done (Section 5b, `400` bars, measured not guessed) --
  the ~150-bar figure this bullet used to warn against is superseded, not still a live risk.
  Residual risk now is implementation-stage only: `fractal_dim`'s missing native test coverage, and
  decoupling its window constant from `recurrence_rate`'s now that they may diverge (both named in
  Section 5b's own "Not yet done" list).
- `recurrence_rate` moving to activity-clock is a *second-pass* finding (real cross-domain grounding,
  RQA on event-indexed RR-interval sequences) -- unlike `mean_rev_z`/`hurst_exponent`, it has ZERO
  live gate consumers to protect (checked directly, Section 5a), so it's a `skewness_idx`-style
  **replacement** candidate, not an additive twin -- don't default to the additive pattern just
  because `mean_rev_z`/`hurst_exponent` used it.
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
