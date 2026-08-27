# Activity-Clock Dual-Clock Kurtosis Implementation Plan

> **STATUS, verified 2026-08-27**: all 15 tasks' code changes exist in the working tree, including
> Task 6's schema field and Task 15's `lbrnet` handoff doc. Both native test suites pass
> (`test_imbalance_bar_engine.cpp` 10/10, `test_kurtosis_gate_logic.cpp` 16/16) and a full
> `./build_dll.sh --no-clean` succeeds cleanly. The individual step checkboxes below were **not**
> ticked incrementally during execution and are left unticked rather than back-filled wholesale —
> they'd misrepresent each task's own "Commit" step, which genuinely has not happened: **nothing is
> committed in either `MindfulTrader` or `../schema`**, and `../schema/PENDING_SCHEMA_CHANGES.md`'s
> PSC-03 row is still `PROPOSED`, not `IMPLEMENTED`. Treat this plan as code-complete,
> build-verified, and commit-pending — the remaining work is committing both repos and finalizing
> PSC-03, not further implementation. See `PRODUCTION_TRIAGE.md` row 1 for the synced status.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Feed the Student-t HMM a genuine Taleb tail-risk signal it currently never sees (Moors-kurtosis, already live and already gating trades five separate ways in C++, but absent from the observation vector), by adding a new `ActivityClockManager` component that builds AFML-style imbalance bars from real tick data and computing kurtosis a second time over that activity clock — and use the fast twin to sharpen the five existing live gates without disturbing their calibration.

**Architecture:** Split into a pure, ACSIL-independent, DOD-shaped `ImbalanceBarEngine` (all real logic: delta tracking, imbalance accumulation, EWMA threshold, fixed-capacity rolling return buffer, kurtosis over that buffer) plus a thin `ActivityClockManager` singleton (matching the `IndicatorManager`/`ContextManager`/`PositionManager`/`RiskManager` convention) that reads Sierra Chart's already-reliable `sc.AskVolume`/`sc.BidVolume`/`sc.Close` every tick and forwards them into the engine — mirrors this codebase's existing `TailRiskEngine`/`InformationEngine` split (pure engine, natively tested; thin ACSIL glue, tested only via the DLL build), not a bespoke new pattern. The existing Moors-kurtosis formula (`RobustMoments.h`) is reused verbatim, computed once over TS3 time-bar returns (already exists, just newly wired to the wire schema) and once over the new engine's imbalance-bar buffer. Both values are added as a new sibling field pair on `LocalRiskContext` and pushed onto the `Event` wire schema at the root level (not inside the fixed 16-field `ObservationData` struct, which must not be extended). The fast value becomes an early-trigger, non-authoritative additive input to the five existing kurtosis-consuming gates.

**Tech Stack:** C++ (ACSIL/Sierra Chart), FlatBuffers (schema repo, `regenerate_schema.sh`), native tests via bare `g++` compilation with a hand-rolled `check(name, bool)` helper (`tests/cpp/`, no test framework — verified against `test_tail_risk_engine.cpp`/`test_feature_scaler.cpp`, this is the real convention).

**Spec:** `docs/superpowers/specs/2026-08-26-activity-clock-tail-risk-and-decay-spec.md` (this plan implements §1-§5 and open question 13's design; §6/§8's sequenced/deferred items — long-memory family, `PredictionAgeUs` decay, `skewness_idx`/`correction_action`/`fisher_info`/`burstiness_index`, skewed-Student-t emission — are explicitly out of scope for this plan).

## Global Constraints

- No heap allocations in recurring ACSIL update paths — every hot-path structure here uses fixed-capacity storage (`RingBuffer<T,Capacity>`, already exists in this codebase — reuse, don't reinvent).
- No invented constants. The EWMA imbalance-bar threshold parameters are explicitly **not** finalized in this plan — Task 3 seeds them with a placeholder marked non-final; deriving real values from historical ES tick data is an explicit follow-on task (Task 15), per this project's standing rule against invented thresholds.
- **Dead/legacy/backward-compatibility code is removed on sight once verified unused** — `PRODUCTION_TRIAGE.md`'s top banner and `CLAUDE.md`'s Code Safety Rules, set 2026-08-26. This system is not in production. Do not add compatibility shims for old behavior anywhere in this plan.
- Schema changes go through `schema/PENDING_SCHEMA_CHANGES.md` (PROPOSED → DECIDED → IMPLEMENTED) before `regenerate_schema.sh` runs. Never call `flatc` directly.
- Never call `build_dll.sh` steps out of order — full clean build via `./build_dll.sh`, incremental via `./build_dll.sh --no-clean`.
- Singleton convention: `static X& Instance();` header declaration, Meyers singleton in `.cpp`, `Init(sc)` called once from `SCStudies.cpp`'s init block (~line 151-157), `Update(sc)` called once per tick from the same per-tick block (~line 410-434).
- The slow (time-bar) kurtosis's threshold *values* remain authoritative for all five existing gates — the fast (activity-clock) kurtosis is additive-only, can trigger caution earlier, never overrides or replaces a calibrated threshold. Mirrors `CLAUDE.md`'s TRAP philosophy ("native governs, model may lead but never suppress the floor").
- **Use the best, institutional DOD constructs this codebase already has — added 2026-08-26, applies to every task below.** This is not a generic "make it fast" aside; it means specifically reusing the patterns this project already converged on, not inventing new ones:
  - **Pure-engine/thin-glue split** (`TailRiskEngine`/`InformationEngine` precedent) — the reason `ImbalanceBarEngine` and `ActivityClockManager` are two classes, not one; keeps the hot-path logic free of ACSIL, virtual dispatch, and RTTI.
  - **`RingBuffer<T,Capacity>`** for every fixed-size rolling window — zero heap allocation for its full lifetime, contiguous `std::array` backing, already the established replacement for `std::deque<T>` at exactly this kind of call site.
  - **Flat POD structs, composition by value** — `LocalRiskContext`/`PredatorContext`'s own documented reasoning ("costs nothing for cache locality... lays out contiguously in memory") applies to `ImbalanceBarEngine`'s own state too: no nested heap-backed members, no indirection where a flat field will do.
  - **No virtual dispatch, no dynamic allocation, on any path called every tick** — matches `IndicatorManager`'s packed-array/enum-indexed O(1) lookup convention (`IndicatorLayout.h`/`IndicatorPackedState.h`) and the project's "true DOD while maintaining OOD goodness" framing (`CLAUDE.md`'s Performance Rules section) — free functions and flat state over class hierarchies, in every task below.

---

## Task 1: `ImbalanceBarEngine` skeleton — pure, DOD-shaped, ACSIL-independent

**Real testing convention, verified 2026-08-26, corrects every earlier reference to GoogleTest/CMake
in this plan**: this codebase does not use GoogleTest or CMake for native `tests/cpp/` tests. The
real, established pattern (`test_tail_risk_engine.cpp`, `test_feature_scaler.cpp`) is a bare
`g++ -std=c++17 -I include tests/cpp/test_X.cpp -o /tmp/X_test && /tmp/X_test`, with a hand-rolled
`check(name, bool)` pass/fail helper — no test framework dependency. `FeatureScaler.h`'s own header
comment confirms *why* this matters: it "was extracted from `ContextManager.h` **specifically to
make this possible**" — the pure/glue split below follows that exact precedent, not a new one.

**Files:**
- Create: `include/ImbalanceBarEngine.h` (pure logic, zero ACSIL dependency)
- Create: `include/ActivityClockManager.h`, `src/ActivityClockManager.cpp` (thin ACSIL glue singleton)
- Modify: `src/SCStudies.cpp:151-157` (Init block), `src/SCStudies.cpp:410-434` (per-tick Update block)
- Test: `tests/cpp/test_imbalance_bar_engine.cpp` (natively tested — the whole reason for the split)

**Interfaces:**
- Produces: `ImbalanceBarEngine` (header-only, POD-style, DOD-shaped — no virtual functions, no heap allocation, matching `TailRiskEngine`/`InformationEngine`'s own shape); `ActivityClockManager::Instance()`, `Init(sc)`, `Reset()`, `Update(sc)` — `ActivityClockManager` owns one `ImbalanceBarEngine` member and forwards real tick data into it. `ActivityClockManager` itself is **not** natively tested (matches `RiskManager`/`ContextManager`/`PositionManager` — none of them have native test precedent either, for the same architectural reason: they `#include "sierrachart.h"` directly, and no ACSIL SDK is vendored for native compilation). Its correctness is verified via `./build_dll.sh` — same standard this codebase already applies to every other `sierrachart.h`-including class.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/cpp/test_imbalance_bar_engine.cpp — unit tests for ImbalanceBarEngine
// (tick-delta imbalance accumulation, AFML-style imbalance-bar construction)
//
// Build & run natively (no Sierra Chart deps, header-only):
//   g++ -std=c++17 -I include tests/cpp/test_imbalance_bar_engine.cpp -o /tmp/ibe_test && /tmp/ibe_test

#include "ImbalanceBarEngine.h"
#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}

int main() {
    {
        ImbalanceBarEngine engine;
        check("fresh engine has zero completed bars", engine.GetCompletedBarCount() == 0);
    }
    {
        ImbalanceBarEngine engine;
        engine.Reset();
        check("Reset clears completed-bar count", engine.GetCompletedBarCount() == 0);
    }
    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -I include tests/cpp/test_imbalance_bar_engine.cpp -o /tmp/ibe_test`
Expected: FAIL — `ImbalanceBarEngine.h` does not exist, compile error.

- [ ] **Step 3: Write minimal implementation**

```cpp
// include/ImbalanceBarEngine.h
// Pure, ACSIL-independent AFML-style dollar-imbalance-bar accumulator.
// DOD-shaped: flat POD state, no virtual dispatch, no heap allocation (fixed-capacity
// RingBuffer backing only) — matches TailRiskEngine.h/InformationEngine.h's own shape.
#pragma once
#include <cstddef>

class ImbalanceBarEngine {
public:
    void Reset() { m_completedBarCount = 0; }
    std::size_t GetCompletedBarCount() const { return m_completedBarCount; }

private:
    std::size_t m_completedBarCount = 0;
};
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -I include tests/cpp/test_imbalance_bar_engine.cpp -o /tmp/ibe_test && /tmp/ibe_test`
Expected: `ALL PASS` (2 checks)

- [ ] **Step 5: Create the thin `ActivityClockManager` glue singleton**

```cpp
// include/ActivityClockManager.h
#pragma once
#include "sierrachart.h"
#include "ImbalanceBarEngine.h"

class ActivityClockManager {
public:
    static ActivityClockManager& Instance();

    void Init(SCStudyInterfaceRef sc);
    void Reset();
    void Update(SCStudyInterfaceRef sc);

    const ImbalanceBarEngine& Engine() const { return m_engine; }

private:
    ActivityClockManager() = default;
    ImbalanceBarEngine m_engine;
};
```

```cpp
// src/ActivityClockManager.cpp
#include "ActivityClockManager.h"

ActivityClockManager& ActivityClockManager::Instance() {
    static ActivityClockManager instance;
    return instance;
}

void ActivityClockManager::Init(SCStudyInterfaceRef sc) {
    Reset();
}

void ActivityClockManager::Reset() {
    m_engine.Reset();
}

void ActivityClockManager::Update(SCStudyInterfaceRef sc) {
    // Tick-delta ingestion added in Task 2.
}
```

- [ ] **Step 6: Wire into the live per-tick loop**

In `src/SCStudies.cpp`, near the existing init block (~line 151-157, alongside `PositionManager::Instance().Init(...)`, `RiskManager::Instance().Init(sc)`, `ContextManager::Instance().Reset()`):

```cpp
ActivityClockManager::Instance().Init(sc);
```

Near the existing per-tick update block (~line 410-434, alongside `RiskManager::Instance().Update(sc)`, `PositionManager::Instance().Update(sc)`):

```cpp
ActivityClockManager::Instance().Update(sc);
```

Add `#include "ActivityClockManager.h"` to `SCStudies.cpp`'s include block.

- [ ] **Step 7: Full build verification**

Run: `./build_dll.sh --no-clean`
Expected: clean build, no new warnings.

- [ ] **Step 8: Commit**

```bash
git add include/ImbalanceBarEngine.h include/ActivityClockManager.h src/ActivityClockManager.cpp src/SCStudies.cpp tests/cpp/test_imbalance_bar_engine.cpp
git commit -m "feat: add ImbalanceBarEngine (pure, DOD-shaped) + ActivityClockManager glue singleton"
```

---

## Task 2: Tick-delta ingestion of `sc.AskVolume`/`sc.BidVolume`

**Files:**
- Modify: `include/ImbalanceBarEngine.h` (pure delta logic — natively tested)
- Modify: `src/ActivityClockManager.cpp` (forwards real `sc` fields into the engine — not natively tested, per Task 1's finding)
- Test: `tests/cpp/test_imbalance_bar_engine.cpp`

**Interfaces:**
- Consumes (in `ActivityClockManager::Update`): `sc.AskVolume[sc.Index]`, `sc.BidVolume[sc.Index]`, `sc.Index` — confirmed intrabar-accumulating per `TripleScreen3.cpp:645-653`'s own design comment and the 2026-08-26 feasibility spike (spec §7).
- Produces: `ImbalanceBarEngine::OnTick(int barIndex, float askVolume, float bidVolume)`, `GetLastSignedDelta()`.

- [ ] **Step 1: Write the failing test**

```cpp
// Append inside main(), before the ALL PASS printf, in tests/cpp/test_imbalance_bar_engine.cpp
{
    ImbalanceBarEngine engine;
    engine.OnTick(/*barIndex=*/5, /*askVolume=*/100.0f, /*bidVolume=*/40.0f);
    // First tick after construction establishes baseline -- delta is the full running total.
    check("delta on first tick of a bar is the full signed total", engine.GetLastSignedDelta() == 60.0f); // 100 - 40

    engine.OnTick(/*barIndex=*/5, /*askVolume=*/130.0f, /*bidVolume=*/45.0f);
    // Same bar (index unchanged) -> delta is incremental: (130-45) - (100-40) = 85 - 60 = 25
    check("delta within same bar is incremental", engine.GetLastSignedDelta() == 25.0f);
}
{
    ImbalanceBarEngine engine;
    engine.OnTick(5, 100.0f, 40.0f);
    // New bar starts fresh at 0 per ACSIL convention -> must NOT compute a spurious
    // large negative delta against the previous bar's final cumulative value.
    engine.OnTick(6, 3.0f, 1.0f);
    check("delta resets across a bar boundary", engine.GetLastSignedDelta() == 2.0f); // 3 - 1, not (3-1)-(100-40)
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -I include tests/cpp/test_imbalance_bar_engine.cpp -o /tmp/ibe_test`
Expected: FAIL — `OnTick`/`GetLastSignedDelta` not declared.

- [ ] **Step 3: Write minimal implementation**

```cpp
// include/ImbalanceBarEngine.h (additions)
public:
    void OnTick(int barIndex, float askVolume, float bidVolume);
    float GetLastSignedDelta() const { return m_lastSignedDelta; }

private:
    int m_lastBarIndex = -1;
    float m_lastAskVolume = 0.0f;
    float m_lastBidVolume = 0.0f;
    float m_lastSignedDelta = 0.0f;
```

```cpp
// include/ImbalanceBarEngine.h (implementation, inline — header-only class matches
// TailRiskEngine.h/InformationEngine.h's own header-only convention)
inline void ImbalanceBarEngine::OnTick(int barIndex, float askVolume, float bidVolume) {
    if (barIndex != m_lastBarIndex) {
        // New bar: sc.AskVolume/BidVolume reset to 0 in ACSIL, so the running total
        // this tick IS the delta -- do not diff against the previous bar's totals.
        m_lastBarIndex = barIndex;
        m_lastAskVolume = 0.0f;
        m_lastBidVolume = 0.0f;
    }
    const float currentSigned = askVolume - bidVolume;
    const float previousSigned = m_lastAskVolume - m_lastBidVolume;
    m_lastSignedDelta = currentSigned - previousSigned;
    m_lastAskVolume = askVolume;
    m_lastBidVolume = bidVolume;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -I include tests/cpp/test_imbalance_bar_engine.cpp -o /tmp/ibe_test && /tmp/ibe_test`
Expected: `ALL PASS` (4 checks)

- [ ] **Step 5: Wire real tick data through `ActivityClockManager::Update(sc)`**

```cpp
// src/ActivityClockManager.cpp
void ActivityClockManager::Update(SCStudyInterfaceRef sc) {
    m_engine.OnTick(sc.Index,
                     static_cast<float>(sc.AskVolume[sc.Index]),
                     static_cast<float>(sc.BidVolume[sc.Index]));
}
```

- [ ] **Step 6: Full build verification**

Run: `./build_dll.sh --no-clean`

- [ ] **Step 7: Commit**

```bash
git add include/ImbalanceBarEngine.h src/ActivityClockManager.cpp tests/cpp/test_imbalance_bar_engine.cpp
git commit -m "feat: tick-delta ingestion of sc.AskVolume/BidVolume in ImbalanceBarEngine"
```

---

## Task 3: Imbalance-bar accumulator with EWMA-adaptive threshold

**Files:**
- Modify: `include/ImbalanceBarEngine.h` (add `#include "RingBuffer.h"`, reuse existing fixed-capacity ring buffer — do not redefine)
- Test: `tests/cpp/test_imbalance_bar_engine.cpp`

**Interfaces:**
- Consumes: `GetLastSignedDelta()` (Task 2).
- Produces: `GetCompletedBarCount()`, `GetImbalanceBarReturns(std::size_t n, float* out)` — Task 5 (kurtosis calculation) consumes this. **Corrected 2026-08-26**: `RingBuffer<T,Capacity>`'s real API is `push_back/pop_front/back()/size()/empty()/clear()/operator[](logicalIndex)` — there is no `copy_last_n` method; `GetImbalanceBarReturns` implements "last N" itself using `size()`/`operator[]`.

- [ ] **Step 1: Write the failing test**

```cpp
// Append inside main(), in tests/cpp/test_imbalance_bar_engine.cpp
{
    ImbalanceBarEngine engine;
    engine.SetImbalanceThresholdForTesting(50.0f); // bypass EWMA for this test

    engine.OnTickWithPrice(1, 20.0f, 0.0f, 100.0f);  // delta=20, cumulative=20
    check("bar not yet closed below threshold", engine.GetCompletedBarCount() == 0);
    engine.OnTickWithPrice(1, 60.0f, 0.0f, 100.5f);  // delta=40, cumulative=60 >= 50 -> bar closes
    check("bar closes once cumulative imbalance crosses threshold", engine.GetCompletedBarCount() == 1);
}
{
    ImbalanceBarEngine engine;
    engine.SetImbalanceThresholdForTesting(10.0f);
    for (int i = 0; i < 5; ++i) {
        engine.OnTickWithPrice(i, static_cast<float>(i + 1) * 12.0f, 0.0f, 100.0f + i); // forces a bar close every tick
    }
    float out[3];
    const std::size_t got = engine.GetImbalanceBarReturns(3, out);
    check("returns buffer is fixed-capacity and rolling (last 3 of 5)", got == 3);
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -I include tests/cpp/test_imbalance_bar_engine.cpp -o /tmp/ibe_test`
Expected: FAIL — new methods not declared.

- [ ] **Step 3: Write minimal implementation**

```cpp
// include/ImbalanceBarEngine.h (additions)
#include "RingBuffer.h"
#include <cmath>

static constexpr std::size_t kImbalanceBarBufferCapacity = 500; // matches TailRiskEngine's windowSize=500 order of magnitude; revisit once real calibration (Task 15) exists

public:
    void OnTickWithPrice(int barIndex, float askVolume, float bidVolume, float price);
    std::size_t GetImbalanceBarReturns(std::size_t n, float* out) const;
    void SetImbalanceThresholdForTesting(float threshold) { m_imbalanceThreshold = threshold; }

private:
    float m_cumulativeImbalance = 0.0f;
    float m_imbalanceThreshold = 50.0f; // NON-FINAL placeholder -- see Task 15, no invented constant should ship without real ES tick calibration
    float m_barOpenPrice = 0.0f;
    bool m_haveBarOpenPrice = false;
    RingBuffer<float, kImbalanceBarBufferCapacity> m_completedBarReturns;
```

```cpp
// include/ImbalanceBarEngine.h (implementation, inline)
inline void ImbalanceBarEngine::OnTickWithPrice(int barIndex, float askVolume, float bidVolume, float price) {
    OnTick(barIndex, askVolume, bidVolume);
    if (!m_haveBarOpenPrice) {
        m_barOpenPrice = price;
        m_haveBarOpenPrice = true;
    }
    m_cumulativeImbalance += m_lastSignedDelta;

    if (std::abs(m_cumulativeImbalance) >= m_imbalanceThreshold) {
        const float barReturn = (m_barOpenPrice > 0.0f)
            ? std::log(price / m_barOpenPrice)
            : 0.0f;
        if (m_completedBarReturns.size() == kImbalanceBarBufferCapacity) {
            m_completedBarReturns.pop_front(); // evict oldest before pushing, per RingBuffer's real push_back contract
        }
        m_completedBarReturns.push_back(barReturn);
        ++m_completedBarCount;
        // Reset for the next imbalance bar.
        m_cumulativeImbalance = 0.0f;
        m_haveBarOpenPrice = false;
        // EWMA threshold update deferred to Task 15 (needs real historical calibration,
        // not an invented constant).
    }
}

inline std::size_t ImbalanceBarEngine::GetImbalanceBarReturns(std::size_t n, float* out) const {
    // RingBuffer has no copy_last_n -- implemented directly via size()/operator[] (both O(1)).
    const std::size_t available = m_completedBarReturns.size();
    const std::size_t count = (n < available) ? n : available;
    const std::size_t startLogicalIndex = available - count; // oldest of the last `count` entries
    for (std::size_t i = 0; i < count; ++i) {
        out[i] = m_completedBarReturns[startLogicalIndex + i];
    }
    return count;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -I include tests/cpp/test_imbalance_bar_engine.cpp -o /tmp/ibe_test && /tmp/ibe_test`
Expected: `ALL PASS` (6 checks)

- [ ] **Step 5: Full build verification**

Run: `./build_dll.sh --no-clean`

- [ ] **Step 6: Commit**

```bash
git add include/ImbalanceBarEngine.h tests/cpp/test_imbalance_bar_engine.cpp
git commit -m "feat: imbalance-bar accumulator with fixed-capacity rolling return buffer"
```

---

## Task 4: Wire real price into `ActivityClockManager::Update(sc)`

**Files:**
- Modify: `src/ActivityClockManager.cpp`
- Test: no new native test (this task only wires an already-tested pure-engine path to real ACSIL data — matches this codebase's own convention that the glue layer itself isn't natively tested)

**Interfaces:**
- Consumes: `sc.Close[sc.Index]` (current price).
- Produces: `ActivityClockManager::Update(sc)` now calls `m_engine.OnTickWithPrice(...)` instead of `m_engine.OnTick(...)`.

- [ ] **Step 1: Update `Update(sc)`**

```cpp
// src/ActivityClockManager.cpp
void ActivityClockManager::Update(SCStudyInterfaceRef sc) {
    m_engine.OnTickWithPrice(sc.Index,
                              static_cast<float>(sc.AskVolume[sc.Index]),
                              static_cast<float>(sc.BidVolume[sc.Index]),
                              static_cast<float>(sc.Close[sc.Index]));
}
```

- [ ] **Step 2: Full build verification**

Run: `./build_dll.sh --no-clean`

- [ ] **Step 3: Commit**

```bash
git add src/ActivityClockManager.cpp
git commit -m "feat: wire real tick price into ImbalanceBarEngine via ActivityClockManager"
```

---

## Task 5: Dual-clock kurtosis — activity-clock twin, `LocalRiskContext.fastTalebKurtosis`

**Files:**
- Modify: `include/LocalRiskContext.h:20` (add sibling field)
- Modify: `src/ContextManager.cpp:553` (populate at the existing single-write point)
- Test: `tests/cpp/test_imbalance_bar_engine.cpp` (extend — the kurtosis-over-the-engine's-buffer computation is itself pure and ACSIL-independent, so it belongs in the same natively-tested file, not a new one)

**Interfaces:**
- Consumes: `ImbalanceBarEngine::GetImbalanceBarReturns(n, out)` (Task 3), `RobustMoments::MoorsKurtosis` (existing, `include/RobustMoments.h:44-55`).
- Produces: `LocalRiskContext::fastTalebKurtosis` — Task 8-13 consume this in gate integration.

- [ ] **Step 1: Write the failing test**

```cpp
// Add near the top of tests/cpp/test_imbalance_bar_engine.cpp
#include "RobustMoments.h"
#include <cstdlib> // for the synthetic price walk below

// Append inside main() -- CORRECTED: MoorsKurtosis(std::array<float,100>) takes a
// fixed-size array BY VALUE, no namespace wrapper, no (pointer, count) overload.
{
    ImbalanceBarEngine engine;
    engine.SetImbalanceThresholdForTesting(1.0f);

    // Force 100 imbalance-bar closes with a bounded synthetic price walk.
    for (int i = 0; i < 100; ++i) {
        const float price = 100.0f + static_cast<float>(i % 7) * 0.1f;
        engine.OnTickWithPrice(i, 2.0f, 0.0f, price); // delta always exceeds threshold=1.0f
    }
    float rawReturns[100];
    const std::size_t n = engine.GetImbalanceBarReturns(100, rawReturns);
    check("100 imbalance bars closed as expected", n == 100);

    std::array<float, 100> returnsArray;
    std::copy(rawReturns, rawReturns + n, returnsArray.begin());
    const float directKurtosis = MoorsKurtosis(returnsArray);
    check("Moors kurtosis over the engine's own buffer is finite", std::isfinite(directKurtosis));
}
```

Add `#include <array>` and `#include <algorithm>` (for `std::copy`) to the test file's includes.

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -I include tests/cpp/test_imbalance_bar_engine.cpp -o /tmp/ibe_test`
Expected: FAIL — new check block references `MoorsKurtosis`, fine since `RobustMoments.h` is already included; actually expected to fail only if `GetImbalanceBarReturns`'s 100-element assumption doesn't hold — this step is mostly a formality since Task 3 already made this compile. Confirm the two new checks fail meaningfully (i.e. would catch a real regression) before moving to Step 3.

- [ ] **Step 3: Add the `LocalRiskContext` field**

```cpp
// include/LocalRiskContext.h (near line 20, alongside the existing field)
float talebKurtosis = 0.0f;        // Moors (1988) octile kurtosis (1.233 = N(0,1) neutral, clamped [0,5]) -- TIME-BAR clock
float fastTalebKurtosis = 0.0f;    // Same Moors formula, computed over ActivityClockManager's imbalance-bar
                                    // return buffer instead of TS3 time-bar returns -- ACTIVITY clock.
                                    // Additive-only signal: see RiskManager/Scoring/PositionManager/
                                    // TradeDecisionEngine integration -- never overrides the time-bar value's
                                    // calibrated thresholds, can only trigger caution earlier.
```

- [ ] **Step 4: Run the native test to confirm it still passes**

Run: `g++ -std=c++17 -I include tests/cpp/test_imbalance_bar_engine.cpp -o /tmp/ibe_test && /tmp/ibe_test`
Expected: `ALL PASS` (8 checks)

- [ ] **Step 5: Populate `fastTalebKurtosis` at the existing single-write point**

In `src/ContextManager.cpp`, at the same write point that already sets `m_localRiskContext.talebKurtosis` (line 553):

```cpp
// src/ContextManager.cpp:553 (existing line, unchanged)
m_localRiskContext.talebKurtosis = m_latestInstitutionalMetrics.talebKurtosis;

// New line immediately after:
{
    float rawReturns[ImbalanceBarEngine::kImbalanceBarBufferCapacity];
    const std::size_t n = ActivityClockManager::Instance().Engine().GetImbalanceBarReturns(
        100, rawReturns); // 100 matches CalculateRealizedKurtosis's own KURT_WINDOW and MoorsKurtosis's fixed array size
    if (n >= 100) {
        std::array<float, 100> returnsArray;
        std::copy(rawReturns, rawReturns + 100, returnsArray.begin());
        m_localRiskContext.fastTalebKurtosis = MoorsKurtosis(returnsArray);
    } else {
        m_localRiskContext.fastTalebKurtosis = 1.23f; // Moors octile-kurtosis N(0,1) neutral baseline,
            // matches CalculateRealizedKurtosis's own warmup value (StudyHelperFunctions.cpp:2917) --
            // insufficient imbalance bars closed yet
    }
}
```

Add `#include "ActivityClockManager.h"`, `#include "RobustMoments.h"`, and `#include <array>` to `ContextManager.cpp`'s includes if not already present. Note `Engine()` (Task 1's accessor) — `ContextManager` reads the pure engine's buffer through `ActivityClockManager`, not directly, keeping the glue singleton as the one access point per this codebase's existing singleton conventions.

- [ ] **Step 6: Full build verification**

Run: `./build_dll.sh --no-clean`

- [ ] **Step 7: Commit**

```bash
git add include/LocalRiskContext.h src/ContextManager.cpp tests/cpp/test_imbalance_bar_engine.cpp
git commit -m "feat: compute fastTalebKurtosis (activity-clock Moors kurtosis twin) in LocalRiskContext"
```

---

## Task 6: Schema — add the two new dims to the wire schema (cross-repo: `schema/` repo)

**CORRECTED 2026-08-27 — this task was NOT executed as written below.** The steps below (and the
open sub-question) describe adding `fast_taleb_kurtosis` to `table Event` near `nh_nl_daily`, NOT
inside `ObservationData`. Verified directly against `mts_schema.fbs` and all 3 repos' generated
bindings: what was actually built instead is `fast_taleb_kurtosis` as **`ObservationData`'s 17th
field directly** (16D→17D). This wasn't a bug — it matches (and is the real-world precedent for)
`PRODUCTION_TRIAGE.md` row 14's 2026-08-27 decision to keep `ObservationData` a struct and edit its
fields in place rather than route new fields through `Event`/`HMM_OBSERVATION_EXTENSIONS`. Left the
original steps below for history; do not follow them as written if replicating this pattern for
another field — add directly to the `ObservationData` struct instead, per row 14's policy.

**Now already logged as PSC-03** in `../schema/PENDING_SCHEMA_CHANGES.md` (added 2026-08-26,
same day this plan was written), with the batching question already resolved:
`../schema/docs/ADR/2026-08-26-schema-change-consolidation-and-sequencing.md` decided this proceeds
independently of PSC-01/PSC-02 (unrelated fields, no shape dependency, and those two have no
committed resolution timeline) — this task executes that ADR's decision, not a fresh one. Steps
below are unchanged; PSC-03's row already carries the exact field shape and open sub-question.

**Files:**
- Modify: `../schema/PENDING_SCHEMA_CHANGES.md` (PSC-03 already PROPOSED — move to DECIDED, then IMPLEMENTED as this task proceeds)
- Modify: `../schema/mts_schema.fbs`
- Regenerate: `include/generated/mts_schema_contract_generated.h` (via script, not by hand)

**Interfaces:**
- Produces: `MTS::Schema::Event::fast_taleb_kurtosis` (new flat float field on the `Event` table, matching `nh_nl_daily`'s pattern) — Task 7 populates it.

**This task is genuinely cross-repo** (`schema/` is its own git repository per the 2026-08-26 research pass, governed by its own `PENDING_SCHEMA_CHANGES.md` process and `schema_change_control_gate.py` sha256 drift check) — it is *not* gated behind a separate session the way `lbrnet` work is in this project's convention, since `schema` changes are explicitly the shared contract all three C++/Python consumers read, but it must follow that repo's own procedural discipline, not be edited ad hoc.

- [ ] **Step 1: Confirm PSC-03's logged entry still matches reality**

`../schema/PENDING_SCHEMA_CHANGES.md`'s PSC-03 row (added 2026-08-26, same day as this plan) already
states the field shape, the `ObservationData`-is-a-fixed-`struct` constraint, the `lbrnet` consumer,
and the nested-vs-flat open sub-question below. Re-read that row now — if anything has drifted since
this plan was written (e.g. the sub-question already got answered elsewhere), update the row before
proceeding rather than re-deciding it here.

**Open sub-question, resolve during this task** (already logged in PSC-03, restated here for the
implementer): the existing (slow, time-bar) `talebKurtosis` is already on the wire via
`Event.asymmetry_context.taleb_kurtosis` (a nested field), but `HMM_OBSERVATION_EXTENSIONS` uses
flat top-level `Event` field names (matching `nh_nl_daily`). Check with a `lbrnet`-side read (or
flag explicitly in the Task 15 handoff doc) whether `live_agent.py`'s accessor can read a nested
field directly, or whether the slow kurtosis also needs a flat passthrough field on `Event` (e.g.
`slow_taleb_kurtosis: float;`) to fit the existing convention. Do not assume either way — this
task's own schema diff should add whichever the answer requires (one new field if nested access
works, two if a flat passthrough is also needed).

- [ ] **Step 2: Add the field to `mts_schema.fbs`**

In `../schema/mts_schema.fbs`, near line 312 (immediately after `nh_nl_daily`):

```
  nh_nl_daily: float;       // Added for HMM Regime Awareness (Raw Index)
  fast_taleb_kurtosis: float; // Moors (1988) octile kurtosis over ActivityClockManager's
                               // imbalance-bar return buffer -- activity clock, additive-only
                               // input to existing gates, see docs/superpowers/specs/
                               // 2026-08-26-activity-clock-tail-risk-and-decay-spec.md
```

If Step 1's open sub-question resolves to "needs a flat passthrough," also add:
```
  slow_taleb_kurtosis: float; // Passthrough of asymmetry_context.taleb_kurtosis for
                               // HMM_OBSERVATION_EXTENSIONS' flat-accessor convention
```

- [ ] **Step 3: Update `PENDING_SCHEMA_CHANGES.md` to DECIDED**

Change PSC-03's status line to `DECIDED, 2026-08-26` once the field addition itself is confirmed correct (no code changes yet — decision precedes regeneration).

- [ ] **Step 4: Regenerate the schema**

Run: `bash /home/rcruz/devel/VSCode/scripts/regenerate_schema.sh`
Expected: `include/generated/mts_schema_contract_generated.h` and `mts_schema_generated.h` regenerate cleanly, new field(s) appear as generated accessors, `kObservationDim` unchanged (the new field is NOT part of `ObservationData`).

- [ ] **Step 5: Update `PENDING_SCHEMA_CHANGES.md` to IMPLEMENTED**

- [ ] **Step 6: Commit**

```bash
cd /home/rcruz/devel/VSCode/schema
git add PENDING_SCHEMA_CHANGES.md mts_schema.fbs
git commit -m "feat: add fast_taleb_kurtosis (and slow_taleb_kurtosis passthrough if needed) to Event table"
cd /home/rcruz/devel/VSCode/MindfulTrader
git add include/generated/mts_schema_contract_generated.h include/generated/mts_schema_generated.h
git commit -m "chore: regenerate schema headers for fast_taleb_kurtosis"
```

---

## Task 7: Populate the new wire field(s) from `LocalRiskContext`

**Files:**
- Modify: `src/EventSerializer.cpp` (or wherever `event.nh_nl_daily = ...`-style assignment happens — confirm exact call site by grepping for the existing `nh_nl_daily`/`Indicator.h:2154` pattern before editing)
- Test: extend an existing `EventSerializer` test, or add a focused native test asserting the new field round-trips correctly through `MakeEvent()`/equivalent builder call.

**Interfaces:**
- Consumes: `ContextManager::Instance().GetLocalRiskContext().fastTalebKurtosis` (Task 5).
- Produces: `Event.fast_taleb_kurtosis` populated on every live/collection event.

- [ ] **Step 1: Locate the exact `Event` population call site**

Run: `grep -n "event\.nh_nl_daily\|mutate_nh_nl_daily" src/*.cpp include/*.h`

Use whichever call site this returns (per Task-6-research, `include/Indicator.h:2154`'s `event.nh_nl_daily = m_dailyValue;` is the live pattern) as the exact template — write the new field assignment immediately adjacent to it, inside the same function, so both regime-awareness-style extension fields are populated in one place.

- [ ] **Step 2: Write the failing test**

```cpp
// Extend the relevant existing EventSerializer/Event-population test file.
// Exact test name/fixture depends on Step 1's findings -- follow that file's
// existing pattern for asserting a field round-trips (e.g. how nh_nl_daily
// or a comparable float field is already asserted elsewhere in that file).
TEST(EventPopulationTest, FastTalebKurtosisIsPopulatedFromLocalRiskContext) {
    ContextManager::Instance().Reset();
    // Force a known fastTalebKurtosis value via the test seam already used
    // for other LocalRiskContext fields in this test file, then assert the
    // built Event reflects it.
}
```

- [ ] **Step 3: Run test to verify it fails**

Expected: FAIL — field not yet assigned.

- [ ] **Step 4: Write the assignment**

```cpp
// At the exact call site found in Step 1, immediately after the nh_nl_daily assignment:
event.fast_taleb_kurtosis = ContextManager::Instance().GetLocalRiskContext().fastTalebKurtosis;
```

- [ ] **Step 5: Run test to verify it passes**

- [ ] **Step 6: Full build verification**

Run: `./build_dll.sh --no-clean`

- [ ] **Step 7: Commit**

```bash
git add -A
git commit -m "feat: populate Event.fast_taleb_kurtosis from LocalRiskContext"
```

---

## Task 8: `ExecutionParams` config fields for the fast gate thresholds

**Verification approach for Tasks 8, 10-14, decided then revised 2026-08-26**: `RiskManager`/
`Scoring`/`PositionManager`/`TradeDecisionEngine` all `#include "sierrachart.h"` directly with no
vendored SDK available for native compilation, and none of them have ever had native test coverage
in this codebase's history (confirmed: no `test_risk_manager.cpp`, `test_scoring.cpp`,
`test_position_manager.cpp`, or `test_trade_decision_engine.cpp` exist). Rather than accept that as
a reason to skip testing entirely, **Task 9 extracts the actual decision logic** (the boolean/
arithmetic conditions these five gates evaluate) into a pure, ACSIL-independent header and tests it
directly there — matching `FeatureScaler.h`'s own extraction precedent. Tasks 8, 10-14 themselves
remain small, low-risk call-site edits (declare a config field, or call an already-tested function)
verified via `./build_dll.sh` plus a short manual-verification checklist — the meaningful logic
itself is covered by Task 9's real tests, not left to inspection alone.

**Corrected 2026-08-26, before any code was written**: the original design (spec §4 item 4) named
a standalone "new fast hard-gate" *and* an "early-trigger additive input to the existing five
gates" as if they were two separate mechanisms. For the hard-halt case specifically, they'd be
redundant — a standalone `EvaluateFastKurtosisGate` would never be called once Task 10 wires the
fast signal directly into `EvaluateHardGates` via `ShouldHaltOnKurtosis`, making it dead code the
moment it ships. Dropped the standalone function entirely; this task now only adds the config
fields Tasks 10-14 need. No redundant, never-called gate function gets created.

**Files:**
- Modify: `include/ExecutionParams.h` (wherever `talebKurtosisHaltThreshold` is declared)

**Interfaces:**
- Produces: `ExecutionParams::fastTalebKurtosisHaltThreshold`, consumed directly by Task 10's `ShouldHaltOnKurtosis` call inside the existing `EvaluateHardGates`.

- [ ] **Step 1: Confirm `talebKurtosisHaltThreshold`'s exact declaration site**

Run: `grep -n "talebKurtosisHaltThreshold" include/ExecutionParams.h`

- [ ] **Step 2: Add the sibling config field**

```cpp
// include/ExecutionParams.h (alongside talebKurtosisHaltThreshold, exact location from Step 1)
float fastTalebKurtosisHaltThreshold = 1.8401f; // NON-FINAL placeholder, mirrors the slow gate's
                                                  // current value until Task 15's real historical
                                                  // calibration exists -- flagged, not invented-and-forgotten
```

- [ ] **Step 3: Full build verification**

Run: `./build_dll.sh --no-clean`

- [ ] **Step 4: Manual verification checklist**

- [ ] Confirm the field sits in the same struct/section as `talebKurtosisHaltThreshold`, same naming convention (camelCase, `...Threshold` suffix).
- [ ] Confirm no call site references this field yet — it's genuinely unused until Task 9, which is expected for one commit, not a smell (Task 9 lands immediately after).

- [ ] **Step 5: Commit**

```bash
git add include/ExecutionParams.h
git commit -m "feat: add fastTalebKurtosisHaltThreshold config field to ExecutionParams"
```

---

## Task 9: Extract kurtosis gate-decision logic into a pure, testable header

**Retracted and corrected, 2026-08-26**: the earlier decision to verify Tasks 8, 10-14 via
`./build_dll.sh` + manual review only is retracted. Where the decision logic is meaningful (has a
real edge case, an asymmetry that's easy to get backwards, or is duplicated across call sites),
extract it into a pure, ACSIL-independent function and test it directly — matching
`FeatureScaler.h`'s own extraction precedent, not inventing a new one. This task does the
extraction once; Tasks 10-14 each become a small, low-risk call-site edit that *uses* an
already-tested function instead of inlining and hand-verifying boolean logic five separate times.

**Files:**
- Create: `include/KurtosisGateLogic.h` (pure, ACSIL-independent)
- Test: `tests/cpp/test_kurtosis_gate_logic.cpp`

**Interfaces:**
- Produces: `ShouldHaltOnKurtosis`, `ShouldEnterKurtosisCrisis`, `ShouldExitKurtosisCrisis`,
  `ShouldApplyFragilityPenalty`, `ShouldCapChase`, `IsCrashRegime`, `ComputeTailRiskPremium` — Tasks
  10-14 each call exactly one of these at their respective call sites.

**One design point worth stating explicitly**: `ShouldExitKurtosisCrisis` takes **only** the slow
kurtosis value as its numeric parameter — there is no `fastKurtosis` parameter at all. This makes
the enter/exit asymmetry (§ design note, Task 11) impossible to violate by accident at the call
site, not just documented by comment. That's the actual benefit of extracting this particular
function, beyond testability.

**A real correctness finding from doing this extraction carefully**: the two `PositionManager.cpp`
conditions this plan originally called "the same 4 sites" are not identical — the chase-cap
condition (`:2313-2321`/`:2764-2771`) has no Amihud term; the crash-regime/stop-type condition
(`:2351-2358`/`:2785-2792`) does. These need **two** distinct shared functions
(`ShouldCapChase`, `IsCrashRegime`), not one — caught here, before Task 13 would have quietly
extracted the wrong shared shape.

- [ ] **Step 1: Write the failing test**

```cpp
// tests/cpp/test_kurtosis_gate_logic.cpp — unit tests for the extracted kurtosis
// gate-decision functions (fast/slow dual-clock early-trigger logic).
//
// Build & run natively (no Sierra Chart deps, header-only):
//   g++ -std=c++17 -I include tests/cpp/test_kurtosis_gate_logic.cpp -o /tmp/kgl_test && /tmp/kgl_test

#include "KurtosisGateLogic.h"
#include <cstdio>
#include <limits>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}

int main() {
    // ShouldHaltOnKurtosis
    check("halts when slow exceeds its own threshold",
          ShouldHaltOnKurtosis(2.0f, 1.8401f, 0.0f, 1.8401f));
    check("halts when fast exceeds its own threshold, slow calm",
          ShouldHaltOnKurtosis(0.5f, 1.8401f, 2.0f, 1.8401f));
    check("does not halt when both are calm",
          !ShouldHaltOnKurtosis(0.5f, 1.8401f, 0.5f, 1.8401f));

    // ShouldEnterKurtosisCrisis
    check("enters on slow alone", ShouldEnterKurtosisCrisis(2.0f, 1.8f, 0.0f, 1.8f));
    check("enters on fast alone, slow calm", ShouldEnterKurtosisCrisis(0.5f, 1.8f, 2.0f, 1.8f));
    const float nanFast = std::numeric_limits<float>::quiet_NaN();
    check("NaN fast reading does not spuriously enter",
          !ShouldEnterKurtosisCrisis(0.5f, 1.8f, nanFast, 1.8f));

    // ShouldExitKurtosisCrisis -- note the signature has no fast-kurtosis parameter at all
    check("exits when slow drops below exit threshold", ShouldExitKurtosisCrisis(1.0f, 1.5f));
    check("does not exit while slow stays elevated", !ShouldExitKurtosisCrisis(1.6f, 1.5f));

    // ShouldApplyFragilityPenalty
    check("penalty guard trips on fast alone",
          ShouldApplyFragilityPenalty(1.0f, 1.3248f, 2.0f, 1.3248f));
    check("penalty guard stays closed when both below guard",
          !ShouldApplyFragilityPenalty(1.0f, 1.3248f, 1.0f, 1.3248f));

    // ShouldCapChase -- no Amihud term
    check("chase cap trips on fast kurtosis alone",
          ShouldCapChase(10.0f, 4.0f, 1.0f, 2.0f, 1.8530f));
    check("chase cap trips on low DOF alone",
          ShouldCapChase(3.0f, 4.0f, 1.0f, 1.0f, 1.8530f));
    check("chase cap stays open when nothing crosses",
          !ShouldCapChase(10.0f, 4.0f, 1.0f, 1.0f, 1.8530f));

    // IsCrashRegime -- has the Amihud term ShouldCapChase deliberately lacks
    check("crash regime trips on Amihud percentile alone",
          IsCrashRegime(10.0f, 4.0f, 1.0f, 1.0f, 1.8530f, 0.95f, 0.90f));
    check("crash regime stays false when nothing crosses",
          !IsCrashRegime(10.0f, 4.0f, 1.0f, 1.0f, 1.8530f, 0.50f, 0.90f));

    // ComputeTailRiskPremium
    const double premiumFastDriven = ComputeTailRiskPremium(
        /*hillPenalty=*/0.0, /*slowKurtosis=*/1.0, /*fastKurtosis=*/2.5,
        /*kurtosisPenaltyLow=*/1.6414, /*kurtosisPenaltyHigh=*/2.0064,
        /*mahalanobis=*/0.0, /*mahalPenaltyLow=*/4.0, /*mahalPenaltyHigh=*/8.0);
    check("tail-risk premium rises above 1.0 on fast kurtosis alone", premiumFastDriven > 1.0);

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -I include tests/cpp/test_kurtosis_gate_logic.cpp -o /tmp/kgl_test`
Expected: FAIL — `KurtosisGateLogic.h` does not exist, compile error.

- [ ] **Step 3: Write the implementation**

```cpp
// include/KurtosisGateLogic.h
// Pure, ACSIL-independent decision logic for how the fast (activity-clock) Taleb-kurtosis
// signal early-triggers the five existing kurtosis-consuming gates, without the fast signal
// ever being able to override the slow (time-bar), calibrated statistic's own threshold values.
// Extracted specifically to make this logic natively testable (tests/cpp/test_kurtosis_gate_logic.cpp),
// same rationale as FeatureScaler.h/TailRiskEngine.h/InformationEngine.h/RobustMoments.h.
#pragma once
#include <algorithm>
#include <cmath>

inline bool ShouldHaltOnKurtosis(float slowKurtosis, float slowThreshold,
                                  float fastKurtosis, float fastThreshold) {
    return slowKurtosis > slowThreshold || fastKurtosis > fastThreshold;
}

inline bool ShouldEnterKurtosisCrisis(float slowKurtosis, float slowEnterThreshold,
                                       float fastKurtosis, float fastEnterThreshold) {
    return slowKurtosis > slowEnterThreshold ||
           (std::isfinite(fastKurtosis) && fastKurtosis > fastEnterThreshold);
}

// Deliberately takes ONLY the slow value -- no fastKurtosis parameter exists, so recovery
// confirmation cannot be wired to the fast, transient signal even by accident.
inline bool ShouldExitKurtosisCrisis(float slowKurtosis, float slowExitThreshold) {
    return slowKurtosis < slowExitThreshold;
}

inline bool ShouldApplyFragilityPenalty(float slowKurtosis, float slowGuard,
                                         float fastKurtosis, float fastGuard) {
    return slowKurtosis > slowGuard || fastKurtosis > fastGuard;
}

// Chase-cap composite -- no Amihud term (distinct from IsCrashRegime below).
inline bool ShouldCapChase(float dof, float dofThreshold,
                            float slowKurtosis, float fastKurtosis, float kurtosisThreshold) {
    return dof <= dofThreshold ||
           slowKurtosis > kurtosisThreshold ||
           (std::isfinite(fastKurtosis) && fastKurtosis > kurtosisThreshold);
}

// Crash-regime/stop-type composite -- includes the Amihud term ShouldCapChase lacks.
inline bool IsCrashRegime(float dof, float dofThreshold,
                           float slowKurtosis, float fastKurtosis, float kurtosisThreshold,
                           float amihudPercentile, float amihudThreshold) {
    return (dof <= dofThreshold) ||
           (slowKurtosis > kurtosisThreshold) ||
           (std::isfinite(fastKurtosis) && fastKurtosis > kurtosisThreshold) ||
           (amihudPercentile > amihudThreshold);
}

inline double ComputeTailRiskPremium(double hillPenalty,
                                      double slowKurtosis, double fastKurtosis,
                                      double kurtosisPenaltyLow, double kurtosisPenaltyHigh,
                                      double mahalanobis,
                                      double mahalPenaltyLow, double mahalPenaltyHigh) {
    const double kurtosisPenalty = std::clamp(
        (slowKurtosis - kurtosisPenaltyLow) / (kurtosisPenaltyHigh - kurtosisPenaltyLow), 0.0, 1.0);
    const double fastKurtosisPenalty = std::clamp(
        (fastKurtosis - kurtosisPenaltyLow) / (kurtosisPenaltyHigh - kurtosisPenaltyLow), 0.0, 1.0);
    const double mahalPenalty = std::clamp(
        (mahalanobis - mahalPenaltyLow) / (mahalPenaltyHigh - mahalPenaltyLow), 0.0, 1.0);
    return 1.0 + std::max({hillPenalty, kurtosisPenalty, fastKurtosisPenalty, mahalPenalty});
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -I include tests/cpp/test_kurtosis_gate_logic.cpp -o /tmp/kgl_test && /tmp/kgl_test`
Expected: `ALL PASS` (14 checks)

- [ ] **Step 5: Commit**

```bash
git add include/KurtosisGateLogic.h tests/cpp/test_kurtosis_gate_logic.cpp
git commit -m "feat: extract kurtosis gate-decision logic into a pure, natively-tested header"
```

---

## Task 10: Early-trigger integration — hard halt (`RiskManager.cpp:884-889`)

**Files:**
- Modify: `src/RiskManager.cpp:884-889` (and verify the duplicate call site at line 2514 — same gate or separate check)

**Interfaces:**
- Consumes: `ShouldHaltOnKurtosis` (Task 9), `ctx.fastTalebKurtosis`, `m_execParams.fastTalebKurtosisHaltThreshold` (Task 8).

- [ ] **Step 1: Verify the line-2514 call site first**

Run: `grep -n -B5 "talebKurtosisHaltThreshold" src/RiskManager.cpp`

Confirm whether the line-2514 occurrence is a second independent gate evaluation (needs the same edit applied) or a duplicate caller of the same `EvaluateHardGates` function (needs no separate edit). Record the finding in the commit message.

- [ ] **Step 2: Call the extracted function**

```cpp
// src/RiskManager.cpp:884-889, modified
if (ShouldHaltOnKurtosis(ctx.talebKurtosis, m_execParams.talebKurtosisHaltThreshold,
                          ctx.fastTalebKurtosis, m_execParams.fastTalebKurtosisHaltThreshold)) {
    return Result<void>::Failure(
        "HARD_GATE: Taleb kurtosis critical"
        " | taleb_kurtosis=" + std::to_string(ctx.talebKurtosis) +
        " fast_taleb_kurtosis=" + std::to_string(ctx.fastTalebKurtosis) +
        " threshold=" + std::to_string(m_execParams.talebKurtosisHaltThreshold));
}
```

Add `#include "KurtosisGateLogic.h"` to `RiskManager.cpp`'s includes. Apply the identical edit to
the line-2514 site only if Step 1 found it to be a separate check.

- [ ] **Step 3: Full build verification**

Run: `./build_dll.sh --no-clean`

- [ ] **Step 4: Manual verification checklist**

Most of the actual decision logic is already covered by Task 9's native test — this checklist is
narrower than it would otherwise need to be, precisely because of that extraction.

- [ ] Confirm the argument order matches `ShouldHaltOnKurtosis`'s signature (`slowKurtosis,
  slowThreshold, fastKurtosis, fastThreshold`) — a transposed pair would compile but be silently
  wrong, and there's no test at this specific call site to catch it.
- [ ] Confirm the failure message still includes both values (`taleb_kurtosis=`/`fast_taleb_kurtosis=`).
- [ ] If Step 1 found line 2514 to be a separate check, confirm it received the identical edit.

- [ ] **Step 5: Commit**

```bash
git add src/RiskManager.cpp
git commit -m "feat: fast kurtosis early-triggers the hard halt gate via ShouldHaltOnKurtosis"
```

---

## Task 11: Early-trigger integration — crisis hysteresis ENTER only (`RiskManager.cpp:899-934`)

**Files:**
- Modify: `include/ExecutionParams.h` (new config field), `src/RiskManager.cpp:899-934` (`RefreshKurtosisEmergencyState`)

**Interfaces:**
- Consumes: `ShouldEnterKurtosisCrisis`, `ShouldExitKurtosisCrisis` (Task 9), `ContextManager::Instance().GetLocalRiskContext().fastTalebKurtosis`.

**Design note**: enter and exit are asymmetric on purpose — the fast signal may trigger entry
early, but must not confirm recovery. Task 9 already enforces this at the type level
(`ShouldExitKurtosisCrisis` has no `fastKurtosis` parameter to pass), so this task's own
verification burden is much smaller than it would be with inline logic.

- [ ] **Step 1: Add the config field**

```cpp
// include/ExecutionParams.h
float fastTalebKurtosisCrisisEnter = 1.8530f; // NON-FINAL placeholder, mirrors talebKurtosisCrisisEnter
                                                // until Task 15's real calibration exists
```

- [ ] **Step 2: Call the extracted functions**

```cpp
// src/RiskManager.cpp:899-934, modified
const float kurtosis = localCtx.talebKurtosis;
const float fastKurtosis = localCtx.fastTalebKurtosis;
if (!std::isfinite(kurtosis)) return;
m_lastObservedKurtosis.store(kurtosis, std::memory_order_relaxed);
const bool wasActive = m_kurtosisEmergencyActive.load(std::memory_order_relaxed);
bool nowActive = wasActive;
if (!wasActive && ShouldEnterKurtosisCrisis(kurtosis, m_execParams.talebKurtosisCrisisEnter,
                                             fastKurtosis, m_execParams.fastTalebKurtosisCrisisEnter)) {
    nowActive = true;
} else if (wasActive && ShouldExitKurtosisCrisis(kurtosis, m_execParams.talebKurtosisCrisisExit)) {
    nowActive = false;
}
```

Add `#include "KurtosisGateLogic.h"` to `RiskManager.cpp`'s includes.

- [ ] **Step 3: Full build verification**

Run: `./build_dll.sh --no-clean`

- [ ] **Step 4: Manual verification checklist**

- [ ] Confirm `ShouldExitKurtosisCrisis` is called with only `kurtosis` and the exit threshold — its
  signature has no slot for `fastKurtosis`, so passing it would be a compile error, not a silent bug,
  but confirm the call still reads correctly (right threshold, not accidentally the enter one).
- [ ] Confirm `m_lastObservedKurtosis.store(...)` and the `isfinite(kurtosis)` early-return guard
  above these calls are unchanged — this task only touches the enter/exit condition, not the
  surrounding bookkeeping.

- [ ] **Step 5: Commit**

```bash
git add include/ExecutionParams.h src/RiskManager.cpp
git commit -m "feat: crisis-hysteresis enters early on fast kurtosis, exit stays slow-clock-only"
```

---

## Task 12: Early-trigger integration — sigmoid fragility penalty (`Scoring.cpp:223-226`)

**Files:**
- Modify: `src/Scoring.cpp:223-226`

- [ ] **Step 1: Confirm the enclosing function name**

Run: `grep -n -B15 "1.3248f" src/Scoring.cpp | head -20`

The research pass captured the guard/sigmoid body but not the containing function's declaration line — confirm it before editing, note it in the commit message.

- [ ] **Step 2: Call the extracted function**

```cpp
// src/Scoring.cpp:223-226, modified
constexpr float kFastFragilityGuard = 1.3248f; // NON-FINAL placeholder, shares the slow guard's
                                                 // value until Task 15's real calibration exists
if (ShouldApplyFragilityPenalty(ctx.talebKurtosis, 1.3248f, ctx.fastTalebKurtosis, kFastFragilityGuard)) {
    const double fragilityPenalty = 1.0 / (1.0 + std::exp(9.7409 * (static_cast<double>(ctx.talebKurtosis) - 1.6414)));
    multiplier *= fragilityPenalty;
}
```

Add `#include "KurtosisGateLogic.h"` to `Scoring.cpp`'s includes. Note: the sigmoid's own midpoint
(`1.6414`) deliberately still reads `ctx.talebKurtosis` (the slow value) directly — only the
*guard* (now `ShouldApplyFragilityPenalty`) decides whether to apply the penalty at all; the
penalty's *magnitude* calculation is untouched by this task.

- [ ] **Step 3: Full build verification**

Run: `./build_dll.sh --no-clean`

- [ ] **Step 4: Manual verification checklist**

- [ ] Confirm the sigmoid's own formula (`9.7409`, `1.6414`) is byte-for-byte unchanged — this task only replaced the guard condition, not the penalty magnitude calculation.
- [ ] Confirm the first argument to `ShouldApplyFragilityPenalty` is `1.3248f` (the existing slow guard), not accidentally `kFastFragilityGuard` in the wrong position.

- [ ] **Step 5: Commit**

```bash
git add src/Scoring.cpp
git commit -m "feat: fragility-penalty guard triggers early on fast kurtosis, magnitude stays slow-anchored"
```

---

## Task 13: Early-trigger integration — crash chase/exit gating (`PositionManager.cpp`, automatic + manual paths)

**Files:**
- Modify: `src/PositionManager.cpp:2313-2321`, `:2351-2358` (automatic path), `:2764-2771`, `:2785-2792` (manual path — mirrors automatic exactly)

- [ ] **Step 1: Confirm the enclosing function/context at each of the 4 sites**

Run: `grep -n -B10 "MaximumChaseAsPrice = 0.0\|crashRegime = " src/PositionManager.cpp`

Confirm each site's real enclosing function name and whether the automatic/manual paths are literally the same function called twice or two separately-named functions (research found them "mirrored," not confirmed identical) — note the finding in the commit message. Also confirm the manual path's exact local variable names (`lrcChase`/`lrc` equivalents) before editing — don't assume they're identical to the automatic path's names.

- [ ] **Step 2: Call the extracted functions at all 4 sites**

```cpp
// Automatic path, :2313-2321 (chase cap) — modified, calls ShouldCapChase (no Amihud term)
const float chaseKurtosis = lrcChase.isValid ? lrcChase.talebKurtosis : 1.23f;
const float fastChaseKurtosis = lrcChase.isValid ? lrcChase.fastTalebKurtosis : 1.23f;
if (ShouldCapChase(chaseDof, 4.0f, chaseKurtosis, fastChaseKurtosis, 1.8530f)) {
    order.MaximumChaseAsPrice = 0.0;
}
```

```cpp
// Automatic path, :2351-2358 (crash-regime stop-type) — modified, calls IsCrashRegime (has the Amihud term)
const float kurtosis = lrc.isValid ? lrc.talebKurtosis : 1.23f;
const float fastKurtosis = lrc.isValid ? lrc.fastTalebKurtosis : 1.23f;
const bool crashRegime = IsCrashRegime(dof, 4.0f, kurtosis, fastKurtosis, 1.8530f,
                                        lrc.isValid ? lrc.amihudPercentile : 0.0f, 0.90f);
```

Add `#include "KurtosisGateLogic.h"` to `PositionManager.cpp`'s includes. Apply the identical two
edits to the manual path (`:2764-2771`, `:2785-2792`), using that path's own local variable names
confirmed in Step 1. **Both mirrored sites call the same two functions** — this is the DRY
improvement this extraction gives beyond testability: one shared implementation instead of four
independently-maintained inline copies.

- [ ] **Step 3: Full build verification**

Run: `./build_dll.sh --no-clean`

- [ ] **Step 4: Manual verification checklist**

- [ ] Confirm all 4 sites (2 automatic + 2 manual) received the edit.
- [ ] Confirm the chase-cap sites call `ShouldCapChase` (no Amihud argument) and the crash-regime/stop-type sites call `IsCrashRegime` (with the Amihud argument) — these are genuinely different functions, don't swap them.
- [ ] Confirm each site's `isValid ? ... : 1.23f` / `isValid ? ... : 0.0f` fallback pattern matches the existing pattern already used for the slow value at that same site.

- [ ] **Step 5: Commit**

```bash
git add src/PositionManager.cpp
git commit -m "feat: crash-regime chase/exit gating early-triggers on fast kurtosis, both auto and manual paths"
```

---

## Task 14: Early-trigger integration — tail-risk-premium composite (`TradeDecisionEngine.h:299-309`)

**Files:**
- Modify: `include/TradeDecisionEngine.h:211` (struct field), `:299-309` (composite)

- [ ] **Step 1: Confirm the exact struct/function names**

Run: `grep -n -B10 "tailRiskPremium" include/TradeDecisionEngine.h | head -25`

- [ ] **Step 2: Add the field and call `ComputeTailRiskPremium`**

```cpp
// include/TradeDecisionEngine.h:211, alongside the existing field
float talebKurtosis = 0.0f;
float fastTalebKurtosis = 0.0f; // activity-clock twin, additive input to the composite below
```

```cpp
// include/TradeDecisionEngine.h:299-309, modified
r.tailRiskPremium = ComputeTailRiskPremium(
    hillPenalty,
    static_cast<double>(in.talebKurtosis), static_cast<double>(in.fastTalebKurtosis),
    /*kurtosisPenaltyLow=*/1.6414, /*kurtosisPenaltyHigh=*/2.0064,
    static_cast<double>(in.mahalanobis),
    /*mahalPenaltyLow=*/4.0, /*mahalPenaltyHigh=*/8.0);
```

Add `#include "KurtosisGateLogic.h"` to `TradeDecisionEngine.h`'s includes. Confirm the original
`mahalPenalty` clamp's actual bounds (`(mahalanobis - 4.0) / 4.0` implies a high bound of `8.0`,
not stated explicitly in the original inline form — verify this via `grep -n -B10 "mahalPenalty"
include/TradeDecisionEngine.h` before finalizing the call, don't assume the `8.0` above is right).

- [ ] **Step 3: Full build verification**

Run: `./build_dll.sh --no-clean`

- [ ] **Step 4: Manual verification checklist**

- [ ] Confirm the `kurtosisPenaltyLow`/`High` and `mahalPenaltyLow`/`High` arguments match the
  original inline clamp bounds exactly (Step 2's verification) — `ComputeTailRiskPremium` takes
  these as parameters rather than hardcoding them, so a transposed argument here would silently
  change the formula.
- [ ] Confirm the struct field addition (`fastTalebKurtosis`) doesn't collide with any existing field name or shift any positional initializer elsewhere in this struct's usage.

- [ ] **Step 5: Run the full native test suite green, one more time**

Run:
```bash
g++ -std=c++17 -I include tests/cpp/test_imbalance_bar_engine.cpp -o /tmp/ibe_test && /tmp/ibe_test
g++ -std=c++17 -I include tests/cpp/test_kurtosis_gate_logic.cpp -o /tmp/kgl_test && /tmp/kgl_test
```
Expected: `ALL PASS` on both — confirms nothing in Tasks 6-14's edits accidentally touched either pure header.

- [ ] **Step 6: Commit**

```bash
git add include/TradeDecisionEngine.h
git commit -m "feat: tail-risk-premium composite includes fastTalebKurtosis as a fourth max-penalty input"
```

---

## Task 15: `lbrnet` handoff document (not executed here) + explicit empirical follow-on list

**Files:**
- Create: `docs/superpowers/specs/2026-08-26-activity-clock-lbrnet-handoff.md` (MindfulTrader repo — the handoff document itself, matching this project's established pattern of writing a clear, actionable handoff for a separate `lbrnet`-rooted session, per `2026-08-16-predator-infrastructure-and-turtle-soup.md`'s own precedent — "create a clear, actionable handoff... for a separate lbrnet-rooted session").

**This task does not touch `lbrnet` code** — per this project's "lbrnet: separate repo/session, don't mix scope" convention, the actual Python-side work happens in a future `lbrnet`-rooted session.

- [ ] **Step 1: Write the handoff document**

```markdown
# Handoff: Activity-Clock Dual-Kurtosis — lbrnet-Side Work

Written from MindfulTrader, 2026-08-26. C++ side implemented per
docs/superpowers/plans/2026-08-26-activity-clock-dual-kurtosis.md (this plan). The following is
lbrnet-rooted work, not yet started, not executed from this repo.

## 1. HMM_OBSERVATION_EXTENSIONS update

`schema/regenerate_schema.sh`'s `HMM_OBSERVATION_EXTENSIONS = ("nh_nl_daily", "daily_bias")`
needs `fast_taleb_kurtosis` added (and `slow_taleb_kurtosis` if Task 6 of the C++ plan determined
a flat passthrough was needed for the existing slow value — check that plan's Task 6 Step 1 outcome
before starting this). This changes `HMM_OBSERVATION_DIM`.

## 2. Whichever HMM-consumer code reads HMM_OBSERVATION_FIELDS/HMM_OBSERVATION_DIM

Check `live_agent.py` (HMM inference) and `regime_view.py` (index lookups) for anywhere the
dimension count or field list is hardcoded rather than derived from the schema constants — this
project's own history includes exactly this class of bug (`lempel_ziv`, `dim3` rail-hit-rate
mismatches) from a live/train dimension-count drift.

## 3. Historical backfill

The activity-clock kurtosis dimension needs values for the FULL historical training dataset, not
just live-forward from whenever this ships. Decide: replay the new C++ ActivityClockManager logic
against historical tick data (requires a C++ replay harness reading raw .scid ticks), or reimplement
the same imbalance-bar-construction + Moors-kurtosis logic in Python against the same historical
tick data lbrnet already has access to. Either way, the two implementations (C++ live, whichever
backfill approach) must produce matching output on the same historical window before this dimension
is trusted for training — same twin-parity discipline as every other C++/Python surface in this
system.

## 4. EWMA threshold calibration (no invented constants)

`ActivityClockManager`'s imbalance-bar threshold currently ships with a NON-FINAL placeholder
(50.0, C++ plan Task 3). Needs deriving from real historical ES tick data — expected bar duration
and expected |imbalance| magnitude, following AFML's own EWMA-based adaptive-threshold recipe.
This is naturally lbrnet-side work (historical data analysis), even though the resulting threshold
gets shipped back into MindfulTrader's C++ config (`config/execution_params.json`, per this
project's existing config-sharing convention from the 2026-08-15 risk-gate-context work).

## 5. Empirical validation — does the activity-clock kurtosis actually discriminate better?

Per the spec's open question 3 — not assumed from the design's elegance alone. Needs a real
backtest comparing HMM cross-state discrimination with vs. without the new dimension(s).

## 6. Open question 13 — gate authority backtest

Whether the existing calibrated gates or freshly-recalibrated activity-clock-based gates should be
authoritative for the five existing risk gates (not the early-trigger-only integration already
shipped in the C++ plan — a full replacement question) is explicitly an empirical backtesting
question: compare realized drawdown and alpha outcomes of (a) current calibrated gates + early
trigger (already shipped), (b) gates fully recalibrated against the activity-clock statistic. Do
not decide this by which option avoids recalibration effort — that reasoning was explicitly
rejected during this initiative's design phase (see the spec's §4 item 4 correction, 2026-08-26).

## 7. The skewed Student-t emission distribution question

Flagged, not decided, in the spec's open question 12 (Hansen 1994; Fernández & Steel 1998) — whether
this system's HMM should use a skewed-t emission per state instead of symmetric. Genuinely separate
from everything above; don't conflate.
```

- [ ] **Step 2: Commit**

```bash
git add docs/superpowers/specs/2026-08-26-activity-clock-lbrnet-handoff.md
git commit -m "docs: write lbrnet-side handoff for activity-clock dual-kurtosis follow-on work"
```

---

## Plan Self-Review Notes

- **Spec coverage**: §1 (founding discovery) → Task 5's justification; §4 items 1-3 (ActivityClockManager core) → Tasks 1-4; §4 item 4 (dual-clock kurtosis + gate integration, corrected) → Tasks 5, 8-14; §7 (feasibility) → already resolved, cited as the basis for Task 2's design; §8 open questions 2/3/4/13 → Task 15's handoff, explicitly not blocking. §6/§8's deferred items (long-memory family, `PredictionAgeUs`, `skewness_idx`/`correction_action`/`fisher_info`/`burstiness_index`, skewed-t emission) are explicitly out of scope, not silently dropped.
- **Placeholder scan**: every numeric threshold introduced (imbalance threshold, fast-gate thresholds) is explicitly marked NON-FINAL with a comment pointing to Task 15 — this is intentional scaffolding to make the code buildable/testable before real calibration exists, not an unflagged placeholder.
- **Cross-repo boundary**: Task 6 is the one task that touches `schema/` (a separate repo) — kept as its own task with its own commit, following that repo's `PENDING_SCHEMA_CHANGES.md` process rather than treated as a MindfulTrader-internal edit.
- **Type consistency check**: `LocalRiskContext.fastTalebKurtosis` (Task 5) is the single new field every later task (8-13) reads by that exact name — verified consistent across all six integration tasks.
