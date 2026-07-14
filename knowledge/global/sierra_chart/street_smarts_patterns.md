---
domain: sierra_chart/screen3_patterns
intent: Street Smarts (Raschke & Connors, 1995) — setup/trigger framework and all named patterns used as Screen 3 entries in the Elder-Raschke Confluence System
scope: global
tags: [raschke, connors, street-smarts, screen3, turtle-soup, momentum-pinball, holy-grail, anti, NR7, crabel, 80-20, adx-gapper, setup-trigger, TS3]
source_files:
  - src/SCStudies.cpp
  - src/IndicatorManager.cpp
  - include/Indicator.h
last_verified: 2026-06-27
dependencies: [elder_triple_screen]
---

# Street Smarts — Raschke & Connors (1995)

## Why This Exists
MindfulTrader's Screen 3 replaces Elder's mechanical trailing stop with Linda Raschke's
named pattern repertoire. The authoritative source for most of these patterns is
*Street Smarts: High Probability Short-Term Trading Strategies* (Raschke & Connors, 1995,
M. Gordon Publishing, ISBN 978-0965046107). Understanding the canonical rules prevents
implementation drift from the original setups.

**Note: Kangaroo Tail is NOT in this book.** It originates from Walter Peters'
*Naked Forex*. See the separate chunk when created.

---

## The Invariant / Contract — Setup / Trigger Framework

Raschke explicitly separates every trade into two stages. This is the organizing
principle of the entire book:

**Setup** — background condition that puts the market in a favorable probability state.
The setup does NOT cause entry. Examples: ADX above 30, LBR/RSI below 30 on prior close,
new 20-bar extreme formed at least 3 sessions ago.

**Trigger** — specific price action that initiates the order, typically a pending stop
placed one tick beyond the prior bar's extreme. The trigger only fills if the market
confirms the anticipated move; otherwise the pending order expires or is cancelled.

This maps directly to MindfulTrader's `RASCHKE_STRATEGY_SETUP` (setup phase) and
`RASCHKE_TACTICAL_TRIGGER` (trigger phase) in the `IndicatorKey` enum.

---

## Book Structure

| Part | Classification | Patterns |
|---|---|---|
| I — Tests | Counter-trend (false breakout / short cycle) | Turtle Soup, Turtle Soup +1, 80-20s, Momentum Pinball, 2-Period ROC |
| II — Retracements | Trend-following (pullback entry) | Holy Grail, The Anti, ADX Gapper |
| III — Climax | Exhaustion reversals | Whiplash, 3-Day Unfilled Gap Reversal, Wolfe Waves |
| IV — Breakout Mode | Volatility compression breakout | NR4/NR7, Historical Volatility + Crabel |

---

## Pattern 1: Turtle Soup (`TURTLE_SOUP = 22`)

**Type**: Counter-trend — fades false breakouts of the 20-bar channel.

**Setup (Long)**
1. Market makes a new 20-bar low
2. Prior 20-bar low occurred **≥ 3 trading sessions before current bar** (minimum separation)

**Trigger (Long)**: Place buy stop **5–15 ticks above the prior 20-bar low level** on the
same session the new low is made. Entry fires when price reverses back above that level.

**Stop**: At or just below today's low (the new 20-bar low).

**Exit**: Trailing stop; 2–6 bar hold. Cancel pending order if a new 20-bar extreme forms.

**Turtle Soup Plus One** variant: additionally requires the close of the new-extreme bar
to be at or beyond the prior 20-bar extreme (confirms close outside the channel trapping
more breakout participants). Entry is placed on the **next bar** (one session later).

---

## Pattern 2: Momentum Pinball (`MOMENTUM_PINBALL = 23`)

**Type**: Counter-trend mean-reversion using Taylor Buy/Sell Day cycle.

**Indicator — LBR/RSI (computed on daily bars)**
```
ROC_1    = close[today] - close[yesterday]   // 1-period rate of change (additive)
LBR_RSI  = RSI(ROC_1, 3)                    // 3-period RSI of that ROC series
```
Oscillates 0–100. Signal is generated on the prior daily close; execution is next session.

**Setup**: LBR/RSI closes **below 30** → Buy Day tomorrow; **above 70** → Sell Day tomorrow.

**Trigger (Long)**
1. After the **first 60-minute bar** closes, capture its range
2. Place buy stop **one tick above the first-hour high**
3. One re-entry permitted at the same level if stopped out

**Stop (Long)**: Low of the first 60-minute bar.

**Exit**: Losing position → close of session (~3:10 PM ET). Winning/breakeven → may carry
overnight for next-morning follow-through. Never held a second night.

**Optional filter**: Long signals higher probability when price is above the 20-period EMA.

---

## Pattern 3: 80-20s

**Type**: Counter-trend intraday reversal after range exhaustion.

**Setup (Long — bearish rejection bar)**
1. Yesterday: opened in the **top 20% of daily range**, closed in the **bottom 20%**
2. Yesterday's bar is larger than the recent average daily bar (ATR filter; lookback unspecified)

**Trigger (Long)**
1. Today: wait for price to trade **5–15 ticks below yesterday's low** (the "extension")
2. Once that extension occurs and price reverses, place buy stop at yesterday's low

**Stop**: Today's low (the intraday extreme just made).

**Exit**: Day trade only — must exit by session close.

**Short setup**: Mirror — yesterday opened bottom 20%, closed top 20%; today extends
above yesterday's high; sell stop at yesterday's high.

---

## Pattern 4: Holy Grail

**Type**: Trend-following — pullback entry after ADX confirms strong trend.

**Setup (Long)**
1. 14-period ADX **above 30** (strong trend)
2. Trend is upward (+DI > -DI implied)
3. Price **retraces to the 20-period EMA**
4. ADX must remain above 30 during the pullback (if it drops below, setup invalidated)
5. **Only the first pullback** to the EMA after ADX exceeds 30 is traded

**Trigger (Long)**: Buy stop **one tick above the high of the bar that touched the 20 EMA**.

**Stop**: Below the swing low formed during the pullback.

**Exit**: Trail stop below the 20 EMA as trend resumes; alternatively target prior swing highs.

**Short setup**: Mirror — ADX above 30, -DI > +DI, first pullback up to 20 EMA, sell stop
one tick below the low of the EMA-touch bar.

---

## Pattern 5: The Anti

**Type**: Trend-following continuation — enters on oscillator hook during corrective pause.

**Prerequisite**: Must be preceded by a short-term impulse move in the trade direction.
Best odds when the prior impulse leg is larger than the corrective leg.

**Oscillator** (either option):
- **3/10 Oscillator**: 3-period EMA minus 10-period EMA (fast/slow MACD variant)
- **Stochastic**: %K = 7 periods, %D = 10 periods (some sources: %D = 16)

**Setup (Long)**
1. Oscillator slow line is in a definite uptrend
2. Fast line has pulled back toward (but not necessarily crossed) the slow line
3. Fast line **hooks back upward** — the "stochastic hook"

**Trigger (Long)**: Buy stop one tick above the high of the hook bar.

**Stop**: Just below the low of the entry bar (or most recent swing low).

**Exit**: 2–4 bar hold. Exit when oscillator lines cross unfavorably.

**Note**: Buy Antis are more frequent than Sell Antis.

---

## Pattern 6: ADX Gapper

**Type**: Trend-following — fade counter-trend gap within strong trend.

**Setup (Long)**
1. 12-period ADX **above 30**
2. +DI (28-period) **> -DI (28-period)** (uptrend confirmed)
3. Today's open **gaps below yesterday's low** (gap opposes the trend)

**Trigger (Long)**: Buy stop **at or just above yesterday's low**.

**Stop**: Today's low (the intraday low of the gap day).

**Short setup**: Mirror — ADX above 30, -DI > +DI becomes -DI > +DI (downtrend), gap
up above yesterday's high, sell stop at yesterday's high.

**Distinction from Holy Grail**: Holy Grail uses 14-period ADX + 20 EMA touch as anchor;
ADX Gapper uses 12-period ADX + 28-period DI + overnight gap as the catalyst.

---

## Pattern 7: NR7 / NR4 — Crabel Integration (`NR7 = 25`)

**Type**: Breakout — enters after volatility compression signals expansion.

**Definitions**
- **NR4**: Today's range is the **narrowest of the last 4 bars**
- **NR7**: Today's range is the **narrowest of the last 7 bars**
- Both are due to Toby Crabel, *Day Trading with Short Term Price Patterns and Opening
  Range Breakout* (1990) — sourced separately; Street Smarts integrates but does not define

**Entry**: On an NR4/NR7 day, place buy stop above the range AND sell stop below the
range simultaneously (opening range breakout variant). Enter in whichever direction price
first breaks. Can cancel the opposing stop on fill.

**Exit**: Trail stop or time-based (intraday or day +1).

**Filter**: Historical volatility (HV) below a threshold confirms compression is
statistically significant. Raschke's chapter combines HV measurements with NR4/NR7.

---

## Pattern 8: 2-Period ROC

**Type**: Counter-trend / Taylor cycle positioning.

```
ROC_2 = close[today] - close[2 days ago]
```
- ROC flips negative → positive: Buy Day signal (enter long on close)
- ROC flips positive → negative: Sell Day signal (enter short on close)
- Exit on next day's close

---

## Pattern 9: 3-Day Unfilled Gap Reversal

**Type**: Climax reversal.

**Setup (Long)**
1. Market gaps down and does not fill the gap on the gap day
2. Gap remains unfilled for **3 trading sessions**

**Entry**: Keep buy stop **one tick above the high of the gap-down day** working across
all three sessions. Take entry if triggered. Cancel if not filled within 3 sessions.

**Stop**: Low of the gap-down day.

---

## Failure Modes

**FM-01: Trading a Turtle Soup without minimum session separation** — Consecutive 20-bar
lows within 1–2 bars of each other are not valid setups. The ≥3 session separation rule
filters lows that are part of the same sustained trend impulse, not genuine breakout failures.

**FM-02: Entering Momentum Pinball before the first 60-minute bar closes** — The setup
fires on prior-day LBR/RSI; the trigger requires the first intraday bar. Triggering directly
at the open bypasses the first-hour range filter and widens the stop arbitrarily.

**FM-03: Trading Holy Grail on second or third pullback to EMA** — Raschke is explicit:
only the first pullback after ADX exceeds 30 has the stated edge. Subsequent pulls have
diminishing odds as the trend matures.

**FM-04: Applying Momentum Pinball in low-volatility environments** — The strategy requires
meaningful average daily range. During compression, the first-hour stop is too wide relative
to expected follow-through and the edge degrades.

---

## References
- Raschke, L.B., Connors, L.A. *Street Smarts: High Probability Short-Term Trading
  Strategies.* M. Gordon Publishing Group, 1995. ISBN 978-0965046107
- Crabel, T. *Day Trading with Short Term Price Patterns and Opening Range Breakout.* 1990
  (NR4/NR7 originator; out of print — see Raschke chapter IV for integration summary)
- Pruitt, G. EasyLanguage implementation of Momentum Pinball: georgepruitt.com
- MQL5 pattern articles: Turtle Soup (2717), Momentum Pinball (2825), 80-20 (2785)
- [Oxford Strat — Turtle Soup Plus One](https://oxfordstrat.com/trading-strategies/turtle-soup-plus-1/)
- [WHSelf — Momentum Pinball indicator](https://www.whselfinvest.com/en-lu/trading-platform/free-trading-strategies/tradingsystem/48-momentum-pinball-trading-indicator-raschke-linda-connors-larry)
