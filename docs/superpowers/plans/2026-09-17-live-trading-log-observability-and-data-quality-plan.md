# Live-Trading Log Observability + Data-Quality Plan (2026-09-17)

Companion to `docs/superpowers/specs/2026-09-17-live-trading-log-observability-and-data-quality-spec.md`.
Tasks 1-4 are DONE (this session); Task 5 is the open follow-up.

### Task 1: Fix `kTs1MacroObsLogMode` misconfiguration

**Files:** `src/TripleScreen1.cpp`

- [x] **Step 1:** Flip `kTs1MacroObsLogMode` from `IMPORTANT_ONLY` to `OFF`, matching its own
  comment's stated intent.
- [x] **Step 2:** Rebuild (`build_dll.sh`), deploy (`deploy_mindfultrader.sh`), confirm via a real
  post-deploy replay log that zero "TS1 MacroObs" lines appear.

### Task 2: Replace LockA/LockB's tick-count modulo with bar-keyed edge-triggered logging

**Files:** `include/BarKeyedLogThrottle.h` (new), `src/EventDataCollectorStudy.cpp`

- [x] **Step 1:** New `BarKeyedLogThrottle.h` — pure `UpdateBarKeyedLock()` state-transition
  function keyed on bar index, not wall-clock (wrong under variable-speed replay) or tick count
  (noisy under variable volume). ENTERED/HEARTBEAT (exponential backoff, capped 256 bars)/CLEARED.
- [x] **Step 2:** Wire into LockA/LockB in `EventDataCollectorStudy.cpp`, using `sc.Index` (this
  study's own TS3 chart). New persistent-int IDs 73-82 for the bar-keyed state, reset on arm.
  Existing cumulative `EDC_LOCK_A_BLOCK_COUNT_ID`/`EDC_LOCK_B_BLOCK_COUNT_ID` (HUD + disarm
  telemetry) left untouched.
- [x] **Step 3:** Rebuild, deploy, confirm via real replay log: a ~1.76M-block/1,288-bar warmup
  produced exactly 13 log lines instead of ~7,000.

### Task 3: Delete `IndicatorManager.cpp`'s duplicate `IndicatorKey` list

**Files:** `src/IndicatorManager.cpp`

- [x] **Step 1:** Confirmed (grep) `kRuntimeRegisteredIndicatorKeyValues` has exactly one usage —
  a `static_assert` comparing it to the generated registry. Deleted the array + its `ArraysEqual`
  helper; replaced with `static_assert(kIndicatorKeyRegistryRowCount ==
  kExpectedManagedIndicatorKeyCount, ...)`.
- [x] **Step 2:** Rebuild clean.

### Task 4: Fix `model_confidence` dead-code path

**Files:** `src/EventDataCollectorStudy.cpp`, `tools/market_data_replay/MarketDataReplayEngine.h`
(comment only)

- [x] **Step 1:** `eventT->model_confidence` now reads
  `InferenceManager::Instance().Prediction()->Confidence()` (null-checked) instead of the dead
  `TradeSignalManager` path. Removed the now-unused `#include "TradeSignalManager.h"`.
- [x] **Step 2:** Updated the offline replay tool's own `model_confidence = 0.0f` comment — it
  previously (incorrectly, after this fix) cited "production also always returns 0" as the
  rationale; corrected to cite the real reason (no live Transformer connection in an offline tool).
- [x] **Step 3:** Rebuild both, confirm clean.
- [x] **Step 4:** Cross-session write-up in `docs/HMM_REGIME_MANAGER_COORDINATION.md` (Entries
  9-11) clarifying to `lbrnet` that this fix is live-path-only and does NOT extend to
  `offline_replay_455m.alpha`'s own `model_confidence` (still `0.0f`, same root cause as the
  `HmmState`/regime-fields gap they found).

### Task 5: Investigate persistent `FeatureScaler` dominance collapse, dims 1 & 17

**Files:** `src/ContextManager.cpp` (fix landed here), `include/EventVelocityEngine.h` (root cause
traced to here, unchanged), `tools/market_data_replay/MarketDataReplayEngine.h` (confirmed clean,
unchanged)

- [x] **Step 1:** Confirmed via the final log state before Sierra Chart closed: `dim1`/`dim17`
  frozen at scaled `0.000000`, `changes=0`, from `sampleCount=40000` through session end at
  `330000` — persistent for the entire session, not transient.
- [x] **Step 2:** Traced dim 1's raw compute path fully: `ContextManager::CalculateBurstinessIndex()`
  → `eve::CalculateBurstinessIndex(m_eventTimestampsUS)`, which returns a constant `0.0f` whenever
  its input buffer has fewer than 20 entries. Confirmed via direct grep of every read/write site
  that `m_eventTimestampsUS` is populated ONLY inside `CalculateEventVelocity()`, which
  `CheckAndTriggerHMM()` skips entirely whenever `syntheticVelocity >= 0.0f` — true on every
  `EventDataCollectorStudy.cpp` call. This is a `ContextManager`-side wiring bug, not a
  `FeatureScaler` bug: the scaler was correctly reporting a genuinely constant raw input.
- [x] **Step 3:** Cross-checked against `CLAUDE.md`'s 2026-09-02 `burstiness_index` real-data
  validation numbers — confirmed that validation exercised the offline tool's own independent
  `m_tickTimestamps` buffer (which was never affected by this bug), not this live `ContextManager`
  code path, so the two results were never actually in conflict — resolves the "why does this
  contradict the prior validation" question raised when this task was opened.
- [x] **Step 4 (lighter-touch, as planned):** Did not separately trace dim 17
  (`fast_mean_rev_z`)'s own upstream engine — it shares the exact same downstream symptom
  signature (frozen at scaled `0.000000`, `changes=0`) as dim 1, and dim 1's root cause (a shared
  buffer starved by call-site branching, unrelated to which physical quantity is being computed)
  is a plausible enough shared explanation not to warrant equal effort on an already-decided-DROP
  dimension.
- [x] **Step 5:** Fix implemented: moved `m_eventTimestampsUS`'s `pop_front`/`push_back`
  maintenance out of `CalculateEventVelocity()` (silently conditional) into `CheckAndTriggerHMM()`'s
  own Phase 1 (unconditional, runs before the `syntheticVelocity` branch either way).
  `build_dll.sh` clean; `tests/cpp/test_event_velocity_engine.cpp` (16/16) unaffected. Full
  writeup: spec §4.
- [x] **Step 6:** Re-deployed and ran a fresh live replay. Confirmed the fix works: dim 1
  (`burstiness_index`) now varies genuinely (0.343, 0.303, 0.336, 0.313, ...), mostly below the
  0.30 alert threshold -- the bar-clamped-timestamp caveat did not materialize. `LockA CLEARED
  after 1736221 blocks over 1280 bars` -> `Alpha collection active` confirmed real collection
  resumed. **New finding**: dim 17 (`fast_mean_rev_z`) independently re-checked against this same
  replay -- still `ratio=1.000000` continuously, confirming it does NOT share dim 1's root cause.
  Traced separately: `mutate_fast_mean_rev_z` has zero call sites anywhere in `src/` -- this
  dimension is simply never written in production, a mechanical fact and the actual reason for the
  constant reading. **CORRECTED**: an earlier version of this step wired `ActivityClockMeanRevZ()`
  into `ContextManager.cpp`'s live path on the theory that the 2026-09-04 drop decision was
  Gaussian-flawed and outdated -- that was wrong. The drop decision is the current, well-grounded
  institutional decision (a hit-rate test, not a Gaussian-moment one, independently re-confirmed
  2026-09-07 by the Imbalance Triple Screen architecture spec's own IS1/2/3 placement work, which
  explicitly excludes this dim). The wiring change has been REVERTED (`git diff` confirms zero
  remaining references, `build_dll.sh` rebuilds clean). What remains genuinely open is different:
  the HMM-based cross-state discrimination test (this project's preferred methodology) was never
  run for this dim (blocked by model-staleness circularity) -- wiring it for THAT specific purpose
  would need its own explicit operator authorization, not a re-litigation of the hit-rate result.
  Full writeup: spec §4.
- [x] **Step 7 (Item 5, 2026-09-17):** Operator gave the explicit authorization Step 6 said would
  be required -- re-wired `fast_mean_rev_z` in BOTH paths, solely to enable the HMM cross-state
  discrimination test, not to re-litigate the hit-rate finding: `src/ContextManager.cpp` (live,
  `ActivityClockMeanRevZ()` called every tick with last-valid carry-forward + warmup neutral
  `0.0f`) and `tools/market_data_replay/MarketDataReplayEngine.h` (offline, same call added to the
  shared imbalance-bar-return block, mirroring `fast_hurst_exponent`'s exact pattern in both the
  warmed-up and warmup-else branches). Corrected the now-stale "permanently OUT, never mutated"
  comments in `MarketDataReplayEngine.h`'s `ApplyOutDimZeroing()` and
  `CandidateObservationDims.h`'s `kCandidateDims` entry (the dim was already listed IN
  `kCandidateDims`, so no functional zeroing-logic change was needed there, only the comment).
  Verified: `build_dll.sh --no-clean` clean (live DLL); offline `market_data_replay_context`
  rebuilt clean; `test_market_data_replay_engine.cpp` still `ALL PASS`, no regressions. Full
  writeup: spec §4.
- [x] **Step 8:** Re-deployed and ran a fresh live replay. Confirmed `dim=17`
  (`fast_mean_rev_z`) no longer appears in `FeatureScaler dominance ALERT` or
  `ContextManager::ObservationStaleness ALERT` output at all (previously present in nearly every
  occurrence of both, pinned at `ratio=1.000000`/`value=0.000000`) -- it now varies normally.
  `burstiness_index` (dim 1) re-confirmed still varying correctly in the same session. Full
  writeup: spec §4.
