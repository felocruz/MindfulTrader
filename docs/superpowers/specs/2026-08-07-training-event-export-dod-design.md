# Training Event Export Pipeline — DOD Conversion (EventDataCollector → IndicatorManager/ContextManager → LBRFileManager → `.alpha`/`.context`)

Two-tier assessment of the `.alpha`/`.context` export pipeline
(`EventDataCollectorStudy.cpp` → `IndicatorManager::GetTrainingEventT()` +
`ContextManager::AddToTrainingEventFB()` → `LBRFileManager.cpp`), traced
end-to-end for the same class of DOD gains found in the 16D observation
vector pipeline (`docs/superpowers/specs/2026-08-07-contextmanager-ring-
buffer-dod-design.md`).

> **Tier 1 status: DONE (2026-08-07).** See §1/§2 below. Implemented, built,
> and verified — all 12 standalone `tests/cpp/*.cpp` suites pass with 0
> failures, `./build_dll.sh` succeeds.

> **Tier 2 status: DONE (2026-08-07).** See §3 below. Implemented exactly as
> designed, with the one open item from §3.2 resolved before implementation
> rather than during it: `PredictionState` is written exclusively by live/
> backtest Python inference responses (`TradeExecutionServer::
> HandlePythonPrediction`, confirmed via `SetPrediction()`'s own doc comment
> and its "passive state holder" design — `ShouldTrigger()` hardcoded
> `false`), never by `EventDataCollectorStudy.cpp` — so it carries no
> per-call conditional-write risk the way `wave`/`m_anchors` do. The
> confirmed hazard itself was fixed with explicit `else` resets in
> `ContextManager::AddToTrainingEventFB()`. `IndicatorManager::
> m_trainingEventScratch` is now the pooled instance; `GetTrainingEventT()`
> returns `MTS::Training::TrainingEventT*`; `observation`/`asymmetry_context`
> are allocated once and overwritten via struct assignment thereafter. The
> one call site (`EventDataCollectorStudy.cpp`) required **zero source
> changes** — confirmed by the file compiling unchanged in the same build.
> `./build_dll.sh` succeeded; all 12 standalone `tests/cpp/*.cpp` suites pass
> with 0 failures. Zero remaining `make_unique<TrainingEventT>` or
> `unique_ptr<TrainingEventT>` anywhere in the codebase.

## 1. Context

### What's already good

`LBRFileManager.cpp` — the final stage of this pipeline — is already well
optimized: a single reused `m_fbb` `FlatBufferBuilder` member (`.Clear()`
between calls, never reconstructed), stack buffers for combined writes
(`uint8_t staged[512]`, `uint8_t combined[8192]`) instead of heap allocation,
and batched flush thresholds (`kFlushEveryRecords`). `IndicatorManager::
SyncFeatureVector()` reserves its fixed 29-element vector upfront.
`IndicatorManager::UpdateDailyCache`'s Volume Profile aggregation vector is
genuinely variable-sized, runs once per trading day (and is disabled by
default), and is the correct container choice — not a finding.
`EventDataCollectorStudy.cpp` itself is a thin orchestration layer with no
local container usage, same pattern already confirmed for `TripleScreen1/2/
3.cpp` in the sibling spec.

### Tier 1 finding: `IndicatorManager::m_recentTrainingDeltaUs`

`std::deque<int64_t>`, capped at `kTrainingTauWindowSize = 100`
(`include/IndicatorManager.h`). On every call to `GetTrainingEventT()` —
which fires on every significant-change event, the trigger for this entire
pipeline — a fresh `std::vector<int64_t> scratch` was copy-constructed from
it (`src/IndicatorManager.cpp:764`, pre-fix) just to run `nth_element` for a
median (`tau_100_log`). Same exact shape as `FeatureScaler::
RobustLocation()`'s pattern from the sibling spec's Round 1.

### Tier 2 finding: the Native Object API allocates on every significant-change event

The larger cost isn't in any container — it's that `GetTrainingEventT()`/
`AddToTrainingEventFB()` build a `TrainingEventT` via FlatBuffers' **Native
Object API**, which is heap-allocation-heavy by construction. Per
significant-change event, traced end-to-end:

1. `std::make_unique<TrainingEventT>()` — the event itself (`IndicatorManager.cpp:747`)
2. `event->indicators = std::make_unique<MTS::Schema::IndicatorState>();` (`IndicatorManager.cpp:789`)
3. `event->features`'s vector buffer (29 floats, `SyncFeatureVector`)
4. `event.observation = std::make_unique<ObservationData>(...)` (`ContextManager.cpp:620`, `AddToTrainingEventFB`)
5. `event.asymmetry_context = std::make_unique<AsymmetryContext>(...)` (`ContextManager.cpp:625`, same function)

Five heap allocations per event (the Tier 1 scratch vector was a sixth,
already fixed), all for objects with a **confirmed transient lifetime** —
traced through `EventDataCollectorStudy.cpp:722-761`: `GetTrainingEventT()`
returns the object, the caller mutates three top-level scalar fields
(`model_confidence`, `timestamp_us`, `event_type_code`), passes it by
reference to `LBRFileManager::LogSynchronizedEvent()`, and it goes out of
scope. No aliasing, no ownership transfer beyond that one call, no storage
anywhere else. This is exactly the shape a **pooled/reused instance** fixes
cleanly.

**A relevant precedent already exists in this exact function**, at
`IndicatorManager.cpp:788`: `if (!event->indicators) { event->indicators =
std::make_unique<...>(); }` — a check-before-allocate guard. This tells us
the pattern of "keep a nested Native Object field allocated once and reuse
it" is not a novel idea for this codebase, just not yet applied to the
top-level object or to `observation`/`asymmetry_context`.

## 2. Tier 1: `m_recentTrainingDeltaUs` (done)

**Container swap:** `std::deque<int64_t>` → `RingBuffer<int64_t,
kTrainingTauWindowSize + 1>` (capacity 101 — the standard "logical window +
1" headroom convention, since the call shape is the same `push_back(v); if
(size() > kTrainingTauWindowSize) pop_front();` pattern as every prior
`RingBuffer` conversion). `kTrainingTauWindowSize`'s declaration had to move
before the member that uses it as a template argument — same forward-
reference rule already hit converting `EfficiencyRatioCalculator::
ER_LOOKBACK` in the sibling spec's Round 4.

**Scratch buffer:** the `std::vector<int64_t> scratch` copy became a fixed
`std::array<int64_t, kTrainingTauWindowSize>`, filled via an explicit
`for (i=0; i<n; ++i)` loop (`n = m_recentTrainingDeltaUs.size()`, which can be
less than the array's full capacity before the window fills — `nth_element`
is bounded to `[begin, begin+n)`, not the array's full capacity, same
"filled prefix vs. fixed capacity" care already required converting
`GetLempelZivComplexity()` in the sibling spec's Round 3).

**Verification:** no new test file — `GetTrainingEventT()` is deeply
ACSIL-coupled (takes `SCStudyInterfaceRef` directly), same fallback already
used for `m_observationHistory`/`StudyHelperFunctions.cpp` in the sibling
spec. Verified via full `./build_dll.sh` and all 12 standalone `tests/cpp/
*.cpp` suites (0 failures, unchanged from before the fix).

## 3. Tier 2 (done): pool the `TrainingEventT` scratch object

### 3.1 Design

Add a persistent `MTS::Training::TrainingEventT` member to `IndicatorManager`
(e.g. `m_trainingEventScratch`), with its three heap-owning nested fields
(`indicators`, `observation`, `asymmetry_context`) allocated **once** —
either in `IndicatorManager`'s constructor, or lazily on first use with the
same check-before-allocate guard already used for `indicators` today.

`GetTrainingEventT()`'s return type changes from `std::unique_ptr<
TrainingEventT>` (ownership transfer) to `MTS::Training::TrainingEventT*`
(non-owning pointer to the persistent member). This is a **safe** signature
change with an unusually low blast radius, confirmed two ways:

- **The one call site needs zero source changes.** `EventDataCollectorStudy.cpp:722-761`
  already uses `auto eventT = ...`, `if (!eventT)`, and `eventT->field`/
  `*eventT` throughout — every one of those expressions means the same thing
  for a raw pointer as for a `unique_ptr`. Only the declared return type in
  `IndicatorManager.h`/`.cpp` needs to change.
- **The existing null-check is already unreachable.** `GetTrainingEventT()`
  has exactly one `return` statement (`IndicatorManager.cpp:912`, pre-Tier-1
  line numbers), always returning a valid, non-null object — `std::
  make_unique` throws on allocation failure rather than returning null, so
  the caller's `if (!eventT) { ...skip...; return; }` guard cannot currently
  fire. Switching to a raw pointer preserves this defensive check's shape
  exactly, without it ever having done anything today.

Each call, instead of allocating fresh, resets the persistent object's
scalar fields and repopulates through the exact same call chain
(`PopulateIndicatorState`, the companion `mutate_*` calls, `InferenceManager::
AddToTrainingEventFB`, `ContextManager::AddToTrainingEventFB`,
`SyncFeatureVector`) that already runs on every call today — none of that
logic changes, only where its output is stored.

### 3.2 The real risk: conditional writes leaking stale data across calls

A pooled object is only safe if **every** field it exposes is unconditionally
overwritten on every call. Where a write is conditional, a field can retain
last call's value on a call where the condition doesn't hold — invisible
today because a fresh object starts from a clean default every time.

**Confirmed hazard**, `ContextManager::AddToTrainingEventFB()`
(`ContextManager.cpp:591-611`):

```cpp
if (wave) {
    event.volatility = wave->volatility;
    event.efficiency = wave->efficiency;
    event.regime_tenure = wave->regimeTenure;
}
// no else -- if wave is null this call, these three fields are left untouched
...
if (m_anchors) {
    event.dist_day_high = m_anchors->distDayHigh;
    // ...four more fields, same pattern
}
```

`wave` is `m_hasWaveContext ? &m_waveContext : nullptr` — this is a real,
reachable state (`m_hasWaveContext` starts false and is set by a separate
TS2 writer), not a theoretical one. With today's fresh-allocation-per-call
design this is harmless (a fresh object's fields already default to zero
when `wave`/`m_anchors` are absent). With a pooled object, a call where
`wave` is null would silently **keep the previous call's `volatility`/
`efficiency`/`regime_tenure`** instead of resetting to the same zero default
a fresh object would have shown.

**Confirmed safe:** `IndicatorManager::PopulateIndicatorState()` — every one
of its ~28 `mutate_*` calls is unconditional (verified by reading the full
function body), so `event->indicators`'s struct fields are always fully
overwritten regardless of which specific indicators changed. `ObservationData`/
`AsymmetryContext` are FlatBuffers **structs** (fixed-layout, not tables) —
`*event.observation = MakeObservationData(...)` is a safe, complete,
trivial-copy overwrite of every field, not a partial mutation, so reusing
the pointed-to struct instance (rather than re-`make_unique`-ing it) carries
no staleness risk once the pointer itself is allocated once. `HmmStateIndicator::
AddToTrainingEventFB`/`MarketClimateIndicator::AddToTrainingEventFB`
(both resolve to the base `Indicator<T>::AddToTrainingEventFB` → 
`MapIndicatorKeyToTrainingEvent`) unconditionally write `event.hmm_state`/
`event.market_climate` respectively.

**Resolved: `PredictionState` is not a pooling risk, by construction.**
`PredictionState::SetPrediction()`'s own doc comment: "Full update from
`TradeExecutionServer::HandlePythonPrediction`" — it's written exclusively
by live/backtest Python inference responses arriving asynchronously,
never by `EventDataCollectorStudy.cpp` (the only caller of
`GetTrainingEventT()`). It's explicitly designed as a "passive state
holder" (`ShouldTrigger()` hardcoded `false`, dirty-mask injection blocked
via a no-op override) — its atomics hold whatever the last real prediction
set them to, read fresh on every call regardless of what triggered the
call. Whatever `MapIndicatorKeyToTrainingEvent` does for
`IndicatorKey::PREDICTION_STATE` (mapped or a no-op), it does the *same*
thing on every call — there is no per-call conditional branching on this
field's own upstream data the way `wave`/`m_anchors` branch on §3.2's
confirmed hazard, so pooling introduces no new staleness risk here.

### 3.3 Required implementation step: audit-and-reset, not just pool

Before pooling, every write site in the call chain (`GetTrainingEventT`
itself, `PopulateIndicatorState`, the three `InferenceManager` sub-object
overrides, `ContextManager::AddToTrainingEventFB`, `SyncFeatureVector`) needs
a pass confirming which fields are conditionally written, followed by one of:

- Adding explicit `else` branches that reset to the same default a
  freshly-constructed `TrainingEventT` would have shown (cheapest, most
  surgical — matches this codebase's existing style), or
- A single "reset all scalar fields to default" helper called at the top of
  `GetTrainingEventT()`, before any population logic runs, covering every
  conditional branch in one place rather than auditing each individually.

The second option is safer against a *future* conditional write being added
without updating a matching reset elsewhere, at the cost of a small,
one-time-per-call scalar-field-copy loop (still zero heap allocation,
trivial next to the five allocations being eliminated).

## 4. Goals and non-goals

**Goals:**
- Eliminate the guaranteed per-significant-change-event heap allocation and
  periodic `std::deque` chunk churn in `m_recentTrainingDeltaUs`. **(Tier 1, done.)**
- Eliminate the five remaining per-event heap allocations in the
  `TrainingEventT` construction chain via a pooled, reused instance.
  **(Tier 2, done.)**

**Non-goals:**
- A full rewrite of the `.alpha` export path to build the FlatBuffer
  directly via the Builder API (bypassing the Native Object API entirely,
  mirroring `EventSerializer.cpp`'s live-path zero-copy approach) — this
  would eliminate essentially all remaining allocation, but is a much
  larger, schema-aware rewrite. Noted as a theoretical "Tier 3" in the
  original brainstorming pass; not designed or scoped here.
- Changing `LBRFileManager.cpp` — already confirmed well-optimized, out of
  scope.
- Any change to what data gets exported or its values — every tier in this
  spec is a pure storage-layer/lifetime change, same standard as the sibling
  spec's every round.

## 5. Verification approach (Tier 2)

1. Complete the field-write audit in §3.3 first — this is a prerequisite,
   not a follow-up; implementing the pool before confirming which fields
   need explicit resets would risk silently reintroducing stale data.
2. `./build_dll.sh` and all 12 standalone `tests/cpp/*.cpp` suites, same
   baseline as every prior round.
3. No dedicated new test file is expected to be feasible — same ACSIL-
   coupling reasoning as Tier 1 and the sibling spec's Round 2/4 fallbacks.
   If the audit in §3.3 identifies a genuinely pure (ACSIL-independent)
   helper worth extracting (e.g. a "reset TrainingEventT scalars to default"
   function), that piece specifically could get characterization coverage
   the way `EventVelocityEngine.h`/`FeatureScaler.h` did.
4. Given this changes object lifetime/ownership (not just container type),
   manual review of every write site identified in §3.3's audit is the
   primary safety net — more scrutiny than a mechanical container swap
   warrants, proportional to the larger risk surface.

## 6. Success criteria

**Tier 1 (done):**
- `m_recentTrainingDeltaUs` is a `RingBuffer<int64_t, kTrainingTauWindowSize + 1>`.
- `std::deque` no longer appears in `IndicatorManager.h`/`.cpp` (comments aside).
- `./build_dll.sh` succeeds; all 12 standalone suites pass with 0 failures.
- No change to `tau_100_log`'s computed value for any input sequence.

**Tier 2 (done):**
- Every field in the `GetTrainingEventT()` call chain is confirmed
  unconditionally written, or has an explicit reset-to-default covering its
  conditional branches.
- `GetTrainingEventT()` returns `MTS::Training::TrainingEventT*` backed by a
  persistent `IndicatorManager` member; the five allocations in §1's list
  no longer occur on the steady-state (post-first-call) path.
- `EventDataCollectorStudy.cpp`'s call site requires no changes beyond
  compiling against the new return type.
- `./build_dll.sh` succeeds; all 12 standalone suites pass with 0 failures;
  no change to any `.alpha`/`.context` emitted value for any input sequence.
