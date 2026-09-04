# Design: Exhaustive Pattern Recording for Training, Selective Decisioning for Live

**Status**: Design, 2026-08-25 — synthesizes a brainstorm session into a concrete architecture.
Not yet implemented; not yet reviewed by the user. This is the next document in the chain after:

1. `2026-08-25-pattern-detection-institutional-hardening-spec.md` — Phase 0 diagnosis: refuted the
   original "sticky field" hypothesis, root-caused the real bug as a detector-authority conflict,
   root-caused Turtle Soup's 280x live-vs-training mismatch, root-caused ITR Breakout's zero count.
2. `2026-08-25-pattern-literature-grounding-and-subsumption-research.md` — literature grounding for
   the 4 fieldless patterns, data-availability audit, and a full 36-pair subsumption/composition
   audit across all 9 patterns.

This document answers the question those two left open: given the confirmed root causes and the
literature/subsumption findings, what should actually change in
`EventDataCollectorStudy.cpp`/`IndicatorManager.cpp`/the schema/the live Transformer feature path.

## 1. Core principle

**`EventDataCollectorStudy.cpp` must be exhaustive. Live trading must be selective. These are not
in tension — they were never solving the same problem.**

- **Exhaustive (training)**: every pattern that fired on a bar gets recorded independently,
  regardless of what else fired. Co-firing and subsumption relationships are recorded as data
  (features), never resolved by dropping a row. This is required by meta-labeling methodology
  (AFML ch. 3) — a hand-collapsed "winner" at recording time permanently destroys the ability to
  later ask whether pattern A's outcome differs when pattern B also fires, which is precisely the
  scalping-vs-high-alpha question this project cares about. Overlapping outcome windows from
  co-firing patterns are a sample-uniqueness problem (AFML ch. 4 — already an acknowledged
  meta-labeler prerequisite on this project's own roadmap), not a reason to discard data.
- **Selective (live)**: exactly one action gets taken per bar, because only one trade can be
  entered. Investigation this session found this is **already correctly implemented** — see
  §3.3. No new selection mechanism needs to be built for live execution.

The apparent tension the user raised ("do we pick the best pattern when several fire?") turned out
to have two different, non-conflicting answers depending on where in the pipeline the question is
asked — §3.3 and §3.4 spell out exactly where.

## 2. What's broken today (recap, full detail in the hardening spec)

- 5 of 9 patterns (Kangaroo Tail, Turtle Soup, Momentum Pinball, Elder Breakout, NR7) have their
  own dedicated `IndicatorKey`/schema field, each independently triggering a `.alpha` write via
  `IndicatorManager::CheckTrigger()`'s `EnteredOrExitedNone(...)` pattern
  (`IndicatorManager.cpp:409-448`). These are reliably captured today.
- 4 patterns (RSI Failure Swing, Stochastic Pop, ITR Breakout, ITR Fade) have **no dedicated
  field** — they exist only inside the legacy `DetectRaschkeTacticalTrigger()` cascade
  (`StudyHelperFunctions.cpp:1262-1535`), which writes to `raschke_tactical_trigger`, whose own
  `CheckTrigger()` case is a hardcoded `return false` (`IndicatorManager.cpp:396`). **A bar where
  one of these 4 fires and nothing else does may not be written to `.alpha` at all.**
- `raschke_tactical_trigger` is written by up to 5 different call sites per tick with no
  consolidation (the legacy cascade, plus separate "official" overwrite blocks for Elder Breakout,
  Turtle Soup, Kangaroo Tail, and NR7) — whichever runs last and passes its own gate wins for that
  bar, an accident of source-line order, not a designed precedence.
- The crude cascade's own Elder Breakout and Turtle Soup checks are proven non-canonical (weaker,
  differently-parameterized duplicates of the already-existing official detectors) by three
  independent sources: code trace, the Python port's own docstring, and literature.
- The Transformer's live `FeatureSpec` consumes `raschke_tactical_trigger` as a single scalar
  (`schema_contract.py:174`) — i.e. it trains on the corrupted, order-dependent field described
  above.

## 3. Architecture

### 3.1 New dedicated fields for the 4 currently-fieldless patterns

Mirror the existing 5-pattern precedent exactly:

- New `IndicatorKey` entries: `RSI_FAILURE_SWING`, `STOCHASTIC_POP`, `ITR_BREAKOUT`, `ITR_FADE`.
- Each gets its own Int8 enum row in the packed layout (`IndicatorLayout.h`/`IndicatorPackedState.h`,
  per the DOD/SoA architecture this codebase already uses for the other 5). **The quality Float32
  companion is NOT assumed here** — the hardening spec's own Phase 1 sketch left this as an
  explicitly undecided question (`schema/PENDING_SCHEMA_CHANGES.md` PSC-02: whether these 4
  oscillator/session-boundary-type triggers are inherently binary or warrant a continuous quality
  signal like the other 5). This design defers to that still-open decision rather than assuming an
  answer; whichever way PSC-02 resolves, the Int8 enum field is needed either way.
- Each gets its own `IndicatorManager::CheckTrigger()` case using `EnteredOrExitedNone(...)`,
  exactly like `KANGAROO_TAIL`/`TURTLE_SOUP`/`MOMENTUM_PINBALL`/`ELDER_BREAKOUT`/`NR7` today
  (`IndicatorManager.cpp:409-448`). This is what makes `EventDataCollectorStudy.cpp` reliably
  capture every firing — no changes needed in `EventDataCollectorStudy.cpp` itself, since it
  already just asks `HasSignificantChange()` and that machinery is what needs fixing.
- **Underlying detection logic**: phased, not blocking the field-wiring.
  - ITR Breakout/Fade: already literature-faithful (Section 4 of the research doc) — move the
    existing logic (`StudyHelperFunctions.cpp:1429-1532`) out of the crude cascade's priority chain
    and into its own dedicated detection path so it's no longer starved by the Elder-Breakout-crude
    check running first (Phase 0 item 4's root cause). This is a reachability fix, not a rewrite.
  - RSI Failure Swing: needs the real Wilder state machine (peak → pullback → failed retest →
    break of intervening extreme) over `Subgraph_RSI10`'s already-available full historical array,
    replacing the current same-bar RSI3-vs-RSI10 proxy (research doc §3). This is new detection
    logic, not just a wiring fix.
  - Stochastic Pop: needs the 3-ingredient literature definition (TS2's existing long-term
    stochastic + a Hurst-based proxy for the retired ADX<20 filter + a corrected short-term-surge
    threshold), replacing the current narrow-band proxy (research doc §2). The Hurst-based ADX
    proxy needs empirical validation before being trusted as equivalent, per the research doc's own
    caveat — this is the most implementation-uncertain piece of the whole design.
  - **Sequencing decision needed from the user**: wire the fields now with the current
    (acknowledged-weak) conditions as a placeholder, so `EventDataCollectorStudy.cpp` at least stops
    silently dropping these bars immediately — or hold the field-wiring until each detector's logic
    is corrected, so training data is never labeled with a known-wrong condition. Both are
    defensible; this document does not decide it (see §7).

### 3.2 Retire the crude cascade's duplicate Elder Breakout / Turtle Soup checks

`DetectRaschkeTacticalTrigger()`'s inline Elder Breakout check (`StudyHelperFunctions.cpp:
1418-1426`, near-universal "new high or low vs. prior bar") and inline Turtle Soup check
(`:1390-1404`, 4-bar lookback, no separation/quality/daily-proximity gates) are deleted outright,
per this project's own dead-code-removal mandate — not deprecated, not aliased. Their own dedicated
official detectors (`DetectElderBreakout()`/`DetectTurtleSoup()`, already wired at
`TripleScreen3.cpp:1066`/`:1209`) are the sole source of truth for these two patterns everywhere,
including whatever `raschke_tactical_trigger` continues to mean after this design (see §3.4). This
directly closes Phase 0 item 3's root cause (the 280x/68x Turtle Soup mismatch) at the source.

### 3.3 Live execution — confirmed unchanged

`PositionManagerPatterns.cpp`'s `CalculateTacticalTriggerPrices()` takes a `patternId` parameter
supplied by its caller as `static_cast<int>(prediction.actionId)`
(`PositionManager.cpp:1944-1945`) — the Transformer's own already-decided output, RPC'd from the
Python model. This function is a pure price-formula lookup table keyed by an action the model has
already chosen; it never reads the live value of `raschke_tactical_trigger` and never faces a
"multiple patterns fired, which one wins" decision. **No changes to this file.** The "select the
best pattern" question the brainstorm started from is the Transformer's own learned decision,
trained on Predator-Fusion-labeled historical outcomes (confirmed training-time-only per the North
Star's live decision architecture) — not a hand-codeable live selection mechanism.

### 3.4 Fix the Transformer's live FeatureSpec (the actual live-side action item)

The one real live-side defect: the Transformer's `FeatureSpec` (`schema_contract.py:174`) currently
consumes `raschke_tactical_trigger` as a single scalar input — i.e. it feeds the model the same
corrupted, order-dependent value the hardening spec diagnosed. Fix: the live feature construction
should read the 9 canonical per-pattern fields directly (the 5 existing + 4 new from §3.1), matching
whatever exhaustive representation `EventDataCollectorStudy.cpp` uses for training data (train/serve
parity — an existing governing principle on this project, `SCHEMA_DRIVEN_SERIALIZATION_PARITY_
INITIATIVE.md`). This is `lbrnet`-side work (the Transformer's feature contract lives there) —
flagged here as a dependency, not owned by this document.

**Open question this design does not resolve**: does `raschke_tactical_trigger` still need to exist
as a field at all, once (a) it's no longer the Transformer's own input (§3.4's fix) and (b) it's
confirmed never read live by `PositionManagerPatterns.cpp` (§3.3)? If no other live consumer is
found after a full-repo audit, it becomes a dead-code-removal candidate under this project's own
mandate. This document does not perform that audit — flagged as a prerequisite before removal,
not a decision made here.

### 3.5 Record confirmed cross-bar/structural relationships as explicit state, not left implicit

Per the subsumption research (§6.1, code-certain):

- **Momentum Pinball → ITR Breakout composite** (Section 1 of the research doc): add a persisted,
  per-day flag (alongside the existing `itrHigh`/`itrLow`/`itrEstablished` persistent variables,
  `StudyHelperFunctions.cpp:1453-1458`) recording whether the prior day's crude-cascade Pinball
  reading (the real LBR/RSI formula, `StudyHelperFunctions.cpp:1276-1325` — kept for this purpose
  even after Momentum Pinball's *own* dedicated field uses the official `DetectMomentumPinball()`
  logic) was extreme. This becomes an additional recorded fact — "was today's ITR Breakout
  Pinball-confirmed" — not a filter that suppresses ITR Breakout's own independent field.
- **ITR Breakout → ITR Fade dependency**: already correctly encoded in the existing
  `hadBreakoutAbove`/`hadBreakdownBelow` shared state (`StudyHelperFunctions.cpp:1457-1458`) — no
  change needed, just documented explicitly as a known sequential dependency (not a co-firing
  question) so it isn't miscounted during any future empirical co-fire analysis.
- **Structural-analog pairs (§6.2 of the research doc)** — Turtle Soup×ITR Fade, Elder
  Breakout×ITR Breakout, RSI Failure Swing×ITR Fade, NR7×Stochastic Pop: no schema change proposed
  here. These are flagged as the priority list for the empirical co-occurrence check named in §7,
  not yet confirmed enough to encode as fixed relationships.

## 4. Data flow (post-design)

```
Every tick, TripleScreen3.cpp:
  9 pattern detectors run independently (5 existing + 4 newly-added official detectors)
    -> each writes ONLY to its own dedicated IndicatorKey field
    -> IndicatorManager::CheckTrigger() fires a .alpha write per-pattern, independently

EventDataCollectorStudy.cpp:
  unchanged -- HasSignificantChange()/GetTrainingEventT() already correct once CheckTrigger()
  covers all 9 patterns; every pattern's firing is now captured, none silently dropped

lbrnet training pipeline (out of this repo's scope, flagged as a dependency):
  Predator Fusion labels historical pattern-fire events by realized outcome (training-time-only,
  confirmed by North Star architecture) -> feeds Transformer's supervised training set
  -> if the training scheme needs one ground-truth action per co-firing bar, tie-break by
     REALIZED OUTCOME, not a fixed importance table (this document's recommendation; not
     implementable from this repo)

Live:
  TripleScreen3.cpp's 9 per-pattern fields -> corrected Transformer FeatureSpec (lbrnet-side fix,
  §3.4) -> Transformer's own learned action decision (prediction.actionId, RPC'd) ->
  PositionManagerPatterns.cpp's CalculateTacticalTriggerPrices(patternId) -- unchanged, pure lookup
```

## 5. Testing

- Golden-vector tests for each of the 4 new detectors, once their logic is finalized (per §3.1's
  sequencing decision) — construct a synthetic bar sequence where the pattern's condition is true
  then false, assert the dedicated field transitions to/from `NONE` and that `CheckTrigger()` fires
  a significant-change event on the transition.
- A regression test confirming `DetectRaschkeTacticalTrigger()`'s deleted Elder Breakout/Turtle
  Soup branches are gone and the official detectors are the only source for those two fields
  (guards against reintroduction).
- A re-run of the hardening spec's own Section 3 sampling methodology, post-fix, confirming: (a)
  all 9 patterns now show non-zero, plausible firing rates in the `.alpha` stream; (b) the Turtle
  Soup three-rate discrepancy is resolved to one number (or a documented, deliberate remaining
  difference between live C++ and the Python training port).
- Full existing MindfulTrader C++ test suite stays green — no regressions to the 2026-08-16 audit's
  passing coverage.

## 6. Explicit non-goals

- Not implementing the Stochastic Pop Hurst-based ADX proxy's empirical validation, or the RSI
  Failure Swing state machine's exact parameters — those are follow-on implementation tasks, not
  decided by this design.
- Not performing the empirical co-occurrence audit named in §7 — this design proposes what to
  build; it doesn't re-derive the subsumption research's own open validation step.
- Not touching the Transformer's training code or Predator Fusion's labeling logic (`lbrnet`) — the
  training-label tie-break recommendation in §4 is a cross-repo dependency note, not something this
  repo can implement alone.
- Not deciding whether `raschke_tactical_trigger` gets removed outright (§3.4's open question) —
  that needs a full-repo consumer audit first.

## 7. Open questions requiring a decision before an implementation plan is written

1. **Sequencing** (§3.1): wire the 4 new fields now with placeholder (known-weak) detection logic,
   or hold until each detector is corrected to match the literature? This changes whether the
   implementation plan is one phase or two.
1b. **PSC-02** (`schema/PENDING_SCHEMA_CHANGES.md`, §3.1): do the 4 new patterns get a continuous
   quality Float32 companion like the existing 5, or is categorical-only correct for these
   oscillator/session-boundary-type triggers? Still open; this design doesn't resolve it.
2. **Hurst-as-ADX-proxy validation** (research doc §2, §8 item 2): needs an actual backtest before
   Stochastic Pop's corrected logic can be trusted — who runs this, and against what data?
3. **`raschke_tactical_trigger` removal** (§3.4): needs a full-repo consumer audit (grep every
   remaining reference, confirm none are load-bearing) before it can be scheduled as a dead-code
   removal. **Confirmed this session**: `TradeExecutionServer::CalculateOrderPrices()`
   (`TradeExecutionServer.cpp:824-907`) also switches on `RaschkeTacticalTrigger` values but has
   **zero call sites anywhere in the codebase** (verified via grep) — a separate, already-dead,
   hardcoded stub (its own trigger value is hardcoded to `MOMENTUM_PINBALL_BUY` regardless of the
   actual prediction, per its own "TODO: Extract tactical trigger from prediction message" comment)
   duplicating `PositionManagerPatterns.cpp`'s real logic with drifted, non-matching multipliers.
   **Full consumer audit completed 2026-09-04 (grep across the entire repo, all 289 matches
   triaged): `raschke_tactical_trigger` is NOT a safe removal candidate — it has a genuine,
   safety-critical live consumer beyond the dead `TradeExecutionServer` stub above.**
   `RiskManager::ValidateKurtosisEmergencyGate()` (`RiskManager.cpp:965-983`) reads the LIVE
   indicator value directly (`IndicatorManager::Instance().GetValue<IndicatorKey::
   RASCHKE_TACTICAL_TRIGGER>()`, not `prediction.actionId`) and uses it as a real hard-safety gate:
   during a kurtosis-emergency state, new trades are BLOCKED unless the current live trigger is a
   trend-continuation pattern (`IsTrendContinuationTrigger()`: `ELDER_BREAKOUT_*`, `NR7_BREAKOUT_*`,
   `ITR_BREAKOUT_*` only). This means the field's correctness directly matters for a live risk
   control, independent of the `PositionManagerPatterns.cpp`/`prediction.actionId` path row 13
   already documented — removing it, or leaving its writers in the detector-authority-conflict state
   this session's cascade-retirement fix partially addressed, has real safety consequences, not just
   a training-data-quality one. Every other match across the 289 found is either documentation, the
   shared `RaschkeTacticalTrigger` enum TYPE used for an unrelated purpose (e.g.
   `ChandelierStopManager::ShouldUseTrailingStop()` switches on a trigger parameter passed in at
   entry time, a snapshot, not a live re-read), or the wire-serialization writers
   (`IndicatorManager.cpp:1205/1270`) that export the field's value for training/telemetry, not a
   live decision consumer.
   This is its own independent dead-code-removal candidate, separate from the field question.
4. **Empirical subsumption validation** (research doc §6, closing caveat): the structural-analog
   and plausible-independent pairings are logical judgments, not measured correlations. Once the 4
   new fields exist and have collected real data, an empirical co-occurrence pass against `.alpha`
   history should confirm or revise §6.2/§6.4's groupings before treating any of them as settled.
5. **Cross-repo coordination**: §3.4 (Transformer FeatureSpec fix) and the training-label tie-break
   recommendation (§4) are `lbrnet`-side changes this repo cannot implement directly — needs a
   session in that repo, or joint coordination, before those pieces can close.

## 8. References

- `2026-08-25-pattern-detection-institutional-hardening-spec.md` (this repo)
- `2026-08-25-pattern-literature-grounding-and-subsumption-research.md` (this repo)
- `PRODUCTION_TRIAGE.md` row 13 (`/home/rcruz/devel/VSCode/PRODUCTION_TRIAGE.md`)
- `lbrnet/docs/architecture/PREDATOR_CROSS_PATTERN_ALPHA_RANKER_ASPIRATION.md` (downstream consumer
  of this work, gated on it per the hardening spec's own non-goals)
