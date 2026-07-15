# Plan — Phase 1 Cutover: PositionManager → TripleBarrierExitManager

**Status:** PLAN — DRAFT FOR REVIEW (2026-07-14). Not yet implementing.
**Canonical spec:** `docs/ADR/triple_barrier_exit_engine_spec.md` (this plan is the execution sequencing for its Phase 1 static cutover).
**Scope:** C++ execution layer. **Schema-free** (reuses existing `PositionUpdate`/`TradeClose`/`TradeRecord.exit_reason`; see the schema-deferral analysis).
**Depends on:** Finding 1 fix (landed `097e11b`) and Elder 1.5R (landed `65d3f66`).

---

## 1. Current architecture (verified 2026-07-14)

The live exit system is **SC-native, bracket-order-driven + trailing**:

| Phase | Location | Behavior |
|---|---|---|
| Entry fill | `PositionManager.cpp:329-344` (`HandleFills`) | `ChandelierStopManager::InitializeStop(orderID, isLong, fillPrice, index, stopPrice, trailingMult)`; `trailingMult` = 3.0, or **4.0 in `PARETO_MOMENTUM`**. Stop/target placed as **SC attached bracket orders**. |
| T1 partial | `PositionManager.cpp:353-383` (scale-out branch) | On `newQty < oldQty`, if `ShouldPatternTrail(pattern)` → `ActivateTrailing(positionID)`. This is the **scale-out ladder** (T1 target fills, remainder trails). |
| Per tick | `PositionManager.cpp:626-776` (`UpdateChandelierStops`) | Only if `IsTrailingActive`. Computes `effectiveAtr = atr × dofStopScale × durationDecay × volumeStopScale`, `UpdateStop(...)`, a **defensive-mode `ForceTightenStop`** overlay, then `ModifyOrder` on the SC stop, then `ShouldStopOut` (log only — SC fills the exit). |
| Exit fill | `PositionManager.cpp:385-395` | `RemoveStop(parentOrderId)`; trade closed via `m_openTrade.Close`. |

**The actual exit fires when SC fills the attached stop/target order**, detected by `HandleFills`. `ShouldStopOut` is advisory logging only.

`ChandelierStopManager` is included at `PositionManager.cpp:3` and compiled via `CMakeLists.txt` (`src/ChandelierStopManager.cpp`).

## 2. Target architecture (Triple-Barrier, Phase 1 static)

Per the canonical spec's standing directives: **single-stage** (no scale-out), **immutable barriers** (no trailing), first-hit-wins `regime-invalidation → vertical(time) → lower(stop) → upper(target)`.

| Barrier | Mechanism (Phase 1) |
|---|---|
| **Lower (stop)** | Immutable SC attached stop order, placed at entry from `ComputeBarriers().stop`. Never modified. SC fills → `HandleFills` closes. |
| **Upper (target)** | Immutable SC attached target order, from `ComputeBarriers().target`. SC fills → `HandleFills` closes. |
| **Vertical (time)** | **NEW** C++ check each tick: `barsHeld >= ComputeBarriers().max_bars` → issue market close. |
| **Regime-invalidation** | **Phase 2** (formal `TripleBarrierEngine` kill-switch). In Phase 1, the already-wired `EvaluateRegimeDefense()` (Finding 1) is the interim regime-exit layer — no formal `REGIME_INVALIDATION` resolution yet. |

Net: stop/target stay SC-native (reliable, zero-latency fills); time-exit becomes a C++ market close; trailing + scale-out + Chandelier ATR machinery are **removed**.

---

## 3. Open decisions (resolve before coding)

**D1 — Barrier source: does `ComputeBarriers()` replace `CalculatePatternPrices()`?**
`CalculatePatternPrices()` (`PositionManagerPatterns.cpp`) computes per-pattern stop/target (Turtle Soup bar-low anchor, Pinball, Elder 1.5R, …). `tbe::ComputeBarriers()` also computes stop/target (per-tier seed → regime scale [0.5,2.0] → structural cap, tightening-only).
*Recommendation:* `ComputeBarriers()` becomes the **single authority** for the final stop/target/max_bars. `CalculatePatternPrices()`'s structural outputs (e.g., N-bar swing levels, bar low/high anchors) feed `BarrierInputs` as the **structural cap / seed**, not as the final bracket. This is exactly what the golden-vector `*_caps_binding/*_nonbinding` cases model. **Action:** map every field of `tbe::BarrierInputs` to a concrete live source (entry, ATR14, dof from `HmmState()`, regime via `ToRegime`, tier + pattern params from `kPatternTable[patternId]`, structural levels from the existing pattern math). Reconcile numerically against the 7 golden vectors on real replay data.

**D2 — Finding 17 (pattern-id routing).** Seed `patternId` from the §5.3 table by enum namespace; **fail-closed** (unknown/colliding id → reject entry or safe default, never silent mis-seed). `TripleBarrierExitManager::ToPatternId()` already maps `RaschkeTacticalTrigger`; verify all live entry patterns fall in `kPatternTable`'s valid range and add the fail-closed guard.

**D3 — Regime scaling overlap.** Chandelier widened the trailing stop in `PARETO_MOMENTUM` (4.0×). The Triple-Barrier engine already applies regime scaling `[0.5,2.0]` inside `ComputeBarriers()`. *Recommendation:* drop the ad-hoc `PARETO→4.0` entry hook; regime effects come solely from the engine (single source of truth).

**D4 — Time-exit market close mechanism.** Reuse a flatten path for the vertical barrier. `EmergencyFlattenPosition()` exists but is emergency-semantics (logs as emergency). *Recommendation:* add a neutral `ClosePositionAtMarket(sc, ExitReason::TIME_STOP)` (or parameterize the existing flatten) so `.btst` `TradeRecord.exit_reason` records `TIME_STOP` distinctly. Schema already has `TIME_STOP`.

**D5 — `EvaluateRegimeDefense` coexistence.** It currently also tightens stops via Chandelier (`ForceTightenStop`) and can flatten on hostile regime. After Chandelier removal, its stop-tightening calls are dead. *Recommendation:* keep its **flatten-on-hostile-regime** behavior (interim regime barrier), remove its Chandelier stop-tightening calls. Formal `REGIME_INVALIDATION` unifies this in Phase 2.

---

## 4. Implementation steps (each = own commit + green build)

1. **Wire barrier computation at entry** (additive, behind the existing path):
   - In `HandleFills` entry branch, build `tbe::BarrierInputs` (D1 mapping) and call `TripleBarrierExitManager::getInstance().OpenBracket(orderID, sc.Index, inputs)`.
   - Place the SC attached stop/target from `bracket.stop`/`bracket.target` (replacing the `InitializeStop` stop wiring). Keep Chandelier temporarily to A/B compare in logs.
2. **Add the vertical (time) barrier**: per-tick `Evaluate(sc.Index, close, regimeInvalidated=false)`; on `TIME_EXIT` → `ClosePositionAtMarket(TIME_STOP)` (D4).
3. **Finding 17 fail-closed routing** (D2) at entry seeding.
4. **Remove scale-out ladder** (single-stage): delete the `newQty < oldQty` trailing-activation branch (`PositionManager.cpp:353-383`); keep pure size bookkeeping. Full-size stop + target.
5. **Remove trailing machinery**: delete `UpdateChandelierStops()` body + its call at `:247`; delete the `EvaluateRegimeDefense` Chandelier stop-tightening calls (D5), keep hostile-regime flatten.
6. **Delete `ChandelierStopManager`**: remove `.h`/`.cpp`, the `#include` at `PositionManager.cpp:3`, the `InitializeStop`/`ActivateTrailing`/`RemoveStop` calls, and the `CMakeLists.txt` `SOURCE_FILES` entry. Confirm zero references (`grep`).
7. **Build green** (`./build_dll.sh`), then acceptance.

---

## 5. Acceptance gate

1. `./build_dll.sh` green; `grep -r Chandelier src include` returns only comments/none.
2. Golden-vector parity: entry barriers produced live match `tbe::ComputeBarriers` for representative patterns (spot-check against the 7 fixtures on replay).
3. Replay a session: confirm exits resolve as stop / target / **time** (new) and `.btst` `TradeRecord.exit_reason` is populated correctly (`STOP_LOSS`/`PROFIT_TARGET`/`TIME_STOP`).
4. First-hit-wins ordering holds (time before stop/target on the same tick if both would trigger — per §4.4).
5. **Phase 1 `omega_net` baseline** vs. the Chandelier baseline — **requires the Python daily-bias short-arm restoration first** for a trustworthy comparison (cross-repo dependency). Do not sign off Phase 1 production-ready until this is measured against the acceptance gates in `../docs/BACKTESTING_FRAMEWORK.md`.

---

## 6. Risk register

- **R1 (behavioral):** removing trailing/scale-out changes the P&L distribution materially. Mitigated by A/B logging (step 1) + the omega_net gate (§5.5).
- **R2 (parity):** `ComputeBarriers` vs. `CalculatePatternPrices` divergence on untested patterns. Mitigated by D1 field-mapping + fixture spot-check; fail-closed (D2) prevents silent mis-seed.
- **R3 (time-exit correctness):** `barsHeld` must use the same bar index base as entry (`m_openTrade.GetEntryIndex()`); verify across session boundaries.
- **R4 (regime double-exit):** ensure `EvaluateRegimeDefense` flatten and the (future Phase 2) `REGIME_INVALIDATION` don't both fire — Phase 1 keeps only the former.
- **R5 (schema):** none — Phase 1 is schema-free; the coordinated `REGIME_INVALIDATION` `ExitReason` bump is deferred to the Phase 2 both-sides change.
