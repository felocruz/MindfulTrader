# Python Performance Attribution Engine Specification

## Document Purpose
This specification defines the Python-based Performance Attribution Engine that monitors AI model trading performance in real-time and generates health status updates for the C++ trading engine. This is the Python counterpart to `AI_MODEL_HEALTH_STATUS_REQUIREMENTS.md`.

**Status:** ✅ Phase 1 Implementation COMPLETE  
**Last Updated:** December 21, 2025  
**Owner:** Python Trading Analytics Team  
**Implementation:** Fully operational - C++ receives model health with every trade close

**Recent Updates (December 21, 2025):**
- ✅ Added RaschkeTacticalTrigger enum entries 15-18 (RSI Failure Swing, Stochastic Pop patterns)
- ✅ Data processor optimized to use pure Polars (removed pandas conversion)
- ✅ Pattern scoring rules added for all 18 tactical triggers
- ✅ Training pipeline verified with enum consistency checks
- ✅ LiveAgent now loads ExpectedPerformance metadata from companion JSON file
- ✅ Model files organized in src/ directory (best_agent_model.keras + metadata.json)
- ✅ Model config embedded in .keras file (vocab_size, architecture params)

---

## 🎯 Quick Reference - What's Implemented

| Component | File | Status | Details |
|-----------|------|--------|---------|
| **Phase 1 Fields** | firestore_manager.py:267-269 | ✅ COMPLETE | confidence, mae_ticks, mfe_ticks (defaults=0.0) |
| **Metrics Calculator** | src/model_health.py:82-258 | ✅ COMPLETE | Sharpe, Sortino, drawdown, profit factor |
| **Health Manager** | src/model_health.py:264-480 | ✅ COMPLETE | 30-day window, 20-trade min, 60s cache |
| **TradeServer Integration** | trade_server.py | ✅ COMPLETE | Returns model_health in CloseTrade response |
| **Configuration** | config.py:114-122 | ✅ COMPLETE | PERFORMANCE_ATTRIBUTION_CONFIG |
| **Testing** | test_phase1_complete.py | ✅ VALIDATED | All 6 test suites passing |

**What C++ Gets Now:**
```json
{
  "status": "success",
  "type": "CloseTradeResponse",
  "model_health": {
    "status": "HEALTHY|WARNING|SOFT_LOCKED|INSUFFICIENT_DATA",
    "alpha_slippage_pct": 0.0,
    "message": "..."
  }
}
```

**Health Status Logic:**
- `INSUFFICIENT_DATA`: < 20 closed trades in 30-day window
- `HEALTHY`: Alpha slippage ≤ 20% (model performing as expected)
- `WARNING`: Alpha slippage > 20% but ≤ 30% (degrading performance)
- `SOFT_LOCKED`: Alpha slippage > 30% (significant underperformance)

**Key Metrics:**
- Alpha Slippage = ((expected_sharpe - actual_sharpe) / expected_sharpe) × 100
- Rolling Window = 30 calendar days
- Min Sample Size = 20 trades (Mark Douglas: statistically significant)
- Cache Duration = 60 seconds (reduces Firestore query overhead)

---

## C++ Integration Status

**C++ Integration Status:** ✅ **COMPLETE** (2025-12-21)
- All required Trade class extensions implemented (MAE/MFE/confidence)
- Data flow validated: C++ → TradeServer → Firestore → Python
- Python Performance Attribution Engine fully operational

**Python Implementation Status:** ✅ **PHASE 1 COMPLETE** (2025-12-21)
- TradeDocument extended with Phase 1 fields (confidence, mae_ticks, mfe_ticks)
- src/model_health.py created with full calculator and health manager
- TradeServer integration complete - returns health status in CloseTrade responses
- All components tested and validated

---

## 🎉 Implementation Readiness Summary (2025-12-21)

### ✅ C++ Side: COMPLETE
All prerequisites for performance attribution are now implemented in the C++ trading engine:

| Component | Status | Location |
|-----------|--------|----------|
| **MAE Tracking** | ✅ Implemented | `Trade::Update()` continuously tracks maximum adverse excursion |
| **MFE Tracking** | ✅ Implemented | `Trade::Update()` continuously tracks maximum favorable excursion |
| **Confidence Storage** | ✅ Implemented | `Trade::SetConfidence()` stores AI signal confidence at entry |
| **ZMQ Transmission** | ✅ Implemented | `Trade::CreateRequestJson()` includes confidence/MAE/MFE |
| **Data Flow** | ✅ Validated | TransformerAgent → TradeExecutionServer → PositionManager → Trade → TradeServer → Firestore |
| **Python Data Model** | 📝 Ready | Extend existing `TradeDocument` in `firestore_manager.py` with 3 Phase 1 fields |

**What This Means:** Python teams can now proceed with Phase 1 implementation. All required trade data (confidence, MAE, MFE) will be available in Firestore.

### ✅ Python Side: Phase 1 & 2 COMPLETE (2025-12-21)
All Phase 1 and Phase 2 components have been implemented and tested:

| Component | Status | Implementation Details |
|-----------|--------|------------------------|
| **TradeDocument Extension** | ✅ Implemented | firestore_manager.py lines 267-269: Added confidence, mae_ticks, mfe_ticks with defaults=0.0 |
| **PerformanceMetrics Calculator** | ✅ Implemented | src/model_health.py: Full Sharpe, Sortino, drawdown calculations with defensive column handling |
| **ModelHealthStatusManager** | ✅ Implemented | src/model_health.py: Health status determination with 30-day rolling window, 20-trade minimum |
| **TradeServer Integration** | ✅ Implemented | trade_server.py: Returns model_health in CloseTrade responses |
| **Configuration** | ✅ Implemented | config.py: PERFORMANCE_ATTRIBUTION_CONFIG with min_sample_size=20 |
| **GUI Dashboard** | ✅ Implemented | trade_analytics.py: Model Health card with 6 self-updating Field components |
| **Testing** | ✅ Validated | All tests confirm health status correctly integrated in C++ responses and GUI display |

**Implementation Highlights:**
- Defensive coding for backward compatibility with existing trades lacking Phase 1 fields
- 60-second cache in health manager to reduce Firestore query overhead
- Graceful error handling - trade operations succeed even if health calculation fails
- Statistical significance threshold: 20 trades (per Mark Douglas)
- **Phase 2:** Field-based self-updating GUI components using field.py architecture
- **Phase 2:** Model Health card in Trade Analytics tab with real-time updates

### 🚀 Next Steps (Phase 3)
1. **✅ COMPLETED:** Core performance attribution engine (Phase 1)
2. **✅ COMPLETED:** GUI Dashboard Integration (Phase 2) - Model Health card with Field-based self-updating components
3. **→ CURRENT:** Load ExpectedPerformance from .keras metadata instead of hardcoded defaults (Phase 2b)
4. **→ NEXT:** Advanced visualization (equity curves, health history charts) (Phase 3)

### 📖 Quick Start for Python Developers
**If you're implementing this spec, start here:**

1. **Read Section 2.1.1** - See example JSON messages from C++ (OpenTrade/CloseTrade)
2. **Read Section 2.2** - Firestore trade collection strategy (direct queries)
3. **Read Section 3** - Performance metrics calculation (Sharpe, alpha slippage, etc.)
4. **Read Section 4** - ModelHealthStatusManager (core health determination logic)
5. **Read Section 13** - Dashboard integration (if implementing GUI side)

**Key Files (MTS Project) - ✅ IMPLEMENTED:**
- ✅ `src/model_health.py` (480 lines) - Complete performance attribution logic:
  - Dataclasses: PerformanceMetrics, ExpectedPerformance, HealthThresholds
  - PerformanceMetricsCalculator (Sharpe, Sortino, win rate, profit factor, max drawdown)
  - ModelHealthStatusManager (health decision logic, 60-second caching, 30-day window)
- ✅ `firestore_manager.py` - Extended TradeDocument with Phase 1 fields:
  - Lines 267-269: confidence: float = 0.0, mae_ticks: float = 0.0, mfe_ticks: float = 0.0
- ✅ `trade_server.py` - Integrated ModelHealthStatusManager:
  - Initializes health manager in __init__() with default ExpectedPerformance
  - Returns model_health dict in CloseTrade response (status, alpha_slippage_pct, message)
- ✅ `config.py` - Added PERFORMANCE_ATTRIBUTION_CONFIG:
  - rolling_window_days=30, risk_free_rate=0.02, min_sample_size=20
- ✅ `trade_analytics.py` - GUI Dashboard Integration (Phase 2):
  - Lines 110-180: 6 Model Health Field components
  - Lines 334-382: Model Health card with self-updating fields
  - Lines 205-251: on_tab_selected calculates and caches health data
  - Lines 866-925: update_model_health_badge for status badge rendering

**Key Files for TransformerAgent Project (Separate Repo) - SPECIFIED:**
- 📝 `src/backtest_metrics.py` - Backtest performance calculation and metadata file generation:
  - Dataclasses: PerformanceMetrics, ExpectedPerformance
  - PerformanceMetricsCalculator (same formulas as MTS, duplicated code is OK)
  - Function to save ExpectedPerformance to src/best_agent_model_metadata.json after training
- 📝 `src/live_agent.py` - LiveAgent loads model from src/best_agent_model.keras and metadata from companion JSON

**Implementation Time:** ✅ Phase 1 completed in 1 day (2025-12-21)

---

## 📋 v3.0 Architectural Simplification

**Key Change:** Eliminated background daemon and JSON file intermediary. Both C++ and GUI now get health status directly from source.

**Architecture Flow:**
1. **C++ → TradeServer:** Opens/closes trades via ZMQ REQ/REP
2. **TradeServer → Firestore:** Writes trade records
3. **TradeServer → C++:** Returns health status in CloseTrade response (synchronous)
4. **Dashboard → Firestore:** Queries trades on-demand (when tab viewed)
5. **Dashboard → Display:** Calculates and shows health status

**What Was Removed:**
- ❌ Background writer daemon
- ❌ `model_health_status.json` file
- ❌ File polling/watching
- ❌ ModelHealthStatusWriter class

**What Remains (✅ IMPLEMENTED):**
- ✅ ModelHealthStatusManager (calculates health from Firestore data) - src/model_health.py lines 264-480
- ✅ PerformanceMetrics calculator - src/model_health.py lines 82-258
- ✅ Trade data model extended (TradeDocument in firestore_manager.py lines 267-269)
- ✅ TradeServer integration - trade_server.py returns model_health in CloseTrade response
- ✅ Dashboard integration - Phase 2 Complete (Model Health card in Trade Analytics)

---

## 📊 Phase 1 Implementation Status (2025-12-21)

### What's Working Now ✅

**Core Components:**
```python
# src/model_health.py (480 lines)
@dataclass
class PerformanceMetrics:          # Lines 22-50: Complete metrics with metadata
@dataclass  
class ExpectedPerformance:         # Lines 53-66: Baseline from backtest
@dataclass
class HealthThresholds:            # Lines 72-79: Configurable thresholds (min=20 trades)

class PerformanceMetricsCalculator # Lines 82-258: Sharpe, Sortino, drawdown calcs
class ModelHealthStatusManager     # Lines 264-480: Health status determination
```

**TradeServer Integration:**
```python
# trade_server.py - CloseTrade handler returns:
{
  "status": "success",
  "message": "CloseTrade successful for doc_id: ...",
  "type": "CloseTradeResponse",
  "order_id": "...",
  "model_health": {                    # NEW - Phase 1
    "status": "INSUFFICIENT_DATA",     # HEALTHY | WARNING | SOFT_LOCKED | INSUFFICIENT_DATA
    "alpha_slippage_pct": 0.0,
    "message": "Insufficient data: 0 trades (need 20)"
  }
}
```

**Configuration:**
```python
# config.py - PERFORMANCE_ATTRIBUTION_CONFIG
{
    'rolling_window_days': 30,           # 30-day rolling window
    'risk_free_rate': 0.02,              # 2% annual for Sharpe/Sortino
    'starting_capital': 10000.0,         # For drawdown calculations
    'warning_threshold_pct': 20.0,       # WARNING if alpha slippage > 20%
    'soft_lock_threshold_pct': 30.0,     # SOFT_LOCKED if alpha slippage > 30%
    'min_sample_size': 20,               # 20 trades (Mark Douglas: statistically significant)
    'cache_duration_seconds': 60         # Cache health for 60 seconds
}
```

**What C++ Receives:**
Every CloseTrade response now includes real-time model health status. C++ can:
- Display health status in logs
- Adjust trading behavior based on status (WARNING/SOFT_LOCKED)
- Track model degradation over time
- Trigger alerts when health changes

---

## 🎯 Final Architecture Decisions

### Core Design Philosophy

1. **C++ Integration:** Health status returned synchronously in CloseTrade REP response (zero latency)
2. **GUI Integration:** Direct Firestore queries when user views dashboard tab (no file intermediary)
3. **Data Source:** Leverage existing Firestore infrastructure (single source of truth)
4. **Single Authority:** `ModelHealthStatusManager` with 60-second cache in TradeServer
5. **Graceful Degradation:** Health calc errors never break trade close operations

### Key Improvements Over Initial Design

✅ **Eliminated C++ File Polling** - Health status now in ZMQ response payload  
✅ **Simplified GUI Architecture** - Dashboard queries Firestore directly, no background daemon needed  
✅ **Reused Existing Infrastructure** - FirestoreManager, TradeServer already production-ready  
✅ **60-Second Cache** - Avoid recalculating metrics on every single trade  
✅ **Single Source of Truth** - Firestore contains all trade data, no file synchronization

---

## 🎯 Key Design Decision: Leverage Existing Infrastructure

**Instead of building a new data pipeline (CSV/SQLite/ZMQ), this spec leverages the existing production-ready Firestore infrastructure:**

✅ **Existing Components to Reuse:**
- `TradeServer` (`trade_server.py`) - Already receives C++ trade messages via ZMQ
- `FirestoreManager` (`firestore_manager.py`) - Already stores trades in Firestore
- `TradeDocument` schema - Extend existing dataclass with Phase 1 fields (confidence, mae_ticks, mfe_ticks)
- GUI Dashboard (`app.py`) - Already displays trades

✅ **What We Add:**
- Performance metrics calculator (Sharpe, alpha slippage, etc.)
- Health status determination logic (HEALTHY/WARNING/SOFT_LOCKED)
- Health status manager integrated into TradeServer
- Dashboard callback for on-demand health display

✅ **Benefits:**
- No new data pipeline to build/maintain
- Single source of truth for trades
- Production-tested infrastructure
- Ready for GUI integration

---

## 1. System Overview

### Architecture

```
┌─────────────────────────────────────────────────────────────────┐
│                   C++ Trading Engine (Sierra Chart)             │
│  ┌────────────────┐         ┌─────────────────┐               │
│  │ Trade Execution│────────▶│  Trade Logger   │               │
│  │   (Live/Sim)   │         │  (CSV/Database) │               │
│  └────────────────┘         └─────────────────┘               │
└──────────────────────────────────────│──────────────────────────┘
                                       │ Write trades
                                       ▼
                        ┌──────────────────────────┐
                        │   Trade Data Storage     │
                        │  - trades.csv            │
                        │  - trades.db (SQLite)    │
                        │  - ZMQ stream (future)   │
                        └──────────────────────────┘
                                       │ Read trades
                                       ▼
┌─────────────────────────────────────────────────────────────────┐
│          Python Performance Attribution Engine                  │
│  ┌────────────────┐  ┌──────────────────┐  ┌────────────────┐ │
│  │ Trade Collector│─▶│Performance Metrics│─▶│Health Monitor  │ │
│  │   & Parser     │  │    Calculator     │  │   & Writer     │ │
│  └────────────────┘  └──────────────────┘  └────────────────┘ │
│                                                      │           │
│                                                      ▼           │
│                                        ┌──────────────────────┐ │
│                                        │model_health_status.json│
│                                        └──────────────────────┘ │
└─────────────────────────────────────────────────────────────────┘
                                       │ Read health
                                       ▼
┌─────────────────────────────────────────────────────────────────┐
│                   C++ Trading Engine (Sierra Chart)             │
│  ┌────────────────┐         ┌─────────────────┐               │
│  │ Health Monitor │────────▶│ Signal Filter   │               │
│  │  (60s cache)   │         │ (Conf. Adjust)  │               │
│  └────────────────┘         └─────────────────┘               │
└─────────────────────────────────────────────────────────────────┘
```

### Design Philosophy
1. **Defensive Programming:** Handle all edge cases gracefully (missing data, corrupt files, etc.)
2. **Performance:** Process 1000+ trades in <1 second
3. **Reliability:** Never crash - log errors and continue with safe defaults
4. **Testability:** Pure functions, dependency injection, comprehensive unit tests
5. **Observability:** Detailed logging at INFO, WARNING, ERROR levels

---

## 2. Data Sources & Trade Ingestion

### 2.1 Existing Infrastructure - Firestore Database

**✅ C++ IMPLEMENTATION COMPLETE (2025-12-21):** The C++ Trade class has been extended to track and transmit:
1. **MAE (Maximum Adverse Excursion)** - Tracks worst drawdown during trade in ticks ✅
2. **MFE (Maximum Favorable Excursion)** - Tracks best profit during trade in ticks ✅
3. **Confidence** - Stores AI signal confidence at entry (from TransformerAgent ZMQ message) ✅

These fields are now **available in Firestore** and ready for performance attribution. See Section 2.1.1 for implementation details.

**IMPORTANT:** The system already has a complete trade data pipeline:
- **C++ Trading Engine** sends `OpenTrade` and `CloseTrade` messages via ZMQ (port 5556)
- **TradeServer** (`trade_server.py`) receives these messages
- **FirestoreManager** (`firestore_manager.py`) stores trades in Google Firestore
- **GUI Application** (`app.py`) displays trades in real-time

**Existing Trade Schema (Firestore `TradeDocument`):**
```python
@dataclass
class TradeDocument:
    order_id: int                  # Sierra Chart order ID
    trade_status: str              # "Open" or "Closed"
    symbol: str                    # e.g., "ESH25", "NQH25"
    side: str                      # "LONG" or "SHORT"
    size: float                    # Number of contracts
    entry_date: str                # "YYYY-MM-DD HH:MM:SS"
    entry_price: float             # Entry price
    exit_date: str                 # "YYYY-MM-DD HH:MM:SS" (empty if open)
    exit_price: float              # Exit price (0 if open)
    strategy: str                  # Strategy name (optional)
    pnl: float                     # Profit/loss in dollars
    entry_grade: int               # 0-100 objective grade
    exit_grade: int                # 0-100 objective grade
    trade_grade: int               # 0-100 objective grade
    
    # ✅ NEW: Performance attribution fields (Phase 1 - extends existing TradeDocument)
    # These fields extend the existing dataclass with backward-compatible defaults
    confidence: float = 0.0        # AI signal confidence (0.0-1.0) - from live_agent
    mae_ticks: float = 0.0         # Maximum Adverse Excursion in ticks
    mfe_ticks: float = 0.0         # Maximum Favorable Excursion in ticks
    
    # ... plus fields for screenshots, inner_dialogue, the_plan, notes
```

**Example Trade Data (OpenTrade message from C++):**
```json
{
  "order_id": 12345,
  "trade_status": "Open",
  "symbol": "ESH25",
  "side": "Long",
  "size": 2.0,
  "entry_date": "2025-12-21 09:30:15",
  "entry_price": 4175.25,
  "confidence": 0.85,
  "mae_ticks": 0.0,
  "mfe_ticks": 0.0
}
```

**Example Trade Data (CloseTrade message from C++):**
```json
{
  "firestore_doc_id": "abc123def456",
  "order_id": 12345,
  "trade_status": "Closed",
  "exit_date": "2025-12-21 10:15:30",
  "exit_price": 4188.50,
  "pnl": 26.50,
  "exit_grade": 87,
  "trade_grade": 82,
  "mae_ticks": 3.25,
  "mfe_ticks": 15.75
}
```

### 2.1.1 C++ Trade Class Extensions (✅ COMPLETED 2025-12-21)

**Files:** `include/Trade.h` and `src/Trade.cpp`

**Status:** All changes implemented and ready for Python integration.

**Implemented Member Variables:**
```cpp
class Trade {
private:
    // ... existing members ...
    
    // Performance attribution fields (ADD THESE)
    double m_mae_ticks{ 0.0 };           // Maximum Adverse Excursion in ticks
    double m_mfe_ticks{ 0.0 };           // Maximum Favorable Excursion in ticks
    float m_confidence{ 0.0f };          // AI signal confidence (0.0-1.0)
    double m_highest_price{ 0.0 };       // Track for MFE calculation
    double m_lowest_price{ 0.0 };        // Track for MAE calculation
};
```

**New Methods:**
```cpp
// Getters
double GetMAETicks() const { return m_mae_ticks; }
double GetMFETicks() const { return m_mfe_ticks; }
float GetConfidence() const { return m_confidence; }

// Setter (called at entry from AI signal)
void SetConfidence(float confidence) { m_confidence = confidence; }
```

**Update Logic (in Trade::Update()):**
```cpp
void Trade::Update(SCStudyInterfaceRef sc) {
    if (m_status != TradeStatusEnum::OPEN) return;
    
    double current_price = sc.Close[sc.Index];
    
    // Track highest/lowest for MAE/MFE
    if (m_highest_price == 0.0 || current_price > m_highest_price) {
        m_highest_price = current_price;
    }
    if (m_lowest_price == 0.0 || current_price < m_lowest_price) {
        m_lowest_price = current_price;
    }
    
    // Calculate MAE/MFE in ticks
    double tick_size = sc.TickSize;
    if (m_side == TradeSideEnum::LONG) {
        m_mfe_ticks = (m_highest_price - m_entry_price) / tick_size;
        m_mae_ticks = (m_entry_price - m_lowest_price) / tick_size;
    } else if (m_side == TradeSideEnum::SHORT) {
        m_mfe_ticks = (m_entry_price - m_lowest_price) / tick_size;
        m_mae_ticks = (m_highest_price - m_entry_price) / tick_size;
    }
    
    // ... existing grade calculation ...
}
```

**ZMQ Message Updates (in Trade::CreateRequestJson()):**
```cpp
nlohmann::json Trade::CreateRequestJson() const {
    // ... existing fields ...
    
    // Add performance attribution fields
    json["mae_ticks"] = m_mae_ticks;
    json["mfe_ticks"] = m_mfe_ticks;
    json["confidence"] = m_confidence;
    
    return json;
}
```

**Integration Point (PositionManager passes confidence to Trade):**
```cpp
// In PositionManager::ProcessOrderExecutionQueue() - IMPLEMENTED
void PositionManager::ProcessOrderExecutionQueue(SCStudyInterfaceRef sc) {
    // ... order submission logic ...
    
    // Store pattern context and confidence in Trade object
    m_openTrade.SetPattern(request.patternEnum, request.patternId, request.patternName);
    m_openTrade.SetConfidence(request.confidence);  // ✅ IMPLEMENTED
    
    // ... response handling ...
}
```

**Complete Data Flow (✅ IMPLEMENTED):**
```
TransformerAgent (Python) → ZMQ {"confidence": 0.85}
  ↓
TradeExecutionServer extracts confidence from JSON
  ↓
OrderExecutionRequest.confidence = 0.85
  ↓
PositionManager::ProcessOrderExecutionQueue()
  ↓
Trade::SetConfidence(0.85) at entry
  ↓
Trade::Update() tracks MAE/MFE continuously
  ↓
Trade::CreateRequestJson() sends to TradeServer (Python)
  ↓
Firestore stores: confidence, mae_ticks, mfe_ticks
  ↓
Performance Attribution Engine reads from Firestore ✅
```

---

**Available Fields for Performance Attribution:**
| Field | Type | Description | Available |
|-------|------|-------------|----------|
| `entry_date` | string | Entry timestamp "YYYY-MM-DD HH:MM:SS" | ✅ |
| `exit_date` | string | Exit timestamp | ✅ |
| `symbol` | string | Instrument symbol | ✅ |
| `side` | string | "LONG" or "SHORT" | ✅ |
| `size` | float | Number of contracts | ✅ |
| `entry_price` | float | Entry price | ✅ |
| `exit_price` | float | Exit price | ✅ |
| `pnl` | float | P&L in dollars | ✅ |
| `entry_grade` | int | Entry quality (0-100) | ✅ |
| `exit_grade` | int | Exit quality (0-100) | ✅ |
| `trade_grade` | int | Overall trade quality (0-100) | ✅ |
| `trade_status` | string | "Open" or "Closed" | ✅ |
| `order_id` | int | Unique trade identifier | ✅ |
| `firestore_doc_id` | string | Firestore document ID | ✅ |
| `confidence` | float | AI model confidence (0.0-1.0) | ✅ **Implemented (2025-12-21)** |
| `mae_ticks` | float | Maximum Adverse Excursion | ✅ **Implemented (2025-12-21)** |
| `mfe_ticks` | float | Maximum Favorable Excursion | ✅ **Implemented (2025-12-21)** |

**C++ Implementation Status (✅ COMPLETE 2025-12-21):**
- ✅ `mae_ticks` - Maximum Adverse Excursion (implemented in Trade.h, tracked in Trade::Update())
- ✅ `mfe_ticks` - Maximum Favorable Excursion (implemented in Trade.h, tracked in Trade::Update())
- ✅ `confidence` - Stores AI signal confidence in Trade at entry (from ZMQ signal via TradeExecutionServer)

**Future Enhancement Fields (Phase 3):**
- ⏳ `regime` - Market regime at entry (from MarketRegimeIndicator)
- ⏳ `model_version` - AI model version identifier
- ⏳ `exit_reason` - Why trade was closed (target hit, stop hit, time exit, etc.)

### 2.2 Trade Collection Strategy - Leverage Existing Firestore

**Phase 1: Direct Firestore Query (RECOMMENDED)**
```python
from firestore_manager import FirestoreManager

class FirestoreTradeCollector:
    """Collect trades directly from Firestore database."""
    
    def __init__(self, firestore_manager: FirestoreManager):
        self.db_manager = firestore_manager
        self.last_processed_doc_id: Optional[str] = None
        self.last_query_time: Optional[datetime] = None
    
    def collect_new_trades(self) -> list[Trade]:
        """
        Query Firestore for new closed trades since last collection.
        
        Uses existing FirestoreManager.get_trades() method which returns
        a DataFrame with all trades including firestore_doc_id.
        """
        # Get all trades from Firestore
        trades_df = self.db_manager.get_trades()
        
        if trades_df.empty:
            return []
        
        # Filter for closed trades only
        closed_trades = trades_df[trades_df['trade_status'] == 'Closed'].copy()
        
        # Filter for new trades (if tracking last processed ID)
        if self.last_processed_doc_id:
            # Get index of last processed trade
            last_idx = closed_trades[
                closed_trades['firestore_doc_id'] == self.last_processed_doc_id
            ].index
            
            if len(last_idx) > 0:
                # Get trades after last processed
                closed_trades = closed_trades.loc[last_idx[0] + 1:]
        
        # Convert DataFrame rows to Trade objects
        new_trades = []
        for _, row in closed_trades.iterrows():
            trade = Trade.from_firestore_row(row)
            new_trades.append(trade)
        
        # Update last processed ID
        if new_trades:
            self.last_processed_doc_id = new_trades[-1].firestore_doc_id
            self.last_query_time = datetime.now(timezone.utc)
            logger.info(f"Collected {len(new_trades)} new closed trades")
        
        return new_trades
    
    def get_trades_in_window(self, days: int = 30) -> list[Trade]:
        """
        Get all closed trades within the specified rolling window.
        
        Args:
            days: Number of days to look back
        
        Returns:
            List of Trade objects within the window
        """
        trades_df = self.db_manager.get_trades()
        
        if trades_df.empty:
            return []
        
        # Filter for closed trades
        closed_trades = trades_df[trades_df['trade_status'] == 'Closed'].copy()
        
        # Convert exit_date string to datetime for filtering
        closed_trades['exit_datetime'] = pd.to_datetime(
            closed_trades['exit_date'], 
            format='%Y-%m-%d %H:%M:%S',
            errors='coerce'
        )
        
        # Calculate cutoff date
        cutoff_date = datetime.now() - timedelta(days=days)
        
        # Filter by date
        window_trades = closed_trades[
            closed_trades['exit_datetime'] >= cutoff_date
        ]
        
        # Convert to Trade objects
        trades = []
        for _, row in window_trades.iterrows():
            trades.append(Trade.from_firestore_row(row))
        
        logger.info(f"Retrieved {len(trades)} trades from {days}-day window")
        return trades
```

**Phase 2: Real-time Listener (Optional Enhancement)**
```python
class FirestoreTradeListener:
    """
    Listen to Firestore changes in real-time using Firestore snapshots.
    This provides immediate updates when trades are closed.
    """
    
    def __init__(self, firestore_manager: FirestoreManager):
        self.db_manager = firestore_manager
        self.callback_fn = None
        self.listener = None
    
    def start_listening(self, on_trade_closed_callback):
        """
        Start listening for trade status changes.
        
        Args:
            on_trade_closed_callback: Function called when trade closes
        """
        trades_ref = self.db_manager._get_collection_ref('trade')
        
        def on_snapshot(doc_snapshot, changes, read_time):
            for change in changes:
                if change.type.name == 'MODIFIED':
                    doc_dict = change.document.to_dict()
                    if doc_dict.get('trade_status') == 'Closed':
                        trade = Trade.from_firestore_dict(doc_dict)
                        on_trade_closed_callback(trade)
        
        # Create snapshot listener
        self.listener = trades_ref.on_snapshot(on_snapshot)
        logger.info("Firestore real-time listener started")
    
    def stop_listening(self):
        """Stop listening to Firestore changes."""
        if self.listener:
            self.listener.unsubscribe()
            logger.info("Firestore listener stopped")
```

**Decision for Phase 1:** Use **FirestoreTradeCollector** - leverages existing infrastructure, no new data pipeline needed!

### 2.3 Advantages of Firestore Integration

**Benefits of reusing existing infrastructure:**

1. **Zero New Infrastructure** - TradeServer and FirestoreManager already handle trade ingestion
2. **Real-time Data** - Trades are immediately available in Firestore when closed
3. **No Data Sync Issues** - Single source of truth, GUI and PAE read same data
4. **Cloud Backup** - Firestore provides automatic replication and backup
5. **Historical Data** - All past trades already stored and queryable
6. **GUI Integration Ready** - Can easily add health status to existing dashboard
7. **Production-Tested** - FirestoreManager already battle-tested in production
8. **Consistent Schema** - TradeDocument schema already defined and validated

**Firestore Performance Characteristics:**
- Read latency: <100ms for typical queries
- Query capabilities: Filter, sort, compound queries
- Scalability: Handles millions of documents
- Cost: ~$0.06 per 100K reads (negligible for 1000 trades/month)

**Migration Path (if needed later):**
```
Phase 1: Firestore only (current)
Phase 2: Firestore + local cache (SQLite mirror)
Phase 3: Firestore + real-time listener (instant updates)
Phase 4: Multi-region Firestore (disaster recovery)
```

---

## 3. Performance Metrics Calculation

### 3.1 Core Metrics

#### Rolling Window Configuration
```python
@dataclass
class RollingWindowConfig:
    window_days: int = 30  # 30-day rolling window
    min_sample_size: int = 20   # Minimum trades for valid metrics (Mark Douglas: 20 is statistically significant)
    update_every_n_trades: int = 50  # Recalculate after N new trades
    update_interval_hours: int = 4  # OR recalculate every N hours
```

#### Trade Data Model

**IMPLEMENTATION NOTE (2025-12-21):**  
The `Trade` dataclass shown below is for **specification purposes only** to document the complete data model for performance attribution. In the actual implementation, we **extend the existing `TradeDocument` dataclass** in `firestore_manager.py` rather than creating a separate class. This approach:
- ✅ Maintains backward compatibility (existing trades unaffected)
- ✅ Leverages existing Firestore CRUD operations
- ✅ Avoids data model duplication
- ✅ Ensures single source of truth

**Implementation Strategy:**
1. Add Phase 1 fields to `TradeDocument` with default values (confidence=0.0, mae_ticks=0.0, mfe_ticks=0.0)
2. Create helper methods on `TradeDocument` or in calculator.py for computed properties
3. Use existing `FirestoreManager.get_trades()` to query data

```python
@dataclass
class Trade:
    """Normalized trade model for performance attribution (reference model only)."""
    
    # Firestore fields (directly available)
    order_id: int
    firestore_doc_id: str
    symbol: str
    side: str  # "LONG" or "SHORT"
    size: float  # Number of contracts
    entry_date: datetime  # Parsed from string
    entry_price: float
    exit_date: Optional[datetime]  # Parsed from string
    exit_price: float
    pnl: float  # P&L in dollars
    entry_grade: int  # 0-100
    exit_grade: int  # 0-100
    trade_grade: int  # 0-100
    trade_status: str  # "Open" or "Closed"
    
    # Phase 1 fields (available from C++ CloseTrade message)
    confidence: Optional[float] = None  # Model confidence (0.0-1.0) - from live_agent
    mae_ticks: Optional[float] = None   # Maximum Adverse Excursion - computed from entry_high/low
    mfe_ticks: Optional[float] = None   # Maximum Favorable Excursion - computed from entry_high/low
    
    # Phase 3 fields (future enhancements)
    regime: Optional[str] = None        # Market regime classification
    model_version: Optional[str] = None # Model version from .keras metadata
    exit_reason: Optional[str] = None   # Exit reason category
    
    # Computed fields
    trade_duration_mins: Optional[float] = None
    pnl_ticks: Optional[float] = None  # Computed from pnl and tick value
    
    @property
    def is_winner(self) -> bool:
        return self.pnl > 0
    
    @property
    def is_closed(self) -> bool:
        return self.trade_status == "Closed"
    
    @classmethod
    def from_firestore_row(cls, row: pd.Series) -> 'Trade':
        """
        Create Trade object from Firestore DataFrame row.
        
        Args:
            row: pandas Series from FirestoreManager.get_trades()
        
        Returns:
            Trade object
        """
        # Parse datetime strings
        entry_date = datetime.strptime(
            row['entry_date'], 
            '%Y-%m-%d %H:%M:%S'
        ) if row.get('entry_date') else None
        
        exit_date = None
        if row.get('exit_date') and row['exit_date']:
            try:
                exit_date = datetime.strptime(
                    row['exit_date'], 
                    '%Y-%m-%d %H:%M:%S'
                )
            except (ValueError, TypeError):
                pass
        
        # Calculate trade duration if both dates available
        duration_mins = None
        if entry_date and exit_date:
            duration_mins = (exit_date - entry_date).total_seconds() / 60.0
        
        # Calculate PnL in ticks (approximate - will need tick value per symbol)
        pnl_ticks = None
        if row['symbol'].startswith('ES'):
            # ES: $12.50 per tick
            pnl_ticks = row['pnl'] / 12.50
        elif row['symbol'].startswith('NQ'):
            # NQ: $5.00 per tick
            pnl_ticks = row['pnl'] / 5.00
        elif row['symbol'].startswith('YM'):
            # YM: $5.00 per tick
            pnl_ticks = row['pnl'] / 5.00
        
        return cls(
            order_id=int(row.get('order_id', 0)),
            firestore_doc_id=row['firestore_doc_id'],
            symbol=row.get('symbol', ''),
            side=row.get('side', ''),
            size=float(row.get('size', 0)),
            entry_date=entry_date,
            entry_price=float(row.get('entry_price', 0)),
            exit_date=exit_date,
            exit_price=float(row.get('exit_price', 0)),
            pnl=float(row.get('pnl', 0)),
            entry_grade=int(row.get('entry_grade', 0)),
            exit_grade=int(row.get('exit_grade', 0)),
            trade_grade=int(row.get('trade_grade', 0)),
            trade_status=row.get('trade_status', 'Open'),
            trade_duration_mins=duration_mins,
            pnl_ticks=pnl_ticks,
            # Phase 1 fields (available from C++)
            confidence=row.get('confidence'),
            mae_ticks=row.get('mae_ticks'),
            mfe_ticks=row.get('mfe_ticks'),
            # Phase 3 fields (future enhancements)
            regime=row.get('regime'),
            model_version=row.get('model_version'),
            exit_reason=row.get('exit_reason')
        )
```

### 3.2 Performance Metrics Calculator

```python
class PerformanceMetrics:
    """Container for calculated performance metrics."""
    
    # Core metrics
    total_trades: int
    winning_trades: int
    losing_trades: int
    win_rate: float  # 0.0 to 1.0
    
    # Returns
    total_pnl_ticks: float
    total_pnl_dollars: float
    avg_win_ticks: float
    avg_loss_ticks: float
    profit_factor: float  # gross_profit / gross_loss
    
    # Risk-adjusted returns
    sharpe_ratio: float
    sortino_ratio: float
    max_drawdown_pct: float
    
    # Slippage metrics
    avg_mae_ticks: float  # Average Maximum Adverse Excursion
    avg_mfe_ticks: float  # Average Maximum Favorable Excursion
    mae_to_pnl_ratio: float  # How much give-back on winners
    
    # Sample statistics
    sample_size: int
    start_date: datetime
    end_date: datetime
    window_days: int
    
    @classmethod
    def calculate(cls, trades: list[Trade]) -> 'PerformanceMetrics':
        """Calculate all metrics from trade list."""
        if not trades:
            return cls.empty()
        
        closed_trades = [t for t in trades if t.is_closed]
        
        # Win rate
        winners = [t for t in closed_trades if t.is_winner]
        win_rate = len(winners) / len(closed_trades) if closed_trades else 0.0
        
        # PnL metrics
        total_pnl = sum(t.pnl_ticks for t in closed_trades)
        gross_profit = sum(t.pnl_ticks for t in winners)
        gross_loss = abs(sum(t.pnl_ticks for t in closed_trades if not t.is_winner))
        
        # Sharpe ratio (annualized)
        returns = [t.pnl_ticks for t in closed_trades]
        sharpe = cls._calculate_sharpe(returns)
        
        # MAE/MFE analysis
        mae_values = [t.mae_ticks for t in closed_trades if t.mae_ticks is not None]
        avg_mae = np.mean(mae_values) if mae_values else 0.0
        
        return cls(
            total_trades=len(closed_trades),
            winning_trades=len(winners),
            losing_trades=len(closed_trades) - len(winners),
            win_rate=win_rate,
            total_pnl_ticks=total_pnl,
            sharpe_ratio=sharpe,
            avg_mae_ticks=avg_mae,
            sample_size=len(closed_trades),
            # ... other fields
        )
    
    @staticmethod
    def _calculate_sharpe(returns: list[float], risk_free_rate: float = 0.0) -> float:
        """Calculate annualized Sharpe ratio."""
        if len(returns) < 2:
            return 0.0
        
        mean_return = np.mean(returns)
        std_return = np.std(returns, ddof=1)
        
        if std_return == 0:
            return 0.0
        
        # Annualize assuming ~250 trading days
        sharpe = (mean_return - risk_free_rate) / std_return
        annualized_sharpe = sharpe * np.sqrt(252)
        
        return annualized_sharpe
```

### 3.3 Expected vs Realized Comparison

```python
# NOTE: ExpectedPerformance and HealthThresholds are defined in config.py
# This is shown here for reference only

@dataclass
class ExpectedPerformance:
    """Backtested/expected performance benchmarks (defined in config.py)."""
    sharpe_ratio: float = 1.85
    win_rate: float = 0.58
    profit_factor: float = 1.75
    avg_win_ticks: float = 15.5
    avg_loss_ticks: float = 10.7
    max_drawdown_pct: float = 8.5
    
    # Metadata
    backtest_period_start: str = "2024-01-01"
    backtest_period_end: str = "2024-12-01"
    sample_size: int = 1000
    
    @classmethod
    def load_from_keras_metadata(cls, model_path: Path) -> 'ExpectedPerformance':
        """Load expected performance from .keras model metadata."""
        import zipfile
        
        with zipfile.ZipFile(model_path, 'r') as zf:
            if 'metadata.json' in zf.namelist():
                metadata_json = zf.read('metadata.json').decode('utf-8')
                metadata = json.loads(metadata_json)
                return cls(**metadata.get('expected_performance', {}))
        
        # Fallback to defaults if metadata not found
        logger.warning(f"No metadata found in {model_path}, using defaults")
        return cls()


class PerformanceComparison:
    """Compare realized vs expected performance."""
    
    def __init__(
        self, 
        expected: ExpectedPerformance,
        realized: PerformanceMetrics
    ):
        self.expected = expected
        self.realized = realized
    
    def calculate_alpha_slippage_pct(self) -> float:
        """
        Calculate percentage degradation from expected alpha.
        
        Formula:
            expected_alpha = expected_sharpe * sqrt(252) * portfolio_vol
            realized_alpha = realized_sharpe * sqrt(252) * portfolio_vol
            slippage = ((expected - realized) / expected) * 100
        
        Simplified (portfolio_vol cancels out):
            slippage = ((expected_sharpe - realized_sharpe) / expected_sharpe) * 100
        """
        if self.expected.sharpe_ratio == 0:
            return 0.0
        
        slippage = (
            (self.expected.sharpe_ratio - self.realized.sharpe_ratio) 
            / self.expected.sharpe_ratio
        ) * 100
        
        return slippage
    
    def calculate_winrate_degradation_pct(self) -> float:
        """Calculate percentage degradation in win rate."""
        if self.expected.win_rate == 0:
            return 0.0
        
        return (
            (self.expected.win_rate - self.realized.win_rate) 
            / self.expected.win_rate
        ) * 100
    
    def calculate_composite_health_score(self) -> float:
        """
        Calculate composite health score (0-100).
        
        Weights:
        - Alpha slippage: 50%
        - Win rate degradation: 30%
        - Profit factor degradation: 20%
        """
        alpha_slip = max(0, 100 - self.calculate_alpha_slippage_pct())
        winrate_slip = max(0, 100 - self.calculate_winrate_degradation_pct())
        
        pf_expected = self.expected.profit_factor
        pf_realized = self.realized.profit_factor
        pf_slip = max(0, 100 - ((pf_expected - pf_realized) / pf_expected * 100))
        
        composite = (alpha_slip * 0.5) + (winrate_slip * 0.3) + (pf_slip * 0.2)
        return max(0, min(100, composite))
```

---

## 4. Model Health Status Distribution

### 4.1 Dual Distribution Architecture

The Performance Attribution Engine supports **two distribution mechanisms** optimized for different consumers:

**Channel 1: C++ Trading Engine (Synchronous REQ/REP) - CRITICAL PATH**
- **Mechanism:** Health status returned in CloseTrade response payload
- **Latency:** Zero - C++ gets status immediately after trade closes
- **Use Case:** C++ needs immediate health status to filter next AI signal
- **Format:** JSON embedded in ZMQ REP message
- **Trigger:** On-demand (calculated when TradeServer receives CloseTrade)
- **Reliability:** Mission-critical - must never block trade close operations

**Channel 2: GUI Dashboard (Asynchronous File-Based) - DISPLAY ONLY**
- **Mechanism:** Health status written to JSON file
- **Latency:** Irrelevant - GUI updates when user views tab (on-demand read)
- **Use Case:** Dashboard displays model performance for human monitoring
- **Format:** `data/model_health_status.json` file
- **Trigger:** Periodic background job (every 5 minutes) or post-trade update
- **Reliability:** Best-effort - stale data acceptable (user can manually refresh)

**Architecture Diagram:**

```
┌─────────────────────────────────────────────────────────────┐
│          Performance Attribution Engine (PAE)               │
│                                                             │
│  - Calculates 30-day rolling metrics                       │
│  - Compares expected vs realized performance               │
│  - Determines HEALTHY/WARNING/SOFT_LOCKED                  │
│  - Caches results for 60 seconds (avoids recalc)          │
└─────────────────────────────────────────────────────────────┘
                        │
                        │ (provides)
                        ▼
        ┌──────────────────────────────┐
        │ ModelHealthStatusManager     │
        │ (single source of truth)     │
        └──────────────────────────────┘
                │                │
                │                │
      (reads)   │                │   (reads)
                │                │
    ┌───────────▼─────┐    ┌────▼─────────────────┐
    │   TradeServer   │    │ HealthStatusWriter   │
    │   (REQ/REP)     │    │ (periodic writer)    │
    └─────────────────┘    └──────────────────────┘
            │                       │
            │ (sends)               │ (writes)
            │                       │
            ▼                       ▼
    ┌─────────────┐        ┌──────────────────────┐
    │  C++ Engine │        │ model_health_        │
    │  (ZMQ REQ)  │        │ status.json          │
    │             │        │                      │
    │ Immediate   │        │ (updated every       │
    │ Decision    │        │  5 minutes or        │
    │ Making      │        │  on trade close)     │
    └─────────────┘        └──────────────────────┘
                                    │
                                    │ (reads on tab view)
                                    ▼
                           ┌──────────────────┐
                           │  TradeAnalytics  │
                           │  (Dash GUI)      │
                           │                  │
                           │  Updates when:   │
                           │  - User views tab│
                           │  - Clicks refresh│
                           └──────────────────┘
```

**Design Philosophy:**

1. **C++ Path is Synchronous:** TradeServer calculates health on-demand, returns in REP - zero polling
2. **GUI Path is Asynchronous:** File written periodically, GUI reads on-demand - no real-time updates needed
3. **Single Authority:** `ModelHealthStatusManager` is the only code that calculates health status
4. **60-Second Cache:** Avoids recalculating on every trade (most trades happen minutes apart)
5. **Graceful Degradation:** If health calc fails, C++ trade close succeeds, GUI shows error state
6. **KISS for GUI:** No file watchers, no WebSockets, no fancy event systems - just read file when needed

### 4.2 Health Status Determination

```python
from enum import Enum

class ModelHealthStatus(Enum):
    HEALTHY = "HEALTHY"
    WARNING = "WARNING"
    SOFT_LOCKED = "SOFT_LOCKED"


@dataclass
class HealthThresholds:
    """Configurable thresholds for health states."""
    warning_threshold_pct: float = 20.0  # Alpha slippage for WARNING
    soft_lock_threshold_pct: float = 30.0  # Alpha slippage for SOFT_LOCKED
    min_sample_size: int = 20   # Minimum trades before calculating (Mark Douglas: 20 is statistically significant)
    stale_threshold_hours: int = 24  # Consider data stale after this


class ModelHealthStatusManager:
    """
    Centralized manager for determining and distributing health status.
    Used by both TradeServer (for C++ responses) and file writer (for GUI).
    """
    
    def __init__(
        self,
        thresholds: HealthThresholds,
        expected_performance: ExpectedPerformance,
        firestore_manager: FirestoreManager
    ):
        self.thresholds = thresholds
        self.expected = expected_performance
        self.db_manager = firestore_manager
        self.last_status: Optional[ModelHealthStatus] = None
        self.last_calculation_time: Optional[datetime] = None
        self.cached_metrics: Optional[PerformanceMetrics] = None
        self.cache_ttl_seconds = 60  # Cache for 60 seconds
    
    def calculate_current_health(self, force_refresh: bool = False) -> dict:
        """
        Calculate current model health status.
        
        Args:
            force_refresh: If True, bypass cache and recalculate
        
        Returns:
            dict: Complete health status data ready for JSON serialization
        """
        # Check cache (avoid recalculating on every trade)
        now = datetime.now(timezone.utc)
        if not force_refresh and self.last_calculation_time:
            elapsed = (now - self.last_calculation_time).total_seconds()
            if elapsed < self.cache_ttl_seconds and self.cached_metrics:
                # Use cached data
                return self._build_health_response(
                    self.cached_metrics,
                    self.last_status or ModelHealthStatus.HEALTHY
                )
        
        # Get trades from Firestore
        trades_df = self.db_manager.get_trades()
        
        if trades_df.empty:
            return self._build_empty_health_response()
        
        # Filter for closed trades in rolling window (30 days)
        closed_trades = trades_df[trades_df['trade_status'] == 'Closed'].copy()
        cutoff_date = now - timedelta(days=30)
        closed_trades['exit_datetime'] = pd.to_datetime(
            closed_trades['exit_date'], 
            format='%Y-%m-%d %H:%M:%S',
            errors='coerce'
        )
        window_trades = closed_trades[closed_trades['exit_datetime'] >= cutoff_date]
        
        # Convert to Trade objects
        trades = [Trade.from_firestore_row(row) for _, row in window_trades.iterrows()]
        
        # Calculate performance metrics
        metrics = PerformanceMetrics.calculate(trades)
        
        # Compare to expected performance
        comparison = PerformanceComparison(self.expected, metrics)
        
        # Determine health status
        status = self._determine_status(comparison, metrics.sample_size)
        
        # Update cache
        self.cached_metrics = metrics
        self.last_calculation_time = now
        self.last_status = status
        
        # Log status changes
        if status != self.last_status:
            self._log_status_change(status, comparison.calculate_alpha_slippage_pct())
        
        return self._build_health_response(metrics, status, comparison)
    
    def _determine_status(
        self,
        comparison: PerformanceComparison,
        sample_size: int
    ) -> ModelHealthStatus:
        """Determine health status based on alpha slippage."""
        # Insufficient data - default to HEALTHY
        if sample_size < self.thresholds.min_sample_size:
            logger.info(
                f"Insufficient sample size ({sample_size} < {self.thresholds.min_sample_size}). "
                "Defaulting to HEALTHY."
            )
            return ModelHealthStatus.HEALTHY
        
        alpha_slippage = comparison.calculate_alpha_slippage_pct()
        
        # Model exceeding expectations (negative slippage)
        if alpha_slippage < 0:
            logger.info(f"Model exceeding expectations (alpha slippage: {alpha_slippage:.2f}%)")
            return ModelHealthStatus.HEALTHY
        
        # Determine status based on thresholds
        if alpha_slippage < self.thresholds.warning_threshold_pct:
            return ModelHealthStatus.HEALTHY
        elif alpha_slippage < self.thresholds.soft_lock_threshold_pct:
            return ModelHealthStatus.WARNING
        else:
            return ModelHealthStatus.SOFT_LOCKED
    
    def _build_health_response(
        self, 
        metrics: PerformanceMetrics, 
        status: ModelHealthStatus,
        comparison: Optional[PerformanceComparison] = None
    ) -> dict:
        """Build complete health status response dict."""
        if comparison is None:
            comparison = PerformanceComparison(self.expected, metrics)
        
        alpha_slippage = comparison.calculate_alpha_slippage_pct()
        
        return {
            "status": status.value,
            "alpha_slippage_pct": round(alpha_slippage, 2),
            "sample_size": metrics.sample_size,
            "last_updated": datetime.now(timezone.utc).isoformat(),
            "metrics": {
                "expected_sharpe": round(self.expected.sharpe_ratio, 2),
                "realized_sharpe": round(metrics.sharpe_ratio, 2),
                "expected_winrate": round(self.expected.win_rate, 3),
                "realized_winrate": round(metrics.win_rate, 3),
                "avg_mae_slippage_ticks": round(metrics.avg_mae_ticks, 2) if metrics.avg_mae_ticks else 0
            },
            "thresholds": {
                "warning_threshold_pct": self.thresholds.warning_threshold_pct,
                "soft_lock_threshold_pct": self.thresholds.soft_lock_threshold_pct,
                "min_sample_size": self.thresholds.min_sample_size
            }
        }
    
    def _build_empty_health_response(self) -> dict:
        """Build response when no trades available."""
        return {
            "status": "HEALTHY",
            "alpha_slippage_pct": 0.0,
            "sample_size": 0,
            "last_updated": datetime.now(timezone.utc).isoformat(),
            "note": "No closed trades available"
        }
    
    def _log_status_change(self, new_status: ModelHealthStatus, alpha_slippage: float):
        """Log status changes with appropriate severity."""
        if new_status == ModelHealthStatus.SOFT_LOCKED:
            logger.critical(
                f"🔒 Model Health: SOFT_LOCKED (Alpha Slippage: {alpha_slippage:.1f}%) - "
                "ALL AI signals will be rejected!"
            )
        elif new_status == ModelHealthStatus.WARNING:
            logger.warning(
                f"⚠️ Model Health: WARNING (Alpha Slippage: {alpha_slippage:.1f}%) - "
                "Only HIGH confidence signals accepted"
            )
        else:
            logger.info(
                f"✅ Model Health: HEALTHY (Alpha Slippage: {alpha_slippage:.1f}%)"
            )


class ModelHealthStatusWriter:
    """Writes health status to JSON file for GUI dashboard."""
    
    def __init__(self, output_path: Path, health_manager: ModelHealthStatusManager):
        self.output_path = output_path
        self.health_manager = health_manager
    
    def write_status(self, force_refresh: bool = False) -> None:
        """
        Calculate and write model health status to JSON file.
        
        Args:
            force_refresh: If True, bypass cache and recalculate
        """
        health_data = self.health_manager.calculate_current_health(force_refresh)
        
        # Atomic write
        temp_path = self.output_path.with_suffix('.tmp')
        with open(temp_path, 'w') as f:
            json.dump(health_data, f, indent=2)
        
        # Atomic rename (POSIX guarantee)
        temp_path.replace(self.output_path)
        
        logger.info(f"Health status written: {health_data['status']} "
                   f"(Slippage: {health_data['alpha_slippage_pct']:.1f}%)")
```

### 4.3 Integration with TradeServer - REQ/REP Pattern (RECOMMENDED for C++)

**This is the elegant solution for C++ integration!**

Instead of C++ polling a file, **return health status in the CloseTrade response**:

```python
# In trade_server.py
from src.model_health import ModelHealthStatusManager

class TradeServer:
    def __init__(self):
        self.context: zmq.Context = zmq.Context()
        self.socket: zmq.Socket = self.context.socket(zmq.REP)
        # ... existing initialization ...
        
        # NEW: Initialize health status manager
        self.health_manager = self._initialize_health_manager()
        
    def _initialize_health_manager(self) -> ModelHealthStatusManager:
        """Initialize health manager for performance attribution."""
        from src.model_health import ExpectedPerformance, HealthThresholds, ModelHealthStatusManager
        from config import PERFORMANCE_ATTRIBUTION_CONFIG
        
        # Load from config.py
        expected = ExpectedPerformance(
            sharpe_ratio=1.85,
            win_rate=0.58,
            profit_factor=1.75
        )
        
        thresholds = HealthThresholds(
            warning_threshold_pct=PERFORMANCE_ATTRIBUTION_CONFIG['warning_threshold_pct'],
            soft_lock_threshold_pct=PERFORMANCE_ATTRIBUTION_CONFIG['soft_lock_threshold_pct'],
            min_sample_size=PERFORMANCE_ATTRIBUTION_CONFIG['min_sample_size']
        )
        
        return ModelHealthStatusManager(thresholds, expected, firestore_manager)
    
    def handle_request(self, request):
        """Processes a single incoming request from the C++ client."""
        try:
            message = json.loads(request.decode('utf-8'))
            message_type = message.get("type")
            logger.info(f"TradeServer:: Received message type: {message_type}")

            response_data = {"status": "success"}

            if message_type == "OpenTrade":
                firestore_doc_id = firestore_manager.add_trade(message)
                if firestore_doc_id:
                    response_data["firestore_doc_id"] = firestore_doc_id
                    response_data["message"] = "OpenTrade successful."
                    response_data["type"] = "OpenTradeResponse"
                    response_data["order_id"] = message.get("order_id")
                else:
                    response_data["status"] = "error"
                    response_data["message"] = "Failed to add trade to Firestore."

            elif message_type == "CloseTrade":
                firestore_doc_id = message.get("firestore_doc_id")
                if firestore_doc_id:
                    update_data = {
                        key: message[key]
                        for key in [
                            "exit_date", "exit_grade", "exit_price", "pnl",
                            "trade_grade", "trade_status",
                            "confidence", "mae_ticks", "mfe_ticks"  # Phase 1 fields
                        ]
                        if key in message
                    }

                    if firestore_manager.update_trade(firestore_doc_id, update_data):
                        response_data["message"] = f"CloseTrade successful for doc_id: {firestore_doc_id}."
                        response_data["type"] = "CloseTradeResponse"
                        response_data["order_id"] = message.get("order_id")
                        
                        # ========== NEW: Calculate and return health status ==========
                        try:
                            health_status = self.health_manager.calculate_current_health()
                            response_data["model_health"] = health_status
                            
                            logger.info(
                                f"Model health calculated: {health_status['status']} "
                                f"(Slippage: {health_status['alpha_slippage_pct']:.1f}%)"
                            )
                        except Exception as e:
                            logger.error(f"Failed to calculate health status: {e}", exc_info=True)
                            # Include error but don't fail the trade close
                            response_data["model_health"] = {
                                "status": "UNKNOWN",
                                "error": str(e)
                            }
                        # =============================================================
                        
                    else:
                        response_data["status"] = "error"
                        response_data["message"] = f"Failed to edit trade for doc_id: {firestore_doc_id}."
                else:
                    response_data["status"] = "error"
                    response_data["message"] = "CloseTrade message is missing 'firestore_doc_id'."

            elif message_type == "ping":
                response_data["message"] = "pong"

            else:
                logger.warning(f"Unknown message type received: {message_type}")
                response_data["status"] = "error"
                response_data["message"] = f"Unknown message type: {message_type}"

            return json.dumps(response_data).encode('utf-8')

        except json.JSONDecodeError:
            logger.error("Failed to decode JSON message.")
            return json.dumps({"status": "error", "message": "Invalid JSON"}).encode('utf-8')
        except Exception as e:
            logger.error(f"An unexpected error occurred: {e}", exc_info=True)
            return json.dumps({"status": "error", "message": str(e)}).encode('utf-8')
```

**Example Response to C++:**

```json
{
  "status": "success",
  "type": "CloseTradeResponse",
  "order_id": 12345,
  "message": "CloseTrade successful for doc_id: abc123.",
  "model_health": {
    "status": "HEALTHY",
    "alpha_slippage_pct": 12.5,
    "sample_size": 847,
    "last_updated": "2025-12-20T14:32:15Z",
    "metrics": {
      "expected_sharpe": 1.85,
      "realized_sharpe": 1.62,
      "expected_winrate": 0.58,
      "realized_winrate": 0.54
    },
    "thresholds": {
      "warning_threshold_pct": 20.0,
      "soft_lock_threshold_pct": 30.0,
      "min_sample_size": 20
    }
  }
}
```

### 4.4 Benefits of REQ/REP Pattern for C++

**✅ Advantages:**
1. **Zero latency** - C++ gets health status immediately after trade closes
2. **No file polling** - C++ doesn't need 60-second cache timeout
3. **Synchronous** - C++ knows health status before processing next signal
4. **Simpler C++ code** - No file parsing, no cache management
5. **Atomic** - Health status matches exact trade that just closed
6. **One source of truth** - TradeServer calculates once, used everywhere
7. **Graceful degradation** - If health calc fails, trade close still succeeds

**❌ Tradeoffs:**
1. **Slight latency added to REP** - Health calculation takes ~50-200ms
2. **Coupling** - TradeServer now depends on performance attribution code
3. **Memory** - TradeServer holds 30-day trade history in memory (acceptable)

**Mitigation:**
- Cache health calculation for 60 seconds (avoid recalc on every trade)
- If calculation takes >500ms, return last cached value
- Make health calculation async-safe (doesn't block REP)

### 4.5 Updated C++ Integration (No File Reading!)

**Old approach (from original spec):**
```cpp
// C++ reads file every 60 seconds - OBSOLETE
ModelHealthStatus AIConnectionMonitor::CheckModelHealthStatus(SCStudyInterfaceRef sc) {
    if (sc.CurrentSystemDateTime - m_lastHealthFileCheck < 60) {
        return m_lastModelHealth;  // Return cached
    }
    
    std::string json = ReadFile(m_healthFilePath);  // File I/O
    // Parse JSON...
}
```

**New approach (from CloseTrade response):**
```cpp
// C++ gets health status from CloseTrade response - BETTER!
void MindfulTrader::HandleCloseTradeResponse(const json& response) {
    // Extract health status from response
    if (response.contains("model_health")) {
        const auto& health = response["model_health"];
        
        std::string status = health["status"];
        float alpha_slippage = health["alpha_slippage_pct"];
        
        // Update internal state immediately
        if (status == "SOFT_LOCKED") {
            m_modelHealth = ModelHealthStatus::SOFT_LOCKED;
            LogCritical(sc, "🔒 Model SOFT_LOCKED (Alpha: %.1f%%) - Rejecting all AI signals!", 
                       alpha_slippage);
        } else if (status == "WARNING") {
            m_modelHealth = ModelHealthStatus::WARNING;
            LogWarning(sc, "⚠️ Model WARNING (Alpha: %.1f%%) - HIGH confidence only!", 
                      alpha_slippage);
        } else {
            m_modelHealth = ModelHealthStatus::HEALTHY;
            LogInfo(sc, "✅ Model HEALTHY (Alpha: %.1f%%)", alpha_slippage);
        }
    }
}

// When receiving new AI signal
bool MindfulTrader::ShouldAcceptSignal(float confidence) {
    switch (m_modelHealth) {
        case ModelHealthStatus::HEALTHY:
            return confidence >= 0.55;
        case ModelHealthStatus::WARNING:
            return confidence >= 0.70;  // Higher threshold
        case ModelHealthStatus::SOFT_LOCKED:
            return false;  // Reject all
    }
}
```

**Key Improvement:** C++ has **perfect, immediate knowledge** of model health after every trade close!

---

```python
from enum import Enum

class ModelHealthStatus(Enum):
    HEALTHY = "HEALTHY"
    WARNING = "WARNING"
    SOFT_LOCKED = "SOFT_LOCKED"


@dataclass
class HealthThresholds:
    """Configurable thresholds for health states."""
    warning_threshold_pct: float = 20.0  # Alpha slippage for WARNING
    soft_lock_threshold_pct: float = 30.0  # Alpha slippage for SOFT_LOCKED
    min_sample_size: int = 20   # Minimum trades before calculating (Mark Douglas: 20 is statistically significant)
    stale_threshold_hours: int = 24  # Consider data stale after this


class ModelHealthStatusWriter:
    """Determines health status and writes JSON file."""
    
    def __init__(
        self,
        output_path: Path,
        thresholds: HealthThresholds,
        expected_performance: ExpectedPerformance
    ):
        self.output_path = output_path
        self.thresholds = thresholds
        self.expected = expected_performance
        self.last_status: Optional[ModelHealthStatus] = None
    
    def determine_status(
        self,
        comparison: PerformanceComparison,
        sample_size: int
    ) -> ModelHealthStatus:
        """
        Determine health status based on alpha slippage.
        
        Rules:
        1. If sample_size < min_sample_size: Return HEALTHY (insufficient data)
        2. If alpha_slippage < warning_threshold: HEALTHY
        3. If warning_threshold <= alpha_slippage < soft_lock_threshold: WARNING
        4. If alpha_slippage >= soft_lock_threshold: SOFT_LOCKED
        5. If alpha_slippage < 0 (improving): HEALTHY
        """
        # Insufficient data - default to HEALTHY
        if sample_size < self.thresholds.min_sample_size:
            logger.info(
                f"Insufficient sample size ({sample_size} < {self.thresholds.min_sample_size}). "
                "Defaulting to HEALTHY."
            )
            return ModelHealthStatus.HEALTHY
        
        alpha_slippage = comparison.calculate_alpha_slippage_pct()
        
        # Model exceeding expectations (negative slippage)
        if alpha_slippage < 0:
            logger.info(f"Model exceeding expectations (alpha slippage: {alpha_slippage:.2f}%)")
            return ModelHealthStatus.HEALTHY
        
        # Determine status based on thresholds
        if alpha_slippage < self.thresholds.warning_threshold_pct:
            return ModelHealthStatus.HEALTHY
        elif alpha_slippage < self.thresholds.soft_lock_threshold_pct:
            return ModelHealthStatus.WARNING
        else:
            return ModelHealthStatus.SOFT_LOCKED
    
    def write_status(
        self,
        status: ModelHealthStatus,
        comparison: PerformanceComparison,
        metrics: PerformanceMetrics
    ) -> None:
        """
        Write model health status to JSON file (atomic write).
        
        Atomic write strategy:
        1. Write to temporary file
        2. Rename to target file (atomic on POSIX systems)
        """
        alpha_slippage = comparison.calculate_alpha_slippage_pct()
        
        health_data = {
            "status": status.value,
            "alpha_slippage_pct": round(alpha_slippage, 2),
            "sample_size": metrics.sample_size,
            "last_updated": datetime.now(timezone.utc).isoformat(),
            "metrics": {
                "expected_sharpe": round(self.expected.sharpe_ratio, 2),
                "realized_sharpe": round(metrics.sharpe_ratio, 2),
                "expected_winrate": round(self.expected.win_rate, 3),
                "realized_winrate": round(metrics.win_rate, 3),
                "avg_mae_slippage_ticks": round(metrics.avg_mae_ticks, 2)
            },
            "thresholds": {
                "warning_threshold_pct": self.thresholds.warning_threshold_pct,
                "soft_lock_threshold_pct": self.thresholds.soft_lock_threshold_pct,
                "min_sample_size": self.thresholds.min_sample_size
            }
        }
        
        # Atomic write
        temp_path = self.output_path.with_suffix('.tmp')
        with open(temp_path, 'w') as f:
            json.dump(health_data, f, indent=2)
        
        # Atomic rename (POSIX guarantee)
        temp_path.replace(self.output_path)
        
        # Log status changes
        if status != self.last_status:
            self._log_status_change(status, alpha_slippage)
            self.last_status = status
    
    def _log_status_change(self, new_status: ModelHealthStatus, alpha_slippage: float):
        """Log status changes with appropriate severity."""
        if new_status == ModelHealthStatus.SOFT_LOCKED:
            logger.critical(
                f"🔒 Model Health: SOFT_LOCKED (Alpha Slippage: {alpha_slippage:.1f}%) - "
                "ALL AI signals will be rejected!"
            )
        elif new_status == ModelHealthStatus.WARNING:
            logger.warning(
                f"⚠️ Model Health: WARNING (Alpha Slippage: {alpha_slippage:.1f}%) - "
                "Only HIGH confidence signals accepted"
            )
        else:
            logger.info(
                f"✅ Model Health: HEALTHY (Alpha Slippage: {alpha_slippage:.1f}%)"
            )
```

---

## 5. Main Engine Orchestration ⚠️ OBSOLETE IN v3.0

**Note:** This section describes the standalone daemon architecture that has been replaced by direct integration into TradeServer and Dashboard. Retained for historical reference.

### 5.1 Update Triggers

```python
@dataclass
class UpdatePolicy:
    """Policy for when to recalculate and update health status."""
    update_every_n_trades: int = 50  # Update after N new trades
    update_interval_seconds: int = 14400  # Update every 4 hours (14400 sec)
    force_update_on_status_change: bool = True  # Immediate update on status change
    

class PerformanceAttributionEngine:
    """Main orchestration engine."""
    
    def __init__(
        self,
        trade_collector: TradeCollector,
        expected_performance: ExpectedPerformance,
        health_writer: ModelHealthStatusWriter,
        update_policy: UpdatePolicy,
        rolling_window_days: int = 30
    ):
        self.trade_collector = trade_collector
        self.expected = expected_performance
        self.health_writer = health_writer
        self.update_policy = update_policy
        self.rolling_window_days = rolling_window_days
        
        # State tracking
        self.trades_since_last_update = 0
        self.last_update_time: Optional[datetime] = None
        self.all_trades: list[Trade] = []
    
    def should_update(self) -> bool:
        """Determine if health status should be recalculated."""
        # First run
        if self.last_update_time is None:
            return True
        
        # Trade count trigger
        if self.trades_since_last_update >= self.update_policy.update_every_n_trades:
            logger.info(f"Update triggered by trade count ({self.trades_since_last_update} trades)")
            return True
        
        # Time interval trigger
        elapsed_seconds = (datetime.now(timezone.utc) - self.last_update_time).total_seconds()
        if elapsed_seconds >= self.update_policy.update_interval_seconds:
            logger.info(f"Update triggered by time interval ({elapsed_seconds/3600:.1f} hours)")
            return True
        
        return False
    
    def get_rolling_window_trades(self) -> list[Trade]:
        """Get trades within rolling window (last N days)."""
        cutoff_date = datetime.now(timezone.utc) - timedelta(days=self.rolling_window_days)
        return [t for t in self.all_trades if t.timestamp >= cutoff_date]
    
    def run_update_cycle(self) -> None:
        """Execute one update cycle: collect, calculate, write."""
        try:
            # Step 1: Collect new trades
            new_trades = self.trade_collector.collect_new_trades()
            
            if new_trades:
                logger.info(f"Collected {len(new_trades)} new trades")
                self.all_trades.extend(new_trades)
                self.trades_since_last_update += len(new_trades)
            
            # Step 2: Check if update needed
            if not self.should_update():
                return
            
            # Step 3: Get rolling window trades
            window_trades = self.get_rolling_window_trades()
            logger.info(
                f"Rolling window: {len(window_trades)} trades "
                f"(last {self.rolling_window_days} days)"
            )
            
            # Step 4: Calculate performance metrics
            metrics = PerformanceMetrics.calculate(window_trades)
            
            # Step 5: Compare to expected performance
            comparison = PerformanceComparison(self.expected, metrics)
            
            # Step 6: Determine health status
            status = self.health_writer.determine_status(comparison, metrics.sample_size)
            
            # Step 7: Write health status file
            self.health_writer.write_status(status, comparison, metrics)
            
            # Step 8: Reset counters
            self.trades_since_last_update = 0
            self.last_update_time = datetime.now(timezone.utc)
            
            logger.info(
                f"Health status updated: {status.value} "
                f"(Alpha Slippage: {comparison.calculate_alpha_slippage_pct():.2f}%)"
            )
            
        except Exception as e:
            logger.error(f"Error in update cycle: {e}", exc_info=True)
            # Don't crash - continue on next cycle
    
    def run_daemon(self, poll_interval_seconds: int = 60) -> None:
        """Run engine as a daemon (continuous loop)."""
        logger.info(f"Starting Performance Attribution Engine daemon (poll every {poll_interval_seconds}s)")
        
        while True:
            try:
                self.run_update_cycle()
                time.sleep(poll_interval_seconds)
            except KeyboardInterrupt:
                logger.info("Daemon shutdown requested")
                break
            except Exception as e:
                logger.error(f"Unexpected error in daemon loop: {e}", exc_info=True)
                time.sleep(poll_interval_seconds)  # Continue after error
```

---

## 6. Configuration Management

**Note:** Configuration is now loaded in TradeServer and Dashboard, not by standalone daemon. Use YAML for thresholds and expected performance values.

### 6.1 Configuration File Format

**File:** `config/performance_attribution.yaml`

```yaml
# Firestore Configuration (reuse existing)
firestore:
  credentials_path: "path/to/firebase-credentials.json"
  use_existing_manager: true  # Reuse FirestoreManager

# Model Configuration
model:
  path: "src/best_agent_model.keras"  # .keras file contains embedded architecture config
  metadata_path: "src/best_agent_model_metadata.json"  # Performance metrics from backtest
  
# Output
output:
  log_file: "/home/trader/logs/performance_attribution.log"
  log_level: "INFO"  # DEBUG, INFO, WARNING, ERROR, CRITICAL

# Expected Performance - LOADED FROM METADATA JSON FILE
# Note: These values are stored in src/best_agent_model_metadata.json by the training process.
# The config below shows the default values used as fallback only.
expected_performance:
  sharpe_ratio: 1.85  # From metadata file
  win_rate: 0.58
  profit_factor: 1.75
  avg_win_ticks: 15.5
  avg_loss_ticks: 10.7
  max_drawdown_pct: 8.5
  backtest_period_start: "2024-01-01"
  backtest_period_end: "2024-12-01"
  sample_size: 1000

# Health Thresholds
thresholds:
  warning_threshold_pct: 20.0
  soft_lock_threshold_pct: 30.0
  min_sample_size: 20
  stale_threshold_hours: 24

# Rolling Window
rolling_window:
  window_days: 30
  min_trades: 20

# Update Policy
update_policy:
  update_every_n_trades: 50
  update_interval_hours: 4
  force_update_on_status_change: true

# Daemon Settings
daemon:
  poll_interval_seconds: 60
  enable_notifications: false  # Future: email/Slack alerts

# Symbol tick values (for PnL conversion)
tick_values:
  ES: 12.50  # E-mini S&P 500
  NQ: 5.00   # E-mini NASDAQ
  YM: 5.00   # E-mini Dow
  RTY: 5.00  # E-mini Russell 2000

# Notifications (Future Phase)
# notifications:
#   email:
#     enabled: false
#     smtp_server: "smtp.gmail.com"
#     smtp_port: 587
#     recipients: ["trader@example.com"]
#   slack:
#     enabled: false
#     webhook_url: "https://hooks.slack.com/..."
```

### 6.2 Configuration Loader

```python
from dataclasses import dataclass
import yaml

@dataclass
class PAEConfig:
    """Performance Attribution Engine configuration."""
    
    # Firestore (reuse existing manager)
    use_existing_firestore: bool
    
    # Output
    health_status_file: Path
    log_file: Path
    log_level: str
    
    # Expected performance
    expected_performance: ExpectedPerformance
    
    # Thresholds
    thresholds: HealthThresholds
    
    # Rolling window
    rolling_window_days: int
    
    # Update policy
    update_policy: UpdatePolicy
    
    # Daemon
    poll_interval_seconds: int
    
    # Tick values
    tick_values: Dict[str, float]
    
    # Model path for loading embedded config
    model_path: Path
    
    @classmethod
    def load(cls, config_path: Path, model_path: Path) -> 'PAEConfig':
        """Load configuration from YAML file and .keras model metadata.
        
        Args:
            config_path: Path to YAML config (for infrastructure settings)
            model_path: Path to .keras model file (for expected performance)
        """
        with open(config_path) as f:
            config_dict = yaml.safe_load(f)
        
        # Load expected performance from .keras metadata
        expected_perf = ExpectedPerformance.load_from_keras_metadata(model_path)
        
        return cls(
            use_existing_firestore=config_dict['firestore'].get('use_existing_manager', True),
            log_file=Path(config_dict['output']['log_file']),
            log_level=config_dict['output']['log_level'],
            expected_performance=expected_perf,  # From .keras metadata
            thresholds=HealthThresholds(**config_dict['thresholds']),
            rolling_window_days=config_dict['rolling_window']['window_days'],
            update_policy=UpdatePolicy(
                update_every_n_trades=config_dict['update_policy']['update_every_n_trades'],
                update_interval_seconds=config_dict['update_policy']['update_interval_hours'] * 3600,
                force_update_on_status_change=config_dict['update_policy']['force_update_on_status_change']
            ),
            poll_interval_seconds=config_dict['daemon']['poll_interval_seconds'],
            tick_values=config_dict.get('tick_values', {
                'ES': 12.50, 'NQ': 5.00, 'YM': 5.00, 'RTY': 5.00
            }),
            model_path=model_path
        )
```

---

## 7. Error Handling & Edge Cases

### 7.1 Error Handling Strategy

| Error Condition | Handling Strategy | Fallback Behavior |
|----------------|-------------------|-------------------|
| Trades file missing | Log WARNING, wait for file creation | Continue polling, return empty list |
| Trades file corrupt | Log ERROR, skip corrupted lines | Parse valid lines, discard corrupt |
| Insufficient trades (<100) | Log INFO, skip calculation | Write HEALTHY status with note |
| Zero variance in returns | Log WARNING, Sharpe = 0 | Use profit factor as backup metric |
| Disk full (can't write status) | Log CRITICAL, retry with backoff | Keep last valid status in memory |
| Expected performance missing | Log ERROR, load defaults | Use conservative defaults (Sharpe=1.5) |
| Negative Sharpe ratio | Valid (losing model), LOG WARNING | Calculate slippage normally |
| All trades are winners | Valid (lucky period), variance=0 | Use win rate as primary metric |
| File permissions denied | Log CRITICAL, raise exception | Fail fast - requires fix |

### 7.2 Edge Case Handling

```python
class EdgeCaseHandler:
    """Handle edge cases in performance calculation."""
    
    @staticmethod
    def handle_insufficient_sample(sample_size: int, min_size: int) -> dict:
        """Handle case where sample size is too small."""
        if sample_size < min_size:
            return {
                "status": "HEALTHY",
                "alpha_slippage_pct": 0.0,
                "sample_size": sample_size,
                "note": f"Insufficient data ({sample_size}/{min_size} trades)"
            }
        return None
    
    @staticmethod
    def handle_zero_variance(returns: list[float]) -> float:
        """Handle case where all returns are identical."""
        if len(set(returns)) == 1:
            # All trades identical - use alternate metric
            avg_return = returns[0]
            if avg_return > 0:
                return 5.0  # Arbitrary high Sharpe for perfect consistency
            else:
                return -5.0  # Arbitrary low Sharpe for consistent losses
        return None
    
    @staticmethod
    def handle_negative_expected_sharpe(expected: float, realized: float) -> float:
        """Handle case where expected Sharpe is negative (shouldn't happen)."""
        if expected < 0:
            logger.error(f"Expected Sharpe is negative ({expected:.2f}) - check config!")
            # Invert comparison logic
            return ((abs(expected) - abs(realized)) / abs(expected)) * 100
        return None
    
    @staticmethod
    def handle_file_write_failure(path: Path, data: dict, retries: int = 3) -> bool:
        """Retry file write with exponential backoff."""
        for attempt in range(retries):
            try:
                temp_path = path.with_suffix('.tmp')
                with open(temp_path, 'w') as f:
                    json.dump(data, f, indent=2)
                temp_path.replace(path)
                return True
            except Exception as e:
                wait_time = 2 ** attempt
                logger.warning(f"Write failed (attempt {attempt+1}/{retries}): {e}")
                time.sleep(wait_time)
        
        logger.critical(f"Failed to write health status after {retries} attempts!")
        return False
```

---

## 8. Testing Strategy

### 8.1 Unit Tests

```python
# tests/test_performance_metrics.py
def test_calculate_sharpe_ratio():
    """Test Sharpe ratio calculation with known values."""
    returns = [1.0, 2.0, -0.5, 3.0, 1.5]  # Simple returns
    sharpe = PerformanceMetrics._calculate_sharpe(returns)
    assert 0 < sharpe < 5  # Reasonable range

def test_alpha_slippage_calculation():
    """Test alpha slippage with degraded performance."""
    expected = ExpectedPerformance(sharpe_ratio=2.0)
    realized = PerformanceMetrics(sharpe_ratio=1.4, sample_size=100)  # 30% degradation
    
    comparison = PerformanceComparison(expected, realized)
    slippage = comparison.calculate_alpha_slippage_pct()
    
    assert abs(slippage - 30.0) < 0.01

def test_health_status_determination():
    """Test status transitions at threshold boundaries."""
    thresholds = HealthThresholds(warning=20.0, soft_lock=30.0)
    writer = ModelHealthStatusWriter(Path("/tmp/health.json"), thresholds, expected)
    
    # Test HEALTHY (19% slippage)
    comparison = PerformanceComparison(
        ExpectedPerformance(sharpe_ratio=2.0),
        PerformanceMetrics(sharpe_ratio=1.62, sample_size=100)
    )
    assert writer.determine_status(comparison, 100) == ModelHealthStatus.HEALTHY
    
    # Test WARNING (25% slippage)
    comparison = PerformanceComparison(
        ExpectedPerformance(sharpe_ratio=2.0),
        PerformanceMetrics(sharpe_ratio=1.50, sample_size=100)
    )
    assert writer.determine_status(comparison, 100) == ModelHealthStatus.WARNING
```

### 8.2 Integration Tests

```python
# tests/test_integration.py
def test_end_to_end_pipeline(tmp_path):
    """Test complete pipeline from trades file to health status."""
    # Setup
    trades_file = tmp_path / "trades.csv"
    health_file = tmp_path / "health.json"
    
    # Create sample trades CSV
    create_sample_trades_csv(trades_file, num_trades=150, avg_sharpe=1.5)
    
    # Create engine
    config = PAEConfig(
        trades_file=trades_file,
        health_status_file=health_file,
        expected_performance=ExpectedPerformance(sharpe_ratio=2.0),
        # ... other config
    )
    engine = PerformanceAttributionEngine.from_config(config)
    
    # Run one cycle
    engine.run_update_cycle()
    
    # Verify health status file created
    assert health_file.exists()
    
    # Verify JSON content
    with open(health_file) as f:
        health_data = json.load(f)
    
    assert health_data['status'] in ['HEALTHY', 'WARNING', 'SOFT_LOCKED']
    assert health_data['sample_size'] == 150
    assert 'alpha_slippage_pct' in health_data
```

### 8.3 Manual Testing with Stub Data

Create stub trade files for different scenarios:

```python
# scripts/create_test_trades.py
def create_healthy_model_trades(output_path: Path):
    """Generate trades for a healthy model (minimal slippage)."""
    # Generate 200 trades with Sharpe ~1.8 (close to expected 1.85)
    pass

def create_warning_model_trades(output_path: Path):
    """Generate trades for WARNING state (20-30% slippage)."""
    # Generate 200 trades with Sharpe ~1.4 (24% degradation from 1.85)
    pass

def create_soft_locked_model_trades(output_path: Path):
    """Generate trades for SOFT_LOCKED state (>30% slippage)."""
    # Generate 200 trades with Sharpe ~1.0 (46% degradation from 1.85)
    pass
```

---

## 9. Deployment & Operations

### 9.1 Deployment Options

**Option A: Systemd Service (Linux)**
```ini
# /etc/systemd/system/performance-attribution.service
[Unit]
Description=Performance Attribution Engine
After=network.target

[Service]
Type=simple
User=trader
WorkingDirectory=/home/trader/trading
ExecStart=/home/trader/.conda/envs/trading/bin/python -m performance_attribution.daemon
Restart=always
RestartSec=10

[Install]
WantedBy=multi-user.target
```

**Option B: Cron Job (Scheduled)**
```cron
# Run every 5 minutes
*/5 * * * * /home/trader/.conda/envs/trading/bin/python /home/trader/trading/performance_attribution/run_once.py
```

**Option C: Docker Container**
```dockerfile
FROM python:3.10-slim
WORKDIR /app
COPY requirements.txt .
RUN pip install -r requirements.txt
COPY . .
CMD ["python", "-m", "performance_attribution.daemon"]
```

### 9.2 Monitoring & Observability

**Log Format:**
```
2025-12-20 14:32:15 [INFO] Engine started (config: /home/trader/config/pae.yaml)
2025-12-20 14:32:16 [INFO] Collected 12 new trades
2025-12-20 14:32:16 [INFO] Rolling window: 487 trades (last 30 days)
2025-12-20 14:32:16 [INFO] Calculated metrics: Sharpe=1.62, WinRate=0.54
2025-12-20 14:32:16 [WARNING] ⚠️ Model Health: WARNING (Alpha Slippage: 24.3%)
2025-12-20 14:32:16 [INFO] Health status written to /home/trader/data/model_health_status.json
```

**Health Check Endpoint (Future):**
```python
from flask import Flask, jsonify

app = Flask(__name__)

@app.route('/health')
def health_check():
    """Return engine health status."""
    return jsonify({
        "engine_status": "running",
        "last_update": engine.last_update_time.isoformat(),
        "trades_collected": len(engine.all_trades),
        "current_health": engine.health_writer.last_status.value
    })
```

---

## 10. Implementation Phases

### Phase 1: Core Health Manager (Day 1, 4-6 hours)
- [ ] Implement `Trade` dataclass
- [ ] Implement `PerformanceMetrics` calculator
- [ ] Implement `PerformanceComparison` and alpha slippage calculation
- [ ] Implement `ModelHealthStatusManager` with 60-second cache
- [ ] Unit tests for calculator and manager
- [ ] Manual testing with Firestore data

**Deliverable:** Functional health manager that queries Firestore and calculates health status

### Phase 2: TradeServer Integration (Day 2, 3-4 hours)
- [ ] Integrate `ModelHealthStatusManager` into TradeServer
- [ ] Add health calculation to CloseTrade handler
- [ ] Return health status in ZMQ REP response
- [ ] Error handling (health calc failures don't break trade close)
- [ ] Integration tests with mock Firestore
- [ ] Test with C++ client

**Deliverable:** C++ receives health status synchronously in CloseTrade responses

### Phase 3: Dashboard Integration (Day 3, 3-4 hours)
- [ ] Add model-health-card div to TradeAnalytics layout
- [ ] Implement `update_model_health_card()` callback
- [ ] Import and use ModelHealthStatusManager in dashboard
- [ ] Add dcc.Interval for optional auto-refresh
- [ ] Style health card (Bootstrap theme)
- [ ] Test health status display with various states

**Deliverable:** Dashboard displays model health status with on-demand refresh

### Phase 4: Configuration & Deployment (Day 4, 2-3 hours)
- [ ] Create YAML config for thresholds and expected performance
- [ ] Load config in TradeServer and dashboard
- [ ] Add config validation
- [ ] Documentation and runbooks
- [ ] Deployment checklist

**Deliverable:** Production-ready system with external configuration

### Phase 5: Advanced Features (Future)
- [ ] SQLite database integration
- [ ] ZMQ real-time stream support
- [ ] Email/Slack notifications
- [ ] Multi-model tracking (separate health per symbol)
- [ ] Performance dashboard (web UI)
- [ ] Automatic model retraining triggers

---

## 11. Architecture Decisions - Final Status

### 11.1 Data Source ✅ RESOLVED
- **Q:** Will C++ write trades to CSV, SQLite, or both?
- **Decision:** ✅ **Use existing Firestore database via FirestoreManager**
- **Rationale:** Infrastructure already in place, no new data pipeline needed. TradeServer already receives OpenTrade/CloseTrade messages and writes to Firestore.

### 11.2 C++ Integration Method ✅ RESOLVED  
- **Q:** Should C++ poll model_health_status.json file every 60 seconds?
- **Decision:** ✅ **C++ gets health status in CloseTrade REP response (synchronous)**
- **Rationale:** Zero latency, no file polling, immediate decision making. TradeServer calculates health and includes in response payload.
- **Implementation:** See Section 4.3 for TradeServer integration code

### 11.3 GUI Refresh Strategy ✅ RESOLVED
- **Q:** Should GUI use polling, event-driven, or hybrid refresh?
- **Decision:** ✅ **Direct Firestore queries when user views dashboard tab**
- **Rationale:** With 5-20 trades/day, real-time GUI updates unnecessary. Tab-switching triggers calculation. Firestore query is fast (<50ms for 60 trades). No background daemon or file synchronization needed.
- **Implementation:** See Section 13.5 for dashboard callback that imports ModelHealthStatusManager

###  11.4 Missing Fields in OpenTrade Message ✅ RESOLVED
- **Q:** C++ OpenTrade message doesn't currently include these performance attribution fields:
  - `confidence` - AI model confidence score (0.0-1.0)
  - `mae_ticks` - Maximum Adverse Excursion
  - `mfe_ticks` - Maximum Favorable Excursion  
  - `regime` - Market regime at entry
  - `model_version` - AI model version identifier
  - `exit_reason` - Why trade was closed
  
- **Decision:** ✅ **Phased approach - prioritize available data**

#### Phase 1: Add Immediately (Data Already Available) ✅
**Add to OpenTrade/CloseTrade messages:**
- ✅ `confidence` (float) - AI model confidence score (0.0-1.0)
  - **Source:** Already extracted in [src/live_agent.py:368-374](src/live_agent.py#L368-L374)
  - **C++ receives:** From Python live_agent ZMQ response
  - **Critical for:** Model certainty vs outcome correlation, confidence-based filtering
  
- ✅ `mae_ticks` (float) - Maximum Adverse Excursion in ticks
  - **Source:** C++ already tracks `entry_high`, `entry_low` in TradeData
  - **Calculation:** `(entry_price - entry_low) / tick_size` for longs, `(entry_high - entry_price) / tick_size` for shorts
  - **Critical for:** Risk tolerance validation, stop placement effectiveness
  
- ✅ `mfe_ticks` (float) - Maximum Favorable Excursion in ticks
  - **Source:** C++ already tracks `entry_high`, `entry_low` in TradeData  
  - **Calculation:** `(entry_high - entry_price) / tick_size` for longs, `(entry_price - entry_low) / tick_size` for shorts
  - **Critical for:** Profit capture efficiency, premature exit detection

**Rationale:** All three values are either already computed (confidence) or trivially calculated from existing data (mae/mfe from high/low tracking). Zero reason to defer - include in Phase 1.

#### Phase 3: Future Enhancements (Deferred) 📊
- ⏳ `model_version` - For A/B testing across model versions (load from .keras metadata)
- ⏳ `regime` - Market regime classification (trending, choppy, volatile, etc.)
- ⏳ `exit_reason` - Why trade closed ("STOP_LOSS", "TARGET_HIT", "SIGNAL_REVERSAL", "TIME_EXIT", "MANUAL")

**Implementation Notes:**
```python
# C++ sends in CloseTrade message:
{
    "firestore_doc_id": "abc123",
    "confidence": 0.87,        # NEW - Phase 1
    "mae_ticks": 4.5,          # NEW - Phase 1  
    "mfe_ticks": 12.3,         # NEW - Phase 1
    "pnl": 125.50,
    "exit_price": 5015.25,
    # ... existing fields
}
```

#### C++ Implementation Guide for Phase 1 Fields

**1. Confidence Score:**
```cpp
// C++ receives from Python live_agent via ZMQ when AI signal arrives
// Store in trade tracking structure
struct TradeInfo {
    float confidence;  // NEW - from AI model
    // ... existing fields
};

// When AI signal message arrives (ZMQ response from Python):
trade.confidence = ai_signal["confidence"].asFloat();  // Extract from JSON

// Include in CloseTrade message:
close_msg["confidence"] = trade.confidence;
```

**2. MAE (Maximum Adverse Excursion):**
```cpp
// Already tracking: entry_high, entry_low during trade
// Calculate MAE in ticks when closing trade:

float CalculateMAE(float entry_price, float entry_low, float entry_high, 
                   const char* side, float tick_size) {
    if (strcmp(side, "LONG") == 0) {
        return (entry_price - entry_low) / tick_size;  // Worst drawdown for longs
    } else {  // SHORT
        return (entry_high - entry_price) / tick_size;  // Worst drawdown for shorts
    }
}

// In CloseTrade message:
close_msg["mae_ticks"] = CalculateMAE(trade.entry_price, trade.entry_low, 
                                      trade.entry_high, trade.side, tick_size);
```

**3. MFE (Maximum Favorable Excursion):**
```cpp
float CalculateMFE(float entry_price, float entry_low, float entry_high,
                   const char* side, float tick_size) {
    if (strcmp(side, "LONG") == 0) {
        return (entry_high - entry_price) / tick_size;  // Best profit for longs
    } else {  // SHORT
        return (entry_price - entry_low) / tick_size;  // Best profit for shorts
    }
}

// In CloseTrade message:
close_msg["mfe_ticks"] = CalculateMFE(trade.entry_price, trade.entry_low,
                                      trade.entry_high, trade.side, tick_size);
```

**Summary for C++ Developer:**
- ✅ Confidence: Extract from Python AI signal response, store, forward in CloseTrade
- ✅ MAE/MFE: Compute from existing `entry_high`/`entry_low` tracking (already in TradeData)
- ✅ All three fields are optional - Python will handle missing values gracefully

### 11.5 Expected Performance Benchmarks ✅ RESOLVED
- **Q:** Where do expected performance metrics come from?
- **Decision:** ✅ **Loaded from .keras model metadata**
- **Implementation:** TransformerAgent training embeds `ExpectedPerformance` in .keras file as JSON metadata
- **Benefits:**
  - Single file contains model + expected metrics
  - No separate config files to manage
  - Model and benchmarks stay synchronized
  - Version controlled as one artifact
- **Fallback:** Use default values if metadata missing (for backward compatibility)
- **Future:** Option 3 (adaptive baseline from live trading) in Phase 4

#### .keras Metadata Format Specification

**For TransformerAgent Team:** Create `src/backtest_metrics.py` in your repo with:
1. PerformanceMetricsCalculator class (calculate Sharpe, win rate, etc. from backtest trades)
2. Function to embed calculated metrics as JSON metadata in .keras file after training
3. Use same field names as MTS for consistency

The TransformerAgent training process should embed this JSON structure in the .keras model file:

```json
{
  "model_config": {
    "expected_performance": {
      "sharpe_ratio": 1.85,
      "win_rate": 0.58,
      "profit_factor": 1.75,
      "avg_win_ticks": 15.5,
      "avg_loss_ticks": 10.7,
      "max_drawdown_pct": 8.5,
      "backtest_period_start": "2024-01-01",
      "backtest_period_end": "2024-12-01",
      "sample_size": 1000
    },
    "health_thresholds": {
      "warning_threshold_pct": 20.0,
      "soft_lock_threshold_pct": 30.0,
      "hard_lock_threshold_pct": 50.0,
      "min_sample_size": 20
    }
  }
}
```

**Embedding Implementation (TransformerAgent - REQUIRED):**
```python
# In TransformerAgent training script (after model training completes)
import json
from pathlib import Path
from dataclasses import dataclass, asdict

@dataclass
class ExpectedPerformance:
    """Expected performance metrics from backtest - matches MTS format exactly."""
    sharpe_ratio: float
    win_rate: float
    profit_factor: float
    avg_win_ticks: float
    avg_loss_ticks: float
    max_drawdown_pct: float
    backtest_period_start: str  # "YYYY-MM-DD"
    backtest_period_end: str    # "YYYY-MM-DD"
    sample_size: int            # Number of backtest trades

def embed_performance_metadata(model, expected_perf: ExpectedPerformance, 
                                model_path: Path) -> None:
    """
    Save expected performance metrics to companion JSON file alongside .keras model.
    
    CRITICAL: This must be called AFTER training, BEFORE deploying model to live trading.
    
    Args:
        model: Trained Keras model (used for validation only)
        expected_perf: ExpectedPerformance calculated from backtest trades
        model_path: Path where model is saved (e.g., "src/best_agent_model.keras")
    """
    # Create metadata structure
    metadata = {
        "expected_performance": asdict(expected_perf),
        "health_thresholds": {
            "warning_threshold_pct": 20.0,
            "soft_lock_threshold_pct": 30.0,
            "hard_lock_threshold_pct": 50.0,
            "min_sample_size": 20
        }
    }
    
    # Save metadata to companion JSON file
    metadata_path = str(model_path).replace('.keras', '_metadata.json')
    with open(metadata_path, 'w') as f:
        json.dump(metadata, f, indent=2)
    
    print(f"✅ Performance metadata saved: {metadata_path}")
    print(f"   Sharpe Ratio: {expected_perf.sharpe_ratio:.2f}")
    print(f"   Win Rate: {expected_perf.win_rate:.1%}")
    print(f"   Sample Size: {expected_perf.sample_size} trades")


# Example usage in training script:
# After backtest completes, calculate expected performance
backtest_trades = load_backtest_results()  # Your backtest trade data
metrics = PerformanceMetricsCalculator.calculate_metrics(backtest_trades)

expected_perf = ExpectedPerformance(
    sharpe_ratio=metrics.sharpe_ratio,
    win_rate=metrics.win_rate,
    profit_factor=metrics.profit_factor,
    avg_win_ticks=metrics.avg_win_ticks,
    avg_loss_ticks=metrics.avg_loss_ticks,
    max_drawdown_pct=metrics.max_drawdown_pct,
    backtest_period_start="2024-01-01",  # Your backtest start
    backtest_period_end="2024-12-01",    # Your backtest end
    sample_size=len(backtest_trades)
)

# Embed and save metadata
embed_performance_metadata(trained_model, expected_perf, Path("src/best_agent_model.keras"))
```

**Loading Implementation (MTS - Already Implemented):**
```python
import json
from pathlib import Path

def load_model_config(model_path: Path) -> dict:
    """Extract model config from .keras metadata."""
    with zipfile.ZipFile(model_path, 'r') as zf:
        if 'metadata.json' in zf.namelist():
            metadata_json = zf.read('metadata.json').decode('utf-8')
            return json.loads(metadata_json)
    return {}  # Fallback to defaults

def load_expected_performance_from_keras(model_path: Path) -> ExpectedPerformance:
    """
    Load ExpectedPerformance from .keras model metadata.
    
    Returns hardcoded defaults if metadata missing (backward compatibility).
    """
    config = load_model_config(model_path)
    
    if 'model_config' in config and 'expected_performance' in config['model_config']:
        perf_dict = config['model_config']['expected_performance']
        return ExpectedPerformance(**perf_dict)
    
    # Fallback defaults (remove after all models have metadata)
    print("⚠️ Model metadata missing - using default ExpectedPerformance")
    return ExpectedPerformance(
        sharpe_ratio=1.85,
        win_rate=0.58,
        profit_factor=1.75,
        avg_win_ticks=15.5,
        avg_loss_ticks=10.7,
        max_drawdown_pct=8.5,
        backtest_period_start="2024-01-01",
        backtest_period_end="2024-12-01",
        sample_size=1000
    )
```

**⚠️ CRITICAL FOR TRANSFORMERAGENT TEAM:**

1. **Where to implement:** Create `src/backtest_metrics.py` in your TransformerAgent repo
2. **What to implement:**
   - `ExpectedPerformance` dataclass (copy from above - must match MTS exactly)
   - `PerformanceMetricsCalculator` class (same formulas as MTS `src/model_health.py`)
   - `embed_performance_metadata()` function (copy from above)
3. **When to call:** At the END of your training pipeline, after backtest validation
4. **Required fields:** All 9 fields in ExpectedPerformance must be populated from backtest data
5. **Testing:** After embedding, use the loading function to verify metadata is readable

**Validation Checklist:**
```python
# Add this test to your training script
def validate_embedded_metadata(model_path: Path):
    """Verify metadata was embedded correctly."""
    config = load_model_config(model_path)
    
    assert 'model_config' in config, "Missing model_config"
    assert 'expected_performance' in config['model_config'], "Missing expected_performance"
    
    perf = config['model_config']['expected_performance']
    required_fields = ['sharpe_ratio', 'win_rate', 'profit_factor', 
                       'avg_win_ticks', 'avg_loss_ticks', 'max_drawdown_pct',
                       'backtest_period_start', 'backtest_period_end', 'sample_size']
    
    for field in required_fields:
        assert field in perf, f"Missing required field: {field}"
    
    print("✅ Metadata validation passed")

# Call after embedding:
embed_performance_metadata(model, expected_perf, model_path)
validate_embedded_metadata(model_path)
```

### 11.4 Integration with Existing GUI ✅ RESOLVED
- **Q:** Should Performance Attribution Engine integrate with existing TradeAnalytics dashboard?
- **Decision:** ✅ **Add Performance Attribution directly to TradeAnalytics dashboard**
- **Existing:** `trade_analytics.py` already displays Sharpe Ratio, equity curve, rolling metrics, P&L distribution, and real-time updates via Firestore
- **Implementation:** Add "Model Health Status" card showing:
  - Health status badge (🟢 HEALTHY / 🟡 WARNING / 🔴 SOFT_LOCKED)
  - Alpha slippage percentage
  - Expected vs Realized metrics comparison
  - Last calculation timestamp
  - Sample size indicator
- **No Background Daemon:** Dashboard callback queries Firestore directly using ModelHealthStatusManager

### 11.5 Firestore Write Permissions ✅ RESOLVED
- **Q:** Should Performance Attribution Engine write back to Firestore?
- **Decision:** ✅ **Yes - use existing FirestoreManager.update_trade() method**
- **Use Cases:** 
  - Store calculated metrics (Sharpe, win rate) per trade
  - Flag underperforming trades for review
  - Store attribution metadata (confidence, mae_ticks, mfe_ticks)
- **Implementation:** 
  - FirestoreManager already provides `update_trade(firestore_doc_id, values)` method
  - Used by TradeServer for CloseTrade updates (see [trade_server.py:85](trade_server.py#L85))
  - Performance Attribution Engine can use same method to write calculated metrics
- **Rationale:** Infrastructure already exists and is production-proven, no reason not to use it

### 11.6 Multiple Models
- **Q:** Should engine track health per model or aggregate across all models?
- **Consideration:** If trading ES, NQ, YM with different models, degrade independently or together?
- **Recommendation:** Single aggregate health for Phase 1, per-model tracking in Phase 4

### 11.7 Alert Notifications
- **Q:** Should engine send alerts (email/Slack) or just write file?
- **Decision:** File-only for Phase 1, add notifications in Phase 4
- **Rationale:** Keep Phase 1 simple, add complexity later

### 11.8 Portfolio Volatility
- **Q:** Should alpha calculation use actual portfolio volatility or assume constant?
- **Current:** Using Sharpe ratio directly (implicitly assumes constant vol)
- **Alternative:** Track portfolio volatility and calculate alpha properly
- **Decision:** Current approach is sufficient if expected/realized use same vol

### 11.9 Performance Window ✅ RESOLVED
- **Q:** Should we use 30-day calendar window or 30 trading days?
- **Decision:** ✅ **30 calendar days**
- **Rationale:** 
  - Simpler implementation (no need to track trading day counter)
  - Firestore queries naturally filter to actual trading days (only closed trades exist)
  - Consistent with how traders think about monthly performance
  - Easy to extend window (60 days, 90 days) without complex date logic
- **Implementation:** `rolling_window_days: 30` in config

### 11.10 Tick Value Mapping ✅ RESOLVED
- **Q:** How to convert P&L dollars to ticks for different symbols?
- **Decision:** ✅ **Add tick value mapping to config file**
- **Implementation:** Config contains `tick_values` dict (ES: 12.50, NQ: 5.00, etc.)

---

## 12. Success Criteria

**Phase 1 Complete When:**
- [ ] TradeServer initializes ModelHealthStatusManager
- [ ] Health status calculated and returned in CloseTrade REP response
- [ ] Dashboard queries Firestore and calculates health status on-demand
- [ ] Manual testing confirms all three health states work (HEALTHY/WARNING/SOFT_LOCKED)
- [ ] C++ receives and parses health status from ZMQ response
- [ ] Unit test coverage >80%

**Production Ready When:**
- [ ] Runs for 7 consecutive days without crashes
- [ ] Handles all error conditions gracefully (Firestore unavailable, calc errors, etc.)
- [ ] Health calculation completes in <200ms (doesn't slow CloseTrade)
- [ ] Logging provides sufficient observability
- [ ] Configuration changes work without code modifications
- [ ] Documented runbook for operations team

---

## 13. GUI Integration - Enhancing TradeAnalytics Dashboard

### 13.1 Current TradeAnalytics Capabilities

**Existing Dashboard** (`trade_analytics.py`) already provides:

**Metrics Currently Displayed:**
- ✅ **Risk-Adjusted Performance**: Sharpe Ratio, Sortino Ratio, Max Drawdown, Calmar Ratio
- ✅ **Trading Quality**: Avg Entry Grade, Avg Exit Grade, Avg Trade Grade
- ✅ **Profitability**: Expectancy
- ✅ **Visualizations**: Equity curve, drawdown overlay, rolling metrics, P&L distribution

**Data Source:** Uses `FirestoreManager.get_trades_and_stats()` - same data we'll use!

**Update Mechanism:** Real-time via Dash callbacks when tab is selected

### 13.2 Proposed Enhancement - Add Model Health Status Card

**New Component:** "AI Model Health" card positioned prominently at top of dashboard

```python
# Addition to TradeAnalytics class
def _create_model_health_card(self, stats_data: dict, health_status: dict) -> dbc.Card:
    """
    Create Model Health Status card for display in TradeAnalytics dashboard.
    
    Args:
        stats_data: Current realized performance metrics from Firestore
        health_status: Health status dict from ModelHealthStatusManager
    
    Returns:
        Dash Bootstrap Card component
    """
    status = health_status.get('status', 'UNKNOWN')
    alpha_slippage = health_status.get('alpha_slippage_pct', 0)
    sample_size = health_status.get('sample_size', 0)
    last_updated = health_status.get('last_updated', 'Never')
    
    # Status badge styling
    status_colors = {
        'HEALTHY': {'bg': 'success', 'icon': '🟢', 'text': 'Model Performing Well'},
        'WARNING': {'bg': 'warning', 'icon': '🟡', 'text': 'Performance Degraded'},
        'SOFT_LOCKED': {'bg': 'danger', 'icon': '🔴', 'text': 'AI Signals Disabled'}
    }
    
    style_info = status_colors.get(status, {'bg': 'secondary', 'icon': '⚪', 'text': 'Unknown'})
    
    return dbc.Card([
        dbc.CardHeader([
            html.Div([
                html.H5([
                    html.Span(style_info['icon'], className="me-2"),
                    "AI Model Health Status"
                ], className="mb-0 d-inline"),
                dbc.Badge(
                    status,
                    color=style_info['bg'],
                    className="float-end",
                    style={'fontSize': '1rem'}
                )
            ])
        ], className="bg-gradient-to-r from-gray-50 to-white border-b py-3"),
        
        dbc.CardBody([
            dbc.Row([
                # Status Description
                dbc.Col([
                    html.Div([
                        html.H6("Current Status", className="text-muted mb-2"),
                        html.H4(style_info['text'], className="mb-0")
                    ])
                ], width=4),
                
                # Alpha Slippage
                dbc.Col([
                    html.Div([
                        html.H6("Alpha Slippage", className="text-muted mb-2"),
                        html.H4([
                            html.Span(f"{alpha_slippage:.1f}%", 
                                     className=f"text-{style_info['bg']}")
                        ], className="mb-0")
                    ])
                ], width=2),
                
                # Expected vs Realized Sharpe
                dbc.Col([
                    html.Div([
                        html.H6("Sharpe Ratio", className="text-muted mb-2"),
                        html.Div([
                            html.Span(
                                f"Expected: {health_status.get('metrics', {}).get('expected_sharpe', 0):.2f}",
                                className="d-block text-sm"
                            ),
                            html.Span(
                                f"Realized: {health_status.get('metrics', {}).get('realized_sharpe', 0):.2f}",
                                className=f"d-block text-sm text-{style_info['bg']}"
                            )
                        ])
                    ])
                ], width=2),
                
                # Expected vs Realized Win Rate
                dbc.Col([
                    html.Div([
                        html.H6("Win Rate", className="text-muted mb-2"),
                        html.Div([
                            html.Span(
                                f"Expected: {health_status.get('metrics', {}).get('expected_winrate', 0):.1%}",
                                className="d-block text-sm"
                            ),
                            html.Span(
                                f"Realized: {health_status.get('metrics', {}).get('realized_winrate', 0):.1%}",
                                className=f"d-block text-sm text-{style_info['bg']}"
                            )
                        ])
                    ])
                ], width=2),
                
                # Sample Size & Last Updated
                dbc.Col([
                    html.Div([
                        html.H6("Monitoring", className="text-muted mb-2"),
                        html.Div([
                            html.Span(f"{sample_size} trades", className="d-block text-sm"),
                            html.Span(
                                f"Updated: {last_updated[:19] if last_updated != 'Never' else 'Never'}",
                                className="d-block text-sm text-muted"
                            )
                        ])
                    ])
                ], width=2)
            ], className="align-items-center")
        ], className="py-4"),
        
        # Optional: Alert message based on status
        dbc.CardFooter([
            _get_status_message(status, alpha_slippage)
        ], className="bg-light border-top")
    ], className="mb-4 shadow-lg border-l-4 border-{style_info['bg']}-500")


def _get_status_message(status: str, alpha_slippage: float) -> html.Div:
    """Generate contextual message based on health status."""
    messages = {
        'HEALTHY': html.Div([
            html.I(className="fas fa-check-circle text-success me-2"),
            html.Span("Model is performing within expected parameters. All AI signals are active.")
        ], className="text-success"),
        
        'WARNING': html.Div([
            html.I(className="fas fa-exclamation-triangle text-warning me-2"),
            html.Span([
                f"Performance has degraded by {alpha_slippage:.1f}%. ",
                html.Strong("Only HIGH confidence signals (≥0.70) are being accepted. "),
                "Position sizing reduced by 50%. Monitor closely for further degradation."
            ])
        ], className="text-warning"),
        
        'SOFT_LOCKED': html.Div([
            html.I(className="fas fa-ban text-danger me-2"),
            html.Span([
                html.Strong(f"CRITICAL: Performance degraded {alpha_slippage:.1f}%. "),
                "All AI signals are being rejected. Manual trading only. ",
                "Model requires immediate review and potential retraining."
            ])
        ], className="text-danger")
    }
    
    return messages.get(status, html.Div("Status unknown"))
```

### 13.3 Data Flow Integration

```
┌─────────────────────────────────────────────────────────────────┐
│                Performance Attribution Engine (Daemon)          │
│                                                                 │
│  Queries Firestore → Calculates Metrics → Writes JSON          │
│        ↓                    ↓                    ↓             │
│  get_trades()      Sharpe, Alpha      model_health_status.json │
└─────────────────────────────────────────────────────────────────┘
                              │                    │
                              │                    │
                              ↓                    ↓
                   ┌──────────────────┐   ┌──────────────────┐
                   │   TradeAnalytics │   │  C++ Trading     │
                   │   Dashboard      │   │  Engine          │
                   │                  │   │                  │
                   │  Displays:       │   │  Reads:          │
                   │  - Health Card   │   │  - Status        │
                   │  - Metrics       │   │  - Thresholds    │
                   │  - Charts        │   │  - Slippage      │
                   └──────────────────┘   └──────────────────┘
```

### 13.4 GUI Dashboard Refresh Strategy

**Implementation Philosophy:** KISS (Keep It Simple, Stupid)

The TradeAnalytics dashboard **updates when the user selects the tab**. No need for complex auto-refresh mechanisms.

**Simple File-Based Approach (Recommended):**

```python
# In trade_analytics.py - Model Health Card

def load_health_status() -> dict:
    """Load health status from JSON file."""
    health_path = Path("data/model_health_status.json")
    if not health_path.exists():
        return {
            "status": "UNKNOWN",
            "note": "Health status file not found"
        }
    
    with open(health_path) as f:
        return json.load(f)


@app.callback(
    Output('model-health-card', 'children'),
    [Input('interval-component', 'n_intervals')]  # Optional: 60-second refresh
)
def update_health_card(n_intervals):
    """
    Update health card display.
    Called when:
    1. User selects TradeAnalytics tab (automatic Dash behavior)
    2. Optional: Every 60 seconds via interval component
    """
    health_data = load_health_status()
    return create_health_card(health_data)
```

**Optional Enhancement: Manual Refresh Button**

If users want to force a refresh without switching tabs:

```python
@app.callback(
    Output('model-health-card', 'children'),
    [Input('refresh-health-button', 'n_clicks'),
     Input('interval-component', 'n_intervals')]
)
def update_health_card(n_clicks, n_intervals):
    """Update health card on button click or interval."""
    health_data = load_health_status()
    return create_health_card(health_data)

# Add button to layout
dbc.Button(
    "🔄 Refresh Health Status",
    id="refresh-health-button",
    color="secondary",
    size="sm"
)
```

**Why This Works:**
- With 5-20 trades/day, health status changes infrequently
- Users viewing dashboard want current snapshot, not real-time streaming
- Firestore query is fast (~10-50ms for 60 trades) - negligible overhead
- Tab switching automatically triggers callback (Dash default behavior)
- Optional 60-second interval ensures data stays fresh while viewing
- Single source of truth (Firestore) - no file synchronization issues

**No Need For:**
- ❌ Background daemon writing JSON files
- ❌ File watchers (watchdog library)
- ❌ WebSocket push notifications
- ❌ Complex event-driven architecture
- ❌ File synchronization between TradeServer and GUI

**Dashboard** simply queries Firestore on-demand when user views the tab, calculates metrics, and displays health status.

---

### 13.5 Implementation - Dashboard Callback

```python
# Add dcc.Store to hold trade event counter
dcc.Store(id='trade-close-event-counter', data=0)

# TradeServer publishes event when trade closes
class TradeServer:
    def handle_request(self, request):
        # ... existing code ...
        if message_type == "CloseTrade":
            # Update Firestore
            firestore_manager.update_trade(firestore_doc_id, update_data)
            
            # Publish event to dashboard (via shared memory, Redis, or ZMQ PUB)
            event_bus.publish('trade_closed', {'doc_id': firestore_doc_id})

# Dashboard listens for events
@app.callback(
    Output('trade-close-event-counter', 'data'),
    Input('trade-event-listener', 'n_intervals'),  # Fast polling (1s) for events
    State('trade-close-event-counter', 'data')
)
def check_for_trade_events(n, current_count):
    # Check event queue (Redis, ZMQ, or file-based flag)
    new_events = event_bus.get_events_since(current_count)
    return current_count + len(new_events)

# Health card updates when event counter changes
@app.callback(
    Output('model-health-card', 'children'),
    [Input(ANALYTICS_STATS_CACHE_STORE_ID, 'data'),
     Input('trade-close-event-counter', 'data')]
)
def update_model_health_card(stats_data: dict, event_count: int):
    # Read health status file
    # ...
```

**Pros:**
- ✅ **Immediate updates** - Traders see status change within 1-2 seconds
- ✅ **Efficient** - Only updates when trades actually close
- ✅ **Lower latency** - Real-time reflection of current state
- ✅ **Resource efficient** - No unnecessary file reads
- ✅ **Better UX** - Responsive dashboard that reacts to market activity
- ✅ **Captures rapid changes** - Shows all state transitions, not just final state

**Cons:**
- ❌ **Complex implementation** - Needs event bus (Redis, ZMQ PUB/SUB, or file flags)
- ❌ **Coupling** - TradeServer must know to publish events
- ❌ **Dependency risk** - If event system fails, dashboard stale until fixed
- ❌ **Race conditions** - Dashboard might update before PAE finishes calculating
- ❌ **Thundering herd** - Many trades closing rapidly could overwhelm system
- ❌ **Debugging harder** - Asynchronous event flow more complex to trace

#### Option C: Hybrid Approach (RECOMMENDED)

```python
# Combine both: Event-driven with periodic fallback
dcc.Store(id='trade-close-event-counter', data=0),
dcc.Interval(id='health-fallback-refresh', interval=5*60*1000, n_intervals=0),  # 5 min fallback

@app.callback(
    Output('model-health-card', 'children'),
    [Input(ANALYTICS_STATS_CACHE_STORE_ID, 'data'),
     Input('trade-close-event-counter', 'data'),      # Event-driven (primary)
     Input('health-fallback-refresh', 'n_intervals')]  # Periodic (fallback)
)
def update_model_health_card(stats_data: dict, event_count: int, fallback_count: int):
    # Read health status file
    # ...
```

**Pros:**
- ✅ **Best of both worlds** - Immediate updates + guaranteed refresh
- ✅ **Fault tolerant** - Works even if events fail
- ✅ **Lower latency** - Events provide real-time updates
- ✅ **Safety net** - Periodic refresh catches any missed events
- ✅ **Flexible** - Can adjust fallback interval (5 min, 10 min, etc.)

**Cons:**
- ❌ **Most complex** - Needs both mechanisms
- ❌ **Potential duplicate updates** - Event + fallback might overlap
- ❌ **Higher implementation cost** - More code to maintain

#### Comparison Table

| Aspect | Polling (60s) | Event-Driven | Hybrid |
|--------|--------------|--------------|--------|
| **Latency** | 0-60 seconds | 1-2 seconds | 1-2 seconds |
| **Complexity** | Low | High | Medium-High |
| **Resource Usage** | Medium | Low | Low-Medium |
| **Reliability** | High | Medium | Very High |
| **Fault Tolerance** | High | Low | High |
| **Implementation Time** | 1 hour | 4-8 hours | 6-10 hours |
| **Maintenance** | Easy | Moderate | Moderate |
| **Recommended For** | Phase 1 MVP | Production | Production+ |

#### Recommendation by Trade Volume

**Low Volume Trading (1-20 trades/day):** Use **Event-Driven with Fallback**
- 60 seconds is too long when trades are infrequent
- Each trade is significant - traders want immediate feedback
- Computational overhead is minimal with few events
- No "thundering herd" concerns
- **Recommendation:** File-based flag (simple, no dependencies) + 5-minute fallback

**High Volume Trading (50+ trades/day):** Use **Hybrid with Throttling**
- Events provide immediate feedback during active periods
- Throttle updates to avoid overwhelming dashboard
- Fallback polling handles quiet periods
- **Recommendation:** Full hybrid with event batching

**Phase 1 Implementation Path:**

For **low-volume trading** (your case):
1. **Start with file-based events** - Simple, 1-2 second latency
2. **Add 5-minute polling fallback** - Safety net
3. Total implementation time: 2-3 hours

For **high-volume trading**:
1. Start with 60-second polling (MVP)
2. Upgrade to event-driven in Phase 2
3. Add throttling and batching as needed

**Implementation Path:**

1. **Week 1:** Implement polling (60s) - works immediately
2. **Week 2:** Add event publishing to TradeServer (ZMQ PUB or file flag)
3. **Week 3:** Add event listener to dashboard (still keep polling as backup)
4. **Week 4:** Tune event system, extend polling to 5 minutes (fallback only)

#### Simple Event Bus (No External Dependencies)

If you don't want Redis/ZMQ complexity, use a **file-based flag**:

```python
# TradeServer writes flag on trade close
def handle_close_trade(self, message):
    # ... update Firestore ...
    
    # Write event flag (atomic)
    flag_file = Path('/tmp/trade_closed_event.flag')
    flag_file.write_text(str(time.time()))  # Timestamp

# Dashboard checks flag every second
@app.callback(
    Output('trade-close-event-counter', 'data'),
    Input('fast-poll-interval', 'n_intervals'),  # 1 second
    State('trade-close-event-counter', 'data')
)
def check_trade_close_flag(n, current_count):
    flag_file = Path('/tmp/trade_closed_event.flag')
    
    if flag_file.exists():
        flag_time = float(flag_file.read_text())
        
        # Check if flag is new (within last 2 seconds)
        if time.time() - flag_time < 2:
            flag_file.unlink()  # Consume flag
            return current_count + 1
    
    return current_count
```

**File-based flag pros:**
- ✅ No external dependencies (Redis, ZMQ, etc.)
- ✅ Simple to implement and debug
- ✅ Atomic writes prevent race conditions
- ✅ Easy to test manually (just create the file)
- ✅ **Perfect for low-volume trading** (1-20 trades/day)

**File-based flag cons:**
- ❌ Not as fast as in-memory event bus (1-2s vs <100ms)
- ❌ File I/O overhead (minimal for flags)
- ❌ Need cleanup if dashboard not running

#### Practical Implementation for Low-Volume Trading

**Step 1: Modify TradeServer to publish event flag**

```python
# In trade_server.py
import os
from pathlib import Path

class TradeServer:
    def __init__(self):
        # ... existing code ...
        self.event_flag_dir = Path('/tmp')  # Or configurable path
    
    def _publish_trade_closed_event(self):
        """Write event flag to notify dashboard of trade close."""
        try:
            flag_file = self.event_flag_dir / 'trade_closed_event.flag'
            # Write current timestamp
            flag_file.write_text(str(time.time()))
            logger.debug(f"Published trade closed event: {flag_file}")
        except Exception as e:
            logger.error(f"Failed to publish trade close event: {e}")
    
    def handle_request(self, request):
        # ... existing code ...
        
        if message_type == "CloseTrade":
            firestore_doc_id = message.get("firestore_doc_id")
            if firestore_doc_id:
                update_data = {
                    key: message[key]
                    for key in [
                        "exit_date", "exit_grade", "exit_price", "pnl",
                        "trade_grade", "trade_status"
                    ]
                    if key in message
                }

                if firestore_manager.update_trade(firestore_doc_id, update_data):
                    response_data["message"] = f"CloseTrade successful for doc_id: {firestore_doc_id}."
                    response_data["type"] = "CloseTradeResponse"
                    response_data["order_id"] = message.get("order_id")
                    logger.info(f"TradeServer:: CloseTrade message processed for doc_id: {firestore_doc_id}.")
                    
                    # PUBLISH EVENT - This is the new line!
                    self._publish_trade_closed_event()
                    
                else:
                    response_data["status"] = "error"
                    response_data["message"] = f"Failed to edit trade for doc_id: {firestore_doc_id}."
        
        # ... rest of code ...
```

**Step 2: Dashboard listens for event flags**

```python
# In trade_analytics.py (or separate module)
import time
from pathlib import Path

def check_for_trade_close_event(flag_path: Path, max_age_seconds: float = 2.0) -> bool:
    """
    Check if trade close event flag exists and is recent.
    
    Args:
        flag_path: Path to event flag file
        max_age_seconds: Maximum age of flag to consider valid (default 2 seconds)
    
    Returns:
        True if event detected, False otherwise
    """
    try:
        if flag_path.exists():
            # Read timestamp from flag
            flag_timestamp = float(flag_path.read_text().strip())
            current_time = time.time()
            
            # Check if flag is recent
            if current_time - flag_timestamp < max_age_seconds:
                # Consume the flag (delete it)
                flag_path.unlink()
                return True
    except Exception as e:
        logger.warning(f"Error checking trade close event: {e}")
    
    return False


# Add to TradeAnalytics class
class TradeAnalytics:
    def __init__(self):
        # ... existing code ...
        self.event_flag_path = Path('/tmp/trade_closed_event.flag')
        self.last_event_check = 0
    
    def register_callbacks(self, app: dash.Dash):
        # ... existing callbacks ...
        
        # Fast poll for event flags (1 second interval)
        @app.callback(
            Output('trade-close-event-counter', 'data'),
            Input('trade-event-poll', 'n_intervals'),
            State('trade-close-event-counter', 'data'),
            prevent_initial_call=True
        )
        def poll_for_trade_events(n_intervals: int, current_count: int) -> int:
            """
            Poll for trade close event flags every second.
            When flag detected, increment counter to trigger health card update.
            """
            if check_for_trade_close_event(self.event_flag_path):
                logger.info("Trade close event detected - triggering health update")
                return (current_count or 0) + 1
            
            return current_count or 0
        
        # Health card updates on event counter change OR periodic fallback
        @app.callback(
            Output('model-health-card', 'children'),
            [Input(ANALYTICS_STATS_CACHE_STORE_ID, 'data'),
             Input('trade-close-event-counter', 'data'),       # Event-driven (primary)
             Input('health-fallback-refresh', 'n_intervals')], # 5-min fallback
            prevent_initial_call=False
        )
        def update_model_health_card(stats_data: dict, 
                                     event_count: int, 
                                     fallback_count: int):
            """
            Update model health status card.
            Triggers on trade close events (1-2s latency) or every 5 minutes (fallback).
            """
            ctx = dash.callback_context
            
            if ctx.triggered:
                trigger_id = ctx.triggered[0]['prop_id'].split('.')[0]
                logger.debug(f"Health card update triggered by: {trigger_id}")
            
            # Read health status file
            health_file = Path('/home/trader/data/model_health_status.json')
            
            if health_file.exists():
                try:
                    with open(health_file) as f:
                        health_status = json.load(f)
                except Exception as e:
                    logger.error(f"Failed to read health status: {e}")
                    health_status = {'status': 'UNKNOWN'}
            else:
                health_status = {
                    'status': 'UNKNOWN',
                    'alpha_slippage_pct': 0,
                    'sample_size': 0,
                    'last_updated': 'Never'
                }
            
            return _create_model_health_card(stats_data or {}, health_status)
    
    def render(self) -> html.Div:
        """Updated render with event-driven components."""
        return html.Div([
            # ... existing header and cards ...
            
            # Event tracking storage
            dcc.Store(id='trade-close-event-counter', data=0),
            
            # Fast polling for event flags (1 second)
            dcc.Interval(
                id='trade-event-poll',
                interval=1*1000,  # 1 second
                n_intervals=0
            ),
            
            # Slow fallback polling (5 minutes)
            dcc.Interval(
                id='health-fallback-refresh',
                interval=5*60*1000,  # 5 minutes
                n_intervals=0
            ),
            
            # Model health card (populated by callback)
            dbc.Row([
                dbc.Col([
                    html.Div(id='model-health-card')
                ], width=12)
            ], className="mb-4"),
            
            # ... rest of layout ...
        ])
```

**Step 3: Configuration**

```yaml
# config/performance_attribution.yaml
event_notification:
  enabled: true
  method: "file_flag"  # Simple file-based flag
  flag_path: "/tmp/trade_closed_event.flag"
  dashboard_poll_interval_ms: 1000  # Check for events every 1 second
  fallback_interval_minutes: 5      # Fallback polling every 5 minutes
```

#### Timeline Comparison for Low-Volume Trading

**Scenario:** 5 trades per day, one trade just closed

| Approach | Latency | Trader Experience |
|----------|---------|-------------------|
| **60-second polling** | 0-60 seconds | "Why hasn't it updated yet?" 😟 |
| **Event-driven (file)** | 1-2 seconds | "Perfect, instant feedback!" 😊 |
| **Event + 5-min fallback** | 1-2 seconds (normal)<br/>0-5 min (if event missed) | "Always reliable!" 😎 |

**With only 5 trades/day:**
- Event overhead: Minimal (5 file writes + 5 reads)
- Benefit: Immediate feedback on every trade
- User satisfaction: Much higher
- Implementation time: +2 hours vs polling-only

### 13.5 Implementation - Dashboard Callback

```python
# Add to TradeAnalytics.register_callbacks()
@app.callback(
    Output('model-health-card', 'children'),
    [Input(ANALYTICS_STATS_CACHE_STORE_ID, 'data'),
     Input('health-status-refresh-interval', 'n_intervals')]  # Auto-refresh every 60s
)
def update_model_health_card(stats_data: dict, n_intervals: int):
    """
    Update model health status card with latest data.
    
    Queries Firestore directly and calculates metrics on-demand.
    """
    try:
        # Import performance attribution components
        from performance_attribution.models import ExpectedPerformance, HealthThresholds
        from performance_attribution.health_manager import ModelHealthStatusManager
        from firestore_manager import firestore_manager
        
        # Initialize health manager (same config as TradeServer)
        expected = ExpectedPerformance(
            sharpe_ratio=1.85,
            win_rate=0.58,
            profit_factor=1.75
        )
        
        thresholds = HealthThresholds(
            warning_threshold_pct=20.0,
            soft_lock_threshold_pct=30.0,
            min_sample_size=20
        )
        
        health_manager = ModelHealthStatusManager(thresholds, expected, firestore_manager)
        
        # Calculate current health (queries Firestore, calculates metrics)
        health_status = health_manager.calculate_current_health()
        
    except Exception as e:
        logger.error(f"Failed to calculate health status: {e}", exc_info=True)
        # Fallback to unknown status
        health_status = {
            'status': 'UNKNOWN',
            'alpha_slippage_pct': 0,
            'sample_size': 0,
            'last_updated': 'Error',
            'error': str(e)
        }
    
    return _create_model_health_card(stats_data or {}, health_status)
```

### 13.6 Layout Integration Point

**Add to `TradeAnalytics.render()` method:**

```python
# After the header section, before key metrics cards
dbc.Row([
    dbc.Col([
        html.Div(id='model-health-card')  # Populated by callback
    ], width=12)
], className="mb-4"),

# Add auto-refresh interval
dcc.Interval(
    id='health-status-refresh-interval',
    interval=60*1000,  # Refresh every 60 seconds
    n_intervals=0
),
```

### 13.7 Benefits of GUI Integration

1. **Unified Dashboard** - Traders see all metrics in one place
2. **Fresh Data** - Direct Firestore queries ensure latest information
3. **No Synchronization** - Single source of truth eliminates file sync issues
4. **Context Awareness** - Health status next to performance metrics
5. **Action Clarity** - Clear messages about what's happening (signals disabled, etc.)
6. **Simple Architecture** - No background daemons or file watching needed
7. **Decision Support** - Visual cues help traders understand when to intervene

### 13.8 Dashboard Integration Checklist

- [ ] Add `model-health-card` div to TradeAnalytics layout
- [ ] Add `dcc.Interval` for optional auto-refresh (60 seconds)
- [ ] Implement `_create_model_health_card()` function
- [ ] Implement `update_model_health_card()` callback with ModelHealthStatusManager
- [ ] Import performance_attribution components in dashboard
- [ ] Test with various health states (HEALTHY, WARNING, SOFT_LOCKED)
- [ ] Add error handling for Firestore/calculation failures
- [ ] Style card to match existing dashboard theme
- [ ] Add tooltips explaining each metric
- [ ] Test refresh behavior (tab switch, interval)

---

## Appendix A: File Structure

```
src/
├── __init__.py
├── models.py               # Trade, PerformanceMetrics dataclasses
├── calculator.py           # PerformanceMetrics calculator
├── comparison.py           # PerformanceComparison, alpha slippage
├── health_manager.py       # ModelHealthStatusManager
├── config.py               # Configuration loading (thresholds, expected performance)
└── utils.py                # Utilities (logging, validation)

tests/
├── test_models.py
├── test_firestore_collector.py  # Test Firestore integration
├── test_calculator.py
├── test_comparison.py
├── test_health_writer.py
├── test_engine.py
└── test_integration.py

config/
└── performance_attribution.yaml

scripts/
├── create_test_trades.py   # Generate stub data in Firestore
└── run_once.py             # One-off execution (non-daemon)

docs/
├── PYTHON_PERFORMANCE_ATTRIBUTION_ENGINE_SPEC.md  # This document
├── AI_MODEL_HEALTH_STATUS_REQUIREMENTS.md         # C++ integration spec
└── RUNBOOK.md                                     # Operations guide

# Shared with existing GUI application
firestore_manager.py    # EXISTING - reuse for trade access
trade_server.py         # EXISTING - receives C++ trade messages
config.py               # EXISTING - ZMQ endpoints, etc.
app.py                  # EXISTING - GUI dashboard
```

---

## Appendix B: Dependencies

```requirements.txt
# Core dependencies
numpy>=1.24.0
pandas>=2.0.0
pyyaml>=6.0

# Future dependencies (Phase 4+)
# pyzmq>=25.0.0          # ZMQ streaming
# flask>=2.3.0           # Health check endpoint
# requests>=2.31.0       # HTTP notifications
```

---

## Appendix C: Architecture Decision - .keras Metadata vs Separate Config Files

### Decision Summary ✅ 
**Embed expected performance metrics in .keras model file metadata instead of using separate JSON/YAML config files.**

### Rationale

**Problems with separate config files (best_model_config.json):**
1. **Synchronization risk:** Model and config can become mismatched after updates
2. **Version control complexity:** Two files to manage for one logical artifact
3. **Deployment overhead:** Must deploy model + config together
4. **Human error:** Easy to forget updating config after retraining

**Benefits of .keras metadata approach:**
1. **Single source of truth:** Model and expected performance travel together
2. **Atomic updates:** Training process embeds metrics in same .keras file
3. **Version control simplicity:** One file to track, one file to deploy
4. **Backward compatibility:** Can fall back to defaults if metadata missing
5. **Self-documenting:** Model file contains its own benchmark data

### Implementation

**During training (TransformerAgent team - LBRNet_v2 project):**
```python
# In src/train.py or new src/backtest_metrics.py
import keras
from pathlib import Path
from typing import Dict, Any
import numpy as np

@dataclass
class ExpectedPerformance:
    """Expected performance metrics from backtesting (same structure as MTS)."""
    sharpe_ratio: float
    win_rate: float
    profit_factor: float
    sortino_ratio: float = 0.0
    max_drawdown_pct: float = 0.0
    avg_win_ticks: float = 0.0
    avg_loss_ticks: float = 0.0
    total_trades: int = 0
    total_pnl: float = 0.0

def embed_performance_metadata(
    model_path: str,
    backtest_results: Dict[str, Any]
) -> None:
    """
    Save performance metadata to a companion JSON file alongside the .keras model.
    Creates: src/best_agent_model_metadata.json
    
    Args:
        model_path: Path to the .keras model file (e.g., 'src/best_agent_model.keras')
        backtest_results: Dictionary with backtest performance metrics
        
    Usage:
        After training completes and backtest runs:
        
        backtest_results = {
            'sharpe_ratio': 1.85,
            'win_rate': 0.58,
            'profit_factor': 1.75,
            'sortino_ratio': 2.15,
            'max_drawdown_pct': -12.5,
            'avg_win_ticks': 8.3,
            'avg_loss_ticks': 6.1,
            'total_trades': 247,
            'total_pnl': 12450.0
        }
        embed_performance_metadata('src/best_agent_model.keras', backtest_results)
    """
    # Load the trained model (not modified, just for validation)
    model = keras.models.load_model(model_path)
    
    # Create metadata dictionary
    model_metadata = {
        "expected_performance": {
            "sharpe_ratio": float(backtest_results['sharpe_ratio']),
            "win_rate": float(backtest_results['win_rate']),
            "profit_factor": float(backtest_results['profit_factor']),
            "sortino_ratio": float(backtest_results.get('sortino_ratio', 0.0)),
            "max_drawdown_pct": float(backtest_results.get('max_drawdown_pct', 0.0)),
            "avg_win_ticks": float(backtest_results.get('avg_win_ticks', 0.0)),
            "avg_loss_ticks": float(backtest_results.get('avg_loss_ticks', 0.0)),
            "total_trades": int(backtest_results.get('total_trades', 0)),
            "total_pnl": float(backtest_results.get('total_pnl', 0.0))
        },
        "training_metadata": {
            "training_date": backtest_results.get('training_date', ''),
            "model_version": backtest_results.get('model_version', 'unknown'),
            "backtest_period": backtest_results.get('backtest_period', '')
        }
    }
    
    # Re-save with metadata
    model.save(model_path, save_metadata=model_metadata)
    print(f"✅ Embedded performance metrics in {model_path}")
    print(f"   Sharpe: {backtest_results['sharpe_ratio']:.2f}, "
          f"Win Rate: {backtest_results['win_rate']:.1%}, "
          f"PF: {backtest_results['profit_factor']:.2f}")
```

**During deployment (Performance Attribution Engine - MTS project):**
```python
# In mts_analysis/trade_server.py or src/model_health.py
def load_expected_performance_from_keras(model_path: Path) -> ExpectedPerformance:
    """
    Load expected performance metrics from .keras model metadata.
    
    Returns ExpectedPerformance with hardcoded defaults if metadata missing.
    """
    try:
        import keras
        model = keras.models.load_model(model_path)
        
        # Extract metadata
        metadata = model.metadata if hasattr(model, 'metadata') else {}
        expected_data = metadata.get('expected_performance', {})
        
        if not expected_data:
            logger.warning(f"No expected_performance metadata in {model_path}. Using defaults.")
            return ExpectedPerformance(
                sharpe_ratio=1.85,
                win_rate=0.58,
                profit_factor=1.75
            )
        
        return ExpectedPerformance(
            sharpe_ratio=expected_data['sharpe_ratio'],
            win_rate=expected_data['win_rate'],
            profit_factor=expected_data['profit_factor'],
            sortino_ratio=expected_data.get('sortino_ratio', 0.0),
            max_drawdown_pct=expected_data.get('max_drawdown_pct', 0.0),
            avg_win_ticks=expected_data.get('avg_win_ticks', 0.0),
            avg_loss_ticks=expected_data.get('avg_loss_ticks', 0.0),
            total_trades=expected_data.get('total_trades', 0),
            total_pnl=expected_data.get('total_pnl', 0.0)
        )
    except Exception as e:
        logger.error(f"Failed to load metadata from {model_path}: {e}")
        return ExpectedPerformance(
            sharpe_ratio=1.85,
            win_rate=0.58,
            profit_factor=1.75
        )
```

### Migration Path

1. **Phase 1:** Support both approaches (metadata + fallback defaults)
2. **Training update:** TransformerAgent embeds metrics in next retrain
3. **Deprecation:** Remove best_model_config.json once metadata proven stable
4. **Future:** All new models use embedded metadata

---

## Appendix C.1: TransformerAgent Project Implementation Guide

**Project:** `LBRNet_v2` (AI Model Training & Backtesting)  
**Location:** Separate repository from MTS GUI/TradeServer  
**Role:** Produces trained models with embedded performance benchmarks

### What Your Project Needs to Implement

#### 1. Calculate Performance Metrics During Backtesting

Your existing `src/backtest.py` already simulates trades. You need to add performance metric calculation:

**Create `src/backtest_metrics.py`:**
```python
"""
Performance metrics calculation for backtesting.
Generates ExpectedPerformance baseline for model health monitoring.

This module is used by the TransformerAgent training project to calculate
performance metrics that will be embedded in the .keras model file.
"""
from dataclasses import dataclass
import numpy as np
import pandas as pd
from typing import List, Dict, Any

@dataclass
class BacktestTrade:
    """Simple trade record for backtest performance calculation."""
    entry_price: float
    exit_price: float
    pnl: float
    side: str  # 'LONG' or 'SHORT'
    entry_date: str
    exit_date: str
    mae_ticks: float = 0.0  # Maximum Adverse Excursion
    mfe_ticks: float = 0.0  # Maximum Favorable Excursion

@dataclass  
class ExpectedPerformance:
    """
    Expected performance metrics from backtesting.
    These become the baseline for live performance monitoring.
    """
    sharpe_ratio: float
    win_rate: float
    profit_factor: float
    sortino_ratio: float = 0.0
    max_drawdown_pct: float = 0.0
    avg_win_ticks: float = 0.0
    avg_loss_ticks: float = 0.0
    total_trades: int = 0
    total_pnl: float = 0.0

class BacktestMetricsCalculator:
    """Calculate performance metrics from backtest trade history."""
    
    def __init__(self, risk_free_rate: float = 0.02):
        self.risk_free_rate = risk_free_rate
    
    def calculate_metrics(self, trades: List[BacktestTrade]) -> ExpectedPerformance:
        """
        Calculate all performance metrics from backtest trades.
        
        Args:
            trades: List of completed backtest trades
            
        Returns:
            ExpectedPerformance with calculated metrics
        """
        if not trades:
            return ExpectedPerformance(
                sharpe_ratio=0.0,
                win_rate=0.0,
                profit_factor=0.0
            )
        
        # Convert to DataFrame for easier calculations
        df = pd.DataFrame([{
            'pnl': t.pnl,
            'entry_date': t.entry_date,
            'exit_date': t.exit_date,
            'mae_ticks': t.mae_ticks,
            'mfe_ticks': t.mfe_ticks
        } for t in trades])
        
        # Basic metrics
        total_pnl = df['pnl'].sum()
        total_trades = len(trades)
        winners = df[df['pnl'] > 0]
        losers = df[df['pnl'] < 0]
        
        win_rate = len(winners) / total_trades if total_trades > 0 else 0.0
        
        # Profit Factor
        total_wins = winners['pnl'].sum() if not winners.empty else 0.0
        total_losses = abs(losers['pnl'].sum()) if not losers.empty else 0.0
        profit_factor = total_wins / total_losses if total_losses > 0 else 0.0
        
        # Sharpe Ratio (annualized)
        returns = df['pnl']
        mean_return = returns.mean()
        std_return = returns.std()
        sharpe = ((mean_return - self.risk_free_rate) / std_return * np.sqrt(252)) if std_return > 0 else 0.0
        
        # Sortino Ratio (downside deviation only)
        downside_returns = returns[returns < 0]
        downside_std = downside_returns.std() if not downside_returns.empty else std_return
        sortino = ((mean_return - self.risk_free_rate) / downside_std * np.sqrt(252)) if downside_std > 0 else 0.0
        
        # Max Drawdown (simplified - needs equity curve)
        cumulative_pnl = df['pnl'].cumsum()
        running_max = cumulative_pnl.expanding().max()
        drawdown = cumulative_pnl - running_max
        max_drawdown_pct = (drawdown.min() / running_max.max() * 100) if running_max.max() > 0 else 0.0
        
        # MAE/MFE averages
        avg_win_ticks = winners['mfe_ticks'].mean() if not winners.empty else 0.0
        avg_loss_ticks = abs(losers['mae_ticks'].mean()) if not losers.empty else 0.0
        
        return ExpectedPerformance(
            sharpe_ratio=sharpe,
            win_rate=win_rate,
            profit_factor=profit_factor,
            sortino_ratio=sortino,
            max_drawdown_pct=max_drawdown_pct,
            avg_win_ticks=avg_win_ticks,
            avg_loss_ticks=avg_loss_ticks,
            total_trades=total_trades,
            total_pnl=total_pnl
        )

def embed_performance_metadata(model_path: str, backtest_results: Dict[str, Any]) -> None:
    """Embed backtest performance metrics in .keras model file."""
    # [Implementation from Appendix C above]
    pass
```

#### 2. Integrate with Your Training Pipeline

**Update `src/train.py` to call embedding after training:**
```python
# At the end of train_transformer_agent()
def train_transformer_agent(X_train, y_train, epochs=10, batch_size=32):
    # ... existing training code ...
    
    print("\n--- Training Complete ---")
    
    # NEW: Run backtest and embed metrics
    print("\n--- Running Backtest to Calculate Expected Performance ---")
    from backtest import Backtester  # Your existing backtester
    from backtest_metrics import BacktestMetricsCalculator, embed_performance_metadata
    
    # Run backtest (you already have this logic somewhere)
    backtester = Backtester(model_path='src/best_agent_model.keras')
    backtest_trades = backtester.run()  # Returns list of trades
    
    # Calculate metrics
    calculator = BacktestMetricsCalculator()
    expected_perf = calculator.calculate_metrics(backtest_trades)
    
    # Convert to dict for embedding
    backtest_results = {
        'sharpe_ratio': expected_perf.sharpe_ratio,
        'win_rate': expected_perf.win_rate,
        'profit_factor': expected_perf.profit_factor,
        'sortino_ratio': expected_perf.sortino_ratio,
        'max_drawdown_pct': expected_perf.max_drawdown_pct,
        'avg_win_ticks': expected_perf.avg_win_ticks,
        'avg_loss_ticks': expected_perf.avg_loss_ticks,
        'total_trades': expected_perf.total_trades,
        'total_pnl': expected_perf.total_pnl,
        'training_date': datetime.now().strftime('%Y-%m-%d'),
        'model_version': RUN_VERSION  # From config.py
    }
    
    # Save metadata to companion JSON file
    embed_performance_metadata('src/best_agent_model.keras', backtest_results)
    
    return history
```

### What You DON'T Need to Implement

❌ **TradeServer** - Not applicable (you're not receiving live trades)  
❌ **FirestoreManager** - Not applicable (you're not storing live trades)  
❌ **ModelHealthStatusManager** - Not applicable (you're not monitoring live performance)  
❌ **TradeServer ZMQ Integration** - Not applicable (different project)  
❌ **GUI Dashboard Updates** - Not applicable (different project)  
❌ **Phase 1/2/3 Implementation** - These are for the MTS project

### Your Deliverable

When your training completes, you produce:
- **`src/best_agent_model.keras`** with embedded architecture config (vocab_size, etc.)
- **`src/best_agent_model_metadata.json`** with performance metrics (sharpe, win_rate, etc.)
- This file is copied to the C++/MTS project where it's loaded and monitored

The MTS project will then:
- Load your model
- Extract ExpectedPerformance from metadata
- Compare live performance vs. your baseline
- Generate health status (HEALTHY/WARNING/SOFT_LOCKED)

---

## Appendix D: Phase 1 Implementation Notes - Defensive Column Handling

### Background
During initial implementation (December 21, 2025), we discovered that existing Firestore trades do not have Phase 1 fields (`confidence`, `mae_ticks`, `mfe_ticks`) since they were created before the C++ integration was completed. The `TradeDocument` dataclass was extended with backward-compatible defaults (all fields = 0.0), but existing database documents lack these columns entirely.

### Issue
When `PerformanceMetricsCalculator.calculate_metrics()` attempted to access Phase 1 columns in the DataFrame, it raised `KeyError` because the columns don't exist in trades retrieved from Firestore:

```python
# Original code (fails with KeyError when columns missing):
avg_win_ticks = winners['mfe_ticks'].mean() if not winners.empty else 0.0
avg_loss_ticks = abs(losers['mae_ticks'].mean()) if not losers.empty else 0.0
```

### Solution
Added defensive column checks to handle missing Phase 1 fields gracefully:

```python
# Defensive code (handles missing columns):
if 'mfe_ticks' in winners.columns and not winners.empty:
    avg_win_ticks = winners['mfe_ticks'].mean()
else:
    avg_win_ticks = 0.0

if 'mae_ticks' in losers.columns and not losers.empty:
    avg_loss_ticks = abs(losers['mae_ticks'].mean())
else:
    avg_loss_ticks = 0.0
```

**Location:** `src/model_health.py`, `PerformanceMetricsCalculator.calculate_metrics()` method (lines ~139-147)

### Migration Path

**Immediate (Phase 1):**
- ✅ Defensive column checks allow calculator to work with existing trades
- ✅ New trades from C++ will have Phase 1 fields populated
- ✅ Calculator returns 0.0 for avg_win_ticks/avg_loss_ticks when columns missing

**Future (When All Trades Have Phase 1 Data):**
Once the C++ trading engine has been running long enough that all active trades in the rolling 30-day window have Phase 1 fields populated, the defensive checks can be simplified:

```python
# TODO: Remove defensive checks once all Firestore trades have Phase 1 fields
# Original simplified version (restore after migration complete):
avg_win_ticks = winners['mfe_ticks'].mean() if not winners.empty else 0.0
avg_loss_ticks = abs(losers['mae_ticks'].mean()) if not losers.empty else 0.0
```

**Timeline:**
- Current state: Mix of old trades (no Phase 1 fields) and new trades (with Phase 1 fields)
- Target state: After ~30 days of live C++ operation, all trades in rolling window will have Phase 1 fields
- Cleanup: Remove defensive checks in `calculate_metrics()` for cleaner code

### Testing Results (2025-12-21)
```
✅ PerformanceMetricsCalculator
Retrieved 86 trades from Firestore
Calculated Metrics:
  Sharpe Ratio: -6.2158
  Sortino Ratio: -12.1551
  Win Rate: 37.29%
  Profit Factor: 0.46
  Max Drawdown: -1.27%
  Total Trades: 59
  Total PnL: $-103.00

✅ ModelHealthStatusManager
Health Status:
  Status: INSUFFICIENT_DATA
  Alpha Slippage: 0.00%
  Message: Insufficient data: 0 trades (need 20)
```

**Note:** `INSUFFICIENT_DATA` status is expected because the 30-day rolling window doesn't have 20+ closed trades yet. This is correct behavior per `min_sample_size` threshold (20 trades = statistically significant per Mark Douglas).

---

## Appendix E: Phase 1 Implementation Summary (2025-12-21)

### Implementation Checklist ✅

**Step 1: Data Model Extension**
- ✅ Extended `TradeDocument` dataclass in firestore_manager.py (lines 267-269)
- ✅ Added Phase 1 fields: confidence, mae_ticks, mfe_ticks (all with default=0.0)
- ✅ Updated `_generate_sample_data()` with realistic test values
- ✅ Backward compatible - existing trades unaffected

**Step 2: Performance Attribution Engine**
- ✅ Created `src/model_health.py` (480 lines)
- ✅ Implemented PerformanceMetrics dataclass (12 fields including metadata)
- ✅ Implemented ExpectedPerformance dataclass (baseline from backtest)
- ✅ Implemented HealthThresholds dataclass (warning=20%, soft_lock=30%, min_sample=20)
- ✅ Implemented PerformanceMetricsCalculator with defensive column handling
- ✅ Implemented ModelHealthStatusManager with 30-day rolling window and 60-second cache

**Step 3: TradeServer Integration**
- ✅ Added imports for ModelHealthStatusManager, ExpectedPerformance, HealthThresholds
- ✅ Initialized health manager in TradeServer.__init__() with default expected performance
- ✅ Added health calculation to CloseTrade handler (after successful trade update)
- ✅ Returns model_health dict in response: {status, alpha_slippage_pct, message}
- ✅ Graceful error handling - trade succeeds even if health calculation fails

**Step 4: Configuration**
- ✅ Added PERFORMANCE_ATTRIBUTION_CONFIG to config.py
- ✅ Set min_sample_size=20 (Mark Douglas: statistically significant)
- ✅ Set rolling_window_days=30, risk_free_rate=0.02, cache_duration_seconds=60

**Step 5: Testing & Validation**
- ✅ Unit tested PerformanceMetricsCalculator with existing Firestore data
- ✅ Unit tested ModelHealthStatusManager with mock ExpectedPerformance
- ✅ Integration tested TradeServer.handle_request() with CloseTrade messages
- ✅ Verified model_health correctly included in response
- ✅ Verified INSUFFICIENT_DATA status with appropriate message

### Key Implementation Details

**Defensive Coding:**
- Calculator checks for Phase 1 column existence before accessing
- Returns 0.0 for avg_win_ticks/avg_loss_ticks when columns missing
- Allows system to work during migration period (mixed old/new trades)

**Health Status States:**
- `HEALTHY`: Alpha slippage ≤ 20%
- `WARNING`: Alpha slippage > 20% but ≤ 30%
- `SOFT_LOCKED`: Alpha slippage > 30%
- `INSUFFICIENT_DATA`: Sample size < 20 trades in 30-day window
- `ERROR`: Health calculation failed (fallback state)

**Performance Optimizations:**
- 60-second cache in ModelHealthStatusManager reduces Firestore queries
- Single Firestore query for 30-day window (timestamp filter)
- DataFrame-based calculations for efficient metric computation

**Current Limitations (Phase 2b):**
- ExpectedPerformance hardcoded in TradeServer (not loaded from .keras metadata - Phase 2b in progress)
- No historical health tracking chart yet (Phase 3)
- Defensive column checks required until all trades have Phase 1 fields (temporary migration state)

### Phase 2b Roadmap (Current - .keras Metadata Integration)

**High Priority - TransformerAgent Team:**
1. **Implement backtest_metrics.py** - Create ExpectedPerformance dataclass and PerformanceMetricsCalculator
2. **Embed metadata in .keras** - Call embed_performance_metadata() after training completes
3. **Validate metadata** - Use validation checklist to verify all 9 required fields present

**High Priority - MTS Team:**
4. **Load ExpectedPerformance from .keras metadata** - Remove hardcoded defaults from TradeServer
5. **Test with embedded metadata** - Verify health calculations use model-specific benchmarks

### Phase 3 Roadmap (Future Enhancements)

**High Priority:**
1. Create health status history chart (time series showing status changes over time)

**Medium Priority:**
4. Add health status to live agent view
5. Implement alerting when status changes to WARNING/SOFT_LOCKED
6. Add health metrics to equity curve visualization

**Low Priority:**
7. Export health history to CSV for analysis
8. Add A/B testing support (compare multiple model versions)
9. Implement regime-aware health thresholds

---

**END OF SPECIFICATION**

**Next Steps:**
1. ✅ Review this specification  
2. ✅ Resolve open questions (Section 11)
3. ✅ Implement configuration dataclasses (config.py - ExpectedPerformance, HealthThresholds)
4. ✅ **Phase 1 Complete:** Core Models & Calculator (`src/model_health.py`)
5. ✅ **Phase 1 Complete:** TradeServer Integration (CloseTrade response)
6. ✅ **Phase 2 Complete:** GUI Dashboard Integration (Model Health card with Field components)
7. **→ Phase 2b Current:** TransformerAgent implements .keras metadata embedding
8. **→ Phase 2b Next:** MTS loads ExpectedPerformance from .keras metadata

