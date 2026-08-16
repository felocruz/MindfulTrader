# Spec: `risk_gate_context` C++ Co-Evolution — Close the Population Gap, Recalibrate Live Gates, Shared Calibration Config

**Status**: SPEC — approved via `superpowers:brainstorming` from an lbrnet-rooted session (2026-08-15).
**Implementation deferred** to a future MindfulTrader-rooted session — nothing in this spec has been
built yet. Read this in full plus `SCRATCHPAD.md`'s 2026-08-15 section before starting.
**Companion spec**: `lbrnet/docs/superpowers/specs/2026-08-15-risk-gate-context-backtester-fidelity.md`
(Python side — already implemented under that spec; this spec's Unit A directly unblocks that spec's
temporary pass-through shim).
**Prerequisite reading**: `docs/ADR/amihud_gate_percentile_spec.md` (the precedent this spec
generalizes), `docs/ADR/risk_gate_context_wire_spec.md`, `docs/ADR/params_convergence_spec.md`
(PC-15/16/17).

## Purpose

An lbrnet-rooted session found, while auditing real collected data, that `RiskGateContext` — the
raw/unscaled risk-gate telemetry `ContextManager.cpp` ships specifically so Python's backtester can
read the same signals `RiskManager`'s live hard gates evaluate — is only present on 67.9% of
`MarketObservation` records, and that at least one live gate threshold
(`taleb_signal_sigma_threshold`) has drifted to three different values across three places in the
codebase with no mechanism preventing it. Both are real, structural gaps in the live system, not
just a Python-side parity problem. This spec closes them, and generalizes
`amihud_gate_percentile_spec.md`'s pattern (raw signal → C++ computes once, ships on the wire, Python
reads directly) to the rest of the gate stack.

## Audit Findings (established 2026-08-15, from the lbrnet-rooted session)

1. **`risk_gate_context` population gap, root cause identified but not yet fully traced.**
   Verified empirically: 67.9% of 2,000,000 sampled `MarketObservation` records (from
   `event_data_20260814_163135.context`) have `risk_gate_context` populated (`is_valid=true` ~100%
   of the time when present); 32.1% have it entirely absent. `EventDataCollectorStudy.cpp:788-797`
   calls `LBRFileManager::LogSynchronizedEvent(*eventT, *eventT->observation,
   *eventT->asymmetry_context, now_us, stitched_regime_tenure)` directly — this call omits the
   `RiskGateContextT*` argument, which defaults to `nullptr`
   (`LBRFileManager.h:44-50`/`:61-68`, `LogContextUnlocked`'s doc comment: "When the caller passes
   nullptr (live / synchronized paths) the field is left unset"). Separately,
   `ContextManager::EmitTrainingContext()` (`ContextManager.cpp:888-935`, called from
   `CheckAndTriggerHMM`'s own internal PATH-1 branch, itself invoked from
   `EventDataCollectorStudy.cpp:532`) *does* build and pass a fully-populated `RiskGateContextT` via
   `LogContext(obs_data, asymContext, now_us, bars_since_last_update, &rgc)`. **Working hypothesis,
   not yet confirmed**: these are two independently-triggered writers into the same `.context`
   stream — `CheckAndTriggerHMM`'s own internal condition for calling `EmitTrainingContext` and
   `EventDataCollectorStudy`'s own `HasSignificantChange()`-gated direct call are not the same
   condition, so the file ends up interleaving output from two logically distinct writers, one of
   which never carries `risk_gate_context`. **First implementation step must be tracing this
   precisely** (instrument both call sites, confirm the 68/32 split maps to "which writer fired,"
   not something else) before designing the fix — do not assume the hypothesis above without
   verifying it against a live trace.
2. **Config drift, already causing a real discrepancy.** `taleb_signal_sigma_threshold` has three
   different values across the codebase: the live `hmm_regime_risk_policy.json`'s
   `empirical_gate_thresholds.taleb_signal_sigma_threshold = 1.8401` (rescaled 2026-08-13 from an
   older `9.697616...`), `backtest_runner.py:1845`'s hardcoded fallback default `9.636797`, and
   lbrnet's own more recent HMM calibration figure `6.67559116507085` (dated 2026-07-26). No
   mechanism keeps these in sync — `lbrnet/scripts/calibrate_hmm_gate_thresholds.py` produces
   `models/HMMEmpiricalGateThresholds.json`, but no script was found anywhere that copies that into
   the live policy file's `empirical_gate_thresholds` block. It appears to have been hand-copied
   once and never re-synced.
3. **FeatureScaler winsorization bounds are hardcoded C++ constants with no shared-config
   equivalent.** `STATE_WINSOR_SIGMA=6.0`, `DIM_WINSOR_SIGMA_OVERRIDE`/`LOGZ_WINSOR_SIGMA_OVERRIDE`/
   `SHRINKAGE_SCALE_MIN` (`FeatureScaler.h`) are `static constexpr`, requiring a C++ recompile +
   `build_dll.sh` to change, with no file Python could read to independently confirm the current
   bound. This is the same failure class as Finding 2 — no single source of truth between languages.
4. **Neither live config file has any git history.** `execution_params.json`/
   `hmm_regime_risk_policy.json` (`/mnt/c/Trading/config/`) exist only as live files plus local
   `.bak.<timestamp>` filesystem copies (both `promote_to_cpp_config.py`'s own backup mechanism and
   pre-existing `.bak_YYYYMMDD_HHMMSS.json` files were found). No diffable, reviewable history exists
   for either file today.
5. **Existing precedent to generalize, not invent from scratch**: `amihud_gate_percentile_spec.md`
   (locked 2026-07-15, C++ side landed, Python side handed off) already did exactly this pattern for
   one gate family (Amihud/PC-17): fixed a non-canonical raw formula, added a session-aware rolling
   percentile computed once in C++, shipped both raw value and percentile on the wire
   (`RiskGateContext.amihud_illiquidity`/`.amihud_percentile`), and reclassified the Python-side
   empirical rebase from "stopgap" to "canonical" once the real percentile existed. This spec's
   Unit B is that same audit, applied to whichever other live gates still compare a raw,
   non-stationary microstructure value against a fixed constant (the exact bug class Finding 2's
   `taleb_signal_sigma_threshold` may itself belong to).
6. **User-directed governance style**: solo developer + AI agents across 3 projects — prefer
   structural correctness (schema versioning, provenance, single-writer-per-section discipline) over
   procedural overhead (no CI gate, no forced-acknowledgment blocking builds, no approval workflow).
   Versioning convention: `"1.0.2"` semver-style, applied consistently (today the two live files use
   two different schemes). Ownership conflicts within one file are resolved by **sectioned ownership
   within a single file** (explicit `_owner`/`_generated_by` markers per section, strict
   read-modify-write discipline per writer) rather than physically splitting into more files — this
   was an explicit, deliberated choice, not a default; the two-JSON-file structure stays.

## Governing objective

Close the `risk_gate_context` population gap so it's unconditionally present going forward (directly
unblocking the companion lbrnet spec's temporary pass-through shim); give both live config files real
git history and a single, versioned, provenance-tracked shared home for calibration constants; audit
the rest of the live gate stack for the same drifted-threshold/non-stationary-comparison pattern
`amihud_gate_percentile_spec.md` already fixed once.

---

## Unit A: Close the `risk_gate_context` population gap

### Problem

See Audit Finding 1. Whatever the precise mechanism, the fix must not duplicate
`LocalRiskContext` computation at a second call site — `ContextManager` already maintains
`m_localRiskContext` as the single source of truth; the fix should expose and reuse it, not recompute it.

### Design (direction, not yet verified against a live trace — confirm Finding 1's hypothesis first)

1. **Trace first**: instrument both `CheckAndTriggerHMM`'s internal `EmitTrainingContext` call site
   and `EventDataCollectorStudy.cpp:788`'s direct `LogSynchronizedEvent` call site (temporary log
   lines or a debug build) against a short live/replay session; confirm whether every
   `LogSynchronizedEvent` call genuinely lacks a fresh `LocalRiskContext`, or whether some other
   condition (e.g. `m_localRiskContext.isValid` being false at that exact instant) is the real cause.
2. **If Finding 1's hypothesis holds**: add a `ContextManager::GetCurrentRiskGateContext() const`
   accessor (or similar) that returns the current `m_localRiskContext` snapshot, already built the
   same way `EmitTrainingContext` builds it internally (lines 906-923) — factor that
   `LocalRiskContext → RiskGateContextT` field-by-field mapping into a small shared helper both call
   sites use, rather than duplicating the 16-field assignment block. Add a `RiskGateContextT*`
   parameter to `LBRFileManager::LogSynchronizedEvent()` (defaulting to `nullptr` for any other
   caller, matching `LogContext`'s existing pattern) and pass the fresh snapshot from
   `EventDataCollectorStudy.cpp:788`.
3. **If Finding 1's hypothesis does NOT hold** (the trace reveals a different mechanism): this
   spec's Unit A needs to be re-scoped once the real cause is known — do not force-fit the accessor
   design above onto a different root cause.

### Test Plan

1. C++ native test (`tests/cpp/test_context_manager.cpp` or nearest equivalent): confirms
   `LogSynchronizedEvent` now receives a non-null, populated `RiskGateContextT` under the same
   conditions `EmitTrainingContext` already populates one.
2. Real-replay verification: re-run a short `EventDataCollector` replay with the fix; re-run this
   session's own population-rate check (67.9%/32.1% split) against the fresh output; confirm it's at
   or near 100%, not just improved.
3. Full native suite green; `build_dll.sh` clean.

### Acceptance Criteria

1. A fresh `.context` collection shows `risk_gate_context` present on effectively 100% of records
   (allowing only for a brief startup-warmup window before `m_localRiskContext.isValid` first
   becomes true, same as any other warmup-gated signal in this system).
2. lbrnet's companion spec's pass-through shim becomes provably dead code on fresh data — flag this
   explicitly to whoever picks up that spec's cleanup once this ships.

### Rollback

Contained to `LBRFileManager`/`ContextManager`/`EventDataCollectorStudy` — revert together if a
regression surfaces; no schema change in this unit (the field already exists).

---

## Unit B: Audit the live gate stack for the same drifted-threshold / non-stationary-comparison pattern

### Problem

`amihud_gate_percentile_spec.md` found and fixed exactly one instance of "raw, non-stationary
microstructure value compared against a fixed constant" (Amihud/PC-17). Finding 2's
`taleb_signal_sigma_threshold` three-way drift suggests this may not be the only instance — it just
hasn't been audited the same way yet.

### Design

1. Re-run `amihud_gate_percentile_spec.md`'s own methodology (§1 "What Gemini's ruling established")
   against every remaining fixed-threshold gate in `RiskManager::EvaluateHardGates()` and
   `ExecutionGate::EvaluateEmpiricalRegimeGates()`: for each, confirm whether the underlying raw
   signal is genuinely stationary (a fixed threshold is fine) or not (needs the same
   rolling-percentile-on-the-wire treatment Amihud got).
2. For `taleb_signal_sigma_threshold` specifically: determine which of the three values
   (`1.8401`/`9.636797`/`6.67559...`) is actually current/correct, fix whichever place(s) are stale,
   and — per Unit C below — make this impossible to silently repeat.
3. Any gate this audit finds needing a wire-level fix follows the exact same shape Amihud did: fix
   the raw formula if needed, compute the stabilizing transform (percentile, robust z, whatever fits
   that signal's actual distribution) once in C++, ship both raw and transformed values on
   `RiskGateContext`, hand off to lbrnet via a spec matching this session's companion spec's shape.

### Test Plan

Native tests per any gate this audit finds needs a fix, following the same fixture/rail-rate
methodology `test_feature_scaler.cpp`'s existing dim-audit tests already use.

### Acceptance Criteria

A written finding (even if "audited, no other gate needs this treatment") for every fixed-threshold
gate in both `EvaluateHardGates()` and `EvaluateEmpiricalRegimeGates()`, plus a resolved,
single-sourced value for `taleb_signal_sigma_threshold`.

### Rollback

Per-gate, same as any native `FeatureScaler`/`RiskManager` change in this codebase's established
pattern (fixture-verified before commit).

---

## Unit C: Shared calibration config — git-tracked, versioned, single-writer-per-section

### Problem

See Audit Findings 2-4. No git history for either live config file; no shared, versioned home for
FeatureScaler's hardcoded bounds; inconsistent versioning schemes between the two existing files.

### Design

1. **New `config/` folder at the MindfulTrader repo root**, git-tracked, containing
   `execution_params.json` and `hmm_regime_risk_policy.json` — this becomes the source of truth;
   `/mnt/c/Trading/config/` becomes a deployment target, not a source.
2. **Unify versioning**: both files adopt `"1.0.2"`-style semver (replacing
   `hmm_regime_risk_policy.json`'s current `"2026-06-11.v3"` date+patch scheme). Bump the minor/patch
   per the existing informal convention (`_updated`/`generated_at_utc` fields already present, kept).
3. **New section in `execution_params.json`** for the FeatureScaler per-dim winsorization bounds
   (`STATE_WINSOR_SIGMA`, `DIM_WINSOR_SIGMA_OVERRIDE[16]`, `LOGZ_WINSOR_SIGMA_OVERRIDE[16]`,
   `SHRINKAGE_SCALE_MIN[16]`) — each entry carries its own provenance (matching this file's own
   existing `_taleb_kurtosis_note` convention): derivation method (GPD fit / exact-formula replica /
   etc.), sample size, date, and the dim name it applies to. `FeatureScaler.h` reads this section at
   startup instead of using `static constexpr` values — a genuine behavior change (recompile no
   longer required to recalibrate a bound), test accordingly.
4. **Sectioned ownership within each file** — every top-level or nested section gets a small
   `_owner`/`_generated_by` string (e.g. `"lbrnet/calibrate_hmm_gate_thresholds.py"`,
   `"manual/MindfulTrader"`) so ownership is self-documenting rather than tribal knowledge. Every
   writer (C++ or Python) does strict read-modify-write on only its own section(s) — never a
   full-file replace. `promote_to_cpp_config.py` already behaves this way; audit it once more under
   this explicit convention but do not otherwise change its behavior (see Non-Goals).
5. **Build the missing sync for `empirical_gate_thresholds`**: a script (Python, run from lbrnet
   after `calibrate_hmm_gate_thresholds.py` produces a fresh `models/HMMEmpiricalGateThresholds.json`)
   that does read-modify-write of exactly that section into `MindfulTrader/config/hmm_regime_risk_policy.json`
   — closing the exact gap that caused Finding 2's drift.
6. **New sync script** (`MindfulTrader/scripts/` or similar) that pushes
   `MindfulTrader/config/*.json` → `/mnt/c/Trading/config/*.json`, atomic write + timestamped backup,
   matching `promote_to_cpp_config.py`'s existing pattern (reuse its `_write_json_atomic`/
   `_backup_file` helpers rather than reimplementing).
7. **No CI gate, no forced-acknowledgment block** — mismatches (e.g. a config missing an expected
   section, or a version the loader doesn't recognize) get a loud log line on load, matching this
   codebase's established "loudly logged, never silent" convention (`lbrnet`'s parquet-cache
   auto-rebuild is the precedent) — never a blocked build or a required approval step.

### Test Plan

1. C++ test: `FeatureScaler` loads bounds from the new config section correctly; falls back to
   documented defaults with a loud log line if the section is missing (never silently uses a stale
   in-memory default without saying so).
2. Python test: the new sync script does read-modify-write correctly — writing a fresh
   `empirical_gate_thresholds` section leaves every other key in `hmm_regime_risk_policy.json`
   byte-identical.
3. Real-run verification: `build_dll.sh` clean after `FeatureScaler.h`'s hardcoded-constant removal;
   confirm a dim's bound can be changed via the config file alone, no recompile, and takes effect.

### Acceptance Criteria

1. Both live config files exist, git-tracked, in `MindfulTrader/config/`, both `"1.0.2"`-style
   versioned.
2. `FeatureScaler.h` has zero remaining hardcoded winsorization bound constants — all sourced from
   config.
3. A script exists that keeps `empirical_gate_thresholds` in sync with lbrnet's own calibration
   output — Finding 2's drift becomes structurally prevented, not just fixed once.
4. Every section in both files carries an explicit `_owner`/`_generated_by` marker.

### Rollback

`config/` folder + sync scripts are new, additive files — safe to remove independently.
`FeatureScaler.h`'s hardcoded-to-config-driven migration is the one unit here with real production
risk (a live system reading a different bound at runtime) — revert to hardcoded constants as a single
commit if this misbehaves.

---

## Non-Goals

- **Removing the lbrnet companion spec's pass-through shim** — that's an lbrnet-side cleanup,
  triggered by this spec's Unit A shipping, but executed in a future lbrnet-rooted session, not here.
- **Rewriting `promote_to_cpp_config.py`** — stays exactly as-is (narrower-scope decision, made
  explicitly during brainstorming): it keeps writing its own specific keys
  (`transition_floor`/`live_toxic_score_threshold`/etc.) directly to the live path; Unit C's new
  sync script is a second, additive write path for the git-tracked source, not a replacement.
- **Any CI gate, schema-drift-blocks-the-build mechanism, or approval workflow** — explicitly
  rejected during brainstorming as inappropriate bureaucracy for a solo developer; structural
  correctness (versioning, provenance, ownership markers) is in scope, procedural gates are not.
- **Physically splitting `hmm_regime_risk_policy.json` into multiple files** — considered and
  explicitly rejected in favor of sectioned single-file ownership; do not revisit without a new
  reason not already weighed here.
- **A companion "was this value rail-clamped" wire field** on `ObservationData`/`RiskGateContext` —
  considered and rejected: once Unit C's shared config guarantees both languages agree on the exact
  current bound, Python can detect a rail-hit reliably via exact-value comparison against the
  now-trustworthy shared bound, with no new wire field needed.

## Investigation Log

- **2026-08-15**: Spec drafted during `superpowers:brainstorming` from an lbrnet-rooted session,
  following extensive Q&A on scope (Python full-cycle now vs. C++ spec-only), the population-gap
  discovery, the rail-hit-visibility question (resolved via shared config, not a new wire field), the
  file-count/ownership question (resolved: sectioned single file, not a split), and the git-tracking
  proposal (resolved: new `config/` folder + sync script, `promote_to_cpp_config.py` untouched).
  Unit A's exact root-cause mechanism is a documented hypothesis, not yet confirmed by a live
  trace — explicitly flagged as the first real step for whoever implements this, not something to
  build against blindly.
