# Schema-Derived ABI-Guard Generation (regenerate_schema.sh "Category C")

## 1. Origin

From the 2026-09-17 investigation into "how much modern, DOD-compliant C++ can `regenerate_schema.sh`
generate in response to `mts_schema.fbs` changes." Two concrete fixes shipped that session as
one-off, hand-picked instances of the *same* underlying defect class:

- `include/FeatureScaler.h`'s `DIM_AMIHUD_INDEX`/`DIM_LZ_INDEX`/`DIM_RECURRENCE_INDEX`/
  `DIM_FRACTAL_INDEX` were hand-typed integer literals mirroring `ObservationData`'s real field
  order. The file's own comments document this silently desyncing from the true schema order
  **twice** (2026-08-28, then again 2026-08-31) before finally being fixed to reference the
  already-existing, reflection-derived `MTS::Schema::Contract::kObs*` constants directly.
- `indicator_key_registry_generated.h`'s `kIndicatorKeyRegistryRows` values were hand-typed
  duplicates of `IndicatorKey.h`'s real enum values, protected only by a metadata hash that
  (subtly) hashed the wrong source file. Fixed by generating the values directly from
  `IndicatorKey.h` (`schema/scripts/generate_indicator_key_rows.py`).

Both fixes were **reactive** — applied only after a bug was found or a specific file was audited.
This spec proposes generalizing the mechanism so future structs get the same protection
*proactively*, without waiting for their own incident.

## 2. The pattern, generalized

Recurring shape: a hand-typed C++ constant (an index, a size, a field-name list) that mirrors a
schema-reflected struct's real field order or layout, with no automated check tying it back to the
schema. Every instance found so far has drifted silently at least once. The fix each time has been
the same "validate or generate directly from reflection, fail the build loudly on mismatch"
pattern (see `generate_contract_header.py`'s `validate_shared_writer_fields()`,
`validate_indicator_key_registry.py`, and the `ObservationDim`-style `kObs*` constants). What's
missing is applying that pattern to *every* candidate struct up front, not one at a time as bugs
surface.

## 3. Scope: which schema types are candidates

FlatBuffers has two kinds of aggregate: `struct` (fixed-layout, no vtable, standard-layout,
memcpy-safe — same field order as declared) and `table` (offset/vtable-based, variable layout,
fields can be added/reordered/omitted independently of declaration order). The existing
`ObservationData`/`AsymmetryContext` treatment (position-index constants + `is_standard_layout`
+ `sizeof` static_asserts + a single memcpy in `MakeObservationData`/`ToObservationArray`) is only
valid for `struct`s — it relies on field order and byte layout being fixed and predictable, which
`table`s do not guarantee.

Current `struct`s in `mts_schema.fbs` (`grep -n "^struct " mts_schema.fbs`):

| Struct | Position-index constants + sizeof guard? |
|---|---|
| `ObservationData` | ✅ done (`kObs*`, this session's `FeatureScaler.h` fix consumes it) |
| `AsymmetryContext` | ✅ done (`kAsym*`) |
| `EventHeader` | ❌ not done |
| `IndicatorState` | ❌ not done (see §5 — the highest-value target) |
| `ImbalanceObservationData` | ❌ not done |

`table`s (`RiskGateContext`, `TrainingEvent`, `Event`, etc.) get the lighter, already-established
field-*name*-list treatment instead (no position constants, no memcpy, no sizeof guard — position
in a table's wire format isn't meaningful the same way).

## 4. Proposed mechanism

For every schema `struct` not yet covered, generate (mirroring the existing `ObservationData`
renderer in `generate_contract_header.py`, generalized to take any struct's reflection key):

1. Per-field position constants (`k<Struct><Field> = i`), so hand-written code that currently
   indexes into that struct's layout by a bare integer can reference a named, reflection-derived
   constant instead.
2. `static_assert(std::is_standard_layout<MTS::Schema::X>::value, ...)`.
3. `static_assert(sizeof(MTS::Schema::X) == kXDim * sizeof(FieldType), ...)` — deliberately **not**
   attempting to independently predict FlatBuffers' own struct-padding/alignment arithmetic in
   Python; the C++ compiler is the source of truth for the real byte size, the generator only needs
   the field *count* (and, if the struct mixes field types like `IndicatorState` does, the count per
   type) from jsonschema reflection — exactly the same division of labor the existing
   `ObservationData`/`AsymmetryContext` checks already use.

## 5. Priority target: `IndicatorState` / `IndicatorLayout.h`

This is the single highest-value application of the general mechanism, higher priority than
`EventHeader`/`ImbalanceObservationData` (which have no documented incident history yet).

`include/IndicatorLayout.h`'s `kIndicatorLayout` table hand-maintains, per `IndicatorKey`, which
packed `Int8`/`Float32` storage block it lives in and its *position within that block* — a value
that only means something if it matches `IndicatorState`'s real declared field order
(`mts_schema.fbs:221`, ~40+ fields). Per that header's own comment, this mapping was produced by a
**one-time manual audit** (Task 2, 2026-08-05) and has had **zero automated cross-check against the
real schema since**. If `IndicatorState` ever gains, loses, or reorders a field, `IndicatorLayout.h`'s
positions would silently point at the wrong slot — the exact defect class already proven to bite
`FeatureScaler.h` (twice) and the `IndicatorKey` registry, just not yet caught here because nobody
has changed `IndicatorState`'s layout since the audit.

Concrete next step (not yet built): generate `IndicatorState`'s per-field position constants and
per-type field counts (Int8 count, Float32 count) the same way `ObservationDim` works today, then
add a `validate_indicator_layout_positions.py` (same family as `validate_indicator_key_registry.py`)
that checks `kIndicatorLayout`'s `StorageBlock::Int8`/`StorageBlock::Float32` row counts and maximum
`position` values are consistent with those generated counts. This does **not** verify that a given
`IndicatorKey` maps to the *correct* field (that's still a human judgment call, like
`IndicatorBindingPolicy`'s wire classification) — it only closes the cheap, mechanical, highest-value
part of the risk: silent count/order drift after a schema edit.

## 6. Explicit non-goals

- No attempt to auto-generate `table` structs' full C++ bodies (too much intertwined domain logic —
  e.g. `RiskGateContext`'s consumers, `IndicatorBindingPolicy`'s `wire_class`/`sink` classification).
- No attempt to replicate FlatBuffers' own struct-padding/alignment arithmetic independently in
  Python (see §4 item 3 — the generator only needs field count/type from reflection; the compiler
  validates the real byte size).
- Does not touch calibration or classification values that require domain judgment
  (`FeatureScaler`'s tuned floats, `IndicatorBindingPolicy`'s per-key wire classification,
  `IndicatorLayout.h`'s per-key *correctness* as opposed to its aggregate count) — those remain
  hand-authored, with validation (not full generation) as the safety net, same as today.

## 7. Implementation sketch (not yet built — this document is the spec, not the change)

1. Generalize `generate_contract_header.py`'s `render_observation_constants()` into a
   `render_struct_position_constants(prefix, field_names)` usable for any struct key, and reuse it
   for `IndicatorState`/`EventHeader`/`ImbalanceObservationData` in addition to the two structs
   already covered.
2. Add each new struct's marker token to the `mts_schema_contract_generated.h` heredoc in
   `regenerate_schema.sh`, following the exact `// __GENERATED_OBSERVATION_FIELD_CONSTANTS__`
   precedent.
3. For `IndicatorState` specifically, follow up with `validate_indicator_layout_positions.py` per
   §5, wired into `regenerate_schema.sh` the same way `validate_indicator_key_registry.py` is today.

## 8. Open questions

- Whether `EventHeader`/`ImbalanceObservationData` warrant the same treatment now or should wait
  until (if ever) they show their own incident — no evidence of drift yet, unlike `IndicatorState`'s
  audit-only history.
- Whether the per-type (Int8 vs Float32) field-count split needed for `IndicatorState`'s validation
  script is cleanly derivable from jsonschema's own type tags, or needs a small amount of
  schema-comment convention (e.g. explicit block markers) to disambiguate reliably.
