# Imbalance-Clock Hot-Path Migration + Standalone C++ BackTester (Real ZMQ) — Spec

**Status**: design only, nothing implemented. Written to decide scope and sequencing before
either thread gets engineering time (operator directive, 2026-09-19).

**Relationship to existing docs**: this spec does not replace
`docs/superpowers/specs/2026-09-18-predator-sniper-execution-architecture.md` — it picks up its
§3/§4/§5 open items (BV reactivity, `Y_imb` liquidity gate) and adds sequencing/scope decisions,
plus a genuinely new second thread (the standalone backtester) that doc doesn't cover.

---

## 1. Imbalance-clock hot-path migration — where to spend effort first

**Two separate things both currently called "imbalance clock" in this repo — do not conflate them**:

1. **Already-live activity-clock dims** (`fast_hurst_exponent`, `fast_taleb_kurtosis`,
   `skewness_idx`, `recurrence_rate`, `fast_mean_rev_z`) — one shared `ImbalanceBarEngine` (fixed
   threshold=700), feeding the HMM observation vector today. Only `fast_taleb_kurtosis` has live
   gate consumers (`Scoring.cpp`'s 5 checks). `fast_hurst_exponent` sits in the vector unconsumed
   by any gate, and its cross-state HMM discrimination power has never been measured — the
   standing top-priority item across the whole observation vector (see North Star pointer).
2. **The Imbalance Triple Screen** (IS1/IS2/IS3, `ImbalanceClockManager`'s K=4:4 hierarchical
   bars-of-bars cascade, `include/ImbalanceClockManager.h`/`ImbalanceContextManager.h`,
   `src/ImbalanceScreen{1,2,3}.cpp`) — a real, already-validated (lower lag-1 autocorrelation than
   K=5 at both IS1/IS2) three-tier structure. **Currently pure display** — nothing reads it for a
   trading decision.

### Ranked candidates for "use imbalance-clock data now, in a hot path that needs real speed"

| Rank | Candidate | Why ranked here |
|---|---|---|
| 1 | Redo the BV-vs-ATR reactivity test with a **clock-agnostic event definition** | Already first in the predator-sniper doc's own dependency-ordered punch list (§5 item 1). Directly targets a genuine hot path (stop/trail distance recomputed every tick). The first systematic test's event-definition method (calendar-bar True Range) structurally biased the comparison toward ATR's own clock — this is a test-design fix, not a fresh design, so it's the fastest real unblock. |
| 2 | Calibrate + wire the `Y_imb`/Kyle's-Lambda liquidity gate | Already real-data-validated (471.9M ticks, survives duration-gap/skip-1-bar controls) as genuine transient impact. Retargeted in the predator-sniper doc as a `RiskGateContext` liquidity-veto candidate. Validated but uncalibrated/unwired — shortest path to an actual *new* gate, as opposed to fixing an existing test. |
| 3 | Wire the IS1/IS2/IS3 cascade into an actual decision | Real and validated as a bar-construction scheme, but going from "correct pure-display cascade" to "feeds an execution decision" needs a new gate + new calibration + the still-unwired Gang-MACD/Phase-Coherence divergence/TRAP sub-case. Bigger lift than 1 or 2. |

**Recommendation**: do Rank 1 first (redefine the reactivity test), since it both unblocks the
punch list's own stated dependency chain and is the item most directly touching "speed" (stop
distance). Rank 2 (`Y_imb` calibration) is the faster pure win if a new gate is wanted before the
BV question is settled — the two are independent and can be reordered without cost.

**Not yet decided in this spec**: which of Rank 1/2 to actually start on. Flagged for operator
confirmation before Task 1 of either begins.

---

## 2. Standalone C++ BackTester — real ZMQ, no Sierra Chart

### 2.1 Why this is a materially bigger lift than the `market_data_replay` precedent

`market_data_replay` succeeded because `ContextManager`'s compute core (`FeatureScaler`, the
Mahalanobis trigger gate) was **already pure C++** — only 2 small dim calculators
(`mean_rev_z`/`liq_fragility`) needed extracting into standalone headers. `BackTesterStudy.cpp` is
different in kind, not degree: it does not wrap a pure core, it **is** the ACSIL coupling itself.
Every function in it takes `SCStudyInterfaceRef sc` and the classes it drives
(`PositionManager::Init(sc, ...)`/`Update(sc)`, `RiskManager::Init(sc)`/`Update(sc)`,
`IndicatorManager::UpdateBarContext(sc)`, `ActivityClockManager::Init(sc)`) are all threaded
through `sc` for bar data, persistent-variable storage, and (for `PositionManager`) real ACSIL
order submission.

**A genuinely useful fact checked this session, not assumed**: `SCStudyInterfaceRef` is
`typedef s_sc& SCStudyInterfaceRef` (`scstructures.h:113`) — `s_sc` is a **plain struct + function
pointers** (`sierrachart.h:33`, `sierra_chart_dependencies/sierrachart.h` is 4163 lines), not an
abstract interface Sierra Chart's runtime alone can construct. That means a **partial mock is
mechanically possible**: allocate a zero-initialized `s_sc`, wire only the specific members
`PositionManager`/`RiskManager`/`IndicatorManager`/`ContextManager` actually read or call (bar
arrays, `GetPersistentInt`/`SetPersistentInt`, `Symbol`/`TickSize`, order-submission function
pointers, `BaseDateTimeIn`) to our own implementations, and leave everything else unpopulated. This
is the same shape of harness quant shops build to run identical production business logic in both
prod and sim — feasible, but the exact touch-point set is **not yet enumerated** (a real audit
task, not something to guess up front).

### 2.2 Scope decision (confirmed with operator, 2026-09-19): Phase 0 = minimal, immediate-fill, real ZMQ

The actual goal, per the operator's own framing, is **not** backtest-accuracy (Sierra Chart replay
remains system-of-record for that) — it's a fast, headless dev/debug harness for the execution
layer and the ZMQ wire protocol. Confirmed scope for Phase 0:

- **Real, unmocked**: `HMMClient` (port 5561, talking to a genuinely running `lbrnet` HMM ROUTER),
  `TradeExecutionServer` (port 5558 REP, talking to a genuinely running `lbrnet`
  `backtest_server.py`/`BacktestLiveAgent`), `SystemOrchestrator`'s CONFIG_REQ/ACK handshake,
  `TransportStream` (port 5555/5559). The entire point of this tool is to exercise this plumbing
  honestly — nothing here gets simulated.
- **Simplified for Phase 0**: order fills are **immediate-fill-at-request-price** — no slippage, no
  partial fills, no real bracket-order (stop/target) mechanics simulated by a matching engine.
  Realistic fill simulation is an explicit later phase, not a Phase-0 blocker.
- **Reused, not rebuilt**: the tick/bar feed — `market_data_replay`'s existing tick-streaming +
  bar-aggregation code (`tools/market_data_replay/MarketDataReplayEngine.h`,
  `tools/observation_vector/market_data_io.h`'s `StreamTicksFullParquet`) already turns
  `lbrnet/data/raw/mes_ticks.parquet` into TS1/TS2/TS3 bars and activity-clock imbalance bars —
  this is the natural feed source, not a new ingestion path.

### 2.3 Proposed architecture

```
mes_ticks.parquet
      │  (reuse: StreamTicksFullParquet + existing bar aggregators)
      ▼
Minimal sc-shim (populates only the s_sc fields PositionManager/RiskManager/
IndicatorManager/ContextManager actually touch — bar arrays, persistent-var
storage, Symbol/TickSize, immediate-fill order submission)
      │
      ▼
Real PositionManager / RiskManager / ContextManager / IndicatorManager
(same singletons, same code — no forked business logic, per this repo's own
"backtesting is a mode over live architecture" rule, BACKTESTING_FRAMEWORK.md)
      │
      ▼
Real HMMClient (5561) ──┐
Real TradeExecutionServer (5558) ──┼──► real, running lbrnet process
Real TransportStream (5555/5559) ──┘
```

### 2.4 Task breakdown (audit before code — do not guess the shim's field list)

1. **Audit task, not yet done**: enumerate every `sc.*` member/function actually read or called by
   `PositionManager`, `RiskManager`, `IndicatorManager::UpdateBarContext`,
   `ContextManager::UpdatePriceStructure`/`CheckAndTriggerHMM`, and `ActivityClockManager::Init`/
   `Update` — grep-driven, not assumed from this session's partial reads. This produces the actual
   shim's required-field list; do not write shim code before this exists.
2. Build the minimal `s_sc` populator: bar-array feed from the reused tick/bar aggregators,
   persistent-variable storage (a plain in-memory map, replacing Sierra Chart's own persistent-var
   mechanism), `GetTradePosition`/order-submission function pointers wired to an
   immediate-fill model.
3. Wire `SystemOrchestrator`/`HMMClient`/`TradeExecutionServer`/`TransportStream` init exactly as
   `BackTesterStudy.cpp`'s own Phase 2/3 initialization block already does (reuse that sequence,
   don't reinvent it) — against a real, separately-started `lbrnet` process.
4. Standalone CLI driver (mirrors `tools/market_data_replay/MarketDataReplay.cpp`'s own shape):
   stream ticks/bars, call the shim-fed per-bar update loop, log/verify ZMQ round-trips.
5. Validation: confirm a real end-to-end round trip (observation → HMM response → prediction →
   `PositionManager` order → fill → position state change) against a live `lbrnet` process, not
   just "it compiles."
6. Defer to a later phase, explicitly out of Phase 0 scope: realistic fill simulation
   (slippage/partial fills/bracket-order matching), full ACSIL parity for anything Phase 0's
   audit doesn't find a real caller for.

### 2.5 Open questions, genuinely unresolved

1. Exact `sc.*` touch-point set for the 4 classes above (Task 1 above — the actual scope-sizing
   question for this whole effort).
2. Whether `PositionManager`'s real order-submission path (`sc.BuyEntry`/`sc.SellEntry`-equivalent
   ACSIL calls) can be intercepted at a single choke point, or is scattered across multiple call
   sites — determines how contained the immediate-fill shim can be.
3. Whether persistent-variable IDs (`BT_PHASE_ID`, `BT_HMM_CLIENT_INITIALIZED_ID`, etc.) collide
   in meaning across a shim's own in-memory store vs. Sierra Chart's real per-study persistence —
   probably fine (fresh process each run) but not yet confirmed.
4. Which `lbrnet`-side counterpart process this tool talks to — `backtest_server.py`
   (`BacktestLiveAgent`, REQ to 5558) is the existing precedent; confirm it needs no changes to
   serve a non-Sierra-Chart caller, or flag a cross-repo ask.

---

## 3. Sequencing between the two threads

Not yet decided — flagged for the next planning pass. Note the two are not entirely independent:
the standalone backtester, once it exists, becomes the natural fast-iteration harness for testing
imbalance-clock gate changes (Rank 1/2 above) without a Sierra Chart replay cycle — so there's a
real argument for doing at least Task 1 of the backtester's audit before committing to either
imbalance-clock candidate, even though the backtester itself is the larger effort.
