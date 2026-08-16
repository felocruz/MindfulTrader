# Spec: The Predator Decision Contract — C++ Execution/Risk Architecture Framework

**Status**: SPEC — approved via `superpowers:brainstorming` (2026-08-16, MindfulTrader-rooted session).
**Scope note**: Pure C++ execution/risk system + its Python twin. **No schema changes in this spec.**
lbrnet's `docs/architecture/PHASE_3_MULTISCALE_PREDATOR_BLUEPRINT.md` (the dual-attention Transformer,
`is_bar_close`/`bar_type` schema fields, `MultiscaleInferenceBuffer`) is a separate, future,
lbrnet-rooted initiative — this spec is the C++ execution/risk *analog* of that blueprint's spirit,
not an implementation of it or a prerequisite for it.
**Governing methodology**: `docs/superpowers/specs/2026-08-16-execution-risk-coevolution-governance-spec.md`
— every decision built under this contract is validated through that spec's twin-first promotion
ladder, not a new validation process.
**Reframes, does not duplicate**: `docs/superpowers/specs/2026-08-16-elder-raschke-triple-barrier-convergence-backlog.md`
Units 4 and 5, and the in-progress Turtle Soup sniper-ization brainstorm — see First-Wave Work below.

## Purpose

This session's literature-grounded audit of the 16D risk-gate system, Triple Screen, and
Chandelier→Triple-Barrier migration surfaced a recurring, previously-unnamed pattern: execution/risk
timing in this system is not uniformly "wait for a completed bar" (historian) nor uniformly "react to
every tick" (sniper) — it's a real, verified mix, and critically, **being tick-reactive is not the
same as being correctly designed**. This spec names the missing discipline and formalizes it as **the
Predator Decision Contract**, consciously modeled on lbrnet's own `PHASE_3_MULTISCALE_PREDATOR_BLUEPRINT.md`
— Gemini's already-designed Python-side architecture that fuses a slow macro-attention branch (50
sparse bar-close frames) with a fast micro-attention branch (150 sub-second order-flow updates) via
asymmetric cross-attention, so the fast signal is always interpreted in the context of the slow one,
never blind to it. That document is a neural-architecture blueprint for the eventual Python decision
model. This spec is the C++ execution/risk layer's counterpart: the same fusion principle, applied as
a decision discipline to `PositionManager`/`RiskManager`/`TripleScreen*` today, so that when the full
Python Phase 3 model eventually exists, the C++ execution layer feeding it is already structurally and
philosophically aligned — not retrofitted after the fact.

## Background: What This Session Established (verified directly, do not re-derive)

- **HMM regime layer — genuinely tick/Mahalanobis-native.** `ContextManager::CheckAndTriggerHMM()`
  runs every tick, gated by a real Mahalanobis-distance trigger (not a bar pattern), and already does
  live mid-bar re-inference today — the most mature piece of this system by this framework's own
  standard.
- **Kangaroo Tail, Momentum Pinball, Elder Breakout (TS3) — tick-reactive, verified directly.**
  `TripleScreen3.cpp:837-847` (Kangaroo Tail), `:918-924` (Momentum Pinball), `:1063-1069` (Elder
  Breakout) all read `sc.Index` (the current, still-forming bar) with no bar-close gate.
- **Turtle Soup (TS3) — the one deliberately historian-gated pattern.** `TripleScreen3.cpp:1215-1241`,
  explicit `lastProcessedBarTS != signalBarIndex` guard, comment: "Institutional timing contract:
  Process ONCE per closed bar." Its definition (a failed breakout of a completed bar's range against a
  prior 20-bar window) genuinely requires knowing a completed close — this is a real candidate for
  ECTS-style ("anticipatory historian") early detection, not a naive tick-index swap. Scoped as its own
  unit, referenced but not designed here.
- **TRAP's native reactive floor — deliberately completed-bar, and correctly so** (parity + whipsaw
  avoidance, per `docs/ADR/triple_barrier_favorable_exit_ruling.md` Q2/Q3 and
  `triple_barrier_gates_parity_adjudication.md` §3). Not a candidate for change under this contract.
- **TRAP's anticipatory τ* layer is the best existing example of a Predator-grade decision** — a
  tick-reactive cost-sensitive threshold (Elkan 2001, τ* = C_FP/(C_FP+C_FN)) whose calibration point is
  conditioned on regime state, per `CLAUDE.md`'s Trap Detection section. Not yet wired live — gated on
  an F₀.₂₅ measurability metric that backlog Unit 3 (currently deferred) would unblock.
- **Tick-reactive ≠ Predator-grade — confirmed by a real, found violation.** `TripleScreen3.cpp:1162-1173`:
  Elder Breakout's "Screen 1 alignment" bonus sets `screen1Bullish` and `screen1Bearish` to the
  **identical condition** (`GAUSSIAN_STABLE || PARETO_MOMENTUM`), with the comment "HMM states are
  non-directional; momentum regimes support breakouts in either direction." This pattern is fast (reads
  the live bar every tick) but its macro-fusion is broken by construction — it cannot actually
  discriminate direction using regime state, despite being wired as if it does. This is the concrete
  proof that "already tick-reactive" is not sufficient; fusion has to be verified, not assumed.
- **lbrnet groundwork already anticipates a C++ counterpart.** `lbrnet/lbrnet/data/multiscale_bars.py`
  (real, built code — Ripple/Wave/Tide bar-cache) is documented as "a Phase 2 subset of the eventual
  Phase 3 'Predator' multiscale data module," with `bar_type` IDs 1/2/3 already reserved "so a future
  C++/wire version reuses the same numbering." This spec does not act on that reservation (no schema
  changes), but it confirms the C++ side was always expected to eventually need its own Predator work.

## The Predator Decision Contract

Five required elements for any execution/risk decision — entry trigger, exit trigger, stop/target
placement, position sizing — to be considered **Predator-grade**:

1. **Explicit macro/patient input** — regime state (HMM), structural level (`StructureTest`/swing
   high-low), persistence (Hurst/DFA), or other slow-moving context. Must be a named, real input to
   the decision — not assumed, not implicit.
2. **Explicit micro/fast input** — the tick-reactive signal itself: a pattern quality score, an
   order-flow/microstructure read, a threshold crossing.
3. **Explicit fusion rule** — the fast signal's decision threshold or action must be *conditioned on*
   the macro input, not evaluated in isolation and not deferred entirely until macro confirmation. The
   existing template to replicate: TRAP's τ* — tick-reactive, but its cost-sensitive threshold moves
   with regime state, per Elkan (2001).
4. **Twin-validation requirement** — no Predator-grade change is promoted to the SC-replay backtester
   without the Python twin first confirming its specific gating metrics, per
   `2026-08-16-execution-risk-coevolution-governance-spec.md`'s promotion ladder. This contract does
   not introduce a new validation process; it inherits that one.
5. **Subordinate to safety, in both position states — non-negotiable, no exception.** Predator fusion
   is additive and discretionary, never authoritative over safety. This is not a new rule invented for
   this spec — it generalizes `CLAUDE.md`'s own existing TRAP philosophy ("Native is always-on and
   authoritative; the model exit is additive... may LEAD but never SUPPRESS the floor") from "applies
   only to TRAP" to "applies to every Predator-grade decision, present and future":
   - **In a position**: the deterministic, non-fusion safety tier — TRAP's native reactive floor,
     then catastrophic regime-defense (`EvaluateRegimeDefense`) — always evaluates first and can
     always fire, completely independent of what any fusion layer concludes. Verified in the real call
     order, `PositionManager.cpp:241-268`: `EvaluateNativeTrapFloor()` unconditional, then each
     subsequent check gated only by `!m_exitSubmittedThisTick` (the first check to fire wins, no
     double-exit). A Predator-grade decision may *lead* — fire earlier than the safety tier would have
     — by independently setting the same exit flag, but must never be implemented as a condition that
     the safety tier's own checks are wrapped in (never `if (!fusionSaysHold) { EvaluateRegimeDefense(...) }`
     — that would let fusion suppress safety, which is exactly what this rule forbids).
   - **Flat (no position)**: hard risk gates (`RiskManager::EvaluateHardGates()`,
     `ExecutionGate::EvaluateEmpiricalRegimeGates()`) are a strict precondition. If they veto — e.g.
     high Shannon entropy, one of TS3's existing hard-gate thresholds — no Predator-grade fusion
     decision is allowed to produce an entry, regardless of what the fusion logic would otherwise
     conclude. Fusion does not get evaluated, or its output is unconditionally discarded, until the
     gates have already cleared the environment as safe enough to consider a trade at all.

**A decision that satisfies #1 and #2 but fails #3 is not Predator-grade — it is merely fast.** Elder
Breakout, above, is the concrete proof this distinction is real, not academic. **A decision that
violates #5 is not Predator-grade regardless of how well #1-#4 are satisfied — it is a safety
regression**, since it would let a fusion opinion override the mechanisms that exist specifically to
act when fusion itself may be wrong, stale, or unavailable.

## Predator Maturity Inventory (current state, established this session)

| Component | Macro input? | Micro input? | Fusion rule? | Verdict |
|---|---|---|---|---|
| HMM regime layer | is the macro layer | Mahalanobis-tick-triggered | tick-reactive within its own scope | **Predator-mature** |
| TRAP anticipatory τ* | Regime state (DofStopScale/Elkan costs) | Per-tick threshold recompute | Yes — cost-sensitive threshold conditioned on regime | **Predator-grade design, not yet wired live** (Unit 3-gated) |
| Elder Breakout | Present but non-functional (identical for both directions) | Yes, tick-reactive | **Confirmed broken** — see finding above | **Contract violation — first-wave fix** |
| Kangaroo Tail | `atSupportLevel`/`atResistanceLevel` (structural) | Yes, tick-reactive | **Audited 2026-08-16 — correctly direction-discriminating** (bullish requires at-support, bearish requires at-resistance, `TripleScreen3.cpp:849-877`) | **Predator-grade, no fix needed** |
| Momentum Pinball | Hurst exponent (persistence) | Yes, tick-reactive | **Audited 2026-08-16 — correctly direction-discriminating** (`slopeAligned` checks slope sign against the *specific* signal direction, continuous regime-conditioned multiplier, `TripleScreen3.cpp:935-965`) | **Predator-grade, no fix needed** |
| Turtle Soup | Yes (20-bar reference window) | No — bar-close gated | N/A, no fast signal yet | **Historian — sniper-ization candidate** (own unit) |
| `REGIME_INVALIDATION` | `StructureTest` `DECISIVE_*` (macro) | Needs tick-evaluation wiring | Not yet built | **First-wave candidate = backlog Unit 4** |
| Profit-protection/Trend-Scanning | HMM regime-gated (per ADR) | MFE/slope significance | Designed, not built | **First-wave candidate = backlog Unit 5** |

## First-Wave Concrete Work

1. **Elder Breakout directional-fusion fix** (new finding from this framework, not previously tracked)
   — replace the non-directional Screen-1-alignment bonus with a real regime-conditioned fusion rule
   that actually discriminates bullish from bearish regime support, satisfying contract element #3.
2. **Kangaroo Tail / Momentum Pinball Predator-contract audit — DONE 2026-08-16, both pass.** Neither
   needs a fix; see the Maturity Inventory above for the specific evidence. This audit itself is the
   proof the contract is a real filter, not just a way to flag Elder Breakout after the fact — passing
   two out of three checked patterns confirms "tick-reactive" and "Predator-grade" aren't automatically
   the same thing in *either* direction (can fail, as Elder Breakout did; can also genuinely pass).
3. **Turtle Soup sniper-ization** — referenced, not designed here; its own brainstorming thread is
   in progress (cheap intra-bar heuristic vs. first real application of ECTS-style prefix training is
   an open decision in that thread, not this spec).
4. **`REGIME_INVALIDATION` wiring** (backlog Unit 4) — now explicitly framed as building this
   contract's fusion rule for the exit side; no change to that unit's existing scope.
5. **Profit-protection/Trend-Scanning** (backlog Unit 5) — same reframing for the exit-side
   alpha-capture question; no change to that unit's existing scope, including its twin-measurement-first
   sequencing.

## Non-Goals

- **No schema changes** (`mts_schema.fbs`) — the Predator blueprint's wire/model-layer extensions
  (`is_bar_close`, `bar_type`, sparse Elder-indicator fields) remain a separate, future, lbrnet-rooted
  initiative. This spec does not implement or depend on them.
- **No neural model / Python Phase 3 implementation** — this spec is the C++ execution/risk analog
  only, not a data-pipeline or modeling project.
- **No always-on micro-hot-path tick publish.** Explicitly deferred until the Python Phase 3 work is
  actually underway and states what it needs — building this speculatively now carries real live
  bandwidth/perf risk (per `AutoLoop=1`'s every-tick-everywhere cost) against an unbuilt consumer.
- **Not a new backlog.** Existing Units 4 and 5, and the separate Turtle Soup brainstorm, are reframed
  under this contract's vocabulary and validation discipline — they are not duplicated or re-scoped
  here.

## Investigation Log

- **2026-08-16**: Synthesized at the close of an extensive `superpowers:brainstorming` session that
  (1) audited the 16D risk-gate system, Triple Screen, and Chandelier→Triple-Barrier migration against
  the actual literature, (2) mapped the Python twin's architecture/parity, (3) traced a live "historian
  vs. sniper" timing question through direct code verification across TS3's primary trigger patterns
  (correcting two of the model's own earlier over-generalizations along the way — first that the whole
  `PRIMARY_TRIGGER_MASK` was bar-close-gated, then a premature claim that Turtle Soup's own gating
  comments might be stale, both resolved by reading the actual call sites directly), (4) discovered
  lbrnet's own `PHASE_3_MULTISCALE_PREDATOR_BLUEPRINT.md` as the Python-side formalization of the same
  fusion principle, with real precursor groundwork (`multiscale_bars.py`) already anticipating a C++
  counterpart, and (5) converged on this spec as the named, C++-side decision framework — deliberately
  scoped to exclude schema work and the neural model, per the user's explicit direction to keep this
  buildable now rather than speculative.
