# Observation Vector — Gang-Statistical Reformulation Initiative (Activity Clock)

**Status, opened 2026-09-06: the activity-clock counterpart to
`docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md`, which now covers
calendar/time-clock dims ONLY. This doc holds every `ObservationData` dimension computed natively
on the imbalance/activity clock (`ImbalanceBarEngine`) — split out so the two clock domains don't
mix in one doc, per the same discipline already applied to Force Index (Track 1 calendar-clock vs
Track 2 imbalance-clock) and the broader time-clock → imbalance-clock migration this repo is
working toward. Content below is MOVED from the 2026-08-31 doc, not duplicated — that doc no
longer contains it.**

**Naming note**: the underlying FlatBuffer fields carry a `fast_` prefix
(`fast_hurst_exponent`, `fast_taleb_kurtosis`, `fast_mean_rev_z`) solely to disambiguate them from
their calendar-clock siblings in the OTHER doc. Since this doc is exclusively activity-clock, that
prefix is redundant noise here — referred to below by their conceptual name (Hurst exponent, Taleb
kurtosis, mean-reversion z-score), with the real schema field name noted once per dim for
code-traceability.

## Per-dim status (moved from the 2026-08-31 doc's §7 ledger, same status vocabulary)

Status vocabulary: **IN** (settled, stays as-is) · **IN-WEAK** (stays, weak, no better alternative
identified) · **IN-UNMEASURED** (shipped, never tested against anything) · **OUT** (tested and
rejected) · **IN-CONTINGENT** (stays, contingently redundant against a future model change).

| Dim (schema field) | Status | Notes | Last verified |
|---|---|---|---|
| Hurst exponent (`fast_hurst_exponent`) | IN-UNMEASURED | Shipped but cross-state discrimination ratio never measured, alone or crossed with `relative_range` (the calendar-clock doc's own dim). `FeatureScaler.h`'s `DIM_WINSOR_SIGMA_OVERRIDE[8]` recalibrated 2026-09-04 (10.9, real GPD fit, n=4,664,244, real full-dataset activity-clock run) | 2026-09-04 |
| Skewness (`skewness_idx`) | IN-CONTINGENT, **partial construction fix** | Asymmetry axis rep, contingently redundant against a future skewed-Student-t emission. **2026-09-04**: a real-data recalibration pass found a genuine collapse signature (correlation(localMad,\|z\|) = -0.2459, n=4,664,244 -- Bowley skewness is mathematically bounded to [-1,1], so a huge z can only come from a collapsed denominator, not real skewness), the same mechanism already fixed for the amihud/liq_fragility/micro_asymmetry dims. `SHRINKAGE_SCALE_MIN[10]` enabled and `DIM_WINSOR_SIGMA_OVERRIDE[10]` finalized at 269.0 as a result. The fix reduces but does NOT fully eliminate the tail (corr -0.2459→-0.1691, max\|z\| 1761→177) — a genuine residual heavy tail remains, unlike `liq_fragility`'s own near-total fix. So "already confirmed robust construct" was true of the Bowley formula's form, but not of this dim's actual live-signal behavior | 2026-09-04 |
| Taleb kurtosis (`fast_taleb_kurtosis`) | IN-UNMEASURED, **top-priority action** | First-ever kurtosis dim in the vector; must be selected into `lbrnet`'s `HMM_KEEP_DIMS` and retrained — still the single highest-priority action across this whole vector. `FeatureScaler.h`'s `DIM_WINSOR_SIGMA_OVERRIDE[13]` recalibrated 2026-09-04 (97.0, real GPD fit, n=4,664,244) | 2026-09-04 |
| Recurrence rate (`recurrence_rate`) | IN, orthogonal to redundancy concerns | Fully replaced its former time-bar (TS2) computation 2026-08-28 — no calendar-clock sibling exists for this dim (unlike the other three here). Already confirmed robust construct (Phase 0 audit) | 2026-08-28 |
| Mean-reversion z-score (`fast_mean_rev_z`) | **OUT, tested and rejected 2026-09-04** | **Confirmed 2026-08-31**: completely unwired — declared, never computed, silently sitting at its schema default. **2026-09-02**: while still unwired, its formula was reformulated to median/MAD (matching the calendar-clock `mean_rev_z` sibling's own fix), committed `b0ab21a`. **RESOLVED 2026-09-04**: the wire-or-drop decision was blocked on cross-state HMM discrimination being unmeasurable (model-staleness circularity), but the forward-return/hit-rate test (`tools/observation_vector/mean_rev_z_variant_comparison.py`, `Scoring.cpp:305`'s real `score>2.0` gate, 60-min horizon) is NOT blocked by that circularity and was re-run after fixing a real staleness bug in the tool itself — its C++ driver was still silently using the OLD mean/std formula even after the header's own 2026-09-02 median/MAD reformulation, invalidating its prior null verdict. Re-ported faithfully against the current real formula (verified byte-for-byte against `StudyHelperFunctions.cpp:3033`/`include/ActivityClockMeanReversion.h`) and re-run on real data: **both variants remain statistically indistinguishable from a coin flip** — calendar-clock `mean_rev_z` n=12,716, hit_rate=0.4988, p=0.790; this activity-clock variant n=1,575,967, hit_rate=0.4994, p=0.137, 95% CI [0.4986,0.5002]. The sample is large enough (CI width ~0.0016) that a genuine small edge would very likely have been detected. **Decision: do not wire into production** on the basis of raw predictive power — a fresh, non-stale confirmation of the null result. The calendar-clock `mean_rev_z`'s own live status is unaffected — this finding is only about whether to ADD this activity-clock twin | 2026-09-04 |

## Model contamination manifest (moved from the 2026-08-31 doc's §5a)

**Standing rule (operator directive, 2026-09-03), inherited from the parent doc**: any finding —
here, in the parent doc, or a casual claim — that relies on `models/hmm_model.pkl`'s state
assignments, or on any retrain that predates the fixes below, is provisional evidence about a
contaminated model, not a settled conclusion. Production model per
`docs/superpowers/specs/2026-08-26-activity-clock-lbrnet-handoff.md`: `K=4, feature_dim=16`,
trained 2026-08-25 — every fix below post-dates that run and is invisible to it.

| Dim | Contamination type | What the trained model actually saw | Fixed |
|---|---|---|---|
| Taleb kurtosis (`fast_taleb_kurtosis`) | Missing (never selected) | Not in training vector at all — first kurtosis dim ever added, never in `HMM_KEEP_DIMS` | Still not selected/retrained on, as of 2026-09-03 |
| Hurst exponent (`fast_hurst_exponent`) | Missing (never selected) | Not in training vector at all | Still not selected/retrained on, as of 2026-09-03 |
| Mean-reversion z-score (`fast_mean_rev_z`) | Missing (never wired) | Schema field exists but is never computed — sits at its default | Dropped outright, 2026-09-04 (see per-dim table above) — this row is now permanently closed, not just "still unwired" |
| Skewness (`skewness_idx`) | Source changed (value discontinuity) | TS3 time-bar cadence (stale, once-per-15-min) | Replaced with activity-clock (tick-native) source, `7c51f33`, 2026-08-27 |

## Relationship to the broader migration

These 5 dims are this system's existing proof-of-concept for the eventual full time-clock →
imbalance-clock cutover discussed in `docs/superpowers/specs/2026-09-06-imbalance-work-rate-spec.md`
and its sibling Force Index docs — they're the only `ObservationData` fields that already live
natively on `ImbalanceBarEngine`'s clock. Any future `ImbalanceMarketObservation`/`ImbalanceTrainingEvent`
schema work should treat this doc, not the 2026-08-31 one, as its starting inventory.
