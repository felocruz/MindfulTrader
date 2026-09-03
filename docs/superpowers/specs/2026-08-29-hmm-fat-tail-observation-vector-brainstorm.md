# Brainstorm (EVOLVING): a rich, non-redundant observation vector spanning the market's known state axes — letting the HMM discover whatever states (fat-tail or otherwise) that basis actually supports

**Status: EVOLVING — this is a live working document, not a decided spec.** It accumulates findings
as the brainstorm continues and is expected to be edited in place, not superseded by dated
follow-ons, until it converges on a final recommended dim set. **Promotion criterion (objective,
added 2026-08-29): converged when every row in §9's ledger is in a terminal status (`IN`, `OUT`, or
`OUT-HMM`) or explicitly triaged out with a stated reason — no `CANDIDATE`, `PAUSED`,
`IN-UNMEASURED`, or `IN-PENDING-FIX` rows remaining.** When that's true, promote the final list to
a real implementation plan (own dated file) and mark this one CLOSED. **Not yet ground truth — see
§8 for the current gap list.**

**Freshness: verified against live repo state as of 2026-08-30 (later same day), HEAD `4969259`
plus committed work on top through `MindfulTrader` `0f27eee`, `schema` `477b750`/`c7788f8`.
`./build_dll.sh --no-clean` confirmed succeeding end-to-end.** §10 (the C++/Arrow `.context`
converter, all 13 tasks), §10.7 (`drift_location_eval`), and the new §10.8 (`jump_ratio_eval`) are
all **implemented, natively tested, and committed** — no longer design-only. §5.0/§9 row 20
(drift/location) is a **tested, rejected** result; §5.1/§9 row 21 (jump/bipower-variation ratio) is
now a **tested, SURVIVES** result (real, substantial effect — see §5.1 and §10.8), not yet promoted
to a schema field (§6.2's governance still applies before that happens). A real methodology gap was
found and documented, not yet fixed, during §5.1's review: both `drift_location_eval` and
`jump_ratio_eval`'s bootstrap/hit-rate tests treat heavily-overlapping forward-return signals as
i.i.d., understating CI/significance — see §10.8's own writeup and §10.9 (new) for the retroactive
note on §10.7. Uncommitted, edited-this-session: `SCRATCHPAD.md`, `ContextManager.h`/`.cpp`,
`TripleScreen2.cpp`/`3.cpp`, `StudyHelperFunctions.cpp`/`.h`, `include/FeatureScaler.h`,
`config/execution_params.json`, and various untracked pre-existing spec files unrelated to this
thread. Re-check this stamp (or update it) before trusting any claim below that depends on current
repo/schema state, rather
than assuming it still holds.

## 0. Goal and scope

**Goal, revised 2026-08-29 (second revision, sharpened on user correction) — this is a "let the
data speak" document, not a "make the model find X" document.** The job is not to engineer dims
until a fat-tail state appears — that would be manufacturing a predetermined outcome, the same
methodological error as curve-fitting. The job is to give the HMM the **richest, most
non-redundant basis spanning the market's known axes of state variation** (§1's taxonomy: trend/
mean-reversion, volatility level, liquidity regime, contagion/clustering, tail-weight, among
others; same "let the data speak" doctrine already cited in §1.2's own source literature —
Nystrup et al.) and then accept whatever EM actually finds — a discrete fat-tail state, a fat-tail
state that only emerges combined with trend/volatility (§1.4/§1.7's reframe), or no discrete
fat-tail state at all. That last outcome is a legitimate finding, not a failure to fix: if a
genuinely rich, non-redundant vector still produces no low-ν state, the honest next questions are
whether K=4 has room (§2's capacity hypothesis) or whether this instrument's tail behavior is
better modeled as continuously time-varying than as a discrete regime at all — not "add more
fat-tail features until one appears."

**"Best data" means non-redundant and axis-grounded, not maximal.** A dim earns its place by
demonstrating measured cross-state discrimination against a **named axis from §1's taxonomy**, and
by not being redundant with something the model already estimates natively (§4's ν_k-redundancy
finding) or with another dim already in the vector — not by volume, and not merely by being
"literature-groundable" in the abstract (the failure mode this document was originally written to
correct, kept below for the record).

**Guardrail carried over from the narrower version, still binding under the broader goal**: this
session's activity-clock observation-vector work (kurtosis/skewness twins, `fractal_dim`
window-widening, `recurrence_rate`'s move, `mean_rev_z`/`hurst_exponent`'s paused twins) started
drifting from "fix the thing that's actually broken" into "apply the activity-clock treatment to
every dim that's mechanically eligible, selected by literature-groundability rather than by
relevance to *any* named market-state axis." Caught directly by the user 2026-08-29 ("are we
blindly meandering?"). Broadening the goal must not reopen that door — "this helps some regime
axis" is not sufficient justification on its own; it needs the same measured cross-state-ratio
evidence every other row in §3 is held to.

**Operating constraint driving today's scope (2026-08-29, user-stated)**: regenerating a
`.context`/`.alpha` training file costs ~3 days. The goal for *today* is to get `MindfulTrader`'s
observation vector into its best possible shape *before* paying that cost once, not to iterate
live against real collected data. A high-power ML workstation arrives in ~2 weeks, removing the
current memory/CPU constraints — expensive machinery (Hawkes MLE fitting, heavier retraining
cycles) is better suited to wait for that than to rush now. A skewed Student-t emission
distribution (Hansen 1994; Fernández & Steel 1998) is a separate, already-noted fallback if the
current symmetric Student-t proves structurally inadequate — out of scope for *this* document,
which is about the observation vector, not the emission distribution; tracked in `PRODUCTION_
TRIAGE.md` row 1's existing notes.

**Explicit instruction governing §1 below, 2026-08-29**: the state taxonomy in §1 is deliberately
**not primed or constrained by the current model's K=4, its DOF values, or anything else about
what this system currently can or cannot do**. §1 asks a clean-slate question — what states does
the institutional literature associate with markets in general — independent of implementation. §2
onward is where the current system's actual evidence re-enters.

---

## 1. Institutional literature: canonical market states (independent of current K)

**Purpose of this section**: before assuming "fat-tail state" is the only latent structure worth
hunting for, or that K=4 is even the right number of *anything*, ask what the regime-switching
literature has actually found markets decompose into, across seven decades of the field. This list
is a menu of what's been found to be real and recurring — not a target K, not a claim that this
system must reproduce all of it.

### 1.1 The foundational split: Hamilton (1989)

**Hamilton (1989)**, *A New Approach to the Economic Analysis of Nonstationary Time Series and the
Business Cycle*, *Econometrica* 57(2) — the paper that started regime-switching econometrics.
Originally macro (GNP growth: expansion vs. recession), the direct financial-return extension
becomes a 2-state split:
- **Bull**: high mean return, low volatility.
- **Bear/Crisis**: negative or low return, high volatility.

A common 3-state refinement adds a middle state — **Bull / Normal / Crisis** — where Normal carries
near-zero mean return at moderate volatility, distinct from both tails. The core recurring finding
across this literature: **volatility and mean return are regime-coupled, not independent axes** —
high-vol states systematically carry lower (often negative) mean returns, not just wider dispersion
around an unchanged mean.

### 1.2 Guidolin & Timmermann (2007) — four states, jointly across stocks and bonds

**Guidolin & Timmermann (2007)**, *Asset Allocation under Multivariate Regime Switching*, *Journal
of Economic Dynamics and Control* — a specific, well-cited four-state taxonomy fitted jointly to
stock and bond returns:
- **Crash**: sharply negative returns, highest volatility.
- **Slow Growth**: modest positive returns, below-average volatility.
- **Bull**: strong positive returns, low volatility.
- **Recovery**: a transitional state following a crash, distinct dynamics from steady-state Bull.

Named here as a concrete example of what a 4-state fit produces on a *different* asset pair (joint
equity/bond), not as validation of this system's own K=4 — the states found are a property of the
data-generating process being modeled, not a universal constant. Worth noting anyway: "Recovery" as
its own distinct state (not just "Bull, but recently post-crash") is a structure this system has
never explicitly considered — its transition dynamics (elevated volatility decaying from a crash,
not yet back to steady-state Bull) are a genuinely different regime from either Bull or Crash.

### 1.3 Ang & Bekaert (2002, 2004) — the high-volatility bear regime carries a correlation signature too

**Ang & Bekaert (2002)**, *International Asset Allocation With Regime Shifts*, *Review of Financial
Studies* 15(4); **Ang & Bekaert (2004)** — international equities characterized by a high-volatility
bear regime with **spikes in cross-market correlation**, lower mean returns, coinciding with bear
markets. The finding that matters beyond "high vol, low return" (already covered by §1.1): **the
state is also marked by a correlation regime shift**, not just a within-asset statistical shift.
For a single-instrument system (MES only, no cross-asset correlation feed), this dimension isn't
directly measurable today — flagged as a structural limitation, not solved here.

### 1.4 The practitioner quadrant: trend × volatility

A widely-used (though not single-paper-attributable) framework crosses two axes — **trendiness**
(persistent/trending vs. mean-reverting/choppy) and **volatility level** (low vs. high) — producing
four cells:
- **Trending, Low-Vol**: steady grind, momentum/trend-following favored.
- **Range-Bound, Low-Vol**: compressed, sideways drift.
- **Choppy, High-Vol**: large swings, mean-reverting at the daily level, momentum strategies get
  chopped up.
- **Trending, High-Vol**: strong direction with large daily noise — **crisis periods**, "only the
  trend itself is tradeable."

**This is worth sitting with**: the "Trending, High-Vol" cell is, in different vocabulary, a
description of a *crash/panic* regime — directional, violent, exactly the character of the fat-tail
episodes this whole initiative is chasing. The Hurst exponent is the literature's own standard
proxy for the trend axis; volatility-level dims (`relative_range`, `log_scale_ratio`,
`log_scale_expansion_ratio`) already cover the other axis. **A genuine hypothesis worth testing later**:
the "fat-tail state" this project has been hunting for as a single scalar-tail-shape phenomenon may
actually be better described as the *intersection* of two axes already partially in the vector
(trend persistence × volatility level) rather than a state requiring a wholly new tail-specific
feature. Not acted on here — logged as a reframing worth testing once retraining is possible.

### 1.5 Liquidity-crisis regime — a distinct causal mechanism, not just "high volatility"

**Brunnermeier & Pedersen (2009)**, *Market Liquidity and Funding Liquidity*, *Review of Financial
Studies* — describes liquidity spirals as a mechanistically distinct state driver: funding
constraints force deleveraging, which withdraws market liquidity, which widens price impact, which
forces further deleveraging. Already informally present in this vector via `amihud_illiquidity`/
`liq_fragility` (§2 below) — named here because the literature treats it as its **own** regime
category, not a subtype of "high volatility."

### 1.6 Contagion / self-exciting clustering regime

**Aït-Sahalia, Cacho-Diaz & Laeven (2015)**, *Journal of Financial Economics* — jump arrivals
modeled as self-exciting: a jump raises the intensity of further jumps, producing clustering, with
intensity mean-reverting until the next episode. Distinct from a static "crisis" label — this is a
regime defined by its *temporal dynamics* (elevated, decaying hazard of another extreme move), not
just its instantaneous moments. Full detail in §5.2 below (candidate feature, not yet built).

### 1.7 The fat-tail / Talebian state (already this project's working target)

Extreme, rare, discontinuous-jump-dominated — realized kurtosis/tail-index far from Gaussian,
Student-t ν low enough that variance itself is a poor risk descriptor. This is the state this whole
initiative already targets; listed here for completeness against the other six, and to make the
comparison explicit: **§1.4's "Trending, High-Vol" and §1.7's "fat-tail" may be the same underlying
phenomenon described by two different literatures** (regime-switching-asset-allocation vs.
extreme-value-theory), not two separate states this system needs to find independently.

### 1.8 Summary table

| State (literature) | Source | Core signature | Currently representable in this vector? |
|---|---|---|---|
| Bull | Hamilton (1989) | High return, low vol | Partially — no direct return-level dim, inferred from vol dims |
| Bear/Crisis | Hamilton (1989) | Low/negative return, high vol | Same as above |
| Normal (middle) | 3-state extension | Near-zero return, moderate vol | Same as above |
| Crash | Guidolin & Timmermann (2007) | Sharp negative return, highest vol | Partial (vol dims), no explicit crash-vs-generic-high-vol separator |
| Slow Growth | Guidolin & Timmermann (2007) | Modest return, below-avg vol | Partial |
| Recovery | Guidolin & Timmermann (2007) | Post-crash transitional dynamics | **Not represented** — nothing distinguishes "post-crash decay" from steady-state |
| High-vol bear + correlation spike | Ang & Bekaert (2002, 2004) | Vol + cross-market correlation shift | **Not representable** — single-instrument system, no cross-asset feed |
| Trending-High-Vol (crisis quadrant) | Practitioner quadrant | Trend axis × vol axis intersection | Partially — `hurst_exponent` (trend) × vol dims exist, never explicitly crossed |
| Liquidity crisis | Brunnermeier & Pedersen (2009) | Funding/liquidity spiral | Yes — `amihud_illiquidity`, `liq_fragility` |
| Contagion/self-exciting clustering | Aït-Sahalia et al. (2015) | Elevated, decaying jump hazard | **Not represented** — `burstiness_index` gestures at it informally, no real hazard estimate |
| Fat-tail / Talebian | This project | Extreme discontinuous jumps, low ν | Newly attempted via `fast_taleb_kurtosis`, untested |

---

### 1.9 Orthogonal-axis decomposition (2026-08-29, user-directed reframe)

**Different question from §1.1-§1.8**: those sections ask "what named *states* does the literature
find." This section asks "what genuinely **orthogonal statistical axes** can a return-generating
process vary along at all" — decompose the space first, then map every current/candidate dim onto
exactly one axis. This surfaces gaps and redundancies by construction, rather than by post-hoc
correlation checking one pair at a time. The classical core of this decomposition is the four
moments (mean, variance, skewness, kurtosis); everything past that is a market-microstructure/
dynamics axis layered on top, each independently motivated, not part of the classical moment
framework.

**Governing principle for this table, carried from §0's goal statement**: an axis with zero
representation is a real gap worth prototyping; an axis with a weak/null representative is an
estimator problem, not an axis problem (don't drop the axis, look for a better estimator); an axis
with 3+ dims is a redundancy question worth checking pairwise, not just against everything at once.

| Axis | Classical or dynamics/microstructure axis? | Current representative(s) | Status |
|---|---|---|---|
| **Location/drift** (regime-dependent mean return, sign and magnitude) | Classical (1st moment) | **None** | **Real gap — arguably larger than tail-weight's was.** Every paper in §1.1-§1.3 defines its states primarily by regime-dependent mean return. Nothing in this vector directly measures recent drift sign/magnitude; it's only inferable indirectly through volatility dims. Prototype before anything else in §5. |
| **Scale/dispersion** (volatility level) | Classical (2nd moment) | `relative_range`, `log_scale_ratio`, `log_scale_expansion_ratio` | **3-way check DONE 2026-08-31** (pairwise, not yet 3-way with `relative_range` — needs High/Low bar data outside this thread's tick-only tooling): `log_scale_ratio`/`log_scale_expansion_ratio` correlate at 0.7638 both-raw, **0.8085 both fixed to bipower variation** — real, substantial, not the "well covered, no action" verdict this row previously implied. Superseded by the whole-vector initiative, `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` — this row's remaining "check the other axes too" framing is now that spec's Phase 1, not a standalone TODO here. |
| **Asymmetry** (skewness) | Classical (3rd moment) | `skewness_idx` | **Contingently redundant** — a skewed-Student-t emission (Hansen 1994; Fernández & Steel 1998) would add a native per-state shape parameter for asymmetry, the same relationship ν_k already has to kurtosis (§4). Not redundant under the current symmetric-t emission; flag, don't act, unless/until that emission change happens. |
| **Tail weight** (kurtosis) | Classical (4th moment) | `fast_taleb_kurtosis` | Same contingent-redundancy structure as above, but against ν_k specifically, already the subject of §4's headline finding. |
| **Persistence** (trend vs. mean-reversion, serial correlation) | Dynamics | `hurst_exponent`/`fast_hurst_exponent`, `mean_rev_z`/`fast_mean_rev_z`, `recurrence_rate` | Axis is legitimate (it's §1.4's trend axis) — every current representative individually measured weak-to-null (§3). **The axis isn't the problem; the estimators are** — don't conclude "persistence doesn't matter here" from this, the null results are estimator-specific (confirmed for `mean_rev_z` via the actual horse-race test, not yet checked for the other two). |
| **Jump/discontinuity share** (fraction of realized variance from jumps vs. continuous diffusion) | Dynamics | None (validated candidate, not yet wired) | **Gap closeable — §5.1's candidate (Barndorff-Nielsen & Shephard bipower-variation ratio) tested and SURVIVES, 2026-08-30** (§9 row 21, §10.8); real, substantial effect. Not yet promoted to an actual field. |
| **Temporal clustering/contagion** (self-exciting extremes) | Dynamics | `burstiness_index` (informal) | Partial — formal version deferred to post-workstation (§5.2, Hawkes intensity). |
| **Liquidity/fragility** | Dynamics/microstructure | `amihud_illiquidity`, `liq_fragility` | Well covered, both strong discriminators (§3) — same pairwise-redundancy question as the volatility trio, not yet done specifically between these two. |
| **Complexity/predictability** (pattern compressibility) | Dynamics/microstructure | `lempel_ziv` | Covered — genuinely distinct from persistence (nonlinear/algorithmic compressibility vs. linear serial correlation), not a duplicate axis despite surface similarity. |
| **Order-flow toxicity/informed trading** | Dynamics/microstructure | `micro_asymmetry` | Real, literature-recognized axis (Easley-O'Hara/PIN-adjacent) — weakest current representative, already dropped from HMM selection. Axis may deserve a better estimator, not necessarily abandonment; not investigated further in this document. |

**What this table changes about §5's priority list, stated plainly**: the drift/location gap is now
the single most glaring hole in the vector — more foundational than the jump-share gap, since every
classical regime-switching paper surveyed in §1 treats regime-dependent mean return as primary, not
incidental. Added to §5 below as the new top prototype candidate.

### 1.10 Lead-time requirement — an evaluation criterion, not just a taxonomy question (2026-08-29, user-stated motivation)

**Why the fat-tail axis specifically is worth continued real investment, stated plainly so it
isn't confused with "forcing an outcome" (§0's guardrail)**: this system has two concrete,
real-money consumers of a fat-tail signal, and they share a requirement neither §1's taxonomy nor
§3's dim-by-dim audit made explicit until now.
- **Offense — "Atratus"** (the user's separate options-trading app): buying cheap out-of-the-money
  options ahead of a tail event captures convexity. The payoff is asymmetric specifically because
  the option is cheap *before* the move is priced in — a confirmation that arrives after the market
  has already repriced the option is worthless to this consumer.
- **Defense — this system's own risk gates**: exiting before catastrophic loss (the TRAP-detection
  framework, `CLAUDE.md`'s "Trap Detection" section) has the identical timing requirement from the
  other side. A fat-tail flag that arrives after the drawdown has already happened is a post-mortem,
  not a risk control.

**The resulting criterion, to be applied to every fat-tail-relevant candidate in §4/§5 going
forward**: does this candidate detect *approach to* a fat-tail state, or only *membership in* one,
once it's already underway? A dim can satisfy "the HMM found a fat-tail state" in an offline
cross-state-ratio audit while being nearly useless to either real consumer, if it only separates
cleanly in the middle of the event rather than ahead of it. This reframes existing findings:
- `ν_k` (the model's native tail-heaviness parameter, §4) is structurally a **steady-state
  descriptor** — by construction it says "you are now in a fat-tail regime," not "one is
  approaching." Necessary for classification, not sufficient for either consumer's timing need.
- `fast_taleb_kurtosis`'s real defensible case (§4) — intra-bar reactivity during high-activity
  windows — is *already* a lead-time argument, now with a concrete business reason it matters
  beyond model-architecture tidiness.
- Self-exciting jump clustering (§5.2, Hawkes intensity) is arguably the **most** lead-time-relevant
  candidate on the list — intensity rises *ahead of and during* a cluster by construction, not just
  in retrospect. Still real estimation cost (MLE fitting), still deferred to the post-workstation
  window on that basis alone — but this is the reason not to let it slide indefinitely once that
  constraint lifts, not a reason to rush it now.

### 1.11 Intra-bar reactivity — a correction, not a new proposal (2026-08-29)

**A claim made earlier in this brainstorm ("TS1/TS2/TS3 dims are frozen for the full bar duration")
was wrong, verified against source and an existing 2026-08-14 audit that already settled this
dim-by-dim.** Most windowed dims in this codebase already implement the pattern this section might
otherwise have proposed as new: a mostly-frozen historical window plus a cheap, direct read of the
current, still-forming bar (`sc.Index`) as the "current point" term — giving genuine tick-level
reactivity without activity-clock migration.

**Confirmed live/tick-reactive today** (reads `sc.Index` directly, verified via source):
`log_scale_ratio`, `hurst_exponent`, `fisher_info` (all TS1); `relative_range`,
`burstiness_index` (both TS2); `mean_rev_z` (TS3, `current_log_p = sc.BaseData[SC_LAST][sc.Index]`).

**Confirmed genuinely bar-gated** (never reads the live bar, confirmed via source comment, per the
2026-08-14 full-institutional-coverage audit): `vol_convexity`, `amihud_illiquidity`,
`liq_fragility` (all TS3). **STATUS, as of 2026-08-31: zero dims remain bar-gated by accident.**
`amihud_illiquidity`/`liq_fragility` were made live-reactive per this section's own decision below,
build-verified 2026-08-29 (winsorization/shrinkage recalibration also done, 2026-08-29/30 — see §6.1/
§9 rows 13/14). `vol_convexity` was removed from the schema entirely, 2026-08-31 (not made
live-reactive — independently decided low-priority regardless of clock, see below), so it's moot
rather than fixed. The only dim still genuinely time-bar-gated today is `fractal_dim`, and that's by
design (§1.9 — a 400-bar structural-persistence window, unrelated to this section's reactivity
question), not an oversight. **Open methodological question, DEFERRED 2026-08-31**: nobody has
re-measured `amihud_illiquidity`/`liq_fragility`'s cross-state discrimination ratio (0.5219/0.2521
below, both measured pre-live-reactivity) against the now-live signal — and doing so against the
*current* `models/hmm_model.pkl` would be circular (that model's states were learned from the
pre-Phase-0 vector). A model-independent lead-time alternative was considered and explicitly
**declined for now**, on priority grounds (would be more feature-vector investigation on a row
already flagged for that exact pattern, without moving its actual blocking gate) — not built. Re-open
only once a retrain exists on the corrected/elite vector. See `docs/superpowers/specs/2026-08-31-
elite-feature-set-curation-initiative.md` §5 for the full decision record.

**CORRECTED, 2026-08-29 — this originally hedged toward caution, citing recalibration cost as a
reason to leave these three bar-gated. That's the same "avoid recalibration effort" rationalization
this session has already caught and retracted twice (kurtosis's design, `fractal_dim`'s split).
Cost of doing it right is a line item to budget, not a reason to keep the patched version — the
three dims don't share one verdict, evaluated properly:**

- **`vol_convexity`** — legitimately low-priority regardless of this question. Independently
  diagnosed weak on unrelated grounds (10-40 bar sample-size noise floor; tail-enrichment rank
  13/16, at/below the no-enrichment baseline). Live-reactivity wouldn't fix a noisy-by-construction
  estimator. Staying bar-gated here isn't patching, it's correctly not worth further investment.
- **`amihud_illiquidity`, `liq_fragility`** — the vector's #2 and #4 discriminators, explicitly
  named in §1.5/§3 as causally tail-adjacent **leading** indicators. Being bar-gated for 15 minutes
  directly undermines the one property that makes them valuable (§1.10's lead-time criterion) — an
  illiquidity spike invisible until bar close is a lagging indicator wearing a leading indicator's
  name. The "deliberate, to avoid the undersampling problem" framing doesn't hold up: the actual
  problem `log_scale_ratio` hit wasn't "live-reactive is bad," it was "the winsorization
  calibration was built from the wrong replica (bar-close-only instead of tick-level)" — already
  fixed once by rebuilding the replica correctly, not by staying frozen. **Decision: make both
  live-reactive.** The one genuine engineering risk (Amihud's `|return|/volume` can misbehave on a
  near-empty denominator in the first instants of a fresh bar) gets solved with the pattern already
  established throughout this codebase — carry-forward-last-valid-value guards
  (`RELATIVE_RANGE_LAST_VALID_VALUE`, `FRACTAL_DIM_LAST_VALID_VALUE`, etc.): gate the live read on a
  minimum volume-so-far threshold, fall back below it. Recalibrating both dims' winsorization bounds
  against a genuine tick-level replica is then **required work, not optional** — budgeted the same
  way `log_scale_ratio`'s rebuild was, not treated as a reason to skip.

**Consequence for `hurst_exponent` specifically, walking back a hypothesis from earlier this
session**: it already reads the live bar, so its worst-in-vector discrimination score (0.0000) is
probably not cadence-blindness after all. More likely: one live point diluted across a 50-200-bar
DFA regression carries very little practical weight — technically reactive, practically still
close to inert. That's a real, different diagnosis (weak estimator/axis fit) than "wrong clock,"
and it argues for `fast_hurst_exponent`'s activity-clock version on its own separate merits, not as
"finally fixing the cadence problem" — there wasn't one here, specifically.

**Consequence for the hardening spec's own flagged, unresolved tension** (whether `mean_rev_z`/
`recurrence_rate` reading the live bar violated a stated "historical-bars-only" contract): given how
common this pattern turns out to be (6 of 9 dims checked), it reads as consistent with established
precedent rather than an outlier bug — worth a final confirmation against the literal contract
text, no longer looking like an accident.

---

## 2. Historical motivation (context only — NOT a gating criterion for what follows, corrected 2026-08-29)

**Reframed 2026-08-29 on user correction**: this section was originally titled "the actual problem"
and treated as the thing this whole document exists to resolve. That's backward-looking in exactly
the way §0 already warns against, and it wasn't applied consistently — the diagnosis below was
measured against a vector already known (§1.11, §4) to have been missing kurtosis entirely,
partially bar-gated in ways that understate reactivity, and about to change substantially (drift
axis, jump-ratio, Amihud/liq_fragility live-reactivity). Treating "did this fix that specific old
model's failure" as the organizing question means gating new engineering on a stale measurement.
**This section is now history/motivation only — why this initiative started — not a live question
this document needs to track toward resolving, and not a filter any candidate dim needs to pass.**
The actual job stays what §0 says: build the richest, non-redundant, axis-spanning vector per §1's
institutional literature, and let whatever a future retrain reveals be revealed, on its own terms,
against the *new* vector — not measured for whether it explains the *old* one's specific numbers.

The Student-t HMM at production K=4 historically showed **zero states resembling a fat-tail
regime**. The DOF-based sign-off gate (`lbrnet/models/regime_gate_metrics.py`) measures this
directly: a Student-t's excess kurtosis is an exact function of its degrees of freedom (κ = 6/(ν−4)
for ν>4), so ν is the model's own native fat-tail parameter. On that historical model: **ν by state
= [20.7, 49.7, 33.9, 33.3]** — all four states far from the ν<4 threshold (full history:
`PRODUCTION_TRIAGE.md` row 1). Whether a feature problem, a K=4 capacity problem, or some
combination explains that specific historical number is **not this document's question to answer**
— it will be answered, if at all, by retraining on the vector this document converges toward, and
the answer to that (whatever it is) is new information to act on then, not a target to engineer
toward now.

**One thing worth keeping from the old framing, restated as information rather than a gate**: none
of this session's activity-clock feature work (`fast_taleb_kurtosis`, `skewness_idx` replacement,
`fractal_dim`, `recurrence_rate`) has ever been retrained or checked against the DOF gate — so
nothing in §3-§6 should be read as "known to fix" anything. That's simply true and worth stating
once; it doesn't need its own hypothesis-tracking apparatus.

---

## 3. Current vector, dim-by-dim, assessed for fat-tail relevance specifically

**Methodology note**: cross-state discrimination ratios below are real, measured
(`lbrnet/docs/superpowers/specs/2026-08-25-vol-convexity-removal-spec.md` §2, computed on the
old production K=4 model, between-state variance / mean within-state variance per dim).
**Discrimination ≠ tail relevance** — a dim can separate states well while only tracking
volatility level, not tail shape. Both columns are kept separate on purpose. **Clock column added
2026-08-29 per §1.11** — Live = reads the current still-forming bar directly (tick-reactive even
though nominally time-bar); Bar-gated = confirmed via source, never reads the live bar;
Event-native/Activity-clock = never bar-gated at all, by construction.

**Backward-looking caveat, 2026-08-29 (per §2's reframe)**: every ratio below was measured on a
vector that had no kurtosis dimension at all and several dims later found to be partially
bar-gated. Read as **directional signal about the old vector**, not a verdict on any dim's value in
the vector this document is converging toward — a new ranking only exists once the new vector is
actually fit. Don't let "#1 discriminator" status here, or lack of one, be the deciding factor for
what ships.

| Dim | What it measures | Clock | Cross-state ratio | Fat-tail relevance |
|---|---|---|---|---|
| `tail_index` (Hill α) | Direct power-law tail estimator | Event-native | 0.0052 (weak) | Direct tail statistic, but **structurally redundant with ν_k** — a Student-t's tail-heaviness is already an exact function of its own fitted ν. Correctly dropped from HMM selection; stays live for `PositionManager.cpp` position sizing (non-HMM use). |
| `skewness_idx` | Tail asymmetry (Bowley quartile) | Activity-clock (since `7c51f33`) | 0.0005 (weak, old TS3-cadence measurement) | Real construct, weak on the stale calendar-time cadence it was measured on. Replaced 2026-08-27 with an activity-clock computation — **never re-measured for cross-state ratio since**. |
| `fast_taleb_kurtosis` | Tail weight (Moors octile kurtosis), activity-clock | Activity-clock | Not yet measured — never selected into `HMM_KEEP_DIMS` | **See §4 — the headline finding.** The vector's first-ever kurtosis dimension of any kind. Real, non-redundant case for it: intra-bar reactivity (transition-speed signal) that a static ν_k cannot represent — NOT "yet another tail-heaviness estimate," which would likely hit the same wall as `tail_index`. |
| `relative_range` | (High−Low)/ATR | **Live** (TS2) | 0.6373 (**#1**) | Volatility-*level* signal. Correlates with tail events (wide-range bars during crashes) but doesn't isolate them from ordinary vol expansion. |
| `amihud_illiquidity` | \|return\|/volume | **Bar-gated today → DECISION: make live-reactive (§1.11)**, guarded on min volume-so-far to avoid a near-empty-denominator artifact | 0.5219 (#2) | Causally tail-adjacent — illiquidity spirals are a documented crash precursor (§1.5). A **leading** indicator only once made live-reactive — bar-gated for 15 min, it's lagging in practice. |
| `burstiness_index` | Inter-arrival-time variance (activity clustering) | **Live** (TS2) | 0.3999 (#3) | Same adjacency, different mechanism — activity clustering is the empirical stylized fact self-exciting jump models exist to explain (§1.6/§5.2). Already informally capturing part of that idea. |
| `liq_fragility` | Liquidity fragility (ATR × volume composite) | **Bar-gated today → DECISION: make live-reactive (§1.11)**, same category as Amihud | 0.2521 (#4) | Same liquidity-spiral logic as Amihud (§1.5) — same lagging-vs-leading correction applies. |
| `log_scale_expansion_ratio` | log(BV_recent/BV_full) (Barndorff-Nielsen & Shephard bipower variation, **REFORMULATED 2026-08-31** — same jump-fragility fix as `log_scale_ratio`, see below) | TS2, not yet checked live-vs-bar-gated | 0.1823 (pre-fix; not yet re-measured) | Volatility level, not shape. Correlates with `log_scale_ratio` at 0.8085 once both are fixed — see the Scale/dispersion row above and `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md`. |
| `lempel_ziv` | Price-path algorithmic complexity | Event-native | 0.0356 | Weak tail relevance — predictability, not extremity. |
| `log_scale_ratio` | Macro volatility ratio, `log(short_BV/long_BV)` (Barndorff-Nielsen & Shephard bipower variation, **REFORMULATED 2026-08-31** — see below) | **Live** (TS1) | 0.0221 (pre-fix; not yet re-measured against the new formula) | Volatility level, weak discriminator. |
| `vol_convexity` | Vol-of-vol | **REMOVED FROM SCHEMA 2026-08-31** (was Bar-gated, TS3 — moot now, C++ computation itself deleted, not just the HMM selection) | 0.0032 (dropped) | Real construct, wrong sample size for the estimator (10-40 bars) — noise floor dominates regardless of regime. |
| `mean_rev_z` | OU-style elasticity | **Live** (TS3) | 0.0030 | Not a tail-shape statistic. **Empirically null** on its own terms too (`tools/mean_rev_z_variant_comparison.py`, both time-bar and activity-clock variants indistinguishable from a coin flip on forward-return sign, well-powered test). Activity-clock twin work **paused** pending the HMM-discrimination test that was never run. |
| `recurrence_rate` | RQA topological stability | Activity-clock (since `7f395d0`) | 0.0030 | Not a tail-shape statistic. Moved to activity-clock in place on literature grounds unrelated to fat-tail (RQA-on-event-indexed-sequences precedent), not yet re-measured for cross-state ratio. |
| `fractal_dim` | Sevcik path roughness | TS2, time-bar by design (§1.9) | 0.0021 | Not a tail-shape statistic. Window widened to 400 bars (`72ab967`) for a structural-persistence reason, not a fat-tail reason. |
| `fisher_info` | Ehlers Fisher Transform (turning-point sharpness) | **Live** (TS1) | 0.0011 | Plausibly extreme-point-adjacent (detects price near a recent extreme) but essentially untested for this specific purpose. |
| `micro_asymmetry` | Order-flow buy/sell asymmetry | TS3, not yet checked live-vs-bar-gated | 0.0001 (dropped) | Weakest in the vector. |
| `hurst_exponent` | Persistence/long-memory | **Live** (TS1) — see §1.11, weakness is likely a diluted-weight estimator issue, not cadence-blindness | 0.0000 (**worst**) | Not a tail-shape statistic at all. Activity-clock twin (`fast_hurst_exponent`) shipped (`4969259`) for a persistence/clock-choice reason, unrelated to fat-tail — but see §1.4: crossed with volatility level, this is the trend axis of the "Trending-High-Vol = crisis" reframing. |
| `fast_mean_rev_z` | Activity-clock elasticity twin | Activity-clock, wiring paused | N/A — wiring paused | Paused pending the null-result implications above; do not resume until the horse-race/HMM-discrimination question is actually closed. |

---

## 4. Headline finding: kurtosis was never in the original 16D vector

Count §3's rows against the original vol-convexity-removal spec's 16-dim table: `skewness_idx`
(asymmetry) was there; an actual tail-*weight* statistic was not, at all, until `fast_taleb_
kurtosis` shipped 2026-08-27. The single most direct, obvious fat-tail feature was structurally
absent from the model's inputs from the beginning — that alone is most of the answer to "why
couldn't the HMM find a fat-tail state historically."

**The honest risk that must not be glossed over**: the DOF-redundancy mechanism that killed
`tail_index` is not specific to Hill's estimator. It applies to *any* statistic whose entire job is
"how tail-heavy is the return distribution," because a well-fit Student-t's ν already encodes that
natively. Sample kurtosis, Moors octile kurtosis, Hill's α are, for a genuinely Student-t process,
all bijective functions of the same underlying parameter. `fast_taleb_kurtosis` is **not
automatically exempt** from this just because it's "a fat-tail statistic" — that reasoning is
exactly the assumption-over-evidence pattern this whole thread has already been corrected on twice.

**What genuinely does distinguish it, and gives it a real (untested, not assumed) case**: it's
measured on the activity clock, updating intra-bar during exactly the high-activity windows a
regime transition would unfold in. ν_k is a static, per-state shape parameter — it cannot represent
the *speed* at which evidence for a fat-tail episode accumulates. The defensible claim is
"faster-updating transition detector," not "a better way to measure the same steady-state tail
weight." **This is the actual test that needs to run once `lbrnet` selects it**: does
`fast_taleb_kurtosis` help the model detect the *onset* of a fat-tail episode, not just describe an
already-identified state's steady-state shape (which ν_k already does).

---

## 5. Literature-sourced candidates NOT yet in the vector

Given §4's redundancy risk, a useful new candidate must capture something ν_k structurally
**cannot** — not re-estimate "how fat is the tail" a third way. §1's state taxonomy adds two more
candidate targets beyond pure tail-shape: the **Recovery** state (§1.2) and the **Trending-High-Vol
crossing** (§1.4) — neither needs a new feature, just a new way of reading existing ones (see §5.4).
§1.9's axis decomposition adds one more, and it now outranks the others in priority.

### 5.0 Drift/location — TESTED AND REJECTED, 2026-08-30 (was: the largest gap per §1.9)

**Verdict: does not support the momentum/continuation hypothesis it was built on — rejected before
any schema or C++ commitment.** Full methodology and result in §9 row 20; summary here for anyone
reading this section in isolation: `tools/drift_location_eval.cpp` (a new, model-independent C++/Arrow
tool — see §10.7 below) computed the return z-score over real 38.5M-row MES bar data and tested
whether its sign predicts the same-direction forward return. Hit rate came back **below** 0.5 at
every horizon tested (30/60/120/240 min: 0.4854 → 0.4895 → 0.4925 → 0.4957), decaying toward null as
the horizon lengthens — the opposite shape a genuine momentum candidate should show. Statistically
significant only because n is in the tens of millions (p≈0, survives Bonferroni); the effect size
itself is economically tiny (1-1.5 percentage points). This is exactly the outcome the "prototype
offline before committing" discipline (§6.0) exists to catch cheaply — no wire field, no `lbrnet`
client update, no pilot collection ever had to be paid for.

Nothing in the vector directly measures regime-dependent mean return (§1.9) — that axis gap is
**still real and still open**; this result only rules out *this specific formulation* of it, not the
axis itself. A future attempt would need a genuinely different construct (a different window,
volatility estimator, or return definition), not a resubmission of the same z-score with different
tuning — that has real prior-art precedent in `hurst_exponent`'s own "diluted weight" IN-WEAK status
(§9 row 7), where a plausible axis persisted through a bad instantiation, but is a separate, later
question, not an automatic retry queued here.

**Reusable-computation check DONE, 2026-08-29 — none exists, confirmed via source, not assumed.**
ADX (the obvious trend-strength/slope-family candidate) was **formally retired March 2026**
(`ContextManager.cpp:659`, `TripleScreen1.cpp`/`TripleScreen3.cpp` — subgraph slots preserved as
`DRAWSTYLE_IGNORE` to avoid index shifts, computation removed, comments explicitly say "Hurst
exponent provides superior trend persistence measurement"). Two things worth noting: (1) there is
nothing left to reuse — this must be built new; (2) even un-retired, ADX measures trend *strength*
(magnitude of directional movement, unsigned), not the signed return level/direction this axis
actually needs — it would have been the wrong tool regardless of retirement status.

**Correction, 2026-08-30**: this section previously claimed the candidate's log-return series was
already available from "`TailRiskEngine`/`ActivityClockManager`" — checked directly against source
before building the prototype and found half wrong: `TailRiskEngine`'s buffer stores `std::abs()` of
each log return (sign destroyed by design, tail-risk magnitude only) and has no public accessor at
all. The real, usable, signed series is `ActivityClockManager::Instance().Engine().
GetImbalanceBarReturns()` (`ImbalanceBarEngine.h`), already the call site `ContextManager.cpp:577-637`
uses for `fast_taleb_kurtosis`/`fast_hurst_exponent`. Moot for this specific candidate now that it's
rejected, but real for whatever future candidate reaches for "the log-return series" next — don't
repeat this claim unchecked.

**Also, cross-state-ratio methodology was explicitly NOT used for this test**, despite an earlier
draft of this section saying it would be — `hmm_model.pkl` was confirmed trained on data with
multiple known invalidities (bar-gated `amihud_illiquidity`/`liq_fragility`, a `FeatureScaler`
dedupe-corruption bug, a `fast_hurst_exponent`-insertion index-shift miscalibration) mid-session,
making its state decode untrustworthy as ground truth right now. The test used instead — same-sign
hit-rate against **real forward market returns** — is model-independent by construction and is now
this document's standing methodology for any candidate that doesn't strictly require HMM state
labels (see §10.7).

### 5.1 Jump / bipower-variation ratio — TESTED AND SURVIVES, 2026-08-30 (was: the more directly load-bearing candidate)

**Verdict: real, substantial predictive power for future |return| magnitude — survives at every
horizon tested, with the opposite sign from the naive hypothesis.** Full methodology and result in
§9 row 21 and §10.8; summary here for anyone reading this section in isolation.
`tools/jump_ratio_eval.cpp` (a new, model-independent C++/Arrow tool, §10.8) computed
`jump_ratio = max(0, (RV-BV)/RV)` over real 38.5M-row MES tick data and tested whether the top
decile of jump_ratio values predicts a different `median(|forward return|)` than the bottom decile
(a magnitude test, not the directional hit-rate test §5.0 used — `jump_ratio` is non-negative by
construction, so there's no sign to test against a forward return's sign). Result, at all 4
horizons (30/60/120/240 min), CI excludes zero (survives) and the **top decile's median is smaller**
than the bottom decile's (55.2%/55.1%/58.3%/62.6% of it) — **the opposite of the naive
"more jump content now → more chaos ahead" hypothesis.** A high-jump-share window (variance
concentrated in a few large moves) predicts a *calmer*, not more volatile, near-term future;
low-jump-share windows (steady, grinding diffusion) predict the larger subsequent moves. This reads
more like a vol-compression/mean-reversion-in-volatility signal than the tail-risk-warning signal
the candidate was originally framed as testing for.

**Barndorff-Nielsen & Shephard (2004, 2006)**: realized variance (RV) includes both continuous
diffusion and discrete jumps; bipower variation (BV) is a jump-*robust* estimator of the continuous
component alone. `(RV − BV)/RV` directly quantifies **what fraction of realized variance came from
discontinuous jumps versus ordinary diffusion** — a fundamentally different, dynamic/time-varying
question from "how heavy is the marginal tail." A high-jump-share regime is mechanistically
different from a regime that's just generically volatile, and nothing currently in the vector
isolates that (`relative_range`/`log_scale_expansion_ratio`/`log_scale_ratio` all measure volatility
*level*, none isolate the jump *component*).

**Methodology correction, 2026-08-30, found by direct user challenge**: the first working version of
this test used `mean(|forward_return|)` as the magnitude statistic, ported verbatim from
`tools/dim_acceptance_eval.py`'s own precedent — the same class of mistake this codebase already
corrected once before (moment-based skewness/kurtosis → Bowley/Moors robust quantile estimators,
2026-08-13, per Kim & White 2004). This codebase's own established convention for fat-tailed data is
`FeatureScaler.h`'s `RobustLocation()`: median and MAD × 1.4826 ("Taleb-consistent"), not the mean.
Corrected to `median(|forward_return|)` — new, native-only bootstrap infrastructure
(`ComputeBootstrapMedianGapCI`, §10.8), independently cross-validated against exact bootstraps under
Normal/Student-t/Cauchy tails during review. The result above is the corrected, median-based one; it
confirms (same sign, similar decisive significance) the direction found by the earlier, since-
superseded mean-based run.

**Known limitation, not yet fixed (found during review, documented not patched ad hoc — see §10.8/
§10.9)**: the bootstrap resamples individual per-tick signals as if independent, but real forward-
return signals overlap heavily (a 240-minute forward return spans thousands of adjacent per-tick
observations) — textbook i.i.d. bootstrap understates true CI width under this much overlap, by an
unquantified but likely large factor. Doesn't change this candidate's verdict (the effect is hundreds
of standard errors from a naively-calibrated zero, so even a much wider correctly-calibrated CI would
still exclude it) — but this is a standing §10.7/§10.8 methodology gap shared by §5.0's own already-
accepted OUT verdict too, not unique to this candidate, and should be fixed once, applied uniformly,
before it's trusted for a more marginal future candidate.

**Why this was a cheap first prototype**: both RV and BV come from the same log-return series
`TailRiskEngine`/`ActivityClockManager` already ingest — no new data source, no new ACSIL
feasibility question. Prototyped and validated entirely offline in native C++/Arrow against real
MES data using §10.7's model-independent methodology (never cross-state-ratio against `hmm_model.pkl`
— its training-data invalidities remain unresolved, §5.0's own correction), before any schema/C++
production commitment.

**Not yet promoted.** This result makes jump_ratio a strong candidate for actual schema addition
(§6.2's governance: needs a new `ObservationData` wire field, same as row 20 would have) and HMM
retraining — but that promotion decision, the schema-change sequence, and the block-bootstrap
methodology fix above are separate, not-yet-started pieces of work, not implied by this section.

### 5.2 Self-exciting jump clustering (Hawkes intensity) — the more powerful, more expensive candidate

**Aït-Sahalia, Cacho-Diaz & Laeven (2015)** (§1.6): jump arrivals modeled as self-exciting — a jump
raises the *intensity* of further jumps, producing the empirically observed clustering of extreme
moves, with intensity mean-reverting until the next one. Genuinely different informational axis:
**contagion dynamics over time**, something no per-state emission parameter can represent at all.

`burstiness_index` (already the vector's #3 discriminator, 0.3999) is *informally* gesturing at
this — inter-arrival clustering — without conditioning specifically on jump events. A real
Hawkes-intensity estimate would be a principled sharpening of an already-proven-useful feature, not
a speculative add. Real estimation cost, though: requires MLE fitting of excitation/decay
parameters, not a closed-form statistic. **Recommendation: defer to the post-workstation-upgrade
window (~2 weeks out)** rather than force it into today's pre-collection pass.

### 5.3 Realized semi-variance decomposition — secondary, same family as 5.1

**Barndorff-Nielsen, Kinnebrock & Shephard (2010)**: splits realized variance into upside/downside
components — a dynamic, time-varying directional-tail-asymmetry measure, versus `skewness_idx`'s
single rolling-window snapshot. Lower priority than 5.1/5.2; logged so it isn't lost.

### 5.4 Not a new feature — a new *reading* of existing features (§1.2, §1.4)

Two states from §1's taxonomy don't require new C++/data work, only a reframing of features already
in the vector, worth testing once retraining is possible:
- **Recovery** (§1.2): distinguishing "post-crash decaying volatility" from "steady-state Bull"
  needs *time-since-last-extreme-event* information — not present as an explicit dim today, but
  approximable from existing pieces (e.g., a decay-since-last-kurtosis-spike construct). Flagged as
  a real gap, not yet designed.
- **Trending-High-Vol crossing** (§1.4/§1.7): an explicit interaction/cross term between
  `hurst_exponent` (or `fast_hurst_exponent`) and a volatility-level dim (`relative_range`) may
  surface the "crisis" cell the quadrant framework predicts, without needing any new raw
  measurement — this is a feature-engineering (cross-term) question, not a data-collection one.

### 5.5 Two already-diagnosed "sound signal, wired to the wrong place" fixes, pulled in from `PRODUCTION_TRIAGE.md` row 1 (2026-08-26 investigation, not yet acted on)

Found while cross-checking this document against the triage record (2026-08-29) — same shape as
`fast_taleb_kurtosis`'s own origin story (a real, already-computed signal that never reached the
HMM's observation vector), not new prototyping work:

- **`raschkeBurst`** (`ContextManager::CalculateBurstinessIndex()`) is a genuine event-arrival-
  timestamp CV-burstiness measure, already computed on the HMM-trigger cadence — but it never
  reaches `burstiness_index`'s wire field, which instead uses a cruder bar-cadence proxy. This is a
  real upgrade candidate for a dim already ranked the vector's #3 discriminator (§9, row 2) — wiring
  the existing, better signal in is cheap (no new computation, just redirecting which value feeds
  the observation vector), unlike most of this document's other candidates.
- **`CalculateMarketSpeed()`'s True-Range tempo proxy** (feeds the already-live `AdaptiveWindowParams`
  adaptive-windowing mechanism) could plausibly be replaced or augmented by `ActivityClockManager`'s
  bar-formation rate — the rate at which imbalance bars complete is itself a direct, real-time
  activity measure, potentially better than a True-Range-based tempo proxy. Less concrete than the
  `raschkeBurst` fix above (needs its own design pass, not just a redirect); logged so it isn't lost.

---

## 6. Converging recommendation (UPDATE THIS SECTION AS THE BRAINSTORM CONTINUES)

**Not final. This is the running answer, to be edited in place as more evidence lands.**

### 6.0 Two-phase validation plan (2026-08-29, user-proposed, agreed)

**The 3-day full data collection is not one all-or-nothing gate — split it into two phases,
matching the "verify before concluding" discipline already applied to every dim decision in this
document, now applied to the collection step itself.**

1. **Phase 1 (now, in progress)**: make meaningful C++-side progress on the vector — implement and
   wire the candidates queued in §9 (drift/location, jump-ratio, the Hurst×volatility cross-term,
   the `burstiness_index`→`raschkeBurst` redirect already landed) — iterating purely on the C++ side,
   without paying the full collection cost yet.
2. **Phase 2 (small pilot run, before the full collection)**: launch a short, cheap data-collection
   run — sized only to validate the *new instrumentation itself* (does the build actually work
   end-to-end, do new dims produce non-degenerate/plausible-range/non-NaN values, does the
   `raschkeBurst` redirect look sane on real ticks) — explicitly **not** sized or intended to be
   retrain-ready. Validated with the same `tools/dim_acceptance_eval.py` harness (§6.1 below), run
   against the pilot's fresh output instead of only the old historical export.
3. **Only after Phase 2 looks clean**: commit to the full 3-day collection and hand off to the
   `lbrnet`/Python side for the actual retrain — per this project's repo-scope convention, that
   handoff point is also where session ownership changes.

**Why this matters, not just as a nicety**: every real problem found so far this session (the
schema-contract drift currently blocking the build, `fractal_dim`'s naive-percentile-mapping trap,
`burstiness_index`'s recalibration need) is exactly the class of bug a full 3-day collection would
only surface *after* paying for it. A pilot run catches the same class of bug for a fraction of the
cost — this phase split is that principle applied to the collection step, not a new one.

### 6.1 `tools/dim_acceptance_eval.py` — the pre-retrain acceptance eval (2026-08-29, built this session)

A reusable, parameterized script (not another one-off) — redundancy check (Pearson + upper-tail
dependence λ_U against the original 16 dims, with known bar-gated comparison columns flagged
automatically rather than silently trusted) and predictive-power check (`directional` hit-rate for
reversion/continuation hypotheses, `magnitude` bootstrap-CI gap test for volatility/tail-risk
hypotheses — methodologically distinct, not interchangeable). Parquet throughout, no CSV
round-trips. Built and reviewed; **deliberately not run for real conclusions yet** — a first attempt
against `raschkeBurst` using the old `event_data.context.parquet` export was aborted mid-run
(2026-08-29): that redirect decision didn't depend on this eval in the first place (already
justified on its own merits — genuine event-arrival signal, already computed, tick-native, versus
a cruder bar-cadence proxy), and running an expensive analysis against data already flagged as
partially bar-gated, for a question §6.0's own plan defers to the Phase 2 pilot run, wasn't
progress. **Real use of this tool starts once Phase 2's fresh pilot data exists**, not before.

### 6.2 Schema-change governance for promoted candidates (2026-08-29, provision added on user request)

**Why this needs its own explicit provision, not just an implicit assumption**: this exact working
tree just carried a live example, and the real root cause (found by the sibling, `SCRATCHPAD.md`,
after this section's first draft) is more serious than a simple ordering gap between editing
`mts_schema.fbs` and running the regen script. **`regenerate_schema.sh` itself embeds a
hand-maintained duplicate copy of `ObservationData`'s field list** (`kObs*` constants,
`MakeObservationData`/`ToObservationArray`) inside its own heredoc, instead of deriving it from
`mts_schema.fbs` at generation time — so the script can report success while silently regenerating
from a stale hardcoded template that has already drifted from the real schema. This is confirmed to
have happened **twice**: once undetected for weeks (`fast_taleb_kurtosis`), and again just now
(`fast_hurst_exponent`/`fast_mean_rev_z`, this time caught only because the build failed outright).
**"Run the regen script" is not sufficient verification on its own** until this structural defect is
fixed — a spec for the real fix (derive the contract template's field list from the `.fbs` directly,
e.g. via `flatc`'s own reflection output, instead of hand-maintaining a second copy) has been
requested from the sibling (`SCRATCHPAD.md`), scoped `MindfulTrader`-side since it blocks this repo's
build specifically. **This fix is now folded into §10's C++/Arrow converter design (2026-08-30)** —
the converter needs the identical reflection mechanism for `RiskGateContext`'s field list, so both
land as one coherent piece of work rather than two separate ones.

**The required sequence, every time a promoted candidate needs a new wire field** (never hand-edit
`../schema/*_generated.h`, never call `flatc` directly — both already stated in `CLAUDE.md`, restated
here because this is where it actually bites):
1. File a `../schema/PENDING_SCHEMA_CHANGES.md` entry (PROPOSED → DECIDED), same convention as
   PSC-03 (`fast_taleb_kurtosis`) / PSC-04 (`fast_hurst_exponent`/`fast_mean_rev_z`).
2. Edit `../schema/mts_schema.fbs`.
3. Run `../schema/regenerate_schema.sh` (or the workspace-stable wrapper,
   `/home/rcruz/devel/VSCode/scripts/regenerate_schema.sh`, which just execs the former) — **then,
   until the structural fix above lands, manually verify `mts_schema_contract_generated.h`'s own
   `kObs*` constants and field count actually match the new `.fbs` field list** — do not trust the
   script's own success log, it has already been wrong twice.
4. Confirm `./build_dll.sh --no-clean` succeeds before wiring any C++ computation against the new
   field — a stale-contract build failure is a schema-regen problem, not a signal to start
   debugging the new dim's own logic.
5. Mark the `PENDING_SCHEMA_CHANGES.md` entry IMPLEMENTED only once `lbrnet`'s own consumer code
   (`HMM_KEEP_DIMS` or equivalent) actually selects the new dim — a generated binding existing is
   not the same as the field being consumed (the exact gap PSC-03 is still open on, per its own
   entry — don't repeat it a third time unflagged).

**Which current §9 rows this actually applies to, mapped explicitly so it isn't rediscovered per
candidate**:
- **Needs a new schema field once promoted**: row 20 (drift/location), row 21 (jump/bipower-variation
  ratio) — both are genuinely new measurements with no existing wire slot.
- **Does NOT need a schema change**: row 2 (`burstiness_index`→`raschkeBurst`, already-existing field,
  data-source redirect only — precedent already executed cleanly this session, no schema touch
  required); rows 13/14 (Amihud/liq_fragility live-reactivity — same field, different cadence).
- **Open design question, not yet decided**: row 22 (Hurst×volatility cross-term) — if it's consumed
  as a derived/downstream reading rather than fed to the HMM as its own input dimension, it may need
  no new field at all; if the HMM needs it directly, it does. Decide this before, not during,
  implementation — don't let the schema question get resolved as an afterthought mid-wiring, the same
  ordering mistake the `fast_mean_rev_z` example already made once.

**Keep, on real (if not yet fully re-validated) grounds:**
- `fast_taleb_kurtosis` — get it selected into `HMM_KEEP_DIMS` and retrain. Highest-priority single
  action in this entire document; nothing else here matters until this is actually tested.
- `relative_range`, `amihud_illiquidity`, `burstiness_index`, `liq_fragility` — top 4 measured
  discriminators, real (if indirect) tail-adjacency stories for the liquidity/activity ones.

**Real C++ work, not just prototyping (§1.11) — do this properly, not deferred for recalibration cost:**
- ~~Make `amihud_illiquidity` and `liq_fragility` live-reactive~~ **DONE, code-side, 2026-08-29** (§9
  rows 13/14): pulled out of `UpdateObservationVectorSubgraphs`'s once-per-bar gate, now called
  every tick from `TripleScreen3.cpp`, guarded on `kLiveBarMinVolume=50.0` (empirically tuned, see
  below — carry-forward fallback below threshold). `./build_dll.sh --no-clean` succeeds. `vol_
  convexity` stays bar-gated — independently low-priority, unrelated to this decision (§1.11).
  ~~2 required follow-ons~~ **BOTH DONE, build-verified, 2026-08-29** (§9 rows 13/14): (1)
  `kLiveBarMinVolume` empirically tuned via a 5/10/50 single-tick-pass comparison
  (`tools/amihud_liqfragility_recalibration.cpp`) — 50 tames the worst-case outlier 6x (amihud
  max|z| 19583.81→3270.57) for ~7% less mean reactivity, adopted (was an uncalibrated 10.0
  placeholder); (2) both dims' `FeatureScaler` winsorization bounds rebuilt from a genuine
  tick-level replica of guard=50's actual distribution (refit after the guard changed, for internal
  consistency, not left mismatched) — old bounds were clipping 8.5%/12.8% of live readings; final
  GPD-derived bounds **amihud=2706.0, liq_fragility=21.26** (liq_fragility re-derived again
  2026-08-30, see below), updated in both `include/FeatureScaler.h` and
  `config/execution_params.json` (the latter overwrites the former at load time — updating only the
  `.h` would have shipped a fix that silently never took effect).
  ~~Shrinkage re-audit for both dims~~ **DONE and independently verified, 2026-08-30** (§9 rows
  13/14): `amihud_illiquidity` (`SHRINKAGE_SCALE_MIN[12]`) audited clean, correlation(localMAD,|z|)
  = -0.0340 (weak, not a collapse signature) — stays disabled, it already has its own dedicated
  `AMIHUD_ABSOLUTE_FLOOR` mechanism. `liq_fragility` (`SHRINKAGE_SCALE_MIN[13]`) showed a real
  collapse signature (correlation -0.2058) and was enabled at 0.0035 — but the *first* attempt to
  confirm this actually worked came back byte-identical to the pre-fix run, a red flag traced to a
  bug in the verification tool itself: `tools/amihud_liqfragility_recalibration.cpp`'s `liqFragZ`
  manually recomputed the plain (non-shrinkage) z formula, silently bypassing
  `ComputeShrinkageZ()`, so it was never actually exercising the code path under test. Fixed by
  exposing the real shrinkage-blended `zLog` unconditionally via `lastRawZ[13]` in
  `FeatureScaler.h`'s LOGZ branch (mirroring the SOFTLOGZ path's existing precedent) and updating
  the tool to read it instead of recomputing. Rerunning then showed the fix genuinely works:
  max|z| 503.22 → 20.59, correlation -0.2058 → +0.0016 (collapse signature actually gone, not
  masked). This also revealed the 2524.5 winsorization bound above had itself been derived from
  the broken (non-shrinkage) z-distribution and was ~120x oversized; re-derived against the
  corrected distribution (u=p99=8.297, n_tail=7709, ξ=-0.1452, Weibull/bounded — the tail is
  fundamentally tamer once shrinkage is genuinely active, not just a Fréchet fit on inflated data),
  p=1/N return level = 21.2565 → **21.26**. `./build_dll.sh --no-clean` succeeds. Both dims' rows
  in §9 and `config/execution_params.json` updated with the verified figures.
- ~~Redirect `burstiness_index` to `raschkeBurst`~~ **DONE, code-side + build-verified, 2026-08-29**
  (§9 row 2, §5.5) — wiring landed, dead code removed, `./build_dll.sh --no-clean` succeeds; the
  pre-existing schema-contract blocker that was holding up build verification is now resolved
  (§6.2). `FeatureScaler` recalibration still required follow-on, not yet done.

**Prototype before committing (today, pre-collection, cheap/offline), in priority order:**
- ~~§5.0 drift/location (return z-score)~~ **TESTED AND REJECTED, 2026-08-30** (§5.0, §9 row 20) —
  offline model-independent test against real MES data found hit_rate below 0.5 at every horizon,
  decaying toward null as horizon lengthens; rejected before any schema/C++ commitment.
- ~~§5.1 jump/bipower-variation ratio~~ **TESTED AND SURVIVES, 2026-08-30** (§5.1, §9 row 21, §10.8) —
  real, substantial effect, opposite the naive hypothesis (high jump-share predicts a calmer, not
  more chaotic, near-term future). Not yet promoted to a schema field.
- **Next up, before any further candidate work (2026-08-30 decision): the i.i.d.-bootstrap-on-
  overlapping-signals methodology gap (§10.9)**, found during §5.1's review — both §10.7's and
  §10.8's tests understate CI width by treating heavily-overlapping per-tick forward-return signals
  as independent. Doesn't change either existing verdict, but running a third candidate through the
  same under-calibrated apparatus means accumulating results on a known-flawed instrument; this
  codebase already has the raw ingredient (a measured Politis-White block length, ≈404.82, from the
  `fractal_dim` work) a real fix should reuse. Fix once, applied uniformly, before §5.4.
- §5.4's Hurst × volatility-level cross-term — zero new data, pure feature-engineering; next
  candidate in queue once the methodology gap above is resolved.

**Defer to post-workstation window:**
- §5.2 Hawkes self-exciting jump intensity.
- §5.4's "Recovery" time-since-last-extreme-event construct — needs design work first, not just a
  prototype script.

**Paused, do not resume without new evidence:**
- `fast_mean_rev_z` wiring — empirically null so far, HMM-discrimination test never run.
- `recurrence_rate`'s activity-clock move — real literature grounding (RQA-on-event-indexed-
  sequences precedent), but doesn't map cleanly to any §1 axis (trend/vol/liquidity/persistence/
  tail-weight/contagion) — genuinely orthogonal to this document's goal, not just deprioritized.
- **Reclassified, not paused**: `fast_hurst_exponent` is no longer "unrelated" under the broadened
  goal — it's the trend-axis half of §5.4's Hurst × volatility-level cross-term hypothesis. Shipped
  already (`4969259`); its cross-state ratio (alone, and crossed with `relative_range`) should be
  re-measured alongside §5.1's jump-ratio prototype, not left out of today's evidence-gathering.

**Future information, not current blockers (reframed 2026-08-29 — neither of these gates any
engineering decision in this document; they're questions a future retrain answers, not questions
this brainstorm needs to resolve first):**
- Whether K=4 has room for a fat-tail state at all — an answer the retrain provides once it
  happens, on the new vector; not a fork this document needs to plan around today (§2).
- Whether "fat-tail state" and "Trending-High-Vol crisis state" (§1.4/§1.7) are the same latent
  phenomenon or genuinely separate — same status, worth testing once a retrain is possible, not
  worth resolving in advance.

---

## 7. Cross-references

- `PRODUCTION_TRIAGE.md` row 1 — the canonical status/history record this document narrows into a
  working list.
- `lbrnet/docs/superpowers/specs/2026-08-25-vol-convexity-removal-spec.md` — source of the real
  cross-state ratio table in §3.
- `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md` — per-dim literature
  grounding/validation status, Pillars 3/4. **Sync convention, 2026-08-29**: this brainstorm doc is
  a cross-cutting orchestration layer, not a replacement for the Gang doc's per-parameter role —
  findings here that belong in that doc's scope (literature-grounding facts, "sound signal wired
  wrong" findings) get mirrored back there as they're found (its own 2026-08-29 changelog entry,
  new Finding 10, `lempel_ziv` row addendum), not left to drift. Same "pointer, not rewrite"
  pattern this project already used for the 2026-08-13/14 specs → Gang doc.
- `docs/superpowers/specs/2026-08-25-observation-vector-institutional-hardening-spec.md`,
  `docs/superpowers/specs/2026-08-26-activity-clock-tail-risk-and-decay-spec.md` — the
  `fractal_dim`/`recurrence_rate`/`mean_rev_z`/`hurst_exponent` activity-clock threads this
  document is deliberately de-prioritizing relative to §4/§5.
- `SCRATCHPAD.md` — `mean_rev_z` null-result detail, sibling coordination log.
- `lbrnet/lbrnet/scripts/materialize_context_parquet.py`,
  `lbrnet/lbrnet/data/observation_vector_bulk_reader.py` — the Python `.context` converter/reader
  §10's C++/Arrow tool replaces outright; read in full to ground that design.

---

## 8. Ground-truth status — NOT YET, per user question 2026-08-29

**This document is not yet ground truth for the observation vector's evolution.** What's missing,
in priority order:

1. **RESOLVED, 2026-08-29** — ~~No freshness mechanism.~~ A document-level stamp now exists in the
   header (verified against HEAD `4969259`). Re-check/update it each session, don't assume it's
   still current indefinitely.
2. **Still open. UPDATED, 2026-08-30 — row 20 (drift/location) reached `OUT` this session** (tested
   offline against real MES data, rejected — §5.0, §9 row 20), joining rows 13/14
   (`amihud_illiquidity`/`liq_fragility`, reached `IN` earlier the same day). Recounted row-by-row
   against §0's literal status tokens (`IN`/`OUT`/`OUT-HMM` = terminal, everything else not): still
   **15 of 25 non-terminal (10 terminal: `relative_range`, `vol_convexity`, `lempel_ziv`,
   `micro_asymmetry`, `tail_index`, `recurrence_rate`, `fractal_dim`, `amihud_illiquidity`,
   `liq_fragility`, drift/location).** Row 21 (jump/bipower-variation ratio) also gained real evidence
   this session (tested, SURVIVES — §5.1, §9 row 21) but is not counted as newly terminal: it moved
   `CANDIDATE` → `CANDIDATE-VALIDATED`, still non-terminal per §0 until actually promoted to a schema
   field. Real prototyping, wiring, and measurement work closes the remaining 15, not more
   documentation.
3. **RESOLVED, 2026-08-29** — ~~No single current field manifest.~~ §9 is exactly this, kept current
   alongside the decision ledger rather than as a separate table.
4. **Still open, narrowed further.** §5.0 is resolved (rejected, §9 row 20); §5.1 is now resolved
   (survives, §9 row 21) but not yet promoted; §5.4 hasn't been prototyped yet. Still no decided "the
   vector will be exactly these N fields" statement — same underlying gap as item 2, different
   framing.
5. **RESOLVED, 2026-08-29** — ~~Decisions scattered, not centralized.~~ §9's ledger is the single
   scannable per-dim status table now.

**Objective promotion criterion, per §0**: every §9 row reaches a terminal status (`IN`, `OUT`, or
`OUT-HMM`) or is explicitly triaged out with a stated reason. Items 2 and 4 above are the same gap
restated — closing them is executing the work, not writing more about it.

---

## 9. Per-dim decision ledger (current field manifest + status)

**MOVED 2026-08-31: this ledger's canonical, kept-current copy now lives in
`docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` §7** — that initiative
now owns whole-vector redundancy/relevance decisions (its own §0). The table below is left in place
for history (it predates the Phase 0 fixes — `vol_convexity` removal, `burstiness_index`/
`mean_rev_z` reformulation — recorded only in the §7 copy) and is **not** updated going forward; read
§7 for current status.

**Fills §8 item 5.** One row per dim — every field currently in `mts_schema.fbs`'s `ObservationData`
(19, verified fresh 2026-08-29; **now 18, 2026-08-31 — `vol_convexity` removed from the schema
entirely, see its row below**) plus every candidate proposed in §5 not yet in the schema at all.
Status vocabulary: **IN** (settled, stays as-is) · **IN-WEAK** (stays, weak, no better alternative
identified) · **IN-CONTINGENT** (stays now, flagged future redundancy risk) · **IN-PENDING-FIX**
(stays, a decided implementation change not yet done) · **IN-UNMEASURED** (shipped, never tested
against anything) · **OUT-HMM** (dropped from HMM model-input selection only; C++ computation and
non-HMM consumers unaffected) · **PAUSED** (groundwork exists, explicitly do not proceed without
new evidence) · **CANDIDATE** (proposed, not yet in the vector) · **CANDIDATE-VALIDATED** (offline
model-independent test survives — real evidence for promotion — but not yet an actual schema field;
non-terminal per §0's promotion criterion until it is) · **CANDIDATE-DEFERRED** (proposed,
explicitly pushed to a later window).

| # | Dim | Status | Clock | Why (one line) | Last verified |
|---|---|---|---|---|---|
| 1 | `log_scale_ratio` | IN-WEAK | Live (TS1) | **REFORMULATED 2026-08-31**: raw `log(short_var/long_var)` deleted outright, replaced with `log(short_BV/long_BV)` (Barndorff-Nielsen & Shephard bipower variation, `include/BipowerVariation.h`) — raw sample variance on windows as small as 8 bars let a single-tick jump dominate quadratically (Mandelbrot 1963), producing false volatility-regime signals; see `lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_118`/`118_REPLY` for the full literature-grounding correspondence (also rules out a fixed-ν Student-t M-estimator alternative on real measured hot-path cost, ~200ns-5.6µs vs. this system's budget). `FeatureScaler.h`'s dim0 winsor/shrinkage calibration (fit to the old raw-variance distribution) is disabled pending re-audit against the new formula — real follow-up debt, not fabricated numbers. Discrimination score (0.0221) predates this change, not yet re-measured. | 2026-08-25 (ratio), 2026-08-29 (clock), 2026-08-31 (formula + rename) |
| 2 | `burstiness_index` | IN-PENDING-FIX | Event-driven (redirected to `raschkeBurst`) | Redirect **landed and BUILD-VERIFIED 2026-08-29** (`TripleScreen2.cpp`, `ContextManager::GetRaschkeBurst()`, old proxy deleted, `./build_dll.sh --no-clean` succeeds — the schema-contract regen blocker was resolved elsewhere in this working tree). Required follow-on, **queued next, not yet started**: `FeatureScaler.h` dim1's existing bound (`45.0f`, GPD+bootstrap-derived) was calibrated against the old cruder proxy's distribution and now feeds a completely different signal (`raschkeBurst`'s CV-of-inter-arrival-times) — needs its own tick-level replica tool, built from scratch (no existing groundwork, unlike rows 13/14) | 2026-08-29 |
| 3 | `relative_range` | IN | Live (TS2) | #1 discriminator (0.6373), volatility-level axis | 2026-08-25 / 2026-08-29 |
| 4 | `log_scale_expansion_ratio` | IN-WEAK | TS2, live-vs-bar-gated **unchecked** | **REFORMULATED 2026-08-31**, identical treatment to row 1's `log_scale_ratio` — raw RV replaced with bipower variation, same Mandelbrot 1963 jump-fragility grounding. `FeatureScaler.h` dim3 calibration disabled pending re-audit. Measured correlation with `log_scale_ratio`: 0.7638 pre-fix -> 0.8085 post-fix (both clean) -- a real, substantial redundancy signal, not resolved by this row alone. Redundancy/keep-or-drop decision now owned by `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md`, not this row. | 2026-08-25 (ratio); clock never checked; 2026-08-31 (formula + rename + redundancy measurement) |
| 5 | `vol_convexity` | **REMOVED FROM SCHEMA, 2026-08-31** (stronger than OUT-HMM — the C++ computation itself was deleted, 19D→18D, not just excluded from HMM selection) | Independently weak on two separate measures (cross-state 0.0032; tail-enrichment rank 13/16, below baseline); small-sample noise floor. Live-reactivity wouldn't fix it — correctly not worth further investment (§1.11). Executed via `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` §4 Phase 0 | 2026-08-31 |
| 6 | `lempel_ziv` | IN, **not for fat-tail use** | Event-native | Complexity axis, moderate general discriminator (0.0356) — but scored exactly 0.000 tail-enrichment, the cleanest tail-irrelevance signal found. Keep for complexity axis only, never lean on it for tail work | 2026-08-23 |
| 7 | `hurst_exponent` | IN-WEAK | Live (TS1), diluted weight | Worst discriminator (0.0000); axis (persistence) is legitimate, weakness is likely one live point diluted across a 50-200-bar DFA regression, not cadence-blindness (§1.11) | 2026-08-29 |
| 8 | `micro_asymmetry` | OUT-HMM | TS3, live-vs-bar-gated **unchecked** | Weakest overall (0.0001), already dropped from HMM selection | 2026-08-25 |
| 9 | `fisher_info` | IN-WEAK | Live (TS1) | Second-worst (0.0011); plausibly extreme-point-adjacent but untested for that specific claim | 2026-08-25 / 2026-08-29 |
| 10 | `fast_hurst_exponent` | IN-UNMEASURED | Activity-clock | Shipped (`4969259`) but cross-state ratio never measured, alone or crossed with `relative_range` (§5.4/§6) | 2026-08-29 (schema confirms field exists) |
| 11 | `tail_index` | OUT-HMM | Event-native | Structurally redundant with the model's own native ν_k (DOF-redundancy, §4); stays live for `PositionManager.cpp` position sizing | 2026-08-25 |
| 12 | `skewness_idx` | IN-CONTINGENT | Activity-clock (`7c51f33`) | Asymmetry axis rep; contingently redundant against a future skewed-Student-t emission's native shape parameter (§1.9); cross-state ratio stale (measured pre-move, 0.0005) | ratio 2026-08-25 (stale) / clock 2026-08-27 |
| 13 | `amihud_illiquidity` | IN | **Live-reactive, build-verified 2026-08-29**, guarded on `kLiveBarMinVolume=50.0` (empirically tuned, was 10.0) | #2 discriminator (0.5219), causally tail-adjacent leading indicator. **`DIM_AMIHUD_INDEX` stale-index bug fixed** (was 11, should be 12). **Winsorization + guard tuning DONE, build-verified, 2026-08-29**: guard-comparison (5/10/50, single tick pass) found 50 tames the worst-case outlier 6x (max\|z\| 19583.81→3270.57) for ~7% less mean reactivity — adopted. Bound refit against guard=50's actual distribution for internal consistency (u=p99=32.68, n_tail=7709, ξ=+0.2920, Fréchet), final bound = **2706.0** (p=1/N return level), updated in both `FeatureScaler.h` and `config/execution_params.json`. **Shrinkage re-audit DONE and verified 2026-08-30**: correlation(localMAD,\|z\|)=-0.0340, no collapse signature — stays disabled (`SHRINKAGE_SCALE_MIN[12]=0`), already covered by its own dedicated `AMIHUD_ABSOLUTE_FLOOR` mechanism. No open items remaining | 2026-08-30 |
| 14 | `liq_fragility` | IN | Same as row 13 — live-reactive, build-verified, same guard=50.0 | #4 discriminator (0.2521), same liquidity-spiral logic. **Winsorization DONE, build-verified 2026-08-29, bound re-derived 2026-08-30** (see next item): guard-insensitive (no meaningful difference across 5/10/50) but refit against guard=50 for consistency anyway. **Shrinkage re-audit DONE and independently verified 2026-08-30**: real collapse signature confirmed (correlation(localMAD,\|z\|)=-0.2058), `SHRINKAGE_SCALE_MIN[13]=0.0035` enabled — first verification attempt gave a byte-identical (suspicious) result, traced to the audit tool bypassing `ComputeShrinkageZ()` entirely; fixed by exposing the real shrinkage-blended z via `lastRawZ[13]` in `FeatureScaler.h` and rerunning: max\|z\| 503.22→20.59, correlation -0.2058→+0.0016 (genuinely fixed). This also caught the 2524.5 winsorization bound as having been derived from the same broken (non-shrinkage) distribution — re-derived against the corrected data (u=p99=8.297, n_tail=7709, ξ=-0.1452, Weibull/bounded), final bound = **21.26** (p=1/N return level), updated in both `FeatureScaler.h` and `config/execution_params.json`. No open items remaining | 2026-08-30 |
| 15 | `fast_taleb_kurtosis` | IN-UNMEASURED, **top-priority action** | Activity-clock | First-ever kurtosis dim in this vector's history; defensible case is lead-time/transition-detection, not steady-state tail-heaviness (§4); must be selected into `lbrnet`'s `HMM_KEEP_DIMS` and retrained | 2026-08-29 (schema confirms field exists) |
| 16 | `recurrence_rate` | IN, **orthogonal to this document's goal** | Activity-clock (`7f395d0`) | Topological stability — doesn't map cleanly to any §1 axis (trend/vol/liquidity/persistence/tail-weight/contagion); real literature grounding for the move, unrelated to this document's purpose | 2026-08-28 (commit) / 2026-08-29 (reclass) |
| 17 | `fractal_dim` | IN | Time-bar (TS2), by design | Structural-persistence/roughness reason, window widened to 400 bars (`72ab967`); explicitly NOT activity-clock-eligible, no literature support found | 2026-08-28 |
| 18 | `mean_rev_z` | IN-WEAK, **empirically null** | Live (TS3) | OU-elasticity; confirmed null predictive power in isolation via the actual horse-race test (`mean_rev_z_variant_comparison.py`) — estimator-specific null, axis itself not indicted | 2026-08-28 (test) / 2026-08-29 (doc) |
| 19 | `fast_mean_rev_z` | PAUSED | Activity-clock, wiring paused | Struct field exists (pre-pause groundwork); `ContextManager.cpp` computation never wired. Horse-race test showed both variants empirically null — do not resume without new evidence | 2026-08-29 (fresh grep, zero `ContextManager` hits) |
| 20 | Drift/location (return z-score) | **OUT, tested and rejected 2026-08-30** | N/A — offline prototype only, never wired | Largest identified axis gap (§1.9), but the offline prototype (`tools/drift_location_eval.cpp`, real 38.5M-row MES data, same-sign continuation hypothesis) found hit_rate **below** 0.5 at every horizon (0.4854@30min → 0.4957@240min, decaying toward null as horizon lengthens — the opposite shape a momentum candidate should show). Statistically significant only because n is in the tens of millions; effect size is economically tiny (1-1.5pp). Rejected before any schema/C++ commitment — exactly the outcome the offline-first discipline exists to catch cheaply | 2026-08-30 (tested) |
| 21 | Jump/bipower-variation ratio | **CANDIDATE-VALIDATED, tested and SURVIVES 2026-08-30** | N/A — offline prototype only, not yet wired | Isolates jump-share of realized variance, an axis nothing currently covers (§5.1). Offline model-independent magnitude test (`tools/jump_ratio_eval.cpp`, real 38.5M-row MES data, median/MAD-based per this codebase's own fat-tail convention) found the top decile's median\|forward return\| is 55.2%/55.1%/58.3%/62.6% of the bottom decile's at 30/60/120/240min, CI excluding zero at every horizon — real, substantial, opposite-of-naive-hypothesis effect (high jump-share predicts a CALMER near-term future, not more chaos). Not yet promoted to a schema field (§6.2); a real i.i.d.-bootstrap-on-overlapping-signals methodology gap was found and documented, not yet fixed (§10.9) — doesn't change this verdict given the effect size, but should be resolved before trusting a more marginal future candidate on the same apparatus | 2026-08-30 (tested) |
| 22 | Hurst × volatility-level cross-term | CANDIDATE | N/A — feature-engineering only, no new data | Tests whether "fat-tail" and "Trending-High-Vol crisis" (§1.4/§1.7) are the same latent phenomenon | 2026-08-29 |
| 23 | Self-exciting jump clustering (Hawkes intensity) | CANDIDATE-DEFERRED | N/A — not built | Most lead-time-relevant candidate (§1.10) but real MLE estimation cost; deferred to post-workstation window | 2026-08-29 |
| 24 | Realized semi-variance decomposition | CANDIDATE-DEFERRED | N/A — not built | Dynamic directional-tail-asymmetry measure (§5.3); logged, no priority assigned yet | 2026-08-29 |
| 25 | Recovery time-since-last-extreme-event construct | CANDIDATE-DEFERRED | N/A — needs design first | Represents Guidolin & Timmermann's "Recovery" state (§1.2); not yet designed, not just unprototyped | 2026-08-29 |

---

## 10. C++/Arrow `.context` → `.context.parquet` converter — design (2026-08-30)

**Why this belongs in this document, not a separate one**: this tool exists to make the dimension
count/layout work above (16D→17D→18D→19D, and whatever §9's `CANDIDATE` rows add next) *safe* to
collect and consume — it is data-pipeline infrastructure for the same observation vector this whole
document is evolving, not an unrelated tool. User's explicit instruction, 2026-08-30: augment this
living document rather than open a new spec file.

**The concrete problem that motivated this** (found analyzing `lbrnet`'s existing converter,
`materialize_context_parquet.py`, before any design work started): `ObservationData` is a
fixed-layout FlatBuffers `struct` (`mts_schema.fbs:379`), not a `table` — no per-field IDs, just raw
byte offsets. `fast_hurst_exponent` and `fast_taleb_kurtosis` were inserted *mid-struct* (positions 9
and 14), not appended, when they shipped (2026-08-27/28). Reading an older-width `.context` file's
raw bytes using the *current* field count/order therefore doesn't fail — it silently relabels every
field from the first inserted position onward, and reads pure garbage past the old struct's true byte
extent. This is not hypothetical: `lbrnet/data/raw/event_data.context` (14.8GB, last written
2026-08-17) is cached at `observation_dim=16` in its own sidecar `.meta.json`, while live code is now
at 19 fields. `train_student_t_hmm.py`'s `_load_unbounded_mo_ss_context()` calls `is_cache_fresh()`
(currently `False`, 16≠19) and on failure auto-invokes `materialize_context_parquet()` — which would
silently produce a corrupted 19-column rebuild, mislabeling `amihud_illiquidity`/`liq_fragility`
(the pair just recalibrated in §9 rows 13/14) among others, the very next time anyone runs an
unbounded training load against that file. Full trace: this session's conversation log, no separate
write-up — the finding is fully captured here.

### 10.1 Ownership boundary (decided)

**C++ owns everything that touches raw `.context` bytes: parsing, analysis, processing, and deriving
`.context.parquet` from them. Python (`lbrnet`) owns everything downstream of `.context.parquet` only
— training, backtesting, evaluation.** This is a full replacement, not a complementary fast path:
`lbrnet/lbrnet/scripts/materialize_context_parquet.py` and
`lbrnet/lbrnet/data/observation_vector_bulk_reader.py` (all three read modes — unbounded, bounded
head, bounded tail — plus incremental-resume bookkeeping) retire once parity is proven. Rationale
(user, 2026-08-30): partial `.context` files get created routinely just to test evolving
functionality — C++ should be the one place that ever interprets those raw bytes, since it's also the
one place that ever writes them (`LBRFileManager.cpp`).

**Explicitly out of scope for this design**: the actual `lbrnet`-side integration (updating
`train_student_t_hmm.py`'s call sites, retiring the Python reader module, migrating any other
consumer) is deferred until the C++ side is shippable — per this repo's standing convention, a
`lbrnet`-rooted session picks that up, informed by a real, working binary rather than a
speculative interface.

### 10.2 Schema-versioning fix (decided)

The wire format already carries a per-file `FileMetadata` table (`symbol`, `timeframe`,
`schema_version: uint16`, `created_timestamp: long`) written once at `Open()`
(`LBRFileManager.cpp::WriteFileMetadata()`), right after the `"LBRN"` magic header. `schema_version`
has been hardcoded to `230` ("v2.3.0") since before any recent `ObservationData` layout change, is
never bumped, and the Python reader currently just skips over this blob (`f.read(meta_size)`,
discarded) rather than parsing it. The exact mechanism needed already exists — it's just unused for
this purpose.

**Decisions**:
- **One field, not two.** Fold the "does `ObservationData`'s byte layout match what I expect"
  question into the *same* `schema_version` field rather than adding a dedicated observation-vector-
  width counter — a struct-width change is a wire-compatibility break either way, which is exactly
  what this field already exists to signal.
- **Hard refuse on mismatch, never silent reinterpretation.** The converter parses `FileMetadata` for
  real (first time this blob is ever actually read, not skipped) and compares `schema_version`
  against its own compiled-in expected value. Any mismatch is a clear, loud error — never an attempt
  at multi-era parsing/upgrading. A mismatched file needs an explicit, separate, one-off decision
  (re-collect fresh, or a dedicated migration script) — never auto-processed.
- **Bump cadence: once per campaign, not once per field.** Bump `schema_version` now, once, to mark
  the current 19-field layout as canonical — and hold it fixed through the *rest of this entire
  observation-vector campaign* (including future field changes from §9's still-open `CANDIDATE` rows
  — drift/location, jump/bipower-ratio, etc., whenever they actually land) until the vector ships to
  `lbrnet`. Only at that point does normal per-change-bump discipline resume for future work.
  (User, 2026-08-30, explicit: "the version is set ONCE and k[ep]t at that value until we ship to
  lbrnet.")
- **Named, accepted residual risk**: two test `.context` files collected at different points *within*
  this campaign (e.g. one at today's 19D, another after a future field lands) will carry the *same*
  `schema_version` and won't be distinguishable by version number alone. Accepted because these are
  explicitly disposable dev/test artifacts for verifying evolving functionality, not a persisted
  training corpus — not silently glossed over, named here on purpose. The one case this mechanism
  exists to catch — a stale, large, real archive (`event_data.context`, stamped 230) reaching a much
  later code state — is fully covered: 230 will never equal the new campaign version, so it refuses
  cleanly.
- **Existing `event_data.context` (16D, stamped 230)**: correctly and cleanly refused by this
  mechanism once it ships. Its recovery/migration is an explicit separate decision, out of scope
  here — not handled by this tool.

### 10.3 Build path (decided)

Standalone, bare `g++`/vcpkg tool — same convention as `tools/amihud_liqfragility_recalibration.cpp`
(a one-line documented `g++` invocation using the existing
`-I /mnt/c/Users/rcruz/vcpkg/installed/x64-windows/include` vcpkg path, built on demand). Not a
CMake target, never touches `build_dll.sh` or the Windows cross-compile — the DLL itself has no
runtime need for Arrow/Parquet.

### 10.4 Output schema (open to redesign — decided)

Not a byte-for-byte match to today's Python output; two concrete changes:

- **`risk_gate_` naming collision fix.** 5 of `RiskGateContext`'s 14 float fields
  (`hurst_exponent`, `fisher_info`, `amihud_illiquidity`, `mean_rev_z`, `fractal_dim`) share a name
  with an `ObservationData` column but hold a genuinely different value (raw/unscaled vs.
  log-z/winsorized) — today's fix is a blanket `risk_gate_` prefix on all 14 fields. New scheme:
  suffix *only* the 5 that actually collide with `_raw` (e.g. `hurst_exponent_raw` next to
  `ObservationData`'s `hurst_exponent`); the other 9 non-colliding `RiskGateContext` fields
  (`shannon_flow_entropy`, `shannon_efficiency`, `taleb_kurtosis`, `taleb_skewness`,
  `elder_chandelier_atr`, `pareto_tail_alpha`, `spread_stress`, `raschke_burst`,
  `amihud_percentile`) keep their plain names — no forced prefix noise where there's no ambiguity.
- **`RiskGateContext`'s field list derived programmatically, not hand-maintained.** Today's Python
  reader hardcodes `RISK_GATE_FLOAT_FIELDS`/`_RISK_GATE_ACCESSOR_NAMES` as two parallel tuples that
  must stay in sync with `mts_schema.fbs` by hand — the same hand-maintained-duplicate defect class
  as `regenerate_schema.sh`'s own confirmed-twice-drifted contract header (§6.2). C++ has no runtime
  member-name reflection the way Python's `RiskGateContextT().__dict__.keys()` trick works, so the
  institutional-grade fix (elite option, per direct request, 2026-08-30) is FlatBuffers' own
  **schema reflection API** (`flatc`-emitted `.bfbs` binary schema + `flatbuffers::Reflection`) to
  enumerate `RiskGateContext`'s real field names/order at generation time — never a second hand-typed
  copy of anything the schema compiler can already tell you.
- **Folded in, same design (decided, 2026-08-30): fix `regenerate_schema.sh`'s structural defect as
  part of this work.** The reflection mechanism the converter needs for `RiskGateContext` is the
  *exact* mechanism §6.2 already flagged as the real fix for `mts_schema_contract_generated.h`'s
  hand-maintained duplicate template (confirmed drifted twice: `fast_taleb_kurtosis` undetected for
  weeks, `fast_hurst_exponent`/`fast_mean_rev_z` blocking the build outright). One coherent piece of
  work: `regenerate_schema.sh` derives `kObservationFieldNames` (already exists, now genuinely
  reflection-derived instead of hand-templated) and a new `kRiskGateFieldNames` from real `.fbs`
  reflection; the converter tool consumes both. Closes a previously-open, already-fired-twice risk as
  a side effect rather than as separate future work.

### 10.5 Downstream cache layer — no action needed (checked 2026-08-30)

`lbrnet/data/hmm_training_cache.py` sits one layer further downstream, between `.context.parquet`
and the actual EM fit: a memmap-backed float32 `(n_rows, n_cols)` `.npy` cache (`event_data.context.
parquet.hmm_train_f32.npy` + `.meta.json` sidecar) so every BIC grid-search candidate/CV fold reuses
the same pre-cast array instead of re-reading the parquet each time. Checked against real data: it's
currently fresh (built 2026-08-25, its recorded `context_parquet_size`/`mtime` match the current
`.context.parquet` exactly) and faithfully 16D, same era as its parent. **Explicitly out of scope for
any fix — it's already safe by construction**: it selects columns *by name* via Polars
(`pl.col(c).cast(...)`), not by raw byte offset, so a dimension mismatch here fails loud
(`ColumnNotFoundError`) rather than silently misaligning, and its freshness key is keyed directly off
`.context.parquet`'s own `stat()` — it self-invalidates correctly the moment that parquet changes,
with no dependency on anything §10 fixes above it.

### 10.6 Open items — RESOLVED, 2026-08-30 (kept for history; see §10.7 for the newest addition)

All items originally listed here are now closed: CLI shape shipped (`context_to_parquet`/
`context_validate`, all 13 plan tasks committed `689465a`..`54d1702`); `.bfbs`/reflection mechanics
resolved as `flatc --jsonschema` (not raw `.bfbs` — simpler, no reflection-bindings vendoring needed,
`schema/scripts/generate_contract_header.py`); `schema_version` shipped as **240**. Still genuinely
open, not resolved by this work: the `event_data.context` (16D) recovery/migration decision (§10.2)
and the `lbrnet`-side integration itself (§10.1) — full handoff written,
`docs/superpowers/specs/2026-08-30-context-converter-lbrnet-handoff.md`, but not started.

### 10.7 `tools/drift_location_eval.cpp` — model-independent candidate validation (added 2026-08-30)

A second, lighter standalone tool, built while testing §5.0 (see that section's result). Notable as
this codebase's **first C++ tool to read Parquet directly** (`parquet::arrow::FileReader` — every
prior tool, including `context_to_parquet.cpp`, only writes) — eliminates the old
`mean_rev_z_variant_comparison.py` pattern's Python round-trip (polars-read → custom-binary-export →
C++-read → CSV-out → Python-scores) entirely. Two real bugs caught and fixed during verification, not
assumed away:
- **A 40x+ performance bug**: an unprojected `ReadTable()` decoding all 12 columns (including a
  string column) took 5+ minutes with zero output on the real 38.5M-row file; projected to just the
  2 needed columns (indices resolved from the real schema, never hardcoded), it's 7.5s.
- **An algorithmic-complexity bug**: `ComputeForwardReturns`'s original per-signal `std::lower_bound`
  (binary search) into a ~308MB sorted array thrashes cache on every probe — measured hanging 100+
  seconds across 4 horizons. Both series are the same monotonically-sorted sequence (verified via a
  real `pl.Series.is_sorted()` check against the production file, not assumed), so it became a single
  O(n) two-pointer merge instead — completes in ~2 minutes total for the whole tool now (dominated by
  page-fault system time on WSL2, not algorithmic complexity).

**Standing methodology, not a one-off**: this tool establishes the pattern for validating any future
candidate that doesn't strictly require HMM state labels — same-sign (continuation) or negated-sign
(reversion) hit-rate against **real forward market returns**, formulas ported verbatim from
`tools/dim_acceptance_eval.py` (Wilson CI, Bonferroni-corrected multi-horizon test), never against
the model's own state decode while `hmm_model.pkl`'s training-data invalidities remain unresolved
(§5.0's own correction). Use this same tool (parameterized differently) or this same pattern for
§5.1's jump/bipower-variation ratio next.

### 10.8 `tools/jump_ratio_eval.cpp` — §5.1 validation, and the mean→median correction (added 2026-08-30)

Shares `market_data_io.h`/`market_test_stats.h` with §10.7's tool (extracted into those headers on
this, their second real use — the "wait for the second use" heuristic this whole plan was built
around). `jump_ratio` is non-negative by construction (`max(0, (RV-BV)/RV)`), so §10.7's same-sign
hit-rate test doesn't apply (no sign to test against); the correct test, confirmed by reading
`tools/dim_acceptance_eval.py`'s `predictive_power_magnitude()` directly rather than assumed, is a
top/bottom-decile bootstrap-gap magnitude test on `|forward return|`.

**A real performance emergency, found only by actually running it against the full 38.5M-row file
(the third time this session a complexity bug was caught this way, not estimated in advance)**: the
exact multinomial bootstrap resample is memory-latency-bound at real decile-group scale (~3.85M
elements/group, ~95ns per random gather) — projected ~97 minutes for a real 4-horizon run; an actual
run was killed after 61 minutes still on horizon 2/4. Fixed by switching to a weighted/"exchangeable"
bootstrap (Praestgaard & Wellner 1993) above a 20,000-element threshold: i.i.d. weights assigned to
each element, summed in one sequential pass instead of n random gathers. Two weight distributions
were tried and rejected before landing on the right one — Poisson(1) (Chamandy et al. 2012) has the
theoretically-correct variance but `std::poisson_distribution` itself measured ~73ns/draw, almost as
expensive as the gather it replaced; Uniform[0,2] was fast (~35ns/draw) but has variance 1/3, not the
~1 the theory requires, and was caught by review producing CIs ~1.7-1.8x too narrow before being
replaced with Exponential(1) (Rubin 1981), which is both fast (~28.5ns/draw) and correctly scaled.

**The mean→median correction (§5.1's own writeup has the full result)**: caught by direct user
challenge, not by a code review — using `mean(|forward_return|)` as the magnitude statistic matched
`dim_acceptance_eval.py`'s own precedent but repeated a mistake this codebase already fixed once
before (Kim & White 2004; `FeatureScaler.h`'s median/MAD "Taleb-consistent" convention is the actual
standing standard here). `ComputeBootstrapMedianGapCI` is new, native-only infrastructure (no Python
counterpart to port) — a weighted-median analog of the mean version, sorting once per group then
finding, per resample, the sorted position where cumulative Exponential(1) weight crosses half the
total. Independently cross-validated against an exact multinomial median bootstrap under Normal,
Student-t(2.5), and Cauchy data during review (ratios 1.02-1.05) — stronger verification than a
normal-only check would have given.

**Real, not yet fixed, methodology gap found during review**: both this tool and §10.7's treat
per-tick forward-return signals as i.i.d. when resampling, but real forward returns overlap heavily
(a 240-minute return spans thousands of adjacent ticks) — the true CI is wider than either tool
currently reports, by an unquantified factor plausibly in the tens-of-x range at these overlap
ratios. Neither candidate's verdict changes (§5.0's rejection and §5.1's survival are both far enough
from the naively-calibrated boundary to survive even a much wider correctly-calibrated CI) — but this
is flagged, not fixed, here: a proper fix (block bootstrap, reusing this codebase's own measured
Politis-White block-length precedent from the `fractal_dim` work, ≈404.82 on real MES data) belongs
applied uniformly across this standing methodology, not patched into one candidate's test ad hoc. See
§10.9.

### 10.9 Standing methodology gap: i.i.d. bootstrap on overlapping signals (found 2026-08-30, not yet fixed)

Applies retroactively to §10.7 (`drift_location_eval`) as well as §10.8 (`jump_ratio_eval`) — both
tools' hit-rate/bootstrap tests resample individual per-tick forward-return signals independently,
but consecutive signals share most of their underlying tick data once the forward horizon (30-240
minutes) is much larger than the per-tick sampling interval. This overstates effective sample size
and understates every reported CI/p-value. Not yet quantified precisely for either tool, and not yet
fixed for either — this codebase already has the raw ingredient (a measured Politis-White circular
block-length, ≈404.82, from the `fractal_dim` window-widening work, `72ab967`) that a real fix should
reuse rather than re-derive. Scoped as its own future initiative (block-bootstrap or equivalent
de-overlapping applied to both existing tools and any future §10.7/§10.8-pattern candidate test), not
folded into either candidate's own section, since fixing it for one candidate but not the other would
leave the ledger's §9 verdicts on an inconsistent evidentiary footing.
