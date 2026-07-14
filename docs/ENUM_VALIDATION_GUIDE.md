# Enum Validation Guide

## Purpose

This guide ensures that C++ enum values exported to training data match the Python enum mappings used in analysis scripts. Mismatches cause incorrect pattern distribution reports and can corrupt ML training data.

## Critical Issue Background

**Problem Discovered (2024-12-29)**: The `analyze_transformer_data.py` script had completely wrong enum mappings:
- Value 4 mapped to "KANGAROO_TAIL" (actually "IDNR4")
- Value 7 mapped to "TWO_B" (actually "WHIPLASH")
- Value 8 mapped to "HOOK" (actually "GHOST")
- ...and 15+ other mismatches

**Impact**: All previous training data analyses reported incorrect pattern distributions, making it impossible to understand which strategies were actually being detected.

## The Three Components

### 1. C++ Enum Definitions (Source of Truth)

**File**: [include/Indicator.h](../include/Indicator.h)

```cpp
enum class RaschkeStrategySetup : int {
    NONE = 0,
    THREE_BAR_TRIANGLE = 1,
    NR4 = 2,
    NR7 = 3,
    IDNR4 = 4,
    WHIPLASH = 7,
    GHOST = 8,
    // ... etc
};
```

These are the **authoritative** enum definitions. All Python mappings must match these exactly.

### 2. C++ Data Export

**File**: [src/DataCollectorStudy.cpp](../src/DataCollectorStudy.cpp)

Key code sections:
```cpp
// Get enum values from indicators
auto raschkeStrategy = IndicatorManager::Instance()
    .GetIndicator<RaschkeStrategyIndicator>(IndicatorKeys::RASCHKE_STRATEGY_SETUP);
RaschkeStrategySetup strategySetup = raschkeStrategy ? 
    raschkeStrategy->Value() : RaschkeStrategySetup::NONE;

// Export to JSON (implicit cast to int)
payload["raschke_strategy_setup"] = strategySetup;  // Exports enum's int value
payload["raschke_tactical_trigger"] = tacticalTrigger;
payload["market_regime"] = marketRegimeValue;
```

**Critical**: The JSON export automatically casts enum class values to their underlying `int` type.

### 3. Python Enum Mappings (Must Match C++)

**File**: [analyze_transformer_data.py](../analyze_transformer_data.py)

```python
strategy_names = {
    0: "NONE",
    1: "THREE_BAR_TRIANGLE",
    2: "NR4",
    3: "NR7",
    4: "IDNR4",  # MUST match C++ enum value 4
    7: "WHIPLASH",
    8: "GHOST",
    # ... etc
}
```

**Critical**: These dictionaries map integer enum values to human-readable names for analysis reports.

## Validation Workflow

### Automated Validation Script

**Tool**: [validate_enum_mappings.py](../validate_enum_mappings.py)

This script:
1. **Parses C++ enums** directly from `include/Indicator.h`
2. **Extracts enum values** and names automatically
3. **Displays correct mappings** for copy-paste into Python scripts
4. **Can be extended** to validate actual Python file content

**Usage**:
```bash
# Run validation (use mamba 'mts' environment)
mamba run -n mts python validate_enum_mappings.py
```

**Expected Output**:
```
================================================================================
ENUM MAPPING VALIDATION
================================================================================

📋 Validating: RaschkeStrategySetup (Python: strategy_names)
   ✓ Parsed 19 values from C++ enum

   Correct Python mapping for 'strategy_names':
{
            0: "NONE",
            1: "THREE_BAR_TRIANGLE",
            2: "NR4",
            3: "NR7",
            4: "IDNR4",
            7: "WHIPLASH",
            8: "GHOST",
            9: "TWO_B_REVERSAL",
            ...
        }
```

### Manual Validation Checklist

When modifying enums, follow this process:

#### Step 1: Modify C++ Enum (if needed)
```bash
# Edit include/Indicator.h
# Add/modify enum values in:
#   - RaschkeStrategySetup
#   - RaschkeTacticalTrigger
#   - MarketRegimeEnum
```

#### Step 2: Run Validation Script
```bash
mamba run -n mts python validate_enum_mappings.py
```

#### Step 3: Update Python Mappings
Copy the correct mapping from validation output into `analyze_transformer_data.py`:

```python
def analyze_strategies_and_triggers(self) -> Dict[str, Any]:
    strategy_counts = Counter([r.get('raschke_strategy_setup', 0) for r in self.records])
    trigger_counts = Counter([r.get('raschke_tactical_trigger', 0) for r in self.records])
    
    # COPY MAPPING FROM validate_enum_mappings.py OUTPUT
    strategy_names = {
        0: "NONE",
        1: "THREE_BAR_TRIANGLE",
        2: "NR4",
        # ... paste complete mapping here
    }
```

#### Step 4: Rebuild C++ Studies
```bash
cd build-windows
cmake --build . -- -j$(nproc)
```

#### Step 5: Regenerate Training Data
```bash
# In Sierra Chart:
# 1. Delete old data/TransformerData.jsonl
# 2. Run DataCollectorStudy in replay mode
# 3. Wait for export to complete
```

#### Step 6: Verify with Analysis
```bash
mamba run -n mts python analyze_transformer_data.py
```

Check that pattern distributions make sense:
- HOLY_GRAIL_CONTINUATION should be common (~30-40%)
- IDNR4 should show up (~10-15%)
- GHOST patterns should be present (~8%)
- No "Unknown_X" patterns should appear

## Enum Reference

### RaschkeStrategySetup (Screen 2 - Strategic Setups)

| Value | C++ Name | Python Name | Description |
|-------|----------|-------------|-------------|
| 0 | NONE | NONE | No pattern detected |
| 1 | THREE_BAR_TRIANGLE | THREE_BAR_TRIANGLE | Consolidation triangle |
| 2 | NR4 | NR4 | Narrow range 4 bar |
| 3 | NR7 | NR7 | Narrow range 7 bar |
| 4 | IDNR4 | IDNR4 | Inside day NR4 |
| 7 | WHIPLASH | WHIPLASH | Failed breakout reversal |
| 8 | GHOST | GHOST | Gap + narrow range |
| 9 | TWO_B_REVERSAL | TWO_B_REVERSAL | Double top/bottom |
| 10 | ANTI | ANTI | Countertrend setup |
| 12 | HOLY_GRAIL_CONTINUATION | HOLY_GRAIL_CONTINUATION | Strong trend continuation |
| 13 | HOLY_GRAIL_BUY | HOLY_GRAIL_BUY | Pullback in uptrend |
| 14 | HOLY_GRAIL_SELL | HOLY_GRAIL_SELL | Pullback in downtrend |
| 15 | SLINGSHOT | SLINGSHOT | Deep pullback to EMA |
| 16 | FIRST_CROSS | FIRST_CROSS | EMA crossover |
| 17 | BREAD_AND_BUTTER | BREAD_AND_BUTTER | Classic trend trade |
| 18 | DOUBLE_REPO | DOUBLE_REPO | Reversal pattern |
| 19 | DOUBLE_REPO_FAILURE | DOUBLE_REPO_FAILURE | Failed reversal (continuation) |
| 20 | FLIP | FLIP | Momentum shift |
| 21 | NR4_NR7_VOLUME_SPIKE | NR4_NR7_VOLUME_SPIKE | Compression + volume |

**Note**: Values 5, 6, 11 are intentionally skipped in the enum.

### RaschkeTacticalTrigger (Screen 3 - Entry Timing)

| Value | C++ Name | Python Name | Description |
|-------|----------|-------------|-------------|
| 0 | NONE | NONE | No trigger |
| 1 | KANGAROO_TAIL_BUY | KANGAROO_TAIL_BUY | Bullish rejection |
| 2 | KANGAROO_TAIL_SELL | KANGAROO_TAIL_SELL | Bearish rejection |
| 3 | TURTLE_SOUP_BUY | TURTLE_SOUP_BUY | Failed breakdown |
| 4 | TURTLE_SOUP_SELL | TURTLE_SOUP_SELL | Failed breakout |
| 5 | MOMENTUM_PINBALL_BUY | MOMENTUM_PINBALL_BUY | Bounce off EMA |
| 6 | MOMENTUM_PINBALL_SELL | MOMENTUM_PINBALL_SELL | Rejection from EMA |
| 7 | ELDER_BREAKOUT_BUY | ELDER_BREAKOUT_BUY | Bullish channel breakout |
| 8 | ELDER_BREAKOUT_SELL | ELDER_BREAKOUT_SELL | Bearish channel breakout |
| 9 | NR7_BREAKOUT_BUY | NR7_BREAKOUT_BUY | Bullish compression breakout |
| 10 | NR7_BREAKOUT_SELL | NR7_BREAKOUT_SELL | Bearish compression breakout |
| 11 | ITR_BREAKOUT_BUY | ITR_BREAKOUT_BUY | Above opening range |
| 12 | ITR_BREAKOUT_SELL | ITR_BREAKOUT_SELL | Below opening range |
| 13 | ITR_FADE_BUY | ITR_FADE_BUY | Failed breakdown (range) |
| 14 | ITR_FADE_SELL | ITR_FADE_SELL | Failed breakout (range) |
| 15 | RSI_FAILURE_SWING_BUY | RSI_FAILURE_SWING_BUY | RSI divergence buy |
| 16 | RSI_FAILURE_SWING_SELL | RSI_FAILURE_SWING_SELL | RSI divergence sell |
| 17 | STOCHASTIC_POP_BUY | STOCHASTIC_POP_BUY | Oversold reversal |
| 18 | STOCHASTIC_POP_SELL | STOCHASTIC_POP_SELL | Overbought reversal |

### MarketRegimeEnum (Screen 1 - Market Tide)

| Value | C++ Name | Python Name | Description |
|-------|----------|-------------|-------------|
| -1 | UNDEFINED | UNDEFINED | Insufficient data |
| 0 | TRENDING_UP | TRENDING_UP | Strong uptrend |
| 1 | TRENDING_DOWN | TRENDING_DOWN | Strong downtrend |
| 2 | TRENDING_IMPULSE | TRENDING_IMPULSE | Impulse move |
| 3 | RANGE_DAY | RANGE_DAY | Bounded range |
| 4 | CONSOLIDATING_CHOP | CONSOLIDATING_CHOP | Sideways chop |
| 5 | EXTREME_DISLOCATION | EXTREME_DISLOCATION | Sentiment extreme |

## Common Pitfalls

### ❌ Wrong: Hardcoded Integer Values
```cpp
// NEVER do this - enum values can change
if (strategy == 4) { ... }  // What is 4?
```

### ✅ Correct: Named Enum Values
```cpp
// Always use named values
if (strategy == RaschkeStrategySetup::IDNR4) { ... }
```

### ❌ Wrong: Assuming Sequential Values
```python
# Enum values have gaps (5, 6, 11 are missing)
strategy_names = {i: name for i, name in enumerate(names)}  # WRONG
```

### ✅ Correct: Explicit Value Mapping
```python
# Map exact integer values
strategy_names = {
    0: "NONE",
    1: "THREE_BAR_TRIANGLE",
    # ... explicitly map each value
}
```

### ❌ Wrong: Manual Enum String Construction
```python
# Don't manually construct enum strings
name = f"STRATEGY_{value}"  # WRONG - won't match C++
```

### ✅ Correct: Use Validation Script
```bash
# Generate correct mapping automatically
mamba run -n mts python validate_enum_mappings.py
# Copy output into analyze_transformer_data.py
```

## Integration with CI/CD

To prevent enum mapping bugs in the future, add validation to your build process:

### Pre-commit Hook (Optional)
```bash
#!/bin/bash
# .git/hooks/pre-commit

# Check if Indicator.h was modified
if git diff --cached --name-only | grep -q "include/Indicator.h"; then
    echo "Indicator.h modified - validating enum mappings..."
    mamba run -n mts python validate_enum_mappings.py
    if [ $? -ne 0 ]; then
        echo "❌ Enum validation failed!"
        echo "Run: mamba run -n mts python validate_enum_mappings.py"
        echo "Then update analyze_transformer_data.py with correct mappings"
        exit 1
    fi
fi
```

### Build-time Validation
Add to your CMakeLists.txt:
```cmake
# Optional: Run enum validation during build
add_custom_target(validate_enums
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/validate_enum_mappings.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    COMMENT "Validating enum mappings..."
)
add_dependencies(MindfulTrader validate_enums)
```

## Troubleshooting

### Problem: "Unknown_X" patterns in analysis output
**Cause**: Python mapping missing enum value  
**Fix**: Run `validate_enum_mappings.py` and copy missing values

### Problem: Pattern counts don't match expectations
**Cause**: Wrong enum mapping (e.g., IDNR4 reported as KANGAROO_TAIL)  
**Fix**: Regenerate entire Python mapping from validation script

### Problem: Validation script fails to parse enum
**Cause**: Complex C++ syntax (macros, comments, etc.)  
**Fix**: Check Indicator.h formatting, ensure standard `enum class Name : int { }` syntax

### Problem: Analysis shows 0% for all patterns
**Cause**: Training data not regenerated after enum changes  
**Fix**: Delete TransformerData.jsonl, re-export in Sierra Chart

## Summary

**Golden Rule**: `include/Indicator.h` is the source of truth. Always validate Python mappings against it using `validate_enum_mappings.py`.

**When to Validate**:
1. After modifying any enum in Indicator.h
2. Before analyzing training data
3. When pattern distributions look suspicious
4. When adding new pattern detection logic

**Quick Validation Command**:
```bash
mamba run -n mts python validate_enum_mappings.py && \
mamba run -n mts python analyze_transformer_data.py | head -100
```

This ensures your enum mappings are correct and your analysis reflects reality.
