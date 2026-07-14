# Dr. Alexander Elder's Trading Methodology
## Comprehensive Technical Specification for MindfulTrader

**Sources**: 
- "Two Roads Diverged: Trading Divergences" (2012-2014)
- "The New High – New Low Index" (2nd Revised Edition, 2014)
- "Trading for a Living" (Triple Screen System)

**Status**: Complete methodology consolidation  
**Date**: December 2025

---

## Table of Contents

1. [Core Philosophy & Triple Screen System](#1-core-philosophy--triple-screen-system)
2. [Trade Grading System](#2-trade-grading-system)
3. [MACD Divergence Detection](#3-macd-divergence-detection)
4. [NH-NL Breadth Analysis](#4-nh-nl-breadth-analysis)
5. [Integration Guidelines](#5-integration-guidelines)
6. [Implementation Status](#6-implementation-status)
7. [References](#7-references)

---

## 1. Core Philosophy & Triple Screen System

### 1.1 Elder's Market Approach

> "When trying to find a divergence, first look at the pattern of an indicator and later at the pattern of prices."  
> — Dr. Alexander Elder

**Fundamental Principle**: A divergence is a **disagreement between the patterns of indicators and prices**, signaling potential reversals before they become obvious to the crowd.

### 1.2 Triple Screen System Overview

Elder's Triple Screen System (from "Trading for a Living") provides a three-tiered approach to trading:

**Screen 1 (Weekly):** Identify long-term trend
- Use weekly indicators (NH-NL, MACD) to determine market regime
- Weekly NH-NL > +2,500 → Bull market confirmed
- Weekly NH-NL divergences → Major trend reversals
- Weekly NH-NL spikes < -4,000 → Extreme buying opportunities

**Screen 2 (Daily):** Identify pullbacks/rallies against main trend
- Use daily indicators to find counter-trend opportunities
- In uptrend: wait for daily NH-NL to dip for long entries
- In downtrend: wait for daily NH-NL to rally for short entries

**Screen 3 (Intraday):** Execute entries using precise triggers
- Elder Breakout or Momentum Pinball setups
- **Elder Breakout Buy:** In uptrend, buy breakout above previous bar's high
- **Elder Breakout Sell:** In downtrend, sell breakdown below previous bar's low

---

## 2. Trade Grading System

**Source:** "Come Into My Trading Room" by Dr. Alexander Elder

### 2.1 Overview

Alexander Elder defines a quantitative grading scale to evaluate the **quality of a trade's execution**. This system separates the **quality of the process** from the **monetary outcome** of the trade.

The system centers on two main metrics:
1. **Channel Capture Grade (Trade Grade)** - How much of the move you caught
2. **Execution Precision Grades** - How well you timed entry and exit

### 2.2 Channel Capture Grade (Trade Grade)

This is Elder's primary tool for measuring how much of a market move you "caught." He uses **Keltner Channels** (typically set at 2.5 or 3.0 ATR around an EMA) to define the boundaries of "normal" price action.

#### Formula

```
Trade Grade = (Exit Price - Entry Price) / Width of the Channel × 100%
```

For **LONG** positions:
```
Trade Grade = (Exit Price - Entry Price) / Channel Width × 100
```

For **SHORT** positions:
```
Trade Grade = (Entry Price - Exit Price) / Channel Width × 100
```

#### The Scale

| Grade | Channel Capture | Quality |
|-------|-----------------|---------|
| **A** | **30% or more** | Excellent - captured substantial portion of move |
| **B** | **20% to 30%** | Good - captured meaningful portion |
| **C** | **10% to 20%** | Acceptable - captured some of the move |
| **D/F** | **Less than 10% or loss** | Poor - minimal capture or losing trade |

#### Key Insights

- **30% is exceptional performance** - This is an A-grade trade
- **20% is good** - Respectable capture of the available move
- **10% is acceptable** - Better than break-even but room for improvement
- Channel width represents the "normal" range of price movement based on recent volatility

### 2.3 Entry Precision Grade

Elder grades how close you came to the **optimal entry price** on the bar (or day) of execution. This measures skill in timing and order placement.

#### Formula (for LONG positions)

```
Entry Grade = (High - Entry Price) / (High - Low) × 100%
```

An **A-grade entry** is in the **bottom 20%** of the bar's range (buying near the low).

#### The Scale (LONG positions)

| Grade | Bar Position | Entry Price Location |
|-------|--------------|---------------------|
| **A** | **0-20%** | Bottom 20% of range (near the low) |
| **B** | **20-40%** | Second quintile |
| **C** | **40-60%** | Middle 20% |
| **D** | **60-80%** | Fourth quintile |
| **F** | **80-100%** | Top 20% of range (near the high) |

For **SHORT positions**, invert the scale - an A-grade short entry is in the **top 20%** of the bar's range (selling near the high):

```
Entry Grade = (Entry Price - Low) / (High - Low) × 100%
```

### 2.4 Exit Precision Grade

Elder grades how close you came to the **optimal exit price** on the bar of execution.

#### Formula (for LONG positions)

```
Exit Grade = (Exit Price - Low) / (High - Low) × 100%
```

An **A-grade exit** is in the **top 20%** of the bar's range (selling near the high).

#### The Scale (LONG positions)

| Grade | Bar Position | Exit Price Location |
|-------|--------------|---------------------|
| **A** | **80-100%** | Top 20% of range (near the high) |
| **B** | **60-80%** | Second quintile |
| **C** | **40-60%** | Middle 20% |
| **D** | **20-40%** | Fourth quintile |
| **F** | **0-20%** | Bottom 20% of range (near the low) |

For **SHORT positions**, invert the scale - an A-grade short exit is in the **bottom 20%** of the bar's range (covering near the low):

```
Exit Grade = (High - Exit Price) / (High - Low) × 100%
```

### 2.5 The "A-Trade" Pre-Entry Scoring System

Elder suggests a **100-point scoring system** to determine if a setup is worth taking **before you enter**. A score above **80/100** qualifies as an **"A-Trade"**.

#### Common Criteria

1. **The Tide** (Trend) - Is price above/below the long-term Moving Average?
   - Example: Above 30-week EMA for bullish bias

2. **The Value Zone** - Is price between the fast and slow EMAs?
   - Example: Between 13-week and 26-week EMA

3. **The Momentum** - Is the MACD-Histogram rising or falling?
   - Rising histogram = bullish momentum
   - Falling histogram = bearish momentum

4. **Additional Factors**:
   - Volume confirmation
   - Support/resistance levels
   - Market regime alignment
   - Risk:reward ratio

#### Scoring Example

| Criterion | Points | Score |
|-----------|--------|-------|
| Price above 30-week EMA | 25 | 25 ✓ |
| In value zone (between EMAs) | 20 | 20 ✓ |
| MACD-Histogram rising | 20 | 20 ✓ |
| Volume above average | 15 | 15 ✓ |
| Near support level | 10 | 10 ✓ |
| Risk:Reward > 3:1 | 10 | 10 ✓ |
| **Total** | **100** | **100** (A-Trade) |

### 2.6 Implementation in MindfulTrader

#### Current Implementation

The `Trade` class in [src/Trade.cpp](src/Trade.cpp) already calculates all three grades:

```cpp
// Entry Grade
m_entry_grade = CalculateGradeValue(m_entry_price - m_entry_low, 
                                    m_entry_high - m_entry_low);

// Exit Grade  
m_exit_grade = CalculateGradeValue(current_exit_price - m_exit_low, 
                                   m_exit_high - m_exit_low);

// Trade Grade (Channel Capture)
m_trade_grade = CalculateGradeValue(current_exit_price - m_entry_price, 
                                    m_channel);
```

Where `CalculateGradeValue()` returns:
```cpp
return (numerator / denominator) * 100.0;
```

#### Active Exit Logic Integration

Using **Elder's actual methodology**, protective actions trigger at:

```cpp
// A-GRADE (30%+): Captured 30% of channel - LOCK IT IN
if (tradeGrade >= 30) {
    ScaleOut50Percent();
    TightenChandelierStop(1.5);
}

// B-GRADE (20%+): Good capture - PROTECT IT  
if (tradeGrade >= 20) {
    MoveStopToBreakevenPlusOneR();
}

// C-GRADE (10%+): Acceptable - SECURE BREAKEVEN
if (tradeGrade >= 10) {
    MoveStopToBreakeven();
}
```

See [docs/TRADE_GRADE_EXIT_INTEGRATION.md](docs/TRADE_GRADE_EXIT_INTEGRATION.md) for full implementation details.

#### Key Takeaways

1. **30% channel capture = A-grade** (NOT 50% or 80%)
2. **20% channel capture = B-grade** (Good trade)
3. **10% channel capture = C-grade** (Acceptable trade)
4. Entry/Exit grades measure **bar positioning** (20% quintiles)
5. Trade grade measures **channel capture** (profit vs available move)
6. The grading system is **process-focused**, not outcome-focused
7. An A-grade trade can still lose money (if stopped out)
8. The goal is consistent A/B grade execution over time

---

## 3. MACD Divergence Detection

### 3.1 Core Principle

A MACD divergence occurs when price and MACD-Histogram move in opposite directions, signaling potential reversals. The **zero-line crossover is mandatory** for valid divergences.

### 3.2 Bullish Divergence (Buy Signal)

#### Formal Definition

**Elder's Definition**: "A bullish divergence occurs when prices trace a bottom, rally, and then sink to a new low. At the same time, MACD-Histogram traces a different pattern. When it rallies from its first bottom, that rally lifts it above the zero line, 'breaking the back of the bear.' When prices sink to a new low, MACD-Histogram declines to a more shallow bottom."

#### Required Conditions (ALL must be true)

```
PRICE PATTERN:
1. Price makes a distinct bottom (trough A)
2. Price rallies from trough A
3. Price declines to a NEW LOWER LOW (trough B)
4. Trough B < Trough A (price lower)

MACD-HISTOGRAM PATTERN:
5. MACD-H makes first bottom (valley A) while price at trough A
6. MACD-H rallies and CROSSES ABOVE ZERO LINE (critical requirement)
7. MACD-H declines to second bottom (valley B) while price at trough B
8. Valley B > Valley A (MACD-H higher, less negative)
9. Valley B must still be BELOW ZERO LINE

BUY SIGNAL:
10. MACD-H ticks up from valley B (uptick = bar less negative than prior bar)
11. Signal fires while MACD-H still below zero (does NOT need to cross zero again)
```

#### Critical Requirements

**Zero-Line Crossover is Mandatory**:
- MACD-Histogram MUST cross above zero between the two bottoms
- This "breaks the back of the bear" — confirms bears are losing control
- WITHOUT zero-line cross → NO divergence (just weakening trend)

**Signal Trigger**:
- Buy signal occurs on first uptick from valley B
- "Uptick" = current MACD-H bar > previous MACD-H bar (less negative)
- Does NOT require crossing zero line again
- Enter immediately on uptick detection

#### State Machine Algorithm

```cpp
enum class DivergenceState {
    SEARCHING_FIRST_TROUGH,        // Looking for first price/MACD low
    WAITING_ZERO_CROSS,            // Need MACD-H to cross above zero
    WAITING_SECOND_TROUGH,         // Looking for second price low
    PENDING_CONFIRMATION,          // Second trough found, waiting for MACD-H uptick
    BULLISH_DIVERGENCE_CONFIRMED   // Buy signal active
};

struct DivergenceTracker {
    // Price tracking
    float priceBottom1 = 0.0f;
    int priceBottom1Index = -1;
    
    // MACD-H tracking
    double macdBottom1 = 0.0;
    int macdBottom1Index = -1;
    
    // State flags
    bool macdCrossedZero = false;
    int zeroCrossIndex = -1;
    
    DivergenceState state = DivergenceState::SEARCHING_FIRST_TROUGH;
};
```

#### Detection Logic (Pseudocode)

```cpp
void UpdateBullishDivergence(int currentIndex, float priceLow, double macdHistogram) {
    
    // PHASE 1: Identify first trough
    if (state == SEARCHING_FIRST_TROUGH) {
        if (macdHistogram < 0 && IsLocalMinimum(macdHistogram)) {
            macdBottom1 = macdHistogram;
            macdBottom1Index = currentIndex;
            priceBottom1 = priceLow;
            priceBottom1Index = currentIndex;
            state = WAITING_ZERO_CROSS;
        }
    }
    
    // PHASE 2: Wait for zero-line crossover
    else if (state == WAITING_ZERO_CROSS) {
        if (macdHistogram >= 0 && macdHistogram[currentIndex-1] < 0) {
            macdCrossedZero = true;
            zeroCrossIndex = currentIndex;
            state = WAITING_SECOND_TROUGH;
        }
        // Reset if new lower low without zero cross
        else if (macdHistogram < macdBottom1) {
            macdBottom1 = macdHistogram;
            macdBottom1Index = currentIndex;
            priceBottom1 = priceLow;
            priceBottom1Index = currentIndex;
            // Stay in WAITING_ZERO_CROSS
        }
    }
    
    // PHASE 3: Look for second trough (price lower, MACD-H higher)
    else if (state == WAITING_SECOND_TROUGH) {
        // Check if MACD-H went back below zero
        if (macdHistogram < 0 && macdHistogram[currentIndex-1] >= 0) {
            // Now we can look for divergence
            if (IsLocalMinimum(macdHistogram) && priceLow < priceBottom1) {
                // Price made lower low
                if (macdHistogram > macdBottom1) {
                    // MACD-H made HIGHER low (divergence detected)
                    state = PENDING_CONFIRMATION;
                }
            }
        }
    }
    
    // PHASE 4: Wait for uptick to confirm
    else if (state == PENDING_CONFIRMATION) {
        if (macdHistogram > macdHistogram[currentIndex-1]) {
            // UPTICK DETECTED - BUY SIGNAL!
            state = BULLISH_DIVERGENCE_CONFIRMED;
            return DivergenceEnum::BULLISH_DIVERGENCE;
        }
    }
}
```

### 3.3 Bearish Divergence (Sell Signal)

#### Formal Definition

**Elder's Definition**: "A bearish divergence is a mirror image of the bullish. Prices rally to a peak, decline, and then rally to a new high. At the same time, MACD-Histogram rallies, falls below the zero line, then rallies to a more shallow peak."

#### Required Conditions (ALL must be true)

```
PRICE PATTERN:
1. Price makes a distinct peak (peak A)
2. Price declines from peak A
3. Price rallies to a NEW HIGHER HIGH (peak B)
4. Peak B > Peak A (price higher)

MACD-HISTOGRAM PATTERN:
5. MACD-H makes first peak (summit A) while price at peak A
6. MACD-H declines and CROSSES BELOW ZERO LINE (critical requirement)
7. MACD-H rallies to second peak (summit B) while price at peak B
8. Summit B < Summit A (MACD-H lower, less positive)
9. Summit B must still be ABOVE ZERO LINE

SELL SIGNAL:
10. MACD-H ticks down from summit B (downtick = bar less positive than prior bar)
11. Signal fires while MACD-H still above zero (does NOT need to cross zero again)
```

#### Critical Requirements

**Zero-Line Crossover is Mandatory**:
- MACD-Histogram MUST cross below zero between the two peaks
- This "breaks the back of the bull" — confirms bulls are losing control
- WITHOUT zero-line cross → NO divergence

**Signal Trigger**:
- Sell signal occurs on first downtick from summit B
- "Downtick" = current MACD-H bar < previous MACD-H bar (less positive)
- Does NOT require crossing zero line again
- Enter short immediately on downtick detection

#### Detection Logic (Pseudocode)

```cpp
void UpdateBearishDivergence(int currentIndex, float priceHigh, double macdHistogram) {
    
    // PHASE 1: Identify first peak
    if (state == SEARCHING_FIRST_PEAK) {
        if (macdHistogram > 0 && IsLocalMaximum(macdHistogram)) {
            macdPeak1 = macdHistogram;
            macdPeak1Index = currentIndex;
            pricePeak1 = priceHigh;
            pricePeak1Index = currentIndex;
            state = WAITING_ZERO_CROSS;
        }
    }
    
    // PHASE 2: Wait for zero-line crossover
    else if (state == WAITING_ZERO_CROSS) {
        if (macdHistogram <= 0 && macdHistogram[currentIndex-1] > 0) {
            macdCrossedZero = true;
            zeroCrossIndex = currentIndex;
            state = WAITING_SECOND_PEAK;
        }
        // Reset if new higher high without zero cross
        else if (macdHistogram > macdPeak1) {
            macdPeak1 = macdHistogram;
            macdPeak1Index = currentIndex;
            pricePeak1 = priceHigh;
            pricePeak1Index = currentIndex;
            // Stay in WAITING_ZERO_CROSS
        }
    }
    
    // PHASE 3: Look for second peak (price higher, MACD-H lower)
    else if (state == WAITING_SECOND_PEAK) {
        // Check if MACD-H went back above zero
        if (macdHistogram > 0 && macdHistogram[currentIndex-1] <= 0) {
            // Now we can look for divergence
            if (IsLocalMaximum(macdHistogram) && priceHigh > pricePeak1) {
                // Price made higher high
                if (macdHistogram < macdPeak1) {
                    // MACD-H made LOWER high (divergence detected)
                    state = PENDING_CONFIRMATION;
                }
            }
        }
    }
    
    // PHASE 4: Wait for downtick to confirm
    else if (state == PENDING_CONFIRMATION) {
        if (macdHistogram < macdHistogram[currentIndex-1]) {
            // DOWNTICK DETECTED - SELL SIGNAL!
            state = BEARISH_DIVERGENCE_CONFIRMED;
            return DivergenceEnum::BEARISH_DIVERGENCE;
        }
    }
}
```

### 2.4 What Is NOT a Divergence

Elder emphasizes these patterns do NOT constitute divergences:

**1. Price drift without discrete bottoms/tops**
- Continuous downtrend with gradually shallower MACD-H lows
- NO rally separating the two price bottoms
- → Just a weakening trend, not a divergence

**2. Missing zero-line crossover**
- Two MACD-H bottoms without crossing above zero between them
- "The bear is getting older and weaker — but the bear is still in charge!"
- → Wait for zero cross before considering divergence

**3. Single price low with multiple MACD-H lows**
- Price makes one low, MACD-H oscillates below zero
- No distinct second price trough
- → Not a divergence pattern

**4. MACD-H crosses zero but price doesn't make second extreme**
- MACD-H crosses zero, but price doesn't make new low/high
- → No divergence opportunity

#### Validation Checklist

```cpp
bool IsValidBullishDivergence() {
    // Must have TWO distinct price lows
    if (!HasTwoDistinctPriceLows()) return false;
    
    // Second price low must be LOWER
    if (priceBottom2 >= priceBottom1) return false;
    
    // MACD-H must have crossed ABOVE zero between bottoms
    if (!macdCrossedZeroUpward) return false;
    
    // Second MACD-H low must be HIGHER (less negative)
    if (macdBottom2 <= macdBottom1) return false;
    
    // Rally must be substantial (not just noise)
    if (zeroCrossIndex - macdBottom1Index < MIN_BARS_BETWEEN) return false;
    
    return true;
}
```

### 2.5 Additional Pattern: MACD-Lines Divergence

Elder notes: "This MACD-Histogram divergence signal was reinforced when MACD Lines traced a bullish pattern between the bottoms A and C, with the second bottom of MACD-Lines more shallow than the first. We rarely see such patterns of MACD Lines. They indicate that the coming uptrend is likely to be especially strong."

**Key Points**:
- MACD-Lines (fast line - slow line) can show divergence too
- NO ZERO LINE for MACD-Lines (they oscillate around signal line)
- Pattern: Price makes lower low, MACD-Lines make higher low
- OR: Price makes higher high, MACD-Lines make lower high
- This is NOT required for MACD-Histogram divergence
- When present → EXTRA STRONG signal

**Implementation Note**:
- Track MACD-Lines separately
- If both MACD-Histogram AND MACD-Lines show divergence → highest confidence
- Elder's 2009 DJIA example showed both → "lasted almost a year"

### 2.6 Multiple Entry Attempts

Elder describes a key professional technique using GE example:

**Pattern**:
1. First divergence signal triggers buy
2. Price rallies briefly, then reverses and hits stop
3. MACD-H ticks up AGAIN → second buy signal (C2)
4. Second entry doubles in price

**Elder's Insight**: "One of the key differences between amateurs and pros is that when a beginner gets stopped out, he feels disgusted and moves on to other stocks. Professionals, on the other hand, often attempt multiple entries, using fairly tight stops. One big success will more than offset several small losses."

**Implementation**:
```cpp
// Allow PENDING_CONFIRMATION state to recur
// If stopped out but divergence pattern still valid:
//   - Reset to PENDING_CONFIRMATION
//   - Wait for next uptick
//   - Re-enter trade
// 
// Stop attempting after:
//   - Pattern invalidated (new extreme breaks previous extreme)
//   - OR 2-3 attempts exhausted
//   - OR MACD-H crosses zero upward (trend changed)
```

### 2.7 Stop Placement Guidelines

**Elder's Stop Rules**: "A general idea is when you buy place your stop in the vicinity of the latest low."

**For Bullish Divergence**:
- Place stop below the second price trough (valley B)
- Allow small buffer for noise (e.g., 0.5 ATR or 1-2%)
- Accept "occasional small hit"
- Have confidence to re-enter if signal repeats

**For Bearish Divergence**:
- Place stop above the second price peak (summit B)
- Similar buffer rules

**Philosophy**:
- "We live in an imperfect world, where even the best signals occasionally fail"
- "All we can do is bet on probabilities and use protective stops"
- Focus on win rate × average win vs loss rate × average loss

### 2.8 Recommended MACD Parameters

**Elder's Standard**: 12-26-9 (fast EMA, slow EMA, signal line)

**His Recommendation**: "I encourage you to experiment with these numbers, making each of them slightly larger or smaller. It is a good idea to use your personal and individual indicator settings. There are no magic numbers – only the numbers you've tested and come to trust."

**Lookback Periods**:
- **Local extremes**: 3-5 bars minimum for peak/trough detection
- **Pattern validity**: 5+ bars minimum between first extreme and zero cross
- **Divergence age**: Consider resetting if > 50-100 bars since first extreme

**Tolerance Values**:
- **Zero-line threshold**: ±0.00001 (floating point precision)
- **Uptick/downtick**: Any positive/negative change (no minimum threshold)
- **Price comparison**: Exact comparison (no tolerance needed)

### 2.9 C++ Implementation Structure

#### Class Member Variables

```cpp
class MACDDivergence {
private:
    // Price extremes
    float m_priceBottom1 = 0.0f;
    float m_priceBottom2 = 0.0f;
    float m_pricePeak1 = 0.0f;
    float m_pricePeak2 = 0.0f;
    
    int m_priceBottom1Index = -1;
    int m_priceBottom2Index = -1;
    int m_pricePeak1Index = -1;
    int m_pricePeak2Index = -1;
    
    // MACD-H extremes
    double m_macdBottom1 = 0.0;
    double m_macdBottom2 = 0.0;
    double m_macdPeak1 = 0.0;
    double m_macdPeak2 = 0.0;
    
    int m_macdBottom1Index = -1;
    int m_macdBottom2Index = -1;
    int m_macdPeak1Index = -1;
    int m_macdPeak2Index = -1;
    
    // State tracking
    bool m_macdCrossedZeroUp = false;
    bool m_macdCrossedZeroDown = false;
    int m_zeroCrossIndexUp = -1;
    int m_zeroCrossIndexDown = -1;
    
    DivergenceEnum m_currentState = DivergenceEnum::NONE;
    
    // Configuration
    const int MIN_BARS_BETWEEN_EXTREMES = 5;  // Minimum bars for valid pattern
    const double MACD_ZERO_THRESHOLD = 0.00001;  // Floating point tolerance
};
```

#### Update Method Signature

```cpp
void Update(
    int currentIndex,
    float priceHigh,
    float priceLow,
    double macdHistogramValue,
    double prevMacdHistogramValue
);
```

#### Helper Methods

```cpp
// Detect local extremes
bool IsLocalMinimum(int index, int lookback = 3);
bool IsLocalMaximum(int index, int lookback = 3);

// Validate patterns
bool HasValidBullishSetup();
bool HasValidBearishSetup();

// Check zero crossovers
bool DidCrossZeroUpward(double current, double previous);
bool DidCrossZeroDownward(double current, double previous);

// Reset divergence tracking
void ResetBullishTracking();
void ResetBearishTracking();
```

---

## 4. NH-NL Breadth Analysis

### 4.1 Core Definition

#### What is NH-NL?

**Daily NH-NL = (Number of New Highs) - (Number of New Lows)**

- **New Highs:** Stocks reaching their highest price in the past **365 calendar days**
- **New Lows:** Stocks falling to their lowest price in the past **365 calendar days**
- **Data Source:** NYSE + AMEX + NASDAQ combined (excluding ETFs, UITs, closed-end funds, warrants, preferred securities)
- **Recommended Provider:** Barchart.com (free) - provides separate and combined figures

#### The Analogy

> "Visualize all stocks as soldiers in a regiment attacking a hill. New Highs are the officers leading the charge uphill. New Lows are the officers deserting and running downhill. When more officers run downhill than uphill, the poorly led attack will fail."

### 4.2 Elder's Core Philosophy

> "NH-NL is a **leading indicator** that tracks the behavior of market leaders. New Highs are leaders in strength pulling the market up. New Lows are leaders in weakness dragging it down."

### 4.3 Weekly NH-NL Signals (Primary Timeframe)

Elder emphasizes the **weekly NH-NL** as the primary market analysis tool.

#### Bull Market Confirmation

**Threshold: Weekly NH-NL > +2,500**

- **Meaning:** "You're on solid ground, this is a bull market; hold on to what you've got and buy pullbacks."
- **Significance:** Only occurs during genuine bull markets, NOT during bear market rallies
- **Action:** Get long and stay long; use daily charts to find entry points
- **Warning Signal:** If S&P makes new highs but weekly NH-NL cannot reach +2,500 → bullish leadership weakening → expect trouble ahead

#### Weekly Divergences

**Bullish Divergence** (market bottom reversal):
1. Both price and NH-NL fall to new lows
2. Both rally, with NH-NL crossing **above zero**
3. Both fall again: price makes **lower low**, NH-NL makes **higher low** → BUY SIGNAL
4. **Critical Rule:** NH-NL MUST cross and re-cross zero line between the two bottoms

**Bearish Divergence** (market top reversal):
1. Both price and NH-NL rise to new highs
2. Both decline, with NH-NL crossing **below zero**
3. Both rally: price makes **higher high**, NH-NL makes **lower high** → SELL SIGNAL
4. **Critical Rule:** NH-NL MUST cross and re-cross zero line between the two peaks

**Duration:** New trends after weekly NH-NL divergences can persist **~1 year**

#### Weekly Spikes (Extreme Panic Levels)

**Major Spike Threshold: Weekly NH-NL < -4,000 then rises above → BUY SIGNAL**

- **Meaning:** Mass capitulation, extreme fear, weak hands shaken out, stocks pass from weak to strong hands
- **Calculation Example:** To reach -4,000 weekly, daily NH-NL must be at least **-800 for 5 consecutive days**
- **Works:** In both bull and bear markets
- **Duration:** 
  - Bull market: rally may last **~1 year**
  - Bear market: rally may last only **a few weeks** (shorts covering)

**Mini-Spike Threshold (Bull Markets Only): Weekly NH-NL < -1,500 then rises above → BUY SIGNAL**

- **Meaning:** Minor panic during bull market, high-quality buying opportunity
- **Frequency:** Occurs about once per year during bull markets
- **Action:** Good for several months of upside

**Historical Exception (October 2008):**
- Weekly NH-NL crashed to **-18,000** (unprecedented; previous historic floor was -6,000)
- Government intervention reliquefied markets
- November 2008: bottomed at -10,000 (bullish divergence forming)
- March 2009: final bottom at -5,854 (normal bear market bottom level)
- **Lesson:** Always use protective stops, even with strong signals

### 4.4 Daily NH-NL Signals (Tactical Timeframe)

Use daily NH-NL for **entry/exit timing** after weekly NH-NL establishes trend direction.

#### Confirmation Band

**Neutral Zone: Daily NH-NL between +100 and -100**

- **Bullish Confirmation:** Daily NH-NL > +100 → bulls in control
- **Bearish Confirmation:** Daily NH-NL < -100 → bears in control
- **Purpose:** Filter whipsaws from simple zero-line crosses

#### Trading Rules (Daily Timeframe)

**When Daily NH-NL > +100:**
- Market is bullish
- Pullbacks to EMA (Exponential Moving Average) = excellent **long entry** opportunities
- Use envelope parallel to EMA as short-term profit target

**When Daily NH-NL < -100:**
- Market is bearish  
- Rallies to EMA = excellent **short entry** opportunities
- Use envelope parallel to EMA as short-term profit target

#### Daily Divergences

**Bullish Divergence:**
1. Both index and daily NH-NL drop to low point A
2. Both rally at B, NH-NL crosses **above zero**
3. Index makes **lower low** at C, NH-NL makes **higher low** → BUY SIGNAL

**Bearish Divergence:**
1. Both index and daily NH-NL rise to high point A
2. Both decline at B, NH-NL crosses **below zero**
3. Index makes **higher high** at C, NH-NL makes **lower high** → SELL SIGNAL

### 4.5 NH-NL State Mapping

**MindfulTrader NH-NL Signal States (aligned with Elder methodology):**

| State ID | Enum Name | Condition | Elder Interpretation |
|----------|-----------|-----------|---------------------|
| 0 | UNCLEAR | -100 < daily NH-NL < +100 | Neutral zone, no clear signal |
| 1 | BULLISH_CONFIRMATION | daily NH-NL > +100 OR weekly NH-NL > +2,500 | Strong bullish leadership |
| -1 | BEARISH_CONFIRMATION | daily NH-NL < -100 | Strong bearish leadership |
| 2 | BULLISH_DIVERGENCE | Price lower low + NH-NL higher low (after zero cross) | Major reversal up signal |
| -2 | BEARISH_DIVERGENCE | Price higher high + NH-NL lower high (after zero cross) | Major reversal down signal |
| 3 | EXTREME_LOWS_BOUNCE | weekly NH-NL < -4,000 then rises above | Extreme panic, buy signal |
| -3 | EXTREME_HIGHS_PEAK | weekly NH-NL > +2,500 but failing to confirm new S&P highs | Narrow rally, weakness warning |

### 4.6 Data Requirements

**Authentic Implementation Requires:**

1. **Data Source:** Actual NYSE+AMEX+NASDAQ New Highs - New Lows (not proxies)
2. **Columns Required:**
   - `date`
   - `new_highs` (daily count)
   - `new_lows` (daily count)
   - `nh_nl_daily` (new_highs - new_lows)
   - `nh_nl_weekly` (5-day rolling sum)
   - `sp500_close` (S&P 500 price for divergence detection)

3. **Data Providers:**
   - **Barchart.com** (free) - provides separate and combined figures
   - **StockCharts.com** ($25/month) - symbols: `$NYHL`, `$NAHL`, `$NQHL`
   - **Alternative vendors**: Norgate Data, CSI Data, QuoteMedia

### 4.7 Divergence Detection Algorithm

```python
def detect_nh_nl_divergence(df):
    """
    Detect bullish/bearish divergences between S&P 500 price and NH-NL.
    
    Elder Rules:
    1. Identify price peaks/troughs
    2. Verify NH-NL crosses zero line between peaks/troughs
    3. Compare relative heights of peaks or depths of troughs
    4. Confirm divergence when price and NH-NL move in opposite directions
    """
```

### 4.8 Critical Success Factors

1. **Use actual NYSE+AMEX+NASDAQ data** - not proxies or derivatives
2. **Weekly timeframe is primary** - daily is for tactical entry/exit
3. **Zero-line crosses are mandatory** - for valid divergence signals
4. **Protective stops are essential** - even with strong NH-NL signals

---

## 5. Integration Guidelines

### 5.1 Triple Screen Integration for MindfulTrader

**Screen 1 (Weekly):** Weekly NH-NL confirms market regime
- Weekly NH-NL > +2,500 → Bull market confirmed → Only take long setups
- Weekly NH-NL < -4,000 then rises → Extreme buying opportunity
- Weekly NH-NL divergences → Major trend reversals

**Screen 2 (Daily):** Daily NH-NL identifies pullback entry opportunities
- In uptrend: wait for daily NH-NL to dip (ideally to +100 level) for long entries
- In downtrend: wait for daily NH-NL to rally (ideally to -100 level) for short entries
- Daily MACD divergences → Short-term reversals

**Screen 3 (Intraday):** Raschke setups execute entries
- Use Linda Raschke patterns (Holy Grail, Turtle Soup, NR7) for precise entries
- Elder Breakout as alternative execution method
- Force Index divergences for profit-taking exits

### 5.2 IndicatorManager Update Pattern

```cpp
// In TripleScreen1.cpp or TripleScreen2.cpp
void UpdateMACDDivergence(SCStudyInterfaceRef& sc, int index) {
    // Get MACD-Histogram value
    SCFloatArray macdHist;
    sc.GetStudyArrayUsingID(Input_MACDStudy.GetStudyID(), 
                            SC_MACD_DIFF,  // MACD-Histogram subgraph
                            macdHist);
    
    double macdValue = macdHist[index];
    double prevMacdValue = (index > 0) ? macdHist[index-1] : 0.0;
    
    float priceHigh = sc.High[index];
    float priceLow = sc.Low[index];
    
    // Update divergence detector
    auto& divergence = IndicatorManager::Instance()
        .GetIndicator<MACDDivergence>(IndicatorKeys::LONG_MACD_DIVERGENCE);
    
    divergence.Update(index, priceHigh, priceLow, macdValue, prevMacdValue);
}
```

### 5.3 Key Elder Quotes for Code Comments

Use these in your implementation:

```cpp
// "When trying to find a divergence, first look at the pattern of an 
//  indicator and later at the pattern of prices." — Elder

// "MACD-Histogram has to cross above the zero line before sinking to 
//  its second bottom. If there is no crossover, then there is no divergence." — Elder

// "MACD-H gives a buy signal when it ticks up from its second bottom. It 
//  doesn't have to cross above the centerline for the second time." — Elder

// "'Breaking the back of the bear' — the rally lifts MACD-H above the zero 
//  line, confirming bears are losing control." — Elder

// "The bear is getting older and weaker — but the bear is still in charge!" 
//  — Elder (on patterns without zero cross)

// "NH-NL is a leading indicator that tracks the behavior of market leaders. 
//  New Highs are leaders in strength pulling the market up." — Elder
```

---

## 6. Implementation Status

### 6.1 Current MindfulTrader Status

**✅ Implemented:**
- Triple Screen framework (TripleScreen1.cpp, TripleScreen2.cpp, TripleScreen3.cpp)
- Study-to-study referencing via Sierra Chart study IDs
- IndicatorManager singleton for cross-study state
- Raschke pattern detection (ENUM_REFERENCE.md for pattern definitions)
- Basic market regime detection (CPP_MARKET_REGIME.md includes Elder NH-NL methodology)

**🔄 In Progress:**
- Elder MACD divergence detection algorithm
- Authentic NYSE+AMEX+NASDAQ NH-NL data integration
- NH-NL divergence detection state machine

**⏳ Pending:**
- Professional re-entry logic for stopped-out divergence trades
- MACD-Lines divergence tracking (extra confirmation signal)
- Dynamic stop placement based on volatility (ATR-based)

### 6.2 Data Requirements Status

**Current Limitation:**
- Many implementations use proxy NH-NL calculations (e.g., SPY derivatives)
- These lack the scale and accuracy of authentic NYSE+AMEX+NASDAQ data
- Elder's exact thresholds (+2,500 weekly, -4,000 panic spike) require real data

**Recommended Action:**
- Invest in professional data source (StockCharts.com $25/month or Barchart.com free)
- NH-NL is a **leading indicator** that could provide significant trading edge
- Cost is negligible compared to potential improvements in trade timing

---

## 7. References

### 7.1 Source Materials

**Books by Dr. Alexander Elder:**
1. **"Two Roads Diverged: Trading Divergences"** (2012-2014)
   - Complete MACD divergence methodology
   - Zero-line crossover requirement
   - Professional re-entry techniques

2. **"The New High – New Low Index"** (2nd Revised Edition, 2014)
   - Co-authored with Kerry Lovvorn
   - Complete NH-NL breadth methodology
   - Exact thresholds and divergence rules

3. **"Come Into My Trading Room"** (2002)
   - Trade grading system (entry/exit/channel capture)
   - Quantitative trade evaluation methodology
   - A-Trade pre-entry scoring system

4. **"Trading for a Living"** (1993)
   - Triple Screen System framework
   - Multi-timeframe analysis approach
   - Psychological aspects of trading

### 7.2 Data Providers

**Free Sources:**
- Barchart.com - provides daily NYSE, AMEX, NASDAQ New Highs/Lows separately and combined

**Paid Sources:**
- StockCharts.com ($25/month) - symbols: `$NYHL` (NYSE), `$NAHL` (AMEX), `$NQHL` (NASDAQ)
- Norgate Data, CSI Data, QuoteMedia - professional historical data vendors

### 7.3 Related MindfulTrader Documentation

- [CPP_MARKET_REGIME.md](CPP_MARKET_REGIME.md) - Linda Raschke market regime + Elder NH-NL integration (Section 3: NH-NL Integration)
- [ENUM_REFERENCE.md](ENUM_REFERENCE.md) - Authoritative pattern definitions and detection order
- [STRATEGIES_PARAMETERS_REFERENCE.md](STRATEGIES_PARAMETERS_REFERENCE.md) - Configuration guide
- [ARCH.md](../ARCH.md) - Architecture overview and integration points

### 7.4 Implementation Checklist

**MACD Divergence:**
- [ ] Implement state machine with SEARCHING_FIRST_TROUGH → WAITING_ZERO_CROSS → WAITING_SECOND_TROUGH → PENDING_CONFIRMATION states
- [ ] Add zero-line crossover detection (mandatory for valid divergence)
- [ ] Implement uptick/downtick signal triggers
- [ ] Add validation checklist (two distinct extremes, substantial rally, etc.)
- [ ] Support professional re-entry on stopped-out trades
- [ ] Track MACD-Lines divergence for extra confirmation (optional)
- [ ] Add visual debugging markers (trough locations, zero cross, signals)

**NH-NL Breadth:**
- [ ] Integrate authentic NYSE+AMEX+NASDAQ NH-NL data source
- [ ] Implement weekly NH-NL calculation (5-day rolling sum)
- [ ] Detect bull market confirmation (weekly > +2,500)
- [ ] Detect panic spikes (weekly < -4,000)
- [ ] Implement divergence detection (price vs NH-NL with zero-line cross requirement)
- [ ] Map to MindfulTrader NH-NL state enum (UNCLEAR, BULLISH_CONFIRMATION, BEARISH_CONFIRMATION, etc.)
- [ ] Integrate with market regime classification system

**Triple Screen Integration:**
- [ ] Screen 1: Weekly NH-NL → market regime filter
- [ ] Screen 2: Daily NH-NL + MACD → entry timing
- [ ] Screen 3: Raschke patterns → execution triggers
- [ ] Add Force Index divergences for exits

---

## Summary

Dr. Alexander Elder's methodology is **precise and algorithmic**, not subjective pattern recognition:

**MACD Divergences:**
1. **Zero-line crossover is mandatory** — no exceptions
2. **Uptick/downtick triggers signal** — immediate entry
3. **Two distinct price extremes required** — with rally/decline between
4. **MACD-H pattern must oppose price** — higher low or lower high
5. **Professional re-entry** — don't give up after one stop-out
6. **Tight stops near extremes** — accept small losses for big wins

**NH-NL Breadth:**
1. **Use actual NYSE+AMEX+NASDAQ data** — not proxies
2. **Weekly timeframe is primary** — daily is tactical
3. **Zero-line crosses are mandatory** — for valid divergences
4. **Exact thresholds matter** — +2,500 bull confirmation, -4,000 panic spike
5. **Leading indicator** — tracks market leadership, not just price
6. **Protective stops essential** — even with strong signals

**Triple Screen System:**
1. **Screen 1 (Weekly)** — Identify long-term trend → NH-NL regime filter
2. **Screen 2 (Daily)** — Identify pullbacks → MACD/NH-NL entry timing
3. **Screen 3 (Intraday)** — Execute entries → Raschke patterns or Elder Breakout

This comprehensive methodology can be implemented with **100% fidelity** to Elder's definitions as rule-based state machines.

**Implementation Priority**: High (core Triple Screen divergence detection with leading breadth indicator)  
**Complexity**: Moderate (state machines + peak/trough detection + divergence logic)  
**Expected Benefit**: Catch major reversals and trend confirmations with precise Elder-validated signals

---

*Document consolidates ELDER_MACD_DIVERGENCE_SPEC.md (634 lines), ELDER_NH_NL_METHODOLOGY.md (296 lines), and elder_nh_nl_extracted.txt (296 lines - duplicate of methodology) into single comprehensive Elder trading methodology reference.*
