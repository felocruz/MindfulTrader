# Imbalance-Clock Hot-Path Migration + Standalone C++ BackTester (Real ZMQ) — Spec

**Status**: design confirmed 2026-09-19; Task 1 audit (§2.4a) done 2026-09-20; Phase 0
implementation started 2026-09-20 (§2.4b) — `PositionManager`/`RiskManager` sc.TickSize
caching, the first concrete step toward shrinking the shim's touch-point surface.

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

### 2.4a Task 1 audit result (2026-09-20, grep-driven, Explore subagent)

Resolves §2.5 open question 1. Full per-file breakdown available on request; consolidated
shim-scoping result below.

**~54 distinct `sc.*` members/functions + 14 persistent-variable IDs**, by category:

- **Price/bar arrays** (index by `sc.Index`, back-indexed for lookback): `sc.Open/High/Low/Close/
  Volume[idx]`, `sc.AskVolume/BidVolume[idx]` (ActivityClockManager only), `sc.Bid`/`sc.Ask`
  (PositionManager spread/chase calcs), `sc.BaseDateTimeIn[idx]`.
- **Temporal**: `sc.Index`, `sc.CurrentSystemDateTime`, `sc.GetCurrentDateTime()`,
  `sc.GetTradingDayDate()`, `sc.GetOHLCForDate()`, `sc.GetFirstIndexForDate()` (all 3 date-lookup
  ones only in `IndicatorManager::UpdateDailyCache`, a callee of `UpdateBarContext`).
- **Instrument info**: `sc.TickSize` (heaviest single member, 28 call sites across
  PositionManager/RiskManager), `sc.CurrencyValuePerTick`, `sc.ChartNumber`, `sc.GetChartSymbol()`.
- **Trade Service API** (`PositionManager`/`RiskManager` order submission + position/order
  queries): `sc.GetTradePosition()`, `sc.GetOrderFillArraySize()`/`GetOrderFillEntry()`,
  `sc.GetOrderByOrderID()`/`GetOrderByIndex()`, `sc.GetNearestStopOrder()`,
  `sc.GetAttachedOrderIDsForParentOrder()`, `sc.BuyOrder()`/`SellOrder()`/`BuyExit()`/`SellExit()`,
  `sc.CancelOrder()`/`CancelAllOrders()`/`FlattenAndCancelAllOrders()` — this is the concrete
  answer to §2.5 open question 2: **not a single choke point**, 13 distinct order-management call
  sites across `PositionManager` alone, all would need shim interception.
  `sc.SendOrdersToTradeService`, `sc.SelectedTradeAccount`, `sc.GetTradeAccountData()`,
  `sc.TradeServiceAccountBalance`.
- **Persistent storage**: `sc.Get/SetPersistentInt/Double/Int64(ID)` — all 14 IDs are
  `RiskManager`-owned (`RISK_CONSECUTIVE_LOSSES_ID` etc., `src/RiskManager.cpp:11-26`);
  `IndicatorManager`/`ContextManager`/`ActivityClockManager` use none in the audited methods.
- **Volume Profile**: `sc.VolumeAtPriceForBars` + its `GetSizeAtBarIndex()`/`GetVAPElementAtIndex()`
  (only in `IndicatorManager::UpdateDailyCache`, always null-checked before use — safe to shim as
  `nullptr`).
- **Misc**: `sc.Input[]` (one debug input), `sc.AddMessageToLog()`, `sc.PlaySound()`.

**Per-code-path summary**:

| Code path | Distinct sc members | Distinct sc functions | Persistent IDs |
|---|---|---|---|
| `PositionManager` | 15 | 9 | 0 |
| `RiskManager` | 8 | 14 | 14 |
| `IndicatorManager::UpdateBarContext` (incl. `UpdateDailyCache` callee) | 10 | 6 | 0 |
| `ContextManager::UpdatePriceStructure` | 3 | 0 | 0 |
| `ContextManager::CheckAndTriggerHMM` | **0 — takes no `sc` parameter at all** | 0 | 0 |
| `ActivityClockManager::Init`/`Update` | 5 | 0 | 0 |

**Key finding for shim scoping**: `ContextManager::CheckAndTriggerHMM` needs no `sc` shimming
whatsoever — it operates on already-extracted values (`now_us`, `isDataCollection`,
`syntheticVelocity`), consistent with `market_data_replay`'s own prior finding that
`ContextManager`'s trigger-decision core is already pure C++. The real shim-surface concentration
is `PositionManager`'s order-management API (13 order call sites, no single choke point) and
`RiskManager`'s persistent-variable/account-data reads — both fully enumerated above, so Task 2
(shim construction) can now proceed with a concrete member list rather than guessing.

### 2.5 Open questions, genuinely unresolved

1. ~~Exact `sc.*` touch-point set for the 4 classes above~~ — **RESOLVED 2026-09-20, see §2.4a.**
2. ~~Whether `PositionManager`'s real order-submission path can be intercepted at a single choke
   point, or is scattered across multiple call sites~~ — **RESOLVED 2026-09-20, see §2.4a: scattered
   across 13 distinct call sites (`BuyOrder`/`SellOrder`/`BuyExit`/`SellExit`/`CancelOrder`/
   `CancelAllOrders`/`FlattenAndCancelAllOrders`/etc.), no single choke point.**
3. ~~Whether persistent-variable IDs collide in meaning across a shim's own in-memory store vs.
   Sierra Chart's real per-study persistence~~ — **superseded 2026-09-20 by a sharper, real
   finding: see §2.4d.** Not an ID-collision risk (fresh process each run, that part was fine) —
   the actual gap was that `RiskManager`'s daily reset never re-fired mid-session at all, live or
   backtest. Found and fixed.
4. ~~Which `lbrnet`-side counterpart process this tool talks to~~ — **RESOLVED 2026-09-20
   (operator directive): `../lbrnet/backtest/backtest_server.py`, co-evolved together rather than
   treated as a frozen external contract. See §2.4c for the concrete host-wiring finding.**

### 2.4b Phase 0 implementation progress (2026-09-20)

**MES tick size confirmed**: 0.25 index points — already hardcoded as `kMesTickSize` in
`tools/market_data_replay/MarketDataReplayEngine.h` (no per-instrument config found in this repo
to read instead), matching real Sierra Chart `sc.TickSize` for the MES contract in production.
This is the shim's own answer for the "Instrument info" category's `sc.TickSize` in §2.4a —
a standalone (no-Sierra-Chart) backtester run against MES data can default to this constant
rather than needing a live `sc.TickSize` source.

**"Start small" step in progress**: reduce the `sc.TickSize` touch-point count found in §2.4a by
caching it once per class at `Init()` time instead of re-reading `sc.TickSize` at every call site
(mirrors `RiskManager`'s own pre-existing `m_invariants.tickSize`/`currencyPerTick` pattern,
`include/RiskManager.h:221`, which predates this initiative but was never applied consistently —
11 of `RiskManager`'s 12 `sc.TickSize`/`sc.CurrencyValuePerTick` call sites still read `sc.`
directly instead of the cached invariant). `PositionManager` gained a new `double m_tickSize`
member (`include/PositionManager.h`), set once in `Init()`. Call-site migration (both classes) is
the concrete next step — not yet done.

**DONE, 2026-09-20**: both classes fully migrated. `PositionManager` (24 sites across
`PositionManager.cpp` + 6 in `PositionManagerPatterns.cpp`) now reads `m_tickSize` everywhere
except `BuildBarrierInputs()` — a free function taking its own `sc` parameter, not a
`PositionManager` method, correctly left reading `sc.TickSize` directly (no member access
possible). `RiskManager`'s 11 remaining direct reads now use the pre-existing
`m_invariants.tickSize`/`m_invariants.currencyPerTick`; `CalculateOrderRisk()`'s `sc` parameter
became fully unused as a result, marked `[[maybe_unused]]` (matches this file's own
`RefreshKurtosisEmergencyState()` precedent) rather than removed, to avoid a signature/call-site
change beyond this step's scope. Full clean `./build_dll.sh` passes. Net effect for the
standalone-backtester shim: `sc.TickSize`/`sc.CurrencyValuePerTick` no longer need to be readable
at arbitrary call sites inside these two classes — only once, at `Init()` time.

### 2.4c lbrnet counterpart confirmed + host-wiring finding (2026-09-20)

Operator directive: the standalone C++ backtester talks to `../lbrnet/backtest/backtest_server.py`
— co-evolve the two together (this repo is authorized to request/make changes there too, not
treat it as a frozen external contract).

**Checked directly (read-only), not assumed**: `BacktestServer.__init__(self, windows_host:
Optional[str] = None)` already defaults `windows_host` via `_get_wsl_host_ip()` (reads
`/etc/resolv.conf`'s nameserver entry — the real Windows host IP, needed today because Sierra
Chart's `TradeExecutionServer`/`SystemOrchestrator` run on Windows while `backtest_server.py` runs
in WSL). It is **already parameterized** — the CLI (`backtest_server.py`'s own `main()`) exposes
`--windows-host` — and both the trade-server REQ socket (port 5558, `BacktestLiveAgent.
_ensure_trade_socket()`) and the CONFIG_REQ handshake (port 5560, `_send_config_request()`) key
off this one value.

**Conclusion**: our new standalone C++ backtester will run natively in the SAME WSL/Linux
environment as `backtest_server.py` (no Windows, no Sierra Chart) — so `backtest_server.py` needs
**zero code changes** to serve it. Just invoke `python backtest_server.py --windows-host
127.0.0.1` instead of the auto-detected real Windows IP, and both the 5558 (trade execution) and
5560 (CONFIG_REQ) sockets will correctly target our C++ tool on localhost. The separate
`_detect_wsl_host()` value (embedded in `ConfigRequestAddHmmRouterHost`, telling C++ where to
find the Python HMM ROUTER on port 5561) needs no change either — it already resolves to a real,
non-loopback WSL-visible IP, reachable from any process on the same WSL VM, Sierra-Chart-hosted
or not.

**RESOLVED 2026-09-20** (grep-verified): all three classes are minimally `sc`-coupled, good news
for shim scope. `SystemOrchestrator.cpp` touches `sc` only inside `DrawHUD(SCStudyInterfaceRef
sc)` — on-chart HUD annotation (`sc.UseTool()`, `sc.ChartNumber`), not the CONFIG_REQ/ACK
handshake logic itself; a standalone backtester has no chart to draw on and simply never calls
it. `HMMClient.cpp` has **zero** `sc.`/`SCStudyInterfaceRef` references at all — its DEALER-socket
regime-inference logic is already fully ACSIL-independent, consistent with `market_data_replay`'s
own prior finding that inference/trigger-decision cores in this codebase tend to already be
decoupled. `TradeExecutionServer.cpp` touches `sc` in exactly one real place —
`UpdateMarketContext(sc)` reading `sc.Close[sc.Index]`/`sc.TickSize`/`sc.CurrencyValuePerTick`,
all three already-audited categories from §2.4a with an established caching precedent (§2.4b) —
plus one stub method (`ValidatePatternWithLiveData`, Phase 5.2, not yet implemented) whose `sc`
parameter is already `[[maybe_unused]]`. No hidden Sierra-Chart-specific framing found in any of
the three.

### 2.4d REAL BUG found + fixed, 2026-09-20: RiskManager daily reset never re-fired mid-session

While investigating open question 3 (persistent-variable ID handling for the shim), found the
real question underneath it was sharper than "collision" — it was whether `RiskManager`'s
day-rollover detection would even fire correctly during a multi-day backtest replay (the tool's
primary use case). Traced (grep/read-verified, not assumed):

- `RiskManager::ResetDailyState(sc)` (clears `RISK_TRADING_HALTED_ID`/`RISK_CONSECUTIVE_LOSSES_ID`/
  `RISK_TRADES_EXECUTED_TODAY_ID`/`RISK_NET_TICKS_TODAY_ID`) had exactly ONE call site —
  `RiskManager::Init(sc)` — itself only invoked from `SCStudies.cpp` under `if (sc.UpdateStartIndex
  == 0)`, ACSIL's chart-load/full-recalculation guard, not a daily check.
- `RiskManager::Update(sc)` ("Continuous monitoring called every bar", its own comment) never
  re-derived `sc.CurrentSystemDateTime.GetDate()` vs. `RISK_LAST_RESET_DATE_ID` anywhere.
- **The smoking gun**: `RiskManager::EnsureMonthlyEquityTracking()`, in the SAME file, correctly
  re-derives `currentYearMonth` vs. `RISK_MONTHLY_RESET_DATE_ID` on every call (via
  `CheckMonthlyLossLimit`, itself called every `Update()`) — proving the "recheck every call"
  pattern was the intended design here, just never applied to the daily case. Confirmed this is
  this repo's own established convention elsewhere too: `IndicatorManager::UpdateDailyCache()`
  (every bar) and `StudyHelperFunctions.cpp`'s ITR tracking both re-derive current-day vs. a
  cached/persistent last-day on every call. Native `sc.IsNewTradingDay()` exists in the ACSIL API
  but is used only in Sierra Chart's own `sc_samples/` reference code, never in this repo — this
  repo's convention is the hand-rolled compare, applied everywhere except `RiskManager`'s daily
  reset.

**Impact**: not backtester-specific — a genuine live-trading gap the backtester would have
inherited and made far more visible (a live session may get away with it only because the
platform happens to be restarted/recalculated near session boundaries, undocumented and fragile;
a multi-day/month backtest replay in one continuous process would never re-trigger it at all,
silently accumulating halt flags/trade counts/consecutive losses across day boundaries instead of
resetting them).

**Fix**: added the same "recheck every call" idiom to `RiskManager::Update(sc)`'s very first
lines, calling the existing `ResetDailyState(sc)` (unchanged) when
`sc.GetPersistentInt(RISK_LAST_RESET_DATE_ID) != sc.CurrentSystemDateTime.GetDate()` — mirrors
`EnsureMonthlyEquityTracking()`'s own pattern exactly, runs before the monthly/daily loss-limit
checks later in the same function so a stale halt flag from a prior day is cleared before those
checks read it. `Init()` untouched (still correct for the chart-load case). Full clean
`./build_dll.sh` passes.

---

## 3. Sequencing between the two threads

Not yet decided — flagged for the next planning pass. Note the two are not entirely independent:
the standalone backtester, once it exists, becomes the natural fast-iteration harness for testing
imbalance-clock gate changes (Rank 1/2 above) without a Sierra Chart replay cycle — so there's a
real argument for doing at least Task 1 of the backtester's audit before committing to either
imbalance-clock candidate, even though the backtester itself is the larger effort.

## 4. Sniper / `SniperContext` / "partial ITS" thread (2026-09-20)

Genuinely new sub-thread, not yet in the task breakdown above — started from the operator's
Sniper-on-the-Imbalance-Clock vision (2026-09-18-predator-sniper-execution-architecture.md §3),
converged onto a concrete near-term scope during this session.

**Shipped**: `include/SniperContext.h` (mirrors `PredatorContext.h`'s exact POD-composition
shape — `gang`/`regime`/`inPosition` + one new field, `is3BarClosed`, the IS3-bar-close
exit-monitoring cadence gate) and `ContextManager::GetSniperContext()` (mirrors
`GetPredatorContext()` exactly — same three fields composed from the same already-computed
sources; `is3BarClosed` deliberately left `false`, no per-tick cadence hook wired yet). Full clean
`./build_dll.sh` passes.

**Key finding: `ContextManager` has zero notion of the Imbalance Clock today**, and this is a
documented, deliberate boundary — `tools/market_data_replay/MarketDataReplayEngine.h`'s own
comment states the "standing calendar-clock/imbalance-clock boundary rule" explicitly forbids
reading `ImbalanceContextManager`/`ImbalanceClockManager` from the calendar-clock path, "a
genuinely separate, not-yet-cut-over system." `SniperContext.gang` currently sources from
`ContextManager`'s calendar-clock `LocalRiskContext` — a reasonable first scaffold, but NOT the
long-term source; per the operator's own stated direction, `ImbalanceContextManager` is meant to
eventually replace `ContextManager`, so `SniperContext`'s real target source is
`ImbalanceContextManager`'s own vector/`ImbalanceRiskGateContextT`.

**"Partial ITS" scoping decision (operator directive)**: the Sniper's execution-layer needs do
NOT require the full Imbalance Triple Screen architecture (schema tables, `ImbalanceTrainingEvent`,
`lbrnet`-side coordination). `ImbalanceContextManager::BuildObservation()`/`BuildRiskGateContext()`
already return existing, stable schema types (`ImbalanceObservationData`/`ImbalanceRiskGateContextT`,
built for the separate `.imbalance.context` training stream) — reading them internally for a
Sniper decision adds zero new schema surface and needs no `lbrnet`-side awareness, exactly like
the existing regime-invalidation kill-switch's own internal Gang-math reads. Scope for a "partial"
implementation: **keep** the full K=4:4 `ImbalanceClockManager` cascade + `ImbalanceContextManager`'s
Gang-math computation (already built/tested); **skip** `ImbalanceScreen{1,2,3}.cpp`'s own
chart-display Subgraphs and `ImbalanceEventDataCollectorStudy.cpp`'s `.imbalance.context` write —
none of that serves execution-layer speed, it's training-export/observability machinery.

**Real operational gap found**: `ImbalanceClockManager::OnTick()` + `ImbalanceContextManager::Update()`
currently only run because `ImbalanceScreen1.cpp` — a separate, OPTIONAL, display-only chart
study — happens to call them (§1.2b's own producer-discipline design). A Sniper that depends on
this data for actual execution decisions cannot rely on an optional display study being present in
the chartbook; "drive the cascade" and "display IS1's value" need to be decoupled.

**CAVEAT (operator correction, 2026-09-20): `BackTesterStudy.cpp` has never actually been run,
ever.** Everything below about it is a structural/code-reading finding, not a validated-in-practice
claim — treat its sequence as a reasoned draft, not a proven one. The `ActivityClockManager` bug-
fix comment cited below reflects that someone traced the code and found/fixed a gap by inspection;
it is not evidence the fix (or the surrounding pipeline) has ever been exercised end-to-end. The
new standalone backtester may end up being the FIRST tool in this whole codebase to actually
execute this pipeline shape at all, `BackTesterStudy.cpp` included.

**By direct code reading (not execution)**: both `SCStudies.cpp` (core trading path) and
`BackTesterStudy.cpp` (Sierra-Chart-replay backtesting) already call
`ActivityClockManager::Instance().Update(sc)` unconditionally every tick (right alongside
`RiskManager::Instance().Update(sc)`/`PositionManager::Instance().Update(sc)`), but neither calls
`ImbalanceClockManager::OnTick()`/`ImbalanceContextManager::Update()` anywhere.

`BackTesterStudy.cpp` carries a directly on-point precedent for why this class of gap
matters even though it's never been run: its own code comment records that this exact file "was
previously missing" the `ActivityClockManager::Instance().Update(sc)` call entirely, which would
leave `fastTalebKurtosis` permanently at its sentinel value throughout any backtest replay (found
post-implementation, 2026-08-26, by code inspection) -- the same failure mode this thread is
trying to avoid repeating for the imbalance clock, regardless of whether either file has actually
been run yet.

**Sequencing decided (operator directive)**: prototype the "make the cascade driver unconditional"
change in the new standalone backtester first (zero risk, doesn't touch `SCStudies.cpp`/
`BackTesterStudy.cpp` at all). Once validated THERE, port the SAME change to **both**
`SCStudies.cpp` AND `BackTesterStudy.cpp` together, not just one -- they must stay consistent with
each other for the same reason the `ActivityClockManager` gap mattered: two ACSIL-coupled entry
points silently diverging on which engines get fed is a real failure class already found once in
this exact file, by inspection -- not yet disproven by any actual run of either file.

**Not yet done**: the actual cascade-driver relocation/duplication itself (in the new standalone
backtester's tick loop, or anywhere else); the `is3BarClosed` per-tick cadence hook in
`GetSniperContext()`; re-sourcing `SniperContext.gang` from `ImbalanceContextManager` instead of
`ContextManager`.

## 5. Convergence principle: mirror `SCStudies.cpp` "as practicable, not more" (operator directive, 2026-09-20)

Both `BackTesterStudy.cpp` and the new standalone backtester should evolve toward matching
`SCStudies.cpp`'s per-tick pipeline wherever practicable — but not be forced into parity where the
three contexts are genuinely different by design. `SCStudies.cpp` is the reference *sequence*
(the only one of the three that would ever face a real broker/ZMQ counterpart), not an
authoritative "production" source of truth — no part of this ecosystem is deployed anywhere (see
`/memories/repo/terminology_and_gemini_workflow.md`).

**Converge (pure state-management, no external-system dependency — no reason for the three to
differ)**: `ActivityClockManager::Instance().Update(sc)`; the imbalance-clock cascade
(`ImbalanceClockManager::OnTick()` + `ImbalanceContextManager::Update()`, the §4 gap); the core
per-tick sequence itself (`IndicatorManager::UpdateBarContext`, `ContextManager::
UpdatePriceStructure`/`CheckAndTriggerHMM`, `RiskManager::Update`, `PositionManager::Update`).

**Do not force convergence (legitimate divergence by design)**: real order submission to an ACSIL
broker connection (already a deliberate Phase 0 scope decision — §2.2's immediate-fill-at-
request-price simplification, not a gap); hardware/external-connectivity monitoring
(`AIConnectionMonitor`'s DISCONNECTED handling, `SystemOrchestrator`'s heartbeat watchdog — these
guard against real external failure modes that only apply where a real connection/broker is at
stake); GUI/HUD drawing (`SystemOrchestrator::DrawHUD`) — no chart exists in either backtest
context.

**Working rule going forward**: when a new discrepancy between the three surfaces, ask "is this
pure tick-driven state management (converge) or does it depend on a real external system/connection
that doesn't exist in this context (leave divergent)" — not "does `SCStudies.cpp` do it" alone.
