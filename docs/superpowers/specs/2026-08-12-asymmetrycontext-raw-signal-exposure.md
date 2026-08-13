# Spec: Expose Raw Risk-Gate Signals on the Existing Unscaled `AsymmetryContext` Channel

Date: 2026-08-12
Status: **NOT AUTHORIZED, NOT IMPLEMENTED.** Requires explicit separate authorization per
`docs/ADR/execution_correctness_findings_spec.md` Findings 19/20/21 (schema-change blast radius +
live-trading stakes). This doc consolidates those findings into one place someone can pick up and
scope; it is not a green light to start.
Scope: `../schema/mts_schema.fbs` (`AsymmetryContext` struct), `src/ContextManager.cpp`
(`GetAsymmetryContext()`/`MakeAsymmetryContext()`), plus `RiskManager.cpp`/`StudyHelperFunctions.cpp`
as read-only context (no change needed there — the live gates are already correct).

This is filed separately from `docs/superpowers/plans/2026-08-12-statistical-context-relrange-sentinel-gap.md`
— that finding is about `relative_range`'s *carry-forward*/scaler behavior; this one is a distinct
defect family (raw-signal-never-serialized) affecting different dims (9, 11, 12), not relative_range.

## The pattern (Findings 19, 20, 21 — `docs/ADR/execution_correctness_findings_spec.md`)

Three of sixteen `ObservationData` dims are consumed two ways: `RiskManager`'s live hard gates check
the **raw, pre-scaling** physics value in-process; `ContextManager`'s `FeatureScaler` then log-z-scores/
winsorizes the *same* dimension for the Student-t HMM, and only that **scaled** value is ever
serialized to `.alpha`/`.context`. The scaling itself is correct, deliberate feature engineering for
the HMM — not a defect. The defect is that no downstream, non-model consumer (a Python parity
backtest, or any future non-HMM consumer) can read the representation the live C++ gate actually acts
on.

| dim | raw value + real gate | C++ raw-value source | wire proxy range (verified, 200k-event samples) | Python workaround (`lbrnet`, `backtest_runner.py`) |
|---|---|---|---|---|
| 12 `liq_fragility` | `[0,1]` fragility score, gate `> 0.85` (`RiskManager.cpp:832-836`) | `CalculateLiquidityFragility()`, `StudyHelperFunctions.cpp:2766-2886` | `[-4.66, 6.0]`, median 0.0 | Rebased to empirical p95 of proxy (≈3.98). **TEMPORARY.** |
| 11 `amihud_illiquidity` | unbounded raw value, gates `0.80`/`0.40` ("GAP 26", `RiskManager.cpp:797-812`) | `CalculateAmihudIlliquidity()`, `StudyHelperFunctions.cpp:3149-3180` | `[-1.9459, 1.9459]` (SOFTLOGZ) | Rebased to empirical p90 (normal)/p75 (fat-tail). **TEMPORARY.** |
| 9 `tail_index` | raw Hill alpha `[1.1, 8.0]` or `0.0` warmup (`PositionManager.cpp:63-69`, `ComputeParetoTopStateRatioProxy()`) | `TailRiskEngine`'s cached `m_cachedHillAlpha` | `[-1.9459, 1.9459]` (SOFTLOGZ), 82.8% pinned at the "invalid" fail-safe point | **Gate removed entirely from the Python replica** — no threshold can work on this proxy (rejects ~everything or ~nothing). Intentional, tracked parity divergence, not a stopgap. |

Related but smaller/different mechanism, tracked here for completeness:

- **Finding 18** — `RiskManager.cpp:820-824`'s Shannon-entropy hard gate (`ctx.shannonFlowEntropy > 0.90`)
  compares raw bits (range `[0, ~3.32]`) against a threshold clearly authored for a normalized ratio —
  fires on ~100% of real observations. This is a straight, live, **one-line C++ bug**, not a
  wire-exposure gap (the raw value the gate needs already reaches wherever it needs to — the threshold
  is just wrong). `lbrnet` patched its own replica by dividing by `log2(10)`; the real C++ gate is
  unfixed. Much smaller blast radius than the schema change below — could be authorized and fixed
  independently, sooner.
- **`vol_convexity` (dim 4)** — not a wire-exposure defect at all. Python simply assumed the wrong
  range (`[-1,+1]`) for the same value both sides see; self-corrected by empirically measuring the true
  range (`[-6,+6]`) and deriving a rescale constant. No C++ action needed.

## Recommended fix (per Findings 19/20/21 — not new analysis, restated for implementability)

**Extend the existing, already-shipping unscaled channel rather than invent new schema machinery.**
`AsymmetryContext` (`schema/mts_schema.fbs:407`, an 8D `struct` already serialized inside
`MarketObservation` alongside `ObservationData`) exists specifically for this purpose — confirmed by
its own in-code comment (`ContextManager.cpp:1219`): *"AsymmetryContext 8D is NOT scaled here. It is
used as raw embedding lookup."*

Concretely:
1. Add 3 new raw `float` fields to `AsymmetryContext` (or a sibling struct, if 8D is considered full —
   TBD by whoever scopes this): raw `[0,1]` `liq_fragility`, raw unbounded `amihud_illiquidity`, raw
   Hill alpha `tail_index`.
2. Populate them in `ContextManager::GetAsymmetryContext()`/`MakeAsymmetryContext()`
   (`ContextManager.cpp:561-598`, `:664-666`) from values already computed and in-process — no new
   calculation, just wiring already-live numbers to a new field, mirroring how the other 8 raw
   `AsymmetryContext` dims are already populated.
3. Requires `regenerate_schema.sh` (schema is `../schema/mts_schema.fbs`, owned per this project's
   schema-ownership rules — not edited directly here) before `build_dll.sh`.
4. Once shipped: delete `lbrnet`'s three stopgaps (`_LIQ_FRAGILITY_TAIL_THRESHOLD`,
   `_AMIHUD_ILLIQUIDITY_TAIL_THRESHOLD_NORMAL/_FAT_TAIL` in `backtest_runner.py`, and reintroduce the
   removed dim-9 gate) and point them at the new raw fields — removal instructions already tracked in
   `docs/ADR/params_convergence_spec.md` PC-15/PC-16/PC-17.

## Why this hasn't been scoped into tasks yet

All three source findings explicitly require separate authorization before implementation, given the
schema-change blast radius and live-trading stakes (this touches the wire format consumed by both the
live C++ gates and the Python backtest/training pipeline simultaneously). This doc exists so the shape
of the fix isn't lost, not to greenlight starting it.

## Open items for whoever scopes the actual implementation plan

- Whether `AsymmetryContext`'s 8D is considered full/fixed-size (would need a new sibling struct) or
  has headroom for 3 more fields — not checked here.
- Whether all three raw fields ship together in one schema change, or are staged (e.g., dim 9 first,
  since its Python-side gate is fully disabled today and has zero stopgap risk to unwind).
- Per the source spec's own "Broader observation, not yet actioned": 13 of 16 `ObservationData` dims
  use `SOFTLOGZ`/`LOGZ` scaling — worth checking whether any *other* dim has this same raw-gate/
  scaled-wire mismatch before assuming only 9/11/12 are affected.
- Finding 18 (Shannon entropy) is independent of this schema work and could be authorized/fixed
  separately, sooner, given its much smaller footprint (one comparison, no schema change).
