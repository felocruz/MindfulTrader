# Tick-Native Micro-Asymmetry Fix Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace dim 7 (`micro_asymmetry`)'s fragile Time-&-Sales-scan computation with a
direct, reliable read of Sierra Chart's own per-bar `sc.BidVolume`/`sc.AskVolume` aggregates, and
delete the dead-on-arrival code this replaces.

**Architecture:** A new tiny, header-only, zero-ACSIL-dependency pure function
(`ofae::ComputeMicroAsymmetry`) implements `(ask-bid)/(ask+bid)` with carry-forward on a true
no-trade bar. `StudyHelperFunctions.cpp`'s `UpdateObservationVectorSubgraphs` calls it with
`sc.AskVolume[sc.Index]`/`sc.BidVolume[sc.Index]` — the same arrays `VolumeIndicator` already reads
successfully every bar (`TripleScreen3.cpp:637-638`) — instead of the removed
`CalculateMicroAsymmetry`/`CalculateMicroAsymmetryFromTimeAndSales`.

**Tech Stack:** C++17, ACSIL (Sierra Chart Studies Interface), header-only pure-logic extraction
pattern (matches `FeatureScaler.h`/`VolumeProfileEngine.h`), manual `g++`-compiled native test
binaries (no gtest in this repo).

## Global Constraints

- No heap allocations in the hot path (per-tick/per-bar update code) — CLAUDE.md Performance Rules.
- No `mts_schema.fbs` changes — dim 7 keeps its existing name/position/semantics
  (`docs/superpowers/specs/2026-08-12-tick-native-toxicity-illiquidity-design.md`).
- Before removing any symbol, search the full repo for usages in `.h`, `.cpp`, and PCH files
  (CLAUDE.md Code Safety Rules) — done for this plan; see Task 3.
- Build must go through `./build_dll.sh` — never raw `cmake`/`ninja`/`flatc`.
- "Factory not warehouse": dead code this change makes obsolete is deleted in the same pass, not
  left in place (user directive, this session).

---

## File Structure

- **Create:** `include/OrderFlowAsymmetryEngine.h` — pure `(ask,bid,lastValid) -> asymmetry`
  function. No Sierra Chart dependency, natively testable, mirrors `FeatureScaler.h`'s extraction
  rationale.
- **Create:** `tests/cpp/test_order_flow_asymmetry_engine.cpp` — native characterization tests for
  the above, mirrors `tests/cpp/test_feature_scaler.cpp`'s structure exactly (no test framework,
  `check()`/`g_failures`, manual `g++` build).
- **Modify:** `include/MindfulTraderConstants.h` — one new persistent-variable slot.
- **Modify:** `src/StudyHelperFunctions.cpp` — `UpdateObservationVectorSubgraphs` calls the new
  helper instead of `CalculateMicroAsymmetry`; delete both dead functions and the stale doc-comment
  line that references the deleted function by name.
- **Modify:** `include/StudyHelperFunctions.h` — delete the two now-dead function declarations.
- **Modify:** `docs/PENDING_USER_ACTIONS.md` — append the dim-11 empirical re-verification step
  (requires a live Sierra Chart replay, not something this plan's tasks can execute).

---

### Task 1: Pure order-flow-asymmetry function + native unit test

**Files:**
- Create: `include/OrderFlowAsymmetryEngine.h`
- Test: `tests/cpp/test_order_flow_asymmetry_engine.cpp`

**Interfaces:**
- Produces: `ofae::ComputeMicroAsymmetry(float askVolume, float bidVolume, float lastValidValue) -> float`
  — used by Task 2.

- [ ] **Step 1: Write the failing test**

Create `tests/cpp/test_order_flow_asymmetry_engine.cpp`:

```cpp
// test_order_flow_asymmetry_engine.cpp — characterization tests for
// OrderFlowAsymmetryEngine's pure (ask,bid) -> asymmetry formula, extracted
// so it can be natively unit-tested without Sierra Chart/ACSIL deps
// (docs/superpowers/specs/2026-08-12-tick-native-toxicity-illiquidity-design.md).
//
// Build & run natively:
//   g++ -std=c++17 -I include tests/cpp/test_order_flow_asymmetry_engine.cpp -o /tmp/ofae_test && /tmp/ofae_test

#include "OrderFlowAsymmetryEngine.h"

#include <cmath>
#include <cstdio>

namespace {

int g_failures = 0;

void check(const char* name, bool ok) {
    if (ok) {
        std::printf("  PASS  %s\n", name);
    } else {
        ++g_failures;
        std::printf("  FAIL  %s\n", name);
    }
}

bool approx(float a, float b, float tol = 1e-6f) {
    return std::fabs(a - b) <= tol;
}

}  // namespace

int main() {
    std::printf("OrderFlowAsymmetryEngine unit tests\n");

    check("balanced_flow_is_zero",
          approx(ofae::ComputeMicroAsymmetry(50.0f, 50.0f, 0.0f), 0.0f));

    check("all_buying_is_plus_one",
          approx(ofae::ComputeMicroAsymmetry(100.0f, 0.0f, 0.0f), 1.0f));

    check("all_selling_is_minus_one",
          approx(ofae::ComputeMicroAsymmetry(0.0f, 100.0f, 0.0f), -1.0f));

    check("partial_buy_skew",
          approx(ofae::ComputeMicroAsymmetry(75.0f, 25.0f, 0.0f), 0.5f));

    check("no_trade_bar_with_no_prior_value_returns_neutral",
          approx(ofae::ComputeMicroAsymmetry(0.0f, 0.0f, 0.0f), 0.0f));

    check("no_trade_bar_carries_forward_prior_value",
          approx(ofae::ComputeMicroAsymmetry(0.0f, 0.0f, 0.7f), 0.7f));

    check("negative_no_trade_carry_forward",
          approx(ofae::ComputeMicroAsymmetry(0.0f, 0.0f, -0.35f), -0.35f));

    std::printf("\n%d failure(s)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -I include tests/cpp/test_order_flow_asymmetry_engine.cpp -o /tmp/ofae_test`
Expected: FAIL — `fatal error: OrderFlowAsymmetryEngine.h: No such file or directory`

- [ ] **Step 3: Write minimal implementation**

Create `include/OrderFlowAsymmetryEngine.h`:

```cpp
// OrderFlowAsymmetryEngine.h — pure, header-only order-flow asymmetry from
// Sierra-Chart-maintained per-bar bid/ask volume. Extracted so it can be
// natively unit-tested (tests/cpp/test_order_flow_asymmetry_engine.cpp) —
// same rationale as FeatureScaler.h/VolumeProfileEngine.h: zero Sierra
// Chart/ACSIL dependency.
//
// Replaces the removed CalculateMicroAsymmetry/CalculateMicroAsymmetryFromTimeAndSales
// (StudyHelperFunctions.cpp), which manually rescanned sc.GetTimeAndSales() with
// hand-rolled sequence tracking and returned NaN (sanitized to a misleading exact
// 0.0) on ~45% of bars — a replay-mechanics reliability gap, not a data-richness
// one (docs/superpowers/plans/2026-08-12-denali-data-feed-proxy-audit.md Part 3).
// sc.BidVolume/sc.AskVolume are the same per-bar arrays VolumeIndicator already
// reads successfully every bar in the identical replay sessions where the T&S
// scan failed.

#pragma once

namespace ofae {

// Order-flow asymmetry in [-1, 1]: +1 = all buying (aggressor at ask),
// -1 = all selling (aggressor at bid), 0 = balanced.
// On a true no-trade bar (askVolume == bidVolume == 0 — confirmed by direct
// .scid byte inspection to be essentially unseen intraday for ES/MES, but
// free to guard), carries the last valid reading forward instead of
// fabricating a misleading exact-zero "balanced" reading.
inline float ComputeMicroAsymmetry(float askVolume, float bidVolume, float lastValidValue) {
    const float total = askVolume + bidVolume;
    if (total <= 0.0f) {
        return lastValidValue;
    }
    return (askVolume - bidVolume) / total;
}

}  // namespace ofae
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -I include tests/cpp/test_order_flow_asymmetry_engine.cpp -o /tmp/ofae_test && /tmp/ofae_test`
Expected: `0 failure(s)`, exit code 0, all 7 `PASS` lines.

- [ ] **Step 5: Commit**

```bash
git add include/OrderFlowAsymmetryEngine.h tests/cpp/test_order_flow_asymmetry_engine.cpp
git commit -m "feat(microstructure): add pure order-flow-asymmetry engine with native test"
```

---

### Task 2: Wire the new helper into the observation-vector pipeline

**Files:**
- Modify: `include/MindfulTraderConstants.h:78-86` (`PersistentVar_AdaptiveCalculators` namespace)
- Modify: `src/StudyHelperFunctions.cpp:2956-2977` (`UpdateObservationVectorSubgraphs`)

**Interfaces:**
- Consumes: `ofae::ComputeMicroAsymmetry(float, float, float) -> float` (Task 1).
- Produces: `Subgraph_MicroAsymmetry[sc.Index]` now sourced from `sc.AskVolume`/`sc.BidVolume`
  instead of `CalculateMicroAsymmetry` — consumed unchanged downstream by `TripleScreen3.cpp:758,774,1388`
  (no changes needed there; they already just read `Subgraph_MicroAsymmetry[sc.Index]`).

- [ ] **Step 1: Add the persistent-variable slot**

In `include/MindfulTraderConstants.h`, inside `namespace PersistentVar_AdaptiveCalculators` (currently
ends at line 86 with `LAST_AMIHUD_SAMPLE_INDEX = 26;`), add one new line:

```cpp
    const int MICRO_ASYMMETRY_LAST_VALID_VALUE = 27; // Last valid (has-trade) order-flow asymmetry for graceful degradation on a no-trade bar
```

- [ ] **Step 2: Replace the `CalculateMicroAsymmetry` call**

In `src/StudyHelperFunctions.cpp`, `UpdateObservationVectorSubgraphs`'s non-warmup branch currently
reads (line 2967):

```cpp
        Subgraph_MicroAsymmetry[sc.Index] = CalculateMicroAsymmetry(sc, Subgraph_VolumeSMA[sc.Index], adaptive_window_n);
```

Replace with:

```cpp
        float& lastValidMicroAsymmetry = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::MICRO_ASYMMETRY_LAST_VALID_VALUE);
        Subgraph_MicroAsymmetry[sc.Index] = ofae::ComputeMicroAsymmetry(
            static_cast<float>(sc.AskVolume[sc.Index]),
            static_cast<float>(sc.BidVolume[sc.Index]),
            lastValidMicroAsymmetry);
        lastValidMicroAsymmetry = Subgraph_MicroAsymmetry[sc.Index];
```

Add the include near the top of `src/StudyHelperFunctions.cpp` (alongside its existing includes):

```cpp
#include "OrderFlowAsymmetryEngine.h"
```

- [ ] **Step 3: Build**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds, no new warnings from this file.

- [ ] **Step 4: Commit**

```bash
git add include/MindfulTraderConstants.h src/StudyHelperFunctions.cpp
git commit -m "feat(microstructure): source micro_asymmetry from sc.BidVolume/AskVolume"
```

---

### Task 3: Delete the dead Time-&-Sales micro-asymmetry code

Per the "factory not warehouse" directive — this code has no remaining caller after Task 2 and no
documented independent value (its "Phase 2 price-action fallback" docstring claim was never
implemented; it just returned `NaN`). Confirmed via repo-wide grep: the only call sites are the
internal one (`CalculateMicroAsymmetry` → `CalculateMicroAsymmetryFromTimeAndSales`) and the one this
plan just removed (`UpdateObservationVectorSubgraphs`) — no other file references either function.

**Files:**
- Modify: `src/StudyHelperFunctions.cpp:2637-2727` (delete both function bodies), `:3030` (fix stale
  doc-comment reference)
- Modify: `include/StudyHelperFunctions.h:260,265` (delete both declarations)

**Interfaces:** None — pure deletion, no other task depends on these symbols existing.

- [ ] **Step 1: Delete the function declarations**

In `include/StudyHelperFunctions.h`, delete these two lines (currently 260 and 265):

```cpp
float CalculateMicroAsymmetryFromTimeAndSales(SCStudyInterfaceRef sc);
```
```cpp
float CalculateMicroAsymmetry(SCStudyInterfaceRef sc, float volume_sma_20, int lookback_n = 20);
```

- [ ] **Step 2: Delete the function bodies**

In `src/StudyHelperFunctions.cpp`, delete the full text from the `CalculateMicroAsymmetryFromTimeAndSales`
function's opening comment (line 2637: `float CalculateMicroAsymmetryFromTimeAndSales(SCStudyInterfaceRef sc) {`)
through the closing brace of `CalculateMicroAsymmetry` (line 2727: `}`) — both functions, contiguous,
inclusive.

- [ ] **Step 3: Fix the stale doc-comment reference**

In `src/StudyHelperFunctions.cpp`, the "INTEGRATION GUIDE" doc-comment block (originally around
line 3030, now ~90 lines earlier after Step 2's deletion) contains:

```cpp
///       features[1] = CalculateMicroAsymmetry(...);   // Uses asym_window
```

Delete this line — it references a function that no longer exists.

- [ ] **Step 4: Verify no remaining references**

Run: `grep -rn "CalculateMicroAsymmetry" src/ include/`
Expected: no output (zero matches).

- [ ] **Step 5: Build**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds.

- [ ] **Step 6: Commit**

```bash
git add src/StudyHelperFunctions.cpp include/StudyHelperFunctions.h
git commit -m "refactor(microstructure): delete dead Time-and-Sales micro-asymmetry code"
```

---

### Task 4: Full clean build + regression check

**Files:** None new — verification only.

- [ ] **Step 1: Full clean build**

Run: `./build_dll.sh`
Expected: build succeeds with no errors. This is the first full clean build since Tasks 1-3; catches
any stale object file or include-order issue the incremental builds could have masked.

- [ ] **Step 2: Run the native unit test suite**

Run:
```bash
g++ -std=c++17 -I include tests/cpp/test_order_flow_asymmetry_engine.cpp -o /tmp/ofae_test && /tmp/ofae_test
g++ -std=c++17 -I include -I include/generated tests/cpp/test_feature_scaler.cpp -o /tmp/fs_test && /tmp/fs_test
```
Expected: both exit 0, zero failures. The second command is a regression check — this plan doesn't
touch `FeatureScaler.h`, so its existing characterization tests must still pass unchanged.

- [ ] **Step 3: Commit** (only if Step 1 or 2 required a fix)

If both steps passed clean with no changes needed, there is nothing to commit here — skip.

---

### Task 5: Record the dim-11 empirical re-verification as a pending hands-on action

Per the design doc: dim 11 (`amihud_illiquidity`)'s zero-collapse may already be resolved by the
existing `FeatureScaler.h` `AMIHUD_ABSOLUTE_FLOOR` fix — this can only be settled by collecting a
fresh `.context` file against the DLL built in Task 4 and re-running the octile zero-ratio analysis.
That requires driving Sierra Chart's replay UI, which is outside what this plan's tasks can execute
directly — recorded as a pending hands-on action, matching this repo's existing convention
(`docs/PENDING_USER_ACTIONS.md`).

**Files:**
- Modify: `docs/PENDING_USER_ACTIONS.md`

- [ ] **Step 1: Append the verification step**

Add a new numbered section at the end of `docs/PENDING_USER_ACTIONS.md`:

```markdown
## 6. Dim 11 (`amihud_illiquidity`) empirical re-verification — after this DLL is deployed

From `docs/superpowers/specs/2026-08-12-tick-native-toxicity-illiquidity-design.md`: dim 11's
FeatureScaler zero-collapse may already be resolved by the existing `AMIHUD_ABSOLUTE_FLOOR` fix in
`FeatureScaler.h` — the `.context` file this was diagnosed from may predate that fix reaching a
deployed build.

1. Deploy the DLL built by this plan's Task 4 and run a historical replay through
   `EventDataCollectorStudy` to produce a fresh `.context` file.
2. Run `context_preflight.py`'s octile zero-ratio analysis (see
   `docs/superpowers/plans/2026-08-12-observation-vector-sentinel-collapse-audit.md` §2 for the
   method) against the fresh file, specifically for dim 11.
3. **If dim 11's zero-ratio now shows the same warmup-decay-then-flatten shape dim 0 already
   shows** (flattening to a low single-digit percentage, not a persistent ~40%): no further C++
   change needed — close this out.
4. **If it doesn't**: apply the carry-forward pattern from
   `docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md` D1
   (`CalculateAmihudIlliquidity`'s `count < 2` branch) as originally speced.
```

- [ ] **Step 2: Commit**

```bash
git add docs/PENDING_USER_ACTIONS.md
git commit -m "docs: record dim-11 empirical re-verification as a pending hands-on action"
```
