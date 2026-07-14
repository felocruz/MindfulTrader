# Overnight Trading Windows - Quick Reference Guide

## 24-Hour Trading Day Timeline (Eastern Time)

```
═══════════════════════════════════════════════════════════════════════════════
                        GLOBEX / ELECTRONIC SESSION
═══════════════════════════════════════════════════════════════════════════════

18:00 ──────────────────────────────────────────────────── 03:00 ET
│                  ASIAN_SESSION (0)                            │
│  Tokyo/Sydney range formation                                 │
│  Action: Monitor range, prepare for London                    │
│  Quality: POOR (range-bound, low volume)                     │
└───────────────────────────────────────────────────────────────┘

03:00 ─────────── 04:00 ET
│   LONDON_WINDOW (1)    │  🎯 KEY ENTRY WINDOW
│  "London Maneuver"     │
│  Action: Watch for false breakout of Asian range             │
│  Entry: Trade the rejection (trap traders on wrong side)     │
│  Quality: GOOD (institutional entry point)                   │
└────────────────────────┘

04:00 ──────────────────────────────────── 08:30 ET
│     LONDON_TO_PREMARKET (2)                │
│  European trading, pre-data positioning    │
│  Action: Watch for continuation/reversal   │
│  Quality: MODERATE                         │
└────────────────────────────────────────────┘

08:30 ───────── 09:00 ET
│  PRE_MARKET_HOOK (3)  │  🎯 KEY ENTRY WINDOW
│  Economic data reaction│
│  Action: Enter on test & rejection of key level              │
│  Entry: "Hook" pattern - false move that traps traders       │
│  Quality: EXCELLENT (best pre-market entry)                  │
└───────────────────────┘

═══════════════════════════════════════════════════════════════════════════════
                       REGULAR TRADING HOURS (RTH)
═══════════════════════════════════════════════════════════════════════════════

09:00 ── 09:30 ET
│  PRE_MARKET (4)  │
│  Final pre-open  │  ⏰ OVERNIGHT EXIT WINDOW (Primary)
│  Action: Exit overnight gaps/failures here                   │
│  Quality: N/A (primarily for exits)                          │
└──────────────────┘

09:30 ────────── 10:30 ET
│   OPENING_HOUR (5)    │  ⏰ OVERNIGHT EXIT WINDOW (Primary)
│  Trend establishment  │
│  Action: Exit gaps (09:30-09:45), first reactions (09:30-10:00)
│  Entry Quality: GOOD (requires confirmation)                 │
└───────────────────────┘

10:30 ──────────────── 12:00 ET
│     SWEET_SPOT (6)         │  🎯 BEST ENTRY WINDOW
│  Cleanest trends           │
│  Action: Highest-quality setups, best follow-through         │
│  Quality: EXCELLENT (institutional participation)            │
└────────────────────────────┘

12:00 ──────────────── 14:00 ET
│  LUNCH_DEAD_ZONE (7)     │  ⚠️ AVOID NEW ENTRIES
│  Choppy, low volume      │
│  Action: Hold existing, no new entries                       │
│  Quality: POOR (institutional lunch)                         │
└──────────────────────────┘

14:00 ────────── 15:00 ET
│  AFTERNOON_SESSION (8)  │
│  Second chance setups   │
│  Action: Trade high-probability continuations only           │
│  Quality: GOOD (but shorter time window)                     │
└─────────────────────────┘

15:00 ──────────── 15:45 ET
│   FINAL_HOUR (9)      │  ⚠️ AVOID NEW ENTRIES (unless strong)
│  Close intraday trades│
│  Action: Exit intraday positions, tighten stops              │
│  Quality: VERY POOR (high noise, squaring)                   │
└───────────────────────┘

15:45 ── 16:00 ET
│ PM_RUN_ENTRY (10)│  🎯 CONDITIONAL ENTRY WINDOW
│ "3:30 PM Run"    │
│ Action: ONLY enter if IMMEDIATE profit expected              │
│ Rule: Must be profitable by 16:00 close to hold overnight    │
│ Quality: CONDITIONAL (high risk, high conviction only)       │
└──────────────────┘

16:00 ──────────────── 18:00 ET
│   AFTER_HOURS (11)        │  ⏰ GOLDEN RULE EVALUATION
│  Position squaring        │
│  Action: Evaluate overnight hold criteria (Golden Rule)      │
│  Quality: POOR (low liquidity)                               │
└───────────────────────────┘

═══════════════════════════════════════════════════════════════════════════════
                           OVERNIGHT HOLD STATE
═══════════════════════════════════════════════════════════════════════════════

18:00 ──────────────────────────────────────────────────── 09:30 ET (next day)
│                    OVERNIGHT_HOLD (12)                            │
│  Position carried through close                                   │
│  Action: Monitor Globex, prepare for morning exit                 │
│  Exit Windows: London (03:00-04:00), Pre-Market Hook (08:30-09:00)│
│  Primary Exit: Opening Hour (09:30-10:30)                         │
└───────────────────────────────────────────────────────────────────┘
```

---

## The Golden Rule for Overnight (16:00 ET Evaluation)

### ✅ PASS → Hold Overnight (SWING_POSITION)

```
Criteria (ALL must be true):
├─ ✅ Position in PROFIT
├─ ✅ Close in top 25% (LONG) or bottom 25% (SHORT) of daily range
├─ ✅ Stop at breakeven or better
├─ ✅ Screen 1 (240-min) trending
├─ ✅ Trend-following setup (not reversal)
└─ ✅ Not Friday (no weekend risk)

Action: Set OVERNIGHT_HOLD state, prepare for morning exit
```

### ❌ FAIL → Scratch at Close (SCRATCH_AT_CLOSE)

```
Any of:
├─ ❌ Position FLAT or at LOSS
├─ ❌ Close in middle or wrong side of daily range
├─ ❌ Stop not at breakeven
├─ ❌ Screen 1 not trending
├─ ❌ Reversal setup (not trend-following)
└─ ❌ Friday (weekend risk)

Action: Exit flat at close, don't hope for gap
```

---

## Morning Exit Decision Tree (OVERNIGHT_HOLD → Exit Type)

### Step 1: Check for Gap (09:30 ET Open)

```
Gap > 0.5% in favor?
├─ YES → GAP_EXIT (3)
│         Exit immediately (09:30-09:45 ET)
│         "Windfall - take profits into strength"
│
└─ NO → Continue to Step 2
```

### Step 2: Check Taylor Objective Point (Globex or RTH)

```
Hit previous day's High (LONG) or Low (SHORT)?
├─ YES → OBJECTIVE_POINT_EXIT (5)
│         Exit at liquidity window (London or NY Open)
│         "Target hit - book profits"
│
└─ NO → Continue to Step 3
```

### Step 3: Check 3-10 Oscillator (During Globex)

```
Fast Line (3-period) crossed Slow Line (16-period)?
├─ YES → MOMENTUM_FAILURE_EXIT (6)
│         Exit pre-market or on first bounce
│         "Momentum shifted - get out"
│
└─ NO → Continue to Step 4
```

### Step 4: Check Open Quality (09:30-10:00 ET)

```
Open flat or against position?
├─ YES → FIRST_REACTION_EXIT (4)
│         Exit on first bounce (don't wait for stop)
│         "Strong close didn't lead to strong open - exit"
│
└─ NO → HOLD_FOR_TARGET (8) or TRAILING_STOP_EXIT (9)
          "Position still healthy - continue holding"
```

---

## Entry Windows Summary

| Window | Time (ET) | Type | Quality | Action |
|--------|-----------|------|---------|--------|
| **LONDON_WINDOW** | 03:00-04:00 | Entry | GOOD | False breakout of Asian range |
| **PRE_MARKET_HOOK** | 08:30-09:00 | Entry | EXCELLENT | Test & rejection of key level |
| **OPENING_HOUR** | 09:30-10:30 | Entry | GOOD | Trend establishment (requires confirmation) |
| **SWEET_SPOT** | 10:30-12:00 | Entry | EXCELLENT | Cleanest trends, best setups |
| **AFTERNOON_SESSION** | 14:00-15:00 | Entry | GOOD | Second chance setups |
| **PM_RUN_ENTRY** | 15:45-16:00 | Entry | CONDITIONAL | Must profit immediately to hold |

---

## Exit Windows Summary

| Window | Time (ET) | Type | Trigger | Action |
|--------|-----------|------|---------|--------|
| **GAP_EXIT** | 09:30-09:45 | Exit | Gap > 0.5% | Sell into strength (windfall) |
| **FIRST_REACTION_EXIT** | 09:30-10:00 | Exit | Flat/adverse open | Exit on first bounce |
| **OBJECTIVE_POINT_EXIT** | Any (Globex/RTH) | Exit | Taylor target | Exit at liquidity window |
| **MOMENTUM_FAILURE_EXIT** | Globex/Pre-Market | Exit | 3-10 Osc cross | Exit pre-market |
| **SCRATCH_EXIT** | 16:00 (close) | Exit | Failed Golden Rule | Exit flat at close |

---

## Implementation Checklist

- [ ] Update `TimeOfDayIndicator::SetFromDateTime()` to include Globex windows
- [ ] Add `HoldingStrategyIndicator::IsStrongClose()` for Golden Rule validation
- [ ] Implement `OvernightExitIndicator::SetFromOvernightContext()` for morning exits
- [ ] Add 3-10 Oscillator calculation to `IndicatorManager`
- [ ] Track `m_prevDayHigh` and `m_prevDayLow` in `Trade` class
- [ ] Add overnight evaluation logic to `PositionManager::Update()`
- [ ] Test Golden Rule validation at 16:00 ET close
- [ ] Test morning exit logic (gaps, objective points, oscillator)
- [ ] Add PM_RUN_ENTRY conditional evaluation

---

**Reference:** [OVERNIGHT_MANAGEMENT_RASCHKE_TAYLOR.md](OVERNIGHT_MANAGEMENT_RASCHKE_TAYLOR.md)
