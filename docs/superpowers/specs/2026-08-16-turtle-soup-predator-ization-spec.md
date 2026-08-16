# Spec: Turtle Soup Predator-ization — Bridge Plan (Option A Now, ECTS Option B Later)

**Status**: SPEC — approved via `superpowers:brainstorming` (2026-08-16, MindfulTrader-rooted session),
literature-reviewed via `lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_103`/response (2026-08-16).
**Implements**: `docs/superpowers/specs/2026-08-16-predator-decision-contract-execution-risk-framework.md`
— the last item of the current first-wave batch (Elder Breakout fix and the Kangaroo Tail/Momentum
Pinball audit are both done).
**Governing methodology**: `docs/superpowers/specs/2026-08-16-execution-risk-coevolution-governance-spec.md`
for twin-first validation; `docs/superpowers/specs/2026-08-16-predator-context-fusion-infrastructure-spec.md`
for the shared `PredatorContext`/free-function dispatch conventions this spec's micro-signal function
plugs into; `docs/superpowers/specs/2026-08-16-ects-prefix-training-infrastructure-spec.md` for
Option B's general dataset-construction/stopping-rule/twin-extension mechanics — **this spec is that
infrastructure's first consumer, not a duplicate of it.**

## Purpose

Turtle Soup is the one TS3 primary trigger pattern that's deliberately historian-gated — it evaluates
once per closed bar (`TripleScreen3.cpp:1215-1241`, "Institutional timing contract") because its
definition (a failed breakout of a completed bar's range against a prior 20-bar extreme) genuinely
requires knowing the bar's final close. This spec brings it to Predator-grade without guessing at the
mechanism blind — the design was pressure-tested against the literature via a second-opinion channel
before being written down.

## Background: The Bridge Plan

Two candidate micro-signal designs were surfaced: a cheap intra-bar geometric heuristic (reusing
Kangaroo Tail's proven tail-to-body-ratio/close-position approach, applied against the 20-bar extreme
instead of a single bar), or a genuine ECTS (Early Classification of Time Series) model trained on
partial-bar prefixes. The result, after a literature pressure-test (`lbrnet/logs/rc_gemini.log`
`CLAUDE_BRIEF_103`) confirms the **bridge plan**: ship the heuristic now (this DLL has never reached
production — that's a standing priority), build the ECTS model as a parallel, non-blocking track using
the general infrastructure in `2026-08-16-ects-prefix-training-infrastructure-spec.md`, and swap it in
later at a single, well-defined seam — **if it actually wins its own empirical comparison against the
heuristic**, which is a genuinely open question for this specific pattern (small-sample, single-pattern,
single-instrument — see that spec's honest discussion of why the heuristic winning is a real
possibility, not a hedge).

**The bridge-plan's seam, specifically**: Option A and Option B are interchangeable at a single point
— how the micro-signal gets computed — provided that seam is a standardized, bounded output (score,
confidence, earliness, validity) that the macro-fusion layer consumes without caring how it was
produced. This matches, and plugs directly into, the free-function convention already established in
`PredatorContext`/`PredatorFusion` (a plain result struct returned by a free function, consistent with
this codebase's DOD/no-virtual-dispatch discipline).

## Design

### Micro-Signal Result (shared by both options)

```cpp
struct TurtleSoupMicroSignal {
    float score;       // [-1.0, +1.0], directional strength (negative = bearish setup forming)
    float confidence;  // [0.0, 1.0]
    float earliness;   // elapsed-bar-fraction at evaluation time, [0.0, 1.0]
    bool  isValid;      // false until minimum tick density / warmup satisfied
};
```

Plain struct, free-function producers — no virtual dispatch, consistent with `PredatorFusion.h`'s
existing convention.

### Option A: Intra-Bar Geometric Heuristic (ship now)

```cpp
TurtleSoupMicroSignal EvaluateTurtleSoupOptionA(
    const BarPartial& currentBarPrefix,   // current-bar high/low/close-so-far, elapsed fraction
    const ReferenceLevels& twentyBarExtremes
);
```

Computes penetration-so-far and close-position-so-far against the 20-bar extreme, using the same
tail-to-body-ratio/rejection-shape approach already proven and audited for Kangaroo Tail
(`TripleScreen3.cpp:849-877`) — evaluated on the live, currently-forming bar (`sc.Index`, not
`sc.Index-1`), replacing the current once-per-closed-bar gate. Fusion rule and safety subordination
are unchanged from the existing entry-pattern pipeline (direction-discriminating macro-conditioning
matching Kangaroo Tail/Momentum Pinball's audited pattern; hard risk gates apply before any entry,
per contract element 5).

### Option B: ECTS Classifier (parallel track, does not block Option A's ship)

Built using the general capability specced in `2026-08-16-ects-prefix-training-infrastructure-spec.md`
— this section covers only what's Turtle-Soup-*specific*; the dataset-construction methodology,
stopping-rule theory, and Python-twin Predator extension are that spec's concern, not duplicated here.

**Turtle-Soup-specific application, staged and cost-ordered:**

1. Extract Turtle-Soup-specific features per prefix snapshot (penetration-so-far vs. the 20-bar
   extreme, tail-shape-so-far, ATR-normalized distance, elapsed-bar-fraction) using the
   infrastructure's general dataset-construction methodology.
2. Prototype with `scikit-learn` (logistic regression first) and compare against Option A **inside
   the Predator-equipped twin** — an empirical, apples-to-apples comparison, not an assumption that
   the classifier wins.
3. **Only if the classifier demonstrably beats the heuristic** on real gating metrics: hand-port to
   C++ per the infrastructure spec's deployment guidance (hand-crafted, not auto-generated; golden-vector
   regression-tested against the Python model). `EvaluateTurtleSoupOptionB()` then replaces
   `EvaluateTurtleSoupOptionA()` at the single call site that currently invokes Option A — no change
   to the macro input, fusion rule, or safety subordination.
4. **If it doesn't win**: Option A remains the shipped answer, and the classifier's negative result is
   still real, useful information — a documented, honest outcome, not a failure requiring justification.

## Test Plan

1. Option A: native unit test suite (golden-vector), including a paired-opposite-direction test case
   (the shape established for `FuseTauStar()` in the infrastructure spec) — proves direction is
   genuinely discriminated, not assumed.
2. Option A: `build_dll.sh` clean; confirm the once-per-closed-bar gate (`lastProcessedBarTS`) is fully
   removed, not left as dead code alongside the new tick-reactive path.
3. Option B (separate track, own test plan when that work starts): offline dataset leakage checks
   (no forward-looking information in any prefix snapshot), model calibration checks, and twin-based
   gating-metric measurement before any promotion decision.

## Acceptance Criteria (Option A — this is what ships now)

1. Turtle Soup evaluates tick-reactively (current forming bar), matching Kangaroo Tail/Momentum
   Pinball's already-audited pattern — no `lastProcessedBarTS`-style once-per-bar gate remains.
2. Fusion rule is direction-discriminating and macro-conditioned (contract elements 1-3 satisfied).
3. `TurtleSoupMicroSignal` is a plain struct, no virtual dispatch introduced.
4. `build_dll.sh` and existing native test suite green.
5. Option B's design is recorded here but explicitly **not required** to ship Option A — this is a
   real bridge, not a placeholder blocked on the harder work.

## Non-Goals

- **No schema changes.**
- **No commitment that Option B beats Option A** — see the infrastructure spec and the Background
  section above; this is a genuinely open empirical question, not assumed either way.
- **No commitment to a ship date for Option B** — it is a parallel, non-blocking track; Option A alone
  satisfies this spec's acceptance criteria.
- **General ECTS mechanics (dataset construction, stopping-rule theory, deployment/library guidance)
  are the infrastructure spec's concern, not re-specified here** — see
  `2026-08-16-ects-prefix-training-infrastructure-spec.md`.

## Investigation Log

- **2026-08-16**: Design pressure-tested via `lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_103`/response
  before being written down, rather than decided from first principles alone. The literature review
  confirmed the classical-classifier approach is genuine ECTS (not a shortcut), surfaced two real
  dataset-construction guardrails (fractional sampling, profitability-inclusive labeling), and
  connected the stopping-rule question to this project's own existing Shiryaev-Wald/Elkan theoretical
  foundation rather than a new one. One implementation-detail mismatch (a proposed virtual-interface
  mechanism) was resolved in favor of this codebase's existing free-function/DOD convention.
- **2026-08-16 (later)**: Feasibility check (where does the classifier live, what libraries exist)
  done before committing to Option B's plan, not assumed — confirmed zero existing C++
  model-deployment precedent and a real, non-trivial cross-compilation cost to any new C++ ML runtime
  library.
- **2026-08-16 (later still)**: User correctly reframed Option B as a general capability question
  rather than Turtle-Soup-specific, surfacing a second real consumer (TRAP's own already-deferred ECTS
  need). Split the general mechanics out into
  `2026-08-16-ects-prefix-training-infrastructure-spec.md`, mirroring the
  `PredatorContext`/`PredatorFusion` infrastructure-vs-consumer pattern — this spec now covers only
  what's Turtle-Soup-specific.
