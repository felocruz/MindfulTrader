# C++ Transformer Fields Implementation Checklist

**Author:** MindfulTrader Development Team
**Last Updated:** December 28, 2025
**Purpose:** Track C++ implementation status for placeholder fields in TransformerData.jsonl

---

## Overview

This document tracks the implementation status of 6 placeholder fields that currently contain zero or near-zero values in the Transformer training dataset. Each field requires C++ logic in Sierra Chart ACSIL studies.

**Current Status:** 6/6 fields implemented ✅ ALL COMPLETE

**Last Implementation Date:** December 28, 2025

---

## Implementation Priority

1. **HIGH PRIORITY (Critical for pattern quality)** ✅ COMPLETED
   - ✅ oscillator_310_cross
   - ✅ oscillator_310_divergence
   - ✅ trend_strength_long (Elite Hedge Fund Methodology)
   - ✅ trend_strength_short (Elite Hedge Fund Methodology)

2. **MEDIUM PRIORITY (Contract cleanup)** ✅ COMPLETED
   - ✅ removed deprecated metadata counters from schema/code/docs

---

## Field Implementation Status

### 1. Deprecated Metadata Counters

**Status:** ✅ REMOVED FROM CONTRACT
**Priority:** MEDIUM
**Removal Date:** April 2026

These fields were deprecated and removed end-to-end from schema, serializers/exporters, and Python ingestion surfaces.

---

### 2. oscillator_310_cross

**Status:** ✅ COMPLETED
**Priority:** 🔴 HIGH (Critical for Linda Raschke methodology)
**Completion Date:** December 28, 2025
**Implementation:** DataCollectorStudy.cpp lines 286-333

**Implementation Details:**
- Fast Line: EMA(3) - EMA(16) from Screen3 (15-min)
- Slow Line: SMA(16, Fast Line)
- Cross detection: Fast vs Slow relationship change
- Values: 0=NEUTRAL, 1=FAST_ABOVE_SLOW (bullish), 2=FAST_BELOW_SLOW (bearish)
- Uses same-chart reference `GetStudyArrayUsingID` (Screen3 is 15-min like DataCollector)

**Required Implementation:**

1. **Oscillator Selection**
   - [ ] Choose oscillator type (recommend: 3/10 MACD or custom momentum indicator)
   - [ ] Research Linda Raschke's preferred 3/10 oscillator definition
   - [ ] Alternative: Use 3-period and 10-period stochastic or RSI

2. **Oscillator Calculation**
   - [ ] Implement 3-period oscillator (fast)
   - [ ] Implement 10-period oscillator (slow)
   - [ ] Calculate difference: `diff = osc_3 - osc_10`

3. **Crossover Detection**
   - [ ] Store previous bar's difference
   - [ ] Detect bullish cross: `prev_diff < 0 && curr_diff > 0` → value = +1
   - [ ] Detect bearish cross: `prev_diff > 0 && curr_diff < 0` → value = -1
   - [ ] No cross: value = 0

4. **C++ Study Location**
   - [ ] Option A: Add to `TripleScreen2.cpp` (inline calculation)
   - [ ] Option B: Create dedicated 3/10 Oscillator study, reference in TripleScreen2
   - [ ] Export `oscillator_310_cross` in TransformerData JSONL

5. **Testing Criteria**
   - [ ] Verify 5-15% of bars show crossovers (typical crossover frequency)
   - [ ] Visually inspect crossovers align with price momentum changes
   - [ ] Confirm Elder Breakout patterns align with bullish crosses

**Integration Steps:**
1. Implement 3/10 oscillator calculation
2. Add crossover detection logic
3. Test on historical data with visual overlay
4. Integrate into TransformerDataExporter
5. Re-export training dataset
6. Validate field with `analyze_transformer_data.py`
7. Update TRANSFORMER_DATA_FIELDS.md with actual distributions

**Completion Date:** _Not started_

---

### 3. oscillator_310_divergence

**Status:** ✅ COMPLETED
**Priority:** 🔴 HIGH (Critical for reversal pattern quality)
**Completion Date:** December 28, 2025
**Implementation:** DataCollectorStudy.cpp lines 336-424

**Implementation Details:**
- 5-bar swing detection for both price and oscillator
- Searches for TWO swings (previous and current) to compare
- Bearish divergence (2): Price higher high + oscillator lower high
- Bullish divergence (1): Price lower low + oscillator higher low
- Minimum 3-bar spacing between swings
- 25-bar lookback window for finding swing pairs

**Required Implementation:**

1. **Prerequisites**
   - [ ] ⚠️ **Depends on:** oscillator_310_cross implementation (must complete first)
   - [ ] Requires swing high/low detection in price
   - [ ] Requires swing high/low detection in oscillator

2. **Swing Detection Logic**
   - [ ] Implement price swing high detection (local peak)
   - [ ] Implement price swing low detection (local trough)
   - [ ] Implement oscillator swing high detection
   - [ ] Implement oscillator swing low detection
   - [ ] Use N-bar lookback (recommend 5-10 bars each side)

3. **Divergence Detection**
   - [ ] **Bearish Divergence (value = -1):**
     - [ ] Price makes higher high
     - [ ] Oscillator makes lower high
     - [ ] Indicates weakening uptrend (reversal signal)
   - [ ] **Bullish Divergence (value = +1):**
     - [ ] Price makes lower low
     - [ ] Oscillator makes higher low
     - [ ] Indicates weakening downtrend (reversal signal)
   - [ ] **No Divergence (value = 0):**
     - [ ] Price and oscillator aligned
     - [ ] Or no recent swing points to compare

4. **C++ Study Location**
   - [ ] Add to existing 3/10 Oscillator study (if separate)
   - [ ] Or add inline to `TripleScreen2.cpp`
   - [ ] Requires persistent storage of previous swings (both price and oscillator)

5. **Testing Criteria**
   - [ ] Verify 2-5% of bars show divergence (divergence is relatively rare)
   - [ ] Manually validate divergence signals on charts (use Sierra Chart drawing tools)
   - [ ] Confirm Turtle Soup patterns coincide with bearish divergence
   - [ ] Check Kangaroo Tail reversals align with divergence zones

**Integration Steps:**
1. Implement swing detection for price and oscillator
2. Add divergence comparison logic
3. Test extensively on historical data (divergence is subtle, prone to false positives)
4. Integrate into TransformerDataExporter
5. Re-export dataset
6. Validate with analyze script
7. Update field documentation

**Completion Date:** _Not started_

---

### 4. trend_strength_long

**Status:** ✅ IMPLEMENTED (Elite Hedge Fund Methodology)
**Priority:** HIGH (Elite Standard)
**Implementation Date:** December 28, 2025
**Location:** DataCollectorStudy.cpp (lines 18-159)
**Completion:** 100%

**Implementation Summary:**

1. **Elite Trend Strength Components** ✅
   - [x] **Component 1: ADX (40% weight)**
     - [x] 14-period ADX via sc.ADX() API
     - [x] Clamped to 0-100 range
   - [x] **Component 2: Volatility-Adjusted EMA Slope (40% weight)**
     - [x] 21-period EMA slope normalized by ATR(14)
     - [x] Rank-based percentile over 200-bar lookback
   - [x] **Component 3: Volatility-Adjusted Momentum (20% weight)**
     - [x] 10-period momentum normalized by ATR(14)
     - [x] Rank-based percentile over 200-bar lookback

2. **Elite Aggregation Formula** ✅
   - [x] Three-component weighted sum with regime-aware scoring:
     ```cpp
     // Bullish Regime (emaBias > 0):
     bullish = (ADX * 0.4) + (emaSlopePercentile * 0.4) + (momentumPercentile * 0.2)
     bearish = (ADX * 0.2) + ((100 - emaSlopePercentile) * 0.4) + ((100 - momentumPercentile) * 0.2)

     // Bearish Regime (emaBias < 0):
     bearish = (ADX * 0.4) + ((100 - emaSlopePercentile) * 0.4) + ((100 - momentumPercentile) * 0.2)
     bullish = (ADX * 0.2) + (emaSlopePercentile * 0.4) + (momentumPercentile * 0.2)
     ```
   - [x] Final values clamped to 0-100 range
   - [x] Returns 0.0 for bars < 200 (insufficient data)

3. **C++ Study Location** ✅
   - [x] Implemented in DataCollectorStudy.cpp
   - [x] Helper functions:
     - `NormalizeToPercentile()` - Rank-based percentile over lookback
     - `CalculateNormalizedEMASlope()` - Volatility-adjusted EMA slope
     - `CalculateNormalizedMomentum()` - Volatility-adjusted momentum
     - `CalculateEliteTrendStrength()` - Main aggregation function
   - [x] Exported in TransformerData.jsonl (line 687)

4. **Validation Criteria** ⏳ PENDING DEPLOYMENT
   - [ ] Deploy and run replay to generate data
   - [ ] Verify trend_strength_long > 60 in strong uptrends
   - [ ] Confirm trend_strength_long < 30 in ranging markets
   - [ ] Validate anticorrelation with trend_strength_short
   - [ ] Check distribution (expect 0.0 for first 200 bars, then 0-100 range)

**Key Features:**
- **Volatility Normalization:** All components divided by ATR(14) to create "thick signal"
- **Rank-Based Percentile:** 200-bar lookback for regime-adaptive normalization
- **Regime-Aware Scoring:** Amplifies bullish score in bullish regime, dampens in bearish
- **Gradient-Descent Friendly:** Continuous 0-100 scale (not discrete cliffs)
- **Professional Standard:** Matches elite hedge fund methodology for regime filtering

**Completion Date:** December 28, 2025

---

### 5. trend_strength_short

**Status:** ✅ IMPLEMENTED (Elite Hedge Fund Methodology)
**Priority:** HIGH (Elite Standard)
**Implementation Date:** December 28, 2025
**Location:** DataCollectorStudy.cpp (lines 18-159)
**Completion:** 100%

**Implementation Summary:**

1. **Mirror of trend_strength_long** ✅
   - [x] Uses same CalculateEliteTrendStrength() function
   - [x] Returns separate bearish score in TrendStrengthScores struct
   - [x] Regime-aware scoring inverts logic for bearish trends

2. **Elite Aggregation Formula (Inverted)** ✅
   - [x] Same components as bullish, but inverted percentiles:
     ```cpp
     // Bearish Regime (emaBias < 0):
     bearish = (ADX * 0.4) + ((100 - emaSlopePercentile) * 0.4) + ((100 - momentumPercentile) * 0.2)
     bullish = (ADX * 0.2) + (emaSlopePercentile * 0.4) + (momentumPercentile * 0.2)
     ```
   - [x] Amplifies bearish score when EMA slope is negative
   - [x] Dampens bullish score in bearish regime

3. **C++ Study Location** ✅
   - [x] Implemented alongside trend_strength_long in DataCollectorStudy.cpp
   - [x] Exported in TransformerData.jsonl (line 688)
   - [x] Returns 0.0 for bars < 200 (insufficient data)

4. **Validation Criteria** ⏳ PENDING DEPLOYMENT
   - [ ] Deploy and run replay to generate data
   - [ ] Verify trend_strength_short > 60 in strong downtrends
   - [ ] Confirm anticorrelation with trend_strength_long
   - [ ] Validate regime sensitivity (one high, one low in trending markets)
   - [ ] Check distribution (expect 0.0 for first 200 bars, then 0-100 range)

**Key Features:**
- **Unified Implementation:** Single function returns both bullish and bearish scores
- **Regime-Aware:** Automatically adapts scoring based on EMA bias
- **Anticorrelated:** When trend_strength_long is high (>70), trend_strength_short should be low (<30)
- **Professional Standard:** Matches elite hedge fund methodology for regime filtering

**Completion Date:** December 28, 2025

---

## Testing Protocol

For each implemented field:

1. **Unit Test**
   - [ ] Test calculation logic in isolation
   - [ ] Verify edge cases (zero volume, missing data, first bars of session)

2. **Integration Test**
   - [ ] Export training data with new field
   - [ ] Run `python analyze_transformer_data.py --field-validation`
   - [ ] Check field moves from "PLACEHOLDER" to "WORKING" status

3. **Visual Validation**
   - [ ] Plot field values on Sierra Chart overlays
   - [ ] Manually inspect 10-20 examples
   - [ ] Confirm field values align with visual interpretation

4. **Statistical Validation**
   - [ ] Check distribution matches expectations (documented in TRANSFORMER_DATA_FIELDS.md)
   - [ ] Verify non-zero percentage is reasonable (not all zeros, not 100% non-zero)
   - [ ] Confirm unique value count > 10 (sufficient variety)

---

## Deployment Checklist

When all fields are implemented:

- [ ] **Step 1:** Full data re-export
  - [ ] Run C++ studies on complete historical dataset
  - [ ] Generate new `data/TransformerData.jsonl`
  - [ ] Backup old file: `data/TransformerData_pre_fields_YYYYMMDD.jsonl`

- [ ] **Step 2:** Field validation
  - [ ] Run: `python analyze_transformer_data.py --field-validation`
  - [ ] Confirm all 11 fields show "WORKING" status
  - [ ] Review any warnings or alerts

- [ ] **Step 3:** Model retraining
  - [ ] Update Transformer model input features (now 11 fields instead of 5)
  - [ ] Retrain model with new data
  - [ ] Evaluate model performance improvement
  - [ ] Document results in TRANSFORMER_MODEL_INTEGRATION.md

- [ ] **Step 4:** Documentation updates
  - [ ] Update TRANSFORMER_DATA_FIELDS.md (move fields from Placeholder to Working)
  - [ ] Update TRAINING_DATA_EXPORT.md with new field export logic
  - [ ] Add completion dates to this checklist

---

## Dependencies

**Field Dependencies (must implement in order):**
1. oscillator_310_cross → required before oscillator_310_divergence
2. trend_strength_long → required before trend_strength_short

**Study Dependencies:**
- All fields require TransformerDataExporter logic in `src/TripleScreen2.cpp`
- Gap fields may need session boundary detection helper functions
- Oscillator fields may benefit from dedicated 3/10 Oscillator study

---

## Progress Tracking

| Field | Status | Priority | Effort | Assigned | Started | Completed |
|-------|--------|----------|--------|----------|---------|-----------|
| oscillator_310_cross | ❌ Not Started | HIGH | 4-6h | _Unassigned_ | _TBD_ | _TBD_ |
| oscillator_310_divergence | ❌ Not Started | HIGH | 6-8h | _Unassigned_ | _TBD_ | _TBD_ |
| trend_strength_long | ❌ Not Started | MEDIUM | 3-5h | _Unassigned_ | _TBD_ | _TBD_ |
| trend_strength_short | ❌ Not Started | MEDIUM | 1-2h | _Unassigned_ | _TBD_ | _TBD_ |

**Total Estimated Effort:** 14-21 hours

---

## See Also

- [TRANSFORMER_DATA_FIELDS.md](TRANSFORMER_DATA_FIELDS.md) - Field definitions and usage
- [TRAINING_DATA_EXPORT.md](TRAINING_DATA_EXPORT.md) - Data export process
- [TRANSFORMER_MODEL_INTEGRATION.md](TRANSFORMER_MODEL_INTEGRATION.md) - Model architecture
- [analyze_transformer_data.py](../analyze_transformer_data.py) - Field validation script

---

**Next Review Date:** _Schedule review after first field implementation_
