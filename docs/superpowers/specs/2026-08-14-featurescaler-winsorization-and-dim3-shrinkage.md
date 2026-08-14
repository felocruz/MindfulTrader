# Spec: Observation-Vector Winsorization Audit + dim3 Adaptive-Scale Shrinkage

Date: 2026-08-14
Owner: C++ execution layer (MindfulTrader) + `lbrnet` empirical tooling
Scope: `include/FeatureScaler.h`, `tests/cpp/test_feature_scaler.cpp`,
`tests/cpp/fixtures_dim3_raw.h`, `../lbrnet/scripts/context_preflight.py`,
`../lbrnet/scripts/validate_lbr_file.py`

Prerequisite reading: `../lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_093` through `098` (the full
empirical/literature trail this spec summarizes — baseline-vs-current file comparison, per-dim
residual-zero triage, the winsorization-bound discovery, and the Gemini correspondence that
converged on the dim1/dim3 fixes). Do not confuse with, or reopen,
`docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md` (the *earlier*,
already-shipped dedupe-at-ingestion fix for dims 1/2/7/8/11 zero-collapse — this spec is downstream
of that one and assumes it's already deployed).

## Problem Statement

A live collection run (`event_data_20260813_191757.context`, 2,076,141 aligned pairs, real MES
replay 2023-09-01..10-20) was checked for 16D observation-vector quality after the earlier
dedupe-at-ingestion fix shipped. Two distinct, unrelated defects were found:

1. **Six dims showed a small residual exact-zero plateau** even deep into the file, past any
   warmup window (dims 0/3/6/8/12/15, 2-5% each). Traced per-dim (`CLAUDE_BRIEF_093`): five are
   legitimate, already-explained mechanisms (cold-start sentinels, DFA clamp floors, Fisher-
   transform midpoint crossings, momentum-regime gating) — not defects. `dim12` (`liq_fragility`)
   remains genuinely unconfirmed (zero D2 dominance alerts across 1.25M real samples argues against
   the leading hypothesis; not pursued further in this spec).
2. **A single global 6-sigma winsorization bound (`STATE_WINSOR_SIGMA`/`ENERGY_WINSOR_SIGMA`,
   `FeatureScaler.h`) is applied identically to all 16 dims regardless of each dim's actual tail
   behavior.** Full-file scan of rail-hit rates (`CLAUDE_BRIEF_095`):

   ```
   dim9  tail_index          16.247%   <- worst, ~2x next-highest
   dim3  correction_action   10.078%
   dim1  burstiness_index     7.848%
   dim7  micro_asymmetry      6.758%
   (all other dims <=5.8%, four at exactly 0.000%)
   ```

   `dim9` is a Hill tail-index estimator — the one dim where a Gaussian-calibrated 6-sigma bound is
   structurally the wrong tool, since its entire purpose is measuring how far real returns deviate
   from Gaussian tail behavior.

## Investigation: separating "needs a wider bound" from "estimator is broken"

Both `dim1` and `dim3` share the same raw formula (`cfc::ComputeBurstinessIndex`,
`CarryForwardCalculators.h`) but turned out to need **different fixes**, discovered only by
building real tick-level replicas against `../lbrnet/data/raw/mes_continuous_ticks.parquet` (the
current-bar-forming-intra-tick dynamic that a closed-bar-only replica cannot reproduce — see
`CLAUDE_BRIEF_097` for the full methodology, including the wrong-calendar-window false start that
forced this correction) over the actual replay window, and comparing against real live telemetry:

- **`dim1`**: replica matched live within 0.2pp (7.63% vs 7.85% at 6-sigma). Tail *tapers* cleanly
  — only 0.18% of samples still exceed 25-sigma. **Diagnosis: needs a wider winsorization bound,
  nothing else.** Confirmed independently by two literature passes (ours and Gemini's) and the
  empirical measurement — three-way agreement.
- **`dim3`**: replica also matched live (11.12% vs 10.08%), but the tail does **not** taper —
  4.71% of samples still exceeded even a 25-sigma bound, with `max|z| = 43,330`. Traced to a
  genuine MAD/floor co-collapse: during sustained (multi-recalibration-interval) quiet-clustering
  regimes, the rolling MAD shrinks far enough that a routine subsequent deviation divides by a
  near-zero denominator. **Diagnosis: the scale *estimator* is unreliable in this regime, not
  merely under-bounded — widening the clamp cannot fix an unreliable denominator.**

Literature grounding for the `dim3` fix (`CLAUDE_BRIEF_097`/`098`/responses, two independent
passes, both converging): GARCH's constant `omega` acting as an irreducible-minimum precedent,
"volatility flooring" as an established risk-margining technique for exactly this failure mode,
MAD's documented discontinuous-influence-function instability under regime persistence, and
Ledoit-Wolf shrinkage as the correct blending framework (`Sigma_hat = delta*F_hat + (1-delta)*S`)
rather than a hard `max()` floor.

## Design Decisions

### D1. `dim1`: widen `STATE_WINSOR_SIGMA` for this dim to 25.0 — **IMPLEMENTED**

`DIM_WIDE_WINSOR_INDEX = 1` / `WIDE_STATE_WINSOR_SIGMA = 25.0f` in `FeatureScaler.h`; `ToSoftLogZ`
takes the winsor bound as a parameter (default `STATE_WINSOR_SIGMA`, i.e. unchanged for every other
dim) and the generic SOFTLOGZ path selects the per-dim bound before calling it. Verified with the
same discipline as D2 below: a real tick-level `dim1` replica (`tests/cpp/fixtures_dim1_raw.h`,
225k real samples) added to `tests/cpp/test_feature_scaler.cpp`, asserting the *true* pre-clamp
`lastRawZ` tail actually tapers past the new bound (measured on this fixture: 4.51% at 6-sigma vs.
0.52% at 25-sigma — an ~8.6x drop, confirming the population isn't just shifted past a different
threshold). Gemini's proposed implementation (`GEMINI_BRIEF_098_RESPONSE`, 3-tier: dim9=25,
dims 1/3/7=12) is **not** what shipped for dim1 — 25.0 was used (matching the empirically-verified
value from `CLAUDE_BRIEF_097`, where the *measured* tail-taper point was 25-sigma, not Gemini's
12-sigma prior for this dim) since the empirical result, not the literature prior, is authoritative
here. `dim9`/`dim7` remain unimplemented — see Status below.

### D2. `dim3`: replace the binary floor-gate/carry-forward mechanism with a shrinkage-blended scale — **IMPLEMENTED**

Scope: `dim3` (`DIM_MACRO_SHRINKAGE_INDEX`) only. `dim1` does not need this (clean tail, D1 suffices).
The other 14 dims have zero empirical evidence of this failure mode; extending the mechanism to them
without a diagnosed reason would be scope creep, not rigor.

Mechanism (`FeatureScaler.h`, `include/FeatureScaler.h` around `DIM_MACRO_SHRINKAGE_INDEX`):

- `macroScaleEwma[i]`: O(1)-memory EWMA of absolute deviation from the local median, avoiding a
  large persistent buffer (the ~100k-sample literal buffer Gemini's first pass suggested would
  fight this codebase's no-heap-allocation hot-path constraint). Seeded from `bufScale` at
  `Calibrate()` time.
- Shrinkage weight `w`: a log-space sigmoid on `madScale / macroScaleEwma` (the compression ratio)
  — **not** `dominanceRatio[i]` as first attempted (see "Bugs found" below).
- `scale_effective = max(w*madScale + (1-w)*macroScaleEwma, ABSOLUTE_FLOOR)`, always live, no
  carry-forward decay for this dim.
- The macro anchor's own update rate is gated by `w` (a ratchet: it only tracks fresh data when
  currently trusted, freezing during suspected-compressed periods) — otherwise a sufficiently long
  quiet regime drags the "long-horizon" anchor down with it, defeating the point of having one.

### D3. `dim3`: also widen its winsorization bound to 25.0 (shared with dim1) — **SUPERSEDED BY D5, kept for the record**

**This section's central empirical claim was wrong — see D5.** The `max|z|=10.69` / "zero samples
beyond 25-sigma" numbers below were computed by a script with a real bug (a `std::vector<float>`
constructed from a byte range, implicitly converting each individual byte to a float instead of
reinterpreting 4-byte groups — corrupted data, not dim3's real behavior). Left as originally written
below so the record shows what was believed and why, rather than silently rewriting history; D5
has the corrected numbers and the actual shipped fix (`DIM3_WIDE_WINSOR_SIGMA=500`, not shared
with dim1's 25).

Raised directly by the question "given the asymmetric importance of properly recognizing fat tails
(Black Swans), would shrinking dim3's residual rate further make the system more robust?" — the
honest answer required checking the literature and re-measuring rather than assuming "less clamping
= better" or "more clamping = safer." Two things had to be true before this was safe to ship, and
both were checked, not assumed:

1. **Is D3 destroying real tail-risk signal, or restoring it?** Hard winsorization is flagged in the
   literature (Taleb, *Statistical Consequences of Fat Tails*; Pickands-Balkema-de Haan/GPD
   exceedance theory) as discarding tail-severity information rather than smoothly compressing it —
   the whole reason `SoftLogZ`'s `log1p` exists is to preserve *relative* severity, but a clamp
   applied before the log transform defeats that for anything past the bound. D2 already fixed the
   *estimator*; D3 fixes the *representation* of the (now-correct) values D2 produces. This is
   restoring information the pipeline was already destroying, not smoothing away real risk.
2. **Was the D2-measured 225k-sample fixture (`fixtures_dim3_raw.h`) representative of dim3's true
   tail, or an artifact of the adversarial slice chosen specifically to contain the known worst
   event?** Re-ran the full, unbiased 1,467,159-sample real series (not the deliberately-adversarial
   225k regression slice) through the post-D2 code: only 0.51% of all samples exceed 6-sigma at
   all, and the true max across the entire real dataset is `10.69` — nowhere near dim1's genuinely
   fat tail (which still has 0.18% beyond 25-sigma). This means dim3, once its estimator bug was
   fixed, does **not** need dim1's kind of heavy-tail accommodation; it just needs the same
   "don't flatten everything past a tight bound" fix, with the same already-vetted 25.0 value
   (reused, not a new separately-tuned number) giving comfortable margin over the observed max.

`ToSoftLogZ`'s call site inside `DIM_MACRO_SHRINKAGE_INDEX` now passes `WIDE_STATE_WINSOR_SIGMA`
directly (dim3 always takes this branch, so no per-call ternary is needed the way the generic path
needs one for dim1). Verified D3 does not change the underlying z-distribution at all (by design —
it only changes what happens to values once they exceed the bound): `|z|>=6` rate and `max|z|`
on the 225k adversarial fixture are unchanged post-D3 (3.11%, 493.93) from post-D2 — D3's job was
never to reduce that rate, only to stop discarding the severity information within it.

### D4. `dim3`: GARCH-omega-style irreducible floor on `macroScaleEwma` — **IMPLEMENTED**

Raised by explicitly re-analyzing D3 for further improvement rather than treating it as final —
found a fourth real, structural issue, not a diminishing-returns retune. Traced the fixture's
worst remaining event (`|z|=493.93`, driving D3's `max|z|` bound choice) to its exact origin:
`Calibrate()` seeded `macroScaleEwma` at `0.0000163` from a real, unusually-quiet 500-sample
burn-in window (raw dim3 barely moving: 0.679421 -> 0.679429 -> 0.679436 -> 0.679444 across dozens
of samples), the anchor barely grew over the next ~450 samples (it can only learn a larger scale
from deviations it observes, and a quiet window has none to offer), then the first real move
(raw jumping 0.679 -> 0.668) produced the blowup purely from dividing by this still-tiny anchor.
This is structural, not incidental: a purely reactive EWMA-of-observed-deviations anchor cannot
protect against the *first* large deviation after any sufficiently quiet regime, by construction —
same gap GARCH's `omega` term exists to close (an irreducible minimum tied to long-run behavior,
independent of the current window, not something that can itself decay to match a quiet regime).

Fix: `DIM3_MACRO_SCALE_MIN = 0.00007f`, applied as a floor both at the `Calibrate()` seed and every
per-tick ratchet update. Empirically grounded, not guessed: computed the rolling 300-sample local
MAD of the real raw `dim3` series across the full 1,467,159-sample live-replay window, sampled
every 500 ticks. The pathological seed (`0.0000163`) sits at that distribution's **1st percentile**
— rare but real (confirming this is a genuine, if uncommon, live-system risk, not a fixture
artifact) — and the true historical minimum local MAD is exactly `0.0`, meaning no floor derived
purely from recent local history can ever fully cover this; it needs an independent reference.
Set at the 5th percentile (`0.0000706`, rounded to `0.00007`) for real margin above the observed
case rather than sitting at its boundary, while staying >60x below the 25th percentile (`0.000854`)
and >1000x below the median (`0.004295`), so normal, non-degenerate regimes are unaffected.

Result on the fixture: the known worst event's `|z|` dropped `493.92 -> 294.00` — real, but
partial. Investigating further revealed the fixture contains **four distinct event clusters**
(idx 952, 14883, 19289, 26052), not one — D4 fixed the specific quiet-seed mechanism at the first,
but the new post-D4 max (`411.64`, at idx 14883) comes from a different cluster.

**Traced all four with the same rigor** (two new diagnostic-only fields, `lastLocalMad`/
`lastShrinkageWeight`, mirroring the `lastRawZ` precedent) and the answer is definitive, not
open-ended:

```
cluster  w       local MAD   macro anchor   raw jump   verdict
1 (952)  0.66    0.0000232   0.00007 <- AT THE D4 FLOOR   -0.0115   artifact (D4's mechanism)
2(14883) 1.00    0.00078     0.00032        -0.314        real (full trust, healthy scales)
3(19289) 0.0008  0.00039     0.00381        -0.859        real (correctly shrinks to a healthy macro)
4(26052) 0.9993  0.00795     0.00893        -2.679        real (largest raw jump, best-scaled regime)
```

Only cluster 1 shows both the local and macro scale estimates near-degenerate — D4's exact
mechanism, already mitigated. Clusters 2-4 all pair healthy, non-degenerate scale estimates
(one to two orders of magnitude above the D4 floor) with genuinely large raw moves in the
underlying metric -- cluster 4's raw jump (`2.679`, dim3 swinging from -0.85 to -3.53, a real,
large realized-variance collapse) is the largest of any cluster, occurring during the *best*-scaled
regime of the four. These are not estimator failures; they are the system correctly identifying
real, substantial regime changes. Per the Black-Swan framing that motivated re-analyzing D3 in the
first place: "fixing" clusters 2-4 further would mean suppressing genuine extreme-event signal from
the downstream HMM, not removing a defect. **Conclusion: `dim3`'s remaining tail is closed out as
resolved** -- one real artifact found and fixed (D4), three genuine tail events correctly
preserved, not further "fixable" without actively harming the system's fat-tail sensitivity.

### D5. `dim3`: correcting D3 — the "tame tail, max=10.69" claim was wrong; dim3 needs its own dedicated bound, not dim1's — **IMPLEMENTED**

Raised by a direct follow-up question ("where does the 3.11% rate stand now") that required
re-deriving numbers rather than restating D3's. Cross-checking surfaced a real bug: **D4's floor
(`DIM3_MACRO_SCALE_MIN`) produces an identical `|z|>=6` rate whether enabled or disabled** (verified
directly — compiled a copy of `FeatureScaler.h` with the constant forced to `0.0f`, ran the full
1,467,159-sample series through both, got `1.6220%` both times). That's expected (D4 only bounds
magnitude, never touches whether the threshold is crossed) — but it directly contradicts D3's
`0.51%` full-series figure, which was computed on the *same* post-D2 code. Two numbers computed
from the same inputs that shouldn't differ, differing, means one of the two measurements is wrong.

Found it: `fs_diag_full.cpp` (the script behind D3's numbers) read the binary fixture as
`std::vector<float> raw((istreambuf_iterator<char>(f)), istreambuf_iterator<char>())` — this
constructs a `vector<float>` from a range of **individual bytes**, implicitly converting each byte
value to a float (e.g. byte `0x3F` becomes `63.0f`) rather than reinterpreting 4-byte groups as
floats, then a subsequent `memcpy` copies from that corrupted, wrongly-typed buffer. D3's `10.69`
max and `0.51%` rate were computed on garbage, not real `dim3` values. Also re-verified dim1's
numbers with the corrected reader (`std::vector<char>` + `memcpy`) as a sanity check on whether
D1 was affected by the same class of bug — it wasn't (dim1's numbers came from the fixture-based
test harness, a different code path): full-series max `35.86`, `0.1767%` still beyond 25-sigma,
matching the originally-reported `~0.18%` almost exactly. D1 stands, now cross-validated twice.

Corrected full-series `dim3` numbers (proper byte-to-float reinterpretation):

```
|z|>=6 rate:  1.6220% (not 0.51%)
p50=9.50  p75=14.55  p90=24.22  p95=40.70  p99=271.61  p99.9=393.49  max=490.19 (not 10.69)
still exceeding 25-sigma:  0.1585% of ALL samples (9.77% of the 6-sigma population)
still exceeding 500-sigma: 0.0000%
```

Dim3's true tail is dramatically heavier than dim1's and a completely different shape: dim1 goes
p90-to-max at `19.78 -> 35.86` (~1.8x, smooth taper); dim3 goes p90-to-max at `24.22 -> 490.19`
(~20x — a small population of genuinely very large events, not a smoothly-tapering tail). Confirmed
via the D4 cluster tracing (same section, above) that this heavy tail is real signal, not estimator
artifact -- clusters 2-4 all showed healthy, non-degenerate local/macro scales paired with
genuinely large raw moves. So per the same Taleb/EVT reasoning D3 already established, this tail
must not be flattened either -- reusing dim1's 25.0 was never going to be enough regardless of the
diagnostic bug, once the true shape was known.

Fix: `DIM3_WIDE_WINSOR_SIGMA = 500.0f`, a dedicated constant (not shared with dim1's
`WIDE_STATE_WINSOR_SIGMA`), set to comfortably cover the observed max (`490.19`) across the entire
real dataset checked. `dim3`'s `ToSoftLogZ` call site now passes this dedicated constant. Re-verified
end to end: native suite passes (`|z|>=500` hits on the fixture: `0`), full-series check confirms
`0` samples exceed `500` across all `1,467,159` real samples, `build_dll.sh` succeeds cleanly.

Meta-note worth keeping: this is the second time a bug in *my own verification tooling*, not the
production code, produced a materially wrong conclusion I initially reported with confidence
(the first being the wrong-calendar-window false start in `CLAUDE_BRIEF_097`). Both were caught
because a later, independent measurement produced a number that didn't reconcile with an earlier
one, and the discrepancy was chased down rather than either number being trusted by default.

### D6. `dim3`: dedicated raw-clamp bound in `ComputeBurstinessIndex` (`CarryForwardCalculators.h`) — **IMPLEMENTED**

A separate layer from D1-D5 above, easy to conflate with them since both bound the same two dims:
D1-D5 all operate on the **z-score** (post robust-median/MAD scaling, inside `FeatureScaler.h`).
This one operates on the **raw physical ratio** (`log(rv_recent/rv_reference)`), one stage earlier,
inside the shared `cfc::ComputeBurstinessIndex` calculator both `dim1` (`CalculateBurstiness`) and
`dim3` (`CalculateRealizedVarianceRatio`) call — hard-clamped to `[-6,+6]` as a data-error backstop,
not a statistical bound (raw log-variance-ratios are inherently small; even a 20x variance surge is
only `log(20)~=3`).

Raised by a direct follow-up question (do the two dims need different raw bounds, not just
different z-bounds?). Verified against real 60-minute MES bars (`mes_wave_60m.parquet`, 19,592 bars)
by recomputing both dims' exact unclamped formulas independently of the whole `FeatureScaler`
pipeline, swept across the full adaptive window range `[10,40]` (`CalculateAdaptiveObservationWindow`'s
own bound):

```
dim1 true raw range, windows 10-40: [-4.587, +5.082]   0/606,577 window/bar combos clip
dim3 true raw range, windows 10-40: [-6.160, +0.787]   3/606,577 window/bar combos clip
```

Two findings, not one: (a) `dim3`'s shared `[-6,+6]` bound is **already clipping real, legitimate
data** — 3 genuine quiet-regime readings truncated to `-6.0`, not caught data corruption; not
hypothetical, already occurring in the historical sample. (b) `dim3`'s raw distribution is
**structurally asymmetric**, not just differently-scaled from `dim1`: its formula compares a
recent-half window against the FULL window (recent is a subset of full), which mechanically bounds
positive ratios (recent variance can't exceed full-window variance by much — max observed `+0.787`)
while leaving negative ratios comparatively unbounded (a quiet recent half against a volatile
historical full window — max observed `-6.160`). A symmetric bound never matched dim3's actual
shape; it was sized for dim1's genuinely symmetric construction (disjoint recent-half vs
older-half) and inherited by dim3 only because both call the same function.

Fix: `ComputeBurstinessIndex` gained two optional parameters (`clampLow = -6.0f, clampHigh = 6.0f`)
defaulting to the existing bound — `dim1`'s call site (`CalculateBurstiness`) and every other
existing caller are unaffected. `dim3`'s call site (`CalculateRealizedVarianceRatio`,
`StudyHelperFunctions.cpp`) now passes `(-10.0f, 6.0f)` — real margin below the observed `-6.160`
extreme, positive side left unchanged (never remotely close to binding; "hasn't been observed" isn't
grounds to tighten a backstop, same reasoning as D4's floor). TDD-verified:
`tests/cpp/test_carry_forward_calculators.cpp` gained 4 new checks reproducing the exact real
boundary case (`ratio = log(0.0021062/1.0) = -6.16`) — confirmed to fail to compile against the old
3-argument signature (red), then pass after the fix (green): the default bound still clips this
value to `-6.0`, the custom `[-10,+6]` bound preserves it at `-6.16` unclamped.

### D7. `dim1`/`dim3`: widen the z-layer winsorization bounds to the widest each dim's own findings can support — **IMPLEMENTED**

Reframed by an explicit new objective mid-investigation: the 16D vector is a near-exclusive
Student-t HMM input, so the governing question for these bounds is "what maximizes information
delivered to the HMM," not "what's a safely conservative number." That reframing, plus two pieces
of new evidence, justified moving both z-layer bounds past D1's `25.0` and D5's `500.0`:

1. **A 2000-resample bootstrap on dim1's GPD fit** (`scipy.stats.genpareto.fit` refit per resample,
   `rng = np.random.default_rng(20260814)`, on the real tail exceedances behind D1's fit) confirmed
   the negative shape parameter (`xi=-0.1956`, Weibull/bounded domain) is real, not a single-fit
   artifact: 2000/2000 draws finite, endpoint distribution `p5=43.25 p50=43.87 p95=44.52 max=45.12`.
   Since `xi<0` means a genuine finite theoretical endpoint (`u - scale/xi = 43.871`), "as wide as
   the findings support" has an actual, non-arbitrary ceiling here — not a target-probability choice.
2. **Reading the actual `lbrnet` Student-t HMM implementation** (`lbrnet/models/student_t_hmm.py`,
   not assumed) resolved the standing counter-question (would wider inputs destabilize downstream
   covariance/precision estimation?): `covariance_type='diag'` only (no cross-dim ill-conditioning
   possible), and the M-step already applies the textbook Peel & McLachlan (2000) robust-t EM
   weight `w_i=(nu+dim)/(nu+delta_i)` to both the mean and covariance updates — a sample's
   contribution is automatically downweighted as its Mahalanobis distance grows, the same mechanism
   that makes Student-t mixtures outlier-robust instead of Gaussian-fragile. `min_covar=1e-12` is
   documented in that file as an IEEE-754 floor only, not a statistical regularizer — confirming the
   real regularization job is already done by the t-weighting, not by upstream pre-clamping. Wide
   inputs are therefore safe for this specific downstream consumer.

`dim1`: no finite-endpoint case needs a target-probability derivation — the wall itself is the
answer. `WIDE_STATE_WINSOR_SIGMA: 25.0 -> 45.0`, clearing the full bootstrap distribution (point
estimate `43.871`, bootstrap `p95=44.52`, max draw `45.12`).

`dim3`: `xi=+0.6583` (Frechet/unbounded) means there is no wall to converge on — "as wide as
findings can make it" has to mean the widest return level the *actual collected evidence* supports
before extrapolating past it, not a ceiling. Anchored the target exceedance probability to `p=1/N`
(`N=1,467,159`, the full historical sample count this dim's GPD fit was built from) via the standard
POT return-level formula `b = u + (scale/xi) * ((p/zeta_u)^-xi - 1)`:

```
p=1e-5                          -> b=783
p=1/N ~= 6.8e-7 (chosen)         -> b=4,587
p=1e-7 (10x beyond what N informs) -> b=16,228
p=1e-8                            -> b=73,889
```

`p=1/N` reads as "as extreme as an event we'd expect to see about once across everything we've ever
recorded" — using the full weight of the evidence collected without extrapolating further into
probability territory the data can't actually speak to. `DIM3_WIDE_WINSOR_SIGMA: 500.0 -> 4587.0`.

Both bounds re-verified end to end after the change: native suite green (`dim1` rail-hit rate at
the new `45` bound: `0.0000%` on the 225k fixture; `dim3` rail-hit rate at the new `4587` bound:
`0` hits, `max|z|=411.64` unchanged since raw-z magnitude is independent of the winsor bound),
`build_dll.sh` succeeds cleanly. See `logs/rc_gemini.log` `CLAUDE_BRIEF_099/100` for the full
GPD/bootstrap derivation this builds on.

### D8. Generalize D2/D4's shrinkage-blend mechanism from `dim3`-only to any dim showing the same scale-collapse signature — **IMPLEMENTED**

Raised while auditing `dim9`/`dim0` under
`docs/superpowers/specs/2026-08-14-observation-vector-full-institutional-coverage-spec.md`: their
first-pass GPD fits (run against the *un*-shrinkage-corrected z, since only `dim3` had shrinkage
enabled at that point) produced absurd results -- `dim9` fit shape `xi=0.78` implying a
360,000-sigma-scale Frechet tail, `dim0` a max observed `|z|=-176`. Tracing the largest `|z|` events
with the exact same method D4 used for `dim3` (compare the local MAD at the event against the
series' own MAD distribution) found the identical signature on both: a *modest* raw deviation
landing on a local MAD that had independently collapsed to near its own series' 1st percentile.
`dim9`'s worst traced event: raw moved only `1.4->8.0` (both within its fixed `[1.1,8.0]` clamp) but
produced `z=1963` because local MAD had collapsed to `0.00336` against a typical `0.428`. `dim0`'s
worst: raw moved `~0.25` but produced `z=-176` against a collapsed local MAD of `0.00144`. Then
checked `dim7` too (flagged for its own audit in the same spec) -- same signature a third time,
`z=438` with every one of its top-10 events' local MAD sitting below the series' own p1.

Three of five dims checked with real over-saturation (plus the original `dim3`) sharing the exact
same underlying weakness is not a per-dim coincidence -- it means D2/D4's fix was correctly
diagnosed but under-generalized when first shipped: Ledoit-Wolf shrinkage-toward-a-stable-anchor and
a GARCH-omega-style variance floor are general small-sample scale-estimation principles, not
properties of `dim3`'s specific `RV_recent/RV_full` formula. Treating the fix as a `dim3`-only
special case (`DIM_MACRO_SHRINKAGE_INDEX == 3`) was the under-generalization, corrected here.

Fix: replaced the single `DIM_MACRO_SHRINKAGE_INDEX`/`DIM3_MACRO_SCALE_MIN` pair (and the
single-dim `DIM_WIDE_WINSOR_INDEX`/`WIDE_STATE_WINSOR_SIGMA` ternary alongside it) with two
per-dim arrays covering all `N_DIMS`:
- `SHRINKAGE_SCALE_MIN[N_DIMS]` -- `0.0f` = shrinkage disabled for this dim (keeps the simpler
  generic floor-gate/carry-forward path; **not every dim needs this**, e.g. `dim2`/`dim11` measured
  `0.000%`/`0.035%` clip rates with no collapse evidence and were deliberately left disabled rather
  than forced through unneeded complexity). Nonzero = shrinkage enabled with this dim's own
  empirically-derived floor (5th percentile of that dim's real rolling-local-MAD distribution,
  identical D4 methodology, sampled every 500 observations across the same
  2023-09-01..10-20 tick-replay window): `dim3=0.00007` (original), `dim9=0.0438`, `dim0=0.000389`,
  `dim7=0.00733`. Values are NOT interchangeable across dims -- each dim's raw values live on a
  completely different magnitude (Hill alpha ~1.1-8.0 vs. a bounded [-1,1] ratio vs. a log-variance
  ratio).
- `DIM_WINSOR_SIGMA_OVERRIDE[N_DIMS]` -- same `0.0f`-sentinel pattern, generalizing both the plain
  generic-path bound (`dim1=45.0`) and the shrinkage-path bound (`dim3=4587.0`) into one lookup used
  by both code paths.

`MACRO_SCALE_DECAY`/`SHRINKAGE_RATIO_MIDPOINT`/`SHRINKAGE_STEEPNESS` stay shared/global constants
-- no evidence yet that per-dim tuning of the blend dynamics themselves is needed, only the scale
floor. Regression safety: `dim3`'s exact behavior is unchanged (re-verified: identical
`3.1093%`/`0`-hits-at-4587/`max|z|=411.64` on the same fixture before and after this refactor) --
this is a pure generalization of the mechanism, not a behavior change for the dim it was proven on.

TDD: three new real-data fixtures (`fixtures_dim9_raw.h`/`fixtures_dim0_raw.h`/`fixtures_dim7_raw.h`,
same `[1000000:1225000]` slice convention as `fixtures_dim3_raw.h`, same tick-replay window). Tests
assert post-shrinkage `max|z|` stays well below the pre-fix contaminated values (dim9: `1963.12` ->
`12.83`; dim0: `175.93` -> `29.86`; dim7: `437.69` -> `93.59`). `varying_adaptive_dim_matches_
independently_computed_zscore` (previously exercised via `dim0`) moved to `dim6`, since `dim0`
legitimately no longer follows the plain generic-path formula that test characterizes -- a new
`dim0_shrinkage_bootstrap_guard_returns_zero_before_calibrate` test covers `dim0`'s new pre-warmup
behavior explicitly (returns `0.0f` before `Calibrate()` seeds the macro anchor, same guard `dim3`
already relied on).

**Immediate payoff -- re-running the GPD fit on the corrected (shrinkage-blended) z confirms the
contaminated numbers were never real**, not just smaller:

```
                     |z|>=6 rate        GPD shape (xi)         bound derived
dim9 (tail_index):   19.2% -> 1.6%      +0.78 -> -0.3259        360,699(bogus) -> 10.0 (theoretical wall 9.687)
dim0 (log_var_ratio): 3.2% -> 1.2%      (n/a) -> +0.2533         (bogus) -> 262.0 (p=1/N return level)
dim7 (micro_asym):    9.3% -> 1.4%      +0.368 -> +0.0580        7,021(bogus) -> 36.0 (p=1/N return level)
```

`dim9`'s shape parameter doesn't just shrink, it **flips domain entirely** -- from apparent
unbounded-Frechet to genuinely bounded-Weibull, the same tail type as `dim1`. That's the clearest
possible confirmation this was an estimator-reliability artifact, not a market-risk finding: a
real market process doesn't change which extreme-value domain it belongs to depending on whether a
software bug is present. All three bounds implemented directly (no bootstrap ceremony for `dim9`/
`dim7` given how modest the resulting widening is relative to the default `6.0` -- unlike `dim1`'s
originally-surprising `45`, the qualitative finding here, not the third decimal digit, is what's
load-bearing). Re-verified end to end: native suite green, `build_dll.sh` succeeds cleanly.

## Implementation: five real bugs found and fixed during test-driven development, not five iterations of tuning

Built a native (`g++`, no Sierra Chart deps) characterization test against a real, contiguous
225,000-sample slice of the actual tick-derived `dim3` sequence (`tests/cpp/fixtures_dim3_raw.h`,
containing the real tick that produced the original 43,330-sigma event). Confirmed the test
**fails** against the original code (9.04% rail-hit rate) before touching `FeatureScaler.h`, and
re-verified after every change — this was not "implement once and declare done":

1. **First shrinkage-weight design used `dominanceRatio[i]`** (reusing existing D2 telemetry, the
   naturally appealing choice). Result: 9.04% -> 7.99%, barely moved. Instrumented directly:
   `dominanceRatio` stayed in 0.07-0.22 for the entire ~35k-sample approach to the known
   pathological event, never near the 0.30 sigmoid midpoint — because it measures *exact-value
   repetition*, and dim3's failure mode is many *distinct* values compressed into a narrow band.
   Wrong signal for this mechanism. **Fixed**: switched to the local/macro MAD compression ratio
   directly. Result: 9.04% -> 3.67%.
2. **Pushed further rather than accepting 3.67% as a stopping point** (per explicit direction not
   to treat the codebase as settled): diagnosed that `macroScaleEwma` itself was eroding across the
   same quiet regime it was meant to protect against (0.093 -> 0.031 over ~35k samples despite a
   ~13.9k-sample half-life). **Fixed**: ratchet-gated the anchor's update rate by `w` itself. Result:
   9.04% -> 3.11% on the rate metric — a smaller move than hoped, but the *magnitude* of the known
   worst event dropped from z~=-95 to z~=-15.5 (the rate metric is a binary >6-sigma count and
   doesn't reward reducing an already-over-threshold event's severity, so it undersold this fix).
3. **The magnitude-blind rate metric masked something worse**: added a `lastRawZ[]` diagnostic
   field (mirroring the existing `latestLogMedian`/`latestLogScale` precedent — the public
   `result[]` is always already clamped, so an over-amplified internal scale is otherwise invisible
   from outside the class) and found `max|z| = 67,942,872` — nearly 1,600x worse than the original
   bug. Traced to a genuine regression: before `Calibrate()` ever fires, `macroScaleEwma` sits at
   its zero-initialized default, so `max(macroScaleEwma, ABSOLUTE_FLOOR)` clamps to the bare `1e-8`
   floor and any nonzero raw value divided by that explodes. The old floor-gate handled this
   bootstrap case for free (zero `madScale` always routes to a safe carry-forward `0.0f`); the
   replacement mechanism dropped that protection. **Fixed**: explicit `macroScaleEwma[i] <= 0.0f`
   guard returning `0.0f` until the anchor has a real seed.
4. **Re-analyzing D3 rather than accepting it as final** (explicit ask: does the asymmetric
   importance of fat-tail recognition mean the residual should shrink further?) surfaced the
   GARCH-omega gap described in D4 above: a purely reactive anchor cannot protect against the first
   large deviation after a genuinely quiet regime. **Fixed**: empirically-grounded irreducible floor
   (`DIM3_MACRO_SCALE_MIN`). Result: the known worst event's `|z|` dropped `493.92 -> 294.00`.
5. **A direct follow-up question about the rate metric surfaced a bug in the verification tooling
   itself, not the production code**: D3's `max|z|=10.69` full-series figure was computed by a
   script that mis-read the binary fixture (byte-to-float type confusion, see D5 above) — the true
   full-series max is `490.19`, not `10.69`, and `0.16%` of all samples still exceeded the 25-sigma
   bound D3 had shipped with. **Fixed**: dedicated `DIM3_WIDE_WINSOR_SIGMA=500`, no longer shared
   with dim1. Caught because a second, independent measurement (the floor-disabled-vs-enabled rate
   comparison) didn't reconcile with the first, and the discrepancy was chased rather than trusted.

Final, current-code numbers on the real 225k-sample fixture, and the corrected full-series numbers:

```
225k adversarial fixture:
  |z|>=6 rate:           9.04% (baseline) -> 3.11% (post D1-D3, unchanged by D3/D4/D5 by design)
  max |z| among hits:    43,330 (known event, pre-D2) -> 493.93 (post D1-D3) -> 411.64 (post D4)
  p50 |z| among hits:    15.46
  p90 |z| among hits:    225.63

full 1,467,159-sample series (corrected reader, post D1-D5):
  |z|>=6 rate:  1.6220%
  p50=9.50  p75=14.55  p90=24.22  p95=40.70  p99=271.61  p99.9=393.49  max=490.19
  0 samples exceed the shipped 500-sigma bound
```

Test suite: `g++ -std=c++17 -O2 -I include -I tests/cpp tests/cpp/test_feature_scaler.cpp -o /tmp/fs_test && /tmp/fs_test` — all pass, zero regressions against the pre-existing suite. Verified against the real project build (`./build_dll.sh --no-clean`, full clang-cl cross-compile of all 30+ source files) after every design decision (D1+D2, D3, D4, D5, D6, D7) — all clean, zero errors.

**Superseded by D7**: `dim1`'s `WIDE_STATE_WINSOR_SIGMA` is now `45.0` (not `25.0` above), and
`dim3`'s `DIM3_WIDE_WINSOR_SIGMA` is now `4587.0` (not `500.0` above) — the numbers in this section
reflect the state as of D1-D5; D7 explains the further widening and why it's safe. D6 additionally
gave `dim3` its own raw-clamp bound (`[-10,+6]`, `CarryForwardCalculators.h`) separate from the
z-layer bounds this section covers.

## Status / What's NOT done

- **D1 through D8 are all implemented, test-covered, and verified against the real project build**
  (`include/FeatureScaler.h`, `include/CarryForwardCalculators.h`, `src/StudyHelperFunctions.cpp`,
  `tests/cpp/test_feature_scaler.cpp`, `tests/cpp/test_carry_forward_calculators.cpp`,
  `tests/cpp/fixtures_dim1_raw.h`, `tests/cpp/fixtures_dim3_raw.h`, `tests/cpp/fixtures_dim9_raw.h`,
  `tests/cpp/fixtures_dim0_raw.h`, `tests/cpp/fixtures_dim7_raw.h`). Full native suite passes, zero
  regressions; `build_dll.sh` succeeds cleanly after each. Final state: `dim1` raw clamp `[-6,+6]`
  (unchanged, D6), z-layer bound `45` (D7); `dim3` raw clamp `[-10,+6]` (D6), z-layer bound `4587`
  (D7); `dim9`/`dim0`/`dim7` gained shrinkage (D8, generalized from `dim3`'s D2/D4) plus their own
  z-layer bounds (`10`/`262`/`36`, derived on the corrected z per D8) — none shared across dims, at
  either layer, for any of the five.
- **`dim9`/`dim7` are DONE, not open** — corrected from an earlier draft of this doc. Gemini's stated
  prior (25.0/12.0) was checked and superseded: both needed the D8 shrinkage generalization first,
  then their own GPD-derived bounds (`10.0`/`36.0`), not Gemini's literature-only numbers.
- **`dim3`'s remaining tail is now fully characterized and closed out.** All four event clusters in
  the 225k adversarial fixture (idx 952, 14883, 19289, 26052) traced individually (`lastLocalMad`/
  `lastShrinkageWeight` diagnostic fields, mirroring `lastRawZ`). Only cluster 1 showed both scale
  estimates near-degenerate — D4's exact mechanism, already mitigated (`493.92 -> 294.00`). Clusters
  2-4 all pair healthy, non-degenerate local/macro scales with genuinely large raw moves in the
  underlying metric; cluster 4's raw jump is the largest of any cluster and occurs during the
  best-scaled regime of the four. Verdict: real tail events, not artifacts — correctly left
  untouched. The `|z|>=6` rate itself (3.11% fixture / **1.62% full series, corrected — see D5**) is
  unchanged by D3, D4, or D5 by design (none of them touch whether the threshold is crossed, only
  what happens once it is), and that's understood to be appropriate rather than an open question.
- **No rebuild/redeploy/recollection has happened yet.** `dim1`/`dim3` are adaptively-scaled, so
  (per the established `recalibrate_context_static_dims.py` precedent, which only applies to
  *static*-scaled dims) neither fix can be retroactively patched into the already-collected
  `event_data_20260813_191757.context` file — a fresh collection run is required to get corrected
  data for either dim.
- **`dim12` (`liq_fragility`) is resolved, not open** — corrected from an earlier draft of this doc,
  which mis-cited `CLAUDE_BRIEF_093`/`095` as still-open without reading the thread to its actual
  conclusion. `CLAUDE_BRIEF_095`'s full-file scan (2,076,141 samples) found it essentially flat
  outside a known warmup artifact, and `GEMINI_BRIEF_095_RESPONSE` endorsed closing it as healthy.
  Never reopened afterward. See
  `docs/superpowers/specs/2026-08-14-observation-vector-full-institutional-coverage-spec.md`'s
  Purpose section for the full correction.
- **Remaining SOFTLOGZ/LOGZ dims (0 is now done via D8; 2, 4, 6, 8, 10, 11, 12, 15 remain) and the
  LOGZ-path per-dim override mechanism are tracked in the full-coverage spec/plan above, not this
  one** — this spec's scope is `dim1`/`dim3`/`dim9`/`dim0`/`dim7` specifically.

## Recommendation

D1 through D8 are code-complete, test-verified, and build-verified against the real project;
`dim3`'s remaining tail is traced and closed (one real artifact fixed via D4, three genuine tail
events correctly preserved, D5 corrected the winsorization bound to match dim3's actual measured
tail rather than a borrowed, too-narrow value from dim1, D6 fixed an already-occurring raw-clamp
truncation, D7 widened both z-layer bounds to the maximum each dim's own findings can defend after
directly verifying the downstream Student-t HMM's own robustness, and D8 generalized D2/D4's
shrinkage mechanism to `dim9`/`dim0`/`dim7` after the same scale-collapse signature was traced on
all three independently — catching that their first-pass GPD bounds were fitted on contaminated
data before shipping a wrong number). Nothing blocks moving forward. Not yet deployed or validated
against a fresh live collection run — next step is
`./deploy_mindfultrader.sh` and a fresh collection run to empirically confirm all eight fixes in
production the way this investigation confirmed them in a native test harness — see
`docs/superpowers/specs/2026-08-14-observation-vector-full-institutional-coverage-spec.md`'s Unit E
for that task. The remaining SOFTLOGZ/LOGZ dims (2, 4, 6, 8, 10, 11, 12, 15) are that same spec's
Units B/C, using this doc's D8-generalized shrinkage mechanism and the same real-data-first
methodology — do not implement bounds for any of them from a literature prior alone, same
discipline that caught dim9/dim0/dim7's contaminated first-pass numbers before they shipped.
