# Spec: Sentinel-Collapse Hardening for Observation Dims 1/2/7/8/11

Date: 2026-08-12
Owner: C++ execution layer (MindfulTrader)
Scope: `StudyHelperFunctions.cpp`, `TripleScreen2.cpp`, `TripleScreen3.cpp`, `FeatureScaler.h`,
`../lbrnet/scripts/context_preflight.py`

Prerequisite reading: `docs/superpowers/plans/2026-08-12-observation-vector-sentinel-collapse-audit.md`
(root-cause evidence this spec implements). Do not confuse with, or reopen,
`docs/superpowers/specs/2026-08-11-edc-ts1-ready-contract-hardening.md` (dims 0/6/8
producer-readiness gate — separate, already-fixed problem).

## Problem Statement

Four ObservationData dims (1 `burstiness_index`, 2 `relative_range`, 7 `micro_asymmetry`, 11
`amihud_illiquidity`) exported to `.context` are exact `0.0` for 40-80% of rows, **flat across the
entire replay window** (no warmup decay) — i.e. not "cold start," a chronic, ongoing defect. A
fifth dim (8 `fisher_info`) shows one large mid-stream dead run of the same character. Root cause:
raw calculators return a fixed numeric sentinel (`0.0f`, or `NaN` sanitized to `0.0f`) on routine,
frequent degenerate input (thin-volume bars, T&S depth gaps, flat ranges) instead of signaling
"no fresh reading." Once frequent enough to anchor `FeatureScaler`'s rolling median, every
recurrence of the sentinel maps to an exact `z=0` → exact `0.0` scaled output, masquerading as
valid, low-information training data.

## Design Decisions

### D1. Raw calculators carry forward the last valid physics value; they never emit a designed sentinel on degenerate input

This is the existing, already-proven pattern in this codebase (`TripleScreen1.cpp`'s
`s_hasLastValidHurst`/`s_lastValidHurst` fallback; `CalculateRealizedKurtosis`'s `prevKurtosis`
carry-forward, `StudyHelperFunctions.cpp:2760-2764`). Extend it to the four (five) affected
calculators. Carry-forward means: "repeat the last physically-observed value," not "assert
`NaN`/`0` and let a downstream layer paper over it." This keeps `.context` rows informative (a
stale-but-real reading) instead of injecting a fabricated neutral-looking `0.0` that corrupts the
scaler's own calibration.

Per-calculator changes:

- **Dim 7 (`micro_asymmetry`)** — **[AMENDED 2026-08-12, see
  `docs/superpowers/plans/2026-08-12-denali-data-feed-proxy-audit.md`]**. The original plan below
  (carry-forward on the T&S scan) is superseded by a root-fix once the Denali/`.scid` audit showed
  `sc.BidVolume[sc.Index]`/`sc.AskVolume[sc.Index]` — the same per-bar aggregated, always-cleanly-
  classified data confirmed at the `.scid` byte level, already consumed successfully every bar by
  `VolumeIndicator::UpdateVolume()` (`TripleScreen3.cpp:637-638`) in the identical replay sessions
  where the T&S scan fails ~45% of the time — is a strictly more robust source than manually
  re-scanning `sc.GetTimeAndSales()` with hand-rolled sequence bookkeeping. **Primary fix:**
  compute `micro_asymmetry` as
  `(sc.AskVolume[sc.Index] - sc.BidVolume[sc.Index]) / (sc.AskVolume[sc.Index] + sc.BidVolume[sc.Index])`,
  mirroring `VolumeIndicator`'s existing pattern. Keep `CalculateMicroAsymmetryFromTimeAndSales`
  only as an optional log-only cross-check (or remove it — the docstring's "Phase 1/Phase 2
  dual-mode" framing was aspirational and never had a real Phase 2). **Backstop (unchanged from the
  original plan, still needed):** carry-forward the last valid value for the rare true no-trade bar
  (`BidVolume == AskVolume == 0`, which the `.scid` audit shows is essentially never seen intraday
  for ES/MES but costs nothing to guard).
- **Dim 11 (`amihud_illiquidity`)** — `CalculateAmihudIlliquidity`
  (`src/StudyHelperFunctions.cpp:3149-3180`): when `count < 2` (thin-bar lookback), return the last
  valid Amihud reading via carry-forward state instead of `0.0f` (line 3174).
- **Dim 2 (`relative_range`)** — `TripleScreen2.cpp:270-274`: when `atr <= 0.00001f`, return the
  last valid `relRange` via a persistent variable instead of leaving the `0.0f` initializer as the
  final value.
- **Dim 1 (`burstiness_index`)** — `CalculateBurstiness` (`src/StudyHelperFunctions.cpp:3182-3210`):
  when `rv_older_rate < kFloor` (line 3206), return last-valid carry-forward instead of `0.0f`.
- **Dim 8 (`fisher_info`)** — `CalculateFisherInformation` (`src/StudyHelperFunctions.cpp:3094-3120`):
  when `maxPrice <= minPrice` (line 3109), return last-valid carry-forward instead of `0.0f`.
  (The `sc.Index < lookback_n` warmup branch, line 3098, stays `0.0f` — true cold start, no prior
  value exists yet; this is not part of the persistent-zero symptom.)

Dims 1, 2, 8, and 11 follow one small, uniform pattern: a function-local `static` (or
`sc.GetPersistentFloat`, matching existing convention in each file) `lastValid`/`hasValidValue`
pair, seeded to today's existing sentinel only until the first real reading lands, then
carry-forward thereafter. No new abstraction — this is a copy of an existing, already-reviewed
pattern, applied four more times. Dim 7 gets the stronger `sc.BidVolume`/`sc.AskVolume` root-fix
above instead (same carry-forward pattern only as its backstop, not its primary mechanism).

### D2. `FeatureScaler` gets a narrow defense-in-depth backstop, not a redesign

D1 fixes the defect at the source. D2 is a cheap, second layer in case a future calculator
reintroduces the same mistake: `FeatureScaler::Calibrate()`/`Recalibrate()` already compute
`madScale` per dim from the rolling window; add a diagnostic (log-only, no behavior change) that
flags when a single raw value occupies more than, say, 30% of a dim's rolling window — this is the
exact signature of a sentinel-collapse and would have caught this class of bug immediately at
commit time. This is telemetry, not a functional change, to avoid re-litigating the scaler's
numerics for a problem that D1 already resolves at the root.

### D3. `context_preflight.py` gains a hard "chronic zero" gate

`constant_ratio_threshold` (0.995) only catches total freeze (the already-fixed TS1 bug class).
Add a new, separate threshold — e.g. `chronic_zero_threshold` default `0.15` — that promotes
`zero_ratio` above that level from a `warnings` entry to a `violations` entry (fails the gate,
`status: FAIL`, non-zero exit under `--strict`). This is what should have caught dims 1/2/7/11
before they shipped in an exported file. Keep the existing `saturation_threshold` (0.98) for the
one-sided "near-fully-saturated" case; the new threshold targets the "chronically, but not fully,
dead" case this incident exposed.

## Implementation Summary

- `src/StudyHelperFunctions.cpp`
  - `CalculateAmihudIlliquidity`: carry-forward on `count < 2`.
  - `CalculateBurstiness`: carry-forward on `rv_older_rate < kFloor`.
  - `CalculateFisherInformation`: carry-forward on `maxPrice <= minPrice` (not on cold-start warmup).
- `src/TripleScreen2.cpp`
  - `relRange` carry-forward when `atr <= 0.00001f`.
- `src/TripleScreen3.cpp`
  - `micro_asymmetry` recomputed from `sc.BidVolume[sc.Index]`/`sc.AskVolume[sc.Index]` (D1
    amendment, `docs/superpowers/plans/2026-08-12-denali-data-feed-proxy-audit.md`), with
    carry-forward as backstop only on the true `BidVolume == AskVolume == 0` case.
- `include/FeatureScaler.h`
  - Add log-only single-value-dominance diagnostic in `Calibrate()`/`Recalibrate()` (D2).
- `../lbrnet/scripts/context_preflight.py`
  - Add `--chronic-zero-threshold` (default `0.15`), promote to `violations` when exceeded.

## Acceptance Criteria

1. Re-run the audit's octile analysis against a freshly-collected `.context` file: dims 1, 2, 7, 11
   zero_ratio drops to a level consistent with genuine warmup/cold-start only (i.e. shows the same
   decay-then-flatten shape dim 0 already shows, flattening to a low single-digit percentage, not a
   persistent 40-80%).
2. `context_preflight.py --input <new file>` reports `status: FAIL` if any dim's `zero_ratio` still
   exceeds `chronic_zero_threshold` post-fix (proves the gate works), and `status: PASS` once D1 is
   deployed and a new file is collected.
3. No behavior change to genuinely-neutral warmup-only zeros (`sc.Index < lookback_n` branches) —
   only degenerate-input-during-steady-state branches move to carry-forward.
4. Existing TS1 dims 0/6/8 gate (2026-08-11 spec) is untouched and continues to pass its own
   acceptance criteria.

## Test Plan (per superpowers:test-driven-development — write these failing first)

- Unit test per calculator: feed a sequence where the degenerate condition holds for N consecutive
  calls after M valid calls; assert output equals the last valid value (not `0.0`/`NaN`) for all N
  degenerate calls, and equals the calculator's existing cold-start sentinel only before any valid
  call has occurred.
- `FeatureScaler` unit test: feed a raw series where a repeated sentinel is deliberately injected at
  a rate (e.g. 45%) matching the observed dim 7 pattern *before* D1 lands (regression fixture proving
  the median-collapse mechanism), then confirm that after switching the fixture to carry-forward
  values (i.e., simulating D1's fix upstream), the scaled output no longer exhibits an exact-zero
  clump at that rate.
- `context_preflight.py` test (or extend existing lbrnet test coverage) asserting a synthetic
  40%-zero dim trips the new `chronic_zero_threshold` violation.

## Rollback Strategy

Each calculator change is independent and mechanically identical (carry-forward instead of
sentinel) — revert per-file if one calculator's carry-forward state turns out to interact badly
with a specific replay edge case (e.g. instrument switch mid-session). D2 is log-only, zero risk.
D3 is a Python script threshold and can be relaxed without touching C++.
