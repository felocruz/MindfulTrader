# Spec: Two-Classifier Risk/Sizing Architecture — C++ Deployment

**Status**: Design discussed and confirmed at the architecture level (2026-08-24, cross-project
production-triage session — see `/home/rcruz/devel/VSCode/PRODUCTION_TRIAGE.md` §0). **Not
started.** No code written. Companion to `lbrnet/docs/superpowers/specs/2026-08-24-two-classifier-
risk-sizing-architecture-spec.md`, which owns the Python-side training design for both
classifiers this spec deploys, and to `2026-08-24-predator-fusion-transformer-signal-decay-spec.md`
(same directory), which owns the `PredictionAgeUs()`/decay mechanism this spec's meta-labeler
wiring depends on.

## 1. Purpose

Two new classifiers (full design: the `lbrnet` companion spec) are trained in Python and need to
run live in C++, on every tick, consuming time-decayed upstream signals rather than raw values
that may be stale between infrequent Python-side updates:

1. **Soft/gate classifier** — general market-state danger veto, runs after the existing
   rule-based hard gate. Deliberately does **not** consume live HMM output (independence from the
   HMM is the point — see the `lbrnet` spec's Section 2 for why).
2. **Meta-labeler** — decides position size once the Transformer has produced a side call.
   **Corrected 2026-08-24, user-confirmed**: Predator Fusion is training-time-only — it never
   runs live, for Turtle Soup or any other named pattern (see Section 2, 3). Its only role is
   scoping/labeling the meta-labeler's *training* set in Python; it is not one of the
   meta-labeler's live inputs. Consumes live: the Transformer's own time-decayed side/confidence
   signal (`PredictionAgeUs()`), the soft classifier's own score, HMM posterior-derived scalars,
   and the existing hand-crafted sizing multipliers already live in `RiskManager`/`Indicator.h`.

## 2. What already exists (verified by reading the code directly, 2026-08-24)

- `FreshnessGateEngine::IsFresh(nowUs, lastWriteUs, maxAgeUs, bypassCheck)`
  (`include/FreshnessGateEngine.h:31`) — generic binary freshness primitive, already used twice.
- `InferenceManager::HmmStateAgeUs(nowUs)` / `IsHmmStateStale(nowUs, maxAgeUs=5'000'000ULL)`
  (`include/InferenceManager.h:69,73`) — **continuous age tracking already exists for HMM state**,
  binary staleness check built on top of it.
- `InferenceManager::IsPredictionFresh(nowUs, maxAgeUs)` (`include/InferenceManager.h:120`) —
  same binary pattern for the Transformer's `PredictionState`, but **no continuous age getter
  exists yet** for it (unlike the HMM side) — this is exactly what the companion decay spec adds.
- `HmmStateIndicator` (`include/Indicator.h:1734`) already carries 7 posterior-derived scalars
  (`riskMultiplier`, `transitionRisk`, `dof`, `mahalanobis`, `tailWeight`, `expectedDuration`,
  `entropy`) and 5 sizing-domain methods built on them (`DofStopScale`, `IsOutlierEmergency`,
  `MahalanobisSizingCap`, `TailWeightDiscount`, `SizingDurationFactor`) — **does not carry the
  full K-length posterior vector**, only these derived scalars plus the discrete decoded state.
- `TurtleSoupFusion.h`'s live entry-fusion functions consume none of the above — pure
  price-geometry pattern detectors, no Transformer or HMM input at all. **Superseded, confirmed
  2026-08-24**: this gap is not being closed by wiring the Transformer signal into Predator Fusion
  — Predator Fusion is training-time-only (below); the Transformer signal instead feeds the
  meta-labeler directly. `EvaluateTurtleSoupOptionA()` has exactly one live call site today,
  `src/TripleScreen3.cpp:1267` — retiring it once the meta-labeler ships and is validated is a
  confirmed performance win (removes per-tick pattern-geometry evaluation, replaced by the
  meta-labeler's single classifier inference). See Section 3, `PRODUCTION_TRIAGE.md` row 10.
- **The existing precedent for deploying a Python-trained classifier natively in C++**:
  `EvaluateTurtleSoupOptionB()` (`include/TurtleSoupFusion.h:77-99`) loads `weights`/`bias` at
  runtime from a `ClassifierParams` struct, populated from `config/classifier_params.json`'s
  `"turtle_soup_option_b"` section — config-driven, not hand-transcribed constants. The header
  comment is explicit this is deliberate: *"not m2cgen-generated, not a linked ML runtime library"*
  (per `docs/superpowers/specs/2026-08-16-ects-prefix-training-infrastructure-spec.md`'s deployment
  guidance). **This is the ONLY existing "Python-trained model runs natively in C++" precedent in
  this codebase** — the Transformer does not count; it stays entirely on the Python side (`MTS`)
  and exchanges predictions with C++ via FlatBuffers over ZMQ (RPC), never a native port. **The
  gap here (header's own mandated golden-vector parity test not existing yet — `tests/cpp/
  TestTurtleSoupFusion.cpp:75` uses only synthetic weights) is no longer a live-safety blocker**,
  now that Predator Fusion is confirmed training-time-only (Section 1) — Option B was never wired
  live and, per the new direction, never will be. What remains genuinely valuable from this code:
  the **deployment pattern itself** (config-driven `ClassifierParams` loading) as the template the
  meta-labeler and soft classifier should reuse — not Option B's own parity status, which is now
  moot. See `PRODUCTION_TRIAGE.md` row 10.
- **Historical-replay capability already exists, just SC-hosted, not standalone**: `BackTesterStudy`
  (`src/BackTesterStudy.cpp`) runs the *real* `RiskManager`/gate/sizing/barrier code under Sierra
  Chart's own replay mechanism, with a genuine `SCStudyInterfaceRef` and live `InferenceManager`
  singleton — no mocking required, unlike a from-scratch standalone harness would need. Separately,
  Sierra Chart's **Simulated Trading** mode runs the unmodified live trading study against
  paper-routed orders (it does not know the orders are simulated) — this is `PRODUCTION_TRIAGE.md`
  §0's "Paper trading" pipeline stage. Open question, not yet verified: does `BackTesterStudy`
  consume the same cached `.context`/`.alpha` files the Python twin (`backtest_runner.py`)
  processes, or does it regenerate the FlatBuffers event stream fresh from Sierra Chart's own raw
  historical bars during its own replay session? This determines whether feeding identical inputs
  to both sides (for the row 5/7 parity work below) is free or needs its own check first.
- **Refinement, confirmed by user 2026-08-24**: `BackTesterStudy` doesn't only run isolated native
  C++ code during replay — it exchanges ZMQ messages with a **real-time backtester** on the Python
  side (the live wire protocol, fed replayed rather than live data; distinct from the *offline*
  `backtest_runner.py`). This is the mechanism that should verify the **soft classifier's and
  meta-labeler's** own live deployment (redirected from Option B, per the training-time-only
  correction above): a **dual-path comparison mode**, not a new standalone script. For every
  historical event during a `BackTesterStudy` replay, log both (a) the native C++ path's output
  (the one that runs live) and (b) what the real-time backtester returns over ZMQ for the same
  event (the real, authoritative Python model — no reimplementation risk for whatever crosses this
  path), and diff across the full replay. Two things unverified before this can be built: (a)
  whether this dual-path logging exists yet (probably not — new, small, well-scoped work), and (b)
  whether the real-time backtester currently serves anything besides Transformer predictions over
  ZMQ — if Transformer-only today, extending it to also serve the two new classifiers is itself a
  prerequisite.

## 3. Work required — three pieces, plus a deferred cleanup

**Confirmed 2026-08-24: each new classifier's model class is decided independently** (soft
classifier vs. meta-labeler may differ), constrained only by requiring a clean, verified C++
deployment path. Default to reusing Option B's exact pattern (a `ClassifierParams`-style config
section + a small dot-product-shaped inference function) if a classifier's chosen model is
linear/logistic or similarly simply-parameterized. If it isn't — this codebase already knows what
that costs: Option B's own GBT candidate won empirically but couldn't ship, because the current
C++ shape is logistic-regression-only — then designing and golden-vector-testing a new C++ serving
shape is itself scoped, budgeted work for items 2/3 below, not an assumption.

1. **Extend `HmmStateAgeUs()` with a decay function** — same design as `PredictionAgeUs()`'s
   decay (companion spec), applied to the 7 existing derived scalars before they reach the
   meta-labeler. Lower lift than the Transformer side: the continuous age getter already exists
   here, only the decay function itself is new. **Do not invent the decay time constant** — same
   discipline as the companion spec: derive it from this system's own data (in this case, the
   actual inter-HMM-update interval distribution, gated by Mahalanobis-distance-change events per
   the user's own description of the trigger condition) before choosing a form/parameter.
2. **Wire the soft/gate classifier live**: consumes its own (TBD, see `lbrnet` spec Section 2)
   `.context`-derived feature set — explicitly NOT the HMM's live output. Runs after the existing
   rule-based hard gate, produces a continuous danger score (not just a binary veto) that is
   itself consumed downstream by the meta-labeler.
3. **Wire the meta-labeler live**: consumes the Transformer's own time-decayed side/confidence
   signal (`PredictionAgeUs()`, companion decay spec — **now the meta-labeler's dependency, not
   Predator Fusion's**, per Section 1's correction), the soft classifier's score, the decayed
   HMM-derived scalars from item 1, and the existing sizing multipliers (`MahalanobisSizingCap()`
   etc.) as inputs — outputs size (size=0 = no trade). Runs every tick, independent of the
   Transformer's own update cadence, which is the entire reason it lives in C++ rather than Python
   (see the design discussion this session — a Python-side meta-labeler would inherit the
   Transformer's own infrequent, event-driven cadence, defeating the point of tick-level sizing
   responsiveness). Predator Fusion is **not** an input here — its role is training-time-only
   (Section 1, 2).

**4. Deferred cleanup, only after item 3 ships and is validated live**: retire
   `TurtleSoupFusion.h`'s one live call site, `src/TripleScreen3.cpp:1267`'s
   `EvaluateTurtleSoupOptionA()` — a confirmed performance win (removes per-tick pattern-geometry
   evaluation from the hot path). Do this last, not first: the meta-labeler must be proven working
   before the only existing live pattern signal is removed. Tracked as `PRODUCTION_TRIAGE.md`
   row 10.

## 4. Train/serve parity — a real but now well-bounded surface, not the sprawling one first assumed

**Corrected twice over, 2026-08-24 — the actual count is three, not five.** The original framing
counted the HMM, the Transformer, Predator Fusion Option B, the soft classifier, and the
meta-labeler as five Python-trained/C++-deployed components. Two corrections since: (1) the
Transformer doesn't belong on this list at all — it stays on the Python side (`MTS`) and talks to
C++ over FlatBuffers/ZMQ (RPC), no C++-side port to verify agreement against; (2) Predator Fusion
(Option B included) is confirmed training-time-only — it never runs live, so there's no live C++
pattern detector to keep in sync with anything either. **That leaves three genuinely
C++-deployed ML components**: the HMM, the soft classifier, and the meta-labeler — all resting on
the same still-unverified foundational capability (`PRODUCTION_TRIAGE.md` rows 5/7).

**Refined scope of what row 5/7 actually needs to check, 2026-08-24**: `.context`/`.alpha` are
themselves C++'s own cached snapshots of the same FlatBuffers events that flow live over ZMQ — the
standalone Python backtester consumes C++'s own output directly, not an independent reimplementation
of feature computation. So feature/observation parity is a non-issue by construction. The entire
remaining risk is narrower than "does the whole pipeline agree" — it's specifically whether each
side's *decision logic* (gates, barrier computation, sizing formulas) produces the same output
given inputs that are already guaranteed identical — pattern detection no longer belongs on this
list at all, since it's training-time-only Python, not a live cross-language concern. That's a
smaller, sharper target, decomposable into per-component golden-vector tests rather than one
monolithic test.

**Each of the two new classifiers needs its own golden-fixture parity test** (Python-trained model
output vs. C++-deployed inference output, identical inputs) before either is trusted live —
the same class of requirement Option B's own header comment first named for itself (now moot for
Option B specifically, since it won't be live), applied here to the two components that actually
will be, with a concrete, working template to copy (Section 2's `ClassifierParams` pattern) rather
than an unspecified requirement. This is not a new class of risk, just more instances of a risk
this project has already named and not yet closed.

## 5. Non-goals

- Not deciding the soft classifier's or meta-labeler's exact model class/architecture — that's a
  Python-side training decision, owned by the `lbrnet` companion spec.
- Not resolving `PRODUCTION_TRIAGE.md` rows 5/7 directly in this spec — flagged as a hard
  prerequisite, tracked and owned there.
- Not picking the HMM-scalar decay time constant now — explicitly deferred to empirical
  derivation, same discipline as the Transformer-signal decay spec.
- Not implementing the full K-length posterior vector plumbing — the existing 7 derived scalars
  are the working assumption unless a specific decision-relevant gap is identified (none has been
  yet; see the `lbrnet` spec Section 3).

## 6. Suggested next artifact: an explicit data-flow diagram

This architecture has gotten genuinely intricate over the course of design discussion — two new
classifiers, three decay mechanisms (Transformer signal, HMM state, and whatever the soft
classifier's own freshness needs turn out to be), a cascade of gates, cross-feeding scores. Before
implementation starts, this deserves one explicit diagram or table (who feeds whom, what decays,
what's deliberately kept independent) as a first-class artifact — not left implicit across two
specs and a chat transcript. Recommend building this before Section 3's implementation work
begins, not after.

**Reuse `BackTesterStudy`/Sierra Chart replay for the parity checks, don't build a new harness**:
Section 2 already confirms real gate/sizing/barrier code (and, once deployed, both new classifiers)
can be exercised against historical data via Sierra Chart's own replay mechanism, with a genuine
`SCStudyInterfaceRef` and `InferenceManager` — no mocking needed. `PRODUCTION_TRIAGE.md` row 5/7's
per-component parity tests should run through this existing mechanism rather than a new standalone
CLI harness.

## 7. Cross-reference

- Live architecture overview: `/home/rcruz/devel/VSCode/PRODUCTION_TRIAGE.md` §0, §1 rows 5/7, 10
  (Predator Fusion C++ retirement, now sequenced *after* this spec's item 3, not before), and 11.
- Python-side training design: `lbrnet/docs/superpowers/specs/2026-08-24-two-classifier-risk-
  sizing-architecture-spec.md` (Section 3.1 owns the model-class-independence decision this spec
  implements on the C++ side).
- Transformer-signal decay mechanism (`PredictionAgeUs()`), the direct dependency for item 2
  above: `2026-08-24-predator-fusion-transformer-signal-decay-spec.md` (same directory).
- Model-class deployment precedent (Section 2/3): `include/TurtleSoupFusion.h`,
  `tests/cpp/TestTurtleSoupFusion.cpp`, `docs/superpowers/specs/2026-08-16-ects-prefix-training-
  infrastructure-spec.md` (rejected m2cgen/embedded-runtime approaches); Python side:
  `lbrnet/lbrnet/scripts/train_turtle_soup_classifiers.py`.
- Twin-agreement foundation this spec's parity tests depend on:
  `docs/ADR/execution_correctness_findings_spec.md:72`.
