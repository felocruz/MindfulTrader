# EDC Audit: Why Observation Dims 0, 6, 8 Export as Zero

Date: 2026-08-11
Scope: EventDataCollectorStudy path (.context export), ContextManager TS1 readiness gate, TS1 macro writer

## Findings

### F1 (Critical): EventDataCollector reset can re-arm stale TS1 snapshot values (including bootstrap zeros) as fresh

Evidence:
- EventDataCollector arms and calls ContextManager reset with current replay-safe timestamp: src/EventDataCollectorStudy.cpp:311
- ContextManager reset preserves TS1 snapshot if finite and seen-before-reset: src/ContextManager.cpp:1348, src/ContextManager.cpp:1412
- Reset then writes preserved dim0/dim6/dim8 back into ObservationData and sets TS1 freshness timestamp to reset reference time ("now"): src/ContextManager.cpp:1413

Impact:
- If preserved TS1 values are bootstrap placeholders (0/0.5/0) or stale, reset makes them appear newly fresh.
- AreTs1DimsReady then passes for up to 6 hours based on synthetic freshness, allowing CheckAndTriggerHMM to export .context rows with dim0/dim6/dim8 effectively frozen at low-information values.

### F2 (High): TS1 freshness contract only checks finiteness, not statistical readiness/quality

Evidence:
- TS1 writer marks commit/fresh whenever dim0/dim6/dim8 are finite: src/TripleScreen1.cpp:532, src/TripleScreen1.cpp:622
- Log variance and Fisher explicitly return 0 during insufficient history warmup: src/StudyHelperFunctions.cpp:3061, src/StudyHelperFunctions.cpp:3098
- Hurst fallback may use neutral fallback when history is insufficient (still finite), enabling early fresh mark with non-informative values.

Impact:
- TS1 freshness can be asserted before TS1 metrics are statistically valid.
- Combined with F1, this can seed reset snapshots with weak placeholders that later propagate into EDC .context exports.

### F3 (High): EventDataCollector path does not own/recompute TS1 macro metrics but relies on external TS1 writers without strict pre-arm validity gate

Evidence:
- EDC calls CheckAndTriggerHMM directly on TS3 path: src/EventDataCollectorStudy.cpp:491
- TS1 macro dims are expected from external writer (TripleScreen1) via shared ContextManager state.
- No hard arm-time assertion that TS1 has recent, non-placeholder writes from active TS1 context before allowing context stream maturation.

Impact:
- Running EDC without properly active TS1 producer context can leave dims 0/6/8 stale/frozen.
- Operator sees zeros in .context despite active EDC collection.

## Task Plan

### Task 1 (Critical): Fix reset freshness semantics for preserved TS1/TS2 snapshots

Goal:
- Preserve values if needed, but do not mint synthetic freshness timestamps from reset time.

Changes:
- In ContextManager::Reset, when restoring TS1/TS2 snapshots, keep original last-write timestamps (savedTs1WriteUs/savedTs2WriteUs) or mark as not-seen depending on policy.
- Do not use reset_reference_time_us as freshness write time for preserved snapshots.

Acceptance:
- After arm/reset with no subsequent TS1 write, AreTs1DimsReady becomes false once true age exceeds threshold based on real write time (not reset time).
- EDC logs TS1 lock wait/reject instead of exporting stale TS1 dimensions.

### Task 2 (High): Introduce TS1 statistical-readiness gate before MarkTs1MacroDimsFresh

Goal:
- TS1 freshness should represent valid macro signal availability, not just finite numbers.

Changes:
- In TripleScreen1 writer block, require minimum history and contract checks before freshness mark.
- Suggested gate:
  - history sufficient for macro and fisher windows,
  - finite dims,
  - dim0 and dim8 not from warmup sentinel path.
- If gate fails, skip MarkTs1MacroDimsFresh and emit sparse diagnostic.

Acceptance:
- Early replay warmup no longer marks TS1 ready using placeholder metrics.
- First TS1 fresh mark occurs only after minimum bar history is satisfied.

### Task 3 (High): Harden AreTs1DimsReady to reject placeholder TS1 state

Goal:
- Prevent finite-but-non-informative TS1 values from passing readiness.

Changes:
- Add readiness qualifiers in AreTs1DimsReady for dim0/dim6/dim8 quality.
- Prefer explicit producer-side readiness flag (set by TS1 writer after true warmup) over implicit numeric heuristics.

Acceptance:
- With only placeholder TS1 values and no true TS1 writer readiness, CheckAndTriggerHMM rejects TS1 as unready.

### Task 4 (Medium): Add EDC arm-time prerequisite checks for TS1/TS2 ownership

Goal:
- Fail fast on miswired chartbook instead of collecting low-integrity context.

Changes:
- On arm, verify TS1 and TS2 freshness + readiness contracts.
- If not satisfied, block arming with actionable log message (which chart/study is missing or stale).

Acceptance:
- EDC cannot enter collection mode when TS1/TS2 producers are absent/unready.

### Task 5 (Medium): Add explicit zero-trap regression tests/log assertions

Goal:
- Prevent recurrence.

Changes:
- Add unit/integration coverage around reset + readiness gating behavior.
- Test case: TS1 writes placeholders, arm/reset, no new TS1 writes, then CheckAndTriggerHMM in collection mode must not emit TS1-ready context.
- Add diagnostic counters for "placeholder rejected" and "reset-restored stale snapshot rejected".

Acceptance:
- CI-level test reproduces pre-fix failure and passes post-fix.

## Suggested Execution Order
1. Task 1
2. Task 2
3. Task 3
4. Task 4
5. Task 5

## Notes
- This issue is specific to EventDataCollector runtime behavior and reset semantics, not schema mapping.
- Observation index mapping for dims 0/6/8 is correct; the failure is lifecycle and readiness governance.
