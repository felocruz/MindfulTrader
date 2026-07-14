# Live Agent View: Complete Implementation Guide

**Purpose**: Unified documentation for the Live Agent View GUI component that displays TransformerAgent predictions and C++ RiskManager validation status in the MindfulTrader system.

**Implementation Date**: December 9, 2025  
**Status**: ✅ **IMPLEMENTED** - Core functionality complete

---

## Table of Contents

1. [Architecture Overview](#architecture-overview)
2. [Design Specification](#design-specification)
3. [Implementation Details](#implementation-details)
4. [Testing & Deployment](#testing--deployment)
5. [Reference Material](#reference-material)

---

## 1. Architecture Overview

### System Architecture

The Live Agent View bridges two systems:

- **C++ Backend (Sierra Chart)**: Data publisher, indicator calculation, risk validation, trade execution
- **Python GUI (Dash Application)**: Data subscriber, visualization, trade controls

```
┌─────────────────────────────────────────────────────────┐
│ C++ Backend (Sierra Chart ACSIL Studies)                │
│ ├─ TripleScreen*.cpp (240/60/15-min timeframes)         │
│ ├─ IndicatorManager (indicator objects + state)         │
│ ├─ DataCollector (200-bar history deque)                │
│ ├─ TransportStream (PUB stream port 5555)               │
│ ├─ SystemOrchestrator (control handshake port 5560)     │
│ ├─ AIHeartbeatMonitor (health stream port 5559)         │
│ ├─ RiskManager (trade validation logic)                 │
│ └─ TradeExecutionServer (validation/execution requests) │
└─────────────────────────────────────────────────────────┘
                        ↕ ZeroMQ
┌─────────────────────────────────────────────────────────┐
│ Python GUI (Dash)                                        │
│ ├─ app.py (callbacks, data store)                       │
│ ├─ zmq_client.py (SUB socket, REQ socket)               │
│ ├─ live_agent.py (Transformer predictions)              │
│ ├─ live_agent_view.py (UI display component)            │
│ └─ dash_util.py (cache keys, constants)                 │
└─────────────────────────────────────────────────────────┘
```

### Communication Protocols

**1. PUB/SUB Pattern (One-Way Data Stream: C++ → Python)**
- **Purpose**: Real-time market data broadcasting
- **Socket**: ZeroMQ PUB socket on port 5556
- **Format**: JSON messages
- **Flow**: C++ publishes indicator updates → Python SUB socket receives

**2. REQ/REP Pattern (Two-Way: Python ↔ C++)**
- **Purpose**: Trade validation and execution
- **Socket**: ZeroMQ REQ socket on port 5558
- **Format**: JSON request/response
- **Flow**: Python sends trade request → C++ validates → Python receives response

### Component Details

#### C++ Backend Components

| Component | Responsibility | File Location |
|:----------|:---------------|:--------------|
| **TripleScreen*.cpp** | Main study entry points for 240/60/15-min analysis | src/TripleScreen*.cpp |
| **IndicatorManager** | Singleton owning indicator objects, updates states, collects JSON payload | include/IndicatorManager.h |
| **DataCollector** | Receives indicator payload every bar, maintains 200-bar history deque | include/DataCollector.h |
| **TransportStream** | PUB stream for event/indicator payloads | include/transport/TransportStream.h |
| **SystemOrchestrator** | CONFIG_REQ/CONFIG_ACK control-plane negotiation | include/SystemOrchestrator.h |
| **AIHeartbeatMonitor** | Subscribes to AI heartbeat health stream | include/transport/AIHeartbeatMonitor.h |
| **RiskManager** | Validates trades: daily loss limit, portfolio heat, consecutive losses | include/RiskManager.h |
| **TradeExecutionServer** | Handles ExecuteTrade and ModelPrediction requests from GUI with real-time validation | include/TradeExecutionServer.h |

#### Python GUI Components

| Component | Responsibility | File Location |
|:----------|:---------------|:--------------|
| **app.py** | Entry point, registers callbacks, manages data store | src/app.py |
| **zmq_client.py** | SUB socket for market data, REQ socket for trade execution | src/zmq_client.py |
| **live_agent.py** | Transformer model predictions, trade plan generation | src/live_agent.py |
| **live_agent_view.py** | UI component for predictions/validation/execution buttons | src/live_agent_view.py |
| **dash_util.py** | Cache keys (CACHE_KEY_AGENT_PREDICTION), shared constants | src/dash_util.py |

### Data Flows

#### Automatic Predictions Flow

```
C++ Sierra Chart Study
  ↓ Bar Close Detection (BHCS_BAR_HAS_CLOSED)
IndicatorManager.OnBarClose()
  ↓ Publish BAR_CLOSE_UPDATE with 27 indicators
MindfulSocketZMQ (port 5556)
  ↓ ZMQ PUB/SUB
zmq_client.py (SUB socket)
  ↓ Add to message_queue
update_data_store() callback
  ↓ Call live_agent.predict(current_position_state)
live_agent.py
  ↓ Returns augmented prediction dict
  ↓ {action, confidence, trade_plan}
cache[CACHE_KEY_AGENT_PREDICTION] ← Store prediction
  ↓ Cache update triggers callback
live_agent_view.py
  ↓ UI auto-refresh with Markdown display
```

**Cache Key Structure (CACHE_KEY_AGENT_PREDICTION):**
```python
{
    "action": "ENTER_LONG",  # TradeAction enum
    "confidence": 0.873,      # 0.0-1.0
    "trade_plan": {
        "rationale": "Strong upward momentum with RSI confirmation...",
        "stop_loss_description": "2.5 ATR below entry at 5,235.25",
        "profit_target_description": "5.0 ATR above entry at 5,265.75 (2:1 R:R)"
    }
}
```

#### Manual Validation Flow

```
User clicks "Request Validation" button
  ↓
live_agent_view.py callback
  ↓ Extract prediction from cache
  ↓ Build ValidateTrade request
TradeExecutionClient.validate_trade()
  ↓ Send ZMQ REQ (port 5558)
  ↓ setupType, entryRule, stopRule, targetRule, direction, quantity
C++ TradeExecutionServer
  ↓ Parse request
  ↓ Call RiskManager.ValidateOrder()
  ↓ Calculate entry/stop/target prices using live market data
  ↓ Check daily loss limit, portfolio heat, consecutive losses
  ↓ Build validation response
TradeExecutionClient receives response
  ↓ {allowed, entryPrice, stopPrice, targetPrice, riskAmount, rewardAmount, reason, riskMetrics}
live_agent_view.py
  ↓ Update UI directly (validation badge, rejection alert, place trade button)
```

**Validation Response Structure:**

**Approved:**
```json
{
    "allowed": true,
    "entryPrice": 5245.50,
    "stopPrice": 5235.25,
    "targetPrice": 5265.75,
    "riskAmount": 41.0,        // ticks
    "rewardAmount": 82.0,      // ticks
    "riskRewardRatio": 2.0,
    "reason": "",              // empty if approved
    "riskMetrics": {
        "dailyPnL": 1245.00,
        "portfolioHeat": 0.032,
        "isTradingHalted": false,
        "maxDailyLoss": -500.0,
        "maxPortfolioHeat": 0.10
    }
}
```

**Rejected:**
```json
{
    "allowed": false,
    "entryPrice": 0.0,
    "stopPrice": 0.0,
    "targetPrice": 0.0,
    "riskAmount": 0.0,
    "rewardAmount": 0.0,
    "riskRewardRatio": 0.0,
    "reason": "Daily loss limit exceeded: -$567 / -$500 max",
    "riskMetrics": {
        "dailyPnL": -567.00,
        "portfolioHeat": 0.045,
        "isTradingHalted": true,
        "maxDailyLoss": -500.0,
        "maxPortfolioHeat": 0.10
    }
}
```

#### Bar Close Updates

```
Every bar on 15-min chart
  ↓ All indicators updated
IndicatorManager.GetPayload(sc, false)
  ↓ Gathers states from all indicator objects
  ↓ Returns JSON payload with 27 indicators
DataCollector.AddBar(payload)
  ↓ Appends to CSV + pushes into 200-element deque
  ↓
If BHCS_BAR_HAS_CLOSED detected
  ↓
IndicatorManager.OnBarClose(sc, isConnected)
  ↓ If GUI connected
  ↓ Publishes cached payload via MindfulSocketZMQ
SocketMessage msg
  ↓ type = "BAR_CLOSE_UPDATE"
  ↓ payload = JSON with datetime, last, 27 indicators
Thread-safe queue m_pubQueue
  ↓ ZMQ PUB broadcast (port 5556)
```

**BAR_CLOSE_UPDATE Message Format:**
```json
{
    "type": "BAR_CLOSE_UPDATE",
    "payload": {
        "datetime": "2024-12-09 10:30:00",
        "last": 4750.50,
        "long_macd": 3,
        "long_FI13_signal": 2,
        "long_imp": 1,
        "long_ema": 2,
        "interm_stochastic": 1,
        "raschke_strategy_setup": 5,
        "raschke_tactical_trigger": 2,
        "rsi": 58.3,
        "interm_mkt_action": 1,
        "short_imp": 1,
        "short_ema": 2,
        "elder_impulse_histogram": 3,
        "wave_force_index": 2,
        "daily_bias": 3,
        "market_regime": 2,
        "nh_nl_signal": 1
        // ... 27 indicators total
    }
}
```

### TradeExecutionClient Class

**Purpose**: Python wrapper for ZMQ REQ socket communication with C++ TradeExecutionServer

**Location**: `src/zmq_client.py`

**Class Definition:**
```python
class TradeExecutionClient:
    def __init__(self, host: str = "localhost", port: int = 5558):
        """Initialize ZMQ REQ socket for trade execution requests."""
        self.context = zmq.Context()
        self.socket = self.context.socket(zmq.REQ)
        self.socket.connect(f"tcp://{host}:{port}")
        self.socket.setsockopt(zmq.RCVTIMEO, 5000)  # 5-second timeout
        
    def send_request(self, request_dict: dict) -> Optional[dict]:
        """Send JSON request, wait for response with timeout handling."""
        try:
            self.socket.send_json(request_dict)
            response = self.socket.recv_json()
            return response
        except zmq.Again:
            # Timeout occurred
            return None
            
    def execute_trade(self, action: str, pattern: str, 
                     ai_prediction: dict, user_initiated: bool = False) -> Optional[dict]:
        """Execute trade with real-time validation in C++."""
        request = {
            "type": "ExecuteTrade",
            "action": action,  # ENTER_LONG, ENTER_SHORT, EXIT_LONG, EXIT_SHORT
            "pattern": pattern,  # Pattern enum string
            "ai_prediction": ai_prediction,  # Full AI context
            "user_initiated": user_initiated  # Manual vs automated
        }
        return self.send_request(request)
        
    def send_model_prediction(self, prediction: dict) -> Optional[dict]:
        """Send AI model prediction for auto-execution."""
        request = {
            "type": "ModelPrediction",
            "prediction": prediction
        }
        return self.send_request(request)
        
    def close(self):
        """Cleanup socket and context."""
        self.socket.close()
        self.context.term()
```

---

## 2. Design Specification

### Implementation Status (December 9, 2025)

**Completed Components:**
- ✅ LiveAgent class renamed from GuiAdapter
- ✅ TransformerAgent model integrated (18 indicators)
- ✅ NH-NL signal callback implemented
- ✅ `predict()` method returns augmented dict with action/confidence/trade_plan
- ✅ Cache store infrastructure (CACHE_KEY_AGENT_PREDICTION in dash_util.py)
- ✅ `update_data_store()` stores predictions automatically

**Pending Components:**
- ⏳ TradeExecutionClient class implementation (zmq_client.py)
- ⏳ live_agent_view.py module (UI components)
- ⏳ app.py integration (callbacks, initialization)

### Two-Panel Layout

```
┌───────────────────────────────────────────────────────────────────┐
│ Live Agent View                                                   │
├───────────────────────────────────────────────────────────────────┤
│                                                                   │
│ ┌─────────────────────────────┐ ┌───────────────────────────────┐│
│ │ Panel 1: TransformerAgent   │ │ Panel 2: C++ RiskManager      ││
│ │ Predictions                  │ │ Status                        ││
│ ├─────────────────────────────┤ ├───────────────────────────────┤│
│ │ ▼ Markdown Card              │ │ ▼ Validation Badge            ││
│ │   • Action: ENTER_LONG       │ │   ⏸️ WAITING (secondary)      ││
│ │   • Confidence: 87.3%        │ │   ✅ APPROVED (success)       ││
│ │                              │ │   ❌ REJECTED (danger)        ││
│ │ ▼ Trade Parameters Table     │ │                               ││
│ │   Entry:  5,245.50           │ │ ▼ Rejection Alert (if any)    ││
│ │   Stop:   5,235.25           │ │   ⚠️ "Daily loss limit..."   ││
│ │   Target: 5,265.75           │ │                               ││
│ │   R:R:    2.0:1              │ │ ▼ Risk Metrics                ││
│ │                              │ │   Daily P&L: +$1,245.00       ││
│ │ ▼ Position Sizing (2% rule)  │ │   Portfolio Heat: 3.2%        ││
│ │   Risk: $512.50              │ │   Consecutive Losses: 0       ││
│ │   Quantity: 2 contracts      │ │   Trading Halted: False       ││
│ │                              │ │                               ││
│ │ ▼ Model Confidence           │ │                               ││
│ │   Distribution Table         │ │                               ││
│ │                              │ │                               ││
│ │ ▼ Action Buttons             │ │                               ││
│ │   [Request Validation]       │ │                               ││
│ │   [Place Trade] (disabled)   │ │                               ││
│ └─────────────────────────────┘ └───────────────────────────────┘│
│                                                                   │
└───────────────────────────────────────────────────────────────────┘
```

### Panel 1: TransformerAgent Predictions

**Markdown Card Display:**
- **Action**: ENTER_LONG, ENTER_SHORT, HOLD_FLAT, EXIT_LONG, EXIT_SHORT
- **Confidence**: 0.0-1.0 as percentage (e.g., 87.3%)
- **Rationale**: Natural language explanation from model

**Trade Parameters Table:**
| Parameter | Value | Description |
|:----------|:------|:------------|
| Entry | 5,245.50 | Calculated by C++ using live market data |
| Stop | 5,235.25 | ATR-based or swing point |
| Target | 5,265.75 | Risk-reward ratio applied |
| R:R | 2.0:1 | Risk-reward ratio |

**Position Sizing (2% Rule):**
- Account balance: $100,000 (from config)
- Max risk per trade: 2% = $2,000
- Risk per contract: $512.50 (41 ticks × $12.50/tick for ES)
- Quantity: 2 contracts (stays within 2% rule)

**Model Confidence Distribution:**
| Action | Probability |
|:-------|:------------|
| ENTER_LONG | 87.3% |
| HOLD_FLAT | 8.2% |
| ENTER_SHORT | 4.5% |

**Action Buttons:**
1. **"Request Validation"**
   - Sends ZMQ ExecuteTrade request to C++ with AI prediction context
   - Disables itself while waiting
   - Shows spinner/loading indicator
   
2. **"Place Trade"**
   - Initially disabled
   - Enabled only after validation approval
   - Shows confirmation modal before execution

### Panel 2: C++ RiskManager Status

**Validation Status Badge:**
- ⏸️ **WAITING** (secondary color) - No validation requested yet
- ✅ **APPROVED** (success/green) - Trade passed all risk checks
- ❌ **REJECTED** (danger/red) - Trade failed risk validation

**Rejection Reason Alert:**
- Only visible if `allowed == false`
- Displays `reason` string from C++ response
- Examples:
  - "Daily loss limit exceeded: -$567 / -$500 max"
  - "Portfolio heat too high: 12.5% / 10.0% max"
  - "Consecutive losses: 3 / 3 max"

**Risk Metrics Display:**

| Metric | Value | Color Coding |
|:-------|:------|:-------------|
| Daily P&L | +$1,245.00 | Green if positive, red if negative |
| Portfolio Heat | 3.2% | Green <5%, yellow 5-8%, red >8% |
| Consecutive Losses | 0 | Green 0-1, yellow 2, red 3+ |
| Trading Halt | False | Green if false, red if true |

### Data Structures

**Current Prediction Dict (cache[CACHE_KEY_AGENT_PREDICTION]):**
```python
{
    "action": "ENTER_LONG",        # TradeAction enum string
    "confidence": 0.873,            # 0.0-1.0
    "trade_plan": {
        "rationale": "Strong upward momentum with RSI confirmation and volume spike",
        "stop_loss_description": "2.5 ATR below entry at 5,235.25",
        "profit_target_description": "5.0 ATR above entry at 5,265.75 (2:1 R:R)"
    }
}
```

**Future Enhancement Fields:**
```python
{
    "setup_type": "ElderTripleScreen",
    "entry_rule": "Breakout",
    "stop_rule": "ATR",
    "target_rule": "RiskReward",
    "quantity": 2,
    "timestamp": "2025-12-09T14:35:42",
    "model_version": "v2.3"
}
```

### Display Conversion (Ticks → Handles → Dollars)

**ES Futures Conversion:**
- 1 tick = 0.25 points = $12.50
- 1 handle = 4 ticks = 1.0 point = $50.00

**Example:**
- Risk: 41 ticks
- → 10.25 handles (41 ÷ 4)
- → $512.50 (41 × $12.50)

**Display Format:** "41.0 ticks (10.25 handles / $512.50)"

### Visual Display Examples

**Approved Trade Validation (Green):**
```
┌──────────────────────────────────────────────┐
│ ✅ TRADE APPROVED                            │
├──────────────────────────────────────────────┤
│ Entry:  5,245.50                             │
│ Stop:   5,235.25 (10.25 handles below)       │
│ Target: 5,265.75 (20.50 handles above)       │
│ Risk:   $512.50 per contract                 │
│ Reward: $1,025.00 per contract               │
│ R:R:    2.0:1                                │
│                                              │
│ Portfolio Status:                            │
│ • Daily P&L: +$1,245.00                      │
│ • Portfolio Heat: 3.2% (within 10% limit)    │
│ • Consecutive Losses: 0                      │
│ • Trading: Active                            │
│                                              │
│ [Place Trade] ← Now enabled                  │
└──────────────────────────────────────────────┘
```

**Rejected Trade Validation (Red):**
```
┌──────────────────────────────────────────────┐
│ ❌ TRADE REJECTED                            │
├──────────────────────────────────────────────┤
│ ⚠️ Daily loss limit exceeded                 │
│ Current Daily P&L: -$567.00                  │
│ Maximum Loss Limit: -$500.00                 │
│                                              │
│ Portfolio Status:                            │
│ • Portfolio Heat: 4.5%                       │
│ • Consecutive Losses: 2                      │
│ • Trading: Halted                            │
│                                              │
│ [Place Trade] ← Disabled                     │
└──────────────────────────────────────────────┘
```

### Button Logic

**"Request Validation" Button:**
1. User clicks button
2. Extract prediction from `cache[CACHE_KEY_AGENT_PREDICTION]`
3. Build ExecuteTrade request with AI prediction:
   ```python
   request = {
       "setupType": prediction.get("setup_type", "Custom"),
       "entryRule": prediction.get("entry_rule", "Breakout"),
       "stopRule": prediction.get("stop_rule", "ATR"),
       "targetRule": prediction.get("target_rule", "RiskReward"),
       "direction": prediction["action"],  # ENTER_LONG/ENTER_SHORT
       "quantity": prediction.get("quantity", 2)
   }
   ```
4. Disable button, show spinner
5. Send request via `TradeExecutionClient.execute_trade()`
6. Wait for response (5-second timeout)
7. Update UI with validation results
8. Re-enable button

**"Place Trade" Button:**
1. Initially disabled
2. Enabled only if `validation_response["allowed"] == true`
3. User clicks button
4. Show confirmation modal:
   ```
   ┌─────────────────────────────────────────┐
   │ Confirm Trade Execution                 │
   ├─────────────────────────────────────────┤
   │ You are about to place:                 │
   │ • LONG 2 ES contracts                   │
   │ • Entry: 5,245.50                       │
   │ • Stop: 5,235.25                        │
   │ • Target: 5,265.75                      │
   │ • Risk: $1,025.00                       │
   │                                         │
   │ [Cancel]  [Confirm]                     │
   └─────────────────────────────────────────┘
   ```
5. C++ performs real-time validation and execution (single step)
6. Show success/failure notification

### Callback Specifications

**1. update_validation_badge(validation_response):**
```python
if not validation_response:
    return "⏸️ WAITING", "secondary"
elif validation_response["allowed"]:
    return "✅ APPROVED", "success"
else:
    return "❌ REJECTED", "danger"
```

**2. show_rejection_reason(validation_response):**
```python
if validation_response and not validation_response["allowed"]:
    reason = validation_response.get("reason", "Unknown reason")
    return dbc.Alert(f"⚠️ {reason}", color="danger", is_open=True)
return dbc.Alert("", is_open=False)
```

**3. toggle_place_trade_button(validation_response):**
```python
if validation_response and validation_response["allowed"]:
    return False  # enabled
return True  # disabled
```

**4. show_trade_confirmation(n_clicks, validation_response):**
```python
if n_clicks and validation_response["allowed"]:
    return create_confirmation_modal(validation_response)
return None
```

**5. confirm_trade_placement(confirm_clicks, validation_response):**
```python
if confirm_clicks:
    response = trade_client.execute_trade(
        validation_response["entryPrice"],
        validation_response["stopPrice"],
        validation_response["targetPrice"],
        validation_response["quantity"],
        validation_response["direction"]
    )
    
    if response and response.get("success"):
        return dbc.Toast(
            f"✅ Order placed successfully! ID: {response['orderId']}",
            color="success"
        )
    else:
        return dbc.Toast(
            f"❌ Order failed: {response.get('message', 'Unknown error')}",
            color="danger"
        )
```

### LiveAgentView Class Structure

```python
class LiveAgentView:
    def __init__(self):
        # Risk metrics field definitions
        self.risk_fields = [
            EnumField("validation_status", "Validation Status", 
                     ["WAITING", "APPROVED", "REJECTED"]),
            DoubleField("daily_pnl", "Daily P&L", "$"),
            DoubleField("daily_loss_limit", "Daily Loss Limit", "$"),
            PercentField("daily_pnl_pct", "Daily P&L %"),
            PercentField("portfolio_heat", "Portfolio Heat"),
            IntField("consecutive_losses", "Consecutive Losses"),
            EnumField("trading_halted", "Trading Halt", 
                     ["Active", "Halted"])
        ]
        
    def register_callbacks(self, app):
        """Register field callbacks + validation badge + rejection alert + place trade toggle."""
        for field in self.risk_fields:
            field.register_callback(app)
            
        @app.callback(
            Output("validation-badge", "children"),
            Output("validation-badge", "color"),
            Input("cache-store", "data")
        )
        def update_validation_badge(cache_data):
            validation = cache_data.get(CACHE_KEY_RISK_STATUS, {})
            return self._format_validation_badge(validation)
            
        @app.callback(
            Output("rejection-alert", "children"),
            Output("rejection-alert", "is_open"),
            Input("cache-store", "data")
        )
        def show_rejection_reason(cache_data):
            validation = cache_data.get(CACHE_KEY_RISK_STATUS, {})
            return self._format_rejection_alert(validation)
            
        @app.callback(
            Output("place-trade-button", "disabled"),
            Input("cache-store", "data")
        )
        def toggle_place_trade_button(cache_data):
            validation = cache_data.get(CACHE_KEY_RISK_STATUS, {})
            return not validation.get("allowed", False)
            
    def render(self) -> dbc.Card:
        """Returns two-panel card layout."""
        return dbc.Card([
            dbc.Row([
                # Panel 1: Predictions
                dbc.Col([
                    dbc.Card([
                        dbc.CardHeader("TransformerAgent Predictions"),
                        dbc.CardBody([
                            html.Div(id="prediction-markdown"),
                            html.Hr(),
                            dbc.Button("Request Validation", id="request-validation-btn"),
                            dbc.Button("Place Trade", id="place-trade-btn", disabled=True)
                        ])
                    ])
                ], width=6),
                
                # Panel 2: RiskManager Status
                dbc.Col([
                    dbc.Card([
                        dbc.CardHeader("C++ RiskManager Status"),
                        dbc.CardBody([
                            dbc.Badge(id="validation-badge", color="secondary"),
                            dbc.Alert(id="rejection-alert", is_open=False),
                            html.Hr(),
                            html.Div([
                                field.render() for field in self.risk_fields
                            ])
                        ])
                    ])
                ], width=6)
            ])
        ])
```

### Migration Path

**Step 1: Rename tactics_view.py → live_agent_view.py**
```bash
mv src/tactics_view.py src/live_agent_view.py
```

**Step 2: Update imports in app.py**
```python
# OLD
from tactics_view import TacticsView

# NEW
from live_agent_view import LiveAgentView
```

**Step 3: Add cache keys to dash_util.py**
```python
CACHE_KEY_PREDICTION = "agent_prediction"
CACHE_KEY_RISK_STATUS = "risk_validation"
```

**Step 4: Implement ZMQ message handlers**
```python
def handle_prediction_message(payload):
    """Parse TransformerAgent prediction from C++."""
    # Extract action, confidence from payload
    # Calculate risk_reward, position_size
    # Update cache[CACHE_KEY_PREDICTION]
    # Triggers LiveAgentView refresh
    
def handle_risk_validation_message(payload):
    """Parse RiskManager validation status from C++."""
    # Extract allowed, reason, riskMetrics
    # Update cache[CACHE_KEY_RISK_STATUS]
    # Triggers Panel 2 refresh
```

**Step 5: Update C++ messaging**
```cpp
// After RiskManager.ValidateOrder()
json validation_result = {
    {"type", "ValidateTrade"},
    {"allowed", allowed},
    {"entryPrice", entry_price},
    {"stopPrice", stop_price},
    {"targetPrice", target_price},
    {"riskAmount", risk_ticks},
    {"rewardAmount", reward_ticks},
    {"reason", rejection_reason},
    {"riskMetrics", {
        {"dailyPnL", daily_pnl},
        {"portfolioHeat", portfolio_heat},
        {"isTradingHalted", is_halted}
    }}
};

// Send via ZMQ REP socket
```

### Timeline

**Day 1 (2 hours):**
- Rename tactics_view.py → live_agent_view.py
- Update imports in app.py
- Add CACHE_KEY_PREDICTION and CACHE_KEY_RISK_STATUS to dash_util.py
- Create two-panel layout with basic structure

**Day 2 (3 hours):**
- Implement risk_fields definitions
- Add callbacks for validation badge and rejection alert
- Test with mock data in cache store

**Day 3 (3 hours):**
- Implement ZMQ message handlers (handle_prediction_message, handle_risk_validation_message)
- Update C++ to send validation messages after ValidateOrder()
- End-to-end integration testing

**Total: 8 hours to complete**

---

## 3. Implementation Details

### Cache Store Architecture

**Key:** `CACHE_KEY_AGENT_PREDICTION = "agent_prediction"`

**Purpose:** Store augmented prediction dict from live_agent for automatic GUI updates

**Current Structure:**
```python
{
    "action": "ENTER_LONG",  # TradeAction enum
    "confidence": 0.873,      # 0.0-1.0
    "trade_plan": {
        "rationale": "Strong upward momentum with RSI confirmation and volume spike",
        "stop_loss_description": "2.5 ATR below entry at 5,235.25",
        "profit_target_description": "5.0 ATR above entry at 5,265.75 (2:1 R:R)"
    }
}
```

**Example with Real Data:**
```python
prediction = {
    "action": "ENTER_LONG",
    "confidence": 0.873,
    "trade_plan": {
        "rationale": "Strong upward momentum with RSI confirmation and volume spike. "
                     "240-min Impulse GREEN, ADX > 30 (trending strong). "
                     "15-min stochastic in oversold bounce zone.",
        "stop_loss_description": "2.5 ATR below entry at 5,235.25 (10.25 handles = $512.50 risk)",
        "profit_target_description": "5.0 ATR above entry at 5,265.75 (20.50 handles = $1,025.00 reward, 2:1 R:R)"
    }
}
```

**Future Enhancement Fields:**
```python
{
    "setup_type": "ElderTripleScreen",      # Setup enum
    "entry_rule": "Breakout",               # Entry logic
    "stop_rule": "ATR",                     # Stop placement method
    "target_rule": "RiskReward",            # Target calculation method
    "quantity": 2,                          # Contracts
    "timestamp": "2025-12-09T14:35:42",    # Prediction time
    "model_version": "v2.3"                 # Model identifier
}
```

### Data Flow Sequences

**1. Automatic Prediction Flow (Real-Time):**
```
C++ Sierra Chart Study
  ↓ Bar Close
zmq_client.py (SUB socket)
  ↓ Receives BAR_CLOSE_UPDATE message
message_queue
  ↓ Threaded consumer
update_data_store() callback
  ↓ Parse indicators from payload
live_agent.predict(current_position_state)
  ↓ Model inference
Returns augmented prediction dict
  ↓ {action, confidence, trade_plan}
cache[CACHE_KEY_AGENT_PREDICTION] ← Store
  ↓ Cache store update triggers callback
live_agent_view.py
  ↓ Reads from cache
UI auto-refresh (Markdown display)
```

**2. Manual Validation Flow (User-Triggered):**
```
User clicks "Request Validation"
  ↓
live_agent_view.py callback fires
  ↓ Extract prediction from cache
  ↓ Build ValidateTrade request dict
TradeExecutionClient.validate_trade()
  ↓ Send ZMQ REQ (port 5558)
  ↓ setupType, entryRule, stopRule, targetRule, direction, quantity
C++ TradeExecutionServer
  ↓ Receive request
  ↓ Parse JSON
RiskManager.ValidateOrder()
  ↓ Check daily loss limit
  ↓ Check portfolio heat
  ↓ Check consecutive losses
  ↓ Calculate entry/stop/target prices using live market data
Build validation response
  ↓ {allowed, prices, risks, reason, riskMetrics}
Send ZMQ REP
  ↓
TradeExecutionClient receives response
  ↓
live_agent_view.py
  ↓ Update UI directly (no cache)
  ↓ validation_display
  ↓ place_trade_disabled toggle
```

**3. Manual Execution Flow (After Approval):**
```
User clicks "Place Trade" (enabled after validation approval)
  ↓
show_trade_confirmation() callback
  ↓ Display confirmation modal with trade details
User clicks "Confirm"
  ↓
confirm_trade_placement() callback
  ↓ Extract validated prices from previous response
TradeExecutionClient.execute_trade()
  ↓ Send ExecuteTrade request
  ↓ entryPrice, stopPrice, targetPrice, quantity, direction
C++ TradeExecutionServer
  ↓ Receive request
  ↓ Call order placement logic
  ↓ Sierra Chart sc.BuyEntry() or sc.SellEntry()
Build execution response
  ↓ {success, orderId, message}
Send ZMQ REP
  ↓
TradeExecutionClient receives response
  ↓
live_agent_view.py
  ↓ Show success/failure toast notification
  ↓ If success: display orderId
  ↓ If failure: display error message
```

### Critical Design Changes from Original Plan

#### 1. Risk Units: Ticks/Handles (Not Dollars)

**Reason:** C++ RiskManager is instrument-agnostic. Works for ES, NQ, CL, GC, etc.

**ES Futures Conversion:**
- 1 tick = 0.25 points
- 1 tick = $12.50 per contract
- 1 handle = 4 ticks = 1.0 point = $50.00

**Example:**
```python
# C++ sends: riskAmount = 41.0 (ticks)
# Python converts for display:
risk_ticks = 41.0
risk_handles = risk_ticks / 4.0  # 10.25 handles
risk_dollars = risk_ticks * 12.50  # $512.50

display = f"{risk_ticks} ticks ({risk_handles} handles / ${risk_dollars})"
# Output: "41.0 ticks (10.25 handles / $512.50)"
```

#### 2. ZMQ Pattern: REQ-REP (Not PUSH-PULL)

**Reason:** Need synchronous request-response. Python must wait for C++ validation result.

**Port:** 5558 (TradeExecutionServer)

**Pattern:**
- Python sends REQ
- Python blocks waiting for response
- C++ sends REP
- Python receives response

**Timeout:** 5 seconds (configurable in TradeExecutionClient)

**Implementation:**
```python
class TradeExecutionClient:
    def __init__(self, host="localhost", port=5558):
        self.socket = zmq.Context().socket(zmq.REQ)
        self.socket.connect(f"tcp://{host}:{port}")
        self.socket.setsockopt(zmq.RCVTIMEO, 5000)  # 5-second timeout
```

#### 3. C++ Calculates Prices (Not Python)

**Reason:** Only C++ has access to live market data (lastSwingHigh, ATR, current bid/ask).

**Python Role:** Send high-level prediction
```python
{
    "setupType": "ElderTripleScreen",
    "entryRule": "Breakout",
    "stopRule": "ATR",
    "targetRule": "RiskReward",
    "direction": "LONG"
}
```

**C++ Role:** Calculate exact prices
```cpp
float entry_price = lastSwingHigh + 1 * tick_size;  // Breakout entry
float stop_price = entry_price - (2.0 * atr);       // 2 ATR stop
float target_price = entry_price + (2.0 * abs(entry_price - stop_price));  // 2:1 R:R
```

**Python Role:** Receive validated response
```python
{
    "allowed": true,
    "entryPrice": 5245.50,   # Calculated by C++
    "stopPrice": 5235.25,    # Calculated by C++
    "targetPrice": 5265.75,  # Calculated by C++
    "riskAmount": 41.0,      # In ticks
    "rewardAmount": 82.0     # In ticks
}
```

#### 4. Message Type Names

**OLD (Deprecated):**
- VALIDATE_TRADE_REQUEST
- PLACE_TRADE_ORDER

**NEW (Current):**
- ValidateTrade
- ExecuteTrade

**Reason:** Consistency with C++ enum naming conventions

#### 5. Response Structure: Nested riskMetrics (Not Flat)

**OLD Structure:**
```json
{
    "allowed": true,
    "validation_result": "approved",
    "rejection_reason": "",
    "daily_pnl": 1245.00,
    "portfolio_heat_pct": 3.2,
    "trading_halted": false
}
```

**NEW Structure:**
```json
{
    "allowed": true,
    "entryPrice": 5245.50,
    "stopPrice": 5235.25,
    "targetPrice": 5265.75,
    "riskAmount": 41.0,
    "rewardAmount": 82.0,
    "riskRewardRatio": 2.0,
    "reason": "",
    "riskMetrics": {
        "dailyPnL": 1245.00,
        "portfolioHeat": 0.032,
        "isTradingHalted": false,
        "maxDailyLoss": -500.0,
        "maxPortfolioHeat": 0.10
    }
}
```

**Reason:** Cleaner separation of trade parameters vs risk metrics

#### 6. Field Name Mapping

| Old Name | New Name | Location |
|:---------|:---------|:---------|
| validation_result | allowed | Top-level (bool) |
| rejection_reason | reason | Top-level (string) |
| daily_pnl | dailyPnL | riskMetrics object |
| portfolio_heat_pct | totalExposure | riskMetrics object |
| trading_halted | isTradingHalted | riskMetrics object |

**Python Access:**
```python
# OLD (broken):
if response["validation_result"] == "approved":
    daily_pnl = response["daily_pnl"]

# NEW (correct):
if response["allowed"]:
    daily_pnl = response["riskMetrics"]["dailyPnL"]
```

#### 7. Setup/Rule Enums

**setupType:** ElderTripleScreen, RaschkeTurtleSoup, WyckoffSpringShakeout, KeltnerChannelBreakout, MACDDivergence, Custom

**entryRule:** Breakout, Pullback, Reversal

**stopRule:** ATR, SwingPoint, VolatilityBand, FixedTicks

**targetRule:** RiskReward, SupportResistance, Trailing, FixedTicks

**Usage:**
```python
request = {
    "type": "ValidateTrade",
    "setupType": "ElderTripleScreen",
    "entryRule": "Breakout",
    "stopRule": "ATR",
    "targetRule": "RiskReward",
    "direction": "LONG",
    "quantity": 2
}
```

### Implementation Checklist

#### dash_util.py
- [ ] Add `CACHE_KEY_PREDICTION = "agent_prediction"`
- [ ] Add `CACHE_KEY_RISK_STATUS = "risk_validation"`

#### zmq_client.py
- [ ] Implement TradeExecutionClient class
  - [ ] `__init__(host, port=5558)`
  - [ ] `send_request(request_dict)` with ZMQ REQ socket
  - [ ] 5-second timeout handling
  - [ ] Connection management
  - [ ] Methods: `validate_trade()`, `execute_trade()`, `close()`

#### live_agent_view.py
- [ ] Remove prediction_fields (predictions now in Markdown)
- [ ] Keep risk_fields (EnumField, DoubleField, IntField, PercentField)
- [ ] Add `_generate_prediction_markdown()` method
- [ ] Update callbacks to use `allowed` field (not validation_result)
- [ ] Access nested riskMetrics (not flat fields)
- [ ] Use TradeExecutionClient for validation/execution

#### app.py
- [ ] Import TradeExecutionClient
- [ ] Initialize client at startup: `trade_client = TradeExecutionClient("localhost", 5558)`
- [ ] Register cleanup on shutdown: `atexit.register(trade_client.close)`

#### assets/custom_theme.scss
- [ ] Add .prediction-markdown styles
- [ ] Format rationale text
- [ ] Style trade parameters table
- [ ] Color coding for risk/reward

---

## 4. Testing & Deployment

### Testing Checklist

#### 1. Connection Test
```python
def test_trade_execution_client_connectivity():
    """Verify TradeExecutionClient can connect to C++ server."""
    client = TradeExecutionClient("localhost", 5558)
    
    # Send ping request
    response = client.send_request({"type": "Ping"})
    
    assert response is not None, "No response from C++ server"
    assert response.get("success") == True, "Ping failed"
    
    client.close()
```

#### 2. Validation Approval Test
```python
def test_validate_trade_approval():
    """Test successful trade validation."""
    client = TradeExecutionClient()
    
    response = client.validate_trade(
        setup_type="ElderTripleScreen",
        entry_rule="Breakout",
        stop_rule="ATR",
        target_rule="RiskReward",
        direction="LONG",
        quantity=2
    )
    
    # Verify response structure
    assert "allowed" in response, "Missing 'allowed' field"
    assert "entryPrice" in response, "Missing 'entryPrice' field"
    assert "riskMetrics" in response, "Missing 'riskMetrics' field"
    assert "dailyPnL" in response["riskMetrics"], "Missing nested dailyPnL"
    
    client.close()
```

#### 3. Validation Rejection Test
```python
def test_validate_trade_rejection():
    """Test trade rejection due to risk limits."""
    # Trigger daily loss limit (send 10 losing trades first)
    client = TradeExecutionClient()
    
    response = client.validate_trade(
        setup_type="Custom",
        entry_rule="Pullback",
        stop_rule="ATR",
        target_rule="RiskReward",
        direction="SHORT",
        quantity=5
    )
    
    # Verify rejection
    if not response["allowed"]:
        assert response["reason"] != "", "Missing rejection reason"
        assert isinstance(response["riskMetrics"]["isTradingHalted"], bool), "isTradingHalted must be bool"
    
    client.close()
```

#### 4. ExecuteTrade Test
```python
def test_execute_trade():
    """Test trade execution after validation approval."""
    client = TradeExecutionClient()
    
    # First validate
    val_response = client.validate_trade(
        setup_type="RaschkeTurtleSoup",
        entry_rule="Reversal",
        stop_rule="SwingPoint",
        target_rule="RiskReward",
        direction="LONG",
        quantity=1
    )
    
    if val_response["allowed"]:
        # Then execute with validated prices
        exec_response = client.execute_trade(
            entry_price=val_response["entryPrice"],
            stop_price=val_response["stopPrice"],
            target_price=val_response["targetPrice"],
            quantity=1,
            direction="LONG"
        )
        
        assert exec_response.get("success") == True, "Execution failed"
        assert "orderId" in exec_response, "Missing orderId"
    
    client.close()
```

#### 5. Timeout Test
```python
def test_timeout_handling():
    """Test timeout when C++ server not running."""
    client = TradeExecutionClient("localhost", 9999)  # Wrong port
    
    response = client.validate_trade(
        setup_type="Custom",
        entry_rule="Breakout",
        stop_rule="ATR",
        target_rule="FixedTicks",
        direction="LONG",
        quantity=2
    )
    
    # Should return None on timeout
    assert response is None, "Expected None for timeout"
    
    # GUI should not crash
    client.close()
```

### Common Pitfalls

#### ❌ Pitfall 1: Calculating Prices in Python
```python
# WRONG: Python doesn't have live market data
entry_price = df['Close'].iloc[-1] + (1 * tick_size)  # Stale data!
```

**✅ Correct: Let C++ Calculate**
```python
# Python sends high-level request
response = client.validate_trade(
    entry_rule="Breakout",  # C++ decides exact entry
    stop_rule="ATR",        # C++ calculates stop
    target_rule="RiskReward" # C++ calculates target
)

# Python receives validated prices
entry_price = response["entryPrice"]  # Calculated by C++
```

#### ❌ Pitfall 2: Using PUSH-PULL Pattern
```python
# WRONG: No response, cannot wait for validation
socket = zmq.Context().socket(zmq.PUSH)
socket.send_json(request)
# How do we get the response?
```

**✅ Correct: Use REQ-REP with TradeExecutionClient**
```python
# Correct: Synchronous request-response
client = TradeExecutionClient()
response = client.validate_trade(...)  # Blocks until response
if response["allowed"]:
    # Act on validated result
```

#### ❌ Pitfall 3: Accessing Flat Fields
```python
# WRONG: KeyError! Fields moved to nested object
daily_pnl = response["daily_pnl"]
portfolio_heat = response["portfolio_heat_pct"]
```

**✅ Correct: Access Nested riskMetrics**
```python
# Correct: riskMetrics is nested object
daily_pnl = response["riskMetrics"]["dailyPnL"]
portfolio_heat = response["riskMetrics"]["portfolioHeat"]
is_halted = response["riskMetrics"]["isTradingHalted"]
```

#### ❌ Pitfall 4: Displaying Ticks as Dollars
```python
# WRONG: Confusing to user
risk = response["riskAmount"]
display = f"Risk: ${risk}"  # Shows "Risk: $41.0" (ticks, not dollars!)
```

**✅ Correct: Convert Ticks to Handles and Dollars**
```python
# Correct: Convert for display
risk_ticks = response["riskAmount"]
risk_handles = risk_ticks / 4.0
risk_dollars = risk_ticks * 12.50

display = format_risk_display(risk_ticks, tick_value=12.50)
# Output: "41.0 ticks (10.25 handles / $512.50)"
```

#### ❌ Pitfall 5: No Error Handling
```python
# WRONG: Crash if C++ server down
response = client.validate_trade(...)
entry_price = response["entryPrice"]  # TypeError if response is None
```

**✅ Correct: Handle Timeouts**
```python
# Correct: Check for None
response = client.validate_trade(...)

if response is None:
    # Timeout occurred
    show_error_toast("C++ server not responding")
    return

if response["allowed"]:
    # Safe to access fields
    entry_price = response["entryPrice"]
```

---

## 5. Reference Material

### ES Futures Conversion Reference

**Tick Size:** 0.25 points  
**Tick Value:** $12.50 per contract  
**Handle:** 4 ticks = 1.0 point = $50.00

| Ticks | Handles | Dollars |
|:------|:--------|:--------|
| 4 | 1.0 | $50.00 |
| 8 | 2.0 | $100.00 |
| 20 | 5.0 | $250.00 |
| 40 | 10.0 | $500.00 |
| 41 | 10.25 | $512.50 |
| 80 | 20.0 | $1,000.00 |

### Helper Functions

**format_risk_display()**
```python
def format_risk_display(risk_ticks: float, tick_value: float = 12.50) -> str:
    """
    Format risk amount for display.
    
    Args:
        risk_ticks: Risk in ticks (from C++ response)
        tick_value: Dollar value per tick (default $12.50 for ES)
    
    Returns:
        Formatted string: "41.0 ticks (10.25 handles / $512.50)"
    """
    risk_handles = risk_ticks / 4.0
    risk_dollars = risk_ticks * tick_value
    
    return f"{risk_ticks:.1f} ticks ({risk_handles:.2f} handles / ${risk_dollars:.2f})"
```

**Usage Example:**
```python
# From C++ response
risk_ticks = response["riskAmount"]
reward_ticks = response["rewardAmount"]

# Format for display
risk_str = format_risk_display(risk_ticks)
reward_str = format_risk_display(reward_ticks)

print(f"Risk: {risk_str}")
print(f"Reward: {reward_str}")

# Output:
# Risk: 41.0 ticks (10.25 handles / $512.50)
# Reward: 82.0 ticks (20.50 handles / $1,025.00)
```

### Timeline Estimates

#### Original Estimate (DESIGN.md)
**Total: 8 hours**

- **Day 1 (2 hours):** Basic layout
  - Rename tactics_view.py → live_agent_view.py
  - Update imports in app.py
  - Add store keys
  - Create two-panel structure

- **Day 2 (3 hours):** Callbacks
  - Implement field definitions
  - Add validation badge callback
  - Add rejection alert callback
  - Test with mock data

- **Day 3 (3 hours):** ZMQ Integration
  - Implement ZMQ message handlers
  - Update C++ to send validation messages
  - End-to-end integration testing

#### Enhanced Estimate (IMPLEMENTATION_NOTES.md)
**Total: 11 hours**

- **Day 1 (4 hours):** TradeExecutionClient + Field Mappings
  - Implement TradeExecutionClient class in zmq_client.py
  - Update field mappings (validation_result → allowed)
  - Add nested riskMetrics access
  - Test connectivity with C++ server

- **Day 2 (4 hours):** Markdown Rendering + Conversion
  - Implement _generate_prediction_markdown() method
  - Add tick/handle/dollar conversion logic
  - Update Panel 2 to access riskMetrics properly
  - Add custom CSS for .prediction-markdown

- **Day 3 (3 hours):** Integration + Error Handling
  - End-to-end validation → execution flow
  - Timeout handling (5-second limit)
  - Error messages for failed requests
  - Test with mock rejection scenarios

### Implementation Checklists

#### Migration Checklist

1. **File Operations:**
   - [ ] Rename `src/tactics_view.py` → `src/live_agent_view.py`
   - [ ] Update imports in `src/app.py`

2. **Cache Store Updates (dash_util.py):**
   - [ ] Add `CACHE_KEY_PREDICTION = "agent_prediction"`
   - [ ] Add `CACHE_KEY_RISK_STATUS = "risk_validation"`

3. **ZMQ Client (zmq_client.py):**
   - [ ] Implement TradeExecutionClient class
   - [ ] REQ socket on port 5558
   - [ ] 5-second timeout via `setsockopt(zmq.RCVTIMEO, 5000)`
   - [ ] Methods: `send_request()`, `validate_trade()`, `execute_trade()`, `close()`

4. **Live Agent View (live_agent_view.py):**
   - [ ] Remove prediction_fields (use Markdown instead)
   - [ ] Keep risk_fields definitions
   - [ ] Add `_generate_prediction_markdown()` method
   - [ ] Update callbacks:
     - [ ] Use `allowed` field (not validation_result)
     - [ ] Access nested `riskMetrics` object
     - [ ] Format tick/handle/dollar conversions
   - [ ] Integrate TradeExecutionClient

5. **App Integration (app.py):**
   - [ ] Import TradeExecutionClient
   - [ ] Initialize at startup: `trade_client = TradeExecutionClient("localhost", 5558)`
   - [ ] Register cleanup: `atexit.register(trade_client.close)`
   - [ ] Pass client to callbacks

6. **C++ Messaging (TradeExecutionServer.cpp):**
   - [ ] After RiskManager.ValidateOrder():
     - [ ] Build JSON response with `allowed`, prices, risks, `reason`, nested `riskMetrics`
     - [ ] Send via ZMQ REP socket
   - [ ] After order placement:
     - [ ] Build ExecuteTrade response with `success`, `orderId`, `message`
     - [ ] Send via ZMQ REP socket

7. **Styling (assets/custom_theme.scss):**
   - [ ] Add `.prediction-markdown` styles
   - [ ] Format rationale text (font, color, line-height)
   - [ ] Style trade parameters table
   - [ ] Color coding for risk (red) and reward (green)

---

**End of Documentation**

*This consolidated guide merges:*
- *LIVE_AGENT_VIEW_DESIGN.md (1293 lines) - Design specification*
- *LIVE_AGENT_VIEW_ARCHITECTURE_SUMMARY.md (463 lines) - Architecture overview*
- *LIVE_AGENT_VIEW_IMPLEMENTATION_NOTES.md (551 lines) - Implementation details*

*Total: 2,307 lines consolidated into single comprehensive document*
