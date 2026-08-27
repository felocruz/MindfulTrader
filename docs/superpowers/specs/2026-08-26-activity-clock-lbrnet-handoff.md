# Handoff: Activity-Clock Dual-Kurtosis lbrnet Work

Written from MindfulTrader, 2026-08-26, **corrected 2026-08-27 after a real factual error was found
and fixed** — see the correction note at the top of §1 before reading anything else. The C++
producer-side work from `docs/superpowers/plans/2026-08-26-activity-clock-dual-kurtosis.md` is
implemented, native-tested, full-build-verified, and **committed** (`MindfulTrader` `ff22e48`,
`schema` `ea8058b`; neither pushed — no remote configured). The following work belongs in a separate
`lbrnet`-rooted session and has not been executed here — do not start it from a `MindfulTrader`
session; this doc exists so that session knows exactly what to do without re-deriving it.

**§9 is new, 2026-08-27, and matters more than its position at the bottom suggests**: two more dims
(`hurst_exponent`, `mean_rev_z`) are now literature-grounded for the same activity-clock treatment,
and one of them (`hurst_exponent`) is this system's single worst HMM cross-state discriminator —
read §9 if you care about the HMM's own soundness, not just schema mechanics.

**§10 is also new, 2026-08-27 — a staleness audit of `lbrnet`'s own existing dimensionality-work
docs against everything above, run by a research agent at the user's explicit request ("identify
existing 16D vector related specs that are now incompatible... or plain inconsistent/incorrect").
Read it before touching ANY of the four documents it names** — two of them make claims that are now
factually contradicted by the schema-side changes above, one contains a pre-existing internal
inconsistency unrelated to this thread, and the production model itself is confirmed still running
on the pre-change contract.

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

Window-widening `recurrence_rate`/`fractal_dim` (§5 of the companion 2026-08-25 hardening spec)
remains **not started** as of this writing — check `PRODUCTION_TRIAGE.md` row 1 before assuming
otherwise. `mean_rev_z` is no longer part of that window-widening task — see §9 below.

## 9. `hurst_exponent` and `mean_rev_z` are now literature-grounded for activity-clock treatment too
— NOT yet implemented, but `lbrnet` should know this is coming, and why it matters for the HMM itself

**Added 2026-08-27, on explicit user instruction** ("make the lbrnet side... aware of our direction
as well") after a real literature pass — not yet implemented in C++, so nothing to consume yet, but
the direction is now decided and `lbrnet` will eventually need to do for these two dims what §1-§3
above describe for `fast_taleb_kurtosis`. Full detail and citations:
`docs/superpowers/specs/2026-08-25-observation-vector-institutional-hardening-spec.md` §5a and
`docs/superpowers/specs/2026-08-26-activity-clock-tail-risk-and-decay-spec.md` §6.

**The literature**: Clark (1973, *Econometrica*) → Ané & Geman (2000, *Journal of Finance*) → López
de Prado's AFML ch. 2 — the same lineage already grounding kurtosis and `skewness_idx` — extends
directly to two more dims on their own separate merits, not by analogy:
- **`mean_rev_z`** is built on lag-1 return autocorrelation. Microstructure literature (non-
  synchronous trading, bid-ask bounce) independently establishes calendar-time sampling as *a source
  of spurious serial correlation* — the exact statistic `mean_rev_z` measures.
- **`hurst_exponent`**: direct literature support that trading time (cumulative trades executed) is
  the more natural timescale for long-memory/Hurst estimation, reducing bias from regular
  calendar-time sampling.

**Why this should matter to whoever is training the HMM, not just to MindfulTrader's schema**:
`hurst_exponent` is **this system's single worst cross-state discriminator** — exactly `0.0000`,
the worst of all 16 original dims (`lbrnet/docs/superpowers/specs/2026-08-14-observation-vector-
full-institutional-coverage-spec.md` row 6 also found it carries the worst pre-shrinkage
scale-collapse artifact of any dim, 24.85% `|z|>=6`). Two independent problems may both be
contributing to that failure — a data-quality artifact already documented, and now, plausibly, a
clock-choice artifact that was never previously considered. **This is not certain** — the literature
establishes that clock choice is *a* real effect for this class of estimator, not that it explains
`hurst_exponent`'s specific failure on this system's specific data. An eventual activity-clock twin
for `hurst_exponent`, once built and shipped, is a genuine opportunity to test that empirically
(same discipline as kurtosis's own open question 13 — compare real HMM discrimination/gate outcomes
with vs. without the twin, don't assume the literature alone settles it) — flag this explicitly if
`lbrnet` is ever asked to help decide whether K=4's fat-tail-state problem (row 1's own sign-off
blocker) has any connection to this.

**Not yet actionable — nothing for `lbrnet` to do right now**: no C++ implementation exists for
either dim's twin yet (design/literature-grounding only, no plan written, no code touched). Both
would need the same treatment `fast_taleb_kurtosis` got: a `MindfulTrader`-rooted implementation
plan, live gate-consumer protection (`mean_rev_z` has one, `Scoring.cpp:305`; `hurst_exponent`'s
own consumers, `StudyHelperFunctions.cpp:623`/`TripleScreen3.cpp`, need the same check before
deciding additive-twin vs. replacement), a schema entry (per row 14's now-demonstrated direct-
struct-field policy), and only then a `lbrnet`-side consumption handoff exactly like this one.
Recorded here now so it isn't independently rediscovered later without this context, per the user's
own explicit instruction — not because there's an immediate `lbrnet`-side action item.

## 10. `lbrnet`'s own existing dimensionality-work docs — staleness audit, 2026-08-27

**Added on explicit user instruction** ("identify existing 16D vector related specs that are now
incompatible... or plain inconsistent/incorrect... we need to deal with them appropriately"), via a
research agent that read each document below in full, not just grepped for keywords. **Not fixed
from this `MindfulTrader`-rooted session** — per this project's own repo-scope convention, editing
`lbrnet`'s docs belongs to an `lbrnet`-rooted session; this section hands the findings over instead
of reaching into that repo.

**Ground truth confirmed first**: `models/ModelManifest.json` shows the currently-authoritative
production model is `K=4, feature_dim=16`, trained 2026-08-25 — i.e. **production has not seen any
of the changes above**. `hmm_utils.py`'s `HMM_KEEP_DIMS`/`HMM_MODEL_INPUT_FIELDS` are schema-name-
derived (`tuple(range(HMM_OBSERVATION_DIM))`, sourced from the schema contract's auto-derived
`OBSERVATION_FIELDS`), so they will mechanically become 17D the moment they're re-run against the
regenerated bindings — no code change needed, but also not yet re-validated against the live
17-field bindings. This "just works mechanically" design is good news for the schema-mechanics side
of the handoff, but doesn't substitute for the semantic re-evaluation the four items below need.

1. **`docs/superpowers/specs/2026-08-25-vol-convexity-removal-spec.md`** (the 16D→12D drop spec,
   **not yet implemented**) — its recommendation to drop `skewness_idx` from HMM training input
   rests on discrimination evidence measured against the *old* TS3 time-bar signal. That evidence no
   longer describes what the field currently computes (`7c51f33`'s activity-clock replacement).
   **Before this spec is acted on**: re-measure `skewness_idx`'s discrimination against the new
   signal — the old near-zero-discrimination finding may or may not still hold, and this spec's
   drop recommendation for this one field specifically should not be trusted as-is until it does.
   The spec is silent on `fast_taleb_kurtosis` entirely (didn't exist when it was written) — whoever
   revives it should decide whether the 17th dim belongs in the same evaluation pass.

2. **`docs/superpowers/specs/2026-08-23-16d-to-optimal-xd-vector-master-protocol-spec.md`** — an
   entire parallel dimensionality-reduction investigation, apparently unrelated in origin to the
   activity-clock thread. Two distinct problems, only the second caused by our work:
   - **Pre-existing, unrelated to anything here**: this spec's own text records "Sign-off decision
     (2026-08-23, approved): drop `lempel_ziv` entirely (16D→15D)" — per `OPEN_FINDINGS_REGISTER.md`
     OF-03, that decision was **reverted** after a regression test proved it broke `GAUSSIAN_FRAGILE`
     detectability; the actual final call was "keep all 16 dims, de-weight `lempel_ziv`" (bullet
     weight 1.4× → map weight 1.0×). The master-protocol document itself was never corrected to say
     so — flagging this because it was found during this audit, not because it's this thread's fault.
   - **Caused by our work**: every phase's "16D" baseline arithmetic (Phase 2 simulation, Phase 3
     K=5 sign-off table, Phase 4 tail-dependence audit, Phase 7's own instruction to "update
     `STUDENT_T_HMM_RUNBOOK.md`'s 16D contract") is now built on a count that no longer matches the
     producer. This needs a real pass, not a global find-replace of "16" — some phases may already be
     concluded (see item 5 below) and only need an annotation, not new numbers.

3. **`docs/hmm/STUDENT_T_HMM_RUNBOOK.md`'s "16D Observation Vector Contract"** (around lines
   447-478) states "Dimension count is strict: 16D only... Training rejects vectors that are not
   exactly 16D" and lists `skewness_idx` with no mention of `fast_taleb_kurtosis`. **Currently still
   operationally true** (production is on the old 16-field contract per `ModelManifest.json`), which
   is exactly why this is a *ticking* inconsistency rather than an active bug — the moment training
   is re-run against the regenerated 17-field bindings, `HMM_KEEP_DIMS` becomes 17D automatically
   (per the ground-truth note above) and this runbook section becomes silently wrong. This is the
   update the master protocol's own Phase 7 already earmarks — surface it there, don't invent a new
   task for it.

4. **`docs/superpowers/specs/2026-08-23-hmm-dimensionality-investigation-spec.md`** and its
   companion plan — completed analysis (Stages 1-4 / Tasks 1-4, per the master protocol and OF-03),
   concluding K=4 dominates and reporting real ARI/stability numbers (e.g. 13D best at 0.797, full
   16D "noticeably less reliable") for various dim-subsets computed against the 16-field vector.
   These numbers were computed against the **old** `skewness_idx` semantics — any reuse of them for
   a fresh decision should carry that caveat; they are not necessarily wrong, just measured against
   a signal that no longer exists in production going forward.

5. **Current status, for whoever picks this up**: per `OPEN_FINDINGS_REGISTER.md` OF-03, the
   master-protocol investigation is **concluded, not paused** — final decision "keep all 16 dims,
   de-weight `lempel_ziv`," with only low-priority optional phases (5/6) left unstarted. This means
   items 2-4 above are mostly a **documentation-correction task** (the investigation already
   happened and reached a real conclusion; the docs just don't reflect what changed since), not a
   request to re-run the whole master protocol from scratch. Item 1 is different — it's an
   **unimplemented** spec whose core evidence needs re-measuring before implementation, not just a
   stale-docs fix.

**Not independently verified by the research agent** (flagged so it isn't silently trusted): the
schema-contract import needed `flatbuffers`, unavailable in the sandbox used, so the 17-field
finding was confirmed via static reading of the generated `ObservationData.py` accessor code
directly rather than a live Python import. High confidence, but worth a real import-and-check before
treating it as beyond doubt.
