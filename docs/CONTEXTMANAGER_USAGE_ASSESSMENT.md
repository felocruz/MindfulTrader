# ContextManager Usage Assessment (Current Architecture)

**Date**: March 7, 2026
**Scope**: `EventDataCollectorStudy.cpp`, `SCStudies.cpp`, `TripleScreen2.cpp`, `TripleScreen3.cpp`
**Status**: Aligned to final Wave/Ripple ingress design

---

## Executive Summary

| Study | ContextManager Usage | Assessment | Grade |
|-------|----------------------|------------|-------|
| `EventDataCollectorStudy.cpp` | `CheckAndTriggerHMM(...)`, `AddToTrainingEventFB(...)` | Correct orchestration and collection flow | A |
| `SCStudies.cpp` | `CheckAndTriggerHMM(...)` in live path | Correct live orchestration boundary | A |
| `TripleScreen2.cpp` | `SetWaveContext(std::move(ctx))` | Correct TS2 ownership of Wave fields | A |
| `TripleScreen3.cpp` | `SetRippleContext(std::move(ctx))`, `SetNormalizedAnchors(std::move(anchors))` | Correct TS3 ownership and anchor feed | A |

---

## Verified Current Call Sites

### TripleScreen2 (TS2 / 60m)
- Uses `ContextManager::Instance().SetWaveContext(std::move(ctx));`
- Role: owner of Wave statistical fields (`volatility`, `efficiency`)

### TripleScreen3 (TS3 / 15m)
- Uses `ContextManager::Instance().SetRippleContext(std::move(ctx));`
- Uses `ContextManager::Instance().SetNormalizedAnchors(std::move(anchors));`
- Role: owner of Ripple statistical fields (`relRange`, `velocity`) and normalized anchor inputs

### EventDataCollectorStudy
- Uses `ContextManager::Instance().CheckAndTriggerHMM(now_us, true, syntheticVelocity);`
- Uses `ContextManager::Instance().AddToTrainingEventFB(*eventT, sc);`
- Role: data collection triggering and training payload enrichment

### SCStudies
- Uses `ContextManager::Instance().CheckAndTriggerHMM(now_us, false);`
- Role: live orchestration trigger path

---

## Architecture Compliance Checks

1. No production usage of legacy `SetStatisticalContext(...)`.
2. Statistical ingress is explicitly split:
   - TS2 -> `SetWaveContext(...)`
   - TS3 -> `SetRippleContext(...)`
3. Normalized anchor ingress remains active from TS3.
4. HMM trigger boundary remains centralized at `CheckAndTriggerHMM(...)`.

---

## Notes

- This file supersedes older February 2026 assessment text that referenced the removed generic statistical ingress API.
- For full design and hardening roadmap, use `docs/ROADMAP_CONTEXTMANAGER_REFACTOR.md`.
- ContextManager trigger behavior is upstream orchestration only; Student-t model acceptance and sign-off gates (Gang, stability veto, run-class rules) are governed by [lbrnet/docs/RUNBOOKS/STUDENT_T_HMM_STRICT_RUNBOOK.md](lbrnet/docs/RUNBOOKS/STUDENT_T_HMM_STRICT_RUNBOOK.md).
- Producer-side context composition and lbrnet model-space dimensionality are intentionally distinct; lbrnet training consumes strict canonical ObservationData model-space (16D).
