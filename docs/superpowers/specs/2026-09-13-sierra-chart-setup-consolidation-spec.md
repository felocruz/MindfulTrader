# Sierra Chart Setup Consolidation Spec

## 1. Origin

Puget Workstation bring-up (`docs/PUGET_SETUP_COORDINATION.md` Entry 12) reached the point of
needing to re-verify Sierra Chart's own settings (data feed, execution, Volume Profile, replay
config) against the new machine. That content already existed, but scattered across three docs
written at different times for different purposes, with real duplication risk: `docs/ADR/
sierra_chart_data_feed_setup.md` (a decision record) and `docs/PENDING_USER_ACTIONS.md` §1/§2 (a
hands-on checklist) both carried their own copy of the same TWS/IB Gateway + Sierra Chart settings
steps. Operator asked for a single centralized reference doc rather than continuing to check/update
two copies going forward.

## 2. Decision — single source of truth, ADRs stay historical

- **New durable doc**: `docs/SIERRA_CHART_SETUP.md` — the SSOT for every current Sierra Chart
  setting this project depends on (data feed/execution config, Volume Profile prerequisites,
  replay/chartbook config for training-data export). This is what gets read and updated going
  forward whenever a setting changes or is re-verified on a new machine.
- **`docs/ADR/sierra_chart_data_feed_setup.md` stays as a historical decision record** — its role is
  documenting *why* Package 11 + Denali + IB-execution-only was chosen (Sierra Chart's own
  documented IB bid/ask-volume-accuracy warning, Package 12/MBO evaluation, the Volume-Profile-proxy
  finding). The step-by-step "how to configure" list is removed from here (now duplicated in the
  SSOT) and replaced with a pointer.
- **`docs/PENDING_USER_ACTIONS.md` §1/§2 trimmed** to a pointer — the rest of that doc (§3-9) is
  investigation/deploy-verification tracking for specific observation-vector fixes, not general
  settings, and stays untouched.
- **`docs/TRAINING_DATA_EXPORT.md` stays as its own workflow doc** (replay procedure, JSONL output
  format, troubleshooting) — it's a procedure, not a settings list, so it isn't merged wholesale.
  Gets a cross-reference pointer to the SSOT for the underlying chart settings it depends on
  (Intraday Data Storage Time Unit, Days to Load).

This mirrors the project's own existing convention: ADRs (`docs/ADR/*`) record decisions and are not
rewritten to track ongoing operational state; specs/plans record initiatives; a small number of
living reference docs (`GUI_INDICATOR_REFERENCE.md`, etc.) are the ones actually kept current.

## 3. New doc structure (`docs/SIERRA_CHART_SETUP.md`)

1. Purpose/pointer map (this doc vs. the ADR vs. `TRAINING_DATA_EXPORT.md` vs.
   `NEW_MACHINE_WSL_SETUP.md`'s install steps)
2. Package/feed/execution settings (from the ADR + `PENDING_USER_ACTIONS.md` §1)
3. Volume Profile / Intraday Data Storage prerequisites (from `PENDING_USER_ACTIONS.md` §2)
4. Chartbook restore pointer (already covered by `NEW_MACHINE_WSL_SETUP.md` §12a — not duplicated)
5. Per-machine verification status table — settings are machine-local Sierra Chart state, so a
   fresh install (e.g. Puget) starts every row as unverified regardless of what the old machine had
   confirmed; this table tracks that explicitly instead of letting old-machine verification silently
   imply new-machine correctness.

## 4. Non-goals

- Not re-deciding the package/feed choice itself — that rationale is untouched, just relocated to
  being ADR-only rather than duplicated.
- Not touching `TRAINING_DATA_EXPORT.md`'s replay procedure content.
- Not a `PUGET_SETUP_COORDINATION.md` entry rewrite — that log keeps its own append-only entries;
  this spec's execution gets one short new entry noting the doc restructuring happened, per that
  file's own protocol.
