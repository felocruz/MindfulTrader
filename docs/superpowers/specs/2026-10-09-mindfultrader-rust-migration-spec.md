# MindfulTrader → Rust / Monorepo Migration — Spec

**Companion plan**: `docs/superpowers/plans/2026-10-09-mindfultrader-rust-migration-plan.md` carries
status, execution phases/stages/gates, infrastructure mechanics, and port-by-port methodology. This
spec carries vision, decisions, target architecture, and per-component design — the "what and why",
not the "what's done and in what order". Read the plan's §0 for current status before trusting
anything below as "not yet started" — this spec itself is not a status tracker and will not be
re-edited just because a port landed.

**This spec, together with its companion plan, is now the single authoritative pair of documents for
this entire initiative.** They replace five separate, drifting documents (deleted from
`docs/superpowers/`, content folded in, full history still in git): `2026-10-07-mindfultrader-monorepo-
consolidation-spec.md`, `2026-10-07-rust-hmm-lifecycle-spec.md`, `2026-10-08-monorepo-infrastructure-
guide.md`, `2026-10-08-monorepo-rust-adoption-roadmap.md` (plan), `2026-10-09-cross-repo-naming-schema-
coherence-audit.md`. They were also, briefly, a single combined "master spec & plan" document
(2026-10-09, same day) before being split back into this spec/plan pair per the repo's own established
convention (a spec paired with a separate plan) — the single-document version was an initial
misunderstanding of what was being asked for, corrected the same day, no content lost either time.
**Reason for the original five-document consolidation (operator, 2026-10-09):** five separate documents
on one initiative had already produced real cross-document drift (the same status having to be updated
in two places, decisions duplicated, status sometimes stale in one copy) — this pair of documents is
precisely so that stops happening. **When this spec's content changes, it is the only place that needs
updating; do not open a new sibling spec for a sub-topic of this initiative — add a section here (or to
the plan, if it's execution/status content) instead.**

**Last updated**: 2026-10-09.

---

## 1. Vision & scope

### 1.1 Why

Opened 2026-10-07 from a brainstorm comparing this system's four-repo layout against `Atratus`
(`/home/rcruz/devel/VSCode/Atratus`, a single repo holding its schema, native code, Rust, and Python
GUI). Atratus's transferable ideas: a monorepo; a pure Rust core behind a C-ABI; an in-process Rust
HMM; PyO3 for the GUI data path. **Atratus is a separate project — never a dependency.** Where its code
is a useful reference (its cross-build toolchain, its ZMQ ring, its HMM shape), copy it with a
provenance comment and own it here; never add a Cargo `path =` dependency, a symlink, or a `sys.path`
entry pointing into `../Atratus`.

### 1.2 Goal

One pure Rust crate (eventually, a small set of crates) owns every computation that currently has
train/offline/live as three separate implementations, consumed three ways: the Sierra DLL (live,
in-process, no network hop), `lbrnet` (training orchestration, via PyO3), and standalone tools (offline
artifact generation). The posteriors the Transformer trains on and the ones C++ sends it live come from
the same code — train/serve skew becomes impossible by construction, not just unlikely.

**Scope confirmed 2026-10-09 (operator): this is "all Rust," not "HMM plus observation_vector math."**
`RiskManager`/`PositionManager`/`ExecutionGate`'s own low-level computations are explicit, equal-footing
candidates for the same methodical port — see §8. There is no special caution tier for this code: this
system is not in production, so the discipline is identical everywhere in this initiative.

### 1.3 Non-goals

No change to the model family (diagonal Student-t HMM, weighted emissions), to K, to the observation
vector's dimensions, or to any threshold, as a *side effect* of porting — model changes are a separate,
explicitly-flagged decision when they happen. No Transformer change beyond where it reads HMM output
from. The merge (plan §1.2) is separately gated and does not change code or wire formats by itself.

### 1.4 What stays as-is

The execution layer's *orchestration* (not its low-level computations, see §8), the Triple Screens,
the Transformer, the labeler, and the GUI's display logic are not part of this initiative's current
scope. `BackTesterStudy`/`backtest_server`: every workstream gates on `.btst`-identical replays; the
acceptance gates in `docs/BACKTESTING_FRAMEWORK.md` apply unchanged.

---

## 2. Decision register (single source — do not duplicate this elsewhere)

| # | Decision | State |
|---|---|---|
| 1 | Monorepo at `VSCode/MindfulTrader/`; `MindfulTrader`→`cpp/`, `MTS`→`GUI/`; `Atratus/` stays separate | **ruled**, deferred own schedule (plan §1.2) |
| 2 | Sequencing: pre-work → merge → `hmm`/`obs` → `transport`; merge decoupled from Rust work | **ruled** (2026-10-08) |
| 3 | HMM lifecycle (train, posteriors, live) moves to Rust; ZMQ port 5561 deleted, not migrated | **ruled** |
| 4 | Observation pipeline moves to Rust; `ContextManager.cpp` itself ports later | **direction set**; dims-first order and numeric policy open |
| 5 | Transformer/HMM coherence rule: C++ HMM state authoritative, sequence-stamped; stale actions (inferred before the current regime began) are not acted on | **ruled**; wording to confirm (regime-epoch vs literal older-sequence, §5.5-1) |
| 6 | Execution-layer computations (`RiskManager`/`PositionManager`/`ExecutionGate`) are in scope, same discipline, no special caution tier | **ruled** (2026-10-09) |
| 7 | One `mts_ffi` staticlib (not per-subsystem `*_ffi` crates) | **ratified by implementation**, 2026-10-08 |
| 8 | `.context.parquet` written directly at collection time; `.alpha`/`TrainingEvent` drops its embedded `ObservationData` copy for a join key | **operator goal, 2026-10-09, not yet decided in full** — depends on `rust/hmm` landing first; §5.4 |
| 9 | `backtest_schema.fbs` merged into `mts_schema.fbs` | **done**, 2026-10-09 (§4.2) |
| 10 | `MTS_Envelope` → `Envelope` | **done**, 2026-10-09 (§4.2) |
| 11 | Numeric policy (fast-math or not, tolerances) | **open** |
| 12 | Manifest holds the regime-engine thresholds (vs. crate constants) | **open**, recommended: manifest |
| 13 | GUI env name (`mts-gui` proposed); whether lbrnet keeps the name `mts` | **open** |
| 14 | MindfulTrader git history scrub (force-push) vs. rotation-only, for the leaked API key | **open**, operator's call |
| 15 | A typed Rust safety gate (Atratus's `ladder_core::safety` pattern) | **deferred** — Sierra stays in the order path for now |

---

## 3. Target architecture

### 3.1 End state (post-merge) directory layout

```
VSCode/MindfulTrader/                      repo root = the monorepo (merge itself: plan §1.2, deferred)
├── rust-toolchain.toml                    ← pinned; must be at the repo root
├── rust/                                  ← THE Cargo workspace
│   ├── Cargo.toml  Cargo.lock  .cargo/config.toml
│   ├── schema/              mts_schema             generated FlatBuffers Rust + contract constants (pure)
│   ├── observation_vector/  mts_observation_vector  18 dims, FeatureScaler, Mahalanobis gate        (pure)
│   ├── hmm/                 mts_hmm                 Student-t HMM: train, filter, regime engine     (pure)
│   ├── transport/           mts_transport           every ZMQ socket                                (zmq)
│   ├── ffi/                 mts_ffi                 ONE staticlib + cbindgen header → the DLL
│   ├── py/                  mts_py                  ONE PyO3 extension "mindful_core" → lbrnet + GUI envs
│   └── tools/                mts_tools              bins: hmm_tool, obs_tool (train/posteriors/replay/verify)
├── cpp/          (today: MindfulTrader/) Sierra DLL; links rust/ffi
├── schema/       .fbs + regenerate_schema.sh (now also emits Rust)
├── lbrnet/       Transformer, labeler, training orchestration (Python); imports mindful_core
└── GUI/          (today: MTS/) Dash GUI (Python); imports mindful_core
```

**Pre-merge reality, today**: `rust/` lives inside `MindfulTrader/` directly (no `../` prefix in any
path), as a deliberate convenience — it is a top-level sibling of `cpp/`/`lbrnet/`/`GUI/`/`schema/` only
once the merge happens. **Open action item**: the merge's "move everything into `cpp/`" step must
explicitly carve `rust/` out, not sweep it along.

### 3.2 Runtime data flow

**Live, end state**: `ticks → Sierra → cpp adapter → mts_observation_vector (dims → scaler → gate) →
mts_hmm (in-process step) → HmmStateIndicator + the event (carrying the HMM output) → mts_transport PUB
→ lbrnet Transformer → action back through transport → PredictionState (paired by §5.5's coherence
rule)`; the GUI subscribes via the same Rust client. ZMQ port 5561 no longer exists; the observation
vector, the model, and the posteriors each have exactly one implementation.

**Offline, end state (goal, §5.4, not yet built)**: `raw ticks → EventDataCollectorStudy.cpp (in-process
mts_observation_vector + mts_hmm) → .context.parquet (full raw vector, canonical) + .alpha/TrainingEvent
(labels + HMM posteriors + join key back to .context.parquet) → Transformer training`. The two-phase
"collect, then separately run the HMM over historical data to fill in posteriors" workflow collapses
into one pass for newly-collected data; the separate pass remains available, intentionally, for
re-scoring already-collected data against a newer/retrained model.

---

## 4. Component spec: Schema

### 4.1 Generation mechanics

`schema/regenerate_schema.sh` generates C++, Python, and (as of 2026-10-09) Rust bindings from a single
source: `schema/mts_schema.fbs`. One invocation per language now (previously two for C++/Python/Rust
each, due to the now-removed `backtest_schema.fbs` split — see §4.2). `flatc` must match the vendored
C++ runtime headers' version exactly (`include/flatbuffers/base.h`) or C++ generation emits a downgraded
`static_assert` that breaks the build — the script refuses to run on a mismatch. Deploys to
`MindfulTrader/include/generated/` (C++), `lbrnet/lbrnet/generated/` (Python, shared with `MTS`'s
imports), and `MindfulTrader/rust/schema/src/generated/` (Rust) — all three paths already cross-repo,
no merge required for this to work.

**`mts_schema` crate specifics**: `#[path = "generated/mts_schema_generated.rs"]` with an `#[allow(...)]`
wrapper wider than the generic lint-suppressing pattern — this workspace additionally denies
`unsafe_op_in_unsafe_fn` and `clippy::unwrap_used` (neither is in the `clippy::all` group), and flatc's
generated code trips both; both must stay in the allow-list or `cargo build`/`clippy` hard-fail.

### 4.2 Naming & structural coherence (the 2026-10-09 audit's findings)

Triggered by the operator's own observation: "these projects were evolved incrementally... the
incoherence resulting from incremental/iterative spec is staring me right in the face." Five findings,
each given a disposition rather than left as a vague unease:

1. **`MTS_Envelope` was the only non-PascalCase table/struct/enum name in either `.fbs` file** (surveyed
   all 46 names, zero other outliers). **Fixed, 2026-10-09**: renamed to `Envelope` (not `MtsEnvelope` —
   the `MTS.Schema` namespace already makes an `MTS` prefix redundant). Real blast radius: 151
   occurrences across 13 hand-written files in three repos (`MindfulTrader`, `lbrnet`, `MTS` — the `MTS`
   GUI repo carried by far the largest share, since it consumes the full envelope protocol). Also fixed:
   `regenerate_schema.sh`'s own embedded C++ helper snippet and `self_test_schema_contract.py`'s
   expected-symbols list, both of which referenced the old name directly.
2. **"MTS" is not actually overloaded** — it is the correct acronym for "Mindful Trading System" (already
   documented elsewhere: the Sierra Chart study itself is named `Mindful Trading System` /
   `scsf_MindfulTrader`), used consistently in the schema namespace, the Rust crate prefix, and the
   system's own name. The only real collision — the `MTS` GUI repo's path sharing the acronym with the
   namespace it's a client of — is not a fresh decision: it is already **ruled** in plan §1.2 (`MTS`→`GUI/`).
   **Resolved**, no new action needed.
3. **The `backtest_schema.fbs` ↔ `mts_schema.fbs` include relationship forced two independent, opposite
   per-language codegen workarounds**: Python needed isolated staging (flatc re-stubs included types
   when generating into a shared directory) while Rust needed `--rust-module-root-file` (without it,
   cross-file references don't resolve at all, `E0433`). **Fixed, 2026-10-09**: merged the two files
   into one. Revised an earlier, wrong first instinct (fix only the generation flags, not the schema
   structure) after two things, both verified: the operator's "we've never run a backtest" point removed
   the one real objection to merging (neither `Envelope`'s `"LBRN"` nor `BacktestFrame`'s `"BTST"` file
   identifier was ever actually embedded by any real C++ write call — both were purely decorative,
   verified directly, not assumed); and testing the "narrow, Python-only fix" showed it doesn't work in
   isolation anyway (`--gen-onefile` only resolves cross-schema references if *both* schemas use it).
   Caught and fixed a genuinely pre-existing bug along the way: `rewrite_generated_python_imports.sh`
   never had a same-namespace rule for `MTS.Backtest` (only `MTS.Schema`/`MTS.Training`) — found because
   this was the first-ever real attempt to import `BacktestFrame.py` in this system's history. Bonus:
   Rust generation simplified from a 77-file `--rust-module-root-file` tree back to one clean
   `mts_schema_generated.rs` file, since there's no cross-file reference left needing the merge flag.
4. **`regenerate_schema.sh`'s organic complexity** (2,300+ lines; a documented `.bak_*` timestamp-backup
   convention this repo's own infra principles already flag as something to stop — "git is the backup").
5. **Already-resolved precedents, recorded so they are not re-litigated**: `mt_`/`MT_` → `mts_` (caught
   because it collided with vcpkg's own `-mt` suffix convention and MetaTrader 4/5); `observation` (bare)
   → `observation_vector` (no standalone meaning in this codebase — confirmed by finding that
   `ObservationData` is a real, distinct, already-taken schema struct name, which also means
   `observation_data` would have been the wrong rename target).

**Open follow-up, not yet done**: extending this audit to `lbrnet`'s and `MTS`'s own internal naming
conventions (C++ class names, Python module names) beyond what touches the schema.

### 4.3 DOD / performance findings (struct vs. table)

Atratus declares its hot-path numerical payload (`ObservationData`) as a FlatBuffers `struct` (fixed
layout, no vtable indirection), not a `table`. This codebase already does the same, and more
thoroughly: 5 structs today (`EventHeader`, `IndicatorState`, `ObservationData`, `AsymmetryContext`,
`ImbalanceObservationData`). Applying the same scan to our own 43 remaining tables surfaced two
genuine, not-yet-converted candidates: **`RiskGateContext`** (18 fields, all scalar) and
**`ImbalanceRiskGateContext`** (7 fields, all scalar) — structurally identical in shape to
`ObservationData`.

**Important correction, caught before acting on it**: the live per-tick C++ decision path does **not**
read `MTS::Schema::RiskGateContext` at all — `RiskManager`/`ExecutionGate` gate on
[LocalRiskContext](/home/rcruz/devel/VSCode/MindfulTrader/include/LocalRiskContext.h), a plain C++
struct with zero FlatBuffers involvement (already maximally DOD). `MTS::Schema::RiskGateContext` is
used only for serialization to `.lbr`/`.context` files. So the real beneficiary of a struct conversion
is bulk training-data I/O (this system's validated real dataset is 471.9M ticks), not live tick
latency — the justification matters for scoping the change correctly, not just doing it.

**Mechanical wrinkle, not a free keyword swap**: FlatBuffers `struct` has no default-value/optionality
syntax — every field must be explicitly set at construction, always. `RiskGateContext` is itself an
*optional field* inside its parent `MarketObservation` table — that optionality lives in the parent
table's vtable, not in `RiskGateContext` itself, so converting doesn't lose the "can be absent"
capability. Both tables have exactly one construction site each (`LBRFileManager.cpp`, via the
object-API `CreateX(fbb, XT*)` pattern).

**Status: proposed, not yet implemented.** Needs the same verification discipline as §4.2's fixes
(generate, build, test) before executing — see the plan's §1.1 (P-series pre-work items) for how
similar changes have been gated so far.

---

## 5. Component spec: HMM lifecycle (train → posteriors → live inference)

### 5.1 What exists today (verified 2026-10-07)

**Live path** (per HMM step, fired by the Mahalanobis significant-change gate in C++):
`HMMClient::RequestUpdateAsync` → DEALER → Python ROUTER (`lbrnet/backtest/backtest_server.py:598`;
`MTS/zmq_client.py:323` binds a second one — a real duplicate-bind bug, plan's P5) → `FeatureSpine`
pairs `MARKET_OBSERVATION` with `SYSTEM_STATE` by sequence id → burn-in → `LiveAgent.predict_hmm` →
`RegimeEngine.infer` → a `RiskStateUpdate` back to `HMMClient::HandleBinaryResponse`, which validates,
normalizes probabilities, and writes `HmmStateIndicator::SetState`/`ClimateIndicator::SetClimate`.

**What `RegimeEngine.infer` computes** (`lbrnet/lbrnet/models/regime_engine.py:792`), all to be ported:

| Output | Source |
|---|---|
| Model input | raw 18D observation, projected through `HMM_KEEP_DIMS` (`slice_for_gmm`; `fast_mean_rev_z` excluded), cast to float32 |
| Posterior | Hamilton (1989) forward filter in log space (`_hamilton_belief_update`): `log_pred = logsumexp_j(logA[j,k] + log_belief[j])`, add emission, renormalize; belief persists across calls |
| Emission | weighted Student-t (`_log_emission_and_delta`): `delta = Σ w_d (x_d−μ_d)²/σ²_d`; full log-density formula with D the true dimension (not Σw) and ν clipped to `[dof_min, dof_max]` |
| Tail diagnostics | `dof`, `mahalanobis = √delta`, `tail_weight = (ν+D)/(ν+delta)` |
| Derived | argmax state; normalized entropy; `confidence = p_max·(0.6+0.4·(1−H))`; `transition_risk = 1−A[s,s]`; `expected_duration = 1/max(transition_risk,1e-6)` |
| Climate | manifest profile lookup + overrides (Taleb fat-tail, Pareto mirror, burstiness EMA hysteresis, entropy-chaos override) |
| Model-risk stateful | variance ratio, black-swan streak, Amari FIM nearest-rival distance, duration tracking, entropy-drift alerts |

**Not part of the contract, not to be ported as-is**: `_project_probs_to_hmm_enums` (its own docstring
says its premise is broken); `cognitive_load`/latency (wall-clock-dependent, non-deterministic — §5.5-2).

**Offline path, confirmed two-phase (2026-10-09, directly relevant to §5.4's goal)**:
`EventDataCollectorStudy.cpp` writes `.alpha`/`TrainingEvent` records with the raw observation vector
populated but the `regime_prob_*`/`regime_confidence`/`regime_entropy` fields sitting at schema
defaults (reserved space, unfilled) — a separate, later pass
(`lbrnet/scripts/materialize_hmm_features.py::compute_context_posteriors`, confirmed via
`tests/test_attach_regime_to_event.py`) runs the HMM over the already-collected data and fills them in,
writing `<name>.context.posteriors.npy` (measured: 21,967,249 rows, record =
`ts_us i8, bar_index i4, dominant_regime i1, p_coiled/p_gaussian_stable/p_gaussian_fragile/p_pareto f4,
mahal_distance f4, obs_vec f4[18]`).

**`ContextManager` producer-side ingress ownership (folded in from two deleted docs,
`docs/CONTEXTMANAGER_USAGE_ASSESSMENT.md`/`docs/CONTEXTMANAGER_ASSESSMENT_SUMMARY.md`, dated March
2026 — re-verified still accurate 2026-10-09 via fresh grep, not trusted as-is)**: statistical ingress
into `ContextManager` is split by owning timeframe, not a single generic setter —
`TripleScreen2.cpp` → `SetWaveContext(std::move(ctx))` (owns `volatility`/`efficiency`),
`TripleScreen3.cpp` → `SetRippleContext(std::move(ctx))` (owns `relRange`/`velocity`) and
`SetNormalizedAnchors(std::move(anchors))`. `EventDataCollectorStudy.cpp`/`SCStudies.cpp` both call
`ContextManager::CheckAndTriggerHMM(...)` (collection vs. live boundary, `isDataCollection` flag) and
`EventDataCollectorStudy.cpp` separately calls `AddToTrainingEventFB(...)` for payload enrichment. The
legacy generic `SetStatisticalContext(...)` has zero production call sites (confirmed). This is
background for whoever eventually Rust-ports `ContextManager` (§5.2) — it is the real shape of the
C++-side API that port must replicate, not a single `Update(ctx)` entry point. Note: the two deleted
docs' own "(16D)" dimensionality claim was stale even before deletion — current is 18D (§6). The
authoritative, still-current `ContextManager` architecture doc is the workspace-shared
`/home/rcruz/devel/VSCode/docs/ROADMAP_CONTEXTMANAGER_REFACTOR.md` (not a MindfulTrader-repo-local
file) — it already covers this ownership split in more depth but doesn't name the
`CheckAndTriggerHMM`/`AddToTrainingEventFB` call sites this paragraph adds.

### 5.2 Target architecture

```
rust/hmm/        mts_hmm        pure crate: model, emission, filter, tail diagnostics, regime engine
                                 (classification + model-risk state), EM trainer, artifact I/O
rust/ffi/        (feature "hmm") hmm_init / hmm_step / hmm_reset, staticlib + cbindgen header
rust/py/         (submodule)     PyO3 for lbrnet: train, filter, batch_causal_filter, load/save artifact
rust/tools/      hmm_tool       CLI: train, posteriors, verify-golden, export
```

**Artifact (replaces `models/hmm_model.pkl`)**: a pickle cannot be read by Rust and is a poor deployment
format. Proposal: extend the existing `ModelManifest.json` (dims, `HMM_KEEP_DIMS`, feature weights, dof
bounds, thresholds the regime engine hard-codes today) plus a flat little-endian float64 weights file
with a header and a SHA-256 in the manifest — deliberately not `bincode` (Rust-only), so Python tools
can still read it with `numpy.frombuffer`. All dimension indices the regime engine reads (`_TAIL_INDEX_IDX`
etc.) come from the schema-generated dimension constants, never hard-coded — this repo has already been
bitten twice by misaligned per-dim arrays (`FeatureScaler`).

**Live (DLL)**: `hmm_step(obs18_f32, ctx) -> HmmStep` called synchronously from the existing
HMM-trigger site, ~µs for K=4, D≈17. Allocation-free: all buffers sized once at `hmm_init` from the
manifest, never per step. Every `extern "C"` function `catch_unwind`-guarded.

**Transformer coupling, inverted, one-way**: C++ publishes the HMM output with each event on the
existing event stream (additive schema fields, non-breaking); the Python Transformer reads regime
features from the event instead of running its own `RegimeEngine`. Coherence rule (§2 item 5, ruled):
the C++ HMM state is authoritative, sequence-stamped; a Transformer action carries the sequence_id it
was inferred from; C++ pairs the action with the HMM state of that sequence_id; an action inferred
before the current regime began (the last flip) is stale and not acted on, but the native floor is
never suppressed by a stale/missing model action (consistent with the trap-detection arbitration rule).

### 5.3 Parity strategy

**Inference** must match Python bit-for-bit-ish (golden file, stated tolerance, real observations)
before any Python HMM code is deleted. **Training** cannot be bit-exact (EM is non-convex); its gate is
statistical — same `.context`, same init, held-out log-likelihood/BIC, state profiles, and posterior
agreement within tolerance. (Stage-by-stage execution of this strategy: plan §4.)

### 5.4 Extension (2026-10-09, operator goal — not yet decided in full, work to begin after plan §4's Stage B-C)

**The goal, stated plainly**: `EventDataCollectorStudy.cpp` writes `.context.parquet` directly during
collection (replacing today's two-step "C++ writes `.context` as a FlatBuffers stream → a separate,
later Python pass materializes it to Parquet" — this repo already writes Parquet directly from native
code elsewhere, `tools/scid_processing/scid_to_ticks_parquet.cpp`, so there is real precedent).
`.alpha`/`TrainingEvent` keeps exactly "whatever the Transformer needs from the HMM" (the already-
existing posterior fields) plus a join key (`sequence_id`), and drops its own embedded
`observation: MTS.Schema.ObservationData` copy, joining back to `.context.parquet` instead when the
training pipeline needs the raw vector alongside labels. This mirrors a pattern the schema already
uses elsewhere (`BacktestFrame`'s own `run_id` field is documented as "redundant with the payload's own
run_id; enables random-access correlation... without deserializing the union" — carry a join key, don't
duplicate the payload).

**The operational win on top of the architectural one**: §5.1 already confirms today's offline pipeline
is two-phase. Once the HMM is in-process at collection time (§5.2), this collapses to one pass:
`.alpha`/`TrainingEvent` records get written *fully populated* — observation vector (or its join key)
and posteriors together — the first time, for newly collected data. The separate offline pass doesn't
disappear as a capability — re-scoring already-collected data against a newer/retrained model is a
real, legitimate, intentionally-occasional operation — it just stops being a *mandatory* step in
routine collection.

**Three distinct consumers of `ObservationData` today**, found by direct consumer survey 2026-10-09 —
each needs its own answer, this goal does not resolve all three uniformly:

1. **HMM live inference** (today: `MarketObservation` over ZMQ port 5561 to a separate Python process)
   — cleanly eliminated by the in-process Rust HMM. No remaining question.
2. **Training data** (`.alpha`/`TrainingEvent`'s embedded `observation` field) — this section's goal:
   de-duplicate against `.context.parquet` via a join key instead of an embedded copy.
3. **Live GUI display** (`MTS/zmq_client.py` genuinely decodes `MarketObservation` live today —
   `GetRootAs`, aligned with `SystemState` by `sequence_id`, feeding
   `lbrnet.feature_spine.MarketObservationData` — confirmed via direct code read). **Not resolved by
   this goal.** If `MarketObservation` disappears from the live wire protocol entirely, the GUI loses
   whatever it currently shows from that data. Two ways this resolves, not yet decided: either something
   still publishes the vector live for display purposes even though the HMM no longer needs it
   transported, or the GUI's display gets redesigned to show something else (e.g., posteriors only) —
   the second is a product decision about what the GUI shows, needing an explicit operator call before
   `MarketObservation` can be removed from the live schema.

**Status: operator goal, confirmed directionally correct, not yet decided in full.** Explicitly depends
on §5.2's in-process Rust HMM landing first — writing `.alpha` records "fully populated the first time"
requires the HMM to already be callable in-process from `EventDataCollectorStudy.cpp`, which doesn't
exist yet. The GUI question (consumer 3) should be resolved before any schema change removes
`MarketObservation` from the live wire protocol, independent of this goal's `.alpha`/`.context.parquet`
changes, which don't depend on that resolution either way.

### 5.5 Open questions

1. "Older-sequence" in the coherence rule (§2 item 5) — confirm it means older than the regime-epoch
   start, not older than every newer sequence (generic ageing is already handled by
   `SemanticFreshnessDiscount`).
2. `cognitive_load`: deterministic replacement, or drop (wall-clock-derived today).
3. Entropy normalization function and the unconditional-state set — not read in full yet; the plan's
   Stage A golden will pin them.
4. `HMM_MODEL_INPUT_DIM` value and `HMM_BURN_IN` — read from code in the plan's Stage A.
5. Whether `meta_hmm_control_plane.py`/`smart_tuner.py`/HPO call `train_student_t_hmm.py` as a
   subprocess or import the model — decides how thin the Python wrapper can be.
6. `train_student_t_hmm.py`'s `--expected-schema-version` default is **230** while the current schema is
   **240** (noted, not investigated) — the existing training gate may already be stale.
7. The four-name probability sidecar (`p_coiled`, `p_gaussian_stable`, `p_gaussian_fragile`, `p_pareto`)
   bakes in K=4; keep for compatibility initially, generalize later.
8. CPU vs. GPU training time at 22M rows.
9. Whether the climate/threshold constants hard-coded in `regime_engine.py` move into the manifest
   (recommended) or stay as crate constants.

---

## 6. Component spec: observation vector pipeline

### 6.1 Dim-tracing reference (read-only trace, 2026-10-07 — where the Sierra coupling actually is)

Operator question: "we may want to get `StudyHelperFunctions.cpp` out of the way of the dims
computations." Finding: **the dim code is a small, separable slice of that file; most of its `sc.` use
is unrelated to dims.** The dim-feeding functions sit in ~800 lines (106 `sc.` occurrences) of a
2,881-line file with 467 total `sc.` occurrences — the other ~360 are Raschke/structure-detection/draw
code, nothing to do with `ObservationData`. The real Sierra boundary is `TripleScreen{1,2,3}.cpp` plus
these wrapper functions, **not** `ContextManager.cpp` (whose `BuildObservationVector()` is already
Sierra-free in practice — only 6 lines in the whole 1,510-line file touch `sc.*`).

| Wrapper (`StudyHelperFunctions.cpp`) | Pure engine it already uses | Remaining Sierra coupling |
|---|---|---|
| `CalculateLogScaleRatio` | `ComputeBipowerVariation` | bar-array pull (near-thin) |
| `CalculateHurstExponent` | `DfaHurstExponent.h` | bar pull + 4 persistent-state ops |
| `CalculateFisherInformation` | `ComputeFisherInformation` | bar pull + 1 persistent |
| `CalculateFractalDimension` | `SevcikFractalDimension.h` | bar pull + 1 persistent |
| `CalculateLogScaleExpansionRatio` | `ComputeBipowerVariation`, `ComputeBurstinessIndex` | bar pull + 1 persistent |
| `CalculateAmihudIlliquidity` | `cfc::ComputeAmihudIlliquidity` (final aggregation only) | **inline accumulation loop + live-bar term still in the wrapper** (68 lines) |
| `CalculateLiquidityFragility` | `LiquidityFragilityEngine.h` | bar pull + cold-start policy + 1 persistent |
| `CalculateMeanReversionSpeed` | `MeanReversionCalculator.h` | bar pull + 1 persistent |
| `CalculateRealizedKurtosis`/`CalculateSkewness` | `RobustMoments.h` | bar pull; may no longer feed the vector (superseded by the activity-clock path per `ContextManager.cpp:457`) — verify |
| `CalculateAdaptiveObservationWindow`/`CalculateFisherAdaptiveWindow` | none (window sizing) | 2 persistent ops each — these size the windows the dims use, so they are dim *inputs* |
| `CalculateVolConvexity` | none | feeds `RiskGateContext`, **not** `ObservationData` — same extraction applies, different consumer |

Two dims already fully Sierra-free (bypass this file entirely): `lempel_ziv` (`InformationEngine`),
`tail_index` (`TailRiskEngine`). `fast_*` dims come from `ActivityClockManager`'s `ImbalanceBarEngine`
(also pure). **Not yet traced to a function**: `relative_range`, `recurrence_rate`, `micro_asymmetry`
(reads `Subgraph_MicroAsymmetry[sc.Index]`, a Sierra subgraph array used as storage — a second kind of
coupling besides `sc.GetPersistent*`), and exactly where `burstiness_index` is finally assembled.

**Dead code, deleted 2026-10-09**: `ComputeHurstFromReturns` (the old R/S Hurst, ~105 lines) had zero
callers anywhere (`src/`, `include/`, `tests/`, `tools/`) since DFA replaced it — deleted under the
standing not-in-production rule (commit `6343b41`), after a fresh full-repo usage search per the Code
Safety Rules.

(Current port status and the port-by-port execution methodology both live in the plan's §5 — this
section is the architectural trace only, and does not change as individual dims get ported.)

---

## 7. Component spec: transport (ZMQ → Rust)

**Goal**: one Rust crate owns every ZMQ socket for all three languages — libzmq compiled into the Rust
staticlib (no more vcpkg `libzmq-mt`/libsodium link in the DLL); Python reaches it through PyO3 with the
GIL released. Payloads stay FlatBuffers; ports and socket patterns stay as they are, so each port flips
independently against an unchanged peer.

**Verified socket inventory (2026-10-07):**

| Port | Pattern | C++ DLL | Python | Migration shape |
|---|---|---|---|---|
| 5555 | PUB | binds (`TransportStream`) | SUBs connect (GUI, lbrnet) | One-way. Ring + background thread, drop-oldest. **First.** |
| heartbeat | SUB | connects (`AIHeartbeatMonitor`) | binds PUB (`MTS/system_orchestrator.py:243`) | One-way receive. **Second.** |
| 5560 | REP | binds (`SystemOrchestrator` control) | REQ connects | Request/reply — needs `submit`/`poll` shape (below) |
| 5562 | REP | binds (`SystemOrchestrator` mental profile) | REQ connects (`action_plan.py:190`) | Request/reply |
| 5558 | REP | binds (`TradeExecutionServer`) | REQ connects (`BacktestLiveAgent`) | Request/reply, backtest-gated |
| 5561 | DEALER→ROUTER | connects (`HMMClient`) | binds ROUTER (×2 today — a real duplicate-bind bug, plan's P5) | **Deleted** by §5's in-process HMM, not migrated |

**The one real design problem**: today's request/reply sockets (HMM inference, trade RPC, control
handshake) are synchronous on the Sierra thread. Across a C-ABI this must become non-blocking
`submit(request) -> ticket` / `poll(ticket) -> Option<bytes>`, with existing timeouts enforced inside
Rust, so the DLL never blocks inside Sierra's update call.

**Intended migration order, with a gate per port** (flip one port; the peer is unchanged; revert = flip
back): (1) `mts_transport`/`mts_ffi` skeleton — gate: DLL builds, imports unchanged, Sierra loads it;
(2) 5555 PUB — gate: stale-heartbeat detection fires at the same threshold; (4) Python side (PyO3
client replaces `transport.py`/`zmq_client.py` internals) — gate: existing Python tests green; (5)
5560/5562/5558, one at a time, `submit`/`poll` shape — gate per port: handshake + round-trip against
the unchanged peer, full `.btst`-matching replay for 5558; (6) 5561 — not migrated, removed by §5; (7)
remove the vcpkg libzmq/libsodium link, delete the old socket code (not behind a flag — not in
production).

**Non-goals**: no schema change, no protocol redesign, no change to port numbers or message ordering.

---

## 8. Component spec: execution-layer computations

**Scope confirmed 2026-10-09 (operator)**: this initiative is "all Rust," not "HMM plus
observation_vector math." `RiskManager`/`PositionManager`/`ExecutionGate`'s own low-level computations
are explicit, equal-footing candidates for the same methodical port.

**Candidates surveyed** (all pure, zero Sierra Chart coupling, confirmed by direct code read):
- `Scoring.h`, `LocalRiskContext.h` (the real live risk struct, §4.3), `TailRiskEngine.h` — same shape
  as the `observation_vector` candidates already ported; stateless-function pattern applies directly.
- `TradeDecisionEngine.h` (560 lines) — already its own shadow-mode-only layer by design (PAER §9 Phase
  1: "Computes shadow metrics on every trade opportunity WITHOUT changing execution behavior... No
  execution logic reads these values"). Arguably the safest starting point in this whole component,
  since it already cannot affect real decisions.
- `KellyCalculator.h` — stateful (a trade-history circular buffer), a different porting shape: needs
  the init/step/reset opaque-handle pattern already planned for `mts_hmm`, not the stateless-function
  pattern used so far.

**No special caution tier for this code**: corrected on exactly this point earlier in this initiative
after initially (wrongly) reaching for "real capital is at stake" framing — there is no live capital,
this system is not in production, so the discipline is identical to every other port in this document:
parity-test, shadow-wire, live-confirm, cutover.

**Status**: scope confirmed, not yet sequenced relative to `mts_hmm`/`mts_observation_vector`'s own
remaining dims (tracked in the plan's §1.3 workstream table). Which crate(s) these land in (a new
`rust/risk` sibling vs. folded into `mts_hmm`) also not yet decided.

---

## 9. Risks

1. **The central Rust-in-Sierra assumption was unproven until W0s** — now resolved (plan §2).
2. **Moving targets**: the observation-vector dims and the HMM (K, features, the unmet fat-tail
   sign-off) are still changing; ports must be manifest/golden-driven so a change lands in one place.
3. **Unvalidated references**: the offline reconstruction has never been byte-validated against a
   genuine Sierra-collected file at the current schema version (blocked on a v240 comparison file not
   existing yet) — Rust ports will match the C++ reference, not necessarily live Sierra, until that
   exists.
4. **Two libzmq copies in one DLL** (expected during the transport transition, untested) — the plan's
   §6.5 has the interim plan; resolve before `mts_transport`'s W9.
5. **Numeric policy** (possible `-ffast-math` in the DLL build vs. none in tools/Rust) — unsettled, §2
   item 11.
6. **A long program touching the same files repeatedly** (`ContextManager`, `HMMClient`,
   `SystemOrchestrator`, the schema, the event stream) — mitigation: every stage reversible until its
   own deletion step, shadow modes throughout, one change at a time.
7. **This is re-platforming, not edge** — this is a standing caution, not a status: do not let this
   program's real progress displace the actual production-readiness gaps it does not itself close (see
   `PRODUCTION_TRIAGE.md`'s own Vision section).
8. **Sierra-process crash risk from Rust**: a panic or allocation on the hot path is a Sierra crash, not
   a log line — `catch_unwind` everywhere, preallocated buffers, an allocation-tripwire test around every
   `*_step` function.

---

## 10. Not yet analyzed (real gaps in this picture, not silently skipped)

- **Labeler ↔ native `StructureTest` parity** (`lbrnet/labeling/triple_barrier_scanner.py` vs. the C++
  detector, a mandated co-evolution per the trap-detection rules) — a shared Rust core could remove
  this duplicate too; not examined.
- **GUI beyond ZMQ** (Firestore lifecycle, websocket broadcaster, the 3,826-line
  `system_orchestrator.py`).
- **Windows-side deployment**: DLL deploy, Sierra paths, the two live JSON config files outside git
  (`/mnt/c/Trading/config/`) that carry migrated thresholds and won't travel to a new machine without a
  manual sync.
- **CI today**: whether any exists for the four repos currently (Atratus's is Python-only).
- **Hot-path budget**: no per-tick latency measurement exists yet; needed before `mts_observation_vector`/
  `mts_hmm`'s in-process shadow-mode stage.
- **PyO3 build/packaging**: Atratus installs its extension by hand (editable `maturin`); this needs a
  scripted build (the plan's §6.3).
- **Extending the naming/coherence audit (§4.2) beyond the schema surface** to `lbrnet`'s/`MTS`'s own
  internal conventions.

---

## 11. Documentation sync, when pieces of this land

- "Rust is a build prerequisite for `cpp/`" → the four mirror docs, once `mts_ffi` is load-bearing for
  a real build (not yet — currently opt-in via `MTS_WITH_RUST`).
- The artifact convention change (`models/hmm_model.pkl` → manifest + weights) → the four mirror docs,
  once §5 ships.
- A `PRODUCTION_TRIAGE.md` row for this whole program, added when work actually starts in earnest
  (Triage Protocol rule 7) — not yet added; this is still pre-work/early-port territory.
