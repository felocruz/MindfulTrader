# Spec: C++/Python Execution-Risk Co-Evolution & Institutional Promotion Ladder

**Status**: SPEC — approved via `superpowers:brainstorming` (2026-08-16, MindfulTrader-rooted session).
**Implementation deferred.** This spec establishes the *governing methodology* by which every
concrete work item in the companion backlog spec (`2026-08-16-elder-raschke-triple-barrier-convergence-backlog.md`)
gets built and validated. It does not itself fix anything.
**Prerequisite reading**: `docs/BACKTESTING_FRAMEWORK.md` (the existing Historical→Simulated→Paper→Live
ladder this spec adds a cheaper pre-stage to), `docs/SCHEMA_DRIVEN_SERIALIZATION_PARITY_INITIATIVE.md`
and `docs/ADR/params_convergence_spec.md` (the two existing, narrower precedents this spec
generalizes), `docs/ADR/triple_barrier_profit_protection_ruling.md` (the model for what a rigorous,
metric-gated promotion decision looks like — this spec makes that pattern standard, not a one-off).

## Purpose

A single 2026-08-16 brainstorming session assessed three subsystems against the actual literature
(the 16D observation-vector/risk-gate stack, the Elder-Raschke Triple Screen entry system, and the
Chandelier→Triple-Barrier exit migration) and, separately, mapped `lbrnet`'s pure-Python "Phase 2"
backtester (`backtest_runner.py::run_backtest_alpha()`) against live C++. Two things fell out of that
work that matter more than any single fix: (1) real, valuable convergence work exists across all
three subsystems (cataloged in the companion backlog spec), and (2) the Python twin, once corrected
for one investigation error, is a genuinely trustworthy fast-iteration proxy for C++ across gate
thresholds, sizing, *and* exit/barrier mechanics — with fills/slippage realism as its one honestly
disclosed weak spot. This spec turns that second finding into a standing methodology: **build every
future execution/risk change against the cheap Python twin first, promote it up a staged confidence
ladder, and enforce twin/C++ parity mechanically rather than by hoping documentation stays current.**

The forcing example for why the enforcement piece is non-negotiable, not a nice-to-have, happened
*during this same brainstorming session*: a stale comment in `PositionManager.cpp` ("Chandelier
remains authoritative... Drives NO orders") led one research agent to conclude the Python twin was
simulating a not-yet-shipped exit architecture — a wrong conclusion, caught only because the
conversation's owner traced the actual order-construction code by hand. A second, independent stale
artifact (a pre-fix baseline number for `dim3`'s rail-hit rate) nearly got treated as a live
regression earlier in the same session. Two independent near-misses from documentation drift, in one
conversation, is the concrete evidence this spec is reacting to — not a hypothetical risk.

## The Twin, Precisely Defined

`lbrnet/backtest/backtest_runner.py::run_backtest_alpha()` (entry points: `run_phase2.py`,
`multi_window_eval.py`). SC-free — no ZMQ, no Sierra Chart process. Streams `data/raw/event_data.alpha`
(a real FlatBuffer `TrainingEvent` stream written by C++'s `IndicatorManager`/`EventDataCollectorStudy`,
not Python-derived), runs the real production Transformer + HMM decision stack in-process
(`OfflineBacktestAgent(LiveAgent)`), applies a Python port of the C++ risk-gate stack
(`_evaluate_live_entry_gates`) and Elder position sizing (`VirtualBroker`), and resolves every trade's
exit via `scan_lookahead_hybrid()`/`compute_barriers()` from
`lbrnet/lbrnet/labeling/triple_barrier_scanner.py` — **the same function the training labeler uses**,
confirmed by direct import, not two parallel implementations.

This is distinct from `backtest_server.py` (the ZMQ server that serves C++'s `BackTesterStudy` during
real Sierra Chart replay — that's the *live* backtester's Python half, already covered by
`docs/BACKTESTING_FRAMEWORK.md`, not this spec) and from `backtest_runner.py`'s own Phase 1 mode
(`run_backtest`/`sweep_tau`, which only sweeps thresholds over already-labeled parquet data — not a
simulator).

## Parity Scorecard (established 2026-08-16, carry forward — do not re-derive)

| Decision surface | Status | Confidence |
|---|---|---|
| Indicators/features | Consumed directly from real C++-computed `.alpha`/`.context` | High |
| Exit-barrier placement | Shares the literal same `compute_barriers()` function as C++'s `TripleBarrierExitManager`/`STEP C` (`PositionManager.cpp:2004-2016`, verified authoritative by direct code trace) | High, *after* correcting an initial wrong finding — see Investigation Log |
| Position sizing | Verified port of the real Elder risk-multiplier chain; correctly does not assume Kelly (matches Kelly's actual removal, see backlog Unit 8) | High |
| Entry risk gates | Ported gate-by-gate with C++ line citations; several gates *deliberately* diverge because live C++ itself has a known, dated bug there (PC-10/PC-13/PC-16/PC-18) | Medium-High — divergences are itemized, not silent |
| Fills/slippage | Idealized: exact barrier-price touch + flat 0.021R tax, no queue-position/market-impact modeling | **Low — the one real, disclosed weakness** |
| Regime/HMM | Consumes stored `cached_regime_state` from `.alpha` rather than re-inferring; the real production Transformer decision model runs in-process | High for the decision model, medium for regime-state freshness |

## The Promotion Ladder

```
Python twin (cheap, fast, iterate freely)
        │  gated by: parity-contract test passes + relevant gating metrics clear threshold
        ▼
SC-replay backtester (BackTesterStudy, authentic sc.* fill callbacks, docs/BACKTESTING_FRAMEWORK.md)
        │  gated by: no material surprise vs. twin's prediction
        ▼
Live simulated / paper trading (SIT — catches queue-position/latency leakage twin+replay both miss)
        │  gated by: SIT passes per BACKTESTING_FRAMEWORK.md's existing criteria
        ▼
Live
```

Each stage answers a *different* question, per `docs/BACKTESTING_FRAMEWORK.md`'s own framing, which
this spec explicitly inherits rather than reinvents: the twin and historical backtest answer "is the
model/strategy any good" (win rate, R-multiple distribution, gate behavior); paper trading is a System
Integration Test that answers "does execution leak edge" (queue position, broker latency) and
explicitly does *not* re-validate model quality. Do not skip a stage because an earlier one looked
clean — they are not redundant, they check different things.

## Mechanism 1: Parity-Contract Enforcement (Mechanical — the Part That Must Not Be Skipped)

A new standing test, generalizing the existing `test_risk_gate_context_contract.py`/
`test_observation_contract.py` pattern (which today only checks *schema shape*) to also check
*decision agreement*: given a frozen, fixed historical fixture (a real slice of `.context`/`.alpha`
data), assert that C++'s recorded output and the Python twin agree on:

1. Entry-gate pass/fail, per gate, per record.
2. Computed stop/target/`max_bars` (the Triple-Barrier engine output).
3. Position size.

Documented, dated, intentional divergences (the PC-10/PC-13/PC-16/PC-18 known-live-bug class) are
**allowed and expected** — the test's job is to make *unintentional* divergence loud immediately, not
to force mechanical equivalence where a conscious decision says otherwise. Any new divergence the test
surfaces must get the same treatment those four got: a dated ADR-style note explaining why, or a fix.

This test must be run whenever either side's execution/risk logic changes. Per this project's own
established governance style (solo developer + AI agents, structural correctness preferred over
procedural overhead — see `docs/superpowers/specs/2026-08-15-risk-gate-context-cpp-coevolution.md`'s
Finding 6), this does not require a CI gate or blocking approval workflow — it requires the test to
*exist* and to be a standing item in the `CLAUDE.md` Done Checklist for any change touching entry
gates, sizing, or exit-barrier logic on either side of the twin.

## Mechanism 2: Narrative Handoff (Complement, Not Substitute)

Continue the existing cross-repo `SCRATCHPAD.md`/`scratchpad.md` dated-note convention (already
proven useful this session for context handoff between MindfulTrader- and lbrnet-rooted sessions).
This mechanism is explicitly **not sufficient alone** — it depends on complete, correct notes and on
someone reading them before touching related code, and this exact session produced two counterexamples
where documentation existed and still misled a reader. Mechanism 1 is what actually enforces
co-evolution; this mechanism is what makes a *future session* efficient once parity is already true.

## "Institutional Enough to Transition" — Concrete Criteria

Before promoting any execution/risk design change from the twin to the SC-replay backtester:

1. The parity-contract test (Mechanism 1) passes for the decision surface being changed, or every
   failure is a dated, reasoned, accepted divergence — not an unexplained one.
2. Whatever specific gating metrics apply to the change clear their pre-defined thresholds against
   the twin. (Example already established: `triple_barrier_profit_protection_ruling.md`'s
   Winner-to-Loser Reversal Rate < 0.20, Whipsaw Ratio > 0.30 rejection threshold, `ΔΩ > 0` at 95%
   confidence — this is the template for how *every* future promotion decision should be gated, not
   a one-off.)
3. A small, cheap confirmation pass in the SC-replay backtester shows no material surprise relative
   to what the twin predicted.

Only after all three: proceed to paper/live-simulated trading per `docs/BACKTESTING_FRAMEWORK.md`'s
existing SIT criteria.

## Known, Disclosed Limits of the Twin (Carry Forward, Do Not Relitigate)

- **Fills/slippage are idealized.** Any design decision sensitive to tight-stop execution quality
  (fade-pattern whipsaw behavior, entry-timing precision) must get the SC-replay confirmation step —
  twin-only validation is not sufficient for these specifically.
- **PC-10/PC-13/PC-16/PC-18 gate divergences are intentional.** The twin is, in these four spots,
  more correct than current live C++. Do not "fix" the twin to match a gate that's itself a
  documented bug; fix the live gate instead, tracked as its own backlog item if not already.
- **No PBO/CSCV/deflated-Sharpe discipline exists yet.** Heavy twin-only iteration risks overfitting
  to the twin's own idiosyncrasies (the flat slippage tax, the lack of queue modeling) in a way a
  fixed historical fixture cannot catch. The twin is exactly the tool needed to eventually build this
  (walk-forward/resampling is cheap against it, too expensive against the SC-replay backtester) — this
  is flagged in the backlog spec as a longer-horizon item this governance model directly enables, not
  something this spec builds.

## Non-Goals

- **Rewriting the twin's fill model.** Idealized fills are a known, accepted limitation, mitigated by
  the SC-replay confirmation step in the ladder — not something to fix as part of adopting this
  methodology.
- **A CI gate or blocking merge check.** Consistent with this project's established governance
  preference (structural correctness over procedural overhead) — the parity-contract test must exist
  and be run, not be enforced by tooling that blocks commits.
- **Fixing any specific gap this spec's audit surfaced.** Every concrete fix (Trend-Scanning,
  `REGIME_INVALIDATION` wiring, Triple Screen fidelity, ADR reconciliation, etc.) lives in the
  companion backlog spec and is executed *through* this ladder, not inside this document.
- **Replacing `docs/BACKTESTING_FRAMEWORK.md`.** This spec adds a cheaper pre-stage in front of an
  existing, working validation ladder; it does not alter the SC-replay/paper/live stages themselves.

## Investigation Log

- **2026-08-16**: Spec drafted at the close of a long `superpowers:brainstorming` session that (1)
  graded the 16D risk-gate system, Triple Screen, and Chandelier→Triple-Barrier migration against the
  literature (see companion backlog spec for the itemized findings), (2) mapped the Python twin's
  architecture and parity status, catching and correcting a wrong initial finding about exit-barrier
  parity (an agent misread a stale `PositionManager.cpp` comment; direct code tracing at
  `PositionManager.cpp:2004-2016` confirmed the engine barriers are unconditionally authoritative,
  matching the twin), and (3) reached explicit user agreement on pairing narrative scratchpad notes
  with a mechanical parity-contract test, rather than relying on the narrative convention alone.
