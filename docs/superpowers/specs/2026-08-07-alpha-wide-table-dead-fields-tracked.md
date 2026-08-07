# `.alpha` Wide-Table Dead Fields — Tracked Gaps

## Origin

`lbrnet/scripts/validate_lbr_file.py` flagged five `TrainingEvent` wide-table fields at 100% zero across every `.alpha` replay this session, and in the pre-migration April 2026 baseline — confirming these predate the `indicator-manager-dod-soa` work and aren't a DOD regression: `RelRange`, `LongMacdNorm`, `TimeOfDayNorm`, `DailyBiasEncoded`, `ImpulseColor`.

Cross-referenced against `lbrnet`'s actual training pipeline (`lbrnet/data/fast_ingestor.py`'s named 29-dimension feature vector) to separate "dead and unused" from "dead and actively feeding a model." Two are the former; three are the latter and matter.

## Not consumed — no action needed

**`LongMacdNorm`, `DailyBiasEncoded`** — only referenced in `lbrnet/data/flatbuffer_stream.py`'s generic field-name-to-accessor reflection map, never as a named feature in `fast_ingestor.py`'s actual feature vector (which uses `long_fi13_norm`/`interm_fi2_norm` and `daily_bias` instead — both confirmed healthy). No evidence either is read anywhere as a real training input. Deprioritized.

## Consumed, real compute exists, root cause narrowed but not confirmed

**`RelRange`** — confirmed as the `'rel_range'` feature in `fast_ingestor.py`'s "Structural Anchors" group. Real, plausible-looking compute exists: `scsf_Screen3_KeltnerChannel` (`TripleScreen3.cpp`) computes `relRange = (High-Low)/ATR` once per bar and calls `ContextManager::SetRippleContext()`. `ContextManager::AddToTrainingEventFB()` reads it via `event.rel_range = ripple ? ripple->relRange : 0.0f;`, where `ripple = m_hasRippleContext ? &m_rippleContext : nullptr`.

Static analysis, fully exhausted this session, ruled out:
- Non-finite input rejection — `reject_ripple_nonfinite=0` for the entire session, across every replay.
- Once-per-bar guard bug — `lastPhysicsIndex` update verified correct, no persistent-variable-ID aliasing.
- `EventDataCollectorStudy.cpp` clobbering the value after the fact — confirmed it only calls `GetTrainingEventT()` and serializes the result verbatim, no field writes of its own.

Yet `has_ripple=0` in **every single** `ContextManager::FreshnessDigest` log line all session, from the first replay through the largest (340,000+ samples) — `m_hasRippleContext` has never once been observed true. Never-rejected + never-true, together, point at the containing code in `scsf_Screen3_KeltnerChannel` never executing at all — but *why* is unconfirmed; nothing else in the function structure explains a persistent skip given Amihud's compute (confirmed working, same function, later in the same tick) runs fine.

**Related but separate, found while investigating this**: `TripleScreen2.cpp` computes a *second*, TS2-side `relRange` (line ~771: `ctx.relRange = barRange / Array_AtrKeltner[sc.Index]`) that gets passed into `ContextManager::SetWaveContext()` — which explicitly discards it (`m_waveContext.relRange = 0.0f;`, comment: *"Strict ownership: ripple path owns relRange/velocity"*). This is intentional design, not a bug — flagging it so a future session doesn't mistake this deliberate discard for the actual defect. The genuinely-used `ObservationData.relative_range` (dim 2, confirmed healthy in `.context`) comes from a third, separate TS2 write site (`TripleScreen2.cpp:310`, `obs->mutate_relative_range(relRange)`) — unrelated to either of the above.

**Next step, not yet done**: add diagnostic logging inside `scsf_Screen3_KeltnerChannel` at the `SetRippleContext` call site (mirroring the pattern that resolved the Amihud zero-trap — see `TS3 AmihudDiag`/`TS3 AmihudInnerDiag` in `src/TripleScreen3.cpp`/`src/StudyHelperFunctions.cpp`), rebuild, redeploy, and check whether the call site is reached at all during a replay.

## Consumed, but no write path exists at all — semantics undecided, deferred

**`TimeOfDayNorm`, `ImpulseColor`** — confirmed as named features (`'time_of_day_norm'`, `'impulse_color'`) in the same `fast_ingestor.py` vector. Zero write sites found anywhere in the C++ codebase for either — this isn't a bug to fix, it's unwritten functionality.

No existing reference implementation to copy from. The one related hint — `AsymmetryContext.session_quality_score: float // TimeOfDayEnum [-1.0, 1.0] (Dual-Representation)` — is itself completely unwired (zero producers), so it's a naming convention suggesting intent, not a working example.

Explicitly deferred rather than guessed at, per user decision (2026-08-06/07 session):
- **`TimeOfDayNorm`** — open question: should this be a cyclical/continuous position-in-session-cycle encoding (e.g. sin/cos of minutes-since-open), or a hand-picked session-quality-score lookup table (matching the `session_quality_score` naming precedent)? Different formulas, not a refinement of the same one — needs a decision, not an implementation guess.
- **`ImpulseColor`** — open question: is a simple signed encoding of the existing, already-healthy `LongImp`/`IntermImp` enum classification sufficient, or is something else intended? Lowest-effort correct implementation exists (reuse existing compute) if the former is confirmed.

## Summary for whoever picks this up next

| Field | Status | Next action |
|---|---|---|
| `LongMacdNorm` | Unused | None |
| `DailyBiasEncoded` | Unused | None |
| `RelRange` | Used, root cause narrowed | Add diagnostic inside `scsf_Screen3_KeltnerChannel`, rebuild/redeploy/replay |
| `TimeOfDayNorm` | Used, unimplemented | Decide encoding (cyclical vs. lookup table), then implement |
| `ImpulseColor` | Used, unimplemented | Confirm signed-enum-encoding is correct, then implement (cheap if so) |
