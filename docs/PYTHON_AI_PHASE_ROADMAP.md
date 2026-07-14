# Python AI Integration Phase Roadmap

**Last Updated**: December 30, 2025  
**Owner**: Python AI Team + C++ Execution Team  
**Status**: Phase 1 Active, Phase 2/3 Pending Validation

---

## Executive Summary

The Python AI team is implementing multi-output models (quality + urgency heads) in **three phases**, with C++ changes gated by validation metrics. This document tracks:

1. What's implemented (Phase 1: ✅ Complete)
2. What's on hold (Phase 2/3: Pending validation)
3. Decision criteria for each phase gate
4. C++ work required at each stage

---

## Phase 1: Multi-Output Model (✅ COMPLETE - Dec 30, 2025)

### Objective
Validate that quality/urgency predictions improve trade performance **without collecting new data**.

### Python Implementation
```python
# Model architecture
model_outputs = {
    'action': Dense(3, activation='softmax'),      # LONG/SHORT/FLAT
    'quality': Dense(1, activation='sigmoid'),      # Setup quality [0-1]
    'urgency': Dense(1, activation='sigmoid')       # Execution urgency [0-1]
}

# Veto mechanism
if prediction['quality'] < 0.6:
    return "VETO_TRADE"  # Skip low-quality setups
```

### Data Sources (All Existing)
| Feature | Source | Timeframe |
|---------|--------|-----------|
| OHLCV | Sierra Chart | 15m bars |
| Pattern Quality | `turtle_soup_quality`, `elder_breakout_quality` | 15m bars |
| Volume/Range | `volume_ratio_percent`, `bar_range_percentile` | 15m bars |

### C++ Integration
**Changes Required**: ✅ None

**Future Enhancement** (when Phase 1 validates):
```cpp
// In Systems.cpp trade execution
if (aiRecommendation.quality < 0.60) {
    sc.AddMessageToLog("Trade vetoed: Low quality", 1);
    return;  // Skip trade
}

SCOrderType orderType = (aiRecommendation.urgency > 0.7) 
    ? SCT_MARKET   // High urgency = aggressive fill
    : SCT_LIMIT;    // Low urgency = passive fill
```

### Success Metrics (Jan 2-15, 2026)
| Metric | Target | Measurement |
|--------|--------|-------------|
| **Quality Filter** | Win rate +10% on quality>0.8 trades | Backtest comparison |
| **Urgency Correlation** | R² > 0.3 with fill slippage | Simulation analysis |
| **Sharpe Improvement** | +15% with veto mechanism | Walk-forward test |
| **False Positives** | <20% good setups vetoed | Manual review |

**Gate Decision**: Proceed to Phase 2 only if 3/4 metrics hit targets.

---

## Phase 2: 1-Minute Bar Collection (⏸️ ON HOLD)

### Trigger Conditions
✅ Phase 1 metrics validated (mid-January 2026)  
✅ Pattern quality fields stable at 1m resolution  
✅ C++ team available for 2-3 week development sprint

### Objective
Capture intra-bar order flow to improve entry timing and quality predictions.

### C++ Changes Required

#### 1. New ACSIL Study: `AI_DataExport_1Min.cpp`

**File**: `src/AI_DataExport_1Min.cpp`

```cpp
SCSFExport scsf_AIDataExport1Min(SCStudyInterfaceRef sc) {
    if (sc.SetDefaults) {
        sc.GraphName = "AI Data Export (1-Min)";
        sc.StudyDescription = "Exports 1m bars to TransformerData1Min.jsonl";
        sc.GraphRegion = 0;
        sc.AutoLoop = 1;
        return;
    }
    
    // Export on bar close only
    if (sc.GetBarHasClosedStatus() != BHCS_BAR_HAS_CLOSED) return;
    
    nlohmann::json barData;
    barData["timestamp"] = sc.BaseDateTimeIn[sc.Index];
    barData["open"] = sc.Open[sc.Index];
    barData["high"] = sc.High[sc.Index];
    barData["low"] = sc.Low[sc.Index];
    barData["close"] = sc.Close[sc.Index];
    barData["volume"] = sc.Volume[sc.Index];
    
    // NEW: Order flow metrics (if available from Sierra Chart)
    barData["cumulative_delta"] = GetCumulativeDelta(sc, sc.Index);
    barData["vwap_distance"] = CalculateVWAPDistance(sc, sc.Index);
    barData["relative_volume"] = sc.Volume[sc.Index] / GetAvgVolume(sc, 20);
    
    // Preserve pattern quality from parent 15m bar
    barData["parent_pattern_quality"] = Get15mPatternQuality(sc);
    
    // Append to separate file
    AppendToJSONL("data/TransformerData1Min.jsonl", barData);
}
```

#### 2. Order Flow Helper Functions

**File**: `src/OrderFlowMetrics.cpp` (new)

```cpp
float GetCumulativeDelta(SCStudyInterfaceRef sc, int index) {
    // If Sierra Chart provides bid/ask volume arrays:
    // return sc.AskVolume[index] - sc.BidVolume[index];
    
    // Otherwise, estimate from tick direction:
    float delta = 0.0f;
    for (int i = std::max(0, index - 5); i <= index; i++) {
        if (sc.Close[i] >= sc.Close[i-1]) {
            delta += sc.Volume[i];  // Uptick
        } else {
            delta -= sc.Volume[i];  // Downtick
        }
    }
    return delta;
}

float CalculateVWAPDistance(SCStudyInterfaceRef sc, int index) {
    // VWAP = sum(price * volume) / sum(volume)
    float vwap = CalculateVWAP(sc, index, 20);  // 20-period VWAP
    float atr = GetATR(sc, index, 14);
    return (sc.Close[index] - vwap) / atr;  // Normalized distance
}
```

#### 3. Pattern Quality at 1m Resolution

**Question for C++ Team**: Can these patterns detect at 1m scale?

| Pattern | 15m Detection | 1m Feasibility | Notes |
|---------|---------------|----------------|-------|
| Turtle Soup | ✅ Yes | ⚠️ Maybe | Need 5+ bars for false breakout |
| Elder Breakout | ✅ Yes | ✅ Yes | Can detect on 1m if 15m confirms |
| Momentum Pinball | ✅ Yes | ❌ No | Requires 20+ bars |
| Kangaroo Tail | ✅ Yes | ✅ Yes | Single bar pattern |
| NR7 | ✅ Yes | ⚠️ Maybe | Needs 7 bars |

**Recommendation**: Export 15m pattern quality as `parent_pattern_quality` in 1m bars. Don't recompute at 1m.

### Data Collection Timeline
| Week | Activity | Bars Collected | Status |
|------|----------|----------------|--------|
| Week 1-2 | C++ development + testing | 0 | Development |
| Week 3-6 | Live data collection | ~20k bars | Validation |
| Week 7-14 | Model training window | ~40k bars | Training |
| Week 15+ | Walk-forward validation | Ongoing | Production |

**Minimum Data Requirement**: 30,000 bars (~3 months of 1m data during market hours)

### Success Metrics (March-April 2026)
| Metric | Target | Measurement |
|--------|--------|-------------|
| **Quality Prediction** | +15% accuracy vs 15m-only | Cross-validation |
| **Entry Timing** | -20% slippage on 1m entries | Backtest simulation |
| **Urgency Accuracy** | R² > 0.5 with realized urgency | Post-trade analysis |
| **Data Quality** | <1% missing bars | Data pipeline monitoring |

**Gate Decision**: Proceed to Phase 3 only if metrics show 20%+ improvement over Phase 1.

---

## Phase 3: Multi-Scale Transformer (⏸️ RESEARCH PHASE)

### Trigger Conditions
✅ Phase 2 shows 20%+ metric improvement  
✅ 1m data collection stable for 3+ months  
✅ Model architecture validated in offline tests

### Objective
Process multiple timeframes simultaneously for hierarchical decision-making.

### Architecture
```
┌──────────────────────────────────────────────────┐
│         Multi-Scale Transformer                  │
│                                                  │
│  Micro (1m)  ──►  Attention ──►  Entry Signal   │
│  Tactical (15m) ──►  Fusion  ──►  Quality Score │
│  Swing (60m)   ──►  Layers  ──►  Urgency Score  │
│  Macro (240m)  ──►           ──►  Regime Context│
└──────────────────────────────────────────────────┘
```

### C++ Changes Required

#### 1. Four Timeframe Exports

**File**: `src/AI_DataExport_MultiScale.cpp`

```cpp
// Export 1m bars
ExportBar("data/Transformer_1Min.jsonl", 60);

// Export 15m bars (existing)
ExportBar("data/Transformer_15Min.jsonl", 900);

// Export 60m bars (NEW)
ExportBar("data/Transformer_60Min.jsonl", 3600);

// Export 240m bars (NEW - macro regime)
ExportBar("data/Transformer_240Min.jsonl", 14400);
```

#### 2. State Machine for Intra-Bar Logic

**File**: `include/IntraBarStateMachine.h`

```cpp
enum class IntraBarState {
    WAITING_FOR_QUALITY,      // Quality head not triggered yet
    ENTRY_WINDOW_ACTIVE,      // Quality confirmed, waiting for urgency
    URGENCY_ELEVATED,         // High urgency detected, execute
    VETO_APPLIED,             // Quality dropped, cancel pending
    POSITION_HELD             // Trade active, monitor exit
};

class IntraBarStateMachine {
public:
    void OnQualityUpdate(float quality);
    void OnUrgencyUpdate(float urgency);
    void OnPriceUpdate(float close);
    IntraBarState GetCurrentState() const;
    
private:
    IntraBarState m_state;
    float m_entryQuality;
    float m_currentUrgency;
    SCDateTime m_stateChangeTime;
};
```

#### 3. Enhanced Live Inference Protocol

**ZeroMQ Message Schema (JSON)**:

```json
{
  "timestamp": 1735574400,
  "action": "ENTER_LONG",
  "confidence": 0.82,
  
  "quality": 0.87,                // Multi-scale quality score
  "urgency": 0.72,                // Immediate execution need
  
  "macro_trend": 1,               // 240m: Bullish(1), Bearish(-1), Neutral(0)
  "swing_structure": "HH_HL",     // 60m: Higher highs/lows
  "tactical_setup": "TURTLE_SOUP", // 15m: Pattern name
  "micro_trigger": "LIQUIDITY_SWEEP", // 1m: Entry catalyst
  
  "intra_bar_state": 2,           // State machine index
  "time_in_state": 120,           // Seconds in current state
  
  "risk_params": {
    "stop_loss": 4985.50,
    "target": 5012.25,
    "position_size": 2
  }
}
```

**C++ Execution Integration**:

```cpp
// In Systems.cpp
void ProcessAIRecommendation(const nlohmann::json& msg) {
    // Validate multi-scale agreement
    if (msg["macro_trend"] == -1 && msg["action"] == "ENTER_LONG") {
        sc.AddMessageToLog("WARNING: Macro bearish but tactical long", 1);
        // Reduce position size or skip
    }
    
    // State machine check
    IntraBarState state = static_cast<IntraBarState>(msg["intra_bar_state"]);
    if (state != IntraBarState::URGENCY_ELEVATED) {
        return;  // Wait for proper state
    }
    
    // Quality veto (Phase 1 logic)
    if (msg["quality"] < 0.65) {
        sc.AddMessageToLog("Trade vetoed: Low quality", 1);
        return;
    }
    
    // Execute with urgency-based order type
    ExecuteTrade(msg, DetermineOrderType(msg["urgency"]));
}
```

### Development Timeline (Estimated)
| Week | Activity | Deliverable |
|------|----------|-------------|
| Week 1-2 | Multi-scale export | Four timeframe JSONL files |
| Week 3-4 | State machine | C++ state tracking |
| Week 5-6 | Integration testing | Live sim validation |
| Week 7-8 | Model retraining | Multi-scale Transformer |
| Week 9-12 | Walk-forward test | Production readiness |

**Total Estimated Effort**: 6-8 weeks C++ + Python combined

---

## Binary Protocol Decision Matrix

### When to Implement Binary (Reference: [BINARY_BRIDGE_SPEC.md](BINARY_BRIDGE_SPEC.md))

| Scenario | Bar Frequency | Daily Packets | Protocol | Rationale |
|----------|---------------|---------------|----------|-----------|
| **Phase 1** | 15m | 96 | ✅ JSON | 0.09ms savings not material |
| **Phase 2** | 1m | ~1,500 | ✅ JSON | Still <1% of inference time |
| **Phase 3 (base)** | Multi-scale | ~2,000 | ✅ JSON | Complexity not justified |
| **Phase 3 (HFT)** | Tick-by-tick | >10,000 | ⚠️ Binary | Consider if latency critical |
| **Never** | Any | <1,000 | ❌ Binary | Over-engineering |

### Hybrid Protocol Option (Phase 3+)

If tick-level data becomes necessary:

```cpp
// Fast Path: Binary price updates (high frequency)
struct PriceUpdate {
    uint32_t seq;
    float close;
    float delta;
    float atr;
} __attribute__((packed));  // 16 bytes

// Slow Path: JSON context (every 15m)
{
  "pattern_quality": 0.87,
  "tactical_setup": "TURTLE_SOUP",
  "macro_regime": 1
}
```

**Benefits**:
- ✅ Binary for 10,000+ price ticks/day (<10µs latency)
- ✅ JSON for 96 pattern updates/day (human-readable)
- ✅ Preserves interpretability where it matters

---

## C++ Team Questions & Answers

### Q1: Pattern Quality at 1m Resolution
**Python Team Asks**: Are these patterns stable at 1m timeframe?

| Pattern | 1m Feasibility | C++ Team Response Needed |
|---------|----------------|--------------------------|
| `turtle_soup_quality` | ⚠️ Maybe (needs 5+ bars) | Can you detect on 1m? |
| `elder_breakout_quality` | ✅ Yes | Confirm detection logic works |
| `momentum_pinball_quality` | ❌ No (needs 20+ bars) | Export 15m value only |
| `kangaroo_tail_quality` | ✅ Yes (single bar) | Should work |
| `nr7_quality` | ⚠️ Maybe (needs 7 bars) | Test feasibility |

**Recommendation**: Export `parent_15m_pattern_quality` in 1m bars. Don't recompute.

### Q2: Order Flow Data Availability
**Python Team Asks**: Can Sierra Chart provide these via ACSIL?

| Metric | Sierra Chart API | Alternative Calculation |
|--------|------------------|-------------------------|
| Cumulative Delta | `sc.AskVolume - sc.BidVolume` | Estimate from tick direction |
| VWAP | Built-in study | Compute from price*volume |
| Volume-at-Price | `sc.VolumeAtPriceForBars` | Histogram from trades |
| Ticks Above Ask | Not directly exposed | Count upticks |

**Action**: C++ team verify which order flow metrics are available.

### Q3: JSONL Append Performance
**Python Team Concern**: Will 1m export (30k bars/week) cause I/O bottleneck?

**Current**: 15m export writes ~100 bars/day (negligible)  
**Phase 2**: 1m export writes ~1,500 bars/day  
**Estimated Impact**: ~150KB/day additional disk I/O

**Recommendation**: 
- ✅ File append is fast enough (opens file, writes line, closes)
- ⚠️ Consider daily log rotation to prevent file bloat
- ⚠️ Monitor disk latency in live environment

### Q4: Execution Layer Integration
**Python Team**: Can order routing consume `quality` and `urgency`?

**Required Changes**:
```cpp
// Add to existing AIRecommendation struct
struct AIRecommendation {
    int action;           // Existing: LONG/SHORT/FLAT
    float confidence;     // Existing
    float quality;        // NEW - Phase 1
    float urgency;        // NEW - Phase 1
};

// In order routing logic
SCOrderType DetermineOrderType(float urgency) {
    if (urgency > 0.8) return SCT_MARKET;      // Aggressive fill
    if (urgency > 0.5) return SCT_LIMIT_CHASE; // Chase the limit
    return SCT_LIMIT;                          // Passive fill
}
```

**Estimated Effort**: 1-2 hours to add fields and basic routing logic.

---

## Phase Gate Summary

| Phase | Status | C++ Work | Timeline | Decision Date |
|-------|--------|----------|----------|---------------|
| **Phase 1** | ✅ Complete | None | Dec 2025 | N/A |
| **Phase 1 Validation** | 🔄 Active | None | Jan 2-15, 2026 | Jan 15, 2026 |
| **Phase 2** | ⏸️ On Hold | 2-3 weeks | Q1 2026 | IF validation passes |
| **Phase 3** | ⏸️ Research | 6-8 weeks | Q2 2026 | IF Phase 2 shows 20%+ gain |
| **Binary Protocol** | ❌ Not Planned | 4-6 weeks | TBD | Only if >10k packets/day |

---

## Contact & Coordination

**Python AI Team Lead**: [Your Name]  
**C++ Execution Team Lead**: [C++ Lead]  
**Next Sync**: After Phase 1 validation (mid-January 2026)

**Immediate Actions**:
- ✅ Python: Train Phase 1 model (this week)
- ✅ Python: Backtest veto mechanism (Jan 2-15)
- ⏸️ C++: Review pattern quality at 1m feasibility
- ⏸️ C++: Verify order flow data availability
- 📅 Schedule: Phase 2 kickoff if metrics hit targets

---

## Appendix: Decision Flowchart

```
Phase 1 Complete (Dec 30, 2025)
         │
         ▼
   Backtest Jan 2-15
         │
    ┌────┴────┐
    │         │
Quality  Urgency
>0.8 Win  R²>0.3?
 Rate      Fill
 +10%?   Slippage
    │         │
    └────┬────┘
         │
    3/4 Metrics Hit?
         │
    ┌────┴────┐
   NO         YES
    │          │
Iterate    Phase 2:
Features   1m Export
    │          │
    └──────────┤
         ▼
   Collect 3 Months
   1m Data
         │
    ┌────┴────┐
   Metrics    Metrics
  <20% Gain   >20% Gain
    │          │
   STOP    Phase 3:
           Multi-Scale
               │
          ┌────┴────┐
      <10k       >10k
    Packets    Packets
     /Day       /Day
      │          │
   JSON      Binary
  Protocol   Protocol
```

---

**Last Updated**: December 30, 2025  
**Version**: 1.0 (Phase 1 Complete)  
**Next Review**: January 15, 2026 (Post-Validation)
