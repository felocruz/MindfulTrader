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

## 8. `skewness_idx` — DONE 2026-08-27 (`MindfulTrader` `7c51f33`), by REPLACEMENT not addition —
different consequence for `lbrnet` than §1-§3, read this even though it's not a new field

**This section's earlier prediction (that `skewness_idx`'s twin would land as an 18th
`ObservationData` field) was wrong — corrected below with what actually happened, verified directly
against the commit.** `skewness_idx` (dim 10) was **replaced in place**, not added alongside:
`ObservationData` stays at 17 fields, no schema change, no `mts_schema.fbs` diff, no new consumer
regen needed. The reasoning (kurtosis went additive/dual-clock specifically to protect 5 existing
live gate consumers calibrated on its slow value; `skewness_idx` has zero such consumers and was
already an HMM drop candidate for suspected staleness — replacement tests that hypothesis directly)
is in `PRODUCTION_TRIAGE.md` row 1's 2026-08-27 update and `MindfulTrader/SCRATCHPAD.md`'s Thread C.

**What this means for `lbrnet` instead — a values/semantics discontinuity, not a binary-layout
one**: dim 10's byte offset and meaning-as-a-schema-field are unchanged, but its **computed values
changed source** at commit `7c51f33` — before that commit, dim 10 was `CalculateSkewness()`'s stale
TS3 time-bar value (updated once per 15-min bar close); after it, dim 10 is `BowleySkewness()` over
`ActivityClockManager`'s imbalance-bar returns (updates far more frequently, per the same
cadence-starvation problem this whole initiative exists to fix). **Any historical `.context`/
`.alpha` data spanning that commit boundary has two different signals under one column name** —
treat pre-/post-`7c51f33` `skewness_idx` data as non-homogeneous for training purposes, the same
twin-parity discipline §3 already asks for on `fast_taleb_kurtosis`, just for a values reason
instead of a byte-layout one. Get the exact collection timestamp of `7c51f33`'s deployment (not the
commit timestamp — whenever the built DLL actually started running live/collecting) before mixing
data across it.

**A second, independently-found and higher-priority data-quality issue from the same commit**:
`MindfulTrader/include/FeatureScaler.h`'s four per-dim calibration arrays (winsorization bounds,
shrinkage scale, rolling-window size) were never updated when `fast_taleb_kurtosis` landed at real
index 13 in the earlier commit (`ff22e48`) — each array still had only 16 entries, silently
misassigning `recurrence_rate`/`fractal_dim`/`mean_rev_z`'s calibration by one index for the entire
window between `ff22e48` and `7c51f33`. `mean_rev_z`'s rolling window specifically defaulted to `0`
in that window, making its buffer pop immediately after every push — **permanently degenerate**,
not just miscalibrated. **Any `.context`/`.alpha` data collected between `ff22e48` and `7c51f33`
has unreliable `recurrence_rate`/`fractal_dim`/`mean_rev_z` values and a broken `mean_rev_z` — check
collection timestamps against both commits before trusting that window's data for those 3 dims,
independent of anything else in this handoff.** Full detail: `7c51f33`'s own commit message.

**Also still open, flagged by `FeatureScaler.h`'s own inline comment (`DIM_WINSOR_SIGMA_OVERRIDE`
index 10), not yet acted on**: `skewness_idx`'s winsorization bound was calibrated against the OLD
time-bar cadence and is marked "NEEDS RE-AUDIT 2026-08-27 — source cadence changed" — the activity-
clock version's distributional properties (tail behavior, clip rate) haven't been re-checked against
the bound chosen for the old, slower-updating signal. This is a `MindfulTrader`-side follow-up, not
`lbrnet`'s to fix, but worth knowing before treating dim 10's post-`7c51f33` values as fully
calibrated.

Window-widening `recurrence_rate`/`fractal_dim`/`mean_rev_z` (§5 of the companion 2026-08-25
hardening spec) remains **not started** as of this writing — check `PRODUCTION_TRIAGE.md` row 1
before assuming otherwise.
