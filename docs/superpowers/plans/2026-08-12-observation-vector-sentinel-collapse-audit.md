# Audit: Dims 7/11 (and 1/2) Zero-Collapse in .context Export — Post TS1-Gate-Fix

Date: 2026-08-12
Scope: `StudyHelperFunctions.cpp` (raw physics calculators), `TripleScreen2.cpp`, `TripleScreen3.cpp`
(observation writers), `FeatureScaler.h` (hybrid scaler), `ContextManager.cpp` (sanitize/export pipeline)

Prior work (do not re-solve): `docs/superpowers/plans/2026-08-11-edc-ts1-zero-dims-audit.md` and
`docs/superpowers/specs/2026-08-11-edc-ts1-ready-contract-hardening.md` fixed dims 0/6/8
(`log_variance_ratio`/`hurst_exponent`/`fisher_info`) being **permanently frozen at bootstrap
placeholders** via a TS1 producer-readiness/quality gate. That fix is verified working (see
Evidence §1 below) and is **not reopened here**. This audit covers a **different, additional**
root-cause layer that the TS1 gate fix does not touch, and which is now the dominant visible
defect in the corrected export.

## Evidence

### §1. Verification methodology

Ran `lbrnet/scripts/context_preflight.py` plus a custom position-tagged sampler
(`context_zero_trace.py`, ad hoc) against
`/mnt/c/SierraChart2/Data/event_data_20260811_223632.context` (the file the user pointed at —
newest EDC export after the TS1-gate fix landed). Loaded 400,000 aligned
`MarketObservation`+`SystemState` pairs (~288 hours of replay), read directly off the FlatBuffer
bytes at the schema-declared offsets — no intermediate Python transform.

**Important scoping note:** the value read from disk is the **post-`FeatureScaler` hybrid-scaled**
observation (`ContextManager.cpp:1220` `currentObs`, not the raw physics `rawObs`) — this is what
`EventSerializer` writes to `.context`. All ratios below are on that exported value.

### §2. Zero-ratio by octile (400k rows, 8 segments of 50k, in stream order)

| dim | name | seg0 | seg1 | seg2 | seg3 | seg4 | seg5 | seg6 | seg7 | pattern |
|---|---|---|---|---|---|---|---|---|---|---|
| 0 | log_variance_ratio | .543 | .068 | .053 | .071 | .068 | .058 | .062 | .057 | **warmup-decay** (TS1 fix working) |
| 1 | burstiness_index | .812 | .763 | .750 | .591 | .762 | .728 | .714 | .653 | **persistent, high** |
| 2 | relative_range | .762 | .691 | .665 | .468 | .703 | .636 | .645 | .616 | **persistent, high** |
| 6 | hurst_exponent | .100 | .107 | .095 | .114 | .105 | .099 | .095 | .085 | persistent, low-flat |
| 7 | micro_asymmetry | .458 | .538 | .499 | .450 | .499 | .521 | .523 | .460 | **persistent, ~50%** |
| 8 | fisher_info | .457 | .504 | .240 | .070 | .067 | .064 | .063 | .118 | decay + one anomaly |
| 11 | amihud_illiquidity | .424 | .378 | .454 | .437 | .402 | .446 | .442 | .425 | **persistent, ~40%** |

Max consecutive-zero run (death-spiral indicator): dim0=25431, dim1=4984, dim2=4984, dim6=97,
dim7=7068, dim8=14290, dim11=3292.

**Read on this table:** dim 0 is the TS1-gate signature — high in the first block only, then drops
to a low, stable baseline. That is the *already-fixed* bug, confirmed still fixed. Dims 1, 2, 7,
and 11 show **no such decay** — the zero rate is flat across all 288 hours of replay, at a rate far
above anything explainable by warmup. Dim 8 shows the warmup-decay shape *plus* one large
mid-stream anomaly (a single 14,290-row dead run), which is a distinct, narrower issue (§5).
Dim 6 sits at a low but non-decaying ~10% — likely the same family at low rate; not chased further
here.

**This confirms the user's report is real and is not an artifact of the earlier fix**: dims 7/11
were very likely *already* broken before the TS1-gate fix, just invisible, because before that fix
`CheckAndTriggerHMM` rarely reached a state where it emitted a steady, representative stream of
rows at all. Now that TS1 readiness unblocks normal emission, the pre-existing dim 7/11 (and 1/2)
defects are fully exposed across the whole file.

### §3. Root cause mechanism (common to all four persistent dims)

Traced each dim from its FlatBuffers slot back through the writer and into `FeatureScaler`:

1. **`FeatureScaler::UpdateAndNormalize`** (`include/FeatureScaler.h:221-371`) computes, per dim,
   `z = (current - median) / madScale` from a rolling robust window (`RobustLocation`,
   `FeatureScaler.h:199-217`), then for SOFTLOGZ dims returns `ToSoftLogZ(z)` (`FeatureScaler.h:191-194`)
   — which is **exactly `0.0f` iff `z == 0`, i.e. iff `current == median` exactly**. For LOGZ dims
   (`result[i] = clamp(zLog, -6, 6)`, `FeatureScaler.h:352`) the same holds for `currentLog == median`.
2. Several raw calculators feeding these dims return a **fixed numeric sentinel** (not "no
   observation", not carry-forward) whenever their upstream input is temporarily degenerate:
   - **Dim 7 (`micro_asymmetry`)** — `CalculateMicroAsymmetryFromTimeAndSales`
     (`src/StudyHelperFunctions.cpp:2637-2697`) returns `quiet_NaN()` whenever
     `TimeSalesArray.Size() == 0`, or there are **zero new BID/ASK-classified T&S records since the
     last call** (`new_directional_records == 0`, line 2686-2689). `CalculateMicroAsymmetry`
     (`:2699-2727`) is documented as a "Dual-Mode" implementation ("Phase 1: Try Time and Sales...")
     but **Phase 2 (price-action fallback) was never implemented** — it just returns `NaN` too
     (line 2726). That `NaN` is written verbatim by `TripleScreen3.cpp:774`
     (`obs->mutate_micro_asymmetry(microAsymmetry)`), then `ContextManager::SanitizeObservationVector`
     (`ContextManager.cpp:97-115`) converts non-finite → **`0.0f`** before the value ever reaches
     `FeatureScaler`.
   - **Dim 11 (`amihud_illiquidity`)** — `CalculateAmihudIlliquidity`
     (`src/StudyHelperFunctions.cpp:3149-3180`) returns a hard `0.0f` (line 3174) whenever fewer
     than 2 bars in the lookback have both `Volume >= 1.0` and positive price (thin/illiquid bars —
     routine overnight Globex behavior for ES, not an edge case). Written directly by
     `TripleScreen3.cpp:770`.
   - **Dim 2 (`relative_range`)** — inline in `TripleScreen2.cpp:270-274`:
     `relRange = 0.0f` unless `atr > 0.00001f`. No fallback/carry-forward on the else branch.
   - **Dim 1 (`burstiness_index`)** — `CalculateBurstiness` (`src/StudyHelperFunctions.cpp:3182-3210`)
     returns `0.0f` (line 3206) whenever the older half-window's realized-range rate is below a
     `1e-12` floor (flat/no-range bars) — same family, not yet empirically isolated to the same
     depth as 7/11 but structurally identical.
3. Because the degenerate branch fires on a **large, non-warmup-limited fraction of bars** (T&S
   depth gaps in replay data, thin Globex volume, near-flat ranges), the sentinel value becomes a
   high-frequency, **exactly repeated** value inside that dim's `FeatureScaler` rolling window. Once
   the sentinel is common enough to tie or become the window's robust median, **every recurrence of
   the sentinel maps to an exact `z == 0` and therefore an exact `0.0` scaled output** — which is what
   lands in `.context`. This is self-reinforcing: the more often the sentinel fires, the more it
   dominates the median, the more exactly-zero output it produces.

This is a fundamentally different failure mode from the TS1 issue: TS1 was "stale/placeholder value
treated as fresh by a broken readiness gate." This is "a legitimate-looking guard clause in a raw
calculator silently emits a fixed sentinel instead of signaling 'no fresh observation,' and the
downstream scaler's median-based transform turns a *frequent* sentinel into an *exact-zero, blends
in as if it were a valid neutral reading* output."

### §4. Why `context_preflight.py` did not already catch this

`constant_ratio_threshold` defaults to `0.995` — only true 100%-frozen dims (the old TS1 bug) trip
the `violations` list. A 40-80% zero rate only trips the (non-fatal) `warnings` list at the
`saturation_threshold` of `0.98`... which it also doesn't reach, so today it produces **zero
warnings and zero violations** — `status: PASS` — for a file where four dims are effectively
half-dead. Confirmed by direct run: `PASS`, one unrelated correlation warning. The gate itself is a
secondary root cause (§ Task 5 in the fix spec).

### §5. Dim 8 anomaly (scoped out, flagged for separate follow-up)

Dim 8 (`fisher_info`) shows the TS1 warmup-decay shape (matches the fixed dim0/6 pattern) plus one
14,290-row fully-dead run mid-stream. `CalculateFisherInformation`
(`src/StudyHelperFunctions.cpp:3094-3120`) returns `0.0f` when `maxPrice <= minPrice` over its
lookback (flat price — plausible during an extended low-liquidity/holiday gap inside the 12-day
replay window). This is almost certainly the same sentinel-collapse family, triggered by a real
flat-price episode rather than a chronic per-bar gap like 7/11/2/1. Not fixed in this pass —
tracked as a lower-priority instance of the same root cause; the general fix in the accompanying
spec (per-dim carry-forward instead of hard sentinel) resolves it for free once applied to
`CalculateFisherInformation`.

## Conclusion

Root cause is **not** a regression from the TS1-gate fix. It is a pre-existing, second, independent
defect class — "sentinel-default collapse through a median-based scaler" — that the TS1 fix simply
unmasked by finally letting a representative, continuous stream of rows through. Fix must be applied
at the raw-calculator layer (stop emitting fixed sentinels on degenerate input; carry forward the
last valid physics reading instead, exactly as `TripleScreen1.cpp`'s Hurst fallback and
`CalculateRealizedKurtosis`'s `prevKurtosis` pattern already do elsewhere in this codebase) — see
companion spec `docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md`.
