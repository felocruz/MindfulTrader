# Observation-Vector Incremental Accumulators Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Convert three of the 16D observation vector's calculators — `CalculateLogVariance` (dim 0),
`CalculateBurstiness` (dim 1), `CalculateFisherInformation` (dim 8) — from O(window) full-rescan-every-call
to O(1)/O(1)-amortized incremental accumulators, with zero behavior change versus today's output
(same formulas, same adaptive windows, same warmup gates — only the *implementation* changes).

**Architecture:** Two new pure, header-only, natively-testable accumulator classes (matching the
established `FeatureScaler.h`/`OrderFlowAsymmetryEngine.h`/`CarryForwardCalculators.h` extraction
pattern): `SlidingWindowMomentAccumulator<MaxCapacity>` (circular prefix-sum buffer — O(1) "sum of last
K" for varying K) and `SlidingWindowExtremaTracker<MaxWindow>` (monotonic deque — O(1) amortized
sliding min/max). Both get one instance added to the existing per-study-instance
`AdaptiveCalculatorsState` (already persistent-pointer-backed, already wired into the existing
`sc.IsFullRecalculation`-triggered `ResetAdaptiveCalculators()` mechanism — no new reset logic needed).
Both classes derive their "same-bar, new tick" handling from ACSIL's actual array-mutability contract
(the current `sc.Index` slot is mutable until the bar closes; every lower index is permanently fixed) —
not from mimicking Sierra Chart's own closed-source implementation, which we can't see and, for the
one built-in we *can* see the source of (`GetHighest`/`GetLowest`, `sierrachart.h:115-156`), turns out
to just brute-force rescan every call rather than do anything incremental. Both classes also defend
against the same contract being violated in a way the existing `sc.IsFullRecalculation` reset doesn't
cover (position moving backward without an intervening reset) — see Tasks 2 and 3's Step 3.5. Task 7
adds a golden-file characterization test (the same standard `test_feature_scaler.cpp` already set for
its own DOD conversion) comparing the new accumulators' output against a brute-force reference over a
synthetic, adversarial (multi-tick-per-bar, varying window size) sequence, before Task 8's final build.

**Tech Stack:** C++17, header-only pure-logic extraction pattern, manual `g++`-compiled native test
binaries (no gtest in this repo), ACSIL persistent-pointer state.

## Related finding, root-caused and fixed separately (not part of this plan's scope)

While working through this plan's "does the calculator get called every tick or once per bar"
question (the same-position-replace design discussion above), a **different, unrelated bug** on dim 7
(`micro_asymmetry`) surfaced and was fixed in `90b17fc` before this plan's tasks were executed —
recorded here because a code comment in that fix (`StudyHelperFunctions.h`) points back to this
section.

**The bug:** `UpdateObservationVectorSubgraphs` gates its entire body to once per bar (first tick
only, via `lastObsUpdateIndex == sc.Index`). Dim 7's earlier fix (the `sc.BidVolume`/`sc.AskVolume`
one from `docs/superpowers/plans/2026-08-12-tick-native-toxicity-illiquidity.md`) was wired into that
gated function — but `sc.BidVolume`/`sc.AskVolume` accumulate *throughout* the still-forming bar, so
reading them at the bar's first tick meant near-zero volume on both sides almost every time, tripping
the carry-forward's `total <= 0` branch and pinning the value at `0.0` permanently (the carry-forward
slot starts at `0.0` and never got a chance to see a real value). Confirmed via a
`ContextManager::ObservationStaleness ALERT` grep of the live application log against the deployed
DLL: 51 alerts, always exactly `0.0`, max stale run 2,552 samples — in a build confirmed to already
include the fix, so this wasn't stale pre-fix data.

**The fix:** relocate the computation out of the once-per-bar-gated function and into the same
per-tick, ungated call site `VolumeIndicator::UpdateVolume()` already uses successfully
(`TripleScreen3.cpp`, right after its `sc.BidVolume`/`sc.AskVolume` read) — same arrays, same cadence,
no gate. This is orthogonal to *this* plan's O(1)-vs-O(window) performance question: dim 7's
`ofae::ComputeMicroAsymmetry` call was and remains O(1) either way — the bug was about *when* it ran,
not how expensive it was.

**Relevance to this plan:** this is a useful cross-check on the same "does this read a value that
accumulates within the still-forming bar" question the same-position-replace design already asked for
dims 0, 1, and 8 above — those three are called directly, per-tick, with no such gate, so they were
never at risk of this specific failure mode. Confirmed by inspection, not just inference: `CalculateLogVariance`
and `CalculateBurstiness` are called directly from `TripleScreen1.cpp`/`TripleScreen2.cpp`'s main
per-tick bodies, not through `UpdateObservationVectorSubgraphs`.

**A second instance of the same root cause, not yet fixed:** `CalculateAmihudIlliquidity` (dim 11) *is*
called through `UpdateObservationVectorSubgraphs`'s once-per-bar gate and *does* read
`sc.Volume[sc.Index]` (also accumulates within the bar) for its current-bar term — tracked as its own
item, `docs/PENDING_USER_ACTIONS.md` §6.

## Global Constraints

- No heap allocations in the hot path — CLAUDE.md Performance Rules. Both new classes are
  `std::array`-backed (via `RingBuffer<T,Capacity>`), zero heap allocation for their lifetime.
- Zero behavior change: every rewritten function must produce output matching the original formula,
  not a different (even if "better") statistic. This is a pure performance pass.
- **Kurtosis and skewness are explicitly excluded** — see
  `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`'s `pending-replacement` rows
  and its 2026-08-12 changelog entry. Do not extend this pattern to those two calculators.
- Sevcik fractal dimension, Hurst/DFA, and RQA recurrence rate are explicitly excluded (not
  incrementalizable with this technique — see the Gang spec and this plan's own design discussion).
- Build must go through `./build_dll.sh` — never raw `cmake`/`ninja`/`flatc`.

---

## File Structure

- **Create:** `include/SlidingWindowMomentAccumulator.h` — circular prefix-sum accumulator (sum,
  sum-of-squares) for varying-K sliding-window queries.
- **Create:** `tests/cpp/test_sliding_window_moment_accumulator.cpp`
- **Create:** `include/SlidingWindowExtremaTracker.h` — monotonic-deque sliding-window min/max.
- **Create:** `tests/cpp/test_sliding_window_extrema_tracker.cpp`
- **Modify:** `include/RingBuffer.h` — add `pop_back()` (needed by the monotonic deque; the existing
  `push_back`/`pop_front`-only API is a FIFO, not a full deque).
- **Modify:** `tests/cpp/test_ring_buffer.cpp` — add `pop_back()` coverage.
- **Modify:** `src/StudyHelperFunctions.cpp` — extend `AdaptiveCalculatorsState`; rewrite
  `CalculateLogVariance`, `CalculateBurstiness`, `CalculateFisherInformation`.

---

### Task 1: `RingBuffer::pop_back()`

**Files:**
- Modify: `include/RingBuffer.h`
- Modify: `tests/cpp/test_ring_buffer.cpp`

**Interfaces:**
- Produces: `RingBuffer<T,Capacity>::pop_back()` — used by Task 3's monotonic deque.

- [ ] **Step 1: Write the failing test**

Add to `tests/cpp/test_ring_buffer.cpp`, just before the final `pop_front_on_empty_is_noop` block:

```cpp
    // pop_back removes the newest element without disturbing the rest.
    {
        RingBuffer<float, 4> buf;
        buf.push_back(1.0f);
        buf.push_back(2.0f);
        buf.push_back(3.0f);
        buf.pop_back();
        check("pop_back_reduces_size", buf.size() == 2);
        check("pop_back_leaves_oldest_intact", buf[0] == 1.0f);
        check("pop_back_new_back_is_second_element", buf.back() == 2.0f);
    }

    // pop_back on an empty buffer is a no-op, not a crash.
    {
        RingBuffer<float, 4> buf;
        buf.pop_back();
        check("pop_back_on_empty_is_noop", buf.size() == 0);
    }

    // push_back after pop_back reuses the freed slot correctly (wrap-around check).
    {
        RingBuffer<float, 3> buf;
        buf.push_back(1.0f);
        buf.push_back(2.0f);
        buf.push_back(3.0f);
        buf.pop_back();      // remove 3.0f, size=2
        buf.push_back(4.0f); // re-fill the freed slot
        check("pop_back_then_push_back_size", buf.size() == 3);
        check("pop_back_then_push_back_order_0", buf[0] == 1.0f);
        check("pop_back_then_push_back_order_1", buf[1] == 2.0f);
        check("pop_back_then_push_back_order_2", buf[2] == 4.0f);
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -I include tests/cpp/test_ring_buffer.cpp -o /tmp/rb_test`
Expected: FAIL — `error: 'class RingBuffer<float, 4>' has no member named 'pop_back'`

- [ ] **Step 3: Write minimal implementation**

In `include/RingBuffer.h`, add immediately after `pop_front()` (currently lines 39-43):

```cpp
    void pop_back() {
        if (m_count == 0) return;
        --m_count;
    }
```

Also update the file's top-of-file API-surface comment (currently line 14) from:
```
// push_back/pop_front/back()/size()/empty()/clear()/
```
to:
```
// push_back/pop_front/pop_back/back()/size()/empty()/clear()/
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -I include tests/cpp/test_ring_buffer.cpp -o /tmp/rb_test && /tmp/rb_test`
Expected: `ALL PASS`, exit code 0.

- [ ] **Step 5: Commit**

```bash
git add include/RingBuffer.h tests/cpp/test_ring_buffer.cpp
git commit -m "feat(ring-buffer): add pop_back, making RingBuffer a full deque"
```

---

### Task 2: `SlidingWindowMomentAccumulator<MaxCapacity>`

**Files:**
- Create: `include/SlidingWindowMomentAccumulator.h`
- Test: `tests/cpp/test_sliding_window_moment_accumulator.cpp`

**Interfaces:**
- Produces: `SlidingWindowMomentAccumulator<N>::push(double value, int64_t position)`,
  `::availableCount() -> size_t`, `::sumLastK(size_t k) -> double`, `::sumSqLastK(size_t k) -> double`,
  `::reset()`. Used by Task 4 and Task 5.

- [ ] **Step 1: Write the failing test**

Create `tests/cpp/test_sliding_window_moment_accumulator.cpp`:

```cpp
// test_sliding_window_moment_accumulator.cpp — characterization tests for
// SlidingWindowMomentAccumulator's O(1) varying-K sliding sum/sum-of-squares,
// extracted so it can be natively unit-tested without Sierra Chart/ACSIL deps
// (docs/superpowers/plans/2026-08-12-observation-vector-incremental-accumulators.md).
//
// Build & run natively:
//   g++ -std=c++17 -I include tests/cpp/test_sliding_window_moment_accumulator.cpp -o /tmp/swma_test && /tmp/swma_test

#include "SlidingWindowMomentAccumulator.h"

#include <cmath>
#include <cstdio>
#include <vector>

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

bool approx(double a, double b, double tol = 1e-9) {
    return std::fabs(a - b) <= tol;
}

}  // namespace

int main() {
    std::printf("SlidingWindowMomentAccumulator unit tests\n");

    // Basic sum/sumSq match brute force for varying K.
    {
        SlidingWindowMomentAccumulator<5> acc;
        int64_t pos = 0;
        for (double v : {1.0, 2.0, 3.0, 4.0, 5.0}) {
            acc.push(v, pos++);
        }
        check("sum_last_3_matches_brute_force", approx(acc.sumLastK(3), 3.0 + 4.0 + 5.0));
        check("sum_last_5_matches_brute_force", approx(acc.sumLastK(5), 1.0 + 2.0 + 3.0 + 4.0 + 5.0));
        check("sumsq_last_3_matches_brute_force", approx(acc.sumSqLastK(3), 9.0 + 16.0 + 25.0));
        check("available_count_is_5", acc.availableCount() == 5);
    }

    // Eviction beyond MaxCapacity: only the last MaxCapacity values are retrievable.
    {
        SlidingWindowMomentAccumulator<3> acc;
        int64_t pos = 0;
        acc.push(100.0, pos++);  // will be evicted
        acc.push(1.0, pos++);
        acc.push(2.0, pos++);
        acc.push(3.0, pos++);
        check("available_count_caps_at_max_capacity", acc.availableCount() == 3);
        check("sum_last_3_excludes_evicted_value", approx(acc.sumLastK(3), 1.0 + 2.0 + 3.0));
    }

    // Varying K across calls (adaptive window), same accumulator instance.
    {
        SlidingWindowMomentAccumulator<10> acc;
        int64_t pos = 0;
        for (double v : {10.0, 20.0, 30.0, 40.0, 50.0}) {
            acc.push(v, pos++);
        }
        check("varying_k_query_1", approx(acc.sumLastK(1), 50.0));
        check("varying_k_query_2", approx(acc.sumLastK(2), 90.0));
        check("varying_k_query_5", approx(acc.sumLastK(5), 150.0));
    }

    // Reset clears state.
    {
        SlidingWindowMomentAccumulator<5> acc;
        acc.push(1.0, 0);
        acc.push(2.0, 1);
        acc.reset();
        check("reset_clears_available_count", acc.availableCount() == 0);
        acc.push(7.0, 2);
        check("reset_then_push_gives_correct_sum", approx(acc.sumLastK(1), 7.0));
    }

    // Critical: multiple pushes at the SAME position (multiple ticks within a
    // still-forming bar) must REPLACE, not accumulate. This is the bug this
    // plan's design discussion caught before any wiring code was written.
    {
        SlidingWindowMomentAccumulator<5> acc;
        acc.push(1.0, 0);
        acc.push(10.0, 1);   // bar 1, first tick: value 10
        acc.push(20.0, 1);   // bar 1, second tick: price moved, value now 20 -- REPLACES the 10
        acc.push(30.0, 1);   // bar 1, third tick: value now 30 -- REPLACES the 20
        check("same_position_replace_available_count_still_2",
              acc.availableCount() == 2);  // bar 0 and bar 1 -- not 4
        check("same_position_replace_sum_uses_latest_value",
              approx(acc.sumLastK(2), 1.0 + 30.0));
        check("same_position_replace_sumsq_uses_latest_value",
              approx(acc.sumSqLastK(2), 1.0 + 900.0));
        acc.push(2.0, 2);  // bar 2: genuinely new position, appends normally
        check("new_position_after_replay_appends", acc.availableCount() == 3);
        check("new_position_after_replace_sum", approx(acc.sumLastK(3), 1.0 + 30.0 + 2.0));
    }

    // Defensive: a position moving BACKWARD without an intervening reset()
    // is not something we've verified ACSIL can never do outside a full
    // recalculation (which already calls reset() before resuming). Rather
    // than silently corrupt the window on an unverified assumption, an
    // out-of-order push must trigger a safe self-reset and be counted.
    {
        SlidingWindowMomentAccumulator<5> acc;
        acc.push(1.0, 5);
        acc.push(2.0, 6);
        check("before_violation_available_count", acc.availableCount() == 2);
        acc.push(3.0, 3);  // position moved backward: 6 -> 3
        check("out_of_order_triggers_reset", acc.availableCount() == 1);
        check("out_of_order_reset_count_incremented", acc.outOfOrderResetCount() == 1);
        check("out_of_order_sum_reflects_post_reset_state", approx(acc.sumLastK(1), 3.0));
        acc.reset();  // legitimate reset() must not clear the diagnostic counter below
        check("legitimate_reset_does_not_clear_out_of_order_count", acc.outOfOrderResetCount() == 1);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -I include tests/cpp/test_sliding_window_moment_accumulator.cpp -o /tmp/swma_test`
Expected: FAIL — `fatal error: SlidingWindowMomentAccumulator.h: No such file or directory`

- [ ] **Step 3: Write minimal implementation**

Create `include/SlidingWindowMomentAccumulator.h`:

```cpp
// SlidingWindowMomentAccumulator.h — pure, header-only sliding-window sum /
// sum-of-squares accumulator supporting O(1) queries for "sum of the last K
// raw values" where K varies call-to-call (K <= MaxCapacity) -- e.g. this
// project's adaptive observation windows
// (docs/superpowers/plans/2026-08-12-observation-vector-incremental-accumulators.md).
//
// Technique: a circular buffer of MaxCapacity+1 CUMULATIVE sums (a
// "prefix-sum ring buffer"), seeded with a 0.0 baseline. sumLastK(k) =
// cum[newest] - cum[newest-k], one subtraction, no window rescan. Standard
// "sliding window sum for varying K" technique -- not novel, just not
// previously present in this codebase (the existing RollingWindowCalculator
// in StudyHelperFunctions.cpp only supports a single FIXED K baked in at
// compile time).
//
// Numerically safe for this project's realistic magnitudes/durations: log
// returns and squared bar ranges are O(1e-4)-O(10) per sample; even a decade
// of continuous 15-min bars (~2.6e5 samples) keeps cumulative sums many
// orders of magnitude below double precision's ~1e15-1e16-significant-digit
// range where catastrophic cancellation would start to matter. No periodic
// re-basing needed.
//
// Position-aware upsert: `position` should be a monotonically non-decreasing
// logical index (e.g. sc.Index). Sierra Chart's AutoLoop=1 convention calls
// the calculators this accumulator backs on EVERY TICK, not once per bar
// close -- multiple pushes at the same position (multiple ticks within a
// still-forming bar) REPLACE the previous value for that position rather
// than accumulating a second entry, matching the live/still-updating
// current-bar semantics of the original per-tick-rescan calculators. This
// is derived from ACSIL's actual array contract (current index mutable,
// every lower index permanently fixed once written), not from mimicking
// Sierra Chart's own internals -- the one built-in whose source we can see
// (GetHighest/GetLowest, sierrachart.h:115-156) isn't incremental at all, it
// just rescans, which is the inefficiency this class exists to avoid.
//
// Defensive: a position moving BACKWARD without an intervening reset() is
// not something we've independently verified ACSIL can never do outside a
// full recalculation (the only verified rewind trigger is
// ResetAdaptiveCalculators() on sc.IsFullRecalculation). push() self-resets
// and counts it via outOfOrderResetCount() rather than trusting that
// assumption silently.

#pragma once

#include "RingBuffer.h"
#include <algorithm>
#include <cstddef>
#include <cstdint>

template <size_t MaxCapacity>
class SlidingWindowMomentAccumulator {
public:
    SlidingWindowMomentAccumulator() {
        m_cumSum.push_back(0.0);
        m_cumSumSq.push_back(0.0);
    }

    // Push (or, if `position` matches the last push's position, replace) one
    // raw value. O(1). Defensive: if `position` moves BACKWARD relative to
    // the last push without an intervening reset(), that means some rewind
    // path exists that this accumulator wasn't told about via reset() (the
    // only verified rewind trigger is ResetAdaptiveCalculators() on
    // sc.IsFullRecalculation) -- fail safe by resetting rather than silently
    // corrupting the window, and count it so this is visible if it ever
    // actually fires.
    void push(double value, int64_t position) {
        if (m_hasLastPosition && position < m_lastPosition) {
            reset();
            ++m_outOfOrderResetCount;
        }

        if (m_hasLastPosition && position == m_lastPosition) {
            // Undo the previous push for this same position before redoing it.
            m_cumSum.pop_back();
            m_cumSumSq.pop_back();
            m_pushCount = (m_pushCount > 0) ? m_pushCount - 1 : 0;
        }

        const double newCumSum = m_cumSum.back() + value;
        const double newCumSumSq = m_cumSumSq.back() + value * value;
        if (m_cumSum.size() == MaxCapacity + 1) {
            m_cumSum.pop_front();
            m_cumSumSq.pop_front();
        }
        m_cumSum.push_back(newCumSum);
        m_cumSumSq.push_back(newCumSumSq);
        m_pushCount = std::min(m_pushCount + 1, MaxCapacity);
        m_lastPosition = position;
        m_hasLastPosition = true;
    }

    // Number of distinct positions (bars) currently retrievable via
    // sumLastK/sumSqLastK (<= MaxCapacity).
    size_t availableCount() const { return m_pushCount; }

    // Sum of the last k raw values pushed. Precondition: k <= availableCount().
    double sumLastK(size_t k) const {
        const size_t n = m_cumSum.size();
        return m_cumSum[n - 1] - m_cumSum[n - 1 - k];
    }

    // Sum of squares of the last k raw values pushed. Precondition: k <= availableCount().
    double sumSqLastK(size_t k) const {
        const size_t n = m_cumSumSq.size();
        return m_cumSumSq[n - 1] - m_cumSumSq[n - 1 - k];
    }

    // Data-clearing reset (also called defensively by push() above). Does
    // NOT clear outOfOrderResetCount() -- that counter tracks anomalies
    // across the accumulator's lifetime, including across legitimate
    // full-recalculation resets.
    void reset() {
        m_cumSum.clear();
        m_cumSumSq.clear();
        m_cumSum.push_back(0.0);
        m_cumSumSq.push_back(0.0);
        m_pushCount = 0;
        m_hasLastPosition = false;
    }

    // Number of times push() detected a backward-moving position and
    // self-reset. Should stay 0 in practice; exposed as a diagnostic so a
    // violation of the "ResetAdaptiveCalculators() is the only rewind path"
    // assumption is visible instead of silently absorbed.
    size_t outOfOrderResetCount() const { return m_outOfOrderResetCount; }

private:
    RingBuffer<double, MaxCapacity + 1> m_cumSum;
    RingBuffer<double, MaxCapacity + 1> m_cumSumSq;
    size_t m_pushCount = 0;
    int64_t m_lastPosition = 0;
    bool m_hasLastPosition = false;
    size_t m_outOfOrderResetCount = 0;
};
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -I include tests/cpp/test_sliding_window_moment_accumulator.cpp -o /tmp/swma_test && /tmp/swma_test`
Expected: `ALL PASS`, exit code 0, all checks including the same-position-replace group pass.

- [ ] **Step 5: Commit**

```bash
git add include/SlidingWindowMomentAccumulator.h tests/cpp/test_sliding_window_moment_accumulator.cpp
git commit -m "feat(dod): add SlidingWindowMomentAccumulator for O(1) varying-K sliding sums"
```

---

### Task 3: `SlidingWindowExtremaTracker<MaxWindow>`

**Files:**
- Create: `include/SlidingWindowExtremaTracker.h`
- Test: `tests/cpp/test_sliding_window_extrema_tracker.cpp`

**Interfaces:**
- Consumes: `RingBuffer<T,Capacity>::pop_back()` (Task 1).
- Produces: `SlidingWindowExtremaTracker<N>::push(float value, int64_t position, size_t windowSize)`,
  `::min() -> float`, `::max() -> float`, `::empty() -> bool`, `::reset()`. Used by Task 6.

- [ ] **Step 1: Write the failing test**

Create `tests/cpp/test_sliding_window_extrema_tracker.cpp`:

```cpp
// test_sliding_window_extrema_tracker.cpp — characterization tests for
// SlidingWindowExtremaTracker's O(1)-amortized sliding min/max via monotonic
// deque, extracted so it can be natively unit-tested without Sierra
// Chart/ACSIL deps
// (docs/superpowers/plans/2026-08-12-observation-vector-incremental-accumulators.md).
//
// Build & run natively:
//   g++ -std=c++17 -I include tests/cpp/test_sliding_window_extrema_tracker.cpp -o /tmp/swet_test && /tmp/swet_test

#include "SlidingWindowExtremaTracker.h"

#include <algorithm>
#include <cstdio>
#include <vector>

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
    std::printf("SlidingWindowExtremaTracker unit tests\n");

    // Basic sliding max/min over a fixed window, checked against brute force.
    {
        SlidingWindowExtremaTracker<5> tracker;
        std::vector<float> values = {3.0f, 1.0f, 4.0f, 1.0f, 5.0f, 9.0f, 2.0f, 6.0f};
        for (size_t i = 0; i < values.size(); ++i) {
            tracker.push(values[i], static_cast<int64_t>(i), 3);
            if (i >= 2) {
                const float bfMax = std::max({values[i - 2], values[i - 1], values[i]});
                const float bfMin = std::min({values[i - 2], values[i - 1], values[i]});
                char nameMax[64];
                char nameMin[64];
                std::snprintf(nameMax, sizeof(nameMax), "max_matches_brute_force_at_%zu", i);
                std::snprintf(nameMin, sizeof(nameMin), "min_matches_brute_force_at_%zu", i);
                check(nameMax, tracker.max() == bfMax);
                check(nameMin, tracker.min() == bfMin);
            }
        }
    }

    // Monotonically increasing sequence: worst case for deque growth. Max
    // should be the newest value, min should be the oldest value still in
    // the window.
    {
        SlidingWindowExtremaTracker<4> tracker;
        for (int64_t i = 0; i < 6; ++i) {
            tracker.push(static_cast<float>(i), i, 4);
        }
        // window=4 at position 5 covers positions 2,3,4,5 -> values 2,3,4,5
        check("monotonic_increasing_max_is_newest", tracker.max() == 5.0f);
        check("monotonic_increasing_min_is_oldest_in_window", tracker.min() == 2.0f);
    }

    // Adaptive window size change mid-stream (this project's macro/fisher
    // windows recompute their length every bar).
    {
        SlidingWindowExtremaTracker<10> tracker;
        tracker.push(10.0f, 0, 5);
        tracker.push(20.0f, 1, 5);
        tracker.push(5.0f, 2, 5);
        tracker.push(30.0f, 3, 2);  // window shrinks to 2 -> covers positions 2,3 -> values 5,30
        check("window_shrink_max_reflects_new_window", tracker.max() == 30.0f);
        check("window_shrink_min_reflects_new_window", tracker.min() == 5.0f);
    }

    // Critical: multiple pushes at the SAME position (multiple ticks within
    // a still-forming bar) must REPLACE, not accumulate as separate deque
    // entries -- same bug class as Task 2's accumulator, verified here too.
    {
        SlidingWindowExtremaTracker<5> tracker;
        tracker.push(10.0f, 0, 3);
        tracker.push(5.0f, 1, 3);
        tracker.push(3.0f, 1, 3);   // still bar 1: price moved down further
        tracker.push(50.0f, 1, 3);  // still bar 1: price spiked up -- this is the ONLY bar-1 value that should count
        check("same_position_replace_max_uses_latest", tracker.max() == 50.0f);
        check("same_position_replace_min_uses_latest", tracker.min() == 10.0f);
        // window=3 covering positions -1..1 clipped to what exists (0,1) -> values 10, 50 (bar 1's replaced value)
    }

    // Defensive: a position moving BACKWARD without an intervening reset()
    // is not something we've independently verified ACSIL can never do
    // outside a full recalculation. Must trigger a safe self-reset, counted.
    {
        SlidingWindowExtremaTracker<5> tracker;
        tracker.push(10.0f, 5, 3);
        tracker.push(20.0f, 6, 3);
        tracker.push(5.0f, 3, 3);  // position moved backward: 6 -> 3
        check("out_of_order_triggers_reset_max", tracker.max() == 5.0f);
        check("out_of_order_triggers_reset_min", tracker.min() == 5.0f);
        check("out_of_order_reset_count_incremented", tracker.outOfOrderResetCount() == 1);
        tracker.reset();  // legitimate reset() must not clear the diagnostic counter
        check("legitimate_reset_does_not_clear_out_of_order_count", tracker.outOfOrderResetCount() == 1);
    }

    // Reset clears state.
    {
        SlidingWindowExtremaTracker<3> tracker;
        tracker.push(1.0f, 0, 3);
        tracker.reset();
        check("reset_makes_tracker_empty", tracker.empty());
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `g++ -std=c++17 -I include tests/cpp/test_sliding_window_extrema_tracker.cpp -o /tmp/swet_test`
Expected: FAIL — `fatal error: SlidingWindowExtremaTracker.h: No such file or directory`

- [ ] **Step 3: Write minimal implementation**

Create `include/SlidingWindowExtremaTracker.h`:

```cpp
// SlidingWindowExtremaTracker.h — pure, header-only sliding-window min/max
// tracker via the classic monotonic-deque "sliding window maximum" technique
// (textbook streaming-algorithms result -- not novel here, just not
// previously present in this codebase). O(1) amortized per push, versus an
// O(window) rescan every call
// (docs/superpowers/plans/2026-08-12-observation-vector-incremental-accumulators.md).
//
// Handles a per-push-varying window size naturally: eviction is driven by
// `position - windowSize + 1` recomputed fresh on every push, so an adaptive
// window (this project's fisher window recomputes its length every bar)
// doesn't need special-casing.
//
// Position-aware upsert: same rationale as SlidingWindowMomentAccumulator.h
// -- Sierra Chart's AutoLoop=1 convention calls the calculator this backs on
// every tick, not once per bar close. Multiple pushes at the same position
// retract any deque entries already recorded for that position before
// re-inserting, so only the latest per-bar value ever counts. Same-position
// entries are always contiguous at the tail of each deque (position is
// monotonically non-decreasing across calls), so retracting them is a
// bounded pop_back loop, not a search.

#pragma once

#include "RingBuffer.h"
#include <cstddef>
#include <cstdint>

template <size_t MaxWindow>
class SlidingWindowExtremaTracker {
public:
    // Push the newest value at a monotonically non-decreasing logical
    // position (e.g. sc.Index). windowSize <= MaxWindow. O(1) amortized.
    // Defensive: see SlidingWindowMomentAccumulator.h's push() for the full
    // rationale -- a backward-moving position without an intervening
    // reset() self-resets rather than silently corrupting the deques.
    void push(float value, int64_t position, size_t windowSize) {
        if ((!m_maxDeque.empty() && position < m_maxDeque.back().position) ||
            (!m_minDeque.empty() && position < m_minDeque.back().position)) {
            reset();
            ++m_outOfOrderResetCount;
        }

        while (!m_maxDeque.empty() && m_maxDeque.back().position == position) {
            m_maxDeque.pop_back();
        }
        while (!m_maxDeque.empty() && m_maxDeque.back().value <= value) {
            m_maxDeque.pop_back();
        }
        m_maxDeque.push_back({value, position});

        while (!m_minDeque.empty() && m_minDeque.back().position == position) {
            m_minDeque.pop_back();
        }
        while (!m_minDeque.empty() && m_minDeque.back().value >= value) {
            m_minDeque.pop_back();
        }
        m_minDeque.push_back({value, position});

        const int64_t oldestAllowed = position - static_cast<int64_t>(windowSize) + 1;
        while (!m_maxDeque.empty() && m_maxDeque[0].position < oldestAllowed) {
            m_maxDeque.pop_front();
        }
        while (!m_minDeque.empty() && m_minDeque[0].position < oldestAllowed) {
            m_minDeque.pop_front();
        }
    }

    bool empty() const { return m_maxDeque.empty(); }
    float max() const { return m_maxDeque[0].value; }
    float min() const { return m_minDeque[0].value; }

    // Data-clearing reset (also called defensively by push() above). Does
    // NOT clear outOfOrderResetCount() -- see
    // SlidingWindowMomentAccumulator.h::reset() for the same rationale.
    void reset() {
        m_maxDeque.clear();
        m_minDeque.clear();
    }

    // Number of times push() detected a backward-moving position and
    // self-reset. Should stay 0 in practice; exposed as a diagnostic.
    size_t outOfOrderResetCount() const { return m_outOfOrderResetCount; }

private:
    struct Entry {
        float value;
        int64_t position;
    };
    // +1 headroom: a push can transiently hold one extra same-position-less
    // entry before the front-eviction loop runs, same convention as
    // RingBuffer.h's own documented "Capacity = maxWindowSize + 1" rule.
    RingBuffer<Entry, MaxWindow + 1> m_maxDeque;
    RingBuffer<Entry, MaxWindow + 1> m_minDeque;
    size_t m_outOfOrderResetCount = 0;
};
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -I include tests/cpp/test_sliding_window_extrema_tracker.cpp -o /tmp/swet_test && /tmp/swet_test`
Expected: `ALL PASS`, exit code 0, all checks including the same-position-replace group pass.

- [ ] **Step 5: Commit**

```bash
git add include/SlidingWindowExtremaTracker.h tests/cpp/test_sliding_window_extrema_tracker.cpp
git commit -m "feat(dod): add SlidingWindowExtremaTracker for O(1)-amortized sliding min/max"
```

---

### Task 4: Wire `SlidingWindowMomentAccumulator` into `CalculateLogVariance`

**Files:**
- Modify: `src/StudyHelperFunctions.cpp:240-250` (`AdaptiveCalculatorsState`), `src/StudyHelperFunctions.cpp:2962-2998` (`CalculateLogVariance`)

**Interfaces:**
- Consumes: `SlidingWindowMomentAccumulator<200>` (Task 2).

- [ ] **Step 1: Add the accumulator to `AdaptiveCalculatorsState`**

In `src/StudyHelperFunctions.cpp`, add the include near the top (alongside the file's existing includes):

```cpp
#include "SlidingWindowMomentAccumulator.h"
```

Modify `AdaptiveCalculatorsState` (currently lines 240-250):

```cpp
struct AdaptiveCalculatorsState {
    ATRCalculator marketSpeedCalc;
    EfficiencyRatioCalculator efficiencyRatioCalc;
    SlidingWindowMomentAccumulator<200> logVarianceAccumulator;
    float prevMarketSpeed = 1.0f;

    void reset() {
        marketSpeedCalc.reset();
        efficiencyRatioCalc.reset();
        logVarianceAccumulator.reset();
        prevMarketSpeed = 1.0f;
    }
};
```

(200 = the upper clamp bound of `macro_window_n` in `TripleScreen1.cpp:470`, the largest `long_n` this
calculator will ever be called with.)

- [ ] **Step 2: Rewrite `CalculateLogVariance`**

Replace the full function body (currently `StudyHelperFunctions.cpp:2962-2998`):

```cpp
float CalculateLogVariance(SCStudyInterfaceRef sc, int lookback_n) {
    // Canonical metric: log(short_var / long_var), robust to tiny variances.
    const int long_n = std::max(20, lookback_n);
    const int short_n = std::max(8, long_n / 4);

    if (sc.Index < (long_n + 1)) return 0.0f;

    auto* state = GetAdaptiveCalculatorsState(sc);
    if (!state) return 0.0f;

    // Push this bar's log-return (or replace this bar's still-forming
    // contribution if called again within the same bar -- see
    // SlidingWindowMomentAccumulator.h's position-aware upsert doc).
    // Non-positive prices are skipped (never pushed for this bar), matching
    // the original loop's validity guard; this is a documented, accepted,
    // negligible behavioral difference for the near-impossible case of a
    // non-positive quote -- see this plan's Task 4 discussion.
    const float price = sc.BaseData[SC_LAST][sc.Index];
    const float prevPrice = sc.BaseData[SC_LAST][sc.Index - 1];
    if (price > 0.0f && prevPrice > 0.0f) {
        state->logVarianceAccumulator.push(
            std::log(static_cast<double>(price) / prevPrice), sc.Index);
    }

    auto& acc = state->logVarianceAccumulator;
    const size_t available = acc.availableCount();
    const size_t shortK = std::min(static_cast<size_t>(short_n), available);
    const size_t longK = std::min(static_cast<size_t>(long_n), available);

    auto windowVariance = [&](size_t k) -> double {
        if (k < 2) return 0.0;
        const double sum = acc.sumLastK(k);
        const double sumSq = acc.sumSqLastK(k);
        const double mean = sum / static_cast<double>(k);
        const double var = (sumSq / static_cast<double>(k)) - (mean * mean);
        return std::max(var, 0.0);
    };

    const double short_var = windowVariance(shortK);
    const double long_var = windowVariance(longK);
    constexpr double kVarEps = 1e-12;

    const double log_ratio = std::log((short_var + kVarEps) / (long_var + kVarEps));
    return std::clamp(static_cast<float>(log_ratio), -6.0f, 6.0f);
}
```

- [ ] **Step 3: Build**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/StudyHelperFunctions.cpp
git commit -m "perf(dod): make CalculateLogVariance O(1) via SlidingWindowMomentAccumulator"
```

---

### Task 5: Wire `SlidingWindowMomentAccumulator` into `CalculateBurstiness`

**Files:**
- Modify: `src/StudyHelperFunctions.cpp` (`AdaptiveCalculatorsState`), `src/StudyHelperFunctions.cpp:3081-3115` (`CalculateBurstiness`)

**Interfaces:**
- Consumes: `SlidingWindowMomentAccumulator<40>` (Task 2), `cfc::ComputeBurstinessIndex` (already
  present, `CarryForwardCalculators.h`).

- [ ] **Step 1: Add a second accumulator to `AdaptiveCalculatorsState`**

Extend the struct from Task 4's Step 1 further:

```cpp
struct AdaptiveCalculatorsState {
    ATRCalculator marketSpeedCalc;
    EfficiencyRatioCalculator efficiencyRatioCalc;
    SlidingWindowMomentAccumulator<200> logVarianceAccumulator;
    SlidingWindowMomentAccumulator<40> burstinessAccumulator;
    float prevMarketSpeed = 1.0f;

    void reset() {
        marketSpeedCalc.reset();
        efficiencyRatioCalc.reset();
        logVarianceAccumulator.reset();
        burstinessAccumulator.reset();
        prevMarketSpeed = 1.0f;
    }
};
```

(40 = the upper clamp bound of `observation_window_n` — `StudyHelperFunctions.cpp:2297` — the largest
`lookback_n` `CalculateBurstiness` will ever be called with, per `TripleScreen2.cpp:284`'s call site.)

- [ ] **Step 2: Rewrite `CalculateBurstiness`**

Replace the full function body (currently `StudyHelperFunctions.cpp:3081-3115`, keep the existing
doc-comment lines):

```cpp
float CalculateBurstiness(SCStudyInterfaceRef sc, int lookback_n) {
    // Half-window variance ratio: log(RV_recent_half / RV_older_half).
    // Positive = volatility clustering in recent half = bursty.
    // Negative = volatility clustering in older half = quieting.
    // Replaces Barabási inter-arrival formula which is degenerate at hourly resolution.
    const int half = lookback_n / 2;
    if (sc.Index < lookback_n || half < 2) return 0.0f;

    auto* state = GetAdaptiveCalculatorsState(sc);
    if (!state) return 0.0f;

    const float range = sc.BaseData[SC_HIGH][sc.Index] - sc.BaseData[SC_LOW][sc.Index];
    state->burstinessAccumulator.push(static_cast<double>(range) * range, sc.Index);

    auto& acc = state->burstinessAccumulator;
    const size_t available = acc.availableCount();
    const size_t totalK = std::min(static_cast<size_t>(lookback_n), available);
    const size_t halfK = std::min(static_cast<size_t>(half), totalK);
    const size_t olderCount = totalK - halfK;

    if (olderCount == 0) return 0.0f;

    const double rv_recent = acc.sumSqLastK(halfK);
    const double rv_older = acc.sumSqLastK(totalK) - rv_recent;

    // Per-sample rate for fair comparison (halves may differ by 1 sample for odd lookback).
    const double rv_recent_rate = rv_recent / static_cast<double>(halfK);
    const double rv_older_rate = rv_older / static_cast<double>(olderCount);

    float& lastValidBurstiness = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::BURSTINESS_LAST_VALID_VALUE);
    const float burstiness = cfc::ComputeBurstinessIndex(rv_recent_rate, rv_older_rate, lastValidBurstiness);
    lastValidBurstiness = burstiness;
    return burstiness;
}
```

- [ ] **Step 3: Build**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/StudyHelperFunctions.cpp
git commit -m "perf(dod): make CalculateBurstiness O(1) via SlidingWindowMomentAccumulator"
```

---

### Task 6: Wire `SlidingWindowExtremaTracker` into `CalculateFisherInformation`

**Files:**
- Modify: `src/StudyHelperFunctions.cpp` (`AdaptiveCalculatorsState`), `src/StudyHelperFunctions.cpp:3000-3020` (`CalculateFisherInformation`)

**Interfaces:**
- Consumes: `SlidingWindowExtremaTracker<120>` (Task 3), `cfc::ComputeFisherInformation` (already
  present, `CarryForwardCalculators.h`).

- [ ] **Step 1: Add the extrema tracker to `AdaptiveCalculatorsState`**

Add the include:

```cpp
#include "SlidingWindowExtremaTracker.h"
```

Extend the struct from Task 5's Step 1 further:

```cpp
struct AdaptiveCalculatorsState {
    ATRCalculator marketSpeedCalc;
    EfficiencyRatioCalculator efficiencyRatioCalc;
    SlidingWindowMomentAccumulator<200> logVarianceAccumulator;
    SlidingWindowMomentAccumulator<40> burstinessAccumulator;
    SlidingWindowExtremaTracker<120> fisherExtremaTracker;
    float prevMarketSpeed = 1.0f;

    void reset() {
        marketSpeedCalc.reset();
        efficiencyRatioCalc.reset();
        logVarianceAccumulator.reset();
        burstinessAccumulator.reset();
        fisherExtremaTracker.reset();
        prevMarketSpeed = 1.0f;
    }
};
```

(120 = the upper clamp bound of `fisher_window_n` — `TripleScreen1.cpp:475` — the largest `lookback_n`
`CalculateFisherInformation` will ever be called with.)

- [ ] **Step 2: Rewrite `CalculateFisherInformation`**

Replace the full function body (currently `StudyHelperFunctions.cpp:3000-3020`, keep the leading
doc-comment):

```cpp
float CalculateFisherInformation(SCStudyInterfaceRef sc, int lookback_n) {
    // Fisher Transform of Price Relative to Range (Stoch-like normalized)
    // y = 0.5 * ln((1+x)/(1-x))

    if (sc.Index < lookback_n) return 0.0f;

    auto* state = GetAdaptiveCalculatorsState(sc);
    if (!state) return 0.0f;

    const float currentPrice = sc.BaseData[SC_LAST][sc.Index];
    state->fisherExtremaTracker.push(currentPrice, sc.Index, static_cast<size_t>(lookback_n));

    const float minPrice = state->fisherExtremaTracker.min();
    const float maxPrice = state->fisherExtremaTracker.max();

    float& lastValidFisherInfo = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::FISHER_INFO_LAST_VALID_VALUE);
    const float fisherInfo = cfc::ComputeFisherInformation(minPrice, maxPrice, currentPrice, lastValidFisherInfo);
    lastValidFisherInfo = fisherInfo;
    return fisherInfo;
}
```

- [ ] **Step 3: Build**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds.

- [ ] **Step 4: Commit**

```bash
git add src/StudyHelperFunctions.cpp
git commit -m "perf(dod): make CalculateFisherInformation O(1)-amortized via SlidingWindowExtremaTracker"
```

---

### Task 7: Golden-file characterization — accumulators vs. brute-force reference

**Files:**
- Create: `tests/cpp/test_incremental_accumulator_golden_file.cpp`

**Interfaces:**
- Consumes: `SlidingWindowMomentAccumulator<N>` (Task 2), `SlidingWindowExtremaTracker<N>` (Task 3).

This does **not** test `CalculateLogVariance`/`CalculateBurstiness`/`CalculateFisherInformation`
directly — those take `SCStudyInterfaceRef` and can't be driven without ACSIL. It tests the *window
statistics* each one now derives from an accumulator (variance-of-last-K; sum-of-squares split into
recent/older halves; min/max-of-last-K) against a brute-force recompute-from-scratch reference fed an
identical synthetic, adversarial sequence (multiple ticks per bar with a replaced-not-accumulated
per-bar value; a window size that changes bar to bar). Combined with
`test_carry_forward_calculators.cpp` (already covers "given these window statistics, compute the final
formula correctly"), this closes the loop end to end — the same standard `test_feature_scaler.cpp`
already set for its own DOD conversion ("captured against the CURRENT... implementation as a golden
baseline... to confirm bit-identical behavior").

- [ ] **Step 1: Write the test**

This is a characterization test comparing two things that don't exist as separate "before/after"
implementations (the accumulator is the only production code) — so there's no red step here in the
usual TDD sense; the "failing" state is "does the accumulator agree with an independently-written
brute-force reference," which is meaningful from the first run. Create
`tests/cpp/test_incremental_accumulator_golden_file.cpp`:

```cpp
// test_incremental_accumulator_golden_file.cpp — golden-file characterization:
// compares SlidingWindowMomentAccumulator/SlidingWindowExtremaTracker output
// against a brute-force reference over a synthetic, adversarial sequence
// (multiple ticks per bar with a REPLACED, not accumulated, per-bar value;
// a window size that changes bar to bar) -- the same standard
// test_feature_scaler.cpp already set for its own DOD conversion
// (docs/superpowers/plans/2026-08-12-observation-vector-incremental-accumulators.md).
//
// Build & run natively:
//   g++ -std=c++17 -I include tests/cpp/test_incremental_accumulator_golden_file.cpp -o /tmp/golden_test && /tmp/golden_test

#include "SlidingWindowMomentAccumulator.h"
#include "SlidingWindowExtremaTracker.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

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

bool approx(double a, double b, double relTol = 1e-6) {
    const double scale = std::max({std::fabs(a), std::fabs(b), 1.0});
    return std::fabs(a - b) <= relTol * scale;
}

// Small fixed-seed LCG for a fully deterministic, reproducible synthetic
// sequence -- not std::random, so this test's pass/fail is stable across
// runs, compilers, and platforms.
class DeterministicLcg {
public:
    explicit DeterministicLcg(uint32_t seed) : m_state(seed) {}
    uint32_t next() {
        m_state = m_state * 1664525u + 1013904223u;
        return m_state;
    }
    int nextInt(int lo, int hi) {
        const uint32_t range = static_cast<uint32_t>(hi - lo + 1);
        return lo + static_cast<int>(next() % range);
    }
    double nextUnit() {  // in [-1.0, 1.0]
        return (static_cast<double>(next()) / static_cast<double>(UINT32_MAX)) * 2.0 - 1.0;
    }

private:
    uint32_t m_state;
};

double bruteForceVariance(const std::vector<double>& values, size_t k) {
    const size_t n = values.size();
    double sum = 0.0;
    double sumSq = 0.0;
    for (size_t i = n - k; i < n; ++i) {
        sum += values[i];
        sumSq += values[i] * values[i];
    }
    const double mean = sum / static_cast<double>(k);
    return std::max((sumSq / static_cast<double>(k)) - (mean * mean), 0.0);
}

double bruteForceSumSq(const std::vector<double>& values, size_t k) {
    const size_t n = values.size();
    double sumSq = 0.0;
    for (size_t i = n - k; i < n; ++i) {
        sumSq += values[i] * values[i];
    }
    return sumSq;
}

void bruteForceMinMax(const std::vector<float>& values, size_t k, float& outMin, float& outMax) {
    const size_t n = values.size();
    outMin = values[n - k];
    outMax = values[n - k];
    for (size_t i = n - k; i < n; ++i) {
        outMin = std::min(outMin, values[i]);
        outMax = std::max(outMax, values[i]);
    }
}

}  // namespace

int main() {
    std::printf("Incremental accumulator golden-file characterization\n");

    constexpr size_t kNumBars = 250;
    constexpr size_t kMaxWindow = 40;  // <= the smallest MaxCapacity/MaxWindow under test

    // --- Log-variance-ratio shape: variance-of-last-K for two window sizes ---
    {
        SlidingWindowMomentAccumulator<kMaxWindow> acc;
        std::vector<double> perBarFinalLogReturn;
        DeterministicLcg rng(12345);
        double price = 100.0;

        for (int64_t bar = 0; bar < static_cast<int64_t>(kNumBars); ++bar) {
            const int ticksThisBar = rng.nextInt(1, 4);
            double lastLogReturnThisBar = 0.0;
            for (int tick = 0; tick < ticksThisBar; ++tick) {
                const double prevPrice = price;
                price = price * (1.0 + rng.nextUnit() * 0.001);
                lastLogReturnThisBar = std::log(price / prevPrice);
                acc.push(lastLogReturnThisBar, bar);
            }
            perBarFinalLogReturn.push_back(lastLogReturnThisBar);

            if (bar < 5) continue;  // warmup

            const size_t shortK = std::min<size_t>(5, perBarFinalLogReturn.size());
            const size_t longK = std::min<size_t>(20, perBarFinalLogReturn.size());

            const double accShortSum = acc.sumLastK(shortK);
            const double accShortSumSq = acc.sumSqLastK(shortK);
            const double accShortMean = accShortSum / static_cast<double>(shortK);
            const double accShortVar = std::max((accShortSumSq / static_cast<double>(shortK)) - (accShortMean * accShortMean), 0.0);

            const double accLongSum = acc.sumLastK(longK);
            const double accLongSumSq = acc.sumSqLastK(longK);
            const double accLongMean = accLongSum / static_cast<double>(longK);
            const double accLongVar = std::max((accLongSumSq / static_cast<double>(longK)) - (accLongMean * accLongMean), 0.0);

            const double bfShortVar = bruteForceVariance(perBarFinalLogReturn, shortK);
            const double bfLongVar = bruteForceVariance(perBarFinalLogReturn, longK);

            char name[96];
            std::snprintf(name, sizeof(name), "log_variance_short_var_matches_brute_force_at_bar_%lld", static_cast<long long>(bar));
            check(name, approx(accShortVar, bfShortVar));
            std::snprintf(name, sizeof(name), "log_variance_long_var_matches_brute_force_at_bar_%lld", static_cast<long long>(bar));
            check(name, approx(accLongVar, bfLongVar));
        }
    }

    // --- Burstiness shape: sum-of-squares split into recent/older halves ---
    {
        SlidingWindowMomentAccumulator<kMaxWindow> acc;
        std::vector<double> perBarFinalRangeSq;
        DeterministicLcg rng(67890);

        for (int64_t bar = 0; bar < static_cast<int64_t>(kNumBars); ++bar) {
            const int ticksThisBar = rng.nextInt(1, 4);
            double lastRangeSqThisBar = 0.0;
            for (int tick = 0; tick < ticksThisBar; ++tick) {
                const double range = std::fabs(rng.nextUnit()) * 2.0 + 0.25;  // plausible bar range
                lastRangeSqThisBar = range * range;
                acc.push(lastRangeSqThisBar, bar);
            }
            perBarFinalRangeSq.push_back(lastRangeSqThisBar);

            if (bar < 10) continue;

            const int lookbackN = 10 + static_cast<int>(bar % 15);  // adaptive-ish window, 10..24
            const size_t totalK = std::min<size_t>(static_cast<size_t>(lookbackN), perBarFinalRangeSq.size());
            const size_t halfK = totalK / 2;
            if (halfK < 1 || totalK - halfK < 1) continue;

            const double accRecent = acc.sumSqLastK(halfK);
            const double accOlder = acc.sumSqLastK(totalK) - accRecent;
            const double bfRecent = bruteForceSumSq(perBarFinalRangeSq, halfK);
            const double bfTotal = bruteForceSumSq(perBarFinalRangeSq, totalK);
            const double bfOlder = bfTotal - bfRecent;

            char name[96];
            std::snprintf(name, sizeof(name), "burstiness_recent_matches_brute_force_at_bar_%lld", static_cast<long long>(bar));
            check(name, approx(accRecent, bfRecent));
            std::snprintf(name, sizeof(name), "burstiness_older_matches_brute_force_at_bar_%lld", static_cast<long long>(bar));
            check(name, approx(accOlder, bfOlder));
        }
    }

    // --- Fisher shape: min/max-of-last-K, with a genuinely varying window size ---
    {
        SlidingWindowExtremaTracker<kMaxWindow> tracker;
        std::vector<float> perBarFinalPrice;
        DeterministicLcg rng(24680);
        double price = 100.0;

        for (int64_t bar = 0; bar < static_cast<int64_t>(kNumBars); ++bar) {
            const int ticksThisBar = rng.nextInt(1, 5);
            const int lookbackN = 8 + static_cast<int>(bar % 20);  // adaptive-ish window, 8..27
            float lastPriceThisBar = 0.0f;
            for (int tick = 0; tick < ticksThisBar; ++tick) {
                price = price * (1.0 + rng.nextUnit() * 0.002);
                lastPriceThisBar = static_cast<float>(price);
                tracker.push(lastPriceThisBar, bar, static_cast<size_t>(lookbackN));
            }
            perBarFinalPrice.push_back(lastPriceThisBar);

            if (bar < 8) continue;

            const size_t k = std::min<size_t>(static_cast<size_t>(lookbackN), perBarFinalPrice.size());
            float bfMin = 0.0f;
            float bfMax = 0.0f;
            bruteForceMinMax(perBarFinalPrice, k, bfMin, bfMax);

            char name[96];
            std::snprintf(name, sizeof(name), "fisher_min_matches_brute_force_at_bar_%lld", static_cast<long long>(bar));
            check(name, tracker.min() == bfMin);
            std::snprintf(name, sizeof(name), "fisher_max_matches_brute_force_at_bar_%lld", static_cast<long long>(bar));
            check(name, tracker.max() == bfMax);
        }
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run it**

Run: `g++ -std=c++17 -I include tests/cpp/test_incremental_accumulator_golden_file.cpp -o /tmp/golden_test && /tmp/golden_test`
Expected: `ALL PASS`, exit code 0. If it fails, that means the accumulator design has a real
discrepancy from brute-force truth — stop and re-examine the accumulator, do not adjust the tolerance
to make it pass.

- [ ] **Step 3: Commit**

```bash
git add tests/cpp/test_incremental_accumulator_golden_file.cpp
git commit -m "test(dod): golden-file characterization of accumulators vs brute-force reference"
```

---

### Task 8: Full clean build + full regression suite

**Files:** None new — verification only.

- [ ] **Step 1: Full clean build**

Run: `./build_dll.sh`
Expected: build succeeds with no errors.

- [ ] **Step 2: Run every native test suite**

```bash
g++ -std=c++17 -I include tests/cpp/test_ring_buffer.cpp -o /tmp/rb_test && /tmp/rb_test
g++ -std=c++17 -I include tests/cpp/test_sliding_window_moment_accumulator.cpp -o /tmp/swma_test && /tmp/swma_test
g++ -std=c++17 -I include tests/cpp/test_sliding_window_extrema_tracker.cpp -o /tmp/swet_test && /tmp/swet_test
g++ -std=c++17 -I include tests/cpp/test_incremental_accumulator_golden_file.cpp -o /tmp/golden_test && /tmp/golden_test
g++ -std=c++17 -I include tests/cpp/test_order_flow_asymmetry_engine.cpp -o /tmp/ofae_test && /tmp/ofae_test
g++ -std=c++17 -I include tests/cpp/test_carry_forward_calculators.cpp -o /tmp/cfc_test && /tmp/cfc_test
g++ -std=c++17 -I include -I include/generated tests/cpp/test_feature_scaler.cpp -o /tmp/fs_test && /tmp/fs_test
```

Expected: all seven suites exit 0, zero failures. The last three are regression checks — this plan
doesn't touch `OrderFlowAsymmetryEngine.h`, `CarryForwardCalculators.h`, or `FeatureScaler.h`, so their
existing tests must still pass unchanged.

- [ ] **Step 3: Commit** (only if Step 1 or 2 required a fix)

If both steps passed clean with no changes needed, there is nothing to commit here — skip.
