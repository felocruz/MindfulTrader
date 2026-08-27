# Spec: Observation-Vector Estimator Hardening -- Dead-Code Candidates and Window Widening

**Status**: Drafted 2026-08-25, companion to `lbrnet/docs/superpowers/specs/2026-08-25-vol-
convexity-removal-spec.md` (which this spec is downstream of -- read that one first for the full
evidence chain). **Not yet implemented.** Grounded in a literature search (Student-t HMM / fat-tail
regime-switching literature) plus a direct window/timeframe audit of every candidate feature's
actual C++ computation, run against the real source in this repo, not assumed from the Python side.

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

**These are proposed targets for discussion, not finalized magic numbers** -- matching this
project's own standing rule against inventing round numbers without derivation (`CLAUDE.md`'s
"ungrounded utility constants" pattern). The ~150/~600 figures are sized to match the regime-tenure
reference at the same order of magnitude; a rigorous derivation (e.g. an autocorrelation-time
diagnostic on the raw signal itself, the same technique this project already used for the HMM's own
calibration holdout sizing, `docs/superpowers/specs/2026-08-24-hmm-gate-threshold-calibration-
institutional-grade-spec.md`) should confirm or adjust them before implementation, not skip
straight from "current window is too short" to "here is the exact new number."

## 6. Explicitly NOT a window-widening case: `hurst_exponent`, `fisher_info`

Both already run on windows comparable to or exceeding the ~6.1-day regime-tenure reference
(`hurst_exponent`: 8.3-33.3 days; `fisher_info`: 5-20 days) -- widening further has no clear
justification from the sample-size argument that motivates Section 5. Yet both rank at or near the
bottom of the HMM's cross-state discrimination ranking (`hurst_exponent` exactly 0.0000, the worst
of all 16 dims; `fisher_info` 0.0011, third-worst). **This project's own prior audit
(`lbrnet/docs/superpowers/specs/2026-08-14-observation-vector-full-institutional-coverage-spec.md`
row 6) already found `hurst_exponent` has the worst scale-collapse data-quality artifact of all 16
dims (24.85% pre-shrinkage `|z|>=6` rate) -- a distortion of that severity plausibly explains
near-zero discrimination on its own, independent of window length.** `fisher_info` has not yet had
the equivalent tail-conditional noise decomposition run against it.

**Recommended next step, not part of this spec's own implementation scope**: run
`fisher_info` through the same tail-conditional noise-decomposition technique already validated for
`hurst_exponent` (`hmm_feature_selection.md`'s Phase 4 audit) before deciding whether it needs a
data-quality fix, a drop, or is genuinely fine as-is. Do not widen its window as a first move --
that would consume implementation effort without addressing the more likely root cause.

## 7. Non-goals

- Not removing `skewness_idx`/`micro_asymmetry`'s C++ computation in this spec (Section 3) --
  timing is MindfulTrader's own call, tracked but not executed here.
- Not touching `vol_convexity`/`tail_index` C++ computation at all (Section 2).
- Not deriving the exact widened window sizes with full rigor (Section 5) -- proposed targets only,
  pending an autocorrelation-time diagnostic.
- Not fixing `hurst_exponent`'s scale-collapse artifact or investigating `fisher_info`'s (Section
  6) -- flagged as the right next step, not attempted here.
- Not touching `mts_schema.fbs` -- see companion `schema/` spec.

## 8. Acceptance gates

- Widened-window changes for `recurrence_rate`/`fractal_dim`/`mean_rev_z` are backed by an explicit
  autocorrelation-time or equivalent derivation for the final window size, not the Section 5
  proposed-target numbers taken as final without that check.
- `mean_rev_z`'s `rho` autocorrelation gets its own, separately-justified lookback, not the outer
  z-score's window by default.
- Existing MindfulTrader unit test coverage for these functions (`test_indicator_computations.cpp`
  or equivalent) updated to reflect new window bounds, and a regression test confirms the widened
  computation still respects the "historical-bars-only, never reads the live forming bar" contract
  these adaptive-window functions already document for themselves.
- `hurst_exponent`/`fisher_info` are NOT touched by this spec's own implementation -- confirmed via
  diff review, not just stated intent.
- Cross-referenced from `lbrnet`'s companion spec and `schema/PENDING_SCHEMA_CHANGES.md` (if either
  needs an entry -- Section 2 concludes neither does for this spec's own scope).

## 9. Residual risk

- Widening `recurrence_rate`/`fractal_dim`/`mean_rev_z`'s windows changes their live, currently-
  transmitted values for every consumer, not just the HMM training path -- confirm no other live
  C++ consumer (routing, sizing, display subgraphs) depends on the *current* short-window behavior
  before widening, the same class of check Section 3 already applies to the dead-code candidates.
- This spec's own proposed window sizes (Section 5) are order-of-magnitude estimates pending a real
  derivation -- do not treat ~150/~600 bars as final without that follow-up.
