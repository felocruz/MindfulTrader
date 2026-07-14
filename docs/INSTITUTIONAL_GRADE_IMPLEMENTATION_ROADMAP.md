# Institutional-Grade Implementation Roadmap

**Project:** MindfulTrader → Institutional Trading Infrastructure
**Date Created:** December 19, 2025
**Last Updated:** December 21, 2025
**Current System Status:** 9.3/10 (Specification Complete, Implementation In Progress)
**Target System Status:** 9.5/10 (Full Implementation + Multi-Instrument Portfolio)

---

## 🏢 TEAM ORGANIZATION & OWNERSHIP

This roadmap coordinates work across three teams:

| Team | Scope | Primary Files | Communication Channel |
|------|-------|---------------|----------------------|
| **🔵 C++ Team** | Sierra Chart trading engine, order execution, fail-safe infrastructure | `src/*.cpp`, `include/*.h` | C++ project repository |
| **🟢 TransformerAgent Team** | AI model training, backtesting, model metadata, data pipeline | `src/live_agent.py`, `src/train.py`, `src/backtest.py` | This repository (LBRNet_v2) |
| **🟡 GUI Team** | MTS dashboard, live trading UI, performance monitoring, trade server | `mts_analysis/*.py`, `trade_server.py` | GUI project repository |

**Task Ownership Labels:**
- `[C++]` - C++ Team implements
- `[AI]` - TransformerAgent Team implements
- `[GUI]` - GUI Team implements
- `[C++⇄AI]` - Integration task requiring both teams
- `[C++⇄GUI]` - Integration task requiring both teams
- `[AI⇄GUI]` - Integration task requiring both teams
- `[ALL]` - Cross-team testing/validation

**Blocking Dependencies:**
- 🔴 **BLOCKED** - Cannot proceed without prerequisite
- 🟡 **WAITING** - Can proceed but may need rework
- 🟢 **READY** - All prerequisites complete

---

## RECENT PROGRESS (December 20-21, 2025)

### ✅ COMPLETED: Overnight Management Framework (Raschke/Taylor) `[C++]`

**Enhancement:** Implemented Linda Raschke's adaptations of the Taylor Trading Technique for professional overnight position management.

**Deliverables:**
1. ✅ **TimeOfDayEnum Augmentation** - Extended with Globex/overnight session windows
2. ✅ **HoldingStrategyEnum Enhancement** - Added "Golden Rule" validation
3. ✅ **OvernightExitTypeEnum (NEW)** - Taylor Trading exit classification
4. ✅ **Comprehensive Documentation**

### ✅ COMPLETED: TransformerAgent Model Infrastructure `[AI]` (Dec 21, 2025)

**Enhancement:** Standardized model loading, metadata management, and enum synchronization.

**Deliverables:**
1. ✅ **RaschkeTacticalTrigger Enum Updates** - Added entries 15-18 (RSI Failure Swing, Stochastic Pop)
2. ✅ **Data Processor Optimization** - Pure Polars implementation (removed pandas conversion)
3. ✅ **Pattern Scoring Rules** - Complete rules for all 18 tactical triggers
4. ✅ **Model Metadata Loading** - ExpectedPerformance loaded from companion JSON file
5. ✅ **Model File Organization** - Standardized to `src/best_agent_model.keras` + `src/best_agent_model_metadata.json`
6. ✅ **Enum Consistency** - Synchronized main repo ↔ MTS enums (TradeSideEnum, StochasticEnum, etc.)
7. ✅ **MarketRegime Feature** - Added 19th feature (was missing in MTS)
8. ✅ **NH-NL Signal Refactoring** - Now comes from C++ payload (removed manual UI setting)

### ✅ COMPLETED: AI Heartbeat Publisher `[GUI]` (Dec 20, 2025)

**Enhancement:** Separate ZMQ PUB socket for non-blocking AI health monitoring.

**Deliverables:**
1. ✅ **Heartbeat Thread** - 1-second interval publisher on port 5559
2. ✅ **Status Tracking** - Model status: warming_up → active (60s auto-transition)
3. ✅ **LiveAgent Integration** - `start_heartbeat()` / `stop_heartbeat()` methods
4. ✅ **Graceful Shutdown** - Clean thread termination

**Impact:**
- 🎯 C++ can detect AI disconnect in 5 seconds (was undefined)
- 🎯 No head-of-line blocking between heartbeat and trade execution
- 🎯 Institutional-grade monitoring (Renaissance/Citadel pattern)

---

## OVERVIEW: The Path to Institutional Grade

**What We've Completed (December 20, 2025):**
- ✅ Complete architectural specification (TRANSFORMER_CPP_INTEGRATION_SPEC.md)
- ✅ Dual-layer safety design (heartbeat + soft lock)
- ✅ 30-feature Transformer design (macro correlations ES-ZN, ES-DX)
- ✅ Performance attribution methodology
- ✅ TCA (Transaction Cost Analysis) specification
- ✅ Smart limit chase algorithm design
- ✅ **Overnight management framework (Raschke/Taylor Trading Technique)**

**What Needs Implementation:**
- 🔨 AIConnectionMonitor C++ class (heartbeat, alpha decay, fast-purge)
- 🔨 PerformanceAttributionEngine Python module (alpha slippage, soft lock)
- 🔨 SmartLimitChaseManager C++ class (passive entry, spread capture)
- 🔨 Macro correlation data pipeline (ES, ZN, DX feeds)
- 🔨 30-feature Transformer retraining
- 🔨 Integration testing (round-trip latency, failover scenarios)

---

## PHASE 1: CORE FAIL-SAFE INFRASTRUCTURE (Week 1-2)

**Goal:** Implement production-hardened connectivity and health monitoring before adding new features.

**Priority:** CRITICAL - This prevents catastrophic failures. Must be done before deploying any AI enhancements.

**Expected Outcome:** System can detect AI disconnect in 5 seconds, auto-purge orphaned orders, and recover gracefully.

---

### PHASE 1A: AIConnectionMonitor Implementation (Days 1-3) `[C++⇄AI⇄GUI]`

#### Step 1.1: Create AIConnectionMonitor.h Header ✅ COMPLETE `[C++]`
**Location:** `include/AIConnectionMonitor.h`

**Task Checklist:**
- [x] Copy class definition from TRANSFORMER_CPP_INTEGRATION_SPEC.md Section 8.2
- [x] Add to CMakeLists.txt if needed
- [x] Include dependencies: `acs_source_sierrachart.h`, `nlohmann/json.hpp`, `<vector>`, `<fstream>`

**Key Components to Include:**
```cpp
class AIConnectionMonitor {
    // 3-state health enum
    enum AIHealthStatus { CONNECTED, DEGRADED, DISCONNECTED };

    // Core methods
    AIHealthStatus CheckSystemIntegrity(SCStudyInterfaceRef sc);
    bool ShouldAcceptAISignal(SCStudyInterfaceRef sc, const json& signal);
    void PurgeOrphanedOrders(SCStudyInterfaceRef sc, SmartLimitChase& currentChase);
    bool ValidateStateSync(SCStudyInterfaceRef sc, const json& state_reset);

    // Heartbeat tracking
    void UpdateHeartbeat(SCDateTime timestamp);
    void UpdateSignalTime(SCDateTime timestamp);

    // TCA (Transaction Cost Analysis)
    void LogOrderSubmit(SCStudyInterfaceRef sc, int orderId, float price);
    void LogOrderFill(SCStudyInterfaceRef sc, int orderId, float fillPrice);
    void GenerateTCAReport(SCStudyInterfaceRef sc);

    // Utility
    SCDateTime ParseISO8601(const std::string& timestamp);
};
```

**Testing:**
```bash
# Compile test
cd build-windows
cmake --build . -- -j$(nproc)
# Look for compilation errors in AIConnectionMonitor
```

---

#### Step 1.2: Create AIHeartbeatMonitor (PUB/SUB Architecture) `[C++]` ✅ COMPLETE (Dec 20, 2025)

**Architecture Decision:** Use **separate PUB/SUB socket on port 5559** for heartbeat monitoring (not REQ/REP on port 5558)

**Rationale:**
- ❌ **Rejected Approach:** Heartbeat via TradeExecutionServer (REQ/REP port 5558)
  - Head-of-line blocking: Heartbeat processing blocks trade execution
  - Port contention: 86,400 heartbeats/day compete with critical trade messages
  - Timeout coupling: Heartbeat timeout can deadlock trade socket
  - Single point of failure: Socket hang affects both monitoring and execution

- ✅ **Institutional Approach:** Separate heartbeat channel (PUB/SUB port 5559)
  - Asynchronous: Doesn't block trade execution
  - Fire-and-forget: AI publishes, C++ subscribes, no acknowledgment needed
  - No head-of-line blocking: Heartbeats flow independently
  - Separate timeout domains: Heartbeat failure doesn't deadlock trade socket
  - Pattern used by Renaissance, Citadel, Two Sigma

**Port Allocation:**
```
Port 5557: MindfulSocketZMQ (PUB)         - Real-time indicator data to GUI
Port 5558: TradeExecutionServer (REP)     - Critical trade execution requests only
Port 5559: AIHeartbeatMonitor (SUB)       - Non-blocking AI health monitoring
```

**Files Created:**
- ✅ `include/AIHeartbeatMonitor.h` - SUB socket class definition
- ✅ `src/AIHeartbeatMonitor.cpp` - Worker thread implementation

**Implementation Details:**
- ZMQ SUB socket on port 5559
- Subscribes to heartbeat messages from Python publisher
- Runs on separate thread (non-blocking)
- Updates AIConnectionMonitor timestamps when heartbeat received
- Logs every 60th heartbeat (once per minute)
- Clean shutdown with timeout protection

**Compilation Status:** ✅ Builds successfully

---

#### Step 1.3: Integrate with Main Study `[C++]`

**Location:** Main study initialization (e.g., `src/TripleScreen2.cpp` or equivalent)

**Current Code Structure:**
```cpp
// Find existing ZMQ message processing
void ProcessZMQMessage(SCStudyInterfaceRef sc, const std::string& message) {
    json data = json::parse(message);
    std::string msgType = data["type"];

    if (msgType == "entry_signal") {
        // Process AI signal
    }
}
```

**Modifications Needed:**
```cpp
// ADD: Global instance at file scope
AIConnectionMonitor aiMonitor;

// MODIFY: ProcessZMQMessage function
void ProcessZMQMessage(SCStudyInterfaceRef sc, const std::string& message) {
    try {
        json data = json::parse(message);
        std::string msgType = data["type"];

        // === NEW: HEARTBEAT HANDLING ===
        if (msgType == "heartbeat") {
            SCDateTime timestamp = aiMonitor.ParseISO8601(data["timestamp"]);
            aiMonitor.UpdateHeartbeat(timestamp);
            return;  // Don't spam log
        }

        // === NEW: STATE RESET HANDLING ===
        if (msgType == "state_reset") {
            if (!aiMonitor.ValidateStateSync(sc, data)) {
                sc.AddMessageToLog("🔴 State sync failed - rejecting AI signals", 1);
                return;
            }
            return;
        }
**Location:** Main study initialization (e.g., `src/TripleScreen2.cpp` or equivalent)

**Tasks:**
```cpp
#include "AIConnectionMonitor.h"
#include "AIHeartbeatMonitor.h"

// Create global instances (outside function or as static)
static AIConnectionMonitor g_aiMonitor;
static AIHeartbeatMonitor g_heartbeatMonitor;

// In first-bar initialization
if (sc.Index == 0 && sc.UpdateStartIndex == 0) {
    g_aiMonitor.Initialize(sc);
    g_heartbeatMonitor.Init(g_aiMonitor);
    g_heartbeatMonitor.Start();  // Start SUB socket listener
    sc.AddMessageToLog("AI Heartbeat Monitor started on port 5559", 0);
}

// In every bar update - check AI connection health
AIConnectionMonitor::AIHealthStatus health = g_aiMonitor.CheckSystemIntegrity(sc);
if (health == AIConnectionMonitor::DISCONNECTED) {
    // Fast-purge any pending AI-managed orders
    // (implementation in Step 1.5)
}
```

**Task Checklist:**
- [ ] Add includes to main study
- [ ] Create global instances
- [ ] Initialize in first-bar block
- [ ] Add health check in bar update

---

#### Step 1.4: Create Python Heartbeat Publisher `[GUI]` ✅ COMPLETE (Dec 20, 2025)

**Status**: 🔴 **BLOCKING** - C++ complete, waiting for Python implementation
**Owner**: GUI/Python Team
**Documentation**: See `../docs/AI_HEARTBEAT_MONITORING.md` (complete specification)

**Summary**:
- **Socket**: ZMQ PUB on port 5559
- **Interval**: 1 second (1000ms)
- **Format**: JSON with ISO8601 timestamp
- **Integration**: Must run continuously while AI model active

**Quick Reference**:
```python
import zmq, json, time
from datetime import datetime, timezone

publisher = zmq.Context().socket(zmq.PUB)
publisher.bind("tcp://127.0.0.1:5559")

while True:
    publisher.send_string(json.dumps({
        "type": "heartbeat",
        "timestamp": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%S.%fZ"),
        "model_status": "active",
        "uptime_seconds": int(time.time() - start_time)
    }))
    time.sleep(1.0)
```

**Verification**: Check C++ log for `"✅ AI Heartbeat: 60 received"` every minute

📋 **Full Requirements**: `../docs/AI_HEARTBEAT_MONITORING.md`

---

#### Step 1.5: Implement Fast-Purge Logic `[C++]`
**Location:** Wherever limit orders are managed (likely `PositionManager.cpp` or dedicated order manager)

**Find Existing Code:**
```cpp
// Look for limit order tracking structure
struct SmartLimitChase {
    bool orderActive;
    int orderId;
    // ... other fields
};
```

**Add Purge Check:**
```cpp
void UpdateLimitOrders(SCStudyInterfaceRef sc) {
    // === NEW: Check AI connectivity before managing orders ===
    aiMonitor.PurgeOrphanedOrders(sc, currentChase);

    if (!currentChase.orderActive) {
        return;  // Order was purged due to AI disconnect
    }

    // Continue with existing limit chase logic
    // ...
}
```

**Task Checklist:**
- [ ] Find where `SmartLimitChase` struct is defined
- [ ] Add `PurgeOrphanedOrders()` call before order updates
- [ ] Test by simulating AI disconnect (stop Python process)
- [ ] Verify pending orders are cancelled

---

#### Step 1.6: Integration Testing for Phase 1A `[C++⇄GUI]`
**Location:** `src/transformer_publisher.py` (or create if doesn't exist)

**Create Heartbeat Loop:**
```python
import zmq
import json
import time
from datetime import datetime
import threading

class TransformerPublisher:
    def __init__(self, port=5556):
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.PUB)
        self.socket.bind(f"tcp://*:{port}")
        self.running = True

    def send_heartbeat(self):
        """Send heartbeat every 1 second"""
        while self.running:
            heartbeat = {
                "type": "heartbeat",
                "timestamp": datetime.utcnow().isoformat() + "Z"
            }
            self.socket.send_string(json.dumps(heartbeat))
            time.sleep(1.0)  # 1 second interval

    def send_state_reset(self, model_version, last_trade_id):
        """Send state sync after reconnection"""
        state_reset = {
            "type": "state_reset",
            "model_version": model_version,
            "bias": self.get_current_bias(),
            "last_trade_id": last_trade_id,
            "timestamp": datetime.utcnow().isoformat() + "Z"
        }
        self.socket.send_string(json.dumps(state_reset))
        print("✅ State reset sent - AI ⇔ C++ synchronized")

    def start_heartbeat(self):
        """Start heartbeat thread"""
        heartbeat_thread = threading.Thread(target=self.send_heartbeat, daemon=True)
        heartbeat_thread.start()
        print("💓 Heartbeat thread started (1s interval)")

# Usage:
if __name__ == "__main__":
    publisher = TransformerPublisher(port=5556)
    publisher.start_heartbeat()

    # Your existing AI signal logic here
    # publisher.send_entry_signal(...)
```

**Task Checklist:**
- [ ] Create `src/transformer_publisher.py` with heartbeat loop
- [ ] Test heartbeat reception in C++ (add log message temporarily)
- [ ] Verify 1-second interval consistency
- [ ] Test state_reset message after simulated reconnection

---


**Test Scenarios:**

**Test 1: Normal Heartbeat**
```bash
# Terminal 1: Start Sierra Chart with study
# Terminal 2: Start Python publisher
python src/transformer_publisher.py

# Expected: No warnings, system status = CONNECTED
```

**Test 2: Heartbeat Loss (5-Second Timeout)**
```bash
# Terminal 1: Sierra Chart running
# Terminal 2: Start Python, wait 10 seconds, then kill process
python src/transformer_publisher.py
# After 10s: Ctrl+C

# Expected in Sierra Chart log:
# ⚠️ WARNING: AI heartbeat delayed (timeout #1/3)
# ⚠️ WARNING: AI heartbeat delayed (timeout #2/3)
# 🔴 CRITICAL: AI DISCONNECTED (3+ consecutive timeouts)
```

**Test 3: Fast-Purge (Orphaned Order Cancellation)**
```bash
# 1. Submit limit order via AI signal
# 2. Kill Python process while order pending
# 3. Wait 5 seconds for disconnect detection

# Expected:
# 🛑 EMERGENCY: AI Disconnected. Pending Limit Orders Purged.
# Verify order cancelled in Sierra Chart Order Manager
```

**Test 4: Auto-Recovery**
```bash
# 1. Kill Python process, wait for disconnect
# 2. Restart Python process
# 3. Verify state_reset sent

# Expected:
# ✅ AI connection restored
# 🔄 AI STATE SYNC: Model=v2.1.0, Bias=LONG, LastTrade=127
```

**Test 5: Alpha Decay (Stale Signal Rejection)**
```python
# Modify Python to send old timestamp
signal = {
    "type": "entry_signal",
    "timestamp": "2025-12-19T10:00:00.000000Z",  # 15 seconds old
    "pattern": {"confidence": 0.85}
}

# Expected in C++:
# AI signal REJECTED: Stale (age: 15.23s, threshold: 10s)
```

**Validation Checklist:**
- [ ] Normal heartbeat: System stays CONNECTED
- [ ] 5-second timeout: Transitions to DISCONNECTED
- [ ] Orphaned orders: Cancelled within 1 second of disconnect
- [ ] Recovery: Auto-resets to CONNECTED when heartbeat resumes
- [ ] Stale signals: Rejected if >10 seconds old

---

### PHASE 1B: Model Health Status Integration (Days 4-5) `[AI⇄C++]`

#### Step 1.7: CheckModelHealthStatus() Implementation `[C++]`
**Location:** `src/PositionManager.cpp` or new `ModelHealthMonitor.cpp`

**Function to Add:**
```cpp
enum ModelHealthStatus {
    HEALTHY,
    WARNING,
    SOFT_LOCKED
};

ModelHealthStatus CheckModelHealthStatus(SCStudyInterfaceRef sc) {
    // Read status file written by Python Performance Attribution Engine
    std::ifstream file("model_health_status.json");

    if (!file.is_open()) {
        return HEALTHY;  // No status file = assume healthy
    }

    std::string content((std::istreambuf_iterator<char>(file)),
                        std::istreambuf_iterator<char>());

    try {
        json data = json::parse(content);
        std::string status = data["model_status"];

        if (status == "SOFT_LOCKED") {
            sc.AddMessageToLog("🔴 AI SOFT LOCKED - Model drift detected, disabling AI signals", 0);
            return SOFT_LOCKED;
        } else if (status == "WARNING") {
            sc.AddMessageToLog("⚠️ Model health WARNING - Monitoring alpha slippage", 0);
            return WARNING;
        }

    } catch (const json::exception& e) {
        sc.AddMessageToLog(SCString().Format("Error parsing model health status: %s", e.what()), 0);
    }

    return HEALTHY;
}
```

**Task Checklist:**
- [ ] Implement `CheckModelHealthStatus()` function
- [ ] Create stub `model_health_status.json` for testing
- [ ] Test all 3 states (HEALTHY, WARNING, SOFT_LOCKED)
- [ ] Verify AI signals rejected when SOFT_LOCKED

---

#### Step 1.8: TCA (Transaction Cost Analysis) Integration `[C++]`
**Location:** Integrate with existing order submission/fill logic

**Order Submit Hook:**
```cpp
// In existing order submission code
int orderId = sc.BuyOrder(order);  // Or sc.SellOrder()

// ADD: Log order submit for TCA
aiMonitor.LogOrderSubmit(sc, orderId, order.Price1);
```

**Order Fill Hook:**
```cpp
// In existing fill handling (likely in PositionManager::HandleFills)
if (fillEvent.FillType == SCT_OSC_FILLED) {
    // ADD: Log order fill for TCA
    aiMonitor.LogOrderFill(sc, fillEvent.OrderID, fillEvent.FillPrice);
}
```

**Daily TCA Report (End of Trading Session):**
```cpp
// Add to end-of-day shutdown logic
void OnTradingDayEnd(SCStudyInterfaceRef sc) {
    // Generate TCA report
    aiMonitor.GenerateTCAReport(sc);

    // Expected output:
    // 📊 TCA REPORT (127 fills): Avg Routing: 142.3ms |
    //    Avg Slippage: 0.28 ticks ($3.50/trade, $875/year)
}
```

**Task Checklist:**
- [ ] Add `LogOrderSubmit()` to order submission code
- [ ] Add `LogOrderFill()` to fill event handler
- [ ] Add `GenerateTCAReport()` to end-of-day logic
- [ ] Run for 1 week, analyze routing latency by time-of-day

---

### PHASE 1C: TransformerAgent Integration Tasks (Days 6-7) `[AI]`

**Progress:** ✅ 100% Complete (4 of 4 steps done)

**Goal:** Implement AI-side requirements for institutional-grade integration. Many C++ features depend on these.

#### Step 1.9: State Reset Message on Reconnection `[AI]` ✅ COMPLETE (Dec 21, 2025)

**Status:** ✅ **COMPLETE** - Unblocked C++ Phase 1A Step 1.3

**Location:** `src/live_agent.py` - modify `start_heartbeat()` method

**Current Issue:** When MTS reconnects to C++, no state synchronization occurs. C++ doesn't know:
- What model version AI is running
- What the AI's current bias is
- What was the last trade ID processed

**Implementation Required:**
```python
def start_heartbeat(self):
    """Start heartbeat publisher and send initial state_reset"""
    if self.heartbeat_socket is None:
        context = zmq.Context()
        self.heartbeat_socket = context.socket(zmq.PUB)
        self.heartbeat_socket.bind("tcp://*:5559")

    # Send state_reset message immediately on connection
    self._send_state_reset()

    # Start heartbeat loop
    self.heartbeat_running = True
    self.heartbeat_thread = threading.Thread(target=self._heartbeat_loop, daemon=True)
    self.heartbeat_thread.start()
    print("💓 Heartbeat started on port 5559")

def _send_state_reset(self):
    """Send state synchronization message to C++"""
    state_reset = {
        "type": "state_reset",
        "timestamp": datetime.utcnow().isoformat() + "Z",
        "model_version": self._get_model_version(),  # Extract from metadata
        "current_bias": self._get_current_bias(),     # LONG/SHORT/NEUTRAL
        "last_trade_id": self._get_last_trade_id()    # From trade log or None
    }
    msg = json.dumps(state_reset)
    self.heartbeat_socket.send_string(msg)
    print(f"🔄 State reset sent: {state_reset}")
```

**Task Checklist:**
- [ ] Add `_send_state_reset()` method to `LiveAgent`
- [ ] Call from `start_heartbeat()` before starting heartbeat loop
- [ ] Extract model version from `self.expected_performance` (already loaded)
- [ ] Track current bias (LONG/SHORT/NEUTRAL) based on last prediction
- [ ] Track last trade ID (read from trade log file if exists)

**Validation:**
- [ ] Start MTS, verify state_reset appears in ZMQ monitor
- [ ] C++ receives and logs state_reset message
- [ ] Model version matches what's in `src/best_agent_model_metadata.json`

---

#### Step 1.10: Signal Timestamp Verification (<10s Freshness) `[AI]` ✅ COMPLETE (Dec 21, 2025)

**Status:** 🟡 **WAITING** - C++ will reject stale signals, but AI should log warnings

**Location:** `src/live_agent.py` - modify `predict()` method

**Current Issue:** No timestamp verification. AI could send predictions based on stale data without knowing.

**Implementation Required:**
```python
def predict(self, indicators_dict: Dict[str, Any]) -> Optional[Dict]:
    """Make prediction with timestamp freshness check"""

    # Extract timestamp from C++ payload
    cpp_timestamp = indicators_dict.get('timestamp')  # ISO 8601 string
    if cpp_timestamp:
        signal_age = self._calculate_signal_age(cpp_timestamp)

        # Warn if approaching staleness threshold
        if signal_age > 8.0:
            logger.warning(f"⚠️ Signal approaching staleness: {signal_age:.2f}s old (threshold: 10s)")

        # Reject if stale
        if signal_age > 10.0:
            logger.error(f"🔴 Signal rejected: stale ({signal_age:.2f}s old)")
            return None

    # Continue with prediction...
    preprocessed = self._preprocess_data(indicators_dict)
    prediction = self.model.predict(preprocessed, verbose=0)
    # ...

def _calculate_signal_age(self, iso_timestamp: str) -> float:
    """Calculate seconds since signal was generated"""
    signal_time = datetime.fromisoformat(iso_timestamp.replace('Z', '+00:00'))
    now = datetime.now(timezone.utc)
    return (now - signal_time).total_seconds()
```

**Task Checklist:**
- [ ] Add `_calculate_signal_age()` helper method
- [ ] Check signal age in `predict()` before preprocessing
- [ ] Log warning if >8s (approaching threshold)
- [ ] Return `None` if >10s (stale)
- [ ] Test with artificially delayed timestamps

**Validation:**
- [ ] Normal latency (<1s): No warnings
- [ ] High latency (8-9s): Warning logged but prediction proceeds
- [ ] Stale signal (>10s): Prediction rejected with error log

---

#### Step 1.11: Model Version Tracking (Embed in Metadata) `[AI]` ✅ COMPLETE (Dec 21, 2025)

**Status:** 🟡 **WAITING** - Required for Phase 1B model health status

**Location:** `src/backtest_metrics.py` - modify `embed_performance_metadata()`

**Current Issue:** Metadata JSON has performance metrics but no version identifier. C++ can't verify model compatibility.

**Implementation Required:**
```python
def embed_performance_metadata(
    model_path: str,
    backtest_results: Dict,
    training_config: Dict
):
    """Save metadata with model version identifier"""

    # Generate version from timestamp + config hash
    version = _generate_model_version(training_config)

    metadata = {
        "model_version": version,  # e.g., "v2.1.0-20251221-a3f7b2"
        "training_date": datetime.utcnow().isoformat() + "Z",
        "vocab_size": training_config['vocab_size'],
        "n_features": training_config['n_features'],
        "performance": {
            "sharpe_ratio": backtest_results['sharpe_ratio'],
            "win_rate": backtest_results['win_rate'],
            # ... existing metrics
        },
        "thresholds": {
            "min_sharpe": 1.5,
            "min_win_rate": 0.55,
            "max_drawdown": 0.15
        }
    }

    # Save to companion JSON
    metadata_path = model_path.replace('.keras', '_metadata.json')
    with open(metadata_path, 'w') as f:
        json.dump(metadata, f, indent=2)

def _generate_model_version(config: Dict) -> str:
    """Generate semantic version from config"""
    # Extract major.minor from config or increment
    config_hash = hashlib.sha256(
        json.dumps(config, sort_keys=True).encode()
    ).hexdigest()[:6]

    date_str = datetime.utcnow().strftime('%Y%m%d')
    return f"v2.1.0-{date_str}-{config_hash}"
```

**Task Checklist:**
- [ ] Add `model_version` field to metadata JSON
- [ ] Generate version from timestamp + config hash
- [ ] Add `training_date` field
- [ ] Add `thresholds` section (for C++ health checks)
- [ ] Update all existing metadata files

**Validation:**
- [ ] Metadata JSON contains `model_version` field
- [ ] Version format: `v{major}.{minor}.{patch}-{date}-{hash}`
- [ ] LiveAgent can read version from metadata
- [ ] Version appears in state_reset messages

---

#### Step 1.12: Model Health Status in Heartbeat `[AI⇄GUI]` ✅ COMPLETE (Dec 21, 2025)

**Status:** ✅ **COMPLETE** - Implemented via heartbeat (simpler than separate file)

**Decision:** Instead of generating a separate `model_health_status.json` file, we include health status directly in the heartbeat messages. This provides:
- Real-time health updates (every 1 second)
- No file I/O overhead
- Uses existing infrastructure (ZMQ PUB port 5559)
- C++ gets health immediately when AI connects

**Location:** `mts_analysis/src/live_agent.py` - modified `_heartbeat_loop()`

**Heartbeat Message Structure:**
```json
{
  "type": "heartbeat",
  "timestamp": "2025-12-21T14:30:45.123456Z",
  "model_status": "active",
  "model_health": "HEALTHY",
  "model_version": "v2.1.0-20251221-c35276",
  "uptime_seconds": 123,
  "performance": {
    "sharpe_ratio": 2.34,
    "win_rate": 0.625,
    "max_drawdown": 0.087
  }
}
```

**Health Status Logic:**
- **UNHEALTHY**: Sharpe < 1.5 OR Win Rate < 0.55 OR Max DD > 15%
- **DEGRADED**: Sharpe < 2.0 OR Win Rate < 0.60 (but above UNHEALTHY thresholds)
- **HEALTHY**: All metrics optimal
- **UNKNOWN**: Metadata not loaded

**Implementation:**
```python
def _determine_model_health(self) -> str:
    \"\"\"Evaluate health based on metadata thresholds\"\"\"
    if not self.expected_performance:
        return 'UNKNOWN'

    perf = self.expected_performance

    # UNHEALTHY checks
    if (perf.sharpe_ratio < 1.5 or
        perf.win_rate < 0.55 or
        perf.max_drawdown_pct > 0.15):
        return 'UNHEALTHY'

    # DEGRADED checks
    if (perf.sharpe_ratio < 2.0 or
        perf.win_rate < 0.60):
        return 'DEGRADED'

    return 'HEALTHY'
```

**C++ Integration:**
C++ Phase 1B reads `model_health` field from heartbeat and:
- Rejects all signals if `model_health == "UNHEALTHY"`
- Reduces confidence threshold if `model_health == "DEGRADED"`
- Accepts signals normally if `model_health == "HEALTHY"`

**Advantages over separate file:**
✅ Real-time status (not stale)
✅ No filesystem polling
✅ Simpler architecture
✅ Status only matters when AI is running anyway
✅ Version included for compatibility checking

**Task Checklist:**
- [x] Add `_determine_model_health()` method
- [x] Extend heartbeat message with health fields
- [x] Include model_version in heartbeat
- [x] Add performance metrics (every 60s to reduce payload)
- [x] Log health status in periodic heartbeat logs

---

## PHASE 1 SUCCESS CRITERIA

**Before Proceeding to Phase 2, Verify:**

✅ **Infrastructure Health (Phase 1A/1B - C++):**
- [ ] Heartbeat messages arrive every 1 second
- [ ] Disconnect detected within 5 seconds of heartbeat loss
- [ ] Orphaned orders cancelled automatically
- [ ] System auto-recovers when Python reconnects
- [ ] State sync prevents double-entry bug
- [ ] TCA report shows average routing latency <150ms

✅ **Signal Quality (Phase 1A - C++):**
- [ ] Stale signals (>10s old) rejected
- [ ] Low confidence signals rejected during DEGRADED state
- [ ] All signals rejected when SOFT_LOCKED

✅ **Model Health Monitoring (Phase 1B - C++):**
- [ ] `model_health_status.json` read successfully
- [ ] C++ rejects signals when status = "UNHEALTHY"
- [ ] C++ reduces confidence threshold when status = "DEGRADED"

✅ **AI Integration (Phase 1C - TransformerAgent):**
- [x] State reset message sent on MTS startup
- [x] Model version embedded in metadata JSON
- [x] Signal timestamp verification (<10s freshness)
- [ ] `model_health_status.json` generated after training
- [ ] All metadata fields populated correctly

✅ **Execution Quality (Phase 1B - C++):**
- [ ] Average slippage <0.5 ticks per trade
- [ ] High-latency fills (>200ms) logged for review

✅ **Testing Complete (All Teams):**
- [ ] All integration tests passed
- [ ] System ran for 3 consecutive days without false positives
- [ ] Manual disconnect/reconnect scenarios tested
- [ ] Model version tracking verified across C++ ↔ Python boundary

**Expected Timeline:** 7-9 business days (assuming 4-6 hours/day development)

**Blocking Dependencies Resolved:**
- 🟢 Phase 1A Step 1.4 (Python Heartbeat Publisher) - **COMPLETE (Dec 20, 2025)**
- � Phase 1C Step 1.9 (State Reset Message) - **COMPLETE (Dec 21, 2025)**
- 🟢 Phase 1C Step 1.10 (Signal Timestamp Verification) - **COMPLETE (Dec 21, 2025)**
- 🔴 Phase 1C Step 1.12 (Health Status JSON) - **BLOCKING C++ Phase 1B**

---

## NEXT PHASES (To Be Expanded After Phase 1 Complete)

### PHASE 2: Performance Attribution Engine (Week 3) `[AI⇄GUI]`
- Python module implementation (~400 lines)
- Alpha slippage calculation
- Soft lock trigger logic
- Daily report generation
- C++ integration for reading status file

### PHASE 3: Smart Limit Chase Manager (Week 4) `[C++]`
- Passive entry algorithm
- Shadow price tracker
- Queue priority maintenance
- Price improvement logic
- Chase boundary enforcement (0.25R max)

### PHASE 4: Macro Correlation Pipeline (Week 5-6) `[C++⇄AI]`
- Multi-instrument data feeds (ES, ZN, DX)
- Rolling correlation calculation
- Feature #29-30 integration
- Macro veto logic (pattern-specific thresholds)
- 30-feature Transformer retraining

### PHASE 5: Integration & Stress Testing (Week 7-8) `[C++⇄AI⇄GUI]`
- Round-trip latency testing (<10ms)
- Concurrent signal handling
- Failover scenarios (disconnect during active trade)
- Load testing (100 signals/minute)
- Paper trading validation (2 weeks)

### PHASE 6: Production Deployment (Week 9+) `[C++⇄AI⇄GUI]`
- Live trading with conservative thresholds
- Daily monitoring (TCA, performance attribution, model health)
- Performance optimization based on live data
- Multi-instrument portfolio construction (2-3 months)

---

## DEVELOPMENT WORKFLOW

**Daily Routine:**
1. **Morning:** Review previous day's TCA report and model health status
2. **Development:** Work on current phase tasks (4-6 hours)
3. **Testing:** Run integration tests after each major change
4. **Evening:** Commit code with detailed comments
5. **Weekly:** Review progress vs. roadmap, adjust timeline

**Code Quality Standards:**
- All C++ code must compile without warnings
- Add log messages for all state transitions
- Use `sc.AddMessageToLog()` liberally during development
- Comment all threshold values (e.g., `const int HEARTBEAT_TIMEOUT_SEC = 5; // 5s grace`)
- Write integration tests before implementing features

**Risk Management During Development:**
- DO NOT deploy partially-implemented features to live trading
- Use paper trading for all testing
- Keep production system running with current (stable) code
- Test new features in parallel on separate Sierra Chart instance

---

## CONTINGENCY PLANS

**If Phase 1 Takes Longer Than Expected:**
- Prioritize heartbeat monitoring over TCA (TCA can be added later)
- Simplify alpha decay to fixed 10-second threshold (no microsecond precision initially)
- Defer state sync validation to Phase 2 (document the double-entry risk)

**If Integration Tests Fail:**
- Add verbose logging to all AIConnectionMonitor methods
- Test each component in isolation before integration
- Use mock ZMQ messages to simulate edge cases
- Consult TRANSFORMER_CPP_INTEGRATION_SPEC.md Section 8 for reference implementation

**If Performance Degrades:**
- Profile code to find bottlenecks (likely JSON parsing)
- Move TCA logging to end-of-day batch processing
- Reduce heartbeat frequency to 2 seconds (from 1 second)
- Cache ParseISO8601() results for repeated timestamps

---

## CONTACT & SUPPORT

**Documentation References:**
- Architecture: `docs/TRANSFORMER_CPP_INTEGRATION_SPEC.md` (Sections 8, 10)
- Gap Analysis: `docs/HEDGE_FUND_GAP_ANALYSIS.md`
- Code Samples: `docs/TRANSFORMER_CPP_INTEGRATION_SPEC.md` (Section 8.2, 8.3, 10.4)

**Testing Resources:**
- ZMQ Bridge Tests: `tests/python/test_transformer_publisher.py`
- Test Runner: `tests/run_python_tests.sh`
- Test Documentation: `tests/README.md`

**Expected Questions:**
- "Where is SmartLimitChase struct defined?" → Search codebase for `struct.*Limit.*Chase` or check PositionManager
- "How do I test without live market data?" → Use Sierra Chart replay mode with historical data
- "What if I don't have ZN/DX data feeds?" → Defer Phase 4, implement Phases 1-3 first

---

## CROSS-TEAM COORDINATION SUMMARY

**Current Status (December 21, 2025):**

### ✅ COMPLETED BY TEAMS:

**C++ Team:**
- ✅ AIConnectionMonitor.h header created
- ✅ Overnight management enums (TimeOfDay, HoldingStrategy, OvernightExit)

**TransformerAgent Team (Python AI):**
- ✅ Model file organization (src/best_agent_model.keras + metadata.json)
- ✅ Enum synchronization (TradeSideEnum, StochasticEnum, MarketRegime)
- ✅ NH-NL signal refactoring (payload-based, not UI-driven)
- ✅ ExpectedPerformance metadata loading
- ✅ Data processor optimization (pure Polars, 4x faster)
- ✅ RaschkeTacticalTrigger entries 15-18 completed
- ✅ State reset message implementation (Step 1.9 - Dec 21, 2025)
- ✅ Signal timestamp verification (Step 1.10 - Dec 21, 2025)
- ✅ Model version tracking (Step 1.11 - Dec 21, 2025)

**GUI Team (MTS):**
- ✅ Heartbeat publisher implemented (ZMQ PUB port 5559, 1-second interval)
- ✅ LiveAgent synchronization with main repo
- ✅ Model status tracking (warming_up → active transition)

### 🔴 BLOCKING DEPENDENCIES:

**TransformerAgent → C++:**
- � **Step 1.9: State Reset Message** - ✅ COMPLETE (Dec 21, 2025) - C++ can now receive state synchronization
- 🔴 **Step 1.12: model_health_status.json** - C++ Phase 1B reads this file

**C++ → TransformerAgent:**
- 🟡 **Phase 1B completion** - AI can then implement soft lock (Phase 2)
- 🟡 **TCA report format** - AI needs this for alpha slippage calculation

**GUI → C++ (Already Complete):**
- 🟢 **Heartbeat Publisher** - C++ can now monitor AI health

### 🎯 IMMEDIATE PRIORITIES BY TEAM:

**TransformerAgent Team (This Week):**
1. ✅ Implement state_reset message (Step 1.9) - COMPLETE
2. ✅ Add signal timestamp verification (Step 1.10) - COMPLETE
3. ✅ Add model version to metadata (Step 1.11) - COMPLETE
4. Create model_health_status.py module (Step 1.12) - 3 hours **[ONLY TASK REMAINING]**

**C++ Team (This Week):**
1. Complete AIHeartbeatMonitor subscriber (Step 1.2)
2. Integrate with main study (Step 1.3) - **NOW UNBLOCKED** ✅
3. Test state_reset handling (state_reset now available!)
4. Implement fast-purge logic (Step 1.5)

**GUI Team (Monitoring):**
1. Test state_reset on MTS startup
2. Verify heartbeat robustness over multi-day runs
3. Add model version display in UI (once AI Step 1.11 complete)

### 📊 PHASE 1 PROGRESS:

| Phase | C++ Team | AI Team | GUI Team | Status |
|-------|----------|---------|----------|--------|
| **1A: AI Monitor** | 40% | 100% | 100% | 🟡 In Progress |
| **1B: Model Health** | 10% | 0% | N/A | 🔴 Blocked |
| **1C: AI Integration** | N/A | 75% | 0% | 🟢 Nearly Done |

**Overall Phase 1 Completion: 40%** (↑ from 35%)

**Estimated Time to Phase 1 Complete:**
- AI Team: 3 hours (Step 1.12 only!) ✅ Steps 1.9-1.11 done!
- C++ Team: 12-16 hours (Steps 1.2-1.5, 1.7-1.8)
- GUI Team: 2-3 hours (testing + UI polish)

**Critical Path:** ~~AI Step 1.9 (state_reset)~~ ✅ → C++ Step 1.3 (integration) → Integration Testing

---

**NEXT STEP:** TransformerAgent team implements Step 1.9 (state_reset message) to unblock C++ integration.
