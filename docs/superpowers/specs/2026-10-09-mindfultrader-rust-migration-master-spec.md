# MindfulTrader → Rust / Monorepo Migration — Master Spec & Plan

**This is now the single authoritative document for this entire initiative.** It replaces five
separate, drifting documents (deleted from `docs/superpowers/`, content folded in below, full history
still in git): `2026-10-07-mindfultrader-monorepo-consolidation-spec.md`,
`2026-10-07-rust-hmm-lifecycle-spec.md`, `2026-10-08-monorepo-infrastructure-guide.md`,
`2026-10-08-monorepo-rust-adoption-roadmap.md` (plan), `2026-10-09-cross-repo-naming-schema-
coherence-audit.md`. **Reason for the consolidation (operator, 2026-10-09):** five separate documents
on one initiative had already produced real cross-document drift (the same status having to be
updated in two places, decisions duplicated, status sometimes stale in one copy) — this is one
document precisely so that stops happening. **When this document changes, it is the only place that
needs updating.** Do not open a new sibling spec for a sub-topic of this initiative; add a section
here instead.

**Last updated**: 2026-10-09. **Read `SCRATCHPAD.md` and this document's §0 first, every session.**

---

## 0. TL;DR — current status, read this first

**Standing cautions (do not let anything below read as more finished than it is):**
- This is a re-platforming, not a validated trading edge. `PRODUCTION_TRIAGE.md`'s Vision section
  still says the HMM is trained on a contaminated vector and has never passed its fat-tail sign-off.
  Porting faithfully does not fix that; the point of doing it now is that live, offline, and training
  stop being three separate implementations.
- This system is **not in production** (`PRODUCTION_TRIAGE.md` standing rule, 2026-08-26). No special
  caution tier applies to any part of this work, including execution-layer code — the discipline is
  identical everywhere: port → parity-test → shadow-wire → live-confirm → cutover.

**Shipped and verified (dated, with evidence — not aspirational):**
- **W0s (Rust-in-Sierra integration spike): fully resolved, 2026-10-08.** All 9 checks accounted for;
  the kill criterion (does the DLL load in Sierra) passed twice independently with real evidence.
  Full results: §5.
- **`rust/` Cargo workspace scaffolded, 2026-10-08** (commit `63bcc63`): `rust-toolchain.toml` (pinned
  1.98.1), `rust/Cargo.toml` workspace with `[workspace.dependencies]`, `mts_ffi` staticlib skeleton,
  `MTS_WITH_RUST` CMake option/preset, `build_rust.sh`. Verified: native `cargo test`, Windows
  cross-compile, DLL links cleanly, import table byte-identical to the non-Rust build.
- **Two `rust/observation_vector` dims ported, shadow-wired, and live-confirmed in Sierra Chart,
  2026-10-08**: `SevcikFractalDimension` (commit `847a7b7`/`aef526a`) and `ComputeBipowerVariation`
  (commit `9c2b738`). Three-layer proof pattern established (Rust unit tests mirroring C++ goldens →
  cross-language parity test → shadow-mode C++ call site, compare-only, never used → live Sierra Chart
  log confirmation). This pattern is now the template for every future port in this initiative.
- **Third `rust/observation_vector` dim ported and shadow-wired, 2026-10-09**: `DfaHurstExponent.h`
  (`hurst_exponent`) — same proof pattern through parity test + shadow wiring + build verification
  (`MTS_WITH_RUST=OFF` byte-identical at 1,779,712 bytes, `=ON` links and grows to 1,903,616 bytes);
  live Sierra Chart confirmation deferred (pattern already twice proven live). `ComputeHurstFromReturns`
  (dead legacy R/S Hurst, zero callers) deleted in the same session (commit `6343b41`).
- **Fourth `rust/observation_vector` dim ported and shadow-wired, 2026-10-09**: `MeanReversionCalculator.h`
  (`mean_rev_z`) — same proof pattern (21/21 cross-language parity assertions incl. the
  flat-window/null-input carry-forward cases; `MTS_WITH_RUST=OFF` byte-identical, `=ON` links and
  grows to 1,932,800 bytes); live Sierra Chart confirmation deferred.
- **Fifth and final self-contained `rust/observation_vector` dim ported and shadow-wired, 2026-10-09**:
  `LiquidityFragilityEngine.h` (`liq_fragility`) — same proof pattern (26/26 cross-language parity
  assertions incl. the thin-volume/null-input carry-forward cases; `MTS_WITH_RUST=OFF` byte-identical,
  `=ON` links and grows to 1,953,280 bytes); live Sierra Chart confirmation deferred. All dims that
  were self-contained (no `FeatureScaler`/dim-index coupling) per §8.1 are now ported — the next
  observation-vector dims all require P2/P3 (schema-driven dim indices) first.
- **`rust/schema` (`mts_schema`) crate scaffolded, 2026-10-09**: generates from `schema/mts_schema.fbs`
  via a new `flatc --rust` target in `regenerate_schema.sh` (schema commit `2e9528d`). Builds, clippies,
  and tests clean.
- **Schema coherence fixes, 2026-10-09**: `MTS_Envelope` → `Envelope` renamed (schema `79ae486`,
  MindfulTrader `9f899f6`, lbrnet `6b131df`, MTS `457a6a0` — 151 occurrences across 13 hand-written
  files in three repos). `backtest_schema.fbs` merged into `mts_schema.fbs` entirely (schema `fe278f2`,
  MindfulTrader `f95281c`, lbrnet `86e4356`) — one schema file now, not two; Rust generation simplified
  from a 77-file `--rust-module-root-file` tree back to one clean file. Full details: §6.2.
- **Naming hygiene fixed earlier this session**: `mt_`/`MT_` prefix → `mts_` (collided with vcpkg's
  own `-mt` suffix convention and MetaTrader 4/5); `observation` (bare) → `observation_vector` (no
  standalone meaning in this codebase; confirmed against the schema's own distinct `ObservationData`).

**Not yet started (the next real pieces of work, in rough order):**
1. `rust/hmm` (`mts_hmm`): the Student-t HMM lifecycle (train → posteriors → live inference) — §7.
   This is the big one; most of the rest of this initiative (the `.context.parquet`/`.alpha` goal in
   §7.5, the execution-layer "all Rust" scope in §10) explicitly depends on it landing first.
2. Execution-layer computations (`RiskManager`/`PositionManager`/`ExecutionGate`) — confirmed in scope
   2026-10-09, not yet sequenced in detail. §10.
3. `rust/transport` — deliberately last (removing ZMQ port 5561 via the HMM port removes the hardest
   socket `transport` would otherwise have to handle).

**Deferred, own schedule, explicitly decoupled from the Rust work:**
- The four-repo → one-repo merge (§4.2) — the Rust work does **not** wait on this; it proceeds per-repo
  today via cross-repo relative Cargo paths.
- `lbrnet`'s 86-file uncommitted backlog (P7) — separate owner/session.
- The hardcoded Gemini API key in `MindfulTrader/config.py:22` / `MTS/config.py:21` — operator's call
  on timing, not urgent per the operator (2026-10-08).

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
candidates for the same methodical port — see §10. There is no special caution tier for this code: this
system is not in production, so the discipline is identical everywhere in this initiative.

### 1.3 Non-goals

No change to the model family (diagonal Student-t HMM, weighted emissions), to K, to the observation
vector's dimensions, or to any threshold, as a *side effect* of porting — model changes are a separate,
explicitly-flagged decision when they happen. No Transformer change beyond where it reads HMM output
from. The merge (§4.2) is separately gated and does not change code or wire formats by itself.

### 1.4 What stays as-is

The execution layer's *orchestration* (not its low-level computations, see §10), the Triple Screens,
the Transformer, the labeler, and the GUI's display logic are not part of this initiative's current
scope. `BackTesterStudy`/`backtest_server`: every workstream gates on `.btst`-identical replays; the
acceptance gates in `docs/BACKTESTING_FRAMEWORK.md` apply unchanged.

---

## 2. Decision register (single source — do not duplicate this elsewhere)

| # | Decision | State |
|---|---|---|
| 1 | Monorepo at `VSCode/MindfulTrader/`; `MindfulTrader`→`cpp/`, `MTS`→`GUI/`; `Atratus/` stays separate | **ruled**, deferred own schedule (§4.2) |
| 2 | Sequencing: pre-work → merge → `hmm`/`obs` → `transport`; merge decoupled from Rust work | **ruled** (2026-10-08) |
| 3 | HMM lifecycle (train, posteriors, live) moves to Rust; ZMQ port 5561 deleted, not migrated | **ruled** |
| 4 | Observation pipeline moves to Rust; `ContextManager.cpp` itself ports later | **direction set**; dims-first order and numeric policy open |
| 5 | Transformer/HMM coherence rule: C++ HMM state authoritative, sequence-stamped; stale actions (inferred before the current regime began) are not acted on | **ruled**; wording to confirm (regime-epoch vs literal older-sequence, §7.6-1) |
| 6 | Execution-layer computations (`RiskManager`/`PositionManager`/`ExecutionGate`) are in scope, same discipline, no special caution tier | **ruled** (2026-10-09) |
| 7 | One `mts_ffi` staticlib (not per-subsystem `*_ffi` crates) | **ratified by implementation**, 2026-10-08 |
| 8 | `.context.parquet` written directly at collection time; `.alpha`/`TrainingEvent` drops its embedded `ObservationData` copy for a join key | **operator goal, 2026-10-09, not yet decided in full** — depends on `rust/hmm` landing first; §7.5 |
| 9 | `backtest_schema.fbs` merged into `mts_schema.fbs` | **done**, 2026-10-09 (§6.2) |
| 10 | `MTS_Envelope` → `Envelope` | **done**, 2026-10-09 (§6.2) |
| 11 | Numeric policy (fast-math or not, tolerances) | **open** |
| 12 | Manifest holds the regime-engine thresholds (vs. crate constants) | **open**, recommended: manifest |
| 13 | GUI env name (`mts-gui` proposed); whether lbrnet keeps the name `mts` | **open** |
| 14 | MindfulTrader git history scrub (force-push) vs. rotation-only, for the leaked API key | **open**, operator's call |
| 15 | A typed Rust safety gate (Atratus's `ladder_core::safety` pattern) | **deferred** — Sierra stays in the order path for now |

---

## 3. Target architecture

### 3.1 End state (post-merge) directory layout

```
VSCode/MindfulTrader/                      repo root = the monorepo (merge itself: §4.2, deferred)
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
→ lbrnet Transformer → action back through transport → PredictionState (paired by §7.6's coherence
rule)`; the GUI subscribes via the same Rust client. ZMQ port 5561 no longer exists; the observation
vector, the model, and the posteriors each have exactly one implementation.

**Offline, end state (goal, §7.5, not yet built)**: `raw ticks → EventDataCollectorStudy.cpp (in-process
mts_observation_vector + mts_hmm) → .context.parquet (full raw vector, canonical) + .alpha/TrainingEvent
(labels + HMM posteriors + join key back to .context.parquet) → Transformer training`. The two-phase
"collect, then separately run the HMM over historical data to fill in posteriors" workflow collapses
into one pass for newly-collected data; the separate pass remains available, intentionally, for
re-scoring already-collected data against a newer/retrained model.

### 3.3 Crate status

| Crate | Status | Notes |
|---|---|---|
| `mts_ffi` | Skeleton shipped, 2026-10-08 | One staticlib, features `hmm`/`observation_vector`/`transport` turn subsystems on |
| `mts_observation_vector` | 2 of ~15 dims ported | §8 |
| `mts_schema` | Scaffolded 2026-10-09 | Generates from the now-single `mts_schema.fbs`; see §6.2 for the merge that simplified this |
| `mts_hmm` | Not started | §7 |
| `mts_transport` | Not started | Deliberately last; §9 |
| `mts_py` (`mindful_core`) | Not started | PyO3 extension for `lbrnet`/`GUI` |

**Principles governing every crate** (established 2026-10-08, proven in practice since):
- Pure crates (`mts_schema`, `mts_observation_vector`, eventually `mts_hmm`) carry no `pyo3`/`zmq`/Sierra
  code — only `std` and math crates. They build and test natively on Linux in seconds; every golden
  test lives there.
- I/O and language edges are separate crates: `mts_transport` (zmq), `mts_ffi` (C-ABI), `mts_py` (PyO3).
- One `mts_ffi` staticlib, not one per subsystem (linking two Rust staticlibs into one DLL gives
  duplicate-symbol errors — each bundles its own copy of `std`/`core`). Same for `mindful_core`: one
  PyO3 extension with submodules, not one per subsystem.
- One version of everything that crosses a language boundary (FlatBuffers: `flatc` = vendored C++
  headers = Rust `flatbuffers` crate = Python `flatbuffers` runtime — today these are genuinely
  different versions in places; P2 fixes this).
- Generated code is committed; CI (once it exists) proves it is fresh.
- Paths are derived from the script's own location, never hardcoded absolute paths.
- Each Python project keeps its own env and lock (P6).

---

## 4. Execution plan

### 4.1 Phase 1 — Pre-work (per project, independent, no merge required)

Rule for every item: it must leave its project better off even if nothing else here ever happens, and
it changes no wire format. This system is not in production, so dead code found along the way is
deleted, not shimmed.

| # | Item | Project | Status | Why it helps |
|---|---|---|---|---|
| P1 | Root-relative paths everywhere (no `/home/rcruz/devel/VSCode/...` in scripts) | all four | Open | Removes most of the ~690 absolute-path rewrites the merge would otherwise need |
| P2 | One FlatBuffers version (C++ headers 25.1.24, system `flatc` 24.3.25 — known mismatch) | schema | Open | A third language (Rust) cannot be added on an unsettled version |
| P3 | `flatc --rust` in `regenerate_schema.sh` | schema | **Done**, 2026-10-09 | Same script, one more target (§6.1) |
| P4 | C++ transport seam: one interface behind today's scattered `zmq::socket_t` members | cpp | Open | §9 then swaps an implementation instead of editing many files |
| P5 | Python transport seam: one module per project importing `zmq`; resolve the duplicate HMM ROUTER bind (both `lbrnet/backtest/backtest_server.py:604` and `MTS/zmq_client.py:323` bind 5561 today) | lbrnet, GUI | Open | |
| P6 | Split the shared `mts` conda env into per-project envs with lock files | lbrnet, GUI | Open | Shared env is the one thing the merged repo must not inherit |
| P7 | Settle uncommitted work: lbrnet 86 files, MindfulTrader (settled), MTS (settled), schema (settled) | all | **Partially done** — lbrnet's 86 files remain, deliberately deferred, separate owner | A merge over dirty trees hides changes |
| P8 | C++ observation seam + goldens: make every dim a pure function of plain inputs, export goldens | cpp | **Mostly already true** — see §8.2's trace | The goldens are the Rust acceptance tests |
| P9 | HMM golden harness: pickle → neutral artifact exporter, full-field golden from real `RegimeEngine` output | lbrnet | Open | The Rust HMM is proven against it before Python HMM code is deleted |

### 4.2 Phase 2 — The merge (deferred, own schedule, not gated by Rust progress)

**Decoupling rule (2026-10-08)**: the repo merge and the Rust work are independent. Rust work proceeds
today, per-repo, via cross-repo relative Cargo paths; the merge happens later, motivated by eliminating
doc-duplication-drift, not by blocking Rust progress on it.

**Approach** (preserves the 479-commit `MindfulTrader` history without a force-push, by making it the
base): one commit moves everything into `cpp/` (`git mv`); `schema`/`lbrnet`/`MTS` are merged in via
throwaway clones + `git filter-repo --to-subdirectory-filter` + `git merge --allow-unrelated-histories`
(originals never rewritten); root files (`PRODUCTION_TRIAGE.md`, `docs/`, `scripts/`, `.claude/`) land
in a separate commit; every pre-merge HEAD gets tagged (`premerge/<repo>`) and pushed before archiving.

**Stages and gates** (do not start the next stage on a red gate):
- **Stage 0 — Preconditions**: run Phase 1's P1-P9; settle uncommitted work; record baselines
  (`./build_dll.sh`, native tool checks, `pytest`, `validate_schema.sh`); full `.git` backup.
- **Stage 1 — Version the root files** (`PRODUCTION_TRIAGE.md`, `docs/`, `scripts/`, `.claude/` under
  git for the first time). Gate: byte-identical to originals.
- **Stage 2 — Merge in a scratch location** (`/home/rcruz/devel/_mt_merge/`, outside `VSCode/`). Gate:
  full history preserved per project; file-tree union matches.
- **Stage 3 — Path rewrite** in the scratch repo (script first, then hand-review executables:
  `regenerate_schema.sh`, `build_dll.sh`, CMake presets, `check_north_star.sh`, `.vscode/`, CI). Gate:
  schema regen round-trips with no diff beyond paths; no old absolute paths remain outside historical specs.
- **Stage 4 — Build and test** in the scratch repo. Gate: matches Stage 0 baselines.
- **Stage 5 — Cut over** (swap directories on disk; re-point the `lbrnet` editable install; update the
  sync hook, CI, agent project settings). Gate: Stage 4 suite green from the real paths.
- **Stage 6 — Remote**: push to `felocruz/MindfulTrader` (fast-forward, no force-push); archive
  `schema`/`lbrnet`/`MTS` on GitHub after pushing `premerge/*` tags.
- **Rollback**: before Stage 5, abandon the scratch repo (nothing in the real tree changed). After
  Stage 5, restore from the Stage 0 bundles and move directories back.

**Known breakage to fix in Stage 3** (measured 2026-10-07): absolute-path references —
schema 33, MindfulTrader→cpp 325, lbrnet 308, MTS→GUI 27; old-name-in-path references — schema 6,
MindfulTrader→cpp 269, lbrnet 75, MTS→GUI 16. Also: `mts`/`mts-cpu-backup` conda envs hold `lbrnet`
as an editable install pointing at the old path; `.vscode/`/`.code-workspace` files; the four-mirror-doc
pre-commit hook's real source needs locating before moving it; Windows-side paths not yet checked.

### 4.3 Phase 3 — Rust workstreams and dependencies

| ID | Workstream | Depends on | Status |
|---|---|---|---|
| W0s | Rust-in-Sierra integration spike | — | **Done** (§5) |
| W1 | Merge into `VSCode/MindfulTrader/` | W0 pre-work | Deferred (§4.2) |
| W2 | `rust/` workspace bootstrap (toolchain, cbindgen, DLL link, CI) | P2/P3, W0s | **Mostly done** — `rust-toolchain.toml`, workspace, `mts_ffi` skeleton all exist and are proven; not yet done: `install_py_ext.sh`, `check_all.sh`, CI |
| W3 | `mts_hmm` inference + regime engine | W2, P9 | Not started |
| W4 | Offline posteriors tool | W3 | Not started |
| W5 | `mts_observation_vector` port, dim by dim | W2, P8 | **In progress** — 2 of ~15 dims done |
| W6 | In-process shadow mode in a full `BackTesterStudy` replay | W3, W5 | Not started |
| W7 | Event-schema cutover: HMM output rides the event; Transformer reads it | W6 | Not started |
| W8 | Rust training | W6 | Not started |
| W9 | `mts_transport`, port by port | W7 (5561 gone), W2 | Not started; deliberately last |
| W10 | Deletions + mirror-doc updates + `PRODUCTION_TRIAGE.md` rows | W6-W9 as each lands | Ongoing as work lands |
| W11 | `ContextManager.cpp` port | W5, W6 | Not started; later, own spec when it comes up |

**Critical path**: `P2/P3 + W0s → W2 → W3 ∥ W5 → W6 → W7 → W9 → W10`, with W4 and W8 branching off
W3/W6. Rust-free work that can start any time: P1, P2, P6, P7, P8, P9.

**Sequencing rationale**: Phase 1 items are independent and valuable even if Phase 3 never happens; the
merge makes Phase 3 land in one repo with one schema (but does not block it, per the decoupling rule);
within Phase 3, `hmm` and `obs` form one chain (`dims → scaler → gate → HMM step`), and removing port
5561 (via `hmm`) deletes the hardest port `transport` would otherwise face — so `transport` goes last.

---

## 5. W0s — Rust-in-Sierra integration spike (closed out, 2026-10-08)

**Idea** (operator, 2026-10-07): add a Rust function, call it from `scsf_EventDataCollector` in
`EventDataCollectorStudy.cpp`, deploy, confirm the DLL loads. Testing inside the real
`MindfulTrader.dll` (not a toy DLL) was the point — it exposes link-level conflicts a trivial DLL would
hide. **Kill criterion** (operator): if the DLL does not load in Sierra Chart, or cannot survive
unload/reload, Rust is scrapped from MindfulTrader entirely.

**Final status, all 9 checks accounted for:**

| # | Check | Result |
|---|---|---|
| 1 | Links cleanly with the real DLL | **PASS** |
| 2 | Imports only system/VC-runtime DLLs (+ existing `libzmq-mt`) | PASS (static check; not reverified on the second spike) |
| 3 | **Sierra loads the DLL; the study runs** — the kill criterion | **PASS, twice independently**, with real evidence (checksum match + magic/version round-trip; `scsf_EventDataCollector` processed 40,000+ real ticks with no crash) |
| 4 | Round-trip proves genuine execution, not just linkage | **PASS** — `tick(input) = input*31 + call_count`, verified against real log lines independently, not reproducible by a stub |
| 5 | Background thread start/stop/join at `LastCallToFunction` | **PASS** |
| 6 | **Unload/reload survives** | **Reasoned through, not empirically run** — resolved 2026-10-08 (see below) |
| 7 | Forced panic caught at the FFI boundary | **PASS** |
| 8 | Call overhead (ns/call) | **PASS — 0.70 ns/call measured** |
| 9 | Vendored `zmq` crate doesn't conflict with the DLL's existing vcpkg libzmq | Not attempted; revisit when `mts_transport` actually needs zmq |

**Check 6 resolution (2026-10-08)**: not run empirically, but walked through mechanism-by-mechanism with
no applicable known failure mode found: (a) this codebase already proves the explicit-shutdown-via-
`sc.LastCallToFunction` pattern works for long-lived ZMQ sockets/threads (`TransportStream`, `HMMClient`,
`SystemOrchestrator`, `AIHeartbeatMonitor`, `TradeExecutionServer` — every `SCSFExport` entry point
already has its own `LastCallToFunction` handler); (b) the Windows loader-lock/`DllMain` deadlock risk
doesn't apply because shutdown happens on Sierra's normal thread, never in `DllMain`; (c) Rust's
historical unwind-across-FFI UB (pre-1.71) doesn't apply because every entry point already calls
`catch_unwind` before returning to C++ (check 7 proved this); (d) the crate is a `staticlib`, not its
own DLL, so its statics are just part of `MindfulTrader.dll`'s own image — no separate Rust-DLL
lifecycle exists to go wrong. **Conclusion: Rust is cleared for MindfulTrader.** Re-run check 6
empirically only if a future crate's design changes one of these premises (e.g. a thread that isn't
explicitly joined).

**Two independent spike runs**: an offline/static check (Stage 1, a clean worktree, a separately-built
DLL, `/W4 /WX` clean, imports verified via `llvm-objdump-22 -p`), and a live Sierra run (a second,
independent spike, methodology differed — linked directly into the real build and deployed over the
live `Data/MindfulTrader.dll` rather than a separately-named spike DLL; worked out safely — clean
revert, Sim-mode-only chart, DLL restored byte-identical afterward — but carried more risk than
necessary; future spikes should use the safer worktree + separate-DLL-name + separate-log method).
Cleanup performed both times: all touched files reverted (`git checkout`), DLL rebuilt clean and
redeployed, test artifacts deleted.

---

## 6. Component spec: Schema

### 6.1 Generation mechanics

`schema/regenerate_schema.sh` generates C++, Python, and (as of 2026-10-09) Rust bindings from a single
source: `schema/mts_schema.fbs`. One invocation per language now (previously two for C++/Python/Rust
each, due to the now-removed `backtest_schema.fbs` split — see §6.2). `flatc` must match the vendored
C++ runtime headers' version exactly (`include/flatbuffers/base.h`) or C++ generation emits a downgraded
`static_assert` that breaks the build — the script refuses to run on a mismatch. Deploys to
`MindfulTrader/include/generated/` (C++), `lbrnet/lbrnet/generated/` (Python, shared with `MTS`'s
imports), and `MindfulTrader/rust/schema/src/generated/` (Rust) — all three paths already cross-repo,
no merge required for this to work.

**`mts_schema` crate specifics**: `#[path = "generated/mts_schema_generated.rs"]` with an `#[allow(...)]`
wrapper wider than the generic lint-suppressing pattern — this workspace additionally denies
`unsafe_op_in_unsafe_fn` and `clippy::unwrap_used` (neither is in the `clippy::all` group), and flatc's
generated code trips both; both must stay in the allow-list or `cargo build`/`clippy` hard-fail.

### 6.2 Naming & structural coherence (the 2026-10-09 audit's findings)

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
   namespace it's a client of — is not a fresh decision: it is already **ruled** in §4.2 (`MTS`→`GUI/`).
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
   **Open**, likely low priority — purely internal to one script, not a cross-language naming contract.
5. **Already-resolved precedents, recorded so they are not re-litigated**: `mt_`/`MT_` → `mts_` (caught
   because it collided with vcpkg's own `-mt` suffix convention and MetaTrader 4/5); `observation` (bare)
   → `observation_vector` (no standalone meaning in this codebase — confirmed by finding that
   `ObservationData` is a real, distinct, already-taken schema struct name, which also means
   `observation_data` would have been the wrong rename target).

**Open follow-up, not yet done**: extending this audit to `lbrnet`'s and `MTS`'s own internal naming
conventions (C++ class names, Python module names) beyond what touches the schema.

### 6.3 DOD / performance findings (struct vs. table)

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

**Status: proposed, not yet implemented.** Needs the same verification discipline as §6.2's fixes
(generate, build, test) before executing.

---

## 7. Component spec: HMM lifecycle (train → posteriors → live inference)

### 7.1 What exists today (verified 2026-10-07)

**Live path** (per HMM step, fired by the Mahalanobis significant-change gate in C++):
`HMMClient::RequestUpdateAsync` → DEALER → Python ROUTER (`lbrnet/backtest/backtest_server.py:598`;
`MTS/zmq_client.py:323` binds a second one — a real duplicate-bind bug, P5) → `FeatureSpine` pairs
`MARKET_OBSERVATION` with `SYSTEM_STATE` by sequence id → burn-in → `LiveAgent.predict_hmm` →
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
says its premise is broken); `cognitive_load`/latency (wall-clock-dependent, non-deterministic — §7.6-2).

**Offline path, confirmed two-phase (2026-10-09, directly relevant to §7.5's goal)**:
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
background for whoever eventually Rust-ports `ContextManager` (§7.2) — it is the real shape of the
C++-side API that port must replicate, not a single `Update(ctx)` entry point. Note: the two deleted
docs' own "(16D)" dimensionality claim was stale even before deletion — current is 18D (§8). The
authoritative, still-current `ContextManager` architecture doc is the workspace-shared
`/home/rcruz/devel/VSCode/docs/ROADMAP_CONTEXTMANAGER_REFACTOR.md` (not a MindfulTrader-repo-local
file) — it already covers this ownership split in more depth but doesn't name the
`CheckAndTriggerHMM`/`AddToTrainingEventFB` call sites this paragraph adds.

### 7.2 Target architecture

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

### 7.3 Parity strategy

**Inference** must match Python bit-for-bit-ish (golden file, stated tolerance, real observations)
before any Python HMM code is deleted. **Training** cannot be bit-exact (EM is non-convex); its gate is
statistical — same `.context`, same init, held-out log-likelihood/BIC, state profiles, and posterior
agreement within tolerance.

### 7.4 Stages and gates (outline — full per-stage detail to be filled in when work starts)

Stage A (golden harness, Python-only, P9) → Stage B-C (Rust inference + regime engine, matched against
the golden) → Stage D (offline posteriors tool) → Stage E (in-process shadow mode in a full
`BackTesterStudy` replay, zero divergence gate) → Stage F (event-schema cutover) → Stage G (Rust
training) → Stage H (deletions: Python HMM code, port 5561, mirror-doc updates).

### 7.5 Extension (2026-10-09, operator goal — not yet decided in full, work to begin after Stage B-C)

**The goal, stated plainly**: `EventDataCollectorStudy.cpp` writes `.context.parquet` directly during
collection (replacing today's two-step "C++ writes `.context` as a FlatBuffers stream → a separate,
later Python pass materializes it to Parquet" — this repo already writes Parquet directly from native
code elsewhere, `tools/scid_processing/scid_to_ticks_parquet.cpp`, so there is real precedent).
`.alpha`/`TrainingEvent` keeps exactly "whatever the Transformer needs from the HMM" (the already-
existing `regime_prob_*`/`regime_confidence`/`regime_entropy` fields — not new fields) plus labels plus
a join key (`sequence_id`), and drops its own embedded `observation: MTS.Schema.ObservationData` copy,
joining back to `.context.parquet` instead when the training pipeline needs the raw vector alongside
labels. This mirrors a pattern the schema already uses elsewhere (`BacktestFrame`'s own `run_id` field
is documented as "redundant with the payload's own run_id; enables random-access correlation... without
deserializing the union" — carry a join key, don't duplicate the payload).

**The operational win on top of the architectural one**: §7.1 already confirms today's offline pipeline
is two-phase. Once the HMM is in-process at collection time (§7.2), this collapses to one pass:
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
on §7.2's in-process Rust HMM landing first — writing `.alpha` records "fully populated the first time"
requires the HMM to already be callable in-process from `EventDataCollectorStudy.cpp`, which doesn't
exist yet. The GUI question (consumer 3) should be resolved before any schema change removes
`MarketObservation` from the live wire protocol, independent of this goal's `.alpha`/`.context.parquet`
changes, which don't depend on that resolution either way.

### 7.6 Open questions

1. "Older-sequence" in the coherence rule (§2 item 5) — confirm it means older than the regime-epoch
   start, not older than every newer sequence (generic ageing is already handled by
   `SemanticFreshnessDiscount`).
2. `cognitive_load`: deterministic replacement, or drop (wall-clock-derived today).
3. Entropy normalization function and the unconditional-state set — not read in full yet; Stage A's
   golden will pin them.
4. `HMM_MODEL_INPUT_DIM` value and `HMM_BURN_IN` — read from code in Stage A.
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

## 8. Component spec: observation vector pipeline

### 8.1 Current status and candidates

**Ported, shadow-wired, live-confirmed**: `SevcikFractalDimension`, `ComputeBipowerVariation` (§0).

**Ported, shadow-wired, build-verified (OFF byte-identical, ON links+grows), not yet live-confirmed
in Sierra Chart, 2026-10-09**: `DfaHurstExponent.h` (`hurst_exponent` — this system's single worst
HMM cross-state discriminator). 16/16 cross-language parity assertions pass (4 new); 12/12 Rust unit
tests pass (3 new). `MeanReversionCalculator.h` (`mean_rev_z`) — 21/21 cross-language parity
assertions pass (5 new, incl. flat-window and null-input carry-forward); 17/17 Rust unit tests pass
(5 new). `LiquidityFragilityEngine.h` (`liq_fragility`) — 26/26 cross-language parity assertions
pass (5 new, incl. thin-volume and null-input carry-forward); 22/22 Rust unit tests pass (5 new).

**None remaining self-contained** (no `FeatureScaler`/dim-index coupling) — all five such dims are
now ported (§0 lists all five). Every remaining dim is blocked on P2/P3 (below).

**Deferred until P2/P3 settle** (one FlatBuffers version lands, dim indices need to come from the
schema, not literals — the exact bug class that has bitten this repo twice already): `EventVelocityEngine.h`
(`burstiness_index`), `CarryForwardCalculators.h`-coupled dims.

**Methodology, proven five times**: port the function → Rust unit tests mirroring the C++ test's exact
golden values → `mts_ffi` FFI wrapper (`(ptr, len)` shape, panic-guarded) → cross-language parity test
(same deterministic-LCG input into both the C++ original and the Rust wrapper, exact equality) → wire
into the real C++ call site in shadow mode (compare-and-log only, never used, logged once per process
not every tick) → verify `MTS_WITH_RUST=OFF` build byte-identical, `=ON` build grows appropriately →
deploy to Sierra Chart for live confirmation → only then consider cutover (not done yet for either
port landed so far).

### 8.2 Dim-tracing reference (read-only trace, 2026-10-07 — where the Sierra coupling actually is)

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

### 8.3 Port-by-port approach

1. **Boundary first (C++, no Rust)**: a plain input view (`BarSeries`: spans of close/high/low/volume/
   ask/bid/trades, current index, tick size) and per-dim state structs replacing `sc.GetPersistent*`
   (the carry-forward the offline engine already has in `CarryForwardCalculators.h`). Each wrapper
   becomes a pure function of `(BarSeries, state, window)`.
2. **Language-neutral goldens**: export existing fixtures + a whole-pipeline golden (real tick slice in,
   18D vector + every gate decision out, from the C++ replay engine) — these become the Rust acceptance
   tests.
3. **`mts_observation_vector` crate**, ported dim by dim in order of stability (dims still in flux last).
   Gate per dim: fixtures + the pipeline golden, tolerances stated up front.
4. **Rust replaces the offline generator**: gate = reproduce the existing `.context` over the full real
   tick set (335,147 pairs, zero mismatches). `tools/market_data_replay/` is then deleted.
5. **In-process in the DLL, shadow mode** against C++'s `ContextManager` through a full
   `BackTesterStudy` replay (same pattern as §7.4 Stage E); then authoritative.
6. **`ContextManager.cpp` later** — state assembly, `TrainingEvent` construction; its own spec when it
   comes up.

---

## 9. Component spec: transport (ZMQ → Rust)

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
| 5561 | DEALER→ROUTER | connects (`HMMClient`) | binds ROUTER (×2 today — a real duplicate-bind bug, P5) | **Deleted** by §7's in-process HMM, not migrated |

**The one real design problem**: today's request/reply sockets (HMM inference, trade RPC, control
handshake) are synchronous on the Sierra thread. Across a C-ABI this must become non-blocking
`submit(request) -> ticket` / `poll(ticket) -> Option<bytes>`, with existing timeouts enforced inside
Rust, so the DLL never blocks inside Sierra's update call.

**Order, with a gate per port** (flip one port; the peer is unchanged; revert = flip back): (1)
`mts_transport`/`mts_ffi` skeleton — gate: DLL builds, imports unchanged, Sierra loads it; (2) 5555 PUB
— gate: GUI/lbrnet SUB receive identical bytes, ring never blocks the Sierra thread; (3) heartbeat SUB —
gate: stale-heartbeat detection fires at the same threshold; (4) Python side (PyO3 client replaces
`transport.py`/`zmq_client.py` internals) — gate: existing Python tests green; (5) 5560/5562/5558,
one at a time, `submit`/`poll` shape — gate per port: handshake + round-trip against the unchanged peer,
full `.btst`-matching replay for 5558; (6) 5561 — not migrated, removed by §7; (7) remove the vcpkg
libzmq/libsodium link, delete the old socket code (not behind a flag — not in production).

**Non-goals**: no schema change, no protocol redesign, no change to port numbers or message ordering.

---

## 10. Component spec: execution-layer computations

**Scope confirmed 2026-10-09 (operator)**: this initiative is "all Rust," not "HMM plus
observation_vector math." `RiskManager`/`PositionManager`/`ExecutionGate`'s own low-level computations
are explicit, equal-footing candidates for the same methodical port.

**Candidates surveyed** (all pure, zero Sierra Chart coupling, confirmed by direct code read):
- `Scoring.h`, `LocalRiskContext.h` (the real live risk struct, §6.3), `TailRiskEngine.h` — same shape
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
remaining dims. Which crate(s) these land in (a new `rust/risk` sibling vs. folded into `mts_hmm`) also
not yet decided.

---

## 11. Infrastructure mechanics

### 11.1 Files and their shape

```
rust-toolchain.toml          repo ROOT, not rust/ — pinned version, components, target
rust/.cargo/config.toml      target-dir = "target"
rust/Cargo.toml              [workspace], [workspace.package], [workspace.dependencies] (every
                              third-party version lives here ONCE — Atratus repeats per crate,
                              a drift risk this repo deliberately avoids), [workspace.lints]
rust/<crate>/Cargo.toml      tiny; pure crates depend only on workspace deps + mts_schema
rust/ffi/Cargo.toml          crate-type = ["staticlib", "rlib"] (rlib so tests run natively on Linux);
                              features hmm/observation_vector/transport gate optional deps
rust/ffi/cbindgen.toml       generates include/generated/mts_core.h
rust/py/pyproject.toml       maturin; cdylib; module name "mindful_core"
```

### 11.2 CMake integration

`cpp/CMakeLists.txt` imports the Rust staticlib (add, don't restructure); a `MTS_WITH_RUST` option
(default OFF) and a matching CMake preset (`wsl-clang-cl-release-rust`, separate build directory) keep
the normal build path completely unaffected when Rust isn't enabled. `-ffast-math` policy: not yet
settled — avoid it wherever goldens compare values across languages (§2 item 11).

### 11.3 Scripts (repo root `scripts/`, every one derives `ROOT` from its own path)

- **`rust_windows_env.sh`** (sourced) sets the cc-rs cross-compile env vars for
  `x86_64-pc-windows-msvc` (clang-cl-22, the xwin sysroot, `/winsysroot` — chosen because the sysroot
  path has no spaces, unlike "Windows Kits", which cc-rs would split).
- **`build_rust.sh [--features ...]`** builds `mts_ffi` for the Windows target, then flags if the
  generated cbindgen header changed (so it gets committed). `cpp/build_dll.sh` calls this first when
  `MTS_WITH_RUST=ON`, then its existing CMake/Ninja steps, then checks the DLL's imports via
  `llvm-objdump-22 -p` (the W0s check 2 pattern, now permanent).
- **`install_py_ext.sh`** (not yet built) — builds the `mindful_core` wheel via `maturin`, installs with
  `pip install --no-deps --force-reinstall` into each Python env. **Never `cp` a rebuilt `.so` over a
  loaded one** — Atratus's live GUI segfaulted this way once; `pip install` unlinks and writes a new
  inode instead, which a running process's existing mapping survives.
- **`check_all.sh`** (not yet built) — the commit gate, steps chained with `&&` never `;` (Atratus once
  committed a red tree because its checks were chained with `;`): `cargo fmt --check` → `cargo clippy
  --workspace --all-targets -D warnings` → `cargo test --workspace` → schema regen + `git diff
  --exit-code` → `ctest` → lbrnet `ruff`+`mypy`+`pytest -m "not slow"` → GUI `ruff`+`mypy`+`pytest`.

### 11.4 CI (not yet built)

One workflow, path-filtered jobs, modelled on Atratus's `ci.yml` (which today gates Python only — a gap
to do better than, since Rust becomes a build prerequisite for `cpp/` the moment `mts_ffi` is wired in).

### 11.5 Cross-cutting runtime rules (bake into every code review of Rust work)

- Every `extern "C"` function is `catch_unwind`-guarded and returns a status.
- `mts_init`/`mts_shutdown` are explicit, driven by `sc.LastCallToFunction`. Nothing relies on
  destructors at unload; no `static` holds a thread or socket past shutdown.
- Unload/reload (§5 check 6) must pass — reasoned through, cleared — before any long-lived thread or
  socket ships in `mts_ffi`.
- One FFI call per tick or bar-close, not per dim. No allocation in any `*_step` function.
- Python calls release the GIL around every blocking or long call; NumPy arrays cross zero-copy
  (`rust-numpy`), never Python lists in hot loops.
- **libzmq, one copy per process image**: during the transition, `mts_transport` links the same dynamic
  vcpkg libzmq the DLL already uses (no vendoring) — read the exact env-var names from the zmq-sys
  version actually locked, they change between releases. After `mts_transport` owns every socket, switch
  to a vendored build (as Atratus does) and drop the vcpkg link entirely.
- f64 inside the model, f32 on the wire (matching the Python HMM today). Tolerances are stated in the
  test before the comparison runs. Goldens are language-neutral files under `rust/<crate>/tests/goldens/`
  — small ones committed, big ones referenced by sha256 + path and skipped (not failed) when absent.

### 11.6 From Atratus — copy this, not that

| Copy (proven) | Do differently / better |
|---|---|
| Pure core crates separate from pyo3/zmq crates | **Pin the toolchain** — Atratus has no `rust-toolchain.toml` |
| xwin + clang-cl-22 + `/winsysroot` cc-rs variables; `/NODEFAULTLIB:libcmt.lib`; `--print native-static-libs` | **`[workspace.dependencies]`** — Atratus repeats versions per crate, a drift risk |
| cbindgen-generated header from `build.rs` | **One FlatBuffers version** — Atratus's crate is 24.3.25 against a 25.12.19 `flatc` |
| `catch_unwind` + explicit init/shutdown | **Scripted wheel install**, never a `.so` copy — Atratus's live GUI segfaulted once from exactly this |
| `[patch.crates-io]` with a vendored, commented crate when upstream needs a fix | **Rust in CI** — Atratus's CI is Python-only |
| Per-project env + lock; CI from the lock | **Small crates** — `atratus_rust` pulls polars + tract-onnx + plotters into one cdylib (slow builds); add a heavy dependency only to the crate that needs it |
| Commit gate: checks chained with `&&` | **Prove it in Sierra first** — Atratus's Rust DLL was never loaded in Sierra; here W0s already passed (§5) |
| Python-free hot path: Python only displays/forwards | `.pyi` stubs for the extension (mypy sees `Any` in Atratus without them) |
| `flatc --rust`'s atomic scratch-dir generation pattern (generate into `.work_gen/`, copy on success) | **Crate placement** — Atratus dumps generated Rust into its one do-everything `sensor_core`; this repo correctly uses a dedicated `mts_schema` crate instead, so `mts_hmm`/`mts_observation_vector`/`mts_transport` each depend on just the schema, not transitively on each other |
| `--rust-module-root-file` for multi-schema generation | **Not applicable anymore** — this repo merged its two schemas into one (§6.2), which Atratus never needed to since it only ever had one |

---

## 12. Risks

1. **The central Rust-in-Sierra assumption was unproven until W0s** — now resolved (§5).
2. **Moving targets**: the observation-vector dims and the HMM (K, features, the unmet fat-tail
   sign-off) are still changing; ports must be manifest/golden-driven so a change lands in one place.
3. **Unvalidated references**: the offline reconstruction has never been byte-validated against a
   genuine Sierra-collected file at the current schema version (blocked on a v240 comparison file not
   existing yet) — Rust ports will match the C++ reference, not necessarily live Sierra, until that
   exists.
4. **Two libzmq copies in one DLL** (expected during the transport transition, untested) — §11.5 has
   the interim plan; resolve before `mts_transport`'s W9.
5. **Numeric policy** (possible `-ffast-math` in the DLL build vs. none in tools/Rust) — unsettled, §2
   item 11.
6. **A long program touching the same files repeatedly** (`ContextManager`, `HMMClient`,
   `SystemOrchestrator`, the schema, the event stream) — mitigation: every stage reversible until its
   own deletion step, shadow modes throughout, one change at a time.
7. **This is re-platforming, not edge** — see §0's standing caution. Do not let this program's real
   progress displace the actual production-readiness gaps it does not itself close.
8. **Sierra-process crash risk from Rust**: a panic or allocation on the hot path is a Sierra crash, not
   a log line — `catch_unwind` everywhere, preallocated buffers, an allocation-tripwire test around every
   `*_step` function.

---

## 13. Not yet analyzed (real gaps in this picture, not silently skipped)

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
  scripted build (§11.3).
- **Extending the naming/coherence audit (§6.2) beyond the schema surface** to `lbrnet`'s/`MTS`'s own
  internal conventions.

---

## 14. Documentation sync, when pieces of this land

- "Rust is a build prerequisite for `cpp/`" → the four mirror docs, once `mts_ffi` is load-bearing for
  a real build (not yet — currently opt-in via `MTS_WITH_RUST`).
- The artifact convention change (`models/hmm_model.pkl` → manifest + weights) → the four mirror docs,
  once §7 ships.
- A `PRODUCTION_TRIAGE.md` row for this whole program, added when work actually starts in earnest
  (Triage Protocol rule 7) — not yet added; this is still pre-work/early-port territory.
