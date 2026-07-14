# Execution Engine Correctness Findings — PositionManager / RiskManager / ChandelierStopManager / Scoring / ExecutionGate

> Status: **SPEC — ready to fix.** Findings below come from a full-file adversarial code review
> of this project's source (`src/PositionManager.cpp`, `src/PositionManagerPatterns.cpp`,
> `src/RiskManager.cpp`, `src/ChandelierStopManager.cpp`, `src/Scoring.cpp`,
> `src/execution/ExecutionGate.cpp`, plus headers), performed 2026-07-10 from the sibling
> `lbrnet` repo while auditing Python/C++ parity. Findings are independent of that parity work —
> these are genuine correctness issues a reviewer would flag in this codebase on its own terms.
> Original writeup: `../lbrnet/docs/ADR/cpp_source_correctness_findings_spec.md` (same content,
> written from the Python side). This copy is the canonical one to act on — it lives with the
> code.
>
> **Addendum (2026-07-11)**: Findings 13-14 added from a follow-on Python/C++ parity audit —
> position-sizing multiplier chain (13) and a correction to Finding 12's parenthetical claim about
> the T1/T2/T3 scale-out ladder, which was wrong (14). Finding 7 also got a short addendum noting
> the current Python-side behavior.
>
> **Addendum (2026-07-11, later same day)**: Findings 13 and 14 fully closed on the `lbrnet`
> side (per-user directive to close every remaining Python parity gap, not just document them).
> Two new, genuinely-new findings surfaced during that work: Finding 15 (Pareto-confidence-sigmoid
> comment mathematically cannot match its own formula — MEDIUM, since sizing behavior depends on
> which one is "correct") and Finding 16 (ATR-volatility-multiplier comment is loosely
> approximate — LOW, cosmetic). Finding 14's structural-swing-cap sub-issue was also confirmed
> genuinely unportable to Python (verified via this codebase's own generated wire policy, not
> just asserted) — a real architectural boundary, not a deferred TODO. All items remain
> Python-port parity gaps or C++-internal comment drift, not new execution-correctness bugs —
> actionable follow-ups (where any exist) live in the `lbrnet` repo's `docs/ADR/` specs cited
> inline.
>
> **Addendum (2026-07-12)**: Finding 17 added — a genuine C++ correctness bug (not a Python-port
> parity gap), found during the `lbrnet` Phase 2 triple-barrier migration's full 9-pattern audit:
> `CalculateStrategySetupPrices`'s fallback path reinterprets a `RaschkeTacticalTrigger` id as an
> unrelated `RaschkeStrategySetup` id for 6 of 9 tactical trigger patterns, producing accidental
> stop/target formulas (worst case: `ITR_FADE` inherits `HOLY_GRAIL`'s trend-continuation logic by
> numeric coincidence — the opposite trading philosophy). **Explicit user directive: defer
> action on Finding 17 until the `lbrnet` Phase 2 migration is complete and there is a deployable
> model** — see the "Timing" note inside Finding 17 itself. Do not fix this opportunistically
> before then even if touching this file for another reason.
>
> **Addendum (2026-07-13)**: Finding 18 added — a genuine C++ correctness bug (not a Python-port
> parity gap), found from the `lbrnet` side during the `params_convergence_spec.md` gate-audit
> arc (PC-14): `RiskManager::EvaluateHardGates()`'s Shannon-entropy hard gate compares raw,
> unnormalized entropy bits against a threshold apparently authored for a normalized ratio,
> firing on effectively 100% of real observations — a units-mismatch bug in the same family as
> the already-fixed-on-the-`lbrnet`-side `taleb_cliff_proximity` bug (PC-13). **Not yet fixed
> here** — `lbrnet`'s Python replica has applied a partial fix (normalization only); the deeper
> root cause (the entropy computation's own input binning) remains open on both sides pending a
> separate authorization/scoping decision.
>
> **Addendum (2026-07-13, later same day)**: Finding 19 added — this one is NOT a C++
> correctness bug (`RiskManager`'s own gate is internally consistent), it's a signal-exposure gap:
> the raw `[0,1]` liquidity-fragility value the real hard gate checks is never itself serialized to
> the wire — only a log-z-scaled, 6-sigma-winsorized proxy of the same dimension is, and `lbrnet`'s
> Python replica was reading that proxy against the real gate's raw-scale threshold. **Fix for the
> next release**: serialize the raw signal as its own field. ⚠️ `lbrnet` currently carries a
> TEMPORARY empirical-percentile workaround (per explicit user instruction) that **must be removed**
> once this ships — see Finding 19 for the full removal instructions.
>
> **Addendum (2026-07-13, later still)**: Finding 20 added — same root-cause family as Finding 19
> (a scaled model-input proxy substitutes for a raw value `RiskManager`'s gate needs, never itself
> serialized), found auditing `ComputeParetoTopStateRatioProxy()`'s Hill-alpha input. Worse than
> Finding 19: the wire's `tail_index` is a robust z-score of Hill alpha (soft-log-compressed,
> range ±1.9459), and the real gate's formula computed on this data is structurally bimodal
> (82.8% of real observations land at exactly the "invalid alpha" fail-safe value) — unlike
> Finding 19, `lbrnet` could not rebase any threshold to a sane rate, so it removed the gate from
> its Python replica entirely instead (per explicit user instruction). **Fix for the next
> release**: same pattern as Finding 19 — add the raw Hill alpha as a new field on the existing
> unscaled channel.
>
> **Addendum (2026-07-13, later still)**: Finding 21 added — third instance of the same family,
> found auditing `RiskManager.cpp:797-812`'s VPIN/Amihud toxicity gate. The wire's `vpin_toxicity`
> is a robust z-score of the raw, unbounded Amihud illiquidity value (soft-log-compressed, range
> ±1.9459, same signature as Finding 20), firing on 31.4%/46.8% of real observations against the
> real 0.80/0.40 thresholds. Unlike Finding 20, this proxy is continuously distributed (not
> bimodal), so `lbrnet` empirically rebased (p90/p75) rather than remove the gate. **Fix for the
> next release**: same pattern — add the raw Amihud value as a new field on the existing unscaled
> channel. ⚠️ `lbrnet` carries a TEMPORARY empirical rebase that must be removed once this ships.
>
> **Addendum (2026-07-14)**: two more gates audited during the same `params_convergence_spec.md`
> arc (PC-18, PC-19) — neither opened a new numbered Finding, because neither confirms a C++ bug
> or a signal-exposure gap. Recorded here for cross-reference only:
> - **PC-18** — `ExecutionGate.cpp:30`'s regime-duration gate (`shannonTenureBars <
>   shannonMinTenureBars`) was fully audited: correct real threshold (143.5 bars ≈ 35.9h once the
>   system's actual 15-minute bar period is applied — not subject to the SOFTLOGZ/LOGZ wire
>   contamination behind Findings 19-21, since it's a raw integer bar count), correct semantics.
>   **Confirmed correct, no C++ issue** — moved to "Verified-correct citations" below. `lbrnet`
>   removed its Python replica of this gate anyway, purely on compute/token-cost grounds (it had
>   become the top rejector after Findings 18-21's fixes) — not because anything here is wrong.
> - **PC-19** — `lbrnet` also removed its Python replica of the `bias_veto_short_into_bullish_trend`
>   branch (`daily_bias` check), on the hypothesis that it's redundant with the model's own
>   transformer input.
>
> **Addendum (2026-07-14, later same day)**: PC-19's hypothesis has now been checked against this
> codebase directly and is **REFUTED**. `daily_bias` is confirmed as a genuine, populated model
> input feature (same standard of evidence as the `time_of_day`/PC-10 precedent) — but that does
> not make the veto redundant, because `Scoring::ApplyDailyBiasFilter()` (`Scoring.cpp:351-390`) is
> called **unconditionally** at every live order entry (`PositionManager.cpp:1711-1719` automatic,
> `:2588-2592` manual), independent of anything the Python model predicts or has learned. Removing
> this gate from `lbrnet`'s Phase 2 backtest replica makes the backtest credit trades that live
> execution will reject regardless — a parity break, not a validated redundancy. Incidentally: a
> full, faithful Python port of this exact veto already exists
> (`lbrnet/core/scoring.py::apply_daily_bias_filter()`, matching this file's own
> "Matches Python: lbrnet/core/scoring.py -> apply_daily_bias_filter" comment) but has zero callers
> anywhere in the `lbrnet` repo — dead code, tracked as `lbrnet`'s PC-21, not a C++ issue. Per
> explicit user instruction, `lbrnet` has NOT restored this gate yet — documented only, restoration
> deferred to the user. See `docs/ADR/params_convergence_spec.md` PC-19/PC-20/PC-21 for full detail.

**Caveat on currency**: files audited were dated through 2026-06-27 (the newest on disk at audit
time). If the live-deployed DLL was built from a newer revision than what's on disk, re-verify
each finding against current `HEAD`/working tree before fixing — line numbers especially may
have shifted.

---

## Finding 1 [CRITICAL] — `PositionManager::UpdateContext()` is never called; the entire regime-defense subsystem runs on permanently frozen state

`UpdateContext()` (declared `include/PositionManager.h:97`, defined
`src/PositionManagerPatterns.cpp:55-72`) is the *only* place that updates
`m_currentHMMState`/`m_previousHMMState`/`m_currentClimate`/`m_previousClimate`:

```cpp
void PositionManager::UpdateContext(SCStudyInterfaceRef sc) {
    auto* hmmInd = InferenceManager::Instance().HmmState();
    if (hmmInd) { m_previousHMMState = m_currentHMMState; m_currentHMMState = hmmInd->Value(); }
    auto* climateInd = InferenceManager::Instance().MarketClimate();
    if (climateInd) { m_previousClimate = m_currentClimate; m_currentClimate = climateInd->Value(); }
    if (!IsFlat()) { EvaluateRegimeDefense(sc); }
}
```

A repo-wide grep found **zero call sites** of `PositionManager::Instance().UpdateContext(...)`
anywhere — not in `SCStudies.cpp`, `BackTesterStudy.cpp`, `SystemOrchestrator.cpp`, or
`PositionManager.cpp` itself — despite the header comment claiming "Called by SystemOrchestrator
when regime changes or by Update() on tick" (`PositionManager.h:96`).

Meanwhile `EvaluateRegimeDefense(sc)` **is** called every tick (`PositionManager.cpp:236`), but it
reads the never-updated `m_currentHMMState`/`m_currentClimate`, which stay at their
default-constructed values (`HMM_NO_PRIOR` / `MarketClimate::GAUSSIAN_STABLE`,
`PositionManager.h:298-303`) for the entire process lifetime.

**Concrete failure scenario**: market flips from `GAUSSIAN_STABLE` to `GAUSSIAN_FRAGILE` while a
position is open. The "HMM regime as independent exit signal" check
(`PositionManagerPatterns.cpp:85-95`, `if (m_currentHMMState != m_previousHMMState && ...)`) never
fires, because both variables are always `HMM_NO_PRIOR`. The climate-shift mid-trade hard gate
(`IsCriticalClimateShift`, line 101 — forced trailing activation on
`TALEBIAN_FRAGILE`/`SHANNON_CHAOS`) never activates. The holding-score toxic/hostile exit logic
(lines 116-172) computes its regime multiplier from the frozen default, never the live regime.
This same stale state feeds `UpdateTradeGradeProtection`'s regime-conditional grade thresholds
(`PositionManager.cpp:3347`). **An entire advertised subsystem ("Elite v2.5: Context-Aware
Position Management") is silently neutered.**

**Fix**: call `PositionManager::Instance().UpdateContext(sc)` once per tick, most likely from
`SCStudies.cpp`'s main update loop alongside the other manager `Update()` calls, or from
`SystemOrchestrator` if regime-change events are meant to drive it per the stale header comment.
Verify against whichever call site was intended before the wiring was lost/never added.

**Addendum (found 2026-07-10, second pass, auditing `../lbrnet/docs/ADR/phase2_chandelier_improvements.md`)**:
this same frozen state has a second, independent consequence beyond the regime-defense
subsystem described above. `PositionManager::UpdateTradeGradeProtection()`
(`PositionManager.cpp:3347`) calls `InferenceManager::GetRegimeGradeThresholds(m_currentHMMState,
m_currentClimate)` — the exact same two frozen fields. `GetRegimeGradeThresholds()`
(`InferenceManager.cpp:138-168`) implements a real, well-designed regime-adjusted Elder Grade
A/B/C threshold system (base 30/20/10, raised to 35/25/15 in `PARETO_MOMENTUM` climate/state,
lowered to 22/15/8 or 20/12/7 in fragile/chaos climates) — but since both switch statements'
input is always frozen at the default (`HMM_NO_PRIOR`/`GAUSSIAN_STABLE`), both always fall
through to their `default:` branch, so the live system likely always uses the base 30/20/10
thresholds regardless of actual market regime. The Grade-C breakeven backstop itself
(`PositionManager.cpp:3373-3404`) still fires — it's the *regime-adjusted threshold* that's
inert, not the backstop mechanism as a whole. Same fix as Finding 1 resolves both.

---

## Finding 2 [MEDIUM] — Manual-entry `decisionPrice` never assigned; breaks partial-fill grace logic for manual trades only

Automatic path (`PositionManager.cpp:2447-2456`) sets `m_pendingEntryOrder.decisionPrice`. Manual
path's equivalent block (`PositionManager.cpp:2878-2886`) sets every other pending-order field but
never `decisionPrice` — it stays at the struct default `0.0f`:

```cpp
m_pendingEntryOrder.active = true;
m_pendingEntryOrder.orderId = orderResult;
m_pendingEntryOrder.requestedQuantity = adjustedQuantity;
m_pendingEntryOrder.isLong = isLong;
m_pendingEntryOrder.submitPrice = entryPrice;
m_pendingEntryOrder.executionStyle = "MANUAL";
m_pendingEntryOrder.repriceCount = 0;
m_pendingEntryOrder.submitTime = sc.CurrentSystemDateTime;
m_pendingEntryOrder.lastRepriceTime = sc.CurrentSystemDateTime;
// decisionPrice left at its struct default: 0.0f
```

`HandleFills`'s partial-fill-rebasing logic (`PositionManager.cpp:276-296`, "GAP 19: INTELLIGENT
PARTIAL FILL REBASING") computes `slippageTicks = |actualFillPrice - decisionPrice| / tickSize`;
with `decisionPrice=0`, this is always enormous for a real futures price (e.g. ~5000 for ES), so
**every partial fill on a manually-submitted trade unconditionally cancels the remainder
immediately**, while the identical automatic-path logic correctly evaluates real slippage.
Fail-direction is safe (cancels rather than over-holds), but the intended grace-window behavior is
silently disabled for the entire manual/UI order path.

**Fix**: add `m_pendingEntryOrder.decisionPrice = entryPrice;` (or the equivalent decision-time
price) to the manual-path block at `PositionManager.cpp:2878-2886`.

---

## Finding 3 [MEDIUM] — `RiskManager::GetConsecutiveLosses()` is a dead stub always returning 0, feeding real dashboard/report fields

`RiskManager.cpp:2009-2013`:
```cpp
int RiskManager::GetConsecutiveLosses() const {
    // Note: Need sc reference to read persistent storage
    return 0;
}
```
No `sc`-parameterized overload exists. Called at `SCStudies.cpp:230` and
`BackTesterStudy.cpp:1561` to populate HUD/report fields — both always show
`consecutive_losses: 0` regardless of the real tracked count (`RISK_CONSECUTIVE_LOSSES_ID`),
masking real state when a trader is deep in a loss streak approaching a halt.

**Fix**: implement the `sc`-parameterized overload (persistent-storage read) referenced by the
comment, and have the no-arg version either delegate to it via a stored `sc` reference or be
removed if genuinely unreachable without one.

---

## Finding 4 [MEDIUM] — `RiskManager::GetRiskMultiplier()` (continuous drawdown sizing) fully implemented, never called

`RiskManager.cpp:2322-2345`, declared `RiskManager.h:299` — zero call sites anywhere. Contradicts
the header's documented behavior ("Drawdown-based position sizing reduction,"
`RiskManager.h:21,113`). The real sizing path only applies a binary halt at 25% drawdown
(`CheckDrawdownLimit`, `RiskManager.cpp:2261-2277`) — no continuous size reduction from 0-25%
despite a fully-implemented continuous-ramp function sitting unused.

**Fix**: wire `GetRiskMultiplier()` into `CalculateSafePositionSize()`'s multiplicative stack
(`RiskManager.cpp:1479-1899`), or confirm it was deliberately superseded by the binary halt and
remove it if so.

---

## Finding 5 [LOW] — "Daily win protection" comment describes behavior that doesn't exist

`RiskManager.cpp:1094-1097`:
```cpp
// Check 3: Daily win protection
if (dailyPnL >= accountEquity * m_execParams.dailyWinWarningPct) {
    // Don't reject - CalculateSafePositionSize() will apply 0.5x multiplier
}
```
`dailyWinWarningPct` is read only here — no other reference anywhere in `src/` or `include/`.
`CalculateSafePositionSize()` has no such 0.5x (or any) multiplier tied to daily-win-warning
proximity. The branch is currently a pure no-op; the comment describes a mechanism that was
either never implemented or was removed.

**Fix**: either implement the described 0.5x multiplier in `CalculateSafePositionSize()`, or
remove the dead check and stale comment if daily-win protection is no longer desired.

---

## Finding 6 [LOW] — Duplicated hard-coded fragility thresholds in a legacy validation path

`RiskManager::IsTradeAllowed()` (`RiskManager.cpp:2440-2454`, an older/GUI-facing path) hardcodes
`talebKurtosis > 15.0f` and `shannonFlowEntropy > 0.9f` inline rather than sourcing from
`m_execParams`. These happen to match `EvaluateHardGates()`'s values today (lines 826, 820) but
are separately-maintained literals — a future tuning pass on one could silently diverge from the
other with no compiler warning.

**Fix**: source both thresholds from `m_execParams` in `IsTradeAllowed()` so there is one place to
tune them, or delete `IsTradeAllowed()` if `EvaluateHardGates()` has fully superseded it.

---

## Finding 7 [MEDIUM] — Chandelier trail: asymmetric one-way-stop-update rule for shorts only

`ChandelierStopManager.cpp:212-225`:
```cpp
if (info.isLong) {
    // Long: stop can only move UP
    if (newStop > info.stopPrice) {
        info.stopPrice = newStop;
        stopUpdated = true;
    }
} else {
    // Short: stop can only move DOWN
    if (newStop < info.stopPrice || info.stopPrice == info.initialStopPrice) {
        info.stopPrice = newStop;
        stopUpdated = true;
    }
}
```
For shorts only, the extra `|| info.stopPrice == info.initialStopPrice` clause forces adoption of
`newStop` on the first post-activation update **regardless of whether it's favorable** — no
equivalent bypass exists for longs. This breaks the function's own documented "stop only moves
favorably" invariant (`ChandelierStopManager.h:15-16,83`) specifically for shorts.

**Fix**: confirm whether the extra clause is an intentional "adopt Chandelier stop on activation"
transition (in which case add the matching clause for longs) or an accidental copy-paste
asymmetry (in which case remove it from the short branch).

**Addendum (found 2026-07-11, Python/C++ parity audit)**: `../lbrnet/backtest/trade_simulator.py`'s
trailing-stop update (`_stop = max(self._stop, chandelier)` for longs, `min(...)` for shorts) is
symmetric on both sides — it matches the long-side rule exactly but does **not** replicate the
short-side snap exception described above. Practical effect is narrow (one bar, at trailing
activation only, short side only).

**Python-side disposition resolved (2026-07-11)**: the `lbrnet` repo's governance standard treats
C++ as not automatically correct — literature/institutional standard is the arbiter when they
disagree. A Chandelier trailing stop's defining property (Chuck LeBeau; standard for ATR trailing
stops generally) is a strict favorable-direction-only ratchet; this exception violates that
invariant with no literature grounding found. `lbrnet` made **no Python change** — the existing
symmetric behavior already matches the institutional standard and needs no fix regardless of how
this C++ finding resolves. This strengthens (does not replace) the **Fix** recommendation above:
absent a concrete justification for the exception, treat it as the copy-paste-asymmetry
possibility, not the intentional-transition possibility. See
`../lbrnet/docs/ADR/phase2_chandelier_improvements.md` for the full rationale.

---

## Finding 8 [LOW] — `Scoring.cpp`: two fully-implemented functions never called anywhere

`GetEventVelocityMultiplier` (`Scoring.cpp:200-211`) and `GetIntrabarConfidenceMultiplier`
(`Scoring.cpp:168-198`) — both declared in `Scoring.h`, both carry comments claiming they mirror
active Python logic ("Matches get_event_velocity_multiplier in scoring.py"), neither has any call
site in this codebase.

**Fix**: wire both into whichever scoring path they were intended for (likely
`GetDeepContextMultiplier` or the pattern-quality pipeline), or remove them if superseded.

---

## Finding 9 [LOW-MEDIUM] — `Scoring.cpp`: NR7 pattern silently gets a hard 0.0 multiplier under high entropy

`Scoring.cpp:257-260`:
```cpp
if (ctx.shannonFlowEntropy > 0.80f) {
    if (isMeanReversionPattern) multiplier *= 1.25;
    else if (isTrendPattern) multiplier *= 0.70;
    else multiplier *= 0.0;
}
```
`isMeanReversionPattern` = `TurtleSoup || KangarooTail`, `isTrendPattern` = `ElderBreakout ||
MomentumPinball` (lines 254-255). `PatternType::NR7` falls into neither bucket, so under
`shannonFlowEntropy > 0.80` an NR7 signal's multiplier is silently forced to exactly `0.0`,
flowing into `RiskManager.cpp:1584` and `PositionManagerPatterns.cpp:124`.

**Fix**: confirm whether killing NR7 entries in high-entropy/chaos regimes is intentional; if so,
add an explicit comment and consider whether ITR_Breakout/ITR_Fade/RSI_Failure_Swing/
Stochastic_Pop (also not in either bucket) should behave the same way rather than falling through
to the same silent zero.

---

## Finding 10 [INFO] — `Scoring.cpp`'s own in-code comment is stale relative to its own implementation

The comment above `GetDeepContextMultiplier` (`Scoring.cpp:230-232`) describes a discrete tiering
scheme ("Kurtosis > 10.0 => 0.6x", "Kurtosis > 4.0 => 0.8x") that does not match the actual
continuous sigmoid implemented two lines below it (`Scoring.cpp:246-251`,
`1/(1+exp(0.5*(kurtosis-6.0)))`, gated on kurtosis > 2.5).

**Fix**: update the comment to describe the actual sigmoid formula (or vice versa, if the sigmoid
was an unintentional drift from the intended discrete tiers — worth confirming which is correct
before just fixing the comment).

---

## Finding 11 [MEDIUM] — `ExecutionGate`: manual entries skip model-freshness checks that automatic entries enforce

`EvaluateAutomatic()` (`src/execution/ExecutionGate.cpp:46-54`) checks `modelReady` and
`predictionFresh` before admitting an entry. `EvaluateManual()` (lines 78-105) never references
either field — a manual entry can proceed even with a stale/not-ready model, as long as context is
fresh and hard gates pass.

**Fix**: confirm whether this is intentional (manual entries are human-initiated and don't need
model gating) or an oversight. If intentional, document it explicitly in `ExecutionGate.h`; if not,
add the same `modelReady`/`predictionFresh` checks to `EvaluateManual()`.

---

## Finding 12 [INFO — not a C++ bug; a Python-port parity gap] — `ChandelierStopManager::ShouldUseTrailingStop()`'s per-pattern dynamic-tightening gate is not modeled in the Python port at all

Found while investigating why `PositionManager.cpp` has both a software-managed Chandelier trail
and a simpler, server-side "runner" trailing order (that pairing is intentional and correctly
documented — see the "GAP 24 v2"/"GAP 18 v2" comments at `PositionManager.cpp:2320-2345`, not a
finding, just context for what follows).

`ChandelierStopManager::ShouldUseTrailingStop()` (`ChandelierStopManager.cpp:413-431`) is a
third, independent axis on top of that pairing — it gates whether the *software-managed dynamic
stop-tightening* (`ChandelierStopManager::ActivateTrailing()`) ever activates for a given
pattern, once a scale-out target fills:

```cpp
bool ChandelierStopManager::ShouldUseTrailingStop(RaschkeTacticalTrigger patternTrigger) {
    switch (patternTrigger) {
        // Trend continuation patterns that benefit from trailing stops
        case RaschkeTacticalTrigger::ELDER_BREAKOUT_BUY:
        case RaschkeTacticalTrigger::ELDER_BREAKOUT_SELL:
        case RaschkeTacticalTrigger::ITR_BREAKOUT_BUY:
        case RaschkeTacticalTrigger::ITR_BREAKOUT_SELL:
            return true;
        // Mean-reversion / scalar patterns (fixed targets, no trailing)
        case RaschkeTacticalTrigger::MOMENTUM_PINBALL_BUY:
        case RaschkeTacticalTrigger::MOMENTUM_PINBALL_SELL:
        case RaschkeTacticalTrigger::TURTLE_SOUP_BUY:
        case RaschkeTacticalTrigger::TURTLE_SOUP_SELL:
        case RaschkeTacticalTrigger::KANGAROO_TAIL_BUY:
        case RaschkeTacticalTrigger::KANGAROO_TAIL_SELL:
        case RaschkeTacticalTrigger::STOCHASTIC_POP_BUY:
        case RaschkeTacticalTrigger::STOCHASTIC_POP_SELL:
            return false;
        case RaschkeTacticalTrigger::NONE:
        default:
            return false;  // Conservative default - no trailing
    }
}
```

Called from `PositionManager.cpp:355-368` (`ShouldPatternTrail()` wrapper, confirmed live — not
dead code): when a scale-out fill reduces position size, this gate decides whether
`ChandelierStopManager::ActivateTrailing()` runs for the remainder. If `false`, the position's
stop stays at its flat, non-decaying server-side trailing distance
(`ATR × DofStopScale`, set once at entry — see the "runner" mechanism above) for the rest of the
trade; only `ELDER_BREAKOUT`/`ITR_BREAKOUT` get the additional dynamic tightening as R-multiple
grows. `NR7_BREAKOUT`, `ITR_FADE`, and `RSI_FAILURE_SWING` aren't in the switch at all and fall to
`default: return false` — also no dynamic tightening.

**This is a genuinely separate axis from `CalculateScaleOutTargets()`'s per-pattern R-multiple
ladder** (T1/T2/T3, `is_mean_reversion`/`target_r_mult`). **Correction (2026-07-11, Python/C++
parity audit): the parenthetical claim in the previous version of this finding — that the T1/T2/T3
ladder is "already correctly mirrored" in `../lbrnet/backtest/backtest_runner.py`'s `_PATTERN_EXIT`
table — was wrong.** That table (`PatternExitConfig`, `../lbrnet/lbrnet/labeling/
triple_barrier_scanner.py:56-91`) has exactly one `target_r_mult` field per pattern — a single
scalar (fixed R-multiple, or `inf` for "no fixed target, rely on trailing") — with no
partial-quantity field anywhere in the dataclass. It correctly mirrors *which target-level
philosophy* a pattern uses (fixed-limit vs. let-it-run), but it does **not** mirror
`CalculateScaleOutTargets()`'s actual mechanism: splitting the position into 2-3 tranches that
exit at different R-multiples while the remainder trails. See new Finding 14 below for the
corrected, standalone treatment of that quantity-structure gap — it is real and unaddressed,
unlike what the retracted parenthetical claimed.

A pattern can have an
unbounded "let it run" final target (`target_r_mult=inf`) while still **not** getting the extra
dynamic stop-tightening modeled here — e.g. `KANGAROO_TAIL` has `target_r_mult=inf` in the
Python table but `ShouldUseTrailingStop()` says it should never get dynamic tightening. These two
facts don't contradict each other (they govern different things — target ceiling vs. stop
tightening behavior) but neither the labeler nor `../lbrnet/backtest/trade_simulator.py` (Phase
2's Chandelier trail simulation) currently distinguishes between patterns at all here — Phase 2
applies the same dynamic decay (floor=2.0, λ=0.35, per Finding 5 in the lbrnet spec) uniformly to
every trade, when in the real system roughly half the patterns never get dynamic tightening and
instead rely solely on the flat, non-decaying runner distance for the life of the trade.

**Not a C++ correctness issue** — `ShouldUseTrailingStop()` is intentional, internally consistent,
and its own logic is fine as written. Recorded here (rather than as a "fix") because it's directly
relevant to any future Phase 2 exit-model parity work: `trade_simulator.py` currently overstates
how many patterns get dynamic stop-tightening, which will bias simulated omega_net in an
unverified direction depending on which patterns dominate the trade mix. No C++ change
recommended; the actionable follow-up (model this per-pattern gate in `trade_simulator.py`) lives
in the `lbrnet` repo.

---

## Finding 13 [INFO — not a C++ bug; a Python-port parity gap] — `RiskManager::CalculateSafePositionSize()`'s live multiplier chain is not modeled by Phase 2's `VirtualBroker`

Found while auditing Phase 2's `VirtualBroker` class (`../lbrnet/backtest/backtest_runner.py:1175-1284`)
against this codebase's real live sizing path.

`CalculateSafePositionSize()` (`RiskManager.cpp:1479-1899`) computes a base risk-driven quantity —
`maxRisk = accountEquity * m_execParams.maxRiskPerTradeFrac` (0.02), `baseSize =
maxRisk/riskPerContract` (line 1518) — then multiplies that base through roughly ten further,
independently-gated continuous factors before arriving at the real submitted order quantity
(lines 1522-1730+): Pareto-confidence sigmoid, top2-margin conviction, thesis-strength scaling,
entropy discount, `HmmStateIndicator::RiskMultiplier()`/`SizingDurationFactor()`
(`include/Indicator.h:2018`, called live at `RiskManager.cpp:1568`), the deep-context multiplier
(`Scoring::GetDeepContextMultiplier`, see Findings 9/10 above and the companion
`scoring_deep_context_parity_spec.md` in the `lbrnet` repo), an ATR-volatility multiplier, a
transport-degraded 0.5x cut, the kurtosis-emergency cap, a Mahalanobis-distance cap, a
Hill-alpha tail-risk gate, and a PARETO_MOMENTUM opportunity boost.

`VirtualBroker.calculate_position_size` (`backtest_runner.py:1268-1277`) implements only the base
formula — `int(equity*0.02 / (stop_ticks*tick_value))` — with none of the ~10 downstream
multipliers. This is distinct from `RiskManager::GetRiskMultiplier()` (Finding 4, confirmed dead
code) — Python isn't modeling the dead drawdown-ramp function; it's simply omitting the entire
*live* multiplier stack that actually determines contract count in production. Phase 2's Elder 6%
monthly limit, 2% daily loss limit, and Raschke session-quality floors were all separately
verified exact against `CheckMonthlyLossLimit`/`CheckDailyLossLimit`/`ValidatePatternQuality`
(`RiskManager.cpp:2279-2320`, `2226-2255`, `922-976`) — only the per-trade sizing formula itself
has this gap.

**Practical effect**: Phase 2's simulated contract count — and therefore its dollar P&L
magnitude and `omega_net` — diverges from what live C++ would actually size on essentially every
trade, independent of whether entry/exit timing is correct. Not a C++ bug; the actionable
follow-up (model this multiplier chain in `VirtualBroker`, or explicitly accept the simplification
and document why) lives in the `lbrnet` repo — see `docs/ADR/phase2_position_sizing_parity_spec.md`.

**Update (2026-07-11, later same day): fully closed on the `lbrnet` side.** Every factor listed
above is now ported (the HMM risk multiplier via a full re-derivation of
`ComputeInstitutionalRiskMultiplier`, `RiskManager.cpp:229-307`, using the built-in
`HMMRiskPolicy` defaults since no override JSON exists in that environment either), except the
transport-degraded cut and prediction-freshness discount, both confirmed architecturally
inapplicable to an offline batch replay (no live connection or wall-clock replay delay exists
to model). Two comment/formula mismatches were found in the process — see new Findings 15/16
below.

---

## Finding 14 [INFO — not a C++ bug; a Python-port parity gap, corrects Finding 12's parenthetical] — `PositionManager::CalculateScaleOutTargets()`'s T1/T2/T3 partial-quantity scale-out ladder is not modeled in Python at all

`CalculateScaleOutTargets()` (`PositionManager.cpp:3048-3215`) is genuinely live — called from the
order-submission path (`PositionManager.cpp:2272`), with `target1Price/target2Price/target3Price`
attached directly to the live order (`AttachedOrderTarget{1,2,3}Type`, lines 2392-2424, including
a `TRIGGERED_TRAILING_STOP_LIMIT_3_OFFSETS` runner leg for T3). Ladder shape is quantity-dependent
(3-tier for qty≥3, 2-tier for qty=2, single trailing runner for qty=1), further scaled by regime
target-width and Hurst-exponent multipliers, and capped by the 60-minute structural swing
high/low + Keltner-rejection logic at lines 3178-3214.

Neither `../lbrnet/lbrnet/labeling/triple_barrier_scanner.py` (the labeler) nor
`../lbrnet/backtest/trade_simulator.py` (Phase 2) model any partial-quantity exit — both resolve
every trade to a single binary stop/target outcome for the full position size. Finding 12 above
previously stated this ladder was "already correctly mirrored" via the `_PATTERN_EXIT` table —
that claim conflated *target-level philosophy* (fixed-R vs. let-it-run, which the table does
correctly capture per pattern) with *position-quantity structure* (partial exits at multiple
levels, which nothing on the Python side captures at all). That claim has been corrected in place
above; this finding is the standalone record of the actual gap.

**Practical effect**: trades that would scale out 50-80% of the position at T1/T2 in live C++,
then reverse before the final target/stop, are recorded by the Python single-target model as a
full stop-out (or a much smaller/losing trade) rather than the partially-banked profit C++ already
realized. This systematically *understates* realized P&L and *overstates* left-tail loss frequency
on the Python side relative to live performance — a pessimistic bias, not an optimistic one. Not a
C++ bug; the actionable follow-up lives in the `lbrnet` repo — see
`docs/ADR/scaleout_ladder_gap_spec.md`.

**Update (2026-07-11, later same day): the R-multiplier/quantity-tier ladder is now implemented
on the `lbrnet` side.** The 60-minute structural swing-cap specifically was investigated further
and confirmed genuinely unportable, not merely deferred: `SHORT_MKT_ACTION`
(`IntermediateMarketAction`, the indicator `CalculateScaleOutTargets()` reads for
`swingHigh()`/`swingLow()`/`upperRejected()`/`lowerRejected()`) is explicitly marked
`WireClass::non_wire_internal` in `include/generated/indicator_binding_policy_generated.h:105`
— this codebase's own generated wire policy, independent of anything the `lbrnet` repo controls.
Closing this specific piece would require adding new fields to `mts_schema.fbs` and wiring this
C++ codebase to populate them, out of scope for a Python-only fix.

---

## Finding 15 [INFO] — `RiskManager.cpp:1530`'s Pareto-confidence-sigmoid comment does not match its own formula

Found while porting `CalculateSafePositionSize()`'s sizing chain to Python (Finding 13) and
needing exact expected values for test assertions.

```cpp
// f(0.50) ≈ 0.5, f(0.75) ≈ 1.0, f(0.90) ≈ 1.7, f(0.95) ≈ 2.2, f(0.99) ≈ 2.8
const double paretoMultiplier = 0.5 + 2.5 / (1.0 + std::exp(-12.0 * (static_cast<double>(modelConfidence) - 0.75)));
```

Direct computation: `f(0.50)=0.62, f(0.75)=1.75, f(0.90)=2.65, f(0.95)=2.79, f(0.99)=2.87` — every
value diverges from the comment, most severely at the claimed inflection point. This isn't loose
rounding: a logistic curve of the shape `floor + range/(1+exp(...))` has its inflection value
*exactly* at `floor + range/2` — for this formula that's `0.5 + 2.5/2 = 1.75`, never `1.0`, for
any coefficient choice. The comment describes a curve shape (0.5x floor, 3.0x ceiling, midpoint
~1.0x) that the actual `floor`/`range` values (0.5, 2.5) cannot produce simultaneously with a
1.0x inflection — `range` would need to be `1.0` (giving floor+range=1.5x ceiling, contradicting
"3.0x at very high confidence") for the comment's midpoint claim to hold.

**Fix**: either the comment or the formula needs correcting, depending on which behavior was
actually intended (0.5x-to-3.0x range with a 1.75x inflection, vs. some other floor/range
combination that actually gives a 1.0x inflection). Not fixed here — this repo doesn't own the
C++ codebase. The Python port (`lbrnet/backtest/backtest_runner.py::_pareto_confidence_multiplier`)
implements the formula as written (what actually executes in production), not the comment.

---

## Finding 16 [INFO, LOW severity] — `RiskManager.cpp:1916`'s ATR-volatility-multiplier comment is a loose approximation of its own formula

```cpp
// ratio=2.0 → 0.75, ratio=3.0 → 0.50, ratio=4.0 → 0.25, ratio=6.0 → 0.05
const double mult = std::exp(-0.35 * (atrRatio - 1.0));
```

Direct computation: `mult(2.0)=0.705, mult(3.0)=0.497, mult(4.0)=0.350, mult(6.0)=0.174` — the
`ratio=3.0`/`4.0`/`6.0` examples diverge further from the comment as the ratio grows (0.35 vs
0.25 at ratio=4; 0.17 vs 0.05 at ratio=6). Lower severity than Finding 15 — likely just loose
illustrative rounding by whoever wrote the comment rather than a design/formula mismatch, since
the qualitative shape (monotonic decay, no hard floor) matches. Noted for completeness since it
was found in the same pass; the Python port again implements the formula, not the comment.

---

## Finding 17 [MEDIUM] — `patternId` fallthrough silently reinterprets a `RaschkeTacticalTrigger` id as an unrelated `RaschkeStrategySetup` id

**Deferred by explicit user directive — see "Timing" note at the end of this
finding. Not to be actioned until after the `lbrnet` Phase 2 triple-barrier
migration ships a deployable model.**

Found from the `lbrnet` side while auditing all 9 `RaschkeTacticalTrigger`
patterns for the Phase 2 triple-barrier migration
(`../lbrnet/docs/ADR/raschke_pattern_stop_target_audit.md`).

`PositionManager.cpp:1941-1944`:

```cpp
bool priceCalcOk = CalculateTacticalTriggerPrices(sc, patternId, isLong, dofScaledAtr, entryPrice, stopPrice, targetPrice);
if (!priceCalcOk) {
    priceCalcOk = CalculateStrategySetupPrices(sc, patternId, isLong, dofScaledAtr, entryPrice, stopPrice, targetPrice);
}
```

`CalculateTacticalTriggerPrices` (`PositionManagerPatterns.cpp:179-296`)
`static_cast`s `patternId` to `RaschkeTacticalTrigger` and only has cases for 3
of 9 pattern pairs (Turtle Soup, Momentum Pinball, Elder Breakout) — it returns
`false` for the other 6. `CalculateStrategySetupPrices`
(`PositionManagerPatterns.cpp:298-457`) then receives the **same integer** and
`static_cast`s it to `RaschkeStrategySetup` — a *different* enum entirely
(Raschke's Screen2 structural setup patterns: `THREE_BAR_TRIANGLE`, `NR4`,
`NR7`, `IDNR4`, `TWO_B_REVERSAL`, `HOLY_GRAIL_BUY/SELL`, `DOUBLE_REPO`,
`DOUBLE_REPO_FAILURE`, etc. — `Indicator.h:123-144`). Because the two enums'
numeric IDs were assigned independently, a `RaschkeTacticalTrigger` id that
falls through can land on a semantically unrelated `RaschkeStrategySetup` case
purely by numeric coincidence:

| Falls-through tactical trigger (id) | Lands on `RaschkeStrategySetup` (id) | Effect |
|---|---|---|
| `KANGAROO_TAIL_BUY`/`SELL` (1, 2) | `THREE_BAR_TRIANGLE`/`NR4` (1, 2) — cased, compression group | Compression-breakout formula applied to a candle-wick reversal pattern |
| `NR7_BREAKOUT_SELL` (10) | `ANTI` (10) — uncased → default | Generic 2R fallback, asymmetric with its own BUY side (which lands on `TWO_B_REVERSAL`, cased) |
| `ITR_FADE_BUY`/`SELL` (13, 14) | `HOLY_GRAIL_BUY`/`SELL` (13, 14) — **cased** | **Trend-continuation pullback formula applied to an explicitly counter-trend fade pattern** — the most severe instance, a philosophical inversion, not just an unrelated coincidence |
| `STOCHASTIC_POP_SELL` (18) | `DOUBLE_REPO` (18) — cased | 0.4×ATR stop, asymmetric with its own BUY side (default, 0.5×ATR) |

The remaining 6-pattern fallthroughs (Kangaroo Tail, NR7 Breakout, ITR
Breakout, ITR Fade, RSI Failure Swing, Stochastic Pop) all get stop/target
prices that were never designed for them — an accident of enum-ID overlap
between two unrelated pattern-classification systems, not intended behavior.

**Fix**: give `CalculateStrategySetupPrices` its own dedicated pattern-ID
namespace check (e.g., assert/guard that `patternId` is actually a valid
`RaschkeStrategySetup` value before the switch, or route based on which enum
space the caller's `patternId` actually belongs to rather than trying one cast
and falling back to the other blind). Longer-term, either give every
`RaschkeTacticalTrigger` pattern a real case in `CalculateTacticalTriggerPrices`
(closing the gap that causes the fallthrough at all), or make the fallthrough
path fail closed (reject the trade, log, alert) rather than silently producing
a plausible-looking but semantically wrong price.

**Timing (explicit user directive, 2026-07-12)**: do not action this finding
now. The `lbrnet` Phase 2 triple-barrier migration
(`../lbrnet/docs/ADR/phase2_triple_barrier_migration_spec.md`) is already
underway and its Python side does not use this fallback path as ground truth
for anything (the migration's own audit confirmed `_LABEL_TARGET_PARAMS`'s
existing defaults were already the right call specifically *because* this
fallback is untrustworthy — see the audit doc above). Revisit this finding
once Phase 2 is complete and there is a deployable model; fixing it before
then is not on the critical path and would compete for attention with work
that is.

---

## Finding 18 [HIGH] — `RiskManager::EvaluateHardGates()`'s Shannon-entropy hard gate compares raw bits against a threshold authored for a normalized ratio, firing on effectively 100% of real observations

Found from the `lbrnet` side (`docs/ADR/params_convergence_spec.md`, PC-14),
2026-07-13, during the paradigm-convergence gate audit that already found and
fixed Finding-adjacent bugs in `taleb_kurtosis_crisis` and
`taleb_cliff_proximity` (PC-11/PC-13, same audit).

`RiskManager.cpp:820-824`:

```cpp
if (ctx.shannonFlowEntropy > 0.90f) {
    return Result<void>::Failure(
        "HARD_GATE: Shannon entropy critical"
        " | shannon_entropy=" + std::to_string(ctx.shannonFlowEntropy) +
        " threshold=0.90");
}
```

`ctx.shannonFlowEntropy` is sourced from `InformationEngine::GetShannonEntropy()`
(`InformationEngine.h:259-278`), whose own docstring states: *"Entropy in bits
(0.0 to ~3.32 for k=10)"* — raw Shannon entropy over K=10 SAX bins
(`MapToBin()`), **not** a normalized `[0,1]` ratio. Verified from the `lbrnet`
side against real `.alpha` production data (200,000-event sample, OOS window):
range **[1.386, 3.282]**, median 2.77 — **100% of real observations exceed
0.90** under this raw comparison. The threshold was clearly authored assuming a
normalized ratio (dividing by `log2(10) ≈ 3.3219`, the fixed K=10 entropy
ceiling, restores sane semantics: the same real-data sample normalizes to an
**11.75%** firing rate, p90=0.905 — a plausible rare-tail crisis gate instead
of an unconditional block).

**This gate is live** — unlike Findings 12-14 (Python-port parity gaps) and
Finding 17 (deferred pending a `lbrnet` milestone), this is a genuine C++
correctness bug currently affecting real order placement, not just backtest
fidelity.

**Deeper, related issue (not part of this units-mismatch bug, but shares the
same root cause)**: `GetShannonEntropy()`'s own input, `MapToBin()`
(`InformationEngine.h:331-357`), uses fixed, absolute basis-point bin
boundaries calibrated for ~2bps sigma on 1-minute bars — never adapted to this
system's actual **15-minute** bar granularity (corrected 2026-07-13,
previously misstated as 15-second in this finding — verified via
`EventDataCollectorStudy.cpp:61`'s `EXPECTED_SECONDS_PER_BAR=900`, and its own
comment that historical replay/training data reflects "one return per
completed bar"), and never made volatility-relative despite the function's
own comment describing an intended sigma-relative scheme that was never
implemented. High-frequency returns are well-documented to be more leptokurtic
(fat-tailed) than lower-frequency ones in general — though the specific
severity of this at 15-minute granularity (vs. the originally-assumed
15-second) has not been re-verified and may be less pronounced; forced through
these fixed, timeframe-mismatched bins, the resulting entropy value carries a
distortion (heavy central-bin concentration most of the time, periodic jumps
to outer bins) independent of whatever threshold or normalization is applied
downstream regardless. SAX (the technique `MapToBin()` implements)
canonically assumes Gaussian-distributed input for its breakpoints to produce
equiprobable symbols; the standard correction for non-Gaussian (fat-tailed)
input, per the original SAX literature, is empirically-derived (quantile)
breakpoints rather than assumed-Gaussian ones — the same percentile-rebase
technique already used to fix `taleb_kurtosis_crisis` (Finding-adjacent,
PC-11, `lbrnet` side).

**Fix, in two parts**:
1. **Units mismatch (this finding's primary bug)**: divide
   `ctx.shannonFlowEntropy` by `log2(10)` (or an equivalent named constant)
   before the `0.90` comparison in `EvaluateHardGates()` — a one-line change,
   restores the threshold's evident intended semantics.
2. **Binning distortion (deeper, larger lift, shared with a `lbrnet`-side-only
   finding on `bias_veto_short_into_bullish_trend`'s dead entropy veto)**:
   replace `MapToBin()`'s fixed absolute-bps breakpoints with empirical
   quantile breakpoints derived from a real, representative sample of actual
   15-minute bar price changes (corrected 2026-07-13, previously misstated as
   15-second — see the correction note above). Should be scoped and fixed
   once, serving both this gate and any future consumer of
   `GetShannonEntropy()`, rather than patched
   independently per call site.

**Not yet fixed here.** The `lbrnet` side has applied a partial fix (item 1
equivalent, Python replica only — `backtest/backtest_runner.py`,
`_SHANNON_ENTROPY_MAX_BITS`); this C++ source is untouched and still fires on
effectively every real entry. Item 2 is unimplemented on either side. Per the
`params_convergence_spec.md` standing rule, implementing either fix here
requires its own separate, explicit authorization given the live-trading
stakes — this finding is preparation/tracking only.

---

## Finding 19 [HIGH] — the raw liquidity-fragility signal `RiskManager`'s hard gate checks is never itself serialized to the wire; only a differently-scaled proxy is, and `lbrnet` reads that proxy against the wrong threshold

Found from the `lbrnet` side (`docs/ADR/params_convergence_spec.md`, PC-15),
2026-07-13, auditing the third of three gates found still causing persistent
100% rejection in `gates_on` after Finding 18's fix. **Unlike Finding 18, this
one is not a C++ correctness bug** — `RiskManager`'s own gate is internally
consistent. The gap is that the raw signal it depends on is never exposed
outside the process, so `lbrnet`'s Python replica cannot faithfully reproduce
this gate with what's actually on the wire.

**The real gate is well-calibrated.** `CalculateLiquidityFragility()`
(`StudyHelperFunctions.cpp:3676-3743`, "v3 — Institutional Redesign, Almgren &
Chriss + Gemini Review") computes a genuine microstructure fragility score —
bar-range/ATR expansion + volume-depletion amplifier, sigmoid-mapped,
`std::clamp`'d to `[0,1]`, asymmetric-EMA-smoothed — with its own documented
calibration: *"typical barRange/ATR is 0.3-1.5 in normal conditions, producing
fragility of 0.0-0.35 (normal) and 0.4-0.8 (stressed)"*. `RiskManager.cpp:832-
836`'s `ctx.spreadStress > 0.85f` hard gate is a legitimate rare-tail cutoff on
this raw, `[0,1]`-bounded value.

**But that raw value never reaches `.alpha`/`.context`.**
`ContextManager.cpp:536` correctly assigns `m_localRiskContext.spreadStress =
obs[OBS_LIQ_FRAGILITY]` from the raw, unscaled observation array — the live
gate is fine. But a separate step later in the same file (`"PHASE 2A: Hybrid
Feature Scaling"`, `m_featureScaler.UpdateAndNormalize(rawObs)`, explicitly
logged as `"dim12_logz=" + currentObs[OBS_LIQ_FRAGILITY]`) log-z-scores and
6-sigma-winsorizes the *same dimension* before it enters the 16D observation
vector that actually gets serialized to `.alpha`/`.context`. The raw `[0,1]`
value is never itself written out as its own field — only this scaled,
model-input proxy is. `lbrnet`'s Python replica (`event_proxy.py::
liq_fragility`) reads `observation[12]` — the scaled proxy — and, until this
finding, compared it directly against the same `0.85` the real gate uses on
the *raw* value.

Verified from the `lbrnet` side against real `.alpha` production data
(200,000-event sample): the wire value ranges **[-4.66, 6.0]** (6.0 = the
winsorization ceiling — matches the "6-sigma winsorization" comment exactly),
median 0.0 — unambiguously not the raw `[0,1]`-clamped score, which cannot be
negative or exceed 1 by construction. Comparing the scaled proxy against `0.85`
fired on **31.3%** of real observations, far from the intended rare-tail
semantic.

**Corrected framing**: the observation vector's scaling is not itself a
defect — it is correct, deliberate feature engineering for its actual sole
consumer, the Student-t HMM (`CheckAndTriggerHMM()` consumes `currentObs`
directly, and that same scaled vector is what gets serialized as
`ObservationData`). **This codebase already has precedent for the fix this
needs**: this same file's own comment states *"AsymmetryContext 8D is NOT
scaled here. It is used as raw embedding lookup"* — a second, deliberately
unscaled wire channel already exists specifically for consumers that need raw
values, alongside the HMM-facing observation vector. The gap is simply that
`liq_fragility` (and, see Finding 20, `tail_index`) never got a field on that
existing raw channel — this is not a new architectural pattern to invent, it
is extending one already shipping in production.

**Fix, for the next release**: add the raw `[0,1]` fragility value
(`obs[OBS_LIQ_FRAGILITY]` before feature scaling) as a new field on the
existing unscaled channel — the same one `AsymmetryContext` already uses for
this exact purpose — so a downstream consumer (Python parity backtest, or any
future non-model consumer) can read the same representation `RiskManager`'s
real gate uses, instead of the model-input-scaled proxy. This is the
permanent fix; anything done on the `lbrnet` side in the meantime is
explicitly a stopgap (see below).

**⚠️ `lbrnet` currently carries a TEMPORARY empirical workaround that must be
removed once this ships.** Per explicit user instruction (2026-07-13),
`lbrnet`'s Python replica (`backtest/backtest_runner.py`,
`_LIQ_FRAGILITY_TAIL_THRESHOLD`) was rebased to the empirical p95 of the real
*scaled* wire value (≈3.98) instead of `0.85`, purely to get a working backtest
in the interim. This rebase does **not** make the Python gate mean the same
thing as the real C++ gate — it only stops it from blocking ~100% of
observations on a proxy it was never meant to be compared against this way.
Once this finding's fix ships and the raw signal is available on the wire,
**the `lbrnet` rebase must be deleted and replaced with a read of the real raw
value** — see `docs/ADR/params_convergence_spec.md` PC-15 for the full tracking
record and removal instructions on that side.

**Not yet fixed here.** Requires its own separate, explicit authorization
given the live-trading stakes and schema-change blast radius — this finding is
preparation/tracking only.

---

## Finding 20 [HIGH] — same root cause as Finding 19, but the raw signal's absence leaves `lbrnet`'s Python replica unable to even rebase a threshold (structurally bimodal data)

Found from the `lbrnet` side (`docs/ADR/params_convergence_spec.md`, PC-16),
2026-07-13, auditing the last of three gates found still causing persistent
100% rejection in `gates_on` after Findings 18/19's fixes. Same general family
as Finding 19 — a wire value that is a scaled proxy of the raw engineering
value `RiskManager`'s hard gate needs, never itself serialized — but the
specific transform here makes the wire data unusable for any temporary
threshold-based workaround at all, unlike Finding 19's.

**Confirmed (user-directed check): the observation vector's scaling is
deliberate and correct — not itself a defect.** `ContextManager::
CheckAndTriggerHMM()` consumes the scaled vector directly as the Student-t
HMM's actual input, and that same scaled vector is what gets serialized as
`ObservationData`. This is correct, intended feature engineering for the HMM.
**The same precedent that resolves Finding 19 applies here**: `AsymmetryContext`
already demonstrates this codebase's pattern for a second, deliberately
unscaled wire channel for consumers that need raw values — `RiskManager`'s
`ComputeParetoTopStateRatioProxy()` (`PositionManager.cpp:63-69`) already uses
a raw, in-process value (`ctx.paretoTailAlpha` / `m_cachedHillAlpha`)
internally; it was simply never given a field on that existing raw channel.

**The formula and the real threshold are both fine.** `lbrnet`'s Python port
of `ComputeParetoTopStateRatioProxy()` is an exact match, and
`load_hmm_regime_risk_policy()` correctly reads the live production
`hmm_regime_risk_policy.json` (`pareto_top_state_ratio_max=0.5`, verified
directly, not the 0.25 fallback).

**But `tail_index` (`observation[9]`) is not the raw Hill Estimator Alpha.**
`ContextManager.h`'s `FeatureScaler` applies `ScaleMode::SOFTLOGZ` to dim 9:
`ToSoftLogZ(z) = copysign(log1p(clamp(z, -6, 6)), z)` — a robust z-score of
Hill alpha *relative to its own rolling baseline*, 6-sigma-clamped, soft-log-
compressed. `log1p(6) = 1.94591...` matches the real observed wire range
`[-1.9459, 1.9459]` exactly (verified against 200,000 real `.alpha` events).
Real raw Hill alpha must be `0.0` (warmup) or in `[1.1, 8.0]`
(`ContextManager.cpp`'s own clamp) — only 15.45% of nonzero wire values fall
in that range at all.

**Unlike Finding 19, no threshold rebase can fix this on the `lbrnet` side.**
`tail_index`'s median is 0.0 (a z-score centered at its own baseline), so any
negative or near-zero value — roughly half of all real data — triggers the
formula's "invalid alpha" fail-safe (`ratio=1.0`) unconditionally, regardless
of actual market conditions. For positive values, since `tail_index` never
exceeds 1.9459, `1/tail_index` never drops below `1/1.9459 ≈ 0.514`. Verified
precisely: **82.8%** of real observations land at *exactly* `ratio=1.0`; the
rest never drop below `0.514`. Any Python-side threshold `<1.0` therefore
rejects at minimum ~83% of everything; any threshold `>=1.0` rejects **0%**
(`ratio` is capped at exactly `1.0` by construction, so nothing can ever
exceed it) — there is no middle ground, unlike Finding 19's continuously-
distributed proxy.

**Fix, for the next release**: add the raw Hill alpha (`m_cachedHillAlpha`,
already computed and cached in-process) as a new field on the existing
unscaled channel — the same one `AsymmetryContext` already uses — so a
downstream consumer can read the same representation `RiskManager`'s real
gate uses, instead of the model-input-scaled proxy. This is the permanent
fix.

**`lbrnet` removed this gate from its Python replica entirely** rather than
attempt a threshold rebase (per explicit user instruction, 2026-07-13) — a
rebase here would either reject ~83-100% of everything or silently disable
the gate while looking calibrated, neither of which is an honest stopgap.
This is an intentional, tracked Python/C++ parity divergence, same pattern as
Findings 12-14 (parity gaps) — not a repair, and not something requiring
removal-for-removal once the C++ fix ships (there is no rebase to undo, only
a gate to reintroduce once the raw signal exists on the wire — see
`docs/ADR/params_convergence_spec.md` PC-16 for the full tracking record).

**Not yet fixed here.** Requires its own separate, explicit authorization
given the live-trading stakes and schema-change blast radius — this finding is
preparation/tracking only.

---

## Finding 21 [HIGH] — third instance of the Finding 19/20 family: `RiskManager`'s VPIN/Amihud toxicity gate checks a raw, unbounded value that is never serialized

Found from the `lbrnet` side (`docs/ADR/params_convergence_spec.md`, PC-17),
2026-07-13, auditing the third and last of the originally-identified
`gates_on` gate candidates. Same root-cause family as Findings 19/20, and the
same `AsymmetryContext` precedent applies (see Finding 19): the observation
vector's scaling is correct, deliberate feature engineering for the
Student-t HMM — not a defect. The gap is, again, that a raw value
`RiskManager`'s hard gate needs was never given a field on the existing
unscaled channel `AsymmetryContext` already demonstrates.

**The real gate checks a raw, unbounded value with no built-in ceiling.**
`RiskManager.cpp:797-812` ("GAP 26"): `ctx.vpin > vpinThreshold`
(`vpinThreshold` = `0.40` in fat-tail regimes, `regime_dof<=4.0` or
`taleb_kurtosis>8.0`, else `0.80`). `ctx.vpin` sources from
`obs[OBS_VPIN_TOXICITY]` — the raw, pre-scaling array, same pattern as
Findings 19/20's raw values. But `CalculateAmihudIlliquidity()`
(`StudyHelperFunctions.cpp:3960-3980`) computes a genuinely unbounded quantity
— its own comment: *"raw; Scaler handles normalization downstream"* — unlike
Finding 19's fragility score, there is no `[0,1]` clamp on this one at all.

**That raw value never reaches `.alpha`/`.context`.** `kObsVpinToxicity = 11`,
and dim 11 uses `ScaleMode::SOFTLOGZ` in `ContextManager.h`'s `FeatureScaler`
— the same transform as Finding 20's `tail_index` (robust z-score relative to
its own rolling baseline, 6-sigma-clamped, soft-log-compressed).

Verified from the `lbrnet` side against real `.alpha` production data
(200,000-event sample): the wire value ranges **[-1.9459, 1.9459]** (matching
Finding 20's `±log1p(6)` signature exactly). Comparing this scaled proxy
against `0.80`/`0.40` fired on **31.4%/46.8%** of real observations, far from
the "regime-aware crisis toxicity" semantic the gate's own comment describes.

**Unlike Finding 20, this proxy IS rebasable.** Only 6.9% of real observations
sit at the exact winsorization ceiling (vs. Finding 20's 82.8% point mass at
its "invalid" fail-safe value) — the distribution is continuous, and firing
rate scales smoothly with threshold. `lbrnet` empirically rebased (per
explicit user instruction) rather than remove the gate.

**Fix, for the next release**: add the raw, unbounded Amihud illiquidity
value (`obs[OBS_VPIN_TOXICITY]` before feature scaling) as a new field on the
existing unscaled channel — the same one `AsymmetryContext` already uses —
so a downstream consumer can read the same representation `RiskManager`'s
real gate uses, instead of the model-input-scaled proxy. This is the
permanent fix.

**⚠️ `lbrnet` currently carries a TEMPORARY empirical-percentile workaround
that must be removed once this ships.** Per explicit user instruction
(2026-07-13), `lbrnet`'s Python replica
(`backtest/backtest_runner.py`, `_VPIN_TOXICITY_TAIL_THRESHOLD_NORMAL`/
`_FAT_TAIL`) was rebased to the empirical p90/p75 of the real scaled wire
value instead of `0.80`/`0.40`. This rebase does not make the Python gate
mean the same thing as the live C++ gate — see
`docs/ADR/params_convergence_spec.md` PC-17 for the full tracking record and
removal instructions on that side.

**Broader observation, not yet actioned**: three of sixteen observation-
vector dimensions (9, 11, 12) have now independently been found to carry this
exact representation mismatch when consumed outside the HMM. Given
`SCALE_MODE_MAP` applies `SOFTLOGZ`/`LOGZ` to 13 of 16 dimensions, this may be
a broader pattern — worth a dedicated audit if further gate-parity issues
surface, but not actioned preemptively here.

**Not yet fixed here.** Requires its own separate, explicit authorization
given the live-trading stakes and schema-change blast radius — this finding is
preparation/tracking only.

---

## Positive finding — no internally-contradictory duplicate constants found elsewhere

Checked `MindfulTraderConstants.h`, the risk/reward ratio gate (`>= 2.0`,
`TripleScreenStrategy.cpp:113` — note: `TripleScreenStrategy.cpp` does not exist in this project;
this citation is from an unrelated legacy source and should be disregarded / was already
retracted on the Python side), position-sizing risk percentage (`0.02`), and the Keltner band
multiplier (`3.0f`, consistent across `TripleScreen2.cpp`/`TripleScreen3.cpp`). Outside Findings
6/7 above, no other duplicated-with-different-value constants were found for the same concept.

---

## Verified-correct citations (no fix needed, listed for confidence)

These specific behaviors were checked against this codebase and found to be exactly as documented
elsewhere (Python-side comments in the sibling `lbrnet` repo cite these correctly):
- `RiskManager::RefreshKurtosisEmergencyState()` (`RiskManager.cpp:841-879`): genuine two-threshold
  hysteresis, enter>5.0/exit<3.0, persistent `std::atomic<bool>` state.
- `ChandelierStopManager::GetDynamicMultiplier()` (`ChandelierStopManager.cpp:460-468`):
  `floor=2.0, onset=2.0, lambda=0.35`, PARETO floor `3.5` (lines 174-178), climax floor `1.0` /
  threshold `2.0` (lines 74-98, 190-202).
- `PositionManager::CalculateScaleOutTargets()` (`PositionManager.cpp:3048-3215`): real, current,
  multi-tier (T1/T2/T3) per-pattern R-multiple scale-out ladder, further scaled by regime
  target-width and Hurst-exponent multipliers and capped by 60-minute structural swing
  highs/lows.
- `HmmStateIndicator::DofStopScale()` (`include/Indicator.h:2035-2041`): Student-t
  degrees-of-freedom ATR-widening multiplier, actively used from 6 call sites in
  `PositionManager.cpp` to scale trailing-stop distance.
- `ExecutionGate.cpp:30`'s regime-duration gate (`shannonTenureBars < shannonMinTenureBars`,
  real threshold 143.5 bars): raw integer bar count (not subject to the SOFTLOGZ/LOGZ wire
  contamination behind Findings 19-21), correctly compared, correct real threshold loaded from
  `hmm_regime_risk_policy.json`. 143.5 bars × 15-minute bar period ≈ 35.9h — a demanding but
  plausibly intentional near-continuous same-regime persistence requirement, not a units bug
  (PC-18, 2026-07-14).
