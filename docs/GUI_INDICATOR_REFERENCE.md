# GUI Indicator Reference

**Purpose**: Complete mapping of C++ indicators transmitted via JSON payload to GUI for display and integration.

**Version**: 1.2  
**Date**: January 5, 2026  
**Maintainer**: MindfulTrader Development Team

**Recent Updates (v1.3):**
- **CRITICAL FIELD NAME RECONCILIATION (January 18, 2026)**:
  - ⚠️ `long_imp` (NOT `long_impulse`) - Verified against raw data
  - ⚠️ `interm_imp` (NOT `interm_impulse`) - Verified against raw data
  - ⚠️ `volume` (raw integer, NOT `volume_raw` enum) - Use `volume_signal` for classification
  - ⚠️ `long_mkt_action`, `interm_mkt_action`, `short_mkt_action` - NOT present in raw data (may be missing from C++ transmission)
  - ⚠️ `daily_bias` - NOT present in raw data as separate field (may be embedded in context)
  - **Source of Truth**: `data/raw/event_data_*.jsonl` files contain actual field names transmitted from C++

**Previous Updates (v1.2):**
- Added `corr_es_zn` indicator (#36) - ES-ZN correlation for macro regime detection (floating-point)
- Added `corr_es_dx` indicator (#37) - ES-DX correlation for currency strength analysis (floating-point)
- Added `zn_trend` indicator (#38) - 10-Year Treasury trend direction (CrossMarketTrendEnum: -1, 0, 1)
- Added `dx_trend` indicator (#39) - Dollar Index trend direction (CrossMarketTrendEnum: -1, 0, 1)
- Added `oscillator_310` indicator (#40) - Raw 3-10 oscillator value for momentum tracking (floating-point)
- Added `oscillator_310_cross` indicator (#41) - 3-10 oscillator cross state (Oscillator310CrossEnum: 0, 1, 2)
- Renumbered indicators 36-43 to 42-50 to accommodate new cross-market indicators
- All changes support Elite Enhancement 13: Cross-Market Correlation System (January 5, 2026)

**Previous Updates (v1.1):**
- Added `overnight_exit` indicator (#33) - Taylor Trading exit classification (10 states)
- Updated `time_of_day` indicator (#31) - Added 13 states including Globex overnight windows
- Updated `holding_strategy` indicator (#32) - Expanded to 6 states with Golden Rule validation (PM_RUN_CONDITIONAL, SCRATCH_AT_CLOSE)
- Renumbered indicators 34-42 to account for new overnight management indicators
- All changes support Linda Raschke/George Taylor overnight position management methodology

---

## Table of Contents
1. [Overview](#overview)
2. [JSON Payload Structure](#json-payload-structure)
3. [Screen 1 Indicators (240-minute timeframe)](#screen-1-indicators-240-minute-timeframe)
4. [Screen 2 Indicators (60-minute timeframe)](#screen-2-indicators-60-minute-timeframe)
5. [Screen 3 Indicators (15-minute timeframe)](#screen-3-indicators-15-minute-timeframe)
6. [General Indicators](#general-indicators)
7. [Price Level Keys](#price-level-keys)
8. [Integration Notes](#integration-notes)
9. [Example Complete JSON Payload](#example-complete-json-payload)
10. [Error Handling](#error-handling)
11. [Appendix: Quick Reference Tables](#appendix-quick-reference-tables)

---

## Overview

This document provides a comprehensive reference for all indicators transmitted from the C++ execution layer (Sierra Chart) to the GUI (Python/JavaScript) via JSON payload. Each indicator includes:

- **JSON Key**: String constant used in JSON payload (from `IndicatorKeys` namespace)
- **Enum Type**: Associated C++ enum class defining possible states
- **Value Range**: Minimum and maximum integer values
- **Description**: Semantic meaning and trading context
- **State Labels**: Human-readable descriptions for each enum value

**Data Flow**: Sierra Chart ACSIL Studies → `IndicatorManager` → JSON Serialization → ZMQ Socket → GUI Consumer

**Update Frequency**: 
- Screen 1 (240-min): 2-3 updates per trading day
- Screen 2 (60-min): 7-8 updates per trading day  
- Screen 3 (15-min): ~26 updates per trading day
- General indicators: Updated with Screen 2 or Screen 3 cadence

---

## JSON Payload Structure

The JSON payload is organized into top-level categories matching the Triple Screen Trading System architecture:

```json
{
  "screen1": { /* 240-min indicators */ },
  "screen2": { /* 60-min indicators */ },
  "screen3": { /* 15-min indicators */ },
  "general": { /* Side, time, regime, NH-NL */ },
  "price_levels": { /* Previous highs/lows, EMAs, channels */ }
}
```

Each indicator is transmitted as an integer enum value. The GUI is responsible for:
1. **Parsing**: Extract integer value from JSON
2. **Validation**: Ensure value is within expected enum range
3. **Display**: Map integer to human-readable label
4. **Styling**: Apply color/icon based on enum state (bullish/bearish/neutral)

---

## Screen 1 Indicators (240-minute timeframe)

Screen 1 establishes the weekly tide (Elder: "Trade with the tide"). These indicators provide strategic direction for all entries.

### 1. long_macd
- **JSON Key**: `"long_macd"`
- **Enum Type**: `MacdEnum`
- **Value Range**: 0–11
- **Description**: MACD histogram state and crossovers on 240-min timeframe
- **States**:
  - `0` = NEG_TICK_UP: Negative histogram ticking up
  - `1` = SPRING: Transitioning from negative to positive (Elder's "spring")
  - `2` = POS_TICK_DOWN: Positive histogram ticking down  
  - `3` = FALL: Transitioning from positive to negative (Elder's "fall")
  - `4` = SUMMER: Strong positive histogram
  - `5` = WINTER: Strong negative histogram
  - `6` = ZERO_FROM_BELOW: Crossing zero from below (was 7)
  - `7` = ZERO_FROM_ABOVE: Crossing zero from above (was 8)
  - `8` = BULLISH_CROSS: Fast EMA crosses above slow EMA (was 9)
  - `9` = BEARISH_CROSS: Fast EMA crosses below slow EMA (was 10)
  - `10` = POSITIVE_FLAT: Histogram positive but flat (consolidating above zero)
  - `11` = NEGATIVE_FLAT: Histogram negative but flat (consolidating below zero)
  - `12` = AT_ZERO: Histogram at zero line (neutral/transitional)
- **Usage**: Primary trend filter. Only take longs when ≥1 (SPRING or better). Only take shorts when ≤7.
- **Note**: FLAT (old value 6) removed December 2025 - replaced with POSITIVE_FLAT/NEGATIVE_FLAT/AT_ZERO for better granularity

### 2. long_FI13_signal
- **JSON Key**: `"long_FI13_signal"`
- **Enum Type**: `FI13Enum`
- **Value Range**: -4 to 4
- **Description**: 13-period Force Index signal (Elder's trend confirmation and divergence detector)
- **States**:
  - `-4` = BEARISH_DUMP: Extreme selling pressure spike
  - `-3` = BEARISH_CROSSOVER: FI crosses below zero (bears take control)
  - `-2` = BEARISH_DIVERGENCE: Price higher high but FI lower high (Elder: sell signal)
  - `-1` = BEARISH_TREND_CONFIRMED: FI negative and falling (strong selling)
  - `0` = UNCLEAR: No strong signal  
  - `1` = BULLISH_TREND_CONFIRMED: FI positive and rising (strong buying)
  - `2` = BULLISH_DIVERGENCE: Price lower low but FI higher low (Elder: buy signal)
  - `3` = BULLISH_CROSSOVER: FI crosses above zero (bulls take control)
  - `4` = BULLISH_PUMP: Extreme buying pressure spike
- **Usage**: Divergences (±2) are most powerful signals. Confirm trend with ±1. Pumps/dumps (±4) indicate capitulation.

### 3. long_macd_divergence
- **JSON Key**: `"long_macd_divergence"`
- **Enum Type**: `MACDDivergenceEnum`  
- **Value Range**: -5 to 5
- **Description**: Dr. Elder's MACD-Histogram divergence state machine (zero-cross requirement enforced)
- **States**:
  - `-5` = BEARISH_DIVERGENCE_SELL_SIGNAL: Downticked from second peak (Elder: sell now)
  - `-4` = BEARISH_DIVERGENCE_PATTERN: Pattern formed, waiting for downtick
  - `-3` = WAITING_SECOND_PEAK: After zero cross down, searching for second peak
  - `-2` = WAITING_ZERO_CROSS_DOWN: First peak found, waiting to "break bull's back"
  - `-1` = SEARCHING_FIRST_PEAK: Looking for first MACD-H high above zero
  - `0` = NONE: No divergence pattern active
  - `1` = SEARCHING_FIRST_TROUGH: Looking for first MACD-H low below zero
  - `2` = WAITING_ZERO_CROSS_UP: First trough found, waiting to "break bear's back"
  - `3` = WAITING_SECOND_TROUGH: After zero cross up, searching for second trough
  - `4` = BULLISH_DIVERGENCE_PATTERN: Pattern formed, waiting for uptick  
  - `5` = BULLISH_DIVERGENCE_BUY_SIGNAL: Upticked from second trough (Elder: buy now)
- **Usage**: Signal states (±5) are actionable. Pattern states (±4) are potential. Zero-cross states (±2) are critical checkpoints.

### 4. long_imp
- **JSON Key**: `"long_imp"` ⚠️ (NOT `"long_impulse"`)
- **Enum Type**: `ImpulseEnum`
- **Value Range**: 0–7
- **Description**: Elder's Impulse System (MACD histogram + EMA slope combination)
- **States**:
  - `0` = GREEN: Both MACD and EMA rising (bulls in full control)
  - `1` = RED: Both MACD and EMA falling (bears in full control)
  - `2` = BLUE: MACD and EMA neutral/conflicting (market indecision)
  - `3` = GREEN_TO_BLUE: Transition from green (weakening bulls)
  - `4` = RED_TO_BLUE: Transition from red (weakening bears)  
  - `5` = BLUE_TO_GREEN: Transition to green (bulls strengthening)
  - `6` = BLUE_TO_RED: Transition to red (bears strengthening)
  - `7` = UNDEFINED: Insufficient data
- **Usage**: GREEN (0) = only longs allowed. RED (1) = only shorts allowed. BLUE (2) = no entries.
- **CRITICAL**: Raw data uses `long_imp` NOT `long_impulse`

### 5. long_ema
- **JSON Key**: `"long_ema"`  
- **Enum Type**: `EmaEnum`
- **Value Range**: 0–3
- **Description**: 130-period EMA slope direction (long-term trend)
- **States**:
  - `0` = UNDEFINED: Insufficient data
  - `1` = FLAT: EMA horizontal (no trend)
  - `2` = INC: EMA rising (uptrend)
  - `3` = DEC: EMA falling (downtrend)
- **Usage**: INC (2) = bullish bias. DEC (3) = bearish bias. FLAT (1) = range-bound.

### 6. long_mkt_action
- **JSON Key**: `"long_mkt_action"` ⚠️ (NOT present in raw data - C++ may not be sending this field)
- **Enum Type**: `PriceActionEnum`  
- **Value Range**: 0–7
- **Description**: Position relative to Keltner Channel (EMA ± 2.5× ATR) on 240-min
- **States**:
  - `0` = FUBK: Follow-up breakout (breakout continuation)
  - `1` = HIT_UPPER_CHANNEL: Price at/near upper Keltner band
  - `2` = ABOVE_VALUE: Price above EMA but inside channel
  - `3` = IN_VALUE_ZONE: Price near EMA (fair value)
  - `4` = BELOW_VALUE: Price below EMA but inside channel  
  - `5` = HIT_LOWER_CHANNEL: Price at/near lower Keltner band
  - `6` = FDBK: Follow-down breakdown (breakdown continuation)
  - `7` = NONE: Undefined state
- **Usage**: HIT_UPPER_CHANNEL (1) = potential mean reversion short. HIT_LOWER_CHANNEL (5) = potential mean reversion long.
- **⚠️ WARNING**: Field NOT found in raw data as of January 2026 - may be missing from C++ transmission

---

## Screen 2 Indicators (60-minute timeframe)

Screen 2 establishes the daily wave (Elder: "Trade with the wave"). These indicators refine entry timing and identify pullbacks within Screen 1 trend.

### 7. interm_stochastic  
- **JSON Key**: `"interm_stochastic"`
- **Enum Type**: `StochasticEnum`
- **Value Range**: 0–5
- **Description**: Stochastic oscillator state (overbought/oversold + divergences)
- **States**:
  - `0` = UNDEFINED: Insufficient data
  - `1` = NORMAL: Stochastic between 20 and 80 (neutral)
  - `2` = OVER_BOUGHT: Stochastic > 80 (overbought, potential reversal)  
  - `3` = OVER_SOLD: Stochastic < 20 (oversold, potential bounce)
  - `4` = BULLISH_DIVERGENCE: Price lower low, Stochastic higher low
  - `5` = BEARISH_DIVERGENCE: Price higher high, Stochastic lower high
- **Usage**: OVER_SOLD (3) = look for long entries in uptrend. OVER_BOUGHT (2) = look for short entries in downtrend.

### 8. raschke_strategy_setup
- **JSON Key**: `"raschke_strategy_setup"`  
- **Enum Type**: `RaschkeStrategySetup`
- **Value Range**: 0–21
- **Description**: Linda Raschke's 19 compression/consolidation/reversal patterns
- **States**:
  - `0` = NONE: No setup pattern detected
  - `1` = THREE_BAR_TRIANGLE: Converging highs/lows (consolidation)
  - `2` = NR4: Narrowest range in 4 bars (minor compression)
  - `3` = NR7: Narrowest range in 7 bars (major compression) 
  - `4` = IDNR4: Inside bar + NR4 (extreme compression)
  - `7` = WHIPLASH: False breakout then reversal (mean reversion)
  - `8` = GHOST: Volume spike without follow-through (exhaustion)
  - `9` = TWO_B_REVERSAL: Second test of extreme fails (reversal)
  - `10` = ANTI: Opposite of expected breakout direction (counter-trend)
  - `12` = HOLY_GRAIL_CONTINUATION: Strong trend without pullback
  - `13` = HOLY_GRAIL_BUY: EMA pullback in uptrend (Linda's favorite)
  - `14` = HOLY_GRAIL_SELL: EMA pullback in downtrend  
  - `15` = SLINGSHOT: Deep EMA pullback then snap-back
  - `16` = FIRST_CROSS: Initial EMA cross after consolidation
  - `17` = BREAD_AND_BUTTER: Range breakout + volume confirmation
  - `18` = DOUBLE_REPO: Two failed breakouts same side (coiled spring)
  - `19` = DOUBLE_REPO_FAILURE: Double repo pattern fails (reversal)
  - `20` = FLIP: Sudden trend reversal (momentum shift)
  - `21` = NR4_NR7_VOLUME_SPIKE: Compression + volume spike (breakout imminent)
- **Usage**: These are strategic setups. Wait for tactical trigger (next indicator) before entry.

### 9. raschke_tactical_trigger  
- **JSON Key**: `"raschke_tactical_trigger"`
- **Enum Type**: `RaschkeTacticalTrigger`
- **Value Range**: 0–10
- **Description**: Entry execution signals for Raschke's 5 hybrid pattern triggers
- **States**:
  - `0` = NONE: No tactical entry signal
  - `1` = KANGAROO_TAIL_BUY: Long lower tail + close near high (buyer rejection at support)
  - `2` = KANGAROO_TAIL_SELL: Long upper tail + close near low (seller rejection at resistance)
  - `3` = TURTLE_SOUP_BUY: Price breaks below 4-day low, closes back inside (false breakdown)  
  - `4` = TURTLE_SOUP_SELL: Price breaks above 4-day high, closes back inside (false breakout)
  - `5` = MOMENTUM_PINBALL_BUY: RSI3 > RSI10 + Stochastic oversold (early reversal)
  - `6` = MOMENTUM_PINBALL_SELL: RSI3 < RSI10 + Stochastic overbought (early reversal)
  - `7` = ELDER_BREAKOUT_BUY: Close above upper Keltner band (volatility expansion)
  - `8` = ELDER_BREAKOUT_SELL: Close below lower Keltner band (volatility expansion)
  - `9` = NR7_BREAKOUT_BUY: Breakout from NR7 compression (upside)
  - `10` = NR7_BREAKOUT_SELL: Breakout from NR7 compression (downside)
- **Usage**: These are ACTIONABLE entry signals. Confirm with Screen 1 trend + Screen 2 FI2 pullback.

### 10. rsi  
- **JSON Key**: `"rsi"`
- **Enum Type**: `RSI`
- **Value Range**: 0–5
- **Description**: RSI (14-period) state classification
- **States**:
  - `0` = UNDEFINED: Insufficient data
  - `1` = NORMAL: RSI between 30 and 70 (neutral zone)
  - `2` = OVERBOUGHT: RSI > 70 (potential reversal zone)
  - `3` = OVERSOLD: RSI < 30 (potential bounce zone)  
  - `4` = BULLISH_DIVERGENCE: Price lower low, RSI higher low
  - `5` = BEARISH_DIVERGENCE: Price higher high, RSI lower high
- **Usage**: OVERSOLD (3) in uptrend = buy pullback. OVERBOUGHT (2) in downtrend = sell rally.

### 11. interm_FI2_signal
- **JSON Key**: `"interm_FI2_signal"`
- **Enum Type**: `FI2Enum`  
- **Value Range**: -2 to 2
- **Description**: 2-period Force Index (Elder's short-term pullback detector)
- **States**:
  - `-2` = SIGNAL_DOWN: FI2 confirms downtrend acceleration
  - `-1` = RALLY_FOR_SHORT: Pullback rally in downtrend (short entry opportunity)
  - `0` = NEUTRAL_OR_TREND_ALIGNED: FI2 aligned with trend
  - `1` = PULLBACK_FOR_LONG: Pullback dip in uptrend (long entry opportunity)
  - `2` = SIGNAL_UP: FI2 confirms uptrend acceleration
- **Usage**: Elder's key timing signal. Enter longs on PULLBACK_FOR_LONG (1). Enter shorts on RALLY_FOR_SHORT (-1).

### 12. ema_proximity  
- **JSON Key**: `"ema_proximity"`
- **Enum Type**: `EmaProximity`
- **Value Range**: -1 to 8
- **Description**: Price relationship to 60-period EMA (pullback depth classification)
- **States**:
  - `-1` = NONE: Undefined
  - `0` = ABOVE_STRONG: Price well above EMA (>1.5× ATR)
  - `1` = ABOVE_TOUCH: Price approaching EMA from above
  - `2` = CROSS_ABOVE: Price crossing above EMA (bullish)  
  - `3` = AT_EMA: Price touching EMA (ideal entry zone)
  - `4` = CROSS_BELOW: Price crossing below EMA (bearish)
  - `5` = BELOW_TOUCH: Price approaching EMA from below
  - `6` = BELOW_STRONG: Price well below EMA (>1.5× ATR)
  - `7` = PRICE_ABOVE_EMA: Generic above state
  - `8` = PRICE_BELOW_EMA: Generic below state
- **Usage**: Linda's "Holy Grail" entries occur at AT_EMA (3) or CROSS_ABOVE/BELOW (2/4) in trending markets.

### 13. price_metrics
- **JSON Key**: `"price_metrics"`  
- **Enum Type**: `PriceMetrics`
- **Value Range**: 0–2
- **Description**: Aggregated price strength assessment (combines momentum + trend)
- **States**:
  - `0` = NORMAL: Balanced price action
  - `1` = STRONG_BULLISH: Multiple bullish indicators aligned (high conviction)
  - `2` = STRONG_BEARISH: Multiple bearish indicators aligned (high conviction)
- **Usage**: STRONG_BULLISH (1) = raise position size. STRONG_BEARISH (2) = reduce longs or add shorts.

### 14. interm_macd_divergence
- **JSON Key**: `"interm_macd_divergence"`  
- **Enum Type**: `MACDDivergenceEnum`
- **Value Range**: -5 to 5
- **Description**: Elder's MACD-Histogram divergence on 60-min timeframe (same states as Screen 1)
- **States**: (See `long_macd_divergence` states above)
- **Usage**: Intraday divergences on 60-min provide early warning of Screen 1 trend exhaustion.

### 15. interm_imp
- **JSON Key**: `"interm_imp"` ⚠️ (NOT `"interm_impulse"`)
- **Enum Type**: `ImpulseEnum`  
- **Value Range**: 0–7
- **Description**: Elder's Impulse System on 60-min timeframe (same states as Screen 1)
- **States**: (See `long_imp` states above)
- **Usage**: GREEN (0) = only longs. RED (1) = only shorts. BLUE (2) = no new entries.
- **CRITICAL**: Raw data uses `interm_imp` NOT `interm_impulse`

### 16. interm_macd
- **JSON Key**: `"interm_macd"`
- **Enum Type**: `MacdEnum`  
- **Value Range**: 0–11
- **Description**: MACD histogram state on 60-min timeframe (same states as Screen 1)
- **States**: (See `long_macd` states above)
- **Usage**: Faster reaction than Screen 1. Use for intraday momentum shifts.

### 17. interm_mkt_action
- **JSON Key**: `"interm_mkt_action"` ⚠️ (NOT present in raw data - C++ may not be sending this field)
- **Enum Type**: `PriceActionEnum`  
- **Value Range**: 0–7
- **Description**: Keltner Channel position on 60-min timeframe (same states as Screen 1)
- **States**: (See `long_mkt_action` states above)
- **Usage**: HIT_UPPER_CHANNEL (1) = overbought on 60-min. HIT_LOWER_CHANNEL (5) = oversold on 60-min.
- **⚠️ WARNING**: Field NOT found in raw data as of January 2026 - may be missing from C++ transmission

---

## Screen 3 Indicators (15-minute timeframe)

Screen 3 provides precise entry execution (Elder: "Pick the right moment"). These indicators confirm entry quality and optimize stop placement.

### 18. structure_test  
- **JSON Key**: `"structure_test"`
- **Enum Type**: `StructureTest`
- **Value Range**: 0–8
- **Description**: Previous bar high/low tests (Elder's "second chance" entries)
- **States**:
  - `0` = NONE: No structure test occurring
  - `1` = FAILED_LOW_CLOSE_INSIDE: Price breaks previous low, closes back inside (bull trap cleared)
  - `2` = FAILED_LOW_STRONG_REVERSAL: Price breaks low, closes much higher (Turtle Soup potential)
  - `3` = FAILED_HIGH_CLOSE_INSIDE: Price breaks previous high, closes back inside (bear trap cleared)  
  - `4` = FAILED_HIGH_STRONG_REVERSAL: Price breaks high, closes much lower (Turtle Soup potential)
  - `5` = DECISIVE_BREAKOUT_HIGH: Price closes decisively above previous high (continuation)
  - `6` = DECISIVE_BREAKDOWN_LOW: Price closes decisively below previous low (continuation)
  - `7` = INSIDE_BAR: Current bar entirely within previous bar range (consolidation)
  - `8` = OUTSIDE_BAR: Current bar engulfs previous bar (volatility expansion)
- **Usage**: Failed tests (1-4) are reversal signals. Decisive breaks (5-6) are continuation signals.

### 19. volume
- **JSON Key**: `"volume"` ⚠️ (Raw integer value, NOT enum - use `volume_signal` for classification)
- **Type**: `int` (raw volume value)
- **Description**: Raw volume for the bar (contract count)
- **Usage**: Use `volume_signal` (indicator #20) for classification states. This field is the raw integer volume.
- **CRITICAL**: Raw data uses `volume` (integer) NOT `volume_raw` (enum). For volume classification, see `volume_signal`.

### 20. volume_signal
- **JSON Key**: `"volume_signal"`
- **Enum Type**: `VolumeEnum`  
- **Value Range**: 0–6
- **Description**: Volume classification relative to moving average (enum with directional bias)
- **States**:
  - `0` = NORMAL: Volume near 20-bar average
  - `1` = HIGH: Volume 1.5-2× average
  - `2` = VERY_HIGH: Volume >2× average (potential exhaustion or breakout)
  - `3` = LOW: Volume 0.5-0.7× average (low conviction)
  - `4` = VERY_LOW: Volume <0.5× average (compression, potential spring)  
  - `5` = HIGH_BUY_VOLUME: High volume + price closing near high (accumulation)
  - `6` = HIGH_SELL_VOLUME: High volume + price closing near low (distribution)
- **Usage**: VERY_HIGH (2) at extreme = exhaustion. VERY_LOW (4) at consolidation = compression before breakout. Incorporates directional bias (buy pressure vs sell pressure).
- **Value Range**: 0–4
- **Description**: Current bar range relative to 14-period ATR
- **States**:
  - `0` = LOW_VOLATILITY: Bar range <0.7× ATR (low volatility, potential coil)
  - `1` = HIGH_MOVE: Bar range 1.3-1.8× ATR (above-average volatility)
  - `2` = EXTREME_VOLATILITY: Bar range >1.8× ATR (panic/capitulation)
  - `3` = EXTREME_LOW: Bar range <0.3× ATR (dead market, extreme compression)
  - `4` = EXTREME_HIGH: Bar range >2.5× ATR (gap or explosive move)  
- **Usage**: EXTREME_LOW (3) = compression before breakout. EXTREME_HIGH (4) = exhaustion or gap.

### 22. daily_bias
- **JSON Key**: `"daily_bias"`
- **Enum Type**: `DailyBiasEnum`
- **Value Range**: -2 to 2
- **Description**: Daily price action bias from overnight/opening range (Market Profile context)  
- **States**:
  - `-2` = BEARISH_REJECTION_TEST: Price tested above value area, rejected down (sell signal)
  - `-1` = BEARISH_ACCEPTANCE: Price accepted below value area (bearish day)
  - `0` = VALUE_AREA_NEUTRAL: Price oscillating within value area (range day)
  - `1` = BULLISH_ACCEPTANCE: Price accepted above value area (bullish day)
  - `2` = BULLISH_REJECTION_TEST: Price tested below value area, rejected up (buy signal)
- **Usage**: Rejection tests (±2) are strongest signals. Acceptance states (±1) define intraday bias.
- **Transmission**: Transmitted via `IndicatorManager::SyncFeatureVector()` (feature vector index 25) and mutated directly to FlatBuffer events (`IndicatorManager.cpp:902-903`)

### 23. short_mkt_action
- **JSON Key**: `"short_mkt_action"` ⚠️ (NOT present in raw data - C++ may not be sending this field)
- **Enum Type**: `PriceActionEnum`
- **Value Range**: 0–7
- **Description**: Keltner Channel position on 15-min timeframe (same states as Screen 1/2)
- **States**: (See `long_mkt_action` states above)
- **Usage**: Fastest reaction timeframe. HIT_UPPER_CHANNEL (1) = immediate overbought. HIT_LOWER_CHANNEL (5) = immediate oversold.
- **⚠️ WARNING**: Field NOT found in raw data as of January 2026 - may be missing from C++ transmission

### 24. kangaroo_tail
- **JSON Key**: `"kangaroo_tail"`  
- **Enum Type**: `KangarooTailEnum`
- **Value Range**: -3 to 3
- **Description**: Elder's long-tail reversal pattern (price rejection at extremes)
- **States**:
  - `-3` = BEARISH_EXTREME: Tail >4× body + >1× ATR (extreme seller rejection)
  - `-2` = BEARISH_STRONG: Tail 2.5-4× body (strong seller rejection)
  - `-1` = BEARISH_WEAK: Tail 2.0-2.5× body (moderate seller rejection)
  - `0` = NONE: No kangaroo tail pattern
  - `1` = BULLISH_WEAK: Tail 2.0-2.5× body (moderate buyer rejection)  
  - `2` = BULLISH_STRONG: Tail 2.5-4× body (strong buyer rejection)
  - `3` = BULLISH_EXTREME: Tail >4× body + >1× ATR (extreme buyer rejection)
- **Usage**: STRONG/EXTREME states (±2, ±3) at support/resistance are high-probability reversal signals.

### 25. turtle_soup
- **JSON Key**: `"turtle_soup"`
- **Enum Type**: `TurtleSoupEnum`  
- **Value Range**: -3 to 3
- **Description**: Linda Raschke's false breakout reversal (stop hunt pattern)
- **States**:
  - `-3` = BEARISH_EXTREME: Penetration >0.5× ATR, close ≤20% of range (professional trap)
  - `-2` = BEARISH_STRONG: Penetration 0.3-0.5× ATR, close near low (strong reversal)
  - `-1` = BEARISH_WEAK: Penetration 0.1-0.3× ATR (moderate stop hunt)
  - `0` = NONE: No Turtle Soup pattern
  - `1` = BULLISH_WEAK: Penetration 0.1-0.3× ATR (moderate stop hunt)  
  - `2` = BULLISH_STRONG: Penetration 0.3-0.5× ATR, close near high (strong reversal)
  - `3` = BULLISH_EXTREME: Penetration >0.5× ATR, close ≥80% of range (professional trap)
- **Usage**: STRONG/EXTREME states (±2, ±3) at 4-day high/low = classic "stop-hunt" entry.

### 26. momentum_pinball
- **JSON Key**: `"momentum_pinball"`
- **Enum Type**: `MomentumPinballEnum`  
- **Value Range**: -3 to 3
- **Description**: Linda Raschke's RSI cross + Stochastic extreme (early momentum reversal)
- **States**:
  - `-3` = BEARISH_EXTREME: Fresh Impulse red + stoch >90 + volume spike (powerful sell)
  - `-2` = BEARISH_STRONG: RSI delta ≤-5, stoch 85-90, FI2 rally (solid sell setup)
  - `-1` = BEARISH_WEAK: Fresh cross, stoch 80-85 (marginal setup)
  - `0` = NONE: No Momentum Pinball pattern
  - `1` = BULLISH_WEAK: Fresh cross, stoch 15-20 (marginal setup)  
  - `2` = BULLISH_STRONG: RSI delta ≥5, stoch 10-15, FI2 pullback (solid buy setup)
  - `3` = BULLISH_EXTREME: Fresh Impulse green + stoch <10 + volume spike (powerful buy)
- **Usage**: STRONG/EXTREME states (±2, ±3) in trending markets = "catch the bounce" entry.

### 27. elder_breakout
- **JSON Key**: `"elder_breakout"`
- **Enum Type**: `ElderBreakoutEnum`  
- **Value Range**: -3 to 3
- **Description**: Elder/Raschke Keltner Channel breakout (volatility expansion)
- **States**:
  - `-3` = BEARISH_EXTREME: Breakout >1× ATR or gap + surge volume + consolidation (major move)
  - `-2` = BEARISH_STRONG: Breakout >0.5× ATR + volume + Hurst >0.55 (solid move)
  - `-1` = BEARISH_WEAK: Barely beyond band 0.1-0.5× ATR (marginal breakout)
  - `0` = NONE: No Elder Breakout pattern
  - `1` = BULLISH_WEAK: Barely beyond band 0.1-0.5× ATR (marginal breakout)  
  - `2` = BULLISH_STRONG: Breakout >0.5× ATR + volume + Hurst >0.55 (solid move)
  - `3` = BULLISH_EXTREME: Breakout >1× ATR or gap + surge volume + consolidation (major move)
- **Usage**: STRONG/EXTREME states (±2, ±3) after consolidation = "channel squeeze then expansion" entry.

### 28. nr7
- **JSON Key**: `"nr7"`
- **Enum Type**: `NR7Enum`  
- **Value Range**: 0–3
- **Description**: Linda Raschke's Narrow Range 7 (compression pattern, coiled spring before breakout)
- **States**:
  - `0` = NONE: Not narrowest range in 7 bars
  - `1` = WEAK: Range 95-100% of 7-bar average (barely narrowest)
  - `2` = STRONG: Range 85-95% + volume declining (solid compression)
  - `3` = EXTREME: Range <80% + volume dry + consolidation (nuclear coiled spring)
- **Usage**: EXTREME (3) = highest probability of explosive breakout. Wait for breakout confirmation before entry.

---

## General Indicators

General indicators provide context independent of timeframe (position, symbol, time-of-day, regime).

### 29. side  
- **JSON Key**: `"side"`
- **Enum Type**: `TradeSideEnum`
- **Value Range**: 0–2
- **Description**: Current recommended trade direction based on Triple Screen alignment
- **States**:
  - `0` = FLAT: No position, no entry signal (market neutral or conflicting signals)
  - `1` = LONG: Long position or long entry signal (bullish alignment)
  - `2` = SHORT: Short position or short entry signal (bearish alignment)
- **Usage**: Transmitted as trading directive. GUI should display prominently with color coding.

### 30. market_symbol
- **JSON Key**: `"market_symbol"`  
- **Enum Type**: `MarketSymbol`
- **Value Range**: 1–5
- **Description**: Which futures contract is being traded
- **States**:
  - `1` = MES: Micro E-mini S&P 500 Futures
  - `2` = MGC: Micro Gold Futures
  - `3` = MCL: Micro Crude Oil Futures
  - `4` = MNQ: Micro Nasdaq-100 Futures
  - `5` = M2K: Micro Russell 2000 Futures  
- **Usage**: Display symbol name in GUI header. Use for symbol-specific configuration (tick size, margin, etc.).

### 31. time_of_day
- **JSON Key**: `"time_of_day"`
- **Enum Type**: `TimeOfDayEnum`
- **Value Range**: 0–7
- **Description**: Session quality classification (Linda Raschke's time-of-day framework)  
- **States**:
  - `0` = PRE_MARKET: Before 9:30 AM (low liquidity, avoid entries)
  - `1` = OPENING_HOUR: 9:30-10:30 (high volatility, trend establishment)
  - `2` = SWEET_SPOT: 10:30-12:00 (cleanest trends, best entries) ⭐
  - `3` = LUNCH_DEAD_ZONE: 12:00-14:00 (avoid entries, choppy action)
### 31. time_of_day
- **JSON Key**: `"time_of_day"`
- **Enum Type**: `TimeOfDayEnum`
- **Value Range**: 0–12
- **Description**: Session quality classification (Linda Raschke framework) with overnight/Globex windows  
- **States**:
  - `0` = ASIAN_SESSION: 18:00-03:00 ET - Globex overnight trading (thin liquidity)
  - `1` = LONDON_WINDOW: 03:00-04:00 ET - European open influence
  - `2` = LONDON_TO_PREMARKET: 04:00-08:30 ET - Pre-US session positioning
  - `3` = PRE_MARKET_HOOK: 08:30-09:00 ET - Economic data reaction window
  - `4` = PRE_MARKET: 09:00-09:30 ET - Pre-market positioning
  - `5` = OPENING_HOUR: 09:30-10:30 ET - High volatility, trend establishment
  - `6` = SWEET_SPOT: 10:30-12:00 ET - Cleanest trends, best entries
  - `7` = LUNCH_DEAD_ZONE: 12:00-14:00 ET - Avoid entries, choppy action
  - `8` = AFTERNOON_SESSION: 14:00-15:00 ET - Second chance setups
  - `9` = FINAL_HOUR: 15:00-15:45 ET - Avoid new entries unless strong setup
  - `10` = PM_RUN_ENTRY: 15:45-16:00 ET - Late-day entry, must profit immediately
  - `11` = AFTER_HOURS: 16:00-18:00 ET - Low liquidity, position squaring
  - `12` = OVERNIGHT_HOLD: Position held overnight (set when carrying position through close)
- **Usage**: Only take new entries during OPENING_HOUR (5), SWEET_SPOT (6), or AFTERNOON_SESSION (8). Avoid LUNCH_DEAD_ZONE (7). PM_RUN_ENTRY (10) acceptable ONLY if position profits by 16:00. Overnight exit evaluation occurs during ASIAN_SESSION (0), LONDON_WINDOW (1), or OPENING_HOUR (5).

### 32. holding_strategy
- **JSON Key**: `"holding_strategy"`
- **Enum Type**: `HoldingStrategyEnum`
- **Value Range**: 0–5
- **Description**: Whether position should be closed intraday or held overnight (Golden Rule validation)
- **States**:
  - `0` = INTRADAY: Close position by market close (weak trend, low conviction)
  - `1` = SWING_POSITION: Hold overnight with trailing stop (passed Golden Rule validation)
  - `2` = WEEKEND_CLOSE: Friday—must close by market close (calendar risk)
  - `3` = PM_RUN_CONDITIONAL: Late entry (15:45-16:00), evaluate at close (must profit by 16:00)
  - `4` = SCRATCH_AT_CLOSE: Failed Golden Rule → exit flat by 15:55, don't hold
  - `5` = UNDEFINED: Not yet determined (early in position)
- **Usage**: SWING_POSITION (1) = qualified to hold overnight (in profit, strong close in top/bottom 25% of range, stop at breakeven, strong Screen 1 trend). SCRATCH_AT_CLOSE (4) = exit immediately, position failed overnight criteria. PM_RUN_CONDITIONAL (3) = re-evaluate at 16:00, if not profitable → scratch. **Golden Rule**: Position MUST be profitable AND close in top 25% (long) or bottom 25% (short) of daily range to hold overnight.

### 33. overnight_exit
- **JSON Key**: `"overnight_exit"`  
- **Enum Type**: `OvernightExitTypeEnum`
- **Value Range**: 0–9
- **Description**: Overnight exit classification (Raschke/Taylor Trading Technique) - HOW to exit overnight positions
- **States**:
  - `0` = NO_OVERNIGHT_POSITION: Flat, no overnight position
  - `1` = STRONG_CLOSE_QUALIFIED: Position in profit, closed top/bottom 25% of range (passed Golden Rule)
  - `2` = FAILED_GOLDEN_RULE: Not in profit OR weak close → MUST scratch next morning
  - `3` = GAP_EXIT: Gap in favor (≥0.5R windfall) → exit 09:30-09:45 ET
  - `4` = FIRST_REACTION_EXIT: Flat/adverse open → exit on first bounce (09:30-10:00 ET)
  - `5` = OBJECTIVE_POINT_EXIT: Taylor target hit (prev day H/L) → exit at liquidity window (10:30-11:00 ET)
  - `6` = MOMENTUM_FAILURE_EXIT: 3-10 Oscillator crossed during Globex → exit pre-market
  - `7` = SCRATCH_EXIT: Immediate exit at 09:30 open (didn't meet Golden Rule)
  - `8` = HOLD_FOR_TARGET: Strong position, target not yet hit, continue holding
  - `9` = TRAILING_STOP_EXIT: Stop moved to profit, let it run or get stopped
- **Usage**: Computed during Globex session (18:00-09:30) or first 90 minutes of RTH (09:30-11:00). GAP_EXIT (3) = take windfall profit in first 15 minutes. FIRST_REACTION_EXIT (4) = wait for bounce/rejection. OBJECTIVE_POINT_EXIT (5) = exit at Sweet Spot when approaching prev day H/L. MOMENTUM_FAILURE_EXIT (6) = 3-10 oscillator crossed zero during overnight, exit pre-market. SCRATCH_EXIT (7) = position failed Golden Rule, exit immediately.

### 34. market_regime
- **JSON Key**: `"market_regime"`  
- **Enum Type**: `MarketRegimeEnum`
- **Value Range**: -1 to 4
- **Description**: Market structure classification (Linda Raschke's regime framework)
- **States**:
  - `-1` = UNDEFINED: Insufficient data
  - `0` = TRENDING_STRONG: Hurst > 0.60, persistent direction (Holy Grail setups) — *originally ADX > 30*
  - `1` = TRENDING_IMPULSE: Hurst 0.55-0.60, sharp move (breakout trades) — *originally ADX 20-30*
  - `2` = RANGE_DAY: Bounded trading, directional lean (mean reversion)  
  - `3` = CONSOLIDATING_CHOP: Hurst < 0.50, narrow ATR (avoid entries, compression building) — *originally ADX < 20*
  - `4` = EXTREME_DISLOCATION: NH-NL extremes, panic/euphoria (capitulation trades)
- **Usage**: TRENDING (0-1) = trade with trend. RANGE_DAY (2) = fade extremes. CONSOLIDATING (3) = wait for breakout.

### 35. nh_nl_signal
- **JSON Key**: `"nh_nl_signal"`
- **Enum Type**: `NhNlSignalEnum`  
- **Value Range**: -4 to 6
- **Description**: Dr. Elder's New Highs - New Lows breadth indicator (market health assessment)
- **States**:
  - `-4` = BROADENING_DECLINE: Price falling + worsening breadth (strong downtrend)
  - `-3` = EXTREME_HIGHS_PEAK: S&P new high but NH-NL can't reach +2500 (narrow rally warning)
  - `-2` = BEARISH_DIVERGENCE: S&P higher high + NH-NL lower high after zero cross (sell signal)
  - `-1` = BEARISH_CONFIRMATION: Daily < -100 (bears in control)
  - `0` = UNCLEAR: Daily between +100 and -100 (neutral zone)  
  - `1` = BULLISH_CONFIRMATION: Daily > +100 or Weekly > +2500 (bull market confirmed)
  - `2` = BULLISH_DIVERGENCE: S&P lower low + NH-NL higher low after zero cross (buy signal)
  - `3` = EXTREME_LOWS_BOUNCE: Weekly < -4000 then rises (panic capitulation, ~1 year trend)
  - `4` = NARROWING_RALLY: Price rising but fewer stocks participating (weak rally will fail)
  - `5` = BROADENING_RALLY: Price rising + increasing participation (healthy trend)
  - `6` = NARROWING_DECLINE: Price falling but breadth improving (selling exhaustion)
- **Usage**: Divergences (±2) and extremes (±3) are actionable signals. Confirmations (±1) validate trend health.

---

## Cross-Market Correlation Indicators (Added January 5, 2026)

Cross-market indicators track ES correlations and trends of ZN (10-Year Treasury) and DX (Dollar Index). Elite hedge funds use these relationships for regime detection, divergence filtering, and risk management.

### 36. corr_es_zn
- **JSON Key**: `"corr_es_zn"`  
- **Type**: `float` (-1.0 to +1.0, transmitted as floating-point, not enum integer)
- **Description**: 60-min rolling 20-bar correlation between ES and ZN (10-Year Treasury Notes)
- **Interpretation**:
  - **< -0.7:** STRONG_NEGATIVE (classic "risk-on/risk-off" behavior) → Normal market
  - **-0.7 to -0.3:** WEAK_NEGATIVE (moderate inverse) → Typical environment
  - **-0.3 to +0.3:** NEUTRAL → **HIGH ALERT** - Correlation breakdown, regime change
  - **+0.3 to +0.7:** WEAK_POSITIVE (moderately aligned) → Risk-off behavior
  - **> +0.7:** STRONG_POSITIVE → **CRITICAL** - Both falling (flight to quality failure), reduce equity exposure
- **Usage**: Monitor for correlation breakdown (neutral/positive). When ES and bonds both fall, exit long equity positions.

### 37. corr_es_dx
- **JSON Key**: `"corr_es_dx"`
- **Type**: `float` (-1.0 to +1.0, transmitted as floating-point, not enum integer)
- **Description**: 60-min rolling 20-bar correlation between ES and DX (Dollar Index)
- **Interpretation**:
  - **< -0.7:** STRONG_NEGATIVE (dollar weakness supports equity rally) → Risk-on
  - **-0.7 to -0.3:** WEAK_NEGATIVE (normal inverse) → Typical environment
  - **-0.3 to +0.3:** NEUTRAL → Decoupling, monitor for stress
  - **+0.3 to +0.7:** WEAK_POSITIVE (both moving together) → Unusual coupling
  - **> +0.7:** STRONG_POSITIVE → Dollar and equities both falling (extreme stress)
- **Usage**: Strong negative correlation = healthy equity rally (dollar weakness). Positive correlation = market stress.

### 38. zn_trend
- **JSON Key**: `"zn_trend"`
- **Enum Type**: `CrossMarketTrendEnum`  
- **Value Range**: -1 to 1
- **Description**: 10-Year Treasury (ZN) trend direction (26-period EMA on 60-min bars)
- **States**:
  - `-1` = DOWN: Bond yields rising, risk appetite increasing (equity bullish)
  - `0` = FLAT: Sideways, no clear trend
  - `1` = UP: Bond yields falling, risk-off behavior (equity bearish)
- **Usage**: ZN UP while ES rallying = unusual "both up" behavior (low conviction rally, reduce size). ZN DOWN while ES rallying = classic risk-on (full size).

### 39. dx_trend
- **JSON Key**: `"dx_trend"`
- **Enum Type**: `CrossMarketTrendEnum`
- **Value Range**: -1 to 1
- **Description**: Dollar Index (DX) trend direction (26-period EMA on 60-min bars)  
- **States**:
  - `-1` = DOWN: Dollar weakening, supports equity rally (equity bullish)
  - `0` = FLAT: Sideways, no clear trend
  - `1` = UP: Dollar strengthening, weighs on equities (equity bearish)
- **Usage**: DX UP while ES rallying = dollar headwind (reduce size 25%). DX DOWN during ES rally = supportive macro (full size).

### 40. oscillator_310
- **JSON Key**: `"oscillator_310"`
- **Type**: `float` (transmitted as floating-point, raw oscillator value: EMA(3) - EMA(16))
- **Description**: Linda Raschke's 3-10 Oscillator raw value (actually 3-16 EMA difference on 15-min bars)
- **Interpretation**:
  - **> 0:** Above zero line = bullish momentum
  - **< 0:** Below zero line = bearish momentum  
  - **Magnitude:** Distance from zero indicates strength of momentum
- **Usage**: Cross above zero = bullish momentum. Cross below zero = bearish momentum. Used for overnight momentum failure detection.

### 41. oscillator_310_cross
- **JSON Key**: `"oscillator_310_cross"`  
- **Enum Type**: `Oscillator310CrossEnum`
- **Value Range**: 0–2
- **Description**: 3-10 Oscillator crossover state (fast line vs slow line)
- **States**:
  - `0` = NEUTRAL: No crossover detected (continues in current direction)
  - `1` = BULLISH_CROSS: Fast EMA crossed above slow EMA (upward momentum accelerating)
  - `2` = BEARISH_CROSS: Fast EMA crossed below slow EMA (downward momentum decelerating)
- **Usage**: BULLISH_CROSS (1) = tactical long entry trigger (if aligned with Screen 1). BEARISH_CROSS (2) = tactical short entry OR **overnight momentum failure exit** (if holding long during Globex).

**Cross-Market Regime Combinations:**

| ES | ZN Trend | DX Trend | Interpretation | Action |
|----|----------|----------|----------------|--------|
| UP | DOWN (-1) | DOWN (-1) | Classic Risk-On | Full size trades |
| UP | UP (+1) | DOWN (-1) | Unusual (both up) | Reduce size 25% |
| DOWN | UP (+1) | UP (+1) | Classic Risk-Off | Avoid new longs |
| UP | DOWN (-1) | UP (+1) | Dollar headwind | Reduce size 25% |
| DOWN | DOWN (-1) | DOWN (-1) | Macro stress | Avoid all entries |

---

## Price Level Keys

Price level keys transmit floating-point values (not enums). These are critical reference levels for stop placement and target calculation.

### 42. prev_high
- **JSON Key**: `"prev_high"`  
- **Type**: `float` (not enum)
- **Description**: Previous day's high (daily data)
- **Usage**: Resistance level. Breakout above = bullish. Use for stop placement on shorts.

### 43. prev_low
- **JSON Key**: `"prev_low"`
- **Type**: `float` (not enum)
- **Description**: Previous day's low (daily data)  
- **Usage**: Support level. Breakdown below = bearish. Use for stop placement on longs.

### 44. max_high
- **JSON Key**: `"max_high"`
- **Type**: `float` (not enum)
- **Description**: Maximum high over lookback period (e.g., 20-day high)
- **Usage**: Major resistance. Breakout above = strong bullish signal.

### 45. min_low  
- **JSON Key**: `"min_low"`
- **Type**: `float` (not enum)
- **Description**: Minimum low over lookback period (e.g., 20-day low)
- **Usage**: Major support. Breakdown below = strong bearish signal.

### 46. interm_ema_price
- **JSON Key**: `"interm_ema_price"`
- **Type**: `float` (not enum)  
- **Description**: 60-period EMA price level (Screen 2 reference)
- **Usage**: Screen 2 pullback target. Enter longs when price touches this level in uptrend.

### 47. fast_interm_ema_price
- **JSON Key**: `"fast_interm_ema_price"`
- **Type**: `float` (not enum)
- **Description**: Fast EMA price level (Screen 2, e.g., 13-period)  
- **Usage**: Short-term trend reference. Price above = bullish short-term bias.

### 48. interm_atr
- **JSON Key**: `"interm_atr"`
- **Type**: `float` (not enum)
- **Description**: 14-period ATR value on Screen 2 (60-min)
- **Usage**: Volatility reference for stop distance and position sizing. Use 2× ATR for swing stops.

### 49. upper_channel  
- **JSON Key**: `"upper_channel"`
- **Type**: `float` (not enum)
- **Description**: Upper Keltner Channel band (EMA + 2.5× ATR)
- **Usage**: Resistance level. Price at/above = overbought, potential mean reversion.

### 50. lower_channel
- **JSON Key**: `"lower_channel"`
- **Type**: `float` (not enum)  
- **Description**: Lower Keltner Channel band (EMA - 2.5× ATR)
- **Usage**: Support level. Price at/below = oversold, potential mean reversion.

---

## Integration Notes

### 1. Parsing Strategy
```python
# Example Python parsing
import json

def parse_indicators(json_payload: str) -> dict:
    """Parse JSON payload and validate enum ranges."""
    data = json.loads(json_payload)
    
    # Extract Screen 1 long_macd
    long_macd_value = data["screen1"]["long_macd"]
    if not (0 <= long_macd_value <= 10):
        raise ValueError(f"Invalid long_macd value: {long_macd_value}")
    
    # Map to human-readable state
    macd_states = {
        0: "NEG_TICK_UP", 1: "SPRING", 2: "POS_TICK_DOWN",
        3: "FALL", 4: "SUMMER", 5: "WINTER", 6: "FLAT",
        7: "ZERO_FROM_BELOW", 8: "ZERO_FROM_ABOVE",
        9: "BULLISH_CROSS", 10: "BEARISH_CROSS"
    }
    long_macd_label = macd_states.get(long_macd_value, "UNKNOWN")
    
    return {
        "long_macd_value": long_macd_value,
        "long_macd_label": long_macd_label
    }
```

### 2. Color Coding Recommendations
- **Bullish States** (positive values or uptrend): Green (#28a745)  
- **Bearish States** (negative values or downtrend): Red (#dc3545)
- **Neutral States** (zero or undefined): Gray (#6c757d)
- **Extreme States** (±3 or EXTREME): Bold + larger font
- **Signal States** (actionable entries): Flashing/highlighted border

### 3. Display Priority
**High Priority** (always visible):
- `side` (FLAT/LONG/SHORT) — Top of GUI, large font
- `raschke_tactical_trigger` — Entry signal, flashing when active  
- `time_of_day` — Session quality, color-coded (green = SWEET_SPOT, red = LUNCH_DEAD_ZONE)
- `market_regime` — Regime classification, affects strategy selection

**Medium Priority** (dashboard):
- Screen 1: `long_macd`, `long_imp`, `long_FI13_signal`
- Screen 2: `interm_stochastic`, `raschke_strategy_setup`, `interm_FI2_signal`
- Screen 3: `kangaroo_tail`, `turtle_soup`, `momentum_pinball`, `elder_breakout`, `nr7`

**Low Priority** (details panel):  
- Divergences: `long_macd_divergence`, `interm_macd_divergence`
- Price levels: All 9 price level keys
- Volume: `volume`, `volume_signal`

### 4. Null/Undefined Handling
If an enum value is transmitted as `-999` or `null`, treat as:
- **Undefined**: Gray out indicator
- **Display**: Show "N/A" or "—"  
- **Logic**: Do not use for trading decisions

### 5. Enum Value Changes
If C++ enum definitions change (adding new states, reordering values):
1. Update this document first
2. Version the JSON payload schema (`"schema_version": "1.1"`)
3. GUI must validate schema version on connect
4. Reject payloads with mismatched schema versions

### 6. Performance Optimization  
- **Caching**: Cache enum-to-label mappings (don't recompute on every update)
- **Diffing**: Only re-render indicators that changed value (compare previous payload)
- **Batch Updates**: Group Screen 3 updates (15-min) to avoid GUI flicker (buffer 100-200ms)

---

## Example Complete JSON Payload

```json
{
  "schema_version": "1.0",
  "timestamp": "2025-12-12T14:35:00Z",
  "screen1": {
    "long_macd": 1,
    "long_FI13_signal": 1,
    "long_macd_divergence": 0,
    "long_imp": 0,
    "long_ema": 2,
    "long_mkt_action": 2
  },
  "screen2": {
    "interm_stochastic": 3,
    "raschke_strategy_setup": 13,
    "raschke_tactical_trigger": 0,
    "rsi": 3,
    "interm_FI2_signal": 1,
    "ema_proximity": 3,
    "price_metrics": 1,
    "interm_macd_divergence": 0,
    "interm_imp": 0,
    "interm_macd": 1,
    "interm_mkt_action": 3
  },
  "screen3": {
    "structure_test": 1,
    "volume": 15234,
    "volume_signal": 0,
    "atr_proximity": 0,
    "daily_bias": 1,
    "short_mkt_action": 3,
    "kangaroo_tail": 0,
    "turtle_soup": 0,
    "momentum_pinball": 2,
    "elder_breakout": 0,
    "nr7": 2
  },
  "general": {
    "side": 1,
    "market_symbol": 1,
    "time_of_day": 2,
    "holding_strategy": 1,
    "market_regime": 1,
    "nh_nl_signal": 1
  },
  "price_levels": {
    "prev_high": 5875.50,
    "prev_low": 5842.25,
    "max_high": 5898.75,
    "min_low": 5795.00,
    "interm_ema_price": 5858.30,
    "fast_interm_ema_price": 5862.80,
    "interm_atr": 12.45,
    "upper_channel": 5889.55,
    "lower_channel": 5827.05
  }
}
```

**Interpretation**:
- **Screen 1**: BULLISH (MACD = SPRING, Impulse = GREEN, EMA = INC, FI13 = BULLISH_TREND_CONFIRMED)
- **Screen 2**: PULLBACK SETUP (Stochastic = OVERSOLD, Setup = HOLY_GRAIL_BUY, FI2 = PULLBACK_FOR_LONG, EMA_PROXIMITY = AT_EMA)
- **Screen 3**: ENTRY SIGNAL (Momentum Pinball = BULLISH_STRONG, NR7 = STRONG compression, Daily Bias = BULLISH_ACCEPTANCE)
- **General**: LONG recommended, SWEET_SPOT session (10:30-12:00), SWING_POSITION, TRENDING_IMPULSE regime
- **Action**: ENTER LONG at current price (~5858.30 at EMA), stop below prev_low (5842.25), target upper_channel (5889.55)

---

## Error Handling

### Common Errors and Solutions

| Error Type | Cause | GUI Handling |
|------------|-------|-------------|
| **Out-of-Range Enum** | Value >max or <min | Log error, display "ERROR", alert user |
| **Missing Key** | Indicator not in JSON | Display "N/A", log warning (possible version mismatch) |
| **Null Value** | Indicator undefined in C++ | Display "—", gray out indicator |
| **Type Mismatch** | Expected int, got string | Reject payload, display "INVALID DATA" |
| **Schema Version Mismatch** | C++ updated, GUI outdated | Display warning banner, request GUI update |
| **Connection Loss** | ZMQ socket disconnect | Display "DISCONNECTED", retry connection |
| **Stale Data** | No update >5 minutes | Display "STALE" indicator, yellow warning |

### Error Logging (Recommended)
```python
import logging

def validate_and_parse(json_payload: str):
    try:
        data = json.loads(json_payload)
    except json.JSONDecodeError as e:
        logging.error(f"Invalid JSON: {e}")
        return None
    
    # Validate schema version
    if data.get("schema_version") != "1.0":
        logging.warning(f"Schema version mismatch: {data.get('schema_version')}")
        return None
    
    # Validate key indicators
    if "screen1" not in data or "long_macd" not in data["screen1"]:
        logging.error("Missing required key: screen1.long_macd")
        return None
    
    return data
```

---

## Appendix: Quick Reference Tables

### A.1: Enum Value Range Summary

| Indicator | Enum Type | Min | Max | Neutral |
|-----------|-----------|-----|-----|---------|
| long_macd | MacdEnum | 0 | 11 | 12 (AT_ZERO) |
| long_FI13_signal | FI13Enum | -4 | 4 | 0 (UNCLEAR) |
| long_macd_divergence | MACDDivergenceEnum | -5 | 5 | 0 (NONE) |
| long_imp | ImpulseEnum | 0 | 7 | 2 (BLUE) |
| long_ema | EmaEnum | 0 | 3 | 1 (FLAT) |
| interm_stochastic | StochasticEnum | 0 | 5 | 1 (NORMAL) |
| raschke_strategy_setup | RaschkeStrategySetup | 0 | 21 | 0 (NONE) |
| raschke_tactical_trigger | RaschkeTacticalTrigger | 0 | 10 | 0 (NONE) |
| rsi | RSI | 0 | 5 | 1 (NORMAL) |
| interm_FI2_signal | FI2Enum | -2 | 2 | 0 (NEUTRAL) |
| ema_proximity | EmaProximity | -1 | 8 | 3 (AT_EMA) |
| price_metrics | PriceMetrics | 0 | 2 | 0 (NORMAL) |
| structure_test | StructureTest | 0 | 8 | 0 (NONE) |
| volume_signal | VolumeEnum | 0 | 6 | 0 (NORMAL) |
| atr_proximity | ATRProximityEnum | 0 | 4 | 0 (LOW_VOLATILITY) |
| daily_bias | DailyBiasEnum | -2 | 2 | 0 (VALUE_AREA_NEUTRAL) |
| kangaroo_tail | KangarooTailEnum | -3 | 3 | 0 (NONE) |
| turtle_soup | TurtleSoupEnum | -3 | 3 | 0 (NONE) |
| momentum_pinball | MomentumPinballEnum | -3 | 3 | 0 (NONE) |
| elder_breakout | ElderBreakoutEnum | -3 | 3 | 0 (NONE) |
| nr7 | NR7Enum | 0 | 3 | 0 (NONE) |
| side | TradeSideEnum | 0 | 2 | 0 (FLAT) |
| market_symbol | MarketSymbol | 1 | 5 | N/A |
| time_of_day | TimeOfDayEnum | 0 | 7 | N/A |
| holding_strategy | HoldingStrategyEnum | 0 | 3 | 3 (UNDEFINED) |
| market_regime | MarketRegimeEnum | -1 | 4 | -1 (UNDEFINED) |
| nh_nl_signal | NhNlSignalEnum | -4 | 6 | 0 (UNCLEAR) |

### A.2: Bullish/Bearish State Quick Lookup

| Indicator | Bullish States | Bearish States |
|-----------|----------------|----------------|
| long_macd | 0, 1, 7, 9 | 2, 3, 8, 10 |
| long_FI13_signal | 1, 2, 3, 4 | -1, -2, -3, -4 |
| long_imp | 0 (GREEN) | 1 (RED) |
| long_ema | 2 (INC) | 3 (DEC) |
| interm_stochastic | 3 (OVERSOLD), 4 (BULL DIV) | 2 (OVERBOUGHT), 5 (BEAR DIV) |
| raschke_tactical_trigger | 1, 3, 5, 7, 9 (odd = buy) | 2, 4, 6, 8, 10 (even = sell) |
| interm_FI2_signal | 1, 2 | -1, -2 |
| ema_proximity | 2 (CROSS_ABOVE) | 4 (CROSS_BELOW) |
| kangaroo_tail | 1, 2, 3 | -1, -2, -3 |
| turtle_soup | 1, 2, 3 | -1, -2, -3 |
| momentum_pinball | 1, 2, 3 | -1, -2, -3 |
| elder_breakout | 1, 2, 3 | -1, -2, -3 |
| daily_bias | 1, 2 | -1, -2 |
| side | 1 (LONG) | 2 (SHORT) |

### A.3: Priority Indicator Thresholds

| Indicator | Actionable States | High-Quality Threshold |
|-----------|-------------------|------------------------|
| raschke_tactical_trigger | Any non-zero (1-10) | STRONG/EXTREME patterns (value ≥7) |
| long_macd_divergence | ±5 (SIGNAL states) | ±5 only (confirmed + uptick/downtick) |
| kangaroo_tail | ±2, ±3 | ±2 or ±3 (STRONG/EXTREME) |
| turtle_soup | ±2, ±3 | ±2 or ±3 (STRONG/EXTREME) |
| momentum_pinball | ±2, ±3 | ±2 or ±3 (STRONG/EXTREME) |
| elder_breakout | ±2, ±3 | ±2 or ±3 (STRONG/EXTREME) |
| nr7 | 2, 3 | 3 (EXTREME compression) |
| interm_FI2_signal | ±1 | ±1 (pullback/rally in trend) |
| time_of_day | 1, 2, 4 | 2 (SWEET_SPOT) |
| market_regime | 0, 1 | 0 (TRENDING_STRONG) |

---

## Document Version History

| Version | Date | Changes |
|---------|------|---------|
| 1.3 | 2026-01-18 | **CRITICAL FIELD NAME RECONCILIATION**: Updated `long_impulse` → `long_imp`, `interm_impulse` → `interm_imp`, `volume_raw` → `volume` (raw int) + `volume_signal` (enum). Flagged missing fields: `long_mkt_action`, `interm_mkt_action`, `short_mkt_action`, `daily_bias` (not present in raw data). All field names now match `data/raw/event_data_*.jsonl` (source of truth). |
| 1.2 | 2026-01-05 | Added 6 cross-market correlation indicators (#36-41): corr_es_zn, corr_es_dx, zn_trend, dx_trend, oscillator_310, oscillator_310_cross. Renumbered price level indicators to #42-50. Elite Enhancement 13 integration. |
| 1.1 | 2025-12-24 | Added overnight_exit (#33). Updated time_of_day and holding_strategy for Raschke/Taylor overnight management. |
| 1.0 | 2025-12-12 | Initial release. All 43 indicators documented (34 enums + 9 price levels). |

---

**End of Document**
