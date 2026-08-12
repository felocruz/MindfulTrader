# Spec: EDC TS1 Ready-Contract Hardening (Dims 0/6/8 Zero-Export Fix)

Date: 2026-08-11
Owner: C++ execution layer (MindfulTrader)
Scope: `ContextManager`, `TripleScreen1`, `EventDataCollectorStudy`

## Problem Statement

During `EventDataCollectorStudy` collection, ObservationData dims `0` (log_variance_ratio), `6` (hurst_exponent), and `8` (fisher_info) can remain at placeholder values and still be treated as TS1-ready, producing low-integrity `.context` records.

## Root Causes

1. Reset path could restore TS1 snapshot values and re-arm freshness semantics for current epoch.
2. TS1 freshness was based on finiteness, not statistical readiness.
3. EDC arm flow lacked a hard preflight gate for TS1/TS2 producer readiness.

## Design Decisions

### D1. TS1 readiness requires explicit quality-qualified post-reset writes

- Introduce explicit TS1 quality-ready state in `ContextManager`.
- `AreTs1DimsReady` requires:
  - seen-after-reset
  - quality-ready-after-reset
  - finite dims 0/6/8 and cached dim9
  - in-contract bounds for dims 0/6/8
  - freshness age within max threshold (unless weekend grace bypasses freshness only)

### D2. Reset restores values only, never synthetic freshness

- `Reset()` may restore snapshot values to avoid abrupt null state.
- `Reset()` must not mark TS1/TS2 as seen/fresh post-reset.
- Producer ownership must re-earn readiness via new writes.

### D3. TS1 writer marks freshness with quality contract

- TS1 writes dim0/dim6/dim8 as before when finite.
- Freshness mark includes `quality_ready` status derived from:
  - sufficient history for macro and Fisher windows
  - finite dims
  - no insufficient-history Hurst fallback path

### D4. EDC arm preflight hard-gates TS1/TS2 ownership readiness

- Arm-time preflight computes and logs readiness for TS1/TS2.
- If either is unready, arming is blocked with actionable errors.

## Implementation Summary

- `include/ContextManager.h`
  - `MarkTs1MacroDimsFresh(uint64_t, bool quality_ready=true)`
  - `HasTs1QualityReadyAfterReset()`
  - new atomic state: `m_ts1QualityReadyAfterReset`
  - reset contract doc clarified: value-restore only; freshness re-earned.

- `src/ContextManager.cpp`
  - `MarkTs1MacroDimsFresh` stores quality flag.
  - `AreTs1DimsReady` now enforces quality flag and dim0/6/8 contract bounds.
  - TS1 rejection log includes `quality_ready` for root-cause visibility.
  - `Reset` no longer re-arms TS1/TS2 freshness/seen flags when restoring snapshots.
  - reset log adds `ts1_freshness_rearmed=0 ts2_freshness_rearmed=0`.

- `src/TripleScreen1.cpp`
  - TS1 macro diagnostics switched to `IMPORTANT_ONLY`.
  - Added TS1 quality-ready contract and explicit fallback detection.
  - `MarkTs1MacroDimsFresh(..., ts1QualityReady)` used to propagate readiness semantics.
  - Digest now reports quality-ready vs quality-reject counters.

- `src/EventDataCollectorStudy.cpp`
  - Preflight now logs TS1/TS2 ownership state, ages, and key dim values.
  - Preflight blocks arming when TS1 or TS2 ownership readiness is not satisfied.

## Smart Logging Plan

### TS1 producer logs

- `TS1 MacroObs quality-not-ready (...)`
  - includes index, history gate, fallback flag, windows, dim0/6/8 values.
- `TS1 MacroObs commit digest ... quality_ready=... quality_reject=...`

### Context manager logs

- `ContextManager::CheckAndTriggerHMM SKIP: TS1 macro dims unavailable/stale ... quality_ready=...`
- `ContextManager::Reset HARD ... ts1_freshness_rearmed=0 ts2_freshness_rearmed=0`

### EDC preflight logs

- `EDC PREFLIGHT OWNERSHIP: ts1_ready=... ts1_quality_ready=... ts1_age_us=... ts1_dim0=... ts1_dim6=... ts1_dim8=... ts2_ready=...`
- hard errors if TS1/TS2 are not ready before arming.

## Acceptance Criteria

1. Arm EDC immediately after reset with no new TS1 write:
   - Preflight must fail with TS1 ownership error.
2. After sufficient TS1 history and quality-qualified writes:
   - Preflight must pass TS1 gate and report `ts1_quality_ready=1`.
3. CheckAndTriggerHMM must not emit TS1-ready path while TS1 quality-ready flag is false.
4. `.context` export should no longer show persistent dim0/6/8 placeholders once preflight passes.

## Rollback Strategy

- If operationally too strict for legacy chartbooks, keep logic and temporarily relax only the EDC arm hard gate to warning mode while preserving ContextManager readiness hardening.
