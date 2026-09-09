# Real-Codebase Findings from the Offline `.context` Generator Work

## 1. Origin

While building/validating `tools/market_data_replay/` (the offline `.context` generator,
`docs/superpowers/plans/2026-09-08-market-data-replay-implementation.md`), several findings
surfaced about the ACTUAL existing codebase (`src/ContextManager.cpp`/`src/TripleScreen2.cpp`), not
just the offline replica. This doc separates what's a genuine bug candidate worth fixing in the real
code from what's a confirmed-correct existing behavior the offline tool simply needed to match.

## 2. Bug candidate: `regime_tenure` counts TICKS, not BARS (semantic mismatch)

**Location**: `ContextManager::SetWaveContext()` (`src/ContextManager.cpp:85-127`), fed by
`TripleScreen2.cpp:751-817`'s `StatisticalContext` block.

**The mismatch**: `m_regimeTenureCounter` is documented, in two separate places, as counting BARS:
- `include/ContextManager.h:466`: `int m_regimeTenureCounter = 0; ///< Incremented each update;
  reflects bars in current regime state`
- `src/ContextManager.cpp`'s own `AddToTrainingEventFB()`: `event.regime_tenure = wave->regimeTenure;
  ///< TS2 bar-closes in current regime`

But the actual increment/reset happens inside `SetWaveContext()`, which is called **every tick**,
unconditionally — `TripleScreen2.cpp:751`'s `if (sc.Index >= 20) { ... ContextManager::Instance()
.SetWaveContext(std::move(ctx)); }` has no `sc.Index != lastProcessedIndex`-style bar-close guard
anywhere in the surrounding code (confirmed by direct read, not assumed — same class of finding as
the offline generator's own Task 6b cadence audit, which found 8 sibling dims in this exact file
were ALSO genuinely per-tick-reactive, just correctly so for those). Concretely: `m_prevEfficiency`/
`m_prevVolatility` (the "last regime" baseline the tenure comparison is made against) are
overwritten on every tick, not just at bar close — so the counter is tracking consecutive
TICKS-without-a->15%-change, not consecutive BARS-without-a->15%-change, despite its own name and
doc comments.

**Why this matters, not just a naming nitpick**: on a real, high-tick-density feed (thousands of
ticks per 60-minute TS2 bar in a liquid market), if intra-bar efficiency/volatility noise
occasionally exceeds the 15% relative-change threshold (very plausible — the denominators
`max(prevEfficiency, 0.1f)`/`max(prevVolatility, 0.0001f)` are small floors, so modest absolute
noise can produce large relative swings), `regime_tenure` could reset far more often than the
"bars in current regime" semantic implies, making the reported tenure value systematically SMALLER
than a genuine bar-level measurement would show — the training-data field (and any consumer reading
`SystemState.bars_since_last_update`) may not mean what its name says.

**Confirmed empirically, same mechanism, different codebase**: the offline generator's own Task 6b
test (`tools/market_data_replay/test_market_data_replay_engine.cpp`) hit exactly this — a noisy
synthetic tick fixture reset regime tenure to 1 on nearly every tick, requiring the test to add a
30-bar calm tail before a genuine reset could even be distinguished from the noise floor. This isn't
proof the same happens on real tick data (a synthetic fixture isn't real market noise), but it
demonstrates the mechanism is real and sensitive, not theoretical.

**Not yet fixed here** — this doc records the finding per operator request; deciding whether the fix
is (a) gate the comparison to bar-close only (matching the documented semantic), (b) rename the
field/comments to honestly describe tick-level tracking, or (c) something else, is a separate,
future decision, not made in this session.

## 3. Confirmed NOT a bug (checked, matches real behavior already): `AllDimsReady()`-style sequential warm-up

The offline generator's Task 10 (`docs/superpowers/plans/2026-09-08-market-data-replay-implementation.md`)
found that gating emission on `FeatureScaler`'s own 500-sample warmup, placed AFTER a check that
every dim's own rolling window is filled, means `FeatureScaler`'s sample counter doesn't even start
incrementing until the slower per-dim requirements (TS1's 100-bar window especially) are satisfied —
the two warm-ups are sequential, not concurrent.

**Checked against the real code and confirmed this already matches `ContextManager::CheckAndTriggerHMM()`'s
own real body** (`src/ContextManager.cpp`): `AreTs1DimsReady()`/`AreTs2StructuralDimsReady()` are
checked and hard-`return` BEFORE `BuildObservationVector()`/`m_featureScaler.UpdateAndNormalize()`
are ever called — so the real system already has this exact sequential-warm-up characteristic. Not
a new bug; the offline tool now correctly replicates existing behavior. Recorded here only so this
doesn't get independently "discovered" and treated as a new finding later — the real system needs
~16+ days of elapsed session time (TS1's 100 240-minute bars) before `FeatureScaler`'s own 500-sample
counter even begins, meaning a fresh reset/redeploy has a long runway before becoming fully
operational. Worth knowing, not necessarily worth changing.
