# Handoff: Activity-Clock Dual-Kurtosis lbrnet Work

Written from MindfulTrader, 2026-08-26, **corrected 2026-08-27 after a real factual error was found
and fixed** — see the correction note at the top of §1 before reading anything else. The C++
producer-side work from `docs/superpowers/plans/2026-08-26-activity-clock-dual-kurtosis.md` is
implemented (code-complete, native-tested, full-build-verified) but **not yet committed** in either
`MindfulTrader` or `schema`. The following work belongs in a separate `lbrnet`-rooted session and
has not been executed here — do not start it from a `MindfulTrader` session; this doc exists so that
session knows exactly what to do without re-deriving it.

Cross-project tracking: `PRODUCTION_TRIAGE.md` row 1 (this thread) and row 14 (`ObservationData`
schema evolution policy, which this work is now the concrete precedent for) — both point back to
this file. Update both if anything here changes materially.

## 1. Consume `ObservationData`'s new 17th field — CORRECTED 2026-08-27

**An earlier version of this section said to add `fast_taleb_kurtosis` to `HMM_OBSERVATION_
EXTENSIONS` and explicitly NOT inside `ObservationData`. That was wrong** — verified directly
against `mts_schema.fbs` and all 3 repos' generated bindings: `fast_taleb_kurtosis` was actually
added as **`ObservationData`'s 17th field directly**, inserted between `liq_fragility` and
`recurrence_rate` (16D→17D, struct size 64→68 bytes). `HMM_OBSERVATION_EXTENSIONS` was never
touched — it still only holds `nh_nl_daily`/`daily_bias`, unrelated to this field.

**Consequence you need to know before touching anything**: because this is a field **inserted in
the middle of a fixed-layout `struct`**, not appended at the end and not an additive `table` field,
every byte offset after `liq_fragility` shifted by 4 bytes — `recurrence_rate`, `fractal_dim`, and
`mean_rev_z` all moved. **Any already-recorded historical `.context`/`.alpha` binary file (the old
16-field, 64-byte layout) is binary-incompatible with code generated against the new 17-field,
68-byte layout** — reading old files with new bindings will misread `recurrence_rate`'s old bytes as
`fast_taleb_kurtosis` and shift everything after it, not just leave the new field zeroed. This is a
harder compatibility break than a simple additive schema change; treat it accordingly (see §3).

Practically, what's needed on `lbrnet`'s side:
- The generated Python binding (`lbrnet/generated/MTS/Schema/ObservationData.py`/`.pyi`) already
  reflects the 17-field struct as of 2026-08-27 — that part is a mechanical regen byproduct, already
  done (uncommitted). Don't regenerate it again; verify it matches `mts_schema.fbs` at whatever
  commit you're working from.
- `OBSERVATION_FIELDS`/`OBSERVATION_DIM` in `schema/regenerate_schema.sh` are auto-derived from the
  struct's real fields (`ObservationDataT().__dict__.keys()`), so they already reflect 17 wherever
  regenerated — no hand-maintained list to update there.
- What's **genuinely not done**: any `lbrnet` *hand-written* code that decides which dims the model
  actually trains/infers on. Concretely: `hmm_utils.py`'s `HMM_KEEP_DIMS` (or wherever the K=4
  model's training-input dimension list lives) needs an explicit decision on whether dim 17
  (`fast_taleb_kurtosis`) joins the kept set — given the entire point of this initiative was getting
  Taleb kurtosis onto an activity clock into the HMM's observation vector, the default expectation
  is yes, but this hasn't been decided or implemented, only made possible.

## 2. HMM consumer audit

Inspect `live_agent.py`, `regime_view.py`, `observation_vector_bulk_reader.py`, and any other HMM
consumer for a **hardcoded `16`** (dimension count, byte offset, or field index) rather than a
schema-derived constant. The 2026-08-25 ADR (`schema/docs/ADR/2026-08-25-observation-vector-
loading-efficiency-and-dropped-hmm-fields.md`) already flagged `observation_vector_bulk_reader.py`'s
`np.frombuffer(..., count=OBSERVATION_DIM, ...)` pattern as schema-derived and therefore safe by
construction — confirm that's still true, and that nothing else took a shortcut and hardcoded `16`
or `64` (bytes) directly. Also verify train/live field ordering matches — the field was inserted
mid-struct, not appended, so an index-based accessor written before this change could now silently
read the wrong field if it wasn't re-derived from the schema.

## 3. Historical backfill and parity — upgraded from "needed" to "structurally required"

Because of §1's mid-struct insertion, this isn't just "the new dimension has no historical values
yet" (which would be true of an additive field too) — **every historical `.context`/`.alpha` file
recorded before this change ships is binary-incompatible with the new struct layout**, not simply
missing one column. Decide explicitly, don't default silently:
- **Regenerate from raw `.scid` ticks** (replay the C++ activity-clock logic, or reimplement the
  same imbalance-bar + Moors-kurtosis logic in Python against the same historical tick data
  `lbrnet` already has access to) — the clean option, but costs real wall-clock time, same class of
  decision as the `lempel_ziv` real-fix deferral (`PRODUCTION_TRIAGE.md` row 3).
- **A sidecar/offset-migration tool** that rewrites old 64-byte records into the new 68-byte layout
  by inserting a placeholder (e.g. the Moors-neutral `1.23f` sentinel `ContextManager.cpp` itself
  uses for insufficient-data cases) at the right offset — cheaper, but only valid if nothing
  downstream depends on `fast_taleb_kurtosis` actually being real for old rows, and needs its own
  correctness check before trusting it (same discipline as the existing `lempel_ziv` sidecar
  correction this project already has precedent for).
- **Discard old `.context`/`.alpha` files and only use post-change collection** — simplest, valid
  given this system is pre-production (`PRODUCTION_TRIAGE.md`'s standing rule: no live consumer to
  protect yet), but shrinks the training set by however much history gets dropped.

Whichever path is chosen, the two implementations (C++ live, whichever backfill approach) must
produce matching output on the same historical window before this dimension is trusted for
training — same twin-parity discipline as every other C++/Python surface in this system.

## 4. EWMA threshold calibration

The C++ engine currently has a non-final `50.0f` imbalance threshold placeholder
(`ImbalanceBarEngine.h`). Derive the activity-bar threshold from real historical ES tick data using
AFML-style EWMA calibration, including expected bar duration and absolute imbalance magnitude. Feed
the resulting value back into `MindfulTrader`'s runtime configuration (`config/execution_params.json`
per this project's existing config-sharing convention); do not treat the placeholder as production
calibration.

## 5. Empirical discrimination test

Backtest HMM cross-state discrimination with and without the activity-clock kurtosis dimension. The
design does not assume the new signal improves state separation until this comparison is measured
on out-of-sample data.

## 6. Gate-authority question

Compare (a) the current calibrated slow-clock gates plus the shipped additive fast early trigger
against (b) gates fully recalibrated on the activity-clock statistic. Decide authority from realized
drawdown and alpha outcomes, not from implementation effort.

## 7. Separate emission question

The skewed Student-t emission question from the activity-clock spec remains open and separate.
Evaluate it independently; do not conflate it with the observation extension or gate work.

## 8. Likely companion: `skewness_idx`'s activity-clock twin

`MindfulTrader`'s next observation-vector step (spec §5c, not yet started, handed to a sibling
Claude Sonnet 5 instance 2026-08-27) is `skewness_idx`'s activity-clock twin, expected to land the
same way as this field did — directly inside `ObservationData` (an 18th field), not via `Event`/
`HMM_OBSERVATION_EXTENSIONS`. If it ships before this handoff is picked up, everything in §1-§3
above applies to it too (same mid-struct-insertion compatibility question, same `HMM_KEEP_DIMS`
decision, same historical-backfill choice) — check `PRODUCTION_TRIAGE.md` row 1 for its actual
landed state before assuming it's still pending.
