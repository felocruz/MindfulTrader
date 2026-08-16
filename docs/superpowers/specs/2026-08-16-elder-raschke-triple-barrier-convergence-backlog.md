# Spec: Elder-Raschke / Triple-Barrier Convergence Backlog

**Status**: SPEC — approved via `superpowers:brainstorming` (2026-08-16, MindfulTrader-rooted session).
**Implementation deferred.** Every unit below is independently implementable and gets its own
`writing-plans` cycle when picked up — this document catalogs and prioritizes, it does not itself
build anything.
**Governing methodology**: `docs/superpowers/specs/2026-08-16-execution-risk-coevolution-governance-spec.md`
— every unit here that touches C++ execution/risk logic must flow through that spec's twin-first
promotion ladder, not be validated against the SC-replay backtester directly.

## Purpose

A single 2026-08-16 brainstorming session assessed three subsystems against the actual academic and
institutional literature: the 16D observation-vector/risk-gate stack, the Elder-Raschke Triple Screen
entry system, and the Chandelier→Triple-Barrier exit migration (plus a fourth pass mapping the Python
twin backtester). All four assessments surfaced genuine, specific, evidence-backed gaps — this spec
is the catalog of that work, decomposed into independently schedulable units, in the priority order
recommended at the end of the session. Reordering is expected; nothing here is sequenced by hard
dependency except where stated.

## Unit 1: Parity-Contract Test Infrastructure

**Why first**: every other unit that touches execution/risk logic needs this to validate its own
convergence per the governance spec's Mechanism 1. Building it first means every subsequent unit gets
it for free instead of each inventing its own ad-hoc check.

**Problem**: `test_risk_gate_context_contract.py`/`test_observation_contract.py` already check schema
*shape* parity between C++ and Python but not decision *agreement*. No standing test currently
confirms C++ and the Python twin agree on entry-gate outcomes, computed barriers, or position size.

**Scope**: build a fixture from a frozen, real historical `.context`/`.alpha` slice; assert agreement
on gate pass/fail (per gate), stop/target/`max_bars`, and position size; encode the four known,
intentional divergences (PC-10/PC-13/PC-16/PC-18) as expected exceptions, not failures.

## Unit 2: Stale-Comment Cleanup (`PositionManager.cpp` / `TripleBarrierExitManager.h`)

**Why urgent despite being small**: this exact stale documentation has now caused two wrong
conclusions in one session — a research agent concluded the Python twin simulated a
not-yet-shipped exit architecture, and a separate agent (correctly, but only after independent
verification) had to flag the same comment as stale for an unrelated reason earlier. This is not a
cosmetic nit; it is an active hazard to the next reader (human or AI).

**Problem**: `PositionManager.cpp:355-425`'s "Triple-Barrier shadow (Phase 1, step 1a)" block still
logs `[TB-SHADOW]` and claims "Drives NO orders (Chandelier remains authoritative)" — false today.
The actual authoritative barrier computation is `PositionManager.cpp:2004-2016`'s "STEP C" block,
which unconditionally overwrites stop/target before the SC bracket is submitted.
`TripleBarrierExitManager.h`'s own header comment has the same staleness (per the Chandelier→Triple
Barrier migration assessment this session).

**Scope**: update both comments to describe current behavior; keep the `[TB-SHADOW]` block's actual
function (latching bracket state for the time-barrier check) but rename/reword so nothing implies a
second, competing live stop/target source exists. `ChandelierStopManager` no longer exists in the
codebase at all — remove any remaining language implying otherwise.

## Unit 3: `ExitReason_TRAP` Schema Gap — **DEFERRED 2026-08-16, not dropped**

**Problem**: `BackTesterStudy.cpp:1199-1208` maps native TRAP exits to `ExitReason_MANUAL` with an
explicit `TODO(schema)` — `.btst` replay data cannot currently attribute TRAP exits. This blocks
measuring the anticipatory model layer's own stated deploy gate (out-of-sample F₀.₂₅ > 0.65,
per `CLAUDE.md`'s Trap Detection section) — the gate exists on paper with no instrument to evaluate it.

**Scope**: add `ExitReason_TRAP` (and confirm `REGIME_INVALIDATION` is separately representable,
see Unit 4) to the relevant FlatBuffer schema; regenerate per `regenerate_schema.sh`; wire
`BackTesterStudy.cpp`'s mapping to the new value.

**Why deferred, not just reordered**: `ExitReason` lives in `backtest_schema.fbs`, consumed only by
`scsf_BackTester`/`.btst` files — the SC-replay backtester, which per the governance spec's promotion
ladder only runs *after* the twin has already justified a change. The Python twin does not need this
fix at all — it computes TRAP/`REGIME_INVALIDATION` attribution independently via
`scan_lookahead_hybrid()`/`compute_barriers()` (the same function the training labeler uses), not by
reading `.btst`'s `ExitReason` enum. So F₀.₂₅ and any other TRAP-attribution metric can already be
measured in the twin without this unit. Additionally, `MindfulTrader.dll` is one shared binary across
every study — deploying a fix here to test it live would require Sierra Chart to reload the whole
module, interrupting whatever `EventDataCollectorStudy` collection is running at the time, which is
its own reason not to build-and-deploy this prematurely. Revisit once a twin-validated change is ready
for SC-replay confirmation and actually needs TRAP-exit attribution in `.btst` output.

## Unit 4: `REGIME_INVALIDATION` Live-Side Enforcement

**Problem**: the Python labeler (`triple_barrier_scanner.py`) trains a genuine 3-way outcome
(TRAP / `REGIME_INVALIDATION` / stop-target-time), and the C++ side's Q0 split (native TRAP definition
excluding `DECISIVE_*` structural breaks) is real and correctly implemented on both repos. But live
C++'s `EvaluateRegimeDefense()` (`PositionManagerPatterns.cpp:60-172`) only flattens on HMM
hostile-regime-flip / toxic-environment thresholds — it does not read `StructureTest`'s `DECISIVE_*`
values at all. This is explicitly Phase 2 scope per `triple_barrier_cutover_phase1_plan.md`, not a
hidden gap, but it is a live train/live asymmetry: the model is trained on a distinction the live
executor only half-enforces.

**Scope**: wire `StructureTest::DECISIVE_*` detection into `EvaluateRegimeDefense()` as an additional,
tick-evaluated flatten trigger, per the governance spec's "fast bad-trade exit" framing — a proactive
flatten racing ahead of the resting Triple-Barrier stop when the structure itself invalidates, using
SC's fastest flatten primitive.

## Unit 5: Profit-Protection / Alpha-Capture Mechanism (Measure First, Then Build)

**Problem**: `triple_barrier_profit_protection_ruling.md` rigorously rejected naive breakeven/trailing
(a proven whipsaw trap for this system's tight fade stops) and instead *mandated* (Rank 1)
differentiated exit objectives — fixed-target for fades (shipped, correct) and **Trend-Scanning for
breakouts** (AFML ch.5, rolling-regression slope-significance exit) — which was never built. All
patterns, including breakouts, currently run the identical static first-touch bracket
(`PositionManager.cpp:2364-2372`). The ruling's own gating metrics (Winner-to-Loser Reversal Rate,
Whipsaw Ratio, MFE Capture Ratio, Ω-delta) have their raw ingredients collected (`mfe_r`/`giveback_r`
in `backtest_runner.py`) but were never aggregated against the ruling's stated thresholds.

**Scope, staged**:
1. Run the Python twin (now confirmed trustworthy for this per the governance spec's parity
   scorecard) to compute W_LR/Whipsaw Ratio/MFE_CR/ΔΩ specifically for breakout trades.
2. If the ruling's thresholds justify it: implement Trend-Scanning for breakouts, expressed as an
   infrequent (per-completed-bar, not per-tick), evidence-gated re-price of the resting SC-native
   order — never a continuously-recomputed C++ trail, which is exactly the "two systems racing"
   failure mode the profit-protection ruling already identified and rejected once (Chandelier vs.
   bracket target). One order-management authority active at a time, always.
3. If the thresholds do *not* justify it: document the negative result and close this unit — Rank 3
   (accept the give-back) remains the correct baseline per the ruling's own Q4 sequencing logic.

## Unit 6: ADR Corpus Reconciliation — **DONE 2026-08-16**

**Problem**: two same-day rulings directly contradict each other and were never reconciled against
one another — `triple_barrier_favorable_exit_ruling.md` rules "REMOVE FROM BOTH REPOS" for
`FAVORABLE_EXIT`; `triple_barrier_gates_parity_adjudication.md` rules the opposite, "HIGHLY
LEGITIMATE — DEFER TO META-LABELER," for the same question. Engineering practice already resolved
this correctly in code (`FAVORABLE_EXIT` was actually deleted 2026-07-16, matching the removal
ruling), but the documents themselves still contradict each other for any future reader auditing the
ADRs in isolation. Separately, `triple_barrier_literature_review.md` cites a hallucinated paper
("Zhang et al. 2020, Dynamic Barring Methods for Financial Time Series") already retracted in a
sibling document but never scrubbed from the original, and mistitles Bertram (2010) in two of three
documents that cite it.

**Scope**: add a short reconciliation note to both contradicting rulings pointing to the actual
shipped resolution and to each other; correct or retract the hallucinated citation in
`triple_barrier_literature_review.md`; fix the Bertram (2010) title everywhere it appears. Purely a
documentation-hygiene unit — no code changes.

## Unit 7: Triple Screen Fidelity Fixes

**Problem, four distinct findings from the Triple Screen literature-fidelity assessment**:
1. **TS1's permission-gate mechanism is disconnected from TS1's own indicators.** The live veto
   (`Scoring::ApplyDailyBiasFilter`) reads a separate `DailyBiasEnum`/Hurst/Value-Area construct
   (`StudyHelperFunctions.cpp:1537`), not TS1's Impulse/MACD-H/Force-Index(13), which are exported as
   ML training features only. Needs a decision: wire TS1's own signals into the live veto (closer to
   Elder's literal design), or formally document the current separation as an intentional,
   accepted substitution (it does preserve Screen 1's *functional* role — permission-only, no
   independent entries — just via a different mechanism).
2. **The Anti pattern has no trend-strength gate at all** (`StudyHelperFunctions.cpp:929-1017`),
   despite `TRADING_STRATEGIES_COMPLETE_REFERENCE.md`'s own table claiming ADX>25 is required — ADX
   was retired system-wide and nothing replaced it here specifically (unlike Holy Grail, which got an
   explicit Hurst substitution).
3. **Turtle Soup and Two-B Reversal appear to have swapped canonical lookback windows** — Turtle
   Soup uses a 4-day lookback (`IndicatorComputations.h:768`) where the source definition calls for
   ~20-day (the Turtle system's own breakout channel being faded); Two-B Reversal has the 20-bar
   window (`StudyHelperFunctions.cpp:1091-1121`) that arguably belongs to Turtle Soup.
4. **`TRADING_STRATEGIES_COMPLETE_REFERENCE.md`'s "21 patterns" table is partly aspirational** — Flip,
   standalone Inside Day, and standalone Consolidation are documented as live but are
   retired/merged/dead in the actual enum (gaps at enum values 5, 6, 11 corroborate this). No `Oops!`
   gap-reversal pattern or 3/10 oscillator exists anywhere despite being named as canonical Raschke
   technique in the assessment's framing.

**Scope**: (1) resolve the TS1 permission-gate question with the user before touching code — this is
a real design choice, not an obvious bug; (2) give Anti an explicit trend-strength gate (Hurst,
matching Holy Grail's precedent, unless investigation shows a reason not to); (3) verify and correct
the Turtle Soup / Two-B lookback assignment against the source definitions; (4) update
`TRADING_STRATEGIES_COMPLETE_REFERENCE.md` to match the live enum — retire dead pattern entries,
flag Double Repo/Ghost/Slingshot/Whiplash as provenance-uncertain rather than presenting them as
confirmed Raschke-canonical.

## Unit 8: Risk-System Doc/Governance Sync (Cheap, Low-Priority)

**Problem**: `docs/RISK_MANAGEMENT_SYSTEM.md` §5/§7 still describes a live half-Kelly sizing
multiplier; `include/KellyCalculator.h`'s own docstring confirms Kelly-fraction computation was
correctly removed (30-trade samples are too small for reliable f* estimation — the doc simply never
caught up). Separately, the "Pareto top-state-ratio" gate is actually a `1/Hill-α` proxy
(`docs/ADR/gate_stack_stationarity_audit_findings.md`) — cosmetic naming debt, already tracked, no
new action needed beyond what that ADR already recorded.

**Scope**: correct `RISK_MANAGEMENT_SYSTEM.md`'s Kelly section to describe the actual current sizing
chain (Elder 2%/6% + institutional risk-multiplier chain, no Kelly). Pareto-gate renaming stays
deferred, as already decided in the stationarity audit.

## Unit 9: Longer-Horizon Research Items (Flagged, Not Scheduled)

Not actionable as simple fixes — each needs its own investigation before any implementation:

- **DFA/Hurst window under-powering** (N≈100, confirmed variance-dominated per Weron 2002 via this
  project's own Monte Carlo spike) — no cheap fix known; window-widening or a different estimator
  are the only real levers.
- **HMM regime-posterior branch inactive in the deployed model** (`num_regime_features=0` per
  `docs/architecture/STUDENT_T_HMM_EVENT_TRANSFORMER_ARCHITECTURE.md`) — the canonical, policy-intended
  regime-conditioned behavior is not what's running today.
- **lbrnet-side stale `taleb_signal_sigma_threshold` copies** (`backtest_runner.py`'s hardcoded
  `9.636797`, `HMMEmpiricalGateThresholds.json`'s `6.67559116507085`) — needs an lbrnet-rooted
  session, per this project's repo-boundary convention.
- **No PBO/CSCV/deflated-Sharpe discipline.** The governance spec's twin-first methodology is exactly
  the enabling tool for building this eventually (walk-forward/resampling is cheap against the twin,
  too expensive against the SC-replay backtester) — not scheduled as its own unit yet, but explicitly
  unblocked by adopting the governance spec.

## Unit 10: Cross-Language Enum Header Consolidation into `rc_enums.h` (Cheap, Low-Priority)

**Problem**: discovered while building `PredatorContext.h` (Predator infrastructure plan, Task 1) —
`HMMStateEnum` lived inline in `Indicator.h`, which includes `sierrachart.h`, so anything needing the
enum without ACSIL dragged in the whole SC header transitively. Fixed narrowly by extracting it into
`include/rc_enums.h`, named to mirror `lbrnet/lbrnet/core/rc_enums.py` (the Python cross-language-parity
enum module) rather than a one-off `HMMStateEnum.h`, so it can serve as the single canonical home for
future extractions instead of spawning another single-enum header each time. `MacdEnum`,
`KangarooTailEnum`, `TurtleSoupEnum`, `MomentumPinballEnum`, `ElderBreakoutEnum`, and `NR7Enum` are
already ACSIL-independent but still live scattered in `IndicatorComputations.h` (each with live call
sites) — not moved as part of that fix, since consolidating already-working, already-tested code was
out of scope for a Task-1 dependency fix.

**Scope**: migrate `MacdEnum`/`KangarooTailEnum`/`TurtleSoupEnum`/`MomentumPinballEnum`/
`ElderBreakoutEnum`/`NR7Enum` (and any other cross-language-parity enum found scattered elsewhere) into
`include/rc_enums.h`, updating every include site and re-running the full native test suite plus
`./build_dll.sh` to confirm zero behavior change. Purely mechanical (move + re-point includes), no
logic changes — still deserves its own isolated commit given the number of touched files.

**Not scheduled** — flagged here so it isn't lost, not queued ahead of the Predator/Turtle-Soup plan
currently executing.

## Non-Goals

- **Any unit here bypassing the governance spec's promotion ladder.** Every unit touching live
  execution/risk logic gets twin-validated first, per that spec, even where the fix looks obviously
  correct.
- **Re-opening Unit A of `docs/superpowers/specs/2026-08-15-risk-gate-context-cpp-coevolution.md`**
  (the `risk_gate_context` population-gap trace) as part of this backlog — that unit is tracked in
  its own spec, held pending a live SC trace, and is out of scope here.
- **Building the PBO/CSCV framework as part of this pass.** Flagged in Unit 9 as enabled, not
  scheduled — needs its own brainstorming session once the higher-priority units land.

## Investigation Log

- **2026-08-16**: Backlog drafted at the close of a `superpowers:brainstorming` session covering four
  research passes (16D risk-gate system vs. literature, Triple Screen vs. Elder/Raschke's actual
  published methodology, Chandelier→Triple-Barrier migration vs. AFML, and the Python twin's
  architecture/parity map) plus direct user-driven design discussion on in-trade profit protection
  and the twin-first co-evolution methodology now captured in the companion governance spec. Priority
  order above reflects the session's own recommendation (parity infrastructure and cheap,
  already-demonstrated-hazardous stale comments first); the user has not yet confirmed this exact
  ordering.
