# Activity-Clock Treatment for `mean_rev_z`, `hurst_exponent`, `recurrence_rate` — Implementation Plan

> **STATUS: DESIGN COMPLETE. Task 1 (`recurrence_rate` replacement) IMPLEMENTED and verified
> 2026-08-28** (full `./build_dll.sh --no-clean` succeeds; `test_recurrence_rate_engine` extended +
> passing; `test_rqa_epsilon`/`test_feature_scaler`/`test_sevcik_fractal_dimension` regression-pass).
> Tasks 2-8 (`mean_rev_z`/`hurst_exponent` additive twins) NOT yet implemented. **Real design detail
> found during implementation, not anticipated by the original plan text**: `ContextManager::
> AreTs2StructuralDimsReady()` reads `m_observationData.recurrence_rate()` directly (not the local
> `obs[]` scratch array) as part of a live HMM-trigger readiness gate — since `TripleScreen2.cpp` no
> longer mutates that field, the new activity-clock block must also call
> `m_observationData.mutate_recurrence_rate(...)` to keep that gate's read live, in addition to
> setting `obs[OBS_RECURRENCE_RATE]` for the actual HMM feature vector. This is a deliberate
> deviation from `skewness_idx`'s precedent (which left `m_observationData.skewness_idx()` stale,
> harmless there since nothing reads it directly) — `recurrence_rate` has a real reader of the raw
> wire field itself, not just the scaled feature vector. `AreTs2StructuralDimsReady()`'s own
> finite/in-range logic was left unchanged (not renamed/refactored) since keeping the field fresh
> was the lower-risk fix.

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give `mean_rev_z` and `hurst_exponent` activity-clock twins (mirroring `fast_taleb_kurtosis`'s already-shipped additive-twin pattern) and replace `recurrence_rate`'s time-bar computation with an activity-clock one in place (mirroring `skewness_idx`'s already-shipped replacement pattern) — per §5a's literature-grounded, per-dim additive-vs-replace decision, which is **not symmetric across the three dims** and must not be treated as one mechanical change applied three times.

**Spec:** `docs/superpowers/specs/2026-08-25-observation-vector-institutional-hardening-spec.md` §5a (why — Clark 1973/Ané & Geman 2000/AFML ch.2 for `mean_rev_z`/`hurst_exponent`; RQA-on-RR-intervals precedent for `recurrence_rate`) and §8/§9 (acceptance gates, residual risk). This plan is the "future implementation plan" §5a's own "Not decided by this section" paragraph and §7's non-goals explicitly deferred to.

**Precedent this plan follows exactly:** `docs/superpowers/plans/2026-08-26-activity-clock-dual-kurtosis.md` (the executed, shipped design for `fast_taleb_kurtosis`) and the `skewness_idx` in-place replacement (`ContextManager.cpp` commit `7c51f33`). **Real-world correction to the kurtosis plan's own Task 6, verified 2026-08-27**: twin fields go directly onto `ObservationData` as new struct fields ("keep the struct, edit fields in place" — `fast_taleb_kurtosis` is `ObservationData`'s 17th field, not a separate `Event`/`HMM_OBSERVATION_EXTENSIONS` table as originally drafted). This plan's new fields (`fast_mean_rev_z`, `fast_hurst_exponent`) follow the **real, shipped** pattern, not the superseded original draft.

## 0. Scope — read before Task 1, this is not symmetric across the three dims

| Dim | Live gate consumer(s) protecting an existing calibration | Treatment | New schema field? |
|---|---|---|---|
| `mean_rev_z` | `Scoring.cpp:305`, `isMeanReversionPattern && ctx.meanRevZ > 2.0f` | **ADDITIVE TWIN** (kurtosis pattern) | Yes — `fast_mean_rev_z` |
| `hurst_exponent` | `Scoring.cpp:266`, `(isDirectionalPattern \|\| pattern == PatternType::Unknown) && ctx.hurstExponent > 0.70f` — confirmed this reads `LocalRiskContext.hurstExponent`, which `ContextManager.cpp:588` sources from `obs[OBS_HURST_EXPONENT]`, itself written only by `TripleScreen1.cpp:629`'s TS1/240-min computation | **ADDITIVE TWIN** (kurtosis pattern) | Yes — `fast_hurst_exponent` |
| `recurrence_rate` | **None** — confirmed directly (spec §5a): grepped `RiskManager.cpp`/`Scoring.cpp`/`PositionManager.cpp`/`TradeDecisionEngine.h`, no hits; `ContextManager.cpp`'s `AreTs2StructuralDimsReady()` only checks finiteness/range, not a calibrated threshold | **REPLACEMENT** (`skewness_idx` pattern) | No |

**Explicitly OUT OF SCOPE, do not touch:**
- `TripleScreen2.cpp`'s own TS2/60-min `HurstExponentIndicator` (feeds `PositionManager.cpp`'s `HurstExponentEnum` regime-decision state machine, e.g. `EvaluateTs2HurstRegime`) — an independent computation for pattern/regime routing, not the HMM observation vector. Different subsystem, different consumer, not in §5a's scope.
- `TripleScreen3.cpp`'s own `Subgraph_HurstExponent` feeding `anchors.hurstExponent` (Predator Context pattern anchors) — same reasoning, independent computation.
- `fractal_dim` (already done, `72ab967`, pure time-bar widening, NOT activity-clock — §5a explicitly keeps it separate) and `fisher_info` (§6, unresearched, separate question).
- Any change to `PositionManager.cpp`'s or `RiskManager.cpp`'s gate *thresholds* — this plan only adds new inputs (`fast_mean_rev_z`, `fast_hurst_exponent`) and swaps `recurrence_rate`'s source; it does not recalibrate or add new gates. Whether/how the new twin fields get consumed by gates is a future decision (mirrors kurtosis: the fast twin was added to the observation vector and made available on `LocalRiskContext`, gate-wiring into `Scoring.cpp`'s five kurtosis checks was Task 9-12 of that plan and is *not* assumed here as automatically in scope — flag it as an open follow-on in Task 8 below rather than doing it silently).

## Global Constraints

(Same as the kurtosis plan — repeated here since this plan may be executed independently.)

- No heap allocations in recurring ACSIL update paths.
- Dead/legacy/backward-compatibility code removed on sight once verified unused (`CLAUDE.md` Code Safety Rules).
- Schema changes go through `schema/PENDING_SCHEMA_CHANGES.md` (PROPOSED → DECIDED → IMPLEMENTED) before `regenerate_schema.sh` runs. Never call `flatc` directly.
- Full clean build via `./build_dll.sh`, incremental via `./build_dll.sh --no-clean`.
- Native test convention: bare `g++ -std=c++17 -I include tests/cpp/test_X.cpp -o /tmp/X_test && /tmp/X_test`, hand-rolled `check(name, bool)` helper — no GoogleTest/CMake.
- Pure-engine/thin-glue split for anything computed from `ActivityClockManager`'s buffer, matching `SevcikFractalDimension.h`/`RQAEpsilonSelector.h`/`RecurrenceRateEngine.h`'s own precedent this session — extract the math into a header with zero ACSIL dependency, natively tested against a brute-force/reference reimplementation of the existing sc-based function, then make the existing sc-based function delegate to it (do not leave two independent copies of the same math).
- The slow (time-bar) `mean_rev_z`/`hurst_exponent` values and their existing gate thresholds remain untouched and authoritative — the fast twins are additive-only inputs, never a silent replacement of a calibrated gate's source.

---

## Task 1: `recurrence_rate` replacement — reuse `RecurrenceRateEngine.h`, fed by imbalance-bar returns

**Why this is simpler than it looks, and simpler than the time-bar version's incremental design**: `ImbalanceBarEngine::GetImbalanceBarReturns()` only ever holds *completed*-bar returns — there is no "live, still-forming" point analogous to the time-bar version's current price (`RecurrenceRateEngine::ComputeRate()`'s whole reason to exist). The activity-clock version therefore does **not** need the closed/live split at all: recompute the full O(n²) RQA matrix over the last 100 imbalance-bar returns only when a **new imbalance bar actually closes** (`ActivityClockManager::Instance().Engine().GetCompletedBarCount()` advances), cache the result otherwise. This is closer to `FeatureScaler`'s own recalibration-cadence gating than to the time-bar engine's tick-vs-bar split.

**Files:**
- Modify: `src/ContextManager.cpp` (the existing `{ ... GetImbalanceBarReturns(100, rawReturns) ... }` block at ~line 566 — extend it to also produce `recurrence_rate`, reusing the *same* 100-return fetch already happening there for kurtosis/skewness, not a second fetch)
- Modify: `src/TripleScreen2.cpp` (remove the `recurrenceRate = CalculateRecurrenceRate(sc, slow_window_n)` call and its `obs->mutate_recurrence_rate(recurrenceRate)` — TS2 no longer owns this dim once it moves into `ContextManager.cpp`'s activity-clock block, matching `skewness_idx`'s removal from `TripleScreen3.cpp`)
- Modify: `include/RecurrenceRateEngine.h` — no interface change expected; it already operates on generic `const float*` arrays, not `sc.BaseData` directly, so it is reusable as-is. Confirm via the new test below rather than assuming.
- Delete (once confirmed unused after this task): `CalculateRecurrenceRate(sc, lookback_n)` in `StudyHelperFunctions.cpp:3378` and its declaration in `include/StudyHelperFunctions.h`, plus the four now-orphaned persistent vars (`RECURRENCE_RATE_LAST_VALID_VALUE`, `RQA_CALIBRATED_EPSILON`, `RQA_LAST_CALIBRATION_BAR_INDEX`, `RQA_ENGINE_STATE_PTR`, `RQA_LAST_WINDOW_BAR_INDEX`) **only after grepping to confirm zero remaining callers** — per `CLAUDE.md`'s Code Safety Rule, do not remove speculatively.
- Test: extend `tests/cpp/test_recurrence_rate_engine.cpp` with a case seeding the engine from a 100-length synthetic returns array (not prices) and confirming `RebuildClosedBarWindow`/`ComputeRate` behave identically regardless of whether the input represents prices or returns (the engine has no opinion on units — this is a real thing to verify, not assume, since epsilon selection scale depends on it).

**Design:**
- New persistent-state need: a cached `float`, plus the `GetCompletedBarCount()` value the cache was last built for (mirrors `RQA_LAST_CALIBRATION_BAR_INDEX`'s role but keyed on completed-imbalance-bar-count, not `sc.Index`). Store both on `ActivityClockManager` itself (it already owns the one `ImbalanceBarEngine`; adding a small cache here avoids yet another `PersistentVar_AdaptiveCalculators` entry and keeps this activity-clock-specific state colocated with the buffer it caches from) — add `float GetCachedRecurrenceRate(...)`-style accessor, or compute it directly inside `ContextManager.cpp`'s block using a `static` local keyed on the count (simpler, matches the block's existing style — prefer this unless it grows unwieldy).
- Epsilon: reuse `SelectEpsilonForTargetRecurrenceRate` (`RQAEpsilonSelector.h`) with the same `RQA_TARGET_RECURRENCE_RATE = 0.05` used today — recalibrated every time the cache rebuilds (i.e., every new imbalance bar, not every 200 bars) since imbalance-bar completion is already the natural, activity-driven cadence; there is no tick-vs-bar mismatch to correct for here the way there was for the time-bar version.
- Window: **100 imbalance-bar returns**, matching the fetch already shared by `fast_taleb_kurtosis`/`skewness_idx` in this exact code block — not because 100 is independently derived for RQA, but because reusing the one buffer fetch already happening here is the simpler, more consistent design, and 100 is comfortably inside `ImbalanceBarEngine`'s 500-capacity buffer with headroom for `RecurrenceRateEngine`'s `kMaxClosedBars = 256`.
- Degenerate/cold-start: fewer than 100 completed imbalance bars available → return `0.0f` and skip the cache update (mirrors kurtosis/skewness's own `count >= 100` warmup gate in the same block, not a new convention).

- [x] Write the engine-reuse test (confirm `RecurrenceRateEngine` is unit-agnostic), confirm it fails to compile/pass only if a real assumption breaks
- [x] Wire the replacement into `ContextManager.cpp`'s existing 100-return block
- [x] Remove `TripleScreen2.cpp`'s `recurrenceRate` computation and mutation
- [x] Confirm zero remaining callers of `CalculateRecurrenceRate(sc, lookback_n)`, then delete it + its now-orphaned persistent vars
- [x] Full `./build_dll.sh --no-clean`; `test_recurrence_rate_engine`, `test_rqa_epsilon` regression pass
- [ ] Commit

---

## Task 2: Pure DFA/Hurst extraction — `include/DfaHurstExponent.h`

**What `CalculateHurstExponent(sc, length, minScale)` (`StudyHelperFunctions.cpp:2522`) actually computes**: this is **Detrended Fluctuation Analysis (DFA)**, not classic rescaled-range (R/S) — despite variable/comment names elsewhere in the codebase saying "R/S". It: (1) converts `length` closed log-returns from `sc.BaseData[SC_LAST]` into a de-meaned cumulative "profile," (2) for each scale `s` in `[minScale, length/4]`, splits the profile into segments, detrends each via linear regression, accumulates RMS fluctuation, (3) regresses `log(fluctuation)` on `log(scale)` — the slope is the Hurst exponent.

**Key structural fact making this a clean fit for the activity-clock buffer**: the function's own first loop (lines ~2549-2561) computes log-returns from raw prices *before* using them — `ImbalanceBarEngine::GetImbalanceBarReturns()` already returns log-returns directly, so the pure extraction should accept **log-returns as input**, skipping the price→return conversion step entirely (do not have callers reconstruct a synthetic price array just to satisfy a prices-shaped interface).

**Files:**
- Create: `include/DfaHurstExponent.h` — pure function `float DfaHurstExponent(const double* logReturns, int length, int minScale)`, body = `CalculateHurstExponent`'s logic starting from `double meanReturn = ...` (skip the log-return computation loop, replaced by the caller-supplied array) through the final regression slope + `std::clamp(hurst, 0.0f, 1.5f)`. Returns `NaN` (not a fallback constant) for any degenerate/insufficient-data case — mirrors `SevcikFractalDimension.h`'s convention of pushing carry-forward/cold-start policy to the caller, not baking it into the pure function.
- Modify: `src/StudyHelperFunctions.cpp`'s `CalculateHurstExponent(sc, length, minScale)` to: (a) build the `logReturns` array from `sc.BaseData[SC_LAST]` exactly as it does today, (b) call `DfaHurstExponent(logReturns.data(), length, minScale)`, (c) apply the existing `fallback_hurst()`/persistent-carry-forward wrapper around the `NaN` result — same refactor shape as `CalculateFractalDimension`'s delegation to `SevcikFractalDimension.h` this session.
- Test: `tests/cpp/test_dfa_hurst_exponent.cpp` — brute-force-equivalence test against a reimplementation of the *original* `CalculateHurstExponent` body (same technique as `test_sevcik_fractal_dimension.cpp`: a `bars[k]`-relative reference function, a chronological-array pure-function call, assert they match within `1e-4`) at `length ∈ {50, 100, 200}`, `minScale=8`. This is a refactor-parity test, not new behavior — it must prove zero change to the existing TS1/TS2 Hurst values before Task 3 adds anything new.

- [ ] Write the brute-force-equivalence test (fails until extraction exists)
- [ ] Extract `DfaHurstExponent.h`, refactor `CalculateHurstExponent` to delegate
- [ ] Confirm the extracted test passes; confirm `./build_dll.sh --no-clean` still succeeds with byte-identical TS1/TS2 Hurst output (no behavior change yet)
- [ ] Commit (parity-only commit, no new fields yet — keeps this refactor separately revertable from Task 3's actual new behavior)

---

## Task 3: `fast_hurst_exponent` additive twin

**Window**: **100 imbalance-bar returns, `minScale=8`** — not borrowed from kurtosis by analogy, but because `CalculateHurstExponent(sc)`'s own existing no-argument convenience overload (`StudyHelperFunctions.cpp:2675`, `return CalculateHurstExponent(sc, 100, 8);`, used by TS2's `HurstExponentIndicator` today) already establishes `length=100, minScale=8` as this codebase's own "standard intraday" Hurst configuration — independent, pre-existing grounding for this exact pair of numbers, stronger than reusing kurtosis's buffer size by convenience alone.

**Files:**
- Modify: `src/ContextManager.cpp`'s 100-return block (same one Task 1 extends) — add `fastHurstExponent = DfaHurstExponent(returnsArray.data() [as double], 100, 8)` (needs a `double` copy of the `float` returns array, or a `DfaHurstExponent` overload taking `const float*` — prefer changing `DfaHurstExponent`'s signature to `const float*` internally converting to `double` per-element, matching `SevcikFractalDimension.h`'s own `float`-in/`double`-internal convention, rather than forcing every caller to pre-convert).
- Modify: `include/LocalRiskContext.h` — add `float fastHurstExponent = 0.5f;` immediately after `hurstExponent` (mirrors `fastTalebKurtosis`'s placement immediately after `talebKurtosis`).
- Modify: `../schema/mts_schema.fbs`'s `ObservationData` struct — add `fast_hurst_exponent: float;` (18th field). File a `schema/PENDING_SCHEMA_CHANGES.md` entry (PROPOSED, mirroring PSC-03's `fast_taleb_kurtosis` entry) before running `regenerate_schema.sh`.
- Modify: `include/ContextManager.h` — add `static constexpr size_t OBS_FAST_HURST_EXPONENT = MTS::Schema::Contract::kObsFastHurstExponent;` and update the `static_assert(... == OBSERVATION_VECTOR_SIZE)` bound.
- Modify: `include/FeatureScaler.h` — add index-18 entries to all four calibration arrays (`SHRINKAGE_SCALE_MIN`, `LOGZ_WINSOR_SIGMA_OVERRIDE`, `DIM_WINSOR_SIGMA_OVERRIDE`, `DIM_WINDOW_SIZE`) with explicit, honest placeholder comments (this project's own 17-dim indexing bug this session — `7c51f33` — is the direct cautionary precedent: **do not leave an implicit/defaulted entry when adding a dim**, write it explicitly even if the calibration value itself is a documented placeholder pending real-data calibration).
- Cold-start default: `0.5f` (matches `hurstExponent`'s own random-walk-neutral default and `DfaHurstExponent`'s degenerate-fallback convention from Task 2).

- [ ] File `schema/PENDING_SCHEMA_CHANGES.md` entry, get it to DECIDED
- [ ] `regenerate_schema.sh`
- [ ] Wire `fastHurstExponent` computation + `LocalRiskContext` field + `FeatureScaler` calibration rows
- [ ] Native test: extend a `FeatureScaler` or dedicated test confirming dim 18 calibrates/scales without regressing dims 0-17
- [ ] Full `./build_dll.sh --no-clean`; commit (mark PENDING_SCHEMA_CHANGES entry IMPLEMENTED in the same session if both repos are touched, else note the cross-repo dependency explicitly per this project's coevolution-governance convention)

---

## Task 4: Pure mean-reversion (z + ρ) extraction — `include/ActivityClockMeanReversion.h`

**What `CalculateMeanReversionSpeed(sc, lookback_n)` (`StudyHelperFunctions.cpp:3247`) actually computes**: (1) a log-price z-score `abs_z_price` over the window, (2) lag-1 return autocorrelation `rho`, (3) `score = abs_z_price * clamp(1 - max(rho,0), 0, 1)`, clamped to `[0,5]`.

**The one real design fork here, decide before writing code**: the z-score component needs a *price-like* series, but `ImbalanceBarEngine` only stores per-bar log-*returns*. The `rho` component already operates on log-returns directly in the original — no conversion needed for that half. **Decision: reconstruct a within-window log-price-like path via cumulative sum of the 100 buffered log-returns** (`cumsum[i] = cumsum[i-1] + logReturns[i]`, `cumsum[-1] = 0`), then z-score the cumsum series exactly as the original z-scores the log-price series. This is standard (log-price is definitionally the cumulative sum of log-returns) and requires no new statistical machinery — but note explicitly in the header comment that the resulting z-score is *relative to the start of the buffered window*, not an absolute price level, since the buffer only ever holds a rolling 100-bar tail (this is a real, but harmless, distinction worth documenting rather than discovering during a later debugging session).

**Files:**
- Create: `include/ActivityClockMeanReversion.h` — pure function `float ActivityClockMeanRevZ(const float* logReturns, int n)`:
  1. Build `cumsum[0..n]` (n+1 points, `cumsum[0]=0`)
  2. z-score `cumsum` exactly as `CalculateMeanReversionSpeed`'s `abs_z_price` (mean/var over the n+1 points, `|last - mean| / std`)
  3. `rho` = lag-1 autocorrelation of `logReturns[0..n)` directly, identical formula to the original (`num`/`den` accumulation over `t=1..n-1`)
  4. `score = clamp(abs_z_price * clamp(1 - max(rho,0), 0, 1), 0, 5)`
  5. Return `NaN` for degenerate cases (flat cumsum std, `n` too small) — caller owns carry-forward, same convention as Tasks 1-3.
- Modify: `src/StudyHelperFunctions.cpp`'s `CalculateMeanReversionSpeed` — **not refactored to delegate** in this task (unlike Hurst/fractal_dim): its z-score operates on *actual* log-prices from `sc.BaseData`, not a cumsum reconstruction, so forcing it through the same pure function would require it to fabricate a cumsum from its own price window, an unnecessary indirection for code that already has real prices available. Leave `CalculateMeanReversionSpeed` as-is; `ActivityClockMeanRevZ` is a **parallel**, not a **shared**, implementation — document this explicitly in both headers' comments so a future reader doesn't assume they're the same extraction pattern as Hurst/fractal_dim and go looking for a delegation call that doesn't exist.
- Test: `tests/cpp/test_activity_clock_mean_reversion.cpp` — hand constructed cases (pure momentum synthetic series → low score via high `rho`; pure mean-reverting synthetic series → high score via low/negative `rho`; flat/degenerate → `NaN`), not a brute-force-equivalence test against `CalculateMeanReversionSpeed` (there is no equivalence to prove, per the point above — this is new, standalone logic, test it on its own merits).

- [ ] Write `test_activity_clock_mean_reversion.cpp` (fails until the header exists)
- [ ] Implement `ActivityClockMeanReversion.h`
- [ ] Confirm test passes; confirm `CalculateMeanReversionSpeed` is untouched (diff review, not just "build still passes")
- [ ] Commit

---

## Task 5: `fast_mean_rev_z` additive twin

**Window**: **100 imbalance-bar returns** — same reused-buffer reasoning as Task 1/3 (one fetch, already established count for this buffer in this codebase); no independent time-bar precedent to match here since `mean_rev_z`'s own TS3 window (`observation_window_n`, 10-40 bars) is adaptive and much smaller than 100, so there is no existing "100" to point to the way Hurst's `length=100` convenience overload provided one — flag this honestly in the code comment as "reuses the shared buffer fetch count, not independently derived for this dim" rather than implying false precedent.

**Files:**
- Modify: `src/ContextManager.cpp`'s 100-return block — add `fastMeanRevZ = ActivityClockMeanRevZ(rawReturns, 100)` (reuses the same `rawReturns`/`returnsArray` already fetched for kurtosis/skewness/recurrence_rate/Hurst in this one block — five dims off one 100-return fetch, not five separate buffer reads).
- Modify: `include/LocalRiskContext.h` — add `float fastMeanRevZ = 0.0f;` immediately after `meanRevZ`.
- Modify: `../schema/mts_schema.fbs`'s `ObservationData` — add `fast_mean_rev_z: float;` (19th field). File/decide the `PENDING_SCHEMA_CHANGES.md` entry in the *same* schema-repo change as Task 3's `fast_hurst_exponent` (both are additive twins landing together — one schema bump for both, not two, since they're being designed and likely implemented in the same session).
- Modify: `include/ContextManager.h` — `OBS_FAST_MEAN_REV_Z`, update `static_assert` bound to cover 19 dims total.
- Modify: `include/FeatureScaler.h` — index-19 entries across all four calibration arrays, same explicit-placeholder discipline as Task 3.
- Cold-start default: `0.0f` (matches `meanRevZ`'s own "no stretch" neutral default).

- [ ] Extend the same `PENDING_SCHEMA_CHANGES.md`/`regenerate_schema.sh` pass from Task 3 to cover this field too (one schema regen for both new dims, confirm via diff review that both land together)
- [ ] Wire `fastMeanRevZ` computation + `LocalRiskContext` field + `FeatureScaler` calibration rows
- [ ] Full `./build_dll.sh --no-clean`; commit

---

## Task 6: Cross-repo schema + downstream sync

- [ ] `../schema/PENDING_SCHEMA_CHANGES.md`: both new fields (`fast_hurst_exponent`, `fast_mean_rev_z`) move PROPOSED → DECIDED → IMPLEMENTED together (one entry or two, follow that file's own existing convention — check whether PSC-03 was one row for `fast_taleb_kurtosis` alone or grouped; match it).
- [ ] Confirm `N_DIMS`/`OBSERVATION_VECTOR_SIZE` propagate correctly everywhere derived from `MTS::Schema::Contract::kObservationDim` (this should be automatic via the existing `constexpr` chain — verify by grep, not assumption, the same way this session verified `FeatureScaler.h`'s arrays were schema-driven before trusting them).
- [ ] `lbrnet`-side consumer code: per `fast_taleb_kurtosis`'s own precedent (row 9 of `PRODUCTION_TRIAGE.md`: "3 repos' generated bindings exist but uncommitted, and `lbrnet`'s hand-written consumer code doesn't select the new dim yet"), this is a **known recurring gap for additive dims specifically** — file the equivalent note for these two new dims rather than assuming the FlatBuffers regen alone makes them consumed. Do not silently repeat the gap a second time without at least flagging it explicitly this time.
- [ ] `PRODUCTION_TRIAGE.md` row 1 / `NORTH_STAR_STATUS` updated to reflect these two dims implemented (mirrors how `72ab967`'s `fractal_dim` ship was recorded this session).
- [ ] Doc-sync mirrors (`README-AI.md`, `.github/copilot-instructions.md`, `CLAUDE.md`, `GEMINI.md`) updated together per the Documentation Sync Contract, same pattern as this session's `c8b908c`.

---

## Task 7: Recalibration — new dims' `FeatureScaler` calibration values are genuine placeholders, not final

Every new dim's `FeatureScaler.h` entries (`DIM_WINSOR_SIGMA_OVERRIDE`, `LOGZ_WINSOR_SIGMA_OVERRIDE`, `SHRINKAGE_SCALE_MIN`) added in Tasks 3/5 are placeholders (e.g., copy `fast_taleb_kurtosis`'s own values as a starting point, or a neutral wide-tolerance default) — **not empirically derived from real MES data**, same honest labeling this project already applies elsewhere (e.g. `skewness_idx`'s "NEEDS RE-AUDIT" comment from the `7c51f33` fix). Do not present them as calibrated in any commit message or doc update. A follow-on empirical-calibration pass (same methodology as `test_feature_scaler.cpp`'s real-data `|z|` distribution checks) is a separate, later task, not bundled into this plan.

- [ ] Confirm every new `FeatureScaler.h` array entry added in Tasks 3/5 carries an explicit "placeholder, not yet calibrated against real data" comment
- [ ] File this as a tracked follow-on (`PRODUCTION_TRIAGE.md` or a dedicated note), do not close it out silently

---

## Task 8: Gate-consumption follow-on (explicitly deferred, not started here)

Whether/how `fast_mean_rev_z`/`fast_hurst_exponent` get consumed by any live gate (mirroring kurtosis's five-gate wiring in its own plan's later tasks) is **out of scope for this plan** — this plan's job is making the two fast twins exist, computed, scaled, and available on `LocalRiskContext` and the HMM's observation vector, same stopping point `fast_taleb_kurtosis` itself reached before any gate-wiring decision was made for it. Do not add gate logic speculatively.

- [ ] File a follow-on note (`PRODUCTION_TRIAGE.md` row 1 or a fresh row) that gate-consumption for both new twins is an open, undecided question, not silently skipped
