# GEMINI.md

This file provides context for Gemini when working with the MindfulTrader project.
It is a mirror of CLAUDE.md — update both in the same change.

---

## North Star — read this before anything else, every session

`/home/rcruz/devel/VSCode/PRODUCTION_TRIAGE.md` is the cross-project source of truth for whether
this system is production-ready — `lbrnet`, `MindfulTrader`, `MTS`, and `schema` are all bound to
it, not just this repo. **Read its new "Vision" section first** (added 2026-09-03): the final-goal
vision (chart-based Triple Screen/Raschke signal generation wrapped in a truly institutional-grade,
literature-grounded, real-data-validated Gang statistical layer) stated plainly alongside an honest
current-state assessment (the HMM is currently trained on a contaminated vector and has never
passed its own fat-tail sign-off; execution-layer gates haven't been checked for HMM-regime
awareness at all) — read this before proposing any change or describing the system's maturity.
Then read `SCRATCHPAD.md`. Check readiness programmatically, don't eyeball the table:
`bash /home/rcruz/devel/VSCode/.claude/scripts/check_north_star.sh` (exit 0 = ready, 1 = not,
prints exactly which rows block it).

`MindfulTrader` currently owns or co-owns rows 3, 5, 6, 7, 10, 11, 12, 13, 14 of that document's
critical path — verify this list is still current before trusting it. Any session that changes one
of those rows' status must update `PRODUCTION_TRIAGE.md`'s `§1`/`§1.1` *and* its `NORTH_STAR_STATUS`
block in the same edit (Triage Protocol rule 7).

**NEXT MAJOR INITIATIVE (operator directive, 2026-09-03) — founding question ANSWERED 2026-09-04,
see recommendations before implementing:** `LocalRiskContext`/`RiskGateContext` (the execution-layer
risk context `RiskManager`/`ExecutionGate`/`PositionManager` actually gate on) was verified to be
blind to the HMM's own signal for 7 of its 8 hard gates — only the Amihud illiquidity veto genuinely
reads live HMM state (`InferenceManager::Instance().HmmState()->Dof()`); the other 7, including two
whose names (`paretoTopStateRatio`, `talebSignalSigma`) suggest otherwise, are blind. Operator's own
framing: a predator with regime-aware eyes but a regime-blind nervous system isn't fully a predator —
it's half of one (the entry-fusion layer already conditions *whether to pounce* on
`PredatorContext.regime`; the risk layer governing *how carefully* mostly doesn't). Full trace +
prioritized recommendations (fix 2 misleading gate names first, resolve gate 8's apparent
duplication of gate 4, extend gate 1's own proven `Dof()`-based pattern to gates 2-5 rather than
inventing a new mechanism) now live in `docs/superpowers/specs/2026-09-03-trade-execution-risk-
management-curation-initiative.md` §2/§3/§3a — not yet implemented, review before starting. Related,
not yet designed: the EVT/GPD-based "how close to the tail, and closing how fast" execution-layer
signal discussed 2026-09-03 (`lbrnet/logs/rc_gemini.log` context around `CLAUDE_BRIEF_123`) — a
candidate concrete deliverable, scoped to feed `RiskGateContext` directly, not the HMM's own
observation vector. **Living ledger opened 2026-09-03**:
`docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md` (same
spirit/format as the observation-vector ledger below) -- item 1 answered 2026-09-04, §3a's
recommendations not yet actively worked.

**Chart-based-TA → Gang-statistical reformulation initiative, opened 2026-09-04**
(`docs/superpowers/specs/2026-09-04-technical-analysis-gang-statistical-reformulation-initiative.md`)
— swing high/low, oscillator divergence, and (case study #2) Wyckoff Spring/Upthrust/Livermore
stop-harvest-reversal patterns are being reassessed for Shannon/Mandelbrot/Taleb/Pareto
reformulation, informed by real imbalance-vs-time-bar chart comparisons on real MES ticks
(`tools/visualization/imbalance_vs_time_bars.py`). **Standing caution, same day**: this is
genuinely elite-grade methodology (extrapolation-risk-aware EVT/GPD calibration, cross-checked via
independent Gemini review — e.g. `amihud_illiquidity`'s winsorization bound recalibrated 2706→3036
this session, `CLAUDE_BRIEF_125`/`_REPLY`) but methodology rigor is not the same claim as a
validated trading edge — keep the Vision section's honest current-state gaps in view, don't let
this work read as "therefore closer to done" than it is.

**Row 1 / activity-clock observation-vector thread, condensed as of 2026-08-31 — full account in
`PRODUCTION_TRIAGE.md` row 1, this mirrors `CLAUDE.md`'s own pointer**: `fast_taleb_kurtosis`
shipped as `ObservationData`'s 17th field directly (`ff22e48`/`ea8058b`); `skewness_idx` replaced
in place to the same activity-clock source (`7c51f33`), plus a real `FeatureScaler.h` calibration
bug found and fixed in the same commit. `fractal_dim`'s HMM-bound window widened to 400 bars
(`72ab967`, measured Politis-White circular block length ≈404.82), decoupled from
`PositionManager.cpp`'s gate (unchanged short window, via a new `ContextManager::
SetFractalDimShort()`) and from `recurrence_rate` (which subsequently gained its own literature
grounding — RQA on event-indexed RR-interval sequences, HRV literature — and moved to
activity-clock treatment too, alongside `mean_rev_z`/`hurst_exponent`, Clark 1973/Ané & Geman
2000/AFML ch. 2). All three are decided but not yet implemented — `hurst_exponent` is this
system's single worst HMM cross-state discriminator, so this may bear on row 1's own K=4 sign-off
question. A staleness audit found 4 `lbrnet`-side dimensionality documents now stale relative to
the above, handed to `lbrnet`'s own session rather than fixed here (`docs/superpowers/specs/
2026-08-26-activity-clock-lbrnet-handoff.md` §10). See `docs/superpowers/specs/2026-08-12-gang-
literature-grounding-spec.md` for the consolidated per-dim literature view.

**New whole-vector Elite Feature Set Curation initiative, founded 2026-08-31**
(`docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md`) — supersedes ad hoc
pairwise dim fixes with a phased methodology (Gaussian-moment audit → redundancy → Feature Saliency
→ mRMR selection), motivated by this system's Student-t HMM using hard-enforced diagonal covariance
(the real curse-of-dimensionality risk is double-counted evidence under violated conditional
independence, not covariance ill-conditioning). **Phase 0 CLOSED OUT, 2026-08-31, bar 3 ambiguous
decisions**: `burstiness_index` (robust CV, MAD/median × a newly-derived 1.4404199
Poisson-neutrality constant, NOT the standard 1.4826 — **superseded 2026-09-02, see below**),
`vol_convexity` (REMOVED from the schema, 19D→18D — decided
2026-08-25, executed today), `mean_rev_z` (median/MAD price z-score + median-centered lag-1
autocorrelation, Kim & White 2004 — do not confuse with the still-undecided `fast_mean_rev_z`/
activity-clock move above, which remains unwired). A real bug was found and fixed as a side effect
of the schema shrink: `FeatureScaler.h`'s `LOGZ_WINSOR_SIGMA_OVERRIDE` array was silently missing
one element, misaligning `liq_fragility`'s calibrated bound by one index. All native tests pass,
`./build_dll.sh --no-clean` builds clean; **committed 2026-09-02 (`d2ab57c`)** after sitting
uncommitted across multiple prior sessions — verified clean before committing. Still open:
`amihud_illiquidity`/`relative_range`/`liq_fragility` (need a literature decision, not a mechanical
fix, since `hurst_exponent` was resolved 2026-09-02). `fast_mean_rev_z`'s wire-or-drop call remains
open, but its unwired `ActivityClockMeanReversion.h` implementation was reformulated to median/MAD
2026-09-02 (`b0ab21a`) ahead of that decision. Separate
finding: re-measuring a dim's importance against the *existing* `models/hmm_model.pkl` is circular
(its state labels were learned from the pre-Phase-0, still-contaminated vector) — recorded as an
open methodological question, not yet resolved.

**`burstiness_index` superseded and FIXED, 2026-09-02** — the 1.4404199-constant formula above was
never validated against genuine per-tick data and turned out to still be broken at real tick
density. Once `lbrnet/data/raw/mes_ticks.parquet` (471.9M real ticks,
`tools/scid_processing/scid_to_ticks_parquet.cpp`, committed) existed, recalibration found the
production bound clipping ~74% of real readings. A first fix attempt (Goh-Barabási bounded
transform on the same IAT-ratio) **failed real-data re-validation** (clip rate unchanged, mean|z|
roughly doubled) — diagnosed with Gemini (`lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_121`/`122`) as
a point-mass degeneracy no post-hoc transform can repair (73.9% of real 100-tick windows have
median inter-arrival-time collapsed to the timestamp field's own 1us floor). Real fix: abandoned
inter-arrival times entirely, reformulated to a robust Index of Dispersion for Counts (Daley &
Vere-Jones 2003) over K=10 self-scaling time sub-bins (`include/EventVelocityEngine.h`), constant
`1.58113883`=√10/2 (Poisson(10)'s exact sigma/MAD, not the standard-Normal 1.4826). Full real-data
re-validation, all 471.9M ticks: mean|z|=1.14, max|z|=49.28, rate-at-bound(6.0)=1.96% — sane,
normal clip rate, no further bound recalibration needed. Committed, native tests pass,
`./build_dll.sh` clean.

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

## Standalone Analysis Tools (`tools/`)

`tools/` hosts standalone, natively-tested analysis/calibration/ingestion utilities — never
added to `CMakeLists.txt`/`build_dll.sh`; built via bare `mamba run -n mts g++ -std=c++17 ...`
(Arrow/Parquet tools also need `$(mamba run -n mts pkg-config --cflags/--libs arrow parquet)`
plus `-Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib`).

- **Organized into subfolders by function** (reorg 2026-09-02): `tools/observation_vector/`
  (16D HMM observation-vector dimension calibration/eval, incl. `market_test_stats.h`/
  `market_data_io.h`), `tools/context_pipeline/` (`.context` training-cache file I/O),
  `tools/scid_processing/` (`.scid` tick decode/mirror-sync/parquet export). Put new tools in
  the matching subfolder, not flat in `tools/`.
- **Compiled binaries go in `tools/bin/`** (gitignored) — mirrors `build-windows/bin/`'s
  convention; never mixed into the source subfolders above.
- **TOP-LEVEL DIRECTIVE (2026-09-03): always keep tool output.** Every `tools/` executable that
  can run more than a few seconds must construct a `ToolProgressLogger` and route ALL
  results/reports through its `Log()` (never a bare `std::printf`/`std::puts`) — it automatically
  archives the full transcript to `tools/output/<toolName>_<timestamp>.txt` on exit (gitignored,
  never truncated/overwritten across runs, unlike `tools/log/`'s live-progress file). This exists
  because a real ~1h43m real-tick-data validation run's only report was printed to a terminal whose
  scrollback was lost before being read, forcing a full re-run.
- Native tests use a `check(name, bool)` + `g_failures` + final `ALL PASS`/`N FAILURE(S)`
  harness convention (see `tools/context_pipeline/test_context_reader.cpp`) — no GoogleTest/CMake.

The output artifact is `build-windows/bin/MindfulTrader.dll`. This is a Windows DLL cross-compiled via `clang-cl` using the `wsl-clang-cl-release` CMake preset.

## Workspace Structure (Critical — Files Span Multiple Repos)

The workspace root is `/home/rcruz/devel/VSCode/`. **Do not search for files inside `lbrnet/` that belong to `MindfulTrader/` or `schema/`.**

```
/home/rcruz/devel/VSCode/
├── MindfulTrader/                        ← THIS REPO (C++ Sierra Chart DLL)
│   ├── src/
│   │   ├── BackTesterStudy.cpp           ← backtesting ACSIL study
│   │   ├── HMMClient.cpp
│   │   └── TradeExecutionServer.cpp
│   └── include/generated/
│       ├── backtest_schema_generated.h   ← generated from schema/backtest_schema.fbs
│       └── mts_schema_generated.h        ← generated from schema/mts_schema.fbs
├── schema/                               ← shared FlatBuffers schemas (workspace-level)
│   ├── backtest_schema.fbs
│   └── mts_schema.fbs
├── lbrnet/                               ← Python ML/inference engine
│   └── backtest/
│       ├── bt_reader.py
│       └── backtest_server.py
├── MTS/                                  ← Plotly Dash GUI
└── docs/                                 ← shared documentation
```

When Gemini tools access files across repos, use full absolute paths as shown above.

## Local Git Repository

This project is a **local Git repo** (initialized 2026-07-14, `master` branch, no remote by default). Use Git-based validation (`git status`, `git diff`, changed-file queries) as a source of truth alongside file reads and runtime command exits.

- Git-ignored (regenerable): `build-windows/`, generated `include/generated/*.bak_*` backups, `__pycache__/`, `.btst`/`.lbr` binary artifacts. Generated schema headers **are** tracked.
- A `pre-commit` hook enforces the Documentation Sync Contract: if you stage one of the four mirror docs (`README-AI.md`, `.github/copilot-instructions.md`, `CLAUDE.md`, `GEMINI.md`) without the others, the commit is blocked. Bypass with `git commit --no-verify`.

## Chart Timeframes (Critical — Do Not Confuse)

- **TS1 (Screen 1)**: 240-minute bars — daily trend
- **TS2 (Screen 2)**: 60-minute bars — intermediate momentum
- **TS3 (Screen 3)**: 15-minute bars — short-term entry timing

These are **minutes**, not seconds. **All three screens are equally hot paths.** `AutoLoop=1` means Sierra Chart invokes every study function on every incoming tick, regardless of bar period.

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

## Performance Rules (Hot Path)

- **No heap allocations** in recurring ACSIL update paths (all three TS screens, every tick)
- **`IndicatorManager`** currently uses a hand-written heterogeneous `IndicatorStore` (~44 differently-typed named members) plus a separate `std::array<BaseIndicator*, MAX_INDICATORS>` pointer-index array for O(1) `IndicatorKey`-enum lookup (`GetIndicator<T>(key)`, never string hashes or map lookups) — this is being migrated to true packed-array (SoA) storage with compile-time devirtualized access; see `docs/superpowers/specs/2026-08-04-indicator-manager-dod-soa-design.md`.
- **Permanent hybrid architecture (indicator-manager-dod-soa plan, Task 15):** the packed arrays (`IndicatorLayout.h`/`IndicatorPackedState.h`) are the canonical, devirtualized path for every hot-path read (`CheckTrigger`, `PopulateIndicatorState`, `GetTrainingEventT`, `EventSerializer`, `BackTesterStudy`'s Float32 exports). `IndicatorStore`/`BaseIndicator`/`Indicator<T>` and its leaf classes remain permanently as (a) the write-side compute engine Triple Screen calls into every tick, and (b) the read path for keys marked `StorageBlock::NotPacked`. This is not a partially-finished migration — it is the intended end-state per the design spec's "true DOD while maintaining OOD goodness" framing. A full write-side rewrite (extracting all remaining indicators' compute logic to free functions, as Task 6 did for `Macd`) remains possible as a future initiative but is out of scope. Task 15 deleted the only 4 fully-dead orphan classes with zero live callers (`Ema`, `AdxIndicator`, and the dead `IndicatorManager::GetIndicator<T>()` instantiations for `HmmStateIndicator`/`MarketClimateIndicator` — those two classes remain live, owned by `InferenceManager`, not `IndicatorManager`). All other leaf classes have live callers and stay.
- **ZMQ** calls must be non-blocking on UI-sensitive paths
- Preserve microsecond timing conventions where latency is tracked

## Code Safety Rules

- Before removing any symbol, search the full repo for usages in `.h`, `.cpp`, and PCH files
- Fix root causes; do not remove symbols to silence compile errors
- **`SCDLLName("Mindful Trader - Version 2.0 Devel")`** in `SCStudies.cpp` is critical — removing it causes the DLL to fail loading in Sierra Chart

## Trap Detection (Native-First)

**TRAP = structural invalidation of the entry thesis** — a sprung-trap reversal (`FAILED_*`) that fires ahead of the money-stop; NOT the price stop and NOT a trend/regime shift (an adverse `DECISIVE_*` counter-break is a separate `REGIME_INVALIDATION`, never TRAP — ruling 2026-07-15, `docs/ADR/triple_barrier_trap_definition_ruling.md`).

Two observers of one truth: (a) native REACTIVE floor = completed-bar `StructureTest` reversal set, deterministic, model-independent, the parity anchor with the labeler; (b) the model's ANTICIPATORY `TRAP_*` (first-class input) gated by the dynamic Bayesian threshold τ* = C_FP/(C_FP+C_FN) (Elkan 2001; C_FP=|target−price|, C_FN=|price−stop|). Native is always-on and authoritative; the model exit is additive, acting only when fresh ∧ p≥τ* ∧ adverse — if Python is stale/down/disagreeing, native governs (the model may LEAD but never SUPPRESS the floor). Phase 1 = EXIT/risk only; enable the anticipatory override only when out-of-sample F_0.25 > 0.65. Co-evolution: the native `StructureTest` TRAP definition must equal the labeler's, which routes `DECISIVE_*` into `REGIME_INVALIDATION`. Timing (ruling 2026-07-15): the reactive floor is completed-bar (parity + training anchor); live intra-bar responsiveness comes from τ* recomputed every tick vs current price over the standing completed-bar-trained p (sequential/quickest-detection + early-classification — Shiryaev/Wald, Dachraoui 2015, Mori 2017 — act when the posterior crosses the cost boundary, not at bar close). Intra-bar RE-INFERENCE (p updating within the bar) is deferred pending ECTS-style intra-bar-prefix training to avoid train/live OOD; Phase 1 uses per-tick τ* only.

## Key Documentation

### Wire Protocol
- `../schema/FLATBUFFER_MASTER_SPECIFICATION.md` — envelope structure, full message catalog

### Trade Lifecycle
- `../docs/TRADE_EXECUTION_SYSTEM.md` — trade open/close flow, C++→Python→Firestore lifecycle
- `../docs/TRADE_EXECUTION_MESSAGE_PROTOCOL.md` — `TradeRequest/Response/Close/CloseResponse` specs

### Risk & Strategy
- `../docs/RISK_MANAGEMENT_SYSTEM.md` — all risk layers: daily loss limits, Kelly sizing, regime scaling
- `../docs/TRADING_STRATEGIES_COMPLETE_REFERENCE.md` — Elder Triple Screen, Raschke patterns

### HMM Integration
- `../docs/HMM_RUNTIME_REFERENCE.md` — `RiskStateUpdate` field contract, 16D observation vector
- `../docs/architecture/STUDENT_T_HMM_EVENT_TRANSFORMER_ARCHITECTURE.md` — HMM↔Transformer authority order

### Backtesting
- `../docs/BACKTESTING_FRAMEWORK.md` — canonical spec: artifacts, promotion gates, ZMQ protocol
- `docs/BACKTESTING_ARCHITECTURE.md` — companion rationale summary

### Active Roadmaps
- `../docs/ROADMAP_EXECUTION_ENGINE.md` — **SUPERSEDED 2026-08-14**, do not implement; all four proposed upgrades already exist under different (mostly more sophisticated) mechanisms, see the doc's own "Superseded" section for the per-item mapping
- `../docs/ROADMAP_CONTEXTMANAGER_REFACTOR.md` — `ContextManager` hardening
- `docs/ADR/execution_correctness_findings_spec.md` — 12 verified correctness/parity findings across `PositionManager`/`RiskManager`/`ChandelierStopManager`/`Scoring`/`ExecutionGate` (2026-07-10 audit); Finding 1 (`UpdateContext()` never called) — RESOLVED (commit `097e11b`; `SyncRegimeState()` wired into `Update()`, `regime_state_wiring_fix_spec.md`); Finding 12 is a Python-port parity gap, not a C++ fix
- `docs/ADR/sierra_chart_data_feed_setup.md` — (Decision implemented 2026-08-04) Switched to Sierra Chart Package 11 + Denali CME-no-depth + IB execution-only for live ES trading; Package 12/MBO evaluated and not recommended. Active opportunity: replace proxies (`StudyHelperFunctions.cpp`'s "TPO Value Area Proxy" and `StructureEngine.cpp`'s close-price-histogram "Point of Control") with real Volume Profile study values via `sc.GetStudyArrayUsingID`.
- `docs/superpowers/specs/2026-08-13-observation-vector-institutional-elevation-spec.md` (plan: `docs/superpowers/plans/2026-08-13-observation-vector-institutional-elevation.md`) — (Shipped 2026-08-13, commits `8173fcf..23a2e54`) 16D observation vector + `FeatureScaler` elevated to consensus institutional practice per `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`'s literature-grounding pass. Fixed a live training-data defect (`FeatureScaler` dedupe-at-ingestion — median-collapse-to-zero was corrupting 31-71% of samples across 6 dims), Miller-Madow entropy correction, RQA epsilon fixed-recurrence-rate recalibration, Hill-plot k-selection + EWMA smoothing, and replaced moment-based skewness/kurtosis with Bowley/Moors robust estimators — including a full empirical percentile-matching threshold migration across every live risk-gate consumer (`RiskManager`/`Indicator`/`Scoring`/`TradeDecisionEngine`/`PositionManager`/`ExecutionGate`) using real MES `.scid` historical data, zero synthetic values. **Known pending follow-ups, explicitly out of this initiative's scope:** `../docs/RISK_MANAGEMENT_SYSTEM.md` §5.3/§10 and `../docs/TRADE_EXECUTION_SYSTEM.md` §H.6 still cite pre-migration kurtosis numbers; `lbrnet/scripts/context_preflight.py`'s D3 `chronic_zero_threshold` gate (`docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md`) was never implemented (lbrnet-side); two live JSON config files outside git (`/mnt/c/Trading/config/`) carry their own copy of the migrated thresholds and won't travel to a new machine without a manual sync; `config_hash`/audit-event config governance (`TRADE_EXECUTION_SYSTEM.md` §14.2) not yet implemented for `ExecutionParams`.

---

## Documentation Sync Contract

`README-AI.md`, `.github/copilot-instructions.md`, and `CLAUDE.md` are mirrors of this file — update all four in the same change when guidance changes.
