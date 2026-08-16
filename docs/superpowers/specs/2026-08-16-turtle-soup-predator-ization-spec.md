# Spec: Turtle Soup Predator-ization — Bridge Plan (Option A Now, ECTS Option B Later)

**Status**: SPEC — approved via `superpowers:brainstorming` (2026-08-16, MindfulTrader-rooted session),
literature-reviewed via `lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_103`/response (2026-08-16).
**Implements**: `docs/superpowers/specs/2026-08-16-predator-decision-contract-execution-risk-framework.md`
— the last item of the current first-wave batch (Elder Breakout fix and the Kangaroo Tail/Momentum
Pinball audit are both done).
**Governing methodology**: `docs/superpowers/specs/2026-08-16-execution-risk-coevolution-governance-spec.md`
for twin-first validation; `docs/superpowers/specs/2026-08-16-predator-context-fusion-infrastructure-spec.md`
for the shared `PredatorContext`/free-function dispatch conventions this spec's micro-signal function
plugs into.

## Purpose

Turtle Soup is the one TS3 primary trigger pattern that's deliberately historian-gated — it evaluates
once per closed bar (`TripleScreen3.cpp:1215-1241`, "Institutional timing contract") because its
definition (a failed breakout of a completed bar's range against a prior 20-bar extreme) genuinely
requires knowing the bar's final close. This spec brings it to Predator-grade without guessing at the
mechanism blind — the design was pressure-tested against the literature via a second-opinion channel
before being written down.

## Background: The Bridge Plan and Its Literature Grounding

Two candidate micro-signal designs were surfaced: a cheap intra-bar geometric heuristic (reusing
Kangaroo Tail's proven tail-to-body-ratio/close-position approach, applied against the 20-bar extreme
instead of a single bar), or a genuine ECTS (Early Classification of Time Series) model trained on
partial-bar prefixes. Rather than pick blind, this was put to `lbrnet/logs/rc_gemini.log`
`CLAUDE_BRIEF_103` for a literature pressure-test. The result confirms the **bridge plan**: ship the
heuristic now (this DLL has never reached production — that's a standing priority), build the ECTS
model as a parallel, non-blocking track, and swap it in later at a single, well-defined seam.

**Key findings from the literature review, credited to that exchange:**

- **The classical-classifier approach is a legitimate, fully-grounded ECTS instance, not a
  simplification of it.** Xing, Pei & Philip's (2009) foundational ECTS formulation has two
  requirements — prefix feature extraction and an earliness/accuracy tradeoff — neither of which
  mandates deep learning. A gradient-boosted-trees or logistic-regression classifier over engineered
  prefix features (elapsed-bar-fraction, penetration-so-far vs. the 20-bar extreme, tail-shape-so-far,
  ATR-normalized distance) satisfies the definition directly.
- **Two concrete dataset-construction guardrails, both adopted into this spec's Option B design**:
  (1) sample prefixes by time-elapsed or volume fraction, not raw tick count — tick count varies
  systematically with volatility, so fixed-tick sampling would bias the dataset toward whatever regime
  produces more ticks per unit time; (2) the label must require a minimum forward-return threshold in
  addition to geometric pattern completion — labeling on shape alone risks training a model that
  recognizes "looks like Turtle Soup" without recognizing "was actually profitable," a real and
  non-obvious failure mode distinct from ordinary overfitting.
- **The stopping-rule literature already in use elsewhere in this system generalizes cleanly here.**
  Dachraoui et al. (2015)'s cost-based sequential framework remains the right foundation; Elkan
  (2001)'s cost-sensitive threshold (already the basis of TRAP's τ*) extends naturally to
  regime-conditioning Turtle Soup's own early-decision threshold. The "penetrate-then-reject" shape
  is, formally, a drift-reversal detection problem — Moustakides (1986)/Shiryaev sequential
  change-point detection (the same theoretical family `CLAUDE.md` already cites for TRAP's Shiryaev-Wald
  framing) is the correct connecting citation, not a new, unrelated theory.
- **The bridge-plan's core premise holds**: Option A and Option B are interchangeable at a single seam
  — how the micro-signal gets computed — provided that seam is a standardized, bounded output
  (score, confidence, earliness, validity) that the macro-fusion layer consumes without caring how it
  was produced. This matches, and plugs directly into, the free-function convention already
  established in `PredatorContext`/`PredatorFusion` (a plain result struct returned by a free function,
  consistent with this codebase's DOD/no-virtual-dispatch discipline — the same interface *shape* the
  literature review proposed, expressed the way the rest of this codebase's hot-path logic already is).

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

1. **Offline dataset construction** from `mes_continuous_ticks.parquet` (already available, no new
   live collection needed): for each historical bar, reconstruct multiple prefix snapshots at
   elapsed-time/volume fractions τ ∈ {0.1, 0.2, ..., 0.9}, each labeled with the bar's real eventual
   outcome — WEAK/STRONG/EXTREME/NONE **and** whether the resulting trade would have cleared a minimum
   forward-return threshold (the profitability guardrail above).
2. **Model**: gradient-boosted trees (or logistic regression) over engineered features
   (elapsed-fraction, penetration-so-far/ATR, tail-shape-so-far, tick-level momentum) — not a deep
   sequence model. `scikit-learn` is already in `lbrnet`'s environment; no new dependency.
3. **Stopping rule**: Dachraoui-style cost-based earliness/accuracy tradeoff, with the decision
   threshold conditioned on regime state via the same Elkan (2001) cost-sensitive formula τ* already
   used for TRAP — reusing, not reinventing, this project's existing theoretical apparatus.
4. **Twin validation**: per the governance spec's promotion ladder — measure real gating metrics via
   the Python twin before this ever reaches the SC-replay backtester.
5. **The swap**: once validated, `EvaluateTurtleSoupOptionB()` replaces `EvaluateTurtleSoupOptionA()`
   at the single call site that currently invokes Option A — no change to the macro input, fusion
   rule, or safety subordination.

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

- **No deep sequence model** (LSTM/Transformer-based early-exit) for Option B — classical GBT/logistic
  regression is the literature-grounded, lower-risk choice per the review above.
- **No live retraining pipeline** — Option B's model training is an offline, batch process against
  already-collected tick data; nothing about live collection changes for this spec.
- **No schema changes.**
- **No commitment to a ship date for Option B** — it is a parallel, non-blocking track; Option A alone
  satisfies this spec's acceptance criteria.

## Investigation Log

- **2026-08-16**: Design pressure-tested via `lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_103`/response
  before being written down, rather than decided from first principles alone. The literature review
  confirmed the classical-classifier approach is genuine ECTS (not a shortcut), surfaced two real
  dataset-construction guardrails (fractional sampling, profitability-inclusive labeling) adopted
  directly into Option B's design, and connected the stopping-rule question to this project's own
  existing Shiryaev-Wald/Elkan theoretical foundation rather than a new one. One implementation-detail
  mismatch (a proposed virtual-interface mechanism) was resolved in favor of this codebase's existing
  free-function/DOD convention — the underlying architectural idea (a standardized, swappable
  micro-signal output) was sound either way; only the C++ mechanism needed adapting to a
  project-specific constraint that hadn't been stated in the original brief.
