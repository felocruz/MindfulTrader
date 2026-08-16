# Session Scratchpad — Where We Left Off

Last updated: 2026-08-16 (afternoon/evening, session paused for a long break by explicit user request)

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
     multiplier, genuinely regime-aware fusion). **Remaining in this batch: Turtle Soup's Predator-ization
     design only.**
   - **Turtle Soup Predator-ization — still an open design question, not yet spec'd**: cheap intra-bar
     heuristic (Kangaroo-Tail-style shape detection against the same 20-bar reference) vs. first real
     application of ECTS-style (early-classification-of-time-series) prefix-trained ML — explicitly
     tied to the SAME deferred "intra-bar re-inference" gap `CLAUDE.md`'s Trap Detection section
     already named for the τ* layer. Don't design this blind; resolve it as part of this same
     brainstorming pass, not in isolation.
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

- **Unit B — DONE** (`docs/superpowers/plans/2026-08-15-risk-gate-audit-unit-b.md`, commits
  `f9f676c`/`1d4cc7a`): `docs/ADR/gate_stack_stationarity_audit_findings.md` written — audited all
  8 fixed-threshold gates in `RiskManager::EvaluateHardGates()`/
  `ExecutionGate::EvaluateEmpiricalRegimeGates()`, 7 confirmed stationary (5 by direct citation to
  the existing `amihud_gate_percentile_spec.md` verdicts, 1 new finding for the Pareto-top-state-ratio
  gate, which is actually a `1/Hill-α` proxy despite its name — naming debt noted, not fixed).
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
