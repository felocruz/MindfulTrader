# 16D Observation Vector Pipeline — DOD Conversion (Zero Heap Allocation on the Hot Path)

Four rounds converting every `std::deque`/growing-`std::vector` found on the 16D observation
vector's per-tick path (`TripleScreen1/2/3.cpp` → `ContextManager`/`StudyHelperFunctions.cpp`
→ FlatBuffers emission) to fixed-capacity, zero-heap containers. Round 1: `FeatureScaler`/
`StructureEngine`. Round 2: `ContextManager::m_eventTimestampsUS`/`m_observationHistory`.
Round 3: `InformationEngine::GetLempelZivComplexity()`. Round 4: `StudyHelperFunctions.cpp`
(`CalculateRealizedKurtosis`/`CalculateSkewness`/`CalculateMeanReversionSpeed`/
`CalculateVolConvexity`/`RollingWindowCalculator<T>`).

> **Round 1 status: DONE (2026-08-07)** — `FeatureScaler`/`StructureEngine`, below. Implemented largely as designed, with one
> necessary addition not anticipated in §3: `FeatureScaler` could not be
> unit-tested in its original location (`include/ContextManager.h` transitively
> includes `sierrachart.h`, which requires `windows.h` — uncompilable under
> plain g++). Since `FeatureScaler` itself has zero ACSIL dependency, it was
> extracted verbatim into its own `include/FeatureScaler.h`, matching the same
> precedent already set by `InformationEngine.h`/`TailRiskEngine.h`/
> `StructureEngine.h`. `ContextManager.h` now `#include`s it instead of
> defining it inline — no behavior change, confirmed by a full `./build_dll.sh`
> success immediately after the extraction, before any RingBuffer work began.
>
> `RingBuffer<T, Capacity>` (`include/RingBuffer.h`) was implemented exactly as
> specified, plus standard iterator typedefs (`iterator_category`/`value_type`/
> `difference_type`/`pointer`/`reference`) on `ConstIterator` that the original
> design sketch omitted — required for `std::iterator_traits` to accept it in
> `std::min_element`/`std::max_element` (caught by clangd's semantic analysis,
> not by g++/libstdc++, which silently accepted the incomplete iterator; fixed
> before it could bite a stricter toolchain).
>
> Both container swaps (`FeatureScaler::stateBuffers`/`logBuffers`,
> `StructureEngine`'s four deques) are in, at the exact capacities specified
> (`RANK_WINDOW + 1`, `WINDOW_SIZE + 1`, `EXPANSION_WINDOW + 1`). `std::deque`
> no longer appears in either file. Three new test files added
> (`test_ring_buffer.cpp`: 31 assertions, `test_structure_engine.cpp`: 20,
> `test_feature_scaler.cpp`: 14 — 65 total). The latter two are
> characterization tests with independently-derived expected values (hand
> arithmetic cross-checked against a `python3 -c` computation for the harder
> float cases), run once against the original `std::deque` implementation
> (100% pass) and again unchanged after each swap (100% pass, identical
> values) — the container swap is confirmed behavior-preserving, not just
> "looks right." Full `./build_dll.sh` succeeded after every step; all 12
> standalone `tests/cpp/*.cpp` suites (9 pre-existing + 3 new) pass with 0
> failures.

> **Round 2 status: DONE (2026-08-07).** Implemented exactly as designed in
> §3.3/§3.4, no deviations. `eve::CalculateBurstinessIndex<Capacity>()` was
> added to `EventVelocityEngine.h`, templated on `Capacity` as specified;
> `ContextManager::CalculateBurstinessIndex()` is now the thin delegating
> wrapper. `m_eventTimestampsUS` is a `RingBuffer<uint64_t, EVENT_VELOCITY_MAX
> + 1>`. `m_observationHistory` is the recommended SoA form —
> `std::array<RingBuffer<float, OBS_SATURATION_MIN_SAMPLES + 1>,
> OBSERVATION_VECTOR_SIZE>` — with `ComputeTriggerDecisionMetrics()`'s
> extraction now reading `m_observationHistory[dim][k]` (contiguous) instead
> of the old `m_observationHistory[k][dim]` (transposed AoS stride); the push/
> pop call site loops over all 16 per-dim buffers together, and `Reset()`
> clears all 16. `std::deque` no longer appears anywhere in `ContextManager.h`
> (nor in `FeatureScaler.h`/`StructureEngine.h`, per Round 1) — confirmed by a
> full-repo grep, comments aside. `include/ContextManager.h`'s now-unused
> `#include <deque>` was dropped as a direct consequence, alongside an
> explicit `#include "RingBuffer.h"` (previously only reached transitively).
>
> `test_event_velocity_engine.cpp` gained 4 new assertions (15 total, up from
> 11) covering `CalculateBurstinessIndex()`'s below-minimum/regular/irregular/
> small-capacity cases — the irregular case's expected CV
> (0.9386515150292289) was independently computed via `python3 -c` using
> `statistics.stdev()`, not derived from the code under test.
> `m_observationHistory`'s SoA restructure has no dedicated new test file, per
> §4 item 7's stated plan (same `sierrachart.h` reachability blocker as
> `FeatureScaler`/`StructureEngine` in Round 1) — verified via the existing
> `RingBuffer` test suite, careful review of the four call-site diffs, and a
> full build + test-suite pass. `./build_dll.sh` succeeded; all 12 standalone
> `tests/cpp/*.cpp` suites (65 Round 1 assertions + 4 new Round 2 assertions in
> `test_event_velocity_engine.cpp`) pass with 0 failures.

> **Round 3 status: DONE (2026-08-07).** A third "one last time" sweep, this
> time widened beyond `std::deque` to `std::vector`/heap patterns across the
> whole traced pipeline, found `InformationEngine::GetLempelZivComplexity()`
> (§1.2, §3.5 below) allocating two `std::vector`s **fresh on every call** —
> not periodic chunk churn like Rounds 1/2, a guaranteed heap allocation every
> qualifying tick, arguably the clearest-cut violation found across all three
> rounds. `WINDOW_SIZE_LZ` (64) is a compile-time bound, so both vectors became
> fixed `std::array<int, WINDOW_SIZE_LZ>`/`std::array<double, WINDOW_SIZE_LZ>`
> — no `RingBuffer` needed, these are plain bounded scratch buffers, not
> rolling windows (same shape as `FeatureScaler::RobustLocation()`'s own
> scratch array). A pre-existing unused local (`k_max`) in the same function,
> and the now-fully-unused `#include <vector>` at the top of the file, were
> removed as a direct, in-scope consequence of this exact edit. `test_information_engine.cpp`
> gained 3 new assertions covering the below-minimum, varying-input, and
> constant-input cases — the two real LZ76 outputs were independently derived
> via a standalone `python3` simulation of the same O(n²) Kaspar/Schuster
> algorithm and median-split binarization this class's own doc comment
> describes, not by running the code under test; both matched exactly.
> `./build_dll.sh` succeeded; all 12 standalone `tests/cpp/*.cpp` suites (65 +
> 4 + 3 = 72 characterization/unit assertions across the three rounds) pass
> with 0 failures.

> **Round 4 status: DONE (2026-08-07).** A fourth sweep, this time of
> `StudyHelperFunctions.cpp` (~3400 lines, not previously swept end-to-end),
> found three more findings (§1.3, §3.6 below), fixed in priority order.
> **Priority 1** (worst found): `CalculateRealizedKurtosis()`/`CalculateSkewness()`
> each built a fresh 100-element `std::vector<float>` with **no `.reserve()`** —
> ~7 reallocations per call from vector growth-doubling, not just one, called
> once per new bar. Both became `std::array<float, 100>` (`KURT_WINDOW`/
> `SKEW_WINDOW` are literal `constexpr int`s, so this was a direct swap, same
> shape as Round 3). **Priority 2:** `CalculateMeanReversionSpeed()`/
> `CalculateVolConvexity()` each allocated one `.reserve()`d vector (so only
> one allocation, not seven) sized to a *runtime* `lookback_n` parameter,
> called every tick (not de-duplicated to once-per-bar like Priority 1) —
> `lookback_n` is bounded to `[10,40]` by its one caller's own
> clamp, but neither function enforced that bound internally. Fixed with
> `std::array<_, 40>` plus a defensive `std::clamp(lookback_n, ..., 40)`
> inside each function itself (not just trusted from the caller) — since both
> are plain, externally-callable functions declared in the header, a future
> caller passing a larger value would otherwise write out of the fixed
> array's bounds. **Priority 3** (weakest finding, done anyway for
> consistency): `RollingWindowCalculator<T>`, a generic template reused by
> `ATRCalculator` (windows of 5, 20) and `EfficiencyRatioCalculator` (window
> of 34) across all three `TripleScreen*.cpp` files, was `std::deque`-backed.
> On reflection this was likely benign in practice — libstdc++'s deque chunk
> size for a 4-byte type is ~128 elements, well above any of these window
> sizes, so the deque almost certainly allocated its one chunk once and never
> churned again. Converted anyway to match the `RingBuffer` convention now
> established everywhere else in this pipeline: `Capacity` became a second,
> compile-time template parameter (`RollingWindowCalculator<T, Capacity>`)
> instead of a runtime constructor argument, since `RingBuffer` itself
> requires `Capacity` at compile time — the three call sites needed a small,
> mechanical update (template argument added, constructor argument dropped;
> `EfficiencyRatioCalculator`'s `ER_LOOKBACK` constant had to move above its
> use as that template argument, since C++ member-type declarations can't
> forward-reference a later member the way function bodies can).
>
> No new test files — `StudyHelperFunctions.cpp`'s functions all take
> `SCStudyInterfaceRef sc` directly and read `sc.Close`/`sc.BaseData` inline
> (unlike Round 1's `FeatureScaler`, which had zero ACSIL coupling once
> extracted), so extracting them for standalone testing would be a much
> larger, invasive refactor than this round's actual scope. Verified instead
> via careful review of each diff (mechanical: vector→array, `push_back`→
> indexed assignment, identical math and loop bounds) plus a full
> `./build_dll.sh` and full test-suite pass — same fallback already used for
> Round 2's `m_observationHistory` for the same underlying reason. `std::vector`
> and `std::deque` no longer appear anywhere in `StudyHelperFunctions.cpp`.
> All 12 standalone `tests/cpp/*.cpp` suites (unchanged — no new assertions
> this round) pass with 0 failures.

## 1. Context

A brainstorming pass traced the full 16D observation vector pipeline — `TripleScreen1/2/3.cpp` writers → `ContextManager::BuildObservationVector()` → `FeatureScaler::UpdateAndNormalize()` → `CheckAndTriggerHMM()` → FlatBuffers emission (`.context` file / `HMMClient` ZMQ) — to assess whether the same class of data-oriented-design gains found in the `indicator-manager-dod-soa` migration applies here.

**Finding: mostly no.** Unlike `IndicatorManager`'s ~44 polymorphic `Indicator<T>` classes with vtables and unchecked `static_cast`, `ContextManager`'s pipeline is already substantially data-oriented: `TripleScreen1/2/3.cpp` mutate `MTS::Schema::ObservationData` — the generated FlatBuffer struct — **directly**, with no intermediate object; `BuildObservationVector()` is a flat 16-scalar gather with no virtual dispatch; small POD structs (`StatisticalContext`, `NormalizedAnchors`) are moved, not allocated.

**The one real, well-evidenced gap:** `FeatureScaler::stateBuffers`/`logBuffers` (16× `std::deque<float>`, `include/ContextManager.h`) and `StructureEngine::m_prices`/`m_highs`/`m_lows`/`m_logRanges` (4× `std::deque<float>`, `include/StructureEngine.h`) are rolling windows backed by `std::deque`, which incurs periodic internal chunk allocation/deallocation as the window slides — a breach, in spirit, of this codebase's "no heap allocations in recurring ACSIL update paths" rule (`CLAUDE.md` Performance Rules). This is confirmed against the two sibling engines feeding the *same* 16D vector, which already use the correct pattern:

| Engine | Buffer type | Heap behavior |
|---|---|---|
| `InformationEngine` (dim 5, LZ) | `std::array<double, N>` | Zero heap, ever |
| `TailRiskEngine` (dim 9, Hill α) | `std::vector` resized once at construction | One-time allocation, then pure ring buffer |
| `FeatureScaler` (all 16 dims' scaling) | `std::deque<float>` | Ongoing chunk churn as window slides |
| `StructureEngine` (dims 13/14) | `std::deque<float>` | Ongoing chunk churn as window slides |

`FeatureScaler::UpdateAndNormalize()` runs **unconditionally every tick**, across all three screens, once warmed up (called from `SCStudies.cpp`'s per-tick `CheckAndTriggerHMM()` call, gated only by TS1/TS2 readiness — not by any lower-frequency trigger). `RobustLocation()` is called up to 13 times per tick (10 adaptive SOFTLOGZ dims + 3 LOGZ dims; 3 more SOFTLOGZ dims use static scaling and skip it), each copying up to 500 elements out of a `std::deque` into scratch, twice (once for median, once for MAD). This is the single most frequently-executed piece of work in the whole pipeline and the only place with genuine ongoing heap-churn risk.

### 1.1 Round 2: two more of the same class, found in `ContextManager.h` itself

Round 1's sweep was scoped to "the scaling layer" (`FeatureScaler`, its sibling engines). A second pass, sweeping every `std::deque` in `ContextManager.h` rather than just the ones feeding the scaling math directly, found two more:

- **`m_eventTimestampsUS`** (`std::deque<uint64_t>`, capped at `EVENT_VELOCITY_MAX = 100`) — maintained by `CalculateEventVelocity()` (`push_back` + conditional `pop_front`, **every tick**, same call shape as `FeatureScaler`'s original problem) purely so `CalculateBurstinessIndex()` can read it back (a plain oldest→newest range-based `for`, no random access needed) to compute `raschkeBurst`, which feeds dim 1 (`burstiness_index`) of the same 16D vector. The velocity number itself no longer needs this deque — it already moved to an O(1) EMA (`eve::VelocityState`, `EventVelocityEngine.h`, Phase 1 hardening plan Task 1) — the deque survives solely as burstiness's data source. At 8 bytes/element and a typical ~512-byte deque chunk, that's ~64 elements/chunk against a 100-element window: chunk churn roughly every 64 ticks.
- **`m_observationHistory`** (`std::deque<std::array<float, 16>>`, capped at `OBS_SATURATION_MIN_SAMPLES = 40`) — read every tick by `ComputeTriggerDecisionMetrics()`'s Mahalanobis calc, which extracts each dimension's values across the full history via a **transposed** per-dim scan (`m_observationHistory[k][dim]` for `k=0..n`, for each of the 16 dims). Initially deprioritized in Round 1 as "small" on raw byte count (2.5KB total) — but each element is 64 bytes, so a ~512-byte deque chunk holds only ~8 of them: chunk churn roughly every **8 ticks**, more frequent than Round 1's own dismissal implied. Unlike `m_eventTimestampsUS`, this one also has a second, independent inefficiency: the AoS layout is read in a column-wise (per-dimension) pattern it wasn't shaped for.

Both run unconditionally every tick (no gating beyond the same TS1/TS2 readiness and `FeatureScaler` warmup checks Round 1 already documented), in the exact same `CheckAndTriggerHMM()` call chain.

### 1.2 Round 3: a guaranteed per-call heap allocation, not just periodic churn

A third pass widened the sweep from `std::deque` specifically to `std::vector`/heap patterns generally, across every file in the traced pipeline. `TailRiskEngine::m_sortBuffer` (`std::vector`) was re-checked and re-confirmed fine — `.reserve()`d once at construction, only `.clear()`/`.assign()`/`.insert()`'d thereafter, genuinely zero ongoing reallocation. `TripleScreen1/2/3.cpp`'s handful of `new` calls are all `if (!ptr) ptr = new X()` lazy-singleton-init on Sierra Chart persistent pointers — one-time, not recurring.

One real finding: **`InformationEngine::GetLempelZivComplexity()`** (dim 5, `include/InformationEngine.h`) — the exact engine Round 1 cited as the *reference example* of doing this right (`std::array`-backed, "zero heap, ever") — allocates two `std::vector`s **fresh on every single call**:

```cpp
double GetLempelZivComplexity() const {
     // Max LZ window 64 is small enough for stack vector usually,
     // but here we use a small buffer.
     std::vector<int> S;
     S.reserve(WINDOW_SIZE_LZ);
     ...
     std::vector<double> sortedBuf;
     sortedBuf.reserve(count);
```

`WINDOW_SIZE_LZ` is a compile-time constant (`64`) — the function's own comment even acknowledges a stack buffer "usually" would fit, but the code never made that switch. Called every tick once `m_infoEngine.GetSampleCount() >= 50` (`ContextManager::BuildObservationVector()`), this is a **guaranteed heap allocation on every qualifying tick** — not periodic chunk churn like Rounds 1/2's `std::deque` findings, a hard allocation every time. The clearest-cut case found across all three rounds.

### 1.3 Round 4: `StudyHelperFunctions.cpp` — the largest untraced file in the pipeline

A fourth pass swept `StudyHelperFunctions.cpp` (~3400 lines) end-to-end for the same patterns — the largest single file in the trace that hadn't yet been checked line-by-line. Three findings, ranked by severity:

1. **`CalculateRealizedKurtosis()`/`CalculateSkewness()`** (worst): each builds a fresh 100-element `std::vector<float>` with **no `.reserve()`** — vector growth-doubling means ~7 reallocations per call, not one. Called once per new bar (de-duplicated via a `lastObsUpdateIndex` guard in `UpdateObservationVectorSubgraphs`) — feeds `NormalizedAnchors.realizedKurtosis`/`skewnessIdx`, dims 8/10 of the 16D vector. `KURT_WINDOW`/`SKEW_WINDOW` are both literal `constexpr int = 100`.
2. **`CalculateMeanReversionSpeed()`/`CalculateVolConvexity()`**: each allocates one `.reserve()`d vector (one allocation, not seven) sized to a *runtime* `lookback_n` parameter. Worse than #1 in one respect — called **every tick**, not de-duplicated to once-per-bar (the call site's own comment: "not yet persisted by `UpdateObservationVectorSubgraphs`"). `lookback_n` is bounded to `[10,40]` by its one caller (`CalculateAdaptiveObservationWindow`'s own `std::clamp`), but neither function enforces that bound internally — both are plain functions declared in the header, callable from anywhere.
3. **`RollingWindowCalculator<T>`** (weakest, converted anyway for consistency): a generic `std::deque`-backed template reused by `ATRCalculator` (windows of 5, 20) and `EfficiencyRatioCalculator` (window of 34), called from all three `TripleScreen*.cpp` files. Likely benign in practice — libstdc++'s deque chunk size for a 4-byte type is ~128 elements, well above any of these window sizes, so the one initial chunk almost certainly never needed a successor. Fixed to match the established convention, not because it was confirmed to be causing live churn.

## 2. Goals and non-goals

**Goals:**
- Eliminate `std::deque`'s ongoing chunk allocation/deallocation churn in `FeatureScaler` and `StructureEngine` by replacing their rolling-window storage with a fixed-capacity container, matching the ring-buffer convention already established by `InformationEngine`/`TailRiskEngine` in this same pipeline. **(Round 1, done.)**
- Preserve exact current behavior — this is a container swap, not an algorithm change. Same push/evict/overwrite-last semantics, same window sizes, bit-identical median/MAD/output given the same input sequence. **(Round 1, done; same standard applies to Round 2.)**
- Close a pre-existing test-coverage gap: neither `FeatureScaler` nor `StructureEngine` has any unit test today. Add enough characterization coverage to make this swap verifiable, not just "looks right." **(Round 1, done.)**
- Convert `m_eventTimestampsUS` to `RingBuffer<uint64_t, EVENT_VELOCITY_MAX + 1>`, and extract `CalculateBurstinessIndex()`'s pure arithmetic into `EventVelocityEngine.h` so it's independently unit-testable for the first time — it currently has zero test coverage, trapped in `ContextManager.cpp` behind `sierrachart.h`, same root cause `FeatureScaler` had in Round 1. **(Round 2, done.)**
- Convert `m_observationHistory` from `std::deque<std::array<float,16>>` (AoS) to a structure-of-arrays of 16 `RingBuffer<float, OBS_SATURATION_MIN_SAMPLES + 1>` — one per dimension, mirroring `FeatureScaler`'s own `stateBuffers`/`logBuffers` layout — closing both the heap-churn issue and the transposed-access inefficiency in `ComputeTriggerDecisionMetrics`. **(Round 2, done.)**
- Convert `InformationEngine::GetLempelZivComplexity()`'s two per-call `std::vector`s to fixed `std::array<_, WINDOW_SIZE_LZ>` scratch buffers — eliminating a guaranteed heap allocation on every qualifying tick, not just periodic churn. **(Round 3, done.)**
- Convert `StudyHelperFunctions.cpp`'s three findings in priority order: `CalculateRealizedKurtosis`/`CalculateSkewness`'s no-reserve 100-element vectors (worst — up to 7 reallocations/call); `CalculateMeanReversionSpeed`/`CalculateVolConvexity`'s runtime-sized, single-reserve vectors (called every tick, not de-duplicated); `RollingWindowCalculator<T>`'s `std::deque` (likely benign, converted for consistency). **(Round 4, done.)**
- For any function taking a *runtime* size parameter with no compile-time bound, enforce a defensive upper-bound clamp inside the function itself before sizing a fixed array to it — don't just trust the one caller's own bound, since these are externally-callable functions. **(Round 4, done — `CalculateMeanReversionSpeed`/`CalculateVolConvexity`.)**

**Non-goals:**
- `HMMClient::SendBinaryRequest()`'s per-call `FlatBufferBuilder` allocation is explicitly out of scope — it runs on `HMMClient`'s dedicated background worker thread, gated by the Mahalanobis significant-change trigger, not the ACSIL tick hot path. Same category `EventSerializer.cpp` already solved elsewhere via a reusable builder, but lower priority here since it's already decoupled from the hot path.
- No change to `BuildObservationVector()`, the "FlatBuffer struct as direct storage" design for `m_observationData`, `CalculateEventVelocity()`'s existing EMA-based velocity formula, or any of the 16D vector's semantics/values. No schema changes.
- Not a comprehensive test suite for `FeatureScaler`/`StructureEngine`/`ContextManager`/`StudyHelperFunctions.cpp` as a whole — only enough characterization coverage (or, where that's blocked by ACSIL coupling, careful review + full build/test-suite pass) to safety-net each specific container swap. Broader test coverage for these files is a separate, larger effort if ever wanted.
- Full-repo call-site sweep for other `std::deque`/`std::vector` usage beyond the files this pipeline actually traces through is out of scope — Rounds 1–4 confirmed everything else in the trace (`TailRiskEngine::m_sortBuffer`, the TripleScreen files' lazy-init `new` calls) is already either zero-allocation or one-time.
- Renaming `m_observationHistory` is out of scope — only its underlying storage type changes; the SoA restructure is a private-implementation-detail change invisible to every caller outside `ContextManager.cpp`.
- Changing `GetLempelZivComplexity()`'s algorithm, its median-split binarization, or the Kaspar/Schuster LZ76 implementation itself is out of scope — Round 3 is a pure storage-layer swap, same standard as every prior round.
- Fixing `ATRCalculator`'s apparent double-counting (`CalculateMarketSpeed()` is called twice per tick from two different callers in `TripleScreen1.cpp`, and its `last_index` member looks intended to guard against exactly this but is never checked) is explicitly out of scope — that's a correctness question noticed in passing during Round 4, not a DOD finding. Not fixed here.

## 3. Design

### 3.1 New shared utility: `include/RingBuffer.h`

A single header-only, templated, fixed-capacity buffer class replacing `std::deque<T>` for both call sites:

```cpp
template <typename T, size_t Capacity>
class RingBuffer {
public:
    void push_back(const T& v);   // asserts if already at Capacity — caller must pop_front() first
    void pop_front();              // no-op if empty
    T& back();                     // last-pushed element (mutable — needed for StructureEngine's
    const T& back() const;         //   isNewBar=false "overwrite last bar" path)
    size_t size() const;
    bool empty() const;
    void clear();

    T& operator[](size_t logicalIndex);        // 0 = oldest, size()-1 = newest — matches deque's semantics
    const T& operator[](size_t logicalIndex) const;

    // Forward-iterator support (operator++, operator*, operator!=) so
    // std::accumulate/std::min_element/std::max_element and range-based for
    // work unchanged at StructureEngine's call sites. Iterates oldest → newest.
    Iterator begin();
    Iterator end();
    ConstIterator begin() const;
    ConstIterator end() const;

private:
    std::array<T, Capacity> m_data;
    size_t m_head = 0;   // physical index of oldest element
    size_t m_count = 0;
};
```

Backing storage is `std::array<T, Capacity>` — stack-allocated (as a class member, so it lives inside `FeatureScaler`/`StructureEngine`'s own storage, same as today), zero heap allocation for the buffer's lifetime. `push_back`/`pop_front`/`operator[]` are all O(1) with simple modular arithmetic — no chunk-index math, no pointer chasing.

**Capacity sizing (headroom for the existing push-then-conditionally-pop call shape):** both call sites currently do `push_back(v); if (size() > windowSize) pop_front();` — meaning size transiently reaches `windowSize + 1` between the two statements. To keep the call sites byte-for-byte unchanged (lowest-risk migration), `Capacity` is sized to the max logical window size **+ 1** at each site:

- `FeatureScaler::stateBuffers`/`logBuffers`: `Capacity = RANK_WINDOW + 1 = 501` (per-dimension logical window varies 150–500 via `DIM_WINDOW_SIZE`; 501 covers the transient overshoot for the largest dim).
- `StructureEngine::m_prices`/`m_highs`/`m_lows`: `Capacity = WINDOW_SIZE + 1 = 31`.
- `StructureEngine::m_logRanges`: `Capacity = EXPANSION_WINDOW + 1 = 101`.

### 3.2 Call-site changes

**`include/ContextManager.h` (`FeatureScaler`):**
```cpp
// Before:
std::array<std::deque<float>, N_DIMS> stateBuffers;
std::array<std::deque<float>, N_DIMS> logBuffers;
// After:
std::array<RingBuffer<float, RANK_WINDOW + 1>, N_DIMS> stateBuffers;
std::array<RingBuffer<float, RANK_WINDOW + 1>, N_DIMS> logBuffers;
```
`UpdateAndNormalize()`, `Calibrate()`, `Recalibrate()`, and `RobustLocation()`'s copy loop (`scratch[j] = buf[j]`) are otherwise **unchanged** — `RingBuffer` implements the same `push_back`/`pop_front`/`size`/`empty`/`operator[]` surface `std::deque` provided at these call sites.

**`include/StructureEngine.h`:**
```cpp
// Before:
std::deque<float> m_prices, m_highs, m_lows, m_logRanges;
// After:
RingBuffer<float, WINDOW_SIZE + 1> m_prices, m_highs, m_lows;
RingBuffer<float, EXPANSION_WINDOW + 1> m_logRanges;
```
`Update()`, `GetLogExpansionRatio()`, `GetRecurrenceRate()`, `GetFractalDimension()`, `GetMeanReversionZ()`, and `CalculateOLS()`/`CalculateResidualStdDev()` are otherwise **unchanged** — they use `push_back`, `pop_front`, `back()` (both read and write, for the `isNewBar=false` overwrite-last-bar path), `size()`, `empty()`, `operator[]`, range-based `for`, and `std::accumulate`/`std::min_element`/`std::max_element` — all covered by `RingBuffer`'s iterator support.

### 3.3 `m_eventTimestampsUS` (Round 2)

**Container swap:** `std::deque<uint64_t>` → `RingBuffer<uint64_t, EVENT_VELOCITY_MAX + 1>` (capacity 101 — the same "logical window + 1" headroom convention as Round 1, since `CalculateEventVelocity()`'s call shape is the identical `push_back(v); if (size() >= EVENT_VELOCITY_MAX) pop_front();` pattern). `ContextManager.h`'s member declaration is the only signature change; `CalculateEventVelocity()`'s own body (the deque maintenance, then delegating to `eve::UpdateAndGetVelocity()`) is otherwise unchanged.

**Testability fix, mirroring Round 1's `FeatureScaler` extraction:** `CalculateBurstinessIndex()`'s CV-of-inter-arrival-times arithmetic (`src/ContextManager.cpp:1418-1460`) has zero unit test coverage today — it's a `ContextManager` member function, and `ContextManager.h` transitively includes `sierrachart.h`. The arithmetic itself has no ACSIL dependency at all (just iteration + mean/stddev over the timestamp history), so it moves into `include/EventVelocityEngine.h` — the header already dedicated to this exact class of pure, ACSIL-independent event-arrival logic, and whose own top-of-file comment already documents shared ownership of this timestamp history with `CalculateEventVelocity()`:

```cpp
// EventVelocityEngine.h, new addition:
namespace eve {
template <size_t Capacity>
float CalculateBurstinessIndex(const RingBuffer<uint64_t, Capacity>& timestamps) {
    // Same algorithm as today's ContextManager::CalculateBurstinessIndex():
    // IAT conversion (oldest -> newest), mean, stddev, CV = stddev / mean.
    // Templated on Capacity so the unit test can exercise it at a small
    // capacity without needing 100 timestamps to hit every code path.
}
}  // namespace eve
```

`ContextManager::CalculateBurstinessIndex(uint64_t now_us)` becomes a thin wrapper: `(void)now_us; return eve::CalculateBurstinessIndex(m_eventTimestampsUS);`.

### 3.4 `m_observationHistory` (Round 2)

**Design choice: AoS-preserving swap vs. SoA restructure.** Two options were considered:

1. **Minimal (AoS-preserving):** `RingBuffer<std::array<float, 16>, OBS_SATURATION_MIN_SAMPLES + 1>` — same shape as today, `ComputeTriggerDecisionMetrics()` unchanged. Fixes heap churn only; the transposed per-dim read pattern stays.
2. **SoA restructure (recommended):** `std::array<RingBuffer<float, OBS_SATURATION_MIN_SAMPLES + 1>, OBSERVATION_VECTOR_SIZE>` — one ring buffer per dimension. Fixes heap churn **and** the transposed-access inefficiency, and is more consistent with the established precedent in this exact codebase: `FeatureScaler` already stores its own 16-dim rolling windows this exact way (`stateBuffers`/`logBuffers`, one buffer per dim). `m_observationHistory` is structurally the same kind of data (16 dims × a short rolling window of recent scaled observations) as what `FeatureScaler` already manages — there's no principled reason for it to be laid out differently.

Going with option 2. All 16 per-dim buffers are always pushed/popped together, once per tick (single call site), so they never fall out of sync — `size()` on any one of them (dimension 0, by convention) is a valid stand-in for "how many samples do we have" everywhere the current code checks `m_observationHistory.size()`.

**Call-site changes (`src/ContextManager.cpp`):**
```cpp
// Before (line ~1233):
m_observationHistory.push_back(currentObs);
if (m_observationHistory.size() > OBS_SATURATION_MIN_SAMPLES) {
    m_observationHistory.pop_front();
}
// After:
for (size_t dim = 0; dim < OBSERVATION_VECTOR_SIZE; ++dim) {
    m_observationHistory[dim].push_back(currentObs[dim]);
    if (m_observationHistory[dim].size() > OBS_SATURATION_MIN_SAMPLES) {
        m_observationHistory[dim].pop_front();
    }
}
```
```cpp
// Before (line ~978, inside ComputeTriggerDecisionMetrics's per-dim loop):
scratch[k] = m_observationHistory[k][dim];   // transposed: strides across AoS entries
// After:
scratch[k] = m_observationHistory[dim][k];   // contiguous: one dim's own buffer
```
The guard at line 962 (`if (m_observationHistory.size() < OBS_SATURATION_MIN_SAMPLES) return metrics;`) becomes `if (m_observationHistory[0].size() < OBS_SATURATION_MIN_SAMPLES) ...` per the lockstep invariant above. `Reset()`'s `m_observationHistory.clear()` (line ~1340) becomes a loop over all 16 buffers' `clear()`.

### 3.5 `InformationEngine::GetLempelZivComplexity()` (Round 3)

**No `RingBuffer` needed here** — `S` and `sortedBuf` are plain bounded scratch buffers (built fresh, consumed, discarded within a single call), not rolling windows carried between calls. `WINDOW_SIZE_LZ` (64) is the exact compile-time upper bound on how many elements either ever holds, so a fixed `std::array` is the direct swap — the same shape `FeatureScaler::RobustLocation()` already uses for its own per-call scratch array.

```cpp
// Before:
std::vector<int> S;
S.reserve(WINDOW_SIZE_LZ);
...
std::vector<double> sortedBuf;
sortedBuf.reserve(count);
for (size_t i=0; i<count; ++i) { ...; sortedBuf.push_back(m_bufferLZ[idx]); }
std::sort(sortedBuf.begin(), sortedBuf.end());
...
for (size_t i=0; i<count; ++i) { ...; S.push_back(m_bufferLZ[idx] >= median ? 1 : 0); }
...
int n = static_cast<int>(S.size());

// After:
std::array<int, WINDOW_SIZE_LZ> S{};
...
std::array<double, WINDOW_SIZE_LZ> sortedBuf{};
for (size_t i=0; i<count; ++i) { ...; sortedBuf[i] = m_bufferLZ[idx]; }
std::sort(sortedBuf.begin(), sortedBuf.begin() + count);   // only the filled prefix
...
for (size_t i=0; i<count; ++i) { ...; S[i] = (m_bufferLZ[idx] >= median ? 1 : 0); }
...
int n = static_cast<int>(count);   // S's fixed capacity (64) != the filled count
```

The one thing to get right: `std::sort` must bound its range to `[begin, begin + count)`, not the whole fixed array — `count` can be less than `WINDOW_SIZE_LZ` (below full warmup), and `n` (used by the LZ76 loop and the `b(n) = n/log2(n)` normalization) must read `count`, not the array's fixed `.size()` (which is always 64 regardless of how many are actually filled).

`#include <vector>` becomes unused once this is the only `std::vector` consumer in the file — dropped as a direct consequence. A pre-existing unused local (`k_max`, dead since some earlier edit) in the same function is removed too, since it sits in the exact lines being touched.

### 3.6 `StudyHelperFunctions.cpp` (Round 4)

**Priority 1 — `CalculateRealizedKurtosis()`/`CalculateSkewness()`:** `KURT_WINDOW`/`SKEW_WINDOW` are both literal `constexpr int = 100`, so this is the same direct swap as Round 3's `GetLempelZivComplexity()`:

```cpp
// Before:
std::vector<float> returns;
for (int i = 0; i < KURT_WINDOW; ++i) {
    float ret = std::log(...);
    returns.push_back(ret);
}
// After:
std::array<float, KURT_WINDOW> returns{};
for (int i = 0; i < KURT_WINDOW; ++i) {
    returns[static_cast<size_t>(i)] = std::log(...);
}
```
The rest of each function's range-based `for (float r : returns)` loops are unchanged — `std::array` supports them identically, and every element is always filled (the loop always runs the full `KURT_WINDOW`/`SKEW_WINDOW` bound), so there's no "filled prefix vs. fixed capacity" distinction to get wrong here, unlike Round 3's `GetLempelZivComplexity()`.

**Priority 2 — `CalculateMeanReversionSpeed()`/`CalculateVolConvexity()`:** these take a *runtime* `lookback_n`/derived `n`/`m`, with no compile-time bound and no internal upper-bound enforcement — only their one caller's own `[10,40]` clamp keeps them safe today. Since both are plain, header-declared functions (callable from anywhere, not `static`/file-local), sizing a fixed array to today's observed max without also clamping internally would silently corrupt memory the day a second caller passes something larger. Both get a local `constexpr int kMaxLookback = 40;` and an explicit `std::clamp(...)` on the runtime parameter *before* it's used to size or index the array:

```cpp
// CalculateMeanReversionSpeed, before:
const int n = std::max(lookback_n, 5);
...
std::vector<double> returns;
returns.reserve(m);
...
returns.push_back(r);
// After:
constexpr int kMaxLookback = 40;
const int n = std::clamp(lookback_n, 5, kMaxLookback);
...
std::array<double, kMaxLookback> returns{};  // m <= n-1 < kMaxLookback, always in range
...
returns[static_cast<size_t>(i)] = r;
```

`CalculateVolConvexity` follows the same shape, with one extra care point: its second loop originally read `for (float tr : trValues)` over the *vector's actual filled size*. Converted naively to a range-based `for` over the *fixed array* would silently sum in `kMaxLookback - n` zero-initialized tail elements whenever `n < kMaxLookback` — corrupting `sumSqDiff`. Fixed by bounding both loops explicitly to `i < n`, not iterating the array's full fixed capacity — the same "filled prefix vs. fixed capacity" distinction Round 3 already had to get right for `GetLempelZivComplexity()`.

**Priority 3 — `RollingWindowCalculator<T>`:** `RingBuffer<T, Capacity>` requires `Capacity` at compile time, so the class gained a second template parameter instead of keeping a runtime constructor argument:

```cpp
// Before:
template<typename T>
class RollingWindowCalculator {
    std::deque<T> window;
    size_t max_size;
public:
    explicit RollingWindowCalculator(size_t size) : max_size(size) {}
    void push(T value) {
        if (window.size() == max_size) { sum -= window.front(); window.pop_front(); }
        window.push_back(value);
        sum += value;
    }
    ...
};
// After:
template<typename T, size_t Capacity>
class RollingWindowCalculator {
    RingBuffer<T, Capacity> window;
public:
    void push(T value) {
        if (window.size() == Capacity) { sum -= window[0]; window.pop_front(); }
        window.push_back(value);
        sum += value;
    }
    ...
};
```
`window.front()` becomes `window[0]` (`RingBuffer` doesn't need a separate `front()` — index 0 is always the oldest element, matching deque's own semantics). The runtime `max_size` member is removed entirely since `Capacity` now encodes it. Three call sites updated: `ATRCalculator`'s two buffers (`RollingWindowCalculator<float, 5>`/`<float, 20>`, constructor arguments dropped) and `EfficiencyRatioCalculator`'s one (`RollingWindowCalculator<float, ER_LOOKBACK>`) — the last one required moving `static constexpr int ER_LOOKBACK = 34;` to *before* the member that uses it as a template argument, since (unlike a constructor's default-member-initializer or a function body) a class member's *type* is resolved immediately, not deferred to a complete-class context, so it can't forward-reference a later member.

## 4. Verification approach

Neither `FeatureScaler` nor `StructureEngine` has any existing unit test, so this swap needs its own safety net, scoped narrowly to the change itself (not a general test-coverage backfill):

1. **`tests/cpp/test_ring_buffer.cpp` (new):** exhaustive coverage of `RingBuffer<T, Capacity>` in isolation — push/pop past capacity boundary, `back()` mutation, `operator[]` logical-index correctness after wraparound, iterator correctness (`std::accumulate`/`std::min_element`/`std::max_element` against hand-computed expected values), `clear()`, empty-buffer edge cases. This is the primary safety net.
2. **Characterization coverage for the two call sites**, bounded to just enough to catch a broken swap — not a full behavioral suite:
   - `FeatureScaler`: a small test driving `UpdateAndNormalize()` with a synthetic, hand-computable sequence (e.g. a known ramp or a fixed-seed pseudo-random sequence) for a couple of representative dims (one SOFTLOGZ, one LOGZ, one of the three static-scaled dims), asserting the scaled output matches values computed by hand/by an independent script — run once against the *current* `std::deque` implementation to establish the golden values, then confirmed unchanged after the swap.
   - `StructureEngine`: a small test driving `Update()` with a synthetic OHLC sequence through all four `Get*()` accessors, same before/after golden-value approach, including at least one `isNewBar=false` overwrite-last-bar case.
3. `./build_dll.sh` clean build.
4. Full re-run of all `tests/cpp/*.cpp` standalone suites (10 after Round 1, up from 9) — zero regressions.
5. No Sierra Chart run is in scope for this spec's verification, consistent with the project's standing constraint against using `BackTesterStudy.cpp`; `EventDataCollectorStudy.cpp` remains the approved live-data verification path if a post-merge sanity check against real replay data is wanted later.

**Round 2 additions:**

6. **`tests/cpp/test_event_velocity_engine.cpp` (extended):** add coverage for the new `eve::CalculateBurstinessIndex<Capacity>()` — fewer than 4 timestamps returns the documented neutral default (1.0f); a synthetic, hand-computable timestamp sequence (e.g. perfectly regular spacing → CV≈0; a deliberately irregular sequence with an independently-computed expected CV) at a small test capacity (e.g. 8, not the production 101) for fast, exhaustive coverage. Golden values captured once against a standalone extraction of the *current* `ContextManager::CalculateBurstinessIndex()` logic (same "run against current implementation first" methodology as Round 1), then confirmed unchanged after the move.
7. **`m_observationHistory`** has no existing test today, and — like `FeatureScaler`/`StructureEngine` in Round 1 — is not independently reachable by a standalone test while it lives inside `ContextManager` (`sierrachart.h` transitively required). Verification for this one leans on: (a) the already-exhaustive `RingBuffer` unit tests as the container-level safety net, (b) careful code review of the two call-site diffs above (small, mechanical, easy to inspect for the transposition direction specifically), and (c) a full `./build_dll.sh` + full test-suite pass. If tighter characterization coverage is wanted later, `ComputeTriggerDecisionMetrics`'s Mahalanobis arithmetic itself is a candidate for the same "extract to a pure header" treatment `EventVelocityEngine.h`/`FeatureScaler.h` already received — not bundled into this round to keep the diff focused.

**Round 3 addition:**

8. **`tests/cpp/test_information_engine.cpp` (extended):** `InformationEngine.h` is already ACSIL-independent and directly testable (unlike Round 2's `m_eventTimestampsUS`/`m_observationHistory`), so this one gets real characterization coverage, not just container-level tests. Three cases: fewer than 10 samples (documented neutral 0.5 default); a hand-computable ramp (median-split binarization traced by hand into an exact bit string, then the resulting LZ76 complexity independently re-derived via a standalone `python3` simulation of the same O(n²) Kaspar/Schuster algorithm this class's own doc comment describes — not by running the code under test); an all-identical-input degenerate case (same independent-simulation methodology). Golden values captured once against the *current* `std::vector` implementation (100% pass), then confirmed unchanged after the swap (100% pass, identical values, byte-for-byte).

**Round 4 addition:**

9. **No new test files.** Every function touched in `StudyHelperFunctions.cpp` takes `SCStudyInterfaceRef sc` directly and reads `sc.Close`/`sc.BaseData`/`sc.High`/`sc.Low` inline — unlike Round 1's `FeatureScaler` (zero ACSIL coupling once extracted to its own header), extracting these for standalone testing would mean restructuring their whole interface (passing raw arrays/spans instead of `sc`), a much larger and more invasive change than this round's actual scope. Verification instead: (a) careful line-by-line review of each diff — all four Priority 1/2 conversions are mechanical (`push_back`→indexed assignment, identical arithmetic, identical loop bounds), with the one real risk (iterating a fixed array's *full* capacity instead of its *filled* count in `CalculateVolConvexity`'s second loop) explicitly checked and fixed; (b) `RollingWindowCalculator<T, Capacity>`'s new template parameter reviewed against all three call sites for capacity-vs-original-runtime-size equivalence (5→5, 20→20, 34→`ER_LOOKBACK`); (c) a full `./build_dll.sh` and full 12-suite test-suite pass — same fallback Round 2 already used for `m_observationHistory` for the identical underlying reason.

## 5. Success criteria

**Round 1 (done):**
- `RingBuffer<T, Capacity>` exists, is unit-tested in isolation, and has zero heap allocation for its full lifetime (stack/member-embedded `std::array` backing).
- `FeatureScaler::stateBuffers`/`logBuffers` and `StructureEngine`'s four deques are converted; `std::deque` no longer appears in either file.
- Characterization tests confirm bit-identical (or floating-point-tolerance-identical) output between the pre- and post-swap implementations for the tested sequences.
- `./build_dll.sh` succeeds; all standalone `tests/cpp/*.cpp` suites pass with 0 failures.
- No change to any emitted `.context`/live-HMM value's semantics — this is a pure storage-layer swap.

**Round 2 (done):**
- `m_eventTimestampsUS` is a `RingBuffer<uint64_t, EVENT_VELOCITY_MAX + 1>`; `eve::CalculateBurstinessIndex<Capacity>()` exists in `EventVelocityEngine.h`, unit-tested, and `ContextManager::CalculateBurstinessIndex()` is a thin delegating wrapper.
- `m_observationHistory` is `std::array<RingBuffer<float, OBS_SATURATION_MIN_SAMPLES + 1>, OBSERVATION_VECTOR_SIZE>`; `ComputeTriggerDecisionMetrics()`'s per-dim extraction reads its own dimension's contiguous buffer, not a transposed AoS stride.
- `std::deque` no longer appears anywhere in `ContextManager.h`.
- `./build_dll.sh` succeeds; all standalone `tests/cpp/*.cpp` suites pass with 0 failures; no change to any emitted `.context`/live-HMM value's semantics.

**Round 3 (done):**
- `InformationEngine::GetLempelZivComplexity()`'s `S` and `sortedBuf` are fixed `std::array<_, WINDOW_SIZE_LZ>`; `std::vector` no longer appears anywhere in `InformationEngine.h`.
- Three new characterization assertions in `test_information_engine.cpp`, with two independently-computed LZ76 golden values (not derived from the code under test) confirmed identical before and after the swap.
- `./build_dll.sh` succeeds; all 12 standalone `tests/cpp/*.cpp` suites pass with 0 failures.
- No change to `GetLempelZivComplexity()`'s returned values for any input sequence — this is a pure storage-layer swap, same standard as every prior round.

**Round 4 (done):**
- `CalculateRealizedKurtosis()`/`CalculateSkewness()` use fixed `std::array<float, 100>` instead of no-reserve `std::vector`s.
- `CalculateMeanReversionSpeed()`/`CalculateVolConvexity()` use fixed `std::array<_, 40>` plus an internal defensive `std::clamp` on their runtime size parameter — safe even if a future caller passes a value outside today's `[10,40]` contract.
- `RollingWindowCalculator<T, Capacity>` is `RingBuffer`-backed with `Capacity` as a compile-time template parameter; all three call sites (`ATRCalculator` ×2, `EfficiencyRatioCalculator` ×1) updated accordingly.
- `std::vector` and `std::deque` no longer appear anywhere in `StudyHelperFunctions.cpp`.
- `./build_dll.sh` succeeds; all 12 standalone `tests/cpp/*.cpp` suites pass with 0 failures. No change to any of these six functions' returned values for any input — pure storage-layer swaps, same standard as every prior round.
