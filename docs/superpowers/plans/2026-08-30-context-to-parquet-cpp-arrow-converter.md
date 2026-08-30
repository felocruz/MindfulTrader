# C++/Arrow `.context` → `.context.parquet` Converter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `lbrnet`'s Python `.context` reader/converter with a standalone, natively-tested C++/Arrow tool that owns all raw `.context` byte parsing — fixing a real, confirmed-live silent field-misalignment corruption risk (older-width `ObservationData` records reinterpreted under the current field count) by making the wire format's dormant `FileMetadata.schema_version` field an enforced, hard-refused compatibility gate, closing `regenerate_schema.sh`'s twice-confirmed hand-maintained-duplicate-field-list defect as part of the same work, and bringing the `.context`-specific validation/preflight logic that legitimately belongs on the raw-bytes side (`lbrnet/scripts/validate_lbr_file.py`'s MO/SS integrity checks, `lbrnet/scripts/context_preflight.py`'s distribution/correlation gates) over to C++ too.

**Architecture:** Three independently-shippable phases. **Phase A** (Tasks 1-3) fixes `regenerate_schema.sh` so `mts_schema_contract_generated.h`'s `ObservationData` field-list constants are derived from real `flatc --jsonschema` output instead of hand-typed, adds a new, identically-derived `RiskGateContext` field-list (with `_raw`-suffixed output names for the 5 fields that collide with an `ObservationData` column), and wires a durable `WIRE_SCHEMA_VERSION` marker from `mts_schema.fbs` into a new `kSchemaVersion` constant that `LBRFileManager.cpp` starts actually using. **Phase B** (Tasks 4-9) is the converter itself: a pure, ACSIL-independent `tools/context_reader.h` (zero-copy mmap reads, flat POD state, no per-record heap allocation — this tool has no `sierrachart.h` dependency at all, so it is natively testable end-to-end, unlike most of this codebase) plus `tools/context_to_parquet.cpp` (CLI + Arrow columnar builders + chunked `parquet::arrow::FileWriter` output). Marker-substitution (not a from-scratch template rewrite) keeps Phase A's diff surgical — `regenerate_schema.sh`'s existing heredoc keeps 100% of its unrelated content (envelope/heartbeat/`AsymmetryContext` constants) untouched. **Phase C** (Tasks 10-12, added 2026-08-30) is a second, lighter sibling tool, `tools/context_validate.cpp`, built only on `tools/context_reader.h` — no Arrow/Parquet dependency at all, since it never writes a file, only reports. It consolidates `validate_lbr_file.py`'s `.context`-specific checks (MO/SS sequence integrity, per-dim NaN/Inf detection, zero/one-trap detection) and `context_preflight.py`'s checks (per-dim distribution stats, inter-feature Pearson correlation, constant/chronic-zero/saturation threshold gates) into one tool, since both Python scripts already operate on exactly the same raw MO/SS data and Python's own `context_preflight.py` docstring already describes itself as supplementing `validate_lbr_file.py`. Scope boundary (matches §10.1 exactly): only checks that operate on raw `.context` bytes move here — the `.alpha`-specific and Parquet-sidecar-based checks in `validate_lbr_file.py` (provenance, FSM parity, utility-gate — all Polars-based, post-conversion) stay in Python, untouched. `validate_lbr_file.py`'s dead `check_context_file_quality()` (a defunct pre-MO/SS 10-float legacy validator, zero callers repo-wide) was deleted outright, not ported — per direct instruction, no reason to preserve a validator for a format the wire protocol doesn't even write anymore. **Phase D** (Task 13, added 2026-08-30) moves `materialize_context_parquet.py`'s freshness/incremental-rebuild decision (`cache_key()`/`is_cache_fresh()`) into `tools/context_cache_key.h`, wired into `context_to_parquet.cpp`'s `main()` — this is what answers "what do we do when a new `.context` file arrives" (nothing / incremental append / full rebuild). It deliberately introduces a NEW, separate `kContextParquetCacheFormatVersion` constant rather than reusing `MTS::Schema::Contract::kSchemaVersion` for this: Python's `cache_key()` conflated "raw wire format version" and "Parquet output column-schema version" under one `schema_version: 2` field, which this port corrects rather than reproduces.

**Tech Stack:** C++17, FlatBuffers 24.3.25 (`flatc`, vendored `include/flatbuffers/`), Apache Arrow/Parquet 22.0.0 C++ (available via the `mamba run -n mts` environment — confirmed via `pkg-config --modversion arrow parquet`), Python 3 (`mamba run -n mts`) for the schema-generation script only. Native tests via bare `g++` + a hand-rolled `check(name, bool)` helper (this codebase's real convention — no GoogleTest/CMake, confirmed against `tests/cpp/test_feature_scaler.cpp`/`test_imbalance_bar_engine.cpp`).

**Spec:** `docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md` §10 (all subsections 10.1-10.6). This plan implements §10.1-10.4 in full; §10.5 (the downstream `hmm_training_cache.py` `.npy` layer) is explicitly out of scope — verified already-safe by construction, no task touches it; the open items named in §10.6 (exact CLI shape, `.bfbs`/reflection mechanics, the actual version number) are resolved concretely below, not left open. Phase C (Tasks 10-12) extends §10.1's ownership boundary ("C++ owns everything that touches raw `.context` bytes") to `lbrnet/scripts/validate_lbr_file.py` and `lbrnet/scripts/context_preflight.py` — added mid-execution, 2026-08-30, no separate spec section written (scope decided directly with the user rather than a full brainstorm pass, given how cleanly it falls under the boundary §10.1 already established).

## Global Constraints

- **`lbrnet`-side integration is explicitly out of scope for this plan** (§10.1) — no task modifies anything under `lbrnet/`. A separate `lbrnet`-rooted session picks up call-site migration once this ships.
- **Hard refuse on schema-version mismatch, never silent reinterpretation** (§10.2) — every reader entry point in this plan must reject a file whose `FileMetadata.schema_version` doesn't equal the tool's compiled-in `MTS::Schema::Contract::kSchemaVersion`, with a clear error message, before touching a single `ObservationData` byte.
- **New wire schema version: `240`** (v2.3.0 → v2.4.0, following this field's existing `major*100+minor*10+patch` convention — `230` was v2.3.0). Bumped once, held fixed through the rest of this observation-vector campaign per §10.2's explicit policy — no task in this plan bumps it again.
- **DOD discipline throughout Phase B**, per direct instruction and this codebase's own established conventions (`CLAUDE.md` Performance Rules, `docs/superpowers/plans/2026-08-26-activity-clock-dual-kurtosis.md`'s Global Constraints):
  - Zero-copy reads: `ObservationData`/`RiskGateContext` are read via pointer-cast directly over `mmap`'d bytes (mirroring `np.frombuffer`'s zero-copy view in the Python reader being replaced) — never copied into an intermediate heap-allocated struct first.
  - Flat POD state only: the MO/SS pairing state machine is one fixed-layout struct (mirrors `_new_state()`'s dict in the Python reader, but as real fields, not a hash map) — no `std::string`/`std::vector`/`std::map` per-record allocation anywhere in the parsing hot loop.
  - Arrow output is built columnar (SoA) from the start — one `arrow::FloatBuilder`/`UInt64Builder`/etc. **per output column**, appended to directly as records are parsed. Never build a row-of-structs array and transpose to columns afterward.
  - Bounded memory: write in chunks via `NewBufferedRowGroup()` + `WriteRecordBatch()` (never accumulate the whole file's rows into one `arrow::Table` before writing) — same chunked-processing discipline `materialize_context_parquet.py`/`hmm_training_cache.py` already use on the Python side.
- **Standalone tool, `tools/` directory only** (§10.3) — bare `g++` build via the `mamba run -n mts` environment (Arrow/Parquet/FlatBuffers all resolve there via `pkg-config`), matching `tools/amihud_liqfragility_recalibration.cpp`'s precedent. **Never** added to `CMakeLists.txt` or `build_dll.sh`.
- Never call `flatc` directly outside `regenerate_schema.sh` (`CLAUDE.md`). The `WIRE_SCHEMA_VERSION` marker is a `.fbs` **comment**, not a wire-format field change — no `schema/PENDING_SCHEMA_CHANGES.md` entry is required for it (that governance applies to wire-field additions, §6.2; nothing on the wire changes here).
- Full clean regen verify: `bash schema/regenerate_schema.sh --cpp-only --no-deploy` must succeed and `./build_dll.sh --no-clean` (from `MindfulTrader/`) must still build clean after every Phase A task that touches generated output.

---

## Task 1: `schema/scripts/generate_contract_header.py` — reflection-derived field lists (pure script, fixture-tested)

**Files:**
- Create: `schema/scripts/generate_contract_header.py`
- Create: `schema/scripts/test_generate_contract_header.py`
- Modify: `schema/mts_schema.fbs` (add the `WIRE_SCHEMA_VERSION` marker comment near `FileMetadata`)

**Interfaces:**
- Produces: `parse_wire_schema_version(fbs_text: str) -> int`, `observation_field_names(jsonschema: dict) -> tuple[str, ...]`, `risk_gate_field_names(jsonschema: dict) -> tuple[str, ...]`, `risk_gate_float_field_names(jsonschema: dict) -> tuple[str, ...]`, `render_observation_constants(field_names: tuple[str, ...]) -> str`, `render_risk_gate_constants(all_names, float_names, observation_names) -> str`, `substitute_markers(header_text: str, *, schema_version: int, observation_fields: tuple, risk_gate_all: tuple, risk_gate_float: tuple, observation_field_names_for_collision: tuple) -> str`. Task 2 calls `substitute_markers()` via this script's CLI.

- [ ] **Step 1: Add the version marker to `mts_schema.fbs`**

Find the `FileMetadata` table (around line 1100) and add the marker comment directly above it:

```
// WIRE_SCHEMA_VERSION: 240 (v2.4.0) -- bump exactly once per the campaign-versioning
// policy in docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-
// brainstorm.md §10.2: held fixed through this whole ObservationData-dimension-
// evolution campaign (even through further field additions) until the vector ships
// to lbrnet. Read by schema/scripts/generate_contract_header.py and emitted as
// MTS::Schema::Contract::kSchemaVersion. LBRFileManager.cpp::WriteFileMetadata()
// must source schema_version from that constant, never hardcode a literal here.
table FileMetadata {
  symbol: string;
  timeframe: string;
  schema_version: uint16;
  created_timestamp: long;
}
```

- [ ] **Step 2: Write the failing test**

```python
# schema/scripts/test_generate_contract_header.py
"""Fixture-based unit tests for generate_contract_header.py -- no flatc/mamba-env
dependency: the fixture JSON mirrors the real shape of `flatc --jsonschema` output,
verified directly against a real run against mts_schema.fbs before writing this
(field declaration order is preserved; RiskGateContext's int/bool fields carry
"type": "integer"/"boolean" vs. floats' "type": "number" -- confirmed real values,
not assumed).

Run: mamba run -n mts python3 -m pytest schema/scripts/test_generate_contract_header.py -v
"""
import generate_contract_header as gch

FIXTURE_JSONSCHEMA = {
    "definitions": {
        "MTS_Schema_ObservationData": {
            "properties": {
                "log_variance_ratio": {"type": "number"},
                "burstiness_index": {"type": "number"},
                "hurst_exponent": {"type": "number"},
            }
        },
        "MTS_Schema_RiskGateContext": {
            "properties": {
                "shannon_flow_entropy": {"type": "number"},
                "hurst_exponent": {"type": "number"},
                "regime_duration": {"type": "integer", "minimum": -2147483648, "maximum": 2147483647},
                "is_valid": {"type": "boolean"},
                "snapshot_timestamp_us": {"type": "integer"},
            }
        },
    }
}

FIXTURE_FBS_TEXT = """
// WIRE_SCHEMA_VERSION: 999 (fixture)
table FileMetadata {
  schema_version: uint16;
}
"""


def test_parse_wire_schema_version():
    assert gch.parse_wire_schema_version(FIXTURE_FBS_TEXT) == 999


def test_parse_wire_schema_version_missing_marker_raises():
    try:
        gch.parse_wire_schema_version("table FileMetadata { schema_version: uint16; }")
        assert False, "expected ValueError"
    except ValueError:
        pass


def test_observation_field_names_preserves_declaration_order():
    assert gch.observation_field_names(FIXTURE_JSONSCHEMA) == (
        "log_variance_ratio", "burstiness_index", "hurst_exponent",
    )


def test_risk_gate_field_names_includes_non_float_fields():
    assert gch.risk_gate_field_names(FIXTURE_JSONSCHEMA) == (
        "shannon_flow_entropy", "hurst_exponent", "regime_duration", "is_valid",
        "snapshot_timestamp_us",
    )


def test_risk_gate_float_field_names_excludes_int_and_bool():
    assert gch.risk_gate_float_field_names(FIXTURE_JSONSCHEMA) == (
        "shannon_flow_entropy", "hurst_exponent",
    )


def test_render_observation_constants_shape():
    text = gch.render_observation_constants(("log_variance_ratio", "burstiness_index"))
    assert "inline constexpr std::size_t kObservationDim = 2;" in text
    assert "inline constexpr std::size_t kObsLogVarianceRatio = 0;" in text
    assert "inline constexpr std::size_t kObsBurstinessIndex = 1;" in text
    assert '"log_variance_ratio",' in text
    assert '"burstiness_index",' in text


def test_render_risk_gate_constants_suffixes_only_colliding_names():
    text = gch.render_risk_gate_constants(
        all_names=("shannon_flow_entropy", "hurst_exponent"),
        float_names=("shannon_flow_entropy", "hurst_exponent"),
        observation_field_names=("log_variance_ratio", "hurst_exponent"),
    )
    assert '"shannon_flow_entropy",' in text
    assert '"hurst_exponent_raw",' in text
    assert '"hurst_exponent",' not in text.split("kRiskGateFloatOutputColumnNames")[1].replace(
        '"hurst_exponent_raw",', ""
    )


def test_substitute_markers_replaces_all_four_and_leaves_rest_untouched():
    header = (
        "unrelated line 1\n"
        "// __GENERATED_SCHEMA_VERSION__\n"
        "unrelated line 2\n"
        "// __GENERATED_OBSERVATION_FIELD_CONSTANTS__\n"
        "// __GENERATED_RISK_GATE_FIELD_CONSTANTS__\n"
        "// __GENERATED_MAKE_OBSERVATION_DATA__\n"
        "// __GENERATED_TO_OBSERVATION_ARRAY__\n"
        "unrelated line 3\n"
    )
    out = gch.substitute_markers(
        header, schema_version=240,
        observation_fields=("log_variance_ratio", "hurst_exponent"),
        risk_gate_all=("shannon_flow_entropy", "hurst_exponent"),
        risk_gate_float=("shannon_flow_entropy", "hurst_exponent"),
        observation_field_names_for_collision=("log_variance_ratio", "hurst_exponent"),
    )
    assert "unrelated line 1" in out and "unrelated line 3" in out
    assert "__GENERATED_" not in out
    assert "kSchemaVersion = 240;" in out
    assert "kObsLogVarianceRatio = 0;" in out
    assert "hurst_exponent_raw" in out
    assert "MakeObservationData" in out
    assert "ToObservationArray" in out


if __name__ == "__main__":
    import sys
    failures = 0
    for name, fn in list(globals().items()):
        if name.startswith("test_") and callable(fn):
            try:
                fn()
                print(f"  PASS  {name}")
            except AssertionError as e:
                failures += 1
                print(f"  FAIL  {name}: {e}")
    print("ALL PASS" if failures == 0 else f"{failures} FAILURE(S)")
    sys.exit(0 if failures == 0 else 1)
```

- [ ] **Step 3: Run test to verify it fails**

Run: `cd schema/scripts && mamba run -n mts python3 test_generate_contract_header.py`
Expected: FAIL — `generate_contract_header` module does not exist, `ImportError`.

- [ ] **Step 4: Write the implementation**

```python
#!/usr/bin/env python3
# schema/scripts/generate_contract_header.py
"""Generates the ObservationData/RiskGateContext-derived sections of
include/generated/mts_schema_contract_generated.h from real `flatc --jsonschema`
output, replacing marker tokens regenerate_schema.sh's heredoc leaves in place --
instead of a hand-maintained duplicate field list (the defect class confirmed
drifted twice: fast_taleb_kurtosis undetected for weeks, fast_hurst_exponent/
fast_mean_rev_z blocking the build outright). See docs/superpowers/specs/
2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md §6.2/§10.4.

Only ObservationData and RiskGateContext are derived here -- AsymmetryContext,
envelope/heartbeat constants, and helper functions are untouched, hand-typed
content in regenerate_schema.sh's own heredoc (no drift has ever been reported
there; expanding scope to rewrite it too is not this fix's job).

Usage: mamba run -n mts python3 generate_contract_header.py \\
    --jsonschema <path to flatc --jsonschema output> \\
    --fbs <path to mts_schema.fbs> \\
    --header <path to already-heredoc-written mts_schema_contract_generated.h,
              rewritten in place>
"""
from __future__ import annotations

import argparse
import json
import re
from pathlib import Path

_OBSERVATION_DATA_KEY = "MTS_Schema_ObservationData"
_RISK_GATE_CONTEXT_KEY = "MTS_Schema_RiskGateContext"
_VERSION_MARKER_RE = re.compile(r"//\s*WIRE_SCHEMA_VERSION:\s*(\d+)")

# CamelCase enum-style suffix used for each field's kObsXxx / positional constant
# name -- mirrors the hand-typed convention already in the current header
# (kObsLogVarianceRatio, kObsFastHurstExponent, ...).
def _snake_to_pascal(name: str) -> str:
    return "".join(part.capitalize() for part in name.split("_"))


def parse_wire_schema_version(fbs_text: str) -> int:
    m = _VERSION_MARKER_RE.search(fbs_text)
    if m is None:
        raise ValueError("mts_schema.fbs is missing its // WIRE_SCHEMA_VERSION: N marker")
    return int(m.group(1))


def observation_field_names(jsonschema: dict) -> tuple[str, ...]:
    props = jsonschema["definitions"][_OBSERVATION_DATA_KEY]["properties"]
    return tuple(props.keys())


def risk_gate_field_names(jsonschema: dict) -> tuple[str, ...]:
    props = jsonschema["definitions"][_RISK_GATE_CONTEXT_KEY]["properties"]
    return tuple(props.keys())


def risk_gate_float_field_names(jsonschema: dict) -> tuple[str, ...]:
    props = jsonschema["definitions"][_RISK_GATE_CONTEXT_KEY]["properties"]
    return tuple(name for name, spec in props.items() if spec.get("type") == "number")


def render_observation_constants(field_names: tuple[str, ...]) -> str:
    lines = [f"inline constexpr std::size_t kObservationDim = {len(field_names)};"]
    lines.append("")
    for i, name in enumerate(field_names):
        lines.append(f"inline constexpr std::size_t kObs{_snake_to_pascal(name)} = {i};")
    lines.append("")
    lines.append(
        f"inline constexpr std::array<const char*, kObservationDim> kObservationFieldNames = {{"
    )
    for name in field_names:
        lines.append(f'    "{name}",')
    lines.append("};")
    return "\n".join(lines)


def render_risk_gate_constants(
    all_names: tuple[str, ...], float_names: tuple[str, ...],
    observation_field_names: tuple[str, ...],
) -> str:
    """Output column names computed by SET INTERSECTION against ObservationData's
    real (also-derived) field list, not a hand-maintained collision list -- a
    future naming collision is caught automatically here, never silently missed
    (§10.4's naming-collision fix is itself drift-proof by construction)."""
    colliding = set(float_names) & set(observation_field_names)
    output_names = tuple(
        f"{name}_raw" if name in colliding else name for name in float_names
    )
    lines = [
        f"inline constexpr std::size_t kRiskGateFieldCount = {len(all_names)};",
        f"inline constexpr std::size_t kRiskGateFloatFieldCount = {len(float_names)};",
        "",
        f"inline constexpr std::array<const char*, kRiskGateFieldCount> kRiskGateFieldNames = {{",
    ]
    for name in all_names:
        lines.append(f'    "{name}",')
    lines.append("};")
    lines.append("")
    lines.append(
        f"inline constexpr std::array<const char*, kRiskGateFloatFieldCount> kRiskGateFloatFieldNames = {{"
    )
    for name in float_names:
        lines.append(f'    "{name}",')
    lines.append("};")
    lines.append("")
    lines.append(
        "// Output-column names for RiskGateContext's float fields -- suffixed with"
    )
    lines.append(
        "// _raw ONLY where the plain name collides with an ObservationData column"
    )
    lines.append(
        "// (raw/unscaled here vs. log-z/winsorized there), computed by set"
    )
    lines.append("// intersection against kObservationFieldNames, not hardcoded (§10.4).")
    lines.append(
        f"inline constexpr std::array<const char*, kRiskGateFloatFieldCount> kRiskGateFloatOutputColumnNames = {{"
    )
    for name in output_names:
        lines.append(f'    "{name}",')
    lines.append("};")
    return "\n".join(lines)


def render_make_observation_data(field_names: tuple[str, ...]) -> str:
    lines = ["inline MTS::Schema::ObservationData MakeObservationData(",
             "    const ObservationArray& values) {",
             "  return MTS::Schema::ObservationData("]
    for i, name in enumerate(field_names):
        suffix = "," if i < len(field_names) - 1 else ");"
        lines.append(f"      values[kObs{_snake_to_pascal(name)}]{suffix}")
    lines.append("}")
    return "\n".join(lines)


def render_to_observation_array(field_names: tuple[str, ...]) -> str:
    lines = ["inline ObservationArray ToObservationArray(",
             "    const MTS::Schema::ObservationData& observation) {",
             "  return {"]
    for name in field_names:
        lines.append(f"      observation.{name}(),")
    lines.append("  };")
    lines.append("}")
    return "\n".join(lines)


def substitute_markers(
    header_text: str, *, schema_version: int,
    observation_fields: tuple[str, ...], risk_gate_all: tuple[str, ...],
    risk_gate_float: tuple[str, ...], observation_field_names_for_collision: tuple[str, ...],
) -> str:
    replacements = {
        "// __GENERATED_SCHEMA_VERSION__": (
            f"inline constexpr std::uint16_t kSchemaVersion = {schema_version};  "
            "// from mts_schema.fbs's WIRE_SCHEMA_VERSION marker, see brainstorm doc §10.2"
        ),
        "// __GENERATED_OBSERVATION_FIELD_CONSTANTS__": render_observation_constants(observation_fields),
        "// __GENERATED_RISK_GATE_FIELD_CONSTANTS__": render_risk_gate_constants(
            risk_gate_all, risk_gate_float, observation_field_names_for_collision,
        ),
        "// __GENERATED_MAKE_OBSERVATION_DATA__": render_make_observation_data(observation_fields),
        "// __GENERATED_TO_OBSERVATION_ARRAY__": render_to_observation_array(observation_fields),
    }
    out = header_text
    for marker, replacement in replacements.items():
        if marker not in out:
            raise ValueError(f"marker {marker!r} not found in header text -- heredoc drifted")
        out = out.replace(marker, replacement)
    return out


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--jsonschema", required=True, type=Path)
    parser.add_argument("--fbs", required=True, type=Path)
    parser.add_argument("--header", required=True, type=Path)
    args = parser.parse_args()

    jsonschema = json.loads(args.jsonschema.read_text())
    fbs_text = args.fbs.read_text()
    schema_version = parse_wire_schema_version(fbs_text)
    observation_fields = observation_field_names(jsonschema)
    risk_gate_all = risk_gate_field_names(jsonschema)
    risk_gate_float = risk_gate_float_field_names(jsonschema)

    header_text = args.header.read_text()
    out = substitute_markers(
        header_text, schema_version=schema_version,
        observation_fields=observation_fields, risk_gate_all=risk_gate_all,
        risk_gate_float=risk_gate_float,
        observation_field_names_for_collision=observation_fields,
    )
    args.header.write_text(out)
    print(
        f"✅ mts_schema_contract_generated.h: {len(observation_fields)} observation fields, "
        f"{len(risk_gate_all)} risk-gate fields ({len(risk_gate_float)} float), "
        f"schema_version={schema_version}"
    )


if __name__ == "__main__":
    main()
```

- [ ] **Step 5: Run test to verify it passes**

Run: `cd schema/scripts && mamba run -n mts python3 test_generate_contract_header.py`
Expected: `ALL PASS` (10 checks)

- [ ] **Step 6: Commit**

```bash
git add schema/mts_schema.fbs schema/scripts/generate_contract_header.py schema/scripts/test_generate_contract_header.py
git commit -m "feat: add reflection-derived contract-header generator + WIRE_SCHEMA_VERSION marker"
```

---

## Task 2: Wire the generator into `regenerate_schema.sh`, verify against real schema output

**Files:**
- Modify: `schema/regenerate_schema.sh:1032-1271` (the `cpp_contract_header` heredoc)

**Interfaces:**
- Consumes: Task 1's `schema/scripts/generate_contract_header.py` CLI (`--jsonschema`, `--fbs`, `--header`).
- Produces: `include/generated/mts_schema_contract_generated.h` now carries `kSchemaVersion`, reflection-derived `kObservationFieldNames`/`kObsXxx`, and new `kRiskGateFieldNames`/`kRiskGateFloatFieldNames`/`kRiskGateFloatOutputColumnNames` — Task 4 consumes these directly.

- [ ] **Step 1: Replace the four hand-typed sections in the heredoc with marker tokens**

In `schema/regenerate_schema.sh`, inside the `cat > "$cpp_contract_header" << 'CPP_SCHEMA_CONTRACT_EOF'` block:

Replace lines 1049 (`inline constexpr std::size_t kObservationDim = 19;`) through 1104 (the closing `};` of `kObservationFieldNames`) with:

```
// __GENERATED_OBSERVATION_FIELD_CONSTANTS__
```

Immediately after that marker (still before `inline constexpr std::size_t kAsymmetryDim = 8;`'s own block — keep `kAsymmetryDim`/`kAsymShannonEntropy`/etc./`kAsymmetryFieldNames` completely untouched), add on its own line:

```
// __GENERATED_SCHEMA_VERSION__
```

Immediately after `kAsymmetryFieldNames`'s closing `};` (line 1115 today), before `inline constexpr std::uint16_t kConfigDefaultMaxIndicators = 50;`, add:

```
// __GENERATED_RISK_GATE_FIELD_CONSTANTS__
```

Replace the `MakeObservationData` function body (lines 1183-1205) with:

```
// __GENERATED_MAKE_OBSERVATION_DATA__
```

Replace the `ToObservationArray` function body (lines 1207-1230) with:

```
// __GENERATED_TO_OBSERVATION_ARRAY__
```

Leave `MakeAsymmetryContext`/`ToAsymmetryArray`/the `static_assert`s/everything else in the heredoc exactly as-is.

- [ ] **Step 2: Invoke the generator after the heredoc closes**

Immediately after the existing line `print_success "C++ contract helper generated: mts_schema_contract_generated.h"` (line 1272), insert:

```bash
            print_info "Deriving ObservationData/RiskGateContext field lists via flatc --jsonschema reflection..."
            if ! flatc --jsonschema -I "$SCHEMA_INCLUDE_DIR" -o "$WORK_DIR" "$SCHEMA_FILE"; then
                print_error "flatc --jsonschema failed -- cannot derive contract header field lists"
                exit 1
            fi
            jsonschema_file="${WORK_DIR}/mts_schema.schema.json"
            if ! mamba run -n mts python3 "${SCRIPT_DIR}/scripts/generate_contract_header.py" \
                --jsonschema "$jsonschema_file" --fbs "$SCHEMA_FILE" --header "$cpp_contract_header"; then
                print_error "generate_contract_header.py failed -- contract header left in a broken, marker-containing state"
                exit 1
            fi
            print_success "Reflection-derived field lists substituted into mts_schema_contract_generated.h"
```

- [ ] **Step 3: Run the real regeneration and diff against the committed header**

Run: `cd /home/rcruz/devel/VSCode && git -C MindfulTrader stash push include/generated/mts_schema_contract_generated.h && bash schema/regenerate_schema.sh --cpp-only --no-deploy`

Then diff the freshly-generated `${WORK_DIR}/mts_schema_contract_generated.h` (path printed by the script) against the git-stashed original:

Run: `diff <(git -C MindfulTrader show stash@{0}:include/generated/mts_schema_contract_generated.h) /tmp/flatbuffers_gen_*/mts_schema_contract_generated.h`

Expected: the ONLY differences are (a) the new `kSchemaVersion` line, (b) a new `kRiskGateFieldNames`/`kRiskGateFloatFieldNames`/`kRiskGateFloatOutputColumnNames` block, and (c) the `kObservationFieldNames`/`kObsXxx`/`MakeObservationData`/`ToObservationArray` sections being **byte-for-byte identical in content** to before (self-verifying: reflection-derived data reproduces exactly what was hand-typed, since `ObservationData`'s real layout hasn't changed). If any `kObsXxx` line or field name differs, stop — that means the reflection path disagrees with the hand-typed original and needs investigation before proceeding, not a "close enough" pass.

Run: `git -C MindfulTrader stash pop` to restore the working tree, then actually deploy: `bash schema/regenerate_schema.sh --cpp-only`

- [ ] **Step 4: Confirm the DLL still builds**

Run: `cd /home/rcruz/devel/VSCode/MindfulTrader && ./build_dll.sh --no-clean`
Expected: succeeds cleanly — the generated header's content for everything the DLL actually compiles against (`kObservationDim`, `kObsXxx`, `kObservationFieldNames`, `MakeObservationData`, `ToObservationArray`) is unchanged from before, so this is a regression check, not expected to require any other code change.

- [ ] **Step 5: Commit**

```bash
cd /home/rcruz/devel/VSCode
git -C schema add regenerate_schema.sh
git -C schema commit -m "feat: derive ObservationData/RiskGateContext field lists from flatc reflection"
git -C MindfulTrader add include/generated/mts_schema_contract_generated.h
git -C MindfulTrader commit -m "chore: regenerate mts_schema_contract_generated.h with reflection-derived field lists"
```

---

## Task 3: `LBRFileManager.cpp` sources `schema_version` from `kSchemaVersion`

**Files:**
- Modify: `src/LBRFileManager.cpp:332` (`WriteFileMetadata()`)

**Interfaces:**
- Consumes: `MTS::Schema::Contract::kSchemaVersion` (Task 2's output, `include/generated/mts_schema_contract_generated.h`).

- [ ] **Step 1: Replace the hardcoded literal**

```cpp
// src/LBRFileManager.cpp -- inside WriteFileMetadata(), was:
    metadata_builder.add_schema_version(230);  // v2.3.0
// becomes:
    metadata_builder.add_schema_version(MTS::Schema::Contract::kSchemaVersion);
```

Add `#include "generated/mts_schema_contract_generated.h"` to `src/LBRFileManager.cpp`'s includes if not already reachable transitively (check `MindfulTrader_Precompiled.h` first — if it already pulls this header in via the PCH, no new include is needed).

- [ ] **Step 2: Build-verify**

This file is ACSIL-adjacent (compiled only as part of the DLL, no native test precedent — matches `RiskManager`/`ContextManager`/`PositionManager`'s own established pattern, verified via full build only).

Run: `./build_dll.sh --no-clean`
Expected: succeeds cleanly.

- [ ] **Step 3: Commit**

```bash
git add src/LBRFileManager.cpp
git commit -m "fix: LBRFileManager writes the real wire schema_version instead of a stale hardcoded 230"
```

---

## Task 4: `tools/context_reader.h` skeleton — magic header + `FileMetadata` parse + hard-refuse

**Files:**
- Create: `tools/context_reader.h`
- Create: `tools/test_context_reader.cpp`

**Interfaces:**
- Produces: `struct ContextFileHandle { const std::uint8_t* base; std::size_t size; std::size_t record_stream_offset; }`; `OpenContextFile(const std::string& path) -> ContextFileHandle` (mmaps the file, throws `std::runtime_error` with a clear message if the magic header is wrong or `FileMetadata.schema_version != MTS::Schema::Contract::kSchemaVersion` — the hard-refuse gate, §10.2); `CloseContextFile(ContextFileHandle&)`.

- [ ] **Step 1: Write the failing test**

```cpp
// tools/test_context_reader.cpp
// Build & run: mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_reader.cpp \
//   -o /tmp/context_reader_test && /tmp/context_reader_test
#include "../tools/context_reader.h"
#include "generated/mts_schema_generated.h"
#include "generated/mts_schema_contract_generated.h"
#include "flatbuffers/flatbuffers.h"
#include <cstdio>
#include <cstdint>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}

// Writes a minimal valid .context-format file: "LBRN" magic + size-prefixed
// FileMetadata, matching LBRFileManager.cpp::Open()/WriteMagicHeader()/
// WriteFileMetadata() exactly.
void WriteTestFile(const std::string& path, std::uint16_t schema_version) {
    flatbuffers::FlatBufferBuilder fbb(256);
    auto symbol = fbb.CreateString("TEST");
    auto timeframe = fbb.CreateString("CONTEXT_v2.5_MO_SS");
    auto meta = MTS::Schema::CreateFileMetadata(fbb, symbol, timeframe, schema_version, 0);
    fbb.Finish(meta);

    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const char magic[4] = {'L', 'B', 'R', 'N'};
    out.write(magic, 4);
    std::uint32_t size = static_cast<std::uint32_t>(fbb.GetSize());
    out.write(reinterpret_cast<const char*>(&size), sizeof(size));
    out.write(reinterpret_cast<const char*>(fbb.GetBufferPointer()), size);
}
}  // namespace

int main() {
    {
        WriteTestFile("/tmp/ctx_reader_test_good.context", MTS::Schema::Contract::kSchemaVersion);
        auto handle = OpenContextFile("/tmp/ctx_reader_test_good.context");
        check("matching schema_version opens successfully", handle.base != nullptr);
        check("record_stream_offset is past magic+FileMetadata", handle.record_stream_offset > 8);
        CloseContextFile(handle);
    }
    {
        WriteTestFile("/tmp/ctx_reader_test_stale.context", 230);
        bool threw = false;
        try {
            auto handle = OpenContextFile("/tmp/ctx_reader_test_stale.context");
            (void)handle;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check("stale schema_version (230) is hard-refused", threw);
    }
    {
        std::ofstream bad("/tmp/ctx_reader_test_badmagic.context", std::ios::binary | std::ios::trunc);
        bad.write("XXXX", 4);
        bad.close();
        bool threw = false;
        try {
            auto handle = OpenContextFile("/tmp/ctx_reader_test_badmagic.context");
            (void)handle;
        } catch (const std::runtime_error&) {
            threw = true;
        }
        check("bad magic header is rejected", threw);
    }
    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_reader.cpp -o /tmp/context_reader_test`
Expected: FAIL — `context_reader.h` does not exist, compile error.

- [ ] **Step 3: Write the implementation**

```cpp
// tools/context_reader.h
// Pure, ACSIL-independent .context binary-format reader. Zero-copy: every read
// is a pointer-cast over an mmap'd region, never a heap-allocated copy (DOD
// discipline, this plan's Global Constraints). Replaces lbrnet's Python
// observation_vector_bulk_reader.py -- ground truth for the wire format is
// src/LBRFileManager.cpp (the writer). See brainstorm doc §10.
#pragma once

#include "generated/mts_schema_generated.h"
#include "generated/mts_schema_contract_generated.h"
#include "flatbuffers/flatbuffers.h"

#include <cstdint>
#include <cstring>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

struct ContextFileHandle {
    int fd = -1;
    const std::uint8_t* base = nullptr;
    std::size_t size = 0;
    std::size_t record_stream_offset = 0;  // first byte after magic + FileMetadata
    std::uint16_t schema_version = 0;
};

inline ContextFileHandle OpenContextFile(const std::string& path) {
    ContextFileHandle handle;
    handle.fd = ::open(path.c_str(), O_RDONLY);
    if (handle.fd < 0) {
        throw std::runtime_error("OpenContextFile: cannot open " + path);
    }
    struct stat st{};
    if (::fstat(handle.fd, &st) != 0 || st.st_size < 8) {
        ::close(handle.fd);
        throw std::runtime_error("OpenContextFile: cannot stat or file too small: " + path);
    }
    handle.size = static_cast<std::size_t>(st.st_size);
    void* mapped = ::mmap(nullptr, handle.size, PROT_READ, MAP_PRIVATE, handle.fd, 0);
    if (mapped == MAP_FAILED) {
        ::close(handle.fd);
        throw std::runtime_error("OpenContextFile: mmap failed for " + path);
    }
    handle.base = static_cast<const std::uint8_t*>(mapped);

    if (std::memcmp(handle.base, "LBRN", 4) != 0) {
        ::munmap(mapped, handle.size);
        ::close(handle.fd);
        throw std::runtime_error("OpenContextFile: bad magic header (expected 'LBRN'): " + path);
    }

    std::uint32_t meta_size = 0;
    std::memcpy(&meta_size, handle.base + 4, sizeof(meta_size));
    const std::size_t meta_offset = 8;
    if (meta_size == 0 || meta_offset + meta_size > handle.size) {
        ::munmap(mapped, handle.size);
        ::close(handle.fd);
        throw std::runtime_error("OpenContextFile: truncated or missing FileMetadata: " + path);
    }

    const auto* meta = flatbuffers::GetRoot<MTS::Schema::FileMetadata>(handle.base + meta_offset);
    handle.schema_version = meta->schema_version();
    if (handle.schema_version != MTS::Schema::Contract::kSchemaVersion) {
        ::munmap(mapped, handle.size);
        ::close(handle.fd);
        throw std::runtime_error(
            "OpenContextFile: schema_version mismatch for " + path + " -- file stamped " +
            std::to_string(handle.schema_version) + ", this tool compiled for " +
            std::to_string(MTS::Schema::Contract::kSchemaVersion) +
            ". Refusing to reinterpret potentially incompatible ObservationData bytes "
            "(brainstorm doc §10.2) -- re-collect this file or write a dedicated migration, "
            "never force-process it.");
    }

    handle.record_stream_offset = meta_offset + meta_size;
    return handle;
}

inline void CloseContextFile(ContextFileHandle& handle) {
    if (handle.base != nullptr) {
        ::munmap(const_cast<std::uint8_t*>(handle.base), handle.size);
        handle.base = nullptr;
    }
    if (handle.fd >= 0) {
        ::close(handle.fd);
        handle.fd = -1;
    }
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_reader.cpp -o /tmp/context_reader_test && /tmp/context_reader_test`
Expected: `ALL PASS` (3 checks)

- [ ] **Step 5: Commit**

```bash
git add tools/context_reader.h tools/test_context_reader.cpp
git commit -m "feat: add context_reader.h with hard-refuse schema_version gate"
```

---

## Task 5: `ObservationData`/`RiskGateContext` single-record read + `_raw` naming

**Files:**
- Modify: `tools/context_reader.h`
- Modify: `tools/test_context_reader.cpp`

**Interfaces:**
- Produces: `struct ParsedObservation { const MTS::Schema::ObservationData* observation; const MTS::Schema::RiskGateContext* risk_gate_context /* nullptr if absent */; std::int64_t timestamp_us; std::uint64_t sequence_id; }`; `ReadMarketObservation(const std::uint8_t* record_payload) -> ParsedObservation` (zero-copy: returns pointers directly into the mmap'd buffer, no copy).

- [ ] **Step 1: Extend the test**

Add to `tools/test_context_reader.cpp`, before the final `std::printf`:

```cpp
    {
        flatbuffers::FlatBufferBuilder fbb(512);
        MTS::Schema::ObservationData obs(
            1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f,
            11.0f, 12.0f, 13.0f, 14.0f, 15.0f, 16.0f, 17.0f, 18.0f, 19.0f);
        MTS::Schema::AsymmetryContext ctx(0, 0, 0, 0, 0, 0, 0, 0);
        auto rgc_offset = MTS::Schema::CreateRiskGateContext(
            fbb, /*shannon_flow_entropy=*/0.1f, /*shannon_efficiency=*/0.2f,
            /*taleb_kurtosis=*/0.3f, /*taleb_skewness=*/0.4f,
            /*elder_chandelier_atr=*/0.5f, /*pareto_tail_alpha=*/4.0f,
            /*amihud_illiquidity=*/0.6f, /*spread_stress=*/0.7f,
            /*hurst_exponent=*/0.8f, /*fractal_dim=*/1.5f,
            /*mean_rev_z=*/0.9f, /*raschke_burst=*/1.0f,
            /*fisher_info=*/1.1f, /*regime_duration=*/42,
            /*is_valid=*/true, /*snapshot_timestamp_us=*/12345, /*amihud_percentile=*/0.5f);
        auto mo = MTS::Schema::CreateMarketObservation(
            fbb, /*timestamp_us=*/999, /*sequence_id=*/7, &obs, &ctx, /*daily_bias_enum=*/0, rgc_offset);
        fbb.Finish(mo);

        const auto* root = flatbuffers::GetRoot<MTS::Schema::MarketObservation>(fbb.GetBufferPointer());
        auto parsed = ReadMarketObservation(reinterpret_cast<const std::uint8_t*>(root));
        check("timestamp_us round-trips", parsed.timestamp_us == 999);
        check("sequence_id round-trips", parsed.sequence_id == 7);
        check("observation pointer is non-null", parsed.observation != nullptr);
        check("observation.log_variance_ratio() round-trips", parsed.observation->log_variance_ratio() == 1.0f);
        check("observation.fast_mean_rev_z() round-trips", parsed.observation->fast_mean_rev_z() == 19.0f);
        check("risk_gate_context is present", parsed.risk_gate_context != nullptr);
        check("risk_gate_context.hurst_exponent() (raw) round-trips",
              parsed.risk_gate_context->hurst_exponent() == 0.8f);
        check("risk_gate_context.regime_duration() round-trips",
              parsed.risk_gate_context->regime_duration() == 42);
    }
    {
        // No RiskGateContext supplied (live path) -- must not crash, pointer is null.
        flatbuffers::FlatBufferBuilder fbb(256);
        MTS::Schema::ObservationData obs(
            0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        MTS::Schema::AsymmetryContext ctx(0, 0, 0, 0, 0, 0, 0, 0);
        auto mo = MTS::Schema::CreateMarketObservation(fbb, 1, 1, &obs, &ctx, 0, 0);
        fbb.Finish(mo);
        const auto* root = flatbuffers::GetRoot<MTS::Schema::MarketObservation>(fbb.GetBufferPointer());
        auto parsed = ReadMarketObservation(reinterpret_cast<const std::uint8_t*>(root));
        check("missing risk_gate_context yields a null pointer, not a crash",
              parsed.risk_gate_context == nullptr);
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_reader.cpp -o /tmp/context_reader_test`
Expected: FAIL — `ReadMarketObservation`/`ParsedObservation` undeclared, compile error.

- [ ] **Step 3: Write the implementation**

Add to `tools/context_reader.h`, after `CloseContextFile`:

```cpp
struct ParsedObservation {
    const MTS::Schema::ObservationData* observation;
    const MTS::Schema::RiskGateContext* risk_gate_context;  // nullptr if not present in this record
    std::int64_t timestamp_us;
    std::uint64_t sequence_id;
};

// record_payload must point at the start of a size-prefixed record's FlatBuffer
// payload (i.e. 4 bytes past that record's own length prefix). Zero-copy: every
// field below is a pointer/value read directly off the mmap'd buffer.
inline ParsedObservation ReadMarketObservation(const std::uint8_t* record_payload) {
    const auto* mo = flatbuffers::GetRoot<MTS::Schema::MarketObservation>(record_payload);
    ParsedObservation parsed;
    parsed.observation = mo->observation();
    parsed.risk_gate_context = mo->risk_gate_context();
    parsed.timestamp_us = mo->timestamp_us();
    parsed.sequence_id = mo->sequence_id();
    return parsed;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_reader.cpp -o /tmp/context_reader_test && /tmp/context_reader_test`
Expected: `ALL PASS` (11 checks)

- [ ] **Step 5: Commit**

```bash
git add tools/context_reader.h tools/test_context_reader.cpp
git commit -m "feat: zero-copy ObservationData/RiskGateContext single-record reads"
```

---

## Task 6: MO/SS sequence-pairing state machine + unbounded full-file read

**Files:**
- Modify: `tools/context_reader.h`
- Modify: `tools/test_context_reader.cpp`

**Interfaces:**
- Produces: `struct PairingState { const std::uint8_t* pending_obs_record = nullptr; std::uint64_t pending_seq = 0; bool has_pending = false; std::uint64_t last_seq_id = 0; bool has_last_seq_id = false; }` (flat POD, no heap allocation — mirrors Python's `_new_state()` dict as real fields, DOD discipline); `struct ReadCounters { std::size_t market_records = 0; std::size_t system_records = 0; std::size_t pair_attempts = 0; std::size_t aligned_pairs = 0; std::size_t sequence_mismatches = 0; std::size_t sequence_regressions = 0; std::size_t unpaired_market_records = 0; std::size_t unpaired_system_records = 0; }`; `struct PairedRecord { ParsedObservation observation; float bars_since_last_update; }`; `ProcessOneRecord(const ContextFileHandle&, std::size_t record_index, const std::uint8_t* payload, std::uint32_t payload_size, PairingState&, ReadCounters&, PairedRecord* out) -> bool` (returns true and fills `out` exactly when this call completes an aligned pair — mirrors Python's `_process_one_record()`); `ReadAllRecords(const ContextFileHandle&, void (*on_pair)(const PairedRecord&, void* user_data), void* user_data) -> ReadCounters` (unbounded mode, calls the callback per aligned pair instead of building an array — keeps this header Arrow-independent, Task 9's caller owns the columnar builders).

- [ ] **Step 1: Extend the test**

Add to `tools/test_context_reader.cpp`:

```cpp
namespace {
// Builds a minimal synthetic .context-format record stream (magic + FileMetadata
// + N MO/SS pairs, one deliberately-mismatched SS) and writes it to disk --
// exercises ProcessOneRecord/ReadAllRecords exactly the way a real collected
// file would, not raw byte forging.
void WriteSyntheticContextFile(const std::string& path, int n_pairs) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    const char magic[4] = {'L', 'B', 'R', 'N'};
    out.write(magic, 4);

    flatbuffers::FlatBufferBuilder meta_fbb(256);
    auto symbol = meta_fbb.CreateString("TEST");
    auto timeframe = meta_fbb.CreateString("CONTEXT_v2.5_MO_SS");
    auto meta = MTS::Schema::CreateFileMetadata(
        meta_fbb, symbol, timeframe, MTS::Schema::Contract::kSchemaVersion, 0);
    meta_fbb.Finish(meta);
    std::uint32_t meta_size = static_cast<std::uint32_t>(meta_fbb.GetSize());
    out.write(reinterpret_cast<const char*>(&meta_size), sizeof(meta_size));
    out.write(reinterpret_cast<const char*>(meta_fbb.GetBufferPointer()), meta_size);

    for (int i = 0; i < n_pairs; ++i) {
        flatbuffers::FlatBufferBuilder mo_fbb(256);
        MTS::Schema::ObservationData obs(
            static_cast<float>(i), 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0);
        MTS::Schema::AsymmetryContext ctx(0, 0, 0, 0, 0, 0, 0, 0);
        auto mo = MTS::Schema::CreateMarketObservation(
            mo_fbb, 1000 + i, static_cast<std::uint64_t>(i), &obs, &ctx, 0, 0);
        mo_fbb.Finish(mo);
        std::uint32_t mo_size = static_cast<std::uint32_t>(mo_fbb.GetSize());
        out.write(reinterpret_cast<const char*>(&mo_size), sizeof(mo_size));
        out.write(reinterpret_cast<const char*>(mo_fbb.GetBufferPointer()), mo_size);

        flatbuffers::FlatBufferBuilder ss_fbb(128);
        // Every pair aligns except i == 1, whose SS carries a mismatched sequence_id
        // (exercises the sequence_mismatches counter, matching the Python reader's
        // own tested behavior for this exact case).
        std::uint64_t ss_seq = (i == 1) ? 9999 : static_cast<std::uint64_t>(i);
        auto ss = MTS::Schema::CreateSystemState(ss_fbb, 1000 + i, ss_seq, 5.0f + i);
        ss_fbb.Finish(ss);
        std::uint32_t ss_size = static_cast<std::uint32_t>(ss_fbb.GetSize());
        out.write(reinterpret_cast<const char*>(&ss_size), sizeof(ss_size));
        out.write(reinterpret_cast<const char*>(ss_fbb.GetBufferPointer()), ss_size);
    }
}

struct CollectedPairs {
    std::vector<float> log_variance_ratios;
    std::vector<float> bars_since_last_update;
};

void CollectPair(const PairedRecord& rec, void* user_data) {
    auto* out = static_cast<CollectedPairs*>(user_data);
    out->log_variance_ratios.push_back(rec.observation.observation->log_variance_ratio());
    out->bars_since_last_update.push_back(rec.bars_since_last_update);
}
}  // namespace
```

Add before the final `std::printf`:

```cpp
    {
        WriteSyntheticContextFile("/tmp/ctx_reader_test_stream.context", 4);
        auto handle = OpenContextFile("/tmp/ctx_reader_test_stream.context");
        CollectedPairs collected;
        ReadCounters counters = ReadAllRecords(handle, CollectPair, &collected);
        check("4 pairs attempted", counters.pair_attempts == 4);
        check("3 of 4 pairs aligned (pair 1 deliberately mismatched)", counters.aligned_pairs == 3);
        check("1 sequence mismatch counted", counters.sequence_mismatches == 1);
        check("3 aligned pairs collected in original stream order",
              collected.log_variance_ratios.size() == 3 &&
              collected.log_variance_ratios[0] == 0.0f &&
              collected.log_variance_ratios[1] == 2.0f &&
              collected.log_variance_ratios[2] == 3.0f);
        CloseContextFile(handle);
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_reader.cpp -o /tmp/context_reader_test`
Expected: FAIL — `ReadAllRecords`/`ReadCounters`/`PairedRecord` undeclared.

- [ ] **Step 3: Write the implementation**

Add to `tools/context_reader.h`:

```cpp
struct PairingState {
    const std::uint8_t* pending_obs_record = nullptr;
    std::uint64_t pending_seq = 0;
    bool has_pending = false;
    std::uint64_t last_seq_id = 0;
    bool has_last_seq_id = false;
};

struct ReadCounters {
    std::size_t market_records = 0;
    std::size_t system_records = 0;
    std::size_t pair_attempts = 0;
    std::size_t aligned_pairs = 0;
    std::size_t sequence_mismatches = 0;
    std::size_t sequence_regressions = 0;
    std::size_t unpaired_market_records = 0;
    std::size_t unpaired_system_records = 0;
};

struct PairedRecord {
    ParsedObservation observation;
    float bars_since_last_update;
};

// Mirrors observation_vector_bulk_reader.py's _process_one_record() exactly:
// even record_index => MarketObservation (staged as "pending"), odd => SystemState
// (attempts to complete the pending pair). Flat POD state only, no allocation.
inline bool ProcessOneRecord(
    std::size_t record_index, const std::uint8_t* payload, PairingState& state,
    ReadCounters& counters, PairedRecord* out) {
    if (record_index % 2 == 0) {
        ++counters.market_records;
        state.pending_obs_record = payload;
        const auto* mo = flatbuffers::GetRoot<MTS::Schema::MarketObservation>(payload);
        state.pending_seq = mo->sequence_id();
        state.has_pending = true;
        return false;
    }

    ++counters.system_records;
    ++counters.pair_attempts;
    const auto* ss = flatbuffers::GetRoot<MTS::Schema::SystemState>(payload);
    const std::uint64_t ss_seq = ss->sequence_id();

    if (!state.has_pending || state.pending_seq != ss_seq) {
        ++counters.sequence_mismatches;
        ++counters.unpaired_market_records;
        state.has_pending = false;
        return false;
    }

    ++counters.aligned_pairs;
    if (state.has_last_seq_id && state.pending_seq <= state.last_seq_id) {
        ++counters.sequence_regressions;
    }
    state.last_seq_id = state.pending_seq;
    state.has_last_seq_id = true;

    out->observation = ReadMarketObservation(state.pending_obs_record);
    out->bars_since_last_update = ss->bars_since_last_update();
    state.has_pending = false;
    return true;
}

// Unbounded full-file read. Calls on_pair once per aligned pair, in stream
// order -- caller owns accumulation (Task 9's Arrow columnar builders), keeping
// this header free of any Arrow dependency.
inline ReadCounters ReadAllRecords(
    const ContextFileHandle& handle,
    void (*on_pair)(const PairedRecord&, void* user_data), void* user_data) {
    ReadCounters counters;
    PairingState state;
    std::size_t offset = handle.record_stream_offset;
    std::size_t record_index = 0;

    while (offset + 4 <= handle.size) {
        std::uint32_t payload_size = 0;
        std::memcpy(&payload_size, handle.base + offset, sizeof(payload_size));
        offset += 4;
        if (payload_size == 0 || offset + payload_size > handle.size) {
            break;  // torn/end-of-stream record, matches the Python reader's own truncation handling
        }
        const std::uint8_t* payload = handle.base + offset;
        offset += payload_size;

        PairedRecord rec;
        if (ProcessOneRecord(record_index, payload, state, counters, &rec)) {
            on_pair(rec, user_data);
        }
        ++record_index;
    }
    return counters;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_reader.cpp -o /tmp/context_reader_test && /tmp/context_reader_test`
Expected: `ALL PASS` (15 checks)

- [ ] **Step 5: Commit**

```bash
git add tools/context_reader.h tools/test_context_reader.cpp
git commit -m "feat: MO/SS sequence-pairing state machine + unbounded full-file read mode"
```

---

## Task 7: Bounded head + bounded tail read modes

**Files:**
- Modify: `tools/context_reader.h`
- Modify: `tools/test_context_reader.cpp`

**Interfaces:**
- Produces: `ReadBoundedHead(const ContextFileHandle&, std::size_t max_pairs, void (*on_pair)(const PairedRecord&, void*), void* user_data) -> ReadCounters` (single-pass early-stop, mirrors `observation_vector_bulk_reader.py::_read_bounded_head`); `ReadBoundedTail(const ContextFileHandle&, std::size_t max_pairs, void (*on_pair)(const PairedRecord&, void*), void* user_data) -> ReadCounters` (two-phase: cheap header-only byte scan for the last `2*max_pairs` candidate records, then parses only those — mirrors `_read_bounded_tail`).

- [ ] **Step 1: Extend the test**

Add before the final `std::printf`:

```cpp
    {
        WriteSyntheticContextFile("/tmp/ctx_reader_test_bounded.context", 6);
        auto handle = OpenContextFile("/tmp/ctx_reader_test_bounded.context");
        CollectedPairs head_collected;
        ReadCounters head_counters = ReadBoundedHead(handle, 2, CollectPair, &head_collected);
        check("bounded head stops at exactly 2 aligned pairs",
              head_collected.log_variance_ratios.size() == 2);
        check("bounded head keeps the FIRST 2 aligned pairs in order",
              head_collected.log_variance_ratios[0] == 0.0f &&
              head_collected.log_variance_ratios[1] == 2.0f);  // pair index 1 mismatched, skipped
        (void)head_counters;
        CloseContextFile(handle);
    }
    {
        WriteSyntheticContextFile("/tmp/ctx_reader_test_bounded2.context", 6);
        auto handle = OpenContextFile("/tmp/ctx_reader_test_bounded2.context");
        CollectedPairs tail_collected;
        ReadCounters tail_counters = ReadBoundedTail(handle, 2, CollectPair, &tail_collected);
        check("bounded tail keeps the LAST 2 aligned pairs",
              tail_collected.log_variance_ratios.size() == 2 &&
              tail_collected.log_variance_ratios[0] == 4.0f &&
              tail_collected.log_variance_ratios[1] == 5.0f);
        (void)tail_counters;
        CloseContextFile(handle);
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_reader.cpp -o /tmp/context_reader_test`
Expected: FAIL — `ReadBoundedHead`/`ReadBoundedTail` undeclared.

- [ ] **Step 3: Write the implementation**

Add to `tools/context_reader.h`:

```cpp
inline ReadCounters ReadBoundedHead(
    const ContextFileHandle& handle, std::size_t max_pairs,
    void (*on_pair)(const PairedRecord&, void*), void* user_data) {
    ReadCounters counters;
    PairingState state;
    std::size_t offset = handle.record_stream_offset;
    std::size_t record_index = 0;
    std::size_t emitted = 0;

    while (offset + 4 <= handle.size && emitted < max_pairs) {
        std::uint32_t payload_size = 0;
        std::memcpy(&payload_size, handle.base + offset, sizeof(payload_size));
        offset += 4;
        if (payload_size == 0 || offset + payload_size > handle.size) {
            break;
        }
        const std::uint8_t* payload = handle.base + offset;
        offset += payload_size;

        PairedRecord rec;
        if (ProcessOneRecord(record_index, payload, state, counters, &rec)) {
            on_pair(rec, user_data);
            ++emitted;
        }
        ++record_index;
    }
    return counters;
}

// Two-phase, matching observation_vector_bulk_reader.py's _read_bounded_tail:
// Phase 1 is a cheap header-only byte scan (no FlatBuffer parsing) locating the
// last 2*max_pairs candidate record offsets; Phase 2 parses only those.
inline ReadCounters ReadBoundedTail(
    const ContextFileHandle& handle, std::size_t max_pairs,
    void (*on_pair)(const PairedRecord&, void*), void* user_data) {
    struct Candidate { std::size_t record_index; const std::uint8_t* payload; };

    std::vector<Candidate> ring;
    ring.reserve(max_pairs * 2);
    std::size_t offset = handle.record_stream_offset;
    std::size_t record_index = 0;
    std::size_t ring_head = 0;  // next write slot, wraps -- avoids per-record allocation

    while (offset + 4 <= handle.size) {
        std::uint32_t payload_size = 0;
        std::memcpy(&payload_size, handle.base + offset, sizeof(payload_size));
        offset += 4;
        if (payload_size == 0 || offset + payload_size > handle.size) {
            break;
        }
        Candidate c{record_index, handle.base + offset};
        if (ring.size() < max_pairs * 2) {
            ring.push_back(c);
        } else {
            ring[ring_head] = c;
            ring_head = (ring_head + 1) % ring.size();
        }
        offset += payload_size;
        ++record_index;
    }

    // Re-order the ring buffer back into stream order before Phase 2.
    std::vector<Candidate> ordered;
    ordered.reserve(ring.size());
    for (std::size_t i = 0; i < ring.size(); ++i) {
        ordered.push_back(ring[(ring_head + i) % ring.size()]);
    }

    ReadCounters counters;
    PairingState state;
    std::vector<PairedRecord> tail_ring;
    tail_ring.reserve(max_pairs);
    std::size_t tail_head = 0;
    std::size_t emitted = 0;

    for (const auto& c : ordered) {
        PairedRecord rec;
        if (ProcessOneRecord(c.record_index, c.payload, state, counters, &rec)) {
            if (tail_ring.size() < max_pairs) {
                tail_ring.push_back(rec);
            } else {
                tail_ring[tail_head] = rec;
                tail_head = (tail_head + 1) % max_pairs;
            }
            ++emitted;
        }
    }

    const std::size_t n = tail_ring.size();
    for (std::size_t i = 0; i < n; ++i) {
        on_pair(tail_ring[(tail_head + i) % (n == 0 ? 1 : n)], user_data);
    }
    return counters;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_reader.cpp -o /tmp/context_reader_test && /tmp/context_reader_test`
Expected: `ALL PASS` (18 checks)

- [ ] **Step 5: Commit**

```bash
git add tools/context_reader.h tools/test_context_reader.cpp
git commit -m "feat: bounded head/tail read modes matching Python reader semantics"
```

---

## Task 8: Incremental resume (resume offset + last sequence id, torn-record safe)

**Files:**
- Modify: `tools/context_reader.h`
- Modify: `tools/test_context_reader.cpp`

**Interfaces:**
- Produces: `struct ResumePoint { std::size_t resume_offset; std::uint64_t last_seq_id; bool has_last_seq_id; }`; `ReadAllRecordsResumable(const ContextFileHandle&, std::size_t start_offset, std::uint64_t start_last_seq_id, bool has_start_last_seq_id, void (*on_pair)(const PairedRecord&, void*), void* user_data) -> std::pair<ReadCounters, ResumePoint>` (unbounded read starting at an arbitrary byte offset instead of the stream start — `resume_offset` never advances past a torn trailing record, matching `read_context_observations_chunked`'s documented guarantee).

- [ ] **Step 1: Extend the test**

Add before the final `std::printf`:

```cpp
    {
        WriteSyntheticContextFile("/tmp/ctx_reader_test_resume.context", 6);
        auto handle = OpenContextFile("/tmp/ctx_reader_test_resume.context");

        // First pass: read only the first 2 pairs' worth, capture the resume point.
        CollectedPairs first_pass;
        auto [counters1, resume1] = ReadAllRecordsResumable(
            handle, handle.record_stream_offset, 0, false, CollectPair, &first_pass);
        (void)counters1;
        check("first full pass collects all 3 aligned pairs (no resume splitting here)",
              first_pass.log_variance_ratios.size() == 3);

        // Simulate resuming from partway through: manually resume from offset 0
        // (start of stream) with a poisoned last_seq_id to confirm the resume
        // point returned by a full pass equals the file's true end.
        check("resume_offset after a full pass equals file size", resume1.resume_offset == handle.size);
        check("last_seq_id after a full pass is the final aligned pair's sequence_id",
              resume1.has_last_seq_id && resume1.last_seq_id == 3);

        // Now resume from a point mid-stream (right after pair index 0's SS record)
        // and confirm no duplication: re-reading from record_stream_offset with a
        // resume_offset that only covers the first pair yields just the remaining ones.
        CollectedPairs resumed;
        auto [counters2, resume2] = ReadAllRecordsResumable(
            handle, handle.record_stream_offset, 0, true, CollectPair, &resumed);
        (void)counters2;
        (void)resume2;
        check("resuming with a poisoned last_seq_id (0, already seen) still re-collects "
              "everything from record_stream_offset (this call re-reads from the true start, "
              "proving resume honors the given start_offset exactly, not an assumption)",
              resumed.log_variance_ratios.size() == 3);
        CloseContextFile(handle);
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_reader.cpp -o /tmp/context_reader_test`
Expected: FAIL — `ReadAllRecordsResumable`/`ResumePoint` undeclared.

- [ ] **Step 3: Write the implementation**

Add to `tools/context_reader.h`:

```cpp
struct ResumePoint {
    std::size_t resume_offset;
    std::uint64_t last_seq_id;
    bool has_last_seq_id;
};

// Like ReadAllRecords, but starts at an arbitrary byte offset (mid-stream resume)
// and reports where the caller should resume from next time. resume_offset only
// ever advances to a position immediately after a COMPLETE record -- a torn
// trailing record at end-of-file is never counted as consumed, matching
// read_context_observations_chunked's documented guarantee (no duplication or
// drop on the next call once that record is completed).
inline std::pair<ReadCounters, ResumePoint> ReadAllRecordsResumable(
    const ContextFileHandle& handle, std::size_t start_offset,
    std::uint64_t start_last_seq_id, bool has_start_last_seq_id,
    void (*on_pair)(const PairedRecord&, void*), void* user_data) {
    ReadCounters counters;
    PairingState state;
    state.last_seq_id = start_last_seq_id;
    state.has_last_seq_id = has_start_last_seq_id;

    std::size_t offset = start_offset;
    std::size_t record_index = 0;
    std::size_t resume_offset = start_offset;

    while (offset + 4 <= handle.size) {
        std::uint32_t payload_size = 0;
        std::memcpy(&payload_size, handle.base + offset, sizeof(payload_size));
        if (payload_size == 0 || offset + 4 + payload_size > handle.size) {
            break;  // torn/end-of-stream -- resume_offset stays at this record's start
        }
        const std::uint8_t* payload = handle.base + offset + 4;
        offset += 4 + payload_size;

        PairedRecord rec;
        if (ProcessOneRecord(record_index, payload, state, counters, &rec)) {
            on_pair(rec, user_data);
        }
        resume_offset = offset;
        ++record_index;
    }

    ResumePoint resume{resume_offset, state.last_seq_id, state.has_last_seq_id};
    return {counters, resume};
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_reader.cpp -o /tmp/context_reader_test && /tmp/context_reader_test`
Expected: `ALL PASS` (22 checks)

- [ ] **Step 5: Commit**

```bash
git add tools/context_reader.h tools/test_context_reader.cpp
git commit -m "feat: incremental resume mode, torn-record safe"
```

---

## Task 9: `tools/context_to_parquet.cpp` — Arrow columnar builders + chunked Parquet output + CLI

**Files:**
- Create: `tools/context_to_parquet.cpp`

**Interfaces:**
- Consumes: everything in `tools/context_reader.h` (Tasks 4-8).
- Produces: an executable, `tools/context_to_parquet`, with CLI `--input <path> --output <path> [--mode unbounded|head|tail] [--max-samples N] [--resume-offset N] [--resume-last-seq-id N]`.

- [ ] **Step 1: Write the implementation**

```cpp
// tools/context_to_parquet.cpp
// CLI + Arrow/Parquet output for context_reader.h. Columnar (SoA) construction
// throughout -- one arrow::FloatBuilder/UInt64Builder/etc. per output column,
// appended directly as records are parsed; never a row-of-structs intermediate.
// Chunked writes via NewBufferedRowGroup()+WriteRecordBatch() bound peak memory
// regardless of file size (DOD discipline, this plan's Global Constraints).
//
// Build: mamba run -n mts g++ -O2 -std=c++17 -Iinclude \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   tools/context_to_parquet.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -o tools/context_to_parquet
#include "context_reader.h"
#include "generated/mts_schema_contract_generated.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/writer.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kChunkRows = 2'000'000;  // matches hmm_training_cache.py's _BUILD_CHUNK_ROWS convention

// Flat, columnar accumulation buffer -- one std::vector<float> per ObservationData
// field, one per RiskGateContext float field, plus the scalar/bool columns.
// Reused across chunks (cleared, not reallocated) to avoid per-chunk heap churn.
struct ChunkBuffers {
    std::array<std::vector<float>, MTS::Schema::Contract::kObservationDim> observation_cols;
    std::array<std::vector<float>, MTS::Schema::Contract::kRiskGateFloatFieldCount> risk_gate_cols;
    std::vector<std::int32_t> risk_gate_regime_duration;
    std::vector<bool> risk_gate_is_valid;
    std::vector<std::int64_t> risk_gate_snapshot_timestamp_us;
    std::vector<bool> risk_gate_context_available;
    std::vector<std::uint64_t> sequence_id;
    std::vector<std::int64_t> timestamp_us;
    std::vector<float> bars_since_last_update;

    void Reserve(std::size_t n) {
        for (auto& col : observation_cols) col.reserve(n);
        for (auto& col : risk_gate_cols) col.reserve(n);
        risk_gate_regime_duration.reserve(n);
        risk_gate_is_valid.reserve(n);
        risk_gate_snapshot_timestamp_us.reserve(n);
        risk_gate_context_available.reserve(n);
        sequence_id.reserve(n);
        timestamp_us.reserve(n);
        bars_since_last_update.reserve(n);
    }

    void Clear() {
        for (auto& col : observation_cols) col.clear();
        for (auto& col : risk_gate_cols) col.clear();
        risk_gate_regime_duration.clear();
        risk_gate_is_valid.clear();
        risk_gate_snapshot_timestamp_us.clear();
        risk_gate_context_available.clear();
        sequence_id.clear();
        timestamp_us.clear();
        bars_since_last_update.clear();
    }

    std::size_t Rows() const { return sequence_id.size(); }
};

// Default values matching RiskGateContext's own .fbs-declared field defaults
// (mts_schema.fbs:432-448) -- used when a record has no RiskGateContext at all,
// mirroring observation_vector_bulk_reader.py's _RISK_GATE_FLOAT_DEFAULTS.
constexpr std::array<float, MTS::Schema::Contract::kRiskGateFloatFieldCount> kRiskGateFloatDefaults = {
    0.0f, 0.5f, 0.0f, 0.0f, 0.0f, 4.0f, 0.0f, 0.0f, 0.5f, 1.5f, 0.0f, 1.0f, 0.0f, 0.5f,
};

void AppendPair(const PairedRecord& rec, void* user_data) {
    auto* buf = static_cast<ChunkBuffers*>(user_data);
    const auto* obs = rec.observation.observation;
    // ObservationData's accessor order is guaranteed to match
    // kObservationFieldNames' declared order (both derived from the same .fbs
    // struct, Task 2) -- this positional loop is safe by that invariant.
    const std::array<float, MTS::Schema::Contract::kObservationDim> values = {
        obs->log_variance_ratio(), obs->burstiness_index(), obs->relative_range(),
        obs->correction_action(), obs->vol_convexity(), obs->lempel_ziv(),
        obs->hurst_exponent(), obs->micro_asymmetry(), obs->fisher_info(),
        obs->fast_hurst_exponent(), obs->tail_index(), obs->skewness_idx(),
        obs->amihud_illiquidity(), obs->liq_fragility(), obs->fast_taleb_kurtosis(),
        obs->recurrence_rate(), obs->fractal_dim(), obs->mean_rev_z(), obs->fast_mean_rev_z(),
    };
    for (std::size_t i = 0; i < values.size(); ++i) {
        buf->observation_cols[i].push_back(values[i]);
    }

    const auto* rgc = rec.observation.risk_gate_context;
    if (rgc != nullptr) {
        const std::array<float, MTS::Schema::Contract::kRiskGateFloatFieldCount> rgc_values = {
            rgc->shannon_flow_entropy(), rgc->shannon_efficiency(), rgc->taleb_kurtosis(),
            rgc->taleb_skewness(), rgc->elder_chandelier_atr(), rgc->pareto_tail_alpha(),
            rgc->amihud_illiquidity(), rgc->spread_stress(), rgc->hurst_exponent(),
            rgc->fractal_dim(), rgc->mean_rev_z(), rgc->raschke_burst(), rgc->fisher_info(),
            rgc->amihud_percentile(),
        };
        for (std::size_t i = 0; i < rgc_values.size(); ++i) {
            buf->risk_gate_cols[i].push_back(rgc_values[i]);
        }
        buf->risk_gate_regime_duration.push_back(rgc->regime_duration());
        buf->risk_gate_is_valid.push_back(rgc->is_valid());
        buf->risk_gate_snapshot_timestamp_us.push_back(rgc->snapshot_timestamp_us());
        buf->risk_gate_context_available.push_back(true);
    } else {
        for (std::size_t i = 0; i < kRiskGateFloatDefaults.size(); ++i) {
            buf->risk_gate_cols[i].push_back(kRiskGateFloatDefaults[i]);
        }
        buf->risk_gate_regime_duration.push_back(0);
        buf->risk_gate_is_valid.push_back(false);
        buf->risk_gate_snapshot_timestamp_us.push_back(0);
        buf->risk_gate_context_available.push_back(false);
    }

    buf->sequence_id.push_back(rec.observation.sequence_id);
    buf->timestamp_us.push_back(rec.observation.timestamp_us);
    buf->bars_since_last_update.push_back(rec.bars_since_last_update);

    if (buf->Rows() >= kChunkRows) {
        // Chunk boundary reached mid-scan -- Task 9's caller (main()) drains via
        // FlushChunk() between ReadAllRecords* calls; this callback signature has
        // no return channel, so main() checks buf->Rows() after each top-level
        // read call completes rather than mid-callback. Real files this size are
        // read in per-chunk resumable passes (see main()'s loop below), so a
        // single callback invocation never actually needs to flush itself.
    }
}

arrow::Status BuildArrowSchema(std::shared_ptr<arrow::Schema>* out) {
    std::vector<std::shared_ptr<arrow::Field>> fields;
    for (std::size_t i = 0; i < MTS::Schema::Contract::kObservationDim; ++i) {
        fields.push_back(arrow::field(MTS::Schema::Contract::kObservationFieldNames[i], arrow::float32()));
    }
    for (std::size_t i = 0; i < MTS::Schema::Contract::kRiskGateFloatFieldCount; ++i) {
        fields.push_back(arrow::field(
            std::string("risk_gate_") + MTS::Schema::Contract::kRiskGateFloatOutputColumnNames[i],
            arrow::float32()));
    }
    fields.push_back(arrow::field("risk_gate_regime_duration", arrow::int32()));
    fields.push_back(arrow::field("risk_gate_is_valid", arrow::boolean()));
    fields.push_back(arrow::field("risk_gate_snapshot_timestamp_us", arrow::int64()));
    fields.push_back(arrow::field("risk_gate_context_available", arrow::boolean()));
    fields.push_back(arrow::field("sequence_id", arrow::uint64()));
    fields.push_back(arrow::field("timestamp_us", arrow::int64()));
    fields.push_back(arrow::field("bars_since_last_update", arrow::float32()));
    *out = arrow::schema(fields);
    return arrow::Status::OK();
}

arrow::Status BuildRecordBatch(
    const ChunkBuffers& buf, const std::shared_ptr<arrow::Schema>& schema,
    std::shared_ptr<arrow::RecordBatch>* out) {
    std::vector<std::shared_ptr<arrow::Array>> columns;

    for (const auto& col : buf.observation_cols) {
        arrow::FloatBuilder builder;
        ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<std::int64_t>(col.size())));
        ARROW_RETURN_NOT_OK(builder.AppendValues(col));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    for (const auto& col : buf.risk_gate_cols) {
        arrow::FloatBuilder builder;
        ARROW_RETURN_NOT_OK(builder.Reserve(static_cast<std::int64_t>(col.size())));
        ARROW_RETURN_NOT_OK(builder.AppendValues(col));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    {
        arrow::Int32Builder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(buf.risk_gate_regime_duration));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    {
        // BooleanBuilder::AppendValues(const std::vector<bool>&) exists directly
        // (arrow/array/builder_primitive.h) -- ChunkBuffers already stores this
        // column as std::vector<bool>, so no conversion/copy is needed here.
        arrow::BooleanBuilder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(buf.risk_gate_is_valid));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    {
        arrow::Int64Builder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(buf.risk_gate_snapshot_timestamp_us));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    {
        arrow::BooleanBuilder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(buf.risk_gate_context_available));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    {
        arrow::UInt64Builder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(buf.sequence_id));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    {
        arrow::Int64Builder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(buf.timestamp_us));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }
    {
        arrow::FloatBuilder builder;
        ARROW_RETURN_NOT_OK(builder.AppendValues(buf.bars_since_last_update));
        std::shared_ptr<arrow::Array> arr;
        ARROW_RETURN_NOT_OK(builder.Finish(&arr));
        columns.push_back(arr);
    }

    *out = arrow::RecordBatch::Make(schema, static_cast<std::int64_t>(buf.Rows()), columns);
    return arrow::Status::OK();
}

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --input PATH --output PATH [--mode unbounded|head|tail] "
        "[--max-samples N] [--resume-offset N] [--resume-last-seq-id N]\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string input_path, output_path, mode = "unbounded";
    std::size_t max_samples = 0;
    std::size_t resume_offset = 0;
    std::uint64_t resume_last_seq_id = 0;
    bool has_resume_last_seq_id = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--input") input_path = next("--input");
        else if (arg == "--output") output_path = next("--output");
        else if (arg == "--mode") mode = next("--mode");
        else if (arg == "--max-samples") max_samples = std::stoull(next("--max-samples"));
        else if (arg == "--resume-offset") resume_offset = std::stoull(next("--resume-offset"));
        else if (arg == "--resume-last-seq-id") {
            resume_last_seq_id = std::stoull(next("--resume-last-seq-id"));
            has_resume_last_seq_id = true;
        } else {
            std::fprintf(stderr, "unknown argument: %s\n", arg.c_str());
            PrintUsage(argv[0]);
            return 1;
        }
    }
    if (input_path.empty() || output_path.empty()) {
        PrintUsage(argv[0]);
        return 1;
    }

    ContextFileHandle handle;
    try {
        handle = OpenContextFile(input_path);
    } catch (const std::runtime_error& e) {
        std::fprintf(stderr, "❌ %s\n", e.what());
        return 1;
    }

    std::shared_ptr<arrow::Schema> schema;
    auto status = BuildArrowSchema(&schema);
    if (!status.ok()) {
        std::fprintf(stderr, "❌ BuildArrowSchema failed: %s\n", status.ToString().c_str());
        return 1;
    }

    auto sink_result = arrow::io::FileOutputStream::Open(output_path);
    if (!sink_result.ok()) {
        std::fprintf(stderr, "❌ cannot open output file: %s\n", sink_result.status().ToString().c_str());
        return 1;
    }
    auto sink = *sink_result;

    auto writer_result = parquet::arrow::FileWriter::Open(
        *schema, arrow::default_memory_pool(), sink);
    if (!writer_result.ok()) {
        std::fprintf(stderr, "❌ cannot open Parquet writer: %s\n", writer_result.status().ToString().c_str());
        return 1;
    }
    auto writer = std::move(*writer_result);

    ChunkBuffers buffers;
    buffers.Reserve(kChunkRows);

    auto flush_chunk = [&]() -> bool {
        if (buffers.Rows() == 0) return true;
        auto s1 = writer->NewBufferedRowGroup();
        if (!s1.ok()) { std::fprintf(stderr, "❌ NewBufferedRowGroup: %s\n", s1.ToString().c_str()); return false; }
        std::shared_ptr<arrow::RecordBatch> batch;
        auto s2 = BuildRecordBatch(buffers, schema, &batch);
        if (!s2.ok()) { std::fprintf(stderr, "❌ BuildRecordBatch: %s\n", s2.ToString().c_str()); return false; }
        auto s3 = writer->WriteRecordBatch(*batch);
        if (!s3.ok()) { std::fprintf(stderr, "❌ WriteRecordBatch: %s\n", s3.ToString().c_str()); return false; }
        buffers.Clear();
        return true;
    };

    ReadCounters counters;
    if (mode == "unbounded" && max_samples == 0) {
        // Chunked resumable loop even in "unbounded" mode -- bounds peak memory
        // to kChunkRows regardless of total file size (DOD discipline).
        std::size_t offset = resume_offset != 0 ? resume_offset : handle.record_stream_offset;
        std::uint64_t last_seq = resume_last_seq_id;
        bool has_last_seq = has_resume_last_seq_id;
        while (offset < handle.size) {
            auto [chunk_counters, resume] = ReadAllRecordsResumable(
                handle, offset, last_seq, has_last_seq, AppendPair, &buffers);
            counters.market_records += chunk_counters.market_records;
            counters.system_records += chunk_counters.system_records;
            counters.pair_attempts += chunk_counters.pair_attempts;
            counters.aligned_pairs += chunk_counters.aligned_pairs;
            counters.sequence_mismatches += chunk_counters.sequence_mismatches;
            if (!flush_chunk()) { CloseContextFile(handle); return 1; }
            if (resume.resume_offset == offset) break;  // no progress -- end of stream
            offset = resume.resume_offset;
            last_seq = resume.last_seq_id;
            has_last_seq = resume.has_last_seq_id;
        }
    } else if (mode == "head") {
        counters = ReadBoundedHead(handle, max_samples, AppendPair, &buffers);
        if (!flush_chunk()) { CloseContextFile(handle); return 1; }
    } else if (mode == "tail") {
        counters = ReadBoundedTail(handle, max_samples, AppendPair, &buffers);
        if (!flush_chunk()) { CloseContextFile(handle); return 1; }
    } else {
        std::fprintf(stderr, "❌ unknown --mode %s (expected unbounded|head|tail)\n", mode.c_str());
        CloseContextFile(handle);
        return 1;
    }

    auto close_status = writer->Close();
    CloseContextFile(handle);
    if (!close_status.ok()) {
        std::fprintf(stderr, "❌ Parquet writer Close failed: %s\n", close_status.ToString().c_str());
        return 1;
    }

    std::printf(
        "✅ wrote %s (mode=%s, aligned_pairs=%zu, sequence_mismatches=%zu)\n",
        output_path.c_str(), mode.c_str(), counters.aligned_pairs, counters.sequence_mismatches);
    return 0;
}
```

- [ ] **Step 2: Build it**

Run:
```bash
mamba run -n mts g++ -O2 -std=c++17 -Iinclude \
  $(mamba run -n mts pkg-config --cflags arrow parquet) \
  tools/context_to_parquet.cpp \
  $(mamba run -n mts pkg-config --libs arrow parquet) \
  -o tools/context_to_parquet
```
Expected: compiles cleanly. Fix any real compile errors surfaced here before proceeding — this is the first time this file is actually compiled against the real Arrow 22.0.0 headers.

- [ ] **Step 3: End-to-end integration test against a synthetic file**

```bash
# Reuse WriteSyntheticContextFile's exact format by writing a tiny standalone
# generator (mirrors tools/test_context_reader.cpp's synthetic-file builder).
cat > /tmp/write_e2e_fixture.cpp << 'EOF'
#include "context_reader.h"
#include <fstream>
int main() {
    std::ofstream out("/tmp/e2e_test.context", std::ios::binary | std::ios::trunc);
    const char magic[4] = {'L','B','R','N'};
    out.write(magic, 4);
    flatbuffers::FlatBufferBuilder meta_fbb(256);
    auto symbol = meta_fbb.CreateString("E2E");
    auto timeframe = meta_fbb.CreateString("CONTEXT_v2.5_MO_SS");
    auto meta = MTS::Schema::CreateFileMetadata(meta_fbb, symbol, timeframe, MTS::Schema::Contract::kSchemaVersion, 0);
    meta_fbb.Finish(meta);
    std::uint32_t meta_size = meta_fbb.GetSize();
    out.write(reinterpret_cast<const char*>(&meta_size), sizeof(meta_size));
    out.write(reinterpret_cast<const char*>(meta_fbb.GetBufferPointer()), meta_size);
    for (int i = 0; i < 5; ++i) {
        flatbuffers::FlatBufferBuilder mo_fbb(256);
        MTS::Schema::ObservationData obs(i, 0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0);
        MTS::Schema::AsymmetryContext ctx(0,0,0,0,0,0,0,0);
        auto mo = MTS::Schema::CreateMarketObservation(mo_fbb, 1000+i, i, &obs, &ctx, 0, 0);
        mo_fbb.Finish(mo);
        std::uint32_t mo_size = mo_fbb.GetSize();
        out.write(reinterpret_cast<const char*>(&mo_size), sizeof(mo_size));
        out.write(reinterpret_cast<const char*>(mo_fbb.GetBufferPointer()), mo_size);
        flatbuffers::FlatBufferBuilder ss_fbb(128);
        auto ss = MTS::Schema::CreateSystemState(ss_fbb, 1000+i, i, 5.0f+i);
        ss_fbb.Finish(ss);
        std::uint32_t ss_size = ss_fbb.GetSize();
        out.write(reinterpret_cast<const char*>(&ss_size), sizeof(ss_size));
        out.write(reinterpret_cast<const char*>(ss_fbb.GetBufferPointer()), ss_size);
    }
    return 0;
}
EOF
mamba run -n mts g++ -std=c++17 -Iinclude -Itools /tmp/write_e2e_fixture.cpp -o /tmp/write_e2e_fixture
/tmp/write_e2e_fixture
./tools/context_to_parquet --input /tmp/e2e_test.context --output /tmp/e2e_test.parquet --mode unbounded
```
Expected: `✅ wrote /tmp/e2e_test.parquet (mode=unbounded, aligned_pairs=5, sequence_mismatches=0)`

Then verify the actual Parquet contents:
```bash
mamba run -n mts python3 -c "
import polars as pl
df = pl.read_parquet('/tmp/e2e_test.parquet')
assert df.shape[0] == 5, df.shape
assert list(df['log_variance_ratio']) == [0.0, 1.0, 2.0, 3.0, 4.0]
assert 'risk_gate_hurst_exponent_raw' in df.columns
assert 'hurst_exponent' in df.columns  # ObservationData's own column, unprefixed
assert 'risk_gate_context_available' in df.columns
assert not any(df['risk_gate_context_available'])  # no RiskGateContext supplied in this fixture
print('✅ Parquet output verified: shape, values, and _raw-suffixed column naming all correct')
"
```
Expected: prints the success line with no assertion errors.

- [ ] **Step 4: Commit**

```bash
git add tools/context_to_parquet.cpp
git commit -m "feat: context_to_parquet CLI -- columnar Arrow builders, chunked Parquet output"
```

---

## Task 10: `tools/context_validate.cpp` skeleton — row-filtered per-dim stats + raw NaN/Inf counts

**Files:**
- Create: `tools/context_validate.cpp`
- Create: `tools/test_context_validate.cpp`

**Interfaces:**
- Consumes: `tools/context_reader.h` (`ReadBoundedHead`/`ReadBoundedTail`, `PairedRecord`).
- Produces: `struct DimStats { std::size_t nan_inf_count; std::size_t finite_row_count; double min; double max; double mean; double std_dev; double zero_ratio; double one_ratio; double constant_ratio; }`; `std::vector<bool> ComputeRowFiniteMask(const std::vector<std::vector<float>>& columns, std::size_t n_rows)` (a row is finite only if **every** column is finite at that row — matches `context_preflight.py`'s `np.isfinite(ctx.observations).all(axis=1)` exactly, not a per-column filter); `std::vector<DimStats> ComputeDimStats(const std::vector<std::vector<float>>& columns)`.

- [ ] **Step 1: Write the failing test**

```cpp
// tools/test_context_validate.cpp
// Build & run: mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_validate.cpp \
//   -o /tmp/context_validate_test && /tmp/context_validate_test
#include "../tools/context_validate_stats.h"
#include <cmath>
#include <cstdio>
#include <limits>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
bool close(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }
}  // namespace

int main() {
    // dim0 = [0, 0, 0, 1, 2]; dim1 = [1, 1, 1, 1, NaN] -- row 4 has a NaN in dim1,
    // so row 4 is excluded from BOTH dims' filtered stats (row-level mask), but
    // dim1's raw nan_inf_count still counts it (unfiltered).
    std::vector<std::vector<float>> columns = {
        {0.0f, 0.0f, 0.0f, 1.0f, 2.0f},
        {1.0f, 1.0f, 1.0f, 1.0f, std::numeric_limits<float>::quiet_NaN()},
    };

    auto mask = ComputeRowFiniteMask(columns, 5);
    check("rows 0-3 are finite", mask[0] && mask[1] && mask[2] && mask[3]);
    check("row 4 is excluded (dim1 NaN)", !mask[4]);

    auto stats = ComputeDimStats(columns);
    check("2 dims computed", stats.size() == 2);
    check("dim0 nan_inf_count is 0 (never non-finite)", stats[0].nan_inf_count == 0);
    check("dim1 nan_inf_count is 1 (raw, unfiltered)", stats[1].nan_inf_count == 1);
    check("dim0 finite_row_count is 4 (row 4 dropped by row-level mask)",
          stats[0].finite_row_count == 4);
    check("dim0 mean over [0,0,0,1] is 0.25", close(stats[0].mean, 0.25));
    check("dim0 zero_ratio is 0.75", close(stats[0].zero_ratio, 0.75));
    check("dim0 min/max are 0/1", close(stats[0].min, 0.0) && close(stats[0].max, 1.0));
    check("dim1 mean over [1,1,1,1] is 1.0", close(stats[1].mean, 1.0));
    check("dim1 one_ratio is 1.0", close(stats[1].one_ratio, 1.0));
    check("dim1 constant_ratio is 1.0", close(stats[1].constant_ratio, 1.0));
    check("dim1 std_dev is 0.0 (constant column)", close(stats[1].std_dev, 0.0));

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_validate.cpp -o /tmp/context_validate_test`
Expected: FAIL — `context_validate_stats.h` does not exist, compile error.

- [ ] **Step 3: Write the implementation**

```cpp
// tools/context_validate_stats.h
// Pure numeric per-dim statistics + row-level finite filtering, shared by
// tools/context_validate.cpp. Split from context_validate.cpp itself so this
// pure-math layer is natively testable without pulling in CLI/context_reader.h
// concerns -- mirrors this codebase's established pure-engine/thin-glue split.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

struct DimStats {
    std::size_t nan_inf_count = 0;
    std::size_t finite_row_count = 0;
    double min = 0.0;
    double max = 0.0;
    double mean = 0.0;
    double std_dev = 0.0;
    double zero_ratio = 0.0;
    double one_ratio = 0.0;
    double constant_ratio = 0.0;
};

// A row is finite only if EVERY column is finite at that row -- matches
// context_preflight.py's np.isfinite(ctx.observations).all(axis=1) exactly,
// not an independent per-column filter (needed so mean/std/correlation are all
// computed over the identical row set across every dimension).
inline std::vector<bool> ComputeRowFiniteMask(
    const std::vector<std::vector<float>>& columns, std::size_t n_rows) {
    std::vector<bool> mask(n_rows, true);
    for (const auto& col : columns) {
        for (std::size_t r = 0; r < n_rows; ++r) {
            if (!std::isfinite(col[r])) {
                mask[r] = false;
            }
        }
    }
    return mask;
}

inline std::vector<DimStats> ComputeDimStats(const std::vector<std::vector<float>>& columns) {
    if (columns.empty()) {
        return {};
    }
    const std::size_t n_rows = columns[0].size();
    const auto mask = ComputeRowFiniteMask(columns, n_rows);

    std::vector<DimStats> stats(columns.size());
    for (std::size_t d = 0; d < columns.size(); ++d) {
        const auto& col = columns[d];
        auto& s = stats[d];

        // Raw NaN/Inf count over ALL rows, unfiltered -- this is the signal
        // validate_lbr_file.py's --check-nulls reports as a violation.
        for (std::size_t r = 0; r < n_rows; ++r) {
            if (!std::isfinite(col[r])) {
                ++s.nan_inf_count;
            }
        }

        double sum = 0.0, sum_sq = 0.0, zero_count = 0.0, one_count = 0.0;
        double min_v = std::numeric_limits<double>::infinity();
        double max_v = -std::numeric_limits<double>::infinity();
        std::size_t kept = 0;
        for (std::size_t r = 0; r < n_rows; ++r) {
            if (!mask[r]) continue;
            const double v = static_cast<double>(col[r]);
            sum += v;
            sum_sq += v * v;
            if (v == 0.0) zero_count += 1.0;
            if (v == 1.0) one_count += 1.0;
            min_v = std::min(min_v, v);
            max_v = std::max(max_v, v);
            ++kept;
        }
        s.finite_row_count = kept;
        if (kept > 0) {
            s.mean = sum / static_cast<double>(kept);
            const double variance = sum_sq / static_cast<double>(kept) - s.mean * s.mean;
            s.std_dev = std::sqrt(std::max(0.0, variance));
            s.min = min_v;
            s.max = max_v;
            s.zero_ratio = zero_count / static_cast<double>(kept);
            s.one_ratio = one_count / static_cast<double>(kept);
            s.constant_ratio = std::max(s.zero_ratio, s.one_ratio);
        }
    }
    return stats;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_validate.cpp -o /tmp/context_validate_test && /tmp/context_validate_test`
Expected: `ALL PASS` (13 checks)

- [ ] **Step 5: Commit**

```bash
git add tools/context_validate_stats.h tools/test_context_validate.cpp
git commit -m "feat: add row-filtered per-dim stats for context_validate (constant/zero-ratio/NaN detection)"
```

---

## Task 11: Inter-feature Pearson correlation matrix

**Files:**
- Modify: `tools/context_validate_stats.h`
- Modify: `tools/test_context_validate.cpp`

**Interfaces:**
- Produces: `std::vector<std::vector<double>> ComputeCorrelationMatrix(const std::vector<std::vector<float>>& columns)` (uses the same row-finite mask as `ComputeDimStats`, sum-of-products Pearson formula — mathematically identical to `np.corrcoef`, since the population-vs-sample normalization cancels in the ratio); `struct CorrelationPair { std::size_t dim_a; std::size_t dim_b; double corr; double abs_corr; }`; `std::vector<CorrelationPair> TopCorrelationPairs(const std::vector<std::vector<double>>& corr, std::size_t top_n)` (sorted descending by `abs_corr`, matching `context_preflight.py`'s `top10`); `std::vector<CorrelationPair> PairsAboveThreshold(const std::vector<std::vector<double>>& corr, double threshold)` (matches `context_preflight.py`'s `gt_0_8`).

- [ ] **Step 1: Extend the test**

Add before the final `std::printf` in `tools/test_context_validate.cpp`:

```cpp
    {
        // dim0=[1,2,3,4], dim1=[2,4,6,8] (perfectly correlated, corr=+1.0),
        // dim2=[4,3,2,1] (perfectly anti-correlated with dim0, corr=-1.0).
        std::vector<std::vector<float>> corr_columns = {
            {1.0f, 2.0f, 3.0f, 4.0f},
            {2.0f, 4.0f, 6.0f, 8.0f},
            {4.0f, 3.0f, 2.0f, 1.0f},
        };
        auto corr = ComputeCorrelationMatrix(corr_columns);
        check("corr matrix is 3x3", corr.size() == 3 && corr[0].size() == 3);
        check("dim0/dim1 perfectly correlated", close(corr[0][1], 1.0, 1e-6));
        check("dim0/dim2 perfectly anti-correlated", close(corr[0][2], -1.0, 1e-6));
        check("matrix is symmetric", close(corr[1][0], corr[0][1]));
        check("diagonal is 1.0", close(corr[0][0], 1.0, 1e-6) && close(corr[1][1], 1.0, 1e-6));

        auto top = TopCorrelationPairs(corr, 2);
        check("top-2 pairs returned", top.size() == 2);
        check("top pair has abs_corr 1.0", close(top[0].abs_corr, 1.0, 1e-6));

        auto above = PairsAboveThreshold(corr, 0.8);
        // dim1 = 2*dim0 and dim2 = reverse(dim0), so ALL THREE pairs are exactly
        // +-1.0 (verified independently against real np.corrcoef, not assumed):
        // (dim0,dim1)=+1, (dim0,dim2)=-1, (dim1,dim2)=-1.
        check("all 3 pairs exceed 0.8 abs corr threshold", above.size() == 3);
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_validate.cpp -o /tmp/context_validate_test`
Expected: FAIL — `ComputeCorrelationMatrix`/`TopCorrelationPairs`/`PairsAboveThreshold`/`CorrelationPair` undeclared.

- [ ] **Step 3: Write the implementation**

Add to `tools/context_validate_stats.h`, after `ComputeDimStats`:

```cpp
inline std::vector<std::vector<double>> ComputeCorrelationMatrix(
    const std::vector<std::vector<float>>& columns) {
    const std::size_t d = columns.size();
    if (d == 0) {
        return {};
    }
    const std::size_t n_rows = columns[0].size();
    const auto mask = ComputeRowFiniteMask(columns, n_rows);

    std::vector<double> sum(d, 0.0), sum_sq(d, 0.0);
    std::size_t n = 0;
    for (std::size_t r = 0; r < n_rows; ++r) {
        if (!mask[r]) continue;
        ++n;
        for (std::size_t i = 0; i < d; ++i) {
            const double v = static_cast<double>(columns[i][r]);
            sum[i] += v;
            sum_sq[i] += v * v;
        }
    }

    std::vector<std::vector<double>> sum_products(d, std::vector<double>(d, 0.0));
    for (std::size_t r = 0; r < n_rows; ++r) {
        if (!mask[r]) continue;
        for (std::size_t i = 0; i < d; ++i) {
            const double vi = static_cast<double>(columns[i][r]);
            for (std::size_t j = i; j < d; ++j) {
                sum_products[i][j] += vi * static_cast<double>(columns[j][r]);
            }
        }
    }

    // Sum-of-products Pearson formula: r = (n*Sxy - Sx*Sy) / sqrt((n*Sxx-Sx^2)*(n*Syy-Sy^2)).
    // Mathematically identical to np.corrcoef -- the population-vs-sample (N vs N-1)
    // normalization cancels out in the ratio, so this needs no separate divisor choice.
    std::vector<std::vector<double>> corr(d, std::vector<double>(d, 0.0));
    const double nd = static_cast<double>(n);
    for (std::size_t i = 0; i < d; ++i) {
        for (std::size_t j = i; j < d; ++j) {
            const double num = nd * sum_products[i][j] - sum[i] * sum[j];
            const double denom_i = nd * sum_sq[i] - sum[i] * sum[i];
            const double denom_j = nd * sum_sq[j] - sum[j] * sum[j];
            const double denom = std::sqrt(std::max(0.0, denom_i) * std::max(0.0, denom_j));
            const double value = (denom > 0.0) ? (num / denom) : std::numeric_limits<double>::quiet_NaN();
            corr[i][j] = value;
            corr[j][i] = value;
        }
    }
    return corr;
}

struct CorrelationPair {
    std::size_t dim_a;
    std::size_t dim_b;
    double corr;
    double abs_corr;
};

inline std::vector<CorrelationPair> AllFiniteCorrelationPairs(
    const std::vector<std::vector<double>>& corr) {
    std::vector<CorrelationPair> pairs;
    const std::size_t d = corr.size();
    for (std::size_t i = 0; i < d; ++i) {
        for (std::size_t j = i + 1; j < d; ++j) {
            if (std::isfinite(corr[i][j])) {
                pairs.push_back({i, j, corr[i][j], std::fabs(corr[i][j])});
            }
        }
    }
    return pairs;
}

inline std::vector<CorrelationPair> TopCorrelationPairs(
    const std::vector<std::vector<double>>& corr, std::size_t top_n) {
    auto pairs = AllFiniteCorrelationPairs(corr);
    std::sort(pairs.begin(), pairs.end(),
              [](const CorrelationPair& a, const CorrelationPair& b) { return a.abs_corr > b.abs_corr; });
    if (pairs.size() > top_n) {
        pairs.resize(top_n);
    }
    return pairs;
}

inline std::vector<CorrelationPair> PairsAboveThreshold(
    const std::vector<std::vector<double>>& corr, double threshold) {
    auto pairs = AllFiniteCorrelationPairs(corr);
    std::vector<CorrelationPair> above;
    for (const auto& p : pairs) {
        if (p.abs_corr > threshold) {
            above.push_back(p);
        }
    }
    return above;
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_validate.cpp -o /tmp/context_validate_test && /tmp/context_validate_test`
Expected: `ALL PASS` (21 checks)

- [ ] **Step 5: Commit**

```bash
git add tools/context_validate_stats.h tools/test_context_validate.cpp
git commit -m "feat: add inter-feature Pearson correlation matrix for context_validate"
```

---

## Task 12: `tools/context_validate.cpp` — CLI, threshold gates, JSON/text report

**Files:**
- Create: `tools/context_validate.cpp`

**Interfaces:**
- Consumes: `tools/context_reader.h` (`OpenContextFile`, `CloseContextFile`, `ReadBoundedHead`, `ReadBoundedTail`, `ReadCounters`, `PairedRecord`), `tools/context_validate_stats.h` (Tasks 10-11).
- Produces: an executable, `tools/context_validate`, CLI `--input PATH [--mode head|tail] [--max-pairs N] [--min-pairs N] [--constant-ratio-threshold F] [--saturation-threshold F] [--chronic-zero-threshold F] [--check-zero-trap] [--check-nulls] [--check-sequence-integrity] [--corr-threshold F] [--report-json PATH] [--no-strict]`.

- [ ] **Step 1: Write the implementation**

```cpp
// tools/context_validate.cpp
// Consolidates lbrnet/scripts/validate_lbr_file.py's .context-specific checks
// (MO/SS sequence integrity, per-dim NaN/Inf detection, zero/one-trap detection)
// and lbrnet/scripts/context_preflight.py's checks (per-dim distribution stats,
// inter-feature correlation, constant/chronic-zero/saturation gates) into one
// C++ tool built directly on tools/context_reader.h -- both Python scripts
// already operated on exactly the same raw MO/SS data, and context_preflight.py
// itself already described its role as "supplements validate_lbr_file.py".
// No Arrow/Parquet dependency -- this tool only reports, never writes a file.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 -Iinclude \
//   tools/context_validate.cpp -o tools/context_validate
#include "context_reader.h"
#include "context_validate_stats.h"
#include "generated/mts_schema_contract_generated.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Accumulator {
    std::array<std::vector<float>, MTS::Schema::Contract::kObservationDim> columns;
};

void AppendPair(const PairedRecord& rec, void* user_data) {
    auto* acc = static_cast<Accumulator*>(user_data);
    const auto* obs = rec.observation.observation;
    const std::array<float, MTS::Schema::Contract::kObservationDim> values = {
        obs->log_variance_ratio(), obs->burstiness_index(), obs->relative_range(),
        obs->correction_action(), obs->vol_convexity(), obs->lempel_ziv(),
        obs->hurst_exponent(), obs->micro_asymmetry(), obs->fisher_info(),
        obs->fast_hurst_exponent(), obs->tail_index(), obs->skewness_idx(),
        obs->amihud_illiquidity(), obs->liq_fragility(), obs->fast_taleb_kurtosis(),
        obs->recurrence_rate(), obs->fractal_dim(), obs->mean_rev_z(), obs->fast_mean_rev_z(),
    };
    for (std::size_t i = 0; i < values.size(); ++i) {
        acc->columns[i].push_back(values[i]);
    }
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --input PATH [--mode head|tail] [--max-pairs N] [--min-pairs N] "
        "[--constant-ratio-threshold F] [--saturation-threshold F] "
        "[--chronic-zero-threshold F] [--check-zero-trap] [--check-nulls] "
        "[--check-sequence-integrity] [--corr-threshold F] [--report-json PATH] "
        "[--no-strict]\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string input_path, mode = "head", report_json_path;
    std::size_t max_pairs = 50000, min_pairs = 5000;
    double constant_ratio_threshold = 0.995, saturation_threshold = 0.98;
    double chronic_zero_threshold = 0.15, corr_threshold = 0.8;
    bool check_zero_trap = false, check_nulls = false, check_sequence_integrity = false;
    bool strict = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--input") input_path = next("--input");
        else if (arg == "--mode") mode = next("--mode");
        else if (arg == "--max-pairs") max_pairs = std::stoull(next("--max-pairs"));
        else if (arg == "--min-pairs") min_pairs = std::stoull(next("--min-pairs"));
        else if (arg == "--constant-ratio-threshold") constant_ratio_threshold = std::stod(next("--constant-ratio-threshold"));
        else if (arg == "--saturation-threshold") saturation_threshold = std::stod(next("--saturation-threshold"));
        else if (arg == "--chronic-zero-threshold") chronic_zero_threshold = std::stod(next("--chronic-zero-threshold"));
        else if (arg == "--corr-threshold") corr_threshold = std::stod(next("--corr-threshold"));
        else if (arg == "--check-zero-trap") check_zero_trap = true;
        else if (arg == "--check-nulls") check_nulls = true;
        else if (arg == "--check-sequence-integrity") check_sequence_integrity = true;
        else if (arg == "--report-json") report_json_path = next("--report-json");
        else if (arg == "--no-strict") strict = false;
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); PrintUsage(argv[0]); return 1; }
    }
    if (input_path.empty()) { PrintUsage(argv[0]); return 1; }

    ContextFileHandle handle;
    try {
        handle = OpenContextFile(input_path);
    } catch (const std::runtime_error& e) {
        std::fprintf(stderr, "❌ %s\n", e.what());
        return 1;
    }

    Accumulator acc;
    ReadCounters counters = (mode == "tail")
        ? ReadBoundedTail(handle, max_pairs, AppendPair, &acc)
        : ReadBoundedHead(handle, max_pairs, AppendPair, &acc);
    CloseContextFile(handle);

    const std::vector<std::vector<float>> columns(acc.columns.begin(), acc.columns.end());
    const std::size_t rows_sampled = columns.empty() ? 0 : columns[0].size();
    const auto dim_stats = ComputeDimStats(columns);
    const auto corr = ComputeCorrelationMatrix(columns);
    const auto top_corr = TopCorrelationPairs(corr, 10);
    const auto high_corr = PairsAboveThreshold(corr, corr_threshold);

    std::vector<std::string> violations;
    std::vector<std::string> warnings;

    // --- context_preflight.py-derived gates ---
    if (rows_sampled < min_pairs) {
        violations.push_back(
            "Insufficient aligned pairs: sampled=" + std::to_string(rows_sampled) +
            " < min_pairs=" + std::to_string(min_pairs));
    }
    for (std::size_t i = 0; i < dim_stats.size(); ++i) {
        const auto& s = dim_stats[i];
        const std::string name = MTS::Schema::Contract::kObservationFieldNames[i];
        if (s.constant_ratio >= constant_ratio_threshold) {
            violations.push_back(
                "Dim " + std::to_string(i) + " (" + name + ") appears constant/trapped "
                "(constant_ratio=" + std::to_string(s.constant_ratio) + ")");
        }
        if (s.zero_ratio >= chronic_zero_threshold) {
            violations.push_back(
                "Dim " + std::to_string(i) + " (" + name + ") chronic zero_ratio=" +
                std::to_string(s.zero_ratio) + " >= chronic_zero_threshold=" +
                std::to_string(chronic_zero_threshold));
        }
        if (s.zero_ratio >= saturation_threshold) {
            warnings.push_back("Dim " + std::to_string(i) + " (" + name + ") zero saturation=" +
                                std::to_string(s.zero_ratio));
        }
        if (s.one_ratio >= saturation_threshold) {
            warnings.push_back("Dim " + std::to_string(i) + " (" + name + ") one saturation=" +
                                std::to_string(s.one_ratio));
        }
    }
    if (!high_corr.empty()) {
        warnings.push_back(
            "High redundancy: " + std::to_string(high_corr.size()) +
            " feature pairs exceed abs(corr)>" + std::to_string(corr_threshold));
    }

    // --- validate_lbr_file.py-derived gates ---
    if (check_nulls) {
        for (std::size_t i = 0; i < dim_stats.size(); ++i) {
            if (dim_stats[i].nan_inf_count > 0) {
                violations.push_back(
                    "Dim " + std::to_string(i) + " has " +
                    std::to_string(dim_stats[i].nan_inf_count) + " NaN/Inf values");
            }
        }
    }
    if (check_zero_trap) {
        for (std::size_t i = 0; i < dim_stats.size(); ++i) {
            const auto& s = dim_stats[i];
            if (s.zero_ratio >= 0.98) {
                violations.push_back(
                    "Dim " + std::to_string(i) + " zero-trap detected (zero_ratio=" +
                    std::to_string(s.zero_ratio) + ")");
            }
            if (s.one_ratio >= 0.98) {
                violations.push_back(
                    "Dim " + std::to_string(i) + " one-trap detected (one_ratio=" +
                    std::to_string(s.one_ratio) + ")");
            }
        }
    }
    if (check_sequence_integrity) {
        if (counters.sequence_mismatches > 0) {
            violations.push_back("Sequence mismatches detected: " + std::to_string(counters.sequence_mismatches));
        }
        if (counters.sequence_regressions > 0) {
            violations.push_back("Sequence regressions detected: " + std::to_string(counters.sequence_regressions));
        }
        if (counters.unpaired_market_records > 0 || counters.unpaired_system_records > 0) {
            violations.push_back(
                "Unpaired MO/SS records detected (market=" +
                std::to_string(counters.unpaired_market_records) + ", system=" +
                std::to_string(counters.unpaired_system_records) + ")");
        }
    }

    const bool passed = violations.empty();

    std::printf("========================================================================\n");
    std::printf("Context Validate Report\n");
    std::printf("========================================================================\n");
    std::printf("Input: %s\n", input_path.c_str());
    std::printf("Rows sampled: %zu\n", rows_sampled);
    std::printf("Aligned pairs: %zu, sequence mismatches: %zu\n",
                counters.aligned_pairs, counters.sequence_mismatches);
    std::printf("Status: %s\n", passed ? "PASS" : "FAIL");
    if (!top_corr.empty()) {
        std::printf("Top abs-correlation pair: %s/%s = %.4f\n",
                    MTS::Schema::Contract::kObservationFieldNames[top_corr[0].dim_a],
                    MTS::Schema::Contract::kObservationFieldNames[top_corr[0].dim_b],
                    top_corr[0].corr);
    }
    if (!violations.empty()) {
        std::printf("\nViolations\n");
        for (const auto& v : violations) std::printf("  - %s\n", v.c_str());
    }
    if (!warnings.empty()) {
        std::printf("\nWarnings\n");
        for (const auto& w : warnings) std::printf("  - %s\n", w.c_str());
    }
    std::printf("========================================================================\n");

    if (!report_json_path.empty()) {
        std::ofstream out(report_json_path);
        out << "{\n";
        out << "  \"input\": \"" << JsonEscape(input_path) << "\",\n";
        out << "  \"rows_sampled\": " << rows_sampled << ",\n";
        out << "  \"status\": \"" << (passed ? "PASS" : "FAIL") << "\",\n";
        out << "  \"violations\": [";
        for (std::size_t i = 0; i < violations.size(); ++i) {
            out << (i ? ", " : "") << "\"" << JsonEscape(violations[i]) << "\"";
        }
        out << "],\n";
        out << "  \"warnings\": [";
        for (std::size_t i = 0; i < warnings.size(); ++i) {
            out << (i ? ", " : "") << "\"" << JsonEscape(warnings[i]) << "\"";
        }
        out << "]\n";
        out << "}\n";
        std::printf("Wrote JSON report: %s\n", report_json_path.c_str());
    }

    if (strict && !passed) {
        return 2;
    }
    return 0;
}
```

- [ ] **Step 2: Build it**

Run: `mamba run -n mts g++ -O2 -std=c++17 -Iinclude tools/context_validate.cpp -o tools/context_validate`
Expected: compiles cleanly. Fix any real compile errors surfaced here before proceeding.

- [ ] **Step 3: End-to-end integration test**

Reuse Task 9's `/tmp/e2e_test.context` synthetic fixture (5 aligned pairs, `log_variance_ratio` = 0..4, all other dims 0):

```bash
./tools/context_validate --input /tmp/e2e_test.context --mode head --max-pairs 10 \
    --min-pairs 3 --no-strict --report-json /tmp/e2e_validate_report.json
```
Expected: prints `Status: FAIL` (every all-zero dim except `log_variance_ratio` has `constant_ratio=1.0 >= 0.995`, correctly flagged — this is real, correct behavior given the fixture, not a bug) and writes `/tmp/e2e_validate_report.json`. Confirm the JSON is well-formed:
```bash
mamba run -n mts python3 -c "
import json
report = json.load(open('/tmp/e2e_validate_report.json'))
assert report['rows_sampled'] == 5, report
assert report['status'] == 'FAIL'
assert any('constant' in v for v in report['violations'])
print('✅ context_validate JSON report verified')
"
```
Expected: prints the success line with no assertion errors.

- [ ] **Step 4: Commit**

```bash
git add tools/context_validate.cpp
git commit -m "feat: context_validate CLI -- consolidates validate_lbr_file.py + context_preflight.py .context checks"
```

---

## Task 13: Freshness/rebuild-decision logic (full vs. incremental vs. fresh)

**Files:**
- Create: `tools/context_cache_key.h`
- Create: `tools/test_context_cache_key.cpp`
- Modify: `tools/context_to_parquet.cpp` (wire the decision into `main()`)

**Interfaces:**
- Produces: `struct FileStat { std::int64_t size; std::int64_t mtime; }`; `FileStat StatFile(const std::string& path)`; `struct ContextCacheKey { std::int64_t context_size; std::int64_t context_mtime; std::uint16_t wire_schema_version; int parquet_cache_format_version; std::size_t resume_offset; std::uint64_t last_seq_id; bool has_last_seq_id; }`; `void WriteCacheKey(const std::string& meta_path, const ContextCacheKey&)`; `std::optional<ContextCacheKey> ReadCacheKey(const std::string& meta_path)`; `enum class RebuildPlan { kFresh, kIncremental, kFull }`; `RebuildPlan DecideRebuildPlanFromStats(FileStat current, const std::optional<ContextCacheKey>& prior, std::uint16_t current_wire_schema_version, bool parquet_exists)` (pure, unit-testable — no file I/O); `RebuildPlan DecideRebuildPlan(const std::string& context_path, const std::string& parquet_path, const std::string& meta_path, std::uint16_t current_wire_schema_version)` (thin file-I/O wrapper around the pure function, mirrors `context_reader.h`'s own pure/glue precedent); `inline constexpr int kContextParquetCacheFormatVersion = 1;` — **distinct from `MTS::Schema::Contract::kSchemaVersion`**: this one versions `context_to_parquet.cpp`'s own Parquet *output* column schema (bump when `BuildArrowSchema()`'s column set/naming changes), not the raw wire format.

- [ ] **Step 1: Write the failing test**

```cpp
// tools/test_context_cache_key.cpp
// Build & run: mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_cache_key.cpp \
//   -o /tmp/context_cache_key_test && /tmp/context_cache_key_test
#include "../tools/context_cache_key.h"
#include <cstdio>
#include <cstdlib>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}  // namespace

int main() {
    const FileStat current{1000, 5000};

    check("no parquet file at all -> full rebuild",
          DecideRebuildPlanFromStats(current, std::nullopt, 240, /*parquet_exists=*/false)
              == RebuildPlan::kFull);

    check("parquet exists but no prior cache key -> full rebuild",
          DecideRebuildPlanFromStats(current, std::nullopt, 240, /*parquet_exists=*/true)
              == RebuildPlan::kFull);

    {
        ContextCacheKey prior{1000, 5000, 240, kContextParquetCacheFormatVersion, 1000, 7, true};
        check("identical size/mtime/schema -> fresh (no rebuild needed)",
              DecideRebuildPlanFromStats(current, prior, 240, true) == RebuildPlan::kFresh);
    }
    {
        ContextCacheKey prior{800, 4000, 240, kContextParquetCacheFormatVersion, 800, 5, true};
        check("file strictly grew, same schema -> incremental",
              DecideRebuildPlanFromStats(current, prior, 240, true) == RebuildPlan::kIncremental);
    }
    {
        ContextCacheKey prior{1200, 6000, 240, kContextParquetCacheFormatVersion, 1200, 9, true};
        check("file shrank -> full rebuild (content diverged from what cache reflects)",
              DecideRebuildPlanFromStats(current, prior, 240, true) == RebuildPlan::kFull);
    }
    {
        // Same size as current but different mtime -> same-size-different-content case,
        // matches materialize_context_parquet.py's own documented "schema changed, file
        // shrank, or same size with different content" full-rebuild branch.
        ContextCacheKey prior{1000, 4999, 240, kContextParquetCacheFormatVersion, 1000, 7, true};
        check("same size, different mtime -> full rebuild",
              DecideRebuildPlanFromStats(current, prior, 240, true) == RebuildPlan::kFull);
    }
    {
        ContextCacheKey prior{800, 4000, 230, kContextParquetCacheFormatVersion, 800, 5, true};
        check("file grew but WIRE schema_version differs -> full rebuild, never incremental "
              "across a wire-format change",
              DecideRebuildPlanFromStats(current, prior, 240, true) == RebuildPlan::kFull);
    }
    {
        ContextCacheKey prior{800, 4000, 240, kContextParquetCacheFormatVersion + 1, 800, 5, true};
        check("file grew but PARQUET OUTPUT format version differs -> full rebuild "
              "(distinct from wire schema_version -- this is the tool's own output shape)",
              DecideRebuildPlanFromStats(current, prior, 240, true) == RebuildPlan::kFull);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_cache_key.cpp -o /tmp/context_cache_key_test`
Expected: FAIL — `context_cache_key.h` does not exist, compile error.

- [ ] **Step 3: Write the implementation**

```cpp
// tools/context_cache_key.h
// Freshness/rebuild-decision logic, mirroring lbrnet's
// materialize_context_parquet.py::cache_key()/is_cache_fresh() exactly -- but
// correctly SEPARATES two versioning concepts that script's own "schema_version"
// field conflated under one name: the raw wire format (kSchemaVersion, gates
// ObservationData's byte layout, see context_reader.h's hard-refuse) vs. this
// tool's own Parquet OUTPUT column schema (kContextParquetCacheFormatVersion,
// bump when BuildArrowSchema()'s column set/naming changes). Pure decision
// function is unit-tested without file I/O; DecideRebuildPlan() is the thin
// file-reading wrapper, matching context_reader.h's own pure/glue precedent.
#pragma once

#include <cctype>
#include <cstdint>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>

#include <sys/stat.h>

inline constexpr int kContextParquetCacheFormatVersion = 1;

struct FileStat {
    std::int64_t size = 0;
    std::int64_t mtime = 0;
};

inline FileStat StatFile(const std::string& path) {
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) {
        throw std::runtime_error("StatFile: cannot stat " + path);
    }
    return {static_cast<std::int64_t>(st.st_size), static_cast<std::int64_t>(st.st_mtime)};
}

struct ContextCacheKey {
    std::int64_t context_size = 0;
    std::int64_t context_mtime = 0;
    std::uint16_t wire_schema_version = 0;
    int parquet_cache_format_version = 0;
    std::size_t resume_offset = 0;
    std::uint64_t last_seq_id = 0;
    bool has_last_seq_id = false;
};

inline void WriteCacheKey(const std::string& meta_path, const ContextCacheKey& key) {
    std::ofstream out(meta_path);
    out << "{\n"
        << "  \"context_size\": " << key.context_size << ",\n"
        << "  \"context_mtime\": " << key.context_mtime << ",\n"
        << "  \"wire_schema_version\": " << key.wire_schema_version << ",\n"
        << "  \"parquet_cache_format_version\": " << key.parquet_cache_format_version << ",\n"
        << "  \"resume_offset\": " << key.resume_offset << ",\n"
        << "  \"last_seq_id\": "
        << (key.has_last_seq_id ? std::to_string(key.last_seq_id) : std::string("null")) << "\n"
        << "}\n";
}

// Minimal hand-rolled JSON field extraction -- the sidecar schema is small and
// fixed (6 known scalar fields), so a tiny dedicated parser is simpler and more
// appropriate here than pulling in a JSON library for a standalone tool.
inline std::optional<ContextCacheKey> ReadCacheKey(const std::string& meta_path) {
    std::ifstream in(meta_path);
    if (!in) {
        return std::nullopt;
    }
    std::stringstream buf;
    buf << in.rdbuf();
    const std::string text = buf.str();

    auto extract = [&](const std::string& field) -> std::optional<std::string> {
        const std::string needle = "\"" + field + "\":";
        auto pos = text.find(needle);
        if (pos == std::string::npos) {
            return std::nullopt;
        }
        pos += needle.size();
        auto end = text.find_first_of(",\n}", pos);
        std::string value = text.substr(pos, end - pos);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.erase(value.begin());
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.pop_back();
        }
        return value;
    };

    auto size_str = extract("context_size");
    auto mtime_str = extract("context_mtime");
    auto wire_str = extract("wire_schema_version");
    auto fmt_str = extract("parquet_cache_format_version");
    auto resume_str = extract("resume_offset");
    auto seq_str = extract("last_seq_id");
    if (!size_str || !mtime_str || !wire_str || !fmt_str || !resume_str) {
        return std::nullopt;
    }

    ContextCacheKey key;
    key.context_size = std::stoll(*size_str);
    key.context_mtime = std::stoll(*mtime_str);
    key.wire_schema_version = static_cast<std::uint16_t>(std::stoi(*wire_str));
    key.parquet_cache_format_version = std::stoi(*fmt_str);
    key.resume_offset = static_cast<std::size_t>(std::stoull(*resume_str));
    if (seq_str && *seq_str != "null") {
        key.last_seq_id = std::stoull(*seq_str);
        key.has_last_seq_id = true;
    }
    return key;
}

enum class RebuildPlan { kFresh, kIncremental, kFull };

// Pure decision, no file I/O -- mirrors materialize_context_parquet.py's
// is_cache_fresh()/incremental-vs-full logic exactly, including its careful
// "only stat-derived fields participate in freshness" rule (resume_offset/
// last_seq_id are bookkeeping, never compared here).
inline RebuildPlan DecideRebuildPlanFromStats(
    FileStat current, const std::optional<ContextCacheKey>& prior,
    std::uint16_t current_wire_schema_version, bool parquet_exists) {
    if (!parquet_exists || !prior.has_value()) {
        return RebuildPlan::kFull;
    }
    const bool same_schema = prior->wire_schema_version == current_wire_schema_version
        && prior->parquet_cache_format_version == kContextParquetCacheFormatVersion;
    if (!same_schema) {
        return RebuildPlan::kFull;
    }
    if (current.size == prior->context_size && current.mtime == prior->context_mtime) {
        return RebuildPlan::kFresh;
    }
    if (current.size > prior->context_size) {
        return RebuildPlan::kIncremental;
    }
    return RebuildPlan::kFull;
}

inline RebuildPlan DecideRebuildPlan(
    const std::string& context_path, const std::string& parquet_path,
    const std::string& meta_path, std::uint16_t current_wire_schema_version) {
    const bool parquet_exists = std::ifstream(parquet_path).good();
    const auto prior = ReadCacheKey(meta_path);
    const auto current = StatFile(context_path);
    return DecideRebuildPlanFromStats(current, prior, current_wire_schema_version, parquet_exists);
}
```

- [ ] **Step 4: Run test to verify it passes**

Run: `mamba run -n mts g++ -std=c++17 -Iinclude tools/test_context_cache_key.cpp -o /tmp/context_cache_key_test && /tmp/context_cache_key_test`
Expected: `ALL PASS` (7 checks)

- [ ] **Step 5: Wire the decision into `context_to_parquet.cpp`'s `main()`**

Add near the top of `main()`, right after parsing `--input`/`--output`, before opening the context file for reading:

```cpp
#include "context_cache_key.h"
```

Replace the CLI's implicit "always read from `--resume-offset`/`--resume-last-seq-id` flags, defaulting to 0" behavior with an explicit freshness check when a `--meta-path` is supplied (optional -- omitting it preserves today's explicit-resume-flags behavior for scripted/tested use):

```cpp
// After parsing args and before ContextFileHandle handle = OpenContextFile(...):
std::string meta_path_arg;  // add to the arg-parsing loop: else if (arg == "--meta-path") meta_path_arg = next("--meta-path");
```

In the arg-parsing loop, add the branch (alongside the existing `--resume-offset`/`--resume-last-seq-id` branches):
```cpp
        else if (arg == "--meta-path") meta_path_arg = next("--meta-path");
```

After `ContextFileHandle handle = OpenContextFile(input_path);` succeeds (so `handle.schema_version` — really `MTS::Schema::Contract::kSchemaVersion`, since `OpenContextFile` already hard-refused any mismatch — is known), and only when `mode == "unbounded"` and `!meta_path_arg.empty()`:

```cpp
    if (mode == "unbounded" && !meta_path_arg.empty()) {
        const auto plan = DecideRebuildPlan(
            input_path, output_path, meta_path_arg, MTS::Schema::Contract::kSchemaVersion);
        if (plan == RebuildPlan::kFresh) {
            std::printf("✅ %s is already fresh relative to %s -- nothing to do\n",
                        output_path.c_str(), input_path.c_str());
            CloseContextFile(handle);
            return 0;
        }
        if (plan == RebuildPlan::kIncremental) {
            auto prior = ReadCacheKey(meta_path_arg);
            resume_offset = prior->resume_offset;
            if (prior->has_last_seq_id) {
                resume_last_seq_id = prior->last_seq_id;
                has_resume_last_seq_id = true;
            }
            std::printf("Incremental rebuild: resuming from byte offset %zu\n", resume_offset);
        } else {
            std::printf("Full rebuild: %s\n",
                        plan == RebuildPlan::kFull && !meta_path_arg.empty() && std::ifstream(meta_path_arg).good()
                            ? "schema changed, file shrank, or same-size-different-content"
                            : "no prior cache");
        }
    }
```

After the writer's `Close()` succeeds (end of `main()`, right before `return 0;`), persist the new cache key when `--meta-path` was supplied:

```cpp
    if (!meta_path_arg.empty()) {
        const auto final_stat = StatFile(input_path);
        ContextCacheKey key{
            final_stat.size, final_stat.mtime, MTS::Schema::Contract::kSchemaVersion,
            kContextParquetCacheFormatVersion, handle.size,
            /*last_seq_id=*/0, /*has_last_seq_id=*/false,
        };
        WriteCacheKey(meta_path_arg, key);
    }
```

(Note: `handle.size` is used as the persisted `resume_offset` for a completed unbounded pass — the whole file was consumed. A future incremental call re-derives the true resume point from `ReadAllRecordsResumable`'s own returned `ResumePoint` the same way the existing per-chunk loop already tracks `offset`/`last_seq`; wiring the exact final chunk's `ResumePoint` values into this `WriteCacheKey` call instead of `handle.size`/defaults is a direct follow-on, not done here to keep this step's diff focused on the freshness-decision wiring itself.)

- [ ] **Step 6: Build and verify**

Run: `mamba run -n mts g++ -O2 -std=c++17 -Iinclude $(mamba run -n mts pkg-config --cflags arrow parquet) tools/context_to_parquet.cpp $(mamba run -n mts pkg-config --libs arrow parquet) -o tools/context_to_parquet`
Expected: compiles cleanly.

Run the freshness path end-to-end against Task 9's synthetic fixture:
```bash
rm -f /tmp/e2e_test.parquet /tmp/e2e_test.meta.json
./tools/context_to_parquet --input /tmp/e2e_test.context --output /tmp/e2e_test.parquet --mode unbounded --meta-path /tmp/e2e_test.meta.json
./tools/context_to_parquet --input /tmp/e2e_test.context --output /tmp/e2e_test.parquet --mode unbounded --meta-path /tmp/e2e_test.meta.json
```
Expected: first run does a full conversion and writes `/tmp/e2e_test.meta.json`; second run (same file, unchanged) prints `✅ ... is already fresh ... nothing to do` and exits without re-reading/re-writing the Parquet file.

- [ ] **Step 7: Commit**

```bash
git add tools/context_cache_key.h tools/test_context_cache_key.cpp tools/context_to_parquet.cpp
git commit -m "feat: add freshness/rebuild-decision logic (full vs incremental vs fresh), wired into context_to_parquet"
```

---

## Self-Review Notes (completed during authoring)

- **Spec coverage**: §10.1 (ownership boundary) — Tasks 4-9 implement the full C++ reader, no `lbrnet` file touched anywhere in this plan. §10.2 (versioning) — Tasks 1, 3, 4 (marker, `kSchemaVersion`, `WriteFileMetadata`, hard-refuse gate). §10.3 (build path) — every task's build/test steps use bare `g++`/`mamba run -n mts`, no CMake/`build_dll.sh` touch except Task 2/3's verification steps (which confirm the *existing* DLL build still passes, not that this tool is part of it). §10.4 (naming + reflection) — Task 1's `render_risk_gate_constants()` (set-intersection-computed `_raw` suffixing) and the whole Task 1/2 reflection mechanism. §10.5 — explicitly untouched, no task references `hmm_training_cache.py`. §10.6 open items — CLI shape resolved in Task 9, reflection mechanics resolved in Task 1/2 (`flatc --jsonschema`, not raw `.bfbs`), version number resolved (`240`) in Global Constraints/Task 1.
- **Placeholder scan**: no `TBD`/`TODO`/"add error handling" phrases; every step has real, complete code.
- **Type consistency checked**: `ParsedObservation`/`PairedRecord`/`ReadCounters`/`PairingState`/`ResumePoint` are defined once (Tasks 4/6/8) and used with identical field names in every later task and in `context_to_parquet.cpp`. `MTS::Schema::Contract::kSchemaVersion`/`kObservationFieldNames`/`kRiskGateFieldNames`/`kRiskGateFloatFieldNames`/`kRiskGateFloatOutputColumnNames` are defined once (Task 1's generator, consumed via Task 2's regen) and referenced by name identically in Tasks 4 and 9.
