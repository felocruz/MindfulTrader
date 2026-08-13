# Observation Vector & FeatureScaler Institutional Elevation Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Elevate the 16D observation vector and its shared `FeatureScaler` to consensus institutional
practice for fat-tailed financial time series, fixing one live training-data defect and four
literature-flagged calculator weaknesses along the way.

**Architecture:** Thirteen independently-committable tasks, each touching one calculator or one shared
component, in priority order (Unit 1 first — it's actively corrupting live training data). No task
blocks another; each produces working, natively-testable C++ on its own.

**Tech Stack:** C++17, native unit tests (no Sierra Chart/ACSIL dependency, `g++` + `tests/cpp/*.cpp`
pattern already established by `test_feature_scaler.cpp`), FlatBuffers-generated headers (read-only in
this plan — no schema changes).

**Spec:** `docs/superpowers/specs/2026-08-13-observation-vector-institutional-elevation-spec.md`
(argues from `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`'s 2026-08-13
consensus-practice findings). Executors should read both.

## Global Constraints

- Work directly on `master`, no feature branches or worktrees (standing repo convention).
- TDD throughout: write the failing test first, watch it fail, then write the minimum code to pass.
- No `lbrnet` edits from this repo — Unit 5 (D3 gate) is explicitly out of scope, tracked only as a
  pointer.
- **Unit 5 (spec's D3 gate) deliberately has no task in this plan** — it's `lbrnet`-repo work,
  out of scope per the no-`lbrnet`-edits constraint above; the spec carries it only as a pointer.
- Every calculator touched here already has a carry-forward-on-degenerate-input fix (D1, shipped
  2026-08-12) — preserve that behavior; do not reintroduce a fabricated-sentinel return path.
- `./build_dll.sh` must succeed before any task is considered done; native unit tests run via
  `g++ -std=c++17 -I include -I include/generated tests/cpp/<file>.cpp -o /tmp/<bin> && /tmp/<bin>`
  (the pattern `test_feature_scaler.cpp` already establishes).

---

### Task 1: `FeatureScaler` dedupe-at-ingestion (Unit 1)

**Files:**
- Modify: `include/FeatureScaler.h:262-277` (`UpdateAndNormalize()` Step 1 buffer-update loop)
- Test: `tests/cpp/test_feature_scaler.cpp` (append)

**Interfaces:**
- Consumes: `RingBuffer<T,N>::back()`/`empty()` (already exist, `include/RingBuffer.h:45-56`).
- Produces: no new public interface — `UpdateAndNormalize()`'s signature and `dominanceRatio[]`
  (`FeatureScaler.h:158`) are unchanged; only the window-ingestion behavior changes.

- [ ] **Step 1: Write the failing test — held value gets a nonzero z-score**

```cpp
// Append to tests/cpp/test_feature_scaler.cpp, inside main() alongside existing checks.
void TestDedupeAtIngestion() {
    FeatureScaler fs;
    // Warm up dim 1 (burstiness_index, SOFTLOGZ) with 500 varying samples so the
    // rolling window and warmup gate are both satisfied before the repeat run.
    for (int i = 0; i < 500; ++i) {
        auto obs = MakeObs(0.0f);
        obs[1] = 0.01f * static_cast<float>(i % 37) - 0.18f;  // varying, never exactly repeats
        fs.UpdateAndNormalize(obs);
    }
    // Now inject a carry-forward run: the same raw value 300 times in a row,
    // simulating a degenerate-input calculator holding its last valid reading.
    std::array<float, FeatureScaler::N_DIMS> result{};
    for (int i = 0; i < 300; ++i) {
        auto obs = MakeObs(0.0f);
        obs[1] = 0.42f;
        result = fs.UpdateAndNormalize(obs);
    }
    // Pre-fix: the window fills with 0.42f repeats, median converges to 0.42f,
    // z collapses to exactly 0. Post-fix: the window stops growing after the
    // first 0.42f push, so the held value's z reflects its true distance from
    // the pre-repeat distribution -- must be nonzero.
    check("dedupe: held value gets nonzero z after long repeat run", std::fabs(result[1]) > 1e-4f);
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
g++ -std=c++17 -I include -I include/generated tests/cpp/test_feature_scaler.cpp -o /tmp/fs_test && /tmp/fs_test
```
Expected: `FAIL  dedupe: held value gets nonzero z after long repeat run` (result[1] is 0.0 pre-fix).

- [ ] **Step 3: Implement dedupe-at-ingestion**

Replace `FeatureScaler.h`'s Step 1 loop (currently):

```cpp
for (size_t i = 0; i < N_DIMS; ++i) {
    const size_t winSize = DIM_WINDOW_SIZE[i];
    if (SCALE_MODE_MAP[i] == ScaleMode::SOFTLOGZ) {
        stateBuffers[i].push_back(raw[i]);
        if (stateBuffers[i].size() > winSize) {
            stateBuffers[i].pop_front();
        }
    } else {
        const float logValue = ToLogEnergy(raw[i]);
        logBuffers[i].push_back(logValue);
        if (logBuffers[i].size() > winSize) {
            logBuffers[i].pop_front();
        }
    }
}
```

with:

```cpp
// Dedupe-at-ingestion: a value identical to the window's most recent entry
// carries zero marginal Shannon information (H(X_t|X_{t-1})=0 for an exact
// repeat) and must not be treated as a fresh independent draw by the
// median/MAD estimator below -- see docs/superpowers/specs/
// 2026-08-13-observation-vector-institutional-elevation-spec.md Unit 1.
for (size_t i = 0; i < N_DIMS; ++i) {
    const size_t winSize = DIM_WINDOW_SIZE[i];
    if (SCALE_MODE_MAP[i] == ScaleMode::SOFTLOGZ) {
        auto& buf = stateBuffers[i];
        if (buf.empty() || buf.back() != raw[i]) {
            buf.push_back(raw[i]);
            if (buf.size() > winSize) {
                buf.pop_front();
            }
        }
    } else {
        const float logValue = ToLogEnergy(raw[i]);
        auto& buf = logBuffers[i];
        if (buf.empty() || buf.back() != logValue) {
            buf.push_back(logValue);
            if (buf.size() > winSize) {
                buf.pop_front();
            }
        }
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
g++ -std=c++17 -I include -I include/generated tests/cpp/test_feature_scaler.cpp -o /tmp/fs_test && /tmp/fs_test
```
Expected: `PASS  dedupe: held value gets nonzero z after long repeat run`, and all pre-existing
`test_feature_scaler.cpp` checks still `PASS` (no regression on streams with no repeats).

- [ ] **Step 5: Write the second failing test — D2 dominance drops post-fix**

```cpp
void TestDedupeLowersDominance() {
    FeatureScaler fs;
    for (int i = 0; i < 500; ++i) {
        auto obs = MakeObs(0.0f);
        obs[2] = 0.01f * static_cast<float>(i % 41) - 0.2f;
        fs.UpdateAndNormalize(obs);
    }
    // Inject the same repeat-collapse signature the hardening spec's D2 diagnostic
    // was built to catch: a single value dominating >30% of the window.
    for (int i = 0; i < 250; ++i) {
        auto obs = MakeObs(0.0f);
        obs[2] = 0.75f;
        fs.UpdateAndNormalize(obs);
    }
    // Force a recalibration so dominanceRatio[] is recomputed.
    fs.Recalibrate();
    check("dedupe: dim2 dominanceRatio stays below the D2 ALERT threshold (0.30)",
          fs.dominanceRatio[2] < 0.30f);
}
```

Add both `TestDedupeAtIngestion();` and `TestDedupeLowersDominance();` calls to `main()`.

- [ ] **Step 6: Run both new tests, verify pass, then commit**

```bash
g++ -std=c++17 -I include -I include/generated tests/cpp/test_feature_scaler.cpp -o /tmp/fs_test && /tmp/fs_test
git add include/FeatureScaler.h tests/cpp/test_feature_scaler.cpp
git commit -m "fix(feature-scaler): dedupe consecutive-identical raw values before rolling-window ingestion

Fixes the median-collapse-to-z=0 defect measured at 61-71% exact-zero in live
.context training data for dims 1/2 (docs/superpowers/specs/2026-08-13-
observation-vector-institutional-elevation-spec.md Unit 1)."
```

---

### Task 2: Miller-Madow entropy bias correction (Unit 2a)

**Files:**
- Modify: `include/InformationEngine.h:294-311` (`GetShannonEntropy()`)
- Test: `tests/cpp/test_information_engine.cpp` (create — no existing test file for this class; follow
  `test_feature_scaler.cpp`'s zero-Sierra-Chart-dependency pattern, verify `InformationEngine.h` has
  none before assuming this works natively).

**Interfaces:**
- Consumes: `m_countP` (existing private member, `InformationEngine.h:366`), `m_histogramP[NUM_BINS]`
  (`InformationEngine.h:353`).
- Produces: `GetShannonEntropy()`'s return type/signature unchanged (still `double`, still bits).

- [ ] **Step 1: Confirm `InformationEngine.h` is natively testable**

```bash
grep -n "#include" include/InformationEngine.h
```
Expected: no `sierrachart.h` or ACSIL includes. If any exist, stop and report — this task's test
approach needs revision before proceeding.

- [ ] **Step 2: Write the failing test**

```cpp
// tests/cpp/test_information_engine.cpp
#include "InformationEngine.h"
#include <cmath>
#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
bool approx(double a, double b, double tol = 1e-9) { return std::fabs(a - b) <= tol; }
}

int main() {
    // Known small-N case: 10 bins, N=50, uniform distribution (5 per bin) --
    // plug-in entropy is exactly log2(10); Miller-Madow subtracts
    // (m-1)/(2*N*ln2) where m = occupied bins (10, all nonzero here).
    InformationEngine ie;
    for (int i = 0; i < 50; ++i) {
        ie.AddObservation(static_cast<double>(i % 10) - 4.5);  // spreads evenly across 10 bins
    }
    const double plugin = std::log2(10.0);
    const double bias = (10.0 - 1.0) / (2.0 * 50.0 * std::log(2.0));
    const double expected = plugin - bias;
    check("Miller-Madow-corrected entropy matches closed-form bias subtraction",
          approx(ie.GetShannonEntropy(), expected, 1e-3));
    check("corrected entropy is strictly less than uncorrected plug-in value",
          ie.GetShannonEntropy() < plugin);

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

(This test assumes `AddObservation(double)` is `InformationEngine`'s existing public ingestion method
and bins evenly by construction — if the real bin-assignment logic doesn't spread these inputs evenly
across all 10 bins, adjust the input sequence so it does, verified by a quick manual trace of
`UpdateHistogram()` before finalizing this test; the assertion that matters is the closed-form bias
match, not the specific input values.)

- [ ] **Step 3: Run test to verify it fails**

```bash
g++ -std=c++17 -I include -I include/generated tests/cpp/test_information_engine.cpp -o /tmp/ie_test && /tmp/ie_test
```
Expected: FAIL (current `GetShannonEntropy()` returns the uncorrected plug-in value).

- [ ] **Step 4: Implement the Miller-Madow correction**

Replace `GetShannonEntropy()` (`InformationEngine.h:294-311`):

```cpp
double GetShannonEntropy() const {
    if (m_countP < 10) return 0.0; // Not enough data

    double entropy = 0.0;
    double totalCount = static_cast<double>(m_countP);
    size_t occupiedBins = 0;

    for (size_t i = 0; i < NUM_BINS; ++i) {
        double count = m_histogramP[i];
        if (count <= 0.0) continue;
        ++occupiedBins;

        double p = count / totalCount;
        entropy -= p * std::log2(p);
    }

    // Miller (1955) bias correction for the small-sample plug-in Shannon
    // entropy estimator: bias ~ (m-1)/(2N ln2) bits, m = occupied bins.
    // Confirmed still-current consensus practice (2026-08-13 grounding pass,
    // matches R's entropy::entropy(method="MM") / infotheo's "mm" default).
    if (occupiedBins > 1) {
        const double bias = (static_cast<double>(occupiedBins) - 1.0) / (2.0 * totalCount * std::log(2.0));
        entropy -= bias;
    }

    return std::max(entropy, 0.0);  // guard against the correction pushing a
                                     // near-zero-entropy reading negative
}
```

- [ ] **Step 5: Run test to verify it passes, then commit**

```bash
g++ -std=c++17 -I include -I include/generated tests/cpp/test_information_engine.cpp -o /tmp/ie_test && /tmp/ie_test
git add include/InformationEngine.h tests/cpp/test_information_engine.cpp
git commit -m "fix(information-engine): apply Miller-Madow bias correction to short-window Shannon entropy

Confirmed still-current consensus practice (docs/superpowers/specs/2026-08-12-
gang-literature-grounding-spec.md Pillar 2), was flagged but never applied."
```

---

### Task 3: RQA epsilon — periodic fixed-recurrence-rate calibration (Unit 2b)

**Files:**
- Modify: `src/StudyHelperFunctions.cpp:3326-3390ish` (`CalculateRecurrenceRate()` — verify current
  line numbers with `grep -n "float CalculateRecurrenceRate" src/StudyHelperFunctions.cpp` before
  editing; they have already drifted once since the Gang spec's original citation)
- Modify: `include/MindfulTraderConstants.h` (two new `PersistentVar_AdaptiveCalculators` indices)
- Test: no existing native test harness covers `StudyHelperFunctions.cpp` (it has ACSIL/`sc.` deps) —
  this task adds a **pure, extracted helper function** instead, tested natively, called from the
  existing ACSIL function.

**Design correction from the spec:** the spec described this as "a small bisection loop, same O(n²)
cost." That undersells one real correctness issue caught during planning: if epsilon is recalibrated
to hit a *fixed* target recurrence rate on **every call**, the output recurrence rate becomes
≈constant by construction — destroying dim 13's value as a discriminating HMM feature (Schinkel et al.
2008's technique makes epsilon comparable across regimes; it doesn't make RR itself informative when
reselected every call). Fix: calibrate epsilon periodically (every `RQA_EPSILON_RECALIBRATION_BARS`
calls) via the fixed-RR order-statistic method, then hold it fixed between recalibrations — RR is free
to vary meaningfully call-to-call against a scale-appropriate, periodically-refreshed epsilon. Same
pattern `FeatureScaler::Calibrate()`/`Recalibrate()` already establishes elsewhere in this codebase.

**Interfaces:**
- Produces: `double SelectEpsilonForTargetRecurrenceRate(const float* prices, int n, double targetRR)`
  — new pure free function, header-declared alongside other pure helpers, zero `sc.` dependency.
- Consumes (in the ACSIL wrapper): `sc.GetPersistentFloat` (existing pattern, e.g.
  `StudyHelperFunctions.cpp:2733`).

- [ ] **Step 1: Write the failing test for the pure helper**

```cpp
// tests/cpp/test_rqa_epsilon.cpp
#include <cmath>
#include <cstdio>
#include <vector>
#include <algorithm>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}

// Declared here to match the production signature this step will add to
// StudyHelperFunctions.h/.cpp -- copy the real implementation in Step 3,
// do not reimplement it differently for the test.
double SelectEpsilonForTargetRecurrenceRate(const float* prices, int n, double targetRR);
}

int main() {
    // 30 prices linearly spaced 100.0 to 103.0 -- deterministic, known pairwise
    // distance distribution.
    std::vector<float> prices(30);
    for (int i = 0; i < 30; ++i) prices[static_cast<size_t>(i)] = 100.0f + 0.1f * static_cast<float>(i);

    const double eps = SelectEpsilonForTargetRecurrenceRate(prices.data(), 30, 0.03);

    // Verify: using this epsilon, the actual measured recurrence rate over
    // these prices is within a small tolerance of the 0.03 target (the
    // defining property of the fixed-RR selection method).
    int recurCount = 30;  // diagonal (i==i) always recurs
    for (int i = 0; i < 30; ++i) {
        for (int j = i + 1; j < 30; ++j) {
            if (std::fabs(prices[static_cast<size_t>(i)] - prices[static_cast<size_t>(j)]) < eps) {
                recurCount += 2;
            }
        }
    }
    const double measuredRR = static_cast<double>(recurCount) / (30.0 * 30.0);
    check("selected epsilon achieves recurrence rate within 0.01 of the 0.03 target",
          std::fabs(measuredRR - 0.03) < 0.01);
    check("epsilon is strictly positive", eps > 0.0);

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails (link error — function doesn't exist yet)**

```bash
g++ -std=c++17 tests/cpp/test_rqa_epsilon.cpp -o /tmp/rqa_test
```
Expected: link failure, `undefined reference to SelectEpsilonForTargetRecurrenceRate`.

- [ ] **Step 3: Implement the pure helper**

Add `#include <vector>` to `StudyHelperFunctions.cpp`'s include block if not already present (verified
2026-08-13: it is not — this file currently has no `std::vector` usage). `std::vector` here is
justified despite the repo's no-heap-allocation-on-hot-paths rule: this helper only runs on the
periodic (every-200-bars) calibration branch added in Step 5, never per-tick, matching the precedent
`TailRiskEngine::m_sortBuffer` already sets for periodic-cadence vector use in this same pipeline.

Add the function to `src/StudyHelperFunctions.cpp` (near `CalculateRecurrenceRate`, before it) and
declare in `include/StudyHelperFunctions.h` (matching this file's existing pure-helper declarations):

```cpp
/// Selects epsilon such that the recurrence rate over `prices[0..n)` is as
/// close as possible to `targetRR`, via the Schinkel, Dimigen & Marwan
/// (2008) fixed-recurrence-rate method: sort all pairwise absolute
/// distances, epsilon = the distance at rank floor(targetRR * numPairs).
/// O(n^2 log n) -- called only at recalibration cadence (Task 3's
/// RQA_EPSILON_RECALIBRATION_BARS), never per-tick.
double SelectEpsilonForTargetRecurrenceRate(const float* prices, int n, double targetRR) {
    if (n < 2) return 1e-6;

    std::vector<float> distances;
    distances.reserve(static_cast<size_t>(n) * static_cast<size_t>(n - 1) / 2);
    for (int i = 0; i < n; ++i) {
        for (int j = i + 1; j < n; ++j) {
            distances.push_back(std::fabs(prices[i] - prices[j]));
        }
    }
    if (distances.empty()) return 1e-6;

    std::sort(distances.begin(), distances.end());

    const size_t numPairs = distances.size();
    size_t rank = static_cast<size_t>(targetRR * static_cast<double>(numPairs));
    rank = std::min(rank, numPairs - 1);

    return std::max(static_cast<double>(distances[rank]), 1e-6);
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
g++ -std=c++17 tests/cpp/test_rqa_epsilon.cpp src/StudyHelperFunctions.cpp -I include -I include/generated -o /tmp/rqa_test && /tmp/rqa_test
```
(If `StudyHelperFunctions.cpp` fails to compile standalone due to ACSIL deps elsewhere in the file,
extract `SelectEpsilonForTargetRecurrenceRate` into a new zero-dependency header instead, e.g.
`include/RQAEpsilonSelector.h`, following the exact precedent `FeatureScaler.h`/`TailRiskEngine.h`/
`InformationEngine.h` already set for "extract for native testability." Prefer this if the standalone
compile fails — it's the established pattern, not a new one.)
Expected: `ALL PASS`.

- [ ] **Step 5: Wire the periodic-calibration wrapper into `CalculateRecurrenceRate`**

Add two new persistent-var indices to `include/MindfulTraderConstants.h`'s
`PersistentVar_AdaptiveCalculators` namespace (next available integer after the existing highest,
currently `33`):

```cpp
const int RQA_CALIBRATED_EPSILON = 34;       // Held epsilon between recalibrations
const int RQA_CALLS_SINCE_CALIBRATION = 35;  // Recalibration cadence counter
```

Modify `CalculateRecurrenceRate` (`StudyHelperFunctions.cpp`) — replace the current:

```cpp
float epsilon = std::max(range * 0.1f, stdP * 0.5f);
epsilon = std::max(epsilon, 1e-6f);
```

with:

```cpp
constexpr int RQA_EPSILON_RECALIBRATION_BARS = 200;  // engineering choice, not literature-
                                                        // prescribed -- matches this codebase's
                                                        // existing periodic-recalibration cadence
                                                        // convention (FeatureScaler::RECALIBRATION_INTERVAL).
constexpr double RQA_TARGET_RECURRENCE_RATE = 0.03;   // midpoint of Schinkel et al. (2008)'s
                                                        // cited 0.01-0.05 practitioner range.
float& calibratedEpsilon = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::RQA_CALIBRATED_EPSILON);
float& callsSinceCalibration = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::RQA_CALLS_SINCE_CALIBRATION);

if (calibratedEpsilon <= 0.0f || callsSinceCalibration >= static_cast<float>(RQA_EPSILON_RECALIBRATION_BARS)) {
    std::vector<float> windowPrices(static_cast<size_t>(lookback_n));
    for (int i = 0; i < lookback_n; ++i) {
        windowPrices[static_cast<size_t>(i)] = sc.BaseData[SC_LAST][sc.Index - i];
    }
    calibratedEpsilon = static_cast<float>(
        SelectEpsilonForTargetRecurrenceRate(windowPrices.data(), lookback_n, RQA_TARGET_RECURRENCE_RATE));
    callsSinceCalibration = 0.0f;
}
++callsSinceCalibration;

float epsilon = calibratedEpsilon;
```

(This sits after the existing degenerate-range carry-forward guard at line ~3348 — unaffected, still
runs first and still skips the O(n²) work on a flat window.)

- [ ] **Step 6: Run the full native test suite plus a `./build_dll.sh` smoke build, then commit**

```bash
g++ -std=c++17 -I include -I include/generated tests/cpp/test_rqa_epsilon.cpp -o /tmp/rqa_test && /tmp/rqa_test
./build_dll.sh --no-clean
git add src/StudyHelperFunctions.cpp include/MindfulTraderConstants.h include/StudyHelperFunctions.h tests/cpp/test_rqa_epsilon.cpp
git commit -m "fix(microstructure): RQA epsilon via periodic fixed-recurrence-rate calibration

Replaces the range/SD heuristic (scale-variant across volatility regimes)
with Schinkel et al. (2008)'s fixed-RR method, recalibrated every 200 bars
rather than every call so recurrence_rate itself stays an informative,
varying HMM feature instead of collapsing to a near-constant."
```

---

### Task 4: Hill-plot stability-region k-selector + EWMA-smoothed alpha (Unit 2c)

**Files:**
- Modify: `include/TailRiskEngine.h` (whole `GetHillAlpha()` + constructor)
- Test: `tests/cpp/test_tail_risk_engine.cpp` (create — file has zero ACSIL deps already, confirmed by
  its existing header comment style matching `FeatureScaler.h`'s extraction precedent).

**Interfaces:**
- Consumes: existing `m_buffer`/`m_sortBuffer`/`m_isFull`/`m_headIndex` private state.
- Produces: `GetHillAlpha()`'s signature/return type unchanged (`double`) — internal behavior only.

- [ ] **Step 1: Write the failing test — k-selector recovers a sane region on synthetic Pareto data**

```cpp
// tests/cpp/test_tail_risk_engine.cpp
#include "TailRiskEngine.h"
#include <cmath>
#include <cstdio>
#include <random>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}

int main() {
    using namespace MindfulTrader;
    // Synthetic Pareto-tailed data with known alpha=3.0 (Type I Pareto:
    // X = (1-U)^(-1/alpha), U~Uniform(0,1)).
    std::mt19937 rng(42);
    std::uniform_real_distribution<double> uniform(0.0, 1.0);
    TailRiskEngine engine(500, 0.05);
    for (int i = 0; i < 500; ++i) {
        const double u = uniform(rng);
        const double x = std::pow(1.0 - u, -1.0 / 3.0) - 1.0;  // shifted to center near 0
        engine.AddObservation(x * 0.001);  // scaled to a log-return-like magnitude
    }
    const double alpha = engine.GetHillAlpha();
    check("Hill alpha on synthetic alpha=3.0 Pareto data lands in [1.5, 5.0]",
          alpha >= 1.5 && alpha <= 5.0);

    // EWMA smoothing: two consecutive reads after adding one more observation
    // apart should not swing wildly (smoothing suppresses single-sample noise).
    const double alphaBefore = engine.GetHillAlpha();
    engine.AddObservation(0.0001);  // one small, unremarkable observation
    const double alphaAfter = engine.GetHillAlpha();
    check("EWMA-smoothed alpha changes by less than 15% on a single unremarkable observation",
          std::fabs(alphaAfter - alphaBefore) / alphaBefore < 0.15);

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

```bash
g++ -std=c++17 -I include tests/cpp/test_tail_risk_engine.cpp -o /tmp/tre_test && /tmp/tre_test
```
Expected: the EWMA-stability check FAILS first (current `GetHillAlpha()` recomputes fresh from a fixed
5% cutoff every call with no smoothing state, so consecutive calls can swing more than 15% on a single
new observation depending on where it lands in the tail).

- [ ] **Step 3: Implement the stability-region k-selector + EWMA smoothing**

Replace `GetHillAlpha()`'s body (`TailRiskEngine.h:88-142`) — keep the existing sort-buffer-copy logic
(Step 1 of the current implementation, lines 96-103), then replace the fixed-`k=m_tailCutoff` Hill
computation with a stability-region scan, and wrap the result in EWMA smoothing:

```cpp
double GetHillAlpha() {
    if (!m_isFull && m_headIndex < m_tailCutoff + 1) {
        return 4.0; // Not enough data yet, assume Gaussian safety
    }

    m_sortBuffer.clear();
    if (m_isFull) {
        m_sortBuffer.assign(m_buffer.begin(), m_buffer.end());
    } else {
        m_sortBuffer.insert(m_sortBuffer.end(), m_buffer.begin(), m_buffer.begin() + m_headIndex);
    }

    size_t validSize = m_sortBuffer.size();
    // Widen the sort to the largest k we might scan (up to 4x the configured
    // tail cutoff, capped at validSize-1) so the stability-region scan below
    // has a real range to search, not just the single configured k.
    size_t maxK = std::min(m_tailCutoff * 4, validSize - 1);
    if (maxK < 2) return 4.0;

    std::partial_sort(
        m_sortBuffer.begin(),
        m_sortBuffer.begin() + static_cast<std::ptrdiff_t>(maxK) + 1,
        m_sortBuffer.end(),
        std::greater<double>()
    );

    // Resnick & Stărică (1997) Hill-plot stability-region selector: compute
    // Hill(k) across a k-range, find the widest window of k where consecutive
    // Hill(k) values stay within a fixed relative band, take its midpoint --
    // cheap (O(maxK), reuses the sort already paid for above), no bootstrap.
    std::vector<double> hillAtK;
    hillAtK.reserve(maxK);
    for (size_t k = 2; k <= maxK; ++k) {
        double threshold = m_sortBuffer[k];
        if (threshold <= 1e-9) { hillAtK.push_back(4.0); continue; }
        double logSum = 0.0;
        for (size_t i = 0; i < k; ++i) {
            logSum += (std::log(m_sortBuffer[i]) - std::log(threshold));
        }
        hillAtK.push_back(logSum > 1e-9 ? static_cast<double>(k) / logSum : 4.0);
    }

    constexpr double kStabilityBandRelative = 0.15;  // 15% band, engineering choice within
                                                        // the practitioner "eyeball stability"
                                                        // convention -- no literature-prescribed
                                                        // exact figure found (2026-08-13 grounding pass).
    size_t bestStart = 0, bestLen = 1;
    size_t start = 0;
    for (size_t i = 1; i < hillAtK.size(); ++i) {
        const double relDiff = std::fabs(hillAtK[i] - hillAtK[start]) / std::max(hillAtK[start], 1e-6);
        if (relDiff > kStabilityBandRelative) {
            start = i;
        }
        if (i - start + 1 > bestLen) {
            bestLen = i - start + 1;
            bestStart = start;
        }
    }
    const size_t midpointIdx = bestStart + bestLen / 2;
    const double rawAlpha = hillAtK[std::min(midpointIdx, hillAtK.size() - 1)];

    // EWMA-smooth the alpha series itself (not the k-selection) -- Resnick &
    // Stărică (1997) show Hill is consistent under GARCH-type dependence, so
    // smoothing the output addresses finite-sample variance without
    // disturbing the underlying estimator. alpha=0.2 -> ~9-sample half-life,
    // an engineering choice matching this codebase's other EMA smoothing
    // conventions (e.g. FeatureScaler's CARRY_DECAY_HALFLIFE=200 samples at
    // a much higher tick-rate cadence -- this dim updates far less often).
    constexpr double kAlphaSmoothing = 0.2;
    if (!m_hasSmoothedAlpha) {
        m_smoothedAlpha = rawAlpha;
        m_hasSmoothedAlpha = true;
    } else {
        m_smoothedAlpha = kAlphaSmoothing * rawAlpha + (1.0 - kAlphaSmoothing) * m_smoothedAlpha;
    }
    return m_smoothedAlpha;
}
```

Add the two new private members near the existing ones:

```cpp
double m_smoothedAlpha = 4.0;
bool m_hasSmoothedAlpha = false;
```

- [ ] **Step 4: Run test to verify it passes**

```bash
g++ -std=c++17 -I include tests/cpp/test_tail_risk_engine.cpp -o /tmp/tre_test && /tmp/tre_test
```
Expected: `ALL PASS`.

- [ ] **Step 5: Commit**

```bash
git add include/TailRiskEngine.h tests/cpp/test_tail_risk_engine.cpp
git commit -m "feat(tail-risk): Hill-plot stability-region k-selection + EWMA-smoothed alpha

Replaces the fixed 5%-tail cutoff with Resnick & Stărică (1997)'s stability-
region selector and smooths the output series, per the 2026-08-13 Pareto-
pillar consensus-practice findings (docs/superpowers/specs/2026-08-12-
gang-literature-grounding-spec.md)."
```

---

### Task 5: Quantile-maintenance benchmark for Bowley/Moors (Unit 3, part 1 of 2)

**Files:**
- Test/benchmark: `tests/cpp/bench_quantile_window.cpp` (create — throwaway benchmark harness, not a
  correctness test; its only output is a timing number that gates Task 6's implementation choice).

**Interfaces:** none — this task produces a decision, not a shipped interface.

- [ ] **Step 1: Write the benchmark harness**

```cpp
// tests/cpp/bench_quantile_window.cpp — throwaway timing harness, not part of
// the regular test suite. Determines whether Task 6 uses a plain full
// re-sort or needs a t-digest, per docs/superpowers/specs/2026-08-13-
// observation-vector-institutional-elevation-spec.md Unit 3's decision rule.
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <random>

int main() {
    std::mt19937 rng(7);
    std::normal_distribution<float> dist(0.0f, 1.0f);
    std::array<float, 100> returns{};
    for (auto& r : returns) r = dist(rng);

    constexpr int kIterations = 100000;  // far more than the 15-min-bar-cadence
                                          // call rate would ever produce in a
                                          // comparable wall-clock span; a
                                          // conservative stress test.
    const auto start = std::chrono::steady_clock::now();
    volatile float sink = 0.0f;  // prevent the optimizer from eliding the sort
    for (int iter = 0; iter < kIterations; ++iter) {
        std::array<float, 100> scratch = returns;
        std::sort(scratch.begin(), scratch.end());
        sink += scratch[50];
    }
    const auto end = std::chrono::steady_clock::now();
    const double totalMicros = std::chrono::duration<double, std::micro>(end - start).count();
    const double perCallMicros = totalMicros / kIterations;

    std::printf("Full re-sort of N=100: %.4f microseconds/call (%d iterations)\n",
                perCallMicros, kIterations);
    std::printf("At 15-minute bar cadence this runs at most once per 900 seconds --\n");
    std::printf("verdict: %s\n",
                 perCallMicros < 10.0 ? "PASS -- full re-sort is trivially cheap enough, use it directly"
                                      : "FAIL -- investigate t-digest (Dunning & Ertl 2019)");
    return 0;
}
```

- [ ] **Step 2: Run the benchmark and record the verdict**

```bash
g++ -std=c++17 -O2 tests/cpp/bench_quantile_window.cpp -o /tmp/quantile_bench && /tmp/quantile_bench
```
Expected: a per-call time in the low single-digit microseconds (sorting 100 floats is trivial), printing
`PASS`. **This verdict determines Task 6's implementation** — Task 6 below is written for the PASS
(full re-sort) branch, per the spec's stated expectation. If this benchmark instead prints `FAIL` on
the actual target hardware, stop and re-scope Task 6 around a t-digest before proceeding — do not
implement Task 6 as written below without first confirming this benchmark passed.

- [ ] **Step 3: Commit the benchmark harness (kept for future re-verification, e.g. after a hardware change)**

```bash
git add tests/cpp/bench_quantile_window.cpp
git commit -m "test(microstructure): benchmark full-resort vs t-digest for Bowley/Moors quantile window

Confirms full re-sort at N=100/15-min cadence is cheap enough -- gates Task 6's
implementation choice per docs/superpowers/specs/2026-08-13-observation-vector-
institutional-elevation-spec.md Unit 3."
```

---

### Task 6: Bowley skewness / Moors kurtosis calculators (Unit 3, part 2 of 2)

**Precondition:** Task 5's benchmark printed `PASS`. If it printed `FAIL`, this task's implementation
must change (see Task 5 Step 2) — do not proceed with the code below without that confirmation.

**Files:**
- Modify: `src/StudyHelperFunctions.cpp:2630-2776` (`CalculateRealizedKurtosis`, `CalculateSkewness`)
- Test: `tests/cpp/test_robust_moments.cpp` (create — pure-function extraction, same pattern as
  Task 3's RQA helper).

**Interfaces:**
- Produces: `float EmpiricalQuantile(std::array<float,100>& sortedReturns, double p)`,
  `float BowleySkewness(std::array<float,100> returns)`, `float MoorsKurtosis(std::array<float,100> returns)`
  — three new pure free functions (extracted to a new zero-dependency header,
  `include/RobustMoments.h`, following the established extraction precedent).

- [ ] **Step 1: Write the failing tests**

```cpp
// tests/cpp/test_robust_moments.cpp
#include "RobustMoments.h"
#include <cmath>
#include <cstdio>
#include <random>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
bool approx(float a, float b, float tol) { return std::fabs(a - b) <= tol; }
}

int main() {
    // Gaussian reference: Moors kurtosis normalizes to ~1.23 under N(0,1)
    // (the value the spec cites) -- verify on a large synthetic Gaussian sample.
    std::mt19937 rng(11);
    std::normal_distribution<float> gauss(0.0f, 1.0f);
    std::array<float, 100> gaussianReturns{};
    for (auto& r : gaussianReturns) r = gauss(rng);

    check("Moors kurtosis on N(0,1) sample is close to the ~1.23 reference value",
          approx(MoorsKurtosis(gaussianReturns), 1.23f, 0.35f));  // wide tolerance -- N=100 single draw

    // Symmetric distribution -> Bowley skewness near 0.
    check("Bowley skewness on symmetric N(0,1) sample is near 0",
          approx(BowleySkewness(gaussianReturns), 0.0f, 0.15f));

    // Outlier robustness: the whole point of Kim & White's replacement --
    // a single extreme value must NOT blow up either statistic the way the
    // old moment-based formula did.
    std::array<float, 100> withOutlier = gaussianReturns;
    withOutlier[0] = 50.0f;  // 50-sigma outlier
    check("Moors kurtosis is not dominated by a single 50-sigma outlier",
          MoorsKurtosis(withOutlier) < 5.0f);  // moment-based kurtosis would spike to hundreds here

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails (compile failure — header doesn't exist yet)**

```bash
g++ -std=c++17 -I include tests/cpp/test_robust_moments.cpp -o /tmp/rm_test
```
Expected: compile failure, `RobustMoments.h: No such file or directory`.

- [ ] **Step 3: Implement `include/RobustMoments.h`**

```cpp
// RobustMoments.h — pure, header-only Bowley skewness / Moors kurtosis over a
// fixed-size returns window. Zero ACSIL dependency, natively testable
// (tests/cpp/test_robust_moments.cpp), same extraction rationale as
// FeatureScaler.h/TailRiskEngine.h/InformationEngine.h. Implements Kim &
// White (2004)'s recommended robust replacement for moment-based
// skewness/kurtosis, per docs/superpowers/specs/2026-08-13-observation-
// vector-institutional-elevation-spec.md Unit 3.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>

/// Linear-interpolation empirical quantile (R's default "Type 7" method) over
/// an already-sorted array.
template <size_t N>
float EmpiricalQuantile(const std::array<float, N>& sorted, double p) {
    const double idx = p * static_cast<double>(N - 1);
    const size_t lo = static_cast<size_t>(std::floor(idx));
    const size_t hi = static_cast<size_t>(std::ceil(idx));
    if (lo == hi) return sorted[lo];
    const double frac = idx - static_cast<double>(lo);
    return static_cast<float>(static_cast<double>(sorted[lo]) +
                               frac * (static_cast<double>(sorted[hi]) - static_cast<double>(sorted[lo])));
}

/// Bowley (1920) quartile skewness: (Q3 - 2*Q2 + Q1) / (Q3 - Q1).
/// Returns NaN if Q3==Q1 (degenerate window) -- caller must carry-forward,
/// same convention as the existing SKEW_VARIANCE_EPS guard it replaces.
inline float BowleySkewness(std::array<float, 100> returns) {
    std::sort(returns.begin(), returns.end());
    const float q1 = EmpiricalQuantile(returns, 0.25);
    const float q2 = EmpiricalQuantile(returns, 0.50);
    const float q3 = EmpiricalQuantile(returns, 0.75);
    const float denom = q3 - q1;
    if (std::fabs(denom) < 1e-10f) return std::numeric_limits<float>::quiet_NaN();
    return (q3 - 2.0f * q2 + q1) / denom;
}

/// Moors (1988) octile kurtosis: [Q(7/8)-Q(5/8)+Q(3/8)-Q(1/8)] / [Q(6/8)-Q(2/8)].
/// Returns NaN if Q(6/8)==Q(2/8) (degenerate window) -- caller must carry-forward.
inline float MoorsKurtosis(std::array<float, 100> returns) {
    std::sort(returns.begin(), returns.end());
    const float q1_8 = EmpiricalQuantile(returns, 1.0 / 8.0);
    const float q2_8 = EmpiricalQuantile(returns, 2.0 / 8.0);
    const float q3_8 = EmpiricalQuantile(returns, 3.0 / 8.0);
    const float q5_8 = EmpiricalQuantile(returns, 5.0 / 8.0);
    const float q6_8 = EmpiricalQuantile(returns, 6.0 / 8.0);
    const float q7_8 = EmpiricalQuantile(returns, 7.0 / 8.0);
    const float denom = q6_8 - q2_8;
    if (std::fabs(denom) < 1e-10f) return std::numeric_limits<float>::quiet_NaN();
    return (q7_8 - q5_8 + q3_8 - q1_8) / denom;
}
```

- [ ] **Step 4: Run test to verify it passes**

```bash
g++ -std=c++17 -I include tests/cpp/test_robust_moments.cpp -o /tmp/rm_test && /tmp/rm_test
```
Expected: `ALL PASS`. (If the Gaussian-reference tolerance is too tight for a single N=100 draw, widen
it — the assertion that must hold is the outlier-robustness check; the Gaussian-reference check is a
sanity bound, not the load-bearing assertion.)

- [ ] **Step 5: Wire into `CalculateRealizedKurtosis`/`CalculateSkewness`**

In `CalculateRealizedKurtosis` (`StudyHelperFunctions.cpp:2630-2704`), replace the block from
`float m4 = 0.0f;` (line 2668) through `return std::clamp(kurtosis, -5.0f, 50.0f);` (line 2703) — keep
everything above it (the `returns` array build, the degenerate-variance carry-forward guard) unchanged:

```cpp
const float moorsKurtosis = MoorsKurtosis(returns);
float kurtosis;
if (std::isnan(moorsKurtosis)) {
    if (sc.Index > 0 && std::isfinite(prevKurtosis)) {
        return std::clamp(prevKurtosis, -5.0f, 50.0f);
    }
    kurtosis = 1.23f;  // Moors' N(0,1) neutral baseline, replaces the old 3.0f
                        // excess-kurtosis neutral baseline -- different scale.
} else {
    kurtosis = moorsKurtosis;
}

// ELITE FIX #3: Regime adjustment -- unchanged mechanism, now applied to
// Moors kurtosis instead of moment-based kurtosis. Re-tuning these
// multipliers against the new statistic's distribution is Task 7's job,
// not this task's -- do not hand-adjust here.
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
    if (vol_ratio > 1.3f) regime_mult = 1.25f;
    if (vol_ratio < 0.7f) regime_mult = 0.75f;
    kurtosis *= regime_mult;
}

return std::clamp(kurtosis, -5.0f, 50.0f);  // clamp bounds also need Task 7's
                                              // re-derivation against Moors' scale
```

Add `#include "RobustMoments.h"` near the top of `StudyHelperFunctions.cpp`.

In `CalculateSkewness` (`StudyHelperFunctions.cpp:2706-2776`), replace the block from `float stddev =`
(line 2746) through `skewness *= regime_mult;` (line 2771) — keep the `returns` array build and the
degenerate-variance carry-forward return (`if (variance < SKEW_VARIANCE_EPS) { return lastValidSkewness; }`)
unchanged:

```cpp
const float bowleySkewness = BowleySkewness(returns);
float skewness;
if (std::isnan(bowleySkewness)) {
    return lastValidSkewness;
}
skewness = bowleySkewness;

// ELITE FIX #4: Regime adjustment -- unchanged mechanism, now applied to
// Bowley skewness instead of moment-based skewness.
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
```

(`skewness = std::clamp(skewness, -1.5f, 1.5f); lastValidSkewness = skewness; return skewness;` at the
end stays unchanged — Bowley skewness is already bounded in `[-1, 1]` by construction, so the existing
`-1.5/1.5` clamp remains a safe, non-binding outer guard, but Task 7 should confirm this empirically
rather than assume it.)

- [ ] **Step 6: Native compile check + `./build_dll.sh`, then commit**

```bash
g++ -std=c++17 -I include -I include/generated tests/cpp/test_robust_moments.cpp -o /tmp/rm_test && /tmp/rm_test
./build_dll.sh --no-clean
git add include/RobustMoments.h src/StudyHelperFunctions.cpp tests/cpp/test_robust_moments.cpp
git commit -m "fix(microstructure): replace moment-based skewness/kurtosis with Bowley/Moors robust estimators

Kim & White (2004)'s recommended replacement -- moment-based skew/kurtosis
are outlier-sensitive exactly under the fat-tailed conditions they exist to
detect. Threshold re-validation for the three live risk-gate consumers is
Task 7, not this commit -- do not deploy this alone without it."
```

---

### Task 7: Empirical threshold re-validation for the new kurtosis scale

**Why this is its own task, not folded into Task 6:** Moors kurtosis normalizes to ≈1.23 under N(0,1)
instead of the old excess-kurtosis 0 baseline — every downstream threshold tuned against the old scale
is now wrong by construction, and this dim feeds real trading-risk gates. Verified consumers:
- `src/Indicator.cpp:508` — `bool isFragile = (kurtosis > 4.0f);`
- `src/Scoring.cpp:186-201` — sigmoid `1/(1+exp(0.5*(kurtosis-6.0)))`, gated on `kurtosis > 2.5`
- `include/ExecutionParams.h:58-64` — `talebKurtosisCrisisEnter=5.0f`, `talebKurtosisCrisisExit=3.0f`,
  `talebKurtosisCrisisCeiling=0.35f`, `talebKurtosisHaltThreshold=15.0f`
- `src/RiskManager.cpp:805` — literal `ctx.talebKurtosis > 8.0f` fat-tail gate
- `src/RiskManager.cpp:1756-1759` — cascade-boost gate literals (`rk > 2.5f`, `<= 0.0f`, `< 3.0f`)

**This task does not hardcode new threshold values.** New numbers must come from an empirical mapping
between the old and new statistic's distributions on real data — inventing replacement constants
without that mapping would repeat the exact "patch and see what happens" mistake this whole spec exists
to avoid.

**Files:**
- Create: `tools/analyze_kurtosis_threshold_migration.py` (or `.cpp` if a native tool is more consistent
  with this repo's existing tooling — check `tools/` or `scripts/` for precedent before choosing).
- Modify (values only, after the analysis, in a follow-up commit): `include/ExecutionParams.h`,
  `src/Indicator.cpp`, `src/Scoring.cpp`, `src/RiskManager.cpp`.

- [ ] **Step 1: Collect a paired old-vs-new kurtosis sample**

Using a `.btst`/backtest replay run (or a fresh `.context` collection) over a representative historical
ES/MES window, run `CalculateRealizedKurtosis`'s **pre-Task-6** logic and **post-Task-6** logic
side-by-side (e.g. temporarily call both the old moment-based formula and the new `MoorsKurtosis()` on
the same `returns` window at every bar, log both) to build a paired sample of (old_kurtosis,
moors_kurtosis) across a real, varied dataset.

- [ ] **Step 2: Compute the percentile mapping**

For each existing threshold constant (4.0, 6.0, 2.5, 5.0, 3.0, 0.35, 15.0, 8.0), find what percentile of
the *old* kurtosis distribution it corresponds to in the collected sample, then find the *new*
(Moors-kurtosis) value at that same percentile. This percentile-matching (not a linear rescale) is the
principled way to carry a threshold's original intent across a change of statistic.

- [ ] **Step 3: Write `tools/analyze_kurtosis_threshold_migration.py`**

```python
#!/usr/bin/env python3
"""Maps each old-kurtosis threshold constant to its Moors-kurtosis equivalent
via percentile-matching on a paired (old, new) sample collected per Task 7
Step 1. Run after collecting the paired CSV; prints the mapped thresholds --
does not modify any C++ files itself (that's a manual follow-up commit once
the mapping is reviewed).
"""
import sys
import csv
import numpy as np

OLD_THRESHOLDS = {
    "isFragile (Indicator.cpp:508)": 4.0,
    "scoring_sigmoid_gate (Scoring.cpp)": 2.5,
    "scoring_sigmoid_center (Scoring.cpp)": 6.0,
    "talebKurtosisCrisisExit (ExecutionParams.h:59)": 3.0,
    "talebKurtosisCrisisEnter (ExecutionParams.h:58)": 5.0,
    "talebKurtosisHaltThreshold (ExecutionParams.h:64)": 15.0,
    "fatTail_literal (RiskManager.cpp:805)": 8.0,
    "cascade_gate_low (RiskManager.cpp:1758)": 3.0,
    "cascade_gate_high (RiskManager.cpp:1756)": 2.5,
}

def main(csv_path: str) -> None:
    old_vals, new_vals = [], []
    with open(csv_path, newline="") as f:
        for row in csv.DictReader(f):
            old_vals.append(float(row["old_kurtosis"]))
            new_vals.append(float(row["moors_kurtosis"]))
    old_arr = np.array(old_vals)
    new_arr = np.array(new_vals)

    print(f"Paired sample size: {len(old_arr)}")
    for name, old_threshold in OLD_THRESHOLDS.items():
        percentile = float((old_arr <= old_threshold).mean() * 100.0)
        mapped = float(np.percentile(new_arr, percentile))
        print(f"{name}: old={old_threshold} (P{percentile:.1f}) -> new={mapped:.4f}")

if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: analyze_kurtosis_threshold_migration.py <paired_sample.csv>")
        sys.exit(1)
    main(sys.argv[1])
```

- [ ] **Step 4: Run the analysis, review the mapped thresholds against domain judgment**

```bash
python3 tools/analyze_kurtosis_threshold_migration.py <paired_sample.csv>
```
Sanity-check each mapped value makes directional sense (e.g. the halt threshold should still be the
most extreme percentile, crisis-enter above crisis-exit, etc.) before using them.

- [ ] **Step 5: Update the five files' threshold constants to the mapped values, with citations**

Update each site listed above, with a comment citing this task and the percentile-matching method, e.g.:

```cpp
// Percentile-matched to the pre-Task-6 moment-based threshold's original
// intent (was 4.0 excess-kurtosis, ~P97 of the historical distribution) --
// see tools/analyze_kurtosis_threshold_migration.py, run 2026-08-13.
bool isFragile = (kurtosis > <mapped_value>f);
```

- [ ] **Step 6: `./build_dll.sh` + commit**

```bash
./build_dll.sh --no-clean
git add tools/analyze_kurtosis_threshold_migration.py include/ExecutionParams.h src/Indicator.cpp src/Scoring.cpp src/RiskManager.cpp
git commit -m "fix(risk): percentile-match live kurtosis risk-gate thresholds to the Moors-kurtosis scale

Task 6 swapped the underlying statistic; every threshold tuned against the
old moment-based excess-kurtosis scale needed re-deriving against Moors'
different baseline (~1.23 vs 0), via percentile-matching on real paired data
rather than guessed replacement constants."
```

---

### Task 8: Citation corrections (Unit 4, items 1-4)

**Files:** `src/StudyHelperFunctions.cpp` (three comment-only sites), `src/ContextManager.cpp` (one
comment-only site). Zero behavior change — comments only.

- [ ] **Step 1: `StudyHelperFunctions.cpp:2675-2679`** (kurtosis bias-correction comment) — change:
```cpp
// Sample Kurtosis Formula (Unbiased Estimator - Sierra Chart's approach)
```
to:
```cpp
// Sample Kurtosis Formula (Unbiased Estimator - Joanes & Gill 1998,
// "Comparing measures of sample skewness and kurtosis", J. Royal
// Statistical Society Series D -- matches Excel/SPSS/Minitab's G2)
```
(Note: this comment sits in the pre-Task-6 code. If Task 6 has already landed, this specific block will
have been replaced by `MoorsKurtosis()` — apply this citation fix to whichever commit lands first;
if Task 6 already shipped, this citation belongs on `RobustMoments.h`'s history/rationale instead, not
a dead comment. Check before editing.)

- [ ] **Step 2: `ContextManager.cpp:693-725`** ("Taleb Cliff" Chandelier distance) — add above the
`elderChandelierATR` calculation:
```cpp
// "Taleb Cliff": distance-to-invalidation framing overlaid on the canonical
// Chandelier Exit parameters (Chuck LeBeau) -- 22-period lookback, 3.0x ATR
// stop. Credit LeBeau for the indicator itself; "Taleb Cliff" is this
// codebase's interpretive name for using it as a risk/regime signal, not a
// claim that Taleb defined these parameters.
```

- [ ] **Step 3: `StudyHelperFunctions.cpp:2327-2330`** (`CalculatePathEfficiencySNR`) — change:
```cpp
/// Spectral Entropy - ELITE: O(1) Rolling Window Implementation (Reference: Elder's Efficiency Ratio)
```
to:
```cpp
/// Path-Efficiency SNR (Kaufman's Efficiency Ratio, squared) -- ELITE: O(1)
/// Rolling Window Implementation. Reference: Perry Kaufman, "Trading Systems
/// and Methods" (1998) -- NOT a Shannon-entropy measure despite the historical
/// "Spectral Entropy" name; that label is dropped here as inaccurate.
```

- [ ] **Step 4: `StudyHelperFunctions.cpp:2757`** (skewness regime comment) — change:
```cpp
// ELITE FIX #4: Regime adjustment (Wyckoff-aligned)
```
to:
```cpp
// ELITE FIX #4: Regime adjustment (house-tuned multiplier, no direct
// methodological link to Wyckoff's framework -- see docs/superpowers/specs/
// 2026-08-12-gang-literature-grounding-spec.md Pillar 3)
```

- [ ] **Step 5: Compile check (comments only, but confirm no syntax breakage) + commit**

```bash
./build_dll.sh --no-clean
git add src/StudyHelperFunctions.cpp src/ContextManager.cpp
git commit -m "docs(microstructure): correct citations -- Joanes & Gill, LeBeau, Kaufman; drop unsupported Wyckoff label

Comment-only, zero behavior change. Per docs/superpowers/specs/2026-08-12-
gang-literature-grounding-spec.md's citation-accuracy findings."
```

---

### Task 9: `StructureEngine::GetFractalDimension()` Sevcik-mislabel fix (Unit 4, item 5)

**Files:** `src/StructureEngine.cpp:85-109`. Comment-only — the formula itself stays (a raw
path-length/displacement roughness ratio), only the misleading Sevcik citation is removed.

- [ ] **Step 1: Fix the misleading comment**

Replace:
```cpp
    // Simple robust estimator: Path Length / Log-Range
    // D = log(L) / log(d) is Hausdorff, but for time series we use Sevcik's or similar.
    // Let's use a simpler "Roughness" proxy:
```
with:
```cpp
    // Path-length/displacement roughness ratio -- NOT Sevcik's method (that's
    // a log-log calculation, 1 + ln(L)/ln(2*segments); this is an unlogged
    // ratio with no citable name). Corrected 2026-08-13, was previously
    // mislabeled -- see docs/superpowers/specs/2026-08-12-gang-literature-
    // grounding-spec.md Finding 3.
```

- [ ] **Step 2: `./build_dll.sh` + commit**

```bash
./build_dll.sh --no-clean
git add src/StructureEngine.cpp
git commit -m "docs(structure-engine): correct GetFractalDimension()'s misleading Sevcik citation

Formula is an unlogged path-length/displacement ratio, not Sevcik's log-log
calculation -- comment-only fix, zero behavior change."
```

---

### Task 10: `paretoRot` naming-mismatch clarifying comment (Unit 4, item 6)

**Scope decision:** the FlatBuffers wire field `paretoRot` (`../schema/mts_schema.fbs`) is **not**
renamed in this task — that's a wire-protocol change requiring `regenerate_schema.sh` and `lbrnet`-side
coordination, out of scope for a "zero functional risk" hygiene unit. Only the C++-side naming
confusion gets documented.

**Files:** `include/ContextManager.h:414`, `src/ContextManager.cpp:685`.

- [ ] **Step 1: Add a clarifying comment at both sites**

`include/ContextManager.h:414`:
```cpp
float paretoRot = 0.0f;  // NOTE: holds a Mandelbrot-pillar metric (fractal
                          // roughness from StructureEngine::GetFractalDimension()),
                          // not a Pareto-pillar one -- field name predates this
                          // finding (docs/superpowers/specs/2026-08-12-gang-
                          // literature-grounding-spec.md Finding 4). Left
                          // unrenamed: the wire-schema field this feeds is a
                          // cross-repo contract (../schema/mts_schema.fbs),
                          // out of scope for a comment-only hygiene fix.
```

`src/ContextManager.cpp:685`:
```cpp
    // See naming-mismatch note at ContextManager.h:414 -- this assigns a
    // Mandelbrot-pillar value into a Pareto-named field.
    m_latestInstitutionalMetrics.paretoRot = m_structureEngine.GetFractalDimension();
```

- [ ] **Step 2: `./build_dll.sh` + commit**

```bash
./build_dll.sh --no-clean
git add include/ContextManager.h src/ContextManager.cpp
git commit -m "docs(context-manager): document paretoRot's Mandelbrot/Pareto naming mismatch

Comment-only -- actual field rename is a wire-schema change (out of scope,
needs regenerate_schema.sh + lbrnet coordination, tracked separately)."
```

---

### Task 11: Remove three confirmed-dead functions (Unit 4, item 7)

**Files:** `include/StructureEngine.h` (+ `.cpp`), `include/InformationEngine.h`.

- [ ] **Step 1: Re-confirm zero call sites (repo may have changed since the Gang spec's original sweep)**

```bash
grep -rn "GetRecurrenceRate\b" --include=*.cpp --include=*.h . | grep -v "include/StructureEngine.h:32\|include/InformationEngine.h:142\|StructureEngine.cpp:"
grep -rn "GetFisherInformation\b" --include=*.cpp --include=*.h . | grep -v "include/InformationEngine.h:112"
```
Expected: no output beyond the declaration/definition sites themselves — confirms zero live callers. If
any call site appears, stop and do not delete that function; report the discrepancy instead.

- [ ] **Step 2: Delete `StructureEngine::GetRecurrenceRate()`**

Remove the declaration (`include/StructureEngine.h:32`) and its definition in `src/StructureEngine.cpp`
(the block ending just before `GetFractalDimension()` at line 85, i.e. lines ~60-83 per Step 1's earlier
read showing the histogram/mode-bin logic immediately preceding `GetFractalDimension`).

- [ ] **Step 3: Delete `InformationEngine::GetFisherInformation()` and `InformationEngine::GetRecurrenceRate()`**

Remove both method bodies from `include/InformationEngine.h` (lines 112 and 142 per the confirmed
locations — read the surrounding braces carefully to remove exactly one complete method each, no more).

- [ ] **Step 4: `./build_dll.sh` (must succeed — confirms no hidden caller was missed) + commit**

```bash
./build_dll.sh --no-clean
git add include/StructureEngine.h src/StructureEngine.cpp include/InformationEngine.h
git commit -m "refactor(microstructure): remove three confirmed-dead functions

StructureEngine::GetRecurrenceRate(), InformationEngine::GetFisherInformation(),
InformationEngine::GetRecurrenceRate() -- zero call sites confirmed immediately
before deletion. Per docs/superpowers/specs/2026-08-12-gang-literature-
grounding-spec.md Finding 6."
```

---

### Task 12: DFA/Hurst empirical bias/CI correction spike (Unit 6a)

**Output:** a finding, not necessarily code — a Changelog entry in the Gang spec, and either a
follow-on task (if a cheap correction is found) or a documented negative result.

**Files:** `tools/dfa_bias_montecarlo.py` (create), `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md` (Changelog append only).

- [ ] **Step 1: Write the Monte Carlo bias-characterization script**

```python
#!/usr/bin/env python3
"""Monte Carlo DFA bias/variance at N~100, following Kristoufek (2010)'s
methodology: simulate fractional Brownian motion at known Hurst values,
run this codebase's DFA parameters (minScale=8, N~100) against each, see
if a stable bias-correction curve emerges. Output-only spike -- does not
modify production code. See docs/superpowers/specs/2026-08-13-observation-
vector-institutional-elevation-spec.md Unit 6a.
"""
import numpy as np

def simulate_fbm(n: int, hurst: float, rng: np.random.Generator) -> np.ndarray:
    # Hosking method or simple approximation via cumulative sum of a
    # fractionally-differenced Gaussian series -- use an established library
    # (e.g. `fbm` package, `pip install fbm`) rather than hand-rolling, to
    # keep the simulation itself trustworthy.
    from fbm import FBM
    f = FBM(n=n, hurst=hurst, length=1, method="daviesharte")
    return f.fbm()

def dfa(series: np.ndarray, min_scale: int = 8) -> float:
    # Mirror this codebase's DFA implementation
    # (src/StudyHelperFunctions.cpp CalculateHurstExponent) parameter-for-
    # parameter -- window length, min_scale=8, max_scale=length/4 -- so the
    # simulated bias applies to OUR estimator, not a generic textbook one.
    n = len(series)
    max_scale = max(min_scale + 1, n // 4)
    y = np.cumsum(series - series.mean())
    scales = np.unique(np.logspace(np.log10(min_scale), np.log10(max_scale), 15).astype(int))
    flucts = []
    for scale in scales:
        segments = n // scale
        if segments < 1:
            continue
        rms = []
        for seg in range(segments):
            chunk = y[seg * scale:(seg + 1) * scale]
            t = np.arange(len(chunk))
            coeffs = np.polyfit(t, chunk, 1)
            trend = np.polyval(coeffs, t)
            rms.append(np.sqrt(np.mean((chunk - trend) ** 2)))
        flucts.append(np.mean(rms))
    log_scales = np.log(scales[:len(flucts)])
    log_flucts = np.log(flucts)
    slope, _ = np.polyfit(log_scales, log_flucts, 1)
    return slope

def main():
    rng = np.random.default_rng(2026)
    n_trials = 200
    for true_hurst in [0.3, 0.4, 0.5, 0.6, 0.7]:
        estimates = [dfa(simulate_fbm(100, true_hurst, rng)) for _ in range(n_trials)]
        bias = np.mean(estimates) - true_hurst
        std = np.std(estimates)
        print(f"true H={true_hurst}: estimated mean={np.mean(estimates):.3f} "
              f"bias={bias:+.3f} std={std:.3f}")

if __name__ == "__main__":
    main()
```

- [ ] **Step 2: Run it, inspect whether a stable bias curve emerges**

```bash
pip install fbm numpy
python3 tools/dfa_bias_montecarlo.py
```

- [ ] **Step 3: Record the finding in the Gang spec's Changelog**

If a stable, monotonic bias-vs-true-H curve emerges (bias magnitude and direction consistent across
repeated runs), append a Changelog entry documenting the curve and file a follow-on task to implement
it as a correction (do not implement it in this spike). If no stable curve emerges (noisy/inconsistent
bias across runs), append a Changelog entry documenting that negative result explicitly — "still
under-powered, no cheap correction found, window-widening remains the only lever" — so this doesn't get
silently re-investigated later without knowing it was already checked.

- [ ] **Step 4: Commit**

```bash
git add tools/dfa_bias_montecarlo.py docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md
git commit -m "spike(mandelbrot): DFA bias/CI Monte Carlo per Kristoufek (2010) methodology

Investigation only, no production code changed. Finding recorded in the Gang
spec's Changelog per Unit 6a."
```

---

### Task 13: Hill estimator intraday seasonality spike (Unit 6b)

**Output:** a finding, not necessarily code.

**Files:** `tools/hill_intraday_seasonality.py` (create), Gang spec Changelog.

- [ ] **Step 1: Write the seasonality-check script**

```python
#!/usr/bin/env python3
"""Checks whether Hill tail-index (dim 9) varies systematically by session
time-of-day on already-collected .context data. Reads MarketObservation.
Observation() directly (ContextRecord's narrower 3-field dataclass doesn't
carry tail_index) -- same low-level struct-framing pattern this session
already used to analyze dims 1/2/4/10/11/12, applied to dim 9 instead.
See docs/superpowers/specs/2026-08-13-observation-vector-institutional-
elevation-spec.md Unit 6b.
"""
import struct
import sys
from collections import defaultdict
from datetime import datetime, timezone
from pathlib import Path

sys.path.insert(0, "../lbrnet")
from lbrnet.generated.MTS.Schema.MarketObservation import MarketObservation
from lbrnet.generated.MTS.Schema.SystemState import SystemState
from lbrnet.generated.MTS.Training.FileMetadata import FileMetadata

import numpy as np


def time_bucket(timestamp_us: int) -> str:
    dt = datetime.fromtimestamp(timestamp_us / 1e6, tz=timezone.utc)
    hour = dt.hour
    if hour < 14: return "pre-open"
    if hour < 15: return "open"
    if 15 <= hour < 19: return "midday"
    if 19 <= hour < 21: return "close"
    return "overnight"


def read_stream_header(path: Path) -> int:
    with path.open("rb") as f:
        magic = f.read(4)
        if magic != b"LBRN":
            raise ValueError(f"Invalid .context file header: {magic!r}")
        meta_size = struct.unpack("<I", f.read(4))[0]
        if meta_size > 0:
            meta = f.read(meta_size)
            FileMetadata.GetRootAsFileMetadata(meta, 0)
        return f.tell()


def iter_tail_index(path: str, max_pairs: int):
    p = Path(path)
    start_offset = read_stream_header(p)
    with p.open("rb") as f:
        f.seek(start_offset)
        pending_mo = None
        record_idx = 0
        n = 0
        while n < max_pairs:
            size_bytes = f.read(4)
            if len(size_bytes) < 4:
                break
            payload_size = struct.unpack("<I", size_bytes)[0]
            payload = f.read(payload_size)
            if len(payload) < payload_size:
                break
            if record_idx % 2 == 0:
                pending_mo = MarketObservation.GetRootAsMarketObservation(payload, 0)
            else:
                ss = SystemState.GetRootAsSystemState(payload, 0)
                if pending_mo is not None and int(ss.SequenceId()) == int(pending_mo.SequenceId()):
                    obs = pending_mo.Observation()
                    if obs is not None:
                        yield int(pending_mo.TimestampUs()), float(obs.TailIndex())
                        n += 1
                pending_mo = None
            record_idx += 1


def main(context_path: str, max_pairs: int = 200_000) -> None:
    buckets = defaultdict(list)
    for timestamp_us, tail_index in iter_tail_index(context_path, max_pairs):
        buckets[time_bucket(timestamp_us)].append(tail_index)

    print(f"{'bucket':<12} {'n':>8} {'mean':>10} {'std':>10}")
    for bucket, values in sorted(buckets.items()):
        arr = np.array(values)
        print(f"{bucket:<12} {len(arr):>8} {arr.mean():>10.4f} {arr.std():>10.4f}")


if __name__ == "__main__":
    main(sys.argv[1])
```

- [ ] **Step 2: Run it against existing collected `.context` data, using established tooling conventions**

```bash
source /home/rcruz/anaconda3/etc/profile.d/conda.sh && mamba activate mts
cd /home/rcruz/devel/VSCode/lbrnet
python3 ../MindfulTrader/tools/hill_intraday_seasonality.py /mnt/c/SierraChart2/Data/<latest>.context
```
Compare mean/std of `tail_index` (dim 9) across time-of-day buckets — a systematic difference (not just
noise) indicates the seasonality effect the spec flagged as an open gap.

- [ ] **Step 3: Record the finding in the Gang spec's Changelog**

Either a deseasonalization recommendation (if a real effect is found) filed as a follow-on task, or a
documented "not a material effect at this cadence" conclusion — don't leave this unresolved silently.

- [ ] **Step 4: Commit**

```bash
git add tools/hill_intraday_seasonality.py docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md
git commit -m "spike(pareto): check Hill tail-index for intraday seasonality across session time-of-day

Investigation only, no production code changed. Finding recorded in the Gang
spec's Changelog per Unit 6b. Uses lbrnet/mamba-mts tooling conventions
(read-only .context sampling, per established repo boundary)."
```
