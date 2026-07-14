# MindfulTrader Documentation Index

**Purpose:** Navigation guide for all MindfulTrader documentation
**Last Updated:** January 18, 2026

---

## 🎯 Quick Start by Role

### For Traders
- **ELDER_TRADING_METHODOLOGY.md** - Trading system philosophy
- **OVERNIGHT_MANAGEMENT_RASCHKE_TAYLOR.md** - Overnight position rules
- **ENUM_REFERENCE.md** - Indicator meanings and interpretations

### For ML Engineers
- **TRANSFORMER_TRAINING_DATA_REFERENCE.md** ⭐ - Complete training data spec (55 fields)
- **TRANSFORMER_LIVE_TRADING_PROTOCOL.md** ⭐ - Real-time inference protocol (23 fields)
- **../../docs/BACKTESTING_FRAMEWORK.md** ⭐ - Canonical backtesting framework/spec/roadmap
- **schema.py** - Python Pydantic schemas

### For C++ Developers
- **ARCHITECTURE.md** - System architecture overview
- **ELITE_MESSAGING_PROTOCOL_SPEC.md** - IPC handshake protocol (port 5560)
- **IMPLEMENTATION_CHECKLIST_CPP.md** - C++ development tasks
- **ENUM_REFERENCE.md** - Complete enum definitions

### For System Integration
- **THREE_MESSAGE_PROTOCOLS.md** - High-level protocol comparison
- **ELITE_MESSAGING_PROTOCOL_SPEC.md** - Master/slave handshake (port 5560)
- **ELITE_WATCHDOG_GUI_INTEGRATION.md** - GUI integration patterns

---

## 📚 Documentation by Category

### 1. Data Protocols (Elite v2.0) ⭐ NEW

| Document | Purpose | Use Case | Port/Transport |
|----------|---------|----------|----------------|
| **TRANSFORMER_LIVE_TRADING_PROTOCOL.md** | Real-time indicator streaming | Live trading decisions | ZMQ PUB (5555) |
| **TRANSFORMER_TRAINING_DATA_REFERENCE.md** | Comprehensive training data | ML model training | DataCollectorStudy → JSONL |
| **../../docs/BACKTESTING_FRAMEWORK.md** | Canonical backtesting framework/spec/roadmap | Model backtesting | BackTesterStudy → JSONL |
| **THREE_MESSAGE_PROTOCOLS.md** | High-level comparison | Protocol selection | N/A (overview) |

**Key Differences:**
- **Training:** 55 fields comprehensive (quality scores, statistical context, event metadata)
- **Live Trading:** 23 fields minimal (sub-millisecond latency, event-driven)
- **Backtesting:** 38 fields validation (training inputs + forward returns)

### 2. System Architecture

| Document | Coverage | Status |
|----------|----------|--------|
| **ARCHITECTURE.md** | Complete system overview | ✅ Current |
| **ELITE_MESSAGING_PROTOCOL_SPEC.md** | IPC handshake protocol (port 5560) | ✅ Production |
| **PSYCHOLOGICAL_GATE_ARCHITECTURE.md** | HMM gating system | 🔨 In progress |
| **BINARY_BRIDGE_SPEC.md** | C++/Python data exchange | ✅ Current |

### 3. Trading System

| Document | Coverage | Status |
|----------|----------|--------|
| **ELDER_TRADING_METHODOLOGY.md** | Triple Screen system theory | ✅ Reference |
| **ELDER_TRADE_GRADING_SYSTEM.md** | Post-trade analysis framework | ✅ Reference |
| **OVERNIGHT_MANAGEMENT_RASCHKE_TAYLOR.md** | Overnight position rules | ✅ Reference |
| **EXIT_STRATEGIES_COMPREHENSIVE.md** | Exit rule taxonomy | ✅ Reference |

### 4. Indicator Reference

| Document | Coverage | Audience |
|----------|----------|----------|
| **ENUM_REFERENCE.md** | Complete enum definitions (30+ indicators) | All |
| **GUI_INDICATOR_REFERENCE.md** | Real-time GUI indicators | GUI developers |
| **CPP_TRANSFORMER_FIELDS_IMPLEMENTATION.md** | C++ field implementation | C++ developers |

### 5. Implementation Guides

| Document | Purpose | Phase |
|----------|---------|-------|
| **IMPLEMENTATION_CHECKLIST_CPP.md** | C++ development tasks | Ongoing |
| **IMPLEMENTATION_CHECKLIST_GUI.md** | GUI development tasks | Ongoing |
| **ELITE_IMPLEMENTATION_GUIDE.md** | Elite v2.0 implementation | ✅ Complete |
| **../../docs/HMM_RUNTIME_REFERENCE.md** | Canonical HMM/regime runtime contract | ✅ Current |

### 6. Data Collection & Validation

| Document | Purpose | Format |
|----------|---------|--------|
| **EVENT_DATA_COLLECTOR.md** | Event-driven training export | JSONL |
| **ENUM_VALIDATION_GUIDE.md** | Enum consistency validation | Testing |
| **INDICATOR_DEPENDENCY_VALIDATION.md** | Cross-chart validation | Testing |
| **FIELD_VALIDATION_SUMMARY.md** | Data quality checks | Testing |

### 7. Backtesting & Performance

| Document | Coverage |
|----------|----------|
| **../../docs/BACKTESTING_FRAMEWORK.md** | Canonical backtesting framework, specs, and roadmap |
| **PERFORMANCE_ATTRIBUTION_HANDOFF.md** | Trade analysis pipeline |
| **PYTHON_PERFORMANCE_ATTRIBUTION_ENGINE_SPEC.md** | Performance metrics |

### 8. AI/ML Integration

| Document | Coverage | Status |
|----------|----------|--------|
| **TRANSFORMER_TRAINING_DATA_REFERENCE.md** | Training data (55 fields) | ✅ v2.0 |
| **TRANSFORMER_LIVE_TRADING_PROTOCOL.md** | Live inference (23 fields) | ✅ v2.0 |
| **../../docs/BACKTESTING_FRAMEWORK.md** | Backtesting framework/spec/roadmap | ✅ current |
| **PSYCHOLOGICAL_GATE_ARCHITECTURE.md** | HMM gating system | 🔨 In progress |
| **AI_MODEL_HEALTH_STATUS_REQUIREMENTS.md** | Model monitoring | ✅ Current |
| **PYTHON_AI_PHASE_ROADMAP.md** | AI development phases | Planning |

### 9. Risk Management

| Document | Coverage |
|----------|----------|
| **CPP_MARKET_REGIME.md** | Regime-based risk |
| **INSTITUTIONAL_GRADE_IMPLEMENTATION_ROADMAP.md** | Risk framework |

### 10. Cross-Market Analysis

| Document | Coverage | Status |
|----------|----------|--------|
| **HEDGE_FUND_GAP_ANALYSIS.md** | ES-ZN, ES-DX correlations | ✅ Reference |
| **CPP_MARKET_REGIME.md** | Regime detection | ✅ Implemented |

### 11. GUI & Dashboard

| Document | Coverage |
|----------|----------|
| **GUI_SYSTEM_ASSESSMENT.md** | GUI architecture analysis |
| **GUI_INDICATOR_REFERENCE.md** | Real-time indicator display |
| **LIVE_AGENT_VIEW.md** | Live trading dashboard |
| **ELITE_WATCHDOG_GUI_INTEGRATION.md** | GUI integration patterns |

### 12. Pattern Detection

| Document | Coverage | Priority |
|----------|----------|----------|
| **PATTERN_DETECTION_ORDER_VALIDATION.md** | Critical ordering rules | 🚨 CRITICAL |
| **CRITICAL_FIX_MOMENTUM_PINBALL_ORDERING.md** | Specific ordering fix | 🚨 CRITICAL |

---

## 🗑️ Deprecated Documentation

| Document | Replaced By | Deprecated Date |
|----------|-------------|-----------------|
| ~~INDICATOR_MESSAGE_PROTOCOL.md~~ | TRANSFORMER_LIVE_TRADING_PROTOCOL.md | January 18, 2026 |

---

## 🎓 Learning Path

### 1. New to MindfulTrader
1. **ARCHITECTURE.md** - System overview
2. **ELDER_TRADING_METHODOLOGY.md** - Trading philosophy
3. **ENUM_REFERENCE.md** - Indicator meanings

### 2. Building ML Models
1. **TRANSFORMER_TRAINING_DATA_REFERENCE.md** - Training data format (55 fields)
2. **EVENT_DATA_COLLECTOR.md** - How to export training data
3. **ENUM_REFERENCE.md** - Enum theory and computation
4. **../../docs/BACKTESTING_FRAMEWORK.md** - Backtesting source of truth

### 3. Live Trading Integration
1. **TRANSFORMER_LIVE_TRADING_PROTOCOL.md** - Real-time protocol (23 fields)
2. **ELITE_MESSAGING_PROTOCOL_SPEC.md** - Handshake protocol (port 5560)
3. **AI_MODEL_HEALTH_STATUS_REQUIREMENTS.md** - Model monitoring

### 4. C++ Development
1. **ARCHITECTURE.md** - System design
2. **IMPLEMENTATION_CHECKLIST_CPP.md** - Development tasks
3. **PATTERN_DETECTION_ORDER_VALIDATION.md** - Critical ordering rules
4. **CPP_TRANSFORMER_FIELDS_IMPLEMENTATION.md** - Field implementation

---

## 🔍 Finding Specific Information

### How do I...

**Export training data?**
→ TRANSFORMER_TRAINING_DATA_REFERENCE.md + EVENT_DATA_COLLECTOR.md

**Connect to live trading stream?**
→ TRANSFORMER_LIVE_TRADING_PROTOCOL.md (port 5555)

**Understand indicator meanings?**
→ ENUM_REFERENCE.md (11,074 lines comprehensive)

**Implement a new indicator?**
→ IMPLEMENTATION_CHECKLIST_CPP.md + CPP_TRANSFORMER_FIELDS_IMPLEMENTATION.md

**Validate backtest results?**
→ ../../docs/BACKTESTING_FRAMEWORK.md

**Fix pattern detection bugs?**
→ PATTERN_DETECTION_ORDER_VALIDATION.md (critical ordering)

**Add cross-market correlations?**
→ HEDGE_FUND_GAP_ANALYSIS.md

**Integrate new GUI component?**
→ ELITE_WATCHDOG_GUI_INTEGRATION.md + GUI_SYSTEM_ASSESSMENT.md

---

## 📊 Data Field Quick Reference

### Field Count by Use Case

| Use Case | Field Count | Document |
|----------|-------------|----------|
| **Training Data** | 55 fields | TRANSFORMER_TRAINING_DATA_REFERENCE.md |
| **Live Trading** | 23 fields | TRANSFORMER_LIVE_TRADING_PROTOCOL.md |
| **Backtesting** | Framework/spec/roadmap | ../../docs/BACKTESTING_FRAMEWORK.md |

### Core Shared Fields (21)

All three use cases share these fields:
- **OHLCV:** date, last (live trading only uses these 2)
- **18 Indicators:** From schema.py (Triple Screen indicators)
- **Market Context:** market_regime

### Training-Only Fields (34)

- **OHLCV extras:** open, high, low, volume (4 fields)
- **Elite v2.0 Statistical Context:** volatility, efficiency, rel_range, velocity, 5 distance anchors (9 fields)
- **Quality Scores:** setup_quality, trend_alignment, pattern_conviction, volatility_regime_score, execution_quality, outcome_confidence (6 fields)
- **Event Metadata:** changed_keys, event_type, event_id, is_pattern_change (4 fields)
- **Temporal Counters:** regime_tenure, bars_in_setup, bars_since_pattern (3 fields)
- **Hypothetical Strategies:** holding_strategy variants (3 fields)
- **Other Context:** is_trend_following, close_percentile, etc. (4 fields)

### Backtest-Only Fields (3)

- **Ground Truth:** forward_return_1h, forward_return_4h, forward_return_24h

---

## 📝 Document Maintenance

### Review Schedule

- **Monthly:** Protocol documents (TRANSFORMER_* suite)
- **Quarterly:** Reference documents (ENUM_REFERENCE.md, ARCHITECTURE.md)
- **As-needed:** Implementation checklists

### Version Control

All documents include:
- Version number
- Last updated date
- Status (✅ Current, 🔨 In progress, 📝 Planning, 🗑️ Deprecated)

---

**Document Maintained By:** MindfulTrader Development Team
**Last Updated:** January 18, 2026
