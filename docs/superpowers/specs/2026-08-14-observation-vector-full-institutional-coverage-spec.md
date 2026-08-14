# Spec: Full-Coverage Institutional Audit of the 16D Observation Vector

## Purpose

`docs/superpowers/specs/2026-08-14-featurescaler-winsorization-and-dim3-shrinkage.md` (D1-D7) proved a
real, repeatable methodology for taking a dim from "unquestioned default" to "institutionally derived
from real data" — but applied it to exactly 2 of the vector's 16 dimensions (`dim1`, `dim3`). Verified
directly in code (`FeatureScaler.h`): those two are the *only* dims with a dimension-specific
winsorization bound. Every other dim still runs on the flat, never-re-derived
`STATE_WINSOR_SIGMA`/`ENERGY_WINSOR_SIGMA = 6.0` default — the exact same kind of unquestioned uniform
bound that turned out to need `45.0` for `dim1` and `4587.0` for `dim3` once actually checked. "Hasn't
been checked" is not "is fine" — D4 (a macro-anchor that looked safe until a real pathological event
was found) and D6 (a raw clamp that had *already* started silently clipping real data before anyone
looked) both prove that directly.

This spec's purpose is to make that check happen uniformly, closing the gap between "the two dims we
happened to investigate" and "the vector as a whole," and validate everything shipped so far against
live production data rather than only native tests.

**Correction to an earlier draft of this spec**: it originally listed `dim12`'s residual-zero rate as
an open anomaly needing root-cause investigation, citing `CLAUDE_BRIEF_093`/`095`. Reading those entries
in full (not just their titles) shows this was already resolved and closed *within* that same thread —
`CLAUDE_BRIEF_094` initially reopened it on a hypothesis, but `CLAUDE_BRIEF_095`'s full-file scan (all
2,076,141 samples, not a partial sample) found `dim12` "essentially flat 0.0% almost everywhere but
chunk 0" (chunk 0 being a known warmup-contamination artifact, unrelated), and
`GEMINI_BRIEF_095_RESPONSE` explicitly endorsed closing it as "healthy, continuous, and
production-ready." `dim12` is never mentioned again anywhere in the log after that. The stale "open"
status was carried forward from the dim1/dim3 spec's own citation without checking whether the thread it
pointed to had already resolved — exactly the "verify docs against the actual source before acting on
their status text" trap this project has hit before. Corrected below; `dim12` needs no root-cause task.

**Governing objective**, per the reframing that drove D7: the 16D vector is a near-exclusive Student-t
HMM input (`lbrnet/models/student_t_hmm.py`), so every bound decision in this spec is judged by "what
maximizes information delivered to the HMM," not "what's conservatively safe" — and D7 already
established, by reading the actual HMM M-step code, that the downstream consumer's own robust-t EM
weighting (`w_i=(nu+dim)/(nu+delta_i)`, Peel & McLachlan 2000) plus `covariance_type='diag'` (no
cross-dim ill-conditioning) makes wide inputs safe. That finding is a one-time derivation, not a
per-dim one — later units in this spec cite it, they don't re-derive it.

## Current State: per-dim audit status

`CLAUDE_BRIEF_095` (`../lbrnet/logs/rc_gemini.log:8172`) already ran a real, full-file (2,076,141 samples,
not a partial sample) `|z|>=6` clip-rate scan across every dim under the flat default, before tonight's
dim1/dim3 investigation existed. That data is real production evidence, not a fixture — reusing it below
instead of re-deriving from scratch, and using it to prioritize the remaining units by actual measured
severity rather than an arbitrary listing order. Cadence column added because it determines which real
data file is even valid to audit against (per-tick dims need `mes_continuous_ticks.parquet`; bar-gated
dims can use the matching timeframe's pre-aggregated file — `mes_tide_240m.parquet`/`mes_wave_60m.parquet`/
`mes_ripple_15m.parquet` for TS1/TS2/TS3 respectively).

| dim | field | mode | cadence | raw calculator | raw-clamp status | measured 6σ clip rate (full 2.08M-sample file, `CLAUDE_BRIEF_095`) | audit status |
|---|---|---|---|---|---|---|---|
| 0 | `log_variance_ratio` | SOFTLOGZ | **TS1 (240min), confirmed** (`TripleScreen1.cpp`) | `CalculateLogVariance` (`StudyHelperFunctions.cpp:3017`); raw clamp `[-6,+6]` verified fine (0/19,368 window/bar combos clip on real TS1 bars) | shrinkage enabled (D8, floor=0.000389), `DIM_WINSOR_SIGMA_OVERRIDE[0]=10.0` (GPD-derived on corrected z) | **done** — bar-close-only replica badly undersampled at first (0.021% vs production's 5.812%, TS1 is evaluated every tick against the live still-forming bar, not just at close); rebuilt as a genuine tick-level replica, found the same scale-collapse signature D8 generalized D2/D4 to fix, then a clean xi=+0.2533 GPD fit on the corrected z |
| 1 | `burstiness_index` | SOFTLOGZ | TS2 (60min) | `CalculateBurstiness` -> `cfc::ComputeBurstinessIndex` | `[-6,+6]`, verified (D6) | 7.848% (pre-fix) | **done** |
| 2 | `relative_range` | LOGZ | **TS2 (60min), live (references `sc.Index`)** confirmed (`TripleScreen2.cpp:273`) | `ComputeRelativeRange` (`CarryForwardCalculators.h`) | simple ratio, no clamp needed | 0.035% | **done, closed clean** — full-history bar-close screen (18,761 real 60min-bar windows) found ZERO exceedances, matching production's already-tiny rate; no dedicated tick-level replica needed (the cheap check already answered the question convincingly, despite the formula technically being live-referenced). |
| 3 | `correction_action` | SOFTLOGZ | TS2 (60min) | `CalculateRealizedVarianceRatio` -> `cfc::ComputeBurstinessIndex` | `[-10,+6]`, verified (D6) | 10.078% (pre-fix) | **done** |
| 4 | `vol_convexity` | LOGZ | **TS3 (15min), bar-gated/historical-only** confirmed via source comment (never reads the live bar) | `CalculateVolConvexity` (`StudyHelperFunctions.cpp:3322`) | unaudited | 2.713% | **done, full confidence** — a top-5-sample check first looked ambiguous, but classifying the FULL 85-event population resolved it: 51 (60%) contaminated, 34 (40%) genuine, the same scale-collapse pattern as dim3/dim9/dim0/dim7/dim6, just not yet checked at population scale. Shrinkage generalized to the LOGZ path for the first time (`SHRINKAGE_SCALE_MIN[4]=0.211521`, shared `ComputeShrinkageZ()` helper). Corrected fit: n_tail=76, shape(xi)=-0.3580 (Weibull/bounded), theoretical endpoint 8.837. `LOGZ_WINSOR_SIGMA_OVERRIDE[4]=12.0`, real margin given the modest sample. |
| 5 | `lempel_ziv` | SOFTLOGZ (static) | n/a | LZ76 on n=64 binary string | discrete, bounded `[0,1]` by construction | 0.000% | **exempt** (bounded, non-adaptive by design) |
| 6 | `hurst_exponent` | SOFTLOGZ | **TS1 (240min), confirmed** — two `CalculateHurstExponent` overloads exist (`StudyHelperFunctions.cpp:2489`/`2639`); confirmed the observation-vector write is `TripleScreen1.cpp:629` (`obs->mutate_hurst_exponent(hurst)`, fed by the `:483` `macro_window_n`-based overload) — `TripleScreen2.cpp:715`'s call feeds a different, non-observation-vector consumer | `CalculateHurstExponent(sc, macro_window_n, 8)` | not shared, raw is a DFA slope clamped `[0,1.5]` at the source | shrinkage enabled (floor=0.000145, exact-formula-derived), `DIM_WINSOR_SIGMA_OVERRIDE[6]=345.0` (GPD-derived, p=1/N return level) | **done, full confidence** — an exact port of the DFA algorithm (not the earlier approximate replica) found a genuinely elevated tail (pre-shrinkage `|z|>=6` rate 24.85%, an order of magnitude worse than any other dim); a tail-conditional noise decomposition (with Gemini, `CLAUDE_BRIEF_101/102`) empirically ruled out a DFA-estimation-instability artifact — intra-bar noise is 4.3x *smaller*, not larger, during the collapsed-MAD episodes producing the extreme z-scores — confirming this is the same scale-collapse mechanism as the other four dims, just far more prevalent. Post-shrinkage: n_tail=11,023 (14.80%, the largest confirmed-genuine tail in this initiative), shape(xi)=+0.3014. |
| 7 | `micro_asymmetry` | SOFTLOGZ | **TS3 (15min), per-tick, confirmed** (`TripleScreen3.cpp:640-657`, reads accumulating `sc.AskVolume`/`BidVolume` every tick) | `ComputeMicroAsymmetry` (`OrderFlowAsymmetryEngine.h:26`); bounded `[-1,1]` by construction, no raw clamp needed | shrinkage enabled (D8, floor=0.00733), `DIM_WINSOR_SIGMA_OVERRIDE[7]=36.0` (GPD-derived on corrected z, p=1/N return level) | **done** — Gemini's `12.0` prior was superseded, not confirmed: real trace found the same scale-collapse signature as dim3/dim9/dim0 (top-10 events' local MAD all below the series' own p1), needed D8's shrinkage fix first; corrected GPD fit gives xi=+0.0580 (barely Frechet) |
| 8 | `fisher_info` | SOFTLOGZ | **TS1 (240min), confirmed** (`TripleScreen1.cpp`) | `CalculateFisherInformation` -> `cfc::ComputeFisherInformation` | verified exactly bounded `[-2.6467,+2.6467]` by construction (`+-0.99` clamp confirmed against source) | `DIM_WINSOR_SIGMA_OVERRIDE[8]=20.0` (GPD-derived, margin above a smaller-sample fit) | **done** — clean scale-collapse trace (no shrinkage needed), genuinely bounded (Weibull) tail, `xi=-1.2574`, fitted endpoint `14.216`; set with real margin given the smaller tail sample (`n_tail=330`) than dim1/dim3/dim9/dim0/dim7 had. |
| 9 | `tail_index` | SOFTLOGZ | **per-tick, confirmed** (`ContextManager.cpp:220`, `TailRiskEngine::AddObservation` on every price change, TS-agnostic) | `TailRiskEngine::GetHillAlpha()` (Hill estimator, own k-selection + EWMA smoothing, 2026-08-13) | **source-bounded**: `ContextManager.cpp:526` clamps `alpha` to `[1.1,8.0]` before it ever reaches `FeatureScaler` | shrinkage enabled (D8, floor=0.0438), `DIM_WINSOR_SIGMA_OVERRIDE[9]=10.0` (GPD-derived on corrected z) | **done** — the worst dim in the vector by clip rate (16.247%), and its first-pass GPD fit (pre-shrinkage) looked like the worst by far in tail severity too (xi=+0.78, ~360,000-sigma implied bound) until traced: pure scale-collapse artifact (raw only moved 1.4->8.0 against a collapsed local MAD). Corrected fit flips domain entirely to xi=-0.3259 (Weibull/bounded, same type as dim1), theoretical wall 9.687 |
| 10 | `skewness_idx` | SOFTLOGZ | **TS3 (15min), confirmed** (`UpdateObservationVectorSubgraphs`, called from `TripleScreen3.cpp:694`) | `CalculateSkewness` -> `BowleySkewness` | bounded `[-1,1]` by construction (quartile skewness) | none — audited, flat default kept | **done, closed clean** — tick-level replica confirms negligible practical impact (0.041% clip rate, only 31 exceedances, too few for a reliable GPD fit); a weak scale-collapse signal is present but not acted on (ruthless simplicity — the flat default already performs fine here). |
| 11 | `amihud_illiquidity` | SOFTLOGZ | **TS3 (15min), confirmed** (same `UpdateObservationVectorSubgraphs` call site as dim10/dim12) | `CalculateAmihudIlliquidity` -> `cfc::ComputeAmihudIlliquidity` | historical-only by construction (bar-gated, never sees the live still-forming bar — confirmed via source, unlike dim0/1/3/6/7/8/9/10/15) | none — audited, flat default kept | **done, closed clean** — `CLAUDE_BRIEF_095`'s real production telemetry already shows `0.000%` clip rate, the design itself excludes the live-bar undersampling risk every other audited dim had, and it already has a dedicated floor fix (`AMIHUD_ABSOLUTE_FLOOR`, `CLAUDE_BRIEF_086`/`087`). No further replica needed. |
| 12 | `liq_fragility` | LOGZ | **TS3 (15min), bar-gated/historical-only** confirmed via source comment (same category as dim11 — never reads the live bar) | `CalculateLiquidityFragility` (`StudyHelperFunctions.cpp:2811`) | unaudited | 0.727% (and independently confirmed healthy, see below) | **done** — resolved-not-anomaly (see correction above) AND now has a real derived bound: full-history (75,051-sample) replica, clean trace (no scale-collapse signature), n_tail=207, shape(xi)=-0.2276 (Weibull/bounded), theoretical endpoint 11.485. `LOGZ_WINSOR_SIGMA_OVERRIDE[12]=12.0`, first dim to use the new LOGZ per-dim mechanism (Task 4). |
| 13 | `recurrence_rate` | SOFTLOGZ (static) | n/a | RQA epsilon-selected recurrence | bounded, structural | 0.000% | **done** |
| 14 | `fractal_dim` | SOFTLOGZ (static) | n/a | Sevcik fractal dimension | bounded, structural | 0.000% | **done** |
| 15 | `mean_rev_z` | SOFTLOGZ | **TS3 (15min), confirmed** (`TripleScreen3.cpp`) | `CalculateMeanReversionSpeed` (`StudyHelperFunctions.cpp:3238`) | bounded `[0,5]` by construction (clamped score) | none — audited, flat default kept | **done, closed clean** — tick-level replica confirms negligible practical impact (0.028% clip rate, only 14 exceedances, far too few for any reliable fit). |

**Update (2026-08-14, same day, later pass):** `dim9`, `dim0`, and `dim7` moved from "unaudited"/"needs
derivation" to **done** — see Unit A below and D8 in
`docs/superpowers/specs/2026-08-14-featurescaler-winsorization-and-dim3-shrinkage.md`. Their first-pass
GPD fits were caught as contaminated by an estimator-reliability artifact (the same scale-collapse
signature D4 originally found and fixed for `dim3`, now confirmed NOT `dim3`-specific) before any wrong
bound shipped.

**Update (2026-08-14, same day, later still — Task 3):** `dim6` and `dim8` also moved to **done**
(`dim6` with a disclosed approximate-formula caveat, see its table row); `dim10`, `dim11`, `dim15`
audited and **closed clean** — real measured clip rates were already negligible and each had too few
tail exceedances for a reliable fit, so no bound change was warranted (forcing one would have been
complexity without evidence behind it). `dim6` was the *fourth* independent confirmation of the same
scale-collapse signature D4 originally found for `dim3` — of five dims checked for it so far
(`dim9`, `dim0`, `dim7`, `dim6` show it; `dim8` doesn't), four needed the fix. Remaining: `dim4`, `dim2`
(Unit C, LOGZ). Three (5, 13, 14) are exempt by construction (bounded, static-scaled, already re-derived
against real data). `dim12` is resolved (not an anomaly — corrected above) and gets its routine bound
check in Unit C. **Update (2026-08-14, Task 4 complete, then a same-day `dim6` follow-up):** all 16 dims
now audited, and **all with full confidence** — zero dims carry a disclosed numeric gap. Final tally: 15
done or closed clean (1, 3, 9, 0, 7, 8, 10, 11, 12, 15, 4, 2, 6, plus 5/13/14 exempt by construction).
`dim4` initially looked ambiguous from a top-5-event sample, but classifying its FULL 85-event
population resolved it decisively (60% contaminated / 40% genuine, same pattern as every other confirmed
dim) — shrinkage was generalized to the LOGZ path for the first time to fix it. `dim2` closed clean via
a cheap screen without needing the full replica treatment its live-bar reference would otherwise call
for, since two independent measurements (production telemetry + a large real bar-close sample) already
agreed emphatically. `dim6` was the last holdout: an exact port of its DFA algorithm (replacing the
earlier approximate replica) found a genuinely elevated tail an order of magnitude worse than any other
dim, and a tail-conditional noise decomposition — run jointly with Gemini,
`logs/rc_gemini.log` `CLAUDE_BRIEF_101`/`102` — empirically ruled out a DFA-instability artifact before
any bound was shipped. Zero dims remain unaudited or under a disclosed caveat.

## The Audit Methodology (codified from D1-D7, as a repeatable playbook)

Every future per-dim audit in this spec follows the same six steps D1-D7 arrived at iteratively —
codifying it here so it's applied uniformly instead of reinvented per dim:

1. **Recompute the raw, pre-clamp calculator output directly from real market data**
   (`mes_wave_60m.parquet` / `mes_continuous_ticks.parquet`, matching the dim's actual timeframe —
   TS2/60min for most of these), independent of the whole `FeatureScaler` pipeline, swept across the
   calculator's real adaptive-window range (most use `CalculateAdaptiveObservationWindow`'s `[10,40]`
   bound; verify per-dim, some may use a fixed window instead).
2. **Check the raw calculator's own backstop clamp** (if any) for correct sizing *and* correct
   symmetry/shape for this dim's specific construction — do not assume a bound is safe just because a
   different dim sharing the same function is safe (D6's exact lesson: `dim3` inherited `dim1`'s `[-6,+6]`
   and was already clipping real data).
3. **Feed the raw series through `FeatureScaler`, capture `lastRawZ`** (the pre-winsorization z), and
   check whether the flat `6.0` default over-saturates. Do not assume either direction — dim1/dim3 both
   needed massive widening, but dim9's raw input is already source-bounded to `[1.1,8.0]`, dim10's to
   `[-1,1]` by construction, so their z-tails may genuinely be tame. Measure, don't extrapolate from
   dim1/dim3's result.
4. **Before trusting any fit, check whether the over-saturation is a genuine tail or a scale-collapse
   artifact.** Added after D8 (generalizing D2/D4's shrinkage fix beyond `dim3`): trace the largest
   `|z|` events and compare each one's local MAD against that dim's own local-MAD distribution. If the
   local MAD at the event sits at/below the series' own p1 while the *raw* deviation is only modest,
   that's the same signature D4 found for `dim3` — a purely reactive floor-gate/carry-forward
   estimator cannot protect against the first large deviation after a quiet regime, and fitting a GPD
   on top of it fits the artifact, not the market (confirmed on `dim9`/`dim0`/`dim7`: contaminated
   fits gave `xi` values and implied bounds off by orders of magnitude, in `dim9`'s case flipping the
   fitted extreme-value domain entirely). If found, enable `SHRINKAGE_SCALE_MIN[i]` for this dim
   (5th-percentile-of-real-rolling-local-MAD floor, same D4 methodology) *before* proceeding — do not
   derive a bound on contaminated z.
5. **Fit a GPD to the exceedances above the natural threshold** (`u=6` matches the existing default
   and is a reasonable starting POT threshold for any dim using it), on the corrected z if step 4
   enabled shrinkage. Determine the domain from the fitted shape `xi`:
   - `xi<0` (Weibull, finite endpoint): bootstrap the endpoint (2000 resamples, same procedure as
     dim1's) and set the bound at the resampled ceiling.
   - `xi>0` (Frechet, unbounded): anchor a target exceedance probability to `p=1/N_total` (same
     reasoning as dim3's — "as wide as findings support" means the widest return level the actual
     collected evidence backs, not further extrapolation) and compute the return level via
     `b = u + (scale/xi) * ((p/zeta_u)^-xi - 1)`.
   - `xi~=0` (exponential limiting case): use `b = u - scale*ln(p/zeta_u)`.
6. **Downstream-consumer safety is already established, do not re-derive it.** D7 confirmed
   (`lbrnet/models/student_t_hmm.py`, read directly): `covariance_type='diag'` (no cross-dim
   ill-conditioning) and the M-step's Peel & McLachlan (2000) robust-t weighting already downweight
   extreme observations for every dim, not just dim1/dim3. Cite this, do not re-verify it per dim.
7. **Implement as a dedicated per-dim constant** (never share a bound across dims by default; if
   shrinkage was enabled in step 4, also add its floor to `SHRINKAGE_SCALE_MIN[i]`), TDD-verify
   against a real-data fixture (red against the old default, green after), verify against
   `./build_dll.sh`. Document the derivation here and in
   `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`.

Steps 1-3 are cheap and should run for every remaining dim regardless of expected outcome — a dim
that turns out to already be fine under the flat default is a valid, useful result (closes the
"unaudited" status honestly) and does not require steps 4-7.

## Units (remaining gaps, prioritized)

### Unit A — `dim9`/`dim0`/`dim7`: derive the bound for confirmed over-saturating dims — **DONE**

Not "check whether these over-saturate" — `CLAUDE_BRIEF_095`'s full-file scan already answered that on
real production data for `dim9` (16.247%, the worst dim in the entire vector) and `dim7` (6.758%);
`dim0`'s own tick-level replica (built after its bar-close-only first attempt badly undersampled,
0.021% vs production's 5.812%) confirmed the same for it. Gemini's `25.0`/`12.0` literature priors were
never run through the GPD+bootstrap methodology D7 used for dim1/dim3 — checked now, and superseded:
all three needed D8's shrinkage generalization first (their first-pass GPD fits, run before shrinkage
was enabled for them, were fitted on scale-collapse-contaminated z and produced nonsense — `dim9`'s
apparent `xi=0.78`/360,000-sigma bound flipped entirely to `xi=-0.3259`/bounded once corrected). Final
bounds, GPD-derived on the corrected z (see D8 in
`docs/superpowers/specs/2026-08-14-featurescaler-winsorization-and-dim3-shrinkage.md`):
`dim9=10.0`, `dim0=262.0`, `dim7=36.0`. Implemented, TDD-verified against real-data fixtures, `build_dll.sh`
clean.

### Unit B — Remaining SOFTLOGZ dims (6, 8, 10, 11, 15 done/closed; `dim12` moved to Unit C) — **DONE**

`hurst_exponent`, `fisher_info`, `skewness_idx`, `amihud_illiquidity`, `mean_rev_z`. All five audited.
Confirmed via source (not assumed) that `dim6`/`dim8` are TS1 and reference the live still-forming bar
the same way `dim0` did (needed genuine tick-level replicas, not bar-close data — a quick bar-close
screen showed the same undersampling gap dim0's first attempt had, 0.000-0.593% vs production's
0.78-3.26%, confirming the rebuild was necessary before trusting any result). `dim11` was the one
exception found: its own source comment confirms it's bar-gated and historical-only by design (never
reads the live bar), so its already-near-zero production clip rate (`0.000%`) could be trusted directly
without a replica.

Results: `dim6` needed D8's shrinkage (4th independent confirmation of the same signature, following
`dim3`/`dim9`/`dim0`/`dim7`) — floor derived from an approximate DFA replica (Python re-implementation,
not the exact C++ algorithm), so the qualitative fix is trusted but the z-layer bound is deliberately
left unset pending a more faithful replica. `dim8` showed a clean trace (no shrinkage needed) and a
genuinely bounded raw value (`[-2.6467,+2.6467]`, verified against `cfc::ComputeFisherInformation`'s own
`+-0.99` clamp) — bound set with real margin given a smaller tail sample (`n_tail=330`) than the other
GPD-derived dims. `dim10`/`dim11`/`dim15` closed clean — negligible real-world impact (clip rates
0.00-0.041%, too few tail exceedances for any reliable fit), no bound change forced.

### Unit C — LOGZ dims (2, 4, 12): mode-appropriateness + bound audit — **DONE**

Two compounding gaps going in, not one: (a) none of the three LOGZ dims had ever had their winsorization
bound questioned the way SOFTLOGZ dims now have — `ENERGY_WINSOR_SIGMA=6.0` was flat across all three
with no per-dim override mechanism at all. **Fixed**: added `LOGZ_WINSOR_SIGMA_OVERRIDE[N_DIMS]`, the
same `0.0f`-sentinel array pattern SOFTLOGZ's `DIM_WINSOR_SIGMA_OVERRIDE` uses, wired into the LOGZ
path's hard z-clamp. No shrinkage mechanism was needed for any LOGZ dim — unlike SOFTLOGZ, `ToLogEnergy`
already compresses the raw value's magnitude *before* the rolling median/MAD ever sees it (SOFTLOGZ
compresses *after*), and the two LOGZ dims with real signal traced clean or only weakly ambiguous, not
the repeated dramatic collapse-then-modest-deviation pattern SOFTLOGZ kept showing. (b) whether `LOGZ`
(as opposed to `SOFTLOGZ`) is even the right scaling mode for each of these three was flagged as a
design question out of scope to resolve from first principles here — still true, not investigated
further; worth a future look, no evidence surfaced during this audit that forced the question. `dim12`
was not blocked on anything — its earlier anomaly status was itself a stale citation (corrected in this
spec's Purpose section); it just got the same
routine audit as `dim2`/`dim4`.

**Results**: confirmed via source that `dim4`/`dim12` are bar-gated/historical-only (same category as
Task 3's `dim11` — never reference the live still-forming bar), so real bar-close replicas were the
correct methodology, not an approximation — built from the FULL multi-year tick history (75,085 bars),
not just the shorter Sep-Oct 2023 slice used for the tick-native SOFTLOGZ dims, since bar-gated
evaluation is cheap enough to afford the longer window and a longer window matters for rare-tail
sampling. `dim2` (confirmed live-referenced, `TripleScreen2.cpp:273`) closed clean via a cheap
full-history bar-close screen (zero exceedances across 18,761 real windows) rather than the full
tick-level rebuild its live-bar reference would otherwise call for — justified because both independent
signals (that screen and production telemetry) already agreed emphatically, the same "the cheap check
already answered convincingly" closure Task 3 used for `dim11`/`dim15`. `dim12` traced clean (207 real
exceedances, no contamination signature, genuinely bounded Weibull tail) and got a confident bound
(`12.0`) directly. `dim4` initially looked like the same ambiguous case as `dim6` — a top-5-event sample
split 2 contaminated / 3 clean, no clear verdict — but classifying the FULL 85-event population
(same-day follow-up, not deferred) resolved it decisively: 51 (60%) contaminated, 34 (40%) genuine, the
identical majority-contaminated pattern already confirmed on 5 other dims. That meant `dim4` needed
shrinkage, not a `dim6`-style deferral — and since shrinkage had only ever been built for the SOFTLOGZ
path, this required generalizing it to LOGZ for the first time: extracted the shared blend computation
into `ComputeShrinkageZ()` (used by both paths now) rather than duplicating the ~40-line mechanism a
second time. Corrected fit on the shrinkage-blended z: `xi=-0.3580` (Weibull/bounded), theoretical
endpoint `8.837`, `n_tail=76`. `LOGZ_WINSOR_SIGMA_OVERRIDE[4]=12.0` — real margin given the still-modest
sample, same proportional-margin logic as `dim8`'s smaller-sample case. Full confidence, no caveat.

### Unit D — Production validation of D1-D7

Everything shipped in the dim1/dim3 spec is native-test-verified and `build_dll.sh`-verified only —
never confirmed against a live collection run. `./deploy_mindfultrader.sh` plus a fresh collection,
compared against the native test predictions (same ~1pp validation bar `CLAUDE_BRIEF_097` already
established for dim1/dim3's original rail-hit rates), closes this. Should run *before* Units B/C/D so
the audit methodology itself gets one real-world confirmation before being applied nine more times.

### Unit E — Documentation/governance sync

- Commit the Gang-doc entries (`docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`)
  Gemini proposed in `GEMINI_BRIEF_099_RESPONSE` (Entries A-C), now that D7 resolved the open questions
  that were blocking them — with the numbers corrected to the actual shipped values (`45.0`, `4587.0`,
  the measured GPD parameters), not the earlier illustrative ones.
- Sync `../docs/RISK_MANAGEMENT_SYSTEM.md` §5.3/§10 and `../docs/TRADE_EXECUTION_SYSTEM.md` §H.6, which
  still cite pre-migration (moment-based) kurtosis numbers superseded by the 2026-08-13 Moors-kurtosis
  migration.
- `lbrnet/scripts/context_preflight.py`'s D3 `chronic_zero_threshold` gate — `lbrnet`-repo work, tracked
  here only as a pointer, same as the 2026-08-13 plan's Unit 5 convention (no `lbrnet` edits from this
  repo).
- Config drift: `/mnt/c/Trading/config/`'s two JSON files carry their own copy of migrated thresholds and
  won't travel to a new machine without a manual sync — flag, don't silently fix (needs the user's live
  environment access).

## Acceptance Criteria for "institutional-grade 16D vector"

- **[MET]** Every one of the 16 dims has a winsorization bound (or an explicit, documented
  static/structural exemption) traceable to real-data derivation via the methodology above — not an
  inherited or unquestioned default, and with zero disclosed numeric gaps remaining. `dim4` and `dim6`
  both went through an intermediate "mechanism fixed, exact number deferred" state during the audit but
  both were fully resolved the same day — `dim4` via full-population exceedance classification, `dim6`
  via an exact-formula DFA replica plus a Gemini-assisted tail-conditional noise decomposition that
  ruled out an estimation-instability artifact before any bound shipped.
- **[MET]** No dim has a known, open, uncharacterized anomaly (closes `dim12`).
- **[NOT MET, next step]** D1-D8 (and every unit added by this spec) validated against at least one
  live production collection run, not only native tests — Unit D, blocked on your explicit go-ahead to
  deploy.
- **[NOT MET]** `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md` and the governance
  docs (`RISK_MANAGEMENT_SYSTEM.md`, `TRADE_EXECUTION_SYSTEM.md`) reflect the actually-shipped code —
  Unit E, not yet started.

## Non-Goals

- Redesigning what any dim measures (e.g. swapping the Hurst estimator methodology, changing what
  `micro_asymmetry` computes) — this is a scaling/bound audit, not a feature-engineering redesign.
- `lbrnet`-side changes beyond what D7 already confirmed safe by reading the code — Unit E's `lbrnet`
  item is a pointer, not a task this repo executes.
- Re-litigating D1-D7's already-shipped, already-verified dim1/dim3 numbers.
