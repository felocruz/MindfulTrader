# Transformer Data Fields Reference

**Author:** MindfulTrader Development Team
**Last Updated:** December 28, 2025
**Purpose:** Comprehensive documentation of all fields in `data/TransformerData.jsonl` training dataset

---

## Overview

The Transformer training dataset contains 41,877 records spanning 698 days (July 26, 2023 → June 24, 2025). Each record represents a single bar with associated pattern recognition, market regime classification, and feature engineering data.

This document categorizes fields into **Working Fields** (fully implemented and validated) and **Placeholder Fields** (awaiting C++ implementation).

---

## Working Fields (5)

These fields are fully implemented in C++ studies and produce valid, distributed data suitable for Transformer model training.

### 1. close_percentile

**Type:** Float (0-100)
**Purpose:** Position of the bar's close within the daily high-low range
**Calculation:** `((close - low) / (high - low)) * 100`
**Implementation Status:** ✅ WORKING

**Expected Distribution:**
- Low range (0-33.3%): 18-23% of records
- Mid range (33.3-66.7%): 18-23% of records
- High range (66.7-100%): 18-23% of records
- Balanced distribution indicates healthy data

**Actual Distribution (Dec 2025 validation):**
- Low: 21.8%
- Mid: 25.8%
- High: 22.4%
- Assessment: ✅ Balanced

**Usage in Pattern Quality:**
- NR7 quality: Penalizes bars closing at extremes (very high or very low percentile)
- Kangaroo Tail quality: Rewards bars closing near opposite end from tail
- Elder Breakout quality: Rewards strong closes (>80th percentile for bulls, <20th for bears)

**C++ Study Location:** `src/TripleScreen2.cpp` (TransformerDataExporter section)

**Good Values:**
- NR7 setups: 30-70% (mid-range close)
- Kangaroo Tail Long: 70-100% (close near high after low wick)
- Elder Breakout Long: 80-100% (strong bull close)

**Bad Values:**
- Values outside 0-100 range (data error)
- Extreme values (<10% or >90%) for mean-reversion setups

---

### 2. bar_range_percentile

**Type:** Float (0-100)
**Purpose:** Current bar's range relative to recent volatility history
**Calculation:** Percentile rank of `(high - low)` over rolling 20-bar window
**Implementation Status:** ✅ WORKING

**Expected Distribution:**
- Good spread across 0-100 range
- Median: 40-50%
- P95 should be near 100% (capturing high-volatility bars)

**Actual Distribution (Dec 2025 validation):**
- Median: 45.0%
- P25: 22.3%
- P75: 68.1%
- P95: 95.2%
- Assessment: ✅ Good spread

**Usage in Pattern Quality:**
- NR7 quality: Rewards extremely narrow ranges (high percentile indicates NR7 failure)
- Momentum Pinball quality: Rewards expanded ranges (breakout confirmation)
- Turtle Soup quality: Rewards narrow follow-through bars after fake breakout

**C++ Study Location:** `src/TripleScreen2.cpp`

**Good Values:**
- NR7 setups: 0-10% (exceptionally narrow bar)
- Momentum Pinball: 70-100% (wide range breakout)
- Elder Breakout: 50-100% (expanding volatility)

**Bad Values:**
- Negative values (calculation error)
- Values >100 (percentile error)
- Always 50% (indicates stale rolling window)

---

### 3. ema_distance_percent

**Type:** Float (-1.0 to +1.0 typical range)
**Purpose:** Price deviation from 21-period Exponential Moving Average
**Calculation:** `((close - EMA21) / EMA21) * 100`
**Implementation Status:** ✅ WORKING

**Expected Distribution:**
- Tight range near zero (-0.64% to +0.07% observed)
- Mean should be close to 0% (market oscillates around EMA)
- Standard deviation: 0.1-0.3%

**Actual Distribution (Dec 2025 validation):**
- Mean: -0.012%
- Median: -0.015%
- Min: -0.64%
- Max: +0.07%
- Std: 0.134%
- Assessment: ✅ Tight range

**Usage in Pattern Quality:**
- Trend Following: Positive values favor LONG, negative favor SHORT
- Mean Reversion: Large deviations (>0.5%) increase reversal probability
- Elder Breakout: Confirms alignment with trend (price above/below EMA)

**C++ Study Location:** `src/TripleScreen2.cpp`

**Good Values:**
- Trend Following Long: +0.1% to +0.5% (above but not overextended)
- Mean Reversion Short: <-0.4% (significantly below, due for bounce)
- Range-bound: -0.1% to +0.1% (oscillating around EMA)

**Bad Values:**
- Values >5% or <-5% (extreme dislocation, possible data error)
- Always exactly 0% (indicates EMA not updating)

---

### 4. volume_ratio_percent

**Type:** Float (0% to 5000% observed, typical 20-200%)
**Purpose:** Current volume relative to 20-bar average volume
**Calculation:** `(current_volume / avg_volume_20) * 100`
**Implementation Status:** ✅ WORKING

**Expected Distribution:**
- Wide dynamic range (0.5% to 4,976% observed)
- Median: 80-90% (slightly below average)
- P95: 200-300% (volume spikes)
- Max values can exceed 1000% (extreme news/event bars)

**Actual Distribution (Dec 2025 validation):**
- Median: 85.9%
- Mean: 102.7%
- P95: 256.4%
- Max: 4,976.2%
- Assessment: ✅ Wide range (healthy)

**Usage in Pattern Quality:**
- Kangaroo Tail: Rewards volume >150% (capitulation bar)
- Elder Breakout: Rewards volume >120% (breakout confirmation)
- NR7: Penalizes high volume (indicates congestion, not breakout setup)

**C++ Study Location:** `src/TripleScreen2.cpp`

**Good Values:**
- Kangaroo Tail: 150-500% (spike volume confirms reversal)
- Elder Breakout: 120-200% (healthy breakout volume)
- NR7: 50-100% (low volume compression)

**Bad Values:**
- Values <10% (likely data error, volume should rarely be that low)
- Always exactly 100% (indicates average not updating)

---

### 5. is_trend_following

**Type:** Boolean (TRUE / FALSE)
**Purpose:** Classifies setup as trend continuation (TRUE) vs. mean reversion (FALSE)
**Calculation:** Based on entry type and pattern characteristics
**Implementation Status:** ✅ WORKING

**Expected Distribution:**
- TRUE: 10-20% (swing/trend trades less frequent)
- FALSE: 80-90% (scalp/range trades dominate intraday)

**Actual Distribution (Dec 2025 validation):**
- TRUE: 6,023 records (14.4%)
- FALSE: 35,854 records (85.6%)
- Assessment: ✅ Expected ratio

**Usage in Pattern Quality:**
- Determines which price action patterns are favored
- Trend Following TRUE: Rewards momentum patterns (Elder Breakout, Momentum Pinball)
- Trend Following FALSE: Rewards reversal patterns (Turtle Soup, Kangaroo Tail, NR7)

**C++ Study Location:** `src/TripleScreen2.cpp` (logic determining entry_type)

**Pattern Association:**
- TRUE: DOUBLE_REPO_FAILURE, ANTI_CONTINUATION (trend rides)
- FALSE: DOUBLE_REPO, MOMENTUM_PINBALL, NR7, TURTLE_SOUP (reversals)

**Good Values:**
- Consistent with entry_type field (SWING_TREND → TRUE, SCALP_RANGE → FALSE)
- Matches pattern characteristics (DOUBLE_REPO_FAILURE should always be TRUE)

**Bad Values:**
- DOUBLE_REPO_FAILURE with is_trend_following = FALSE (logic error)
- 100% TRUE or 100% FALSE (indicates field not updating correctly)

---

## Placeholder Fields (4)

These fields exist in the dataset but are **not implemented** in C++ studies. They currently contain all-zero or near-zero values and require C++ logic development.

### 1. oscillator_310_cross

**Type:** Integer (0, 1, 2)
**Purpose:** Detects 3/10 oscillator crossover (Raschke "Most Reliable Intraday Setup")
**Calculation:** Fast Line (EMA3-EMA16) vs Slow Line (SMA16 of Fast)
**Implementation Status:** ✅ IMPLEMENTED (December 28, 2025)

**Expected Values:**
- 0: No cross (NEUTRAL)
- 1: Bullish cross (FAST_ABOVE_SLOW)
- 2: Bearish cross (FAST_BELOW_SLOW)

**Implementation Details:**
- **Fast Line:** EMA(3) - EMA(16) from Screen3 (15-min)
- **Slow Line:** SMA(16, Fast Line)
- **Cross Detection:** Fast vs Slow relationship change (not zero-line cross)
- Expected frequency: 5-15% of bars

**C++ Study Location:** `src/DataCollectorStudy.cpp` lines 286-333

**Usage:**
- Anti pattern confirmation (+15 pts for London Window with oscillator agreement)
- Elder Breakout confirmation (breakout aligned with oscillator)
- Gap exit signals (divergence at morning session)
- Multi-day cycle rotation detection (Slow Line flattening)

---

### 2. oscillator_310_divergence

**Type:** Integer (0, 1, 2)
**Purpose:** Detects price/oscillator divergence (Taylor/Raschke methodology)
**Calculation:** Compare TWO price swings vs TWO oscillator swings
**Implementation Status:** ✅ IMPLEMENTED (December 28, 2025)

**Expected Values:**
- 0: No divergence (NONE)
- 1: Bullish divergence (price lower low, oscillator higher low)
- 2: Bearish divergence (price higher high, oscillator lower high)

**Implementation Details:**
- **Swing Detection:** 5-bar pattern (center bar highest/lowest among ±2 bars)
- **Two-Swing Algorithm:** Finds previous swing and current swing, compares them
- **Lookback:** 25 bars to find both swings, minimum 3-bar spacing
- Expected frequency: 2-5% of bars (divergence is rare)

**C++ Study Location:** `src/DataCollectorStudy.cpp` lines 336-424

**Usage:**
- Turtle Soup quality: Rewards setups on bearish divergence (overbought)
- Elder Breakout filter: Avoid longs on bearish divergence
- Kangaroo Tail confirmation: Divergence at reversal zone increases quality
- Gap exit signals: Price gaps favorable but oscillator diverges = take profits

---

### 3. trend_strength_long

**Type:** Float (0-100)
**Purpose:** Bullish trend strength score using elite hedge fund methodology
**Calculation:** Elite 3-component aggregation (ADX 40%, volatility-adjusted EMA slope 40%, volatility-adjusted momentum 20%)
**Implementation Status:** ✅ IMPLEMENTED (Elite 0-100 Scale)

**Expected Range:**
- 0-30: Weak/no bullish trend
- 30-70: Moderate bullish trend
- 70-100: Strong bullish trend ("thick signal" regime)

**Elite Implementation Details:**
- **ADX Component (40%):** 14-period ADX for trend strength measurement
- **Volatility-Adjusted EMA Slope (40%):** 21-period EMA slope normalized by ATR(14), rank-based percentile over 200 bars
- **Volatility-Adjusted Momentum (20%):** 10-period momentum normalized by ATR(14), rank-based percentile over 200 bars
- **Regime-Aware Scoring:** Separate bullish/bearish scores with amplification in trending regimes, dampening in counter-trend
- **Gradient-Descent Friendly:** Continuous 0-100 scale (not discrete buckets), log-normal distribution
- **Signal-to-Noise:** ATR normalization creates "thick signal" resistant to whipsaws

**Validation Criteria:**
- Requires 200+ bars for percentile ranking context
- Returns 0.0 for bars < 200 (insufficient data)
- Higher scores in trending markets, lower in ranging markets
- Anticorrelated with trend_strength_short in strong trends

**Usage:**
- Elder Breakout quality: Reward breakouts in strong trends (>60)
- DOUBLE_REPO_FAILURE quality: Confirm trend continuation environment
- Holy Grail filter: Avoid longs when trend_strength_long < 30
- Risk scaling: Higher position sizes when trend_strength_long > 70

**C++ Study Location:** DataCollectorStudy.cpp (CalculateEliteTrendStrength function)

**Priority:** HIGH (Elite hedge fund standard for regime filtering)

---

### 4. trend_strength_short

**Type:** Float (0-100)
**Purpose:** Bearish trend strength score using elite hedge fund methodology
**Calculation:** Elite 3-component aggregation (ADX 40%, inverted volatility-adjusted EMA slope 40%, inverted volatility-adjusted momentum 20%)
**Implementation Status:** ✅ IMPLEMENTED (Elite 0-100 Scale)

**Expected Range:**
- 0-30: Weak/no bearish trend
- 30-70: Moderate bearish trend
- 70-100: Strong bearish trend ("thick signal" regime)

**Elite Implementation Details:**
- **ADX Component (40%):** 14-period ADX for trend strength measurement (same as bullish)
- **Inverted Volatility-Adjusted EMA Slope (40%):** 21-period EMA slope normalized by ATR(14), inverted for bearish bias
- **Inverted Volatility-Adjusted Momentum (20%):** 10-period momentum normalized by ATR(14), inverted for bearish bias
- **Regime-Aware Scoring:** Separate bullish/bearish scores with amplification in trending regimes, dampening in counter-trend
- **Gradient-Descent Friendly:** Continuous 0-100 scale (not discrete buckets), log-normal distribution
- **Signal-to-Noise:** ATR normalization creates "thick signal" resistant to whipsaws

**Validation Criteria:**
- Requires 200+ bars for percentile ranking context
- Returns 0.0 for bars < 200 (insufficient data)
- Higher scores in downtrending markets, lower in ranging markets
- Anticorrelated with trend_strength_long in strong trends

**Usage:**
- Elder Breakout SHORT quality: Reward breakouts in strong downtrends (>60)
- DOUBLE_REPO_FAILURE SHORT quality: Confirm bearish continuation environment
- Turtle Soup filter: Avoid shorts when trend_strength_short < 30
- Risk scaling: Higher position sizes when trend_strength_short > 70
- Avoid counter-trend LONG trades when trend_strength_short > 70

**C++ Study Location:** DataCollectorStudy.cpp (CalculateEliteTrendStrength function)

**Priority:** HIGH (Elite hedge fund standard for regime filtering)

---

## Field Validation

The `analyze_transformer_data.py` script provides automated validation of all fields:

```bash
# Run full analysis including field validation
mamba activate mts
python analyze_transformer_data.py

# Field validation report section includes:
# - Working Fields Validation (checks expected distributions)
# - Placeholder Fields Validation (detects implementation status)
```

### Validation Criteria

**Working Fields:**
- Distribution checks (balanced for percentiles, tight range for EMA distance)
- Range validation (ensure values within expected bounds)
- Warnings generated if distributions deviate from expectations

**Placeholder Fields:**
- Detect all-zero or near-zero fields
- Count unique values (≤2 unique values indicates placeholder)
- Generate alerts when C++ implementation needed

### Example Validation Output

```
✅ WORKING FIELDS VALIDATION (5 fields)
✅ WORKING close_percentile
     Distribution: Low: 21.8%, Mid: 25.8%, High: 22.4%
     Assessment: Balanced

❌ PLACEHOLDER FIELDS VALIDATION (4 fields - C++ Implementation Needed)
```

---

## Implementation Checklist

Track C++ implementation progress in [CPP_TRANSFORMER_FIELDS_IMPLEMENTATION.md](CPP_TRANSFORMER_FIELDS_IMPLEMENTATION.md).

---

## See Also

- [TRANSFORMER_MODEL_INTEGRATION.md](TRANSFORMER_MODEL_INTEGRATION.md) - Model architecture and training
- [TRAINING_DATA_EXPORT.md](TRAINING_DATA_EXPORT.md) - Data export process from C++ studies
- [CPP_TRANSFORMER_FIELDS_IMPLEMENTATION.md](CPP_TRANSFORMER_FIELDS_IMPLEMENTATION.md) - Implementation tracking
- [analyze_transformer_data.py](../analyze_transformer_data.py) - Field validation script

---

**Last Validation:** December 28, 2025 (41,877 records analyzed)
