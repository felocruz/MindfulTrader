---
domain: sierra_chart/screen3_patterns
intent: ELDER_FAKE_BREAKOUT implementation spec — Elder's False Breakout with Divergence; one pattern, two directional mirrors; TS2 MACD-H divergence arms, TS3 Keltner band reversal triggers; proposed replacement for ELDER_BREAKOUT
scope: global
tags: [elder, screen3, fake-breakout, divergence, MACD-H, keltner, TS2, TS3, spec, replacement-for-ELDER_BREAKOUT]
source_files:
  - src/TripleScreen3.cpp
  - include/Indicator.h
status: SPEC — not yet implemented
last_verified: 2026-06-27
dependencies: [elder_triple_screen, elder_breakout]
---

# ELDER_FAKE_BREAKOUT — Implementation Spec

## Answer: One Pattern, Two Directional Mirrors

There is **one pattern** with **two directional implementations** that are exact mirrors:

| Version | Direction | Price breakout | Divergence condition | Reversal trigger |
|---|---|---|---|---|
| Bearish Fake Breakout | Short | Close above Keltner `TopBand` | TS2 MACD-H bearish divergence | Close back below `TopBand` |
| Bullish Fake Breakout | Long | Close below Keltner `BottomBand` | TS2 MACD-H bullish divergence | Close back above `BottomBand` |

The logic is identical in both; only the direction of all comparisons flips.

---

## Why This Replaces ELDER_BREAKOUT

The existing `ELDER_BREAKOUT` implementation is **removed and replaced in-place** by this
pattern. The old code is a **continuation** signal (close beyond the band = go in that
direction). Elder's actual published breakout pattern is a **reversal** signal — price exiting
the channel reveals exhaustion when accompanied by MACD-H divergence, and the trade fades the
breakout on the return inside. The old implementation contradicts Elder's explicit channel
rule: *"Never buy above the upper channel line."*

The replacement uses the same Keltner band geometry, the same subgraph slots, the same
`RaschkeTacticalTrigger` enum values, and the same `IndicatorKey`. The only thing that
changes is the detection logic in `TripleScreen3.cpp`: **the breakout is the warning,
not the entry.**

---

## The Invariant / Contract

**Signal condition:** Price exceeds the Keltner band **while TS2 shows a MACD-H divergence
pattern**, then closes back inside the band on TS3. The pattern requires both conditions
simultaneously — divergence without the price reversal is setup only; price reversal without
divergence is noise.

Elder's direct quote: *"This is the strongest signal in technical analysis."*

---

## Three-Phase Detection

### Phase 1 — TS2 Arming: MACD-H Divergence

Accessed via:
```cpp
auto& divInd = indMgr.GetIndicator<MACDDivergence>(IndicatorKey::INTERM_MACD_DIVERGENCE);
MACDDivergenceEnum divState = divInd.Value();
```

**Bearish arm condition** (for short):
```
divState == BEARISH_DIVERGENCE_PATTERN (-4)    // Pattern complete, awaiting downtick
         OR BEARISH_DIVERGENCE_SELL_SIGNAL (-5) // Downtick confirmed on TS2
```

**Bullish arm condition** (for long):
```
divState == BULLISH_DIVERGENCE_PATTERN (4)     // Pattern complete, awaiting uptick
         OR BULLISH_DIVERGENCE_BUY_SIGNAL (5)  // Uptick confirmed on TS2
```

**What `-4 / BEARISH_DIVERGENCE_PATTERN` means (per state machine in `Indicator.h:412`):**
Price on TS2 made a higher high; MACD-H at that high was lower than the prior high (less
positive), and MACD-H crossed zero between the two peaks. This is Elder's complete divergence
criterion: *"MACD-H MUST cross the zero line between peaks — no crossover, no divergence."*

**What `-5 / BEARISH_DIVERGENCE_SELL_SIGNAL` means:**
MACD-H has now downticked from the second peak on TS2 — the micro-turn Elder calls the
actual signal. Either state arms the TS3 phase; `-5` is the higher-confidence arm.

### Phase 2 — TS3 Breakout Detection

Using Keltner bands already computed for the `scsf_Screen3_KeltnerChannel` study:
```
Bearish: sc.Close[0] > Subgraph[SG_TOP_BAND][0]
Bullish: sc.Close[0] < Subgraph[SG_BOTTOM_BAND][0]
```

Store a `bool isAboveTopBand` flag for the session; set true on first qualifying close.
Store the breakout bar index to allow a maximum lookback before the reversal must occur.

Gap case: `sc.Open[0] > Subgraph[SG_TOP_BAND][0]` means the session opened through the band
already — flag as `isGap = true` (same gap-detection logic as existing `ELDER_BREAKOUT`).

### Phase 3 — TS3 Reversal Trigger

The trade fires when price returns inside the band within a lookback window:
```
Bearish: sc.Close[0] < Subgraph[SG_TOP_BAND][0]
         AND isAboveTopBand == true (prior phase 2 was triggered)
         AND TS2 arm condition still active
         AND bars since breakout ≤ FAKE_BREAKOUT_MAX_REVERSAL_BARS (suggested: 4)

Bullish: sc.Close[0] > Subgraph[SG_BOTTOM_BAND][0]
         AND isBelowBottomBand == true
         AND TS2 arm condition still active
         AND bars since breakout ≤ FAKE_BREAKOUT_MAX_REVERSAL_BARS (4)
```

The `FAKE_BREAKOUT_MAX_REVERSAL_BARS` constant prevents arming an entry on a reversal that
occurs 20 bars after the original breakout. The divergence is fresh; the reversal should be
prompt. Four 15-min bars = 1 hour of tolerance.

---

## Quality Scoring

Assign a quality float `[0.0, 1.0]` for Orchestrator consumption. Factors to weight:

| Factor | Bullish weight | Bearish weight |
|---|---|---|
| TS2 state is `-5/+5` (confirmed downtick/uptick) vs `-4/+4` (pattern only) | +0.15 | +0.15 |
| Gap through band (extreme overshoot) | +0.10 | +0.10 |
| Screen 1 MACD (`LONG_MACD`) slope aligned with reversal | +0.15 | +0.15 |
| HMM state `PARETO_FAT_TAIL` or `STUDENT_T_STRESS` (extreme = best for exhaustion reversals) | +0.10 | +0.10 |
| Volume on breakout bar above `SG_AVG_VOLUME` (climactic flush) | +0.10 | +0.10 |
| Reversal occurs on bar 1–2 (prompt) vs bar 3–4 (marginal) | +0.10 / 0.0 | +0.10 / 0.0 |

Threshold: forward to `RaschkeTacticalTrigger` only if quality ≥ 0.5.

---

## Context Gates

Same three gates as `ELDER_BREAKOUT`, semantics unchanged:

| Gate | Condition | Note |
|---|---|---|
| `impulseAligned` | `INTERM_IMP` is RED for bearish fake breakout / GREEN for bullish | Confirms TS2 Impulse System agrees with reversal direction |
| `screenAligned` | HMM state via `InferenceManager::HmmState()` supports reversal | Prefer `PARETO_FAT_TAIL` (reversals at extremes) |
| `channelSqueeze` | ATR(10) < 5-bar ATR avg × 0.9 | Lower priority than `impulseAligned`; include as metadata |

---

## Forwarding to RaschkeTacticalTrigger

The existing enum values are reused and **must not be renamed** while the current FlatBuffers
schema is in force. `RaschkeTacticalTrigger` is serialized in `../schema/mts_schema.fbs`;
renaming the enum values would be a breaking wire-format change requiring coordinated schema
migration across C++, Python consumers, and stored `.btst` files:

```cpp
// Bearish fake breakout (short)
if (quality >= 0.5f)
    SetRaschkeTacticalTrigger(RaschkeTacticalTrigger::ELDER_BREAKOUT_SELL);

// Bullish fake breakout (long)
if (quality >= 0.5f)
    SetRaschkeTacticalTrigger(RaschkeTacticalTrigger::ELDER_BREAKOUT_BUY);
```

The new implementation replaces the old one in-place. Python consumers, serialized event
fields, and any downstream systems already reading `ELDER_BREAKOUT_BUY/SELL` require no
changes — only the C++ detection logic changes.

---

## Visualization

Reuse subgraph slots currently assigned to ELDER_BREAKOUT:
```
SG_ELDER_BREAKOUT_BULLISH (22) → triangle-up at reversal bar close (bullish fake breakout)
SG_ELDER_BREAKOUT_BEARISH (23) → triangle-down at reversal bar close (bearish fake breakout)
```

Mark at the REVERSAL bar (phase 3), not the breakout bar (phase 2). The triangle marks where
the trade would enter, not where the breakout occurred.

---

## Stop Placement

**Chandelier Stop** (existing `ChandelierStopManager`): ATR-based trailing stop,
scaled by HMM `DofStopScale()` for fat-tail regime widening. No change to stop
management — the pattern provides the entry signal only.

Pattern-derived stop levels from Elder (stop at the band that was breached) are NOT
used — Chandelier is authoritative.

---

## Comparison with Current ELDER_BREAKOUT

| Attribute | Current ELDER_BREAKOUT | ELDER_FAKE_BREAKOUT (this spec) |
|---|---|---|
| Signal type | Continuation (breakout → follow direction) | Reversal (breakout → fade on return) |
| TS2 arming | None (INTERM_IMP gate only) | `INTERM_MACD_DIVERGENCE` state required |
| Entry trigger | Close beyond band | Close BACK INSIDE band after prior breakout |
| Elder-canonical? | No (contradicts "never buy above channel line") | Yes — his named pattern from *Step by Step Trading* |
| Consolidation detection | 5-bar proximity count | Not needed — divergence plays the same filtering role |
| Quality tiers | WEAK / STRONG / EXTREME | Same 3-tier output |

---

## Elder Source Material

From *Step by Step Trading* (Wiley, 2015), Ch. 7: "False Breakout with Divergence":

> *"When price breaks out of a channel and MACD-Histogram shows divergence, it is giving
> you one of the most powerful signals — more powerful than any other because it combines
> two independent analytical methods saying the same thing."*

Elder's operational definition of the signal:
1. Price trades **outside** the channel line (above upper or below lower)
2. MACD-H at that extreme is **weaker** than at the prior extreme (divergence confirmed)
3. Price returns **inside** the channel → enter against the breakout direction

The MACD-H zero-cross rule from `Indicator.h:384` applies: *"If there is no crossover,
then there is no divergence."* This is already enforced by `DetectElderMACDDivergence()`
in the state machine — arming on `-4` / `+4` guarantees this rule was satisfied on TS2.

---

## Implementation Constraints

- **TS2 access:** Already established — `indMgr.GetIndicator<MACDDivergence>(IndicatorKey::INTERM_MACD_DIVERGENCE)` on line 13 of `IndicatorKey` enum
- **Band access:** `sc.Subgraph[SG_TOP_BAND]` and `sc.Subgraph[SG_BOTTOM_BAND]` already populated in `scsf_Screen3_KeltnerChannel`
- **Bar flag persistence:** Two `static bool` flags per direction needed: `isAboveTopBand`, `isBelowBottomBand`; reset each session
- **Breakout bar index:** One `static int` per direction: `breakoutBarIdx`; compare against `sc.Index` to enforce `FAKE_BREAKOUT_MAX_REVERSAL_BARS`
- **No new subgraph slots needed** — reuses `SG_ELDER_BREAKOUT_BULLISH` / `SG_ELDER_BREAKOUT_BEARISH`
- **No new IndicatorKey needed** — reuses `ELDER_BREAKOUT = 24`; enum value and name stay as-is

---

## Failure Modes

**FM-01: Divergence on TS2 is stale by the time TS3 reversal fires** — `BEARISH_DIVERGENCE_SELL_SIGNAL`
has a finite validity window; the state machine can reset. Check that the TS2 arm state is
still active on the same bar as the TS3 reversal, not just at the breakout bar.

**FM-02: Multiple fake-breakout attempts during a single divergence window** — The TS2 state
may remain in `-4/-5` for several 60-min bars while TS3 oscillates around the band. Limit to
one ELDER_FAKE_BREAKOUT signal per TS2 divergence episode; reset only when TS2 returns to NONE.

**FM-03: Confusing direction — arming bearish on bullish divergence** — The bearish
fake breakout requires `BEARISH_DIVERGENCE_SELL_SIGNAL (-5)` (price higher high + MACD lower
high). Double-check sign convention: negative enum values = bearish states in `MACDDivergenceEnum`.

**FM-04: Chandelier stop placement on a reversal entry** — On a fake breakout short,
the Chandelier stop trails upward — exactly the wrong direction initially. The Chandelier
must be seeded at the breakout extreme (the recent high) as its initial anchor, not at the
current close. Confirm `ChandelierStopManager` handles this correctly at entry.

---

## References
- Elder, A. *Step by Step Trading.* Wiley, 2015. Ch. 7 — False Breakout with Divergence (primary source)
- Elder, A. *The New Trading for a Living.* Wiley, 2014. Ch. 25 — Divergences (state machine rules)
- `include/Indicator.h` lines 380–414 — `MACDDivergenceEnum` state machine and Elder's critical rules
- `src/TripleScreen3.cpp` lines 16–79 — `SubgraphIndex` enum confirming `SG_TOP_BAND`, `SG_BOTTOM_BAND`, `SG_ELDER_BREAKOUT_BULLISH/BEARISH`
- See `elder_breakout.md` — documents what ELDER_BREAKOUT currently does (the pattern this replaces)
- See `elder_triple_screen.md` — for Triple Screen hierarchy governing Screen 1/2 conditions
