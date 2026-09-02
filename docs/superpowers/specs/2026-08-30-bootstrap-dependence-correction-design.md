# Bootstrap/Hit-Rate Dependence Correction — Design

**Status: DESIGN, awaiting user review before writing-plans.**

## 0. Problem

`tools/observation_vector/market_test_stats.h`'s `ComputeHitRate` (closed-form Wilson CI on a hit/miss proportion) and
`ComputeBootstrapMedianGapCI` (percentile bootstrap on a median gap) both treat individual per-tick
forward-return signals as i.i.d. when computing a confidence interval. Real forward returns overlap
heavily: a 240-minute forward return computed per-tick over a 38.5M-row series shares most of its
underlying tick data with thousands of adjacent signals. Both functions' independence assumption is
false for their real callers (`tools/observation_vector/drift_location_eval.cpp`, `tools/observation_vector/jump_ratio_eval.cpp`), which
understates every reported CI/p-value's width. Neither existing verdict (drift/location rejected,
jump/bipower-variation ratio survives) is expected to flip — both are far from a naively-calibrated
significance boundary — but a future, more marginal candidate validated on this same apparatus could
be misjudged.

Full background: `docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md`
§10.9.

**Scope note**: `ComputeBootstrapMeanGapCI` (the third function originally in scope) was deleted
during this design's own brainstorming (2026-08-30) — zero real callers, and mean(|x|) is the wrong
default for this codebase's fat-tailed data (Kim & White 2004; this repo's own established median/MAD
convention). This spec covers only `ComputeHitRate` and `ComputeBootstrapMedianGapCI`.

## 1. Literature grounding (established during brainstorming, verified via web search, not asserted from memory)

- **Hansen & Hodrick (1980)** and the subsequent overlapping-returns literature: horizon-induced
  overlap creates a deterministic moving-average-type dependence structure in the resampled series —
  this is the mechanism, not just an empirical curiosity.
- **Politis & White (2004)**, corrected by **Patton, Politis & White (2009)**: automatic, data-driven
  block-length selection for dependent bootstraps — already this codebase's own precedent algorithm
  (`arch.bootstrap.optimal_block_length`, used for `fractal_dim`'s window-widening, commit `72ab967`).
  The prescribed practice is to calibrate from the actual series under test, not from a hand-derived
  formula.
- **Ledoit & Wolf (2008), "Robust performance hypothesis testing with the Sharpe ratio"**: the direct
  institutional precedent for financial-return hypothesis testing under dependence — a circular block
  bootstrap with block length from automatic calibration on the real return series.
- **Kish (1965)** design effect and **Moulton (1990)**: the standard survey-statistics/econometrics
  correction for a naively-computed variance under clustering or serial correlation — inflate by a
  measured design effect (DEFF) rather than assume independence.
- **Newey & West (1987)** long-run variance (HAC): `arch.covariance.kernel.NeweyWest` exposes this
  directly as public API (`cov.long_run`, `cov.short_run`) — `long_run / short_run` **is** the design
  effect for a sample mean under dependence, computed via the exact same already-adopted library used
  for `optimal_block_length`.

## 2. Why the fat-tail correction matters, and how it changes the design (found via direct user challenge, not initially considered)

Newey-West's long-run/short-run variance ratio is the correct DEFF for a **sample mean**'s asymptotic
variance under dependence. `ComputeHitRate`'s hit-rate is itself a mean of a bounded {0,1} indicator
— directly compatible. But `ComputeBootstrapMedianGapCI` tests a **median**, whose asymptotic
variance under dependence has a different formula (density-at-the-median-based, not a function of the
raw values' variance at all). Applying a raw-value DEFF to a median-based CI would be internally
inconsistent: a fat-tail-robust point estimate whose uncertainty is corrected using a fat-tail-
*sensitive* moment quantity — the same category of error as using `mean(|x|)` for a fat-tailed
magnitude test, one layer deeper.

**Resolution**: derive DEFF for `ComputeBootstrapMedianGapCI` from Newey-West applied to the
**below-median indicator series** (`1` if an observation is at or below its group's sample median,
else `0`) rather than to the raw `|forward_return|` values — the standard way quantile standard
errors are corrected for dependence (transform to an indicator, then apply the same long-run-variance
machinery used for proportions). This has a second, direct benefit for this specific codebase: an
indicator series is bounded `{0,1}` by construction, so estimating its long-run variance is **immune
to the underlying return series' tail heaviness** — the same robustness principle that motivated
median/MAD in the first place also makes its own dependence-correction well-behaved. Both DEFF
derivations in this design (hit/miss for `ComputeHitRate`, below-median for
`ComputeBootstrapMedianGapCI`) are therefore indicator-based, bounded, and fat-tail-safe. No mean-
based raw-value DEFF appears anywhere in this design.

## 3. Chosen approach: DEFF variance inflation (not a block-bootstrap rebuild)

Three approaches were considered:
- **A — full circular block-bootstrap rebuild** (most textbook-complete, but requires a new,
  time-order-preserving resampling mechanic for both functions — the existing large-n path in
  `ComputeBootstrapMedianGapCI` sorts by value first, which is fundamentally incompatible with block
  structure; `ComputeHitRate` would need to become a resampling function for the first time). Highest
  risk given this session's own recent history: two prior resampling-algorithm rewrites in this same
  file each took multiple review rounds to get right (mean-bootstrap's weight distribution, then
  median-bootstrap's own).
- **B — DEFF variance inflation (chosen)**: leave every existing resampling/computation mechanic
  completely untouched; derive a design effect per candidate+horizon from the real signal data
  (offline, Python, reusing `arch`), and widen the already-computed CI around the already-computed
  point estimate by `sqrt(DEFF)`. Institutionally correct (Kish/Moulton, Newey-West are standard, not
  a shortcut), minimally invasive, and does not gamble already-shipped, already-validated candidate
  results on a new resampling algorithm.
- **C — subsampling (Politis, Romano & Wolf 1999)**: a legitimate alternative, but not what the most
  directly-on-point precedent found (Ledoit & Wolf 2008) actually recommends for this exact situation.

**Decision: B.**

## 4. Design

### 4.1 Offline calibration tool: `tools/observation_vector/block_length_and_variance_inflation.py`

Mirrors `tools/observation_vector/window_autocorrelation_diagnostic.py`'s style and conventions (same `mts` env, same
`arch` dependency, same "derive once from real data, record the number" pattern as `fractal_dim`'s
≈404.82). For each candidate (`drift_location`, `jump_ratio`) × horizon (30/60/120/240 min):

- Reconstructs the real indicator series the C++ tool actually resamples:
  - `drift_location` (`ComputeHitRate`): the hit/miss sequence (`1` if `Sign(forward_return) ==
    Sign(candidate_value)`, else `0`), in original chronological order, over the same signal set the
    real `tools/observation_vector/drift_location_eval.cpp` run used.
  - `jump_ratio` (`ComputeBootstrapMedianGapCI`): for each of the top/bottom decile groups
    separately, the below-median indicator sequence (`1` if `|forward_return| <= group median`, else
    `0`), in original chronological order.
- Computes `arch.bootstrap.optimal_block_length(x)` on that indicator series — recorded for
  documentation/citation only (matching the `fractal_dim` precedent's own reporting convention), not
  consumed directly by the C++ correction.
- Computes `arch.covariance.kernel.NeweyWest(x).cov` and derives `DEFF = long_run / short_run`. For
  `jump_ratio`'s two decile groups, `DEFF` is the simple average of the top and bottom groups'
  individually-derived values (both groups are comparable size and share the same horizon-driven
  overlap mechanism — noted here as a deliberate simplification, not a rigorous variance-weighted
  combination, since the two groups' sample sizes are close enough in practice that the difference is
  immaterial).
- Output: a small table (`sqrt(DEFF)` per candidate × horizon) printed and saved, to be hand-embedded
  into the C++ tools as named constants with a comment citing this script — same convention as every
  other calibrated constant in this codebase (e.g. `fractal_dim`'s window width, the Amihud/
  liq_fragility winsorization bounds).

### 4.2 `ComputeHitRate` — effective-sample-size substitution

Add one parameter, defaulting to exact backward compatibility:

```cpp
inline HitRateResult ComputeHitRate(
    const std::vector<double>& forward_returns, const std::vector<double>& candidate_values,
    double variance_inflation = 1.0) {
    // ... existing n, k counting loop, unchanged ...
    result.n = n;
    result.k = k;
    if (n == 0) return result;
    result.hit_rate = static_cast<double>(k) / static_cast<double>(n);

    // Effective-sample-size substitution: n_eff = n / DEFF, k_eff = hit_rate * n_eff. At
    // variance_inflation=1.0, n_eff == n and k_eff == k exactly (bit-identical to pre-correction
    // behavior) -- every downstream formula (se_null, z_stat, p_value, Wilson CI) is fed n_eff/k_eff
    // instead of n/k, so the CI and p-value are corrected consistently from one shared substitution
    // rather than needing separate ad hoc adjustments that could disagree with each other.
    const double n_eff = static_cast<double>(n) / variance_inflation;
    const double k_eff = result.hit_rate * n_eff;
    const double se_null = std::sqrt(0.25 / n_eff);
    result.z_stat = (result.hit_rate - 0.5) / se_null;
    result.p_value = 2.0 * (1.0 - 0.5 * (1.0 + std::erf(std::fabs(result.z_stat) / std::sqrt(2.0))));
    const auto ci = ComputeWilsonCI(k_eff, n_eff);  // signature widened size_t -> double, see 4.4
    result.ci_lo = ci.lo;
    result.ci_hi = ci.hi;
    return result;
}
```

`result.n`/`result.k` keep reporting the **raw** observed counts (what was actually measured) for
transparency — only the internal `n_eff`/`k_eff` used for the statistical formulas are adjusted.

### 4.3 `ComputeBootstrapMedianGapCI` — post-hoc CI widening around the point estimate

Add one parameter, defaulting to exact backward compatibility. No change to the resampling loop
itself — this is purely a widening of the already-computed percentile CI:

```cpp
inline MedianGapResult ComputeBootstrapMedianGapCI(
    const std::vector<double>& top, const std::vector<double>& bottom,
    std::size_t n_boot = 1000, std::uint64_t seed = 0, double variance_inflation = 1.0) {
    // ... entire existing body unchanged, through computing result.gap/ci_lo/ci_hi as today ...

    // Widen each side around the point estimate independently (preserves any asymmetry the bootstrap
    // distribution itself has -- does not assume a symmetric/normal-shaped CI). At
    // variance_inflation=1.0 this is the identity transform (bit-identical to pre-correction output).
    const double scale = std::sqrt(variance_inflation);
    result.ci_lo = result.gap - (result.gap - result.ci_lo) * scale;
    result.ci_hi = result.gap + (result.ci_hi - result.gap) * scale;
    return result;
}
```

### 4.4 `ComputeWilsonCI` signature widening

Current signature takes `std::size_t k, std::size_t n` but the function body immediately casts both
to `double` — the integer constraint is not functionally load-bearing. Widen to
`ComputeWilsonCI(double k, double n, double z = 1.96)`. Existing integer callers convert implicitly
and losslessly (realistic sample sizes are far below the point where `size_t -> double` loses
precision); this is a safe, minimal generalization needed to accept a non-integer `n_eff`.

### 4.5 Rollout

`drift_location_eval.cpp` and `jump_ratio_eval.cpp` each gain a small per-horizon
`variance_inflation` lookup (from 4.1's script output), passed into their existing calls with a
comment citing the calibration script and the derived numbers. Both tools are re-run against the
real production files; the new, correctly-calibrated CIs are recorded as the final authoritative
numbers in the brainstorm doc (§5.0, §5.1, §9) and `SCRATCHPAD.md`, alongside an explicit note that
neither verdict changed.

## 5. Testing

- **Backward compatibility**: every existing test in `test_drift_location_stats.cpp` (26 checks,
  exercises `ComputeHitRate`/`ComputeWilsonCI`) and `test_market_test_stats_median.cpp` (9 checks,
  exercises `ComputeBootstrapMedianGapCI`) must pass **unchanged** with the new parameter defaulting
  to `1.0` — proves the correction is additive, not a rewrite.
- **Known-factor widening**: a new test calling each corrected function with an explicit
  `variance_inflation` (e.g. `4.0`) and asserting the resulting CI half-width scales by exactly
  `sqrt(4.0) = 2.0` relative to the `variance_inflation=1.0` call on identical input data.
- **`ComputeHitRate` CI/p-value consistency**: a test asserting that under a nontrivial
  `variance_inflation`, the corrected CI excluding 0.5 and the corrected p-value being significant
  agree (both derived from the same `n_eff`/`k_eff`, so this should hold by construction — the test
  guards against a future edit breaking that shared derivation).
- **Calibration script**: no formal unit test (this mirrors `window_autocorrelation_diagnostic.py`,
  which also has none) — validated by running against real data and sanity-checking the printed
  `DEFF`/block-length numbers are finite, `DEFF >= 1.0` (dependence can only inflate variance
  relative to the naive i.i.d. estimate, never deflate it below 1 for positively-autocorrelated
  overlap), and roughly consistent in magnitude with the horizon-driven overlap intuition (longer
  horizons -> more overlap -> larger DEFF).
- **Real end-to-end re-verification**: both `drift_location_eval` and `jump_ratio_eval` re-run
  against the full production files with the derived `variance_inflation` values; confirm neither
  verdict flips and record the new numbers.

## 6. Out of scope

- A full block-bootstrap rebuild (Approach A) — not chosen; noted for a future initiative if DEFF
  inflation is later found insufficient.
- Any change to `ComputeJumpRatio`, `ComputeLogReturns`, `ComputeForwardReturns`, or any other
  function not named above.
- Re-deriving `fractal_dim`'s own block length or touching its consumers — unrelated.
