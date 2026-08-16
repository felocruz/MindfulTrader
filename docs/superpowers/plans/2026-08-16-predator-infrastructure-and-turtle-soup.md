# Predator Infrastructure, Turtle Soup Option A, and Classifier-Consumption Scaffold Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build the shared `PredatorContext`/`PredatorFusion` infrastructure, bring Turtle Soup to
Predator-grade via a tick-reactive geometric heuristic (Option A), and scaffold the C++-side
classifier-consumption path (config-driven parameter loading + hand-crafted inference) so a future
lbrnet-rooted session can drop in real trained parameters without any C++ redesign.

**Architecture:** A unified `PredatorContext` struct (composing the existing `LocalRiskContext` +
HMM regime state) is assembled once per tick by `ContextManager`. Fusion decisions are plain free
functions (no virtual dispatch) gated by a bitmask dispatch mechanism mirroring `IndicatorManager`'s
proven dirty-mask idiom. Turtle Soup's `DetectTurtleSoup()` raw sensor is reused as-is; only its
call-site gating changes from once-per-closed-bar to tick-reactive, with fusion logic modeled on
Kangaroo Tail's already-audited direction-discriminating pattern. The classifier-consumption scaffold
(`ClassifierParams`, `EvaluateTurtleSoupOptionB()`) is built and tested against synthetic parameters
now; it is not wired to the active call site until a real trained model is validated in the Python
twin (a separate, future decision, out of scope here).

**Tech Stack:** C++17, ACSIL (Sierra Chart), existing native test convention
(`g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_X.cpp -o /tmp/tX && /tmp/tX`), the project's
existing `nlohmann::json`-based config-loading idiom (`FeatureScaler::LoadConfig()`).

**Spec:** `docs/superpowers/specs/2026-08-16-predator-context-fusion-infrastructure-spec.md` (Tasks 1-3),
`docs/superpowers/specs/2026-08-16-turtle-soup-predator-ization-spec.md` (Tasks 4, 6-7),
`docs/superpowers/specs/2026-08-16-predator-decision-contract-execution-risk-framework.md` (governing
contract), `docs/superpowers/specs/2026-08-16-ects-prefix-training-infrastructure-spec.md` (Task 6's
deployment guidance).

## Global Constraints

- **No schema changes** (`mts_schema.fbs` untouched).
- **No virtual dispatch anywhere in this plan** — every new interface is a plain struct + free
  function, matching this codebase's DOD convention.
- **No new C++ third-party dependency** — `nlohmann::json` is already linked (used by
  `FeatureScaler.h`); no ML runtime library is introduced.
- **Zero observable live-behavior change from Tasks 1-3** (infrastructure only) — `FuseTauStar()`
  computes a value but is not wired to take live exit action (matches the existing, deliberate
  telemetry-only handling of `TRAP_LONG`/`TRAP_SHORT` at `PositionManager.cpp:1682-1688` — this plan
  does not change that gating decision).
- **Task 4 (Turtle Soup Option A) is the only task in this plan that changes live trading behavior.**
  Tasks 1-3, 5-7 are additive/infrastructure with no active-call-site change.
- **`./build_dll.sh` must stay green after every task**, and native unit tests must pass before any
  commit.
- Follow the standing **dead-code-removal mandate**: when this plan removes gating logic (Task 4),
  remove it outright — no renamed/quieted leftovers, no "removed" comments preserving dead structure.

---

### Task 1: `PredatorContext` — Unified Macro Context

**Files:**
- Create: `include/PredatorContext.h`
- Modify: `include/ContextManager.h:381` (add accessor near `GetLocalRiskContext()`)
- Modify: `src/ContextManager.cpp` (implement `GetPredatorContext()`)
- Test: `tests/cpp/test_predator_context.cpp`

**Interfaces:**
- Produces: `struct PredatorContext { LocalRiskContext gang; HMMStateEnum regime; bool inPosition; uint64_t applicabilityMask; };`
  and `const PredatorContext& ContextManager::GetPredatorContext() const;` — every later task in this
  plan consumes this exact struct and accessor.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/cpp/test_predator_context.cpp — standalone unit tests for PredatorContext assembly.
//
// Build: g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_predator_context.cpp -o /tmp/t_pc && /tmp/t_pc

#include "PredatorContext.h"

#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}  // namespace

int main() {
    std::printf("PredatorContext tests\n");

    // --- Default-constructed PredatorContext is safe (zero-initialized, not garbage) ---
    {
        PredatorContext ctx{};
        check("default gang.isValid is false", ctx.gang.isValid == false);
        check("default regime is COILED_SPRING (enum 0)", ctx.regime == HMMStateEnum::COILED_SPRING);
        check("default inPosition is false", ctx.inPosition == false);
        check("default applicabilityMask is zero", ctx.applicabilityMask == 0ULL);
    }

    // --- Struct is trivially copyable (DOD requirement: no heap, no vtable) ---
    {
        PredatorContext a{};
        a.gang.hurstExponent = 0.72f;
        a.regime = HMMStateEnum::PARETO_MOMENTUM;
        a.inPosition = true;
        PredatorContext b = a;  // plain copy, must compile and preserve values
        check("copy preserves hurstExponent", b.gang.hurstExponent == 0.72f);
        check("copy preserves regime", b.regime == HMMStateEnum::PARETO_MOMENTUM);
        check("copy preserves inPosition", b.inPosition == true);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails (header doesn't exist yet)**

Run: `g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_predator_context.cpp -o /tmp/t_pc`
Expected: FAIL with `fatal error: 'PredatorContext.h' file not found`

- [ ] **Step 3: Create `include/PredatorContext.h`**

```cpp
// PredatorContext.h — unified macro context for the Predator Decision Contract
// (docs/superpowers/specs/2026-08-16-predator-decision-contract-execution-risk-framework.md).
//
// Composes the existing LocalRiskContext (Gang/Taleb/Pareto/Shannon intelligence, already
// populated by ContextManager every tick) with the HMM regime-state enum (previously only
// reachable via InferenceManager::Instance().HmmState()) into a single flat, POD struct.
// Composition-by-value costs nothing for cache locality — a nested POD struct lays out
// contiguously in memory identically to a flattened one, so this gives call sites one
// object instead of two separate singleton lookups, with zero DOD penalty.
//
// Every fusion free function in PredatorFusion.h takes this by const-reference — no
// allocation, no indirection, no virtual dispatch.

#pragma once

#include "ContextManager.h"  // LocalRiskContext
#include "Indicator.h"       // HMMStateEnum

struct PredatorContext {
    LocalRiskContext gang{};
    HMMStateEnum regime = HMMStateEnum::COILED_SPRING;
    bool inPosition = false;
    uint64_t applicabilityMask = 0ULL;
};
```

- [ ] **Step 4: Run test to verify it passes (compiles; ContextManager.h include resolves)**

Run: `g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_predator_context.cpp -o /tmp/t_pc && /tmp/t_pc`
Expected: `ALL PASS`

- [ ] **Step 5: Add the `GetPredatorContext()` accessor to `ContextManager`**

In `include/ContextManager.h`, near the existing `GetLocalRiskContext()` accessor (line 381), add:

```cpp
const PredatorContext& GetPredatorContext() const { return m_predatorContext; }
```

Add the member alongside the existing `m_localRiskContext` member (near the comment
"Elite v3.2: Unified local risk context (public via GetLocalRiskContext())"):

```cpp
mutable PredatorContext m_predatorContext;  // Assembled lazily from m_localRiskContext + HMM state
```

Add `#include "PredatorContext.h"` near the top of `ContextManager.h` (alongside the other includes).

- [ ] **Step 6: Implement assembly in `src/ContextManager.cpp`**

Find `ContextManager`'s per-tick update entry point (the function that already refreshes
`m_localRiskContext` — grep `m_localRiskContext =` or `m_localRiskContext\.` in
`src/ContextManager.cpp` to find the exact call site) and add, immediately after
`m_localRiskContext` is refreshed for the tick:

```cpp
// Assemble PredatorContext from the two already-computed sources — no new computation,
// just a cheap struct copy of data that already exists this tick.
m_predatorContext.gang = m_localRiskContext;
if (const auto* hmm = InferenceManager::Instance().HmmState()) {
    m_predatorContext.regime = hmm->Value();
} else {
    m_predatorContext.regime = HMMStateEnum::COILED_SPRING;  // safe default, matches struct's own default
}
// m_predatorContext.inPosition and .applicabilityMask are set by Task 2's dispatch helper,
// not here — ContextManager doesn't know position state; PositionManager does.
```

Add `#include "InferenceManager.h"` to `src/ContextManager.cpp` if not already present (check first
with `grep -n "InferenceManager.h" src/ContextManager.cpp`).

- [ ] **Step 7: `./build_dll.sh --no-clean` and confirm clean**

Run: `./build_dll.sh --no-clean`
Expected: `✓ Build completed` — this proves `ContextManager.cpp`/`.h` compile with the new member and
accessor in the real cross-compilation toolchain, not just the standalone g++ test harness.

- [ ] **Step 8: Commit**

```bash
git add include/PredatorContext.h include/ContextManager.h src/ContextManager.cpp tests/cpp/test_predator_context.cpp
git commit -m "feat: add PredatorContext — unified macro-context struct for the Predator Decision Contract

Composes the existing LocalRiskContext with HMM regime state into one
flat, DOD-consistent struct, assembled once per tick from data that's
already computed. Zero new computation, zero behavior change.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

---

### Task 2: `FusionKey` and the Applicability-Mask Dispatch Helper

**Files:**
- Create: `include/FusionKey.h`
- Create: `include/PredatorFusion.h`
- Test: `tests/cpp/test_predator_fusion_dispatch.cpp`

**Interfaces:**
- Consumes: `PredatorContext` (Task 1).
- Produces: `enum class FusionKey : uint8_t { ... }`, `constexpr uint64_t FusionKeyMask(FusionKey)`,
  `uint64_t ComputeApplicabilityMask(bool inPosition, HMMStateEnum regime)` — Task 3's `FuseTauStar()`
  and Task 4/6's Turtle Soup functions are dispatched through this.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/cpp/test_predator_fusion_dispatch.cpp — applicability-mask dispatch tests.
//
// Build: g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_predator_fusion_dispatch.cpp -o /tmp/t_pfd && /tmp/t_pfd

#include "PredatorFusion.h"

#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}  // namespace

int main() {
    std::printf("PredatorFusion dispatch tests\n");

    // --- Contract element #5, structural: entry-fusion bits can never be set while in a position ---
    {
        const uint64_t mask = ComputeApplicabilityMask(/*inPosition=*/true, HMMStateEnum::GAUSSIAN_STABLE);
        check("TAU_STAR (exit-side) bit set while in position",
              (mask & FusionKeyMask(FusionKey::TAU_STAR)) != 0ULL);
        check("TURTLE_SOUP_OPTION_A (entry-side) bit CLEAR while in position — structural safety guarantee",
              (mask & FusionKeyMask(FusionKey::TURTLE_SOUP_OPTION_A)) == 0ULL);
    }

    // --- Flat: entry-fusion bits set, exit-fusion bits clear ---
    {
        const uint64_t mask = ComputeApplicabilityMask(/*inPosition=*/false, HMMStateEnum::PARETO_MOMENTUM);
        check("TURTLE_SOUP_OPTION_A bit set while flat",
              (mask & FusionKeyMask(FusionKey::TURTLE_SOUP_OPTION_A)) != 0ULL);
        check("TAU_STAR bit CLEAR while flat (no position to exit)",
              (mask & FusionKeyMask(FusionKey::TAU_STAR)) == 0ULL);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_predator_fusion_dispatch.cpp -o /tmp/t_pfd`
Expected: FAIL with `fatal error: 'PredatorFusion.h' file not found`

- [ ] **Step 3: Create `include/FusionKey.h`**

```cpp
// FusionKey.h — bit-index enum for the Predator applicability-mask dispatch
// (mirrors IndicatorKey.h's convention: one enum value per bit position).
//
// This is a SEPARATE bit-space from IndicatorKey/IndicatorManager's own dirty-mask —
// analogous pattern, different concept (which fusion FUNCTIONS are applicable this tick,
// not which INDICATORS are dirty). Do not conflate the two enums or their masks.

#pragma once

#include <cstdint>

enum class FusionKey : uint8_t {
    TAU_STAR = 0,                  // exit-side: TRAP anticipatory threshold (Task 3)
    TURTLE_SOUP_OPTION_A = 1,      // entry-side: Turtle Soup geometric heuristic (Task 4)
    TURTLE_SOUP_OPTION_B = 2,      // entry-side: Turtle Soup classifier (Task 6, not yet wired live)
    MAX_FUSION_KEYS = 3
};
```

- [ ] **Step 4: Create `include/PredatorFusion.h`**

```cpp
// PredatorFusion.h — free-function fusion interface + applicability-mask dispatch.
//
// Reuses IndicatorManager's already-proven dirty-mask/trigger-mask idiom (m_dirty_mask,
// PRIMARY_TRIGGER_MASK, __builtin_ctzll-based bit iteration) rather than inventing a new
// mechanism. Each fusion function is a plain free function — no virtual dispatch — assigned
// a bit in FusionKey. The applicability mask is computed once per tick from the cheapest
// available preconditions (position state, regime), and is ALSO the structural enforcement
// of Predator Decision Contract element #5: an entry-fusion bit can never be set while
// inPosition is true, so it is impossible to call an entry-side fusion function on the
// wrong side of position state, not merely disciplined against.

#pragma once

#include "FusionKey.h"
#include "Indicator.h"  // HMMStateEnum

#include <cstdint>

constexpr uint64_t FusionKeyMask(FusionKey key) {
    return 1ULL << static_cast<uint64_t>(key);
}

constexpr uint64_t ENTRY_FUSION_MASK =
    FusionKeyMask(FusionKey::TURTLE_SOUP_OPTION_A) |
    FusionKeyMask(FusionKey::TURTLE_SOUP_OPTION_B);

constexpr uint64_t EXIT_FUSION_MASK =
    FusionKeyMask(FusionKey::TAU_STAR);

// Computes which fusion functions are applicable this tick. Position state is the
// first-level, structural split (contract element #5); regime-applicability is a
// second-level filter individual fusion functions can further narrow against, but
// this helper only enforces the structural (position-state) split — a fusion function
// still checks its own regime-conditioning internally (e.g., FuseTauStar checks regime
// itself; this mask only guarantees it's never called on the wrong side of position state).
inline uint64_t ComputeApplicabilityMask(bool inPosition, HMMStateEnum /*regime*/) {
    return inPosition ? EXIT_FUSION_MASK : ENTRY_FUSION_MASK;
}
```

- [ ] **Step 5: Run test to verify it passes**

Run: `g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_predator_fusion_dispatch.cpp -o /tmp/t_pfd && /tmp/t_pfd`
Expected: `ALL PASS`

- [ ] **Step 6: `./build_dll.sh --no-clean`**

Expected: `✓ Build completed` (these are new, additive headers — nothing includes them yet, so this
just confirms no syntax/include-path regressions).

- [ ] **Step 7: Commit**

```bash
git add include/FusionKey.h include/PredatorFusion.h tests/cpp/test_predator_fusion_dispatch.cpp
git commit -m "feat: add FusionKey + applicability-mask dispatch for PredatorFusion

Reuses IndicatorManager's proven dirty-mask idiom. The mask computation
is both a real perf optimization (skip inapplicable fusion calls every
tick under AutoLoop=1) and the structural enforcement of Predator
Decision Contract element 5 -- an entry-fusion bit provably cannot be
set while in a position.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

---

### Task 3: `FuseTauStar()` — Reference Fusion Function (Elkan 2001, Computed but Not Wired Live)

**Files:**
- Modify: `include/PredatorFusion.h` (add the function + result struct)
- Test: `tests/cpp/test_predator_fusion_dispatch.cpp` (extend)

**Interfaces:**
- Consumes: `PredatorContext` (Task 1).
- Produces: `struct TauStarFusionResult { bool shouldExit; float effectiveThreshold; };` and
  `TauStarFusionResult FuseTauStar(const PredatorContext& ctx, double distanceToTarget, double distanceToStop, double modelConfidence);`

**Important scope note**: there is no pre-existing bespoke τ* implementation to migrate — verified
directly (`grep -rn "TauStar\|tau_star" src include` returns nothing beyond `DofStopScale()`, which
this function consumes as an input, not a prior implementation of τ* itself). `TRAP_LONG`/`TRAP_SHORT`
model actions are currently handled as pure telemetry at `PositionManager.cpp:1682-1688` ("position
maintained", no order action) — **this task computes the τ* value for the first time, as a real,
tested function, but does NOT change that existing telemetry-only handling.** Wiring it to take live
exit action remains gated on backlog Unit 3's `ExitReason_TRAP` schema work (currently deferred), per
the Predator Decision Contract's own Maturity Inventory. Do not wire it live as part of this task.

- [ ] **Step 1: Write the failing test (append to `test_predator_fusion_dispatch.cpp`)**

```cpp
    // --- FuseTauStar: Elkan (2001) cost-sensitive threshold, tau* = C_FP / (C_FP + C_FN) ---
    // C_FP = distance to target (cost of exiting too early, forfeiting the remaining move)
    // C_FN = distance to stop (cost of not exiting, i.e. the loss if wrong)
    {
        PredatorContext ctx{};
        ctx.inPosition = true;
        ctx.regime = HMMStateEnum::GAUSSIAN_STABLE;

        // Symmetric case: target and stop equidistant -> tau* = 0.5
        TauStarFusionResult r1 = FuseTauStar(ctx, /*distanceToTarget=*/2.0, /*distanceToStop=*/2.0, /*modelConfidence=*/0.6);
        check("symmetric distances -> tau* == 0.5", std::fabs(r1.effectiveThreshold - 0.5f) < 1e-4f);
        check("confidence 0.6 < tau* 0.5 is FALSE so shouldExit is true (0.6 >= 0.5)", r1.shouldExit == true);

        // Asymmetric: stop much closer than target -> tau* low -> easier to trigger exit
        TauStarFusionResult r2 = FuseTauStar(ctx, /*distanceToTarget=*/8.0, /*distanceToStop=*/1.0, /*modelConfidence=*/0.2);
        check("stop much closer than target -> tau* < 0.5", r2.effectiveThreshold < 0.5f);

        // Confidence below threshold -> should not exit
        TauStarFusionResult r3 = FuseTauStar(ctx, /*distanceToTarget=*/2.0, /*distanceToStop=*/2.0, /*modelConfidence=*/0.1);
        check("low confidence below tau* -> shouldExit false", r3.shouldExit == false);
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_predator_fusion_dispatch.cpp -o /tmp/t_pfd`
Expected: FAIL with `error: 'FuseTauStar' was not declared`

- [ ] **Step 3: Implement in `include/PredatorFusion.h`**

Append:

```cpp
struct TauStarFusionResult {
    bool shouldExit;
    float effectiveThreshold;  // the computed tau* value, exported for telemetry/analysis
};

// Elkan (2001) cost-sensitive threshold: tau* = C_FP / (C_FP + C_FN).
// C_FP (cost of a false positive, i.e. exiting when the trade would have worked) is modeled
// as the remaining distance to target; C_FN (cost of a false negative, i.e. not exiting when
// the trade is actually failing) is modeled as the remaining distance to stop -- matching
// CLAUDE.md's Trap Detection section exactly ("C_FP=|target-price|, C_FN=|price-stop|").
inline TauStarFusionResult FuseTauStar(
    const PredatorContext& ctx,
    double distanceToTarget,
    double distanceToStop,
    double modelConfidence
) {
    TauStarFusionResult result{};
    const double denom = distanceToTarget + distanceToStop;
    const float tauStar = (denom > 1e-9)
        ? static_cast<float>(distanceToTarget / denom)
        : 0.5f;  // degenerate case (both distances ~0): neutral threshold
    result.effectiveThreshold = tauStar;
    result.shouldExit = (ctx.inPosition) && (static_cast<float>(modelConfidence) >= tauStar);
    return result;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_predator_fusion_dispatch.cpp -o /tmp/t_pfd && /tmp/t_pfd`
Expected: `ALL PASS`

- [ ] **Step 5: `./build_dll.sh --no-clean`; confirm no call site wires this to a live action**

Run: `./build_dll.sh --no-clean` — expect green.
Run: `grep -rn "FuseTauStar" src/` — expect **zero results** (header-only, not yet called from any
`.cpp` file). This confirms the "computed but not wired live" scope boundary held.

- [ ] **Step 6: Commit**

```bash
git add include/PredatorFusion.h tests/cpp/test_predator_fusion_dispatch.cpp
git commit -m "feat: implement FuseTauStar as the Predator contract's reference fusion function

Elkan (2001) cost-sensitive threshold, tau* = C_FP/(C_FP+C_FN), computed
and tested for the first time as a real function -- no pre-existing
bespoke implementation existed to migrate (verified: TRAP_LONG/SHORT
model actions are still pure telemetry at PositionManager.cpp:1682-1688,
unchanged by this commit). Not wired to any live call site -- remains
gated on backlog Unit 3's ExitReason_TRAP schema work.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

---

### Task 4: Turtle Soup Option A — Tick-Reactive Geometric Heuristic

**Files:**
- Create: `include/TurtleSoupFusion.h`
- Modify: `src/TripleScreen3.cpp:1150-1290` (remove the once-per-closed-bar gate; wire tick-reactive
  call)
- Test: `tests/cpp/test_turtle_soup_fusion.cpp`

**Interfaces:**
- Consumes: `PredatorContext` (Task 1), `DetectTurtleSoup()` (existing, `IndicatorComputations.h:768`,
  unchanged).
- Produces: `struct TurtleSoupMicroSignal { float score; float confidence; float earliness; bool isValid; };`
  and `TurtleSoupMicroSignal EvaluateTurtleSoupOptionA(...)` — Task 6's Option B produces the same
  struct shape, at the same call site, later.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/cpp/test_turtle_soup_fusion.cpp — Option A geometric heuristic + fusion tests.
//
// Build: g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_turtle_soup_fusion.cpp -o /tmp/t_ts && /tmp/t_ts

#include "TurtleSoupFusion.h"

#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}  // namespace

int main() {
    std::printf("Turtle Soup Option A fusion tests\n");

    // --- Bullish setup: price penetrated below the 20-bar low, then recovered above it ---
    {
        PredatorContext ctx{};
        ctx.inPosition = false;
        ctx.regime = HMMStateEnum::GAUSSIAN_STABLE;

        TurtleSoupMicroSignal sig = EvaluateTurtleSoupOptionA(
            /*low=*/98.0f, /*high=*/101.0f, /*closeSoFar=*/100.5f,
            /*twentyBarLow=*/99.0f, /*twentyBarHigh=*/105.0f,
            /*atr=*/1.0f, /*elapsedFraction=*/0.6f
        );
        check("bullish penetrate-then-recover -> positive score", sig.score > 0.0f);
        check("isValid true given a real bar range", sig.isValid == true);
        check("earliness matches elapsedFraction", std::fabs(sig.earliness - 0.6f) < 1e-4f);
    }

    // --- Bearish setup: price penetrated above the 20-bar high, then rejected back below it ---
    {
        TurtleSoupMicroSignal sig = EvaluateTurtleSoupOptionA(
            /*low=*/99.0f, /*high=*/106.5f, /*closeSoFar=*/104.5f,
            /*twentyBarLow=*/95.0f, /*twentyBarHigh=*/106.0f,
            /*atr=*/1.0f, /*elapsedFraction=*/0.4f
        );
        check("bearish penetrate-then-reject -> negative score", sig.score < 0.0f);
    }

    // --- Direction-discrimination proof (the shape that would have caught Elder Breakout's bug):
    //     a bullish setup and a bearish setup with mirrored geometry must produce OPPOSITE-signed
    //     scores, not the same result regardless of direction. ---
    {
        TurtleSoupMicroSignal bull = EvaluateTurtleSoupOptionA(
            98.0f, 101.0f, 100.5f, 99.0f, 105.0f, 1.0f, 0.5f);
        TurtleSoupMicroSignal bear = EvaluateTurtleSoupOptionA(
            99.0f, 106.5f, 104.5f, 95.0f, 106.0f, 1.0f, 0.5f);
        check("bullish and bearish setups produce opposite-signed scores, not identical",
              (bull.score > 0.0f) != (bear.score > 0.0f));
    }

    // --- No penetration at all -> invalid/neutral signal ---
    {
        TurtleSoupMicroSignal sig = EvaluateTurtleSoupOptionA(
            /*low=*/100.0f, /*high=*/102.0f, /*closeSoFar=*/101.0f,
            /*twentyBarLow=*/95.0f, /*twentyBarHigh=*/106.0f,
            /*atr=*/1.0f, /*elapsedFraction=*/0.5f
        );
        check("no extreme penetration -> isValid false (no setup forming)", sig.isValid == false);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_turtle_soup_fusion.cpp -o /tmp/t_ts`
Expected: FAIL with `fatal error: 'TurtleSoupFusion.h' file not found`

- [ ] **Step 3: Create `include/TurtleSoupFusion.h`**

```cpp
// TurtleSoupFusion.h — Turtle Soup Predator-ization, Option A (tick-reactive geometric
// heuristic), per docs/superpowers/specs/2026-08-16-turtle-soup-predator-ization-spec.md.
//
// Reuses the same tail-to-body-ratio/close-position-in-range shape already proven and
// audited for Kangaroo Tail (TripleScreen3.cpp:849-877), applied against the 20-bar
// reference extreme instead of a single bar's own range, evaluated on the CURRENT,
// still-forming bar (not sc.Index-1) -- this is what makes it tick-reactive instead of
// the historian-gated once-per-closed-bar check it replaces.

#pragma once

#include "PredatorContext.h"

#include <algorithm>
#include <cmath>

struct TurtleSoupMicroSignal {
    float score = 0.0f;       // [-1.0, +1.0]; positive = bullish setup forming, negative = bearish
    float confidence = 0.0f;  // [0.0, 1.0]
    float earliness = 0.0f;   // elapsed-bar-fraction at evaluation time, [0.0, 1.0]
    bool  isValid = false;    // false until a real penetration-then-reject shape is forming
};

// low/high/closeSoFar: the current, still-forming bar's range and last price.
// twentyBarLow/twentyBarHigh: the prior 20-bar reference extremes (unchanged from the
// existing DetectTurtleSoup() call site's prevHighest/prevLowest computation).
inline TurtleSoupMicroSignal EvaluateTurtleSoupOptionA(
    float low, float high, float closeSoFar,
    float twentyBarLow, float twentyBarHigh,
    float atr, float elapsedFraction
) {
    TurtleSoupMicroSignal sig{};
    sig.earliness = elapsedFraction;

    if (high <= low || atr <= 0.0f) {
        return sig;  // isValid stays false
    }
    const float barRangeSoFar = high - low;
    const float closePosition = (barRangeSoFar > 0.001f) ? ((closeSoFar - low) / barRangeSoFar) : 0.5f;

    // Bullish: low penetrated below the 20-bar low, close-so-far has recovered back above it,
    // and the close sits in the upper part of the bar's range-so-far (rejection shape).
    const float bullishPenetration = twentyBarLow - low;  // positive if low broke below
    if (bullishPenetration > 0.0f && closeSoFar > twentyBarLow && closePosition >= 0.55f) {
        const float penetrationToAtr = std::min(1.0f, bullishPenetration / atr);
        sig.score = std::min(1.0f, 0.3f + 0.7f * penetrationToAtr);
        sig.confidence = std::min(1.0f, closePosition);
        sig.isValid = true;
        return sig;
    }

    // Bearish: high penetrated above the 20-bar high, close-so-far has rejected back below it,
    // and the close sits in the lower part of the bar's range-so-far.
    const float bearishPenetration = high - twentyBarHigh;  // positive if high broke above
    if (bearishPenetration > 0.0f && closeSoFar < twentyBarHigh && closePosition <= 0.45f) {
        const float penetrationToAtr = std::min(1.0f, bearishPenetration / atr);
        sig.score = -std::min(1.0f, 0.3f + 0.7f * penetrationToAtr);
        sig.confidence = std::min(1.0f, 1.0f - closePosition);
        sig.isValid = true;
        return sig;
    }

    return sig;  // isValid stays false — no penetrate-then-reject shape forming yet
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_turtle_soup_fusion.cpp -o /tmp/t_ts && /tmp/t_ts`
Expected: `ALL PASS`

- [ ] **Step 5: Commit the pure-function layer before touching the live call site**

```bash
git add include/TurtleSoupFusion.h tests/cpp/test_turtle_soup_fusion.cpp
git commit -m "feat: add EvaluateTurtleSoupOptionA — tick-reactive geometric heuristic

Reuses Kangaroo Tail's already-audited tail-shape/close-position
approach against the 20-bar reference extreme, evaluated on the
current forming bar. Pure function, not yet wired to the live call
site (next commit).

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

- [ ] **Step 6: Remove the once-per-closed-bar gate in `src/TripleScreen3.cpp`**

Locate the block at `TripleScreen3.cpp:1154-1176` (the "Institutional timing contract" comment,
`lastProcessedBarTS`/`signalBarIndex`/`runTurtleSoup` gating). Per the standing dead-code-removal
mandate, remove this gating outright — do not leave `lastProcessedBarTS` as an unused persistent
variable. Replace the `if (runTurtleSoup) { ... }` block's condition so the body runs every tick
(matching Kangaroo Tail/Momentum Pinball's own unconditional-per-tick pattern), and change the raw
`DetectTurtleSoup()` call to read the *current* bar (`sc.Index`) instead of the completed signal bar
(`sc.Index - 1`) for its own high/low/close inputs — keep the 20-bar reference-window computation
(`prevHighest`/`prevLowest`, still sourced from history, unchanged) exactly as-is, since only the
*signal bar itself* needs to become tick-reactive, not the macro reference window.

Concretely: replace

```cpp
    constexpr int TURTLE_SOUP_LENGTH = 20;
    constexpr int TURTLE_SOUP_MIN_SEPARATION = 4;

    // Use persistent variable to track last processed bar index
    // Reuse specific ID or just use a unique integer for this study instance
    int& lastProcessedBarTS = sc.GetPersistentInt(101); // 101 for Turtle Soup Logic

    const int signalBarIndex = sc.Index - 1;

    // Only process if we have a closed bar and haven't processed it yet
    // Note: In AutoLoop during history, sc.Index iterates.
    // For live updates, we want to run this check on every tick but only EXECUTE logic once per closed bar.

    bool runTurtleSoup = false;
    if (signalBarIndex >= TURTLE_SOUP_LENGTH &&
        sc.GetBarHasClosedStatus(signalBarIndex) == BHCS_BAR_HAS_CLOSED &&
        lastProcessedBarTS != signalBarIndex) {
        runTurtleSoup = true;
    }

    if (runTurtleSoup) {
        lastProcessedBarTS = signalBarIndex;

        // Retrieve context variables
        float atr = Subgraph_AtrTemp3[signalBarIndex];
```

with

```cpp
    constexpr int TURTLE_SOUP_LENGTH = 20;
    constexpr int TURTLE_SOUP_MIN_SEPARATION = 4;

    // Predator-ized (2026-08-16): evaluates the CURRENT, still-forming bar every tick,
    // matching Kangaroo Tail/Momentum Pinball's tick-reactive pattern. The 20-bar reference
    // window below still uses only fully-closed history (unchanged) -- only the signal
    // bar's own high/low/close became tick-reactive, per
    // docs/superpowers/specs/2026-08-16-turtle-soup-predator-ization-spec.md.
    const int signalBarIndex = sc.Index;

    const bool runTurtleSoup = (signalBarIndex - 1 >= TURTLE_SOUP_LENGTH);

    if (runTurtleSoup) {
        // Retrieve context variables
        float atr = Subgraph_AtrTemp3[signalBarIndex];
```

Then update every remaining reference in this block from using `signalBarIndex` as "the completed
prior bar" to reflect that it is now the *current* bar: the reference-window computation must exclude
the current (still-forming) bar exactly as it already excludes the signal bar today — grep this block
for `prevWindowLastIndex` (currently `signalBarIndex - 1`); change it to `signalBarIndex - 1` still
(this is unchanged — it already meant "one bar before the signal bar," and now the signal bar is
`sc.Index` instead of `sc.Index - 1`, so `prevWindowLastIndex` naturally becomes `sc.Index - 1`,
i.e. the last fully-closed bar, which is correct and requires no further edit beyond the
`signalBarIndex` reassignment above).

- [ ] **Step 7: Wire the fusion call at the same call site**

Immediately after the existing `DetectTurtleSoup(...)` call (which remains, unchanged, as the
existing quality/strength classification used for `soupIndicator->SetMetrics(...)` and the
`RaschkeTacticalTrigger` update further below), add the new Option A fusion call, sourcing the
`PredatorContext` from `ContextManager::Instance().GetPredatorContext()`:

```cpp
        // === Predator-ization: Option A tick-reactive fusion (2026-08-16) ===
        const PredatorContext& predatorCtx = ContextManager::Instance().GetPredatorContext();
        TurtleSoupMicroSignal turtleSoupSignal = EvaluateTurtleSoupOptionA(
            sc.Low[signalBarIndex], sc.High[signalBarIndex], sc.Close[signalBarIndex],
            prevLowest, prevHighest, atr,
            // Option A's score/confidence do not depend on elapsed-bar-fraction — it
            // evaluates the whole bar-so-far range on every call, not a sequential
            // prefix computation. The field exists on TurtleSoupMicroSignal for shape
            // parity with Option B (which does use it); Option A reports 1.0
            // (whole-range-so-far already considered), not a placeholder.
            /*elapsedFraction=*/1.0f
        );
        // turtleSoupSignal is exported via soupIndicator's existing metrics/context path below —
        // no separate export needed; this call proves the fusion function runs correctly every
        // tick, feeding the same DetectTurtleSoup()-derived quality classification.
```

Add `#include "TurtleSoupFusion.h"` and `#include "PredatorContext.h"` to the top of
`src/TripleScreen3.cpp` if not already present.

- [ ] **Step 8: Remove the now-redundant `screenAligned` HMM-alignment block for Turtle Soup**

This block (`TripleScreen3.cpp:1272-1283` in the pre-edit file, "Context: HMM State Alignment") is
**not** the Elder-Breakout-style bug (it genuinely discriminates direction — bullish checks
`GAUSSIAN_STABLE`/`PARETO_MOMENTUM`, bearish checks `GAUSSIAN_FRAGILE`/`COILED_SPRING`) and per the
Predator audit is **not being replaced** by this task — leave it exactly as-is. Do not remove or
modify it; only the gating (`lastProcessedBarTS`) and the `DetectTurtleSoup()` input source
(`sc.Index` vs `sc.Index - 1`) change in this task.

- [ ] **Step 9: `./build_dll.sh` (full, not `--no-clean`) and confirm green**

Run: `./build_dll.sh`
Expected: `✓ Build completed` with zero warnings-as-errors from the removed `lastProcessedBarTS`
persistent variable (confirm no "unused variable" error survives — `sc.GetPersistentInt()` calls have
side effects via Sierra Chart's persistent-storage API, so removing the call entirely, not just the
variable, is what matters. Re-check the diff shows the whole `sc.GetPersistentInt(101)` line is gone).

- [ ] **Step 10: Run the native test suite one more time to confirm no regression**

Run: `g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_turtle_soup_fusion.cpp -o /tmp/t_ts && /tmp/t_ts`
Run: `g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_predator_fusion_dispatch.cpp -o /tmp/t_pfd && /tmp/t_pfd`
Expected: `ALL PASS` for both.

- [ ] **Step 11: Commit**

```bash
git add src/TripleScreen3.cpp
git commit -m "feat: wire Turtle Soup to tick-reactive evaluation (Predator-ization Option A)

Removes the once-per-closed-bar gate (lastProcessedBarTS) entirely --
Turtle Soup now evaluates the current, still-forming bar every tick,
matching Kangaroo Tail/Momentum Pinball's already-audited pattern.
The 20-bar reference window still uses only fully-closed history,
unchanged. DetectTurtleSoup()'s own quality classification and the
existing, correctly direction-discriminating HMM-alignment context
are both left exactly as-is -- only the gating and the signal bar's
own data source changed. build_dll.sh clean.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

---

### Task 5: `ClassifierParams` — Config-Driven Parameter Loading Scaffold

**Files:**
- Create: `config/classifier_params.json`
- Create: `include/ClassifierParams.h`
- Test: `tests/cpp/test_classifier_params.cpp`

**Interfaces:**
- Produces: `struct ClassifierParams { std::vector<float> weights; float bias; bool isLoaded; };` and
  `static ClassifierParams ClassifierParams::LoadConfig(const std::string& consumerKey, const std::string& path = kConfigPathWindows);`
  — Task 6 consumes this for Turtle Soup's `"turtle_soup_option_b"` section.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/cpp/test_classifier_params.cpp — config-driven classifier parameter loading.
//
// Build: g++ -std=c++17 -Wall -Wextra -I include -I /mnt/c/Users/rcruz/vcpkg/installed/x64-windows/include tests/cpp/test_classifier_params.cpp -o /tmp/t_cp && /tmp/t_cp

#include "ClassifierParams.h"

#include <cstdio>
#include <fstream>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}  // namespace

int main() {
    std::printf("ClassifierParams tests\n");

    // --- Safe default when config file is missing: all-zero weights, isLoaded false ---
    {
        ClassifierParams params = ClassifierParams::LoadConfig(
            "turtle_soup_option_b", "/tmp/does_not_exist_classifier_params.json");
        check("missing file -> isLoaded false", params.isLoaded == false);
        check("missing file -> empty weights (neutral/inert)", params.weights.empty());
    }

    // --- Loads real weights from a well-formed test config file ---
    {
        const std::string testPath = "/tmp/test_classifier_params.json";
        std::ofstream f(testPath);
        f << R"({
  "turtle_soup_option_b": {
    "model_type": "logistic_regression",
    "weights": [0.1, -0.2, 0.3],
    "bias": 0.05,
    "feature_names": ["penetration_atr", "close_position", "elapsed_fraction"]
  }
})";
        f.close();

        ClassifierParams params = ClassifierParams::LoadConfig("turtle_soup_option_b", testPath);
        check("real config -> isLoaded true", params.isLoaded == true);
        check("real config -> 3 weights loaded", params.weights.size() == 3);
        check("real config -> weights[0] correct", std::fabs(params.weights[0] - 0.1f) < 1e-5f);
        check("real config -> weights[1] correct (negative)", std::fabs(params.weights[1] - (-0.2f)) < 1e-5f);
        check("real config -> bias correct", std::fabs(params.bias - 0.05f) < 1e-5f);
    }

    // --- Unknown consumer key in a well-formed file -> safe default, not a crash ---
    {
        ClassifierParams params = ClassifierParams::LoadConfig(
            "nonexistent_consumer", "/tmp/test_classifier_params.json");
        check("unknown consumer key -> isLoaded false, no crash", params.isLoaded == false);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURES\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_classifier_params.cpp -o /tmp/t_cp`
Expected: FAIL with `fatal error: 'ClassifierParams.h' file not found`

- [ ] **Step 3: Create `config/classifier_params.json`** (the git-tracked source of truth, sectioned by
  consumer, matching `execution_params.json`/`hmm_regime_risk_policy.json`'s existing provenance
  convention)

```json
{
  "_version": "1.0.0",
  "_owner": "MindfulTrader C++ execution layer",
  "_generated_by": "manual placeholder pending first real training run (see docs/superpowers/specs/2026-08-16-ects-prefix-training-infrastructure-spec.md handoff)",
  "turtle_soup_option_b": {
    "model_type": "logistic_regression",
    "weights": [],
    "bias": 0.0,
    "feature_names": ["penetration_atr", "close_position", "elapsed_fraction"],
    "_note": "Empty weights = inert/neutral until a real trained model is dropped in by a future lbrnet-rooted session, per the ECTS infrastructure spec's handoff."
  }
}
```

- [ ] **Step 4: Create `include/ClassifierParams.h`**

```cpp
// ClassifierParams.h — config-driven classifier parameter loading, following
// FeatureScaler::LoadConfig()'s exact established idiom: compiled-in safe default
// (empty/inert), overwritten in place if config/classifier_params.json is present and
// contains the requested consumer key. Sectioned by consumer name (mirrors
// execution_params.json/hmm_regime_risk_policy.json's provenance convention) so a second
// consumer (e.g. TRAP's own eventual ECTS application) is a new section, not a new file.

#pragma once

#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <vector>

struct ClassifierParams {
    std::vector<float> weights;
    float bias = 0.0f;
    bool isLoaded = false;

    static constexpr const char* kConfigPathWindows = "/mnt/c/Trading/config/classifier_params.json";

    static ClassifierParams LoadConfig(const std::string& consumerKey,
                                        const std::string& path = kConfigPathWindows) {
        ClassifierParams params{};  // safe default: empty weights, isLoaded false

        std::ifstream f(path);
        if (!f.is_open()) {
            return params;  // missing file -> inert default, never a crash
        }

        try {
            nlohmann::json j;
            f >> j;
            if (!j.contains(consumerKey)) {
                return params;  // unknown consumer key -> inert default
            }
            const auto& section = j.at(consumerKey);
            if (!section.contains("weights") || !section.contains("bias")) {
                return params;
            }
            const auto weightsJson = section.at("weights").get<std::vector<float>>();
            if (weightsJson.empty()) {
                return params;  // placeholder/empty config -> stay inert, don't half-load
            }
            params.weights = weightsJson;
            params.bias = section.at("bias").get<float>();
            params.isLoaded = true;
        } catch (const std::exception&) {
            return ClassifierParams{};  // malformed JSON -> safe default, not a partial/garbage state
        }
        return params;
    }
};
```

- [ ] **Step 5: Run test to verify it passes**

Run: `g++ -std=c++17 -Wall -Wextra -I include -I /mnt/c/Users/rcruz/vcpkg/installed/x64-windows/include tests/cpp/test_classifier_params.cpp -o /tmp/t_cp && /tmp/t_cp`
Expected: `ALL PASS`

- [ ] **Step 6: `./build_dll.sh --no-clean`**

Expected: `✓ Build completed`.

- [ ] **Step 7: Commit**

```bash
git add config/classifier_params.json include/ClassifierParams.h tests/cpp/test_classifier_params.cpp
git commit -m "feat: add ClassifierParams config-driven loading scaffold

Follows FeatureScaler::LoadConfig()'s exact idiom -- compiled-in safe
default (empty/inert), overwritten in place if config/classifier_params.json
is present. Sectioned by consumer name for future multi-consumer use
(Turtle Soup now, TRAP's own ECTS application eventually). Placeholder
weights (empty array) until a real trained model is dropped in by a
future lbrnet-rooted session.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

---

### Task 6: `EvaluateTurtleSoupOptionB()` — Hand-Crafted Inference (Built and Tested, Not Wired Live)

**Files:**
- Modify: `include/TurtleSoupFusion.h` (add the function)
- Test: `tests/cpp/test_turtle_soup_fusion.cpp` (extend)

**Interfaces:**
- Consumes: `ClassifierParams` (Task 5), same feature inputs as Option A.
- Produces: `TurtleSoupMicroSignal EvaluateTurtleSoupOptionB(const ClassifierParams& params, float penetrationAtr, float closePosition, float elapsedFraction);`
  — same output struct as Option A, per the bridge plan's standardized seam. **Not called from
  `TripleScreen3.cpp` in this task** — Option A remains the active call site until a real trained
  model wins its twin-based comparison (a separate, future decision).

- [ ] **Step 1: Write the failing test (append to `test_turtle_soup_fusion.cpp`)**

```cpp
    // --- Option B: hand-crafted logistic regression inference ---
    {
        // Synthetic test weights (not a real trained model -- proves the inference math only).
        ClassifierParams params{};
        params.weights = {2.0f, -1.0f, 0.5f};  // [penetration_atr, close_position, elapsed_fraction]
        params.bias = 0.0f;
        params.isLoaded = true;

        // Strong positive penetration, high close position -> should score clearly bullish
        TurtleSoupMicroSignal sig = EvaluateTurtleSoupOptionB(params, /*penetrationAtr=*/1.0f, /*closePosition=*/0.2f, /*elapsedFraction=*/0.5f);
        check("Option B: isValid true when params.isLoaded", sig.isValid == true);
        check("Option B: positive score for strong bullish features",
              sig.score > 0.0f);

        // Unloaded params (inert default) -> isValid false, never fires
        ClassifierParams unloaded{};
        TurtleSoupMicroSignal inert = EvaluateTurtleSoupOptionB(unloaded, 1.0f, 0.2f, 0.5f);
        check("Option B: unloaded params -> isValid false (inert until real model exists)",
              inert.isValid == false);
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -Wall -Wextra -I include -I /mnt/c/Users/rcruz/vcpkg/installed/x64-windows/include tests/cpp/test_turtle_soup_fusion.cpp -o /tmp/t_ts`
Expected: FAIL with `error: 'EvaluateTurtleSoupOptionB' was not declared`

- [ ] **Step 3: Implement in `include/TurtleSoupFusion.h`**

```cpp
#include "ClassifierParams.h"

// Option B: hand-crafted logistic regression inference (not m2cgen-generated, not a
// linked ML runtime library -- per docs/superpowers/specs/2026-08-16-ects-prefix-training-infrastructure-spec.md's
// deployment guidance). Weights/bias come from ClassifierParams, loaded from
// config/classifier_params.json's "turtle_soup_option_b" section. Feature order MUST match
// that section's "feature_names": [penetration_atr, close_position, elapsed_fraction].
//
// Golden-vector regression test requirement (mandatory before this is ever wired live):
// this function's output must be verified against the Python model's own predict_proba()
// for a comprehensive set of real inputs -- not just the synthetic weights used here to
// prove the inference math compiles and runs correctly.
inline TurtleSoupMicroSignal EvaluateTurtleSoupOptionB(
    const ClassifierParams& params,
    float penetrationAtr, float closePosition, float elapsedFraction
) {
    TurtleSoupMicroSignal sig{};
    sig.earliness = elapsedFraction;

    if (!params.isLoaded || params.weights.size() != 3) {
        return sig;  // inert until a real trained model is loaded — isValid stays false
    }

    const float logit = params.weights[0] * penetrationAtr
                       + params.weights[1] * closePosition
                       + params.weights[2] * elapsedFraction
                       + params.bias;
    const float probability = 1.0f / (1.0f + std::exp(-logit));

    // Map probability [0,1] to score [-1,+1] the same way Option A's score is signed:
    // > 0.5 is bullish-leaning, < 0.5 is bearish-leaning.
    sig.score = (probability - 0.5f) * 2.0f;
    sig.confidence = std::fabs(probability - 0.5f) * 2.0f;
    sig.isValid = true;
    return sig;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -Wall -Wextra -I include -I /mnt/c/Users/rcruz/vcpkg/installed/x64-windows/include tests/cpp/test_turtle_soup_fusion.cpp -o /tmp/t_ts && /tmp/t_ts`
Expected: `ALL PASS`

- [ ] **Step 5: `./build_dll.sh --no-clean`; confirm this is NOT wired to the live call site**

Run: `./build_dll.sh --no-clean` — expect green.
Run: `grep -rn "EvaluateTurtleSoupOptionB" src/` — expect **zero results**. This confirms Option B
exists, compiles, and is tested, but `TripleScreen3.cpp` still only calls Option A — the scope
boundary from this plan's header (no live-behavior change beyond Task 4) held for this task too.

- [ ] **Step 6: Commit**

```bash
git add include/TurtleSoupFusion.h tests/cpp/test_turtle_soup_fusion.cpp
git commit -m "feat: add EvaluateTurtleSoupOptionB — hand-crafted classifier inference scaffold

Hand-written logistic regression inference (dot product + sigmoid),
not m2cgen-generated code, not a linked ML runtime -- consumes
ClassifierParams (Task 5). Built and unit-tested against synthetic
weights only; NOT wired to TripleScreen3.cpp's live call site. Option A
remains the active implementation until a real trained model is
validated in the Python twin and proven to win its own empirical
comparison -- a separate, future decision, out of scope here.

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```

---

### Task 7: Final Verification

- [ ] **Step 1: Full clean build**

Run: `./build_dll.sh` (no `--no-clean` — a genuine from-scratch build)
Expected: `✓ Build completed`, zero warnings-as-errors.

- [ ] **Step 2: Run every native test written in this plan**

```bash
for t in test_predator_context test_predator_fusion_dispatch test_turtle_soup_fusion test_classifier_params; do
  echo "=== $t ==="
  g++ -std=c++17 -Wall -Wextra -I include -I /mnt/c/Users/rcruz/vcpkg/installed/x64-windows/include \
    tests/cpp/$t.cpp -o /tmp/$t && /tmp/$t
done
```

Expected: `ALL PASS` for all four.

- [ ] **Step 3: Confirm the scope boundary held — grep for anything accidentally wired live**

```bash
grep -rn "FuseTauStar\|EvaluateTurtleSoupOptionB" src/
```

Expected: zero results (both remain header-only, uncalled from any `.cpp`, per this plan's explicit
scope).

- [ ] **Step 4: Update `SCRATCHPAD.md`** with a dated entry recording: `PredatorContext`/`PredatorFusion`
infrastructure built and tested; Turtle Soup Option A live and tick-reactive (Elder Breakout, Kangaroo
Tail/Momentum Pinball audit, and now Turtle Soup Option A — the entire first-wave Predator batch is
now actually implemented, not just spec'd); `ClassifierParams`/`EvaluateTurtleSoupOptionB()` scaffolded
and tested but deliberately not live, awaiting the lbrnet-rooted training handoff.

- [ ] **Step 5: Final commit**

```bash
git add SCRATCHPAD.md
git commit -m "docs: record Predator infrastructure + Turtle Soup Option A as implemented

Co-Authored-By: Claude Sonnet 5 <noreply@anthropic.com>"
```
