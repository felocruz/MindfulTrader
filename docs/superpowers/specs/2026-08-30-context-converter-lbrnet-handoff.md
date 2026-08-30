# Handoff: C++/Arrow `.context` Converter — lbrnet Integration Work

Written from `MindfulTrader`, 2026-08-30. The C++ side
(`docs/superpowers/plans/2026-08-30-context-to-parquet-cpp-arrow-converter.md`, all 13 tasks) is
implemented, natively tested (54 checks across 4 suites), end-to-end verified against synthetic
fixtures, and **committed** (`MindfulTrader` `689465a`..`54d1702`; `schema` `477b750`/`c7788f8` —
neither repo has a remote configured, nothing pushed). **The `lbrnet`-side integration itself has not
been started — do not begin it from a `MindfulTrader` session; this doc exists so that session knows
exactly what to do without re-deriving it.**

**Ownership boundary now in effect (§10.1 of the brainstorm doc)**: C++ owns *all* raw `.context`
byte parsing, analysis, and conversion, permanently — not just for this initiative. Any future
`lbrnet` need to read `.context` bytes directly should route through these tools or extend
`tools/context_reader.h`, not grow a new Python reader.

Cross-reference: `docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md`
§10 (full design rationale — read this first for *why*, this doc is the *what to do*).

## 1. What shipped, in one table

| New file (`MindfulTrader`) | Replaces (`lbrnet`) | Status |
|---|---|---|
| `tools/context_reader.h` | `lbrnet/lbrnet/data/observation_vector_bulk_reader.py` | Done, 25 native tests pass |
| `tools/context_to_parquet.cpp` (binary: `tools/context_to_parquet`) | `lbrnet/lbrnet/scripts/materialize_context_parquet.py` | Done, e2e verified |
| `tools/context_validate_stats.h` + `tools/context_validate.cpp` (binary: `tools/context_validate`) | `lbrnet/scripts/validate_lbr_file.py`'s `.context`-specific path + `lbrnet/scripts/context_preflight.py` (in full) | Done, e2e verified |
| `tools/context_cache_key.h` | `materialize_context_parquet.py`'s `cache_key()`/`is_cache_fresh()` | Done, wired into `context_to_parquet`'s `--meta-path` |
| `schema/scripts/generate_contract_header.py` | (fixes `regenerate_schema.sh`'s hand-maintained-duplicate defect) | Done, no `lbrnet` action needed |

Also done, `lbrnet`-side, already committed by this session directly (small, explicitly authorized —
see git log for the exact diff, not re-derived here): `lbrnet/scripts/validate_lbr_file.py`'s dead
`check_context_file_quality()` function deleted (zero callers repo-wide, defunct pre-MO/SS 10-float
legacy format). **This is the only `lbrnet` file this session touched — everything else below is
still to do.**

## 2. Build the tools

Both binaries are standalone, native (Linux/WSL, not the Windows DLL cross-compile), built via the
`mts` mamba environment:

```bash
cd MindfulTrader
mamba run -n mts g++ -O2 -std=c++17 -Iinclude tools/context_validate.cpp -o tools/context_validate

mamba run -n mts g++ -O2 -std=c++17 -Iinclude \
  $(mamba run -n mts pkg-config --cflags arrow parquet) \
  tools/context_to_parquet.cpp \
  $(mamba run -n mts pkg-config --libs arrow parquet) \
  -o tools/context_to_parquet
```

Neither binary is committed (git-ignored by extension, matching `tools/amihud_liqfragility_recalibration`'s
existing precedent of staying build-from-source) — `lbrnet`'s wrapper code should build-if-missing or
document the build step, not assume the binary exists.

## 3. CLI mapping — `context_to_parquet` replaces `materialize_context_parquet.py`

| Python (`materialize_context_parquet.py` / `train_student_t_hmm.py` call sites) | C++ (`context_to_parquet`) |
|---|---|
| `materialize_context_parquet(context_path)` (unbounded, auto incremental-vs-full via `is_cache_fresh()`) | `--input PATH --output PATH --mode unbounded --meta-path PATH.meta.json` |
| (no bounded-head equivalent in the Python *converter* — bounded reads there go through `observation_vector_bulk_reader.read_context_observations`, not this file) | `--mode head --max-samples N` |
| (tail sampling, same note) | `--mode tail --max-samples N` |
| `cache_key()`'s `resume_offset`/`last_seq_id` (internal bookkeeping) | `--resume-offset N --resume-last-seq-id N` (only needed if **not** using `--meta-path`'s automatic freshness decision) |

**Important semantic difference, decide explicitly before wiring**: Python's `is_cache_fresh()`
compares `context_size`/`context_mtime`/`observation_dim`/`schema_version` recorded in a sidecar
`<context_path>.meta.json`. The C++ tool's `--meta-path` mechanism (`tools/context_cache_key.h`) uses
a **different JSON schema** (`wire_schema_version`+`parquet_cache_format_version` instead of Python's
single conflated `schema_version`+`observation_dim` — see the brainstorm doc §10.6's own callout on
why these got split). If you point `--meta-path` at an *existing* Python-written `.meta.json`,
`ReadCacheKey()` will find it unparseable (missing the new field names) and safely fall back to
`RebuildPlan::kFull` — not a crash, but also not a real "was this file already converted" check
against old state. **Decide**: point new collections at a fresh `.meta.json` path from day one
(recommended, avoids any ambiguity), or write a tiny migration that translates an old Python
`.meta.json` into the new schema before first use. Not decided here.

## 4. CLI mapping — `context_validate` replaces `validate_lbr_file.py` (`.context` path) + `context_preflight.py`

| Python flag | Source script | C++ flag |
|---|---|---|
| `--sample-tail-pairs N` / default head sampling | `validate_lbr_file.py` | `--mode tail --max-pairs N` / `--mode head --max-pairs N` |
| `--verify-features-vector` | `validate_lbr_file.py` | `--check-sequence-integrity` |
| `--check-nulls` | `validate_lbr_file.py` | `--check-nulls` (same name) |
| `--check-zero-trap` | `validate_lbr_file.py` | `--check-zero-trap` (same name, same 0.98 threshold) |
| `--expected-schema-version N` | `validate_lbr_file.py` | **Not needed** — `context_reader.h`'s `OpenContextFile()` already hard-refuses any `schema_version` mismatch unconditionally, before this tool even starts reading records. There is no "soft check" mode anymore; a mismatch is always fatal. |
| `--max-pairs` / `--min-pairs` | `context_preflight.py` | same names, same defaults (50000 / 5000) |
| `--constant-ratio-threshold` / `--saturation-threshold` / `--chronic-zero-threshold` | `context_preflight.py` | same names, same defaults (0.995 / 0.98 / 0.15) |
| (hardcoded `> 0.8` in `inter_feature_corr()`) | `context_preflight.py` | `--corr-threshold` (now configurable, default 0.8) |
| `--report-json PATH` | `context_preflight.py` | same name |
| `--strict` / `--no-strict` | both | `--no-strict` only (strict is the C++ tool's default, matching both scripts' own default) |

**Not ported, stays in Python, unchanged**: `validate_lbr_file.py`'s `.alpha`-specific functions
(`read_lbr_events`, `check_alpha_file_quality`, `check_alpha_provenance_contract`,
`check_alpha_fsm_parity_contract`, `check_alpha_delta_utility_gate`) and their CLI flags
(`--check-provenance`, `--check-fsm-parity`, `--check-utility-gate`, `--require-pnlticks-*`). These
operate on a different wire format (`TrainingEvent`/`IndicatorState`) and/or already-converted
Parquet sidecars via Polars — genuinely Python's domain, not just unported.

## 5. Column schema — BREAKING CHANGE, read before touching any consumer

The new tool's Parquet output is **not** column-identical to `materialize_context_parquet.py`'s. This
was a deliberate decision (brainstorm doc §10.4), not an oversight:

- **`ObservationData`'s 19 columns**: unchanged names (`log_variance_ratio`, ..., `fast_mean_rev_z`),
  no `risk_gate_` involvement — identical to before.
- **`RiskGateContext`'s 14 float columns**: **naming scheme changed**. Python prefixed *all 14* with
  `risk_gate_` uniformly. The new tool prefixes **none** of them — instead, only the 5 that
  genuinely collide with an `ObservationData` column name (`hurst_exponent`, `fisher_info`,
  `amihud_illiquidity`, `mean_rev_z`, `fractal_dim`) get a `_raw` suffix (e.g.
  `hurst_exponent_raw`); the other 9 (`shannon_flow_entropy`, `shannon_efficiency`,
  `taleb_kurtosis`, `taleb_skewness`, `elder_chandelier_atr`, `pareto_tail_alpha`, `spread_stress`,
  `raschke_burst`, `amihud_percentile`) are now **bare, unprefixed column names**. Confirmed via a
  real e2e Parquet read, not assumed — see `tools/context_to_parquet.cpp`'s `BuildArrowSchema()`.
- **`RiskGateContext`'s 4 non-float columns** (`regime_duration`, `is_valid`,
  `snapshot_timestamp_us`, `risk_gate_context_available`): unchanged, keep the `risk_gate_` prefix
  (no naming-collision question applies to these — no `ObservationData` counterpart exists).
- **Everything else** (`sequence_id`, `timestamp_us`, `bars_since_last_update`): unchanged.

**Full old→new column rename table** (nothing else changes):

| Old (Python) | New (C++) |
|---|---|
| `risk_gate_shannon_flow_entropy` | `shannon_flow_entropy` |
| `risk_gate_shannon_efficiency` | `shannon_efficiency` |
| `risk_gate_taleb_kurtosis` | `taleb_kurtosis` |
| `risk_gate_taleb_skewness` | `taleb_skewness` |
| `risk_gate_elder_chandelier_atr` | `elder_chandelier_atr` |
| `risk_gate_pareto_tail_alpha` | `pareto_tail_alpha` |
| `risk_gate_amihud_illiquidity` | `amihud_illiquidity_raw` |
| `risk_gate_spread_stress` | `spread_stress` |
| `risk_gate_hurst_exponent` | `hurst_exponent_raw` |
| `risk_gate_fractal_dim` | `fractal_dim_raw` |
| `risk_gate_mean_rev_z` | `mean_rev_z_raw` |
| `risk_gate_raschke_burst` | `raschke_burst` |
| `risk_gate_fisher_info` | `fisher_info_raw` |
| `risk_gate_amihud_percentile` | `amihud_percentile` |

**Any `lbrnet` code that reads a `.context.parquet` file by these old `risk_gate_*` column names
needs updating** the moment it starts consuming C++-produced Parquet output — grep for
`risk_gate_` across `lbrnet/` before wiring this in, don't assume `train_student_t_hmm.py` is the
only reader (`_load_bounded_from_parquet_cache`, any ad-hoc analysis notebook/script, and
`amihud_gate_percentile_spec.md`-adjacent tooling are all real candidates, not verified here).

## 6. Freshness cache — what actually happens to the existing stale file

`lbrnet/data/raw/event_data.context` (14.8GB, `schema_version`-stamped `230` in its wire bytes,
i.e. pre-`fast_hurst_exponent`/`fast_taleb_kurtosis`/`fast_mean_rev_z`, genuinely 16-field
`ObservationData`) will be **hard-refused** by `OpenContextFile()` the instant any of these tools
touch it — `230 != 240`. This is the exact corruption risk this whole initiative exists to close (see
brainstorm doc §10, "The concrete problem that motivated this"). **Explicitly not handled here,
needs an `lbrnet`-rooted decision**:
- Re-collect fresh 19-field data from Sierra Chart (clean, but costs real collection wall-clock
  time), or
- Write a dedicated one-off migration tool that knows the *old* 16-field byte layout and re-encodes
  historical records into new-layout `.context` files (real engineering effort, only worth it if the
  historical span in that file has irreplaceable value).

Neither option is started. Do not silently pick one without flagging the choice back to the user —
this is exactly the kind of decision the brainstorm doc's §10.2 named as "explicit, separate, not
handled by this tool."

## 7. Concrete `lbrnet` call sites to update

- `lbrnet/lbrnet/scripts/train_student_t_hmm.py`: `_load_unbounded_mo_ss_context()`,
  `_load_bounded_mo_ss_context()`, `_load_bounded_from_parquet_cache()` — all three currently call
  into `materialize_context_parquet`/`observation_vector_bulk_reader` directly; need to shell out to
  (or subprocess-wrap) `tools/context_to_parquet`/`context_validate` instead, or reimplement the
  equivalent orchestration in Python around the new binaries.
- `lbrnet/lbrnet/scripts/materialize_context_parquet.py`,
  `lbrnet/lbrnet/data/observation_vector_bulk_reader.py`: retire once the call sites above no longer
  import them — confirm zero remaining importers before deleting (this session's own standing
  discipline: verified-unused code is deleted, not left as a parallel unused path).
- `lbrnet/scripts/validate_lbr_file.py`: retire `validate_context_mo_ss_stream()` and the
  `.context`-branch CLI flags once callers point at `context_validate` instead. The `.alpha` path
  stays untouched (§4 above).
- `lbrnet/scripts/context_preflight.py`: retire in full (fully superseded) once callers point at
  `context_validate --check-...` flags instead.
- **Not affected, confirmed safe, no action needed**: `lbrnet/data/hmm_training_cache.py` (§10.5 of
  the brainstorm doc — column-name-based, self-invalidating on the Parquet file's own `stat()`,
  already checked this session).

## 8. What this doc does not decide

- Whether `lbrnet`'s wrapper code invokes these binaries via `subprocess`, or whether a thin
  `ctypes`/pybind-style binding is worth building later for tighter integration. Default assumption:
  `subprocess`, matching how `lbrnet` already shells out to other external tools in this codebase —
  not verified against a specific precedent here, worth checking before assuming.
- The exact `.meta.json` sidecar path convention `lbrnet` should standardize on for new collections
  (§3's open question).
- The `event_data.context` migration decision (§6).

## 9. Cross-references

- `docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md` §10 — full design
  rationale, read first.
- `docs/superpowers/plans/2026-08-30-context-to-parquet-cpp-arrow-converter.md` — the implementation
  plan, all 13 tasks, task-level detail on every function/struct named above.
- `PRODUCTION_TRIAGE.md` row 1 — cross-project status tracker; update it once `lbrnet`-side work here
  is scoped or started.
