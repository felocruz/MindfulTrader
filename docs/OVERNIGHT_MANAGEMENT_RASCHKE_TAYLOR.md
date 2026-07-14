# Overnight Management: Linda Raschke's Taylor Trading Technique

**Document Version:** 1.0  
**Date:** December 20, 2025  
**Author:** Rafael Cruz, Ph.D.  
**Purpose:** Implementation specification for overnight position management using Raschke's adaptations of Taylor Trading Technique

---

## Table of Contents

1. [Overview](#overview)
2. [The Golden Rule for Overnight Entries](#the-golden-rule-for-overnight-entries)
3. [Overnight Exit Windows](#overnight-exit-windows)
4. [Tradable Overnight Entry Windows](#tradable-overnight-entry-windows)
5. [Implementation Details](#implementation-details)
6. [Integration with Existing System](#integration-with-existing-system)

---

## Overview

Linda Raschke's adaptations of the Taylor Trading Technique provide a **specific set of rules** based on the relationship between the Close, the Open, and the "Objective Point" (previous day's high/low). This is **NOT** just a clock setting—it's tactical overnight management that requires immediate feedback and precise timing.

### Core Philosophy

> "You should only carry a position overnight if it is showing a profit and closes in the top/bottom 25% of the daily range (a 'Strong Close')."
> — Linda Raschke

**The overnight decision is NOT about hoping for a gap in your favor. It's about carrying STRENGTH overnight and managing the position aggressively the next morning.**

---

## The Golden Rule for Overnight Entries

### Primary Filter: Immediate Feedback

**The Rule:** You should only carry a position overnight if **BOTH** conditions are met:

1. ✅ Position is **showing a profit** at the 16:00 ET close
2. ✅ Price closes in the **top 25% (long)** or **bottom 25% (short)** of the daily range

### Tactical Timing: The "3:30 PM PM-Run"

If you are entering late in the day (e.g., the `PM_RUN_ENTRY` window: **15:45-16:00 ET**):

- **The trade MUST move in your favor almost immediately**
- If the market is flat or at a loss by the 16:00 ET close → **SCRATCH IT**
- **Do NOT hope for a gap in your favor** — that's gambling, not trading

### Implementation Logic

```cpp
// Calculate daily range position
float dailyRange = dailyHigh - dailyLow;
float rangePosition = (currentClose - dailyLow) / dailyRange;

// LONG: Must close in top 25% of range
bool strongCloseLong = (rangePosition >= 0.75);

// SHORT: Must close in bottom 25% of range
bool strongCloseShort = (rangePosition <= 0.25);

// Golden Rule check
bool passesGoldenRule = (positionProfit > 0.0) && 
                        (isLong ? strongCloseLong : strongCloseShort);

if (!passesGoldenRule) {
    // SCRATCH: Exit flat at close, don't hold overnight
    return HoldingStrategyEnum::SCRATCH_AT_CLOSE;
}
```

### Strong Close Visual

```
Daily Range Example (LONG position):
┌─────────────────────────────────────────┐
│ Daily High: 4200.00            ← TOP 25%│  ✅ Strong Close Zone (LONG)
│                                          │
│ 75% Level: 4185.00            ───────────┤
│                                          │
│                                          │  ⚠️ Weak Close Zone
│ 50% Level (Mid): 4170.00                │
│                                          │
│                                          │  ❌ Wrong Side of Range
│ 25% Level: 4155.00            ───────────┤
│                                          │
│ Daily Low: 4140.00          ← BOTTOM 25% │  ✅ Strong Close Zone (SHORT)
└─────────────────────────────────────────┘

LONG: Close at 4190.00 → rangePosition = 0.83 → ✅ PASS (top 25%)
LONG: Close at 4175.00 → rangePosition = 0.58 → ❌ FAIL (middle of range)
SHORT: Close at 4145.00 → rangePosition = 0.08 → ✅ PASS (bottom 25%)
```

---

## Overnight Exit Windows

Raschke focuses on the **first 30-90 minutes of the following day** for exiting overnight positions. This is when the market reveals its true intentions.

### A. The "Gap" Exit (Profit Taking)

**Scenario:** Market gaps in your favor the next morning (a "windfall")

**Action:** Exit **immediately** or within the first **5-15 minutes** (09:30-09:45 ET)

**Logic:** 
- Gaps are often **emotional exhaustion points**
- Raschke frequently quotes: *"An overnight gap presents an excellent opportunity to take profits."*
- Don't be greedy—take the windfall and move on

**Implementation:**

```cpp
enum class OvernightExitTypeEnum {
    GAP_EXIT = 3,  // Gap in favor (windfall) → exit 09:30-09:45 ET
    // ...
};

// Gap detection (> 0.5% move in favor)
bool IsGapInFavor(float overnightEntry, float openPrice, bool isLong) {
    float gapPercent = (openPrice - overnightEntry) / overnightEntry;
    
    if (isLong) {
        return gapPercent > 0.005;  // > 0.5% gap up
    } else {
        return gapPercent < -0.005; // > 0.5% gap down (absolute)
    }
}
```

### B. The "First Reaction" Exit (Failure to Follow Through)

**Scenario:** You are long and the market opens **flat** or **slightly down**

**Action:** Exit on the **first reaction** (the first small bounce) — typically **09:30-10:00 ET**

**Logic:**
- If a "strong close" doesn't lead to a "strong open," **the market is telling you the momentum has shifted**
- **Don't wait for your stop to get hit**—exit because the **reason for the trade (momentum) is gone**
- This is a strategic exit, not a mechanical stop-out

**Implementation:**

```cpp
enum class OvernightExitTypeEnum {
    FIRST_REACTION_EXIT = 4,  // Flat/adverse open → exit on first bounce (09:30-10:00 ET)
    // ...
};

// Detect flat or unfavorable open
bool IsFlatOrUnfavorableOpen(float overnightEntry, float openPrice, bool isLong) {
    float openMove = openPrice - overnightEntry;
    
    if (isLong) {
        // LONG: Open flat or down
        return openMove <= 0.0;
    } else {
        // SHORT: Open flat or up
        return openMove >= 0.0;
    }
}

// Exit logic: On first bounce (15-min bar)
if (IsFlatOrUnfavorableOpen(entry, open, isLong)) {
    // Wait for first reaction:
    // LONG: Wait for small bounce up, then exit
    // SHORT: Wait for small dip down, then exit
    if (currentTime >= SC_MARKET_OPEN && currentTime <= SC_MARKET_OPEN + 30_minutes) {
        if (hasReactedInFavor()) {
            return OvernightExitTypeEnum::FIRST_REACTION_EXIT;
        }
    }
}
```

### C. The Taylor "Objective Point"

**Scenario:** Price reaches the Taylor target (previous day's high for longs, previous day's low for shorts)

**Action:** Exit at the **liquidity window** where the target is hit

**Logic:**
- **Buy Day Exit:** If you bought the "test of the previous day's low," your target is the **previous day's high**
- **Time of Day:** If the market reaches this target during:
  - **London Open** (03:00-04:00 AM ET) → Exit during Globex
  - **New York Open** (09:30-10:30 AM ET) → Exit during RTH
- These are the two primary **"liquidity windows"** where swing targets are hit

**Implementation:**

```cpp
enum class OvernightExitTypeEnum {
    OBJECTIVE_POINT_EXIT = 5,  // Taylor target hit (prev day H/L) → exit at liquidity window
    // ...
};

// Taylor Objective Point check
bool HasHitObjectivePoint(float currentPrice, float prevDayHigh, float prevDayLow, bool isLong) {
    if (isLong) {
        // LONG: Target is previous day's high
        return currentPrice >= prevDayHigh;
    } else {
        // SHORT: Target is previous day's low
        return currentPrice <= prevDayLow;
    }
}

// Exit at liquidity windows
TimeOfDayEnum currentSession = GetTimeOfDay(currentTime);
if (HasHitObjectivePoint(price, prevHigh, prevLow, isLong)) {
    if (currentSession == TimeOfDayEnum::LONDON_WINDOW ||
        currentSession == TimeOfDayEnum::OPENING_HOUR) {
        return OvernightExitTypeEnum::OBJECTIVE_POINT_EXIT;
    }
}
```

### D. The 3-10 Oscillator Exit (Momentum Failure)

**Scenario:** The 3-10 Oscillator (3-period minus 16-period MA) crosses during the Globex session

**Action:** Exit **pre-market** (before 09:30 ET) or on the first bounce after open

**Logic:**
- The 3-10 Oscillator is Raschke's preferred momentum indicator for overnight management
- If the **Fast Line (3-period)** crosses the **Slow Line (16-period)** on the 15-minute or 60-minute chart during Globex
  - This signals **momentum has shifted against you**
- Exit pre-market if possible (electronic session), or on first reaction after open

**Implementation:**

```cpp
enum class OvernightExitTypeEnum {
    MOMENTUM_FAILURE_EXIT = 6,  // 3-10 Oscillator crossed during Globex → exit pre-market
    // ...
};

// 3-10 Oscillator calculation (simplified)
// Full version: 3-period EMA - 16-period EMA
float threeLineOscillator = EMA(close, 3) - EMA(close, 16);
float threeLineOscPrev = EMA(closePrev, 3) - EMA(closePrev, 16);

// Detect crossover
bool HasMomentumCrossover(float osc, float oscPrev, bool isLong) {
    if (isLong) {
        // LONG: Fast line crosses below slow line (bearish crossover)
        return (oscPrev > 0.0) && (osc <= 0.0);
    } else {
        // SHORT: Fast line crosses above slow line (bullish crossover)
        return (oscPrev < 0.0) && (osc >= 0.0);
    }
}

// Exit during Globex or pre-market
if (HasMomentumCrossover(osc, oscPrev, isLong)) {
    TimeOfDayEnum session = GetTimeOfDay(currentTime);
    if (session == TimeOfDayEnum::ASIAN_SESSION ||
        session == TimeOfDayEnum::LONDON_WINDOW ||
        session == TimeOfDayEnum::LONDON_TO_PREMARKET ||
        session == TimeOfDayEnum::PRE_MARKET_HOOK) {
        return OvernightExitTypeEnum::MOMENTUM_FAILURE_EXIT;
    }
}
```

---

## Tradable Overnight Entry Windows

If you aren't already in a trade but want to enter "overnight" (Globex/Electronic session), Raschke typically looks at **two specific times**:

### Window 1: The "London Maneuver" (03:00-04:00 AM ET)

**Tactic:** Watch for a **"false breakout"** of the Asian session range

**Detail:**
- If the market **spikes** during the London open and then **traps back inside** the Asian range
- This often sets the **trend for the NY morning**
- Entry is on the **rejection** of the false breakout

**Implementation:**

```cpp
enum class TimeOfDayEnum {
    LONDON_WINDOW = 1,  // 03:00-04:00 ET - "London Maneuver" false breakout setups
    // ...
};

// Detect London false breakout
struct AsianRange {
    float high;  // Asian session high (18:00-03:00 ET)
    float low;   // Asian session low
};

bool IsLondonFalseBreakout(AsianRange asian, float londonHigh, float londonLow, float currentClose) {
    // Spiked above Asian high but closed back inside
    bool falseBreakoutUp = (londonHigh > asian.high) && (currentClose < asian.high);
    
    // Spiked below Asian low but closed back inside
    bool falseBreakoutDown = (londonLow < asian.low) && (currentClose > asian.low);
    
    return falseBreakoutUp || falseBreakoutDown;
}

// Entry: Trade the rejection direction
// If false breakout up → GO SHORT (rejection of highs)
// If false breakout down → GO LONG (rejection of lows)
```

### Window 2: The "Pre-Market Hook" (08:30-09:00 AM ET)

**Tactic:** Watch how the market reacts to **8:30 AM economic data**

**Detail:**
- If the market **"tests"** a level (previous day's high/low, overnight range) and **rejects** it
- This is Raschke's **preferred entry** right before the 9:30 AM open
- The "hook" is a **false move** that traps traders on the wrong side

**Implementation:**

```cpp
enum class TimeOfDayEnum {
    PRE_MARKET_HOOK = 3,  // 08:30-09:00 ET - Economic data reaction, pre-open entry window
    // ...
};

// Detect pre-market hook pattern
struct OvernightRange {
    float high;  // Globex session high
    float low;   // Globex session low
};

bool IsPreMarketHook(OvernightRange overnight, float testPrice, float rejectPrice, bool isLong) {
    if (isLong) {
        // LONG SETUP: Test overnight low, reject back up
        bool testedLow = (testPrice <= overnight.low + 0.001);  // Within tick
        bool rejectedUp = (rejectPrice > testPrice + 0.005);    // > 0.5% bounce
        return testedLow && rejectedUp;
    } else {
        // SHORT SETUP: Test overnight high, reject back down
        bool testedHigh = (testPrice >= overnight.high - 0.001);
        bool rejectedDown = (rejectPrice < testPrice - 0.005);
        return testedHigh && rejectedDown;
    }
}

// Entry: On the rejection bar (15-min or 5-min)
// Stop: Just beyond the tested level (tight)
// Target: NY open volatility (9:30-10:30 AM)
```

---

## Summary Table for Implementation

| Scenario                  | Logic / Time               | Action                                                          | Exit Window         |
|---------------------------|----------------------------|-----------------------------------------------------------------|---------------------|
| **ENTRY (Late Day)**      | 15:45 ET (`PM_RUN_ENTRY`)  | Must be in profit to hold overnight                             | 16:00 ET close      |
| **EXIT (Gap Up/Down)**    | 09:30-09:45 ET             | Sell into the strength immediately (windfall)                   | Opening 5-15 min    |
| **EXIT (Flat Open)**      | 09:30-10:00 ET             | Exit on the first bounce (don't wait for stop)                  | First reaction      |
| **TARGET HIT (Taylor)**   | Any (Globex or RTH)        | Exit at the Previous Day's High/Low (Objective Point)           | Liquidity window    |
| **OSCILLATOR EXIT**       | Globex (any time)          | 3-10 Oscillator crossover → exit pre-market                     | Before 09:30 ET     |
| **LONDON ENTRY**          | 03:00-04:00 ET             | False breakout of Asian range → trade rejection                 | London Window       |
| **PRE-MARKET ENTRY**      | 08:30-09:00 ET             | Test and rejection of key level → entry on hook                 | Pre-Market Hook     |

---

## Implementation Status

**Status:** ✅ **COMPLETE** (December 24, 2025)

All overnight management enums and logic have been fully implemented in the C++ execution layer. For complete technical documentation including:
- Enum definitions and source code
- Detailed computation algorithms
- Helper function implementations
- Usage in Triple Screen System
- Source code file references with line numbers

**See:** [ENUM_REFERENCE.md](ENUM_REFERENCE.md) sections:
- [TimeOfDayEnum](#timeofdayenum) - 13 session quality states with Globex windows
- [HoldingStrategyEnum](#holdingstrategyenum) - 6 overnight decision states with Golden Rule validation
- [OvernightExitTypeEnum](#overnightexittypeenum) - 10 Taylor Trading exit types

**See Also:**
- [GUI_INDICATOR_REFERENCE.md](GUI_INDICATOR_REFERENCE.md) - GUI indicator mappings for overnight management
- [OVERNIGHT_WINDOWS_QUICK_REFERENCE.md](OVERNIGHT_WINDOWS_QUICK_REFERENCE.md) - 24-hour trading timeline visual reference

---

## Key Implementation Notes

### Enum Summary

**TimeOfDayEnum (13 states):**
- Globex windows: ASIAN_SESSION (0), LONDON_WINDOW (1), LONDON_TO_PREMARKET (2), PRE_MARKET_HOOK (3)
- RTH windows: OPENING_HOUR (5), SWEET_SPOT (6), LUNCH_DEAD_ZONE (7), etc.
- Overnight state: OVERNIGHT_HOLD (12)

**HoldingStrategyEnum (6 states):**
- INTRADAY (0), SWING_POSITION (1), WEEKEND_CLOSE (2)
- PM_RUN_CONDITIONAL (3), SCRATCH_AT_CLOSE (4), UNDEFINED (5)

**OvernightExitTypeEnum (10 states):**
- Entry validation: STRONG_CLOSE_QUALIFIED (1), FAILED_GOLDEN_RULE (2)
- Exit types: GAP_EXIT (3), FIRST_REACTION_EXIT (4), OBJECTIVE_POINT_EXIT (5), MOMENTUM_FAILURE_EXIT (6), SCRATCH_EXIT (7)
- Hold continuation: HOLD_FOR_TARGET (8), TRAILING_STOP_EXIT (9)

---

## Integration with Existing System

### 1. Trade Class Updates

The `Trade` class should track overnight state:

```cpp
class Trade {
    // ... existing members ...
    
    // Overnight management
    OvernightExitTypeEnum m_overnightExitType;
    float m_prevDayHigh;
    float m_prevDayLow;
    float m_overnightEntryPrice;
    float m_dailyHigh;  // Today's high (for strong close check)
    float m_dailyLow;   // Today's low (for strong close check)
    
    // 3-10 Oscillator for momentum exit
    float m_threeLineOscillator;
    float m_threeLineOscPrev;
};
```

### 2. PositionManager Updates

The `PositionManager` should evaluate overnight criteria at key times:

```cpp
void PositionManager::EvaluateOvernightHold(SCStudyInterfaceRef sc) {
    TimeOfDayEnum currentSession = GetTimeOfDay(sc.GetCurrentDateTime());
    
    // At close (16:00 ET): Evaluate Golden Rule
    if (currentSession == TimeOfDayEnum::AFTER_HOURS && sc.Index == closeBarIndex) {
        bool passesGoldenRule = EvaluateGoldenRule(sc);
        
        if (!passesGoldenRule) {
            m_openTrade.SetHoldingStrategy(HoldingStrategyEnum::SCRATCH_AT_CLOSE);
            // Exit position flat
            CloseTrade(sc, "SCRATCH: Failed Golden Rule");
        } else {
            m_openTrade.SetHoldingStrategy(HoldingStrategyEnum::SWING_POSITION);
            m_openTrade.SetTimeOfDay(TimeOfDayEnum::OVERNIGHT_HOLD);
        }
    }
    
    // Next morning: Evaluate exit type
    if (m_openTrade.GetTimeOfDay() == TimeOfDayEnum::OVERNIGHT_HOLD) {
        OvernightExitTypeEnum exitType = EvaluateOvernightExit(sc);
        
        if (ShouldExitNow(exitType, currentSession)) {
            CloseTrade(sc, GetExitReason(exitType));
        }
    }
}
```

### 3. Indicator Integration

Add to `IndicatorManager`:

```cpp
namespace IndicatorKeys {
    // ... existing keys ...
    constexpr auto OVERNIGHT_EXIT_TYPE = "overnight_exit_type";
}

// Initialize in IndicatorManager::Init()
m_indicators[IndicatorKeys::OVERNIGHT_EXIT_TYPE] = 
    std::make_unique<OvernightExitIndicator>(IndicatorKeys::OVERNIGHT_EXIT_TYPE);
```

---

## Testing and Deployment

**Implementation Files:**
- `include/Indicator.h` (lines 987-1074): Enum definitions
- `src/Indicator.cpp` (lines 310-598): TimeOfDayIndicator, HoldingStrategyIndicator, OvernightExitIndicator implementations
- `src/StudyHelperFunctions.cpp` (lines 1155-1294): CalculateHoldingStrategy() integration
- `src/TripleScreen1.cpp`: Strategic overnight decision layer (240-min)
- `src/TripleScreen3.cpp` (lines 931-1005): Tactical overnight exit evaluation (15-min)
- `src/IndicatorManager.cpp` (lines 73, 261): OvernightExitIndicator initialization

**Testing Checklist:**
- ✅ TimeOfDayEnum transitions across 24-hour cycle
- ✅ Golden Rule validation (profitability + strong close in top/bottom 25% of range)
- ✅ Gap detection and windfall exit logic
- ✅ Taylor objective point detection (prev day H/L)
- ✅ 3-10 Oscillator momentum failure detection (3EMA - 16EMA cross)
- ✅ First reaction exit timing (09:30-10:00 ET)
- ✅ PM_RUN_ENTRY conditional logic (must profit by 16:00)

**Deployment:**
- DLL successfully compiled: `build-windows/bin/MindfulTrader.dll`
- Deploy to Sierra Chart: `./deploy_mindfultrader.sh`

---

## References

- **Linda Raschke:** Professional Trader & Educator, LBRGroup
- **Richard D. Wyckoff Taylor:** "The Taylor Trading Technique" (1950s)
- **George Douglass Taylor:** Original Taylor Trading Technique developer
- **Time of Day Concept:** Session-based trading windows for overnight management

---

**End of Document**
