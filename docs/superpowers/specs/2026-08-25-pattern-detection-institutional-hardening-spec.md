# Spec: Institutional Hardening of the 9-Pattern Detection/Recording Pipeline

**Status**: Drafted 2026-08-25, mid-investigation -- this is a live working draft, not a
reviewed-and-approved spec. Written from the `lbrnet` side of a cross-repo brainstorming session
(see `lbrnet/logs/rc_gemini.log` and `lbrnet/scratchpad.md`, 2026-08-25 entries) that set out to
generalize Predator Fusion's pattern-classifier architecture across all 9 named patterns
(`docs/architecture/PREDATOR_CROSS_PATTERN_ALPHA_RANKER_ASPIRATION.md`, lbrnet repo). Before that
generalization can be trusted, this project's own standing discipline ("diagnose before fix," "no
p-hacked shortcuts") requires establishing that the underlying C++ detection/recording is actually
correct. It is not yet clear that it is -- this spec exists to find out and fix what's broken.
**Phase 0 (Section 4) is NOT complete** -- two concrete anomalies are documented with strong
circumstantial evidence, not yet root-caused to a specific line of code. Do not skip to Phase 1's
fixes without finishing Phase 0's diagnosis first.

## 1. Purpose

MindfulTrader detects 9 Raschke/Elder pattern setups live in C++ (Turtle Soup, Kangaroo Tail,
Momentum Pinball, Elder Breakout, NR7 Breakout, ITR Breakout, ITR Fade, RSI Failure Swing,
Stochastic Pop) and records their fired/quality state into every live event via
`IndicatorState`/`RaschkeTacticalTrigger` (`mts_schema.fbs`). The lbrnet-side plan to generalize
per-pattern training-data construction (and eventually a cross-pattern alpha ranker) across all 9
patterns depends on this recorded state being trustworthy. A first data-quality pass against real
historical `.alpha` data found it is not, in at least two concrete ways (Section 3). This spec's
purpose is to make the 9-pattern detection/recording pipeline **iron-clad** -- correct, consistent,
tested, and documented -- before any Python-side generalization work builds on top of it.

## 2. Institutional grounding

The 9 patterns implement setups from Linda Bradford Raschke's *Street Smarts* (1995) and Alexander
Elder's trend/breakout methodology (`RaschkeStrategySetup`/`RaschkeTacticalTrigger` docstrings,
`lbrnet/core/rc_enums.py:371-471`, cite the source text directly per-pattern). The correctness bar
this spec applies is not "does it compile" -- it is **does the live-recorded state actually mean
what its own docstring says it means**, verified against real historical data, not just unit-test
synthetic inputs. This is the same standard already applied earlier this week to Elder Breakout (a
real directional-fusion bug found and fixed 2026-08-16, commit `4b0753a`) and to Kangaroo Tail/
Momentum Pinball (audited clean, 2026-08-16) -- this spec extends that same audit discipline to the
gaps that audit did not cover: `raschke_tactical_trigger`'s cross-pattern semantics, NR7's
post-deletion state, and the 4 patterns with zero dedicated Python-side representation.

## 3. Evidence (verified against real data, 2026-08-25)

**Method**: sampled the first 2,000,000 events of `lbrnet/data/raw/event_data.alpha` (of
~57M total) via the corrected `event.Indicators()` nested accessor (see Section 3.4 -- the
existing `lbr_to_polars.py` reader is stale and crashes on the current schema shape). Confirmed the
sample itself is not degenerate: 2,000,000 distinct timestamps out of 2,000,000 events, 3,502
distinct `nr7_quality` values, `turtle_soup_quality` nonzero in 251,151 events -- the underlying
recording pipeline is genuinely live and varying, not frozen or stubbed.

### 3.1 `raschke_tactical_trigger` value distribution (18 pattern x direction values + NONE)

```
NONE=352,660  KT_BUY=275      KT_SELL=1,306     TS_BUY=2         TS_SELL=4
PB_BUY=328,084 PB_SELL=253,443 EB_BUY=267,215   EB_SELL=162,105
NR7_BUY=192,892 NR7_SELL=171,270
ITR_BO_BUY=0    ITR_BO_SELL=0  ITR_FADE_BUY=7,595  ITR_FADE_SELL=12,831
RSI_BUY=106,377 RSI_SELL=101,260  STOCH_BUY=14,879  STOCH_SELL=27,802
(sums exactly to 2,000,000 -- confirmed no unaccounted values)
```

**Anomaly A -- implausibly high firing rates for 4 of 9 patterns.** Momentum Pinball fires (either
direction) in 29.1% of all events, Elder Breakout in 21.5%, NR7 Breakout in 18.2%, RSI Failure
Swing in 10.4%. Raschke's own tactical-trigger framing (`rc_enums.py:420-428`) describes these as
"precise entry confirmation when Screen1 (trend) and Screen2 (setup) align" -- a genuinely rare,
multi-timeframe-aligned condition. A one-in-three-to-one-in-ten-bar hit rate is not consistent with
that definition.

**Leading hypothesis, not yet confirmed**: `raschke_tactical_trigger` is sticky (carries forward
across bars until a *different* non-NONE value overwrites it) rather than reset to `NONE` each bar
a pattern's condition stops holding. Evidence for this, found by reading the call sites directly:

- `TripleScreen3.cpp:872,877` (`Update(KANGAROO_TAIL_BUY/SELL)`) and `TripleScreen3.cpp:1315,1318`
  (`Update(TURTLE_SOUP_BUY/SELL)`) are both called ONLY inside the positive-condition branch of an
  `if` -- no sibling `Update(RaschkeTacticalTrigger::NONE)` call was found in either file for these
  two patterns specifically (grepped `MindfulTrader/src/*.cpp` for the literal call, none found).
- `IndicatorManager.cpp:396`: `CheckTrigger()`'s `case RASCHKE_TACTICAL_TRIGGER: return false` --
  this indicator never independently triggers an event write on its own change (unlike e.g.
  `LONG_IMP`, which does check its own dirty bit). Combined with `IndicatorManager.cpp:1270`
  (`state.mutate_raschke_tactical_trigger(GetValue<...>())`, called unconditionally for every
  outgoing event regardless of whether this field changed), the CURRENT stored value -- whatever it
  last was -- gets copied into every single event.
- `RaschkeTacticalIndicator` (`Indicator.h:1036-1040`) has no `ShouldTrigger()` override, unlike
  `KangarooTail`'s own indicator class (`Indicator.h:1054-1064`), which explicitly overrides
  `ShouldTrigger()` to fire on `entered`/`exited` transitions of ITS OWN enum value -- meaning
  `KangarooTail`'s own dedicated enum field likely DOES reset to NONE on exit, but the SEPARATE
  `RaschkeTacticalTrigger` field that also gets set when Kangaroo Tail fires may not.
- Two `return RaschkeTacticalTrigger::NONE;` fallback returns exist in
  `StudyHelperFunctions.cpp:1268,1534` -- **not yet confirmed** which pattern(s)' detection
  functions these belong to, or whether their NONE return value actually reaches
  `raschkeTacticalIndicator->Update(...)` (as opposed to being computed but never fed into the
  sticky field). This is exactly the kind of asymmetry (some patterns correctly transient, others
  not) that would explain both this anomaly and Anomaly B below.

**If confirmed sticky**: every downstream consumer of `raschke_tactical_trigger` -- including this
project's own `2026-08-24-two-classifier-risk-sizing-architecture-spec.md`'s live `FeatureSpec`
(the Transformer already trains on this field, per `schema_contract.py:174`) -- has been training
on a corrupted signal: "was pattern X the most recent thing to fire" conflated with "is pattern X's
setup condition true right now." These are not the same feature, and a Transformer trained on the
former while believers think it sees the latter is a real, live train/serve-adjacent correctness
bug, not just a future-generalization blocker.

### 3.2 Turtle Soup: live trigger rate vs. validated Python dataset rate, 280x apart

`raschke_tactical_trigger` shows Turtle Soup firing **6 times in 2,000,000 events** (2 buy, 4 sell)
-- a rate of 3 per million. Turtle Soup's own validated Python pipeline
(`lbrnet/scripts/build_turtle_soup_dataset.py`) produced 48,602 labeled candidate rows from the
full ~57M-row history -- a rate of ~853 per million, **~280x higher**. This is not sampling noise
at n=2,000,000 (expected count at the dataset's implied rate would be ~1,700, not 6).

The separate, non-tactical-trigger `turtle_soup` field (`TurtleSoupEnum`, a broader
setup-state signal, distinct from the "all screens aligned" tactical trigger) shows a
still-different rate: 116,632 non-NONE values in the same 2,000,000-event sample (5.8%) -- itself
~68x higher than the trigger rate, and still not matching the Python dataset's 0.085% either
(directionally closer, not reconciled). **Three different numbers for "did Turtle Soup happen
here," none matching another, is not yet explained.** Possible causes, none yet confirmed:
different detection thresholds between the live C++ geometric check and the Python
`detect_turtle_soup()` port; the Python port scanning a materially different lookback/window;
`raschke_tactical_trigger`'s sticky-field problem (Section 3.1) suppressing Turtle Soup's true rate
because higher-frequency patterns (Momentum Pinball especially) win the sticky slot almost every
bar, leaving little room for Turtle Soup's rarer condition to ever get recorded there at all --
this last one, if true, would mean Turtle Soup's *true* live condition-firing rate could be much
higher than either recorded field currently shows.

### 3.3 Coverage gaps, confirmed via the earlier cross-repo audit (2026-08-25, same investigation)

- **NR7's dedicated `DetectNR7()` function was deleted** (`docs/superpowers/specs/
  2026-08-06-indicator-orphan-cleanup-design.md`, confirmed zero production callers at the time).
  Live NR7 detection is now inline in `TripleScreen3.cpp:1394-1520`, with no dedicated unit test at
  the function level (only exercised indirectly via `test_indicator_computations.cpp`'s broader
  coverage, unconfirmed whether it actually reaches this inline path). Despite this, `nr7`/
  `nr7_quality` ARE being populated live (confirmed: 3,502 distinct `nr7_quality` values, `nr7`
  present at every one of 2,000,000 sampled events) -- so the inline replacement works at some
  level, but has no direct regression coverage proving it computes the RIGHT thing, only that it
  computes SOMETHING.
- **Quality-score asymmetry**: 5 of 9 patterns (Kangaroo Tail, Turtle Soup, Momentum Pinball, Elder
  Breakout, NR7) have a dedicated continuous `*_quality: float` field in `IndicatorState`
  (`mts_schema.fbs:223-228`). The other 4 (ITR Breakout, ITR Fade, RSI Failure Swing, Stochastic
  Pop) have only the categorical `raschke_tactical_trigger`/`raschke_strategy_setup` enum value --
  no continuous strength signal of their own. Not yet determined whether this is an intentional
  design choice (these 4 are inherently binary/oscillator-cross triggers with no natural continuous
  "quality") or a genuine gap.
- **`ITR_BREAKOUT_BUY`/`ITR_BREAKOUT_SELL` fired zero times** in the full 2,000,000-event sample --
  the only one of the 18 trigger values with a literal zero count. Worth confirming this pattern's
  detection logic is even reachable/wired, not just rare.

### 3.4 Tooling bug found along the way (lbrnet-side, not MindfulTrader, noted for completeness)

`lbrnet/data/lbr_to_polars.py` reads a stale, pre-migration schema shape -- it calls
`event_t.RaschkeTacticalTrigger()` etc. directly on the `TrainingEvent` table, which no longer has
these accessors after the "v2.5: Indicator State Struct (Zero-Copy) -- Replaces the Wide Table of
40+ fields" schema migration (`mts_schema.fbs:219`) nested them under `event_t.Indicators()`
instead. It hard-crashes (`ValueError: TrainingEvent schema contract violation`) on the current
`.alpha` file. `lbrnet/scripts/validate_lbr_file.py` already uses the correct nested accessor
(`validate_lbr_file.py:327`, `event.Indicators()`) -- confirms this is a `lbr_to_polars.py`-specific
staleness, not a schema problem. Tracked here because it blocked this investigation and would block
any future one; the actual fix belongs in `lbrnet`, not this spec's own MindfulTrader scope.

## 4. Phase 0 -- mandatory diagnosis

**Status update, 2026-08-25 (continued session, MindfulTrader-rooted, full C++ + lbrnet source
access): items 1-4 below now have a confirmed root cause each, with file+line citations on both
sides of the C++/Python boundary. Item 5 (re-sample post-fix) is correctly deferred until Phase 1
fixes land. The original "sticky field" hypothesis (Section 3.1) is REFUTED as stated -- the real
defect is more precise, and arguably more consequential: a detector-authority conflict, not a
missing reset.**

### 4.0 Corrected mechanism (supersedes Section 3.1's sticky-field hypothesis)

`TripleScreen3.cpp:710` calls `DetectRaschkeTacticalTrigger()`
(`StudyHelperFunctions.cpp:1262-1535`) **unconditionally, every tick**, and that function DOES
return `RaschkeTacticalTrigger::NONE` explicitly when nothing matches (the lookback guard at
`StudyHelperFunctions.cpp:1268` and the final fallback at `:1534`) -- so the field is NOT
"never reset" as Section 3.1 hypothesized. The real defect: **this single legacy cascade function
and up to 4 separate "official" per-pattern blocks later in the same tick's execution
(`TripleScreen3.cpp`) all write to the same `raschkeTacticalIndicator`, with no consolidation step
and no shared quality bar** -- whichever one runs last in source-code order and has its own gate
pass wins for that bar; if a later official block's gate fails, the earlier value (possibly the
crude cascade's) stands unchallenged. Per-pattern breakdown:

- **Momentum Pinball**: TWO independent implementations exist. The crude one (inline in
  `DetectRaschkeTacticalTrigger()`, `StudyHelperFunctions.cpp:1276-1325` -- "3-period RSI of ROC
  <30/>70") is what actually reaches the field. The current/real one (`DetectMomentumPinball()`,
  `TripleScreen3.cpp:921`, with proper RSI-cross + Stochastic + Impulse + volume + Hurst-alignment
  logic) explicitly does **not** write to the field any more -- its own call site's comment
  (`TripleScreen3.cpp:986`) says "Logic removed: RaschkeTacticalTrigger updates (moved to
  Python/Orchestrator)". The field's Momentum Pinball value is sourced from an
  acknowledged-retired proxy; the actual current detector's output never reaches it.
- **RSI Failure Swing / Stochastic Pop**: exist ONLY as the crude cascade's own simplified checks
  (`StudyHelperFunctions.cpp:1328-1384`), whose own comments admit the simplification ("Would need
  RSI array to compare historical RSI values properly... Current implementation simplified"). No
  official detector overrides these at all -- what's recorded is the only implementation that
  exists, and it is a known-weak proxy by its own author's comment.
- **Elder Breakout / Turtle Soup**: BOTH exist in two forms that disagree on definition. Crude:
  Elder fires on any new high-or-low vs the immediately prior bar (`StudyHelperFunctions.cpp:
  1418-1426`, no consolidation/volume/Hurst gate -- true on the large majority of bars); Turtle
  Soup uses a 4-bar lookback undercut-then-reclaim (`TURTLE_SOUP_LOOKBACK=4`,
  `StudyHelperFunctions.cpp:637,1390-1404`). Official: Elder requires Keltner-channel breakout +
  consolidation + volume + Hurst gating (`DetectElderBreakout()`, `TripleScreen3.cpp:1066`, forwarded
  unconditionally once STRONG/EXTREME per its own comment "Forward ALL breakouts... Orchestrator
  will filter"); Turtle Soup requires a 20-bar lookback (`TURTLE_SOUP_LENGTH=20`,
  `TripleScreen3.cpp:1166`) + 4-bar separation from the prior extreme + STRONG/EXTREME quality +
  proximity to the **prior day's** high/low (`atDailyHigh`/`atDailyLow`, `TripleScreen3.cpp:
  1257-1258,1314,1317`). Whichever ran last and passed its own gate wins; when the official gate
  fails, the crude (near-universal, for Elder) value from line 710 stands for that bar.
- **Kangaroo Tail / NR7**: each has exactly one detector, cleanly gated (quality>=0.6 for
  Kangaroo Tail; always-STRONG for NR7 breakout) -- architecturally sound relative to this field.
- **ITR Breakout / ITR Fade**: exist only in the crude cascade, positioned AFTER the near-universal
  Elder-Breakout-crude check in the same if/return priority chain -- see item 4 below.

**Net effect**: `PositionManagerPatterns.cpp`'s price-routing consumers and the Transformer's live
`FeatureSpec` consumers are both reading a field whose provenance (which of up to 2 competing,
differently-gated algorithms fired last) is an accident of source-code line order, not a designed
priority. This is a different, and arguably worse, finding than "sticky" -- it's not that the field
fails to reset, it's that for 7 of 9 patterns either the only implementation is an admitted proxy,
or two disagreeing implementations race with no reconciliation.

### 4.1 Root causes, items 1-4

1. **(was: find call sites)** Done -- see 4.0's breakdown above. Confirmed: `RASCHKE_TACTICAL_TRIGGER`
   never independently triggers its own event write (`IndicatorManager.cpp:396`,
   `CheckTrigger() -> false`), and `state.mutate_raschke_tactical_trigger(GetValue<...>())`
   (`IndicatorManager.cpp:1270`) copies the current stored value into every outgoing event
   unconditionally -- both exactly as Section 3.1 originally cited, still accurate, just not
   evidence of "stickiness" once 4.0's per-bar-reset mechanism is understood.
2. **(was: sticky vs intentional design)** Reframed per 4.0 -- not a sticky/transient question.
   The real design question: should there be ONE authoritative detector per pattern (retire the
   crude cascade's per-pattern duplicates in favor of the already-existing, better-gated official
   ones for Elder Breakout/Turtle Soup/Momentum Pinball; build "official" detectors for RSI Failure
   Swing/Stochastic Pop/ITR, which currently have none), with an explicit priority/precedence rule
   if more than one pattern can legitimately fire on the same bar? `PositionManagerPatterns.cpp`'s
   price-routing use (confirmed live consumer, `src/PositionManagerPatterns.cpp:203-280`) needs
   this decided just as much as the Transformer's `FeatureSpec` training use does.
3. **(was: Turtle Soup 280x/68x mismatch)** ROOT-CAUSED, three independently diverging filter
   stacks under one name:
   - Python's candidate generator (`lbrnet/lbrnet/data/intrabar_snapshot.py::
     build_intrabar_snapshots()`, called from `lbrnet/lbrnet/scripts/build_turtle_soup_dataset.py:147`)
     emits a candidate for **every (closed-bar, tau-fraction) pair** -- 9 sub-bar tau fractions per
     bar (`_TAU_FRACTIONS`, `intrabar_snapshot.py:21`) -- wherever a raw 20-bar-extreme
     penetration-then-recovery holds, with **zero** gates: no minimum-penetration-ATR filter, no
     separation-from-prior-extreme filter, no daily-high/low proximity, no quality tier
     (`intrabar_snapshot.py:124-137`). This alone multiplies raw opportunity count ~9x per bar
     versus any single-evaluation-per-bar C++ path, before any filter difference is even counted.
   - The intermediate `turtle_soup` field (`TurtleSoupEnum`, 5.8% rate) reflects
     `DetectTurtleSoup()`'s real gates (`_MIN_PENETRATION_ATR=0.05` + WEAK/STRONG/EXTREME tiers,
     `lbrnet/lbrnet/data/turtle_soup_detector.py:52-104`, confirmed byte-for-byte port of
     `IndicatorComputations.h:768`) plus the 4-bar separation filter -- but NOT the daily-high/low
     gate.
   - `raschke_tactical_trigger`'s own Turtle Soup count (3/million) additionally requires
     STRONG/EXTREME-only (excludes WEAK) AND proximity to the **prior day's** high/low -- a
     materially rarer, session-level condition absent from both the Python pipeline and the
     `turtle_soup` field's own gate.
   - Confirms the spec's own suspicion, independently: `turtle_soup_detector.py`'s docstring
     (lines 8-11) already states "four_day_high/four_day_low... always the prior 20-closed-bar
     high/low, not a literal 4-day window" -- the Python side is self-aware that "Turtle Soup" here
     means the C++ 20-bar definition, not the classic literature 4-day one; the literature-name
     ambiguity is not the live bug, the filter-stack divergence is.
   - **Free verification available, no new run needed**: `build_turtle_soup_dataset.py`'s own
     output already carries a `shape_confirmed` boolean per candidate (`label_candidate()`,
     `build_turtle_soup_dataset.py:124-127`) recording whether `detect_turtle_soup()`'s properly-
     gated logic agrees with that candidate. Filtering the existing
     `data/training/turtle_soup_snapshots.parquet` (48,602 rows) by `shape_confirmed=True` gives an
     apples-to-apples comparison against the C++ `turtle_soup` (`soupEnum`) field's own gate,
     without re-running detection.
4. **(was: ITR_BREAKOUT zero count)** ROOT-CAUSED, not dead/unreachable code by construction but
   architecturally starved: within `DetectRaschkeTacticalTrigger()`'s priority-ordered if/return
   cascade, the crude Elder-Breakout check (any new high or low vs the immediately-prior bar --
   true on the large majority of bars) is evaluated BEFORE the ITR block
   (`StudyHelperFunctions.cpp:1418-1427` precedes `:1429-1532`), so the function returns early on
   nearly every bar before ever reaching ITR Breakout's code. ITR Breakout is only reachable on the
   rare true "inside bar" (no new high AND no new low vs the prior bar -- consistent with
   `ITR_FADE`'s own observed ~1% combined rate) AND additionally requires high-volume confirmation
   AND a fresh once-per-day-per-direction break of the day's opening range. That conjunction is
   rare enough that 0-in-2,000,000 is plausible without a separate reachability bug, though a
   larger and/or targeted sample (inside bars only) would confirm it's "very rare" rather than
   "impossible."

**Do not proceed to Phase 1 fixes until every item below has a confirmed root cause, per this
project's own "diagnose before fix" discipline (root CLAUDE.md, Fix Before Launch gate).** Items
1-4 above now meet that bar. Item 5 remains open by design (post-fix verification).

1-4. Superseded by §4.0/§4.1 above (confirmed root causes with file+line citations, 2026-08-25
   continued session) -- kept only as a record of the original diagnostic questions.

5. Re-run this spec's Section 3 sampling methodology (or extend it to a tail sample, not just head,
   per this project's own head-vs-tail sampling precedent) once Phase 1 fixes land, to confirm the
   fix actually changed the observed distribution in the expected direction -- not just that the
   code changed.

## 5. Phase 1 -- fixes (scope depends on Phase 0's findings; sketched, not final)

**Narrow slice DONE, 2026-09-04**: retired the crude cascade's (`DetectRaschkeTacticalTrigger()`,
`StudyHelperFunctions.cpp`) duplicate Momentum Pinball/Turtle Soup/Elder Breakout checks -- each
already has a properly-gated official detector in `TripleScreen3.cpp` that now exclusively owns
`raschke_tactical_trigger` for its pattern (Momentum Pinball's official detector already didn't
write to this field at all -- "moved to Python/Orchestrator" -- so the crude check was the only
thing populating it; removing it means no C++ write for that pattern any more, matching the stated
intent). Removed the crude checks entirely rather than adding a reset, since the official detectors
are strictly better-gated, not reformulated the crude ones. 6 now-dead Pinball-only constants
removed from `PatternConstants` (`-Werror=-Wunused-const-variable` caught them immediately).
`./build_dll.sh --no-clean` clean. **Not yet done, still open**: RSI Failure Swing/Stochastic
Pop/ITR still only exist as the crude cascade's own admitted-simplified proxies (no official
detector to defer to yet); the schema-split question below is untouched; Phase 0 item 5's
re-sampling (confirm the fix changed the observed firing-rate distribution) not yet run.

- If `raschke_tactical_trigger` genuinely conflates "last significant trigger" (a legitimate,
  sticky, position-management-routing signal) with "is this pattern's setup true right now" (what
  live feature-consumption implicitly assumes): split into two fields, or make the field's
  transience explicit and correct for whichever semantic is actually needed by which consumer.
  Update `mts_schema.fbs`'s own field comment to state the true semantics precisely (currently says
  neither) -- this is exactly the kind of comment-vs-code drift this project's CLAUDE.md flags as a
  standing risk. **Any actual field-shape change is tracked as `schema/PENDING_SCHEMA_CHANGES.md`
  PSC-01** -- update that entry's status (PROPOSED -> DECIDED or REJECTED) as part of resolving this
  item, do not decide the schema question here in isolation from the other 2 consumers.
- Reconcile Turtle Soup's three conflicting rates to one, root-caused number, once Phase 0 item 3
  is done. If the live C++ detector and the Python port genuinely use different criteria, decide
  which is authoritative and bring the other in line -- do not silently keep both.
- Add dedicated unit test coverage for NR7's inline detection logic (`TripleScreen3.cpp:1394-1520`)
  at the function level, closing the gap left by `DetectNR7()`'s deletion -- verify it actually
  reproduces the pattern's defining geometry (compression, then expansion), not just that it
  returns *some* value.
- Decide, with a documented rationale (not silently), whether the 4 quality-score-less patterns
  (ITR Breakout/Fade, RSI Failure Swing, Stochastic Pop) need a continuous quality signal added, or
  whether categorical-only is correct for oscillator-cross-type triggers (**tracked as
  `schema/PENDING_SCHEMA_CHANGES.md` PSC-02** -- update its status once decided, either direction).
  If added, follow the same formula-grounding discipline as the existing 5 (each pattern's quality
  formula should cite its literature rationale, matching `DetectTurtleSoup()`'s own `base_score +
  close_back_bonus +
  close_position_bonus` precedent).
- Confirm or fix `ITR_BREAKOUT_BUY/SELL`'s live reachability (Phase 0 item 4's finding).

## 6. Explicit non-goals

- **Not the Python-side PatternAdapter/cross-pattern generalization itself.** That work (tracked in
  `lbrnet/docs/architecture/PREDATOR_CROSS_PATTERN_ALPHA_RANKER_ASPIRATION.md`) is explicitly
  gated on THIS spec landing first -- building it against unverified/possibly-sticky C++ state would
  bake today's data-quality problems into a training set, the exact mistake this spec exists to
  prevent.
- **Not re-litigating the 2026-08-16 Kangaroo Tail/Momentum Pinball/Elder Breakout direction-
  discrimination audit** (already passed) -- this spec's Momentum Pinball/Elder Breakout concern is
  specifically about `raschke_tactical_trigger`'s cross-pattern stickiness, not their own dedicated
  enum fields or quality formulas, which that audit already covered.
- **Not fixing `lbr_to_polars.py`** (Section 3.4) -- that's an `lbrnet`-repo fix, tracked separately,
  noted here only because it blocked this investigation.
- **Not deciding the pattern_id encoding for any future cross-pattern classifier** (e.g. whether
  `raschke_tactical_trigger`, once fixed, becomes THE canonical `pattern_id` feature the aspiration
  doc wants) -- that's a design decision for the Python-side spec, downstream of this one, not to be
  pre-decided here.

## 7. Acceptance gates

- Every item in Phase 0 (Section 4) has a written, code-cited root cause -- not a hypothesis, a
  confirmed finding (file + line + verified behavior).
- `raschke_tactical_trigger`'s actual semantics (transient vs. intentionally-sticky, per-pattern if
  they differ) are documented in `mts_schema.fbs`'s own field comment, matching what the code
  verifiably does -- not what a docstring assumed it did before this investigation.
- The Turtle Soup three-rate discrepancy (Section 3.2) is resolved to one number with a stated
  reason for any remaining, intentional difference between live C++ and the Python port.
- Re-sampling this spec's Section 3 methodology after fixes lands shows firing rates consistent
  with each pattern's own literature-grounded rarity (no pattern firing >10-15% of bars, as a sanity
  ceiling pending real per-pattern base-rate research -- not an invented final number).
- NR7's inline detection has dedicated unit test coverage proving it reproduces the compression-
  then-expansion geometry `DetectNR7()` originally implemented, not just "runs and returns a value."
- Full existing MindfulTrader C++ test suite stays green -- no regressions to the 2026-08-16 audit's
  already-passing coverage.

## 8. Testing

- Golden-vector tests for whichever call sites Phase 0 identifies as needing a NONE-reset fix --
  construct a synthetic bar sequence where a pattern's condition is true then false, assert the
  recorded field returns to NONE (or documents why it deliberately doesn't).
- A dedicated NR7 unit test (Phase 1) using a hand-constructed narrow-range-then-breakout bar
  sequence with a known expected detection outcome.
- A cross-language count-reconciliation test: run the live C++ detection logic (or its already-
  validated Python port) against a fixed historical window and assert candidate counts match within
  a documented tolerance -- the concrete mechanism for closing Section 3.2, not just a one-time
  manual check.
- Re-run this spec's own Section 3 sampling script (should be written as a small, reusable
  diagnostic -- not a throwaway, since Phase 0 item 5 requires re-running it) against a post-fix
  `.alpha` file, before and after, to prove the fix changed the observed distribution.

## 9. Residual risk

- This spec's Section 3 evidence comes from a 2,000,000-event HEAD sample of one `.alpha` file, not
  a tail sample or a full-file scan. This project's own precedent (HMM training's head-vs-tail
  sampling divergence) means the true full-file rates could differ from what's reported here --
  directionally, the anomalies are large enough (280x, 29% vs. expected rarity) that sampling
  variance alone is very unlikely to be the explanation, but Phase 0 item 5's re-sampling step
  should use a larger and/or tail sample before treating any fix as fully validated.
- `RaschkeTacticalTrigger`'s sticky-vs-transient question may not have a single right answer --
  different consumers (`PositionManagerPatterns.cpp`'s price-routing use vs. the Transformer's live
  feature use) may have been relying on different, currently-conflated semantics for a while. Phase
  0 item 2 explicitly plans for "split into two fields" as a real possible outcome, not just "fix
  the bug" -- changing this field's behavior without checking every existing consumer risks fixing
  one bug by introducing a regression in whichever consumer actually wanted the sticky behavior.
