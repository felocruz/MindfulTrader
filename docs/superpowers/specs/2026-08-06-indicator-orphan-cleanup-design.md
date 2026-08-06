# Indicator Orphan Cleanup — Design Spec

## 1. Why

### 1.1 Origin: classifying indicators by consumer, in service of the DOD cache-locality goal

The motivation behind the `indicator-manager-dod-soa` migration (`docs/superpowers/plans/2026-08-05-indicator-manager-dod-soa.md`) was cache-miss reduction in the per-tick hot path: compute indicators → check whether any changed → decide whether to publish to Python. That scan (`IndicatorManager::HasSignificantChange()` → `CheckTrigger()`) is confirmed fully devirtualized today — its `switch` has an explicit `case` for all 54 `IndicatorKey` values, no `default:` fallback to virtual dispatch, and `m_dirty_mask` is a single cache-resident `uint64_t`. That specific goal is met.

Reasoning about whether any further DOD work is warranted requires knowing which indicators are actually consumed, by whom, and how. Classifying all 54 `IndicatorKey` values against their real consumers (`EventSerializer.cpp`, `PopulateIndicatorState`, `GetTrainingEventT`, `PositionManager.cpp`, `RiskManager.cpp`, `ContextManager.cpp`, and finally `../schema/mts_schema.fbs` and the generated `indicator_binding_policy_generated.h` binding-policy table) surfaced four keys with no confirmed consumer anywhere: `LONG_MKT_ACTION`, `SHORT_MKT_ACTION`, `THREE_LINE_OSCILLATOR`/`THREE_LINE_OSCILLATOR_PREV`, and `VWAP`. This spec resolves each.

### 1.2 Method note: shallow grep undercounts real consumers

An early pass classified `MARKET_CLIMATE`, `HMM_STATE`, `SIDE`, `MARKET_SYMBOL`, and `OVERNIGHT_EXIT` as unconsumed based on grepping `IndicatorKey` names across four files. All five turned out to be live — reached through side-channel calls between indicators (`MarketClimateIndicator::Update()` calling `ContextManager::Instance().SetRegimeDuration()` directly) or through top-level `Event`/`TrainingEvent` schema fields outside the nested `IndicatorState` struct. The four keys resolved below were re-verified against the generated binding-policy table (`include/generated/indicator_binding_policy_generated.h`, sourced from `schema/regenerate_schema.sh`), which formally and mechanically records `has_live_writer`/`has_training_writer` per key — this is the authoritative source used for the findings below, not string search alone.

## 2. Goals and non-goals

**Goals:**
- Remove confirmed-dead compute (`LONG_MKT_ACTION`) and confirmed-dead compute masquerading as a live container (`SHORT_MKT_ACTION`'s classification) without breaking the live data that object also happens to carry.
- Leave genuinely reserved-but-empty enum slots alone, consistent with existing precedent in the same file.
- Leave a real, actively-computed, deliberately-scoped feature (`VWAP`) untouched pending a future decision on its consumer.

**Non-goals:**
- This is not a decision to pursue "full DOD" migration (rewriting the remaining ~30 indicators' write-side compute into free functions, or closing the ~18 `NotPacked` schema gaps). That was assessed separately as low-likelihood-of-success if attempted now, given the current hybrid migration is itself unverified in its target runtime.
- This does not decide VWAP's future consumer. Four institutional-use candidates were discussed (execution-quality grading, Raschke/Elder confluence filter, toxicity-gate input, regime-conditioning input for `MarketClimateIndicator`) — none selected. Recorded in §4 for future reference only.
- No schema changes (`../schema/mts_schema.fbs` is untouched by every resolution below).
- No deployment. Per the established institutional path (Validation Ladder, `SYSTEM_INTEGRATION_TEST_PROTOCOL.md`), these changes require the same unresolved verification gate as the rest of the `indicator-manager-dod-soa` migration — a Sierra Chart run has not yet happened for any of this work, including the changes proposed here.

## 3. Findings and resolutions

### 3.1 `LONG_MKT_ACTION` — delete

**Finding:** `LongMarketAction::Update(GetPriceActionEnum(...))` is called every tick from `TripleScreen1.cpp:301`. Confirmed zero readers: not in `EventSerializer.cpp`, not in `PopulateIndicatorState`/`GetTrainingEventT`, not in `PositionManager.cpp`/`RiskManager.cpp`/`ContextManager.cpp`, not in the wire schema (`IndicatorLayout.h`'s own audit comment: *"the enum value is computed but never serialized anywhere"*), and the generated binding-policy row confirms `has_live_writer=false, has_training_writer=false`.

**Resolution:** Delete the `LongMarketAction` class (`include/Indicator.h`), its `IndicatorStore` member and `m_indicators[]` registration (`include/IndicatorManager.h`, `src/IndicatorManager.cpp`), and the write call site (`src/TripleScreen1.cpp:301-302`). Same precedent as Task 15 of the DOD/SoA plan (4 orphan classes deleted for the same reason).

### 3.2 `SHORT_MKT_ACTION` — extract live companion values, then delete

**Finding:** `ShortMarketAction::Update(GetPriceActionEnum(...))`'s own classification value has the same dead-value shape as `LONG_MKT_ACTION` — confirmed via the same binding-policy row (`WireClass::non_wire_internal, FieldSink::none, has_live_writer=false, has_training_writer=false`). But the same C++ object (`m_store.short_mkt_action`) is reused as the storage location for four unrelated, genuinely live values:

```cpp
// src/IndicatorManager.cpp:709-714
companions.prevHigh        = m_store.short_mkt_action.PrevHigh();
companions.prevLow         = m_store.short_mkt_action.PrevLow();
companions.prevFourBarHigh = m_store.short_mkt_action.PrevFourBarHigh();
companions.prevFourBarLow  = m_store.short_mkt_action.PrevFourBarLow();
```

These four feed the schema's top-level `Event.prev_high`/`prev_low`/`prev_four_bar_high`/`prev_four_bar_low` (and the `TrainingEvent` equivalents) — confirmed present in `../schema/mts_schema.fbs`. They're written from `TripleScreen3.cpp` via `SetPriceValues()`/`SetPrevDayHighLow()`/`SetPrevFourBarExtremes()`, called on the same object.

**Resolution:** Extract `PrevHigh()`/`PrevLow()`/`PrevFourBarHigh()`/`PrevFourBarLow()` and their setters into their own holder, decoupled from `ShortMarketAction`'s identity (e.g., a small dedicated struct on `IndicatorStore`, not tied to any `IndicatorKey`). Update `IndicatorManager.cpp:709-714` and the `TripleScreen3.cpp` write sites to use the new holder. Only then delete the `ShortMarketAction` class and its dead `PriceActionEnum` compute. **Ordering matters**: deleting the class before extracting the companion values would silently break `prev_high`/`prev_low`/`prev_four_bar_high`/`prev_four_bar_low` in production — the exact class of miss (`FM-01`-shaped) already documented once this migration.

### 3.3 `THREE_LINE_OSCILLATOR` / `THREE_LINE_OSCILLATOR_PREV` — leave reserved

**Finding:** Enum values only (`IndicatorKey.h:56-57`), a `NotPacked` row in `IndicatorLayout.h`, and nothing else — no leaf class, no write, no read, anywhere, predating this migration. Confirmed by the user's own lack of recollection of this indicator ever existing functionally.

**Resolution:** No code change. `IndicatorKey.h` already has direct precedent for this exact situation one line above: `// 30 reserved (SESSION_AGE removed)`. Follow the same pattern rather than renumbering every subsequent key, which would be higher-risk for no benefit.

### 3.4 `VWAP` — no action, defer

**Finding:** `VwapIndicator::UpdateVwap()` computes session-cumulative VWAP and an ATR-normalized distance every tick (`TripleScreen3.cpp`). `Indicator.h` states explicit design intent: *"VWAP is for trade-execution only — no FlatBuffer schema field exists."* Confirmed via the generated binding-policy row (`WireClass::non_wire_internal, FieldSink::none, has_live_writer=false, has_training_writer=false`) — consistent with that stated intent. But the intended execution-side consumer was never built: exhaustive search of `RiskManager.cpp`, `PositionManager.cpp`, `ContextManager.cpp`, and every other class in `src/`/`include/` found zero readers of `GetVwapPrice()`/`GetDistanceNorm()`/the `VwapPositionEnum` classification.

Searched `../lbrnet` for a documented plan: no genuine match (`rc_enums.py`'s `VWAP` entry is an unrelated Interactive Brokers execution-algorithm type, not this indicator). Searched this project's own docs: `docs/PYTHON_AI_PHASE_ROADMAP.md` mentions a similarly-named but distinct proposal (a Python-side `vwap_distance` feature-engineering idea with an unresolved action item, *"C++ team verify which order flow metrics are available"*) — not a tracked spec for the indicator that was actually built.

**Resolution:** Leave the compute in place. This is a half-finished feature (deliberate design intent, no consumer), not dead code — removing it would discard real, working design intent on the strength of an absent consumer alone. Four institutional-use candidates were discussed as future options, none committed to:
1. Execution-quality grading (VWAP distance at entry/exit, alongside existing Keltner-channel-based `trade_grade`/`entry_grade`/`exit_grade`)
2. Confluence/veto filter for existing Raschke/Elder counter-trend patterns in `TripleScreen3.cpp` (mean-reversion fair-value anchor, same shape as the existing Hurst/entropy "ELITE PHYSICS FILTER" gates)
3. Additional input to `RiskManager`'s existing toxicity/illiquidity gates (`amihud_illiquidity`/`amihud_percentile`)
4. Regime-conditioning input to `MarketClimateIndicator::Update()`'s momentum/coil decision matrix, alongside Hurst/entropy/HMM state

Revisit only when there's a concrete reason to pick one of these (or another use) — not as part of this cleanup.

## 4. Verification approach

For §3.1 and §3.2 (the only code changes in this spec):
- Full-repo grep for the deleted class names and `IndicatorKey` values after the change, confirming no dangling references (headers, PCH, generated files).
- `./build_dll.sh` clean build.
- Re-run the 9 standalone `tests/cpp/*.cpp` unit tests (`g++ -std=c++2a`, per this session's verification method) — particularly `test_indicator_packed_state.cpp` and `test_indicator_layout.cpp`, since both touch `kIndicatorLayout` row classifications.
- For §3.2 specifically: confirm `prev_high`/`prev_low`/`prev_four_bar_high`/`prev_four_bar_low` still populate correctly — this is the one change in this spec with real production data at stake if done in the wrong order.
- No Sierra Chart run is in scope for this spec's verification; it inherits the same outstanding institutional gate (Simulated Trading → SIT) as the rest of the DOD/SoA migration before any of this reaches production.

## 5. Success criteria

- `LONG_MKT_ACTION` and `SHORT_MKT_ACTION`'s dead classification compute no longer exist in the codebase.
- `prev_high`/`prev_low`/`prev_four_bar_high`/`prev_four_bar_low` are unaffected — same values, now sourced from a holder with no misleading name.
- `THREE_LINE_OSCILLATOR`/`_PREV` and `VWAP` are unchanged; this spec's disposition for each (reserved / deferred) is recorded so a future session doesn't re-derive the same investigation from scratch.
- `./build_dll.sh` succeeds; all 9 standalone unit test files still pass with 0 failures.
