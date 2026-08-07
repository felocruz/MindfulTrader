# ADR — `burstiness_index` (HMM Observation Dim 1) Is Not Classical Burstiness

**Status:** DOCUMENTED, rename DEFERRED (same class/precedent as the `vpin_toxicity` → `amihud_illiquidity` item in `amihud_gate_percentile_spec.md` §O4/§2c).
**Discovered:** 2026-07-23, during the Student-t HMM static-scaling recalibration investigation (`logs/rc_gemini.log` CLAUDE_BRIEF_023/024/025 in the `lbrnet` repo).

## The mismatch

`ObservationData.burstinessIndex` (schema `mts_schema.fbs`, obs index 1, model field `burstiness_index`) is named after classical burstiness (the coefficient of variation of inter-arrival times — the standard "how clustered are event arrivals in time" measure, sometimes called the Fano-factor-adjacent Kleinberg/Fano burstiness convention).

That classical definition **is implemented elsewhere in this codebase**, correctly, under a different name: `raschkeBurst` (`LocalRiskContext`/`RiskGateContext`), computed by `CalculateBurstinessIndex()` (`ContextManager.cpp:1429`, declared `ContextManager.h:876`), documented explicitly at its field definition as "CV of inter-arrival times (clustering)" (`ContextManager.h:95`). `raschkeBurst` feeds only live risk gates (spread-gate tightening thresholds `>2.0`/`>3.0`, per `amihud_gate_percentile_spec.md` §6 item 3) — it is **never** part of the HMM's 16D observation vector.

The HMM's `burstiness_index` (obs index 1) is a **different signal entirely**: source comments at its scaling-constant definitions (`ContextManager.cpp:28,47`) document it as a **"half-window variance ratio"** — a realized-volatility-clustering measure, not an inter-arrival-time statistic. This is corroborated by this session's empirical spread analysis on the ECME-fixed K=4 retrain: `burstiness_index` showed real, coherent state-differentiating signal (spread rank 6th of 16, State 2 = momentum/burst regime), and its behavior tracked `relative_range` (a LOGZ volatility-magnitude channel) — consistent with a volatility-clustering interpretation, not an arrival-timing one.

**Not yet pinned down**: the exact function that computes the HMM's raw `burstiness_index` value before scaling (the "half-window variance ratio" itself) was not located in this pass — only its scaling-stage source comments were verified. Anyone acting on this ADR to actually rename or re-document the metric should trace that computation site first, rather than assuming a specific function name.

## Why this matters

Two real metrics, both plausibly called "burstiness," feed two different consumers (HMM training vs. live risk gates) under names that invite confusion between them — exactly the class of problem `vpin_toxicity`/`amihud_illiquidity` already is (a stale name describing what the field used to compute, or what it sounds like it should compute, rather than what it actually computes).

## Recommendation

Defer any rename to the same "next retrain cycle" bundling precedent as `vpin_toxicity` (`amihud_gate_percentile_spec.md` §O4/§2c): renaming a model-input dimension name ripples into training/inference feature maps (Python name-based readers in `train_student_t_hmm.py`, `hmm_utils.py`), so it should land together with a deliberate retrain, not as an isolated documentation-only patch. Candidate replacement name once actioned: something reflecting "volatility clustering" or "realized-variance ratio" rather than "burstiness," to stop implying the classical CV-of-inter-arrival-times definition.

## Cross-reference

- `amihud_gate_percentile_spec.md` §O4, §2c — the `vpin_toxicity` → `amihud_illiquidity` rename this ADR is deliberately following the precedent of.
- `lbrnet/docs/hmm/STUDENT_T_HMM_RUNBOOK.md` — HMM 16D observation vector contract; this ADR's finding should be reflected there once acted on.
- `logs/rc_gemini.log` (lbrnet repo) — `CLAUDE_BRIEF_023` Part 1 (discovery), `CLAUDE_BRIEF_024`/Gemini's response (recommendation to track this alongside the Amihud rename).
