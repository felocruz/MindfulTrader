# Triple-Barrier Exit Engine — C++ Live Execution Spec (Chandelier + Retail Scale-Out → Dynamics-Driven Triple Barrier)

> Status: **SPEC — PHASE 0/1 LOCKED (2026-07-14); later phases EVOLVABLE.** All
> §8 open questions are resolved: Elder Breakout target locked at 1.5R; Finding 1
> (`UpdateContext()`) promoted to a mandatory Phase 1 deliverable; daily-bias veto
> KEPT on both sides. Phase 0/1 is a final implementation contract. Phase 2+
> parameters, physics weights, and thresholds remain calibration targets — evolve
> those as each phase's replay-calibration evidence lands.
>
> **Owner**: MindfulTrader (C++ producer/execution layer), co-evolved with
> `lbrnet` (Python).
> **Co-evolution counterpart**: `../../lbrnet/docs/ADR/phase2_triple_barrier_migration_spec.md`
> (Python Phase 2). Python currently has a *working implementation of its own
> side*, but that is **not the source of truth** for this design. Neither repo is
> authoritative by default: a design decision is settled only once the C++ and
> Python implementations are **mutually consistent** and both validated. Until
> then, every decision here — including those that originated on the Python side —
> is provisional and open to revision from either direction. Different parts of
> the system may legitimately *lead* from different sides (e.g. the physics-driven
> exit layer is native-first and may be pioneered in C++; the labeler's barrier
> algebra may be pioneered in Python) — leadership is per-component, not global.
>
> ### Co-evolution principle (read first)
>
> This spec and its Python counterpart evolve **concurrently**. When the two
> sides disagree, the tie-breaker is **not** "whichever repo wrote it first" — it
> is the institutional/literature standard (Raschke/Elder, López de Prado, the
> physics stack), decided on the merits, then made consistent on both sides. The
> shared golden-vector parity fixture (§5.2) is the mechanical gate that proves
> consistency has actually been reached, not just asserted. Treat any statement
> below sourced from the Python migration as a **proposal to co-validate**, not a
> settled input.
>
> **Grounding sources** (read before implementing any phase):
> - `../../lbrnet/knowledge/global/execution/triple_barrier_method.md` — López de
>   Prado, *Advances in Financial Machine Learning* Ch. 3: the canonical method,
>   volatility-scaled barriers, first-hit-wins, no uncapped-target concept.
> - `../../lbrnet/knowledge/global/execution/raschke_swing_barriers_and_position_sizing.md`
>   — Raschke/Elder structural stop/target grounding, per-pattern tactical
>   toolkit, verified against real C++ source.
> - `../../lbrnet/docs/ADR/phase2_triple_barrier_migration_spec.md` — the
>   co-evolving design directives (hard cutover, single-stage, all 9 patterns);
>   proposals to co-validate, not settled inputs (see co-evolution principle).
> - `../../lbrnet/docs/labeling/LABELING_AND_AUGMENTATION_SPEC.md` — TRAP weighting
>   policy table (normative default) for physics-signal fusion weights.
> - `execution_correctness_findings_spec.md` (this repo) — Findings 7, 12, 14, 17
>   are directly resolved or touched by this migration; see §7.

---

## 1. Decision and rationale

Replace the C++ live-execution exit engine — `ChandelierStopManager` (ATR trailing
stop) **and** the T1/T2/T3 retail scale-out ladder (`CalculateScaleOutTargets` +
attached OCO targets) — with a **dynamics-driven triple-barrier exit**:

1. A **bounded three-barrier race** per López de Prado (upper = profit-take,
   lower = stop-loss, vertical = max-hold time limit; first-hit-wins).
2. Barrier *geometry* anchored in **Raschke/Elder pattern structure** (swing
   fractals, candle-wick stops, N-bar extremes) — per-pattern, not one uniform rule.
3. Barriers **immutable once set at entry** — the engine is blind to price-path
   fluctuations inside them (no retail trailing). The *only* mid-hold intervention
   is a discrete **regime-invalidation kill-switch**: if the point-in-time physics
   context (`.context`) mutates to a state where the entry's statistical edge no
   longer exists, the position is terminated. This is what replaces the retail
   scale-out ladder. Coherent rule: **we do not manage the price path; we manage
   regime sanity** (Claude/Gemini convergence, 2026-07-14).

### Why replace the scale-out ladder (not just the trailing stop)

Fixed T1/T2/T3 R-multiple scale-out is a **retail psychological construct** —
it books partial profit at arbitrary R-multiples to make a discretionary trader
feel good, not because market dynamics said the edge decayed at 1.5R. It is also
**mathematically alpha-destructive**: exiting half at 1.5R forces the remaining
half to reach **4.5R** just to net 3.0R — it truncates the right-hand tail (the
big winners) while still absorbing full entry tail-risk. That is an insurance
premium paid to the market to ease anxiety. This system has strictly better
information for the *when-to-reduce* decision:

- **Statistical mechanics** — Shannon flow entropy, Hurst/DFA persistence, Taleb
  kurtosis / cliff proximity, Pareto (Hill) tail index — measure in real time
  whether the edge that justified the entry still exists.
- **Pattern structure** — Raschke/Elder tactics encode *where* structure says the
  move should end (swing high/low, N-bar extreme, expansion exhaustion).
- **López de Prado** — the triple barrier gives a rigorous, path-aware,
  first-hit-wins frame that unifies all three into a single, testable exit rule
  and collapses labeler ↔ Phase-1 ↔ Phase-2 ↔ live into one barrier definition.

An exit should fire because a *measurable* condition changed (entropy spike, tail
regime, structural target reached, persistence collapse, time exhausted), not
because price hit an arbitrary multiple of initial risk.

### Why now

The Python side has a working implementation of its half of this migration, while
the live C++ layer still runs Chandelier + scale-out. That divergence is the
cost driver: as long as the two sides run different exit definitions, the
labeler, Phase-1 τ* calibration, Phase-2 backtest, and live execution keep
re-accruing reconciliation debt every time either side changes. Co-evolving both
onto **one jointly-agreed barrier definition** collapses that recurring category
into a non-issue. This is not "C++ catching up to Python" — it is both sides
converging on a shared definition, with each free to push corrections back into
the other (e.g. this migration's C++ audit may revise Python's per-pattern
parameters, not only the reverse).

---

## 2. Standing directives (user-issued, apply to both sides)

These are explicit **user** directives about the design itself (not Python-repo
decisions). They first surfaced during the Python work but govern both sides
equally, and remain open to the same co-evolution revision as everything else if
the institutional case changes:

1. **Hard cutover.** `ChandelierStopManager` is fully removed — not flag-gated,
   not commented out, no dead code, no backward-compat path. The retail scale-out
   ladder is removed on the same terms.
2. **Single-stage.** No "trail after the upper barrier fires" second stage. Every
   pattern runs one bounded finite race in v1. (The mid-hold regime-invalidation
   kill-switch in Phase 2 is a discrete safety exit, not a trailing stop — see
   §5.4.)
3. **All 9 patterns** run the same barrier structure; per-pattern differences live
   in geometry/parameters, not in a different mechanism.
4. **No uncapped/infinite target.** The method is a race between three *finite*
   conditions by construction. A "let it run" runner is a deliberate future
   departure requiring its own grounding, not a default.
5. **Native-first.** The exit engine must run fully in C++ independent of Python
   availability. Physics signals consumed are already computed natively
   (`ContextManager`/`StatisticalContext`); Python is refinement/calibration only.
6. **Schema evolution is in-scope.** `../../schema/mts_schema.fbs` may be evolved
   as part of this migration (user decision 2026-07-14). This removes the
   "schema-change blast radius" caveat that previously gated raw-signal
   serialization (Findings 19–21) and any new exit/structural wire fields — those
   become planned schema work, not deferred blockers. Schema changes still follow
   the regeneration contract (`regenerate_schema.sh` before build) and the
   doc-sync/versioning discipline, and remain a co-evolution artifact (the wire is
   the C++/Python contract — both sides move together).
7. **Immutable barriers; regime-invalidation is the only mid-hold intervention**
   (Claude/Gemini ruling, 2026-07-14). Price barriers (target/stop) are locked at
   entry and never move with price — no trailing, no milestone-based tightening.
   The position's *lifecycle* is nonetheless regime-dependent: a discrete
   kill-switch flattens (at larger size: reduces) the position when the live
   `.context` physics violate alpha-validity thresholds. Rationale: a
   **concentrated single-instrument book** has no diversification cushion for a
   non-diversifiable tail event, so regime-invalidation defense is correct — but it
   fires on *measured EV decay*, not price-path anxiety. Continuous barrier-width
   modulation is **not** used mid-hold (it is the `vol_convexity` failure class —
   see §5.4).

`ChandelierStopManager` is wired into `src/PositionManager.cpp` at ~8 live call
sites: `InitializeStop` (~L333), `IsTrailingActive`/`ActivateTrailing` (~L356-364),
`RemoveStop` (~L385), `UpdateStop`/`GetStopPrice`/`ForceTightenStop` (~L699-758),
`ShouldStopOut` (~L758). Adjacent mechanisms in scope:

- **Scale-out ladder** — `CalculateScaleOutTargets()` (`PositionManager.cpp:3048-3215`)
  + attached OCO orders (`AttachedOrderTarget1/2/3Type`, ~L2393-2419, ~L2862),
  including the `SCT_ORDERTYPE_TRIGGERED_TRAILING_STOP_LIMIT_3_OFFSETS` runner leg.
- **Structural anchoring/capping** — `PositionManager.cpp:1961-2016` (stop anchor to
  confirmed swing fractal, tightening-only) and `:3178-3211` (target cap at
  `swingHigh()/swingLow()`). **Retained and generalized** by this spec (§5.3), not
  removed — this is exactly the Raschke/Elder structural layer we want.
- **Seed stop/target** — `CalculateTacticalTriggerPrices` / `CalculateStrategySetupPrices`
  (`PositionManagerPatterns.cpp:179-457`). These seed the barriers and carry the
  **Finding 17 enum-collision bug** — fixed as part of this work (§7, per user
  decision 2026-07-14).

Files to be deleted: `src/ChandelierStopManager.cpp`, `include/ChandelierStopManager.h`,
and their `CMakeLists.txt` entry (`src/ChandelierStopManager.cpp`, line ~64).

---

## 4. Barrier definitions

Let `entry` = fill price, `atr` = ATR10 (DOF-scaled per `DofStopScale()` where the
pattern uses it), and `dir ∈ {+1 long, −1 short}`.

### 4.1 Lower barrier (stop-loss) — structural, tightening-only

```
stop_candidate = entry − dir · (stop_mult · atr)          # volatility-scaled seed
stop_structural = Raschke/Elder structural level          # per-pattern (§5.3)
stop = the TIGHTER of {stop_candidate, stop_structural}   # never looser than seed
```

Per-pattern `stop_mult` and structural level source (2-bar low for Elder Breakout,
hammer/shooting-star wick for Kangaroo Tail, etc.) come from the per-pattern audit
(§5.3). The stop is **fixed at entry** — no ratcheting (that was Chandelier).

### 4.2 Upper barrier (profit-take)

Per the composition ruling (**Reading B**, 2026-07-14 — recommended by Claude,
confirmed against live C++ by Gemini CLI), the target source is **per-tier**:

```
# HIGH dedicated (Turtle Soup, Momentum Pinball):
#   target = target_structural            # the raw N-bar extreme, DIRECTLY
#   (target_r_mult is NOT applied — a 1.0R cap would truncate the fade's edge)
# Elder Breakout (hybrid): target = entry + dir · (1.5 · risk)   # fixed 1.5R
# LOW / defaulted:  target = entry + dir · (target_r_mult · risk)
# ALL: then cap at the universal 60-min swing extreme if nearer (tightening-only)
```

Finite by construction (directive #4). Rationale for Reading B: the dedicated C++
functions assign the N-bar structural extreme to `targetPrice` directly — no
volatility candidate, no nearest-of comparison — and for a mean-reversion fade the
opposing structural extreme *is* the authentic Raschke target; a 1.0R cap would cut
off the reversion. The `target_r_mult` column in §5.3 is therefore the
ladder-parity representation: it drives the LOW patterns and Elder's 1.5R, and is
*informational only* for the HIGH dedicated patterns.

### 4.3 Vertical barrier (time limit) — regime-conditioned max-hold

**Not per-pattern** (co-evolution finding, 2026-07-14). The originally-assumed
per-pattern `max_hold_bars` has **no real mechanism in compiled C++** —
`GetMaxHoldBars()`/`TimeExitConstants` appear in `docs/EXIT_STRATEGIES_COMPREHENSIVE.md`
but do not exist in the built DLL, so there was never anything to mirror. The
horizon is therefore **regime-conditioned**, matching the Python labeler's
`_MAX_BARS_BY_REGIME`, on the **TS3 15-minute bar clock**
(`currentBarIndex − entryBarIndex`):

| HMM regime at entry | max bars | rationale |
|---|---|---|
| `COILED_SPRING` | 40 | compression/setup; slow-building |
| `GAUSSIAN_STABLE` | 25 | normal baseline (also the absent/default) |
| `GAUSSIAN_FRAGILE` | 15 | toxic flow / fat tails; fast adverse development |
| `PARETO_MOMENTUM` | 12 | directional thrust; resolves quickly |

Fixed at entry from the **live** regime — which means the regime input must not be
frozen (ties directly to §7 Finding 1: if `UpdateContext()` stays unwired, every
trade silently falls to the `GAUSSIAN_STABLE`/25-bar default). On expiry with
neither horizontal barrier touched → flatten at market; label falls back to
`sign(pnl)`.

**Regime ∧ pattern via `min()` (design direction).** The horizon combines the
regime cap above with an optional per-pattern cap by taking the **tighter** of the
two: `max_bars = min(regime_cap, pattern_cap)`. This is native to first-hit-wins
(the time barrier is the tightest applicable horizon), avoids the parameter
explosion of a 4×9 cross-product (9 pattern caps + 4 regime caps combined by
`min`), and **degrades gracefully**: set every `pattern_cap = ∞` and it collapses
exactly to the regime-only table — so Phase 1 ships regime-only, and pattern caps
(e.g. NR7 ≈ 3 bars per its `EXPAND:1-3BARS` tag) are tightened one at a time as
separately-attributable Phase-3 increments. Time has no R:R coupling, so `min()` is
safe here — unlike the horizontal barriers (§4.1/§4.2), which combine by
regime-scales-the-stop / target-as-R-multiple to preserve the ratio.

Sierra Chart attached orders do **not** natively express a time barrier, so the
vertical barrier is **software-managed** in `PositionManager::Update()` on the tick
loop (all three screens are hot every tick — no reduced cadence assumption).

### 4.4 First-hit-wins resolution

On each tick, check in priority order: **regime-invalidation → vertical → lower →
upper** (the horizontal order mirrors the Python `scan_lookahead()`
structural-priority checks). The first satisfied condition resolves the trade.
Regime-invalidation (§5.4) sits at the top because it is the safety layer — a
toxic-flow / fat-tail context override supersedes waiting for price to reach a
barrier. Horizontal barriers are placed as a **static attached OCO bracket at
entry** (stop order + limit target); the vertical barrier and the regime
kill-switch are enforced in software and cancel the resting bracket on fire.

---

## 5. Architecture

### 5.1 New component: `TripleBarrierExitManager`

Replaces `ChandelierStopManager`. Responsibilities:

- Compute the three barriers at entry via the 4-step Adaptive Barrier Protocol (§5.6).
- Own the static OCO bracket geometry handed to the order-submission path.
- Enforce the vertical barrier and regime-invalidation kill-switch each tick.
- Report resolution (`STOP_HIT` / `TARGET_HIT` / `TIME_EXIT` / `REGIME_INVALIDATION`).

DOD/hot-path rules apply: no heap allocation in the tick path, `std::array` +
`IndicatorKey`-style lookups, deterministic. Per-position state is a small POD
struct (barrier levels, entry bar index, max-hold, pattern id).

### 5.2 Parity contract with `lbrnet`

The C++ and Python barrier computations must converge to **numerically identical**
output for the same inputs — `compute_barriers()` / `scan_lookahead_hybrid()` on
the Python side, `TripleBarrierExitManager` on the C++ side. Concretely: same ATR
estimator (ATR10, DOF-scaled where applicable), same per-pattern `stop_mult` /
`target_r_mult`, same structural anchoring/capping rule, same vertical-barrier
semantics. **The shared golden-vector fixture (same inputs → same barrier levels,
asserted on both sides) is the mechanical definition of "consistent" for the
co-evolution principle** — a decision is not settled until this fixture is green
on both repos.

Parity is bidirectional, but authority is **per-quantity**, decided on the merits:
- **Real 15/60/240-min bars** (TS1/TS2/TS3) and the live `StatisticalContext`
  physics signals are computed natively in C++ first; Python reconstructs them
  (`multiscale_bars.py`) to match — here C++ leads because it owns the source data.
- **Barrier algebra and label semantics** may be pioneered in Python's labeler and
  ported to C++ — here Python may lead.
- Where the two disagree on a value, resolve to the literature/institutional
  standard (the same rule `lbrnet` already applied to Finding 7's Chandelier
  asymmetry), then make both sides consistent. Neither "C++ said so" nor "Python
  said so" is itself an argument.

### 5.3 Raschke/Elder structural layer (per-pattern geometry)

Reuse the existing, already-live structural anchoring (`PositionManager.cpp:1961-2016`,
`:3178-3211`) via `IntermediateMarketAction::swingHigh()/swingLow()/prevSwingHigh()/
prevSwingLow()` (60-min 5-bar fractal). Generalize it into a per-pattern table
keyed by `RaschkeTacticalTrigger`, drawing on the tag metadata already curated in
`rc_enums.py:419-465` (candle-wick stop for Kangaroo Tail, 4-bar extreme for Turtle
Soup, 10-bar for Momentum Pinball, 2-bar structural low for Elder Breakout, etc.).
A full 9-pattern audit (cross-referencing tag metadata + `CalculateTacticalTriggerPrices`
+ `ShouldUseTrailingStop()`) was run **jointly** against the live C++ source and the
Python-side audit in `../../lbrnet/docs/ADR/raschke_pattern_stop_target_audit.md`.
The reconciled result is below (§8 Q1, **RESOLVED**).

**Reconciled 9-pattern parameter table (Phase 0).** `stop_mult` / `target_r_mult`
are the values both sides already agree on; `target_r_mult` = the scale-out
ladder's t1 tier (`PositionManager.cpp:3060-3095`), which `_LABEL_TARGET_PARAMS`
mirrors exactly. Values apply to both BUY/SELL sides (mirrored).

| Pattern (`RaschkeTacticalTrigger`) | `stop_mult` | `target_r_mult` (ladder t1) | Structural stop | Structural target | Confidence | Finding-17 seed |
|---|---|---|---|---|---|---|
| Turtle Soup | 0.5×ATR | 1.0 | `low ∓ 0.5×ATR` (dedicated) | 4-bar high/low extreme (dedicated) | **HIGH** — dedicated C++ verified | dedicated formula |
| Momentum Pinball | 0.4×ATR | 1.0 | `low ∓ 0.4×ATR` (dedicated) | 10-bar high/low extreme (dedicated) | **HIGH** — dedicated C++ verified | dedicated formula |
| Elder Breakout | *(structural)* | 1.5 | **2-bar structural** `min(low,prevLow) − tick` — **NOT ATR** (correction) | fixed 1.5R target | **HIGH** — dedicated, stop corrected | dedicated formula + structural stop |
| Kangaroo Tail | 0.5 (default) | 1.5 | universal 60-min swing layer | universal 60-min swing cap | **LOW** — defaulted | agreed defaults — **not** collision fallback |
| NR7 Breakout | 0.5 (default) | 2.0 | universal layer | universal layer | **LOW** — defaulted | agreed defaults — not fallback |
| ITR Breakout | 0.5 (default) | 1.5 | universal layer | universal layer | **LOW** — defaulted, no dedicated formula | agreed defaults — not fallback |
| ITR Fade | 0.5 (default) | 1.0 | universal layer | universal layer | **LOW** — defaulted (collision = philosophical inversion; must not use) | agreed defaults — not fallback |
| RSI Failure Swing | 0.5 (default) | 1.5 | universal layer | universal layer | **LOW** — defaulted | agreed defaults — not fallback |
| Stochastic Pop | 0.5 (default) | 1.0 | universal layer | universal layer | **LOW** — defaulted | agreed defaults — not fallback |

Every pattern additionally receives the universal tightening-only structural
stop-anchor / target-cap (60-min swing fractal) on top, so even defaulted patterns
get structural refinement.

**Notes:**
- **`stop_mult=0.5` for the 6 defaulted patterns is deliberate, not a guess** — it
  is the modal real stop multiplier across every pattern with a verified C++
  formula, and it explicitly avoids the **Finding 17** enum-collision fallback
  (which for these 6 produces semantically-wrong prices — worst case ITR Fade
  inheriting Holy Grail's trend-continuation logic). The Finding 17 fix routes
  `patternId` by its actual enum namespace and seeds from **this table**, never the
  blind cast.
- **Elder Breakout stop correction** is the one confirmed, actionable change: the
  real dedicated formula uses a 2-bar structural low/high, not `0.5×ATR`. Port the
  structural stop.
- **Elder Breakout target — RESOLVED 2026-07-14: locked at 1.5R.** The dedicated
  C++ formula's historical **2.0R** initial target is retired in favor of the
  scale-out-ladder-t1 **1.5R** (which `_LABEL_TARGET_PARAMS` already uses). This
  preserves parity with the `lbrnet` Phase 1 τ* calibration and is the correct
  target for a single-stage bracket that has no trailing mechanism to shield a
  late-stage reversal (e.g. ~1.9R back to the stop). **Live-code gap (Gemini CLI
  verified 2026-07-14)**: `PositionManagerPatterns.cpp:253,274` still hardcode
  `ELDER_TARGET_R_MULTIPLE = 2.0f` — the Phase 1 implementation must change both to
  `1.5f`.
- **Trailing-eligibility** (`ShouldUseTrailingStop`): only Elder Breakout and ITR
  Breakout are `true`. In **v1 (single-stage)** this is inert — both still get a
  finite upper barrier. The classification is retained as the **Phase 4**
  (multi-instrument) input for which patterns are Line-2 convexity candidates — the
  useful residue of Finding 12, captured here before the function is deleted.

### 5.4 Regime-invalidation kill-switch (the mid-hold mechanism) + entry-time physics

Barriers are **immutable once set at entry** (§4.1/§4.2). The engine is blind to
price-path fluctuations inside them — no trailing, no milestone tightening. Physics
enters in exactly **two disciplined places**, and *continuous mid-hold
barrier-width modulation is deliberately NOT one of them* (that is the retired
`vol_convexity` failure class — see the lesson below).

**(1) Entry-time physics — barrier construction + regime scale.** At entry the
bracket is built by the 4-step Adaptive Barrier Protocol (§5.6); regime enters
*once*, as a bounded stop-width scale (§4.1: regime scales the stop, target rides as
an R-multiple, R:R preserved). This is the only place regime touches barrier
*geometry*.

**(2) Mid-hold — the regime-invalidation kill-switch (the ONLY mid-hold action).**
An O(1) `.context` check on the tick loop. If the live physics violate
alpha-validity thresholds — the entry's statistical edge has demonstrably
evaporated — the position is terminated immediately (a `REGIME_INVALIDATION` market
exit, top priority in the §4.4 race). Trigger inputs (all native, already live):

| Signal | Source (live C++) | Kill trigger |
|---|---|---|
| **VPIN / Amihud toxicity** | `ctx.vpin` (raw — see parity note) | `> TOXICITY_THRESHOLD_MAX` → toxic flow, flat out |
| **Hurst / DFA persistence** | `ContextManager` (efficiency/persistence) | collapse from trend-fractal → high-entropy MR = edge gone |
| **Taleb kurtosis / cliff proximity** | `RiskManager` kurtosis-emergency state | fat-tail regime onset |
| **Shannon flow entropy** | `InformationEngine::GetShannonEntropy()` (normalize per Finding 18) | disorder spike |
| **HMM regime / MarketClimate** | `InferenceManager` (via `UpdateContext()` — §7 Finding 1) | regime flip against the position |

**Response is binary flat-to-cash at current scale.** At 1–2 contract sizing a
partial reduction is impossible, so the graduated ladder (tighten → reduce →
flatten) degenerates to binary anyway. Keep the *trigger* a crisp alpha-validity
threshold; the *response* is unconditional flat. Response-tiering by severity is
deferred to larger size / the multi-instrument era (§6 Phase 4).

**Why this, not continuous modulation:** in a **concentrated single-instrument
book** there is no diversification cushion for a non-diversifiable tail event, so a
regime-invalidation defense is institutionally correct (Claude/Gemini convergence,
2026-07-14) — but it is *point-in-time regime invalidation* driven by measured EV
decay, not a price-path trailing stop. Immutable barriers + a discrete kill also
make the `vol_convexity` coupled-triangle hazard **structurally impossible**
mid-hold (there is no continuous stop/target mutation to degenerate).

**Parity + raw-signal dependency.** The kill-switch must match the Python
`_context_kill_switch_triggered()` semantics exactly (co-evolution). Because VPIN
toxicity (and the other gate signals) reach the wire only as a scaled proxy
(Findings 19–21), the kill thresholds must be defined on the **raw** value — so the
kill-switch **inherits the raw-signal wire-serialization dependency** (§8 Q5), now
in-scope via schema evolution (directive #6). Fusion weights/thresholds follow
`LABELING_AND_AUGMENTATION_SPEC.md`'s TRAP policy — reliability-adjusted,
replay-calibrated, not hand-tuned.

**Co-evolution lesson (Python side, 2026-07-13) — now scoped to entry-time width
scaling only.** Python's `base_stop * (1.0 + vol_convexity)` assumed
`vol_convexity ∈ ~[-1,+1]`; it is actually z-scored ([-6,+6], std≈2.24), and ~20.4%
of records had `vol_convexity ≤ -0.99`, collapsing the stop to the tick floor while
the target held → a spurious `Ω_net=15.68`. Since mid-hold continuous modulation is
retired, this lesson now binds **only** the entry-time regime stop-width scale:
> 1. **Never assume a physics signal is a bounded ratio** — normalize by an
>    empirical scale (e.g. 95th percentile of `|signal|`) before use.
> 2. **Bound the entry stop-width scale to `[0.5×, 2.0×]`**; the target rides as an
>    R-multiple of the scaled stop so R:R cannot be broken.

### 5.5 Position sizing (unchanged mechanism, new input)

`RiskManager::CalculateSafePositionSize()` keeps its 2% Elder rule and full
multiplier stack; it only needs the **new stop distance** (lower-barrier distance)
as its `riskPerContract` input instead of the Chandelier-derived stop. No new
sizing logic. Confirm the multiplier chain (Findings 13/15/16) is fed correctly.

### 5.6 The 4-step Adaptive Barrier Protocol (entry-time barrier construction)

Both the C++ live engine and the Python labeler build the entry bracket by the
same ordered protocol (parity-critical — the golden-vector fixture asserts each
step). Order matters: structural refinement is always **last** so it can only
tighten, never fight the volatility/regime scale.

1. **Seed (per §5.3 tier, Reading B)** — `stop = stop_mult × ATR10` (DOF-scaled; or
   the dedicated 2-bar structural stop for Elder Breakout). Target: the dedicated
   N-bar structural extreme for HIGH patterns (Turtle Soup 4-bar, Momentum Pinball
   10-bar), a fixed 1.5R for Elder Breakout, or `target_r_mult × risk` for LOW
   patterns.
2. **Regime scale** — apply the single bounded `[0.5×, 2.0×]` regime stop-width
   scale (§4.1); the target re-derives as the R-multiple (R:R preserved).
3. **Structural join** — pull the confirmed Raschke/Elder swing fractal from the
   closed higher-timeframe Wave/Tide bar caches (`join_asof` parity with Python).
4. **Structural cap/anchor** — tighten the stop to the swing fractal if tighter,
   cap the target at the structural extreme if nearer (§4.1/§4.2, tightening-only).

The result is an **asymmetric, single-stage, immutable bracket** locked at entry.

---

## 6. Phased rollout with acceptance gates

Each phase is gated by replay calibration per `../../docs/BACKTESTING_FRAMEWORK.md`;
do not mark a phase production-ready until its gates are explicitly evaluated.

- **Phase 0 — Framing & parity fixtures. ✅ COMPLETE (2026-07-14).** This doc +
  the shared golden-vector fixture (`tests/fixtures/triple_barrier_golden_vectors.json`),
  green on **both** sides: Python `compute_barriers()` (7 cases via
  `test_triple_barrier_scanner.py`; the cross-check surfaced + fixed **PC-22**, the
  bar-anchored-stop parity bug) and the C++ pure core `tbe::ComputeBarriers()`
  (`include/TripleBarrierEngine.h`, 7/7 via `tests/cpp/test_triple_barrier_parity.cpp`).
  The barrier definition is provably consistent (§5.2). The Finding 17 seed-function
  fix and the Elder `2.0f→1.5f` change move to Phase 1 (they are live-code changes,
  not fixture artifacts).
- **Phase 1 — Static single-stage triple-barrier cutover.** Delete
  `ChandelierStopManager` + scale-out ladder. Implement `TripleBarrierExitManager`:
  the 4-step Adaptive Barrier Protocol (§5.6) producing an immutable bracket +
  regime-conditioned software vertical barrier. **Finding 1 wiring is a MANDATORY
  Phase 1 deliverable** (not deferred): the vertical barrier is regime-conditioned,
  so `UpdateContext()` must be live or every trade silently defaults to the 25-bar
  horizon — an immediate live/backtest divergence that would invalidate the parity
  fixture on day one. Gate: barrier parity with labeler; no regression in replay
  omega_net vs the pre-cutover baseline attributable to mechanism change.
  **Baseline validity prerequisite (RESOLVED, §8 Q6)**: both sides keep the
  daily-bias veto — `lbrnet` must resurrect its dead `apply_daily_bias_filter()`
  and wire it into the Phase 2 replica so entry policy matches, with a parity test
  asserting identical allow/veto on both sides.
- **Phase 2 — Regime-invalidation kill-switch.** Wire the O(1) `.context` check
  (§5.4) into the tick loop; binary flat-to-cash on alpha-validity threshold
  violation. Finding 1's live regime input is already in place from Phase 1; this
  phase adds the raw-signal wire fix (Findings 19–21, now in-scope) so thresholds
  key off raw values, matched to Python `_context_kill_switch_triggered()`. Gate:
  net-positive tail defense under replay with no churn regression.
- **Phase 3 — Entry-time physics refinement.** Calibrate the bounded regime
  stop-width scale (§5.6 step 2) and the per-pattern vertical-barrier caps (§4.3
  `min()` model, pattern caps down from ∞). Gate: each increment attributable and
  net-positive; mid-hold behavior unchanged (still immutable barriers + kill).
- **Phase 4 — Multi-instrument era (deferred).** Multi-line independent-barrier
  allocation (Line 1 Structural Harvest + Line 2 Convexity/Tail, each an
  uncompromised single-stage barrier, physics-gated at entry) and portfolio-vol
  rebalancing (mechanism B). Requires a concurrent-position `PositionManager`
  refactor (today's single `m_openTrade`) and resolution of the overlapping-label
  non-IID parity consequence. Not pursued until the system trades multiple
  instruments — the single-instrument book is better served by the Phase-2
  kill-switch as the interim realization of the same philosophy (Claude/Gemini
  ruling, 2026-07-14).

---

## 7. Interaction with existing correctness findings

- **Finding 7** (Chandelier short-stop asymmetry) — **mooted by deletion.**
- **Finding 12** (`ShouldUseTrailingStop` per-pattern gate) — its trail/no-trail
  classification is **absorbed** into the §5.3 per-pattern geometry table, then the
  function is deleted. The classification data is still useful; the mechanism isn't.
- **Finding 14** (scale-out ladder not modeled in Python) — **closed by removing
  the ladder** on both sides; single barrier definition end-to-end.
- **Finding 17** (enum-collision seeding wrong stop/target for 6/9 patterns) —
  **fixed as part of Phase 0/1** (user decision 2026-07-14, now unblocked since
  lbrnet Phase 2 shipped). The barriers must not be seeded by the buggy fallback;
  route `patternId` by its actual enum namespace or fail-closed.
- **Finding 1** (`UpdateContext()` never called) — **MANDATORY Phase 1 deliverable**
  (promoted 2026-07-14; formerly a Phase 2 prerequisite). The Phase 1
  regime-conditioned vertical barrier already reads `m_currentHMMState`/
  `m_currentClimate`; if `UpdateContext()` stays unwired, every trade defaults to
  the 25-bar horizon, producing immediate live/backtest divergence that invalidates
  the golden-vector parity fixture. The `InferenceManager` tick hook must be live
  when `TripleBarrierExitManager` ships. (It also serves the Phase 2 kill-switch.)

---

## 8. Open questions (to resolve as the spec evolves)

1. **Per-pattern parameter table** — **RESOLVED (§5.3 table).** `stop_mult` /
   `target_r_mult` / structural sources reconciled and agreed on both sides. Elder
   Breakout target locked at **1.5R** (2026-07-14; see §5.3 note).
2. **Physics fusion weights & normalization** — the §5.4 signal-to-action mapping
   is qualitative here; quantitative weights come from replay calibration under the
   TRAP-policy governance, not this doc. Hard constraints already established by
   co-evolution: normalize each z-scored signal before use, and bound every
   barrier-width multiplier to `[0.5×, 2.0×]` applied symmetrically to stop and
   target (see §5.4 co-evolution lesson).
3. **Kill-switch response** — **RESOLVED (§5.4).** Binary flatten-to-cash at current
   1–2 contract scale (partial reduction is impossible on 1 contract, so the
   graduated ladder degenerates to binary anyway). Response-tiering (severity →
   tighten vs reduce vs flatten) is deferred to larger size / the multi-instrument
   era. The *trigger* is a crisp alpha-validity threshold violation; the *response*
   is unconditional flat.
4. **Vertical-barrier clock** — **RESOLVED (§4.3).** Regime-conditioned (not
   per-pattern — no real per-pattern C++ mechanism existed), on the TS3 15-min bar
   clock, matching Python `_MAX_BARS_BY_REGIME`. Fixed at entry from the live
   regime (depends on Finding 1).
5. **Raw-signal wire serialization** — now **in-scope** (schema evolution approved
   2026-07-14, directive #6): add raw liquidity-fragility / Hill-α / Amihud (and the
   VPIN toxicity the kill-switch keys on) as fields on the unscaled
   `AsymmetryContext`-style channel of `mts_schema.fbs`. This is a **hard dependency
   of the Phase 2 kill-switch**, not just a physics-layer nicety — the kill
   thresholds (`TOXICITY_THRESHOLD_MAX` etc.) must be defined on the **raw** value,
   and the wire currently carries only a scaled proxy (Findings 19–21). Move both
   sides together (wire is the co-evolution contract).
6. **Daily-bias counter-trend veto — RESOLVED 2026-07-14: KEEP on both sides.**
   `Scoring::ApplyDailyBiasFilter()` (`Scoring.cpp:351-390`) fires unconditionally
   at every live order entry (automatic `PositionManager.cpp:1711-1719`, manual
   `:2588-2592`), hard-vetoing longs into `BEARISH_TREND_PERSISTENT` and shorts into
   `BULLISH_TREND_PERSISTENT` (plus a +15% size boost for bias-aligned
   mean-reversion). The redundancy hypothesis (that the model's learned `daily_bias`
   feature subsumes it) is **refuted on the merits**: a deterministic, higher-
   timeframe Raschke/Elder trend filter is an *unconditional systemic safety rail*
   that must exist independent of the model's high-dimensional feature associations
   — entering counter-trend intraday mean-reversions against a strongly persistent
   daily bias is a high-velocity tail risk this concentrated book cannot absorb.
   - **C++ action**: `ApplyDailyBiasFilter()` remains active — **not deleted**.
   - **Python action**: `lbrnet` must **resurrect** the dead
     `apply_daily_bias_filter()` (PC-21) and wire it back into the Phase 2
     training/backtest pipelines.
   - **Validation**: a co-evolution parity test asserts identical binary allow/veto
     for the same `(pattern, isLong, daily_bias)` on both sides — a Phase 0/1 gate
     item (baseline validity, §6 Phase 1).

---

## 9. Done checklist (per phase)

- Build succeeds via `./build_dll.sh`; schema regenerated first if `.fbs` touched.
- Barrier parity fixture green on both C++ and Python.
- Replay-calibration acceptance gates for the phase explicitly evaluated
  (met/unmet stated), per `BACKTESTING_FRAMEWORK.md`.
- Native exit behavior fully functional without Python.
- No Chandelier / scale-out dead code remains (`grep` clean).
- Doc-sync contract honored (README-AI / copilot-instructions / CLAUDE / GEMINI).
