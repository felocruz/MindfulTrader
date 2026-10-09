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
Rust (currently silenced by `mts_schema`'s lint-suppressing wrapper, not fixed at the source).
Rename to `MtsEnvelope` fixes it at the one place it actually lives — the `.fbs` file — rather than
suppressing it in every language's generated-code wrapper forever.

**Disposition: _(operator to decide)_**

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

`backtest_schema.fbs` includes `mts_schema.fbs`. This single relationship has already forced two
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

**Disposition: _(operator to decide — this is schema-structure, not naming; likely needs its own
scoped investigation into whether the include relationship can be simplified, e.g. a single merged
schema file, before deciding fix-now vs. defer)_**

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

- It does not fix anything yet for the still-open findings (§3.1, §3.3, §3.4 — marked `(operator to
  decide)`). §3.2 is the one finding this pass already resolved, by recognizing it as already ruled
  elsewhere, not by deciding anything new here.
- It does not re-survey `lbrnet`'s or `MTS`'s internal (non-schema-touching) naming conventions —
  flagged as an open follow-up, not silently skipped (§5).
- It does not re-decide §3.5's (or now §3.2's) already-settled items.

## 5. Open questions for the operator

1. Which of §3.1, §3.3, §3.4 should be fixed now, vs. deferred (with a reason), vs. accepted
   permanently? (§3.2 no longer needs a decision here — resolved, see §3.2.)
2. §3.1 (`MTS_Envelope` → `MtsEnvelope`) is the narrowest, cheapest fix here, and — now that §3.2 is
   understood to be a repo-path/acronym collision rather than a naming-convention question — it no
   longer needs to wait on anything else. Is it the right first mover?
3. Should the already-ruled `MTS` → `GUI/` consolidation step (§3.2) be reprioritized earlier than
   its current place in the Phase 3 merge sequencing, given it just resolved a real, live piece of
   confusion rather than being purely a structural tidy-up?
4. Should a follow-up pass extend this audit to `lbrnet`'s and `MTS`'s own internal naming
   conventions (C++ class names, Python module names) beyond what touches the schema, or is the
   schema surface the right boundary to stop at for now?
5. Once §3.1/§3.3/§3.4 dispositions are decided, does the Rust-adoption roadmap's Phase 2 sequencing
   (paused for this audit) resume as-is, or does it need reordering so schema-naming fixes land
   before the round-trip proof work (so the proof is written against final names, not names about
   to change)?
