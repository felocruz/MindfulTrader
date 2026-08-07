---
domain: cpp/indicator_manager
intent: IndicatorManager's hybrid DOD/OOD architecture — which reads go through the packed SoA arrays, which stay on the legacy IndicatorStore/BaseIndicator/Indicator<T> hierarchy, and why the hierarchy can never be fully deleted
scope: global
tags: [indicator-manager, dod, soa, packed-array, indicator-key, indicator-layout, storage-block, get-value, check-trigger, hybrid-architecture, not-packed]
source_files:
  - include/IndicatorManager.h
  - src/IndicatorManager.cpp
  - include/IndicatorLayout.h
  - include/IndicatorPackedState.h
  - include/Indicator.h
  - src/Indicator.cpp
last_verified: 2026-08-06
dependencies: []
---

# IndicatorManager — Hybrid DOD/OOD Architecture

## Why This Exists

`IndicatorManager` was originally a hand-written heterogeneous `IndicatorStore` (~44
differently-typed named members) plus a `std::array<BaseIndicator*, MAX_INDICATORS>`
pointer-index array, with virtual dispatch (`ShouldTrigger()`, `ExtractInt8AndClearDirty()`,
`AddToTrainingEventFB()`) driving every hot-path read. This caused a live crash in
`OSCILLATOR_310`'s virtual dispatch and was the target of the `indicator-manager-dod-soa`
migration plan (2026-08-05, `docs/superpowers/plans/2026-08-05-indicator-manager-dod-soa.md`).

**The plan's original goal — delete the entire legacy hierarchy — proved infeasible
mid-flight**, not from lack of effort but from two structural facts discovered by a full
call-site inventory after Task 10:
1. `GetValue<Key>()` is **read-only**. ~40 call sites in `TripleScreen1/2/3.cpp` use
   `GetIndicator<T>(key)` to call **mutator** methods (`Update()`, `SetOHLC()`,
   `SetMetrics()`, `SetFromChart()`...) — the producer/write side. There is no packed-array
   substitute for a write call; replacing these would mean extracting every remaining
   indicator's compute logic into free functions (the pattern `Macd`/`ComputeMacd` already
   demonstrates) and having Triple Screen call `SetValue<Key>(ComputeX(...))` directly — a
   full rewrite of ~30 indicators, not a call-site swap.
2. ~18 call sites read `IndicatorKey`s that `kIndicatorLayout` deliberately marks
   `StorageBlock::NotPacked` — `ZN_TREND`/`DX_TREND`, `CORR_*_DELTA`/`CORR_*_ACCEL`,
   `HURST_EXPONENT`, `LONG_MKT_ACTION`/`SHORT_MKT_ACTION`, `VWAP`, `OVERNIGHT_EXIT` — these
   have no live `IndicatorState` schema field, so there is no packed slot to migrate to.

The end-state is therefore an **intentional, permanent hybrid**, not a partially-finished
migration: packed arrays are canonical for hot-path reads; the legacy hierarchy stays as
the write-side engine and the `NotPacked` read path. This matches the design spec's own
"true DOD while maintaining OOD goodness" framing.

---

## The Invariant / Contract

**Before touching any `GetIndicator<T>(key)` call site, classify it first:**

| Call site does... | Migratable to `GetValue<>()`? |
|---|---|
| Calls a mutator (`Update`, `SetX`, `SetFromChart`...) | **No** — write-only, permanent |
| Reads a key `kIndicatorLayout` marks `NotPacked` | **No** — no packed slot exists |
| Reads a key with a real row, but the row has **no write side wired** | **No** — see Failure Modes below, this is the most dangerous case |
| Reads a key with a real, wired row (`Int8` and/or `Float32`) | **Yes** |

`IndicatorManager` remains the sole facade — nothing outside it reads/writes `m_packed`
directly. Leaf `Indicator<T>` objects hold raw `int8_t*`/`float*` pointers directly into
`m_packed` (wired once, in the constructor) and write through them on `Update()` — this is
the plan's own designed dual-write mechanism, not a violation of the facade rule.

---

## How It Works

**`include/IndicatorLayout.h`** — `kIndicatorLayout`: a `constexpr std::array<IndicatorDescriptor, 62>` mapping every `IndicatorKey` to `{StorageBlock::Int8 | Float32 | NotPacked, position}`. Some keys have two rows (an `Int8` classification enum + a `Float32` "quality"/"norm"/"correlation" companion). `DescriptorFor(key, block)` is the explicit, always-unambiguous lookup; `UniqueDescriptorFor(key)` is the convenience form that only succeeds for genuinely single-row keys.

**`include/IndicatorPackedState.h`** — the flat arrays themselves (`m_currentI8`/`m_prevI8`/`m_currentF32`/`m_prevF32` + a dirty bitmask), position-based and key-agnostic. `m_prevI8`/`m_prevF32` are load-bearing — 9 trigger families read `GetPrevI8()` for entered/exited edge detection; never remove them.

**Reading (once you've confirmed the key is genuinely wired):**
```cpp
// Single-row key:
auto v = indMgr.GetValue<IndicatorKey::RSI>();
// Two-row key — MUST use the explicit block form, single-key form fails a static_assert:
auto cls = indMgr.GetValue<IndicatorKey::INTERM_MACD, mts::StorageBlock::Int8>();
auto norm = indMgr.GetValue<IndicatorKey::INTERM_MACD, mts::StorageBlock::Float32>();
```

**Writing** happens two ways depending on whether the call site is inside `IndicatorManager` itself or in a producer file:
- Inside `IndicatorManager.cpp` (e.g. `SetValue<Key>(v)` / `SetValue<Key,Block>(v)`) — direct packed write.
- In `TripleScreen1/2/3.cpp` (the normal case) — `indMgr.GetIndicator<Macd>(IndicatorKey::LONG_MACD)->SetFromChart(...)`. `Indicator<T>::Update()` dual-writes into the packed slot via the raw pointer set up in the constructor; the legacy object itself is still the thing that owns the compute logic.

**One irreducible API gap:** `INTERM_IMP` has two rows in the **same** block (`Int8` @12 classification, `Int8` @26 the `impulse_run_length` companion). `DescriptorFor(key, block)` disambiguates only by `(key, block)`, so it always resolves to the first match (position 12) — position 26 has no `GetValue<>()` path at all. `impulse_run_length` is read via `m_store.interm_imp.RunLength()` directly, permanently, for this reason.

**Devirtualized hot paths** (no virtual dispatch, no `m_indicators[]` iteration): `CheckTrigger`, `PopulateIndicatorState`, `GetTrainingEventT`. `EventSerializer.cpp` and `BackTesterStudy.cpp`'s exports read the packed arrays for every key that's actually wired.

---

## Failure Modes

**FM-01 (Critical, already happened once in this exact codebase): a `kIndicatorLayout` row existing is NOT the same claim as "safe to read via `GetValue<>()` today."** Two positions have a real row but **no write side wired anywhere**: `INTERM_IMP` Int8 position 26 (`impulse_run_length`) and `INTERM_MACD` Float32 position 8 (`interm_macd_norm` — constructor comment: "intentionally NOT wired... deferred to Task 9/10", which came and went without wiring it). `GetValue<>()` on either silently returns 0 forever — no compile error, no crash, just wrong data. This exact bug shipped, was caught in code review, reverted, and had to be proactively excluded from two more tasks before the migration was done. **Before adding any new `GetValue<>()` call, grep `AssertPackedStateParity()` and the constructor's `SetPackedSlotPointer`/`RawF32Pointer` wiring to confirm a write side actually exists for that exact `(key, block)` pair** — do not infer it from the layout table alone.

**FM-02: dropping a null-check can silently change control flow, not just remove dead code.** `GetValue<>()` never returns null, so `if (ptr) { read-only-use }` guards are safe to drop when migrating. But some null-checks gate more than a read — e.g. `RiskManager.cpp`'s `if (!znTrend || !dxTrend) return Result<void>::Success();` is a fail-open policy decision (allow the trade when correlation data isn't ready), not a null-safety guard. Removing structural guards silently changes behavior.

**FM-03: `m_prevF32` is currently dormant.** No production code writes it (no `SetValue<Key, Float32>` call site does the current→prev shift) and nothing reads `GetPrevF32()`. The first float-based entered/exited trigger that reads it will get a permanently-zero value — the same trap class as FM-01, just not sprung yet.

**FM-04: `PRIMARY_TRIGGER_MASK` membership and its writer must move together.** `IndicatorKey::SIDE` was left in `PRIMARY_TRIGGER_MASK`/`CheckTrigger` after its legacy push-sites were deleted (Task 10), orphaning the trigger silently — the *value* was still correct (read live from `PositionManager::GetTradeSide()`), but the dirty bit that used to drive prompt `Event` publication on a trade-side flip had no writer left. Caught only by a whole-branch review, not any per-task review. Fixed by tracking `m_lastKnownSide` and setting the bit directly in `UpdateBarContext()`.

---

## References
- `docs/superpowers/plans/2026-08-05-indicator-manager-dod-soa.md` — full plan, including the "Revised end-state" section documenting the mid-flight rescope and Global Constraints
- `include/IndicatorLayout.h` — `kIndicatorLayout`, `DescriptorFor`/`UniqueDescriptorFor`, extensive per-row provenance comments
- `include/IndicatorManager.h:153-199` — `GetValue`/`SetValue` template forms
- `src/IndicatorManager.cpp:24-52` — `IndicatorKeyMask`, `PRIMARY_TRIGGER_MASK`
- `src/IndicatorManager.cpp:200-340` (constructor) — the ground truth for which packed positions actually have a write side wired
