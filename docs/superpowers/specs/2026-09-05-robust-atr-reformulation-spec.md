# Robust (Gang-Grounded) ATR Reformulation — Spec

**Status: SPEC v3 — design proposed, NOT implemented. Opened 2026-09-05, following directly from
`docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md` §2 item 5
("Wilder ATR as a systemic single point of failure") and its cross-reference from
`docs/superpowers/specs/2026-09-04-indicator-gang-statistical-reformulation-initiative.md` §6.3.
v1's plain-median design was critiqued by an independent Gemini instance (`CLAUDE_BRIEF_134`/`_REPLY`),
producing v2 (Huber-in-log-space + Kaufman ER-adaptive rate). v2 was itself sent back for review
(`CLAUDE_BRIEF_135`/`_REPLY`) and one real design flaw was found in it — Kaufman's Efficiency Ratio
measures directional persistence, not volatility/scale, and gating ATR's own reactivity on it is a
category error (the same class of mistake already caught once before in this repo's Impulse System
redesign work, "Inertia = Hurst"). v3 (§2a, §3) replaces it with a scale-innovation-driven trigger,
adds a Jensen's-inequality correction (with an explicit target-quantity decision flagged, not silently
resolved), and validates all prior citations. §3.4 adds Data-Oriented Design / hot-path performance
requirements (operator directive, 2026-09-05) — the filter runs on every live tick, so its O(1),
zero-allocation state and bar-close gating are load-bearing design constraints, not an afterthought.**

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

## 2a. Second-round critique received (CLAUDE_BRIEF_135/135_REPLY, 2026-09-05) — one real flaw found
in v2, everything else confirmed

v2 (§3 as it stood) was sent back with two explicit pushbacks (against Gemini's correction constant
and his HMM-DOF gate) and two independently-sourced additions (log-space transform, named citations
for the recursive filter), asking Gemini to check whether the pushbacks were justified. Verdicts:

1. **Pushback on the `c = E[TR]/Median(TR)` correction constant — Gemini agrees, empirical
   re-fitting is correct.** His own reasoning strengthens §3.3's case further: `ATR_wilder /
   ATR_robust` isn't even a constant ratio across regimes (Wilder's EMA explodes under fat-tailed
   spikes while a robust estimator stays grounded, so the ratio itself varies non-linearly) — a
   single scalar correction would have been wrong on its own terms, not just an unnecessary extra
   moving part. §3.3 stands, strengthened.
2. **Pushback on Kaufman ER-adaptive rate — REJECTED. Gemini found a real, fatal category error in
   my own v2 design, not a stylistic preference.** Kaufman's Efficiency Ratio measures *directional
   path efficiency* (a fractal/persistence-like axis: `ER→1` = straight-line movement, `ER→0` =
   choppy/non-directional). Volatility EXPANSION during a genuine regime shift (a crash, a
   liquidity-break panic) is frequently accompanied by exactly the kind of high-entropy, two-sided,
   whipsaw price action that drives `ER→0` — under v2's formula this would drive `alpha_t` toward
   its SLOWEST value at precisely the moment the filter most needs to react fast. **This is
   confirmed as the same category-error class already caught once before in this exact repo**: the
   Impulse System redesign work (`docs/superpowers/specs/2026-09-04-indicator-gang-statistical-
   reformulation-initiative.md` §5.2) already established that Hurst/persistence-type measures and
   volatility/scale-type measures are different statistical axes that must not be conflated ("Inertia
   = Hurst is a category error") — Kaufman's ER is architecturally the same kind of
   persistence/path-efficiency axis as Hurst, so gating a scale/dispersion filter's reactivity on it
   repeats the identical mistake in a new location. **§3.2 below is fully replaced, not patched.**
   (Caveat, not fully conceded either way: Gemini's framing treats this as a universal failure mode;
   in fact a strongly one-directional crash — most bars moving the same way — would keep `ER`
   relatively high even while volatility is expanding, so the failure mode is specifically two-sided/
   whipsaw volatility expansions, not literally every volatility expansion. This doesn't change the
   verdict — the category error is real and ER is still the wrong axis to gate on regardless — but
   the failure isn't quite as universal as stated.)
3. **Huber-unbiased-under-skew overstatement — confirmed as an overstatement, as I flagged.**
4. **Log-space transform — confirmed sound, with one addition: Jensen's inequality.** Exponentiating
   a log-space location estimate back to linear units recovers the GEOMETRIC center, not `E[TR]`
   (`E[TR] = E[exp(logTR)] > exp(E[logTR])` for any non-degenerate distribution, by Jensen's
   inequality for the convex `exp()` function) — a real, correct mathematical point, addressed in §3
   below with an explicit decision (not a silent fix), since **this does NOT automatically mean the
   correction must be applied**: it depends on which quantity the design is actually targeting (see
   §3.1a's revision).
5. **Citations verified as real and on-point**: Gemini independently confirmed Gelper, Fried & Croux
   (2010) and Muler & Yohai (2008) as accurate, non-hallucinated, correctly-matched citations for the
   recursive Huber-EMA construction — both now treated as settled rather than provisional.

## 3. Revised proposed design (v3, 2026-09-05, incorporating both rounds of critique + independent literature)

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

**Jensen's-inequality correction — an explicit design decision, not a silent bug fix (round-2
finding, CLAUDE_BRIEF_135_REPLY).** Exponentiating a robust LOCATION estimate of `log(TR)` back to
linear units gives that location's exact monotonic image (e.g. exponentiating a median-of-log-TR
gives exactly the median of TR — no correction needed, true for any distribution). But if the design
goal is to reconstruct something closer to Wilder's original *mean-like* ATR level (which is pulled
upward by the right tail, arguably a deliberately conservative property for a stop-width reference —
under-estimating tail-driven risk is the worse failure mode for a stop distance), a further
log-normal-style correction is needed: `ATR_robust = exp(mu_hat + 0.5 * sigma_hat^2)` (the standard
lognormal-moment formula, valid under an approximate log-normality assumption for `TR`). **Decision:
apply this correction** — preserving Wilder's original conservative (mean-pulled-by-tail) semantics
rather than silently shifting to a more aggressive median-like target, keeping §3.3's empirical
re-fit focused on removing single-outlier contamination rather than absorbing a deliberate,
unacknowledged change in what "typical range" means for stop-sizing purposes.

### 3.2 Trigger fast-tracking from the filter's OWN scale innovations, not Kaufman's Efficiency Ratio
(Kaufman ER REJECTED, category error — see §2a item 2)

**v2's Kaufman-ER-adaptive rate is rejected outright** — Kaufman's Efficiency Ratio is a directional-
persistence measure, the same statistical axis as Hurst, and gating a volatility/scale filter's
reactivity on it repeats the exact "Inertia = Hurst" category error this repo already corrected once
in the Impulse System redesign (§2a item 2). The replacement must be driven by the filter's own scale
innovations directly, not an external persistence/direction proxy.

Gemini's own round-2 fix (`CLAUDE_BRIEF_135_REPLY`) proposes exactly this — switch to a fast learning
rate when the standardized innovation `e_t` exceeds the Huber clipping threshold `k` for **two
consecutive bars** (distinguishing a sustained regime shift from a single-bar spike, which alone
never moves the slow rate) — but the specific "two consecutive" rule is an arbitrary, uncited
threshold. **This spec adopts the underlying idea but grounds the trigger rule properly, reusing a
theoretical framework already adopted elsewhere in this exact repo for the identical class of
problem** (distinguish a genuine persistent shift from noise, react as fast as possible without
whipsawing): `CLAUDE.md`'s own TRAP-detection doctrine already cites Wald's Sequential Probability
Ratio Test (1945) and the Shiryaev quickest-detection/disorder problem for exactly this
"react-the-instant-there's-real-evidence, not before" question. The directly-applicable, purpose-built
tool from that same theoretical lineage is **CUSUM** (Page, E.S. (1954), "Continuous Inspection
Schemes," *Biometrika* 41(1/2):100-115) — a running cumulative-sum statistic on the standardized
innovations that crosses a decision threshold once accumulated evidence of a sustained shift exceeds
what a single noisy bar could produce, rather than an arbitrary fixed "N consecutive bars" count:

```
S_t     = max(0, S_t-1 + e_t - referenceOffset)     -- one-sided CUSUM on standardized innovations
alpha_t = alpha_fast   if S_t > cusumThreshold        (sustained expansion detected)
        = alpha_slow   otherwise
```

This is internally consistent with a theoretical framework this repo has already adopted for a
conceptually identical detection problem, rather than importing Kaufman's KAMA formula (wrong axis)
or keeping Gemini's own ad hoc "two consecutive bars" rule (arbitrary, untuned). `referenceOffset`
and `cusumThreshold` are open parameters requiring the same empirical validation as everything else
in §5 — not assumed correct from the literature alone.

### 3.3 Recalibration: use this repo's own established percentile-matching method, not a synthetic
bias-correction constant (unchanged from v2 — confirmed correct in round 2, §2a item 1)

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

### 3.4 Hot-path performance (Data-Oriented Design) — operator directive, 2026-09-05

This filter runs inside the live ACSIL update path (`TripleScreen1.cpp`/`TripleScreen3.cpp`, called
on every tick per `AutoLoop=1`) — this repo's own standing hot-path rule (`CLAUDE.md`: "no heap
allocations in recurring ACSIL update paths," all three screens are equally hot) applies in full.
The v3 design is not just statistically preferable to any fixed-window order-statistic approach
(§2a item 2's window-exit-discontinuity argument) — **it is also the objectively better engineering
choice on pure performance/memory grounds, for the same underlying reason**:

1. **O(1) state, zero window buffer, zero allocation — a genuine DOD win over the REJECTED v1
   design, not just a statistical one.** A fixed-window rolling median (v1's original proposal)
   would have required either a maintained `N`-length ring buffer of recent True Range values plus a
   full `O(N log N)` re-sort every bar, or a specialized streaming-quantile structure (t-digest, GK01,
   etc. — this repo's own `RobustMoments.h`/Moors-kurtosis work already benchmarked and rejected
   t-digest as unnecessary machinery at this system's window sizes, `16d8f07`). The v3 recursive
   filter needs **none of that**: per-instance state is exactly three floats (`mu_hat`, `sigma_hat`,
   `cusum_S`), updated in place, no buffer, no resort, no dynamic memory at all. Two instances exist
   in this system (TS1 `ATR(14)`, TS3 `ATR(10)`) — the total footprint is ~24 bytes, trivially
   cache-resident alongside whatever other per-screen hot state already exists.
2. **Gate the recursive update to bar-close, not every tick — the single highest-leverage
   performance decision here, more important than any micro-optimization of the arithmetic itself.**
   True Range's own definition (`max(High-Low, |High-PrevClose|, |Low-PrevClose|)`) is well-defined
   using the CURRENT bar's High/Low even while that bar is still forming, which is why Sierra
   Chart's built-in `sc.ATR()` recomputes continuously intrabar. The v3 filter should NOT mirror that
   — it should reuse the exact bar-close-gating idiom this repo already established for its other
   rolling-window statistics (`UpdateObservationVectorSubgraphs()`'s own guard,
   `StudyHelperFunctions.cpp:2908-2911`: `if (!sc.IsFullRecalculation && lastObsUpdateIndex ==
   sc.Index) return;` — explicitly documented in that same function as intentional, since only
   `micro_asymmetry` needs every-tick reactivity, "unlike the rest of this function's outputs").
   Every consumer in §4 reads ATR at a discrete decision point (entry time; the Triple-Barrier
   engine's barriers are immutable once set, per this repo's own governing rule) or on a per-bar
   cadence (Trade Grade, `ATRProximityEnum`) — none of them need an intrabar-forming-bar preview of
   this specific filter. Gating to bar-close eliminates the `log()`/`exp()`/Huber-clip/CUSUM
   arithmetic firing dozens-to-hundreds of times per bar (once per tick) down to exactly once per bar
   close — at that cadence (15/60/240-minute bars) the actual compute cost is immaterial regardless
   of how it's implemented; the real risk this section exists to head off is accidentally wiring the
   update to fire per-tick and paying transcendental-function cost needlessly on every single tick,
   all session long, for no consumer that actually needs it.
3. **Write the output into the existing packed-array (SoA) path, not a new virtual `Indicator<T>`
   subclass.** Per this repo's own stated architectural direction (`docs/superpowers/specs/
   2026-08-04-indicator-manager-dod-soa-design.md`, `CLAUDE.md`'s "permanent hybrid architecture"
   note), the packed arrays (`IndicatorLayout.h`/`IndicatorPackedState.h`) are the canonical,
   devirtualized read path for every hot-path consumer (`CheckTrigger`, `PopulateIndicatorState`,
   `GetTrainingEventT`, `EventSerializer`) — plain `IndicatorKey`-enum-indexed array lookup, never a
   virtual call or a string/map hash. The robust-ATR filter's OUTPUT value should be written there
   directly, matching every other hot-path-read indicator; its three-float internal recursive state
   is write-side-only bookkeeping (mirrors the same split already established for every other
   `Indicator<T>`-derived leaf class: `IndicatorStore` computes, the packed array is read).
4. **Branchless/minimal-branch arithmetic throughout — no new control-flow surprises.** The Huber
   clip (`psi_k`) is a two-comparison `std::clamp`; the CUSUM update is a single `std::max(0, ...)`;
   neither introduces virtual dispatch, exceptions, or unpredictable branching into the hot path.
5. **Reuse this repo's own established NaN/degenerate-input guard pattern, proactively — a real bug
   class this exact codebase has already been bitten by twice.** A recursive filter with no window
   to "reset" from is more exposed to a single degenerate tick (e.g. a zero-range or non-finite `TR`)
   permanently poisoning `mu_hat`/`sigma_hat` going forward, unlike a windowed estimator where a bad
   value eventually ages out. Guard every update with `std::isfinite()` and carry forward the last
   valid state on failure — the exact pattern `fast_hurst_exponent` already uses (a static-local
   carry-forward guard), and the exact fix already required once this session for the activity-clock
   skewness/kurtosis call site that lacked it (`tools/RECALIBRATION_LEDGER.md`'s 2026-09-04 finding:
   a missing `std::isnan` guard let a degenerate window silently write NaN into a live risk-gate
   input). Do not repeat that omission here — build the guard in from the start, not as a
   post-incident fix.

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

## 5. Validation plan (empirical, required before cutover; revised for the v3 design)

Same family and discipline as `tools/observation_vector/*` — build a standalone comparison tool
(`tools/observation_vector/atr_robustness_comparison.cpp`, `tools/bin/`, `ToolProgressLogger`-routed
per this repo's own standing directive) over the full real MES tick/bar dataset
(`lbrnet/data/raw/mes_ticks.parquet`, resampled to the relevant bar timeframes), computing
`Wilder_EMA(TR,N)` alongside the v3 Huber-in-log-space/CUSUM-adaptive filter (§3) and reporting:

1. **R-Scale Distortion Ratio** (Gemini's own proposed diagnostic, adopted): `S_R = ATR_v3 / ATR_wilder`
   computed at every historical trade-entry timestamp — report its full distribution (not just its
   mean), since a mean near 1.0 could still hide a fat-tailed distortion at exactly the volatility
   extremes where it matters most.
2. **Skew-bias magnitude, empirically, WITH and WITHOUT the Jensen correction** (resolving §2 item 1's
   unverified "15-30%" figure, and §3.1a's decision to apply the correction): directly measure
   `E[TR] - center_estimate(TR)` for (a) the plain-median baseline, (b) linear-space Huber, (c)
   log-space Huber without Jensen correction, and (d) log-space Huber WITH the `+0.5*sigma^2`
   correction — confirm (d) closes the gap to `E[TR]` materially better than (a)-(c), as §3.1a's
   literature grounding predicts, or not.
3. **Contamination-window / regime-lag behavior**: pick real historical outlier-TR bars (gap events,
   halt reopens) AND real historical genuine volatility-expansion episodes (sustained regime shifts,
   not one-off spikes — including two-sided/whipsaw expansions specifically, per §2a item 2's
   caveat) and trace the v3 filter's trajectory through both — confirm the CUSUM trigger (§3.2)
   suppresses the former while tracking the latter faster than Wilder's fixed-alpha EMA, including
   the whipsaw case that would have defeated the rejected Kaufman-ER approach.
4. **CUSUM parameter sensitivity**: sweep `referenceOffset`/`cusumThreshold` (§3.2) against the same
   real historical episodes from item 3 — these are literature-grounded in form (Page 1954) but not
   yet validated in magnitude for this specific instrument/timeframe.
5. **R-multiple basis shift and consumer re-fit**: for the existing 7 golden-vector Triple-Barrier
   fixtures plus a broader real-replay sample, recompute every consumer in §4's inventory under the
   v3 estimator and re-percentile-match each one's own constant/threshold directly (per §3.3 — no
   synthetic correction factor, empirical re-fit only).

**Decision rule**: if the v3 filter's contamination-window and regime-lag behavior both confirm the
expected benefits (robust to single-bar noise, no slower than Wilder EMA at tracking genuine
expansions, including whipsaw ones) and every consumer's constant has been re-fit against it, cut
over directly (no dual-run shadow period needed for capital-preservation reasons, per the
pre-production risk framing above — Gemini's own recommendation for a parallel-run period is worth
doing anyway if it surfaces regime coverage a single validation pass might miss, but is not required
by capital risk).

## 6. Open questions (not yet decided)

1. **Window length parity**: TS1 uses `ATR(14)`, TS3 uses `ATR(10)` — for the v3 filter, these map to
   `SC_slow`/the recursive filter's own slow rate. Same recommendation as v1/v2: keep the existing
   effective windows for this spec's scope; a separate window-length study is a distinct question,
   out of scope here.
2. **Downstream double-smoothing removal**: does `TripleScreen1.cpp`'s 20-bar `MovingMedian` on top
   of the new robust base ATR become fully redundant, or does it still add value? Decide empirically
   during validation, not by assumption — unchanged from v1/v2.
3. **Scope of "ATR" being replaced**: unchanged from v1/v2 — confirm no other independent ATR
   computation exists elsewhere in the codebase (`sc.ATR(` / `MOVAVGTYPE_WILDERS` grep) before calling
   the swap complete.
4. **`scale_t`'s own estimator — RESOLVED, 2026-09-05 (CLAUDE_BRIEF_135_REPLY)**: Gemini's round-2
   reply specified a concrete recursive scale-update reusing this repo's own standing `1.4826` MAD-
   consistency constant (already used for `liq_fragility` and elsewhere): `sigma_hat_t = sigma_hat_t-1
   + alpha_slow * (1.4826 * |x_t - mu_hat_t-1| - sigma_hat_t-1)`. Adopted as specified — internally
   consistent with this repo's own existing convention, no further design work needed here, only
   empirical validation (§5).
5. **Honesty check on the v3 synthesis itself**: §3's combination (Huber-in-log-space + Jensen
   correction + CUSUM-triggered adaptive rate) is this spec's own synthesis of four independently
   well-grounded techniques (Huber 1964; Gelper/Fried/Croux 2010; Andersen-Bollerslev-Diebold-Labys/
   Ebens 2001; Page 1954), not itself a single published, directly-on-point paper — flagged
   explicitly so this isn't mis-read as more literature-canonical than it is (same honesty convention
   already used elsewhere in this repo, e.g. the `FeatureScaler` shrinkage-floor spec's own
   "principled synthesis, not a direct implementation" framing).

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
- Kaufman, P. (1995), *Smarter Trading*, and Kaufman, P. (1998), *Trading Systems and Methods* —
  the Efficiency-Ratio-adaptive smoothing-constant formula (Kaufman's Adaptive Moving Average).
  **Considered and REJECTED for §3.2** (round 2, `CLAUDE_BRIEF_135_REPLY`) — Kaufman's ER measures
  directional persistence, not volatility/scale, and gating ATR's own reactivity on it is a category
  error (the same class already caught in this repo's Impulse System redesign, "Inertia = Hurst").
  Kept here for the historical record and because `ContextManager::efficiency` remains a correctly-
  attributed existing indicator in this repo for its own original purpose — just not the right input
  for this filter.
- Page, E.S. (1954), "Continuous Inspection Schemes," *Biometrika* 41(1/2):100-115 — CUSUM, the
  scale-innovation-driven trigger replacing the rejected Kaufman-ER approach (§3.2); chosen for
  internal consistency with the sequential/quickest-detection framework (Wald 1945 SPRT; Shiryaev
  disorder problem) this repo's own TRAP-detection doctrine (`CLAUDE.md`) already uses for the
  identical class of problem (distinguish a genuine persistent shift from noise, react as fast as
  possible without whipsawing).
- `docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md` §2 item
  5 — the founding finding this spec implements.
- `docs/superpowers/specs/2026-09-04-indicator-gang-statistical-reformulation-initiative.md` §6.3 —
  the narrower `ATRProximityEnum`/`EmaProximity` candidate this spec supersedes in scope.
- `lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_134`/`CLAUDE_BRIEF_134_REPLY` — the first-round
  independent critique that superseded v1 (§2).
- `lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_135`/`CLAUDE_BRIEF_135_REPLY` — the second-round review
  that found the Kaufman-ER category error, confirmed the citations, and specified the `scale_t`
  recursion and Jensen's-inequality correction (§2a).
