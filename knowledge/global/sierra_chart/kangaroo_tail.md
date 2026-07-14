---
domain: sierra_chart/screen3_patterns
intent: Elder's Kangaroo Tail reversal pattern — canonical 3-bar definition, entry/stop rules, and distinction from Peters/Nekritin's structurally different single-candle variant
scope: global
tags: [elder, kangaroo-tail, screen3, reversal, bar-pattern, TS3, classical-chart-analysis, failed-raid]
source_files:
  - src/SCStudies.cpp
  - include/Indicator.h
last_verified: 2026-06-27
dependencies: [elder_triple_screen]
---

# Elder's Kangaroo Tail

## Why This Exists
The Kangaroo Tail is Elder's own coined pattern, first introduced in *Come Into My Trading
Room* (2002) and given a dedicated chapter in *The New Trading for a Living* (2014, Ch. 20).
MindfulTrader uses it as a Screen 3 entry trigger (`KANGAROO_TAIL = 21` in `IndicatorKey`).
A separate 1-candle pattern of the same name exists in Peters/Nekritin's *Naked Forex* (2012)
— they are mechanically distinct. Using the wrong definition produces wrong stop placement
and wrong entry timing.

---

## The Invariant / Contract

**Elder's canonical definition (direct quote):**
> *"A kangaroo tail (or finger) is a single, very tall bar, flanked by two regular bars,
> that protrudes from a tight weave of prices. Kangaroo tails reflect failed bull or bear raids."*

Three things must be true simultaneously:
1. **Three-bar structure** — one anomalously tall bar between two narrow, regular-range bars
2. **Emergence from consolidation** — the tall bar protrudes from a "tight weave" (range-bound
   price action), not from open trending space
3. **Failed raid** — the tall bar's open and close are near the base of the spike; the
   extreme was rejected

---

## How It Works

### Bar Structure (OHLC bar chart — not candlestick)

```
Bar 1 (left flank)   — narrow, regular range, part of the tight weave
Bar 2 (the tail)     — single dramatically tall bar; open AND close near the base;
                       the ENTIRE bar range is anomalous, not just the shadow
Bar 3 (right flank)  — narrow bar, closes back near the base of Bar 2
```

The "tail" refers to the entire height of Bar 2 — Elder's concept is about range anomaly
on an OHLC bar, not the wick length of a candlestick body.

### Signal Direction

| Tail direction | Bar 2 open/close position | Meaning | Signal |
|---|---|---|---|
| Spike downward | Near the top of Bar 2's range | Failed bear raid | Bullish reversal |
| Spike upward | Near the bottom of Bar 2's range | Failed bull raid | Bearish reversal |

### Entry
Enter against the tail direction **as soon as the pattern is recognized** — typically on
or near the close of Bar 3 as it forms. This is a market order entry, not a pending stop.

### Stop Placement
Place the initial protective stop at **approximately the midpoint of Bar 2's range**.
Elder's logic: if price re-enters the midpoint of the tail, the failed-raid thesis is
invalidated and the position should be exited.

### Exit and Duration
- Short-term tactic only. Elder's direct quote: *"On the daily charts, these signals
  fizzle out after a few days."*
- No fixed pip or R-multiple target. Manage exits using Elder's moving average channels
  (envelope bands as dynamic targets).
- **Timeframe weighting**: *"The longer the time frame, the more meaningful its signal."*
  Weekly tails outrank daily; daily outrank intraday.

### Triple Screen Placement
Elder presents the Kangaroo Tail in his Classical Chart Analysis chapters, **not** in the
Triple Screen chapters. His canonical Screen 3 in the Triple Screen text is the trailing
stop-order mechanic. Using the KT as a Screen 3 entry trigger is a practitioner extension
of Elder's tools — legitimate, but not explicitly Elder's own formulation in print.

---

## Peters/Nekritin Variant (*Naked Forex*, 2012) — Do Not Confuse

Peters/Nekritin adopted the same name for a structurally different pattern. Their version
is a 1-candle pattern with hard knockout rules unrelated to Elder's definition.

| Attribute | **Elder (2002/2014)** | Peters/Nekritin (2012) |
|---|---|---|
| Bar count | **3 bars** | 1 single candle |
| "Tail" refers to | Entire anomalous bar height | Wick/shadow only |
| Chart type | OHLC bar chart | Candlestick |
| Prior containment rule | None | Open AND close must be within prior bar's range |
| Room to the left | None | 5–30 bars of clear space at wick tip (hard requirement) |
| S/R zone required | None | Wick must touch a validated zone (hard knockout) |
| Entry | Immediate on recognition | Stop order beyond candle extreme; cancel if next bar doesn't trigger |
| **Stop placement** | **~50% through Bar 2's range** | **At the extreme tip of the wick** |
| Hold duration | A few days max (daily charts) | Zone-to-zone; R:R based |

The stop placement difference is the highest-consequence distinction: Elder's stop is at
the midpoint of the tail bar; Peters' stop is at the far extreme tip. These produce
materially different risk exposures on the same bar.

---

## Raschke — Related Geometry, Different Trade Direction

Raschke does not use the term "Kangaroo Tail." Her related patterns apply the same
underlying bar geometry with a different trade logic:

**"Eat the Tail"** (*Short-Term Scalping* PDF): A single-bar long-wick rejection bar,
but traded as a **continuation** — entry triggers when the next bar breaks through the
tail's extreme *in the trend direction*. Elder and Peters both trade *against* the wick
(counter-trend reversal). Same geometry, opposite trade direction.

**V-Spike Climax Reversal**: Raschke's higher-conviction reversal. Requires both range
expansion AND a volume surge — two hard gates neither Elder nor Peters require.

---

## Failure Modes

**FM-01: Applying Peters' stop (tip of wick) instead of Elder's stop (midpoint)** — The
Peters' stop is significantly wider on the same bar. Using it when the implementation
targets Elder's pattern over-risks every Kangaroo Tail trade.

**FM-02: Triggering on a spike in open trending space** — Elder explicitly requires
emergence from a "tight weave." A spike during an active trend impulse does not qualify;
the consolidation context is part of the pattern definition.

**FM-03: Confusing the 1-candle Peters version with Elder's 3-bar structure** — Most
TradingView indicators and retail forex sources implement the Peters version. Any
third-party indicator labeled "Kangaroo Tail" should be verified against the 3-bar
OHLC definition before use.

**FM-04: Treating the KT as prescribed entry (not a Screen 3 filter)** — In the
Elder-Raschke Confluence System, Screen 1 and Screen 2 must agree first. The KT signals
the entry timing on Screen 3 only within a confirmed directional context. Trading it as
a standalone reversal without Screen 1/2 alignment is outside the system's design.

---

## References
- Elder, A. *Come Into My Trading Room.* Wiley, 2002. ISBN 978-0-471-22534-8
  (first introduction of the Kangaroo Tail)
- Elder, A. *The New Trading for a Living.* Wiley, 2014. ISBN 978-1-118-44392-7
  (Part 3: Classical Chart Analysis, Ch. 20 — dedicated chapter)
- Peters, A., Nekritin, A. *Naked Forex.* Wiley, 2012. ISBN 978-1-118-11401-8
  (Ch. 8 — distinct 1-candle variant; same name, different rules)
- Raschke, L.B. *Short-Term Scalping: Fun with Candlesticks.* LBRGroup PDF
  ("Eat the Tail" — related geometry, continuation direction)
- Worden Forums — "Dr. Elder's Kangaroo Tail Reversal Pattern" (community attribution)
- Trading Literacy — kangaroo-tail-pattern (secondary summary, Elder-attributed)
