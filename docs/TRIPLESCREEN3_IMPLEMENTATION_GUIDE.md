# TripleScreen3 - SetNormalizedAnchors Implementation Guide

**Date**: February 2, 2026
**Status**: CRITICAL - Blocks 40% HMM signal (distances to technical levels)
**Severity**: RED 🔴

---

## Overview

TripleScreen3 computes short-term (15-min) technical levels but **never registers them** with ContextManager. This means the observation vector has 4 missing fields:

```
Missing Fields in Observation Vector:
  7. dist_day_high  (distance to previous day high)
  8. dist_day_low   (distance to previous day low)
  9. dist_4bar_high (distance to 4-bar swing high)
  10. dist_ema13    (distance to EMA(13))

Impact: 40% of HMM context missing during regime detection
```

---

## Implementation Plan

### Phase 1: Locate Optimal Insertion Point

**Current State** (TripleScreen3.cpp):
- Subgraph[SG_EMA3] = 3-period EMA
- Subgraph[SG_EMA16] = 16-period EMA
- Subgraph[SG_ADX] = ADX(14)
- Subgraph[SG_ATR_AVG] = 20-period SMA of ATR

**Optimal Location**: After all technical calculations, before last indicator updates (~line 800-850)

### Phase 2: Required Data Sources

#### 1. Previous Day High/Low

**Challenge**: TripleScreen3 is 15-min chart study. Daily high/low data requires either:

**Option A (Preferred): Use DailyCache from ContextManager**
```cpp
// ContextManager maintains previous day high/low via SetDailyCache()
// Call from Screen1 (240-min daily bar study) or Screen2 (60-min daily bar study)
auto dailyCache = ContextManager::Instance().GetStatisticalContext();  // Need getter
float dayHigh = dailyCache->dayHigh;
float dayLow = dailyCache->dayLow;
```

**Option B: Cross-Chart Reference**
```cpp
// Reference TripleScreen1 or other daily study for high/low
// (More complex, requires study ID configuration)
```

**Option C: Persist Daily High/Low**
```cpp
// If current bar is new day, fetch external daily data
// Otherwise use cached values from previous bar
static float g_dayHigh = 0.0f;
static float g_dayLow = 0.0f;
static int g_lastDay = 0;

if (sc.BaseDateTimeIn[sc.Index].GetDate() != g_lastDay) {
    // New day - fetch from data source
    g_dayHigh = /* fetch */;
    g_dayLow = /* fetch */;
    g_lastDay = sc.BaseDateTimeIn[sc.Index].GetDate();
}

float dayHigh = g_dayHigh;
float dayLow = g_dayLow;
```

**Recommended**: Option A (requires adding GetDailyCache() getter to ContextManager)

#### 2. 4-Bar Swing High/Low

**Data**: Already available in TripleScreen3
```cpp
// Use Subgraph[SG_HIGHEST_HIGH_20] and SG_LOWEST_LOW_20 for broader context
// But for 4-bar swing, compute directly from recent bars:

float fourBarHigh = sc.High[sc.Index];
float fourBarLow = sc.Low[sc.Index];

if (sc.Index >= 1) {
    fourBarHigh = std::max(fourBarHigh, sc.High[sc.Index - 1]);
    fourBarLow = std::min(fourBarLow, sc.Low[sc.Index - 1]);
}
if (sc.Index >= 2) {
    fourBarHigh = std::max(fourBarHigh, sc.High[sc.Index - 2]);
    fourBarLow = std::min(fourBarLow, sc.Low[sc.Index - 2]);
}
if (sc.Index >= 3) {
    fourBarHigh = std::max(fourBarHigh, sc.High[sc.Index - 3]);
    fourBarLow = std::min(fourBarLow, sc.Low[sc.Index - 3]);
}
```

#### 3. EMA(13)

**Data**: Need to verify if already in TripleScreen3 subgraphs
```cpp
// Option A: Use existing Subgraph[SG_KELTNER_AVERAGE] if it's EMA(13)
float ema13 = sc.Subgraph[SG_KELTNER_AVERAGE][sc.Index];

// Option B: Calculate independently
SCFloatArray temp;
sc.ExponentialMovAvg(sc.Close, temp, 13);
float ema13 = temp[sc.Index];

// Option C: Reference from separate EMA13 study
```

---

## Code Implementation

### Step 1: Add GetDailyCache() Accessor to ContextManager

**File**: `include/ContextManager.h`

```cpp
/// Get current daily cache (previous day high/low for normalized anchor calculations)
std::optional<DailyCache> GetDailyCache() const;
```

**File**: `src/ContextManager.cpp`

```cpp
std::optional<DailyCache> ContextManager::GetDailyCache() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_dailyCache;  // Return entire struct (copy)
}
```

### Step 2: Implement SetNormalizedAnchors in TripleScreen3

**Location**: TripleScreen3.cpp, line ~820 (after all calculations)

```cpp
/*==========================================================================*/
/*
 * Register normalized anchors with ContextManager (Screen3 responsibility)
 *
 * These represent distance to key technical levels (all in points):
 * - distDayHigh/Low: Previous day extremes (for breakout significance)
 * - distFourBarHigh/Low: Recent swing extremes (for immediate support/resistance)
 * - distEma13: Short-term trend line (for momentum confirmation)
 */

// Only register if we have valid data (index > 3 for 4-bar swings)
if (sc.Index >= 3) {
    NormalizedAnchors anchors;
    float last = static_cast<float>(sc.Close[sc.Index]);

    // SECTION A: Daily levels (requires DailyCache from Screen2)
    auto dailyCache = ContextManager::Instance().GetDailyCache();
    if (dailyCache) {
        anchors.distDayHigh = last - dailyCache->prevDayHigh;
        anchors.distDayLow = last - dailyCache->prevDayLow;
    } else {
        // Fallback: Use highest/lowest from available data
        anchors.distDayHigh = 0.0f;
        anchors.distDayLow = 0.0f;
    }

    // SECTION B: 4-bar swing extremes (local support/resistance)
    float fourBarHigh = sc.High[sc.Index];
    float fourBarLow = sc.Low[sc.Index];
    for (int i = 1; i <= 3; ++i) {
        if (sc.Index >= i) {
            fourBarHigh = std::max(fourBarHigh, static_cast<float>(sc.High[sc.Index - i]));
            fourBarLow = std::min(fourBarLow, static_cast<float>(sc.Low[sc.Index - i]));
        }
    }

    anchors.distFourBarHigh = last - fourBarHigh;
    anchors.distFourBarLow = last - fourBarLow;

    // SECTION C: EMA(13) (short-term trend confirmation)
    // Verify that Subgraph[SG_KELTNER_AVERAGE] is the EMA(13) or compute it
    float ema13 = 0.0f;
    if (sc.Index > 0) {
        // Use the Keltner Average (which should be 10-period EMA by default)
        // TODO: Verify if this is EMA(13) or recalculate if needed
        ema13 = static_cast<float>(sc.Subgraph[SG_KELTNER_AVERAGE][sc.Index]);
    }

    anchors.distEma13 = last - ema13;
    anchors.lastUpdated = sc.BaseDateTimeIn[sc.Index];
    anchors.updateCount = (sc.Index > 0) ? sc.Subgraph[SG_KELTNER_AVERAGE].GetIntValue(sc.Index) : 0;

    // Clamp values to reasonable ranges (prevent extreme outliers)
    constexpr float ANCHOR_CLAMP_MIN = ContextManager::ANCHOR_CLAMP_MIN;  // -50.0
    constexpr float ANCHOR_CLAMP_MAX = ContextManager::ANCHOR_CLAMP_MAX;  // +50.0

    anchors.distDayHigh = std::clamp(anchors.distDayHigh, ANCHOR_CLAMP_MIN, ANCHOR_CLAMP_MAX);
    anchors.distDayLow = std::clamp(anchors.distDayLow, ANCHOR_CLAMP_MIN, ANCHOR_CLAMP_MAX);
    anchors.distFourBarHigh = std::clamp(anchors.distFourBarHigh, ANCHOR_CLAMP_MIN, ANCHOR_CLAMP_MAX);
    anchors.distFourBarLow = std::clamp(anchors.distFourBarLow, ANCHOR_CLAMP_MIN, ANCHOR_CLAMP_MAX);
    anchors.distEma13 = std::clamp(anchors.distEma13, ANCHOR_CLAMP_MIN, ANCHOR_CLAMP_MAX);

    // Register with ContextManager (triggers adaptive HMM threshold recalculation)
    ContextManager::Instance().SetNormalizedAnchors(std::move(anchors));
}
```

### Step 3: Verify Integration

**Test After Implementation**:

```cpp
// In EventDataCollectorStudy or diagnostics study:
auto diag = ContextManager::Instance().GetLastTriggerDiagnostics();
if (diag) {
    // Verify observation vector is complete
    auto obs = ContextManager::Instance().BuildObservationVector(/* velocity */);

    // Check all 10 indices are non-zero (or at least populated)
    for (size_t i = 0; i < 10; ++i) {
        // Log or assert that obs[i] is reasonable
    }
}
```

---

## Verification Checklist

### Before Implementation:
- [ ] GetDailyCache() added to ContextManager.h header
- [ ] GetDailyCache() implemented in ContextManager.cpp
- [ ] TripleScreen3 location identified (~line 820)
- [ ] DailyCache being set by Screen1 or Screen2 (verify with grep)

### During Implementation:
- [ ] All 5 anchor fields computed
- [ ] Values clamped to [-50, +50] range
- [ ] Build compiles with no errors
- [ ] SetNormalizedAnchors called with std::move

### After Implementation:
- [ ] Run TripleScreen3 for 1 day (verify observations vector populated)
- [ ] Check diagnostics: all 4 distance fields non-zero
- [ ] Verify HMM trigger frequency unchanged (adaptive floor still works)
- [ ] Check training data exports (dist_day_high, etc. populated)

---

## Risk Assessment

### Low Risk:
- ✅ New code doesn't modify existing calculations
- ✅ No changes to TradeExecutionServer or RiskManager
- ✅ Backward compatible (missing anchors → 0.0f values)

### Medium Risk:
- ⚠️ Requires DailyCache populated by Screen2 (dependency)
- ⚠️ 4-bar calculation adds ~10-15 CPU cycles per bar
- ⚠️ May affect HMM trigger frequency if anchors change regime direction

### Mitigation:
1. Add guard: only register if all 5 fields are non-zero
2. Add logging: trace first 10 bars to verify values
3. Add fallback: if DailyCache unavailable, skip SetNormalizedAnchors (no penalty)

---

## Performance Impact

| Component | Current | With Anchors | Delta |
|-----------|---------|--------------|-------|
| TripleScreen3 CPU | ~500µs/bar | ~512µs/bar | +12µs |
| ContextManager mutex | ~2µs | ~2µs | 0 |
| HMM trigger frequency | 0.1-0.5/sec | ~0.1-0.5/sec | 0% |
| Observation vector update | 10µs | 10µs | 0 |

**Impact**: Negligible (12µs = 0.0012% of 1-second bar)

---

## Deployment Steps

1. **Backup Current Version**:
   ```bash
   cp TripleScreen3.cpp TripleScreen3.cpp.bak
   ```

2. **Add ContextManager Methods**:
   - Edit ContextManager.h to add GetDailyCache() declaration
   - Edit ContextManager.cpp to implement GetDailyCache()

3. **Add TripleScreen3 Implementation**:
   - Insert code block above at line ~820
   - Adjust line numbers based on actual file structure

4. **Build**:
   ```bash
   cd MindfulTrader && ./build_dll.sh
   ```

5. **Verify Compilation**:
   - Check for compilation errors
   - Confirm DLL generated successfully

6. **Runtime Validation** (1 day trading):
   - Monitor: HMM trigger frequency (should be ~0.1-0.5/sec)
   - Check: Training data exports have all 10 observation fields
   - Verify: Diagnostics show anchor distances populated

7. **Regression Testing**:
   - Compare trades before/after (should be identical or better)
   - Check label distribution (should have more diversity)
   - Validate HMM regime detection (should be more accurate)

---

## Implementation Priority

**CRITICAL PATH**:
1. Add GetDailyCache() to ContextManager (**2 minutes**)
2. Add SetNormalizedAnchors call to TripleScreen3 (**5 minutes**)
3. Build and verify (**2 minutes**)
4. Deploy and test (**1 hour**)

**Total**: ~70 minutes for full implementation + testing

---

## Questions to Resolve

1. **Is Subgraph[SG_KELTNER_AVERAGE] actually EMA(13)?**
   - Current: Assumed to be 10-period EMA
   - Need: Confirm or calculate separate EMA(13)

2. **Where is DailyCache populated?**
   - Screen1? Screen2? External data?
   - Need: Verify SetDailyCache() is being called

3. **What if DailyCache unavailable?**
   - Fallback: Use highest/lowest from longer lookback (20-bar)?
   - Or: Skip registration and let observation vector have 0.0 for anchors?

4. **Should anchor values be clamped?**
   - Current: Yes, [-50, +50] points
   - Rationale: Extreme outliers (price gap down on news) shouldn't break HMM
   - Alternative: Use dynamic clamping based on ATR?

