# Alexander Elder Trade Grading System

**Source:** "Come Into My Trading Room" by Dr. Alexander Elder  
**Date Documented:** December 30, 2025  
**Purpose:** Authoritative reference for Elder's quantitative trade grading methodology

---

## Overview

Alexander Elder defines a quantitative grading scale to evaluate the **quality of a trade's execution**. This system is designed to separate the **quality of the process** from the **monetary outcome** of the trade.

The system is centered on two main metrics:
1. **Channel Capture Grade** - How much of the move you caught
2. **Execution Precision Grade** - How well you timed entry and exit

---

## 1. Channel Capture Grade (Trade Grade)

This is Elder's primary tool for measuring how much of a market move you "caught." He uses **Keltner Channels** (typically set at 2.5 or 3.0 ATR around an EMA) to define the boundaries of "normal" price action.

### Formula

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

### The Scale

| Grade | Channel Capture | Quality |
|-------|-----------------|---------|
| **A** | **30% or more** | Excellent - captured substantial portion of move |
| **B** | **20% to 30%** | Good - captured meaningful portion |
| **C** | **10% to 20%** | Acceptable - captured some of the move |
| **D/F** | **Less than 10% or loss** | Poor - minimal capture or losing trade |

### Key Insights

- **30% is exceptional performance** - This is an A-grade trade
- **20% is good** - Respectable capture of the available move
- **10% is acceptable** - Better than break-even but room for improvement
- Channel width represents the "normal" range of price movement based on recent volatility

---

## 2. Entry Precision Grade

Elder grades how close you came to the **optimal entry price** on the bar (or day) of execution. This measures skill in timing and order placement.

### Formula (for LONG positions)

```
Entry Grade = (High - Entry Price) / (High - Low) × 100%
```

An **A-grade entry** is in the **bottom 20%** of the bar's range (buying near the low).

### The Scale (LONG positions)

| Grade | Bar Position | Entry Price Location |
|-------|--------------|---------------------|
| **A** | **0-20%** | Bottom 20% of range (near the low) |
| **B** | **20-40%** | Second quintile |
| **C** | **40-60%** | Middle 20% |
| **D** | **60-80%** | Fourth quintile |
| **F** | **80-100%** | Top 20% of range (near the high) |

### For SHORT positions

Invert the scale - an A-grade short entry is in the **top 20%** of the bar's range (selling near the high).

```
Entry Grade = (Entry Price - Low) / (High - Low) × 100%
```

---

## 3. Exit Precision Grade

Elder grades how close you came to the **optimal exit price** on the bar of execution.

### Formula (for LONG positions)

```
Exit Grade = (Exit Price - Low) / (High - Low) × 100%
```

An **A-grade exit** is in the **top 20%** of the bar's range (selling near the high).

### The Scale (LONG positions)

| Grade | Bar Position | Exit Price Location |
|-------|--------------|---------------------|
| **A** | **80-100%** | Top 20% of range (near the high) |
| **B** | **60-80%** | Second quintile |
| **C** | **40-60%** | Middle 20% |
| **D** | **20-40%** | Fourth quintile |
| **F** | **0-20%** | Bottom 20% of range (near the low) |

### For SHORT positions

Invert the scale - an A-grade short exit is in the **bottom 20%** of the bar's range (covering near the low).

```
Exit Grade = (High - Exit Price) / (High - Low) × 100%
```

---

## 4. The "A-Trade" Pre-Entry Scoring System

Elder suggests a **100-point scoring system** to determine if a setup is worth taking **before you enter**. A score above **80/100** qualifies as an **"A-Trade"**.

### Common Criteria

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

### Scoring Example

| Criterion | Points | Score |
|-----------|--------|-------|
| Price above 30-week EMA | 25 | 25 ✓ |
| In value zone (between EMAs) | 20 | 20 ✓ |
| MACD-Histogram rising | 20 | 20 ✓ |
| Volume above average | 15 | 15 ✓ |
| Near support level | 10 | 10 ✓ |
| Risk:Reward > 3:1 | 10 | 10 ✓ |
| **Total** | **100** | **100** (A-Trade) |

---

## Implementation in MindfulTrader System

### Current Implementation

The `Trade` class in `src/Trade.cpp` already calculates all three grades:

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

### Correct Thresholds for Active Exit Logic

Using **Elder's actual methodology**, protective actions should trigger at:

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

---

## Key Takeaways

1. **30% channel capture = A-grade** (NOT 50% or 80%)
2. **20% channel capture = B-grade** (Good trade)
3. **10% channel capture = C-grade** (Acceptable trade)
4. Entry/Exit grades measure **bar positioning** (20% quintiles)
5. Trade grade measures **channel capture** (profit vs available move)
6. The grading system is **process-focused**, not outcome-focused
7. An A-grade trade can still lose money (if stopped out)
8. The goal is consistent A/B grade execution over time

---

## References

- **Primary Source:** "Come Into My Trading Room" by Dr. Alexander Elder
- **Secondary Source:** "The New Trading for a Living" by Dr. Alexander Elder
- **Related:** Keltner Channel methodology, ATR-based volatility measurement

---

## Historical Note

Previous implementations incorrectly used:
- ❌ 80/60/40 thresholds (academic grading scale)
- ❌ 50/30/20 thresholds (incorrect interpretation)

**Correct thresholds per Elder:**
- ✅ **30/20/10** (A/B/C grades for channel capture)
- ✅ **20% quintiles** (A/B/C/D/F for entry/exit bar position)

This document serves as the authoritative reference for all future implementations.
