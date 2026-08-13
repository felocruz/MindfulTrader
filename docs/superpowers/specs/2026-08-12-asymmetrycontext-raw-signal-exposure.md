# SUPERSEDED: Expose Raw Risk-Gate Signals (Findings 19/20/21) — Already Shipped, Different Design

Date filed: 2026-08-12. Corrected same day after further investigation.

**This doc's original recommendation (extend `AsymmetryContext` with 3 raw fields) is wrong and was
never implemented as written — not because it wasn't authorized, but because a real spec already
existed and a better design already shipped before this doc was written.**

## What actually happened

`docs/ADR/risk_gate_context_wire_spec.md` (dated 2026-07-14, status "DRAFT FOR REVIEW") designed this
exact problem properly and explicitly **rejected** the `AsymmetryContext`-extension approach this doc
proposed: *"`AsymmetryContext` is a struct (fixed 8 floats) → cannot be extended additively (rejected
as a home for that reason)."* Instead it specified a new, dedicated `RiskGateContext` **table**
(FlatBuffers tables support additive optional fields; structs don't) mirroring the full
`LocalRiskContext` (16 fields, not just the 3 this doc proposed), embedded on `MarketObservation`.

That design was implemented and shipped **2026-07-15, commit `1190e1f`, on `master`** —
`ContextManager::EmitTrainingContext()` populates `MarketObservation.risk_gate_context` (`spread_stress`,
`pareto_tail_alpha`, `amihud_illiquidity`, and 13 more raw fields) from `m_localRiskContext` on every
`.context` write. This closes Findings 19/20/21 on the C++ side. Verified directly against the schema,
generated headers, and `ContextManager.cpp` on 2026-08-12 — a month after it shipped.

**Root cause of this doc's error**: it was written from `docs/ADR/execution_correctness_findings_spec.md`'s
Findings 19/20/21 text, which still read "Not yet fixed here... requires separate authorization" —
that text was stale (never updated after the `RiskGateContext` fix landed), and this doc's author
(this session) didn't check `../schema/mts_schema.fbs` directly before writing a recommendation. Fixed
2026-08-12 — see `docs/ADR/execution_correctness_findings_spec.md`'s Findings 19/20/21, now marked
RESOLVED (C++ side) with the correct commit reference.

## What's still genuinely open (unchanged by this correction)

The C++ side is done. The `lbrnet` co-evolution specified in `risk_gate_context_wire_spec.md` §6
(read `RiskGateContext` in `context_stream.py`, delete the `PC-15/16/17` stopgaps in
`backtest_runner.py`, reintroduce the removed dim-9 gate) was never done — verified 2026-08-12, zero
commits touch either file for this. That's `lbrnet`-side work, tracked separately per this session's
scope boundary (MindfulTrader only).

## For future reference

Do not re-implement this doc's original "extend `AsymmetryContext`" recommendation — it's superseded
by the already-shipped `RiskGateContext` table. If revisiting this area, start from
`docs/ADR/risk_gate_context_wire_spec.md` and `docs/ADR/execution_correctness_findings_spec.md`
Findings 19-21, not from this file.
