# Prompt for Gemini CLI — Elite Feature Set Curation: `market_data_replay` significant-change gate still over-triggering on real data after the push-on-change fix

**Role**: You are being consulted as an independent second reviewer (read-only), the same
capacity used elsewhere in this project family for cross-checking statistical/methodology
decisions before they're trusted.

## Hard constraints — read this first

1. **Do NOT edit, create, or delete any source file in this repository.** Your job is analysis
   and recommendations only.
2. **Do NOT run any build, test, or long-running data-processing command** (e.g. do not launch
   `market_data_replay`, do not compile anything, do not run anything against
   `lbrnet/data/raw/mes_ticks.parquet` — it's 476.7M rows / several GB and a full pass takes
   over an hour). Reading/`grep`ing source files and the referenced doc/output files listed below
   is fine and encouraged.
3. **Write your findings as a reply appended to this same file** (`docs/Gemini.md`), under a new
   `## Gemini's Analysis` heading at the end — do not scatter conclusions across other files.
4. If you want to propose a code change, **describe it in prose/pseudocode in your reply** —
   do not write it into the actual source files yourself. A human (or a separate coding session)
   will implement whatever you recommend, after review.

## 1. Background — what this system is

`MindfulTrader` is a C++ trading-signal producer. `tools/market_data_replay/` is an offline tool
that replays raw historical tick data through the same statistical pipeline the live system uses,
to generate training data for a Student-t HMM regime-detection model. A core piece of that
pipeline is a **Mahalanobis-distance-based "significant change" gate**: it only emits a training
record when the current 10-dimensional feature vector has moved meaningfully away from its own
recent rolling history (median/MAD-based robust z-scores per dimension, combined into one
Euclidean distance). The goal is **quality over quantity** — emit on genuine regime shifts, not on
every tick, so the downstream HMM gets diverse, information-rich labels instead of a
near-continuous, highly autocorrelated stream.

The 10 candidate feature dimensions (see `tools/market_data_replay/CandidateObservationDims.h`):
`log_scale_ratio`, `burstiness_index`, `relative_range`, `lempel_ziv`, `hurst_exponent`,
`fisher_info`, `amihud_illiquidity`, `liq_fragility`, `fractal_dim`, `mean_rev_z`. These are a mix
of: pure per-tick-varying microstructure statistics (e.g. `liq_fragility`, changes on ~99% of real
ticks), and much lower-cadence statistics that only update at bar-close or on a rolling-window
recompute (e.g. `relative_range`, `log_scale_ratio` — computed once per TS2/TS1 bar close, which at
real tick density can be hundreds to tens of thousands of ticks apart).

## 2. The problem this session was trying to fix

A real full-density run of this tool (`lbrnet/data/raw/full_fidelity_smoke.parquet`, 34.8M ticks)
showed a **58.5%–69.6% "significant change" emission rate** — far higher than the ~12-14% a small
synthetic/6M-tick smoke test had suggested was sane. That's not a viable rate for the stated
"quality over quantity" goal (it's barely different from emitting on almost every tick).

### Root cause diagnosed this session (empirically, on the real data)

The gate (`tools/market_data_replay/CandidateTriggerGate.h`, originally copy-pasted in spirit from
the live system's `include/ObservationTriggerGate.h`) maintained a **tick-indexed rolling window**
of the last 40 observations per dimension, pushing a new sample into that window on *every tick*
regardless of whether the dimension's value actually changed. For a low-cadence dimension like
`relative_range` (measured directly: frozen/unchanged on 99.7% of tick-to-tick transitions, with
runs of up to 35,119 identical consecutive ticks before the next bar-close update), this means a
40-tick window is almost always **entirely filled with 39-40 copies of one frozen value**. When the
value finally does step to something new, the median/MAD computed from that window collapses to
~0 (or a tiny floor), so the new tick's z-score gets amplified by roughly 1000x — trivially
triggering "significant" on nearly every real update event for that dimension. Measured directly on
`full_fidelity_smoke.parquet`:

| dim | frac. unchanged tick-to-tick | longest frozen run (ticks) |
|---|---|---|
| `relative_range` | 99.7% | up to 35,119 |
| `fast_taleb_kurtosis` (not a candidate dim) | 99.3% | up to 4,086 |
| `tail_index` (not a candidate dim) | ~83-88% | up to 1,883 |
| `log_scale_ratio` | ~82-85% | up to 184 |
| `liq_fragility` | ~0.7% (changes almost every tick — genuinely NOT degenerate) | 43 |

## 3. The fix implemented this session (already committed to the working tree, not yet trusted)

In `tools/market_data_replay/CandidateTriggerGate.h`:

1. **Push-on-change, not push-per-tick.** `PushObservation()` now tracks one `m_lastPushed[dim]`
   value; a new sample is appended to a dimension's rolling window **only if it differs from the
   last pushed value by more than `kFreezeEpsilon=1e-6f`**. Frozen/duplicate ticks are skipped
   entirely for that dimension's window.
2. **Per-dim independent warm-up.** Since dimensions now advance at wildly different real cadences
   once (1) is in effect, `AllDimsWarmedUp()` requires **every** dimension to independently have
   accumulated 40 *distinct* pushed values (not a single shared/lockstep sample count as before).
3. **Compute-before-push ordering.** `ComputeTriggerDecisionMetrics()` is now called *before*
   `PushObservation()` for the same tick (previously the current tick's own value was pushed into
   its own history before being tested against it — self-referential contamination).
4. **`kBaseEpsilon=4.0` threshold, re-derived (not changed):** for 10 independent standard-normal
   z's, `sum(z_i^2) ~ chi-squared(10)`, whose 90th percentile is 15.987 (`sqrt(15.987)=3.998≈4.0`)
   — i.e. the existing constant was confirmed to already correspond to a ~10% base (noise-only)
   trigger rate at this dimensionality, *provided the per-dim z's are genuinely close to
   independent, light-tailed (~Gaussian) draws*.

All native unit tests pass (17/17 in a new `test_candidate_trigger_gate.cpp`, including a
regression test that replays the exact frozen-then-step pattern above and confirms the resulting
z-score is now bounded/sane instead of blowing up), and 75/75 pre-existing engine tests still pass.

## 4. The puzzle — the fix did NOT lower the real-data rate

A real, full 50,000,000-tick run against `lbrnet/data/raw/mes_ticks.parquet` with the fix applied
(log: `tools/log/market_data_replay.log`, tail below) produced:

```
=== SUMMARY: ticks processed=50000000 records written=35496299 significant-change rate=0.709926 ===
```

**71.0%** — essentially unchanged from the 69.6% rate measured on a similarly-sized run *before*
the fix. Despite the fix directly targeting and (per unit tests) correctly neutralizing the exact
frozen-then-jump degeneracy mechanism diagnosed in §2, the real-world aggregate emission rate did
not meaningfully improve.

A second diagnostic tool was written mid-session to investigate further:
`tools/market_data_replay/diagnose_real_data_trigger.cpp` — streams real ticks through the same
engine and aggregates, per candidate dimension, `mean|z|`, `max|z|`, `frac(|z|>=2)`, `frac(|z|>=4)`
over every tick where a real trigger decision was actually made (i.e. `AllDimsWarmedUp()==true`).
**This run has now completed** (bounded to 8,000,000 ticks — smaller than the 50M-tick run above,
so the exact rate differs slightly, 73.2% vs. 71.0%, but the same order of magnitude). Full result:

```
processed 4000000 ticks, decisions=729767, significant=492338 (67.47%)
processed 5000000 ticks, decisions=1729767, significant=1291299 (74.65%)
processed 6000000 ticks, decisions=2729767, significant=1994837 (73.08%)
processed 7000000 ticks, decisions=3729767, significant=2678753 (71.82%)
processed 8000000 ticks, decisions=4729767, significant=3460826 (73.17%)
=== FINAL: ticks=8000000 decisions=4729767 significant=3460826 rate=0.7317 ===
dim                     mean|z|     max|z|   frac|z|>=2   frac|z|>=4
log_scale_ratio          1.2315   520.9872       0.1426       0.0258
burstiness_index         1.0467    31.1353       0.1173       0.0304
relative_range           1.1612   246.2571       0.0657       0.0377
lempel_ziv               0.8287    14.1278       0.0801       0.0206
hurst_exponent           2.9017  3389.4976       0.1297       0.0145
fisher_info              1.1061   515.3809       0.1257       0.0146
amihud_illiquidity      31.5738 11106.6152       0.4257       0.3435
liq_fragility           12.3934 71820.7109       0.2440       0.1008
fractal_dim              2.0744    60.8941       0.2873       0.1495
mean_rev_z               2.4984  2173.3386       0.1514       0.0347
```

**This is the single most important clue in this whole prompt, and should anchor your analysis.**
8 of the 10 dims have `mean|z|` in the roughly 1-3 range (elevated above a clean unit-normal's
expected ~0.8, but not wildly so) and `frac(|z|>=4)` mostly under ~5%. **`amihud_illiquidity` and
`liq_fragility` are dramatically different**: mean|z| of 31.6 and 12.4 respectively (vs. ~1-3 for
everything else), max|z| in the tens of thousands, and `frac(|z|>=4)` of 34.4% and 10.1% — i.e.
`amihud_illiquidity` alone is individually clearing the "energy_fast_track"-style single-dim
significance bar on over a third of all decision ticks. `hurst_exponent` and `mean_rev_z` also have
suspiciously large `max|z|` (3389 and 2173) despite unremarkable means, suggesting occasional
extreme spikes rather than a uniformly elevated distribution. This looks much more like **two (or
three) specific dims still carrying a residual, unfixed degeneracy** than a generic
cross-correlation or fat-tail effect spread evenly across all 10 — though you should evaluate that
independently rather than take this framing as given.

## 5. What I want you to actually do

Read the following files directly for full context (do not modify any of them):

- `tools/market_data_replay/CandidateTriggerGate.h` — the gate itself, full implementation
  including the chi-squared derivation comment.
- `tools/market_data_replay/CandidateObservationDims.h` — the 10 candidate dims.
- `tools/market_data_replay/MarketDataReplayEngine.h` — specifically `ComputeShouldEmit()` (search
  for that function name) — the call site that extracts the 10-dim vector from the full 18D scaled
  observation and invokes the gate.
- `include/ObservationTriggerGate.h` — the **original, live-production** gate this tool-local copy
  was derived from. Note especially `GetAdaptiveMahalanobisEpsilon()` — the live system's epsilon
  is NOT a fixed constant; it's multiplicatively adjusted by event velocity, an entropy/SNR term,
  and **a kurtosis-based "fragility ramp"** (`KURT_FRAGILITY_RAMP_START`/`KURT_FRAGILITY_SLOPE`).
  The tool-local `CandidateTriggerGate` you just read does **not** replicate any of this adaptivity
  — it uses a single fixed `kBaseEpsilon=4.0`. Consider whether this omission is relevant.
- `include/FeatureScaler.h` — the scaler that produces the actual (not raw) values fed into the
  gate (rolling robust Soft-Log-Z / Log-Z transforms, periodic recalibration every N samples).
  The candidate dims are extracted from this scaler's *scaled* output, not raw values.
- `include/LiquidityFragilityEngine.h` and `include/CarryForwardCalculators.h` (the latter has
  `ComputeAmihudIlliquidity`) — the actual per-tick computation for the two dims flagged in §4's
  real-data breakdown as dramatic outliers.
- `tools/market_data_replay/diagnose_real_data_trigger.cpp` — the diagnostic tool described above,
  so you understand exactly what it measures (and can suggest what it should measure instead/also,
  if useful).
- `docs/superpowers/specs/2026-09-09-market-data-replay-dim-selection-spec.md` — the full spec for
  this initiative, §0a/§3a/§3b in particular, which narrates the diagnosis and fix already applied
  in more detail than this prompt repeats.
- `tools/log/market_data_replay.log` — the full log of the 50M-tick run that produced the 71.0%
  result (progress lines with RSS growth, contract-roll markers, the final SUMMARY line quoted
  above).

**Your task**: form and rank concrete, falsifiable hypotheses for why the real-world 10-dimensional
Mahalanobis significant-change rate remains ~70-73% even after (a) removing the tick-indexed
MAD-collapse degeneracy and (b) confirming the fixed threshold is theoretically sound for 10
*independent, light-tailed* z's. **Given §4's per-dim breakdown, prioritize explaining
`amihud_illiquidity` and `liq_fragility` specifically** — both are computed from the live,
still-forming TS3 bar and update on essentially every tick (confirmed: `liq_fragility` changes on
~99.3% of tick-to-tick transitions in real data, i.e. it was never subject to the frozen-value
degeneracy §2/§3 fixed), yet they are now by far the two worst-behaved dims. Whatever is driving
their z-scores this high is very unlikely to be the same tick-vs-update-cadence mismatch already
fixed — look for something specific to these two dims' own computation or scaling. One concrete,
directly relevant prior incident worth checking for a repeat: `docs/superpowers/specs/
2026-08-31-elite-feature-set-curation-initiative.md` records that `include/FeatureScaler.h`'s
`LOGZ_WINSOR_SIGMA_OVERRIDE` calibration array was previously found **silently missing one
element, which misaligned `liq_fragility`'s calibrated bound by one index** (caught by native tests,
not inspection) — see if `FeatureScaler.h`'s current positional calibration arrays
(`DIM_WINSOR_SIGMA_OVERRIDE`, `LOGZ_WINSOR_SIGMA_OVERRIDE`, `SHRINKAGE_SCALE_MIN`,
`SCALE_MODE_MAP`, etc.) are correctly aligned for `amihud_illiquidity`/`liq_fragility`'s actual
field indices in the current 18D layout, or whether a similar misalignment has crept back in.

Other candidate angles worth at least considering (not an exhaustive or prescriptive list — use
your own judgment too, and don't let this list distract from the §4 lead above if your own reading
of the code points there more strongly):

- **Cross-dimensional correlation.** The chi-squared(10) derivation assumes independence across
  the 10 z's. If real regime shifts genuinely move several of these 10 dims together (they are
  not obviously independent — several are all derived from the same underlying price/volume
  process at overlapping windows), the true null distribution of the combined distance could be
  meaningfully heavier-tailed than chi-squared(10), inflating the real trigger rate far above the
  ~10% the derivation predicts.
- **Fat tails vs. the Gaussian assumption.** This entire codebase's own stated philosophy
  (Student-t HMM, Taleb-style kurtosis awareness elsewhere) is that real market microstructure data
  is NOT well-approximated by Gaussian z-scores — it's fat-tailed. The live production gate
  compensates for this with a kurtosis-adaptive epsilon multiplier; the tool-local candidate gate
  does not. Is the fixed `kBaseEpsilon=4.0` simply too low once the real (fat-tailed, not unit
  normal) distribution of these z's is accounted for?
- **Non-stationarity introduced by push-on-change itself.** By requiring 40 *distinct* pushed
  values for a rarely-updating dimension (e.g. `relative_range`, which only changes ~0.3% of
  ticks), the effective wall-clock/real-calendar span covered by that 40-sample rolling window
  could now be very long (plausibly many days or weeks of real trading, if updates are rare
  enough) — much longer than the original tick-indexed window's real time span. Could this
  reintroduce a *different* problem: comparing "now" against a stale, non-representative
  historical baseline that no longer reflects the current regime, especially if the underlying
  series has genuine long-run drift/trend (which real price-derived features do)? This would be a
  different failure mode from the one fixed, potentially with the same symptom (elevated real
  trigger rate).
- **A residual implementation gap.** Double-check the fix as actually implemented (not just as
  designed) for anything that could still let one or more dims re-trigger near-continuously —
  e.g. whether `FeatureScaler`'s own periodic recalibration (search `RECALIBRATION_INTERVAL` in
  `include/FeatureScaler.h`) could be producing small but systematic value shifts that the
  push-on-change `kFreezeEpsilon=1e-6f` tolerance does NOT treat as "no change" but a genuinely
  fixed baseline would, effectively feeding the gate a constant trickle of tiny "genuine" updates
  for otherwise-slow dims — worth checking whether this could produce a different, subtler version
  of the original degeneracy.

For each hypothesis you find credible, state (a) what evidence in the codebase/data supports or
argues against it, (b) a concrete, cheap experiment or additional statistic that would confirm or
rule it out (e.g. "compute the empirical correlation matrix of the 10 per-dim z-scores over a
representative sample and compare its largest eigenvalue to what chi-squared(10) independence would
imply"), and (c) if you have a specific fix in mind, describe it in enough detail that a human
implementer would not need to guess at the details — but again, do not write the code yourself in
this repo's source files.

## 6. What NOT to conclude

Please do not simply conclude "the fix didn't work, revert it" — the fix is independently
verified-correct against every specific unit-testable claim it was designed for (the frozen-value
MAD-collapse mechanism is definitively gone, confirmed both by unit test and by the chi-squared
threshold re-derivation being sound in isolation). The puzzle is specifically *why a
theoretically-sound fix for a confirmed real bug did not move the real-world aggregate metric* —
that gap itself is the interesting finding, and likely points at a second, distinct issue (or a
second-order effect of the fix itself) rather than the fix being wrong per se.

## Gemini's Analysis

*Invoked directly via `gemini --skip-trust --approval-mode plan -p ...` (read-only/plan mode —
structurally incapable of editing any file, not just instructed not to). Response captured
verbatim below, not edited or summarized. One incidental note from its own run: it attempted to
read `tools/log/market_data_replay.log` and was blocked by this repo's configured ignore patterns
(a read attempt, not a write) — it proceeded without that file.*

After a detailed review of the provided source files and diagnostic output, I have identified the
root cause of the ~70-73% emission rate. The index alignment in `FeatureScaler.h` was audited
first and confirmed to be correct (`amihud_illiquidity` is correctly at index 11, `liq_fragility`
at 12); the issue is not a recurrence of the positional array bug.

Instead, the underlying issue is that the push-on-change fix in `CandidateTriggerGate.h` was
applied to the **scaled** output of `FeatureScaler`, not the raw values. This effectively traded
one form of MAD-collapse (tick-indexed duplicate pushes) for a subtler, equally fatal form:
**micro-drift MAD collapse**.

When a raw underlying market value undergoes microscopic continuous drift, `FeatureScaler`
amplifies that tiny drift (by dividing it by a small local `madScale`) into a scaled drift that
easily bypasses `CandidateTriggerGate`'s `kFreezeEpsilon=1e-6f`. Thus, `PushObservation` still
pushes on almost every tick. The gate's 40-sample window fills with a tightly clustered,
deterministic sequence of these micro-drifts, causing the gate's *own* rolling MAD to collapse.
When a macroscopic event finally occurs, the genuine jump is evaluated against this collapsed
micro-drift variance, yielding z-scores in the thousands.

This single hypothesis explains the exact pathological behaviors observed across all four outlier
dimensions outlined in your diagnostic log:

#### A. Volume-Induced Micro-Drift (`amihud_illiquidity`, `liq_fragility`)
- **Mechanism:** Both dimensions incorporate the live, still-forming TS3 bar's volume
  (`liveVolumeSoFar`). Because volume increments on almost every tick, even when the price is
  perfectly frozen, the raw values of these dimensions drift continuously by tiny amounts.
  `FeatureScaler` divides this raw drift by a tiny local MAD (e.g., `~1e-10` for Amihud), yielding
  a scaled change that is consistently `> 1e-6`.
- **Symptom:** `CandidateTriggerGate` pushes on almost every tick. The 40-sample window fills
  exclusively with the volume-induced micro-drifts of a single, frozen price bar. When the price
  finally breaks out (a macroscopic jump), the gate computes a z-score against the volume-drift
  variance, resulting in the massive `max|z|` spikes (11,106 and 71,820) seen in the logs.
- **Verification:** Run a diagnostic that logs the `PushObservation` acceptance rate per-dimension.
  You will see dims 11 and 12 pushing >90% of the time despite price being flat.

#### B. EWMA Shrinkage Micro-Drift (`hurst_exponent`)
- **Mechanism:** `hurst_exponent` updates its raw value only on price changes. However, it opts
  into `FeatureScaler`'s shrinkage mechanism (`SHRINKAGE_SCALE_MIN[5] = 0.000145f`). In
  `FeatureScaler`, the `macroScaleEwma` decays on *every tick* (`MACRO_SCALE_DECAY = 0.99995f`).
  This continuous decay in the effective scale causes the final scaled output to micro-drift by
  `> 1e-6` per tick, even when the raw Hurst input is perfectly frozen.
- **Symptom:** The exact same MAD-collapse dynamics as above, explaining `hurst_exponent`'s
  `max|z|` of 3,389 despite having a perfectly frozen raw value.
- **Verification:** Temporarily set `SHRINKAGE_SCALE_MIN[5] = 0.0f`. The `hurst_exponent` spike
  should vanish from the diagnostic tool's report.

#### C. Shannon Decay Micro-Drift (`mean_rev_z`)
- **Mechanism:** `mean_rev_z` does not use shrinkage. However, if the price is flat for an
  extended period, its local raw MAD drops below `FeatureScaler`'s `ABSOLUTE_FLOOR`, triggering the
  Shannon decay fallback (`exp2f(-carryForwardCount / CARRY_DECAY_HALFLIFE)`). This exponential
  decay continuously lowers the scaled value on every tick, bypassing the `1e-6` epsilon.
- **Symptom:** The gate's window fills with a smooth exponential decay curve (which has near-zero
  variance). When the price finally moves, breaking the degeneracy, the scaled value jumps back to
  a true reading, resulting in the `max|z|` of 2,173.
- **Verification:** Log `calibration[16].carryForwardCount`. You will find that `mean_rev_z`'s
  extreme z-scores occur exactly at the termination of a long Shannon decay sequence.

### 2. Double-Normalization Redundancy (Secondary Observation)

By running a rolling median/MAD normalization inside `CandidateTriggerGate` on values that are
*already* median/MAD normalized (and winsorized) by `FeatureScaler`, the architecture is
essentially normalizing the data twice.

`FeatureScaler` outputs values that are already institutional-grade z-scores (or Soft-Log-Z
scores). The theoretical derivation for `kBaseEpsilon=4.0` (chi-squared of 10) assumes you are
summing the squares of independent standard normal variables. Summing the squares of
`FeatureScaler`'s outputs directly would honor this math. Re-normalizing them via a short
40-sample window in the gate destroys `FeatureScaler`'s careful long-horizon and shrinkage
calibrations, forcing the gate to overreact to short-term micro-variance.

### 3. Lack of Fat-Tail Adaptivity vs Production (Tertiary Observation)

The tool-local `CandidateTriggerGate` removed `GetAdaptiveMahalanobisEpsilon()` in favor of a fixed
`4.0` threshold. Real market data is unequivocally fat-tailed. The live production gate
compensates for this by dynamically widening the epsilon multiplier based on kurtosis, entropy,
and velocity. By dropping this adaptivity, the offline tool is holding fat-tailed realities to a
strict Gaussian threshold, which inherently over-triggers compared to live production.

### Concrete Recommendations

1. **Top Priority: Stop Double-Normalizing.**
   Remove the rolling median/MAD computation entirely from `CandidateTriggerGate`. The gate should
   compute the Mahalanobis distance directly from the `FeatureScaler` outputs (i.e.,
   `distance_sq += currentObs[dim] * currentObs[dim]`), because `FeatureScaler`'s precise purpose
   is to provide ready-to-use, robustly-scaled z-scores. This instantly deletes the micro-drift
   MAD-collapse bug, honors the chi-squared math, and respects the institutional scaling logic
   (shrinkage, bounded limits, etc.) you already engineered.

2. **Alternative (If Double-Normalization is Kept): Push on Raw Change, Not Scaled Change.**
   If the gate must maintain its own independent rolling MAD, the `PushObservation` trigger must
   evaluate the `kFreezeEpsilon` against the **raw** observation vector, not the scaled one.
   Additionally, for live-bar volume dimensions (`amihud_illiquidity`, `liq_fragility`), the push
   should be gated explicitly on a macroscopic event (like a price change or a bar close) rather
   than allowing volume micro-increments to fill the window.

3. **Restore Adaptive Epsilon.**
   Port `GetAdaptiveMahalanobisEpsilon()` from `include/ObservationTriggerGate.h` to the candidate
   gate and feed it the raw `lempel_ziv` and `tail_index` values, ensuring the offline generator
   shares the exact same "quality over quantity" bar as production.

## Verification & Resolution (post-Gemini, same session)

Recommendation #1 (stop double-normalizing) was independently verified against the codebase before
being trusted, then empirically confirmed, then implemented:

- **Codebase verification**: `include/FeatureScaler.h`'s own comments confirm `amihud_illiquidity`'s
  real rolling MAD is ~1e-11 to 1e-10 (matches Gemini's independent estimate almost exactly) and
  that its floor (`AMIHUD_ABSOLUTE_FLOOR=1e-16`) was deliberately lowered specifically so this
  dim's z-score never collapses to exactly zero — i.e. it structurally can never be bit-identical
  tick-to-tick, so `CandidateTriggerGate`'s push-on-change mechanism could never skip it, exactly as
  the analysis predicted. `FeatureScaler.h`'s own index audit note (line ~188) also independently
  confirms the index-alignment hypothesis Gemini ruled out itself was indeed clean.
- **Empirical confirmation, before touching the real gate**: extended
  `tools/market_data_replay/diagnose_real_data_trigger.cpp` to compute a **direct**
  `sqrt(sum(currentObs[dim]^2))` metric (no rolling window at all) alongside the existing
  double-normalized one, on the same 8,000,000 real ticks used for the original per-dim breakdown
  above. Result: current (double-normalized) gate = **73.17%**, direct metric = **7.14%** —
  matching the chi-squared(10) 90th-percentile prediction (~10%) closely, and squarely vindicating
  recommendation #1 over #2/#3.
- **Implemented**: `CandidateTriggerGate` rewritten to be stateless beyond
  `HasBaseline()`/`SetBaseline()` — no rolling window, no median/MAD, no per-dim warm-up concept of
  its own. `kBaseEpsilon=4.0` left unchanged (now genuinely applicable). All native tests updated
  and passing (11/11 gate-level, 75/75 engine-level). A full 476.7M-tick real-dataset validation
  run was launched to confirm the rate at full scale; result to be appended here once complete.

**Full-dataset validation, completed 2026-09-16**: the entire 476,745,947-tick `mes_ticks.parquet`
run with the real fix applied gave a final **5.07% significant-change rate** (24,170,802 records
written, `tools/log/market_data_replay.log` / `tools/output/market_data_replay_20260916_005410.txt`)
— sane, in the same order of magnitude as both the 8M-tick sample (7.14%) and the chi-squared(10)
theoretical prediction (~10%), down from 71-73% before this fix. This closes the investigation your
analysis kicked off.

Recommendations #2 and #3 were not pursued — #1 fully resolved the problem on its own, confirmed
empirically, and is architecturally simpler (removing code, not adding an adaptive-epsilon
mechanism on top of a design that turned out to be unnecessary in the first place). Thank you for
the analysis — the specific, falsifiable mechanism (continuously-reactive dims defeating
push-on-change) was exactly right and saved considerable time versus continuing to guess.


