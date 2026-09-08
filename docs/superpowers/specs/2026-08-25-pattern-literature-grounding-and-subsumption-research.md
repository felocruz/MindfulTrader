# Research: Literature Grounding, Data Availability, and Subsumption for the 9 Raschke/Elder Patterns

**Status**: Research document, 2026-08-25 — feeds the Phase 1 fix-design decision in
`2026-08-25-pattern-detection-institutional-hardening-spec.md` (§4.1 item 2). This is NOT an
architecture decision. It answers three questions the user asked before making one: how
literature-grounded is each of the 4 currently-fieldless patterns (RSI Failure Swing, Stochastic
Pop, ITR Breakout, ITR Fade), what alpha should we expect, and do we have the data to compute them
properly. A fourth question emerged during the brainstorm and turned out to matter more than
expected: can one pattern subsume another, and if so, does that change how `EventDataCollectorStudy.cpp`
should record it. **Update, same session**: that question grew into a full 36-pair logical/geometric
subsumption audit across all 9 patterns (Section 6), including 5 code-certain (provable, not
probabilistic) findings — 3 disjoint pairs and 1 sequential-dependency baked into shared persistent
state. Still not an architecture decision; the audit's own findings still need empirical
confirmation against real `.alpha` history (Section 6's closing caveat) before being finalized.

**Source-quality caveat**: citations below come from secondary sources (WebSearch), not a direct
read of Connors & Raschke's *Street Smarts* (1995) or Wilder's *New Concepts in Technical Trading
Systems* (1978) — this project does not have either primary text in-repo. Treat pattern-definition
claims as "consistent with multiple independent secondary sources," not "verified against the
original page." If a design decision hinges on an exact numeric threshold from the book itself, get
the primary text before finalizing.

## 1. Momentum Pinball / ITR Breakout — NOT two independent patterns

**This is the headline finding.** Per Raschke's own LBR/RSI methodology (secondary-source
confirmed): the Pinball indicator (3-period RSI of 1-period ROC) reading below 30 or above 70 on
**day 1** is a *filter*, not a standalone entry signal. The actual entry rule places a buy-stop
above (or sell-stop below) **day 2's first-hour range** — i.e., an ITR breakout. Pinball and ITR
Breakout are sequential stages of one documented strategy, not two patterns that happen to
co-occur.

**The current C++ code already half-admits this gap.** `StudyHelperFunctions.cpp:1274`'s own
comment: *"Note: Full strategy includes 'first hour ITR' entry mechanics for daily charts, but we
detect the signal condition here (Pinball < 30 or > 70)."* Neither of this repo's two Momentum
Pinball implementations (the crude cascade check, or `DetectMomentumPinball()`'s "Gang Physics"
version) actually waits for or requires a next-day ITR breakout — both fire on the Pinball reading
alone, same bar. Conversely, `DetectRaschkeTacticalTrigger()`'s ITR Breakout check
(`StudyHelperFunctions.cpp:1503-1518`) fires on ANY day's opening-range break with volume
confirmation, with no dependency on whether a Pinball reading preceded it the prior day.

**Implication**: this is not a same-bar mutual-exclusivity or precedence question (§3's "test A,
then B" framing doesn't apply here) — it's a missing multi-day composition. Both patterns as
currently implemented are approximations of *half* of one real strategy, evaluated in isolation.
A literature-faithful implementation would need Pinball's day-1 reading to persist as a *state*
(similar to how `itrHigh`/`itrLow`/`itrEstablished` already persist per-day via
`sc.GetPersistentFloat`/`GetPersistentInt`) that gates whether an ITR breakout on day 2 counts as a
"Momentum Pinball entry" specifically, separate from ITR Breakout firing on its own for other
reasons. **Data availability: yes** — the persistent-variable machinery this needs already exists
in this file for ITR's own day-tracking; extending it to also remember "was yesterday's Pinball
reading extreme" is a small addition, not a new subsystem.

## 2. Stochastic Pop — three ingredients, one deliberately deleted from this codebase

Per secondary sources (StockCharts-published scan derived from the book): the bullish setup
requires **three** simultaneous conditions — (a) a long-term Stochastic Oscillator above 50 (trend
context), (b) ADX below 20 (weak/consolidating trend — the "dip" is a pause, not a reversal), (c) a
short-term Stochastic surging above 80 (the "pop" — momentum snapping back in the trend's favor).
Bearish is the mirror image.

**Gap vs. current implementation**: `DetectRaschkeTacticalTrigger()`'s Stochastic Pop check
(`StudyHelperFunctions.cpp:1361-1384`) only tests a narrow band just above/below the extreme
(`STOCH_OVERSOLD` to `+10`) combined with an RSI3-vs-RSI10 ordering — it implements none of the
three literature ingredients as stated. This is a materially different, ad hoc condition wearing
the same name.

**Data availability, ingredient by ingredient**:
- **(a) Long-term Stochastic**: available, just not cross-wired. `TripleScreen2.cpp:1070` already
  computes a slower Stochastic on TS2/60min (`Subgraph_FastD`, default `Input_FastK=10`) —
  materially longer-period than TS3's tight `sc.Stochastic(sc.BaseDataIn, Subgraph_StochK, 3, 3, 3,
  ...)`. TS3's pattern detection does not currently read this TS2 value at all.
- **(b) ADX < 20**: **not available as stated** — `AdxIndicator` was fully deleted from this
  codebase (DOD/SoA migration Task 15, confirmed zero remaining references,
  `IndicatorKey::ADX_14` does not exist) in favor of the Hurst exponent, per this project's own
  documented rationale ("Hurst exponent provides superior trend persistence measurement"). A
  faithful Stochastic Pop would need either (i) a Hurst-based proxy for "weak/consolidating trend"
  (plausible — low `|Hurst - 0.5|` is a reasonable analogue to low ADX, but this is a translation,
  not a citation, and should be validated empirically before being called equivalent) or (ii)
  reintroducing ADX specifically for this one pattern, which cuts against the project's own prior
  decision to retire it.
- **(c) Short-term Stochastic surge > 80**: available (`Subgraph_StochK` already computed every
  bar on TS3), just not the condition currently being tested — the current code checks a narrow
  band near the extreme rather than "surged above 80."

**Expected alpha**: no literature-cited win rate found for this specific pattern in the sources
searched; the general genre (mean-reversion continuation, "buy the dip in a strong trend") is a
well-documented family, not a fringe idea, but this spec found no specific number to cite. Treat as
unknown pending an actual backtest against real ingredient (a), (b)-proxy, and (c), not the current
implementation.

## 3. RSI Failure Swing — a genuine multi-point RSI-only pattern, currently reduced to a same-bar proxy

Wilder's actual definition (secondary-source confirmed, consistent across multiple independent
sources): a **Top Swing Failure** is RSI peaking above 70, pulling back, failing to make a new RSI
high on the next rally (even if price itself makes a new high — that divergence is the signal),
then RSI breaking below the intervening pullback low. **Bottom Swing Failure** mirrors this on the
downside (RSI bottoms below 30, rallies, fails to make a new low on the next decline, breaks the
intervening rally high). This is a **4-point state machine over RSI's own historical series**
(peak → pullback → failed retest → break of intervening extreme) — it does not involve comparing
two different RSI periods against each other at all; Wilder's original method uses one RSI series.
One secondary source cites a 60-70% win rate for the pattern generally, "less commonly traded than
basic overbought/oversold readings" — cite as a rough, unverified-against-primary-source figure,
not a number to design a threshold around.

**Gap vs. current implementation**: `DetectRaschkeTacticalTrigger()`'s check
(`StudyHelperFunctions.cpp:1328-1359`) does none of this — it compares the CURRENT bar's RSI10
against RSI3 and against recent price extremes, a single-bar heuristic invented for this codebase,
not a port of Wilder's method. The function's own comment admits it: *"Note: Would need RSI array
to compare historical RSI values properly... Current implementation simplified."*

**Data availability: yes, and cheaply.** `Subgraph_RSI10`/`Subgraph_RSI3` are `SCSubgraphRef`
types — full historical arrays computed every bar via `sc.RSI(sc.Close, Subgraph_RSI10,
MOVAVGTYPE_SIMPLE, 10)` (`TripleScreen3.cpp:704`), fully indexable at any past bar
(`Subgraph_RSI10[sc.Index - N]`). The current function only receives today's scalar RSI values as
arguments (`DetectRaschkeTacticalTrigger(sc, rsi3, rsi10, stochK)`) — a proper implementation is a
**data-wiring fix** (pass the subgraph reference instead of a scalar, then run a peak/pullback/
failed-retest/break state machine over it, similar in shape to how NR7's own 3-bar-lookback
breakout check already works, `TripleScreen3.cpp:1491-1523`), not a missing-indicator problem.

## 4. ITR Breakout / ITR Fade — closest to literature-faithful already, purely a reachability bug

The "first hour establishes the day's range, buy-stop above/sell-stop below with a protective stop
at the opposite extreme" description (secondary-source confirmed, and a well-known concept
independent of Raschke specifically — opening-range breakout trading predates her book) matches
what `StudyHelperFunctions.cpp:1429-1532` already tracks: `itrHigh`/`itrLow` established during the
opening hour, `hadBreakoutAbove`/`hadBreakdownBelow` once-per-day flags, and volume confirmation
before firing. **No data-availability gap found here** — Phase 0 (the hardening spec, item 4)
already root-caused this pattern's zero-count as architectural starvation (the near-universal crude
Elder-Breakout check runs first in the same if/return cascade and returns before ITR's code is ever
reached), not a missing-ingredient problem. The one literature gap specific to ITR is Section 1's
finding: it should optionally be gated by a prior day's Pinball reading to fully match Raschke's
composite strategy, which it currently is not.

## 5. Turtle Soup and Elder Breakout — independently reconfirmed, not new findings

Not part of the original 4-pattern research ask, but confirmed in passing: Raschke/Connors' actual
Turtle Soup fades a new **20-day** high/low reversal (secondary-source confirmed) — matching this
repo's "official" `TripleScreen3.cpp` implementation (`TURTLE_SOUP_LENGTH=20`), NOT the crude
cascade's 4-bar version (`TURTLE_SOUP_LOOKBACK=4`, `StudyHelperFunctions.cpp:637`). This is now
confirmed by three independent sources: the hardening spec's own Phase 0 code trace, the Python
port's own docstring (`turtle_soup_detector.py:8-11`), and now the literature. All three point the
same direction: the crude cascade's Turtle Soup is the non-canonical one and is a Phase 1
retirement candidate, not the official 20-bar version.

## 6. Subsumption / composition map across all 9 patterns — full pairwise audit (36/36)

Method: exact boolean conditions extracted from source for all 9 patterns — the 5 "official"
detectors (`IndicatorComputations.h`'s `DetectKangarooTail`/`DetectTurtleSoup`/
`DetectMomentumPinball`/`DetectElderBreakout`, plus NR7's inline `TripleScreen3.cpp:1394-1523`
logic) and the crude cascade's own checks for the 4 fieldless patterns
(`StudyHelperFunctions.cpp:1262-1535`) — then reasoned pairwise. Entries marked **CODE-CERTAIN**
are provable directly from the conditions (opposite-sign comparisons, disjoint numeric ranges, or
shared state-machine variables) — not probabilistic claims. Everything else is a plausibility
judgment from the conditions' economic/geometric meaning and still needs empirical confirmation
against real historical data before being treated as fact.

### 6.1 Code-certain findings (provable now, no data run needed)

- **Momentum Pinball (official) × RSI Failure Swing (crude): DISJOINT.** Bullish Momentum Pinball
  requires `rsi3 > rsi10` (`freshBullishCross`, `IndicatorComputations.h:1005,1017`). Bullish RSI
  Failure Swing requires `rsi10 > rsi3` (`StudyHelperFunctions.cpp:1355`). These cannot both be true
  on the same bar. Same contradiction holds bearish (`rsi3<rsi10` vs. `rsi10<rsi3`). **These two
  patterns can never co-fire in the same direction, by construction.**
- **Momentum Pinball (official) × Stochastic Pop (crude): DISJOINT.** Bullish Momentum Pinball
  requires `stochK < 20` (`IndicatorComputations.h:1017`). Bullish Stochastic Pop (crude) requires
  `stochK > 20 && stochK < 30` (`StudyHelperFunctions.cpp:1370`). Non-overlapping ranges. Bearish is
  the mirror (`stochK>80` vs. `70<stochK<80`). **Also structurally incapable of co-firing.**
- **RSI Failure Swing (crude) × Stochastic Pop (crude): DISJOINT.** Same mechanism as the first
  finding — bullish RFS requires `rsi10>rsi3`, bullish SP requires `rsi3>rsi10` (via `rsi3 >
  RSI_OVERSOLD && rsi3 > rsi10`, `StudyHelperFunctions.cpp:1372`).
- **Reading on these three together**: within the crude cascade's own original oscillator logic
  (Momentum Pinball's *official* replacement aside — the crude cascade's own Pinball check uses a
  different LBR/RSI formula, not RSI3-vs-RSI10 sign), RFS and Stochastic Pop were seemingly
  designed as a deliberate, disjoint priority scheme on `rsi3`/`rsi10`/`stochK` bands — genuinely
  mutually exclusive by numeric construction, not an accident. The Phase 0 bug is elsewhere in the
  cascade (the near-universal Elder-Breakout-crude check and the buried ITR block bolted on after
  this oscillator logic, plus the later-added "official" overwrite blocks never reconciled with
  it) — not in this specific 3-pattern oscillator design, which holds together on its own terms.
- **ITR Breakout × ITR Fade: SEQUENTIAL-DEPENDENT, not independent.** ITR Fade's own condition
  requires a same-day, opposite-direction ITR Breakout to have already fired
  (`hadBreakdownBelow`/`hadBreakoutAbove`, shared persistent state, `StudyHelperFunctions.cpp:
  1522,1528`). ITR Fade cannot fire on a day where ITR Breakout hasn't already fired in the other
  direction. This is a real dependency baked into shared state, not a same-bar co-occurrence
  question at all.
- **Momentum Pinball (crude, the real LBR/RSI formula) → ITR Breakout: cross-day composition**
  (Section 1) — restated here as the 36th pair for completeness; not a same-bar relationship.

### 6.2 Structural analogs — same underlying market phenomenon, different reference window (plausible near-redundancy, needs empirical confirmation)

| Pair | Shared concept | Different reference |
|---|---|---|
| Turtle Soup × ITR Fade | "Price breached a level, then failed and closed back inside" | 20-bar rolling extreme vs. the day's own opening range |
| Elder Breakout × ITR Breakout | "Price is breaking out with continuation force" | Keltner band (20-EMA±2.5×ATR) vs. the day's opening range |
| RSI Failure Swing × ITR Fade | "Bullish/bearish reversal at/near a recent extreme" | RSI divergence at a price extreme vs. a failed opening-range breakout |
| NR7 × Stochastic Pop | "Low-volatility/weak-trend pause, then expansion" | 7-bar range compression vs. ADX(-proxy)+long-term-stochastic context |

These are the pairs most likely to inflate a naive co-fire count without adding real independent
evidence — not because one implies the other logically, but because they're plausibly measuring
the *same* underlying market event through two different lenses. Worth explicit empirical
correlation checks before trusting a "both fired" feature as two independent confirmations.

### 6.3 Near-disjoint by regime contradiction (same direction is economically incoherent)

| Pair | Why |
|---|---|
| Turtle Soup × Elder Breakout | Reversion-into-range vs. continuation-out-of-range, same direction |
| Turtle Soup × ITR Breakout | Same reversion-vs-continuation contradiction |
| Elder Breakout × RSI Failure Swing | EB-bullish implies price relatively high (above upper band); RFS-bullish implies price at a recent low — contradictory implicit price levels |
| Elder Breakout × ITR Fade | Continuation-breakout vs. failed-breakout-reversal, same direction |
| RSI Failure Swing × ITR Breakout | Momentum-fading-at-a-low vs. volume-confirmed-breakout-of-the-high |
| Kangaroo Tail × NR7 | Long-tail (wide, asymmetric) bar vs. narrowest-range-in-7-bars bar; also temporally offset (NR7's breakout fires 1-3 bars after its own compression bar) |

### 6.4 Plausible, likely-independent co-fire candidates (good meta-labeler feature candidates, not redundant by construction or contradiction)

Kangaroo Tail × Momentum Pinball, Kangaroo Tail × Stochastic Pop, Kangaroo Tail × Elder Breakout
(dip-then-rally shape is coherent), Turtle Soup × Momentum Pinball (different data entirely —
price-structure vs. oscillator — a genuinely reinforcing combo if both fire), Turtle Soup × RSI
Failure Swing, Elder Breakout × NR7 (compression-then-breakout, adjacent but not identical
measurement to 6.2's NR7×Stochastic-Pop pair), NR7 × RSI Failure Swing, NR7 × ITR Breakout/Fade,
Momentum Pinball × Elder Breakout (mild regime tension — reversal-starting vs. already-trending —
but not contradictory), Momentum Pinball × NR7, Momentum Pinball × ITR Breakout/Fade, Stochastic
Pop × ITR Breakout/Fade.

**This completes all 36 pairs.** Sections 6.1-6.3 are the actionable ones (provable-now, or
flagged-as-likely-redundant, or flagged-as-likely-contradictory); 6.4 is the residual "genuinely
worth testing as independent evidence" set.

**Caveat that does still apply**: this is a *logical/geometric* audit of the conditions as written,
not an *empirical* one against real historical data. Two conditions being logically independent
doesn't guarantee low real-world correlation (both could still spike together during, say, high-
volatility regimes for reasons neither condition encodes), and two conditions being "plausibly
analogous" (Section 6.2) isn't proof of high empirical overlap. The DISJOINT/SEQUENTIAL-DEPENDENT
findings in 6.1 are certain (they follow from the code, not from data); everything in 6.2-6.4 should
be treated as a prioritized list of what to check first against real `.alpha` history, not a
finished answer.

## 7. Implications for the brainstorm's two open design threads

**"Record everything, let the model learn co-fire effects" (per the AFML-literature answer given
earlier in this session)**: still the right default, but this research sharpens it — a co-fire
feature is only informative to the extent the co-firing patterns are NOT subsumption pairs. Where
subsumption is confirmed (ITR Breakout ⟸ crude Elder Breakout), recording "both fired" adds a
redundant bit, not new evidence; the meta-labeler would need to learn that redundancy is harmless
(it likely can, at some small feature-count cost) — but it's cleaner to document known subsumption
relationships explicitly rather than rely on the model to rediscover them.

**"Test A, then B; record whichever fires" (the user's precedence-cascade idea)**: confirmed
correct for the LIVE single-decision point, confirmed wrong for training-data recording (see the
prior in-chat answer, reproduced here for the written record): collapsing to one winner at
recording time destroys the ability to later ask whether A's outcome differs when B also fires.
**Additionally, this research shows the cascade framing doesn't even apply uniformly** — the
Pinball/ITR-Breakout pair is a cross-bar composition, not a same-bar alternative, so a single
"precedence table" mechanism would need to handle at least two structurally different relationship
types (same-bar mutual information overlap vs. cross-bar sequential composition), not one uniform
rule.

## 8. Open questions (not resolved by this document)

1. Should Momentum Pinball's live/training representation be split into "Pinball reading" (a
   same-bar oscillator state, independent of what follows) and "Pinball-confirmed ITR entry" (the
   full 2-day composite, gated on yesterday's reading) as two distinct recorded signals? This
   research suggests yes but doesn't design it.
2. Is a Hurst-based proxy for Stochastic Pop's ADX<20 ingredient empirically equivalent enough to
   use, or does this pattern need its own case-by-case indicator? Needs a backtest, not a literature
   answer.
3. **DONE (2026-08-25, same session)** — full 36-pair logical/geometric audit in Section 6. Residual
   work: empirically verify Section 6.2's "structural analog" and 6.4's "plausible independent"
   groupings against real historical `.alpha` co-occurrence rates, once the 4 fieldless patterns are
   instrumented — logical analysis alone can't distinguish "plausibly independent" from "correlated
   for an unrelated reason" without real data.
4. Does the primary text (*Street Smarts*, Wilder's book) exist anywhere accessible to this project,
   to upgrade this document's citations from secondary-source-consistent to verified?
