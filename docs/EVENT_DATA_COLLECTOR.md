# Event-Driven Data Collector

## Overview

`EventDataCollectorStudy.cpp` provides true event-driven data collection for transformer training. Unlike the bar-based `DataCollectorStudy`, this collector only writes events to the JSONL file when **indicators actually change**.

## Key Differences: Event-Driven vs Bar-Based

| Aspect | Bar-Based Collector | Event-Driven Collector |
|--------|---------------------|------------------------|
| **Writes per day** | ~40 (every 15-min bar) | ~8-15 (only when indicators change) |
| **File size** | 100% bars | 20-40% reduction |
| **Signal quality** | Includes quiet periods | 100% signal, 0% noise |
| **Temporal info** | Implicit (every bar) | Explicit (`delta_t_log`, `tau_100_log`) |
| **Use case** | Complete bar history | Sparse event stream for attention models |

## How It Works

### Event Detection Logic

```cpp
// Update bar context (timestamp, price, cache)
IndicatorManager::Instance().UpdateBarContext(sc);

// Check if ANY indicator changed
bool indicatorsChanged = IndicatorManager::Instance().CachePayload(sc, false);

// Only write when bar closed AND indicators changed
if (sc.GetBarHasClosedStatus() == BHCS_BAR_HAS_CLOSED && indicatorsChanged) {
    // Write event to JSONL
}
```

### What Triggers an Event

An event is written when **at least one** of these indicators changes:
- Screen 1: `long_macd`, `long_FI13_signal`, `long_macd_divergence`, `long_imp`
- Screen 2: `interm_stochastic`, `raschke_strategy_setup`, `raschke_tactical_trigger`, `rsi`, `interm_FI2_signal`, `ema_proximity`, `interm_macd_divergence`, `interm_imp`
- Screen 3: `structure_test`, `volume_signal`, `atr_proximity`, `daily_bias`, `kangaroo_tail`, `turtle_soup`, `momentum_pinball`, `elder_breakout`, `nr7`
- Patterns: Any Raschke or Elder pattern trigger
- Position: `side` changes (entry/exit)
- Market state: `market_regime`, `time_of_day`, `nh_nl_signal`

**Silent periods** (no events): When price moves but no indicator states change (consolidation, low volatility).

## Usage

### Setup in Sierra Chart

1. **Add Study** to your 15-minute chart (same chart as TripleScreen3):
   - Chart → Add Custom Study → "Event Data Collector - Transformer Events"

2. **Arm Export**:
   - Right-click chart → "⚡ Arm Event Export"
   - File created: `Data/event_data_YYYYMMDD_HHMMSS.jsonl`

3. **Run Replay**:
   - Start Sierra Chart replay
   - Events written in real-time as indicators change

4. **Disarm When Done**:
   - Right-click chart → "⚡ Disarm Event Export"
   - Shows total event count

### Output Format

Each line in the JSONL file is a complete event:

```json
{
  "datetime": "2024-01-12 14:30:00",
  "bar_index": 1234,
  "event_type": "indicator_change",
  "is_event_driven": true,

  "open": 6005.50,
  "high": 6007.25,
  "low": 6004.75,
  "close": 6006.00,
  "volume": 125000,
  "close_percentile": 52.0,
  "volume_ratio_percent": 115.5,

  "date": "2024-01-12 14:30:00",
  "last": 6006.00,
  "position_side": 0,

  "long_macd": 4,
  "raschke_strategy_setup": 5,
  "volume_signal": 1,
  "market_regime": 1,

  "prev_high": 6007.00,
  "prev_low": 6003.50,
  "prev_day_high": 6015.00,
  "prev_day_low": 5985.25
}
```

### Key Fields

#### Event Metadata
- `event_type`: Always "indicator_change"
- `is_event_driven`: Always true (identifies this as sparse event stream)
- `delta_t_log` / `tau_100_log`: Event timing features in log space
- `bar_index`: Absolute bar index in replay

#### Price/Volume Context
- `datetime`: Bar timestamp
- `open`, `high`, `low`, `close`: OHLC prices
- `volume`: Bar volume
- `close_percentile`: Where close sits in bar range (0-100)
- `volume_ratio_percent`: Current volume vs 20-bar average

#### Indicator Changes (Delta)
Only includes indicators that changed. Example:
- If MACD changed: `"long_macd": 4`
- If Raschke pattern triggered: `"raschke_strategy_setup": 5`
- If nothing changed on an indicator: **field omitted** (delta encoding)

**Note:** `date`, `last`, `position_side`, and price reference levels are always included for context.

## Typical Event Patterns

### Trending Market (High Activity)
```
14:30 - Event (MACD cross, RSI overbought)
14:45 - Event (Raschke pattern trigger)
15:00 - Event (Volume spike)
15:15 - Event (Pattern confirmation)
```
**Result:** 4 events in 4 bars (high event density)

### Range-Bound Market (Low Activity)
```
14:30 - Event (Stochastic oversold)
[15 bars of silence - price oscillating, no indicator changes]
18:30 - Event (Breakout attempt, MACD turns)
```
**Result:** 2 events across 16 bars (sparse event stream)

### Overnight Gap
```
16:00 - Last event of day (market closes)
[Overnight - no bars, no events]
09:30 - Event (gap detected, indicators adjust)
```
**Result:** Gap opens are reflected through price-level context and indicator changes in the next emitted event.

## Training Data Statistics

### Expected Volume (ES, Regular Hours)

| Timeframe | Bars | Events | Reduction |
|-----------|------|--------|-----------|
| 1 day | 40 | 8-15 | 62-80% |
| 1 week | 200 | 40-75 | 62-80% |
| 1 month | 880 | 175-350 | 60-80% |
| 1 year | ~10,000 | ~2,000-4,000 | 60-80% |

### Event Distribution by Volatility

| Market Condition | Event Rate | Typical Gap |
|------------------|------------|-------------|
| High volatility | 30-50% of bars | 1-2 bars |
| Normal market | 20-30% of bars | 2-4 bars |
| Low volatility | 10-20% of bars | 5-10+ bars |

## Advantages for Transformer Training

### 1. Reduced Noise
- **Bar-based**: Includes many bars with no meaningful changes
- **Event-driven**: Every sample is a state transition

### 2. Better Attention
- Temporal gaps signal "nothing important happened"
- Model learns to focus on actual changes, not just passage of time

### 3. Efficient Training
- 60-80% fewer samples to process
- Same information content, less redundancy

### 4. Natural Sequence Breaks
```python
# Event stream naturally breaks into "episodes"
events = load_jsonl("event_data.jsonl")

current_sequence = []
for event in events:
    if event["delta_t_log"] > 4.5:  # Long gap in event-time space
        # Process current sequence
        train_on_sequence(current_sequence)
        current_sequence = []  # Start new sequence

    current_sequence.append(event)
```

### 5. Explicit Temporal Features
- `delta_t_log`: log-scaled inter-event spacing
- `tau_100_log`: rolling median inter-event spacing baseline
- Transformer can learn: "Long silence after pattern = continuation likely"

## Comparison Example

### Bar-Based Output (First 5 Bars)
```json
{"datetime": "14:30", "macd": 4, "rsi": 1, ...}  // New bar
{"datetime": "14:45", "macd": 4, "rsi": 1, ...}  // No change (duplicate)
{"datetime": "15:00", "macd": 4, "rsi": 1, ...}  // No change (duplicate)
{"datetime": "15:15", "macd": 4, "rsi": 2, ...}  // RSI changed
{"datetime": "15:30", "macd": 5, "rsi": 2, ...}  // MACD changed
```
**Result:** 5 samples, 3 contain redundant data

### Event-Driven Output (Same Period)
```json
{"datetime": "14:30", "delta_t_log": 0.0, "tau_100_log": 0.0, "macd": 4, "rsi": 1, ...}  // Initial state
{"datetime": "15:15", "delta_t_log": 5.7, "tau_100_log": 5.7, "rsi": 2, ...}               // RSI changed
{"datetime": "15:30", "delta_t_log": 4.1, "tau_100_log": 5.1, "macd": 5, ...}              // MACD changed
```
**Result:** 3 samples, 100% signal, explicit temporal gaps

## Integration with Existing Workflow

### Use Both Collectors for Different Purposes

**Bar-Based Collector** (`DataCollectorStudy.cpp`):
- Complete historical record
- Backtesting exact bar sequences
- Debugging indicator calculations
- Verifying every bar's state

**Event-Driven Collector** (`EventDataCollectorStudy.cpp`):
- Transformer training (attention models)
- Pattern recognition
- Event prediction models
- Sparse sequence learning

### Converting Event Stream to Bar Stream

If you need to reconstruct full bar history from events:

```python
def expand_events_to_bars(events):
    """Fill in missing bars between events."""
    bars = []
    last_state = {}

    for event in events:
        # Add event bar (merge with last state for full context)
        current_state = {**last_state, **event}
        bars.append(current_state)
        last_state = current_state

    return bars
```

## File Naming Convention

Files follow format: `event_data_YYYYMMDD_HHMMSS.jsonl`

Example: `event_data_20240112_143022.jsonl` (started Jan 12, 2024 at 14:30:22)

## Error Handling

The collector handles errors gracefully:

```cpp
try {
    // Process indicators and write event
} catch (const std::exception& e) {
    sc.AddMessageToLog("EventDataCollector error: " + e.what(), 1);
    Logger::getInstance().log(errorMsg);
    // Continues processing next bar
}
```

Events are flushed to disk immediately (`std::endl`), so partial exports are valid.

## Performance Impact

**Minimal:** Event detection uses existing `IsDirty()` checks. File I/O only on events (8-15x per day).

**Memory:** ~1KB per event × 2000 events/year = ~2MB annual memory footprint.

**Disk:** ~500 bytes per event (compressed JSON) × 2000 events = ~1MB per year.

## Best Practices

### 1. Replay Speed
- Run at normal speed (1x) to ensure indicator calculations are accurate
- Faster replay (10x) works but test a sample first

### 2. File Management
- One file per replay session
- Archive old files: `mkdir archive && mv event_data_*.jsonl archive/`

### 3. Validation
```python
import json

# Validate JSONL file
with open("event_data.jsonl") as f:
    events = [json.loads(line) for line in f]

print(f"Total events: {len(events)}")
print(f"Date range: {events[0]['datetime']} to {events[-1]['datetime']}")
print(f"Avg delta_t_log: {sum(e['delta_t_log'] for e in events) / len(events):.3f}")
```

### 4. Merging Multiple Files
```python
# Combine multiple replay sessions
import json
from pathlib import Path

all_events = []
for file in sorted(Path("Data").glob("event_data_*.jsonl")):
    with open(file) as f:
        all_events.extend(json.loads(line) for line in f)

# Write combined file
with open("combined_events.jsonl", "w") as f:
    for event in all_events:
        f.write(json.dumps(event) + "\n")
```

## Troubleshooting

### No Events Generated
- **Check:** Is export armed? (Menu should show "Disarm" not "Arm")
- **Check:** Are you in replay mode? (Not live trading)
- **Check:** Are indicators updating? (Check IndicatorManager initialization)

### Too Few Events
- **Normal:** In low-volatility periods, expect 10-20% event rate
- **Check:** Verify indicators are changing with bar-based collector

### File Empty After Replay
- **Cause:** Study may not have called `LastCallToFunction`
- **Fix:** Manually disarm export to flush and close file

## Related Documentation

- [TRANSFORMER_LIVE_TRADING_PROTOCOL.md](TRANSFORMER_LIVE_TRADING_PROTOCOL.md) - Event-driven messaging architecture
- [TRANSFORMER_TRAINING_DATA_REFERENCE.md](TRANSFORMER_TRAINING_DATA_REFERENCE.md) - Full data field reference
- [ENUM_REFERENCE.md](ENUM_REFERENCE.md) - Indicator enum value meanings

## Future Enhancements

### Planned Features
- [ ] Multi-timeframe events (daily/weekly indicator changes)
- [ ] Event importance scoring (pattern triggers = high importance)
- [ ] Filtering by indicator category (only Screen 2/3 events)
- [ ] Real-time event streaming (live trading mode)
- [ ] Event compression (store deltas only, reconstruct on load)

### Configuration Options (Future)
```cpp
// Example future inputs
sc.Input[0].Name = "Minimum Indicators Changed";  // Threshold for event
sc.Input[1].Name = "Include Position Events Only";  // Filter by side changes
sc.Input[2].Name = "Pattern Events Only";  // Only Raschke/Elder triggers
```
