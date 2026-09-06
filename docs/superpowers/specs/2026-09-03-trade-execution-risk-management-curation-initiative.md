# Trade Execution / Risk Management Curation — Institutional Methodology

**Status, opened 2026-09-03: seeded from prior closed audits + the currently-queued NEXT MAJOR
INITIATIVE. Not yet actively worked — this doc exists so that work has a living home the moment the
observation-vector/ContextManager thread closes out, per `CLAUDE.md`/`GEMINI.md`/`PRODUCTION_TRIAGE.md`'s
own STANDING PRIORITY callout (2026-09-03).**

## 0. Origin and mandate

Direct sibling of `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` (the
observation-vector ledger) — same methodology, same discipline, different layer. That initiative asked
"is each dimension feeding the HMM built on solid, literature-grounded, real-data-validated math?" This
one asks the equivalent question one layer downstream: **is the data `RiskManager`/`ExecutionGate`/
`PositionManager` actually gate on — `LocalRiskContext`/`RiskGateContext` — solid, non-redundant, and
not blind to signal that already exists elsewhere in the system?**

**Founding question (operator directive, 2026-09-03, recorded in `CLAUDE.md`'s own North Star section):**
the HMM exists specifically to detect fat tails/regime shifts. Does its own inferred output (`.regime`,
`ν_k`, state probabilities) feed back into the execution-layer risk context as a genuine "heads up" for
the Predator, or does `RiskManager`'s own hard-gate layer fire on `LocalRiskContext` fields that were
frozen before the HMM ever ran? This is the same "solidify the data a downstream layer actually
consumes" discipline just applied to the observation vector, one hop further downstream.

**No exact predecessor ledger existed for this layer** (checked 2026-09-03, not assumed) — the closest
prior work was `docs/ADR/gate_stack_stationarity_audit_findings.md` (CLOSED 2026-08-15, a similar
per-gate table but a point-in-time stationarity audit, not a living ledger; **fully subsumed by §3
below and removed 2026-09-04** — nothing in it wasn't already reproduced here) and
`docs/superpowers/specs/2026-08-15-risk-gate-context-cpp-coevolution.md` (Units A/B/C, **all three now
addressed as of 2026-09-04** — Unit B/C done 2026-08-15 (Unit C re-verified 2026-09-04, one deliberate
scope reduction: the lbrnet-side threshold-sync script was explicitly descoped, not missed), Unit A
(the `risk_gate_context` population gap) implemented 2026-09-04 but its live-replay verification step
was not performed this session — see that spec's own Unit A status marker). This doc is the new
living home going forward.

## 0a. Parked for future research (2026-09-05, not now — do not act on these without a fresh decision)

While scoping the ATR→bipower-variation replacement (§2 item 5,
`docs/superpowers/specs/2026-09-05-activity-clock-triple-barrier-reformulation-spec.md`), a much
larger reframing came up: is a distance-multiple stop/target the right FORM at all, vs. genuinely
separate institutional disciplines — (1) meta-labeling for position sizing (AFML Ch. 3.6, already a
direction this repo's own risk-gate stack partially reflects), (2) trend-scanning for winner exits
(López de Prado, *Machine Learning for Asset Managers* Ch. 5 — already ranked Rank-1 in this repo's
own 2026-07-15 Gemini-adjudicated ruling, never implemented), and (3) regime-conditioned optimal
stopping for loser invalidation (Bertram 2010's OU free-boundary approach for mean-reverting regimes
specifically — with a real, not-yet-verified connection to this system's own already-computed
`mean_rev_z` lag-1 autocorrelation as a possible θ/reversion-speed estimate). RL-learned exits were
considered and explicitly rejected already (same 2026-07-15 ruling — overfitting/parity risk on a
single-instrument, low-sample book), so not a candidate here.

**Explicitly parked, not decided against — just not now.** Real, substantive directions, worth
returning to, but each is its own real research project (OU parameter estimation alone has known
finite-sample bias issues), not a same-session task. Current priority: ship the bipower-variation/
MedRV ATR replacement (§2 item 5) first — a real, scoped, already-specced fix — before opening any of
these three up.

## 1. Status vocabulary

Same as the observation-vector ledger, for consistency: **IN** (settled, stays as-is) · **IN-WEAK**
(stays, weak, no better alternative identified) · **IN-PENDING-FIX** (stays, a decided implementation
change not yet done) · **PAUSED** (groundwork exists, explicitly do not proceed without new evidence) ·
**CANDIDATE** (proposed, not yet built) · **OPEN** (actively being investigated) · **BLOCKED** (real
work identified, blocked on something else finishing first).

## 2. The active thread: HMM signal blindness (founding question — ANSWERED 2026-09-04, see §3a for recommendations)

| # | Item | Status | Why | Last verified |
|---|---|---|---|---|
| 1 | `LocalRiskContext` vs. `PredatorContext`'s HMM visibility | **ANSWERED, 2026-09-04** | **Verdict: 7 of 8 hard gates are blind to real HMM regime output; only gate 1 (Amihud) genuinely sees it.** Traced every gate's actual data source (not just its name):<br>- **Gate 1 (Amihud, `RiskManager.cpp:854-868`): HMM-AWARE.** Calls `InferenceManager::Instance().HmmState()->Dof()` directly, tightens the veto threshold p90→p75 when `dof≤4.0` (fat-tail regime detected).<br>- **Gates 2-5 (Taleb cliff/Shannon entropy/Taleb kurtosis/spread stress, `RiskManager.cpp:869-892`): BLIND.** Read only raw `LocalRiskContext` fields, no `InferenceManager`/HMM reference anywhere in `EvaluateHardGates()`.<br>- **Gates 6-8 (`ExecutionGate.cpp:24-36`): BLIND, DESPITE MISLEADING NAMES (fixed 2026-09-04, §3a item 1).** `paretoTopStateRatio` (gate 6) sounded like HMM top-state occupancy but is actually `1/Hill-α` (`ComputeParetoTopStateRatioProxy()`, `PositionManager.cpp:63-69`) — a raw tail-index statistic. `shannonTenureBars` (gate 7) is `ctx.regimeDuration`, a raw bar-count from `MarketClimateIndicator`, not the Student-t HMM's own posterior-state persistence. `talebSignalSigma` (gate 8) is literally `ctx.talebKurtosis` again — the exact same raw feature gate 4 already checks, just aliased under a different name (confirmed NOT an accidental duplicate, §3a item 2).<br>**So the founding concern is confirmed, and worse than "not yet checked": most of this gate stack was never actually reading the HMM at all, and 2 of the 3 gate names that sound regime-aware were misleading (now renamed).** `PredatorContext.regime` is not read by ANY of the 8 gates. | 2026-09-04 |
| 2 | EVT/GPD "distance to the tail, closing how fast" execution signal | **CANDIDATE** | Discussed 2026-09-03 (`lbrnet/logs/rc_gemini.log` context around `CLAUDE_BRIEF_123`) as a concrete first deliverable for item 1 — reuse this codebase's own existing GPD-fitting machinery (currently only used to derive *static* `FeatureScaler` bounds) to produce a *live* signal: a reading's estimated exceedance probability against its own fitted tail, plus its short-term rate of change. Deliberately scoped to feed `RiskGateContext` directly, NOT the HMM's own observation vector — avoiding the `tail_index` redundancy trap already learned this session (a Hill-estimator dim was dropped from the observation vector as structurally redundant with the model's own ν_k; the same redundancy risk applies here if this signal were fed back INTO the model instead of to the execution layer). | 2026-09-03 (idea only, not designed) |
| 3 | "Tightness/cost" liquidity axis | **ANSWERED, 2026-09-04 (investigation only, not yet implemented)** | Classic market-microstructure liquidity taxonomy (Kyle 1985; Harris) has (at least) four distinct axes: **tightness** (round-trip bid-ask spread cost), depth, resiliency, immediacy. This system's current liquidity fields don't cleanly cover tightness as its own persistent signal: `amihud_illiquidity` is a price-impact-per-dollar-volume measure (closer to depth), and `spreadStress`/`liq_fragility` (`LocalRiskContext.h`) was reformulated 2026-09-02/03 into a median-based *elasticity ratio* (closer to resiliency) — NOT the raw current bid-ask spread cost.<br>**Traced all 3 places raw spread actually appears** (`grep`-verified, no others exist): `PositionManager::ManagePendingEntry()` (cancels a working entry order if spread widens past `GetSessionSpreadLimit()` while waiting for fill), `EvaluateAutomatic`'s admission gate (`allowedSpreadTicks`, tightened by `liqPenalty`/`spreadStress`/`raschkeBurst`), and manual-entry admission (`EvaluateManual`'s `MICROSTRUCTURE_SPREAD` check) — **all three are entry-side only**. `GetSessionSpreadLimit()` itself is a fixed time-of-day-bucketed constant (1.5/1.0/2.0 ticks), not stationarity-checked or percentile-based at all — the same weak pattern Amihud had before its own fix.<br>**New finding, not previously documented: exits have ZERO spread awareness.** Traced `PositionManager::ClosePositionAtMarket()` and `EmergencyFlattenPosition()` (the only two exit-execution functions) — both call `sc.FlattenAndCancelAllOrders()`/market orders unconditionally, with no spread check, no logging of exit-time spread cost, nothing. This matters because forced exits (Mahalanobis outlier emergency, hard-gate violations, trailing-stop hits) are exactly the moments market microstructure literature says liquidity is most likely to have also dried up (Brunnermeier & Pedersen 2009, funding/market liquidity spirals) — the entry-only gate protects the discretionary decision to enter, but offers zero visibility into the cost of a mandatory exit at the worst possible moment.<br>**Answer: tightness DOES deserve a persistent signal, but NOT a blocking gate on exits** — delaying or vetoing a hard-gate-triggered emergency flatten because the spread is wide would be actively dangerous (the position must get flat regardless of cost; wide spread is a symptom of the same regime stress causing the exit, not a reason to wait). The correct design is advisory/telemetry: a session-aware rolling-percentile signal of realized spread cost (mirroring Amihud's Layer B treatment) recorded at BOTH entry and exit — useful for post-hoc cost analysis and possibly informing execution style (limit vs. market) when time isn't critical, but must never gate a risk-driven exit. **Not yet implemented** — this is a design recommendation from investigation, not a shipped signal. | 2026-09-04 |
| 4 | `log_scale_expansion_ratio`-based volatility-expansion gate | **CANDIDATE** | Surfaced 2026-09-03 while re-assessing `log_scale_ratio`/`log_scale_expansion_ratio` for the observation vector (`docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md` row 4): the two dims are redundant with each other (0.8085 correlation, confirmed genuine via Spearman attenuation-correction, not noise) but the underlying construct — `log(short_BV/long_BV)`, a model-free realized-volatility-regime-change detector, distinct from `relative_range`'s volatility-*level* axis — is real and not represented anywhere in `RiskManager.cpp`/`PositionManager.cpp` today (checked: only ATR-level `elderChandelierATR` and range-level `relative_range` exist there). TS2/tactical (`log_scale_expansion_ratio`)'s faster-reacting window fits the profile of signals already used for real-time admission gating (same family as `liq_fragility`/`raschkeBurst` tightening `allowedSpreadTicks`). **Design constraint (Vision section, `PRODUCTION_TRIAGE.md`): must be built stationarity-checked/percentile-based from day one** (Amihud-gate precedent), not a raw fixed threshold — and is a natural first candidate to wire item 1's HMM-regime-awareness into (tighten further when the HMM, once retrained, already flags an elevated-risk regime), rather than being designed in isolation from that founding question. | 2026-09-03 (idea only, not designed) |
| 5 | **Wilder ATR as a systemic single point of failure** ("is ATR an Achilles heel?" — operator question, 2026-09-05) | **ANSWERED, 2026-09-05 (investigation only, not yet implemented)** | **Verdict: not exaggerated — ATR is the single most load-bearing, most pervasive scale unit in the entire execution/backtest stack, more foundational than any one observation-vector dim.** Full inventory of consumers, verified by direct code read, both repos: (a) **every stop distance, all 9 Raschke/Elder patterns** (`src/PositionManagerPatterns.cpp`: `TURTLE_SOUP_STOP_BUFFER`/`PINBALL_STOP_MULTIPLIER`/`COMPRESSION_STOP_MULTIPLIER`/`HOLY_GRAIL_STOP_MULTIPLIER`/`TWO_B_STOP_MULTIPLIER`/`DOUBLE_REPO_STOP_MULTIPLIER`/`REPO_FAILURE_STOP_MULTIPLIER`/`DEFAULT_STOP_MULTIPLIER`, all `N × ATR`, only ever *tightened*, never widened, by a structural cap); (b) **the Triple-Barrier exit engine's own stop seed** (`tbe::BarrierInputs.atr10`, `docs/ADR/triple_barrier_exit_engine_spec.md` — `stop_candidate = entry − dir·(stop_mult·atr)` computed for every entry before any structural cap applies); (c) **position sizing** (`RiskManager::GetAtrVolatilityMultiplier()`, `RiskManager.cpp:1901-1919`); (d) **the R-multiple basis of every P&L metric in both live and backtest** — `lbrnet/backtest/backtest_runner.py` states explicitly at multiple sites that `1R = stop_mult × ATR10`, so win-rate/Sharpe/omega/`F_0.25` deploy-gate grading are ALL denominated in this one indicator; (e) `ATRProximityEnum`/`EmaProximity` regime classification (already this doc's sibling `2026-09-04-indicator-gang-statistical-reformulation-initiative.md` §6.3, ranked #1-priority candidate independently); (f) Trade Grade scoring (Keltner Channels, `2.5-3.0× ATR` band); (g) VWAP distance normalization; (h) Elder Breakout distance-beyond-band classification. **Why this outranks any single observation-vector dim**: one contaminated ATR reading (a single outlier bar) propagates simultaneously into stop placement, position size, the exit engine's own barrier seed, AND the R-multiple label every subsequent performance metric for that trade is measured against — a genuine single point of failure, not a redundant-signal question. **The fragility itself is the same already-fixed class**: Wilder ATR is a first-order EMA, the identical "one outlier persists for its whole half-life" defect already repaired in `liq_fragility`/`burstiness_index`/`amihud_illiquidity` this session. **Only one partial mitigation exists, and it's narrow**: `TripleScreen1.cpp` applies a 20-bar rolling *median* (`Subgraph_ATRAvg`, code comment: "robust to fat-tail ATR spikes") — but that's only the *baseline denominator* for `RiskManager`'s volatility multiplier (d above). The raw `ATR(14)`/`ATR(10)` that actually sets stop width, target-cap distance, the Triple-Barrier engine's seed, and the R-multiple basis is **not** robustified at the source anywhere. **Spec drafted 2026-09-05**: `docs/superpowers/specs/2026-09-05-robust-atr-reformulation-spec.md` proposes swapping Wilder's EMA-of-True-Range for a robust Median-of-True-Range at the same call sites (same name/units/interface, no consumer changes), plus a real-data validation plan and an inventory of downstream thresholds needing re-derivation. Not yet implemented — spec only. **Risk framing revised per operator directive (2026-09-05): pre-production, so no live capital is at risk** — the "much higher-stakes touch" framing below (this row's original 2026-09-05 wording) was written assuming live-trading stakes; the spec itself now reflects the corrected, lower-stakes posture (validate on real data, then cut over directly, no dual-run shadow needed). **SUPERSEDED FOR THE TRIPLE-BARRIER SCOPE, 2026-09-05 (operator directive — decisive institutional replacement, not a robustified-in-place hedge)**: `docs/superpowers/specs/2026-09-05-activity-clock-triple-barrier-reformulation-spec.md` now governs items (a)-(d) above (all 9 patterns' stop widths, the Triple-Barrier engine's own barrier seed, position sizing, and the R-multiple basis) — ATR is removed from that path OUTRIGHT, replaced by activity-clock-native jump-robust realized volatility (bipower variation/MedRV), not a robustified ATR variant. The robust-ATR spec's remaining scope is items (e)-(h) only (the 4 peripheral, non-barrier consumers). | 2026-09-05 |

## 3. Seed material: the 8 gates already audited (originally from a 2026-08-15 ADR, now merged and removed — nothing lost)

Stationarity verdicts below reproduce that closed 2026-08-15 audit in full (`docs/ADR/amihud_gate_percentile_spec.md`
§6's own stationarity test re-run against every fixed-threshold comparison in
`RiskManager::EvaluateHardGates()`/`EvaluateEmpiricalRegimeGates()`) — that audit asked "is each gate's
raw signal stationary enough for a fixed threshold, or does it need Amihud-style rolling-percentile
treatment." **Important scope note**: that audit did NOT ask item 1's question above (HMM-signal
visibility) — it's a different axis entirely (stationarity vs. regime-awareness), so a gate being
"stationary, no treatment needed" there says nothing about whether it's also blind to HMM regime state.
**Conclusion of that original audit, preserved verbatim in spirit**: no gate in either function needed
Amihud-style rolling-percentile treatment; the only real defect it surfaced was `taleb_signal_sigma_
threshold`'s value-drift bug (compiled default `1.8382` vs. live JSON `1.8401`, both intended to be the
same 2026-08-13 P85.0 rescale) — fixed by syncing the compiled default to `1.8401`
(`RiskManager.cpp:74`, `docs/superpowers/plans/2026-08-15-risk-gate-context-cpp-coevolution.md`'s Unit B
Task 2). lbrnet-side stale copies of that same threshold (`backtest_runner.py`'s hardcoded `9.636797`,
`HMMEmpiricalGateThresholds.json`'s `6.67559116507085`) remain an open cross-repo follow-up, not fixed
from this repo.

| # | Gate | Location | Stationarity (2026-08-15) | HMM-signal-aware? (ANSWERED 2026-09-04) |
|---|---|---|---|---|
| 1 | Amihud illiquidity veto | `RiskManager.cpp:854-868` | Already fixed (session-aware rolling percentile) | **YES** — reads `InferenceManager::Instance().HmmState()->Dof()` directly |
| 2 | Taleb cliff / Elder Chandelier | `RiskManager.cpp:869-874` | Stationary, no treatment needed | **NO** — `LocalRiskContext` only |
| 3 | Shannon entropy halt | `RiskManager.cpp:875-880` | Stationary, no treatment needed | **NO** — `LocalRiskContext` only |
| 4 | Taleb (Moors) kurtosis halt | `RiskManager.cpp:881-886` | Stationary, no treatment needed | **NO** — `LocalRiskContext` only |
| 5 | Spread stress (`liq_fragility`) | `RiskManager.cpp:887-892` | Stationary, no treatment needed | **NO** — `LocalRiskContext` only. **Note: `liq_fragility`'s own formula was reformulated 2026-09-03 (`a76ec00`) — this gate's `>0.85f`/`>0.70f` thresholds were deliberately preserved unchanged (same bounded [0,1] output shape), but the underlying signal's real-data distribution shifted (old: compressed 0.0-0.35 "normal" band; new: uses the full [0,1] range, p1=0.175/median=0.502/p99=0.855). The absolute thresholds surviving the reformulation was a design goal, not yet independently re-verified against the new distribution's own percentiles post-fix.** |
| 6 | Pareto top-state ratio (Hill-α proxy) | `ExecutionGate.cpp:24-26` | Stationary (naming debt noted, deferred) | **NO, WAS MISLEADINGLY NAMED** — actually `1/Hill-α` (`PositionManager.cpp:63-69`), not an HMM state-occupancy probability despite the name. **RENAMED 2026-09-04**: `paretoTopStateRatio`/`paretoTopStateRatioMax` → `hillTailIndexProxy`/`hillTailIndexProxyMax`; `ComputeParetoTopStateRatioProxy()` → `ComputeHillTailIndexProxy()` (see §3a item 1) |
| 7 | Shannon regime tenure | `ExecutionGate.cpp:29-31` | Stationary, no treatment needed | **NO** — raw `MarketClimateIndicator` bar-count duration, not the Student-t HMM's own posterior-state persistence |
| 8 | Taleb-signal-sigma | `ExecutionGate.cpp:34-36` | Stationary; value-drift bug fixed (Task 2, that plan) | **NO** — literally `ctx.talebKurtosis` again, aliased under a different name (same raw feature as gate 4). **RENAMED 2026-09-04**: `talebSignalSigma`/`talebSignalSigmaThreshold` → `talebKurtosisEntryGate`/`talebKurtosisEntryGateThreshold`. **Confirmed NOT a duplicate of gate 4** — different scope (entry-only vs. continuous halt) and different threshold source (see §3a item 2) |

## 3a. Recommendations (2026-09-04, following from §2 item 1's answer)

**Framing, in the operator's own words (2026-09-04)**: a predator with regime-aware eyes but a
regime-blind nervous system isn't fully a predator — it's half of one. `PredatorContext.regime` and
the entry-fusion layer (e.g. `TripleScreen3.cpp`'s Turtle Soup Option A applicability mask,
`ComputeApplicabilityMask(predatorCtx.inPosition, predatorCtx.regime)`) already condition *whether to
pounce* on the HMM's regime. The risk/safety layer governing *how carefully* to pounce mostly doesn't
— it's not merely an unfinished feature, it's an internal inconsistency: the same regime assessment
drives one decision but not the other.

Concrete, prioritized next steps (not yet built, not yet decided as final — a proposal from this
session's findings, for review before implementation):

1. **DONE, 2026-09-04: fixed the two misleading gate names.** Verified first that no wire schema
   field or JSON-config key actually needed to change (`schema/mts_schema.fbs`'s `RiskGateContext`
   table has no `pareto_top_state_ratio`/`taleb_signal_sigma` field at all — those were always local
   C++-only derivations computed from the raw wire fields `pareto_tail_alpha`/`taleb_kurtosis`; the
   JSON keys `pareto_top_state_ratio_max`/`taleb_signal_sigma_threshold` in
   `hmm_regime_risk_policy.json` are the only cross-repo-coupled names, and those were left
   untouched since lbrnet's own calibration/consumption scripts read them directly). So the entire
   fix was contained to this repo's C++: `ExecutionGate::GateContext.paretoTopStateRatio` →
   `hillTailIndexProxy`, `.paretoTopStateRatioMax` → `hillTailIndexProxyMax`,
   `.talebSignalSigma` → `talebKurtosisEntryGate`, `.talebSignalSigmaThreshold` →
   `talebKurtosisEntryGateThreshold`; `PositionManager.cpp`'s `ComputeParetoTopStateRatioProxy()` →
   `ComputeHillTailIndexProxy()`; `RiskManager::GetParetoTopStateRatioMax()` →
   `GetHillTailIndexProxyMax()`, `GetTalebSignalSigmaThreshold()` →
   `GetTalebKurtosisEntryGateThreshold()`. `ReasonCode::HmmRegimeGateParetoBreach`/
   `HmmRegimeGateTalebBreach` left unchanged (numeric values 30/32 stable; "Pareto"/"Taleb" alone
   are accurate family labels, only "TopStateRatio"/"SignalSigma" were the misleading part).
   `./build_dll.sh --no-clean` clean.
2. **DONE, 2026-09-04: gate 8 is NOT a duplicate of gate 4 — settled, intentional two-tier design.**
   Traced `RiskManager::EvaluateHardGates()`'s only caller path: `PositionManager::EnforceHardGates()`
   (`PositionManagerPatterns.cpp:20`) runs continuously regardless of position state and can cancel
   working orders or **flatten an existing position**. `ExecutionGate::EvaluateEmpiricalRegimeGates()`
   (gate 8) only ever runs inside `EvaluateAutomatic()`/`EvaluateManual()`, i.e. only blocks **new
   entry admission** — it never touches an existing position. The two gates check the same raw
   feature (`ctx.talebKurtosis`) but at genuinely different scope (broad continuous safety halt vs.
   narrow entry-admission filter) with independently-calibrated thresholds from different config
   sources (`talebKurtosisHaltThreshold`, compiled `ExecutionParams` default 2.0064, vs.
   `talebKurtosisEntryGateThreshold`, lbrnet-calibrated `hmm_regime_risk_policy.json` value 1.8401)
   — legitimate defense-in-depth, not an accidental copy-paste. The only real defect was that this
   intent was never documented; cross-reference comments were added at both gate sites
   (`RiskManager.cpp`'s kurtosis hard gate, `ExecutionGate.cpp`'s `HmmRegimeGateTalebBreach` check)
   so a future reader doesn't have to re-derive this.
3. **Extend gate 1's own proven pattern (`InferenceManager::Instance().HmmState()->Dof()`) to gates
   2-5** rather than inventing a new mechanism — it already exists, is already live, and gates 2-5
   (Taleb cliff, Shannon entropy, Taleb kurtosis, spread stress) are semantically exactly the kind of
   signal that should tighten when the HMM's own `dof`/regime indicates fragility, the same way gate 1
   already does for Amihud. This is the most direct, lowest-novelty way to close the "half a predator"
   gap. **Not yet started.**
4. **Item 2's duplication question (above) is now resolved as "not a duplicate"** — so item 3's
   regime-wiring work for gates 2-5 is NOT preempted by anything in gate 8; both remain independently
   worth doing.
5. **Item 2's EVT/GPD execution signal (§2 above) is a good complementary, not competing, deliverable**
   — it answers "how close to the tail and closing how fast" as a live signal; item 3 above answers
   "does the HMM's own posterior agree this is fragile." Both are legitimate, different questions worth
   feeding `RiskGateContext` together, not a choice between them.

## 4. Other cached background material (reference, not yet triaged into this ledger)

- **`docs/ADR/execution_correctness_findings_spec.md`** — 12 verified correctness/parity findings
  across `PositionManager`/`RiskManager`/`ChandelierStopManager`/`Scoring`/`ExecutionGate` (2026-07-10
  audit). Finding 1 (`UpdateContext()` never called) RESOLVED; Finding 12 is a Python-port parity gap,
  not a C++ fix. Not yet cross-referenced against this ledger's own items — do that before assuming
  either doc is complete on its own.
- **`docs/superpowers/specs/2026-08-15-risk-gate-context-cpp-coevolution.md`** — Units A (close the
  `risk_gate_context` population gap), B (gate-stack stationarity audit, →§3 above), C (shared,
  git-tracked calibration config for `FeatureScaler`'s winsorization bounds). Unit B fully done; Units A
  and C's actual completion status needs re-verification (the spec's own plans show detailed
  implementation steps, but this session hasn't confirmed they were executed and committed) before this
  ledger treats them as closed.
- **`docs/ADR/amihud_gate_percentile_spec.md`** — the original precedent both this doc and the
  observation-vector ledger's methodology descend from (canonical formula fix + session-aware rolling
  percentile + honest rename, `vpin`→`amihud_illiquidity`). Fully implemented C++-side; `lbrnet`-side
  co-evolution (§4b of that spec) was still open as of that doc's own last update — cross-repo, not
  fixed from here.
- **`../../docs/RISK_MANAGEMENT_SYSTEM.md`** / **`../../docs/TRADE_EXECUTION_SYSTEM.md`** — the
  canonical architecture/governance references for this whole layer (not living ledgers, static
  documentation) — read these for how the gates in §3 fit into the broader daily-loss-limit/Kelly-sizing/
  trade-lifecycle picture before making any change here.

## 5. Immediate next action

Once the observation-vector/`ContextManager` thread closes out: trace item 1 in §2 (does any existing
`RiskManager`/`ExecutionGate` hard gate already see HMM regime state, or are all 8 gates in §3 blind to
it) before designing item 2's EVT/GPD signal — confirming the actual gap empirically, not assuming it,
matches this initiative's own founding discipline.
