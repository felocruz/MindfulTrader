# Bootstrap/Hit-Rate Dependence Correction Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix `tools/observation_vector/market_test_stats.h`'s `ComputeHitRate` and `ComputeBootstrapMedianGapCI` so their reported confidence intervals account for the real serial dependence in overlapping per-tick forward-return signals, instead of assuming independence.

**Architecture:** Add one optional `variance_inflation` parameter (default `1.0`, exact backward compatibility) to each function. `ComputeHitRate` uses an effective-sample-size substitution (`n_eff = n / variance_inflation`) feeding its existing formulas unchanged; `ComputeBootstrapMedianGapCI` widens its already-computed percentile CI around the unchanged point estimate by `sqrt(variance_inflation)`. Neither function's resampling/counting logic is touched. The actual `variance_inflation` (design effect, DEFF) values are derived offline via a new Python script using `arch.covariance.kernel.NeweyWest`'s long-run/short-run variance ratio, computed on bounded {0,1} indicator series exported by the two CLI tools — fat-tail-safe by construction (an indicator series is bounded regardless of how heavy the underlying return distribution's tails are).

**Tech Stack:** C++17 (existing `market_test_stats.h`/`drift_location_eval.cpp`/`jump_ratio_eval.cpp`), Python 3 via `mamba run -n mts` (new calibration script, using the already-installed `arch` package).

**Spec:** `docs/superpowers/specs/2026-08-30-bootstrap-dependence-correction-design.md`

## Global Constraints

- **RPATH required on every Arrow/Parquet-linked build**: `-Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib`.
- **`mamba run -n mts`** for every Python and C++ toolchain invocation — no bare `python3`/`g++`.
- **Never touches `build_dll.sh`/`CMakeLists.txt`** — standalone tools under `tools/`.
- **Backward compatibility is a hard requirement, not a nice-to-have**: every existing check in `tools/observation_vector/test_drift_location_stats.cpp` (26 checks) and `tools/observation_vector/test_market_test_stats_median.cpp` (9 checks) must pass **unchanged** after the C++ signature changes, since the new parameter defaults to `1.0`.
- **Real end-to-end verification against the actual production file is required before the rollout task (Task 7) is done** — `lbrnet/data/raw/mes_continuous_ticks.parquet`, 38,547,467 rows. Read the actual printed numbers and sanity-check them (finite, `DEFF >= 1.0`, CI still ordered); a clean exit code is not sufficient. This codebase has been burned multiple times this session by trusting a benchmark or an estimate instead of a real production-scale run.
- **Reference values verified against real Python execution, not hand arithmetic** — every numeric test value below was computed via a real `mamba run -n mts python3` invocation before being written into this plan (shown inline).
- **No placeholder DEFF numbers get committed as final.** Task 6 produces real numbers from a real run; Task 7 consumes those exact numbers, not estimates.

---

## Task 1: Widen `ComputeWilsonCI`'s signature to accept real-valued `k`/`n`

**Files:**
- Modify: `tools/observation_vector/market_test_stats.h:85`

**Interfaces:**
- Produces: `WilsonInterval ComputeWilsonCI(double k, double n, double z = 1.96)` (was `std::size_t k, std::size_t n`) — needed because Task 2's effective-sample-size correction produces a non-integer `n_eff`.

This is a signature-only change (the function body already casts both parameters to `double` internally — the `std::size_t` constraint was never functionally load-bearing). No new test needed; the existing 4 checks in `tools/observation_vector/test_drift_location_stats.cpp` that call `ComputeWilsonCI(50, 100)`/`ComputeWilsonCI(70, 100)` with integer literals are the regression check (integer literals convert to `double` implicitly and losslessly).

- [ ] **Step 1: Run the existing test suite to confirm the baseline passes**

Run: `mamba run -n mts g++ -std=c++17 tools/observation_vector/test_drift_location_stats.cpp -o /tmp/t_drift && /tmp/t_drift`
Expected: `ALL PASS` (26 checks)

- [ ] **Step 2: Widen the signature**

In `tools/observation_vector/market_test_stats.h`, change:

```cpp
inline WilsonInterval ComputeWilsonCI(std::size_t k, std::size_t n, double z = 1.96) {
```

to:

```cpp
inline WilsonInterval ComputeWilsonCI(double k, double n, double z = 1.96) {
```

The function body (lines 86-91) needs no changes — it already does `static_cast<double>(n)`/`static_cast<double>(k)` on every use, which becomes redundant-but-harmless once the parameters are already `double`. Leave those casts in place (removing them is unrelated cleanup, out of scope for this task).

- [ ] **Step 3: Rebuild and re-run to confirm zero regression**

Run: `mamba run -n mts g++ -std=c++17 tools/observation_vector/test_drift_location_stats.cpp -o /tmp/t_drift && /tmp/t_drift`
Expected: `ALL PASS` (26 checks) — identical output to Step 1, proving the widening is behavior-preserving.

- [ ] **Step 4: Commit**

```bash
git add tools/observation_vector/market_test_stats.h
git commit -m "refactor: widen ComputeWilsonCI to accept real-valued k/n

Needed for the upcoming effective-sample-size dependence correction
(ComputeHitRate), which produces a non-integer n_eff. The function body
already computed in double precision internally -- the std::size_t
parameter types were never functionally load-bearing. Zero behavior
change: existing integer-literal callers convert implicitly and
losslessly; all 26 checks in test_drift_location_stats.cpp still pass."
```

---

## Task 2: `ComputeHitRate` gains `variance_inflation` via effective-sample-size substitution

**Files:**
- Modify: `tools/observation_vector/market_test_stats.h:120-145`
- Test: `tools/observation_vector/test_drift_location_stats.cpp` (add new checks; existing 26 must still pass)

**Interfaces:**
- Consumes: `ComputeWilsonCI(double k, double n, double z = 1.96)` from Task 1.
- Produces: `HitRateResult ComputeHitRate(const std::vector<double>& forward_returns, const std::vector<double>& candidate_values, double variance_inflation = 1.0)`. `result.n`/`result.k` still report the **raw** observed counts (unchanged meaning); only the internal statistical formulas use the effective, DEFF-adjusted sample size.

- [ ] **Step 1: Write the failing tests**

Append to `tools/observation_vector/test_drift_location_stats.cpp`, right after the existing hit-rate test block (after the `check("zero candidate values excluded from n", ...)` block, before the final `std::printf(g_failures == 0 ...)` line):

```cpp
    {
        // variance_inflation default (1.0) must reproduce the existing
        // 70/100 fixture's exact values -- proves the new parameter is
        // additive, not a behavior change, when unset.
        std::vector<double> forward_returns(100, 1.0);
        std::vector<double> candidate_values(100, 1.0);
        for (int i = 0; i < 30; ++i) candidate_values[i] = -1.0;
        auto baseline = ComputeHitRate(forward_returns, candidate_values);
        auto explicit_one = ComputeHitRate(forward_returns, candidate_values, /*variance_inflation=*/1.0);
        check("variance_inflation=1.0 matches the no-argument default exactly",
              close(explicit_one.z_stat, baseline.z_stat, 1e-12) &&
              close(explicit_one.p_value, baseline.p_value, 1e-12) &&
              close(explicit_one.ci_lo, baseline.ci_lo, 1e-12) &&
              close(explicit_one.ci_hi, baseline.ci_hi, 1e-12));
    }
    {
        // Effective-sample-size substitution, verified against a real
        // mamba run -n mts python3 computation (not hand arithmetic):
        // n=10000, k=5200 (hit_rate=0.52), variance_inflation=4.0 ->
        // n_eff=2500, k_eff=1300.
        //   baseline (DEFF=1.0): z=4.000000000000004 p=6.334248366623996e-05
        //     ci=(0.510202040212211, 0.529782599288679)
        //   DEFF=4.0: z=2.000000000000002 p=4.550026389635820e-02
        //     ci=(0.500400006272198, 0.539538622433388)
        // z_stat scales by EXACTLY 1/sqrt(DEFF) (pure algebraic
        // substitution into se_null). The Wilson CI width does NOT scale
        // by exactly sqrt(DEFF) -- Wilson's own small-sample nonlinear
        // correction terms mean the ratio is only asymptotically exact,
        // converging to it as n_eff grows (verified: 1.9147 at n_eff=25,
        // 1.9991 at n_eff=2500 -- checked via real Python before choosing
        // n=10000 for this test specifically so the approximation is tight).
        std::vector<double> forward_returns(10000, 1.0);
        std::vector<double> candidate_values(10000, 1.0);
        for (int i = 0; i < 4800; ++i) candidate_values[i] = -1.0;  // 5200 hits / 10000
        auto baseline = ComputeHitRate(forward_returns, candidate_values);
        auto corrected = ComputeHitRate(forward_returns, candidate_values, /*variance_inflation=*/4.0);
        check("n/k still report raw observed counts under correction",
              corrected.n == 10000 && corrected.k == 5200);
        check("hit_rate is unaffected by variance_inflation",
              close(corrected.hit_rate, baseline.hit_rate, 1e-12));
        check("z_stat matches Python reference under DEFF=4.0",
              close(corrected.z_stat, 2.000000000000002, 1e-9));
        check("z_stat scales by exactly 1/sqrt(DEFF)",
              close(baseline.z_stat / corrected.z_stat, 2.0, 1e-9));
        check("p_value matches Python reference under DEFF=4.0",
              close(corrected.p_value, 4.550026389635820e-02, 1e-9));
        check("Wilson CI matches Python reference under DEFF=4.0",
              close(corrected.ci_lo, 0.500400006272198, 1e-9) &&
              close(corrected.ci_hi, 0.539538622433388, 1e-9));
        const double baseline_width = baseline.ci_hi - baseline.ci_lo;
        const double corrected_width = corrected.ci_hi - corrected.ci_lo;
        check("CI width scales by approximately sqrt(DEFF) at this n (within 1%)",
              corrected_width / baseline_width > 1.98 && corrected_width / baseline_width < 2.02);
    }
    {
        // CI/p-value consistency: both must tell the same significance
        // story under a nontrivial correction (both are derived from the
        // same n_eff/k_eff substitution, so this holds by construction --
        // this test guards against a future edit breaking that shared
        // derivation, e.g. if someone "optimizes" one path without the other).
        std::vector<double> forward_returns(10000, 1.0);
        std::vector<double> candidate_values(10000, 1.0);
        for (int i = 0; i < 4800; ++i) candidate_values[i] = -1.0;
        auto corrected = ComputeHitRate(forward_returns, candidate_values, /*variance_inflation=*/4.0);
        const bool ci_significant = corrected.ci_lo > 0.5 || corrected.ci_hi < 0.5;
        const bool p_significant = corrected.p_value < 0.05;
        check("CI and p-value agree on significance under a nontrivial correction",
              ci_significant == p_significant);
    }
```

- [ ] **Step 2: Run tests to verify they fail**

Run: `mamba run -n mts g++ -std=c++17 tools/observation_vector/test_drift_location_stats.cpp -o /tmp/t_drift`
Expected: FAIL — compile error, `ComputeHitRate` called with 3 arguments but only accepts 2.

- [ ] **Step 3: Implement the effective-sample-size substitution**

In `tools/observation_vector/market_test_stats.h`, replace the `ComputeHitRate` function (currently lines 120-145):

```cpp
inline HitRateResult ComputeHitRate(
    const std::vector<double>& forward_returns, const std::vector<double>& candidate_values) {
    HitRateResult result;
    std::size_t n = 0, k = 0;
    for (std::size_t i = 0; i < forward_returns.size(); ++i) {
        if (!std::isfinite(forward_returns[i])) continue;
        if (candidate_values[i] == 0.0 || !std::isfinite(candidate_values[i])) continue;
        ++n;
        if (Sign(forward_returns[i]) == Sign(candidate_values[i])) {
            ++k;
        }
    }
    result.n = n;
    result.k = k;
    if (n == 0) {
        return result;
    }
    result.hit_rate = static_cast<double>(k) / static_cast<double>(n);
    const double se_null = std::sqrt(0.25 / static_cast<double>(n));
    result.z_stat = (result.hit_rate - 0.5) / se_null;
    result.p_value = 2.0 * (1.0 - 0.5 * (1.0 + std::erf(std::fabs(result.z_stat) / std::sqrt(2.0))));
    const auto ci = ComputeWilsonCI(k, n);
    result.ci_lo = ci.lo;
    result.ci_hi = ci.hi;
    return result;
}
```

with:

```cpp
// variance_inflation (DEFF, design effect): the ratio of the true
// dependence-adjusted variance to the naive i.i.d. variance, derived
// offline (see tools/observation_vector/block_length_and_variance_inflation.py) from a
// Newey-West long-run/short-run variance ratio computed on this
// candidate's real hit/miss indicator series. Default 1.0 (no
// correction) is exact backward compatibility -- every formula below
// reduces to its original, unadjusted form when variance_inflation=1.0.
//
// Applied via an effective-sample-size substitution: n_eff = n / DEFF,
// k_eff = hit_rate * n_eff. Every downstream formula (se_null, z_stat,
// p_value, Wilson CI) is fed n_eff/k_eff instead of n/k, so the CI and
// the p-value are corrected consistently from one shared substitution --
// they cannot disagree with each other the way two independently-adjusted
// formulas could. result.n/result.k still report the RAW observed
// counts (what was actually measured); only the internal statistical
// formulas use the effective, dependence-adjusted sample size.
inline HitRateResult ComputeHitRate(
    const std::vector<double>& forward_returns, const std::vector<double>& candidate_values,
    double variance_inflation = 1.0) {
    HitRateResult result;
    std::size_t n = 0, k = 0;
    for (std::size_t i = 0; i < forward_returns.size(); ++i) {
        if (!std::isfinite(forward_returns[i])) continue;
        if (candidate_values[i] == 0.0 || !std::isfinite(candidate_values[i])) continue;
        ++n;
        if (Sign(forward_returns[i]) == Sign(candidate_values[i])) {
            ++k;
        }
    }
    result.n = n;
    result.k = k;
    if (n == 0) {
        return result;
    }
    result.hit_rate = static_cast<double>(k) / static_cast<double>(n);
    const double n_eff = static_cast<double>(n) / variance_inflation;
    const double k_eff = result.hit_rate * n_eff;
    const double se_null = std::sqrt(0.25 / n_eff);
    result.z_stat = (result.hit_rate - 0.5) / se_null;
    result.p_value = 2.0 * (1.0 - 0.5 * (1.0 + std::erf(std::fabs(result.z_stat) / std::sqrt(2.0))));
    const auto ci = ComputeWilsonCI(k_eff, n_eff);
    result.ci_lo = ci.lo;
    result.ci_hi = ci.hi;
    return result;
}
```

- [ ] **Step 4: Run tests to verify they pass**

Run: `mamba run -n mts g++ -std=c++17 tools/observation_vector/test_drift_location_stats.cpp -o /tmp/t_drift && /tmp/t_drift`
Expected: `ALL PASS` (26 original + 8 new = 34 checks)

- [ ] **Step 5: Commit**

```bash
git add tools/observation_vector/market_test_stats.h tools/observation_vector/test_drift_location_stats.cpp
git commit -m "feat: add variance_inflation (DEFF) correction to ComputeHitRate

Effective-sample-size substitution (n_eff = n/DEFF, k_eff = hit_rate*n_eff)
feeds every downstream formula (se_null, z_stat, p_value, Wilson CI) from
one shared derivation, so the CI and p-value can't disagree under
correction. Default 1.0 is exact backward compatibility, verified: all 26
pre-existing checks pass unchanged. New checks verify the effective-n
substitution against a real Python reference (n=10000, k=5200, DEFF=4.0),
confirm z_stat scales by exactly 1/sqrt(DEFF), confirm the Wilson CI width
scales by approximately sqrt(DEFF) (not exactly -- Wilson's own small-
sample nonlinearity means this is only asymptotic, verified via real
Python computation before writing the test), and confirm CI/p-value
significance agreement under a nontrivial correction."
```

---

## Task 3: `ComputeBootstrapMedianGapCI` gains `variance_inflation` via post-hoc CI widening

**Files:**
- Modify: `tools/observation_vector/market_test_stats.h:252-312`
- Test: `tools/observation_vector/test_market_test_stats_median.cpp` (add new checks; existing 9 must still pass; also fix one stale comment)

**Interfaces:**
- Produces: `MedianGapResult ComputeBootstrapMedianGapCI(const std::vector<double>& top, const std::vector<double>& bottom, std::size_t n_boot = 1000, std::uint64_t seed = 0, double variance_inflation = 1.0)`. No change to the resampling loop or point estimate — this widens the already-computed `ci_lo`/`ci_hi` around the unchanged `gap`.

- [ ] **Step 1: Fix a stale comment referencing the deleted `ComputeBootstrapMeanGapCI`**

In `tools/observation_vector/test_market_test_stats_median.cpp`, the `n_boot == 0` test block currently reads:

```cpp
        // n_boot == 0: point estimate has no randomness and must remain
        // exact; CI bounds must be NaN, not a garbage read (mirrors the
        // n_boot=0 guard already proven necessary for ComputeBootstrapMeanGapCI).
```

Change the last line to:

```cpp
        // n_boot == 0: point estimate has no randomness and must remain
        // exact; CI bounds must be NaN, not a garbage read (the guard
        // this reference itself needs -- gaps.size()-1 would wrap on an
        // empty vector without it).
```

- [ ] **Step 2: Write the failing tests**

Append to `tools/observation_vector/test_market_test_stats_median.cpp`, right before the final `std::printf(g_failures == 0 ...)` line:

```cpp
    {
        // variance_inflation default (1.0) must reproduce the exact same
        // (gap, ci_lo, ci_hi) as calling with the parameter omitted --
        // proves the new parameter is additive, not a behavior change.
        std::vector<double> top = {3.0, -5.0, 8.0};
        std::vector<double> bottom = {1.0, -1.0, 2.0, 10.0};
        auto baseline = ComputeBootstrapMedianGapCI(top, bottom, 1000, 42);
        auto explicit_one = ComputeBootstrapMedianGapCI(top, bottom, 1000, 42, /*variance_inflation=*/1.0);
        check("variance_inflation=1.0 matches the 4-argument call exactly",
              close(explicit_one.gap, baseline.gap, 1e-12) &&
              close(explicit_one.ci_lo, baseline.ci_lo, 1e-12) &&
              close(explicit_one.ci_hi, baseline.ci_hi, 1e-12));
    }
    {
        // Known-factor widening: same seed -> same underlying bootstrap
        // distribution, so the widened CI must be an EXACT deterministic
        // function of the unwidened one: new_ci_lo = gap - (gap-ci_lo)*scale,
        // new_ci_hi = gap + (ci_hi-gap)*scale, scale = sqrt(variance_inflation).
        // No external reference values needed -- this is a self-consistency
        // property of the post-hoc widening transform, not a new statistical
        // claim, so it's verified against the function's own unwidened output
        // rather than a hand- or Python-derived number.
        std::vector<double> top(50000);
        std::vector<double> bottom(50000);
        std::mt19937_64 gen(7);
        std::normal_distribution<double> nd(0.0, 1.0);
        for (auto& x : top) x = nd(gen);
        for (auto& x : bottom) x = nd(gen) + 0.02;
        auto baseline = ComputeBootstrapMedianGapCI(top, bottom, 1000, 99);
        auto widened = ComputeBootstrapMedianGapCI(top, bottom, 1000, 99, /*variance_inflation=*/4.0);
        check("gap is unaffected by variance_inflation",
              close(widened.gap, baseline.gap, 1e-12));
        const double scale = 2.0;  // sqrt(4.0)
        const double expected_lo = baseline.gap - (baseline.gap - baseline.ci_lo) * scale;
        const double expected_hi = baseline.gap + (baseline.ci_hi - baseline.gap) * scale;
        check("ci_lo widens by exactly sqrt(variance_inflation) around gap",
              close(widened.ci_lo, expected_lo, 1e-9));
        check("ci_hi widens by exactly sqrt(variance_inflation) around gap",
              close(widened.ci_hi, expected_hi, 1e-9));
        check("widened CI is strictly wider than baseline",
              (widened.ci_hi - widened.ci_lo) > (baseline.ci_hi - baseline.ci_lo));
    }
    {
        // n_boot=0 still returns NaN CI bounds regardless of
        // variance_inflation (NaN * anything is NaN, gap stays exact) --
        // confirms the widening transform doesn't crash or silently
        // produce a finite value out of the existing NaN-CI guard.
        std::vector<double> top = {3.0, -5.0, 8.0};
        std::vector<double> bottom = {1.0, -1.0, 2.0, 10.0};
        auto result = ComputeBootstrapMedianGapCI(top, bottom, /*n_boot=*/0, /*seed=*/0, /*variance_inflation=*/4.0);
        check("n_boot=0 point estimate is still exact under variance_inflation",
              close(result.gap, 3.5, 1e-9));
        check("n_boot=0 CI bounds are still NaN under variance_inflation",
              std::isnan(result.ci_lo) && std::isnan(result.ci_hi));
    }
```

- [ ] **Step 3: Run tests to verify they fail**

Run: `mamba run -n mts g++ -std=c++17 tools/observation_vector/test_market_test_stats_median.cpp -o /tmp/t_median`
Expected: FAIL — compile error, `ComputeBootstrapMedianGapCI` called with 4-5 arguments but only accepts up to 2.

- [ ] **Step 4: Implement the post-hoc widening**

In `tools/observation_vector/market_test_stats.h`, change the `ComputeBootstrapMedianGapCI` signature (currently at line 252-254):

```cpp
inline MedianGapResult ComputeBootstrapMedianGapCI(
    const std::vector<double>& top, const std::vector<double>& bottom,
    std::size_t n_boot = 1000, std::uint64_t seed = 0) {
```

to:

```cpp
inline MedianGapResult ComputeBootstrapMedianGapCI(
    const std::vector<double>& top, const std::vector<double>& bottom,
    std::size_t n_boot = 1000, std::uint64_t seed = 0, double variance_inflation = 1.0) {
```

Leave the entire function body (resampling loop, point estimate, percentile computation) unchanged through the point where `result.ci_hi = PercentileFromSorted(gaps, 97.5);` is set. Immediately after that line and before `return result;` (currently the last two lines of the function, around line 310-312), add:

```cpp
    // Widen each side around the point estimate independently (preserves
    // any asymmetry the bootstrap distribution itself has -- does not
    // assume a symmetric/normal-shaped CI). variance_inflation=1.0 is the
    // identity transform: scale=1.0, ci_lo/ci_hi unchanged.
    const double scale = std::sqrt(variance_inflation);
    result.ci_lo = result.gap - (result.gap - result.ci_lo) * scale;
    result.ci_hi = result.gap + (result.ci_hi - result.gap) * scale;
```

so the full tail of the function reads:

```cpp
    std::sort(gaps.begin(), gaps.end());
    MedianGapResult result;
    result.gap = point_gap;
    result.ci_lo = PercentileFromSorted(gaps, 2.5);
    result.ci_hi = PercentileFromSorted(gaps, 97.5);

    // Widen each side around the point estimate independently (preserves
    // any asymmetry the bootstrap distribution itself has -- does not
    // assume a symmetric/normal-shaped CI). variance_inflation=1.0 is the
    // identity transform: scale=1.0, ci_lo/ci_hi unchanged.
    const double scale = std::sqrt(variance_inflation);
    result.ci_lo = result.gap - (result.gap - result.ci_lo) * scale;
    result.ci_hi = result.gap + (result.ci_hi - result.gap) * scale;
    return result;
}
```

Also update the function's doc comment (the "KNOWN LIMITATION, not yet addressed here" paragraph, currently around lines 231-244) — replace it with:

```cpp
// DEPENDENCE CORRECTION: the bootstrap resampling above still treats
// individual elements as i.i.d. (a real limitation for this function's
// actual callers, whose forward-return signals overlap heavily -- see
// tools/observation_vector/block_length_and_variance_inflation.py and docs/superpowers/specs/
// 2026-08-30-bootstrap-dependence-correction-design.md for the full
// derivation). Rather than rebuilding the resampling itself into a
// dependence-aware (block) bootstrap -- a materially larger, higher-risk
// change given this file's own history of resampling-algorithm rewrites
// needing multiple review rounds -- the caller supplies a measured
// variance_inflation (DEFF) factor, and the already-computed CI is widened
// around the unchanged point estimate below. DEFF for this function should
// be derived from the BELOW-MEDIAN INDICATOR series (not the raw |values|),
// since this function tests a median, whose asymptotic variance under
// dependence is a different quantity than a mean's -- see the design spec
// §2 for why applying a raw-value DEFF here would be inconsistent with
// this function's own fat-tail-robustness rationale.
```

- [ ] **Step 5: Run tests to verify they pass**

Run: `mamba run -n mts g++ -std=c++17 tools/observation_vector/test_market_test_stats_median.cpp -o /tmp/t_median && /tmp/t_median`
Expected: `ALL PASS` (9 original + 6 new = 15 checks)

- [ ] **Step 6: Commit**

```bash
git add tools/observation_vector/market_test_stats.h tools/observation_vector/test_market_test_stats_median.cpp
git commit -m "feat: add variance_inflation (DEFF) correction to ComputeBootstrapMedianGapCI

Post-hoc widening of the already-computed percentile CI around the
unchanged point estimate -- no change to the resampling loop itself,
deliberately, given this file's own history of resampling-algorithm
rewrites needing multiple review rounds. Default 1.0 is exact backward
compatibility, verified: all 9 pre-existing checks pass unchanged. New
checks verify the widening is a self-consistent deterministic transform
of the unwidened output (same seed -> exact scale-by-sqrt(DEFF)
relationship, no external reference values needed for this part), that
gap/n_boot=0 behavior is unaffected, and fix a stale comment referencing
the deleted ComputeBootstrapMeanGapCI."
```

---

## Task 4: Export the real hit/miss indicator series from `drift_location_eval.cpp`

**Files:**
- Modify: `tools/observation_vector/drift_location_eval.cpp`

**Interfaces:**
- Produces: `--export-signals-dir DIR` CLI flag. When set, writes one CSV per horizon: `DIR/drift_location_hitmiss_h<H>.csv`, single column `hit` (0/1), one row per signal that passed `ComputeHitRate`'s own inclusion filter (finite forward return, finite nonzero candidate value), in original chronological order.

- [ ] **Step 1: Add the flag to argument parsing and usage string**

In `tools/observation_vector/drift_location_eval.cpp`, change `PrintUsage` (currently lines 28-32):

```cpp
void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --ticks-parquet PATH [--window N] [--horizons 30,60,120,240] "
        "[--report-json PATH]\n", argv0);
}
```

to:

```cpp
void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --ticks-parquet PATH [--window N] [--horizons 30,60,120,240] "
        "[--report-json PATH] [--export-signals-dir DIR]\n", argv0);
}
```

In `main()`, change the variable declarations (currently line 47):

```cpp
    std::string ticks_path, report_json_path;
```

to:

```cpp
    std::string ticks_path, report_json_path, export_signals_dir;
```

and add a new `else if` branch to the argument-parsing loop (currently lines 57-61), immediately after the `--report-json` branch:

```cpp
        else if (arg == "--report-json") report_json_path = next("--report-json");
        else if (arg == "--export-signals-dir") export_signals_dir = next("--export-signals-dir");
```

- [ ] **Step 2: Export the hit/miss indicator series inside the per-horizon loop**

In the per-horizon `for` loop (currently starting at line 103), immediately after the line `const auto result = ComputeHitRate(fwd, signal_z);` (line 107), add:

```cpp
        if (!export_signals_dir.empty()) {
            std::ofstream sig_csv(export_signals_dir + "/drift_location_hitmiss_h" + std::to_string(h) + ".csv");
            sig_csv << "hit\n";
            for (std::size_t i = 0; i < fwd.size(); ++i) {
                if (!std::isfinite(fwd[i])) continue;
                if (signal_z[i] == 0.0 || !std::isfinite(signal_z[i])) continue;
                sig_csv << (Sign(fwd[i]) == Sign(signal_z[i]) ? 1 : 0) << "\n";
            }
        }
```

This reproduces `ComputeHitRate`'s own inclusion filter and hit/miss definition exactly (same `std::isfinite`/nonzero checks, same `Sign()` comparison from `market_test_stats.h`), so the exported series matches the real population the statistical function operates on -- not a re-derivation, a direct mirror of 4 lines already reviewed and tested inside `ComputeHitRate` itself.

- [ ] **Step 3: Rebuild**

Run:
```bash
mamba run -n mts g++ -O2 -std=c++17 \
  $(mamba run -n mts pkg-config --cflags arrow parquet) \
  tools/observation_vector/drift_location_eval.cpp \
  $(mamba run -n mts pkg-config --libs arrow parquet) \
  -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
  -o tools/drift_location_eval
```
Expected: compiles cleanly.

- [ ] **Step 4: Run against the real production file and verify the exports**

```bash
mkdir -p /tmp/drift_location_signals
./tools/drift_location_eval --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_continuous_ticks.parquet --export-signals-dir /tmp/drift_location_signals
```

Expected: the tool runs to completion exactly as before (identical console output to a run without `--export-signals-dir` -- the flag only adds file writes, no behavior change to the printed results), AND `/tmp/drift_location_signals/` contains 4 files (`drift_location_hitmiss_h30.csv`, `_h60.csv`, `_h120.csv`, `_h240.csv`). Verify with:

```bash
mamba run -n mts python3 -c "
import pandas as pd
for h in [30, 60, 120, 240]:
    df = pd.read_csv(f'/tmp/drift_location_signals/drift_location_hitmiss_h{h}.csv')
    assert set(df['hit'].unique()) <= {0, 1}, f'h={h}: non-binary values found'
    print(f'h={h}min: n={len(df)} rows, hit_rate={df[\"hit\"].mean():.4f}')
"
```

Expected: prints 4 lines, one per horizon, each with a real row count in the millions and a `hit_rate` close to (not necessarily identical to, since this recomputes the mean independently) the value the tool itself printed to console in Step 4's own run.

- [ ] **Step 5: Commit**

```bash
git add tools/observation_vector/drift_location_eval.cpp
git commit -m "feat: add --export-signals-dir to drift_location_eval for DEFF calibration

Writes the real hit/miss indicator series (same inclusion filter and
Sign() comparison ComputeHitRate itself uses) to CSV, one file per
horizon -- input for tools/observation_vector/block_length_and_variance_inflation.py's
offline DEFF derivation (Task 6). Verified against the real 38.5M-row
production file: 4 files written, all values binary, row counts and
hit rates consistent with the tool's own console output."
```

---

## Task 5: Export the real below-median indicator series from `jump_ratio_eval.cpp`

**Files:**
- Modify: `tools/observation_vector/jump_ratio_eval.cpp`

**Interfaces:**
- Consumes: `MedianAbs` from `market_test_stats.h` (already included).
- Produces: `--export-signals-dir DIR` CLI flag. When set, writes two CSVs per horizon: `DIR/jump_ratio_top_belowmedian_h<H>.csv` and `DIR/jump_ratio_bottom_belowmedian_h<H>.csv`, single column `below_median` (0/1), using the SAME `top_median`/`bottom_median` values (`MedianAbs(top_fwd)`/`MedianAbs(bottom_fwd)`) that `ComputeBootstrapMedianGapCI`'s own point estimate is built from.

- [ ] **Step 1: Add the flag to argument parsing and usage string**

Change `PrintUsage` (currently lines 41-45):

```cpp
void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --ticks-parquet PATH [--window N] [--horizons 30,60,120,240] "
        "[--top-decile 0.10] [--report-json PATH]\n", argv0);
}
```

to:

```cpp
void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --ticks-parquet PATH [--window N] [--horizons 30,60,120,240] "
        "[--top-decile 0.10] [--report-json PATH] [--export-signals-dir DIR]\n", argv0);
}
```

In `main()`, change the variable declarations (currently line 65):

```cpp
    std::string ticks_path, report_json_path;
```

to:

```cpp
    std::string ticks_path, report_json_path, export_signals_dir;
```

and add a new `else if` branch to the argument-parsing loop (currently lines 76-81), immediately after the `--report-json` branch:

```cpp
        else if (arg == "--report-json") report_json_path = next("--report-json");
        else if (arg == "--export-signals-dir") export_signals_dir = next("--export-signals-dir");
```

- [ ] **Step 2: Export the below-median indicator series inside the per-horizon loop**

In the per-horizon `for` loop, immediately after the lines computing `top_median`/`bottom_median` (currently):

```cpp
        const double top_median = MedianAbs(top_fwd);
        const double bottom_median = MedianAbs(bottom_fwd);
```

add:

```cpp
        if (!export_signals_dir.empty()) {
            std::ofstream top_csv(export_signals_dir + "/jump_ratio_top_belowmedian_h" + std::to_string(h) + ".csv");
            top_csv << "below_median\n";
            for (double v : top_fwd) top_csv << (std::fabs(v) <= top_median ? 1 : 0) << "\n";
            std::ofstream bottom_csv(export_signals_dir + "/jump_ratio_bottom_belowmedian_h" + std::to_string(h) + ".csv");
            bottom_csv << "below_median\n";
            for (double v : bottom_fwd) bottom_csv << (std::fabs(v) <= bottom_median ? 1 : 0) << "\n";
        }
```

- [ ] **Step 3: Rebuild**

Run:
```bash
mamba run -n mts g++ -O2 -std=c++17 \
  $(mamba run -n mts pkg-config --cflags arrow parquet) \
  tools/observation_vector/jump_ratio_eval.cpp \
  $(mamba run -n mts pkg-config --libs arrow parquet) \
  -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
  -o tools/jump_ratio_eval
```
Expected: compiles cleanly.

- [ ] **Step 4: Run against the real production file and verify the exports**

```bash
mkdir -p /tmp/jump_ratio_signals
./tools/jump_ratio_eval --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_continuous_ticks.parquet --export-signals-dir /tmp/jump_ratio_signals
```

Expected: this is the same ~15-minute real run as before (median-based bootstrap, `n_boot=1000`) -- budget for it, don't cut it short. Console output identical to a run without `--export-signals-dir`. `/tmp/jump_ratio_signals/` should contain 8 files (top + bottom × 4 horizons). Verify with:

```bash
mamba run -n mts python3 -c "
import pandas as pd
for h in [30, 60, 120, 240]:
    for side in ['top', 'bottom']:
        df = pd.read_csv(f'/tmp/jump_ratio_signals/jump_ratio_{side}_belowmedian_h{h}.csv')
        assert set(df['below_median'].unique()) <= {0, 1}, f'h={h} {side}: non-binary values found'
        frac = df['below_median'].mean()
        print(f'h={h}min {side}: n={len(df)} rows, frac_below_median={frac:.4f}')
"
```

Expected: prints 8 lines. `frac_below_median` should be close to 0.5 for each (that's the point of splitting at the median) — a value wildly off 0.5 (e.g. below 0.3 or above 0.7) would indicate an indexing/threshold bug, not an acceptable result to accept at face value.

- [ ] **Step 5: Commit**

```bash
git add tools/observation_vector/jump_ratio_eval.cpp
git commit -m "feat: add --export-signals-dir to jump_ratio_eval for DEFF calibration

Writes the real below-median indicator series for both decile groups
(using the same MedianAbs() threshold ComputeBootstrapMedianGapCI's own
point estimate is built from) to CSV, one pair of files per horizon --
input for tools/observation_vector/block_length_and_variance_inflation.py's offline DEFF
derivation (Task 6). Verified against the real 38.5M-row production
file: 8 files written, all values binary, frac_below_median close to
0.5 for every horizon/side as expected."
```

---

## Task 6: `tools/observation_vector/block_length_and_variance_inflation.py` — the offline DEFF calibration script

**Design note added 2026-08-30, before dispatch, per a second-opinion research pass (`lbrnet/logs/
rc_gemini.log`, `CLAUDE_BRIEF_116`/`117`) plus real benchmarking done by the controller before this
task was dispatched — two real, load-bearing findings changed this task's design from an earlier
draft:**

1. **`arch.covariance.kernel.NeweyWest`'s Bartlett kernel can *underestimate* the true long-run
   variance if its bandwidth doesn't exceed the tick-count length of the horizon-driven overlap.**
   The dependence here is a physically bounded MA(L) process (L = number of ticks the forward-return
   horizon spans), and a Bartlett kernel downweights valid positive correlation near lag L unless the
   bandwidth is chosen to comfortably exceed it. **Do not use `NeweyWest`'s automatic bandwidth** --
   pass an explicit, real-data-derived bandwidth instead (below).
2. **`NeweyWest` is also computationally expensive at this tool's real scale, independent of finding
   #1** -- confirmed by direct benchmark (not assumed): at `n=800,000`, `bandwidth=6000` took 11.1s;
   at `n=1,500,000`, `bandwidth=3000` took 11.6s. At this tool's real scale (~3.85M elements per
   series, with a bandwidth in the thousands to tens-of-thousands depending on horizon), each call is
   expected to take tens of seconds to several minutes, and **12 total calls are needed** (4 horizons
   for `drift_location` + 4 horizons x 2 decile groups for `jump_ratio`) -- **budget ~20-30 minutes
   real wall-clock time for this task's Step 2, similar in spirit to Task 5's own ~15-minute real
   run.** This is a genuine, measured cost of a one-time offline calibration, not a hang.

**Given finding #1, this task now also computes a second, independent, self-tuning DEFF estimate via
Geyer's (1992) initial monotone sequence tau_int estimator** (confirmed via `statsmodels.tsa.
stattools.acf(fft=True)`, which is fast at this scale -- benchmarked at 1.3-1.6s even at the largest
bandwidth needed, no performance concern) -- Geyer's method self-truncates its own effective lag
cutoff (sums adjacent autocorrelation pairs until the sum turns negative) rather than requiring a
hand-chosen bandwidth, so it is NOT vulnerable to finding #1's underestimation risk. **The final DEFF
used is `max(NeweyWest_DEFF, Geyer_DEFF)`** -- since the known failure mode is underestimation, not
overestimation, taking the larger of two independently-derived estimates is the conservative,
defensible choice, not an arbitrary tie-break.

**Files:**
- Create: `tools/observation_vector/block_length_and_variance_inflation.py`

**Interfaces:**
- Consumes: the CSVs written by Tasks 4/5 in `/tmp/drift_location_signals/` and `/tmp/jump_ratio_signals/`.
- Produces: a printed report (block length + both DEFF estimates + the final chosen DEFF per
  candidate/horizon) and a printed C++-ready table of `variance_inflation` (the final, max-of-two
  DEFF) values per horizon for each candidate — consumed by Task 7.

- [ ] **Step 1: Write the script**

```python
#!/usr/bin/env python3
"""Offline dependence-correction calibration for the two candidate-
validation tools' bootstrap/hit-rate CIs (docs/superpowers/specs/
2026-08-30-bootstrap-dependence-correction-design.md): both
tools/observation_vector/drift_location_eval.cpp's ComputeHitRate and
tools/observation_vector/jump_ratio_eval.cpp's ComputeBootstrapMedianGapCI treat individual
per-tick forward-return signals as i.i.d., but real forward returns
overlap heavily (a 240-minute forward return shares most of its
underlying tick data with thousands of adjacent signals), understating
every reported CI's width.

Method: the design effect (DEFF, Kish 1965: ratio of true to naive-i.i.d.
variance), computed on BOUNDED {0,1} INDICATOR series (hit/miss for
drift_location; below-median for jump_ratio), not on the raw
|forward_return| values. This is deliberate, not an approximation:
ComputeHitRate's hit-rate is itself a mean of a 0/1 indicator, so a
Newey-West (1987) long-run/short-run variance ratio on that indicator is
exactly the right generalization for a mean. ComputeBootstrapMedianGapCI
tests a MEDIAN; per the Bahadur (1966) representation and Sen (1968)'s
dependent-quantile theorem, a sample quantile's asymptotic variance under
dependence is governed by the long-run variance of the below-quantile
indicator process I(X<=q) -- the unobserved marginal density at the
quantile cancels out of the DEFF ratio entirely (verified algebraically:
DEFF_quantile = LRV(I(X<=q)) / Var_iid(I(X<=q)), independent of the
density term), so applying the SAME indicator-transform-then-Newey-West
recipe used for the hit-rate case is the correct generalization for a
median too, per Babu (1986) and Politis (2001)'s treatment of dependent
quantile resampling via HAC estimators on the indicator transform. This
also has the practical benefit that a bounded indicator series is
estimator-stable regardless of how heavy the underlying return
distribution's tails are.

Two independent DEFF estimates are computed and the larger is used --
see this task's own plan-document design note for why (NeweyWest's
Bartlett kernel can underestimate variance for a bounded-overlap MA(L)
process unless its bandwidth comfortably exceeds L; Geyer's (1992)
self-truncating tau_int estimator doesn't have that failure mode, so
taking the max is a conservative cross-check, not an arbitrary tie-break).

Real tick density (verified directly against the production file, not
assumed): 38,547,467 ticks over a 1,685,015.4-minute span = 22.8766
ticks/minute (wall-clock average, including off-hours). Bandwidth for
each horizon is 5x the average tick-count the horizon spans, as a safety
margin against intraday tick-density variability (this is an average,
not a worst-case measurement -- 5x is a deliberate cushion, not derived
from a measured peak).

Data: CSVs exported by tools/observation_vector/drift_location_eval.cpp and
tools/observation_vector/jump_ratio_eval.cpp's --export-signals-dir flag (see that flag's
own doc comment for the exact population each series represents) --
run those tools first if the expected files below don't exist.

Real runtime cost, measured before this script was written (see plan
Task 6's own design note): ~20-30 minutes total across all 12 NeweyWest
calls. This is expected, not a hang -- budget for it.

Usage (mts env):
    source /home/rcruz/anaconda3/etc/profile.d/conda.sh && mamba activate mts
    python3 tools/observation_vector/block_length_and_variance_inflation.py
"""
from pathlib import Path

import numpy as np
import pandas as pd
from arch.covariance.kernel import NeweyWest
from statsmodels.tsa.stattools import acf

DRIFT_LOCATION_DIR = Path("/tmp/drift_location_signals")
JUMP_RATIO_DIR = Path("/tmp/jump_ratio_signals")
HORIZONS = [30, 60, 120, 240]

# Real measured average tick density (verified against the production
# file directly -- see this module's own docstring for the derivation).
TICKS_PER_MINUTE = 22.8766
BANDWIDTH_SAFETY_MULTIPLE = 5


def bandwidth_for_horizon(horizon_minutes: int) -> int:
    """Bandwidth (in signal-index lags) for NeweyWest, sized to
    comfortably exceed the tick-count length of this horizon's overlap
    window -- see this task's design note on why the default automatic
    bandwidth is unsafe for this specific bounded-MA(L) dependence
    structure."""
    return int(np.ceil(BANDWIDTH_SAFETY_MULTIPLE * TICKS_PER_MINUTE * horizon_minutes))


def geyer_tau_int(x: np.ndarray, max_lag: int) -> tuple[float, float]:
    """Geyer (1992) initial monotone sequence estimator of the integrated
    autocorrelation time tau_int (standard normalization:
    tau_int = 1 + 2*sum_{k=1}^{K} rho(k)), and the implied
    N_eff = n/tau_int. Self-truncating: sums adjacent autocorrelation
    pairs Gamma_m = rho[2m] + rho[2m+1] until the running sum stops being
    positive and monotone-decreasing, rather than requiring a hand-chosen
    cutoff. Verified against two known cases before being used here (an
    earlier draft of this function used an incorrectly doubled
    normalization, N_eff = n/(2*tau_int) -- caught by the first of these
    two checks, an i.i.d. sanity test, before this script was dispatched):
      - i.i.d. data (no dependence): theoretical tau_int=1.0 exactly;
        measured 1.0114 at n=300,000.
      - AR(1), phi=0.7: theoretical tau_int=(1+phi)/(1-phi)=5.6667;
        measured 5.6438 at n=2,000,000 (~0.4% error)."""
    x = np.asarray(x, dtype=np.float64)
    n = len(x)
    rho = acf(x, nlags=max_lag, fft=True)
    m_max = (max_lag - 1) // 2
    gammas = [rho[2 * m] + rho[2 * m + 1] for m in range(m_max + 1)]
    big_m = 0
    for m in range(1, len(gammas)):
        if gammas[m] > 0 and gammas[m] <= gammas[m - 1]:
            big_m = m
        else:
            break
    tau_int = -1.0 + 2.0 * sum(gammas[: big_m + 1])
    n_eff = n / tau_int if tau_int > 0 else float(n)
    return tau_int, n_eff


def report_deff(label: str, series: np.ndarray, horizon_minutes: int) -> float:
    """Prints both DEFF estimates (Newey-West with an explicit,
    horizon-sized bandwidth; Geyer's self-tuning tau_int) and the final
    chosen value (the max of the two) for one indicator series. Returns
    the final DEFF."""
    series = series.astype(np.float64)
    bandwidth = bandwidth_for_horizon(horizon_minutes)
    cov = NeweyWest(series, bandwidth=bandwidth).cov
    long_run = float(cov.long_run[0, 0])
    short_run = float(cov.short_run[0, 0])
    deff_nw = long_run / short_run
    tau_int, n_eff = geyer_tau_int(series, max_lag=bandwidth)
    # DEFF = N/N_eff = tau_int directly under the standard normalization
    # (tau_int=1 + 2*sum rho(k)), NOT 2*tau_int -- an earlier draft used
    # the doubled form, caught by an i.i.d. sanity check (see
    # geyer_tau_int's own docstring) before this script was dispatched.
    deff_geyer = tau_int
    deff_final = max(deff_nw, deff_geyer)
    print(f"  {label:<38} n={len(series):>9} bandwidth={bandwidth:>7}  "
          f"DEFF_NW={deff_nw:8.4f}  DEFF_Geyer={deff_geyer:8.4f}  "
          f"-> final={deff_final:8.4f}  sqrt(final)={deff_final ** 0.5:7.4f}")
    if abs(deff_nw - deff_geyer) / max(deff_nw, deff_geyer) > 0.5:
        print(f"    NOTE: the two DEFF estimates disagree by >50% -- worth a closer look "
              f"before trusting this horizon's number, per CLAUDE_BRIEF_116/117's own "
              f"sanity-check rule.")
    return deff_final


def main() -> None:
    print("=== drift_location (ComputeHitRate: hit/miss indicator) ===")
    drift_deff: dict[int, float] = {}
    for h in HORIZONS:
        path = DRIFT_LOCATION_DIR / f"drift_location_hitmiss_h{h}.csv"
        series = pd.read_csv(path)["hit"].to_numpy()
        drift_deff[h] = report_deff(f"h={h}min", series, h)

    print("\n=== jump_ratio (ComputeBootstrapMedianGapCI: below-median indicator) ===")
    jump_deff: dict[int, float] = {}
    for h in HORIZONS:
        top = pd.read_csv(JUMP_RATIO_DIR / f"jump_ratio_top_belowmedian_h{h}.csv")["below_median"].to_numpy()
        bottom = pd.read_csv(JUMP_RATIO_DIR / f"jump_ratio_bottom_belowmedian_h{h}.csv")["below_median"].to_numpy()
        top_deff = report_deff(f"h={h}min top decile", top, h)
        bottom_deff = report_deff(f"h={h}min bottom decile", bottom, h)
        # Simple average of the two groups' final DEFFs (deliberate
        # simplification, not a rigorous variance-weighted combination --
        # both groups are comparable size and share the same
        # horizon-driven overlap mechanism, per the design spec's own
        # §4.1 note).
        jump_deff[h] = (top_deff + bottom_deff) / 2.0
        print(f"    -> combined (simple average of top/bottom): "
              f"DEFF={jump_deff[h]:.4f}  sqrt(DEFF)={jump_deff[h] ** 0.5:.4f}")

    print("\n=== C++ constants to embed (Task 7) ===")
    print("drift_location_eval.cpp's VarianceInflationFor():")
    for h in HORIZONS:
        print(f"        {{{h}, {drift_deff[h]:.6f}}},")
    print("jump_ratio_eval.cpp's VarianceInflationFor():")
    for h in HORIZONS:
        print(f"        {{{h}, {jump_deff[h]:.6f}}},")


if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run against the real exported CSVs from Tasks 4/5**

Run: `mamba run -n mts python3 tools/observation_vector/block_length_and_variance_inflation.py`

**Expected real runtime: ~20-30 minutes** (12 `NeweyWest` calls at real production scale — this is a
measured, expected cost, not a hang; see this task's own design note). Budget for it the same way
Task 5's ~15-minute real run was budgeted for — do not cut it short or investigate the runtime itself
as a problem.

Expected output: prints the full report (both DEFF estimates + the final chosen value for all 4
drift_location horizons, all 8 jump_ratio top/bottom series, the 4 combined jump_ratio DEFFs) ending
with two C++-ready tables. **Read the actual printed numbers.** Sanity-check before proceeding to
Task 7:
- Every final `DEFF >= 1.0` (dependence can only inflate variance relative to i.i.d., never deflate it
  below 1 for positively-autocorrelated overlap — a value below 1.0 would indicate a bug, not a real
  result to accept).
- `DEFF` generally increases with horizon within each candidate (240min should show more
  overlap-driven inflation than 30min) — if this doesn't hold, investigate before trusting the
  numbers, don't proceed silently.
- No `NaN`/`Inf` in any printed value.
- Note (don't block on) any horizon where the script prints the >50%-disagreement warning between
  `DEFF_NW` and `DEFF_Geyer` — record which ones, if any, in your report, since Task 8's documentation
  should mention it if it occurred.

Record the two printed tables verbatim (copy the actual terminal output) — Task 7 needs these exact numbers.

- [ ] **Step 3: Commit**

```bash
git add tools/observation_vector/block_length_and_variance_inflation.py
git commit -m "feat: add offline DEFF calibration script for the dependence correction

Derives the design effect (Kish 1965) per candidate/horizon from the real
hit/miss and below-median indicator series exported by Tasks 4/5 --
indicator-based, not raw-value-based, so the derivation is fat-tail-safe
(bounded {0,1} series regardless of the underlying return distribution's
tail heaviness) and statistically consistent with what each function
actually tests (a mean for ComputeHitRate, a median for
ComputeBootstrapMedianGapCI, per the Bahadur 1966/Sen 1968 dependent-
quantile theorem -- the marginal density cancels out of the DEFF ratio).

Two independent DEFF estimates per series (Newey-West 1987 with an
explicit, horizon-sized bandwidth -- NOT the automatic default, which can
underestimate variance for this bounded-overlap MA(L) dependence
structure per a second-opinion research pass; and Geyer 1992's
self-tuning tau_int, immune to that specific failure mode) -- final value
is the max of the two, a conservative choice given the known failure
mode is underestimation, not overestimation.

Run against the real 38.5M-row production file's exports (~20-30min real
runtime, NeweyWest is expensive at this scale, confirmed by direct
benchmark before this script was written); all final DEFF values sane
(>=1.0, no NaN/Inf, increasing with horizon as expected)."
```

---

## Task 7: Wire the real DEFF values into both tools and re-verify end to end

**Files:**
- Modify: `tools/observation_vector/drift_location_eval.cpp`
- Modify: `tools/observation_vector/jump_ratio_eval.cpp`

**Interfaces:**
- Consumes: `ComputeHitRate(forward_returns, candidate_values, double variance_inflation)` from Task 2; `ComputeBootstrapMedianGapCI(top, bottom, n_boot, seed, double variance_inflation)` from Task 3; the real DEFF tables printed by Task 6.

- [ ] **Step 1: Add the DEFF lookup to `drift_location_eval.cpp`**

Add `#include <map>` to the includes (alongside the existing `<vector>` etc.), then add this function to the anonymous namespace, right after `ParseHorizons`:

```cpp
double VarianceInflationFor(int horizon_minutes) {
    // DEFF (design effect / variance inflation factor) per horizon,
    // derived via tools/observation_vector/block_length_and_variance_inflation.py against
    // the real 38.5M-row production file's hit/miss indicator series --
    // measured, not theoretical. Re-run that script (after re-running
    // this tool with --export-signals-dir) to reproduce or update these
    // numbers if the candidate logic or the underlying tick data changes.
    static const std::map<int, double> kDeff = {
        {30, /* PASTE Task 6's real drift_location DEFF for h=30 here */},
        {60, /* PASTE Task 6's real drift_location DEFF for h=60 here */},
        {120, /* PASTE Task 6's real drift_location DEFF for h=120 here */},
        {240, /* PASTE Task 6's real drift_location DEFF for h=240 here */},
    };
    auto it = kDeff.find(horizon_minutes);
    return it != kDeff.end() ? it->second : 1.0;
}
```

Replace the 4 placeholder comments with the exact 4 numbers Task 6 printed under "drift_location_eval.cpp's VarianceInflationFor()" — these are real, measured values from a real run, not estimates; do not invent or round them.

Change the call site (currently `const auto result = ComputeHitRate(fwd, signal_z);`) to:

```cpp
        const auto result = ComputeHitRate(fwd, signal_z, VarianceInflationFor(h));
```

- [ ] **Step 2: Add the DEFF lookup to `jump_ratio_eval.cpp`**

Add `#include <map>` to the includes, then add the same-shaped function to the anonymous namespace, right after `Percentile`:

```cpp
double VarianceInflationFor(int horizon_minutes) {
    // DEFF (design effect / variance inflation factor) per horizon,
    // derived via tools/observation_vector/block_length_and_variance_inflation.py against
    // the real 38.5M-row production file's below-median indicator series
    // (averaged across the top/bottom decile groups) -- measured, not
    // theoretical. Re-run that script (after re-running this tool with
    // --export-signals-dir) to reproduce or update these numbers if the
    // candidate logic or the underlying tick data changes.
    static const std::map<int, double> kDeff = {
        {30, /* PASTE Task 6's real jump_ratio DEFF for h=30 here */},
        {60, /* PASTE Task 6's real jump_ratio DEFF for h=60 here */},
        {120, /* PASTE Task 6's real jump_ratio DEFF for h=120 here */},
        {240, /* PASTE Task 6's real jump_ratio DEFF for h=240 here */},
    };
    auto it = kDeff.find(horizon_minutes);
    return it != kDeff.end() ? it->second : 1.0;
}
```

Replace the 4 placeholder comments with the exact 4 numbers Task 6 printed under "jump_ratio_eval.cpp's VarianceInflationFor()".

Change the call site (currently `const auto result = ComputeBootstrapMedianGapCI(top_fwd, bottom_fwd, /*n_boot=*/1000);`) to:

```cpp
        const auto result = ComputeBootstrapMedianGapCI(top_fwd, bottom_fwd, /*n_boot=*/1000, /*seed=*/0,
                                                          VarianceInflationFor(h));
```

- [ ] **Step 3: Rebuild both tools**

```bash
mamba run -n mts g++ -O2 -std=c++17 \
  $(mamba run -n mts pkg-config --cflags arrow parquet) \
  tools/observation_vector/drift_location_eval.cpp \
  $(mamba run -n mts pkg-config --libs arrow parquet) \
  -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
  -o tools/drift_location_eval

mamba run -n mts g++ -O2 -std=c++17 \
  $(mamba run -n mts pkg-config --cflags arrow parquet) \
  tools/observation_vector/jump_ratio_eval.cpp \
  $(mamba run -n mts pkg-config --libs arrow parquet) \
  -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
  -o tools/jump_ratio_eval
```
Expected: both compile cleanly.

- [ ] **Step 4: Re-run both against the real production file**

```bash
./tools/drift_location_eval --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_continuous_ticks.parquet --report-json /tmp/drift_location_report_corrected.json
./tools/jump_ratio_eval --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_continuous_ticks.parquet --report-json /tmp/jump_ratio_report_corrected.json
```

Expected: both run to completion (the `jump_ratio_eval` run takes ~15 minutes as before — do not cut it short). **Read the actual printed numbers.** Verify:

```bash
mamba run -n mts python3 -c "
import json
drift = json.load(open('/tmp/drift_location_report_corrected.json'))
jump = json.load(open('/tmp/jump_ratio_report_corrected.json'))
for h in drift['horizons']:
    assert h['ci_lo'] <= h['ci_hi'], h
    print(f\"drift_location h={h['horizon_minutes']}min: hit_rate={h['hit_rate']:.4f} \"
          f\"ci=[{h['ci_lo']:.4f},{h['ci_hi']:.4f}] p={h['p_value']:.4e}\")
for h in jump['horizons']:
    assert h['ci_lo'] <= h['ci_hi'], h
    print(f\"jump_ratio h={h['horizon_minutes']}min: gap={h['gap']:+.6f} \"
          f\"ci=[{h['ci_lo']:+.6f},{h['ci_hi']:+.6f}] survives={h['survives']}\")
"
```

Expected: both print 4 lines each, all CIs correctly ordered, all wider than the pre-correction numbers recorded in the brainstorm doc/ledger (§5.0 for drift_location: 0.4854-0.4957 hit rates; §5.1 for jump_ratio: the 55.2%/55.1%/58.3%/62.6% figures) — **confirm neither verdict flips**: drift_location's hit-rate CIs should still not survive Bonferroni (still rejected), jump_ratio's `survives` should still read `true` at all 4 horizons. If either verdict DOES flip, stop and investigate before proceeding — that would be a genuinely important finding, not something to paper over.

- [ ] **Step 5: Re-run the native test suites one final time to confirm no regression**

```bash
mamba run -n mts g++ -std=c++17 tools/observation_vector/test_drift_location_stats.cpp -o /tmp/t1 && /tmp/t1 | tail -1
mamba run -n mts g++ -std=c++17 tools/observation_vector/test_market_test_stats_median.cpp -o /tmp/t2 && /tmp/t2 | tail -1
mamba run -n mts g++ -std=c++17 tools/observation_vector/test_jump_ratio_stats.cpp -o /tmp/t3 && /tmp/t3 | tail -1
```
Expected: `ALL PASS` for all three (34, 15, and 8 checks respectively).

- [ ] **Step 6: Commit**

```bash
git add tools/observation_vector/drift_location_eval.cpp tools/observation_vector/jump_ratio_eval.cpp
git commit -m "feat: wire real DEFF values into drift_location_eval and jump_ratio_eval

Per-horizon variance_inflation values from tools/block_length_and_
variance_inflation.py's real calibration run (Task 6), embedded as
named constants with the derivation cited. Both tools re-run against
the full 38,547,467-row production file with the correction active:
CIs are correctly wider than the pre-correction numbers, both ordered,
neither verdict flips (drift_location still rejected, jump_ratio still
survives at all 4 horizons) -- confirms the correction is real and
consistent with §5.0/§5.1's own framing that neither result was close
to a naively-calibrated boundary. All native suites re-verified
unchanged (34+15+8 checks)."
```

---

## Task 8: Record the corrected numbers in the brainstorm doc and SCRATCHPAD

**Files:**
- Modify: `docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md`
- Modify: `SCRATCHPAD.md`

**Interfaces:**
- Consumes: the real corrected numbers from Task 7's Step 4 output.

- [ ] **Step 1: Update the brainstorm doc**

In `docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md`:
- §10.9 ("Standing methodology gap: i.i.d. bootstrap on overlapping signals"): change its status from "not yet fixed" to fixed, citing this plan's commits and the real DEFF values derived, and note `ComputeBootstrapMeanGapCI` was deleted (no longer relevant to this section).
- §5.0 and §9 row 20 (drift/location): add the corrected hit-rate CIs from Task 7, noting the verdict (rejected) is unchanged.
- §5.1 and §9 row 21 (jump/bipower-variation ratio): add the corrected CIs from Task 7, noting the verdict (survives) is unchanged.
- Update the top freshness stamp to the final commit from Task 7.

Write the actual real numbers from Task 7's Step 4 output into these sections — do not carry forward the pre-correction numbers as if they were still current.

- [ ] **Step 2: Update SCRATCHPAD.md**

Add a new top entry (this repo's own "Where We Left Off" convention, newest first) summarizing: the dependence-correction fix, `ComputeBootstrapMeanGapCI`'s deletion, the DEFF calibration methodology, and the corrected final numbers for both candidates — mirroring the style of the existing top entry from the jump-ratio/drift-location session.

- [ ] **Step 3: Commit**

```bash
git add docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md SCRATCHPAD.md
git commit -m "docs: record the dependence-corrected CIs for drift_location and jump_ratio

Both candidates' verdicts are confirmed unchanged under the real DEFF
correction (docs/superpowers/specs/2026-08-30-bootstrap-dependence-
correction-design.md, this plan). §10.9's standing methodology gap is
now closed for both tools."
```
