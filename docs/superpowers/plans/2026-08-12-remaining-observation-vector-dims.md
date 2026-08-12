# Remaining 16D Observation Vector Dims Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Resolve every dim flagged as "still needs resolving" per the current status audit
(`docs/PENDING_USER_ACTIONS.md` §6, live `ObservationStaleness` log evidence) — dims 3, 11, 12
with concrete, ready-to-implement fixes; dims 4 and 9 with a scoped root-cause investigation
(systematic-debugging: root cause before fix). Dim 10 (skewness) is explicitly **out of scope** —
it's a research decision (`pending-replacement`, Kim & White robust estimator), not an implementation
task, and shouldn't be rushed to match this plan's pace.

**Architecture:** Three of these five dims (7 already fixed, 11, 12) share one root-cause family
already confirmed once this session: a calculator inside `UpdateObservationVectorSubgraphs`'s
once-per-bar gate reads an ACSIL array that accumulates *throughout the still-forming bar*
(`sc.Volume`, `sc.BidVolume`, `sc.AskVolume`) at the bar's first tick, when it's near its minimum.
Dim 7's fix moved the read to per-tick cadence (safe there because its formula is a stateless ratio).
Dims 11 and 12 are NOT stateless in the same way — dim 11 sums over a multi-bar window, dim 12 threads
an EMA-smoothed `prev_fragility` bar-to-bar — so their fix is different in shape: **stop reading the
live, current bar's value at all; read the last fully-closed bar's value instead** (shift the index by
one). This preserves each function's intended bar-level cadence/smoothing exactly, just fixes *which*
bar's data feeds the accumulating-array term.

## Global Constraints

- Zero behavior change beyond the specific bug being fixed — don't touch working code.
- Dim 10 is out of scope for this plan (see Goal).
- Dims 4 and 9's tasks are investigation-first: no fix code without a demonstrated root cause,
  per `superpowers:systematic-debugging`. It is a valid, honest outcome for either investigation to
  conclude "no bug found, this looks organic" — document that and stop, don't manufacture a fix.
- Build must go through `./build_dll.sh` — never raw `cmake`/`ninja`/`flatc`.
- None of this plan's tasks include a deploy step — batch the deploy after every task here (and the
  separately-tracked dims 1/2/7/8 fixes) land, per the earlier decision to avoid repeated disruptive
  redeploys of the live collection.

---

## File Structure

- **Modify:** `include/CarryForwardCalculators.h` — new `ComputeAmihudIlliquidity` pure function.
- **Modify:** `tests/cpp/test_carry_forward_calculators.cpp` — tests for the above.
- **Modify:** `include/MindfulTraderConstants.h` — two new persistent-var slots (Amihud, correction_action).
- **Modify:** `src/StudyHelperFunctions.cpp` — `CalculateAmihudIlliquidity` (dim 11),
  `CalculateRealizedVarianceRatio` (dim 3), `CalculateLiquidityFragility` (dim 12).
- **Modify:** `docs/PENDING_USER_ACTIONS.md` §6 — close out once dim 11's fix lands.

---

### Task 1: Dim 11 (`amihud_illiquidity`) — carry-forward + closed-bar-only window

**Files:**
- Modify: `include/CarryForwardCalculators.h`
- Modify: `tests/cpp/test_carry_forward_calculators.cpp`
- Modify: `include/MindfulTraderConstants.h`
- Modify: `src/StudyHelperFunctions.cpp:3042-3068` (`CalculateAmihudIlliquidity`)

**Interfaces:**
- Produces: `cfc::ComputeAmihudIlliquidity(double sum, int count, float lastValidValue) -> float`.

- [ ] **Step 1: Write the failing test**

Add to `tests/cpp/test_carry_forward_calculators.cpp`, in the `main()` body after the existing
Fisher-info checks (before the final `printf`/`return`):

```cpp
    // --- ComputeAmihudIlliquidity (dim 11) ---
    check("amihud_normal_case_is_mean",
          approx(cfc::ComputeAmihudIlliquidity(0.006, 3, 0.0f), 0.006 / 3.0));

    check("amihud_degenerate_count_carries_forward",
          approx(cfc::ComputeAmihudIlliquidity(0.006, 1, 0.42f), 0.42f));

    check("amihud_degenerate_zero_count_carries_forward",
          approx(cfc::ComputeAmihudIlliquidity(0.0, 0, 0.42f), 0.42f));

    check("amihud_degenerate_no_prior_value_returns_neutral",
          approx(cfc::ComputeAmihudIlliquidity(0.006, 1, 0.0f), 0.0f));
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -I include tests/cpp/test_carry_forward_calculators.cpp -o /tmp/cfc_test`
Expected: FAIL — `error: no member named 'ComputeAmihudIlliquidity' in namespace 'cfc'`

- [ ] **Step 3: Add the pure function**

In `include/CarryForwardCalculators.h`, add after `ComputeFisherInformation` (before the closing
`}  // namespace cfc`):

```cpp
// Dim 11 (amihud_illiquidity): mean(|log-return|/dollar-volume) over the
// valid samples in the lookback window. Degenerate when fewer than 2 valid
// samples were found (thin/illiquid lookback) — carries the last valid
// value forward instead of a fabricated exact-zero "perfectly liquid"
// reading.
inline float ComputeAmihudIlliquidity(double sum, int count, float lastValidValue) {
    if (count < 2) {
        return lastValidValue;
    }
    return static_cast<float>(sum / count);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -I include tests/cpp/test_carry_forward_calculators.cpp -o /tmp/cfc_test && /tmp/cfc_test`
Expected: `0 failure(s)`.

- [ ] **Step 5: Add the persistent-var slot**

In `include/MindfulTraderConstants.h`, extend `PersistentVar_AdaptiveCalculators` (currently ends at
line 89 with `FISHER_INFO_LAST_VALID_VALUE = 30;`):

```cpp
    const int AMIHUD_LAST_VALID_VALUE = 31; // Last valid amihud_illiquidity for graceful degradation on a thin/illiquid window
```

- [ ] **Step 6: Rewrite `CalculateAmihudIlliquidity`**

Replace the full function body (currently `src/StudyHelperFunctions.cpp:3042-3068`, keep the leading
doc-comment lines about the canonical Amihud formula):

```cpp
float CalculateAmihudIlliquidity(SCStudyInterfaceRef sc, int lookback_n) {
    // Canonical Amihud (2002) illiquidity: mean( |r_t| / DollarVolume_t ) over the
    // lookback, with r_t = log return ln(P_t / P_{t-1}) and DollarVolume_t = P_t * V_t.
    // Log returns + dollar volume make the measure price-level STATIONARY: a $2 move
    // at ES=6000 is not the same event as a $2 move at ES=2000, and a fixed threshold
    // is meaningless without this normalization (root cause of the old 0.80/0.40 bug).
    // Amihud is the canonical low-frequency proxy for Kyle (1985) lambda (price impact
    // per unit dollar flow). High values = illiquid (large impact per dollar traded).
    if (sc.Index < lookback_n) return 0.0f;

    // Window is entirely closed/historical bars (i=1..lookback_n), never the
    // current still-forming bar (i=0). This function is called from
    // UpdateObservationVectorSubgraphs's once-per-bar gate (first tick of
    // each bar only) -- sc.Volume[sc.Index] at that moment is the live,
    // still-accumulating volume, near its minimum almost every call, which
    // would otherwise make the current-bar term spuriously fail the
    // dollarVol/vol floor checks below on nearly every call (same root
    // cause as dim 7's and dim 12's once-per-bar timing bugs -- see
    // docs/superpowers/plans/2026-08-12-remaining-observation-vector-dims.md).
    double sum = 0.0;
    int count = 0;
    for (int i = 1; i <= lookback_n; ++i) {
        int idx = sc.Index - i;
        if (idx < 1) break;
        const double price = static_cast<double>(sc.Close[idx]);
        const double prevPrice = static_cast<double>(sc.Close[idx - 1]);
        const double vol = static_cast<double>(sc.Volume[idx]);
        if (vol < 1.0 || price <= 0.0 || prevPrice <= 0.0) continue;
        const double dollarVol = price * vol;
        if (dollarVol < 1.0) continue;
        const double logRet = std::abs(std::log(price / prevPrice));
        sum += logRet / dollarVol;
        ++count;
    }

    float& lastValidAmihud = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::AMIHUD_LAST_VALID_VALUE);
    const float amihud = cfc::ComputeAmihudIlliquidity(sum, count, lastValidAmihud);
    lastValidAmihud = amihud;
    return amihud;
}
```

- [ ] **Step 7: Build**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds.

- [ ] **Step 8: Commit**

```bash
git add include/CarryForwardCalculators.h tests/cpp/test_carry_forward_calculators.cpp include/MindfulTraderConstants.h src/StudyHelperFunctions.cpp
git commit -m "fix(microstructure): carry-forward + closed-bar-only window for amihud_illiquidity (dim 11)"
```

---

### Task 2: Dim 3 (`correction_action`) — reuse the existing burstiness carry-forward

**Files:**
- Modify: `include/MindfulTraderConstants.h`
- Modify: `src/StudyHelperFunctions.cpp:3005-3025` (`CalculateRealizedVarianceRatio`)

**Interfaces:**
- Consumes: `cfc::ComputeBurstinessIndex` (already exists, already tested — same
  `log(recent_rate/other_rate)` clamped-to-`[-6,6]` shape this function needs, just with
  `rv_full_rate` in place of burstiness's `rv_older_rate`).

**No new pure function needed** — `CalculateRealizedVarianceRatio`'s formula
(`log(max(rv_recent_rate,floor)/rv_full_rate)`, clamped `[-6,6]`) is identical in shape to
`ComputeBurstinessIndex`, and dim 3's schema bounds (`OBS_CORRECTION_ACTION`, `-6.0f`/`6.0f`) match
exactly. This dim is called directly from `TripleScreen2.cpp:296` (not through
`UpdateObservationVectorSubgraphs`), so it has no once-per-bar timing exposure — the only gap is the
missing carry-forward on its degenerate branch.

- [ ] **Step 1: Add the persistent-var slot**

In `include/MindfulTraderConstants.h`, add after `AMIHUD_LAST_VALID_VALUE` (from Task 1):

```cpp
    const int CORRECTION_ACTION_LAST_VALID_VALUE = 32; // Last valid correction_action for graceful degradation on a near-zero full-window variance rate
```

- [ ] **Step 2: Rewrite `CalculateRealizedVarianceRatio`**

Replace the full function body (currently `src/StudyHelperFunctions.cpp:3005-3025`, keep the leading
comment):

```cpp
float CalculateRealizedVarianceRatio(SCStudyInterfaceRef sc, int lookback_n) {
    // log(RV_recent / RV_full) — positive = volatility expanding, negative = contracting.
    // Stateless, naturally centered at 0, bounded by construction.
    const int half = lookback_n / 2;
    if (sc.Index < lookback_n || half < 2) return 0.0f;

    double rv_recent = 0.0, rv_full = 0.0;
    for (int i = 0; i < lookback_n; ++i) {
        int idx = sc.Index - i;
        if (idx < 1 || sc.Close[idx - 1] <= 0.0f) continue;
        double r = std::log(static_cast<double>(sc.Close[idx]) / sc.Close[idx - 1]);
        double r2 = r * r;
        rv_full += r2;
        if (i < half) rv_recent += r2;
    }

    // Scale full-window variance to per-sample rate for fair comparison.
    double rv_full_rate = rv_full / lookback_n;
    double rv_recent_rate = rv_recent / half;

    float& lastValidCorrectionAction = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::CORRECTION_ACTION_LAST_VALID_VALUE);
    const float correctionAction = cfc::ComputeBurstinessIndex(rv_recent_rate, rv_full_rate, lastValidCorrectionAction);
    lastValidCorrectionAction = correctionAction;
    return correctionAction;
}
```

- [ ] **Step 3: Build**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add include/MindfulTraderConstants.h src/StudyHelperFunctions.cpp
git commit -m "fix(microstructure): carry-forward for correction_action (dim 3) via existing burstiness formula"
```

---

### Task 3: Dim 12 (`liq_fragility`) — closed-bar-only volume read

**Files:**
- Modify: `src/StudyHelperFunctions.cpp:2766-2834` (`CalculateLiquidityFragility`)
- Modify: `src/TripleScreen3.cpp` (call site, to pass the previous bar's volume)

**Interfaces:** None new — this is a one-line data-source change plus a signature/call-site update.

`CalculateLiquidityFragility` already carries forward correctly on its `atrRef < 0.0001` branch
(`prev_fragility`, threaded through by the caller) — that part is fine, don't touch it. The bug is
narrower: line 2810's `sc.Volume[sc.Index]` (the live, current-bar volume) feeds the "thinness"
signal, and this function is called from `UpdateObservationVectorSubgraphs`'s once-per-bar gate (bar's
first tick only) — so `currentVolume` is read at its near-minimum almost every call, systematically
biasing `thinness` toward "market is thin" and pushing `fragility_smoothed` toward its extremes more
than warranted (matches the observed both-clamp-extremes pattern in the `ObservationStaleness` log).
Fix: read the last fully-closed bar's volume (`sc.Volume[sc.Index - 1]`) instead — available and
stable regardless of when this function is called, and the EMA-smoothing (`prev_fragility`, `alpha`)
logic is otherwise unchanged, so the function's intended bar-level cadence isn't altered, only *which*
bar's volume feeds the thinness term.

- [ ] **Step 1: Change the volume read**

In `src/StudyHelperFunctions.cpp`, inside `CalculateLiquidityFragility` (currently line 2810):

```cpp
    const float currentVolume = static_cast<float>(sc.Volume[sc.Index]);
```

Replace with:

```cpp
    // Last fully-closed bar's volume, not the current still-forming bar's
    // (this function is called once per bar, at the bar's first tick, via
    // UpdateObservationVectorSubgraphs -- sc.Volume[sc.Index] at that moment
    // is live and near its minimum almost every call, systematically biasing
    // "thinness" toward its extreme. See
    // docs/superpowers/plans/2026-08-12-remaining-observation-vector-dims.md.
    const float currentVolume = (sc.Index >= 1) ? static_cast<float>(sc.Volume[sc.Index - 1]) : 0.0f;
```

- [ ] **Step 2: Build**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds. No call-site or signature changes needed — this is a one-line data-source
swap inside the existing function body.

- [ ] **Step 3: Commit**

```bash
git add src/StudyHelperFunctions.cpp
git commit -m "fix(microstructure): read last-closed-bar volume for liq_fragility thinness signal (dim 12)"
```

---

### Task 4: Dim 4 (`vol_convexity`) — investigate whether the same timing bug applies

**Files:** None yet — investigation only. Add a fix task (Task 4b, written after this investigation
concludes) only if a real bug is confirmed.

**Why this is investigation-first, not a known fix:** `CalculateVolConvexity`
(`src/StudyHelperFunctions.cpp:3111-3153`, also called from `UpdateObservationVectorSubgraphs`) reads
`sc.BaseData[SC_HIGH][idx]`/`sc.BaseData[SC_LOW][idx]` for `idx = sc.Index - i`, `i = 0..n-1` — the
current bar's High/Low (`i=0`) are ALSO live/updating within the still-forming bar, same family as
dims 7/11/12. But unlike those three, the current-bar term here is just 1 of `n` (up to 40) terms
averaged into `meanTR`/`sumSqDiff` — diluted, not dominant. The `ObservationStaleness` log shows only
9 alerts (vs. dim 7's 51, dim 12's comparable severity) — consistent with a diluted version of the
same bug, but not confirmed.

- [ ] **Step 1: Quantify the dilution**

Compute, by hand or a scratch script, what fraction of `trValues[]`'s sum a single near-zero
`i=0` term (current bar's TR computed from a near-flat live High=Low=Open state) could shift
`meanTR`/`trStd` by, for `n` in its actual observed range (`observation_window_n`, clamped `[10,40]`
per `StudyHelperFunctions.cpp:2297`). If a single term out of 10-40 can plausibly swing the
coefficient-of-variation output all the way to a `-6`/`+6` LOGZ-clamp-worthy z-score, that's the bug.
If not, look elsewhere (Step 2).

- [ ] **Step 2: Check whether the alerts correlate with genuinely flat True Range**

Grep `/mnt/c/Trading/logs/MindfulTrader.log` for the specific bars/timestamps around dim 4's 9 alerts
(cross-reference `samples=` counts against the alert lines already captured in this session's
`/tmp/parse_staleness.py` output) and check the surrounding `TS1`/`TS2`/`TS3` MacroObs digest lines
(if any log ATR/TR values near those samples) for whether True Range was genuinely near-constant
across the window at that time — i.e., is `vol_convexity` legitimately near 0 (a real
calm/convexity-free period), not a bug at all. This is a valid conclusion — document it and stop if so.

- [ ] **Step 3: Decide**

- If Step 1 shows the current-bar term can plausibly explain the clamp-pegging: apply the same
  closed-bar-only fix as Tasks 1 and 3 (shift the loop to `i=1..n`, using only historical bars).
- If Step 2 shows the alerts correlate with genuinely flat True Range: no bug — document the
  conclusion in `docs/PENDING_USER_ACTIONS.md` and close this out.
- If neither: escalate — this needs a fresh pair of eyes on `FeatureScaler`'s `LOGZ` path itself
  (`FeatureScaler.h`'s `ToLogEnergy`/winsorization), not just the raw calculator.

**Task 4 conclusion (recorded 2026-08-12): (a) — real bug confirmed.** Full investigation in
`.superpowers/sdd/2026-08-12-remaining-observation-vector-dims/task-4-report.md`. Key findings,
verified directly against the current code (`src/StudyHelperFunctions.cpp:3228-3269`, line numbers
shifted from this task's original 3111-3153 estimate by earlier tasks' edits):

- `CalculateVolConvexity` is called directly from `scsf_Screen3_KeltnerChannel`
  (`TripleScreen3.cpp:782`), **not** through `UpdateObservationVectorSubgraphs`'s once-per-bar gate —
  structurally different from dims 7/11/12's "frozen all bar" defect. It recomputes every tick
  (correct cadence), but its `i=0..n-1` loop's `i=0` term reads the live, still-forming current bar's
  High/Low every time, contaminating the window.
- Dilution math: even at the most-diluting window size (`n=40`), a single contaminated term produces
  `cv≈0.16` vs. a true value of `0` in an otherwise-flat historical window; at `n=10`, `cv≈0.33`. Log
  evidence shows dim 4 hitting the literal `±6.0` LOGZ winsorization ceiling/floor (2 of 9 alerts),
  co-occurring with broader `sanitize_clamped` spikes — not isolated, not explained by "genuinely flat
  True Range" alone.
- **Fix pattern differs from Tasks 1/3:** no bar-gate is needed (the function is correctly stateless
  and already recomputes every tick) — only the loop range needs to change, from `i=0..n-1` to
  `i=1..n`, dropping the live current bar from `trValues[]` entirely and reading only closed/historical
  bars.
- **Secondary/deferred (not part of Task 4b, needs its own future task if confirmed):** 7 of 9 alerts
  show `value=0.0` with long `stale_run` — ambiguous between a genuinely flat period and the documented
  `FeatureScaler` MAD-floor carry-forward death spiral (dim 11/Amihud precedent). Re-check after Task 4b
  lands; if it persists, needs a dedicated `FeatureScaler`-side fix, not a `CalculateVolConvexity`
  change.

---

### Task 4b: Dim 4 (`vol_convexity`) — closed-bar-only window (fix, per Task 4's investigation)

**Files:**
- Modify: `src/StudyHelperFunctions.cpp:3228-3269` (`CalculateVolConvexity`)

**Interfaces:** None new — no persistent-var slot, no new pure function. This function is already
stateless (no `GetPersistentFloat`/`GetPersistentInt` calls) and must remain so.

**Scope decision (ruthless-simplicity call, not left to the implementer to relitigate):** unlike Tasks
1-3, this fix does **not** add a carry-forward slot for the degenerate/cold-start branch. The existing
`if (sc.Index < lookback_n) return 0.0f;` guard already returns a neutral value on true session
cold-start (the first ~40 bars of a multi-year backtest/session) — there is no prior "last valid value"
meaningfully available at that point, unlike the Tasks 1-3 cases (mid-session degenerate windows with a
real prior value to fall back to). Do not add a new persistent slot for this. Only the loop range and
the guard's threshold change.

- [ ] **Step 1: Rewrite the loop to use only closed bars**

Replace the full function body (currently `src/StudyHelperFunctions.cpp:3228-3269`, keep the leading
comment):

```cpp
float CalculateVolConvexity(SCStudyInterfaceRef sc, int lookback_n) {
    // Volatility of Volatility (High moment of volatility)
    // StdDev of ATR over lookback
    // Uses only closed/historical bars (i=1..n) -- sc.Index itself (the
    // still-forming current bar) is never read, since this function is
    // called every tick and its High/Low would otherwise leak the live,
    // not-yet-final bar into a statistic meant to summarize completed bars.

    // Better: Calculate TR locally to be robust

    // kMaxLookback matches the [10,40] adaptive observation window contract
    // this function's one caller (TripleScreen3.cpp) always passes today --
    // defensive upper bound so the fixed-capacity scratch buffer below can
    // never be written out of range if a future caller passes something larger.
    constexpr int kMaxLookback = 40;
    const int n = std::clamp(lookback_n, 1, kMaxLookback);
    if (sc.Index < n + 1) return 0.0f;

    std::array<float, kMaxLookback> trValues{};
    double sumTR = 0;

    for (int i = 1; i <= n; i++) {
        int idx = sc.Index - i;
        float h = sc.BaseData[SC_HIGH][idx];
        float l = sc.BaseData[SC_LOW][idx];
        float c_prev = sc.BaseData[SC_LAST][idx-1];

        float tr = std::max(h-l, std::max(std::abs(h-c_prev), std::abs(l-c_prev)));
        trValues[static_cast<size_t>(i - 1)] = tr;
        sumTR += tr;
    }

    double meanTR = sumTR / n;
    double sumSqDiff = 0;

    for (int i = 0; i < n; i++) {
        const float tr = trValues[static_cast<size_t>(i)];
        sumSqDiff += (tr - meanTR) * (tr - meanTR);
    }

    const double trStd = std::sqrt(sumSqDiff / n);
    const double trMean = std::max(meanTR, 1e-6);
    const float cv = static_cast<float>(trStd / trMean);
    return std::clamp(cv, 0.0f, 5.0f);
}
```

Note the guard moved below the `n` clamp and changed from `sc.Index < lookback_n` to
`sc.Index < n + 1`: the function now needs `n` closed bars *plus* one more for the oldest bar's
`c_prev` (`idx - 1`), i.e. `sc.Index >= n + 1`, not `sc.Index >= lookback_n`. The second loop (`i=0..n-1`
over `trValues[]`) is unchanged — only the population loop's source range changed.

- [ ] **Step 2: Build**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds.

- [ ] **Step 3: Commit**

```bash
git add src/StudyHelperFunctions.cpp
git commit -m "fix(microstructure): closed-bar-only window for vol_convexity (dim 4)"
```

---

### Task 5: Dim 9 (`tail_index`) — investigate whether the long stale runs are organic

**Files:** None yet — investigation only.

**Why this is a separate investigation, not related to Tasks 1-4's root cause:** `tail_index` is
computed by `TailRiskEngine` (`include/TailRiskEngine.h`), fed via
`ContextManager::UpdateMarketPhysics()`, called directly from `SCStudies.cpp` on every tick/price
change — **not** through `UpdateObservationVectorSubgraphs`'s once-per-bar gate at all. The
once-per-bar timing bug family (Tasks 1-4) cannot be the explanation here. The `ObservationStaleness`
log shows 56 alerts, max stale run 10,141, small *varying* (not single-sentinel) values.

- [ ] **Step 1: Check whether long stale runs are expected by design**

Read `TailRiskEngine::GetHillAlpha()` (`include/TailRiskEngine.h:88-141`) and its consumption gate
(`ContextManager.cpp:512`, `GetSampleCount() >= 50`). The Hill estimator only recomputes meaningfully
when the top-`k` tail observations change — for a `windowSize=500`, `tailPercent=0.05` (`k=25`)
configuration, `alpha` is expected to move slowly, changing only when an observation enters or leaves
the top-25 by absolute log-return. Estimate: over how many ticks/bars would you expect the top-25 to
stay unchanged in a typical quiet period? If that number is comparable to the observed 10,141-sample
stale runs, this is very likely organic, not a bug.

- [ ] **Step 2: Check for a warmup-reset interaction**

`ContextManager::Reset()` calls `m_tailRiskEngine.Reset()`. If `EventDataCollectorStudy`/
`BackTesterStudy` ever re-arm mid-session (multiple `Reset()` calls within one continuous log), each
one restarts the 50-sample warmup (`GetSampleCount() >= 50` gate), which caches the `4.0f`
"assume safe" default for 50 samples each time. Check `docs/PENDING_USER_ACTIONS.md`-style arm/reset
log lines (`grep "Context reset generation="`) against the timestamps of dim 9's longest stale runs —
if a reset happened shortly before a long run, that's the explanation (and arguably not a bug, just
warmup, same shape as dim 0's already-accepted pattern).

- [ ] **Step 3: Decide**

- If Steps 1-2 explain the pattern: document the conclusion (organic/warmup) in
  `docs/PENDING_USER_ACTIONS.md` and close this out — no code change.
- If neither explains a 10,141-sample run: this needs a fresh, deeper look at `TailRiskEngine`'s
  event-driven update path for a genuine staleness bug — scope a new investigation at that point,
  don't guess at a fix now.

---

### Task 6: Full clean build + full regression suite

**Files:** None new — verification only.

- [ ] **Step 1: Full clean build**

Run: `./build_dll.sh`
Expected: build succeeds with no errors.

- [ ] **Step 2: Run every native test suite**

```bash
g++ -std=c++17 -I include tests/cpp/test_carry_forward_calculators.cpp -o /tmp/cfc_test && /tmp/cfc_test
g++ -std=c++17 -I include tests/cpp/test_order_flow_asymmetry_engine.cpp -o /tmp/ofae_test && /tmp/ofae_test
g++ -std=c++17 -I include tests/cpp/test_ring_buffer.cpp -o /tmp/rb_test && /tmp/rb_test
g++ -std=c++17 -I include -I include/generated tests/cpp/test_feature_scaler.cpp -o /tmp/fs_test && /tmp/fs_test
```

Expected: all four suites exit 0, zero failures.

- [ ] **Step 3: Update `docs/PENDING_USER_ACTIONS.md` §6**

Mark dim 11's fix as implemented (both the D1 carry-forward and the closed-bar-only window), pending
the batched deploy + re-verify.

- [ ] **Step 4: Commit**

```bash
git add docs/PENDING_USER_ACTIONS.md
git commit -m "docs: mark dim 11 fix implemented, pending batched deploy"
```

---

## Explicitly out of scope

- **Dim 10 (skewness)** — research decision (`pending-replacement`), not an implementation task.
- **The deploy itself** — batch this after Task 6, alongside the already-committed dims 1/2/7/8 fixes.
- **The incremental-accumulator DOD performance plan**
  (`docs/superpowers/plans/2026-08-12-observation-vector-incremental-accumulators.md`) — orthogonal
  performance work, no correctness urgency, not part of this plan.
