# Phase I Hardening: Event Velocity, Entropy Stationarity, Weekend Freshness Gate, Hot-Path Copy — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Fix four independently-verified correctness/robustness issues in `ContextManager`'s core physics engines and event-collection gating, proposed as "Phase I" in `lbrnet/logs/rc_gemini.log` `GEMINI_BRIEF_082` (consolidating the earlier `GEMINI_REVIEW_078`/`081` audits) and independently confirmed against the actual source before this plan was written.

**Architecture:** Three of the four fixes follow this codebase's established pattern: extract or use an already-pure, header-only engine, fix it there with a failing test first, then delegate from the live call site. The fourth (weekend-aware freshness gate) needs a small amount of new ACSIL-dependent glue (day-of-week detection), following the pattern already established in `EventDataCollectorStudy.cpp`'s existing "Market-Closed Gate."

**Tech Stack:** C++17, hand-rolled assertion harnesses (no test framework), Sierra Chart ACSIL SDK (`sierra_chart_dependencies/`, this project's pinned vendor copy — the authoritative reference for any ACSIL signature).

## Global Constraints

- No heap allocations in recurring ACSIL update paths (CLAUDE.md Performance Rules) — Task 1's fix is specifically *removing* a per-tick-bounded-but-still-present computation pattern; Task 4's fix specifically *eliminates* a possible reallocation. Neither task may introduce a new allocation.
- Before removing any symbol, search the full repo for usages (CLAUDE.md Code Safety Rules) — Task 1 in particular touches a member (`m_eventTimestampsUS`) with a second, independent consumer (`CalculateBurstinessIndex`) that must keep working unchanged.
- Build via `./build_dll.sh` only — never raw `cmake`/`ninja`.
- TDD: write the failing test before the production change (CLAUDE.md Guardrail 4).
- No schema/wire changes — this touches only internal C++ computation, no `.fbs` fields.

## Design decisions made during scoping (read before starting)

1. **Task 1 does NOT remove `m_eventTimestampsUS`.** The original roadmap item ("transition from the capped deque to an EMA") reads as a full replacement, but `CalculateBurstinessIndex()` (`ContextManager.cpp:1429`, feeding `raschkeBurst`) independently reads the *same* deque for its own CV-of-inter-arrival-times computation (a different, legitimate metric — see `docs/ADR/burstiness_index_misnomer.md` for why this one is the *correct* classical-burstiness implementation in this codebase). The deque and its existing cap/push/pop bookkeeping stay exactly as they are; only the **velocity return value's own computation** changes from "count within a 2s window" to "EMA of inter-arrival time," using new, separate state.
2. **Task 2's fix lives entirely inside `InformationEngine.h`.** That class is already pure/header-only (no Sierra Chart or `ContextManager` dependency) and already has no standalone unit test — this plan adds one (`tests/cpp/test_information_engine.cpp`), matching the established `VolumeProfileEngine.h`/`DailyBiasEngine.h`/`TripleBarrierEngine.h` pattern. The fix keeps the existing bin thresholds unchanged (they were always intended as sigma multiples, per the code's own comment) and only replaces the fixed, hardcoded "assumed sigma ≈ 0.0002" scaling with a live rolling estimate.
3. **Task 3 threads a new parameter through `ContextManager::CheckAndTriggerHMM()` and its 4 callers**, rather than doing day-of-week arithmetic on the raw `now_us` epoch value inside `ContextManager` itself. `CheckAndTriggerHMM` never receives `SCStudyInterfaceRef sc`, so it has no way to determine day-of-week/session-time on its own; correctly handling ET/DST without a timezone library inside pure C++ is a real risk this plan avoids by reusing the *already-established*, ACSIL-native `sc.BaseDateTimeIn[...].GetDayOfWeek()` + `sc.StartTime1`/`EndTime1` pattern from `EventDataCollectorStudy.cpp`'s existing Market-Closed Gate (confirmed: Sierra Chart DOW convention is `1=Sunday, 2=Monday, ..., 6=Friday, 7=Saturday`). All 4 callers (`EventDataCollectorStudy.cpp:491`, `SCStudies.cpp:429`, `BackTesterStudy.cpp:896,916`) already have `sc` in scope. The day-of-week/session-time detection itself is extracted into one shared helper (`StudyHelperFunctions.h`/`.cpp`) to avoid duplicating it 4 times.
4. **Task 3's actual freshness-check math IS extracted into a small pure, testable helper**, even though the day-of-week detection around it isn't. The "is this fresh, given now/last-write/max-age/grace-bypass" arithmetic has no Sierra Chart dependency at all and is exactly the kind of logic this codebase's TDD convention expects to be tested directly.
5. **Task 4 needs no new test.** It's a one-line, semantically-narrowing change (`operator=` → `.assign()`, both already covered by the class's existing behavior contract) with no existing test file for `TailRiskEngine.h` to extend. Verified by build + the class's existing (informal) usage in `ContextManager.cpp`.
6. **`GetTs1MacroAgeUs`-style diagnostic getters are left alone.** Only the pass/fail *comparison* changes; the raw age-reporting used in log messages (`ContextManager.cpp:1113`) stays truthful (still reports real elapsed wall-clock time), so operators can still see the true weekend-gap duration in logs even while the gate itself no longer blocks on it.

---

### Task 1: EMA-based event velocity (uncap the 50 events/sec ceiling)

**Files:**
- Create: `include/EventVelocityEngine.h`
- Create: `tests/cpp/test_event_velocity_engine.cpp`
- Modify: `include/ContextManager.h` (new member, unchanged `m_eventTimestampsUS`)
- Modify: `src/ContextManager.cpp` (`CalculateEventVelocity`, `Reset()`)

**Interfaces:**
- Produces: `namespace eve { struct VelocityState { uint64_t lastEventUs = 0; double emaIntervalUs = 0.0; }; float UpdateAndGetVelocity(VelocityState& state, uint64_t nowUs, double tauUs); }` — consumed by `ContextManager::CalculateEventVelocity`.

- [ ] **Step 1: Write the failing test**

Create `tests/cpp/test_event_velocity_engine.cpp`:

```cpp
// test_event_velocity_engine.cpp — unit tests for the EMA-based, uncapped event
// velocity estimator (docs/superpowers/plans/2026-08-04-phase1-hardening.md Task 1;
// lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1).
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I include tests/cpp/test_event_velocity_engine.cpp -o /tmp/eve_test && /tmp/eve_test

#include "EventVelocityEngine.h"

#include <cmath>
#include <cstdio>

using namespace eve;

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

bool approx(float a, float b, float relTol = 0.05f) {
    return std::fabs(a - b) <= relTol * std::max(1.0f, std::fabs(b));
}

}  // namespace

int main() {
    std::printf("EventVelocityEngine unit tests\n");

    // First-ever event: no prior interval to measure, velocity is 0.
    {
        VelocityState state;
        const float v = UpdateAndGetVelocity(state, /*nowUs=*/1'000'000, /*tauUs=*/2'000'000.0);
        check("first_event_returns_zero_velocity", v == 0.0f);
    }

    // Steady 100/sec arrival rate (10ms apart) should converge to ~100 events/sec,
    // uncapped -- this is the core fix: the old deque-count formula could never
    // exceed EVENT_VELOCITY_MAX(100)/EVENT_VELOCITY_WINDOW_SEC(2) = 50.0.
    {
        VelocityState state;
        uint64_t t = 0;
        float v = 0.0f;
        for (int i = 0; i < 500; ++i) {
            t += 10'000;  // 10ms = 100 events/sec
            v = UpdateAndGetVelocity(state, t, /*tauUs=*/2'000'000.0);
        }
        check("converges_to_100_events_per_sec_uncapped", approx(v, 100.0f));
        check("genuinely_exceeds_the_old_50_cap", v > 50.0f);
    }

    // Steady 1000/sec (news-spike rate, 1ms apart) -- must not be capped either.
    {
        VelocityState state;
        uint64_t t = 0;
        float v = 0.0f;
        for (int i = 0; i < 2000; ++i) {
            t += 1'000;  // 1ms = 1000 events/sec
            v = UpdateAndGetVelocity(state, t, /*tauUs=*/2'000'000.0);
        }
        check("converges_to_1000_events_per_sec_during_a_news_spike", approx(v, 1000.0f, 0.1f));
    }

    // Non-monotonic timestamp (replay seek/duplicate tick): must not crash or
    // produce a negative/garbage value; should hold the prior estimate steady.
    {
        VelocityState state;
        UpdateAndGetVelocity(state, 1'000'000, 2'000'000.0);
        const float vBefore = UpdateAndGetVelocity(state, 1'010'000, 2'000'000.0);
        const float vAfter = UpdateAndGetVelocity(state, 1'005'000, 2'000'000.0);  // time went backwards
        check("non_monotonic_timestamp_holds_prior_estimate", vAfter == vBefore);
    }

    // Quiet market (10 seconds between events) -- velocity should be very low,
    // not zero (there IS a real, if slow, arrival rate) and not NaN/inf.
    // Note: the first call's nowUs must be non-zero -- 0 is this engine's
    // "uninitialized" sentinel (the same 0-means-never-written convention
    // ContextManager::AreTs1DimsReady already uses for m_ts1MacroLastWriteUs),
    // so a first call with nowUs=0 would be indistinguishable from cold-start
    // and short-circuit before any interval is measured.
    {
        VelocityState state;
        UpdateAndGetVelocity(state, 1'000'000, 2'000'000.0);
        const float v = UpdateAndGetVelocity(state, 11'000'000, 2'000'000.0);
        check("quiet_market_gives_low_finite_velocity", std::isfinite(v) && v > 0.0f && v < 1.0f);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails (header doesn't exist yet)**

Run: `g++ -std=c++17 -I include tests/cpp/test_event_velocity_engine.cpp -o /tmp/eve_test`
Expected: FAIL to compile with `EventVelocityEngine.h: No such file or directory`

- [ ] **Step 3: Write the minimal implementation**

Create `include/EventVelocityEngine.h`:

```cpp
// EventVelocityEngine.h — pure, header-only EMA-based event-arrival-rate
// estimator (docs/superpowers/plans/2026-08-04-phase1-hardening.md Task 1;
// lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1).
//
// SCOPE: replaces a windowed-count velocity formula that was mathematically
// capped at (deque capacity / window seconds) regardless of true event rate --
// during high-intensity events (CPI, FOMC, US open) ES tick rates can exceed
// 500-1000/sec, but the old formula could never report above 50.0. This
// computes an exponential moving average of inter-arrival TIME instead of
// counting arrivals in a fixed window, giving an O(1), uncapped, continuously
// reactive velocity with constant memory and no loop overhead. No Sierra Chart
// types, natively unit-testable (tests/cpp/test_event_velocity_engine.cpp).
//
// tauUs is the EMA's time constant, in microseconds -- pass the same window
// the old formula used (2 seconds) to preserve its intended responsiveness
// while removing its cap.

#pragma once

#include <cmath>
#include <cstdint>

namespace eve {

struct VelocityState {
    uint64_t lastEventUs = 0;
    double emaIntervalUs = 0.0;
};

inline float UpdateAndGetVelocity(VelocityState& state, uint64_t nowUs, double tauUs) {
    if (state.lastEventUs == 0) {
        state.lastEventUs = nowUs;
        return 0.0f;  // first event ever: no interval to measure yet
    }

    const double dtUs = static_cast<double>(nowUs) - static_cast<double>(state.lastEventUs);
    state.lastEventUs = nowUs;

    if (dtUs <= 0.0) {
        // Non-monotonic timestamp (replay seek/duplicate tick) -- hold the
        // prior estimate rather than corrupt it with a negative/zero interval.
        return state.emaIntervalUs > 0.0 ? static_cast<float>(1'000'000.0 / state.emaIntervalUs) : 0.0f;
    }

    if (state.emaIntervalUs <= 0.0) {
        state.emaIntervalUs = dtUs;  // seed on the first real interval
    } else {
        const double alpha = 1.0 - std::exp(-dtUs / tauUs);
        state.emaIntervalUs = alpha * dtUs + (1.0 - alpha) * state.emaIntervalUs;
    }

    return static_cast<float>(1'000'000.0 / state.emaIntervalUs);
}

}  // namespace eve
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -I include tests/cpp/test_event_velocity_engine.cpp -o /tmp/eve_test && /tmp/eve_test`
Expected: `ALL PASS (0 failures)`

- [ ] **Step 5: Delegate from the live call site**

In `include/ContextManager.h`, add `#include "EventVelocityEngine.h"` near the top, and add a new private member near `m_eventTimestampsUS` (search for `std::deque<uint64_t> m_eventTimestampsUS;`):

```cpp
    std::deque<uint64_t> m_eventTimestampsUS;       // Close prices
    eve::VelocityState m_velocityState;              // EMA-based event velocity (Task 1, phase1-hardening plan)
```

(Keep whatever the existing line's trailing comment actually says — only add the new member below it, do not alter the existing declaration or its comment.)

In `src/ContextManager.cpp`, find `CalculateEventVelocity`:

```cpp
float ContextManager::CalculateEventVelocity(uint64_t now_us) {
    if (m_eventTimestampsUS.size() >= EVENT_VELOCITY_MAX) {
        m_eventTimestampsUS.pop_front();
    }
    m_eventTimestampsUS.push_back(now_us);

    if (m_eventTimestampsUS.empty()) {
        return 0.0f;
    }

    uint64_t window_size_us = static_cast<uint64_t>(EVENT_VELOCITY_WINDOW_SEC) * 1000000ULL;
    uint64_t window_start = (now_us > window_size_us) ? (now_us - window_size_us) : 0;

    size_t count = 0;
    for (auto it = m_eventTimestampsUS.rbegin(); it != m_eventTimestampsUS.rend(); ++it) {
        if (*it < window_start) break;
        ++count;
    }

    return static_cast<float>(count) / EVENT_VELOCITY_WINDOW_SEC;
}
```

Replace with:

```cpp
float ContextManager::CalculateEventVelocity(uint64_t now_us) {
    // Deque maintenance UNCHANGED: CalculateBurstinessIndex() (below) independently
    // reads this same m_eventTimestampsUS history for its own CV-of-inter-arrival-times
    // computation (raschkeBurst) -- do not remove or resize this buffer.
    if (m_eventTimestampsUS.size() >= EVENT_VELOCITY_MAX) {
        m_eventTimestampsUS.pop_front();
    }
    m_eventTimestampsUS.push_back(now_us);

    // Velocity itself: EMA of inter-arrival time (uncapped, O(1)), replacing the
    // old windowed-count formula, which was mathematically capped at
    // EVENT_VELOCITY_MAX / EVENT_VELOCITY_WINDOW_SEC = 50.0 events/sec regardless
    // of true event rate (docs/superpowers/plans/2026-08-04-phase1-hardening.md
    // Task 1; lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1).
    const double tauUs = static_cast<double>(EVENT_VELOCITY_WINDOW_SEC) * 1'000'000.0;
    return eve::UpdateAndGetVelocity(m_velocityState, now_us, tauUs);
}
```

- [ ] **Step 6: Reset the new state alongside the existing deque reset**

In `src/ContextManager.cpp`, find `m_eventTimestampsUS.clear();` inside `Reset()` (around line 1350) and add the new state's reset directly after it:

```cpp
    m_eventTimestampsUS.clear();
    m_velocityState = eve::VelocityState{};
```

- [ ] **Step 7: Rebuild and confirm no regressions**

Run: `./build_dll.sh --no-clean`
Expected: `Build Successful`. Confirm `CalculateBurstinessIndex()`'s behavior is unaffected — it reads the same `m_eventTimestampsUS` deque, which is populated identically to before.

- [ ] **Step 8: Commit**

```bash
git add include/EventVelocityEngine.h tests/cpp/test_event_velocity_engine.cpp include/ContextManager.h src/ContextManager.cpp
git commit -m "fix(context): replace capped event-velocity formula with an uncapped EMA"
```

---

### Task 2: Volatility-standardize Shannon entropy's bin boundaries

**Files:**
- Create: `tests/cpp/test_information_engine.cpp`
- Modify: `include/InformationEngine.h`

**Interfaces:**
- No new public interface — `InformationEngine::AddObservation`/`MapToBin`/`GetRecurrenceRate` keep their existing signatures; only their internal bin-mapping behavior changes.

- [ ] **Step 1: Write the failing test**

Create `tests/cpp/test_information_engine.cpp`:

```cpp
// test_information_engine.cpp — unit tests for InformationEngine's
// volatility-standardized bin mapping (docs/superpowers/plans/2026-08-04-phase1-hardening.md
// Task 2; lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1.2).
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I include tests/cpp/test_information_engine.cpp -o /tmp/ie_test && /tmp/ie_test

#include "InformationEngine.h"

#include <cstdio>

using namespace MindfulTrader;

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

}  // namespace

int main() {
    std::printf("InformationEngine unit tests\n");

    // Basic sanity: engine starts cold, GetRecurrenceRate/GetFisherInformation
    // don't crash and report the documented "not enough samples" defaults.
    {
        InformationEngine engine;
        check("cold_start_recurrence_rate_is_zero", engine.GetRecurrenceRate(0.0001) == 0.0);
        check("cold_start_fisher_info_is_zero", engine.GetFisherInformation() == 0.0);
    }

    // THE CORE FIX: the same absolute log-return magnitude must land in a
    // DIFFERENT bin depending on the current (rolling) volatility regime --
    // proving the bins are no longer keyed to a fixed, hardcoded assumed sigma.
    // We can't reach into private state directly, so we observe this through
    // GetRecurrenceRate(), which internally calls the same MapToBin() the
    // histograms use: feed a long run of LOW-volatility returns to build a
    // low-volatility baseline, then check that a MODERATE return now reads as
    // "rare" (low recurrence) -- under the OLD fixed-sigma logic this same
    // absolute magnitude would already be well inside the common bins for any
    // regime, low or high volatility alike.
    {
        InformationEngine engineLowVol;
        // Quiet regime: tiny returns, ~0.00001 (1bp) in magnitude, alternating sign.
        for (int i = 0; i < 400; ++i) {
            engineLowVol.AddObservation((i % 2 == 0) ? 0.00001 : -0.00001);
        }
        // A 0.0005 (50bp) return is 50x this regime's typical magnitude -- should
        // now be a RARE event (low recurrence) in a properly volatility-standardized
        // scheme, not just another "common" bin entry as the old fixed ~2bp-sigma
        // assumption would have made moderately-sized moves look like nothing unusual
        // relative to a HIGH-volatility regime's own typical range.
        const double recurrenceOfModerateMove = engineLowVol.GetRecurrenceRate(0.0005);
        check("moderate_move_is_rare_in_a_quiet_regime", recurrenceOfModerateMove < 0.05);
    }

    {
        InformationEngine engineHighVol;
        // Volatile regime: much larger returns, ~0.0005 (50bp) in magnitude, alternating sign.
        for (int i = 0; i < 400; ++i) {
            engineHighVol.AddObservation((i % 2 == 0) ? 0.0005 : -0.0005);
        }
        // The SAME 0.0005 (50bp) move that was rare in the quiet regime should now
        // be COMMON (high recurrence) in a regime where it's the typical magnitude --
        // this is the volatility-regime-independence the fix is meant to restore.
        const double recurrenceOfSameMoveInVolatileRegime = engineHighVol.GetRecurrenceRate(0.0005);
        check("same_absolute_move_is_common_in_a_volatile_regime", recurrenceOfSameMoveInVolatileRegime > 0.3);
    }

    // Reset() must clear the new rolling-volatility state too, not just the
    // existing histogram/buffer state -- otherwise a fresh engine after Reset()
    // would inherit a stale volatility estimate from before the reset.
    {
        InformationEngine engine;
        for (int i = 0; i < 400; ++i) {
            engine.AddObservation((i % 2 == 0) ? 0.0005 : -0.0005);  // build a high-vol estimate
        }
        engine.Reset();
        for (int i = 0; i < 400; ++i) {
            engine.AddObservation((i % 2 == 0) ? 0.00001 : -0.00001);  // now feed a quiet regime
        }
        const double recurrenceAfterReset = engine.GetRecurrenceRate(0.0005);
        check("reset_clears_stale_volatility_estimate", recurrenceAfterReset < 0.05);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -I include tests/cpp/test_information_engine.cpp -o /tmp/ie_test && /tmp/ie_test`
Expected: at least `moderate_move_is_rare_in_a_quiet_regime` and `same_absolute_move_is_common_in_a_volatile_regime` FAIL (the current fixed-assumed-sigma bins classify both regimes' moves through the same hardcoded thresholds, so a 50bp move gets the same bin regardless of the surrounding regime).

- [ ] **Step 3: Write the minimal implementation**

In `include/InformationEngine.h`, find the private member declarations (search for `size_t m_headIndexP;`) and add two new members near them:

```cpp
        size_t m_headIndexP;
        size_t m_headIndexQ;
        size_t m_headIndexLZ;
        size_t m_countP;
        size_t m_countQ;
        double m_emaAbsLogReturn = 0.0;   // rolling volatility estimate (Task 2, phase1-hardening plan)
```

Find `AddObservation` and add the volatility-EMA update at the top of its body:

```cpp
        void AddObservation(double logReturn) {
            // Rolling volatility estimate (EMA of |logReturn|), used by MapToBin() to
            // standardize returns into sigma-scores instead of comparing against a
            // fixed, hardcoded assumed sigma (docs/superpowers/plans/2026-08-04-phase1-hardening.md
            // Task 2; lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1.2). Alpha is tied to
            // WINDOW_SIZE_P (the same short-term regime window already used elsewhere in
            // this class) for a consistent, non-arbitrary responsiveness.
            const double absReturn = std::abs(logReturn);
            constexpr double kVolatilityEmaAlpha = 2.0 / (static_cast<double>(WINDOW_SIZE_P) + 1.0);
            if (m_emaAbsLogReturn <= 0.0) {
                m_emaAbsLogReturn = absReturn;
            } else {
                m_emaAbsLogReturn = kVolatilityEmaAlpha * absReturn + (1.0 - kVolatilityEmaAlpha) * m_emaAbsLogReturn;
            }

            // Update Long-Term Window (Q) - Baseline
            double oldValQ = m_bufferQ[m_headIndexQ];
```

(Everything from `// Update Long-Term Window (Q) - Baseline` onward is the existing body, unchanged — only the volatility-EMA block above it is new.)

Find `MapToBin` and replace its body:

```cpp
        size_t MapToBin(double val) const {
            // Volatility-standardize before binning: replaces the old fixed,
            // hardcoded "assumed sigma ~ 0.0002 (2bps) for 1-minute" approximation
            // with a live rolling estimate, so entropy reflects distribution
            // STRUCTURE (randomness vs. organization), not the current volatility
            // regime (docs/superpowers/plans/2026-08-04-phase1-hardening.md Task 2;
            // lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1.2). Bin thresholds are
            // the SAME sigma-multiple boundaries as before (this class's original
            // comment already documented them in sigma terms); only the value being
            // compared against them changed from a fixed assumption to a live estimate.
            // Bins:
            // 0: < -5 sigma
            // 1: -5 to -2
            // 2: -2 to -1
            // 3: -1 to -0.5
            // 4: -0.5 to 0
            // 5: 0 to 0.5
            // 6: 0.5 to 1
            // 7: 1 to 2
            // 8: 2 to 5
            // 9: > 5 sigma
            constexpr double kMinSigma = 1e-6;  // floor to avoid division blowup at cold-start/flat-market
            const double sigma = (m_emaAbsLogReturn > kMinSigma) ? m_emaAbsLogReturn : kMinSigma;
            const double sigmaScore = val / sigma;

            if (sigmaScore < -5.0) return 0;
            if (sigmaScore < -2.0) return 1;
            if (sigmaScore < -1.0) return 2;
            if (sigmaScore < -0.5) return 3;
            if (sigmaScore < 0.0)  return 4;
            if (sigmaScore < 0.5)  return 5;
            if (sigmaScore < 1.0)  return 6;
            if (sigmaScore < 2.0)  return 7;
            if (sigmaScore < 5.0)  return 8;
            return 9;
        }
```

Find `Reset()` and add the new member's reset:

```cpp
        void Reset() {
            m_bufferP.fill(0.0);
            m_bufferQ.fill(0.0);
            m_bufferLZ.fill(0.0);
            m_histogramP.fill(0.0);
            m_histogramQ.fill(0.0);
            m_headIndexP = 0;
            m_headIndexQ = 0;
            m_headIndexLZ = 0;
            m_countP = 0;
            m_countQ = 0;
            m_emaAbsLogReturn = 0.0;
        }
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -I include tests/cpp/test_information_engine.cpp -o /tmp/ie_test && /tmp/ie_test`
Expected: `ALL PASS (0 failures)`

- [ ] **Step 5: Rebuild the full project**

Run: `./build_dll.sh --no-clean`
Expected: `Build Successful` — `InformationEngine` is used unchanged by `ContextManager.cpp` (same public API), so no other file should need changes.

- [ ] **Step 6: Commit**

```bash
git add tests/cpp/test_information_engine.cpp include/InformationEngine.h
git commit -m "fix(information-engine): volatility-standardize Shannon entropy bin mapping"
```

---

### Task 3: Weekend-aware freshness gate for TS1/TS2 dims (fix the Sunday-open data-loss window)

**Files:**
- Create: `include/FreshnessGateEngine.h`
- Create: `tests/cpp/test_freshness_gate_engine.cpp`
- Modify: `include/StudyHelperFunctions.h`, `src/StudyHelperFunctions.cpp` (new shared ACSIL helper)
- Modify: `include/ContextManager.h`, `src/ContextManager.cpp` (`AreTs1DimsReady`, `AreTs2StructuralDimsReady`, `CheckAndTriggerHMM`)
- Modify: `src/EventDataCollectorStudy.cpp`, `src/SCStudies.cpp`, `src/BackTesterStudy.cpp` (the 4 call sites)

**Interfaces:**
- Produces: `namespace fge { bool IsFresh(uint64_t nowUs, uint64_t lastWriteUs, uint64_t maxAgeUs, bool bypassCheck); }` — consumed by `AreTs1DimsReady`/`AreTs2StructuralDimsReady`.
- Produces: `bool IsPostWeekendReopenGracePeriod(SCStudyInterfaceRef sc)` (in `StudyHelperFunctions.h`) — consumed by all 4 `CheckAndTriggerHMM` call sites.
- `ContextManager::CheckAndTriggerHMM`'s signature gains one new trailing parameter: `void CheckAndTriggerHMM(uint64_t now_us, bool isDataCollection, float syntheticVelocity = -1.0f, bool isPostWeekendReopenGrace = false);` — existing callers that don't pass it keep compiling and behaving identically (default `false`, i.e. today's strict behavior).

This task's day-of-week detection is real ACSIL glue and cannot be unit-tested the way the pure engine can — it's build-verified only, matching this codebase's established testing boundary (no `sc` mocking layer exists).

- [ ] **Step 1: Write the failing test for the pure freshness-comparison logic**

Create `tests/cpp/test_freshness_gate_engine.cpp`:

```cpp
// test_freshness_gate_engine.cpp — unit tests for the pure freshness/age
// comparison used by TS1/TS2 readiness gates (docs/superpowers/plans/2026-08-04-phase1-hardening.md
// Task 3; lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1.3).
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I include tests/cpp/test_freshness_gate_engine.cpp -o /tmp/fge_test && /tmp/fge_test

#include "FreshnessGateEngine.h"

#include <cstdio>

using namespace fge;

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

}  // namespace

int main() {
    std::printf("FreshnessGateEngine unit tests\n");

    constexpr uint64_t kSixHoursUs = 6ULL * 60ULL * 60ULL * 1'000'000ULL;
    constexpr uint64_t kFortyNineHoursUs = 49ULL * 60ULL * 60ULL * 1'000'000ULL;

    // Ordinary weekday: 1 hour stale, well within a 6h budget, no grace needed.
    check("fresh_within_budget_no_grace_needed",
          IsFresh(/*nowUs=*/10 * 3'600ULL * 1'000'000ULL,
                  /*lastWriteUs=*/9 * 3'600ULL * 1'000'000ULL,
                  kSixHoursUs, /*bypassCheck=*/false));

    // Ordinary weekday: genuinely stuck for 8 hours (real staleness, no weekend
    // involved) -- must still correctly BLOCK when bypassCheck is false.
    check("genuinely_stale_still_blocks_without_grace",
          !IsFresh(/*nowUs=*/10 * 3'600ULL * 1'000'000ULL,
                   /*lastWriteUs=*/2 * 3'600ULL * 1'000'000ULL,
                   kSixHoursUs, /*bypassCheck=*/false));

    // The Sunday-open scenario: 49 hours stale (Friday 17:00 -> Sunday 18:00),
    // which is what triggered the original bug. Without the grace bypass this
    // fails (matching today's real behavior); WITH it, it must pass.
    check("weekend_gap_blocks_without_grace_bypass",
          !IsFresh(kFortyNineHoursUs, 0, kSixHoursUs, /*bypassCheck=*/false));
    check("weekend_gap_passes_with_grace_bypass",
          IsFresh(kFortyNineHoursUs, 0, kSixHoursUs, /*bypassCheck=*/true));

    // Even with the grace bypass active, a lastWriteUs of exactly 0 (never
    // written at all, e.g. before startup warm-up) has nothing to do with the
    // weekend gap and is a different failure mode entirely -- IsFresh() itself
    // only compares the two timestamps given to it; the "never written" case is
    // handled by the CALLER's m_ts1SeenAfterReset check before IsFresh() is ever
    // reached, so this test just documents that IsFresh() alone doesn't special-case it.
    check("bypass_does_not_change_pure_age_arithmetic_when_ages_are_equal",
          IsFresh(1000, 1000, 0, /*bypassCheck=*/false) == IsFresh(1000, 1000, 0, /*bypassCheck=*/true));

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails (header doesn't exist yet)**

Run: `g++ -std=c++17 -I include tests/cpp/test_freshness_gate_engine.cpp -o /tmp/fge_test`
Expected: FAIL to compile with `FreshnessGateEngine.h: No such file or directory`

- [ ] **Step 3: Write the minimal implementation**

Create `include/FreshnessGateEngine.h`:

```cpp
// FreshnessGateEngine.h — pure, header-only freshness/age comparison for the
// TS1/TS2 readiness gates (docs/superpowers/plans/2026-08-04-phase1-hardening.md
// Task 3; lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1.3).
//
// SCOPE: the actual "is this timestamp too old" arithmetic, extracted so it's
// independently testable. Does NOT decide whether a weekend-grace bypass
// SHOULD apply -- that's real ACSIL day-of-week/session-time detection living
// in StudyHelperFunctions.h's IsPostWeekendReopenGracePeriod(), which cannot be
// unit-tested the same way (no Sierra Chart types allowed in this header). The
// caller (ContextManager::AreTs1DimsReady/AreTs2StructuralDimsReady) combines
// both: it calls IsFresh() with bypassCheck already resolved by the caller
// chain, so this file has no Sierra Chart dependency at all.
//
// When bypassCheck is true, the caller has already determined we are within
// the post-weekend-reopen grace window (e.g. Sunday, at or after session open,
// within the same number of hours as the readiness max-age budget) -- in that
// specific window, TS1/TS2's slowly-evolving regime dims (Hurst exponent,
// tail index, log-variance-ratio, recurrence rate, fractal dimension) are
// still a reasonable approximation of the current regime, since the market
// was CLOSED for the entire gap and could not have produced new information
// to be stale relative to. bypassCheck does not affect any other failure mode
// (never-written, non-finite dims, out-of-contract values) -- those are
// checked by the caller before IsFresh() is ever reached.

#pragma once

#include <cstdint>

namespace fge {

inline bool IsFresh(uint64_t nowUs, uint64_t lastWriteUs, uint64_t maxAgeUs, bool bypassCheck) {
    if (bypassCheck) {
        return true;
    }

    if (nowUs >= lastWriteUs) {
        return (nowUs - lastWriteUs) <= maxAgeUs;
    }

    // Defensive for replay seeks or chart timeline jumps.
    return (lastWriteUs - nowUs) <= maxAgeUs;
}

}  // namespace fge
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -I include tests/cpp/test_freshness_gate_engine.cpp -o /tmp/fge_test && /tmp/fge_test`
Expected: `ALL PASS (0 failures)`

- [ ] **Step 5: Add the shared ACSIL day-of-week/session-time helper**

In `include/StudyHelperFunctions.h`, add near the other free-function declarations:

```cpp
/// True if the current bar falls within the post-weekend-reopen grace window:
/// Sunday, at or after the session open time, within kWeekendGraceHours of it.
/// Reuses the same Sierra Chart day-of-week convention (1=Sunday, 2=Monday, ...,
/// 6=Friday, 7=Saturday) and session-time-derivation pattern already established
/// in EventDataCollectorStudy.cpp's Market-Closed Gate, so it stays correct
/// regardless of chart timezone (ET, CT, etc.) the same way that gate does.
/// See docs/superpowers/plans/2026-08-04-phase1-hardening.md Task 3.
bool IsPostWeekendReopenGracePeriod(SCStudyInterfaceRef sc);
```

In `src/StudyHelperFunctions.cpp`, add the implementation (near other similarly-scoped helper functions):

```cpp
bool IsPostWeekendReopenGracePeriod(SCStudyInterfaceRef sc)
{
    // Grace window matches LOCK_D_TS1_MAX_AGE_US's own 6-hour budget
    // (EventDataCollectorStudy.cpp) -- covers exactly the ~49-hour Friday-close
    // to Sunday-reopen gap this fix exists for, without masking a genuinely
    // stuck system on any other day.
    constexpr int kWeekendGraceHours = 6;

    const int barDOW = sc.BaseDateTimeIn[sc.Index].GetDayOfWeek();  // 1=Sunday, ..., 7=Saturday
    if (barDOW != 1) {
        return false;
    }

    int barHour = 0, barMinute = 0, barSecond = 0;
    sc.BaseDateTimeIn[sc.Index].GetTimeHMS(barHour, barMinute, barSecond);
    const int barTimeHHMM = barHour * 100 + barMinute;

    const int openHHMM = (sc.StartTime1 / 3600) * 100 + (sc.StartTime1 % 3600) / 60;
    const int graceEndHHMM = openHHMM + kWeekendGraceHours * 100;

    return barTimeHHMM >= openHHMM && barTimeHHMM < graceEndHHMM;
}
```

- [ ] **Step 6: Thread the grace flag through `CheckAndTriggerHMM` and the two readiness checks**

In `include/ContextManager.h`, find:

```cpp
    void CheckAndTriggerHMM(uint64_t now_us, bool isDataCollection, float syntheticVelocity = -1.0f);
```

Replace with:

```cpp
    void CheckAndTriggerHMM(uint64_t now_us, bool isDataCollection, float syntheticVelocity = -1.0f,
                             bool isPostWeekendReopenGrace = false);
```

Find:

```cpp
    bool AreTs1DimsReady(uint64_t now_us, uint64_t max_age_us) const;
```

Replace with:

```cpp
    bool AreTs1DimsReady(uint64_t now_us, uint64_t max_age_us, bool bypassCheck = false) const;
```

Find:

```cpp
    bool AreTs2StructuralDimsReady(uint64_t now_us, uint64_t max_age_us) const;
```

Replace with:

```cpp
    bool AreTs2StructuralDimsReady(uint64_t now_us, uint64_t max_age_us, bool bypassCheck = false) const;
```

Add `#include "FreshnessGateEngine.h"` near the top of `include/ContextManager.h`.

In `src/ContextManager.cpp`, find the tail of `AreTs1DimsReady` (the two-branch age comparison):

```cpp
    if (now_us >= lastWriteUs) {
        return (now_us - lastWriteUs) <= max_age_us;
    }

    // Defensive for replay seeks or chart timeline jumps.
    return (lastWriteUs - now_us) <= max_age_us;
}
```

Replace with:

```cpp
    return fge::IsFresh(now_us, lastWriteUs, max_age_us, bypassCheck);
}
```

and update the function signature line itself:

```cpp
bool ContextManager::AreTs1DimsReady(uint64_t now_us, uint64_t max_age_us, bool bypassCheck) const {
```

Apply the identical change to `AreTs2StructuralDimsReady` (same signature update, same tail replacement with `fge::IsFresh(now_us, lastWriteUs, max_age_us, bypassCheck)`).

Find `CheckAndTriggerHMM`'s signature and the two readiness-check call sites:

```cpp
void ContextManager::CheckAndTriggerHMM(uint64_t now_us, bool isDataCollection, float syntheticVelocity) {
```

Replace with:

```cpp
void ContextManager::CheckAndTriggerHMM(uint64_t now_us, bool isDataCollection, float syntheticVelocity,
                                         bool isPostWeekendReopenGrace) {
```

Find:

```cpp
    const bool ts1MacroReady = AreTs1DimsReady(now_us, kTs1MacroMaxAgeUs);
```

Replace with:

```cpp
    const bool ts1MacroReady = AreTs1DimsReady(now_us, kTs1MacroMaxAgeUs, isPostWeekendReopenGrace);
```

Find:

```cpp
    const bool ts2StructuralReady = AreTs2StructuralDimsReady(now_us, kTs2StructuralMaxAgeUs);
```

Replace with:

```cpp
    const bool ts2StructuralReady = AreTs2StructuralDimsReady(now_us, kTs2StructuralMaxAgeUs, isPostWeekendReopenGrace);
```

- [ ] **Step 7: Update all 4 call sites**

In `src/EventDataCollectorStudy.cpp`, find:

```cpp
            ContextManager::Instance().CheckAndTriggerHMM(now_us, true, syntheticVelocity);
```

Replace with:

```cpp
            ContextManager::Instance().CheckAndTriggerHMM(now_us, true, syntheticVelocity,
                                                            IsPostWeekendReopenGracePeriod(sc));
```

In `src/SCStudies.cpp`, find:

```cpp
        ContextManager::Instance().CheckAndTriggerHMM(now_us, false);
```

Replace with:

```cpp
        ContextManager::Instance().CheckAndTriggerHMM(now_us, false, -1.0f, IsPostWeekendReopenGracePeriod(sc));
```

In `src/BackTesterStudy.cpp`, find (there are two identical call sites, update both):

```cpp
            ContextManager::Instance().CheckAndTriggerHMM(GetReplaySafeNowUs(sc), false);
```

and

```cpp
        ContextManager::Instance().CheckAndTriggerHMM(GetReplaySafeNowUs(sc), false);
```

Replace both with their respective indentation preserved, e.g.:

```cpp
            ContextManager::Instance().CheckAndTriggerHMM(GetReplaySafeNowUs(sc), false, -1.0f,
                                                            IsPostWeekendReopenGracePeriod(sc));
```

```cpp
        ContextManager::Instance().CheckAndTriggerHMM(GetReplaySafeNowUs(sc), false, -1.0f,
                                                        IsPostWeekendReopenGracePeriod(sc));
```

- [ ] **Step 8: Rebuild**

Run: `./build_dll.sh --no-clean`
Expected: `Build Successful`. If `sc.BaseDateTimeIn[sc.Index].GetDayOfWeek()`, `GetTimeHMS`, or `sc.StartTime1` don't match what's used here, cross-check against `sierra_chart_dependencies/sierrachart.h`/`scdatetime.h` directly (this exact `GetDayOfWeek`/`GetTimeHMS`/`StartTime1` combination is already used identically in `EventDataCollectorStudy.cpp`'s existing Market-Closed Gate, so it should already be known-correct).

- [ ] **Step 9: Manual verification note**

This cannot be fully verified without a real Sierra Chart replay spanning a weekend. Add a note to `docs/PENDING_USER_ACTIONS.md` (if that file still exists) or equivalent: confirm via a Sunday-spanning historical replay that `.context`/live collection no longer shows a 4-hour Sunday-evening gap, and that the log no longer shows repeated `"TS1 macro dims unavailable/stale"` rejections specifically in the first 6 hours after Sunday's session open.

- [ ] **Step 10: Commit**

```bash
git add include/FreshnessGateEngine.h tests/cpp/test_freshness_gate_engine.cpp \
        include/StudyHelperFunctions.h src/StudyHelperFunctions.cpp \
        include/ContextManager.h src/ContextManager.cpp \
        src/EventDataCollectorStudy.cpp src/SCStudies.cpp src/BackTesterStudy.cpp
git commit -m "fix(context): weekend-aware freshness gate for TS1/TS2 readiness checks"
```

---

### Task 4: Harden `TailRiskEngine`'s hot-path buffer copy

**Files:**
- Modify: `include/TailRiskEngine.h`

**Interfaces:** None — internal implementation detail, no signature change.

- [ ] **Step 1: Make the change**

In `include/TailRiskEngine.h`, find (inside `GetHillAlpha()`):

```cpp
                m_sortBuffer = m_buffer; // Copy all
```

Replace with:

```cpp
                m_sortBuffer.assign(m_buffer.begin(), m_buffer.end()); // Copy all -- .assign() guarantees no
                                                                        // reallocation when capacity (reserved
                                                                        // in the constructor) is already
                                                                        // sufficient, unlike operator=, whose
                                                                        // reallocation behavior on unequal sizes
                                                                        // is implementation-defined (docs/superpowers/plans/
                                                                        // 2026-08-04-phase1-hardening.md Task 4;
                                                                        // lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1.4).
```

- [ ] **Step 2: Rebuild**

Run: `./build_dll.sh --no-clean`
Expected: `Build Successful`. No test exists for this class; verified by build plus the class's existing usage in `ContextManager.cpp` (`m_tailRiskEngine.GetHillAlpha()`).

- [ ] **Step 3: Commit**

```bash
git add include/TailRiskEngine.h
git commit -m "fix(tail-risk): use .assign() instead of operator= to guarantee no hot-path reallocation"
```
