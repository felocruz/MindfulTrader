# Spec: Centralized Threshold Calibration Machinery — Consolidating Fragmented Recalibration
Scripts Into One Pipeline With a Real C++ Distribution Contract

**Status**: **EVOLVING** — opened 2026-08-28, update in place as this matures, don't let it go
stale (same convention as `2026-08-12-gang-literature-grounding-spec.md`). On explicit user
instruction, prompted directly by the `fractal_dim` gate investigation (`docs/superpowers/specs/
2026-08-25-observation-vector-institutional-hardening-spec.md`) finding a live `PositionManager.cpp`
threshold (`1.6f`) that has never fired in 2.5 years of real data — a single, narrow instance of a
much larger, now-surveyed pattern. **This is a starting spec, not a finished design** — real survey
work, a problem statement, and a proposed direction with explicit open questions, not a locked
architecture. User's own framing: "start something along these lines," return to finish the
`fractal_dim` work after.

**REQUIRED, added on explicit user instruction, 2026-08-28**: this initiative must include cleaning
up ALL the dated/stale `.py`/`.json` artifacts the survey below found — not as an optional nicety,
as a mandatory deliverable. See Section 3.5.

## 1. The problem, verified by direct survey, not assumed

**`lbrnet` has at least 11 separate, independently-evolved threshold-calibration scripts**, each
producing its own artifact, with no single source of truth despite at least one explicitly claiming
to be one:

| Script | Produces | Stated purpose |
|---|---|---|
| `calibrate_hmm_gate_thresholds.py` | `HMMEmpiricalGateThresholds.json` | Shannon/Taleb/Pareto gate thresholds from replayed model inference |
| `calibrate_deployment_thresholds.py` | `decision_thresholds.json` | "SINGLE SOURCE OF TRUTH for the live decision boundary" (its own docstring's words — true only for its own artifact, not threshold calibration generally, given the other 10 rows here) |
| `calibrate_context_thresholds.py` | `context_calibration.json` | Frozen `amihud_illiquidity` percentile |
| `calibrate_burst_thresholds_from_alpha.py` | burst-augmentation thresholds | Robust thresholds from raw `.alpha`/`TrainingEvent` streams |
| `calibrate_event_sample_sizes.py` | event-rate calibration | Fast `.context` stream sampling-rate calibration |
| `optimize_quality_thresholds_from_alpha.py` | pattern quality-gate thresholds (5 gates: `kangaroo_tail`, etc.) | Threshold-vector search directly from `.alpha` |
| `optimize_native_gate_thresholds.py` | (legacy) | **DEPRECATED**, "retained temporarily for migration traceability" — still present, still findable, still executable |
| `recompute_hmm_gate_report.py` | `HMMGateReport.json` | Recomputes the gate verdict from an *existing* `HMMEmpiricalGateThresholds.json`, without re-fitting |
| `refresh_directional_alpha_and_thresholds.py` | directional `.alpha` + thresholds | Conditional rebuild when raw input is newer |
| `replay_hmm_gate_compare.py` | comparison report | No-retrain old-vs-new Taleb-gate metric comparison |
| `tune_risk_gate.py` | tuned risk-gate thresholds | Object-oriented risk-gate tuning + meta-tuning |

**Real, observed consequences of this fragmentation** (not hypothetical):
- `lbrnet/models/` contains dozens of dated, prefixed snapshot artifacts for the *same* logical
  threshold family — `HMMEmpiricalGateThresholds_stale_pre_institutional_20260821.json`,
  `_pre_crash_field_20260821_150318.json`, `_holdout_recalib.json`, `_holdout_recalib_w42430.json`,
  `_holdout_recalib_w169720.json`, `.promoted_20260415_065239.json`, `.quick.json`, plus
  `decision_thresholds_f1_prev_20260610_191117.json`, `decision_thresholds_stale_v4la_20260705.json`,
  `decision_thresholds_prereconcile_20260610_221537.json`, `decision_thresholds_epoch16_locked.json`
  — manual, ad hoc versioning by filename suffix, not a real versioning scheme.
- **`lbrnet/evals/cases/hmm_stale_empirical_gate_thresholds.json` exists as a dedicated eval case**
  specifically because an agent once had to reason through "these thresholds were calibrated by a
  different model over a quarter of the current dataset, that's not a valid baseline" from first
  principles, under a gate-failure that looked like a real model defect but wasn't. **This is a
  known, recurring, eval-worthy failure mode, not a one-off.**
- **Not every threshold is even externalized to config.** `PositionManager.cpp`'s `fractal_dim`
  gate (`1.6f`/`1.3f`, `PositionManager.cpp:2144,2146`) is a hardcoded C++ literal, not read from
  `/mnt/c/Trading/config/execution_params.json` at all — it cannot be centrally recalibrated without
  a full rebuild+redeploy, unlike the config-driven kurtosis/skewness thresholds this project has
  already migrated (`ExecutionParams::LoadConfig()`). Nobody has audited which live thresholds are
  config-driven vs. hardcoded-literal across the whole C++ surface — this spec's own `fractal_dim`
  finding is presumably not the only instance.
- **`/mnt/c/Trading/config/` (the actual live C++ config location, confirmed to exist) holds only
  `execution_params.json` and `hmm_regime_risk_policy.json`** — two hand-maintained files, outside
  git (per this project's own memory of prior sessions), that must be manually kept in sync with
  whatever any of the 11 scripts above most recently produced. There is no automated publish step
  from "a script recalibrated a threshold" to "the live config file reflects it."
- **A real, already-existing methodology doc partially overlaps but does not solve this**:
  `lbrnet/docs/ADR/gate_threshold_coevolution_spec.md` + `knowledge/global/execution/gate_threshold_
  coevolution.md` address the *statistical methodology* for tuning gate thresholds against realized
  alpha (meta-labeling framing, CPCV, bilevel optimization) — a research question about *how to pick
  a good threshold value*. This spec is about a different, orthogonal problem: *given N different
  calibration processes each producing their own artifact on their own schedule, how does a single,
  traceable, non-stale value reach the live C++ config file*. Read both; they don't compete.

## 2. What "good" looks like, before designing toward it

Not decided here — named so this spec has a real target rather than open-ended scope creep:
- **One place to look** to answer "what are ALL the currently-live threshold values, and where did
  each one come from" — not 11 scripts' worth of tribal knowledge about which script owns which
  value.
- **Automatic staleness detection**, not an agent re-deriving "is this threshold stale" from first
  principles every time (the exact skill the `hmm_stale_empirical_gate_thresholds` eval tests
  manually) — a threshold artifact should carry its own provenance (source script, model version/
  hash, dataset window, generation timestamp) so staleness is a mechanical check, not a judgment call.
- **A real publish path** from "a calibration script produced a new value" to "`/mnt/c/Trading/
  config/*.json` reflects it" — today that step is manual and unaudited.
- **A complete inventory of which live C++ thresholds are config-driven vs. hardcoded literals** —
  this spec's own `fractal_dim` finding suggests the inventory doesn't fully exist yet.

## 3. Proposed direction — a registry + pipeline, not a rewrite of the 11 scripts' internals

**Explicitly NOT proposed**: rewriting any of the 11 existing calibration scripts' actual statistical
logic. Each already encodes real, specific domain knowledge (Shannon/Taleb/Pareto replay,
quality-gate vector search, burst-augmentation robustness, etc.) — this is a consolidation of
*orchestration and distribution*, not a re-derivation of *methodology*.

**Sketch — item 1 DECIDED 2026-08-28, items 2-4 still for discussion**:
1. **A single unified threshold registry manifest** (one JSON) that every calibration script writes
   an entry into on completion — `{threshold_name, value, source_script, model_version/hash,
   dataset_window, generated_at_utc, consumer(s)}` — replacing each script's independent, ad hoc
   output file naming convention. Chosen over per-family files precisely because per-family files
   would only partially fix "one place to look" (still N shapes to track, just orchestrated).
2. A **staleness-check step**, mechanical rather than judgment-based, comparing each registry
   entry's `model_version`/`dataset_window` against the currently-deployed model/dataset — this is
   the eval case's own required behavior, automated instead of re-derived per session.
3. A **publish step** that writes the registry's current values into `/mnt/c/Trading/config/*.json`
   in the exact shape `ExecutionParams::LoadConfig()`/`cpp_config.py` already expect — explicit,
   auditable, not a manual copy.
4. A **C++-side threshold inventory audit** (`MindfulTrader`-rooted, separate task) — grep every
   live gate/threshold in `RiskManager.cpp`/`Scoring.cpp`/`PositionManager.cpp`/`TradeDecisionEngine.h`
   for hardcoded numeric literals vs. `ExecutionParams`-sourced values, producing a real list of
   "config-driven" vs. "hardcoded, needs migrating first" — `fractal_dim`'s `1.6f`/`1.3f` is the one
   confirmed instance so far, there are very likely more.

## 3.5. REQUIRED cleanup — every dated/stale artifact this survey found, named explicitly

**Added on explicit user instruction, 2026-08-28: this is a mandatory deliverable of this
initiative, not an optional nicety.** This project's own standing rule already covers the
justification (`PRODUCTION_TRIAGE.md`'s top-of-doc callout: no live production consumer exists yet,
so the default is deletion once verified unused, not indefinite preservation) — this section makes
that concrete and enumerated for the threshold-calibration surface specifically, rather than leaving
it as a vague "clean up later" aspiration that never happens. **Scoped deliberately to
threshold-calibration artifacts only** — `lbrnet/models/` also holds a large, separate sprawl of
model-checkpoint/backup directories (`archive/tier123_retrain_pre_*_fix_*`, `_pre_revert_backup_*`,
`hpo_checkpoints/unified_v1..v7`, `ablation_task3b`, `ktune`) that are a different initiative's
problem, not this one's — do not scope-creep into those.

**Verification discipline before deleting any of these, per this project's own Code Safety Rules**:
confirm no live script/test/doc still reads each specific file before removing it — "no production
consumer" doesn't mean "no consumer at all"; some of these may still be read by a test fixture or a
comparison script (e.g. `replay_hmm_gate_compare.py` explicitly does old-vs-new comparisons and may
legitimately need a "before" snapshot). Check first, then delete — the standing rule flips the
*default* to deletion, it does not remove the obligation to check.

**`lbrnet/models/` — dated/stale JSON snapshots, explicit staleness markers in two filenames**:
- `HMMEmpiricalGateThresholds.promoted_20260415_065239.json`
- `HMMEmpiricalGateThresholds.quick.json`
- `HMMEmpiricalGateThresholds_holdout_recalib.json`
- `HMMEmpiricalGateThresholds_holdout_recalib_w169720.json`
- `HMMEmpiricalGateThresholds_holdout_recalib_w42430.json`
- `HMMEmpiricalGateThresholds_pre_crash_field_20260821_150318.json`
- `HMMEmpiricalGateThresholds_stale_pre_institutional_20260821.json` — **literally named "stale"**
- `archives/decision_thresholds_epoch16_20260420_174407.json`
- `decision_thresholds_epoch16_locked.json`
- `decision_thresholds_f1_diagnostic.json`
- `decision_thresholds_f1_prev_20260610_191117.json`
- `decision_thresholds_prereconcile_20260610_221537.json`
- `decision_thresholds_provenance_stale_v4la_20260705.json` — **literally named "stale"**
- `decision_thresholds_stale_v4la_20260705.json` — **literally named "stale"**
- `feasibility/20260414_210346/HMMEmpiricalGateThresholds.feasibility.json`
- `feasibility/promoted_20260414_231015/HMMEmpiricalGateThresholds.feasibility.json`
- `feasibility/retune_20260414_220848/HMMEmpiricalGateThresholds.feasibility.json`

**Presumed live/current, verify before assuming otherwise — keep unless proven stale**:
`models/HMMEmpiricalGateThresholds.json`, `models/decision_thresholds.json`,
`models/decision_thresholds_provenance.json` (no date/stale/prev/diagnostic marker in the name).

**`lbrnet/docs/data/` — dated, smoke-test, and one-off verification artifacts**:
- `quality_threshold_optimization_200k_20260417_170722.json` (dated)
- `quality_threshold_optimization_report_single_candidate_verify.json` (one-off verification run)
- `quality_threshold_optimization_smoke.json` (smoke-test artifact)
- `fsm_native_gate_threshold_sweep_report_smoke.json` (smoke-test artifact)
- `fsm_native_gate_threshold_sweep_fast_report.json` ("fast" dev-run variant)

**Presumed live/current, verify before assuming otherwise**: `docs/data/burst_thresholds_from_
alpha.json`/`.md`, `docs/data/burst_thresholds_from_directional.json`/`.md`,
`docs/data/quality_threshold_optimization_report.json`, `docs/data/fsm_native_gate_threshold_sweep_
report.json` (no staleness marker in the name).

**Dated log file**: `lbrnet/logs/calibrate_hmm_gate_thresholds_axis_fix_20260822_075908.log` — a
one-off debugging log, not an artifact anything reads programmatically; lowest-risk deletion
candidate on this whole list.

**Script itself, not just its outputs**: `lbrnet/lbrnet/scripts/optimize_native_gate_thresholds.py`
— its own docstring already says "DEPRECATED... retained temporarily for migration traceability...
should be replaced." "Temporarily" has already outlived its own justification; this initiative is
the natural point to actually delete it rather than let "temporarily" keep meaning "indefinitely."

**Acceptance gate for this section specifically**: by the time this initiative's registry/pipeline
work lands, every file above is either (a) confirmed still-needed and given a real reason why (not
just left alone by default), or (b) deleted. "We built the new system and left the old junk sitting
next to it" is an explicit failure condition for this spec, not an acceptable partial completion.

## 4. Open questions

1. **DECIDED, 2026-08-28: a single unified manifest**, not a small versioned set of per-family
   files. Every calibration script writes its entry into one registry rather than continuing today's
   pattern of each script owning its own independently-shaped output file — this is the direct fix
   for "one place to look," which a per-family-files-plus-orchestration alternative would only
   partially deliver (still N shapes to know about, just with a wrapper on top). Implementation
   (schema fields, migration order for the 11 existing scripts) is still `lbrnet`-rooted work, not
   done here — this closes the *shape* question, not the build.
2. **Where does the registry live** — `lbrnet` (where all 11 producing scripts already are) is the
   natural owner, but the registry's *consumer* is `MindfulTrader`'s C++ runtime via
   `/mnt/c/Trading/config`. Cross-repo ownership needs the same explicit-handoff discipline this
   project already applies elsewhere (row 14's `ObservationData` precedent), not an assumed default.
   **Related, checked directly, 2026-08-28**: `lbrnet/lbrnet/scripts/` is a flat 59-file directory
   with no subfolder structure at all, unlike the rest of the package (`models/`, `training/`,
   `data/`, `features/`, `labeling/`, `inference/`, etc., all domain-organized already) — a
   `calibration/` (or similarly-named) home for these scripts would fit that existing convention.
   **Recommendation, not a decision**: don't move the 11 existing scripts into a new folder as a
   standalone tidiness pass before the registry design is settled — fold the folder question into
   the registry implementation itself (new orchestration code gets a real home; existing scripts
   migrate into it as part of that work, not twice).
3. **Automate the publish step, or keep it a reviewed, explicit action?** Given no live capital is
   at risk yet (`PRODUCTION_TRIAGE.md`'s standing pre-production rule), full automation is lower-risk
   today than it will be later — but the mechanism chosen now should not assume that stays true
   forever.
4. **Does `optimize_native_gate_thresholds.py` (already marked DEPRECATED, still present) get
   deleted as part of this, or is that an unrelated cleanup?** Named here so it isn't silently
   forgotten, not decided as in-scope.
5. **Should this registry also become the thing the `hmm_stale_empirical_gate_thresholds`-style eval
   checks against**, replacing "does the agent reason about staleness correctly" with "does the
   registry's own mechanical staleness flag fire correctly"? Plausible, not decided — would need
   `lbrnet`'s own eval-authoring session to weigh in.

## 5. Non-goals

- Not re-deriving or second-guessing any of the 11 scripts' own statistical methodology.
- Not implementing anything in this spec — survey, problem statement, and a proposed direction only.
- Not deciding the exact registry schema, file location, or automation level (Section 4).
- Not auditing the full C++ hardcoded-vs-config-driven threshold inventory here — named as a
  concrete follow-on task (Section 3 item 4), not executed in this pass.
- **Not deleting any of Section 3.5's named files from this `MindfulTrader`-rooted session** — per
  this project's repo-scope convention, that execution belongs to an `lbrnet`-rooted session, same
  as every other `lbrnet`-side action item in this document. Section 3.5's list and verify-then-
  delete instructions are the deliverable of this spec; the actual deletion is that session's job.

## 6. Cross-references

- `docs/superpowers/specs/2026-08-25-observation-vector-institutional-hardening-spec.md` — the
  `fractal_dim` gate finding that prompted this spec.
- `lbrnet/docs/ADR/gate_threshold_coevolution_spec.md` + `lbrnet/knowledge/global/execution/
  gate_threshold_coevolution.md` — the complementary statistical-methodology doc for *how* to pick a
  good threshold value against realized alpha; this spec is about *getting whatever value is chosen,
  by whichever method, from calibration to the live C++ config* without today's fragmentation.
- `lbrnet/evals/cases/hmm_stale_empirical_gate_thresholds.json` — concrete evidence this failure
  mode is real and already considered eval-worthy by this project.
- `PRODUCTION_TRIAGE.md` — new row added for this initiative, tracking cross-project.
