---
domain: sierra_chart/replay
intent: ACSIL replay/backtesting API — IsReplayRunning, IsFullRecalculation, replay modes, study call triggers, and canonical guard patterns
scope: global
tags: [replay, backtesting, acsil, IsFullRecalculation, IsReplayRunning, AutoLoop, sc.Index, full-recalc, BackTesterStudy]
source_files:
  - src/BackTesterStudy.cpp
  - sierra_chart_dependencies/sierrachart.h
  - sierra_chart_dependencies/scconstants.h
last_verified: 2026-06-23
dependencies: []
---

# Sierra Chart ACSIL Replay & Backtesting

## Why This Exists
ACSIL studies run identically during live trading and chart replay. The critical distinction is that
replay always begins with a full recalculation pass over all historical bars, during which trading
functions must be suppressed. Getting this wrong means spurious orders on historical data, phantom
fills, and P&L calculations that never happened. This chunk documents the exact API surface and the
patterns enforced in `BackTesterStudy.cpp`.

## The Invariant / Contract

**Every study function that submits orders must guard with `if (sc.IsFullRecalculation) return;`
before any order-submission call.** This single guard prevents the replay initialization pass from
generating thousands of phantom trades on historical bars.

```cpp
// Canonical guard — place before any sc.BuyEntry / sc.SellEntry / sc.FlattenAndCancel call
if (sc.IsFullRecalculation)
    return;
```

## How It Works

### Replay State Detection

```cpp
// sc member — reads ReplayStatus != REPLAY_STOPPED
int IsReplayRunning();   // returns 1 if REPLAY_RUNNING or REPLAY_PAUSED, else 0

// Raw status integer (from scconstants.h)
enum ReplayStatus { REPLAY_STOPPED = 0, REPLAY_RUNNING = 1, REPLAY_PAUSED = 2 };
int sc.ReplayStatus;     // read-only; use IsReplayRunning() rather than this directly

// Full recalculation flag — true on chart load, study add, or replay start
int sc.IsFullRecalculation;  // boolean (0/1)
```

`IsReplayRunning()` returns true for both RUNNING and PAUSED states. The replay start always
triggers `IsFullRecalculation = 1` for the pass over historical bars.

### Time-Aware DateTime

```cpp
// Correct time accessor — returns replay simulated time during replay, system time otherwise
SCDateTime sc.GetCurrentDateTime();

// sc.CurrentDateTimeForReplay is set only during replay and reflects elapsed replay time
// (not wall-clock). At accelerated replay speeds, this can be ahead of expected time.
// Never read it directly — always use GetCurrentDateTime().
```

### Replay Modes (Backtesting Accuracy Tier)

| Mode | Description | Use Case |
|------|-------------|----------|
| Standard Replay | Fast, no tight bar synchronization | Chart review |
| Calculate Same as Real Time | Recalculates after Chart Update Interval elapses | Time-triggered logic |
| **Accurate Trading System Back Test** | Called on every High/Low/Last price change or new bar | **MindfulTrader backtesting** |
| Calculate at Every Tick/Trade | Same as Accurate but per data record | Highest precision, slowest |

**Accurate Trading System Back Test Mode** is the required mode for MindfulTrader because it
guarantees studies are called at every meaningful price change, matching how live trading works.

### Bar-Based vs. Replay Backtesting

- **Bar-Based**: Uses OHLC data only. Each bar triggers four study calls (Open → High or Low depending on Close proximity → Close). Fill timestamps mark the bar's start time. Faster.
- **Replay-Based**: Processes underlying intraday `.scid` records (1-tick to 1-minute per record). Slower, more precise. Only intraday data can be replayed.

### AutoLoop Behavior During Replay

When `sc.AutoLoop = 1`, Sierra Chart iterates `sc.Index` over all bars automatically during full
recalculation. `sc.IsNewBar()` always returns false with AutoLoop enabled — do not use it.

```cpp
// sc.SetDefaults block
sc.AutoLoop = 1;  // SC iterates sc.Index 0..ArraySize-1

// sc.IsNewBar() is incompatible with AutoLoop — returns false always
// Use sc.UpdateStartIndex == sc.ArraySize - 1 for "last bar only" logic instead
```

All three TS screens (TS1=240min, TS2=60min, TS3=15min) receive every tick with AutoLoop=1.
Bar period controls aggregation only; it does not reduce tick frequency.

### Study Update Triggers

The study function is called when:
1. Study added to chart, or chart settings changed
2. Chart reload or chartbook open
3. Data records are read from the `.scid` file during replay
4. Market data updates (trades, bid/ask, historical downloads)
5. Order and position updates

Calls occur at intervals defined by the Chart Update Interval (Global Settings), not immediately.
At accelerated replay speeds, study functions may be skipped if the interval hasn't elapsed.
Use `sc.UpdateAlways = 1` in `sc.SetDefaults` for time-triggered logic that must fire reliably.

### Calculation Order (Cross-Chart)

```cpp
// From scconstants.h
enum PrecedenceLevelEnum {
    STD_PREC_LEVEL      = 0,   // calculates first (default)
    LOW_PREC_LEVEL      = 1,
    VERY_LOW_PREC_LEVEL = 2,
};
int sc.CalculationPrecedence;  // set in sc.SetDefaults
```

For multi-chart synchronized replay (TS1/TS2/TS3): synchronization is lost if any chart skips
time periods. To avoid: disable Skip Empty Periods, use identical Session Times across all charts,
and ensure all charts load the full replay date range.

### Order Function Return Codes During Replay

All `sc.BuyEntry`, `sc.SellEntry`, etc. return skip codes during suppressed states:

```cpp
const int SCT_SKIPPED_FULL_RECALC                     = -8998;
const int SCT_SKIPPED_DOWNLOADING_HISTORICAL_DATA     = -8999;
const int SCT_SKIPPED_ONLY_ONE_TRADE_PER_BAR          = -8997;
const int SCT_SKIPPED_INVALID_INDEX_SPECIFIED         = -8996;
const int SCT_SKIPPED_TOO_MANY_NEW_BARS_DURING_UPDATE = -8995;
const int SCT_SKIPPED_AUTO_TRADING_DISABLED           = -8994;
```

Check return values in `BackTesterStudy.cpp` when diagnosing why orders are not being submitted.

### Programmatic Replay Control

```cpp
// Start/stop/control replay from within a study
int sc.StartChartReplay(int ChartNumber, float ReplaySpeed, const SCDateTime& StartDateTime);
int sc.StopChartReplay(int ChartNumber);
int sc.PauseChartReplay(int ChartNumber);
int sc.ResumeChartReplay(int ChartNumber);
int sc.ChangeChartReplaySpeed(int ChartNumber, float ReplaySpeed);
float sc.GetChartReplaySpeed(int ChartNumber);

// Query replay state of another chart
int sc.GetReplayStatusFromChart(int ChartNumber);  // nonzero = replay active

// Check if replay has fully completed
int sc.GetReplayHasFinishedStatus();
```

### Bar Closure Detection

```cpp
// Returns BHCS_BAR_HAS_CLOSED (2) for any bar except sc.ArraySize-1 (last forming bar)
// Returns BHCS_BAR_HAS_NOT_CLOSED (3) for the last bar
// Returns BHCS_SET_DEFAULTS (4) during the defaults pass
int sc.GetBarHasClosedStatus(int BarIndex);

enum {
    BHCS_BAR_HAS_CLOSED     = 2,
    BHCS_BAR_HAS_NOT_CLOSED = 3,
    BHCS_SET_DEFAULTS       = 4,
};
```

## Failure Modes

**FM-01: Phantom orders on historical bars** — Omitting `if (sc.IsFullRecalculation) return;`
causes every order call during the replay initialization pass to attempt execution against
historical bars. BacktestLiveAgent receives thousands of spurious requests.

**FM-02: Wrong time during accelerated replay** — Using `sc.CurrentDateTimeForReplay` directly
instead of `sc.GetCurrentDateTime()` gives the elapsed-time value, which can be ahead of the
expected timestamp at high replay speeds.

**FM-03: IsNewBar() always false** — Attempting to use `sc.IsNewBar()` with `AutoLoop = 1` silently
returns false. Time-at-bar-close logic breaks without any error. Use UpdateStartIndex-based checks.

**FM-04: Multi-chart sync loss** — Any chart with Skip Empty Periods enabled or a different Session
Times setting causes inter-chart desynchronization during replay. TS1/TS2/TS3 study results become
temporally inconsistent.

**FM-05: Study not called at expected replay times** — At accelerated replay speeds, the Chart
Update Interval may not elapse between replay ticks, causing study calls to be skipped entirely.
Fix: `sc.UpdateAlways = 1` in sc.SetDefaults.

## References
- [ACSIL Members — Variables and Arrays](https://www.sierrachart.com/index.php?page=doc/ACSIL_Members_Variables_And_Arrays.html)
- [ACSIL Members — Functions](https://www.sierrachart.com/index.php?page=doc/ACSIL_Members_Functions.html)
- [Replaying Charts](https://www.sierrachart.com/index.php?page=doc/ReplayChart.html)
- [Auto Trade System Back Testing](https://www.sierrachart.com/index.php?page=doc/Backtesting.php)
- [Working with ACSIL Arrays and Looping](https://www.sierrachart.com/index.php?page=doc/ACS_ArraysAndLooping.html)
- Local: `sierra_chart_dependencies/sierrachart.h` (version 25.1.24)
- Local: `sierra_chart_dependencies/scconstants.h`
- `docs/BACKTESTING_FRAMEWORK.md` — MindfulTrader-specific backtesting governance
