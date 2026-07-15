# Plan — Phase 1 Cutover: PositionManager → TripleBarrierExitManager

**Status:** PLAN — D1–D5 resolved; step 1a (shadow) implemented (`5d52f23`). Steps 1b–7 pending sign-off.
**Canonical spec:** `docs/ADR/triple_barrier_exit_engine_spec.md` (this plan is the execution sequencing for its Phase 1 static cutover).
**Scope:** C++ execution layer. **Schema-free** (reuses existing `PositionUpdate`/`TradeClose`/`TradeRecord.exit_reason`; see the schema-deferral analysis).
**Depends on:** Finding 1 fix (landed `097e11b`) and Elder 1.5R (landed `65d3f66`).

> **Deployment context (2026-07-15):** the system is **not in production yet — this version is the first production deploy.** There is no production incumbent to preserve. Consequences for this plan: (a) the end state ships **engine-only** — do **not** deploy a dual Chandelier+engine system; the A/B shadow (step 1a) is a **dev validation** tool, time-boxed, not a production hedge; (b) acceptance is measured against **absolute** institutional gates (`../docs/BACKTESTING_FRAMEWORK.md`), not "better than Chandelier" (Chandelier was never in production); (c) once the shadow logs validate the D1 mapping on replay, proceed decisively through 1b→7 to the clean end-state before first deploy.

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

### 1.1 Correction (2026-07-15) — the live exit is richer than §1 stated; a doctrine decision is implied

A full surface map (order-assembly block `PositionManager.cpp:2418-2507`) shows §1 **understated** the current system. It is not "bracket + advisory Chandelier" — it is a deliberate multi-stage **"let winners run"** exit (GAP 18 v2, Elder/Raschke):

- **3-level scale-out ladder** — `CalculateScaleOutTargets()` splits T1 (50%) / T2 (30%) / T3 runner (20%) [size≥3; size=2 → T1+runner; size=1 → pure runner]. Targets set **once** at entry (no re-sizing on partial fills).
- **SC server-side trailing** — the **normal-regime stop** is `SCT_ORDERTYPE_TRIGGERED_TRAILING_STOP_LIMIT_3_OFFSETS` (trails autonomously server-side), and the **last (runner) target** is *also* a server-side trailing type. Trail distance = `ATR14 × DofStopScale`.
- **Regime-aware stop TYPE** (`2418-2438`): crash → `STOP_WITH_BID_ASK_TRIGGERING` (market), toxic → `STOP_LIMIT` (2-tick), normal → server trailing. (Now consumes the Phase-A `amihudPercentile`.)
- **Server-side move-to-breakeven** on T1 fill (`MoveToBreakEven`, OCO-triggered, BE+1 tick).
- **Chandelier = advisory overlay** — `ModifyOrder`s the stop every tick on top of the server-side trailing.

**Two consequences:**

1. **DOCTRINE DECISION (blocks the cutover; needs sign-off).** Triple-Barrier is **single-stage, immutable, first-touch** (no scale-out, no runner, no trailing, no move-to-BE). Cutover therefore **abandons the "every trade gets a runner / let winners run" doctrine** for a fixed-target first-touch doctrine. This is a strategy change, not a mechanical refactor — decide explicitly:
   - **(A) Full single-stage immutable** (spec as written): one full-size target at `ComputeBarriers().target`, one immutable stop, time barrier. Simplest, matches the label-consistency argument with the Python triple-barrier labeler, but drops the runner alpha.
   - **(B) Hybrid**: immutable **stop** + **time** barrier from the engine, but **retain the scale-out ladder / runner target** on the upside. Keeps "let winners run"; the engine owns the downside + horizon only. Diverges from the pure spec and from the Python labeler's single-target assumption.
   - Recommendation: decide against the **Python triple-barrier labeler's** target definition — if training labels are single-target first-touch, live must match (A) for train/live parity; if the labeler supports a runner/ladder, (B) is viable.

2. **Latent double-control bug (today).** The normal-regime stop is a SC server-side trailing order **and** Chandelier `ModifyOrder`s that same stop each tick — two controllers, two different trail formulas (`ATR14×DofStopScale` server vs `trailingMult 3.0/4.0` Chandelier). Whichever cutover option is chosen removes the Chandelier controller and resolves this.

Remaining §2-§6 below assume **option (A)** (the spec's single-stage design); revise if (B) is chosen.

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
5. **Absolute acceptance (pre-production).** The Triple-Barrier system must meet the acceptance gates in `../docs/BACKTESTING_FRAMEWORK.md` **in absolute terms** on replay — this is the first production deploy, so the bar is "institutionally sound," not "better than the never-shipped Chandelier." The Chandelier A/B log (step 1a shadow) is used only to sanity-check that the engine barriers are reasonable and to catch gross regressions during dev; it is **not** a production gate and the dual system is not shipped. The lbrnet daily-bias short-arm restoration is still required for a trustworthy `omega_net` reading, but as an input to the absolute gate, not an A/B baseline.

---

## 6. Risk register

- **R1 (behavioral):** removing trailing/scale-out changes the P&L distribution materially. **Pre-production, this is not a regression risk** (no incumbent) but a design-correctness one — validated by the shadow log (step 1a) + absolute acceptance gate (§5.5), not by A/B superiority over Chandelier.
- **R2 (parity):** `ComputeBarriers` vs. `CalculatePatternPrices` divergence on untested patterns. Mitigated by D1 field-mapping + fixture spot-check; fail-closed (D2) prevents silent mis-seed.
- **R3 (time-exit correctness):** `barsHeld` must use the same bar index base as entry (`m_openTrade.GetEntryIndex()`); verify across session boundaries.
- **R4 (regime double-exit):** ensure `EvaluateRegimeDefense` flatten and the (future Phase 2) `REGIME_INVALIDATION` don't both fire — Phase 1 keeps only the former.
- **R5 (schema):** none — Phase 1 is schema-free; the coordinated `REGIME_INVALIDATION` `ExitReason` bump is deferred to the Phase 2 both-sides change.
