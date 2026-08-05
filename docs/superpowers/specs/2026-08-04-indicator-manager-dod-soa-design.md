# IndicatorManager DOD/SoA Evolution (Phase II) — Design Spec

**Status:** Approved by user 2026-08-04. Ready for `superpowers:writing-plans`.

**Origin:** Gemini's `GEMINI_BRIEF_082` (`lbrnet/logs/rc_gemini.log`) proposed a `PackedIndicatorState` (three cache-aligned `int8_t[54]` arrays) as an indication of what's architecturally possible. This spec is the user's own design, arrived at through a full brainstorming session against the *actual* current codebase — Gemini's brief is a reference point, not a mandate, and this spec both narrows it (schema stays a `struct`, no benchmarking harness) and widens it (covers float-valued indicators too, not just int8-valued ones).

## 1. Why

### 1.1 CLAUDE.md's description of `IndicatorManager` is stale

`CLAUDE.md` currently claims: *"`IndicatorManager` uses DOD: `std::array<Indicator, IndicatorKey::COUNT>` — always use `IndicatorKey` enum lookups, never string hashes or map lookups."* This does not match the code:

- `Indicator` is a template (`Indicator<T>`, `include/Indicator.h:940`), not a concrete type — `std::array<Indicator, ...>` as literally written would not compile.
- There is no `IndicatorKey::COUNT`; the sentinel is `IndicatorKey::MAX_INDICATORS = 54` (`include/Indicator.h:74`).
- The actual storage is a hand-written heterogeneous struct, `IndicatorStore` (`include/IndicatorManager.h:140-198`), of ~44 differently-typed *named* member objects (each a small class deriving `Indicator<T>` or `BaseIndicator` directly) — this is OOD composition, not SoA — plus a **separate** `std::array<BaseIndicator*, MAX_INDICATORS> m_indicators` of raw pointers into that struct, populated by ~40 hand-written lines in the constructor (`src/IndicatorManager.cpp:124-185`). These two data structures are hand-synchronized by the author and must agree by construction — a real maintenance hazard this spec eliminates by design (§3.2).
- `GetIndicator<T>(key)` (`src/IndicatorManager.cpp:1094-1139`) is index-array + `static_cast`, genuinely devirtualized — no maps/hashes/strings anywhere in that specific path. But `PopulateIndicatorState`'s per-indicator training-event population loop (`src/IndicatorManager.cpp:425-448`) walks `m_indicators` and calls `m_indicators[i]->AddToTrainingEventFB(*event)` — real virtual dispatch, once per indicator, every time an event is built.

### 1.2 There is a live production crash tied to that virtual dispatch

`src/IndicatorManager.cpp:425-448` carries this comment:

> "Crash guard: production evidence repeatedly points to a fault during virtual dispatch at `OSCILLATOR_310` (index 26). Emit the same training field directly from owned storage."

The code special-cases index 26 to bypass the virtual call rather than fixing the underlying issue. **Eliminating this workaround at the root (not just special-casing it further) is an explicit, named success criterion of this spec**, not an incidental side effect.

### 1.3 `Indicator.h`/`Indicator.cpp` have grown large (2679 + 959 lines) for what they do

`BaseIndicator` (abstract, 8 pure virtual methods) + `Indicator<T>` (generic template base) + ~38 leaf classes, one per indicator or indicator family. Most leaf "compute" methods (`Impulse::SetFromColor`, `VwapIndicator::UpdateVwap`, `PriceMetricsIndicator::SetOHLC`) already mutate only a small, bounded set of `this`'s fields from a handful of explicit scalar parameters — they are structurally close to free functions already. One genuine free function already exists in this file (`RobustLogZ`, `src/Indicator.cpp:393`), establishing that the codebase already has a convention for hoisting pure math out of objects when there's no need for identity.

## 2. Goals and non-goals

**Goals:**
- Full Structure-of-Arrays storage for **all** published indicator values — both the ~30 simple int8-enum-valued indicators (Gemini's original scope) and the float-valued ones (quality scores, correlations) that Gemini's brief didn't cover.
- Eliminate virtual dispatch, and the `OSCILLATOR_310` crash workaround specifically, from the indicator read/write/serialize hot path.
- `IndicatorManager` remains the sole OOD-style facade other code interacts with — no caller outside `IndicatorManager` ever touches the packed arrays directly. Individual `Indicator` leaf **classes** are not preserved as a design goal; only `IndicatorManager`'s encapsulating role is.
- C++ packed array order mirrors `IndicatorState`'s existing FlatBuffer schema field grouping (floats block, then int8 block) exactly.
- Introduce free functions for indicator computation, converting existing near-free-function methods.
- A single source of truth for the `IndicatorKey` → storage-location mapping (replacing today's two hand-synchronized structures).

**Non-goals (explicitly deferred, not forgotten):**
- Converting `IndicatorState` from a FlatBuffer `struct` to a `table` with vector fields — deferred to a future version. This spec only reorders/groups the struct's *existing* fields to match the new C++ layout; the schema's field-by-field shape does not change.
- Measured performance benchmarking (cycle counts, cache-miss profiling). Success is verified architecturally (no vtables in the hot path, contiguous arrays, unrolled serialization, crash eliminated) — consistent with how this codebase already verifies ACSIL glue code (build-verified, not benchmarked).
- `HmmStateIndicator`, `PredictionState`, `MarketClimateIndicator` — already extracted to `InferenceManager` (per existing `// moved to InferenceManager (Mar 2026)` comments in `IndicatorManager.h`). Out of scope; this spec only touches what's still in `IndicatorStore`.
- Splitting `IndicatorKey` itself into typed sub-enums. The single flat enum stays exactly as-is at every call site; the block/position mapping lives entirely inside `IndicatorManager`.

## 3. Architecture

### 3.1 Storage

`IndicatorStore` and `std::array<BaseIndicator*, ...> m_indicators` are replaced by:

```cpp
// Published, dirty-tracked, schema-mirroring state — the "hot" core.
alignas(64) std::array<int8_t, N_I8>  m_currentI8;
alignas(64) std::array<int8_t, N_I8>  m_prevI8;
alignas(64) std::array<float,  N_F32> m_currentF32;
alignas(64) std::array<float,  N_F32> m_prevF32;
```

`N_I8` and `N_F32` are the counts of int8-valued and float-valued published indicators respectively (determined precisely during the Step-1 audit, §4).

**`m_prevI8`/`m_prevF32` are load-bearing, not just a dirty-bit convenience.** A design-review round with Gemini proposed dropping both `prev` arrays entirely, reasoning that only the current value and the fact-of-change (a dirty bit) are ever read downstream. Verified against the actual code and found false: `Indicator<T>::m_prevValue` (the current codebase's equivalent) is read directly, by value, inside at least 9 indicator families' `ShouldTrigger()` overrides to detect state *transitions* — not just "did it change," but "did it specifically enter or exit a named state this tick" — e.g. `include/Indicator.h:1162-1167` (Stochastic: entering/exiting overbought/oversold), `:1407-1408` (KangarooTail: entered/exited), `:1459-1460` (TurtleSoup), `:1516-1517` (MomentumPinball), `:1574-1575` (ElderBreakout), `:1655-1656` (NR7), `:1706-1711` (RSI), `:1831-1844` (ATRProximity), `:1869-1878` (EmaProximity). These are exactly the pattern-detection indicators this system exists to compute — KangarooTail, TurtleSoup, and MomentumPinball are Raschke-pattern triggers, not incidental fields. Dropping the `prev` arrays would silently break entered/exited trigger detection for all of them. `m_prevI8`/`m_prevF32` stay in the design as originally specified; the memory cost (well under 100 bytes total) is trivial next to that risk.

Default values (Gemini's proposed third array) are **not** per-instance storage — they never change at runtime, so they become a `static constexpr` table used only by `Reset()`. This removes one array (and one more thing that could drift out of sync) versus the literal 3-array `PackedIndicatorState` proposal.

### 3.2 The descriptor table — single source of truth

```cpp
enum class StorageBlock : uint8_t { Int8, Float32 };

struct IndicatorDescriptor {
    IndicatorKey  key;
    StorageBlock  block;
    size_t        position;   // index within m_currentI8/m_currentF32
};

constexpr std::array<IndicatorDescriptor, MAX_INDICATORS> kIndicatorLayout = {
    // one row per published scalar. An indicator with two published values
    // (e.g. KangarooTail's int8 trigger-state AND its float quality score)
    // contributes two rows sharing the conceptual key concept but distinct
    // positions in distinct blocks.
    ...
};
```

This single table is generated once, by hand, during the audit (§4), and is the *only* place that maps an `IndicatorKey` to a physical storage location — replacing today's two independently-hand-maintained structures (`IndicatorStore`'s field declarations and the constructor's `m_indicators[key] = &m_store.x` lines).

### 3.3 Two access paths, both devirtualized

**Compile-time (the common case).** Virtually every call site in `TripleScreenX.cpp`/`StudyHelperFunctions.cpp` references a specific, statically-known `IndicatorKey`. A template `IndicatorTraits<Key>` (generated from `kIndicatorLayout`) lets:

```cpp
indMgr.GetValue<IndicatorKey::LONG_MACD>();
indMgr.SetValue<IndicatorKey::LONG_MACD>(newValue);
```

resolve to exactly one array access at compile time — no branch, no loop, no vtable. This directly generalizes the `CheckTrigger` "Static Metaprogramming Dispatcher" already proven in this codebase (`include/IndicatorManager.h:135-138`, labeled "Phase 1.2" in existing comments) — not a new technique, an extension of one already shipped and working.

`IndicatorTraits<Key>` also carries the indicator's true value type (e.g. `MacdEnum`, `bool`, `int8_t`) alongside its storage location, so `GetValue<Key>()`/`SetValue<Key>(v)` are typed at the call site — callers get back the same enum type they get today (`static_cast` to/from the underlying `int8_t` happens inside the accessor, once, not scattered across every call site). This preserves today's type safety at call sites; only the physical storage and dispatch mechanism change.

**Runtime (only where code must iterate all indicators).** `CheckTrigger(size_t index)` and `PopulateIndicatorState` need to walk every indicator. These use `kIndicatorLayout[index]` — a plain array lookup, still no virtual call, no heap allocation, no string/map lookup — then dispatch on `StorageBlock` to read/write the right array. `PopulateIndicatorState` specifically becomes two straight-line, unrolled loops (one over the int8 block, one over the float block) copying directly into the FlatBuffer struct mutators, eliminating the polymorphic-pointer loop that carries the `OSCILLATOR_310` crash workaround today.

### 3.4 Working-state exception (not everything is a flat array)

A handful of richer indicators (KangarooTail, TurtleSoup, MomentumPinball) compute their *published* value from internal scratch state that is never serialized — e.g. a rolling Hurst estimate, a tail-to-body ratio. That scratch state does not need to live in the two schema-mirroring arrays; it stays as small dedicated PODs, one per indicator family, still with no virtual dispatch and no heap allocation, just not literally packed into the uniform block. Only what is actually published and dirty-tracked lives in `m_currentI8`/`m_currentF32`. This keeps the hot, common-path core lean without forcing an awkward shape onto the small number of indicators that don't fit a single scalar.

### 3.5 Compute functions

Existing near-free-function methods become literal free functions taking explicit primitive inputs and returning the computed value:

```cpp
int8_t ComputeImpulseFromColor(int color, int prevColor, float maDiff, float macdDiff, float atr);
```

Call sites move from `impulseInd->SetFromColor(...)` to `indMgr.SetValue<IndicatorKey::LONG_IMP>(ComputeImpulseFromColor(...))`. Where an indicator is inherently stateful (VWAP's running sum, an EMA's previous value), the function takes the small mutable state by explicit reference rather than hiding it in `this`:

```cpp
float ComputeVwap(VwapState& state, float typicalPrice, float barVolume, float atr, bool newSession);
```

This is not a new pattern for this codebase — it is the same shape as `VolumeProfileEngine.h`, `DailyBiasEngine.h`, and `EventVelocityEngine.h` (all introduced in the sessions immediately preceding this spec): pure, header-only, independently unit-testable via the established standalone-`g++` convention (`tests/cpp/*.cpp`). `HurstExponentIndicator::Calculate` (already barely touches `this`) and `RobustLogZ` (already a free function) are direct existing precedent.

## 4. Migration sequence

One unified plan, executed step by step (to be broken into detailed tasks by `superpowers:writing-plans`):

1. **Audit.** Reconcile all 54 `IndicatorKey` values against `IndicatorState`'s actual schema fields. Produce the exact `kIndicatorLayout` table. Identify: any key with no corresponding schema field (internal-only, e.g. metadata not published); any schema field needing a second key; the precise values of `N_I8`/`N_F32`.
2. **Introduce the new packed arrays and descriptor/traits mechanism *alongside* the existing `IndicatorStore`** (dual-write). Add a debug-only assertion comparing the old accessor's value against the new packed array's value every tick, for every migrated indicator. No call-site changes yet — this step is pure infrastructure plus a live parity check.
3. **Migrate the ~30 simple int8-valued indicators**, family by family: convert their compute methods to free functions, update `TripleScreenX.cpp`/`StudyHelperFunctions.cpp` call sites to `GetValue<K>()`/`SetValue<K>()`, verify against the Step 2 parity assertion before moving to the next family.
4. **Migrate the float-valued/quality-score indicators** the same way.
5. **Rewrite `CheckTrigger` and `PopulateIndicatorState`** to operate directly on the two packed arrays (unrolled, straight-line) instead of the polymorphic pointer loop — this is where the `OSCILLATOR_310` workaround is root-cause eliminated.
6. **Remove** the parity-check assertions, the old `IndicatorStore`, `BaseIndicator`, `Indicator<T>`, and all ~38 leaf classes, once every call site is migrated and verified clean.
7. **Correct `CLAUDE.md`'s stale `IndicatorManager` description** (and its three doc mirrors — `README-AI.md`, `.github/copilot-instructions.md`, `GEMINI.md` — per the Documentation Sync Contract) to reflect the new architecture.

## 5. Testing strategy

- New pure compute functions and the descriptor/traits mapping get standalone `tests/cpp/*.cpp` unit tests, following this codebase's established convention (hand-rolled `g++` compiles, no mocking framework).
- Step 2's dual-write parity assertion is the primary regression safety net during migration: any divergence between the old and new storage path is caught immediately, tick-by-tick, before any call site depends on the new path.
- ACSIL-dependent call-site changes (`TripleScreenX.cpp`, `StudyHelperFunctions.cpp`) remain build-verified only via `./build_dll.sh`, consistent with how this codebase already handles ACSIL glue code (no mocking layer exists for `sc`).
- No microbenchmark harness is introduced (§2, non-goals).

## 6. Success criteria

- No virtual dispatch remains in the indicator read/write/serialize hot path.
- All published indicator values live in contiguous, cache-friendly packed storage.
- The `OSCILLATOR_310` crash workaround is eliminated at the root, not special-cased further.
- `IndicatorManager` remains the sole facade; no external code touches `m_currentI8`/`m_currentF32` directly.
- The C++ packed array order matches `IndicatorState`'s existing schema field order exactly; no schema changes are required for this phase.
- A single table (`kIndicatorLayout`) is the only place mapping `IndicatorKey` to storage location — no more hand-synchronized parallel structures.
