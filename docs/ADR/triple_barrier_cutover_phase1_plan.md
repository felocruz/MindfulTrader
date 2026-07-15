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

**D1 — resolved live-source mapping (2026-07-15).** Wiring point: the entry-fill branch of `PositionManager::HandleFills` (`sc`, the fill, and `pos` are all in scope). Core parity is verified green — `tbe::ComputeBarriers()` passes all 7 golden vectors natively (`g++ -std=c++17 -I include tests/cpp/test_triple_barrier_parity.cpp`). Field map:

| `BarrierInputs` | Live source | Notes |
|---|---|---|
| `pattern_id` | `m_openTrade.GetPatternId()` → `ToPatternId()` | int; **Finding 17 fail-closed guard needed** (see below) |
| `is_long` | `pos.PositionQuantity > 0` | |
| `entry` | `latestFill.FillPrice` | |
| `bar_high` / `bar_low` | `sc.High[sc.Index]` / `sc.Low[sc.Index]` | TS3 15-min current bar |
| `prev_high` / `prev_low` | `sc.High[sc.Index-1]` / `sc.Low[sc.Index-1]` | Elder 2-bar stop; idx≥1 at entry |
| `atr10` | `m_cachedATR10` (`GetCachedATR10`) | fed by TS3 `Subgraph_AtrTemp3`; **0.0 until TS3 warm** → engine treats as absent |
| `dof_stop_scale` | `HmmState()->DofStopScale()` else `1.5` | DOF→scale already encapsulated |
| `regime_stop_width_scale` | `1.0` | Phase 1 identity (Phase 3 bounds to [0.5,2.0]) |
| `nbar_extreme_high` / `_low` | structural extreme from the per-pattern math (`std::max/min_element` over lookback, `PositionManagerPatterns.cpp`) | **PLUMBING GAP (see below)** |
| `swing_high` / `swing_low` | `IntermediateMarketAction::swingHigh()/swingLow()` (TS2 60-min) | 0.0 if not ready → absent (non-fatal) |
| `regime` | `ToRegime(HmmState()->Value())` | `HMM_NO_PRIOR` → `GAUSSIAN_STABLE` |
| `tick_size` | `sc.TickSize` | |

**Two items surfaced by the mapping (fold into D2 + the impl):**
1. **`nbar_extreme_*` plumbing gap.** The N-bar structural extreme *is* computed at signal time (e.g. `TURTLE_SOUP_BUY` 4-bar high, `PositionManagerPatterns.cpp:214`), but only as the pattern's final `targetPrice`, at order **placement** — not captured for the fill-time barrier build. Fix: **cache the structural extreme (and the structural swing levels) on the `Trade`/order at entry** so they are available in `HandleFills` to seed `nbar_extreme_*`. Do NOT recompute at fill (different `sc.Index`).
2. **`pattern_id` provenance (Finding 17).** `GetPatternId()` returns an int; the wiring must **fail-closed** — validate it is a live `RaschkeTacticalTrigger` in `kPatternTable`'s valid range (1..18); `0/NONE` or out-of-range → reject entry or safe default, never silent mis-seed.


**D2 — Finding 17 (pattern-id routing, fail-closed).**
*Context:* barriers protect an **already-filled** position (`HandleFills` is post-fill), so "reject the entry" is not available — the position exists and must be protected. `pattern_id = ToPatternId(m_openTrade.GetPatternId())` returns a raw int.
**Decision (recommended):** seed `patternId` from `GetPatternId()` and guard at the `OpenBracket` call:
- Valid iff `1 ≤ patternId ≤ 18` (a live `RaschkeTacticalTrigger` present in `kPatternTable`). `0/NONE` or out-of-range ⇒ **safe-default to LOW-tier generic params** (`Tier::LOW`, `stop_mult 0.5`, `target_r_mult 1.5`, regime `max_bars`) and emit a **WARN** with the raw id — never silent-mis-seed with another pattern's tier, and never leave the filled position unprotected.
- Escalate to an immediate flatten only if the id is *structurally* impossible (negative / ≫18), which signals a real upstream bug rather than a benign `NONE`.
- **Drift guard:** add a compile-time check that `static_cast<int>(RaschkeTacticalTrigger::<max>) == kPatternTable.size() - 1` (== 18) so the enum and the table cannot silently diverge.

**D3 — Regime scaling overlap.**
*Context:* the ad-hoc `PARETO_MOMENTUM → trailingMultiplier = 4.0` hook (`PositionManager.cpp:335-338`) is a **trailing-specific** widening, structurally incompatible with immutable barriers. The engine expresses regime effect via `MaxBarsForRegime` (PARETO = 12 bars — momentum resolves fast) and the Phase-3 `regime_stop_width_scale` `[0.5,2.0]` (Phase 1 = 1.0 identity).
**Decision (recommended):** **drop the `PARETO→4.0` hook** — the engine is the single source of truth for regime effect. Accept that in Phase 1 PARETO no longer widens the stop (intentional; the old 4.0× was a trailing hack). The A/B log (impl step 1) quantifies the P&L impact; if the data justifies a principled PARETO stop-width, re-introduce it **only** via the bounded `regime_stop_width_scale` when Phase 3 activates — never as an ad-hoc entry constant.

**D4 — Time-exit market close mechanism.**
*Context:* `.btst` exit reason = `MapExitReason(InferExitReason(trade, tickSize))`, which **infers from exit price vs stop/target** — a time-exit fills at an arbitrary price and would misclassify as `ExitReason_MANUAL`. `ExitReason_TIME_STOP = 3` already exists in the schema. `EmergencyFlattenPosition()` carries emergency logging semantics (wrong for a routine horizon exit).
**Decision (recommended):** add a **neutral `ClosePositionAtMarket(sc, exitTag)`** (same market-close order mechanics as `EmergencyFlattenPosition`, without the emergency logging) and record the reason **explicitly, not by price inference**:
- Set an explicit exit-reason tag on the `Trade` (e.g. `m_exitReasonTag = "TIME_STOP"`); have `InferExitReason()` **honor an explicit tag before falling back to price inference**.
- Add `if (reason == "TIME_STOP") return ExitReason_TIME_STOP;` to `MapExitReason`.
- Wire the vertical-barrier `TIME_EXIT` resolution → `ClosePositionAtMarket(TIME_STOP)`. Keep `EmergencyFlattenPosition` for genuine emergencies (Mahalanobis, tail-risk, disconnect) so `TIME_STOP` stays analytically distinct from `MANUAL`/emergency. **Schema-free** (enum already exists).

**D5 — `EvaluateRegimeDefense` coexistence.**
*Context:* `EvaluateRegimeDefense` (`PositionManagerPatterns.cpp:81`) flattens on hostile regime (`HOSTILE_REGIME_EXIT` :99, `TOXIC_ENV_EXIT` :164) via `EmergencyFlattenPosition`; the Chandelier `ForceTightenStop` path (`PositionManager.cpp:728`) lives in `UpdateChandelierStops` and dies with Chandelier removal.
**Decision (recommended):** **keep** the flatten-on-hostile-regime behavior as the Phase-1 **interim regime barrier** (informal `REGIME_INVALIDATION` until Phase 2 formalizes it); **remove** the now-dead Chandelier stop-tightening. Guard against double-exit (R4): Phase 1 keeps **only** `EvaluateRegimeDefense`; the formal engine `REGIME_INVALIDATION` stays disabled (`Evaluate(..., regimeInvalidated=false)`) until Phase 2. Its flatten maps to `ExitReason_MANUAL` in Phase 1 (acceptable); Phase 2's coordinated schema bump gives `REGIME_INVALIDATION` its own `ExitReason`.


---

## 4. Implementation steps (each = own commit + green build)

1. **Wire barrier computation at entry** (additive, behind the existing path). Split into two increments for safety:
   - **1a — Shadow (DONE, non-destructive).** In the `HandleFills` entry-fill branch, build `tbe::BarrierInputs` (D1 mapping) and call `tbe::ComputeBarriers()`; log `[TB-SHADOW]` comparing engine `stop/target/maxBars/rr` against the live `Trade` stop/target. Drives **no orders**, latches no state, exception-guarded. Purpose: validate the D1 mapping + barrier math numerically on replay (acceptance §5.2) with zero behavior risk. D2 fail-closed default (`NONE`/OOR → LOW-tier) is exercised here.
   - **1b — Engine-driven (pending 1a log review).** Switch to `TripleBarrierExitManager::getInstance().OpenBracket(orderID, sc.Index, inputs)` and place the SC attached stop/target from `bracket.stop`/`bracket.target` (replacing the `InitializeStop` stop wiring). Keep Chandelier temporarily to A/B compare in logs.
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
