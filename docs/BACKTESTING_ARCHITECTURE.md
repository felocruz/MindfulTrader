# Backtesting Architecture

**Last Updated**: 2026-05-04
**Canonical Reference**: `../../docs/BACKTESTING_FRAMEWORK.md`

---

This document is a companion rationale summary. For the authoritative implementation contract, artifact schemas, promotion gates, ZMQ protocol map, and milestone roadmap, see the canonical framework document at:

```
/home/rcruz/devel/VSCode/docs/BACKTESTING_FRAMEWORK.md
```

---

## What Makes This Pipeline Different

Most backtesting frameworks simulate what a model *would* have done. This system actually *runs* the model.

During Sierra Chart replay, `IndicatorManager`, `PositionManager`, `RiskManager`, and `Trade` execute unchanged. The Transformer and HMM run real inference through the same ZMQ pipeline as live trading. Fill events come from authentic Sierra Chart callbacks, not synthetic price assumptions. Backtesting is a mode over the live architecture — not a parallel one.

## Key Architectural Decisions

All decisions below are documented in detail in `BACKTESTING_FRAMEWORK.md`.

| Decision | Choice | Rationale |
|---|---|---|
| Execution model | Advisory GO signal; Sierra Chart manages stops/targets natively | Removes custom OMS bugs and latency from critical path |
| Fill source | Authentic Sierra Chart ACSIL callbacks | Same code path as live trading; no fill simulation layer |
| Replay mode | Accurate Trading System Back Test Mode | Record-level efficiency; suitable for bar-based strategies |
| Execution gap estimate | 10–15% of edge | ~1–2 tick round-trip spread; queue position not modeled |
| Output format (current) | JSONL | Debuggable; sufficient for current research phase |
| Output format (planned) | FlatBuffers → Parquet post-conversion | Compact, typed, no Arrow DLL deployment in Sierra Chart DLL |
| Schema strategy | Sibling `backtest_schema.fbs` with `include "mts_schema.fbs"` | Type reuse without version coupling to live wire schema |

## Validation Ladder

```
Accurate Backtest    →  Does the model have alpha?
        ↓ delta: OHLC fill approximation
Simulated Trading    →  Does entry difficulty survive real bid/ask?
        ↓ delta: queue position + broker latency
Paper Trading        →  Does execution quality hold at the broker?
        ↓ delta: psychology + sizing
Live Trading         →  Final confirmation
```

The same pipeline runs in all four stages. The delta between stages is diagnostic: it tells you exactly where edge is being created or consumed.

## Related Documents

| Document | Purpose |
|---|---|
| `../../docs/BACKTESTING_FRAMEWORK.md` | **Canonical spec**: artifacts, promotion gates, ZMQ protocol, milestones |
| `docs/TRADE_EXECUTION_SYSTEM.md` | PositionManager, order routing, RiskManager |
| `docs/TRANSFORMER_MODEL_INTEGRATION.md` | Transformer inference pipeline, ZMQ protocol |
| `docs/FLATBUFFER_SERIALIZATION_PATTERN.md` | FlatBuffers patterns used in this codebase |
| `src/BackTesterStudy.cpp` | Implementation (Phases 1–3) |
| `../../schema/mts_schema.fbs` | Live wire schema (to be included by backtest_schema.fbs) |
