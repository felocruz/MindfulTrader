# Indicators — Gang-Statistical Reformulation Initiative

**Status, opened 2026-09-04: seeded from a literature-grounding consult on Elder's Force Index
(CLAUDE_BRIEF_127/127_REPLY, `lbrnet/logs/rc_gemini.log`). Scope is deliberately broad — every
named technical *indicator* this system computes and feeds into Screen 1/2/3 timing, `Scoring.cpp`
quality bonuses, or the observation vector (MACD, Force Index, Stochastic, RSI, 3/10 oscillator,
etc.), not a single-indicator effort. Force Index (FI2/FI13) is the first case study (§1). Not yet
actively worked beyond that — no code written, no other indicators triaged yet.**

## 0. Origin and mandate

Sibling of `docs/superpowers/specs/2026-09-04-technical-analysis-gang-statistical-reformulation-
initiative.md` (chart-based **pattern** detection — swing high/low, oscillator divergence as a
*structural* phenomenon, Wyckoff stop-harvest/reversal) but a **distinct scope**: that initiative
asks whether pattern-recognition logic (extremes, divergence-as-disagreement, stop-harvest
sequences) is statistically rigorous. This one asks the same question about the underlying named
**indicators** themselves (Force Index, MACD, Stochastic, RSI, the 3/10 oscillator's own
construction) — is each indicator's *formula* literature-grounded and real-data-validated, the same
bar already applied to every observation-vector dimension in
`docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md`? These two initiatives
will likely cross-reference each other constantly (e.g. Force Index divergence-with-price is both
"an indicator's formula" — this doc's concern — and "a divergence pattern" — the sibling's concern)
but should stay separately tracked since an indicator's formula can be wrong independent of whether
the pattern-detection logic built on top of it is.

## 1. Case study #1 — Elder's Force Index (FI2/FI13) and its divergence with price

### 1.1 Current implementation (baseline, as of 2026-09-04)

```
Force_Index_raw(t) = (Close(t) - Close(t-1)) x Volume(t)
Force_Index_2       = 2-period EMA of Force_Index_raw    (FI2, Screen 2 timing, `src/TripleScreen2.cpp`)
Force_Index_13      = 13-period EMA of Force_Index_raw    (FI13, Screen 1 trend confirmation, `src/TripleScreen1.cpp`)
```

Real load-bearing surface, not a peripheral signal:
- **Screen 2 entry gate** (`knowledge/global/sierra_chart/elder_triple_screen.md`): in an uptrend,
  wait for FI2 to dip below zero (pullback) before Screen 3 looks for a buy-stop entry; symmetric
  for downtrends. `FI2Signal::setFromChart` (`Indicator.cpp`) implements this.
- **Force Index divergence** — Elder's own strongest version of the signal: price makes a new low
  but FI2 makes a shallower low (bullish divergence), or the symmetric top case. Feeds a quality
  bonus in `Scoring.cpp` (MACD/FI divergence bonus) and `MomentumPinball`'s FI2-pullback bonus.
- **Regime-conditional suppression already exists**: `Scoring::IsIndicatorEventSignificant()`
  explicitly suppresses `LONG_FI13_SIGNAL`/`INTERM_FI2_SIGNAL` as "trend-followers which
  false-signal in chop" during high-Shannon-entropy (chaos) regimes — the system already treats FI
  as regime-conditional in reliability, just not yet on a literature-grounded statistical basis.
- Both `FI2`/`FI13` are exported into the observation/training data (`interm_fi2_norm`,
  `long_fi13_norm`) feeding the Transformer.

### 1.2 Literature-grounding findings (CLAUDE_BRIEF_127/127_REPLY/128/128_REPLY, 2026-09-04)

**Key mathematical insight (Gemini)**: on a *fixed-volume* clock, `V_t = C` (a constant per bar by
construction), so `FI_t = ΔP_t x V_t = C x ΔP_t` — Force Index reduces to plain price momentum once
volume is fixed by the clock itself. Elder's volume-weighting is a patch for calendar-time's
heteroskedastic volume, not a fundamental microstructure signal.

**Caveat, not yet resolved (Claude's own read, verifying before this goes further)**: this
system's actual "activity clock" (`ImbalanceBarEngine`) is a fixed-**imbalance** clock
(`|ask_vol - bid_vol|` cumulative crossing a threshold), not a fixed-**volume** clock — total volume
within an imbalance-triggered bar is NOT fixed by construction (two-sided volume that nets to a
small imbalance can accumulate a lot of volume without triggering). So the exact identity
`FI_t = C x ΔP_t` does not literally hold here; the directional claim (volume-weighting carries much
less marginal information on an imbalance/activity clock than on calendar time) is plausible but
**not yet verified against this system's own real imbalance-bar data** — see §3 open questions.

**Divergence, mechanistically**: FI/price divergence is a crude proxy for **order-flow exhaustion /
liquidity absorption** — price extends on progressively lower participation, meaning informed
aggressive flow isn't confirming the move. Literature support:
- Hasbrouck (1991), "Measuring the Information Content of Stock Trades" — canonical grounding for
  why volume-less price moves are statistically transient (uninformed/liquidity-driven, not
  permanent impact).
- Easley, López de Prado & O'Hara (2012), "The Volume Clock: Insights into the High-Frequency
  Paradigm" — VPIN's whole premise (toxicity measured in volume-time) is a rigorous version of what
  FI divergence crudely approximates in calendar time.
- Cont, Kukanov & Stoikov (2014), "The Price Impact of Order Book Events" — Order Flow Imbalance
  (OFI) is the modern, rigorous descendant of the same "signed flow vs. price" idea FI encodes
  crudely via total volume rather than signed (bid/ask) flow.
- Kyle (1985), "Continuous Auctions and Insider Trading" — Kyle's Lambda, the formal ancestor of
  what FI approximates heuristically (price-impact-per-unit-flow).

**Statistical fragility (Gemini)**: raw FI multiplies two heavy-tailed distributions (`ΔP`, `V`) —
functionally infinite-variance in the tails; a single block print coinciding with a gap blows up the
raw value, and EMA smoothing smears that outlier forward for the EMA's whole half-life rather than
correcting it. This is the same class of problem already fixed this session for `burstiness_index`/
`mean_rev_z`/`liq_fragility` (moment-based → robust median/MAD reformulations).

**Candidate reformulation, NOT YET BUILT** (mirrors this system's own already-shipped
`amihud_illiquidity`/`liq_fragility` sqrt-law reformulations, `docs/superpowers/specs/
2026-08-31-elite-feature-set-curation-initiative.md` rows for those dims): `ΔP x √V` instead of
`ΔP x V` (Kyle & Obizhaeva 2016, "Market Microstructure Invariance" — square-root law of price
impact, already the exact citation this repo used for `amihud_illiquidity`'s own 2026-09-03
reformulation, commit `a6d0630`), smoothed via median/MAD rather than EMA. **Status: CANDIDATE** —
literature-grounded, not empirically validated against this system's real tick data, and not
implemented. Elder's Force Index is a live Screen 2 entry-timing gate, not a background risk
signal — changing its formula is a higher-stakes touch than this session's risk-gate/observation-
vector work and should not be done without real-data validation first (same discipline as every
other reformulation this repo has shipped this session).

**Imbalance-clock question RESOLVED, 2026-09-04 (CLAUDE_BRIEF_128/128_REPLY)**: Gemini's original
`V_t = C` claim was checked against this system's actual `ImbalanceBarEngine` (a fixed-imbalance
clock, not fixed-volume) and found not to hold literally — total volume within an imbalance-
triggered bar varies with how much two-sided "fighting" occurred before the threshold tripped.
Gemini, on being corrected, gave a coherent argument for why this actually *rescues* the
volume-weighting concept rather than making it redundant: a "fast" imbalance bar (threshold reached
on thin, one-sided volume) and a "slow" imbalance bar (threshold reached only after heavy two-sided
volume) represent genuinely different liquidity conditions, and `ΔP x √V` on this specific clock is
a plausible way to distinguish "thin sweep" from "thick institutional grind." Gemini was explicit,
appropriately, that **no settled literature (unlike Amihud's case) directly validates this specific
construction on an imbalance clock** — this is a theoretically-motivated hypothesis, not an
established result, and needs empirical validation, not just literature grounding, before treating
it as CANDIDATE-with-confidence.

**Concrete empirical test design (Gemini's proposal, endorsed)**: does `ΔP x √V` (sampled on the
real imbalance clock) contain more forward-predictive information about momentum
continuation/exhaustion than pure `ΔP` alone, sampled on the same clock? If yes, the robustified
Force Index survives as a real candidate; if no (the imbalance clock's own construction already
captures the useful clustering), it should be dropped rather than built. **Not yet run** — this
would be a new offline validation tool in the same family as `tools/observation_vector/
mean_rev_z_variant_comparison.cpp`/`drift_location_eval.cpp` (real tick data, forward-return/
hit-rate methodology, no schema/C++ commitment until validated). This is now the concrete next step
for this case study, not further literature research.

## 2. Case study #2 — MACD (fast/slow EMA discrepancy) and MACD-Histogram divergence

**Founding pattern match (operator, 2026-09-04)**: same conceptual shape already named in the
sibling TA-patterns initiative's own §0 founding observation ("MACD, the 3/10 oscillator,
Bollinger/Keltner bands... structurally all the same move") — recognizing this for MACD
specifically, not a new discovery, extending an already-correct instinct.

### 2.1 Current implementation (baseline, as of 2026-09-04)

```
MACD_line      = EMA(fast) - EMA(slow)              (standard: 12, 26)
MACD_histogram = MACD_line - EMA(MACD_line, 9)
```

Even more deeply embedded than Force Index: Screen 1's **entire trend-direction determination**
comes from MACD histogram slope (`MacdEnum`: `AT_ZERO`/`BULLISH_CROSS`/`BEARISH_CROSS`/four
"seasons" `SUMMER`/`FALL`/`WINTER`/`SPRING` mapping slope+sign combinations). A dedicated
`MACDDivergenceEnum` state machine (`include/Indicator.h`, `include/StudyHelperFunctions.h`'s
`MACDDivergenceState`) tracks peaks/troughs for both `LONG_MACD_DIVERGENCE` (Screen 1) and
`INTERM_MACD_DIVERGENCE` (Screen 2) — a genuinely sophisticated, hand-coded divergence tracker, not
a simple threshold check.

### 2.2 Literature-grounding findings (2026-09-04, Claude's own analysis, Gemini consult pending)

**Structurally different from Force Index**: MACD is pure price (no volume multiplication), so the
"multiplying two heavy-tailed distributions" fragility that hit Force Index doesn't apply the same
way. MACD's real weakness: a linear (non-robust), fixed-time-constant (12/26/9) two-pole band-pass
filter comparing two arbitrary scales, with hand-coded peak/trough divergence detection.

**Sharpest Gang connection identified so far**: MACD's core question — "does the fast-scale trend
agree with the slow-scale trend" — is *structurally identical* to what Hurst/DFA scaling analysis
(already this system's own toolkit, `hurst_exponent`/`fast_hurst_exponent`) answers rigorously via
log-log regression across many window sizes, not two arbitrary fixed periods. This system also
already shipped MACD's **volatility-domain twin** this session: `log_scale_ratio`/
`log_scale_expansion_ratio` (`log(short_BV/long_BV)`, bipower-variation-based, activity-clock-ready,
GPD-recalibrated 2026-09-04) is structurally the same "compare two window scales" move MACD makes,
just already reformulated rigorously for volatility. MACD's **trend-domain** twin — a robust,
multi-window drift/momentum discrepancy — does not yet exist anywhere in this system's Gang
toolkit. Unlike Force Index (redundant with Amihud/OFI concepts already present), this is
genuinely novel territory for this system, not a fix to something already covered.

**Candidate literature** (not yet Gemini-verified — see §2.3):
- Peters, E. (1994), *Fractal Market Analysis* — explicitly reframes moving-average-crossover
  systems (which MACD literally is) through a Hurst/R-S-analysis lens; the most direct hit found.
- Mandelbrot, B., Fisher, A. & Calvet, L. (1997), "A Multifractal Model of Asset Returns."
- Gençay, R., Selçuk, F. & Whitcher, B. (2001), *An Introduction to Wavelets and Other Filtering
  Methods in Finance and Economics* — MACD as a crude two-pole filter vs. proper wavelet/
  multiresolution decomposition.
- Same standing activity-clock citations (Mandelbrot & Taylor 1967; Clark 1973; Ané & Geman 2000).

**Divergence-as-pattern overlap**: MACD-Histogram divergence is conceptually the same "oscillator
disagrees with price at an extreme" pattern already opened as case study #1 in the sibling
`2026-09-04-technical-analysis-gang-statistical-reformulation-initiative.md` (3/10 oscillator
divergence) — cross-reference there, do not duplicate the pattern-detection angle; this doc's own
concern is the underlying indicator formula, same split as Force Index's §1.3.

### 2.3 Gemini consult

**Status: QUEUED, not yet sent.** Follow the same CLAUDE_BRIEF pattern as Force Index
(CLAUDE_BRIEF_127-128) — get an independent read on (1) whether Peters (1994)'s MA-crossover/Hurst
framing is as directly applicable as it looks, (2) whether the multifractal/wavelet citations above
are the right ones or whether better literature exists, (3) whether MACD deserves the same
"redundant once you fix the underlying question properly" verdict Force Index got, or whether its
trend-domain question is genuinely distinct enough from `log_scale_ratio`'s volatility-domain
question to be worth building as its own construct.

## 3. Status vocabulary

Same as the sibling ledgers, for consistency: **IN** (settled, stays as-is) · **IN-WEAK** (stays,
weak, no better alternative identified) · **IN-PENDING-FIX** (stays, a decided implementation change
not yet done) · **PAUSED** (groundwork exists, explicitly do not proceed without new evidence) ·
**CANDIDATE** (proposed, not yet built) · **OPEN** (actively being investigated) · **BLOCKED** (real
work identified, blocked on something else finishing first).

Force Index (§1): **CANDIDATE**. MACD (§2): **OPEN** (literature consult queued).

## 4. Open questions (Phase 0, not started)

1. **RESOLVED (conceptually) 2026-09-04, empirical test not yet run**: does this system's real
   imbalance-bar data actually show `ΔP x √V` carries more forward-predictive information about
   momentum continuation/exhaustion than pure `ΔP` alone? Literature grounding says the imbalance
   clock's non-fixed volume plausibly preserves real signal (see §1.2's resolution), but no settled
   literature validates the specific construction — needs the concrete offline test §1.2 describes
   (same family as `mean_rev_z_variant_comparison.cpp`/`drift_location_eval.cpp`), not further
   literature search.
2. If `ΔP x √V` (median/MAD) is built, does it replace FI2/FI13 in place (train/serve parity risk
   for the live Screen 2 gate and the Transformer's `interm_fi2_norm`/`long_fi13_norm` inputs) or
   ship as a new, additive signal first? Not yet decided — same category of question as the sibling
   TA initiative's own open question 4.
3. Is Force Index divergence-with-price (as a *pattern*, not the indicator's raw formula) already
   in scope of the sibling `2026-09-04-technical-analysis-gang-statistical-reformulation-
   initiative.md`'s oscillator-divergence case study, or does it need its own entry there once this
   doc's formula-level work is further along? Cross-reference, don't duplicate.
4. Other indicators not yet triaged for this initiative: Stochastic (5,3,3)/(14,3,3), RSI, the 3/10
   oscillator's own fast/slow construction (already partially covered by the sibling initiative's
   case study #1, but from the *pattern* angle, not the indicator-formula angle) — none
   investigated yet.
5. MACD-specific (§2): is Gemini's literature read going to confirm Peters (1994)/multifractal
   framing, or point somewhere else entirely? Queued, not yet sent.
6. MACD-specific: if a robust multi-window trend-discrepancy construct is built, does it replace
   MACD in place (Screen 1's entire trend-direction mechanism — a much higher-stakes touch than
   Force Index's Screen 2 role) or ship additively first? Not yet decided — likely an even more
   conservative answer than Force Index's own open question 2, given Screen 1 is upstream of
   everything else in the Triple Screen hierarchy.

## 5. References

- Elder, A. (1993), *Trading for a Living* — Force Index's original definition and divergence rule,
  and MACD-Histogram divergence (case study #2).
- Hasbrouck, J. (1991), "Measuring the Information Content of Stock Trades," *Journal of Finance*.
- Kyle, A.S. (1985), "Continuous Auctions and Insider Trading," *Econometrica*.
- Kyle, A.S. & Obizhaeva, A.A. (2016), "Market Microstructure Invariance," *Econometrica* — already
  used in this repo for `amihud_illiquidity`/`liq_fragility` (2026-09-03).
- Cont, R., Kukanov, A. & Stoikov, S. (2014), "The Price Impact of Order Book Events," *Journal of
  Financial Econometrics*.
- Easley, D., López de Prado, M. & O'Hara, M. (2012), "The Volume Clock: Insights into the
  High-Frequency Paradigm," *Journal of Portfolio Management*.
- Mandelbrot, B. & Taylor, H. (1967); Clark, P. (1973); Ané, T. & Geman, H. (2000) — subordinated
  stochastic processes / activity-clock justification, already this repo's standing citation for
  every other activity-clock dim (`fast_hurst_exponent`, `fast_taleb_kurtosis`, `fast_mean_rev_z`).
- Peters, E. (1994), *Fractal Market Analysis* — moving-average-crossover systems via Hurst/R-S
  analysis (case study #2, not yet Gemini-verified).
- Mandelbrot, B., Fisher, A. & Calvet, L. (1997), "A Multifractal Model of Asset Returns" (case
  study #2, not yet Gemini-verified).
- Gençay, R., Selçuk, F. & Whitcher, B. (2001), *An Introduction to Wavelets and Other Filtering
  Methods in Finance and Economics* (case study #2, not yet Gemini-verified).
- `lbrnet/logs/rc_gemini.log` CLAUDE_BRIEF_127/127_REPLY/128/128_REPLY — Force Index consult
  transcript.
