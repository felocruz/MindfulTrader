# MindfulTrader Monorepo Consolidation — Spec

**Status**: design only, nothing implemented, no repo has been touched. Opened 2026-10-07 from a
brainstorm comparing this system's four-repo layout with `Atratus` (`/home/rcruz/devel/VSCode/Atratus`),
which is a single repo holding its schema, native code, Rust and Python GUI. This is the **living
umbrella doc** for the whole effort; updated through the same session as decisions were made (§1a).

**Scope.** §1-6: the repo merge itself. §7: pre-work done in each project first. §8-10: the Rust work
the merge enables (transport, HMM lifecycle, observation pipeline). §12: the whole picture — end state,
workstreams and dependencies, what stays, what is not yet analyzed, cross-cutting risks. Companion spec:
`2026-10-07-rust-hmm-lifecycle-spec.md`. Session pointer: `SCRATCHPAD.md`.

## 1. Decision (operator, 2026-10-07)

- `/home/rcruz/devel/VSCode/` stays the workspace folder holding independent projects. `Atratus/`
  stays a sibling and its own repo (it shares no code, libraries or state with this system).
- The four MTS projects merge into **one repo under `VSCode/MindfulTrader/`**, reusing the GitHub
  repo `felocruz/MindfulTrader` and its name. Two directories are renamed:
  `MindfulTrader` → `cpp`, `MTS` → `GUI`.
- Non-goals **of the merge (§1-6)**: no code or wire-format change; only directories move. The `MTS`
  C++ namespace, `mts_schema.fbs` and the `mts` conda env keep their names through the merge.
- The Rust work in §8-10 is **separate and separately gated**. It does change code, adds additive
  schema fields (the HMM output on the event stream, §9), and retires `models/hmm_model.pkl` in favour of
  a manifest + weights artifact (§9). Each such change lands with its own gate, never as a side effect
  of the merge.

## 1a. Decision log (2026-10-07 brainstorm, in order)

1. Atratus (`../Atratus`, one repo: schema → generated Rust + Python → Cargo workspace → Dash GUI, C++
   reduced to a 130-line ACSIL tick pump over a Rust staticlib) is the reference shape. Its transferable
   ideas for MTS: monorepo (A), pure Rust core behind a C-ABI (B), in-process Rust HMM (C), typed Rust
   safety gate (D, deferred — Sierra stays in the order path for now), PyO3 for the GUI data path (E).
   Operator chose **A first**.
2. Monorepo lives at `VSCode/MindfulTrader/` (reuses the GitHub repo/name), `MindfulTrader`→`cpp/`,
   `MTS`→`GUI/`; `VSCode/` stays the workspace of independent projects; `Atratus/` stays its own repo (§1, §2).
3. "Pre-work" in each project before the merge, including moving ZMQ to Rust (§7, §8).
4. HMM decision: the **whole Student-t HMM lifecycle moves to Rust** — training, offline posteriors,
   in-process live inference; 5561 deleted, not migrated (§9). Dedicated spec:
   `2026-10-07-rust-hmm-lifecycle-spec.md`.
5. **Transformer coherence ruling** (operator, "appropriate"): the C++ HMM state is authoritative and
   sequence-stamped; a Transformer action is paired with the HMM state of its `sequence_id` and is stale
   if inferred before the current regime began; the native floor is never suppressed. Full text:
   lifecycle spec §3. One interpretation still to confirm (regime-epoch vs literal older-sequence).
6. **Observation vector moves to Rust too** (operator, later the same session): the code that computes the
   18D vector comes into Rust, with the dim calculators free of any Sierra Chart dependency so they can be
   unit-tested; `ContextManager.cpp` itself follows later (§10).

7. **Sequencing ruled** (operator: "go with your proposal"): §1b stands as written.

Status: design only. No repository, directory or code has been changed by any of the above.

## 1b. Sequencing across the whole effort (RULED, operator, 2026-10-07: "go with your proposal")

```
 Phase 1  pre-work, per project, no Rust, each stands alone        (§7: P1-P9)
          └─ includes the golden/seam work the Rust ports will be tested against
 Phase 2  the merge into VSCode/MindfulTrader/                      (§4-5: Stages 0-6)
 Phase 3  core/ Cargo workspace, in the merged repo
          ├─ core/hmm   Rust HMM lifecycle                          (§9 + lifecycle spec, Stages B-H)
          ├─ core/obs   Rust observation pipeline                   (§10, steps 3-7)
          └─ core/transport  Rust ZMQ                               (§8)
```

Reasoning: Phase 1 items are independent and valuable even if Phase 3 never happens; the merge makes
Phase 3 land in one repo with one schema; within Phase 3, `hmm` and `obs` form one chain
(`dims → scaler → gate → HMM step`) and removing 5561 (by `hmm`) deletes the hardest port from
`transport`, so `transport` goes last. Phase-1 work that is Rust-free (HMM Stage A goldens, the C++
observation seam and goldens) can start before the merge. Rust-free Phase-1 work (P8, P9) may start before the merge.

## 2. Target layout

```
VSCode/
├── Atratus/                       unchanged, own repo
└── MindfulTrader/                 the monorepo (repo root)
    ├── CLAUDE.md  GEMINI.md  README-AI.md  .github/copilot-instructions.md   system-level mirrors
    ├── PRODUCTION_TRIAGE.md       from VSCode/ root (now versioned)
    ├── .claude/  scripts/         from VSCode/ root (check_north_star.sh, shared ops scripts)
    ├── .github/workflows/         one CI, one job per project
    ├── docs/                      from VSCode/docs (143 files)
    ├── schema/                    old `schema` repo
    ├── cpp/                       old `MindfulTrader` repo
    ├── lbrnet/                    old `lbrnet` repo (name unchanged)
    └── GUI/                       old `MTS` repo
```

`../schema`, `../docs` and `../lbrnet` references from inside `cpp/`, `GUI/` and `lbrnet/` stay valid
because those directories remain siblings. Only references to the old names, and absolute paths,
need rewriting.

## 3. Measured baseline (2026-10-07)

| Repo | Commits | Tracked | Working tree | Remote |
|---|---|---|---|---|
| schema | 20 | 33 files, 0.4 MB | 18 MB | felocruz/schema |
| MindfulTrader | 479 | 564 files, 31 MB | 187 MB | felocruz/MindfulTrader |
| lbrnet | 458 | 1,070 files, 52 MB | **35 GB** (data/models untracked) | felocruz/lbrnet |
| MTS | 6 | 136 files, 15 MB | 27 MB | felocruz/MTS |

`VSCode/` itself is not a git repo: `PRODUCTION_TRIAGE.md` (127 KB), `docs/`, `scripts/` and
`.claude/` have no history or backup, yet every repo's CLAUDE.md binds to them.

References that break (tracked files, `git grep`):

| Repo | Absolute `/home/rcruz/devel/VSCode/<proj>` | Old names `MindfulTrader`/`MTS` in paths |
|---|---|---|
| schema | 33 | 6 |
| MindfulTrader → cpp | 325 | 269 |
| lbrnet | 308 | 75 |
| MTS → GUI | 27 | 16 |

Other known coupling:

- `schema/regenerate_schema.sh` derives `WORKSPACE_ROOT`, writes to `MindfulTrader/include/generated`
  and `lbrnet/lbrnet/generated`, and reads `MindfulTrader/include/flatbuffers/base.h`.
- `mts` and `mts-cpu-backup` conda envs hold `lbrnet-2.0.0` as an **editable install** pointing at the
  old path (`direct_url.json`).
- `MTS/MTS GUI.code-workspace` and each project's `.vscode/` settings.
- `check_north_star.sh` hardcodes `/home/rcruz/devel/VSCode/PRODUCTION_TRIAGE.md`.
- `MindfulTrader` has a git pre-commit hook enforcing the four-mirror-doc sync (the hook did not
  show up in `.git/hooks` when checked; locate its real source before moving it).
- Agent session and memory state keyed by project path (e.g. `/tmp/claude-1000/-home-rcruz-devel-VSCode-*`).
- Not checked: Windows-side paths (`/mnt/c/Trading/...`, Sierra Chart deploy targets). Expected
  unaffected, verify in Stage 0.

## 3a. What the monorepo removes

Today a schema change takes three commits in three repos with nothing making them atomic, plus
`schema_change_control_gate.py`, `validate_schema_sync.py`, the MCP contract server and
`PENDING_SCHEMA_CHANGES.md` to bridge the boundary. After the merge a schema change and its
regenerated outputs (`cpp/include/generated`, `lbrnet/lbrnet/generated`) land in one commit. Those
tools are not deleted by this migration; whether to shrink them is a later decision.

## 4. Approach

Preserve the 479-commit `MindfulTrader` history without a force-push by making it the base:

1. In the existing `MindfulTrader` repo, one commit moves everything into `cpp/` (`git mv`; git
   tracks renames, `git log --follow` keeps working).
2. For `schema`, `lbrnet` and `MTS`: **throwaway clones**, `git filter-repo --to-subdirectory-filter
   schema|lbrnet|GUI`, then `git merge --allow-unrelated-histories` into the base. The originals are
   never rewritten.
3. Add the root files (`PRODUCTION_TRIAGE.md`, `docs/`, `scripts/`, `.claude/`) in a separate commit.
4. Tag every pre-merge HEAD (`premerge/<repo>`) and push the tags to the old remotes before archiving.

Directory swap on disk (same filesystem, so `lbrnet`'s 35 GB is renamed, not copied):
`mv MindfulTrader _MindfulTrader_old` → `mkdir MindfulTrader` → `mv _MindfulTrader_old MindfulTrader/cpp`
and so on. Untracked content (`lbrnet/data`, models, build outputs) moves with its directory and must
be covered by the root `.gitignore`.

## 5. Stages and verification gates

Each stage ends with a gate; do not start the next stage on a red gate.

**Stage 0 — Preconditions (no moves).**
- Run the §7 pre-work, P1-P9 (each is an independent per-project change; P1 shrinks Stage 3, P6
  retires open question 2, P8/P9 build the goldens the Rust ports are tested against).
- Settle uncommitted work: lbrnet has 86 changed files (62 modified, 3 deleted, 21 untracked),
  MindfulTrader 12, MTS 2, schema 1. Commit or discard each, per owner. A merge on a dirty tree hides changes.
- Record baselines: `./build_dll.sh`, `native tools` checks, `pytest` (lbrnet, GUI), schema
  `validate_schema.sh`, so every later gate compares against known-good.
- Locate the real source of the four-mirror-doc hook; check Windows-side paths; list every conda env
  and IDE config that embeds a project path.
- Full backup: `tar` of the three small repos' `.git` dirs, and a `git bundle` of each repo. (lbrnet's
  untracked 35 GB is moved, not copied; confirm free space is not needed.)

**Stage 1 — Version the root files (safe, independent, worth doing alone).** Put `PRODUCTION_TRIAGE.md`,
`docs/`, `scripts/`, `.claude/` under git. Gate: files byte-identical to the originals.

**Stage 2 — Merge in a scratch location.** Perform §4 steps 1-3 in `/home/rcruz/devel/_mt_merge/`
(outside `VSCode/`) on clones. Gate: `git log -- <dir>` shows each project's full history; commit
counts match §3; the tree file list equals the union of the four trees plus root files.

**Stage 3 — Path rewrite, in the scratch repo.** Rewrite absolute and old-name references (§3), script
first, then hand-review the executable ones: `regenerate_schema.sh`, `build_dll.sh`, CMake presets,
`check_north_star.sh`, `.vscode/` and workspace files, CI. Docs get a mechanical rewrite. Gate:
`schema/regenerate_schema.sh` round-trip yields **no diff** in `cpp/include/generated` and
`lbrnet/lbrnet/generated`; `grep` for old absolute paths returns nothing outside historical specs.

**Stage 4 — Build and test in the scratch repo.** `cpp/build_dll.sh` clean build; native tool tests;
`lbrnet` pytest with the re-pointed editable install; GUI smoke start. Gate: results equal the Stage 0
baselines.

**Stage 5 — Cut over.** Swap directories on disk (§4), re-run the `lbrnet` editable install in `mts`,
update the sync hook, CI and agent project settings. Gate: Stage 4 suite green from the real paths.

**Stage 6 — Remote.** Push the merged history to `felocruz/MindfulTrader`; because the cpp move is a
normal commit this is a fast-forward and needs no force-push. Archive `schema`, `lbrnet`, `MTS` on
GitHub after pushing the `premerge/*` tags; update the old READMEs to point at the new home.

Rollback: before Stage 5 nothing in the real tree changed, so abandon the scratch repo. After Stage 5,
restore from the Stage 0 bundles and move directories back.

## 6. Open questions

1. `lbrnet`'s 86 uncommitted files: whose are they and are they ready to commit? Gating for Stage 0.
2. The `mts` conda env is shared by `lbrnet` and `GUI` — resolved as pre-work P6 (§7): split before the merge.
3. CI: one workflow with a job per project (path-filtered), modelled on `Atratus/.github/workflows/ci.yml`
   — which today gates only Python; decide whether native C++ tool checks run in CI.
4. Whether `docs/` stays flat at the root or is partitioned per project (flat keeps current
   `../docs/...` links valid; recommended).
5. Whether the root `CLAUDE.md` should hold system-level rules only, with per-directory CLAUDE.md files
   kept for each project (recommended; the four-mirror sync contract then applies at both levels).
6. ~~The §1b ordering~~ — ruled 2026-10-07 (§1b): pre-work → merge → `hmm`/`obs` → `transport`, with
   Rust-free P8/P9 allowed to start before the merge.
7. Open items inherited from the Rust specs: the Transformer coherence wording (lifecycle spec §9-1),
   the numeric/fast-math policy (§10), the untraced dims in §10a (`relative_range`, `recurrence_rate`,
   `micro_asymmetry`, burstiness assembly), and the open Phase 0 dim decisions that make some dims a moving target (§10).
8. **Documentation impact, to do when each piece lands (Documentation Sync Contract):** the four mirror
   docs (`README-AI.md`, `.github/copilot-instructions.md`, `CLAUDE.md`, `GEMINI.md`) need the new layout
   after the merge, the artifact convention change (`models/hmm_model.pkl` → manifest + weights) after
   §9, and Rust as a build prerequisite for `cpp/` (Done Checklist) after §8-10. None changed yet.

## 7. Pre-work, per project (before Stage 2; each item stands alone)

Rule for every item: it must leave its project better off even if nothing else here ever happens, and
it changes no wire format. The system is not in production (`PRODUCTION_TRIAGE.md`), so dead code
found along the way is deleted, not shimmed.

| # | Item | Project | Why it helps the merge |
|---|---|---|---|
| P1 | **Root-relative paths.** Every script resolves its project root from its own location (`$(dirname "$0")`, `Path(__file__)`), never `/home/rcruz/devel/VSCode/...`. Docs keep absolute paths only where they are historical records. | all four | Removes most of the ~690 absolute-path rewrites in §3; nothing breaks at the new location. |
| P2 | **One FlatBuffers version.** C++ headers are 25.1.24, the system `flatc` is 24.3.25 (known mismatch), and Atratus's Rust crate pins 24.3.25. Pick one version, install the matching `flatc`, regenerate, confirm no diff beyond the version assertions. | schema | A third language (Rust, §8) cannot be added on an unsettled version. |
| P3 | **`flatc --rust` in `regenerate_schema.sh`**, generating into a not-yet-existing consumer path (guarded like the other targets). Nothing consumes it until §8. | schema | Same script, one more target; mirrors Atratus's `regenerate_schema.sh`. |
| P4 | **C++ transport seam.** One interface (publish bytes / serve request / poll reply) behind which today's `zmq::socket_t` members (`SystemOrchestrator` ×2, `TradeExecutionServer`, `AIHeartbeatMonitor`, `TransportStream`) and `HMMClient`'s raw C-API DEALER sit. Delete the deprecated socket path (`include/MINDFUL_SOCKET_ZMQ_DEPRECATED.md` and anything only it references, after a full-repo usage search per the Code Safety Rules). | cpp | §8 then swaps an implementation instead of editing 27 files. |
| P5 | **Python transport seam.** lbrnet: `transport.py` becomes the only module that imports `zmq`. GUI: the same for `zmq_client.py`; `system_orchestrator.py` (3,826 lines) and `action_plan.py` stop creating sockets themselves. Resolve the **duplicate HMM ROUTER** (both `lbrnet/backtest/backtest_server.py:604` and `MTS/zmq_client.py:323` bind 5561). | lbrnet, GUI | Two clients and one server of one protocol become one module each. |
| P6 | **Split the `mts` conda env** into per-project envs with lock files, as Atratus did after its 2026-09-01 numpy/cryptography incident; re-point the editable `lbrnet-2.0.0` install. | lbrnet, GUI | Shared env is the one thing the merged repo must not inherit. |
| P7 | **Settle uncommitted work** (Stage 0 precondition, restated): lbrnet 86 files, MindfulTrader 12, MTS 2, schema 1. | all | A merge over dirty trees hides changes. |
| P8 | **C++ observation seam + goldens** (§10 steps 1-2): (trace of `StudyHelperFunctions.cpp` DONE, §10a) move the residual dim math behind an adapter of plain structs (as already done for `mean_rev_z`/`liq_fragility`); export the existing `fixtures_dim*_raw.h` and a whole-pipeline golden (real ticks in → 18D + gate decisions out) in a language-neutral format. | cpp | Every dim becomes a function of plain inputs, testable without Sierra; the goldens are the Rust acceptance tests. |
| P9 | **HMM golden harness** (lifecycle spec Stage A): pickle → neutral artifact exporter, and a full-field golden from the real `RegimeEngine` over a real `.context` slice. Python only. | lbrnet | The Rust HMM is proven against it before any Python HMM code is deleted. |

Verification for P1-P6: each project's existing checks unchanged (`./build_dll.sh`, native tool tests,
`pytest`, schema validate) plus a live loopback: the C++ test client and the Python server still
complete the CONFIG handshake and exchange one HMM request on 5561.

## 8. Rust transport — port-by-port migration (after the merge)

**Goal.** One Rust crate owns every ZMQ socket for all three languages, as Atratus does
(`atratus_sensor/src/ipc.rs` for the DLL side, `rust/src/client.rs` for the GUI side). libzmq is
compiled into the Rust staticlib, so the DLL no longer links vcpkg `libzmq-mt-4_3_5.lib` / libsodium.
Python reaches it through PyO3 with the GIL released. Payloads stay FlatBuffers; ports and socket
patterns stay as they are, so each port flips independently against an unchanged peer.

**Where it lives.** A top-level Cargo workspace in the monorepo (`core/`): a pure `transport` crate,
a `transport_ffi` staticlib (C-ABI + cbindgen header consumed by `cpp/`), and a PyO3 module for
`lbrnet/` and `GUI/`. Toolchain: the clang-cl + xwin-sysroot cross-build already proven in
`Atratus/cpp/build_dll.sh` (same `~/.local/sysroots/x86_64-pc-windows-msvc` MindfulTrader uses).

**Verified socket inventory (2026-10-07; bind side established from `bind`/`connect` calls):**

| Port | Pattern | C++ DLL | Python | Rust-migration shape |
|---|---|---|---|---|
| 5555 | PUB | binds (`TransportStream`, sndhwm set) | SUBs connect (GUI, lbrnet) | One-way. Ring + background thread, drop-oldest, like Atratus's sensor. **First.** |
| heartbeat | SUB | connects (`AIHeartbeatMonitor`, rcvtimeo 5 s) to a Python PUB bound in `MTS/system_orchestrator.py:243` | binds PUB | One-way receive. **Second.** |
| 5560 | REP | binds (`SystemOrchestrator` control, CONFIG_REQ/ACK) | REQ connects | Request/reply — needs `poll`/`submit` shape (below). |
| 5562 | REP | binds (`SystemOrchestrator` mental profile, rcvtimeo 2 s) | REQ connects (`action_plan.py:190`) | Request/reply. |
| 5558 | REP | binds (`TradeExecutionServer`, rcvtimeo 250 ms) | REQ connects (`BacktestLiveAgent`) | Request/reply on the trade path; backtest-gated. |
| 5561 | DEALER→ROUTER | connects (`HMMClient`: DEALER + inproc PAIR worker, sndhwm/linger set) | binds ROUTER (×2 today, see P5) | **Decide §9 first** — may disappear. |

(The 5556 "trade validation" REP named in older docs was not found among the C++ bind sites; confirm
before including it. The inventory above is from socket-creation grep, not a protocol read — Stage 0
of this section includes reading each flow.)

**The one real design problem.** Atratus's sensor is fire-and-forget telemetry; nothing on its hot path
waits. MTS has synchronous request/reply on the Sierra thread (HMM inference, trade RPC, control
handshake). Across a C-ABI that must become non-blocking `submit(request) -> ticket` /
`poll(ticket) -> Option<bytes>` with the existing timeouts enforced inside Rust, so the DLL never
blocks inside Sierra's update call. Document each flow's current blocking semantics and timeout before
porting it; keep behavior identical (including `BackTesterStudy`'s replay ordering, governed by
`docs/BACKTESTING_FRAMEWORK.md` acceptance gates).

**Order, with a gate per port** (flip one port; the peer is unchanged; revert = flip back):

1. `transport` crate + `transport_ffi` skeleton: init/shutdown, context-per-process rule preserved.
   Gate: DLL builds, imports only system/VC-runtime DLLs (check with `objdump -p`), Sierra loads it.
2. 5555 PUB. Gate: the GUI and lbrnet SUB receive identical bytes (diff of a recorded stream) and
   the ring never blocks the Sierra thread under replayed-tick load.
3. Heartbeat SUB. Gate: stale-heartbeat detection fires at the same threshold as today.
4. Python side: PyO3 client replaces lbrnet's `transport.py` and the GUI's `zmq_client.py` internals
   behind the P5 seams. Gate: existing Python tests green with the Rust client.
5. 5560 / 5562 / 5558 request-reply ports, one at a time, with the `submit`/`poll` shape.
   Gate per port: handshake and one trade round-trip against the unchanged peer; for 5558 a full
   `BacktestLiveAgent` replay matching the pre-migration `.btst` output.
6. 5561: not migrated — removed by the §9 in-process HMM.
7. Remove the vcpkg `libzmq`/`libsodium` link from `cpp/CMakeLists.txt` and delete the old socket code
   (not behind a flag — project is not in production).

**Non-goals.** No schema change, no protocol redesign, no change to port numbers or message ordering.

**Risks.** Sierra loads the DLL in-process, so a transport bug is a Sierra crash, not a log line; keep
the C-ABI tiny and panic-safe (`catch_unwind` at every `extern "C"` boundary). Windows MSVC runtime
mixing (the Atratus build links `/NODEFAULTLIB:libcmt.lib` to match the study's `/MD`) — reuse its
settings verbatim. Rust becomes a build prerequisite for `cpp/` — add it to CI and the Done Checklist.

## 9. Decision (operator, 2026-10-07): the whole Student-t HMM lifecycle moves to Rust

Decided direction (dedicated spec: `2026-10-07-rust-hmm-lifecycle-spec.md`; nothing implemented):

- A pure `core/hmm` crate (no zmq/pyo3/ibapi, like Atratus's `sensor_core`) owns **training, offline
  posterior generation and live inference**, so train/serve skew is impossible by construction.
- **Training** (from `.context`) moves to Rust. Python keeps orchestration only (meta-HMM control plane,
  HPO, audit scripts) by calling the same crate through PyO3 (Atratus already exposes `HMMTrainer` /
  `StudentTHMM` this way).
- **Offline posteriors** for Transformer training (today `materialize_hmm_features.py` /
  `compute_context_posteriors()` writing `*.context.posteriors.npy` + `.meta.json` sidecars, joined
  to `.alpha` in Python) are written by a Rust tool from the same crate. First version keeps the
  existing sidecar format so Transformer training code is unchanged.
- **Live inference** runs in-process in the DLL (C-ABI staticlib), fired by the existing Mahalanobis
  significant-change gate. The 5561 DEALER/ROUTER is **deleted, not migrated** (§8 step 6 is replaced
  by its removal); `FeatureSpine` pairing and burn-in disappear because C++ already holds both inputs.
- **Transformer coupling inverts to one-way:** C++ ships the HMM posterior with each event (additive
  schema fields, non-breaking, per `docs/ADR/risk_gate_context_wire_spec.md`'s pattern); the Python
  Transformer consumes it. The piggybacked `reinfer_action_id` on `RiskStateUpdate` goes away and the
  HMM/Transformer coherence rule needs a ruling of its own.

Parity strategy: **inference** must match Python bit-for-bit-ish (golden file, stated tolerance, real
observations) before any Python HMM code is deleted. **Training** cannot be bit-exact (EM is
non-convex); its gate is statistical: same `.context`, same init, held-out LL/BIC, state profiles and
posterior agreement within tolerance.

Open items for the dedicated spec: CPU training time vs the current GPU path; Rust `.context`/`.alpha`
reader (third implementation of the file format after the C++ writer and Python reader); the
Transformer coherence rule; manifest format replacing `models/hmm_model.pkl` (update the cross-project
artifact convention in all four mirror docs when it lands); no-allocation hot-path inference.

## 10. Observation pipeline in Rust (operator direction, 2026-10-07; design only)

**Direction.** The code that computes the 18D `ObservationData` vector moves to Rust, built from
functions that take plain data and have no Sierra Chart (`sc.*`) dependency, so each is unit-testable.
`ContextManager.cpp` itself moves later. Together with §8 and §9 the chain becomes
`ticks/bars → dims → FeatureScaler → Mahalanobis gate → HMM step` inside one Rust `core/`, with the ACSIL
study shrinking toward Atratus's shape: a thin adapter that reads Sierra arrays and calls Rust.

**Where we start (measured 2026-10-07) — much of the de-coupling is already done, in C++:**

- `src/ContextManager.cpp` (1,510 lines) has only **6 lines** containing `sc.` (all in one block, lines
  755-772: tick size and a High/Low lookback). Its `BuildObservationVector()` (line 373) is already
  Sierra-free in practice.
- `tools/market_data_replay/MarketDataReplayEngine.h` already reconstructs **all 18 dims** from raw ticks
  using ~25 pure headers (`ImbalanceBarEngine`, `FeatureScaler`, `ObservationTriggerGate`,
  `EventVelocityEngine`, `SevcikFractalDimension`, `DfaHurstExponent`, `RecurrenceRateEngine`,
  `TailRiskEngine`, `InformationEngine`, `LiquidityFragilityEngine`, `MeanReversionCalculator`,
  `RobustMoments`, `BipowerVariation`, …), validated against the real 471.9M-tick `mes_ticks.parquet`
  (335,147 aligned MO+SS pairs, zero sequence mismatches). `tests/cpp/` has 41 `test_*.cpp` files and
  real-data `fixtures_dim{0,1,3,6,7,8,9,12}_raw.h`.
- What is still Sierra-coupled: the glue that reads bar arrays — line counts containing `sc.`:
  `src/StudyHelperFunctions.cpp` 257 (467 occurrences), `src/SCStudies.cpp` 121, `src/IndicatorManager.cpp`
  51. The trace of which of those feed a dim is §10a (done for `StudyHelperFunctions.cpp`).

So "make the dim calculators Sierra-free and testable" is largely a finished C++ fact; the Rust work is a
port with a ready-made golden source, not a greenfield design. Atratus's `sensor_core/microstructure.rs`
is a different (13D) vector and is a style reference only, not reusable dims.

### 10a. P8 trace result: `StudyHelperFunctions.cpp` and the dims (read-only, 2026-10-07)

Operator: "we may want to get `StudyHelperFunctions.cpp` out of the way of the dims computations."
Finding: **the dim code is a small, separable slice of that file; most of its `sc.` use is unrelated to dims.**

- The dim-feeding functions sit in lines ~2085-2882 (about 800 lines, 106 `sc.` occurrences) of a 2,881-line
  file with 467. The other ~360 occurrences are Raschke setup/tactical detection, structure detection,
  `Draw*` helpers and efficiency-ratio code — nothing to do with `ObservationData`.
- **Data flow today:** `TripleScreen{1,2,3}.cpp` (the Sierra study functions) call the `Calculate*`
  wrappers in `StudyHelperFunctions.cpp`; those read `sc.Close/High/Low/Volume[...]` and
  `sc.GetPersistent*` (last-valid carry-forward state), call pure engines, and the Triple Screens write
  the result into `ContextManager`'s `ObservationData` (`mutate_*`, "Central Observation Store");
  `ContextManager::BuildObservationVector()` (line 373) reads it back. **So the real Sierra boundary is
  `TripleScreen{1,2,3}.cpp` plus these wrappers, not `ContextManager`.**
- Two dims bypass the file entirely and are already Sierra-free: `lempel_ziv` (`InformationEngine`) and
  `tail_index` (`TailRiskEngine`), event-level engines fed by `UpdateMarketPhysics()`; the `fast_*` dims
  come from `ActivityClockManager`'s `ImbalanceBarEngine` (also pure).

| Wrapper (`StudyHelperFunctions.cpp`) | Called from | Pure engine it already uses | Remaining Sierra coupling |
|---|---|---|---|
| `CalculateLogScaleRatio` | TS1:478 | `ComputeBipowerVariation` | bar-array pull (near-thin) |
| `CalculateHurstExponent` | TS1:483, TS2:737 | `DfaHurstExponent.h` | bar pull + 4 persistent-state ops (last-valid carry-forward) |
| `CalculateFisherInformation` | TS1:502 | `ComputeFisherInformation` | bar pull + 1 persistent |
| `CalculateFractalDimension` | TS2:309, 311 | `SevcikFractalDimension.h` | bar pull + 1 persistent |
| `CalculateLogScaleExpansionRatio` | TS2:322 | `ComputeBipowerVariation`, `ComputeBurstinessIndex` | bar pull + 1 persistent |
| `CalculateAmihudIlliquidity` | TS3:710 | `cfc::ComputeAmihudIlliquidity` (final aggregation only) | **inline accumulation loop + live-bar term still in the wrapper** (68 lines) |
| `CalculateLiquidityFragility` | TS3:711 | `LiquidityFragilityEngine.h` | bar pull + cold-start policy + 1 persistent |
| `CalculateMeanReversionSpeed` | TS3:800 | `MeanReversionCalculator.h` | bar pull + 1 persistent |
| `CalculateRealizedKurtosis`, `CalculateSkewness` | the subgraph writer `UpdateObservationVectorSubgraphs` (line 2458) | `RobustMoments.h` | bar pull; `ContextManager.cpp:457` says these bar-based values were superseded by the activity-clock path, so they likely **no longer feed the vector** — verify |
| `CalculateAdaptiveObservationWindow`, `CalculateFisherAdaptiveWindow` | TS1/2/3 | none (window sizing from `coherence_score`) | 2 persistent ops each; they size the windows the dims use, so they are dim *inputs* |
| `CalculateVolConvexity` | TS3:805 | none | feeds `RiskGateContext` via `SetVolConvexity`, **not** `ObservationData` (restored 2026-09-18); same extraction applies, different consumer |

Not yet traced to a function: `relative_range`, `recurrence_rate`, `micro_asymmetry` (TS3 reads
`Subgraph_MicroAsymmetry[sc.Index]` — a Sierra subgraph array used as storage), and where `burstiness_index`
is finally assembled (`ComputeBurstinessIndex` is called from the expansion-ratio wrapper). Sierra
subgraph arrays holding intermediate dim values are a second coupling beside `sc.GetPersistent*`.

**What "out of the way" means concretely (P8 deliverables):**
1. A plain input view (`BarSeries`: spans of close/high/low/volume/ask/bid/trades, current index, tick
   size) and per-dim state structs replacing `sc.GetPersistent*` (the carry-forward the offline engine
   already has in `CarryForwardCalculators.h`).
2. Each wrapper body becomes a pure function of (`BarSeries`, state, window) — for most this is just
   replacing `sc.Close[idx]` with `series.close[idx]`; `CalculateAmihudIlliquidity` also needs its inline
   loop moved into the pure header. The Sierra side keeps only a 3-line adapter (build the view, call, store).
3. New home for the pure functions (a `dims/` area in `cpp/include`, to be ported to `core/obs` later),
   so `StudyHelperFunctions.cpp` keeps only Raschke/structure/draw code.
4. **Dead-code candidate:** `ComputeHurstFromReturns` (the old R/S Hurst, ~98 lines, zero `sc.` use) has no
   caller anywhere in `src/`, `include/`, `tests/` or `tools/` (grep 2026-10-07) since DFA replaced it.
   Under the standing not-in-production rule, delete after a full-repo usage search (per the Code Safety Rules).
5. Verify with the existing goldens: `fixtures_dim*_raw.h` and the offline engine's output must be
   unchanged by the extraction (it is a refactor; no value may move).

**Approach: port against goldens, one dim at a time.**

1. **Boundary first (C++, no Rust, worth doing alone).** Define the adapter seam: the Sierra side reads
   `sc.*` arrays into plain structs (tick; closed TS1/TS2/TS3 bar OHLCV and bar-close events) and calls
   pure code. Move the residual SC-coupled dim math behind it (the same extraction already done for
   `mean_rev_z` and `liq_fragility`). Result: every dim is a function of plain inputs.
2. **Language-neutral goldens.** Export the existing fixtures to a neutral format, and generate a
   **whole-pipeline golden**: a real tick slice in, the 18D vector plus every gate decision out, from the
   C++ replay engine. These become the Rust acceptance tests.
3. **`core/obs` crate**, ported dim by dim in order of stability (dims still in flux last — see risks).
   Gate per dim: its fixtures plus the pipeline golden, with tolerances stated up front.
4. **Rust replaces the offline generator.** Gate: reproduce the existing `.context` over the full real tick
   set (same 335,147 pairs, zero mismatches). The C++ `tools/market_data_replay/` is then deleted.
5. **In-process in the DLL, shadow mode** against the C++ `ContextManager` through a full
   `BackTesterStudy` replay (the same pattern as the HMM spec's Stage E); then authoritative.
6. **`ContextManager.cpp` later:** state assembly, `TrainingEvent`/`.alpha` assembly, `.context` writing.
   `LBRFileManager` cannot link outside Windows (it pulls in `windows.h`); the standalone
   `ContextFileWriter.h` is the model for a Rust writer.
7. **PyO3 for lbrnet.** The same crate replaces Python re-implementations of feature math (the
   live-vs-offline feature parity tests in lbrnet show they exist; not audited here), which closes the
   Python-port parity gap (execution findings, Finding 12) for features as the HMM spec does for the model.

**Schema dependency.** Dim indices must come from the schema, not literals: extend
`schema/scripts/generate_contract_header.py` (which writes `mts_schema_contract_generated.h`) to emit a
Rust contract module, alongside pre-work P3. Per-dim array misalignment has already bitten this repo
(`FeatureScaler`'s `LOGZ_WINSOR_SIGMA_OVERRIDE`, `DIM_RECURRENCE_INDEX`).

**Risks and cautions.**

- **The dims are still moving.** Phase 0 of Elite Feature Set Curation left open decisions
  (`amihud_illiquidity`, `relative_range`, `liq_fragility`, `fast_mean_rev_z` wire-or-drop); `mean_rev_z`,
  `hurst_exponent` and `recurrence_rate` are decided for activity-clock treatment but not implemented;
  Feature Saliency EM may change the set. Porting a moving target means double maintenance. Mitigation:
  port stable dims first; for a dim still changing, change C++ and Rust together against one shared
  golden, or defer it.
- **The reference has never been validated against Sierra.** The offline reconstruction builds its own
  bars from ticks; byte-validation against a genuine Sierra-collected file (replay plan Task 12) is
  **blocked** — the only `.context` on disk is schema 230, the current schema is 240. Rust will match the
  C++ replay engine, which proves the port, not the replay's fidelity to live Sierra bars. Obtain a fresh
  SC-collected v240 file before treating live parity as established. Also open: `regime_tenure` counts
  ticks, not bars (`2026-09-08-real-dll-findings-from-offline-generator-spec.md`).
- **Floating-point parity.** `CMakeLists.txt` sets `-O3 -march=native -ffast-math -mavx2` under
  `if(NOT MSVC)`. Whether that applies to the clang-cl DLL build depends on CMake's `MSVC` variable —
  **not verified**; check the real compile commands. `-ffast-math` licenses reassociation and a no-NaN
  assumption, which matters to this codebase's NaN/carry-forward guards, and the offline tools are built
  by bare `g++` without it. Decide one numeric policy (Rust has no fast-math by default) and state the
  tolerance before comparing.
- **Hot path.** Dims run on every tick on all three screens. Rust-owned rolling state, no per-tick
  allocation, and a coarse FFI boundary (one call per tick or per bar-close, not per dim).
- **Scope creep into the rest of the study.** The 30+ indicators, `PositionManager`, `RiskManager` and the
  Triple Screen logic stay C++; this section moves only the observation pipeline and, later,
  `ContextManager`.

**Open questions:** which of the `StudyHelperFunctions.cpp` `sc.*` references feed a dim; the single
numeric policy; whether to port stable dims before the open Phase 0 decisions are settled or wait;
whether the Rust writer replaces `ContextFileWriter.h` immediately or after Step 4.

## 11. Related, later (not part of this migration)

A typed Rust safety gate (Atratus's `ladder_core::safety` pattern) would slot into the same `core/`
workspace; deferred, because Sierra stays in the order path for now.

## 12. The whole picture (consolidated 2026-10-07; read this first when picking the effort back up)

### 12.1 End state

```
VSCode/MindfulTrader/                         one repo, one schema, one CI
├── schema/      mts_schema.fbs ──flatc──► cpp/include/generated, core (Rust), lbrnet/generated, GUI
├── core/        Rust Cargo workspace (new)
│   ├── obs/       18 dims + FeatureScaler + Mahalanobis gate     (pure; no sc.*, no zmq)
│   ├── hmm/       Student-t HMM: train, posteriors, live step, regime engine  (pure)
│   ├── transport/ every ZMQ socket for all three languages
│   ├── *_ffi/     staticlibs + cbindgen headers linked into the Sierra DLL
│   ├── *_py/      PyO3 modules for lbrnet and GUI
│   └── tools/     train, posteriors, replay (replaces tools/market_data_replay)
├── cpp/         Sierra DLL = thin adapter + TS1/2/3 indicators + PositionManager/RiskManager/…
├── lbrnet/      Transformer, labeler, training orchestration (Python)
├── GUI/         Dash GUI (Python), talks to the system through the PyO3 transport client
├── docs/  PRODUCTION_TRIAGE.md  .claude/  scripts/
```

Runtime, end state:
`ticks → Sierra → cpp adapter → core/obs (dims → scaler → gate) → core/hmm (in-process step) →
HmmStateIndicator + the event (carrying the HMM output) → core/transport PUB → lbrnet Transformer →
action back through transport → PredictionState (paired by the §9 coherence rule)`; the GUI subscribes via
the same Rust client. Offline: `raw ticks → core/obs tool → .context → core/hmm train → artifact + manifest
→ posteriors sidecar → Transformer training`. 5561 no longer exists; the observation vector, the model and
the posteriors have exactly one implementation each.

### 12.2 What moves and what stays (measured 2026-10-07, non-generated tracked lines)

| Area | Size | Fate |
|---|---|---|
| Observation math (pure headers + the `Calculate*` wrappers) | ~25 pure headers; wrappers ~800 lines in `StudyHelperFunctions.cpp` | **moves** to `core/obs` (§10) |
| Python HMM: `student_t_hmm.py` 1,693 + GPU 501, `regime_engine.py` 1,827, `hmm_utils.py` 619, `train_student_t_hmm.py` >4,500 | ~9k lines | **moves** to `core/hmm` (§9); Python keeps orchestration only |
| ZMQ: 5 socket-owning C++ classes + `HMMClient`; lbrnet 3 files; GUI 5 files | — | **moves** to `core/transport` (§8) |
| C++ `src/` | 29,280 lines (`PositionManager` 3,204; `RiskManager` 2,953; `StudyHelperFunctions` 2,881; `SystemOrchestrator` 1,891; `BackTesterStudy` 1,837; `TripleScreen1/2/3` 3,796; `IndicatorManager` 1,545; `ContextManager` 1,510) | **stays C++** apart from the above; `ContextManager` is a later port (§10 step 6) |
| C++ `include/` 19,027, `tools/` 19,038, `tests/` 131,079 (much of it likely real-data fixtures — not checked) | — | tools/market_data_replay deleted when `core/obs` replaces it; the rest stays |
| lbrnet Python (excl. generated) | ~102k lines: scripts 37,074; core 11,148; models 9,023; backtest 8,020; data 6,319; features 4,169; labeling 3,296; inference 1,944; tests 18,320 | **stays Python**; only the HMM files above move |
| GUI (MTS) Python | ~32k lines (`system_orchestrator.py` 3,826; `help_system.py` 3,543; `trade_analytics.py` 2,393; `zmq_client.py` 1,535) | **stays Python**; its ZMQ client becomes the PyO3 client |

The Rust work is a bounded fraction of the system. The execution layer (`PositionManager`, `RiskManager`,
`ExecutionGate`), the Triple Screens, the Transformer, the labeler and the GUI do not move.

### 12.3 Workstreams and dependencies

Sizes are rough scope calls (S/M/L), not estimates — the repo's own triage convention is "no invented numbers".

| ID | Workstream | Ref | Depends on | Size | Gate (short) |
|---|---|---|---|---|---|
| W0 | Pre-work P1-P9 (each independent; P7 gating) | §7 | — | S each; P6/P8/P9 M | each project's existing checks unchanged |
| W0s | **Spike: Rust staticlib linked into the real `MindfulTrader.dll`, called from `EventDataCollectorStudy.cpp`, loaded in Sierra** (design and status: §12.8; built, awaiting the Sierra load) | §12.5, §12.8 | — | S-M | all §12.8 checks recorded; **kill criterion in §12.8** |
| W1 | Merge into `VSCode/MindfulTrader/` | §4-5 | W0 (P1, P7 at least) | M | Stages 0-6 gates |
| W2 | `core/` workspace bootstrap: toolchain pin, cbindgen, DLL link, maturin build script, CI jobs for Rust + native C++ + Python | §8, §12.6 | W1, P2/P3, W0s | M | clean build of DLL with the Rust staticlib; CI green |
| W3 | `core/hmm` inference + regime engine | lifecycle spec B-C | W2, P9 | M-L | full-field golden match |
| W4 | Offline posteriors tool | lifecycle D | W3 | M | match the 21.97M-row sidecar |
| W5 | `core/obs` port, dim by dim | §10 steps 3-4 | W2, P8 | L | per-dim fixtures + whole-pipeline golden; reproduces the real-tick `.context` |
| W6 | In-process shadow mode in a full `BackTesterStudy` replay (HMM, then obs), then authoritative | lifecycle E, §10 step 5 | W3, W5 | M | zero divergence; `.btst` identical |
| W7 | Event-schema cutover: HMM output rides the event; Transformer reads it; coherence rule implemented | lifecycle F, §3 | W6 | M | replay equality incl. the flip/stale case |
| W8 | Rust training | lifecycle G | W6 (prove inference on the frozen model first) | L | statistical gate; CPU time at 22M rows |
| W9 | `core/transport`, port by port (5555, heartbeat, then request/reply) | §8 | W7 (5561 gone), W2 | L | per-port gates |
| W10 | Deletions + mirror-doc updates + PRODUCTION_TRIAGE rows | lifecycle H, §6-8 | W6-W9 as each lands | S-M | usage search clean; docs synced |
| W11 | `ContextManager.cpp` port | §10 step 6 | W5, W6 | L | later; own spec |

Critical path: `P2/P3 + W0s → W2 → W3 ∥ W5 → W6 → W7 → W9 → W10`, with W4 and W8 branching off W3/W6.
Rust-free work that can start now: P1, P2, P6, P7, P8, P9 (and W0s once a Rust toolchain is in place).

### 12.4 Decision register

| Decision | State |
|---|---|
| Monorepo at `VSCode/MindfulTrader/`; `cpp/`, `GUI/`; `Atratus/` stays separate | **ruled** |
| Sequencing: pre-work → merge → `hmm`/`obs` → `transport` | **ruled** |
| HMM lifecycle (train, posteriors, live) in Rust; 5561 deleted | **ruled** |
| Observation pipeline in Rust; `ContextManager` later | **direction set**; dims-first order and numeric policy open |
| Transformer coherence rule | **ruled**; wording to confirm (regime epoch vs literal older-sequence) |
| Numeric policy (fast-math or not, tolerances) | **open** (§10) |
| CI shape; flat vs partitioned `docs/`; root `CLAUDE.md` scope | **open**, recommendations in §6 |
| Manifest holds the regime-engine thresholds (vs crate constants) | **open**, recommended: manifest (lifecycle §8) |
| Typed Rust safety gate; Rust order path | **deferred** (§11) — Sierra stays in the order path |

### 12.5 Cross-cutting risks

1. **The central assumption is not yet proven in Sierra.** Atratus's own triage (row 13) says its Rust-linked
   `Atratus.dll` is "Built 2026-10-05 (cross-compiled; not yet loaded in Sierra)". Everything in §8-10
   depends on that pattern working inside Sierra's process. Hence W0s: prove it with a trivial DLL
   (one C-ABI call, init/shutdown, unload) before any dependent work.
2. **Moving targets.** The dims (Phase 0 open items) and the HMM (K, features, Feature Saliency, the unmet
   fat-tail sign-off) are still changing; ports must be manifest/golden-driven so a change lands in one place.
3. **Unvalidated references.** The offline reconstruction has never been byte-validated against Sierra
   (Task 12 blocked on a v240 file); the Rust ports will match the C++ reference, not necessarily live Sierra.
   A fresh Sierra-collected v240 `.context` is a prerequisite for claiming live parity.
4. **Two libzmq copies in one DLL (expected, untested).** `cpp/CMakeLists.txt` links the vcpkg import library
   `libzmq-mt-4_3_5.lib` (+ `libsodium.lib`; the runtime `libzmq-mt-4_3_5.dll` sits in Sierra's `Data/`),
   while a Rust `zmq` crate that compiles libzmq in (as Atratus's does) would define the same `zmq_*`
   symbols. During the §8 transition this can fail at link time or, worse, silently bind the wrong copy.
   Resolve before W9: either Rust links the same dynamic libzmq (`zmq-sys` against the system lib, no
   vendoring) or the C++ side drops its direct libzmq use first. §12.8 stage 2 tests it early.
5. **Numeric policy.** Possible `-ffast-math` in the DLL build vs none in tools and Rust (§10).
6. **A long program touching the same files.** Many workstreams change `ContextManager`, `HMMClient`,
   `SystemOrchestrator`, the schema and the event stream. Mitigation: every stage reversible until its
   deletion step, shadow modes, one change at a time (prove inference on the frozen model before Rust trains).
7. **This is re-platforming, not edge.** The Vision section of `PRODUCTION_TRIAGE.md` still lists the HMM
   fat-tail sign-off and HMM-blind risk gates as the real gaps. None of this program closes them by itself
   (it only makes the HMM available in-process to `RiskGateContext`). Do not let it displace that work.
   When the program starts, add its rows to `PRODUCTION_TRIAGE.md` (Triage Protocol rule 7).

### 12.6 Not yet analyzed (gaps in this picture)

- **Labeler ↔ native `StructureTest` parity** (`lbrnet/labeling/triple_barrier_scanner.py` vs the C++
  detector, a mandated co-evolution per the trap-detection rules). A shared Rust core could remove that
  duplicate too; not examined.
- **GUI beyond ZMQ** (Firestore lifecycle, websocket broadcaster, 3,826-line `system_orchestrator.py`).
- **Execution layer** (`PositionManager`/`RiskManager`/`ExecutionGate`): stays C++; HMM-aware gating is the
  separate predator-sniper initiative (`2026-09-18-predator-sniper-execution-architecture.md` §3a).
- **`BackTesterStudy` / `backtest_server`**: every workstream gates on `.btst`-identical replays; the
  existing acceptance gates in `docs/BACKTESTING_FRAMEWORK.md` apply and were not re-read here.
- **Windows-side deployment**: DLL deploy, Sierra paths, and the two live JSON config files outside git
  (`/mnt/c/Trading/config/`) that carry migrated thresholds.
- **CI today**: whether any exists for the four repos; Atratus's is Python-only.
- **Hot-path budget**: no per-tick latency measurement exists in this analysis; needed for W5/W6 FFI design.
- **PyO3 build/packaging**: Atratus installs its extension by hand (editable `maturin`); needs a scripted build.

### 12.7 First moves, in order

1. P7: settle uncommitted work (lbrnet 86 files, MindfulTrader 12, MTS 2, schema 1).
2. W0s: the Rust-in-Sierra spike.
3. P2/P3: one FlatBuffers version, then `flatc --rust`.
4. P1, P6 in parallel (root-relative paths; split the `mts` env).
5. P8, P9: the goldens the Rust ports are tested against.
6. Then the merge (W1).

### 12.8 W0s design: the Rust-in-Sierra integration spike (operator suggestion, 2026-10-07)

**Idea.** Add a Rust function, call it from `scsf_EventDataCollector` in `EventDataCollectorStudy.cpp`, deploy,
and confirm the DLL loads. Testing inside the **real** `MindfulTrader.dll` (not a toy DLL) is the point: it
exposes link-level conflicts with the existing 29k-line build that a trivial DLL would hide.
"Deploy the GUI" is read here as: load the DLL in the Sierra Chart GUI and check it; say if you meant the MTS GUI.

**Facts checked 2026-10-07:** the toolchain is present (`cargo`, `rustc`, target `x86_64-pc-windows-msvc`,
`clang-cl-22`, the xwin sysroot at `~/.local/sysroots/x86_64-pc-windows-msvc`); Sierra is at
`/mnt/c/SierraChart2` and loads `Data/MindfulTrader.dll` (deployed 2026-09-17; the local build output is
from 2026-09-20, so they differ); `deploy_mindfultrader.sh` copies the build over it after a timestamped
backup; the DLL's exports are governed by `MindfulTrader.def`; MSVC builds use `/W4 /WX` (a cbindgen header
must be warning-clean); `Logger` writes `C:/Trading/logs/MindfulTrader.log`, readable from WSL at
`/mnt/c/Trading/logs/MindfulTrader.log`. That log is the agent-checkable verification channel; Sierra's
own Message Log is the human one.

**Safety rules for the spike**
- Work in a **clean `git worktree` of `HEAD`**, not the working tree: it has 12 uncommitted modified files
  (ContextManager, RobustMoments, generated headers, …) that must not be baked into the test DLL.
- The Rust link is behind a CMake option, **off by default** (`MT_RUST_SPIKE=ON`), built into its own build
  directory, so the normal `./build_dll.sh` output is unchanged.
- The spike call is clearly labelled, one place, removable; it is not production code and is deleted or
  folded into W2 afterwards.
- Deploying replaces the live `Data/MindfulTrader.dll` (the script backs it up first) and loading it is a
  Windows-side action in Sierra; those two steps are the operator's go/no-go, not done unattended.

**Stage 1 — minimal crate, no dependencies.** `staticlib`, cbindgen header, three C-ABI functions:
`mt_spike_ping(out buf, len) -> status` (returns a version string and a magic number),
`mt_spike_start/stop` (spawns and joins a background thread that increments a counter), and a
`catch_unwind`-guarded `mt_spike_panic_test` that must return an error instead of unwinding into Sierra.
The study calls `ping` once on first run, `start` on first run, `stop` on `sc.LastCallToFunction`, and logs
the results through `Logger` and `sc.AddMessageToLog`.

**Stage 2 — add the `zmq` crate** (vendored build, as Atratus) and call one trivial zmq function, to surface
the duplicate-libzmq conflict (§12.5-4) now rather than at W9.

**Checks (record each pass/fail and the evidence in this section):**

| # | Check | How |
|---|---|---|
| 1 | Links cleanly with the real DLL (CRT consistency, `/W4 /WX`, PCH, no duplicate symbols) | build output |
| 2 | Imports are only system/VC-runtime DLLs (+ the existing `libzmq-mt`) | `llvm-objdump-22 -p` on the DLL |
| 3 | Sierra loads the DLL; the study appears and runs | Sierra Study list; no load error |
| 4 | The ping round-trips (string + integer) | `MindfulTrader.log` and Sierra Message Log |
| 5 | The background thread advances while the study runs, and joins at `LastCallToFunction` | counter in the log; no hang on remove |
| 6 | **Unload/reload survives:** remove the study, replace the DLL while Sierra is open, re-add — no crash, no zombie thread, statics re-initialize | manual, repeat 3x |
| 7 | A forced Rust panic is caught at the boundary | `mt_spike_panic_test` result in the log |
| 8 | **Call overhead from the ACSIL thread:** time 1e6 calls to a no-op C-ABI function, log ns/call | first data for the hot-path budget gap (§12.6) |
| 9 | Stage 2: DLL still links and loads with a vendored libzmq present, or the exact failure is recorded | build output + load |
| 10 | Size and build-time delta of the DLL | `ls -l`, build timing |

**Why check 6 matters most.** Sierra unloads and reloads study DLLs; a Rust thread or global that outlives
`DLL_PROCESS_DETACH` is a crash inside Sierra's process. Atratus's sensor holds a `OnceLock` singleton and a
ZMQ router thread and has not been through this. Whatever shape check 6 settles on (explicit
`init`/`shutdown` driven by `LastCallToFunction`, never relying on destructors at unload) becomes the rule
for every later `*_ffi` crate.

**Exit criteria.** Checks 1-7 pass, or each failure is understood and has a recorded fix; checks 8-10 have
numbers. Only then is W2 (the `core/` bootstrap) unblocked. If check 3, 6 or 9 fails fundamentally, §8-10
need redesign (e.g. Rust in a separate helper process instead of in-DLL) and this spec must be updated first.

**Kill criterion (operator, 2026-10-07): the goal is to find out whether the DLL loads in Sierra Chart. If it
does not, Rust is scrapped from MindfulTrader.** Applied as: a fundamental failure of check 3 or 6 (the DLL
will not load, or cannot survive unload/reload) after a diagnosis that finds no practical fix withdraws the
Rust plans in §8-10 and the lifecycle spec. The Rust-free work stays and keeps its value (pre-work P1-P9,
the merge, the C++ observation seam and goldens, the transport and HMM seams). A link-time or packaging
problem with a known fix is not a kill. The spec is to be updated immediately on a kill, before any other work.

**Implementation (2026-10-07).** Branch `spike/rust-in-dll` (one commit, `98ec03f`, never merged or pushed)
in a clean `git worktree` of `HEAD` at `/home/rcruz/devel/_mt_spike`, so none of the working tree's
uncommitted changes are in it. Contents: `rust_spike/` (a no-dependency staticlib: magic + version string,
background worker with explicit `start`/`stop` that joins, `noop` for timing, a contained deliberate panic),
a hand-written `include/mt_spike.h` (cbindgen is a W2 item), a `MT_RUST_SPIKE` CMake option (OFF by
default), the call sites in `EventDataCollectorStudy.cpp` (`#ifdef MT_RUST_SPIKE`, logs to
`C:/Trading/logs/rust_spike.log` and Sierra's Message Log with the prefix `RUSTSPIKE`, deliberately not via
`Logger`, whose first use renames the shared `MindfulTrader.log`), and the spike build's `SCDLLName` changed
so it cannot be mistaken for the production DLL. Built with the unmodified `./build_dll.sh --no-clean` after a
one-time `cmake --preset wsl-clang-cl-release -DMT_RUST_SPIKE=ON`. The live `Data/MindfulTrader.dll` was
**not** touched: the spike DLL was copied as a new file, `C:\SierraChart2\Data\MindfulTrader_RustSpike.dll`
(sha256 `c99f5316…ff40`, identical to the build output).

**How to run it (operator, Windows side).** In Sierra: Analysis → Studies → Add Custom Study, choose the DLL
`MindfulTrader_RustSpike.dll`, function "Event Data Collector", apply to the TS3 chart. The study is inert
unless armed. Read Window → Message Log, or `/mnt/c/Trading/logs/rust_spike.log` from WSL. For check 6:
remove the study, re-copy a rebuilt DLL over `MindfulTrader_RustSpike.dll` while Sierra is open, re-add;
repeat 3x. To revert: delete `MindfulTrader_RustSpike.dll`.

**Results so far (static, no Sierra involved):**

| # | Check | Result |
|---|---|---|
| 1 | Links cleanly with the real DLL | **PASS** — links with `/W4 /WX`, no duplicate-symbol or CRT errors; stage 2 (zmq crate) not attempted yet |
| 2 | Imports only system/VC-runtime DLLs (+ existing `libzmq-mt`) | **PASS** — `llvm-objdump-22 -p`: `libzmq-mt-4_3_5.dll`, `KERNEL32`, `MSVCP140`, `VCRUNTIME140`, `ntdll`, `api-ms-win-*`; the Rust staticlib added no new import |
| 10 | Size / build time | DLL 1,923,584 B vs 1,779,712 B for the 2026-09-20 production build (+143,872 B); build step ~7 s wall on 32 jobs (not a clean-build timing) |
| 3-9 | Load, ping, worker thread, unload/reload, panic, call overhead, stage 2 | **pending: needs Sierra** |

**Results from Sierra (2026-10-07, a second, independent spike — methodology differs from the plan
above, see caveat).** Run before this agent had read this spec (so it did not follow the worktree /
separately-named-DLL / separate-log-file safety rules above): the crate (`rust_probe/`, deleted after)
was linked directly into the real `CMakeLists.txt`/`build_dll.sh`, the call added directly in
`EventDataCollectorStudy.cpp` (reverted via `git checkout` after), and the build was deployed over the
live `Data/MindfulTrader.dll` itself (via the existing `deploy_mindfultrader.sh`, which does take a
timestamped backup) rather than a separately-named spike DLL. Sierra Chart was launched fresh for the
test (confirmed no pre-existing instance beforehand) on the **ExampleChartbook**/**BacktestingHourlyTrading**
chartbooks, Sim/delayed data only — no live order path was touched. Logging reused the shared `Logger`
(`C:/Trading/logs/MindfulTrader.log`), not a separate spike log. **Caveat for future spikes:** follow the
worktree + separately-named-DLL + separate-log-file rules above; this run worked out safely (clean
revert, Sim-mode-only chart, DLL restored and byte-matched to a pre-Rust rebuild afterward) but carried
more risk to the live `Data/MindfulTrader.dll` than necessary.

| # | Check | Result |
|---|---|---|
| 1 | Links cleanly with the real DLL | **PASS** — `/NODEFAULTLIB:libcmt.lib` needed (Rust's dynamic-CRT `/MD` object vs the toolchain's forced `/DEFAULTLIB:libcmt.lib`), same fix `Atratus/cpp/CMakeLists.txt` already uses |
| 2 | Imports only system/VC-runtime DLLs | not reverified this run (crate had zero deps beyond `std`; expect same PASS as the earlier static check) |
| 3 | **Sierra loads the DLL; the study appears and runs** | **PASS** — `scsf_EventDataCollector` ("Event Data Collector - Elite v2.3") attached and armed on a live chart in Sierra 2949; processed 40,000+ ticks afterward with no crash (`ContextManager`/`FeatureScaler` diagnostics kept logging normally); process stayed `Responding: True` throughout |
| 4 | The round-trip proves genuine execution, not just linkage | **PASS, with independent numeric proof** — `mindful_rust_probe_tick(input) -> input*31 + call_count`, a process-wide atomic counter; three real log lines (`tick(71030)=2201931,count=1`; `tick(0)=2,count=2`; `tick(1)=34,count=3`) all matched the formula exactly when checked independently — not reproducible by a stub or corrupted call |
| 5 | Background thread start/stop/join at `LastCallToFunction` | **not tested** — the probe had no thread, only a static atomic |
| 6 | **Unload/reload survives** | **not tested** — the study was armed once; no DLL swap while Sierra stayed open, no study remove/re-add cycle. Per this spec's own "why check 6 matters most", this is the single most important remaining check before `core/transport`/`core/hmm` (which do need long-lived threads/singletons, e.g. a ZMQ router) can be trusted in-process |
| 7 | Forced panic caught at the FFI boundary | **not tested** — no `catch_unwind` in the probe; the real crate (per Stage 1 above) must have this on every `extern "C"` fn before anything non-trivial ships |
| 8 | Call overhead (ns/call) | **not measured** |
| 9 | Stage 2 (vendored `zmq` crate, surfaces the duplicate-libzmq risk, §12.5-4) | **not attempted** |
| 10 | Size/build-time delta | DLL grew 1,775,616 → 1,781,248 B (+5,632 B) for a single-function, zero-dependency crate; build step ~7 s wall (not a clean-build timing) |

**Net effect on the kill criterion:** check 3 is the one named in the kill criterion ("the goal is to
find out whether the DLL loads in Sierra Chart") and it is now a clean **PASS with real evidence**, not
just a static/offline one. Checks 5-9, especially 6, remain open and should be run — using this
section's prescribed safer methodology (worktree, separately-named DLL, separate log) — before treating
W0s as fully exited and unblocking W2.

Cleanup performed the same session: `rust_probe/`, `include/MindfulRustProbe.h`, and all three touched
files' diffs were reverted (`git checkout`); the DLL was rebuilt clean and redeployed; test export files,
DLL backups made during the test, and screenshots were deleted; the test Sierra Chart instance was closed.
`git status` after cleanup shows only this repo's pre-existing unrelated changes.
