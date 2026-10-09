# Cross-Repo Naming & Schema Coherence Audit

**Status:** audit only, nothing fixed yet. Opened 2026-10-09 at the operator's request, mid-session,
while scaffolding `rust/schema` (`mts_schema`). Trigger, in the operator's own words: *"these projects
were evolved incrementally, and we have been trying to get a cohesive system for a while now. Rust
could be the missing piece that finally makes our system that system I envisioned. My problem is that
the incoherence resulting from incremental/iterative spec is staring me right in the face. And we
cannot simplify/optimize the system without addressing that incoherence."* Work on the schema
round-trip proof (the next step of the Rust-adoption roadmap) was explicitly paused to open this first.

**Read with:**
- `2026-10-08-monorepo-infrastructure-guide.md` — the Cargo/CMake/schema mechanics this audit's
  findings feed into.
- `../plans/2026-10-08-monorepo-rust-adoption-roadmap.md` — the Phase 2 sequencing this audit pauses.

**Scope of this pass:** the FlatBuffers schema surface (`schema/mts_schema.fbs`,
`schema/backtest_schema.fbs`, `schema/regenerate_schema.sh`) and the cross-repo naming it touches
(C++/Python/Rust generated code, repo names). This is where today's concrete evidence comes from,
because it's what today's work was touching. **Not yet surveyed**: `lbrnet`'s and `MTS`'s own
internal naming conventions beyond where they touch the schema, and `MindfulTrader`'s C++ class/file
naming beyond what's cited below. Flagged as an open follow-up (§5), not silently out of scope.

---

## 1. Why now, specifically

Two things make this the cheapest point in the system's history to decide on schema naming/structure,
not just a convenient one:

1. **Zero Rust consumers exist yet.** `mts_schema` was scaffolded this session and nothing outside it
   depends on its generated symbol names. Every day a rename is deferred, more Rust/C++/Python code
   accretes against today's names, raising the cost of ever fixing it.
2. **The system is not in production** (`PRODUCTION_TRIAGE.md` standing rule, 2026-08-26 — see this
   repo's own top-of-file banner). There is no live consumer, no external contract, no silent
   behavior change to protect. The same logic that already governs dead-code deletion applies
   here: default to fixing, not preserving, once a genuine inconsistency is verified — not the
   other way around.

## 2. Methodology — a decision gate per finding, not a blanket pass

For each finding below, the operator decides one of three dispositions (recorded in §3, not
pre-decided by this doc):

- **Fix now** — low blast radius, clear win, do it before anything new depends on the old name.
- **Defer, with a written reason** — real cost to fix now (e.g., genuinely high blast radius, needs
  its own dedicated spec), explicitly scheduled, not silently dropped.
- **Accept permanently** — on inspection, not actually incoherent, or the cost of change exceeds the
  benefit even with nothing in production yet. Record *why*, so it isn't re-litigated later.

This mirrors the discipline already used successfully twice this session (§3.5) — the goal of this
doc is to make that discipline explicit and catalog-driven instead of ad hoc.

## 3. Findings catalog (evidence-grounded, surveyed 2026-10-09)

### 3.1 Schema table/struct naming: one precise outlier, not systemic chaos

Surveyed every `table`/`struct` name in both `.fbs` files (`grep -oE "^(table|struct) [A-Za-z_]+"`):

- `mts_schema.fbs`: 38 names, all clean PascalCase (`Heartbeat`, `TrainingEvent`, `ObservationData`,
  `AsymmetryContext`, ...) **except one**: `MTS_Envelope` (and its declared `root_type MTS_Envelope`).
- `backtest_schema.fbs`: all 8 names clean PascalCase (`BacktestFrame`, `DecisionEvent`,
  `PredictionAck`, ...). Zero outliers.
- Enum names: all 22 (`EventType`, `HeartbeatStatus`, `BacktestReplayMode`, ...) clean PascalCase.
  Zero outliers.

**This is the finding that actually triggered today's Rust friction**: `non_camel_case_types`
flagged `MTS_Envelope`/`MTS_EnvelopeBuilder`/`MTS_EnvelopeArgs`/`MTS_EnvelopeOffset` in the generated
Rust (previously silenced by `mts_schema`'s lint-suppressing wrapper, not fixed at the source).

**Disposition: FIXED, 2026-10-09.** Operator chose `Envelope` over my originally-proposed
`MtsEnvelope` — the namespace is already `MTS.Schema`, so a prefix would just be redundant
(`MTS.Schema.MtsEnvelope`). Renamed at the one place it actually lives (the `.fbs` `table`/
`root_type` declaration), not just suppressed in each language's wrapper. Mechanical
string-substitution rename (`MTS_Envelope` → `Envelope`) was safe everywhere it appears — every
derived form (`MTS_EnvelopeBuilder`, `GetMTS_Envelope`, `CreateMTS_Envelope`, the Python module
filename, ...) is generated purely from the table name, confirmed by sampling actual usages in all
three languages before the global replace.
**Real blast radius, not just the schema file**: 151 occurrences across 13 hand-written (not
generated) consumer files in three repos — `MindfulTrader` (`SystemOrchestrator.h/.cpp`,
`EliteFlatBufferHelper.h/.cpp`, `TradeExecutionServer.cpp`, `HMMClient.cpp`), `lbrnet`
(`backtest/backtest_server.py`, `lbrnet/inference/live_agent.py`), and `MTS`
(`system_orchestrator.py`, `websocket_broadcaster.py`, `trade_server.py`, `action_plan.py`,
`zmq_client.py` — by far the largest share, since the GUI consumes the full envelope protocol).
Also needed updating: `schema/regenerate_schema.sh` (it embeds a hand-written C++ envelope-helper
snippet referencing `MTS::Schema::MTS_Envelope` directly inside a heredoc — would have emitted code
referencing a type that no longer exists) and `schema/self_test_schema_contract.py` (an expected-
symbols list that would have falsely reported the rename as "symbol missing").
**Verification, not just "it compiles"**: full `regenerate_schema.sh` run (all three languages,
checksums verified) → `self_test_schema_contract.py` initially caught the not-yet-fixed `MTS`-repo
consumers by name (`zmq_client`, `action_plan` import failures) *before* I'd touched them, then
passed clean after → `cargo build/clippy/test --workspace` clean for `mts_schema` → full clean
`./build_dll.sh` (PCH regenerated after the generated-header size change; the "redefinition of
kObservationDim" error was a PCH-staleness cascade, not an independent bug) → runtime
`import lbrnet.generated.MTS.Schema.Envelope` verified in both `lbrnet` and `MTS` Python envs
(not just `py_compile` syntax checks).

### 3.2 "MTS" is not actually overloaded — it's one acronym, correctly used, colliding with one
already-scheduled repo rename

**Correction (operator, 2026-10-09): "MTS" = "Mindful Trading System" — the name of the entire
system**, already documented as such elsewhere (`docs/PENDING_USER_ACTIONS.md`,
`docs/SIERRA_CHART_SETUP.md`: the Sierra Chart study itself is literally named `Mindful Trading
System` / `scsf_MindfulTrader`). So `namespace MTS.Schema`/`MTS.Training`/`MTS.Backtest` is **correctly
named** — it's the wire protocol for the Mindful Trading System. The `mts_*` Rust crate prefix is
the same acronym, consistently applied. This is not four unrelated things; it's one term, used
correctly, in three of the four places originally listed.

**The actual collision was narrower than first framed**: `/home/rcruz/devel/VSCode/MTS/` (the Plotly
Dash GUI repo) happens to share the acronym with the system-wide namespace it's a client of — so
`from lbrnet.generated.MTS.Schema.Event import Event`, read *inside* the `MTS` repo, has no textual
signal that this `MTS` means "the wire protocol," not "this repo." That reading confusion is real
(and is exactly the "fogginess" this audit was opened to address) — but it isn't a fresh naming
decision to make. **It's already ruled**: `docs/superpowers/specs/
2026-10-07-mindfultrader-monorepo-consolidation-spec.md`'s decision register (§12.4) already decided
`MTS` (repo) → `GUI/` (subdirectory of the merged monorepo) — "ruled," not proposed. Once that merge
step executes, the repo-path/namespace collision disappears on its own: `MTS` will unambiguously mean
only the system-wide acronym everywhere it appears (schema namespace, Rust crate prefix, Sierra Chart
study name), and the GUI will be addressed by path (`GUI/`), not by an acronym that collides with it.

**Disposition: RESOLVED — no new action from this audit.** Tracked by the existing, already-ruled
`MTS` → `GUI/` consolidation step. Recorded here as validating evidence for why that step matters
(this is a second, independent case — after `backtest_schema.fbs`'s live cross-language friction,
§3.3 — of a pre-existing ruled decision turning out to matter sooner than its own merge timeline
implied), not as a new open question.

### 3.3 Schema structural debt: the two-file include relationship has two independent workarounds

`backtest_schema.fbs` includes `mts_schema.fbs`. This single relationship had already forced two
separate, language-specific workarounds, found independently, months apart:

- **Python** (pre-existing, documented in `regenerate_schema.sh` itself, dated 2026-07-15): flatc's
  Python codegen re-emits every included `MTS.Schema` type as an empty stub when generating
  `backtest_schema.fbs` into the same output directory as `mts_schema.fbs` — verified by the
  script's own comment (`AsymmetryContext 95 lines → 5-line stub`). Worked around via an isolated
  staging directory, copying back only the backtest-unique subtree.
  - **Note the asymmetry with Rust (§3.3 cont'd)**: Rust needs the *opposite* fix — both schemas
    generating into the **same** directory (`--rust-module-root-file` merges them into one
    `mts::{schema,backtest,training}` tree; separate directories break cross-file references
    entirely, `E0433`). Two different language toolchains, two *opposite* workarounds, for the
    same underlying include relationship. That's strong evidence the relationship itself is
    fragile, not just that each toolchain needs its own incidental patch.
- **C++**: unaffected (its backtest pass writes a separate `backtest_schema_generated.h` and leaves
  `mts_schema_generated.h` intact) — but only because C++ headers don't re-emit included types the
  way Python's and Rust's whole-tree codegen does.

**Disposition: FIXED, 2026-10-09 — merged the two `.fbs` files into one.** My first recommendation
(fix only the Python/Rust *generation flags*, leave the schema structure alone) turned out to be
wrong on two counts, both caught before implementing: (1) the operator pointed out that backtesting
has *never been run*, so my objection to merging — "`BacktestFrame`'s `file_identifier`
verification is load-bearing" — assumed a proven mechanism that doesn't exist; (2) testing the
"narrow, Python-only fix" directly showed it doesn't work in isolation (`--gen-onefile` only
resolves cross-schema references if *both* schemas use it, so even that path would have touched the
live schema's Python packaging). With both schemas' Python-migration costs equal either way, the
merge became the better, more permanent option.
**The one real technical risk, checked empirically, not assumed**: FlatBuffers allows only one
`root_type`/`file_identifier` per schema file, and `flatc` silently keeps the *last* declared one on
conflict with zero warning (verified directly: a two-root test schema silently dropped the first
declaration). Resolved by checking what's actually load-bearing, not what's merely declared: **all 5
real `BacktestFrame` write call sites** (`BackTesterStudy.cpp`) call the generic
`FinishSizePrefixed(frameOff)` with no identifier argument, and **all 6 real `Envelope` write call
sites** (`EliteFlatBufferHelper.cpp`, `PositionManager.cpp`, `TelemetryAdapter.h`) call the generic
`Finish(envelope)` with no identifier argument either — neither `"LBRN"` nor `"BTST"` was ever
actually embedded by any hand-written code. Recorded as a new, separate, out-of-scope finding (both
identifiers are schema-declared but unused), not fixed here — fixing it would have been scope creep
beyond the structural merge.
**Real, non-hypothetical blast radius once past that**: this wasn't a zero-cost rename.
`src/BackTesterStudy.cpp` had a stale `#include "generated/backtest_schema_generated.h"` (removed —
already transitively available via the precompiled header). `rewrite_generated_python_imports.sh`
had *never* had a same-namespace-sibling rule for `MTS.Backtest` (only `MTS.Schema`/`MTS.Training`)
— a genuinely pre-existing gap (confirmed: the script has had zero `Backtest` rules since it was
written, unrelated to anything I changed), caught only because this session ran the first-ever real
import of `BacktestFrame.py` (`import MTS.Backtest.BacktestRecord` failed with
`ModuleNotFoundError: No module named 'MTS'`) — direct, concrete evidence for the operator's "we've
never run a backtest" point. Fixed (mirrored the existing `Schema`/`Training` rules).
**Bonus simplification, not originally planned**: merging also let Rust drop
`--rust-module-root-file` entirely — plain `--rust` now produces one clean `mts_schema_generated.rs`
(down from a 77-file tree), since there's no cross-file reference left to need the merge flag for.
**Full verification chain**: schema `flatc` syntax check → full `regenerate_schema.sh` run, all
three languages, one invocation each, all checksums pass → self-test passed → `cargo
build/clippy/test --workspace` clean for `mts_schema` → full clean `./build_dll.sh` (one real stale
`#include` caught and fixed, not a false alarm) → runtime Python imports verified for
`BacktestFrame`, `BacktestRecord`, `Envelope`, `TrainingEventT` in the `lbrnet` env. Updated the four
mirror docs (`CLAUDE.md`/`README-AI.md`/`GEMINI.md`/`copilot-instructions.md`) and one stale code
comment that named the now-nonexistent file.

### 3.4 `regenerate_schema.sh`'s organic complexity (process debt, same root cause)

Not naming, but the same "incremental growth without a consolidation pass" pattern, surfaced while
adding the Rust target today:

- `.bak_<timestamp>` backup rotation for C++/Python deploy targets — our own infrastructure guide
  (§5) already flags this as something to stop ("in a single repo, git is the backup"). The new
  Rust deploy path deliberately does **not** carry this convention forward (atomic stage-then-`mv`
  swap instead) — a small, already-executed example of "fix, don't propagate" from today.
- The script is 2,317 lines for what is conceptually "run flatc three ways, copy the output
  somewhere." Most of that length is legitimate (checksum validation, dry-run support, per-language
  post-processing), but it has clearly never had a structural refactor pass since its original
  authoring.

**Disposition: _(operator to decide — likely low priority relative to §3.1/§3.2/§3.3 since it's
purely internal to one script, not a cross-language naming contract)_**

### 3.5 Already-resolved precedents this session — evidence the methodology works, not open findings

Recorded here so they are **not** re-litigated as new findings:

- **`mt_`/`MT_` prefix → `mts_`** (this session, before `rust/` was scaffolded): caught because `mt_`
  collided with vcpkg's own "-mt" multi-threaded-CRT suffix convention already in this codebase,
  *and* with MetaTrader 4/5 in the trading-software domain. Renamed before anything depended on it.
- **`observation` (bare) → `observation_vector`**: caught because "observation" alone has no
  standalone meaning in this codebase (only ever used as the compound `ObservationVector`/
  `ObservationData`). Confirmed today (§3.1's survey work) that `ObservationData` is a real, distinct,
  already-taken schema struct name — validating the original decision rather than reopening it.

## 4. What this audit deliberately does NOT do

- It does not fix anything yet for the one still-open finding (§3.4 — marked `(operator to
  decide)`). §3.1, §3.2, and §3.3 are the three findings this pass already resolved — §3.1 and §3.3
  by executing the fix, §3.2 by recognizing it as already ruled elsewhere.
- It does not re-survey `lbrnet`'s or `MTS`'s internal (non-schema-touching) naming conventions —
  flagged as an open follow-up, not silently skipped (§5).
- It does not re-decide §3.5's (or now §3.1's/§3.2's/§3.3's) already-settled items.

## 5. Open questions for the operator

1. Should §3.4 be fixed now, deferred (with a reason), or accepted permanently? (§3.1, §3.2, §3.3
   no longer need a decision here — all three resolved, see their own sections.)
2. Should the already-ruled `MTS` → `GUI/` consolidation step (§3.2) be reprioritized earlier than
   its current place in the Phase 3 merge sequencing, given it just resolved a real, live piece of
   confusion rather than being purely a structural tidy-up?
3. Should a follow-up pass extend this audit to `lbrnet`'s and `MTS`'s own internal naming
   conventions (C++ class names, Python module names) beyond what touches the schema, or is the
   schema surface the right boundary to stop at for now?
4. Now that §3.1 and §3.3 are both resolved, does the Rust-adoption roadmap's Phase 2 sequencing
   (paused for this audit) resume as-is, or does §3.4 warrant finishing first so the round-trip
   proof is written against a final, settled process, not one still being cleaned up?
