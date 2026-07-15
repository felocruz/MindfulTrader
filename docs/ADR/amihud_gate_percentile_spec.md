# Spec — Amihud Gate: Percentile Normalization + Honest Rename (Layer B)

**Status:** SPEC — DECISIONS LOCKED (2026-07-15), C++ implementation in progress. Schema-touching (rename + `amihud_percentile` field) + live-gate logic change + Amihud formula correctness fix.
**Decision of record:** `docs/ADR/liquidity_toxicity_gate_decision.md` (Gemini ruling, reconciled against code).
**Resolves:** PC-03 (rename) + a genuine live-gating calibration bug + a non-canonical Amihud formula + reclassifies lbrnet PC-17.
**Depends on / pairs with:** `risk_gate_context_wire_spec.md` (the raw signal on the wire). Best executed as one verified pass since both touch the same gate code.

> **Strategy note (2026-07-15):** Finish the C++ side and **freeze the `.context` instrument once**, then hand off to Python. The Python co-evolution (§5/§6) is deliberately deferred until C++ is complete, because the Python gate-parity work can only be validated against a **freshly regenerated `.context`** that carries the final raw values, the canonical Amihud formula, and the serialized percentile. Regenerating mid-way produces a baseline Python would have to discard.

---

## 0. Locked decisions (2026-07-15)

| # | Decision | Resolution |
|---|----------|-----------|
| Timeframe | Which Triple Screen frame computes Amihud | **TS3 / Ripple / 15-min — KEEP.** Institutionally correct: Amihud is a low-frequency proxy for Kyle (1985) λ (Goyenko-Holden-Trzcinka 2009); 15-min tames bid-ask-bounce noise that corrupts tick/1-min Amihud while staying at the entry-decision timeframe. Finer = rejected VPIN; coarser (TS2/TS1) = laggy for entry protection. |
| Formula | Canonical correctness | **FIX.** Current `CalculateAmihudIlliquidity` uses `|ΔP|` (absolute price change, not a return), raw contract volume (not dollar volume), and a non-standard `* volumeAvg` re-multiplication → price-level **non-stationary** (root cause of the fixed-threshold failure). Rewrite to canonical: `mean(|r_t| / DollarVolume_t)`, `r_t = ln(P_t / P_{t-1})` (**log return** — additive, symmetric, standard for fat-tailed/Student-t modeling and consistent with the system's Taleb kurtosis/skewness estimators), `DollarVolume_t = P_t · V_t`. Drop `* volumeAvg`. |
| O1 | Rolling-percentile window | **21 trading days, SESSION-AWARE (separate RTH vs overnight percentile pools), continuous ring (no daily reset).** Sample one raw Amihud per closed 15-min bar. 24h/globex data → overnight thin-volume Amihud must not contaminate the RTH pool. Amihud's native aggregation is monthly; liquidity-risk/TCA practice converges on a ~1-month recent-normal window. RTH pool ≈ 546 samples, overnight ≈ 1470 over 21d — both resolve p90/p75. |
| O2 | Percentile parity mechanism | **Ship the C++-computed `amihud_percentile` on the wire** (new `RiskGateContext` field). Exact bar-for-bar veto parity; Python reads it directly rather than re-deriving (no window-drift risk). |
| O3 | Threshold values | **p90 normal / p75 fat-tail** (fat-tail = Student-t DOF ≤ 4 **or** realized kurtosis > 8). Validate firing rate against the 200k-event `.alpha` sample before sign-off (not 0%, not ~100%). |
| O4 | `vpin_toxicity` obs-field rename | **DEFER** to next retrain cycle (obs index 11 unchanged, name only is stale — renaming ripples into training/inference feature maps). |

---

## 1. What Gemini's ruling established (reconciled with code)

1. **Reject real VPIN.** Andersen & Bondarenko (2014): VPIN's predictive value is subsumed by volume/volatility; its bulk-volume classification is artifact-heavy. Redundant here — the system already measures toxicity directly via order-flow imbalance + T&S micro-asymmetry. **No VPIN field is added.**

2. **The fixed threshold is the real bug.** `RiskManager::EvaluateHardGates` uses `ctx.vpin > 0.80` (`0.40` fat-tail) — a **raw, unbounded, non-stationary** Amihud value compared to a **fixed** constant. Non-stationary in price/volume regime → the constant is meaningless across regimes. Fix: normalize to a **rolling empirical percentile** (or robust median/MAD z-score) over a trailing window; regime-tightening operates on the percentile (e.g. veto if > p90 normally, > p75 in fat-tail: DOF ≤ 4 or kurtosis > 8).

3. **PC-17 reclassified: stopgap → canonical.** lbrnet's `_VPIN_TOXICITY_TAIL_THRESHOLD_NORMAL/FAT_TAIL` (p90 / p75) were labeled a temporary rebase. They are, in fact, **exactly Gemini's prescribed design.** The defect is the C++ side's fixed `0.80`/`0.40`, not the Python percentile approach. Both sides converge on rolling percentiles.

4. **Dual-axis veto (deferred, Layer C).** `REJECT IF amihud_pct > L OR ofi_toxicity_pct > T`. Requires wiring order-flow imbalance into `LocalRiskContext` + a new gate. Validate value first; not in this spec.

## 2. Rename inventory (`vpin` → `amihud_illiquidity`)

Three representations, three blast radii:

**(a) Raw single-source-of-truth field — C++-internal, no model/contract impact (DONE, Phase B):**
- `LocalRiskContext.vpin` → `amihudIlliquidity` (`ContextManager.h`), populate at `ContextManager.cpp` and the `RiskGateContext` wire mapping.
- All reads updated: `RiskManager::EvaluateHardGates` log strings, the three mirror-assignment RHS (`baseRec.context.vpin = lrc.amihudIlliquidity`, `rpIn.vpin = lrc.amihudIlliquidity`, `rec.context.vpin = localCtx.amihudIlliquidity`), and `Scoring.cpp` gate reads.
- `PositionManager` toxic-flow now reads `lrc.amihudPercentile` (Phase A) — no raw read remains.
- **Already done earlier:** the `RiskGateContext.amihud_illiquidity` schema field (born correct).

**(b) Serialized DTO mirror fields + JSON keys — CONTRACT-coupled (DEFER to a Python-coordinated pass):**
- `RejectionLedger::ContextSnapshot.vpin` + JSON key `{"vpin", …}` (consumed by the Python PAER).
- `RiskPolicy` input `.vpin` (`TradeDecisionEngine.h`) + its own `ToJson` `{"vpin", …}` key + the `in.vpin / 0.80` sizing normalization.
- `NormalizedAnchors.vpin` (written from the subgraph at `TripleScreen3.cpp`).
- **Rationale for deferral:** these DTO field names double as JSON/serialization keys read downstream; renaming them in isolation breaks the consumer contract. Rename them together with the Python readers (same discipline as (c)). The C++ source now assigns from `lrc.amihudIlliquidity`, so the value is correct; only the mirror *name* is legacy.
- **Also flagged (separate calibration follow-up):** `Scoring.cpp` (`> 0.80/0.60`) and `RiskPolicy` (`in.vpin / 0.80`) still gate/normalize on the raw, non-stationary Amihud against fixed constants — the same weakness Layer B fixed in the hard gate. Percentile conversion of these two sites is a pending behavior-change decision (not part of the cosmetic Phase B rename).

**(c) Scaled observation field — MODEL INPUT, retrain blast radius (DEFER to next HPO/training cycle):**
- schema `ObservationData.vpin_toxicity` (`mts_schema.fbs:395`, obs index 11)
- `OBS_VPIN_TOXICITY` enum (`ContextManager.cpp:39,58,791`) + generated `kObsVpinToxicity` (`mts_schema_contract_generated.h:33`)
- Python name-based readers (`schema_contract` field map, `calibrate_context_thresholds.py`, `backtest_runner.py` obs[11])
- **Rationale for deferral:** renaming a model-input dimension name ripples into training/inference feature maps; per PC-03's original rationale, bundle with a retrain. Index 11 is unchanged; only the *name* is stale — low risk to defer.

## 3. Implementation steps (C++)

0. **Fix the Amihud formula** (`CalculateAmihudIlliquidity`, `StudyHelperFunctions.cpp:3960`): compute `mean(|ln(P_t/P_{t-1})| / (P_t·V_t))` over the adaptive lookback; drop the `* volumeAvg` re-multiplication. Canonical, price-level-stationary. This changes the raw `.context` values (intended — done while freezing the instrument).
1. **Rename (a)** across the C++ raw-gate path (`vpin` → `amihudIlliquidity`; use LSP rename per struct where the language server is functional; otherwise careful per-file edits + build). Keep `RejectionLedger`/`RiskPolicy`/`NormalizedAnchors` mirrors + the field-map string consistent. Build green.
2. **Session-aware rolling-percentile estimator** in `ContextManager`: maintain **two** trailing windows of raw Amihud values (RTH pool + overnight pool), each ≈ 21 trading days, sampled **once per closed 15-min bar** (not per tick). O(1) ring buffer + rolling rank, no hot-path alloc, continuous (no daily reset). Route each sample to the pool by `TimeOfDayIndicator` session. Expose `AmihudPercentile()` (percentile of the current raw value within its session's pool).
3. **Rewrite the gate** in `EvaluateHardGates`: replace `ctx.vpin > 0.80/0.40` with `amihud_pct > pL/pT` where `pL=0.90`, `pT=0.75` (fat-tail: DOF ≤ 4 or kurtosis > 8). Keep it a hard veto. Apply the same percentile logic to the `PositionManager` toxic-flow checks (currently fixed `> 0.75 / > 0.40`).
4. **Serialize** both the raw value (`RiskGateContext.amihud_illiquidity`) **and** the computed `amihud_percentile` (new `RiskGateContext` field, O2) — Python reads the percentile directly for exact parity.
5. **Python co-evolution:** deferred — see §4b (execute after `.context` regen).
6. **Regenerate** (`regenerate_schema.sh` from base env) + build both repos + commit all three (schema, MindfulTrader, lbrnet).

## 4. Open decisions — RESOLVED

All four resolved 2026-07-15; see §0. Summary: O1 = 21-day session-aware continuous window; O2 = ship `amihud_percentile` on the wire; O3 = p90/p75 (validate vs sample); O4 = defer the obs-field rename. Plus the newly-discovered **formula fix** (log return + dollar volume, drop `* volumeAvg`).

## 4b. Python handoff (execute AFTER C++ complete + `.context` regenerated)

The Python co-evolution cannot be validated until a fresh `.context` is regenerated by the completed C++ side. Once that exists, lbrnet steps in:

1. **Read from `RiskGateContext`** on the `.context` `MarketObservation` (via `iter_context_stream` / `context_stream.py`):
   - `amihud_illiquidity` (raw, canonical log-return/dollar-volume value) — for audit/analysis.
   - `amihud_percentile` (0–1) — **the gate input**; compare directly to the thresholds. No Python-side window computation (parity by construction).
2. **Rewrite the toxicity gate** to `amihud_percentile > 0.90` (normal) / `> 0.75` (fat-tail: DOF ≤ 4 or kurtosis > 8), matching C++ `EvaluateHardGates` bar-for-bar.
3. **Reclassify PC-17:** the p90/p75 constants become canonical — delete the "TEMPORARY stopgap / rebase once C++ ships" framing.
4. **Delete PC-15/16/17 scaled-proxy stopgaps** and restore the real thresholds for the sibling gates: `spread_stress > 0.85` (Finding 19), raw Hill-α Pareto breach (Finding 20), Amihud percentile (Finding 21). Read all three from `RiskGateContext.*`, not `observation[9/11/12]`.
5. **Runtime contract test:** add `test_risk_gate_context_contract.py` mirroring `test_observation_contract.py` — assert the `RiskGateContext` binding fields match the C++ `LocalRiskContext` field set (+ `amihud_percentile`), so future schema drift is caught at test time.
6. **Close** Findings 19/20/21 = PC-15/16/17; PC-03 CLOSED.

**Backward-compat:** treat an absent `risk_gate_context` (old `.context`) as "legacy → keep stopgap path" until re-collection, or gate the switch on field presence.

## 5. Acceptance

1. Build green (C++); rename complete (no residual raw-side `vpin` in the gate path); gate uses percentile.
2. Gate firing rate on a replay is sane (not 0%, not ~100%) — the calibration bug's symptom (PC-17 noted the scaled proxy fired 31–46%) resolves.
3. C++ and Python agree on the veto decision bar-for-bar on a shared replay (percentile parity).
4. PC-03 CLOSED; PC-17 reclassified as canonical (stopgap framing removed).
