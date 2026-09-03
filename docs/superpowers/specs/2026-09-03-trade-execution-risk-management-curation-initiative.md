# Trade Execution / Risk Management Curation — Institutional Methodology

**Status, opened 2026-09-03: seeded from prior closed audits + the currently-queued NEXT MAJOR
INITIATIVE. Not yet actively worked — this doc exists so that work has a living home the moment the
observation-vector/ContextManager thread closes out, per `CLAUDE.md`/`GEMINI.md`/`PRODUCTION_TRIAGE.md`'s
own STANDING PRIORITY callout (2026-09-03).**

## 0. Origin and mandate

Direct sibling of `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` (the
observation-vector ledger) — same methodology, same discipline, different layer. That initiative asked
"is each dimension feeding the HMM built on solid, literature-grounded, real-data-validated math?" This
one asks the equivalent question one layer downstream: **is the data `RiskManager`/`ExecutionGate`/
`PositionManager` actually gate on — `LocalRiskContext`/`RiskGateContext` — solid, non-redundant, and
not blind to signal that already exists elsewhere in the system?**

**Founding question (operator directive, 2026-09-03, recorded in `CLAUDE.md`'s own North Star section):**
the HMM exists specifically to detect fat tails/regime shifts. Does its own inferred output (`.regime`,
`ν_k`, state probabilities) feed back into the execution-layer risk context as a genuine "heads up" for
the Predator, or does `RiskManager`'s own hard-gate layer fire on `LocalRiskContext` fields that were
frozen before the HMM ever ran? This is the same "solidify the data a downstream layer actually
consumes" discipline just applied to the observation vector, one hop further downstream.

**No exact predecessor ledger existed for this layer** (checked 2026-09-03, not assumed) — the closest
prior work is `docs/ADR/gate_stack_stationarity_audit_findings.md` (CLOSED 2026-08-15, a similar
per-gate table but a point-in-time stationarity audit, not a living ledger) and
`docs/superpowers/specs/2026-08-15-risk-gate-context-cpp-coevolution.md` (Units A/B/C, partially
implemented — Unit B fully done, Unit C's shared-config work status needs re-verification before
trusting it's still current). This doc is the new living home going forward; it does not replace either
as a historical record.

## 1. Status vocabulary

Same as the observation-vector ledger, for consistency: **IN** (settled, stays as-is) · **IN-WEAK**
(stays, weak, no better alternative identified) · **IN-PENDING-FIX** (stays, a decided implementation
change not yet done) · **PAUSED** (groundwork exists, explicitly do not proceed without new evidence) ·
**CANDIDATE** (proposed, not yet built) · **OPEN** (actively being investigated) · **BLOCKED** (real
work identified, blocked on something else finishing first).

## 2. The active thread: HMM signal blindness (founding question, not yet started)

| # | Item | Status | Why | Last verified |
|---|---|---|---|---|
| 1 | `LocalRiskContext` vs. `PredatorContext`'s HMM visibility | **OPEN** | `PredatorContext` (a higher-level fusion struct, `ContextManager::GetPredatorContext()`) already carries `.regime` from the HMM's own inferred state. But `RiskManager::EvaluateHardGates()` fires directly on `LocalRiskContext` — need to verify whether ANY of its 8 hard gates (see §3 below) ever see HMM output, or whether every one of them is blind to regime/ν_k entirely, deciding only from raw pre-HMM features. Not yet traced. | 2026-09-03 (queued, not started) |
| 2 | EVT/GPD "distance to the tail, closing how fast" execution signal | **CANDIDATE** | Discussed 2026-09-03 (`lbrnet/logs/rc_gemini.log` context around `CLAUDE_BRIEF_123`) as a concrete first deliverable for item 1 — reuse this codebase's own existing GPD-fitting machinery (currently only used to derive *static* `FeatureScaler` bounds) to produce a *live* signal: a reading's estimated exceedance probability against its own fitted tail, plus its short-term rate of change. Deliberately scoped to feed `RiskGateContext` directly, NOT the HMM's own observation vector — avoiding the `tail_index` redundancy trap already learned this session (a Hill-estimator dim was dropped from the observation vector as structurally redundant with the model's own ν_k; the same redundancy risk applies here if this signal were fed back INTO the model instead of to the execution layer). | 2026-09-03 (idea only, not designed) |

## 3. Seed material: the 8 gates already audited (carried forward from the closed 2026-08-15 audit)

`docs/ADR/gate_stack_stationarity_audit_findings.md`'s own verdicts, reproduced here as the living
ledger's starting point — that audit asked "is each gate's raw signal stationary enough for a fixed
threshold, or does it need Amihud-style rolling-percentile treatment." **Important scope note**: that
audit did NOT ask item 1's question above (HMM-signal visibility) — it's a different axis entirely
(stationarity vs. regime-awareness), so a gate being "stationary, no treatment needed" there says
nothing about whether it's also blind to HMM regime state.

| # | Gate | Location | Stationarity (2026-08-15) | HMM-signal-aware? (2026-09-03, NOT YET CHECKED) |
|---|---|---|---|---|
| 1 | Amihud illiquidity veto | `RiskManager.cpp:854-868` | Already fixed (session-aware rolling percentile) | Not yet checked |
| 2 | Taleb cliff / Elder Chandelier | `RiskManager.cpp:869-874` | Stationary, no treatment needed | Not yet checked |
| 3 | Shannon entropy halt | `RiskManager.cpp:875-880` | Stationary, no treatment needed | Not yet checked |
| 4 | Taleb (Moors) kurtosis halt | `RiskManager.cpp:881-886` | Stationary, no treatment needed | Not yet checked |
| 5 | Spread stress (`liq_fragility`) | `RiskManager.cpp:887-892` | Stationary, no treatment needed | **Note: `liq_fragility`'s own formula was reformulated 2026-09-03 (`a76ec00`) — this gate's `>0.85f`/`>0.70f` thresholds were deliberately preserved unchanged (same bounded [0,1] output shape), but the underlying signal's real-data distribution shifted (old: compressed 0.0-0.35 "normal" band; new: uses the full [0,1] range, p1=0.175/median=0.502/p99=0.855). The absolute thresholds surviving the reformulation was a design goal, not yet independently re-verified against the new distribution's own percentiles post-fix.** |
| 6 | Pareto top-state ratio (Hill-α proxy) | `ExecutionGate.cpp:24-26` | Stationary (naming debt noted, deferred) | Not yet checked |
| 7 | Shannon regime tenure | `ExecutionGate.cpp:29-31` | Stationary, no treatment needed | Not yet checked |
| 8 | Taleb-signal-sigma | `ExecutionGate.cpp:34-36` | Stationary; value-drift bug fixed (Task 2, that plan) | Not yet checked |

## 4. Other cached background material (reference, not yet triaged into this ledger)

- **`docs/ADR/execution_correctness_findings_spec.md`** — 12 verified correctness/parity findings
  across `PositionManager`/`RiskManager`/`ChandelierStopManager`/`Scoring`/`ExecutionGate` (2026-07-10
  audit). Finding 1 (`UpdateContext()` never called) RESOLVED; Finding 12 is a Python-port parity gap,
  not a C++ fix. Not yet cross-referenced against this ledger's own items — do that before assuming
  either doc is complete on its own.
- **`docs/superpowers/specs/2026-08-15-risk-gate-context-cpp-coevolution.md`** — Units A (close the
  `risk_gate_context` population gap), B (gate-stack stationarity audit, →§3 above), C (shared,
  git-tracked calibration config for `FeatureScaler`'s winsorization bounds). Unit B fully done; Units A
  and C's actual completion status needs re-verification (the spec's own plans show detailed
  implementation steps, but this session hasn't confirmed they were executed and committed) before this
  ledger treats them as closed.
- **`docs/ADR/amihud_gate_percentile_spec.md`** — the original precedent both this doc and the
  observation-vector ledger's methodology descend from (canonical formula fix + session-aware rolling
  percentile + honest rename, `vpin`→`amihud_illiquidity`). Fully implemented C++-side; `lbrnet`-side
  co-evolution (§4b of that spec) was still open as of that doc's own last update — cross-repo, not
  fixed from here.
- **`../../docs/RISK_MANAGEMENT_SYSTEM.md`** / **`../../docs/TRADE_EXECUTION_SYSTEM.md`** — the
  canonical architecture/governance references for this whole layer (not living ledgers, static
  documentation) — read these for how the gates in §3 fit into the broader daily-loss-limit/Kelly-sizing/
  trade-lifecycle picture before making any change here.

## 5. Immediate next action

Once the observation-vector/`ContextManager` thread closes out: trace item 1 in §2 (does any existing
`RiskManager`/`ExecutionGate` hard gate already see HMM regime state, or are all 8 gates in §3 blind to
it) before designing item 2's EVT/GPD signal — confirming the actual gap empirically, not assuming it,
matches this initiative's own founding discipline.
