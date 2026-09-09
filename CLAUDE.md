# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## North Star — read this before anything else, every session

`/home/rcruz/devel/VSCode/PRODUCTION_TRIAGE.md` is the cross-project source of truth for whether
this system is production-ready — `lbrnet`, `MindfulTrader`, `MTS`, and `schema` are all bound to
it, not just this repo. **Read its new "Vision" section first** (added 2026-09-03): the final-goal
vision (chart-based Triple Screen/Raschke signal generation wrapped in a truly institutional-grade,
literature-grounded, real-data-validated Gang statistical layer) stated plainly alongside an honest
current-state assessment (the HMM is currently trained on a contaminated vector and has never
passed its own fat-tail sign-off; execution-layer gates haven't been checked for HMM-regime
awareness at all) — read this before proposing any change or describing the system's maturity.
Then read `SCRATCHPAD.md`.

`MindfulTrader` currently owns or co-owns rows 3, 5, 6, 7, 10, 11, 12, 13, 14 of that document's
critical path (12 and 13 added 2026-08-25; 14 added 2026-08-26, TOP PRIORITY per its own top-of-doc
callout — verify this list is still current before trusting it, row additions don't always get
mirrored here promptly). Any session that changes one of those rows' status must update
`PRODUCTION_TRIAGE.md`'s `§1`/`§1.1` *and* its `NORTH_STAR_STATUS` block in the same edit (Triage
Protocol rule 7) — a status change that isn't reflected there didn't really happen, for planning
purposes across the other three repos.

**NEXT MAJOR INITIATIVE (operator directive, 2026-09-03) — founding question ANSWERED 2026-09-04,
see recommendations before implementing:** `LocalRiskContext`/`RiskGateContext` (the execution-layer
risk context `RiskManager`/`ExecutionGate`/`PositionManager` actually gate on) was verified to be
blind to the HMM's own signal for 7 of its 8 hard gates — only the Amihud illiquidity veto genuinely
reads live HMM state (`InferenceManager::Instance().HmmState()->Dof()`); the other 7, including two
whose names (`paretoTopStateRatio`, `talebSignalSigma`) suggest otherwise, are blind. Operator's own
framing: a predator with regime-aware eyes but a regime-blind nervous system isn't fully a predator —
it's half of one (the entry-fusion layer, e.g. Turtle Soup Option A's applicability mask, already
conditions *whether to pounce* on `PredatorContext.regime`; the risk layer governing *how carefully*
mostly doesn't). Full trace + prioritized recommendations (fix 2 misleading gate names first, resolve
gate 8's apparent duplication of gate 4, extend gate 1's own proven `Dof()`-based pattern to gates
2-5 rather than inventing a new mechanism) now live in
`docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md` §2/§3/§3a
— not yet implemented, review the recommendations before starting. Related, not yet designed:
the EVT/GPD-based "how close to the tail, and closing how fast" execution-layer signal discussed
2026-09-03 (`lbrnet/logs/rc_gemini.log` context around `CLAUDE_BRIEF_123`) — a candidate concrete
first deliverable for this initiative, deliberately scoped to feed `RiskGateContext` directly, not
the HMM's own observation vector (avoiding the `tail_index` redundancy trap already learned this
session). **Living ledger opened 2026-09-03**:
`docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md` (same
spirit/format as the observation-vector ledger below) — seeded with the 8 already-audited
`RiskManager`/`ExecutionGate` gates and this section's own founding question, not yet actively worked.

**Chart-based-TA → Gang-statistical reformulation initiative, opened 2026-09-04**
(`docs/superpowers/specs/2026-09-04-technical-analysis-gang-statistical-reformulation-initiative.md`)
— swing high/low, oscillator divergence, and (case study #2) Wyckoff Spring/Upthrust/Livermore
stop-harvest-reversal patterns are being reassessed for Shannon/Mandelbrot/Taleb/Pareto
reformulation, informed by real imbalance-vs-time-bar chart comparisons on real MES ticks
(`tools/visualization/imbalance_vs_time_bars.py`). **Standing caution, same day**: this is
genuinely elite-grade methodology (extrapolation-risk-aware EVT/GPD calibration, cross-checked via
independent Gemini review — e.g. `amihud_illiquidity`'s winsorization bound recalibrated 2706→3036
this session, `CLAUDE_BRIEF_125`/`_REPLY`) but methodology rigor is not the same claim as a
validated trading edge — keep the Vision section's honest current-state gaps in view, don't let
this work read as "therefore closer to done" than it is.

**Imbalance Triple Screen migration initiative + Force Index Track 2 (`Y_imb`) closure, opened/
resolved 2026-09-06**: foundational architecture doc opened
(`docs/superpowers/specs/2026-09-06-imbalance-triple-screen-architecture-spec.md` — new
`ImbalanceScreen1/2/3.cpp`, multi-frame `ImbalanceBarEngine`, new `ImbalanceMarketObservation`/
`ImbalanceTrainingEvent` schema tables, 4 open design questions, nothing implemented) alongside 3
sibling gang-statistical reformulation docs split from the same external brainstorm: risk-gating
(`2026-09-06-risk-gating-gang-statistical-reformulation-initiative.md`), labeling/augmentation
(`2026-09-06-labeling-data-augmentation-gang-statistical-reformulation-initiative.md`), and
observation-vector (`2026-09-06-observation-vector-gang-statistical-reformulation-initiative.md`,
holds the 5 dims already on the activity clock, split out of the 2026-08-31 elite-feature-set doc).
Also spawned a two-track Force Index reformulation: Track 1
(`2026-09-06-force-index-hardening-spec.md`, calendar-clock hardening) was ABANDONED OUTRIGHT
2026-09-07 (operator directive: not worth further token spend after real-data validation showed
`√V` was harm reduction, not a fix, and a deeper institutional re-think surfaced more remaining
fragility than was worth chasing) — its spec, validation tool, and all build artifacts were
deleted, not just closed; do not resurrect without a fresh operator ask. Track 2
(`2026-09-06-imbalance-work-rate-spec.md`, a genuinely new activity-clock construct
`Y_imb = ΔP/θ`) is **RESOLVED**: real-data-tested against 471.9M real MES ticks,
found real (survives duration-gap and skip-1-bar artifact controls) but decaying to noise by 5
bars; independent Gemini literature review (Almgren & Chriss 2001; Bouchaud/Eisler/Cont-Kukanov-
Stoikov impact-decay literature) classified it as the well-known TRANSIENT component of price
impact, not alpha — closed out as an observation-vector candidate, retargeted to the risk-gating
doc's new §2.4 as an activity-clock Kyle's-Lambda liquidity-gate candidate instead.
`ImbalanceBarEngine` itself was hardened in the process (per-bar imbalance-magnitude retention,
NaN guard, production-named `SetImbalanceThreshold()`, 16/16 native tests pass).

**Row 1 / activity-clock observation-vector thread, current state as of 2026-08-31 — read
`PRODUCTION_TRIAGE.md` row 1 for the full account, this is the condensed pointer**:
- **Shipped and committed**: `fast_taleb_kurtosis` (`ff22e48`/`ea8058b`) as `ObservationData`'s 17th
  field directly (16D→17D — NOT via `Event`/`HMM_OBSERVATION_EXTENSIONS`, despite the original
  plan's Task 6 saying otherwise; this is now row 14's real-world precedent for "keep the struct,
  edit fields in place"). `skewness_idx` replaced in place (`7c51f33`) to source from the same
  activity-clock buffer — no schema change, but a real values/semantics discontinuity for any
  `.context`/`.alpha` data spanning that commit. A real `FeatureScaler.h` calibration-array
  indexing bug (found independently in the same commit) is fixed.
- **Literature-grounded, decided, NOT yet implemented**: `mean_rev_z` and `hurst_exponent` also
  move to activity-clock treatment — Clark (1973)/Ané & Geman (2000)/AFML ch. 2, extended on their
  own direct merits (autocorrelation and long-memory estimation specifically), not by analogy.
  `hurst_exponent` is this system's single worst HMM cross-state discriminator — this may bear on
  row 1's own K=4/fat-tail sign-off question. `recurrence_rate`/`fractal_dim` (Sevcik/RQA) are
  explicitly NOT included — literature search found no precedent either way. Full citations:
  `docs/superpowers/specs/2026-08-25-observation-vector-institutional-hardening-spec.md` §5a,
  `docs/superpowers/specs/2026-08-26-activity-clock-tail-risk-and-decay-spec.md` §6/§9.
- **`docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`** (the living
  literature-grounding reference for every Shannon/Mandelbrot/Taleb/Pareto parameter in this repo)
  has been kept in sync with all of the above — read it, not just the newer specs, for the
  consolidated per-dim view.
- **A staleness audit found 4 `lbrnet`-side dimensionality documents now stale or internally
  inconsistent** relative to the above (one predates this thread — a `lempel_ziv` drop decision
  that was reverted but never corrected in its own doc). Not fixed from here, per repo-scope
  convention — handed to `lbrnet`'s own session: `docs/superpowers/specs/2026-08-26-activity-clock-
  lbrnet-handoff.md` §10.
- **`fractal_dim` window-widening SHIPPED AND COMMITTED, 2026-08-28 (`72ab967`)**, by the sibling
  Claude Sonnet 5 instance this was handed to on 2026-08-27: widened to 400 bars (measured Politis-
  White circular block length ≈404.82 on real MES data, superseding the original ~150-bar proposal),
  decoupled from `recurrence_rate` (which later also moved to activity-clock treatment, see below —
  so it kept its original short window unchanged) and from `PositionManager.cpp`'s gate (which now
  reads an independently-maintained short-window copy via a new `ContextManager::
  SetFractalDimShort()`, fixing the `ContextManager.cpp:589` coupling point the spec flagged as
  critical). `include/SevcikFractalDimension.h` extracted as the pure single source of truth,
  production delegates to it; native test coverage added. `recurrence_rate` (RQA) was subsequently
  found to have its own real cross-domain literature grounding (RQA on event-indexed RR-interval
  sequences, HRV literature) and moved to activity-clock treatment alongside `mean_rev_z`/
  `hurst_exponent` — decided, NOT yet implemented, no plan written yet (this reverses the "explicitly
  NOT included" verdict two bullets above, which predates that second literature pass).
- `docs/superpowers/specs/2026-08-25-pattern-detection-institutional-hardening-spec.md` (row 13,
  unrelated thread) — Phase 0 diagnosis done, still blocked on 5 open questions (§7), untouched
  since 2026-08-25.
- **New whole-vector Elite Feature Set Curation initiative, founded 2026-08-31**
  (`docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md`) — supersedes ad hoc
  pairwise dim fixes with a phased methodology (Gaussian-moment audit → redundancy → Feature Saliency
  → mRMR selection), motivated by this system's Student-t HMM using hard-enforced diagonal covariance
  (so the real curse-of-dimensionality failure mode is double-counted evidence under violated
  conditional independence, not covariance ill-conditioning). **Phase 0 (Gaussian-moment audit) CLOSED
  OUT, 2026-08-31, bar 3 ambiguous decisions**: `burstiness_index` (robust CV, MAD/median × a
  newly-derived 1.4404199 Poisson-neutrality constant, NOT the standard 1.4826 — **superseded
  2026-09-02, see below**), `vol_convexity`
  (REMOVED from the schema entirely, 19D→18D — already decided 2026-08-25 as the weakest
  discriminator measuring the wrong thing structurally, executed today, not reformulated), `mean_rev_z`
  (median/MAD price z-score + median-centered lag-1 autocorrelation, Kim & White 2004) — do not confuse
  this fix (the existing time-bar/live `mean_rev_z` formula) with the separate, still-undecided
  `fast_mean_rev_z`/activity-clock move referenced two bullets above, which remains unwired. A real bug
  was found and fixed as a side effect of the schema shrink: `FeatureScaler.h`'s
  `LOGZ_WINSOR_SIGMA_OVERRIDE` array was silently missing one element, misaligning `liq_fragility`'s
  calibrated bound by one index — caught by the native test suite, not inspection, the same failure
  class `DIM_RECURRENCE_INDEX`/`DIM_FRACTAL_INDEX`'s own comments already warned about. All native
  tests pass, `./build_dll.sh --no-clean` builds clean; **committed 2026-09-02 (`d2ab57c`)** after
  sitting uncommitted across multiple prior sessions — verified clean before committing, not just
  trusting the prior claim. Still open: `amihud_illiquidity`/`relative_range`/`liq_fragility` (2
  ambiguous Gaussian-moment-adjacent cases needing a literature decision, not a mechanical fix, since
  `hurst_exponent` was resolved 2026-09-02, see below) and `fast_mean_rev_z`'s wire-or-drop call
  (its unwired `ActivityClockMeanReversion.h` implementation was reformulated to median/MAD
  2026-09-02, `b0ab21a`, ahead of that decision, so it no longer needs a formula fix first). **Separate finding, same
  day**: any attempt to re-measure a dim's importance against the *existing* `models/hmm_model.pkl`
  (e.g. did making `amihud_illiquidity`/`liq_fragility` live-reactive on 2026-08-29 help) is circular —
  that model's state labels were learned from the pre-Phase-0, still-contaminated vector. Recorded as
  an open methodological question (initiative doc §5), not yet resolved either way.
- **`burstiness_index` superseded and FIXED, 2026-09-02** — the 1.4404199-constant formula above
  turned out to still be broken at real tick density (never validated against genuine per-tick data
  before). Once `lbrnet/data/raw/mes_ticks.parquet` (471.9M real ticks,
  `tools/scid_processing/scid_to_ticks_parquet.cpp`, committed) existed, recalibration found the
  production bound clipping ~74% of real readings. A first fix attempt (Goh-Barabási bounded
  transform on the same IAT-ratio) **failed real-data re-validation** (clip rate unchanged, mean|z|
  roughly doubled) — diagnosed with Gemini (`lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_121`/`122`) as
  a point-mass degeneracy no post-hoc transform can repair (73.9% of real 100-tick windows have
  median inter-arrival-time collapsed to the timestamp field's own 1us floor). Real fix: abandoned
  inter-arrival times entirely, reformulated to a robust Index of Dispersion for Counts (Daley &
  Vere-Jones 2003) over K=10 self-scaling time sub-bins (`include/EventVelocityEngine.h`), constant
  `1.58113883`=√10/2 (Poisson(10)'s exact sigma/MAD, not the standard-Normal 1.4826). Full real-data
  re-validation, all 471.9M ticks: mean|z|=1.14, max|z|=49.28, rate-at-bound(6.0)=1.96% — sane,
  normal clip rate, no further bound recalibration needed. Committed, native tests pass,
  `./build_dll.sh` clean. Full detail: initiative doc §7 row 2.
- **Elite Feature Set Curation initiative, Phase 1 (whole-vector redundancy audit) DONE for the
  calendar-clock vector, 2026-09-07**: `tools/observation_vector/whole_vector_redundancy_eval.cpp`
  ran the full 11-dim correlation matrix against all 471.9M real MES ticks (76,411 TS3 bar-close
  snapshots) — max |r|=0.34, no redundancy found across the vector (full result:
  `tools/output/whole_vector_redundancy_eval_20260907_201445.txt`). Phase 2 (Feature Saliency EM
  fitting) got its own dedicated implementation spec the same day:
  `docs/superpowers/specs/2026-09-07-feature-saliency-em-fitter-spec.md`. **New thread opened same
  day, not yet spec'd**: an offline, non-Sierra-Chart `.context`-file generator (reconstructs
  TS1/TS2/TS3 from raw tick data, computes the real 18D vector, replicates the real Mahalanobis
  significant-change gate — no simplified substitute cadence) is now provably feasible:
  `ContextManager::ComputeTriggerDecisionMetrics`/`FeatureScaler` were confirmed ALREADY pure C++
  (no `sc.*` dependency at all), and the two remaining SC-coupled dim calculators (`mean_rev_z`,
  `liq_fragility`) were extracted into pure, natively-unit-tested headers
  (`include/MeanReversionCalculator.h`, `include/LiquidityFragilityEngine.h`) this session, verified
  bit-faithful against the production formulas and wired back into `StudyHelperFunctions.cpp`
  (`./build_dll.sh --no-clean` clean, 9/9 new native checks pass, no `FeatureScaler` regression).
  Every one of the 18 dims' real math is now pure/testable; the generator itself is not yet built.
- **Offline `.context` generator (`tools/market_data_replay/`) SHIPPED, 2026-09-08** — the thread
  opened 2026-09-07 above is now functionally complete.
  `docs/superpowers/plans/2026-09-08-market-data-replay-implementation.md` Tasks 1-11 done (75/75
  native checks pass): `MarketDataReplayEngine.h` reconstructs all 18 observation dims from raw
  tick data (TS1/TS2/TS3 `TickBarAggregator`s + activity-clock `ImbalanceBarEngine` + the real
  `FeatureScaler`/`ObservationTriggerGate`), a CLI driver (`MarketDataReplay.cpp`) + standalone
  `ContextFileWriter.h` (byte-faithful `.context` writer -- `LBRFileManager.cpp` itself can't link
  into a standalone Linux tool, confirmed via direct compile attempt: transitively pulls in
  `windows.h`), validated against the real 471.9M-tick `mes_ticks.parquet` (335,147 aligned MO+SS
  pairs, zero sequence mismatches). Two real findings/fixes surfaced along the way, both already
  landed in this repo: (1) **`ContextManager.cpp`'s `ShouldTriggerHMM()` quality-over-quantity
  correction** — data collection previously emitted on ANY dim moving by ~1e-5 (near-continuous,
  highly autocorrelated output), now shares the SAME Mahalanobis significant-change standard as
  live trading (this system's own cited Rydén/Teräsvirta/Åsbrink 1998 HMM precedent argues for
  lower-frequency, information-rich sampling, not near-continuous ticks) —
  `docs/superpowers/specs/2026-09-08-context-emission-gate-quality-over-quantity-spec.md`. (2) a
  real, NOT-yet-fixed finding: `regime_tenure` is documented ("bars in current regime state" /
  "TS2 bar-closes in current regime") but the actual counter increments/resets on every TICK, not
  bar close (`SetWaveContext()`'s real body has no bar-close guard) —
  `docs/superpowers/specs/2026-09-08-real-dll-findings-from-offline-generator-spec.md`. **Task 12
  (byte-validation against a genuine SC-collected file) is BLOCKED** — the only `.context` file on
  disk (`lbrnet/data/raw/event_data.context`) is schema_version 230, current schema is 240;
  `context_reader.h`'s own hard-refuse gate correctly blocks reading it. Needs a fresh SC-collected
  file before that task can proceed.

## Project Overview

MindfulTrader is the **C++ producer/execution layer** (ACSIL + low-latency messaging) for a Sierra Chart algorithmic trading system. It implements the **Elder-Raschke Confluence System** — Elder's Triple Screen three-timeframe hierarchy with Raschke entry patterns on Screen 3, conditioned by a regime-aware layer (Student-t HMM, Hurst/DFA, Shannon entropy, Taleb kurtosis) — publishing FlatBuffer events over ZMQ to downstream Python consumers (`lbrnet` for ML training, `MTS` for GUI).

**No GUI logic** (belongs in `MTS`), **no ML training logic** (belongs in `lbrnet`), and **no schema source edits** outside `../schema/mts_schema.fbs` ownership rules.

## Build & Test Commands

```bash
# Full clean build (cross-compiles to Windows DLL from WSL)
./build_dll.sh

# Incremental rebuild
./build_dll.sh --no-clean

# Schema must be regenerated before build if .fbs schema changed
bash /home/rcruz/devel/VSCode/scripts/regenerate_schema.sh

# Python ZMQ integration tests
cd tests && ./run_python_tests.sh
```

**Do not** use ad-hoc `flatc` or raw `cmake`/`ninja` invocations — always go through `build_dll.sh`.

The output artifact is `build-windows/bin/MindfulTrader.dll`. This is a Windows DLL cross-compiled via `clang-cl` using the `wsl-clang-cl-release` CMake preset.

## Done Checklist

- Build succeeds via `./build_dll.sh`
- If schema was touched, `regenerate_schema.sh` was run first
- Any contract-impacting changes are documented and compatibility considered
- Native trap-risk behavior remains available and actionable without Python confirmation

## Standalone Analysis Tools (`tools/`)

`tools/` hosts standalone, natively-tested analysis/calibration/ingestion utilities — never
added to `CMakeLists.txt`/`build_dll.sh`; built via bare `mamba run -n mts g++ -std=c++17 ...`
(Arrow/Parquet tools also need `$(mamba run -n mts pkg-config --cflags/--libs arrow parquet)`
plus `-Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib`).

- **Organized into subfolders by function** (reorg 2026-09-02): `tools/observation_vector/`
  (16D HMM observation-vector dimension calibration/eval, incl. `market_test_stats.h`/
  `market_data_io.h`), `tools/context_pipeline/` (`.context` training-cache file I/O),
  `tools/scid_processing/` (`.scid` tick decode/mirror-sync/parquet export). Put new tools in
  the matching subfolder, not flat in `tools/`. `#include "x.h"` resolves relative to the
  including file's own directory, so same-group cross-includes need no path prefix.
- **Compiled binaries go in `tools/bin/`** (gitignored) — mirrors `build-windows/bin/`'s
  convention; never mixed into the source subfolders above.
- **TOP-LEVEL DIRECTIVE (2026-09-03): always keep tool output.** Every `tools/` executable that
  can run more than a few seconds must construct a `ToolProgressLogger` and route ALL
  results/reports through its `Log()` (never a bare `std::printf`/`std::puts`) — it automatically
  archives the full transcript to `tools/output/<toolName>_<timestamp>.txt` on exit (gitignored,
  never truncated/overwritten across runs, unlike `tools/log/`'s live-progress file). This exists
  because a real ~1h43m real-tick-data validation run's only report was printed to a terminal whose
  scrollback was lost before being read, forcing a full re-run.
- **TOP-LEVEL DIRECTIVE (2026-09-04): check `tools/RECALIBRATION_LEDGER.md` before launching a new
  heavy recalibration pass.** `tools/output/` is entirely gitignored, so a completed archive full of
  real findings is otherwise invisible to `git status`/code review and can sit unused indefinitely
  (found 2026-09-04: 3 completed archives sat unused for hours, only noticed by chance). Every
  `ToolProgressLogger` now auto-appends a row to this git-tracked ledger on exit — check its
  PENDING REVIEW rows first; a pending row may already answer the question a new run would
  re-derive at real compute cost. Update its Status column by hand once a finding is consumed.
- Native tests use a `check(name, bool)` + `g_failures` + final `ALL PASS`/`N FAILURE(S)`
  harness convention (see `tools/context_pipeline/test_context_reader.cpp`) — no GoogleTest/CMake.

## Local Git Repository

This project is a **local Git repo** (initialized 2026-07-14, `master` branch, no remote by default). Use Git-based validation (`git status`, `git diff`, changed-file queries) as a source of truth alongside file reads and runtime command exits.

- Git-ignored (regenerable): `build-windows/`, generated `include/generated/*.bak_*` backups, `__pycache__/`, `.btst`/`.lbr` binary artifacts. Generated schema headers **are** tracked.
- A `pre-commit` hook enforces the Documentation Sync Contract: if you stage one of the four mirror docs (`README-AI.md`, `.github/copilot-instructions.md`, `CLAUDE.md`, `GEMINI.md`) without the others, the commit is blocked. Bypass with `git commit --no-verify`.

## Chart Timeframes (Critical — Do Not Confuse)

- **TS1 (Screen 1)**: 240-minute bars — daily trend
- **TS2 (Screen 2)**: 60-minute bars — intermediate momentum
- **TS3 (Screen 3)**: 15-minute bars — short-term entry timing

These are **minutes**, not seconds. The bar period controls aggregation only — it does **not** reduce tick frequency.

**All three screens are equally hot paths.** `AutoLoop=1` means Sierra Chart invokes every study function on every incoming tick/trade, regardless of bar period. TS1 and TS2 receive just as many ticks as TS3.

## Architecture

### Data Flow

```
Sierra Chart Tick
    ↓
SCStudies.cpp (main ACSIL entry — calls all managers each tick)
    ├→ TripleScreen1/2/3 — update indicators per timeframe
    ├→ IndicatorManager::UpdateBarContext()
    ├→ ContextManager::UpdateStatistics()
    ├→ PositionManager::Update()
    ├→ RiskManager::Evaluate()
    ├→ TradeSignalManager::GenerateSignals()
    ├→ EventSerializer::PublishEvent()   → ZMQ PUB port 5555
    └→ AIHeartbeatMonitor               → ZMQ PUB port 5559
```

### ZMQ Ports

| Port | Type | Purpose |
|------|------|---------|
| 5555 | PUB  | Main event stream (indicators, signals, positions) |
| 5556 | REP  | Trade execution validation (request/reply) |
| 5558 | REP  | BacktestLiveAgent trade execution (REQ/REP during replay) |
| 5559 | PUB  | Heartbeat monitor (~1s interval) |
| 5560 | REP  | Control-plane handshake (CONFIG_REQ/ACK) |
| 5561 | ROUTER/DEALER | HMM regime inference (backtest + live) |

### Key Singletons

| Class | File | Role |
|-------|------|------|
| `IndicatorManager` | `src/IndicatorManager.cpp` | Lifecycle & caching for 30+ indicators (DOD) |
| `PositionManager` | `src/PositionManager.cpp` | Trade state machine, fills, P&L |
| `RiskManager` | `src/RiskManager.cpp` | Daily loss limits, Kelly sizing |
| `ContextManager` | `src/ContextManager.cpp` | Volatility, efficiency, regime detection |
| `SystemOrchestrator` | `src/SystemOrchestrator.cpp` | CONFIG_REQ/ACK handshake |
| `TransportStream` | `src/transport/TransportStream.cpp` | ZMQ PUB socket |
| `HMMClient` | `src/HMMClient.cpp` | DEALER socket to Python HMM ROUTER on 5561 |
| `TradeExecutionServer` | `src/TradeExecutionServer.cpp` | REP socket on 5558 (backtest) |

### FlatBuffers / Schema

Generated headers live in `include/generated/`. Two schemas exist:

- **`../schema/mts_schema.fbs`** — live wire schema; generates `mts_schema_generated.h`
- **`../schema/backtest_schema.fbs`** — backtesting artifact schema; generates `backtest_schema_generated.h`

Regenerate via the script above; never call `flatc` directly.

**Known FlatBuffers version state (as of 2026-05-10):**
- Headers (`include/flatbuffers/base.h`): version **25.1.24**
- System `flatc` binary (mamba mts env): version **24.3.25** — mismatch
- `mts_schema_generated.h`: asserts 25.1.24 ✓
- `backtest_schema_generated.h`: assertion updated to 25.1.24 ✓ (was stale at 24.3.25)
- Long-term fix: upgrade `flatc` to 25.1.24 and run `regenerate_schema.sh`

`CMakeLists.txt` enforces two schema contracts:
- **WS-07** (`scripts/audit_shared_root_writes.sh`): validates manual FlatBuffer writes bypass generated helpers correctly
- **WS-03** (`MindfulTraderSchemaContract` target): compiles generated policy artifacts

## Performance Rules (Hot Path)

- **No heap allocations** in recurring ACSIL update paths (all three TS screens, every tick)
- **`IndicatorManager`** currently uses a hand-written heterogeneous `IndicatorStore` (~44 differently-typed named members) plus a separate `std::array<BaseIndicator*, MAX_INDICATORS>` pointer-index array for O(1) `IndicatorKey`-enum lookup (`GetIndicator<T>(key)`, never string hashes or map lookups) — this is being migrated to true packed-array (SoA) storage with compile-time devirtualized access; see `docs/superpowers/specs/2026-08-04-indicator-manager-dod-soa-design.md`.
- **Permanent hybrid architecture (indicator-manager-dod-soa plan, Task 15):** the packed arrays (`IndicatorLayout.h`/`IndicatorPackedState.h`) are the canonical, devirtualized path for every hot-path read (`CheckTrigger`, `PopulateIndicatorState`, `GetTrainingEventT`, `EventSerializer`, `BackTesterStudy`'s Float32 exports). `IndicatorStore`/`BaseIndicator`/`Indicator<T>` and its leaf classes remain permanently as (a) the write-side compute engine Triple Screen calls into every tick, and (b) the read path for keys marked `StorageBlock::NotPacked`. This is not a partially-finished migration — it is the intended end-state per the design spec's "true DOD while maintaining OOD goodness" framing. A full write-side rewrite (extracting all remaining indicators' compute logic to free functions, as Task 6 did for `Macd`) remains possible as a future initiative but is out of scope. Task 15 deleted the only 4 fully-dead orphan classes with zero live callers (`Ema`, `AdxIndicator`, and the dead `IndicatorManager::GetIndicator<T>()` instantiations for `HmmStateIndicator`/`MarketClimateIndicator` — those two classes remain live, owned by `InferenceManager`, not `IndicatorManager`). All other leaf classes have live callers and stay.
- **ZMQ** calls must be non-blocking on UI-sensitive paths
- Preserve microsecond timing conventions where latency is tracked

## Code Safety Rules

- **Standing rule, set 2026-08-26 in `PRODUCTION_TRIAGE.md`'s top banner (read it there for the
  full statement): this system is not in production. Once you've verified there's truly no live/
  test usage, the default is deletion, not preservation — remove dead code, legacy paths, and
  backward-compatibility shims on sight. This flips the moment `READY_FOR_PRODUCTION` there reads
  YES.**
- Before removing any symbol, search the full repo for usages in `.h`, `.cpp`, and PCH files
- Fix root causes; do not remove symbols to silence compile errors
- If unsure whether code is used cross-project, preserve it and document concern
- **`SCDLLName("Mindful Trader - Version 2.0 Devel")`** in `SCStudies.cpp` is critical — removing it causes the DLL to fail loading in Sierra Chart

## Trap Detection (Native-First)

**TRAP = structural invalidation of the entry thesis** — a sprung-trap reversal (`FAILED_*`) that fires ahead of the money-stop; it is NOT the price stop and NOT a trend/regime shift (an adverse `DECISIVE_*` counter-break is a separate `REGIME_INVALIDATION`, never TRAP — ruling 2026-07-15, `docs/ADR/triple_barrier_trap_definition_ruling.md`).

Two observers of one truth: (a) a native REACTIVE floor = completed-bar `StructureTest` reversal set, deterministic, model-independent, the parity anchor with the labeler; (b) the model's ANTICIPATORY `TRAP_*`, now a first-class input, gated by the dynamic Bayesian threshold τ* = C_FP/(C_FP+C_FN) (Elkan 2001; C_FP=|target−price|, C_FN=|price−stop|). Native is always-on and authoritative; the model exit is additive, acting only when fresh ∧ p≥τ* ∧ adverse — if Python is stale/down/disagreeing, native governs and the model may LEAD but never SUPPRESS the floor. Phase 1 = EXIT/risk only (TRAP-as-entry deferred); enable the anticipatory override only when out-of-sample F_0.25 > 0.65. TRAP ranks priority #1 ahead of stop/target/time. Co-evolution: the native `StructureTest` TRAP definition must equal the labeler's (`triple_barrier_scanner.py`), which routes `DECISIVE_*` out of TRAP into `REGIME_INVALIDATION`. Timing (ruling 2026-07-15): the reactive floor is completed-bar (parity + training anchor); live intra-bar responsiveness comes from τ* recomputed every tick vs current price over the standing completed-bar-trained p (sequential/quickest-detection + early-classification — Shiryaev/Wald, Dachraoui 2015, Mori 2017 — act when the posterior crosses the cost boundary, not at bar close). Intra-bar RE-INFERENCE (p updating within the bar) is deferred pending ECTS-style intra-bar-prefix training to avoid train/live OOD; Phase 1 uses per-tick τ* only.

`StatisticalContext` (volatility, efficiency, relRange, velocity, regimeTenure) is the canonical mechanics backbone and must remain wired through TS2/TS3 → `ContextManager` → `TrainingEvent`.

Canonical weighting, thresholds, and governance gates: `../lbrnet/docs/labeling/LABELING_AND_AUGMENTATION_SPEC.md` under *TRAP weighting policy table (normative default)*.

## Backtesting Pipeline

### Architecture

`BackTesterStudy.cpp` runs during Sierra Chart replay and drives the full live pipeline (same `IndicatorManager`, `PositionManager`, `RiskManager`, `HMMClient`, `TransportStream` as production). It is not a simulation — it runs real inference.

**Three phases:**
- Phase 1: Data export (redirects to `scsf_EventDataCollector`)
- Phase 2: Pure neural network — Transformer inference, no risk filtering
- Phase 3: Risk-managed — full pipeline with `RiskManager` gating

### .btst Binary File Format

Size-prefixed FlatBuffer records, one per event. Record ordering:
1. `RunManifest` — written once at study init
2. `DecisionEvent` — one per `HasSignificantChange()` firing
3. `PredictionAck` — one per ACK/REJECT from `PositionManager`
4. `TradeRecord` — one per completed round-trip (written at exit)
5. `RunSummary` — written once at study teardown

### Join Keys

- `decision_id` (uint64, monotonic) → links `DecisionEvent` ↔ `PredictionAck`
- `execution_key` = `run_id + "::" + str(parent_internal_order_id)` → links `PredictionAck` ↔ `TradeRecord`

**Note:** `DecisionEvent.model_action` and `model_confidence` are always 0 in C++ because `WriteDecisionEventFb()` fires before Python responds. Consumers must join `DecisionEvent ← PredictionAck` on `decision_id` to get model outputs. The Python `bt_reader.py` `to_dataframes()` does this join automatically.

### Python Counterparts

| File | Role |
|------|------|
| `lbrnet/backtest/backtest_server.py` | ZMQ inference server for replay; `BacktestLiveAgent` (REQ to 5558); HMM ROUTER on 5561 |
| `lbrnet/backtest/bt_reader.py` | Reads `.btst` files into pandas DataFrames; performs decision←ack backfill join |

### Governance

- Canonical source of truth: `../docs/BACKTESTING_FRAMEWORK.md`
- Reuse the existing live message protocol and lifecycle semantics — do not create parallel business logic paths
- Do not mark backtesting work production-ready unless acceptance gates in that document are explicitly evaluated

## Key Documentation

### Wire Protocol
- `../schema/FLATBUFFER_MASTER_SPECIFICATION.md` — envelope structure, full message catalog, serialization patterns (zero-copy, builder API, request correlation), field conventions

### Trade Lifecycle
- `../docs/TRADE_EXECUTION_SYSTEM.md` — canonical governance: trade open/close flow, C++→Python→Firestore lifecycle, sovereign-grade design decisions
- `../docs/TRADE_EXECUTION_MESSAGE_PROTOCOL.md` — companion: `TradeRequest/Response/Close/CloseResponse` message specs and code examples

### Risk & Strategy
- `../docs/RISK_MANAGEMENT_SYSTEM.md` — all risk layers: daily loss limits, Kelly sizing, regime scaling, consecutive-loss gates
- `../docs/TRADING_STRATEGIES_COMPLETE_REFERENCE.md` — complete catalog: Elder Triple Screen setups, Raschke patterns, all entry/exit tactics

### HMM Integration
- `../docs/HMM_RUNTIME_REFERENCE.md` — `RiskStateUpdate` field contract, C++↔Python message flow, 16D observation vector field order
- `../docs/architecture/STUDENT_T_HMM_EVENT_TRANSFORMER_ARCHITECTURE.md` — HMM↔Transformer authority order, train/live parity rules

### Labeling & Trap Policy
- `../lbrnet/docs/labeling/LABELING_AND_AUGMENTATION_SPEC.md` — TRAP weighting policy table (normative default for native trap-risk thresholds)

### Active Roadmaps
- `../docs/ROADMAP_EXECUTION_ENGINE.md` — **SUPERSEDED 2026-08-14**, do not implement; all four proposed upgrades already exist under different (mostly more sophisticated) mechanisms, see the doc's own "Superseded" section for the per-item mapping
- `../docs/ROADMAP_CONTEXTMANAGER_REFACTOR.md` — `ContextManager` architecture baseline and remaining hardening work
- `../docs/SCHEMA_DRIVEN_SERIALIZATION_PARITY_INITIATIVE.md` — active initiative: eliminating train/live serialization drift between C++ event path and Python training ingestion
- `docs/ADR/execution_correctness_findings_spec.md` — 12 verified correctness/parity findings across `PositionManager`/`RiskManager`/`ChandelierStopManager`/`Scoring`/`ExecutionGate` (2026-07-10 audit). Finding 1 (`UpdateContext()` never called — entire regime-defense subsystem runs on frozen state) — RESOLVED (commit `097e11b`; `SyncRegimeState()` wired into `Update()`, `regime_state_wiring_fix_spec.md`); Finding 12 is a Python-port parity gap, not a C++ fix.
- `docs/ADR/sierra_chart_data_feed_setup.md` — (Decision implemented 2026-08-04) Switched to Sierra Chart Package 11 + Denali CME-no-depth + IB execution-only for live ES trading; Package 12/MBO evaluated and not recommended. Active opportunity: replace proxies (`StudyHelperFunctions.cpp`'s "TPO Value Area Proxy" and `StructureEngine.cpp`'s close-price-histogram "Point of Control") with real Volume Profile study values via `sc.GetStudyArrayUsingID`.
- `docs/superpowers/specs/2026-08-13-observation-vector-institutional-elevation-spec.md` (plan: `docs/superpowers/plans/2026-08-13-observation-vector-institutional-elevation.md`) — (Shipped 2026-08-13, commits `8173fcf..23a2e54`) 16D observation vector + `FeatureScaler` elevated to consensus institutional practice per `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`'s literature-grounding pass. Fixed a live training-data defect (`FeatureScaler` dedupe-at-ingestion — median-collapse-to-zero was corrupting 31-71% of samples across 6 dims), Miller-Madow entropy correction, RQA epsilon fixed-recurrence-rate recalibration, Hill-plot k-selection + EWMA smoothing, and replaced moment-based skewness/kurtosis with Bowley/Moors robust estimators — including a full empirical percentile-matching threshold migration across every live risk-gate consumer (`RiskManager`/`Indicator`/`Scoring`/`TradeDecisionEngine`/`PositionManager`/`ExecutionGate`) using real MES `.scid` historical data, zero synthetic values. **Known pending follow-ups, explicitly out of this initiative's scope:** `../docs/RISK_MANAGEMENT_SYSTEM.md` §5.3/§10 and `../docs/TRADE_EXECUTION_SYSTEM.md` §H.6 still cite pre-migration kurtosis numbers; `lbrnet/scripts/context_preflight.py`'s D3 `chronic_zero_threshold` gate (`docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md`) was never implemented (lbrnet-side); two live JSON config files outside git (`/mnt/c/Trading/config/`) carry their own copy of the migrated thresholds and won't travel to a new machine without a manual sync; `config_hash`/audit-event config governance (`TRADE_EXECUTION_SYSTEM.md` §14.2) not yet implemented for `ExecutionParams`.

### Operator Guides
- `../docs/VISUAL_REGIME_TUNING_GUIDE.md` — Elder Triple Screen chart observations → specific system parameter changes

---

## Documentation Sync Contract

`README-AI.md`, `.github/copilot-instructions.md`, `CLAUDE.md`, and `GEMINI.md` are mirrors — update all four in the same change when guidance changes.
