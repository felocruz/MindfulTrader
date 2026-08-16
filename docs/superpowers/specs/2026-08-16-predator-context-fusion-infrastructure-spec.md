# Spec: Predator Context & Fusion Infrastructure (`PredatorContext` / `PredatorFusion`)

**Status**: SPEC — approved via `superpowers:brainstorming` (2026-08-16, MindfulTrader-rooted session).
**Implements**: `docs/superpowers/specs/2026-08-16-predator-decision-contract-execution-risk-framework.md`
— this spec is that framework's concrete C++ mechanism (elements #1/#2/#3 as a callable interface,
element #5 as a structural guarantee). It does not fix any individual pattern (Elder Breakout, Turtle
Soup, Kangaroo Tail/Momentum Pinball audits remain separately scoped, consuming this infrastructure
once it exists) and makes **no schema changes** and **no live-behavior changes** — τ* is migrated onto
the new interface as a regression-safe reference implementation, not redesigned.
**Governing methodology**: `docs/superpowers/specs/2026-08-16-execution-risk-coevolution-governance-spec.md`
for any *decision* built on this infrastructure; this infrastructure itself is validated by native
unit tests (mechanism), not the twin (policy) — see Validation Strategy below.

## Purpose

The Predator Decision Contract identified that macro context (regime state, structural levels,
persistence) is currently split across at least two accessors (`ContextManager::GetLocalRiskContext()`
and `InferenceManager::Instance().HmmState()`), queried ad hoc and inconsistently per pattern — Elder
Breakout's broken, non-directional fusion (`TripleScreen3.cpp:1162-1173`) is the concrete, found proof
this inconsistency produces real bugs. This spec builds the shared mechanism so every current and
future Predator-grade decision gets macro context, a fusion interface, and safety-subordination
correctness for free, rather than each pattern reinventing (and sometimes breaking) its own.

## Components

### 1. `include/PredatorContext.h` — unified macro context, DOD-consistent

```cpp
struct PredatorContext {
    LocalRiskContext gang;       // unchanged, owned/populated by ContextManager exactly as today
    HMMStateEnum regime;         // from InferenceManager::Instance().HmmState()
    bool inPosition;             // cheap, already-known position state
    uint64_t applicabilityMask;  // see Fusion Dispatch below
};
```

Composition-by-value: a nested POD struct lays out contiguously in memory identically to a flattened
one — no indirection, no vtable, no heap allocation. `ContextManager::Instance().GetPredatorContext()`
assembles this from the two already-computed sources once per tick; it introduces no new computation,
only a cheap struct copy of data that already exists.

### 2. `include/PredatorFusion.h` — free-function fusion interface

One small, focused free function per decision, matching the free-function compute style `CLAUDE.md`
already documents as the established hot-path pattern (Task 6's MACD precedent) — not a virtual
interface:

```cpp
struct TauStarFusionResult { bool shouldExit; float effectiveThreshold; };
TauStarFusionResult FuseTauStar(const PredatorContext& ctx, const TrapModelSignal& micro);
```

Each decision gets its own result struct and function — independently reviewable, independently
testable, no shared dispatcher class to grow into a God-object.

### 3. Fusion Dispatch — broad-phase/narrow-phase applicability mask

Reuses `IndicatorManager`'s already-proven dirty-mask/trigger-mask idiom (`m_dirty_mask`,
`PRIMARY_TRIGGER_MASK`, `__builtin_ctzll`-based bit iteration in `HasSignificantChange()`/
`CheckTrigger()`) rather than inventing a new mechanism:

- Each fusion function is assigned a bit in a new `FusionKey` enum (mirrors `IndicatorKey`'s
  convention).
- `PredatorContext::applicabilityMask` is computed once per tick from the cheapest available
  preconditions: position state (flat → entry-fusion bits only; in-position → exit-fusion bits only —
  this is also the structural enforcement of contract element #5, not just an optimization: an
  entry-fusion function's bit can never be set while `inPosition` is true, so it is *impossible* to
  call it on the wrong side of position state, not merely disciplined-against) and regime-applicability
  (a given fusion may only be relevant for a subset of `HMMStateEnum` values).
- Dispatch iterates only set bits, calling only applicable fusion functions — the same
  early-out-on-a-mask shape already proven in this codebase, extended to a new use, not a new pattern.

This is a genuine performance optimization (avoiding N wasted fusion calls every tick, compounding
under `AutoLoop=1`'s every-tick-everywhere cost) *and* a correctness mechanism (position-state gating
makes contract element #5 structurally true, not just tested-for).

### 4. Reference Implementation: τ* Migration

TRAP's anticipatory τ* (Elkan 2001, τ* = C_FP/(C_FP+C_FN)) is migrated onto `FuseTauStar()` as the
first real fusion function — chosen because it's the one decision already correctly designed, making
it the best proof the interface holds up under a real, non-trivial case. This is a refactor, not a
redesign: byte-identical output before/after, verified by regression test (matching this project's
established "TDD-verified regression-safe... identical... before and after" convention, e.g. the D8
shrinkage generalization).

## Validation Strategy (mechanism vs. policy — established this session, not new)

Per the separation-of-mechanism-and-policy principle (Hydra OS, Levin et al. 1975) and this project's
own already-adopted precedent (the governance spec's schema-shape-vs-decision-agreement split;
`test_feature_scaler.cpp`'s golden-vector regression tests for the shrinkage *mechanism*, separate from
the empirical, real-data-driven derivation of the actual bound *values*):

- **This infrastructure** (context assembly, fusion dispatch, mask computation) is validated by native
  C++ unit tests — deterministic, golden-vector, no twin involved. It makes no trading decision on its
  own; there is nothing for the twin to validate.
- **Each concrete fusion decision** built on top of it (τ*'s eventual live wiring, Elder Breakout's
  fix, Turtle Soup's Predator-ization) is validated through the governance spec's twin-first promotion
  ladder, per that decision's own gating metrics — unchanged by this spec.

## Test Plan

1. `PredatorContext` assembly: golden-fixture unit test confirms it correctly reflects `LocalRiskContext`
   and `InferenceManager`'s HMM state for known inputs.
2. Applicability mask: unit tests for known position-state/regime combinations (e.g., flat +
   `GAUSSIAN_STABLE` → entry-fusion bits set, exit-fusion bits clear; in-position + hostile regime →
   correct exit-fusion bits set) — proves element #5's structural guarantee, not just documents it.
3. `FuseTauStar()`: regression test — byte-identical output vs. the pre-migration bespoke τ*
   implementation, across the existing native test fixture set.
4. Paired-opposite-direction test template (the shape that would have caught Elder Breakout):
   established here against `FuseTauStar()` as the reference case, ready for Elder Breakout's own
   future unit to reuse directly rather than invent its own test shape.
5. `./build_dll.sh` clean.

## Acceptance Criteria

1. `include/PredatorContext.h` and `include/PredatorFusion.h` exist; zero schema changes; zero
   observable live-behavior change (τ* migration is byte-identical, not a new decision).
2. Applicability mask correctly and structurally prevents entry-fusion bits from being set while
   `inPosition` is true (contract element #5), verified by test, not just asserted in review.
3. τ* successfully migrated as the reference implementation, regression-tested identical to
   pre-migration behavior.
4. `build_dll.sh` and the existing native test suite both green.

## Non-Goals

- **No schema changes** (`mts_schema.fbs`) — unchanged from the parent framework spec's scoping.
- **No fixing of Elder Breakout, Turtle Soup, or Kangaroo Tail/Momentum Pinball audits** — these
  remain separately scoped first-wave work, now consuming this infrastructure once it ships, not
  duplicated or advanced here.
- **No Python-side Predator implementation** — this is the C++ execution/risk analog only.
- **No twin-validation of the infrastructure itself** — per the Validation Strategy section; twin
  validation applies to concrete decisions built on top, not to context assembly or dispatch mechanics.
- **No new dispatcher class or registry abstraction** — the bitmask idiom is reused as-is; do not
  generalize into a plugin/registry system beyond what `IndicatorManager` already established.

## Investigation Log

- **2026-08-16**: Designed at the close of the same `superpowers:brainstorming` session that produced
  the Predator Decision Contract framework spec. Key decisions, each explicitly checked rather than
  assumed: reuse `LocalRiskContext` rather than duplicate it (confirmed its existing fields already
  cover most macro inputs, and that HMM regime state is the one piece missing from it);
  free-function-per-decision over a virtual interface (DOD/cache-locality requirement, matches
  established local precedent); broad-phase/narrow-phase applicability-mask dispatch, reusing
  `IndicatorManager`'s existing dirty-mask idiom rather than inventing a new mechanism, both for a real
  performance win under `AutoLoop=1` and as the structural enforcement of contract element #5; mechanism
  vs. policy validation split grounded in established software-engineering doctrine (Hydra OS,
  Cohn's testing pyramid, Mike Acton's DOD testing practice) and this project's own prior use of the
  identical split.
