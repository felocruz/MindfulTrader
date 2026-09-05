# Robust (Gang-Grounded) ATR Reformulation — Spec

**Status: SPEC v2 — design proposed, NOT implemented. Opened 2026-09-05, following directly from
`docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md` §2 item 5
("Wilder ATR as a systemic single point of failure") and its cross-reference from
`docs/superpowers/specs/2026-09-04-indicator-gang-statistical-reformulation-initiative.md` §6.3.
v1's plain-median design was sent to an independent Gemini instance for critique
(`lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_134`/`_REPLY`); v2 (§2-§3 below) supersedes it with a
recursive Huber-in-log-space filter + Kaufman Efficiency-Ratio-adaptive learning rate, following
further independent literature search beyond what Gemini's own reply cited.**

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

## 2. Independent critique received (CLAUDE_BRIEF_134/134_REPLY, 2026-09-05) — the plain-median
design below is SUPERSEDED, not just refined

The original v1 design in this spec (plain `Median_N(TR)`, unchanged from §0's mandate at the time)
was sent as a self-contained brief to an independent, no-repo-access Gemini instance. The reply
raised three substantive objections, each checked here rather than accepted at face value (this
repo's own standing discipline toward AI-generated claims — verify before trusting):

1. **Skew/location bias — CONFIRMED, real.** True Range is a strictly positive, right-skewed
   quantity (Gamma/log-normal-like), so `Median(TR) < E[TR]` structurally — a plain median
   systematically understates the range Wilder's EMA (a mean-like estimator) used to report,
   which would silently tighten every downstream stop distance unless corrected. **Gemini's own
   proposed fix (a theoretical `c = E[TR]/Median(TR)` correction constant) is NOT adopted as-is —
   see §3's simpler resolution below.** Gemini's specific numeric claim ("15% to 30%" understatement)
   is an unverified illustrative figure, not derived from this system's real data — treat as a
   hypothesis to check empirically (§5), not a settled number.
2. **Discontinuity on window exit — CONFIRMED, and independently found to be a structural argument
   against ANY fixed-window order-statistic estimator, not just plain median.** A rolling median
   (or a rolling trimmed mean, or Rousseeuw & Croux's own `Sn`/`Qn` robust scale estimators) all
   share the same defect: a value's influence disappears abruptly the instant it exits the window,
   producing a level jump uncorrelated with any new information arriving that bar. This is a
   decisive, general argument for an estimator with NO fixed window at all — see §3.
3. **Volatility-expansion lag — CONFIRMED as a real tradeoff, but Gemini's proposed fix (a hard
   HMM-DOF-threshold state gate switching between two fixed learning rates) is itself an ad hoc,
   newly-invented discontinuity requiring its own tuning/validation.** A better-grounded fix already
   exists inside this exact codebase — see §3's Kaufman-ER-adaptive-rate design.

**Also independently challenged (not raised by the operator, found during this pass): Gemini's own
comparison table claims the Huber M-estimator has "Scale Bias: None (Unbiased)." This is an
overstatement.** Huber M-estimators of location are consistent/unbiased for *symmetric* error
distributions; True Range is right-skewed, so a Huber-robustified center will ALSO sit below `E[TR]`
(less severely than a plain median, but not zero) — the same skew-bias concern in item 1 applies,
softer in degree, not eliminated by switching estimator family alone. Flagged here so it isn't
silently inherited from Gemini's table into an implementation.

## 3. Revised proposed design (v2, 2026-09-05, incorporating the critique + independent literature)

**Two independent upgrades over the v1 plain-median design, each separately grounded, combined as a
single synthesis (not itself a single named published technique — labeled honestly as a synthesis,
same convention already used elsewhere in this repo for the `FeatureScaler` shrinkage-floor spec):**

### 3.1 Replace the fixed window with a recursive, Huber-robustified exponential filter

```
TR_t        = unchanged (True Range definition doesn't change)
logTR_t     = log(TR_t)                                  -- see 3.1a for why
e_t         = (logTR_t - RATR_t-1) / scale_t              -- standardized innovation
psi_k(e)    = max(-k, min(k, e)),  k = 1.345              -- Huber's clipping constant
RATR_t      = RATR_t-1 + alpha_t * psi_k(e_t) * scale_t   -- alpha_t from §3.2
ATR_robust_t = exp(RATR_t)                                -- back-transform
```

This has **no fixed window at all**, structurally eliminating the discontinuity-on-exit defect
(§2 item 2) that afflicts a plain median, a trimmed mean, or any other order-statistic computed over
a sliding window — a value's influence decays smoothly (geometrically) rather than vanishing the
instant it exits an arbitrary cutoff. `scale_t` is a running robust dispersion estimate (e.g. an
exponentially-weighted MAD of recent innovations); `k=1.345` is Huber's own standard tuning constant
(95% Gaussian efficiency).

**Citations**: Huber, P.J. (1964), "Robust Estimation of a Location Parameter," *Annals of
Mathematical Statistics* — the clipping function itself. **More directly on point than Gemini's own
citation list**: Gelper, S., Fried, R. & Croux, C. (2010), "Robust Forecasting with Exponential and
Holt-Winters Smoothing," *Journal of Forecasting* 29(3):285-300 — this is the actual named technique
Gemini's recursive formula reinvents without citing: a Huber-type bounded-influence recursive update
replacing plain exponential smoothing, from the same Croux robust-statistics lineage already cited
in this repo for `liq_fragility` (Rousseeuw & Croux 1993). Also relevant: Muler, N. & Yohai, V.J.
(2008), "Robust estimates for GARCH models," *Journal of Statistical Planning and Inference*
138(10):2918-2940 — the equivalent recursive-bounded-influence idea applied to volatility filtering
specifically, in a financial-econometrics setting.

#### 3.1a Why compute the filter in log-space, not raw True-Range space

True Range's right-skew means a linear-space Huber filter still inherits some of the same skew-bias
Gemini correctly flagged for the median (§2 item 1, extended above to Huber). **Working in
log(True Range) instead is independently well-precedented in financial econometrics for exactly this
reason**: Andersen, T.G., Bollerslev, T., Diebold, F.X. & Labys, P. (2001), "The Distribution of
Realized Exchange Rate Volatility," *Journal of the American Statistical Association* 96(453):42-55,
and its equity-market companion Andersen, Bollerslev, Diebold & Ebens (2001), "The Distribution of
Realized Stock Return Volatility," *Journal of Financial Economics* 61(1):43-76 — both show
log(realized volatility) is close to Gaussian even when raw realized volatility is strongly
right-skewed. Applying the Huber filter to `log(TR)` rather than raw `TR` puts the estimator on the
scale where its symmetric-distribution assumption is closest to true, materially reducing (not
necessarily eliminating) the residual skew-bias flagged above — **this is a genuine additional
finding from independent literature search, not something Gemini's own reply raised.**

### 3.2 Replace Gemini's ad hoc HMM-DOF state-gate with Kaufman's Efficiency-Ratio-adaptive rate
(already implemented in this exact codebase)

Gemini's own fix for volatility-expansion lag (§2 item 3) was a hard threshold ("if HMM DOF<4, switch
to a fast learning rate") — itself a new, untuned discontinuity. **A materially better-grounded
alternative already exists, fully implemented, in this repo**: `ContextManager::efficiency`
(`src/StudyHelperFunctions.cpp:98`'s `EfficiencyRatioCalculator`) computes Kaufman's Efficiency Ratio
— already confirmed, in this repo's own prior literature-grounding pass
(`docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`), as "most precisely Kaufman's
Efficiency Ratio (Perry Kaufman, *Trading Systems and Methods*, 1998 — the same ER used in Kaufman's
Adaptive Moving Average)." Reuse it directly, via Kaufman's own exact, decades-precedented
smoothing-constant formula (Kaufman, P. (1995), *Smarter Trading*, and Kaufman (1998) as above):

```
SC_fast = 2/(2+1),  SC_slow = 2/(N+1)                       -- N = the same window already in use
alpha_t = [ ER_t * (SC_fast - SC_slow) + SC_slow ]^2        -- Kaufman's KAMA formula, continuous
```

This gives a **continuous** (not step-function) adaptive learning rate — fast during genuinely
directional/efficient price action (`ER_t` near 1), slow during choppy/inefficient noise (`ER_t` near
0) — reusing an indicator this system already computes and already has a confirmed literature
attribution for, rather than introducing a new HMM-DOF threshold requiring its own separate
validation. This directly answers Gemini's own item 4 concern (fast reaction to genuine volatility
regime shifts) without inventing a new mechanism.

### 3.3 Recalibration: use this repo's own established percentile-matching method, not a synthetic
bias-correction constant

Gemini's proposed `c = E[TR]/Median(TR)` correction constant is **not adopted** — it's an extra,
separately-estimated piece of machinery solving a problem this repo already has a proven, simpler
answer for. Every consumer of ATR (§4 below) uses it multiplicatively (`stop_mult × ATR`,
`2.5-3.0× ATR` Keltner width, `breakoutDistance = distance/atr`, etc.) — so any level-shift the new
estimator introduces is fully absorbed by re-fitting each consumer's own constant against the new
estimator on real historical data, using the exact same empirical percentile-matching methodology
already used this session for the Moors-kurtosis/Bowley-skewness/`burstiness_index` reformulations
(Task 7's own precedent: re-derive downstream thresholds directly from real-data percentiles, not a
theoretical correction formula). One fewer synthetic constant to estimate and maintain, and it
reuses a methodology already proven in this exact codebase rather than importing a new one. This
also reflects a hard lesson already paid for this session (the `amihud_illiquidity` `ζ_u`
subsampling-correction bug): prefer an empirically-fit, real-data-derived constant over a
theoretical distributional-assumption formula when the two could disagree.

## 4. Consumers requiring re-validation (inventory, from the item-5 finding)

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

## 5. Validation plan (empirical, required before cutover; revised for the v2 design)

Same family and discipline as `tools/observation_vector/*` — build a standalone comparison tool
(`tools/observation_vector/atr_robustness_comparison.cpp`, `tools/bin/`, `ToolProgressLogger`-routed
per this repo's own standing directive) over the full real MES tick/bar dataset
(`lbrnet/data/raw/mes_ticks.parquet`, resampled to the relevant bar timeframes), computing
`Wilder_EMA(TR,N)` alongside the v2 Huber-in-log-space/Kaufman-adaptive filter (§3) and reporting:

1. **R-Scale Distortion Ratio** (Gemini's own proposed diagnostic, adopted): `S_R = ATR_v2 / ATR_wilder`
   computed at every historical trade-entry timestamp — report its full distribution (not just its
   mean), since a mean near 1.0 could still hide a fat-tailed distortion at exactly the volatility
   extremes where it matters most.
2. **Skew-bias magnitude, empirically** (resolving §2 item 1's unverified "15-30%" figure): directly
   measure `E[TR] - center_estimate(TR)` for both the plain-median baseline and the v2 Huber-in-log
   estimator, on real data — confirm whether log-space Huber materially reduces the bias relative to
   plain linear-space median, as §3.1a's literature grounding predicts, or not.
3. **Contamination-window / regime-lag behavior**: pick real historical outlier-TR bars (gap events,
   halt reopens) AND real historical genuine volatility-expansion episodes (sustained regime shifts,
   not one-off spikes) and trace the v2 filter's trajectory through both — confirm it suppresses the
   former (Huber clipping) while tracking the latter faster than Wilder's fixed-alpha EMA (Kaufman-ER
   adaptive rate), not just one or the other.
4. **R-multiple basis shift and consumer re-fit**: for the existing 7 golden-vector Triple-Barrier
   fixtures plus a broader real-replay sample, recompute every consumer in §4's inventory under the
   v2 estimator and re-percentile-match each one's own constant/threshold directly (per §3.3 — no
   synthetic correction factor, empirical re-fit only).

**Decision rule**: if the v2 filter's contamination-window and regime-lag behavior both confirm the
expected benefits (robust to single-bar noise, no slower than Wilder EMA at tracking genuine
expansions) and every consumer's constant has been re-fit against it, cut over directly (no dual-run
shadow period needed for capital-preservation reasons, per the pre-production risk framing above —
Gemini's own recommendation for a parallel-run period is worth doing anyway if it surfaces regime
coverage a single validation pass might miss, but is not required by capital risk).

## 6. Open questions (not yet decided)

1. **Window length parity**: TS1 uses `ATR(14)`, TS3 uses `ATR(10)` — for the v2 filter, these map to
   `SC_slow` in Kaufman's formula (§3.2). Same recommendation as v1: keep the existing effective
   windows for this spec's scope; a separate window-length study is a distinct question, out of
   scope here.
2. **Downstream double-smoothing removal**: does `TripleScreen1.cpp`'s 20-bar `MovingMedian` on top
   of the new robust base ATR become fully redundant, or does it still add value? Decide empirically
   during validation, not by assumption — unchanged from v1.
3. **Scope of "ATR" being replaced**: unchanged from v1 — confirm no other independent ATR
   computation exists elsewhere in the codebase (`sc.ATR(` / `MOVAVGTYPE_WILDERS` grep) before calling
   the swap complete.
4. **`scale_t`'s own estimator (new, v2)**: §3.1's Huber filter needs a running robust dispersion
   estimate to standardize innovations before clipping — not yet specified precisely (a natural
   candidate is an EWMA of `|logTR_t - RATR_t-1|`, i.e. an exponentially-weighted MAD-like
   quantity, but the exact recursion needs to be pinned down and validated, not assumed).
5. **Honesty check on the v2 synthesis itself**: §3's combination (Huber-in-log-space + Kaufman-ER-
   adaptive rate) is this spec's own synthesis of three independently well-grounded techniques, not
   itself a single published, directly-on-point paper — flagged explicitly so this isn't
   mis-read as more literature-canonical than it is (same honesty convention already used elsewhere
   in this repo, e.g. the `FeatureScaler` shrinkage-floor spec's own "principled synthesis, not a
   direct implementation" framing).

## 7. Explicitly out of scope

- Replacing ATR's *conceptual* role entirely with an activity-clock/bipower-variation-based
  volatility measure (already tracked separately as
  `docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md` §2 item
  4, `log_scale_expansion_ratio`) — that is a different question (a new, additional signal) from this
  spec's narrower one (robustify the existing indicator in place).
- Any change to stop/target *multiplier* values (`stop_mult`, `target_r_mult` per pattern) — this
  spec only touches the ATR estimator itself, not the per-pattern constants applied to it.

## 8. References

- Wilder, J.W. (1978), *New Concepts in Technical Trading Systems* — original ATR definition.
- Rousseeuw, P.J. & Croux, C. (1993), "Alternatives to the Median Absolute Deviation," *JASA* —
  already this repo's standing citation for median/MAD-based robust scale estimators
  (`liq_fragility`, and the ATR-as-scale-reference candidate in the sibling indicator initiative).
- Kim, T.-H. & White, H. (2004), "On more robust estimation of skewness and kurtosis," *Journal of
  Financial Econometrics* — already this repo's standing citation for why mean/moment-based
  estimators are least reliable exactly under the fat-tailed conditions they exist to measure;
  generalizes here from kurtosis/skewness to any EMA-based smoother, including ATR.
- Huber, P.J. (1964), "Robust Estimation of a Location Parameter," *Annals of Mathematical
  Statistics* 35(1):73-101 — the bounded-influence clipping function underlying the v2 filter (§3.1).
- Gelper, S., Fried, R. & Croux, C. (2010), "Robust Forecasting with Exponential and Holt-Winters
  Smoothing," *Journal of Forecasting* 29(3):285-300 — the actual named technique (recursive
  Huber-robustified exponential smoothing) Gemini's own reply reinvented without citing; found via
  independent literature search, not present in Gemini's response.
- Muler, N. & Yohai, V.J. (2008), "Robust estimates for GARCH models," *Journal of Statistical
  Planning and Inference* 138(10):2918-2940 — recursive bounded-influence volatility filtering in a
  financial-econometrics-specific setting, supporting citation for §3.1.
- Andersen, T.G., Bollerslev, T., Diebold, F.X. & Labys, P. (2001), "The Distribution of Realized
  Exchange Rate Volatility," *JASA* 96(453):42-55, and Andersen, Bollerslev, Diebold & Ebens (2001),
  "The Distribution of Realized Stock Return Volatility," *Journal of Financial Economics* 61(1):
  43-76 — near-Gaussianity of log-realized-volatility, the grounding for computing the v2 filter in
  log-space rather than linear True-Range space (§3.1a); found via independent literature search, not
  present in Gemini's response.
- Kaufman, P. (1995), *Smarter Trading*, and Kaufman, P. (1998), *Trading Systems and Methods* — the
  Efficiency-Ratio-adaptive smoothing-constant formula (Kaufman's Adaptive Moving Average), reused
  directly for §3.2's adaptive learning rate; already confirmed as the correct attribution for this
  repo's own existing `ContextManager::efficiency` indicator in a prior literature-grounding pass
  (`docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md`).
- `docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md` §2 item
  5 — the founding finding this spec implements.
- `docs/superpowers/specs/2026-09-04-indicator-gang-statistical-reformulation-initiative.md` §6.3 —
  the narrower `ATRProximityEnum`/`EmaProximity` candidate this spec supersedes in scope.
- `lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_134`/`CLAUDE_BRIEF_134_REPLY` — the independent critique
  this v2 design responds to (§2).
