# MindfulTrader Unified Messaging Manifest (MT-UMM)

**Project**: Elite Event-Driven Transformer (LBRNet)

**Version**: 2.2 (Institutional-Grade Specification with HMM Integration)

**Date**: January 18, 2026 (v2.2 SSOT - HMM Architecture Finalized)

**Status**: ✅ **ARCHITECTURE LOCKED / IMPLEMENTATION IN PROGRESS** - Single Source of Truth for Elite v2.2 Protocol

---

## Table of Contents

1. [Architectural Philosophy](#1-architectural-philosophy)
2. [Protocol Comparison Matrix](#2-protocol-comparison-matrix)
3. [Common Core Schema (46 Fields)](#3-common-core-schema-46-fields)
4. [Protocol-Specific Extensions](#4-protocol-specific-extensions)
5. [Data Flow Diagrams](#5-data-flow-diagrams)
6. [Latency Budget Breakdown](#6-latency-budget-breakdown)
7. [Version Control & Migration](#7-version-control--migration)
8. [Example Messages](#8-example-messages)
9. [Critical Implementation Notes](#9-critical-implementation-notes)
10. [Open Questions](#10-open-questions)

---

## 1. Architectural Philosophy

### 1.1 Distributed Master-Slave Architecture

The system operates as a **distributed master-slave architecture** between **Windows (Sierra Chart/C++)** and **WSL2 (Python/PyTorch)** using ZeroMQ for ultra-low latency IPC.

**C++ Master Controller** (Windows/Sierra Chart):
- Context enrichment via HMM regime probabilities (Port 5561 REQ/REP)
- Risk gating using entropy thresholds (C++ internal)
- Feature stream publication (Port 5555 PUB)
- Emergency position flattening on connection loss

**Python Inference Engine** (WSL2):
- Elite Transformer Agent (<200µs inference)
- SystemOrchestrator handshake (Port 5560)
- HMM Server for regime probability calculation (Port 5561 ROUTER async)
- Heartbeat monitoring (Port 5559 PUB)

### 1.2 The Three Laws of MT-UMM

1. **Stationarity (Clarified):**
   - **Model Inputs**: The Transformer *never* receives raw price levels in its training features. All features are delta-encoded or normalized via ATR.
   - **Ground Truth Exception**: The `last` (close price) field exists in messages for CHL labeling and execution logic, but is NOT used as a model input feature.
   - **Rationale**: Allows CHL to compute forward-looking returns while maintaining model stationarity.

2. **Sparsity:**
   - Messages are only generated when the `IndicatorManager` detects a "Significant Change."
   - Typical frequency: 8-15 events per trading day (vs 390 bars).
   - Reduces Transformer's noise-floor and focuses on regime transitions.

3. **Synchronization:**
   - Every message across all protocols inherits from the same 46-field **Common Core**.
   - Protocol-specific extensions are additive only (never modify core fields).
   - Ensures model trained on Protocol A can infer on Protocol B without feature mismatch.

### 1.2.1 Statistical Gating: Mahalanobis Distance Filtering

ContextManager implements a precision-matrix-based statistical gate to prevent unnecessary HMM updates on market noise.

**Architecture**:
1. **10D Observation Vector** is constructed from context + anchors + event velocity
2. **Noise Floor Check**: L1-norm of delta < 0.005 skips update (jitter rejection)
3. **Mahalanobis Distance**: $d^2 = \Delta^T P \Delta$ where P is precision matrix
4. **Trigger Decision**:
   - Always trigger if: Not initialized OR heartbeat timeout (default 60 sec)
   - Otherwise trigger if: $d^2 >$ χ² threshold (default 3.84 for 1 DoF)
5. **Result**: Update sent to Python HMM server via async DEALER socket

**Benefit**: Reduces HMM calls 90%+ while capturing regime transitions
**Implementation**: ContextManager.cpp lines 100-155

### 1.2.2 Position Sizing Integration: HmmState Decoder

PositionManager dynamically scales position size based on HMM regime probabilities.

**HmmState Enum** (derived from regime probabilities):

| HmmState | Regime Meaning | Position Multiplier | Risk Profile |
|----------|----------------|-------------------|---------------|
| KINETIC_FLUSH | High Vol + Bullish trend | 1.2-1.4× | Aggressive |
| INST_SWEEP | High Vol + Bearish trend | 1.2-1.4× | Aggressive |
| STABLE_DRIFT | Low Vol + Bullish trend | 0.8-1.0× | Conservative |
| UNKNOWN | Low Vol + Bearish trend | 0.8-1.0× | Conservative |
| STATIONARY_COIL | Ranging (no trend) | 0.4-0.6× | Minimal |
| UNKNOWN | Initialization/uncertainty | 0.4× | Neutral |

**Implementation**: PositionManager.cpp lines 603-618
**Usage**: Check cached HmmState from IndicatorManager, apply multiplier before order submission

### 1.2.3 Risk Management Integration: HMM-Adjusted Risk Multipliers

RiskManager dynamically adjusts stop-loss distance and position risk based on HMM regime.

**Risk Multiplier Application**:

| HmmState | Base Multiplier | Interpretation | Stop-Loss Distance |
|----------|-----------------|-----------------|--------------------|
| KINETIC_FLUSH/INST_SWEEP | 1.3-1.5× | High confidence, widen stops | ATR × 2.5-3.0 |
| STABLE_DRIFT/UNKNOWN | 0.8-1.0× | Lower volatility, normal stops | ATR × 2.0 |
| STATIONARY_COIL | 0.4-0.6× | Ranging = low confidence, tighten | ATR × 1.2-1.5 |
| UNKNOWN | 0.4× | Initialization phase | Conservative |

**Implementation Pattern** (RiskManager.cpp line 644):
- Base risk from Elder + equity calculation
- Apply HMM multiplier: `baseRisk × hmmInd->RiskMultiplier()`
- Enforce daily limits: `min(hmmAdjusted, maxDailyLoss)`

**Caching Strategy**: RefreshMetrics() updates cache when position opens or bar closes (not every tick)

### 1.3 Threading & Caching Architecture (Elite v2.2)

#### 1.3.1 Main Study Thread

The ACSIL study thread (`scsf_MindfulTrader`) operates as the coordinator:

1. **Per-event**:
   - Update ContextManager (statistical context + anchors)
   - Call CheckAndTriggerHMM() (Mahalanobis gating)
   - If triggered: HMMClient::RequestUpdateAsync() (non-blocking)
2. **Per-bar**: Publish events via ZMQ
3. **Never waits** for HMM response (asynchronous design)

**Key Property**: Study thread never blocks on network I/O or Python computation.

#### 1.3.2 Worker Thread (HMMClient)

Runs continuously in background, manages all HMM communication:

1. **Polling Loop** (500ms timeout):
   - Poll items: [DEALER socket, inproc PAIR signal]
   - When signal: New observation ready → Send to Python
   - When DEALER readable: Response ready → Cache in HMMRegimeIndicator
2. **Async Communication**:
   - DEALER/ROUTER pattern (not REQ/REP)
   - HIGH_WATER_MARK = 1: Only latest observation kept (drop stale requests)
   - DEALER drops intermediate frames if Python slow
3. **Result Caching**:
   - HMMRegimeIndicator: Discrete HmmState enum (6 values: KINETIC_FLUSH, INST_SWEEP, STABLE_DRIFT, UNKNOWN, STATIONARY_COIL, UNKNOWN)
   - HMMEventIndicator: Detailed probability vector (lvb, hvb, rng, hvr, lvr, entropy)

**Benefit**: Study thread never waits; worst case is using stale HMM results (1-2 events behind).

#### 1.3.3 Thread Safety

**Protected Data** (ContextManager - RAII pattern):
```cpp
std::mutex m_mutex;
std::lock_guard<std::mutex> lock(m_mutex);  // Automatic unlock
```

**HMMClient** (Atomic flags + mutexes):
```cpp
std::atomic<bool> m_running;           // Safe bool flag for worker thread
std::array<float, 10> m_latestObservation;  // Protected by m_obsMutex
std::atomic<bool> m_hasNewObservation;      // Signaling flag
```

#### 1.3.4 Latency Impact

**Without HMM**: Study thread processes immediately (~100µs)
**With HMM**:
- Study thread: +0µs (async, fire-and-forget)
- Worker thread: Waits for Python response (may lag 1-2 events)
- PositionManager: Uses cached HMM value (may be stale 30-60 seconds)
- **Total latency impact to study thread**: ~0µs (deferred to worker thread)

**Staleness Acceptable Because**: HMM captures regime (slow-moving); 1-2 event lag << regime duration (typically 60+ events)

---

## 2. Protocol Comparison Matrix

| Field Category | Key Examples | Live (5555) | Training (Disk) | Backtest (5559) |
| --- | --- | --- | --- | --- |
| **Common Core** (46) | `efficiency`, `velocity`, `rel_range` | ✅ | ✅ | ✅ |
| **Indicators** (29) | `long_macd`, `raschke_setup`, `rsi` | ✅ (Transformer features) | ✅ | ✅ |
| **Ground Truth** (5) | `open`, `high`, `low`, `close`, `volume` | ❌ | ✅ | ❌ |
| **Volatility** (1) | `atr_10` (Raw ATR Value) | ❌ | ✅ (v2.0) | ❌ |
| **Bar Metrics** (3) | `close_percentile`, `volume_ratio_percent`, `bar_index` | ❌ | ✅ | ❌ |
| **Context** (12) | `prev_high`, `prev_low`, distances | ❌ | ✅ | ❌ |
| **Diagnostics** (4) | `latency_us`, `sequence_number` | ✅ | ❌ | ✅ |
| **HMM Regime** (6) | `regime_prob_*`, `regime_entropy` | ✅ (v2.2) | ✅ (v2.2 reprocess) | ❌ |
| **Results** (12) | `pnl_ticks`, `equity_curve`, `metrics` | ❌ | ❌ | ✅ |
| **Total Packet** | | **52** (46+6 HMM) | **~76** (70+6 HMM) | **58** |
| **Transformer Input** | | **35** (29+6 HMM) | **35** (29+6 HMM) | **35** |

**CRITICAL NOTE on Field Counts (v2.2)**:
- **Full Packet**: 46 Common Core + 6 HMM = **52 fields** (C++ `GetPayload()` size)
- **Live Transformer Input**: 29 indicators + 6 HMM = **35 features** (Python feature slice)
- **C++ Implementation**: Always pack full 52 fields; Python handles feature selection

**CRITICAL NOTE on OHLC (Updated January 13, 2026)**:
- ✅ **Training Protocol NOW includes full OHLC** in `EventDataCollectorStudy.cpp` (v2.0)
- Fields added: `open`, `high`, `low`, `close`, `volume` (lines 177-181)
- ❌ Live and Backtest protocols still send only `last` (stationarity requirement)

**HMM ARCHITECTURE (SSOT January 18, 2026 - Elite Async)**:
- ✅ **Live Protocol adds 6 HMM probability fields** (regime_prob_lvb/hvb/rng/hvr/lvr + regime_entropy)
- ✅ **Training Protocol requires reprocess** of 5.9M events to add HMM labels
- **Port 5561**: HMM Server (DEALER/ROUTER async) - C++ sends 10D observation, receives 6D probabilities
- **Port 5562**: Mental Profile (moved from 5561)
- **C++ Pattern**: Non-blocking async (worker thread polls DEALER, study thread never waits)
- **Python Pattern**: ROUTER socket processes requests with client identity routing
- **Graceful Lag**: Values lag by ~1 event (30-60 min), initial state = 0.166 uniform uncertainty
- **Threading**: Worker writes to `HMMRegimeIndicator` with mutex, study thread reads cached values
- Python no longer needs asof-join for CHL labeling - complete data in message
- **Open Question Q1**: RESOLVED - OHLC is now in Training Protocol

---

## 3. Common Core Schema (45 Fields)

**All protocols share these 45 fields in every message:**

### 3.1 Price & Temporal (2 fields)

| Field | Type | Range | Purpose |
|-------|------|-------|---------|
| `last` | Float64 | ES price (e.g., 4000.00-5000.00) | Close price of current bar (for CHL, not model input) |
| `timestamp` | Int64 | Unix nanoseconds | Event timestamp for temporal ordering |

### 3.1.1 Event Velocity (Calculated in Real-Time)

| Field | Type | Calculation | Purpose |
|-------|------|-------------|---------|
| `event_velocity` | Float | count(timestamps in 60s window) / 60 | Market activity density |
| `log_event_velocity` | Float | log1p(event_velocity) | Normalized for Mahalanobis filtering |
| `event_velocity_raw` | Int | Count of events in 60-min window | Debugging/diagnostics |

**Range**:
- Typical quiet: 0.05-0.2 events/sec (3-12 events/min)
- Normal: 0.2-0.5 events/sec
- Hot market: 0.5-2.0 events/sec

**Implementation**: ContextManager::CheckAndTriggerHMM() lines 75-83
**Threading**: Managed via deque in locked section (m_eventTimestampsUS with std::lock_guard)
**Purpose**: Used in Mahalanobis distance calculation for regime change detection

### 3.2 HMM Statistical Context (4 fields)

| Field | Type | Range | Purpose |
|-------|------|-------|---------|
| `efficiency` | Float64 | -1.0 to 1.0 | Directional efficiency: abs(close-open) / (high-low) |
| `velocity` | Float64 | -10.0 to 10.0 | Momentum: (close-open) / ATR |
| `volatility` | Float64 | 0.0 to 5.0 | Normalized volatility: (high-low) / ATR |
| `rel_range` | Float64 | 0.0 to 3.0 | Relative range: current_range / ATR(10) |

### 3.3 Elder Triple Screen - Screen 1: Long-term Trend (5 fields)

| Field | Enum Type | Values | Purpose |
|-------|-----------|--------|---------|
| `long_macd` | MACDEnum | UNDEFINED(0), BULLISH_CROSS(1), BEARISH_CROSS(2), etc. | Daily MACD state |
| `long_FI13_signal` | ForceIndexEnum | UNDEFINED(0), BEARISH(1), NEUTRAL(2), BULLISH(3) | Force Index direction |
| `long_macd_divergence` | DivergenceEnum | UNDEFINED(0), NONE(1), BULLISH(2), BEARISH(3) | Price/MACD divergence |
| `long_imp` | ImpulseEnum | UNDEFINED(0), RED(1), BLUE(2), GREEN(3) | Elder Impulse System |

### 3.4 Elder Triple Screen - Screen 2: Intermediate Oscillators (9 fields)

| Field | Enum Type | Values | Purpose |
|-------|-----------|--------|---------|
| `interm_stochastic` | StochasticEnum | UNDEFINED(0), OVER_SOLD(1), NORMAL(2), OVER_BOUGHT(3) | Stochastic state |
| `raschke_strategy_setup` | RaschkeStrategyEnum | UNDEFINED(0), NONE(1), HOLY_GRAIL_CONT(2), ... (12 patterns) | Linda Raschke setup |
| `raschke_tactical_trigger` | RaschkeTriggerEnum | UNDEFINED(0), NONE(1), ... (8 triggers) | Raschke entry trigger |
| `rsi` | RSIEnum | UNDEFINED(0), OVERSOLD(1), NORMAL(2), OVERBOUGHT(3) | RSI state |
| `interm_FI2_signal` | ForceIndexEnum | Values same as long_FI13_signal | Intermediate Force Index |
| `ema_proximity` | ProximityEnum | UNDEFINED(0), BELOW(1), TOUCHING(2), ABOVE(3) | Price vs EMA13 |
| `interm_macd_divergence` | DivergenceEnum | Values same as long_macd_divergence | Intermediate divergence |
| `interm_imp` | ImpulseEnum | Values same as long_imp | Intermediate impulse |

### 3.5 Elder Triple Screen - Screen 3: Execution Patterns (6 fields)

| Field | Enum Type | Values | Purpose |
|-------|-----------|--------|---------|
| `structure_test` | StructureTestEnum | UNDEFINED(0), NONE(1), SUPPORT_TEST(2), RESISTANCE_TEST(3) | EMA13 test |
| `volume_signal` | VolumeEnum | UNDEFINED(0), LOW(1), NORMAL(2), HIGH(3) | Volume confirmation |
| `atr_proximity` | ProximityEnum | Values same as ema_proximity | Price vs ATR bands |
| `daily_bias` | DailyBiasEnum | UNDEFINED(0), BEARISH(1), NEUTRAL(2), BULLISH(3) | Daily trend bias |
| `short_mkt_action` | MarketActionEnum | Values same as Screen 3 market-action enum states | Short-term action |
| `kangaroo_tail` | KangarooTailEnum | UNDEFINED(0), NONE(1), BULLISH(2), BEARISH(3) | Price rejection |

### 3.6 Elder Pattern Recognition (5 fields)

| Field | Enum Type | Values | Purpose |
|-------|-----------|--------|---------|
| `turtle_soup` | TurtleSoupEnum | UNDEFINED(0), NONE(1), BULLISH(2), BEARISH(3) | Failed breakout |
| `momentum_pinball` | MomentumPinballEnum | UNDEFINED(0), NONE(1), LONG_SETUP(2), SHORT_SETUP(3) | Oscillator extreme |
| `elder_breakout` | ElderBreakoutEnum | UNDEFINED(0), NONE(1), BULLISH(2), BEARISH(3) | Impulse breakout |
| `nr7` | NR7Enum | UNDEFINED(0), NONE(1), ACTIVE(2) | Narrow range 7 |
| `holding_strategy` | HoldingStrategyEnum | UNDEFINED(0), STAND_ASIDE(1), ... (8 strategies) | Position guidance |

### 3.7 State Management (5 fields)

| Field | Enum Type | Values | Purpose |
|-------|-----------|--------|---------|
| `side` | SideEnum | UNDEFINED(0), FLAT(1), LONG(2), SHORT(3) | Current position |
| `time_of_day` | TimeOfDayEnum | ASIAN(1), SWEET_SPOT(2), LUNCH_DEAD_ZONE(3), AFTERNOON(4) | Time window |
| `overnight_exit` | BoolEnum | FALSE(0), TRUE(1) | Exit before close |
| `market_regime` | RegimeEnum | UNDEFINED(0), RANGING(1), TRENDING(2), VOLATILE(3) | Market state |
| `nh_nl_signal` | NHNLEnum | UNDEFINED(0), NONE(1), NEW_HIGH(2), NEW_LOW(3) | Breakout signal |

### 3.8 Correlation & Macro (5 fields)

| Field | Enum Type | Values | Purpose |
|-------|-----------|--------|---------|
| `oscillator_310` | OscillatorEnum | UNDEFINED(0), BELOW(1), NEUTRAL(2), ABOVE(3) | 3/10 oscillator |
| `corr_es_zn` | CorrelationEnum | UNDEFINED(0), NEGATIVE(1), NEUTRAL(2), POSITIVE(3) | ES/ZN correlation |
| `corr_es_dx` | CorrelationEnum | Values same | ES/DX correlation |
| `zn_trend` | TrendEnum | UNDEFINED(0), DOWN(1), SIDEWAYS(2), UP(3) | ZN trend |
| `dx_trend` | TrendEnum | Values same | DX trend |

### 3.9 Event Metadata (4 fields)

| Field | Type | Values | Purpose |
|-------|------|--------|---------|
| `type` | String | "indicator_change", "time_based", etc. | Event trigger type |
| `changed_keys` | List[String] | ["rsi", "macd", ...] | Which indicators changed |
| `is_event_driven` | Boolean | true/false | Event vs bar-based |
| `event_type_id` | Int32 | 1=indicator_change, 2=time_based, etc. | Categorical ID |

**Total Common Core: 46 fields** (3 + 4 + 5 + 9 + 6 + 5 + 5 + 5 + 4)

**Note**: Live Trading Protocol adds 6 HMM probability fields (regime_prob_lvb/hvb/rng/hvr/lvr + regime_entropy) for Transformer attention weighting, bringing total to 35 fields for live inference.

---

## 4. Protocol-Specific Extensions

### 4.1 Training Protocol Extensions (+24 fields = ~70 total)

**Purpose**: Historical data collection for supervised learning with ground truth labels.

**Output Format**: JSONL (newline-delimited JSON), one file per trading day.

**Implementation**: `src/EventDataCollectorStudy.cpp` (✅ Complete as of v2.1)

**Additional Fields**:

| Field | Type | Range/Values | Purpose | Status |
|-------|------|--------------|---------|--------|
| `bar_index` | Int32 | 0-390 | Index within day for temporal ordering | ✅ |
| **`open`** | Float64 | ES price | Bar open for CHL calculation | ✅ v2.0.1 |
| **`high`** | Float64 | ES price | Bar high for CHL calculation | ✅ v2.0.1 |
| **`low`** | Float64 | ES price | Bar low for CHL calculation | ✅ v2.0.1 |
| **`close`** | Float64 | ES price | Bar close (same as `last`) | ✅ v2.0.1 |
| **`volume`** | Int64 | Contract count | Bar volume | ✅ v2.0.1 |
| **`atr_10`** | Float64 | 2.0-20.0 ES points | Raw ATR(10) for volatility-adjusted labels | ✅ v2.0 |
| **`regime_prob_lvb`** | Float32 | 0.0-1.0 | HMM: Low Volatility Bullish probability | ⚠️ v2.2 (reprocess) |
| **`regime_prob_hvb`** | Float32 | 0.0-1.0 | HMM: High Volatility Bullish probability | ⚠️ v2.2 (reprocess) |
| **`regime_prob_rng`** | Float32 | 0.0-1.0 | HMM: Ranging probability | ⚠️ v2.2 (reprocess) |
| **`regime_prob_hvr`** | Float32 | 0.0-1.0 | HMM: High Volatility Bearish probability | ⚠️ v2.2 (reprocess) |
| **`regime_prob_lvr`** | Float32 | 0.0-1.0 | HMM: Low Volatility Bearish probability | ⚠️ v2.2 (reprocess) |
| **`regime_entropy`** | Float32 | 0.0-2.5 | HMM: Shannon entropy (uncertainty) | ⚠️ v2.2 (reprocess) |
| `close_percentile` | Float64 | 0.0-100.0 | (close-low)/(high-low)*100 for bar structure | ✅ |
| `volume_ratio_percent` | Float64 | 0.0-500.0 | Volume vs 20-bar average | ✅ |
| `prev_high` | Float64 | ES price | Previous bar high (reference level) | ✅ |
| `prev_low` | Float64 | ES price | Previous bar low (reference level) | ✅ |
| `prev_close` | Float64 | ES price | Previous bar close | ✅ |
| `dist_to_prev_high` | Float64 | -50.0 to 50.0 | (last - prev_high) for proximity | ✅ |
| `dist_to_prev_low` | Float64 | -50.0 to 50.0 | (last - prev_low) for proximity | ✅ |
| `dist_to_pdh` | Float64 | -100.0 to 100.0 | Distance to previous day high | ✅ |
| `dist_to_pdl` | Float64 | -100.0 to 100.0 | Distance to previous day low | ✅ |
| `session_high` | Float64 | ES price | Current session high | ✅ |
| `session_low` | Float64 | ES price | Current session low | ✅ |
| `position_side` | Int32 | 0=flat, 1=long, 2=short | Position state at event time | ✅ |
| `regime_tenure` | Int32 | 1-100 | Bars since regime change | ✅ |
| `event_enrichment` | JSON | {...} | Additional metadata for research | ✅ |

**Python Consumption** (Updated for v2.0.1 - No Join Required):
```python
# scripts/collect_event_driven_data.py
import polars as pl

# Load C++ events - NOW includes OHLCV directly!
df_events = pl.read_ndjson("data/raw/event_data_2026-01-13.jsonl")

# Verify OHLCV fields present (v2.0.1+)
assert all(col in df_events.columns for col in ['open', 'high', 'low', 'close', 'volume', 'atr_10']), \
    "Missing OHLCV/ATR fields - check EventDataCollectorStudy version"

# Apply CHL labeling directly (no join needed)
from lbrnet.features.causal_horizon_labeler import CausalHorizonLabeler
chl = CausalHorizonLabeler(
    stop_loss_atr_mult=2.0,   # Use ATR-based adaptive stops
    take_profit_atr_mult=3.0  # ATR available in v2.0+
)
df_labeled = chl.apply_labels(df_events)

# Result: ~1,000 sequences with 7-class labels (ENTER_LONG, ENTER_SHORT, STAND_ASIDE, etc.)
```

### 4.2 Live Trading Protocol Extensions (+4 base +6 HMM = 35 total)

**Purpose**: Real-time inference in production with sub-millisecond latency.

**Transport**: ZMQ PUB/SUB on port 5555 (managed by Elite SystemOrchestrator).

**HMM Integration**: DEALER/ROUTER async on port 5561 (C++ worker thread polls for regime probabilities from Python HMM server, non-blocking)

**Elite Protocol Integration** (v1.0.2+):
- Messages routed through centralized I/O loop in `system_orchestrator.py`
- Master/Slave handshake via port 5560 (REGISTER → CONFIG_ACK → READY)
- State machine: UNINITIALIZED → NEGOTIATING → REGISTERED → INITIALIZING → READY → ACTIVE_TRADING
- Bidirectional liveness monitoring with C++ Master Controller
- Automatic socket recovery using Lazy Pirate pattern

**Additional Fields (Base Protocol)**:

| Field | Type | Purpose |
|-------|------|---------|
| `trade_signal_id` | String (UUID) | Unique identifier for tracking signal → execution |
| `latency_us` | Int32 | C++ detection to ZMQ publish latency (microseconds) |
| `sequence_number` | Int64 | Monotonic counter for detecting dropped packets |
| `is_live` | Boolean | true in production, false in simulation |

**Additional Fields (HMM Regime Context - SSOT Jan 18, 2026)**:

| Field | Type | Range | Purpose |
|-------|------|-------|---------|
| `regime_prob_lvb` | Float32 | 0.0-1.0 | Low Volatility Bullish probability |
| `regime_prob_hvb` | Float32 | 0.0-1.0 | High Volatility Bullish probability |
| `regime_prob_rng` | Float32 | 0.0-1.0 | Ranging probability |
| `regime_prob_hvr` | Float32 | 0.0-1.0 | High Volatility Bearish probability |
| `regime_prob_lvr` | Float32 | 0.0-1.0 | Low Volatility Bearish probability |
| `regime_entropy` | Float32 | 0.0-2.5 | Shannon entropy (uncertainty measure) |

**HMM Usage (Both/And Architecture)**:
- **C++**: Uses `regime_entropy` for gating (position sizing, pattern quality)
- **Transformer**: Uses all 6 probability fields for attention weighting
- **Total Live Protocol**: 29 base + 6 HMM = **35 fields**

#### 4.2.1 HMM Server Architecture (Updated Jan 30, 2026)

**Port Assignment**:
- **5561**: HMM Server (Python ROUTER socket, receives from C++ DEALER)
- **5562**: Mental Profile (Python PUB socket)

**Communication Pattern**: DEALER/ROUTER async (C++ worker thread, Python server)
- Asynchronous DEALER/ROUTER with worker thread separates concerns
- Study thread produces observations, Worker thread consumes results
- HIGH_WATER_MARK=1: Only latest market state processed (intermediate observations dropped)
- Non-blocking: Study thread never waits for Python response

**Request Timing**: Event-driven (only when indicators change)
- Frequency: 8-15 requests/day (matches C++ event rate)
- No fixed schedule (60-min bars not required)
- Stateless server (each request independent)

**10D Observation Vector** (C++ → Python):
```json
{
  "volatility": 0.85,
  "efficiency": 0.73,
  "rel_range": 0.92,
  "velocity": 1.24,
  "dist_day_high": -12.5,
  "dist_day_low": 22.3,
  "dist_four_bar_high": -8.2,
  "dist_four_bar_low": 15.7,
  "dist_ema_13": 3.5,
  "log_event_velocity": 0.42
}
```

**6D Probability Response** (Python → C++):
```json
{
  "regime_prob_lvb": 0.05,
  "regime_prob_hvb": 0.23,
  "regime_prob_rng": 0.42,
  "regime_prob_hvr": 0.18,
  "regime_prob_lvr": 0.12,
  "regime_entropy": 1.83
}
```

**Error Handling**: Uniform uncertainty fallback
- On timeout/error: Set all probs to 0.2, entropy to 1.0
- Transformer sees "no context" signal
- C++ risk manager reduces position automatically
- Trading continues (doesn't halt on HMM failure)

**log_event_velocity Calculation**:
```cpp
// In IndicatorManager.cpp
std::deque<double> m_eventTimestamps;  // Rolling 60-min window

float CalculateLogEventVelocity(SCDateTime currentTime) {
    // Remove timestamps older than 60 minutes
    double currentTimeSeconds = currentTime.GetAsDouble() * 86400.0;
    double cutoffTime = currentTimeSeconds - 3600.0;  // 60 min ago

    while (!m_eventTimestamps.empty() && m_eventTimestamps.front() < cutoffTime) {
        m_eventTimestamps.pop_front();
    }

    // Add current event
    m_eventTimestamps.push_back(currentTimeSeconds);

    // Calculate events per hour
    int eventCount = m_eventTimestamps.size();
    float eventsPerHour = static_cast<float>(eventCount);

    // Return log(events + 1) for numerical stability
    return std::log(eventsPerHour + 1.0f);
}
```

**Synchronization Validation**:
- Python training pipeline is **gold standard**
- C++ must match Python's 10D calculations to 6 decimal places
- Use `Validation.json` with 100 OHLCV test cases
- Block deployment if C++/Python vectors diverge

**Python Consumption** (Elite Protocol Pattern):
| `latency_us` | Int32 | C++ detection to ZMQ publish latency (microseconds) |
| `sequence_number` | Int64 | Monotonic counter for detecting dropped packets |
| `is_live` | Boolean | true in production, false in simulation |

**Python Consumption** (Elite Protocol Pattern):
```python
# app.py - Live inference with Elite SystemOrchestrator
from system_orchestrator import get_orchestrator, SystemState
from src.live_agent import LiveAgent
from queue import Queue

# Elite: Initialize orchestrator with state management
orchestrator = get_orchestrator(host_ip="172.20.112.1")  # WSL2 Windows host

# Elite: Register state change callback
def on_state_change(new_state: SystemState):
    if new_state == SystemState.ACTIVE_TRADING:
        logger.info("✅ System ready for live trading")
    elif new_state == SystemState.DISCONNECTED:
        logger.error("❌ Connection lost - halting inference")

orchestrator.state_change_callback = on_state_change

# Elite: Connect and perform Master/Slave handshake
if orchestrator.connect():
    logger.info("Elite handshake complete - ports assigned")
else:
    logger.error("Failed to register with C++ Master Controller")
    sys.exit(1)

# Elite: Register indicator update handler
message_queue = Queue()
zmq_client = ZmqClient(message_queue, orchestrator)
orchestrator.feature_factory = zmq_client

# Elite: Start unified I/O loop (handles all ports)
orchestrator.start_io_loop()

# Initialize LiveAgent with orchestrator integration
agent = LiveAgent(orchestrator=orchestrator)

# Process indicator updates from centralized queue
while orchestrator.state in [SystemState.READY, SystemState.ACTIVE_TRADING]:
    try:
        topic, msg = message_queue.get(timeout=1.0)

        # Elite: Automatic sequence validation in ZmqClient._check_sequence_id()
        # Elite: Latency tracking via telemetry.network_jitter_ms

        if topic == "INDICATOR_UPDATE":
            # Header validation (Elite Protocol)
            if 'header' in msg:
                header = msg['header']
                # Check sequence (handled automatically by ZmqClient)
                # Check latency
                if 'latency_us' in header and header['latency_us'] > 500:
                    logger.warning(f"⚠️ Latency budget exceeded: {header['latency_us']}µs")
                    orchestrator.telemetry.update_network_jitter(header['latency_us'] / 1000.0)

            # Infer with payload (46-field Common Core)
            payload = msg.get('payload', {})
            action = agent.infer(payload)

            # Elite: Track state for C++ mirroring
            if action in ['ENTER_LONG', 'ENTER_SHORT']:
                orchestrator.transition_state(SystemState.ACTIVE_TRADING)

    except Empty:
        continue
    except Exception as e:
        logger.exception(f"Error processing message: {e}")
        if orchestrator.config.max_retries > 0:
            continue  # Elite: Graceful degradation
        else:
            break

# Elite: Clean shutdown
orchestrator.shutdown()
logger.info("Orchestrator shutdown complete")
```

### 4.2.1 HMM Server Architecture (SSOT Jan 18, 2026)

**Port Assignment**:
- **5561**: HMM Server (Python REP socket)
- **5562**: Mental Profile (moved from 5561, Python PUB socket)

**Communication Pattern**: REQ/REP (C++ client, Python server)

**Request Timing**: Event-driven (only when indicators change)
- Frequency: 8-15 requests/day (matches C++ event rate)
- No fixed schedule (60-min bars not required)
- Stateless server (each request independent)

**10D Observation Vector** (C++ → Python):
```json
{
  "volatility": 0.85,
  "efficiency": 0.73,
  "rel_range": 0.92,
  "velocity": 1.24,
  "dist_day_high": -12.5,
  "dist_day_low": 22.3,
  "dist_four_bar_high": -8.2,
  "dist_four_bar_low": 15.7,
  "dist_ema_13": 3.5,
  "log_event_velocity": 0.42
}
```

**6D Probability Response** (Python → C++):
```json
{
  "regime_prob_lvb": 0.05,
  "regime_prob_hvb": 0.23,
  "regime_prob_rng": 0.42,
  "regime_prob_hvr": 0.18,
  "regime_prob_lvr": 0.12,
  "regime_entropy": 1.83
}
```

**Error Handling**: Uniform uncertainty fallback
- On timeout/error: Set all probs to 0.2, entropy to 1.0
- Transformer sees "no context" signal
- C++ risk manager reduces position automatically
- Trading continues (doesn't halt on HMM failure)

**log_event_velocity Calculation**:
```cpp
// In IndicatorManager.cpp
std::deque<double> m_eventTimestamps;  // Rolling 60-min window

float CalculateLogEventVelocity(SCDateTime currentTime) {
    // Remove timestamps older than 60 minutes
    double currentTimeSeconds = currentTime.GetAsDouble() * 86400.0;
    double cutoffTime = currentTimeSeconds - 3600.0;  // 60 min ago

    while (!m_eventTimestamps.empty() && m_eventTimestamps.front() < cutoffTime) {
        m_eventTimestamps.pop_front();
    }

    // Add current event
    m_eventTimestamps.push_back(currentTimeSeconds);

    // Calculate events per hour
    int eventCount = m_eventTimestamps.size();
    float eventsPerHour = static_cast<float>(eventCount);

    // Return log(events + 1) for numerical stability
    return std::log(eventsPerHour + 1.0f);
}
```

**Synchronization Validation**:
- Python training pipeline is **gold standard**
- C++ must match Python's 10D calculations to 6 decimal places
- Use `Validation.json` with 100 OHLCV test cases
- Block deployment if C++/Python vectors diverge

---

### 4.2.2 HMM Implementation Details (SSOT v2.2 Clarifications)

**Context**: These expert-level clarifications resolve friction points for high-frequency integration on mid-range Windows 10 hardware.

#### 1. Async DEALER/ROUTER Architecture

**Decision: Elite ZMQ Pattern (Non-Blocking Async)**

- **Pattern**: DEALER/ROUTER instead of blocking REQ/REP
- **Study Thread**: Never blocks - fires observation via inproc signal, continues immediately
- **Worker Thread**: Manages DEALER socket with `zmq_poll()`, updates `HMMRegimeIndicator` atomically
- **Rationale**:
  - **UI Responsiveness**: Study thread never waits - Sierra Chart stays responsive
  - **Flexibility**: HMM can take 10ms, 100ms - doesn't matter, worker thread handles it
  - **Graceful Lag**: Values lag by ~1 event (30-60 min), acceptable for regime context
  - **Auto-Reconnect**: DEALER socket reconnects automatically if Python crashes

```cpp
// C++ HMMClient Worker Thread (Elite Pattern):
void HMMClient::WorkerThread() {
    void* dealer = zmq_socket(context, ZMQ_DEALER);
    zmq_connect(dealer, "tcp://172.20.112.1:5561");

    int hwm = 1;  // Drop oldest if Python slow
    zmq_setsockopt(dealer, ZMQ_SNDHWM, &hwm, sizeof(hwm));

    while (running) {
        zmq_pollitem_t items[] = {
            { dealer, 0, ZMQ_POLLIN, 0 },      // Python replies
            { inproc_signal, 0, ZMQ_POLLIN, 0 } // Study thread requests
        };

        zmq_poll(items, 2, -1);

        // New observation from study thread?
        if (items[1].revents & ZMQ_POLLIN) {
            ObservationVector obs = GetLatestObservation();
            zmq_send(dealer, obs.serialize(), ZMQ_DONTWAIT);
        }

        // Reply from Python HMM?
        if (items[0].revents & ZMQ_POLLIN) {
            zmq_msg_t reply;
            zmq_msg_recv(dealer, &reply, ZMQ_DONTWAIT);

            // Atomically update indicator (thread-safe)
            HMMRegimeIndicator::Instance().SetProbabilities(ParseResponse(reply));
        }
    }
}
```

#### 2. Thread-Safe Indicator Updates

**Decision: Lock-Based Synchronization**

- **Pattern**: Worker thread writes, study thread reads - need mutex protection
- **HMMRegimeIndicator**: Thread-safe with `std::mutex` for atomic 6-float updates
- **Queue Depth**: Set ZMQ `SNDHWM` (Send High Water Mark) to **1** (drop oldest if Python slow)
- **Optimization**: Only send new observation if values changed beyond threshold (0.001)

```cpp
// HMMRegimeIndicator (Thread-Safe Cache):
class HMMRegimeIndicator : public Indicator<HMMProbabilities> {
private:
    mutable std::mutex m_mutex;
    HMMProbabilities m_value;

public:
    void SetProbabilities(float lvb, float hvb, float rng,
                         float hvr, float lvr, float entropy) {
        std::lock_guard<std::mutex> lock(m_mutex);
        m_value = {lvb, hvb, rng, hvr, lvr, entropy};
        m_isDirty = true;
    }

    HMMProbabilities GetValue() const override {
        std::lock_guard<std::mutex> lock(m_mutex);
        return m_value;  // Copy under lock
    }
};
```

#### 3. Field Count Clarification (46 vs. 35 vs. 29)

**Decision: 46 Common Core is the Data Standard; 35 is the Live Payload; 29 is the Feature Subset**

- **Full Packet (52 fields)**: The **46 Common Core Fields** (MTS_UMM Section 3) represent the total packet structure (including metadata like `timestamp_us`, `sequence_number`, and `last`).
- **Live Payload (35 fields)**: **29 stationary indicators** + **6 HMM probability slots** = **35 features** sent to Transformer.
- **The 29**: This refers specifically to the **stationary signal indicators** used as inputs for the Transformer's attention heads.
- **C++ Implementation**: `IndicatorManager::GetPayload()` packs **46 core fields** + **6 HMM probabilities** = **52 total fields** in the JSON object. The "35-field live protocol" refers to the Transformer feature subset (29 indicators + 6 HMM). Python handles the slicing. Do not reduce the count in C++; data density is better than managing multiple schemas.

**Producer/Consumer Pattern**:
- **C++ (Producer)**: "Fire-and-forget" - detect change, query HMM (3ms cap), broadcast via Port 5555
- **Python (Consumer)**: Buffering via `multiprocessing.Queue` or `collections.deque` to handle bursts

#### 4. Entropy Threshold Gating

**Decision: Soft Flag (The "Informed Agent" Approach)**

- **Action**: Proceed with a **Soft Flag**. Publish the event to Python regardless of entropy, but include the `regime_entropy` value and an `hmm_uncertain` boolean.
- **Rationale**: The Transformer is trained on "noisy" data. It may still find a high-probability setup even in a high-entropy regime. Let the model decide if the confidence outweighs the regime noise. C++ should only "Hard Block" if the HMM Server is physically **offline** (Connection Refused).

```cpp
// Add to payload:
payload["regime_entropy"] = entropy;
payload["hmm_uncertain"] = (entropy > 0.9);  // Soft flag for Python
```

#### 5. Sequence Number Scope

**Decision: Per-Session Monotonic (Reset on Handshake)**

- **Action**: The `sequence_number` should start at `1` upon a successful `REGISTER/CONFIG_ACK` handshake and increment per message.
- **Persistence**: Do not worry about persistent storage across PC restarts.
- **Rationale**: The Python `SystemOrchestrator` uses the handshake to reset its "Expectation Engine." If the sequence resets to 1 after a crash/reconnect, Python accepts it as a fresh stream. This simplifies C++ logic significantly on slower hardware.

```cpp
// On handshake complete:
m_sequenceNumber = 1;

// On each message:
payload["sequence_number"] = m_sequenceNumber++;
```

#### 6. Training Data Reprocess Scope

**Decision: Chronological Full Reprocess (The "Gold Standard" Requirement)**

- **Action**: Full reprocess of all 5.9M events (2024–2026).
- **Priority**: Chronological (oldest to newest).
- **Rationale**: The Transformer needs to see how regimes evolve over years to understand seasonality and long-term shifts.
- **Hardware Mitigation**: Since the machine is slow, the Quant team should run the reprocess on a **dedicated Linux instance/server** and provide the finished Parquet/JSONL files to the C++ team. **Do not run the 48-hour reprocess on the Windows 10 trading machine.**

#### 7. Additional Implementation Requirements

**HMM Health Check** (During Port 5560 Handshake):
- C++ should attempt one "Ping" to Port 5561 during initialization.
- If it fails, the system starts in **DEGRADED MODE** immediately.
- Degraded mode: Populate 6 probabilities with `0.166`, set `market_regime` to last known discrete value.

**Degraded Mode Definition**:
- If HMM is offline, C++ populates the 6 probabilities with `0.166` and sets `market_regime` to the last known discrete value from the legacy C++ logic.
- This allows "v2.1 fallback" without breaking the v2.2 message schema.

**HMM Cache Strategy**:
- C++ should **not** cache HMM state.
- The HMM Server (Python side) maintains the hidden state.
- C++ simply provides the "Observation" (the 10D vector).

**C++ Implementation Summary (Elite Async Pattern)**:

```cpp
// Elite Protocol v2.2: Non-Blocking Async HMM Integration
// Study thread NEVER blocks - worker thread handles ZMQ

// Study Thread (scsf_MindfulTrader):
if (IndicatorManager::Instance().IsDirty()) {

    // 1. Request HMM update (non-blocking, sends to worker thread)
    HMMClient::Instance().RequestUpdateAsync();

    // 2. Get CURRENT cached HMM values (from previous event or initial 0.166)
    auto hmmRegime = IndicatorManager::Instance()
        .GetIndicator<HMMRegimeIndicator>(IndicatorKeys::HMM_REGIME);
    auto probs = hmmRegime->GetValue();  // Thread-safe read

    // 3. Pack 52-field payload (46 core + 6 HMM)
    nlohmann::json payload = IndicatorManager::Instance().GetPayload(sc);
    payload["regime_prob_lvb"] = probs.lvb;
    payload["regime_prob_hvb"] = probs.hvb;
    payload["regime_prob_rng"] = probs.rng;
    payload["regime_prob_hvr"] = probs.hvr;
    payload["regime_prob_lvr"] = probs.lvr;
    payload["regime_entropy"] = probs.entropy;

    // 4. Broadcast to Transformer (Port 5555) - NO BLOCKING
    MindfulSocketZMQ::Instance().PublishEvent(payload);
}
// Study thread continues immediately - worker handles HMM async
```

**Key Requirements**:
- **Socket Type**: `ZMQ_DEALER` (Port 5561, async non-blocking)
- **Threading**: Dedicated worker thread with `zmq_poll()` for DEALER + inproc signal
- **Synchronization**: `std::mutex` in `HMMRegimeIndicator` for thread-safe reads/writes
- **Fallback**: Initial state = 0.166 uniform, then uses cached values from previous events
- **HWM**: Set `SNDHWM = 1` to drop oldest observation if Python slow

**Next Step for C++ Team**: Implement `HMMClient.cpp` using **Uniform Uncertainty Fallback** and **Serialized REQ/REP** logic with 3000µs timeout and SNDHWM=1 configuration.

---

### 4.3 Backtest Protocol Extensions (+12 fields = 58 total)

**Purpose**: Historical replay with simulated execution and performance tracking.

**Transport**: ZMQ PUB/SUB on port 5559 (different from Live).

**Additional Fields**:

| Field | Type | Purpose |
|-------|------|---------|
| `replay_speed` | Float32 | 1.0=realtime, 10.0=10× speed |
| `session_id` | String | Unique backtest run identifier |
| `simulated_fill_price` | Float64 | Execution price (includes slippage model) |
| `simulated_fill_time_ns` | Int64 | Timestamp of simulated fill |
| `position_size` | Int32 | Number of contracts (1-10) |
| `pnl_ticks` | Int32 | Running P&L in ticks (1 tick = $12.50 ES) |
| `pnl_dollars` | Float64 | Running P&L in USD |
| `max_drawdown_ticks` | Int32 | Peak-to-trough drawdown |
| `trades_today` | Int32 | Number of trades executed today |
| `win_rate` | Float32 | Percentage of winning trades |
| `avg_r_multiple` | Float32 | Average R-multiple per trade |
| `equity_curve` | List[Float] | Rolling equity (for visualization) |

**Python Consumption**:
```python
# src/backtest.py - Replay with performance tracking
for event in replay_events:
    action = agent.infer(event)

    if action in ['ENTER_LONG', 'ENTER_SHORT']:
        # Track performance
        metrics = {
            'pnl_ticks': event['pnl_ticks'],
            'drawdown': event['max_drawdown_ticks'],
            'win_rate': event['win_rate']
        }
        logger.info(f"Trade #{event['trades_today']}: {metrics}")
```

---

## 5. Data Flow Diagrams

### 5.1 Training Protocol Flow (v2.1 - Institutional-Grade)

```
┌─────────────────────────────────────────────────────────────────────┐
│ C++ (Sierra Chart)                                                  │
│                                                                     │
│ EventDataCollectorStudy (v2.1):                                    │
│   1. Bar completes (15-min)                                        │
│   2. IndicatorManager::DetectChanges()                             │
│   3. If significant change → Pack message (~70 fields)             │
│      ✅ Includes OHLCV (self-describing message)                    │
│      ✅ Includes atr_10 (adaptive stops)                            │
│   4. Atomic write with flush → data/raw/event_data_2026-01-13.jsonl│
│                                                                     │
│ Frequency: 8-15 events/day                                        │
│ Elite Features: Nanosecond timestamps, enum alignment, atomic I/O  │
└─────────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────────┐
│ Python (Data Augmentation) - NO JOIN REQUIRED! ✅                   │
│                                                                     │
│ scripts/collect_event_driven_data.py:                               │
│   1. Load JSONL (10K-2.28M events) - Complete data in message      │
│   2. Verify OHLCV + atr_10 present (v2.1 assertion)               │
│   3. CausalHorizonLabeler (direct application):                    │
│      • Forward-looking returns (next 20 bars)                      │
│      • 2× ATR stop-loss, 3× ATR take-profit (adaptive)            │
│      • Labels: ENTER_LONG, ENTER_SHORT, STAND_ASIDE, TRAP, HOLD   │
│   4. Slice into sequences (50 events each)                         │
│   5. Save → data/training/event_sequences_labeled.parquet          │
│                                                                     │
│ Output: ~1,000 sequences with 7-class labels                       │
│ Performance: ~3 seconds faster (no join), total data locality      │
└─────────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────────┐
│ Python (Model Training)                                             │
│                                                                     │
│ scripts/run_training.py:                                            │
│   1. Load sequences (train/val split)                              │
│   2. TransformerAgent (event-driven architecture)                  │
│   3. Train with focal loss + class weighting                       │
│   4. Early stopping on val_enter_recall                            │
│   5. Save → best_agent_model.keras                                 │
│                                                                     │
│ Target: val_enter_recall >3%, PR-AUC >0.40                        │
└─────────────────────────────────────────────────────────────────────┘
```

### 5.2 Live Trading Protocol Flow (Elite Protocol v1.0.2)

```
┌─────────────────────────────────────────────────────────────────────┐
│ C++ (Sierra Chart) - Master Controller + Real-time Detection       │
│                                                                     │
│ SystemOrchestrator (Port 5560):                                    │
│   - Master/Slave handshake (REP socket)                            │
│   - CONFIG_ACK with port assignments                               │
│   - Bidirectional liveness monitoring                              │
│                                                                     │
│ MindfulSocketZMQ (Port 5555 PUB):                                  │
│   1. Bar tick arrives (<50µs processing)                           │
│   2. IndicatorManager::DetectChanges()                             │
│   3. Pack Elite message with header + payload (50 fields)          │
│   4. ZMQ publish → tcp://*:5555                                    │
│                                                                     │
│ Latency: C++ side <50µs                                           │
└─────────────────────────────────────────────────────────────────────┘
                              ↓ ZMQ (<100µs network)
┌─────────────────────────────────────────────────────────────────────┐
│ Python (WSL2) - Elite SystemOrchestrator + Inference               │
│                                                                     │
│ SystemOrchestrator (Port 5560 REQ):                                │
│   1. REGISTER → CONFIG_ACK handshake                               │
│   2. Setup all sockets (5555, 5556, 5557, 5558, 5559)             │
│   3. READY → ACTIVE_TRADING state transition                       │
│   4. Unified I/O loop (zmq.Poller) manages all ports               │
│                                                                     │
│ ZmqClient (Port 5555 SUB):                                         │
│   1. Receive message via orchestrator's I/O loop                   │
│   2. Auto-validate sequence_id (detect drops)                      │
│   3. Route to message_queue for app.py processing                  │
│                                                                     │
│ app.py (Main Thread):                                               │
│   1. Pop message from queue (non-blocking)                         │
│   2. Extract header (sequence, latency) + payload (46-field core)  │
│   3. LiveAgent.infer() → Transformer prediction (<200µs)           │
│   4. If ENTER signal → Display to user in GUI                      │
│   5. User approves → TradeExecutionClient.send_request_async()     │
│                                                                     │
│ TradeExecutionClient (Port 5558 REQ):                              │
│   - Non-blocking async pattern (20ms polling)                      │
│   - 2.5s absolute timeout                                          │
│   - Lazy Pirate recovery on timeout                                │
│                                                                     │
│ Latency: Python inference <200µs (excluding user approval)        │
│ Total round-trip: <500µs (automated), <5s (manual approval)       │
└─────────────────────────────────────────────────────────────────────┘
                              ↓ ZMQ (<100µs network)
┌─────────────────────────────────────────────────────────────────────┐
│ C++ (Risk Manager) - Execution                                     │
│                                                                     │
│ TradeSocketZMQ (Port 5556 REP):                                    │
│   1. Receive trade validation request                              │
│   2. Risk checks: 2%/6% daily limits, regime compatibility         │
│   3. If validated → Execute via Sierra Chart API                   │
│   4. Send execution confirmation → Python                          │
│                                                                     │
│ Manage stops, take-profits, position sizing                        │
│                                                                     │
│ Latency: Execution <50µs (validation + order submission)          │
└─────────────────────────────────────────────────────────────────────┘
```

### 5.3 Backtest Protocol Flow

```
┌─────────────────────────────────────────────────────────────────────┐
│ C++ (BackTesterStudy) - Historical Replay                          │
│                                                                     │
│ For each historical event:                                          │
│   1. Load from disk (or regenerate from bars)                      │
│   2. Add simulated fills (slippage model)                          │
│   3. Calculate P&L, drawdown, metrics                              │
│   4. ZMQ publish → tcp://*:5559                                    │
│                                                                     │
│ Replay speed: Configurable (1× to 100× realtime)                  │
└─────────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────────┐
│ Python (Backtest Analyzer) - Performance Tracking                  │
│                                                                     │
│ src/backtest.py:                                                    │
│   1. Receive events via ZMQ                                        │
│   2. LiveAgent.infer() (same as live)                              │
│   3. Track performance metrics                                     │
│   4. Generate equity curve, trade list                             │
│                                                                     │
│ Output: Backtest report with Sharpe, max DD, win rate             │
└─────────────────────────────────────────────────────────────────────┘
```

---

## 6. Latency Budget Breakdown

**Target**: <500µs automated path / <5s manual approval path

| Stage | Component | Target Latency | Monitoring |
|-------|-----------|----------------|------------|
| **Detection** | C++ IndicatorManager | <50µs | `timestamp_us` field |
| **HMM Context** | **C++ ↔ HMM Server (Port 5561)** | **<3000µs (3ms)** | **ZMQ REQ Timeout** |
| **Serialization** | C++ → JSON → ZMQ | <30µs | Protobuf packing time |
| **Network** | ZMQ localhost | <100µs | `latency_us` field |
| **Deserialization** | Python JSON parse | <20µs | `json.loads()` time |
| **Inference** | Transformer forward pass | <200µs | `perf_counter()` |
| **Decision** | Post-processing | <50µs | Confidence thresholding, risk checks |
| **Response** | Python → ZMQ → C++ | <50µs | Round-trip acknowledgment |
| **TOTAL** | **End-to-End (HMM Enabled)** | **<3500µs (3.5ms)** | **Alert if >5ms** |
| **TOTAL** | **End-to-End (Automated Only)** | **<500µs** | **Alert if >750µs** |

**Monitoring Strategy**:
```python
# In app.py
if msg['latency_us'] + inference_time_us > 500:
    logger.warning(f"⚠️ Latency budget exceeded: {total_latency}µs")
    metrics['latency_violations'] += 1

    if metrics['latency_violations'] > 10:
        logger.error("❌ CRITICAL: 10 latency violations - disabling live trading")
        disable_trading()
```

**Hardware Requirements**:
- **CPU**: 8+ cores (pin model inference to dedicated cores)
- **RAM**: 16GB minimum (model + message buffers)
- **Network**: Localhost only (no external network hops)
- **OS**: Linux (WSL2 acceptable, native preferred)

### 6.1 Port Configuration Summary

**Elite v2.2 Port Allocation** (WSL2 ↔ Windows):

| Port | Protocol | Role | Service |
|------|----------|------|---------|
| **5555** | PUB/SUB | Feature Stream | C++ (PUB) → Python (SUB) |
| **5556** | REP/REQ | Risk Control | C++ (REP) ← Python (REQ) |
| **5558** | REQ/REP | Trade Execution | Python (REQ) → C++ (REP) |
| **5559** | PUB/SUB | AI Heartbeat | Python (PUB) → C++ (SUB) |
| **5560** | REQ/REP | System Orchestrator | Python (REQ) → C++ (REP) |
| **5561** | DEALER/ROUTER | **HMM Query (Async)** | **C++ (DEALER) ↔ Python (ROUTER)** |
| **5562** | PUB/SUB | Mental Profile | Python (PUB) → C++ (SUB) |

**Critical Notes**:
- **Port 5561**: HMM Server requires 3ms timeout (event-driven, 8-15 requests/day)
- **Port 5562**: Mental Profile moved from 5561 in v2.2 to accommodate HMM
- **Firewall**: Ensure Windows allows ports 5555-5562 from WSL2 subnet (172.20.0.0/16)

---

## 7. Version Control & Migration

### 7.1 Protocol Versioning

**Format**: `<major>.<minor>` (e.g., 1.0, 2.0, 2.2)

- **Major version**: Breaking changes (field removal, type changes, Common Core modifications)
- **Minor version**: Additive changes (new fields, new protocols)

**Version History**:
- **v1.0** (2025-12-01): Original 46-field Common Core
- **v2.0** (2026-01-13 AM): Added `atr_10` to Training Protocol
- **v2.0.1** (2026-01-13 PM): Added OHLCV to Training Protocol
- **v2.1** (2026-01-13 Evening): Finalized elite architectural decisions, identified implementation gaps
- **v2.1.1** (2026-01-18 AM): Verified `timestamp_us` implementation
- **v2.2** (2026-01-18 PM): **CURRENT** - HMM Architecture integrated (35-field live protocol)

**C++ Changes** (EventDataCollectorStudy.cpp - ✅ COMPLETED):
```cpp
// v2.0: Add ATR (line 186-189)
const auto atrProximity = IndicatorManager::Instance()
    .GetIndicator<ATRProximityIndicator>(IndicatorKeys::ATR_PROXIMITY);
if (atrProximity) {
    payload["atr_10"] = atrProximity->GetATR10();
}

// v2.0.1: Add OHLCV (lines 177-181)
payload["open"] = sc.Open[sc.Index];
payload["high"] = sc.High[sc.Index];
payload["low"] = sprotocol version
    has_ohlcv = all(col in df_events.columns for col in ['open', 'high', 'low', 'close', 'volume'])
    has_atr = 'atr_10' in df_events.columns

    if has_ohlcv and has_atr:
        logger.info("✅ Using v2.0.1 data (OHLCV + ATR)")
        chl = CausalHorizonLabeler(stop_loss_atr_mult=2.0, take_profit_atr_mult=3.0)
        return chl.apply_labels(df_events)
    elif has_atr:
        logger.warning("⚠️ v2.0 data (ATR only) - joining OHLC from disk")
        df_bars = pl.read_parquet("data/TransformerData.parquet")
        df_events = df_events.join_asof(df_bars, on=['date', 'bar_index'])
        chl = CausalHorizonLabeler(stop_loss_atr_mult=2.0, take_profit_atr_mult=3.0)
        return chl.apply_labels(df_events)
    else:
        logger.warning("⚠️ v1.0 data - using fixed stops, joining OHLC")
        df_bars = pl.read_parquet("data/TransformerData.parquet")
        open' in msg and 'atr_10' in msg:
        return "2.0.1-training"  # OHLCV + ATR
    elif 'atr_10' in msg:
        return "2.0-training"    # ATR onlyHorizonLabeler(stop_loss_ticks=10, take_profit_ticks=20)
    # In scripts/collect_event_driven_data.py
def apply_chl_labeling(df_events):
    # Auto-detect atr_10 availability
    has_atr = 'atr_10' in df_events.columns

    if has_atr:
        logger.info("✅ Using ATR-based stops (v2.0 data)")
        chl = CausalHorizonLabeler(stop_loss_atr_mult=2.0, take_profit_atr_mult=3.0)
    else:
        logger.info("ℹ️ Using fixed stops (v1.0 data)")
        chl = CausalHorizonLabeler(stop_loss_ticks=10, take_profit_ticks=20)

    return chl.apply_labels(df_events)
```

**Version Detection** (Python side):
```python
def detect_protocol_version(msg: dict) -> str:
    """Auto-detect protocol version from message fields."""
    if 'atr_10' in msg:
        return "2.0"
    elif 'pnl_ticks' in msg:
        return "1.0-backtest"
    elif 'latency_us' in msg:
        return "1.0-live"
    elif 'date' in msg and 'bar_index' in msg:
        return "1.0-training"
    else:
        raise ValueError(f"Unknown protocol version: {list(msg.keys())}")
```

### 7.3 Backward Compatibility Guarantees

1. **Common Core is Immutable**: 46 fields never change across versions
2. **Extensions are Additive**: New fields only added, never removed
3. **Enum Values are Append-Only**: New enum values get new IDs, old IDs never reused
4. **Python Auto-Detects**: Code checks for field presence, falls back gracefully

**Breaking Change Policy**:
- Require 6-month deprecation notice
- Provide migration scripts for historical data
- Maintain parallel protocols during transition

---

## 8. Example Messages

### 8.1 Training Protocol Example (v2.0)

```json
{
  "type": "indicator_change",
  "timestamp": 1705161600000000000,
  "changed_keys": ["rsi", "interm_stochastic", "raschke_strategy_setup"],
  "is_event_driven": true,
  "event_type_id": 1,

  "last": 4750.25,
  "efficiency": 0.73,
  "velocity": 1.24,
  "volatility": 0.85,
  "rel_range": 0.92,

  "long_macd": 1,
  "long_FI13_signal": 3,
  "long_macd_divergence": 1,
  "long_imp": 3,

  "interm_stochastic": 1,
  "raschke_strategy_setup": 2,
  "raschke_tactical_trigger": 1,
  "rsi": 1,
  "interm_FI2_signal": 3,
  "ema_proximity": 2,
  "interm_macd_divergence": 1,
  "interm_imp": 3,

  "structure_test": 2,
  "volume_signal": 3,
  "atr_proximity": 2,
  "daily_bias": 3,
  "short_mkt_action": 3,
  "kangaroo_tail": 2,

  "turtle_soup": 1,
  "momentum_pinball": 2,
  "elder_breakout": 2,
  "nr7": 1,
  "holding_strategy": 2,

  "side": 1,
  "time_of_day": 2,
  "overnight_exit": 0,
  "market_regime": 2,
  "nh_nl_signal": 1,

  "oscillator_310": 3,
  "corr_es_zn": 2,
  "corr_es_dx": 2,
  "zn_trend": 3,
  "dx_trend": 2,

  "date": "2026-01-13",
  "bar_index": 42,
  "atr_10": 12.5,
  "prev_high": 4755.00,
  "prev_low": 4745.00,
  "prev_close": 4748.75,
  "dist_to_prev_high": -4.75,
  "dist_to_prev_low": 5.25,
  "dist_to_pdh": -15.25,
  "dist_to_pdl": 22.50,
  "session_high": 4760.00,
  "session_low": 4735.00,
  "regime_tenure": 8
}
```

### 8.2 Live Trading Protocol Example (v2.0)

```json
{
  "type": "indicator_change",
  "timestamp": 1705161600000000000,
  "changed_keys": ["rsi"],
  "event_type_id": 1,

  "last": 4750.25,
  "efficiency": 0.73,
  "velocity": 1.24,

  "... [all 46 Common Core fields] ...",

  "trade_signal_id": "550e8400-e29b-41d4-a716-446655440000",
  "latency_us": 42,
  "sequence_number": 1337,
  "is_live": true
}
```

### 8.3 Backtest Protocol Example (v2.0)

```json
{
  "type": "indicator_change",
  "timestamp": 1705161600000000000,

  "... [all 46 Common Core fields] ...",

  "replay_speed": 10.0,
  "session_id": "backtest_2026_01_13_run_5",
  "simulated_fill_price": 4750.50,
  "simulated_fill_time_ns": 1705161600050000000,
  "position_size": 1,
  "pnl_ticks": 15,
  "pnl_dollars": 187.50,
  "max_drawdown_ticks": -8,
  "trades_today": 3,
  "win_rate": 0.667,
  "avg_r_multiple": 1.85,
  "equity_curve": [10000.0, 10125.0, 10050.0, 10187.50]
}
```

---

## 9. Critical Implementation Notes

### 9.1 Python Elite Protocol Integration (Critical for Live Trading)

**WSL2 Networking Pattern** (IMPORTANT):
- **Python (WSL2)**: Connects to `tcp://172.20.112.1:<port>` (Windows host IP)
- **C++ (Windows)**: Binds to `tcp://0.0.0.0:<port>` (listens on all interfaces)
- **Exception**: Port 5558 (TradeExecutionClient) - Python BINDS, C++ CONNECTS (reversed)
- **Firewall**: Ensure Windows firewall allows ports 5555-5560 from WSL2 subnet

**SystemOrchestrator State Machine** (Required for Production):
```python
SystemState.UNINITIALIZED       # Initial state, no connections
    ↓ orchestrator.connect()
SystemState.WAITING_FOR_CPP     # Attempting TCP connection to port 5560
    ↓ Send REGISTER message
SystemState.NEGOTIATING         # Waiting for CONFIG_ACK from C++ Master
    ↓ Receive CONFIG_ACK, setup sockets
SystemState.REGISTERED          # All sockets created, not yet validated
    ↓ orchestrator.start_io_loop()
SystemState.INITIALIZING        # Receiving initial data (200-bar bootstrap)
    ↓ Transformer loaded, heartbeat active
SystemState.READY               # Idle, ready for trading signals
    ↓ First ENTER_LONG/SHORT signal
SystemState.ACTIVE_TRADING      # Position open, actively managing trade
    ↓ Connection timeout (60s) or sequence gap >3
SystemState.DEGRADED            # Network issues, only high-confidence trades
    ↓ 3 consecutive timeouts
SystemState.DISCONNECTED        # ❌ CRITICAL: Flatten positions, halt trading
```

**Mandatory Initialization Sequence**:
```python
# 1. Create orchestrator BEFORE any ZMQ sockets
orchestrator = SystemOrchestrator(host_ip="172.20.112.1")

# 2. Perform Master/Slave handshake
if not orchestrator.connect():
    logger.critical("Failed to connect to C++ Master - cannot proceed")
    sys.exit(1)

# 3. Register all handlers (BEFORE starting I/O loop)
orchestrator.feature_factory = zmq_client          # Port 5555 indicator updates
orchestrator.trade_execution_handler = trade_server  # Port 5558 execution confirmations
orchestrator.initialization_handler = live_agent    # Port 5557 200-bar bootstrap

# 4. Start unified I/O loop (manages all 5 ports)
orchestrator.start_io_loop()

# 5. Wait for READY state before accepting trades
while orchestrator.state != SystemState.READY:
    time.sleep(0.1)
    if orchestrator.state == SystemState.DISCONNECTED:
        logger.critical("Orchestrator failed to reach READY state")
        sys.exit(1)

logger.info("✅ Elite Protocol initialization complete")
```

**Sequence Number Validation** (Automatic via ZmqClient):
- Every message header must contain `sequence_id` (monotonic counter)
- `ZmqClient._check_sequence_id()` automatically detects gaps
- **Gap ≤ 3**: Log warning, increment dropped_message_count
- **Gap > 3**: Log critical error, consider EMERGENCY_EXIT
- Access stats via `zmq_client.get_stats()` for monitoring dashboard

**Latency Monitoring** (Real-time via Telemetry):
```python
# Track network jitter with EMA (alpha=0.3)
orchestrator.telemetry.update_network_jitter(msg['header']['latency_us'] / 1000.0)

# Alert on budget violations
if orchestrator.telemetry.network_jitter_ms > 0.5:  # 500µs budget
    logger.warning(f"⚠️ Network jitter: {orchestrator.telemetry.network_jitter_ms:.2f}ms")

# Transition to DEGRADED if sustained latency
if orchestrator.telemetry.network_jitter_ms > 1.0:  # 1ms sustained
    orchestrator.transition_state(SystemState.DEGRADED)
```

**Lazy Pirate Recovery Pattern** (TradeExecutionClient):
- Non-blocking async send (returns immediately)
- Poll for response every 20ms (no UI freeze)
- 2.5s absolute timeout → recreate socket (LINGER=0)
- Max 3 retries before declaring C++ unresponsive
- Used for port 5558 (trade execution) and port 5560 (control)

**Heartbeat Publisher Requirements** (AI Model Health):
- Bind to `tcp://0.0.0.0:5559` (Python acts as PUB server)
- Publish every 1 second: `{"type": "heartbeat", "timestamp": ISO8601, "model_status": "active", "uptime_seconds": int}`
- C++ subscribes and monitors (5s timeout → DISCONNECTED)
- Managed by `LiveAgent._publish_heartbeat()` in separate thread
- **Critical**: Must publish continuously or C++ halts all AI signals

**Error Recovery Strategy**:
```python
# Handle state transitions gracefully
if orchestrator.state == SystemState.DEGRADED:
    # Only accept high-confidence trades (>0.85)
    if action in ['ENTER_LONG', 'ENTER_SHORT'] and confidence < 0.85:
        logger.info(f"Skipping trade (confidence={confidence:.2f}, system DEGRADED)")
        action = 'STAND_ASIDE'

elif orchestrator.state == SystemState.DISCONNECTED:
    # Emergency: Flatten all positions via TradeExecutionClient
    logger.critical("❌ System DISCONNECTED - sending EMERGENCY_EXIT")
    trade_client.send_request_async(
        request_type="EMERGENCY_EXIT",
        reason="PYTHON_ORCHESTRATOR_DISCONNECTED"
    )
    # Wait for C++ acknowledgment, then halt
    response = trade_client.check_response(timeout_ms=5000)
    if response and response['status'] == 'ACK':
        logger.info("C++ acknowledged emergency exit")
    sys.exit(1)  # Require manual restart
```

---

### 9.2 Stationarity Clarification

**The Apparent Paradox**: Law #1 states "never raw price levels" but `last` field exists.

**Resolution**:
- The `last` field is used~~(Open Question Q1)~~ - ✅ RESOLVED

**Decision**: C++ now includes OHLC in Training Protocol (v2.0.1)

**Implementation**: `EventDataCollectorStudy.cpp` lines 177-181
```cpp
payload["open"] = sc.Open[sc.Index];
payload["high"] = sc.High[sc.Index];
payload["low"] = sc.Low[sc.Index];
payload["close"] = sc.Close[sc.Index];
payload["volume"] = static_cast<int64_t>(sc.Volume[sc.Index]);
```

**Benefits Realized**:
- ✅ No Python asof-join required
- ✅ Faster data loading (~3 seconds saved per file)
- ✅ Self-contained training data (no external dependencies)
- ✅ CHL labeling works immediately after data collection

**Tradeoffs Accepted**:
- JSONL files ~20% larger (acceptable for training data)
- Duplicate bar data vs disk storage (optimized for Python convenience) bar data
- Python performs asof-join to get OHLC from `data/TransformerData.parquet`

**Pros of Current Approach** (Python join):
- Keeps C++ message size small (64 fields vs 68 fields)
- C++ doesn't duplicate bar data already on disk
- Python has full flexibility to join other data sources

**Cons of Current Approach**:
- Python requires bar data file to be present
- Join operation adds 2-3 seconds to data loading
- Tight coupling between event data and bar data versions

**Alternative** (C++ includes OHLC):
- Add `open`, `high`, `low`, `close`, `volume` to Training Protocol → 69 fields
- Python gets complete data in one message
- No join required, faster loading
- **Tradeoff**: Larger JSONL files (~20% increase)

**Recommendation**: **Discuss with C++ team based on priorities:**
- If minimizing disk usage: Keep Python join
- If maximizing Python convenience: Add OHLC to C++

### 9.3 `atr_10` Placement ~~(Open Question Q2)~~ - ✅ RESOLVED (v2.1)

**Decision**: **Keep `atr_10` in Training Protocol ONLY**

**Elite Rationale**:
- **Stationary Purity**: Live Protocol must remain a "Stationary Pure-Feature" stream. Adding absolute ATR values introduces a non-stationary variable that the Transformer might accidentally overfit to.
- **Data Locality**: CHL labeling needs raw ATR to compute adaptive stops (2×/3× ATR). This is an offline labeling concern, not a live inference requirement.
- **Risk Separation**: C++ handles all position sizing and risk management in live trading. Python should never calculate position sizes from live data.

**Alternative for Live Position Sizing** (If Required):
- Python calculates position size from Common Core `volatility` (stationary ratio) + pre-configured constants
- Or: Pull ATR once per session via initial heartbeat message (session-level constant, not bar-level variable)
- Never: Send bar-by-bar ATR to live inference stream

**Status**: Production-locked in Training Protocol only

### 9.4 Sequence Number Validation ~~(Open Question Q3)~~ - ✅ RESOLVED (v2.1)

**Decision**: **Active Heartbeat & Fail-Safe Watchdog**

**Elite Rationale**:
- **Passive logging is for retail systems.** Institutional systems use mutual acknowledgment.
- **Data integrity is non-negotiable.** A single dropped message could represent a critical regime change.
- **Fail-safe architecture**: System defaults to safe state (flatten positions) on communication failure.

**Implementation - Bidirectional Watchdog**:

```python
class EliteSequenceValidator:
    """Institutional-grade sequence validation with automatic fail-safe."""
    def __init__(self, zmq_reply_socket):
        self.last_seq = None
        self.gap_count = 0
        self.reply_socket = zmq_reply_socket
        self.MAX_GAPS_PER_HOUR = 5

    def validate(self, msg):
        seq = msg['sequence_number']

        if self.last_seq is not None:
            expected = self.last_seq + 1

            if seq != expected:
                gap_size = seq - expected
                self.gap_count += 1

                logger.critical(f"🚨 SEQUENCE GAP: Expected {expected}, got {seq} (gap={gap_size})")

                # ELITE: Active response - request replay or emergency exit
                if gap_size <= 3:
                    self.reply_socket.send_json({
                        "type": "REQ_REPLAY",
                        "missing_sequences": list(range(expected, seq))
                    })
                else:
                    # Large gap - assume system fault
                    logger.critical("❌ CRITICAL GAP - Requesting emergency flatten")
                    self.reply_socket.send_json({
                        "type": "EMERGENCY_EXIT",
                        "reason": "SEQUENCE_INTEGRITY_VIOLATION",
                        "gap_size": gap_size
                    })
                    return False  # Signal to halt trading

        self.last_seq = seq
        return True
```

**C++ Response Logic** (Required Implementation):
```cpp
// In scsf_MindfulTrader - Handle Python emergency signals
if (python_msg["type"] == "EMERGENCY_EXIT") {
    SCString reason = python_msg["reason"];
    sc.AddMessageToLog("❌ PYTHON EMERGENCY EXIT: " + reason, 1);

    // Flatten all positions immediately
    PositionManager::Instance().FlattenAll(sc, "SEQUENCE_FAULT");

    // Disable trading until manual reset
    sc.SetPersistentInt(TRADING_ENABLED_FLAG, 0);
}
```

**Heartbeat Timeout** (60-minute silence detection):
- If Python receives no messages for 60 minutes during trading hours → Assume C++ crash
- Python sends alert to monitoring system
- If C++ receives no ACKs from Python for 60 minutes → Assume Python crash, flatten positions

**Status**: Required for live trading deployment

---

## 11. Python-Side Testing & Validation

### 11.1 Elite Protocol Handshake Test

**Test**: `test_elite_protocol.py`

```bash
# Terminal 1: Start mock C++ Master Controller
python test_elite_protocol.py --mode master

# Terminal 2: Test Python orchestrator connection
python test_elite_protocol.py --mode slave
```

**Expected Output**:
```
[Python] → REGISTER (version=1.0.2)
[C++] → CONFIG_ACK (ports assigned, heartbeat_interval_ms=1000)
[Python] → READY
[C++] → SYSTEM_START
✅ Elite handshake complete
```

### 11.2 Sequence Number Validation Test

**Test**: Inject dropped messages and verify detection

```python
# test_sequence_validation.py
import json

def test_sequence_gap_detection():
    zmq_client = ZmqClient(message_queue, orchestrator)

    # Send normal sequence
    for seq in [1, 2, 3, 4, 5]:
        msg = {"header": {"sequence_id": seq}, "payload": {}}
        zmq_client.update(msg)

    assert zmq_client.dropped_message_count == 0

    # Inject gap (skip 6, 7, 8)
    msg = {"header": {"sequence_id": 9}, "payload": {}}
    zmq_client.update(msg)

    # Should detect 3 dropped messages
    assert zmq_client.dropped_message_count == 3
    logger.info("✅ Sequence gap detection working")

if __name__ == "__main__":
    test_sequence_gap_detection()
```

### 11.3 Latency Budget Monitoring

**Test**: Verify latency tracking and DEGRADED state transition

```python
# test_latency_monitoring.py
def test_latency_budget():
    orchestrator = SystemOrchestrator()

    # Simulate normal latency (200µs)
    for _ in range(10):
        orchestrator.telemetry.update_network_jitter(0.2)  # 0.2ms

    assert orchestrator.telemetry.network_jitter_ms < 0.5
    assert orchestrator.state != SystemState.DEGRADED

    # Simulate latency spike (1.5ms sustained)
    for _ in range(20):
        orchestrator.telemetry.update_network_jitter(1.5)

    # Should transition to DEGRADED
    assert orchestrator.telemetry.network_jitter_ms > 1.0
    logger.info(f"Network jitter: {orchestrator.telemetry.network_jitter_ms:.2f}ms")
    logger.info("✅ Latency monitoring working")

if __name__ == "__main__":
    test_latency_budget()
```

### 11.4 WSL2 Networking Validation

**Test**: Verify cross-boundary communication (WSL2 ↔ Windows)

```bash
# On Windows (cmd.exe or PowerShell):
netstat -an | findstr "5555 5556 5557 5558 5559 5560"

# Expected output:
# TCP    0.0.0.0:5555           0.0.0.0:0              LISTENING
# TCP    0.0.0.0:5556           0.0.0.0:0              LISTENING
# TCP    0.0.0.0:5557           0.0.0.0:0              LISTENING
# TCP    0.0.0.0:5560           0.0.0.0:0              LISTENING
# TCP    0.0.0.0:5559           0.0.0.0:0              LISTENING  (from Python WSL2)
# TCP    172.20.112.1:5555      172.20.X.X:XXXXX       ESTABLISHED (from Python WSL2)

# On WSL2:
netstat -tuln | grep '5555\|5556\|5557\|5558\|5559\|5560'

# Expected output (Python perspective):
# tcp  0  0 172.20.X.X:XXXXX  172.20.112.1:5555  ESTABLISHED  (SUB to C++ PUB)
# tcp  0  0 0.0.0.0:5559       0.0.0.0:*          LISTEN       (Heartbeat PUB)
```

### 11.5 Emergency Exit Integration Test

**Test**: Verify DISCONNECTED state triggers position flattening

```python
# test_emergency_exit.py
def test_emergency_exit():
    orchestrator = SystemOrchestrator()
    orchestrator.connect()
    orchestrator.start_io_loop()

    # Simulate position open
    orchestrator.transition_state(SystemState.ACTIVE_TRADING)

    # Simulate connection loss (3 consecutive timeouts)
    orchestrator.transition_state(SystemState.DISCONNECTED)

    # Verify emergency exit triggered
    # (Requires mock TradeExecutionClient to capture request)
    assert mock_trade_client.last_request['type'] == 'EMERGENCY_EXIT'
    assert mock_trade_client.last_request['reason'] == 'PYTHON_ORCHESTRATOR_DISCONNECTED'

    logger.info("✅ Emergency exit protocol working")

if __name__ == "__main__":
    test_emergency_exit()
```

### 11.6 Live Trading Pre-Flight Checklist

**Before enabling real money trading**, verify ALL of the following:

- [ ] **Elite Handshake**: `orchestrator.connect()` succeeds, state reaches `READY`
- [ ] **Sequence Validation**: `zmq_client.dropped_message_count` tracked correctly
- [ ] **Latency Monitoring**: `telemetry.network_jitter_ms` updates in real-time
- [ ] **Heartbeat Active**: AI heartbeat publishes every 1s, C++ logs "✅ AI Heartbeat: N received"
- [ ] **State Transitions**: Manual state change triggers callback (test with `transition_state()`)
- [ ] **Trade Execution**: Send mock trade via `trade_client.send_request_async()`, receive ACK within 2.5s
- [ ] **WSL2 Connectivity**: Ping Windows host `ping 172.20.112.1`, verify firewall rules
- [ ] **Lazy Pirate Recovery**: Kill C++ process, verify Python detects DISCONNECTED within 5s
- [ ] **Emergency Exit**: Trigger DISCONNECTED state, verify C++ receives EMERGENCY_EXIT
- [ ] **Dashboard Integration**: GUI displays orchestrator state, telemetry, and dropped message count

**Performance Targets**:
- Message processing: <1ms (Python queue → app.py callback)
- Transformer inference: <200µs (forward pass on CPU)
- Network round-trip: <500µs (Python → C++ → Python)
- State transition: <10ms (callback notification)
- Emergency exit: <100ms (signal detection → position flatten)

---

This **Institutional-Grade Specification (v2.1)**, finalized on the evening of January 13, 2026, serves as the definitive source of truth for the Elite Event-Driven Transformer (LBRNet).

By integrating the GUI team's updates with the Elite architectural decisions, we have resolved the "Data Locality" and "Stationarity" paradoxes. The protocol is now locked for production deployment.

⚠️ **IMPLEMENTATION STATUS**: While architectural decisions are finalized, several v2.1 fields are specified but not yet implemented in C++. See Section 10.4 for critical implementation gaps requiring immediate attention.

---

## 1. Resolved Architectural Decisions (Final)

All three open questions from the design phase are now resolved and locked for production:

| ID | Decision | Rationale | Implementation Status |
|---|---|---|---|
| **Q1** | **OHLC in Training Protocol** | **Data Locality**: Eliminates the 3-second "Join Paradox" in Python. Training data is now self-describing. | ✅ **IMPLEMENTED**: `EventDataCollectorStudy.cpp` lines 177-181 (v2.0.1) |
| **Q2** | **`atr_10` Training-Only** | **Stationary Purity**: Prevents the Transformer from overfitting to absolute price scales in live inference. | ✅ **IMPLEMENTED**: `EventDataCollectorStudy.cpp` lines 186-189 (v2.0) |
| **Q3** | **Active Heartbeat Watchdog** | **Zero-Trust**: Mandatory institutional safety. Systems must mutually acknowledge liveness or flatten. | ⚠️ **SPECIFIED**: `EliteSequenceValidator` & `SystemWatchdog` design complete (Python-side only) |

---

## 2. Updated Data Flow: Training Protocol (v2.1 - Single-Source Pipeline)

With the inclusion of OHLCV and ATR, the training pipeline has transitioned from a "distributed join" model to a "high-integrity stream" model.

### The New "Single-Source" Pipeline:

```
┌─────────────────────────────────────────────────────────────────────┐
│ 1. C++ Collection (EventDataCollectorStudy)                        │
│    • Every significant indicator change triggers atomic write       │
│    • ~70 fields per event: 46 Core + OHLCV + ATR + metadata       │
│    • Frequency: 8-15 events/day (high signal-to-noise)            │
│    • Atomic flush prevents crash-induced data corruption           │
└─────────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────────┐
│ 2. Python Ingestion (scripts/collect_event_driven_data.py)         │
│    • pl.read_ndjson() loads complete event stream                  │
│    • ✅ NO join_asof required (data is self-describing)             │
│    • Verify OHLCV + atr_10 present (v2.1 assertion)               │
│    • Performance: ~3 seconds faster than v1.0 join approach       │
└─────────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────────┐
│ 3. CHL Labeling (Immediate Application)                            │
│    • CausalHorizonLabeler uses embedded atr_10 field               │
│    • Adaptive stops: 2× ATR stop-loss, 3× ATR take-profit         │
│    • Labels: ENTER_LONG, ENTER_SHORT, STAND_ASIDE, TRAP, HOLD     │
│    • 100% data integrity: Model learns exactly what C++ "saw"     │
└─────────────────────────────────────────────────────────────────────┘
                              ↓
┌─────────────────────────────────────────────────────────────────────┐
│ 4. Model Training (scripts/run_training.py)                        │
│    • Slice into sequences (50 events each)                         │
│    • TransformerAgent with focal loss + class weighting            │
│    • Target: val_enter_recall >3%, PR-AUC >0.40                   │
│    • Output: best_agent_model.keras                                │
└─────────────────────────────────────────────────────────────────────┘
```

**Result**: Complete data locality with zero external dependencies for training pipeline.

---

## 3. System Integrity Watchdog (Elite Deployment)

To maintain "Institutional-Grade" status, the communication link is supervised by a **bidirectional acknowledgment system** with automatic fail-safe logic.

### Watchdog Trigger Matrix

| Trigger | Detection | Python Action | C++ Action | Recovery Time |
|---------|-----------|---------------|------------|---------------|
| **Minor Gap (1-3)** | Missing sequence IDs | Send `REQ_REPLAY` | Resend missing packets | <100ms |
| **Major Gap (>3)** | Discontinuity detected | Send `EMERGENCY_EXIT` | **FLATTEN ALL POSITIONS** | <100ms |
| **Heartbeat Timeout** | 60 min silence | Alert Monitoring System | **FLATTEN + DISABLE TRADING** | Immediate |
| **Latency Spike** | `latency_us` > 1000µs | Transition to DEGRADED state | Log performance warning | N/A (soft degradation) |

**Implementation Note**: The watchdog requires bidirectional ZMQ communication. Python acts as the monitoring authority (detects gaps), while C++ acts as the execution authority (flattens positions).

---

## 4. Elite Implementation Checklist

Before activating live trading with v2.1 protocol, verify the following requirements:

### 4.1 Microsecond Timestamp Precision
- [x] **Verify**: `timestamp_us` field uses `sc.BaseDateTimeIn[sc.Index].AsMicrosecondsSinceBaseDate()`
- [x] **Test**: Timestamps provide microsecond resolution for temporal ordering
- [x] **Status**: ✅ **IMPLEMENTED** - Field exists in `IndicatorManager.cpp:316,401` as `timestamp_us` (Int64 microseconds)

### 4.2 Sequence Number Validation
- [ ] **Verify**: `sequence_number` field exists as monotonic Int64 counter
- [ ] **Test**: Inject dropped message, verify Python detects gap within 1 message
- [ ] **Status**: ⚠️ **NOT IMPLEMENTED** - Field specified in v2.1 but not in `IndicatorManager::GetPayload()`

### 4.3 Latency Tracking
- [ ] **Verify**: `latency_us` field calculated as (send_time - detection_time)
- [ ] **Test**: Measure under load, verify <500µs budget maintained
- [ ] **Status**: ⚠️ **NOT IMPLEMENTED** - Field specified in v2.1 but not in `IndicatorManager::GetPayload()`

### 4.4 Enum Alignment (The "Shared Contract")
- [ ] **Verify**: `include/IndicatorEnums.h` (C++) synchronizes with `lbrnet/enums.py` (Python)
- [ ] **Test**: Cross-reference all enum values, ensure no ID conflicts
- [ ] **Why**: Prevents "Silent Failure" where Python interprets `1` as `BULLISH` while C++ meant `OVERBOUGHT`
- [ ] **Status**: ✅ **IMPLEMENTED** - Validation script exists (`validate_enum_mappings.py`)

### 4.5 Atomic Writing (Training Protocol)
- [ ] **Verify**: `g_eventFileStream.flush()` called after every event write
- [ ] **Test**: Kill Sierra Chart mid-write, verify JSONL file integrity
- [ ] **Status**: ⚠️ **UNKNOWN** - Requires code review of `EventDataCollectorStudy.cpp`

### 4.6 Master/Slave Handshake (Port 5560)
- [ ] **Verify**: Python `SystemOrchestrator` completes UNINITIALIZED → READY → ACTIVE_TRADING state machine
- [ ] **Test**: Start C++ first, then Python - verify CONFIG_ACK received
- [ ] **Status**: ✅ **SPECIFIED** - Python-side implementation complete (Section 9.1)

---

## 10. Institutional-Grade System Integrity (v2.1)

### Resolved Architectural Decisions Summary

| Question | Status | Final Decision | Rationale |
|----------|--------|----------------|-----------|
| **Q1: OHLC Placement** | ✅ **RESOLVED** | Added to **Training Protocol** only. | **Data Locality**: Eliminates the "Join Paradox" and saves ~3s per file load. |
| **Q2: `atr_10` Location** | ✅ **RESOLVED** | Added to **Training Protocol** only. | **Stationary Purity**: Prevents the Transformer from overfitting to absolute price scales. |
| **Q3: Seq. Validation** | ✅ **RESOLVED** | **Active Heartbeat & Fail-Safe**. | **Zero-Trust**: Mandatory for institutional risk management. |

---

### 10.1 System Integrity Watchdog

To achieve "Elite" status, the communication link is supervised by a **bidirectional acknowledgment (ACK) system** with an automatic fail-safe mechanism.

#### Watchdog Trigger Matrix

| Trigger | Detection | Python Action | C++ Response | Result |
|---------|-----------|---------------|--------------|--------|
| **Sequence Gap (≤3)** | Missing ID detected | Send `REQ_REPLAY` | Resend specific packets | System recovers |
| **Sequence Gap (>3)** | Discontinuity | Send `EMERGENCY_EXIT` | **FLATTEN ALL** | Manual reset required |
| **H-Beat Timeout** | 60 min silence | Log Critical Error | Assume crash, **FLATTEN** | Prevents zombie trades |
| **Latency Spike** | `latency_us` > 1000 | Flag "Degraded State" | Log performance warning | System throttles speed |

---

### 10.2 Elite Final Review Checklist

Before locking the v2.1 deployment, verify these technical implementation details:

#### 1. Nanosecond Precision

- [ ] `timestamp` must use highest resolution: `sc.BaseDateTimeIn[sc.Index].ToUNIXTime() * 1e9`.
- [ ] Verify precision: Timestamps must show unique sub-microsecond increments during high-volatility events.

#### 2. Enum Alignment (The "Shared Contract")

- [ ] Shared Header: `include/IndicatorEnums.h` (C++) must be the source of truth for `lbrnet/enums.py`.
- [ ] **Why**: Prevents "Silent Failure" where Python interprets `1` as `BULLISH` while C++ meant `OVERBOUGHT`.

#### 3. Atomic Writing (Training Protocol)

- [ ] Flush JSONL to disk after every event:
```cpp
g_eventFileStream << payload.dump() << '\n';
g_eventFileStream.flush(); // REQUIRED FOR ATOMICITY
```
- [ ] **Why**: Prevents data corruption during Sierra Chart crashes.

---

### 10.3 Version 2.2 Summary & Status

**Institutional Readiness**: 90% (Architecture Locked, Implementation Details Finalized)

**Key Wins in v2.2**:
- **HMM Integration**: 6D probability vector for regime-aware attention weighting
- **Uniform Uncertainty Fallback**: Robust error handling (3ms timeout → 0.166 uniform probs)
- **Stateless HMM Server**: Event-driven REQ/REP with SNDHWM=1 for slow hardware optimization
- **Field Count Clarity**: 46 Common Core + 6 HMM = 52 total fields (29 Transformer features)
- **Soft Entropy Gating**: `hmm_uncertain` flag allows Transformer final decision
- **Per-Session Sequence**: Reset on handshake simplifies crash recovery

**Implementation Clarifications Resolved**:
1. ✅ Timeout fallback strategy (uniform uncertainty)
2. ✅ Concurrency model (stateless, serialized, drop stale requests)
3. ✅ Field count (52 total, 29 Transformer inputs)
4. ✅ Entropy gating (soft flag approach)
5. ✅ Sequence number scope (per-session monotonic)
6. ✅ Training reprocess (full chronological, offload to Linux server)
7. ✅ Health check during handshake (ping 5561)
8. ✅ Degraded mode behavior (0.166 fallback + discrete regime)

**Next Milestone**:
1. Implement `HMMClient.cpp` with 3ms timeout and SNDHWM=1 configuration
2. Add `log_event_velocity` rolling 60-min window to IndicatorManager
3. Create `HMMRegimeIndicator` class for 6D probability storage
4. Conduct "HMM Timeout Test": Delay Python REP by 5ms, verify uniform uncertainty fallback

---

### 10.4 Critical Implementation Gaps (Requires Immediate Attention)

⚠️ **SPECIFICATION vs IMPLEMENTATION DIVERGENCE**

The following v2.1 fields are documented in this specification but **NOT YET IMPLEMENTED** in the C++ codebase:

#### Common Core Fields (Implementation Status):

| Field | Type | Specified Location | Current Status | Priority |
|-------|------|-------------------|----------------|----------|
| `timestamp_us` | Int64 | Section 3.1, Line 85 | ✅ **IMPLEMENTED** in `IndicatorManager.cpp:316,401` | **COMPLETE** |
| `sequence_number` | Int64 | Section 4.2, 9.4, Example Line 738 | ❌ Not found in `IndicatorManager::GetPayload()` | **CRITICAL** |
| `latency_us` | Int32 | Section 4.2, 6.0, Example Line 738 | ❌ Not found in `IndicatorManager::GetPayload()` | **HIGH** |

#### HMM Integration (SSOT Jan 18, 2026):

| Component | Type | Specified Location | Current Status | Priority |
|-----------|------|-------------------|----------------|----------|
| `HMMClient` | C++ Class | Section 4.2.1 | ❌ Not implemented (port 5561 REQ socket) | **CRITICAL** |
| `log_event_velocity` | Float32 | Section 4.2.1 | ❌ Not in IndicatorManager (rolling 60-min window) | **CRITICAL** |
| `HMMRegimeIndicator` | C++ Class | Section 4.2.1 | ❌ Not implemented (6D probability storage) | **CRITICAL** |
| `regime_prob_*` (6 fields) | Float32 | Section 4.2, 4.2.1 | ❌ Not in live payload | **CRITICAL** |
| Mental Profile Port | Config | Section 4.2.1 | ❌ Still on 5561, needs migration to 5562 | **HIGH** |
| Training Reprocess | Data | Section 4.2.1 | ❌ 5.9M events need HMM probability labels | **HIGH** |

#### Training Protocol Fields (Specified as ✅ but Missing):

| Field | Specified Location | Current Status |
|-------|-------------------|----------------|
| `prev_high` | Section 4.1 Training table | ❌ Not in `EventDataCollectorStudy.cpp` |
| `prev_low` | Section 4.1 Training table | ❌ Not in `EventDataCollectorStudy.cpp` |
| `dist_to_prev_high` | Section 4.1 Training table | ❌ Not in `EventDataCollectorStudy.cpp` |
| `dist_to_prev_low` | Section 4.1 Training table | ❌ Not in `EventDataCollectorStudy.cpp` |
| `dist_to_pdh` | Section 4.1 Training table | ❌ Not in `EventDataCollectorStudy.cpp` |
| `dist_to_pdl` | Section 4.1 Training table | ❌ Not in `EventDataCollectorStudy.cpp` |
| `session_high` | Section 4.1 Training table | ❌ Not in `EventDataCollectorStudy.cpp` |
| `session_low` | Section 4.1 Training table | ❌ Not in `EventDataCollectorStudy.cpp` |
| `event_enrichment` | Section 4.1 Training table | ❌ Not in `EventDataCollectorStudy.cpp` |

#### Recommended Actions:

**Option 1: Update Document to Reflect Reality** (Low Risk)
- Mark unimplemented fields as "Planned" instead of "✅ Implemented"
- Add implementation status column to all field tables
- Update Common Core count from "46 fields" to actual implemented count
- Set realistic timeline for missing critical fields

**Option 2: Implement Missing Critical Fields** (High Priority)
- ~~Add `timestamp_us` (Int64 microseconds)~~ ✅ **COMPLETE** (IndicatorManager.cpp:316)
- Add `sequence_number` (Int64 monotonic counter) with thread-safe increment
- Add `latency_us` (Int32 microseconds) calculated from detection to send time
- Update Elite Implementation Checklist (Section 4) with verification tests

**Option 3: Hybrid Approach** (Recommended)
- ~~**Immediate**: Implement timestamp_us~~ ✅ **COMPLETE** (IndicatorManager.cpp:316)
- **Immediate**: Implement the 2 remaining critical Common Core fields (sequence_number, latency_us) - Required for watchdog
- **Short-term**: Mark Training Protocol fields as "Planned for v2.2"
- **Documentation**: Add implementation status badges to all tables

#### Impact Assessment:

**With timestamp_us implemented, but without sequence_number/latency_us**:
- ✅ Temporal ordering works (timestamp_us provides microsecond precision)
- ❌ Cannot detect dropped messages (sequence validation impossible)
- ❌ Cannot measure system latency (performance monitoring blind)
- ⚠️ Can implement partial watchdog (timeout detection works, but not sequence gaps)
- ❌ Python `EliteSequenceValidator` will fail on sequence gap detection

**Without HMM Integration (35-field protocol incomplete)**:
- ❌ C++ pattern quality gating degraded (uses discrete regime instead of entropy)
- ❌ Position sizing cannot adapt to regime uncertainty
- ❌ Transformer attention weights lack regime probability context
- ⚠️ Can trade with discrete MarketRegimeIndicator (fallback to legacy behavior)
- ❌ Cannot train next-generation model (5.9M events lack HMM labels)

**Recommendation**:
1. **Block live trading deployment until remaining critical Common Core fields (sequence_number, latency_us) are implemented.**
2. **Implement HMM integration before production launch** (Critical for regime-aware position sizing and Elite protocol compliance).

**Update (Jan 18, 2026 AM)**: `timestamp_us` field verified as implemented in IndicatorManager.cpp. Remaining blockers: sequence_number and latency_us.

**Update (Jan 18, 2026 PM)**: HMM Architecture finalized. New critical gaps:
- ❌ `HMMClient` class (port 5561 REQ socket)
- ❌ `log_event_velocity` calculation in IndicatorManager
- ❌ `HMMRegimeIndicator` storage class (6D probability vector)
- ❌ Training data reprocess (5.9M events need HMM labels)
- ❌ Mental Profile port migration (5561 → 5562)

**Recommendation**: **Block live trading deployment until HMM client integration complete** (Critical for regime-aware position sizing and pattern gating).

---

## Document Change Log

| Version | Date | Changes | Author | Status |
|---------|------|---------|--------|--------|
| 1.0 | 2025-12-01 | Initial version with 46-field Common Core | C++ Team | Released |
| 2.0 | 2026-01-13 AM | Added `atr_10` to Training Protocol | C++ Team | Released |
| 2.0.1 | 2026-01-13 PM | Added OHLCV to Training Protocol, resolved Q1 | C++ Team + Python Team | Released |
| 2.1 | 2026-01-13 Evening | Finalized all open questions with elite architectural decisions, identified critical implementation gaps | C++ Team + Python Team + Quant Team | Released |
| 2.1.1 | 2026-01-18 AM | Verified `timestamp_us` implementation (microseconds), updated critical field status | Documentation Review | Released |
| 2.2.0 | 2026-01-18 PM | SSOT HMM Architecture: Added 6 HMM probability fields to live protocol (52 total fields), documented port 5561 REQ/REP pattern, event-driven HMM requests, log_event_velocity calculation, HMMRegimeIndicator storage, **implementation clarifications** (timeout fallback, concurrency model, field count, entropy gating, sequence scope, training reprocess strategy, health check, degraded mode) | Quant Team + C++ Team | **CURRENT** |

**Next Review**: 2026-01-19 (Implement HMM Client + Reprocess 5.9M Events)

**Document Status**: ✅ **v2.2 SSOT - ARCHITECTURE LOCKED / IMPLEMENTATION IN PROGRESS**

**Next Step for C++ Team**: Generate the `HMMClient.h` header and update `IndicatorManager` to support the 3ms REQ/REP loop on Port 5561.

**Critical Action Items** (Priority Order):
1. **HMMClient REQ socket** on port 5561 (3ms timeout)
2. **log_event_velocity** calculation in IndicatorManager (rolling 60-min window)
3. **HMMRegimeIndicator** class for 6D probability storage
4. **sequence_number** and **latency_us** fields (watchdog enablement)
5. **Mental Profile port migration** (5561 → 5562)
6. **Training data reprocess** (5.9M events with HMM labels)

---

**MT-UMM v2.2 - Elite Protocol Specification (Institutional Grade)**
