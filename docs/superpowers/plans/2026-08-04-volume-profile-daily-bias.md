# Real Volume Profile Value Area for Daily Bias — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `CalculateDailyBias()`'s naive 15%/85% range-split "TPO Value Area Proxy" with the real, volume-weighted previous-day Value Area (VAH/VAL), computed natively via ACSIL's own Volume-at-Price data — with automatic fallback to the existing proxy if that data isn't available.

**Architecture:** Two pure, header-only, unit-testable engines feed one ACSIL wiring task:
1. `include/DailyBiasEngine.h` (already-existing design, extended) — the Raschke 80%-Rule classification, given whatever VAH/VAL it's handed.
2. `include/VolumeProfileEngine.h` (new) — the standard Market-Profile Point-of-Control / Value-Area expansion algorithm, given a price→volume histogram.

The ACSIL glue in between aggregates the previous trading day's `sc.VolumeAtPriceForBars` (TS3's own 15-minute bars, no external chart or study needed) into that histogram, once per trading-day change.

**Tech Stack:** C++17, Sierra Chart ACSIL SDK (`/mnt/c/SierraChart2/ACS_Source/`), no test framework (hand-rolled assertion harness matching `tests/cpp/test_triple_barrier_parity.cpp`).

## Global Constraints

- No heap allocations in recurring ACSIL update paths (CLAUDE.md Performance Rules) — the VAP aggregation must be gated to "once per trading day," not per-tick (mirrors the existing `LAST_AMIHUD_SAMPLE_INDEX` guard pattern in `TripleScreen3.cpp`).
- Before removing any symbol, search the full repo for usages in `.h`, `.cpp`, and PCH files (CLAUDE.md Code Safety Rules).
- No schema/wire changes — this touches only internal C++ computation.
- Build via `./build_dll.sh` only — never raw `cmake`/`ninja`.
- TDD: write the failing test before the production change (CLAUDE.md Guardrail 4).

## Design decisions made during scoping (read before starting)

1. **`StructureEngine.cpp`'s `GetRecurrenceRate()` is out of scope.** The prior research (`docs/ADR/sierra_chart_data_feed_setup.md`) flagged it as a second "Point of Control proxy," but it has zero call sites in the live code (`grep -rn "GetRecurrenceRate()" src/*.cpp include/*.h` only finds its own definition/declaration). The live `recurrence_rate` observation dimension comes from an unrelated, already-legitimate RQA function (`CalculateRecurrenceRate()`, `StudyHelperFunctions.cpp:4173`). Nothing to swap there.
2. **No chart study needed at all.** Sierra Chart's ACSIL SDK exposes `sc.GetPointOfControlAndValueAreaPricesForBar(BarIndex, POC, VAH, VAL, Percentage)` directly (confirmed in `/mnt/c/SierraChart2/ACS_Source/sierrachart.h`) — but it's **per-bar**, not per-day, and has no `ChartNumber` parameter (it only reads the calling chart's own bars). Since none of TS1/TS2/TS3 is a daily-period chart, this plan does **not** call that function directly. Instead it aggregates the lower-level `sc.VolumeAtPriceForBars` (a `c_VAPContainer`, confirmed in `VAPContainer.h`) across the previous trading day's own TS3 15-minute bars, and computes POC/VA itself — self-contained on the existing TS3 chart, no new chart or study, and it makes the VA-expansion algorithm itself pure/testable as a bonus.
3. **Prerequisite: `sc.MaintainVolumeAtPriceData = 1`** must be set in `TripleScreen3.cpp`'s `SetDefaults` block, and Sierra Chart's global setting **Data/Trade Service Settings → Intraday Data Storage Time Unit** must be **1 Tick** (confirmed via Sierra Chart's own ACSIL docs). This does **not** require market depth — it's built from trade-level Time & Sales data, which Denali CME (no-depth) already provides and which this codebase already consumes (`sc.GetTimeAndSales()`, `sc.BidVolume`/`sc.AskVolume`). Task 1 verifies this setting is actually in effect before the rest of the plan is meaningful.

**Design review (`lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_077` / `GEMINI_REVIEW_077`):** this plan was reviewed before implementation. Two substantive corrections came out of it and are folded into Tasks 2 and 5 below: (a) the Value-Area expansion must use the standard **2-row rule** (compare sums of the next *two* ticks on each side, not one) — this also fixes a real bug in the original single-row design, where a `std::map`-based histogram silently skipped untraded ticks instead of treating them as real zero-volume positions; and (b) the aggregation must be **RTH-only (09:30-16:00 ET)**, filtering out overnight/Globex bars, for the same reason this codebase already keeps a separate overnight pool for the Amihud percentile calculation. One correction ran the other way: Gemini's suggested RTH-check snippet used `HMS_TIME`, which the vendored SDK (`scdatetime.h:985`) marks deprecated in favor of `SCDateTime(H,M,S,MS).GetTime()` — the modern form is what Task 5 uses. The third recommendation (self-aggregation over a chart-visible study) confirmed the plan's existing design with no change needed.

---

### Task 1: Verify the Intraday Data Storage prerequisite (manual)

**Files:** None — operational verification. Its outcome (confirmed or corrected) gates whether Task 5's aggregation will see any data at runtime.

- [ ] **Step 1: Check the current global setting**

In Sierra Chart: `Global Settings` → `Data/Trade Service Settings` → find `Intraday Data Storage Time Unit`. Confirm it is set to `1 Tick`. If it's set to a coarser unit (e.g., 1 Second), `sc.VolumeAtPriceForBars` will be empty or inaccurate — this must be fixed before Task 5's output can be trusted.

- [ ] **Step 2: Confirm no historical re-download is silently required**

Changing this setting can require Sierra Chart to reload/redownload intraday historical data at the new granularity for it to apply retroactively. Note whether a redownload is needed for the ES chart before Task 5's aggregation will see valid data for "yesterday," and budget time for that if so.

---

### Task 2: `VolumeProfileEngine.h` — pure Point-of-Control / Value-Area algorithm (TDD)

**Design note (post-Gemini review, `lbrnet/logs/rc_gemini.log` `GEMINI_REVIEW_077`):** the algorithm below implements the textbook **2-row Value Area expansion rule** — compare the sum of the next *two* price ticks above the current VAH against the sum of the next *two* ticks below the current VAL, expand by two ticks on the winning side, expand both sides on an exact tie. This isn't just a convention correction: it requires a dense, tick-indexed array rather than a `std::map` keyed by traded price, because the 2-row rule must see an untraded tick as a real "0 volume" position it can look past — a `std::map`'s iterators (`std::next`/`std::prev`) silently skip straight to the next *traded* price, which is a different (wrong) thing once you're looking two ticks ahead. The array's cache-locality benefit Gemini also raised is real but secondary: this runs once per trading day, not once per tick, so it isn't hot-path in the CLAUDE.md sense.

**Files:**
- Create: `include/VolumeProfileEngine.h`
- Create: `tests/cpp/test_volume_profile_engine.cpp`

**Interfaces:**
- Produces: `namespace vpe { struct PriceVolume { int32_t priceInTicks; double volume; }; struct ValueArea { int32_t pocPriceInTicks, valueAreaHighInTicks, valueAreaLowInTicks; bool valid; }; ValueArea ComputeValueArea(const std::vector<PriceVolume>& levels, double valueAreaPercentage = 70.0); }` — consumed by Task 5's ACSIL aggregation.

- [ ] **Step 1: Write the failing test**

Create `tests/cpp/test_volume_profile_engine.cpp`:

```cpp
// test_volume_profile_engine.cpp — unit tests for the pure Point-of-Control /
// Value-Area expansion algorithm.
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I include tests/cpp/test_volume_profile_engine.cpp -o /tmp/vpe_test && /tmp/vpe_test

#include "VolumeProfileEngine.h"

#include <cstdio>

using namespace vpe;

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
    std::printf("VolumeProfileEngine unit tests\n");

    // Empty input -> invalid result, no crash.
    {
        const ValueArea va = ComputeValueArea({});
        check("empty_input_is_invalid", !va.valid);
    }

    // Single price level -> POC = VAH = VAL = that price.
    {
        const ValueArea va = ComputeValueArea({{5000, 100.0}});
        check("single_level_poc_equals_bounds",
              va.valid && va.pocPriceInTicks == 5000 &&
              va.valueAreaHighInTicks == 5000 && va.valueAreaLowInTicks == 5000);
    }

    // Symmetric bell-shaped volume around 5002 (the classic textbook case):
    //   4999:10  5000:20  5001:40  5002:60  5003:40  5004:20  5005:10
    // total=200, target(70%)=140. POC=5002 (60), captured=60.
    // 2-row check: above(5003+5004=40+20=60) vs below(5001+5000=40+20=60) ->
    // exact tie -> expand BOTH sides by 2: VAH=5004, VAL=5000,
    // captured=60+60+60=180 >= 140 -> stop.
    {
        const std::vector<PriceVolume> levels = {
            {4999, 10.0}, {5000, 20.0}, {5001, 40.0}, {5002, 60.0},
            {5003, 40.0}, {5004, 20.0}, {5005, 10.0},
        };
        const ValueArea va = ComputeValueArea(levels, 70.0);
        check("symmetric_distribution_poc",
              va.valid && va.pocPriceInTicks == 5002);
        check("symmetric_distribution_value_area_bounds",
              va.valueAreaHighInTicks == 5004 && va.valueAreaLowInTicks == 5000);
    }

    // Skewed distribution: POC not centered, value area should still expand
    // toward whichever side has more volume at each step.
    // 5000:5  5001:10  5002:80(POC)  5003:5
    // total=100, target(70%)=70. POC=5002(80) already >= 70 -> VAH=VAL=5002,
    // loop doesn't even need to expand.
    {
        const std::vector<PriceVolume> levels = {
            {5000, 5.0}, {5001, 10.0}, {5002, 80.0}, {5003, 5.0},
        };
        const ValueArea va = ComputeValueArea(levels, 70.0);
        check("dominant_poc_needs_no_expansion",
              va.valid && va.pocPriceInTicks == 5002 &&
              va.valueAreaHighInTicks == 5002 && va.valueAreaLowInTicks == 5002);
    }

    // The reason this needs to be a 2-row rule, made concrete: POC at 5002,
    // an UNTRADED tick at 5003 (no PriceVolume entry for it at all -- must
    // be treated as a real 0-volume position, not skipped over), and a real
    // institutional cluster hiding one tick further at 5004. A naive 1-row
    // (or std::map-iterator-skipping) algorithm would see "next entry above
    // POC" as 5004 directly (wrongly treating the gap as if adjacent), or
    // would stall at the empty tick depending on implementation -- either
    // way it does not correctly evaluate "the next two ticks" as a unit.
    // 5000:10 5001:5 5002:50(POC) [5003: implicit 0, no entry] 5004:40
    // total=105, target(70%)=73.5. captured=50.
    // above(idx3+idx4 = 0+40=40) vs below(idx1+idx0 = 5+10=15) -> above wins.
    // VAH steps 2 ticks to 5004 (tunneling through the empty 5003), captured=90>=73.5.
    {
        const std::vector<PriceVolume> levels = {
            {5000, 10.0}, {5001, 5.0}, {5002, 50.0}, {5004, 40.0},
        };
        const ValueArea va = ComputeValueArea(levels, 70.0);
        check("two_row_rule_tunnels_through_an_untraded_gap",
              va.valid && va.pocPriceInTicks == 5002 &&
              va.valueAreaHighInTicks == 5004 && va.valueAreaLowInTicks == 5002);
    }

    // Multiple PriceVolume entries at the SAME price must be summed, not
    // overwritten (this models multiple bars in the aggregated day
    // contributing volume at the same price level).
    {
        const std::vector<PriceVolume> levels = {
            {5000, 10.0}, {5000, 15.0}, // same price, two bars -> 25 total
            {5001, 5.0},
        };
        const ValueArea va = ComputeValueArea(levels, 70.0);
        check("duplicate_price_entries_are_summed",
              va.valid && va.pocPriceInTicks == 5000);
    }

    // All-zero volume -> invalid (no real distribution to compute from).
    {
        const ValueArea va = ComputeValueArea({{5000, 0.0}, {5001, 0.0}});
        check("all_zero_volume_is_invalid", !va.valid);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails (header doesn't exist yet)**

Run: `g++ -std=c++17 -I include tests/cpp/test_volume_profile_engine.cpp -o /tmp/vpe_test`
Expected: FAIL to compile with `VolumeProfileEngine.h: No such file or directory`

- [ ] **Step 3: Write the minimal implementation**

Create `include/VolumeProfileEngine.h`:

```cpp
// VolumeProfileEngine.h — pure, header-only Point-of-Control / Value-Area
// computation from a price->volume histogram (docs/ADR/sierra_chart_data_feed_setup.md;
// docs/superpowers/plans/2026-08-04-volume-profile-daily-bias.md;
// design reviewed lbrnet/logs/rc_gemini.log GEMINI_REVIEW_077).
//
// SCOPE: given an already-accumulated set of (price, volume) pairs — e.g. one
// trading day's worth of TS3's own sc.VolumeAtPriceForBars, summed across all
// of that day's 15-min bars by the ACSIL caller — compute the Point of
// Control (the price level with the most volume) and the Value Area bounds
// via the standard 2-row Market Profile expansion: compare the sum of the
// next two ticks above VAH against the sum of the next two ticks below VAL,
// expand by two ticks toward the winning side, expand both sides on an exact
// tie, until ValueAreaPercentage of total volume is captured. Represented as
// a dense, tick-indexed array (not a map) so an untraded tick is a real
// zero-volume position the 2-row lookahead can see past, not a skipped node.
// No Sierra Chart types, natively unit-testable
// (tests/cpp/test_volume_profile_engine.cpp).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vpe {

struct PriceVolume {
    int32_t priceInTicks;
    double volume;
};

struct ValueArea {
    int32_t pocPriceInTicks = 0;
    int32_t valueAreaHighInTicks = 0;
    int32_t valueAreaLowInTicks = 0;
    bool valid = false;  // false if input was empty or carried no real volume
};

inline ValueArea ComputeValueArea(const std::vector<PriceVolume>& levels, double valueAreaPercentage = 70.0) {
    ValueArea result;
    if (levels.empty()) return result;

    int32_t minPrice = levels.front().priceInTicks;
    int32_t maxPrice = levels.front().priceInTicks;
    for (const auto& lv : levels) {
        minPrice = std::min(minPrice, lv.priceInTicks);
        maxPrice = std::max(maxPrice, lv.priceInTicks);
    }

    const size_t numTicks = static_cast<size_t>(maxPrice - minPrice) + 1;
    std::vector<double> volumeByOffset(numTicks, 0.0);
    double totalVolume = 0.0;
    for (const auto& lv : levels) {
        volumeByOffset[static_cast<size_t>(lv.priceInTicks - minPrice)] += lv.volume;
        totalVolume += lv.volume;
    }
    if (totalVolume <= 0.0) return result;

    // POC = the offset with the most volume. Ties resolve to the lowest
    // price (first offset) encountered during the scan, for determinism.
    size_t pocOffset = 0;
    for (size_t i = 1; i < numTicks; ++i) {
        if (volumeByOffset[i] > volumeByOffset[pocOffset]) pocOffset = i;
    }

    size_t lowOffset = pocOffset;
    size_t highOffset = pocOffset;
    double captured = volumeByOffset[pocOffset];
    const double targetVolume = totalVolume * (valueAreaPercentage / 100.0);

    auto sumAbove = [&](size_t fromOffset) {
        double sum = 0.0;
        for (size_t i = fromOffset + 1; i <= fromOffset + 2 && i < numTicks; ++i) {
            sum += volumeByOffset[i];
        }
        return sum;
    };
    auto sumBelow = [&](size_t fromOffset) {
        double sum = 0.0;
        for (size_t i = 1; i <= 2 && fromOffset >= i; ++i) {
            sum += volumeByOffset[fromOffset - i];
        }
        return sum;
    };

    while (captured < targetVolume) {
        const bool haveAbove = highOffset + 1 < numTicks;
        const bool haveBelow = lowOffset >= 1;
        if (!haveAbove && !haveBelow) break;  // exhausted both sides of the distribution

        const double aboveVolume = haveAbove ? sumAbove(highOffset) : 0.0;
        const double belowVolume = haveBelow ? sumBelow(lowOffset) : 0.0;

        if (haveAbove && (!haveBelow || aboveVolume > belowVolume)) {
            highOffset = std::min(highOffset + 2, numTicks - 1);
            captured += aboveVolume;
        } else if (haveBelow && (!haveAbove || belowVolume > aboveVolume)) {
            lowOffset -= std::min<size_t>(2, lowOffset);
            captured += belowVolume;
        } else {
            // Exact tie with both sides available: expand both, per the
            // standard convention (maintains distribution symmetry).
            highOffset = std::min(highOffset + 2, numTicks - 1);
            lowOffset -= std::min<size_t>(2, lowOffset);
            captured += aboveVolume + belowVolume;
        }
    }

    result.pocPriceInTicks = minPrice + static_cast<int32_t>(pocOffset);
    result.valueAreaHighInTicks = minPrice + static_cast<int32_t>(highOffset);
    result.valueAreaLowInTicks = minPrice + static_cast<int32_t>(lowOffset);
    result.valid = true;
    return result;
}

}  // namespace vpe
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -I include tests/cpp/test_volume_profile_engine.cpp -o /tmp/vpe_test && /tmp/vpe_test`
Expected: `ALL PASS (0 failures)`

- [ ] **Step 5: Commit**

```bash
git add include/VolumeProfileEngine.h tests/cpp/test_volume_profile_engine.cpp
git commit -m "feat(volume-profile): add pure, testable Point-of-Control/Value-Area engine (2-row rule)"
```

---

### Task 3: `DailyBiasEngine.h` — extract the classification core with real-Value-Area support (TDD)

**Files:**
- Create: `include/DailyBiasEngine.h`
- Create: `tests/cpp/test_daily_bias_engine.cpp`

**Interfaces:**
- Produces: `namespace dbe { enum class Bias : int { ... }; struct DailyBiasInputs { float lastPrice, prevDayHigh, prevDayLow, hurstExponent, valueAreaLow = 0.0f, valueAreaHigh = 0.0f; }; Bias ComputeDailyBias(const DailyBiasInputs&); }` — consumed by Task 4's wrapper.

- [ ] **Step 1: Write the failing test**

Create `tests/cpp/test_daily_bias_engine.cpp`:

```cpp
// test_daily_bias_engine.cpp — unit tests for the pure Daily Bias classification core.
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I include tests/cpp/test_daily_bias_engine.cpp -o /tmp/dbe_test && /tmp/dbe_test

#include "DailyBiasEngine.h"

#include <cstdio>

using namespace dbe;

namespace {

int g_failures = 0;

void check(const char* name, Bias got, Bias expected) {
    if (got == expected) {
        std::printf("  PASS  %s\n", name);
    } else {
        ++g_failures;
        std::printf("  FAIL  %s  got=%d exp=%d\n", name,
                     static_cast<int>(got), static_cast<int>(expected));
    }
}

}  // namespace

int main() {
    std::printf("DailyBiasEngine unit tests\n");

    // Zone A: breakout above prevDayHigh, Hurst trending -> persistent
    check("breakout_up_trending_persistent",
          ComputeDailyBias({5150.0f, 5100.0f, 5000.0f, 0.6f}),
          Bias::BULLISH_TREND_PERSISTENT);

    // Zone A: breakout above prevDayHigh, Hurst mean-reverting -> trap
    check("breakout_up_mean_reverting_trap",
          ComputeDailyBias({5150.0f, 5100.0f, 5000.0f, 0.3f}),
          Bias::BEARISH_VOLATILITY_TRAP);

    // Zone B: breakdown below prevDayLow, Hurst trending -> persistent
    check("breakdown_trending_persistent",
          ComputeDailyBias({4950.0f, 5100.0f, 5000.0f, 0.6f}),
          Bias::BEARISH_TREND_PERSISTENT);

    // Physics veto: Hurst ~0.5 -> random walk veto regardless of price
    check("hurst_random_walk_veto",
          ComputeDailyBias({5150.0f, 5100.0f, 5000.0f, 0.5f}),
          Bias::PHYSICS_VETO_RANDOM_WALK);

    // Invalid prevDay data -> random walk veto
    check("invalid_prev_day_veto",
          ComputeDailyBias({5020.0f, 0.0f, 0.0f, 0.6f}),
          Bias::PHYSICS_VETO_RANDOM_WALK);

    // No real Value Area supplied (defaults 0.0f) -> falls back to the
    // 15%/85% range-split proxy. range=100, proxy val=5015, vah=5085.
    // lastPrice=5020 is between val and vah -> Zone E rotation under the proxy.
    check("fallback_proxy_rotation_when_no_real_value_area",
          ComputeDailyBias({5020.0f, 5100.0f, 5000.0f, 0.6f}),
          Bias::VALUE_AREA_ROTATION);

    // Real Value Area supplied and DIFFERENT from the proxy: real VAL=5022 >
    // proxy VAL=5015, moving lastPrice=5020 from "rotation" (under the proxy)
    // into "bullish mean reversion" (under the real, volume-weighted VAL).
    // This is the behavior this task exists to change.
    check("real_value_area_overrides_proxy_classification",
          ComputeDailyBias({5020.0f, 5100.0f, 5000.0f, 0.6f, /*valueAreaLow=*/5022.0f, /*valueAreaHigh=*/5085.0f}),
          Bias::BULLISH_MEAN_REVERSION);

    // Only one of the two real bounds supplied -> still falls back to proxy
    // (haveRealValueArea requires BOTH bounds to avoid a mismatched pair).
    check("partial_real_value_area_still_falls_back",
          ComputeDailyBias({5020.0f, 5100.0f, 5000.0f, 0.6f, /*valueAreaLow=*/5022.0f, /*valueAreaHigh=*/0.0f}),
          Bias::VALUE_AREA_ROTATION);

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails (header doesn't exist yet)**

Run: `g++ -std=c++17 -I include tests/cpp/test_daily_bias_engine.cpp -o /tmp/dbe_test`
Expected: FAIL to compile with `DailyBiasEngine.h: No such file or directory`

- [ ] **Step 3: Write the minimal implementation**

Create `include/DailyBiasEngine.h`:

```cpp
// DailyBiasEngine.h — pure, header-only Daily Bias classification core
// (docs/ADR/sierra_chart_data_feed_setup.md "genuine low-hanging fruit").
//
// SCOPE: Deterministic Raschke 80%-Rule / gap-analysis classification only —
// no Sierra Chart types, no I/O — so it is natively unit-testable
// (tests/cpp/test_daily_bias_engine.cpp) without the ACSIL SDK. Live code
// (StudyHelperFunctions.cpp's CalculateDailyBias()) wraps ComputeDailyBias()
// and maps dbe::Bias -> DailyBiasEnum (Indicator.h) by identical underlying
// values, keeping this header free of engine headers for native testability
// (same pattern as TripleBarrierEngine.h).
//
// valueAreaLow/valueAreaHigh are the REAL, volume-weighted Value Area,
// computed by VolumeProfileEngine.h from Sierra Chart's own Volume-at-Price
// data (Task 5). Pass 0.0f for either (the sentinel "not available") to fall
// back to the naive 15%/85% range-split proxy this engine replaces.

#pragma once

namespace dbe {

enum class Bias : int {
    PHYSICS_VETO_RANDOM_WALK = 0,
    BULLISH_TREND_PERSISTENT = 1,
    BULLISH_MEAN_REVERSION = 2,
    PHYSICS_VETO_HIGH_ENTROPY = 3,
    VALUE_AREA_ROTATION = 4,
    BULLISH_VOLATILITY_TRAP = 5,
    BEARISH_TREND_PERSISTENT = -1,
    BEARISH_MEAN_REVERSION = -2,
    BEARISH_VOLATILITY_TRAP = -5,
};

struct DailyBiasInputs {
    float lastPrice = 0.0f;
    float prevDayHigh = 0.0f;
    float prevDayLow = 0.0f;
    float hurstExponent = 0.0f;
    float valueAreaLow = 0.0f;   // Real Volume Profile VAL; 0.0f = use proxy
    float valueAreaHigh = 0.0f;  // Real Volume Profile VAH; 0.0f = use proxy
};

inline Bias ComputeDailyBias(const DailyBiasInputs& in) {
    if (in.hurstExponent > 0.45f && in.hurstExponent < 0.55f) {
        return Bias::PHYSICS_VETO_RANDOM_WALK;
    }

    if (in.prevDayHigh <= 0.0f || in.prevDayLow <= 0.0f || in.prevDayHigh <= in.prevDayLow) {
        return Bias::PHYSICS_VETO_RANDOM_WALK;
    }

    const float range = in.prevDayHigh - in.prevDayLow;

    // Real Volume Profile Value Area takes precedence over the naive
    // range-split proxy. Require BOTH bounds to avoid mixing a real bound
    // with a mismatched proxy bound.
    const bool haveRealValueArea = in.valueAreaLow > 0.0f && in.valueAreaHigh > 0.0f;
    const float val = haveRealValueArea ? in.valueAreaLow  : in.prevDayLow + (range * 0.15f);
    const float vah = haveRealValueArea ? in.valueAreaHigh : in.prevDayLow + (range * 0.85f);

    if (in.lastPrice > in.prevDayHigh) {
        if (in.hurstExponent > 0.0f && in.hurstExponent < 0.4f) {
            return Bias::BEARISH_VOLATILITY_TRAP;
        }
        return Bias::BULLISH_TREND_PERSISTENT;
    }
    if (in.lastPrice < in.prevDayLow) {
        if (in.hurstExponent > 0.0f && in.hurstExponent < 0.4f) {
            return Bias::BULLISH_VOLATILITY_TRAP;
        }
        return Bias::BEARISH_TREND_PERSISTENT;
    }
    if (in.lastPrice > in.prevDayLow && in.lastPrice < val) {
        return Bias::BULLISH_MEAN_REVERSION;
    }
    if (in.lastPrice < in.prevDayHigh && in.lastPrice > vah) {
        return Bias::BEARISH_MEAN_REVERSION;
    }
    return Bias::VALUE_AREA_ROTATION;
}

}  // namespace dbe
```

- [ ] **Step 4: Run test to verify it passes**

Run: `g++ -std=c++17 -I include tests/cpp/test_daily_bias_engine.cpp -o /tmp/dbe_test && /tmp/dbe_test`
Expected: `ALL PASS (0 failures)`

- [ ] **Step 5: Commit**

```bash
git add include/DailyBiasEngine.h tests/cpp/test_daily_bias_engine.cpp
git commit -m "feat(daily-bias): add pure, testable DailyBiasEngine with real-Value-Area support"
```

---

### Task 4: Delegate `CalculateDailyBias()` to the new engine

**Files:**
- Modify: `include/StudyHelperFunctions.h:13`
- Modify: `src/StudyHelperFunctions.cpp:1486-1554` (the `CalculateDailyBias` function)

**Interfaces:**
- Consumes: `dbe::DailyBiasInputs`, `dbe::Bias`, `dbe::ComputeDailyBias` (Task 3).
- Produces: `DailyBiasEnum CalculateDailyBias(float lastPrice, float prevDayHigh, float prevDayLow, float hurstExponent, float entropy, float valueAreaLow = 0.0f, float valueAreaHigh = 0.0f)` — consumed by Task 5's updated `TripleScreen3.cpp` call site. The first five parameters are unchanged; the two new trailing parameters default to `0.0f` so the one existing call site keeps compiling and behaving identically until Task 5 updates it to pass real values.

- [ ] **Step 1: Update the header declaration**

In `include/StudyHelperFunctions.h:13`, change:

```cpp
DailyBiasEnum CalculateDailyBias(float lastPrice, float prevDayHigh, float prevDayLow, float hurstExponent, float entropy);
```

to:

```cpp
DailyBiasEnum CalculateDailyBias(float lastPrice, float prevDayHigh, float prevDayLow, float hurstExponent, float entropy,
                                  float valueAreaLow = 0.0f, float valueAreaHigh = 0.0f);
```

- [ ] **Step 2: Replace the function body to delegate**

In `src/StudyHelperFunctions.cpp`, add `#include "DailyBiasEngine.h"` near the top alongside the existing includes, then replace the full body of `CalculateDailyBias` (lines 1486-1554, the whole function including its old inline "TPO Value Area Proxy" comment and math) with:

```cpp
DailyBiasEnum CalculateDailyBias(float lastPrice, float prevDayHigh, float prevDayLow, float hurstExponent, float /*entropy*/,
                                  float valueAreaLow, float valueAreaHigh)
{
    const dbe::Bias bias = dbe::ComputeDailyBias({
        lastPrice, prevDayHigh, prevDayLow, hurstExponent, valueAreaLow, valueAreaHigh
    });
    return static_cast<DailyBiasEnum>(static_cast<int8_t>(bias));
}
```

- [ ] **Step 3: Rebuild and confirm no regressions**

Run: `./build_dll.sh --no-clean`
Expected: `Build Successful` — this only changes `CalculateDailyBias`'s internals; its one existing call site (`TripleScreen3.cpp:682`, per `grep -rn "CalculateDailyBias" src/*.cpp include/*.h`) still compiles because the two new parameters default to `0.0f`.

- [ ] **Step 4: Commit**

```bash
git add include/StudyHelperFunctions.h src/StudyHelperFunctions.cpp
git commit -m "refactor(daily-bias): delegate CalculateDailyBias to DailyBiasEngine"
```

---

### Task 5: Aggregate real Volume-at-Price into a daily Value Area, once per trading day

**Files:**
- Modify: `include/MindfulTraderConstants.h` (new persistent-var index)
- Modify: `include/IndicatorManager.h` (new cache fields + getters/setter)
- Modify: `src/TripleScreen3.cpp` (`SetDefaults`, new day-gated aggregation block, updated `CalculateDailyBias` call)

**Interfaces:**
- Consumes: `vpe::PriceVolume`, `vpe::ValueArea`, `vpe::ComputeValueArea` (Task 2); `dbe`-backed `CalculateDailyBias(..., valueAreaLow, valueAreaHigh)` (Task 4); ACSIL: `sc.MaintainVolumeAtPriceData`, `sc.VolumeAtPriceForBars` (`c_VAPContainer`: `GetSizeAtBarIndex`, `GetVAPElementAtIndex`), `sc.GetFirstIndexForDate`, `sc.TickSize`, `sc.BaseDateTimeIn[].GetDate()`/`.GetTime()`, `SCDateTime(Hour, Minute, Second, Millisecond).GetTime()` (the non-deprecated replacement for `HMS_TIME`, `scdatetime.h:985`) — all confirmed present in `/mnt/c/SierraChart2/ACS_Source/sierrachart.h`, `scdatetime.h`, and `VAPContainer.h`, none previously used elsewhere in this codebase (verify signatures against the SDK header at implementation time regardless).
- Produces: `float IndicatorManager::GetCachedValueAreaHigh() const` / `GetCachedValueAreaLow() const` — the last consumers in this chain.

This task's new aggregation code calls the Sierra Chart ACSIL SDK directly, so it cannot be unit-tested the way Tasks 2-3 were — this codebase has no `sc` mocking layer (confirmed: `grep -rln "gtest\|catch2\|TEST_CASE\|TEST(" .` returns nothing). Verification here is build-green plus a Sierra Chart replay per Step 6, matching how the rest of this ACSIL codebase is verified.

- [ ] **Step 1: Reserve a new persistent-var index**

In `include/MindfulTraderConstants.h`, next to `const int LAST_AMIHUD_SAMPLE_INDEX = 26;` (line 85), add:

```cpp
    const int LAST_VALUE_AREA_SAMPLE_INDEX = 27; // Last bar index a daily Value Area aggregation was run for
```

- [ ] **Step 2: Set `MaintainVolumeAtPriceData` in `SetDefaults`**

In `src/TripleScreen3.cpp`, inside the `if (sc.SetDefaults)` block (line 186), add:

```cpp
        sc.MaintainVolumeAtPriceData = 1;  // Required for VolumeAtPriceForBars aggregation below (Task 5)
```

- [ ] **Step 3: Extend `IndicatorManager`'s cache**

In `include/IndicatorManager.h`, inside the `DailyCache` struct (~line 184):

```cpp
    struct DailyCache {
        int tradingDay = -1;        // Cached trading day (Julian date)
        float prevDayHigh = 0.0f;   // Previous day's session high
        float prevDayLow = 0.0f;    // Previous day's session low
        float valueAreaHigh = 0.0f; // Previous day's real Volume Profile VAH (0.0f = unavailable)
        float valueAreaLow = 0.0f;  // Previous day's real Volume Profile VAL (0.0f = unavailable)
        bool validated = false;     // Validation done once at startup
    };
```

Next to `GetCachedPrevDayHigh()`/`GetCachedPrevDayLow()` (~line 51-52), add the getters and a setter:

```cpp
    float GetCachedPrevDayHigh() const { return m_dailyCache.prevDayHigh; }
    float GetCachedPrevDayLow() const { return m_dailyCache.prevDayLow; }
    float GetCachedValueAreaHigh() const { return m_dailyCache.valueAreaHigh; }
    float GetCachedValueAreaLow() const { return m_dailyCache.valueAreaLow; }
    void SetCachedValueArea(float valueAreaHigh, float valueAreaLow) {
        m_dailyCache.valueAreaHigh = valueAreaHigh;
        m_dailyCache.valueAreaLow = valueAreaLow;
    }
```

- [ ] **Step 4: Add the day-gated aggregation block in `TripleScreen3.cpp`**

Add `#include "VolumeProfileEngine.h"` near the top of `src/TripleScreen3.cpp` alongside its existing includes. Then, near the existing "Layer B: session-aware Amihud percentile sampling" block (~line 762, which uses the same day/bar-gating idiom this mirrors), add a new block:

```cpp
    // === Real Volume Profile Value Area: aggregate yesterday's TS3 bars once
    // per trading-day change (docs/superpowers/plans/2026-08-04-volume-profile-daily-bias.md) ===
    {
        const int currentTradingDay = sc.BaseDateTimeIn[sc.Index].GetDate();
        int& lastValueAreaDay = sc.GetPersistentInt(PersistentVar_AdaptiveCalculators::LAST_VALUE_AREA_SAMPLE_INDEX);
        if (currentTradingDay != lastValueAreaDay && sc.Index >= 1) {
            lastValueAreaDay = currentTradingDay;

            const int todayFirstIndex = sc.GetFirstIndexForDate(sc.ChartNumber, currentTradingDay);
            if (todayFirstIndex > 0) {
                const int priorTradingDay = sc.BaseDateTimeIn[todayFirstIndex - 1].GetDate();
                const int yesterdayFirstIndex = sc.GetFirstIndexForDate(sc.ChartNumber, priorTradingDay);
                const int yesterdayLastIndex = todayFirstIndex - 1;

                if (yesterdayFirstIndex >= 0 && yesterdayLastIndex >= yesterdayFirstIndex && sc.VolumeAtPriceForBars != nullptr) {
                    // RTH-only (09:30-16:00 ET), per lbrnet/logs/rc_gemini.log
                    // GEMINI_REVIEW_077 §2: Market Profile theory is built on
                    // RTH volume, and this codebase already keeps the exact
                    // same RTH/overnight split for the Amihud percentile pools
                    // a few lines below (the "Layer B" block) for the same
                    // reason -- thin overnight/Globex volume would otherwise
                    // dilute the Value Area into something unnaturally wide.
                    // sc.HMS_TIME() is deprecated (scdatetime.h:985) -- use
                    // SCDateTime(Hour, Minute, Second, Millisecond).GetTime()
                    // directly. TimeOfDayIndicator is NOT reused here: it's a
                    // stateful object tracking the CURRENT bar, and calling
                    // its SetFromDateTime() for each historical bar in this
                    // loop would overwrite live state other code reads.
                    static const int kRthOpenTime = SCDateTime(9, 30, 0, 0).GetTime();
                    static const int kRthCloseTime = SCDateTime(16, 0, 0, 0).GetTime();

                    std::vector<vpe::PriceVolume> levels;
                    for (int barIndex = yesterdayFirstIndex; barIndex <= yesterdayLastIndex; ++barIndex) {
                        const int barTime = sc.BaseDateTimeIn[barIndex].GetTime();
                        if (barTime < kRthOpenTime || barTime >= kRthCloseTime) {
                            continue;  // overnight/Globex bar -- excluded from the Value Area
                        }
                        const unsigned int numLevels = sc.VolumeAtPriceForBars->GetSizeAtBarIndex(barIndex);
                        for (unsigned int i = 0; i < numLevels; ++i) {
                            const s_VolumeAtPriceV2* vap = nullptr;
                            if (sc.VolumeAtPriceForBars->GetVAPElementAtIndex(barIndex, i, &vap) && vap != nullptr) {
                                levels.push_back({vap->PriceInTicks, vap->Volume});
                            }
                        }
                    }

                    const vpe::ValueArea va = vpe::ComputeValueArea(levels, 70.0);
                    if (va.valid && sc.TickSize > 0.0) {
                        indMgr.SetCachedValueArea(
                            static_cast<float>(va.valueAreaHighInTicks * sc.TickSize),
                            static_cast<float>(va.valueAreaLowInTicks * sc.TickSize)
                        );
                    } else {
                        indMgr.SetCachedValueArea(0.0f, 0.0f);
                        Logger::getInstance().log(
                            "TripleScreen3: Volume Profile Value Area aggregation produced no valid "
                            "result for trading day " + std::to_string(priorTradingDay) +
                            " — falling back to the range-split proxy for today."
                        );
                    }
                } else {
                    indMgr.SetCachedValueArea(0.0f, 0.0f);
                }
            }
        }
    }
```

`indMgr` is the existing `IndicatorManager` reference already in scope in this function (used two lines above `prevDayHigh`'s declaration at line 575) — no new reference needs to be obtained for this block.

- [ ] **Step 5: Pass the cached real Value Area into `CalculateDailyBias`**

Change the `CalculateDailyBias` call (lines 682-687) from:

```cpp
        dailyBiasIndicator->Update(CalculateDailyBias(
            sc.Close[sc.Index],
            prevDayHigh,
            prevDayLow,
            Subgraph_HurstExponent[sc.Index],
            Subgraph_PathEfficiencySNR[sc.Index]
        ));
```

to:

```cpp
        dailyBiasIndicator->Update(CalculateDailyBias(
            sc.Close[sc.Index],
            prevDayHigh,
            prevDayLow,
            Subgraph_HurstExponent[sc.Index],
            Subgraph_PathEfficiencySNR[sc.Index],
            indMgr.GetCachedValueAreaLow(),
            indMgr.GetCachedValueAreaHigh()
        ));
```

- [ ] **Step 6: Full rebuild**

Run: `./build_dll.sh --no-clean`
Expected: `Build Successful`. If `sc.GetFirstIndexForDate`, `sc.VolumeAtPriceForBars`, `c_VAPContainer::GetSizeAtBarIndex`/`GetVAPElementAtIndex`, or `s_VolumeAtPriceV2::PriceInTicks`/`Volume` don't match this plan's assumed signatures, this is where it will surface — re-check them against `/mnt/c/SierraChart2/ACS_Source/sierrachart.h` and `VAPContainer.h` directly and adjust.

- [ ] **Step 7: Manual verification via Sierra Chart replay**

Confirm Task 1's prerequisite is actually in effect, then load the DLL in Sierra Chart and run a short historical replay through `BackTesterStudy.cpp`. Temporarily add a `Logger::getInstance().log(...)` near the `CalculateDailyBias` call to confirm `GetCachedValueAreaLow/High()` return non-zero, plausible ES price levels once a new trading day starts (not `0.0f`, not equal to `prevDayLow`/`prevDayHigh`). Remove the temporary log line afterward.

- [ ] **Step 8: Commit**

```bash
git add include/MindfulTraderConstants.h include/IndicatorManager.h src/TripleScreen3.cpp
git commit -m "feat(daily-bias): wire real Volume-at-Price Value Area into Daily Bias classification"
```

---

### Task 6: Documentation

**Files:**
- Modify: `docs/ADR/sierra_chart_data_feed_setup.md` ("Not yet done" section)
- Modify: `docs/GUI_INDICATOR_REFERENCE.md:416-424` (`daily_bias` entry)

- [ ] **Step 1: Update the ADR's "Not yet done" section**

In `docs/ADR/sierra_chart_data_feed_setup.md`, replace:

```markdown
## Not yet done

- Package 11 has been subscribed to/active, but the Volume Profile replacement (the low-hanging-fruit item) has not been scoped as an actual implementation task (no TDD plan, no test file) — this doc serves as the research/decision record, not an implementation spec.
```

with:

```markdown
## Not yet done

- ~~Package 11 has been subscribed to/active, but the Volume Profile replacement...~~ **Scoped and implemented** — `docs/superpowers/plans/2026-08-04-volume-profile-daily-bias.md`. Two corrections the plan made to this doc's original research: (1) `StructureEngine.cpp`'s `GetRecurrenceRate()` (the second proxy this doc flagged) turned out to be dead code with zero live call sites — only `CalculateDailyBias()`'s Value Area proxy was actually swapped. (2) The swap did **not** end up needing Package 11's chart-visible Volume Profile study at all — ACSIL exposes `sc.VolumeAtPriceForBars`/`sc.MaintainVolumeAtPriceData` directly, letting the plan aggregate the previous day's real Value Area from TS3's own bars without adding any study to the chart.
```

- [ ] **Step 2: Update the `daily_bias` GUI doc entry**

In `docs/GUI_INDICATOR_REFERENCE.md`, remove or update the stale `⚠️ WARNING` line under `### 22. daily_bias` (line 424) — this plan's research (`IndicatorManager.cpp:542-622`'s `SyncFeatureVector`) found `daily_bias` **is** populated at feature-vector index 25 via `dailyBiasIndicator->intValue()`, contradicting the doc's claim it's "NOT found in raw data." If this doc's warning refers to a different wire path than that 29D feature vector, note which one instead of leaving an unqualified warning.

- [ ] **Step 3: Commit**

```bash
git add docs/ADR/sierra_chart_data_feed_setup.md docs/GUI_INDICATOR_REFERENCE.md
git commit -m "docs: mark Volume Profile Value Area swap implemented, correct daily_bias transmission note"
```
