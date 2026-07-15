# Spec — Amihud Gate: Percentile Normalization + Honest Rename (Layer B)

**Status:** SPEC — DRAFT FOR REVIEW (2026-07-14). Schema-touching (rename) + live-gate logic change.
**Decision of record:** `docs/ADR/liquidity_toxicity_gate_decision.md` (Gemini ruling, reconciled against code).
**Resolves:** PC-03 (rename) + a genuine live-gating calibration bug + reclassifies lbrnet PC-17.
**Depends on / pairs with:** `risk_gate_context_wire_spec.md` (the raw signal on the wire). Best executed as one verified pass since both touch the same gate code.

---

## 1. What Gemini's ruling established (reconciled with code)

1. **Reject real VPIN.** Andersen & Bondarenko (2014): VPIN's predictive value is subsumed by volume/volatility; its bulk-volume classification is artifact-heavy. Redundant here — the system already measures toxicity directly via order-flow imbalance + T&S micro-asymmetry. **No VPIN field is added.**

2. **The fixed threshold is the real bug.** `RiskManager::EvaluateHardGates` uses `ctx.vpin > 0.80` (`0.40` fat-tail) — a **raw, unbounded, non-stationary** Amihud value compared to a **fixed** constant. Non-stationary in price/volume regime → the constant is meaningless across regimes. Fix: normalize to a **rolling empirical percentile** (or robust median/MAD z-score) over a trailing window; regime-tightening operates on the percentile (e.g. veto if > p90 normally, > p75 in fat-tail: DOF ≤ 4 or kurtosis > 8).

3. **PC-17 reclassified: stopgap → canonical.** lbrnet's `_VPIN_TOXICITY_TAIL_THRESHOLD_NORMAL/FAT_TAIL` (p90 / p75) were labeled a temporary rebase. They are, in fact, **exactly Gemini's prescribed design.** The defect is the C++ side's fixed `0.80`/`0.40`, not the Python percentile approach. Both sides converge on rolling percentiles.

4. **Dual-axis veto (deferred, Layer C).** `REJECT IF amihud_pct > L OR ofi_toxicity_pct > T`. Requires wiring order-flow imbalance into `LocalRiskContext` + a new gate. Validate value first; not in this spec.

## 2. Rename inventory (`vpin` → `amihud_illiquidity`)

Two representations, two blast radii:

**(a) Raw gate value — C++-internal, no model impact (rename in this pass):**
- `LocalRiskContext.vpin` (`ContextManager.h:74`) → `amihudIlliquidity`
- populate: `ContextManager.cpp:535` (`m_localRiskContext.vpin = obs[OBS_VPIN_TOXICITY]`)
- gate: `RiskManager.cpp:791,805,808` (`ctx.vpin` + error strings — change "VPIN toxicity critical")
- mirrors: `RiskManager.cpp:1202` (`baseRec.context.vpin`), `:1345` (`rpIn.vpin`), `:1834` (`rec.context.vpin`)
- `Scoring.cpp:283,285` (`ctx.vpin`)
- `PositionManager.cpp:2352-2353, 2840-2841` (`lrc.vpin` toxic-flow)
- mirror structs each with their own `vpin`: `RejectionLedger.h:87` + field-map string `:146` (`{"vpin", …}`), the `RiskPolicy` input type (`rpIn`), `NormalizedAnchors` (`anchors.vpin`, `TripleScreen3.cpp:727,1327`)
- **Already done:** the new `RiskGateContext.amihud_illiquidity` schema field (born correct).

**(b) Scaled observation field — MODEL INPUT, retrain blast radius (DEFER to next HPO/training cycle):**
- schema `ObservationData.vpin_toxicity` (`mts_schema.fbs:395`, obs index 11)
- `OBS_VPIN_TOXICITY` enum (`ContextManager.cpp:39,58,791`) + generated `kObsVpinToxicity` (`mts_schema_contract_generated.h:33`)
- Python name-based readers (`schema_contract` field map, `calibrate_context_thresholds.py`, `backtest_runner.py` obs[11])
- **Rationale for deferral:** renaming a model-input dimension name ripples into training/inference feature maps; per PC-03's original rationale, bundle with a retrain. Index 11 is unchanged; only the *name* is stale — low risk to defer.

## 3. Implementation steps

1. **Rename (a)** across the C++ raw-gate path (use LSP rename per struct where the language server is functional; otherwise careful per-file edits + build). Keep `RejectionLedger`/`RiskPolicy`/`NormalizedAnchors` mirrors consistent. Build green.
2. **Rolling-percentile estimator** in `ContextManager` (or `RiskManager`): maintain a trailing window of raw Amihud values (window ≈ 21 trading days' worth of bars; O(1) ring buffer + rolling rank, no hot-path alloc). Expose `AmihudPercentile()`.
3. **Rewrite the gate** in `EvaluateHardGates`: replace `ctx.vpin > 0.80/0.40` with `amihud_pct > pL/pT` where `pL=0.90`, `pT=0.75` (fat-tail: DOF ≤ 4 or kurtosis > 8). Keep it a hard veto.
4. **Serialize** both the raw value (`RiskGateContext.amihud_illiquidity` — already speced) and, if useful for parity/audit, the computed percentile. Python reads the raw value and computes the *same* percentile (shared window definition) OR reads the C++-computed percentile directly (simpler parity — recommended).
5. **Python co-evolution:** reclassify PC-17 — the p90/p75 constants become the canonical thresholds; delete the "TEMPORARY stopgap" framing; read `amihud_illiquidity` (raw) or the serialized percentile from `RiskGateContext`.
6. **Regenerate** (after the flatc-version decision) + build both repos.

## 4. Open decisions

- **O1 — Window definition:** trailing bar-count for the rolling percentile (21 trading days on 15-min bars ≈ 21×26 ≈ 546 bars). Confirm the exact window + whether it resets on session/day boundaries.
- **O2 — Percentile parity mechanism:** ship C++-computed `amihud_percentile` on the wire (simplest exact parity), vs. ship only raw and have both sides compute the percentile from a shared window spec (risk of drift). Recommend **shipping the percentile**.
- **O3 — Threshold values:** p90/p75 (Gemini's example, matches PC-17's empirical rebase) vs. re-derive from the real `.alpha` Amihud distribution. Recommend validating against the 200k-event sample.
- **O4 — Observation-field rename timing:** confirm deferral of `vpin_toxicity` → `amihud_illiquidity` to the next retrain cycle.

## 5. Acceptance

1. Build green (C++); rename complete (no residual raw-side `vpin` in the gate path); gate uses percentile.
2. Gate firing rate on a replay is sane (not 0%, not ~100%) — the calibration bug's symptom (PC-17 noted the scaled proxy fired 31–46%) resolves.
3. C++ and Python agree on the veto decision bar-for-bar on a shared replay (percentile parity).
4. PC-03 CLOSED; PC-17 reclassified as canonical (stopgap framing removed).
