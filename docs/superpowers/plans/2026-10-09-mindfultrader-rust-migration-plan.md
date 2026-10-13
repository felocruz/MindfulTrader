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

**Last updated**: 2026-10-12.

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
  1.98.1), `rust/Cargo.toml` workspace with `[workspace.dependencies]`, `ffi` staticlib skeleton,
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
  `=ON` links and grows to 1,953,280 bytes); live Sierra Chart confirmation deferred. (This bullet's
  "all self-contained dims now ported" claim was corrected the same day — see the next bullet; it was
  true of the 5 dims identified *at the time*, not of every genuinely self-contained dim.)
- **Correction + sixth through tenth `rust/observation_vector` dims ported and shadow-wired, same
  day**: the §5.1/§1.3 "blocked on P2/P3" claim for `EventVelocityEngine.h`/`CarryForwardCalculators.h`
  was **wrong** — verified by direct code read that neither file (nor their real production call
  sites in `StudyHelperFunctions.cpp`/`TripleScreen2.cpp`/`ContextManager.cpp`) has any
  schema/`Contract::`/dim-index coupling at all; the actual dim-index coupling lives entirely in
  `FeatureScaler.h`, downstream of all 18 dims equally, not a blocker unique to these five. Ported:
  `ComputeRelativeRange` (dim 2, 2 call sites), `ComputeBurstinessIndex` (dim 3's real caller despite
  its name — `log_scale_expansion_ratio`, not `burstiness_index`), `ComputeFisherInformation` (dim 8),
  `ComputeAmihudIlliquidity` (dim 11) — all four in `CarryForwardCalculators.h` — plus
  `eve::CalculateBurstinessIndex` (dim 1, `burstiness_index`/`raschkeBurst`) in
  `EventVelocityEngine.h`. 38/38 Rust unit tests pass (16 new); 33/33 cross-language parity
  assertions pass (7 new). Six real production call sites shadow-wired (3 in
  `StudyHelperFunctions.cpp`, 2 in `TripleScreen2.cpp`, 1 in `ContextManager.cpp` — the first ports
  to touch files other than `StudyHelperFunctions.cpp`). `MTS_WITH_RUST=OFF` byte-identical
  (1,779,712 bytes); `=ON` links and grows to 2,009,088 bytes. Live Sierra Chart confirmation
  deferred.
- **Operator-directed audit, same day: verified no dead/legacy C++ code was blindly ported into
  `rust/observation_vector`** — every function in the crate (13 total, incl. the internal
  `empirical_quantile` helper) re-traced to a live, actually-called C++ function via direct grep of
  `src/*.cpp` for real (non-test) callers; none are dead. Found two real gaps as a result, not dead
  code but *orphaned* ports: `bowley_skewness`/`moors_kurtosis` had been ported, unit-tested, FFI-
  wrapped, and parity-tested, but never shadow-wired to their real call site
  (`ContextManager.cpp`, feeding `OBS_SKEWNESS`/dim 10 and `LocalRiskContext::fastTalebKurtosis`);
  `dfa_hurst_exponent` had a second, independent live call site in `ContextManager.cpp`
  (`fastHurstRaw`/`LocalRiskContext::fastHurstExponent`, dim 9's additive twin) that was missed when
  the dim was first ported — only `StudyHelperFunctions.cpp`'s call site had been wired. Both gaps
  fixed: shadow-wired now, `MTS_WITH_RUST=OFF` reconfirmed byte-identical, `=ON` reconfirmed to link
  and grow (2,042,368 bytes). **Lesson: after porting a dim, grep for every real call site of the
  underlying C++ function, not just the first one found — a function can have more than one live
  caller.**
- **15th `rust/observation_vector` dim ported and shadow-wired, 2026-10-12**: `lempel_ziv`
  (`InformationEngine::GetLempelZivComplexity()`, LZ76 complexity, Kaspar & Schuster 1987) — a
  recount while picking this dim found the crate-status table's "12 of ~18" figure already stale
  (`micro_asymmetry`/`recurrence_rate` were ported in an undocumented-here earlier session; direct
  enumeration of every `pub fn` in `rust/observation_vector/src/lib.rs` against the real 18-name
  `ObservationData` constructor found 14 dims already covered before this one, not 12 — corrected
  in the crate-status table below). Unlike every prior dim, the math lived inside a stateful class
  method (ring-buffer snapshot + binarization + LZ76 count, all in one function body), not an
  already-standalone header — first extracted the pure compute (median-threshold binarization →
  LZ76 partition-count → `n/log2(n)` normalization) into a new `include/LempelZivComplexity.h`,
  leaving `InformationEngine`'s own circular-buffer bookkeeping (`GetLzWindowSnapshot()`, newly
  exposed) untouched; a native regression test (`tests/cpp/test_lempel_ziv_complexity.cpp`) proves
  the refactored class method produces byte-identical output to its pre-refactor body, not just
  "looks equivalent." Ported to Rust (`lempel_ziv_complexity`), FFI-wrapped,
  cross-language-parity-tested (full window + below-warmup + null-input cases, exact match to
  1e-12), and shadow-wired at its one real call site (`ContextManager.cpp`, "the unconditional
  sole authority" per its own comment — confirmed no second caller exists, unlike
  `fast_hurst_exponent`'s lesson above). 4 new Rust unit tests (58/58 total), 3 new cross-language
  parity assertions, 14 new native C++ checks. Both DLL configurations build clean:
  `MTS_WITH_RUST=OFF` (1,780,224 bytes — not byte-identical to the pre-refactor baseline, expected
  and correct here since, unlike every additive-only dim before it, this port's C++-side change is
  an unconditional production refactor, not code gated behind `#ifdef MTS_WITH_RUST`; behavioral
  identity is what the regression test proves, not DLL bytes) and `=ON` (2,031,104 bytes, links
  and grows). Live Sierra Chart confirmation deferred (pattern already proven live twice). **3 of
  18 dims now genuinely remain unported**: `log_scale_ratio` (`StudyHelperFunctions.cpp`'s
  `CalculateLogScaleRatio`), `tail_index` (Hill-estimator EVT math, not yet traced to one owning
  header), and `fast_mean_rev_z` (a genuinely distinct activity-clock function,
  `ActivityClockMeanRevZ` — not a second call site of the already-ported calendar-clock
  `mean_rev_z`, and its `FeatureScaler` calibration carries an unresolved "MOOT, DROPPED" vs.
  "wired 2026-09-17, EXPLICIT operator authorization" contradiction worth resolving before porting
  it).
- **`rust/schema` crate scaffolded, 2026-10-09**: generates from `schema/mts_schema.fbs`
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
- **`rust/execution` crate scaffolded + first port, 2026-10-10**: workspace member
  added (`rust/Cargo.toml`); first port is `ExecutionGate.h`/`.cpp` → `order_gate.rs` (spec §8.4,
  §8.7 D4 — literal behavior-preserving port, zero Sierra/zmq/pyo3/ibapi dependency). 18 unit tests
  (the C++ original's 4 `RunExecutionGateSelfCheck()` golden cases reproduced as `#[test]`s, plus
  14 new ones covering paths the C++ self-check didn't exercise — e.g. regime-gate precedence
  ordering, manual-vs-automatic model-staleness divergence). `cargo test --workspace` and
  `cargo clippy -p execution --all-targets` both clean (workspace lints: `clippy::all = deny`,
  `unwrap_used = deny`, `float_cmp = deny`). Not yet wired into `ffi`/the DLL — this is a pure
  port with no live call site changed yet, same first step already used for every
  `observation_vector` dim.
- **`RegimeManager` 8-file call-site migration DONE, 2026-10-10** (§4a): retired every genuine
  `HMMStateEnum` positional-identity read across `PositionManager`/`RiskManager`/`Scoring`/
  `TripleScreen3`/`Indicator`/`InferenceManager`/`Trade`/`TripleBarrierExitManager` in favor of the
  already-built, previously-zero-caller `RegimeManager` facade — closing the same label-switching
  bug class `lbrnet`'s own `STUDENT_T_HMM_MASTER_PLAN.md` found live in 3+ places, and de-risking
  `student_t_hmm`'s design (spec §5.2) by proving the rule/label-based consumption pattern against the
  *existing* HMM first. Real scope correction along the way: the initial 86-hit grep estimate was
  mostly false positives (legitimate continuous-field reads like `.Dof()`/`.Mahalanobis()`, not
  positional-identity branches); the true count was ~7 genuine consumer sites, all now migrated,
  plus one deliberately-deferred-then-completed hard site (`PositionManagerPatterns.cpp`'s
  hostile-regime-change check, which needed a new `RegimeSnapshot`-typed member pair, not a 1:1
  swap, since `RegimeSnapshot`'s constructor is `friend class RegimeManager`-only). Also found and
  deleted genuinely dead code in the same sweep: `Trade.h`'s `SetEntryRegime`/`GetEntryHMMState`/
  `GetEntryClimate` had zero callers anywhere in the repo (one had no `.cpp` body at all). Two full
  clean `./build_dll.sh` runs verify the whole migration; a real operational lesson surfaced along
  the way — `--no-clean` gave a false-positive "Build Successful" after a header-only edit (stale
  PCH silently reused) — now a standing rule in §4a: any header edit in this migration gets a full
  clean build, never `--no-clean`.

- **`mts_` prefix dropped from Rust crate/package/directory names, 2026-10-10**: `mts_ffi` → `ffi`,
  `mts_observation_vector` → `observation_vector`, `mts_schema` → `schema`, `mts_execution` →
  `execution` (the future HMM crate is named `student_t_hmm`, not `mts_hmm` or bare `hmm` — explicit
  correction, see spec §1.2). Scope, precisely: `Cargo.toml` package names, internal `use` paths,
  Rust-side doc comments/prose, directory names, and every doc reference across the spec/plan/four
  mirror docs (~103 occurrences swept). **Explicitly NOT renamed at the time** (deliberate, scoped
  out, resolved the next day — see the following bullet): the exported `extern "C"`/C-ABI symbol
  names (`mts_observation_vector_*`, `mts_abi_version`, `MtsStatus`). CMake option/variable names
  (`MTS_WITH_RUST`, `MTS_FFI_LIB`) and the shared cross-language schema file itself
  (`schema/mts_schema.fbs`, `mts_schema_generated.h`/`.rs` — flatc's own naming convention from the
  `.fbs` filename, unrelated to Rust crate naming) remain out of scope permanently, not just at the
  time. Verified: `cargo test --workspace` and `cargo clippy --workspace` both clean; full clean
  builds of both CMake presets confirmed (`MTS_WITH_RUST=OFF` default and `=ON` rust-linked, the
  latter confirming `ffi.lib` — not `mts_ffi.lib` — actually links).
- **`mts_` prefix also dropped from the exported C-ABI symbol names, 2026-10-11** (operator ruling,
  resolving the prior day's open question): `mts_observation_vector_*` → `observation_vector_*` (12
  functions in `rust/ffi/src/observation_vector.rs`), `mts_abi_version` → `abi_version`, the
  `MtsStatus` enum → `Status` (`rust/ffi/src/lib.rs`). Rationale recorded in spec §1.2: cbindgen's
  `namespace = "mts"` (`cbindgen.toml`) already wraps every symbol for C++ callers
  (`mts::observation_vector_sevcik_fractal_dimension(...)`), so the `mts_` prefix on the symbol name
  itself was redundant namespacing on top of a namespace that already does that job — not load-
  bearing C-ABI uniqueness. Updated the 3 live C++ call-site files (`ContextManager.cpp`/
  `StudyHelperFunctions.cpp`/`TripleScreen2.cpp`, 14 call sites) and the cross-language parity test
  (`tests/cpp/test_rust_observation_vector_parity.cpp`, 25 call sites + a stale `mts_ffi`-era build
  comment fixed as a drive-by). **Found and fixed a real pre-existing clippy gap as a side effect**:
  marking the 8 genuinely-pointer-taking wrapper functions `pub unsafe extern "C" fn` (required —
  `clippy::not_unsafe_ptr_arg_deref` was firing under `-D warnings`, pre-existing, not introduced by
  the rename) surfaced `clippy::missing_safety_doc`, fixed by adding a `# Safety` section to each;
  the 4 pure-scalar-arg functions (`compute_burstiness_index`/`compute_relative_range`/
  `compute_fisher_information`/`compute_amihud_illiquidity`) were correctly left non-`unsafe` after
  an initial overly-broad blanket edit was caught and reverted. `unsafe` on an `extern "C" fn` is a
  Rust-source-level safety-contract annotation only — it does not change the C-ABI, so no C++ call
  site needed any change for this part. Verified: `cargo test -p ffi --features observation_vector`
  and `cargo clippy -p ffi --all-targets --features observation_vector -- -D warnings` both clean;
  `cargo clippy --workspace --all-targets -- -D warnings` (default features) clean; the
  cross-language parity test recompiled natively against the renamed symbols and rerun (33/33
  assertions still pass); both CMake presets (`MTS_WITH_RUST=OFF`/`=ON`) rebuilt clean from scratch
  with the regenerated `mts_core.h`.
- **Pre-existing clippy debt found and fixed across the whole workspace, 2026-10-10** (standing
  directive: any issue found, regardless of who introduced it, gets recorded and fixed, not
  silently dropped). Found via `cargo clippy --workspace --all-targets -- -D warnings` — stricter
  than this initiative's prior invocations, which hadn't forced every warn-level lint to deny and
  hadn't been run per-crate with `--all-targets` on `observation_vector` specifically. Confirmed via
  `git log`/`git diff` that every item below predates this session's rename work (not introduced by
  it). Fixed:
  - `rust/observation_vector/src/lib.rs`: 9× `unwrap()` on `Option` in production code, all the
    identical `.sort_by(|a, b| a.partial_cmp(b).unwrap())` pattern (panics on NaN input) — replaced
    with `.sort_by(f32::total_cmp)`/`.sort_by(f64::total_cmp)` (stable since Rust 1.62; total order
    including NaN, no panic, matching the slice's actual element type at each of the 9 sites — 6
    were `f32`, 3 were `f64` scratch buffers, caught by a type-mismatch compile error after an
    initial blanket `f32` substitution).
  - Same file: 3× `clippy::excessive_precision` float literals (more decimal digits than `f32` can
    represent) truncated to clippy's own suggested minimal form (`1.58113883` → `1.581_138_8`,
    `2.8224230` → `2.822_423`, `0.5000000` → `0.5`) — cosmetic only, same runtime value.
  - Same file: 6× `clippy::float_cmp` (strict `assert_eq!` on floats) in tests asserting exact
    implementation-guaranteed sentinel/boundary returns (e.g. early-return `0.0` below minimum
    samples, clamped `-1.0`/`1.0` extremes) — not the result of arithmetic subject to rounding, so
    strict equality is the semantically correct check; added a single `#[allow(clippy::float_cmp)]`
    at the `mod tests` level with a rationale comment, rather than converting to a weaker
    tolerance-based check or scattering six individual allows.
  - Same file: 1× `clippy::useless_vec` (`&vec![100.0f32; 41]` → `&[100.0f32; 41]`).
  - `rust/ffi/build.rs`: 2× `unwrap()` on `Result` (`CARGO_MANIFEST_DIR` env var read,
    `cbindgen.toml` parse) → `.expect("...")` with a diagnostic message — build-script code, so
    panicking on these is correct behavior (no sensible fallback), the fix is purely "give the
    panic a message," matching clippy's own suggestion.
  - `rust/schema/src/lib.rs`: flatc-generated code (`generated/mts_schema_generated.rs`, never
    hand-edited) trips the plain rustc lint `mismatched_lifetime_syntaxes` (inconsistent elided vs.
    named lifetime spelling across `root_as_envelope` et al.'s own signature — a flatc codegen
    quirk, not a correctness bug) — added to the file's existing generated-code
    `#[allow(...)]` list (same established pattern already used there for `clippy::unwrap_used`
    etc.), since this lint isn't part of the `clippy::all` group that list already covers.
  - Verification: `cargo clippy --workspace --all-targets -- -D warnings` now fully clean;
    `cargo test --workspace` unchanged pass count (38 `observation_vector` + all others), no
    regressions; `include/generated/mts_core.h` regenerated as a side effect of running `cargo
    build` again (picked up an already-fixed-at-the-source but not-yet-regenerated stale
    `mts_observation_vector::` doc-comment reference from before this session's rename — harmless,
    doc-comment-only diff, confirmed via `git diff`).
  - **Operational lesson surfaced while re-verifying the `MTS_WITH_RUST=ON` preset**: running
    `scripts/build_rust.sh` with no arguments silently produces an `ffi.lib` with only
    `mts_abi_version` exported — `observation_vector` is an **optional** Cargo feature
    (`default = []` in `ffi/Cargo.toml`), so every call site relying on its FFI exports (6 real
    production sites across `StudyHelperFunctions.cpp`/`TripleScreen2.cpp`/`ContextManager.cpp`)
    fails at link time with "undefined symbol," not at build time — caught by the full rebuild this
    fix pass required, not by any faster check. Correct invocation:
    `scripts/build_rust.sh --features observation_vector`. **New standing rule: never run
    `build_rust.sh` bare when verifying the Rust-linked preset — always pass the full feature list
    the DLL actually needs**, same spirit as the existing PCH-staleness rule in §4a.
- **Two real spec-document bugs found and fixed, 2026-10-11** (same standing directive): the spec's
  own section numbering had a duplicate — `## 10. Risks` and `## 10. Not yet analyzed` both
  numbered 10 (with `## 11. Documentation sync` following, so the whole tail was off by one),
  renumbered to 10/11/12 respectively. Two stale cross-references in spec §1.2/§1.4 pointed to
  "§10" for the indicator-computation component spec, which is actually §9 — both corrected.
  Checked the plan and all four mirror docs for any cross-reference to the old numbers; none found,
  so no further fallout.
- **`-ffast-math` found to silently break this codebase's NaN-sentinel carry-forward convention —
  investigated, confirmed, and removed, 2026-10-11** (spec §2 item 11, §10 item 5 has the full
  write-up). Operator asked "tell me about fast-math" (decision-register item 5 from the prior
  open-questions inventory); investigating it surfaced a real, concrete correctness risk, not just
  the generic "numeric policy/tolerances" framing the spec previously carried. `CMakeLists.txt` had
  enabled `-ffast-math` unconditionally for the whole DLL (`-O3 -march=native -ffast-math -mavx2`,
  both presets) since before this initiative started. Empirically verified with the exact clang-22
  toolchain and exact repo flags, using a minimal repro matching this codebase's real
  runtime-NaN-sentinel pattern: `-ffast-math`'s `-ffinite-math-only` component makes the compiler
  assume no value is ever NaN/Inf and fold `std::isnan()` to `false` unconditionally (Clang itself
  warns `-Wnan-infinity-disabled: use of NaN is undefined behavior due to the currently enabled
  floating-point options` at exactly this kind of call site). This directly threatens the "return
  NaN for a degenerate window, caller carries the last valid value forward" convention used
  throughout `RobustMoments.h` (`MoorsKurtosis`/`BowleySkewness`'s own internal carry-forward
  checks), `StudyHelperFunctions.cpp`'s Hurst/fractal-dim carry-forward, and every Rust-FFI
  shadow-wire comparison this initiative has built — all pre-existing, predating any Rust work,
  found only because this initiative's own numeric-policy question prompted the investigation.
  **Fixed**: `-ffast-math` removed from `CMakeLists.txt` (operator decision, kept `-O3 -march=native
  -mavx2`); re-verified `isnan()` correctly reports `true` again with the post-fix flags, no
  compiler warning; both CMake presets (`MTS_WITH_RUST=OFF` and `=ON`, the latter rebuilt fully
  clean from a deleted `build-windows-rust/`) rebuilt successfully. **Not yet done, explicitly
  flagged, not silently dropped**: no hot-path performance re-benchmark exists (this initiative has
  no latency budget established at all yet — §11's own listed gap, not introduced by this fix); no
  live Sierra Chart re-confirmation that the degenerate-window carry-forward branches now actually
  trigger on real flat/thin data (build-verified, not yet runtime-observed).

**Not yet started (the next real pieces of work, in rough order):**
1. `rust/student_t_hmm`: the Student-t HMM lifecycle (train → posteriors → live inference) — spec §5.
   This is the big one; most of the rest of this initiative (the `.context.parquet`/`.alpha` goal in
   spec §5.4, the execution-layer "all Rust" scope in spec §8) explicitly depends on it landing first.
2. Execution layer, broker connectivity + computations (`RiskManager`/`PositionManager`/
   `ExecutionGate` + direct IBKR via `ibapi`/`tokio`, replacing Sierra's `SCTradingInterface`
   routing) — scope confirmed and deepened 2026-10-09/2026-10-10, full design in spec §8 (folded in
   2026-10-10 from a standalone doc, same day, per the one-spec-one-plan rule above). Crate:
   `execution` (§3, W12 below). **Next concrete action**: W0e, the MES-futures IBKR-algo
   what-if spike (spec §8.8) — de-risked by teammate research (Adaptive/TWAP/VWAP/Balance Impact
   and Risk all documented as supporting CME futures) but not yet empirically confirmed against
   this account; needs a reachable TWS/Gateway, which this dev environment does not have.
3. `rust/transport` — deliberately last for its original six-port scope (removing ZMQ port 5561 via
   the HMM port removes the hardest socket `transport` would otherwise have to handle) — **except
   its skeleton + two brand-new ports** (the execution-intent channel, the position/fill feed),
   which W12 needs much sooner and may pull forward (W9 row below).
4. **Indicator computation** (the ~38 `IndicatorStore`/`BaseIndicator`/`Indicator<T>` leaf classes'
   actual compute logic — MACD, RSI, Kangaroo Tail, Turtle Soup, NR7, stochastic, etc.) — a third
   Rust pillar, ruled 2026-10-10, scope only (spec §9). This is the same rewrite `docs/superpowers/
   plans/2026-08-05-indicator-manager-dod-soa.md` deferred as "Phase III" and declared permanently
   out of scope for C++ (that plan is now marked partially superseded, same reason: it predates
   Rust adoption). Not yet sequenced, not yet given a crate name or a `W`-number — needs its own
   design pass (crate placement, interaction with the existing packed-array read-side, per-
   indicator porting order) before any code starts.

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
| W0e | MES-futures IBKR-algo what-if spike (bare `ibapi` binary, no module skeleton) | — | De-risked by teammate documentation research (spec §8.7 D3), not yet empirically run — needs a reachable TWS/Gateway |
| W1 | Merge into `VSCode/MindfulTrader/` | W0 pre-work | Deferred (§1.2) |
| W2 | `rust/` workspace bootstrap (toolchain, cbindgen, DLL link, CI) | P2/P3, W0s | **Mostly done** — `rust-toolchain.toml`, workspace, `ffi` skeleton all exist and are proven; not yet done: `install_py_ext.sh`, `check_all.sh`, CI |
| W3 | `student_t_hmm` inference + regime engine | W2, P9 | Not started |
| W4 | Offline posteriors tool | W3 | Not started |
| W5 | `observation_vector` port, dim by dim | W2, P8 | **10 of ~18 dims done** (every genuinely self-contained one, confirmed by direct code read 2026-10-09, not just the first 5 initially found); remaining dims have real `FeatureScaler`/dim-index coupling |
| W6 | In-process shadow mode in a full `BackTesterStudy` replay | W3, W5 | Not started |
| W7 | Event-schema cutover: HMM output rides the event; Transformer reads it | W6 | Not started |
| W8 | Rust training | W6 | Not started |
| W9 | `transport`, port by port | W7 (5561 gone), W2 | Not started; deliberately last for the original 6-port migration — **but its skeleton + 2 brand-new ports (execution-intent channel, position/fill feed) may land earlier, pulled forward for W12 below (spec §8.7 D1); those 2 ports have no existing live peer to regress** |
| W10 | Deletions + mirror-doc updates + `PRODUCTION_TRIAGE.md` rows | W6-W9 as each lands | Ongoing as work lands |
| W11 | `ContextManager.cpp` port | W5, W6 | Not started; later, own spec when it comes up |
| W12 | `execution`: IBKR broker connectivity + order lifecycle, port by port (spec §8.4's module list) | pure sub-modules (order_gate, stop_manager, trap_exit): none, same as observation_vector dims; broker-connectivity sub-modules (ibkr_session, order_watcher, signal_feed, intent_client, position_feed): W0e, W9's pulled-forward skeleton+2-ports slice | **`order_gate.rs`/`stop_manager.rs`/`trap_exit.rs` all ported, 2026-10-10/11** — `order_gate.rs` (`ExecutionGate.h`/`.cpp`), 18/18 tests; `stop_manager.rs` (`TripleBarrierEngine.h`'s `ComputeBarriers`, all 7 golden vectors from `tests/fixtures/triple_barrier_golden_vectors.json` reproduced exactly, plus 2 new edge-case tests), 9/9 tests; `trap_exit.rs` (`TripleBarrierExitManager.h`'s bracket lifecycle — `open_bracket`/`evaluate`/`current`/`close`, no pre-existing C++ test existed for this state machine specifically so 16 new tests were written directly from the C++ logic, incl. first-hit-wins race-order precedence: regime-invalidation > time-exit > stop > target), 16/16 tests. 43/43 total, clippy clean (`cargo clippy -p execution --all-targets -- -D warnings`). All three are pure ports, not yet wired into `ffi`/the DLL — same "port first, wire later" bar as every `observation_vector` dim. Broker-connectivity sub-modules still blocked: D2 (backtesting `BrokerAdapter`) and D9 (deployment topology) need resolving first — spec §8.7. **Bracket order construction verified against the real `ibapi` 5.0.0 crate, 2026-10-11, premise corrected same day** (spec §8.7 D3a): `stop_manager.rs` places a static, entry-anchored stop/target bracket (`ComputeBarriers`'s output, immutable once placed) via the crate's own `.bracket()` convenience builder — no trailing-stop/`modifyOrder`-on-ATR-change mechanism is needed; an earlier draft of this pointer assumed a Chandelier-style continuously-trailing stop, which is stale (`ChandelierStopManager` was already deleted from the codebase, see spec §8.3) |

**Critical path**: `P2/P3 + W0s → W2 → W3 ∥ W5 → W6 → W7 → W9 → W10`, with W4 and W8 branching off
W3/W6. W12 branches off W0e + W9's pulled-forward slice — it does not block, and is not blocked by,
the main `W3 ∥ W5 → W6 → W7` chain. Rust-free work that can start any time: P1, P2, P6, P7, P8, P9.

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
| 9 | Vendored `zmq` crate doesn't conflict with the DLL's existing vcpkg libzmq | Not attempted; revisit when `transport` actually needs zmq |

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
| `ffi` | Skeleton shipped, 2026-10-08; `student_t_hmm` feature wired 2026-10-12 | One staticlib, features `observation_vector`/`student_t_hmm`/`transport` turn subsystems on |
| `observation_vector` | 15 of 18 dims ported (corrected count, 2026-10-12 — the prior "12" figure was stale); remaining: `log_scale_ratio`, `tail_index`, `fast_mean_rev_z` | spec §6 / §5 below |
| `schema` | Scaffolded 2026-10-09 | Generates from the now-single `mts_schema.fbs`; see spec §4.2 for the merge that simplified this |
| `student_t_hmm` | Substantially built (model/train/init/registry/artifact/audit/gate-metrics/regime_engine/runtime_calibration/calibration/bootstrap all shipped — every lifecycle module in spec §5.2's target architecture now exists); full K-selection not started (deprioritized); first real (non-synthetic) model artifact now on disk at `fixtures/hmm_model_20260918_manifest.json` (decision #34) | Spec §5; plan §4 Stages G0-G6 all done (every G0 blocker resolved, decisions #30/#32/#33/#34), G7 (C++ cutover) not started |
| `transport` | Not started | Deliberately last; spec §7 — except skeleton + 2 new ports, see W9 row above |
| `py` (`mindful_core`) | Not started | PyO3 extension for `lbrnet`/`GUI` |
| `execution` | 3 of ~5 pure sub-modules ported (`order_gate.rs` 2026-10-10, `stop_manager.rs`/`trap_exit.rs` 2026-10-11) | `rust/execution/` — spec §8; W12 above. Broker-connectivity sub-modules (`ibkr_session`/`order_watcher`/etc.) still blocked on W0e (algo what-if spike, needs reachable TWS/Gateway) |

**Principles governing every crate** (established 2026-10-08, proven in practice across all five
`observation_vector` ports so far): pure logic crates have zero Sierra/zmq/pyo3 dependency; the FFI
boundary is the only `unsafe` surface; every port goes through the proof pattern in §5.2 before any
production call site reads its result.

---

## 4. HMM lifecycle execution stages (spec §5's companion)

Stage A (golden harness, Python-only, P9) → Stage B-C (Rust inference + regime engine, matched against
the golden) → Stage D (offline posteriors tool) → Stage E (in-process shadow mode in a full
`BackTesterStudy` replay, zero divergence gate) → Stage F (event-schema cutover) → Stage G (Rust
training) → Stage H (deletions: Python HMM code, port 5561, mirror-doc updates).

Full per-stage detail to be filled in here when this work starts (not yet — §0 confirms `student_t_hmm` has
not been started).

**Data-flow ruling, 2026-10-10 (spec §5.4, now settled, not just directional)**: training and live
inference both happen natively (Rust) — training offline via `rust/tools`' `hmm_tool`, live inference
in-process via `ffi`'s `hmm_step()`, never on the Python/lbrnet side again. Whatever HMM info the
Transformer needs *to train* gets written directly into `.alpha` (collapsing today's two-phase offline
pipeline — collect, then a separate Python pass backfills posteriors — into one pass, once the HMM is
callable in-process from `EventDataCollectorStudy.cpp`). Whatever HMM info the Transformer needs *to
infer live* rides on the existing Event FlatBuffer over ZMQ, exactly as §5.2 already specified. Consumer
3 (GUI live display of the raw observation vector) remains explicitly unresolved by this ruling — see
spec §5.4's own caveat.

**Stage A / P9 status, verified 2026-10-10**: checked `lbrnet` directly — no golden-harness/neutral-
artifact-exporter work exists yet (confirmed by search, not assumed stale from an old doc). This is the
real, current blocker for Stage B-C (`student_t_hmm`'s actual inference code, proven against a real trained
model) — but it does not block the genuinely golden-independent slice of `student_t_hmm` (pure math: log-
sum-exp, the weighted Student-t log-density formula; pure state-machine logic: the hysteresis/registry/
alert-escalation `state_manager.rs` module, spec §5.2) — those are hand/synthetic-testable today, with
golden-parity validation deferred to whenever P9 lands.

**Training-data access into Rust: zero, confirmed 2026-10-11** (operator: "have we brought all the
data needed for Student-t HMM training into Rust?"). Not just "`student_t_hmm` hasn't been written" —
checked directly: `rust/Cargo.toml`'s workspace has no `parquet`/`arrow`/`ndarray`/numpy-compatible
dependency anywhere, so no Rust code today can read any of the existing data formats (`.context.parquet`,
`.alpha`, `.context.posteriors.npy`). The two `.context.parquet` files that exist on disk
(`lbrnet/data/raw/offline_replay_*.context.parquet`) were produced by an existing Python offline
conversion step, not by the RULED "write `.context.parquet` directly at collection time" design (§5.4) —
confirmed that design isn't implemented either: zero `.parquet` references anywhere in
`EventDataCollectorStudy.cpp`/`LBRFileManager.cpp`. No model-artifact format (replacing
`models/hmm_model.pkl`, spec §5.2's manifest+weights-file proposal) exists yet either. `rust/tools`
(the planned `hmm_tool` CLI, spec §5.2) doesn't exist as a workspace member. Every piece of this —
golden harness (P9), data-format access, artifact format, training CLI — is unstarted; this isn't a
partial gap, it's the full prerequisite stack ahead of any `student_t_hmm` work.

**Reference implementation found for the "`.context.parquet` directly at collection time" design,
2026-10-11** (operator: "does the `tools` folder have anything we can use for this?") —
strengthens, not replaces, the gap above: `tools/market_data_replay/MarketDataReplay.cpp` already
implements the exact pattern ("no `.context` intermediate," chunked Arrow writer straight from
C++), `tools/context_pipeline/context_to_parquet.cpp` supplies the exact target column schema, and
the real interception point is confirmed as `src/EventDataCollectorStudy.cpp`'s
`LBRFileManager::Instance().LogSynchronizedEvent(...)` call site. Full writeup: spec §5.4. This is
still a C++ reference (mamba `mts` env's Arrow/Parquet, not Rust) — the "zero `parquet`/`arrow` in
`rust/Cargo.toml`" gap above is unchanged — but it means G7's integration/cutover work (Stage G
sequencing, below) has a working pattern to port from rather than a cold design.

**`HMMClient` obsolescence + concrete `hmm_step` call-site design, 2026-10-11** (operator question:
"do you see that HMMClient may not be applicable... we need an institutional-grade way to feed the
observation vector directly to the Student-t HMM when Mahalanobis tells us to"). Confirmed by direct
trace of `HMMClient.cpp`'s real current body (not just the port-5561-deleted line already in the
decision register): the whole 966-line class — DEALER socket, worker thread, sequence-ID staleness
rejection, timestamp-drift validation, FlatBuffer envelope deserialization, host failover — exists
solely to handle cross-process async IPC and is fully obsolete (not just its socket) once `hmm_step`
is a synchronous in-process call. Concrete replacement traced against the real call site
(`ContextManager::EmitLiveContext`/`EmitTrainingContext`): `HMMClient::Instance().RequestUpdateAsync(
...)` is replaced by a direct synchronous `hmm_step(...)` call fed the *already*-Mahalanobis-gated
observation vector `otg::ObservationTriggerGate` already computes today — no new trigger logic.
`risk_multiplier` stays a downstream-computed quantity (`RiskManager::ComputeInstitutionalRiskMultiplier`),
not part of `HmmStep`'s own raw output, preserving the existing separation of concerns. The Python-side
"atomic regime-triggered re-inference" piggyback (today's `reinfer_action_id`, needed because HMM and
Transformer inference happen in the same Python message today) is **not replaced by new machinery** —
it's structurally eliminated by two already-existing/already-ruled pieces: the "Transformer coupling,
inverted, one-way" design (spec §5.2, Transformer reads regime off the event stream instead of running
its own inference) and `HmmStateIndicator::IsDirty()` (`include/Indicator.h`, already means "did regime
change since the last `SetState`" — zero new field needed in `HmmStep`'s contract). Full design,
including the init/step/reset opaque-handle lifecycle rationale (the model's Hamilton-filter belief and
model-risk state are genuinely stateful, unlike every `observation_vector` dim): spec §5.2. **Deletion
candidate once wired in**: `HMMClient.h`/`.cpp` in full — **open, not yet verified**: whether anything
besides `HandleBinaryResponse` needs the `RiskStateUpdate` message shape itself (e.g. a GUI subscriber),
needs a consumer sweep before the deletion step. This is design only — `student_t_hmm` itself has not
started (Stage A/P9 still blocks Stage B-C), so none of this is implemented yet.

**Consumer sweep done, GUI-display question answered, 2026-10-11** (spec §5.4 consumer 3, full
write-up there): the "needs a consumer sweep" open item directly above is resolved for the GUI case —
traced `MTS/zmq_client.py`/`regime_view.py`/`live_agent_view.py`/`websocket_broadcaster.py` directly.
Bigger finding than expected: **MTS itself runs the live Python HMM inference server today**
(`zmq_client.py`'s `_hmm_router_loop` binds the port-5561 ROUTER `HMMClient` talks to, calls
`self.live_agent.predict_hmm(...)`, replies with `RiskStateUpdate`) — so this entire MTS-side serving
path (`_hmm_router_loop`, `handle_hmm_flatbuffer_message`, `_predict_hmm_from_feature`,
`_build_risk_state_envelope`, `feature_spine`'s MO+SS pairing) is a second, MTS-side deletion
candidate alongside C++'s `HMMClient`. Found 3 display fields that silently break or stop making
sense post-migration as currently implemented (not just "no longer needed"): `observation_vector`'s
display is sourced exclusively through the now-obsolete `MarketObservation` delivery, even though
`Event.observation` already exists on the main stream and MTS simply never reads it today;
`burnin`/`burnin_remaining` measures MTS's *own Python-process* restart, not anything about the new
source of truth; `source`'s multi-path meaning disappears once there's only one path. **Proposed
fix, operator-approved ("we can change the schema... to conform to our new direction")**: additive
`RegimeStatus` struct on `Event` (climate/entropy/entropy_velocity/drift-alert-count/burnin, all
rule/label-derived — never the raw HMM `state_id`), replacing `MarketObservation`/`RiskStateUpdate`
for this consumer entirely. Deliberately excludes `transition_risk`/`dof`/`mahalanobis`/`tail_weight`/
`expected_duration`/`probability_vector` — confirmed zero display consumers for any of them.
Side finding, not yet acted on: `MarketClimate` is hand-duplicated (`include/Indicator.h` C++ enum,
`lbrnet.models.regime_registry.MarketClimate` Python, not schema-generated) — same risk class as
`HMMStateEnum`; flagged for the implementation step (migrate to a schema enum alongside adding
`RegimeStatus.climate`), not decided yet, not blocking this design. Still open: a *broader* sweep of
whether any other `lbrnet` consumer (beyond MTS) needs `RiskStateUpdate`. **Sequencing decision**:
the actual `.fbs` edit is deliberately not done yet — nothing to populate `RegimeStatus` with until
`student_t_hmm` lands (Stage A/P9 still blocks Stage B-C, above); implementation (schema edit +
`regenerate_schema.sh` across all three repos + the MTS-side deletions +
`MarketClimate`-schema-enum call) happens alongside that work, not before it.

**Stage G (Rust training) scope expanded massively, 2026-10-11** (operator: "Python will not [be]
involved in the training of the Student-t HMM... update the spec and plan with the porting of
Python HMM-related code to Rust... consider refactoring that can potentially elevate the Rust
implementation to a more institutional level"). This resolves spec §5.5's former item 5 (whether
Meta-HMM calls the trainer as subprocess or import — moot, see below) and is a strictly larger
scope than Stage G's original framing ("Rust training" read narrowly as "a Rust equivalent of
`train_student_t_hmm.py`'s EM loop"). Full inventory, verdicts, the "L_MAP
decline" bug chain, the ~20-test correctness checklist, and the institutional-elevation candidate
list: **spec §5.1a** (new section) — this plan entry is the task-tracking pointer, not a
duplicate of that content.

Four parallel background research agents surveyed the complete `lbrnet` Python HMM surface (core
model, registry/classification, control-plane/calibration scaffolding, ~3,150 lines of
architecture docs + ~20 regression tests) and Atratus's existing Rust Student-t HMM
(`sensor_core/src/hmm.rs` + its spec + `knowledge/hmm_model_validation.md`) was read directly for
inspiration (read-only prior art, no code/data link to MindfulTrader). Headline findings
(relabeled F1-F6, 2026-10-11, to free the G-prefix for the dependency-ordered sequencing
immediately below — same content, no substance change):

- **F1 — canonical taxonomy ruling**: `MarketClimate` (5-state, `regime_registry.py`) is canonical;
  the legacy `HMMStateEnum` (4-state) is being retired in `lbrnet` itself and its consumers
  (`regime_manager.py` Python, `regime_focal_loss.py`, `hmm_context_features.py`) need remapping
  or explicit out-of-scope calls, not silent carry-forward. Consistent with decision #16's already-
  ruled `RegimeStatus` schema design.
- **F2 — port scope, file by file** (full table: spec §5.1a): port `student_t_hmm.py` (core EM/
  filter math), `regime_engine.py` (live stateful engine), `regime_registry.py` (classification),
  `regime_gate_metrics.py` (port **first** — cleanest, lowest-risk, highest-reuse), the
  methodology (not file shape) of `train_student_t_hmm.py`, `hmm_parametric_bootstrap.py` (fixing
  its verbatim duplication of `regime_gate_metrics.py`'s tenure helper in the same pass),
  `materialize_hmm_features.py`/`regime_calibration.py` (confirmed must-exist-after-port),
  `calibrate_hmm_gate_thresholds.py`'s methodology (flagging the `arch`-crate gap, §5.5 item 11),
  `audit_hmm_release.py` (port as-is, trivial). **Drop entirely**: the 4-file Meta-HMM "trainer run
  plan" sub-cluster (confirmed orphaned, deleted from `lbrnet` 2026-10-11 — this is what resolves
  the former §5.5 item 5), the 3 benchmark files, `regime_focal_loss.py` (Transformer-training
  scope, not HMM). **Correction, 2026-10-11**: a separate 5-file Meta-HMM "recommendation/ledger"
  sub-cluster was originally lumped into the same "orphaned, drop" verdict — it's actually live,
  feeding `risk_gate_orchestrator.py`; needs its own port/keep decision, not dropped (spec §5.1a).
  **Likely drop, pending a real measurement**: `student_t_hmm_gpu.py` (screening-only, no dof
  support, 20-33x slower than CPU per the runbook's own numbers — Rust CPU EM may make this moot).
- **F3 — the "L_MAP decline" bug chain is mandatory reading before writing the Rust EM loop**: four
  distinct historical covariance/DOF M-step bugs (density normalization, τ/σ² conversion, ECME
  staleness, min_covar/global_var anchor coupling) plus one genuinely unresolved residual (item 15,
  must be carried forward as an open question, not silently closed) and a separate covariance
  denominator bug. Full account: spec §5.1a.
- **F4 — parity gate is the ~20-file test checklist**, not a vague "matches the math" goal (spec
  §5.3's update). Every named test needs a Rust equivalent before Stage G is parity-complete.
- **F5 — institutional-elevation candidates** (none decided, all flagged for a future prioritization
  pass): Atratus-inspired extreme-anomaly numerical-stability stress test (required, not optional);
  Brent's-method DOF root-finder (hand-rolled, matching Atratus's own approach); `Sn`/`Qn` robust
  estimators (Rousseeuw & Croux 1993) as a mass-point-robust MAD alternative, directly motivated by
  this repo's own `burstiness_index`/`amihud_illiquidity` history; artifact-serialization
  reconsideration (`bincode` vs. the manifest+raw-floats hybrid) now that Python may never read the
  trained model directly; an explicit Rust trait replacing `RegimeEngine`'s Python duck-typed model
  interface.
- **F5a — Elite Feature Set Curation's Phase 2b worth revisiting once `model.rs` lands, 2026-10-11**:
  Phase 2b (an exact joint Feature-Saliency/Student-t EM) was ruled out 2026-09-14 as
  **mathematically** intractable (`docs/superpowers/specs/2026-08-31-elite-feature-set-curation-
  initiative.md`'s own forward-looking note) — that doesn't change in Rust. What does change: the
  two real alternatives that ruling identified (Variational Bayes; an L1/LASSO-penalized M-step)
  become far cheaper to prototype once the real Student-t HMM math and the feature-saliency tooling
  (`FeatureSaliencyEM.h`/`FeatureSaliencyEval.cpp`) share one language instead of a Python/C++
  cross-language bridge. Not scoped, not started — a reason to revisit the decision to pick it up,
  tracked here so Stage G's own landing doesn't silently miss the connection.
- **F6 — defaults reconciliation — RESOLVED 2026-10-11 (decision #26, spec §5.1a)**:
  `dof_learning_rate` (0.02 vs. 0.15) confirmed **dead code** (`_update_dof_ecme()` never reads it;
  no legacy path exists) — dropped from the Rust port entirely, no field to carry. `bullet_weight`
  (1.40 vs. 1.30) confirmed real and consumed, but never a fixed constant — `train_student_t_hmm.py`
  BIC-grid-searches it per run, so `model.rs`'s existing per-instance `feature_weights` field (not
  a crate constant, mirroring the `dof_min`/`dof_max` fix) already handles it correctly; only G5's
  own grid-search default range remains open, deferred to G5 itself.

This is design/inventory only — nothing above has been implemented; Stage A/P9 still blocks Stage
B-C, which in turn precedes Stage G in the stage ordering at the top of this section.

**Stage G internal sequencing — dependency-ordered task breakdown, 2026-10-11** (operator: "split
it" — F1-F6 above is a flat inventory of findings/verdicts, not an execution order; this turns it
into one). Organized by what blocks what, not by file-list order. Nothing here is started.

- **G0 — blocking decisions (no code; resolve before the dependent sub-stage below, not during it)**:
  (a) the still-open `HmmStep` state-identity shape (§5.2 — rule/label-based vs. raw positional
  `state_id`) gates G4's external API, not G2's internal math; (b) **RESOLVED 2026-10-11
  (decision #26, spec §5.1a)** — F6's defaults reconciliation: `dof_learning_rate` dropped entirely
  (confirmed dead code); `bullet_weight` needs no reconciliation (`model.rs`'s existing
  per-instance `feature_weights` field already correct) — only G5's own BIC-grid default range
  remains, not a G0 blocker anymore; (c) **RESOLVED 2026-10-11 (decision #28, spec open question
  #14)** — the artifact serialization format call: `bincode` rejected (confirmed real,
  non-retiring Python consumers still need raw read access — `train_event_transformer.py`/
  `tune_event_transformer.py`'s `sigma_eff` feature), simplified further than originally proposed
  to one plain JSON manifest (no separate binary weights file — the ~190-float total parameter
  count at `PRODUCTION_D`/`PRODUCTION_K` doesn't earn that complexity); (d) **RESOLVED 2026-10-11
  (decision #25, spec §5.1c)** — the
  climate/threshold-constants placement call: false dichotomy, both (manifest-backed + mandatory
  train → audit (GO/NO-GO) → auto-calibrate chain for the 4 Tier-1 constants + `TAIL_RISK_THRESHOLD`,
  porting the already-shipped `calibration_artifacts_provenance.json` atomic-write-together pattern
  from `lbrnet`'s Transformer pipeline rather than a silent-fallback `path.exists()` check; plain
  struct-field defaults, no manifest slot, for the 6 Tier-3 constants). This sharpens G3/G4/G6's
  scope: `artifact.rs` must also write the provenance sidecar atomically with the calibration
  artifact, and G4's constant-loading design hard-refuses stale/missing-provenance artifacts
  rather than falling back silently; (e) the `arch`-crate research spike (§5.5 item 11) gates G6's
  `calibration.rs` beyond a stub — this one can run latest of the five, since calibration is a
  late-stage concern either way.
- **G1 — standalone, no-model-dependency ports (correction, 2026-10-11: these do NOT wait for
  Stage A/P9 — they're exactly the "golden-independent slice" already noted above; can start now,
  parallelizable with each other and with G0)**: `audit.rs` (zero dependencies, trivial,
  deterministic JSON-in/JSON-out); `gate_metrics.rs` (operates on labeled state-sequence arrays —
  testable against synthetic/hand-built sequences, doesn't need a real trained model). Lowest risk,
  highest reuse, per F2 — the intentional first real code in this crate.
  - **SHIPPED, 2026-10-11**: both ports land. `audit.rs` adds `serde`/`serde_json` to the crate
    (and the workspace dependency table) — justified here, unlike `model.rs`'s own
    zero-dependency math: this is once-per-training-run offline tooling, not hot-path code, so
    the heap-allocation discipline that ruled out `ndarray` for `model.rs` doesn't apply.
    `gate_metrics.rs` ports every function except `extract_means_by_state()` (a Python
    API-shape-ambiguity problem — `(K,D)` vs `(K,1,D)` `means_` arrays — that doesn't exist for
    `StudentTHmm<D,K>`'s own always-`[[f64;D];K]` field); `compute_pareto_thrust_shares`'s
    signature deliberately diverges from the Python original (takes the already-extracted
    per-state `relative_range` component directly, not `(counts, means, idx_rel_range)`, since
    this crate doesn't embed a dimension-index convention). 27 new tests (10 `audit.rs`, 17
    `gate_metrics.rs`). Workspace: 135/135 tests pass, clippy clean.
- **G2 — core math (`model.rs`): highest risk, most other sub-stages depend on it**. Emission
  density, Hamilton forward-backward, Viterbi decode, predict. Must embed the F3 "L_MAP bug chain"
  lessons as tests written *alongside* the port, not bolted on after (density normalization, τ/σ²
  conversion, covariance-update denominator, ECME DOF staleness, min_covar/global_var coupling,
  the one-sided relative-convergence check), plus the Atratus-inspired extreme-anomaly stability
  stress test (F5). Pure math is hand/synthetic-testable now per the existing Stage A/P9 note;
  golden-parity validation against a real trained model's output is deferred until Stage A lands.
  Depends on: G0(c) for anything beyond an artifact stub. **k-means++ initialization should start
  from `tools/observation_vector/FeatureSaliencyEM.h`'s existing C++ port of
  `student_t_hmm.py`'s own `_kmeans_plusplus_seed_indices`** (found 2026-10-11, spec §5.1a
  elevation item 8) rather than re-deriving it from the Python source cold.
  - **Inference-only slice SHIPPED, 2026-10-11**: `rust/student_t_hmm` crate scaffolded;
    `model.rs` ports `log_emission_prob`/`compute_tail_weight`/`stable_posterior`/`forward_step`/
    `predict`/`decode` against MindfulTrader's real weighted formula (checked side-by-side against
    Atratus's simpler unweighted `hmm.rs` — not copied: the Jacobian-correction term, "true `D`
    not `sum(w_j)`" density-normalization fix, and `log1p` precision are all MindfulTrader-only).
    `D`/`K` are const generics (zero-dependency fixed-size arrays, not `ndarray` — a dim-count
    change is a one-line constant edit plus the same rebuild every other change already requires,
    per the operator's own "is recompiling that expensive?" challenge, 2026-10-11). `ln_gamma`
    hand-rolled (Lanczos approximation) rather than adding a crate dependency for one function.
    8/8 new tests pass, incl. the exact `student_t_hmm.py`-documented D=2/w=[1.4,1.0]
    density-integrates-to-1 regression and an extreme-anomaly stability stress test (rewritten
    natively, not copied, from Atratus's own). Training (EM/M-step/DOF estimation) not started —
    Atratus's own `rust/src/trainer.rs` (a real 610-line Baum-Welch implementation I had not
    previously checked) reviewed for this next: its DOF root-finder is a weaker, un-bracketed
    Newton's-method fallback than both MindfulTrader's `scipy.optimize.brentq` and Atratus's own
    stated spec — do not copy it; hand-roll real Brent's method instead (already on the elevation
    list). Its never-materialize-the-ξ-tensor memory pattern and mean-weighted/covariance-
    unweighted M-step denominator split (independently matching our own L_MAP item 8 fix) are
    worth reusing. Workspace: 108/108 tests pass, clippy clean.
  - **Training slice (`train.rs`) SHIPPED, 2026-10-11**: EM/M-step and the ECME degrees-of-freedom
    update land, ported from `fit_streaming()`'s M-step (the single most bug-prone region per F3's
    8-bug "L_MAP decline" history) — sequence-level log-space forward-backward (`forward_backward`,
    matching `_forward_backward_numba`'s own max-subtraction scaled recursion exactly), the
    weighted M-step (startprob/transmat/means/covars, including the full mean-shrinkage-prior
    cross-term and the deliberately-UNweighted covariance denominator — re-confirmed fresh against
    Atratus's own `trainer.rs` M-step on the operator's direct request this session, which
    independently uses the identical `combined_weight_sum`-for-mean/`resp_sum`-for-covariance
    split despite being a wholly separate implementation, but does NOT replicate the ECME-
    staleness-correction second pass below — confirms that's a MindfulTrader-specific refinement,
    not something to inherit "for free"), and the two-pass ECME DOF update (`recompute_post_mstep_
    dof_stats` recomputes Mahalanobis deltas under the just-updated mean/covariance while reusing
    the first pass's unchanged `gamma`, exactly matching `_update_dof_ecme`'s own staleness-fix
    comment). `digamma` and a real bracketed Brent's-method root finder (`brent_root_find`,
    Brent 1973) are hand-rolled — not Atratus's weaker fixed-10-iteration unbracketed Newton's-
    method fallback, confirmed deliberately not copied (same "no crate dependency for one special
    function" convention as `model.rs`'s own `ln_gamma`). Decision #26's two findings are both
    directly exercised here: `dof_learning_rate` needed no field at all (confirmed dead code);
    `bullet_weight` needed no fix (already correctly absorbed into `model.rs`'s per-instance
    `feature_weights`, exercised by this slice's own golden test). **Golden-parity validated
    against two real invocations of the actual Python `StudentTHMM.fit_streaming()`** (mts conda
    env, 2026-10-11, not independently-rederived "looks plausible" test data) — a tiny
    deterministic 8-row/D=2/K=2 fixture, once with every MAP prior disabled and once with every
    prior active (`mean_prior_kappa`/`covar_prior_eta`/`trans_prior_alpha`/`sticky_diag_alpha`/
    `bullet_weight` all non-zero, deliberately exercising the cross-term-heavy covariance-prior
    formula the bug chain centers on) — both match Python's own printed output to the full `f64`
    precision captured. 13 new tests (digamma/Brent unit tests, one-sided convergence-check
    tests, 2 golden-parity tests). Workspace: 141/141 tests pass, clippy clean.
  - **Orchestration loop (`fit()`) SHIPPED, same session**: completes `train.rs` from a bare
    single-iteration M-step into an actually-usable "train to convergence" entry point —
    `compute_log_prior` (ported verbatim from `_compute_log_prior()`, the MAP-EM log-prior kernel
    needed to score `L_MAP = total_loglik + log_prior`), a `Checkpoint` snapshot/restore
    mechanism, and `fit()` itself: iterate `fit_one_iteration` until `check_convergence` trips or
    `n_iter` exhausts, track the best-observed `L_MAP` checkpoint on a strict `>` (first-found
    wins on ties, matching Python), and restore it if the final iteration's `L_MAP` ends up worse
    — ported verbatim from `fit_streaming()`'s own post-loop "declined overall, restore the best
    snapshot" guard. Deliberately does NOT port the validation-based early-stopping path
    (orthogonal generalization mechanism, out of scope) and does NOT initialize `model` — k-means
    ++ seeding needs a real PRNG with no numpy-`default_rng`-bit-exact Rust equivalent, flagged as
    a genuinely separate open question rather than silently folded in; callers must pass an
    already-initialized model. **Golden-parity validated against two more real Python
    invocations**: a healthy 5-iteration ascending trace (no priors, no restore needed — the
    common path) and a deliberately adversarial K=3/all-priors-active 15-iteration trace
    engineered specifically to force a real decline (found by trial, not assumed) — Python's own
    `checkpoint_restored_=True`/`best_train_iter=0` output reproduced exactly, the one fixture
    that actually exercises the restore branch rather than just the "ascended cleanly" case. 2 new
    tests. Workspace: 143/143 tests pass, clippy clean.
  - **Initialization (`init.rs`) SHIPPED, same session** (operator: K-selection deprioritized,
    K=4 fixed is fine for now — narrows this slice to "seed one K=4 model," not multi-restart/
    BIC screening): k-means++ seeding (Arthur & Vassilvitskii 2007), ported from the ALGORITHM
    STRUCTURE of `tools/observation_vector/FeatureSaliencyEM.h`'s existing `KMeansPlusPlusSeed`
    (per the plan's own earlier note — starts from that institutional C++ port, not
    `student_t_hmm.py`'s `_kmeans_plusplus_seed_indices` cold), itself a verbatim-method port of
    that same Python function. Backed by a hand-rolled `SplitMix64` PRNG (Vigna's public spec,
    not a crate) — deliberately NOT chasing bit-exact parity with either the C++ port's
    `std::mt19937_64` or Python's `np.random.default_rng`/PCG64: the layer above the raw bit
    generator (turning `u64`s into a uniform draw) is itself implementation-defined even within
    the C++ standard alone (libstdc++ vs. libc++ differ), so no choice of raw generator buys real
    cross-language parity at the distribution layer — a deliberately flagged scope boundary, not
    a silently-accepted gap. One confirmed, deliberately-not-silently-resolved divergence from the
    Python original carried over from the C++ port instead: the degenerate all-points-coincide
    fallback draws uniformly over ALL indices (could re-pick an already-chosen one), where Python
    explicitly excludes already-chosen indices — not fixed, since the existing C++ port already
    made this call and this module was told to start from it. Validated three ways: `SplitMix64`
    reproduces Vigna's own reference C implementation's published output bit-for-bit (compiled and
    run directly via `gcc`, not trusted from memory); k-means++ itself is deterministic for a
    fixed seed, always returns distinct indices, and correctly handles the fully-coincident-points
    degenerate case without divide-by-zero/NaN; and a 200-trial statistical sanity check confirms
    it favors one seed per well-separated cluster in the clear majority of random seeds (the whole
    point of the algorithm over uniform seeding). 8 new tests. Workspace: 151/151 tests pass,
    clippy clean.
- **G3 — dependent on `model.rs`'s output shape**: `bootstrap.rs` (needs `model.rs`'s emission
  formula + shares a helper with `gate_metrics.rs`, fixing the Python-side duplication per F2).
  **SHIPPED, 2026-10-12** (spec decision #33): ports `hmm_parametric_bootstrap.py` in full —
  Markov-chain state-path simulation + 2D Student-t emission simulation directly from a fitted
  model's own `transmat`/`startprob`/`means`/`covars`/`dof` (not resampled real history, sidesteps
  the real nonparametric block bootstrap's `N_eff>=30` ceiling per the real module's own
  motivation), the 3 windowed gate statistics (`pareto_top_state_ratio`/`shannon_mean_tenure`/
  `taleb_signal_sigma`), and the CI + misspecification-p-value orchestrator. New pieces this
  module needed that didn't exist yet anywhere in the crate: a Box-Muller standard-normal sampler
  and a Marsaglia-Tsang (2000) gamma/chi-squared sampler (with an Ahrens-Dieter 1974 boost for
  `shape<1`, not reachable at this crate's own `dof_min=2.1` floor but implemented anyway so the
  sampler is correct over its full domain, not just the floor-constrained case). Found and fixed
  one real algorithmic mismatch while porting `simulate_state_path`: the real Python's per-step
  state draw is `(u[:, None] > row_cum).sum(axis=1)` (a direct count of cumulative-probability
  entries strictly less than `u`), which is NOT equivalent to a naive "first index with
  `cum[k] > u`" search at the exact-tie edge (`u` landing precisely on a cumulative boundary) —
  the count formula keeps the LOWER state at that edge; fixed with a `u <= cum[k]` comparison
  instead, caught by deriving both formulas by hand on a worked example before trusting either.
  Shares `gate_metrics::mean_tenure_by_state` (fixing the real Python module's own flagged
  duplication) and `calibration::percentile_sorted` (newly made `pub(crate)` for this). Not
  bit-exact cross-language parity (same scope boundary `init.rs`'s k-means++ seeding and
  `calibration.rs`'s stationary bootstrap already established) — validated via moment-matching
  (gamma/chi-squared sampler mean/variance converge to closed-form values across 200k draws for
  several dof) and structural checks instead. 16 new tests, workspace 216/216 pass, clippy clean
  (incl. Windows cross-compile).
  `registry.rs` **(corrected scope, 2026-10-11, operator: confirmed understanding of `MarketClimate`'s
  real ownership before implementing)** — ports `lbrnet/models/regime_registry.py`'s real
  `build_regime_registry()` classification cascade (tenure/Taleb-DOF-fragility/Pareto-thrust/
  vol-range priority-ordered rules with an explicit Chow-1970 "reject option" fallback), NOT
  `rc_enums.py`'s `MarketClimate` (confirmed a deprecated PEP-562 lazy re-export shim since
  2026-09-22 — real owner is `regime_registry.py`, which the enum's own docstring calls "the HMM
  State Manager"), and NOT the retired `HMMStateEnum`/Hungarian-alignment positional scheme
  (removed 2026-09-21, `RegimeProfile.alignment_cost`'s own comment). Architecturally this is a
  **one-time, post-training manifest-generation step** (classifies each of the K states once,
  producing a lookup table), not a live per-tick classifier — confirmed against
  `RegimeManager.cpp`, which only ever reads an already-computed climate value, never re-derives
  it — so this belongs conceptually next to `artifact.rs`'s manifest-generation territory, and is
  largely already prepared: built almost entirely from functions this session already ported in
  `gate_metrics.rs` (`mean_tenure_by_state`/`compute_pareto_thrust_shares`/`dof_fragility_score`/
  `taleb_gate_pass`/`TALEB_KURTOSIS_UNDEFINED_DOF`). **See the new §5.1d for a second, more
  serious finding surfaced while confirming this** — a genuinely diverged, currently-live second
  classifier native to this repo's own C++ that this port must reconcile with, not silently
  ignore — consumes `model.rs`'s posterior/state output; can be developed against synthetic
  posterior vectors in parallel with late G2, doesn't need to wait for G2 to fully finish).
  **SHIPPED, 2026-10-11**: `classify_states()` ports `build_regime_registry()`'s cascade verbatim
  (all 6 branches: 5 climates + the Chow-1970 reject-option fallback), built almost entirely from
  `gate_metrics.rs`'s already-ported functions as expected. Deliberately excludes Python's
  `centroid`/`alignment_cost` `RegimeProfile` fields (display-only/confirmed-dead respectively,
  left to `artifact.rs`) and `RegimeRegistry`'s `gang_validation`/`metadata` sign-off payload (a
  separate training-sign-off concern). **Golden-parity validated against a real
  `build_regime_registry()` invocation** (mts conda env, 2026-10-11) using a hand-constructed fake
  model object (real function, synthetic model data) deliberately engineered so all 6 of a 6-state
  fixture land in 6 DIFFERENT cascade branches, not just the easy/common path — exact match on
  climate assignment and every continuous score (`shannon_entropy`/`taleb_fragility`/
  `pareto_thrust`/`sample_share`/`mean_tenure_bars`) for all 6 states. 4 new tests. Workspace:
  155/155 tests pass, clippy clean.
  `artifact.rs` full implementation — **SHIPPED, 2026-10-11**: save/load a `StudentTHmm<D,K>` +
  its `registry.rs` classification as one plain JSON manifest (per decision #28's resolved format
  — no separate binary weights file), with a `model_sha256` content hash computed over the core
  model params alone (never the classification layered on top), anchoring decision #25's
  calibration-provenance design for whenever `calibration.rs` lands. Added `sha2` as a new
  dependency (workspace-level, matching the `serde`-for-offline-tooling precedent — a well-vetted
  crypto-hash crate, not hand-rolled, unlike this crate's own special-function hand-rolls). Loads
  reject a `D`/`K` mismatch against the manifest loudly rather than silently truncating/padding.
  4 new tests (round-trip exactness, hash determinism/classification-independence, dimension-
  mismatch rejection, missing-file handling). Workspace: 159/159 tests pass, clippy clean.
- **G4 — live engine assembly (`regime_engine.rs`)**: wraps `model.rs` + `registry.rs` for stateful live
  inference, climate overrides, model-risk tracking (variance-ratio drift, black-swan streak,
  Amari/FIM distance, duration tracking). This is where G0(a)'s state-identity ruling and G0(d)'s
  constants-placement ruling must be finalized in the external API, not deferred further — they
  were deferable during G2/G3's internal math but not here, since `regime_engine.rs` is what `hmm_step`
  (spec §5.2) actually calls.
  **SHIPPED, 2026-10-12** (spec decision #30): `RegimeState` (renamed from `HmmStep`) + `RegimeEngine`,
  ported from `regime_engine.py`'s `RegimeState`/`RegimeEngine` + `scoring.py`'s 4 multiplier tables,
  golden-parity-validated against real Python execution (entropy/confidence formulas, the full
  P2.4/P2.4b/velocity-deadzone/entropy-chaos override cascade). Design: "producer, not oracle" —
  `RegimeEngine` owns exactly one mutable-authority method (`step()`); every rule method
  (`is_momentum_aligned`, `get_hmm_multiplier`, `is_hostile_change_from`, ...) lives on the
  immutable `RegimeState` value itself, safe across the planned multi-thread execution topology
  (spec §8) without a lock around the engine for a query. No `RegimeSnapshot`-equivalent wrapper
  type (C++'s `RegimeSnapshot` is retired, not reproduced — its only reason to exist was the
  Python/async-ZMQ process boundary). The P2.4/P2.4b override cascade's 3 named observation-vector
  inputs (`tail_index`/`liq_fragility`/`burstiness_index`) arrive via a `StepContext` struct, never
  an array index — no generated dimension-index contract needed on the Rust side at all, since
  `rust/observation_vector` already computes each as its own named function. Amari FIM distance
  confirmed a static per-state constant, precomputed once at construction, not per step. 17 new
  tests, workspace 176/176 pass, clippy clean (incl. Windows cross-compile). **FFI wiring SHIPPED,
  2026-10-12** (spec decision #31, `rust/ffi/src/student_t_hmm.rs`): `hmm_init`/`hmm_step`/
  `hmm_reset`/`hmm_shutdown` opaque-handle lifecycle — did NOT need G0(e)/`calibration.rs` first
  after all (only a *production-calibrated* artifact needs that, not the plumbing, which
  shadow-wires against any valid `artifact::load`-readable model). Found and fixed two real bugs
  along the way: `RegimeEngine::step()` was allocating once its variance-ratio window filled
  (fixed: ring buffer + allocation-free `median_inplace`, plus a new `RegimeEngine::reset()`); and
  cbindgen does not evaluate ANY named Rust `const` — local or cross-crate — into a C++ array
  size, so `RegimeState::probability_vector`'s field needed a literal `[f32; 4]` plus a compile-time
  assertion tying it to `PRODUCTION_K`, not the named constant itself. 6 new FFI tests, `llvm-nm-22`
  confirms all 4 symbols export from the Windows cross-compiled staticlib, generated
  `include/generated/mts_core.h` diff is pure-additive. **Not yet done**: the C++-side consequence
  (plan Stage G7: retire `HMMStateEnum`/`RegimeSnapshot`, collapse `HmmStateIndicator`/
  `MarketClimateIndicator` into one, re-key `RegimeManager.cpp` onto `MarketClimate`) — deliberately
  not started this session, since the rule-API methods (`is_momentum_aligned`, `get_hmm_multiplier`,
  ...) aren't exposed through the FFI layer yet (out of this wiring task's scope).
- **G5 — training orchestration (`train.rs`)**: EM/MAP-EM loop, ECME dof (Brent's method),
  multi-restart screening, BIC-based K-selection. Depends on `model.rs` (shares core math),
  `artifact.rs` (to save), and G0(b)'s defaults reconciliation (now resolved). **EM/MAP-EM loop +
  ECME dof + k-means++ initialization SHIPPED, 2026-10-11** (see the progress notes under G2
  above — `fit_one_iteration`/`fit`/`init.rs`). **Multi-restart screening and BIC-based K-selection
  deprioritized, 2026-10-11 (operator directive): "Using K=4 may work for now so K-selection is
  not a priority right now"** — `model.rs`'s own `PRODUCTION_K: usize = 4` is already the intended
  fixed value (the runbook's own Smart Tuner V1 "frozen contract"), so this crate can train a
  single K=4 model end-to-end without either. Not dropped from scope entirely, just not next —
  revisit if a real need to search K (or re-screen multiple random restarts for the same K)
  resurfaces. Most of F4's ~20-test correctness checklist lives here (it's almost entirely about
  the M-step/EM loop) — budget this sub-stage the most time and the most test-writing effort of
  the whole crate, consistent with F3's warning that this exact code region has an eight-bug
  documented history plus one still-unexplained residual.
- **G6 — post-training pipeline**: `runtime_calibration.rs` (temporal half-life from the trained
  transition matrix — small, depends only on G5's output); `calibration.rs` (gate-threshold
  calibration methodology — depends on G0(e)'s `arch`-crate spike resolving; can proceed in
  parallel with G5 once that spike lands, doesn't have to wait for G5 to finish first).
  **`runtime_calibration.rs` SHIPPED, 2026-10-12** (`rust/student_t_hmm/src/runtime_calibration.rs`):
  `expected_state_durations_steps`/`calibrate_temporal_half_life`, ported from
  `lbrnet/analytics/regime_calibration.py`'s pure math only — deliberately NOT ported:
  `estimate_context_step_seconds`/`load_hmm_transition_matrix` (real `.context`-file I/O, `step_seconds`
  is a parameter here instead) and `write_agent_state_bin`/`read_agent_state_half_life` (FlatBuffer
  `Diagnostic` artifact persistence, belongs with `artifact.rs`'s territory if ever needed). Golden-
  parity validated against a real Python invocation (mts conda env). **Real finding along the way**:
  `max(0.1, x)` (both Rust's `f64::max` and Python's built-in `max()`) silently launders a NaN `x`
  to `0.1` rather than propagating it — meaning the `is_finite()`-based fallback-to-default branch
  is reachable only via `+Infinity`, never NaN, in BOTH languages; confirmed identical in real
  Python before writing the Rust test, not assumed — a faithful-port quirk documented in the test
  itself, not treated as a bug to "fix". 6 new tests, workspace 189/189 pass, clippy clean (incl.
  Windows cross-compile).
  **`calibration.rs` SHIPPED, 2026-10-12** (spec decision #32, G0(e) resolved):
  `rust/student_t_hmm/src/calibration.rs` — `politis_white_block_length`/`geyer_tau_int`/
  `stationary_bootstrap_indices`/`bootstrap_threshold_ci`, ported directly from the real, installed
  `arch` package's own source (v8.0.0), not reconstructed from the papers alone. **A real
  methodology fork was raised and resolved via an independent consultation**
  (`lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_158`/`_REPLY`, read-only `--approval-mode plan`): our
  own hypothesis that Politis-White's finite-variance assumption is unsound for this crate's
  fat-tailed Student-t data (`dof_min=2.1`) was independently reviewed and rejected — the `series`
  bootstrapped here are window-level summary statistics (bounded ratios, interior-order-statistic
  percentiles), not raw observations, and have well-behaved tails by extreme value theory
  regardless of the raw data's own tail weight; Politis-Romano-Wolf subsampling would have been an
  unjustified downgrade. Two real bugs found via golden-parity testing against the real `arch`
  package: Rust's `f64::min` always prefers the finite operand over NaN regardless of argument
  order, but CPython's built-in `min(a, b)` (what `arch`'s own source calls) is order-dependent —
  needed a hand-written `python_min` helper at the one load-bearing call site; and an overly
  conservative early-exit guard incorrectly returned NaN for a legitimate small-`n` case Python
  computes a finite value for (Python's out-of-range slicing clips to empty, never raises — fixed
  with `saturating_sub`/`.min(nobs)` clamping). 10 new tests, workspace 199/199 pass, clippy clean
  (incl. Windows cross-compile). **Follow-up refactor, same day**: `politis_white_block_length`
  changed from `f64` (implicit NaN sentinel + the `python_min` workaround) to `Option<f64>` — the
  degenerate precondition is now checked explicitly before any possibly-NaN arithmetic runs, so
  `python_min` is deleted entirely, not just unused; a representation change only, every golden
  value unchanged. 1 new test (an explicit property check that `optimal_block_length_sb` is always
  finite `>= 1.0`), workspace 200/200 pass, clippy clean. G0(e) is now fully resolved; G6 is
  complete.
- **G7 — integration/cutover**: wire `materialize_hmm_features.py`'s and `regime_calibration.py`'s
  replacement logic into whatever Rust pipeline stamps posteriors and computes live decay params;
  hook `regime_engine.rs`/`model.rs` behind `ffi`'s `hmm_step` (already designed, spec §5.2); delete
  `HMMClient.h`/`.cpp`, the already-deleted 4-file Meta-HMM "trainer run plan" sub-cluster (done
  2026-10-11, confirm nothing else references it by this point), the 3 benchmark files, and
  (pending G1/F2's "likely drop" confirmation once a real speed measurement exists) `student_t_hmm_gpu.py`'s
  Rust-side non-equivalent. **New task, 2026-10-11 (decision #29, spec §5.1d item 5)**: delete
  `MarketClimateIndicator`'s native kurtosis/entropy/Hurst cascade (`src/Indicator.cpp`'s
  `UpdateContext()` body) and `TripleScreen3.cpp`'s per-tick call into it — confirmed a live write-
  race bug (its every-tick write overwrites `HMMClient::HandleBinaryResponse`'s wire-received,
  trained-model classification before any consumer reads it), not a design worth preserving.
  `regime_engine.rs` becomes the sole writer of climate state once this lands; do not carry the race
  forward by leaving the native write path in place "just in case."

**Net effect of this sequencing**: the actual critical path is G0(a,c,e) → G2 → {G3, G5} → G4 → G6
→ G7, with G1 (and G0(c)'s stub-level unblocking) running earliest and in parallel, and G0(e)/G6's
`calibration.rs` deliberately pushed latest since it's the one piece with a real open
library-availability gap that could slip without blocking anything else. **Update, 2026-10-11**:
G0(b) and G0(d) are now both resolved (decisions #25-#26) — G0(a) (gates G4 only) and G0(c)/(e)
(stub-level unblocking only) are the remaining true G0 blockers; G2's training slice (EM/M-step)
can proceed without waiting on anything further from G0. **Further update, same day**: G0(c) is
now ALSO resolved (decision #28) — `artifact.rs` shipped in full, not just a stub; the only true
remaining G0 blocker gating anything beyond a stub is G0(a) (`regime_engine.rs`'s external API, G4) and
G0(e) (the `arch`-crate spike, `calibration.rs`, G6). **Update, 2026-10-12**: G0(a) resolved
(decision #30, `regime_engine.rs` shipped) and G0(e) resolved (decision #32, `calibration.rs`
shipped) — **every G0 blocker is now resolved.** G4 and G6 are both done; the remaining critical
path is G7 only (the C++-side cutover).

### 4a. `RegimeManager` 8-file call-site migration (spec §5.2's companion groundwork, approved 2026-10-10)

**Status: DONE, 2026-10-10.** Retires direct `HMMStateEnum` reads in favor of the already-built,
zero-caller `include/RegimeManager.h`/`.cpp` facade — independently valuable (closes the same
label-switching bug class `lbrnet`'s own `STUDENT_T_HMM_MASTER_PLAN.md` found live in 3+ places) and
de-risks `student_t_hmm`'s design (proves the rule/label-based consumption pattern against the *existing*
HMM before the Rust port has to get it right cold). Was flagged "not-yet-approved" in `lbrnet`'s own
plan doc (last touched 2026-09-22) — approved for MindfulTrader 2026-10-10, this plan is now that
"separate MindfulTrader-side session" its own doc said this needed.

**The 8 files** (named in `RegimeManager.h`'s own header comment): `PositionManager`, `RiskManager`,
`Scoring`, `TripleScreen3`, `Indicator`, `InferenceManager`, `Trade`, `TripleBarrierExitManager`.
**Real scope correction, 2026-10-10**: the first grep-based estimate (86 raw hits) was far too
broad — it matched every call to `HmmState()`, including legitimate continuous-field reads
(`.Dof()`, `.Mahalanobis()`, `.RiskMultiplier()`, etc.) that have nothing to do with the
label-switching bug class. The real migration scope is only genuine `HMMStateEnum::VALUE`
comparisons/branches — verified file by file by direct code read, not re-estimated.

| File | Status |
|---|---|
| PositionManager | **Done, 2026-10-10** — 5 sites (`regimeForcePassive`/`momentumBoost` pair, 2× `ToRegime`, `GetRegimeGradeThresholds`) migrated to `RegimeManager`; build-verified. The 6th site, `PositionManagerPatterns.cpp`'s hostile-regime-change check, needed a `RegimeSnapshot`-typed member (not a 1:1 swap) — done, see below |
| RiskManager | **Done, 2026-10-10** — 1 site (`GrantsMomentumBoost`); build-verified |
| Scoring | **Done — nothing to migrate.** All hits are the lookup-table construction `RegimeManager::GetPatternMultiplier` itself delegates to (the implementation being wrapped, not a consumer) |
| TripleScreen3 | **Done, 2026-10-10** — 2 clusters: Turtle Soup bullish/bearish alignment → `IsMomentumAligned`/`IsMeanReversionAligned` (verified behaviorally identical via the hostility carve-out logic); NR7's 4-state tautological OR → `IsBurnInComplete()` (confirmed this is the *exact* site `RegimeManager.h`'s own comment names as that method's motivation). Build-verified |
| Indicator | **Done — nothing to migrate.** `MarketClimateIndicator::UpdateContext`'s `isHmmMomentum`/`isHmmMeanRev` is the canonical source `RegimeManager.h` itself cites as `IsMomentumAligned`/`IsMeanReversionAligned`'s basis — it's the classification, not a consumer |
| InferenceManager | **Done — nothing to migrate.** All hits are the static function bodies (`IsHostileRegimeChange`, `GetStaleFishBarThreshold`, `GetRegimeGradeThresholds`) `RegimeManager` wraps — the backing implementation, not a consumer |
| Trade | **Done, 2026-10-10.** `Trade.h`'s only `HMMStateEnum` usage (`m_entryHMMState`/`SetEntryRegime`/`GetEntryHMMState`, paired with `GetEntryClimate`/`m_entryClimate` set by the same setter) had **zero real callers anywhere in the repo**, confirmed by direct grep, and `SetEntryRegime` had no `.cpp` implementation at all (never odr-used) — deleted outright per this repo's standing dead-code-deletion default. `SetOriginalTargets`/`GetOriginalTarget1-3` (same "GAP 27" comment block, a separate concern — mid-trade target tightening, not regime tracking) deliberately left untouched, out of this cleanup's scope. Full clean build verified (`./build_dll.sh`, not `--no-clean` — see the PCH note below) |
| TripleBarrierExitManager | **Done — nothing to migrate.** Its only `HMMStateEnum` reference is `ToRegime()`'s own implementation — the backing logic `RegimeManager::GetTripleBarrierExitParams()` wraps |

**Hostile-change site — done, 2026-10-10.** `PositionManagerPatterns.cpp`'s hostile-regime-change
check (gates an emergency flatten) is migrated: `PositionManager.h` gained two new, additive
`RegimeSnapshot` members (`m_currentRegimeSnapshot`/`m_previousRegimeSnapshot`) shifted in exact
lockstep with the existing `HMMStateEnum` pair inside `SyncRegimeState()` — `RegimeSnapshot`'s
constructor is deliberately private (`friend class RegimeManager` only), so it has to be captured
via `GetCurrentSnapshot()` at the same point the raw enum is captured, not derived from an
already-stored raw value. The raw enum members were **not removed** — they still back the
change-detection guard and diagnostic logging unchanged. `EvaluateRegimeDefense()`'s call became
`RegimeManager::Instance().IsHostileRegimeChange(m_previousRegimeSnapshot, isLong)`; the same file's
`GetHMMMultiplier(pattern, m_currentHMMState)` also migrated to `RegimeManager::Instance().
GetPatternMultiplier(pattern)` (same file, same migration pass, confirmed safe — identical live
source). Full clean `./build_dll.sh` verified (not `--no-clean`, per the lesson above — this
touched `PositionManager.h`).

**The entire 8-file `RegimeManager` migration is now closed out.** All genuine consumer call sites
across all 8 files are migrated and build-verified; the files with "nothing to migrate" status
were confirmed foundational by direct code read, not assumed.

**Discipline**: `RegimeManager`'s own header comment states every method is "a thin, verified-
equivalent wrapper around logic that already exists and is already live; no new regime-
classification behavior is introduced" — this migration must be behavior-preserving, swap the call
site only, no logic redesign bundled in. Build-verify (`./build_dll.sh`) after each file, not just
at the end, so a regression is attributable to one change.

**Real lesson, 2026-10-10 — `--no-clean` gave a false-positive "Build Successful" after a header-
only edit.** `./build_dll.sh --no-clean` reported success in 0s with zero object files rebuilt
after a `Trade.h` change (removing dead members shrank the precompiled header from 6826→6389
bytes); the stale `.pch`/object files were silently reused. Only forcing a `.cpp` touch surfaced
the real error: `clang-cl` refuses a stale PCH outright (*"file has been modified since the
precompiled header was built"*) — so the false positive was `--no-clean`'s dependency tracking
missing the PCH invalidation, not a silent miscompile. **Rule going forward: any header-file edit
in this migration gets a full `./build_dll.sh` (no flag), not `--no-clean`** — `--no-clean` is only
safe to trust after a `.cpp`-only change.

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
`CarryForwardCalculators.h`'s four functions — `ComputeRelativeRange` (dim 2), `ComputeBurstinessIndex`
(dim 3's real caller, `log_scale_expansion_ratio` — the name is a historical artifact, it no longer
feeds dim 1), `ComputeFisherInformation` (dim 8), `ComputeAmihudIlliquidity` (dim 11) — plus
`EventVelocityEngine.h`'s `CalculateBurstinessIndex` (dim 1, `burstiness_index`/`raschkeBurst`).
38/38 Rust unit tests pass (16 new); 33/33 cross-language parity assertions pass (7 new). Six real
production call sites shadow-wired across three files (`StudyHelperFunctions.cpp` ×3,
`TripleScreen2.cpp` ×2, `ContextManager.cpp` ×1 — the first ports to touch files other than
`StudyHelperFunctions.cpp`).

**Correction, 2026-10-09 (same day)**: the "blocked on P2/P3" claim below for
`EventVelocityEngine.h`/`CarryForwardCalculators.h` was wrong — verified by direct code read that
neither file, nor their real call sites, has any schema/dim-index coupling. The real coupling lives
entirely in `FeatureScaler.h`, downstream of every dim equally (including the first five ported) —
not a blocker unique to these five. **Lesson for future sessions: re-verify a "blocked" claim by
direct code read before accepting it, even (especially) one this document itself made.**

**None remaining self-contained among the originally-identified ten** (no `FeatureScaler`/dim-index
coupling) — all ten are ported. Two more confirmed self-contained and ported the same way,
2026-10-11 (see the dated entry below): `recurrence_rate` and `micro_asymmetry`. Every other
remaining dim genuinely has `FeatureScaler`/dim-index coupling (not merely assumed to, per the
correction above) and is blocked on P2/P3 (§1.1).

**Resolved, 2026-10-11** (operator asked for "low hanging fruit"; this was the plan's own flagged
open item): `recurrence_rate` (`RecurrenceRateEngine.h` + `RQAEpsilonSelector.h`) and
`micro_asymmetry` (`OrderFlowAsymmetryEngine.h`) were confirmed self-contained by direct code read
(zero `Contract::`/`FeatureScaler`/dim-index coupling in either file) and ported. `recurrence_rate`
was the first genuinely *stateful* C++ source ported into this crate (`RecurrenceRateEngine` caches
an O(n²) pairwise count across `RebuildClosedBarWindow`/`ComputeRate` calls) — resolved by porting a
single stateless `recurrence_rate(closed_bars, live_price, epsilon)` function that recomputes the
full matrix fresh every call, rather than replicating the incremental cache. This is not a
simplification that changes behavior: both real production call sites
(`ContextManager.cpp`'s `s_recurrenceEngine`, `ImbalanceContextManager.h`'s
`ComputeImbalanceRecurrenceRate`) already call `RebuildClosedBarWindow`+`ComputeRate` together,
gated on bar-close, never streaming across ticks with the window held fixed — the incremental cache
buys nothing at either real call site, so a fresh-every-call stateless port is behaviorally
identical (confirmed: `RecurrenceRateEngine.h`'s own native test already proves the incremental
engine equals a brute-force n² reference). 16 new Rust unit tests (54/54 total, up from 38); 5 new
cross-language parity assertions (38/38 total). Shadow-wired: `micro_asymmetry` into
`TripleScreen3.cpp` (first Rust shadow-wire in this file); `recurrence_rate` +
`select_epsilon_for_target_recurrence_rate` into `ContextManager.cpp`. **Deliberately NOT
shadow-wired**: `ImbalanceContextManager.h`'s `ComputeImbalanceRecurrenceRate` (the second real
`recurrence_rate` call site, activity-clock IS2 path) — that header's own doc comment declares its
compute functions side-effect-free by design ("no hidden singleton reads, no side effects"); adding
a `Logger::getInstance().log(...)` shadow-compare call there would violate that explicit
architectural contract. Recorded as a known, deliberately-deferred follow-up, not a missed site (the
C++ source itself is unchanged either way; only a second shadow-wire comparison point is skipped).
One clippy finding along the way: `select_epsilon_for_target_recurrence_rate`'s infeasibility check
needed `#[allow(clippy::neg_cmp_op_on_partial_ord)]` — the negated-comparison form
(`!(off_diag_target > 0.0)`, not `<= 0.0`) is deliberate, matching the C++ original's NaN-routes-to-
fallback behavior bit-for-bit, which `<=` would silently not preserve. Verified: `cargo test
--workspace` (all pass) and `cargo clippy --workspace --all-targets -- -D warnings` (clean); native
cross-language parity test recompiled and rerun (38/38 assertions pass); both CMake presets
(`MTS_WITH_RUST=OFF`/`=ON`) rebuilt clean from scratch.

**Still genuinely unconfirmed** (not addressed by the above): wherever `burstiness_index`'s
`FeatureScaler`-side calibration is finally read — still worth re-checking for the same "looks
coupled but isn't" property before assuming it's genuinely blocked.

**Methodology, proven twelve times**: see §5.2 below.


### 5.2 Port-by-port approach

The proof pattern each landed dim has followed, and the template for every future one:

1. Port the pure function from its C++ header into `rust/observation_vector/src/lib.rs`, using
   fixed-size stack arrays where the C++ original does (no heap allocation, matching the hot-path
   discipline of the rest of this codebase).
2. Rust unit tests mirroring the C++ test file's exact golden values.
3. An `ffi` FFI wrapper (`(ptr, len)` shape, panic-guarded via `catch_unwind`); regenerate the
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
3. **`observation_vector` crate**, ported dim by dim in order of stability (dims still in flux last).
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
rust/<crate>/Cargo.toml      tiny; pure crates depend only on workspace deps + schema
rust/ffi/Cargo.toml          crate-type = ["staticlib", "rlib"] (rlib so tests run natively on Linux);
                              features student_t_hmm/observation_vector/transport gate optional deps
rust/ffi/cbindgen.toml       generates include/generated/mts_core.h
rust/py/pyproject.toml       maturin; cdylib; module name "mindful_core"
```

### 6.2 CMake integration

`cpp/CMakeLists.txt` imports the Rust staticlib (add, don't restructure); a `MTS_WITH_RUST` option
(default OFF) and a matching CMake preset (`wsl-clang-cl-release-rust`, separate build directory) keep
the normal build path completely unaffected when Rust isn't enabled. `-ffast-math` policy: **ruled,
2026-10-11 — removed entirely from the C++ build** (§0's TL;DR entry below has the full finding and
fix; spec §2 item 11/§10 item 5). Rust never had an equivalent flag to begin with (no stable
`-ffast-math`-equivalent in rustc), so this also means C++ and Rust numerics are now on the same
strict-IEEE-754 footing by default, not just "avoid it near goldens."

### 6.3 Scripts (repo root `scripts/`, every one derives `ROOT` from its own path)

- **`rust_windows_env.sh`** (sourced) sets the cc-rs cross-compile env vars for
  `x86_64-pc-windows-msvc` (clang-cl-22, the xwin sysroot, `/winsysroot` — chosen because the sysroot
  path has no spaces, unlike "Windows Kits", which cc-rs would split).
- **`build_rust.sh [--features ...]`** builds `ffi` for the Windows target, then flags if the
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
to do better than, since Rust becomes a build prerequisite for `cpp/` the moment `ffi` is wired in).

### 6.5 Cross-cutting runtime rules (bake into every code review of Rust work)

- Every `extern "C"` function is `catch_unwind`-guarded and returns a status.
- `mts_init`/`mts_shutdown` are explicit, driven by `sc.LastCallToFunction`. Nothing relies on
  destructors at unload; no `static` holds a thread or socket past shutdown.
- Unload/reload (§2 check 6) must pass — reasoned through, cleared — before any long-lived thread or
  socket ships in `ffi`.
- One FFI call per tick or bar-close, not per dim. No allocation in any `*_step` function.
- Python calls release the GIL around every blocking or long call; NumPy arrays cross zero-copy
  (`rust-numpy`), never Python lists in hot loops.
- **libzmq, one copy per process image**: during the transition, `transport` links the same dynamic
  vcpkg libzmq the DLL already uses (no vendoring) — read the exact env-var names from the zmq-sys
  version actually locked, they change between releases. After `transport` owns every socket, switch
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
| `flatc --rust`'s atomic scratch-dir generation pattern (generate into `.work_gen/`, copy on success) | **Crate placement** — Atratus dumps generated Rust into its one do-everything `sensor_core`; this repo correctly uses a dedicated `schema` crate instead, so `student_t_hmm`/`observation_vector`/`transport` each depend on just the schema, not transitively on each other |
| `--rust-module-root-file` for multi-schema generation | **Not applicable anymore** — this repo merged its two schemas into one (spec §4.2), which Atratus never needed to since it only ever had one |
