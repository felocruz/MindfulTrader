---
domain: sierra_chart/screen3_patterns
intent: NR7/NR4 volatility-compression breakout — Street Smarts (Raschke/Connors) version is the operative Screen 3 entry; Crabel original documented for provenance and Stretch mechanics
scope: global
tags: [crabel, NR7, NR4, inside-day, stretch, opening-range-breakout, ORB, screen3, volatility-compression, TS3, street-smarts, HV-ratio]
source_files:
  - include/Indicator.h
  - src/SCStudies.cpp
last_verified: 2026-06-27
dependencies: [elder_triple_screen, street_smarts_patterns]
---

# NR7 / NR4 — Street Smarts Operative Version (Crabel Origin)

## Why This Exists
`NR7 = 25` in `IndicatorKey` uses the **Street Smarts (Raschke & Connors, 1995)
adaptation** of Crabel's NR7 — not Crabel's original Stretch/ORB mechanic. The
Street Smarts version is the operative entry rule for the Elder-Raschke Confluence
System because its trigger (prior bar high/low ± 1 tick) is bar-close-driven and
integrates naturally with the ACSIL study cycle. Crabel's original Stretch mechanic
requires live opening-price capture and belongs to a standalone day-trading system;
it is documented here for provenance and to explain why the setup condition works.

---

## The Invariant / Contract

**NR7 predicts volatility expansion, not direction.** The setup is bilateral by
default — both a buy stop and a sell stop are placed simultaneously. Direction is
resolved by whichever stop triggers first. Do not pre-select direction from the NR7
condition alone; use Screen 1/2 alignment from the Triple Screen hierarchy to bias
direction within the Elder-Raschke Confluence System.

---

## Pattern Definitions

### NR7 (Narrowest Range 7)
```
range[i] = High[i] - Low[i]          // absolute intrabar range only; no true range
NR7 = range[0] < min(range[1..6])    // strictly less than — ties do NOT qualify
```
Today's bar must be the unique minimum of the 7-bar window. If today's range equals
any prior bar's range exactly, it does NOT qualify. Use `<`, not `<=`.

### NR4 (Narrowest Range 4)
```
NR4 = range[0] < min(range[1..3])
```
Identical logic, 4-bar lookback. Fires more frequently than NR7; lower-conviction
compression signal. All other rules are identical.

### Inside Day (ID)
```
ID = High[0] < High[1] AND Low[0] > Low[1]
```
Today's bar is 100% contained within the prior bar's range on both sides. Orthogonal
to NR7 — a bar can be NR7 without being an ID (protrudes outside yesterday's range
while still being the narrowest of 7), and vice versa.

### ID/NR4 Compound
Both conditions fire simultaneously: today is an Inside Day AND an NR4. This is the
highest-conviction compression setup — two independent structural tests passed at once.
Named as a standalone pattern in *Street Smarts*; underlying components from Crabel.
**Significantly rarer than NR7 alone** — higher quality per setup, lower frequency.

---

## Entry Mechanics — Two Versions

### Version A: Crabel Original (Opening Range Breakout + Stretch)

**The Stretch** (calculated fresh each morning):
```
For each of the prior 10 trading sessions:
    daily_value[i] = MIN( |Open[i] - High[i]|, |Open[i] - Low[i]| )
Stretch = average(daily_value[1..10])
```
The Stretch is the average distance from the open to the nearer extreme — the typical
"first move" size for that instrument. It is instrument-specific and recalculates daily.

**Bracket orders placed at or immediately after the open:**
```
Buy stop:  Today's Open + Stretch
Sell stop: Today's Open − Stretch
```
Both orders live simultaneously. The first to fill becomes the active trade. The
unfilled order automatically becomes the initial protective stop.

**What the "opening range" means in Crabel's original:** A micro-range of the first
30 seconds to 5 minutes of the session. The Stretch mechanism operationalizes this —
it is not a time-based first-bar range. Modern 15–30 minute ORB approaches are later
community adaptations.

### Version B: Street Smarts Adaptation (Prior Bar High/Low)

Raschke & Connors drop the Stretch and simplify to:
```
Buy stop:  NR7 bar's High + 1 tick
Sell stop: NR7 bar's Low  − 1 tick
```
Orders placed the session after the NR7 bar closes. The unfilled stop is the initial
protective stop (symmetric by construction).

**Street Smarts also adds an HV ratio filter (the key Chapter 20 contribution):**
```
HV6   = 6-day historical volatility of log returns (short-term)
HV100 = 100-day historical volatility of log returns (long-term baseline)

Signal requires: NR7 fires AND HV6 / HV100 ≤ 0.5
```
Short-term volatility must be compressed to half the long-run baseline. This eliminates
NR7 days occurring during periods of structurally low volatility where a "narrow bar"
may not be unusual in absolute terms.

**For MindfulTrader / Elder-Raschke Confluence System:** The Street Smarts version
(prior bar high/low trigger) integrates more naturally as a Screen 3 entry since the
trigger aligns with the bar-close timing of the ACSIL study cycle. The Stretch version
requires intraday open-price awareness.

---

## Stop Placement

**Crabel original:** Unfilled bracket leg = `Open ∓ Stretch`. Symmetric.

**Street Smarts:** Unfilled bracket leg = NR7 bar's other extreme ± 1 tick. Symmetric.

**MindfulTrader:** The Chandelier Stop (`ChandelierStopManager`) overrides both with
ATR-based trailing stops conditioned on HMM regime state (3×–4× ATR, widened by
`DofStopScale()` when Student-t DOF is low). Pattern-specific stop rules from Crabel
or Street Smarts do not govern live stop management.

---

## Exit Rules (Crabel Original)

| Exit mode | Rule |
|---|---|
| End-of-session | Market-on-close (MOC) order — same trading day |
| First profitable close | Exit on first daily close showing a profit |
| Break-even stop | Move stop to break-even within ~1 hour of entry |
| Protective stop hit | Unfilled bracket leg |

**Crabel explicitly warns against overnight holding.** Trades filled late in the day
carry maximum risk; the NR7/ORB edge is a same-session open-to-close phenomenon.

---

## Directional Bias

**Default: bilateral, no pre-entry directional filter.** NR7 predicts expansion, not
direction. Crabel's named exception is the **ORBP (Opening Range Breakout Preference)**
— when a strong pre-existing directional bias exists before the open, only one side of
the bracket is placed. ORBP is a discretionary override, not a systematic filter.

**Street Smarts behavioral rule:** "Switch to breakout mode the day after the NR7 forms.
Do not counter-trend trade." Whichever direction triggers, follow it. This is a
discipline rule, not a mathematical pre-entry screen.

**Within the Elder-Raschke Confluence System:** Screen 1 and Screen 2 alignment provides
the directional bias. On an NR7 day, only the bracket leg that agrees with the Screen 1
tide is placed (equivalent to ORBP). This integrates Crabel's compression signal into the
Triple Screen hierarchy without abandoning the tide filter.

---

## Historical Validation

| Study | Instrument | Period | Result |
|---|---|---|---|
| Crabel (1990) — original tables | 42 futures markets | ~1980–1989 | Win rate claimed 60–76%; primary source unverifiable (book out of print) |
| QuantifiedStrategies — bare NR7 | SPY ETF | 1993–2023 | 899 trades; avg gain 0.27% per trade; CAGR 7.8% — edge present but modest |
| QuantifiedStrategies — NR7 + 1 filter | SPY ETF | 1993–2023 | ~76% win rate; profit factor 2.35; avg gain 0.46% — filter matters |
| Bulkowski | 1,201 stocks | 1990–2013 | 57% upward breakout rate in bull markets |
| Oxford Strategy — 42 futures | 42 futures | 1980–present | Edge confirmed; NR_Length ≥ 6 and longer holds improve results; raw NR7 not independently tradeable at realistic transaction costs without supplemental filters |

**Conservative prior:** 57% directional accuracy (Bulkowski stocks), 0.27% average gain
per trade (QuantifiedStrategies bare, 899 trades). The widely-cited 76% figure belongs
to an enhanced version with an additional filter, not raw NR7.

---

## Failure Modes

**FM-01: Using `<=` instead of `<` for range comparison** — Ties do not qualify.
A bar equal to the prior minimum is not a compression extreme; it is neutral. Use strict
less-than throughout.

**FM-02: Trading NR7 without directional context in the Confluence System** — Crabel's
system is standalone bilateral; the Elder-Raschke system requires Screen 1/2 alignment
first. Placing both bracket legs ignores the tide and violates the system's screening
hierarchy.

**FM-03: Using Crabel's Stretch entry in an ACSIL context without intraday open price** —
The Stretch requires today's opening price at session start. If the study runs on bar
close and the opening price isn't captured separately, the Stretch bracket cannot be
computed. The Street Smarts version (prior bar high/low) sidesteps this.

**FM-04: Treating NR7 as overnight signal** — Crabel explicitly designed the edge around
same-session breakout. Entering on next-day open from an NR7 condition without the ORB
mechanism imports the bar geometry without the timing discipline that gives it edge.

---

## References
- Crabel, T. *Day Trading with Short Term Price Patterns and Opening Range Breakout.*
  1990. (Out of print; PDF: buysidedigest.com/wp-content/uploads/2024/08/Day-trading-with-short-term-price-patterns-Tony-crabel.pdf)
- Raschke, L.B., Connors, L.A. *Street Smarts.* M. Gordon Publishing, 1995. Ch. 19–20
  (Range Contraction + Historical Volatility Meets Toby Crabel)
- Crabel, T. "Playing the Opening Range." *Stocks & Commodities* V.6:9 (1988)
  https://store.traders.com/-v06-c09-playing-pdf.html
- QuantifiedStrategies — NR7 backtest: https://www.quantifiedstrategies.com/nr7-trading-strategy-toby-crabel/
- StockCharts ChartSchool — NR7: https://chartschool.stockcharts.com/table-of-contents/trading-strategies-and-models/trading-strategies/narrow-range-day-nr7
- Bulkowski — NR7 statistics: https://thepatternsite.com/nr7.html
- Oxford Strategy — NR7 + ORB: https://oxfordstrat.com/trading-strategies/nr7/
- MyPivots — NR4/NR7 definitions: https://www.mypivots.com/dictionary/definition/136/narrow-range-7-nr7
- TradingView Pine Script (Street Smarts Ch.20): https://www.tradingview.com/script/avSZLevJ-Toby-Crabel-s-narrow-range-with-historical-volatility/
