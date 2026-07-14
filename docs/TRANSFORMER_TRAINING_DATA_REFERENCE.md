# TransformerAgent Training Data Reference

**Purpose**: Complete mapping of C++ indicators and features exported via DataCollectorStudy for TransformerAgent ML training.

**Version**: 2.0 (Elite v2.0 Protocol)
**Date**: January 18, 2026
**Maintainer**: MindfulTrader Development Team

**IMPORTANT DISTINCTION:**
- **This Document**: Training data export format (DataCollectorStudy → TransformerData.jsonl) for ML model training
- **GUI_INDICATOR_REFERENCE.md**: Real-time trading indicators (MindfulTrader → GUI) for live execution

---

## Table of Contents
1. [Overview](#overview)
2. [Data Export Workflow](#data-export-workflow)
3. [JSON Structure](#json-structure)
4. [OHLCV Fields](#ohlcv-fields)
5. [Phase 1: Bar-Level Context Metrics](#phase-1-bar-level-context-metrics)
6. [Phase 2: Triple Screen Indicators](#phase-2-triple-screen-indicators)
7. [Phase 3: Pattern Detection Indicators](#phase-3-pattern-detection-indicators)
8. [Phase 4: Strategy & Regime Indicators](#phase-4-strategy--regime-indicators)
9. [**Elite v2.0: Statistical Context & Quality Scores**](#elite-v20-statistical-context--quality-scores) ⭐ NEW
10. [Phase 5: Training-Specific Fields](#phase-5-training-specific-fields)
11. [Data Augmentation Fields](#data-augmentation-fields)
12. [Field Summary Table](#field-summary-table)
13. [Python Augmentation Process](#python-augmentation-process)
14. [Related Documentation](#related-documentation)

---

## Overview

The DataCollectorStudy (`src/DataCollectorStudy.cpp`) exports comprehensive training data for the TransformerAgent ML model. This data captures the complete market state and indicator configuration at each 15-minute bar close during historical replay.

**Data Flow**: Sierra Chart Replay → DataCollectorStudy → TransformerData.jsonl → Python Augmentation → ML Training

**Key Differences from Real-Time Trading:**

| Aspect | Real-Time Trading (GUI) | Training Data Export (TransformerAgent) |
|--------|-------------------------|----------------------------------------|
| **Purpose** | Execute trades in live market | Train ML model on historical data |
| **Update Frequency** | Every bar close (live) | Replay mode only |
| **Data Structure** | Nested JSON (screen1/screen2/screen3/general) | Flat JSON (all fields at root level) |
| **Side Field** | Current position (FLAT/LONG/SHORT) | Always FLAT (0) during export |
| **Holding Strategy** | Single value for current position | Three values (flat/long/short scenarios) |
| **Target Label** | N/A (trading decisions) | Added by Python (outcome classification) |
| **Activation** | Chart loads and runs | Manual arm + replay required |

---

## Data Export Workflow

### Activation Steps
1. Load 15-minute chart (Screen 3) with DataCollectorStudy applied
2. Use Chart Shortcut Menu: "Phase 1: Arm Data Export"
3. Start historical replay (Ctrl+R)
4. Study exports one JSON line per bar close to `C:\SierraChart2\Data\TransformerData.jsonl`
5. After replay completes, stop and disarm export

### Data Collection Process
```
Bar Close → UpdateBarContext() → CachePayload() → Export JSONL Line
```

Each line contains:
- **OHLCV data**: Price and volume for current bar
- **All indicators**: From IndicatorManager (via GetPayload())
- **Context metrics**: Elite enhancements (percentiles, gaps, distances)
- **Hypothetical strategies**: holding_strategy_long, holding_strategy_short
- **Pattern metadata**: is_trend_following, trend_strength

---

## JSON Structure

Unlike the nested GUI payload (screen1/screen2/screen3/general), training data uses a **flat structure** with all fields at the root level:

```json
{
  "datetime": "2023-07-26 09:45:00",
  "open": 4588.25,
  "high": 4589.0,
  "low": 4586.75,
  "close": 4588.5,
  "volume": 12543,
  "long_macd": 4,
  "raschke_strategy_setup": 0,
  "kangaroo_tail": 0,
  "side": 0,
  "holding_strategy": 5,
  "holding_strategy_long": 5,
  "holding_strategy_short": 5,
  "is_trend_following": false,
  "close_percentile": 77.78,
  ...
}
```

**Note**: Python augmentation will add `target` field based on forward-looking price movement.

---

## OHLCV Fields

### 1. datetime
- **Type**: String
- **Format**: `"YYYY-MM-DD HH:MM:SS"`
- **Description**: Bar timestamp (15-minute intervals)
- **Example**: `"2023-07-26 09:45:00"`
- **Usage**: Index for time-series analysis, session detection

### 2. open
- **Type**: Float
- **Description**: Opening price of 15-minute bar
- **Example**: `4588.25`

### 3. high
- **Type**: Float
- **Description**: Highest price during 15-minute bar
- **Example**: `4589.0`

### 4. low
- **Type**: Float
- **Description**: Lowest price during 15-minute bar
- **Example**: `4586.75`

### 5. close (also exported as "last")
- **Type**: Float
- **Description**: Closing price of 15-minute bar
- **Example**: `4588.5`
- **Usage**: Primary price for indicator calculations

### 6. volume
- **Type**: Integer (int64)
- **Description**: Volume traded during 15-minute bar
- **Example**: `12543`
- **Usage**: Conviction metric for breakouts and pattern validation

---

## Phase 1: Bar-Level Context Metrics

Elite enhancement features that provide statistical context for each bar.

### 7. close_percentile
- **Type**: Float (0-100)
- **Description**: Position of close within bar's high-low range
- **Calculation**: `100 * (close - low) / (high - low)`
- **Example**: `77.78` (close near top of bar)
- **Trading Context**:
  - >75%: Strong bullish bar (Golden Rule validation)
  - <25%: Strong bearish bar
  - ~50%: Indecision bar
- **Usage**: Validates "strong close" for overnight holds

### 8. bar_range_percentile
- **Type**: Float (0-100)
- **Description**: Current bar range vs 20-bar distribution
- **Calculation**: Percentile rank of `(high - low)` in last 20 bars
- **Example**: `45.0` (median volatility)
- **Trading Context**:
  - <20%: Narrow range (NR7/compression setup building)
  - >80%: Wide range (breakout bar or volatility spike)
- **Usage**: Volatility context for entry timing

### 9. volume_ratio_percent
- **Type**: Float (percentage)
- **Description**: Current volume vs 20-bar average
- **Calculation**: `100 * (current_volume / avg_20_volume)`
- **Example**: `125.5` (25.5% above average)
- **Trading Context**:
  - >150%: Volume spike (breakout conviction)
  - <70%: Low conviction (avoid breakouts)
- **Usage**: Validates pattern strength (Raschke: "Volume confirms")

### 10. ema_distance_percent
- **Type**: Float (percentage)
- **Description**: Distance from 21-period EMA (60-min timeframe)
- **Calculation**: `100 * (close - ema21) / ema21`
- **Example**: `0.0463` (0.046% above EMA)
- **Trading Context**:
  - ±0.3%: "Kissing the EMA" (Holy Grail entry zone)
  - >1.5%: Extended from EMA (avoid chasing)
  - <-1.5%: Deep pullback (Slingshot setup)
- **Source**: Cross-chart reference to TripleScreen2 (60-min EMA21)

### 11. oscillator_310_divergence
- **Type**: Integer (0, 1, 2)
- **Description**: 3-10 Oscillator divergence with price swings
- **States**:
  - `0` = NONE: No divergence detected
  - `1` = BULLISH: Price lower low, oscillator higher low
  - `2` = BEARISH: Price higher high, oscillator lower high
- **Example**: `0`
- **Trading Context**: Classic divergence (reversals with momentum confirmation)
- **Detection**: Requires 2 swing highs/lows in 25-bar window (currently placeholder)

---

## Phase 2: Triple Screen Indicators

All indicator enum values from IndicatorManager (`GetPayload()`). These are identical to GUI indicators but exported in flat structure.

### Screen 1 (240-minute timeframe) - Weekly Tide

#### 14. long_macd
- **Type**: Integer (0-12)
- **Enum**: `MacdEnum`
- **Description**: MACD histogram state on 240-min chart
- **States**: NEG_TICK_UP (0), SPRING (1), POS_TICK_DOWN (2), FALL (3), SUMMER (4), WINTER (5), ZERO_FROM_BELOW (6), ZERO_FROM_ABOVE (7), BULLISH_CROSS (8), BEARISH_CROSS (9), POSITIVE_FLAT (10), NEGATIVE_FLAT (11), AT_ZERO (12)
- **Trading Context**: "Trade with the tide" (Elder)
- **Reference**: See ENUM_REFERENCE.md for full details

#### 15. long_FI13_signal
- **Type**: Integer (-2, -1, 0, 1, 2)
- **Enum**: `FI13Enum`
- **Description**: Force Index (13-period) bull/bear signal
- **States**: STRONG_BEAR (-2), WEAK_BEAR (-1), UNCLEAR (0), WEAK_BULL (1), STRONG_BULL (2)

#### 16. long_macd_divergence
- **Type**: Integer (0-7)
- **Enum**: `MACDDivergenceEnum`
- **Description**: 8-state MACD-price divergence machine
- **States**: NONE (0), BULLISH_DIVERGENCE (1), BEARISH_DIVERGENCE (2), HIDDEN_BULL_DIV (3), HIDDEN_BEAR_DIV (4), TRIPLE_BULL_DIV (5), TRIPLE_BEAR_DIV (6), UNCLEAR (7)

#### 17. long_imp
- **Type**: Integer (0-5)
- **Enum**: `ImpulseEnum`
- **Description**: Elder Impulse System (EMA + MACD alignment)
- **States**: BLUE (0), GREEN (1), RED (2), LIME (3), MAGENTA (4), GRAY (5)
- **Trading Context**: GREEN = bullish tide, RED = bearish tide

#### 18. long_ema
- **Type**: Integer (0-2)
- **Enum**: `EmaEnum`
- **Description**: EMA trend direction (rising/falling/flat)
- **States**: FALLING (0), FLAT (1), RISING (2)

### Screen 2 (60-minute timeframe) - Strategy Setup

#### 19. interm_stochastic
- **Type**: Integer (0-5)
- **Enum**: `StochasticEnum`
- **Description**: Stochastic %K position and crossovers
- **States**: OVERSOLD (0), BUYING (1), MIDRANGE (2), OVERBOUGHT (3), SELLING (4), UNCLEAR (5)

#### 20. raschke_strategy_setup
- **Type**: Integer (0-21)
- **Enum**: `RaschkeStrategySetup`
- **Description**: Linda Raschke's 21 compression/reversal patterns
- **Key Patterns**:
  - `0` = NONE
  - `13` = HOLY_GRAIL_BUY (ADX >30, pullback to EMA)
  - `14` = HOLY_GRAIL_SELL
  - `17` = BREAD_AND_BUTTER (classic trend trade)
  - `18` = DOUBLE_REPO (reversal)
  - `19` = DOUBLE_REPO_FAILURE (continuation - highest priority)
  - `3` = NR7 (narrowest range in 7 bars)
- **Pattern Detection Order**: CRITICAL - Check DOUBLE_REPO_FAILURE before DOUBLE_REPO (see docs/CRITICAL_FIX_MOMENTUM_PINBALL_ORDERING.md)
- **Reference**: ENUM_REFERENCE.md, PATTERN_DETECTION_ORDER_VALIDATION.md

#### 21. raschke_tactical_trigger
- **Type**: Integer (0-9)
- **Enum**: `RaschkeTacticalTrigger`
- **Description**: 10 tactical entry triggers
- **Key Triggers**:
  - `0` = NONE
  - `1` = MOMENTUM_PINBALL_BUY (RSI cross + Stochastic <20)
  - `2` = MOMENTUM_PINBALL_SELL (RSI cross + Stochastic >80)
  - `3` = ELDER_BREAKOUT_BUY (Keltner breakout + ADX >25)
  - `4` = ELDER_BREAKOUT_SELL

#### 22. rsi
- **Type**: Float (0-100)
- **Description**: Relative Strength Index (14-period)
- **Example**: `55.23`
- **Trading Context**: Oversold <30, Overbought >70

#### 23. interm_FI2_signal
- **Type**: Integer (-2, -1, 0, 1, 2)
- **Enum**: `FI2Enum`
- **Description**: Force Index (2-period) tactical signal

#### 24. ema_proximity
- **Type**: Integer (0-10)
- **Enum**: `EmaProximity`
- **Description**: Price distance from EMA21 (categorical)
- **States**: VERY_FAR_BELOW (0) ... TOUCHING (5) ... VERY_FAR_ABOVE (10)

#### 25. interm_macd_divergence
- **Type**: Integer (0-7)
- **Enum**: `MACDDivergenceEnum`
- **Description**: MACD divergence on 60-min timeframe

#### 26. interm_imp
- **Type**: Integer (0-5)
- **Enum**: `ImpulseEnum`
- **Description**: Impulse System on 60-min timeframe

### Screen 3 (15-minute timeframe) - Entry Execution

#### 27. atr_proximity
- **Type**: Integer (0-4)
- **Enum**: `ATRProximityEnum`
- **Description**: Price proximity to daily high/low (ATR-normalized)
- **States**: AT_LOW (0), NEAR_LOW (1), MIDRANGE (2), NEAR_HIGH (3), AT_HIGH (4)

#### 28. daily_bias
- **Type**: Integer (-2, -1, 0, 1, 2)
- **Enum**: `DailyBiasEnum`
- **Description**: Intraday momentum direction
- **States**: STRONG_DOWN (-2), WEAK_DOWN (-1), NEUTRAL (0), WEAK_UP (1), STRONG_UP (2)

#### 29. kangaroo_tail
- **Type**: Integer (0-3)
- **Enum**: `KangarooTailEnum`
- **Description**: Within-bar rejection pattern (long wick)
- **States**: NONE (0), BULLISH_TAIL (1), BEARISH_TAIL (2), BOTH_TAILS (3)

#### 30. kangaroo_tail_quality
- **Type**: Float (0.0-1.0)
- **Description**: Pattern strength score
- **Example**: `0.75` (high quality)
- **Calculation**: Volume spike × rejection ratio × range percentile

#### 31. turtle_soup
- **Type**: Integer (0-2)
- **Enum**: `TurtleSoupEnum`
- **Description**: False breakout trap reversal (cross-bar pattern)
- **States**: NONE (0), BULLISH_TRAP (1), BEARISH_TRAP (2)

#### 32. momentum_pinball
- **Type**: Integer (-2, -1, 0, 1, 2)
- **Enum**: `MomentumPinballEnum`
- **Description**: RSI cross + Stochastic extreme
- **States**: STRONG_SELL (-2), WEAK_SELL (-1), NONE (0), WEAK_BUY (1), STRONG_BUY (2)

#### 33. momentum_pinball_quality
- **Type**: Float (0.0-1.0)
- **Description**: Signal strength (volume + divergence + regime alignment)
- **Example**: `0.70`

#### 34. elder_breakout
- **Type**: Integer (0-4)
- **Enum**: `ElderBreakoutEnum`
- **Description**: Keltner Channel breakout + ADX validation
- **States**: NONE (0), WEAK_LONG (1), STRONG_LONG (2), WEAK_SHORT (3), STRONG_SHORT (4)

#### 35. elder_breakout_quality
- **Type**: Float (0.0-1.0)
- **Description**: Breakout strength (ADX + volume + channel squeeze)
- **Example**: `0.65`

#### 36. nr7
- **Type**: Integer (0-3)
- **Enum**: `NR7Enum`
- **Description**: Narrow Range 7 compression pattern
- **States**: NONE (0), NR7 (1), NR4 (2), IDNR4 (3)

#### 37. oscillator_310
- **Type**: Float
- **Description**: Raw 3-10 oscillator value (EMA3 - EMA16)
- **Example**: `2.35`
- **Usage**: Momentum tracking, divergence analysis

#### 38. oscillator_310_cross
- **Type**: Integer (0-2)
- **Enum**: `Oscillator310CrossEnum`
- **Description**: Crossover state of 3-10 oscillator
- **States**: NEUTRAL (0), BULLISH_CROSS (1), BEARISH_CROSS (2)
- **Trading Context**: Overnight momentum failure exits (cross against position)

---

## Phase 4: Strategy & Regime Indicators

### General Indicators

#### 39. side
- **Type**: Integer (0, 1, 2)
- **Enum**: `TradeSideEnum`
- **Description**: Position side during training export
- **States**: FLAT (0), LONG (1), SHORT (2)
- **IMPORTANT**: Always `0` (FLAT) during DataCollector export
- **Usage**: Python augmentation creates synthetic side=1 and side=2 examples

#### 40. market_symbol
- **Type**: Integer (0-10)
- **Enum**: `MarketSymbol`
- **Description**: Instrument identifier
- **Common Values**: ES (1), NQ (2), YM (3), RTY (4)

#### 41. time_of_day
- **Type**: Integer (0-12)
- **Enum**: `TimeOfDayEnum`
- **Description**: Trading session window classification (13 states including Globex)
- **Key Windows**:
  - `0` = ASIAN_SESSION (18:00-03:00 ET)
  - `1` = LONDON_WINDOW (03:00-04:00 ET)
  - `4` = PRE_MARKET_HOOK (08:30-09:00 ET)
  - `5` = OPENING_HOUR (09:30-10:30 ET)
  - `6` = SWEET_SPOT (10:30-11:30 ET, 14:00-15:00 ET)
  - `7` = LUNCH_DEAD_ZONE (11:30-13:00 ET)
  - `10` = PM_RUN_ENTRY (15:45-16:00 ET)
- **Trading Context**: Session quality affects strategy selection

#### 42. holding_strategy
- **Type**: Integer (0-5)
- **Enum**: `HoldingStrategyEnum`
- **Description**: Overnight position management decision
- **States**:
  - `0` = INTRADAY: Exit at 16:00 ET (no overnight risk)
  - `1` = SWING_POSITION: Hold overnight (strong trend + profit)
  - `2` = WEEKEND_CLOSE: Exit Friday PM (no weekend risk)
  - `3` = PM_RUN_CONDITIONAL: Late entry (evaluate at close)
  - `4` = SCRATCH_AT_CLOSE: Failed Golden Rule (exit flat)
  - `5` = UNDEFINED: Insufficient data or no position
- **IMPORTANT**: During export, this is always `5` (UNDEFINED) because side=0 (FLAT)
- **Reference**: Golden Rule validation in ENUM_REFERENCE.md

#### 43. overnight_exit
- **Type**: Integer (0-9)
- **Enum**: `OvernightExitTypeEnum`
- **Description**: Taylor Trading exit classification for overnight holds
- **States**: STRONG_CLOSE_QUALIFIED (0), FAILED_GOLDEN_RULE (1), GAP_EXIT (2), FIRST_REACTION_EXIT (3), OBJECTIVE_POINT_EXIT (4), MOMENTUM_FAILURE_EXIT (5), SCRATCH_EXIT (6), HOLD_FOR_TARGET (7), TRAILING_STOP_EXIT (8)

#### 44. market_regime
- **Type**: Integer (-1 to 5)
- **Enum**: `MarketRegimeEnum`
- **Description**: Linda Raschke's market structure classification (240-min)
- **States**:
  - `0` = TRENDING_UP (ADX >30, GREEN impulse)
  - `1` = TRENDING_DOWN (ADX >30, RED impulse)
  - `2` = TRENDING_IMPULSE (ADX 20-30, sharp move)
  - `3` = RANGE_DAY (bounded trading)
  - `4` = CONSOLIDATING_CHOP (ADX <20, low volatility)
  - `5` = EXTREME_DISLOCATION (NH-NL extremes)
  - `-1` = UNDEFINED
- **Trading Context**: Dictates strategy selection (trend-following vs mean-reversion)

#### 45. nh_nl_signal
- **Type**: Integer (-3 to 3)
- **Enum**: `NhNlSignalEnum`
- **Description**: Market breadth signal (Dr. Elder)
- **States**: STRONG_BEAR (-3), BEAR (-2), WEAK_BEAR (-1), UNCLEAR (0), WEAK_BULL (1), BULL (2), STRONG_BULL (3)

### Cross-Market Correlation (Elite Enhancement 13)

#### 46. corr_es_zn
- **Type**: Float (-1.0 to 1.0)
- **Description**: ES-ZN correlation (20-bar rolling, 60-min)
- **Example**: `0.45` (moderate positive correlation)
- **Trading Context**:
  - >0.7: Risk-off (both falling) → reduce size
  - <-0.3 to >0.3: Correlation breakdown → regime shift warning

#### 47. corr_es_dx
- **Type**: Float (-1.0 to 1.0)
- **Description**: ES-DX correlation (20-bar rolling, 60-min)
- **Trading Context**: DX strength affects ES direction (currency headwind)

#### 48. zn_trend
- **Type**: Integer (-1, 0, 1)
- **Enum**: `CrossMarketTrendEnum`
- **Description**: 10-Year Treasury trend direction
- **States**: DOWN (-1), FLAT (0), UP (1)
- **Detection**: 26-period EMA (rising 3+ bars = UP)

#### 49. dx_trend
- **Type**: Integer (-1, 0, 1)
- **Enum**: `CrossMarketTrendEnum`
- **Description**: Dollar Index trend direction
- **States**: DOWN (-1), FLAT (0), UP (1)

---

## Elite v2.0: Statistical Context & Quality Scores

**Added:** January 18, 2026
**Purpose:** HMM gating and label quality weighting for transformer training

Elite v2.0 enhances training data with **23 new fields** organized into 4 categories:

1. **Statistical Context** (9 fields): Market microstructure for HMM state estimation
2. **Quality Scores** (6 fields): Label confidence weighting for loss functions
3. **Event Metadata** (5 fields): Training sample filtering and stratification
4. **Temporal Counters** (3 fields): Pattern stability and regime persistence

### Statistical Context (9 fields)

These fields provide **bounded, stationary features** for Hidden Markov Model gating in the transformer architecture. All metrics are dimensionless or normalized to prevent scale drift.

#### 50. volatility
- **Type**: Float (0.0-1.0, typically 0.0001-0.01)
- **Description**: Rolling 20-period standard deviation of log returns
- **Calculation**: `std(log(close[t] / close[t-1]))` over 20 bars
- **Example**: `0.00082` (82 basis points daily vol)
- **Trading Context**:
  - <0.0005: Low volatility (compression, range setups)
  - >0.0015: High volatility (breakouts, momentum trades)
- **HMM Usage**: Volatility regime detection (vol clustering)
- **Stationarity**: Bounded by natural price dynamics, mean-reverting

#### 51. efficiency
- **Type**: Float (0.0-1.0)
- **Description**: Trend efficiency ratio (net change / total change)
- **Calculation**: `abs(close[t] - close[t-20]) / sum(abs(close[i] - close[i-1]))`
- **Example**: `0.42` (42% efficient, choppy trend)
- **Trading Context**:
  - >0.7: Clean trend (follow-through likely)
  - <0.3: Choppy price action (mean reversion favored)
- **HMM Usage**: Trend vs range regime classification
- **Stationarity**: Ratio bounded [0,1], dimensionless

#### 52. rel_range
- **Type**: Float (0.0-5.0, typically 0.2-2.0)
- **Description**: Relative bar range (high-low) normalized by ATR(10)
- **Calculation**: `(high - low) / ATR(10)`
- **Example**: `0.45` (bar is 45% of average true range)
- **Trading Context**:
  - <0.5: Narrow range bar (NR7 setup building)
  - >1.5: Expansion bar (breakout or trap)
- **HMM Usage**: Volatility expansion/contraction cycles
- **Stationarity**: ATR normalization removes price scale
- **Note**: Uses Keltner ATR from Screen3 for consistency with execution logic

#### 53. velocity
- **Type**: Float (-1.0 to 1.0, typically -0.5 to 0.5)
- **Description**: Momentum acceleration (change in oscillator_310)
- **Calculation**: `(oscillator_310[t] - oscillator_310[t-1]) / max_delta`
- **Example**: `0.18` (momentum accelerating upward)
- **Trading Context**:
  - >0.3: Momentum surge (continuation trades)
  - <-0.3: Momentum exhaustion (reversal setup)
- **HMM Usage**: Momentum regime shifts (impulse vs exhaustion)
- **Stationarity**: Normalized by historical max delta

#### 54. dist_day_high
- **Type**: Float (typically -50 to 0)
- **Description**: Delta encoding of distance from daily high
- **Calculation**: `last - day_high` (always ≤0)
- **Example**: `-1.25` (1.25 points below daily high)
- **Trading Context**:
  - >-0.5: Near highs (breakout potential)
  - <-10: Deep from highs (reversal zone)
- **HMM Usage**: Intraday price positioning
- **Stationarity**: Delta encoding removes absolute price level

#### 55. dist_day_low
- **Type**: Float (typically 0 to 50)
- **Description**: Delta encoding of distance from daily low
- **Calculation**: `last - day_low` (always ≥0)
- **Example**: `5.0` (5.0 points above daily low)
- **Trading Context**:
  - <0.5: Near lows (support test)
  - >10: Strong from lows (uptrend confirmed)

#### 56. dist_four_bar_high
- **Type**: Float (typically -10 to 0)
- **Description**: Delta encoding of distance from 4-bar rolling high
- **Calculation**: `last - max(high[t-3:t])`
- **Example**: `-0.75` (0.75 points below recent high)
- **Trading Context**:
  - ~0: At resistance (breakout or rejection)
  - <-3: Pullback depth (Holy Grail entry zone)

#### 57. dist_four_bar_low
- **Type**: Float (typically 0 to 10)
- **Description**: Delta encoding of distance from 4-bar rolling low
- **Calculation**: `last - min(low[t-3:t])`
- **Example**: `4.75` (4.75 points above recent low)
- **Trading Context**:
  - ~0: At support (reversal or breakdown)
  - >3: Above pullback (continuation confirmed)

#### 58. dist_ema_13
- **Type**: Float (typically -20 to 20)
- **Description**: Delta encoding of distance from 13-period EMA
- **Calculation**: `last - ema_13` (Screen 3, 15-min)
- **Example**: `-2.35` (2.35 points below EMA)
- **Trading Context**:
  - ±0.5: "Kissing the EMA" (entry zone)
  - >5: Extended above (profit target zone)
  - <-5: Deep pullback (Slingshot setup)
- **HMM Usage**: Mean reversion vs trend following regime

### Quality Scores (6 fields)

These fields provide **label confidence weighting** for the loss function. Higher scores indicate higher-quality training examples that should have greater influence on model updates.

#### 59. setup_quality
- **Type**: Float (0.0-1.0)
- **Description**: Pattern setup validity score
- **Calculation**: Composite of:
  - Pattern alignment across timeframes (0.4 weight)
  - Indicator conviction (strong vs weak states, 0.3 weight)
  - Volume confirmation (>150% avg = +0.3)
- **Example**: `0.85` (high-quality setup)
- **Training Usage**: Weight = `setup_quality^2` (quadratic penalty for low quality)
- **Trading Context**:
  - >0.8: Elite setups (full position size)
  - 0.6-0.8: Good setups (standard size)
  - <0.6: Marginal setups (skip or reduce size)

#### 60. trend_alignment
- **Type**: Float (0.0-1.0)
- **Description**: Multi-timeframe trend coherence
- **Calculation**:
  - 1.0: All 3 screens aligned (240-min, 60-min, 15-min)
  - 0.67: Two screens aligned
  - 0.33: Mixed signals
  - 0.0: Conflicting trends
- **Example**: `1.0` (perfect alignment)
- **Training Usage**: Higher weight for aligned trends (reduce whipsaw examples)

#### 61. pattern_conviction
- **Type**: Float (0.0-1.0)
- **Description**: Indicator strength composite
- **Calculation**: Average of:
  - MACD histogram magnitude / ATR
  - Stochastic distance from midline (|50 - stoch|)
  - Force Index magnitude
- **Example**: `0.72` (moderate conviction)
- **Training Usage**: Weight extreme conviction higher (clear signals)

#### 62. volatility_regime_score
- **Type**: Float (0.0-1.0)
- **Description**: Appropriateness of volatility for strategy
- **Calculation**:
  - Trend-following strategies: Penalize low volatility (<0.0005)
  - Mean-reversion strategies: Penalize high volatility (>0.0015)
- **Example**: `0.68`
- **Training Usage**: Filter mismatched strategy-volatility combinations

#### 63. execution_quality
- **Type**: Float (0.0-1.0)
- **Description**: Trade timing and structure
- **Calculation**: Composite of:
  - Entry timing (time_of_day score)
  - Spread conditions (bid-ask tightness)
  - Stop placement (risk/reward ratio)
- **Example**: `0.78`
- **Training Usage**: Prefer examples with clean execution

#### 64. outcome_confidence
- **Type**: Float (0.0-1.0)
- **Description**: Ground truth label confidence
- **Calculation**: Based on:
  - Move magnitude (larger = more confident)
  - Path dependency (straight move vs whipsaw)
  - Timeframe consistency (1h vs 4h vs 24h agreement)
- **Example**: `0.92` (very confident label)
- **Training Usage**: Critical - low confidence labels get near-zero weight
- **Note**: Calculated by Python augmentation after forward returns known

### Event Metadata (4 fields)

These fields support **training sample filtering** and **stratified analysis** but are NOT model inputs.

#### 65. changed_keys
- **Type**: Array[String]
- **Description**: List of indicators that changed this bar
- **Example**: `["raschke_strategy_setup", "momentum_pinball"]`
- **Training Usage**:
  - Feature importance analysis (which indicators drive predictions?)
  - Stratified sampling (over-sample rare pattern changes)
- **Export**: Only in EVENT_DATA_COLLECTOR.md format

#### 66. event_type
- **Type**: String ("event" or "heartbeat")
- **Description**: Elite v2.0 message classification
- **Training Usage**:
  - Filter heartbeats (duplicate data, no indicator changes)
  - Focus training on "event" samples (information-bearing)
- **Export**: Only in EVENT_DATA_COLLECTOR.md format

#### 68. event_id
- **Type**: Integer (sequential)
- **Description**: Unique event sequence number
- **Example**: `1247`
- **Training Usage**: Time-series cross-validation splits (no lookahead)

#### 69. is_pattern_change
- **Type**: Boolean
- **Description**: True if any pattern detector changed (raschke_strategy_setup, momentum_pinball, elder_breakout, kangaroo_tail, turtle_soup, nr7)
- **Training Usage**: Over-sample pattern transitions (key learning moments)

### Temporal Counters (3 fields)

These fields track **pattern stability** and **regime persistence** for filtering flickering signals.

#### 70. regime_tenure
- **Type**: Integer (0-500)
- **Description**: Bars since market_regime last changed
- **Example**: `42` (regime stable for 42 bars = 10.5 hours)
- **Trading Context**:
  - <5: New regime (wait for confirmation)
  - 5-20: Young regime (high confidence)
  - >50: Aging regime (reversal watch)
- **Training Usage**: Filter regime flickers (<3 bars, noise)

#### 71. bars_in_setup
- **Type**: Integer (0-100)
- **Description**: Bars since raschke_strategy_setup entered non-NONE state
- **Example**: `12` (setup persisting 12 bars = 3 hours)
- **Trading Context**:
  - 1-5: Fresh setup (entry window)
  - 5-20: Aging setup (reduced edge)
  - >20: Stale setup (likely false)
- **Training Usage**: Weight fresh setups higher (actionable signals)

#### 72. bars_since_pattern
- **Type**: Integer (0-100)
- **Description**: Bars since any pattern detector triggered
- **Example**: `0` (just triggered), `25` (long ago)
- **Trading Context**: Pattern memory decay (>10 bars = stale)
- **Training Usage**: Filter examples far from pattern triggers

---

## Phase 5: Training-Specific Fields

Fields added specifically for ML training, not used in real-time trading.

### Pattern Context Metadata

#### 50. momentum_pinball_context
- **Type**: JSON Object
- **Description**: Detailed context for Momentum Pinball pattern
- **Fields**:
  - `impulse_changed` (bool): Screen 1 impulse color changed on current bar
  - `fi2_pullback` (bool): FI2 indicates pullback in trend
- **Example**: `{"impulse_changed": false, "fi2_pullback": false}`
- **Usage**: Feature engineering for pattern quality scoring

#### 51. elder_breakout_context
- **Type**: JSON Object
- **Description**: Detailed context for Elder Breakout pattern
- **Fields**:
  - `adx` (float): Current ADX value
  - `channel_squeeze` (bool): Keltner bands narrowing (compression)
  - `volume_spike` (float): Volume ratio vs 20-bar average
  - `consolidation_bars` (int): Bars in consolidation before breakout
- **Example**: `{"adx": 25.0, "channel_squeeze": false, "volume_spike": 1.81, "consolidation_bars": 5}`

#### 52. kangaroo_tail_context
- **Type**: JSON Object
- **Description**: Detailed context for Kangaroo Tail pattern
- **Fields**:
  - `tail_length_atr` (float): Wick length in ATR units
  - `rejection_ratio` (float): Wick size vs body size
  - `volume_ratio` (float): Volume vs average
  - `bar_range_percentile` (float): Range size percentile
- **Example**: `{"tail_length_atr": 0.8, "rejection_ratio": 3.2, "volume_ratio": 1.45, "bar_range_percentile": 65.0}`

#### 53. nr7_context
- **Type**: JSON Object
- **Description**: Detailed context for NR7 pattern
- **Fields**:
  - `avg_range` (float): Average range of last 7 bars
  - `current_range` (float): Current bar range
  - `compression_pct` (float): How much narrower than average (percentage)
  - `is_inside_bar` (bool): Current bar inside previous bar
- **Example**: `{"avg_range": 4.2, "current_range": 2.1, "compression_pct": 50.0, "is_inside_bar": false}`

---

## Data Augmentation Fields

Fields that enable Python augmentation to create synthetic LONG/SHORT training examples.

### 54. holding_strategy_long
- **Type**: Integer (0-5)
- **Enum**: `HoldingStrategyEnum`
- **Description**: **Hypothetical holding strategy IF in LONG position**
- **Calculation**: Uses same Golden Rule logic as real holding_strategy, but assumes side=LONG
- **Example**: `5` (UNDEFINED if insufficient impulse history)
- **Usage**: Python copies this to `holding_strategy` field when creating side=1 examples
- **Source**: `CalculateHypotheticalHoldingStrategies()` in StudyHelperFunctions.cpp

### 55. holding_strategy_short
- **Type**: Integer (0-5)
- **Enum**: `HoldingStrategyEnum`
- **Description**: **Hypothetical holding strategy IF in SHORT position**
- **Calculation**: Same as holding_strategy_long but assumes side=SHORT
- **Example**: `5` (UNDEFINED if insufficient impulse history)
- **Usage**: Python copies this to `holding_strategy` field when creating side=2 examples

### 56. is_trend_following
- **Type**: Boolean
- **Description**: Classifies current setup as trend-following vs reversal
- **Logic**: True if any of:
  - Momentum Pinball BUY/SELL
  - Elder Breakout BUY/SELL
  - Holy Grail BUY/SELL
  - Bread and Butter
  - First Cross
- **Example**: `false`
- **Usage**: Augmentation filtering (balance trend vs reversal examples)
- **Rationale**: Ensures ML model sees both strategy types equally

### 57. trend_strength_long
- **Type**: Float (0.0-100.0)
- **Description**: Bullish trend strength score (volatility-adjusted)
- **Calculation**: `CalculateEliteTrendStrength()` - considers:
  - Impulse color persistence (Screen 1)
  - ADX strength
  - MACD histogram expansion
  - ATR-normalized momentum
- **Example**: `35.8`
- **Usage**: Position sizing, augmentation weighting

### 58. trend_strength_short
- **Type**: Float (0.0-100.0)
- **Description**: Bearish trend strength score (volatility-adjusted)
- **Example**: `62.4`
- **Usage**: Same as trend_strength_long (bearish side)

---

## Field Summary Table

### Quick Reference

| Field Name | Type | Range/Enum | Source | Training-Specific |
|------------|------|------------|--------|-------------------|
| datetime | String | timestamp | OHLCV | No |
| open | Float | price | OHLCV | No |
| high | Float | price | OHLCV | No |
| low | Float | price | OHLCV | No |
| close | Float | price | OHLCV | No |
| volume | Integer | count | OHLCV | No |
| close_percentile | Float | 0-100 | Phase 1 | Yes (Elite) |
| bar_range_percentile | Float | 0-100 | Phase 1 | Yes (Elite) |
| volume_ratio_percent | Float | 0-∞ | Phase 1 | Yes (Elite) |
| ema_distance_percent | Float | percentage | Phase 1 | Yes (Elite) |
| oscillator_310_divergence | Integer | 0-2 | Phase 1 | Yes (placeholder) |
| long_macd | Integer | MacdEnum | Screen 1 | No |
| long_FI13_signal | Integer | FI13Enum | Screen 1 | No |
| long_macd_divergence | Integer | MACDDivergenceEnum | Screen 1 | No |
| long_imp | Integer | ImpulseEnum | Screen 1 | No |
| long_ema | Integer | EmaEnum | Screen 1 | No |
| interm_stochastic | Integer | StochasticEnum | Screen 2 | No |
| raschke_strategy_setup | Integer | RaschkeStrategySetup | Screen 2 | No |
| raschke_tactical_trigger | Integer | RaschkeTacticalTrigger | Screen 2 | No |
| rsi | Float | 0-100 | Screen 2 | No |
| interm_FI2_signal | Integer | FI2Enum | Screen 2 | No |
| ema_proximity | Integer | EmaProximity | Screen 2 | No |
| interm_macd_divergence | Integer | MACDDivergenceEnum | Screen 2 | No |
| interm_imp | Integer | ImpulseEnum | Screen 2 | No |
| atr_proximity | Integer | ATRProximityEnum | Screen 3 | No |
| daily_bias | Integer | DailyBiasEnum | Screen 3 | No |
| kangaroo_tail | Integer | KangarooTailEnum | Screen 3 | No |
| kangaroo_tail_quality | Float | 0.0-1.0 | Screen 3 | Yes (quality) |
| turtle_soup | Integer | TurtleSoupEnum | Screen 3 | No |
| momentum_pinball | Integer | MomentumPinballEnum | Screen 3 | No |
| momentum_pinball_quality | Float | 0.0-1.0 | Screen 3 | Yes (quality) |
| elder_breakout | Integer | ElderBreakoutEnum | Screen 3 | No |
| elder_breakout_quality | Float | 0.0-1.0 | Screen 3 | Yes (quality) |
| nr7 | Integer | NR7Enum | Screen 3 | No |
| oscillator_310 | Float | value | Oscillator | No |
| oscillator_310_cross | Integer | Oscillator310CrossEnum | Oscillator | No |
| side | Integer | TradeSideEnum | General | No (always 0) |
| market_symbol | Integer | MarketSymbol | General | No |
| time_of_day | Integer | TimeOfDayEnum | General | No |
| holding_strategy | Integer | HoldingStrategyEnum | General | No (always 5) |
| overnight_exit | Integer | OvernightExitTypeEnum | General | No |
| market_regime | Integer | MarketRegimeEnum | General | No |
| nh_nl_signal | Integer | NhNlSignalEnum | General | No |
| corr_es_zn | Float | -1.0 to 1.0 | Cross-Market | No |
| corr_es_dx | Float | -1.0 to 1.0 | Cross-Market | No |
| zn_trend | Integer | CrossMarketTrendEnum | Cross-Market | No |
| dx_trend | Integer | CrossMarketTrendEnum | Cross-Market | No |
| momentum_pinball_context | JSON | object | Pattern Context | Yes |
| elder_breakout_context | JSON | object | Pattern Context | Yes |
| kangaroo_tail_context | JSON | object | Pattern Context | Yes |
| nr7_context | JSON | object | Pattern Context | Yes |
| holding_strategy_long | Integer | HoldingStrategyEnum | Augmentation | **YES** |
| holding_strategy_short | Integer | HoldingStrategyEnum | Augmentation | **YES** |
| is_trend_following | Boolean | true/false | Augmentation | **YES** |
| trend_strength_long | Float | 0.0-100.0 | Augmentation | **YES** |
| trend_strength_short | Float | 0.0-100.0 | Augmentation | **YES** |

**Total Fields**: 58+ (depends on IndicatorManager configuration)

---

## Python Augmentation Process

> **📋 CODEBASE BOUNDARY NOTE:**
> This section documents the **TransformerAgent team's Python processing** (their codebase, separate from our C++ code).
> **C++ Exports** (our code): Raw data with `side=0`, `holding_strategy=5`, plus `holding_strategy_long`/`holding_strategy_short`
> **Python Processing** (their code): Conditional augmentation, validation funnel, target label generation
> **Their Implementation**: `src/collect_data.py` in TransformerAgent repository (not present in our C++ codebase)

### Purpose
DataCollectorStudy exports all bars with `side=0` (FLAT) and `holding_strategy=5` (UNDEFINED). Python augmentation creates **conditional** LONG and SHORT examples that respect Elder's Triple Screen censorship rules and Raschke's validation framework.

**Key Principle**: We only create synthetic position states when indicators would PERMIT that position in real trading. This prevents training the model on impossible scenarios (e.g., LONG positions during RED impulse censorship).

### Augmentation Steps

1. **Read Base Data**: Load TransformerData.jsonl (all side=0 examples)

2. **Apply 5-Layer Validation Funnel** (Elder's Triple Screen + Raschke Quality Framework):
   - **Layer 1 (Score)**: Pattern strength meets context-adjusted threshold
   - **Layer 2 (Time)**: Valid entry session (exclude lunch/final hour/pre-market)
   - **Layer 3 (Veto)**: Daily bias and NH-NL breadth allow direction
   - **Layer 4 (Censorship)**: Elder Impulse System permits direction
     * LONG blocked if 240-min impulse is RED ("Don't fight bearish tape")
     * SHORT blocked if 240-min impulse is GREEN ("Don't fight bullish tape")
   - **Layer 5 (Profitability)**: Forward-looking validation confirms trade would be profitable

3. **Generate Position States Conditionally** (NOT blindly 3x per bar):
   - **FLAT (side=0)**: Always generated for every bar
     * Provides STAND_ASIDE baseline
     * Labels: ENTER_LONG, ENTER_SHORT, STAND_ASIDE, TRAP_LONG, TRAP_SHORT
     * **Note**: TransformerAgent may use `position_state="FLAT"` field in their processed data
   - **LONG (side=1)**: Only if ALL 5 validation layers pass for bullish direction
     * Set `side = 1` (TransformerAgent may rename to `position_state="LONG"`)
     * Copy `holding_strategy = holding_strategy_long`
     * Labels: HOLD_LONG, EXIT_LONG (based on exit criteria)
   - **SHORT (side=2)**: Only if ALL 5 validation layers pass for bearish direction
     * Set `side = 2` (TransformerAgent may rename to `position_state="SHORT"`)
     * Copy `holding_strategy = holding_strategy_short`
     * Labels: HOLD_SHORT, EXIT_SHORT (based on exit criteria)

   **Result**: Dataset size varies based on indicator constraints (typically 1.5-2.5x bars, NOT 3x)

   **Example**: 51,817 bars → ~90K training examples:
   - FLAT: 51,817 (always present)
   - LONG: ~20,000 (only when GREEN/BLUE impulse + no vetos + profitable)
   - SHORT: ~18,000 (only when RED/BLUE impulse + no vetos + profitable)

4. **Balance Trend vs Reversal**: Use `is_trend_following` flag to ensure equal representation
   - Trend-following strategies: Holy Grail, Momentum Pinball, Elder Breakout, Bread & Butter
   - Reversal strategies: Double Repo, Turtle Soup, Kangaroo Tail, Two-B Reversal

5. **Add Target Labels**: Forward-looking outcome classification
   - FLAT state: ENTER_LONG, ENTER_SHORT, STAND_ASIDE, TRAP_LONG, TRAP_SHORT
   - LONG state: HOLD_LONG, EXIT_LONG
   - SHORT state: HOLD_SHORT, EXIT_SHORT
   - Calculation window: Next 4-10 bars (1-2.5 hours)
   - Success criteria: Price moves in position direction by ≥1.5× ATR before hitting stop

6. **Feature Engineering**: Python may add derived features
   - Momentum (rate of change)
   - Volatility regimes
   - Pattern frequency counts
   - Session-relative positioning

### Example Transformations

#### Example 1: Bar with Valid LONG and SHORT Opportunities

**Original Export (side=0)**:
```json
{
  "datetime": "2023-07-26 10:15:00",
  "close": 4587.75,
  "long_imp": 2,  // BLUE (neutral - allows both directions)
  "daily_bias": 3,  // NEUTRAL (no veto)
  "side": 0,
  "holding_strategy": 5,
  "holding_strategy_long": 1,  // SWING_POSITION
  "holding_strategy_short": 0,  // INTRADAY
  "momentum_pinball": -2  // STRONG_SHORT signal (passes score threshold)
}
```

**After Augmentation (2 examples - SHORT blocked by profitability check)**:

```json
// Example 1: FLAT (always generated)
{
  "datetime": "2023-07-26 10:15:00",
  "close": 4587.75,
  "side": 0,  // C++ export field
  "position_state": "FLAT",  // TransformerAgent's transformed field (optional)
  "holding_strategy": 5,  // UNDEFINED (not in position)
  "target": "STAND_ASIDE"  // No valid entry at this bar (added by TransformerAgent)
}

// Example 2: LONG (validated - all 5 layers passed)
{
  "datetime": "2023-07-26 10:15:00",
  "close": 4587.75,
  "side": 1,  // C++ export field
  "position_state": "LONG",  // TransformerAgent's transformed field (optional)
  "holding_strategy": 1,  // SWING_POSITION (from holding_strategy_long)
  "target": "HOLD_LONG"  // No exit signal, continue holding (added by TransformerAgent)
}

// SHORT NOT GENERATED: Failed Layer 5 (profitability)
// Even though momentum_pinball=-2 (strong signal) and impulse=BLUE (allows shorts),
// forward-looking validation showed this trade would not reach profit target.
```

#### Example 2: Bar with Impulse Censorship

**Original Export (side=0)**:
```json
{
  "datetime": "2023-07-26 14:30:00",
  "close": 4592.25,
  "long_imp": 1,  // GREEN (bulls in control - censors shorts)
  "daily_bias": 4,  // BULL_ACC (bullish acceptance - vetos shorts)
  "kangaroo_tail": 2,  // STRONG_SHORT reversal pattern
  "side": 0,
  "holding_strategy_long": 0,  // INTRADAY
  "holding_strategy_short": 0   // INTRADAY
}
```

**After Augmentation (1 example only)**:

```json
// Example 1: FLAT (always generated)
{
  "datetime": "2023-07-26 14:30:00",
  "close": 4592.25,
  "side": 0,  // C++ export field
  "position_state": "FLAT",  // TransformerAgent's transformed field (optional)
  "holding_strategy": 5,
  "target": "STAND_ASIDE"  // Added by TransformerAgent
}

// LONG NOT GENERATED: No valid entry signal (kangaroo_tail is SHORT pattern)
// SHORT NOT GENERATED: Failed Layer 4 (impulse censorship)
// Elder's Rule: "Don't fight the tape" - long_imp=GREEN blocks all SHORT entries
// Even though kangaroo_tail=2 (strong reversal), we respect impulse censorship.
```

### Rationale

**Why Conditional Augmentation (Not Blind 3x)?**
- **Respects Trading Rules**: Never generates position states that indicators would prohibit in real trading
- **Prevents Impossible Scenarios**: Model never sees "LONG during RED impulse" or "SHORT during GREEN impulse"
- **Realistic Training**: Dataset reflects actual tradeable opportunities, not artificial symmetry
- **Elder's Wisdom**: "Don't fight the tape" - impulse censorship is fundamental, not optional

**Why Not Always 3x Per Bar?**
- Many bars have one-directional bias (GREEN impulse → LONG only, RED impulse → SHORT only)
- Some bars have no valid entries (lunch dead zone, low score, vetos active)
- Result: ~1.8x average (51,817 bars → ~90K examples), varies by market conditions

**Dataset Composition (Typical)**:
| Position State | Count | Percentage | Notes |
|---------------|-------|------------|-------|
| FLAT | 51,817 | 100% of bars | Always generated |
| LONG | ~20,000 | ~39% of bars | Only when GREEN/BLUE impulse + validations pass |
| SHORT | ~18,000 | ~35% of bars | Only when RED/BLUE impulse + validations pass |
| **Total** | **~90,000** | **~1.74x bars** | Conditional augmentation |

**Why Augment At All?**
- Real trading system only opens positions on specific setups (sparse labels)
- ML model needs to learn decision boundaries for BOTH long and short sides
- Augmentation creates balanced training data (avoid side bias)

**Why Hypothetical Strategies?**
- `holding_strategy` depends on current position side (cannot be calculated without knowing side)
- During export (side=0), we pre-compute what the strategy WOULD be for LONG and SHORT
- Python selects the appropriate hypothetical value based on augmented side

**Critical for ML Training**:
- Without augmentation: Model only learns from actual trades (biased dataset)
- With **conditional** augmentation: Model learns decision boundaries within valid indicator contexts
- With **blind 3x** augmentation: Model would learn to ignore indicator constraints (dangerous!)
- `is_trend_following` ensures balanced strategy representation

**Implementation Reference (TransformerAgent Codebase)**: `src/collect_data.py` (lines 2343-2560)
- 5-layer validation funnel determines which position states to generate
- See `is_bullish_entry_signal` and `is_bearish_entry_signal` logic
- **Note**: This file is in TransformerAgent's Python repository, not in our C++ codebase

---

## Integration Notes

### Cross-Chart References

DataCollectorStudy runs on 15-minute chart (Screen 3) but reads indicators from other timeframes:

- **Screen 1 (240-min)**: `sc.GetStudyArrayFromChartUsingID()` for Impulse colors
- **Screen 2 (60-min)**: `sc.GetStudyArrayFromChartUsingID()` for EMA21

**Alignment**: Uses `sc.GetNearestMatchForDateTimeIndex()` to map 15-min bars to 60-min and 240-min bars.

### IndicatorManager Integration

All standard indicators exported via `IndicatorManager::GetPayload(sc, false)`:
- Updates bar context: `UpdateBarContext(sc)`
- Caches payload: `CachePayload(sc, false)`
- Exports JSON: `GetPayload(sc, false)` returns flat JSON

**Note**: `INTERM_MACD` is excluded from export (see IndicatorManager.cpp line 144).

### File Output

- **Path**: `C:\SierraChart2\Data\TransformerData.jsonl`
- **Format**: JSON Lines (one object per line, no array wrapper)
- **Size**: ~1MB per 1000 bars (depends on context metadata)
- **Flush**: Automatic flush after each line (prevents data loss)

### Progress Logging

- Every 1000 bars: Log to Sierra Chart and Logger
- First 5 bars: Diagnostic output (MACD, Raschke strategy values)
- Bar close validation: Only exports if `BHCS_BAR_HAS_CLOSED` confirmed

---

## Error Handling

### Exception Safety

All export logic wrapped in try-catch:
```cpp
try {
    // Export logic
} catch (const std::exception& e) {
    Logger::getInstance().log("DataCollector Exception: " + std::string(e.what()));
}
```

### Replay Interruption

If replay stops mid-export:
- File is flushed and closed
- `DC_FILE_OPENED_FLAG` reset to 0
- `DC_EXPORT_ARMED_FLAG` remains 1 (user can resume)

### Missing Data Handling

- **Insufficient bars** (sc.Index < 2): Set augmentation fields to UNDEFINED (5), zeroes, or false
- **Missing cross-chart data**: Skip calculations, log warning
- **Invalid indicator values**: Use enum UNDEFINED/UNCLEAR states

---

## Appendix: Quick Reference

### Activation Checklist

- [ ] Load 15-minute chart (Screen 3)
- [ ] Apply DataCollectorStudy to chart
- [ ] Arm export via Chart Shortcut Menu
- [ ] Start historical replay (Ctrl+R)
- [ ] Monitor progress (log every 1000 bars)
- [ ] Stop replay when complete
- [ ] Disarm export to prevent accidental overwrites
- [ ] Verify output file: `C:\SierraChart2\Data\TransformerData.jsonl`

### Key File References

**C++ Codebase (MindfulTrader - Data Export):**
- **DataCollectorStudy**: `src/DataCollectorStudy.cpp` (lines 250-690)
- **Hypothetical Strategies**: `src/StudyHelperFunctions.cpp` (`CalculateHypotheticalHoldingStrategies`)
- **Indicator Enums**: `include/Indicator.h`
- **Enum Descriptions**: `docs/ENUM_REFERENCE.md` (11,074 lines)
- **Pattern Detection**: `src/StudyHelperFunctions.cpp` (`DetectRaschkeStrategySetup`)
- **Elite Trend Strength**: `src/StudyHelperFunctions.cpp` (`CalculateEliteTrendStrength`)

**Python Codebase (TransformerAgent - Data Processing):**
- **Augmentation Logic**: `src/collect_data.py` (TransformerAgent repository)
- **5-Layer Validation**: `is_bullish_entry_signal`, `is_bearish_entry_signal` functions
- **Target Label Generation**: Forward-looking outcome classification logic

### Related Documentation

**Elite v2.0 Protocol Suite (NEW):**
- **TRANSFORMER_LIVE_TRADING_PROTOCOL.md**: Real-time indicator streaming (ZMQ port 5555) for live trading decisions
- **BACKTESTING_FRAMEWORK.md**: Canonical backtesting framework/spec/roadmap
- **INDICATOR_MESSAGE_PROTOCOL.md**: ⚠️ DEPRECATED - Replaced by TRANSFORMER_LIVE_TRADING_PROTOCOL.md

**Core Documentation:**
- **GUI_INDICATOR_REFERENCE.md**: Real-time trading indicators (nested JSON structure)
- **ENUM_REFERENCE.md**: Complete enum theory, computation, and trading context
- **ENUM_VALIDATION_GUIDE.md**: Enum validation and Python mapping
- **PATTERN_DETECTION_ORDER_VALIDATION.md**: Critical ordering for Raschke patterns
- **OVERNIGHT_MANAGEMENT_RASCHKE_TAYLOR.md**: Golden Rule and overnight holds
- **HEDGE_FUND_GAP_ANALYSIS.md**: Cross-market correlation methodology
- **EVENT_DATA_COLLECTOR.md**: Event-driven training data export format (Elite v2.0)

---

## Version History

**v2.0 (January 18, 2026) - Elite v2.0 Protocol**
- Added 23 Elite v2.0 fields (statistical context, quality scores, event metadata, temporal counters)
- Documented HMM gating architecture integration
- Added quality score weighting for loss functions
- Clarified 3 use case separation (training, live trading, backtesting)
- Cross-referenced TRANSFORMER_LIVE_TRADING_PROTOCOL.md and BACKTESTING_FRAMEWORK.md

**v1.0 (January 7, 2026)**
- Initial comprehensive documentation
- Covers all 58+ training data fields
- Documents augmentation workflow
- Distinguishes training export from real-time trading

---

**Document Maintained By**: MindfulTrader Development Team
**Last Reviewed**: January 18, 2026
