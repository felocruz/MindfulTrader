# GUI Team Implementation Checklist - Elite Messaging Protocol

**Start Date:** December 22, 2025  
**Target:** Parallel implementation with C++ team  
**Reference:** ELITE_MESSAGING_PROTOCOL_SPEC.md  
**Language:** Python 3.10+

---

## Priority 1: Port 5560 - Handshake Client (Day 1)

### Goal
Connect to C++ master controller, negotiate capabilities, receive port configuration.

### Create: `src/system_client.py`

```python
import zmq
import json
import time
from typing import Dict, Optional

class SystemClient:
    """
    Client for Port 5560 - Master Controller handshake
    """
    
    def __init__(self):
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.REQ)
        self.socket.connect("tcp://127.0.0.1:5560")
        self.socket.setsockopt(zmq.RCVTIMEO, 5000)  # 5s timeout
        
        self.capabilities = {
            "indicator_display": True,
            "manual_trade_entry": True,
            "chart_visualization": True,
            "risk_dashboard": True
        }
        
        self.config = None
    
    def negotiate(self) -> bool:
        """
        Send CONFIG_REQ and receive CONFIG_ACK
        Returns True if handshake successful
        """
        request = {
            "header": {
                "msg_type": "CONFIG_REQ",
                "version": "1.0.2",
                "timestamp_ns": time.time_ns(),
                "sender": "PYTHON_GUI"
            },
            "payload": {
                "component_name": "GUI_SUBSCRIBER",
                "capabilities": [k for k, v in self.capabilities.items() if v]
            }
        }
        
        try:
            self.socket.send_string(json.dumps(request))
            response_str = self.socket.recv_string()
            response = json.loads(response_str)
            
            if response["header"]["msg_type"] == "ERROR":
                error = response["payload"]
                print(f"🔴 Handshake failed: {error['error_message']}")
                
                # Version mismatch - critical error
                if error["error_code"] == "VERSION_MISMATCH":
                    print("⚠️ C++ and Python versions incompatible!")
                    return False
                
                return False
            
            # Store configuration
            self.config = response["payload"]
            
            print(f"✅ Handshake successful: {self.config['negotiation_status']}")
            print(f"   Ports available: {self.config['available_ports']}")
            print(f"   System state: {self.config['current_state']}")
            
            return True
            
        except zmq.error.Again:
            print("🔴 Handshake timeout (C++ not responding)")
            return False
        except Exception as e:
            print(f"🔴 Handshake error: {e}")
            return False
    
    def query_state(self) -> Optional[str]:
        """
        Query current system state
        Returns state name or None on error
        """
        request = {
            "header": {
                "msg_type": "STATE_QUERY",
                "version": "1.0.2",
                "timestamp_ns": time.time_ns(),
                "sender": "PYTHON_GUI"
            },
            "payload": {}
        }
        
        try:
            self.socket.send_string(json.dumps(request))
            response_str = self.socket.recv_string()
            response = json.loads(response_str)
            
            return response["payload"]["current_state"]
            
        except Exception as e:
            print(f"🔴 State query error: {e}")
            return None
    
    def close(self):
        self.socket.close()
        self.context.term()
```

**📋 Checklist:**
- [ ] Create `src/system_client.py`
- [ ] Implement `negotiate()` method
- [ ] Implement `query_state()` method
- [ ] Handle version mismatch error
- [ ] Store port configuration
- [ ] Test with C++ SystemOrchestrator

**Test:**
```bash
python3 -c "
from src.system_client import SystemClient
client = SystemClient()
if client.negotiate():
    print('Ready to start GUI')
"
```

---

## Priority 2: Port 5555 - Indicator Subscriber (Days 1-2)

### Goal
Subscribe to indicator stream, display in GUI, detect gaps.

### Create: `src/indicator_subscriber.py`

```python
import zmq
import json
import time
from typing import Dict, Callable, Optional
from collections import deque

class IndicatorSubscriber:
    """
    Subscriber for Port 5555 - Real-time indicator stream
    """
    
    def __init__(self, callback: Optional[Callable] = None):
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.SUB)
        self.socket.connect("tcp://127.0.0.1:5555")
        self.socket.subscribe("")  # Subscribe to all messages
        
        self.callback = callback
        self.last_sequence = -1
        self.gap_count = 0
        
        # Keep-alive tracking
        self.last_message_time = time.time()
        self.keep_alive_interval = 300  # 5 minutes
    
    def start(self):
        """
        Start listening for indicators (blocking)
        """
        print("📡 Indicator subscriber started...")
        
        while True:
            try:
                # Receive message
                msg_str = self.socket.recv_string(flags=zmq.NOBLOCK)
                data = json.loads(msg_str)
                
                # Update last message time
                self.last_message_time = time.time()
                
                # Extract header
                header = data["header"]
                msg_type = header["msg_type"]
                sequence = header["sequence_id"]
                
                # Check for gaps
                if self.last_sequence >= 0:
                    expected = self.last_sequence + 1
                    if sequence != expected:
                        gap_size = sequence - expected
                        self.gap_count += gap_size
                        print(f"⚠️ Gap detected: Expected {expected}, got {sequence} (gap={gap_size})")
                
                self.last_sequence = sequence
                
                # Handle message types
                if msg_type == "BAR_CLOSE_UPDATE":
                    self._handle_bar_close(data["payload"])
                elif msg_type == "KEEP_ALIVE":
                    print(f"💚 Keep-alive received (seq {sequence})")
                else:
                    print(f"⚠️ Unknown message type: {msg_type}")
                
                # Callback for GUI update
                if self.callback:
                    self.callback(data)
                
            except zmq.Again:
                # No message available - check keep-alive timeout
                time_since_last = time.time() - self.last_message_time
                if time_since_last > self.keep_alive_interval + 60:  # 1 min grace
                    print(f"🔴 No message for {time_since_last:.0f}s - C++ may be dead")
                
                time.sleep(0.1)
                
            except KeyboardInterrupt:
                print("\n👋 Subscriber stopped")
                break
            except Exception as e:
                print(f"🔴 Subscriber error: {e}")
                time.sleep(1)
    
    def _handle_bar_close(self, payload: Dict):
        """
        Process BAR_CLOSE_UPDATE payload
        """
        # Extract key indicators
        long_macd = payload.get("LONG_MACD", {})
        rsi = payload.get("RSI", 0.0)
        stoch = payload.get("INTERM_STOCHASTIC", {})
        
        print(f"📊 Bar Close: MACD={long_macd.get('value', 0):.2f}, "
              f"RSI={rsi:.1f}, Stoch={stoch.get('k', 0):.1f}")
    
    def close(self):
        self.socket.close()
        self.context.term()
```

**📋 Checklist:**
- [ ] Create `src/indicator_subscriber.py`
- [ ] Implement `start()` method (blocking loop)
- [ ] Implement sequence gap detection
- [ ] Implement keep-alive timeout detection
- [ ] Handle BAR_CLOSE_UPDATE messages
- [ ] Handle KEEP_ALIVE messages
- [ ] Add callback for GUI updates
- [ ] Test with C++ MindfulSocketZMQ

**Test:**
```bash
python3 -c "
from src.indicator_subscriber import IndicatorSubscriber
sub = IndicatorSubscriber()
sub.start()  # Will print indicators as they arrive
"
```

---

## Priority 3: GUI Integration (Days 2-3)

### Goal
Display indicators in real-time GUI, show connection status.

### Modify: `src/main_window.py` (or equivalent)

**Add connection status indicator:**
```python
class MainWindow(QMainWindow):
    def __init__(self):
        super().__init__()
        
        # Connection status
        self.status_bar = QStatusBar()
        self.setStatusBar(self.status_bar)
        
        self.connection_label = QLabel("🔴 Disconnected")
        self.status_bar.addPermanentWidget(self.connection_label)
        
        # System client
        self.system_client = SystemClient()
        
        # Indicator subscriber
        self.indicator_sub = None
        
        # Connect on startup
        self.connect_to_cpp()
    
    def connect_to_cpp(self):
        """
        Negotiate with C++ and start subscriber
        """
        if self.system_client.negotiate():
            self.connection_label.setText("🟢 Connected")
            
            # Start indicator subscriber in background thread
            self.indicator_sub = IndicatorSubscriber(callback=self.update_indicators)
            self.sub_thread = threading.Thread(target=self.indicator_sub.start, daemon=True)
            self.sub_thread.start()
        else:
            self.connection_label.setText("🔴 Handshake Failed")
            QMessageBox.critical(self, "Connection Error", 
                "Failed to connect to C++ trading system")
    
    def update_indicators(self, data: Dict):
        """
        Callback from indicator subscriber - update GUI
        """
        payload = data["payload"]
        
        # Update MACD chart
        long_macd = payload.get("LONG_MACD", {})
        self.macd_chart.append(long_macd.get("value", 0))
        
        # Update RSI gauge
        rsi = payload.get("RSI", 0.0)
        self.rsi_gauge.setValue(rsi)
        
        # Update Stochastic
        stoch = payload.get("INTERM_STOCHASTIC", {})
        self.stoch_k_line.append(stoch.get("k", 0))
        self.stoch_d_line.append(stoch.get("d", 0))
```

**📋 Checklist:**
- [ ] Add connection status indicator to GUI
- [ ] Create SystemClient instance
- [ ] Call `negotiate()` on startup
- [ ] Start IndicatorSubscriber in background thread
- [ ] Implement callback to update GUI widgets
- [ ] Handle connection failures gracefully
- [ ] Add reconnect button

---

## Optional: Port 5558 - Trade Validation (Day 3+)

### Goal
Allow GUI to send manual trade requests for validation.

### Create: `src/trade_client.py`

```python
import zmq
import json
import time
from typing import Dict, Optional

class TradeClient:
    """
    Client for Port 5558 - Trade validation/execution
    """
    
    def __init__(self):
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.REQ)
        self.socket.connect("tcp://127.0.0.1:5558")
        self.socket.setsockopt(zmq.RCVTIMEO, 5000)
        
        self.sequence_id = 1
    
    def validate_trade(
        self, 
        side: str, 
        confidence: float = 0.80
    ) -> Optional[Dict]:
        """
        Validate trade with C++ before execution
        """
        request = {
            "header": {
                "msg_type": "VALIDATE_TRADE",
                "version": "1.0.2",
                "timestamp_ns": time.time_ns(),
                "sequence_id": self.sequence_id,
                "sender": "PYTHON_GUI"
            },
            "payload": {
                "side": side,
                "confidence": confidence,
                "pattern": "MANUAL_GUI_ENTRY"
            }
        }
        
        self.sequence_id += 1
        
        try:
            self.socket.send_string(json.dumps(request))
            response_str = self.socket.recv_string()
            response = json.loads(response_str)
            
            if response["header"]["msg_type"] == "ERROR":
                print(f"🔴 Validation failed: {response['payload']['error_message']}")
                return None
            
            payload = response["payload"]
            
            if payload["validation_status"] == "APPROVED":
                print(f"✅ Trade validated: {side} @ {payload['trade_params']['entry_price']}")
                return payload
            else:
                print(f"⚠️ Trade rejected: {payload['rejection_reason']}")
                return None
            
        except zmq.error.Again:
            print("🔴 Validation timeout")
            return None
```

**📋 Checklist:**
- [ ] Create `src/trade_client.py`
- [ ] Implement `validate_trade()` method
- [ ] Implement `execute_trade()` method
- [ ] Add to GUI as "Manual Trade" button
- [ ] Show validation results in dialog
- [ ] Handle rejections gracefully

---

## Testing Strategy

### End-to-End Test

```python
# test_full_flow.py

from src.system_client import SystemClient
from src.indicator_subscriber import IndicatorSubscriber
from src.trade_client import TradeClient
import time

# 1. Handshake
print("=== Phase 1: Handshake ===")
sys_client = SystemClient()
assert sys_client.negotiate(), "Handshake failed"

# 2. Subscribe to indicators
print("\n=== Phase 2: Indicator Subscription ===")
indicators_received = []

def capture_indicator(data):
    indicators_received.append(data)
    if len(indicators_received) >= 3:
        return  # Got 3 messages, test passed

sub = IndicatorSubscriber(callback=capture_indicator)

import threading
sub_thread = threading.Thread(target=sub.start, daemon=True)
sub_thread.start()

time.sleep(10)  # Wait for 3 bar closes
assert len(indicators_received) >= 3, "No indicators received"
print(f"✅ Received {len(indicators_received)} indicator updates")

# 3. Validate trade
print("\n=== Phase 3: Trade Validation ===")
trade_client = TradeClient()
validated = trade_client.validate_trade("LONG", confidence=0.80)
assert validated is not None, "Trade validation failed"
print(f"✅ Trade validated: Risk {validated['risk_analysis']['risk_percent']:.2f}%")

print("\n🎉 All tests passed!")
```

---

## Success Criteria

- [ ] GUI connects to C++ on startup (port 5560 handshake)
- [ ] GUI receives indicator updates (port 5555 stream)
- [ ] GUI detects sequence gaps and logs them
- [ ] GUI detects keep-alive timeout
- [ ] GUI validates manual trades (port 5558)
- [ ] Connection status shown in GUI
- [ ] Version mismatch handled gracefully

---

## GUI Development Workflow

**Day 1:**
1. Implement SystemClient
2. Test handshake with C++
3. Add connection status to GUI

**Day 2:**
1. Implement IndicatorSubscriber
2. Test with live C++ stream
3. Wire up GUI callbacks

**Day 3:**
1. Implement TradeClient
2. Add manual trade button
3. End-to-end testing

---

## Reference

- **Full Spec:** `docs/ELITE_MESSAGING_PROTOCOL_SPEC.md`
- **Port 5560 (Handshake):** Lines 1069-1885
- **Port 5555 (Indicators):** Lines 1887-2512
- **Port 5558 (Trades):** Lines 4776-5497
- **Message Schemas:** Section 4.5 (lines 398-822)

---

## Need Help?

If C++ responses don't match expected schemas:
1. Check C++ logs for errors
2. Verify version numbers match (1.0.2)
3. Use `json.dumps(request, indent=2)` to debug request format
4. Check ZMQ socket timeouts (default 5s)
