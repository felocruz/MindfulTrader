---
domain: sierra_chart/screen3_patterns
intent: ELDER_BREAKOUT (IndicatorKey=24) — Keltner Channel breakout detector in TripleScreen3.cpp; close beyond band after consolidation, Hurst-filtered, quality-scored; flagged for replacement with ELDER_FAKE_BREAKOUT
scope: global
tags: [elder, screen3, keltner, channel-breakout, consolidation, hurst, TS3, ELDER_BREAKOUT, replacement-candidate]
source_files:
  - src/TripleScreen3.cpp
  - include/Indicator.h
last_verified: 2026-06-27
dependencies: [elder_triple_screen]
---

# ELDER_BREAKOUT — Keltner Channel Breakout (Replacement Candidate)

## Why This Exists
`ELDER_BREAKOUT = 24` in `IndicatorKey` is implemented in `TripleScreen3.cpp` as a
**Keltner Channel breakout detector** — price closing beyond the Keltner bands after a
consolidation period. It is not the Triple Screen trailing-stop entry described in Elder's
books, and it is not Elder's named False Breakout with Divergence pattern.

**Design intent vs. actual implementation:** The knowledge base previously documented
this as the Triple Screen Third Screen trailing-stop mechanic. Code reading shows the
actual implementation is a channel expansion signal. The pattern is under review for
replacement with `ELDER_FAKE_BREAKOUT` (Elder's False Breakout with Divergence).

---

## The Invariant / Contract

**Signal condition:** Close crosses beyond the Keltner Channel upper or lower band after
3+ of the prior 5 bars consolidated near that same band. Filtered by Hurst exponent for
trend persistence, volume confirmation, and gap detection.

---

## How It Works

### Keltner Channel Parameters (`TripleScreen3.cpp`)
```
Center EMA:    10-period EMA of close (configurable; default = 10)
ATR:           ATR(10) Wilder's smoothing  (hardcoded)
Band width:    EMA ± (ATR × 2.0)           (symmetric multiplier, configurable)
ATR baseline:  20-period SMA of ATR(10)    (regime reference)
```

### Consolidation Detection (lines 943–967)
For the prior `ELDER_CONSOLIDATION_LOOKBACK = 5` bars, count bars where:
```
|close[i] - TopBand[i]| ≤ ATR[i]      → nearUpperCount
|close[i] - BottomBand[i]| ≤ ATR[i]   → nearLowerCount
```
If `nearUpperCount ≥ ELDER_MIN_CONSOLIDATION_BARS (3)` → consolidation at resistance.
If `nearLowerCount ≥ 3` → consolidation at support.

### Breakout Detection
```
Bullish: close > TopBand    after consolidation near TopBand
Bearish: close < BottomBand after consolidation near BottomBand
Gap:     open already beyond band (isGap = true → quality boost)
```
`DetectElderBreakout()` (in `StudyHelperFunctions.cpp`) produces `ElderBreakoutEnum`:
`BULLISH_WEAK`, `BULLISH_STRONG`, `BULLISH_EXTREME`, and their bearish mirrors.

### Hurst Exponent Alignment Filter (lines 999–1023)
Applied after detection to adjust quality score:
```
Hurst ≥ 0.65 AND slope aligned with breakout direction → quality × 1.12 (boost)
Hurst ≥ 0.70 AND slope misaligned                     → quality × 0.70 (penalize)
Hurst < 0.55                                           → quality × 0.80 (penalize)
```
Trend-persistent breakouts are boosted; mean-reversion breakouts are penalized.

### Context Gates (lines 1039–1094)
Three boolean flags computed and passed to `SetContext()`:

| Flag | Condition |
|---|---|
| `channelSqueeze` | ATR(10) < 5-bar average ATR × 0.9 (ATR declined 10%+) |
| `impulseAligned` | `INTERM_IMP` is GREEN for bullish / RED for bearish breakout |
| `screenAligned` | HMM state is `GAUSSIAN_STABLE` or `PARETO_MOMENTUM` |

These flags are data-only — the Orchestrator/Python layer applies scoring weights.
Climate filtering was removed; Python is the decision authority.

### Forwarding to RaschkeTacticalTrigger (lines 1099–1109)
STRONG and EXTREME breakouts are forwarded unconditionally:
```
BULLISH_STRONG / BULLISH_EXTREME → RaschkeTacticalTrigger::ELDER_BREAKOUT_BUY
BEARISH_STRONG / BEARISH_EXTREME → RaschkeTacticalTrigger::ELDER_BREAKOUT_SELL
```

### Visualization
Quality ≥ 0.5: STRONG/EXTREME breakouts mark `Subgraph_ElderBreakoutBullish/Bearish`
at the breakout close price.

---

## Cross-Timeframe Access from TS3

TS3 accesses these Screen 1/2 indicators via `IndicatorManager::Instance()`:

| IndicatorKey | Timeframe | Used for |
|---|---|---|
| `LONG_MACD` | TS1 (Screen 1) | Coherence score (line 639) |
| `INTERM_MACD` | TS2 (Screen 2) | Coherence score (line 640) |
| `INTERM_IMP` | TS2 (Screen 2) | Impulse alignment gate (lines 824, 1056) |

`INTERM_MACD_DIVERGENCE` is **not currently accessed** here — it would be the arming
condition for the planned `ELDER_FAKE_BREAKOUT` replacement.

---

## Philosophical Conflict with Elder

Elder's explicit stated rule: *"Never buy above the upper channel line."* He treats
price exiting the envelope/channel bands as a mean-reversion sell signal (profit target
for longs, secondary short entry in downtrends), not a buy entry. The current
`ELDER_BREAKOUT` implementation uses the same geometry in the opposite direction.

The implementation is a legitimate volatility-expansion signal — the logic is internally
consistent — but it is not derived from Elder's published methodology and should not be
cited as such.

---

## Planned Replacement: ELDER_FAKE_BREAKOUT

The architectural decision is to replace the current Keltner Channel breakout with
**Elder's False Breakout with Divergence** — the only named breakout pattern in Elder's
actual work. The replacement would use:

- **Arming condition:** `INTERM_MACD_DIVERGENCE` at `BEARISH_DIVERGENCE_SELL_SIGNAL`
  (or `BULLISH_DIVERGENCE_BUY_SIGNAL`) — already computed on TS2, accessible via
  `indMgr.GetIndicator<MACDDivergence>(IndicatorKey::INTERM_MACD_DIVERGENCE)`
- **Trigger:** Price on TS3 reverses back inside the 20-bar extreme that was violated —
  `Subgraph_HighestHigh20Period` / `Subgraph_LowestLow20Period` already computed here
- **Stop:** Chandelier (existing system — no change)

---

## Failure Modes

**FM-01: Breakout into Shannon Chaos regime** — The NR7 detector in this same file
blocks on `SHANNON_CHAOS`. The Elder Breakout `allowBreakout` flag is always `true`
(climate filter removed). A false breakout on a random-walk bar has no trend to sustain
it. Orchestrator/Python must apply the climate gate downstream.

**FM-02: Consolidation count based on close proximity, not range contraction** — The
detection counts bars where close was near the band, not bars with narrow range. A bar
can count as "consolidation" while still having a wide intrabar range. This is weaker
than NR7's strict range-minimum definition.

**FM-03: Citing as an Elder-methodology pattern** — The implementation is a channel
expansion signal. Elder's own work does not use channel breakouts as buy entries. Do
not represent `ELDER_BREAKOUT` as canonical Elder methodology in strategy documentation.

---

## References
- `src/TripleScreen3.cpp` lines 918–1124 — complete implementation
- `src/StudyHelperFunctions.cpp` — `DetectElderBreakout()` function
- Elder, A. *Come Into My Trading Room.* Wiley, 2002 — Keltner Channel and AutoEnvelope
  chapters (Elder's mean-reversion, not breakout, application)
- See `elder_triple_screen.md` §8 (Impulse System) for `INTERM_IMP` used as alignment gate
