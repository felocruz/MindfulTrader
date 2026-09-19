# Live-Trading Log Observability + Data-Quality Findings (2026-09-17)

## 1. Origin

Operator ran `EventDataCollectorStudy.cpp` in a real Sierra Chart replay session and asked for an
assessment of `/mnt/c/Trading/logs/MindfulTrader.log`. This spec consolidates everything found and
fixed across that investigation (log volume, warmup mechanics, dead-code bugs) plus one still-open
data-quality finding that needs its own follow-up. Companion plan:
`docs/superpowers/plans/2026-09-17-live-trading-log-observability-and-data-quality-plan.md`.

## 2. Fixed this session

1. **`kTs1MacroObsLogMode` misconfigured ON** (`TripleScreen1.cpp`) — the constant's own comment
   said *"Keep disabled by default to avoid polluting runtime logs"* but the value was
   `IMPORTANT_ONLY`, not `OFF`. This one flag was responsible for ~64% of a 20,325-line log
   (`TS1 MacroObs digest/commit digest/quality-not-ready/stale dim`, all WS-09-investigation debug
   scaffolding never meant to ship on). Fixed: `OFF`. Confirmed post-deploy: zero "TS1 MacroObs"
   lines in the new replay's log.
2. **`EventDataCollectorStudy.cpp`'s LockA/LockB used a bare tick-count `% 250` modulo** — noisy
   (33.6% of the same log) and semantically wrong for a system that runs at variable-speed replay
   (this replay compressed ~12 real calendar days into a few real minutes — wall-clock throttling
   would be actively incorrect here) and at inconsistent tick density (raw tick count is a poor
   proxy for market time). New `include/BarKeyedLogThrottle.h`: a pure state-transition helper
   keyed on **bar index** (`sc.Index`, this study's own TS3 chart), not wall-clock or tick count —
   edge-triggered ENTERED/HEARTBEAT (exponential backoff, 1/2/4/.../256 bars)/CLEARED events.
   Confirmed post-deploy: a ~1.76M-block, 1,288-bar warmup produced exactly 13 log lines (1
   BLOCKED + 11 HEARTBEAT + 1 CLEARED) instead of the ~7,000 the old modulo would have produced,
   and is far more informative (`bars_blocked=N`, not a meaningless tick count).
3. **`IndicatorManager.cpp`'s `kRuntimeRegisteredIndicatorKeyValues`** — a third, purely
   duplicative hand-typed copy of the generated `IndicatorKey` registry, used only in a
   `static_assert` that verified nothing more than "two independently hand-typed lists agree with
   each other." Deleted; replaced with a row-count check against the same named constant
   `indicator_binding_policy_generated.h` already uses. (Broke the build this session as a side
   effect of an unrelated, legitimate generator fix — see the market-data-replay-alpha-generator
   spec §10 finding 3/4 for that thread.)
4. **`EventDataCollectorStudy.cpp`'s `model_confidence` was a permanent `0.0f` sentinel** — read
   from `TradeSignalManager`, whose only writer has zero callers anywhere. Fixed: now reads
   `InferenceManager::Instance().Prediction()->Confidence()`, the real live path (same one
   `PositionManager.cpp`'s own direction-conflict/staleness checks use). **Correction, verified
   empirically (2026-09-17)**: this is a structurally-correct fix (reads the right, non-dead
   source) but is NOT a practical fix for ordinary replay/data-collection sessions. Traced:
   `InferenceManager`'s `PredictionState` is written ONLY by `TradeExecutionServer.cpp:644`
   (`MutablePrediction()->SetPrediction(...)`), and `TradeExecutionServer::Instance().Initialize()`
   (which opens the ZMQ connection to a live Python Transformer) is called ONLY from
   `SCStudies.cpp`/`BackTesterStudy.cpp` — never from `EventDataCollectorStudy.cpp` itself. Built a
   throwaway native inspector (`/tmp/inspect_alpha.cpp`, reads the real `.alpha` FlatBuffer stream
   directly against this repo's current schema) and confirmed against a genuine 500,000-record
   sample from tonight's live replay session: **`model_confidence` is `0.0f` for every single
   record** — not because the fix failed, but because no live Transformer connection was active
   during that pure-collection session. The fix only has an observable effect in the (real, but
   narrower than originally claimed) scenario where `SCStudies.cpp` is ALSO running live trading
   in the same Sierra Chart process while `EventDataCollectorStudy.cpp` collects data concurrently
   — `InferenceManager` is a process-wide singleton, so a genuinely live prediction from live
   trading WOULD be visible to the collector in that case. **Withdrawing the earlier, overstated
   claim** that ".alpha streams collected going forward carry real per-tick confidence" as a
   general statement — it's conditional on a live trading connection being active, not automatic.
   **Does NOT extend to the offline `MarketDataReplayContext.cpp` tool**, which has no live
   Transformer connection to query at all — see `docs/HMM_REGIME_MANAGER_COORDINATION.md`
   Entries 9-11 for the cross-session exchange with `lbrnet` on this exact distinction.

All four fixes rebuilt clean (`build_dll.sh`), deployed (`deploy_mindfultrader.sh`), and directly
confirmed against a real post-deploy Sierra Chart replay log (not just compiled — actually
observed the new behavior firing correctly).

## 3. Confirmed working as a side effect: TS1's ~17-day warmup is real, not a bug

The FIRST replay session analyzed this evening never escaped TS1's macro-window warmup
(`sc.Index` capped at 45-49, `macro_window_n=100`) because its underlying data only spanned ~6 real
calendar days (2023-09-22 → 09-28). Zero `.context`/`.alpha` collection resulted the entire
session (`LockA blocks=1220042`, `LockB/C/D/E blocks=0`) — not a bug, a warmup precondition not yet
met. The SECOND (current) replay session's data range is wider: `sc.Index` reached 248 within the
first ~5 minutes, warmup cleared at bar 1288, and `EventDataCollector: Alpha collection active
(LockA=ready, LockB=ready, LockD=ready, LockE=ready, LockC=telemetry-warmup)` confirms real
collection is now happening. This matches the offline `MarketDataReplayContext.cpp` tool's own
already-documented finding (100 TS1 bars ≈ 16-17 real days) — now independently confirmed in live
production too, not just the offline tool.

## 4. RESOLVED (2026-09-17) — persistent FeatureScaler dominance collapse, dims 1 and 17

**Finding**: `ContextManager.cpp`'s own D2 sentinel-collapse diagnostic
(`FeatureScaler dominance ALERT dim=N ratio=R sampleCount=S`, fires when `dominanceRatio[i] > 0.30`
at each `RECALIBRATION_INTERVAL` checkpoint) fired for **dim 1 (`burstiness_index`) and dim 17
(`fast_mean_rev_z`) at `ratio=1.000000` continuously from `sampleCount=40000` through session end
at `sampleCount=330000`** — the entire replay, never resolving. The paired
`ContextManager::ObservationStaleness` log confirmed the SCALED value itself was frozen at exactly
`0.000000` with `changes=0` for the whole session — not merely dominated by a repeated value, but
provably never updated even once after warmup.

**Root cause, verified by direct trace, not FeatureScaler's fault**: `burstiness_index`/
`raschke_burst`'s raw source, `ContextManager::CalculateBurstinessIndex()` →
`eve::CalculateBurstinessIndex(m_eventTimestampsUS)`, has its own documented guard —
`if (n < kMinSamples=20) return 0.0f;` (`EventVelocityEngine.h:180`, "Poisson-neutral default —
insufficient data"). `m_eventTimestampsUS` is a buffer SHARED with `CalculateEventVelocity()`,
which is the ONLY function that ever appended to it
(`if (size>=MAX) pop_front(); push_back(now_us);`) — but `CalculateEventVelocity()` itself is
called only on the branch `syntheticVelocity < 0.0f` inside `CheckAndTriggerHMM()`'s Phase 1:
```cpp
float event_velocity = (syntheticVelocity >= 0.0f)
    ? syntheticVelocity
    : CalculateEventVelocity(now_us);   // <- buffer maintenance lived ONLY here
```
`EventDataCollectorStudy.cpp` — the ONLY caller in this entire trace — always passes a real,
non-negative `syntheticVelocity` (initialized `0.0f`, later an EMA state, always `>= 0.0f`), so
`CalculateEventVelocity()` is **never called** from data collection, meaning `m_eventTimestampsUS`
**never receives a single entry** in any `EventDataCollectorStudy.cpp`-driven session — live
data collection included, not just this replay. `CalculateBurstinessIndex()` therefore always took
its `n < 20` branch and returned the constant `0.0f`, forever. Live production's OWN
`SCStudies.cpp` path is unaffected (it omits `syntheticVelocity`, always taking the
`CalculateEventVelocity()` branch, which correctly maintained the buffer as a side effect) — this
bug was specific to the `.context`/`.alpha` TRAINING-DATA collection path, not live trading
decisions. Confirmed the offline `MarketDataReplayContext.cpp` tool does NOT share this bug — it
maintains its own `m_tickTimestamps` buffer unconditionally, per-tick, independent of any velocity
branch (`MarketDataReplayEngine.h:216`).

**Fix**: moved the shared buffer's maintenance (`pop_front`/`push_back`) out of
`CalculateEventVelocity()` (where it silently only ran on one branch) into `CheckAndTriggerHMM()`'s
own Phase 1, unconditionally, before the `syntheticVelocity` branch — so it now runs on every call
regardless of which velocity path is used. `CalculateEventVelocity()` itself is now a pure
EMA-velocity-return function operating on `m_velocityState`, no longer touching the shared buffer.
Verified: `build_dll.sh` clean, and the existing `tests/cpp/test_event_velocity_engine.cpp` suite
(16/16 checks, `CalculateBurstinessIndex`/`CalculateEventVelocity`'s own unit tests) still passes —
unaffected because the bug was in `ContextManager`'s call-site wiring, not in either tested
function's own internal logic, which is exactly why it went undetected until a real live-log trace
surfaced it.

**Verified against a fresh live replay, 2026-09-17 (post-deploy)**: `dim 1` (`burstiness_index`)
no longer shows `ratio=1.000000` -- it now varies genuinely (0.343, 0.303, 0.336, 0.313, ...) and
mostly stays below the 0.30 alert threshold, occasionally blipping just above it (normal variation,
not a collapse). The bar-clamped-timestamp caveat originally raised here did not materialize --
the fix produces real varying values, not merely a different degenerate constant. `LockA CLEARED
after 1736221 blocks over 1280 bars` → `Alpha collection active` also confirmed real collection
resumed correctly post-fix.

**Impact**: `burstiness_index` (`ObservationData` dim 1), `raschke_burst`
(`AsymmetryContext`/`RiskGateContext`) were very likely frozen at `0.0f` in EVERY
`EventDataCollectorStudy.cpp`-collected `.context`/`.alpha` file ever produced, live or replay,
until this fix — a genuine historical training-data-quality defect, not limited to tonight's
session. Out of scope for this spec to assess how much existing collected data is affected or
whether retraining is warranted — flagging for the operator's own judgment, not deciding here.

**`fast_mean_rev_z` (dim 17) re-checked against the SAME fresh replay -- still `ratio=1.000000`
continuously (sampleCount=115000 through 225000+), confirming it does NOT share dim 1's root
cause.** Traced directly: `mutate_fast_mean_rev_z` has zero call sites anywhere in `src/`
(grep-confirmed) -- this dimension is simply never written in live production at all, which is
why `FeatureScaler` correctly reports a constant raw input.

**CORRECTED (2026-09-17, after further verification): the decision to leave it unwired IS the
current, well-grounded institutional decision -- do not treat it as outdated or Gaussian-flawed.**
`docs/superpowers/specs/2026-09-18-predator-sniper-execution-architecture.md`'s
own per-dim ledger states plainly, dated 2026-09-04: *"both variants remain statistically
indistinguishable from a coin flip ... Decision: do not wire into production."* That test was a
forward-return/**hit-rate** test (binary direction classification, n=1,575,967, 95% CI width
~0.0016) — not a moment-based Gaussian statistic, so the "Gaussian metrics on fat-tailed dims"
concern does not clearly invalidate it, and it was specifically re-run to fix a staleness bug in
the test tool itself (confirming the null result fresh, post-formula-reformulation). The decision
was independently re-confirmed 3 days later: `docs/superpowers/specs/2026-09-06-imbalance-triple-
screen-architecture-spec.md` §1.1a (dated 2026-09-07) explicitly excludes it when enumerating
*"the 4 still-live dims"* for IS1/IS2/IS3 placement. **An earlier version of this section
(mis)characterized this as an open question and wired `ActivityClockMeanRevZ()` into
`ContextManager.cpp`'s live path — that change has been REVERTED** (confirmed via `git diff`
showing zero remaining references, and a clean `build_dll.sh` rebuild) after the operator caught
the contradiction with these two documented decisions.

**What IS still genuinely open, separate from the hit-rate finding**: the observation-vector doc
notes the wire-or-drop decision was originally *"blocked on cross-state HMM discrimination being
unmeasurable (model-staleness circularity)"* — meaning the HMM-based per-dim discrimination test
(this project's current preferred methodology, per `2026-09-09-market-data-replay-dim-selection-
spec.md` §0b/0c) has never actually been run for this specific dim. That is a real, distinct,
still-open question — but it requires an explicit operator decision to wire the dim specifically
to enable that measurement, overriding the hit-rate-based production decision for a different,
stated reason.

**RE-WIRED under explicit operator authorization, 2026-09-17 (Item 5)**: the operator explicitly
directed wiring `fast_mean_rev_z` for the sole stated purpose above (enabling the previously-
blocked HMM cross-state discrimination test), NOT as a re-litigation of the 2026-09-04 hit-rate
finding, which stands unchanged. Implemented in both paths so future `.context`/`.alpha`
collections (live and offline) carry real values instead of the `0.0f` sentinel:
- **Live**: `src/ContextManager.cpp`'s `BuildObservationVector()` — `ActivityClockMeanRevZ()` is
  now called every tick (100-bar activity-clock window, `include/ActivityClockMeanReversion.h`),
  writing `obs[OBS_FAST_MEAN_REV_Z]`/`m_localRiskContext.fastMeanRevZ`, with last-valid carry-
  forward for non-finite outputs and an explicit `0.0f` neutral default during warmup.
- **Offline**: `tools/market_data_replay/MarketDataReplayEngine.h` — same `ActivityClockMeanRevZ()`
  call added to the shared imbalance-bar-return compute block (mirroring the `fast_hurst_exponent`
  pattern exactly), in both the warmed-up (`count>=100`) and warmup-else branches. The dim was
  already present in `tools/market_data_replay/CandidateObservationDims.h`'s `kCandidateDims`
  array (comment there corrected from *"decided DROP"* to reflect the new authorization), so
  `ApplyOutDimZeroing()` required no functional change — only a stale-comment fix (it never
  zeroed this dim, since the array already listed it as IN, but its comment wrongly implied the
  dim was permanently unmutated).
- **Verified**: `./build_dll.sh --no-clean` clean rebuild (live DLL); offline
  `market_data_replay_context` binary rebuilt clean; offline engine's full native test suite
  (`test_market_data_replay_engine.cpp`) still `ALL PASS`, no regressions.
- **Verified against a fresh live replay, 2026-09-17 (post-deploy)**: `dim=17`
  (`fast_mean_rev_z`) no longer appears in EITHER `FeatureScaler dominance ALERT` (was
  `ratio=1.000000` continuously through 500K+ samples before this fix) or
  `ContextManager::ObservationStaleness ALERT` (was `dim17(value=0.000000, stale_run=<ever-
  growing>, changes=0)` in nearly every alert before this fix) — confirming it now varies
  normally, same as its 4 activity-clock siblings. `burstiness_index` (dim 1) also re-confirmed
  still varying correctly (`ratio` 0.30-0.39 range) in this same fresh session.
- **Offline path independently verified, same day**: ran the full 471.9M-tick
  `market_data_replay_context` replay to completion (476,745,947 ticks, 20,637,402 `.context`
  records, 73,239 `.alpha` records, RSS peaked ~3.2GB under an 8192MB budget — completed without
  truncating, unlike the prior pre-fix run which hit its 3072MB budget at 455M/471.9M ticks).
  Read back `fast_mean_rev_z` from the resulting `offline_replay_full_20260917.context.parquet`
  directly via `pyarrow`: 99.17% of rows nonzero (20,466,955/20,637,402), range `[0.0, 5.0]`
  (matches the clamped-score design), mean 1.09 — confirms the offline path independently of the
  live-log check above.

## 5. Cross-references

- `docs/superpowers/specs/2026-09-16-market-data-replay-alpha-generator-spec.md` §10 (findings 1-5)
  — the schema-generation and `model_confidence` fixes' full technical trace.
- `docs/HMM_REGIME_MANAGER_COORDINATION.md` Entries 8-11 — the `model_confidence` /
  `HmmState`/regime-fields cross-session exchange with `lbrnet`.
- `/memories/repo/market_data_replay_dim_selection.md` — `fast_mean_rev_z`'s DROP status.
- `docs/superpowers/plans/2026-08-12-statistical-context-relrange-sentinel-gap.md` — the original
  sentinel-collapse precedent this diagnostic was built to catch (dims 1/2 at the time).
