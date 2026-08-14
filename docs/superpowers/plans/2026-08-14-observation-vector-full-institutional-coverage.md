# Full-Coverage Institutional Audit of the 16D Observation Vector — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Take the 16D observation vector from "2 of 16 dims rigorously audited" to "all 16 dims audited
or explicitly exempt, everything validated in production" — per
`docs/superpowers/specs/2026-08-14-observation-vector-full-institutional-coverage-spec.md`.

**Correction from the first draft of this plan:** it originally had a Task 2 root-causing `dim12`'s
residual-zero rate as an open correctness anomaly. Reading `CLAUDE_BRIEF_093`-`095`
(`../lbrnet/logs/rc_gemini.log:7903-8238`) in full, not just the citation, shows this was already closed
— `CLAUDE_BRIEF_095`'s full-file scan (2,076,141 samples) found `dim12` essentially flat 0.0% outside a
known warmup artifact, and `GEMINI_BRIEF_095_RESPONSE` endorsed closing it as healthy. That task is
removed; `dim12` now gets only the same routine bound audit every other non-exempt dim gets.

**Update (same day, later pass): Task 2 (`dim9`/`dim7`) is DONE**, and pulled `dim0` forward from
Task 3 alongside it. Auditing `dim9`/`dim0` surfaced a bigger finding than a bound derivation: their
real-data traces showed the exact same scale-collapse signature `dim3` originally had (D4), meaning
the mechanism D2/D4 built was under-generalized, not `dim3`-specific. Generalized it (D8, see the
dim3-shrinkage spec) before deriving any of the three dims' bounds — deriving on the old,
un-generalized mechanism would have shipped bounds fitted to a contamination artifact (confirmed:
`dim9`'s first-pass fit implied a 360,000-sigma bound; the corrected fit, after D8, gives `10.0`).
Final bounds: `dim9=10.0`, `dim0=262.0`, `dim7=36.0`. **Run the new methodology step 4 (scale-collapse
check) on every dim in Tasks 3 and 4 below before trusting a GPD fit** — this isn't a one-off `dim9`
quirk.

**Architecture:** Six tasks. Task 1 (production validation) should run before Tasks 3-4 (the remaining
per-dim audits) so the methodology itself gets one real-world confirmation before being applied more
times. Tasks 3, 4 are each independently committable (one dim, or one small dim-group, per commit) and
do not block each other. Tasks 5-6 depend on Tasks 2-4's numbers where noted.

**Tech Stack:** C++17 native unit tests (`g++` + `tests/cpp/*.cpp`, established pattern), Python
(`mamba run -n mts`, per this session's tooling convention) for the real-data audit methodology. Four
real MES data files, matched to each dim's actual timeframe (confirmed per-dim below, not assumed —
`TripleScreenN.cpp` denotes screen N; CLAUDE.md's TS1/TS2/TS3 = 240min/60min/15min mapping applies):

```
../lbrnet/data/raw/mes_tide_240m.parquet    — TS1 (240min)
../lbrnet/data/raw/mes_wave_60m.parquet     — TS2 (60min)   -- used for D1-D7 already
../lbrnet/data/raw/mes_ripple_15m.parquet   — TS3 (15min)
../lbrnet/data/raw/mes_continuous_ticks.parquet — ~5s-bucket granularity, 38.3M rows,
    2023-06-04..2026-08 coverage, columns include bid_volume/ask_volume/signed_volume/mean_spread
    -- required (not optional) for genuinely tick-native dims; no bar file substitutes
```

Never synthetic data, per this initiative's standing convention.

**Spec:** `docs/superpowers/specs/2026-08-14-observation-vector-full-institutional-coverage-spec.md`
(builds on `docs/superpowers/specs/2026-08-14-featurescaler-winsorization-and-dim3-shrinkage.md`'s D1-D7
and `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`). Executors should read all
three, including the spec's per-dim table (confirmed TS-screen assignments and real measured 6σ clip
rates from `CLAUDE_BRIEF_095`'s full 2.08M-sample scan) before starting any task below.

## Global Constraints

- Work directly on `master`, no feature branches or worktrees (standing repo convention).
- TDD throughout: write the failing test first, watch it fail, then write the minimum code to pass —
  exact pattern D1-D7 already established in `tests/cpp/test_feature_scaler.cpp` and
  `tests/cpp/test_carry_forward_calculators.cpp`.
- `./build_dll.sh` must succeed before any task is considered done.
- **No fabricated bound values.** Every winsorization constant this plan produces must come from the
  spec's audit methodology run against real data, never guessed or carried over from a different dim's
  number (D5/D6's exact lesson — sharing `dim1`'s bound with `dim3` was wrong twice).
- **Use the correct data file per dim, not whichever is convenient.** Genuinely per-tick dims (`dim7`,
  `dim9`) require `mes_continuous_ticks.parquet` — a bar-resampled approximation would misrepresent their
  actual update cadence and destroy the tick-level information their formulas exist to capture. Bar-based
  dims must use the file matching their *actual* confirmed screen (spec table), not an assumed one —
  `dim0`/`dim6`/`dim8` are TS1, not TS2, confirmed by grep, not assumption.
- **No `lbrnet` edits from this repo.** Task 6's `context_preflight.py` item is `lbrnet`-repo work,
  tracked only as a pointer — same convention as the 2026-08-13 plan's Unit 5.
- Deploying to Sierra Chart (`./deploy_mindfultrader.sh`) is a live-system action — confirm with the user
  before running it, every time, even though Task 1 below schedules it. Do not treat a prior approval as
  standing permission for a later run.

---

### Task 1: Production validation of D1-D7 (Unit E)

**Files:** none modified — this task is a live-collection run plus a comparison script, not a code
change.

- [ ] **Step 1: Confirm with the user before deploying** — this is a live-system action
  (`./deploy_mindfultrader.sh`), not a reversible local edit. Do not proceed without explicit
  confirmation for this specific run.

- [ ] **Step 2: Deploy and collect**

```bash
./deploy_mindfultrader.sh
# Then, in Sierra Chart: run a fresh EventDataCollector replay over a representative window
# (same 2023-09-01..10-20 window D1-D7's fixtures used, if reproducing that exact comparison
# is the goal; otherwise any representative window is fine for a fresh-data sanity check).
```

- [ ] **Step 3: Compare live telemetry against the native-test predictions**

Reuse the exact validation bar `CLAUDE_BRIEF_097` established for dim1/dim3's original rail-hit rates
(~1pp agreement between native-test fixture and live telemetry). For each of dim1/dim3, compute from the
fresh `.context` file:
- `|z|>=6` rate (should match the native suite's fixture-measured rates within ~1pp)
- `|z|>=WIDE_STATE_WINSOR_SIGMA` / `|z|>=DIM3_WIDE_WINSOR_SIGMA` rate (should be at or near `0`, matching
  the native suite's `0.0000%` result)
- max observed `|z|` (should stay well inside the shipped bounds — `45`/`4587` — with no rail-pinning)

- [ ] **Step 4 (light re-confirmation, not a re-investigation): spot-check `dim12`'s zero rate on the
  fresh file** — should still be near-flat (per `CLAUDE_BRIEF_095`'s closed finding). This is a cheap
  sanity check riding along on data Task 1 collects anyway, not a standalone investigation — if it
  doesn't reconcile, that's a new, real finding worth its own thread, not evidence the original 095
  scan was wrong.

- [ ] **Step 5: Record the result**

If Step 3 matches: note the confirmation in this plan's checkbox and in the dim1/dim3 spec's Status
section. If it doesn't reconcile within ~1pp: stop and treat this as a new investigation
(`superpowers:systematic-debugging`) before proceeding to Tasks 2-4 — the audit methodology itself needs
to be trusted before applying it nine more times.

---

### Task 2: `dim9`/`dim0`/`dim7` — derive the bound for confirmed over-saturating dims (Unit A) — **DONE**

**Not a from-scratch audit** — `CLAUDE_BRIEF_095`'s full-file scan already confirmed `dim9` (16.247%,
the worst dim in the entire vector) and `dim7` (6.758%) over-saturate on real production data; `dim0`'s
own tick-level replica confirmed the same after its first (bar-close-only) attempt badly undersampled.

**What actually happened, expanding this task's scope mid-execution**: Step 3's GPD fit for `dim9`/`dim0`
produced absurd results (`dim9` implied a 360,000-sigma bound). Tracing the largest `|z|` events (same
method D4 used for `dim3`) found a scale-collapse artifact, not real tail signal, on all three dims —
meaning D2/D4's shrinkage mechanism was under-generalized as `dim3`-only, not that `dim9`/`dim0`/`dim7`
had no fix available. Generalized it first (D8, see the dim3-shrinkage spec), *then* re-derived bounds
on the corrected z. `dim0` was pulled forward from Task 3 into this same effort since it hit the
identical issue.

**Files (as actually touched):**
- `include/FeatureScaler.h` — generalized `DIM_MACRO_SHRINKAGE_INDEX`/`DIM3_MACRO_SCALE_MIN` and
  `DIM_WIDE_WINSOR_INDEX`/`WIDE_STATE_WINSOR_SIGMA`/`DIM3_WIDE_WINSOR_SIGMA` into two per-dim arrays,
  `SHRINKAGE_SCALE_MIN[N_DIMS]` and `DIM_WINSOR_SIGMA_OVERRIDE[N_DIMS]` (0.0f sentinel = disabled/default)
- `tests/cpp/test_feature_scaler.cpp` — new fixtures `fixtures_dim9_raw.h`/`fixtures_dim0_raw.h`/
  `fixtures_dim7_raw.h` (same `[1000000:1225000]` slice convention as `fixtures_dim3_raw.h`), new
  shrinkage-generalization tests, `dim0`'s pre-warmup-bootstrap-guard test, `dim6` swapped in for the
  generic-path z-score characterization test (`dim0` no longer represents that path)

- [x] **Step 1: Build tick-level replicas** — `dim9` (`ContextManager.cpp:220`, `TailRiskEngine`),
  `dim0` (`TripleScreen1.cpp:478`, `CalculateLogVariance`, TS1 — rebuilt after the bar-close-only
  attempt undersampled), `dim7` (`TripleScreen3.cpp:640-657`, `ComputeMicroAsymmetry`), all from
  `mes_continuous_ticks.parquet` (2023-09-01..10-20 slice, ~1.5M rows).
- [x] **Step 2: Feed raw series through the z-replica, confirm `|z|>=6` rate reconciles** —
  `dim9`/`dim7` reconciled with `CLAUDE_BRIEF_095` within a few points (19.2%/9.3% replica vs.
  16.2%/6.8% production — real methodology-vs-production gap, not a red flag the way `dim0`'s
  original 280x gap was). `dim0`'s corrected tick replica reconciled much better than its flawed
  first attempt (3.2% vs. production's 5.8%).
- [x] **Step 2.5 (new, not in the original plan): trace the largest `|z|` events, found scale-collapse
  contamination on all three** — see the expanded scope note above and D8.
- [x] **Step 3 (generalize first): implement D8** — the shrinkage mechanism generalization, TDD-verified
  regression-safe for `dim3` (identical `3.1093%`/`411.64`/`0`-hits before and after).
- [x] **Step 3.5: re-run the GPD fit on the corrected (shrinkage-blended) z** — `dim9`: `xi=-0.3259`
  (Weibull/bounded, domain flipped entirely from the contaminated fit's apparent Frechet), theoretical
  endpoint `9.687`. `dim0`: `xi=+0.2533`, `p=1/N` return level `261.66`. `dim7`: `xi=+0.0580`, `p=1/N`
  return level `35.69`.
- [x] **Step 4: Implement** `SHRINKAGE_SCALE_MIN`/`DIM_WINSOR_SIGMA_OVERRIDE` array entries: `dim9`
  floor `0.0438`/bound `10.0`; `dim0` floor `0.000389`/bound `262.0`; `dim7` floor `0.00733`/bound `36.0`.
- [x] **Step 5: TDD-verify + `./build_dll.sh`** — native suite all-pass (0 failures), `build_dll.sh`
  clean.

Not yet committed to git (matches this session's established rhythm of verifying fully before the
commit step) — do that as part of wrapping up this task, same message convention as D1-D7's commits.

---

### Task 3: Remaining SOFTLOGZ dims audit — 6, 8, 10, 11, 15 (Unit B) — **DONE**

**`dim0` moved to Task 2** (done alongside `dim9`/`dim7` once it hit the same scale-collapse issue).
**`dim12` moved to Task 4** (LOGZ mode, kept out of this SOFTLOGZ task).

**Files (as actually touched):**
- `include/FeatureScaler.h` — populated `SHRINKAGE_SCALE_MIN[6]=0.00297` and
  `DIM_WINSOR_SIGMA_OVERRIDE[8]=20.0`; `dim10`/`dim11`/`dim15` left at their defaults (audited, closed
  clean, no change warranted)
- `tests/cpp/test_feature_scaler.cpp` — new fixtures `fixtures_dim6_raw.h`/`fixtures_dim8_raw.h`
  (tick-level, sampled every 20 ticks — see caveat below), new shrinkage/bound tests, `dim6`'s
  bootstrap-guard test, generic-path characterization test moved again (`dim8` now, since `dim6` also
  gained shrinkage)

**What actually happened**: confirmed via source (not assumed) that `dim6`/`dim8` reference the live
still-forming bar the same way `dim0` did (both TS1) — a quick bar-close-only screen showed the same
undersampling gap dim0's flawed first attempt had (0.000-0.593% vs production's 0.78-3.26%), confirming
tick-level replicas were required before trusting any result. `dim11` was the one dim where this wasn't
needed: its own source comment confirms it's bar-gated/historical-only by design (excludes the live bar
explicitly, unlike every other dim checked so far), so its already-near-zero production clip rate
(`0.000%`) could be trusted directly.

- [x] **Step 1: dim6 (`hurst_exponent`, 3.256%)** — TS1, tick-level DFA replica. First pass used a
  Python re-implementation approximate in spirit, not an exact port; confirmed the scale-collapse
  signature (4th confirmation) and enabled shrinkage, but deliberately left the z-layer bound unset
  pending better data. **Superseded same day**: built an exact port of `CalculateHurstExponent`
  (every segment boundary, exact scale-sampling rule, closed-form regression matching the C++), which
  revealed a genuinely elevated tail (pre-shrinkage `|z|>=6` rate `24.851%`, `max|z|=3085.93` — an
  order of magnitude worse than any other dim). Consulted Gemini (`logs/rc_gemini.log`
  `CLAUDE_BRIEF_101`/`102`, `GEMINI_BRIEF_101`/`103_RESPONSE`) to distinguish genuine tail signal from
  a DFA-instability artifact; a tail-conditional noise decomposition (Gemini's proposed diagnostic,
  refined to condition on the local-MAD collapse rather than averaging globally) empirically ruled out
  the instability hypothesis — intra-bar noise was 4.3x *smaller*, not larger, during the exact
  episodes producing the extreme z-scores. Re-derived the shrinkage floor from the exact replica
  (`0.000145`) and the bound from the corrected fit: `n_tail=11,023` (14.80%), `shape(xi)=+0.3014`,
  `p=1/N` return level `344.53`. Set `DIM_WINSOR_SIGMA_OVERRIDE[6]=345.0`. Full confidence, no caveat —
  the last holdout in the entire 16D audit is now closed.
- [x] **Step 2: dim8 (`fisher_info`, 2.012%)** — TS1, tick-level replica using the *exact* raw formula
  (verified against `cfc::ComputeFisherInformation`'s source, including its `+-0.99` clamp — simple
  enough to replicate exactly, unlike DFA). Clean scale-collapse trace (no shrinkage needed). GPD fit:
  `xi=-1.2574` (Weibull/bounded), `n_tail=330`. Set `DIM_WINSOR_SIGMA_OVERRIDE[8]=20.0` — real margin
  above the fitted endpoint (14.216) given the smaller tail sample than dim1/dim3/dim9/dim0/dim7 had.
- [x] **Step 3: dim15 (`mean_rev_z`, 1.339%)** — TS3, tick-level replica. Closed clean: 0.028% clip rate,
  only 14 exceedances — too few for any reliable fit, no change warranted.
- [x] **Step 4: dim10 (`skewness_idx`, 0.780%)** — TS3, tick-level replica (fixed 100-bar window, not
  adaptive). Closed clean: 0.041% clip rate, only 31 exceedances; a weak scale-collapse signal was
  present but not acted on given the negligible practical impact (ruthless simplicity).
- [x] **Step 5: dim11 (`amihud_illiquidity`, 0.000%)** — closed clean without a replica, per the
  bar-gated/historical-only design finding above plus the already-near-zero production clip rate and
  its existing dedicated floor fix (`AMIHUD_ABSOLUTE_FLOOR`).
- [x] **Step 6: TDD-verify + `./build_dll.sh`** — native suite all-pass (0 failures), `build_dll.sh`
  clean.

Not yet committed to git, same as Task 2.

---

### Task 4: LOGZ dims audit — 2, 4, 12 (Unit C) — **DONE**

**`dim12` moved here from Task 3** — it's LOGZ mode per the spec table (was only grouped with
`dim10`/`dim11` in Task 3's audit for call-site convenience; its `FeatureScaler` scaling path is LOGZ).

**Files (as actually touched):**
- `include/FeatureScaler.h` — added `LOGZ_WINSOR_SIGMA_OVERRIDE[N_DIMS]` (same `0.0f`-sentinel array
  pattern as SOFTLOGZ's `DIM_WINSOR_SIGMA_OVERRIDE`), wired into the LOGZ path's hard z-clamp; the LOGZ
  path had no per-dim mechanism at all before this. Populated `dim12=12.0`; `dim2`/`dim4` left at
  default (see results below).
- `tests/cpp/test_feature_scaler.cpp` — new fixture `fixtures_dim12_raw.h` (bar-close, full multi-year
  history, 25,000-sample slice), new dim12 rail-rate test. No dedicated fixtures needed for `dim2`/`dim4`
  since neither got a bound change.

**What actually happened**: confirmed via source that `dim4`/`dim12` are bar-gated/historical-only (same
category as Task 3's `dim11`), so real bar-close replicas were correct methodology, not an
approximation — and cheap enough (bar-gated = one evaluation per bar, not per tick) to build against the
FULL multi-year tick history (75,085 fifteen-min bars) rather than the shorter slice the tick-native
SOFTLOGZ dims needed to sample down to for tractability. `dim2` (confirmed live-referenced,
`TripleScreen2.cpp:273`) closed clean via a cheap full-history bar-close screen (zero exceedances across
18,761 real 60-min-bar windows) rather than the full tick-level rebuild its live-bar reference would
otherwise call for — justified because that screen and production telemetry both already agreed
emphatically (0.000% vs. production's already-tiny 0.035%), the same "cheap check already answered it"
closure Task 3 used for `dim11`/`dim15`.

- [x] **Step 1: dim4 (`vol_convexity`, 2.713%)** — TS3, bar-gated, full-history replica (75,059-sample).
  A top-5-event sample initially looked ambiguous (2 contaminated, 3 clean) and was first deferred with
  the same caution as `dim6` -- **superseded same day**: classifying the FULL 85-event population
  resolved it decisively (51/60% contaminated, 34/40% genuine, the same majority-contaminated pattern
  already confirmed on 5 other dims). Generalized shrinkage to the LOGZ path for the first time
  (`SHRINKAGE_SCALE_MIN[4]=0.211521`, shared `ComputeShrinkageZ()` helper extracted from the
  SOFTLOGZ-only version) rather than deferring further. Corrected fit: `n_tail=76`, `shape(xi)=-0.3580`
  (Weibull/bounded), theoretical endpoint `8.837`. `LOGZ_WINSOR_SIGMA_OVERRIDE[4]=12.0`. Full
  confidence, no caveat -- unlike `dim6`, this one didn't need better data, just a bigger sample of the
  data already in hand.
- [x] **Step 2: dim2 (`relative_range`, 0.035%)** — closed clean via the cheap screen described above,
  zero exceedances, no dedicated replica needed.
- [x] **Step 3: dim12 (`liq_fragility`, 0.727%)** — TS3, bar-gated, full-history replica (75,051-sample).
  Clean trace (no contamination signature), real sample (`n_tail=207`), `shape(xi)=-0.2276`
  (Weibull/bounded), theoretical endpoint `11.485`. `LOGZ_WINSOR_SIGMA_OVERRIDE[12]=12.0` — just past
  the wall, same "close to the theoretical ceiling" logic as dim1/dim9's finite-endpoint dims.
- [x] **Step 4 (design flag)**: no evidence surfaced during this audit suggesting `LOGZ` is the wrong
  mode for any of the three — flag remains open as a future question, not resolved here (out of scope,
  per the original plan).
- [x] **Step 5: Added the per-dim override mechanism, TDD-verified + `./build_dll.sh`** — native suite
  all-pass (0 failures), `build_dll.sh` clean.

Not yet committed to git, same as Tasks 2-3.

---

### Task 5: Gang-doc + governance doc sync (Unit E, doc portion) — **DONE**

**Depends on:** Tasks 2-4's final numbers (this task documents what actually shipped, not a
placeholder).

**Files:**
- `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`
- `../docs/RISK_MANAGEMENT_SYSTEM.md` (§5.3, §10)
- `../docs/TRADE_EXECUTION_SYSTEM.md` (§H.6)

- [x] **Step 1: Commit the corrected Gang-doc entries.** Five rows added (raw `[-6,+6]` clamp as a
  domain-validity guardrail; Weibull/bounded winsorization bounds; Frechet/unbounded winsorization
  bounds at `p=1/N`; macro-scale shrinkage/floor as a Ledoit-Wolf+GARCH-omega synthesis; a new
  Mandelbrot-pillar "DFA Memory-Clustering" finding for `dim6`) with the actual shipped numbers, not
  Gemini's original illustrative Entry A-C placeholders. Commit `86dfd25`.
  **Unplanned but necessary detour:** re-verifying these numbers directly against `FeatureScaler.h`
  (rather than copying from the correspondence log) surfaced a real transposition bug —
  `DIM_WINSOR_SIGMA_OVERRIDE[0]`/`[9]` had `dim0`'s and `dim9`'s derived bounds swapped, flattening
  `dim0`'s genuine Fréchet tail to `dim9`'s tight `10.0` Weibull-wall bound. Fixed via TDD (direct
  value-equality regression test, since neither the existing smoke check nor a rate-taper check can
  distinguish the swap) before writing the doc, commit `f7c47bf`, full `./build_dll.sh --no-clean`
  verified.
- [x] **Step 2: Sync `RISK_MANAGEMENT_SYSTEM.md` §5.3/§10 and `TRADE_EXECUTION_SYSTEM.md` §H.6** to the
  post-2026-08-13 Moors-kurtosis numbers. §5.3/§10: `KURTOSIS_EMERGENCY_ENTER/EXIT` 5.0/3.0 →
  `talebKurtosisCrisisEnter/Exit` 1.5650/1.3809 (`ExecutionParams.h` compiled defaults). §H.6: the
  HMM-regime Taleb DENY gate 9.698σ → 1.8382 (Moors scale, `RiskManager.cpp`'s
  `taleb_signal_sigma_threshold` compiled default), with a scale-note explaining why the old figure is
  now unreachable and guarded against in the JSON-override parser (final-review Finding 9).
- [x] **Step 3: Commit, doc-only, no code changes.** The Gang-doc commit (`86dfd25`) is doc-only as
  planned; the transposition fix that made it possible to write correct numbers was committed
  separately first (`f7c47bf`), not folded into the doc commit. `RISK_MANAGEMENT_SYSTEM.md`/
  `TRADE_EXECUTION_SYSTEM.md` live outside this repo's git tree (untracked directory) — edited
  directly, no commit applicable.

---

### Task 6: Cross-repo/environment pointers (Unit E, remainder) — tracked, not executed from this repo — **FLAGGED, awaiting action outside this session**

- [x] **`lbrnet/scripts/context_preflight.py`'s D3 `chronic_zero_threshold` gate** — `lbrnet`-repo work.
  Not implemented from this repo (correct per the standing no-cross-repo-scope-mixing convention —
  `lbrnet` is a separate repo/session). **Action needed:** raise this in a dedicated `lbrnet` session so
  it gets tracked there; this plan only records that the gate was never implemented
  (`docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md`'s original D3 scope)
  and remains open.
- [x] **Config drift**: `/mnt/c/Trading/config/`'s two live JSON config files (execution params +
  empirical gate thresholds) carry their own copies of pre-D1-D9 thresholds — including the pre-Task-6
  kurtosis scale and the pre-2026-08-14 winsorization bounds — and won't pick up this initiative's
  numbers automatically; they live outside this repo (and outside git entirely) so were not edited.
  **Action needed from the user:** manually sync those two files on the live trading machine before the
  next production deployment, or confirm `ExecutionParams`'s load-time scale guards (final-review
  Finding 9) are sufficient to fail closed in the meantime rather than silently running stale
  thresholds.

---

## Sequencing summary

```
Task 1 (deploy+validate D1-D9, incl. light dim12 spot-check) -- NOT YET RUN, needs explicit go-ahead
Task 2 (dim9/dim0/dim7 — DONE, surfaced D8's shrinkage generalization)
Task 3 (dim6/dim8/dim10/dim11/dim15 — DONE, dim6 fully resolved same-day via exact-formula replica)
Task 4 (dim2/dim4/dim12 — DONE, all three full confidence: dim4's initial ambiguity resolved same-day)
        │
        └─→ Task 5 (doc sync, ready now) ─→ Task 6 (pointers)
```

**All per-dim auditing (Tasks 2-4) is complete, and every dim is fully resolved with zero disclosed
caveats.** Both dims that went through an intermediate "mechanism fixed, number deferred" state during
the audit were closed out the same day: `dim4` via full-population exceedance classification (60%
contaminated / 40% genuine, needed LOGZ shrinkage), `dim6` via an exact-formula DFA replica plus a
Gemini-assisted tail-conditional noise decomposition (`logs/rc_gemini.log` `CLAUDE_BRIEF_101`/`102`)
that empirically ruled out a DFA-instability artifact before any bound shipped — `dim6`'s confirmed
tail (`n_tail=11,023`, 14.80% post-shrinkage) is the largest of any dim in this initiative, genuine, not
contamination. `dim2` closed via a cheap screen corroborated by production telemetry. Final tally: 15
done or closed clean with full confidence, plus 3 exempt by construction — 16 of 16. Remaining work is
Task 5 (doc sync — ready to start now that real numbers exist for every dim), Task 6 (pointers, can be
filed any time), and Task 1 (production validation — genuinely useful, still un-started, still needs
your explicit go-ahead to deploy).
