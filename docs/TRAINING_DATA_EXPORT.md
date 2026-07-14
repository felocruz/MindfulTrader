# MindfulTrader Training Data Export Workflow

**Last Updated:** December 28, 2025

## 1. Overview

This document outlines the definitive architecture and user workflow for generating time-aligned, historical training data for the Transformer neural network. The primary goal is to capture the complete state of all indicators across three different timeframes (240-min, 60-min, 15-min) at the precise moment each 15-minute bar closes.

This process leverages Sierra Chart's **Manual Chart Replay** with **synchronized multi-chart back testing** to ensure all timeframe dependencies are calculated in proper order. The export produces a JSONL (JSON Lines) file where each line represents one 15-minute bar with all indicators and OHLCV data.

**Critical:** Target labels are **NOT** included in the C++ export. Python post-processing adds target labels using pattern detection logic (see [TARGET_LABEL_GENERATION_SPEC.md](TARGET_LABEL_GENERATION_SPEC.md) and [BACKTESTING_FRAMEWORK.md](../../docs/BACKTESTING_FRAMEWORK.md)).

**Recent Improvements (Dec 2025):**
- ✅ Cross-chart index alignment fixes using `GetNearestMatchForDateTimeIndex()`
- ✅ Session-based gap detection using `GetTradingDayDate()`
- ✅ 3-10 Oscillator cross and divergence detection implemented
- ✅ Screen3 same-chart reference fix

---

## 2. Core Architecture: Standalone Data Collection Study

The architecture is built around a dedicated **`scsf_DataCollector`** study that is completely separate from the production trading system (`scsf_MindfulTrader`).

### **Singleton State Holder (`IndicatorManager`)**

The existing `IndicatorManager` class acts as the central, in-memory repository for the *current state* of all indicators. As the replay runs, the indicator calculation studies on their respective charts (240-min, 60-min, 15-min) naturally update this singleton, ensuring it always holds the latest cross-timeframe values at any given moment.

### **Data Collector (`scsf_DataCollector`)**

This study runs on the 15-minute chart and is responsible for:
- Providing an "Arm/Disarm" menu control for user-initiated export
- Opening/managing the output file stream (`TransformerData.jsonl`)
- On each 15-minute bar (when armed), reading the complete indicator state from `IndicatorManager`
- Adding OHLCV data (datetime, open, high, low, close, volume)
- Writing one JSON line per bar to the output file
- Progress logging every 1000 bars

**Key Design Principles:**
- **NO** dependencies on `PositionManager`, `RiskManager`, or ZMQ communication classes
- **NO** target label generation (Python handles this post-processing)
- **NO** trading logic or order execution
- Pure data export function only

### **Indicator Calculation Studies**

The existing `scsf_Screen1_Impulse` (240-min), `scsf_Screen2_MACD_Keltner` (60-min), and `scsf_Screen3_Stochastic` (15-min) studies continue to perform their calculations and update the shared `IndicatorManager` singleton, exactly as they do during live trading.

---

## 3. Sierra Chart Replay Configuration (Critical)

### Why Multi-Chart Synchronized Replay is Required

The `scsf_DataCollector` runs on the 15-min chart but needs indicator values calculated from:
- **Screen 1 (240-min)**: Impulse colors, HoldingStrategy, long-term trend
- **Screen 2 (60-min)**: MACD, Keltner Channels, intermediate trend  
- **Screen 3 (15-min)**: Stochastic, momentum, pattern detection

If you only replay ONE chart, the other timeframes freeze and the indicator values become stale or invalid → **garbage training data**.

### Correct Method: Manual Replay with Chartbook Synchronization

According to Sierra Chart documentation:

> "When you are replaying multiple charts at the same time to perform a back test, then you need to use the **Replay Back Testing - Manual** method...With a synchronized Back Test, **the dependency between the charts is determined and the calculations are done in the proper order**. This ensures consistency and accuracy."

Sierra Chart automatically detects that Screen 3 (DataCollector) references Screen 1 and Screen 2, and calculates them in dependency order: Screen 1 → Screen 2 → Screen 3.

---

## 4. User Workflow: Step-by-Step

### Prerequisites

1. **Chartbook Setup:**
   - Chart 1: 240-min with `scsf_Screen1_Impulse` study
   - Chart 2: 60-min with `scsf_Screen2_MACD_Keltner` study
   - Chart 3: 15-min with `scsf_Screen3_Stochastic` + **`scsf_DataCollector`** studies

2. **Historical Data:**
   - Set **Chart >> Chart Settings >> Days to Load** to sufficient history (e.g., 365+ days)
   - Ensure all three charts have loaded the same date range

3. **Disconnect from Data Feed:**
   - **File >> Disconnect** (essential for synchronized back test)

### Replay Execution Steps

1. **Arm the Data Collector:**
   - Right-click on the 15-minute chart
   - Select **"Phase 1: Arm Data Export"** from the menu
   - Check **Window >> Message Log** for confirmation: `"DataCollector: Data export ARMED. Start replay to generate file."`

2. **Open Replay Control Panel:**
   - Select **Chart >> Replay Chart >> Replay Chart (Control Panel)**

3. **Configure Replay Window:**
   - **Charts to Replay**: Select **"For All Charts in Chartbook"**
   - **Replay Mode**: Select **"Accurate Trading System Back Test Mode"**
   - **Use Start Date-Time**: **Uncheck this option** (replay from earliest data)
   - **Skip Empty Records**: Check (recommended)

4. **Start Synchronized Replay:**
   - Scroll the 15-minute chart to the desired starting point (or beginning)
   - Press the **Play** button on the Replay Window
   - When prompted, enter **Processing Step in Seconds**:
     * Suggested: **60 seconds** (good balance of speed vs. accuracy)
     * Or 25% of smallest bar timeframe = ~240 seconds for 15-min bars
     * Smaller = more accurate but slower; larger = faster but less accurate

5. **Monitor Progress:**
   - Sierra Chart will be busy during replay
   - Progress logged every 1000 bars in Message Log
   - Press **Stop** button on Replay Window to interrupt if needed
   - Diagnostic logs appear every 1000 bars showing MACD/Raschke indicator values

6. **Verify Completion:**
   - When replay finishes, check Message Log for bar count
   - Press **Stop** on Replay Window
   - Right-click 15-minute chart → **"Phase 1: Disarm Data Export"**

7. **Retrieve the Data:**
   - Navigate to `C:\SierraChart2\Data\` directory
   - File: **`TransformerData.jsonl`**
   - Format: One JSON object per line (JSONL format)
   - Each line contains: `datetime`, `open`, `high`, `low`, `close`, `volume`, plus all indicator values
   - **NO target field** - Python post-processing adds this (see [BACKTESTING_FRAMEWORK.md](../../docs/BACKTESTING_FRAMEWORK.md))

---

## 5. Technical Implementation Details

### `scsf_DataCollector` Study

**Location:** [src/SCStudies.cpp](../src/SCStudies.cpp#L425-L590)

**Persistent State:**
- `DC_EXPORT_ARMED_FLAG_ID`: Tracks armed/disarmed state (1 = armed, 0 = disarmed)
- `DC_FILE_OPENED_FLAG_ID`: Tracks whether file stream is open
- `DC_EXPORT_MENU_ID`: Stores menu item ID for arm/disarm toggle
- `DC_MENU_EXISTS_ID`: One-time flag for menu initialization

**File Handling:**
- Uses `static std::ofstream g_dataCollectorFileStream` for file operations
- File opened on first bar when armed (`UpdateStartIndex == 0`)
- Output path: `C:\SierraChart2\Data\TransformerData.jsonl`
- File mode: Truncate on open (each replay produces fresh export)
- File closed on study shutdown (`LastCallToFunction`)

**Export Logic (per bar when armed):**
```cpp
// 1. Update all indicators
IndicatorManager::Instance().UpdateBarContext(sc);

// 2. Update TimeOfDay indicator
timeOfDayIndicator->SetFromDateTime(sc.BaseDateTimeIn[sc.Index]);

// 3. Get indicator payload (pure indicators, no target)
nlohmann::json payload = IndicatorManager::Instance().GetPayload(sc, false);

// 4. Add OHLCV data
payload["datetime"] = "<YYYY-MM-DD HH:MM:SS>";
payload["open"] = sc.Open[sc.Index];
payload["high"] = sc.High[sc.Index];
payload["low"] = sc.Low[sc.Index];
payload["close"] = sc.Close[sc.Index];
payload["volume"] = sc.Volume[sc.Index];

// 5. Write JSON line (NO target label)
g_dataCollectorFileStream << payload.dump() << std::endl;
```

**Critical Design Constraints:**
- **NO** ZMQ queue initialization (no socket communication needed)
- **NO** `PositionManager` or `RiskManager` references
- **NO** trading logic or order execution
- Relies entirely on shared `IndicatorManager` singleton

### `IndicatorManager` (State Holder)

**Role:** Centralized in-memory store for all indicator values across all timeframes

**Key Methods:**
- `UpdateBarContext(sc)`: Updates timestamp, current price, and daily cache for current bar
- `GetPayload(sc, false)`: Returns `nlohmann::json` object with complete indicator state
- Shared singleton accessed by all studies (Screen 1/2/3 + DataCollector)

**Important:** `IndicatorManager` is initialized by `scsf_MindfulTrader` on first chart load. If running DataCollector standalone, the IndicatorManager is used but does NOT require queue initialization (no publishing during data collection).

### Indicator Calculation Studies

**Screen 1 (`scsf_Screen1_Impulse`)**: 240-min chart
- Calculates Impulse colors (Bull/Bear/Neutral)
- Updates HoldingStrategy indicator
- Long-term MACD histogram

**Screen 2 (`scsf_Screen2_MACD_Keltner`)**: 60-min chart  
- MACD line/signal/histogram
- Keltner Channels (upper/lower/middle)
- Intermediate timeframe trend

**Screen 3 (`scsf_Screen3_Stochastic`)**: 15-min chart
- Stochastic oscillator
- Linda Raschke pattern detection
- Market regime classification

All three studies update the shared `IndicatorManager` singleton on each bar calculation.

---

## 6. Data Format Specification

### JSONL Output Format

**File:** `C:\SierraChart2\Data\TransformerData.jsonl`

**Structure:** One JSON object per line (newline-delimited JSON)

**Example Line:**
```json
{"datetime":"2024-01-15 09:30:00","open":4500.25,"high":4502.50,"low":4499.75,"close":4501.00,"volume":12500,"long_macd":5,"long_macd_histogram":2.34,"raschke_strategy_setup":0,"keltner_position":1,"stochastic_k":45.6,"stochastic_d":42.1,...}
```

**Fields:**
- `datetime`: ISO format timestamp (YYYY-MM-DD HH:MM:SS)
- `open`, `high`, `low`, `close`: Bar OHLC prices
- `volume`: Bar volume
- All indicator values as returned by `IndicatorManager::GetPayload()`
- **NO `target` field** - Python adds this during post-processing

### Python Post-Processing

See [BACKTESTING_FRAMEWORK.md](../../docs/BACKTESTING_FRAMEWORK.md) and [TARGET_LABEL_GENERATION_SPEC.md](TARGET_LABEL_GENERATION_SPEC.md) for:
- Target label generation logic (pattern-based, lookahead-based, or hybrid)
- Quality score calculation
- Train/validation/test split
- Feature engineering for Transformer input

---

## 7. Troubleshooting

### Problem: Screen 1/2 Indicators Show -999 in Diagnostic Logs

**Cause:** Other charts are not replaying (frozen timeframes)

**Solution:** 
- Verify **"For All Charts in Chartbook"** is selected on Replay Window
- Ensure all three charts are in the same Chartbook
- Check **"Accurate Trading System Back Test Mode"** is selected

### Problem: Export File is Empty or Missing

**Cause:** DataCollector was not armed, or file path is incorrect

**Solution:**
- Check Message Log for "Data export ARMED" confirmation
- Verify output path exists: `C:\SierraChart2\Data\`
- Check file permissions (write access)

### Problem: Replay Runs Very Slowly

**Cause:** Time step increment too small, or too much historical data

**Solution:**
- Increase "Processing Step in Seconds" (try 120 or 240 seconds)
- Reduce "Days to Load" in Chart Settings
- Close other Sierra Chart windows/charts
- Disable unnecessary studies on charts

### Problem: Inconsistent Indicator Values Between Replay Runs

**Cause:** Charts not synchronized, or replay mode incorrect

**Solution:**
- Always use **"Accurate Trading System Back Test Mode"**
- Disconnect from data feed before replay (**File >> Disconnect**)
- Ensure all charts start from same date range
- Use same Processing Step value for consistent timing

### Problem: Missing Multi-Timeframe Indicators

**Cause:** Study references not configured, or dependency chain broken

**Solution:**
- Verify Screen 3 studies reference Screen 1/2 via Study IDs
- Check `Input.SetStudyID()` configuration in each study
- Ensure all studies are enabled (not disabled in Study Settings)

---

## 8. Performance Optimization

### Fast Export (Recommended for Most Use Cases)

- **Processing Step**: 60-240 seconds
- **Days to Load**: 180-365 days
- **Expected Time**: Minutes to tens of minutes
- **Accuracy**: High (sufficient for neural network training)

### Maximum Accuracy Export (Slow)

- **Processing Step**: 15-30 seconds (25% of bar timeframe)
- **Days to Load**: 365+ days
- **Expected Time**: Hours
- **Accuracy**: Maximum (every price tick processed)
- **Use Case:** Final validation run, production model training

### Trade-offs

From Sierra Chart documentation:
> "The smaller the timeframe, the more accurate the Back Test, however it will take longer to complete. The larger the timeframe, the faster the Back Test will be but it will be less accurate."

For neural network training, a Processing Step of 60 seconds typically provides an excellent balance between export speed and data quality.

---

## 9. Related Documentation

- **[BACKTESTING_FRAMEWORK.md](../../docs/BACKTESTING_FRAMEWORK.md)** - Three-phase backtesting plan (Data Export → Pure NN → Risk-Managed)
- **[TARGET_LABEL_GENERATION_SPEC.md](TARGET_LABEL_GENERATION_SPEC.md)** - Target label generation approaches and consistency requirements
- **[STRATEGIES_PARAMETERS_REFERENCE.md](STRATEGIES_PARAMETERS_REFERENCE.md)** - Pattern detection rules and position management parameters
- **[GUI_INDICATOR_REFERENCE.md](GUI_INDICATOR_REFERENCE.md)** - Complete indicator payload documentation

---

## 10. Future Enhancements

### Multi-Symbol Export

Currently supports single symbol per export. Future enhancement could:
- Export multiple symbols in parallel
- Add `symbol` field to JSON output
- Batch process entire watchlist

### Incremental Export

Currently truncates file on each replay. Future enhancement could:
- Append new data only (delta export)
- Resume from last exported datetime
- Reduce replay time for daily updates

### Real-Time Export During Live Trading

Currently replay-only. Future enhancement could:
- Export live bar-close data during production trading
- Continuous model retraining pipeline
- Online learning feedback loop
