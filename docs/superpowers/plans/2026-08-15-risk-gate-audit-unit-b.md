# Risk-Gate Stack Stationarity Audit (Unit B) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Produce a written stationarity finding for every fixed-threshold gate in `RiskManager::EvaluateHardGates()` and `ExecutionGate::EvaluateEmpiricalRegimeGates()`, and resolve the confirmed `taleb_signal_sigma_threshold` compiled-default/live-JSON value mismatch.

**Architecture:** This unit is primarily a documentation deliverable — a new ADR-style findings doc that cites `docs/ADR/amihud_gate_percentile_spec.md`'s already-completed audit verdicts for 6 of 8 gates, adds one new finding for a gate that ADR never explicitly covered, and records the confirmed config-drift bug. The one code change is a single-line default-value fix plus a grep-based regression guard — no new abstractions, no new config infrastructure (that is Unit C's job).

**Tech Stack:** C++17 (RiskManager.cpp), Markdown (ADR doc).

**Spec:** `docs/superpowers/specs/2026-08-15-risk-gate-context-cpp-coevolution.md` (Unit B). Prerequisite reading: `docs/ADR/amihud_gate_percentile_spec.md` §6 (the stationarity-audit methodology and its existing verdicts, which this plan reuses rather than re-derives).

## Global Constraints

- Do not modify any file under `../lbrnet` — that repo is a separate session boundary. lbrnet's own stale `taleb_signal_sigma_threshold` values (`backtest_runner.py`'s `9.636797` fallback, `models/HMMEmpiricalGateThresholds.json`'s `6.67559116507085`) are explicitly out of scope here; flag them in the findings doc as a cross-repo follow-up, don't fix them.
- Do not build a config-sync mechanism or new JSON schema here — Unit C (separate plan) owns the shared-config infrastructure that prevents this class of drift recurring. This plan only fixes today's single mismatched value.
- Reuse `docs/ADR/amihud_gate_percentile_spec.md`'s §6 verdicts verbatim (cite file:line) for the 6 gates it already covers — do not re-derive stationarity from scratch for those.
- `taleb_signal_sigma_threshold`'s correct current value is `1.8401` — this is the `hmm_regime_risk_policy.json`-live, P85.0-percentile-matched value from the 2026-08-13 rescale (documented at `src/RiskManager.cpp:68-73` and `hmm_regime_risk_policy.json`'s own `_taleb_signal_sigma_threshold_note`). This is the most recent, deliberate, methodology-consistent calibration in the MindfulTrader repo — more recent than lbrnet's 2026-07-26 figure — so it is authoritative for this fix.

---

### Task 1: Write the gate-stack stationarity audit findings document

**Files:**
- Create: `docs/ADR/gate_stack_stationarity_audit_findings.md`

**Interfaces:**
- Consumes: `docs/ADR/amihud_gate_percentile_spec.md` §6's existing verdicts (cited, not re-derived).
- Produces: the audit-trail artifact Unit B's acceptance criteria requires — "a written finding for every fixed-threshold gate in both `EvaluateHardGates()` and `EvaluateEmpiricalRegimeGates()`."

- [x] **Step 1: Write the findings document with this exact content**

```markdown
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
```

- [x] **Step 2: Commit**

```bash
git add docs/ADR/gate_stack_stationarity_audit_findings.md
git commit -m "docs: gate-stack stationarity audit findings (Unit B)"
```

---

### Task 2: Fix the `taleb_signal_sigma_threshold` compiled-default drift

**Files:**
- Modify: `src/RiskManager.cpp:74`
- Test: manual grep verification (see Step 2) + full build (see Step 3) — no standalone unit-test harness exists for `RiskManager.cpp` (it has ZMQ/Windows dependencies, unlike `FeatureScaler.h` which was specifically extracted to be dependency-free for native testing), so this task's verification is grep + full build, not a new test executable.

**Interfaces:**
- Consumes: nothing from Task 1 (independent code change; Task 1's doc references this task's outcome).
- Produces: `src/RiskManager.cpp:74`'s compiled default now reads `1.8401`, matching the live `hmm_regime_risk_policy.json` value exactly.

- [x] **Step 1: Make the one-line fix**

Read `src/RiskManager.cpp` around line 74 first to confirm the exact current line (comment context may have shifted), then change:

```cpp
double taleb_signal_sigma_threshold = 1.8382;
```
to:
```cpp
double taleb_signal_sigma_threshold = 1.8401;
```

Update the inline comment immediately above/beside it (already documents the 2026-08-13 rescale story per the earlier research) to note the compiled default now matches the live JSON exactly — do not remove the existing rescale-history comment, just correct the trailing value if it restates `1.8382` anywhere nearby.

- [x] **Step 2: Grep-verify no other MindfulTrader C++ site still hardcodes the stale value**

```bash
grep -rn "1\.8382" src/ include/ tools/
```
Expected: no output (or only output in a comment explicitly documenting the historical/superseded value, which is fine — confirm by reading any hit).

- [x] **Step 3: Full build verification**

```bash
./build_dll.sh --no-clean
```
Expected: build succeeds (per this repo's Done Checklist — `build_dll.sh` is the only sanctioned build entry point, never raw `cmake`/`ninja`).

- [x] **Step 4: Commit**

```bash
git add src/RiskManager.cpp
git commit -m "fix: sync compiled taleb_signal_sigma_threshold default to live JSON value (1.8401)"
```

---

## Self-Review Notes (for whoever executes this plan)

- Task 1 and Task 2 are independent — either can run first, or in parallel across two subagents, since Task 1 only *references* Task 2's outcome in prose (the findings doc says "fixed by Task 2"), it doesn't depend on Task 2's diff existing first. If Task 1 runs first, its Task 2 reference is still accurate since Task 2 is guaranteed to follow in this same plan.
- No wire schema change, no config infrastructure — confirmed out of scope per Global Constraints.
- Acceptance criteria from the spec (Unit B): "a written finding for every fixed-threshold gate in both functions" — Task 1 covers all 8. "a resolved, single-sourced value for `taleb_signal_sigma_threshold`" — Task 2 resolves the MindfulTrader-side mismatch; full single-sourcing (a mechanism that prevents *any* future drift, including lbrnet's) is Unit C's job, not this plan's — this plan only fixes today's concrete instance.
