# Linda Raschke Trading Methodology

**Version:** 1.0  
**Date:** December 24, 2025  
**Purpose:** Consolidated reference for Linda Raschke's trading theory, patterns, and methodologies implemented in MindfulTrader

---

## Table of Contents

1. [Overview & Philosophy](#overview--philosophy)
2. [Market Regime Classification](#market-regime-classification)
3. [Session Quality Framework](#session-quality-framework)
4. [The 3-10 Oscillator](#the-3-10-oscillator)
5. [Pattern Detection Methodology](#pattern-detection-methodology)
6. [Overnight Position Management](#overnight-position-management)
7. [Taylor Trading Technique](#taylor-trading-technique)
8. [Exit Strategies](#exit-strategies)
9. [Risk Management Principles](#risk-management-principles)
10. [References](#references)

---

## Overview & Philosophy

### Core Trading Principles

Linda Bradford Raschke's methodology, documented primarily in **"Street Smarts: High Probability Short-Term Trading Strategies"** (1995, co-authored with Laurence Connors), emphasizes:

1. **Pattern Recognition Over Prediction**
   > "We're not trying to predict the future—we're identifying repeating patterns that have statistical edges."

2. **Session Quality Awareness**
   - Trade during high-quality windows (Sweet Spot: 10:30-12:00 ET)
   - Avoid low-quality sessions (Lunch Dead Zone: 12:00-14:00 ET)
   - London session (03:00-04:00 ET) provides unique overnight entry opportunities

3. **Multi-Timeframe Integration**
   - Strategic trend (240-minute) establishes market tide
   - Tactical oscillator (60-minute) identifies setup structure
   - Precise entry (15-minute) times the trigger

4. **Rigorous Risk Management**
   > "Capital preservation first. If you're unsure about holding overnight, close it."

5. **Professional Execution**
   - Use limit orders in the Sweet Spot for better fills
   - Avoid chasing momentum during opening hour chaos
   - Exit positions that don't move in your favor quickly

### The "Raschke Edge"

Raschke's patterns exploit three market inefficiencies:

1. **Amateur Stop Hunts** (Turtle Soup, Two-B)
   - False breakouts trap retail traders
   - Professionals fade the move for quick profits

2. **Mechanical Exhaustion** (Momentum Pinball)
   - Oscillator extremes + momentum shift = mean reversion
   - 2-3 day cycle exit prevents trend resumption risk

3. **Compression Releases** (NR7, Holy Grail)
   - Narrow range = coiled spring
   - Breakout with trend confirmation = explosive move

---

## Market Regime Classification

### The Five Regimes (Raschke Framework)

Based on **ADX (trend strength)** and **ATR (volatility)**, markets operate in five distinct regimes:

| Regime | ADX | ATR | Characteristics | Best Strategies |
|--------|-----|-----|----------------|-----------------|
| **TRENDING_STRONG** | >30 | High | Persistent directional movement | Holy Grail, trend-following |
| **TRENDING_IMPULSE** | 20-30 | Expanding | Sharp momentum thrust | Breakout trades, momentum |
| **RANGE_DAY** | <20 | Bounded | Oscillating between levels | Turtle Soup, mean-reversion |
| **CONSOLIDATING_CHOP** | <20 | Contracting | Narrow range, low volatility | NR7 compression (wait for breakout) |
| **EXTREME_DISLOCATION** | Any | Extreme | Panic/euphoria (NH-NL extremes) | Capitulation trades |

### ADX: Trend Strength Measure

**Calculation:** Average Directional Movement over 14 periods

**Raschke Thresholds:**
- **ADX > 30:** Strong trend—trade with trend, use Holy Grail setups
- **ADX 20-30:** Impulse move—breakouts work, but tighten stops
- **ADX < 20:** Range-bound—fade extremes with Turtle Soup, avoid trend trades

**Key Insight:**
> "ADX doesn't tell you direction—it tells you whether the market has trend conviction. In choppy markets (ADX < 20), the only edge is fading false breakouts."

### ATR: Volatility Context

**Purpose:** Normalize stop distances and identify regime shifts

**Usage:**
- Stop placement: 0.4-0.6 ATR for mean-reversion patterns
- Regime detection: ATR spike = volatility expansion (avoid entries)
- Position sizing: Scale contracts inversely with ATR

---

## Session Quality Framework

### The Eight Trading Windows (RTH)

Based on **institutional participation** and **volatility characteristics**:

| Window | Time (ET) | Quality | Raschke Guidance |
|--------|-----------|---------|------------------|
| **Pre-Market** | 09:00-09:30 | N/A | Exit overnight gaps here |
| **Opening Hour** | 09:30-10:30 | GOOD | High volatility, trend establishment—requires confirmation |
| **Sweet Spot** | 10:30-12:00 | EXCELLENT | Cleanest trends, best entries, professional participation |
| **Lunch Dead Zone** | 12:00-14:00 | POOR | Choppy, low volume—AVOID new entries |
| **Afternoon Session** | 14:00-15:00 | GOOD | Second chance setups, shorter time window |
| **Final Hour** | 15:00-15:45 | VERY POOR | Position squaring, high noise—avoid new entries |
| **PM Run Entry** | 15:45-16:00 | CONDITIONAL | Only if IMMEDIATE profit expected by 16:00 close |
| **After Hours** | 16:00-18:00 | POOR | Low liquidity, evaluate overnight hold criteria |

### Overnight/Globex Windows

| Window | Time (ET) | Purpose | Entry Opportunity |
|--------|-----------|---------|-------------------|
| **Asian Session** | 18:00-03:00 | Range formation | Monitor, avoid entries (thin liquidity) |
| **London Window** | 03:00-04:00 | European open | **"London Maneuver"** - false breakout entries |
| **London-to-PreMarket** | 04:00-08:30 | Pre-data positioning | Build toward US open |
| **Pre-Market Hook** | 08:30-09:00 | Economic data reaction | **"Hook" pattern** - test and reject |

### Raschke's Session Rules

1. **Only enter during Opening Hour, Sweet Spot, or Afternoon Session**
2. **Sweet Spot provides best fill prices** (use limit orders)
3. **London Window is for overnight entries only** (requires 3-10 oscillator setup)
4. **Never enter during Lunch Dead Zone** (highest failure rate)
5. **PM Run entries must profit by 16:00** (or scratch immediately)

---

## The 3-10 Oscillator

### Theory & Calculation

The **3-10 Oscillator** is Raschke's version of MACD—the "heartbeat" of her trade management.

**Components:**
```
Fast Line = 3-period EMA - 16-period EMA  (NOT 10-period, despite the name)
Slow Line = 16-period SMA of Fast Line
```

**Why These Periods?**
- **3-period EMA:** Immediate momentum
- **16-period EMA:** Short-term average (NOT 10—this is historical naming)
- **16-period SMA of Fast:** Represents **2-3 day market cycle**

### The Three Critical Uses

#### 1. First Cross Entry (The "Anti" Pattern)

**Setup:** Overnight pullback to support/resistance during Globex
**Trigger:** Fast Line "hooks" back across Slow Line during London session (03:00-04:00 ET)

> "The Anti is the most reliable intraday setup" — Raschke, "Street Smarts"

**Logic:**
- Market thrust creates momentum (Fast Line > Slow Line)
- Overnight pullback brings Fast Line back toward Slow Line
- London open provides liquidity for resumption
- Fast Line hooks back = trend resuming after healthy pullback

**Entry:** Close of 15-minute bar where Fast Line crosses back over Slow Line
**Stop:** Below pullback low (LONG) or above pullback high (SHORT), typically 3-5 points
**Target:** Previous day high (LONG) or low (SHORT)

#### 2. Divergence Exit (Gap Exhaustion)

**Setup:** Position held overnight, market gaps in favor at 09:30 open
**Detection:** Price makes new high but Fast Line lower than prior peak

**Logic:**
- Gap provides windfall profit opportunity
- Momentum exhaustion (oscillator divergence) = unsustainable
- Exit during first 15 minutes (09:30-09:45) before reversal

> "The biggest mistake is waiting for your stop to get hit in the morning. If momentum has reversed during Globex, exit on first opportunity."

#### 3. Slow Line Exit (Cycle Rotation)

**Setup:** Position held 2-3 days
**Detection:** Slow Line flattens or changes slope

**Logic:**
- Slow Line represents 2-3 day cycle direction
- Flattening = cycle rotating from Buy Day → Sell Day → Short Day
- Exit before cycle completes rotation (don't wait for reversal)

**Timeframes:**
- **15-minute:** Overnight hooks and London entries
- **5-minute:** Gap divergence exits at open (more sensitive)
- **60-minute:** Multi-day swing trades

---

## Pattern Detection Methodology

### Pattern Classification System

Raschke's 21 patterns are organized by **market structure** and **timeframe**:

#### Compression Patterns (Pre-Breakout)
1. **NR7** - Narrow Range 7 (7-bar range compression)
2. **Inside Day** - Range completely inside prior day
3. **Consolidation** - 3+ bars with tight high-low range

#### Reversal Patterns (Trend Change)
4. **Turtle Soup** - False breakout of 4-day high/low
5. **Two-B** - Double top/bottom failure at swing extreme
6. **Kangaroo Tail** - Within-bar rejection (long tail)
7. **Momentum Pinball** - RSI cross + Stochastic extreme

#### Trend Continuation Patterns
8. **Holy Grail** - Breakout during strong trend (ADX > 30)
9. **Double Repo** - Pullback to EMA in strong trend
10. **Double Repo Failure** - Failed pullback = stronger continuation

#### Special Patterns
11. **Ghost** - Hidden divergence (MACD vs price)
12. **Slingshot** - Sharp V-reversal from oversold/overbought
13. **First Cross** - 3-10 oscillator cross after thrust

### Key Pattern Details

#### Turtle Soup

**Theory:** Stop hunt of amateur traders who buy breakouts

**Setup:**
- 4-day high (for SHORT) or 4-day low (for LONG) identified
- Price breaks beyond extreme by small amount
- Fails to close beyond the extreme (trap)

**Entry:**
- SHORT: Price breaks above 4-day high but closes back inside range
- LONG: Price breaks below 4-day low but closes back inside range

**Stop:** Beyond the false breakout extreme + 0.5 ATR buffer
**Target:** Opposite side of 4-day range (1.5-2R typically)

**Raschke Quote:**
> "The Turtle Soup pattern exploits the amateur's compulsion to buy breakouts. When they get trapped, we fade the move for quick profit."

**Why 4 Days?**
- "Street Smarts" Chapter 8 specifies 4-day lookback explicitly
- Captures weekly high/low (4 trading days ≈ 1 week)
- Long enough to be significant, short enough to be tradeable

#### Momentum Pinball

**Theory:** Mechanical oscillator exhaustion + early momentum shift

**Setup:**
- Stochastic < 20 (oversold) or > 80 (overbought)
- RSI(3) crosses above RSI(10) for LONG (or below for SHORT)
- Indicates momentum shift BEFORE price reversal

**Entry:** Close of bar where RSI(3) > RSI(10) AND Stochastic extreme
**Stop:** Below swing low - 0.4 ATR (tight stop, mean-reversion pattern)
**Target:** Day 2-3 exit (Taylor Trading 2-day cycle), typically 1-1.5R

**Raschke Insight:**
> "Pinball is NOT a full reversal—it's a 2-3 day bounce. Exit by Day 3 or the trend resumes."

**Why It Works:**
- Catches early phase of mean reversion
- RSI cross precedes price reversal (leading indicator)
- Short holding period (2-3 days) captures bounce without trend resumption risk

#### Kangaroo Tail

**Theory:** Within-bar rejection shows failed attempt to move price

**Setup:**
- Bar with long tail (wick) relative to body
- Tail = 2× body size minimum
- Close in opposite half of range (LONG: upper half, SHORT: lower half)

**Detection:**
```
Tail Length = |Low - Close| for LONG or |High - Close| for SHORT
Body Size = |Open - Close|
Kangaroo = Tail Length >= 2.0 × Body Size
```

**Entry:** Market open next bar (15-minute chart)
**Stop:** Beyond the tail extreme + 0.4 ATR
**Target:** Recent swing high/low (1.5-2R)

**Visual Example (LONG):**
```
High ────┐
         │  Body (small)
Close ───┤
         │
         │  TAIL (long rejection)
Low ─────┘
```

#### Holy Grail

**Theory:** Breakout in strong trend with pullback to moving average

**Setup:**
- ADX > 30 (strong trend)
- Price pulls back to 20-period EMA
- Price breaks prior swing high (LONG) or low (SHORT)

**Entry:** Close of breakout bar
**Stop:** Below EMA - 0.6 ATR (slightly wider for trend pattern)
**Target:** Measured move or previous major resistance (2-3R)

**Why ADX > 30?**
> "Breakouts work in trends, fail in ranges. ADX > 30 ensures trend conviction."

#### NR7 (Narrow Range 7)

**Theory:** Compression pattern—coiled spring before breakout

**Setup:**
- Current bar has narrowest high-low range of last 7 bars
- Indicates volatility contraction (calm before storm)

**Entry:** Breakout of NR7 bar high (LONG) or low (SHORT) next bar
**Stop:** Opposite side of NR7 bar + 0.5 ATR
**Target:** Measured move = NR7 range × 3 (typical expansion)

**Key Insight:**
- NR7 doesn't predict direction—it predicts expansion
- Combine with trend context (Screen 1) for directional bias

---

## Overnight Position Management

### The Golden Rule for Overnight

A position is ONLY qualified to hold overnight if **ALL** conditions are met:

1. ✅ **Position is in profit** by 16:00 ET close
2. ✅ **Close is in top 25% (LONG) or bottom 25% (SHORT) of daily range** ("Strong Close")
3. ✅ **If entered in PM Run (15:45-16:00), must show IMMEDIATE profit** by 16:00
4. ✅ **Screen 1 (240-min) trend is strong** (3+ bars of bullish/bearish Impulse)
5. ✅ **Stop is at breakeven or better** (no overnight risk to principal)

**If ANY condition fails → SCRATCH_AT_CLOSE** (exit flat by 15:55, don't hope for gap)

### Strong Close Definition

```
Daily Range = Daily High - Daily Low
Range Position = (Close - Daily Low) / Daily Range

LONG: Close >= Daily Low + 0.75 × Range  (top 25%)
SHORT: Close <= Daily High - 0.75 × Range  (bottom 25%)
```

**Visual Example (LONG):**
```
Daily High: 4200.00      ← Top 25% (Strong Close Zone for LONG)
75% Level:  4185.00      ───────────
                         
50% Level:  4170.00      ← Middle (Weak Close)
                         
25% Level:  4155.00      ───────────
Daily Low:  4140.00      ← Bottom 25% (Strong Close Zone for SHORT)

LONG Close at 4190 → Range Position = 0.83 → ✅ PASS (top 25%)
LONG Close at 4175 → Range Position = 0.58 → ❌ FAIL (middle)
```

### PM Run Entry Special Rule

**Window:** 15:45-16:00 ET

**Rule:** Entries during PM Run are acceptable ONLY if:
- Position is profitable by 16:00 close
- If flat or negative at close → **SCRATCH** (don't gamble on overnight gap)

**Raschke Philosophy:**
> "Late-day entries must work immediately. If you're unsure at the close, exit—capital preservation first."

---

## Taylor Trading Technique

### George Taylor's 3-Day Cycle

George Taylor (1950s) observed markets move in **3-day cycles**:

1. **Buy Day** - Market finds support, forms base
2. **Sell Day** - Rally to resistance, longs exit
3. **Short Sale Day** - Breakdown from resistance, shorts enter

**Raschke Adaptation:** Use objective points (previous day high/low) as cycle targets

### Overnight Exit Classification

Once a position holds overnight (passed Golden Rule), next morning exit is determined by:

#### 1. Gap Exit (Windfall)

**Detection:** Open ≥ 0.5R in favor (significant gap)
**Action:** Exit 09:30-09:45 ET (first 15 minutes)
**Rationale:** Take the windfall, don't be greedy

#### 2. First Reaction Exit

**Detection:** Flat or adverse open (gap < 0.3R in favor or against)
**Action:** Wait for first bounce/rejection (09:30-10:00 ET)
**Rationale:** Let position show hand before exiting

#### 3. Objective Point Exit (Taylor Target)

**Detection:** Price approaches previous day high (LONG) or low (SHORT)
**Action:** Exit during Sweet Spot (10:30-11:00 ET) at liquidity window
**Rationale:** Natural profit-taking zone, resistance/support level

**Taylor Insight:**
- Previous day high = supply zone (LONG target)
- Previous day low = demand zone (SHORT target)
- These are natural inflection points where positions reverse

#### 4. Momentum Failure Exit

**Detection:** 3-10 Oscillator crossed zero during Globex (momentum shift)
**Action:** Exit pre-market (08:30-09:00) or first 30 minutes of RTH
**Rationale:** Momentum failed overnight, trend reversing

#### 5. Scratch Exit

**Detection:** Position did NOT meet Golden Rule at 16:00 close
**Action:** Exit immediately at 09:30 open (no evaluation)
**Rationale:** Position was not qualified, don't hope for recovery

### Exit Windows Summary

| Exit Type | Time Window | Trigger |
|-----------|-------------|---------|
| **Gap Exit** | 09:30-09:45 | Gap ≥ 0.5R windfall |
| **First Reaction** | 09:30-10:00 | Flat/adverse open, wait for bounce |
| **Objective Point** | 10:30-11:00 | Approaching prev day H/L |
| **Momentum Failure** | 08:30-09:30 | 3-10 oscillator crossed during Globex |
| **Scratch** | 09:30 | Failed Golden Rule (immediate exit) |

---

## Exit Strategies

### The Taylor Trading 2-Day Cycle (Momentum Pinball)

**Problem:** Momentum Pinball is a counter-trend pattern—if held too long, trend resumes

**Solution:** Exit by Day 2-3 regardless of profit target

**Exit Rules:**
1. **Day 1 (Entry Day):** Hold if profitable, scratch if flat by close
2. **Day 2 Morning:** Primary exit window (1-1.5R typical)
3. **Day 3 Close:** Hard exit deadline (2R maximum)

**Rationale:**
- Pinball captures **mechanical exhaustion bounce** (not full reversal)
- 2-3 day cycle is mean-reversion period
- Beyond Day 3, trend resumption risk increases sharply

**Raschke Quote:**
> "Pinball is a 65-70% win rate pattern because we exit early. The time-based exit is more important than the price target."

### Trend Continuation Exit Rules

**Holy Grail, Double Repo:**
- Trail stop below 20-EMA (moving average acts as dynamic support)
- Exit if ADX drops below 25 (trend weakening)
- Target: 2-3R or major resistance level

**Double Repo Failure:**
- Higher target: 3R (failed pullback = strong trend continuation)
- Trail stop more aggressively (1 ATR below swing low)
- This is the strongest continuation signal

### Compression Pattern Exits

**NR7:**
- Target: Measured move = NR7 range × 3
- Stop: Opposite side of NR7 bar + 0.5 ATR
- Exit if expansion stalls (second narrow range bar)

---

## Risk Management Principles

### Position Sizing

**Base Rule:** Risk 1-2% of account per trade

**Pattern-Specific Adjustments:**
- **High-probability setups** (Holy Grail, Anti): 2% risk
- **Counter-trend patterns** (Turtle Soup, Pinball): 1% risk
- **Compression patterns** (NR7): 1.5% risk (direction unknown)

### Stop Placement by Pattern

| Pattern | Stop Distance | Rationale |
|---------|--------------|-----------|
| **Turtle Soup** | 0.5 ATR beyond false breakout | Tight, mean-reversion |
| **Momentum Pinball** | 0.4 ATR below swing low | Tightest stop (2-3 day hold) |
| **Holy Grail** | 0.6 ATR below EMA | Wider for trend breathing room |
| **NR7** | 0.5 ATR beyond NR7 bar | Standard compression stop |
| **Kangaroo Tail** | 0.4 ATR beyond tail | Tight rejection stop |

### Raschke's Stop Loss Philosophy

1. **Never move stop closer to entry** (let pattern breathe)
2. **Move to breakeven after 1R profit** (eliminate risk)
3. **Trail stops in trends** (let profits run)
4. **Scratch immediately if setup invalidated** (don't hope)

> "A stop loss is an insurance premium. Pay it gladly when wrong."

### Calendar Risk Management

**Friday Positions:**
- MUST close by 16:00 (no weekend risk)
- Tighten stops after 15:00 (don't enter new positions)

**Economic Events:**
- No new entries 30 minutes before major reports (Fed, NFP, CPI)
- Exit or tighten stops before known catalysts

---

## References

### Primary Sources

1. **"Street Smarts: High Probability Short-Term Trading Strategies"**  
   Linda Bradford Raschke and Laurence Connors (1995)
   - Chapter 5: Using Oscillators in Mean-Reversion
   - Chapter 8: The Turtle Soup Pattern
   - Chapter 12: Overnight Management

2. **Linda Raschke Professional Trading Courses**
   - Session quality framework
   - 3-10 Oscillator methodology
   - London Maneuver and Pre-Market Hook

3. **George Taylor's "Taylor Trading Technique"** (1950s)
   - 3-day market cycle (Buy/Sell/Short Day)
   - Objective points (previous day high/low)

### Implementation Documentation

For technical implementation details, see:

- **[ENUM_REFERENCE.md](ENUM_REFERENCE.md)** - Complete enum documentation with algorithms
  - RaschkeStrategySetup (21 patterns)
  - RaschkeTacticalTrigger (10 entry triggers)
  - TimeOfDayEnum (13 session states)
  - HoldingStrategyEnum (6 overnight decision states)
  - OvernightExitTypeEnum (10 Taylor exit types)

- **[OVERNIGHT_MANAGEMENT_RASCHKE_TAYLOR.md](OVERNIGHT_MANAGEMENT_RASCHKE_TAYLOR.md)** - Golden Rule specification

- **[OVERNIGHT_WINDOWS_QUICK_REFERENCE.md](OVERNIGHT_WINDOWS_QUICK_REFERENCE.md)** - 24-hour timeline visual reference

- **[GUI_INDICATOR_REFERENCE.md](GUI_INDICATOR_REFERENCE.md)** - GUI indicator mappings

- **[STRATEGIES_PARAMETERS_REFERENCE.md](STRATEGIES_PARAMETERS_REFERENCE.md)** - All parameters with sources

---

## Trading Philosophy Summary

### The Raschke Approach in Three Principles

1. **Trade Patterns, Not Predictions**
   - Markets repeat behavioral patterns
   - Identify high-probability setups with statistical edges
   - Let the pattern develop fully before entering

2. **Respect Session Quality**
   - Best trades occur during Sweet Spot (10:30-12:00 ET)
   - Avoid choppy sessions (Lunch Dead Zone)
   - London session provides unique overnight opportunities

3. **Manage Risk Ruthlessly**
   - Capital preservation > being right
   - Scratch positions that don't work immediately
   - Use time-based exits for mean-reversion patterns
   - Only hold overnight if ALL Golden Rule criteria met

> "Trading is not about being right—it's about making money when right and losing small when wrong."  
> — Linda Raschke

---

**Document End**

*For implementation details, code examples, and algorithms, see ENUM_REFERENCE.md*  
*For overnight management rules and Golden Rule specification, see OVERNIGHT_MANAGEMENT_RASCHKE_TAYLOR.md*
