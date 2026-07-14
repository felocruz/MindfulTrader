# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

MindfulTrader is the **C++ producer/execution layer** (ACSIL + low-latency messaging) for a Sierra Chart algorithmic trading system. It implements the **Elder-Raschke Confluence System** — Elder's Triple Screen three-timeframe hierarchy with Raschke entry patterns on Screen 3, conditioned by a regime-aware layer (Student-t HMM, Hurst/DFA, Shannon entropy, Taleb kurtosis) — publishing FlatBuffer events over ZMQ to downstream Python consumers (`lbrnet` for ML training, `MTS` for GUI).

**No GUI logic** (belongs in `MTS`), **no ML training logic** (belongs in `lbrnet`), and **no schema source edits** outside `../schema/mts_schema.fbs` ownership rules.

## Build & Test Commands

```bash
# Full clean build (cross-compiles to Windows DLL from WSL)
./build_dll.sh

# Incremental rebuild
./build_dll.sh --no-clean

# Schema must be regenerated before build if .fbs schema changed
bash /home/rcruz/devel/VSCode/scripts/regenerate_schema.sh

# Python ZMQ integration tests
cd tests && ./run_python_tests.sh
```

**Do not** use ad-hoc `flatc` or raw `cmake`/`ninja` invocations — always go through `build_dll.sh`.

The output artifact is `build-windows/bin/MindfulTrader.dll`. This is a Windows DLL cross-compiled via `clang-cl` using the `wsl-clang-cl-release` CMake preset.

## Done Checklist

- Build succeeds via `./build_dll.sh`
- If schema was touched, `regenerate_schema.sh` was run first
- Any contract-impacting changes are documented and compatibility considered
- Native trap-risk behavior remains available and actionable without Python confirmation

## Local Git Repository

This project is a **local Git repo** (initialized 2026-07-14, `master` branch, no remote by default). Use Git-based validation (`git status`, `git diff`, changed-file queries) as a source of truth alongside file reads and runtime command exits.

- Git-ignored (regenerable): `build-windows/`, generated `include/generated/*.bak_*` backups, `__pycache__/`, `.btst`/`.lbr` binary artifacts. Generated schema headers **are** tracked.
- A `pre-commit` hook enforces the Documentation Sync Contract: if you stage one of the four mirror docs (`README-AI.md`, `.github/copilot-instructions.md`, `CLAUDE.md`, `GEMINI.md`) without the others, the commit is blocked. Bypass with `git commit --no-verify`.

## Chart Timeframes (Critical — Do Not Confuse)

- **TS1 (Screen 1)**: 240-minute bars — daily trend
- **TS2 (Screen 2)**: 60-minute bars — intermediate momentum
- **TS3 (Screen 3)**: 15-minute bars — short-term entry timing

These are **minutes**, not seconds. The bar period controls aggregation only — it does **not** reduce tick frequency.

**All three screens are equally hot paths.** `AutoLoop=1` means Sierra Chart invokes every study function on every incoming tick/trade, regardless of bar period. TS1 and TS2 receive just as many ticks as TS3.

## Architecture

### Data Flow

```
Sierra Chart Tick
    ↓
SCStudies.cpp (main ACSIL entry — calls all managers each tick)
    ├→ TripleScreen1/2/3 — update indicators per timeframe
    ├→ IndicatorManager::UpdateBarContext()
    ├→ ContextManager::UpdateStatistics()
    ├→ PositionManager::Update()
    ├→ RiskManager::Evaluate()
    ├→ TradeSignalManager::GenerateSignals()
    ├→ EventSerializer::PublishEvent()   → ZMQ PUB port 5555
    └→ AIHeartbeatMonitor               → ZMQ PUB port 5559
```

### ZMQ Ports

| Port | Type | Purpose |
|------|------|---------|
| 5555 | PUB  | Main event stream (indicators, signals, positions) |
| 5556 | REP  | Trade execution validation (request/reply) |
| 5558 | REP  | BacktestLiveAgent trade execution (REQ/REP during replay) |
| 5559 | PUB  | Heartbeat monitor (~1s interval) |
| 5560 | REP  | Control-plane handshake (CONFIG_REQ/ACK) |
| 5561 | ROUTER/DEALER | HMM regime inference (backtest + live) |

### Key Singletons

| Class | File | Role |
|-------|------|------|
| `IndicatorManager` | `src/IndicatorManager.cpp` | Lifecycle & caching for 30+ indicators (DOD) |
| `PositionManager` | `src/PositionManager.cpp` | Trade state machine, fills, P&L |
| `RiskManager` | `src/RiskManager.cpp` | Daily loss limits, Kelly sizing |
| `ContextManager` | `src/ContextManager.cpp` | Volatility, efficiency, regime detection |
| `SystemOrchestrator` | `src/SystemOrchestrator.cpp` | CONFIG_REQ/ACK handshake |
| `TransportStream` | `src/transport/TransportStream.cpp` | ZMQ PUB socket |
| `HMMClient` | `src/HMMClient.cpp` | DEALER socket to Python HMM ROUTER on 5561 |
| `TradeExecutionServer` | `src/TradeExecutionServer.cpp` | REP socket on 5558 (backtest) |

### FlatBuffers / Schema

Generated headers live in `include/generated/`. Two schemas exist:

- **`../schema/mts_schema.fbs`** — live wire schema; generates `mts_schema_generated.h`
- **`../schema/backtest_schema.fbs`** — backtesting artifact schema; generates `backtest_schema_generated.h`

Regenerate via the script above; never call `flatc` directly.

**Known FlatBuffers version state (as of 2026-05-10):**
- Headers (`include/flatbuffers/base.h`): version **25.1.24**
- System `flatc` binary (mamba mts env): version **24.3.25** — mismatch
- `mts_schema_generated.h`: asserts 25.1.24 ✓
- `backtest_schema_generated.h`: assertion updated to 25.1.24 ✓ (was stale at 24.3.25)
- Long-term fix: upgrade `flatc` to 25.1.24 and run `regenerate_schema.sh`

`CMakeLists.txt` enforces two schema contracts:
- **WS-07** (`scripts/audit_shared_root_writes.sh`): validates manual FlatBuffer writes bypass generated helpers correctly
- **WS-03** (`MindfulTraderSchemaContract` target): compiles generated policy artifacts

## Performance Rules (Hot Path)

- **No heap allocations** in recurring ACSIL update paths (all three TS screens, every tick)
- **`IndicatorManager`** uses DOD: `std::array<Indicator, IndicatorKey::COUNT>` — always use `IndicatorKey` enum lookups, never string hashes or map lookups
- **ZMQ** calls must be non-blocking on UI-sensitive paths
- Preserve microsecond timing conventions where latency is tracked

## Code Safety Rules

- Before removing any symbol, search the full repo for usages in `.h`, `.cpp`, and PCH files
- Fix root causes; do not remove symbols to silence compile errors
- If unsure whether code is used cross-project, preserve it and document concern
- **`SCDLLName("Mindful Trader - Version 2.0 Devel")`** in `SCStudies.cpp` is critical — removing it causes the DLL to fail loading in Sierra Chart

## Trap Detection (Native-First)

C++ must implement and run a native trap-risk detector in the live execution path, **independent of Python availability**. Python trap outputs are a refinement/calibration layer — not a prerequisite. If Python inference is delayed, unavailable, stale, or disagrees, C++ safety behavior is authoritative.

`StatisticalContext` (volatility, efficiency, relRange, velocity, regimeTenure) is the canonical mechanics backbone and must remain wired through TS2/TS3 → `ContextManager` → `TrainingEvent`.

Canonical weighting, thresholds, and governance gates: `../lbrnet/docs/labeling/LABELING_AND_AUGMENTATION_SPEC.md` under *TRAP weighting policy table (normative default)*.

## Backtesting Pipeline

### Architecture

`BackTesterStudy.cpp` runs during Sierra Chart replay and drives the full live pipeline (same `IndicatorManager`, `PositionManager`, `RiskManager`, `HMMClient`, `TransportStream` as production). It is not a simulation — it runs real inference.

**Three phases:**
- Phase 1: Data export (redirects to `scsf_EventDataCollector`)
- Phase 2: Pure neural network — Transformer inference, no risk filtering
- Phase 3: Risk-managed — full pipeline with `RiskManager` gating

### .btst Binary File Format

Size-prefixed FlatBuffer records, one per event. Record ordering:
1. `RunManifest` — written once at study init
2. `DecisionEvent` — one per `HasSignificantChange()` firing
3. `PredictionAck` — one per ACK/REJECT from `PositionManager`
4. `TradeRecord` — one per completed round-trip (written at exit)
5. `RunSummary` — written once at study teardown

### Join Keys

- `decision_id` (uint64, monotonic) → links `DecisionEvent` ↔ `PredictionAck`
- `execution_key` = `run_id + "::" + str(parent_internal_order_id)` → links `PredictionAck` ↔ `TradeRecord`

**Note:** `DecisionEvent.model_action` and `model_confidence` are always 0 in C++ because `WriteDecisionEventFb()` fires before Python responds. Consumers must join `DecisionEvent ← PredictionAck` on `decision_id` to get model outputs. The Python `bt_reader.py` `to_dataframes()` does this join automatically.

### Python Counterparts

| File | Role |
|------|------|
| `lbrnet/backtest/backtest_server.py` | ZMQ inference server for replay; `BacktestLiveAgent` (REQ to 5558); HMM ROUTER on 5561 |
| `lbrnet/backtest/bt_reader.py` | Reads `.btst` files into pandas DataFrames; performs decision←ack backfill join |

### Governance

- Canonical source of truth: `../docs/BACKTESTING_FRAMEWORK.md`
- Reuse the existing live message protocol and lifecycle semantics — do not create parallel business logic paths
- Do not mark backtesting work production-ready unless acceptance gates in that document are explicitly evaluated

## Key Documentation

### Wire Protocol
- `../schema/FLATBUFFER_MASTER_SPECIFICATION.md` — envelope structure, full message catalog, serialization patterns (zero-copy, builder API, request correlation), field conventions

### Trade Lifecycle
- `../docs/TRADE_EXECUTION_SYSTEM.md` — canonical governance: trade open/close flow, C++→Python→Firestore lifecycle, sovereign-grade design decisions
- `../docs/TRADE_EXECUTION_MESSAGE_PROTOCOL.md` — companion: `TradeRequest/Response/Close/CloseResponse` message specs and code examples

### Risk & Strategy
- `../docs/RISK_MANAGEMENT_SYSTEM.md` — all risk layers: daily loss limits, Kelly sizing, regime scaling, consecutive-loss gates
- `../docs/TRADING_STRATEGIES_COMPLETE_REFERENCE.md` — complete catalog: Elder Triple Screen setups, Raschke patterns, all entry/exit tactics

### HMM Integration
- `../docs/HMM_RUNTIME_REFERENCE.md` — `RiskStateUpdate` field contract, C++↔Python message flow, 16D observation vector field order
- `../docs/architecture/STUDENT_T_HMM_EVENT_TRANSFORMER_ARCHITECTURE.md` — HMM↔Transformer authority order, train/live parity rules

### Labeling & Trap Policy
- `../lbrnet/docs/labeling/LABELING_AND_AUGMENTATION_SPEC.md` — TRAP weighting policy table (normative default for native trap-risk thresholds)

### Active Roadmaps
- `../docs/ROADMAP_EXECUTION_ENGINE.md` — `PositionManager`/`RiskManager` refactor spec
- `../docs/ROADMAP_CONTEXTMANAGER_REFACTOR.md` — `ContextManager` architecture baseline and remaining hardening work
- `../docs/SCHEMA_DRIVEN_SERIALIZATION_PARITY_INITIATIVE.md` — active initiative: eliminating train/live serialization drift between C++ event path and Python training ingestion
- `docs/ADR/execution_correctness_findings_spec.md` — 12 verified correctness/parity findings across `PositionManager`/`RiskManager`/`ChandelierStopManager`/`Scoring`/`ExecutionGate` (2026-07-10 audit). Finding 1 (`UpdateContext()` never called — entire regime-defense subsystem runs on frozen state) is highest priority; Finding 12 is a Python-port parity gap, not a C++ fix.

### Operator Guides
- `../docs/VISUAL_REGIME_TUNING_GUIDE.md` — Elder Triple Screen chart observations → specific system parameter changes

---

## Documentation Sync Contract

`README-AI.md`, `.github/copilot-instructions.md`, `CLAUDE.md`, and `GEMINI.md` are mirrors — update all four in the same change when guidance changes.
