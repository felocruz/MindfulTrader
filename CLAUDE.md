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
- **`IndicatorManager`** currently uses a hand-written heterogeneous `IndicatorStore` (~44 differently-typed named members) plus a separate `std::array<BaseIndicator*, MAX_INDICATORS>` pointer-index array for O(1) `IndicatorKey`-enum lookup (`GetIndicator<T>(key)`, never string hashes or map lookups) — this is being migrated to true packed-array (SoA) storage with compile-time devirtualized access; see `docs/superpowers/specs/2026-08-04-indicator-manager-dod-soa-design.md`.
- **Permanent hybrid architecture (indicator-manager-dod-soa plan, Task 15):** the packed arrays (`IndicatorLayout.h`/`IndicatorPackedState.h`) are the canonical, devirtualized path for every hot-path read (`CheckTrigger`, `PopulateIndicatorState`, `GetTrainingEventT`, `EventSerializer`, `BackTesterStudy`'s Float32 exports). `IndicatorStore`/`BaseIndicator`/`Indicator<T>` and its leaf classes remain permanently as (a) the write-side compute engine Triple Screen calls into every tick, and (b) the read path for keys marked `StorageBlock::NotPacked`. This is not a partially-finished migration — it is the intended end-state per the design spec's "true DOD while maintaining OOD goodness" framing. A full write-side rewrite (extracting all remaining indicators' compute logic to free functions, as Task 6 did for `Macd`) remains possible as a future initiative but is out of scope. Task 15 deleted the only 4 fully-dead orphan classes with zero live callers (`Ema`, `AdxIndicator`, and the dead `IndicatorManager::GetIndicator<T>()` instantiations for `HmmStateIndicator`/`MarketClimateIndicator` — those two classes remain live, owned by `InferenceManager`, not `IndicatorManager`). All other leaf classes have live callers and stay.
- **ZMQ** calls must be non-blocking on UI-sensitive paths
- Preserve microsecond timing conventions where latency is tracked

## Code Safety Rules

- Before removing any symbol, search the full repo for usages in `.h`, `.cpp`, and PCH files
- Fix root causes; do not remove symbols to silence compile errors
- If unsure whether code is used cross-project, preserve it and document concern
- **`SCDLLName("Mindful Trader - Version 2.0 Devel")`** in `SCStudies.cpp` is critical — removing it causes the DLL to fail loading in Sierra Chart

## Trap Detection (Native-First)

**TRAP = structural invalidation of the entry thesis** — a sprung-trap reversal (`FAILED_*`) that fires ahead of the money-stop; it is NOT the price stop and NOT a trend/regime shift (an adverse `DECISIVE_*` counter-break is a separate `REGIME_INVALIDATION`, never TRAP — ruling 2026-07-15, `docs/ADR/triple_barrier_trap_definition_ruling.md`).

Two observers of one truth: (a) a native REACTIVE floor = completed-bar `StructureTest` reversal set, deterministic, model-independent, the parity anchor with the labeler; (b) the model's ANTICIPATORY `TRAP_*`, now a first-class input, gated by the dynamic Bayesian threshold τ* = C_FP/(C_FP+C_FN) (Elkan 2001; C_FP=|target−price|, C_FN=|price−stop|). Native is always-on and authoritative; the model exit is additive, acting only when fresh ∧ p≥τ* ∧ adverse — if Python is stale/down/disagreeing, native governs and the model may LEAD but never SUPPRESS the floor. Phase 1 = EXIT/risk only (TRAP-as-entry deferred); enable the anticipatory override only when out-of-sample F_0.25 > 0.65. TRAP ranks priority #1 ahead of stop/target/time. Co-evolution: the native `StructureTest` TRAP definition must equal the labeler's (`triple_barrier_scanner.py`), which routes `DECISIVE_*` out of TRAP into `REGIME_INVALIDATION`. Timing (ruling 2026-07-15): the reactive floor is completed-bar (parity + training anchor); live intra-bar responsiveness comes from τ* recomputed every tick vs current price over the standing completed-bar-trained p (sequential/quickest-detection + early-classification — Shiryaev/Wald, Dachraoui 2015, Mori 2017 — act when the posterior crosses the cost boundary, not at bar close). Intra-bar RE-INFERENCE (p updating within the bar) is deferred pending ECTS-style intra-bar-prefix training to avoid train/live OOD; Phase 1 uses per-tick τ* only.

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
- `../docs/ROADMAP_EXECUTION_ENGINE.md` — **SUPERSEDED 2026-08-14**, do not implement; all four proposed upgrades already exist under different (mostly more sophisticated) mechanisms, see the doc's own "Superseded" section for the per-item mapping
- `../docs/ROADMAP_CONTEXTMANAGER_REFACTOR.md` — `ContextManager` architecture baseline and remaining hardening work
- `../docs/SCHEMA_DRIVEN_SERIALIZATION_PARITY_INITIATIVE.md` — active initiative: eliminating train/live serialization drift between C++ event path and Python training ingestion
- `docs/ADR/execution_correctness_findings_spec.md` — 12 verified correctness/parity findings across `PositionManager`/`RiskManager`/`ChandelierStopManager`/`Scoring`/`ExecutionGate` (2026-07-10 audit). Finding 1 (`UpdateContext()` never called — entire regime-defense subsystem runs on frozen state) — RESOLVED (commit `097e11b`; `SyncRegimeState()` wired into `Update()`, `regime_state_wiring_fix_spec.md`); Finding 12 is a Python-port parity gap, not a C++ fix.
- `docs/ADR/sierra_chart_data_feed_setup.md` — (Decision implemented 2026-08-04) Switched to Sierra Chart Package 11 + Denali CME-no-depth + IB execution-only for live ES trading; Package 12/MBO evaluated and not recommended. Active opportunity: replace proxies (`StudyHelperFunctions.cpp`'s "TPO Value Area Proxy" and `StructureEngine.cpp`'s close-price-histogram "Point of Control") with real Volume Profile study values via `sc.GetStudyArrayUsingID`.
- `docs/superpowers/specs/2026-08-13-observation-vector-institutional-elevation-spec.md` (plan: `docs/superpowers/plans/2026-08-13-observation-vector-institutional-elevation.md`) — (Shipped 2026-08-13, commits `8173fcf..23a2e54`) 16D observation vector + `FeatureScaler` elevated to consensus institutional practice per `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`'s literature-grounding pass. Fixed a live training-data defect (`FeatureScaler` dedupe-at-ingestion — median-collapse-to-zero was corrupting 31-71% of samples across 6 dims), Miller-Madow entropy correction, RQA epsilon fixed-recurrence-rate recalibration, Hill-plot k-selection + EWMA smoothing, and replaced moment-based skewness/kurtosis with Bowley/Moors robust estimators — including a full empirical percentile-matching threshold migration across every live risk-gate consumer (`RiskManager`/`Indicator`/`Scoring`/`TradeDecisionEngine`/`PositionManager`/`ExecutionGate`) using real MES `.scid` historical data, zero synthetic values. **Known pending follow-ups, explicitly out of this initiative's scope:** `../docs/RISK_MANAGEMENT_SYSTEM.md` §5.3/§10 and `../docs/TRADE_EXECUTION_SYSTEM.md` §H.6 still cite pre-migration kurtosis numbers; `lbrnet/scripts/context_preflight.py`'s D3 `chronic_zero_threshold` gate (`docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md`) was never implemented (lbrnet-side); two live JSON config files outside git (`/mnt/c/Trading/config/`) carry their own copy of the migrated thresholds and won't travel to a new machine without a manual sync; `config_hash`/audit-event config governance (`TRADE_EXECUTION_SYSTEM.md` §14.2) not yet implemented for `ExecutionParams`.

### Operator Guides
- `../docs/VISUAL_REGIME_TUNING_GUIDE.md` — Elder Triple Screen chart observations → specific system parameter changes

---

## Documentation Sync Contract

`README-AI.md`, `.github/copilot-instructions.md`, `CLAUDE.md`, and `GEMINI.md` are mirrors — update all four in the same change when guidance changes.
