# Observation-Vector Carry-Forward Completion Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Close the remaining gaps in the sentinel-collapse carry-forward sweep that fixed dims 1, 2,
3, 7, 8, 11, 12 today — four dims (10 `skewness_idx`, 13 `recurrence_rate`, 14 `fractal_dim`, 15
`mean_rev_z`) were never audited and still return a fabricated hard sentinel on routine, non-warmup
degenerate input; `StatisticalContext.relRange`'s independent sibling of dim 2 was never fixed; and
`FeatureScaler`'s D2 single-value-dominance diagnostic — spec'd but never built — would give visibility
into any future recurrence of this defect class instead of a repeat of today's silent discovery.

**Architecture:** Same pattern already reviewed and shipped for dims 1/2/3/8/11 today: replace a raw
sentinel return (`0.0f`/`1.0f`/`1.5f`) on a routine mid-session degenerate branch with a
`sc.GetPersistentFloat`-backed carry-forward of the last valid computed value. Four of the five fixes
here are direct inline edits (no new `cfc::` extraction) because their degenerate guard trivially wraps
an expensive, ACSIL-dependent computation (an O(n) or O(n²) price-window scan) rather than a small pure
formula — extracting a `cfc::` wrapper would either force the expensive computation to always run
(losing the existing early-exit performance property) or add an abstraction with nothing meaningful to
unit-test in isolation, matching how dims 4 and 12's closed-bar-only fixes were already done directly
inline today without a new header. The fifth (`ctx.relRange`) reuses the existing, already-tested
`cfc::ComputeRelativeRange` verbatim — same formula as dim 2, different consumer. The D2 diagnostic adds
a pure, natively-testable `FeatureScaler::ComputeValueDominance()` and a `dominanceRatio` member array;
logging happens at the `ContextManager.cpp` call site (`FeatureScaler.h` has zero Logger/ACSIL
dependency by design, and must keep it for its own native test suite to keep working).

**Tech Stack:** C++17, ACSIL (Sierra Chart Studies Interface), `sc.GetPersistentFloat` carry-forward
state (established pattern, `PersistentVar_AdaptiveCalculators` namespace), manual `g++`-compiled
native test binaries (no gtest in this repo).

**Spec:** `docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md` (D1 pattern
this plan extends to the 4 missed dims, D2 diagnostic this plan implements) and
`docs/superpowers/plans/2026-08-12-statistical-context-relrange-sentinel-gap.md` (Problem 1, the
`ctx.relRange` fix this plan implements — Problem 2 in that doc, dims 1/2's still-elevated zero-ratio,
stays investigation-first and is explicitly **out of scope** here per that doc's own task list).

## Global Constraints

- Zero behavior change beyond the specific degenerate-branch bug being fixed — every non-degenerate
  code path's output must be bit-identical to today's.
- No heap allocations in the hot path (CLAUDE.md Performance Rules) — all fixes here are stack-only
  (`sc.GetPersistentFloat` references, no containers).
- `FeatureScaler.h` must remain natively testable with zero Sierra Chart/ACSIL/Logger dependency — the
  D2 diagnostic's logging happens at the `ContextManager.cpp` call site, never inside the header.
- Preserve each fixed function's existing early-exit performance property (skip the expensive
  computation on the degenerate path) — do not force a formula to always run just to fit a `cfc::`
  wrapper shape.
- Build must go through `./build_dll.sh` — never raw `cmake`/`ninja`/`flatc`.
- Dims 1/2's still-open zero-ratio investigation (`statistical-context-relrange-sentinel-gap.md`
  Problem 2) is out of scope — no fix without a confirmed root cause, per that doc's own task list and
  `superpowers:systematic-debugging`.

---

## File Structure

- **Modify:** `include/MindfulTraderConstants.h` — 5 new persistent-var slots (33-37).
- **Modify:** `src/StudyHelperFunctions.cpp` — `CalculateSkewness` (dim 10), `CalculateRecurrenceRate`
  (dim 13), `CalculateFractalDimension` (dim 14), `CalculateMeanReversionSpeed` (dim 15): carry-forward
  on their degenerate branches.
- **Modify:** `src/TripleScreen2.cpp` — `ctx.relRange`'s degenerate branch (lines ~768-774).
- **Modify:** `include/FeatureScaler.h` — `ComputeValueDominance()` static pure function,
  `dominanceRatio` member array, populated in `Calibrate()`/`Recalibrate()`.
- **Modify:** `tests/cpp/test_feature_scaler.cpp` — native tests for `ComputeValueDominance()`.
- **Modify:** `src/ContextManager.cpp` — log the D2 diagnostic after `UpdateAndNormalize()`.

---

### Task 1: Dim 10 (`skewness_idx`) — carry-forward on degenerate variance

**Files:**
- Modify: `include/MindfulTraderConstants.h`
- Modify: `src/StudyHelperFunctions.cpp:2706-2764` (`CalculateSkewness`)

**Interfaces:** None new — direct inline fix, no new function.

- [ ] **Step 1: Add the persistent-var slot**

In `include/MindfulTraderConstants.h`, extend `PersistentVar_AdaptiveCalculators` (currently ends at
line 92 with `CORRECTION_ACTION_LAST_VALID_VALUE = 32;`):

```cpp
    const int SKEWNESS_LAST_VALID_VALUE = 33; // Last valid skewness_idx for graceful degradation on a near-zero 100-bar return variance
```

- [ ] **Step 2: Rewrite `CalculateSkewness`**

Replace the full function body (currently `src/StudyHelperFunctions.cpp:2706-2764`, keep the leading
doc-comment):

```cpp
float CalculateSkewness(SCStudyInterfaceRef sc, SCFloatArrayRef atrArray) {
    /// Skewness - ELITE: Regime-Adjusted Implementation #4
    /// Detects distribution asymmetry: (-) = left tail crashes, (+) = right tail rallies
    ///
    /// Institutional Regime Adjustment:
    ///   Normal volatility: skewness as-is (baseline)
    ///   High-vol trending (>1.2x avg): amplify x1.3 (rallies steeper, crashes sharp)
    ///   Low-vol ranging (<0.8x avg): dampen x0.8 (noise creates spurious asymmetry)
    constexpr int SKEW_WINDOW = 100;
    if (sc.Index < SKEW_WINDOW) return 0.0f;

    std::array<float, SKEW_WINDOW> returns{};
    for (int i = 0; i < SKEW_WINDOW; ++i) {
        returns[static_cast<size_t>(i)] = std::log(sc.Close[sc.Index - i] / std::max(sc.Close[sc.Index - i - 1], 0.001f));
    }

    float mean_ret = 0.0f;
    for (float r : returns) mean_ret += r;
    mean_ret /= SKEW_WINDOW;

    float variance = 0.0f;
    for (float ret : returns) {
        float diff = ret - mean_ret;
        variance += diff * diff;
    }
    variance /= SKEW_WINDOW;

    float& lastValidSkewness = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::SKEWNESS_LAST_VALID_VALUE);

    // ES 15s log-return variance is typically very small; avoid collapsing to zero.
    // Degenerate (near-flat return window) carries the last valid value forward
    // instead of a fabricated exact-zero "no skew" reading -- returning before
    // the regime-adjustment block below means a carried-forward value is never
    // re-multiplied by a fresh regime factor -- same sentinel-collapse fix
    // already applied to dims 1/2/3/7/8/11/12
    // (docs/superpowers/plans/2026-08-12-observation-vector-carry-forward-completion.md).
    constexpr float SKEW_VARIANCE_EPS = 1e-10f;
    if (variance < SKEW_VARIANCE_EPS) {
        return lastValidSkewness;
    }
    float stddev = std::sqrt(variance);

    float m3 = 0.0f;
    for (float ret : returns) {
        float diff = ret - mean_ret;
        m3 += diff * diff * diff;
    }
    m3 /= SKEW_WINDOW;

    float skewness = m3 / (stddev * stddev * stddev);

    // ELITE FIX #4: Regime adjustment (Wyckoff-aligned)
    float atrCurrent = atrArray[sc.Index];
    constexpr int VOL_COMPARE_WINDOW = 20;
    if (sc.Index >= VOL_COMPARE_WINDOW) {
        float atrAvg = 0.0f;
        for (int i = 0; i < VOL_COMPARE_WINDOW; ++i) {
            atrAvg += atrArray[sc.Index - i];
        }
        atrAvg /= VOL_COMPARE_WINDOW;
        float vol_ratio = atrCurrent / std::max(atrAvg, 0.0001f);
        float regime_mult = 1.0f;
        if (vol_ratio > 1.2f) regime_mult = 1.30f;
        if (vol_ratio < 0.8f) regime_mult = 0.80f;
        skewness *= regime_mult;
    }

    skewness = std::clamp(skewness, -1.5f, 1.5f);
    lastValidSkewness = skewness;
    return skewness;
}
```

- [ ] **Step 3: Build**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds. No isolated unit test here — `CalculateSkewness` takes `SCStudyInterfaceRef`
and can't be driven without ACSIL, same as dims 4/12's direct fixes today.

- [ ] **Step 4: Commit**

```bash
git add include/MindfulTraderConstants.h src/StudyHelperFunctions.cpp
git commit -m "fix(microstructure): carry-forward for skewness_idx on degenerate variance (dim 10)"
```

---

### Task 2: Dim 13 (`recurrence_rate`) — carry-forward on degenerate range

**Files:**
- Modify: `include/MindfulTraderConstants.h`
- Modify: `src/StudyHelperFunctions.cpp:3279-3330` (`CalculateRecurrenceRate`)

**Interfaces:** None new — direct inline fix, no new function.

- [ ] **Step 1: Add the persistent-var slot**

In `include/MindfulTraderConstants.h`, add after `SKEWNESS_LAST_VALID_VALUE` (from Task 1):

```cpp
    const int RECURRENCE_RATE_LAST_VALID_VALUE = 34; // Last valid recurrence_rate for graceful degradation on a flat price window
```

- [ ] **Step 2: Rewrite `CalculateRecurrenceRate`**

Replace the full function body (currently `src/StudyHelperFunctions.cpp:3279-3330`):

```cpp
float CalculateRecurrenceRate(SCStudyInterfaceRef sc, int lookback_n) {
    if (sc.Index < lookback_n) return 0.0f;

    // RQA (Recurrence Quantification Analysis) Recurrence Rate
    // RR = (1 / N^2) * Sum(Theta(epsilon - dist(i,j)))
    // Where Theta is Heaviside step fun.
    // Epsilon = 10% of range or 0.5 * StdDev

    float minP = FLT_MAX, maxP = -FLT_MAX;
    for(int i=0; i<lookback_n; i++) {
        float p = sc.BaseData[SC_LAST][sc.Index - i];
        if (p < minP) minP = p;
        if (p > maxP) maxP = p;
    }

    float range = maxP - minP;
    float& lastValidRecurrenceRate = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::RECURRENCE_RATE_LAST_VALID_VALUE);
    // Degenerate (flat price window) carries the last valid value forward
    // instead of a fabricated "1.0 = 100% recurrence" reading -- checked before
    // the O(n^2) distance-matrix loop below, so the expensive computation is
    // still skipped on the degenerate path exactly as before -- same
    // sentinel-collapse fix already applied to dims 1/2/3/7/8/11/12.
    if (range <= 0.00001f) {
        return lastValidRecurrenceRate;
    }

    // Adaptive tolerance: mix range-based and variance-based scales.
    float meanP = 0.0f;
    for (int i = 0; i < lookback_n; ++i) {
        meanP += sc.BaseData[SC_LAST][sc.Index - i];
    }
    meanP /= static_cast<float>(lookback_n);

    float varP = 0.0f;
    for (int i = 0; i < lookback_n; ++i) {
        const float d = sc.BaseData[SC_LAST][sc.Index - i] - meanP;
        varP += d * d;
    }
    varP /= static_cast<float>(lookback_n);
    const float stdP = std::sqrt(std::max(varP, 0.0f));

    float epsilon = std::max(range * 0.1f, stdP * 0.5f);
    epsilon = std::max(epsilon, 1e-6f);
    int recurCount = 0;
    int totalCount = lookback_n * lookback_n;

    // Calculate distance matrix and count pairs within epsilon
    // Optimization: Since distance is symmetric, compute i > j and double, plus diagonal (i=j, always 0 < eps)
    // Diagonal count = N.
    recurCount += lookback_n;

    for(int i=0; i<lookback_n; i++) {
        float p1 = sc.BaseData[SC_LAST][sc.Index - i];
        for(int j=i+1; j<lookback_n; j++) {
            float p2 = sc.BaseData[SC_LAST][sc.Index - j];
            if (std::abs(p1 - p2) < epsilon) {
                recurCount += 2; // Add both (i,j) and (j,i)
            }
        }
    }

    const float recurrenceRate = std::clamp((float)recurCount / (float)totalCount, 0.0f, 1.0f);
    lastValidRecurrenceRate = recurrenceRate;
    return recurrenceRate;
}
```

- [ ] **Step 3: Build**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add include/MindfulTraderConstants.h src/StudyHelperFunctions.cpp
git commit -m "fix(microstructure): carry-forward for recurrence_rate on flat price window (dim 13)"
```

---

### Task 3: Dim 14 (`fractal_dim`) — carry-forward on degenerate price range

**Files:**
- Modify: `include/MindfulTraderConstants.h`
- Modify: `src/StudyHelperFunctions.cpp:3122-3163` (`CalculateFractalDimension`)

**Interfaces:** None new — direct inline fix, no new function.

- [ ] **Step 1: Add the persistent-var slot**

In `include/MindfulTraderConstants.h`, add after `RECURRENCE_RATE_LAST_VALID_VALUE` (from Task 2):

```cpp
    const int FRACTAL_DIM_LAST_VALID_VALUE = 35; // Last valid fractal_dim for graceful degradation on a flat price window
```

- [ ] **Step 2: Rewrite `CalculateFractalDimension`**

Replace the full function body (currently `src/StudyHelperFunctions.cpp:3122-3163`):

```cpp
float CalculateFractalDimension(SCStudyInterfaceRef sc, int lookback_n) {
    // Sevcik Fractal Dimension Approximation
    // D = 1 + ln(L) / ln(2*N)
    // L = Sum of euclidean distances between normalized points

    if (sc.Index < lookback_n) return 1.5f; // Brownian guess -- true cold-start, no prior value exists yet

    float minP = FLT_MAX, maxP = -FLT_MAX;
    for(int i=0; i<lookback_n; i++) {
        float p = sc.BaseData[SC_LAST][sc.Index - i];
        if(p < minP) minP = p;
        if(p > maxP) maxP = p;
    }

    float& lastValidFractalDim = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::FRACTAL_DIM_LAST_VALID_VALUE);

    // Degenerate (flat price window) carries the last valid value forward
    // instead of a fabricated "1.0 = flat line" reading -- checked before the
    // path-length scan below, so the expensive computation is still skipped on
    // the degenerate path exactly as before -- same sentinel-collapse fix
    // already applied to dims 1/2/3/7/8/11/12.
    if (maxP <= minP) return lastValidFractalDim;

    const int segments = lookback_n - 1;
    if (segments <= 0) return 1.5f; // Unreachable in practice (lookback_n always >=30) -- defensive, true cold-start shape

    double length = 0.0;
    double priceRange = maxP - minP;

    for(int i=1; i<lookback_n; i++) {
        int idx = sc.Index - lookback_n + i; // Moving forward from start of window
        float p1 = sc.BaseData[SC_LAST][idx-1];
        float p2 = sc.BaseData[SC_LAST][idx];

        // Normalized coordinates (Time on X [0..1], Price on Y [0..1])
        // Use segment count (N-1), not point count (N), to avoid discretization bias.
        double dy = (p2 - p1) / priceRange;
        double dx = 1.0 / static_cast<double>(segments);

        length += std::sqrt(dx*dx + dy*dy);
    }

    // Degenerate (zero-length path -- should be unreachable once maxP>minP is
    // established above; kept as a defensive carry-forward, not a fresh sentinel).
    if (length <= 0.0) {
        return lastValidFractalDim;
    }
    const float dim = static_cast<float>(
        1.0 + std::log(length) / std::log(2.0 * static_cast<double>(segments)));
    const float fractalDim = std::clamp(dim, 1.0f, 2.0f);
    lastValidFractalDim = fractalDim;
    return fractalDim;
}
```

- [ ] **Step 3: Build**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add include/MindfulTraderConstants.h src/StudyHelperFunctions.cpp
git commit -m "fix(microstructure): carry-forward for fractal_dim on flat price window (dim 14)"
```

---

### Task 4: Dim 15 (`mean_rev_z`) — carry-forward on degenerate price std-dev

**Files:**
- Modify: `include/MindfulTraderConstants.h`
- Modify: `src/StudyHelperFunctions.cpp:3165-3230` (`CalculateMeanReversionSpeed`)

**Interfaces:** None new — direct inline fix, no new function.

- [ ] **Step 1: Add the persistent-var slot**

In `include/MindfulTraderConstants.h`, add after `FRACTAL_DIM_LAST_VALID_VALUE` (from Task 3):

```cpp
    const int MEAN_REV_Z_LAST_VALID_VALUE = 36; // Last valid mean_rev_z for graceful degradation on a near-zero log-price std-dev
```

- [ ] **Step 2: Rewrite `CalculateMeanReversionSpeed`**

Replace the full function body (currently `src/StudyHelperFunctions.cpp:3165-3230`, keep the leading
doc-comment):

```cpp
float CalculateMeanReversionSpeed(SCStudyInterfaceRef sc, int lookback_n) {
    // Mean-Reversion Elasticity Score (contract: mean_rev_z in [0, 5]).
    // 1) Compute price stretch as |z(log-price)| over lookback window.
    // 2) Suppress score in momentum regimes using lag-1 return autocorrelation.

    // kMaxLookback matches the [10,40] adaptive observation window contract
    // this function's one caller (TripleScreen3.cpp) always passes today
    // (CalculateAdaptiveObservationWindow's own std::clamp(..., 10, 40)) --
    // defensive upper bound so a fixed-capacity scratch buffer below can
    // never be written out of range if a future caller passes something larger.
    constexpr int kMaxLookback = 40;
    const int n = std::clamp(lookback_n, 5, kMaxLookback);
    if (sc.Index < (n + 1)) return 0.0f; // True cold-start, no prior value exists yet

    constexpr double kPriceEps = 1e-6;
    const int start_idx = sc.Index - n + 1;

    // Price z-score from log-price window.
    double sum_log_p = 0.0;
    double sum_log_p_sq = 0.0;
    for (int i = 0; i < n; ++i) {
        const double p = std::max(static_cast<double>(sc.BaseData[SC_LAST][start_idx + i]), kPriceEps);
        const double lp = std::log(p);
        sum_log_p += lp;
        sum_log_p_sq += lp * lp;
    }

    const double mean_log_p = sum_log_p / static_cast<double>(n);
    const double var_log_p = std::max((sum_log_p_sq / static_cast<double>(n)) - (mean_log_p * mean_log_p), 0.0);
    const double std_log_p = std::sqrt(var_log_p);

    float& lastValidMeanRevZ = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::MEAN_REV_Z_LAST_VALID_VALUE);
    // Degenerate (flat price window) carries the last valid value forward
    // instead of a fabricated exact-zero "no stretch" reading -- same
    // sentinel-collapse fix already applied to dims 1/2/3/7/8/11/12.
    if (std_log_p < 1e-6) {
        return lastValidMeanRevZ;
    }

    const double current_log_p = std::log(std::max(static_cast<double>(sc.BaseData[SC_LAST][sc.Index]), kPriceEps));
    const double abs_z_price = std::abs((current_log_p - mean_log_p) / std_log_p);

    // Lag-1 autocorrelation on log-returns: positive rho => momentum, negative => reversion.
    const int m = n - 1;
    if (m < 3) {
        // Genuinely computed (not degenerate) -- too few samples for the
        // autocorrelation term, so it's skipped, not faked. Still updates the
        // carry-forward state so a later degenerate call has a real value to
        // fall back on.
        const float score = std::clamp(static_cast<float>(abs_z_price), 0.0f, 5.0f);
        lastValidMeanRevZ = score;
        return score;
    }

    double sum_r = 0.0;
    std::array<double, kMaxLookback> returns{};  // m <= n-1 < kMaxLookback, always in range
    for (int i = 0; i < m; ++i) {
        const int idx = start_idx + i + 1;
        const double p = std::max(static_cast<double>(sc.BaseData[SC_LAST][idx]), kPriceEps);
        const double p_prev = std::max(static_cast<double>(sc.BaseData[SC_LAST][idx - 1]), kPriceEps);
        const double r = std::log(p / p_prev);
        returns[static_cast<size_t>(i)] = r;
        sum_r += r;
    }

    const double mean_r = sum_r / static_cast<double>(m);
    double num = 0.0;
    double den = 0.0;
    for (int t = 1; t < m; ++t) {
        const double r_t = returns[t] - mean_r;
        const double r_prev = returns[t - 1] - mean_r;
        num += r_t * r_prev;
        den += r_prev * r_prev;
    }

    const double rho = (den > 1e-12) ? (num / den) : 0.0;
    const double elasticity_gate = std::clamp(1.0 - std::max(rho, 0.0), 0.0, 1.0);
    const double score = abs_z_price * elasticity_gate;

    const float meanRevZ = std::clamp(static_cast<float>(score), 0.0f, 5.0f);
    lastValidMeanRevZ = meanRevZ;
    return meanRevZ;
}
```

- [ ] **Step 3: Build**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add include/MindfulTraderConstants.h src/StudyHelperFunctions.cpp
git commit -m "fix(microstructure): carry-forward for mean_rev_z on degenerate price std-dev (dim 15)"
```

---

### Task 5: `StatisticalContext.relRange` — fix via the existing `cfc::ComputeRelativeRange`

**Files:**
- Modify: `include/MindfulTraderConstants.h`
- Modify: `src/TripleScreen2.cpp:768-774`

**Interfaces:**
- Consumes: `cfc::ComputeRelativeRange(float, float, float, float) -> float` (already exists, already
  tested, `include/CarryForwardCalculators.h` — same formula already used for dim 2 at
  `TripleScreen2.cpp:273`).

Per `docs/superpowers/plans/2026-08-12-statistical-context-relrange-sentinel-gap.md` Problem 1: this is
a second, independent `relRange` computation (`ctx.relRange`, feeding `StatisticalContext` →
`ContextManager`'s ripple context → `TrainingEvent.rel_range`) with the identical degenerate-ATR bug
dim 2 was already fixed for. Uses its own persistent slot per that doc's explicit instruction — do not
reuse `RELATIVE_RANGE_LAST_VALID_VALUE`, which is dim 2's own independent carry-forward state.

- [ ] **Step 1: Add the persistent-var slot**

In `include/MindfulTraderConstants.h`, add after `MEAN_REV_Z_LAST_VALID_VALUE` (from Task 4):

```cpp
    const int CTX_REL_RANGE_LAST_VALID_VALUE = 37; // Last valid StatisticalContext.relRange for graceful degradation on a zero/unpopulated ATR (independent of dim 2's own carry-forward state)
```

- [ ] **Step 2: Rewrite the `ctx.relRange` block**

In `src/TripleScreen2.cpp`, replace (currently lines 768-774):

```cpp
        // 3. Relative Range: Current bar range normalized by ATR (already O(1))
        if (Array_AtrKeltner[sc.Index] > 0.0f) {
            float barRange = sc.High[sc.Index] - sc.Low[sc.Index];
            ctx.relRange = barRange / Array_AtrKeltner[sc.Index];
        } else {
            ctx.relRange = 0.0f;
        }
```

with:

```cpp
        // 3. Relative Range: Current bar range normalized by ATR (already O(1))
        // Degenerate (unpopulated/zero ATR) carries the last valid value forward
        // instead of a fabricated exact-zero reading -- this is
        // StatisticalContext's independent sibling of dim 2's own
        // relative_range fix (TripleScreen2.cpp:273-274), feeding
        // TrainingEvent.rel_range, not the ObservationData vector
        // (docs/superpowers/plans/2026-08-12-statistical-context-relrange-sentinel-gap.md).
        float& lastValidCtxRelRange = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::CTX_REL_RANGE_LAST_VALID_VALUE);
        ctx.relRange = cfc::ComputeRelativeRange(sc.High[sc.Index], sc.Low[sc.Index], Array_AtrKeltner[sc.Index], lastValidCtxRelRange);
        lastValidCtxRelRange = ctx.relRange;
```

- [ ] **Step 3: Build**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds. `cfc::` is already included in this file (dim 2's fix at line 273 already
uses it) — no new `#include` needed.

- [ ] **Step 4: Commit**

```bash
git add include/MindfulTraderConstants.h src/TripleScreen2.cpp
git commit -m "fix(context-manager): carry-forward for StatisticalContext.relRange sibling (dim-2-adjacent)"
```

---

### Task 6: `FeatureScaler` D2 — single-value-dominance diagnostic

**Files:**
- Modify: `include/FeatureScaler.h`
- Modify: `tests/cpp/test_feature_scaler.cpp`
- Modify: `src/ContextManager.cpp`

**Interfaces:**
- Produces: `FeatureScaler::ComputeValueDominance(const RingBuffer<float, RANK_WINDOW+1>&) -> float`
  (pure, static), `FeatureScaler::dominanceRatio` (public member, `std::array<float, N_DIMS>`,
  populated by `Calibrate()`/`Recalibrate()`).

Per `docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md` D2: a log-only
diagnostic that flags when a single raw value occupies more than 30% of a dim's rolling window — the
exact signature of a sentinel-collapse (or, per this session's live-data finding on dims 1/2, a
carry-forward-then-median-collapse). `FeatureScaler.h` has zero Sierra Chart/ACSIL/Logger dependency by
design (`tests/cpp/test_feature_scaler.cpp` depends on that) — the diagnostic computation lives in the
header; the actual `Logger` call lives in `ContextManager.cpp`, which already has both.

- [ ] **Step 1: Write the failing test**

Add to `tests/cpp/test_feature_scaler.cpp`, in the `main()` body before the final `printf`/`return`
(the file already includes `FeatureScaler.h` and has the `check()`/`approx()` helpers this reuses):

```cpp
    // --- ComputeValueDominance (D2 sentinel-collapse diagnostic) ---
    {
        RingBuffer<float, FeatureScaler::RANK_WINDOW + 1> buf;
        for (int i = 0; i < 10; ++i) buf.push_back(1.0f);
        check("dominance_all_identical_is_one",
              approx(FeatureScaler::ComputeValueDominance(buf), 1.0f, 1e-6f));
    }
    {
        RingBuffer<float, FeatureScaler::RANK_WINDOW + 1> buf;
        for (int i = 0; i < 10; ++i) buf.push_back(static_cast<float>(i));
        check("dominance_all_distinct_is_one_over_n",
              approx(FeatureScaler::ComputeValueDominance(buf), 0.1f, 1e-6f));
    }
    {
        RingBuffer<float, FeatureScaler::RANK_WINDOW + 1> buf;
        for (int i = 0; i < 6; ++i) buf.push_back(0.0f);   // 6/10 = 60% dominance
        for (int i = 0; i < 4; ++i) buf.push_back(static_cast<float>(i + 1));
        check("dominance_repeated_sentinel_matches_expected_ratio",
              approx(FeatureScaler::ComputeValueDominance(buf), 0.6f, 1e-6f));
    }
    {
        RingBuffer<float, FeatureScaler::RANK_WINDOW + 1> buf;  // empty
        check("dominance_empty_buffer_is_zero",
              approx(FeatureScaler::ComputeValueDominance(buf), 0.0f, 1e-6f));
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -I include -I include/generated tests/cpp/test_feature_scaler.cpp -o /tmp/fs_test`
Expected: FAIL — `error: 'ComputeValueDominance' is not a member of 'FeatureScaler'`

- [ ] **Step 3: Add `dominanceRatio` member and `ComputeValueDominance()`**

In `include/FeatureScaler.h`, add immediately after the `calibration` member declaration (currently
line 157, `std::array<DimCalibration, N_DIMS> calibration = {};`):

```cpp
    /// Per-dim result of the last Calibrate()/Recalibrate() dominance check:
    /// fraction of the rolling window occupied by its single most-frequent
    /// exact value. Exposed (not logged here -- this header has zero
    /// ACSIL/Logger dependency by design) so a caller with Logger access can
    /// flag a sentinel-collapse signature
    /// (docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md D2).
    std::array<float, N_DIMS> dominanceRatio = {};
```

Add the static function immediately after `RobustLocation()` (currently ends at line 216):

```cpp
    /// Fraction of `buf` occupied by its single most-frequent exact value.
    /// O(n^2) on up to RANK_WINDOW (500) samples -- only called from
    /// Calibrate()/Recalibrate() (warmup completion + every
    /// RECALIBRATION_INTERVAL samples), never per-tick. Pure, native-testable.
    static float ComputeValueDominance(const RingBuffer<float, RANK_WINDOW + 1>& buf) {
        const int n = static_cast<int>(buf.size());
        if (n == 0) return 0.0f;
        int maxCount = 0;
        for (int j = 0; j < n; ++j) {
            int count = 0;
            for (int k = 0; k < n; ++k) {
                if (buf[static_cast<size_t>(k)] == buf[static_cast<size_t>(j)]) ++count;
            }
            maxCount = std::max(maxCount, count);
        }
        return static_cast<float>(maxCount) / static_cast<float>(n);
    }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -I include -I include/generated tests/cpp/test_feature_scaler.cpp -o /tmp/fs_test && /tmp/fs_test`
Expected: exit 0, all 4 new checks `PASS`, and every pre-existing check in this file still `PASS`
(this task doesn't touch `UpdateAndNormalize`/`RobustLocation`/anything the existing golden-file
characterization covers).

- [ ] **Step 5: Wire `dominanceRatio` into `Calibrate()`/`Recalibrate()`**

In `include/FeatureScaler.h`, in `Calibrate()` (currently lines 375-393), add one line to each branch:

```cpp
    void Calibrate() {
        for (size_t i = 0; i < N_DIMS; ++i) {
            float bufScale = 0.0f;
            if (SCALE_MODE_MAP[i] == ScaleMode::SOFTLOGZ) {
                const auto& buf = stateBuffers[i];
                if (buf.size() >= 5) {
                    bufScale = RobustLocation(buf).second;
                }
                dominanceRatio[i] = ComputeValueDominance(buf);
            } else {
                const auto& buf = logBuffers[i];
                if (buf.size() >= 5) {
                    bufScale = RobustLocation(buf).second;
                }
                dominanceRatio[i] = ComputeValueDominance(buf);
            }
            const float floorToUse = (i == DIM_AMIHUD_INDEX) ? AMIHUD_ABSOLUTE_FLOOR : ABSOLUTE_FLOOR;
            calibration[i].minScaleFloor = std::max(floorToUse, bufScale * RELATIVE_FLOOR_FRACTION);
        }
        calibrated = true;
    }
```

Apply the identical two-line addition (`dominanceRatio[i] = ComputeValueDominance(buf);` in each
branch) to `Recalibrate()` (currently lines 398-417), which has the exact same per-branch structure.

- [ ] **Step 6: Run the full native test suite again**

Run: `g++ -std=c++17 -I include -I include/generated tests/cpp/test_feature_scaler.cpp -o /tmp/fs_test && /tmp/fs_test`
Expected: exit 0, all checks `PASS` — confirms wiring `dominanceRatio` into `Calibrate()`/`Recalibrate()`
didn't disturb their existing `minScaleFloor` output (this task's diagnostic is additive-only).

- [ ] **Step 7: Log the diagnostic in `ContextManager.cpp`**

In `src/ContextManager.cpp`, immediately after the call to `UpdateAndNormalize()` (currently line 1220,
`auto currentObs = m_featureScaler.UpdateAndNormalize(rawObs);`), add:

```cpp
    auto currentObs = m_featureScaler.UpdateAndNormalize(rawObs);

    // D2: sentinel-collapse diagnostic (log-only, no behavior change) -- flag
    // any dim whose rolling window is >30% dominated by a single repeated
    // value, the exact signature of a sentinel-collapse or a
    // carry-forward-then-median-collapse (this session's live-data finding on
    // dims 1/2 -- docs/superpowers/plans/2026-08-12-statistical-context-relrange-sentinel-gap.md
    // Problem 2). Gated to the same cadence FeatureScaler itself refreshes
    // dominanceRatio (warmup completion + every RECALIBRATION_INTERVAL
    // samples) so this doesn't repeat the same finding every tick.
    if (m_featureScaler.calibrated &&
        (m_featureScaler.sampleCount == FeatureScaler::RANK_WINDOW ||
         (m_featureScaler.sampleCount % FeatureScaler::RECALIBRATION_INTERVAL) == 0)) {
        for (size_t i = 0; i < FeatureScaler::N_DIMS; ++i) {
            if (m_featureScaler.dominanceRatio[i] > 0.30f) {
                Logger::getInstance().log(
                    "FeatureScaler dominance ALERT dim=" + std::to_string(i) +
                    " ratio=" + std::to_string(m_featureScaler.dominanceRatio[i]) +
                    " sampleCount=" + std::to_string(m_featureScaler.sampleCount)
                );
            }
        }
    }
```

- [ ] **Step 8: Build**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds.

- [ ] **Step 9: Commit**

```bash
git add include/FeatureScaler.h tests/cpp/test_feature_scaler.cpp src/ContextManager.cpp
git commit -m "feat(context-manager): add FeatureScaler D2 single-value-dominance diagnostic"
```

---

### Task 7: Full clean build + full regression suite

**Files:** None new — verification only.

- [ ] **Step 1: Full clean build**

Run: `./build_dll.sh`
Expected: build succeeds with no errors.

- [ ] **Step 2: Run every native test suite**

```bash
g++ -std=c++17 -I include tests/cpp/test_ring_buffer.cpp -o /tmp/rb_test && /tmp/rb_test
g++ -std=c++17 -I include tests/cpp/test_carry_forward_calculators.cpp -o /tmp/cfc_test && /tmp/cfc_test
g++ -std=c++17 -I include tests/cpp/test_order_flow_asymmetry_engine.cpp -o /tmp/ofae_test && /tmp/ofae_test
g++ -std=c++17 -I include -I include/generated tests/cpp/test_feature_scaler.cpp -o /tmp/fs_test && /tmp/fs_test
```

Expected: all four suites exit 0, zero failures. The first three are regression checks — this plan
doesn't touch `RingBuffer.h`, `CarryForwardCalculators.h`'s existing functions, or
`OrderFlowAsymmetryEngine.h`, so their existing tests must still pass unchanged.

- [ ] **Step 3: Update `docs/PENDING_USER_ACTIONS.md`**

Append a new numbered section recording this batch as pending the next Sierra Chart deploy + replay
re-verification, matching this repo's established convention (see the existing dims-1/2/3/4/7/8/11/12
section for the format: bundled commits, deploy steps, per-dim acceptance criteria). Acceptance
criteria for this batch: dims 10/13/14/15's `ObservationStaleness` alerts (if any) should show carried
values changing at legitimate degenerate-run boundaries, not a single frozen sentinel; the new
`FeatureScaler dominance ALERT` log line should be checked first for dims 1/2 (Problem 2's still-open
investigation) to see whether it fires and at what ratio, before that investigation writes any fix.

- [ ] **Step 4: Commit**

```bash
git add docs/PENDING_USER_ACTIONS.md
git commit -m "docs: record carry-forward completion batch pending deploy + record D2 diagnostic follow-up"
```
