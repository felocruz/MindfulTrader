# Knowledge Base Index

> Routing directory for assembling context. Each entry: chunk file → one-line intent hook.
> Add new chunks here immediately after creating the file.

---

## global/sierra_chart

| File | Intent |
|---|---|
| [acsil_replay.md](global/sierra_chart/acsil_replay.md) | ACSIL replay/backtesting API — IsReplayRunning, IsFullRecalculation, replay modes, study call triggers, and canonical guard patterns |
| [elder_triple_screen.md](global/sierra_chart/elder_triple_screen.md) | Elder-Raschke Confluence System — MindfulTrader's named methodology: Elder's three-screen hierarchy + Raschke Screen 3 patterns + HMM/entropy regime layer |
| [street_smarts_patterns.md](global/sierra_chart/street_smarts_patterns.md) | Street Smarts (Raschke & Connors, 1995) — setup/trigger framework; Turtle Soup, Momentum Pinball, Holy Grail, Anti, ADX Gapper, NR7, 80-20; Screen 3 pattern canonical rules |
| [kangaroo_tail.md](global/sierra_chart/kangaroo_tail.md) | Elder's Kangaroo Tail — 3-bar OHLC failed-raid reversal pattern; canonical entry/stop rules; distinction from Peters/Nekritin 1-candle variant |
| [crabel_nr7.md](global/sierra_chart/crabel_nr7.md) | NR7/NR4 breakout — Street Smarts version is operative (prior bar H/L trigger + HV6/HV100 ≤ 0.5 filter); Crabel Stretch documented for provenance only |
| [elder_breakout.md](global/sierra_chart/elder_breakout.md) | ELDER_BREAKOUT (IndicatorKey=24) — Keltner Channel breakout after consolidation; Hurst filter + context scoring; flagged replacement candidate for ELDER_FAKE_BREAKOUT |
| [elder_fake_breakout_spec.md](global/sierra_chart/elder_fake_breakout_spec.md) | ELDER_FAKE_BREAKOUT spec — Elder's False Breakout with Divergence; TS2 MACD-H divergence arms, TS3 Keltner band reversal triggers; one pattern, two directional mirrors (SPEC, not yet implemented) |

## global/architecture

*(empty — add chunks here as they are created)*

## global/zmq

*(empty — add chunks here as they are created)*

## global/cpp

| File | Intent |
|---|---|
| [indicator_manager_dod.md](global/cpp/indicator_manager_dod.md) | IndicatorManager's hybrid DOD/OOD architecture — packed SoA arrays are canonical for hot-path reads; IndicatorStore/BaseIndicator/Indicator<T> stays permanently as the write-side engine + NotPacked read path; the FM-01 "row exists ≠ write side wired" trap that already bit this codebase once |

---

## Migration Queue (docs to be chunked — in order of priority)

These existing docs contain valuable content that hasn't been chunked yet.

| Source | Target chunk(s) | Status |
|---|---|---|
| `docs/BACKTESTING_FRAMEWORK.md` | `global/architecture/backtesting_framework.md` | pending |
| `schema/FLATBUFFER_MASTER_SPECIFICATION.md` | `global/architecture/flatbuffer_contract.md` | pending |
| `docs/TRADE_EXECUTION_SYSTEM.md` | `global/architecture/trade_lifecycle.md` | pending |
| `docs/RISK_MANAGEMENT_SYSTEM.md` | `global/cpp/risk_management.md` | pending |
| `docs/HMM_RUNTIME_REFERENCE.md` | `global/architecture/hmm_runtime.md` | pending |
| `src/IndicatorManager.cpp` + `IndicatorManager.h` | `global/cpp/indicator_manager_dod.md` | done (2026-08-06) |
| ZMQ port topology (from CLAUDE.md) | `global/zmq/port_topology.md` | pending |
| `docs/TRADING_STRATEGIES_COMPLETE_REFERENCE.md` | `global/architecture/trading_strategies.md` | pending |
| `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md` | `global/cpp/gang_hurst_mandelbrot.md`, `global/cpp/gang_shannon_entropy.md`, `global/cpp/gang_taleb_kurtosis.md`, `global/cpp/gang_pareto_hill_estimator.md` (one per pillar) | pending — spec is still evolving (rows marked `needs-empirical-validation`); chunk once findings settle to `validated`/`under-powered` (see spec's own status-table convention) |
| `docs/ADR/triple_barrier_exit_engine_spec.md` + `triple_barrier_cutover_phase1_plan.md` (+ 5 companion rulings) | `global/cpp/exit_doctrine_triple_barrier.md` | pending — **stable, settled fact, not fluid**: `ChandelierStopManager` was fully deleted (commit `9ee5326`, 2026-07-15); live exit doctrine is single-stage immutable first-touch Triple Barrier, not multi-stage Chandelier trailing/scale-out. High-priority chunk — this is exactly the kind of "assistant doesn't know a major deletion happened" gap the KB exists to prevent, and unlike the Gang spec above there's nothing left to validate first. |

---

## Chunking Rules

1. **One chunk = one answerable question.** If you can't write the `intent:` in one sentence, split the file.
2. **Global chunks don't reference scratchpad state.** If it changes week-to-week, it's local.
3. **`last_verified` must be within 90 days** or the assembler will warn. Verify against actual code before setting.
4. **500 lines max per chunk.** Longer = two chunks.
5. **Always add to INDEX.md immediately** after creating a chunk file.
