# Session Scratchpad — Where We Left Off

**PICK UP HERE, 2026-09-03 — observation-vector dim fixes continue (`hurst_exponent` resolved,
`amihud_illiquidity` reformulated). NEXT MAJOR INITIATIVE queued right below — do not let it slide
once the observation-vector/ContextManager thread closes out.**

**⚠️ NEXT MAJOR INITIATIVE, queued by the operator 2026-09-03 — pick up the MOMENT the observation
vector + ContextManager work is done:** `LocalRiskContext`/`RiskGateContext` (the execution-layer
risk context `RiskManager`/`ExecutionGate`/`PositionManager` actually gate on) must stop being blind
to the HMM's own signal. `PredatorContext` already carries `.regime` (via `GetPredatorContext()`),
but that's a higher-level fusion struct assembled *after* `RiskManager`'s own hard gates already fire
directly on `LocalRiskContext` — verify whether those lower-level gates ever see HMM output at all,
or only the raw pre-HMM features. The HMM exists specifically to detect fat tails/regime shifts —
its own output should feed back as a "heads up" signal for the Predator (and the hard-gate layer
beneath it), not stay siloed inside the observation-vector→model→regime pipeline. Full framing in
`CLAUDE.md`/`GEMINI.md`'s own North Star section (same note, don't duplicate maintenance here).
Related, not yet designed: the EVT/GPD-based "how close to the tail, and closing how fast"
execution-layer signal discussed 2026-09-03 (`lbrnet/logs/rc_gemini.log` around `CLAUDE_BRIEF_123`)
— a candidate first deliverable, scoped to feed `RiskGateContext` directly, not the HMM's own vector.

- `hurst_exponent` (row 7): DFA q=2 vs MFDFA q=1 ambiguity resolved via real Monte Carlo evidence
  (`tools/observation_vector/dfa_vs_mfdfa_q1_montecarlo.py`) — keep q=2, q=1 doesn't survive real
  contamination testing. Committed.
- `amihud_illiquidity` (row 13): reformulated to sqrt-law volume scaling (Kyle & Obizhaeva 2016;
  Lillo, Farmer & Mantegna 2003 — real-data-fitted impact exponent γ=0.512, matching the theoretical
  0.5) + geometric-mean aggregation (Hasbrouck 2009 — an order of magnitude more outlier-robust than
  median or raw mean on real MES rolling windows). An activity-clock (dollar-volume-bar) alternative
  was tested and REJECTED — empirically worse (CV=1.175 vs 0.918), unlike the other dims where
  activity-clock treatment helped. Committed (`a6d0630`). **Follow-up not yet done**:
  `FeatureScaler.h`'s dim11 calibration (`AMIHUD_ABSOLUTE_FLOOR` etc.) was tuned for the old
  linear-ratio scale and needs re-derivation against the new formula's real distribution.
- `fast_mean_rev_z` (row 19): its unwired `ActivityClockMeanReversion.h` reformulated to median/MAD
  ahead of its still-open wire-or-drop decision. Committed (`b0ab21a`).
- **Next action**: `FeatureScaler` dim11 recalibration (above), then remaining ledger rows
  (`relative_range`/`liq_fragility`'s own ambiguity, row 4's redundancy question) — see
  `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` §7.

---




**Elite Feature Set Curation initiative, spec:
`docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` — supersedes pairwise
dim-redundancy fixes going forward. Operator mandate: whole-vector, institutional, no going back to
one-off pairwise patches. Read that spec before continuing this thread — it supersedes the narrower
`log_scale_ratio`/`log_scale_expansion_ratio`-only framing in the bullets just below.**

**UPDATED 2026-08-31 (later same day) — Phase 0 (Gaussian-moment audit) is now CLOSED OUT, bar 4 open
items. Everything in this block that used to say "not yet applied" is done. Read this block, not the
older bullets right below it, for current status.**

- **Fixed and build-verified today**: `burstiness_index` (robust CV, `MAD/median × 1.4404199` — a
  newly-derived Poisson-neutrality constant, NOT the standard 1.4826, `EventVelocityEngine.h`);
  `vol_convexity` (REMOVED from the schema entirely, 19D→18D, `../schema/mts_schema.fbs` — already
  decided 2026-08-25, executed today, not reformulated); `mean_rev_z` (median/MAD price z-score +
  median-centered lag-1 autocorrelation, Kim & White 2004, `StudyHelperFunctions.cpp`'s
  `CalculateMeanReversionSpeed`). Full detail, all three: the initiative spec's §4 Phase 0.
- **Real bug found+fixed as a side effect of the schema shrink**: `FeatureScaler.h`'s
  `LOGZ_WINSOR_SIGMA_OVERRIDE` array literal was missing one element (17 for an 18-slot array),
  silently misaligning `liq_fragility`'s `21.26f` calibrated bound to index 11 instead of 12 — caught
  by `test_feature_scaler.cpp` failing, not by inspection. Rewrote the array with one explicit,
  individually-commented literal per dim so this class of miscount can't recur silently.
- **Verification**: `test_feature_scaler.cpp`, `test_event_velocity_engine.cpp`,
  `test_bipower_variation.cpp` all pass; `./build_dll.sh --no-clean` builds clean.
- **NOT committed yet** — large batch (schema change + `StudyHelperFunctions.{cpp,h}`,
  `TripleScreen1/2/3.cpp`, `ContextManager.{cpp,h}`, `EventVelocityEngine.h`, `FeatureScaler.h`,
  `test_feature_scaler.cpp`, deleted `tests/cpp/fixtures_dim4_raw.h`) — pending explicit commit ask.
- **Still open, unchanged**: the 3 ambiguous Phase 0 cases (`hurst_exponent`/`fast_hurst_exponent`,
  `amihud_illiquidity`, `relative_range`/`liq_fragility` — need a literature decision, not a
  mechanical fix) and `fast_mean_rev_z`'s wire-or-drop call. Phase 1 (whole-vector correlation audit)
  is unblocked and ready to scope, not yet started.
- **New methodological finding, same day, NOT yet acted on**: scoping a re-measurement of
  `amihud_illiquidity`/`liq_fragility`'s cross-state discrimination ratio now that both are
  live-reactive (2026-08-29) surfaced that this would be circular against the *existing*
  `models/hmm_model.pkl` — its state labels were learned from the pre-Phase-0, still-contaminated
  vector. Recorded as an open question in the initiative spec's §5, not resolved: either defer to a
  post-retrain re-measurement, or build a model-independent lead-time test instead (no HMM needed,
  same methodology as `jump_ratio_eval.cpp`/`drift_location_eval.cpp`). Nothing built for this yet.
- Doc sync done today: `PRODUCTION_TRIAGE.md` row 1, `CLAUDE.md`/`GEMINI.md`'s condensed Row 1
  pointers, and the initiative spec itself all updated to current state (this entry). Still
  outstanding: `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`'s per-dim rows
  for `burstiness_index`/`mean_rev_z`/`vol_convexity` (removed).

---

**Older bullets below, kept for the reasoning trail on `log_scale_ratio`/`log_scale_expansion_ratio`
specifically (superseded as the "current state" summary by the block above, still accurate on their
own narrower topic):**

- Also confirmed, load-bearing for the whole initiative: this system's Student-t
  HMM (`lbrnet/lbrnet/models/student_t_hmm.py`) uses **hard-enforced diagonal covariance**
  (`covariance_type='diag'`, raises otherwise) — the curse-of-dimensionality failure mode is
  double-counted evidence under violated conditional independence, NOT covariance-matrix
  ill-conditioning (impossible under diagonal covariance). Every redundancy judgment in the new spec
  is anchored on this fact.
- Both `log_scale_ratio` (was `log_variance_ratio`) and `log_scale_expansion_ratio` (was
  `correction_action`) had their raw-variance formulas deleted and replaced with Barndorff-Nielsen &
  Shephard bipower variation (`include/BipowerVariation.h`, single source of truth, natively tested) —
  raw sample variance let single-tick jumps dominate the sum quadratically (Mandelbrot 1963),
  producing false volatility-regime signals. A fixed-ν rolling Student-t M-estimator alternative was
  considered and rejected: measured via real microbenchmark at ~200ns-5.6µs per call, 2-3 orders of
  magnitude over this system's hot-path budget at real window sizes — full reasoning in
  `lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_118`/`118_REPLY`.
- Real, measured result feeding the new initiative: the two now correlate at 0.7638 both-raw, 0.8085
  both-fixed against 38.5M real MES ticks — confirms Spearman (1904)'s correction-for-attenuation
  prediction empirically, not just in theory.
- **Still open**: commit everything from this thread (still uncommitted, along with the newer Phase 0
  fixes above); re-audit `log_scale_ratio`/`log_scale_expansion_ratio`'s `FeatureScaler.h` calibration
  against real data (both currently disabled pending that audit, not fabricated placeholders); update
  `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`'s dim0/dim3 rows.

---

**Found from an `lbrnet`-rooted session (2026-08-31), doing the lbrnet-side integration work
handed off in `docs/superpowers/specs/2026-08-30-context-converter-lbrnet-handoff.md`: `tools/context_to_parquet`
needs a `--stats-json` output before `train_student_t_hmm.py` can be migrated onto it. NOT FIXED —
fix this from a MindfulTrader-rooted session, then tell lbrnet it's ready.**

- **Problem**: `lbrnet/lbrnet/scripts/train_student_t_hmm.py`'s `load_context_observations()` has an
  existing full-stream data-quality gate (lines ~1723-1730) that hard-fails training when
  `seq_mismatch / pair_attempts` exceeds `config.max_seq_mismatch_ratio`, or raw `seq_mismatch` count
  exceeds `config.max_seq_mismatches`. These two counts currently come from `self.load_metrics`,
  populated by whichever of the trainer's three loader methods ran (see the handoff doc, and
  `train_student_t_hmm.py`'s own `_load_unbounded_mo_ss_context`/`_load_bounded_mo_ss_context`).
  `tools/context_to_parquet.cpp` already computes the exact same counts internally — its `ReadCounters`
  struct (`tools/context_reader.h:124-133`) has `market_records`, `system_records`, `pair_attempts`,
  `aligned_pairs`, `sequence_mismatches`, `sequence_regressions`, `unpaired_market_records`,
  `unpaired_system_records` — but `main()` (`tools/context_to_parquet.cpp:408-410`) only ever prints
  `aligned_pairs`/`sequence_mismatches` to stdout as human-readable text (`"✅ wrote %s (mode=%s,
  aligned_pairs=%zu, sequence_mismatches=%zu)\n"`), never `pair_attempts` (the ratio's denominator),
  and writes no JSON stats file at all — the only JSON it writes is the freshness cache key
  (`--meta-path`, via `context_cache_key.h`'s `WriteCacheKey`), which is a different, smaller struct
  (`ContextCacheKey`, 6 fields, none of them `ReadCounters`). `tools/context_validate`'s
  `--report-json` doesn't cover this gap either — its JSON has no counts at all (only
  `input`/`rows_sampled`/`status`/`violations`/`warnings`), and it only ever samples a bounded
  head/tail slice (default `--max-pairs 50000`), never the full stream, so it can't stand in for a
  full-stream gate regardless.
- **Impact**: the lbrnet-side integration (subsystem #1 of the handoff, the one that actually
  unblocks fresh training runs) is blocked on this — there's no way to read the exact
  `pair_attempts`/`aligned_pairs`/`sequence_mismatches` counts for a full unbounded conversion from
  outside the C++ process today. User explicitly rejected both a stdout-regex approximation and
  dropping the gate — this must be fixed with real data, not worked around.
- **Solution** (small, surgical, mirrors an already-existing pattern in this same file pair — not a
  new design): add a `--stats-json PATH` flag to `context_to_parquet.cpp`, writing the final
  accumulated `counters` (the `ReadCounters` already summed across all chunks at
  `tools/context_to_parquet.cpp:359-363`) to a flat JSON file with all 8 fields, using the exact same
  `std::ofstream` field-by-field literal-write style `context_cache_key.h:48-59`'s `WriteCacheKey()`
  already uses for `ContextCacheKey` (no JSON library needed, this codebase's established precedent
  for small fixed-schema sidecars). Wire it in next to the existing `--meta-path` handling in `main()`
  (around line 390), write it once at the very end after `counters` has its final accumulated values
  (works for all three modes — unbounded's `counters` accumulates across the chunked loop, head/tail's
  `counters` is a single `ReadBoundedHead`/`ReadBoundedTail` return value). Add native test coverage
  matching this repo's existing `tools/test_context_validate.cpp` convention.
- **After this ships**: tell the `lbrnet`-rooted session (or update
  `docs/superpowers/specs/2026-08-30-context-converter-lbrnet-handoff.md` §3/§7 directly) that
  `--stats-json` is available, so `train_student_t_hmm.py`'s `_load_unbounded_mo_ss_context` can read
  it and populate `self.load_metrics['pair_attempts']`/`['seq_mismatch']` with exact, not
  approximated, values.

---

**Two offline candidate-validation tools + real results — DONE and COMMITTED, 2026-08-30**
(`MindfulTrader` `62ea7ee`..`a5fe024`, following the converter work below). Brainstorm doc §5.0/§5.1,
§10.7/§10.8/§10.9.

- **§5.0 drift/location (`tools/drift_location_eval.cpp`): TESTED AND REJECTED.** Real 38.5M-row MES
  data, same-sign hit-rate test — hit_rate below 0.5 at every horizon (0.4854→0.4957), decaying
  toward null. Rejected before any schema/C++ commitment. First C++ tool in this repo to read
  Parquet directly; two real performance bugs found and fixed via actual measurement (unprojected
  Arrow read, 5+min→7.5s; per-signal binary search, 100+s hang→O(n) two-pointer merge).
- **§5.1 jump/bipower-variation ratio (`tools/jump_ratio_eval.cpp`): TESTED AND SURVIVES** — real,
  substantial effect, opposite the naive hypothesis: high jump-dominated realized variance predicts
  a CALMER, not more chaotic, near-term future (top-decile median\|forward return\| is
  55.2%/55.1%/58.3%/62.6% of bottom-decile's at 30/60/120/240min, CI excluding zero every time). Not
  yet promoted to a schema field.
  - **Real performance emergency mid-session**: exact multinomial bootstrap resample is memory-
    latency-bound at real ~3.85M-element decile-group scale (~95ns/gather) — a real run was killed
    after 61 minutes still on horizon 2/4 (projected ~97min total). Fixed via a weighted/
    exchangeable bootstrap (Praestgaard & Wellner 1993) above a 20,000-element threshold; two weight
    distributions tried and rejected (Poisson(1) itself measured ~73ns/draw, nearly as slow as the
    gather it replaced) before landing on Exponential(1) (Rubin 1981) — fast AND correctly scaled.
    A first attempt at the "correctly scaled" part (Uniform[0,2]) was itself caught by review
    producing CIs ~1.7-1.8x too narrow (wrong weight variance) before Exponential(1) replaced it.
  - **Mean→median correction, caught by direct user challenge, not a code review**: the first
    working version used `mean(\|forward_return\|)`, matching `dim_acceptance_eval.py`'s own
    precedent — but this codebase's own, later, harder-won standard for fat-tailed data is
    `FeatureScaler.h`'s median/MAD ("Taleb-consistent"), the same correction already made once
    before (Bowley/Moors replacing moment-based skewness/kurtosis, 2026-08-13). New
    `ComputeBootstrapMedianGapCI` (native-only, no Python counterpart) independently cross-validated
    against exact bootstraps under Normal/Student-t/Cauchy tails during review.
  - **New standing-methodology gap found, documented, NOT fixed**: both this tool and
    `drift_location_eval` treat heavily-overlapping per-tick forward-return signals as i.i.d. when
    bootstrap-resampling — understates every CI's width by an unquantified, plausibly large factor.
    Doesn't change either verdict (both effects are far from a naively-calibrated boundary) but is
    the **concrete next action** before any further candidate: a block bootstrap, reusing this
    repo's own measured Politis-White block-length precedent (`fractal_dim`, ≈404.82, `72ab967`),
    applied once, uniformly, not patched into one candidate's test. Full detail: brainstorm doc §10.9.
- Housekeeping done same session: brainstorm doc §5.0/§5.1/§9/§10.7-10.9/§1.9/§8 all updated to match
  (was showing jump-ratio as `CANDIDATE`/"not built", now stale-checked); `PRODUCTION_TRIAGE.md` row
  1 updated (Triage Protocol rule 7) — `NORTH_STAR_STATUS` unchanged (row 1 stays `IN_PROGRESS`, this
  is progress within the sub-thread, not a status transition).

---

**C++/Arrow `.context` converter — DONE and COMMITTED, 2026-08-30** (`MindfulTrader` `689465a`..
`54d1702`, `schema` `477b750`/`c7788f8`). Brainstorm doc §10 → 13-task plan
(`docs/superpowers/plans/2026-08-30-context-to-parquet-cpp-arrow-converter.md`), all done:
`tools/context_reader.h`/`context_to_parquet.cpp`/`context_validate.cpp`/`context_validate_stats.h`/
`context_cache_key.h`, plus `schema/scripts/generate_contract_header.py` fixing
`regenerate_schema.sh`'s hand-maintained-duplicate defect. 54 native tests pass, `./build_dll.sh
--no-clean` green. `WIRE_SCHEMA_VERSION` bumped 230→240 (mts_schema.fbs), now hard-refused on
mismatch by `context_reader.h::OpenContextFile()` — closes the exact silent-corruption risk this
whole thread exists for. One real bug (blanket `risk_gate_` prefix silently defeating the new
naming design) caught and fixed *after* the first e2e "success," by writing a stronger assertion.
**`lbrnet` side NOT started** — full handoff at
`docs/superpowers/specs/2026-08-30-context-converter-lbrnet-handoff.md` (CLI flag mapping, breaking
column-rename table, freshness-cache JSON-schema incompatibility, `event_data.context` 16D-legacy
migration still undecided). `lbrnet/scripts/validate_lbr_file.py`'s dead `check_context_file_quality()`
deleted directly (zero callers, explicit instruction) — left uncommitted in that repo.

---


**OPERATIONAL NOTE, 2026-08-29 — the sibling (Copilot-backed) is out of tokens, unavailable for the
next couple of days.** This session is continuing their in-progress `amihud_illiquidity`/
`liq_fragility` recalibration directly rather than waiting — picking up exactly where they left off
(their tool, their fix), not duplicating. If you're reading this as the sibling coming back online:
check the entries below dated after this note before assuming your own last state is still current.

**RECALIBRATION IN PROGRESS, 2026-08-29 — real, dramatic confirmation the follow-on was genuinely
required, not precautionary.** Ran the sibling's `tools/amihud_liqfragility_recalibration.{cpp,py}`
(`--live-bar-min-volume 10`, real 38.5M-tick MES history) against the REAL `FeatureScaler`:

- **`amihud_illiquidity` (dim 12)**: current production bound (flat default, 6.0σ) — **rate-at-bound
  = 8.5254%** (i.e. 1 in ~12 live readings clip). mean|z|=2.71, max|z|=19583.81(!), p99=35.6,
  p99.9=111.1. For comparison, the OLD bar-gated computation's own audit found **0.000%** clip rate
  at this same bound — the live-reactive signal is categorically more volatile, exactly as §1.11
  predicted, now measured not assumed.
- **`liq_fragility` (dim 13)**: current bound (LOGZ override, 12.0σ) — **rate-at-bound = 12.8465%**.
  mean|z|=6.58, max|z|=481.10, p99=69.9, p99.9=207.1. Same story, worse magnitude.
- **A third, independent bug found while starting this**: `FeatureScaler::DIM_AMIHUD_INDEX` was
  stale at `11` (should be `12` — never updated when `fast_hurst_exponent`'s dim9 insertion shifted
  every later index). Fixed directly (1-line change, already in the working tree).

**WINSORIZATION RECALIBRATION DONE, BUILD-VERIFIED, 2026-08-29.** Tail samples dumped, GPD/EVT fit
(Pickands-Balkema-de Haan, `scipy.stats.genpareto.fit`, u=p99, floc=0 — same methodology as every
other dim's bound in this project, Gang doc D6/D7/Task 3/4):

- **`amihud_illiquidity`**: u=35.63, n_tail=7709 (1.000% exceedance), ξ=+0.3752 (Fréchet/unbounded).
  p=1/N return level (N=38,540,567 real ticks) = **6821.5**. `DIM_WINSOR_SIGMA_OVERRIDE[12]`: 0.0 →
  6821.5.
- **`liq_fragility`**: u=69.87, n_tail=7708 (1.000% exceedance), ξ=+0.1857 (Fréchet/unbounded).
  p=1/N return level = **2472.5**. `LOGZ_WINSOR_SIGMA_OVERRIDE[13]`: 12.0 → 2472.5.
- Both bounds updated in **`include/FeatureScaler.h`** (compiled defaults) **and
  `config/execution_params.json`** (which overwrites the compiled defaults at load time — updating
  only the `.h` would have shipped a fix that silently never takes effect). `./build_dll.sh
  --no-clean` confirmed succeeding with both changes in place.
- Shrinkage (`SHRINKAGE_SCALE_MIN[12]`/`[13]`) explicitly **NOT** re-audited — flagged stale in both
  files' comments rather than silently left looking resolved. Real remaining follow-on, not done.

**`kLiveBarMinVolume` empirical tuning — DONE, decisive result, 2026-08-29.** Tested 5/10/50 in one
tick pass (instead of 3 separate 38.5M-tick reads):

| guard | amihud max\|z\| | amihud mean\|z\| | amihud rate@6.0σ | liq_fragility rate@12.0σ |
|---|---|---|---|---|
| 5.0 | 19583.81 | 2.7629 | 8.5723% | 12.8439% |
| 10.0 (current) | 19583.81 | 2.7107 | 8.5254% | 12.8465% |
| 50.0 | **3270.57** | 2.5286 | 8.1556% | 12.8722% |

**Guard=50 measurably tames the worst-case outlier (19583.81 → 3270.57, a 6x reduction in the single
most extreme spike — exactly the near-empty-denominator failure mode this guard exists to prevent)
while only costing ~7% of mean reactivity and a fraction of a point of clip rate.** `liq_fragility`
shows no meaningful difference across any guard value. **Decision: kLiveBarMinVolume 10.0 → 50.0.**
Real tradeoff worth naming honestly: a higher guard delays the live term contributing until later in
each bar, costing some of the lead-time benefit live-reactivity exists for (§1.10) — but the mean|z|
difference (2.71→2.53) suggests this cost is modest, not the dominant effect.

**FINAL, DONE, BUILD-VERIFIED, 2026-08-29 — re-fit against guard=50's actual distribution complete.**
`kLiveBarMinVolume` shipped at **50.0** in both `StudyHelperFunctions.cpp` call sites (Amihud's and
LiqFragility's own constants). GPD fit against guard=50's real data:

- **`amihud_illiquidity`**: u=32.68, n_tail=7709 (1.000%), ξ=+0.2920 (Fréchet). p=1/N return level =
  **2706.0** (meaningfully smaller than guard=10's 6821.5 — guard=50 genuinely tamed the tail, this
  wasn't a wasted re-fit).
- **`liq_fragility`**: u=70.02, n_tail=7709 (1.000%), ξ=+0.1881 (Fréchet). p=1/N return level =
  **2524.5** (barely changed from guard=10's 2472.5, consistent with liq_fragility being
  guard-insensitive).
- Both final bounds updated in `include/FeatureScaler.h` and `config/execution_params.json`.
  `./build_dll.sh --no-clean` succeeds with the complete, internally-consistent change set (new
  guard + matching bounds).

**This closes both required follow-ons from §1.11 in full.** Only remaining open item for these two
dims: the shrinkage re-audit (`SHRINKAGE_SCALE_MIN[12]`/`[13]`, disabled, flagged stale in both
files' comments, not re-derived) — a separate question from winsorization, tracked, not silently
closed.

---

**SHRINKAGE RE-AUDIT DONE, BOTH DIMS, BUILD-VERIFIED, 2026-08-30 — this closes row 13/14's last
open item, both rows now `IN` in §9 (terminal).**

- Extended `tools/amihud_liqfragility_recalibration.cpp`'s `ZStats` to track local MAD alongside
  `|z|` (correlation, top-1%-extreme-event MAD ratio, MAD percentiles) — real evidence, not
  eyeballing.
- **`amihud_illiquidity` (dim 12)**: correlation(localMAD,|z|) = -0.0340 — weak, not a collapse
  signature. Already has its own dedicated `AMIHUD_ABSOLUTE_FLOOR` mechanism doing this job.
  `SHRINKAGE_SCALE_MIN[12]` stays `0.0f` — audited clean, not skipped.
- **`liq_fragility` (dim 13)**: correlation(localMAD,|z|) = -0.2058 — real, decisive collapse
  signature (top-1%-extreme |z| events at 19.6% of median local scale; the single worst |z| event's
  own local scale lands almost exactly at the series' own p0.1). Enabled `SHRINKAGE_SCALE_MIN[13] =
  0.0035` (observed local-scale p1).
- **Verification bug caught and fixed before accepting the result.** The first attempt to confirm
  the fix worked came back byte-identical to the pre-fix run — correctly treated as suspicious
  rather than accepted. Root cause: the tool's `liqFragZ` manually recomputed the plain
  (non-shrinkage) z formula at the call site, silently bypassing `ComputeShrinkageZ()` entirely, so
  it never actually exercised `SHRINKAGE_SCALE_MIN[13]` either way. Fixed by exposing the real
  shrinkage-blended `zLog` unconditionally via `lastRawZ[13]` in `FeatureScaler.h`'s LOGZ branch
  (mirroring the SOFTLOGZ path's existing `lastRawZ[i] = z` precedent) and updating the tool to read
  that instead of recomputing.
- **Rerunning with the real value showed the fix genuinely works**: max|z| 503.22 → 20.59,
  correlation -0.2058 → +0.0016 (collapse signature actually gone, not masked). `amihud`'s output
  was byte-identical across both runs, as expected (shrinkage stays disabled there, and my fix only
  touched the LOGZ branch) — a useful consistency check that the fix didn't have side effects.
- **This also caught the `liq_fragility` winsorization bound (2524.5, derived 2026-08-29) as itself
  computed from the broken, non-shrinkage-corrected z-distribution — ~120x oversized.** Re-derived
  GPD fit against the corrected distribution: u=p99=8.297, n_tail=7709, ξ=-0.1452 (Weibull/bounded —
  the tail is fundamentally tamer once shrinkage genuinely fires, not a Fréchet artifact of inflated
  data), p=1/N return level = **21.2565 → 21.26**. `LOGZ_WINSOR_SIGMA_OVERRIDE[13]` updated:
  2524.5 → 21.26, in both `include/FeatureScaler.h` and `config/execution_params.json`.
- `./build_dll.sh --no-clean` succeeds with the complete change set (shrinkage floor + corrected
  winsor bound + diagnostic exposure). No other dims touched.
- Updated `docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md` §6.1
  and §9 rows 13/14 (both now `IN`), and §8's non-terminal count (18/25 → 16/25, 9 terminal rows).

---

**ACK, 2026-08-29 — saw your doc updates.** `amihud_gate_percentile_spec.md`'s expanded backward-
compat section (the 32.1%-absent `risk_gate_context` structural gap, `EventDataCollectorStudy.cpp`'s
`LogSynchronizedEvent()` not passing a `RiskGateContextT`) and the Gang-doc changelog entry (Finding
10 mirror, `lempel_ziv` tail-irrelevance addendum) are noted — neither changes my current priority
queue below, but the `risk_gate_context` absence gap is a real, unclaimed follow-up worth someone
picking up explicitly rather than letting it sit as a mentioned-but-orphaned finding.

---

**PRIORITY #2 DONE (code side), 2026-08-29 — `amihud_illiquidity`/`liq_fragility` are now
live-reactive.** Per §1.11 of `docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-
brainstorm.md`: both dims were computed once per bar inside `UpdateObservationVectorSubgraphs`'s
gate, reading the last CLOSED bar's range/volume — turning the vector's #2 and #4 discriminators
(causally leading illiquidity-spiral indicators) into lagging ones. Fixed:

- `CalculateAmihudIlliquidity`/`CalculateLiquidityFragility` (`src/StudyHelperFunctions.cpp`) pulled
  OUT of `UpdateObservationVectorSubgraphs`'s once-per-bar gate, now called directly every tick from
  `TripleScreen3.cpp` (same pattern already used for `mean_rev_z`/`vol_convexity`/`micro_asymmetry`).
- Both now read the CURRENT still-forming bar (`sc.Index`) instead of the last closed one
  (`sc.Index - 1`), guarded on a minimum volume-so-far threshold (`kLiveBarMinVolume = 10.0`,
  **UNCALIBRATED placeholder** — see required follow-on below) below which each falls back to
  carry-forward (Amihud: skips the live term, keeps the closed-bar window only; LiqFragility:
  returns `prev_fragility` unchanged) rather than manufacturing a near-empty-denominator artifact.
- `UpdateObservationVectorSubgraphs`'s signature lost 3 now-unused params (`Subgraph_AmihudIlliquidity`,
  `Subgraph_LiqFragility`, `Subgraph_VolumeSMA` — the last was only there to feed LiqFragility).
  Single call site (`TripleScreen3.cpp`) updated to match.
- `./build_dll.sh --no-clean` — **builds clean, 284s**. `test_carry_forward_calculators` (the native
  test covering `cfc::ComputeAmihudIlliquidity`, unchanged by this edit) still all-pass, confirming
  the pure aggregation logic wasn't disturbed — only the caller-side windowing/gating changed.

**REQUIRED FOLLOW-ON, NOT YET DONE, please pick up — this is not optional per §1.11's own text**:
rebuild `FeatureScaler.h`'s winsorization/shrinkage bounds for dims 11 (`amihud_illiquidity`) and 13
(`liq_fragility`, current index — see the 19-dim map further down this file) against a genuine
tick-level replica of the NEW live-reactive computation, the same way `log_variance_ratio`'s replica
was rebuilt once already. The bounds currently in `FeatureScaler.h`/`config/execution_params.json`
were calibrated against the OLD bar-gated distribution and are now measuring a different, more
volatile signal (intra-bar reactivity changes the value's variance/tail behavior, especially right
after the `kLiveBarMinVolume` guard releases each bar). Do not defer this citing recalibration
effort — see `feedback_recalibration_effort_not_a_design_reason` memory. The `kLiveBarMinVolume =
10.0` guard constant itself is also an unvalidated placeholder and should be tuned as part of the
same tick-level study (there's currently no evidence 10 contracts is the right cutover point vs.,
say, 5 or 50 — pick this empirically, not by continuing to guess).

---

**BUILD UNBLOCKED, 2026-08-29 — priority #1 from the HANDOFF below is done.** Root cause was NOT the
`.fbs` (already correct, 19 fields) — it was `schema/regenerate_schema.sh`'s embedded heredoc for
`mts_schema_contract_generated.h`: `ObservationData`'s field list there is a **hand-maintained copy**
of the schema, hardcoded to `kObservationDim = 17` and missing `fast_hurst_exponent`/`fast_mean_rev_z`
entirely (the "regenerated" file's log output claiming success was misleading — it regenerated from
a stale hardcoded template, not from the `.fbs`). Fixed by hand-editing that heredoc to match the
live 19-field order (`kObsFastHurstExponent = 9`, `kObsFastMeanRevZ = 18`, shifting 10-17
accordingly), re-ran `regenerate_schema.sh`, then `./build_dll.sh --no-clean` — **builds clean,
323s, `bin/MindfulTrader.dll` produced.**

**REQUIRED FOLLOW-ON, not yet done, please pick up**: this is the SECOND time this exact template
has drifted from the real schema (first time undetected for weeks per the fast_taleb_kurtosis
history in this file below). A hand-maintained duplicate field list that silently diverges from the
`.fbs` it's supposed to mirror is a structural defect in the generator itself, not just a one-off
fix. Please write a spec (`docs/superpowers/specs/`) for making `schema/regenerate_schema.sh` derive
`ObservationData`'s `kObs*` constants / field-name array / `MakeObservationData`/`ToObservationArray`
directly from `mts_schema.fbs` at generation time (e.g. parse the struct's field list out of the
schema, or drive the heredoc from flatc's own reflection output) instead of maintaining a second,
independently-hand-edited copy inside the shell script. Scope this as MindfulTrader-side work since
it blocks this repo's build specifically — lbrnet/schema cross-repo implications can wait, per
current instruction to stay focused on MindfulTrader until it's done.


---

**HANDOFF, 2026-08-29 — implementation of the observation-vector brainstorm's Phase 1 work goes to
you from here.** Primary reference: `docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-
vector-brainstorm.md` — read §0 (goal/scope), §6 (converging recommendation + the two-phase
validation plan, §6.0), and §9 (the 25-row per-dim decision ledger) before starting; §5.0-§5.5 have
the per-candidate technical detail. This doc is explicitly NOT yet ground truth (§8) — 18 of 25 §9
rows are non-terminal, that's the actual scope of what's left.

**Priority 1 (build blocker) is RESOLVED as of this update** — `./build_dll.sh --no-clean` succeeds
cleanly, confirmed just now. The schema-contract regen gap flagged below got fixed elsewhere in this
working tree (very recent `schema/regenerate_schema.sh` + generated-header timestamps, seconds
apart) — not by this note's author. The `burstiness_index`→`raschkeBurst` redirect is now
build-verified too (§9 row 2). Start at priority 2 below.

**Priority order, per §6 (highest first):**
1. **Unblock the build**: `./build_dll.sh --no-clean` currently fails — see the next entry below,
   still unresolved as of this handoff. Nothing else here can be build-verified until this is fixed.
2. **`amihud_illiquidity`/`liq_fragility` live-reactivity** (§1.11, §9 rows 13/14) — decided, not
   implemented. Make both read the live/still-forming bar (currently bar-gated, TS3), guarded on a
   minimum volume-so-far threshold for Amihud's near-empty-denominator risk (carry-forward-last-
   valid-value pattern, same as `RELATIVE_RANGE_LAST_VALID_VALUE`/`FRACTAL_DIM_LAST_VALID_VALUE`).
   Required follow-on, not optional: rebuild both dims' `FeatureScaler.h` winsorization bounds from
   a genuine tick-level replica (same rebuild `log_variance_ratio` already needed once) — don't ship
   the wiring change and skip this citing recalibration cost, see `feedback_recalibration_effort_
   not_a_design_reason` memory if that reasoning starts to creep in.
3. **§5.0 drift/location prototype** (§9 row 20, top-priority new candidate) — the largest identified
   axis gap. Reusable-computation check already done (2026-08-29, this session): ADX was formally
   retired March 2026 and wouldn't have been the right tool anyway (measures trend strength, not
   signed return level) — build the volatility-normalized return z-score construct described in §5.0
   fresh. Prototype offline (Python, real MES data) before any C++, same cross-state-ratio /
   redundancy methodology as `tools/dim_acceptance_eval.py` (§6.1) — but see that tool's own
   caveats (bar-gated comparison columns, Phase 2 timing) before trusting any result out of it.
4. **§5.1 jump/bipower-variation ratio prototype** (§9 row 21) — cheap, same log-return series
   `TailRiskEngine` already ingests, Python-only first pass.
5. **§5.4 Hurst×volatility-level cross-term** (§9 row 22) — zero new data, pure feature-engineering,
   tests whether "fat-tail state" and "Trending-High-Vol crisis" (§1.4/§1.7) are the same phenomenon.

**Explicitly NOT priority right now** (§6): `fast_mean_rev_z` wiring (paused, empirically null),
`recurrence_rate` (done, genuinely orthogonal to this document's goal), Hawkes/Recovery-construct
(deferred to post-workstation window).

**BUILD BLOCKED, 2026-08-29 — flagging for whoever picks up `fast_mean_rev_z`'s schema work next,
not fixed here.** `./build_dll.sh --no-clean` currently fails: `mts_schema_contract_generated.h`
expects 18 `ObservationData` constructor args, `mts_schema_generated.h` (matching the live 19-field
`mts_schema.fbs`, `fast_mean_rev_z` appended) expects 19 — the contract header wasn't regenerated

**BUILD BLOCKED, 2026-08-29 — flagging for whoever picks up `fast_mean_rev_z`'s schema work next,
not fixed here.** `./build_dll.sh --no-clean` currently fails: `mts_schema_contract_generated.h`
expects 18 `ObservationData` constructor args, `mts_schema_generated.h` (matching the live 19-field
`mts_schema.fbs`, `fast_mean_rev_z` appended) expects 19 — the contract header wasn't regenerated
together with the schema change. `ContextManager.h:329` also references `kObsFastHurstExponent`,
missing from the stale contract header. This is **pre-existing, uncommitted, in-progress state**
(all of `ContextManager.h`/`.cpp`, `LocalRiskContext.h`, `mts_schema_contract_generated.h` were
already dirty before this note) — not caused by the unrelated `burstiness_index`→`raschkeBurst`
redirect landing in the same working tree (see below). Per `CLAUDE.md`: fix is `regenerate_schema.sh`,
never hand-edit generated headers or call `flatc` directly — held off running it unilaterally since
this is someone else's in-progress schema work, not confirmed safe to regenerate over. Whoever
resumes this: run `regenerate_schema.sh`, then `./build_dll.sh --no-clean` to confirm both this and
the burstiness redirect below compile clean together.

**`burstiness_index` redirected to `raschkeBurst`, 2026-08-29 (Gang doc Finding 10) — wiring done,
build NOT YET VERIFIED due to the blocker above.** `TripleScreen2.cpp` no longer computes its own
bar-cadence True-Range half-window proxy; it now reads `ContextManager::GetRaschkeBurst()` (new
getter, `include/ContextManager.h`) — the real, already-computed event-arrival-timestamp
CV-burstiness, refreshed every tick via `CheckAndTriggerHMM`, previously wired only to
`LocalRiskContext`/`RiskGateContext`, never the HMM's own observation vector. Dead code removed:
`CalculateBurstiness(sc, lookback_n)` (`StudyHelperFunctions.cpp`/`.h`), zero remaining callers
confirmed by grep before deletion. **REQUIRED FOLLOW-ON, not yet done**: `FeatureScaler.h`'s dim1
(`burstiness_index`) winsorization/shrinkage bounds were calibrated against the old proxy's
distribution (a dedicated 22,510-line tick-level fixture, `tests/cpp/fixtures_dim1_raw.h`, exists
for the *old* signal) — needs the same tick-level-replica recalibration `log_variance_ratio` already
went through once, against `raschkeBurst`'s real distribution instead. Full detail: `docs/
superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md` §9 row 2, §5.5.

**EMPIRICAL RESULT, 2026-08-28 (third pass — ran the test the REVISED section below specified).
Verdict: NULL RESULT. Neither variant showed predictive power. Task 5 stays paused; do not wire
`fast_mean_rev_z` into production on this evidence, and do not treat this as license to replace
`mean_rev_z` either — both are empirically silent, not distinguished.**

**What was run** (`tools/mean_rev_z_variant_comparison.{cpp,py}`, real MES data — 38,547,467
1-second bars + 75,599 15-min bars, `lbrnet/data/raw/mes_continuous_ticks.parquet` /
`mes_ripple_15m.parquet`): built both variants (`time_bar` = faithful port of the live
`CalculateMeanReversionSpeed`, `bars=15min`; `activity_clock` = real `ActivityClockMeanRevZ` header,
imbalance bars) on the same data, fed each through `Scoring.cpp:305`'s exact gate condition
(`score > 2.0f`), then measured forward-return sign-hit-rate at 30/60/120/240-min horizons for every
resulting signal. Imbalance threshold calibrated to 900 first (bar-formation rate: 68,706 activity
bars vs 75,599 15-min bars — comparable order of magnitude; the first attempt at threshold=15 was
invalid, producing 8.78M bars, a 147x-mismatched sampling rate, and was discarded).

**Result — both variants indistinguishable from a coin flip, at every horizon tested:**

| Horizon | Variant | n | hit_rate | 95% CI (Wilson) | p (vs null=0.5) |
|---|---|---|---|---|---|
| 30min | time_bar | 7598 | 0.4932 | [0.4819, 0.5044] | 0.233 |
| 30min | activity_clock | 8698 | 0.5054 | [0.4949, 0.5159] | 0.314 |
| 60min | time_bar | 7581 | 0.5019 | [0.4907, 0.5132] | 0.739 |
| 60min | activity_clock | 8669 | 0.5076 | [0.4970, 0.5181] | 0.159 |
| 120min | time_bar | 7539 | 0.5127 | [0.5014, 0.5239] | 0.028 |
| 120min | activity_clock | 8552 | 0.5065 | [0.4960, 0.5171] | 0.226 |
| 240min | time_bar | 7478 | 0.5055 | [0.4942, 0.5168] | 0.343 |
| 240min | activity_clock | 8316 | 0.4897 | [0.4789, 0.5004] | 0.059 |

7 of 8 cells fail to reject the null outright. The one nominal hit (`time_bar`@120min, p=0.028) does
not survive Bonferroni correction for the 8 tests run (needs p<0.00625) — it's exactly the false-
positive rate you'd expect from noise across 8 comparisons, not evidence. Every CI is tight (±1.1-1.2
points) and contains 0.50, so this isn't underpowered/inconclusive — at n≈7500-8700 per cell it had
the power to detect a ~2-3 point edge and found none, for either variant.

**Scope of this null, stated precisely** (do not over-generalize it): this only tests "`mean_rev_z`
(or `fast_mean_rev_z`) crossing 2.0 in isolation predicts forward-return sign" at these 4 horizons,
at this sampling calibration. It does NOT test either variant combined with `Scoring.cpp`'s other
pattern conditions, and does NOT test HMM per-state discrimination power (the acceptance criterion
the REVISED section below actually proposed as the real bar, alongside this forward-return test) —
that remains untested and is the natural next step if this thread continues.

**Decision given this evidence**: neither of the two branches the REVISED section below anticipated
("one dominates cleanly → replace" / "both show real incremental power → keep both additive")
occurred. There is no empirical basis from this test to ship `fast_mean_rev_z` additively (it showed
no discriminative edge over what's already live), and no basis to replace `mean_rev_z` with it
either (the activity-clock variant didn't outperform, it was equally silent). Recommend: leave Task
5's wiring paused (schema field + header + `LocalRiskContext`/`ContextManager.h` constants stay as
already committed/uncommitted, but do NOT wire the computation into `ContextManager.cpp`'s
activity-clock block or add the 19th entry to `FeatureScaler.h`'s calibration arrays on this
evidence). If the HMM-discrimination test is run later and also comes back null, the honest
conclusion extends further: the live `mean_rev_z > 2.0f` gate itself (`Scoring.cpp:305`) may not be
carrying real edge, which is a materially bigger finding than this task scoped — flag, don't fix
here.

---

**REVISED, 2026-08-28 (second pass — first pass below was too soft, corrected after direct user
pushback: "stop patching, be institutional"). Verdict: PAUSE Task 5's wiring. Do not ship the
additive twin as a default. Run the horse-race measurement below first, decide from evidence.**

**Why the first pass was wrong, not just under-argued**: it concluded "additive twin, ship it" and
defended that with "protects the live gate, zero risk, reversible" — that's a risk-aversion
rationalization standing in for a modeling decision, the same shape of error already caught twice
this session (the unresearched Hurst claim; the initially-rejected fractal_dim split, reversed only
once real correlation data existed). Reversibility is not evidence of correctness.

**The real distinction that matters, on reflection**: kurtosis and `mean_rev_z` are NOT the same
shape of question. Calendar-time and trading-time kurtosis are plausibly two different, both-real
economic questions (tail-fatness at different aggregation levels can genuinely diverge). `mean_rev_z`
is not that kind of statistic — it's an estimate of a single **dynamical parameter** (OU-style
reversion speed/elasticity) of the price process itself. Under the same subordination theory this
whole program is built on (Clark 1973), that process genuinely evolves in trading time — meaning
the calendar-time estimate isn't "a different valid view," it's a **biased estimate of the same
underlying quantity**. Literature check (Lo & MacKinlay 1990, *J. Econometrics* 45(1-2):181-211;
Roll 1984, *J. Finance*) confirms the specific contamination story is real but small in its own
founding paper (≈0.07 of total autocorrelation) and mechanically about cross-asset portfolio
staleness / tick-level bid-ask bounce, neither of which cleanly describes a single, deeply liquid
futures contract at 15-min bars (microstructure-noise literature: bias is well known to fade past
~5-min aggregation). That weakens "contaminated, replace it" as a literature claim — but it does
NOT flip the conclusion to "therefore additive." It means: **we don't have a literature-only answer
either way** for whether the two variants are redundant (same signal, one biased) or complementary
(genuinely different information) — and the HMM has already had 4 dimensions killed this quarter
(`vol_convexity`, `tail_index`, `skewness_idx`, `micro_asymmetry`) for exactly the failure mode of
carrying no incremental discriminative power. Adding a highly-correlated second copy of the same
dynamical parameter is the textbook way to manufacture a fifth. Do not do that reflexively.

**Required before Task 5 continues wiring anything into `LocalRiskContext`/`FeatureScaler`/schema**:
run the same empirical-first discipline already used for `fractal_dim` (`tools/
fractal_dim_threshold_migration.{cpp,py}`), extended one step further than that tool went:
1. Compute both `mean_rev_z` (15-min) and `fast_mean_rev_z` (imbalance-bar) on the same real MES
   history. Correlation alone is a first signal (as it was for `fractal_dim`), not the deciding one.
2. **The test that actually decides it**: feed each variant through `Scoring.cpp:305`'s exact gate
   condition (`> 2.0f`, or its own percentile-matched equivalent for the new distribution) and
   measure forward-return/hit-rate on real `isMeanReversionPattern` trades for each variant,
   independently. This is the identical acceptance criterion already used to kill the four dead HMM
   dims — apply it here instead of exempting this decision from it.
3. Decide from the result, not from precedent: one variant dominates cleanly → **replace**, with a
   properly re-derived threshold (percentile-matching methodology, not a carried-over `2.0f`), and
   delete the loser — don't leave a known-inferior parallel field sitting in the vector.
   Both show real incremental power (e.g. via the HMM's own per-state discrimination metric,
   measured, not assumed) → keep both, additive, and say so because the data showed it.

**On whether both get fed to the HMM**: independent of the above — both would become separate
`ObservationData` wire fields either way if additive wins (18th/19th; already schema-appended,
uncommitted). Whether the HMM's *training* actually selects either as a model input is a separate
`lbrnet`-side decision (`HMM_KEEP_DIMS`), not yet made — same already-flagged gap `fast_taleb_
kurtosis`/`fast_hurst_exponent` currently sit in (wire field + generated binding exist, `lbrnet`'s
hand-written consumer code doesn't select them yet). Don't let that gap repeat a third time
unflagged.

---

**FIRST PASS, 2026-08-28 (superseded above, kept for the reasoning trail — this is the version the
user correctly rejected as "patching," not an institutional answer):** ADDITIVE TWIN, ship Task 5 as
planned, with "protects the live gate, zero risk, reversible" as the stated justification. Literature
grounding (Lo & MacKinlay 1990, Roll 1984, microstructure-noise/aggregation literature) was accurate
but used to justify a default rather than to actually decide — see REVISED section above for why
that's insufficient and what's required instead.

---

**ORIGINAL QUESTION FOR SIBLING, 2026-08-28 — resolved above, kept for the reasoning trail.** User
asked directly: why does `fast_mean_rev_z` need to be a SEPARATE additive
field at all, instead of just replacing `mean_rev_z`'s own computation in place with the
activity-clock version (same name, same slot) -- mirroring `recurrence_rate`/`skewness_idx`'s
already-shipped REPLACEMENT pattern, not `fast_taleb_kurtosis`/`fast_hurst_exponent`'s ADDITIVE one.

**Current plan's stated reasoning for additive** (`docs/superpowers/plans/2026-08-28-activity-
clock-mean-rev-hurst-recurrence.md` §0): `mean_rev_z` has a real, calibrated live gate
(`Scoring.cpp:305`, `isMeanReversionPattern && ctx.meanRevZ > 2.0f`), so — by direct analogy to
kurtosis's own justification (protect calibrated gates, add rather than silently move their input
distribution underneath them) — it was scoped additive, same as `fast_hurst_exponent`
(`Scoring.cpp:266`) just shipped.

**The real tension the user's question surfaces, worth the sibling's honest second opinion, not a
rubber-stamp of the above**: kurtosis's two clocks are plausibly BOTH informative (time-bar and
activity-clock both capture real, if different, market physics) -- symmetric case for additive.
`mean_rev_z`'s own literature motivation (spec `2026-08-25-observation-vector-institutional-
hardening-spec.md` §5a: microstructure literature -- non-synchronous trading, bid-ask bounce --
establishes that calendar-time sampling is itself *a source of spurious serial correlation*, the
exact quantity `rho` measures) is NOT symmetric in the same way -- it's an argument that the
TIME-BAR `mean_rev_z`'s `rho` term may be measuring a sampling ARTIFACT, not just "a different but
equally valid clock." If that's right, keeping the old (plausibly-biased) `mean_rev_z` live and
gated on forever, merely ADDING a corrected twin alongside it, arguably preserves a known-flawed
signal in production rather than fixing it -- closer in spirit to `skewness_idx`'s own precedent
(BowleySkewness was a straight-up better computation, not a second, complementary clock) than to
kurtosis's. Counter-consideration: unlike `skewness_idx`, `mean_rev_z` DOES have a live gate
(`2.0f` threshold) calibrated against the OLD distribution -- replacing in place without
re-validating that threshold against the new signal's real distribution risks silently breaking a
working gate, which is exactly the failure additive-twin design exists to avoid. This is a genuine
fork, not a settled question by analogy alone -- please give a real opinion, not just "kurtosis did
X so do X here too."

**Implementation state, low-cost to reverse either way**: `fast_mean_rev_z` has already been added
to `../schema/mts_schema.fbs` (appended after `mean_rev_z`, pure append, no reindexing) and
regenerated (`mts_schema_generated.h`/hand-fixed `mts_schema_contract_generated.h`), and
`schema/PENDING_SCHEMA_CHANGES.md`'s PSC-04 updated to cover it — all uncommitted in both repos.
`include/ActivityClockMeanReversion.h` (Task 4's pure extraction) is also done and natively tested,
independent of this question (needed either way — as the twin's computation if additive, or as
`mean_rev_z`'s new sole computation if replaced). **NOT yet done**: `LocalRiskContext`/
`ContextManager`/`FeatureScaler` wiring for `fast_mean_rev_z` specifically (Task 5's remaining
steps) — paused here pending this question, since going additive vs. replace changes which of
those get touched (a new field+twin member vs. swapping `mean_rev_z`'s own source and revalidating
`Scoring.cpp:305`'s `2.0f` threshold against the new distribution).

Prior state, 2026-08-28 — `fast_hurst_exponent` SHIPPED as `ObservationData`'s 18th field
(additive twin, `hurst_exponent`'s live gate `Scoring.cpp:266` protected, unchanged). Schema:
inserted after `fisher_info` (dim 9), shifting `tail_index`..`mean_rev_z` each +1 -- see
`docs/superpowers/specs/2026-08-26-activity-clock-lbrnet-handoff.md` §11 for the full index map.
**Two real, previously-undetected bugs found and fixed in the same pass, both predating this
session** (found via `test_feature_scaler.cpp` failing after the new dim insertion, not by design):
(1) `config/execution_params.json`'s `featurescaler_winsorization.dims` array had never been
updated for `fast_taleb_kurtosis`'s own 17D extension; (2) far more serious,
`FeatureScaler.h`'s `DIM_RECURRENCE_INDEX`/`DIM_FRACTAL_INDEX` dispatch constants were STILL 13/14
(pre-kurtosis 16D positions) since kurtosis shipped -- production had been silently applying static
scaling to the wrong dims for `recurrence_rate`/`fractal_dim` the entire time. Both fixed, all
native tests + full `./build_dll.sh --no-clean` verified green. This is Task 3 of
`docs/superpowers/plans/2026-08-28-activity-clock-mean-rev-hurst-recurrence.md` (`mean_rev_z`'s own
twin, Tasks 5-8, remains unimplemented); Task 1 (`recurrence_rate` activity-clock replacement,
`7f395d0`) and Task 2 (pure DFA/Hurst extraction, `65f6bbb`) shipped earlier the same day.

Prior state, 2026-08-28 — `fractal_dim`'s window-widening to 400 bars SHIPPED AND COMMITTED
(`72ab967`): production `CalculateFractalDimension` now delegates to the pure
`include/SevcikFractalDimension.h` extraction, `TripleScreen2.cpp` computes the HMM-bound (400-bar)
and `PositionManager.cpp`-gate-bound (short-window, unchanged) values independently, and the
`ContextManager.cpp:589` coupling point flagged by the spec was fixed (gate now reads a separately-
maintained `m_fractalDimShortRaw`, not the widened `obs[OBS_FRACTAL_DIM]`). Verified via new native
test (`test_sevcik_fractal_dimension.cpp`, all pass), full `./build_dll.sh --no-clean`, and
regression passes on `test_recurrence_rate_engine`/`test_rqa_epsilon`/`test_feature_scaler`. This
closes out row 1's last pure time-bar-widening item. **Plan written 2026-08-28 for the remaining
activity-clock work**: `docs/superpowers/plans/2026-08-28-activity-clock-mean-rev-hurst-recurrence.md`
scopes `mean_rev_z`/`hurst_exponent` as additive twins (`fast_mean_rev_z`/`fast_hurst_exponent`,
new schema fields 18/19, mirroring `fast_taleb_kurtosis`'s shipped pattern) and `recurrence_rate`
as an in-place replacement (mirroring `skewness_idx`'s shipped pattern, no schema change) — design
complete, NOT yet implemented. Prior state (2026-08-27): `skewness_idx`'s
activity-clock replacement + a real `FeatureScaler.h`
17-dim indexing bug fix both landed and committed (`7c51f33`). **`mean_rev_z`, `hurst_exponent`, AND
`recurrence_rate`** all move to activity-clock treatment instead — decided, NOT yet implemented, no
plan written yet. `recurrence_rate`'s case is real cross-domain grounding (RQA on event-indexed
RR-interval sequences, HRV literature), found only after a direct "ground this in the literature"
instruction prompted a second, more targeted pass — `recurrence_rate`/`fractal_dim` were wrongly
treated as a symmetric pair before that. Full detail: `2026-08-25-observation-vector-institutional-
hardening-spec.md` §5a (corrected twice same day). A staleness audit also found 4 `lbrnet`-side
dimensionality docs now stale relative to all of this — handed to `lbrnet`'s own session, not fixed
here (`2026-08-26-activity-clock-lbrnet-handoff.md` §10). Read `PRODUCTION_TRIAGE.md` row 1 (synced
same day) for the terse cross-project version.

## Thread C: Activity-clock tail-risk signal for the Student-t HMM (row 1, MindfulTrader-rooted) — DESIGN + PLAN DONE, handed to Claude Sonnet 5 for execution

**READ THIS FIRST if you are Claude Sonnet 5 picking this up**: the plan is written, critically
reviewed, and corrected multiple times — it is ready to execute starting at Task 1, via
`superpowers:executing-plans`. Don't re-derive the design; the spec and plan below already contain
every correction found. This repo is direct-to-master, no worktrees (standing convention) — skip
`using-git-worktrees` if that skill's default process asks for one.

- **Plan (execute this)**: `docs/superpowers/plans/2026-08-26-activity-clock-dual-kurtosis.md` — 15
  tasks, each with real code, real file:line citations, and a self-review section at the bottom.
- **Spec (background/rationale, read if a task's "why" is unclear)**: `docs/superpowers/specs/
  2026-08-26-activity-clock-tail-risk-and-decay-spec.md` — EVOLVING, dense, every section changed
  at least once on 2026-08-26.

**One-paragraph origin, for context**: pivoted off the same-week Atratus/Black-Swan research,
applied back to this system's own live HMM. Founding discovery: "Taleb kurtosis" already exists,
already gates trades five separate ways in C++, but was never in the HMM's own observation vector
at all. Design: a new `ImbalanceBarEngine` (pure, DOD-shaped) + thin `ActivityClockManager` glue
singleton build AFML-style imbalance bars from real `sc.AskVolume`/`sc.BidVolume` deltas, feeding
kurtosis into the vector on two clocks (existing time-bar + new activity-clock twin), plus an
early-trigger, non-authoritative additive input to the five existing kurtosis-consuming gates.

**Real corrections made during plan review, before any code was written — know these before
starting, so you don't rediscover them the hard way**:
1. `RingBuffer<T,Capacity>` has no `copy_last_n` — "last N" is implemented via `size()`/`operator[]`.
2. This codebase's real native-test convention is bare `g++ -std=c++17 -I include test_X.cpp -o
   /tmp/X_test && /tmp/X_test` with a hand-rolled `check(name, bool)` helper — **no GoogleTest, no
   CMake**, verified against `test_tail_risk_engine.cpp`/`test_feature_scaler.cpp`.
3. `RiskManager`/`Scoring`/`PositionManager`/`TradeDecisionEngine` all `#include "sierrachart.h"`
   directly with no vendored SDK for native compilation — they have never had native test coverage
   in this codebase's history, for that reason. The plan's answer: extract the actual gate-decision
   logic into one pure header (`include/KurtosisGateLogic.h`, Task 9 — seven functions, one file,
   natively tested) so the meaningful logic *is* tested, while the thin call-site edits (Tasks 8,
   10-14) are verified via `./build_dll.sh` + a manual checklist, matching how this codebase already
   verifies these exact classes.
4. `RobustMoments::MoorsKurtosis` takes `std::array<float,100>` **by value**, no namespace, no
   `(pointer, count)` overload — construct the fixed-size array explicitly.
5. A real redundancy was caught and removed: the original design named a standalone "new fast
   hard-gate" *and* an "early-trigger integration" as if separate — for the hard-halt case they'd be
   redundant (the standalone gate would never be called), so it was dropped before it could become
   dead code the moment it shipped.
6. The crisis-hysteresis enter/exit asymmetry (fast signal can trigger entry early, must never
   confirm exit/recovery) is enforced **at the type level** — `ShouldExitKurtosisCrisis` has no
   parameter for the fast value at all, not just a comment saying not to pass it.
7. The "divergence between the two clocks is itself informative" claim (an earlier draft's framing)
   was checked against the actual literature (Bollerslev-Tauchen-Zhou 2009, Zhang-Mykland-
   Aït-Sahalia 2005) and found **not directly supported** — downgraded to `plausible-engineering-
   choice` in the spec (§4 item 5). Don't restate it as settled.
8. Whether the *existing* five gates' calibrated thresholds should eventually be replaced by
   freshly-recalibrated activity-clock-based ones is an explicit **empirical backtesting question**
   (spec open question 13) — not decided by which option avoids recalibration effort. That reasoning
   was tried once, caught, and retracted during design — don't reintroduce it.

**Explicitly out of scope for this plan** (sequenced separately, don't fold in): the long-memory
family's activity-clock twins, `PredictionAgeUs`/`HmmStateAgeUs` decay reframing, the
`skewness_idx`/`correction_action`/`fisher_info`/`burstiness_index` follow-ups (own sequencing,
`skewness_idx` first — see spec §5c), and the skewed-Student-t-emission question (flagged,
explicitly `lbrnet`-rooted, not MindfulTrader's to decide).

**Not yet done**: literally anything in the plan's 15 tasks — this session did design, review, and
correction only, zero code touched. `writing-plans` and this review pass are both complete;
`superpowers:executing-plans` is the next skill to invoke, starting at Task 1.

**Post-implementation update (2026-08-26, after Claude Sonnet 5 executed the plan)**: a real,
confirmed gap was found and is still open — `ActivityClockManager::Update(sc)` is wired into
`SCStudies.cpp` (live) only; `EventDataCollectorStudy.cpp` (training-data collection) and
`BackTesterStudy.cpp` (backtest replay) are separate ACSIL entry points that never call it, so
`fastTalebKurtosis` is permanently stuck at the sentinel `1.23f` in both of those paths — training
data will never see a real reading, and backtesting can't exercise the gate integration at all. Not
yet decided whether to fix directly or hand back to Claude Sonnet 5 — pick this up before treating
Thread C as shipped.

**New, TOP PRIORITY item spawned by this thread, now its own row: PRODUCTION_TRIAGE.md row 14**
(`ObservationData` schema evolution policy). See the corrected account below — an earlier version
of this note claimed `fast_taleb_kurtosis` was routed onto the `Event` wire root, which turned out
to be wrong.

**2026-08-27 update — the post-implementation gap above is RESOLVED; plan is code-complete and
build-verified, but fully uncommitted.** Re-verified directly against the code, not the doc trail:
`ActivityClockManager::Instance().Init/Update(sc)` is now called from all three ACSIL entry points —
`SCStudies.cpp`, `EventDataCollectorStudy.cpp`, and `BackTesterStudy.cpp` all wire it. All 15 tasks'
target files carry the real gate integrations. Both native test suites pass (`test_imbalance_bar_
engine.cpp` 10/10, `test_kurtosis_gate_logic.cpp` 16/16) and a full `./build_dll.sh --no-clean`
succeeds cleanly. **COMMITTED 2026-08-27**: `MindfulTrader` `ff22e48`, `schema` `ea8058b` (neither
pushed — no remote configured on either repo by default). The plan file's own 96 checkboxes remain
unticked by design (see the plan's own status banner). `CLAUDE.md`'s pointer edit did not make it
into the `MindfulTrader` commit — blocked by the Documentation Sync Contract pre-commit hook
(README-AI.md/.github/copilot-instructions.md/GEMINI.md not updated in lockstep), left uncommitted
rather than bypassing the hook; harmless.

**Unrelated tangent, resolved same day**: user reported a suspected overnight crash "while making
fast_taleb_kurtosis changes to lbrnet." Checked directly — no trace of `fast_taleb_kurtosis` in
`lbrnet`'s *hand-written* Python code (training scripts, `HMM_KEEP_DIMS`, `live_agent.py`), and no
syntax errors in any modified `lbrnet` file. (**Correction below**: `lbrnet`'s *generated* schema
binding does already have it — a mechanical regen byproduct, not evidence of hand-written work
having started, so this finding still stands.) `lbrnet` does carry a large amount of uncommitted/
untracked state, but it traces to the already-documented, already-recovered 2026-08-25 crash in
`lbrnet/scratchpad.md` (about `lempel_ziv`/K=4 retrain work, unrelated to this thread) plus ordinary
accumulated in-progress work. **User's explicit decision: leave `lbrnet`'s uncommitted state alone
for a separate `lbrnet`-rooted session to sort out — don't investigate or touch it from
`MindfulTrader`.**

**CORRECTION, 2026-08-27 — a factual error in this thread's own prior notes, found while scoping
the handoff to the sibling instance.** Every note above and in `PRODUCTION_TRIAGE.md` claiming
`fast_taleb_kurtosis` was routed onto the `Event` wire root via `HMM_OBSERVATION_EXTENSIONS`, with
migration into `ObservationData` left as future work, was **wrong**. Verified directly against
`mts_schema.fbs` and all 3 repos' generated bindings: `fast_taleb_kurtosis` is already the **17th
field directly inside `struct ObservationData`** (16D→17D, 64→68 bytes) — this deviates from the
activity-clock plan's own Task 6 (which specified the `Event`-root design) but is exactly what row
14's struct-stays-and-gets-edited-in-place decision describes. `HMM_OBSERVATION_EXTENSIONS` was
never touched (`nh_nl_daily`/`daily_bias` only) and has nothing to do with this field.
`self_test_schema_contract.py`'s `OBSERVATION_FIELDS` assertion isn't violated — it's auto-derived
from the struct's real fields, so it already reflects 17. All 3 repos' generated bindings for the
17-field struct exist (`MindfulTrader`'s `mts_schema_generated.h`/`mts_schema_contract_generated.h`,
`schema`'s `regenerate_schema.sh` template, `lbrnet`'s `generated/MTS/Schema/ObservationData.py`/
`.pyi`), all uncommitted. What's genuinely still missing is `lbrnet`'s *hand-written* consumer code
(`HMM_KEEP_DIMS`, training scripts, `live_agent.py`) actually selecting/using dim 17 — separate from
the mechanical generated-binding regen. `mts_schema.fbs`'s stale "16D Fixed" comment has been fixed
to 17D. Full corrected detail: `PRODUCTION_TRIAGE.md`'s top-of-doc callout, row 1, row 9, row 14
(both `§1`/`§1.1`), and `schema/PENDING_SCHEMA_CHANGES.md`'s PSC-03 — all corrected same day.

**Row 14 (`ObservationData` schema evolution policy) DECIDED, 2026-08-27** — `ObservationData` stays
a `struct` (no struct-to-table conversion); its field set is instead **incrementally edited in
place** to match whatever the C++ side computes, a coordinated breaking edit across all 3 repos'
generated bindings each time, acceptable pre-production. `HMM_OBSERVATION_EXTENSIONS` is not being
formalized as the permanent mechanism — direct struct edits are, and `fast_taleb_kurtosis` is
already the demonstrated example, not a pending future one. Still open, a spec must settle: whether
`HMM_OBSERVATION_EXTENSIONS` is retired now that direct struct edits are sanctioned, or kept for
some other purpose, and formalizing this pattern as standing policy for future dims. See
`PRODUCTION_TRIAGE.md`'s top-of-doc callout and row 14 for the full statement.

**`skewness_idx` activity-clock work DONE, 2026-08-27 (commit `7c51f33`) — but landed as a
REPLACEMENT, not an 18th field, correcting the plan the item below now describes as stale.**
Before implementing, re-checked the "add an 18th field" premise against kurtosis's own stated
reason for going additive (spec §4 item 4): kurtosis needed dual-clock treatment specifically to
avoid disturbing 5 existing live gate consumers calibrated on the slow value. `skewness_idx` has
**zero** such gate consumers (verified via grep across `RiskManager.cpp`/`Scoring.cpp`/
`PositionManager.cpp`) — nothing to protect by adding rather than replacing. Spec §5b also already
flagged `skewness_idx` as a genuine drop candidate for `lbrnet`'s HMM (weak cross-state
discrimination, suspected staleness artifact) — replacing its stale TS3 time-bar source with the
tick-native activity-clock one directly tests that hypothesis instead of shipping a redundant twin
next to a dim about to be dropped anyway. Implemented: `ContextManager::BuildObservationVector()`
now computes dim 10 via `BowleySkewness()` over the same imbalance-bar returns buffer already
fetched for `fast_taleb_kurtosis` (one shared fetch, no duplicate engine work); the now-dead
`TripleScreen3.cpp` `obs->mutate_skewness_idx()` call was removed (`Subgraph_SkewnessIdx`/
`CalculateSkewness()` stay live for `anchors.skewnessIdx`/PredatorContext, untouched); `OBS_SKEWNESS`
removed from `ContextManager.cpp`'s `kTs3Dims` staleness-monitoring group (no longer TS3-owned).
`FeatureScaler.h`'s existing 2026-08-14 skewness calibration (winsor/shrinkage) is now flagged stale
pending a fresh audit — the underlying signal's cadence changed, the old audit doesn't transfer.

**Real, previously-undiscovered bug found and fixed in the same pass**: `FeatureScaler.h`'s four
per-dim calibration arrays (`SHRINKAGE_SCALE_MIN`, `LOGZ_WINSOR_SIGMA_OVERRIDE`,
`DIM_WINSOR_SIGMA_OVERRIDE`, `DIM_WINDOW_SIZE`) were never updated when `fast_taleb_kurtosis` landed
at `ObservationData`'s real index 13 — each still had only 16 literal entries, silently
misassigning `recurrence_rate`/`fractal_dim`/`mean_rev_z`'s calibration by one index since. Worst
case: `mean_rev_z`'s `DIM_WINDOW_SIZE` defaulted to `0`, making its rolling buffer pop immediately
after every push — permanently degenerate scaling for that dim since the kurtosis plan shipped, not
noticed until now. All four arrays fixed to correct, explicit 17-entry mappings in the same commit.
`./build_dll.sh --no-clean` succeeds cleanly.

Window-widening `recurrence_rate`/`fractal_dim`/`mean_rev_z` per `docs/superpowers/specs/
2026-08-25-observation-vector-institutional-hardening-spec.md` §5 remains **not started** — still
needs an autocorrelation-time derivation before its proposed ~150/~600-bar targets are finalized.

**SUPERSEDED IN PART, 2026-08-27, SAME DAY — READ THIS BEFORE THE "READ THIS FIRST" BLOCK BELOW IF
YOU ARE MID-TASK ON `mean_rev_z`'s TIME-BAR WINDOW DERIVATION.** A real literature-grounding pass
(Clark 1973 / Ané & Geman 2000 / AFML ch. 2, plus microstructure spurious-serial-correlation and
trading-time-for-Hurst literature) found direct institutional grounding for moving **`mean_rev_z`**
and **`hurst_exponent`** to `ActivityClockManager`-based (activity-clock) windowing instead of a
wider time-bar window — full detail now in `2026-08-25-observation-vector-institutional-hardening-
spec.md` §5a and `2026-08-26-activity-clock-tail-risk-and-decay-spec.md` §6 (both updated same day).
**`recurrence_rate`/`fractal_dim` are unaffected** — literature search found no precedent either way
for RQA/Sevcik fractal dimension under information-driven bars, so they stay on the pure
time-bar-widening path below exactly as scoped. If you've already run the autocorrelation-time
diagnostic against `mean_rev_z`'s TS3 time-bar series (`tools/window_autocorrelation_diagnostic.py`),
that measurement isn't wasted — it's a real input to what `mean_rev_z`'s *existing* time-bar gate
value's autocorrelation looks like — but the *target* for `mean_rev_z` is no longer "a wider TS3
window," it's an activity-clock twin (kurtosis's additive dual-clock pattern, not `skewness_idx`'s
replacement pattern — confirmed `mean_rev_z` has a live gate consumer, `Scoring.cpp:305`). This is a
design decision, not yet an implementation plan — `writing-plans` hasn't been invoked for it.

**URGENT SCOPE CHANGE, 2026-08-27, SAME DAY — READ THIS BEFORE PROCEEDING IF YOU ARE MID-
IMPLEMENTATION ON `recurrence_rate`'s 400-bar WINDOW.** A direct user instruction ("ground your
answer on the literature") triggered a second, more targeted literature search — it found real
cross-domain grounding to move `recurrence_rate` (RQA) to `ActivityClockManager`-based treatment,
the same as `mean_rev_z`/`hurst_exponent`, **not** the pure time-bar widening it was grouped with
below. Precedent: heart-rate-variability research routinely applies RQA directly to beat-to-beat
RR-interval sequences (event-indexed, not fixed-time) — a large, established literature, same
evidentiary standard this project's own Gang doc already accepts (Sevcik's formula choice is
grounded in an EEG paper, not a finance one). **`fractal_dim` is NOT included** — a second, targeted
search for the adjacent case (fractal/path-length methods under event-indexed sampling) still found
nothing; `recurrence_rate` and `fractal_dim` are not symmetric, treating them as a pair was this
thread's own earlier error.

**Concretely, if you were about to implement or already implemented the 400-bar target for
`recurrence_rate`**: stop — that work is now superseded. The 400-bar derivation (`§5b` of
`2026-08-25-observation-vector-institutional-hardening-spec.md`, now corrected) still stands, but
scoped to **`fractal_dim` alone**. `recurrence_rate`'s window constant needs decoupling from
`fractal_dim`'s (they share one today, `slow_window_n` in `TripleScreen2.cpp`) rather than both
being bumped to 400. `recurrence_rate` has **zero live gate consumers** (checked directly —
`ContextManager.cpp`'s only reference is a finite/in-`[0,1]`-range freshness check, not a calibrated
threshold) — so it's a `skewness_idx`-style **replacement** candidate, not an additive twin like
`mean_rev_z`/`hurst_exponent`. No implementation plan exists for this yet, same as the other two —
this is a decided direction, not yet an executable task. Full detail: `2026-08-25-observation-
vector-institutional-hardening-spec.md` §5a (corrected), §5b (corrected scope).

**HANDED TO A SIBLING CLAUDE SONNET 5 INSTANCE, 2026-08-27 — READ THIS FIRST if you are that
instance picking this up.** This is now the sole remaining item from the original two-task
observation-vector batch (`skewness_idx` above is done). Spec:
`docs/superpowers/specs/2026-08-25-observation-vector-institutional-hardening-spec.md` §4-§6 (window
audit + proposed targets), §5c of the sibling `2026-08-26-activity-clock-tail-risk-and-decay-spec.md`
is unrelated to this item — don't conflate the two specs.

**Current state, verified against the actual C++ source (spec §4)**:
| Dim | Function (file:line) | Screen | Current window | Real-world span |
|---|---|---|---|---|
| `recurrence_rate` | `CalculateRecurrenceRate(sc, lookback_n)`, `StudyHelperFunctions.cpp:3369` | TS2, 60min | `max(30, observation_window_n)` clamped `[2,40]` | 30-40 bars = 1.25-1.67 days |
| `fractal_dim` | `CalculateFractalDimension(sc, lookback_n)`, `StudyHelperFunctions.cpp:3177` | TS2, 60min | same as `recurrence_rate` | 30-40 bars = 1.25-1.67 days |
| `mean_rev_z` | `CalculateMeanReversionSpeed(sc, lookback_n)`, `StudyHelperFunctions.cpp:3238` | TS3, 15min | outer z-score `n=clamp(lookback_n,5,40)`; inner lag-1 `rho` uses `m=n-1` — **same window, not independently parameterized, this is itself part of the fix** | 10-40 bars = 2.5-10 hours |

Reference point: this HMM's own fitted mean regime tenure (production model, 2026-08-25 sign-off
run) is ~589 bars at TS3/15min, ≈6.1 real days — all three dims above run well under a day against
that reference.

**Proposed targets (spec §5) — explicitly NOT finalized, do not implement these numbers directly**:
`recurrence_rate`/`fractal_dim` → ~150 bars (≈6.25 days at 60min); `mean_rev_z`'s outer z-score →
~600 bars (≈6.25 days at 15min), with `rho`'s inner window **decoupled** from the outer z-score's
(give it its own explicit, likely-longer lookback — sharing one window for two statistically
distinct estimators was an implementation shortcut, not a deliberate choice, per the spec's own
source-reading). **Required first step**: run an autocorrelation-time diagnostic on the raw signal
itself (same technique this project already used for the HMM's calibration holdout sizing,
`2026-08-24-hmm-gate-threshold-calibration-institutional-grade-spec.md`) to confirm or adjust the
~150/~600 figures before touching any window constant — don't skip straight from "too short" to
"here's the exact number," same discipline `fast_taleb_kurtosis`'s own imbalance threshold is still
waiting on (Task 15 of the kurtosis plan, separate, unrelated dims).

**No schema impact** — confirmed by `schema/docs/ADR/2026-08-25-observation-vector-loading-
efficiency-and-dropped-hmm-fields.md` §2: a window-size change alters what *value* a field carries,
not its wire type/size. Purely a `MindfulTrader`-internal C++ change; no `../schema` or `lbrnet`
commit needed for this one (unlike `fast_taleb_kurtosis`/`skewness_idx`).

**Explicitly NOT part of this task, still correctly out of scope**: `hurst_exponent`/`fisher_info`
(spec §6) — both already run on windows ≥ the 6.1-day reference; their weak HMM discrimination is
more likely a data-quality artifact (`hurst_exponent`'s known 24.85% `|z|>=6` scale-collapse) than a
window problem — don't widen these as a first move. The 2 C++ dead-code candidates named in the same
spec (§3) are a separate item; `skewness_idx`'s `CalculateSkewness()` path is confirmed NOT dead as
of `7c51f33` (still live for `anchors.skewnessIdx`/`PredatorContext`) — only `micro_asymmetry`'s
`ofae::ComputeMicroAsymmetry()` remains a candidate.

**Verification convention, same as every other item in this thread**: no native test precedent for
these functions specifically (they live in `StudyHelperFunctions.cpp`, `#include "sierrachart.h"`
directly) — verify via `./build_dll.sh --no-clean` succeeding cleanly, plus whatever native test
exists for the autocorrelation-time diagnostic itself if one gets written as a reusable utility.

**REPLY TO SIBLING'S QUESTION, 2026-08-27 — brief on the stationary-vs-circular block-length choice
for `recurrence_rate`/`fractal_dim`'s final window, researched on request, not asserted.** Sibling
had narrowed scope correctly to `recurrence_rate`/`fractal_dim` only (`mean_rev_z` already dropped
per the correction above) and measured, via `tools/window_autocorrelation_diagnostic.py`
(Politis & White 2004, corr. Patton/Politis/White 2009), `stationary≈353.64-353.57` bars and
`circular≈404.82-404.74` bars on TS2/60min `|log-returns|` (volatility clustering), asking whether
to lock in circular (~405) as the more conservative choice or wait for input on which estimator.

**Sanity check, not a fresh claim**: the two numbers are internally consistent with theory —
Politis-White's stationary/circular tuning constants (2 vs 4/3) imply a fixed theoretical ratio
`b_circular/b_stationary = (2/(4/3))^(1/3) = 1.1447`; the measured `404.82/353.64 = 1.1447` matches
exactly. Confirms the numbers are correctly computed, not a bug or noise.

**The real answer isn't "conservative vs not" — it's which object matches our actual use case.**
`optimal_block_length` returns two numbers calibrated for two *different* bootstrap resampling
schemes, not two candidate answers to one question: stationary (`b_sb`) is the *mean* of a
geometrically-distributed random block length (for the stationary bootstrap); circular (`b_cb`) is
a *fixed*-length block (for the circular block bootstrap). We aren't bootstrapping — we're picking
one fixed rolling-window length for a point estimator (RQA/Sevcik). **Circular is the structurally
correct analogy** (a fixed number, calibrated for fixed-length blocks), not stationary (the mean of
a distribution being repurposed as a literal window size). Recommend **circular, ~405 bars**, for
this reason — not because it's bigger/safer.

**Worth stating explicitly in the implementation, not silently**: this project already has an
established, reviewed precedent for this exact tool
(`lbrnet/docs/superpowers/specs/2026-08-24-hmm-gate-threshold-calibration-institutional-grade-spec.md`
§5b), which picks **stationary**, feeding it into `StationaryBootstrap` — not a contradiction, that
spec does real bootstrap CI construction (the tool's literal designed use case); ours is a different
application (fixed window-size derivation) with a different correct answer. Say so in a comment so a
future reader doesn't read "circular here, stationary there" as inconsistency.

**On rounding**: this project's standing rule is no invented round numbers without derivation.
405→400 is a ~1% nudge, immaterial statistically, but state it as *convenience rounding from a
measured 405*, not itself derived — e.g. a code comment "400 (measured: 404.82, rounded for
config readability)."

**One number worth keeping, not just implementing silently**: 405 bars at TS2/60min ≈ 16.9 days —
notably longer than both the original ~150-bar guess (§5's own proposed target) and the ~6.1-day
regime-tenure reference that motivated proposing it. The measured decorrelation time for volatility
clustering on this instrument is real information, materially larger than either prior guess — log
it in the spec's own acceptance-gate notes, not just the final chosen number.

Sources checked: Patton, Politis & White (2009 correction), *Econometric Reviews* 28(4);
Politis & White (2004), *Journal of Business & Economic Statistics* 22(2); `arch.bootstrap.
optimal_block_length` documentation (confirms the `b_sb`/`b_cb` distinction and per-bootstrap-type
usage guidance).

**CLOSING UPDATE ON `fractal_dim`'s GATE, 2026-08-27/28 — the sibling built the migration tooling
(`tools/fractal_dim_threshold_migration.{cpp,py}`, `include/SevcikFractalDimension.h`,
`tests/cpp/test_sevcik_fractal_dimension.cpp`) and ran the real analysis.** Two findings, both now
written up in `2026-08-25-observation-vector-institutional-hardening-spec.md` §5b's new subsection:
1. **`correlation(fractal_dim@30, fractal_dim@400) = 0.0115`** — essentially zero. Decision:
   **split**, not recalibrate — `PositionManager.cpp`'s gate keeps its own short window, decoupled
   from the HMM's 400-bar value. This reverses the earlier rejection of the split option (it was
   rejected for lacking a complementary-information rationale) — the near-zero correlation is real,
   measured evidence of exactly that rationale, not "avoid recalibration effort" in disguise.
2. **`PositionManager.cpp:2144`'s `fractalDim>1.6f` branch has fired zero times across 19,327 real
   30-bar readings** (observed max ≪ 1.6) — broken on the *current* window, unrelated to any of the
   window-widening work. Naive percentile-mapping (what Task 7's methodology would mechanically
   produce) maps to the new distribution's sample maximum (`1.4393`) — a second non-functional
   threshold, not a fix. Confirmed by actually running the tool, not assumed. **SUPERSEDED,
   2026-08-28 (corrected here 2026-08-28 — this passage was left saying "needs re-derivation" after
   the spec itself was already updated to the resolution below; caught during final handoff
   consistency pass, don't trust this file over the spec on this point going forward): no
   hand-derived replacement threshold is being pursued.** `fractal_dim@30` (already wire-transmitted
   raw via `RiskGateContext.fractal_dim`, zero new schema work) is proposed instead as a raw feature
   into the soft/gate danger classifier (row 11) — a learned model finding the real relationship
   beats hand-picking a second linear cutoff, especially given the first one was wrong. GAP 11's gate
   stays inert (already non-functional, so nothing live is at risk) until that classifier ships, then
   becomes a retirement candidate. Full writeup: hardening spec §5b's two-window subsection + §9.

**This dead-gate finding is also what prompted `PRODUCTION_TRIAGE.md` row 15** (centralized
threshold-calibration machinery, `docs/superpowers/specs/2026-08-28-centralized-threshold-
calibration-machinery-spec.md`) — a broader survey found this is a known, recurring pattern across
`lbrnet`'s 11+ independently-evolved calibration scripts, not a one-off. Spec only, from this
session — implementation stays `lbrnet`-rooted, per explicit user confirmation. Registry shape
decided: single unified manifest. §3.5 of that spec is a **required, enumerated cleanup list** for
every dated/stale threshold artifact the survey found (~28 files) — not optional.

## Thread A: Pattern-detection hardening (row 13) — Phase 0 DONE, design DONE, 5 open questions block a plan

Start here: `docs/superpowers/specs/2026-08-25-pattern-detection-institutional-hardening-spec.md`
§4.0/§4.1 (root cause), then `docs/superpowers/specs/2026-08-25-pattern-literature-grounding-and-
subsumption-research.md` (literature + 36-pair audit), then `docs/superpowers/specs/2026-08-25-
pattern-recording-exhaustive-collection-selective-live-design.md` (the actual design, read this
one first if short on time — it references the other two).

**The original "sticky field" hypothesis from this morning is WRONG — refuted with code evidence.**
`raschke_tactical_trigger` IS reset every bar (`TripleScreen3.cpp:710` calls
`DetectRaschkeTacticalTrigger()` unconditionally, which explicitly returns `NONE` on no-match). The
real bug: a **detector-authority conflict** — up to 5 call sites write the same field per tick with
no consolidation, whichever runs last and passes its own gate wins by accident of source-line
order. Turtle Soup's 280x mismatch is fully root-caused (3 independently-diverging filter stacks,
not one bug). ITR Breakout's zero count is root-caused (architecturally starved by an unrelated
check running first in the same priority cascade, not dead code).

**Literature research found Momentum Pinball and ITR Breakout are literally one Raschke strategy
split across two days** (day-1 Pinball reading gates a day-2 ITR-breakout entry), not two
independent patterns — neither current implementation does this composite at all. Stochastic Pop's
real 3-ingredient definition needs an indicator (ADX) this codebase deleted in the DOD/SoA
migration; RSI Failure Swing needs a real Wilder swing-point state machine the data already
supports but the code doesn't use.

**Full 36-pair subsumption audit done** (design doc §6, or the research doc's own copy) — 5
code-certain findings (3 disjoint pattern pairs by numeric construction, 1 sequential-dependency:
ITR Fade requires a same-day prior ITR Breakout).

**Confirmed this session, changes the whole live-side framing**: `PositionManagerPatterns.cpp`
never reads `raschke_tactical_trigger` live — it keys off `prediction.actionId` (the Transformer's
own already-decided output). There is no live "pick the best fired pattern" mechanism to build;
that's the Transformer's learned job. The real live fix is narrower: the Transformer's `FeatureSpec`
(`lbrnet`-side, `schema_contract.py:174`) needs to stop reading the corrupted single scalar and read
the 9 canonical per-pattern fields instead.

**Also found**: `TradeExecutionServer::CalculateOrderPrices()` (`TradeExecutionServer.cpp:824-907`)
is a separate, confirmed-dead stub (zero call sites, hardcoded trigger value, drifted constants vs.
the real formula) — its own independent removal candidate.

**5 open questions block writing an implementation plan** (design doc §7) — sequencing (wire the 4
new fields now vs. after their logic is corrected), PSC-02 (quality float or not, per-pattern not
uniform), the Hurst-as-ADX-proxy validation (needs an actual backtest, ADX and Hurst measure
genuinely different things), the `raschke_tactical_trigger` removal audit (+ the confirmed-dead
`TradeExecutionServer` stub), empirical (not just logical) subsumption confirmation, and `lbrnet`
coordination for the FeatureSpec fix (probably bundle with the next full retrain, not a one-off).
User said: "deal with each separately when the time comes" — no rush, pick one at a time.

## Thread B: Observation-vector / vol_convexity (row 1) — 4 decisions made, none implemented

Start here: `lbrnet/docs/superpowers/specs/2026-08-25-vol-convexity-removal-spec.md` (§3/§4 have
today's updates; read the Status-line "Update, continued session" block first).

**Confirmed by reading the code directly**: `CalculateVolConvexity()` (MindfulTrader,
`StudyHelperFunctions.cpp:3322`) uses ONLY realized ES futures OHLC (10-40 bars) — the literature
construct it's named after needs option-implied vol surfaces or thousands of aggregated
observations, which this system's data feed (confirmed futures-only, no options/IV pipeline
anywhere in the repo) cannot provide. This is a data-source defect, separate from (though
compounding) the HMM's own weak-cross-state-discrimination finding.

**Four decisions made today, none implemented yet**:
1. Drop `vol_convexity` from backtest barrier-width modulation (`lbrnet/backtest/
   backtest_runner.py:319-339`, `_apply_context_barrier_modulation()`) — a consumer the original
   spec had left untouched.
2. **Retire the Taleb-diagnostic/P2.3 crash-oversampling mechanism entirely** (`compute_taleb_gate_
   metrics()`, `_legacy_taleb_metrics()`, `_apply_crash_oversampling()`) rather than reworking onto
   a raw array — this was the spec's own "Section 4 fork," now decided on evidence: a proven
   sign-convention bug in `combined_signal = max(robust_z(vol_convexity), robust_z(tail_index))`
   (signed z-score + `np.maximum()` structurally can't let a negative `tail_index` crash-signal
   win), 96.55% empirical dominance by `vol_convexity` on the real 56.9M-row dataset, and an already
   -documented real sign-off regression (`2026-08-22-hmm-crash-oversampling-axis-realignment-spec.md`).
3. **Rejected**: substituting DOF for `tail_index` in `PositionManager.cpp`'s live sizing
   (`paretoTailAlpha`). `RiskManager.cpp:1711-1728`'s "TAIL COHERENCE DIVERGENCE" check deliberately
   depends on Hill-alpha and DOF being independent, cross-validating signals — substituting one for
   the other deletes that check rather than simplifying it.
4. **Found, not decided**: `build_directional_alpha.py:608-628`'s crash-oversampling threshold
   lookup is a THIRD, separate `vol_convexity` consumer, genuinely unexamined — flagged open in the
   spec, pick this up next if continuing this thread.

**Original window-widening/dead-code content from this morning's MindfulTrader spec
(`2026-08-25-observation-vector-institutional-hardening-spec.md`) — UPDATED 2026-08-27, see
Thread C's tail for the full account, this paragraph is now stale as originally written.**
`recurrence_rate`/`fractal_dim` (30-40 bars @ 60min, propose ~150) remain window-widen candidates
pending a real autocorrelation-time derivation. `mean_rev_z` is **no longer** a window-widen
candidate — a later literature pass (2026-08-27) found direct grounding to move it to activity-clock
treatment instead (§5a of that spec). `hurst_exponent` is **no longer** "explicitly not a window
case" in the sense of being excluded from further treatment either — same literature pass grounds
an activity-clock twin for it too (its likely data-quality artifact, still real, is a separate,
compounding issue, not a reason to skip the clock-choice question). `fisher_info` is unaffected,
still a likely data-quality-artifact candidate, not researched for clock conversion. `skewness_idx`
is DONE (`7c51f33`, replacement not addition); `micro_asymmetry` remains a dead-code candidate once
`lbrnet`'s spec lands.

## Standing note for tomorrow (or any session)

Corrected today, recorded in memory: **this system has no production deployment yet** — don't gate
proposed changes to "live-looking" risk-consumer code behind mandatory ablation studies as if real
capital were at stake. Still name real technical risks when found (several were, today, and held up)
— just don't let "this touches RiskManager" alone be a reason to slow down.

## 2026-08-24 (later same day) — Two new live classifiers designed (soft gate classifier + meta-labeler) — spec written, queued

Grew out of the same production-triage session as the entry below. Full design:
`docs/superpowers/specs/2026-08-24-two-classifier-cpp-deployment-spec.md` (this repo) +
`lbrnet/docs/superpowers/specs/2026-08-24-two-classifier-risk-sizing-architecture-spec.md`
(Python-side training design, sibling repo). One-line summary of the confirmed architecture:

`Hard gate -> soft/gate classifier (danger veto, deliberately independent of HMM) -> Transformer
(side) + Predator Fusion (pattern) -> meta-labeler (size, genuine AFML meta-labeling, consumes
HMM-derived scalars + existing sizing multipliers + gate classifier's score + pattern output) ->
execution`. Both new classifiers train in Python, deploy to C++ for tick-reactivity, same
`PredictionAgeUs()`-style decay treatment as the entry below (extended to `HmmStateAgeUs()` too,
which already has the continuous age-getter, unlike the Transformer side).

**Real, not-yet-closed risk this creates**: now 5 Python-trained/C++-deployed components need
golden-fixture parity tests (HMM, Transformer, Predator Fusion Option B, + these 2 new ones), all
resting on the still-open `PRODUCTION_TRIAGE.md` row 5/7 gap (no C++/Python-twin agreement test
exists at all yet) — that gap's priority just went up, not down. Also: an explicit
"which upstream change requires which downstream retrain" dependency map doesn't exist yet across
HMM/Transformer/Predator-Fusion/gate-classifier/meta-labeler and should exist before this ships.

**Next action when this resumes**: read both specs in full, in particular the still-open items —
soft classifier's exact feature list + label definition (not yet decided), meta-labeler's
sample-size check against actual Predator Fusion pattern-firing frequency (not yet run), and the
suggested explicit data-flow diagram (not yet drawn) — before writing any implementation code.

**Concrete candidate for the still-open feature list, added 2026-08-28**: `RiskGateContext.
fractal_dim` (`../schema/mts_schema.fbs:442`) — user asked directly whether the *fast* (short-window,
`lrc.fractalDim`, the same value `PositionManager.cpp`'s gate reads) `fractal_dim` variant could feed
the soft classifier. Yes, and it's already mechanically available: `RiskGateContext` is a wholly
separate wire table from `ObservationData` (the HMM's input), populated from raw `LocalRiskContext`
at `ContextManager::EmitTrainingContext()`, already logged in `.context`/`.alpha` — no new C++/schema
work needed. Satisfies "deliberately different from the HMM" on two grounds: structural (separate
table) and empirical (`correlation(fractal_dim@30, fractal_dim@400) = 0.0115`, measured during the
gate-threshold migration work above). **Dependency resolved, 2026-08-28 (`72ab967`)**: row 1's
`fractal_dim`/`recurrence_rate` split has shipped — the two are no longer the same computation.
`RiskGateContext` also exposes `hurst_exponent`/`mean_rev_z`/`taleb_kurtosis`/
`taleb_skewness` raw values, same consideration not yet evaluated for those. Full detail:
`PRODUCTION_TRIAGE.md` row 11.

## 2026-08-24 — Predator Fusion does not yet consume the Transformer signal at all — spec written, queued for next Predator Fusion session

Found and confirmed during a cross-project production-triage session (`/home/rcruz/devel/VSCode/PRODUCTION_TRIAGE.md`,
the parent-level doc coordinating `lbrnet`/`MindfulTrader`/`MTS`/`schema`). The user's mental
model was that Predator Fusion should act on the Transformer's last signal with staleness decay
applied (Python predicts on indicator-delta/16D-observation change, not every tick; C++ runs
every tick). **Verified by reading the actual code, not assumed**: this integration doesn't
exist yet. `TurtleSoupFusion.h`'s live entry-fusion functions
(`EvaluateTurtleSoupOptionA`/`OptionB`) take no Transformer-signal input at all — pure
price-geometry pattern detectors. The freshness plumbing exists (`InferenceManager::
IsPredictionFresh()`, mirroring the already-working `IsHmmStateStale()` pattern) but is a binary
check, never called by Predator Fusion, and there's no continuous age getter (`PredictionAgeUs()`)
to decay against in the first place.

**Not a regression** — Predator Fusion Option A was reasonably built first as a self-contained,
independently-testable price-geometry detector. The Transformer-signal fusion (with decay) is
genuinely new, not-yet-started work.

**Full spec written and ready to pick up**:
`docs/superpowers/specs/2026-08-24-predator-fusion-transformer-signal-decay-spec.md`. Covers:
add `PredictionAgeUs()` (mirrors `HmmStateAgeUs()` exactly, mechanical); design a continuous
decay function applied to `modelConfidence` (user's explicit preference over a hard freshness
gate); wire the decayed signal into the entry-fusion functions (currently no parameter for it
at all). **The decay function's time constant is explicitly NOT decided** — it needs empirical
derivation from this system's own inter-prediction-arrival-interval distribution (pull from
historical logs when this is picked up), not a borrowed literature value (Grinold-Kahn-style
alpha-decay half-lives are months-scale, the wrong order of magnitude for this problem) and not
an invented round number. See the spec's Section 3 for the full open-questions list.

**Next action when this project resumes**: read the spec, pull the real inter-prediction-interval
data first, then implement in the order given (age getter → decay function → fusion wiring).

## 2026-08-23 — `InformationEngine::GetLempelZivComplexity()` confirmed ceiling-saturated on
## real current data; blocking a Student-t HMM feature-selection decision in the lbrnet project

A parallel `lbrnet` session (16D HMM observation-vector dimensionality investigation) directly
measured `lempel_ziv`'s value distribution against the CURRENT, live `.context.parquet` data
(2,000,000-row random sample from the full 56,963,578-row dataset): **55.80% of all samples sit
at exactly one ceiling value**, only **11 distinct values** total across the whole sample. Median
equals the max. This independently confirms (with fresh, current data, not just re-citing the
prior finding) a limitation this repo's own literature-grounding pass already flagged but never
acted on: `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md:44` — median-split
(2-symbol) binarization over a short `WINDOW_SIZE_LZ=64` window is known in the LZ-complexity
literature to bias toward looking "maximally complex" for most real sequences, because a 2-symbol
alphabet over 64 samples gives the LZ76 parse very little room to distinguish genuinely different
return dynamics. The LZ76 parsing algorithm itself (Kaspar & Schuster 1987) is implemented
correctly — this is a quantization/resolution limitation upstream of it, not a parsing bug.

**Why this blocks lbrnet right now**: lbrnet's Student-t HMM feature-selection work
(`knowledge/global/training/hmm_feature_selection.md` in that repo) needs to decide whether
`lempel_ziv` stays in or is dropped from the model's 16D input vector. A real regression test
tied to actual historical alignment behavior showed `lempel_ziv`'s STATE-level (cross-state mean)
signal is still load-bearing for `HMMStateEnum.GAUSSIAN_FRAGILE` detection, even though its raw
OBSERVATION-level tail-relevance measured at ~0. That tension traces directly back to this
window/quantization limitation: the metric still carries some real signal at the aggregate level,
but is degenerate for the majority of individual observations, which is exactly what a coarse
2-symbol short-window LZ estimate would produce.

**Not fixing this from the lbrnet side** — this is C++ producer logic
(`include/InformationEngine.h` `GetLempelZivComplexity()`), out of lbrnet's scope per its own
Python/C++ project boundary. The already-scoped fix in this repo's own prior grounding pass is
**multi-symbol (tertile+) quantization**, not a window-length change — recorded here as the
concrete next step whenever this repo picks this up, not yet started.

---

## 2026-08-16 (evening) — Predator infrastructure + Turtle Soup Option A implemented and committed
## (was SPEC-only as of the previous entry below). Read this FIRST.

Executed `docs/superpowers/plans/2026-08-16-predator-infrastructure-and-turtle-soup.md` inline,
all 7 tasks, in this same session. Full clean `./build_dll.sh` green; all 4 native test suites
`ALL PASS`. Commits (in order): `PredatorContext` (Task 1), `FusionKey`/`PredatorFusion`
applicability-mask dispatch (Task 2), `FuseTauStar` (Task 3), `EvaluateTurtleSoupOptionA` +
live wiring (Task 4), `ClassifierParams` scaffold (Task 5), `EvaluateTurtleSoupOptionB` scaffold
(Task 6) — plus several small naming-cleanup commits along the way (see git log).

**What's actually live now**: Turtle Soup evaluates the current, still-forming bar every tick
(the `lastProcessedBarTS`/once-per-closed-bar gate is gone entirely), gated behind the new
applicability-mask dispatch (`ComputeApplicabilityMask`) so the entry-side fusion structurally
cannot fire while in a position. `FuseTauStar` and `EvaluateTurtleSoupOptionB` are built, unit-
tested, and confirmed via `grep` to have zero call sites in `src/` — deliberately not wired live
per the plan's own scope boundary (τ* gated on backlog Unit 3's `ExitReason_TRAP` schema work;
Option B gated on a future lbrnet-rooted training run).

**Real bugs caught and fixed during execution, not just spec-following**:
1. A genuine plan bug caught in critical review *before* any code was written: Task 4's original
   draft would have reassigned the shared `signalBarIndex` variable (`sc.Index - 1` → `sc.Index`),
   silently breaking the unrelated "CRITICAL FIX — MOVED FROM TURTLE SOUP" normalized-anchors block
   further down in the same function, which reuses that same variable independently of Turtle
   Soup's own gating. Fixed by introducing a separate `currentBarIndex` variable instead, leaving
   `signalBarIndex` completely untouched.
2. Task 1 discovered `PredatorContext.h` needed `LocalRiskContext` (from `ContextManager.h`) and
   `HMMStateEnum` (from `Indicator.h`) — both files transitively pull in `sierrachart.h`, which
   would have broken native testability. Fixed by extracting both into their own ACSIL-independent
   headers (`LocalRiskContext.h`, `rc_enums.h` — the latter deliberately named to mirror lbrnet's
   `core/rc_enums.py`, not renamed to PascalCase despite the rest of this session's new files
   following that convention), matching the existing `MacdEnum`/`IndicatorComputations.h`
   extraction precedent. Logged the pre-existing scattered `MacdEnum`/`KangarooTailEnum`/etc.
   consolidation into `rc_enums.h` as a new Unit 10 in the convergence backlog (not done now).
3. Task 3's own test had a real math bug: the "stop much closer than target" scenario computed
   tau*=0.889, not "low" as its own comment claimed — verified against Elkan's actual cost-
   minimization derivation directly. `FuseTauStar`'s implementation was correct throughout; only
   the test's chosen numbers were backwards. Fixed in both the test and the plan doc.
4. Task 4's own test had a similar bug: the bearish Turtle-Soup-Option-A scenario's `closeSoFar`
   put `closePosition` on the wrong side of the 0.45 threshold the bearish branch requires (0.73
   instead of ≤0.45) — the scenario would never have triggered. Fixed the test data.

**Explicitly NOT done, by design**: the lbrnet-rooted handoff (ECTS prefix-training, Python-twin
Predator extension, empirical Option A vs. Option B comparison) has not been written yet — that's
the next pending task, per the user's own instruction ("create a clear, actionable handoff...
for a separate lbrnet-rooted session").

---

## 2026-08-16 (afternoon/evening) — Long brainstorming arc: converged the execution/risk system
## toward "The Predator Decision Contract" + its concrete C++ infrastructure spec. Nothing implemented
## yet — everything below is SPEC/DESIGN, committed to git, ready for `writing-plans` when resumed.
## Read this FIRST — it supersedes nothing above, it continues from the morning's Task 1 closure.

**User is taking a long break and explicitly asked for continuity insurance against a power outage.**
All work below is committed locally (9 commits, `6ba7b3e`..`36d5788`) — **NOT yet pushed to origin**;
push is a pending decision, ask before doing it (see end of this entry).

### The arc, in order

1. **Governance spec** (`docs/superpowers/specs/2026-08-16-execution-risk-coevolution-governance-spec.md`):
   established the C++/Python twin-first promotion ladder (Python twin → SC-replay backtester → paper
   → live) and the mechanical parity-contract test as the real co-evolution enforcement (narrative
   scratchpad notes are a complement, not a substitute — this was an explicit user decision after two
   documentation-drift near-misses earlier the same day).
2. **Convergence backlog** (`docs/superpowers/specs/2026-08-16-elder-raschke-triple-barrier-convergence-backlog.md`):
   9 units cataloged from a literature-grounded audit of the 16D risk-gate system, Triple Screen, and
   the Chandelier→Triple-Barrier migration (three research-agent reports, not reproduced here — read
   the spec). **Unit 2 (stale-comment cleanup) DONE. Unit 6 (ADR corpus reconciliation) DONE. Unit 3
   (`ExitReason_TRAP` schema) explicitly DEFERRED** (rationale: the Python twin doesn't need it to
   measure TRAP attribution; `MindfulTrader.dll` is one shared binary, so deploying it would interrupt
   whatever `EventDataCollectorStudy` collection is running). **Units 1, 4, 5, 7, 8, 9 — not started.**
3. **PCH/include-hygiene spec** (`docs/superpowers/specs/2026-08-16-pch-and-include-hygiene-spec.md`):
   discovered mid-session that `MindfulTrader_Precompiled.h` bundles 20 actively-developed project
   headers into the PCH, defeating its purpose (verified empirically: touching `PositionManager.h`
   forces a 70s/35-file full rebuild vs. 21s/2-file for an unrelated header). Refined design: a new,
   narrowly-scoped `include/pch.h` becomes the sole precompile target, `MindfulTrader_Precompiled.h` is
   retired entirely (not kept in slimmed form). **Deliberately deferred** ("leave this beast for a
   later time") — spec is complete and ready, nothing implemented.
4. **The "historian vs. sniper" investigation** (folded into the Predator Decision Contract spec, not
   its own doc): direct code verification found TS3's primary trigger patterns are NOT uniformly
   bar-close-gated as first assumed — Kangaroo Tail, Momentum Pinball, and Elder Breakout are genuinely
   tick-reactive (`TripleScreen3.cpp:837-847`, `:918-924`, `:1063-1069`, all read the current forming
   bar, no gate); Turtle Soup is the one deliberate exception (`:1215-1241`, explicit
   "Institutional timing contract: Process ONCE per closed bar"). **Two of the model's own
   over-generalizations were caught and corrected mid-investigation** by the user's skepticism — worth
   remembering as a pattern: don't trust a single example (Turtle Soup) or a research agent's blanket
   claim without checking the other call sites directly.
5. **Found: lbrnet already has a "Predator" concept** — `lbrnet/docs/architecture/PHASE_3_MULTISCALE_PREDATOR_BLUEPRINT.md`
   (Gemini's blueprint, not yet implemented — a dual-attention Transformer fusing 50 sparse macro
   bar-close frames with 150 sub-second micro order-flow updates via cross-attention). Real precursor
   groundwork already exists: `lbrnet/lbrnet/data/multiscale_bars.py` (Ripple/Wave/Tide bar-cache),
   with `bar_type` IDs 1/2/3 already reserved "so a future C++/wire version reuses the same numbering."
6. **The Predator Decision Contract** (`docs/superpowers/specs/2026-08-16-predator-decision-contract-execution-risk-framework.md`):
   the C++ execution/risk analog of lbrnet's Predator blueprint — a decision discipline, not a neural
   architecture. **Five required elements**: (1) explicit macro input, (2) explicit micro input,
   (3) explicit fusion rule (regime-conditioned threshold, template = TRAP's τ*), (4) twin-validation
   before promotion, (5) **subordinate to safety, no exception** — generalizes `CLAUDE.md`'s existing
   "native governs, model may lead but never suppress" TRAP philosophy to every current/future
   Predator-grade decision, verified against the real call order (`PositionManager.cpp:241-268`'s
   `m_exitSubmittedThisTick` guard). Real, found violation: **Elder Breakout's directional-fusion bonus
   is broken by construction** — `screen1Bullish`/`screen1Bearish` are set to the identical condition
   (`TripleScreen3.cpp:1162-1173`), proving "tick-reactive" ≠ "Predator-grade." First-wave work
   (Elder Breakout fix, Kangaroo Tail/Momentum Pinball audit, Turtle Soup Predator-ization, Units 4/5
   reframed) is named but **none of it is implemented yet**.
7. **PredatorContext/PredatorFusion infrastructure spec** (`docs/superpowers/specs/2026-08-16-predator-context-fusion-infrastructure-spec.md`):
   the concrete C++ mechanism, requested explicitly before any individual pattern gets fixed. A unified
   `PredatorContext` struct (composes existing `LocalRiskContext` + HMM state, DOD-consistent, zero new
   computation), a free-function-per-decision fusion interface (no virtual dispatch), and a
   broad-phase/narrow-phase applicability-bitmask dispatch that **reuses `IndicatorManager`'s existing
   dirty-mask idiom** — both a real perf win under `AutoLoop=1` and the *structural* enforcement of
   contract element 5 (an entry-fusion bit provably cannot be set while `inPosition` is true). τ*
   migrates onto the new interface as a byte-identical, regression-tested reference implementation —
   this is infrastructure, not a new decision, so it's validated by native unit tests (mechanism), not
   the twin (policy) — an explicit, sourced design decision (Hydra OS mechanism/policy separation,
   Cohn's testing pyramid, Mike Acton's DOD testing practice, and this project's own prior use of the
   identical split for `test_feature_scaler.cpp`).

### What's next, in the order it was queued (nothing started yet)

1. Whoever resumes: **read the three specs in commit order** (governance → Predator Decision Contract
   → PredatorContext/PredatorFusion infrastructure) before touching anything — the infrastructure spec
   assumes the contract spec's five elements as given.
2. The PredatorContext/PredatorFusion infrastructure spec is **implementation-ready but
   `writing-plans` was never invoked** — next concrete step when resumed, if the user wants to move
   from spec to code.
3. **First-wave Predator-contract work, folded into one unified effort (2026-08-16, post-break
   decision — Turtle Soup's Predator-ization is not a separate thread, it's part of this same batch,
   matching how the Predator Decision Contract spec's own "First-Wave Concrete Work" section already
   listed it):**
   - Elder Breakout directional-fusion fix — **DONE 2026-08-16 (`4b0753a`), resolved as a deletion,
     not a redesign.** Provenance check confirmed the pattern is legitimate (real Elder Keltner-Channel
     + Raschke strength grading, a continuation breakout, not the fake/failed-breakout fade the user
     actually recalled — that's a separate, currently-unimplemented idea, deliberately not conflated
     here). Tracing consumers of the broken `screenAligned` logic found `ChannelSqueeze()`/
     `ImpulseAligned()`/`ScreenAligned()` had **zero call sites anywhere** — fully dead code, never
     reaching the live entry decision (which already had correct Hurst+slope fusion elsewhere in the
     same function, `TripleScreen3.cpp:1078-1104`). Removed outright (both the `TripleScreen3.cpp`
     computation block and the three unused fields/accessors in `include/Indicator.h`) per the new
     standing **dead-code-removal mandate** (`feedback_dead_code_removal_mandate` memory — this DLL
     has never shipped to production, no backward-compat hacks, delete on sight). `build_dll.sh` clean.
   - Kangaroo Tail / Momentum Pinball audit against the 5-element contract — **DONE 2026-08-16, both
     pass, no fix needed** (Kangaroo Tail: `atSupportLevel`/`atResistanceLevel` correctly
     direction-discriminating; Momentum Pinball: `slopeAligned` + Hurst-conditioned continuous
     multiplier, genuinely regime-aware fusion).
   - **Turtle Soup Predator-ization — SPEC'D 2026-08-16
     (`docs/superpowers/specs/2026-08-16-turtle-soup-predator-ization-spec.md`), not yet implemented.**
     Bridge plan: ship a tick-reactive geometric heuristic now (Option A, reusing Kangaroo Tail's
     proven approach against the 20-bar extreme instead of a single bar); Option B (a classifier) is a
     parallel, non-blocking track that later swaps in at a single, standardized micro-signal seam
     **only if it empirically beats Option A** — a genuinely open question (small-sample,
     single-pattern), not assumed either way. Design was pressure-tested via `lbrnet/logs/rc_gemini.log`
     `CLAUDE_BRIEF_103` before being written down.
   - **Option B's general mechanics split into their own spec**
     (`docs/superpowers/specs/2026-08-16-ects-prefix-training-infrastructure-spec.md`) — the offline
     prefix-dataset construction, Dachraoui/Elkan/Shiryaev-Wald stopping-rule theory, and
     Predator-equipped Python twin extension are a *general* capability, not Turtle-Soup-specific,
     mirroring `PredatorContext`/`PredatorFusion`'s own infrastructure-vs-consumer split. Second real
     consumer already identified: TRAP's own anticipatory τ* layer, which `CLAUDE.md` already names
     "ECTS-style intra-bar-prefix training" as the prerequisite for (currently deferred for exactly
     that reason). Deployment guidance settled: hand-crafted C++ port (not `m2cgen` auto-generated
     code) once/if any consumer's model proves out, golden-vector regression-tested against the Python
     model — no live Python round-trip inference, ever, for any consumer.
   - **This closes out the entire first-wave Predator batch** — Elder Breakout, Kangaroo
     Tail/Momentum Pinball audit, and Turtle Soup's design are all done. **Only Option A's actual
     implementation remains before this specific batch is fully shipped** (Option B is a separate,
     later, non-blocking track per its own spec).
4. Backlog Units 1 (parity-contract test infra), 4 (`REGIME_INVALIDATION` wiring), 5
   (profit-protection measurement via the Python twin — was queued before the sniper/historian/Predator
   detour pulled focus away), 7 (Triple Screen fidelity), 8 (doc sync), 9 (flagged research) — none
   started.
5. PCH spec — deliberately deferred, no timeline attached.

### Housekeeping

- **Push to origin**: not yet done, explicitly pending a decision — ask before pushing, per standing
  git-safety convention, even though the user's stated concern this time (power-outage data loss) is
  exactly the risk pushing would mitigate that local commits alone don't.
- The 4 pre-existing uncommitted files from before this session started (`.claude/settings.local.json`,
  `data/NH_NL.csv`, `data/daily_high_low.csv`, `docs/ADR/amihud_gate_percentile_spec.md`) are still
  untouched, still unexplained — not this session's work, don't assume what they are.
- The `EventDataCollectorStudy` live collection from the morning (`event_data_20260815_230249.*`) was
  last confirmed still actively growing — status not re-checked at session pause; check freshness
  before trusting it for any future twin-measurement work (Unit 5).

## 2026-08-16 (morning) — Task 1 (observation-vector production validation) CLOSED: dim3's
## non-reconciliation root-caused as a stale pre-fix baseline, not a defect. Read this FIRST.

A fresh live collection had been running since 2026-08-15 23:02:49
(`/mnt/c/SierraChart2/Data/event_data_20260815_230249.context`, DLL rebuilt 22:55 same day, includes
every fix through `a8a2f34`). Used it to close the two open items in
`docs/superpowers/plans/2026-08-14-observation-vector-full-institutional-coverage.md` Task 1:

- **Step 4 (dim12 zero-rate spot-check) — DONE.** 0.0000% zero_ratio on a 500K-row tail sample —
  matches `CLAUDE_BRIEF_095`'s "essentially flat 0.0%" finding, closed clean.
- **Step 5 (dim3 non-reconciliation, the real work) — root-caused via `superpowers:systematic-debugging`.**
  The `CLAUDE_BRIEF_095` baseline (10.078%) came from `event_data_20260813_191757.context`, a run
  started *before* `ee86c77` ("generalize dim3's scale-collapse shrinkage fix",
  2026-08-14T14:01:32) was committed. The 1.468% figure (Task 1 Step 3) came from a run started
  *after* that fix. Tonight's fresh run (latest build) shows 0.0%. All three numbers
  (10.078% → 1.468% → 0.0%) track the fix's deployment timeline monotonically, across builds that all
  replay from the same 2023-08-16 historical reset point (so it isn't a calendar-window artifact
  either) — this is the signature of a working fix measured against a stale pre-fix baseline, not an
  unresolved estimator problem. One honest caveat: no surviving DLL binary from the Aug 13/14 window
  to independently byte-confirm which commit each run's DLL was built from — this rests on commit
  timestamps plus the repo's established rebuild-before-collection convention.

**Task 1 is now DONE.** Per the plan's own tally, all 16 dims are audited/exempt and Task 1's
production validation is closed — only Task 5 (doc sync) and Task 6 (pointers) remain on that plan.

Ad-hoc analysis scripts used (not committed, not part of the repo): `/tmp/task1_spotcheck.py`
(dim rail-hit-rate + zero-ratio via `lbrnet`'s `read_context_observations`, tail-sampled),
`/tmp/check_ts.py` (embedded timestamp inspection, confirmed both the fresh and the 2023-replay file
share the identical replay start epoch `1693822631729000` = 2023-09-04). Used `mamba run -n mts`
per this project's standing Python-env rule, not `conda run` (corrected mid-session after using
`conda run` for the first preflight call).

## 2026-08-15 (evening) — `risk_gate_context` co-evolution spec: Units B and C SHIPPED,
## Unit A explicitly held. Read this FIRST — it supersedes the "NEW SPEC ready" section below.

Split the spec below into three separate plans (per subsystem, per `writing-plans` skill
guidance) and executed two of them inline this session, direct-to-master, all six commits green:

- **Unit B — DONE** (plan doc removed 2026-09-04, fully duplicated by its own output; its output doc
  `docs/ADR/gate_stack_stationarity_audit_findings.md` was itself ALSO removed 2026-09-04, fully
  merged into `docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md`
  §3/§3a; original commits `f9f676c`/`1d4cc7a`): audited all
  8 fixed-threshold gates in `RiskManager::EvaluateHardGates()`/
  `ExecutionGate::EvaluateEmpiricalRegimeGates()`, 7 confirmed stationary (5 by direct citation to
  the existing `amihud_gate_percentile_spec.md` verdicts, 1 new finding for the Pareto-top-state-ratio
  gate, which is actually a `1/Hill-α` proxy despite its name — naming debt noted at the time, fixed
  2026-09-04 (renamed to `hillTailIndexProxy`)).
  `taleb_signal_sigma_threshold` compiled default fixed `1.8382` -> `1.8401` in `RiskManager.cpp:74`
  to match the live JSON exactly. `build_dll.sh` green.
- **Unit C — DONE** (`docs/superpowers/plans/2026-08-15-risk-gate-shared-config-unit-c.md`, commits
  `3ac4419`/`a8a2f34`/`7454e17`): new git-tracked `config/` folder (`execution_params.json`
  `1.1.0`, `hmm_regime_risk_policy.json` converted from date-scheme to `1.0.0` semver, both with
  `_owner`/`_generated_by` sectioned provenance). `FeatureScaler.h`'s four winsorization constants
  (`STATE_WINSOR_SIGMA`/`DIM_WINSOR_SIGMA_OVERRIDE`/`LOGZ_WINSOR_SIGMA_OVERRIDE`/
  `SHRINKAGE_SCALE_MIN`) converted from `static constexpr` to `static` (loadable), loaded exactly
  once via `FeatureScaler::LoadConfig()` called from `ContextManager`'s constructor (mirrors
  `RiskManager.cpp`'s `GetHMMRiskPolicy()` lazy-load-once idiom — no per-tick cost, verified by
  reading the actual hot-path call pattern before designing this). New
  `scripts/promote_config_to_live.py` pushes `config/*.json` -> `/mnt/c/Trading/config/*.json`
  (atomic write + timestamped backup). Native `tests/cpp/test_feature_scaler.cpp` (now needs
  `-I /mnt/c/Users/rcruz/vcpkg/installed/x64-windows/include` and `src/Logger.cpp` linked in — see
  the file's own updated header comment) and `tests/python/test_promote_config_to_live.py` both
  green; `build_dll.sh` green after both code-touching tasks.
- **Unit A — explicitly held, not started, no plan written.** Its first step needs a live/replay
  Sierra Chart trace (instrument `CheckAndTriggerHMM`'s `EmitTrainingContext` call site and
  `EventDataCollectorStudy.cpp:788`'s direct `LogSynchronizedEvent` call site, confirm which
  actually causes the 67.9%/32.1% `risk_gate_context` population split) — per this repo's standing
  rule, that needs fresh confirmation before deploying/running Sierra Chart again, not assumed from
  a general "proceed." Pick this up whenever ready to run that trace.

**Not pushed to `origin`** — all six commits are local on `master` only; push is a separate,
not-yet-made decision.

**Cross-repo follow-ups flagged, not actioned here:** lbrnet's own `taleb_signal_sigma_threshold`
values (`backtest_runner.py`'s `9.636797` hardcoded fallback, `lbrnet/models/HMMEmpiricalGateThresholds.json`'s
`6.67559116507085`, dated 2026-07-26) are still stale relative to the `1.8401` this session
established as authoritative — needs an lbrnet-rooted session. The Unit C spec's own design point 5
(the lbrnet-side sync script for `empirical_gate_thresholds`) was also explicitly out of scope here
for the same reason.

## 2026-08-15 (later) — NEW SPEC ready for implementation: `risk_gate_context` C++ co-evolution.
## Read this FIRST if picking up MindfulTrader work — it supersedes the "Config drift" bullet below.

An lbrnet-rooted session found a real, structural gap while auditing `risk_gate_context` (the raw
gate-input telemetry `ContextManager.cpp` ships for Python parity): it's only present on **67.9%**
of `MarketObservation` records (verified against 2,000,000 samples), not a legacy-data artifact —
two independent write paths exist (`ContextManager::EmitTrainingContext()` populates it;
`EventDataCollectorStudy.cpp:788`'s direct `LogSynchronizedEvent()` call doesn't). Separately, found
`taleb_signal_sigma_threshold` drifted to three different values across the codebase with no sync
mechanism, and confirmed `FeatureScaler.h`'s winsorization bounds are hardcoded C++ constants with
no Python-readable equivalent.

**Full spec, ready to implement**: `docs/superpowers/specs/2026-08-15-risk-gate-context-cpp-coevolution.md`
— three units: (A) close the population gap (root-cause hypothesis written down, explicitly
flagged as unconfirmed — trace it first, don't build against it blindly), (B) audit the rest of the
live gate stack for the same drifted-threshold pattern `amihud_gate_percentile_spec.md` already
fixed once for Amihud, (C) a new git-tracked `config/` folder + shared, versioned, sectioned-ownership
calibration config (closes the drift problem structurally, not just once). Companion lbrnet spec
(`../lbrnet/docs/superpowers/specs/2026-08-15-risk-gate-context-backtester-fidelity.md`) already
implements the Python side against *today's* 67.9%-populated reality (pass-through on absence,
explicitly a temporary shim) — Unit A shipping here is what makes that shim removable.
**This spec is not yet implemented** — start with Unit A's trace step.

## 2026-08-15 — Overnight replay crashed in a real power outage; ran the deferred Task 1 Step 3/4
## methodology against the collected data from an lbrnet-rooted session. Read this before anything below.

**The crash, forensically**: a genuine power outage killed the whole machine (not just this
process) while the overnight replay below was still running. `MindfulTrader.log` stops abruptly
at `2026-08-15 13:35:28`, no clean shutdown line. `edc_breadcrumb.bin`
(`C:\SierraChart2\Data\edc_breadcrumb.bin`, the crash-diagnostic file `EventDataCollectorStudy.cpp`
already writes every cycle) reads step **30** (`AddToTrainingEventFB done`) — i.e. the crash hit
*before* `LogSynchronizedEvent` (steps 50/60, the real disk write) was ever entered for that last
cycle. **No torn/partial write from the crash itself** — confirmed by an exhaustive byte-level scan
of the `.alpha` file (see below), which ends cleanly at its true EOF.

**Separate, still-unexplained finding**: the `.context`/`.alpha` output files had already stopped
growing at **19:31/19:33 on 2026-08-14 — roughly 18 hours before the crash**, while the log kept
showing the collector actively cycling (`TS1 MacroObs` write/commit counters climbing from ~2.8M to
~15.6M, `LockC` transitions from #10450 to #67450) right up to the crash. Confirmed via three
independent checks (WSL mount mtime, native Windows `Get-Item` bypassing WSL entirely, and a full
byte-level record scan of `.alpha` — 8,798,410 real `TrainingEvent` records + 5,093 harmless
trailing zero-length padding records, ending exactly at the file's true size, no truncation) that
no new record was ever appended after that point. **Leading, unconfirmed hypothesis**:
`EventDataCollectorStudy.cpp:788`'s `if (eventT->observation && eventT->asymmetry_context) {
...write... } else { WriteBreadcrumb(45); /* skipped */ }` branch — if `observation`/
`asymmetry_context` went null starting around that time, every subsequent cycle would silently
skip the actual write (incrementing `EDC_NULL_OBS_SKIP_COUNT_ID` only, no log line) while the rest
of the loop kept running and logging normally. The one thing that would confirm this
(`EDC DIAG: ... nullObsSkips=...`) only fires on a graceful disarm, which the power-loss crash
bypassed — so this is not yet confirmed, only the best-fit hypothesis. **Confirming it requires a
live Sierra Chart session** (restart + let it disarm once, or add a log line at that skip site) —
do not restart the collector without asking the user first, per this repo's own standing rule that
deploying to Sierra Chart always needs fresh confirmation, not standing permission.

**Data integrity verdict**: the collected data itself (through 19:31/33 Aug 14) is intact, not
corrupted, not truncated. Copied to `../lbrnet/data/raw/event_data_20260814_163135.{context,alpha}`
and re-verified there too.

**Ran the actual Task 1 Step 3/4 methodology** (below, previously only a general structural scan
had been done) against the Sept 1 – Oct 20, 2023 window of this same file (2,108,061 aligned
pairs — the exact reference window `CLAUDE_BRIEF_095`'s production rail-hit-rate numbers came
from):

- **`dim6` (hurst_exponent) tail — CONFIRMED, closes the last open item in the 16D audit.** 701 of
  2,108,061 records show `dim6` pinned exactly at the 345.0 wide-bound saturation point
  (`log1p(345)=5.8464` in stored space). This is the direct on-real-data evidence the scratchpad
  below was waiting for — the tail bound genuinely engages, isn't too tight, isn't a phantom.
- **`dim1` (burstiness_index) rail-hit rate — CONFIRMED clean, matches Step 3's target.** `|z|>=6`
  rate = 7.687% vs. the documented production baseline of 7.848% (0.16pp, well inside the ~1pp
  tolerance). Zero hits at the wide bound (45.0), max raw-z-equivalent 42.45 — matches the native
  fixture's `0.0000%` result exactly.
- **`dim3` (correction_action) rail-hit rate — NOT RECONCILED, a real open discrepancy.** `|z|>=6`
  rate = **1.468%** vs. the documented production baseline of **10.078%** — an 8.6pp gap, nowhere
  near the ~1pp tolerance. Wide-bound behavior is fine (zero hits, max raw-z 31.35 well under
  4587), so this isn't a saturation/pinning problem — specifically far fewer values cross the
  ordinary 6-sigma threshold than the documented baseline says should happen. Per this same
  coverage plan's own Step 5 rule ("if it doesn't reconcile within ~1pp: stop and treat this as a
  new investigation before proceeding") **this should block trusting the audit methodology
  further until root-caused** — not yet investigated. Candidate causes, none checked yet:
  different data window than `CLAUDE_BRIEF_095`'s original scan; dim3's shrinkage-blend mechanism
  (D2/original, generalized further by D8) suppressing the rate differently than expected; or the
  original baseline itself needing re-verification.
- **Zero-ratio spot-check, dims 1/2/7/11 (D1 sentinel-collapse fix)**: 0.274% / 0.125% / 0.247% /
  0.599% over the same 2,108,061-pair window — healthy, comfortably below any collapse threshold.
  Different exact numbers from the original ~50K-row check further down this file (0.16%/0.03%/
  0.22%/2.83%), expected given a much larger and differently-windowed sample — same qualitative
  conclusion (fix holding).
- **`dim12` (Task 1 Step 4 spot-check) — still not done.**

**Cross-repo note, not a MindfulTrader action item**: while running this, found and fixed a real
bug in `../lbrnet/lbrnet/data/observation_vector_bulk_reader.py` — it crashed (`struct.error`) when
its tail-reader encountered the trailing zero-length padding records mentioned above, because its
bounds check only rejected negative sizes, never zero. TDD-fixed there (4 call sites, `<= 0` not
`< 0`), tests added, unrelated to any MindfulTrader/C++ code.

## What was running before the crash (2026-08-14 session, for full context)

An overnight EventDataCollector Phase 1 replay was live:
- Log: `/mnt/c/Trading/logs/MindfulTrader.log`
- Output: `/mnt/c/SierraChart2/Data/event_data_20260814_163135.context` (+ `.alpha` sibling)
- Started (Export armed / hard reset): 2026-08-14 16:31:35
- `LockA` unlocked (Alpha collection active): 2026-08-14 16:47:32 — took 15m57s wall-clock,
  which corresponds to ~19.3 days of TS1 (240-min bar) timeframe warmup to reach
  `macro_window=100`. This is expected/reasonable, not a bug — see "LockA audit" below.
- Replayed market dates so far (as of 17:10:44 that day): ~2023-08-16 through ~2023-09-04. File
  was still growing at 121MB as of that check. **Superseded**: the file's actual final state
  (frozen 19:31/33 the same evening) reached all the way through 2025-01-31 before growth stopped
  — see the 2026-08-15 section above for the full post-mortem.

## LockA audit — resolved, don't re-investigate

Traced `EventDataCollectorStudy.cpp`'s `LockA` gate (`ContextManager::IsObservationSaturated()`
→ `FeatureScaler.warmedUp`/`sampleCount`, 500-sample threshold) down to `AreTs1DimsReady()`
requiring `macro_window=100` TS1 bars. Confirmed via full timestamped `idx=` trajectory (not
just point samples) that TS1 was steadily advancing the whole time — ~1 bar/8-10 real seconds —
not stuck. Once it crossed 100 bars, `LockA`'s 500-sample requirement resolved in under 1 second
(replay throughput is very high once unblocked). **Conclusion: working as designed, no defect.**
100 bars is already the literature-minimum this session's own Gang-doc audit flagged as
`under-powered` for DFA/Hurst (Weron 2002 doesn't characterize below N=256) — no slack to shrink
this warmup further without trading away estimator reliability.

## `.context` preflight findings (first 50,000 aligned pairs only — file is much bigger now)

Confirmed via source (`ContextManager.cpp:895-926`, `MakeObservationData(currentObs)`) that
`.context` stores the **FeatureScaler-scaled output**, not raw physics values — so this data
directly exercises today's winsorization/shrinkage work.

- **D1 sentinel-collapse fix confirmed working on real data**: dims 1/2/7/11 (originally 40-80%
  exact-zero incident) now show 0.16% / 0.03% / 0.22% / 2.83% zero-ratio respectively. Clean
  validation outside of unit tests.
- **dim4's new 12.0 winsorization bound is live and engaging**: sampled max = 12.000 exactly.
- **dim2 hit its flat -6.0 default bound exactly** in tick-level replay — a small honest
  correction to today's earlier audit, which closed dim2 as "0 exceedances, no tick-level
  replica needed" based on a bar-close-only historical screen. Not urgent (winsorization caught
  it correctly), but the "closed clean" characterization was slightly optimistic for tick-level
  behavior specifically. Worth a note if `dim2`'s Gang-doc/FeatureScaler.h comment is ever
  revisited — not filed as an open task, just a documented observation.
- **dim6 has not yet shown its heavy tail** in this sample — max ±5.846, nowhere near either the
  old 6.0 or new 345.0 bound. Not contradictory (see "next step" above), just unresolved by this
  slice of data.
- Zero violations, zero warnings, correlation matrix clean (max abs corr 0.59, nothing >0.8).
- Full JSON report from this run is scratch-only, not saved to the repo (was written to a
  session tmp path, not durable) — re-run `mamba run -n mts python scripts/context_preflight.py
  --input <path> --report-json <out>` from `lbrnet/` if this exact analysis needs reproducing.

## Uncommitted, intentionally left as-is

- `data/NH_NL.csv` and `data/daily_high_low.csv` — both refreshed through 2026-08-13 real data
  (NH-NL from user-supplied StockCharts export, daily high/low via
  `populate_daily_high_low_hybrid.py --start-date 2026-08-07`), both mirrored to
  `/mnt/c/Trading/data/`. User explicitly said "leave it" (uncommitted) — this is expected repo
  state, not stray work to investigate or commit unprompted.

## This session's completed work (all committed)

1. `f7c47bf` — fixed a real `DIM_WINSOR_SIGMA_OVERRIDE[0]`/`[9]` transposition bug in
   `FeatureScaler.h`, caught while cross-checking Gang-doc numbers directly against shipped code
   rather than trusting derivation notes. TDD regression test added.
2. `86dfd25` — Gang-doc entries for the full 16D observation-vector audit (raw clamp guardrail,
   Weibull vs. Fréchet winsorization bounds, shrinkage-as-Ledoit-Wolf-synthesis, new Mandelbrot-
   pillar `dim6` memory-clustering finding).
3. `b1c5ddb` / `3050fdf` — Task 5/6 of
   `docs/superpowers/plans/2026-08-14-observation-vector-full-institutional-coverage.md` marked
   done/flagged. Task 6 items (lbrnet D3 gate, `/mnt/c/Trading/config/` drift) are cross-repo
   pointers, not implemented from here by design.
4. `26bb5f9` — `docs/ROADMAP_EXECUTION_ENGINE.md` audited and marked **SUPERSEDED**: all four
   proposed upgrades already exist, mostly via more sophisticated mechanisms; one (time-decay
   exit) was implemented and deliberately removed for Triple-Barrier train/live parity, so
   re-implementing it would be a regression. Synced across all 4 Documentation Sync Contract
   mirrors plus `docs/CLAUDE.md`.
5. `lbrnet` repo (separate session boundary respected): committed
   `docs/superpowers/specs/2026-08-14-context-preflight-chronic-zero-gate-spec.md` (commit
   `87d4ec7`) — the D3 task brief, not yet implemented, meant to be picked up from an
   lbrnet-rooted session.

## Explicitly deferred, still open

- **`dim3`'s rail-hit-rate discrepancy (1.468% measured vs. 10.078% documented baseline)** — new,
  see the 2026-08-15 section above. Per this plan's own Step 5 rule, treat as a blocking
  investigation, not a pass.
- **The 18-hour file-growth freeze root cause** — new, see the 2026-08-15 section above. Leading
  hypothesis (silent null-obs-skip gate at `EventDataCollectorStudy.cpp:788`) unconfirmed; needs a
  live session (restart + disarm, or add logging) — ask before restarting the collector.
- **Task 1 Step 4** (`dim12` spot-check on the fresh file) — still not done.
- **Task 1** (production deploy/validation) — the overnight replay that was collecting for this
  crashed (see above); Steps 3 (partial: dim1/dim6 done, dim3 open) and 4 remain before this task
  can close.
- ~~**lbrnet D3** (`context_preflight.py` chronic-zero gate)~~ — **CONFIRMED CLOSED**, verified
  directly via `git log` in the lbrnet repo during this session: commit `f307ea5`
  (`feat(preflight): add chronic-zero gate for mid-range dead dims (D3)`), 5 new tests in
  `tests/test_context_preflight.py`, full lbrnet suite 555 passed. Safe to drop from tracking.
- ~~**Config drift** at `/mnt/c/Trading/config/`~~ — **SUPERSEDED**, folded into the new
  `2026-08-15-risk-gate-context-cpp-coevolution.md` spec's Unit C (git-tracked `config/` folder,
  shared calibration config, structural fix for the drift rather than a one-off manual sync). Don't
  treat this as a separate item — work it from that spec.
- **`risk_gate_context` C++ co-evolution spec** — see the top section above. Units B and C SHIPPED
  2026-08-15 evening (6 commits, `master`, not pushed). Only Unit A remains: its live trace to
  confirm (or correct) the population-gap hypothesis, explicitly held pending a live/replay Sierra
  Chart session — ask before running it, don't assume standing permission.
- **`config_hash` audit-event governance** (`TRADE_EXECUTION_SYSTEM.md` §14.2) — discussed in
  depth (see conversation), scoped down to hashing `ExecutionParams::LoadFromFile()`'s /
  `RiskManager.cpp`'s already-in-memory `payload` string and logging via the existing `Logger`
  call site — not started.
- **Volume Profile proxy replacement** (`docs/ADR/sierra_chart_data_feed_setup.md`) — identified
  as a good next quant-value candidate, not started.
