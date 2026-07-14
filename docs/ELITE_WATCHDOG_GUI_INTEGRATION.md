# Elite Watchdog GUI Integration Guide

**Date:** December 22, 2025  
**Component:** SystemOrchestrator Port 5560 Master Controller  
**Status:** ✅ DEPLOYED - Elite institutional-grade watchdog active  
**Architecture:** Non-blocking async discovery with strike escalation

---

## Executive Summary: What Changed

The SystemOrchestrator now runs an **elite watchdog thread** that ensures:
- ✅ **Zero UI freezing** during connection attempts (1s poll timeout, not infinite wait)
- ✅ **Thread-safe discovery** (no SCStudyInterfaceRef in background threads)
- ✅ **Heartbeat liveness monitoring** (strike escalation on stale connections)
- ✅ **Graceful degradation** (3-strike system: WARN → DEGRADED → DISCONNECTED)

**CRITICAL FOR GUI:** The handshake is now **fully asynchronous**. Sierra Chart will not block waiting for you. You must connect proactively.

---

## Port 5560 Handshake Protocol (Updated)

### Step 1: GUI Initiates Connection

The GUI must send a `CONFIG_REQ` message to `172.20.112.1:5560` (or appropriate Windows host IP).

**Message Format:**
```json
{
  "header": {
    "msg_type": "CONFIG_REQ",
    "version": "1.0.2",
    "timestamp_ns": 1766445169249426778,
    "sender": "PYTHON_GUI_CLIENT"
  },
  "payload": {
    "component_name": "LIVE_AGENT_VIEW",
    "capabilities": [
      "indicator_display",
      "manual_trade_entry",
      "chart_visualization",
      "risk_monitoring",
      "trade_journaling"
    ]
  }
}
```

**Key Fields:**
- `msg_type`: Must be `"CONFIG_REQ"`
- `version`: Client version (semantic versioning: `major.minor.patch`)
  - **CRITICAL**: Major version must match Sierra Chart (currently `1.x.x`)
- `timestamp_ns`: Nanosecond Unix timestamp
- `component_name`: Identifier for your GUI component
- `capabilities`: Array of strings declaring what your GUI can do

---

### Step 2: Sierra Chart Responds with CONFIG_ACK

**Within 1 second**, the watchdog thread will respond with:

```json
{
  "header": {
    "msg_type": "CONFIG_ACK",
    "sender": "SIERRA_CPP_CORE",
    "sequence_id": 1,
    "timestamp_ns": 1766445169261955400,
    "version": "1.0.2"
  },
  "payload": {
    "negotiation_status": "ACCEPTED",
    "current_state": "READY",
    "available_ports": {
      "indicator_pub": 5555,
      "trade_req": 5556,
      "transformer_req": 5557,
      "execution_rep": 5558,
      "heartbeat_sub": 5559
    },
    "config": {
      "heartbeat_interval_ms": 1000,
      "trade_timeout_ms": 2500,
      "max_retries": 3,
      "enable_tca": true,
      "validation_latency_budget_ms": 50
    }
  }
}
```

**Response Fields:**
- `negotiation_status`: 
  - `"ACCEPTED"` - Version compatible, proceed to use other ports
  - `"REJECTED"` - Version incompatible, upgrade required
- `current_state`: SystemOrchestrator state machine
  - `"READY"` - System ready for trading
  - `"WAITING_FOR_AI"` - Still waiting for Transformer agent
  - `"ACTIVE_TRADING"` - Trades in progress
  - `"DEGRADED"` - Performance issues detected (strike 1-2)
  - `"DISCONNECTED"` - Connection lost (strike 3)
- `available_ports`: Dictionary of service port numbers
- `config`: Runtime configuration values

---

## Available Service Ports (What Each Does)

### Port 5555 - Indicator Publisher (PUB socket)
**Purpose:** Real-time market indicators from C++ studies  
**Pattern:** Publish-Subscribe (one-way broadcast)  
**Message Rate:** Every bar update (~60-100ms on 5-second bars)

**What GUI Receives:**
```json
{
  "header": {
    "msg_type": "INDICATOR_UPDATE",
    "timestamp_ns": 1766445169300000000,
    "sequence_id": 42
  },
  "payload": {
    "symbol": "MES",
    "timeframe": "5s",
    "indicators": {
      "macd_hist": -0.25,
      "keltner_position": "INSIDE_RANGE",
      "price_ema_separation": -3.5,
      "market_regime": "RANGING",
      // ... 18 total indicators
    }
  }
}
```

**GUI Action:** Update real-time indicator dashboard (no response required).

---

### Port 5556 - Trade Request (REQ socket)
**Purpose:** Submit trade signals from AI/manual entry  
**Pattern:** Request-Reply (synchronous)  
**Timeout:** 2500ms (configurable via `trade_timeout_ms`)

**GUI Sends:**
```json
{
  "header": {
    "msg_type": "TRADE_SIGNAL",
    "sender": "PYTHON_GUI_CLIENT",
    "timestamp_ns": 1766445169400000000
  },
  "payload": {
    "symbol": "MES",
    "direction": "LONG",
    "entry_price": 5975.50,
    "stop_loss": 5970.00,
    "take_profit": 5985.00,
    "quantity": 1,
    "strategy": "TRIPLE_SCREEN",
    "confidence": 0.82,
    "risk_percent": 1.5
  }
}
```

**Sierra Chart Validates and Responds:**
```json
{
  "header": {
    "msg_type": "TRADE_RESPONSE",
    "timestamp_ns": 1766445169402500000
  },
  "payload": {
    "status": "VALIDATED",  // or "REJECTED"
    "reason": "Pre-flight checks passed",
    "trade_id": "MES_20251222_142609",
    "execution_port": 5558
  }
}
```

**Validation Includes:**
- Risk manager 2%/6% daily loss limits
- Position sizing against account balance
- Market regime compatibility (no longs in downtrends)
- Latency budget (must respond within 50ms)

**GUI Action:** 
- If `status == "VALIDATED"`: Connect to port 5558 for execution
- If `status == "REJECTED"`: Display rejection reason, do NOT trade

---

### Port 5557 - Transformer Request (REQ socket)
**Purpose:** Request AI predictions from Transformer agent  
**Pattern:** Request-Reply (synchronous)  
**Timeout:** 500ms (fast inference required)

**GUI Sends:**
```json
{
  "header": {
    "msg_type": "PREDICTION_REQUEST",
    "timestamp_ns": 1766445169500000000
  },
  "payload": {
    "symbol": "MES",
    "timeframe": "5s",
    "lookback_bars": 200
  }
}
```

**Transformer Agent Responds:**
```json
{
  "header": {
    "msg_type": "PREDICTION_RESPONSE",
    "timestamp_ns": 1766445169580000000
  },
  "payload": {
    "action": "BUY",
    "confidence": 0.78,
    "hold_bars": 12,
    "reasoning": "Bullish divergence + momentum alignment"
  }
}
```

**GUI Action:** Display prediction in action plan dashboard.

---

### Port 5558 - Execution Reply (REP socket)
**Purpose:** Receive order fill confirmations  
**Pattern:** Reply (responds to trade executions)  
**Flow:** Sierra Chart places order → IB fills → sends confirmation here

**Sierra Chart Sends:**
```json
{
  "header": {
    "msg_type": "EXECUTION_UPDATE",
    "timestamp_ns": 1766445169700000000
  },
  "payload": {
    "trade_id": "MES_20251222_142609",
    "status": "FILLED",
    "fill_price": 5975.75,
    "fill_time_ns": 1766445169695000000,
    "slippage_ticks": 0.25,
    "ib_order_id": 12345
  }
}
```

**GUI Must Reply (Acknowledge):**
```json
{
  "header": {
    "msg_type": "EXECUTION_ACK",
    "timestamp_ns": 1766445169702000000
  },
  "payload": {
    "trade_id": "MES_20251222_142609",
    "received": true
  }
}
```

**GUI Action:** Update position tracking, log to Firestore journal.

---

### Port 5559 - Heartbeat Subscriber (SUB socket)
**Purpose:** Monitor Sierra Chart health (watchdog monitoring)  
**Pattern:** Publish-Subscribe (one-way broadcast)  
**Message Rate:** Every 1000ms (configurable via `heartbeat_interval_ms`)

**Sierra Chart Broadcasts:**
```json
{
  "header": {
    "msg_type": "HEARTBEAT",
    "timestamp_ns": 1766445170000000000,
    "sequence_id": 120
  },
  "payload": {
    "state": "READY",
    "strike_count": 0,
    "active_positions": 1,
    "last_bar_update_ms": 45
  }
}
```

**GUI Action:**
- Monitor `strike_count`: If >0, display warning (connection issues)
- Check `state`: If `"DEGRADED"` or `"DISCONNECTED"`, halt trading
- Verify `timestamp_ns` freshness: If >2 seconds old, Sierra Chart may be frozen

---

## Elite Watchdog: Strike Escalation System

### What Happens When GUI Disconnects

The watchdog thread monitors heartbeat liveness every 500ms. If the GUI stops responding:

**Strike 1 (Warning):**
- Logged to Sierra Chart Message Log
- `current_state` remains `"READY"`
- Trading continues normally
- **GUI Action:** Check network connection

**Strike 2 (Degraded):**
- State transitions to `"DEGRADED"`
- Heartbeat broadcast includes `"state": "DEGRADED"`
- **GUI Action:** Attempt reconnection immediately
- Trading may continue with reduced functionality

**Strike 3 (Emergency Shutdown):**
- State transitions to `"DISCONNECTED"`
- All trading HALTED
- Emergency callbacks triggered (close positions, cancel orders)
- **GUI Action:** Display critical alert, halt all operations

### How to Reset Strikes

Send a fresh `CONFIG_REQ` message. Successful handshake resets strike count to 0.

---

## State Machine Transitions

```
UNINITIALIZED → WAITING_FOR_AI (on Initialize())
WAITING_FOR_AI → NEGOTIATING (on CONFIG_REQ received)
NEGOTIATING → READY (on version validation success)
READY → ACTIVE_TRADING (on first trade signal)
ACTIVE_TRADING ↔ READY (trades complete/idle)
ANY → DEGRADED (on strike 2)
ANY → DISCONNECTED (on strike 3 or emergency shutdown)
```

**GUI Must Monitor:** Subscribe to port 5559 and watch `current_state` field.

---

## Required GUI Implementation Checklist

### Phase 1: Handshake (Startup)
- [ ] Connect to port 5560 (ZMQ REQ socket)
- [ ] Send CONFIG_REQ with correct version (`1.0.2`)
- [ ] Wait up to 5 seconds for CONFIG_ACK (retry with exponential backoff)
- [ ] Parse `available_ports` and store for later use
- [ ] Check `negotiation_status == "ACCEPTED"` (abort if REJECTED)
- [ ] Verify `current_state` is `"READY"` or `"WAITING_FOR_AI"`

### Phase 2: Connect to Service Ports
- [ ] Subscribe to port 5555 (indicator updates) - ZMQ SUB socket
- [ ] Subscribe to port 5559 (heartbeat monitoring) - ZMQ SUB socket
- [ ] Create REQ socket for port 5556 (trade requests) - **connect on demand**
- [ ] Create REQ socket for port 5557 (Transformer) - **connect on demand**
- [ ] Create REP socket for port 5558 (execution confirmations) - **if trading enabled**

### Phase 3: Heartbeat Monitoring Loop
```python
import zmq
import time
from datetime import datetime

def heartbeat_monitor():
    context = zmq.Context()
    heartbeat_sub = context.socket(zmq.SUB)
    heartbeat_sub.connect("tcp://172.20.112.1:5559")
    heartbeat_sub.subscribe("")  # Subscribe to all messages
    
    last_heartbeat_time = time.time()
    
    while True:
        try:
            # Non-blocking poll with timeout
            if heartbeat_sub.poll(timeout=2000):  # 2 second timeout
                msg = heartbeat_sub.recv_json()
                last_heartbeat_time = time.time()
                
                state = msg["payload"]["state"]
                strike_count = msg["payload"]["strike_count"]
                
                # Update GUI status indicator
                update_connection_status(state, strike_count)
                
                # Handle degraded state
                if state == "DEGRADED":
                    show_warning("Sierra Chart connection degraded")
                elif state == "DISCONNECTED":
                    show_critical_alert("Sierra Chart DISCONNECTED - Trading halted")
                    halt_all_trading_operations()
            else:
                # No heartbeat received in 2 seconds
                elapsed = time.time() - last_heartbeat_time
                if elapsed > 5.0:
                    show_critical_alert(f"No heartbeat for {elapsed:.1f}s - Connection lost")
                    halt_all_trading_operations()
        except Exception as e:
            log_error(f"Heartbeat monitor error: {e}")
            time.sleep(1)
```

### Phase 4: Indicator Dashboard Updates
```python
def indicator_stream():
    context = zmq.Context()
    indicator_sub = context.socket(zmq.SUB)
    indicator_sub.connect("tcp://172.20.112.1:5555")
    indicator_sub.subscribe("")
    
    while True:
        try:
            msg = indicator_sub.recv_json()
            indicators = msg["payload"]["indicators"]
            
            # Update GUI indicator widgets
            update_macd_display(indicators["macd_hist"])
            update_keltner_display(indicators["keltner_position"])
            update_regime_display(indicators["market_regime"])
            # ... update all 18 indicators
        except Exception as e:
            log_error(f"Indicator stream error: {e}")
```

### Phase 5: Trade Submission
```python
def submit_trade_signal(symbol, direction, entry, stop, target, qty, confidence):
    context = zmq.Context()
    trade_req = context.socket(zmq.REQ)
    trade_req.connect("tcp://172.20.112.1:5556")
    
    # Set timeout (2500ms per config)
    trade_req.setsockopt(zmq.RCVTIMEO, 2500)
    
    request = {
        "header": {
            "msg_type": "TRADE_SIGNAL",
            "sender": "PYTHON_GUI_CLIENT",
            "timestamp_ns": time.time_ns()
        },
        "payload": {
            "symbol": symbol,
            "direction": direction,
            "entry_price": entry,
            "stop_loss": stop,
            "take_profit": target,
            "quantity": qty,
            "strategy": "AI_DRIVEN",
            "confidence": confidence
        }
    }
    
    try:
        trade_req.send_json(request)
        response = trade_req.recv_json()
        
        if response["payload"]["status"] == "VALIDATED":
            return True, response["payload"]["trade_id"]
        else:
            reason = response["payload"]["reason"]
            show_error(f"Trade rejected: {reason}")
            return False, None
    except zmq.Again:
        show_error("Trade validation timeout (2500ms exceeded)")
        return False, None
    finally:
        trade_req.close()
```

---

## Version Compatibility Rules

### Semantic Versioning: `major.minor.patch`

**Major Version (Breaking Changes):**
- GUI version `2.0.0` **CANNOT** connect to Sierra Chart `1.x.x`
- Handshake will return `"negotiation_status": "REJECTED"`
- **Required Action:** Upgrade GUI or Sierra Chart to match major versions

**Minor Version (New Features):**
- GUI version `1.5.0` **CAN** connect to Sierra Chart `1.3.0`
- Backward compatible (older minor versions work with newer)
- New capabilities may not be available

**Patch Version (Bug Fixes):**
- Fully compatible across all patch versions
- `1.0.2` works with `1.0.1`, `1.0.0`, etc.

**Current Versions (December 22, 2025):**
- Sierra Chart SystemOrchestrator: `1.0.2`
- Required GUI minimum: `1.0.0`
- Recommended GUI: `1.0.2` (latest)

---

## Error Handling & Recovery

### Connection Refused (Sierra Chart Not Running)
```python
try:
    req.connect("tcp://172.20.112.1:5560")
    req.send_json(config_req)
except zmq.ZMQError as e:
    if e.errno == zmq.ECONNREFUSED:
        show_error("Sierra Chart not running or port 5560 blocked")
        # Retry with exponential backoff
```

### Timeout During Handshake
```python
req.setsockopt(zmq.RCVTIMEO, 5000)  # 5 second timeout
try:
    response = req.recv_json()
except zmq.Again:
    show_error("Handshake timeout - Sierra Chart may be frozen")
    # Retry or abort
```

### Version Mismatch
```python
if response["payload"]["negotiation_status"] == "REJECTED":
    sierra_version = response["header"]["version"]
    gui_version = "1.0.2"
    show_error(f"Version mismatch: GUI {gui_version} incompatible with Sierra Chart {sierra_version}")
    # Prompt user to upgrade
```

### Strike Escalation Recovery
```python
# On strike 2 (DEGRADED state)
if state == "DEGRADED":
    # Attempt immediate reconnection
    reconnect_to_port_5560()
    
# On strike 3 (DISCONNECTED state)
if state == "DISCONNECTED":
    # Emergency: halt all operations
    cancel_pending_orders()
    close_open_positions()
    show_critical_alert("Emergency shutdown triggered - manual intervention required")
```

---

## Testing Checklist

### Test 1: Normal Handshake
- [ ] Start Sierra Chart with MindfulTrader.dll loaded
- [ ] Run GUI, connect to port 5560
- [ ] Verify CONFIG_ACK received within 1 second
- [ ] Verify `negotiation_status == "ACCEPTED"`
- [ ] Verify all 5 service ports listed

### Test 2: Heartbeat Liveness
- [ ] Connect GUI, establish handshake
- [ ] Subscribe to port 5559
- [ ] Verify heartbeat messages every 1 second
- [ ] Verify `strike_count == 0`
- [ ] Kill GUI process, wait 2 seconds
- [ ] Restart GUI, check if strike count increased

### Test 3: Strike Escalation
- [ ] Connect GUI, establish handshake
- [ ] Stop sending heartbeat responses
- [ ] Wait 2 seconds → verify strike 1 logged
- [ ] Wait another 2 seconds → verify state == "DEGRADED"
- [ ] Wait another 2 seconds → verify state == "DISCONNECTED"
- [ ] Reconnect and verify strike count resets to 0

### Test 4: Version Rejection
- [ ] Modify GUI to send version `2.0.0`
- [ ] Attempt handshake
- [ ] Verify `negotiation_status == "REJECTED"`
- [ ] Verify error message displayed to user

### Test 5: UI Responsiveness
- [ ] Start GUI with rapid connect/disconnect loop
- [ ] Manually interact with Sierra Chart (scroll, change symbols)
- [ ] Verify zero UI freezing (watchdog is non-blocking)

---

## Production Deployment Notes

### Network Configuration
- **WSL2 Environment:** GUI connects to `172.20.112.1` (Windows host IP)
- **Same Machine:** Use `localhost` or `127.0.0.1`
- **Remote Sierra Chart:** Use actual IP address (ensure firewall rules allow ports 5555-5560)

### Firewall Rules (Windows)
```powershell
# Allow inbound connections to MindfulTrader ports
New-NetFirewallRule -DisplayName "MindfulTrader Port 5555" -Direction Inbound -LocalPort 5555 -Protocol TCP -Action Allow
New-NetFirewallRule -DisplayName "MindfulTrader Port 5556" -Direction Inbound -LocalPort 5556 -Protocol TCP -Action Allow
New-NetFirewallRule -DisplayName "MindfulTrader Port 5557" -Direction Inbound -LocalPort 5557 -Protocol TCP -Action Allow
New-NetFirewallRule -DisplayName "MindfulTrader Port 5558" -Direction Inbound -LocalPort 5558 -Protocol TCP -Action Allow
New-NetFirewallRule -DisplayName "MindfulTrader Port 5559" -Direction Inbound -LocalPort 5559 -Protocol TCP -Action Allow
New-NetFirewallRule -DisplayName "MindfulTrader Port 5560" -Direction Inbound -LocalPort 5560 -Protocol TCP -Action Allow
```

### Monitoring & Logging
```python
import logging

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s [%(levelname)s] %(message)s',
    handlers=[
        logging.FileHandler('mindfultrader_gui.log'),
        logging.StreamHandler()
    ]
)

# Log all handshake attempts
logging.info(f"Sending CONFIG_REQ to {host}:{port}")
logging.info(f"Received CONFIG_ACK: status={status}, state={state}")

# Log heartbeat status
logging.debug(f"Heartbeat received: state={state}, strike_count={strike_count}")

# Log trade submissions
logging.info(f"Trade signal: {symbol} {direction} @ {entry}, status={status}")
```

---

## Summary: GUI Developer Actions

1. **Update connection logic:** Use async/non-blocking handshake (watchdog is now instant)
2. **Implement heartbeat monitoring:** Subscribe to port 5559, detect strikes
3. **Handle state transitions:** React to DEGRADED/DISCONNECTED states
4. **Version validation:** Ensure GUI version matches Sierra Chart major version
5. **Error recovery:** Implement reconnection logic with exponential backoff
6. **Production logging:** Log all handshake, heartbeat, and trade events

The elite watchdog ensures Sierra Chart never freezes. The GUI must be equally robust in handling disconnections and reconnections gracefully.

---

**Elite Status Achieved:** ✅ Non-blocking async discovery, thread-safe validation, strike escalation  
**Next Milestone:** GUI implements full heartbeat monitoring loop and state machine awareness
