# Activity-Clock (Imbalance-Bar) Reframing of Tail-Risk Signals — Evolving Spec

**Status**: EVOLVING — live brainstorm, being written as it develops, not yet a finished design for
sign-off. Sections below reflect what's actually been decided vs. what's still open; don't treat
early sections as more final than later ones just because they were written first.

## 0. Origin

This thread started from a MindfulTrader/lbrnet/Atratus cross-project brainstorm about feeding the
Student-t HMM a genuine Taleb-style tail-detection signal (motivated by the same Taleb/Mandelbrot/
institutional-quant research done for Atratus's Black Swan Predator — `Atratus/knowledge/
black_swan_literature.md`). That HMM is a **shared source of truth**: MindfulTrader consumes its
regime state live for trading, and Atratus's "Spotter" section will consume the same model's
posteriors via lbrnet's planned HTTP endpoint — so improving this model benefits both consumers.

## 1. Key finding: "Taleb kurtosis" already exists, is already live, and is absent from the HMM

Verified directly against the code (not assumed from docs), 2026-08-26:

- **Computed by** `CalculateRealizedKurtosis()` (`src/StudyHelperFunctions.cpp:2645-2739`) — Moors
  (1988) robust **octile kurtosis**, over a 100-bar window of TS3 (15-min) log returns, with a
  regime-ATR multiplier, clamped `[0,5]`. Replaced plain moment-kurtosis 2026-08-13 (`298b9e0`).
- **Already gates trades through five separate mechanisms**: a hard halt (`RiskManager.cpp:884`),
  a crisis-hysteresis enter/exit gate, Amihud fat-tail tightening, a sigmoid fragility penalty on
  trade score (`Scoring.cpp:223-224`), crash-regime chase/exit logic (`PositionManager.cpp`), and a
  tail-risk-premium composite (`TradeDecisionEngine.h:291-308`). It also drives its own native,
  rule-based `MarketClimate::TALEBIAN_FRAGILE` regime classification (kurtosis > 1.4753,
  `Indicator.cpp:499-527`) — entirely separate from the HMM's own `HMMStateEnum`.
- **It is completely absent from the Student-t HMM's 16D observation vector.** Confirmed against
  `ContextManager::BuildObservationVector()` — none of the vector's dims read `talebKurtosis`.
  `OBS_TAIL_INDEX` (dim9, Hill α) and `OBS_SKEWNESS` (dim10, Bowley) are separate metrics, not this
  one, and both are already dropped per the 16D→12D trim (`2026-08-25-vol-convexity-removal-spec.md`
  in `lbrnet`).
- Also distinct from `ModelKurtosis()` (`include/Indicator.h:1845`, Student-t-DOF-implied,
  `3*(dof-2)/(dof-4)`, used only as a sizing multiplier) — explicitly noted in the code
  (`RiskManager.cpp:1811`) as "a different statistic from Taleb kurtosis."

**Conclusion**: the redundant-hard-gate half of "feed the HMM a tail signal, and also gate on it"
is already done, thoroughly. The actual gap is narrower: the HMM itself never sees this signal.

## 2. Cadence finding: bar-gated by explicit design, not a bug — but not free of tension either

`UpdateObservationVectorSubgraphs()` (`StudyHelperFunctions.cpp:2894`) is called every tick from
`TripleScreen3.cpp:697` (matching `CLAUDE.md`'s "every screen is a hot path" architecture), but an
internal guard (`if (!sc.IsFullRecalculation && lastObsUpdateIndex == sc.Index) return;`,
`StudyHelperFunctions.cpp:2908-2911`) means the actual kurtosis/Hurst/Skewness/Amihud/LiqFragility
calculations only execute once per TS3 bar close — `sc.Index` doesn't change within a still-forming
bar. This is **explicitly intentional**, documented in the code itself (lines 2891-2893): only
`micro_asymmetry` (dim7) is called out as needing every-tick updates, "unlike the rest of this
function's outputs." Not the same bug class as Elder Breakout's fake tick-reactivity (a genuine
logic defect) — these are rolling-window statistics over *completed* bar returns, which don't have
a well-defined "intrabar" version without changing what's being measured.

**The real tension** (raised directly): this system's own design philosophy is "predator, not
historian" (`2026-08-16-predator-decision-contract-execution-risk-framework.md`'s five-element
contract). A tail-risk gate that only updates once per 15-minute bar is, by that philosophy's own
standard, historian-grade on exactly the dimension that most needs to be predator-grade.

**Rejected fix**: naively recomputing the 100-bar kurtosis window using the still-forming bar's
partial return. This doesn't make the signal faster, it changes what's being measured — a partial
bar's return is a different, non-stationary quantity from a completed bar's return, and feeding it
to a model/gate calibrated on completed-bar statistics risks exactly the train/live divergence class
this project has already paid for elsewhere (`lempel_ziv` quantization, the `dim3` rail-hit-rate
mismatch, Turtle Soup's 280x live/train divergence). This mirrors `CLAUDE.md`'s own TRAP section,
which explicitly defers "intra-bar RE-INFERENCE... pending ECTS-style intra-bar-prefix training" for
the identical reason.

## 3. Literature considered for a genuine fix

- **Ané & Geman (2000)**, *Order Flow, Transaction Clock, and Normality of Asset Returns* (*Journal
  of Finance*) — return distributions approach Gaussian (excess kurtosis largely disappears) when
  sampled in transaction/volume time rather than calendar time. Directly on-point for kurtosis
  specifically — suggests some apparent fat-tailedness under clock-time sampling is a sampling
  artifact, not pure signal. Caveat: their specific moment-recovery procedure has been criticized in
  follow-up literature for not cleanly recovering higher moments — a real idea, not an uncontested
  one.
- **Lee & Mykland (2008)**, *Jumps in Financial Markets: A New Nonparametric Test and Jump Dynamics*
  (*Review of Financial Studies*) — a genuinely tick-native jump-detection test (local
  volatility-normalized ratio test). Considered and **set aside**: not López de Prado's own work
  (different, unrelated academic lineage — flagged to correct a misattribution mid-brainstorm), and
  the AFML information-driven-bars approach (below) was judged the better institutional fit for this
  project's own already-established literature base (AFML already grounds CPCV/PBO/meta-labeling
  elsewhere in this system).
- **López de Prado, AFML ch. 2 — information-driven bars** (tick/volume/dollar imbalance bars):
  **the adopted approach.** Rather than fixed-time bars, a bar closes once cumulative *signed*
  order-flow imbalance crosses an adaptively-estimated (EWMA-based) threshold — bars close fast when
  order flow turns one-sided (exactly during tail events), slow when balanced. This doesn't
  approximate the bar-completion problem, it dissolves it: every such bar, once closed, is a
  complete, well-defined observation — just on an activity clock instead of a time clock.
- **Xing/Pei/Yu's ECTS line** (2011 survey; extended to a formal cost-based-optimization criterion,
  *Machine Learning*, 2021) — the literature's disciplined version of "treat a prefix as done early"
  (Minimum Prediction Length, empirically derived, not guessed). Relevant if a fixed-prefix approach
  to the *existing* time-bar kurtosis were ever wanted instead of switching clocks — not currently
  the chosen direction, noted for completeness.

## 4. Proposed design: dual-clock kurtosis via a centralized ActivityClockManager, not a replacement

**Core decision**: don't replace the existing time-bar-cadence kurtosis. Add a second,
independently-clocked version, and feed the HMM both.

**Infrastructure shape, revised 2026-08-26**: the original sketch of a single-purpose
`DollarImbalanceBarEngine` built just to feed kurtosis was the wrong grain. Since §6 already plans
to reuse the same activity-clock machinery for the long-memory family and `PredictionAgeUs` decay,
this should be a **shared, centralized ActivityClockManager** from day one — a new singleton-style manager
matching this codebase's existing convention (`IndicatorManager`, `ContextManager`,
`PositionManager`, `RiskManager` — all called from the same central per-tick dispatch in
`SCStudies.cpp`), not a one-off accumulator built and rebuilt per consumer.

1. **`ActivityClockManager`** — name kept deliberately despite the literal "clock" ambiguity flagged
   mid-brainstorm (this codebase already has three genuinely time-based screens, TS1/TS2/TS3); kept
   because it matches López de Prado's own established terminology (Clark 1973's subordinated
   stochastic clock; Ané-Geman's "transaction clock") rather than avoiding a term the literature
   itself uses correctly.
   Owns one or more activity-clock accumulators, independent of `TripleScreen3.cpp`'s real bars and
   touching no existing pattern/indicator. **Revised, no new raw tick-ingestion mechanism needed**:
   `docs/superpowers/specs/2026-08-12-tick-native-toxicity-illiquidity-design.md` already confirms
   Sierra Chart maintains `sc.BidVolume[sc.Index]`/`sc.AskVolume[sc.Index]` as genuine, tick-accumulated,
   per-bar arrays — real bid/ask trade classification, confirmed reliable at the `.scid` byte level,
   especially robust since the 2026-08-06/07 Denali backfill. On every tick, `ActivityClockManager`
   takes the delta of these already-validated running totals (one persistent float pair remembering
   the last read) and accumulates the signed difference — no second, redundant per-tick accumulator,
   matching that same doc's own stated principle ("building a second, redundant accumulator would
   duplicate work Sierra Chart already does correctly"). The first accumulator: a dollar-imbalance-bar
   construction per AFML's recipe — accumulates signed dollar imbalance since the last imbalance-bar
   closed, closes a bar once `|cumulative imbalance|` crosses an EWMA-derived adaptive threshold.
   **Uses real Ask/Bid classification for the tick rule, not AFML's own default proxy** (AFML's
   uptick/downtick tick rule is itself a fallback for systems without real trade-side classification —
   this system already has something better, so the manager should use it directly rather than
   reimplementing AFML's proxy rule).
2. **Exposes a clean, reusable API** — e.g. "give me the last N completed imbalance-bar returns" —
   so every consumer (kurtosis's activity-clock twin now; the long-memory family and
   `PredictionAgeUs` decay later, per §6) pulls from the same ingestion/accumulation logic instead of
   each reimplementing it. One place to calibrate the EWMA threshold parameters (§8, open question
   2), one implementation to keep C++/Python-consistent, not three-plus quietly-drifting copies.
3. **Reuse the existing Moors-kurtosis calculation verbatim**, pointed at the ActivityClockManager's rolling
   buffer of completed imbalance-bar returns instead of TS3's time-bar returns. Same trusted math,
   different clock — no new estimator to validate, no partial-observation risk (every imbalance bar
   is complete by construction).
4. **Feeds three places, revised 2026-08-26, then corrected same day**: (a) a new HMM observation
   dimension, *alongside* the existing (currently absent, now being added) time-bar-cadence kurtosis
   — not replacing it; (b) a new, fast hard-gate check, structurally parallel to the existing gates;
   (c) an early-trigger additive input to the existing five gate consumers themselves (hard halt,
   crisis-hysteresis enter, sigmoid fragility, chase/exit, tail-risk-premium composite) — the *slow*
   reading can only push toward caution earlier, never override, mirroring `CLAUDE.md`'s own TRAP
   shape ("native governs, model may lead but never suppress the floor"). **Correction, same day**:
   an earlier version of this item justified keeping the slow, time-bar statistic authoritative for
   threshold *values* partly by citing "protects Task 7's percentile-matching work from
   invalidation" — that's an engineering-convenience argument, and it was wrong to weigh it against
   actual risk/return outcomes. Recalibration effort is a one-time, bounded cost; if a properly-
   recalibrated activity-clock statistic produces genuinely better risk-adjusted outcomes (lower
   realized drawdown, better tail-event capture, no material alpha loss from false triggers), that
   cost should simply be paid, not avoided. **Which statistic is authoritative for the five existing
   gates is now explicitly an empirical backtesting question (§8, new open question 13), not a
   design default** — compare the current calibrated gates against gates recalibrated fresh on the
   activity-clock statistic (and the early-trigger hybrid above) on realized drawdown and alpha
   outcomes, and let that decide, not which option avoids rework.
5. **Downgraded, 2026-08-26 — was overclaimed**: the earlier framing here asserted "the divergence
   between the calm time-bar reading and the fast activity-bar reading is likely more informative
   than either alone," analogized to realized-vs-implied vol spreads. **Checked against the actual
   literature and this specific claim is not directly supported.** The closest real precedents are
   the Variance Risk Premium (Bollerslev, Tauchen & Zhou 2009 — a genuine, well-established
   predictive spread, but between *implied* (forward-looking) and *realized* (backward-looking)
   volatility, not two backward-looking measures on different clocks) and volatility signature plots
   (comparing realized volatility across sampling frequencies — closer in shape, but used as a
   *microstructure-noise bias diagnostic*, not a regime-change signal, and the literature itself
   found no clean, systematic divergence pattern across stocks/time). Neither paper makes the
   specific claim asserted here. **Status: `plausible-engineering-choice`, analogous to two adjacent
   established constructs, not itself a directly-cited finding** — matching this project's own
   Gang-doc honesty vocabulary rather than overclaiming. Feeding both dims to the HMM is still
   justified (§6's discipline still requires each dim earn empirical value once built), just not on
   this specific "divergence is informative" argument as stated.
6. **Redundancy is deliberate, not incidental — and now has a sharper, literature-grounded reason,
   not just Taleb's antifragility framing.** The same volatility-signature-plot literature confirms a
   real, established **bias-variance tradeoff across sampling frequency/clock type**: lower-frequency
   measurement is less biased but higher-variance; higher-frequency is more reactive but more
   noise-prone. Time-bar and activity-bar kurtosis genuinely trade this off differently — it's not
   vague redundancy, each clock is better suited to a different concern (calibration-stability vs.
   speed). Also matches this codebase's existing TRAP precedent (two independent observers of one
   truth) and gives genuinely uncorrelated failure modes (a trade-count/volume data issue wouldn't
   affect the time-bar reading and vice versa) rather than two consumers of one identical number.

**Scope boundary, stated explicitly so this doesn't drift**: `ActivityClockManager` manages the
construction of activity-clocked bars and the resulting rolling return/statistic history for *those*
bars — nothing more. It does not manage, own, or touch TS1/TS2/TS3's existing time-bars or any
indicator computed against them (Hurst, Skewness, Amihud, `mean_rev_z`, etc. all keep running
exactly as today). `ContextManager`'s role doesn't change in kind — it remains the single place that
assembles the full observation vector — it simply gains one additional upstream source, used only
for the specific dims that earned activity-clock treatment (§6), alongside its existing TS-bar/HMM
sources for everything else.

**Explicitly considered and rejected, same day**: full activity-clocked *counterparts* to TS1/TS2/
TS3 themselves (i.e., three parallel Elder-Triple-Screen hierarchies, one per clock type). This is a
different, much larger idea than the ActivityClockManager above, and the wrong generalization — TS1/TS2/TS3
aren't just clock cadences, they're Elder's specific trend/momentum/entry-timing methodology, with
dozens of patterns tuned against exactly those three timeframes. Tripling that surface has no
literature backing it (AFML's information-driven bars are proposed for statistical/ML feature
quality, not as a replacement for trend/momentum/entry-timing architecture) and would triple the
C++/Python parity burden for no justified gain. The ActivityClockManager's job is narrower and sound: back a
handful of specific statistical signals with a better clock, not re-architect the trading system's
market-timing hierarchy.

## 4a. Coordination with the existing tick-native initiative (dims 7/11) — converge, don't merge

**Correction, 2026-08-26**: earlier drafts of this section treated `micro_asymmetry` (dim 7) as
still part of the HMM's vector, just excluded from activity-clock treatment. Verified directly
against `lbrnet/docs/superpowers/specs/2026-08-25-vol-convexity-removal-spec.md` (drafted, not yet
implemented): `micro_asymmetry` is one of the **four** dims actually being dropped in the 16D→12D
trim (`vol_convexity`, `tail_index`, `skewness_idx`, `micro_asymmetry`) — not merely deprioritized.
The final 12D vector is: `log_variance_ratio, burstiness_index, relative_range, correction_action,
lempel_ziv, hurst_exponent, fisher_info, amihud_illiquidity, liq_fragility, recurrence_rate,
fractal_dim, mean_rev_z`. So dim 7's OFI fix (below) no longer improves the HMM's own input at all
— it improves whatever *other* live consumers `micro_asymmetry`/`raschke_tactical_trigger`-adjacent
telemetry still has, independent of the observation vector.

`docs/superpowers/specs/2026-08-12-tick-native-toxicity-illiquidity-design.md` (status: DESIGN
APPROVED, not yet implemented) is a separate, already-approved, narrowly-scoped fix: replace dim 7's
unreliable Time & Sales scan with the same `sc.AskVolume`/`sc.BidVolume`-derived OFI formula this
spec's `ActivityClockManager` also depends on, and re-verify (not necessarily fix) dim 11
(`amihud_illiquidity`, which *does* remain in the 12D vector)'s `FeatureScaler`-level zero-collapse.
Both efforts still read the same raw ingredient, HMM-relevance of dim 7 aside.

**Decision: converge at the infrastructure level, don't bundle the specs or implementation tasks.**
The dim 7/11 fix is small, already approved, and ready to ship independently — holding it hostage to
this much larger, still-open-question-laden design (EWMA calibration, historical backfill, §8) would
be a real cost for no benefit. That fix proceeds on its own existing track. `ActivityClockManager`
should still be the one place that owns real `sc.AskVolume`/`sc.BidVolume`-derived tick
classification — dim 7's fix (if it has surviving non-HMM consumers) should read from it rather than
maintaining a second, independent access path, but this is no longer motivated by HMM feature
quality the way it was before this correction.

**VPIN-adjacency, addressed explicitly, not glossed over.** The 2026-08-12 doc (citing
`docs/ADR/liquidity_toxicity_gate_decision.md`, 2026-07-14) already rejected VPIN/BVC-based toxicity
proxies. Precise framing, corrected mid-brainstorm: this was **not** "VPIN replaced by Amihud" — the
established architecture is dual-axis, **Amihud (dim 11) for illiquidity/price-impact** and **OFI/
`micro_asymmetry` (dim 7) for toxicity/adverse-selection**, the axis VPIN was actually proposed for.
VPIN was rejected specifically because its Bulk Volume Classification is a *noisy proxy* for trade
direction, valuable only when real classification is unavailable — which this system already has.
**`ActivityClockManager` is not VPIN and doesn't reopen that decision**: it uses the same real
classification (not a proxy), and its purpose is bar *construction* for tail-risk/long-memory
statistics, not a standalone probability-of-informed-trading score. That said, §4's note that the
manager's *raw* imbalance magnitude could itself be a risk-relevant signal (beyond just feeding
kurtosis) sits close enough to that closed topic that it should get its own explicit sign-off before
being treated as in scope — not assumed safe by proximity to this reasoning.

## 4b. Which other `RiskManager` gates inherit this, and which don't

**Inherit automatically, no extra design work**: every existing gate that already reads
`talebKurtosis`/`gateCtx.talebSignalSigma` — the hard halt, the crisis-hysteresis enter/exit gate,
the sigmoid fragility penalty on trade score, the crash-regime chase/exit logic, and the
`taleb_signal_sigma_threshold`/`HmmRegimeGateTalebBreach` check (§1) — benefits the moment the
dual-clock kurtosis exists, since they're downstream consumers of the same value. Nothing new to
build for them individually.

**Need their own separate justification — same discipline as §6, not an automatic extension just
because the manager exists**:
- **Amihud fat-tail tightening** — already excluded (§6): Amihud is volume-normalized by its own
  formula, no case made for activity-clock treatment.
- **The Shannon entropy gate** — **partially resolved by §5b**: its feed (`InformationEngine`, via
  `UpdateMarketPhysics()`) is already tick-native, so the staleness argument that motivated kurtosis
  doesn't apply here. Whether the entropy *estimator itself* (not its feed cadence) would still
  benefit from activity-clock windowing is a different, still-open question — not examined.
- **The Pareto/Hill-α-based gates** ("TAIL COHERENCE DIVERGENCE" check, Pareto-top-state-ratio gate)
  — tied to open question 5 (`tail_index` under an activity clock). **Narrowed by §5b**: `tail_index`
  itself is already tick-native (`TailRiskEngine`) and its weak ranking isn't a cadence artifact, so
  this is now a lower-priority thread than `skewness_idx`'s genuinely open case.

**Firmly out of scope, a different category, not just "not yet justified"**: daily loss limits,
Kelly sizing, consecutive-loss gates (account-state controls, unrelated to market microstructure),
and the Mahalanobis/DOF-based sizing multipliers (`MahalanobisSizingCap`, `TailWeightDiscount`,
`SizingDurationFactor`, `DofStopScale`) — these are HMM-posterior-derived, a training-side/`lbrnet`
concern, not something this C++ tick-level manager touches.

## 5. Why this specifically improves the *event-driven* observation-vector architecture

The system moved from bar-based to event-driven: the 16D vector is sent to Python / saved to
`.context` whenever Mahalanobis distance detects a meaningful change — which can fire multiple times
intra-bar. But today, of the 16 dims, only `micro_asymmetry` can actually change between two
same-bar firings — kurtosis, Hurst, Skewness, Amihud, LiqFragility are all frozen at their last
bar-close value. So multiple Mahalanobis-triggered firings within one bar currently capture mostly
*stale, repeated* payloads for 15 of 16 dims.

An imbalance-bar-clocked dimension is **always well-defined at any arbitrary sampling instant** —
it reads the latest *completed* imbalance bar, whatever that is right now, with no "partial" state
to worry about (unlike sampling a time-bar value mid-bar). That's a better structural fit for an
already-event-driven architecture than the current time-bar-clocked content is. During a genuine
unfolding tail event — exactly when Mahalanobis-triggered firings matter most — this makes each
*already-happening* firing carry a genuinely fresh tail-risk reading instead of repeating the same
stale number three times in a row. This is a fix to something already built, not a new feature
added for its own sake.

## 5a. Tier 1 discovery, 2026-08-26: a second "already exists, wired to the wrong place" signal

Verified directly against the code, same pattern as §1's Taleb-kurtosis finding, found while looking
for creative extensions of `ActivityClockManager`:

- **`raschkeBurst`** (`ContextManager::CalculateBurstinessIndex()`, `ContextManager.cpp:1510-1517` →
  `eve::CalculateBurstinessIndex(m_eventTimestampsUS)`) is a **genuine, real-event-arrival-timestamp
  -based burstiness measure** — coefficient-of-variation of inter-arrival times, the classic
  Goh-Barabási point-process formulation — computed inside `CheckAndTriggerHMM()`
  (`ContextManager.cpp:1148-1163`), the *same function* that governs the Mahalanobis-triggered
  observation-send described in §5. Already essentially tick-native, on the same event cadence as
  the vector's own send-triggering logic.
- **It never reaches dim1 (`OBS_BURSTINESS_INDEX`)** — that dimension instead uses a cruder,
  TS2-bar-cadence, volatility-rate-ratio proxy (`CalculateBurstiness()`, `StudyHelperFunctions.cpp:
  3148`, via `cfc::ComputeBurstinessIndex()`, comparing recent-window vs. older-window realized-vol
  rates). `raschkeBurst` instead only feeds `LocalRiskContext`/`RiskGateContext` telemetry
  (`ContextManager.cpp:562, 922`) — risk-gate context export, not the HMM's own input vector.
- **Possibly the single highest-value, lowest-cost item in this entire spec**: unlike everything
  else here, this may need **no new infrastructure at all** — not even `ActivityClockManager` — just
  correcting which existing, already-computed, already-tick-native signal feeds dim1. Not yet
  decided whether to *replace* dim1's proxy outright or add `raschkeBurst` as a genuine dual-clock
  companion (mirroring §4's kurtosis treatment) — see open questions.
- **`CalculateEventVelocity()`** (`ContextManager.cpp:591-598`, same `m_eventTimestampsUS` ring
  buffer, same `CheckAndTriggerHMM()` call site, "events per second") — also already tick-native,
  also already computed on this cadence. Currently feeds only `m_lastEventVelocityPerSec`. Connects
  to §6's `CalculateMarketSpeed()` finding below — worth checking whether these two already-existing
  "tempo" signals should be reconciled rather than left as two independent, potentially-inconsistent
  notions of market speed.

## 5b. Were the 4 dropped dims cadence-starved, not genuinely uninformative? Checked precisely, not assumed uniformly

Raised 2026-08-26: could the 16D→12D drop's "weak discrimination" verdict for some of the four
dropped dims (`vol_convexity`, `tail_index`, `skewness_idx`, `micro_asymmetry`) be an artifact of
being frozen across most Mahalanobis-triggered intra-bar sends, the same mechanism §5 identified for
kurtosis, rather than a true reflection of the statistic's value? Checked each individually — the
answer splits, it doesn't apply uniformly:

- **`skewness_idx` — yes, plausible, verified bar-gated.** `CalculateSkewness()`
  (`StudyHelperFunctions.cpp:2931`) sits inside the exact same bar-gated branch as kurtosis — only
  executes once per TS3 bar close. Every intra-bar Mahalanobis firing during that bar carried an
  identical, frozen value. A real signal diluted by duplicate-row staleness in whatever analysis
  ranked it could plausibly look weaker than it is. **Genuine candidate for reconsideration** once
  `ActivityClockManager` exists — strengthens open question 5, doesn't just repeat it.
- **`tail_index` — no, already ruled out, and the evidence is stronger than expected.** Not computed
  in the bar-gated function at all: it lives in a separate, already-existing `TailRiskEngine`, fed by
  `ContextManager::UpdateMarketPhysics()` (`SCStudies.cpp:302`), explicitly commented "tick-driven...
  on every price change." Already fed at maximum resolution when it was ranked marginal (9/16) —
  cadence-starvation cannot explain that ranking. The drop verdict stands on its own evidence,
  independent of this thread's whole premise.
- **`micro_asymmetry` — no, already tick-native** (the one explicit exception to the bar-gating
  guard, confirmed earlier). Its drop is for an unrelated, already-known reason (the unreliable Time
  & Sales scan — a data-quality defect, not a cadence one).
- **`vol_convexity` — no, cadence is irrelevant to its actual defect.** Wrong data source (needs
  options-implied vol, this system is futures-only); computing it more often changes nothing about
  what it's fundamentally missing.

**Side discovery, relevant to `ActivityClockManager`'s own design**: `TailRiskEngine` and
`InformationEngine` (which also feeds Shannon entropy via the same `UpdateMarketPhysics()` call) are
a *third* existing architecture pattern in this codebase — neither the bar-gated
`UpdateObservationVectorSubgraphs()` family nor a from-scratch design — a streaming engine that
ingests raw tick-level returns continuously and answers queries on demand. A proven precedent worth
modeling `ActivityClockManager`'s internals on, not just a tangential find. Also partially resolves
§4b's open item on the Shannon entropy gate: its *feed* is already tick-native, so it likely doesn't
need activity-clock treatment for the staleness reason kurtosis did — whether the entropy estimator
itself would still benefit from activity-clock *windowing* (a different question) remains open.

## 5c. Sequencing decision, 2026-08-26: `skewness_idx` first, grounded in Student-t HMM theory + web-literature check

Given four follow-up items needing resolution (`skewness_idx` reconsideration, `correction_action`'s
unexamined status, `fisher_info`'s separate naming/window question, `burstiness_index`'s
replace-vs-add call), checked against this project's own `2026-08-12-gang-literature-grounding-spec.md`
plus a web-literature pass before sequencing, rather than picking an order arbitrarily.

**`skewness_idx` goes first.** Three reasons, not just "it's next in line":
1. **The statistical-formula question is already closed** by the Gang doc: Kim & White (2004)'s
   robust-estimator critique already drove the Bowley (1920) quartile-skewness replacement,
   implemented and marked `validated`. Nothing to re-derive.
2. **Reuses infrastructure already fully designed for kurtosis.** §5b verified `skewness_idx` sits
   in the identical bar-gated code path as kurtosis — direct reuse of `ActivityClockManager`, not a
   new design.
3. **Direct web-literature confirmation, not just an analogy extended from kurtosis**: a real
   empirical comparison (Jarque-Bera testing across bar types) found volume bars come *closest* to
   normal-distribution skewness *and* kurtosis, time bars are the *worst*, dollar bars sit in
   between — a direct result for skewness specifically, strengthening §5b's case for giving it the
   same activity-clock treatment as kurtosis, not just borrowing kurtosis's own justification by
   proximity.

**A bigger question this surfaced, flagged but explicitly NOT decided here — belongs to `lbrnet`,
not this spec.** Skewed Student-t emission distributions are already institutional standard in
exactly this setting: Hansen (1994) introduced the two-piece skewed Student-t specifically for
asymmetric conditional financial returns; Fernández & Steel (1998) generalized it; Markov-switching
models with skewed-t emission distributions are an established technique, adopted specifically
because a *symmetric* Student-t is known to be insufficient when real regime-conditional returns
show meaningful skew. This means the deeper version of "does skewness matter to this HMM" isn't
just "does `skewness_idx` deserve an activity-clock twin as an observation feature" (in scope here)
— it's **"should this system's Student-t HMM itself use a skewed-t emission distribution per
state, instead of a symmetric one?"** That's a real, legitimate, training-side architectural
question, but it's `lbrnet`-rooted (which distribution family the HMM fits), not something to design
or decide from this MindfulTrader-rooted session — matching this project's own standing "`lbrnet`:
separate repo/session, don't mix scope" convention. Logged here so it isn't lost or quietly
conflated with the narrower, in-scope observation-feature question.

**Proposed sequence**: `skewness_idx` (activity-clock twin, ready to design now) → `correction_action`
(needs fresh Gang-pillar classification first — genuinely blank slate) → `fisher_info` (its own
track: naming fix + Ehlers-convention window check, not a Gang-grounding question) →
`burstiness_index`'s replace-vs-add call (an engineering decision, not blocked on further research,
can happen whenever).

## 6. Generalization — which other dims/mechanisms get the same treatment, and which don't

**Explicit discipline stated and held**: this is not a blanket "activity-clock everything" move.
Each candidate needs its own literature-grounded justification and has to earn empirical
discriminative value, same bar every existing dim already had to clear in the 16D→12D trim. Every
dual-clocked dimension is also a new C++/Python parity surface — and this project's own North Star
already names the twin-parity gap (`PRODUCTION_TRIAGE.md` row 5/7) as the single biggest blocker
across the whole system, so this isn't a cost to take on carelessly.

**Sequenced in, same underlying justification as kurtosis**:
- **`hurst_exponent` — CORRECTED and STRENGTHENED, 2026-08-27, via a real literature pass (not the
  Weron 2002 sample-size argument this bullet originally rested on).** Direct citation found:
  trading time (cumulative trades executed) is established in the long-memory-estimation literature
  as the more natural timescale, reducing biases regular calendar-time sampling introduces — the same
  Clark (1973)/Ané & Geman (2000)/AFML ch. 2 lineage already grounding kurtosis, extended here on its
  own direct merits, not by analogy. **This corrects an earlier, unresearched mid-conversation claim
  (2026-08-27, same day) that Hurst was "genuinely time-based and resistant to clock conversion"** --
  that reasoning was stated before checking the literature and the literature says the opposite; see
  `2026-08-25-observation-vector-institutional-hardening-spec.md` §5a for the full citation detail
  and the additive-vs-replace framing (has live gate consumers,
  `StudyHelperFunctions.cpp:623`/`TripleScreen3.cpp` regime thresholds — needs kurtosis's dual-clock
  pattern, not `skewness_idx`'s replacement pattern). **Real stakes**: `hurst_exponent` is this
  system's single worst HMM cross-state discriminator (exactly `0.0000`) -- if clock choice is a
  genuine contributor (not certain, but now literature-plausible), this bears directly on row 1's
  Student-t HMM sign-off problem, not just this spec's own scope.
- **`fractal_dim` (Sevcik) and `recurrence_rate` (RQA) — NOT literature-grounded for clock
  conversion, corrected 2026-08-27; remain window-widening-only.** This bullet originally grouped
  these two with `hurst_exponent` under one "long-memory/complexity family" justification (Weron
  2002's N≥256 sample-size requirement for Hurst/DFA). **That grouping was already wrong before this
  literature pass, independent of it**: this project's own Gang literature-grounding doc
  (`2026-08-12-gang-literature-grounding-spec.md` row "Sevcik fractal dimension window") already
  established that Sevcik's method is "a single-pass calculation, not a multi-scale regression like
  DFA -- it doesn't carry DFA's same minimum-sample fragility," so Weron's N≥256 DFA-specific finding
  never actually applied to `fractal_dim` in the first place. A separate 2026-08-27 literature search
  specifically for RQA/Sevcik-fractal-dimension under information-driven vs. time-bar sampling found
  **no direct precedent either way** -- silence, not a ruling. Both dims stay on
  `2026-08-25-observation-vector-institutional-hardening-spec.md`'s pure time-bar window-widening
  path (§5 there) until a real literature hook is found, not extended here by unsupported analogy.
- **`lempel_ziv` — NOT examined by either literature pass (2026-08-26 or 2026-08-27); status
  unchanged, listed here only so it isn't silently assumed either way.** Its own C++ real-fix is
  independently deferred (`PRODUCTION_TRIAGE.md` row 3, explicit user decision, 3+ days data
  collection cost) -- any activity-clock question for it is moot until that lands regardless.
- **`PredictionAgeUs()` / `HmmStateAgeUs()` staleness decay** — currently wall-clock microseconds;
  the decay time constant is *already* an open, undecided parameter in
  `2026-08-24-predator-fusion-transformer-signal-decay-spec.md` ("needs empirical derivation from
  this system's own inter-prediction-arrival-interval distribution... not a borrowed literature
  value"). Decaying against elapsed *activity* (volume/dollar volume since the prediction) rather
  than elapsed clock time is the same argument applied to signal freshness instead of tail-shape: a
  prediction made 5 minutes ago during a flash crash is far staler, informationally, than one made 5
  minutes ago during a quiet lull. **Not yet designed in detail** — flagged here so it isn't
  designed independently and then rediscovered as the same idea later.
- **Realized-volatility / ATR-based sizing and stop distances** — same activity-clock idea applied
  to volatility estimation broadly; well-precedented in the realized-vol literature ("realized
  volatility in volume time" is an established variant). **Not yet scoped in detail.**
- **`CalculateMarketSpeed()`** (`StudyHelperFunctions.cpp:326`, verified live) — a True-Range-based
  tempo proxy feeding the already-wired `AdaptiveWindowParams::UpdateWindows()` mechanism, confirmed
  live at ≥2 call sites (`WINDOW_STATE_PTR`, `FISHER_WINDOW_STATE_PTR`,
  `StudyHelperFunctions.cpp:2290-2334`). True Range can spike from one large print with little real
  order-flow imbalance behind it; `ActivityClockManager`'s bar-formation *rate* (how fast imbalance
  bars close) is a more information-theoretically grounded tempo signal, and because
  `AdaptiveWindowParams` is already reused across multiple screens, improving this one input would
  ripple out to every existing consumer at once — high leverage if it pans out. **Not yet scoped in
  detail**; should also be reconciled with §5a's `CalculateEventVelocity()` finding rather than
  treated as a third independent "market speed" notion.

**Correction, 2026-08-26**: `fisher_info` is **not** a Fisher Information Measure / complexity
estimator, despite the name and my earlier assumption to the contrary. This project's own
`2026-08-12-gang-literature-grounding-spec.md` (Finding 1) already caught this: it's **Ehlers'
technical-analysis Fisher Transform**, repointed at normalized price position, with a window
(30-120 bars) 3-12× longer than Ehlers' own documented 9-10 bar convention. Not a Shannon-pillar
construct at all — its open question is a naming/documentation fix plus checking the window against
Ehlers' own stated convention, a different kind of investigation than the Gang-grounded ones in this
spec. Removed from the long-memory-family framing; tracked in §8 as its own, separate item.

**Flagged as the next candidate to examine, given genuinely blank-slate status — not yet done, per
the 2026-08-26 dim-accounting pass (§8, open question 11)**:
- **`correction_action`** — shares the exact same recent-window-vs-older-window ratio formulation as
  `burstiness_index`'s flawed TS-bar-cadence proxy (same function family, `CarryForwardCalculators.h`
  groups dims 1/3 together in its own doc comment). May have the same kind of fixable gap §5a found
  for `burstiness_index` — not yet examined.

**Moved OUT of "explicitly not generalized to," 2026-08-27 — a real literature hook was found**:
- **`mean_rev_z`** — this bullet previously said "no literature hook connects [it] to the time-change
  argument." That was true only until a 2026-08-27 literature pass specifically for this dim: it is
  built on lag-1 return autocorrelation (`rho`), and separate microstructure literature (non-
  synchronous trading, bid-ask bounce) directly establishes that **calendar-time sampling is itself a
  source of *spurious* serial correlation** -- the exact statistic `mean_rev_z` measures. This is a
  distinct, direct citation, not an analogy borrowed from kurtosis. Full detail, additive-vs-replace
  framing (has a live gate consumer, `Scoring.cpp:305` -- needs kurtosis's dual-clock pattern), and
  citations: `2026-08-25-observation-vector-institutional-hardening-spec.md` §5a.

**Explicitly NOT generalized to, with reasoning**:
- **Amihud illiquidity / LiqFragility** — already volume-normalized by construction (the formula
  itself divides by dollar volume); an activity clock is likely redundant with what the formula
  already does. No case made yet for revisiting this.

**Not part of the final 12D vector at all, so not applicable here**: `micro_asymmetry` — corrected
2026-08-26 (§4a): this is one of the four dims being *dropped* in the 16D→12D trim, not merely
excluded from activity-clock treatment. Its own tick-native OFI fix (§4a) still matters for whatever
non-HMM consumers it may retain, but not for this spec's purpose.
- **`log_variance_ratio`, `relative_range`** — genuinely untouched by this entire brainstorm; no
  lead yet on whether either has a case for activity-clock treatment.

**Named but deliberately out of scope for this spec** (bigger forks, each deserving its own future
brainstorm, not bundled in here):
- **The Triple Barrier's vertical (time) barrier itself**, reframed in activity-bars instead of
  clock-time/bars — genuinely predator-consistent (a fast adverse move forces a decision sooner; a
  quiet consolidation gets more patience), but touches live exit logic directly — much larger blast
  radius than an observation-vector addition.
- **HMM regime tenure measured in activity-time rather than calendar days** — a training-methodology
  question that belongs in `lbrnet`, not decided from a MindfulTrader-rooted session.

## 6a. Speculative ideas — preserved, not pursued, don't lose these

Raised during the 2026-08-26 creative sweep. Explicitly **not** justified by literature or evidence
the way §5a/§6's other candidates are — kept here on purpose so they aren't quietly dropped, not
because they're ready to build.

- **A combined "tempo × tail-shape" regime fingerprint** — cross bar-formation rate (§6's
  `CalculateMarketSpeed` candidate) with activity-clocked kurtosis (§4) into a new, native 2-axis
  regime signal, distinct from both `MarketClimate::TALEBIAN_FRAGILE` (kurtosis-only) and the HMM's
  own trained states. Interesting, unverified, no literature case built yet — would need one before
  being taken seriously.
- **Execution-quality / order-timing informed by real-time signed imbalance** — a genuinely
  different application domain from everything else in this spec: smart order timing/slippage
  avoidance, not risk-gating or HMM feature quality. Named because it's a real "elsewhere" use of
  `ActivityClockManager`'s raw imbalance data, not because it's been scoped at all.

## 7. Feasibility question — RESOLVED, independently re-verified 2026-08-26 (spike, not just cross-reference)

Originally: whether Sierra Chart's ACSIL exposes per-trade signed volume/aggressor-side data cheaply
on every tick. Initially resolved by cross-reference to `2026-08-12-tick-native-toxicity-illiquidity-
design.md`; **independently re-verified today via a dedicated spike** rather than trusting the
cross-reference alone, per the "verify before concluding" standard the rest of this spec holds
itself to:

1. **Sierra Chart's own ACSIL documentation** confirms `sc.BidVolume`/`sc.AskVolume` update per-tick
   with upticks/downticks correctly classified into Ask/Bid — gated on one real configuration
   dependency: "Intraday Data Storage Time Unit" must be set to "1 Tick" in Global Settings. Not a
   universal guarantee, but strongly, circumstantially confirmed already satisfied here: the
   2026-08-12 `.scid` byte-level audit found `trades/record == 1.000` and `single-trade-records ==
   100%` across three separate eras (2023/2025/2026 data) — coarser storage would routinely bundle
   multiple trades per record, and it doesn't, anywhere sampled.
2. **This codebase's own code states the exact property needed, as an explicit design rationale**:
   `TripleScreen3.cpp:645-653`'s comment reads outright — *"sc.BidVolume/AskVolume accumulate
   throughout the still-forming bar"* — and `micro_asymmetry`'s entire tick-native design already
   depends on and exploits exactly this property, successfully, in production.
3. `VolumeIndicator::UpdateVolume()` (`TripleScreen3.cpp:641-642`) already reads these same fields
   every tick, in the identical code path, proven reliable in the exact replay sessions where a
   different hand-rolled approach (`CalculateMicroAsymmetryFromTimeAndSales`) failed ~45% of the
   time.

**Conclusion**: `ActivityClockManager` doesn't need its own raw tick-ingestion mechanism — it deltas
these already-trusted, already-intrabar-accumulating running totals every tick (§4), the same
technique this codebase already trusts for `micro_asymmetry`, just applied to build an imbalance
accumulator instead of a ratio. No outstanding feasibility risk on this point.

## 8. Open questions

1. ~~Feasibility of tick-level signed volume/aggressor data in ACSIL~~ — **RESOLVED**, see §7.
2. EWMA calibration for the imbalance-bar threshold (expected bar duration, expected imbalance
   magnitude) — needs real historical ES tick data, not an invented constant, matching this
   project's standing rule against invented thresholds.
3. Whether the imbalance-bar-clocked kurtosis actually discriminates better than the time-bar
   version once built — an empirical question, not assumed from the design's elegance alone.
4. Historical backfill mechanics — the imbalance-bar engine needs to run over the full historical
   tick dataset to generate a training-time version of this dimension, not just live-forward; exact
   mechanism (C++ replay vs. Python-side reimplementation) not yet decided.
5. **Narrowed by §5b**: whether re-adding `skewness_idx` under an activity clock is worth
   revisiting — genuinely strengthened (verified bar-gated, same mechanism as kurtosis, plausible
   staleness-diluted-its-ranking story). `tail_index` is **no longer part of this question** — §5b
   verified it's already tick-native (`TailRiskEngine`), so its weak ranking isn't a cadence
   artifact; its drop verdict stands independently.
6. Naming/placement of the new engine relative to existing `PredatorContext`/`PredatorFusion`
   infrastructure (`2026-08-16-predator-context-fusion-infrastructure-spec.md`) — may be a natural
   fit for that dispatch mechanism rather than a fully separate component; not yet checked against
   that spec's actual API.
7. Whether `ActivityClockManager`'s raw imbalance magnitude (not just downstream statistics like
   kurtosis) should itself become a risk-relevant signal — flagged in §4a as VPIN-adjacent; needs its
   own explicit sign-off, not assumed in scope by proximity to this design.
8. Whether `micro_asymmetry`'s tick-native OFI fix (§4a) has any surviving non-HMM consumer worth
   converging onto `ActivityClockManager` — no longer an HMM-feature-quality question since it's
   being dropped from the vector; not yet checked whether it matters for anything else.
9. Whether `raschkeBurst` (§5a) should *replace* dim1's current TS2-bar-cadence proxy outright, or
   be added as a genuine dual-clock companion mirroring §4's kurtosis treatment — not decided; this
   one may not even need `ActivityClockManager` to resolve, since the real signal already exists.
10. Whether `CalculateEventVelocity()` and `CalculateMarketSpeed()` (§5a/§6) should be reconciled
    into one tempo signal, or are legitimately measuring different things and should stay separate —
    not examined yet.
11. **Narrowed by §5c**: whether `correction_action` has its own version of the §5a finding — a
    real, sound, already-computed signal not reaching where it'd help most — not examined yet;
    sequenced after `skewness_idx` (§5c). `fisher_info` **removed from this question** — corrected
    (§6, §5c): it's Ehlers' Fisher Transform, not a complexity estimator, and needs its own
    naming/window-convention fix, not Gang-grounding. `log_variance_ratio`/`relative_range` still
    have no lead at all.
12. Whether this system's Student-t HMM should use a skewed-t emission distribution per state
    (Hansen 1994; Fernández & Steel 1998) instead of a symmetric one — a real, institutionally-
    precedented question surfaced by §5c's web-literature check, explicitly flagged as `lbrnet`-
    rooted and NOT decided or in scope here. Do not conflate with question 5's narrower, in-scope
    `skewness_idx` observation-feature question.
13. **New, 2026-08-26 — corrects an earlier design-default error (§4 item 4).** Whether the slow,
    time-bar kurtosis or a freshly-recalibrated activity-clock kurtosis should be authoritative for
    the five existing gate consumers' threshold values is an empirical backtesting question, not a
    default. Needs: gates recalibrated fresh against the activity-clock statistic's own real
    historical distribution (not reusing the old thresholds), backtested against the current
    calibrated gates and the early-trigger hybrid, compared on realized drawdown and alpha outcomes
    — whichever wins on actual risk-adjusted performance becomes authoritative, not whichever avoids
    recalibration effort. Also settles, empirically rather than by assumption, whether activity-clock
    windows' duration instability (days-long when quiet, minutes-long during a crisis) materially
    degrades gate usefulness in practice or is a theoretical concern that doesn't show up on real
    data.

## 9. References

- `CLAUDE.md` — Trap Detection (Native-First) section, the precedent for two-observers-of-one-truth
  and the deferred intra-bar-reinference reasoning this spec's §2 mirrors.
- `docs/superpowers/specs/2026-08-16-predator-decision-contract-execution-risk-framework.md` —
  "predator, not historian" philosophy this whole thread is answering to.
- `docs/superpowers/specs/2026-08-25-observation-vector-institutional-hardening-spec.md` — §5's
  pure window-widening path (`recurrence_rate`/`fractal_dim`) and §5a (added 2026-08-27, the full
  literature-grounded `mean_rev_z`/`hurst_exponent` activity-clock detail this spec's §6 summarizes).
- `docs/superpowers/specs/2026-08-24-predator-fusion-transformer-signal-decay-spec.md` — the
  already-open `PredictionAgeUs()` decay-constant question §6 connects to.
- `lbrnet/docs/superpowers/specs/2026-08-25-vol-convexity-removal-spec.md` — the 16D→12D trim this
  spec's new dimension(s) would be added on top of.
- `Atratus/knowledge/black_swan_literature.md` — the Taleb/Mandelbrot/institutional-quant research
  that originated this whole thread.
- `docs/superpowers/specs/2026-08-12-tick-native-toxicity-illiquidity-design.md` — the sibling,
  already-approved dim 7/11 fix this spec's §4a converges with (real `sc.AskVolume`/`sc.BidVolume`
  data source shared by both); resolved §7's feasibility question.
- `docs/ADR/liquidity_toxicity_gate_decision.md` (2026-07-14) — the VPIN/BVC rejection §4a addresses
  directly, with the corrected dual-axis (Amihud/OFI) framing.
- `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md` — this project's own
  Shannon/Mandelbrot/Taleb/Pareto literature-grounding framework; source of the `fisher_info`
  mislabeling correction (Finding 1) and the already-adopted Kim & White/Bowley-skewness resolution
  §5c builds on.
- Kim, T.H. & White, H. (2004). "On more robust estimation of skewness and kurtosis." *Finance
  Research Letters*, 1(1), 56-73.
- Hansen, B. E. (1994). "Autoregressive Conditional Density Estimation." *International Economic
  Review*, 35(3), 705-730. (Two-piece skewed Student-t distribution.)
- Fernández, C. & Steel, M. F. J. (1998). "On Bayesian Modeling of Fat Tails and Skewness."
  *Journal of the American Statistical Association*, 93(441), 359-371.
- Ané, C. & Geman, H. (2000). "Order Flow, Transaction Clock, and Normality of Asset Returns."
  *Journal of Finance*, 55(6), 2259-2284.
- Clark, P. K. (1973). "A Subordinated Stochastic Process Model with Finite Variance for
  Speculative Prices." *Econometrica*, 41(1), 135-156. (The root citation Ané & Geman and AFML's
  information-driven bars both extend — added 2026-08-27, per §6's `hurst_exponent`/`mean_rev_z`
  correction, not previously cited here despite grounding the whole approach.)
- Non-synchronous trading / bid-ask bounce as a source of spurious return serial correlation —
  general microstructure finding cited 2026-08-27 (§6) grounding `mean_rev_z`'s clock-conversion
  case specifically; exact primary citation not yet pinned to a single named paper in this pass,
  flagged for follow-up rather than left uncited.
- Trading-time (cumulative trades executed) as the more natural timescale for long-memory/Hurst
  estimation, reducing regular-sampling bias — cited 2026-08-27 (§6) grounding `hurst_exponent`'s
  clock-conversion case; exact primary citation not yet pinned to a single named paper in this pass,
  flagged for follow-up rather than left uncited.
- Bollerslev, T., Tauchen, G., & Zhou, H. (2009). "Expected Stock Returns and Variance Risk
  Premia." *Review of Financial Studies*, 22(11), 4463-4492. (Cited §4 item 5 — real precedent for
  divergence-as-signal, but implied-vs-realized, not the specific claim made there.)
- Zhang, L., Mykland, P. A., & Aït-Sahalia, Y. (2005). "A Tale of Two Time Scales: Determining
  Integrated Volatility with Noisy High-Frequency Data." *Journal of the American Statistical
  Association*, 100(472), 1394-1411. (Volatility signature plots / multi-frequency divergence as a
  microstructure-noise diagnostic — cited §4 items 5-6; already in this repo's own
  `2026-08-12-gang-literature-grounding-spec.md`.)
- López de Prado, M. (2018). *Advances in Financial Machine Learning*, Ch. 2 (information-driven
  bars).
- Weron, R. (2002). "Estimating long-range dependence: finite sample properties and confidence
  intervals." *Physica A*, 312(1-2), 285-299.
- Xing, Z., Pei, J., & Yu, P. S. (2011). "Early Classification on Time Series." *Knowledge and
  Information Systems*. Extended: (2021), "Early Classification of Time Series: Cost-based
  Optimization Criterion and Algorithms." *Machine Learning*.
