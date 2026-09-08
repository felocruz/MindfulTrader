# Labeling & Data Augmentation — Gang-Statistical Reformulation Initiative

**Status, opened 2026-09-06: seeded entirely from an external operator/Gemini brainstorm session
conducted outside this repo (`/mnt/c/Trading/MTS_Fractal_Evolution.txt`, no repo access, no
in-thread citation verification, no code). Twin/sibling of
`docs/superpowers/specs/2026-09-04-technical-analysis-gang-statistical-reformulation-initiative.md`,
`docs/superpowers/specs/2026-09-04-indicator-gang-statistical-reformulation-initiative.md`, and
`docs/superpowers/specs/2026-09-06-risk-gating-gang-statistical-reformulation-initiative.md` — same
discipline, a fourth scope: the labeler and data-augmentation pipeline (primarily `lbrnet`-owned,
per this repo's own boundary — "no ML training logic (belongs in `lbrnet`)" — but documented from
this repo because the brainstorm happened here and touches this repo's own `ImbalanceBarEngine.h`
and the co-evolution-governed TRAP definition). Nothing implemented, nothing independently
verified. Read §1 before anything else — it flags a real governance conflict with an already-ruled-
on definition that must not be silently overridden.**

## 0. Origin and mandate

Same "is this statistically rigorous, or an ad hoc heuristic" audit already applied to chart
patterns, indicator formulas, and risk-gating signals, now applied to the labeler/augmentation
layer: how bars are aggregated for training, how forward-looking labels (`ENTER_LONG`/`TRAP_LONG`/
etc.) are assigned, and how in-flight trajectory samples are generated for sequence-model training.
This is squarely `lbrnet`'s domain per `CLAUDE.md`'s own project-boundary rule, so anything here is
a candidate proposal for a future `lbrnet`-rooted session to evaluate, not something this repo
should implement unilaterally — same posture already established for this repo's other
`lbrnet`-handoff docs (e.g. `docs/superpowers/specs/2026-08-26-activity-clock-lbrnet-handoff.md`).

## 1. CRITICAL: possible conflict with the already-ruled-on TRAP definition — read before anything else

**This repo has an existing, formal, co-evolution-governed ruling on what TRAP means**
(`docs/ADR/triple_barrier_trap_definition_ruling.md`, referenced throughout `CLAUDE.md`'s Trap
Detection section): **TRAP = structural invalidation of the entry thesis, specifically a
`FAILED_*` structural reversal test (the reactive floor) or its anticipatory Transformer-predicted
counterpart (`TRAP_*`), gated by the dynamic Bayesian threshold τ\* = C_FP/(C_FP+C_FN) (Elkan
2001).** The 2026-07-15 scope-split ruling is explicit and deliberate: an adverse `DECISIVE_*`
counter-break is a SEPARATE `REGIME_INVALIDATION` resolution, **never** TRAP — because lumping the
two produces a multimodal target that degrades the anticipatory model (this is a settled, reasoned
conclusion, not an arbitrary line).

**The brainstorm's proposed labeling rule (§2.2 below) defines `TRAP_LONG`/`TRAP_SHORT` as: volume
imbalance surges (`θ_T ≫ 0`) while price progress stagnates (`Y_imb → 0`) alongside positive state
entropy growth (`dH_norm/dN > 0`).** This is a plausible, self-consistent construction on its own
terms (a liquidity-absorption/exhaustion signature), but **it was proposed with zero awareness of
the existing ruling** — it does not reference `FAILED_*` structural reversal tests, τ\*, or the
`DECISIVE_*`/`REGIME_INVALIDATION` split at all. Two honest possibilities, not yet resolved:

1. The brainstorm's construction is a genuinely different, complementary signal (e.g. a candidate
   INPUT feature that could inform whether a `FAILED_*` reversal test is more or less likely at a
   given moment), not a redefinition of TRAP itself.
2. The brainstorm's construction is an alternative, competing definition of what should count as
   TRAP — in which case it must go through the same reasoned adjudication process the existing
   ruling went through (`docs/ADR/triple_barrier_trap_definition_ruling.md`'s own precedent,
   originally a Gemini-adjudicated ruling per `coevolution_governance.md`), not be adopted by
   quietly substituting a new formula into the labeler.

**Do not wire any of §2 below into `triple_barrier_scanner.py`'s actual `TRAP_*`/`ENTER_*` label
assignment without first reconciling this against the existing ruling** — per this repo's own
co-evolution governance rule (`coevolution_governance.md`: "ground it in the literature FIRST...
if grounded, BOTH sides implement the same grounded version identically... the incumbent
implementation having a feature is NOT evidence the feature is correct" — cuts both ways: the
INCUMBENT ruling isn't automatically right either, but it was reasoned through and documented, and
a new candidate needs the same rigor to override it, not silent substitution).

## 1a. Wider possibility, made explicit: this may replace the Triple-Barrier Method itself, not just reconcile TRAP within it

**§1 above framed the question narrowly — does §2.2's labeling rule redefine TRAP within the
existing Triple-Barrier framework. The wider, more consequential possibility must be stated
explicitly: the Thermodynamic Look-Ahead Labeler (§2.2) is a structurally different labeling
PARADIGM from Triple-Barrier itself, not merely a different TRAP formula inside it.** Triple-Barrier
(López de Prado, AFML Ch. 3) assigns a label via a first-hit-wins race among three fixed barriers
(static stop, static target, vertical time limit). §2.2 instead assigns a label
(`ENTER_LONG`/`ENTER_SHORT`/`TRAP_LONG`/`TRAP_SHORT`/`NO_EDGE`) directly from thresholds on a
forward-looking Imbalance Yield Ratio and entropy-rate signature — no stop/target/time barriers at
all. **This initiative's scope explicitly includes evaluating whether this becomes a genuine
replacement of Triple-Barrier as this system's labeling scheme**, not only whether it can safely
coexist as an additional signal inside it.

**This is a categorically bigger claim than §1's TRAP-reconciliation question**, for two reasons:

1. **The static stop/target/vertical-time barrier design is itself already a settled, grounded
   decision**, not an open question — `coevolution_governance.md`'s own record: "Static stop /
   target / vertical(time): GROUNDED (AFML Ch.3). Keep both sides." Proposing to replace it needs
   at least the same rigor (literature grounding, then Gemini-adjudicated ruling, then coordinated
   `MindfulTrader`/`lbrnet` implementation) that produced that original decision — not a brainstorm
   substituting for it.
2. **This directly reopens a question already deliberately parked, not a fresh one.** The
   trade-execution-risk-management initiative's own §0a
   (`docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md`)
   already raised "is a distance-multiple stop/target the right FORM at all, vs genuinely separate
   institutional disciplines (meta-labeling for sizing, trend-scanning for winner exits,
   regime-conditioned optimal stopping for loser invalidation)" and explicitly parked it: "Real,
   substantive directions, worth returning to, but each is its own real research project... not a
   same-session task." §2.2 is a candidate concrete answer to that exact parked question, arriving
   from an independent direction (an activity-clock/information-theoretic labeling rule, rather than
   AFML's own named alternative disciplines) — the two should be evaluated together when this is
   picked up, not treated as unrelated.

**Not a decision, an explicit scope statement**: this doc does not conclude Triple-Barrier should be
replaced — only that the question is now explicitly in scope, alongside (not instead of) §1's
narrower TRAP-reconciliation question. Whichever is resolved first should inform the other, since a
full Triple-Barrier replacement would make §1's TRAP-redefinition question moot (TRAP's own
`FAILED_*`/`DECISIVE_*` split is itself a Triple-Barrier-era construct).

## 2. Candidate constructs (CANDIDATE, unverified, no cited literature grounding in the source brainstorm)

**Same honesty flag as the risk-gating twin doc**: none of the constructs below carry a literature
citation in the source brainstorm — no named paper, no verified precedent. Plausible synthesis of
real concepts (imbalance bars, forward-looking barriers, sequence augmentation) already used
elsewhere in this repo's own work, not verified academic constructs.

### 2.1 Imbalance Clock Aggregator — adaptive threshold, a genuine gap vs. this repo's existing engine

Brainstorm's proposed bar-generation rule: accumulate signed volume `θ_T = Σ b_k·v_k`; emit a bar
when `|θ_T| ≥ b_target`, where `b_target = E[T]·|E[b_k·v_k]|` is itself updated via EWMA
(`α=0.01` in the brainstorm's own pseudocode) after every bar closes — i.e. the threshold adapts to
recently-observed tick count and imbalance-per-tick, not a fixed constant.

**Real, concrete gap found by checking this repo's actual code (not assumed)**: `include/
ImbalanceBarEngine.h` uses a **fixed** `m_imbalanceThreshold = 50.0f` (confirmed by direct read,
2026-09-06) — no EWMA adaptation exists today. This is the one candidate in this whole doc with a
clear, checkable implementation gap against real code, not just a conceptual comparison.

**Literature-grounding CHECKED 2026-09-06 — direct match, not approximate.** AFML Ch. 2's
information-driven (imbalance) bars define exactly this construction: accumulate signed flow
`θ_T = Σ b_k·v_k`, sample a new bar when `|θ_T| ≥ E_0[T]·|E_0[b_t·v_t]|`, and López de Prado's own
prescribed estimation method for `E_0[T]`/`E_0[b_t·v_t]` is an EWMA over prior realized bars — the
identical mechanism as the brainstorm's `b_target` formula and its `α`-updated EWMA. This is not
"the same general territory," it's the same formula and the same estimation approach; the
brainstorm's own citation gap (not naming AFML) is cosmetic, not substantive — the underlying
mechanism is textbook, already this repo's own established precedent for information-driven bars
elsewhere. Promotes this specific gap from "worth checking" to "confirmed real, confirmed
literature-grounded, ready for an implementation decision" — independent of §1/§1a's still-open
TRAP-definition question, which concerns §2.2 below, not this adaptive-threshold mechanism.

### 2.2 Thermodynamic Look-Ahead Labeler — see §1/§1a before reading this as a TRAP definition or a Triple-Barrier replacement

```
Y_imb(k) = ΔP / Σ b_k·v_k                          -- Imbalance Yield Ratio over forward horizon k∈[1,K_max]
TRAP_*   : θ_T ≫ 0  AND  Y_imb → 0  AND  dH_norm/dN > 0     -- imbalance surge, no price progress, entropy rising
ENTER_*  : Y_imb high, dH_norm/dN < 0, H > 0.55              -- imbalance surge, yield holds, entropy falling, persistent
```

Mechanistically this is a liquidity-absorption/exhaustion detector (aggressive flow producing
minimal price displacement) — the same conceptual family as Force Index divergence (indicator
initiative §1) and Wyckoff Spring/Upthrust stop-harvest-reversal (TA-patterns initiative §3,
case study #2), NOT a verified TRAP-equivalent per §1's governance flag. **Directly relevant to an
already-open question**: the TA-patterns initiative's own §7 asks "does an imbalance-run-based
stop-harvest/reversal signal feed the existing TRAP framework, or become an independent
pattern-detection addition?" — this brainstorm section is a concrete, if unvetted, candidate answer
to that exact open question. Cross-reference and resolve together, don't duplicate.

### 2.3 t-FSM Life-Cycle Data Augmentation

Proposes stepping through an active position's lifespan in imbalance-clock increments, emitting
per-step training samples with an in-flight context vector (`[elapsed_imbalance_bars,
unrealized_pnl, peak_yield, local_entropy_delta]`) and a discrete FSM action token
(`HOLD_POSITION`/`EXECUTE_TRAP_FADE`/`EXIT_THERMAL_DECAY`/`STAND_DOWN`) derived from thresholds on
the same signals as §2.2. This is a sequence-augmentation strategy, not a single-bar label — its
soundness depends entirely on §2.2's own labels being correct/reconciled first (§1); not
independently assessable until that's resolved.

## 3. Relationship to existing work — do not duplicate or silently override

- **`docs/ADR/triple_barrier_trap_definition_ruling.md`** — the authoritative existing TRAP
  definition; §1 above is the load-bearing cross-check, not optional context.
- **`docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md`
  §0a** — the already-parked "is a distance-multiple stop/target the right FORM at all" question;
  §1a above makes explicit that §2.2 is a candidate concrete answer to it, arriving independently.
  Evaluate together when either is picked up.
- **`lbrnet/docs/labeling/LABELING_AND_AUGMENTATION_SPEC.md`** (referenced throughout `CLAUDE.md`
  as the canonical TRAP-weighting-policy normative default; not re-read into this doc, this repo's
  workspace can't reach the `lbrnet` folder directly) — any eventual `lbrnet`-side work here must
  reconcile against that spec, not this brainstorm alone.
- **`docs/superpowers/specs/2026-09-04-technical-analysis-gang-statistical-reformulation-
  initiative.md` §3 (case study #2, Wyckoff Spring/Upthrust)** — §2.2 above is a candidate answer
  to that doc's own open question 5 (§7); resolve together.
- **`include/ImbalanceBarEngine.h`** — §2.1's adaptive-threshold idea is a concrete, scoped
  enhancement candidate to this repo's OWN C++ engine (not just an `lbrnet`-side concern), since
  every activity-clock dim (`fast_hurst_exponent`, `fast_taleb_kurtosis`, `skewness_idx`, the
  now-dropped `fast_mean_rev_z`) depends on this engine's bar boundaries.
- **`coevolution_governance.md`** (repo memory) — governs how any eventual change here must be
  adjudicated across `MindfulTrader`/`lbrnet` if it ever moves past CANDIDATE.

## 4. Status vocabulary

Same as the sibling ledgers: **CANDIDATE** (proposed, not yet built) · **OPEN** (actively being
investigated) · **BLOCKED** (real work identified, blocked on something else finishing first).
Everything in §2 is **CANDIDATE** pending: (a) §1/§1a's TRAP-definition and Triple-Barrier-
replacement reconciliation (a hard blocker for §2.2/§2.3, not optional — §1a's claim is
categorically bigger than §1's and needs commensurately more rigor before any implementation), (b)
a real literature-grounding pass for §2.1 (AFML Ch. 2's own adaptive-bar-threshold precedent, not
yet cited in-thread despite being genuinely applicable), and (c) cross-reference resolution with
the TA-patterns initiative's own case study #2.

## 5. Next steps (not started)

1. **Resolve §1/§1a first, before anything else in this doc.** Determine whether §2.2's
   construction is (a) a candidate NEW feature/signal, (b) a competing TRAP definition, or (c) a
   candidate full Triple-Barrier replacement — each needs a different, escalating level of
   adjudication rigor (§1a explicitly requires at least the same rigor that produced the original
   AFML Ch.3 grounding decision). Do not substitute quietly at any of these three levels.
2. ~~Literature-ground §2.1's adaptive-threshold mechanism against AFML Ch. 2~~ — **CHECKED
   2026-09-06: direct match, not approximate.** See §2.1's updated bullet — this specific gap
   (fixed `m_imbalanceThreshold` vs. EWMA-adaptive) is now confirmed real AND literature-grounded,
   ready for an implementation decision independent of items 1/3/4's still-open TRAP/Triple-Barrier
   questions (§2.2 only).
3. Resolve the TA-patterns initiative's own open question 5 (§7 there) together with §2.2 here,
   not independently.
4. Only after 1-3: decide whether any of this becomes an `lbrnet`-side labeler change (requires its
   own `lbrnet`-rooted session per this repo's project boundary) or stays a `MindfulTrader`-side
   `ImbalanceBarEngine.h` enhancement (§2.1 only, does not require touching the labeler).
