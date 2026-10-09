# MindfulTrader → Rust / Monorepo Migration — Implementation Plan

**Spec**: `docs/superpowers/specs/2026-10-09-mindfultrader-rust-migration-spec.md` (vision, decisions,
target architecture, per-component design — argue from there, not from here). This plan carries status,
execution phases/stages/gates, infrastructure mechanics, and port-by-port methodology — the "what's
done and in what order", not the "what and why". **Read §0 first, every session.**

**Goal**: get `MindfulTrader`/`lbrnet`/`MTS`/`schema` to the point where one pure Rust core (behind a
C-ABI) owns every computation that today has separate train/offline/live implementations, without
breaking any existing behavior along the way — port, parity-test, shadow-wire, live-confirm, cutover,
repeated per unit of work, never a big-bang rewrite.

**Architecture**: independently-committable workstreams in dependency order (§1.3's table), each
producing working, natively-testable Rust (plus a C++ shadow wire-up where it has a live call site) on
its own. No task blocks another except where the dependency table says so.

**Tech Stack**: Rust (pinned via `rust-toolchain.toml`), `cbindgen` for the C-ABI header, native
`cargo test`, cross-compiled to `x86_64-pc-windows-msvc` via clang-cl/xwin for the Sierra DLL, FlatBuffers
for every payload (schema changes route through `schema/regenerate_schema.sh` only).

**When this plan's content changes, it is the only place that needs updating; do not open a new sibling
plan for a sub-topic of this initiative — add a section here (or to the spec, if it's architecture/
design content) instead.**

**Last updated**: 2026-10-09.

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
  Full results: §2.
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
  were self-contained (no `FeatureScaler`/dim-index coupling) per §5.1 are now ported — the next
  observation-vector dims all require P2/P3 (schema-driven dim indices) first.
- **`rust/schema` (`mts_schema`) crate scaffolded, 2026-10-09**: generates from `schema/mts_schema.fbs`
  via a new `flatc --rust` target in `regenerate_schema.sh` (schema commit `2e9528d`). Builds, clippies,
  and tests clean.
- **Schema coherence fixes, 2026-10-09**: `MTS_Envelope` → `Envelope` renamed (schema `79ae486`,
  MindfulTrader `9f899f6`, lbrnet `6b131df`, MTS `457a6a0` — 151 occurrences across 13 hand-written
  files in three repos). `backtest_schema.fbs` merged into `mts_schema.fbs` entirely (schema `fe278f2`,
  MindfulTrader `f95281c`, lbrnet `86e4356`) — one schema file now, not two; Rust generation simplified
  from a 77-file `--rust-module-root-file` tree back to one clean file. Full details: spec §4.2.
- **Naming hygiene fixed earlier this session**: `mt_`/`MT_` prefix → `mts_` (collided with vcpkg's
  own `-mt` suffix convention and MetaTrader 4/5); `observation` (bare) → `observation_vector` (no
  standalone meaning in this codebase; confirmed against the schema's own distinct `ObservationData`).
- **This plan was briefly combined with its spec into one "master spec & plan" document (2026-10-09,
  same day) before being split back into this spec/plan pair** per the repo's own established
  convention — a misunderstanding of what was asked for, corrected the same day with zero content
  lost; see the spec's own header for the full note.

**Not yet started (the next real pieces of work, in rough order):**
1. `rust/hmm` (`mts_hmm`): the Student-t HMM lifecycle (train → posteriors → live inference) — spec §5.
   This is the big one; most of the rest of this initiative (the `.context.parquet`/`.alpha` goal in
   spec §5.4, the execution-layer "all Rust" scope in spec §8) explicitly depends on it landing first.
2. Execution-layer computations (`RiskManager`/`PositionManager`/`ExecutionGate`) — confirmed in scope
   2026-10-09, not yet sequenced in detail. Spec §8.
3. `rust/transport` — deliberately last (removing ZMQ port 5561 via the HMM port removes the hardest
   socket `transport` would otherwise have to handle).

**Deferred, own schedule, explicitly decoupled from the Rust work:**
- The four-repo → one-repo merge (§1.2) — the Rust work does **not** wait on this; it proceeds per-repo
  today via cross-repo relative Cargo paths.
- `lbrnet`'s 86-file uncommitted backlog (P7) — separate owner/session.
- The hardcoded Gemini API key in `MindfulTrader/config.py:22` / `MTS/config.py:21` — operator's call
  on timing, not urgent per the operator (2026-10-08).

---

## 1. Execution plan

### 1.1 Phase 1 — Pre-work (per project, independent, no merge required)

Rule for every item: it must leave its project better off even if nothing else here ever happens, and
it changes no wire format. This system is not in production, so dead code found along the way is
deleted, not shimmed.

| # | Item | Project | Status | Why it helps |
|---|---|---|---|---|
| P1 | Root-relative paths everywhere (no `/home/rcruz/devel/VSCode/...` in scripts) | all four | Open | Removes most of the ~690 absolute-path rewrites the merge would otherwise need |
| P2 | One FlatBuffers version (C++ headers 25.1.24, system `flatc` 24.3.25 — known mismatch) | schema | Open | A third language (Rust) cannot be added on an unsettled version |
| P3 | `flatc --rust` in `regenerate_schema.sh` | schema | **Done**, 2026-10-09 | Same script, one more target (spec §4.1) |
| P4 | C++ transport seam: one interface behind today's scattered `zmq::socket_t` members | cpp | Open | Spec §7 then swaps an implementation instead of editing many files |
| P5 | Python transport seam: one module per project importing `zmq`; resolve the duplicate HMM ROUTER bind (both `lbrnet/backtest/backtest_server.py:604` and `MTS/zmq_client.py:323` bind 5561 today) | lbrnet, GUI | Open | |
| P6 | Split the shared `mts` conda env into per-project envs with lock files | lbrnet, GUI | Open | Shared env is the one thing the merged repo must not inherit |
| P7 | Settle uncommitted work: lbrnet 86 files, MindfulTrader (settled), MTS (settled), schema (settled) | all | **Partially done** — lbrnet's 86 files remain, deliberately deferred, separate owner | A merge over dirty trees hides changes |
| P8 | C++ observation seam + goldens: make every dim a pure function of plain inputs, export goldens | cpp | **Mostly already true** — see spec §6.1's trace | The goldens are the Rust acceptance tests |
| P9 | HMM golden harness: pickle → neutral artifact exporter, full-field golden from real `RegimeEngine` output | lbrnet | Open | The Rust HMM is proven against it before Python HMM code is deleted |

### 1.2 Phase 2 — The merge (deferred, own schedule, not gated by Rust progress)

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

### 1.3 Phase 3 — Rust workstreams and dependencies

| ID | Workstream | Depends on | Status |
|---|---|---|---|
| W0s | Rust-in-Sierra integration spike | — | **Done** (§2) |
| W1 | Merge into `VSCode/MindfulTrader/` | W0 pre-work | Deferred (§1.2) |
| W2 | `rust/` workspace bootstrap (toolchain, cbindgen, DLL link, CI) | P2/P3, W0s | **Mostly done** — `rust-toolchain.toml`, workspace, `mts_ffi` skeleton all exist and are proven; not yet done: `install_py_ext.sh`, `check_all.sh`, CI |
| W3 | `mts_hmm` inference + regime engine | W2, P9 | Not started |
| W4 | Offline posteriors tool | W3 | Not started |
| W5 | `mts_observation_vector` port, dim by dim | W2, P8 | **Self-contained dims done** (5 of ~15); rest blocked on P2/P3 |
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

## 2. W0s — Rust-in-Sierra integration spike (closed out, 2026-10-08)

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

## 3. Crate status

| Crate | Status | Notes |
|---|---|---|
| `mts_ffi` | Skeleton shipped, 2026-10-08 | One staticlib, features `hmm`/`observation_vector`/`transport` turn subsystems on |
| `mts_observation_vector` | 5 of ~15 dims ported (all self-contained ones; rest blocked on P2/P3) | spec §6 / §5 below |
| `mts_schema` | Scaffolded 2026-10-09 | Generates from the now-single `mts_schema.fbs`; see spec §4.2 for the merge that simplified this |
| `mts_hmm` | Not started | Spec §5 |
| `mts_transport` | Not started | Deliberately last; spec §7 |
| `mts_py` (`mindful_core`) | Not started | PyO3 extension for `lbrnet`/`GUI` |

**Principles governing every crate** (established 2026-10-08, proven in practice across all five
`mts_observation_vector` ports so far): pure logic crates have zero Sierra/zmq/pyo3 dependency; the FFI
boundary is the only `unsafe` surface; every port goes through the proof pattern in §5.2 before any
production call site reads its result.

---

## 4. HMM lifecycle execution stages (spec §5's companion)

Stage A (golden harness, Python-only, P9) → Stage B-C (Rust inference + regime engine, matched against
the golden) → Stage D (offline posteriors tool) → Stage E (in-process shadow mode in a full
`BackTesterStudy` replay, zero divergence gate) → Stage F (event-schema cutover) → Stage G (Rust
training) → Stage H (deletions: Python HMM code, port 5561, mirror-doc updates).

Full per-stage detail to be filled in here when this work starts (not yet — §0 confirms `mts_hmm` has
not been started).

---

## 5. Observation vector pipeline: status and methodology

### 5.1 Current status and candidates

**Ported, shadow-wired, live-confirmed**: `SevcikFractalDimension`, `ComputeBipowerVariation` (§0).

**Ported, shadow-wired, build-verified (OFF byte-identical, ON links+grows), not yet live-confirmed
in Sierra Chart, 2026-10-09**: `DfaHurstExponent.h` (`hurst_exponent` — this system's single worst
HMM cross-state discriminator). 16/16 cross-language parity assertions pass (4 new); 12/12 Rust unit
tests pass (3 new). `MeanReversionCalculator.h` (`mean_rev_z`) — 21/21 cross-language parity
assertions pass (5 new, incl. flat-window and null-input carry-forward); 17/17 Rust unit tests pass
(5 new). `LiquidityFragilityEngine.h` (`liq_fragility`) — 26/26 cross-language parity assertions
pass (5 new, incl. thin-volume and null-input carry-forward); 22/22 Rust unit tests pass (5 new).

**None remaining self-contained** (no `FeatureScaler`/dim-index coupling) — all five such dims are
now ported (§0 lists all five). Every remaining dim is blocked on P2/P3 (§1.1).

**Deferred until P2/P3 settle** (one FlatBuffers version lands, dim indices need to come from the
schema, not literals — the exact bug class that has bitten this repo twice already): `EventVelocityEngine.h`
(`burstiness_index`), `CarryForwardCalculators.h`-coupled dims.

**Methodology, proven five times**: see §5.2 below.

### 5.2 Port-by-port approach

The proof pattern each landed dim has followed, and the template for every future one:

1. Port the pure function from its C++ header into `rust/observation_vector/src/lib.rs`, using
   fixed-size stack arrays where the C++ original does (no heap allocation, matching the hot-path
   discipline of the rest of this codebase).
2. Rust unit tests mirroring the C++ test file's exact golden values.
3. An `mts_ffi` FFI wrapper (`(ptr, len)` shape, panic-guarded via `catch_unwind`); regenerate the
   cbindgen header.
4. A cross-language parity test (same deterministic-LCG input into both the C++ original and the Rust
   wrapper, exact equality) in `tests/cpp/test_rust_observation_vector_parity.cpp`.
5. Wire into the real C++ call site in shadow mode (compare-and-log only, never used, logged once per
   process not every tick).
6. Verify `MTS_WITH_RUST=OFF` build byte-identical, `=ON` build grows appropriately.
7. Deploy to Sierra Chart for live confirmation (not yet done for every port landed so far — see §5.1's
   per-dim status).
8. Only then consider cutover.

**Boundary-and-golden groundwork this methodology assumes already exists** (verified 2026-10-07):

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
   `BackTesterStudy` replay (same pattern as §4's Stage E); then authoritative.
6. **`ContextManager.cpp` later** — state assembly, `TrainingEvent` construction; its own spec when it
   comes up.

---

## 6. Infrastructure mechanics

### 6.1 Files and their shape

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

### 6.2 CMake integration

`cpp/CMakeLists.txt` imports the Rust staticlib (add, don't restructure); a `MTS_WITH_RUST` option
(default OFF) and a matching CMake preset (`wsl-clang-cl-release-rust`, separate build directory) keep
the normal build path completely unaffected when Rust isn't enabled. `-ffast-math` policy: not yet
settled — avoid it wherever goldens compare values across languages (spec §2 item 11).

### 6.3 Scripts (repo root `scripts/`, every one derives `ROOT` from its own path)

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

### 6.4 CI (not yet built)

One workflow, path-filtered jobs, modelled on Atratus's `ci.yml` (which today gates Python only — a gap
to do better than, since Rust becomes a build prerequisite for `cpp/` the moment `mts_ffi` is wired in).

### 6.5 Cross-cutting runtime rules (bake into every code review of Rust work)

- Every `extern "C"` function is `catch_unwind`-guarded and returns a status.
- `mts_init`/`mts_shutdown` are explicit, driven by `sc.LastCallToFunction`. Nothing relies on
  destructors at unload; no `static` holds a thread or socket past shutdown.
- Unload/reload (§2 check 6) must pass — reasoned through, cleared — before any long-lived thread or
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

### 6.6 From Atratus — copy this, not that

| Copy (proven) | Do differently / better |
|---|---|
| Pure core crates separate from pyo3/zmq crates | **Pin the toolchain** — Atratus has no `rust-toolchain.toml` |
| xwin + clang-cl-22 + `/winsysroot` cc-rs variables; `/NODEFAULTLIB:libcmt.lib`; `--print native-static-libs` | **`[workspace.dependencies]`** — Atratus repeats versions per crate, a drift risk |
| cbindgen-generated header from `build.rs` | **One FlatBuffers version** — Atratus's crate is 24.3.25 against a 25.12.19 `flatc` |
| `catch_unwind` + explicit init/shutdown | **Scripted wheel install**, never a `.so` copy — Atratus's live GUI segfaulted once from exactly this |
| `[patch.crates-io]` with a vendored, commented crate when upstream needs a fix | **Rust in CI** — Atratus's CI is Python-only |
| Per-project env + lock; CI from the lock | **Small crates** — `atratus_rust` pulls polars + tract-onnx + plotters into one cdylib (slow builds); add a heavy dependency only to the crate that needs it |
| Commit gate: checks chained with `&&` | **Prove it in Sierra first** — Atratus's Rust DLL was never loaded in Sierra; here W0s already passed (§2) |
| Python-free hot path: Python only displays/forwards | `.pyi` stubs for the extension (mypy sees `Any` in Atratus without them) |
| `flatc --rust`'s atomic scratch-dir generation pattern (generate into `.work_gen/`, copy on success) | **Crate placement** — Atratus dumps generated Rust into its one do-everything `sensor_core`; this repo correctly uses a dedicated `mts_schema` crate instead, so `mts_hmm`/`mts_observation_vector`/`mts_transport` each depend on just the schema, not transitively on each other |
| `--rust-module-root-file` for multi-schema generation | **Not applicable anymore** — this repo merged its two schemas into one (spec §4.2), which Atratus never needed to since it only ever had one |
