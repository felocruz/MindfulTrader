# Technical Analysis Constructs — Gang-Statistical Reformulation Initiative

**Status, opened 2026-09-04: seeded from a conceptual discussion (3/10 oscillator's fast/slow-scale
structure prompted the "chart-based TA and Gang statistics share the same underlying move" framing).
Scope is deliberately broad — every chart-based TA construct this system uses (swing high/low,
oscillator divergence, and others not yet enumerated), not a single-construct effort. Not yet
actively worked — no code written, no literature search done beyond what's already cited in sibling
initiatives. Swing high/low + oscillator divergence are the first seeded case study (§1-§2); a second
case study (stop-harvest/reversal, §3) was folded in 2026-09-04 — not the whole scope, expect more
constructs to be added as this initiative grows.**

## 0. Origin and mandate

Direct sibling of `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md`
(observation vector) and `docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md`
(risk/execution gates) — same discipline, a third layer. Those two ask "is the data the HMM trains on
/ the data risk gates fire on solid and literature-grounded?" This one asks the same question one
layer further upstream, across the whole chart-based TA surface: **is technical-analysis pattern
detection itself (swing high/low, oscillator divergence, and whatever else gets identified as this
initiative grows) built on solid statistical footing, or on ad hoc fixed-window/threshold rules that
the Gang toolkit (Shannon, Mandelbrot, Taleb, Pareto) already used elsewhere in this repo could
replace with something more rigorous?**

**Founding observation (operator, 2026-09-04)**: MACD, the 3/10 oscillator, Bollinger/Keltner bands,
moving-average ribbons — structurally all the same move: take one process at two different
scales/windows and treat the discrepancy as signal. That is the same shape as the statistical
constructs already in production (Hurst/DFA = self-similarity across scales, Shannon entropy =
information-theoretic surprise, Taleb kurtosis/GPD = tail-risk, Pareto/EVT = extreme-value
characterization). Swing high/low is the crudest version of this family — "is this point extreme
relative to a fixed N-bar window" — and oscillator divergence is "does momentum's version of extreme
disagree with price's version of extreme." Neither currently has a literature-grounded statistical
formulation; both are fixed-window/threshold heuristics today. **Other TA constructs in this codebase
(e.g. Keltner-band rejection, NR7/volatility-contraction breakout, ATR-based stops) likely share the
same fixed-window/threshold shape and are candidates for this initiative once §1-§2's case study is
far enough along to generalize from — not yet enumerated or triaged.**

## 1. Case study #1 — swing high/low & oscillator divergence: current chart-based implementation (baseline, as of 2026-09-04)

- **Swing high/low (price)**: `IntermediateMarketAction::updateSwingHigh`/`updateSwingLow`
  (`src/Indicator.cpp:275-290`) — simple "new extreme replaces old extreme" state, no statistical
  characterization of how extreme.
- **3/10 oscillator**: `Oscillator310` (`include/Indicator.h:~2161`) — fast line = EMA(3)-EMA(16),
  slow line = SMA(16) of fast line; `ComputeOscillator310Cross` (`include/IndicatorComputations.h:~487`)
  detects signal-line crossovers (fast vs. slow), a stateless, threshold-free comparison.
- **Oscillator divergence**: per `docs/TRANSFORMER_DATA_FIELDS.md`/`docs/CPP_TRANSFORMER_FIELDS_IMPLEMENTATION.md`,
  implemented in `src/DataCollectorStudy.cpp:336-424` — fixed 25-bar lookback window, minimum 3-bar
  swing spacing, compares two price swings against two oscillator swings, emits a binary
  bullish/bearish/none flag.

## 2. Case study #1 conceptual mapping: chart-based move → Gang-statistical analogue (not yet designed, ideas only)

| Chart-based TA move | Gang-statistical analogue | Status |
|---|---|---|
| Swing high/low = local pivot vs. a fixed N-bar window | **Pareto/EVT**: score extremity via the process's own fitted tail distribution (GPD return level), reusing this repo's own existing GPD-fitting machinery (currently only used to derive *static* `FeatureScaler` calibration bounds, e.g. `tools/observation_vector/observation_vector_recalibration.cpp`'s `FitGPD()`) rather than an arbitrary N-bar rule | CANDIDATE |
| Oscillator divergence = fast/slow disagreement at price vs. oscillator extrema | **Shannon**: reformulate as falling mutual information / rising conditional entropy between price-extremity and momentum-state at that point, replacing a binary flag with a graded information-theoretic surprise measure | CANDIDATE |
| Fixed 25-bar divergence lookback / 5-bar swing detection window | **Mandelbrot/Hurst**: window width should scale with the currently-measured persistence/fractal regime (same discipline already applied to `fractal_dim`'s window-widening, `72ab967`), not be a hardcoded constant | CANDIDATE |
| Binary bullish/bearish divergence flag (0/1/2) | **Taleb**: continuous tail-probability/surprise score instead of a threshold-triggered boolean, so "how much of a divergence" is graded | CANDIDATE |

None of these have literature citations verified in-thread yet (unlike the observation-vector
initiative's §1, which required independently-verified sources before any implementation) — that
verification pass is Phase 0 of this initiative, not yet started.

## 3. Case study #2 — stop-harvest/reversal (Wyckoff Spring/Upthrust, Livermore's "coming and going") via order-flow run bars

**Status: seeded 2026-09-04 from a conceptual discussion, not yet scoped or designed — a candidate
second case study, not a commitment.**

Origin: operator observation of a recurring pattern — a hard directional drop (stop harvesting)
reaching a bottom, then an equally hard reversal — traced to Wyckoff's "Spring" (false breakdown
shaking out stops) and its mirror "Upthrust", and Livermore's own "I got them coming and going." This
is not a new pattern to this system:

- **Already implemented, native-first**: the TRAP detection framework (`CLAUDE.md`'s Trap Detection
  section) — `StructureTest` `FAILED_*` reversal (reactive floor) + Transformer `TRAP_*`
  (anticipatory), gated by dynamic Bayesian threshold τ* = C_FP/(C_FP+C_FN) (Elkan 2001). This is
  functionally a Spring/Upthrust detector already.
- **Already named in the 9-pattern catalog**: Turtle Soup (false breakout of a prior N-day extreme,
  then fade) and Kangaroo Tail (single-bar probe-and-reject) are both textbook instances of this same
  shape at different timeframes.

Conceptual mapping to a Gang-statistical/imbalance formulation (none built yet):

| Phase | Chart-based signature | Gang-statistical signature |
|---|---|---|
| Hard drop (stop harvest) | Sharp red candle(s) | A **run** of consecutive sell-imbalance bars (AFML's "run bars" — distinct from plain imbalance bars, purpose-built to detect one-sided/mechanically-driven order flow), often paired with elevated `amihud_illiquidity`/`liq_fragility` |
| Bottom/exhaustion | Subjective "selling looks done" | Imbalance **flips sign** — objective, real-time end of the sell-run |
| Reversal | Sharp green candle(s) | Buy-imbalance run begins; short-horizon `mean_rev_z`/Hurst anti-persistence conditional on the prior extreme |

**Critical caveat, must not be skipped**: an extreme move and an *exhausted* move are not the same
thing — conflating them is the classic Taleb tail-risk trap (plenty of extreme drops keep going). A
GPD/EVT tail-extremity reading alone is NOT a reversal signal by itself; the existing TRAP framework
already encodes this discipline correctly (native floor requires a *confirmed structural reversal
test*, not extremity alone, and the anticipatory signal is cost-gated via τ*, never firing on
extremity alone). Any reformulation here must preserve that discipline, not regress to "big move =>
fade it."

**Direct dependency on §5's correctness-bug track**: Turtle Soup's own live firing-rate data is
currently unreliable — `docs/superpowers/specs/2026-08-25-pattern-detection-institutional-hardening-spec.md`
§3.2 found live `raschke_tactical_trigger` Turtle Soup firing at ~280x below its validated Python
dataset rate (3/million vs. ~853/million), not yet root-caused. **Do not use live Turtle Soup trigger
data to validate or motivate this case study until that discrepancy is resolved** — it would be
validating against a known-unreliable signal.

Open, not yet answered: does an imbalance-run-based reformulation belong as a NEW signal feeding the
existing TRAP framework (most likely, given TRAP is already the authoritative owner of this exact
pattern class), or as an independent pattern-detection addition? Not yet decided.

**Sibling initiative, opened same day**: `docs/superpowers/specs/2026-09-04-indicator-gang-
statistical-reformulation-initiative.md` covers named *indicators*' own formulas (Force Index, MACD,
Stochastic, RSI, the 3/10 oscillator's construction) — a distinct scope from this doc's *pattern-
detection logic* concern, though the two will cross-reference often (e.g. Force Index divergence-
with-price is both "an indicator's formula" there and "a divergence pattern" here).

## 4. Status vocabulary

Same as the sibling ledgers, for consistency: **IN** (settled, stays as-is) · **IN-WEAK** (stays,
weak, no better alternative identified) · **IN-PENDING-FIX** (stays, a decided implementation change
not yet done) · **PAUSED** (groundwork exists, explicitly do not proceed without new evidence) ·
**CANDIDATE** (proposed, not yet built) · **OPEN** (actively being investigated) · **BLOCKED** (real
work identified, blocked on something else finishing first).

## 5. Related but SEPARATE work — do not conflate

`docs/superpowers/specs/2026-08-25-pattern-detection-institutional-hardening-spec.md` (row 13 of
`PRODUCTION_TRIAGE.md`) is investigating a suspected **correctness bug**, not a redesign:
`raschke_tactical_trigger` appears to be a sticky field that never resets to `NONE` (evidence in that
spec's §3.1), corrupting 4 of 9 patterns' live firing rates by up to ~280x versus their validated
Python dataset rates. That spec's Phase 0 diagnosis is not yet complete (blocked on 5 open questions,
§7, untouched since 2026-08-25). **This initiative should not build a new swing-high/low/divergence
formulation on top of the current chart-based pipeline without first checking whether that pipeline's
own bug affects swing/divergence state too** — the sticky-field mechanism described there was found in
`RaschkeTacticalIndicator`/`KangarooTail`, not yet checked against `IntermediateMarketAction`'s swing
tracking or `DataCollectorStudy.cpp`'s divergence detector specifically. Verify before assuming either
document is complete on its own. **Also gates case study #2 (§3) — see that section's own dependency
note.**

## 6. Other candidate TA constructs for future case studies (not yet triaged)

Named in the founding observation (§0) as likely sharing the same fixed-window/threshold shape as
case study #1, but not yet investigated, scoped, or even confirmed to be a good fit for this
initiative: Keltner-band rejection (`setKeltnerRejection`), NR7/volatility-contraction breakout,
ATR-based stop placement (`ChandelierStopManager`). This list is a starting point for triage, not a
commitment that all three belong here — each needs its own "is this actually the same fixed-window
heuristic problem" check before being added as a real case study, same bar case study #1 itself still
needs to clear in Phase 0.

## 7. Open questions (Phase 0, not started)

1. Does `IntermediateMarketAction::updateSwingHigh/updateSwingLow` or the divergence detector in
   `DataCollectorStudy.cpp` share the sticky-field failure mode documented in §5? Not yet checked.
2. Literature grounding: is there existing academic precedent for EVT/GPD-based local-extremum
   scoring specifically (as opposed to this repo's existing use of GPD for calibration-bound fitting),
   and for entropy-based divergence detection? Not yet searched.
3. Would a Gang-statistical swing/divergence score feed the observation vector (HMM), the risk-gate
   layer (`RiskGateContext`), the pattern-detection/scoring layer, or a new consumer entirely? Not yet
   decided — each has a different redundancy risk (same `tail_index`-vs-ν_k trap already learned in
   the trade-execution initiative's item 2 applies here too if this ever feeds back into the HMM's own
   observation vector).
4. Is this initiative additive (new signal alongside existing swing/divergence detection) or a
   replacement (retiring the fixed-window chart-based version)? Not yet decided — replacing carries
   train/serve parity risk for any existing consumer of the current fields (per §5's Transformer
   `FeatureSpec` dependency already flagged in the pattern-detection spec).
5. Case study #2 (§3): does an imbalance-run-based stop-harvest/reversal signal feed the existing
   TRAP framework, or become an independent pattern-detection addition? Not yet decided.

## 8. Next steps

Phase 0 (not started): verify §7 question 1 against real data, do the literature-grounding pass for
§2's four candidates (mirroring the observation-vector initiative's §1 discipline — every citation
independently verified, not asserted), decide §7 questions 3-5 before writing any code, triage
§6's candidate list once case study #1 is far enough along to know what "the same shape" actually
means in practice, and resolve §5's Turtle Soup live-data discrepancy before validating case study #2
against it.
