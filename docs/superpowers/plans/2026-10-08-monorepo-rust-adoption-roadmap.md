# Monorepo + Rust Adoption Roadmap — Sequencing Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Status:** plan, sequencing decided 2026-10-08; most individual items still not started. This is
the *sequencing* layer over three existing design docs — it does not re-derive their content, only
orders the work and resolves one conflict between them.

**Read with (this plan defers to these for all technical detail):**
- `docs/superpowers/specs/2026-10-07-mindfultrader-monorepo-consolidation-spec.md` — the merge itself
  (§1-6), pre-work P1-P9 (§7), the Rust transport plan (§8).
- `docs/superpowers/specs/2026-10-07-rust-hmm-lifecycle-spec.md` — the Student-t HMM port.
- `docs/superpowers/specs/2026-10-08-monorepo-infrastructure-guide.md` — the concrete Cargo/CMake/
  Python-env/CI shape (`rust/` workspace, `mts_ffi`, `mindful_core`, scripts, pre-commit, CI).

## 1. The one sequencing decision this plan makes, and why

The three docs above were all written assuming (or defaulting to) **the repo merge happens before
the `rust/` Rust workspace is bootstrapped** — the infrastructure guide's own §11 puts "the merge"
at step 6 and "W2 bootstrap" at step 7.

**This plan reverses that order, per an explicit operator decision (2026-10-08 session):**
Rust-in-Sierra work and the repo merge are **independent decisions with independent schedules**.
Rust work starts now, per-repo, without waiting for the merge. The merge happens later, whenever its
own preconditions (chiefly `lbrnet`'s P7) are actually ready, and is motivated on its own terms (doc-
duplication-drift elimination — four manually-synced copies of `CLAUDE.md`/`GEMINI.md`/
`README-AI.md` today), not as a prerequisite for Rust.

**Why:** the biggest, least-reversible move in the whole program (merging four repos, 1,093 commits,
35 GB of data, force-push implications) should never be taken in service of a technical bet that
hadn't yet cleared its own kill criterion. That kill criterion (W0s check 3: does a Rust staticlib
load inside Sierra Chart) has since passed cleanly (§2), which removes the original urgency either
way — but the sequencing principle stands for future bets of this shape.

**Mechanical consequence:** before the merge, a MindfulTrader `rust/` crate that `lbrnet` or `GUI`
wants to depend on is reached via an ordinary cross-repo relative path dependency
(`{ path = "../../MindfulTrader/rust/hmm" }`), the same sibling-directory convention this workspace
already uses for `../schema`, `../docs`. When the merge eventually happens, those paths get the same
mechanical rewrite already scoped for the ~690 other path references (consolidation spec §3/§5
Stage 3) — no new risk category, just one more line in an already-understood rewrite.

**`rust/`'s placement, confirmed (operator, 2026-10-08):** `rust/` is the single, permanent home for
*every* Rust crate in the system, regardless of which of the four current repos the logic
conceptually belongs to (`schema`'s FlatBuffers bindings, `lbrnet`'s future PyO3 wrapper, `GUI`'s
future transport client, as well as `MindfulTrader`'s own `observation_vector`/`hmm`/`transport`/`ffi`). In the
merged monorepo (still named/hosted as `felocruz/MindfulTrader`, consolidation spec §1), `rust/` is a
**top-level sibling of `cpp/`, `lbrnet/`, `GUI/`, `schema/`** — never nested inside any of them.
**Consequence for the merge's own mechanics (consolidation spec §4 step 1, "one commit moves
everything into `cpp/` via `git mv`"):** that step must explicitly **exclude** `rust/` from the move
and instead hoist it to the new monorepo root as a sibling. Today, pre-merge, `rust/` lives nested
inside the current `MindfulTrader` repo (no new 5th git repo needed) — but that nesting is a
*transient, pre-merge-only* convenience, not the end state, and the merge Stage 2/3 steps
(consolidation spec §5) must carry this carve-out or `rust/` silently ends up at `cpp/rust/` instead.

- [ ] Add this carve-out explicitly to the consolidation spec's own Stage 2/3 description
  (currently silent on `rust/` since it predates this decision) — do this before Stage 2 is ever
  actually run, not after.

## 2. W0s status (input to this plan, not re-litigated here)

Full results table: consolidation spec §12.8. Summary as of 2026-10-08:

| Check | Result |
|---|---|
| 1 (links cleanly) | PASS |
| 2 (imports unchanged) | PASS (static check; not reverified on the second spike) |
| 3 (Sierra loads it) — **the kill criterion** | **PASS**, twice independently, with real evidence (checksum match + magic/version round-trip) |
| 4 (round-trip proves execution) | PASS |
| 5 (background thread advances) | PASS |
| 6 (unload/reload survival) | **Reasoned through, not empirically run** — see below |
| 7 (panic caught at FFI boundary) | PASS |
| 8 (call overhead) | PASS — **0.70 ns/call** measured |
| 9 (vendored zmq doesn't conflict) | Not attempted |

**Check 6 resolution (2026-10-08 session):** not run empirically, but walked through mechanism-by-
mechanism and found no applicable known failure mode: (a) the existing C++ codebase already proves
the explicit-shutdown-via-`sc.LastCallToFunction` pattern works for long-lived ZMQ sockets/threads
(`TransportStream`, `HMMClient`, `SystemOrchestrator`, `AIHeartbeatMonitor`, `TradeExecutionServer`
— every `SCSFExport` entry point has its own `LastCallToFunction` handler already), so this is not
new territory for this codebase; (b) the Windows loader-lock/`DllMain` deadlock risk doesn't apply
because shutdown happens on Sierra's normal thread via `LastCallToFunction`, never in `DllMain`; (c)
Rust's historical unwind-across-FFI UB (pre-1.71, RFC 2945) doesn't apply because every entry point
already calls `catch_unwind` before returning to C++ (check 7 proved this); (d) the crate is a
`staticlib`, not its own DLL, so its statics are just part of `MindfulTrader.dll`'s own image — no
separate Rust-DLL lifecycle exists to go wrong. **Conclusion: Rust is cleared for MindfulTrader.**
Re-run check 6 empirically only if a future crate's design changes one of these premises (e.g. a
thread that isn't explicitly joined, or a dependency with its own `Drop`-based cleanup).

- [ ] (Optional, low priority) Empirically confirm check 6 anyway once a real crate exists, nearly
  free at that point since the explicit start/stop plumbing will already be there for other reasons.
- [ ] Check 9 (vendored zmq vs the DLL's existing vcpkg libzmq, consolidation spec §12.5-4) —
  revisit when `rust/transport` actually needs zmq (§6.3 of the infrastructure guide has the interim
  "same dynamic vcpkg libzmq, no vendoring" plan).

## 3. Phase 1 — Foundations (parallelizable across repos, no merge required)

- [x] W0s kill criterion (check 3) — **done**, see §2.
- [x] P7 settle uncommitted work in `schema` (1 file), `MindfulTrader` (15 items), `MTS` (2 files) —
  **done**, 12 commits, 2026-10-07/08.
- [ ] P7 settle uncommitted work in `lbrnet` (86 files: 62 modified, 3 deleted, 21 untracked) —
  **deliberately deferred**, operator: "separate session/owner". Still blocks the merge's Stage 0.
- [ ] P1: root-relative paths in every script (`$(dirname "$0")`, `Path(__file__)`, never
  `/home/rcruz/devel/VSCode/...`) — consolidation spec §7. Shrinks the eventual merge's rewrite
  surface; safe and independent per project.
- [ ] P2: one FlatBuffers version (today: C++ headers 25.1.24, `flatc` 24.3.25, Python runtime
  25.9.23/24.3.25 depending on pip vs conda) — consolidation spec §7, infrastructure guide §2.3.
  Blocks any Rust crate reading the schema (`mts_schema`).
- [ ] P6: split the shared `mts` conda env (today `lbrnet` and `GUI` both use it) — consolidation
  spec §7. Infrastructure guide proposes `mts` stays `lbrnet`'s, `GUI` gets its own (`mts-gui`
  proposed, open question).
- [ ] P8: C++ observation seam + language-neutral goldens (consolidation spec §10 step 1-2) — the
  acceptance tests `rust/observation_vector` will be ported against.
- [ ] P9: HMM golden harness (lifecycle spec Stage A, Python-only, no Rust) — the acceptance tests
  `rust/hmm` will be ported against.
- [ ] Hardcoded Google API key in `MindfulTrader/config.py:22` and `MTS/config.py:21` — confirmed
  live and in active use (`MTS/journal_analysis.py` calls the Gemini API with it). **Deliberately
  deferred, operator's call on timing** (2026-10-08 session). Infrastructure guide §9.4 has the fix
  (rotate, move to env/keyring, decide separately on history scrub vs rotation-only).

## 4. Phase 2 — Incremental Rust, per-repo, pre-merge

This is the part that starts **now**, independent of Phase 1's remaining items and independent of
the merge. Order within this phase follows the existing specs' own dependency chain (schema → observation_vector ∥
hmm → transport), adapted only in that each step happens inside whichever repo currently owns the
code, not inside a merged monorepo's `rust/`.

- [x] **Scaffold `rust/` in MindfulTrader** — done 2026-10-08, commit `63bcc63`: `rust/Cargo.toml`
  (`[workspace]`, `[workspace.dependencies]`, `[profile.release]` per infrastructure guide §3.3),
  `rust/.cargo/config.toml`, `rust-toolchain.toml` at the repo root (pinned 1.98.1).
- [x] `mts_ffi` skeleton: one staticlib, `mts_abi_version()` only, no features enabled yet — done
  2026-10-08, commit `63bcc63`. Proves the CMake `MTS_WITH_RUST` option + link step work end to end:
  native `cargo test` (3/3 pass), cross-compiled to `x86_64-pc-windows-msvc`, `cmake --preset
  wsl-clang-cl-release-rust` links cleanly, import table confirmed byte-identical to the non-Rust
  build (`llvm-objdump-22 -p` — nothing calls `mts_abi_version()` yet, so nothing new is pulled in,
  expected static-linking behavior). **Not done, deliberately deferred**: no C++ call site wired, so
  no fresh Sierra Chart round-trip for this specific skeleton — operator judged the W0s spikes
  already sufficient evidence; revisit once a real subsystem gives the call site something to prove.
- [ ] `schema/regenerate_schema.sh` gains a `flatc --rust` target, writing into MindfulTrader's
  `rust/schema/` (P3, consolidation spec §7, infrastructure guide §5). Lives in the `schema` repo;
  no merge needed since `regenerate_schema.sh` already writes into sibling-repo paths today.
- [x] **First two real `rust/observation_vector` (`mts_observation_vector`) ports, done 2026-10-08**:
  `SevcikFractalDimension` and `BowleySkewness`/`MoorsKurtosis` (+ their shared `EmpiricalQuantile`
  helper), ported from `include/SevcikFractalDimension.h`/`include/RobustMoments.h`. Chosen as
  low-hanging fruit: pure, zero-SC-dependency, already-tested C++ headers. Exposed via `mts_ffi`'s
  new `observation_vector` feature (`mts_observation_vector_sevcik_fractal_dimension`,
  `_bowley_skewness`, `_moors_kurtosis`, each taking a raw `(ptr, len)` pair — the only shape that
  crosses a C ABI cleanly). **Three layers of proof, not just "it compiles"**: (1) `rust/
  observation_vector`'s own unit tests (6/6 pass) mirror each C++ test file's structure
  (brute-force-reference comparison for Sevcik; Gaussian-sample statistical-property checks for
  Bowley/Moors); (2) a new `tests/cpp/test_rust_observation_vector_parity.cpp` (9/9 pass) is the
  authoritative cross-language proof — it feeds the *same* deterministic-LCG-generated input array
  into both the original C++ function and the Rust FFI wrapper and asserts exact equality (not just
  "similar", not just "same statistical shape"); (3) both native Linux and
  `x86_64-pc-windows-msvc` cross-compiles build clean with the feature enabled. No production C++
  call site wired yet (same deliberate deferral as the `mts_ffi` skeleton above) — this step's job
  was proving the port-and-call pipeline works correctly, which the parity test now does
  definitively.
- [ ] `rust/observation_vector` (`mts_observation_vector`) continues dim-by-dim against the P8
  goldens — consolidation spec §10 steps 3-4, lifecycle-spec-equivalent gating. The two functions
  above are the first two dims; the remaining ~16 follow the same pure-port + parity-test pattern.
- [ ] `rust/hmm` (`mts_hmm`) inference + regime engine against the P9 goldens — lifecycle spec Stages
  B-C.
- [ ] Once a MindfulTrader `rust/` crate is useful to `lbrnet` or `GUI`, add it there via a cross-repo
  relative path dependency (§1) — first candidate is likely `mts_hmm` via PyO3, closing the
  already-named Python-port parity gap (execution findings Finding 12).
- [ ] `rust/transport` — **last**, per the existing specs' own reasoning (removing 5561 via `hmm`
  deletes the hardest port `transport` would otherwise face) — consolidation spec §8, lifecycle spec
  §9.

Everything from "in-process shadow mode" onward (W6-W10 in the consolidation spec's workstream
table) is unchanged by this plan and still gated exactly as already specified there.

## 5. Phase 3 — The merge (deferred, own schedule, not gated by Rust progress)

- [ ] Runs consolidation spec Stages 0-6 as already designed, whenever Phase 1's `lbrnet` P7 and the
  rest of Phase 1 are settled. Not re-planned here.
- [ ] Primary motivation to track going in: eliminating the four-mirror-doc duplication/drift
  problem (`CLAUDE.md`/`GEMINI.md`/`README-AI.md`/`.github/copilot-instructions.md` currently hand-
  synced across `schema`/`MindfulTrader`/`lbrnet`/`MTS`), not Rust enablement — Phase 2 does not
  depend on this phase at all.

## 6. Open questions

1. **Which Phase 2 item actually goes first after the empty scaffold** — `mts_ffi` skeleton link
   proof, or `schema`'s `flatc --rust` target? Leaning skeleton-first (proves the harder, riskier
   mechanical step — CMake+Cargo link — before investing in any real crate), but not decided.
2. **One `mts_ffi`/one `mindful_core`** vs. the original umbrella spec's per-subsystem `*_ffi`/`*_py`
   — infrastructure guide §2.2/§12 item 1 recommends one of each (duplicate-`std` linking problem).
   Not yet ratified by the operator; needs a decision recorded in the consolidation spec's decision
   register (§12.4) once Phase 2 starts.
3. **`GUI`'s conda env name** (`mts-gui` proposed) and whether `lbrnet` keeps `mts` — infrastructure
   guide §12 item 2, tied to Phase 1's P6.
4. **Numeric policy**: whether `-O3 -march=native -ffast-math -mavx2` (in `CMakeLists.txt` under
   `if(NOT MSVC)`) actually applies to the clang-cl DLL build is unverified (depends on CMake's
   `MSVC` variable detection for clang-cl) — consolidation spec §10/§12.5-5, infrastructure guide
   §4.3. Recommended policy: no fast-math anywhere a value is compared against a golden. Not yet
   verified against real `compile_commands.json` output.
5. **`MindfulTrader` git history scrub** (force-push, to remove the API key from history) vs.
   rotation-only — infrastructure guide §9.4/§12 item 4, explicitly "the operator's call". Tied to
   §3's deferred API key item.
6. **`windows-cross` CI job** (via `cargo-xwin`) now or later — infrastructure guide §12 item 5. Low
  priority until Phase 2 has a real crate worth CI-checking on every push.
7. **Transformer coherence rule wording** — "older-sequence" against the regime-epoch start vs. the
   literal latest HMM step — lifecycle spec §9-1, still open, relevant once `rust/hmm` reaches
   Stage F (event-schema cutover).
8. **Cross-repo CI mechanics**, once each repo gets its own CI: how does `lbrnet`'s CI job check out
   MindfulTrader's `rust/` crate for its relative-path dependency to resolve? Not addressed by any
   existing doc — today there is no CI for any of the four repos (infrastructure guide §1, §12.6 of
   the consolidation spec), so this is unblocked for now but will need an answer before Phase 2's
   cross-repo dependency step goes anywhere near automation.
