# C++ Market Regime: Complete Implementation Guide

**Purpose**: Unified documentation for implementing Linda Raschke's 5-category Market Regime classification and Dr. Alexander Elder's NH-NL breadth signals in C++ Sierra Chart.

**Created**: December 10, 2025  
**Updated**: December 11, 2025  
**Status**: ✅ **90% COMPLETE** - Core implementation complete, awaiting authentic NYSE data validation

---

## Table of Contents

1. [Overview & Status](#overview--status)
2. [Market Regime Specification](#market-regime-specification)
3. [NH-NL Integration](#nh-nl-integration)
4. [Implementation Guide](#implementation-guide)
5. [Testing & Validation](#testing--validation)
6. [FAQ & References](#faq--references)

---

## 1. Overview & Status

### Implementation Status

**✅ Phase 1 Complete (December 10, 2025):**
- MarketRegimeEnum added to `include/Indicator.h` (6 states)
- MarketRegimeIndicator class created and registered in IndicatorManager
- CalculateMarketRegime() function in `StudyHelperFunctions.cpp`
- Integration in `scsf_Screen1_Impulse()` (TripleScreen1.cpp)
- Bar close optimization: Only calculates at 240-min bar closes
- Uses `sc.Index - 1` for closed bar values (not new bar)
- Fetches indicators from IndicatorManager (Impulse, DailyBias, ATR)
- Sends `market_regime` to Python GUI via JSON payload
- Simplified algorithm without ADX dependency

**✅ Phase 2 Complete (December 11, 2025):**
- NhNlDataLoader class implemented for CSV data access
- CalculateMarketRegime() updated with NH-NL weekly parameter
- NH-NL Signal enum with Elder's 11-state methodology
- CalculateNhNlSignal() function with divergence detection
- 1-day lag implemented to prevent lookahead bias
- CSV export (automatic via IndicatorManager)
- ZMQ payload (automatic via IndicatorManager)

**⏳ Phase 3 Pending:**
- Authentic NYSE+AMEX+NASDAQ data from StockCharts.com ($25/month subscription)
- Validation against Elder's book examples
- Model retraining with 20 indicators (currently 19)
- Python code cleanup after validation

### Quick Start for C++ Developer

**NH-NL Data File:**
- **Path:** `C:\Trading\data\nh_nl_historical.csv`
- **Status:** ✅ Ready (27 KB, 505+ rows, updated daily)
- **Access:** Configure in TripleScreen1 study Input settings
- **Shared:** Accessible by both C++ (Windows) and Python (WSL at `/mnt/c/Trading/data`)

**Current Behavior:**
- Market regime updates 2-3 times per trading day (240-min bar closes: 9:30 AM, 1:30 PM, 4:00 PM ET)
- Regime persists between updates (same value for 4 hours)
- 20 indicators exported: 18 original + market_regime + nh_nl_signal
- Both transmitted via ZMQ and written to TransformerData.csv

**Next Steps:**
1. Subscribe to StockCharts.com for authentic NYSE+AMEX+NASDAQ NH-NL data
2. Download historical data (2-5 years) using symbols: `$NYHL`, `$NAHL`, `$NQHL`
3. Validate Elder signals against book examples
4. Retrain model with 20 indicators
5. Remove temporary Python regime calculation code

---

## 2. Market Regime Specification

### Enum Definition

**Location:** `include/Indicator.h`

```cpp
enum class MarketRegimeEnum : int {
    TRENDING_STRONG = 0,      // Strong trend (ADX > 30, persistent direction)
    TRENDING_IMPULSE = 1,     // Impulse move (ADX 20-30, sharp price move)
    RANGE_DAY = 2,            // Bounded range with directional lean
    CONSOLIDATING_CHOP = 3,   // Sideways, low volatility (ADX < 20, narrow ATR)
    EXTREME_DISLOCATION = 4,  // Sentiment extreme (NH-NL extremes)
    UNDEFINED = -1            // Insufficient data
};
```

**String Mapping (for CSV export):**
```cpp
const char* MarketRegimeToString(MarketRegime regime) {
    switch (regime) {
        case MarketRegime::TRENDING_STRONG: return "trending strong";
        case MarketRegime::TRENDING_IMPULSE: return "trending impulse";
        case MarketRegime::RANGE_DAY: return "range day";
        case MarketRegime::CONSOLIDATING_CHOP: return "consolidating chop";
        case MarketRegime::EXTREME_DISLOCATION: return "extreme dislocation";
        default: return "undefined";
    }
}
```

### Required Indicators

Calculate these indicators before calling regime detection:

| Indicator | Timeframe | Calculation | Purpose |
|:----------|:----------|:------------|:--------|
| **ADX(14)** | 240-min | Welles Wilder's ADX | Trend strength detector |
| **ATR(14)** | 240-min | Average True Range | Volatility measure |
| **NH-NL Daily** | Daily | New Highs - New Lows | Market breadth |
| **NH-NL Weekly** | 5-day sum | Rolling 5-day NH-NL sum | Extreme detection |
| **Impulse System** | 240-min | Already calculated (GREEN/RED/BLUE) | Trend direction |
| **DailyBias** | Daily | Already calculated (BULLISH_ACCEPTANCE, etc.) | Daily structure |

**Data Sources:**
- **ADX, ATR, Impulse**: Calculated from price bars (already in your study)
- **NH-NL Daily/Weekly**: From `nh_nl_historical.csv` (see NH-NL Integration section)
- **DailyBias**: Already calculated in your study

### Calculation Timing

**⚠️ CRITICAL: Calculate ONLY at 240-min bar close** (not every tick, not every 15-min bar)

**Calculation Times** (for ES futures, 9:30 AM - 4:00 PM ET):
- **9:30 AM bar close** → Initial regime at market open
- **1:30 PM bar close** → Mid-day regime update
- **4:00 PM bar close** → Final regime at market close

**Why 240-min?**
- ADX(14) and ATR(14) are calculated on 240-min timeframe
- Prevents false signals from intraday noise
- Regime represents multi-hour/multi-day market character
- Typically persists 3-10 days (TRENDING_STRONG) or 5-20 days (CONSOLIDATING_CHOP)

**Implementation:**
```cpp
void SCSFMyStudy(SCStudyInterfaceRef sc) {
    // Only calculate regime at 240-min bar close
    if (sc.GetBarHasClosedStatus() == BHCS_BAR_HAS_CLOSED) {
        // Get current bar's base graph index (240-min chart)
        int bar_index = sc.Index;
        
        // Calculate regime for this 240-min bar
        MarketRegime regime = CalculateMarketRegime(
            adx_240[bar_index], 
            atr_240[bar_index], 
            atr_avg_20[bar_index],
            nh_nl_daily, 
            nh_nl_weekly,
            impulse_240[bar_index], 
            daily_bias[bar_index]
        );
        
        // Store regime for this bar
        regime_subgraph[bar_index] = (float)regime;
        
        // Send to GUI (if real-time bar)
        if (bar_index == sc.ArraySize - 1) {
            SendRegimeToGUI(regime);
        }
    }
}
```

**Regime Update Frequency:**
- **Historical backfill**: Calculate once per 240-min bar (stable, no changes)
- **Real-time trading**: Recalculate at each 240-min bar close (2-3 times per day)
- **Intraday behavior**: Regime remains constant between 240-min bar closes

**Example Intraday Transition:**
```
Date: 2024-12-10
9:30 AM:  CONSOLIDATING_CHOP (ADX=18, ATR=0.7x avg)
          ↓ [All 15-min bars use this regime until 1:30 PM]
1:30 PM:  TRENDING_IMPULSE (ADX=24, ATR=1.4x avg) ← Breakout detected
          ↓ [All 15-min bars use this regime until 4:00 PM]
4:00 PM:  TRENDING_STRONG (ADX=31, ATR=1.5x avg) ← Trend accelerated
```

**Performance Note:** Calculating regime only at 240-min bar close (2-3 times/day) vs every tick (thousands/day) = **99.9% CPU savings**

### Detection Algorithm

#### Phase 1 Implementation (Current)

**Simplified algorithm without ADX** (uses Impulse persistence as proxy):

```cpp
MarketRegimeEnum CalculateMarketRegime(SCStudyInterfaceRef sc, int barIndex) {
    // Fetch from IndicatorManager
    auto longImpulse = IndicatorManager::Instance().GetIndicator<Impulse>(IndicatorKeys::LONG_IMP);
    auto dailyBias = IndicatorManager::Instance().GetIndicator<DailyBiasIndicator>(IndicatorKeys::DAILY_BIAS);
    auto intermAction = IndicatorManager::Instance().GetIndicator<IntermediateMarketAction>(IndicatorKeys::INTERM_MKT_ACTION);
    
    ImpulseEnum impulse_240 = longImpulse->Value();
    DailyBiasEnum daily_bias = dailyBias->Value();
    float atr_current = intermAction->atr();
    
    // Priority 1: EXTREME_DISLOCATION (NH-NL weekly extremes)
    // Phase 1: Skipped (no CSV integration yet)
    
    // Priority 2: TRENDING_STRONG (persistent Impulse)
    if (impulse_240 == ImpulseEnum::GREEN || impulse_240 == ImpulseEnum::RED) {
        return MarketRegimeEnum::TRENDING_STRONG;
    }
    
    // Priority 3: TRENDING_IMPULSE (ATR expansion)
    if (atr_current > (atr_avg * 1.3f)) {
        return MarketRegimeEnum::TRENDING_IMPULSE;
    }
    
    // Priority 4: CONSOLIDATING_CHOP (ATR contraction)
    if (atr_current < (atr_avg * 0.8f)) {
        return MarketRegimeEnum::CONSOLIDATING_CHOP;
    }
    
    // Priority 5: RANGE_DAY (neutral/rejection daily bias)
    if (daily_bias == DailyBiasEnum::VALUE_AREA_NEUTRAL ||
        daily_bias == DailyBiasEnum::BULLISH_REJECTION_TEST ||
        daily_bias == DailyBiasEnum::BEARISH_REJECTION_TEST) {
        return MarketRegimeEnum::RANGE_DAY;
    }
    
    // Default fallback
    return MarketRegimeEnum::RANGE_DAY;
}
```

#### Phase 2 Target Algorithm (Future)

**Full algorithm with ADX and NH-NL integration:**

```cpp
MarketRegime CalculateMarketRegime(
    float adx_240,              // ADX(14) on 240-min chart
    float atr_240,              // ATR(14) on 240-min chart
    float atr_avg_20,           // 20-period moving average of ATR(14)
    int nh_nl_daily,            // Daily NH - NL value
    int nh_nl_weekly,           // 5-day sum of NH-NL
    ImpulseSystemEnum impulse_240,  // 240-min Impulse (GREEN/RED/BLUE)
    DailyBiasEnum daily_bias    // Daily structure bias
) {
    // Priority 1: EXTREME_DISLOCATION (most important, rare)
    if (abs(nh_nl_weekly) > 4000) {
        return MarketRegime::EXTREME_DISLOCATION;
    }
    
    // Priority 2: TRENDING_STRONG (Holy Grail conditions)
    if (adx_240 > 30.0f && 
        (impulse_240 == ImpulseSystemEnum::GREEN || impulse_240 == ImpulseSystemEnum::RED)) {
        return MarketRegime::TRENDING_STRONG;
    }
    
    // Priority 3: TRENDING_IMPULSE (momentum breakout)
    if (adx_240 >= 20.0f && adx_240 <= 30.0f &&
        (impulse_240 == ImpulseSystemEnum::GREEN || impulse_240 == ImpulseSystemEnum::RED) &&
        atr_240 > (atr_avg_20 * 1.3f)) {  // ATR expansion (30% above average)
        return MarketRegime::TRENDING_IMPULSE;
    }
    
    // Priority 4: CONSOLIDATING_CHOP (low volatility, sideways)
    if (adx_240 < 20.0f && 
        atr_240 < (atr_avg_20 * 0.8f)) {  // ATR contraction (20% below average)
        return MarketRegime::CONSOLIDATING_CHOP;
    }
    
    // Priority 5: RANGE_DAY (bounded but directional)
    if (adx_240 < 25.0f &&
        (daily_bias == DailyBiasEnum::VALUE_AREA_NEUTRAL ||
         daily_bias == DailyBiasEnum::BULLISH_REJECTION_TEST ||
         daily_bias == DailyBiasEnum::BEARISH_REJECTION_TEST)) {
        return MarketRegime::RANGE_DAY;
    }
    
    // Default: RANGE_DAY (fallback if no clear classification)
    return MarketRegime::RANGE_DAY;
}
```

### Threshold Summary

| Regime | Conditions |
|:-------|:-----------|
| **EXTREME_DISLOCATION** | `abs(nh_nl_weekly) > 4000` (Elder's capitulation threshold) |
| **TRENDING_STRONG** | `ADX > 30` AND `Impulse = GREEN/RED` (Holy Grail environment) |
| **TRENDING_IMPULSE** | `ADX 20-30` AND `Impulse = GREEN/RED` AND `ATR > 1.3 × ATR_avg` (breakout) |
| **CONSOLIDATING_CHOP** | `ADX < 20` AND `ATR < 0.8 × ATR_avg` (compression) |
| **RANGE_DAY** | `ADX < 25` AND `DailyBias = NEUTRAL/REJECTION` (bounded trading) |

**Elder's NH-NL Thresholds** (for reference):
- Daily bullish: `NH-NL > +100`
- Daily bearish: `NH-NL < -100`
- Weekly bull confirmation: `NH-NL_weekly > +2500`
- **Weekly extreme panic**: `NH-NL_weekly < -4000` ← EXTREME_DISLOCATION trigger

### CSV Export Format

#### Single Regime Column (Current)

Add to your `TransformerData.csv` export:

```cpp
// In your CSV export loop (iterating through 15-min bars):
// For each 15-min bar, use the CURRENT 240-min regime value
// Regime only updates at 240-min bar closes, so same value repeats across 16 bars

// Get the current 240-min regime (calculated at last 240-min bar close)
MarketRegime current_regime = GetCurrent240MinRegime();

fprintf(csv_file, "%s,", MarketRegimeToString(current_regime));
```

**⚠️ CRITICAL:** Each row in `TransformerData.csv` represents a **15-min bar**, but the `market_regime` column value only changes every **16 bars** (when 240-min bar closes).

**CSV Output Example** (each row = one 15-min bar):
```csv
Date,Time,Close,Volume,...,market_regime
2024-12-10,09:30:00,4746.00,950000,...,consolidating chop
2024-12-10,09:45:00,4747.25,880000,...,consolidating chop
2024-12-10,10:00:00,4748.50,920000,...,consolidating chop
... (13 more 15-min bars, all with "consolidating chop")
2024-12-10,13:30:00,4758.50,1500000,...,trending impulse  ← Regime updated at 240-min bar close
2024-12-10,13:45:00,4759.25,1450000,...,trending impulse
... (15 more 15-min bars, all with "trending impulse")
2024-12-10,16:00:00,4768.00,1200000,...,trending strong   ← Regime updated at 240-min bar close
```

**Column Name:** `market_regime` (lowercase, underscore separator)

---

## 3. NH-NL Integration

### CSV File Requirements

**File:** `nh_nl_historical.csv` (Python-generated, shared with C++)

**Location:** `C:\Trading\data\nh_nl_historical.csv` (accessible from WSL at `/mnt/c/Trading/data`)

**Status:** ✅ File exists (27 KB, 505+ rows), updated daily

**Format:**
```csv
date,nh_nyse,nl_nyse,diff_nyse,nh_nasdaq,nl_nasdaq,diff_nasdaq
2024-01-02,245,18,227,412,31,381
2024-01-03,198,25,173,367,42,325
...
```

**Columns:**
- `date`: YYYY-MM-DD format
- `nh_nyse`, `nl_nyse`: NYSE New Highs/Lows
- `diff_nyse`: NYSE NH-NL differential
- `nh_nasdaq`, `nl_nasdaq`: NASDAQ New Highs/Lows
- `diff_nasdaq`: NASDAQ NH-NL differential

### C++ Loading Logic

**NhNlDataLoader Class** (implemented):

```cpp
#include <map>
#include <string>
#include <fstream>

struct NhNlData {
    int daily;
    int weekly;
    float sp500_price;
};

class NhNlDataLoader {
private:
    std::map<std::string, NhNlData> nh_nl_cache;
    
public:
    void LoadCsv(const char* csv_path);
    NhNlData GetDataForDate(SCDateTime bar_date);
};

void NhNlDataLoader::LoadCsv(const char* csv_path) {
    std::ifstream file(csv_path);
    std::string line;
    
    // Skip header
    std::getline(file, line);
    
    while (std::getline(file, line)) {
        // Parse: Date,NH_NL_Daily,NH_NL_Weekly,S&P_500
        char date[32];
        int daily, weekly;
        float sp500;
        
        if (sscanf(line.c_str(), "%[^,],%d,%d,%f", 
                   date, &daily, &weekly, &sp500) == 4) {
            NhNlData data = {daily, weekly, sp500};
            nh_nl_cache[std::string(date)] = data;
        }
    }
}

NhNlData NhNlDataLoader::GetDataForDate(SCDateTime bar_date) {
    char date_str[32];
    int year, month, day;
    bar_date.GetDateYMD(year, month, day);
    snprintf(date_str, sizeof(date_str), "%04d-%02d-%02d", year, month, day);
    
    auto it = nh_nl_cache.find(std::string(date_str));
    if (it != nh_nl_cache.end()) {
        return it->second;
    }
    
    // Return neutral if not found
    return {0, 0, 0.0f};
}
```

**Usage in Study:**
```cpp
void SCSFMyStudy(SCStudyInterfaceRef sc) {
    // Load NH-NL data once at startup
    if (sc.Index == 0) {
        g_nhNlLoader.LoadCsv("C:\\Trading\\data\\nh_nl_historical.csv");
    }
    
    // For each bar, get NH-NL (with 1-day lag to prevent lookahead bias)
    SCDateTime previousDay = sc.BaseDateTimeIn[sc.Index] - 1 * DAYS;
    NhNlData nh_nl = g_nhNlLoader.GetDataForDate(previousDay);
    
    // Calculate regime
    MarketRegime regime = CalculateMarketRegime(
        adx_240, atr_240, atr_avg_20,
        nh_nl.daily, nh_nl.weekly,  // ← NH-NL from CSV (1-day lag)
        impulse_240, daily_bias
    );
}
```

### Missing NH-NL Data Handling

```cpp
// If NH-NL data not available for this date
if (nh_nl.daily == 0 && nh_nl.weekly == 0) {
    // Option A: Use UNCLEAR regime
    return MarketRegime::UNDEFINED;
    
    // Option B: Skip EXTREME_DISLOCATION check, continue with ADX/ATR logic
    // (Regime can still be TRENDING_STRONG, CONSOLIDATING_CHOP, etc.)
}
```

**Recommendation:** Use **Option B** during development. EXTREME_DISLOCATION is rare (only during market crashes), so most regimes can be detected without NH-NL.

### NH-NL Signal Enum (20th Indicator)

**Elder's 11-State Breadth Methodology:**

```cpp
enum class NhNlSignalEnum : int {
    UNCLEAR = 0,                  // Daily -100 to +100 (Elder: neutral zone)
    BULLISH_CONFIRMATION = 1,     // Daily > +100 OR Weekly > +2500 (Elder: bull market confirmed)
    BEARISH_CONFIRMATION = -1,    // Daily < -100 (Elder: bears in control)
    BEARISH_DIVERGENCE = -2,      // S&P higher high + NH-NL lower high (Elder: sell signal)
    BULLISH_DIVERGENCE = 2,       // S&P lower low + NH-NL higher low (Elder: buy signal)
    EXTREME_LOWS_BOUNCE = 3,      // Weekly < -4000 then rises (Elder: panic, 1-year rally)
    EXTREME_HIGHS_PEAK = -3,      // S&P new high but weekly < +2500 (Elder: narrow rally)
    NARROWING_RALLY = 4,          // Price rising but breadth deteriorating
    BROADENING_RALLY = 5,         // Price rising with breadth improving
    NARROWING_DECLINE = 6,        // Price falling but breadth improving (exhaustion)
    BROADENING_DECLINE = -4       // Price falling with breadth worsening
};
```

**Elder's Exact Thresholds** (from "The New High - New Low Index" book):
- Daily confirmation: **+100 / -100** (not ±500)
- Weekly bull market: **+2500** ("solid ground, hold longs")
- Weekly panic spike: **-4000** ("mass capitulation, buy signal, rally lasts ~1 year")
- Weekly mini-spike: **-1500** (bull markets only, "good for several months")

**Calculation Logic:**

```cpp
NhNlSignalEnum CalculateNhNlSignal(
    int nh_nl_daily,          // Daily NYSE+AMEX+NASDAQ NH-NL differential
    int nh_nl_weekly,         // 5-day rolling sum (Elder uses 5 trading days)
    int nh_nl_prev_weekly,    // Previous week's sum (for breadth trend)
    float currentPrice,       // Current S&P 500 price
    float recentHigh,         // Highest price in last 20 bars
    float recentLow,          // Lowest price in last 20 bars
    bool priceIsRising        // True if price above 13-EMA
) {
    // Priority 1: EXTREME PANIC SPIKE (Elder's most powerful buy signal)
    if (nh_nl_weekly < -4000) return NhNlSignalEnum::EXTREME_LOWS_BOUNCE;
    
    // Priority 1b: BULL MARKET CONFIRMATION FAILURE
    bool priceNearHigh = (currentPrice >= recentHigh * 0.98f);  // Within 2%
    if (priceNearHigh && nh_nl_weekly < 2500 && nh_nl_weekly < nh_nl_prev_weekly) {
        return NhNlSignalEnum::EXTREME_HIGHS_PEAK;
    }
    
    // Priority 2: ELDER'S DIVERGENCES (Most Powerful Trading Signals)
    bool breadthImproving = nh_nl_weekly > nh_nl_prev_weekly;
    bool breadthDeteriorating = nh_nl_weekly < nh_nl_prev_weekly;
    int breadthChange = nh_nl_weekly - nh_nl_prev_weekly;
    
    bool priceNearLow = (currentPrice <= recentLow * 1.02f);  // Within 2%
    
    // Bearish divergence: Price at high but breadth deteriorating
    if (priceNearHigh && breadthDeteriorating && breadthChange < -500) {
        return NhNlSignalEnum::BEARISH_DIVERGENCE;
    }
    
    // Bullish divergence: Price at low but breadth improving
    if (priceNearLow && breadthImproving && breadthChange > 500) {
        return NhNlSignalEnum::BULLISH_DIVERGENCE;
    }
    
    // Priority 3: BREADTH PARTICIPATION (Elder's Trend Health)
    if (priceIsRising) {
        if (breadthDeteriorating && nh_nl_weekly < nh_nl_prev_weekly - 300) {
            return NhNlSignalEnum::NARROWING_RALLY;
        }
        if (breadthImproving && nh_nl_weekly > 1500 && breadthChange > 200) {
            return NhNlSignalEnum::BROADENING_RALLY;
        }
    } else {
        if (breadthImproving && breadthChange > 300) {
            return NhNlSignalEnum::NARROWING_DECLINE;
        }
        if (breadthDeteriorating && nh_nl_weekly < -1500 && breadthChange < -200) {
            return NhNlSignalEnum::BROADENING_DECLINE;
        }
    }
    
    // Priority 4: ELDER'S DAILY CONFIRMATION BAND
    if (nh_nl_daily > 100 || nh_nl_weekly > 2500) {
        return NhNlSignalEnum::BULLISH_CONFIRMATION;
    }
    if (nh_nl_daily < -100) {
        return NhNlSignalEnum::BEARISH_CONFIRMATION;
    }
    
    // Default: UNCLEAR (Elder's neutral zone)
    return NhNlSignalEnum::UNCLEAR;
}
```

**Critical Implementation: 1-Day Lag**

```cpp
// PREVENT DATA LEAK: Use previous trading day's NH-NL (not current bar's date)
// Elder: "NH-NL is calculated end-of-day. Today's trading uses yesterday's NH-NL."
SCDateTime previousDay = sc.BaseDateTimeIn[barIndex] - 1 * DAYS;
NhNlData nhNlData = s_nhNlLoader->GetDataForDate(previousDay);

// For breadth trend comparison, look back 2 days
SCDateTime twoDaysBack = sc.BaseDateTimeIn[barIndex] - 2 * DAYS;
NhNlData prevNhNlData = s_nhNlLoader->GetDataForDate(twoDaysBack);
```

---

## 4. Implementation Guide

### Phase 1: NH-NL CSV Loader ✅ COMPLETE

**Goal:** C++ can read and lookup NH-NL data

**Implementation:**
- ✅ Created `NhNlDataLoader` class
- ✅ Reads CSV at study initialization
- ✅ Parses and stores in `std::map<SCDateTime, NhNlData>`
- ✅ Implemented `getNhNlForDate(SCDateTime date)` lookup function
- ✅ Handles missing dates (returns default values)
- ✅ Logs statistics (rows loaded, date range covered)

**Files:**
- `include/NhNlDataLoader.h`
- `src/NhNlDataLoader.cpp`
- `src/TripleScreen1.cpp` (instantiates loader at startup)

**C++ CSV Path Configuration:**
```cpp
// In TripleScreen1 study Input settings:
SCString Input_NhNlCsvPath;
Input_NhNlCsvPath.Name = "NH-NL CSV Path";
Input_NhNlCsvPath.SetString("C:\\Trading\\data\\nh_nl_historical.csv");
```

### Phase 2: Update CalculateMarketRegime() ✅ COMPLETE

**Goal:** Add NH-NL integration to existing regime function

**Implementation:**
- ✅ Updated CalculateMarketRegime() signature: added `int nh_nl_weekly` parameter
- ✅ Added Priority 1 logic: EXTREME_DISLOCATION (abs(nh_nl_weekly) > 4000)
- ✅ Kept existing ADX/ATR logic (Priority 2-6)
- ✅ Updated function declaration in StudyHelperFunctions.h
- ✅ Updated call site in TripleScreen1.cpp (passes nh_nl_weekly)
- ✅ Logs regime changes for debugging

**Files Modified:**
- `src/StudyHelperFunctions.cpp`
- `include/StudyHelperFunctions.h`
- `src/TripleScreen1.cpp`

### Phase 3: CSV/ZMQ Export ✅ ALREADY COMPLETE

**Status:** MarketRegime automatically included in all exports via IndicatorManager registration

**How It Works:**
- `IndicatorManager::GetPayload()` includes ALL indicators in `m_indicators` map
- `DataCollector::WriteHistoricalDataToCsv()` uses dynamic header generation from JSON keys
- No manual export code needed - registration = automatic export

### Phase 4: NH-NL Signal Export ✅ COMPLETE

**Goal:** Compute and send NH-NL signal enum as 20th indicator

**Implementation:**
- ✅ Added NhNlSignalEnum to Indicator.h (11 Elder states)
- ✅ Implemented CalculateNhNlSignal() in StudyHelperFunctions.cpp
- ✅ Created NhNlSignalIndicator class
- ✅ Registered in IndicatorManager (automatic export)
- ✅ Integrated in TripleScreen1 with 1-day lag and price context
- ✅ Export automatic (CSV + ZMQ) via IndicatorManager

**Files Modified:**
- `include/Indicator.h`
- `include/StudyHelperFunctions.h`
- `src/StudyHelperFunctions.cpp`
- `include/IndicatorManager.h`
- `src/IndicatorManager.cpp`
- `src/TripleScreen1.cpp`

### File Organization

```
MindfulTrader/
├── include/
│   ├── NhNlDataLoader.h          // CSV reader + date lookup ✅
│   ├── Indicator.h               // MarketRegimeEnum ✅ + NhNlSignalEnum ✅
│   ├── IndicatorManager.h        // Regime + NH-NL signal storage ✅
│   └── StudyHelperFunctions.h    // CalculateMarketRegime() ✅ + CalculateNhNlSignal() ✅
│
├── src/
│   ├── NhNlDataLoader.cpp        // ✅ IMPLEMENTED
│   ├── StudyHelperFunctions.cpp  // ✅ UPDATED
│   ├── TripleScreen1.cpp         // ✅ UPDATED
│   ├── TripleScreen2.cpp         // (May need updates if exporting CSV)
│   ├── DataCollector.cpp         // ✅ Auto-export via IndicatorManager
│   └── MindfulSocketZMQ.cpp      // ✅ Auto-export via IndicatorManager
│
└── C:\Trading\data\             // Shared data folder
    └── nh_nl_historical.csv      // ✅ 27KB, 505+ rows, updated daily
```

### Implementation Checklist

#### ✅ Phase 1 Complete
- [x] Add `MarketRegimeEnum` to `include/Indicator.h`
- [x] Implement `CalculateMarketRegime()` function
- [x] Register `MarketRegimeIndicator` in IndicatorManager
- [x] Integrate in `scsf_Screen1_Impulse()`
- [x] Add bar close optimization (BHCS_BAR_HAS_CLOSED)
- [x] Use `sc.Index - 1` for closed bar values
- [x] Fetch indicators from IndicatorManager
- [x] Send `market_regime` to GUI via JSON

#### ✅ Phase 2 Complete
- [x] Create NhNlDataLoader class
- [x] Load NH-NL CSV at startup
- [x] Add `getNhNlForDate()` lookup function
- [x] Update CalculateMarketRegime() with NH-NL parameter
- [x] Integrate EXTREME_DISLOCATION detection
- [x] Add 1-day lag to prevent lookahead bias

#### ✅ Phase 3 Already Done
- [x] CSV export (automatic via IndicatorManager)
- [x] ZMQ payload (automatic via IndicatorManager)

#### ✅ Phase 4 Complete
- [x] Add NhNlSignalEnum (11 Elder states)
- [x] Implement CalculateNhNlSignal() with divergence detection
- [x] Register in IndicatorManager
- [x] Integrate in TripleScreen1
- [x] 1-day lag implementation
- [x] Export automatic (CSV + ZMQ)

#### ⏳ Phase 5 Pending (Data Validation)
- [ ] Subscribe to StockCharts.com ($25/month)
- [ ] Download authentic NYSE+AMEX+NASDAQ NH-NL data
- [ ] Validate Elder signals against book examples
- [ ] Test with 6 unit test cases
- [ ] Verify regime distribution is realistic
- [ ] Model retraining with 20 indicators
- [ ] Remove temporary Python regime calculation

---

## 5. Testing & Validation

### Unit Test Cases

Test your `CalculateMarketRegime()` function with these scenarios:

| Test Case | ADX | ATR/Avg | NH-NL Weekly | Impulse | Expected Regime |
|:----------|:----|:--------|:-------------|:--------|:----------------|
| **Holy Grail Setup** | 35.0 | 1.2 | -250 | GREEN | TRENDING_STRONG |
| **Breakout Move** | 24.0 | 1.5 | +500 | RED | TRENDING_IMPULSE |
| **Chop Zone** | 15.0 | 0.7 | +50 | BLUE | CONSOLIDATING_CHOP |
| **Range Day** | 18.0 | 1.0 | +80 | BLUE | RANGE_DAY |
| **Market Crash** | 28.0 | 1.3 | -4500 | RED | EXTREME_DISLOCATION |
| **Insufficient Data** | 0.0 | 0.0 | 0 | UNDEFINED | UNDEFINED |

**Expected Results:**
```cpp
// Test 1: Holy Grail
regime = CalculateMarketRegime(35.0f, 12.0f, 10.0f, -25, -250, GREEN, BULLISH_ACCEPTANCE);
assert(regime == MarketRegime::TRENDING_STRONG);

// Test 5: Market Crash (NH-NL priority override)
regime = CalculateMarketRegime(28.0f, 13.0f, 10.0f, -450, -4500, RED, BEARISH_ACCEPTANCE);
assert(regime == MarketRegime::EXTREME_DISLOCATION);  // NH-NL overrides ADX
```

### Historical Backfill Validation

After implementing regime calculation:

1. **Export TransformerData.csv** with `market_regime` and `nh_nl_signal` columns
2. **Verify distribution** (Python):
   ```python
   import pandas as pd
   df = pd.read_csv("data/TransformerData.csv")
   print(df['market_regime'].value_counts())
   print(df['nh_nl_signal'].value_counts())
   ```

**Expected Distribution** (approximate):
- **Market Regime:**
  - RANGE_DAY: ~40-50% (most common)
  - CONSOLIDATING_CHOP: ~25-30%
  - TRENDING_IMPULSE: ~10-15%
  - TRENDING_STRONG: ~5-10% (Holy Grail conditions)
  - EXTREME_DISLOCATION: <1% (rare, only during crashes)

- **NH-NL Signal:**
  - UNCLEAR: ~50-60% (neutral zone)
  - BULLISH_CONFIRMATION: ~15-20%
  - BEARISH_CONFIRMATION: ~15-20%
  - Divergences: ~5-10% combined
  - Extremes: <1%

### Performance Considerations

**Memory Usage:**
- NH-NL data: ~2-5 years × 252 trading days = ~1,250 dates × 16 bytes = **~20 KB**

**CPU Usage:**
- Regime calculation is lightweight (5-10 float comparisons per bar)
- NH-NL signal calculation adds minimal overhead
- Calculate only at 240-min bar close (not every tick)
- Negligible impact on Sierra Chart performance

### Error Handling

```cpp
MarketRegime CalculateMarketRegime(...) {
    // Validate inputs
    if (adx_240 < 0.0f || atr_240 < 0.0f || atr_avg_20 <= 0.0f) {
        sc.AddMessageToLog("Invalid ADX/ATR values for regime calculation", 1);
        return MarketRegime::UNDEFINED;
    }
    
    // Check for divide-by-zero
    if (atr_avg_20 == 0.0f) {
        sc.AddMessageToLog("ATR average is zero, cannot calculate regime", 1);
        return MarketRegime::UNDEFINED;
    }
    
    // Normal calculation...
}
```

**Logging** (for debugging):
```cpp
if (sc.Index % 100 == 0) {  // Log every 100 bars
    sc.AddMessageToLog(SCString().Format(
        "Regime: %s, ADX: %.2f, ATR: %.2f, NH-NL: %d/%d",
        MarketRegimeToString(regime), adx_240, atr_240, 
        nh_nl_daily, nh_nl_weekly
    ), 0);
}
```

### Effort Estimate

| Phase | Hours | Status | Complexity |
|-------|-------|--------|------------|
| Phase 1: NH-NL Loader | 6-8 | ✅ **COMPLETE** | Medium |
| Phase 2: Update CalculateMarketRegime() | 2-3 | ✅ **COMPLETE** | Low |
| Phase 3: CSV Export | 0 | ✅ **ALREADY DONE** | N/A |
| Phase 4: ZMQ Integration | 0 | ✅ **ALREADY DONE** | N/A |
| Phase 5: NH-NL Signal | 6-8 | ✅ **COMPLETE** | Medium |
| Testing & Validation | 6-8 | ⏳ **PENDING DATA** | Medium |
| Model Retraining | 2-3 | ⏳ **AFTER DATA** | Low |
| Documentation | 2-3 | ✅ **COMPLETE** | Low |
| **TOTAL** | **25-35 hours** | **90% Complete** | **Medium** |

**Actual Time:** ~18 hours (Phases 1-2-5 complete, Phases 3-4 were automatic)

---

## 6. FAQ & References

### Frequently Asked Questions

**Q: What if NH-NL data is missing for a date?**  
A: Algorithm falls back to ADX/ATR-based detection. Only EXTREME_DISLOCATION requires NH-NL.

**Q: Can regime be calculated on 60-min or 15-min charts?**  
A: Algorithm designed for 240-min (4-hour) timeframe. Lower timeframes may give false signals (too noisy).

**Q: How often does regime change?**  
A: Typically 1-3 times per week. TRENDING_STRONG persists 3-10 days, CONSOLIDATING_CHOP 5-20 days.

**Q: Does regime change intraday?**  
A: Yes, but **only at 240-min bar closes** (9:30 AM, 1:30 PM, 4:00 PM ET). Between these times, regime remains constant.

**Q: Should I calculate regime on every tick or every 15-min bar?**  
A: **NO.** Only calculate at **240-min bar close**. ADX/ATR are 240-min indicators. Calculating more frequently causes false signals and wastes CPU.

**Q: What if ADX or ATR is not available?**  
A: Return `MarketRegime::UNDEFINED`. Regime detection requires both indicators.

**Q: Can I use MACD instead of ADX?**  
A: No. Linda Raschke specifically uses ADX for trend strength. MACD is a momentum indicator, not a trend strength measure.

**Q: Where do I get authentic NH-NL data?**  
A: Subscribe to StockCharts.com ($25/month). Download symbols: `$NYHL` (NYSE), `$NAHL` (AMEX), `$NQHL` (NASDAQ).

### Key Challenges & Solutions

#### Challenge 1: NH-NL CSV Path ✅ RESOLVED
**Solution:** Study input setting with default path `C:\Trading\data\nh_nl_historical.csv`, shared with Python via `/mnt/c/Trading/data`

#### Challenge 2: NH-NL Data Lags by 1 Day
**Solution:** Use yesterday's NH-NL for today with 1-day lag documentation. Prevents lookahead bias.

#### Challenge 3: ADX/ATR Require History
**Solution:** First 5 days of regime = UNDEFINED (warm-up period). After 30 bars × 240 min = 5 trading days, regime becomes valid.

#### Challenge 4: Python Removal Timing
**Solution:** Keep Python calculation until C++ validated with authentic data. Mark as "DEPRECATED - C++ now provides" in comments after validation.

### References

**Linda Raschke's Framework:**
- ADX > 30: Holy Grail setup environment (strong trend)
- ADX < 20: Avoid trend-following trades (chop)
- ATR expansion/contraction: Regime transitions

**Alexander Elder's NH-NL Thresholds:**
- Daily neutral: -100 to +100
- Daily bullish: > +100
- Weekly extreme panic: < -4,000 (capitulation)
- Weekly bull confirmation: > +2,500

**Related Documentation:**
- `MARKET_REGIME.md` - Strategy overview
- `NH_NL_DATA_ACQUISITION.md` - Data sourcing guide
- `SCORING_LOGIC_V2.md` - How regime affects scoring
- `CPP_DATA_EXPORT_LOGIC.md` - TransformerData.csv export
- `ELDER_NH_NL_METHODOLOGY.md` - Elder's complete methodology

### Success Criteria

**Minimum Viable (Must Have):**
- [x] C++ exports MarketRegime in TransformerData.csv ✅
- [x] C++ sends MarketRegime in 200-bar sequence ✅
- [x] C++ exports NH-NL Signal in TransformerData.csv ✅
- [x] C++ sends NH-NL Signal in 200-bar sequence ✅
- [x] Elder's exact methodology implemented (11 breadth states) ✅
- [x] 1-day lag prevents lookahead bias ✅
- [ ] Validate against Elder book examples - **PENDING STOCKCHARTS DATA**
- [ ] Model retrains with 20 indicators - **PENDING DATA**
- [x] No crashes during replay or live session ✅

**Production Ready (Should Have):**
- [ ] Regime distribution is realistic - **PENDING VALIDATION**
- [x] Warm-up period documented and handled ✅
- [x] NH-NL data update procedure documented ✅
- [x] Error handling for missing NH-NL dates ✅
- [x] Logging for regime changes ✅

**Nice to Have:**
- [ ] GUI displays current regime visually - **FUTURE**
- [ ] Regime change alerts - **FUTURE**
- [ ] Historical regime statistics - **FUTURE**

---

**Status:** ✅ **90% COMPLETE** - Ready for authentic data & final validation  
**Next Action:** Subscribe to StockCharts.com, download authentic NYSE+AMEX+NASDAQ NH-NL data  
**Completed:** Core infrastructure, NH-NL loader, regime calculation, Elder signal detection, automatic export  
**Pending:** Authentic data integration, Elder signal validation, model retraining  

---

*This guide supersedes the old split spec/integration regime docs.*

*Canonical cross-project runtime reference: `../../docs/HMM_RUNTIME_REFERENCE.md`*
