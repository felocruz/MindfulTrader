# Predator / Sniper Execution Architecture — Consolidated

**Status: CONSOLIDATION, 2026-09-18.** This document merges 15 previously scattered
brainstorm/design specs (listed in §7) into one living architecture doc, organized around a single
pipeline. None of the underlying decisions changed by merging them — every status/finding below is
carried over verbatim in substance from its source spec, just organized by pipeline stage instead
of by creation date. The 15 source files were deleted (not archived) once their content was folded
in here; full historical detail (design discussion, literature citations, rejected alternatives)
remains in git history (`git log --follow -- <old path>`) for anyone who needs the original prose.
**Not touched or absorbed**: `docs/ADR/triple_barrier_*.md` (7 files) — those are formal,
Gemini-adjudicated rulings (the authoritative decision record for Triple-Barrier questions), cited
from here, not merged into here. Same for `docs/ADR/risk_gate_context_wire_spec.md`,
`amihud_gate_percentile_spec.md`, and other ADR-level rulings.

## 0. The pipeline

```
PREDATOR (regime survey — decides WHETHER)
   │  HMM 4-state regime + Trading Partner Classifier veto + structural pattern detection
   ▼
TRANSFORMER (decides WHICH direction)
   ▼
META-LABELER (decides HOW MUCH — position size)
   ▼
SNIPER (decides WHERE exactly — stop/target geometry, entry timing, activity-clock precision)
```

Each stage answers a genuinely different question and draws on a different part of the Gang
toolkit (Raschke/Wyckoff structural geometry, Mandelbrot/Hurst persistence, Shannon entropy,
Taleb/EVT tail risk, Pareto/GPD calibration). Conflating stages is the recurring failure mode this
doc's own source specs kept re-discovering independently — keeping them explicit is the reason to
consolidate at all, not just tidiness.

## 1. The Predator — regime survey (decides WHETHER)

**Built and live:**
- `PredatorContext`/`PredatorFusion.h` — unified macro context (`LocalRiskContext` + `HMMStateEnum`
  + `inPosition` + `applicabilityMask`), free-function fusion dispatch reusing `IndicatorManager`'s
  dirty-mask idiom. Position-state gating makes "never fire an entry-fusion while in a position"
  structurally true, not just tested-for.
- τ* anticipatory TRAP exit (`FuseTauStar`, Elkan 2001) — the first real fusion function, migrated
  onto this infrastructure as a regression-safe reference implementation.
- Turtle Soup Predator-ization, Option A (intra-bar geometric heuristic, reuses Kangaroo Tail's
  tail-to-body-ratio approach against the 20-bar extreme) — shipped, live call site
  `TripleScreen3.cpp:1267`.
- Native TRAP floor (`FAILED_*` structural reversal) — built, live, the parity anchor with the
  labeler.

**Spec'd, not started (C++ side):**
- Turtle Soup Option B (genuine ECTS model on partial-bar prefixes) — bridge-plan's later half;
  only proceeds if it empirically beats Option A on this specific pattern (small-sample, single-
  instrument — a real open question, not assumed to win).
- Trading Partner Classifier (the "soft/gate classifier") — general danger veto, deliberately
  independent of live HMM output by design (a second, orthogonal survey axis, not a duplicate of
  the HMM regime read). **Implemented and validated on the Python/training side already** — full
  content in §2a, not just a spec reference.
- `PredictionAgeUs()` continuous decay of the Transformer's staleness (mirrors `HmmStateAgeUs()`)
  — decay function form not yet chosen (candidates: exponential, linear ramp — needs empirical
  derivation, not an invented constant). **Consumer corrected 2026-08-24**: feeds the meta-labeler's
  live inference, not Predator Fusion — Predator Fusion is training-time-only, confirmed by
  operator, never runs live for any pattern.

**Founding gap, ANSWERED but not yet fixed**: 7 of 8 `RiskManager`/`ExecutionGate` hard gates are
blind to the HMM's own live regime output — only the Amihud illiquidity veto genuinely reads
`InferenceManager::Instance().HmmState()->Dof()`. Two gate names were misleadingly regime-sounding
(`paretoTopStateRatio`→renamed `hillTailIndexProxy`, `talebSignalSigma`→renamed
`talebKurtosisEntryGate`) but were never actually regime-aware. Recommendations exist (extend gate
1's `Dof()`-based pattern to gates 2-5), not implemented.

**Regime-gating Gang candidates, BLOCKED**: a Persistence-Entropy Divergence Index (`Δ_HS =
H̃(Hurst)·H̄(state entropy)`) and a Phase-State Machine (5-state, driven by entropy-rate/Hurst-rate
signs) were proposed as sharper regime reads. Both blocked on a real, confirmed API gap: per-state
HMM posterior probabilities are not exposed anywhere on the C++ side today (`HmmStateIndicator`
only carries the argmax state + 7 derived scalars, not the full K-length posterior vector).

**The Predator's sensory input** (the HMM's own `ObservationData`, the "eyes"): 5 dims already
compute natively on the activity clock (`fast_hurst_exponent`, `fast_taleb_kurtosis`,
`skewness_idx`, `recurrence_rate`, `fast_mean_rev_z`). Per-dim status: `fast_hurst_exponent`/
`fast_taleb_kurtosis` IN-UNMEASURED (shipped, cross-state discrimination never tested — still the
top-priority action across the whole vector); `skewness_idx` IN-CONTINGENT (real construction fix
landed, residual heavy tail remains); `recurrence_rate` IN (robust, orthogonal); `fast_mean_rev_z`
resolved OUT twice over — first on raw predictive power (statistically indistinguishable from a
coin flip, n=1.57M), then (2026-09-18) via a genuine HMM cross-state discrimination test lbrnet
ran: reintroducing it collapsed one state to 0.39% occupancy (Celeux & Durand pathology), confirming
the original call. Moved from IN to OUT in the offline replay tool's candidate set same day.

## 2. Transformer + Meta-labeler — direction and size

**Status: C++ deployment not started for either classifier** (`two-classifier-cpp-deployment-spec`).
The Python/training side is materially ahead of that — two genuinely distinct classifiers exist on
the `lbrnet` side, at different maturity, both full content absorbed below (not just referenced).

### 2a. Trading Partner Classifier (the "soft/gate classifier") — implemented, validated

**What it is**: a learned, graded, *bidirectional* analog of `RiskManager::EvaluateHardGates()`.
"Gate" was the original name and was deliberately dropped — a gate only has veto vocabulary (fire
or stay silent); once the classifier is side-aware it can say more: not just "don't enter/exit
here" but "this specific side, in this specific context, is favored." It is a second opinion *on a
decision a pattern already initiated*, never an independent trade-originator.

**Two-tier sequencing, reconciled with the Predator Decision Contract's element #5** ("subordinate
to safety, in both position states — non-negotiable"): hard gates and this classifier are not
alternatives, they are two tiers over two different subsets of `gang` (`LocalRiskContext`'s 15
substantive fields), evaluated in strict order:
- **Tier 1 — hard gates** (`EvaluateHardGates()`, `RiskManager.cpp:834-897`): reads only 6 of the
  15 fields (`amihud_illiquidity`, `amihud_percentile`, `elder_chandelier_atr`,
  `shannon_flow_entropy`, `taleb_kurtosis`, `spread_stress`). Absolute, boolean, side-blind,
  evaluated first, non-overridable, unchanged by any of this.
- **Tier 2 — this classifier**, evaluated only once Tier 1 finds no imminent danger. Trained on
  *all 15* fields — the 6 Tier 1 reads, plus the 9 it ignores entirely (`taleb_skewness`,
  `pareto_tail_alpha`, `hurst_exponent`, `fractal_dim`, `mean_rev_z`, `raschke_burst`,
  `fisher_info`, `regime_duration`) — plus `side`, which Tier 1 structurally lacks. "Softer" does
  NOT mean weaker: Tier 2 retains full veto power over its own richer feature set, second in
  sequence, not subordinate in authority within that sequence.

**Why `side` matters and doesn't reintroduce pattern-specificity**: `taleb_kurtosis` (which Tier 1
reads) measures fat-tail *magnitude* only, sign-blind — it can't distinguish a crash-skewed tail
(dangerous for longs) from a squeeze-skewed one (dangerous for shorts). `taleb_skewness` (already
in `gang`, one of the 9 fields Tier 1 ignores) carries exactly that directional information. `side`
is a position-state input always available without pattern-specific geometry — evaluating both
`predict_proba(gang, LONG)` and `predict_proba(gang, SHORT)` symmetrically requires no pattern to
have fired yet, so this classifier sits at the same pipeline point `EvaluateHardGates()` occupies
today (before any pattern/tactical-trigger runs), not only reactively after one proposes a
candidate.

**Feature set (final)**: `[shannon_flow_entropy, shannon_efficiency, taleb_kurtosis,
taleb_skewness, elder_chandelier_atr, pareto_tail_alpha, amihud_illiquidity, amihud_percentile,
spread_stress, hurst_exponent, fractal_dim, mean_rev_z, raschke_burst, fisher_info,
regime_duration, side]` — 15 `gang` fields + `side` (`+1`/`-1`/`0`). `is_valid` gates whether it
runs at all (mirrors `EvaluateHardGates()`'s own precondition); `snapshot_timestamp_us` excluded
(bookkeeping, not signal).

**Three use cases** (naming them explicitly matters — collapsing back to "just a gate" is exactly
the framing this design moved past):
1. **Entry/exit veto** (the original framing) — `abs(bet_size) > τ` decides act/veto, same as
   `EvaluateHardGates()` already does, just graded and side-aware. Runs bidirectionally against
   the current `gang` reading alone, no pattern proposal required.
2. **Symmetric risk-awareness for an already-open position** — the concrete failure mode fixed:
   short position, a black-swan move forming skewed toward a crash. A side-blind hard gate sees
   only kurtosis magnitude and flattens regardless of direction, closing a *profitable* short right
   as the crash begins. Trained on `(gang, side, outcome)` triples, the classifier can learn that
   `(elevated kurtosis, strongly negative skew, side=short)` is favorable, not a veto trigger.
3. **Position-management conviction** — already in a position, `bet_size` for that side is large
   and positive: "this position is well-supported, hold it," or at entry, "the pattern fired *and*
   the partner strongly agrees — lean into a bigger size." Amplifies conviction on a trade a
   pattern already originated; does not originate trades from `gang` alone (explicit non-goal).

**Components (real, on the Python side)**: `predator_fusion.py`'s `compute_bet_size(probability,
side)` (López de Prado's z-score/CDF conversion, pattern-agnostic); `lbrnet/data/
trading_partner_classifier.py` (dataset labeling from any pattern-outcome dataset with
`timestamp_us`/`direction`/`profitable` — Turtle Soup's today — training-frame prep, logistic
regression + GBT training, threshold calibration by sweeping τ on a held-out split to maximize
aggregate expected value, then freezing it); `backtest/trading_partner_twin.py` (evaluates the
trained classifier against real historical outcomes AND against a replay of `EvaluateHardGates()`'s
own boolean logic on the same data — scored via Omega_net/AUPR, never raw accuracy, because class
imbalance in profitable/not outcomes makes accuracy misleading here).

**Real work completed**: logistic regression + GBT training, a Python replay of
`EvaluateHardGates()`, golden-vector parity against `predict_proba()`, EV-swept threshold
calibration. **Deliverable path for the eventual C++ hand-port (not yet done)**: mandatory
golden-vector regression tests proving Python's `predict_proba()`/`compute_bet_size()` matches the
hand-ported C++ formula bit-for-bit before any live wiring — same discipline as every other
cross-language model port in this project, no exception for a model this small.

**Non-goals** (explicit): no entry origination from `gang` alone (Use Case 3 amplifies size on an
already-pattern-originated trade, never originates one itself); no pattern-specific classifier
(deferred, tracked in `lbrnet`'s `PREDATOR_CROSS_PATTERN_ALPHA_RANKER_ASPIRATION.md`); no
cross-pattern ranking; no live C++ wiring yet; no change to `EvaluateHardGates()` itself; classical
models only (logistic regression + GBT), no deep learning.

### 2b. Tier 2 Physics/Regime Signal — design only, does not supersede 2a

A separate, newer, lower-maturity artifact — a genuinely signed (not side-blind) physics/regime
signal sitting between Tier 1 (hard gates) and Tier 3 (pattern checks): `Tier 1 → Tier 2 (this) →
Tier 3 (patterns)`.

**Why one signed model, not a decomposed hazard/direction pipeline**: literature argues against
treating physics-derived risk measures as inherently side-blind. Chen, Hong & Stein (2001): 
conditional skewness genuinely forecasts crashes — a directional signal, not a side-blind hazard
reading. French, Schwert & Stambaugh (1987) and the leverage-effect/asymmetric-volatility
literature: negative returns raise future volatility more than positive returns of equal
magnitude. Barndorff-Nielsen, Kinnebrock & Shephard (2010): realized semivariance decomposes
volatility itself into directional components. `taleb_skewness` (and any sign-carrying field)
stays in one model, not stripped into a side-blind stage.

**Why the label is a short nowcast, not a multi-hour forecast**: reusing the main transformer's
own triple-barrier labeling construct would make this a smaller, worse-featured duplicate of the
transformer's job. Architecturally closer to the hard gates and the HMM — both are nowcasts ("what
is the state right now"), not forecasts. Timescale grounding: the 2010 Flash Crash's acute phase
was ~13 minutes (4% drop in 4 minutes); Kirilenko et al. (2017)'s forensic reconstruction was at
~1-second resolution — pointing to a confirmation window of seconds to a few minutes, not the
transformer's multi-hour horizon.

**Why persistence is asymmetric and time-weighted (fast attack, slow decay)**: grounded at three
levels — EGARCH (Nelson 1991, `σ²_t` depends recursively on `σ²_{t-1}` plus an asymmetric shock
term); dual-lambda exponential smoothing (a real futures margin-modeling patent, US11393029: "high
reactivity to crisis and slow reactivity to periods of calm"); LSTM forget/input gates (the
fully-learned general version of the same shape — right concept, not the right implementation
given this project's classical-models-only constraint). `.context` anchors arrive irregularly
(dense during bursts, sparse during calm), so the decay recursion is time-weighted using the same
continuous-time EWMA construction standard for irregularly-arriving tick data:
```
Y_i = Y_i_raw                                     if sign(Y_i_raw) != sign(Y_{i-1})
Y_i = max(Y_i_raw, decay^(Δt_i / τ) * Y_{i-1})    if sign(Y_i_raw) == sign(Y_{i-1})
```

**Feature set (X)**: all 15 `gang` fields (including `taleb_skewness`, not stripped out); HMM
regime posteriors (`p_coiled`, `p_gaussian_stable`, `p_gaussian_fragile`, `p_pareto`,
`mahal_distance`); short backward-window trajectory features on hazard-carrying fields (exact
window length undecided). No externally-supplied `side` — direction is part of the model's own
output.

**Label (Y) construction, training only**: symmetric barriers (`upper = close_t + m·σ_t`, `lower =
close_t − m·σ_t`, `σ_t` = DOF-scaled ATR10) anchored at each `.context` row, resolved via
first-hit-wins walk-forward through real 1-second OHLC bars — capped at the last 1-second bar
before the *next* `.context` row's own bar (a label must not outlive the regime it describes; if
neither barrier touches by then, drop the row, matching this project's inconclusive-exclusion
convention). Resolved raw labels then get the time-weighted dual-lambda persistence recursion
above applied across the sequence to produce the final training target.

**Output**: a single number in `(-1, 1)` — sign = which side the physics currently favors,
magnitude = conviction (a position-sizing multiplier at entry, or a scale-out fraction on an open
position). Consulted the same way as 2a's Use Case 1/exit role, just producing its own sign rather
than grading an externally-supplied one.

**Relationship to 2a** — both exist, neither replaces the other:

| | Trading Partner Classifier (2a) | Tier 2 Physics Signal (2b) |
|---|---|---|
| Side | Supplied externally (pattern's proposal / open position) | Part of the model's own output |
| Label | Real triple-barrier `profitable` outcome, multi-hour horizon | Symmetric barrier race on 1-second bars, capped at next `.context` row, time-weighted persistence |
| Status | Implemented, validated against real Turtle Soup data | Design only |
| Training data | Turtle Soup's dataset today, extensible | Dense pattern-free sampling (every `.context` row or a sampled subset) |

Whether 2b eventually supersedes, complements, or feeds into 2a is a later decision, not yet made.

**Non-goals**: same as 2a (no entry origination from `gang`/regime/trajectory alone; no
replacement of Tier 1; no live C++ wiring; no deep learning).

**Open questions, genuinely unresolved (flagged, not guessed, per this project's own standing rule
against inventing placeholder constants)**:
1. Symmetric bracket multiple `m` (barrier width in units of `σ_t`).
2. Max forward window `M_bars_max` (1-second bars) — granularity decided, magnitude not.
3. Backward trajectory window length and which fields receive trajectory features.
4. Decay time constant `τ` — fixed or regime-conditioned (matching `max_bars_for_regime`'s own
   precedent)?
5. Training data granularity — every `.context` row, or a sampled subset.
6. Timestamp skew between `.context`'s wall-clock processing time and the trade-timestamped tick
   data — needs empirical measurement before treating the anchor as exact.
7. `σ_t`'s actual data source — `atr_10`/`dof` aren't native `.context.parquet` columns; whether to
   use `elder_chandelier_atr` directly, join to `.alpha` for the literal `atr_10`, and/or join to
   HMM posteriors for DOF-scaling is deferred.

### 2c. Meta-labeler — position size

Decides position size once the Transformer has produced a side call (AFML Ch.10). Consumes: the
Transformer's time-decayed side/confidence (`PredictionAgeUs()`), the Trading Partner Classifier's
score, HMM posterior-derived scalars, and the existing hand-crafted sizing multipliers already
live in `RiskManager`/`Indicator.h`. **Confirmed 2026-08-24**: Predator Fusion output is NOT one of
the meta-labeler's live inputs — Predator Fusion only scopes/labels the meta-labeler's *training*
set in Python.

**Existing deployment precedent** (reusable template for all three, once C++ deployment starts):
`EvaluateTurtleSoupOptionB()`'s `ClassifierParams` pattern — config-driven weights/bias loaded from
`config/classifier_params.json` at runtime, not hand-transcribed constants, not a linked ML runtime.
Option B itself was never wired live and never will be (training-time-only ruling supersedes it) —
only the *deployment mechanism* survives as the template.

**Validation mechanism, not yet built**: a `BackTesterStudy`-hosted dual-path comparison — log both
the native C++ path's output and what the real-time backtester (a genuine Python model over ZMQ,
fed replayed data) returns for the same event, diff across a full replay. Prerequisite: confirm the
real-time backtester serves anything besides Transformer predictions today (probably doesn't yet).

## 3. The Sniper — placement and timing (decides WHERE) — the genuinely missing piece

This is the layer none of the 15 source specs actually built. Everything below is either an
existing, reusable primitive or a named, still-open validation gap — not a green-field invention.

**Existing baseline (live today)**: Triple-Barrier (AFML Ch.3) — stop/target/vertical-time-limit,
first-touch-wins race, seeded from Wilder ATR on 15-minute calendar bars.

**Ruled, not implemented**: pattern-class-differentiated exit objective — fades (Turtle Soup,
Momentum Pinball) get fixed-target first-touch (mean-reverting; trend-scanning them is a category
error); breakouts (Elder) get trend-scanning (AFML MLAM Ch.5, fitting rolling forward regressions,
exiting when the slope's t-statistic loses significance). **Phase 1 ships uniform first-touch for
all patterns as the control group** — the differentiation is a data-gated Phase 3/4 upgrade, not
yet built.

**Decisive design, NOT implemented, core claim UNRESOLVED**: activity-clock Bipower Variation
(Barndorff-Nielsen & Shephard 2004/2006) replacing Wilder ATR as the barrier-width scale reference,
computed on `ImbalanceBarEngine`'s imbalance clock instead of calendar time. Real validation run
2026-09-18 (471.9M real ticks): BV's magnitude runs ~42-55% of ATR's (expected, different
construction, not a defect — multiplier constants need re-derivation either way). **The actual
motivating claim — that BV is natively jump-robust AND more reactive than ATR — failed to confirm
on a systematic 30-independent-event test**: the event-definition method (calendar-bar True Range)
turned out to structurally bias the comparison toward Wilder's own clock, and the reaction-fraction
metric itself was numerically unstable. Real next step, not yet built: redefine jump events via a
clock-agnostic method (raw price displacement over a fixed wall-clock window) before trusting
either direction.

**EVT/GPD live stop-distance calibration — candidate, never designed until this consolidation.**
This repo already has GPD-fitting machinery (used today only for static `FeatureScaler` calibration
bounds). The same return-level calculation answers a different question for the Sniper: given a
structural level (swing high/low, Turtle Soup's false-breakout extreme), how far beyond it is
genuine invalidation vs. normal noise? Fit a GPD to the tail of "excursions beyond the structural
level that get reclaimed" and place the stop at the p95/p99 return level, scaled by whatever
volatility unit the item above resolves to.

**HMM-regime-gated target selection — candidate, never designed.** The existing fixed pattern-class
rule (fades=fixed, breakouts=trend-scan) could be replaced by a live Hurst/HMM-state read instead
of a hardcoded per-pattern rule — directly extends the already-adjudicated ruling rather than
replacing it. Real open question: does live-regime gating actually improve on the static rule, or
just add complexity? Needs the same empirical bar as the BV work, not assumed.

**Shannon entropy as a time-barrier modulator — new idea, zero grounding done.** High entropy
(regime classification itself uncertain) → shorter time barrier, cut the trade loose faster, less
statistical support the longer it goes unconfirmed. Low entropy (confident, persistent state) →
longer barrier. **No literature citation identified for this specific application** — flag as
reasoned synthesis only until checked, same honesty discipline as every Gang-reformulation
candidate in this doc.

**"Speed" (activity-clock bar-completion rate) as an entry/stop urgency signal — grounded, untested.**
Real citation: Engle & Russell (1998), "Autoregressive Conditional Duration," *Econometrica* —
inter-event duration is a well-established information-bearing quantity in market microstructure,
not an invented idea. **Real, unresolved risk**: this morning's `Y_imb` investigation found
sub-second imbalance bars are very likely burst-splitting artifacts, not real information — "speed"
needs the identical duration-binning/skip-1-bar artifact-control discipline before being trusted as
signal, not just plausibility.

**Full activity-clock migration for entry timing itself** (not just barrier scale) is its own
already-large, separately-tracked initiative (`2026-09-06-imbalance-triple-screen-architecture-
spec.md` — 4 open design questions, nothing implemented; and its sibling `imbalance-work-rate-
spec.md`, RESOLVED as transient impact not alpha, retargeted as a Kyle's-Lambda liquidity-gate
candidate for the Sniper's own risk axis, not a directional signal).

**A known, already-documented blocker for anything that wants the MODEL to react intra-bar**
(not just native structural/statistical signals, which have no such constraint): intra-bar
re-inference of the Transformer is explicitly deferred pending ECTS-style training on intra-bar
prefixes, else train/live out-of-distribution. Per-tick τ* recomputation against the standing
completed-bar-trained posterior is what currently provides intra-bar responsiveness instead.

## 4. Cross-cutting Gang-statistical threads feeding multiple stages

- **TA-patterns reformulation** (swing high/low → Pareto/EVT extremity scoring; oscillator
  divergence → Shannon mutual-information framing; Wyckoff Spring/Upthrust stop-harvest case study)
  — Phase 0 literature-grounding not started. Feeds the Predator's structural detection AND the
  Sniper's "WHERE" geometry.
- **Indicator reformulation** (Force Index, MACD, 3/10 oscillator, Impulse System) — Force Index
  Track 1 (calendar-clock) abandoned outright 2026-09-07; Track 2 (`Y_imb`) resolved and retargeted
  per §3 above.
- **Labeling/augmentation reformulation**: the "Thermodynamic Look-Ahead Labeler" (a candidate full
  Triple-Barrier *replacement*, not just a barrier-placement refinement) was **REJECTED 2026-09-18**
  — its core `Y_imb` term is the same construct already proven to decay to noise within 5 bars, an
  evidentiary disqualification for a forward-looking label. The bigger question it was answering —
  should meta-labeling/trend-scanning/regime-conditioned-optimal-stopping replace Triple-Barrier's
  *form* entirely, not just its inputs — **remains genuinely open**, not resolved either way by
  that rejection.
- **Risk-gating reformulation**: `Δ_HS`/Phase-State Machine (blocked, §1), `Y_imb`/Kyle's-Lambda
  liquidity gate (validated, uncalibrated — real next step for the Sniper's risk-veto axis).

## 5. Validation punch-list (the actual dependency-ordered next steps)

1. Redefine the BV-vs-ATR reactivity test with a clock-agnostic event definition (blocks 3).
2. Run the "speed" artifact-vs-signal test (same discipline as the `Y_imb` work).
3. Build and validate the EVT/GPD live stop-distance prototype (the Sniper's actual core
   mechanism).
4. Decide explicitly whether HMM-regime-gated target selection is in scope for v1, given the HMM's
   own not-yet-passed fat-tail sign-off (see `PRODUCTION_TRIAGE.md`'s own Vision section).
5. Entropy-time-barrier: literature search first, real-data test second.

## 6. What stays separate, on purpose

- `docs/ADR/triple_barrier_*.md` (7 files) — the formal ruling record, cited not merged.
- `RegimeManager.h`/`.cpp` — a verified-equivalent facade over existing HMM-state logic, zero live
  callers, its own not-yet-approved 8-file call-site migration (spec lives in `lbrnet`).
- `coevolution_governance.md`'s twin-first validation discipline governs any of the above once it
  moves past design.

## 7. Source specs merged into this document (deleted, full history in git log)

`2026-08-16-predator-decision-contract-execution-risk-framework.md`,
`2026-08-16-predator-context-fusion-infrastructure-spec.md`,
`2026-08-16-turtle-soup-predator-ization-spec.md`,
`2026-08-24-predator-fusion-transformer-signal-decay-spec.md`,
`2026-08-24-two-classifier-cpp-deployment-spec.md`,
`2026-09-03-trade-execution-risk-management-curation-initiative.md`,
`2026-09-04-indicator-gang-statistical-reformulation-initiative.md`,
`2026-09-04-technical-analysis-gang-statistical-reformulation-initiative.md`,
`2026-09-06-risk-gating-gang-statistical-reformulation-initiative.md`,
`2026-09-06-observation-vector-gang-statistical-reformulation-initiative.md`,
`2026-09-06-labeling-data-augmentation-gang-statistical-reformulation-initiative.md`,
`2026-09-06-imbalance-triple-screen-architecture-spec.md`,
`2026-09-06-imbalance-work-rate-spec.md`,
`2026-09-05-activity-clock-triple-barrier-reformulation-spec.md`,
`2026-08-16-elder-raschke-triple-barrier-convergence-backlog.md`.

**Also fully absorbed (§2a/§2b), from `lbrnet` (not deleted there — that repo's own call, this is
our copy of the substance)**: `lbrnet/docs/superpowers/specs/2026-08-18-predator-fusion-secondary-
classifier-spec.md` (Trading Partner Classifier) and `lbrnet/docs/superpowers/specs/2026-08-19-
predator-fusion-tier2-physics-signal-spec.md` (Tier 2 Physics Signal), added 2026-09-18.
