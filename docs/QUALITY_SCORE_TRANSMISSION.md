# Pattern Quality Scores & Context Metrics - Phase 1

## Overview
Pattern indicators now transmit quality scores AND critical context metrics to the Python GUI for enhanced scoring multipliers per SCORING_ENHANCEMENT_PLAN.md.

## Implementation Status: Phase 1

### All Patterns - Quality Scores (Baseline)
All 5 pattern indicator classes in `include/Indicator.h` now override `AddToPayload()`:

1. **KangarooTail** - `kangaroo_tail_quality`
2. **TurtleSoup** - `turtle_soup_quality` + context
3. **MomentumPinball** - `momentum_pinball_quality` + context
4. **ElderBreakout** - `elder_breakout_quality` + context
5. **NR7** - `nr7_quality`

### Phase 1: Critical Context Metrics
Three patterns include context metrics needed for scoring multipliers:
- **TurtleSoup** - 4 context fields (daily levels, ADX, screen alignment)
- **ElderBreakout** - 3 context fields (compression, consolidation, ADX)
- **MomentumPinball** - 2 context fields (FI2 pullback, Impulse change)

---

## JSON Payload Structure

### Before (Missing Critical Data)
```json
{
  "turtle_soup": 3,              // BULLISH_EXTREME enum value
  "elder_breakout": 2            // BULLISH_STRONG enum value
}
```

### After Phase 1 (Quality + Context)
```json
{
  "turtle_soup": 3,
  "turtle_soup_quality": 0.82,
  "turtle_soup_context": {
    "at_daily_high": false,
    "at_daily_low": true,        // ×1.15 multiplier
    "adx": 18.5,                  // <25 = ×1.2 multiplier
    "screen_aligned": true        // ×1.1 multiplier
  },
  
  "elder_breakout": 2,
  "elder_breakout_quality": 0.74,
  "elder_breakout_context": {
    "channel_squeeze": true,      // ×1.25 multiplier
    "consolidation_bars": 7,      // ≥5 = ×1.15 multiplier
    "adx": 32.0,                  // >30 = ×1.1 multiplier
    "volume_spike": 2.3           // >1.5 = confirmation signal
  },
  
  "momentum_pinball": 2,
  "momentum_pinball_quality": 0.71,
  "momentum_pinball_context": {
    "fi2_pullback": true,          // ×1.2 multiplier (Elder timing)
    "impulse_changed": true        // ×1.15 multiplier (fresh signal)
  },
  
  "kangaroo_tail": 2,
  "kangaroo_tail_quality": 0.75,
  // No context in Phase 1
  
  "nr7": 3,
  "nr7_quality": 0.71
  // No context in Phase 1
}
```

## Quality Score Calculation

Quality scores are already computed in the study functions (e.g., `TripleScreen3.cpp`) using metrics like:

### KangarooTail Quality
- Tail-to-body ratio (2x = 0.5, 4x = 1.0)
- Tail-to-ATR ratio (0.5x ATR = 0.5, 1x ATR = 1.0)
- Close position in bar range (near extreme = higher)
- Context: at support/resistance levels

### TurtleSoup Quality
- Penetration distance beyond 4-day extreme
- Close-back distance inside range
- Close position (near opposite extreme = higher)
- Context: at daily high/low, ADX strength, screen alignment

### MomentumPinball Quality
- RSI delta magnitude (cross strength)
- Stochastic depth (how extreme)
- Fresh Impulse color change bonus
- Volume spike magnitude
- Context: FI2 pullback/rally, MACD-H direction, screen alignment

### ElderBreakout Quality
- Breakout distance beyond Keltner band
- ADX trend strength (>20 = trending, >30 = strong)
- Volume spike magnitude
- Consolidation bars before breakout
- Gap bonus
- Context: channel squeeze, Impulse alignment, screen alignment

### NR7 Quality
- Range compression (current range vs 7-bar average)
- Volume decline (compression signal)
- Consolidation duration
- Context: Impulse alignment, screen regime prediction

## Phase 1 Context Metrics Explained

### TurtleSoup Context (Stop Hunt Detection)
- **at_daily_high** / **at_daily_low** - Pattern at previous day extreme (×1.15)
- **adx** - Trend strength; <25 = ideal ranging for false breakouts (×1.2)
- **screen_aligned** - Multi-timeframe confirmation from Screen1 (×1.1)

### ElderBreakout Context (Volatility Expansion)
- **channel_squeeze** - Keltner bands narrowing = coiled spring (×1.25)
- **consolidation_bars** - Duration of compression; ≥5 bars ideal (×1.15)
- **adx** - Trend strength; >30 = strong directional move (×1.1)
- **volume_spike** - Volume relative to average; >1.5 = strong confirmation

### MomentumPinball Context (Elder Timing)
- **fi2_pullback** - FI2 shows pullback/rally = Elder's timing signal (×1.2)
- **impulse_changed** - Fresh Impulse color change = momentum shift (×1.15)

---

## Python GUI Integration

### Phase 1: Enhanced Scoring with Context
```python
def calculate_enhanced_score(pattern_data):
    """Apply scoring multipliers based on context"""
    base_quality = pattern_data.get('quality', 0.0)
    context = pattern_data.get('context', {})
    
    score = base_quality
    
    # TurtleSoup multipliers
    if context.get('at_daily_low') or context.get('at_daily_high'):
        score *= 1.15  # Daily extreme bonus
    
    if context.get('adx', 100) < 25:
        score *= 1.2   # Ranging market ideal for stop hunts
    
    if context.get('screen_aligned'):
        score *= 1.1   # Multi-timeframe confirmation
    
    # ElderBreakout multipliers
    if context.get('channel_squeeze'):
        score *= 1.25  # Compression bonus
    
    if context.get('consolidation_bars', 0) >= 5:
        score *= 1.15  # Coiling duration bonus
    
    if context.get('adx', 0) > 30:
        score *= 1.1   # Strong trend bonus
    
    if context.get('volume_spike', 0) > 1.5:
        score *= 1.1   # Volume confirmation bonus
    
    # MomentumPinball multipliers
    if context.get('fi2_pullback'):
        score *= 1.2   # Elder timing confirmation
    
    if context.get('impulse_changed'):
        score *= 1.15  # Fresh momentum shift
    
    return min(score, 1.0)  # Cap at 1.0

# Example usage
def process_indicator_payload(payload):
    # Process TurtleSoup
    if payload.get('turtle_soup') != 0:
        soup_data = {
            'quality': payload.get('turtle_soup_quality', 0.0),
            'context': payload.get('turtle_soup_context', {})
        }
        enhanced_score = calculate_enhanced_score(soup_data)
        
        if enhanced_score >= 0.85:
            generate_alert(f"EXCEPTIONAL Turtle Soup: {enhanced_score:.0%}", priority="HIGH")
    
    # Filter by quality
    if kangaroo_strength == 2 and kangaroo_quality >= 0.7:
        # BULLISH_STRONG with high quality - prioritize this signal
        generate_alert("High-quality Kangaroo Tail", priority="HIGH")
        
    turtle_strength = payload.get('turtle_soup', 0)
    turtle_quality = payload.get('turtle_soup_quality', 0.0)
    
    if turtle_strength == 3 and turtle_quality >= 0.8:
        # BULLISH_EXTREME with very high quality - excellent setup
        generate_alert("Exceptional Turtle Soup", priority="CRITICAL")
```

## Technical Details

### AddToPayload Implementation Pattern
```cpp
void KangarooTail::AddToPayload(nlohmann::json& payload) override {
    // First add the enum value (pattern strength)
    Indicator<KangarooTailEnum>::AddToPayload(payload);
    
    // Then add the quality score with "_quality" suffix
    payload[std::string(JsonKey()) + "_quality"] = m_qualityScore;
}
```

This pattern:
- Preserves backward compatibility (enum still transmitted)
- Adds quality as a new key with consistent naming convention
- Uses `JsonKey()` to get the indicator name ("kangaroo_tail", etc.)
- Appends "_quality" suffix for easy identification

### Quality Score Range
All quality scores are normalized to `0.0 - 1.0` range:
- `0.0` = No pattern or invalid pattern
- `0.3 - 0.5` = Weak pattern, marginal quality
- `0.5 - 0.7` = Moderate quality, decent setup
- `0.7 - 0.85` = High quality, strong setup
- `0.85 - 1.0` = Exceptional quality, textbook setup

## Benefits

1. **Precision**: Distinguishes between BULLISH_STRONG at 0.65 quality vs 0.88 quality
2. **Risk Management**: Filter out low-quality noise patterns
3. **Performance Tracking**: Analyze quality score vs trade outcome correlation
4. **Machine Learning**: Quality scores as features for predictive models
5. **Trade Optimization**: Better entry selection when multiple patterns present

### Phase 1 Testing Checklist
- [ ] Verify all 5 patterns transmit quality scores (0.0-1.0 range)
- [ ] Verify TurtleSoup context present when pattern active
- [ ] Verify ElderBreakout context present when pattern active
- [ ] Verify MomentumPinball context present when pattern active
- [ ] Test at_daily_low bonus triggers correctly (×1.15)
- [ ] Test ADX < 25 bonus for TurtleSoup (×1.2)
- [ ] Test channel_squeeze bonus for ElderBreakout (×1.25)
- [ ] Test consolidation_bars ≥ 5 bonus (×1.15)
- [ ] Test volume_spike > 1.5 confirmation (×1.1)
- [ ] Test fi2_pullback bonus (×1.2)
- [ ] Test impulse_changed bonus (×1.15)
- [ ] Validate enhanced scores match expected multipliers

---

## Future Phases (Not Yet Implemented)

### Phase 2: Additional Context Metrics
After Phase 1 validates successfully, add:
- **KangarooTail**: tail_to_body_ratio, tail_to_atr, close_position, at_support/resistance
- **TurtleSoup**: penetration_distance, close_distance, close_position
- **MomentumPinball**: rsi_delta, stoch_depth, volume_spike, macd_h_rising
- **ElderBreakout**: breakout_distance, volume_spike, is_gap, impulse_aligned
- **NR7**: current_range, avg_7bar_range, range_percentile, volume metrics

### Phase 3: MACD Values
- Transmit actual MACD-Histogram values for GUI charting
- Enable divergence validation without separate data feed

---

## Related Files
- `include/Indicator.h` (lines 1245-1504) - AddToPayload implementations
- `src/TripleScreen3.cpp` - Pattern detection and quality calculation
- `src/StudyHelperFunctions.cpp` - Pattern detection helper functions
- `include/IndicatorManager.h` - Payload transmission to Python GUI

## Build & Deploy
```bash
cd /home/rcruz/devel/VSCode/MindfulTrader
mkdir -p build && cd build
cmake -S .. -B . -DCMAKE_BUILD_TYPE=Release
cmake --build . -- -j$(nproc)
./deploy_mindfultrader.sh
```
