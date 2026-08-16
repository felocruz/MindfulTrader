# Gate-Stack Stationarity Audit Findings

**Status:** COMPLETE — audit closed 2026-08-15, per Unit B of
`docs/superpowers/specs/2026-08-15-risk-gate-context-cpp-coevolution.md`.
**Methodology:** re-running `docs/ADR/amihud_gate_percentile_spec.md` §6's own
stationarity test against every fixed-threshold comparison in
`RiskManager::EvaluateHardGates()` (`src/RiskManager.cpp:831-894`) and
`EvaluateEmpiricalRegimeGates()` (`src/execution/ExecutionGate.cpp:19-40`).

## Gates in `RiskManager::EvaluateHardGates()`

1. **Amihud illiquidity veto** (`RiskManager.cpp:854-868`) — already fixed by
   `amihud_gate_percentile_spec.md`; gates on `ctx.amihudPercentile`, a
   session-aware rolling percentile, not a raw fixed threshold. No further
   action.
2. **Taleb cliff proximity / Elder Chandelier stop distance**
   (`RiskManager.cpp:869-874`, `ctx.elderChandelierATR < 0.50f`) —
   **STATIONARY, fixed threshold defensible.** `amihud_gate_percentile_spec.md`
   §6 explicitly lists this gate (self-normalizing ratio to ATR, dimensionless)
   in its "not worth it, session-invariant" bucket. No action needed.
3. **Shannon entropy halt** (`RiskManager.cpp:875-880`,
   `ctx.shannonFlowEntropy > shannonEntropyHaltFrac * kShannonMaxEntropyBits`) —
   **STATIONARY.** Bounded `[0, log2(10)]` normalized ratio.
   `amihud_gate_percentile_spec.md` §6 lists Shannon entropy in the
   session-invariant bucket. No action needed.
4. **Taleb (Moors octile) kurtosis halt** (`RiskManager.cpp:881-886`,
   `ctx.talebKurtosis > talebKurtosisHaltThreshold`) — **STATIONARY.**
   `talebKurtosis` is the Moors (1988) robust octile-kurtosis estimator
   (`RobustMoments.h`), dimensionless, clamped `[0,5]`, threshold itself
   already percentile-derived once (P85.0, `RiskManager.cpp:68-73`).
   `amihud_gate_percentile_spec.md` §6 lists kurtosis in the session-invariant
   bucket. No action needed.
5. **Liquidity fragility / spread stress veto** (`RiskManager.cpp:887-892`,
   `ctx.spreadStress > 0.85f`) — **STATIONARY.** Bounded `[0,1]` sigmoid
   composite of two diurnal-normalizing ratios. `amihud_gate_percentile_spec.md`
   §6 item 2 already ran this exact verification ("VERIFIED — session-pooling
   NOT warranted"). No action needed.

## Gates in `ExecutionGate::EvaluateEmpiricalRegimeGates()`

6. **Pareto top-state-ratio breach** (`ExecutionGate.cpp:24-26`,
   `ctx.paretoTopStateRatio > ctx.paretoTopStateRatioMax`) — **NEW FINDING,
   not previously covered by the Amihud ADR.** The raw signal
   (`ComputeParetoTopStateRatioProxy`, `PositionManager.cpp:1613`/`:2491`) is
   `clamp(1.0f / ctx.paretoTailAlpha, 0.0f, 1.0f)` — a robust, order-statistic
   Hill (1975) tail-index proxy, dimensionless, same general family as Moors
   kurtosis (finding 4 above). **Verdict: STATIONARY, fixed threshold
   (`0.25` compiled default, JSON-overridable) defensible** — this is a
   dimensionless tail statistic, not a price/volume-level-dependent
   microstructure quantity, so it belongs in the same "not worth it" bucket
   as kurtosis/entropy rather than needing Amihud's rolling-percentile
   treatment.
   **Separate documentation debt, not fixed here:** the field name
   `paretoTopStateRatio` implies an HMM top-state occupancy probability, but
   it is actually computed as `1/Hill-α` — the same naming-drift class the
   Amihud ADR fixed for `vpin` → `amihud_illiquidity`. A rename touches
   `PositionManager.cpp` (2 call sites), `ExecutionGate.cpp`,
   `ContextManager.h:414`'s field, and the wire schema field
   `RiskGateContext.pareto_top_state_ratio` — out of scope for this audit;
   flagged here for a future low-priority pass, not filed as an action item.
7. **Shannon regime-tenure breach** (`ExecutionGate.cpp:29-31`,
   `ctx.shannonTenureBars < ctx.shannonMinTenureBars`) — **STATIONARY.** Raw
   signal is a bar count (duration), not a price/volume-scale quantity.
   Matches `amihud_gate_percentile_spec.md` §6's "regime-latency"
   session-invariant bucket. No action needed.
8. **Taleb-signal-sigma breach** (`ExecutionGate.cpp:34-36`,
   `ctx.talebSignalSigma > ctx.talebSignalSigmaThreshold`) — raw signal
   (`PositionManager.cpp:1615`/`:2493`) is `max(0.0f, ctx.talebKurtosis)`, the
   same Moors octile kurtosis as finding 4 — **STATIONARY, no treatment
   needed on the same grounds.**
   **Confirmed separate bug (not a stationarity problem): threshold
   value drift.** `taleb_signal_sigma_threshold` had two live-but-mismatched
   MindfulTrader-side values: compiled default `1.8382`
   (`RiskManager.cpp:74`) vs. live `hmm_regime_risk_policy.json` override
   `1.8401` — both intended to be the same 2026-08-13 P85.0 percentile-match
   rescale, differing only because the compiled default was never updated to
   match. **Fixed by Task 2 of this plan** (compiled default now `1.8401`).
   lbrnet-side values (`backtest_runner.py`'s `9.636797` hardcoded fallback,
   `lbrnet/models/HMMEmpiricalGateThresholds.json`'s `6.67559116507085`,
   dated 2026-07-26 — older than the 2026-08-13 rescale) are a cross-repo
   follow-up for an lbrnet-rooted session, not fixed here. The structural
   mechanism to keep all of these in sync going forward is Unit C's
   git-tracked shared-config + sync-script work, a separate plan.

## Summary

| # | Gate | Location | Stationary? | Action |
|---|------|----------|-------------|--------|
| 1 | Amihud illiquidity | RiskManager.cpp:854-868 | Already fixed | none |
| 2 | Taleb cliff / Elder Chandelier | RiskManager.cpp:869-874 | Yes | none |
| 3 | Shannon entropy halt | RiskManager.cpp:875-880 | Yes | none |
| 4 | Taleb (Moors) kurtosis halt | RiskManager.cpp:881-886 | Yes | none |
| 5 | Spread stress | RiskManager.cpp:887-892 | Yes | none |
| 6 | Pareto top-state ratio (Hill-α proxy) | ExecutionGate.cpp:24-26 | Yes (new finding) | none (naming debt noted, deferred) |
| 7 | Shannon regime tenure | ExecutionGate.cpp:29-31 | Yes | none |
| 8 | Taleb-signal-sigma | ExecutionGate.cpp:34-36 | Yes | value drift fixed (Task 2) |

**Conclusion:** no gate in either function needs Amihud-style rolling-percentile
treatment. The only real defect this audit surfaced was the
`taleb_signal_sigma_threshold` value drift, fixed in Task 2.
