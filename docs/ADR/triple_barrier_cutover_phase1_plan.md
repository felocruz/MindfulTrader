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

   **RESOLVED (2026-07-15) → Option (A), grounded in the LITERATURE (not "Python is authoritative").** Under the co-evolution principle neither source is authoritative by default; the tie-breaker is the institutional literature. Consulting it:
   - **López de Prado, *Advances in Financial ML* (2018), Ch. 3** — the Triple-Barrier Method is *by construction* a **bounded, first-hit-wins race between three finite barriers** (upper profit-take / lower stop / vertical time), volatility-scaled. There is **no fourth "let it run indefinitely" state** — the uncapped runner is **not a triple-barrier concept**. (Corroborated by the mlfinlab reference implementation, the reasonabledeviations AFML notes, and the workspace KB `lbrnet/knowledge/global/execution/triple_barrier_method.md`, which states any runner extension is "a deliberate departure from the textbook method, not a variant of it.")
   - **Adjudication:** the Python labeler (single-target, first-hit) *correctly implements the textbook*; the current **C++ live path (scale-out ladder + uncapped runner + trailing) is the one in error** — it is an ungrounded doctrine *and* breaks train/live parity with the labels the model learns. → **Option (A).**
   - **Where the "let winners run" alpha correctly belongs (successor literature), so it is not lost:** *not* as multiple profit targets in the exit, but as a **separate, deliberately-grounded layer**:
     1. **Meta-labeling → bet sizing** (AFML Ch. 10; "meta-labelling learns the *size* of the bet after the side is known"). Express conviction as larger **position size** with a single first-touch exit — preserves label parity. The existing risk-gate stack is already a meta-labeling layer in spirit (`gate_threshold_coevolution.md`).
     2. **Trend-scanning labels** (López de Prado's own successor labeling method, mlfinlab) — if the desk wants to *systematically* harvest trends/runners, **re-label with trend-scanning** and match the live exit to trend-scan semantics, rather than bolting a ladder onto triple-barrier labels.
   - **Keep** the codebase's legitimate method-improvements: High/Low-based MFE/MAE and optional tick-level `.scid` path resolution (more path granularity, in the method's spirit).
   - **Net:** ship **(A)** now (single full-size target = the labeler's T1-tier, immutable stop, time barrier, first-hit-wins). Treat runner alpha as a **future, separately-grounded** layer (meta-label sizing and/or trend-scanning) — **never** an ad-hoc scale-out on top of first-touch labels.

   **GEMINI ADJUDICATION — CONFIRMED (2026-07-14, `gemini_adjudication_doctrine.md`).** Independent sovereign ruling agrees: **Option A governs**; the live scale-out+trailing+uncapped-runner path is "structurally in error." Key enrichments now folded in:
   - **The single mathematically-sound divergence condition is meta-labeling** (AFML Ch. 3.6): live may execute a complex path *only if* a secondary model is trained on the **actual realized P&L of that complex path**. Absent that, any exit ≠ the labeled single-stage bracket violates the OOS/IID assumptions. → we do **not** diverge in Phase 1.
   - **Runner-alpha ranking (for this single-instrument futures book):** **Rank 1 — Trend-Scanning labels** (López de Prado, *Machine Learning for Asset Managers* Ch. 5): replace the horizontal target with an adaptive trend-significance exit; **Rank 2 — dynamic regime-scale target multiplier at ENTRY** (scale `target_r_mult` by Hurst/entropy/HMM state; parity-preserved because the scaling is identical train+live); **Rank 3 — meta-label → bet sizing** (scales risk units, not exit geometry). This **refines D3**: the PARETO "let it run" intent is not abandoned — it moves from an ad-hoc trailing widen to the **parity-preserving entry-time target scaling** (Phase 3), the engine's `regime_stop_width_scale` being the stop-side analog.
   - **Literature improvements ranked:** Trend-Scanning (highly practical, backtestable) > OU optimal-stopping (Bertram 2010 / AFML Ch. 13 — excellent for *fades*, param-estimation risk) > RL exits (**unusable for parity** — overfit, fragile sim).
   - **Sequential-trading selection bias (AFML Ch. 4):** one-position-at-a-time → concurrency = 1 (no sample-uniqueness weighting needed) **but** a poorly-modeled exit corrupts the entry-eligibility of subsequent signals → strict deterministic single-stage exits are *required* for chronological backtest integrity. Another argument for (A).
   - **Dual-controller stop = critical defect** (confirmed): server-side trailing + per-tick DLL modifier on one order → race conditions, execution orphans (orphaned stop = infinite tail risk), SSOT violation. `TripleBarrierExitManager` must be the **sole** controller.
   - **Cross-refs:** Verdict 1 (Elder 2.0R→1.5R) is **already resolved** in code (`65d3f66`; Gemini's audit was pre-fix). Verdict 3 (Finding-17 `ITR_FADE 13/14` ≡ `HOLY_GRAIL 13/14`) is a **confirmed active collision** → hardens D2's fail-closed guard (not hypothetical).

2. **Latent double-control bug (today).** The normal-regime stop is a SC server-side trailing order **and** Chandelier `ModifyOrder`s that same stop each tick — two controllers, two different trail formulas (`ATR14×DofStopScale` server vs `trailingMult 3.0/4.0` Chandelier). Whichever cutover option is chosen removes the Chandelier controller and resolves this.

3. **Structural (conditions/gates) barrier — GEMINI-RULED (2026-07-15, `triple_barrier_favorable_exit_ruling.md`), literature-grounded, both-sides.** The "close on conditions" barrier is `StructureTest`-based, not a generic HMM-regime bool. Rulings (each grounded independently, incumbent-labeler NOT authoritative):
   - **`FAVORABLE_EXIT` → REMOVE FROM BOTH REPOS.** Exiting a *winner* early on a favorable structural reversal truncates the positive-skew fade edge and injects path-dependent non-stationarity into *primary* labels; no literature supports path-dependent profit-taking in primary labels (AFML: static/unconditional barriers; trailing belongs in meta-labeling/bet-sizing). **lbrnet TODO:** delete `FAVORABLE_EXIT` from `triple_barrier_scanner.py` + its schema fields (`exit_horizon_bars`, FAVORABLE_* in `mts_schema.fbs`) — re-label/retrain-affecting. **C++:** never add it to `tbe::Resolution`.
   - **`TRAP_CANDIDATE` (adverse-T₁) → KEEP**, priority #1 (ahead of the hard stop), grounded in optimal-stopping / drift-violation (Bertram 2010). **Hard constraint: evaluate on the COMPLETED bar (`sc.Index-1`), never intra-bar** (intra-bar → whipsaw, and it diverges from the labeler which uses completed bars).
   - **`StructureTest` parity fix (CRITICAL):** C++ `DetectStructure()` reads `sc.Close[sc.Index]` intra-bar; the labeler uses completed bars. The Phase-2 barrier check must use the completed bar, and the `lookbackHigh/Low` period must be mapped identically C++↔Python. `0.5·ATR` buffer is valid (structural-break significance) **only** on completed bars.
   - **This is Phase-2 scope** — `TRAP`/`FAVORABLE` are not in the C++ engine yet; the ruling locks the Phase-2 design and mandates the Python removal. It does **not** affect the Phase-1 Step C (stop/target/scale-out).
   - **Citation caveats (verdicts unaffected):** Bertram 2010's real title is "Analytic solutions for optimal statistical arbitrage trading" (Gemini mis-titled it); Bai & Perron (1998, *Econometrica*) is applied by analogy. Gemini correctly retracted the hallucinated "Zhang 2020" and the misapplied Spooner RL citations. Authoritative matrix: LdP AFML 2018, LdP MLAM 2020 (trend-scanning), Bertram 2010, Bai-Perron 1998.

Remaining §2-§6 below assume **option (A)** (the spec's single-stage design); revise if (B) is chosen.

4. **Winner give-back / profit protection — GEMINI-RULED (2026-07-15, `triple_barrier_profit_protection_ruling.md`).** A profitable winner *can* revert to a full loss under first-touch (smooth reversal, no structural break, within `max_bars`). Rulings, decided on merits (labeler is refactored to follow, so it is NOT a constraint; the only enduring constraint is the rule be labeler-reproducible — a deterministic path-function barrier or a trained meta-model):
   - **Exit objective is PATTERN-CLASS-DIFFERENTIATED:** **fades (Turtle Soup, Momentum Pinball) → fixed-target first-touch** (mean-reverting; trend-scanning them is a category error); **breakouts (Elder) → trend-scanning (LdP MLAM Ch. 5)** eventually (a static 1.5R cap truncates the trend right tail). Phase 1 ships **pure first-touch for all** as the control group; the breakout→trend-scan differentiation is a **data-gated Phase 3/4 upgrade**.
   - **Naive breakeven / trailing profit-lock → PERMANENTLY REJECTED** (Rank 5): on 0.4–0.5·ATR fade stops, a BE move after ~+1R sits in the max-noise density → whipsaw; destroys expectancy. Profit protection only helps at **MFE ≥ ~2.0–2.5R** (beyond 1·ATR) *and* in fragile/coiled regimes.
   - **Protection order (if the data justifies it):** ① differentiated objectives (breakout trend-scan) → ② meta-label exit model (conditional on MFE≥1.0R) → ③ accept give-back (control) → ④ deterministic time-decay barrier. Never ⑤ (BE/trailing).
   - **Sequencing:** ship pure first-touch → **measure** → upgrade only on statistically-significant evidence.
   - **Q5 gating metrics (added to §5 acceptance):** `W_LR` (winner→loser reversal rate; <0.20 ⇒ ignore give-back), `W_SR` (whipsaw ratio; >0.30 ⇒ reject any BE), `MFE_CR` (MFE capture ratio), `ΔΩ` (Omega/Sharpe delta; must be significant at 95% or reject the added complexity). `.btst` already records MAE/MFE.
   - Citation caveat: the time-decay barrier is loosely attributed to Bertram 2010 (Rank 4, not decision-critical).

5. **TRAP definition & two-layer exit — GEMINI-RULED (2026-07-15, `triple_barrier_trap_definition_ruling.md`).** TRAP is elevated to Phase 1 as the one early exit consistent with first-touch (a genuine *invalidation*, not profit protection).
   - **Q0 SPLIT (confirmed):** TRAP = sprung-trap reversal tests ONLY (`StructureTest` `FAILED_LOW/HIGH_CLOSE_INSIDE`, `FAILED_LOW/HIGH_STRONG_REVERSAL`). Adverse `DECISIVE_BREAKDOWN_LOW` (long) / `DECISIVE_BREAKOUT_HIGH` (short) → a **separate `REGIME_INVALIDATION`** resolution (already in `tbe::Resolution`), never TRAP. Rationale: mixing mean-reverting reversal (low vol/low Hurst) with momentum counter-break (high vol/high Hurst) = multimodal target that degrades the anticipatory model.
   - **Q1 τ\* dynamic Bayesian gate (Elkan 2001):** act on model `TRAP_*` iff `p ≥ τ* = C_FP/(C_FP+C_FN)`, `C_FP=|target−price|`, `C_FN=|price−stop|` (from entry-latched barriers + current price → reproducible). For tight-stop fades τ* ≈ 0.68–0.92 (rises near the stop → let the stop work). Caveats: C_FP assumes full-target forgone (biases τ* high = conservative, OK); deploy the anticipatory override only when out-of-sample `F_0.25 > 0.65` (β calibrated from cost ratio, not hardcoded).
   - **Q2 (my position, unchallenged):** Phase 1 = TRAP EXIT/risk only; TRAP-as-entry deferred.
   - **Q3 (my position + Gemini deploy-gate):** native completed-bar `StructureTest` floor is always-on/authoritative (parity anchor); model τ* exit is additive, acting only when fresh ∧ p≥τ* ∧ adverse; native governs when Python is stale/down/disagreeing; model may LEAD but never SUPPRESS the floor.
   - **Two-layer architecture:** reactive native floor (C++ sees the trap on the completed bar) + anticipatory model (Python forecasts it earlier from training). Both resolve to one structural truth; the native `StructureTest` def == labeler TRAP def (reproducibility).
   - **Intra-bar timing (ruling 2026-07-15):** we do NOT wait for the 15-min close to act. The reactive floor is **completed-bar** (parity + the model's training anchor — a reversal is close-defined, so it has irreducible 1-bar confirmation lag). Live intra-bar responsiveness is carried by **τ\* recomputed EVERY TICK vs current price** over the standing (completed-bar-trained) model `p`; the exit fires the instant `p ≥ τ*`. Grounded in sequential/quickest-detection (Wald SPRT 1945; Shiryaev disorder problem) + early-classification of time series (Dachraoui/Bondu/Cornuéjols 2015 ECML-PKDD; Mori et al. 2017 IEEE TNNLS): the completed-bar label is the terminal value of an intra-bar posterior (Doob martingale) — act when it crosses the cost boundary, don't wait. **Phase 1 = Option A** (bar-gated `p` + per-tick τ*). **Option B (intra-bar RE-INFERENCE — `p` updates within the bar) is DEFERRED**: because training is completed-bar (below), feeding partial-bar features intra-bar is train/live OOD; enabling it requires ECTS-style training on intra-bar prefixes (train on partial bars labeled with the eventual completed-bar outcome). Intraday-pattern microstructure (Admati–Pfleiderer 1988; Harris 1986) supports late-bar confirmation reliability.
   - **Training (Option 1, confirmed):** labeler defines TRAP on completed bars; the model trains on that completed-bar label (from streaming inputs it learns to nowcast). Keeps the labeler the clean parity anchor.
   - **Co-evolution changes:** C++ = native `StructureTest` TRAP floor (reversal set) + τ*-gated model exit → `TRAP` resolution; route adverse `DECISIVE_*` → `REGIME_INVALIDATION`. Labeler (`triple_barrier_scanner.py`) = remove `DECISIVE_*` from `_T1_LONG_IDS`/`_T1_SHORT_IDS`, route to a new `REGIME_INVALIDATION` outcome. Doctrine mirrored to the 4 sync docs.
   - Citations verified: Elkan 2001 (IJCAI) real & apt; Raschke/Connors *Street Smarts* 1995 real; LdP real. Nit: Sperandeo volume/year muddled (`Trader Vic` 1991 vs `Trader Vic II` 1994) — source genuine.

## 2. Target architecture (Triple-Barrier, Phase 1 static)

Per the canonical spec's standing directives: **single-stage** (no scale-out), **immutable barriers** (no trailing), first-hit-wins `regime-invalidation → vertical(time) → lower(stop) → upper(target)`.

| Barrier | Mechanism (Phase 1) |
|---|---|
| **Lower (stop)** | Immutable SC attached stop order, placed at entry from `ComputeBarriers().stop`. Never modified. SC fills → `HandleFills` closes. |
| **Upper (target)** | Immutable SC attached target order, from `ComputeBarriers().target`. SC fills → `HandleFills` closes. |
| **Vertical (time)** | **NEW** C++ check each tick: `barsHeld >= ComputeBarriers().max_bars` → issue market close. |
| **Regime-invalidation** | **Phase 2** — and it is NOT a generic HMM kill-switch. Per `gemini_adjudication_doctrine.md` + `triple_barrier_gates_parity_adjudication.md` (verified against code), the labeler resolves TWO `StructureTest`-based barriers: **adverse-T₁ → `TRAP_CANDIDATE`** (priority #1, ahead of the stop) and **favorable-T₁ → `FAVORABLE_EXIT`** (mfe ≥ 0.5R). C++ already computes `StructureTest` (`DetectStructure`, `StudyHelperFunctions.cpp:1559`, 0.5×ATR reversal buffer) but does NOT wire it into the engine. Phase 2 must feed `StructureTest` into `TripleBarrierExitManager::Evaluate()` natively (replacing the generic `regimeInvalidated` bool), evaluated on the **completed bar** (`sc.Index-1`, not live intra-bar `sc.Close[sc.Index]` — Trap 1) with lookback params mirrored to lbrnet (Trap 2), + a coordinated `ExitReason` bump (`TRAP_CANDIDATE_EXIT`, `FAVORABLE_STRUCTURAL_EXIT`). In Phase 1, `EvaluateRegimeDefense()` remains only as a **catastrophic-safety override** (D5), not the labeled structural barrier. |

Net: stop/target stay SC-native (reliable, zero-latency fills); time-exit becomes a C++ market close; trailing + scale-out + Chandelier ATR machinery are **removed**.

---

## 3. Open decisions (resolve before coding)

> **STATUS (2026-07-15): D1–D5 all RESOLVED and IMPLEMENTED** — the text below is the as-built record, not open questions. D1 live-source mapping is realized in `BuildBarrierInputs()`; D2 Finding-17 fail-closed lives in that same helper (invalid/out-of-range pattern → LOW-tier default); D3 the `PARETO→4.0` trailing hook was dropped (engine is the single regime authority); D4 `ClosePositionAtMarket()` + explicit `TIME_STOP` tag + `MapExitReason` case shipped; D5 hostile-regime flatten retained as a wide catastrophic override (climate-shift/hostile-env demoted to telemetry when trailing was deleted), Chandelier stop-tightening removed. Commits: Step C `2eb658e`, stale-fish + Step D `9ee5326`, native TRAP floor `711e7cf`.

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

**GEMINI GATES-PARITY ADJUDICATION — CONFIRMED (2026-07-14, `triple_barrier_gates_parity_adjudication.md`; code-verified).** The Phase-1 `EvaluateRegimeDefense()` / `EnforceHardGates()` in-position flattens are NOT the labeler's structural barriers, so they are a train/live divergence. Ruling = **Option B**: keep them **only as catastrophic-safety overrides** (black-swan parachutes), with thresholds set **wide enough that they never fire under normal conditions** — preserving ~normal-path parity while retaining tail-risk protection. Do NOT let them stand in for the labeled `TRAP_CANDIDATE`/`FAVORABLE_EXIT` exits (that is the Phase-2 `StructureTest` wiring, above). `FAVORABLE_EXIT` (harvest a winner on favorable structural exhaustion) is legitimate but must be modeled via the **meta-labeling / bet-sizing** layer (primary model stays on clean binary first-touch labels; the meta-labeler learns success of the realized structural-exit path — AFML Ch. 3.6), never by corrupting the primary labels. Both structural barriers must use the **same 0.5×ATR reversal buffer + lookback params** on both sides (verified: C++ `DetectStructure` uses `0.5*atr`).


---

## 4. Implementation steps (each = own commit + green build)

> **STATUS (2026-07-15): steps 1–6 LANDED; step 7 build green, acceptance (§5) pending a replay pass.**

1. **Wire barrier computation at entry** — **DONE.**
   - **1a — Shadow (DONE, `5d52f23`).** `[TB-SHADOW]` A/B logging in the `HandleFills` entry-fill branch; validated the D1 mapping numerically, zero behavior risk.
   - **1b — Engine-driven (DONE, `2eb658e`).** `tbe::ComputeBarriers()` (via `BuildBarrierInputs()`) is now the single authority: barriers are computed right after Phase 3 and overwrite `stopPrice`/`targetPrice` so both sizing and the SC attached bracket use engine barriers. `OpenBracket` latches the bracket for the time barrier.
2. **Vertical (time) barrier — DONE (`7b40437`).** Per-tick check in `Update()` against the entry-latched `maxBars` → `ClosePositionAtMarket("TIME_STOP")` (D4), guarded by `m_exitSubmittedThisTick`.
3. **Finding 17 fail-closed routing (D2) — DONE (`2eb658e`).** `BuildBarrierInputs()` validates `pattern_id` in `kPatternTable` range and falls back to the LOW-tier default otherwise.
4. **Remove scale-out ladder — DONE (`2eb658e`).** Single full-size target; the `HandleFills` scale-out branch is size-bookkeeping only.
5. **Remove trailing machinery — DONE (`9ee5326`).** `UpdateChandelierStops()` + its `Update()` call deleted; `EvaluateRegimeDefense` Chandelier tightening removed (climate/hostile → telemetry); hostile/toxic **flatten** retained (D5).
6. **Delete `ChandelierStopManager` — DONE (`9ee5326`).** `.h`/`.cpp`, includes, all call sites, and the `CMakeLists.txt` entry removed; `grep` clean (only doc comments remain).
7. **Build green — DONE; acceptance (§5) pending replay.** `./build_dll.sh --no-clean` green across all slices.

**Added beyond the original plan:** native **TRAP floor** (`711e7cf`) — priority-#1 completed-bar `StructureTest` reversal exit (the reactive layer of the two-observer TRAP doctrine). Remaining TRAP work tracked in §7.

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

---

## 7. Deferred follow-ups (revisit soon)

Landed so far (context): Step C single-stage first-touch cutover (`2eb658e`); stale-fish removal + Step D `ChandelierStopManager` deletion (`9ee5326`); TRAP doctrine across the 4 mirror docs + this plan (`0d674ef`, `0ac8a01`); **native TRAP floor** (`711e7cf`, priority-#1 completed-bar `StructureTest` reversal exit — the reactive layer). The items below are the paired co-evolution / enhancement slices, in rough priority order.

1. **Schema — `ExitReason_TRAP` (+ `ExitReason_REGIME_INVALIDATION`).** Add to `../schema/backtest_schema.fbs`, run `regenerate_schema.sh`, and update `BackTesterStudy.cpp::MapExitReason` (currently maps `"TRAP"` → `MANUAL`). **Prerequisite** for attributing native-floor TRAP exits in `.btst` replay — without it we can't measure TRAP behavior for the `F_0.25` deploy gate. Cheap, self-contained; do first.

2. **Labeler Q0 split (co-evolution parity).** In `lbrnet/lbrnet/labeling/triple_barrier_scanner.py`: remove `DECISIVE_BREAKDOWN_LOW` (id 6) from `_T1_LONG_IDS` and `DECISIVE_BREAKOUT_HIGH` (id 5) from `_T1_SHORT_IDS`, and route an adverse decisive counter-break to a **new `REGIME_INVALIDATION`** scan outcome (distinct from `TRAP_CANDIDATE`). Makes the label the model trains on match the native floor's reversal-only definition (`FAILED_*`). **Needs a fresh `.context` regen to validate.** Governing rationale: `triple_barrier_trap_definition_ruling.md` (Q0).

3. **Anticipatory τ\* model exit (the second observer).** Requires wiring **in-position TRAP inference** — today `TRAP_LONG/SHORT` predictions arrive in `PredictionSlot` but are logged-only (no exit action). Add: (a) request/consume a model TRAP probability `p` for the open position; (b) compute `τ* = C_FP/(C_FP+C_FN)` each tick from the entry-latched bracket (`C_FP=|target−price|`, `C_FN=|price−stop|`); (c) additive exit when `fresh ∧ p ≥ τ* ∧ adverse`, never suppressing the native floor. **Ships DISABLED behind the deploy gate `out-of-sample F_0.25 > 0.65`.** Governing: `triple_barrier_trap_definition_ruling.md` (Q1 τ*, Elkan 2001).

4. **Option B — intra-bar TRAP re-inference (full ECTS edge).** Refresh the TRAP-relevant fast features + re-run inference intra-bar so `p` itself updates within the bar (not just per-tick τ* over a bar-gated `p`). Blocked on **ECTS-style training on intra-bar prefixes** (train on partial bars labeled with the eventual completed-bar outcome) to avoid train/live OOD. Governing: the intra-bar timing ruling (§5 of the Trap doctrine; Dachraoui 2015 / Mori 2017 / Shiryaev).

5. **`CalculateScaleOutTargets` cleanup.** Now dead (no caller after Step C; no `Chandelier` dependency) — left in place to keep Step D surgical. Delete decl+def (`PositionManager.h` / `.cpp`) in a standalone cleanup commit.

6. **Regime-invalidation FLATTEN escalation (optional).** The `EvaluateRegimeDefense` climate-shift / hostile-env branches were demoted to telemetry when trailing was removed (Step D). Decide whether an adverse `DECISIVE_*` / hostile-regime mid-trade should escalate to a `REGIME_INVALIDATION` market flatten (a sibling of TRAP) rather than only telemetry. Pairs naturally with items 1–2.
