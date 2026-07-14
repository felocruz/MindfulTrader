# AI Model Health Status Requirements

## Purpose
This document specifies the **model_health_status.json** file format and integration requirements for the Python Performance Attribution Engine to communicate model health state to the C++ trading engine. This enables "soft lock" functionality - automatically disabling AI-generated signals when model performance degrades beyond acceptable thresholds.

**Expected Value:** $5K-$8K/year in prevented losses from degraded model performance.

---

## 1. File Format Specification

### File Location
```
/path/to/data/model_health_status.json
```

**Location Requirements:**
- Must be accessible to both Python (writer) and C++ (reader)
- Recommend: Same directory as daily_high_low.csv or configurable path
- File permissions: Python writes, C++ reads (644 permissions)

### JSON Schema

```json
{
  "status": "HEALTHY",
  "alpha_slippage_pct": 12.5,
  "sample_size": 847,
  "last_updated": "2025-12-20T14:32:15Z",
  "metrics": {
    "expected_sharpe": 1.85,
    "realized_sharpe": 1.62,
    "expected_winrate": 0.58,
    "realized_winrate": 0.54,
    "avg_mae_slippage_ticks": 2.3
  },
  "thresholds": {
    "warning_threshold_pct": 20.0,
    "soft_lock_threshold_pct": 30.0,
    "min_sample_size": 100
  }
}
```

### Field Definitions

| Field | Type | Required | Description |
|-------|------|----------|-------------|
| `status` | string | **YES** | Model health state: "HEALTHY", "WARNING", or "SOFT_LOCKED" |
| `alpha_slippage_pct` | number | **YES** | Percentage degradation from expected alpha (0-100) |
| `sample_size` | integer | **YES** | Number of trades in performance window (30-day rolling) |
| `last_updated` | string | **YES** | ISO8601 UTC timestamp of last calculation |
| `metrics.expected_sharpe` | number | NO | Backtested/expected Sharpe ratio |
| `metrics.realized_sharpe` | number | NO | Actual realized Sharpe ratio |
| `metrics.expected_winrate` | number | NO | Backtested/expected win rate (0.0-1.0) |
| `metrics.realized_winrate` | number | NO | Actual realized win rate (0.0-1.0) |
| `metrics.avg_mae_slippage_ticks` | number | NO | Average MAE slippage in ticks |
| `thresholds.warning_threshold_pct` | number | NO | Alpha slippage % for WARNING state (default: 20%) |
| `thresholds.soft_lock_threshold_pct` | number | NO | Alpha slippage % for SOFT_LOCKED state (default: 30%) |
| `thresholds.min_sample_size` | integer | NO | Minimum trades before calculating slippage (default: 100) |

---

## 2. Model Health Status States

### HEALTHY (Normal Operation)
**Condition:** `alpha_slippage_pct < warning_threshold_pct` (default: <20%)

**C++ Behavior:**
- Accept all AI-generated signals (subject to confidence thresholds)
- Normal risk sizing (regime multipliers active)
- Log: `✅ Model Health: HEALTHY (Alpha Slippage: 12.5%)`

**Python Action:**
- Continue normal performance tracking
- Update status file every 4 hours or after 50 new trades

---

### WARNING (Degraded Performance)
**Condition:** `warning_threshold_pct <= alpha_slippage_pct < soft_lock_threshold_pct` (default: 20%-30%)

**C++ Behavior:**
- Accept only HIGH confidence signals (>0.70 threshold instead of >0.55)
- Reduce position sizing by 50% (0.5x multiplier)
- Log: `⚠️ Model Health: WARNING (Alpha Slippage: 24.3%) - HIGH confidence only`

**Python Action:**
- Flag for immediate investigation
- Send notification (email/Slack if configured)
- Increase update frequency (every 1 hour)

---

### SOFT_LOCKED (Critical Degradation)
**Condition:** `alpha_slippage_pct >= soft_lock_threshold_pct` (default: ≥30%)

**C++ Behavior:**
- **Reject ALL AI-generated signals** (hard stop)
- Cancel any working orders (same as AI disconnect fast-purge)
- Allow manual trading only
- Log: `🔒 Model Health: SOFT_LOCKED (Alpha Slippage: 32.7%) - ALL AI signals rejected`

**Python Action:**
- **CRITICAL ALERT** - Model requires retraining or review
- Write SOFT_LOCKED status immediately (do not wait for update interval)
- Log detailed performance breakdown for investigation

---

## 3. Alpha Slippage Calculation

### Formula
```python
expected_alpha = expected_sharpe * sqrt(252) * portfolio_volatility
realized_alpha = realized_sharpe * sqrt(252) * portfolio_volatility

alpha_slippage_pct = ((expected_alpha - realized_alpha) / expected_alpha) * 100
```

### Rolling Window
- **30-day rolling window** (last 30 calendar days of trades)
- Minimum 100 trades required for statistical significance
- Update after every 50 new trades OR every 4 hours (whichever comes first)

### Edge Cases
| Condition | Behavior |
|-----------|----------|
| sample_size < min_sample_size | Status = HEALTHY, log "insufficient data" |
| File missing/corrupt | Status = HEALTHY, log warning, proceed with caution |
| last_updated > 24 hours old | Status = WARNING, treat as stale data |
| alpha_slippage_pct < 0 (improving) | Status = HEALTHY, model exceeding expectations |

---

## 4. C++ Integration (Step 1.6)

### Implementation Location
**File:** `include/AIConnectionMonitor.h` and `src/AIConnectionMonitor.cpp`

### New Enum (Add to AIConnectionMonitor.h)
```cpp
enum class ModelHealthStatus {
    HEALTHY,       // Alpha slippage < 20%
    WARNING,       // Alpha slippage 20-30%
    SOFT_LOCKED    // Alpha slippage >= 30%
};
```

### New Method Signature
```cpp
class AIConnectionMonitor {
public:
    // ... existing methods ...
    
    // Model health monitoring
    ModelHealthStatus CheckModelHealthStatus(SCStudyInterfaceRef sc);
    bool ShouldAcceptSignalWithModelHealth(
        float confidence, 
        ModelHealthStatus health
    ) const;
    
private:
    ModelHealthStatus m_lastModelHealth;
    SCDateTime m_lastHealthFileCheck;
    std::string m_healthFilePath;
};
```

### Read Logic (Pseudocode)
```cpp
ModelHealthStatus AIConnectionMonitor::CheckModelHealthStatus(SCStudyInterfaceRef sc) {
    // 1. Check cache (only read file every 60 seconds)
    if (sc.CurrentSystemDateTime - m_lastHealthFileCheck < 60) {
        return m_lastModelHealth;
    }
    
    // 2. Read model_health_status.json
    std::string json = ReadFile(m_healthFilePath);
    
    // 3. Parse JSON (use existing JSON parser or simple string parsing)
    std::string status = ExtractField(json, "status");
    float alphaSlippage = ExtractFloatField(json, "alpha_slippage_pct");
    int sampleSize = ExtractIntField(json, "sample_size");
    std::string lastUpdated = ExtractField(json, "last_updated");
    
    // 4. Validate freshness (<24 hours old)
    SCDateTime updateTime = ParseISO8601(lastUpdated);
    if (sc.CurrentSystemDateTime - updateTime > 86400) {
        LogWarning(sc, "Model health status file stale (>24h old)");
        return ModelHealthStatus::WARNING;
    }
    
    // 5. Convert string status to enum
    ModelHealthStatus health = ModelHealthStatus::HEALTHY;
    if (status == "WARNING") health = ModelHealthStatus::WARNING;
    else if (status == "SOFT_LOCKED") health = ModelHealthStatus::SOFT_LOCKED;
    
    // 6. Log status changes
    if (health != m_lastModelHealth) {
        if (health == ModelHealthStatus::SOFT_LOCKED) {
            LogCritical(sc, "🔒 Model Health: SOFT_LOCKED (Alpha: %.1f%%) - AI signals disabled", 
                        alphaSlippage);
        } else if (health == ModelHealthStatus::WARNING) {
            LogWarning(sc, "⚠️ Model Health: WARNING (Alpha: %.1f%%) - HIGH confidence only", 
                       alphaSlippage);
        } else {
            LogInfo(sc, "✅ Model Health: HEALTHY (Alpha: %.1f%%)", alphaSlippage);
        }
    }
    
    // 7. Update cache
    m_lastModelHealth = health;
    m_lastHealthFileCheck = sc.CurrentSystemDateTime;
    
    return health;
}
```

### Signal Acceptance Logic
```cpp
bool AIConnectionMonitor::ShouldAcceptSignalWithModelHealth(
    float confidence, 
    ModelHealthStatus health
) const {
    switch (health) {
        case ModelHealthStatus::HEALTHY:
            return confidence >= 0.55;  // Normal threshold
        
        case ModelHealthStatus::WARNING:
            return confidence >= 0.70;  // HIGH confidence only
        
        case ModelHealthStatus::SOFT_LOCKED:
            return false;  // Reject ALL AI signals
    }
}
```

### Integration into scsf_MindfulTrader
```cpp
// In SCStudies.cpp scsf_MindfulTrader function

// Check model health every bar (cached internally)
ModelHealthStatus modelHealth = g_aiMonitor.CheckModelHealthStatus(sc);

// When processing AI signal
if (!g_aiMonitor.ShouldAcceptSignalWithModelHealth(signal.confidence, modelHealth)) {
    sc.AddMessageToLog("AI signal rejected (model health)", 1);
    return;  // Skip signal
}

// If SOFT_LOCKED, cancel working orders (fast-purge)
if (modelHealth == ModelHealthStatus::SOFT_LOCKED && 
    lastModelHealth != ModelHealthStatus::SOFT_LOCKED) {
    // First time entering SOFT_LOCKED
    PositionManager& pm = PositionManager::Instance();
    pm.CancelAllWorkingOrders(sc);
    sc.AddMessageToLog("🔒 SOFT_LOCKED: Cancelled all working orders", 0);
}

// Track state changes
static ModelHealthStatus lastModelHealth = ModelHealthStatus::HEALTHY;
lastModelHealth = modelHealth;
```

---

## 5. Python Integration (Performance Attribution Engine)

### Module Overview
**File:** `performance_attribution_engine.py` (to be implemented in Phase 2)

**Responsibilities:**
1. Track all trades from C++ via ZMQ or database
2. Calculate 30-day rolling performance metrics
3. Compare realized vs expected alpha
4. Write `model_health_status.json` file
5. Trigger alerts when status degrades

### Minimal Implementation (Quick Start)
```python
import json
from datetime import datetime, timezone
from pathlib import Path

class ModelHealthMonitor:
    def __init__(self, data_dir: str):
        self.health_file = Path(data_dir) / "model_health_status.json"
        self.warning_threshold = 20.0
        self.soft_lock_threshold = 30.0
        
    def update_health_status(
        self,
        expected_sharpe: float,
        realized_sharpe: float,
        sample_size: int
    ):
        """Calculate alpha slippage and write status file."""
        
        # Calculate alpha slippage (simplified)
        expected_alpha = expected_sharpe * 15.87  # sqrt(252)
        realized_alpha = realized_sharpe * 15.87
        alpha_slippage_pct = ((expected_alpha - realized_alpha) / expected_alpha) * 100
        
        # Determine status
        if alpha_slippage_pct < self.warning_threshold:
            status = "HEALTHY"
        elif alpha_slippage_pct < self.soft_lock_threshold:
            status = "WARNING"
        else:
            status = "SOFT_LOCKED"
        
        # Write JSON file (atomic write)
        health_data = {
            "status": status,
            "alpha_slippage_pct": round(alpha_slippage_pct, 2),
            "sample_size": sample_size,
            "last_updated": datetime.now(timezone.utc).isoformat(),
            "metrics": {
                "expected_sharpe": round(expected_sharpe, 2),
                "realized_sharpe": round(realized_sharpe, 2)
            },
            "thresholds": {
                "warning_threshold_pct": self.warning_threshold,
                "soft_lock_threshold_pct": self.soft_lock_threshold,
                "min_sample_size": 100
            }
        }
        
        # Atomic write (write to temp, then rename)
        temp_file = self.health_file.with_suffix(".tmp")
        temp_file.write_text(json.dumps(health_data, indent=2))
        temp_file.replace(self.health_file)
        
        return status

# Usage
monitor = ModelHealthMonitor("/path/to/data")
status = monitor.update_health_status(
    expected_sharpe=1.85,
    realized_sharpe=1.62,
    sample_size=847
)
print(f"Model Health: {status}")
```

### Update Frequency
| Event | Action |
|-------|--------|
| Every 50 new trades | Recalculate and update file |
| Every 4 hours | Recalculate and update file (even if <50 trades) |
| Status change detected | **Immediate write** (do not wait for interval) |
| SOFT_LOCKED triggered | **Immediate write + ALERT** |

---

## 6. Testing Strategy

### Test Case 1: HEALTHY State (Stub JSON)
**File:** `data/model_health_status_healthy.json`
```json
{
  "status": "HEALTHY",
  "alpha_slippage_pct": 8.3,
  "sample_size": 450,
  "last_updated": "2025-12-20T14:30:00Z"
}
```

**Expected C++ Behavior:**
- Log: `✅ Model Health: HEALTHY (Alpha: 8.3%)`
- Accept signals with confidence >= 0.55
- Normal position sizing

---

### Test Case 2: WARNING State (Stub JSON)
**File:** `data/model_health_status_warning.json`
```json
{
  "status": "WARNING",
  "alpha_slippage_pct": 24.7,
  "sample_size": 320,
  "last_updated": "2025-12-20T14:30:00Z"
}
```

**Expected C++ Behavior:**
- Log: `⚠️ Model Health: WARNING (Alpha: 24.7%) - HIGH confidence only`
- Accept signals with confidence >= 0.70 only
- Position sizing reduced by 50%

---

### Test Case 3: SOFT_LOCKED State (Stub JSON)
**File:** `data/model_health_status_locked.json`
```json
{
  "status": "SOFT_LOCKED",
  "alpha_slippage_pct": 35.2,
  "sample_size": 280,
  "last_updated": "2025-12-20T14:30:00Z"
}
```

**Expected C++ Behavior:**
- Log: `🔒 Model Health: SOFT_LOCKED (Alpha: 35.2%) - ALL AI signals rejected`
- Reject ALL AI signals regardless of confidence
- Cancel all working orders (fast-purge)
- Manual trading still allowed

---

### Test Case 4: Missing/Corrupt File
**Action:** Delete or corrupt `model_health_status.json`

**Expected C++ Behavior:**
- Log: `⚠️ Model health file missing or corrupt - proceeding with caution`
- Default to HEALTHY state (fail-safe)
- Log warning every 5 minutes until file restored

---

### Test Case 5: Stale File (>24h old)
**File:** Update `last_updated` to 48 hours ago

**Expected C++ Behavior:**
- Log: `⚠️ Model health status file stale (>24h old) - treating as WARNING`
- Force WARNING state regardless of status field
- Log warning every 60 minutes

---

### Test Case 6: State Transition Testing
**Action:** Cycle through stub files every 60 seconds

**Expected C++ Behavior:**
- Log each state transition
- Cancel orders when entering SOFT_LOCKED
- Adjust confidence thresholds when entering WARNING
- Resume normal operation when returning to HEALTHY

---

## 7. Troubleshooting

### Issue: C++ not reading updated file
**Symptoms:** Status stuck on old value after Python updates file

**Diagnosis:**
```cpp
// Check cache timeout (should be 60 seconds)
if (sc.CurrentSystemDateTime - m_lastHealthFileCheck < 60) {
    // Still using cached value
}
```

**Solution:** Wait 60 seconds or reduce cache timeout during testing

---

### Issue: Python writes but C++ reads partial JSON
**Symptoms:** JSON parsing fails intermittently

**Root Cause:** Python writing directly to file (non-atomic)

**Solution:** Use atomic write pattern:
```python
# Write to temp file first
temp_file = Path("model_health_status.tmp")
temp_file.write_text(json.dumps(data))

# Atomic rename (POSIX guarantee)
temp_file.replace("model_health_status.json")
```

---

### Issue: False SOFT_LOCKED during low sample size
**Symptoms:** Status oscillates during startup with <100 trades

**Solution:** Python should not write status until `sample_size >= min_sample_size`
```python
if sample_size < 100:
    # Do not write file yet
    return
```

---

### Issue: File permissions prevent C++ read
**Symptoms:** C++ logs "Cannot open model_health_status.json"

**Solution:**
```bash
chmod 644 model_health_status.json
chown trader:trader model_health_status.json
```

---

## 8. Configuration

### File Path Configuration
**Option 1: ConfigManager (Recommended)**
```cpp
// In config.json
{
  "model_health_file": "/home/trader/data/model_health_status.json"
}

// In C++ initialization
ConfigManager& config = ConfigManager::Instance();
m_healthFilePath = config.GetString("model_health_file", 
                                     "data/model_health_status.json");
```

**Option 2: Hardcoded Path**
```cpp
// Simple approach for testing
m_healthFilePath = "/home/trader/data/model_health_status.json";
```

### Thresholds Configuration
Allow users to customize thresholds via config:
```json
{
  "model_health": {
    "warning_threshold_pct": 20.0,
    "soft_lock_threshold_pct": 30.0,
    "min_sample_size": 100,
    "cache_timeout_seconds": 60
  }
}
```

---

## 9. Success Criteria

**Before considering Step 1.6 complete:**

✅ **Implementation:**
- [ ] `CheckModelHealthStatus()` implemented in AIConnectionMonitor
- [ ] `ShouldAcceptSignalWithModelHealth()` implemented
- [ ] Integrated into scsf_MindfulTrader study
- [ ] Fast-purge triggered on SOFT_LOCKED entry
- [ ] State transitions logged with emoji indicators

✅ **Testing:**
- [ ] All 6 test cases passed (HEALTHY, WARNING, SOFT_LOCKED, missing, stale, transitions)
- [ ] File caching working (60-second timeout)
- [ ] Confidence thresholds adjusted correctly in WARNING state
- [ ] All AI signals rejected in SOFT_LOCKED state

✅ **Integration:**
- [ ] Compiles without errors
- [ ] No false positives during normal operation
- [ ] Clean shutdown when file missing/corrupt
- [ ] Runs for 3 consecutive days without issues

---

## 10. Future Enhancements (Phase 2+)

**Phase 2: Python Performance Attribution Engine**
- Full 30-day rolling window implementation
- Sharpe ratio, win rate, MAE slippage tracking
- Email/Slack notifications on status changes
- Daily performance reports

**Phase 3: Model Retraining Triggers**
- Automatic model retraining when SOFT_LOCKED for >7 days
- A/B testing framework for comparing old vs new model
- Gradual rollout (10% → 50% → 100% of signals)

**Phase 4: Multi-Model Health Tracking**
- Separate health status for each model (ES, NQ, YM, etc.)
- Aggregate health score across all models
- Per-model soft locks (lock ES but allow NQ if ES degraded)

---

## Appendix: JSON Schema (Formal Specification)

```json
{
  "$schema": "http://json-schema.org/draft-07/schema#",
  "type": "object",
  "required": ["status", "alpha_slippage_pct", "sample_size", "last_updated"],
  "properties": {
    "status": {
      "type": "string",
      "enum": ["HEALTHY", "WARNING", "SOFT_LOCKED"]
    },
    "alpha_slippage_pct": {
      "type": "number",
      "minimum": -100,
      "maximum": 100
    },
    "sample_size": {
      "type": "integer",
      "minimum": 0
    },
    "last_updated": {
      "type": "string",
      "format": "date-time"
    },
    "metrics": {
      "type": "object",
      "properties": {
        "expected_sharpe": {"type": "number"},
        "realized_sharpe": {"type": "number"},
        "expected_winrate": {"type": "number", "minimum": 0, "maximum": 1},
        "realized_winrate": {"type": "number", "minimum": 0, "maximum": 1},
        "avg_mae_slippage_ticks": {"type": "number"}
      }
    },
    "thresholds": {
      "type": "object",
      "properties": {
        "warning_threshold_pct": {"type": "number"},
        "soft_lock_threshold_pct": {"type": "number"},
        "min_sample_size": {"type": "integer"}
      }
    }
  }
}
```

---

**Document Version:** 1.0  
**Last Updated:** December 20, 2025  
**Next Review:** After Step 1.6 C++ implementation complete
