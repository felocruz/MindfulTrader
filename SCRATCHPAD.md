# Session Scratchpad — Where We Left Off

Last updated: 2026-08-27 — Thread C's post-implementation gap is now RESOLVED (see its own update
below); the plan is code-complete and build-verified but fully uncommitted. Read `PRODUCTION_TRIAGE.md`
row 1 (synced same day) for the terse cross-project version.

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

**Next MindfulTrader-owned step after this thread's commit cleanup, per spec §5c**:
`skewness_idx`'s activity-clock twin (same move as kurtosis — `BowleySkewness` over
`ActivityClockManager`'s imbalance-bar buffer). **Given the corrected precedent above, this should
most likely land as a new field directly inside `ObservationData` too (an 18th field), not on
`Event` — confirm against row 14's eventual policy spec before assuming either way.** Formula
question already closed, infrastructure already built. A second, independent thread —
window-widening `recurrence_rate`/`fractal_dim`/`mean_rev_z` per `docs/superpowers/specs/
2026-08-25-observation-vector-institutional-hardening-spec.md` §5 — needs an autocorrelation-time
derivation before its proposed ~150/~600-bar targets are finalized. **Both handed to a sibling
Claude Sonnet 5 instance, 2026-08-27** — not being executed from this session.

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
(`2026-08-25-observation-vector-institutional-hardening-spec.md`) is unchanged by today's
work** — still applicable, still not started: `recurrence_rate`/`fractal_dim` (30-40 bars @ 60min,
propose ~150) and `mean_rev_z` (10-40 bars @ 15min, propose ~600) window-widen candidates pending a
real autocorrelation-time derivation; `hurst_exponent`/`fisher_info` explicitly NOT a window case
(likely data-quality artifacts instead); `skewness_idx`/`micro_asymmetry` dead-code candidates once
lbrnet's spec lands.

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
