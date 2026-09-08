# Imbalance Work Rate — Spec (Track 2 of 2: new, activity-clock-native)

**Status: RESOLVED, 2026-09-06 — closed out as a Track 2 observation-vector alpha candidate, see
§8. `Y_imb` is real (survives duration-gap and skip-1-bar controls) but decays to noise by 5
imbalance bars — literature-grounded as the well-known TRANSIENT/temporary component of price
impact (Almgren & Chriss 2001), not alpha. Retargeted to the risk-gating initiative
(`docs/superpowers/specs/2026-09-06-risk-gating-gang-statistical-reformulation-initiative.md` §2.4)
as an activity-clock Kyle's Lambda liquidity/execution-cost gate candidate instead. §§1-7 below are
kept as the historical record of the original alpha-candidate investigation, opened 2026-09-06,
split from `docs/superpowers/specs/2026-09-04-indicator-gang-statistical-reformulation-initiative.md`
§7 item 1 (external brainstorm cross-pollination). Track 2 of 2 — companion Track 1 (calendar-
clock Force Index hardening) was abandoned outright 2026-09-07 (operator directive) and its own
spec deleted, not just closed; this closure is unaffected either way. Enum name and construction
below were first-draft proposals for the alpha path and are now moot — see §8 for what (if
anything) carries forward.**

**CORRECTED 2026-09-06 (operator directive) — this is the MIGRATION TARGET, not a permanent
addition alongside Track 1.** This repo's broader goal is a full eventual transition from
time-clock to activity/imbalance-clock across the whole system — a parallel "Imbalance Triple
Screen" (`ImbalanceScreen1.cpp`/`ImbalanceScreen2.cpp`/`ImbalanceScreen3.cpp`, structurally
mirroring `TripleScreen1/2/3.cpp` at 3 imbalance-bucket sizes instead of 3 calendar periods) plus
new `ImbalanceMarketObservation`/`ImbalanceTrainingEvent` FlatBuffer tables alongside (eventually
replacing) `MarketObservation`/`TrainingEvent`. Once this construct (or its eventual successor) is
validated and cut over, Track 1's calendar-clock Force Index gets deleted outright, per this
repo's own standing pre-production rule (`CLAUDE.md`: "the default is deletion, not preservation").
This spec is an early, single-indicator instance of that larger migration, not an independent
addition living beside the legacy version forever.

## 0. Scope and architecture (operator directive, 2026-09-06)

**This is a genuinely new indicator, not a reformulation of Force Index** — different clock
(imbalance/activity, via `ImbalanceBarEngine`, not calendar bars), different inputs (signed
imbalance and price yield, no volume term at all), different consumer path. Unlike this repo's
established permanent-twin precedent (`hurst_exponent`/`fast_hurst_exponent`,
`mean_rev_z`/`fast_mean_rev_z`), **this construct is the intended eventual REPLACEMENT for Force
Index, not a permanent parallel addition** — see the corrected Status note above. It will
eventually live inside the future `ImbalanceScreen1/2/3.cpp` study files (whichever
macro/intermediate/micro frame fits Force Index's own existing Screen 1/Screen 2 role split), not
`TripleScreen1/2/3.cpp`.

**File placement (operator directive)**: the new enum (`ImbalanceWorkRateEnum`, name open) lives in
`include/rc_enums.h` (ACSIL-independent, cross-language parity with `lbrnet`'s own
`core/rc_enums.py`, matching `HMMStateEnum`'s existing rationale) — this is a new indicator from
day one, not an extraction of something already `lbrnet`-visible, so parity should be built in from
the start rather than retrofitted later (the retrofit-later path is exactly what `rc_enums.h`'s own
header comment flags as a "separate, non-surgical consolidation" for `MacdEnum` et al.). The new
indicator CLASS (`ImbalanceWorkRateSignal : public Indicator<ImbalanceWorkRateEnum>`, matching
`FI2Signal`/`FI13Signal`'s own structure — training-event serialization, packed-slot float export,
any smoothing/robustness wrapper) stays in `include/Indicator.h`, same file as every other concrete
`Indicator<T>`-derived class.

## 1. Origin: the "Work Rate" formula (external brainstorm, no cited literature grounding)

```
θ_τ      = signed cumulative imbalance at bar close (ask volume - bid volume, accumulated until
           |θ_τ| crosses the imbalance-bar threshold -- ImbalanceBarEngine's own trigger condition)
Y_imb(τ) = ΔP_τ / θ_τ                          -- Imbalance Yield: price displacement per unit imbalance
W_τ      = θ_τ · Y_imb(τ)  =  ΔP_τ              -- ALGEBRAIC DEGENERACY, see §2 below
```

**Same honesty flag as the risk-gating/labeling twin docs**: zero literature citation for this
specific construction in the source brainstorm (`MTS_Fractal_Evolution.txt`) — this is a plausible
synthesis using concepts real elsewhere in this repo (imbalance bars, price yield), not a verified
academic construct. Requires its own literature-grounding pass before promotion past CANDIDATE.

## 2. A real problem found before any implementation: the literal formula is algebraically degenerate

**Checked directly, not assumed**: `W_τ = θ_τ · Y_imb(τ) = θ_τ · (ΔP_τ / θ_τ) = ΔP_τ` — the
imbalance term cancels exactly, leaving plain price change. Taken completely literally, "Work Rate"
carries zero information beyond `ΔP` alone, which would fail the doc's own stated validation bar
(§4) trivially and isn't worth building at all.

**This is very likely not what was actually intended** — re-reading the brainstorm's own framing
("Work Rate: Measures the conversion efficiency of aggressive volume imbalance into actual price
displacement"), the intended construct is almost certainly `Y_imb` ALONE (the yield/efficiency
ratio itself — how much price moves per unit of imbalance, a genuine distinct signal from `θ_τ` or
`ΔP` individually), not `θ_τ · Y_imb` re-multiplied back out. **Proposed correction, pending
confirmation**: drop the redundant multiplication, treat `Y_imb(τ) = ΔP_τ / θ_τ` itself as the
candidate signal (large |Y_imb| = efficient/thin-liquidity move; small |Y_imb| = high imbalance
absorbed with little price progress — an exhaustion/absorption signature, conceptually close to
Amihud's own illiquidity ratio but on the imbalance clock instead of dollar volume). This
correction is this repo's own finding, not in the source brainstorm — flag as such if this ever
goes back to Gemini for further review.

## 3. A real engineering gap found before any implementation: `ImbalanceBarEngine` doesn't expose what this needs

**Checked directly against `include/ImbalanceBarEngine.h`, 2026-09-06**: the engine currently
stores only completed-bar LOG-RETURNS (`m_completedBarReturns`, a ring buffer) — it does NOT store
each bar's own cumulative imbalance magnitude (`θ_τ`) at the moment the bar completed;
`m_cumulativeImbalance` is reset to `0.0f` immediately after a bar closes (`OnTickWithPrice()`,
lines 45-51) with no prior value retained anywhere accessible. Computing `Y_imb(τ)` requires BOTH
the bar's return AND its completion-time imbalance magnitude — **this is a real prerequisite
extension to `ImbalanceBarEngine` itself, not something that can be computed from its current
public interface (`GetImbalanceBarReturns()` alone is insufficient)**. Any implementation must add
a parallel ring buffer (or a combined struct) capturing `θ_τ` alongside each bar's `ΔP_τ`/return —
scoped, contained, but a real prerequisite step before §4's validation tool can even be written.

## 4. Proposed candidate enum states (first draft, open to revision)

Pending §2's correction (using `Y_imb` directly, not the degenerate `W_τ`) and mirroring
`FI2Enum`/`FI13Enum`'s own semantic shape (trend-confirmation / divergence / crossover) but
reinterpreted for an efficiency-ratio construct:

```cpp
enum class ImbalanceWorkRateEnum : int8_t {
    NEUTRAL           = 0,   // |Y_imb| within its own normal empirical range
    ABSORPTION        = 1,   // large imbalance, small price yield -- liquidity absorption/exhaustion
    EFFICIENT_THRUST  = 2,   // large imbalance, large price yield -- genuine directional conviction
    BULLISH_DIVERGENCE = 3,  // price makes a new low, Y_imb doesn't confirm (shallower/reversing)
    BEARISH_DIVERGENCE = -3  // symmetric top case
};
```

**Not finalized** — this is a first-draft proposal for discussion, matching the same "propose,
don't silently decide" posture as every other CANDIDATE in the sibling twin docs. Real percentile
bounds for NEUTRAL/ABSORPTION/EFFICIENT_THRUST would need the same GPD/empirical-percentile
recalibration convention already used throughout this project's `FeatureScaler.h` work, once real
`Y_imb` data exists to calibrate against (blocked on §3's engine extension).

## 5. Validation plan (required before implementation — this is a NEW candidate dim, not a hardening)

Unlike Track 1 (construction-correctness only), this needs the full "does this dim deserve to
exist at all" bar already established for other candidate additions in this repo
(`jump_ratio_eval.cpp`, `drift_location_eval.cpp`, `mean_rev_z_variant_comparison.cpp`):

1. Extend `ImbalanceBarEngine` per §3 (or build a standalone offline replica, matching this
   session's own established `tools/observation_vector/` pattern, rather than touching the
   production engine before validation).
2. Compute `Y_imb(τ)` (§2's corrected formula) against real MES tick data resampled onto the
   imbalance clock.
3. Forward-return/hit-rate test: does `Y_imb(τ)` (or its rate-of-change) carry genuine
   forward-predictive information about momentum continuation/exhaustion, beyond what `ΔP` alone
   or this system's own existing activity-clock dims (`fast_hurst_exponent`, `fast_taleb_kurtosis`)
   already provide? If the imbalance clock's own construction already captures the useful
   clustering (the same possible-null outcome already flagged for `ΔP×√V` in Track 1's parent
   case study), this should be dropped, not built.

**Decision rule**: promote to CANDIDATE-VALIDATED only if item 3 shows a real, non-redundant
effect (same bar as `jump_ratio_eval.cpp`'s own real, survived result) — literature grounding
alone (still missing, per §1) is necessary but not sufficient, matching this repo's own standing
discipline throughout every other reformulation this session.

## 6. Explicitly out of scope

- Any change to `CalculateForceIndex`/`FI2Enum`/`FI13Enum`/`FI2Signal`/`FI13Signal` — see Track 1's
  companion spec, permanently separate.
- Any `ObservationData`/`mts_schema.fbs` field addition — a schema change is a separate, later
  decision gated on §5's validation surviving, not assumed here.
- Wiring into any live `RiskGateContext`/`Scoring.cpp`/Screen entry-gate consumer before validation.

## 7. References

- Kyle, A.S. (1985), "Continuous Auctions and Insider Trading," *Econometrica* — Kyle's Lambda,
  the formal ancestor of the price-impact-per-unit-flow concept `Y_imb` approximates.
- Amihud, Y. (2002), "Illiquidity and stock returns," *Journal of Financial Markets* — the closest
  existing named construct in spirit (price impact per unit flow), already implemented in this
  repo for dollar volume; `Y_imb` is the imbalance-clock analogue, not yet cross-checked against
  Amihud's own methodology in detail.
- `docs/superpowers/specs/2026-09-04-indicator-gang-statistical-reformulation-initiative.md` §7
  item 1 — the external brainstorm cross-pollination entry this spec formalizes and corrects (§2).
- Force Index Track 1 (calendar-clock hardening) — abandoned outright 2026-09-07, spec deleted;
  was the permanently-separate companion track, now moot.
- `include/ImbalanceBarEngine.h` — the existing engine this construct depends on and must extend
  (§3).

## 8. Empirical validation results and final decision (2026-09-06)

**§3's engine gap was fixed** (a second index-aligned ring buffer, `m_completedBarImbalances`,
plus `GetImbalanceBarMagnitudes()`, `include/ImbalanceBarEngine.h`) and §5's validation tool was
built and run against the full real dataset (471,930,891 real MES ticks,
`/home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet`, `tools/observation_vector/
imbalance_work_rate_eval.cpp`, imbalance-threshold=700, 108,691 bars). §2's correction (using
`Y_imb` alone, not the degenerate `W_τ`) is what was tested.

**Raw naive-momentum hit rate, confirmed (`Y_imb>0`) vs. absorbed (`Y_imb<0`) subsets:**

| horizon | confirmed hit rate | absorbed hit rate |
|---|---|---|
| K=1 | 0.6457 (n=102,794) | 0.4038 (n=3,742) |
| K=5 | 0.5097 (n=103,828) | 0.4952 (n=3,772) |
| K=20 | 0.5021 (n=103,813) | 0.5109 (n=3,772) |

A real, non-overlapping-CI split at K=1, decayed to statistical noise by K=5, fully gone by K=20.

**Two follow-up diagnostics, run to test whether K=1 is a bar-construction artifact** (a single
order-flow burst/meta-order spanning >1 imbalance-bar boundary would mechanically produce this
exact K=1-only pattern with zero real information):

1. **Duration-binned K=1 test** (hit rate binned by wall-clock gap between consecutive bar
   closes): bars closing <1 second apart (~1.9% of the sample) show near-deterministic
   continuation/reversal (confirmed hit rate 0.92–1.00, absorbed 0.00 across three sub-second
   buckets) — this portion is very likely pure artifact. But the remaining 97%+ of bars (gap ≥1s)
   still show a split nearly identical to the unconditioned result (confirmed 0.6396, absorbed
   0.4093) — the artifact does not explain the bulk of the effect.
2. **Skip-1-bar test** (bar t predicts bar t+2's own return, bar t+1 skipped entirely): confirmed
   0.5470 (down from 0.6457), absorbed 0.4558 (up from 0.4038) — both CIs still exclude 0.50, so a
   real but weaker effect survives even without the immediately-adjacent bar.

**Conclusion: `Y_imb`'s K=1 effect is real, not pure artifact — but it is extremely short-lived**
(materially decayed within 1-2 bars, fully gone by 5, in imbalance-bar/event time).

**Independent, literature-grounded review (Gemini, read-only, no code access changes made)**:
consulted twice — first for the artifact-vs-signal question (confirmed the burst-splitting
hypothesis as the most likely explanation for the sub-second buckets, proposed the two diagnostics
above), then for an institutional literature-grounded verdict on what to do with the surviving
effect. Verdict, with citations:

- The decay shape (near-deterministic at sub-second gaps, real-but-weaker after skip-1, gone by
  K=5) matches the literature on **transient mechanical price impact and order-book resiliency**,
  not persistent informational alpha: Bouchaud, Gefen, Potters & Wyart (2004) and Eisler, Bouchaud
  & Kockelkoren (2012) propagator models describe exactly this kind of rapid impact decay as
  resting limit orders replenish the book; Cont, Kukanov & Stoikov (2014) "The Price Impact of
  Order Book Events" find order-flow imbalance's predictive power for *future* price changes
  collapses to near-zero within a few trades due to market-maker mean reversion. The decay is
  markedly *faster* than Tóth et al. (2011)'s "square-root law" power-law decay of genuine
  multi-day institutional meta-order impact — consistent with local liquidity-clearing micro-
  bursts, not real persistent institutional positioning.
- In Almgren & Chriss (2001)'s permanent-vs-temporary impact decomposition, the fact that the
  effect falls to ~50% (no information) by K=5 means the initial K=1 displacement is overwhelmingly
  **temporary impact** (liquidity cost) rather than **permanent impact** (information/alpha). The
  absorbed subset is classical liquidity exhaustion (Biais, Hillion & Spatt 1995) — a book-
  depth/resiliency measure, not a directional edge.
- `Y_imb = ΔP/θ` is, structurally, an activity-clock analogue of **Kyle's Lambda** (Kyle 1985) —
  price impact per unit of signed order flow — and maps directly onto this repo's existing
  Amihud illiquidity veto and `liq_fragility` gate (calendar-clock, dollar-volume-based) as a
  higher-frequency, event-clock-native modernization of the same concept.

**Final decision**: **close this out as a Track 2 observation-vector alpha candidate — do not
promote `Y_imb` (or naive momentum on it) into the HMM observation vector.** It lacks the
persistence required of a directional signal, and promoting a construct proven to decay to noise
within 5 bars would contaminate the state space with noise, contrary to this system's own
elite-feature-set discipline. **Retargeted** to the risk-gating initiative as a candidate
activity-clock Kyle's-Lambda / transient-impact liquidity gate — see
`docs/superpowers/specs/2026-09-06-risk-gating-gang-statistical-reformulation-initiative.md` §2.4.
The diagnostics above (duration-binning, skip-1-bar) and the underlying tool
(`tools/observation_vector/imbalance_work_rate_eval.cpp`) remain in the repo as validated,
real-data-tested groundwork for that follow-on use, not wasted effort.
