# C++ Implementation Checklist - Elite Messaging Protocol

**Start Date:** December 22, 2025  
**Target:** Parallel implementation with GUI team  
**Reference:** ELITE_MESSAGING_PROTOCOL_SPEC.md

---

## Priority 1: Port 5560 - Master Controller (Days 1-2)

### Files to Modify

#### 1. `include/SystemOrchestrator.h`

**Add to class definition:**
```cpp
// State machine
enum class SystemState {
    UNINITIALIZED,
    WAITING_FOR_AI,
    NEGOTIATING,
    INITIALIZING,
    VALIDATION,
    READY,
    ACTIVE_TRADING,
    DEGRADED,
    DISCONNECTED
};

private:
    // State management
    std::atomic<SystemState> m_currentState{SystemState::UNINITIALIZED};
    
    // REP server (port 5560)
    zmq::context_t m_context{1};
    zmq::socket_t m_repSocket{m_context, ZMQ_REP};
    std::thread m_repWorker;
    std::atomic<bool> m_stopRepWorker{false};
    
    // Version management
    struct Version {
        int major{1};
        int minor{0};
        int patch{2};
    };
    Version m_version;
    
    // Capabilities
    std::vector<std::string> m_capabilities{
        "port_5555_indicators",
        "port_5556_trades", 
        "port_5557_transformer",
        "port_5558_validation",
        "port_5559_heartbeat"
    };
    
    // Methods
    void RepWorkerFunction();
    nlohmann::json HandleConfigRequest(const nlohmann::json& request);
    nlohmann::json HandleStateQuery(const nlohmann::json& request);
    nlohmann::json CreateErrorResponse(const std::string& code, const std::string& msg);
    bool IsVersionCompatible(int major, int minor, int patch);
    int64_t GetNanosecondTimestamp() const;
    std::string StateToString(SystemState state) const;

public:
    bool IsReadyForTrading() const;
    SystemState GetCurrentState() const;
    void SetState(SystemState newState);
```

**📋 Checklist:**
- [ ] Add SystemState enum
- [ ] Add private members (socket, thread, state)
- [ ] Add version struct
- [ ] Add capabilities vector
- [ ] Add method declarations
- [ ] Add public accessors

---

#### 2. `src/SystemOrchestrator.cpp`

**Add to constructor:**
```cpp
SystemOrchestrator::SystemOrchestrator() {
    m_currentState.store(SystemState::UNINITIALIZED);
    
    // Start REP server
    m_repWorker = std::thread(&SystemOrchestrator::RepWorkerFunction, this);
    
    Logger::getInstance().log("SystemOrchestrator: REP server starting on port 5560");
}
```

**Add to destructor:**
```cpp
SystemOrchestrator::~SystemOrchestrator() {
    m_stopRepWorker.store(true);
    if (m_repWorker.joinable()) {
        m_repWorker.join();
    }
}
```

**Implement REP worker (copy from spec lines 1320-1450):**
```cpp
void SystemOrchestrator::RepWorkerFunction() {
    // See ELITE_MESSAGING_PROTOCOL_SPEC.md Section 5.5
    // Full implementation provided
}

nlohmann::json SystemOrchestrator::HandleConfigRequest(const nlohmann::json& request) {
    // See spec lines 1450-1550
}

nlohmann::json SystemOrchestrator::HandleStateQuery(const nlohmann::json& request) {
    // See spec lines 1550-1600
}
```

**📋 Checklist:**
- [ ] Modify constructor to start REP server
- [ ] Modify destructor to stop REP server
- [ ] Implement `RepWorkerFunction()` (bind to port 5560, message loop)
- [ ] Implement `HandleConfigRequest()` (version check, capabilities)
- [ ] Implement `HandleStateQuery()` (return current state)
- [ ] Implement `CreateErrorResponse()` helper
- [ ] Implement `IsVersionCompatible()` (major.minor check)
- [ ] Implement `GetNanosecondTimestamp()` helper
- [ ] Implement `StateToString()` converter
- [ ] Implement `IsReadyForTrading()` (state == READY || ACTIVE_TRADING)
- [ ] Implement `GetCurrentState()` accessor
- [ ] Implement `SetState()` mutator

**Test:**
```bash
# Python test client (create test_port_5560.py)
python3 test_port_5560.py
# Should see: "✅ CONFIG_ACK received, capabilities: [...]"
```

---

## Priority 2: Port 5555 - XPUB Upgrade (Days 2-3)

### Files to Modify

#### 1. `include/MindfulSocketZMQ.h`

**Change socket type:**
```cpp
// OLD: zmq::socket_t m_socket{m_context, ZMQ_PUB};
// NEW:
zmq::socket_t m_socket{m_context, ZMQ_XPUB};

// Add members
std::atomic<uint64_t> m_msgSequence{0};
std::atomic<int> m_subscriberCount{0};
std::chrono::steady_clock::time_point m_lastPublish;

// Add methods
nlohmann::json CreateEnvelope(
    const std::string& msg_type,
    const nlohmann::json& payload);
void MonitorSubscriptions();
int64_t GetNanosecondTimestamp() const;
```

**📋 Checklist:**
- [ ] Change ZMQ_PUB → ZMQ_XPUB
- [ ] Add m_msgSequence counter
- [ ] Add m_subscriberCount tracker
- [ ] Add m_lastPublish timestamp
- [ ] Add CreateEnvelope() method
- [ ] Add MonitorSubscriptions() method

---

#### 2. `src/MindfulSocketZMQ.cpp`

**Modify Init():**
```cpp
void MindfulSocketZMQ::Init() {
    try {
        m_socket.bind(ZMQ_ENDPOINT);
        
        // XPUB: Enable verbose mode to get SUBSCRIBE/UNSUBSCRIBE events
        int verbose = 1;
        m_socket.set(zmq::sockopt::xpub_verbose, verbose);
        
        // Set high water mark (aggressive message dropping)
        int hwm = 10;
        m_socket.set(zmq::sockopt::sndhwm, hwm);
        
        Logger::getInstance().log("✅ MindfulSocketZMQ (XPUB) bound to port 5555");
        
        // Start subscription monitor thread
        std::thread([this]() { MonitorSubscriptions(); }).detach();
        
    } catch (const zmq::error_t& e) {
        Logger::getInstance().log("🔴 MindfulSocketZMQ bind error: " + std::string(e.what()));
    }
}
```

**Implement MonitorSubscriptions() (spec lines 2080-2150):**
```cpp
void MindfulSocketZMQ::MonitorSubscriptions() {
    // Poll for SUBSCRIBE/UNSUBSCRIBE events
    // Update m_subscriberCount
    // See spec for full implementation
}
```

**Modify PublishIndicatorUpdate() (spec lines 2150-2250):**
```cpp
void MindfulSocketZMQ::PublishIndicatorUpdate(...) {
    // Check if subscribers > 0
    if (m_subscriberCount.load() == 0) {
        return;  // Don't publish if no subscribers
    }
    
    // Create typed envelope
    nlohmann::json envelope = CreateEnvelope("BAR_CLOSE_UPDATE", payload);
    
    // Add sequence number
    envelope["header"]["sequence_id"] = m_msgSequence.fetch_add(1);
    
    // Send with dontwait (drop if slow)
    std::string msg = envelope.dump();
    zmq::message_t zmq_msg(msg.begin(), msg.end());
    m_socket.send(zmq_msg, zmq::send_flags::dontwait);
    
    m_lastPublish = std::chrono::steady_clock::now();
}
```

**📋 Checklist:**
- [ ] Modify Init() to set XPUB_VERBOSE + HWM
- [ ] Start MonitorSubscriptions() thread
- [ ] Implement MonitorSubscriptions() (parse events, update counter)
- [ ] Implement CreateEnvelope() (header + payload + telemetry)
- [ ] Modify PublishIndicatorUpdate() (check subscribers, add sequence)
- [ ] Add 5-minute keep-alive timer
- [ ] Implement GetNanosecondTimestamp() helper

**Test:**
```bash
# Python test client (create test_port_5555.py)
python3 test_port_5555.py
# Should see: "✅ Subscribed, receiving indicators with sequence_id"
```

---

## Priority 3: State Machine Integration (Day 3)

### Files to Modify

**All sockets check state before operating:**

#### `src/TradeSocketZMQ.cpp`
```cpp
void TradeSocketZMQ::SendTradeRequest(...) {
    if (!SystemOrchestrator::Instance().IsReadyForTrading()) {
        Logger::getInstance().log("⚠️ Trade blocked: System not ready");
        return;
    }
    // ... existing logic
}
```

#### `src/SystemOrchestrator.cpp`
```cpp
bool SystemOrchestrator::PerformDiscoveryHandshake(int timeout_ms) {
    // CONFIG_REQ/CONFIG_ACK handshake is the canonical startup path.
    // Ensure requests are validated and only accepted in valid states.
}
```

**📋 Checklist:**
- [ ] Add state check to TradeSocketZMQ::SendTradeRequest()
- [ ] Add/verify strict state checks in SystemOrchestrator handshake path
- [ ] Add state check to TradeExecutionServer::WorkerFunction()
- [ ] Add state transitions on events (e.g., TRANSFORMER_READY → VALIDATION)

---

## Testing Strategy

### Test 1: Master Controller Handshake
```python
# test_port_5560.py
import zmq, json, time

context = zmq.Context()
socket = context.socket(zmq.REQ)
socket.connect("tcp://127.0.0.1:5560")

request = {
    "header": {
        "msg_type": "CONFIG_REQ",
        "version": "1.0.2",
        "timestamp_ns": time.time_ns(),
        "sender": "PYTHON_GUI"
    },
    "payload": {
        "component_name": "GUI_SUBSCRIBER",
        "capabilities": ["indicator_display", "manual_trade_entry"]
    }
}

socket.send_string(json.dumps(request))
response = json.loads(socket.recv_string())

print(f"✅ Handshake: {response['payload']['negotiation_status']}")
print(f"   Ports: {response['payload']['available_ports']}")
```

### Test 2: XPUB Subscriber
```python
# test_port_5555.py
import zmq, json

context = zmq.Context()
socket = context.socket(zmq.SUB)
socket.connect("tcp://127.0.0.1:5555")
socket.subscribe("")  # Subscribe to all

print("Listening for indicators...")
for i in range(10):
    msg = socket.recv_string()
    data = json.loads(msg)
    print(f"Seq {data['header']['sequence_id']}: {data['payload']['LONG_MACD']}")
```

---

## Success Criteria

- [ ] Port 5560 accepts CONFIG_REQ, returns CONFIG_ACK
- [ ] Port 5555 detects GUI subscribe/unsubscribe
- [ ] Port 5555 includes sequence_id in every message
- [ ] State machine blocks trades when not READY
- [ ] Version mismatch rejected with clear error
- [ ] GUI can run test scripts successfully

---

## Next Steps (After Priority 1-2 Complete)

- Port 5556: TradeSocketZMQ (Lazy Pirate pattern)
- Port 5557: Legacy transformer init endpoint (reserved/compat)
- Port 5558: TradeExecutionServer (Validation/Execution)
- Port 5559: AIHeartbeatMonitor (Zombie Detection)

**Each port is independent after SystemOrchestrator is ready.**

---

## Reference

- **Full Spec:** `docs/ELITE_MESSAGING_PROTOCOL_SPEC.md`
- **Port 5560:** Lines 1069-1885
- **Port 5555:** Lines 1887-2512
- **Message Schemas:** Section 4.5 (lines 398-822)
