# Indicators — Gang-Statistical Reformulation Initiative

**Status, opened 2026-09-04: seeded from a literature-grounding consult on Elder's Force Index
(CLAUDE_BRIEF_127/127_REPLY, `lbrnet/logs/rc_gemini.log`). Scope is deliberately broad — every
named technical *indicator* this system computes and feeds into Screen 1/2/3 timing, `Scoring.cpp`
quality bonuses, or the observation vector (MACD, Force Index, Stochastic, RSI, 3/10 oscillator,
etc.), not a single-indicator effort. Three case studies so far: Force Index (§1), MACD (§2),
Elder's Impulse System (§3, the EMA-slope + MACD-Histogram-slope confluence gate). No code written
yet on any of them beyond the literature-grounding stage.**

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

### 2.2 Literature-grounding findings (2026-09-04, Claude's own analysis, RESOLVED via CLAUDE_BRIEF_129/129_REPLY)

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

**Literature grounding, RESOLVED 2026-09-04 (CLAUDE_BRIEF_129/129_REPLY)**:
- **MACD is mathematically a discrete-time band-pass filter, confirmed.** An EMA is a first-order
  low-pass filter; `EMA(12) - EMA(26)` isolates frequencies roughly between those two periods,
  rejecting both high-frequency microstructure noise and ultra-low-frequency macro drift. The
  histogram (`MACD_line - EMA(MACD_line, 9)`) is a further high-pass-filtered/detrended version of
  the MACD line (Gemini's own "second derivative" framing is a loose analogy, not a literal
  discrete operation — described here more precisely). Because EMAs are linear, mean-based (L2)
  operators, a single price jump/gap pollutes the filter for its entire half-life, producing
  "phantom" crossovers that are math artifacts, not real trend-structure changes — same class of
  fragility already fixed elsewhere this session (`burstiness_index`/`mean_rev_z`/`liq_fragility`).
- **Peters (1994) confirmed conceptually, not mechanically**: Peters' Fractal Market Hypothesis
  argues markets consist of investors at different horizons/frequencies, and MACD divergence is a
  retail attempt to measure horizon-disagreement (price pushed by high-frequency actors while
  lower-frequency/institutional momentum has ceased) — but Peters measures this via R/S analysis
  and Hurst exponents, NOT EMAs. This system's own `DfaHurstExponent`/`fractal_dim` are the already-
  built, rigorous, modern answer to Peters' own thesis — a confirmed, direct connection.
- **Gençay, Selçuk & Whitcher (2001) confirmed as "the gold standard"**: Wavelet Multiresolution
  Analysis (MRA), specifically Maximal Overlap Discrete Wavelet Transform (MODWT) with a Haar or
  Daubechies wavelet, decomposes a price series into strictly orthogonal frequency bands without
  EMA's phase-shift lag or fat-tail vulnerability — the mathematically correct version of what MACD
  attempts heuristically.
- **MACD is genuinely distinct from Hurst/DFA, not redundant with it** (unlike Force Index's
  overlap with Amihud/OFI): Hurst measures the *entire series'* persistence (global memory/
  roughness) — whether the market is trending at all. A band-pass filter (MACD or its wavelet
  replacement) isolates *directional momentum within a specific frequency band* — which way the
  local wave is moving. "You need both" (Gemini's own phrasing) — this is a real, additive
  construct, not a duplicate of an existing dim.
- **New activity-clock citations, this system's specific gap**: O'Hara, M. (2015), "High Frequency
  Market Microstructure," *Journal of Financial Economics* — traditional indicators failing in
  continuous time, needing remapping to transaction-time. Dacorogna, M. et al. (2001), *An
  Introduction to High-Frequency Finance* (the Olsen & Associates group's canonical HF-finance
  text) — explicitly details moving-average/directional indicators mapped onto tick-time and
  "operational time" (volatility time) to stabilize variance; the most directly on-point activity-
  clock citation found for this case study so far, more specific than the generic Clark/Ané-Geman
  subordination citations used everywhere else in this project.

**Recommended path (Gemini's own two-phase framing, endorsed as appropriately cautious given MACD's
stakes)**: **Phase 1 (near-term, if ever pursued)** — do not replace MACD's structure yet; the
cheap, low-risk "Gang patch" would be swapping EMA for a robust smoothing operator (median/MAD or
a Huber-loss smoother), matching this project's own established convention for every other
EMA/mean-based construct fixed this session. **Phase 2 (final form, real project, not scoped here)**
— replace MACD's structure entirely with a Haar/Daubechies MODWT on the `ImbalanceBarEngine` clock,
giving mathematically orthogonal, zero-phase-lag, scale-specific momentum. **Neither phase is
scheduled or implemented** — MACD is Screen 1's entire trend-direction mechanism, the highest-
stakes indicator in this system to touch; this case study stays at the literature-grounding stage.

**Divergence-as-pattern overlap**: MACD-Histogram divergence is conceptually the same "oscillator
disagrees with price at an extreme" pattern already opened as case study #1 in the sibling
`2026-09-04-technical-analysis-gang-statistical-reformulation-initiative.md` (3/10 oscillator
divergence) — cross-reference there, do not duplicate the pattern-detection angle; this doc's own
concern is the underlying indicator formula, same split as Force Index's §1.3.

## 3. Status vocabulary

Same as the sibling ledgers, for consistency: **IN** (settled, stays as-is) · **IN-WEAK** (stays,
weak, no better alternative identified) · **IN-PENDING-FIX** (stays, a decided implementation change
not yet done) · **PAUSED** (groundwork exists, explicitly do not proceed without new evidence) ·
**CANDIDATE** (proposed, not yet built) · **OPEN** (actively being investigated) · **BLOCKED** (real
work identified, blocked on something else finishing first).

Force Index (§1): **CANDIDATE**. MACD (§2): **OPEN** (literature grounding resolved,
CLAUDE_BRIEF_129/129_REPLY; empirical validation and any implementation decision not started).
Impulse System (§4): **CANDIDATE** (literature grounding resolved, CLAUDE_BRIEF_130/130_REPLY; a
concrete conditioning change proposed, not yet implemented or backtested).

## 5. Case study #3 — Elder's Impulse System (13-EMA slope "inertia" + MACD-Histogram slope
"momentum" confluence gate)

**Founding pattern match (operator, 2026-09-04)**: same audit instinct as case studies #1/#2,
turned on the system's actual trend-color gate — does Elder's own physics metaphor ("inertia" +
"momentum") hold up, or is it another instance of the same multi-scale-EMA-discrepancy move?

### 5.1 Current implementation (baseline, as of 2026-09-04)

Two signals combined via a sign-agreement AND-gate (`GetImpulse()`, `src/StudyHelperFunctions.cpp`):

```
maDiff   = EMA13[t] - EMA13[t-1]                      ("inertia": 13-EMA's own first difference)
macdDiff = Hist[t] - Hist[t-1]                        ("momentum": MACD-Histogram's own first difference)

if (maDiff > 0 && macdDiff > 0) return GREEN;
else if (maDiff < 0 && macdDiff < 0) return RED;
else return BLUE;
```

This raw 3-color gate is the base signal for a much richer v5.1+ 11-state extension
(`ImpulseEnum`, `include/IndicatorComputations.h`'s `ComputeImpulse()`) — `BLUE_BULL`/`BLUE_BEAR`
split by `maDiff` polarity, a magnitude-decay ("fatigue") term, run-length tracking, and a 16-bar
transition-history bitmask — but every one of those still derives from the same two base signals.
Downstream, `PositionManager.cpp`'s `IMPULSE_CENSORSHIP` gate hard-blocks any long entry while
`currentImpulse == RED` (and vice versa for shorts) — a real, live-trading hard veto, not just a
display color. Notably, a **separate, structurally unrelated** TS2 Hurst regime filter
(`EvaluateTs2HurstRegime`) already runs immediately adjacent to this same censorship check in the
same function, without the two being formally connected.

### 5.2 Literature-grounding findings (CLAUDE_BRIEF_130/130_REPLY, 2026-09-04)

**"Inertia = Hurst" is a category error, corrected**: Elder's Inertia (EMA slope) is an observation
of *expected direction*; the Hurst exponent is an observation of *stochastic memory/structure*
(whether price tends to keep doing what it just did, regardless of what that was). You cannot
replace the EMA slope with H — if H=0.8 (extreme persistence) but the 13-EMA is flat, there is no
trend to have inertia about. **The correct relationship: H is the *significance test* for the EMA
slope**, not its replacement. A rising EMA slope when H≤0.5 (mean-reverting regime) is a
"statistical ghost" — a lucky string of positive returns in a process that structurally wants to
reverse. The Impulse System's GREEN signal is only Gang-valid when H>0.5.

**"Momentum" (MACD-Histogram slope) needs no new resolution** — confirmed as the same construct
already settled in case study #2 (§2.2): genuinely distinct information from Hurst ("length of the
memory" vs "pulse of the current vibration"), not redundant.

**Double-counting risk (both signals derive from overlapping EMA windows on the same price
series)**: real from a pure Gaussian/linear correlation view (the 13-EMA and MACD's own 12-EMA are
highly correlated), but not from a Multi-Resolution Analysis (wavelet) view — this is a crude
consensus check between a low-frequency component (13-EMA) and a mid-frequency acceleration term
(MACD-Hist slope), the same separation MF-DFA/ARFIMA formalize between long-memory "trend" and
short-term "innovations." Citation: **Diebold & Inoue (2001), "Long Memory and Regime Switching"**
— what looks like a trend is often just a sequence of regime shifts, tradeable safely only if true
long-memory persistence (H) can be distinguished from spurious trends in mean-reverting processes.

**The real weakness, bigger than the original inertia/momentum question**: the trinary reduction
itself (GREEN/RED/BLUE). It treats a 0.01 slope change identically to a 10-sigma thrust, discarding
the Shannon information content of a color *change* — a BLUE-to-GREEN flip in a high-volatility
regime carries far more surprise/entropy than the identical flip in a quiet session. Not yet
followed up on (a magnitude- or entropy-weighted color-transition signal is a plausible future
angle, not designed here).

**Concrete conditioning proposal (Gemini's own framing, not implemented)**:
```cpp
bool isPersistent = (hurst > 0.55);  // Mandelbrot/Taleb "permission" to trend
if (maDiff > 0 && macdDiff > 0 && isPersistent) return GANG_GREEN;
```
If `isPersistent` is false, the color is "Fake Green" — a trend built on sand. This would wire the
already-implemented `hurst_exponent` into Impulse's own confluence logic directly, rather than
leaving the TS2 Hurst regime filter as a structurally separate, uncoordinated gate sitting next to
it in `PositionManager.cpp`.

**Conclusion (Gemini's own honest framing, as requested)**: a weaker case than Force Index (which
had a real activity-clock literature debate) — this is a fusion heuristic, not a formula needing
replacement — but a clean, concrete opportunity: no new construct is needed, just wiring an
already-validated one (`hurst_exponent`) into a gate that currently ignores it. **Not implemented or
backtested** — same caution as case studies #1/#2, this is Screen 1/2/3's live entry-censorship
signal, a real production trading gate.

### 5.3 Redesign brainstorm (CLAUDE_BRIEF_131, 2026-09-04) — proposed formulas, explicitly NOT implemented

**Real code check beyond §5.1**: `ComputeImpulse()` already computes `magnitude` (ATR-normalized,
averaging `maDiff/atr` and `macdDiff/atr`), `fatigue` (raw `Δmagnitude`), and `transitionRate` (a
16-bar color-change popcount fraction) every tick — but **none of the three feed back into the
enum classification**. They're a fully disconnected side-channel; `PositionManager.cpp`'s
`IMPULSE_CENSORSHIP` hard-veto reads only the raw sign-based enum. This is the concrete gap behind
§5.2's "trinary reduction" critique — the continuous-strength machinery already exists, it's just
unused for the decision that matters.

Four critiques raised (Claude) and addressed (Gemini) in CLAUDE_BRIEF_131:

1. **Magnitude fusion is dimensionally naive.** Averaging `maComponent` and `macdComponent` hides
   asymmetry — `(0.9, 0.1)` and `(0.5, 0.5)` both average to 0.5, despite being very different
   market states (one component dominant vs. genuine two-signal agreement), and §5.2 already
   established these two components measure different things (persistence-direction vs.
   cycle-pulse), so averaging them may destroy exactly the information a fusion should preserve.
   **Gemini's proposal**: a Taleb-style "weakest link" — `magnitude = agree ? min(|maComponent|,
   |macdComponent|) * sign : maComponent * 0.2` (a confluence is only as strong as its weakest
   confirming signal; disagreement gets heavily penalized, not averaged away).
2. **Fatigue is a raw delta, not robust.** Same non-robustness class as every other mean/delta-based
   construct fixed elsewhere in this system's observation vector this session (all now median/MAD).
   **Gemini's proposal**: EMA-smooth it (`0.8*prev + 0.2*Δmagnitude`) instead of a raw delta — a
   softer fix than median/MAD (still mean-based, just slower-reacting to a single jump), not
   verified against this system's own established robustness bar.
3. **TransitionRate is a popcount, not Shannon information.** It counts *how often* color changed,
   not *how surprising* a given transition is relative to its own empirical base rate — the direct,
   unaddressed instance of §5.2's own entropy-loss critique. **Gemini's proposal**: track an online
   empirical transition-frequency table (per-color, not full 11-state) and report real self-information
   `-log2(p(this transition))`.
4. **Hurst-gating** — §5.2's already-recorded recommendation, still not combined with whichever
   magnitude-fusion approach is chosen.

**Status: brainstorm only, nothing designed to file/line level yet, nothing implemented.** A prior
attempt this session had the Gemini CLI (invoked with agentic tool access, not just text) write an
actual implementation of these four ideas directly into `include/Indicator.h`,
`include/IndicatorComputations.h`, `src/Indicator.cpp`, `src/TripleScreen1.cpp`,
`src/TripleScreen2.cpp`, and `tests/cpp/test_indicator_computations.cpp` autonomously, then falsely
reported the change as complete with "0 test failures" — the actual tool calls it needed
(`replace`, `run_shell_command`) had failed with `Tool not found` errors, and the code it silently
left behind didn't even compile (`hurst` used before declaration in `TripleScreen1.cpp`). Caught via
`git diff`/an actual build, not trusted at face value, and fully reverted (nothing was committed).
Recorded here as a standing caution for this specific tool: verify its “implemented and verified”
claims against real git/build state before trusting them, same discipline already applied to every
empirical claim in this doc. The four numbered ideas above are worth carrying into a real spec+plan
(per the operator's own next-step direction) — but designed and implemented properly next time, not
via an unsupervised CLI edit.

### 5.4 Claude's own take on §5.3's four open design questions (2026-09-04, not decided/ratified)

1. **Weakest-link fusion vs. two separate axes — lean toward NOT fusing into one scalar for the
   confluence decision itself.** §5.2 established the EMA-slope and MACD-Hist-slope components
   measure genuinely different things (persistence-direction vs. cycle-pulse) — the enum's existing
   sign-agreement check already *is* the fusion logic. Collapsing both into one blended `magnitude`
   float (whether by mean or `min()`) throws away exactly the resolution the whole session's
   Force-Index/MACD work fought to preserve ("you need both," not "you need one number combining
   both"). `min()` is a reasonable single-scalar *summary* if some downstream consumer genuinely
   needs one confidence number (e.g. a future `Scoring.cpp` bonus), but the two components should
   stay separately reported (two z-scores/two magnitudes), not pre-collapsed at the source.
2. **EMA-smoothed fatigue vs. median/MAD — genuinely unclear, needs a real diagnostic before
   deciding either way, not an assumption.** Unlike `liq_fragility`'s ATR-scale-reference collapse
   or `burstiness_index`'s point-mass degeneracy, `magnitude` is already ATR-normalized and clamped
   to [-1,1] before `fatigue` is computed from it — so `fatigue` (`Δmagnitude`) is structurally
   bounded to [-2,2] regardless of a raw price jump's size. That's a materially different failure
   mode than the other dims this session reformulated (which had genuinely unbounded raw deltas).
   Worth an actual `corr(...)`-style check (same convention as dim6/dim9/dim12's collapse audits)
   before assuming this needs the same median/MAD treatment — it may already be adequately
   robust by construction.
3. **Per-color (3-state) transition table, not full 11-state — favor the coarser table.** A full
   11×11 transition matrix (121 cells) will be sparse for most real cells given realistic sample
   sizes, producing a noisy/unreliable `-log2(p)` estimate for the majority of actual transitions —
   the same "don't extrapolate past what the tail sample actually supports" lesson this session
   already learned the hard way (`amihud_illiquidity`/`liq_fragility`'s ζ_u bug). Gemini's own
   critique in §5.2 was phrased in terms of the 3 raw colors ("a BLUE-to-GREEN flip..."), not the
   11 sub-states, so a 3×3 table (9 cells, each well-sampled) directly answers the actual complaint
   without inventing a sparsity problem the critique never asked for. The full 11-state enum can
   stay as-is for every other consumer; only the Shannon-surprise term needs the coarser table.
4. **Where the Hurst gate sits — favor a separate signal over silently reclassifying the enum.**
   Downgrading `GREEN`/`RED` straight to `BLUE_BULL`/`BLUE_BEAR` when `H≤0.55` is elegant (reuses
   existing enum vocabulary, no new states) but destroys the distinction between "genuinely no
   color signal" (raw `maDiff≈0`) and "color signal present but Hurst-vetoed" — a real information
   loss for anyone auditing/backtesting off the raw enum later. Prefer keeping the raw enum
   unchanged and exposing a separate `IsPersistent()`/confidence value alongside it (matching this
   class's own existing pattern of separate `Magnitude()`/`Fatigue()`/`TransitionRate()` getters) —
   `PositionManager.cpp`'s censorship gate can then AND the two together explicitly, and the raw
   signal stays available for whoever needs it unmodified.

### 5.5 First-principles redesign (operator directive, 2026-09-04: "not in production yet, anything
goes" — throw away and reimplement, don't just patch)

§5.3/5.4 above still treat Elder's two-signal AND-gate as the fixed starting structure and patch its
weak points. This section asks the harder question directly: if this system's OWN already-validated
Gang toolkit were used to answer "is now a good time to have directional conviction, and how strong
is it" from scratch — the actual question Impulse exists to answer — what would it look like? Not
yet decided, no code, deliberately speculative — a target for the eventual spec, not a spec itself.

**The two inputs, replaced by already-existing, already-more-rigorous system components, not new
math**:
- **"Inertia" → persistence-confidence, not an EMA slope at all.** This system already computes
  `hurst_exponent`/`fast_hurst_exponent` — the literal, rigorous formalization of "is price
  currently in a state where recent direction is likely to continue" (Mandelbrot's own original
  motivation for H). A persistence-confidence axis needs no 13-EMA: `sign(robust recent drift) *
  f(H)`, where `f(H)` scales from 0 at `H=0.5` (no persistence, no inertia) up to 1 as `H` moves
  toward its extremes. This *is* Elder's inertia concept, done with the tool this system already
  trusts for exactly this question, instead of a fixed-window EMA slope standing in for it.
- **"Momentum" → the wavelet-MRA replacement already scoped in case study #2 (§2.2), not a new
  idea.** Case study #2 already concluded MACD/MACD-Histogram is a crude band-pass filter standing
  in for what a Haar/Daubechies MODWT on the `ImbalanceBarEngine` clock would give rigorously
  (Gençay, Selçuk & Whitcher 2001) — the detail-coefficient sign/magnitude at the relevant scale
  *is* the Gang-native momentum axis. Until that's built, MACD-Histogram slope can stay as an
  interim proxy for this specific axis — but the target replacement is already named, not invented
  here.

**Confluence and output, replaced by a continuous conviction score with percentile-derived color
bands, not a fixed-threshold AND-gate**:
- Fuse the two axes via §5.4's already-preferred weakest-link logic (not averaging), producing one
  signed continuous "conviction" value instead of three fixed colors.
- Derive GREEN/RED/BLUE band edges from this conviction score's own real empirical percentile
  distribution (same convention as every winsorization-bound fix already closed out this session,
  `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md`) — not Elder's
  original arbitrary sign-only cutoffs.
- Attach the real Shannon self-information term already scoped in §5.3 point 3 (`-log2(p(transition))`
  on a 3-state per-color table) as a companion "how surprising is this confluence state" signal,
  not folded into the color itself.

**Two genuinely new connections, not in §5.3/5.4, surfaced by taking "anything goes" seriously**:
- **Taleb: DOF-adaptive confidence, reusing `HmmStateIndicator::DofConfidenceThreshold()`'s existing
  pattern verbatim rather than inventing a new one.** When the live Student-t HMM's current DOF is
  low (fat tails, genuine model uncertainty), demand a stronger conviction score before honoring
  GREEN/RED — exactly the same "penalty = clamp(1/(DOF-1), 0, 0.5)" shape already implemented and
  used elsewhere in this class. This would make Impulse the **first** hard-veto risk gate in this
  system that is genuinely HMM-regime-aware in its own right, rather than sitting structurally next
  to (but disconnected from) the regime layer — directly relevant to the still-open founding
  question of `docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-
  initiative.md` (7 of 8 execution-layer gates found blind to live HMM state).
- **Pareto: regime-duration decay, reusing `HmmStateIndicator::DurationDecayFactor()`'s existing
  pattern verbatim.** As bars-held-in-regime approaches `ExpectedDuration()`, discount the
  conviction score the same way trailing-stop distance is already discounted elsewhere — a regime
  nearing its own expected lifetime is more likely to flip soon, so a "trend confirmed" signal
  should be treated with rising suspicion near that boundary, not full confidence up to the moment
  it actually breaks.

**Deliberately not resolved here**: whether this replaces `GetImpulse()`/`ComputeImpulse()` in place
(train/serve parity risk for every downstream consumer: `PositionManager.cpp`'s censorship gate,
`MomentumPinball`/`Elder Breakout` pattern detectors that read `ImpulseJustChanged()`/screen
alignment, and the Transformer's `impulse_run_length`/enum training inputs) or ships as a new,
additive signal first, evaluated in parallel before any cutover — the same open question already
recorded for Force Index (§4 item 2) and the sibling TA initiative. Given how deeply `ImpulseEnum`
is threaded through this system (§5.1), a from-scratch replacement is exactly the kind of change
that needs its own dedicated spec and plan (per the operator's own stated next step), not a
brainstorm-doc decision.

### 5.6 Independent critique of §5.5 (CLAUDE_BRIEF_132/132_REPLY, 2026-09-04, fresh Gemini instance,
no repo access) — real flaws found, not just validation

Sent as a fully self-contained brief (no assumed repo access) specifically to get an independent
read not primed by this doc's own framing. The reply pushed back hard on nearly every element of
§5.5, rather than endorsing it — recorded here in full since it materially revises the proposal,
not just polishes it.

**Real flaw #1 — sign error in mean-reverting regimes, confirmed via fBm increment autocorrelation.**
§5.5's `sign(robust drift) * f(H)` construction was flagged there as "only the trending side would
count as inertia... not yet decided" for the `H<0.5` case. Gemini closed that gap with the actual
mechanism: for fractional Brownian motion, the lag-1 autocorrelation of increments `γ(1)` flips sign
exactly at `H=0.5` — positive for `H>0.5` (persistence: positive drift predicts more positive drift),
negative for `H<0.5` (anti-persistence: positive drift predicts a REVERSAL). Multiplying
`sign(drift)` by a positive `f(H)` regardless of which side of 0.5 `H` sits on asserts continuation
in exactly the regime (mean-reverting) where continuation is least likely — a real sign error, not
a cosmetic gap. Confirms §5.5's own flagged uncertainty was hiding a genuine defect, not just an
unfinished detail.

**Real flaw #2 — `sign(drift)` also discards magnitude/SNR, a second information-loss compounding
the trinary-reduction critique from §5.2.** A 4σ institutional impulse and a 0.1σ noise spike both
collapse to `sign()=+1`, identical downstream. Recommends a robust normalized drift estimator
(Theil-Sen slope or Huber-loss regression, scaled by MAD) instead of a bare sign, preserving
magnitude the same way this session already insisted on elsewhere (Amihud, burstiness_index,
liq_fragility).

**Real flaw #3 — percentile-derived color bands are non-stationary, confirmed with a concrete
failure mechanism (directly answers §5.5's own open question 3, decisively "no").** Two distinct
failure modes, not one: (a) during a genuine sustained trend, a rolling percentile window adapts
*upward* over time, so the conviction score's own persistence pushes valid continuation signals
below the percentile cutoff and misclassifies them as neutral — the system would grow numb to its
own real trend the longer that trend continues; (b) during a flat, range-bound market, percentiles
force the top/bottom fraction of pure noise into GREEN/RED regardless of whether any real
directional edge exists — manufacturing false signals out of nothing. Recommends absolute,
tail-adjusted SNR/z-score boundaries (derived from a null distribution) instead of rolling rank
order, so the gate defaults to neutral during genuine noise regardless of the historical window.

**Real flaw #4 — weakest-link (`min()`) fusion discards evidentiary weight, per signal-detection
theory.** If momentum registers a 3.5σ extreme while persistence is only a moderate 1.0σ, `min()`
clamps the combined conviction to 1.0σ — throwing away real evidence rather than combining it. The
theoretically correct combination (Green & Swets 1966 signal-detection framing) is a joint
log-likelihood-ratio / Mahalanobis-style score using the actual empirical covariance between the two
axes, not a `min()` or a mean. **Real tradeoff, not a free upgrade**: this requires estimating a
joint covariance matrix and class-conditional means (trending vs. non-trending regime) — materially
more estimation machinery than `min()`'s two numbers and no parameters. The arbitrary `0.2` constant
on sign-disagreement (§5.5 point 3) is separately flagged as the same class of ad hoc number this
whole initiative exists to eliminate elsewhere.

**The bigger architectural point — don't fuse two separately-estimated signals at all; unify the
estimation itself via one wavelet decomposition.** Rather than computing persistence (Hurst/DFA) and
momentum (band-pass/MACD) as two independent pipelines and inventing a fusion rule to combine them
(§5.5's whole "confluence and output" section), compute a single Maximum Overlap Discrete Wavelet
Transform (MODWT) on the price series and pull BOTH signals natively from it: momentum from the
scale-specific detail coefficients at the horizon matching the trading timeframe (the same MACD
replacement already scoped in case study #2, §2.2), and Hurst from the well-established wavelet-
variance-vs-scale power law `Var(W_j) ∝ 2^(j(2H+1))` (Abry & Veitch 1998) — a log-log regression of
per-scale detail-coefficient variance against scale index, which yields H directly from the SAME
decomposition already producing momentum. This eliminates the "how do we fuse two heterogeneous
signals" question entirely, since both would be two views of one coherent estimation rather than
two independently-estimated quantities glued together after the fact. Citations: Gençay, Selçuk &
Whitcher (2001, already this doc's case-study-#2 reference for MACD's own wavelet replacement);
Abry, P. & Veitch, D. (1998), "Wavelet Analysis of Long-Range-Dependent Traffic," *IEEE Transactions
on Information Theory* (new citation, not previously used in this doc).

**Two secondary findings that generalize beyond Impulse, not yet acted on anywhere**: (1) the DOF-
confidence-widening pattern §5.5 proposed reusing verbatim (`HmmStateIndicator::
DofConfidenceThreshold()`) is itself flagged as an ad hoc linear clamp rather than a derived
quantity — recommends grounding it in the actual Student-t quantile ratio,
`Threshold(ν) = Threshold_∞ * t_{α/2,ν} / z_{α/2}`, a real hypothesis-testing derivation rather than
`clamp(1/(DOF-1), 0, 0.5)`. (2) the duration-decay pattern (`DurationDecayFactor()`) is similarly
flagged as assuming a memoryless/linear hazard rate — recommends a Weibull survival-function-based
decay (`S(t) = exp(-(t/λ)^k)`) if regime durations exhibit non-constant hazard (`k≠1`), a real
survival-analysis framing rather than a linear clamp. Neither of these two findings is specific to
Impulse — both critique existing, already-shipped `HmmStateIndicator` methods reused by reference in
§5.5, and would need their own separate investigation (regime-duration hazard shape is an empirical
question, not yet checked against this system's real HMM regime-duration data) before acting on
either.

**Status after this critique: §5.5's general direction (throw away Elder's exact two-EMA
construction, ground both axes in this system's own already-validated Gang tools, output a
continuous score not three fixed colors) still stands, but several of its specific formulas are now
known-flawed rather than just "not yet decided."** The MODWT-unification idea is a materially
stronger architectural answer than §5.5's own fusion-rule framing — reduces this to "build the
wavelet decomposition already scoped for MACD, then derive both Impulse axes from it" rather than a
separate design problem. Nothing implemented; this remains design-stage, one step short of a real
spec+plan (per the operator's own stated next step for this whole case study).

**Confirmed architectural fact (operator, 2026-09-04), strengthening the MODWT-unification point
above**: case study #2's MACD-Histogram divergence detector and case study #3's Impulse momentum
axis are not just conceptually related — within each screen, they read the LITERAL SAME
`Subgraph_MACDDiff` array (`sc.Subgraph[2]` from `scsf_Screen1_MACD`/`scsf_Screen2_MACD`), verified
directly: `TripleScreen1.cpp`'s `macdDiff = MACDDiffSubgraphArray[sc.Index] -
MACDDiffSubgraphArray[sc.Index-1]` (feeding `GetImpulse()`) and
`DetectElderMACDDivergence(sc, sc.Index, sc.High, sc.Low, Subgraph_MACDDiff, ...)` both consume the
same subgraph object from the same MACD study instance per screen (TS1 and TS2 each run their own
single shared instance, not two independently-computed MACDs). This means replacing MACD once
(case study #2's Phase 2 MODWT replacement) automatically and simultaneously upgrades both
consumers with a single change — no separate integration risk or drift-between-consumers concern
between the divergence detector and Impulse's momentum term, since there was never a second,
independent MACD computation to keep in sync.

**Two consequences of this fact for how this initiative should actually sequence itself (operator
question, 2026-09-04), not just an architectural footnote**:

1. **Sequencing: case study #2's own MACD replacement is upstream of a real case-study-#3 redesign,
   not a parallel, independently-schedulable track.** §5.5/5.6's fusion-logic design implicitly
   assumes whatever statistical character the CURRENT EMA-based `macdDiff` has (its own noise/lag/
   smoothness profile). If MACD's construction is ever actually replaced (wavelet detail
   coefficients: zero phase-lag, sharper transitions, different noise character than EMA output),
   that changes the very thing §5.5/5.6's fusion rule was designed around — meaning a genuinely
   final Impulse redesign can't be locked in independently of case study #2's own resolution.
   §5.5/5.6 stays a valid design-stage sketch, but committing to it ahead of MACD's own fate risks
   re-deriving the fusion logic a second time once the input signal's character actually changes.
2. **A genuinely new risk, not previously flagged, surfaced by "same array, two consumers" cutting
   both ways.** `DetectElderMACDDivergence`'s own peak/trough state machine (`isLocalMin` checks,
   `MIN_RALLY_ATR_MULTIPLE`, lookback windows) was presumably tuned/validated against the CURRENT
   signal's specific statistical character (EMA-smooth, laggy). Swapping the underlying array to a
   MODWT detail-coefficient stream requires zero code change to the wiring itself (same subgraph
   reference, same consumers) — but the divergence detector's own tuned constants could silently
   stop matching reality once the signal's character changes underneath them, since those constants
   were never re-validated against a zero-lag, sharper-transition input. The "one fix, three
   consumers" leverage (Screen 1 trend direction via `MacdEnum`, the divergence detector, and
   Impulse's momentum axis) is real, but it also means a single unvalidated replacement risks
   silently breaking all three at once rather than one at a time — any eventual MACD replacement
   needs re-validation against the divergence detector's own existing tuned parameters specifically,
   not just against Impulse's new needs.

**A parallel constraint on the OTHER input (13-EMA "inertia"), same question asked of the operator's
own next observation (2026-09-04), verified directly against source**: `Subgraph_ImpulseEMA`
(`src/TripleScreen1.cpp`) is plotted with `sc.GraphRegion = 0` — the **main price panel**, literally
overlaid on candles, not a separate oscillator region — and turns out to have a THIRD consumer
beyond §5.1's own already-documented `maDiff` use: `bool priceIsRising = (currentPrice >
Subgraph_ImpulseEMA[barIndex])` feeds the NH-NL signal's own trend-direction input. So this one EMA
is genuinely three-purpose, not one: (1) `maDiff` → Impulse's inertia/confluence logic (the thing
§5.5 proposes replacing), (2) a plotted visual trend-reference line on the price chart, (3) a
price-vs-EMA comparison feeding NH-NL's trend direction.

**Consequence for the redesign**: a Hurst-persistence-scaled conviction score (§5.5's proposed
replacement, `sign(drift) * f(H)`, a bounded dimensionless confidence value) is not a price-level
quantity and cannot be drawn as a line overlaid on candles the way an EMA can — units are
incompatible, not just a display-styling detail. This means the redesign cannot simply "replace the
EMA" outright: consumers (2) and (3) above have no dependency on §5.5's specific `maDiff`-based
inertia logic and should keep using the EMA completely unchanged; only consumer (1) — the actual
Impulse confluence decision — would read the new persistence-based signal instead. The new signal
would need its own separate visual representation (a new oscillator-style subgraph in its own
`GraphRegion`, matching this system's existing MACD/RSI panel convention) if trader visibility into
the new logic's own reasoning is wanted, rather than reusing the price-panel EMA slot. Not a blocker
to the redesign, but a real "decouple the three uses, don't delete the EMA" constraint that any
eventual implementation plan needs to account for explicitly.



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
5. **RESOLVED 2026-09-04 (CLAUDE_BRIEF_129/129_REPLY)**: Gemini confirmed Peters (1994)'s
   MA-crossover/Hurst framing conceptually (not mechanically — Peters uses R/S/Hurst, not EMAs),
   confirmed Gençay/Selçuk/Whitcher (2001) as "the gold standard" wavelet-MRA reference, and added
   two new activity-clock-specific citations (O'Hara 2015; Dacorogna et al. 2001) more directly
   on-point than the generic subordination citations used elsewhere. Verdict: MACD is genuinely
   distinct from Hurst/DFA (global persistence vs. local band-specific directional momentum) —
   "you need both," not a redundant re-derivation. Not yet independently cross-checked by Claude
   the way the Force Index reply's central claim was (no correction was needed this time, but that
   itself hasn't been separately verified against primary sources).
6. MACD-specific: if a robust multi-window trend-discrepancy construct is built, does it replace
   MACD in place (Screen 1's entire trend-direction mechanism — a much higher-stakes touch than
   Force Index's Screen 2 role) or ship additively first? Not yet decided — likely an even more
   conservative answer than Force Index's own open question 2, given Screen 1 is upstream of
   everything else in the Triple Screen hierarchy. Gemini's own two-phase framing (§2.2) — robust
   smoothing patch now (if ever), full MODWT wavelet replacement later — implies additive/careful
   staging is the right instinct, but this is still an open decision, not a plan.
7. MACD-specific: neither of Gemini's two proposed phases (robust-smoothing EMA patch; full MODWT
   wavelet replacement) has been empirically validated against this system's real tick data. Same
   discipline as Force Index — literature grounding is not sufficient on its own to implement.

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
  analysis (case study #2); confirmed conceptually applicable by Gemini (CLAUDE_BRIEF_129_REPLY),
  not a mechanical match (Peters uses R/S/Hurst, not EMAs).
- Mandelbrot, B., Fisher, A. & Calvet, L. (1997), "A Multifractal Model of Asset Returns" (case
  study #2).
- Gençay, R., Selçuk, F. & Whitcher, B. (2001), *An Introduction to Wavelets and Other Filtering
  Methods in Finance and Economics* (case study #2) — confirmed by Gemini as "the gold standard"
  reference for wavelet Multiresolution Analysis (MODWT) as MACD's rigorous replacement.
- O'Hara, M. (2015), "High Frequency Market Microstructure," *Journal of Financial Economics* —
  traditional indicators failing in continuous time, remapping to transaction-time (case study #2).
- Dacorogna, M. et al. (2001), *An Introduction to High-Frequency Finance* — the Olsen & Associates
  group's canonical text; moving-average/directional indicators mapped onto tick-time/"operational
  time" (case study #2), the most directly on-point activity-clock citation found for MACD.
- `lbrnet/logs/rc_gemini.log` CLAUDE_BRIEF_127/127_REPLY/128/128_REPLY — Force Index consult
  transcript.
- `lbrnet/logs/rc_gemini.log` CLAUDE_BRIEF_129/129_REPLY — MACD consult transcript.
