---
domain: sierra_chart/elder_triple_screen
intent: Elder-Raschke Confluence System — MindfulTrader's named trading methodology combining Elder's Triple Screen hierarchy with Raschke entry patterns and regime-aware signal filtering
scope: global
tags: [elder-raschke-confluence, elder, raschke, triple-screen, macd, impulse-system, force-index, TS1, TS2, TS3, timeframe, tide-wave-ripple, screen1, screen2, screen3, confluence]
source_files:
  - src/SCStudies.cpp
  - src/IndicatorManager.cpp
last_verified: 2026-06-27
dependencies: []
---

# Elder-Raschke Confluence System

## Why This Exists
MindfulTrader implements the **Elder-Raschke Confluence System** — a named evolution of
Elder's Triple Screen Trading System. The three-timeframe hierarchy (TS1=240min tide,
TS2=60min wave, TS3=15min ripple), Impulse System, and Force Index are inherited directly
from Alexander Elder. Screen 3's mechanical trailing stop has been replaced by Linda
Raschke's pattern repertoire (Kangaroo Tail, Turtle Soup, Momentum Pinball, NR7). A
regime-aware signal layer (Student-t HMM, Hurst/DFA, Shannon entropy, Taleb kurtosis)
conditions all three screens. "Confluence" names the core requirement: all three screens
must agree before an entry fires.

Misreading any screen's role (especially Screen 1 as a signal source rather than a
directional filter) produces architectural defects.

## The Invariant / Contract

**Screen 1 establishes the only permitted trade direction. Screens 2 and 3 exist to time
the entry within that direction — never to override it.** A bearish Screen 1 cancels all
long setups regardless of how strong the Screen 2/3 signals appear.

---

## 1. Bibliography

| Work | Year | Publisher | Notes |
|---|---|---|---|
| "Triple Screen Trading System" | 1986 | *Futures Magazine* (April) | Original public presentation |
| *Trading for a Living* | 1993 | John Wiley & Sons (ISBN 978-0-471-59224-2) | Chapters 43–45; canonical system definition |
| *Come Into My Trading Room* | 2002 | John Wiley & Sons (ISBN 978-0-471-22534-8) | Introduces Impulse System; "Triple Screen Update" section |
| *The New Trading for a Living* | 2014 | John Wiley & Sons (ISBN 978-1-118-44392-7) | Revised edition with updated tools |

The Impulse System was **not** in the 1993 book — it first appeared in 2002.

---

## 2. Three-Screen Structure (Tide / Wave / Ripple)

| Screen | Metaphor | Role | Output |
|---|---|---|---|
| **Screen 1** | The Tide | Long-term trend — directional filter | Bias only (long-only or short-only) |
| **Screen 2** | The Wave | Intermediate pullback — entry setup | Setup signal (agree / no agree) |
| **Screen 3** | The Ripple | Short-term precision entry | Conditional stop order |

**Asymmetric design:** Screens 1 and 2 are analytical. Screen 3 requires no chart — it is a
mechanical conditional order placed automatically after Screens 1 and 2 agree.

---

## 3. Timeframe Ratio Rule

**Elder's stated ratio: 4–6× between adjacent screens (5 is the canonical midpoint example).**
The ratio applies Screen 1→Screen 2 AND Screen 2→Screen 3.

| Use Case | Screen 1 | Screen 2 | Screen 3 |
|---|---|---|---|
| MindfulTrader (futures intraday) | 240-min | 60-min | 15-min |
| General intraday | 240-min | 60-min | 10–15-min |
| Swing trading | Weekly | Daily | 4-hour |
| Position trading | Monthly | Weekly | Daily |

MindfulTrader's 240/60/15 stack satisfies the 4× ratio (240÷60=4, 60÷15=4) at the floor of
Elder's specified range.

---

## 4. Screen 1 — The Tide (Long-Term Trend Filter)

**Indicators:**
- **MACD Histogram (12, 26, 9):** fast EMA 12, slow EMA 26, signal line 9-period EMA
- **13-period EMA** of closing price

**Critical rule — slope, not level:**
Screen 1 reads the **slope** of the MACD histogram (current bar vs. prior bar), NOT whether
the histogram is above or below zero.

```
Rising histogram  → bullish tide  (even if histogram value is negative)
Falling histogram → bearish tide  (even if histogram value is positive)
```

Elder direct quote: *"The best buy signals occur when MACD-Histogram is below its centerline
but its slope turns up, showing that bears have become exhausted."*

**Asymmetric filtering rules (non-negotiable):**

| Screen 1 State | Allowed | Forbidden |
|---|---|---|
| Bullish (histogram rising) | Long positions only | All short selling — discarded regardless of lower-timeframe signals |
| Bearish (histogram falling) | Short positions only | All long buying — discarded regardless of lower-timeframe signals |
| Neutral / ambiguous | No trade | — |

---

## 5. Screen 2 — The Wave (Intermediate Oscillator / Setup)

**Purpose:** Within the Screen 1 trend, identify a **counter-trend pullback** on the
intermediate timeframe. You are buying the temporary dip within an uptrend, or shorting the
temporary bounce within a downtrend.

### Primary Oscillator: Force Index (2-period EMA)

```
Force_Index_raw(t) = (Close(t) − Close(t−1)) × Volume(t)
Force_Index_2     = 2-period EMA of Force_Index_raw
```

Three components only: price direction/magnitude × volume conviction.

**Screen 2 entry rules (Force Index 2-EMA):**

| Screen 1 State | Force Index condition | Action |
|---|---|---|
| Uptrend (bullish) | FI(2) dips **below zero** | Correction within uptrend detected → proceed to Screen 3 for buy stop |
| Downtrend (bearish) | FI(2) rises **above zero** | Bounce within downtrend detected → proceed to Screen 3 for sell stop |
| Flat / ambiguous | — | **No setup emitted** — Elder's slope rule requires a clear direction |

**C++ mapping (`FI2Signal::setFromChart`, `Indicator.cpp`):** The Screen 1 state is passed as a `MacdEnum` argument. `POSITIVE_FLAT` and `NEGATIVE_FLAT` (histogram above/below zero with zero slope) map to `NEUTRAL_OR_TREND_ALIGNED` — no signal. Fixed 2026-06-27; prior to the fix these states incorrectly emitted `PULLBACK_FOR_LONG` / `RALLY_FOR_SHORT`.

**Force Index divergence (strongest signals):**
- Bullish divergence: Price makes new low, FI makes shallower low → bears losing power
- Bearish divergence: Price makes new high, FI makes lower high → bulls losing power

### Alternative Oscillators (Elder explicitly endorses all as fully valid)

| Oscillator | Parameters | Bull entry trigger | Bear entry trigger |
|---|---|---|---|
| Stochastic | (5,3,3) preferred; (14,3,3) also used | Falls below 30, hooks up | Rises above 70, hooks down |
| Williams %R | 13 or 14 period | Falls to oversold extreme, turns up | Rises to overbought extreme, turns down |
| RSI | 14 period | Drops to oversold, turns up | Rises to overbought, turns down |
| Elder-Ray (Bear Power) | 13-period EMA base | Bear Power dips below zero, turns up | Bull Power rises above zero, turns down |

---

## 6. Screen 3 — The Ripple (Entry Trigger)

**No chart or indicator analysis on Screen 3.** It is a mechanical conditional order with
a daily adjustment rule. Entry is via a trailing stop placed one tick beyond the prior bar's
extreme.

**Bullish entry mechanics:**
1. Place a **buy stop one tick above the high of the prior completed bar** (Screen 2 timeframe)
2. If session closes without trigger: **trail the stop down** to one tick above the new bar's high
3. Continue trailing each bar until triggered or Screen 1 reverses → cancel order

**Bearish entry mechanics:**
1. Place a **sell stop one tick below the low of the prior completed bar**
2. Trail up each bar (to one tick below the new, higher bar's low)
3. Same Screen 1 reversal cancellation rule

**Stop loss placement after entry:**
- Long: one tick below the low of the entry bar or prior bar (whichever is lower)
- Short: one tick above the high of the entry bar or prior bar (whichever is higher)

---

## 7. Force Index — Full Specification

```
Force_Index_raw(t) = (Close(t) − Close(t−1)) × Volume(t)
Force_Index_2      = 2-period EMA  of raw FI   ← Screen 2 oscillator
Force_Index_13     = 13-period EMA of raw FI   ← intermediate trend/strength
```

Elder also specifies pairing Force Index with a **22-day EMA of price** for trend context.

| Condition | Interpretation |
|---|---|
| FI > 0 and rising | Bulls controlling; momentum strengthening |
| FI > 0 and falling toward zero | Bull power weakening; correction risk |
| FI < 0 and falling | Bears controlling; momentum strengthening |
| FI < 0 and rising toward zero | Bear power weakening; correction ending |
| FI crosses above zero | Bullish signal |
| FI crosses below zero | Bearish signal |

**Breakout confirmation:** Genuine breakout requires a Force Index spike in the breakout direction. Breakout with weak Force = suspect.

---

## 8. The Impulse System (introduced 2002)

**Two components — both evaluated by slope, not level:**
1. **13-period EMA** of closing price — trend direction
2. **MACD Histogram (12, 26, 9)** — momentum direction

| Color | EMA(13) | MACD-H | Meaning |
|---|---|---|---|
| **Green** | Rising (> prior bar) | Rising (> prior bar) | Both rising — bulls control trend AND momentum |
| **Red** | Falling (< prior bar) | Falling (< prior bar) | Both falling — bears control trend AND momentum |
| **Blue** (neutral) | Mixed | Mixed | One rising, one falling, or flat |

**Prohibition-first framing (Elder's own):**

| Color | Allowed | Forbidden |
|---|---|---|
| Green | Buy long, or stand aside | **Shorting absolutely forbidden** |
| Red | Sell short, or stand aside | **Buying absolutely forbidden** |
| Blue | Either direction, or stand aside | Nothing forbidden; selectivity advised |

**Key nuance:** Green does not command a buy — it *prohibits a short*. Red does not command a
short — it *prohibits a buy*. The system is censorship, not prescription.

**Multi-timeframe rule:** The Impulse color on the higher timeframe governs what is permitted
on the lower timeframe. A long trade on Screen 2 requires Screen 1 Impulse to be Green (or
at minimum not Red).

---

## 9. Indicator Parameters — Complete Reference

| Indicator | Parameter | Value | Used In |
|---|---|---|---|
| MACD Histogram — fast EMA | Periods | 12 | Screen 1, Impulse System |
| MACD Histogram — slow EMA | Periods | 26 | Screen 1, Impulse System |
| MACD Histogram — signal line | Periods | 9 | Screen 1, Impulse System |
| EMA — trend | Periods | 13 | Screen 1, Impulse System |
| Force Index — Screen 2 | EMA smoothing | 2 | Screen 2 oscillator |
| Force Index — intermediate | EMA smoothing | 13 | Standalone / Screen 1 supplement |
| Force Index — price companion | EMA of price | 22 | Force Index trend context |
| Stochastic — preferred | Params | (5, 3, 3) | Screen 2 alternative |
| Williams %R | Periods | 13 or 14 | Screen 2 alternative |

---

## Failure Modes

**FM-01: Reading MACD-H level instead of slope** — Treating a positive MACD-H value as
bullish regardless of whether it is rising or falling. A falling positive histogram is
bearish by Elder's definition. Screens TS1/TS2 both use slope logic; wrong comparison
direction inverts the signal.

**FM-02: Treating Screen 3 as an analytical screen** — Applying indicators to the
15-minute chart for discretionary entries. Screen 3 is a mechanical stop order, not an
analysis layer. Any indicator logic on TS3 that overrides the trailing-stop mechanic is
outside the original system.

**FM-03: Confusing 5:1 as a fixed ratio** — Elder's range is 4–6×. MindfulTrader's 4×
stack (240/60/15) is at the valid floor. Treating 5 as mandatory would require changing
the stack.

**FM-04: Shorting on a Green Impulse bar** — The Impulse system's Green prohibition is
absolute. Any code path that submits a short entry when the Screen 1 Impulse is Green
violates the system's core filtering rule.

**FM-05: Generating setups on flat MACD-H (FIXED 2026-06-27)** — `POSITIVE_FLAT` (histogram
above zero, zero slope) and `NEGATIVE_FLAT` (below zero, zero slope) are ambiguous by Elder's
slope rule and must produce no signal. Prior to the fix, `FI2Signal::setFromChart` treated them
as weak bullish/bearish, emitting `PULLBACK_FOR_LONG` / `RALLY_FOR_SHORT` on flat Screen 1.
Fixed by falling through to `NEUTRAL_OR_TREND_ALIGNED` in both cases (`Indicator.cpp:342-346`).

---

## References
- Elder, A. *Trading for a Living.* Wiley, 1993. ISBN 978-0-471-59224-2 (Chapters 43–45)
- Elder, A. *Come Into My Trading Room.* Wiley, 2002. ISBN 978-0-471-22534-8 (Impulse System)
- Elder, A. *The New Trading for a Living.* Wiley, 2014. ISBN 978-1-118-44392-7
- Elder, A. "Triple Screen Trading System." *Futures Magazine*, April 1986
- [StockCharts ChartSchool — Force Index](https://chartschool.stockcharts.com/table-of-contents/technical-indicators-and-overlays/technical-indicators/force-index)
- [StockCharts ChartSchool — Elder Impulse System](https://chartschool.stockcharts.com/table-of-contents/chart-analysis/chart-types/elder-impulse-system)
