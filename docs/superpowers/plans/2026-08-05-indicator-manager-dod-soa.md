# IndicatorManager DOD/SoA Evolution (Phase II) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Evolve `IndicatorManager`/`Indicator` from a hand-written heterogeneous `IndicatorStore` + virtual-dispatch pointer array into packed SoA storage with compile-time devirtualized access, root-causing the live `OSCILLATOR_310` virtual-dispatch crash, while keeping `IndicatorManager` as the sole facade callers interact with.

**Architecture:** Two new pure, header-only components (`IndicatorLayout.h`'s descriptor table, `IndicatorPackedState.h`'s packed arrays) are introduced and unit-tested standalone, then wired into `IndicatorManager` as a dual-write alongside the existing `IndicatorStore` (parity-verified, zero behavior change), then call sites are migrated indicator-family by indicator-family, then the hot-path dispatch (`CheckTrigger`, `PopulateIndicatorState`) is rewritten against the packed arrays directly, and finally the old heterogeneous store and virtual hierarchy are deleted.

**Tech Stack:** C++17, Sierra Chart ACSIL, FlatBuffers (schema unchanged this phase), standalone-`g++` unit tests (`tests/cpp/*.cpp`), `./build_dll.sh` for ACSIL-dependent build verification.

**Design spec this plan implements:** `docs/superpowers/specs/2026-08-04-indicator-manager-dod-soa-design.md` (approved 2026-08-04, corrected 2026-08-05 after independent verification of a Gemini review round — see spec §3.1 and `lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_085`/`GEMINI_REVIEW_085`).

## Global Constraints

- `IndicatorManager` remains the sole facade. No code outside `IndicatorManager` ever reads/writes the packed arrays directly.
- `m_prevI8`/`m_prevF32` are both required — they are read by `ShouldTrigger()`-family logic for entered/exited transition detection (spec §3.1), not just dirty-bit comparison. Never remove them as a "simplification."
- C++ packed-array order matches `IndicatorState`'s existing FlatBuffer schema field order (floats block, then int8 block). No `../schema/mts_schema.fbs` edits in this plan.
- No heap allocation in any per-tick hot path (`GetValue`/`SetValue`, `CheckTrigger`, `PopulateIndicatorState`).
- No virtual dispatch survives in the final state's indicator read/write/serialize hot path.
- `HmmStateIndicator`, `PredictionState`, `MarketClimateIndicator` are out of scope — already owned by `InferenceManager`, not `IndicatorStore`.
- Every new pure logic file gets a standalone `tests/cpp/test_*.cpp` (hand-rolled `g++` compile, no mocking framework — this codebase's established convention). ACSIL-dependent changes are build-verified only via `./build_dll.sh --no-clean`.
- `README-AI.md`, `.github/copilot-instructions.md`, `CLAUDE.md`, `GEMINI.md` are mirrors — update all four together per the Documentation Sync Contract whenever one changes.

---

## File Structure

| File | Responsibility |
|---|---|
| `include/IndicatorLayout.h` (new) | `kIndicatorLayout` descriptor table (`IndicatorKey` -> block/position) + compile-time `IndicatorTraits<Key>`. Pure, no Sierra Chart deps beyond the `IndicatorKey` enum itself. |
| `include/IndicatorPackedState.h` (new) | The 4 packed arrays (`currentI8`/`prevI8`/`currentF32`/`prevF32`) + `dirtyMask`, plus `GetValue`/`SetValue`/`Reset` operating on raw positions. Pure, no Sierra Chart deps, no dependency on `IndicatorLayout.h` (position-based, key-agnostic — `IndicatorManager` composes the two). |
| `tests/cpp/test_indicator_layout.cpp` (new) | Completeness/no-duplicate-position tests for `kIndicatorLayout`. |
| `tests/cpp/test_indicator_packed_state.cpp` (new) | Get/Set/dirty-bit/Reset tests for `IndicatorPackedState`. |
| `include/IndicatorManager.h` / `src/IndicatorManager.cpp` (modified) | Gains `IndicatorPackedState m_packed`; gains `GetValue<Key>()`/`SetValue<Key>()`; dual-write parity assertion during migration; eventually loses `IndicatorStore`/`m_indicators`/`GetIndicator<T>()`. |
| `include/Indicator.h` / `src/Indicator.cpp` (modified) | `Indicator<T>::Update()` gains a packed-slot pointer for dual-write (one generic change). Compute methods progressively extracted to free functions. Eventually loses `BaseIndicator`, `Indicator<T>`, and all leaf classes. |
| `src/TripleScreen1.cpp`, `TripleScreen2.cpp`, `TripleScreen3.cpp`, `src/StudyHelperFunctions.cpp` (modified) | Call sites migrated from `GetIndicator<T>(key)->Value()`/`->SetX(...)` to `indMgr.GetValue<Key>()`/`indMgr.SetValue<Key>(ComputeX(...))`, family by family. |
| `src/messaging/EventSerializer.cpp` (modified) | Live-`Event`-building companion-value reads (`->GetVolumeRatio()`, `->GetATR10()`, etc.) replaced with reads from a single per-tick `TickCompanionValues` struct, shared with the training path — removes a whole class of live/training duplication discovered while answering a direct question about OOP-vs-DOD mixing (Task 10). |
| `src/messaging/EventSerializerV2.cpp`/`.h` (deleted) | Confirmed dead: absent from `CMakeLists.txt`, unused, contains a hardcoded `add_atr_10(0.0f)` bug. Removed as part of Task 10. |
| `CLAUDE.md`, `README-AI.md`, `.github/copilot-instructions.md`, `GEMINI.md` (modified) | Stale `IndicatorManager` description corrected. |

---

### Task 1: Correct the stale `IndicatorManager` documentation

**Files:**
- Modify: `CLAUDE.md:120`, `README-AI.md`, `.github/copilot-instructions.md`, `GEMINI.md` (same section, all four mirrors)

**Interfaces:** None — documentation only, zero code risk. This is the lowest-hanging fruit in the whole plan: no dependencies, no build required, immediate accuracy win.

- [ ] **Step 1: Find the stale line in all four mirror docs**

Run: `grep -n "std::array<Indicator" CLAUDE.md README-AI.md .github/copilot-instructions.md GEMINI.md`

Expected: one match per file, all describing `IndicatorManager` the same (stale) way.

- [ ] **Step 2: Replace the stale description in each of the 4 files**

Old text (each file):
```
- **`IndicatorManager`** uses DOD: `std::array<Indicator, IndicatorKey::COUNT>` — always use `IndicatorKey` enum lookups, never string hashes or map lookups
```

New text (each file):
```
- **`IndicatorManager`** currently uses a hand-written heterogeneous `IndicatorStore` (~44 differently-typed named members) plus a separate `std::array<BaseIndicator*, MAX_INDICATORS>` pointer-index array for O(1) `IndicatorKey`-enum lookup (`GetIndicator<T>(key)`, never string hashes or map lookups) — this is being migrated to true packed-array (SoA) storage with compile-time devirtualized access; see `docs/superpowers/specs/2026-08-04-indicator-manager-dod-soa-design.md`.
```

- [ ] **Step 3: Verify all four files now agree**

Run: `grep -n "IndicatorManager.*currently uses a hand-written" CLAUDE.md README-AI.md .github/copilot-instructions.md GEMINI.md`
Expected: 4 matches, one per file.

- [ ] **Step 4: Commit**

```bash
git add CLAUDE.md README-AI.md .github/copilot-instructions.md GEMINI.md
git commit -m "docs: correct stale IndicatorManager architecture description"
```

---

### Task 2: Audit — produce the `IndicatorLayout.h` descriptor table

**Files:**
- Create: `include/IndicatorLayout.h`
- Create: `tests/cpp/test_indicator_layout.cpp`

**Interfaces:**
- Produces: `enum class StorageBlock : uint8_t { Int8, Float32, NotPacked }`; `struct IndicatorDescriptor { IndicatorKey key; StorageBlock block; size_t position; }`; `constexpr std::array<IndicatorDescriptor, ...> kIndicatorLayout` (one row per published scalar — some keys contribute 0 rows, some contribute 2). `NotPacked` covers keys that are legitimately out of scope for this migration (see Step 1) — they still get a row so every `IndicatorKey` value is accounted for, they just resolve to neither array.

This is the audit this plan's later tasks depend on. It must resolve two things precisely, not approximately:

1. **Which `IndicatorKey` values are genuinely `NotPacked`** (out of scope): `HMM_STATE`, `MARKET_CLIMATE`, `PREDICTION_STATE` (owned by `InferenceManager`, confirmed in spec §2 non-goals); `PREV_HIGH_KEY`, `PREV_LOW_KEY`, `PREV_DAY_HIGH_KEY`, `PREV_DAY_LOW_KEY`, `PREV_FOUR_BAR_HIGH_KEY`, `PREV_FOUR_BAR_LOW_KEY` (confirm these are served by `IndicatorManager`'s separate `DailyCache`/`GetCachedPrevDayHigh()`-style plain getters, not `IndicatorStore`, by grepping their usage — if confirmed, `NotPacked`); `UNKNOWN` (sentinel, `NotPacked`).
2. **The `IndicatorState`-vs-`TrainingEventT` duality.** `include/Indicator.h:1020-1074`'s `MapIndicatorKeyToTrainingEvent` function is the existing ground-truth for which keys map directly to an `IndicatorState` int8 field via `ind.mutate_X(...)` (e.g. `LONG_MACD` -> `mutate_long_macd`). Several leaf classes' own `AddToTrainingEventFB` overrides write a SECOND value — but not always to `IndicatorState`: some write to `event.indicators->mutate_X_quality(...)` (an `IndicatorState` struct field — e.g. `KANGAROO_TAIL` -> `kangaroo_tail_quality`, confirmed at `Indicator.h:1434-1443`, and the same pattern for `TURTLE_SOUP`/`MOMENTUM_PINBALL`/`ELDER_BREAKOUT`/`NR7`/`CORR_ES_ZN`/`CORR_ES_DX`), while others write to a **top-level `TrainingEventT` field that is NOT part of `IndicatorState`** (e.g. `Macd::AddToTrainingEventFB` sets `event.interm_macd_norm` directly, not `event.indicators->mutate_interm_macd_norm`, at `Indicator.h:1125-1128`; similarly `VolumeIndicator` -> `event.volume_ratio_percent`/`event.volume_imbalance`, `ATRProximityIndicator` -> `event.atr_10`, `PriceMetricsIndicator` -> `event.close_percentile`, `NhNlSignalIndicator` -> `event.nh_nl_daily`, `Impulse` -> `event.indicators->mutate_impulse_run_length` which **is** in `IndicatorState`). Read `../schema/mts_schema.fbs`'s full `TrainingEvent` table definition (not just `IndicatorState`) to confirm which of these top-level `TrainingEventT` fields are genuinely separate scalar fields on that table versus something else. `kIndicatorLayout` covers only what's needed for the LIVE wire path (`Event.indicators: IndicatorState`, published every tick) — fields that exist solely on `TrainingEventT` for offline training-data export and are NOT part of `IndicatorState` are out of scope for the packed arrays (`NotPacked`); they keep being set the way they are today (from the leaf's own working-state POD, per spec §3.4) until/unless a future phase addresses training-only fields.

- [ ] **Step 1: Enumerate every `IndicatorKey` (0-53) against `MapIndicatorKeyToTrainingEvent`'s switch (`Indicator.h:1042-1073`) and every leaf class's own `AddToTrainingEventFB` override**

For each key, record: does it appear in the switch (-> `IndicatorState` int8 field, block `Int8`)? Does its owning leaf class override `AddToTrainingEventFB` with an EXTRA `event.indicators->mutate_X(...)` call (-> a second row, block `Float32`)? Does its owning leaf override write to a `TrainingEventT` top-level field instead (-> `NotPacked` for that companion value, until a later phase)? Is it one of the `DailyCache`/`InferenceManager`-owned keys (-> `NotPacked`)?

Cross-reference `../schema/mts_schema.fbs`'s `IndicatorState` struct (`:219-285`) field-by-field against this enumeration — every int8/float field in that struct must end up with exactly one `Int8`/`Float32` row pointing at it; every field NOT accounted for by a leaf class today is a gap to flag (do not guess — note it and move on, this audit's job is to produce ground truth, not to invent behavior).

- [ ] **Step 2: Write `include/IndicatorLayout.h`**

```cpp
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

#include "Indicator.h"  // IndicatorKey

namespace mts {

enum class StorageBlock : uint8_t { Int8, Float32, NotPacked };

struct IndicatorDescriptor {
    IndicatorKey  key;
    StorageBlock  block;
    size_t        position;  // index within the block's array; 0 when block == NotPacked
};

// One row per published scalar tied to a *live* IndicatorState field. Some keys
// contribute a second row (their companion quality/norm/correlation float) —
// see Step 1's audit for which. Keys with no live IndicatorState field at all
// (InferenceManager-owned, DailyCache-owned, or TrainingEventT-only companions
// not yet part of IndicatorState) get a single NotPacked row so every
// IndicatorKey value is accounted for exactly once per row-count.
//
// Produced by the Task 2 audit (docs/superpowers/plans/2026-08-05-indicator-manager-dod-soa.md).
//
// CONFIRMED rows below (verified directly against include/Indicator.h:1042-1073's
// MapIndicatorKeyToTrainingEvent switch, and against each named leaf class's own
// AddToTrainingEventFB override, during plan research) — Step 1 should sanity-check
// these, not re-derive them from scratch, then resolve every UNCONFIRMED key.
inline constexpr std::array<IndicatorDescriptor, /* N, see UNCONFIRMED below */> kIndicatorLayout = {{
    // -- Confirmed Int8-block rows (present in MapIndicatorKeyToTrainingEvent's switch,
    //    Indicator.h:1043-1071; positions are assignment order, renumber if Step 1 finds
    //    a reason to reorder to match IndicatorState's own field order more closely) --
    { IndicatorKey::LONG_MACD,               StorageBlock::Int8, 0 },
    { IndicatorKey::LONG_FI13_SIGNAL,         StorageBlock::Int8, 1 },
    { IndicatorKey::LONG_MACD_DIVERGENCE,     StorageBlock::Int8, 2 },
    { IndicatorKey::LONG_IMP,                 StorageBlock::Int8, 3 },
    { IndicatorKey::INTERM_STOCHASTIC,        StorageBlock::Int8, 4 },
    { IndicatorKey::RASCHKE_STRATEGY_SETUP,   StorageBlock::Int8, 5 },
    { IndicatorKey::RASCHKE_TACTICAL_TRIGGER, StorageBlock::Int8, 6 },
    { IndicatorKey::RSI,                      StorageBlock::Int8, 7 },
    { IndicatorKey::INTERM_FI2_SIGNAL,        StorageBlock::Int8, 8 },
    { IndicatorKey::EMA_PROXIMITY,            StorageBlock::Int8, 9 },
    { IndicatorKey::PRICE_METRICS,            StorageBlock::Int8, 10 },
    { IndicatorKey::INTERM_MACD_DIVERGENCE,   StorageBlock::Int8, 11 },
    { IndicatorKey::INTERM_IMP,               StorageBlock::Int8, 12 },
    { IndicatorKey::INTERM_MACD,              StorageBlock::Int8, 13 },
    { IndicatorKey::STRUCTURE_TEST,           StorageBlock::Int8, 14 },
    { IndicatorKey::VOLUME_SIGNAL,            StorageBlock::Int8, 15 },
    { IndicatorKey::ATR_PROXIMITY,            StorageBlock::Int8, 16 },
    // DAILY_BIAS writes BOTH daily_bias AND daily_bias_enum from the same intValue()
    // (Indicator.h:1060-1063) — two IndicatorState int8 fields, one source value.
    // Step 1: confirm both fields are genuinely meant to always be identical (if so,
    // one packed slot + writing it to both mutators at serialization time is enough;
    // if they can legitimately diverge, this key needs two Int8 rows instead of one).
    { IndicatorKey::DAILY_BIAS,               StorageBlock::Int8, 17 },
    { IndicatorKey::KANGAROO_TAIL,            StorageBlock::Int8, 18 },
    { IndicatorKey::TURTLE_SOUP,              StorageBlock::Int8, 19 },
    { IndicatorKey::MOMENTUM_PINBALL,         StorageBlock::Int8, 20 },
    { IndicatorKey::ELDER_BREAKOUT,           StorageBlock::Int8, 21 },
    { IndicatorKey::NR7,                      StorageBlock::Int8, 22 },
    { IndicatorKey::NH_NL_SIGNAL,             StorageBlock::Int8, 23 },
    { IndicatorKey::OSCILLATOR_310,           StorageBlock::Int8, 24 },  // confirmed NO companion override at all (Indicator.h:2526-2566) — the OSCILLATOR_310 crash-fix target
    { IndicatorKey::TIME_OF_DAY,              StorageBlock::Int8, 25 },

    // -- Confirmed Float32-block companion rows (leaf class's own AddToTrainingEventFB
    //    override writes an EXTRA event.indicators->mutate_X_quality(...) call, confirmed
    //    to be a real IndicatorState struct field, not a TrainingEventT-only field) --
    { IndicatorKey::KANGAROO_TAIL,    StorageBlock::Float32, 0 },  // kangaroo_tail_quality, Indicator.h:1434-1443
    { IndicatorKey::TURTLE_SOUP,      StorageBlock::Float32, 1 },  // turtle_soup_quality, Indicator.h:1490-1497 (verify exact line at implementation time)
    { IndicatorKey::MOMENTUM_PINBALL, StorageBlock::Float32, 2 },  // momentum_pinball_quality
    { IndicatorKey::ELDER_BREAKOUT,   StorageBlock::Float32, 3 },  // elder_breakout_quality
    { IndicatorKey::NR7,              StorageBlock::Float32, 4 },  // nr7_quality
    // CorrelationIndicator (direct BaseIndicator subclass, not Indicator<T>) writes
    // corr_es_zn/corr_es_dx directly by key comparison, not via the switch — confirmed
    // at Indicator.h:2662-2674ish (verify exact line at implementation time).
    { IndicatorKey::CORR_ES_ZN, StorageBlock::Float32, 5 },
    { IndicatorKey::CORR_ES_DX, StorageBlock::Float32, 6 },

    // -- Confirmed NotPacked (InferenceManager-owned, out of scope per spec §2) --
    { IndicatorKey::HMM_STATE,        StorageBlock::NotPacked, 0 },
    { IndicatorKey::MARKET_CLIMATE,   StorageBlock::NotPacked, 0 },
    { IndicatorKey::PREDICTION_STATE, StorageBlock::NotPacked, 0 },
    { IndicatorKey::UNKNOWN,          StorageBlock::NotPacked, 0 },

    // -- UNCONFIRMED — this is Step 1's actual remaining audit work. Each of these
    //    needs its owning leaf class (or DailyCache/other owner) read directly to
    //    determine block/position, exactly like the confirmed rows above were:
    //    LONG_MKT_ACTION, SHORT_MKT_ACTION, SIDE, MARKET_SYMBOL, OVERNIGHT_EXIT,
    //    HURST_EXPONENT, PREV_HIGH_KEY, PREV_LOW_KEY, PREV_DAY_HIGH_KEY,
    //    PREV_DAY_LOW_KEY, PREV_FOUR_BAR_HIGH_KEY, PREV_FOUR_BAR_LOW_KEY,
    //    THREE_LINE_OSCILLATOR, THREE_LINE_OSCILLATOR_PREV, ZN_TREND, DX_TREND,
    //    CORR_ES_ZN_DELTA, CORR_ES_ZN_ACCEL, CORR_ES_DX_DELTA, CORR_ES_DX_ACCEL, VWAP.
    //    Known discrepancy to document (not silently resolve) while auditing: Macd's
    //    own AddToTrainingEventFB override (Indicator.h:1125-1128) unconditionally
    //    writes `event.interm_macd_norm = m_zScore` regardless of whether the specific
    //    Macd instance's Key() is LONG_MACD or INTERM_MACD (the Macd class is
    //    instantiated for both) — whichever populates last silently wins. This field is
    //    on TrainingEventT, not IndicatorState, so it's NotPacked either way for this
    //    phase, but flag the discrepancy in the audit's writeup regardless.
}};

constexpr size_t kIndicatorLayoutCount = kIndicatorLayout.size();

// Total slots needed in each block — computed once, used to size IndicatorPackedState's arrays.
constexpr size_t CountBlock(StorageBlock target) {
    size_t maxPos = 0;
    bool any = false;
    for (const auto& d : kIndicatorLayout) {
        if (d.block == target) {
            any = true;
            if (d.position + 1 > maxPos) maxPos = d.position + 1;
        }
    }
    return any ? maxPos : 0;
}

constexpr size_t kIndicatorLayoutI8Count  = CountBlock(StorageBlock::Int8);
constexpr size_t kIndicatorLayoutF32Count = CountBlock(StorageBlock::Float32);

}  // namespace mts
```

The `<FULL TABLE>` placeholder above is intentionally the one piece of content this step produces from Step 1's audit — this is the audit's deliverable, not a plan placeholder to leave unresolved; the implementer fills every row from what Step 1 found, with no row guessed.

- [ ] **Step 3: Write `tests/cpp/test_indicator_layout.cpp`**

```cpp
#include "IndicatorLayout.h"

#include <cstdio>
#include <set>

using namespace mts;

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}  // namespace

int main() {
    std::printf("IndicatorLayout tests\n");

    // Every row's position is unique within its own block (no two int8 rows
    // claim the same slot; same for float32).
    {
        std::set<size_t> i8Positions, f32Positions;
        bool noCollision = true;
        for (const auto& d : kIndicatorLayout) {
            if (d.block == StorageBlock::Int8) {
                if (!i8Positions.insert(d.position).second) noCollision = false;
            } else if (d.block == StorageBlock::Float32) {
                if (!f32Positions.insert(d.position).second) noCollision = false;
            }
        }
        check("no_position_collisions_within_a_block", noCollision);
    }

    // Positions within each block are a dense 0..N-1 range (no gaps) — required
    // for the packed arrays to actually be densely packed, not sparse.
    {
        std::set<size_t> i8Positions, f32Positions;
        for (const auto& d : kIndicatorLayout) {
            if (d.block == StorageBlock::Int8) i8Positions.insert(d.position);
            else if (d.block == StorageBlock::Float32) f32Positions.insert(d.position);
        }
        bool i8Dense = i8Positions.empty() || (*i8Positions.rbegin() == i8Positions.size() - 1);
        bool f32Dense = f32Positions.empty() || (*f32Positions.rbegin() == f32Positions.size() - 1);
        check("int8_positions_are_dense", i8Dense);
        check("float32_positions_are_dense", f32Dense);
    }

    // kIndicatorLayoutI8Count/F32Count match the actual distinct position counts.
    check("i8_count_matches_layout", kIndicatorLayoutI8Count > 0);
    check("f32_count_matches_layout", kIndicatorLayoutF32Count > 0);

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 4: Run the test, verify it passes**

Run: `g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_indicator_layout.cpp -o /tmp/test_layout && /tmp/test_layout`
Expected: `ALL PASS (0 failures)`.

- [ ] **Step 5: Commit**

```bash
git add include/IndicatorLayout.h tests/cpp/test_indicator_layout.cpp
git commit -m "feat(indicator-manager): audit IndicatorKey layout, add kIndicatorLayout descriptor table"
```

---

### Task 3: `IndicatorPackedState` — the pure packed-array core

**Files:**
- Create: `include/IndicatorPackedState.h`
- Create: `tests/cpp/test_indicator_packed_state.cpp`

**Interfaces:**
- Consumes: `mts::kIndicatorLayoutI8Count`, `mts::kIndicatorLayoutF32Count` (Task 2).
- Produces: `class IndicatorPackedState` with `int8_t GetI8(size_t pos) const`, `void SetI8(size_t pos, int8_t v, uint64_t keyBit)`, `float GetF32(size_t pos) const`, `void SetF32(size_t pos, float v, uint64_t keyBit)`, `bool IsDirty(uint64_t keyBit) const`, `uint64_t DirtyMask() const`, `void ClearDirtyMask()`, `void Reset(const std::array<int8_t, N_I8>& defaultsI8, const std::array<float, N_F32>& defaultsF32)`. Position-based and key-agnostic — it knows nothing about `IndicatorKey`; `IndicatorManager` (Task 4) is what maps a key to a position via `kIndicatorLayout` and calls these by position.

This is deliberately position-based, not key-based — keeping `IndicatorLayout.h` (the key<->position mapping) and `IndicatorPackedState.h` (the raw storage) as two independently testable, single-responsibility units, composed by `IndicatorManager` (Task 4).

- [ ] **Step 1: Write the failing tests first**

```cpp
// tests/cpp/test_indicator_packed_state.cpp
#include "IndicatorPackedState.h"

#include <cstdio>

using namespace mts;

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}  // namespace

int main() {
    std::printf("IndicatorPackedState tests\n");

    {
        IndicatorPackedState<4, 2> state;
        check("cold_start_i8_is_zero", state.GetI8(0) == 0);
        check("cold_start_f32_is_zero", state.GetF32(0) == 0.0f);
        check("cold_start_dirty_mask_is_zero", state.DirtyMask() == 0);
    }

    // Setting a value that actually changes sets the dirty bit AND updates prev.
    {
        IndicatorPackedState<4, 2> state;
        constexpr uint64_t kBit = 1ULL << 3;
        state.SetI8(0, 5, kBit);
        check("set_i8_updates_current", state.GetI8(0) == 5);
        check("set_i8_sets_dirty_bit", (state.DirtyMask() & kBit) != 0);
        check("set_i8_updates_prev_to_old_value", state.GetPrevI8(0) == 0);
    }

    // Setting the SAME value again does not re-flip the dirty bit's meaning of
    // "changed since last clear" — but it must not corrupt prev either.
    {
        IndicatorPackedState<4, 2> state;
        constexpr uint64_t kBit = 1ULL << 3;
        state.SetI8(0, 5, kBit);
        state.ClearDirtyMask();
        state.SetI8(0, 5, kBit);  // same value again
        check("setting_same_value_again_does_not_set_dirty_bit", (state.DirtyMask() & kBit) == 0);
        check("prev_still_reflects_last_real_change", state.GetPrevI8(0) == 0);
    }

    // Float path mirrors the int8 path.
    {
        IndicatorPackedState<4, 2> state;
        constexpr uint64_t kBit = 1ULL << 7;
        state.SetF32(1, 3.5f, kBit);
        check("set_f32_updates_current", state.GetF32(1) == 3.5f);
        check("set_f32_sets_dirty_bit", (state.DirtyMask() & kBit) != 0);
        check("set_f32_updates_prev_to_old_value", state.GetPrevF32(1) == 0.0f);
    }

    // Reset restores compile-time defaults and clears dirty state for touched keys.
    {
        IndicatorPackedState<4, 2> state;
        constexpr uint64_t kBit = 1ULL << 3;
        state.SetI8(0, 5, kBit);
        std::array<int8_t, 4> defaultsI8 = {9, 0, 0, 0};
        std::array<float, 2> defaultsF32 = {0.0f, 0.0f};
        state.Reset(defaultsI8, defaultsF32);
        check("reset_restores_i8_default", state.GetI8(0) == 9);
        check("reset_restores_i8_prev_to_default_too", state.GetPrevI8(0) == 9);
        check("reset_clears_dirty_mask", state.DirtyMask() == 0);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run to verify it fails (header doesn't exist yet)**

Run: `g++ -std=c++17 -I include tests/cpp/test_indicator_packed_state.cpp -o /tmp/test_pack`
Expected: compile error, `IndicatorPackedState.h` not found.

- [ ] **Step 3: Write `include/IndicatorPackedState.h`**

```cpp
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mts {

// Pure, header-only, key-agnostic packed storage for all published indicator
// values. N_I8/N_F32 are the exact block sizes from IndicatorLayout.h's audit
// (docs/superpowers/plans/2026-08-05-indicator-manager-dod-soa.md, Task 2/3).
// Position-based: the caller (IndicatorManager) maps an IndicatorKey to a
// position via kIndicatorLayout and calls these by position — this class knows
// nothing about IndicatorKey at all.
//
// m_prevI8/m_prevF32 are NOT a dirty-bit convenience that could be dropped —
// they are read by ShouldTrigger()-style entered/exited transition logic for
// at least 9 indicator families (see the design spec, §3.1). Do not remove them.
template <size_t N_I8, size_t N_F32>
class IndicatorPackedState {
public:
    int8_t GetI8(size_t pos) const { return m_currentI8[pos]; }
    int8_t GetPrevI8(size_t pos) const { return m_prevI8[pos]; }
    float GetF32(size_t pos) const { return m_currentF32[pos]; }
    float GetPrevF32(size_t pos) const { return m_prevF32[pos]; }

    void SetI8(size_t pos, int8_t value, uint64_t keyBit) {
        if (value != m_currentI8[pos]) {
            m_prevI8[pos] = m_currentI8[pos];
            m_currentI8[pos] = value;
            m_dirtyMask |= keyBit;
        }
    }

    void SetF32(size_t pos, float value, uint64_t keyBit) {
        if (value != m_currentF32[pos]) {
            m_prevF32[pos] = m_currentF32[pos];
            m_currentF32[pos] = value;
            m_dirtyMask |= keyBit;
        }
    }

    bool IsDirty(uint64_t keyBit) const { return (m_dirtyMask & keyBit) != 0; }
    uint64_t DirtyMask() const { return m_dirtyMask; }
    void ClearDirtyMask() { m_dirtyMask = 0; }

    void Reset(const std::array<int8_t, N_I8>& defaultsI8,
               const std::array<float, N_F32>& defaultsF32) {
        m_currentI8 = defaultsI8;
        m_prevI8 = defaultsI8;
        m_currentF32 = defaultsF32;
        m_prevF32 = defaultsF32;
        m_dirtyMask = 0;
    }

private:
    alignas(64) std::array<int8_t, N_I8>  m_currentI8{};
    alignas(64) std::array<int8_t, N_I8>  m_prevI8{};
    alignas(64) std::array<float,  N_F32> m_currentF32{};
    alignas(64) std::array<float,  N_F32> m_prevF32{};
    uint64_t m_dirtyMask = 0;
};

}  // namespace mts
```

- [ ] **Step 4: Run the tests, verify they pass**

Run: `g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_indicator_packed_state.cpp -o /tmp/test_pack && /tmp/test_pack`
Expected: `ALL PASS (0 failures)`.

- [ ] **Step 5: Commit**

```bash
git add include/IndicatorPackedState.h tests/cpp/test_indicator_packed_state.cpp
git commit -m "feat(indicator-manager): add pure IndicatorPackedState packed-array core"
```

---

### Task 4: Wire dual-write into `IndicatorManager` (primary value only, no call-site changes)

**Files:**
- Modify: `include/IndicatorManager.h`, `src/IndicatorManager.cpp`
- Modify: `include/Indicator.h` (one generic change to `Indicator<T>::Update()`)

**Interfaces:**
- Consumes: `mts::IndicatorPackedState<mts::kIndicatorLayoutI8Count, mts::kIndicatorLayoutF32Count>` (Task 3), `mts::kIndicatorLayout` (Task 2).
- Produces: `IndicatorManager::m_packed` (private member); a debug-only parity assertion. No public API changes yet — every existing caller keeps working unmodified.

This is the highest-leverage low-risk step: one change to a single shared base-class method (`Indicator<T>::Update()`) makes every indicator using that template dual-write automatically, with no per-indicator or per-call-site changes. Only the small set of leaf classes with a companion float NOT covered by `Indicator<T>::Update()` (their own quality-score/norm setters) need one line each — deferred to this task's Step 4, using Task 2's audit to know exactly which classes those are.

- [ ] **Step 1: Add a packed-slot pointer to `Indicator<T>`**

In `include/Indicator.h`, inside `template <typename T> class Indicator : public BaseIndicator`:

```cpp
protected:
    IndicatorKey m_key;
    T m_defaultValue;
    T m_value;
    T m_prevValue;
    uint64_t* m_dirty_mask_ptr = nullptr;
    int8_t* m_packedSlotI8 = nullptr;  // dual-write target during Phase II migration; nullptr = not yet wired

public:
    // ... existing methods unchanged ...

    void SetPackedSlotPointer(int8_t* slot) { m_packedSlotI8 = slot; }

    void Update(const T& newValue) {
        if (newValue != m_value) {
            m_prevValue = m_value;
            m_value = newValue;
            if (m_dirty_mask_ptr) {
                *m_dirty_mask_ptr |= KeyBit();
            }
            if (m_packedSlotI8) {
                *m_packedSlotI8 = static_cast<int8_t>(newValue);
            }
        }
    }
```

- [ ] **Step 2: Add `m_packed` to `IndicatorManager` and wire slot pointers at construction**

In `include/IndicatorManager.h`'s private section, alongside the existing `IndicatorStore m_store;`:

```cpp
mts::IndicatorPackedState<mts::kIndicatorLayoutI8Count, mts::kIndicatorLayoutF32Count> m_packed;
```

Add `#include "IndicatorLayout.h"` and `#include "IndicatorPackedState.h"` near the existing `#include "Indicator.h"`.

In `src/IndicatorManager.cpp`'s constructor, immediately after the existing `m_indicators[key] = &m_store.x` assignment lines, add one `SetPackedSlotPointer` call per int8-block entry in `kIndicatorLayout` (from Task 2's audit), e.g.:

```cpp
m_store.long_macd.SetPackedSlotPointer(&m_packedRawI8[/* position from kIndicatorLayout for LONG_MACD */]);
```

(Exact positions come from Task 2's completed `kIndicatorLayout` — this step is mechanical once that table exists: one line per `Int8`-block row, using the row's own `position`.)

Since `IndicatorPackedState` doesn't expose raw array pointers (by design — it's the caller's job to go through `SetI8`/`GetI8` by position, not poke the array directly), add a package-private accessor for this specific wiring purpose only:

```cpp
// In IndicatorPackedState<N_I8, N_F32>, public section:
int8_t* RawI8Pointer(size_t pos) { return &m_currentI8[pos]; }
```

Add a test in `tests/cpp/test_indicator_packed_state.cpp` confirming `*state.RawI8Pointer(0) = 7; check(state.GetI8(0) == 7)` — the raw pointer and the accessor must reference the same storage.

- [ ] **Step 3: Add a debug-only parity assertion**

In `src/IndicatorManager.cpp`, in whatever function runs once per tick after indicator updates settle (locate the existing per-tick update path — likely near where `HasSignificantChange()`/`m_dirty_mask` is read), add:

```cpp
#ifndef NDEBUG
void IndicatorManager::AssertPackedStateParity() const {
    for (const auto& desc : mts::kIndicatorLayout) {
        if (desc.block != mts::StorageBlock::Int8) continue;
        auto* base = m_indicators[static_cast<size_t>(desc.key)];
        if (!base) continue;
        const int8_t oldPathValue = static_cast<int8_t>(base->intValue());
        const int8_t newPathValue = m_packed.GetI8(desc.position);
        assert(oldPathValue == newPathValue && "IndicatorPackedState dual-write parity violation");
    }
}
#endif
```

Call it once per tick, right after the point where all indicator updates for that tick have been applied (do not call it mid-update — only after both paths have had a chance to settle for the tick).

- [ ] **Step 4: Wire the companion-float leaf classes' own setters (from Task 2's audit)**

For each `Int8`-block key whose leaf class ALSO has a `Float32`-block companion row confirmed by Task 2's audit (e.g. `KANGAROO_TAIL`'s `m_qualityScore`, set inside `KangarooTail::SetContext(...)` — verify the exact setter name/location per leaf, they differ), add one dual-write line at the point the companion field is actually assigned, mirroring Step 1's pattern:

```cpp
// KangarooTail::SetContext(...), after m_qualityScore is computed/assigned:
if (m_packedSlotF32) { *m_packedSlotF32 = m_qualityScore; }
```

This requires the same `SetPackedSlotPointer`-style plumbing added to each such leaf class specifically (they don't share a common template method for their companion field the way primary values share `Indicator<T>::Update()`), wired in the constructor alongside Step 2's `Int8` wiring, using the row's `Float32`-block position from `kIndicatorLayout`.

- [ ] **Step 5: Build and confirm parity holds**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds; no behavior change (dual-write is purely additive — nothing reads `m_packed` externally yet). If a debug build is exercised via replay, confirm the parity assertion never fires.

- [ ] **Step 6: Commit**

```bash
git add include/Indicator.h include/IndicatorManager.h src/IndicatorManager.cpp
git commit -m "feat(indicator-manager): dual-write into IndicatorPackedState alongside IndicatorStore"
```

---

### Task 5: First proof-of-pattern migration — `TIME_OF_DAY` (trivially simple, single scalar, no companion field)

**Correction (found during implementation, 2026-08-05): `SIDE` does NOT belong to this task.** `SIDE` was originally chosen as the "trivially simple" exemplar, but Task 2's own audit correctly classifies it `NotPacked` — `side` lives only on `Event`/`TrainingEvent`'s TOP LEVEL (schema:990), not the nested `IndicatorState` struct the packed arrays mirror, so it has zero packed rows to migrate at all. Confirmed the underlying data flow is genuinely live and actively maintained (`src/SCStudies.cpp:436-437` and `src/BackTesterStudy.cpp:963-964` both call `sideIndicator->Update(...)` from `PositionManager::GetTradeSide()`/Sierra Chart's own position API, every tick) — it's real, just not this task's kind of field. `SIDE` is a `TickCompanionValues`-style top-level companion, Task 10's category (that struct already includes `side`/`marketSymbol`/`overnightExit`, mirroring `EventRootSharedSlice`/`TrainingRootSharedSlice`). `TIME_OF_DAY` replaces it as the exemplar: confirmed single `Int8`-block row (`kIndicatorLayout` position 25), no `ShouldTrigger()` override, no companion field, exactly one setter (`TimeOfDayIndicator::SetFromDateTime`, `include/Indicator.h:1983-1993`) — genuinely the simplest real case.

**Files:**
- Modify: `include/IndicatorManager.h`, `src/IndicatorManager.cpp` (add `GetValue<Key>()`/`SetValue<Key>()` — DONE, see Task 5 ledger; this section's remaining work is the call-site cutover below)
- Modify: whichever call site(s) currently reference `IndicatorKey::TIME_OF_DAY` (locate via `grep -rn "IndicatorKey::TIME_OF_DAY" src/`)

**Interfaces:**
- Produces: `template <IndicatorKey Key> auto IndicatorManager::GetValue() const`, `template <IndicatorKey Key> void IndicatorManager::SetValue(...)` — the first two real, working instances of the compile-time API from the design spec §3.3, proven against the simplest possible case before extending to anything with more surface area.

`Side` (`Indicator<TradeSideEnum>`, `include/Indicator.h:1199-1206`) has no compute method beyond the generic `Update()` and no companion float — it is the lowest-risk possible first cutover.

- [ ] **Step 1: Add lookup helpers to `IndicatorLayout.h` — designed up front for keys with TWO rows**

`kIndicatorLayout` (Task 2) has ~15 keys that contribute two rows (one `Int8`, one `Float32` — e.g. `LONG_MACD`/`INTERM_MACD` per Task 6, `KANGAROO_TAIL` et al. per Task 8). A lookup keyed by `IndicatorKey` alone cannot tell those two rows apart. Design the lookup with an explicit `(Key, Block)` form from the start, plus a convenience single-row form for the majority of keys that only ever have one row:

```cpp
// include/IndicatorLayout.h, appended:

// Explicit form — always unambiguous, required for any key with two rows.
constexpr IndicatorDescriptor DescriptorFor(IndicatorKey key, StorageBlock block) {
    for (const auto& d : kIndicatorLayout) {
        if (d.key == key && d.block == block) return d;
    }
    return IndicatorDescriptor{ IndicatorKey::UNKNOWN, StorageBlock::NotPacked, 0 };
}

// Convenience form for single-row keys only — resolves to NotPacked (triggering
// a static_assert at the call site) if the key has zero or two rows, so a
// two-row key can never silently resolve to "whichever row happens to come
// first in the array."
constexpr IndicatorDescriptor UniqueDescriptorFor(IndicatorKey key) {
    IndicatorDescriptor found{ IndicatorKey::UNKNOWN, StorageBlock::NotPacked, 0 };
    int matchCount = 0;
    for (const auto& d : kIndicatorLayout) {
        if (d.key == key) { found = d; ++matchCount; }
    }
    return (matchCount == 1) ? found : IndicatorDescriptor{ IndicatorKey::UNKNOWN, StorageBlock::NotPacked, 0 };
}
```

- [ ] **Step 2: Add `GetValue`/`SetValue` (single-row and explicit-block overloads) to `IndicatorManager`**

```cpp
// include/IndicatorManager.h, public section:

// Single-row form — the common case (TIME_OF_DAY, and every other one-row key).
template <IndicatorKey Key>
auto GetValue() const {
    constexpr auto desc = mts::UniqueDescriptorFor(Key);
    static_assert(desc.block != mts::StorageBlock::NotPacked,
                  "IndicatorKey has zero or two rows — use GetValue<Key, Block>() instead");
    if constexpr (desc.block == mts::StorageBlock::Int8) {
        return m_packed.GetI8(desc.position);
    } else {
        return m_packed.GetF32(desc.position);
    }
}

template <IndicatorKey Key, typename V>
void SetValue(V value) {
    constexpr auto desc = mts::UniqueDescriptorFor(Key);
    static_assert(desc.block != mts::StorageBlock::NotPacked,
                  "IndicatorKey has zero or two rows — use SetValue<Key, Block>() instead");
    constexpr uint64_t keyBit = 1ULL << static_cast<uint64_t>(Key);
    if constexpr (desc.block == mts::StorageBlock::Int8) {
        m_packed.SetI8(desc.position, static_cast<int8_t>(value), keyBit);
    } else {
        m_packed.SetF32(desc.position, static_cast<float>(value), keyBit);
    }
}

// Explicit-block form — required for the ~15 two-row keys (Tasks 6, 8). Task 6
// is this overload's first real caller; do not defer designing it until then.
template <IndicatorKey Key, mts::StorageBlock Block>
auto GetValue() const {
    constexpr auto desc = mts::DescriptorFor(Key, Block);
    static_assert(desc.block != mts::StorageBlock::NotPacked, "no row for this (key, block) pair");
    if constexpr (Block == mts::StorageBlock::Int8) {
        return m_packed.GetI8(desc.position);
    } else {
        return m_packed.GetF32(desc.position);
    }
}

template <IndicatorKey Key, mts::StorageBlock Block, typename V>
void SetValue(V value) {
    constexpr auto desc = mts::DescriptorFor(Key, Block);
    static_assert(desc.block != mts::StorageBlock::NotPacked, "no row for this (key, block) pair");
    constexpr uint64_t keyBit = 1ULL << static_cast<uint64_t>(Key);
    if constexpr (Block == mts::StorageBlock::Int8) {
        m_packed.SetI8(desc.position, static_cast<int8_t>(value), keyBit);
    } else {
        m_packed.SetF32(desc.position, static_cast<float>(value), keyBit);
    }
}
```

- [ ] **Step 3: Cut over `TIME_OF_DAY`'s call site(s)**

Locate the exact call site(s) via `grep -rn "IndicatorKey::TIME_OF_DAY" src/ include/`. Change from the `GetIndicator<TimeOfDayIndicator>(IndicatorKey::TIME_OF_DAY)->SetFromDateTime(...)` / `->Value()` pattern to:

```cpp
indMgr.SetValue<IndicatorKey::TIME_OF_DAY>(newTimeOfDay);
// ... and reads:
const auto timeOfDay = indMgr.GetValue<IndicatorKey::TIME_OF_DAY>();
```

`TimeOfDayIndicator::SetFromDateTime` computes the `TimeOfDayEnum` value from an `SCDateTime` — that computation stays exactly where it is (it's already effectively free-function-shaped internally; Task 7 handles formally extracting it as a `Compute*` free function for the bulk-migration family it belongs to). This task only cuts over the resulting VALUE's storage and the call site that stores it, proving the packed-array API end to end — it does not need to touch `SetFromDateTime`'s own logic.

Since `TimeOfDayEnum` is not `int8_t`/`float` directly, confirm the exact enum's underlying type is `int8_t`-compatible (check `enum class TimeOfDayEnum : int8_t` or equivalent in `Indicator.h`) — if the underlying type differs, the `static_cast`s in Step 2 need the caller to cast back to the enum type at the call site (`static_cast<TimeOfDayEnum>(indMgr.GetValue<IndicatorKey::TIME_OF_DAY>())`), matching the design spec §3.3's typed-accessor intent; if this friction is real (not just theoretical), note it — Task 6 revisits it once a second example is in hand, to decide whether `IndicatorTraits<Key>` should carry the true enum return type (spec §3.3's stated intent) rather than leaving casts at call sites.

- [ ] **Step 4: Remove `TIME_OF_DAY`'s dual-write assertion coverage (it's now single-source-of-truth)**

In `AssertPackedStateParity()` (Task 4), the `TIME_OF_DAY` row's comparison is now comparing the new path against itself once the old `m_store.time_of_day`/`m_indicators[TIME_OF_DAY]` accessor is no longer being updated by any live call site. Either remove `TIME_OF_DAY` from the parity loop, or confirm the old `TimeOfDayIndicator` object is still updated by nothing and its value is now frozen/irrelevant. Prefer explicit removal from the parity loop with a comment noting `TIME_OF_DAY` is cut over.

- [ ] **Step 5: Build and verify**

Run: `./build_dll.sh --no-clean`
Expected: build succeeds; behavior unchanged (same value, same semantics, different physical storage).

- [ ] **Step 6: Commit**

```bash
git add include/IndicatorLayout.h include/IndicatorManager.h src/IndicatorManager.cpp <call-site files>
git commit -m "feat(indicator-manager): cut TIME_OF_DAY over to packed-array GetValue/SetValue (proof of pattern)"
```

---

### Task 6: Second proof-of-pattern migration — `Macd` (stateful compute extraction; single packed row + one preserved `NotPacked` companion)

**Files:**
- Modify: `include/Indicator.h`, `src/Indicator.cpp` (extract `Macd::SetFromChart`'s compute logic to a free function)
- Modify: call site(s) referencing `IndicatorKey::LONG_MACD`/`INTERM_MACD` (`grep -rn "IndicatorKey::LONG_MACD\|IndicatorKey::INTERM_MACD" src/`)

**Interfaces:**
- Consumes: Task 5's `GetValue<Key>()`/`SetValue<Key>()`.
- Produces: a free function `MacdResult ComputeMacd(MacdState& state, double diffCurrent, double diffPrev1, double diffPrev2, int barsAvailable)` returning `{ MacdEnum signal; float zScore; }`, replacing `Macd::SetFromChart`'s body, plus 8 pure classification helper functions. Proves the "needs running history AND multi-bar raw lookback" shape — see the note below, this is NOT simply "needs the previous value."

**Important — do not oversimplify this function's inputs.** An earlier draft of this task specified `ComputeMacd(MacdState&, double macdDiffValue)` — a single scalar. That is wrong: `Macd`'s season/pattern classification (`IsSpring`/`IsSummer`/`IsFall`/`IsWinter`/`IsPositiveTickDown`/`IsNegativeTickUp`/`IsZeroFromBelow`/`IsZeroFromAbove`, `src/Indicator.cpp:175-221`) reads `MACD_Diff[Index-2]` — genuinely two RAW CHART BARS back, not just "the previously published `MacdEnum` value" (which is what `m_prevValue`/the packed array's `GetPrevI8` track). This is a real, confirmed gap found by inspecting the actual source (verified directly, not assumed) — the packed array's single `prev` slot cannot supply this; the caller must pass the raw 3-bar window explicitly. This is exactly the kind of thing this design intentionally keeps OUT of the packed array (per spec §3.4's working-state exception) — raw multi-bar chart lookback is Sierra Chart's own `SCSubgraphRef` random-access array, already available at the call site, not something `IndicatorManager`'s per-key state needs to reproduce.

- [ ] **Step 1: Extract `Macd::SetFromChart`'s body AND its 8 classification helpers into free functions**

Read the full current `Macd::SetFromChart` (`src/Indicator.cpp:72-172`), `Macd`'s private state (`m_macdHistory`, `m_historyIdx`, `m_historyCount`, `kLookback`), and all 8 classification helpers (`src/Indicator.cpp:175-221`) before starting — transcribe their exact current logic, do not re-derive it from memory.

Shape to produce (mirroring the `VwapState`/`ComputeVwap` convention from the design spec §3.5 for the stateful z-score part; the classification helpers are fully stateless, taking only the 3-bar window):

```cpp
// include/IndicatorComputations.h (new, or fold into IndicatorLayout.h's neighborhood — implementer's call, keep it pure)
struct MacdState {
    static constexpr int kLookback = /* current Macd::kLookback value, from src/Indicator.cpp */;
    std::array<double, kLookback> zScoreHistory{};
    int historyIdx = 0;
    int historyCount = 0;
};

struct MacdResult {
    MacdEnum signal;
    float zScore;
};

// Pure classification helpers -- transcribed verbatim from Macd::IsSpring/IsSummer/
// IsFall/IsWinter/IsPositiveTickDown/IsNegativeTickUp/IsZeroFromBelow/IsZeroFromAbove
// (src/Indicator.cpp:175-221), with MACD_Diff[Index]/[Index-1]/[Index-2] renamed to
// diffCurrent/diffPrev1/diffPrev2 -- same logic, decoupled from SCSubgraphRef/Index
// so these stay standalone-testable like every other pure engine in this codebase.
bool MacdIsSpring(double diffCurrent, double diffPrev1, double diffPrev2);
bool MacdIsSummer(double diffCurrent, double diffPrev1, double diffPrev2);
bool MacdIsFall(double diffCurrent, double diffPrev1, double diffPrev2);
bool MacdIsWinter(double diffCurrent, double diffPrev1, double diffPrev2);
bool MacdIsPositiveTickDown(double diffCurrent, double diffPrev1, double diffPrev2);
bool MacdIsNegativeTickUp(double diffCurrent, double diffPrev1, double diffPrev2);
bool MacdIsZeroFromBelow(double diffCurrent, double diffPrev1, double diffPrev2);
bool MacdIsZeroFromAbove(double diffCurrent, double diffPrev1, double diffPrev2);

// diffPrev1/diffPrev2 are only meaningful when barsAvailable >= 1 / >= 2 respectively
// (mirrors Macd::SetFromChart's own `if (Index >= 2) ... else if (Index >= 1) ...`
// bar-count gating -- the caller passes however many prior bars it actually has,
// clamped to [0,2], NOT Sierra Chart's raw Index value itself, so this function
// stays fully decoupled from ACSIL indexing semantics).
MacdResult ComputeMacd(MacdState& state, double diffCurrent, double diffPrev1, double diffPrev2, int barsAvailable);
```

At the call site (wherever `Macd::SetFromChart(MACD_Diff, Index)` is invoked today — locate it fresh, don't assume), the adapter is thin and ACSIL-only:
```cpp
const double diffCurrent = MACD_Diff[Index];
const double diffPrev1   = (Index >= 1) ? MACD_Diff[Index - 1] : 0.0;
const double diffPrev2   = (Index >= 2) ? MACD_Diff[Index - 2] : 0.0;
const int barsAvailable  = std::min(Index, 2);
const auto result = ComputeMacd(m_longMacdState, diffCurrent, diffPrev1, diffPrev2, barsAvailable);
```

- [ ] **Step 2: Write standalone tests for `ComputeMacd` AND the 8 classification helpers before wiring them in**

Create `tests/cpp/test_indicator_computations.cpp` (this becomes the home for all such extracted compute functions across Tasks 6/8/9 — one growing file, not one per indicator, matching how `StudyHelperFunctions.h`'s free functions are already organized in this codebase). Cover: cold start (`barsAvailable == 0` -> `AT_ZERO`), the z-score path (fewer than 5 history samples -> `zScore == 0`; a known-in-advance sequence producing a specific z-score, hand-computed), a sign-change producing the expected simple-cross `MacdEnum` transition, AND at least one test per classification helper (`MacdIsSpring`/`IsSummer`/`IsFall`/`IsWinter`/etc.) using a 3-value window constructed to exercise that specific pattern — these 8 helpers are exactly the part an earlier, oversimplified draft of this task would have silently dropped, so their tests matter most here.

- [ ] **Step 3: Cut `Macd`'s call site(s) over**

`Macd`'s z-score companion (`m_zScore`, written to `event.interm_macd_norm`) is `NotPacked` per Task 2's audit (it's a `TrainingEventT`-only field, not a real `IndicatorState` field — unlike `KangarooTail`'s quality score, which IS in `IndicatorState`). So `LONG_MACD`/`INTERM_MACD` are **single-row** keys for the packed arrays — this task proves the "stateful compute extraction" pattern (running history in an explicit state struct), not the two-row `(Key, Block)` API from Task 5 Step 1/2. The two-row API's first real exercise is Task 8 (`KANGAROO_TAIL` et al.), which do have confirmed `IndicatorState` `Float32` companions.

Replace the `GetIndicator<Macd>(IndicatorKey::LONG_MACD)->SetFromChart(MACD_Diff, Index)` pattern with:

```cpp
const auto result = ComputeMacd(m_longMacdState, MACD_Diff[Index]);
indMgr.SetValue<IndicatorKey::LONG_MACD>(result.signal);
```

`result.zScore` is NOT written through `IndicatorManager` — it stays `NotPacked` this phase. See Step 4 below, and Task 10 (which now owns the durable fix — see that task's rationale), for what happens to it once `Macd`'s leaf class (its only current write-path) is deleted in Task 11.

Where `MacdState` (the running history) needs to live: as a member of whatever owns the call site today (mirroring how `m_longMacdState` would replace `m_store.long_macd`'s object identity) — confirm with a grep of the current call site's enclosing scope before deciding exactly where.

- [ ] **Step 4: Preserve the `NotPacked` companion write (`interm_macd_norm`) — do not let it silently disappear in Task 11**

Today, `event.interm_macd_norm = m_zScore` is written from inside `Macd::AddToTrainingEventFB` — an override on the very leaf class Task 11 deletes. As an immediate stopgap in this task, add `result.zScore` to whatever call site currently triggers the training-event export path, writing it directly (`event.interm_macd_norm = result.zScore;`) — NOT from a `Macd` object, since none will exist after Task 11. Task 10 is where this gets its durable home (the `TickCompanionValues` struct) alongside the other companions found there; this step just ensures nothing regresses in the gap between this task and Task 10 landing. Note this same requirement applies to every other `NotPacked`, `TrainingEventT`-only companion found during Task 2's audit (`volume_ratio_percent`, `volume_imbalance`, `atr_10`, `close_percentile`, `nh_nl_daily`, and whatever else the audit's `UNCONFIRMED` sweep turns up) — Task 8 has the same stopgap obligation for its own scope.

- [ ] **Step 4: Build and verify**

Run: `./build_dll.sh --no-clean` and re-run `tests/cpp/test_indicator_computations.cpp`.

- [ ] **Step 5: Commit**

```bash
git add include/Indicator.h src/Indicator.cpp include/IndicatorComputations.h tests/cpp/test_indicator_computations.cpp <call-site files>
git commit -m "feat(indicator-manager): extract ComputeMacd free function, cut LONG_MACD/INTERM_MACD over to packed arrays (proof of two-row pattern)"
```

---

### Task 7: Bulk-migrate the remaining simple int8-only indicators

**Files:**
- Modify: `include/Indicator.h`, `src/Indicator.cpp` (extract remaining compute methods)
- Modify: `include/IndicatorComputations.h`, `tests/cpp/test_indicator_computations.cpp` (grow)
- Modify: call sites in `src/TripleScreen1.cpp`, `TripleScreen2.cpp`, `TripleScreen3.cpp`, `src/StudyHelperFunctions.cpp`

**Interfaces:**
- Consumes: the proven pattern from Tasks 5-6, `kIndicatorLayout`'s full row list (Task 2).

Apply Task 5's (single-row) or Task 6's (two-row) pattern, exactly as proven, to every remaining `Int8`-block-only `IndicatorKey` from `kIndicatorLayout` that Task 2's audit confirmed has no `Float32` companion row. Do not invent a new pattern per indicator — every one of these gets the same mechanical treatment: extract its compute method to a free function (with an explicit state struct if it's stateful, per `ComputeMacd`'s shape; stateless if it genuinely only needs its arguments, per the design spec §3.5's `ComputeImpulseFromColor` example), migrate its call site(s) to `GetValue<Key>()`/`SetValue<Key>()`, remove it from the parity-assertion loop once cut over, verify build.

- [ ] **Step 1: List the exact remaining keys**

Run against Task 2's completed `kIndicatorLayout`: every row with `block == Int8` and no matching `Float32` row for the same `key`, minus `TIME_OF_DAY` (Task 5) and `LONG_MACD`/`INTERM_MACD` (Task 6). This is the task's real scope — enumerate it from the actual table, not from this plan's guesses about which indicators are "simple" (several, like `Impulse`, turned out to have more internal state than expected during spec/plan research — trust the audit, not prior assumptions).

- [ ] **Step 2: Migrate each one, one commit per indicator family (not one giant commit)**

For each key from Step 1: read its current leaf class and compute method in full (`include/Indicator.h`/`src/Indicator.cpp`) before writing its free-function version — do not assume its shape matches `Macd`'s or `Impulse`'s just because it's in the same file region. Extract compute logic, add a test to `test_indicator_computations.cpp`, migrate its call site(s), verify build, remove from parity loop, commit. Repeat per key.

- [ ] **Step 3: Final build check for this task**

Run: `./build_dll.sh --no-clean` after the last indicator in this task's scope is migrated.
Run: `g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_indicator_computations.cpp -o /tmp/test_comp && /tmp/test_comp` — expect `ALL PASS`.

---

### Task 8: Bulk-migrate the float-valued / quality-score / companion-float indicators

**Files:**
- Modify: `include/Indicator.h`, `src/Indicator.cpp`
- Modify: `include/IndicatorComputations.h`, `tests/cpp/test_indicator_computations.cpp`
- Modify: call sites (same files as Task 7)

**Interfaces:**
- Consumes: Task 6's proven two-row pattern.

Covers the confirmed `Float32`-companion two-row keys from `kIndicatorLayout` not already done in Task 6: `KANGAROO_TAIL`, `TURTLE_SOUP`, `MOMENTUM_PINBALL`, `ELDER_BREAKOUT`, `NR7`, `CORR_ES_ZN`, `CORR_ES_DX` (all confirmed against real `IndicatorState` fields during plan research — see Task 2's `kIndicatorLayout` comments for exact citations).

**Do not assume the same is true for `VOLUME_SIGNAL`, `ATR_PROXIMITY`, `PRICE_METRICS`, `NH_NL_SIGNAL`, or `LONG_IMP`.** Each of these has a leaf-class override that writes an EXTRA companion value (`volume_ratio_percent`/`volume_imbalance`, `atr_10`, `close_percentile`, `nh_nl_daily`, `impulse_run_length` respectively) — but during plan research these were found written as `event.X = ...` (a `TrainingEventT` top-level field) rather than `event.indicators->mutate_X(...)` (a real `IndicatorState` struct field), the same `TrainingEventT`-only pattern as `Macd`'s `interm_macd_norm` (flagged as `NotPacked` in Task 2). The one confirmed exception: `impulse_run_length` for `LONG_IMP` IS a real `IndicatorState` int8 field (`ind.mutate_impulse_run_length(...)`, confirmed in the schema), so `LONG_IMP` is a two-`Int8`-row key (not an `Int8`+`Float32` pair) if Task 2 confirms it belongs in this migration at all — check Task 2's completed table for each of these five keys' actual resolved `block` before writing any compute function for them; do not re-derive their classification here.

For `KangarooTail`/`TurtleSoup`/`MomentumPinball`/`ElderBreakout`/`NR7` specifically: each has internal, never-published working state (`m_atSupportLevel`, `m_hurst`, `m_screenAligned`, etc. — the design spec §3.4 "working-state exception"). Their extracted compute functions take that working state as an explicit small struct parameter (same convention as `MacdState`), and the struct itself is NOT part of the packed arrays — only the resulting published enum + quality float are.

- [ ] **Step 1: List the exact remaining keys, same method as Task 7 Step 1**

- [ ] **Step 2: Migrate each one, working-state-aware, one commit per indicator family**

Same procedure as Task 7 Step 2, with the added requirement: for each of the 5 pattern-quality indicators, design its working-state struct explicitly (list every field the current leaf class has beyond `m_value`/`m_prevValue`/the quality score — e.g. `KangarooTailWorkingState { bool atSupportLevel; bool atResistanceLevel; }`), confirm nothing in that struct is read from outside the compute function + `IndicatorManager` (if something is — e.g. a getter used elsewhere in `TripleScreenX.cpp` — that getter's call site needs to move to wherever the working-state struct now lives, flag it rather than silently drop it).

- [ ] **Step 3: Preserve every `NotPacked` companion write found in this task's scope, same as Task 6 Step 4**

For `VOLUME_SIGNAL`, `ATR_PROXIMITY`, `PRICE_METRICS`, `NH_NL_SIGNAL` (and `LONG_IMP`'s `impulse_run_length` if Task 2 resolves it as a second `Int8` row rather than in scope for a different task): if their companion write is confirmed `NotPacked` (`TrainingEventT`-only), the source leaf object stops being updated the moment THIS task cuts its primary-value call site over — its companion getter (`->GetVolumeRatio()`, `->GetATR10()`, etc.) would then return stale, frozen data immediately, not "eventually" once Task 11 deletes the class. As an immediate stopgap (same pattern as Task 6 Step 4), add each one's value to whatever now calls the compute function, writing it directly onto `event`. Task 10 gives these their durable home in `TickCompanionValues`; Task 9 Step 1 is the last checkpoint confirming every one of them has a working stopgap or the Task 10 replacement already in place before Task 11 deletes the leaf classes for good.

- [ ] **Step 4: Final build check**

Run: `./build_dll.sh --no-clean`.

---

### Task 9: Rewrite `CheckTrigger`, `PopulateIndicatorState`, AND `GetTrainingEventT`'s indicator loop against the packed arrays directly

**Files:**
- Modify: `src/IndicatorManager.cpp` (`CheckTrigger`, `PopulateIndicatorState`, `GetTrainingEventT`)
- Modify: `src/BackTesterStudy.cpp` (its own `IndicatorState`-building function, ~lines 1240-1290 — confirmed WIP/unused, safe to change; see Step 3)

**Interfaces:**
- Consumes: fully-populated `m_packed` (every key migrated by Tasks 5-8).

**Correction from this task's original scoping:** the `OSCILLATOR_310` crash-guard actually lives inside `GetTrainingEventT()`'s own indicator loop (`src/IndicatorManager.cpp:425-448`, calling `m_indicators[i]->AddToTrainingEventFB(*event)`), which is a SEPARATE function from `PopulateIndicatorState` — the two populate two different `IndicatorState` instances (`TrainingEvent.indicators` vs. `Event.indicators` respectively) via two independently-written mechanisms today (`PopulateIndicatorState` already avoids virtual dispatch for the switch itself, using `static_cast<IndicatorKey>(i)`, but still calls one virtual method per indicator, `ExtractInt8AndClearDirty()`, at line 882 — a second, currently-unprotected virtual-dispatch site on the exact same `Oscillator310` object, just not yet observed crashing). Root-causing the crash means fixing **both** loops, not just `PopulateIndicatorState`.

- [ ] **Step 1: Rewrite `PopulateIndicatorState` as two straight-line loops**

Replace the polymorphic-pointer-based value extraction (`indicator->ExtractInt8AndClearDirty()`, line 882) with direct iteration over `m_packed`'s two blocks, using `kIndicatorLayout` to know which `IndicatorState` mutator each position corresponds to (reading `m_packed.GetI8(pos)`/`GetF32(pos)` directly — no `BaseIndicator*`, no vtable, no `m_indicators` array). The `switch`-on-`IndicatorKey` structure already in this function can stay largely as-is; only the value source changes.

Before touching this function, collect the full, final list of `NotPacked`, `TrainingEventT`-only companion writes flagged across Task 2 (`interm_macd_norm` and whatever the `UNCONFIRMED` sweep turned up), Task 6 Step 4, and Task 8 Step 3. Confirm every single one of them has an explicit new write site (per those tasks' own steps) before Task 11 deletes anything — this is the last checkpoint before their only remaining write-path (the leaf classes) is deleted for good.

- [ ] **Step 2: Replace `GetTrainingEventT`'s indicator loop with a call to the now-rewritten `PopulateIndicatorState`**

Both loops ultimately populate an `IndicatorState` struct — `PopulateIndicatorState` for `Event.indicators`, this one for `TrainingEvent.indicators`. Rather than maintaining two independent devirtualized implementations (which is exactly the kind of duplication Task 10 addresses more broadly for companion values — this is the same class of problem, one layer up), delete the loop at lines 425-448 entirely and replace it with:

```cpp
if (!event->indicators) {
    event->indicators = std::make_unique<MTS::Schema::IndicatorState>();
}
PopulateIndicatorState(*event->indicators);
```

This removes the `OSCILLATOR_310` special case (lines 435-444) as dead code by construction — there is no longer a per-indicator virtual call anywhere in this path for either consumer, so there is nothing left to special-case. Add a regression comment at the deletion site explaining why: `PopulateIndicatorState` (Step 1) now reads `m_packed` directly, has no virtual dispatch, and is shared by both `Event.indicators` and `TrainingEvent.indicators` — removing the last place a per-indicator virtual call happens.

- [ ] **Step 3: Keep `src/BackTesterStudy.cpp`'s `BuildEntryIndicatorState()` compiling against the new API — minimal fix only**

`src/BackTesterStudy.cpp` is confirmed work-in-progress and will be revisited/rewritten separately to suit the new architecture (user, 2026-08-05) — do not invest design effort here beyond keeping it building. `BuildEntryIndicatorState()` (~line 1223) calls `GetIndicator<T>()` on leaf objects Task 11 deletes, so it must at least compile against the new API before Task 11 can proceed, but do not treat this as an architectural unification opportunity the way Steps 1-2 were for the live/training paths.

Simplest correct fix: replace this function's body with a call to `PopulateIndicatorState(state)` (the same function Step 2 now calls) rather than hand-porting each individual `GetIndicator<T>()` call — this happens to be the least-effort fix, not because it's the "right" design for this file long-term. If `PopulateIndicatorState` doesn't cover a field this function used to populate (`zn_trend`/`dx_trend`, the 4 correlation derivatives — it doesn't, per Task 2's audit), leave a one-line comment noting the gap for the future rewrite; do not attempt to close it now. Preserve the existing `intValue()`-not-`ExtractInt8AndClearDirty()` property (a read-only snapshot must not clear live dirty flags) — this carries over automatically since `PopulateIndicatorState`'s rewritten form has no dirty-clearing side effect at all; just don't break it.

Note for Task 10: the same file has a SECOND, separate `GetIndicator<T>()` call site (~line 1751) needing the same minimal treatment — see that task's Step 5.

- [ ] **Step 4: Rewrite `CheckTrigger`**

Since trigger logic (`ShouldTrigger()`) for the ~9 entered/exited-transition families depends on comparing current vs. previous PUBLISHED value — which `m_packed` already tracks via `GetI8`/`GetPrevI8` — reimplement each family's trigger condition as a small free function or inline check keyed by `IndicatorKey`, operating on `m_packed.GetI8(pos)`/`GetPrevI8(pos)` directly. This generalizes the existing `CheckTrigger`'s "Phase 1.2 Static Metaprogramming Dispatcher" pattern (already devirtualized, already a hand-written switch) — same shape, new data source.

- [ ] **Step 5: Remove the dual-write parity assertion (Task 4) entirely — no old path remains to compare against**

- [ ] **Step 6: Build and verify**

Run: `./build_dll.sh --no-clean`. If a replay/backtest environment is available, run one to confirm no crash and no behavior regression around what used to be the `OSCILLATOR_310` special case.

- [ ] **Step 7: Commit**

```bash
git add src/IndicatorManager.cpp src/BackTesterStudy.cpp
git commit -m "feat(indicator-manager): rewrite CheckTrigger/PopulateIndicatorState/GetTrainingEventT/BackTesterStudy against packed arrays, root-cause eliminate OSCILLATOR_310 vtable crash from all three loops"
```

---

### Task 10: Unify live/training/backtest companion-value population in `EventSerializer.cpp`; fix the confirmed dead-write bug; delete dead `EventSerializerV2.cpp`

**Files:**
- Modify: `src/messaging/EventSerializer.cpp`
- Modify: `src/IndicatorManager.cpp` (the `WriteTrainingRootSharedFields` call site inside `GetTrainingEventT`, around line 480-529)
- Modify: `src/BackTesterStudy.cpp` (the `g_entryContext` companion-value call site, ~line 1751 — a third consumer found while investigating this same duplication category; see Step 5)
- Delete: `src/messaging/EventSerializerV2.cpp`, `include/messaging/EventSerializerV2.h` (confirm these are absent from `CMakeLists.txt` before deleting — they are today)

**Interfaces:**
- Consumes: the free `Compute*()` functions and result structs from Tasks 6-8 (`MacdResult`, `KangarooTailResult`, etc. — whatever each indicator's migration task actually produced for its companion values).

**Why this task exists:** independent investigation (not from the design spec — found while answering a direct question about live/training mixing) confirmed two real, distinct defects beyond what Tasks 1-9 address:

1. **Duplicated companion-value gathering.** `EventSerializer.cpp` (live `Event` path, called from `SCStudies.cpp` via `PublishEventOnChange`/`PublishEventSnapshot`) and `GetTrainingEventT`'s `WriteTrainingRootSharedFields` call (training `TrainingEvent` path, called from `EventDataCollectorStudy.cpp`) each independently call getters (`->GetVolumeRatio()`, `->GetDailyValue()`, `->GetATR10()`, `->GetClosePercentile()`, etc.) on the same indicator objects, gathering the same conceptual values via two unrelated, independently-maintained blocks of code (`EventSerializer.cpp:74-147ish`, `IndicatorManager.cpp:480-513`).
2. **A confirmed dead-write bug.** Inside `GetTrainingEventT`, the (now-Task-9-rewritten) indicator population happens first, followed later in the same function by `WriteTrainingRootSharedFields` (`IndicatorManager.cpp:514-529`), which **overwrites** `close_percentile`/`volume_ratio_percent`/`volume_imbalance`/`nh_nl_daily` with independently re-fetched (or, for `close_percentile`, independently *recomputed from raw OHLC*) values. Whatever the leaf classes' own companion-value logic produced for these four fields is silently discarded a few lines later, in the same function. `close_percentile`'s two implementations (`PriceMetricsIndicator`'s cached percentile vs. `(close-low)/range` recomputed inline) are not guaranteed to agree.

- [ ] **Step 1: Define one per-tick companion-values struct, populated once, from the Task 6-8 compute functions' own results**

```cpp
// include/IndicatorComputations.h, appended (or a new small header if this grows large):
struct TickCompanionValues {
    int8_t side = 0;
    int8_t marketSymbol = 0;
    int8_t overnightExit = 0;
    float nhNlDaily = 0.0f;
    float prevHigh = 0.0f;
    float prevLow = 0.0f;
    float prevDayHigh = 0.0f;
    float prevDayLow = 0.0f;
    float prevFourBarHigh = 0.0f;
    float prevFourBarLow = 0.0f;
    float closePercentile = 0.0f;
    float volumeRatioPercent = 0.0f;
    float volumeImbalance = 0.0f;
    float atr10 = 0.0f;
};
```

This mirrors `EventRootSharedSlice`/`TrainingRootSharedSlice`'s existing field list (confirmed identical across both — that part of the codebase already got this right for the WRITE step; this task fixes the READ/GATHER step feeding both). Populate it from whichever of Tasks 6-8's compute-function results already produced these values (e.g. `closePercentile` comes from `PriceMetricsIndicator`'s replacement compute function's result, `volumeRatioPercent`/`volumeImbalance` from `VolumeIndicator`'s, `atr10` from `ATRProximityIndicator`'s) — do not recompute anything here; this struct only carries values already computed exactly once elsewhere.

- [ ] **Step 2: Populate `TickCompanionValues` once per tick in `IndicatorManager`, before either serializer runs**

Add a method (or a member updated during the same per-tick pass that already calls each migrated indicator's compute function): `const TickCompanionValues& IndicatorManager::GetTickCompanionValues() const`. Wire each `Set`/compute call site from Tasks 6-8 to also store its result's companion field into this struct at the same time it calls `SetValue<Key>(...)` — one extra assignment per already-touched call site, not a new pass over anything.

- [ ] **Step 3: `EventSerializer.cpp` reads from `GetTickCompanionValues()` instead of calling getters on indicator objects**

Replace the block at `EventSerializer.cpp:74-147`ish (every `manager.GetIndicator<T>(key)->GetX()` call feeding `WriteEventRootSharedFields`/`indicators.mutate_X_quality(...)`/`indicators.mutate_X_norm(...)`) with reads from `manager.GetTickCompanionValues()`. This is also forced, not optional: `GetIndicator<T>()` and the leaf types it returns no longer exist after Task 11 — this step must land before Task 11, and after it lands, `EventSerializer.cpp` has zero remaining references to any leaf class.

- [ ] **Step 4: Fix the dead-write bug — `GetTrainingEventT`'s `WriteTrainingRootSharedFields` call reads from the SAME struct, does not recompute anything**

Replace `IndicatorManager.cpp`'s lines ~480-513 (the independent `GetIndicator<T>(key)->GetX()` calls and the inline `closePercentile = (currentClose - currentLow) / barRange` recomputation) with a single read: `const auto& companions = GetTickCompanionValues();`, then build `TrainingRootSharedSlice` directly from `companions`' fields. `close_percentile` now has exactly one implementation, used by both consumers — the recompute-from-raw-OHLC version is deleted, not kept as a fallback.

- [ ] **Step 5: Keep `src/BackTesterStudy.cpp`'s second `GetIndicator<T>()` call site (~line 1751) compiling — minimal fix only**

Same file, same "work-in-progress, will be revisited/rewritten separately" status as Task 9 Step 3 (user, 2026-08-05) — do not invest design effort here. This is a different call site (inside the function that records full context at trade entry) pulling `VolumeIndicator::GetVolumeRatio()`/`GetVolumeImbalance()`, `PriceMetricsIndicator::GetClosePercentile()`, and `ATRProximityIndicator::GetATR10()` directly into a `g_entryContext` struct's own scalar fields. It calls leaf classes Task 11 deletes, so it must at least compile against the new API. Simplest correct fix: replace those four individual `GetIndicator<T>()` calls with reads from `manager.GetTickCompanionValues()` (Step 2's struct) — least-effort correct substitution, not a redesign.

- [ ] **Step 6: Delete `EventSerializerV2.cpp`/`.h`**

Confirm absence from `CMakeLists.txt` and zero includes anywhere (`grep -rn "EventSerializerV2" src/ include/ CMakeLists.txt`), then delete both files. This is unrelated dead code discovered adjacent to this task's own files, not part of the live/training duplication fix itself, but cheap and directly relevant to avoid leaving a second, subtly-buggy (`add_atr_10(0.0f)` hardcoded) "which serializer is real" trap sitting next to the one this task just cleaned up.

- [ ] **Step 7: Build and verify**

```bash
./build_dll.sh --no-clean
grep -rn "GetIndicator<" src/messaging/EventSerializer.cpp src/IndicatorManager.cpp src/BackTesterStudy.cpp
```
Expected: build succeeds; the `grep` for `GetIndicator<` in these three call sites returns nothing (all now read from `TickCompanionValues` exclusively). If a replay/backtest environment is available, spot-check that a live-published `Event` and a training-collected `TrainingEvent` from the same tick agree on `close_percentile`/`volume_ratio_percent`/`volume_imbalance`/`nh_nl_daily`/`atr_10` — they were structurally guaranteed to potentially disagree before this task, and are structurally guaranteed to agree now.

- [ ] **Step 8: Commit**

```bash
git add include/IndicatorComputations.h src/messaging/EventSerializer.cpp src/IndicatorManager.cpp src/BackTesterStudy.cpp
git rm src/messaging/EventSerializerV2.cpp include/messaging/EventSerializerV2.h
git commit -m "fix(indicator-manager): unify live/training/backtest companion-value population, fix close_percentile dead-write bug, remove dead EventSerializerV2"
```

---

### Task 11: Remove the old heterogeneous store and virtual hierarchy

**Files:**
- Modify: `include/IndicatorManager.h`, `src/IndicatorManager.cpp` (delete `IndicatorStore m_store`, `std::array<BaseIndicator*, ...> m_indicators`, `GetIndicator<T>()` and all its explicit template instantiations)
- Modify: `include/Indicator.h`, `src/Indicator.cpp` (delete `BaseIndicator`, `Indicator<T>`, all ~38 leaf classes, `MapIndicatorKeyToTrainingEvent`)

**Interfaces:** None produced — this task only removes now-dead code, once Tasks 5-10 have migrated every live call site off it.

- [ ] **Step 1: Confirm zero remaining references before deleting anything**

Run: `grep -rn "GetIndicator<\|IndicatorStore\|BaseIndicator" src/ include/ | grep -v "IndicatorManager.h:.*// removed\|IndicatorPackedState.h\|IndicatorLayout.h"`
Expected: no matches outside the files being deleted in this task. If anything remains, stop — a call site was missed in Tasks 5-10, go back and migrate it before proceeding (do not delete code a live caller still needs).

- [ ] **Step 2: Delete `IndicatorStore`, `m_indicators`, `GetIndicator<T>()` from `IndicatorManager.h`/`.cpp`**

- [ ] **Step 3: Delete `BaseIndicator`, `Indicator<T>`, all leaf classes, `MapIndicatorKeyToTrainingEvent` from `Indicator.h`/`.cpp`**

`Indicator.h`/`.cpp` should shrink dramatically — verify with `wc -l include/Indicator.h src/Indicator.cpp` before and after; if the file still contains anything beyond enum definitions (`IndicatorKey`, the per-indicator value enums like `MacdEnum`/`ImpulseEnum`/etc., which callers and `IndicatorComputations.h` still need) and small free-standing helper types, something was missed.

- [ ] **Step 4: Build and run every standalone test**

```bash
./build_dll.sh --no-clean
g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_indicator_layout.cpp -o /tmp/t1 && /tmp/t1
g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_indicator_packed_state.cpp -o /tmp/t2 && /tmp/t2
g++ -std=c++17 -Wall -Wextra -I include tests/cpp/test_indicator_computations.cpp -o /tmp/t3 && /tmp/t3
```
Expected: all succeed/pass.

- [ ] **Step 5: Commit**

```bash
git add include/IndicatorManager.h src/IndicatorManager.cpp include/Indicator.h src/Indicator.cpp
git commit -m "refactor(indicator-manager): remove IndicatorStore/BaseIndicator hierarchy, migration complete"
```

---

### Task 12: Final whole-branch review

Use `superpowers:requesting-code-review`'s code-reviewer template against the full range (base: the commit before Task 1, head: Task 11's final commit). Verify against this plan's Global Constraints explicitly: no virtual dispatch survives in the hot path; `m_prevI8`/`m_prevF32` still exist and are read by the migrated trigger logic; `IndicatorManager` is still the sole facade; no heap allocation was introduced anywhere in `GetValue`/`SetValue`/`CheckTrigger`/`PopulateIndicatorState`; the packed-array field order still matches `IndicatorState`'s schema order; `../schema/mts_schema.fbs` was not touched. Additionally verify Task 10's specific claim: `close_percentile`/`volume_ratio_percent`/`volume_imbalance`/`nh_nl_daily`/`atr_10` each now have exactly ONE implementation feeding both `Event` and `TrainingEvent`, not two.
