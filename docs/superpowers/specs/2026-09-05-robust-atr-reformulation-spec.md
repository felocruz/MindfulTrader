# Robust (Gang-Grounded) ATR Reformulation — Spec

**Status: SPEC — design proposed, NOT implemented. Opened 2026-09-05, following directly from
`docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md` §2 item 5
("Wilder ATR as a systemic single point of failure") and its cross-reference from
`docs/superpowers/specs/2026-09-04-indicator-gang-statistical-reformulation-initiative.md` §6.3.**

**Risk framing (operator directive, 2026-09-05): this system is not in production yet — no live
capital is at risk, so the stakes here are lower than the "changes the executable risk contract"
framing in the item-5 finding implied.** That finding's caution was calibrated for a live-trading
system; absent live capital, the correct posture is this repo's own standing rule
(`CLAUDE.md`: "the default is deletion/replacement, not preservation" pre-production) — validate on
real data, then cut over directly, without a parallel-shadow-run safety net. Empirical validation is
still required (this repo's own standing discipline — literature grounding is necessary, not
sufficient, on its own), just not for capital-preservation reasons.

## 0. Origin and mandate

`docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md` §2 item 5
found that Wilder's ATR (a first-order EMA of True Range) is the single most load-bearing, most
pervasive scale unit in this system — not just `ATRProximityEnum`/`EmaProximity` (already flagged as
a candidate fix in the sibling indicator-formula initiative), but the stop-width seed for all 9
Raschke/Elder patterns, the Triple-Barrier exit engine's own barrier seed, `RiskManager`'s
position-sizing multiplier, and the R-multiple denominator every live/backtest P&L metric is measured
against. It carries the exact same fragility class already fixed this session for
`liq_fragility`/`burstiness_index`/`amihud_illiquidity`: a mean-based (EMA) estimator where a single
outlier bar contaminates every subsequent reading for the smoother's whole half-life.

This spec proposes the concrete fix and its validation plan.

## 1. Problem statement (baseline, as of 2026-09-05)

```
TR_t  = max(High_t - Low_t, |High_t - Close_{t-1}|, |Low_t - Close_{t-1}|)
ATR_t = Wilder_EMA(TR, N)   -- N=14 (TS1, src/TripleScreen1.cpp) or N=10 (TS3, src/TripleScreen3.cpp)
```

Wilder's smoothing is `ATR_t = ATR_{t-1} + (TR_t - ATR_{t-1})/N` — algebraically an EMA with
`alpha = 1/N`. Like any EMA, a single extreme `TR_t` (a gap, a fat-tail print) is folded in once and
then decays out over the smoother's own half-life (`~N` bars) — every stop/target/size/R-multiple
computed during that decay window is contaminated by a bar that has already passed.

**One partial mitigation already exists, and it is narrow**: `TripleScreen1.cpp` applies a 20-bar
rolling *median* (`Subgraph_ATRAvg`, `sc.MovingMedian(Subgraph_ATR, Subgraph_ATRAvg, 20)`, code
comment: "robust to fat-tail ATR spikes") — but this only smooths the *baseline denominator* consumed
by `RiskManager::GetAtrVolatilityMultiplier()`. The raw `ATR(14)`/`ATR(10)` itself — the value that
actually sets stop width, target-cap distance, the Triple-Barrier engine's seed, and the R-multiple
basis — is not robustified anywhere at its source.

## 2. Proposed design

**Do not invent a new construct. Swap the smoothing operator inside the existing ATR computation,
keep everything else (name, units, call sites, interface) unchanged** — the same surgical pattern
already used this session for kurtosis (Moors octile replacing moment kurtosis) and skewness (Bowley
quartile replacing moment skewness): a robust order-statistic estimator standing in for a fragile
mean-based one, same slot, same consumers, no call-site changes required.

```
TR_t     = unchanged (True Range definition doesn't change)
RATR_t   = Median_N(TR)     -- N=14 or N=10, same windows as today, per call site
```

**Why plain median, not a median/MAD construction (unlike `liq_fragility`)**: `liq_fragility`
needed MAD because it measures *dispersion* of a ratio around a central tendency. ATR's whole job is
to *be* a central-tendency estimate of typical bar range — Wilder's EMA already served exactly that
role (a smoothed average). Median-of-TR over the same window is the direct robust substitute for
that same role, not an additional layer. No new machinery, no new hyperparameter beyond the window
length already in use.

**Citation**: median-based True Range smoothing is a well-precedented robust-statistics engineering
pattern (Rousseeuw & Croux 1993, median/order-statistics as robust alternatives to mean-based
estimators — the same grounding already used in this repo for `liq_fragility`), applied here to an
existing, decades-old technical-analysis indicator (Wilder 1978) rather than to a novel construct.
This is honestly a thinner literature domain than Hill/GPD/Hurst — there is no equivalent of a named
"Median True Range" canonical paper found — but the underlying robust-statistics justification for
preferring an order statistic over an EMA under fat tails is the same one already accepted elsewhere
in this codebase (Kim & White 2004's critique of moment-based estimators generalizes to any
EMA/mean-based smoother, not just kurtosis/skewness specifically).

**Consequence of the swap**: `TripleScreen1.cpp`'s existing 20-bar `MovingMedian` smoothing of
`Subgraph_ATRAvg` becomes redundant once the base `ATR` itself is already a robust median — that
downstream double-smoothing can likely be simplified/removed once the base swap is validated (not
before; keep both during validation so the comparison tool below can isolate which stage is doing
the work).

## 3. Consumers requiring re-validation (inventory, from the item-5 finding)

Every one of these reads the raw `ATR(14)`/`ATR(10)` value directly and must be checked against real
historical data before cutover, not assumed compatible:

1. **Stop-width seeds, all 9 patterns** (`src/PositionManagerPatterns.cpp`): `TURTLE_SOUP_STOP_BUFFER`,
   `PINBALL_STOP_MULTIPLIER`, `COMPRESSION_STOP_MULTIPLIER`, `HOLY_GRAIL_STOP_MULTIPLIER`,
   `TWO_B_STOP_MULTIPLIER`, `DOUBLE_REPO_STOP_MULTIPLIER`, `REPO_FAILURE_STOP_MULTIPLIER`,
   `DEFAULT_STOP_MULTIPLIER`.
2. **Triple-Barrier exit engine's own barrier seed** (`tbe::BarrierInputs.atr10`).
3. **Position sizing** (`RiskManager::GetAtrVolatilityMultiplier()`, `RiskManager.cpp:1901-1919`).
4. **R-multiple basis of every backtest metric** (`lbrnet/backtest/backtest_runner.py`: `1R = stop_mult
   × ATR10`).
5. `ATRProximityEnum`/`EmaProximity` regime classification (the narrower candidate already tracked in
   the sibling indicator-formula initiative, §6.3).
6. Trade Grade scoring (Keltner Channels, `2.5-3.0× ATR` band).
7. VWAP distance normalization (`VwapIndicator::GetDistanceNorm()`).
8. Elder Breakout distance-beyond-band classification (`DetectElderBreakout`, `breakoutDistance =
   (close - upperBand) / atr`).

**Co-evolution note**: per this repo's own governing rule (neither runtime is authoritative;
ground it in the literature, then move both sides together), this must ship identically in C++ and
in `lbrnet/backtest`'s Python replica — ATR isn't computed independently on each side today and must
not become two divergent formulas.

## 4. Validation plan (empirical, required before cutover)

Same family and discipline as `tools/observation_vector/*` — build a standalone comparison tool
(`tools/observation_vector/atr_robustness_comparison.cpp`, `tools/bin/`, `ToolProgressLogger`-routed
per this repo's own standing directive) over the full real MES tick/bar dataset
(`lbrnet/data/raw/mes_ticks.parquet`, resampled to the relevant bar timeframes), computing both
`Wilder_EMA(TR,N)` and `Median_N(TR)` side by side and reporting:

1. **Divergence magnitude**: distribution of `|RATR - ATR| / ATR` across the full history — how often
   and how far the two estimators disagree, not just whether they disagree at all.
2. **Contamination-window behavior**: pick real historical outlier-TR bars (e.g. gap events, halt
   reopens) and show both estimators' trajectories for the following `~N` bars — does the median
   estimator actually avoid the "persists for the whole half-life" artifact in practice, on this
   system's own real data, not just in theory.
3. **R-multiple basis shift**: for a sample of historical patterns (using the existing 7 golden-vector
   fixtures already used for Triple-Barrier parity, plus a broader real-replay sample), recompute
   `stop_mult × RATR` and compare against `stop_mult × ATR` — quantify how much the R-multiple
   denominator (and therefore every backtest metric downstream of it) would shift.
4. **Downstream threshold re-derivation check**: confirm whether any already-calibrated
   ATR-denominated threshold elsewhere in the system (e.g. `elderChandelierATR`/Taleb-cliff gate,
   `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`'s Chandelier-Exit row) would
   need re-percentile-matching the same way `liq_fragility`'s consumers were re-matched after its own
   reformulation — do not assume "no", check explicitly.

**Decision rule**: if divergence is small and the contamination-window behavior confirms the
expected robustness benefit with no material R-multiple distortion, cut over directly (no dual-run
shadow period needed, per the pre-production risk framing above). If R-multiple shift is large,
re-derive/re-percentile-match the affected downstream thresholds in the same commit, not as a
follow-up.

## 5. Open questions (not yet decided)

1. **Window length parity**: TS1 uses `ATR(14)`, TS3 uses `ATR(10)` — should the median replacement
   use the same two window lengths as today, or is this a natural point to reconsider whether 10/14
   are themselves well-grounded (no citation found in situ for either, same "picked once, never
   re-derived" pattern already flagged for other thresholds this session)? Recommend: keep the
   existing windows for this spec's scope (isolate the estimator-robustness variable only); a
   separate window-length study is a distinct question, out of scope here.
2. **Downstream double-smoothing removal**: does `TripleScreen1.cpp`'s 20-bar `MovingMedian` on top
   of the new robust base ATR become fully redundant, or does it still add value (e.g. further
   damping residual median noise)? Decide empirically during validation, not by assumption.
3. **Scope of "ATR" being replaced**: this spec covers the two live production call sites
   (`Subgraph_ATR` in `TripleScreen1.cpp`, `Subgraph_AtrTemp3` in `TripleScreen3.cpp`). Confirm no
   other independent ATR computation exists elsewhere in the codebase before calling the swap
   complete (a repo-wide grep for `sc.ATR(` / `MOVAVGTYPE_WILDERS` should be run as part of
   implementation, not assumed from this spec's own inventory).

## 6. Explicitly out of scope

- Replacing ATR's *conceptual* role entirely with an activity-clock/bipower-variation-based
  volatility measure (already tracked separately as
  `docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md` §2 item
  4, `log_scale_expansion_ratio`) — that is a different question (a new, additional signal) from this
  spec's narrower one (robustify the existing indicator in place).
- Any change to stop/target *multiplier* values (`stop_mult`, `target_r_mult` per pattern) — this
  spec only touches the ATR estimator itself, not the per-pattern constants applied to it.

## 7. References

- Wilder, J.W. (1978), *New Concepts in Technical Trading Systems* — original ATR definition.
- Rousseeuw, P.J. & Croux, C. (1993), "Alternatives to the Median Absolute Deviation," *JASA* —
  already this repo's standing citation for median/MAD-based robust scale estimators
  (`liq_fragility`, and the ATR-as-scale-reference candidate in the sibling indicator initiative).
- Kim, T.-H. & White, H. (2004), "On more robust estimation of skewness and kurtosis," *Journal of
  Financial Econometrics* — already this repo's standing citation for why mean/moment-based
  estimators are least reliable exactly under the fat-tailed conditions they exist to measure;
  generalizes here from kurtosis/skewness to any EMA-based smoother, including ATR.
- `docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md` §2 item
  5 — the founding finding this spec implements.
- `docs/superpowers/specs/2026-09-04-indicator-gang-statistical-reformulation-initiative.md` §6.3 —
  the narrower `ATRProximityEnum`/`EmaProximity` candidate this spec supersedes in scope.
