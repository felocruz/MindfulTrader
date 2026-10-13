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

**Last updated**: 2026-10-11.

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

**Scope stated precisely, 2026-10-10 (operator): three pillars, one hard rule.** Rust owns (1) trade
execution + risk management (§8), (2) the HMM + observation vector (§5/§6), and (3) **indicator
computation** — the actual math behind every indicator (MACD, RSI, Kangaroo Tail, Turtle Soup, NR7,
stochastic, etc.), not just the 18D observation vector's dims. C++ owns exactly the ACSIL studies —
the thin orchestration layer that reads `sc.*` data and calls into Rust with plain values. **The hard
rule: Rust code never references any ACSIL function or variable, full stop** — the same "pure core,
thin edges" discipline already proven for `observation_vector`, now stated as universal, not
crate-specific. Component spec for pillar 3: §9 (opened 2026-10-10, scope only — design not yet
started).

**Naming ruling (operator, 2026-10-11): no `mts_` prefix on any Rust crate, directory, or file.**
"We 'know' the overall system is named MTS — no need to pollute dir/file names with it." Every
crate dropped the prefix (`mts_execution`→`execution`, `mts_observation_vector`→`observation_vector`,
`mts_schema`→`schema`, `mts_ffi`→`ffi`); the planned HMM crate is named `student_t_hmm`, not the
genuinely-ambiguous bare `hmm` (operator, 2026-10-10). Scope of this ruling: Rust package/crate
names, directory names, Rust-side doc-comment/prose references to them, and (ruled 2026-10-11,
resolving the question originally left open) the exported `extern "C"`/C-ABI symbol names too —
`mts_observation_vector_*` → `observation_vector_*` (12 functions), `mts_abi_version` → `abi_version`,
the `MtsStatus` enum → `Status`. This is a no-op for every C++ call site's actual meaning: cbindgen's
`namespace = "mts"` (`cbindgen.toml`) already wraps every symbol for C++ callers
(`mts::observation_vector_sevcik_fractal_dimension(...)`, not a bare global), so the `mts_` prefix on
the symbol name itself was redundant namespacing, not load-bearing C-ABI uniqueness — the namespace
already does that job. The 3 live C++ call sites (`ContextManager.cpp`/`StudyHelperFunctions.cpp`/
`TripleScreen2.cpp`) plus the cross-language parity test (`tests/cpp/test_rust_observation_vector_
parity.cpp`) were updated in the same change and re-verified (33/33 parity assertions still pass).
**Still explicitly out of scope, left as-is, different naming domain**: CMake option/variable names
(`MTS_WITH_RUST`, `MTS_FFI_LIB`); the shared cross-language schema file itself (`schema/mts_schema.fbs`,
`mts_schema_generated.h`/`.rs` — flatc's own naming, not a Rust crate); and the cbindgen `namespace =
"mts"` itself (that one stays — it's the actual C++-side project namespace, not redundant symbol-name
decoration, and removing it would make every one of these functions a bare global in the C++ global
namespace).
Verified: full `cargo test --workspace` + `cargo clippy --workspace --all-targets -- -D warnings`
clean after the rename; both C++ build paths (`MTS_WITH_RUST=OFF` default and `=ON` with the renamed
`ffi.lib` actually linked) rebuilt clean from scratch; the cross-language parity test recompiled and
rerun natively against the renamed symbols.

**Standing principle (operator, 2026-10-10): Rust's adoption is itself recent (opened 2026-10-07) —
any design spec or decision made before that date is presumptively open for rethinking in light of
it, not grandfathered in.** This already overturned one such decision: `docs/superpowers/plans/
2026-08-05-indicator-manager-dod-soa.md` had explicitly declared the C++ `IndicatorStore`/
`BaseIndicator`/`Indicator<T>` write-side compute engine "permanent" and scoped a full write-side
rewrite out as a deferred "Phase III" (user decision, 2026-08-05) — that plan is now marked
partially superseded (its own header), since pillar 3 above is exactly that rewrite, just in Rust
rather than C++ free functions. Apply the same scrutiny to any other pre-2026-10-07 "permanent"/
"intended end-state" claim found later — don't assume this was the only one.

### 1.3 Non-goals

No change to the model family (diagonal Student-t HMM, weighted emissions), to K, to the observation
vector's dimensions, or to any threshold, as a *side effect* of porting — model changes are a separate,
explicitly-flagged decision when they happen. No Transformer change beyond where it reads HMM output
from. The merge (plan §1.2) is separately gated and does not change code or wire formats by itself.

### 1.4 What stays as-is

The execution layer's *orchestration* (not its low-level computations, see §8); the Triple Screens'
and other studies' *ACSIL orchestration shell* (not the indicator computation they call into, see
§1.2 pillar 3/§9 — this line previously said "the Triple Screens... are not part of this
initiative's current scope" without that distinction, corrected 2026-10-10); the Transformer; the
labeler; and the GUI's display logic are not part of this initiative's current scope.
`BackTesterStudy`/`backtest_server`: every workstream gates on `.btst`-identical replays; the
acceptance gates in `docs/BACKTESTING_FRAMEWORK.md` apply unchanged.

### 1.5 Hardware context (set 2026-10-11) — read before preserving a memory-/CPU-conservative pattern by reflex

Operator context, recorded for every future porting decision in this initiative: nearly all of this
system's existing C++/Python (HMM, Transformer, data pipelines) was developed on a **10-year-old
Dell Precision workstation — 15GB RAM, a slow CPU, no GPU**. A new ML-capable workstation (Puget
Systems) has since lifted those constraints. This matters because several real, already-found code
patterns in this codebase look like deliberate architectural choices but may instead be narrow
workarounds tuned to that specific 15GB ceiling — concrete examples found this session, not
speculation:
- `test_student_t_hmm_predict_chunking.py` — chunked `predict()` was added specifically after
  unchunked `_posterior()` OOM-killed real sign-off workers (~1.7-2.2GB transient at N=13.5M).
- `estep_max_workers`-bounded E-step thread pool — explicitly guards against unbounded
  `executor.map()` over a whole chunk generator.
- `context_to_parquet.cpp`/`tools/market_data_replay/MarketDataReplay.cpp`'s shared
  `kChunkRows = 2'000'000` chunking convention, plus the latter's `--max-rss-mb` flag.
- CPU-only `joblib`/`loky` process parallelism throughout `train_student_t_hmm.py`, never GPU —
  and `student_t_hmm_gpu.py`'s own "20-33x slower than CPU" finding (§5.1a) was measured somewhere,
  but not necessarily on hardware representative of the new workstation; worth re-measuring before
  trusting it as a reason to drop that path, not just citing the old number.
- The orphaned (examples-only-referenced) `fast_ingestor.py`'s entire premise ("Handle 35M+ events
  without loading into RAM") is itself evidence of real historical memory pressure (§5.4a).

**The actionable principle for every port/refactor in this initiative, not a blanket directive
either way**: when a memory- or CPU-conservative pattern is found during porting, investigate which
of two things it actually is before carrying it forward unchanged.
- **Genuine architectural requirement — keep it**: e.g. bounded peak memory for determinism
  regardless of input scale, or a chunking boundary that correctly respects sequence/session
  edges (`test_student_t_hmm_predict_chunking.py`'s own "must respect true sequence boundaries"
  requirement) — these remain good engineering on any hardware, not a workaround.
- **A narrow workaround for the old 15GB ceiling — reconsider it**: e.g. an arbitrarily conservative
  chunk size, worker-pool bound, or batch size tuned down specifically to survive that machine,
  where a larger value would be strictly better (more throughput, simpler code, fewer chunking edge
  cases to get right) on the new hardware with no correctness cost.
Don't reflexively preserve either category by default — and don't reflexively strip memory-bounding
either, since §9's "no heap allocations in recurring ACSIL update paths" rule and similar
hot-path-performance constraints are independent of the old machine's RAM ceiling and stay regardless
of hardware.

---

## 2. Decision register (single source — do not duplicate this elsewhere)

| # | Decision | State |
|---|---|---|
| 1 | Monorepo at `VSCode/MindfulTrader/`; `MindfulTrader`→`cpp/`, `MTS`→`GUI/`; `Atratus/` stays separate | **ruled**, deferred own schedule (plan §1.2) |
| 2 | Sequencing: pre-work → merge → `hmm`/`obs` → `transport`; merge decoupled from Rust work | **ruled** (2026-10-08) |
| 3 | HMM lifecycle (train, posteriors, live) moves to Rust; ZMQ port 5561 deleted, not migrated | **ruled**; `HMMClient` class itself (not just the port) confirmed fully obsolete, concrete replacement design in §5.2, 2026-10-11 |
| 4 | Observation pipeline moves to Rust; `ContextManager.cpp` itself ports later | **direction set**; dims-first order and numeric policy open |
| 5 | Transformer/HMM coherence rule: C++ HMM state authoritative, sequence-stamped; stale actions (inferred before the current regime began) are not acted on | **ruled**; wording to confirm (regime-epoch vs literal older-sequence, §5.5-1) |
| 6 | Execution-layer computations (`RiskManager`/`PositionManager`/`ExecutionGate`) are in scope, same discipline, no special caution tier | **ruled** (2026-10-09) |
| 7 | One `ffi` staticlib (not per-subsystem `*_ffi` crates) | **ratified by implementation**, 2026-10-08 |
| 8 | `.context.parquet` written directly at collection time; `.alpha`/`TrainingEvent` drops its embedded `ObservationData` copy for a join key | **ruled** (2026-10-10) — depends on `rust/student_t_hmm` landing first for sequencing, not for the decision itself; §5.4 |
| 9 | `backtest_schema.fbs` merged into `mts_schema.fbs` | **done**, 2026-10-09 (§4.2) |
| 10 | `MTS_Envelope` → `Envelope` | **done**, 2026-10-09 (§4.2) |
| 11 | Numeric policy (fast-math or not, tolerances) | **ruled** (operator, 2026-10-11): `-ffast-math` removed from `CMakeLists.txt` entirely — see §10 item 5 for the concrete correctness finding that forced this, not just a tolerance preference |
| 12 | Manifest holds the regime-engine thresholds (vs. crate constants) | **open**, recommended: manifest |
| 13 | GUI env name (`mts-gui` proposed); whether lbrnet keeps the name `mts` | **open** |
| 14 | MindfulTrader git history scrub (force-push) vs. rotation-only, for the leaked API key | **open**, operator's call |
| 15 | A typed Rust safety gate (Atratus's `ladder_core::safety` pattern) | **deferred** — Sierra stays in the order path for now |
| 16 | `MarketObservation`/`RiskStateUpdate`'s GUI-display consumer (§5.4 consumer 3) replaced by an additive `RegimeStatus` struct on the existing `Event` table | **ruled** (operator, 2026-10-11), design in §5.4 — **corrected 2026-10-11, see row 20**: the design's "zero display consumers, stays internal" list for `transition_risk`/`dof`/`expected_duration` was true for display but wrong as a blanket claim; not yet implemented (`student_t_hmm` hasn't landed); `MarketClimate` schema-enum migration flagged alongside but **not yet decided** |
| 17 | `stop_manager.rs` design corrected: a static, entry-anchored, immutable stop/target bracket (`TripleBarrierEngine.h`/`TripleBarrierExitManager.h`'s real current behavior), not a Chandelier-style continuously-trailing stop | **confirmed stale and fixed, 2026-10-11** — `ChandelierStopManager` was deleted entirely during a documented cutover (`docs/ADR/triple_barrier_cutover_phase1_plan.md`), predating this whole initiative; the spec had repeated the stale "Chandelier trailing stop" framing across §8.2/§8.3/§8.5/§8.7 D3/D3a since the execution-layer scoping conversation. All corrected; `elderChandelierATR` (a surviving, unrelated risk-gate diagnostic) explicitly distinguished from the deleted order-management class |
| 18 | Python is removed from the Student-t HMM lifecycle entirely — not just the live-inference round-trip (decision #3) but training, K-selection, calibration, and release-audit tooling too; the full `lbrnet` Python HMM stack is surveyed and scoped for Rust porting or retirement | **ruled** (operator, 2026-10-11): "Python will not [be] involved in the training of the Student-t HMM." Full stack inventory + port/retire scoping: §5.1a. Resolves §5.5's former item 5 (meta-tuning subprocess-vs-import question — moot, see §5.1a) |
| 19 | Whether moving the Student-t HMM to Rust means `mts_schema.fbs`/generated code is no longer needed | **assessed, 2026-10-11 — no, overall; yes for a specific, confirmed subset.** The schema stays essential for ≥4 HMM-unrelated roles (main `Event` stream, trade execution, backtesting artifacts, `.alpha`/`TrainingEvent`); `SystemState`/`HmmRiskState`/`RiskStateUpdate`/`MarketObservation` (the wrapper table only) are confirmed removable. Full assessment: §5.1b |
| 20 | The Rust regime engine adds to `TrainingEvent` the HMM data needed to train the Transformer, and to `Event` the HMM data needed for the Transformer to infer live | **confirmed consistent with §5.4's existing ruling; one correction made** (operator, 2026-10-11): grounded directly in `lbrnet`'s `FEATURE_SPECS`/`feature_buffer.py`, the Transformer has a *confirmed* real need for `transition_risk`/`expected_duration`/`dof` — decision #16's "zero display consumers, stays internal" list wrongly implied these stay internal for every consumer, not just the display one. Full correction, the widen-`RegimeStatus`-vs-separate-struct open question, a `regime_dof` fragility/simplification finding, and the plain-struct-FFI write mechanism (not Rust touching FlatBuffers directly): §5.4 |
| 21 | The Rust side should use `rust/schema`'s own `regenerate_schema.sh`-generated types to construct `RegimeStatus`, not a hand-rolled parallel Rust struct | **corrected 2026-10-11** (operator: "code generated for Rust... by `regenerate_schema.sh`... to process that new HMM info in an optimal way") — row 20's "plain Rust struct, Rust never touches `rust/schema`" mechanism was wrong; confirmed via direct codegen read that `.fbs` structs compile to `#[repr(transparent)]` POD newtypes (`pub struct X(pub [u8; N])`) with a generated `::new(...)` constructor — already in exact wire-format bytes, FFI-safe, zero hand-duplication risk. Gives `rust/schema` its first real consumer. Surfaces an explicitly-flagged, not-yet-decided struct-vs-table tension for `RegimeStatus` itself (field list has already grown twice across decisions #16/#20). Full account: §5.4 |
| 22 | New crate `rust/py` (PyO3) processes both the live ZMQ `Event` stream and the `.alpha`/`.lbr` training file for `lbrnet`, replacing Python's own FlatBuffers-parsing feature-extraction layer with fast Rust | **scoped, 2026-10-11** (operator direction, confirmed against the real pipeline) — `lbrnet/data/lbr_to_polars.py`'s own docstring already says "DEPRECATED... Future institutional pipelines should use C++ extensions or Arrow readers". **Correction, same day**: `fast_ingestor.py`/`flatbuffer_stream.py` were originally cited here as "confirmed dead code" from a search scoped only to `lbrnet/`'s own package directory — a repo-root-scoped re-check found `flatbuffer_stream.py` is genuinely live (wrapped by `backtest/alpha_event_stream.py`, feeding the heavily-used `backtest_runner.py`); only `fast_ingestor.py` (examples-file-only reference) remains a weak dead-code candidate. Transformer confirmed **TensorFlow/Keras**, not PyTorch (operator correction) — `rust/py` must emit plain `numpy` arrays. Full design, scope boundary (data-plumbing only, not the TF/Keras model), and open build/GIL questions: §5.4a |
| 23 | The Transformer training-dataset *generation* pipeline (today's `lbr_to_polars.py` + `tristream_dataset.py` windowing) moves to a separate, offline, native Rust tool — not incremental live-DLL work — and Rust also owns *loading* the finished artifact back in on the Python side, narrowing decision #22's `rust/py` scope to a fast loader rather than a bulk-FlatBuffers-parser for training | **scoped, 2026-10-11** (operator direction, confirmed as a two-stage architecture) — `tristream_dataset.py`'s real output is windowed tensors `(indices, values, hints, position, labels)`, not a trivial format; its load path already defers windowing to load time via a compact pre-windowing cache + `numpy.lib.stride_tricks.sliding_window_view` reconstruction (confirmed by direct code read), which Stage 2 must replicate, not just read a file. Operator's own "institutional-grade or amateur" question answered: intent is institutional-grade (matches `lbr_to_polars.py`'s own self-declared direction); execution is institutional-grade only with the same golden-harness parity discipline used everywhere else in this initiative. Full two-stage design: §5.4a |
| 24 | Standing hardware context: most existing C++/Python was written on a 15GB-RAM, no-GPU, 10-year-old workstation; a new ML-capable workstation has since lifted those constraints | **recorded, 2026-10-11** (operator) — memory-/CPU-conservative patterns found during porting must be investigated (genuine architectural requirement vs. narrow workaround for the old ceiling) rather than reflexively preserved or reflexively stripped; concrete already-found examples and the actionable principle: §1.5. Mirrored as a standing banner in `CLAUDE.md`/`GEMINI.md`/`copilot-instructions.md`/`README-AI.md` per the Doc Sync Contract |
| 25 | `regime_engine.py`'s 11 climate/threshold constants (G0(d)) — placement (manifest vs. crate constant) and whether calibration needs the trained HMM | **resolved 2026-10-11** (operator: "if calibration needs the trained model, we must have a mandatory calibration step after training"; "why not train → calibrate automatically?") — confirmed via direct code read that both `tune_risk_gate.py` and `calibrate_hmm_gate_thresholds.py` require an already-trained model as a mandatory input (hard failure without it); confirmed auto-chaining is well-supported (same data corpus, no new data-collection step, training already reserves and embeds a calibration-holdout window in the model's own metadata). Resolution: **train → audit (GO/NO-GO, reusing `audit.rs`) → if GO: auto-calibrate against that model's own reserved holdout, writing the calibration artifact + a provenance sidecar atomically together (porting the already-shipped `trainer.py`/`live_agent.py` `calibration_artifacts_provenance.json` pattern, found via `lbrnet/evals/cases/` real incident scenarios, not invented fresh)**; Rust loader hard-refuses stale/missing-provenance calibration artifacts rather than silently falling back to a hardcoded default, closing a confirmed live gap in current Python. Three-tier breakdown of the 11 constants, the `context_calibration.json` sibling-artifact finding, and the memory-conservative-pattern re-check (confirmed already done correctly on the new hardware, nothing stale) — all in §5.1c |
| 26 | `dof_learning_rate` (0.02 vs. 0.15) and `bullet_weight` (1.40 vs. 1.30) defaults disagreement between `student_t_hmm.py` and `train_student_t_hmm.py` (F6/G0(b)) | **resolved 2026-10-11** — two different outcomes, not one canonical-value pick: `dof_learning_rate` is confirmed **dead code** (`_update_dof_ecme()`'s own docstring states it is never read; full-file grep confirms `self.dof_learning_rate` is write-once, read-never; no legacy update path exists) — dropped from the Rust port entirely, no field needed. `bullet_weight` is confirmed **real and consumed**, but was never a fixed constant to begin with — `train_student_t_hmm.py` BIC-grid-searches it per training run, so the value embedded in any given trained model is that run's own winning candidate. `model.rs`'s existing `feature_weights: [f64; D]` design (a per-instance field, not a crate constant — already correct, mirroring the `dof_min`/`dof_max` fix) needs no change; the only remaining open item is G5 (`train.rs`)'s own BIC-search default range, deferred to when G5 is built. Full detail: §5.1a |
| 27 | Operator recollection: "I believe we were in the middle of implementing the C++ HMM State Manager when we pivoted to Rust" | **confirmed true, 2026-10-11** — traced to `lbrnet/docs/architecture/STUDENT_T_HMM_MASTER_PLAN.md`'s own living backlog ("Regime Manager migration — remaining stages not started": C++ facade + `regime_engine.py` cleanup + State Manager API surface complete; cross-repo C++ follow-on explicitly flagged "needs a separate `MindfulTrader`-side session" — this one) plus uncommitted `lbrnet` evidence (`regime_manager.py`, confirmed superseded by a later, more complete `RegimeState`-method design in uncommitted `regime_engine.py`/`regime_registry.py` diffs) and a confirmed-live C++ bug this later Python work fixed for itself but never carried over (`HmmStateIndicator`/`RegimeManager.cpp`/`MarketClimateIndicator` all still depend on the exact positional-`state_id`/`HMMStateEnum` assumption Python's own Hungarian-alignment removal (2026-09-21) invalidated). Full account, additional still-unfixed Python-side bugs of the same class (including a `TrainingEvent` wire-schema K=4-hardcoding bug), and concrete `registry.rs`/`regime_engine.rs` recommendations: §5.1d |
| 28 | Artifact serialization format (G0(c), open question #14) — `bincode` vs. manifest + raw-floats hybrid | **resolved 2026-10-11** — `bincode` rejected: confirmed real, non-retiring Python consumers (`train_event_transformer.py`/`tune_event_transformer.py`, Transformer training stays Python/TensorFlow per decision #22, directly `joblib.load()`s the HMM artifact for the Kazemi et al. 2019 `sigma_eff` feature) still need raw read access — not a stale justification. **Simplified further than originally proposed**: one plain JSON manifest, no separate binary weights file — `PRODUCTION_D=18`/`PRODUCTION_K=4`'s ~190-float total parameter count doesn't earn a custom binary format's complexity. `model_sha256` computed over the core model params alone (never the `registry.rs` classification layered on top), anchoring decision #25's calibration-provenance design. `artifact.rs` shipped: save/load round-trips exactly, hash is deterministic and classification-independent, dimension mismatches fail loudly rather than silently truncating/padding. 4 new tests, 159/159 workspace tests pass, clippy clean. |
| 29 | Whether `MarketClimateIndicator`'s native kurtosis/entropy/Hurst "physics veto" hybrid logic survives into `regime_engine.rs` as an explicit safety layer, or is retired (§5.1d item 3) | **resolved 2026-10-11 (operator: "honest, institutional assessment... does some of its functionality belong in the HMM State Manager?")** — **retire.** Traced all three inputs to their real source: `hurst` is confirmed 100% redundant (`ContextManager.cpp:647` copies `ObservationData.hurst_exponent()` verbatim — the exact dimension the HMM already trains on jointly with 17 others); `kurtosis`/`entropy` are genuinely different measurements from the HMM vector's own, but with no evidence the divergence was ever a deliberate ensemble design, reading instead as organic drift between separate initiatives. **Bigger finding surfaced while checking usage, 2026-10-11: a confirmed LIVE BUG, not just a design question** — `HMMClient::HandleBinaryResponse`'s `climateInd->SetClimate(climate_id)` (the real, wire-received, trained classification, written asynchronously/infrequently) and `TripleScreen3.cpp`'s native `UpdateContext()` (written every tick) both write the SAME `MarketClimateIndicator` instance; the per-tick native write wins almost every race, meaning the live system's actual climate-conditioned decisions today are driven almost entirely by the cruder heuristic, not the trained model. Fixing this (deleting the native write path, `regime_engine.rs` as sole writer) is part of the Rust cutover (plan Stage G7), not optional cleanup. Full account: §5.1d item 3/5. |
| 30 | G0(a): `HmmStep`'s state-identity shape (§5.2) — rule/label-based vs. raw positional `state_id` | **resolved 2026-10-12, `regime_engine.rs` SHIPPED** (`rust/student_t_hmm/src/regime_engine.rs`, Stage G4). Renamed `HmmStep` → `RegimeState` (the content is the live regime engine's classification + diagnostics, not a bare HMM math step — "step" stays only on the producing verb, `RegimeEngine::step()`). Design, in order of the questions that forced it: (1) **"producer, not oracle"** — `RegimeEngine` owns exactly one mutable-authority method (`step()`); every rule question (`is_momentum_aligned`, `get_hmm_multiplier`, `is_hostile_change_from`, ...) is a method on the immutable `RegimeState` value itself, so the real planned multi-thread topology (tokio order-watcher, Transformer-inference receiver, user-flatten receiver, spec §8) never needs a lock around the engine for a query — only `step()` needs exclusive access. (2) **`RegimeSnapshot` (C++, `include/RegimeManager.h`) is retired, not reproduced** — its entire reason for existing was the Python/async-ZMQ process boundary; `RegimeState` is already `Copy`/cheap, so a caller just stores a copy and calls `.is_hostile_change_from(&entry, is_long)` on the current one. (3) **No generated dimension-index contract needed for the P2.4/P2.4b observation-vector override cascade** (`tail_index`/`liq_fragility`/`burstiness_index`) — `rust/observation_vector` already computes each as its own named function; a `StepContext{tail_index, liq_fragility, burstiness_index}` carries them into `step()` by name, never by array index, avoiding the exact misaligned-per-dim-array hazard class this repo has already been bitten by twice. Ported 1:1 from `lbrnet/models/regime_engine.py`'s `RegimeState`/`RegimeEngine` + `lbrnet/core/scoring.py`'s 4 multiplier tables (`HMM_MULTIPLIERS`/`CLIMATE_MULTIPLIERS`/`CLIMATE_CONVICTION_BOOST`/`CLIMATE_RISK_MULTIPLIER`/`CLIMATE_HALF_LIFE_SCALE`), golden-parity-validated against real Python execution (mts conda env, 2026-10-12: normalized entropy, confidence, the full P2.4/P2.4b/velocity-deadzone/entropy-chaos override cascade). Amari FIM distance confirmed a **static per-state constant** (depends only on the trained model, never the live observation) — precomputed once at `RegimeEngine::new()`, not per step. `is_momentum_aligned`/`is_mean_reversion_aligned`/`is_hostile_change_from` are a genuinely NEW mapping onto the 5-way `MarketClimate` taxonomy (no Python equivalent exists — these were always a C++-`RegimeManager`-only concept); `ShannonChaos` is deliberately excluded from both alignment sets, an improvement over the retiring 4-way `HMMStateEnum` scheme which forced ambiguous/chaotic states into the mean-reversion bucket. 17 new tests, workspace 176/176 pass, clippy clean (incl. the Windows cross-compile target). **Not yet done, tracked separately**: wiring `regime_engine.rs` behind `ffi`'s `hmm_step`/`hmm_init` (needs G0(e)'s `calibration.rs` for the full `RegimeEngine::new()` artifact-load path), and the C++-side consequence (retiring `HMMStateEnum`/`RegimeSnapshot`, collapsing `HmmStateIndicator`/`MarketClimateIndicator` into one, re-keying `RegimeManager.cpp` onto `MarketClimate` — plan Stage G7). |
| 31 | FFI wiring: `hmm_init`/`hmm_step`/`hmm_reset`/`hmm_shutdown` behind `rust/ffi` (`regime_engine.rs`'s own "not yet done" item, decision #30) | **SHIPPED 2026-10-12** (`rust/ffi/src/student_t_hmm.rs`). Correction to decision #30's own text: this did NOT need G0(e)/`calibration.rs` first after all — that only blocks a *production-calibrated* artifact, not the opaque-handle plumbing itself, which shadow-wires against any valid `artifact::load`-readable model (same `port → parity-test → shadow-wire` discipline already proven for every `observation_vector` dim). Opaque `HmmHandle` wraps a boxed `RegimeEngine<PRODUCTION_D, PRODUCTION_K>`; a `#[repr(C)] RegimeState` mirrors the core `RegimeState<4>`'s fields with every float downcast to `f32` at this exact boundary (plan §6.5: "f64 inside the model, f32 on the wire"); a local `#[repr(u8)] MarketClimate` duplicates the core enum (cbindgen's `parse_deps = false` can't see a dependency crate's types) with an exhaustive-match `From` impl, not a bare `as u8` cast, so a new upstream variant fails to compile here instead of silently drifting. **Two real bugs found and fixed before this could ship, not theoretical concerns**: (1) `RegimeEngine::step()` was allocating on every call once the variance-ratio rolling window filled (`Vec::push`/`remove(0)` growing past its initial capacity, plus a fresh `Vec` + stable-sort clone inside the shared `median()` helper) — violates §6.5's explicit "no allocation in any `*_step` function" rule; fixed with a fixed-size ring buffer (`[[f64; D]; VARIANCE_RATIO_WINDOW]` + cursor/filled counters) and a new allocation-free `median_inplace` using `sort_unstable_by` on a stack array. Added `RegimeEngine::reset()` in the same pass (ported from `reset_belief()`'s exact scope — runtime state resets, loaded model/registry survive), needed for `hmm_reset`. (2) cbindgen does not evaluate ANY named Rust `const` — local or cross-crate — into a C++ array size; using even a crate-local `HMM_K` constant for `RegimeState::probability_vector`'s array size emitted the literal unresolved token `float probability_vector[HMM_K];` into the generated header (confirmed by inspecting the actual generated output, not assumed) — fixed with a literal `[f32; 4]` plus a `const _: () = assert!(...)` tying it back to `PRODUCTION_K` so a future dimension change fails the Rust build loudly instead of silently shipping a stale header. Verified: native tests (6 new, round-trip init→step→reset→shutdown against a real `artifact::save`-written fixture, null/bad-arg rejection, exhaustive enum-conversion), `llvm-nm-22` confirms all 4 symbols exported (`T hmm_init`/`hmm_step`/`hmm_reset`/`hmm_shutdown`) from the Windows cross-compiled staticlib, clippy clean on both targets, generated `include/generated/mts_core.h` diff is pure-additive (102 lines, nothing removed/reordered). Deliberately NOT exposed yet: the rule-API methods (`is_momentum_aligned`, `get_hmm_multiplier`, ...) — out of this wiring task's scope, belongs to Stage G7's C++-side `RegimeManager.cpp` re-keying. |
| 32 | G0(e): the `arch`-crate gap for `calibration.rs`'s stationary block-bootstrap methodology (open question #11) | **RESOLVED 2026-10-12, `calibration.rs` SHIPPED** (`rust/student_t_hmm/src/calibration.rs`). Confirmed via direct crates.io API search: no Rust crate implements either Politis-White (2004) block-length selection or Politis-Romano (1994) stationary bootstrap — both ported from scratch. **A real methodology fork was raised and resolved via an independent consultation** (`lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_158`/`_REPLY`, read-only `--approval-mode plan`, verified via file-mtime check that nothing was modified): our own hypothesis going in — that Politis-White's finite-variance assumption is unsound here, given this crate's `dof_min=2.1` and `TALEB_KURTOSIS_UNDEFINED_DOF=4.0` gate, and that Politis-Romano-Wolf subsampling should replace it — was **independently reviewed and rejected**. The `series` this module bootstraps are window-level SUMMARY statistics (bounded ratios, tenure counts, interior-order-statistic percentiles), not raw Student-t observations; by extreme value theory a 95th-percentile statistic over a large window has a vastly lighter effective tail index than the raw data's own (a top-5%-of-10,000-rows order statistic inherits roughly 500× the raw tail index in finite moments). Adopting subsampling anyway would have been an unjustified downgrade (wider, less efficient CIs, no clean automatic subsample-size rule) to solve a problem that doesn't bite. **Ported faithfully from the real, installed `arch` package's own source** (v8.0.0, `site-packages/arch/bootstrap/base.py`'s `_single_optimal_block` and `_samplers_python.py`'s `stationary_bootstrap_sample_python` — read directly, not reconstructed from the papers alone): `politis_white_block_length` (fully deterministic, golden-parity tested bit-for-bit against a real `arch.bootstrap.optimal_block_length` invocation on a 228-element AR(1) fixture), `geyer_tau_int` (the script's own already-sanctioned fallback — direct O(n·max_lag) autocovariance, not Python's FFT-based one, a deliberate scope cut since this module only ever operates on the same ~228-element window-stat arrays, not the raw-per-bar-scale `_derive_window_rows` needs FFT for), `stationary_bootstrap_indices` (geometric-block-length circular resampling), and `bootstrap_threshold_ci` (percentile CI + the script's own Geyer-vs-bootstrap ESS disagreement flag + `ci_valid` gate). **Two real bugs found and fixed via golden-parity testing, not theoretical**: (1) Rust's `f64::min` always prefers the finite operand over NaN regardless of argument order, but CPython's built-in `min(a, b)` (what the real `arch` source literally calls) is order-dependent — `min(nan, 17.0)` stays `nan` in real Python, verified directly — requiring a hand-written `python_min` helper, not `.min()`, at the one call site where this is load-bearing (`b_sb = min(b_sb, b_max)`); caught by a constant-series golden test expecting `NaN` that initially got `17.0` instead. (2) an overly-conservative early-exit guard incorrectly returned `NaN` for a legitimate n=10 case that real Python computes a finite value for — Python's own out-of-range slicing (`eps[i+1:]`/`eps[:-(i+1)]`) never raises, it clips to empty; replaced with `saturating_sub`/`.min(nobs)` clamping that matches Python's "clip to empty, never panic" behavior for the realistic size range this module targets. 10 new tests, workspace 199/199 pass, clippy clean (incl. Windows cross-compile). **Follow-up refactor, same day (operator: "NaN is more of a Python construct... would it be more institutional to refactor so it is more optimal and better behaved in the Rust world")**: `politis_white_block_length`'s signature changed from `f64` (implicit NaN sentinel, requiring the `python_min` workaround above) to `Option<f64>` — the degenerate precondition (`d_sb <= 0.0`, exactly `lr_acv == 0.0`) is now checked explicitly BEFORE computing `b_sb`, so by the time the final `.min(b_max)` clamp runs, the value is already guaranteed finite and ordinary `f64::min` is correct; `python_min` is deleted entirely, not just unused. This is a representation change only — every golden-tested numeric value is unchanged, only the "could not be estimated" signal moved from an implicit float bit-pattern to an explicit, compiler-enforced `None`. `optimal_block_length_sb`'s own finite/`>= 1.0` guarantee is now also directly tested (a dedicated property-style test across every known-degenerate input), not just inferred from reading `geyer_tau_int`'s guards. 1 new test, workspace 200/200 pass, clippy clean. |
| 33 | G3: `bootstrap.rs` -- parametric bootstrap for gate-threshold CIs, the last remaining module in spec section 5.2's target architecture list before Stage G7 | **SHIPPED 2026-10-12** (`rust/student_t_hmm/src/bootstrap.rs`). Ports `lbrnet/models/hmm_parametric_bootstrap.py` in full: simulates independent regime-state paths and 2D Student-t emissions directly from a fitted model's own transmat/startprob/means/covars/dof (not resampled real history), sidestepping `calibration.rs`'s own nonparametric block bootstrap's N_eff>=30 ceiling that real market regime persistence (~5,000-10,000-bar decorrelation length) leaves even an 8-10M-row holdout sitting right at (Zucchini, MacDonald and Langrock 2016 ch. 3.6; grounded via `lbrnet/logs/rc_gemini.log` CLAUDE_BRIEF_153/_REPLY, 2026-09-21, pre-dating this session). Two new RNG primitives this crate did not yet have: a Box-Muller standard-normal sampler and a Marsaglia-Tsang (2000) gamma sampler (chi-squared(df) = 2*Gamma(df/2)), with an Ahrens-Dieter (1974) sub-1-shape boost included for completeness though not reachable at this crate's own dof_min=2.1 floor (shape=dof/2>=1.05 always in practice). **One real algorithmic mismatch found and fixed while porting `simulate_state_path`**: the real Python's per-step state draw, `(u[:, None] > row_cum).sum(axis=1)`, is a direct count of cumulative-probability entries strictly less than u -- NOT equivalent, at the exact-tie edge (u landing precisely on a cumulative boundary), to a naive first-index-with-cum[k]>u search; the count formula keeps the LOWER state at that edge. Caught by deriving both formulas by hand on a worked numeric example before trusting either, not by a failing test (no golden fixture exists for this stochastic module, see below) -- fixed with a u<=cum[k] comparison, a dedicated `sample_from_cumulative_exact_tie_keeps_lower_state` test added so the distinction is pinned down, not left to be rediscovered. Shares `gate_metrics::mean_tenure_by_state` (fixing the real Python module's own flagged pre-existing duplication) and `calibration::percentile_sorted` (newly made `pub(crate)` for this, instead of a second copy). **Revisits an earlier in-session judgment that this module was "too statistically risky" to attempt next** -- that judgment predated `calibration.rs`'s own precedent (now explicit, not just a vague intuition) that a crate-internal stochastic sampler only needs moment-matching/structural validation, not bit-exact cross-language parity, to be trustworthy; once that bar was established it de-risked the chi-squared-sampler work enough to proceed. Validated via moment-matching (sample mean/variance of 200k draws converge to the closed-form Gamma/Chi-squared theoretical values for several dof, including the sub-1-shape boost path) and structural checks (state paths always land in [0,K), CI bounds widen with ci_size, the misspecification p-value is high for a typical real estimate and low for an extreme one). 16 new tests, workspace 216/216 pass, clippy clean (incl. Windows cross-compile). |
| 34 | G7 prerequisite: no real (non-synthetic) model artifact existed yet to shadow-wire `hmm_step` against | **Produced and verified, then deliberately torn back down, 2026-10-12** (operator: "I don't expect any Python training to take effect and the script will be removed ASAP"). A one-time, two-half bridge (a Python dumper run once in the `mts` conda env, plus a short-lived `rust/tools` `hmm_tool import-legacy` subcommand) converted the one real trained pickle on disk, `lbrnet/models/hmm_model.pkl` (2026-09-18 training run), plus its paired `ModelManifest_candidate_20260918_full.json`'s already-computed `state_map` (the real `build_regime_registry()` classification, from the real decoded Viterbi path over 21.9M real training rows -- deliberately read forward, not re-derived, since re-running decode over the full corpus is a separate, expensive undertaking this bridge did not attempt), into a real `artifact.rs`-format manifest, round-trip-verified bit-for-bit. **Real finding surfaced in the process**: this real trained model is D=17/K=4, not this codebase's often-cited `PRODUCTION_D=18` -- it predates the later 17-to-18 dim changes; a real, not hypothetical, illustration of why `artifact::load`'s D/K mismatch check exists. **A genuine upstream bug found and fixed along the way, not specific to this one artifact**: `serde_json`'s float deserializer is not correctly-rounded by default for every decimal string (serde-rs/json#707's exact failure class) -- reproduced directly (not assumed from the issue tracker alone) via a real 1-ULP mis-parse of this model's own `amihud_illiquidity` covariance value (`5.801714613487153e-08`), caught by `hmm_tool`'s own round-trip assertion failing on first run. Fixed by enabling serde_json's own `float_roundtrip` Cargo feature workspace-wide (not a hand-rolled parser) -- every future `artifact.rs` save/load round-trip now inherits the fix, not just this one conversion. **Both halves of the bridge were deleted once the one real artifact was produced and verified** (operator directive, matching "no further Python training is expected, ever" -- unlike the eventual Rust-native `hmm_tool train`/`posteriors`/`verify-golden` subcommands the plan still names as real future work, this `import-legacy` capability had exactly one real input it would ever process and that conversion is now done): `rust/tools` crate removed entirely (workspace membership reverted), `export_legacy_pickle.py` deleted. **What remains, permanently**: the verified artifact itself, `rust/student_t_hmm/fixtures/hmm_model_20260918_manifest.json` (6.3KB, the crate's first real non-synthetic artifact), plus a small permanent `examples/load_check.rs` smoke-check loading it and printing each state's real `MarketClimate` classification -- this is now available for whenever shadow-wiring `hmm_step` against a real model becomes the next step. Workspace 216/216 tests still pass, clippy clean (incl. Windows cross-compile) after the `tools` crate removal. |
| 35 | `observation_vector`: `lempel_ziv` (15th dim), plus a stale crate-status count correction | **SHIPPED 2026-10-12** (`include/LempelZivComplexity.h`, `rust/observation_vector::lempel_ziv_complexity`). First dim whose math lived inside a stateful class method (`InformationEngine::GetLempelZivComplexity()`, ring-buffer snapshot + binarization + LZ76 count all in one body) rather than an already-standalone header -- extracted the pure compute into a new header first, proved zero behavior change via a native regression test comparing the refactored class method against the pre-refactor body on a real sequence, THEN ported that pure function to Rust (same two-step discipline `MeanReversionCalculator.h`/`LiquidityFragilityEngine.h` already established, now explicitly named as the template for any future stateful-class-method dim). Cross-language parity exact to 1e-12; shadow-wired at its one real call site (`ContextManager.cpp`, confirmed sole caller). Both DLL configs build clean -- `MTS_WITH_RUST=OFF` is NOT byte-identical to the pre-refactor baseline here, correctly: unlike every additive-only dim before it, this port changes unconditional production code (the extraction itself), not just code gated behind `#ifdef MTS_WITH_RUST"; the regression test, not a DLL byte-diff, is what proves behavioral identity for this kind of port. **Real documentation-staleness finding surfaced while choosing this dim**: the crate-status table's "12 of ~18 dims ported" was already wrong before this session touched anything -- direct enumeration of every `pub fn` in `lib.rs` against the real 18-name `ObservationData` constructor found 14 dims already covered (`micro_asymmetry`/`recurrence_rate` had been ported in a session whose own dated bullet never got added to this log) -- corrected to 15 of 18 (post-`lempel_ziv`), with the 3 genuinely remaining dims (`log_scale_ratio`, `tail_index`, `fast_mean_rev_z`) now named explicitly rather than left to "~18" vagueness. `fast_mean_rev_z` in particular carries its own unresolved real contradiction (FeatureScaler calls it "MOOT, DROPPED" 2026-09-04; `ContextManager.cpp` calls it "wired 2026-09-17, EXPLICIT operator authorization") worth resolving before it is next. 4 new Rust tests (58/58 total), 3 new cross-language parity assertions, 14 new native C++ checks, both DLL configs build clean. |
| 36 | `observation_vector`: `log_scale_ratio` (16th dim) | **SHIPPED 2026-10-12** (`include/LogScaleRatio.h`, `rust/observation_vector::log_scale_ratio`). Unlike every prior dim, the hard math (bipower variation) was already ported and shadow-wired (`compute_bipower_variation`, 2026-10-08) -- only the thin ratio+clamp composition step remained. Extracted as a function of two ALREADY-COMPUTED BV values (not raw log-return windows), so the Rust port composes the existing `compute_bipower_variation` port rather than duplicating it, and the C++ extraction avoids a second BV computation the caller already did. FFI wrapper (`observation_vector_log_scale_ratio`) is the first in this codebase to take two plain scalar `f64` arguments instead of a pointer/length pair. Cross-language parity exact to 1e-6 (3 new assertions); shadow-wired at the real call site alongside its pre-existing BV-level shadow checks -- two independent shadow comparisons now cover this one dim end-to-end. 5 new Rust tests (63/63 total), 5 new native C++ checks. Both DLL configs build clean -- `MTS_WITH_RUST=OFF` unchanged in size from decision #35's own baseline (net-neutral refactor: 3 inline lines replaced by 1 header call), `=ON` links and grows. 2 of 18 observation-vector dims now genuinely remain unported: `tail_index`, `fast_mean_rev_z`. |
| 37 | `observation_vector`: `tail_index` (17th dim) | **SHIPPED 2026-10-12** (`include/HillTailIndex.h`, `rust/observation_vector::hill_tail_index`). The most complex port in this series so far -- Resnick & Starica (1997) Hill-plot stability-region k-selector (partial_sort + running-prefix-log-sum Hill(k) scan + first-plateau-wins selector over [10, 4*tail_cutoff]). Unlike `lempel_ziv`'s class, `TailRiskEngine::GetHillAlpha()` is already a pure O(1) state read -- the real estimation happens inside `AddObservation()`, feeding an EWMA-smoothed alpha that depends on the entire observation history, not a recomputable snapshot. Extracted `ComputeRawAlpha()`'s math into a new header returning `std::optional<double>` (not an implicit NaN sentinel, per this session's own institutional-NaN-handling precedent, `calibration.rs`'s `Option<f64>` refactor); added `GetWindowSnapshot()`/`GetTailCutoff()` accessors so shadow-wiring could compare the RAW (pre-EWMA) Hill alpha -- the only level meaningfully comparable without replicating the engine's sequential EWMA state in Rust, not the smoothed `GetHillAlpha()` output itself. A regression test proves the refactored class method still matches an independent `HillTailIndex()` call on the identical window. **Design choice caught and fixed mid-port**: the header takes a MUTABLE buffer it `partial_sort`s in place (not `const` + an internal copy) specifically to preserve `ComputeRawAlpha`'s own zero-extra-allocation design -- the first draft introduced a needless second buffer copy before this was caught. Also removed one now-genuinely-dead member (`m_hillAtK`), per this repo's own standing dead-code-deletion rule. Ported to Rust (`hill_tail_index`, taking `&mut [f64]`), FFI-wrapped (`observation_vector_hill_tail_index`, the first wrapper in this family needing a mutable `*mut f64` rather than `const`), cross-language-parity-tested (3 new assertions on a real 500-element production-shaped window, exact to 1e-9), and shadow-wired at the real call site. 4 new Rust tests (67/67 total), 6 new native C++ checks. Both DLL configs build clean -- `MTS_WITH_RUST=OFF` (1,780,736 bytes) and `=ON` (2,053,632 bytes, links and grows). 1 of 18 observation-vector dims now genuinely remains unported: `fast_mean_rev_z` (its own unresolved "MOOT, DROPPED" vs. "wired, EXPLICIT operator authorization" contradiction worth resolving before porting it). |
| 38 | `observation_vector`: `fast_mean_rev_z` (18th schema dim) — port or retire | **RETIRED 2026-10-13, not ported** — rejected by both its forward-return test (corrected 5-horizon declustered sweep, no significant cell) and the 2026-09-18 HMM state-occupancy test. Live/offline computation deleted; dim stays in the 18D schema as a constant 0.0f. `observation_vector` port is therefore complete at 17 of 17 live dims. Full account: `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` row 19 |
| 39 | Execution: separate ORDER management from POSITION management; give the broker seam an amend; fix the watcher's release/clock semantics (operator review of the order-watcher port, 2026-10-13) | **DONE 2026-10-13.** D3a had turned a strategy rule ("Phase 1 barriers don't move", triple-barrier doctrine, `docs/ADR/triple_barrier_cutover_phase1_plan.md` §1) into an execution rule ("bracket orders are never updated"), and the order-watcher port cited the legacy C++ ("never modifies a working order") as if it confirmed that. Neither is institutional reasoning: amend (FIX `OrderCancelReplaceRequest`, IBKR `modifyOrder`) is a standard order-management action. Now: `BrokerAdapter::amend` (`broker::Amend`: quantity and/or price only; side, role, type and algo fixed) with IBKR and pessimistic-Sierra translations and a broker-confirmed `Event::OrderAmended` in `position_state`; whether a bracket LEVEL may move stays a strategy decision above the seam. `order_watcher`: `Release` split into `Finished` (a fact) and `Escalate` (broker unreachable -- state unknown, reconcile or flatten, never dropped: the C++ orphaned the order); two caller-supplied clocks (market time for intent expiry and churn, a steady local clock for TTL and budget, so a stalled feed cannot freeze the watchdog; the shell must also call it on a timer). D3/D3a rewritten below |

---

## 3. Target architecture

### 3.1 End state (post-merge) directory layout

```
VSCode/MindfulTrader/                      repo root = the monorepo (merge itself: plan §1.2, deferred)
├── rust-toolchain.toml                    ← pinned; must be at the repo root
├── rust/                                  ← THE Cargo workspace
│   ├── Cargo.toml  Cargo.lock  .cargo/config.toml
│   ├── schema/              schema               generated FlatBuffers Rust + contract constants (pure)
│   ├── observation_vector/  observation_vector   18 dims, FeatureScaler, Mahalanobis gate        (pure)
│   ├── student_t_hmm/       student_t_hmm        Student-t HMM: train, filter, regime engine     (pure)
│   ├── transport/           transport            every ZMQ socket                                (zmq)
│   ├── ffi/                 ffi                  ONE staticlib + cbindgen header → the DLL
│   ├── py/                  py                   ONE PyO3 extension "mindful_core" → lbrnet + GUI envs
│   └── tools/                tools               bins: hmm_tool, obs_tool (train/posteriors/replay/verify)
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

**Live, end state**: `ticks → Sierra → cpp adapter → observation_vector (dims → scaler → gate) →
student_t_hmm (in-process step) → HmmStateIndicator + the event (carrying the HMM output) → transport PUB
→ lbrnet Transformer → action back through transport → PredictionState (paired by §5.5's coherence
rule)`; the GUI subscribes via the same Rust client. ZMQ port 5561 no longer exists; the observation
vector, the model, and the posteriors each have exactly one implementation.

**Offline, end state (goal, §5.4, not yet built)**: `raw ticks → EventDataCollectorStudy.cpp (in-process
observation_vector + student_t_hmm) → .context.parquet (full raw vector, canonical) + .alpha/TrainingEvent
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

**`schema` crate specifics**: `#[path = "generated/mts_schema_generated.rs"]` with an `#[allow(...)]`
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

### 5.1a Full `lbrnet` Python HMM stack inventory (surveyed 2026-10-11, decision #18)

Operator ruling (decision #18): **Python will not be involved in the training of the Student-t
HMM** — this supersedes the narrower §5.5-item-5 question (whether a Rust trainer merely
*invokes* `train_student_t_hmm.py`) with a much larger one: does *any* `lbrnet` Python file need a
Rust equivalent, and if so, is this a chance to elevate the implementation, not just transliterate
it? Four parallel research agents surveyed every `lbrnet` file that touches the Student-t HMM
(core model, registry/classification, control-plane/calibration scaffolding, and the ~3,150 lines
of architecture docs + ~20 regression tests that encode its hard-won correctness lessons), plus
Atratus's own existing Rust Student-t HMM (`sensor_core/src/hmm.rs` + its spec +
`knowledge/hmm_model_validation.md`) was read directly for inspiration — **Atratus is not
MindfulTrader and there is no code/data link between the two; it is read-only prior art.** Findings
below are the result; nothing has been implemented yet.

**Two coexisting, inconsistent state taxonomies found** — any port must pick one, not assume
consistency: the legacy `HMMStateEnum` (4 states; used by `regime_manager.py`,
`regime_focal_loss.py`'s `regime_weights`, `hmm_context_features.py`; explicitly being retired in
`lbrnet`'s own code) vs. the newer `MarketClimate` (5 states, adds `TALEBIAN_FRAGILE`/
`SHANNON_CHAOS`; used by `regime_registry.py`/`regime_engine.py`). **Ruling: `MarketClimate`
(5-state) is canonical for the Rust port; `HMMStateEnum`-only consumers are out of scope or need
remapping** — this is also consistent with decision #16's already-ruled `RegimeStatus` schema
design, which is `MarketClimate`-shaped.

| File (lines) | Role | Port / retire / drop verdict |
|---|---|---|
| `models/student_t_hmm.py` (1693) | System-of-record CPU model: diagonal-covariance Student-t HMM, log-space Hamilton forward-backward, MAP-EM/ECM/ECME, k-means++ seeding, weighted Mahalanobis, DOF via ECME (`scipy.optimize.brentq`) | **Port** — the core math crate, highest-value and highest-risk; see 20-test correctness checklist below |
| `models/student_t_hmm_gpu.py` (501) | PyTorch-batched restart *screening only* — explicitly "not system of record," has **no** dof/ν estimation at all (brentq doesn't vectorize), 20-33x slower than CPU/joblib per the runbook's own measurement | **Likely drop** — Rust's own CPU EM is expected to be fast enough (Atratus precedent + no Python interpreter overhead) to make GPU screening moot; recommend confirming with a real restart-count/wall-clock measurement once the Rust trainer exists, not assuming up front |
| `models/regime_engine.py` (1827) | Live stateful inference: sequential Hamilton filter + `RegimeState` (climate classification overrides — Talebian-fragile/Pareto-momentum/velocity-EMA deadzone-hysteresis/entropy-chaos — variance-ratio drift, black-swan streak, Amari/FIM nearest-rival distance (telemetry-only, Monte-Carlo-verified, no citation), duration tracking, `cognitive_load`) | **Port** — this is the live engine behind `hmm_step`/`RegimeStatus` (§5.2/§5.4); duck-types the model object via attribute probing — the Rust port needs an explicit trait, not structural typing. `reset_belief()` does **not** reset entropy-drift-alerts or duration history — a real lifecycle subtlety to preserve or deliberately change. No locks despite mutable shared state (concurrent `infer()` would race) — a latent bug, not a feature, don't carry forward |
| `models/regime_registry.py` | `MarketClimate` classification cascade (the newer 5-state taxonomy) | **Port** — canonical taxonomy owner, see ruling above |
| `models/regime_manager.py` | Offline/batch labeling counterpart, pinned to the legacy 4-state `HMMStateEnum` | **Drop or remap** — superseded by `regime_registry.py`'s 5-state cascade per the canonical-taxonomy ruling; confirm zero remaining production callers before deleting |
| `models/regime_gate_metrics.py` (202) | Pure numpy quality-gate statistics (no TF, no I/O), run-length encoding via boolean-vs-numeric diff (8x less transient memory, measured at 55M+ row scale) | **Port first** — the cleanest, most-reused, lowest-risk file in the whole stack; recommend as the literal first Rust module written once Stage A's golden harness exists |
| `models/hmm_parametric_bootstrap.py` | Parametric bootstrap coupled to the Student-t emission formula; duplicates `regime_gate_metrics.py`'s `_mean_tenure_by_state` verbatim (a source-flagged, pre-existing cleanup opportunity) | **Port**, and fix the duplication in the same pass (single shared helper, not two copies) |
| `models/regime_focal_loss.py` | TF/Keras focal-loss weighting keyed off `HMMStateEnum`, feeds Transformer *training*, not the HMM itself | **Out of scope** for this port — belongs to a future Rust-side Transformer-training initiative, if one is ever started; do not conflate with HMM porting |
| `models/hmm_utils.py`, `hmm_features.py`, `hmm_context_features.py`, `hmm_training_cache.py` | Shared helpers; `hmm_context_features.py` is pinned to the legacy 4-state enum and reads HMM entropy/state for Transformer feature engineering | **Mixed** — `hmm_training_cache.py` likely ports alongside the trainer; `hmm_features.py`/`hmm_context_features.py` need a scoping call once the Transformer-training question above is answered — flagged, not decided |
| `scripts/train_student_t_hmm.py` (5843) | The real trainer: BIC-based K-selection (filtered by quality gates, not pure argmin), multi-restart screening (seed stride 1009, winner = highest MAP-LL), chronological (never shuffled) holdout + forward-walk validation, joblib/loky parallelism, `joblib.dump()` pickle artifact (the de facto model interchange format), optional Anthropic-API AI-diagnosis (`--enable-ai-diagnosis`, network-dependent, non-deterministic), Matplotlib diagnostics | **Port the methodology, not the file** — CLI/orchestration structure doesn't translate 1:1; AI-diagnosis and Matplotlib plotting are explicitly out of scope for a core numerical port (not dropped from the *product*, just not a Rust-crate concern) |
| `scripts/materialize_hmm_features.py` | Applies a frozen model to stamp posteriors onto `.alpha`/`.context.posteriors.npy`, consumed downstream by Transformer training and `build_directional_alpha.py`'s provenance-manifest check | **Must exist after the port in some form** — confirmed load-bearing by a real downstream consumer |
| `scripts/regime_calibration.py` | Computes the live runtime `temporal_half_life_sec` decay parameter from the trained transition matrix, read by `live_agent.py` at startup | **Must exist after the port** — "the clearest must-exist-after-the-port case" per the surveying agent |
| `scripts/calibrate_hmm_gate_thresholds.py` (931) | Richest statistical methodology in the stack: Geyer (1992) `tau_int`, Politis & White (2004) optimal block length, stationary block-bootstrap CIs with a minimum-effective-blocks validity guard (Efron & Tibshirani 1993), mean-residual-life diagnostic (Coles 2001), disjoint train/calibration enforcement. Uses Python's `arch` package (stationary bootstrap) | **Port the methodology; flag the `arch`-crate gap** — no obvious Rust crate equivalent found for stationary block-bootstrap; needs either a from-scratch implementation (Politis & White's own paper gives the algorithm) or a scoped literature search before committing to the port's timeline |
| `scripts/audit_hmm_release.py` | Deterministic JSON-in/JSON-out GO/NO-GO gate reader, no discretionary interpretation | **Port as-is** — "the cleanest keep-it-as-is case"; only its producer (artifact format) needs to stay compatible with whatever Rust trainer replaces `train_student_t_hmm.py` |
| `scripts/meta_hmm_control_plane.py`, `build_meta_hmm_run_plan.py`, `validate_meta_hmm_run_plan.py`, `contracts/meta_hmm_trainer_bridge_contract.py` (4 files) | A self-contained "trainer run plan" sub-cluster, meta-tuning/HPO scaffolding | **Confirmed orphaned, deleted 2026-10-11** (`git rm`, lbrnet commit pending) — exhaustive repo-root-wide search found zero external callers (only cross-references within this 4-file sub-cluster + its own now-also-deleted test). `lbrnet/contracts/__init__.py` updated to drop the dead import; both the package and the still-live sibling scripts (below) verified to still import cleanly after the cut |
| `scripts/generate_meta_hmm_recommendation.py`, `validate_meta_hmm_recommendation.py`, `record_meta_hmm_recommendation_outcome.py`, `contracts/meta_hmm_recommendation_contract.py`, `contracts/meta_hmm_recommendation_ledger_contract.py` (5 files) | A separate "recommendation/ledger" sub-cluster — **corrected 2026-10-11, this was wrongly folded into the "confirmed orphaned" verdict above in the original research pass** | **Live — do not drop, needs its own port/keep decision.** Confirmed invoked by `lbrnet/scripts/run_optimization_pipeline.py` (`_python_module_cmd(...)` subprocess calls to all three scripts), which is itself imported by `lbrnet/orchestrator/risk_gate_orchestrator.py` — a real production orchestrator, not scaffolding. The original "zero mentions across all 3 canonical architecture docs" finding was accurate but was a claim about *documentation coverage*, not *liveness* — conflating the two nearly led to deleting live code. Scoping call (port to Rust vs. keep as Python, given it's risk-gate-adjacent not HMM-training-adjacent) is a new, separate open question, not resolved here |
| 3 benchmark files (under `scripts/`) | Dev-only performance benchmarking harnesses | **Drop** — pure dev tooling, not product scope |

**Reference documentation read in full** (not modified, source of the literature citations and the
test inventory below): `knowledge/global/models/student_t_hmm_theory.md`,
`docs/architecture/STUDENT_T_HMM_MASTER_SPEC.md`, `STUDENT_T_HMM_MASTER_PLAN.md`,
`STUDENT_T_HMM_EVENT_TRANSFORMER_ARCHITECTURE.md`, `docs/hmm/STUDENT_T_HMM_RUNBOOK.md` (1336
lines, the densest operational doc — holds the canonical tuning protocol, Smart Tuner V1 frozen
contract, formula catalog, and the 16-item "Path to Elite" lessons-learned changelog referenced
below).

**Inconsistent defaults found in the source Python itself — RESOLVED 2026-10-11 (decision #26,
G0(b))**, two different outcomes per constant, neither is "pick 0.02 or 0.15 / 1.40 or 1.30":

- **`dof_learning_rate` (0.02 training-script CLI default vs. 0.15 `student_t_hmm.py` class
  default): confirmed dead code, not a disagreement to reconcile at all.**
  `_update_dof_ecme()`'s own docstring states it directly — "Applied directly (no learning-rate
  blend, unlike the old heuristic) ... `dof_learning_rate` is retained only as a stored/serialized
  config value for other scripts' tuning grids (`smart_tuner.py` etc.), not consumed here" — and a
  full-file grep confirms `self.dof_learning_rate` is assigned once at `__init__` and never read
  again anywhere in `student_t_hmm.py`; no legacy/alternate DOF-update method exists to fall back
  to. `smart_tuner.py` does still sweep it as a nominal search dimension (`--dof-learning-rate`),
  but since the value is never consumed downstream, that sweep is functionally inert — every
  candidate in that dimension produces an identical fit. **Resolution: drop `dof_learning_rate`
  from the Rust port entirely — no field, no constant, nothing to reconcile; the Rust trainer's
  DOF update (Brent's method, F5's elevation item) needs no learning-rate parameter at all.**
- **`bullet_weight` (1.40 training-config default vs. 1.30 `student_t_hmm.py` class default):
  confirmed real and consumed** (`weights[idx] = self.bullet_weight` sets specific "bullet dim"
  indices of the per-dimension feature-weight array used directly in the Mahalanobis-weighted
  emission density) **but not a single fixed constant to begin with** —
  `train_student_t_hmm.py` runs its own BIC-based grid search over multiple candidate
  `bullet_weight` values per training run (`bullet_weight_grid`), so whichever value ends up
  embedded in any specific trained model is that run's own winning candidate, not a universal
  constant. `model.rs`'s existing design already handles this correctly without any fix needed:
  `feature_weights: [D; f64]` is already a per-instance field loaded/constructed per trained model
  (mirroring the `dof_min`/`dof_max` per-instance-field fix made earlier this session), not a
  hardcoded crate constant — so inference and `artifact.rs`'s load path have nothing to reconcile.
  **The only remaining open decision narrows to G5 (`train.rs`)'s own BIC grid-search default
  starting point/range for `bullet_weight`** — a much smaller, later call than "pick one canonical
  constant," deferred to when G5 is actually built.

**The "L_MAP decline" bug chain (runbook's "Path to Elite" items 10-16)** is the single most
important piece of prior art for anyone writing the Rust EM loop — a multi-day forensic
investigation that found and fixed four distinct, independently-landed numerical bugs in the
MAP-EM covariance/DOF update, each must become an explicit Rust regression test, not just
"reproduce the final formula":
1. Raw-LL-only convergence monitoring is theoretically wrong for MAP-EM (Green 1990 only
   guarantees ascent of the *penalized* objective `L_MAP`, not raw LL) — checkpoint-restore must
   be scored on `L_MAP`, never raw LL alone.
2. A density-normalization bug: the Student-t emission didn't integrate to 1 under non-uniform
   per-dimension weights because the gamma-function term used the weighted-sum `Σw_j`, not the
   true dimension `D`.
3. A τ/σ² feature-weight conversion bug in the covariance M-step caused a ~28.6% variance
   underestimate on the weighted "bullet" dimensions — the single largest driver of the decline.
4. An ECME DOF staleness bug: the DOF conditional-maximization step was fed E-step statistics
   accumulated under the **previous** iteration's covariance, not the just-updated one, violating
   Liu & Rubin (1995)'s ECME monotonicity requirement — independently cross-checked against a 2026
   arXiv paper (libhmm, 2605.29208) whose CM-step ordering matches the fix.
5. A separate, same-code-region mean-prior covariance cross-term bug (missing a Σ-dependence term
   when `mean_prior_kappa > 0`) was found and fixed, though re-verification showed its real
   magnitude on production data is negligible (~13-33 nats vs. a ~375,000-nat dominant term).
6. A `min_covar` floor miscalibration (0.1 → 1e-12): the flat 0.1 floor bound continuously on
   15-24 of 64 (state×dim) slots every iteration on real data; its root cause was that
   `global_var` (the covariance-prior anchor itself) was *also* floored at the same flat 0.1,
   corrupting the prior's target for naturally-small-variance dimensions (e.g. ~0.005-0.02).
7. **Item 15 — an exhaustive further investigation found a real, non-diagnostic-artifact residual
   decline whose root cause was NOT found**, despite 5 synthetic reproduction configs, a full
   manual code audit, and literature review. Confirmed correctly mitigated by checkpoint-restore
   (twice independently verified) but genuinely open. **The Rust port must carry this forward as a
   documented open question, not silently "fix" it by construction and claim resolution it hasn't
   earned.**
8. A distinct covariance M-step denominator bug (living in the same code region, separately
   audited): the covariance update must divide by the **unweighted** responsibility sum
   (`Σγ_ik`), not the weighted one (`Σγ_ik·w_ik`) the mean's own M-step correctly uses.

**The ~20-file `test_student_t_hmm_*`/`test_train_student_t_hmm_*` suite is the exact
numerical-correctness contract a Rust port must reproduce bit-for-bit (or within documented
float32/reassociation tolerances)** — grouped by domain, each name below is a literal regression
test the Rust port needs an equivalent of:
- **Scaling/parity**: a second Python-side z-score normalization stacked on top of the C++
  `FeatureScaler`'s already-robust normalization was found and *removed* — the HMM fits directly
  on raw already-C++-scaled values; `predict_proba()`/`score()` must each call `_posterior()`
  exactly once, not twice; each BIC grid candidate must call `decode()` once, not `score()`+
  `predict()` separately.
- **Mean/covariance M-step** (the L_MAP bug chain above, each with its own test):
  `test_student_t_hmm_mean_prior_covariance_crossterm.py`,
  `test_student_t_hmm_min_covar_floor_calibration.py`,
  `test_student_t_hmm_covariance_denominator.py`,
  `test_student_t_hmm_weighted_covariance_mstep.py`,
  `test_student_t_hmm_covar_prior_eta_scaling.py` (the prior's `eta` must N-scale at production
  sample counts — at N=13.5M a fixed `eta=16` contributes <0.12%, effectively disabling the prior).
- **Emission density/DOF**: `test_student_t_hmm_emission_normalization.py` (numerically integrates
  the real density via scipy quadrature, must equal 1.0), `test_student_t_hmm_dof_ecme.py`
  (recovers true ν from synthetic Student-t data; confirms decoupling from covariance scale, unlike
  the old moment estimator which ceiling-sought), `test_student_t_hmm_ecme_dof_staleness.py`.
- **Convergence/checkpoint**: `test_student_t_hmm_relative_convergence.py` (relative, not absolute,
  tolerance at ~1.1e8 LL magnitudes; convergence must be **one-sided** `delta<tol`, never
  sign-agnostic `abs(delta)<tol` — McLachlan & Krishnan), `test_student_t_hmm_checkpoint_restore.py`
  (restore scored on L_MAP, not raw LL).
- **Initialization**: `test_student_t_hmm_kmeans_plusplus_init.py`,
  `test_train_student_t_hmm_representative_init.py` (streaming-fit's init sample must span the
  full chronological range, not just the first ~512 rows — proven not fixable by covariance-prior
  N-scaling alone, since occupancy collapse is an E-step responsibility-competition effect),
  `test_student_t_hmm_seeded_init.py`.
- **Forward-backward/streaming/chunking**: `test_student_t_hmm_streaming_batching.py`
  (`_log_emission_and_delta()` must be row-independent of call size),
  `test_student_t_hmm_predict_chunking.py` (chunked `predict()` guards a real production OOM —
  unchunked `_posterior()` was ~1.7-2.2GB transient at N=13.5M; chunking must respect true sequence
  boundaries, never merge across them), `test_student_t_hmm_estep_thread_parallelism.py` (parallel
  E-step matches sequential within reassociation tolerance, bounded in-flight concurrency).
- **Context-file loading**: `test_train_student_t_hmm_context_loading.py` (bounded vs. unbounded
  MO/SS-context loader parity against MindfulTrader's own `LogContextUnlocked()` record format).
- **GPU path** (if kept): `test_student_t_hmm_gpu_batched_screening.py` (per-restart
  `_global_mean`/`_global_var` must come from that restart's own init sample, not restart 0 — a
  found-and-fixed bug).

**Institutional-grade elevation opportunities** (from Atratus's prior art + the literature already
cited across these docs — none yet decided, all candidates for plan discussion):
1. Atratus's `stable_posterior()` numerically-stable log-sum-exp pattern, explicitly stress-tested
   against a 1e9-magnitude synthetic anomaly tick
   (`test_predict_stable_under_extreme_anomaly`) — the Rust port should require an equivalent
   extreme-value stress test as a non-negotiable, not an afterthought.
2. Brent's method for the DOF root equation (Atratus's own from-scratch implementation, matching
   `scipy.optimize.brentq`'s role in the Python ECME step) — direct inspiration for the Rust
   equivalent; no crate search needed, the method is simple enough to hand-roll correctly.
3. Robust `Sn`/`Qn` estimators (Rousseeuw & Croux 1993) are more mass-point-robust than MAD at
   degenerate/low-cardinality scales — directly relevant given MindfulTrader's own repeated
   MAD-related bugs this initiative (burstiness_index, amihud_illiquidity); worth evaluating for
   any Rust-side robust-statistics helper the HMM calibration code needs, not yet decided.
4. Feature saliency via an MML penalty (Law, Figueiredo & Jain 2004) — directly relevant to the
   separate, already-underway Elite Feature Set Curation initiative's Phase 2
   (`docs/superpowers/specs/2026-09-07-feature-saliency-em-fitter-spec.md`); flagged as a
   cross-initiative synergy, not a decision for this spec.
5. Reconsider the artifact serialization format (currently planned as a manifest + raw-floats
   hybrid, §5.2) now that Python may never need to read the trained model directly — Atratus uses
   plain `bincode`, materially simpler. Not yet decided; needs weighing against the still-real
   downstream consumers confirmed above (`materialize_hmm_features.py`/`regime_calibration.py`
   equivalents, if those stay partly Python, or their own Rust replacements if not).
6. The practices the literature review in `knowledge/hmm_model_validation.md` recommends are
   **already Python's actual practice**, not new: multiple seeded-EM-restarts-per-K (McLachlan &
   Peel 2000; Celeux & Durand 2008), correct BIC parameter count (Rabiner 1989), time-based
   held-out split not shuffled folds (López de Prado 2018 purge/embargo), and K=4
   production-locked rather than purely BIC-argmin (consistent with Pohle et al. 2017's finding
   that information criteria over-select K) — these should simply be **preserved**, not
   "improved," by the port.
7. `RegimeEngine`'s duck-typed model interface (Python structural typing) needs an explicit Rust
   trait — this is a forced elevation, not optional, since Rust has no equivalent escape hatch.
8. **Found 2026-10-11, not just inspiration but working reusable reference**:
   `tools/observation_vector/FeatureSaliencyEM.h` (a C++ Gaussian-mixture + Feature-Saliency EM
   fitter, Law/Figueiredo/Jain 2004's MML-penalty paper — already named above, §5.1a's elevation
   item 4, as relevant to the separate Elite Feature Set Curation initiative) has its own code
   comment stating its k-means++ seeding was **"ported from `student_t_hmm.py`'s own
   `_kmeans_plusplus_seed_indices` algorithm (same method, not a different one)."** This is a
   genuine, already-landed C++ port of one piece of the Python HMM's own init logic sitting in
   this repo today — `model.rs`'s k-means++ initialization (the
   `test_student_t_hmm_kmeans_plusplus_init.py` parity target, §5.1a) should be built from this
   reference, not re-derived from the Python source cold.

### 5.1b Schema impact of the Python retirement (assessed 2026-10-11, decision #19)

Operator question: "one consequence of transitioning the Student-t HMM from Python to Rust on C++
is that we may no longer need to rely on the `mts_schema.fbs` or any code generated from it...
please assess this angle. You may want to look at lbrnet for clues on what we may no longer need
in the schema for HMM training." Assessed directly against the real `mts_schema.fbs` table
inventory and real `lbrnet` import sites (not inferred) — **verdict: no, overall, but yes for a
specific, confirmed subset.**

**Why the schema stays essential overall**: it serves at least four roles with nothing to do with
where the HMM runs — the main live `Event` stream (port 5555, read by the GUI and any Python
monitor), the trade execution protocol (`TradeRequest`/`Response`/`Close`), the backtesting
artifact format (`.btst`: `RunManifest`/`TradeRecord`/`DecisionEvent`/`PredictionAck`), and the
`.alpha`/`TrainingEvent` format (Transformer training, which stays Python-side — nobody has
proposed porting the Transformer in this initiative). None of those go away because the HMM moved.

**What genuinely becomes unnecessary — confirmed by direct schema read**:
- **`SystemState`**: narrow by schema-comment design — `timestamp_us`/`sequence_id`/
  `bars_since_last_update`, explicitly commented "must match `MarketObservation.sequence_id`." A
  single-purpose companion record for the port-5561 IPC and the on-disk `.context` MO+SS pairing.
  No other consumer found. Clean removal candidate.
- **`HmmRiskState` + `RiskStateUpdate`**: the schema's own comment states it outright — *"Sent by
  Python HMM to guide C++ risk management."* This is exactly what `hmm_step`'s plain-struct FFI
  return value (§5.2) already replaces; `reinfer_action_id` is the same piggyback §5.2 already
  shows is structurally eliminated by `HmmStateIndicator::IsDirty()`, not replaced by new
  machinery.
- **`MarketObservation`** — the *wrapper table* only, not the `ObservationData`/`AsymmetryContext`
  structs it wraps (those stay, see below). Its only remaining uses are the dead live-IPC
  protocol and the dead on-disk `.context` record format (superseded by `.context.parquet`,
  decision #8).

**New finding from the `lbrnet` survey, not previously identified**: `lbrnet/backtest/
backtest_server.py` runs its **own** HMM ROUTER thread — "binds ROUTER on 5561, serves C++ DEALER
requests" — confirmed via direct grep of its own code comments and `HmmRiskStateStart`/
`RiskStateUpdateStart` FlatBuffer-builder imports. This is a **third** Python-side server of the
same dead protocol, parallel to C++'s `HMMClient` (§5.2) and MTS's `_hmm_router_loop` (§5.4
consumer 3's survey) — it was not previously on the deletion list. If `BackTesterStudy.cpp` also
calls Rust `hmm_step` in-process during replay (the same architectural pattern already designed
for live, §5.2), this file's HMM-specific section (its ROUTER thread, `predict_hmm` glue, and the
`HmmRiskState`/`RiskStateUpdate` FlatBuffer-builder calls) becomes dead the same way. **Open, not
yet decided**: whether `backtest_server.py` does anything else (e.g. the Transformer/`PredictionAck`
side of backtest replay) that must survive in the same file — needs its own consumer-boundary
check before deletion, same caveat pattern as §5.2's `HMMClient` sweep.

**Also newly confirmed dead**: `lbrnet/scripts/materialize_context_parquet.py` — its own docstring
states it "materializes a raw-observations parquet cache **alongside a `.context` file**." There is
no `.context` file to materialize from once collection writes `.context.parquet` directly
(decision #8). Clean, nameable deletion candidate, distinct from `materialize_hmm_features.py`
(§5.1a, which stays — it stamps posteriors into `.alpha`, a different job).

**What does NOT become unnecessary (the overclaim this assessment deliberately avoids)**:
`ObservationData`/`AsymmetryContext` are schema structs already embedded directly in the main
`Event` table (confirmed by direct schema read: `Event.observation: ObservationData`,
`Event.asymmetry_context: AsymmetryContext`), published live by `src/messaging/EventSerializer.cpp`
to the GUI regardless of where the HMM itself runs. They stay, full stop.

**A real tension surfaced by this assessment, not resolved here**: `Event` also carries raw
`hmm_state: int8`/`market_climate: int8` fields today — exactly the positional-identity problem
decision #16's additive `RegimeStatus` struct is meant to replace. So this migration *adds* schema
surface (the new `RegimeStatus`) even as it removes other surface (`SystemState`/`HmmRiskState`/
`RiskStateUpdate`/`MarketObservation`) — net schema size is roughly a wash, not "the schema is no
longer needed." Separately, decision #16's own side-finding (`MarketClimate` hand-duplicated across
C++/Python, not schema-generated) argues for *more* schema reliance for that one enum, not less —
a genuine counter-pull worth keeping in view, not papered over.

**Broader consequence, confirmed but not uniquely caused by the HMM move**: `lbrnet/data/
observation_vector_bulk_reader.py` is shared `.context`-reading infra with non-HMM consumers too
(`lbrnet/scripts/tune_risk_gate.py`, `calibrate_event_sample_sizes.py`, several test files) —
confirmed via direct grep of every `MarketObservation`/`SystemState` import site in `lbrnet`. Once
`.context.parquet` is the sole collection-time format, all of these need a Parquet-reading path,
not just the HMM-training-specific files (`materialize_hmm_features.py`, `regime_calibration.py`,
both already flagged §5.1a as "must exist after port" but whose *own* `.context`-reading internals
also need to switch to Parquet). This is a consequence of the already-ruled direct-Parquet decision
(#8), reinforced by this move but not something the HMM migration alone creates or resolves.

### 5.1c Climate/threshold-constant calibration: three tiers, model-dependency, and mandatory
auto-calibration (resolved 2026-10-11, decision #25, G0(d))

Operator question: "where do `gate_metrics.rs`'s constants get set/computed?" led to tracing all 11
threshold-like constants in `regime_engine.py`. They split into three evidence-grounded tiers, not
one undifferentiated pile:

- **Tier 1 — already empirically calibrated and loaded at runtime (4 constants)**:
  `ENTROPY_CHAOS_THRESHOLD`/`TRANSITION_RISK_THRESHOLD`/`MAHALANOBIS_THRESHOLD`/
  `DURATION_RATIO_THRESHOLD`. `lbrnet/scripts/tune_risk_gate.py` calibrates these via walk-forward
  grid search (3 of the 4 are actually swept — `transition_risk` is deliberately excluded from
  `SWEEP_GRID`), writes `models/RiskGateThresholds.json`, and
  `lbrnet/core/cpp_config.py::load_risk_gate_thresholds()` loads it at runtime, falling back to the
  hardcoded `regime_engine.py` constants only if the artifact is absent. `MAHALANOBIS_THRESHOLD`'s
  specific *fallback* value (5.63) has a model-independent origin (a Monte Carlo simulation of the
  weighted-chi-square distribution, dependent only on dimension count/weight scheme — a schema
  fact), but its *calibrated* value still comes from the same model-dependent grid search as the
  other 3.
- **Tier 2 — hardcoded now, documented intent to become calibrated (1 constant)**:
  `TAIL_RISK_THRESHOLD` — a one-time empirical estimate with an explicit code comment pointing at
  `calibrate_hmm_gate_thresholds.py`'s block-bootstrap pipeline, confirmed via grep to NOT yet be
  wired to any runtime-loaded artifact.
- **Tier 3 — pure hardcoded heuristics, no calibration script anywhere (6 constants)**:
  `VELOCITY_EPSILON`/`VELOCITY_EPSILON_EXIT`/`VELOCITY_SMOOTHING_ALPHA`, `ENTROPY_DRIFT_WINDOW`,
  `TAIL_RISK_STREAK_COUNT`, the 4 `COGNITIVE_LOAD_*` constants, the 3 `VARIANCE_RATIO_*` constants.
  Confirmed their only consumer (`live_agent.py`) just imports and reads them directly — no
  override path exists.

**Does calibration need the trained HMM? Confirmed yes, for everything genuinely calibrated** —
both `tune_risk_gate.py` (`--model`, loaded via `RegimeEngine(model_path=...).load_model()`, hard
`RuntimeError` if it fails to load) and `calibrate_hmm_gate_thresholds.py` (`--model`,
`required=True`) take an already-trained model as a mandatory input and run a grid
search/bootstrap over real held-out data classified *through that specific model's* regime
assignments. Tier 3 constants need no trained model at all — nothing to wire.

**Operator's proposal: auto-chain train → calibrate, since calibration only needs data already on
disk.** Confirmed well-supported, not a stretch: `tune_risk_gate.py`'s default `--input` is
`data/training/training_data_events_augmented.alpha`, the *same* corpus `train_student_t_hmm.py`
trains from — no new expensive data-collection step needed. Stronger still:
`train_student_t_hmm.py` already has a `--reserve-calibration-holdout-rows` flag (an "OF-06: no
data leakage" requirement for sign-off runs) whose exact holdout window timestamps get written
directly into the trained model's own `source_info` metadata — the model artifact already carries
the instructions for what data its own calibration pass should run against. No documented
rationale was found anywhere for keeping these two steps manual/decoupled.

**One real reason not to blindly auto-run calibration on every training invocation**: this session
already found unconverged "discovery"/"smoke" 18D training candidates with `dof_` pinned at the
floor — garbage models produced during routine experimentation. Auto-calibrating every one would
waste compute and could leave a plausible-looking calibration artifact for a model that should
never ship. Resolution: gate the auto-calibration trigger on `audit.rs`'s GO/NO-GO verdict (already
ported this session, §5.1a) — **train → audit (GO/NO-GO) → if GO: auto-calibrate against this
exact model's own reserved holdout → model is eligible for live inference; if NO-GO: stop, no
calibration ever runs.**

**Independent confirmation this exact failure class has already happened in `lbrnet`, found via
its `evals/cases/` AutoResearch eval scenarios (not model-performance tooling, but real documented
incidents)**:
- `hmm_stale_empirical_gate_thresholds.json`: a real sign-off run failed its gate
  (`overall_pass=false`) because `HMMEmpiricalGateThresholds.json` was calibrated via a Viterbi
  decode of an **old, superseded** model over **~24% of the current row count**, predating an
  institutional data expansion — proof staleness causes false *negatives* (rejecting a genuinely
  fit model), not just false positives. It also names a third stale-artifact-prone file not
  previously found: `models/context_calibration.json`'s `TOXICITY_THRESHOLD_MAX` (feeds
  `backtest_runner.py`'s Phase 2 kill-switch) — traced to `calibrate_context_thresholds.py` and
  confirmed **not** model-dependent at all (a pure `amihud_illiquidity` percentile over a
  `.context` time window, no HMM load) — it needs a *different* trigger (recalibrate when the data
  corpus materially grows, not when a new model trains) and already has its own
  `context_calibration_provenance.json` sidecar (window/record-count/stats) — a reasonable existing
  precedent, not something broken, but a distinct mechanism from the model-dependent tiers above.
- `phase1_stale_calibration_artifacts.json`: a different pipeline (the Transformer, not the HMM)
  already hit the general class of this bug for real — a v5 training run was OOM-killed before
  `trainer.py::_post_fit()` ran, leaving stale v3/v4-era `logit_adjustment.npy`/`temperature.npy`
  that would have silently corrupted v5's calibration. The shipped fix is simpler than a
  content-hash scheme: `_post_fit()` writes a `calibration_artifacts_provenance.json` sidecar
  (model path + UTC timestamp) **atomically alongside** the calibration artifacts, and
  `live_agent.py::_calibration_artifact_provenance_ok()` refuses to load calibration files if that
  sidecar is absent or its recorded `model_path` doesn't match — correctness comes from
  write-together atomicity, not independent hash verification.

**Resolution for the Rust port — replaces the former "manifest vs. crate constants" framing
entirely**, which was a false dichotomy:
1. All 11 constants become fields on the climate-classification config struct with sensible
   hardcoded defaults (the same `DEFAULT_*`-naming fix already applied to `dof_min`/`dof_max`) —
   never bare module constants read directly by the math.
2. The 4 Tier-1 constants (+ `TAIL_RISK_THRESHOLD` proactively, ahead of Python's own
   still-unwired intent) get a genuine manifest-override slot, populated by the mandatory
   train → audit → auto-calibrate chain above — not an optional nice-to-have.
3. The calibration artifact and a provenance sidecar (model identity + timestamp) are written
   atomically together, **porting the `trainer.py`/`live_agent.py` pattern**, not inventing a new
   one. The Rust loader hard-refuses to use a calibration artifact whose provenance is absent or
   mismatched for these model-dependent thresholds — it does not silently fall back to the
   hardcoded default, which is exactly the live gap found in current Python
   (`cpp_config.py::load_risk_gate_thresholds()` only checks `path.exists()`, with a soft log
   warning, no identity check at all; `models/RiskGateThresholds.json` does not currently exist on
   disk in this repo, meaning live Python is *currently* running 100% on Tier-1 fallback defaults
   with nothing enforcing calibration ever ran).
4. `context_calibration.json`'s `TOXICITY_THRESHOLD_MAX` is flagged as a related but *separate*
   artifact family — data-window-dependent, not model-dependent — needing its own recalibration
   trigger, not folded into the train/audit/calibrate chain.
5. Tier-3 constants stay plain hardcoded struct-field defaults — no manifest slot, no provenance
   gate needed, since nothing calibrates them today.
6. `MAHALANOBIS_THRESHOLD`'s own fallback-value fragility is flagged explicitly: it's a one-time
   Monte Carlo result tied to the *current* 16-dim/5-bullet-dim/1.30-weight configuration — if the
   observation vector's dimension count or bullet-dim weighting changes (the still-open Phase 2
   Feature-Saliency EM question, 5 of 18 dims flagged zero-saliency), this fallback itself goes
   stale and needs re-deriving, not just re-reading from a file.

**Memory-conservative pattern check (operator's second ask, same turn)**: confirmed the training
side already did the "investigate before preserving/stripping" work decision #24 calls for, not
something still owed. `train_student_t_hmm.py`'s `stream_chunk_rows` (and the dead-physics-cast
chunk size) were explicitly re-benchmarked on the new Ryzen 9950X/Puget hardware (dated
2026-09-15, in-comment benchmark tool + logged-output citations) — the old Xeon-era default
(250,000) was found *too large even on the new hardware*; smaller chunks (62,500) are measurably
faster, not merely safer, so chunking stayed and was tightened further, not relaxed.
`tune_risk_gate.py`/`calibrate_hmm_gate_thresholds.py`'s own memory-conscious patterns (mmap-based
zero-copy streaming parse, FFT-based O(n log n) autocovariance) are universally sound engineering
with no old-hardware-specific comments found — nothing stale to revisit there either. The one real
OOM incident found (the Transformer `phase1_stale_calibration_artifacts.json` case) predates the
hardware upgrade, but its lesson is pipeline-atomicity correctness, not memory-conservative-code
complexity to strip — any crash/interrupt/bug could trigger the same failure regardless of RAM, so
it must be built into the new auto-chained HMM pipeline either way (point 3 above).

**Open, not yet decided**: `tune_risk_gate.py` also runs its own internal walk-forward meta-tuning
(`--train-months`/`--val-months`/`--holdout-months` folds) beyond the single calibration-holdout
slice, for robustness against overfitting the threshold search to one time window. If the reserved
calibration holdout is short, there may not be enough time depth inside it to run those folds —
whether the Rust auto-calibration step needs that same walk-forward layer, or whether a single
holdout evaluation suffices for Phase 1, is not yet decided.

### 5.1d Regime Manager migration: an already-designed, partially-completed cross-repo initiative
the Rust unification must finish (discovered 2026-10-11, decision #27)

Operator recollection: "I believe we were in the middle of implementing the C++ HMM State Manager
when we pivoted to Rust." **Confirmed true, not just plausible** — traced to a specific,
authoritative source, not inferred from fragments.

**Canonical source**: `lbrnet/docs/architecture/STUDENT_T_HMM_MASTER_PLAN.md`'s own living-backlog
has a section titled **"Regime Manager migration — remaining stages not started"**, stating
outright: the C++ `RegimeManager` facade, `regime_engine.py` cleanup, Python Transformer-
coevolution accessors, and State Manager API surface are **complete**; **not started**: (a)
`scoring.py`/`regime_manager.py` algorithmic redesign ("HIGH STAKES: quant research, not a
mechanical refactor — do not treat as a routine pickup"), (b) **"Cross-repo C++ follow-on (needs a
separate `MindfulTrader`-side session)"** — precisely this conversation, (c) final removal of
superseded code paths. One item on that list — "an 8-file C++ call-site migration onto the
`RegimeManager` facade" — **was actually completed**, by this very Rust-migration session
(2026-10-10, confirmed via `git log`) — the `lbrnet` backlog doc simply hasn't been told; worth
syncing back if `lbrnet` is ever revisited, not actioned from here (standing "leave `lbrnet`'s own
pile as-is" instruction).

**Reassurance on the operator's stated worry, checked directly, not assumed**: the uncommitted
Python work this section describes is physically present and intact in the `lbrnet` working tree
on THIS machine right now (`git status`/`git diff` read directly) — nothing was lost on a prior
workstation.

**The uncommitted evidence**: `lbrnet/models/regime_manager.py` + `tests/test_regime_manager.py`
(untracked, mtime 2026-09-18) — "the Python-side analogue of MindfulTrader's `RegimeManager`,"
explicitly documented as "two independent implementations of the same regime-decision contract,
not one importing the other." **Superseded by later uncommitted work**: `regime_engine.py`
(844-line uncommitted diff against the last commit) and `regime_registry.py` (289-line uncommitted
diff), both mtime 2026-09-22 — four days later.

**The real architectural fix this later work makes, and the bug it fixes**: `RegimeState` gains a
full "State Manager API" — `get_hmm_multiplier`/`get_climate_multiplier`/
`get_climate_conviction_boost`/`get_climate_risk_multiplier`/`get_half_life_scale`/
`write_regime_features`/`state_from_posteriors` — every method resolving via `profile_label`/
`climate` (the per-retrain-stable, intrinsic-signal-derived archetype `build_regime_registry()`
computes fresh each retrain), **never** raw `state_id`. Its own docstring names the thing it
replaces directly: *"Batch/offline consumers should call this instead of independently
re-implementing state_id -> profile -> label resolution (**the pattern `regime_manager.py`
currently duplicates**)."* Root cause fixed: `HMMStateEnum.from_id(state_id)` assumed a Hungarian-
alignment reorder step guaranteeing `raw_id` stability across retrains — that step was **removed
2026-09-21** (confirmed empirically broken: only 1 of 4 states still matched the assumed
convention on the real production model). `regime_registry.py`'s companion diff deletes the whole
alignment machinery (`compute_hmmstate_affinity`/`compute_state_summary_for_alignment`/
`align_hmm_states_to_canonical_enum`, ~165 lines — confirmed via its own now-deleted regression
test fixture, `tests/test_regime_registry_hmmstate_affinity.py`), adds a clean `_ClimateEnum` base
class (`from_id`/`handle_missing`/`offset()`), and fixes a documented "classification with a
reject option" (Chow 1970) gap in `build_regime_registry()`'s `GAUSSIAN_STABLE` fallback (it used
to be an unconditional default; now requires positive quiet-signal evidence, falling back to
`SHANNON_CHAOS` — max-entropy, most risk-averse — when genuinely ambiguous).

**This fix was never carried to C++ — confirmed directly this session, not assumed.**
`HmmStateIndicator::SetState()` (`include/Indicator.h`) still does a raw
`static_cast<HMMStateEnum>(stateId)`; `RegimeManager.cpp` still branches directly on
`HMMStateEnum::PARETO_MOMENTUM`/`GAUSSIAN_STABLE`/`COILED_SPRING`/`GAUSSIAN_FRAGILE`;
`MarketClimateIndicator::UpdateContext()` (`src/Indicator.cpp`) takes `HMMStateEnum hmmState`
directly and folds it into its own independently-evolved native kurtosis/entropy/Hurst "physics
veto" hybrid cascade — a genuinely separate classifier from Python's `regime_registry.py`/
`regime_engine.py`, not a passthrough of it. This is exactly the still-open "Cross-repo C++
follow-on" item.

**Additional still-unfixed bugs of the same class, confirmed via the master plan, not yet fixed
even on the Python side**: `tune_risk_gate.py`, `build_directional_alpha.py`'s
`_attach_regime_to_event()`, and `materialize_hmm_features.py::serialize_enriched_training_event()`
all still do positional-`HMMStateEnum`/hardcoded-4-slot reads. The last is the most serious: it
bakes K=4 directly into the **`TrainingEvent` wire schema itself** as four fixed named fields
(`regimeProbDrift`/`regimeProbFlush`/`regimeProbCoil`/`regimeProbSweep`), unlike the live
`HmmRiskState.probability_vector: [float]` wire field, which is already a correctly-sized dynamic
vector. The master plan states plainly this "requires a schema migration... that also touches the
C++/MindfulTrader side."

**Cross-check against this session's own decisions #20/#21** (`RegimeStatus` widening to carry
`transition_risk`/`expected_duration`/`dof` via `rust/schema`'s generated FlatBuffers struct
types): related, but **not the same fix** — #20/#21 is about the live `Event`/`TrainingEvent`
`RegimeStatus` struct gaining fields, not about replacing the fixed-4-slot `regimeProbDrift/
Flush/Coil/Sweep` positional array with a dynamic vector. Flagged explicitly rather than silently
merged: when open question #16 (`TrainingEvent`'s regime-field superset question) is finally
decided, this K=4-hardcoding bug should be fixed in the SAME pass — the Rust `TrainingEvent`
generation work will already be touching this exact surface, making this the cheapest possible
moment, not a separate future migration.

**Recommendation for Stage G3/G4 (`registry.rs`/`regime_engine.rs`)**:
1. `regime_engine.rs`'s live classification/lookup design adopts `profile_label`/`market_climate_id`-based
   resolution as canonical, never raw `state_id`/the legacy 4-state `HMMStateEnum` — this directly
   completes the "Cross-repo C++ follow-on" `lbrnet`'s own backlog has been waiting on.
2. Do **not** port `regime_manager.py` (confirmed superseded) or C++'s current `RegimeManager.cpp`/
   `MarketClimateIndicator` internals as reference designs — use the later, more complete
   `RegimeState`-method design (`state_from_posteriors`, `write_regime_features`,
   `get_hmm_multiplier`, etc.) as the reference instead. Port the DESIGN, not the code — Python
   itself won't run any of this once decision #18 lands.
3. ~~Explicitly undecided, flagged not silently resolved: does `MarketClimateIndicator`'s
   independently-evolved native kurtosis/entropy/Hurst "physics veto" hybrid logic become an
   explicit additional safety layer inside `regime_engine.rs` (properly reconciled, no more positional-
   cast coupling), or is it retired as redundant with what the HMM's own classification already
   encodes? Needs an explicit operator call, not an engineering default either way.~~ —
   **RESOLVED 2026-10-11 (decision #29)**: retire. Traced each of `MarketClimateIndicator`'s three
   inputs to their real source, not assumed: `hurst` (`ContextManager.cpp:647`,
   `m_localRiskContext.hurstExponent = obs[OBS_HURST_EXPONENT]`) is confirmed **100% redundant**
   — literally `ObservationData.hurst_exponent()`, the exact dimension the HMM already trains on
   and jointly weighs against 17 others; a fixed single-axis threshold cannot improve on a
   multivariate Student-t posterior that already sees that same value. `kurtosis`/`entropy` are
   genuinely different measurements (`ContextManager.cpp`'s Moors 1988 octile kurtosis + a 10-bin
   Shannon entropy in bits, vs. the HMM vector's Kotz & Nadarajah 2004-lineage
   `fast_taleb_kurtosis` + Lempel-Ziv complexity) — not literally duplicated data, but no evidence
   this divergence was ever a deliberate ensemble/robustness design; it reads as organic drift
   between separate initiatives (the older `TailRiskEngine`/`InformationEngine` physics work vs.
   the newer activity-clock observation-vector work), not intentional diversity worth preserving
   as-is.
4. Whatever `TrainingEvent` wire-schema work lands for decision #20/#21/open-question-#16 should
   also close out the `regimeProbDrift/Flush/Coil/Sweep` K=4-hardcoding bug in the same pass.
5. **New finding while resolving item 3, 2026-10-11 — not a design question, a confirmed live
   bug**: `MarketClimateIndicator` has TWO independent writers racing for the same state.
   `HMMClient::HandleBinaryResponse`'s `climateInd->SetClimate(climate_id)` (`src/HMMClient.cpp`
   line ~782) writes the wire-received, REAL, trained `regime_registry.py` classification —
   asynchronously, only when a new HMM response arrives (Mahalanobis-gated, not every tick).
   `TripleScreen3.cpp`'s `climateIndicator->UpdateContext(riskCtx, currentHmmState)` writes the
   NATIVE physics-hybrid heuristic — synchronously, **every tick**. Because the native write fires
   far more often, it wins almost every time: the trained model's own classification is
   overwritten within microseconds of arriving, before `RegimeManager::CurrentClimate()` (feeding
   pattern multipliers, momentum/mean-reversion alignment, grade thresholds) can act on it in
   practice. **This means the live C++ system's actual climate-conditioned decisions today are
   driven almost entirely by the cruder heuristic, not by the trained model, regardless of which
   one is nominally "supposed to" be authoritative.** Fixing this (removing the native write path,
   making the HMM State Manager — `regime_engine.rs` — the sole writer) is part of the Rust cutover
   (plan Stage G7), not an optional cleanup.

### 5.2 Target architecture

```
rust/student_t_hmm/        student_t_hmm        pure crate: model, emission, filter, tail diagnostics, regime engine
                                 (classification + model-risk state), EM trainer, artifact I/O
rust/ffi/        (feature "hmm") hmm_init / hmm_step / hmm_reset, staticlib + cbindgen header
rust/py/         (submodule)     PyO3 for lbrnet: train, filter, batch_causal_filter, load/save artifact
rust/tools/      hmm_tool       CLI: train, posteriors, verify-golden, export
```

**`student_t_hmm` internal module layout** (expanded 2026-10-11 per §5.1a's full-stack survey;
each module names the `lbrnet` file(s) it ports, so the file-by-file verdict table in §5.1a stays
the single source of truth for scope — this is only the resulting Rust shape):
```
student_t_hmm/src/
  model.rs               -- emission density, Hamilton forward-backward, Viterbi decode, predict
                             (student_t_hmm.py's inference side)
  train.rs                -- EM/MAP-EM loop, ECME dof (Brent's method), multi-restart screening,
                             BIC-based K-selection (student_t_hmm.py's fit()/fit_streaming() +
                             train_student_t_hmm.py's orchestration methodology, not its CLI shape)
  regime_engine.rs                -- RegimeEngine equivalent: stateful live inference, climate overrides,
                             model-risk tracking (regime_engine.py)
  registry.rs               -- MarketClimate classification cascade (regime_registry.py, the
                             canonical 5-state taxonomy per §5.1a's ruling)
  gate_metrics.rs            -- quality-gate statistics, ported first (regime_gate_metrics.py)
  bootstrap.rs                -- parametric bootstrap (hmm_parametric_bootstrap.py), sharing one
                             helper with gate_metrics.rs instead of the Python duplication
  calibration.rs                -- Geyer tau_int / stationary block-bootstrap / mean-residual-life
                             methodology (calibrate_hmm_gate_thresholds.py) — the `arch`-crate gap
                             (§5.1a) must be resolved here before this module is more than a stub
  runtime_calibration.rs          -- temporal_half_life_sec from the transition matrix
                             (regime_calibration.py)
  audit.rs                         -- deterministic GO/NO-GO JSON gate reader (audit_hmm_release.py)
  artifact.rs                       -- manifest + weights serialization, see below
```
Explicitly out of scope for this crate (§5.1a): the orphaned 4-file Meta-HMM "trainer run plan"
sub-cluster (deleted 2026-10-11, not just excluded), the 3 benchmark files (drop),
`regime_focal_loss.py` (Transformer-training concern, not HMM), `student_t_hmm_gpu.py` (likely
drop, pending a real speed comparison against Rust's CPU EM). **Not** out of scope, corrected
2026-10-11: the live Meta-HMM "recommendation/ledger" sub-cluster (§5.1a) — feeds
`risk_gate_orchestrator.py`, needs its own port/keep decision, separate from this crate's scope
question since it's risk-gate-adjacent, not HMM-training-adjacent.

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
**Renamed 2026-10-12 (decision #30): `HmmStep` → `RegimeState`** — the return type's content is the
live regime engine's classification + diagnostics, not a bare HMM math step; "step" stays only on
the producing verb (`RegimeEngine::step()`). The rest of this section's prose below still says
`HmmStep` in several places (preserved as written, historical reasoning) — read every occurrence as
`RegimeState`.

**`HMMClient` is fully obsolete the moment this lands — not just port 5561, the whole 966-line
class (`include/HMMClient.h`/`src/HMMClient.cpp`) — confirmed by operator question, 2026-10-11,
"do you see that HMMClient may not be applicable," traced against the real current code.** Nearly
all of its machinery exists solely to handle cross-process async IPC and evaporates once the call
is synchronous and in-process:
- The DEALER socket, worker thread, and inproc PAIR signal (`m_worker`/`m_inproc_signal`) — a
  synchronous Rust call has no socket and runs on the calling thread.
- Sequence-ID staleness rejection (`m_lastSentSequenceId`/`returnedSeq < lastSentSeq`) and
  timestamp-drift validation (`kMaxStaleTimestampDriftUs`/`kMaxFutureTimestampLeadUs`, 250ms/50ms
  windows) — both exist only because an async network response can arrive out of order or late; a
  synchronous call's return value is *definitionally* the answer to *this* call, every time.
- FlatBuffer `Envelope`/`RiskStateUpdate` deserialization and probability-vector
  sanitization/renormalization (`HandleBinaryResponse`'s ~290 lines) — wire-format concerns that
  don't exist when the "wire" is a plain Rust struct returned by value across an FFI boundary.
- Host failover (`AttemptHostFailover`/`m_hostCandidates`) — nothing to fail over to; the model is
  linked into the same DLL.

**Concrete replacement, traced against the real call site
(`ContextManager::EmitLiveContext`/`EmitTrainingContext`, `src/ContextManager.cpp`)**:
`HMMClient::Instance().RequestUpdateAsync(currentObs, barsSinceLastUpdate, sequence_id, now_us)` is
replaced in both functions by a direct, synchronous call into the `ffi` crate's `hmm_step` wrapper
(same `#[cfg(feature = "student_t_hmm")]`-gated shape `observation_vector`'s wrappers already use),
fed the exact same already-Mahalanobis-gated `currentObs`/`bars_since_last_update` C++ already
computes — **no new trigger logic, no new gate**; `otg::ObservationTriggerGate`'s existing
Mahalanobis significant-change decision (§6.1) is unchanged and remains the sole arbiter of *when*
to call. `hmm_init`/opaque-handle lifecycle (the model's Hamilton-filter belief and model-risk state
— variance ratio, black-swan streak, duration tracking — persist across calls, so this cannot be the
stateless-function pattern used for every `observation_vector` dim; it is the init/step/reset
opaque-handle pattern this section already names) follows the same lifecycle discipline as every
other long-lived `rust/ffi` resource (§6.5): init on `sc.LastCallToFunction`-adjacent study load,
step every trigger, explicit shutdown, never a static destructor.

**`HmmStep`'s raw output is deliberately minimal — it is NOT where `risk_multiplier` is computed.**
Traced against `HandleBinaryResponse`'s real current body: `risk_multiplier` is computed by
`RiskManager::Instance().ComputeInstitutionalRiskMultiplier(state_id, probs, entropy,
transition_risk, dof, unknown_posture)` *downstream of* the HMM's own output, not emitted by
`RegimeEngine.infer` itself. `hmm_step`'s return value should therefore carry only the model's own
raw outputs (state_id, `unknown_posture` validity flag, probability vector, dof, mahalanobis
distance, tail_weight, expected_duration, entropy, transition_risk, confidence, climate) and leave
`risk_multiplier` to whichever layer owns that business logic today (`RiskManager`, C++) — the same
"pure core, thin edges" separation already proven for `observation_vector`, and a candidate for its
own future Rust port (this session's `execution` crate survey already named `RiskManager` as
in-scope for pillar 1, §8.3) rather than folded into the HMM's own contract.

**The Python-side "atomic regime-triggered re-inference" piggyback (today's
`regimeUpdate->reinfer_action_id()`, written in the same `RiskStateUpdate` message so a Transformer
re-inference lands on the same worker-thread message as the HMM state, closing a coherence gap
between stale-regime and fresh-regime Transformer actions) is not something this port needs to
reproduce — it is structurally eliminated by two already-ruled/already-built pieces, not replaced
by new machinery:**
1. **"Transformer coupling, inverted, one-way"** (this section, above): once the Transformer reads
   regime features off the event stream C++ publishes, instead of running its own `RegimeEngine`,
   there is no second process whose knowledge of "did regime change" can lag — whatever regime
   state C++ just computed synchronously *is* what the next published event carries, every time.
2. **`HmmStateIndicator::IsDirty()` already exists and already means exactly "did regime change
   since the last `SetState` call"** (`include/Indicator.h`'s `Indicator<T>::Update()`: sets the
   dirty bit iff `newValue != m_value`; `HmmStateIndicator::ShouldTrigger() { return IsDirty(); }`
   already present). The instant `stateInd->SetState(...)` returns from the new synchronous call,
   `stateInd->IsDirty()` tells C++ authoritatively whether to request a fresh Transformer
   inference — via the existing, separate, already-working `TradeExecutionServer`/`PredictionAck`
   channel (`InferenceManager::Instance().MutablePrediction()`'s normal update path, unaffected by
   this change) — with zero added latency and zero new field needed in `HmmStep`'s own contract.

**Deletion**: `HMMClient.h`/`.cpp` (966 lines total) become dead code the moment the call sites
above are repointed, and should be deleted in the same change — not kept "just in case," per this
repo's standing dead-code-deletion default. **Open, not yet verified**: whether any consumer other
than `HMMClient::HandleBinaryResponse` needs the `RiskStateUpdate` FlatBuffer message itself (e.g. a
GUI subscriber reading live HMM state) — port 5561 the *socket* is already ruled deleted (§7's port
table), but if something else reads this message shape for display purposes, that becomes a
publish-only addition to the existing port 5555 event stream, not a reason to keep `HMMClient`.
Needs a consumer sweep before the deletion step, not before the design above.

**`HmmStep`'s state identity MUST be rule/label-based, not a raw positional `state_id` — ruled
2026-10-10, found while investigating this initiative's own groundwork.** `HMMStateEnum` (both
`lbrnet/lbrnet/core/rc_enums.py` and its C++ mirror `include/rc_enums.h`) is already mid-retirement
on both sides of this cross-repo system: the Python docstring says outright *"HMMStateEnum itself
is scheduled for full removal; do not rely on it for new code"* (cross-repo design doc:
`lbrnet/docs/architecture/STUDENT_T_HMM_MASTER_SPEC.md`/`_PLAN.md`, §9/"Regime Manager migration"),
because fixed positional IDs assume a retrained model's raw EM state ordering matches the
convention — label-switching means that's only true if an alignment step runs and succeeds, and an
empirical check on one real retrain found only 1 of 4 states actually self-consistent with it; 3
more live position-indexing bugs of the same class were found elsewhere (`tune_risk_gate.py` etc.).
The replacement already exists on both sides, built but not yet wired in: C++'s
`include/RegimeManager.h`/`.cpp` (188 lines, shipped additively, **zero live callers**, confirmed by
direct grep) exposes rule/label-based named methods (`IsMomentumAligned`, `RequiresPassiveExecution`,
`GrantsMomentumBoost`, `GetPatternMultiplier`, ...) behind an opaque `RegimeSnapshot` Memento token
that **never exposes the raw `HMMStateEnum` to callers**; Python's `lbrnet/lbrnet/models/
regime_manager.py` (2026-09-18) is the independently-built counterpart (not a literal port of the
C++ file — kept in sync via `docs/HMM_REGIME_MANAGER_COORDINATION.md`, a 1045-entry cross-repo
handoff log). **Consequence for this port**: if `student_t_hmm`/`hmm_step()` simply reproduces today's
wire schema's raw `primary_state: int8` as its output shape, it re-bakes the exact bug class
already being retired into the new architecture. `HmmStep`'s design must emit identity in a form
`RegimeManager`-style consumption can stay rule/label-based on — either the state-alignment-to-
canonical-convention step moves into `student_t_hmm` itself (so a raw id is safe again, alignment
cost tracked per-model the way `ModelManifest.json`'s `state_map[*].alignment_cost` already does
today), or `HmmStep` carries enough label/profile information directly so callers never need to
re-derive identity from a positional index. Exact shape: open, needs its own design pass before
`student_t_hmm`'s `RegimeSnapshot`-equivalent is finalized — not decided in this edit.

**RESOLVED 2026-10-12 (decision #30, §2) — full design + shipped implementation:**
`regime_engine.rs` (`rust/student_t_hmm/src/regime_engine.rs`). Short version: option 1 above is what happened —
the state-alignment-to-canonical-convention step already lives in `student_t_hmm` (`registry.rs`'s
`classify_states()`, shipped earlier this session), so `RegimeState` (renamed from `HmmStep`)
carries both `state_id` (opaque, positional, log/dirty-bit/indexing only) AND `profile_label`/
`climate` (the resolved, rule/label-based identity) — never requiring a caller to re-derive
identity from the positional index. No separate `RegimeSnapshot`-equivalent wrapper type was
needed in the end; see decision #30 for why.

**Companion C++-side groundwork, now in scope (ruled 2026-10-10)**: the 8-file call-site migration
(`PositionManager`/`RiskManager`/`Scoring`/`TripleScreen3`/`Indicator`/`InferenceManager`/`Trade`/
`TripleBarrierExitManager`) from direct `HMMStateEnum` reads onto the already-built `RegimeManager`
facade — flagged "not-yet-approved" in `lbrnet`'s own plan doc, **now approved and in progress**
(plan §4a tracks status). This is independently valuable (retires a real, already-demonstrated bug
class) and directly de-risks `student_t_hmm`'s design: doing it first proves out the label-based
consumption pattern against the *existing* HMM before the Rust port has to get it right from a
cold start.

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

**The ~20-file `test_student_t_hmm_*` suite (full list: §5.1a) is the enumerated regression
checklist for this parity gate, not a vague "match the math" aspiration** — each test name maps to
a specific, previously-real numerical bug (density normalization, τ/σ² covariance conversion, the
covariance-update denominator, ECME DOF staleness, the mean-prior cross-term, the min_covar/
global_var anchor relationship, one-sided relative convergence, L_MAP-not-raw-LL checkpoint
restore, k-means++/representative-sampling initialization, chunked-streaming memory/sequence-
boundary correctness). The Rust port is not considered parity-complete until it has an equivalent
test for every item on that list, and the one genuinely open item (L_MAP decline, item 15) is
carried forward as a documented open question rather than silently closed.

### 5.4 Extension — RULED (operator, 2026-10-10; direction set 2026-10-09, work begins after plan §4's Stage B-C)

**Ruling, stated in the operator's own words (2026-10-10)**: *"We will train, and use for prediction,
the Student-t HMM on the [Rust/native] side. Whatever HMM info needed to train the transformer will
be added to the `.alpha` file. Whatever HMM info needed by Transformer to make inferences will be
sent with the FlatBuffers Event via ZMQ."* This settles both halves of the goal below as decided,
not just directionally correct — the offline `.alpha` flow and the live Event flow (§5.2's
"Transformer coupling, inverted, one-way") are both confirmed, not merely proposed.

**The goal, stated plainly**: `EventDataCollectorStudy.cpp` writes `.context.parquet` directly during
collection (replacing today's two-step "C++ writes `.context` as a FlatBuffers stream → a separate,
later Python pass materializes it to Parquet" — this repo already writes Parquet directly from native
code elsewhere, `tools/scid_processing/scid_to_ticks_parquet.cpp`, so there is real precedent).
**Stronger, more directly-applicable precedent found 2026-10-11** (operator: "does the `tools`
folder have anything we can use for this?"): `tools/market_data_replay/MarketDataReplay.cpp`
already implements exactly this pattern, not just an analogous one — its own header comment states
"writes a flat Parquet file ... direct from C++ — no `.context` intermediate," using chunked Arrow
column builders (`ChunkBuffers`, `NewBufferedRowGroup()`+`WriteRecordBatch()` at 2M-row chunks to
bound memory) — the template to copy, not merely a precedent to cite. Two gaps to close, not
reasons the precedent doesn't apply: (a) it's an offline tool reading a static `mes_ticks.parquet`,
not hooked into live Sierra Chart collection, and (b) it has no `RiskGateContext` columns (no live
risk-gate state available offline). `tools/context_pipeline/context_to_parquet.cpp` supplies the
exact target column schema to hit instead (18D `ObservationData` + 15 `RiskGateContext` floats +
`regime_duration`/`is_valid`/`snapshot_timestamp_us`/`context_available` + `sequence_id`/
`timestamp_us`/`bars_since_last_update`) — this is the shape `lbrnet`'s training pipeline already
expects, so matching it means nothing downstream notices the collection-time change. The concrete
interception point, traced directly: `src/EventDataCollectorStudy.cpp`'s
`LBRFileManager::Instance().LogSynchronizedEvent(*eventT, *eventT->observation,
*eventT->asymmetry_context, now_us, stitched_regime_tenure, &rgc)` call is the single site that
currently writes the binary `.context`/`.alpha` MO+SS records live — this is where a direct Parquet
writer replaces it. **Caveat, unchanged by this finding**: all of the above (`MarketDataReplay.cpp`,
`context_to_parquet.cpp`, `scid_to_ticks_parquet.cpp`) are C++ tools built against the mamba `mts`
env's Arrow/Parquet libs, not Rust — this doesn't change the separately-confirmed finding (plan §4)
that `rust/Cargo.toml` has zero `parquet`/`arrow` dependency today; it gives a working C++ reference
implementation of the exact pattern to port into `rust/student_t_hmm`'s data-ingestion path, not a
drop-in Rust dependency.
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
   `lbrnet.feature_spine.MarketObservationData` — confirmed via direct code read). **Not resolved
   by the 2026-10-10 ruling above** (that ruling addresses what the Transformer needs — training
   via `.alpha`, live inference via the Event — not what the GUI displays) **— resolved 2026-10-11
   instead, by a separate, later ruling, see below.**

**Status: RULED (operator, 2026-10-10) for consumers 1 and 2.** Still explicitly depends on §5.2's
in-process Rust HMM landing first — writing `.alpha` records "fully populated the first time"
requires the HMM to already be callable in-process from `EventDataCollectorStudy.cpp`, which doesn't
exist yet; the ruling settles *what* happens once it does, not the sequencing.

**Consumer 3 (GUI display), RESOLVED 2026-10-11** (operator: "analyze what `../MTS/*.py` needs for
display purposes and propose a message sent by the regime manager in Rust"; "we can change the
schema... to conform to our new direction"). Full consumer survey of `MTS/regime_view.py`,
`MTS/live_agent_view.py`, `MTS/zmq_client.py`, `MTS/websocket_broadcaster.py` — a bigger finding
than the open question implied:

**MTS is not merely an HMM consumer — it is the live HMM inference *server*.**
`zmq_client.py`'s `_hmm_router_loop()` binds the ZMQ ROUTER on port 5561 that `HMMClient` (the C++
DEALER) talks to, receives the `MarketObservation`+`SystemState` request pair, calls
`self.live_agent.predict_hmm(...)` (the real `lbrnet.LiveAgent`, imported directly into the Dash
process), and replies with the `RiskStateUpdate` envelope (`_build_risk_state_envelope`). This means
§5.2's "`HMMClient` fully obsolete" finding has a mirror-image consequence on the MTS side: the
entire Python HMM-serving path in `zmq_client.py` (`_hmm_router_loop`, `handle_hmm_flatbuffer_message`,
`_predict_hmm_from_feature`, `_build_risk_state_envelope`, and `feature_spine`'s MO+SS
pairing-by-sequence_id, which exists solely to reconstruct the HMM's input feature vector from two
independently-arriving async messages) becomes dead code too, not just a display-field question.

**Real display consumers, field-by-field** (confirmed via direct grep, zero consumers found for
several fields previously assumed relevant):

| Field | Consumer | Needed? |
|---|---|---|
| `climate_name` | `regime_view.py`, `live_agent_view.py` header/panel | Yes |
| `entropy` | same | Yes |
| `velocity` (entropy rate-of-change) | `live_agent_view.py` dead-zone check | Yes |
| `entropy_drift_alerts` | `live_agent_view.py` drift-warning banner | Yes |
| `observation_vector` (lz/nhnl/bias sub-fields) | `regime_view.py` | Yes — see below |
| `timestamp_us` | `regime_view.py` freshness label | Yes (reuse `Event`'s own) |
| `burnin`/`burnin_remaining` | `regime_view.py` mode tile | Yes, but needs a different source (below) |
| `source` | `regime_view.py` (cosmetic) | No longer meaningful, see below |
| `state_id`/`primary_state` | none (carried in cache, never rendered) | **No** |
| `transition_risk`, `dof`, `mahalanobis_distance`, `tail_weight`, `expected_duration`, `probability_vector` | none anywhere in MTS | **No** |

**Three things that currently work but would silently break or stop making sense, found by this
trace — not just a field list, real redesign content:**
1. **`regime_view.py`'s `observation_vector` display is sourced *exclusively* through the
   now-obsolete `MarketObservation` HMM-request delivery** (`_predict_hmm_from_feature` attaches
   `prediction['observation_vector']` from the array it received over the HMM-router path). The
   main `Event` table **already has** an `observation: ObservationData` field (`mts_schema.fbs`,
   "Added for Elite v2.5") published every tick on the existing port-5555 stream — but confirmed via
   grep that MTS **never reads it** (only the HMM-router-specific `MarketObservation.Observation()`
   is ever decoded, `zmq_client.py:663`). Once `HMMClient`'s dedicated `MarketObservation` delivery
   is deleted, this display goes blank unless MTS starts reading the field that's already on the
   wire today.
2. **`burnin`/`burnin_remaining` measures the wrong restart event.** Today it's
   `self.hmm_burnin_required: int = 40`, a counter reset whenever the **MTS/Dash Python process**
   restarts — guarding against trusting a freshly-reloaded Python `LiveAgent.regime_engine` before
   it's seen 40 observations. Once HMM state lives in the long-running C++/Sierra process (which
   restarts far less often than the GUI), this specific "Python process just restarted" event no
   longer corresponds to anything meaningful at the new source of truth. If a burn-in/warmup
   display is still wanted, it must be computed and published by the Rust HMM/C++ side (tracking
   *its own* post-reset observation count), not re-invented locally in MTS.
3. **`source`'s dynamic meaning disappears.** Today it distinguishes several *possible* Python code
   paths that could have produced a `regime_stats` update (explicit HMM message vs. a direct
   diagnostic fallback vs. a `regime_engine.get_stats()` fallback). Once there is exactly one path
   (Rust HMM → C++ publish → MTS subscribe), this field has nothing left to distinguish.
4. The **"flush pending re-inference after the HMM reply is on the wire"** ordering hack
   (`self.live_agent.flush_pending_reinference()`, tied to `_hmm_router_loop`'s send timing) is
   superseded by §5.2's finding: `HmmStateIndicator::IsDirty()` + the existing, separate
   `TradeExecutionServer`/`PredictionAck` channel already closes this coherence gap with zero added
   machinery.

**Proposed design: additive `RegimeStatus` struct on the existing `Event` table, not a new port**,
consistent with the already-ruled "Transformer coupling, inverted, one-way" mechanism (§5.2) —
operator confirmed schema changes are in scope ("we can change the schema... to conform to our new
direction"):

```fbs
struct RegimeStatus {
  climate: int8;                      // MarketClimate enum -- rule/label-derived (the Rust regime
                                       // engine's own classification), NEVER the raw HMM state_id
  entropy: float;                     // normalized Shannon entropy, [0,1]
  entropy_velocity: float;            // renamed from today's 'velocity' -- self-documenting
  entropy_drift_alert_count: uint32;
  burnin_active: bool;                // the Rust HMM's OWN post-reset warmup, not MTS's
                                       // process-restart counter (finding 2 above)
  burnin_bars_remaining: uint16;
}

table Event {
  ...
  observation: ObservationData;       // already exists -- MTS needs to start reading this, not a
                                       // schema change
  regime_status: RegimeStatus;        // NEW, additive, non-breaking
}
```

Deliberately **excluded from `RegimeStatus`** (zero *display* consumers, confirmed): `state_id`,
`mahalanobis_distance`, `tail_weight`, `reinfer_action_id`, `probability_vector`. These genuinely
stay C++/Rust-internal — no consumer found anywhere, display or otherwise. `sequence_id`/
`timestamp_us` reuse `Event`'s own top-level fields rather than duplicating.

**Correction, 2026-10-11 (operator: "the regime manager in Rust can add to `TrainingEvent` the
Student-t HMM data needed to train the Transformer, and add to `Event` the data needed for the
Transformer to make inferences")**: the paragraph above originally also listed `transition_risk`,
`dof`, and `expected_duration` as "zero display consumers, stays internal" — that was true for the
*display* question (§5.4 consumer 3) but is **wrong as a blanket statement**, found by checking a
consumer this design hadn't checked yet: the Transformer's own live feature pipeline. Grounded
directly in `lbrnet/lbrnet/generated/MTS/Schema/schema_contract.py`'s `FEATURE_SPECS` tuple and
`lbrnet/lbrnet/features/feature_buffer.py`'s runtime behavior (not inferred):

```python
FeatureSpec("hmm_state", "metadata", "root", "HmmState", "HMMStateEnum")       # read off Event today
FeatureSpec("market_climate", "metadata", "root", "MarketClimate", "MarketClimate")  # read off Event today
FeatureSpec("regime_entropy", "continuous", "regime", "entropy", None)
FeatureSpec("regime_dof", "continuous", "hmm_state_derived", "", None)
FeatureSpec("regime_transition_risk", "continuous", "regime", "transition_risk", None)
FeatureSpec("regime_expected_duration", "continuous", "regime", "expected_duration", None)
```

`feature_buffer.py`'s own code comment for `source == 'regime'` states the field value is
"**caller-supplied via `push_event()`, not read from the FlatBuffer at all**" — i.e., `entropy`/
`transition_risk`/`expected_duration` are *not* wire fields today, they're passed in directly from
Python's own live `regime_engine.infer()` call. This is the exact code-level proof of §5.2's
"Transformer runs its own `RegimeEngine`" finding, not a new problem — but it means the Transformer
has a **confirmed, real need** for `transition_risk`/`expected_duration`/`dof` that this design must
now carry on the wire, since the Rust regime engine replacing `regime_engine.infer()` (§5.1a) is not
itself reachable from Python anymore.

**Open design decision, not resolved here**: do these three fields widen `RegimeStatus` itself
(one shared struct for both the GUI and the Transformer — the GUI simply ignores the fields it
doesn't display), or does the Transformer get its own, separate struct on `Event`/`TrainingEvent`?
Leaning toward widening `RegimeStatus` (avoids maintaining two near-identical structs and two
separate `.fbs`/codegen surfaces for what is, in both cases, "the Rust regime engine's current
output"), but this is a real choice, not decided unilaterally here.

**A fragility this correction surfaces, worth fixing rather than carrying forward**: `regime_dof`'s
current mechanism (`source: "hmm_state_derived"`) is a *separate* per-state lookup array
(`load_hmm_model(config.HMM_MODEL_PATH)`, then `model.dof_` indexed by the raw `hmm_state` the
Transformer separately read off the event) — two independently-loaded things that must agree on
state ordering, the exact bug class (`FeatureScaler` misalignment, `HMMStateEnum` label-switching)
this spec has already flagged twice elsewhere. The Rust regime engine already knows its own current
state's dof internally (§5.1a: `student_t_hmm.py`'s own `dof_` array, ECME-estimated) — it should
emit `dof` **directly as a per-event field**, the same way `transition_risk`/`expected_duration`
would be, eliminating the Transformer's separate model-artifact load entirely for this one feature.

**Raw `hmm_state` is confirmed read directly by the Transformer feature pipeline today** (both the
`hmm_state` `FeatureSpec` above and `lbrnet/lbrnet/features/hmm_context_features.py`'s
`state_id = event.hmm_state`, used for `ignition_flag` detection) — this is not a new concern, but
it is new *evidence* that the still-open "`HmmStep`'s state identity must be rule/label-based, not
raw positional `state_id`" question (§5.2) has a confirmed Transformer-side consumer at stake, not
only the already-named C++ call sites — raising, not lowering, the priority of resolving it before
`RegimeStatus`/the Transformer's wire contract is finalized.

**Mechanism — corrected 2026-10-11 (operator: "we need code generated for Rust... by
`regenerate_schema.sh`... to process that new HMM info in an optimal way")**: the paragraph
originally here said Rust should return a hand-rolled plain struct and "never touches `rust/schema`'s
generated types." That was wrong — checked directly against `rust/schema/src/generated/
mts_schema_generated.rs`'s real codegen output for existing `.fbs` structs (`ObservationData`,
`AsymmetryContext`, `IndicatorState`): each compiles to
```rust
#[repr(transparent)]
#[derive(Clone, Copy, PartialEq)]
pub struct ObservationData(pub [u8; 72]);
```
with a generated `::new(field1, field2, ...)` constructor and named getters. This is POD,
`#[repr(transparent)]` over a fixed byte array — **already in exact wire-format bytes**, zero
serialization step, and FFI-safe to pass by value. The Rust regime engine should construct
`RegimeStatus` (once added to the `.fbs`) using this real generated type's own constructor
(`schema::generated::RegimeStatus::new(...)`), not a hand-rolled parallel Rust struct that merely
"matches" the schema by convention — this is the single-source-of-truth guarantee the project has
been bitten by its absence twice already (`FeatureScaler`'s misaligned arrays, `HMMStateEnum`'s
label-switching), now extended to the Rust side specifically. This gives `rust/schema` its **first
real consumer** — confirmed zero real consumers as of this session's earlier check (`ffi`/
`observation_vector`/`execution` all have zero usages); this changes once `RegimeStatus` lands.
The division of labor is otherwise unchanged from the original design: `Event`/`TrainingEvent` are
`table`s (confirmed by direct schema read, not structs), which need the full FlatBuffers builder —
the **existing** C++ call sites keep owning that (`src/EventDataCollectorStudy.cpp`'s `eventT`/
`TrainingEventT` construction, `src/messaging/EventSerializer.cpp`'s live `Event` construction,
§5.1b). Only the *nested* `RegimeStatus` struct value is built from the Rust-generated type and
passed across FFI as a raw value; C++ does `event_builder.add_regime_status(&value)` with it. (The
engine referenced here is `regime_engine.rs`, §5.1a/§5.2's module layout — not to be confused with the
existing, separate C++ `RegimeManager.h`/`.cpp` facade, §5.2's "companion C++-side groundwork,"
which consumes the HMM's *already rule-based* output downstream, not the raw engine itself.)

**A real tension this surfaces, flagged not resolved — struct vs. table for `RegimeStatus`**:
FlatBuffers `struct`s are fixed-layout and not extensible (no adding/reordering/removing a field
without a breaking change); `table`s support additive optional fields with defaults but need the
full builder (no simple zero-copy value-passing — embedding one table inside another built in a
different language/builder context requires FlatBuffers' heavier "nested flatbuffer" indirection,
not a direct struct-style embed). This matters concretely, not just in theory: `RegimeStatus`'s own
field list has already grown twice across two consecutive design turns in this very document
(decision #16's display-only fields → decision #20's Transformer-needed additions) — live evidence
the output shape is still being discovered, which argues for a `table`. Recommendation, not a
unilateral decision: **keep it a `struct`** (matches every other composite `Event` field —
`ObservationData`/`AsymmetryContext`/`IndicatorState` — and keeps the simple FFI mechanism above),
but explicitly defer finalizing its field list until Stage G4 (plan §4, live engine assembly) when
a *complete* consumer sweep (GUI display, Transformer training, Transformer live inference) can be
done in one pass instead of incrementally discovering fields turn by turn, as has happened so far.
If a future consumer sweep still finds the field list churning after that point, revisit `table`
then rather than accepting indefinite struct churn.

**`TrainingEvent` may legitimately carry a richer field set than `Event`, not necessarily the exact
same fields — not decided here, flagged for the implementation step**: training/offline analysis
tolerates diagnostic fields the live, low-latency wire format doesn't need to carry every tick —
e.g. `mahalanobis_distance`/`tail_weight`/`variance_ratio`/`black_swan_alert`/`duration_bars`/
`duration_ratio`/`fim_distance_min` (today's `HmmRiskState`'s full model-risk diagnostic set,
§5.1b) could be worth keeping in `.alpha` for ablation studies even though no confirmed live
Transformer-inference consumer needs them per the `FEATURE_SPECS` grep above. This mirrors how
`.alpha` already carries far more context than live `Event` for other fields (labels, trade
context) — not a reason to bloat `Event`, but a real reason `TrainingEvent`'s field list shouldn't
be assumed identical to `RegimeStatus`'s just because both come from the same Rust engine call.

**Implementation-time hygiene note, not a design blocker**: `lbrnet`'s `FEATURE_SPECS` already has
its own SHA256-hashed versioning (`FEATURE_CONTRACT_ID`, `schema_contract.py`) specifically to
detect train/live feature-contract drift — switching `regime_entropy`/`regime_transition_risk`/
`regime_expected_duration`/`regime_dof` from `source: "regime"`/`"hmm_state_derived"` (caller-
supplied / side-loaded) to a wire-field source (read off `Event`) changes this hash and must be
tracked through `lbrnet`'s own contract-versioning mechanism when implemented — not a reason to
delay the design, just a real downstream step not to forget.

**Related finding, same trace, not yet acted on**: `MarketClimate` is a hand-maintained
`enum class : int8_t` in `include/Indicator.h` (C++) and separately hand-maintained in Python
(`lbrnet.models.regime_registry.MarketClimate`, relocated 2026-09-22 from `lbrnet.core.rc_enums`,
which keeps a backward-compat re-export shim) — **not** a FlatBuffers schema enum, unlike almost
everything else this wire-format work touches. Values currently agree (`GAUSSIAN_STABLE = 0` on both
sides, spot-checked) but this is exactly the hand-duplicated-across-languages pattern this project has
already been bitten by twice (`FeatureScaler`'s misaligned arrays, `HMMStateEnum`'s label-switching
bug). Worth migrating to a schema-generated enum in the same change that adds `RegimeStatus.climate`
to the wire, rather than baking a third hand-maintained copy into a brand-new field — **not yet
decided, flagged for the implementation step, not blocking this design**.

**Open, not yet verified** (unchanged from the prior note): whether anything besides
`HandleBinaryResponse`/MTS's HMM router needs the `RiskStateUpdate` message shape for some other
purpose — now answered for the GUI-display case (no, `RegimeStatus` replaces it), but a broader
sweep of `lbrnet` consumers of `RiskStateUpdate` has not been done.

**Sequencing decision, 2026-10-11**: the actual `.fbs` edit (adding `RegimeStatus`, wiring
`Event.regime_status`) is deliberately **not** done in this change — there is nothing yet to
populate it (`student_t_hmm` hasn't landed, Stage A/P9 still blocks it, §4) and editing the schema
ahead of the real Rust HMM's finalized output shape risks a second schema churn once that lands.
This section is the complete, ready-to-implement design; the implementation step (the `.fbs` edit,
`regenerate_schema.sh` across MindfulTrader/`lbrnet`/MTS, the `MarketClimate` schema-enum migration
decision, and the actual MTS-side deletions) happens alongside `student_t_hmm`'s own landing, not
before it.

### 5.4a Transformer training-data pipeline: a two-stage Rust architecture (scoped 2026-10-11, decisions #22-#23)

Operator direction, across three turns: (1) have Rust process both the live ZMQ `Event` stream and
the `.alpha`/`.lbr` training file for `lbrnet` (decision #22); (2) move the pipeline that
*generates* the Transformer's training dataset from Python to a **separate, offline, native Rust
tool** — not something run incrementally inside the live DLL, since windowing inherently needs
look-ahead across a full session (decision #23); (3) have Rust also own *loading* that finished
dataset back in on the Python side, not just generating it. The result is a clean two-stage
architecture, not one crate trying to do both jobs:

- **Stage 1 — offline, native, no Python at all**: a new Rust tool (shape: `tools/`-style
  standalone binary, same precedent as `tools/market_data_replay/`) reads already-collected
  `.alpha`/`.lbr` data and ports `lbrnet/data/tristream_dataset.py`'s real windowing/labeling logic
  — not just a format conversion — writing a finished, compact training-cache artifact.
- **Stage 2 — `rust/py` (PyO3), narrower than originally scoped**: loaded from Python at both live-
  inference time and training time. Its training-side job is no longer "parse raw FlatBuffers and
  window them" (that moved to Stage 1) — it's "load Stage 1's finished artifact fast and hand it to
  `tf.data`," plus its already-scoped live per-event extraction job, unchanged.

**Scope note**: broader than §5's HMM focus — this is the Transformer's entire training-data
pipeline, not an HMM-specific concern; placed here (not a new top-level section) because it grows
directly out of §5.4's Transformer-consumption thread and a new top-level section would force a
disruptive renumbering of §6-§12 and every cross-reference to them across this spec and its
companion plan.

**Framework correction, same turn**: the Transformer is **TensorFlow/Keras**, not PyTorch — operator
correction, confirmed by direct import grep of the real files (`lbrnet/models/transformer.py`,
`lbrnet/scripts/train_event_transformer.py`, `tune_event_transformer.py`, `objective.py`,
`layers.py`, `lbrnet/inference/live_agent.py` — zero `torch` imports anywhere in this set). This
matters for both stages: output must be plain `numpy` arrays (via the `pyo3`/`numpy` crate
ecosystem's `PyArray` bridge, Stage 2) or a plain Parquet artifact (Stage 1) — not anything
PyTorch-flavored. `tf.convert_to_tensor` from a numpy array is exactly as cheap as
`torch.from_numpy`, so this doesn't change the fundamental design, it just rules out a
PyTorch-specific output type.

**A genuine, self-identified find while tracing the real pipeline — corrected 2026-10-11 after a
narrower-than-intended original search**: `lbrnet/data/fast_ingestor.py` and `lbrnet/data/
flatbuffer_stream.py` (the "10-50× speedup," "zero-copy," PyTorch-`IterableDataset` files cited as
evidence Python was straining against FlatBuffers overhead) were originally called "confirmed dead
code, zero real callers" — that grep was scoped only to `lbrnet/` (the Python package directory)
and missed the repo's own sibling top-level directories (`backtest/`, `examples/`, `tests/`). A
repo-root-scoped re-check found the two files have **different, genuinely distinct liveness**:
- **`flatbuffer_stream.py` is live, not dead** — wrapped by `backtest/alpha_event_stream.py`
  ("Wraps `FlatBufferEventStream`..."), which feeds `backtest/offline_backtest_agent.py`, which is
  imported by `backtest/backtest_runner.py` — itself referenced by 25+ files across `backtest/`/
  `tests/`/`lbrnet/scripts/`, a core, heavily-used piece of the real offline-backtest-replay
  infrastructure. **Not a deletion candidate.**
- **`fast_ingestor.py` is still a reasonable, if weaker, deletion candidate** — its only reference
  anywhere in the repo is `examples/fast_ingestor_examples.py`, a demo file, not production code.
  Flagged as a dead-code-deletion candidate per this repo's standing default (verify-then-delete),
  but not "zero references" as originally stated.

This is a real lesson for this initiative's own research discipline, not just a correction: a
"confirmed dead code" claim needs a repo-root-scoped search, not a search scoped to the most
obviously-relevant subdirectory — the same standing rule ("search the full repo for usages before
removing code") applies to *claims about* dead code, not only to actual deletions.

**The real pipeline, traced directly**:
- **Training**: `.alpha`/`.lbr` (FlatBuffers, size-prefixed `TrainingEvent` records) →
  `lbrnet/data/lbr_to_polars.py`'s `load_lbr_to_dataframe()` (FlatBuffers → Polars DataFrame, via
  Python loops and per-field dict unpacking) → (a materialization/caching step elsewhere) →
  `lbrnet/data/tristream_dataset.py`'s windowing/labeling + `load_tristream_from_parquet()`
  (Polars/Parquet → TF tensors) → `lbrnet/scripts/train_event_transformer.py` (TF/Keras training
  loop). **Stage 1 above replaces everything up to and including the windowing step.**
- **Live inference**: ZMQ `Event` → `lbrnet/features/feature_buffer.py`'s `push_event()` (the
  `FEATURE_SPECS`-driven extraction already examined in §5.4, including the broken
  `regime`/`hmm_state_derived` side-channel) → `lbrnet/inference/live_agent.py` (TF/Keras
  `predict()`, ZMQ reply to C++). **Unchanged — still `rust/py`'s live-extraction job.**

**`lbr_to_polars.py` already self-identifies as exactly what Stage 1 should replace** — its own
docstring, verbatim: *"DEPRECATED: This module is considered legacy/amateur code. It relies on slow
Python loops and dictionary unpacking. Future institutional pipelines should use C++ extensions or
Arrow readers."* This is the codebase itself flagging the fix the operator is now proposing — Rust
is a strict improvement on "C++ extensions" (memory safety, and it's the direction this entire
initiative is already moving). Confirmed genuinely live, not dead: real callers include
`lbrnet/scripts/collect_data.py`, `scripts/preflight_check.py`.

**Institutional-grade or amateur — the operator's own question, answered**: the *intent* is
unambiguously institutional-grade — it's exactly what `lbr_to_polars.py`'s own docstring already
calls for. Whether the *execution* lands institutional-grade is a discipline question, not a
language question: it requires (a) a golden-harness parity test against `tristream_dataset.py`'s
**current real output** — the exact `(indices, values, hints, position, labels)` tensors — before
any Python is deleted, the same "port → parity test → shadow → cutover" discipline used everywhere
else in this initiative; (b) preserving the windowing/labeling *semantics* exactly, including the
`WINDOW_BUILDER_VERSION` cache-versioning convention (any semantic change must be a separate,
explicitly-decided change, not an accidental side effect of the rewrite); (c) keeping the enum
lookup-table / `FeatureRegistry` logic as a single source of truth, not a second hand-maintained
copy. Done that way: institutional-grade. Rushed without the parity step: exactly the amateur
shortcut the existing docstring already warns against.

**A real wrinkle found while confirming "loading," 2026-10-11**: `tristream_dataset.py`'s load path
(`load_tristream_from_parquet`) does **not** read fully-windowed tensors off disk — its own code
comment explains why: *"v5: compact per-event cache format (indices/values/hints persisted
pre-windowing + `_window_sel`, reconstructed via `sliding_window_view` at load) — v4 caches store
`X_indices`/`X_values`/`X_hints` directly, ~seq_len× larger on disk."* The cache deliberately stores
data pre-windowing (one row per event) plus a `_window_sel` selector array, and reconstructs the
actual `(N, seq_len, 19)` sliding-window tensors **at load time** via
`numpy.lib.stride_tricks.sliding_window_view(...)[sel]` — a zero-copy view trick, repeated for
every stream (indices/values/hints, timestamps, and — confirming yet again the Transformer's real
dependency on HMM-derived fields, same thread as decision #20 — `hmm_state_int` and
`regime_prob_drift`/`flush`/`coil`/`sweep`). **Consequence**: Stage 2's "load" job is not a trivial
Parquet read — it must replicate this compact-format/sliding-window reconstruction trick, which
Rust does natively and cheaply (its own slice/stride views). **Nuance, not a reason to abandon
this**: Polars itself is Rust under the hood, so plain Python Parquet-reading isn't the "slow
FlatBuffers dict-unpacking" problem `lbr_to_polars.py` complains about — the real value of doing
this specific load step in `rust/py` is correctly and fast replicating the compact-format
reconstruction, and keeping Stage 1 (writer) and Stage 2 (reader) as one Rust-owned,
single-source-of-truth pair so a `WINDOW_BUILDER_VERSION`-style format change can't silently drift
between the tool that writes the cache and the code that reads it.

**Proposed scope, split across both stages**, both built on `rust/schema`'s generated FlatBuffers
readers where FlatBuffers parsing is still involved (the same single-source-of-truth principle
established in decision #21, not a hand-rolled parallel parser):
1. **Stage 1 (new offline tool)**: read `.alpha`/`.lbr`, port `tristream_dataset.py`'s windowing/
   labeling/enum-LUT logic, write the finished compact-cache artifact — replacing `lbr_to_polars.py`
   and the generation half of `tristream_dataset.py` entirely (and the orphaned `fast_ingestor.py`,
   not `flatbuffer_stream.py` — that one is live via `backtest/alpha_event_stream.py`, correction
   above).
2. **Stage 2a (`rust/py`, training side)**: load Stage 1's compact-cache artifact, reconstruct the
   windowed tensors (the `sliding_window_view`/`_window_sel` trick above), hand `numpy` arrays to
   `tf.data` — replacing the loading half of `tristream_dataset.py`.
3. **Stage 2b (`rust/py`, live side, unchanged from the original scoping)**: parse one incoming
   `Event` FlatBuffer and return a `numpy` feature vector, replacing the FlatBuffers-parsing portion
   of `feature_buffer.py`'s `push_event()` — the windowing/sequence-buffer logic around it can stay
   Python, only the raw bytes → numpy extraction moves to Rust.

**This directly and permanently fixes the `regime`/`hmm_state_derived` side-channel found in §5.4**:
once `RegimeStatus`/`regime_transition_risk`/`regime_expected_duration`/`regime_dof` are real wire
fields (decision #20/#21), both Stage 1 (writing them into the training cache) and Stage 2b (live)
read them directly off the `Event`/`TrainingEvent` FlatBuffer using the same generated readers —
eliminating `feature_buffer.py`'s `push_event(event, regime_state=...)` side argument and the
separate `load_hmm_model(...).dof_` lookup array entirely, not just working around them.

**`FEATURE_SPECS`/`FEATURE_CONTRACT_ID` (`schema_contract.py`, §5.4) stays the canonical feature
contract** — both stages' extraction code should be driven by (or kept in lockstep parity with)
this same spec list, not a second hand-maintained copy of "which fields, in which order" —
continuing the single-source-of-truth discipline this spec has applied everywhere else.

**Explicitly out of scope**: the Transformer *model* itself (TF/Keras) is not proposed to move to
Rust — no Rust ML framework maturity argument is being made here, and this isn't what was asked.
This is a data-plumbing replacement only: FlatBuffers bytes → a finished training artifact →
numpy/TF tensors, faster and memory-safe, feeding the same TF/Keras training loop and live
`predict()` call unchanged.

**Open, not resolved here**:
- **Stage 1's exact artifact format**: the compact pre-windowing cache format `tristream_dataset.py`
  already uses (Parquet-based, per the wrinkle above) is the natural target to replicate rather
  than inventing a new one — not yet decided whether to replicate it byte-for-byte or take the
  opportunity to redesign it now that it's Rust-native.
- **Build/packaging path is genuinely new**, not covered by anything else in this spec:
  `build_rust.sh` builds the Windows-cross-compiled DLL target; `rust/py` needs a *different*
  target (a Linux `cdylib`/`.so` importable by the `mts` mamba env's Python, built via
  `maturin`/`pyo3-build-config` conventions, not `clang-cl`/CMake) — needs its own build script
  and its own entry in whatever CI eventually exists (plan §6.4, "not yet built"). Stage 1's tool
  needs its own `tools/`-style bare build invocation, separate again.
- **GIL handling**: PyO3's GIL-release patterns (`py.allow_threads(...)`) should be used during
  Stage 2's parsing/reconstruction work specifically because the whole point is throughput — not
  yet designed.
- Whether Stage 1's windowing/reconstruction logic and Stage 2's live-extraction logic share one
  crate or split further (e.g. a pure-Rust extraction/windowing library crate that both the Stage 1
  binary and the `rust/py` PyO3 wrapper depend on, mirroring `student_t_hmm`'s own core-vs-`ffi`
  split) — leaning toward the split (consistent with this initiative's established "pure core, thin
  edges" pattern everywhere else, and Stage 1/Stage 2a already clearly share the same
  windowing/reconstruction code), not decided.

### 5.5 Open questions

1. "Older-sequence" in the coherence rule (§2 item 5) — confirm it means older than the regime-epoch
   start, not older than every newer sequence (generic ageing is already handled by
   `SemanticFreshnessDiscount`).
2. `cognitive_load`: deterministic replacement, or drop (wall-clock-derived today).
3. Entropy normalization function and the unconditional-state set — not read in full yet; the plan's
   Stage A golden will pin them.
4. `HMM_MODEL_INPUT_DIM` value and `HMM_BURN_IN` — read from code in the plan's Stage A.
5. ~~Whether `meta_hmm_control_plane.py`/`smart_tuner.py`/HPO call `train_student_t_hmm.py` as a
   subprocess or import the model~~ — **RESOLVED 2026-10-11 (§5.1a)**: moot for this specific
   question — `meta_hmm_control_plane.py` itself is confirmed orphaned (deleted, along with its
   3 sibling files, 2026-10-11). **Correction, same day, found while executing cleanup**: this was
   originally stated as "the whole 11-file Meta-HMM scaffolding is orphaned" — that overclaimed;
   5 of the ~9 files (the recommendation/ledger sub-cluster) are confirmed live, feeding
   `risk_gate_orchestrator.py` via `run_optimization_pipeline.py`. Full corrected breakdown: §5.1a.
6. `train_student_t_hmm.py`'s `--expected-schema-version` default is **230** while the current schema is
   **240** (noted, not investigated) — the existing training gate may already be stale. Partially
   informed by §5.1a: direct-Parquet-mode training already *bypasses* this `.context`-header check
   today, so a Rust trainer ingesting Parquet directly may not need to carry the check forward at
   all — still needs an explicit decision, not yet made.
7. The four-name probability sidecar (`p_coiled`, `p_gaussian_stable`, `p_gaussian_fragile`, `p_pareto`)
   bakes in K=4; keep for compatibility initially, generalize later.
8. CPU vs. GPU training time at 22M rows — now sharpened by §5.1a into a concrete decision: whether
   `student_t_hmm_gpu.py`'s PyTorch-batched restart-screening path gets a Rust equivalent at all.
   The runbook's own measurement (GPU screening is 20-33x *slower* than CPU/joblib) plus Rust's
   expected non-interpreted-language speedup makes dropping it the likely answer, but this should
   be confirmed with a real measurement once the Rust CPU trainer exists, not assumed.
9. ~~Whether the climate/threshold constants hard-coded in `regime_engine.py` move into the manifest
   (recommended) or stay as crate constants~~ — **RESOLVED 2026-10-11 (decision #25, §5.1c)**: false
   dichotomy — both, split by tier (manifest-backed + mandatory auto-calibration for the 4 Tier-1 +
   `TAIL_RISK_THRESHOLD`; plain struct-field defaults, no manifest slot, for the 6 Tier-3
   constants). New open sub-question from this resolution: whether the Rust auto-calibration step
   needs `tune_risk_gate.py`'s own internal walk-forward meta-tuning folds (`--train-months`/
   `--val-months`/`--holdout-months`) beyond the single calibration-holdout slice, or whether a
   single holdout evaluation suffices for Phase 1 — not yet decided (§5.1c).
10. ~~`dof_learning_rate` (0.02 vs. 0.15) and `bullet_weight` (1.40 vs. 1.30) differ between
    `student_t_hmm.py`'s own class defaults and `train_student_t_hmm.py`'s `HMMTrainingConfig`
    defaults — one canonical value must be chosen and documented before porting; which file's
    default was actually exercised in production training needs confirming~~ — **RESOLVED
    2026-10-11 (decision #26, §5.1a)**: not a canonical-value pick for either — `dof_learning_rate`
    is confirmed dead code (never read by `_update_dof_ecme()`), dropped entirely from the Rust
    port; `bullet_weight` is real but per-training-run BIC-grid-searched, not fixed, and
    `model.rs`'s existing per-instance `feature_weights` field already handles this correctly with
    no change needed — only open item is G5's own grid-search default range, deferred to G5.
11. ~~`calibrate_hmm_gate_thresholds.py`'s stationary block-bootstrap methodology (Politis & White
    2004) relies on Python's `arch` package with no obvious Rust crate equivalent found — needs
    either a from-scratch implementation or a further crate search before `calibration.rs` (§5.2)
    can be more than a stub~~ — **RESOLVED 2026-10-12 (decision #32)**: no Rust crate exists for
    either Politis-White block-length selection or Politis-Romano stationary bootstrap (confirmed,
    not just assumed); both ported from scratch, faithfully, directly against the real installed
    `arch` package's own source. `calibration.rs` SHIPPED.
12. **New (§5.1a)**: `hmm_features.py`/`hmm_context_features.py` — scoping call pending the
    separate question of whether a Rust-side Transformer-training initiative is ever started;
    `hmm_context_features.py` is also pinned to the legacy 4-state `HMMStateEnum`, which the
    canonical-taxonomy ruling (§5.1a) says is being retired.
13. **New (§5.1a)**: confirm `regime_manager.py` (Python, the legacy-4-state offline/batch labeling
    counterpart, distinct from C++'s `RegimeManager.h`/`.cpp`) has zero remaining production
    callers before deleting it in favor of `regime_registry.py`'s 5-state cascade.
14. ~~artifact serialization format — reconsider `bincode` (simpler, Atratus's choice) vs. the
    currently-proposed manifest + raw-floats hybrid, now that Python may never need to read the
    trained model directly; weigh against whichever downstream consumers
    (`materialize_hmm_features.py`/`regime_calibration.py` or their Rust replacements) still need
    to read it~~ — **RESOLVED 2026-10-11 (G0(c), `artifact.rs` shipped)**: `bincode` rejected —
    confirmed real, non-retiring Python consumers exist (`train_event_transformer.py`/
    `tune_event_transformer.py`, which stay Python/TensorFlow per decision #22 and directly
    `joblib.load()` the HMM artifact for the Kazemi et al. 2019 `sigma_eff` feature), not a stale
    justification. **Simplified further than originally proposed**: given `PRODUCTION_D=18`/
    `PRODUCTION_K=4`'s ~190-float total parameter count, a separate binary weights file is
    complexity this tiny a payload doesn't earn — one plain JSON manifest (dims, weights,
    `registry.rs` classification, a `model_sha256` content hash over the core params alone) is
    simpler and equally Python-readable with plain `json.load`, no `numpy.frombuffer` needed.
    Calibration-threshold/provenance fields (decision #25) deliberately deferred to whenever
    `calibration.rs` (G0(e)) lands, not committed to a shape now. Full account: `artifact.rs`'s own
    module doc comment.
15. **New (§5.4, decision #20)**: does `RegimeStatus` widen to carry `transition_risk`/
    `expected_duration`/`dof` (serving both the GUI and the Transformer from one struct), or does
    the Transformer get its own separate struct on `Event`/`TrainingEvent`? Leaning toward
    widening, not decided.
16. **New (§5.4, decision #20)**: whether `TrainingEvent`'s regime-field set should be a strict
    superset of `Event`'s (carrying `HmmRiskState`'s full diagnostic set — `mahalanobis_distance`/
    `tail_weight`/`variance_ratio`/`black_swan_alert`/`duration_bars`/`duration_ratio`/
    `fim_distance_min` — for offline ablation studies) or identical to it — not decided. **Cross-
    referenced 2026-10-11 (§5.1d, decision #27)**: whichever design wins must also close out the
    `materialize_hmm_features.py`/`TrainingEvent` wire schema's confirmed-live
    `regimeProbDrift/Flush/Coil/Sweep` K=4-hardcoding bug in the same pass, not as a separate later
    migration — the Rust `TrainingEvent` generation work will already be touching this exact
    surface.
17. **New (§5.4, decision #21)**: `RegimeStatus` as a FlatBuffers `struct` (fixed-layout, simple
    zero-copy Rust FFI via the generated `#[repr(transparent)]` newtype, matches every other
    composite `Event` field) vs. a `table` (additive/extensible, matching the observed field-list
    churn across decisions #16/#20, at the cost of the full builder + "nested flatbuffer"
    indirection instead of a direct struct embed). Leaning `struct`, deliberately deferred to Stage
    G4's full consumer sweep rather than decided now — not decided.
18. **New (§5.4a, decisions #22-#23)**: build/packaging path — `maturin`/`pyo3-build-config`
    conventions for `rust/py`'s Linux `cdylib` importable by the `mts` mamba env's Python, distinct
    from `build_rust.sh`'s Windows-cross-compile target; plus a separate bare-build invocation for
    Stage 1's standalone dataset-generation tool (`tools/`-style). Neither has its own build script
    or CI entry yet.
19. **New (§5.4a, decision #22)**: GIL-release strategy (`py.allow_threads(...)`) for `rust/py`'s
    parsing/extraction hot path — not yet designed.
20. **New (§5.4a, decisions #22-#23)**: whether Stage 1's windowing/reconstruction logic and
    Stage 2's live-extraction logic share one crate or split into a pure-Rust extraction/windowing
    library both the Stage 1 binary and the `rust/py` PyO3 wrapper depend on (mirroring
    `student_t_hmm`'s own core-vs-`ffi` split) — leaning toward the split, sharpened by decision
    #23's finding that Stage 1 and Stage 2a already clearly share the same windowing/reconstruction
    code, not decided.
21. **New (§5.4a, decision #23)**: Stage 1's exact artifact format — replicate
    `tristream_dataset.py`'s existing compact pre-windowing cache format byte-for-byte, or redesign
    it now that it's Rust-native — not decided.
22. **New (§5.1a, found during cleanup 2026-10-11)**: the Meta-HMM "recommendation/ledger"
    sub-cluster (`generate_meta_hmm_recommendation.py`, `validate_meta_hmm_recommendation.py`,
    `record_meta_hmm_recommendation_outcome.py`, `contracts/meta_hmm_recommendation_contract.py`,
    `contracts/meta_hmm_recommendation_ledger_contract.py`) is confirmed live — feeds
    `risk_gate_orchestrator.py` via `run_optimization_pipeline.py` — but its port-vs-keep-as-Python
    call was never made; it's risk-gate-adjacent, not HMM-training-adjacent, so it may belong to a
    different initiative entirely (the risk-gating/execution-architecture thread) rather than
    `student_t_hmm`'s own scope — not decided.
23. **Carried forward, still open**: whether anything beyond MTS and `backtest_server.py` needs the
    `RiskStateUpdate` message shape — flagged in §5.4's own text as "a broader sweep... has not
    been done," never promoted to this list or resolved.
24. ~~whether `MarketClimateIndicator`'s independently-evolved native kurtosis/entropy/Hurst
    "physics veto" hybrid classifier becomes an explicit additional safety layer inside `regime_engine.rs`
    (reconciled, no more positional `HMMStateEnum`-cast coupling), or is retired as redundant with
    what the HMM's own `profile_label`-based classification already encodes — needs an explicit
    operator call, not an engineering default either way~~ — **RESOLVED 2026-10-11 (decision #29)**:
    retire — confirmed redundant/unprincipled signal composition AND a confirmed live write-race
    bug (the native per-tick write overwrites the trained model's own classification before
    downstream consumers can act on it). New follow-up this resolution creates, not yet scoped:
    deleting `TripleScreen3.cpp`'s native write call and `MarketClimateIndicator::UpdateContext()`'s
    own cascade is a Stage G7 cutover task, not yet on the G7 bullet's own file list.
25. **New (§5.1d, decision #27)**: three more `lbrnet`-side consumers (`tune_risk_gate.py`,
    `build_directional_alpha.py::_attach_regime_to_event()`, and
    `materialize_hmm_features.py::serialize_enriched_training_event()`) still do positional-
    `HMMStateEnum`/hardcoded-4-slot reads that Python's own `regime_engine.py` rewrite already
    flags as unfixed — out of scope to fix in `lbrnet` directly from this session (standing
    "leave `lbrnet`'s own pile as-is" instruction), but the Rust `TrainingEvent`/wire-schema work
    (open question #16) should not reintroduce the same hardcoded-K-slots pattern when it lands.

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
GIL released. Payloads stay FlatBuffers; **business semantics (message content, trading behavior, port
numbers for the six ports below) stay unchanged — wire *mechanics* do not** (ruled, operator,
2026-10-10, below).

**RULED (operator, 2026-10-10): Rust best-practice freedom for transport mechanics, not a license to
drift business semantics.** Porting ZMQ/FlatBuffers from C++/Python to Rust is free to use idiomatic
Rust patterns for socket management, FlatBuffer handling, and data layout (DOD) — it is not required
to replicate C++/Python's exact mechanics line-for-line. Concretely in scope for a better Rust design:
zero-copy FlatBuffer reads instead of `TransportStream::Emit`'s copy-into-queue-then-worker-thread
pattern; a proper per-message-type discriminator (today's port 5555 has **none** — `EventSerializer`
publishes bare `Event`-rooted buffers directly while `PositionUpdate`/`Diagnostic` frames are
separately `Envelope`-wrapped onto the *same* unframed socket, and `lbrnet`'s real consumer
(`event_consumer.py`) calls `GetRootAsEvent()` unconditionally with no type check — this is a real
defect found 2026-10-10 investigating the sibling execution-migration spec's D1, not a pattern worth
preserving); the request/reply `submit`/`poll` non-blocking shape already identified above as
necessary is exactly this kind of allowed mechanics change, not an exception to the rule. **What stays
frozen regardless**: port numbers and peer-visible message *content/semantics* for the six verified-
live ports (so each port still flips independently against an unchanged peer, per the migration order
below); no trading-behavior or threshold change as a side effect of a transport-layer port (that
discipline is separately owned by each business-logic component's own spec, e.g. §8 below and the
sibling execution-migration spec's §6 D4). Be inspired by what's good in the existing design and keep
it — `GateEvent`/`IntentEvent`/`RecoveryEvent`'s `ReasonCode`/`IntentState`/`GateDirective` vocabulary
(§8, `ExecutionGate.h`) is already genuinely institutional and should carry forward as-is, not be
reinvented for its own sake.

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
back): (1) `transport`/`ffi` skeleton — gate: DLL builds, imports unchanged, Sierra loads it;
(2) 5555 PUB — gate: stale-heartbeat detection fires at the same threshold; (4) Python side (PyO3
client replaces `transport.py`/`zmq_client.py` internals) — gate: existing Python tests green; (5)
5560/5562/5558, one at a time, `submit`/`poll` shape — gate per port: handshake + round-trip against
the unchanged peer, full `.btst`-matching replay for 5558; (6) 5561 — not migrated, removed by §5; (7)
remove the vcpkg libzmq/libsodium link, delete the old socket code (not behind a flag — not in
production).

**Non-goals** (business semantics, not transport mechanics — see the 2026-10-10 ruling above): no
schema *semantic* change, no port-number change, no trading-behavior change as a side effect
— **for this component's own original scope** (the six verified-live ports above). §8 (execution
layer) deliberately adds two brand-new `transport` ports (an acknowledged execution-intent
channel and a position/fill feed) plus a new schema message, and per §8.7 D1 proposes pulling
forward only `transport`'s skeleton + those two new ports ahead of this component's
"deliberately last" W9 sequencing — the two new ports have no existing live peer to regress,
unlike the six below. §8 is authoritative for its own scope; this section's non-goals are
unchanged for the original six-port migration.

---

## 8. Component spec: execution layer (broker connectivity + computations)

**Folded in here 2026-10-10** from a standalone spec (`docs/superpowers/specs/2026-10-10-ibkr-rust-
execution-migration-spec.md`, opened and deleted the same day — operator directive: this initiative
gets one spec/plan pair, not a new sibling document per sub-topic, same rule this document's own
header already states). Everything below is that document's content, restructured to fit here; no
substance lost, nothing re-decided.

**Not to be confused with** `docs/superpowers/specs/2026-09-18-predator-sniper-execution-architecture.md`
— that document's "Predator"/"Sniper" are stages of the **signal-decision pipeline** (HMM regime
survey → Transformer direction → meta-labeler sizing → stop/target geometry), which stays in
C++/Sierra Chart under this component's scope (§8.2). This component's "execution layer" is the
layer *downstream* of that pipeline: taking a decided trade (direction, size, stop, target) and
actually getting it filled and managed at the broker.

**Scope confirmed 2026-10-09 (operator)**: this initiative is "all Rust," not "HMM plus
observation_vector math." `RiskManager`/`PositionManager`/`ExecutionGate`'s own low-level
computations are explicit, equal-footing candidates for the same methodical port (decision #6, §2).
**Scope deepened 2026-10-10 (operator)**: not just the low-level computations ported in place —
broker connectivity (`ibapi`, direct IBKR) moves out of Sierra Chart's `SCTradingInterface` routing
entirely, into a standalone Rust process, §8.2–§8.4 below.

### 8.1 Why a standalone process, and where the broker design comes from

Opened from a direct comparison to `Atratus`'s Predator/Sniper options-execution design
(`../Atratus/docs/superpowers/specs/2026-10-07-predator-sniper-spec.md`, `../Atratus/CLAUDE.md`
§"Architecture"). **Atratus is a separate project — never a dependency**, same rule as this spec's
own §1.1. Where its design is a genuinely reusable pattern, it is copied here with attribution and
owned independently, not imported.

What transfers:
- **Every broker order is sent by one Rust `ibapi` path, never by C++, never by Python** — Atratus's
  own rule, substituting "the new Rust execution crate" for "Sierra Chart's `SCTradingInterface`".
- **Blocking by default; `tokio` only where racing matters.** Atratus's vendored `ibapi` is used via
  `ibapi::client::blocking::Client` everywhere except the order-watcher, which gets its own `tokio`
  runtime on its own thread with its own TWS client id, racing order status / quote updates /
  condition checks / a time limit in one `tokio::select!` (Atratus spec §9 D3(c)). This pattern —
  not a wholesale async rewrite — is the one being adopted.
- **IBKR's own algos do the order-working — exclusively (ruled, operator, 2026-10-10, §8.7 D3).**
  No Rust-native price-stepping fallback. If IBKR rejects an algo for the instrument/order type,
  that is resolved by choosing a different IBKR algo, never by hand-rolling our own stepping logic
  — this diverges from Atratus's own D3(c), which keeps a stepping fallback for exactly this case.
  The order-watcher's job is narrower than Atratus's: it does not work the order itself, it decides
  **when to update or cancel** the IBKR-algo-managed order as conditions change (stop/target moved,
  gating conditions no longer hold, time limit reached) — the algo always does the actual price
  discovery/working.
- **One gated order path.** Every order — not just some — passes through a single function
  (Atratus: `ladder_api::gated_order`) that applies the safety gate (kill switch, caps, approval)
  between "decided to buy/sell X" and "an order is actually placed". Correctness of this path
  outranks the decision logic that feeds it.
- **State isolation and an explicit arm for live.** Atratus's `state/<env>/LIVE_ORDERS_ARMED` file
  (written only by a typed-confirmation script) and a global `state/KILL_SWITCH` are cheap, proven
  safety mechanics worth copying outright (§8.7 D5).

What does **not** transfer: Atratus trades MES options with a human-clicked "Hunt" entry; this
system trades MES futures (per `docs/ADR/sierra_chart_data_feed_setup.md`) with an **automated**
entry decided by the HMM/Transformer/meta-labeler pipeline, no per-trade human click. The entry
timing/sizing/stop-target decision stays exactly as the existing signal-decision pipeline computes
it — only the *execution mechanics downstream of that decision* are being replatformed.

**Rust best-practice freedom is scoped to transport/implementation mechanics, not business logic**
— same 2026-10-10 ruling as §7's own, which this component's wire needs (§8.5) ride on directly.
Trading-behavior parity (stop/trap thresholds, gate decisions, numeric outcomes) is a separate,
already-settled discipline — §8.7 D4 does not get loosened by that ruling.

### 8.2 Scope — confirmed with the operator, 2026-10-10

| # | Question | Decision |
|---|---|---|
| 1 | Does this retire Sierra Chart/ACSIL entirely? | **No.** Sierra Chart/ACSIL keeps owning market data ingestion and signal generation — Triple Screens, `IndicatorManager`, `ContextManager`, HMM/Transformer inference, the existing ZMQ event stream (port 5555). Only order placement/management moves. |
| 2 | How much of the execution layer moves? | **All of it** — the full order lifecycle and state machine: position state, fills, bracket/stop management (static entry-anchored stop/target bracket, OCO — **operator's original framing said "Chandelier trailing stop"; corrected 2026-10-11, see §8.3's `stop_manager.rs` entry — `ChandelierStopManager` was already deleted from the codebase by the time this scoping conversation happened**), trap-detection-driven exits, emergency flatten. C++ is reduced to emitting trade decisions (direction, size, stop, target) and live price/context updates; Rust owns everything from there to the broker and back. |

This is decision #6's (§2) concrete target architecture for the *broker-facing* half of the
execution layer specifically, going further than a same-process C-ABI port: a **standalone
process**, because `ibapi`'s TWS connection, the order-watcher's `tokio` runtime, and the broker
state machine have no reason to live inside the Sierra Chart DLL's process or thread model, and
doing so would recreate Sierra's own `SCTradingInterface` coupling problem one layer down.

### 8.3 What moves, named against today's actual code

| Today (C++, in `MindfulTrader.dll`) | Responsibility | Moves to Rust? |
|---|---|---|
| `PositionManager.cpp`/`.h` | Order submission (`sc.BuyOrder`/`SellOrder`, `s_SCNewOrder`), fill tracking, position state, `EmergencyFlattenPosition`, `TripleBarrierClose` | **Yes, in full** |
| `RiskManager.cpp`/`.h` | Daily loss limits, Kelly sizing, `IsTradeAllowed`, `EmergencyHalt`, `CancelAllWorkingOrders`, `FlattenPositionIfOpened`, order-ack/working-order TTL monitoring | **Yes, in full** |
| `include/execution/ExecutionGate.h`/`.cpp` | Pure gate decision (`Allow`/`Deny`/`Ignore` + `ReasonCode`) fusing connectivity, model-freshness, and HMM-regime inputs | **Yes** — becomes the Rust crate's own gate, fed the same inputs over the wire (§8.5) |
| `TripleBarrierEngine.h`/`TripleBarrierExitManager.h` | Static, entry-anchored stop/target bracket (`ComputeBarriers`, immutable once placed — **not** "Chandelier-style trailing stop recompute," corrected 2026-10-11), `FAILED_*` reactive TRAP floor, τ\* Bayesian anticipatory-exit gate | **Yes** — explicitly in scope per the operator's §8.2 answer (originally framed as "Chandelier trailing stop... trap-detection-driven exits," now-stale terminology; the actual scope — bracket placement + trap exits — is unaffected by the correction) |
| `TradeExecutionServer.cpp` (port 5558 REP) | Backtest-mode trade RPC, `TRADE_COMMAND` | **Superseded or repurposed — open, §8.7 D7** |
| Triple Screens, `IndicatorManager`, `ContextManager`, `InferenceManager`/HMM client, Transformer | Signal generation: regime, direction, size, stop/target geometry | **No — stays in C++**, per §8.2 |
| `EventSerializer`/`TransportStream` (ZMQ PUB 5555) | Publishes indicator/signal/position state | **No — stays in C++**, becomes Rust's *input* feed (§8.5) |

**Candidates surveyed for the pure-computation slice** (all pure, zero Sierra Chart coupling,
confirmed by direct code read): `Scoring.h`, `LocalRiskContext.h` (the real live risk struct,
§4.3), `TailRiskEngine.h` — same shape as the `observation_vector` candidates already ported,
stateless-function pattern applies directly. `TradeDecisionEngine.h` (560 lines) — already its own
shadow-mode-only layer by design (PAER §9 Phase 1: "Computes shadow metrics on every trade
opportunity WITHOUT changing execution behavior... No execution logic reads these values");
arguably the safest starting point in this whole component, since it already cannot affect real
decisions. `KellyCalculator.h` — stateful (a trade-history circular buffer), needs the
init/step/reset opaque-handle pattern already planned for `student_t_hmm`, not the stateless-function
pattern used so far.

**No special caution tier for this code**: corrected on exactly this point earlier in this
initiative after initially (wrongly) reaching for "real capital is at stake" framing — there is no
live capital, this system is not in production, so the discipline is identical to every other port
in this document: parity-test, shadow-wire, live-confirm, cutover.

### 8.4 Target architecture

A new, standalone Rust binary/service (working name `execution_service`, final name TBD) with its
own process lifecycle, independent of Sierra Chart's. Proposed module layout, modeled on Atratus's
`rust/src/{ibkr_broker,es_options,ib_session,ladder_api,exit_manager}.rs` split (same idea, this
system's own instrument and risk model):

```
rust/execution/
├── ibkr_session.rs      -- one persistent ibapi connection (blocking client default); vendored,
│                           patched ibapi per Atratus's precedent (TWS_READ_TIMEOUT 1s -> 20s fix
│                           is a real, generically-needed fix, not Atratus-specific — re-verify
│                           against this account's own TWS/Gateway behavior before assuming it,
│                           don't just copy the patch unverified; crates.io's current ibapi 5.0.0
│                           may already differ from Atratus's vendored 4.2.0 — check before vendoring)
├── mes_futures.rs        -- MES futures contract resolution, front-month roll, tick size (never
│                           hard-coded 0.25 — Atratus's own D3 fallback-stepping lesson)
├── order_gate.rs         -- the single gated order path (kill switch, daily-loss halt, caps,
│                           ReasonCode-based fail-closed decisions) — Rust port of
│                           ExecutionGate.h's Outcome/ReasonCode enum + ladder_core::safety's
│                           kill-switch/arm-file pattern (§8.7 D5)
├── position_state.rs     -- broker-mirror position state + annotations + reconciliation (the broker,
│                           never this module, is the source of truth -- §8.7 D2 "Results")
├── stop_manager.rs       -- static, entry-anchored bracket placement (port of
│                           TripleBarrierEngine.h's `ComputeBarriers`, §8.7 D3a) — the bracket is
│                           computed ONCE at entry and is immutable thereafter; NOT a continuously
│                           re-trailed stop. **Corrected 2026-10-11**: earlier drafts of this spec
│                           called this "Chandelier-style trailing stop recompute" — stale.
│                           `ChandelierStopManager` (the actual trailing-stop order-management
│                           class) was deleted entirely during a documented cutover
│                           (`docs/ADR/triple_barrier_cutover_phase1_plan.md`), confirmed by direct
│                           grep (zero files named `*chandelier*` remain; `TripleBarrierExitManager.h`'s
│                           own comment states the deletion explicitly). Do not confuse this with
│                           `elderChandelierATR` (`ContextManager.cpp`/`LocalRiskContext.h`), a
│                           *surviving but unrelated* concept — a risk-gate diagnostic (distance from
│                           current price to a theoretical Chuck LeBeau Chandelier level, feeding
│                           `RiskManager`/`Scoring`/`TradeDecisionEngine`'s tail-proximity gates),
│                           computed independently of any actual stop order and never placed on the
│                           wire to a broker.
├── trap_exit.rs          -- native reactive TRAP floor (StructureTest FAILED_* parity with the
│                           labeler) + τ* anticipatory-exit gate (Elkan 2001) — ported behavior,
│                           not redesigned (§8.7 D4)
├── order_watcher.rs      -- the ONLY async module: dedicated tokio runtime + thread + its own
│                           TWS client id, tokio::select! over order status / quote / stop-recalc
│                           trigger / timer. Never works the order itself — an IBKR algo
│                           always does (ruled 2026-10-10, no Rust-native stepping fallback); for
│                           the unfilled entry it decides hold / cancel / finished / escalate
│                           (built 2026-10-13, decision #39; amends go through
│                           BrokerAdapter::amend, see D3a's order- vs. position-management split)
├── signal_feed.rs        -- consumes rust/transport's port-5555 subscription (continuous
│                           price/context ticks only, §8.5) — an `transport` client, not its
│                           own zmq socket; decodes with rust/schema's generated FlatBuffer bindings
├── intent_client.rs      -- consumes a NEW rust/transport port (§8.5/§8.7 D1) carrying discrete
│                           execution-intent messages (direction/size/stop/target) from C++, which
│                           requires a new FlatBuffer schema message (TradeRequest's price/qty/
│                           stop/target are explicitly unset today) — new `transport` inventory
├── position_feed.rs      -- publishes through a NEW rust/transport port (§8.5): position/fill/P&L
│                           state for MTS/GUI, replacing PositionUpdate's old C++-side publisher —
│                           again, new `transport` inventory, not this crate's own PUB socket
├── broker_adapter.rs     -- [built as broker.rs: place / amend / cancel / flatten / poll_events]
│                           trait BrokerAdapter { fn submit(...), fn modify(...), fn cancel(...),
│                           ... } with two impls: IbkrAdapter (real ibapi, modify = IBKR
│                           modifyOrder on the live algo order) and ReplayFillAdapter (backtest,
│                           §8.6) — position_state/stop_manager/trap_exit run identically against
│                           either
└── main.rs
```

Reuses, not rebuilds: `rust/schema` (generated FlatBuffer bindings) **and `rust/transport`
(`transport`, §7) for every socket** — `rust/execution` has no direct `zmq`/`tokio-zmq`
dependency of its own; it depends on `transport` exactly as `ffi`/`py` do (§7's "one
Rust crate owns every ZMQ socket for all three languages" — the standalone execution process is a
fourth consumer of the same crate, just linked natively instead of through the C-ABI or PyO3).
`rust/execution` (crate `execution`, matching the unprefixed-crate-name convention of every other
crate in §3's table — the `mts_` prefix was dropped repo-wide 2026-10-11, §1.2) is a new
`rust/Cargo.toml` workspace member.

### 8.5 The C++ ↔ Rust wire contract

**RULED (operator, 2026-10-10): split channel, not a single reused port.** Investigation of the
actual wire mechanics (`TransportStream.cpp`, `EventSerializer.cpp`) found port 5555 is a plain ZMQ
`PUB` socket, `SNDHWM = 1000`, no replay, no delivery guarantee — exactly why the system already
keeps discrete trade actions on separate acknowledged REQ/REP sockets (5556/5558) instead of 5555.
A trade decision (enter/exit + concrete stop/target) must not silently vanish the way a dropped
indicator tick can; it gets its own channel.

- **Continuous feed (reused): ZMQ SUB on the existing port 5555 PUB stream**, consumed through
  `transport` (§7), not a bespoke socket. `stop_manager.rs`/`trap_exit.rs` need ongoing market
  data every tick, not just at decision time — **corrected 2026-10-11**: not to "recompute
  trailing stops" (stale premise, see §8.3's `stop_manager.rs` correction — the bracket is
  immutable once placed), but to evaluate the immutable bracket's first-hit-wins resolution
  against live price/bar index (stop/target/time/regime-invalidation, `TripleBarrierExitManager::
  Evaluate()`'s own job) and the reactive TRAP floor, respectively. This is a second, independent
  consumer of the live tick stream Sierra Chart already produces (`Event.observation`,
  `.indicators`, `RiskGateContext.elder_chandelier_atr` — a risk-gate diagnostic metric, distinct
  from the deleted `ChandelierStopManager`, see §8.3). Lossy-by-design is acceptable here; Rust
  evaluates its own bracket/trap state from the raw numbers rather than depending on any single
  update arriving.
- **Discrete channel (NEW `transport` port — number TBD, §8.7 D1): acknowledged, for
  execution-intent messages only** (direction, size, stop, target from the signal-decision
  pipeline). Mirrors the existing 5556/5558 REQ/REP pattern — C++ becomes the client (REQ) now
  that Rust, not C++, acts on the decision. **Requires a new FlatBuffer schema message**, not just
  a new Rust consumer of an existing one: `TradeRequest`'s own doc comment says entry
  price/quantity/stop/target are "determined by C++ side only" and deliberately never serialized
  today (`PositionManager` computes and acts on them in-process). Once execution is a separate
  process those concrete numbers must cross the wire — a real schema change, going through
  `schema/regenerate_schema.sh` and touching C++, Python/`lbrnet`, and Rust codegen together.
- **A real wire-format defect, fixed in the Rust port, not preserved** (per §7's 2026-10-10
  mechanics-freedom ruling): `TransportStream::Emit()` has no topic frame, and `EventSerializer`
  publishes bare `Event`-rooted buffers directly (not `Envelope`-wrapped, despite `Envelope` being
  the schema's declared `root_type`) while `PositionUpdate`/`Diagnostic` frames are separately
  `Envelope`-wrapped onto the *same* socket. `lbrnet`'s real consumer (`lbrnet/event_consumer.py`)
  calls `GetRootAsEvent()` unconditionally with no type check — a real, pre-existing ambiguity, not
  a tidy single-topic design worth replicating. `transport`'s Rust port should give port 5555 a
  proper per-message-type discriminator (e.g. a one-byte type tag ahead of the FlatBuffer payload,
  or consistent `Envelope`-wrapping for every message including `Event`) — verify the real current
  wire behavior with a live capture first, then design `signal_feed.rs`'s parser against the
  corrected framing, not the ambiguous one.

No feedback channel from Rust back into the C++ observation vector is anticipated — the
observation vector is market-microstructure, not account/position state (confirm this holds before
building; if `ContextManager` turns out to need live position awareness for regime gating, that's
a new, explicit wire message, not an implicit assumption).

**Position/fill updates for GUI display need a third, Rust-owned channel.** `PositionUpdate`'s own
doc comment says "Sent by C++ PositionManager" — that class won't exist once this ships. Rust is a
separate process; it cannot call into C++'s in-process `TransportStream` singleton, so it needs its
own `transport`-mediated PUB (new port) for position/fills/P&L. **Recommend** `MTS`/eventual
`GUI/` subscribes to that new stream, keeping the GUI exactly as passive as it is today
(display-only, no new PyO3 dependency) — §8.7 D6 is the open question on whether GUI needs
anything beyond display.

### 8.6 Backtesting — the hard part, and why

`BackTesterStudy.cpp` runs the *same* `PositionManager`/`RiskManager` C++ classes in-process during
Sierra Chart replay as in live trading — `sc.BuyOrder`/`SellOrder` resolve to Sierra's own internal
historical-replay fill simulation, not a real broker. `ibapi` has no equivalent: it only knows how
to talk to a real TWS/Gateway (live or IBKR paper), which cannot replay arbitrary historical dates
at controlled speed. If the execution layer moves wholesale to Rust, naively this is a:

- **(A) Pluggable `BrokerAdapter` trait — recommended.** `position_state`/`stop_manager`/`trap_exit`
  run against a `ReplayFillAdapter` during backtest (synthetic fills driven by the same ZMQ price
  stream already feeding `signal_feed.rs`) and an `IbkrAdapter` live — one Rust implementation of
  the business logic either way. This is the only option consistent with the repo's own
  Backtesting Governance Contract ("Treat backtesting as a mode over live architecture; do not
  create parallel business logic paths outside explicit backtesting surfaces") and with the whole
  point of this initiative (train/offline/live stop being separate implementations).
- **(B) Freeze the old C++ path for backtest, Rust for live only.** Explicitly **not recommended** —
  this is precisely the skew this initiative (§1.2) exists to eliminate, just relocated to the
  execution layer instead of the observation vector.
- **(C) IBKR paper-trading playback.** Infeasible — no historical-date replay at controlled speed
  on a live paper account.

If (A), `.btst` artifact production (`DecisionEvent`/`PredictionAck`/`TradeRecord`) needs a new
owner — either the Rust service writes `.btst` records itself (new FlatBuffer-writing code in
Rust, reusing `rust/schema`), or `BackTesterStudy.cpp` keeps writing them by observing the
`ReplayFillAdapter`'s own output over a local channel. **Open, §8.7 D7.**

**RULED 2026-10-13 (§8.7 D2)**: (A), with a Sierra Chart simulation "broker" -- an evolved
`BackTesterStudy.cpp` relaying the Rust service's intents into Sierra's simulator -- as the primary
replay adapter, deliberately pessimistic (limit entries filled only on trade-through), plus an
explicit IBKR-algo-to-Sierra-order mapping and a three-tier validation ladder. Full ruling in D2.

### 8.7 Decision register — open items (operator sign-off needed before implementation)

- **D1 — Wire contract. RULED (operator, 2026-10-10): split channel** (§8.5), **and all of it
  rides on `transport`, not bespoke zmq usage in `rust/execution`** (ruled, operator,
  2026-10-10 — "we are migrating 'everything' ZMQ related to Rust") — port 5555 PUB reused as-is
  for continuous price/context ticks (lossy acceptable, matches today's SNDHWM=1000 drop
  behavior); a **new** acknowledged channel for discrete execution-intent messages
  (direction/size/stop/target), mirroring the existing 5556/5558 REQ/REP pattern; a **new**
  position/fill PUB port (Rust is a separate process, can't reach into C++'s in-process
  `TransportStream`) — both new ports are `transport` inventory, not sockets owned by
  `rust/execution` itself. Still open, needed before implementation:
  - the new channel's exact port number(s) and transport (REQ/REP vs. ROUTER/DEALER — the
    order-watcher's `tokio::select!` may want non-blocking, favoring ROUTER/DEALER, and §7 already
    proposes a non-blocking `submit`/`poll` shape for its existing request/reply ports — the new
    channel should likely use that same shape, not a third pattern);
  - the new schema message's exact shape (extend `TradeRequest` vs. a dedicated new table —
    extending risks breaking `TradeRequest`'s other consumers' assumption that those fields are
    always unset; a new table is likely cleaner);
  - confirming the real (not documented) root-type discrimination behavior on 5555 via a live
    capture before writing `signal_feed.rs`;
  - **the sequencing conflict this creates against §7's own migration order**: workstream W9
    (`transport`, port by port, plan §1.3) is explicitly "deliberately last," gated on W7 (HMM
    event-schema cutover, which deletes port 5561 first) — but this component needs at least a
    slice of `transport` (the skeleton + the two brand-new ports) much sooner, as soon as the
    MES-futures algo what-if spike (§8.8) clears. **Resolution**: the two new ports carry no
    existing peer to break (unlike 5555/5556/5558/5561, where the "flip one port, peer unchanged,
    revert = flip back" discipline exists precisely because a live peer could regress) — pull
    forward only `transport`'s skeleton + these two new ports (plan's W9 row/crate table
    already reflect this split).
- **D2 — Backtesting fill simulation. RULED (operator, 2026-10-13): (A), `BrokerAdapter` trait,
  with a Sierra Chart simulation "broker" as the primary replay adapter, built by evolving
  `src/BackTesterStudy.cpp`; entries modelled as limits filled only on trade-through.**
  - **Purpose, stated by the operator**: the Sierra replay is a deliberately *pessimistic* test of
    the execution logic, not a live-P&L forecast. IBKR's algos are expected to execute at least as
    well as the replay's fills, so a system that holds up here should do no worse live, and
    fundamental execution-logic flaws surface in replay rather than in live trading.
  - **Why Sierra replay is the primary engine, not a stopgap**: under §8.2's scope Sierra/ACSIL
    keeps owning signal generation (Triple Screen, Raschke patterns, indicators), so a full-system
    backtest needs Sierra replaying the charts. A pure-Rust tick-driven adapter (the 476M-tick
    history via `tools/market_data_replay/`) can only exercise the execution layer against recorded
    signals -- a later, narrower tool the trait keeps possible; revisit once §9 moves indicator
    computation to Rust.
  - **Sierra's simulator as a broker**: the backtest-only C++ is a thin relay (place / cancel /
    flatten by order id, report fills and order updates back), with no decisions in it -- the same
    relationship `IbkrAdapter` has with TWS, so it does not violate the "no parallel business logic"
    governance rule. A Rust DTC client to Sierra's DTC server is a possible later replacement for
    the relay, not a prerequisite (whether DTC orders fill under *chart replay* is unverified).
  - **What `BackTesterStudy.cpp` already provides and keeps**: in-process replay of the real
    pipeline, Phase 2/3 modes, fault injection (emergency flatten at bar N, simulated
    disconnect/reconnect), walk-forward windows, run manifest, `.btst` output. It evolves by
    swapping its direct `sc.BuyOrder`/`CancelOrder`/`FlattenAndCancelAllOrders` path (today inside
    `PositionManager`/`RiskManager`) for intents from the Rust service, and gains unattended
    orchestration via ACSIL replay control (verified present in the vendored `sierrachart.h`):
    `StartChartReplayNew` with `REPLAY_MODE_ACCURATE_TRADING_SYSTEM_BACK_TEST` and `ChartsToReplay`
    covering the TS1/TS2/TS3 charts, `GetReplayHasFinishedStatus`, `Pause`/`ResumeChartReplay`.
  - **A1 -- conservative fills, imposed by construction (Sierra's simulator is not automatically
    conservative)**:
    - *Entries: limit, filled only on trade-through.* On MES's 0.25 grid, a buy limit at P is traded
      through exactly when a trade prints at <= P - 1 tick -- which is exactly when a touch-filled buy
      limit at P - 1 tick fills. So if Sierra's simulator fills limits on touch, the relay places
      the limit one tick better and the pessimistic report overlay (below) books the fill at P (mirror for sells). **First
      verify which semantics Sierra uses**; if it is already trade-through, no offset (otherwise the
      pessimism is double-counted).
    - *One-event activation delay*: under lockstep the relay places each order on the replay event
      after the decision, never on the decision's own tick.
    - *No `LIMIT_CHASE` in this tier*: its zero-latency repricing (`PositionManager.cpp`'s own
      comment) is optimistic, the opposite of this tier's purpose. (Today's C++ entry path uses it;
      that is the pre-migration live behaviour, not the backtest model.)
    - *Stops*: Sierra fills a triggered stop at the triggering trade; real stops slip, worst in gaps,
      so the pessimistic report overlay applies a 1-tick adverse haircut to every stop and market fill.
    - *Costs*: the real $0.61/side commission measured in W0e (D3), plus CME exchange/regulatory
      fees (exact figure to confirm).
    - *Calibration check, once*: replay a few days with the A1 rules vs. plain touch-filled limits
      and Sierra's limit-chase, and confirm A1 is the most pessimistic of the three.
  - **IBKR algo -> Sierra-sim order mapping** (match the algo's character where it can be modelled,
    and resolve every ambiguity pessimistically):

    | Live order (IBKR) | Sierra-sim translation | Notes |
    |---|---|---|
    | Adaptive Patient | `LIMIT` at the passive touch (buy at bid), trade-through | Patient rests; may miss the fill -- intended |
    | Adaptive Normal | `LIMIT` at the passive touch, trade-through | MES's 1-tick spread has no on-grid midpoint, so Normal's price improvement is not modelled -- pessimistic |
    | Adaptive Urgent | `MARKET`, overlay +1 tick adverse | Urgent is meant to fill, so the trade-through rule does not apply; it pays the spread plus slippage |
    | Bracket target leg (IBKR `LMT` child) | attached `LIMIT` (OCO), trade-through | Same one-tick emulation |
    | Bracket stop leg (IBKR `STP` child) | attached `STOP_WITH_LAST_TRIGGERING` (OCO), overlay +1 tick adverse | Last-trade triggering avoids bid/ask-trigger differences from IBKR's default |
    | Emergency flatten / trap-floor exit | `MARKET`, overlay +1 tick adverse | |
    | TWAP / VWAP / PctVol / DarkIce | not modelled | Slicing/size-hiding algos have nothing to slice at 1 to a few MES contracts. If live size ever warrants them, the relay emulates them as N child limits spaced over the window (deferred) |

  - **A2 -- lockstep and clock**: replay must never run ahead of the Rust service. The study blocks
    on (or pauses replay for) an acknowledgement per decision -- the existing 5558 REQ/REP
    `BacktestLiveAgent` pattern is the precedent -- and the Rust service's clock is the replay's
    event timestamp, never wall time (every timer in `order_watcher.rs`/time-barrier logic goes
    through an injectable clock from its first line).
  - **Results -- the broker is the source of truth (CORRECTED 2026-10-13, operator; this bullet
    originally made a Rust ledger the source of P&L, which would have been a second, drift-prone
    copy of numbers the broker already maintains)**: position, average price, realized/unrealized
    P&L, commissions and fills are always *read* from the broker of record -- TWS via `ibapi` live,
    Sierra's simulator (`sc.GetTradePosition`, fill data, `GetTradeStatisticsForSymbolV2`) in
    replay -- never recomputed in Rust. `position_state.rs` is a **broker mirror plus annotations**:
    it holds the broker's last-reported state, adds what no broker knows (the `decision_id` and
    pattern a trade came from, for `.btst` join keys; MAE/MFE; orders sent but not yet
    acknowledged), and **reconciles** the position implied by the broker's own fill stream against
    the broker's position reports -- the broker always wins, and a divergence that outlasts a short
    grace window raises an alert (the signature of an orphaned bracket leg or a double entry,
    exactly the flaws this backtest tier exists to catch). When the broker is unreachable the
    mirror reports "last known as of T, stale" rather than silently serving old values. The A1
    adjustments are a **backtest-only report overlay** computed from Sierra's own fill list
    ("broker P&L" vs. "pessimistic P&L"), never a parallel position tracker; live, TWS's numbers
    stand unadjusted. The existing C++ shows why: `RiskManager.cpp` reads broker truth
    (`GetTradePosition`/`OpenProfitLoss`) while `Trade.cpp` keeps its own P&L in points with no
    multiplier or commissions -- two P&L figures in one system today. Parameter sweeps
    change the Rust service's config per run; `SetChartStudyInput` only for signal-side (ACSIL)
    parameters.
  - **Migration parity mode**: while C++ `PositionManager` still exists, the same replay runs the
    C++ logic and the Rust service side by side and compares decisions -- the observation-vector
    shadow-wire discipline applied to execution, making the cutover verifiable.
  - **Validation ladder (acceptance path)**: (1) Sierra replay, pessimistic fills -- logic flaws,
    fast, repeatable; (2) IBKR paper trading forward in real time -- real algos, rejects, partial
    fills, disconnects, pacing, bracket-child handling, none of which replay can show; (3) live at
    minimum size.
  - **Built, 2026-10-13 (slice 2, pure Rust, no sockets)**: `rust/execution/src/broker.rs` (the
    broker-neutral `OrderRequest` and the object-safe `BrokerAdapter` trait: `place`/`cancel`/
    `flatten`/`poll_events`, with adapters reporting their own submissions as `position_state`
    events), `sierra_sim.rs` (this D2 table as one tested pure function returning the exact Sierra
    orders plus per-leg overlay instructions; Sierra's limit-fill semantics must be passed
    explicitly -- no default -- until the spike below measures them) and `ibkr_orders.rs` (the live
    counterpart, restricted to combinations W0e saw IBKR accept). Both translators share one
    rounding rule: limits snap against us, stop triggers snap to trigger later -- the opposite
    direction from a limit on the same side, a bug the tests caught during development.
  - **Relay message contract (PROPOSED 2026-10-13, not implemented -- needs `transport` and a
    shared-schema change, D1)**. The C++ relay study is dumb: Rust sends it `sierra_sim::translate`'s
    output verbatim, and it reports back what Sierra did.
    - *Rust -> relay*: `PlaceSierraPlan { request_id, decision_id, parent, target?, stop? }` (each
      leg: `SCT_ORDERTYPE_*` id, side, quantity, price); `Amend { internal_order_id, quantity,
      price }` (`sierra_sim::amend`'s leg, via `sc.ModifyOrder`; added 2026-10-13, decision #39);
      `Cancel { internal_order_id }`; `Flatten {}`.
    - *Relay -> Rust*: `OrderAck { request_id, parent/target/stop InternalOrderIDs }`,
      `OrderRejected { request_id, code, message }`, `OrderCancelled { internal_order_id }`,
      `OrderAmended { internal_order_id, quantity }`, `Fill { internal_order_id, fill_id, signed_qty, price, at }`, `PositionSnapshot { quantity,
      average_price, open_pnl, closed_pnl, at }` (from `sc.GetTradePosition`) -- each maps 1:1 onto
      a `position_state::Event` -- plus the replay timestamp of every step (the Rust clock, A2).
    - *Lockstep*: the relay is the requesting side; each replay step that carries anything (a
      price update, a fill, an order-state change) is one request carrying that step's events, and
      the relay does not return from its study call until Rust replies with that step's commands.
      **Open design point**: in replay the continuous price feed must be lockstepped too -- port
      5555's lossy PUB cannot be the bracket/trap evaluator's price source during a backtest, or a
      fast replay outruns Rust; folding the step's price into this request is the likely answer.
    - *Request cost*: one round trip per replay step bounds backtest speed; batching steps that
      carry no decision-relevant change is the obvious optimisation, to be measured, not assumed.
  - **Still open under D2**: Sierra's limit-fill (touch vs. trade-through) and stop-fill-price
    semantics in `REPLAY_MODE_ACCURATE_TRADING_SYSTEM_BACK_TEST` (first spike: one automated replay
    across the three charts, one stub decision from Rust, one relayed order, fill checked against
    replay-time prices); exact CME fee figure; relay vs. DTC.
- **D3 — IBKR algo selection for MES futures. Order-working mechanism RULED (operator,
  2026-10-10): rely on IBKR algos exclusively — no Rust-native stepping fallback.** The
  order-watcher's only two actions on a live algo order are **update** (IBKR `modifyOrder` in
  place [**superseded 2026-10-13, decision #39**: the watcher's entry-order repertoire is hold /
  cancel (+ `Finished`/`Escalate`); amends exist on the broker seam as `BrokerAdapter::amend`,
  see D3a's order- vs. position-management split] — for price/quantity-only changes that don't change direction or algo type) and **cancel**
  (for structural changes — direction flip, algo-type change, thesis invalidated by the trap floor,
  time limit expired), never a hand-rolled price-stepping loop. **Corrected 2026-10-11 (see D3a)**:
  the original "e.g. a trailing-stop recompute moving the stop a tick" example for **update** was
  wrong — under the current `TripleBarrierExitManager` design the stop/target bracket is computed
  once at entry and is immutable thereafter (no trailing recompute exists to update). The
  **update** case, if it applies at all, is on the still-unfilled *parent* entry algo order (e.g. a
  price/quantity nudge before the algo has filled) — not on the bracket's stop/target legs, which
  once placed only ever get **cancelled** (regime-invalidation kill-switch, vertical/time-barrier
  expiry) — confirm or correct this narrower split before implementing `order_watcher.rs`.
  **De-risked 2026-10-10 (teammate research, Gemini)**: IBKR's own documentation (Order Types/Algos
  product-support matrix, the Web
  API's `get-algos-by-instrument` endpoint, Traders' Academy, and the `ibapi`/`order.algoStrategy`
  reference) states native IB Algos support `secType = "FUT"` generally and CME futures
  (`ES`/`MES`) specifically, with **Adaptive**, **TWAP**, **VWAP**, and **Balance Impact and Risk**
  named as concrete candidates — this was an unverified assumption before, now a
  documentation-confirmed expectation, substantially lowering the risk this blocks the design.
  **W0e RESULT, 2026-10-13 (empirical, paper account `DUT056479`, TWS server version 225, front
  month `MESZ6` conId 815824257 -- min tick 0.25 and multiplier 5 both read from IBKR's contract
  details, not assumed)**: `rust/execution/examples/w0e_mes_algo_whatif.rs`, `ibapi` 5.0.0
  blocking client, every order `what_if = true`, paper-account guard before any request, and an
  independent post-run open-orders check (0 open orders). What-if BUY 1 MES, commission $0.61,
  initial-margin change ~$3,538 for every accepted variant:
  - **ACCEPTED**: plain LMT and MKT; **Adaptive** at Urgent/Normal/Patient on LMT, and Normal on
    MKT (D3's first choice -- confirmed); **TWAP** (no params, or `allowPastEndTime=1` -- a first
    attempt with `strategyType=Marketable` got `[443] Unknown algo attribute: strategyType`, a
    parameter error on this product, not a support gap); **VWAP** (`maxPctVol=0.1`).
  - **REJECTED**: **Balance Impact and Risk** (`BalanceImpactRisk`) and **Arrival Price**
    (`ArrivalPx`) -- both `[439] Algorithm definition not found`, i.e. not defined for this
    instrument on this account. This contradicts the 2026-10-10 documentation research, which
    named Balance Impact and Risk as CME-futures-capable -- exactly the "documentation can be stale
    or account-permission-gated" risk this check existed for. Neither was a primary candidate, so
    nothing in D3's ruling changes: `order_watcher.rs` works orders with Adaptive (alternates:
    TWAP, VWAP); BalanceImpactRisk/ArrivalPx are out unless a later check on the live account
    says otherwise (the live account's algo permissions were deliberately not probed -- the spike
    refuses non-paper accounts).
  - **Round 2, same day** -- testing a teammate research note (Gemini) that claimed
    BalanceImpactRisk is options-only and named Arrival Price, PctVol and Dark Ice as the CME-futures
    alternatives, routed via an `IBALGO` destination. Empirically: **PctVol** (`pctVol=0.1`) and
    **DarkIce** (`displaySize=1`) are ACCEPTED -- two more usable alternates; Arrival Price is
    REJECTED under both strategy strings (`ArrivalPx` and the note's `ArrivalPrice`, both `[439]`);
    routing to an `IBALGO` exchange is REJECTED (`[200] No security definition`) -- through the
    API, algos are chosen by `algo_strategy` with the contract on its own CME exchange, which is
    how every accepted variant was sent. The options-only claim for BalanceImpactRisk is consistent
    with round 1's `[439]` but unsourced. Usable on MES, all accepted: Adaptive, TWAP, VWAP, PctVol,
    DarkIce.
  - **`ibapi` 5.0.0 over WSL2 (mirrored networking) -> Windows TWS**: the stock 1s read timeout
    held -- the session survived a 300s idle soak, after which fresh `managed_accounts`/
    `contract_details` round trips took 47ms. Atratus's vendored 1s->20s patch (on 4.2.0) is NOT
    needed on this evidence, so `ibkr_session.rs` starts on unpatched upstream 5.0.0. Caveat:
    this exercised an idle long-lived connection, not slow/gappy responses (5.0.0's own
    mid-frame-stall tolerance targets that case; not yet observed here).
  - **Implementation note for `mes_futures.rs`**: a `ContractDetails.contract` echoed back into a
    request fails `[10372]` (its `last_trade_date_or_contract_month` comes back in a form TWS
    rejects); reference contracts by conId + exchange (Atratus's `by_con_id` precedent).
  **Original framing, kept for the record (now done, see the result above)**:
  an empirical read-only what-if check against this specific account/contract-month (Atratus's own
  precedent: `scripts/put_order_whatif.py`, generalized to an MES-futures equivalent, trying
  Adaptive first, TWAP/VWAP/Balance Impact and Risk as alternates) — documentation can be stale or
  account-permission-gated, so this isn't skipped, but it is no longer the hard blocking spike it
  was originally framed as; a negative result would now be a genuine surprise worth escalating,
  not an expected possible outcome.
  - **Order-watcher decision core built, 2026-10-13** (`rust/execution/src/order_watcher.rs`): a
    behavior-preserving port of `PositionManager::ManageWorkingEntryOrder` + `IsQuoteChurnBreached`
    -- the same ordered cancel checks (intent expiry, working-order TTL, HMM transition risk,
    execution budget, spread, toxic flow, quote churn, prediction direction, prediction staleness),
    first match wins. Its repertoire is hold / cancel for the unfilled entry, plus `Finished`
    (the broker reports the order done) and `Escalate` (broker unreachable: unknown state, hand to
    reconciliation / kill switch -- the C++ `ScDisconnected` path dropped the order, an orphan).
    No amend decision, because an IBKR algo reprices its own child orders -- a consequence of the
    algo choice, not evidence for D3a (the C++ never amended only because Sierra's limit-chase
    repriced by cancel + new). Clock-free, with two caller-supplied clocks (decision #39): market
    time for intent expiry and quote churn, a steady local clock for working TTL and execution
    budget, so a stalled feed cannot freeze the watchdog; the async shell calls it on a timer as
    well as on market events.
  - **OPEN DECISION (operator) -- escalate urgency instead of cancelling.** Raising a working
    Adaptive entry from Patient/Normal to Urgent (an amend of `adaptivePriority`) is the
    institutional alternative to an `OrderAckTimeout` cancel. Not built: it changes behavior, it
    is coupled to the budget decision below, IBKR's acceptance of an algo-parameter modify on MES
    is unverified (W0e did not test modifies), and its Sierra-sim equivalent (limit -> market)
    needs D2's fill-semantics spike first.
  - **OPEN DECISION (operator) -- entry time budgets for IBKR algos.** The C++ budgets were tuned
    for Sierra's zero-latency limit-chase: execution budget = min(session budget 800-1200 ms,
    ack-timeout 1200 ms) x Fisher scale (0.75/1.0/1.5), then capped by the climate (100-500 ms,
    `InferenceManager::GetRegimeExecutionBudgetCap`). The climate cap is always below the session
    budget, so in practice an entry is cancelled as `OrderAckTimeout` after ~375-750 ms -- far too
    short for an IBKR Adaptive order (Patient especially routinely needs seconds or more). The
    port therefore takes every budget as configuration (`WatcherConfig::sierra_chase_defaults()`
    holds the C++ values for parity); the values for IBKR algos are not chosen yet. Two related
    notes: the climate the cap keys on is today's `MarketClimateIndicator`, whose live write race
    (decision #29) means it mostly reflects the native heuristic, not the trained HMM; and the
    C++ quote-churn window reads the machine's local clock, not exchange time.
- **D3a — Bracket order construction mechanics, verified against the real `ibapi` 5.0.0 crate
  (already vendored in this environment), 2026-10-11** (operator-supplied research, checked
  against actual crate source rather than trusted as documentation claims; **premise corrected
  2026-10-11, same day** — the original version of this entry assumed `stop_manager.rs` needs a
  Chandelier-style continuously-trailing stop, which is stale — see the target-architecture
  correction above. Rewritten to match the real current design: a static, entry-anchored,
  immutable bracket, `TripleBarrierEngine.h`/`TripleBarrierExitManager.h`'s actual behavior).
  - **An IB Algo is an entry/execution algorithm, not an order container** — it is the *parent*
    leg; bracket legs (take-profit, stop-loss) are ordinary child orders attached via `parent_id`,
    sent as one atomic tree (`parent.transmit = false`, intermediate children `transmit = false`,
    final child `transmit = true`). Verified field-for-field against `ibapi::orders::Order`
    (`parent_id: i32`, `transmit: bool`, `aux_price: Option<f64>`, `algo_strategy: String`,
    `algo_params: Vec<TagValue>`) — implementable with today's dependency, not a hypothetical
    shape.
  - **The crate's own convenience `.bracket()` builder (`BracketOrderBuilder::build()`) is a good
    fit for the actual current design, not a limitation to work around.** It builds exactly a
    fixed-price `LMT` take-profit child + fixed-price `STP` stop-loss child, both derived once at
    construction time — matching `TripleBarrierExitManager::OpenBracket`'s own "compute + latch the
    immutable bracket at entry" contract field-for-field (`tbe::ComputeBarriers()`'s `stop`/
    `target` outputs map directly onto `.stop_loss(price)`/`.take_profit(price)`). No manual
    trailing-stop construction (`TrailBy`, `trailing_percent`, `trail_stop_price`) is needed —
    those fields exist in the crate for other use cases, not this one.
  - **ORDER management vs. POSITION management (rewritten 2026-10-13, decision #39 -- supersedes
    the next bullet's "never update")**. Two layers, which the earlier text conflated:
    - *Position management (strategy)*: where the stop and target sit, and when to leave (TRAP,
      regime invalidation, time). Phase 1's triple-barrier doctrine keeps the barrier LEVELS where
      entry put them, for label parity with the triple-barrier labeler (cutover ADR §1, Option A);
      breakeven/trailing moves were rejected on evidence for these fade stops (ADR §1 item 4).
      This is deferred, not prohibited: a tactical level change is admissible once the labeler
      can reproduce it (a deterministic path rule or a trained meta-model, ADR §1 items 1/4), and
      regime-scaled targets at entry are already sanctioned (ADR §1, "Rank 2").
    - *Order management (execution)*: how the orders implementing those levels are worked. Amends
      that do not move a barrier level (quantity, repricing a target limit for fill quality, and
      later possibly stop-market vs. stop-limit as the C++ once chose by regime) are ordinary
      execution and go through `BrokerAdapter::amend`. The adapter does not police doctrine; the
      strategy layer decides what to ask for. Single-controller rule (ADR §1, "dual-controller
      stop = critical defect") still holds: one owner per order.
  - **What "update" and "cancel" actually mean for this bracket (2026-10-11 text; its "never
    modified" is the Phase 1 STRATEGY rule for levels, not an execution rule -- see above)**:
    once placed, the stop/target legs are never modified (matching D3's corrected update/cancel
    split above) — `TripleBarrierExitManager::Evaluate()`'s own public API has no mutation method
    at all (`OpenBracket`/`Evaluate`/`Current`/`Close` only), confirmed by direct read. The only
    action ever taken on a live bracket is **cancel** (regime-invalidation kill-switch, vertical/
    time-barrier expiry, both resolved by `Evaluate()`'s first-hit-wins logic) — never update. Any
    "update" action in `order_watcher.rs`'s repertoire applies solely to the still-unfilled parent
    entry algo order, a separate concern from the bracket legs this section is about.
  - **`trap_exit.rs` is the reactive TRAP floor** (D3's "cancel" case: "thesis invalidated by the
    trap floor") — a discrete emergency cancel-and-flatten, unaffected by any of the above; it was
    never a trailing-stop concern to begin with, correction or no correction.
  - **Not currently relevant, parked for later**: IBKR's native ScaleTrader/Accumulate-Distribute
    algos (scale-in/scale-out with per-clip profit-taking) — this system is single-entry today, no
    scale-trading design exists to connect this to; worth remembering only if that ever changes.
- **D4 — Port fidelity for stop/trap logic — this is business-logic behavior, not transport
  mechanics, so the 2026-10-10 "Rust best-practice freedom" ruling (§7, §8.1) does not loosen it.**
  Recommended: literal behavior-preserving port first (`TripleBarrierEngine.h`/
  `TripleBarrierExitManager.h` → `stop_manager.rs`/`trap_exit.rs`), same discipline as this
  document's own non-goals ("no change to... any threshold, as a *side effect* of porting"). A
  redesign, if wanted, is a separate, explicitly-flagged decision — same bar as any other
  trading-behavior change. Contrast with `signal_feed.rs`/`intent_client.rs`/`position_feed.rs`'s
  wire mechanics (§8.5), which **are** free to be redesigned for idiomatic Rust/DOD/FlatBuffers
  practice.
- **D5 — Live arm / kill-switch mechanics.** Recommended: adopt Atratus's `state/<env>/
  LIVE_ORDERS_ARMED` (typed-confirmation script only) + global `state/KILL_SWITCH` pattern outright
  — proven, cheap, no reason to redesign.
- **D6 — GUI's relationship to the new service.** Recommended: GUI stays passive — it subscribes
  to a new Rust-published ZMQ position/fill stream (§8.5), no PyO3 dependency added to `MTS` by
  this component. Atratus's PyO3-`IbSession`-for-GUI pattern is **not** recommended here unless/
  until the GUI itself needs direct control actions beyond display.
- **D7 — `TradeExecutionServer` (port 5558) fate.** Open: retire outright (if the Rust service
  takes over `.btst` writing per §8.6(A)), or keep it as the interface `BackTesterStudy.cpp` uses
  to drive the Rust engine during replay. Depends on D2's resolution.
- **D8 — Documentation-contract fallout**, to execute once this component lands, not before: the
  Trap Detection Ownership section in all four mirror docs currently says "C++ must implement and
  run a native trap-risk detector" — this needs generalizing to "the native execution layer (C++
  today; Rust once this component lands)" without weakening the actual principle (Python-optional,
  model-independent reactive floor). Do this edit when `trap_exit.rs` actually ships, not
  speculatively now.
- **D9 — Process/deployment model.** Open: one long-running Rust process per trading session,
  started/stopped alongside Sierra Chart (manually, or via a supervisor)? Where does it run
  relative to the WSL2+Windows split already in play for this account (`docs/PUGET_SETUP_
  COORDINATION.md`)? Needs its own small design pass once D1–D3 are settled.

### 8.8 Next concrete step

**Update, 2026-10-13**: the spike below has RUN -- D3 confirmed (Adaptive/TWAP/VWAP accepted on MES,
BalanceImpactRisk/ArrivalPx not defined; PctVol/DarkIce also accepted in a round-2 check), and stock
`ibapi` 5.0.0 held over WSL2; full result in
§8.7 D3. D2 and D9 remain open as stated below.

Not a go-ahead to start writing `order_gate.rs`/`position_state.rs` yet — D2 (backtesting) still
changes how much of §8.4's module list needs a `ReplayFillAdapter`-aware design from day one, and
D9 (deployment topology) isn't settled. D3 (does IBKR even accept algos on MES futures) is
**de-risked as of 2026-10-10** (teammate documentation research — Adaptive/TWAP/VWAP/Balance
Impact and Risk all named as supporting CME futures) but not yet empirically confirmed against
this account. The recommended first concrete step, mirroring this plan's own W0s spike (§2 — "does
the kill criterion even pass"): a small throwaway spike that (a) opens a real `ibapi` connection to
this account's TWS/Gateway from a bare Rust binary and (b) runs the MES-futures algo what-if check
(D3) as a confirmation rather than a true unknown — resolving the last real doubt before any of the
module skeleton is written.

---

## 9. Component spec: indicator computation (scope only, 2026-10-10 — design not yet started)

**Ruled (operator, 2026-10-10)**: the actual compute logic behind every indicator — MACD, RSI,
Kangaroo Tail, Turtle Soup, NR7, stochastic, and the rest of the ~38 leaf classes in
`IndicatorStore`/`BaseIndicator`/`Indicator<T>` — moves to Rust. This is the same "full write-side
rewrite" `docs/superpowers/plans/2026-08-05-indicator-manager-dod-soa.md` identified, scoped out as
a deferred "Phase III," and declared the C++ write-side compute engine "permanent" in the absence of
(that plan predates, 2026-08-05) — see §1.2's standing principle on pre-Rust-adoption decisions.
That plan's own completed work (the packed-array read-side, the dual-write mechanics, Task 15's
narrower dead-class deletion) is unaffected and remains accurate history; only the write side's
"permanent" framing is superseded.

**What's confirmed so far**: the hard rule from §1.2 — Rust indicator-computation code never
references `sc.*`/ACSIL; C++'s Triple Screen/study functions read raw ACSIL data and call into Rust
with plain values, same shape as `observation_vector`'s existing "pure core, thin edges" pattern,
just scoped to the full indicator set instead of the 18 observation dims.

**Not yet decided — needs its own design pass before any port starts**:
- Crate name and workspace placement (a new `rust/indicators` sibling, or folded into
  `observation_vector` given the overlapping "pure function over plain inputs" shape — the two
  are conceptually adjacent but today serve different consumers, worth keeping distinct unless a
  real reason to merge surfaces).
- Sequencing relative to `student_t_hmm`/`execution` — not yet placed in the plan's workstream table
  (plan §1.3) or given a `W`-number.
- How this interacts with `IndicatorManager`'s packed-array (`IndicatorLayout.h`/
  `IndicatorPackedState.h`) read-side machinery the 2026-08-05 plan already shipped — does C++ keep
  writing into the packed arrays from Rust-returned values (packed arrays survive as the hot-path
  read cache, only the compute step moves), or does the packed-array design itself get revisited
  now that the write side is Rust, not C++? Open.
- Per-indicator porting order and parity-test strategy — likely the same proven pattern as every
  other port in this document (port → unit test → cross-language parity test → shadow-wire →
  live-confirm → cutover), but not yet confirmed for this specific, much larger surface (~38
  classes vs. observation_vector's 18 dims).

---

## 10. Risks

1. **The central Rust-in-Sierra assumption was unproven until W0s** — now resolved (plan §2).
2. **Moving targets**: the observation-vector dims and the HMM (K, features, the unmet fat-tail
   sign-off) are still changing; ports must be manifest/golden-driven so a change lands in one place.
3. **Unvalidated references**: the offline reconstruction has never been byte-validated against a
   genuine Sierra-collected file at the current schema version (blocked on a v240 comparison file not
   existing yet) — Rust ports will match the C++ reference, not necessarily live Sierra, until that
   exists.
4. **Two libzmq copies in one DLL** (expected during the transport transition, untested) — the plan's
   §6.5 has the interim plan; resolve before `transport`'s W9.
5. **`-ffast-math` silently broke this codebase's NaN-sentinel carry-forward convention — found,
   confirmed, and fixed, 2026-10-11.** `CMakeLists.txt` enabled `-ffast-math` unconditionally for the
   whole DLL (`-O3 -march=native -ffast-math -mavx2`, both presets) since before this initiative
   started. Empirically verified with the exact clang-22 toolchain and exact repo flags: under
   `-ffast-math`'s `-ffinite-math-only` component, the compiler assumes no value is ever NaN/Inf and
   folds `std::isnan()` to `false` unconditionally (Clang itself emits `-Wnan-infinity-disabled: use
   of NaN is undefined behavior due to the currently enabled floating-point options` at the exact call
   sites below) — confirmed via a minimal repro matching this codebase's real runtime-NaN-sentinel
   pattern (isnan reports `1` without the flag, `0` with it, same compiler, same optimization level).
   This directly breaks the "return NaN for a degenerate window, caller carries the last valid value
   forward" convention used throughout `RobustMoments.h` (`MoorsKurtosis`/`BowleySkewness`'s own
   internal carry-forward checks), `StudyHelperFunctions.cpp`'s Hurst/fractal-dim carry-forward, and
   every Rust-FFI shadow-wire comparison this initiative has built (`bothNan = std::isnan(hurst) &&
   std::isnan(rustHurst)`, etc.) — all predating this session's Rust work, a real latent correctness
   risk independent of the migration, surfaced only by this initiative's own numeric-policy question.
   **Fixed**: `-ffast-math` removed from `CMakeLists.txt` entirely (operator decision, 2026-10-11 —
   kept `-O3 -march=native -mavx2`). Re-verified with the post-fix flag set: `isnan()` correctly
   reports `1` again, no compiler warning. Both CMake presets (`MTS_WITH_RUST=OFF`/`=ON`) rebuilt
   clean from scratch. **Not yet done**: a hot-path performance re-benchmark against the
   now-slightly-more-conservative flag set (no existing before/after numbers to compare against —
   this initiative has not yet established a hot-path latency budget at all, §11's own listed gap);
   live Sierra Chart re-confirmation that the degenerate-window carry-forward branches now actually
   trigger (the `std::isnan()` fix is build-verified, not yet runtime-observed against real flat/thin
   data). Full decision: §2 item 11.
6. **A long program touching the same files repeatedly** (`ContextManager`, `HMMClient`,
   `SystemOrchestrator`, the schema, the event stream) — mitigation: every stage reversible until its
   own deletion step, shadow modes throughout, one change at a time.
7. **This is re-platforming, not edge** — this is a standing caution, not a status: do not let this
   program's real progress displace the actual production-readiness gaps it does not itself close (see
   `PRODUCTION_TRIAGE.md`'s own Vision section).
8. **Sierra-process crash risk from Rust**: a panic or allocation on the hot path is a Sierra crash, not
   a log line — `catch_unwind` everywhere, preallocated buffers, an allocation-tripwire test around every
   `*_step` function.
9. **`student_t_hmm`'s full prerequisite stack is unstarted, confirmed 2026-10-11** (operator: "have
   we brought all the data needed for Student-t HMM training into Rust?"). Not just "the model code
   hasn't been written" — checked directly: zero `parquet`/`arrow`/`ndarray`/numpy-compatible
   dependency anywhere in `rust/Cargo.toml`'s workspace, so no Rust code today can read any existing
   training-data format (`.context.parquet`, `.alpha`, `.context.posteriors.npy`). The
   `.context.parquet` files that exist on disk were produced by an existing Python offline conversion
   step, not the already-RULED "write `.context.parquet` directly at collection time" design (§5.4),
   which also isn't implemented (`EventDataCollectorStudy.cpp`/`LBRFileManager.cpp` have zero
   `.parquet` references). No model-artifact format (replacing `models/hmm_model.pkl`) exists; no
   `rust/tools`/`hmm_tool` CLI exists as a workspace member. Golden harness (P9), data-format access,
   artifact format, training CLI, and the model itself are all unstarted — plan §4 has the full
   record.

---

## 11. Not yet analyzed (real gaps in this picture, not silently skipped)

- **Labeler ↔ native `StructureTest` parity** (`lbrnet/labeling/triple_barrier_scanner.py` vs. the C++
  detector, a mandated co-evolution per the trap-detection rules) — a shared Rust core could remove
  this duplicate too; not examined.
- **GUI beyond ZMQ** (Firestore lifecycle, websocket broadcaster, the 3,826-line
  `system_orchestrator.py`).
- **Windows-side deployment**: DLL deploy, Sierra paths, the two live JSON config files outside git
  (`/mnt/c/Trading/config/`) that carry migrated thresholds and won't travel to a new machine without a
  manual sync.
- **CI today**: whether any exists for the four repos currently (Atratus's is Python-only).
- **Hot-path budget**: no per-tick latency measurement exists yet; needed before `observation_vector`/
  `student_t_hmm`'s in-process shadow-mode stage.
- **PyO3 build/packaging**: Atratus installs its extension by hand (editable `maturin`); this needs a
  scripted build (the plan's §6.3).
- **Extending the naming/coherence audit (§4.2) beyond the schema surface** to `lbrnet`'s/`MTS`'s own
  internal conventions.

---

## 12. Documentation sync, when pieces of this land

- "Rust is a build prerequisite for `cpp/`" → the four mirror docs, once `ffi` is load-bearing for
  a real build (not yet — currently opt-in via `MTS_WITH_RUST`).
- The artifact convention change (`models/hmm_model.pkl` → manifest + weights) → the four mirror docs,
  once §5 ships.
- A `PRODUCTION_TRIAGE.md` row for this whole program, added when work actually starts in earnest
  (Triage Protocol rule 7) — not yet added; this is still pre-work/early-port territory.
