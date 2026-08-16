# Spec: Precompiled-Header Right-Sizing & Per-File Include Hygiene Audit

**Status**: SPEC — approved via `superpowers:brainstorming` (2026-08-16, MindfulTrader-rooted session).
**Implementation deferred.** Standalone from the trading-system convergence work in
`2026-08-16-execution-risk-coevolution-governance-spec.md` and its companion backlog — this is a
build-infrastructure initiative, discovered while investigating a clangd diagnostic during that
other work, but independently scoped and independently valuable.

## Purpose

While fixing a stale comment in the Triple-Barrier exit path, a clangd diagnostic ("cannot open
source file windows.h/string/array/cstdint," "squiggles disabled for this translation unit") led to
tracing the project's precompiled-header setup. The finding is more consequential than an IDE
annoyance: `include/MindfulTrader_Precompiled.h` (wired via `target_precompile_headers()` in
`CMakeLists.txt:167-171`, applied to the whole `MindfulTrader` target with no per-file exceptions)
bundles two very different kinds of content together:

- Genuinely stable material a PCH should contain: `sierrachart.h` (vendor SDK header), STL headers,
  and two stable third-party libraries (`nlohmann/json.hpp`, `flatbuffers`).
- **20 of the project's own actively-developed headers** — `PositionManager.h`, `RiskManager.h`,
  `TradeSignalManager.h`, `ContextManager.h`, `IndicatorManager.h`, `InferenceManager.h`,
  `HMMClient.h`, `Indicator.h`, `Trade.h`, `StudyHelperFunctions.h`, `TransportStream.h`,
  `MindfulTraderConstants.h`, `TradeCommunication.h`, `Logger.h`, `AIConnectionMonitor.h`,
  `ZMQContextManager.h`, `DailyHighLowLoader.h`, `TradeExecutionServer.h`, `NhNlDataLoader.h`,
  `SystemOrchestrator.h`, `messaging/EventSerializer.h` — exactly the files under active, frequent
  development (several are the direct subject of the concurrent convergence backlog).

A PCH is only a net win when its contents are stable, because the entire mechanism is "compile once,
reuse many times." Bundling in headers that change constantly defeats that premise and was directly
verified to have a real, measured cost (see Investigation Log) — this is not a hypothetical risk.

## Verified Findings (2026-08-16, do not re-derive)

1. **Correctness holds — no silent-staleness risk.** Empirically tested: appended a harmless trailing
   comment to `PositionManager.h` (a PCH-bundled header), ran `./build_dll.sh --no-clean`. Result:
   `cmake_pch.cxx.obj` correctly rebuilt, all ~35 translation units in the target recompiled, build
   green in 70s. CMake's dependency tracking for `target_precompile_headers()` correctly detects
   PCH-header changes on this WSL-hosted, `/mnt/c`-toolchain cross-compilation setup — the one
   genuinely dangerous failure mode (a stale `.pch` silently letting mismatched code ship in a live
   trading system) is not present. Change reverted after the test (`git checkout -- include/PositionManager.h`).
2. **The cost is real and measured, not estimated.** The same probe's rebuild (70s, all ~35 files +
   PCH object) versus an earlier, unrelated two-file comment-only edit that didn't touch a
   PCH-bundled header (21s, 2 files relinked) — roughly 3.3x for what was a single trailing comment
   line in one header. This tax is paid on every edit to any of the 20 bundled project headers.
3. **The clangd failure is a downstream symptom, not a separate bug.** `clangd` re-parses the PCH
   header text for live diagnostics rather than consuming the compiled `.pch` binary the same way the
   real `clang-cl` invocation does (which resolves `/vctoolsdir`/`/winsdkdir`-mounted Windows-toolchain
   headers correctly). Bundling volatile project headers into the PCH makes this fragile in exactly
   the files most under active development — confirmed project-wide, not file-specific: of the 40
   entries in `build-windows/compile_commands.json`, `/Yu` PCH usage is not universal
   (`all(... '/Yu' in e['command'] ...)` is `False`), meaning some translation units are already
   excluded from PCH for other reasons — precedent already exists for per-file PCH opt-out in this
   build.
4. **Timing risk of deferring**: the concurrent convergence backlog's Units 4/5
   (`REGIME_INVALIDATION` wiring, Trend-Scanning/profit-protection) will very likely touch
   `PositionManager.h`/`RiskManager.h` directly — both PCH-bundled — meaning the ~3.3x tax compounds
   specifically through the immediately upcoming work, not a hypothetical future edit.

## Approach (agreed, refined 2026-08-16): New `pch.h` as the Sole Precompile Target, `MindfulTrader_Precompiled.h` Retired

Refined during the same brainstorming session from an initial "trim in place" idea to a cleaner
two-file structure, after checking whether an in-between version (keep `MindfulTrader_Precompiled.h`
as a surviving aggregator, just with a new stable-only file prepended) would work — it would not:
as long as any file every `.cpp` includes still unconditionally pulls in the 20 volatile project
headers, the measured full-rebuild blast radius is untouched regardless of which file that
aggregation happens to live in, or whether the compiler treats it as precompiled. The fix requires
eliminating the volatile aggregator entirely, not relocating its stable prefix.

**Institutional name for the new dedicated PCH file: `include/pch.h`** — the standard MSVC/clang-cl
convention for a file whose only job is being the precompile target, kept deliberately narrow. (If
matching this repo's existing PascalCase header-naming convention is preferred over the raw MSVC
convention, `PrecompiledHeader.h` is the alternative — pick one before implementation, don't leave
both as live options.)

Rejected alternatives and why, for the record:
- **Manual per-file audit before stripping the PCH** — reasons out by hand what the compiler proves
  for free; more effort, more room for human error, no advantage identified over the chosen approach.
- **A slimmed two-tier PCH** (keep some "stable enough" project headers) — reintroduces exactly the
  "trust someone to notice when a header stops being stable" rot this project's own recent work
  (the C++/Python co-evolution governance spec) was written specifically to avoid. Not adopted unless
  Step 3 below shows a build-speed regression that justifies revisiting.
- **Keep `MindfulTrader_Precompiled.h` alive with a new stable-only file prepended** — the initial
  version of this approach; rejected once traced through in detail (see refinement note above) because
  it does not eliminate the volatile aggregator that actually causes the measured cost.

### Steps

1. **Create `include/pch.h`** containing only the verified-stable content: `sierrachart.h`, the
   existing STL header list, `nlohmann/json.hpp`, `flatbuffers/flatbuffers.h` +
   `generated/mts_schema_generated.h` (generated, but regenerated only on deliberate schema changes
   per `regenerate_schema.sh` — not organic day-to-day churn; keep unless this assumption is proven
   wrong during the audit). Point `CMakeLists.txt:167-171`'s `target_precompile_headers()` at this new
   file instead of `include/MindfulTrader_Precompiled.h`.
2. **Retire `include/MindfulTrader_Precompiled.h` entirely** — do not keep it around in any form, even
   slimmed. Every `.cpp` file's include of it goes away.
3. **Full clean rebuild, fix per-file compile errors mechanically.** `./build_dll.sh` (clean, not
   `--no-clean`) will emit "unknown type / undeclared identifier / no member" errors, file by file, for
   every `.cpp` that was implicitly relying on the old aggregator's blanket project-header inclusion.
   Add `#include "pch.h"` plus the exact explicit project-header `#include`(s) each error names to
   that specific file. Iterate to a fully green clean build. Expect this to touch most of the ~35
   `.cpp` files in the target to some degree — each fix is mechanical and proven correct by the file
   compiling afterward, not a design judgment call.
4. **Measure, don't assume, the outcome.** Re-run the exact probe from Investigation Log (touch
   `PositionManager.h`, `--no-clean` rebuild) and confirm it no longer forces all ~35 files to
   recompile — only its real dependents. Record the before/after numbers.
5. **Re-check clangd.** Confirm the diagnostics that started this investigation
   (`TripleBarrierExitManager.h`'s missing `windows.h`/`string`/`array`/`cstdint`) are resolved for a
   sample of affected files, without needing a separate IDE-only compile database — a correctly
   scoped `pch.h` should let clangd's own PCH consumption work as intended.

## Non-Goals

- **A two-tier/partial PCH.** Considered and explicitly deferred — see Approach section. Do not
  introduce without first measuring a real regression from the full-strip approach.
- **A separate clangd-only compile database that strips `/Yu` flags.** This was the original,
  narrower idea before the deeper PCH-content problem was found; superseded by fixing the actual PCH
  content instead of working around it in tooling. Do not build this unless Step 4 shows the
  right-sized PCH still doesn't satisfy clangd for some unrelated reason.
- **Per-file PCH exclusion via `SKIP_PRECOMPILE_HEADERS`.** The project-wide, single-PCH structure
  (one file, applied to the whole target) is kept — only *which* file is the target changes (from
  `MindfulTrader_Precompiled.h` to the new `pch.h`), and its contents shrink to stable-only. If Step 3
  reveals a real need for per-file exclusion, treat that as new information requiring a return to
  brainstorming, not something to design blind here.
- **Any change to the trading-system convergence backlog's priority order.** This spec is
  independent; sequencing it relative to Units 1-9 of the companion backlog is an execution decision,
  not a design one, and is explicitly left open (see Investigation Log's timing-risk note).

## Test Plan

1. Full clean `./build_dll.sh` succeeds with zero errors after the strip-and-fix pass.
2. The `PositionManager.h`-touch probe, re-run post-fix, shows a materially smaller recompile set
   than the pre-fix 35-file/70s baseline — record the actual number.
3. Spot-check clangd diagnostics clear on `TripleBarrierExitManager.h` and at least 2-3 other files
   that previously showed the same class of error.
4. `tests/run_python_tests.sh` (ZMQ integration tests) still passes — confirms the trimmed PCH didn't
   accidentally drop something load-bearing for runtime behavior, not just compile-time.

## Acceptance Criteria

1. `include/pch.h` exists, contains only system/STL/stable third-party content, and is the sole
   `target_precompile_headers()` target in `CMakeLists.txt`.
2. `include/MindfulTrader_Precompiled.h` no longer exists — fully retired, not kept in any slimmed
   form.
3. Every `.cpp` file explicitly includes every project header it directly uses — no file relies on
   transitive inclusion via a shared aggregator for a project header.
4. Touching any formerly-bundled project header no longer forces a full-target rebuild — only its
   actual dependents recompile.
5. `build_dll.sh` and the Python integration test suite both green.

## Rollback

Additive/corrective changes to header contents and includes, plus one file deletion
(`MindfulTrader_Precompiled.h`) and one new file (`pch.h`) — no runtime logic touched. If the
strip-and-fix pass surfaces something unexpected mid-way, revert as a single commit: restore
`MindfulTrader_Precompiled.h`, delete `pch.h`, and point `CMakeLists.txt`'s
`target_precompile_headers()` back at the restored file. The pre-existing mega-PCH behavior returns
immediately.

## Investigation Log

- **2026-08-16**: Discovered mid-session while fixing a stale comment in `PositionManager.cpp`/
  `TripleBarrierExitManager.h` (see the execution-risk convergence backlog's Unit 2) — a clangd
  diagnostic on the edited header led to tracing `CMakeLists.txt`'s PCH wiring and
  `MindfulTrader_Precompiled.h`'s actual contents. User's own framing: "I may not have understood and
  nor correctly used the PCH functionality" — confirmed correct via the 20-bundled-project-header
  finding above. Verified via a live, reverted probe (append/rebuild/measure/revert on
  `PositionManager.h`) rather than assumed: PCH invalidation is correctly detected (no staleness
  risk), and the rebuild cost is real (~3.3x, 70s vs. 21s) and about to compound through the
  concurrently-planned convergence backlog's Units 4/5. Compiler-driven strip-and-fix approach agreed
  with the user over a manual audit or a two-tier PCH.
- **2026-08-16, same session, refined**: user proposed keeping `MindfulTrader_Precompiled.h` alive
  with a new stable-only file prepended as its first include, asking whether this would be
  institutional-grade or amateur. Traced through in detail: it would not fix the measured problem —
  the full-rebuild blast radius is caused by the volatile-header aggregator pattern itself, not by
  where the stable content is textually located or whether it's precompiled. Refined to the current
  approach: a new, narrowly-scoped `include/pch.h` becomes the sole precompile target, and
  `MindfulTrader_Precompiled.h` is retired entirely rather than kept in any form. Deferred —
  "leave this beast for a later time" — no implementation started; this spec captures the refined
  design so the next session doesn't have to re-derive it.
