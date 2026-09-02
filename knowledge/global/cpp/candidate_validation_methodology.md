---
domain: cpp/hmm_observation_vector
intent: how do we validate a new HMM observation-vector candidate dimension offline, cheaply, before committing to a schema field and C++ production wiring?
scope: global
tags: [hmm, observation-vector, bootstrap, arrow, parquet, validation, fat-tail, median, mad, model-independent]
source_files:
  - tools/observation_vector/market_data_io.h
  - tools/observation_vector/market_test_stats.h
  - tools/observation_vector/drift_location_eval.cpp
  - tools/observation_vector/jump_ratio_eval.cpp
last_verified: 2026-08-30
dependencies: []
---

# Model-independent offline candidate validation (drift_location_eval / jump_ratio_eval pattern)

## Why This Exists

Adding a new dimension to the HMM observation vector is expensive to get wrong: it requires a
`../schema/mts_schema.fbs` field addition, C++ production wiring, `lbrnet` consumer updates, and a
full retrain before any evidence of whether the dimension is actually useful exists. Two real
candidates (drift/location, jump/bipower-variation ratio) were validated entirely offline in native
C++/Arrow against real MES tick data *before* touching the schema — one was rejected (drift/location,
hit_rate below 0.5 at every horizon), one survived with a large, real effect (jump/bipower-variation
ratio). Neither cost a schema change, an `lbrnet` update, or a retrain to find out. This is now the
standing methodology for any future candidate in this family, not a one-off script per candidate.

**Critically: do NOT validate against `hmm_model.pkl`'s own state decode (cross-state-ratio).** That
model was trained on data with multiple confirmed invalidities (bar-gated dims frozen mid-bar, a
`FeatureScaler` dedupe-corruption bug, an index-shift miscalibration from a mid-vector field
insertion) — its state labels are not trustworthy ground truth right now. Test against **real forward
market returns** instead; this makes the test model-independent by construction.

## The Invariant / Contract

**Two test shapes, chosen by whether the candidate is signed or non-negative — do not default to one
without checking:**

- **Directional/hit-rate test** (`ComputeHitRate` in `market_test_stats.h`) — for a *signed*
  candidate (e.g. a return z-score). Tests same-sign (continuation) or negated-sign (reversion)
  agreement between the candidate's sign and the forward return's sign. Wilson CI, z-test vs. null
  0.5, Bonferroni-corrected across horizons.
- **Magnitude/bootstrap-gap test** (`ComputeBootstrapMeanGapCI` / `ComputeBootstrapMedianGapCI`) —
  for a *non-negative* candidate (e.g. a variance ratio) that has no sign to test. Splits into
  top/bottom decile by candidate value, tests whether the two groups' forward-return magnitude
  differs. No Bonferroni correction (matches the Python reference this was ported from).

**Use `median(|forward_return|)`/MAD, not `mean(|forward_return|)`, for the magnitude test.** This
codebase's own established convention for fat-tailed data is median + MAD × 1.4826
(`FeatureScaler.h`'s `RobustLocation()`, "Taleb-consistent") — not the mean, which Kim & White (2004)
show is least reliable exactly under the fat-tailed conditions this system's candidates are meant to
capture. This codebase already made this exact correction once for skewness/kurtosis (Bowley/Moors
robust quantile estimators replacing moment-based statistics, 2026-08-13) — using `mean(|x|)` for a
new candidate's validation repeats the same mistake. `ComputeBootstrapMeanGapCI` still exists (kept
for parity with the Python reference script) but should not be a new candidate's default choice.

**Known, not-yet-fixed limitation**: both bootstrap functions and the hit-rate test resample
individual per-tick forward-return signals as if independent. Real forward returns overlap heavily
(a 240-minute forward return spans thousands of adjacent per-tick signals) — this understates every
reported CI/p-value's width by an unquantified, plausibly large factor. Doesn't invalidate either of
the two real verdicts reached so far (both are far from a naively-calibrated significance boundary),
but a genuinely marginal future candidate could be misjudged by it. A real fix (block bootstrap)
should reuse this codebase's own measured Politis-White block-length precedent (`fractal_dim`,
≈404.82 real MES ticks, `72ab967`) rather than re-derive one, and should be applied once, uniformly,
not patched into one candidate's test ad hoc.

## How It Works

Shared infrastructure, extracted on the second real use (`market_data_io.h`/`market_test_stats.h`
pulled out of `drift_location_eval.cpp`/`drift_location_stats.h` when `jump_ratio_eval.cpp` needed
the same pieces):

- `tools/observation_vector/market_data_io.h` — `TickSeries ReadTicksParquet(path)`: direct Arrow Parquet read (this
  codebase's first C++ tool to read Parquet — every prior tool only writes). **Must use column
  projection** (`Schema::GetFieldIndex`, never hardcoded indices) — an unprojected `ReadTable()`
  reading all columns of the real 38.5M-row MES tick file took 5+ minutes; projected to the 2 needed
  columns, 7.5s.
- `tools/observation_vector/market_test_stats.h` — `ComputeForwardReturns(signal_ts, signal_price, timestamps, prices,
  horizon_minutes)`: the forward-return computation shared by every test. **Must be a single-pass
  two-pointer merge, not per-signal `std::lower_bound`** — both series are monotonically sorted
  (verify with a real `pl.Series.is_sorted()` check, don't assume), so binary search per signal
  cache-thrashes a large sorted array; a version doing this was measured hanging 100+ seconds across
  4 horizons on the real file. Also holds `ComputeWilsonCI`, `ComputeHitRate`,
  `ComputeBootstrapMeanGapCI`, `ComputeBootstrapMedianGapCI`.
- Candidate-specific math (e.g. `tools/observation_vector/jump_ratio_stats.h`'s `ComputeJumpRatio`) stays in its own
  header — only the generic test/IO infrastructure is shared.
- The CLI tool (`tools/observation_vector/drift_location_eval.cpp`, `tools/observation_vector/jump_ratio_eval.cpp`) wires candidate math +
  shared IO + shared test, reports per-horizon results, optionally writes a JSON report.

**Bootstrap performance at real scale (~3.5-3.9M elements per decile group)**: the exact multinomial
resample (draw n indices with replacement) is **memory-latency-bound**, not throughput-bound — each
random gather into a multi-million-element array that doesn't fit L2/L3 measured at ~95ns, meaning
`n_boot=2000` costs tens of minutes per horizon. Above a 20,000-element threshold, both bootstrap
functions switch to a weighted/"exchangeable" bootstrap (Praestgaard & Wellner 1993): assign every
element an i.i.d. weight and sum in one **sequential** pass (memory-bandwidth-bound, no random
gather) instead of resampling indices. **The weight distribution's variance must equal ~1** (matching
the multinomial resample's implied per-element variance) for this to be correctly calibrated — a
first attempt using `Uniform[0,2]` weights (variance 1/3, chosen only because it was cheap to
generate) produced CIs measured ~1.7-1.8x too narrow before being replaced with `Exponential(1)`
(Rubin 1981's Bayesian-bootstrap weight, variance exactly 1) — which is also faster.
`std::poisson_distribution` (also variance-1, Chamandy et al. 2012) was tried first and rejected:
the library implementation itself measured ~73ns/draw, nearly as slow as the memory gather it was
meant to replace.

```cpp
// Typical call site (see tools/observation_vector/jump_ratio_eval.cpp for the full pattern)
auto fwd = ComputeForwardReturns(signal_ts, signal_price, series.timestamp_us, series.close, horizon_minutes);
auto result = ComputeBootstrapMedianGapCI(top_decile_fwd, bottom_decile_fwd, /*n_boot=*/1000);
bool survives = (result.ci_lo > 0.0) || (result.ci_hi < 0.0);
```

## Failure Modes

- **Validating against `hmm_model.pkl`'s state decode instead of real forward returns** — the model's
  training data has confirmed invalidities; its state labels are not ground truth right now.
- **Using `mean(|x|)` for a magnitude test on fat-tailed data** — matches an external Python script's
  precedent but not this codebase's own, later, more rigorous convention (median/MAD). Repeats a
  mistake this codebase already fixed once.
- **Choosing an "exchangeable bootstrap" weight distribution for speed without checking its
  variance** — any i.i.d., mean-1 weight distribution will *run*, but only a variance-1 one is
  correctly calibrated; a wrong-variance choice fails silently (produces a plausible-looking but
  systematically wrong CI, not a crash or an obviously-wrong number).
- **Assuming a small-n benchmark generalizes to real production scale** — the exact-resample path
  (small n) and the weighted-bootstrap path (large n) have completely different performance profiles
  (memory-latency-bound gather vs. memory-bandwidth-bound sequential scan); a fast small-n test run
  says nothing about real-scale cost. Always benchmark at the real group size before launching an
  expensive end-to-end run against the full production file, and re-benchmark after any change to
  the bootstrap's inner loop — a first "fix" to this exact code accidentally amortized a one-time
  sort over too few resamples and reported a benchmark ~1.8x worse than the real steady-state cost.
- **Trusting a bootstrap CI's width at face value when the underlying signals overlap heavily** — see
  the not-yet-fixed limitation above. Not wrong for a decisive result, dangerous for a marginal one.

## References

- `docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md` §10.7 (drift
  /location tool), §10.8 (jump-ratio tool + the mean→median correction), §10.9 (the i.i.d./overlap
  limitation), §5.0 (rejected result), §5.1 (survives result), §9 (per-dim decision ledger).
- `lbrnet/tools/dim_acceptance_eval.py` — the original Python reference these C++ tools port from
  (Wilson CI, hit-rate, and mean-based bootstrap-gap formulas mirror it exactly; the median-based
  bootstrap-gap function is new, native-only infrastructure with no Python counterpart).
- `include/FeatureScaler.h`'s `RobustLocation()` — this codebase's median/MAD convention.
- Barndorff-Nielsen & Shephard (2004, 2006); Praestgaard & Wellner (1993); Rubin (1981); Kim & White
  (2004); Chamandy, Muralidharan, Najmi & Naidu (2012).
