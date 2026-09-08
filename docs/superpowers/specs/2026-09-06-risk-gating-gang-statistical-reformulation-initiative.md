# Risk-Gating — Gang-Statistical Reformulation Initiative

**Status, opened 2026-09-06: seeded entirely from an external operator/Gemini brainstorm session
conducted outside this repo (`/mnt/c/Trading/MTS_Fractal_Evolution.txt`, no repo access, no
in-thread citation verification, no code). Twin/sibling of
`docs/superpowers/specs/2026-09-04-technical-analysis-gang-statistical-reformulation-initiative.md`
(chart patterns) and `docs/superpowers/specs/2026-09-04-indicator-gang-statistical-reformulation-
initiative.md` (named indicator formulas) — same discipline, a third scope: is the live
risk/regime-GATING signal layer (not an indicator formula, not a pattern-detection rule) built on
solid Gang-statistical footing? Nothing implemented, nothing independently verified. Read §1 before
anything else — it flags a real naming collision with this repo's own existing, already-approved
architecture that must not be missed.**

## 0. Origin and mandate

Same "is this move statistically rigorous, or an ad hoc heuristic" question already applied to
chart patterns and indicator formulas, now applied to the risk-gating/regime layer specifically —
the signals that decide *how carefully* to trade a given regime, not *whether a pattern fired* or
*what an indicator reads*. Directly adjacent to, and should be read alongside,
`docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md` (the
"is `RiskGateContext` blind to the HMM's own signal" initiative — founding question answered
2026-09-04: 7 of 8 hard gates are blind to live HMM regime state). This doc's candidates are
concrete, if unverified, answers to that initiative's still-open §2 item 3 (extend gate 1's
`Dof()`-based HMM-awareness pattern to gates 2-5) and item 2 (an EVT/GPD "how close to the tail"
signal) — a second candidate axis (Hurst-persistence vs. state-entropy divergence), not a
replacement for either.

## 1. CRITICAL: naming collision with this repo's own existing, approved "Predator" architecture — read before anything else

**The brainstorm's proposed `PredatorState` enum (Search/Stalk/Lock/Fire/StandDown) is NOT the same
thing as this repo's own existing `PredatorContext`/`PredatorFusion.h`
(`docs/superpowers/specs/2026-08-16-predator-decision-contract-execution-risk-framework.md`,
approved, partially implemented) — despite the identical name and one directly-overlapping term
(the brainstorm's "Stalk (Coiled Spring)" phase vs. this repo's own real, TRAINED
`HMMStateEnum::COILED_SPRING` state).** This is very likely not a coincidence — the brainstorm was
almost certainly seeded by a description of this system's real architecture — but the two are
**structurally different constructs**:

- **This repo's real `PredatorContext`** (`include/PredatorContext.h`) is a flat POD struct:
  `LocalRiskContext gang` (existing Gang/Taleb/Pareto/Shannon fields) + `HMMStateEnum regime` (the
  live Student-t HMM's actual fitted state, one of 4: `COILED_SPRING`/`GAUSSIAN_STABLE`/
  `GAUSSIAN_FRAGILE`/`PARETO_MOMENTUM`) + `inPosition`/`applicabilityMask`. The "Predator Decision
  Contract" (5 numbered requirements, `docs/superpowers/specs/2026-08-16-...md`) governs how fusion
  functions (`PredatorFusion.h`, e.g. `FuseTauStar`) may read this context to gate entry/exit
  decisions. This is real, approved, and partially wired (TRAP's τ* fusion is the flagship example).
- **The brainstorm's `PredatorState`** is a proposed NEW 5-state phase-space machine
  (Search/Stalk/Lock/Fire/StandDown) driven by the SIGN of `dH_norm/dτ` and `dE_H/dτ` (entropy-rate
  and Hurst-memory-rate derivatives) — a candidate REGIME-CLASSIFICATION scheme, conceptually
  closer to what the HMM itself already does (assign one of 4 states) than to the existing
  `PredatorContext` struct (which just carries whatever regime the HMM already assigned).

**Do not implement anything from §2 below under the name `PredatorState`, `Predator*`, or any name
overlapping this repo's existing symbols without an explicit renaming decision first** — treat this
as a five-phase CANDIDATE regime-classification idea that would need to be reconciled against (not
substituted for) the HMM's own already-trained 4-state output before any code is written. Internally,
this doc refers to it as the **Phase-State Machine** to keep the two concepts unambiguous.

## 2. Candidate constructs (CANDIDATE, unverified, no cited literature grounding in the source brainstorm)

**Important honesty flag, distinct from the sibling initiatives' own candidates**: every construct
below has **zero literature citations** in the source brainstorm — no named paper, no verified
precedent, unlike (for example) the indicator initiative's `ΔP x √V` (Kyle & Obizhaeva 2016) or
`amihud_illiquidity` (Amihud 2002). These read as a plausible, self-consistent synthesis of concepts
already real elsewhere in this repo (Hurst, Shannon entropy, HMM state probabilities), not verified
academic constructs. A literature-grounding pass (matching the sibling initiatives' own Phase 0
discipline) is required before any of these move past CANDIDATE status — this section records the
math and the plausible connection to this repo's existing building blocks, nothing more.

### 2.1 Persistence-Entropy Divergence Index (`Δ_HS`) and Markovian State Disconnect (`D_KL`)

```
H̃(t)     = 2·|H(t) - 0.5|                                    ∈ [0,1]   (normalized Hurst deviation)
H̄(S_t)   = -Σ_i P(S_t=i)·log2(P(S_t=i)) / log2(K)             ∈ [0,1]   (normalized Shannon state entropy, K=4 here)
Δ_HS(t)  = H̃(t) · H̄(S_t)                                      ∈ [0,1]   (Coherence Anomaly Score)

D_KL(P‖Q) = Σ_i P(S_t=i)·log2( P(S_t=i) / Q(S_t=i|H(t)) )               (State Expectation KL Divergence)
```

- **`Δ_HS` interpretation**: healthy trend/range = high Hurst-memory paired with LOW state
  uncertainty (`Δ_HS ≈ 0`). `Δ_HS → 1` = "Exhaustion/Climax Trap" — trajectory history still shows
  strong persistence, but the HMM's own state-probability distribution has dissolved into near-
  uniform uncertainty across its 4 states — a candidate live signal for "the model no longer agrees
  with what price history suggests," i.e. exactly the "does the HMM's own signal feed back as a
  heads-up" question the trade-execution initiative's founding question asks.
- **`D_KL` interpretation**: `Q(S_t|H(t))` is an as-yet-undefined expected-state distribution
  conditioned on the Hurst regime (e.g. `H(t)>0.60` should imply the HMM's own state distribution
  concentrates on a continuation-consistent state) — a genuine design gap, not just an
  implementation detail: what `Q` actually is has not been specified, only gestured at.
- **Proposed applications (brainstorm's own framing, unverified)**: exhaustion exit (tighten/scale
  out if `Δ_HS>0.65` while in a profitable position); false-breakout filter (require `Δ_HS<0.25`
  during a swing high/low breakout, else flag as a liquidity sweep) — this second application
  directly overlaps `docs/superpowers/specs/2026-09-04-technical-analysis-gang-statistical-
  reformulation-initiative.md`'s own swing-high/low case study; cross-reference, don't duplicate,
  if this is ever pursued.
- **Feasibility, CHECKED 2026-09-06 — confirmed blocker, not just unchecked**:
  `hurst_exponent`/`fast_hurst_exponent` already exist in this system's computed vector, but
  per-state posterior probabilities `P(S_t=i)` are NOT exposed anywhere on the C++ side. `include/
  InferenceManager.h`/`HmmStateIndicator` (`include/Indicator.h`) expose only the argmax
  `HMMStateEnum` plus scalar diagnostics (`RiskMultiplier`, `TransitionRisk`, `Dof`, `Mahalanobis`,
  `TailWeight`, `ExpectedDuration`, `Entropy`) — no per-state probability array exists in the
  current API surface at all. `Δ_HS`/`D_KL` as specified in §2.1 above are **not buildable as-is**:
  either the Python HMM server needs to start sending the full state-probability vector over the
  wire (a real protocol change, not a C++-only fix) and `HmmStateIndicator` needs a new field to
  hold it, or the formulas need reformulating around what's actually available (e.g. `Entropy` —
  already exposed — might already BE a usable proxy for `H̄(S_t)`, not yet checked against §2.1's
  own definition).

### 2.2 Phase-State Machine (brainstorm's own name: "PredatorState" — renamed here, see §1)

Five candidate regime-classification states, driven by the SIGN of two rates of change
(`dH_norm/dτ` = entropy rate, `dE_H/dτ` = Hurst-memory-acceleration rate — both on the imbalance
clock `τ`, per this repo's own established activity-clock convention):

```
Search    : H_norm high, dE_H/dτ ≈ 0, dH_norm/dτ ≈ 0    -- thermalized noise, stand down
Stalk     : dH_norm/dτ < 0, dE_H/dτ ≤ 0                 -- entropy compressing, energy coiling
Lock      : dH_norm/dτ → 0 (local min), dE_H/dτ > 0      -- pre-ignition threshold
Fire      : dE_H/dτ > 0, H_norm low, IEI = E_H/H_norm crosses θ_fire  -- expansion, "authorized"
StandDown : dE_H/dτ > 0 AND dH_norm/dτ > 0               -- price expanding but entropy ALSO
                                                             inflating -- fakeout/liquidity trap
```

- **Direct structural overlap with this repo's real, existing `HMMStateEnum` (4 states) and the
  Impulse System redesign already scoped in the indicator initiative's §5.5/5.6** (Hurst-persistence
  gating + entropy-rate-of-change fusion) — this is very likely the SAME underlying idea as the
  indicator initiative's §7 item 4 (Phase Coherence State, GREEN/RED/BLUE), extended to 5 states
  with an explicit "coiling" pre-state. **These two docs' candidates should be reconciled, not
  developed independently** — whichever of the two (3-color Phase Coherence State vs. 5-state
  Phase-State Machine) is pursued first should absorb the other's design questions, not be
  designed twice.
- **Real open design question, not resolved in the source brainstorm**: does this replace, feed
  into, or run alongside the HMM's own 4-state classification? A 5-state phase machine driven by
  raw entropy/Hurst rates and a 4-state Student-t HMM fitted via EM on 18 features are two
  different classification mechanisms answering a similar question — reconciling them (or
  deciding they serve genuinely different purposes, e.g. this as a faster/tick-reactive pre-filter
  ahead of the HMM's own slower-updating regime) is unresolved.

### 2.3 Not carried forward from the brainstorm (recorded for completeness, not pursued here)

The brainstorm's "High-Probability Pounce Setups" (Compression Breakout, Trend Pullback
Re-ignition) and its "Execution Gating Matrix" (2x2 quadrant of `d/dt E_H` vs `H_norm` direction)
are applications of §2.2's Phase-State Machine, not independent constructs — not recorded
separately here; they inherit whatever status §2.2 eventually gets.

### 2.4 Activity-clock Kyle's Lambda (`Y_imb`) — real-data-tested transient-impact/liquidity gate, retargeted from the observation-vector track

**Unlike §2.1/§2.2, this candidate IS empirically validated on real data** (not brainstorm-only) —
retargeted here 2026-09-06 after being closed out as an observation-vector alpha candidate, see
`docs/superpowers/specs/2026-09-06-imbalance-work-rate-spec.md` §8 for the full investigation.

```
θ_τ      = signed cumulative imbalance at imbalance-bar close (ImbalanceBarEngine's own trigger
           condition — ask volume minus bid volume, accumulated until |θ_τ| crosses threshold)
Y_imb(τ) = ΔP_τ / θ_τ                     -- price displacement per unit of signed order flow
```

- **What was found (real MES ticks, 471.9M ticks, threshold=700, 108,691 bars)**: naive momentum
  on `Y_imb`'s sign has a real, non-artifact effect at 1-bar horizon (confirmed 0.6457 vs. absorbed
  0.4038 hit rate, survives duration-gap and skip-1-bar controls) that decays to noise by 5 bars —
  literature-grounded (Almgren & Chriss 2001; Bouchaud, Gefen, Potters & Wyart 2004; Eisler,
  Bouchaud & Kockelkoren 2012; Cont, Kukanov & Stoikov 2014) as the well-known TRANSIENT component
  of price impact, structurally an activity-clock analogue of **Kyle's Lambda** (Kyle 1985).
- **Why it belongs here, not in the observation vector**: transient impact is a liquidity-cost/
  execution-quality signal, not directional alpha (it contains ~zero permanent/informational
  content per the K=5 collapse) — exactly the kind of thing `RiskGateContext`'s existing Amihud
  illiquidity veto and `liq_fragility` gate already exist to measure, just on the calendar clock
  and dollar volume instead of the imbalance clock and signed order flow.
- **Proposed application (unverified, CANDIDATE)**: use `|Y_imb|` (or its trailing distribution)
  as a real-time, event-clock-native liquidity-fragility signal — e.g. dynamically tightening
  `allowedSpreadTicks` or gating market-order entries when recent imbalance bars show large
  `|Y_imb|` (thin/fragile book) — a higher-frequency modernization of the existing Amihud gate,
  candidate answer to `docs/superpowers/specs/2026-09-03-trade-execution-risk-management-
  curation-initiative.md` §2 item 3 (extend gate 1's HMM-awareness/live-reactive pattern to other
  gates), not a replacement for the HMM-awareness question itself.
- **CHECKED 2026-09-06 — NOT redundant, genuinely complementary, not a straight upgrade either.**
  `ComputeAmihudIlliquidity` (`include/CarryForwardCalculators.h`) is a geometric-mean-aggregated
  (Hasbrouck 2009), sqrt-volume-scaled (Kyle & Obizhaeva 2016) ratio of `|log-return|/sqrt(dollar-
  volume)` over a ~20-40 BAR ROLLING WINDOW — magnitude-only (no sign), driven by TOTAL volume, a
  smoothed liquidity-cost measure. `Y_imb = ΔP_τ/θ_τ` is computed per SINGLE imbalance bar (no
  window aggregation), SIGNED (preserves confirmed-vs-absorbed direction, the whole basis of the
  K=1 finding), and driven by NET/SIGNED order flow, not total volume. These measure genuinely
  different things — Amihud answers "how costly is trading here on average," `Y_imb` answers
  "did this specific burst of directional pressure move price or get absorbed." `liq_fragility`
  (`CalculateLiquidityFragility`) is also volume-scaled and median-based but its exact formula
  wasn't pulled this pass — same "different axis, not redundant" conclusion likely holds by the
  same reasoning, not independently re-verified.
- **Still not done**: no threshold/percentile calibration; no wiring into `RiskGateContext`
  proposed or attempted — the relationship-to-existing-gates question above is resolved, but
  nothing about implementation has started.
- **Available groundwork**: `tools/observation_vector/imbalance_work_rate_eval.cpp` (real-data
  validated, includes the duration-binning and skip-1-bar diagnostics) and the `ImbalanceBarEngine`
  extension (`m_completedBarImbalances`/`GetImbalanceBarMagnitudes()`) it depends on already exist
  and are committed — reusable starting point, not a from-scratch build.

## 3. Relationship to existing initiatives — do not duplicate

- **`docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md`**
  is the primary home for anything that ships as a `RiskGateContext` consumer — §2.1's `Δ_HS`/`D_KL`
  are candidate answers to that doc's §2 item 2 (EVT/GPD execution signal) and item 3 (extend gate
  1's HMM-awareness pattern), not a separate gate design. Cross-reference from there once/if this
  moves past CANDIDATE.
- **`docs/superpowers/specs/2026-09-04-indicator-gang-statistical-reformulation-initiative.md`**
  §7 item 4 (Phase Coherence State) likely duplicates §2.2 above — reconcile before building either.
- **`docs/superpowers/specs/2026-08-16-predator-decision-contract-execution-risk-framework.md`**
  is the REAL, approved home for anything using the name "Predator" — see §1's naming-collision
  warning. Any eventual implementation of §2.2 must either adopt a distinct name or be explicitly
  folded into this contract's own maturity inventory, not bolted on separately.

## 4. Status vocabulary

Same as the sibling ledgers: **CANDIDATE** (proposed, not yet built) · **OPEN** (actively being
investigated) · **BLOCKED** (real work identified, blocked on something else finishing first).
§2.1/§2.2 are **BLOCKED, not merely CANDIDATE, as of 2026-09-06**: (a) a real literature-grounding
pass is still needed (none exists yet — see §2's honesty flag); (b) the feasibility check against
`InferenceManager`'s actual exposed API is now DONE and confirms a real blocker — per-state
posterior probabilities are not exposed anywhere on the C++ side today, only the argmax
`HMMStateEnum` plus scalar diagnostics (§2.1's own updated bullet); (c) reconciliation with the
indicator initiative's own overlapping Phase Coherence State candidate (§3) is still needed. §2.4
is **CANDIDATE** on a stronger footing than §2.1/§2.2 — real-data validated and literature-grounded
already, and its relationship check against the existing Amihud/`liq_fragility` gates is now DONE
(confirmed complementary, not redundant) — remaining work is calibration and `RiskGateContext`
wiring, not open feasibility questions.

## 5. Next steps (not started)

1. Literature-ground `Δ_HS`/`D_KL` — is there a named precedent (regime-switching / persistence-vs-
   uncertainty divergence literature) or is this a pure synthesis needing its own honest "no direct
   precedent found" framing, same as this repo's own `FeatureScaler` shrinkage-floor spec once
   required?
2. ~~Check whether `InferenceManager`/`HmmState()` exposes per-state posterior probabilities
   today~~ — **CHECKED 2026-09-06: NO, confirmed absent.** See §2.1's updated feasibility bullet
   for the exact API surface and two possible paths forward (wire-protocol change, or reformulate
   around the already-exposed `Entropy()` field — not yet checked as a substitute).
3. Reconcile §2.2 (Phase-State Machine) with the indicator initiative's §7 item 4 (Phase Coherence
   State) into one candidate, not two independently-designed overlapping ones.
4. Only after 1-3: decide whether any of this becomes a `RiskGateContext` signal (trade-execution
   initiative's scope) or a `PredatorContext` extension (existing Predator Decision Contract's
   scope) — not both independently.
5. ~~§2.4 (`Y_imb`/activity-clock Kyle's Lambda): check its relationship to the existing Amihud/
   `liq_fragility` gate formulas~~ — **CHECKED 2026-09-06: confirmed complementary, not redundant.**
   See §2.4's updated bullet. Remaining work is calibration + `RiskGateContext` wiring, independent
   of items 1-4 above.
