# `.context` Emission Gate: Quality-Over-Quantity Correction

## 1. Origin

Found while implementing the offline `.context` generator's Task 8
(`docs/superpowers/plans/2026-09-08-market-data-replay-implementation.md`, `docs/superpowers/specs/
2026-09-08-offline-context-generator-spec.md`). Tracing `ContextManager::CheckAndTriggerHMM`'s real
body surfaced that the data-collection emission gate (`isDataCollection=true`, the branch
`EventDataCollectorStudy.cpp` always takes) does **not** use the Mahalanobis significant-change
metric (`ObservationTriggerGate::ComputeTriggerDecisionMetrics`) at all, even though that metric is
computed on every call. Confirmed via direct read of `ShouldTriggerHMM()` (`src/ContextManager.cpp:39`,
before this fix):

```cpp
inline bool ShouldTriggerHMM(
    bool hmm_initialized,
    bool significant_change,
    bool is_data_collection,
    bool any_observation_changed) {
    if (!is_data_collection) {
        return !hmm_initialized || significant_change;   // live-trading branch
    }
    // "Collection narrative contract: emit first sample and when any dimension changes."
    return !hmm_initialized || any_observation_changed;   // data-collection branch
}
```

`any_observation_changed` (`UpdateCollectionObservationTelemetry()`, `src/ContextManager.cpp:727`)
fires when ANY of the 18 scaled dims moves by more than `OBS_CHANGE_EPS=1e-5f` (energy dims) or
`OBS_CHANGE_EPS * OBS_GEOMETRY_CHANGE_MULT=3e-5f` (the rest) since the previous call —
`ContextManager.h:274-275`.

## 2. The problem: near-continuous, low-information emission

`1e-5f`/`3e-5f` are extremely small relative to the scaled observation's real range (per
`ObservationTriggerGate.h`'s own bounds table, e.g. `[-6,6]`, `[0,25]`, `[0,100]`). On a real,
continuously-varying tick feed, scaled values essentially never sit bit-identical between
consecutive ticks once `FeatureScaler` is warmed up — so in practice this gate fires on almost
every tick. The mechanism is a no-op filter in effect, not a meaningful one: `.context` emission is
closer to "record nearly everything" than "record genuine changes."

## 3. Why this is the wrong direction for training a Student-t HMM (literature grounding)

1. **Effective sample size collapses under autocorrelation.** Consecutive near-duplicate scaled
   observations are strongly autocorrelated; the *effective* information content of the resulting
   dataset is far smaller than its row count suggests (standard time-series statistics — see e.g.
   Kass & Raftery on effective sample size under serial correlation).
2. **Student-t emission parameters need genuine tail variation, not central density.** The
   degrees-of-freedom parameter ν (the entire reason to use Student-t over Gaussian emissions) is
   identified by genuine excursions into the tails, not many near-identical central observations.
3. **Transition-matrix sampling bias.** Non-uniform sampling density across calm vs.
   regime-changing periods inflates the apparent self-transition (persistence) probability relative
   to the true regime-switching rate.
4. **Already this codebase's own cited precedent**: `docs/superpowers/specs/2026-08-31-elite-
   feature-set-curation-initiative.md` §1 cites **Rydén, Teräsvirta & Åsbrink (1998)** as this
   system's own foundational financial-HMM precedent — built on *daily*, not intraday-tick, data —
   explicitly because "regime detection [is] a lower-frequency phenomenon than raw microstructure
   noise." Near-continuous tick-level emission is the opposite direction from that precedent.
5. **This repo has already built the recognized fix for this exact class of problem, just not
   applied here**: the information-driven-bars literature (AFML ch. 2; Easley/López de
   Prado/O'Hara) — already adopted in this repo as `ImbalanceBarEngine`/the activity-clock system —
   argues for sampling on genuine information content rather than "did anything move at all."

## 4. Decision (operator directive, 2026-09-08)

**Reuse the same Mahalanobis significant-change gate already used for live trading
(`isDataCollection=false`) for data collection too.** Both paths now apply the same
quality-over-quantity standard: emit on the first sample, or when the Mahalanobis distance from the
rolling baseline exceeds the existing trigger thresholds — not on any-dim-moved-by-epsilon.

This is a minimal, surgical change: `ObservationTriggerGate::ComputeTriggerDecisionMetrics()` is
**already computed on every call today**, regardless of `isDataCollection` (`CheckAndTriggerHMM`'s
Phase 5 runs unconditionally) — the fix only changes which computed value `ShouldTriggerHMM` reads,
not what gets computed.

**`UpdateCollectionObservationTelemetry()`'s per-dim change/staleness tracking is KEPT, not
removed** — its side effects (`m_staleRunLength`/`m_changeCount` feeding the periodic
"ObservationStaleness ALERT"/freshness-digest logging) are genuinely useful operational telemetry
(detecting a dim that's silently stuck/dead) independent of whether its return value governs
emission. Only the *gating* use of its return value is removed.

## 5. Code change

`ShouldTriggerHMM()` collapses to a single branch (both paths now agree):

```cpp
inline bool ShouldTriggerHMM(bool hmm_initialized, bool significant_change) {
    return !hmm_initialized || significant_change;
}
```

`is_data_collection`/`any_observation_changed` are dropped from the signature (no longer
consulted) — call site in `CheckAndTriggerHMM` updated to pass 2 args instead of 4.
`UpdateCollectionObservationTelemetry()`'s call site is unchanged (still runs, still logs), only its
return value is no longer threaded into `ShouldTriggerHMM`.

## 6. Downstream: offline `.context` generator (`tools/market_data_replay/`)

Task 8 of `docs/superpowers/plans/2026-09-08-market-data-replay-implementation.md` is updated to
match: reuse `ObservationTriggerGate` (the real Mahalanobis class) directly, the same as the
now-unified real gate, rather than the `any_observation_changed`/epsilon mechanism originally
planned before this correction.

## 7. What this does NOT change

- `FeatureScaler`'s own 500-sample warmup gate — unaffected, still required before any emission.
- The TS1/TS2 staleness/readiness gates (`AreTs1DimsReady`/`AreTs2StructuralDimsReady`) — unrelated,
  unaffected.
- `RiskGateContext`/`AsymmetryContext` construction — unaffected.
- Any lbrnet-side training code — this only changes what rows exist in future `.context` files, not
  how existing files are consumed.

## 8. Verification

- `./build_dll.sh` builds clean.
- No native unit test exercises `ShouldTriggerHMM`/`UpdateCollectionObservationTelemetry` directly
  today (`ContextManager.h` transitively includes `sierrachart.h`, not standalone-testable) — this
  change is verified by compilation + the offline generator's own native test suite exercising the
  now-shared `ObservationTriggerGate` logic instead.
