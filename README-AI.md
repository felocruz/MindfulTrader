# README-AI: MindfulTrader (C++ / ACSIL)

**Doc Sync Contract**: Update this file, `./.github/copilot-instructions.md`, `./CLAUDE.md`, and `./GEMINI.md` in the same change when guidance changes.

## AI Context Pointer
- Primary project context: `./.github/copilot-instructions.md`
- Workspace integration map: `../docs/README-AI-WORKSPACE.md`

**Last Updated**: May 10, 2026

## Purpose
MindfulTrader is the **C++ producer/execution layer** (ACSIL + low-latency messaging) implementing the **Elder-Raschke Confluence System** (Elder's Triple Screen hierarchy + Raschke Screen 3 patterns + HMM/entropy regime layer).

## Hard Requirements (Must Follow)
1. **Schema regeneration**: `bash /home/rcruz/devel/VSCode/scripts/regenerate_schema.sh`
2. **C++ build**: `cd /home/rcruz/devel/VSCode/MindfulTrader && ./build_dll.sh`
3. **Do not** use ad-hoc `flatc` or manual cmake/ninja flows for normal work.
4. This project is now a **local Git repo** (`master` branch, no remote by default). Use Git-based validation (`git status`, `git diff`, changed-file queries) as a source of truth alongside file reads and runtime command exits. Git-ignored: `build-windows/`, generated `*.bak_*` backups, `__pycache__/`, `.btst`/`.lbr` artifacts.

## Scope
- Owns indicator processing, event production, and execution-side protocol handling.
- Produces FlatBuffer payloads consumed by `lbrnet` and `MTS`.
- Keeps hot paths allocation-light and deterministic.

## Boundaries
- No GUI logic (belongs in `MTS`).
- No ML training logic (belongs in `lbrnet`).
- No schema source edits outside `../schema/mts_schema.fbs` ownership rules.

## Code Safety Rules
- Before removing code, search the full repo for usages (`.h`, `.cpp`, PCH-related usage).
- Fix root causes; do not remove symbols to silence compile errors.
- If unsure whether code is used cross-project, preserve it and document concern.

## ZMQ Ports

| Port | Type | Purpose |
|------|------|---------|
| 5555 | PUB  | Main event stream (indicators, signals, positions) |
| 5556 | REP  | Trade execution validation (request/reply) |
| 5558 | REP  | BacktestLiveAgent trade execution (REQ/REP during replay) |
| 5559 | PUB  | Heartbeat monitor (~1s interval) |
| 5560 | REP  | Control-plane handshake (CONFIG_REQ/ACK) |
| 5561 | ROUTER/DEALER | HMM regime inference (backtest + live) |

## Key Singletons

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

## FlatBuffers Version State (as of 2026-05-10)

- Headers (`include/flatbuffers/base.h`): **25.1.24**
- System `flatc` binary (mamba mts env): **24.3.25** — mismatch
- `mts_schema_generated.h`: asserts 25.1.24 ✓
- `backtest_schema_generated.h`: asserts 25.1.24 ✓ (updated from stale 24.3.25)
- Long-term fix: upgrade `flatc` to 25.1.24 and run `regenerate_schema.sh`

## Backtesting Pipeline

`BackTesterStudy.cpp` drives the full live pipeline during Sierra Chart replay (same managers, ZMQ, HMM as production). Three phases: data export → pure neural network → risk-managed.

### .btst Binary File Format

Size-prefixed FlatBuffer records: `RunManifest` → `DecisionEvent`(s) → `PredictionAck`(s) → `TradeRecord`(s) → `RunSummary`.

### Join Keys

- `decision_id` → links `DecisionEvent` ↔ `PredictionAck`
- `execution_key` = `run_id + "::" + str(parent_internal_order_id)` → links `PredictionAck` ↔ `TradeRecord`

**Note:** `DecisionEvent.model_action` is always 0 in C++ — `WriteDecisionEventFb()` fires before Python responds. Join `decisions ← acks` on `decision_id` to get model outputs. `bt_reader.py` `to_dataframes()` does this automatically.

### Python Counterparts (in `lbrnet/backtest/`)

| File | Role |
|------|------|
| `backtest_server.py` | ZMQ inference server; `BacktestLiveAgent` (REQ to 5558); HMM ROUTER on 5561 |
| `bt_reader.py` | Reads `.btst` files into DataFrames; performs decision←ack backfill join |

## Performance & Runtime
- Avoid heavy allocations in recurring ACSIL update paths.
- Keep ZMQ interactions non-blocking on UI-sensitive paths.
- Preserve microsecond timing conventions where latency is tracked.
- **`IndicatorManager`** currently uses a hand-written heterogeneous `IndicatorStore` (~44 differently-typed named members) plus a separate `std::array<BaseIndicator*, MAX_INDICATORS>` pointer-index array for O(1) `IndicatorKey`-enum lookup (`GetIndicator<T>(key)`, never string hashes or map lookups) — this is being migrated to true packed-array (SoA) storage with compile-time devirtualized access; see `docs/superpowers/specs/2026-08-04-indicator-manager-dod-soa-design.md`.

## Trap Detection Ownership (Native-First)
1. **TRAP = structural invalidation of the entry thesis** — a sprung-trap reversal (probe-and-fail) that fires ahead of the money-stop. It is neither the price stop nor a trend/regime shift.
2. C++ must implement and run a native trap-risk detector in the live execution path, independent of Python availability — a safety-critical first-response layer that may exit immediately.
3. **Two observers of one truth:** (a) native REACTIVE floor = completed-bar `StructureTest` reversal set (`FAILED_*`), deterministic/model-independent — the parity anchor with the labeler; (b) model ANTICIPATORY = Transformer `TRAP_*` emitted earlier from training. The model is now a first-class anticipatory input (no longer merely a future refinement).
4. **Scope SPLIT (ruling 2026-07-15):** TRAP = `FAILED_*` reversal tests ONLY. An adverse `DECISIVE_*` counter-break is a separate `REGIME_INVALIDATION` resolution, never TRAP — lumping them yields a multimodal target that degrades the anticipatory model.
5. **Anticipatory gate = dynamic Bayesian threshold τ\*** (Elkan 2001): act on model `TRAP_*` iff `p ≥ τ* = C_FP/(C_FP+C_FN)` with `C_FP=|target−price|`, `C_FN=|price−stop|`, computed from the entry-latched barriers + current price (reproducible). For tight-stop fades τ* sits at ~0.68–0.92, preventing noise exits.
6. **Arbitration:** native floor is always-on and authoritative; the model exit is ADDITIVE, acting only when fresh ∧ `p ≥ τ*` ∧ adverse to the open position. If Python is delayed/unavailable/stale/low-confidence/disagreeing, native governs. The model may LEAD (earlier) but never SUPPRESS the floor.
7. **Phase 1 = EXIT/risk only.** TRAP-as-entry (fading into the trap) is deferred to a later, separately-validated slice.
8. **Deploy gate for the anticipatory override:** out-of-sample `F_0.25 > 0.65` (precision-weighted). The native floor ships unconditionally.
9. **Priority #1:** the TRAP resolution ranks ahead of stop/target/time (matches the labeler first-hit ordering). Conservative, precision-first — reduce catastrophic loss without reversal churn.
10. **Co-evolution:** the native `StructureTest` TRAP definition must equal the labeler's (`triple_barrier_scanner.py`); the labeler routes `DECISIVE_*` out of TRAP into `REGIME_INVALIDATION`. `StatisticalContext` (volatility, efficiency, relRange, velocity, regimeTenure) remains the mechanics backbone (TS2/TS3 -> ContextManager -> TrainingEvent).
11. Rulings: `docs/ADR/triple_barrier_trap_definition_ruling.md` (Q0 split + Q1 τ*). Canonical default weighting/reliability-adjusted scoring still in `../lbrnet/docs/labeling/LABELING_AND_AUGMENTATION_SPEC.md`.
12. **Intra-bar timing (ruling 2026-07-15):** the reactive floor is completed-bar (parity + the model's training anchor); live intra-bar responsiveness is carried by τ* recomputed EVERY TICK vs current price over the standing (completed-bar-trained) model `p` — the exit fires the instant `p ≥ τ*`, intra-bar. Framed as sequential/quickest-detection + early-classification (Shiryaev/Wald; Dachraoui 2015; Mori 2017): act when the posterior crosses the cost boundary, don't wait for bar close. Intra-bar RE-INFERENCE (updating `p` within the bar) is DEFERRED pending ECTS-style training on intra-bar prefixes (else train/live OOD); Phase 1 uses per-tick τ* only.

## Cross-Project Model Artifact Convention
- Canonical HMM artifact naming used by downstream Python consumers is `models/hmm_model.pkl`.

## Key Documentation

### Wire Protocol
- `../schema/FLATBUFFER_MASTER_SPECIFICATION.md` — envelope structure, full message catalog, serialization patterns, field conventions

### Trade Lifecycle
- `../docs/TRADE_EXECUTION_SYSTEM.md` — canonical governance: trade open/close flow, C++→Python→Firestore lifecycle
- `../docs/TRADE_EXECUTION_MESSAGE_PROTOCOL.md` — `TradeRequest/Response/Close` message specs and code examples

### Risk & Strategy
- `../docs/RISK_MANAGEMENT_SYSTEM.md` — daily limits, Kelly sizing, regime scaling, consecutive-loss gates
- `../docs/TRADING_STRATEGIES_COMPLETE_REFERENCE.md` — complete catalog: all Elder Triple Screen setups, Raschke patterns, entry/exit tactics

### HMM Integration
- `../docs/HMM_RUNTIME_REFERENCE.md` — `RiskStateUpdate` field contract, C++↔Python message flow, 16D observation vector
- `../docs/architecture/STUDENT_T_HMM_EVENT_TRANSFORMER_ARCHITECTURE.md` — HMM↔Transformer authority order, train/live parity rules

### Labeling & Trap Policy
- `../lbrnet/docs/labeling/LABELING_AND_AUGMENTATION_SPEC.md` — TRAP weighting policy table (normative default)

### Active Roadmaps
- `../docs/ROADMAP_EXECUTION_ENGINE.md` — `PositionManager`/`RiskManager` refactor spec
- `../docs/ROADMAP_CONTEXTMANAGER_REFACTOR.md` — `ContextManager` architecture baseline and remaining hardening
- `../docs/SCHEMA_DRIVEN_SERIALIZATION_PARITY_INITIATIVE.md` — eliminating train/live serialization drift
- `docs/ADR/execution_correctness_findings_spec.md` — 12 verified correctness/parity findings across `PositionManager`/`RiskManager`/`ChandelierStopManager`/`Scoring`/`ExecutionGate` (2026-07-10 audit); Finding 1 (`UpdateContext()` never called) — RESOLVED (commit `097e11b`; `SyncRegimeState()` wired into `Update()`, `regime_state_wiring_fix_spec.md`); Finding 12 is a Python-port parity gap, not a C++ fix

### Operator Guides
- `../docs/VISUAL_REGIME_TUNING_GUIDE.md` — Triple Screen chart observations → specific parameter changes

## Done Checklist
- Build succeeds via `./build_dll.sh`.
- If schema touched, regeneration script was run first.
- Any contract-impacting changes are documented and compatibility considered.
- Native trap-risk behavior remains available and actionable without Python confirmation.
