# Gemini CLI Verification & Audit Report

**Date:** Tuesday, July 14, 2026  
**Auditor:** Gemini CLI (Default Mode)  
**Target Repository:** MindfulTrader (C++ Sierra Chart DLL)  
**Subject:** Triple-Barrier Exit Engine Spec Verification, Composition Fork Ruling, and Finding-17 Enum Collision Confirmation.

---

## Executive Summary

As requested, Gemini CLI has completed an end-to-end audit of the `triple_barrier_exit_engine_spec.md` against the compiled C++ source code in `PositionManagerPatterns.cpp` and `Indicator.h`. 

All findings are backed by empirical evidence (exact source line ranges and numeric identifiers) and are documented below to facilitate immediate, seamless collaboration with Claude Code.

---

## Task 1 — Spec Table Verification Against Real Source

We audited the reconciled 9-pattern parameter table (§5.3 of the spec) against the compiled C++ logic in `PositionManagerPatterns.cpp` for the three **HIGH-confidence** patterns:

### 1. Turtle Soup (BUY/SELL) — **CONFIRMED**
* **Spec Claim:** Stop = `low ∓ 0.5×ATR`, Target = 4-bar high/low extreme.
* **C++ Implementation:** Matches the spec precisely. The stop uses `TURTLE_SOUP_STOP_BUFFER = 0.5f` multiplied by the ATR, and the target utilizes a 4-bar lookback (`TURTLE_SOUP_LOOKBACK = 4`) to locate the structural high/low extreme using the STL's `std::max_element` / `std::min_element`.
* **C++ Source Code Reference:** `/home/rcruz/devel/VSCode/MindfulTrader/src/PositionManagerPatterns.cpp` (lines 189, 191, 198–221)

```cpp
    constexpr float TURTLE_SOUP_STOP_BUFFER = 0.5f;
    constexpr int TURTLE_SOUP_LOOKBACK = 4;
    // ...
        case RaschkeTacticalTrigger::TURTLE_SOUP_BUY:
        {
            entryPrice = close;
            stopPrice = low - (TURTLE_SOUP_STOP_BUFFER * atr);

            const int lookbackStart = std::max(0, idx - TURTLE_SOUP_LOOKBACK);
            targetPrice = *std::max_element(&sc.High[lookbackStart], &sc.High[idx + 1]);
            return true;
        }
        case RaschkeTacticalTrigger::TURTLE_SOUP_SELL:
        {
            entryPrice = close;
            stopPrice = high + (TURTLE_SOUP_STOP_BUFFER * atr);

            const int lookbackStart = std::max(0, idx - TURTLE_SOUP_LOOKBACK);
            targetPrice = *std::min_element(&sc.Low[lookbackStart], &sc.Low[idx + 1]);
            return true;
        }
```

### 2. Momentum Pinball (BUY/SELL) — **CONFIRMED**
* **Spec Claim:** Stop = `low ∓ 0.4×ATR`, Target = 10-bar high/low extreme.
* **C++ Implementation:** Matches the spec precisely. The stop uses `PINBALL_STOP_MULTIPLIER = 0.4f` multiplied by the ATR, and the target utilizes a 10-bar lookback (`PINBALL_SWING_LOOKBACK = 10`) to find the structural swing high/low.
* **C++ Source Code Reference:** `/home/rcruz/devel/VSCode/MindfulTrader/src/PositionManagerPatterns.cpp` (lines 190, 192, 223–245)

```cpp
    constexpr float PINBALL_STOP_MULTIPLIER = 0.4f;
    constexpr int PINBALL_SWING_LOOKBACK = 10;
    // ...
        case RaschkeTacticalTrigger::MOMENTUM_PINBALL_BUY:
        {
            entryPrice = close;
            stopPrice = low - (PINBALL_STOP_MULTIPLIER * atr);

            const int lookbackStart = std::max(0, idx - PINBALL_SWING_LOOKBACK);
            targetPrice = *std::max_element(&sc.High[lookbackStart], &sc.High[idx + 1]);
            return true;
        }
        case RaschkeTacticalTrigger::MOMENTUM_PINBALL_SELL:
        {
            entryPrice = close;
            stopPrice = high + (PINBALL_STOP_MULTIPLIER * atr);

            const int lookbackStart = std::max(0, idx - PINBALL_SWING_LOOKBACK);
            targetPrice = *std::min_element(&sc.Low[lookbackStart], &sc.Low[idx + 1]);
            return true;
        }
```

### 3. Elder Breakout (BUY/SELL) — **MISMATCH FLAGGED**
* **Stop (CONFIRMED):** The C++ code uses a 2-bar structural stop `std::min(low, sc.Low[idx - 1]) - sc.TickSize` (BUY) and `std::max(high, sc.High[idx - 1]) + sc.TickSize` (SELL) instead of an ATR stop. This matches the corrected spec description.
* **Target (MISMATCH):** The spec claims the target is "now locked to 1.5R" in favor of the single-stage bracket (retiring the historical 2.0R target). However, **the live C++ code is still hardcoded to a 2.0R multiplier** via `constexpr float ELDER_TARGET_R_MULTIPLE = 2.0f;` at lines 253 & 274.
* **C++ Source Code Reference:** `/home/rcruz/devel/VSCode/MindfulTrader/src/PositionManagerPatterns.cpp` (lines 247–285)

```cpp
        case RaschkeTacticalTrigger::ELDER_BREAKOUT_BUY:
        {
            if (idx < 1) [[unlikely]] return false;
            constexpr float ELDER_TARGET_R_MULTIPLE = 2.0f; // <--- MISMATCH: Still 2.0f (Spec claims locked to 1.5R)
            const float prevHigh = sc.High[idx - 1];
            entryPrice = (high > prevHigh) ? close : prevHigh + sc.TickSize;
            stopPrice = std::min(low, sc.Low[idx - 1]) - sc.TickSize;

            const float riskAmount = entryPrice - stopPrice;
            targetPrice = entryPrice + (ELDER_TARGET_R_MULTIPLE * riskAmount);
            return true;
        }
```

---

## Task 2 — Ruling on the Composition Fork

### Ruling: Reading B Governs

We rule in favor of **Reading B**: the dedicated formula (direct structural N-bar target) governs for HIGH-confidence patterns; the volatility-derived target candidate cap of §4.2 must be bypassed or ignored for these setups.

### Rationale & Quantitative Edge
1. **Mathematical Reality of Code:** The current C++ implementation of `CalculateTacticalTriggerPrices` for Turtle Soup and Momentum Pinball does not compute a volatility-derived target candidate (`target_r_mult * risk`) nor perform a `std::min`/`std::max` nearest-neighbor comparison. It assigns the structural N-bar extreme to `targetPrice` directly.
2. **Expectancy & R:R Preservation (The Quant Edge):**
   * Turtle Soup and Momentum Pinball are **mean-reversion fade patterns**. They seek to exploit exhausted momentum or failed breakouts at trading range boundaries.
   * Entry occurs with extremely tight, volatility-scaled stops (`0.4×ATR` or `0.5×ATR` relative to the local bar extreme). Consequently, the initial risk $R = |entry - stop|$ is very small.
   * If **Reading A** were applied using the spec's default `target_r_mult = 1.0` for Turtle Soup, the volatility target candidate would be set just $1.0R$ away (i.e. `~0.4-0.5×ATR`). Because the 4-bar or 10-bar opposing structural extreme is almost always significantly further away than a tight $1.0R$, the generic "nearest of" cap rule in §4.2 would force the target down to $1.0R$.
   * Exiting a high-probability mean-reversion fade at $1.0R$ on noise cuts off the distribution's tail, preventing the capture of the structural reversion back across the range. This destroys the positive mathematical expectancy of the setup.
   * **Conclusion:** For Turtle Soup and Momentum Pinball, the target must remain the raw structural extreme (Reading B). The `target_r_mult` parameter in the spec table is only an abstract representation for defaulted/low-confidence patterns lacking dedicated formulas.

---

## Task 3 — Confirmation of Finding-17 Enum Collision

### Finding: Collision Confirmed

We confirmed the absolute enum-collision vulnerability described in Finding 17. The C++ compiler cannot prevent these types of logical bugs because both enums are defined as standard, distinct `enum class` types underlyingly represented by `int8_t`, and the code uses raw `static_cast` inside the position manager without validating the enum boundary namespaces.

### The Specific Collision: `ITR_FADE` ↔ `HOLY_GRAIL`
* In `RaschkeTacticalTrigger`, the values `ITR_FADE_BUY` and `ITR_FADE_SELL` are defined as **13** and **14**.
* In `RaschkeStrategySetup`, the values `HOLY_GRAIL_BUY` and `HOLY_GRAIL_SELL` are also defined as **13** and **14**.
* When `CalculateStrategySetupPrices` is called with an `ITR_FADE` tactical trigger ID (which is `13` or `14`), the code blindly performs:
  `const auto setup = static_cast<RaschkeStrategySetup>(patternId);`
  This evaluates directly to `RaschkeStrategySetup::HOLY_GRAIL_BUY` (13) or `RaschkeStrategySetup::HOLY_GRAIL_SELL` (14).
* As a result, an intraday mean-reversion range-fade (`ITR_FADE`) will inherit the pullback-to-EMA trend-continuation logic of the `HOLY_GRAIL` setup, computing a target with a `+1.0×ATR` trend extension (which is philosophically inverted and highly dangerous).

---

### Exact Quoted Lines

**RaschkeTacticalTrigger (with explicit underlying values):**
*File: `/home/rcruz/devel/VSCode/MindfulTrader/include/Indicator.h:88–110`*
```cpp
enum class RaschkeTacticalTrigger : int8_t
{
    NONE = 0,
    KANGAROO_TAIL_BUY = 1,
    KANGAROO_TAIL_SELL = 2,
    TURTLE_SOUP_BUY = 3,
    TURTLE_SOUP_SELL = 4,
    MOMENTUM_PINBALL_BUY = 5,
    MOMENTUM_PINBALL_SELL = 6,
    ELDER_BREAKOUT_BUY = 7,
    ELDER_BREAKOUT_SELL = 8,
    NR7_BREAKOUT_BUY = 9,
    NR7_BREAKOUT_SELL = 10,
    ITR_BREAKOUT_BUY = 11,
    ITR_BREAKOUT_SELL = 12,
    ITR_FADE_BUY = 13, // <--- Value 13
    ITR_FADE_SELL = 14, // <--- Value 14
    RSI_FAILURE_SWING_BUY = 15,
    RSI_FAILURE_SWING_SELL = 16,
    STOCHASTIC_POP_BUY = 17,
    STOCHASTIC_POP_SELL = 18
};
```

**RaschkeStrategySetup (with explicit underlying values):**
*File: `/home/rcruz/devel/VSCode/MindfulTrader/include/Indicator.h:123–145`*
```cpp
enum class RaschkeStrategySetup : int8_t
{
    NONE = 0,
    THREE_BAR_TRIANGLE = 1,
    NR4 = 2,
    NR7 = 3,
    IDNR4 = 4,
    WHIPLASH = 7,
    GHOST = 8,
    TWO_B_REVERSAL = 9,
    ANTI = 10,
    HOLY_GRAIL_CONTINUATION = 12,
    HOLY_GRAIL_BUY = 13, // <--- Value 13 (COLLIDES WITH ITR_FADE_BUY)
    HOLY_GRAIL_SELL = 14, // <--- Value 14 (COLLIDES WITH ITR_FADE_SELL)
    SLINGSHOT = 15,
    FIRST_CROSS = 16,
    BREAD_AND_BUTTER = 17,
    DOUBLE_REPO = 18,
    DOUBLE_REPO_FAILURE = 19,
    FLIP = 20,
    NR4_NR7_VOLUME_SPIKE = 21
};
```

---

## Action Items for Claude Code & Next Phase

1. **Update Elder Breakout Target Multiple:** Modify `PositionManagerPatterns.cpp:253` and `:274` from `2.0f` to `1.5f` to align with the Phase 1 target-locking specification.
2. **Defend Against Finding 17 Collision:** In both `CalculateTacticalTriggerPrices` and `CalculateStrategySetupPrices`, avoid blind type-casting. Implement explicit namespace boundary assertions or routing checks to ensure that tactical trigger IDs never slip into the strategy setup switch block.
3. **Preserve Reading B Geometry:** Ensure that when building `TripleBarrierExitManager`'s 4-step protocol, the raw structural extremes are assigned directly for high-confidence patterns (Turtle Soup / Momentum Pinball) without being truncated by the generic volatility R-multiple candidate.

---
*Report compiled and saved securely at `/home/rcruz/devel/VSCode/MindfulTrader/docs/ADR/gemini_verification_findings.md`.*
