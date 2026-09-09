# Imbalance Triple Screen — Architecture Spec

**Status, opened 2026-09-06: the foundational architecture doc for this repo's broader
time-clock → activity/imbalance-clock migration, referenced by (not duplicating) the Force Index
Track 1/2 specs, the activity-clock observation-vector doc, and the risk-gating/labeling twin
docs. Nothing implemented — this is the infrastructure inventory those specs' eventual
implementations will sit on top of. Deliberately broad, expect this to grow as more pieces are
identified.**

## 0. Mandate

### 0a. FOUNDATIONAL RISK, surfaced 2026-09-06 — read before trusting any comparison plan in this doc

**`BackTesterStudy.cpp` has never actually been run, ever** (operator's own words) — despite being
fully coded (Phase 1/2/3, `.btst` binary output, `RunSummary`, walk-forward inputs, the
`IN_MODEL_ARTIFACT_PATH`/`IN_TRANSFORMER_ARTIFACT_PATH` artifact-lineage inputs this doc's own
offline comparison plan leans on throughout §1.4/§1.6/§3). This is a MUCH more foundational gap
than anything else in this document: the entire "train both HMMs, compare via BackTesterStudy,
then cut over" plan (§1.6's scope note, §1.7/§1.8's whole premise) assumes `BackTesterStudy.cpp`
works as coded on the EXISTING calendar-clock system it was originally built for — that has never
been verified. Every reference in this doc to its artifact-path mechanism as an "existing"/
"already-established" pattern describes code that exists, not a proven, working precedent — that
distinction matters and should not be glossed over. **Before any `ImbalanceBackTesterStudy.cpp`
design work, before relying on this mechanism for any real comparison decision, `BackTesterStudy
.cpp` needs to be run for the first time, on the calendar-clock system, and shown to actually
produce a usable `.btst`/`RunSummary`.** See Next Steps item 0 below — this now outranks every
other open item in this doc.

Per operator directive (2026-09-06): the end goal is a full eventual cutover from calendar-clock
to activity/imbalance-clock across the whole system, structurally mirroring the existing Elder
Triple Screen (`TS1`/`TS2`/`TS3`) with 3 imbalance-bucket sizes instead of 3 calendar periods
(`IS1`/`IS2`/`IS3` or similar naming, 5:1 ratio — macro/intermediate/micro). Everything built along
the way (Force Index's Work Rate, activity-clock observation-vector dims already shipped) is a
transitional, single-indicator instance of this larger migration — see each of those specs' own
"staged migration, not permanent fork" framing, corrected 2026-09-06.

## 1. New components this migration requires

### 1.1 `ImbalanceScreen1.cpp` / `ImbalanceScreen2.cpp` / `ImbalanceScreen3.cpp`

New ACSIL study files, structurally parallel to `TripleScreen1/2/3.cpp`, each driven by one of 3
imbalance-bucket sizes (`N×25`/`N×5`/`N` imbalance units) instead of a calendar period (240/60/15
min). **CORRECTED 2026-09-07 (§1.2c): these files no longer compute anything.** Activity-clock-
native dims are computed centrally by `ImbalanceContextManager` (§1.2c), and (once a real one
exists, §1.3) activity-clock-native Gang-statistical indicators (Work Rate -- since retargeted
away from this role, see §1.3 -- and any future Gang-MACD/Gang-Impulse) would be computed
centrally by `ImbalanceIndicatorManager`, both invoked directly from `IS1`'s cascade. The screen
files are pure, precedence-agnostic DISPLAY: each just reads its own dim(s) from whichever manager
owns them and puts the value on a `Subgraph`.

#### 1.1a `ImbalanceScreen1.cpp` concrete scoping, 2026-09-07 — the smallest starting point

**Structural template, verified against the real file, not assumed**: `TripleScreen1.cpp` is
actually THREE separate `SCSFExport` study functions in one file (`scsf_...Impulse System`,
`scsf_...MACD`, `scsf_...13-Period FI`, each its own `sc.AutoLoop = 1` ACSIL study attached to the
same TS1 chart) — `ImbalanceScreen1.cpp` should follow this same one-file/multiple-studies
convention, not a single monolithic function, once it has enough indicators to warrant a split.

- **RESOLVED 2026-09-07 (answers Next Steps item 9a), SUPERSEDED 2026-09-07 by §1.2a/§1.2b — read
  those before trusting anything below this bullet.** Original claim ("each `ImbalanceScreen*.cpp`
  owns its own local `ImbalanceBarEngine` instance directly") is now WRONG for all three screens:
  none of `ImbalanceScreen1/2/3.cpp` own an `ImbalanceBarEngine` instance directly. The one real
  engine lives privately inside a new singleton holder (`ImbalanceClockManager`, §1.2b), fed by
  `ImbalanceScreen1.cpp` (the highest-precedence screen, chosen as producer specifically because it
  calculates first — see §1.2b's ordering table). `ImbalanceScreen2.cpp`/`ImbalanceScreen3.cpp` are
  pure readers of the holder's `GetIS2Bar()`/`GetIS3Bar()`. The "no `ImbalanceScreenManager`
  coordinating all three" reasoning below no longer holds even in spirit — a coordinating singleton
  is exactly what `ImbalanceClockManager` is, deliberately, mirroring `ContextManager`/
  `IndicatorManager`'s own established pattern for cross-chart shared state (§1.2b's rationale).
- **Real gap found, not previously flagged anywhere in this doc**: there is currently NO
  macro/intermediate/micro split among the activity-clock dims at all — checked directly against
  `ContextManager.cpp`'s activity-clock block (lines ~570-645): `skewness_idx`, `fast_taleb_
  kurtosis`, `fast_hurst_exponent`, and `recurrence_rate` (the 4 still-live dims; `fast_mean_rev_z`
  is OUT, per the observation-vector doc's own table) ALL read from the SAME single 100-bar window
  off the SAME single `ActivityClockManager`-owned engine today. Unlike the calendar-clock side
  (where Screen 1/2/3 ownership of `log_scale_ratio`/`fisher_info` vs. `amihud_illiquidity`/
  `fractal_dim` vs. `mean_rev_z`/etc. is already real and long-established), there is no existing
  precedent to mirror for which activity-clock dim belongs on `IS1` vs. `IS2` vs. `IS3` — this has
  to be decided fresh, not ported.
- **RESOLVED 2026-09-07 — all 4 dims placed, grounded in real calendar-clock precedent, not
  first-principles guessing.** Checked each dim's own calendar-clock sibling's actual screen
  ownership directly against the real code (not assumed):
  - **`hurst_exponent` → `IS1`** (confirms the earlier candidate): `TripleScreen1.cpp:629`,
    `obs->mutate_hurst_exponent(hurst)`, computed over `macro_window_n` — the calendar-clock
    version is ITSELF TS1/Screen-1-owned, not just conceptually macro-flavored.
  - **`recurrence_rate` → `IS2`**: historically TS2-owned as one of TS2's "structural" dims
    (alongside `fractal_dim`) before its 2026-08-28 activity-clock replacement — `ContextManager
    .cpp`'s own comment documents this explicitly ("recurrence_rate is no longer TS2-owned —
    `TripleScreen2.cpp` no longer mutates this field").
  - **`skewness_idx` → `IS3`**: `TripleScreen3.cpp:788-793`, under the file's own "CANONICAL
    OBSERVATIONDATA VECTOR - SCREEN 3 (RIPPLE)" section header.
  - **`taleb_kurtosis` → `IS3`**: `TripleScreen3.cpp:1381`, `anchors.realizedKurtosis`, feeding
    `ContextManager.cpp:269`'s `m_latestInstitutionalMetrics.talebKurtosis` — same screen as
    `skewness_idx`, both are TS3/Ripple-owned "shape of the return distribution, fast-reacting"
    measures on the calendar-clock side, a real, not coincidental, grouping.
  - **Net result**: `IS1` gets 1 dim (`hurst_exponent`), `IS2` gets 1 dim (`recurrence_rate`),
    `IS3` gets 2 dims (`skewness_idx`, `taleb_kurtosis`) — asymmetric, matching the real
    calendar-clock precedent exactly rather than forcing an artificial even split.
- **`IndicatorKey` namespacing, RESOLVED 2026-09-07 (answers §1.3's follow-on)**: append new
  imbalance-only keys immediately after today's `INTERM_MKT_ACTION = 54`, starting at the current
  `MAX_INDICATORS` boundary (55), with a named boundary constant
  (`constexpr uint8_t kFirstImbalanceIndicatorKey = 55;`) rather than jumping to a round number
  like 200 — `IndicatorKey` is a dense, `uint8_t`-sized index into a fixed-size packed array
  (`std::array<BaseIndicator*, MAX_INDICATORS>`), so leaving a large unused gap would waste array
  space for no benefit; a simple boundary CONSTANT is sufficient for calendar-clock-specific code
  to skip the imbalance range (`key < kFirstImbalanceIndicatorKey`), no gap needed. Four concrete
  keys, matching the dim-placement mapping above: `IMBALANCE_HURST_EXPONENT = 55` (`IS1`),
  `IMBALANCE_RECURRENCE_RATE = 56` (`IS2`), `IMBALANCE_SKEWNESS_IDX = 57` (`IS3`),
  `IMBALANCE_TALEB_KURTOSIS = 58` (`IS3`); `MAX_INDICATORS` becomes `59`.
- **Tick-level integration point**: per-tick, call the local engine's `OnTickWithPrice(tickIndex,
  askVolume, bidVolume, price)` using real `sc.AskVolume[sc.Index]`/`sc.BidVolume[sc.Index]`/
  `sc.Close[sc.Index]` — the same real inputs `ActivityClockManager::Update(sc)` already feeds its
  own single engine with today, just routed to `ImbalanceScreen1.cpp`'s own local instance instead.
- **Explicitly NOT scoped here**: `ImbalanceScreen2.cpp`/`ImbalanceScreen3.cpp`'s own file
  structure/implementation (the dim-placement mapping above now covers WHICH dims, not HOW each
  screen file is built — that follows §1.1a's own template once written);
  `ImbalanceIndicatorManager`'s own packed-array write path wiring (§1.3, still needs its own
  design pass); any schema/`ImbalanceObservationData` field addition (§1.5's case-by-case
  inclusion still applies per-dim, this is only a placement proposal, not an inclusion decision).

### 1.2 `ImbalanceBarEngine` multi-frame extension

Today's `include/ImbalanceBarEngine.h` is a single instance with one fixed threshold
(`m_imbalanceThreshold = 50.0f`). Originally scoped as three gaps; two are now resolved:

1. **RESOLVED 2026-09-07 — turns out to require zero new engine code.** `ImbalanceBarEngine` is
   already a plain, standalone, instantiable class (no singleton pattern of its own) — 3
   independently-parameterized instances (one per `IS1`/`IS2`/`IS3`) already work today by simply
   constructing 3 separate objects and calling `SetImbalanceThreshold()` on each with a different
   bucket size; `SetImbalanceThreshold`'s own doc comment already anticipated exactly this. The
   only real remaining gap is WHERE those 3 instances live (a new holder class, or inline in each
   `ImbalanceScreen*.cpp` — not decided, small either way) — not a class-design gap.
2. **IMPLEMENTED 2026-09-07: EWMA-adaptive threshold.** `EnableAdaptiveThreshold(float alpha =
   0.01f)` (opt-in, disabled by default — existing `SetImbalanceThreshold()`-only callers are
   byte-for-byte unaffected, verified by test) implements the AFML Ch. 2 (López de Prado 2018)
   `b_target = E[T]·|E[b_k·v_k]|` mechanism confirmed literature-grounded 2026-09-06
   (`docs/superpowers/specs/2026-09-06-labeling-data-augmentation-gang-statistical-reformulation-
   initiative.md` §2.1): both `E[T]` (expected ticks per bar) and `E[b_k·v_k]` (expected per-tick
   signed imbalance) are EWMA-updated after every completed bar, seeded from the first real
   completed bar's own observed values (not a fabricated zero). 5 new native tests added
   (`tests/cpp/test_imbalance_bar_engine.cpp`, 23/23 pass total), full `./build_dll.sh --no-clean`
   verified clean. `α=0.01` matches the source brainstorm's own pseudocode, not independently
   re-derived — a real, still-open empirical-calibration follow-on, not asserted as final here.
3. **Per-bar imbalance-magnitude retention**: already done (`m_completedBarImbalances`/
   `GetImbalanceBarMagnitudes()`, shipped during the `Y_imb` investigation, 2026-09-06) — needed
   for `Y_imb` and likely other future constructs, not just Work Rate.

### 1.2a Multi-resolution bar construction, RESOLVED 2026-09-07 — hierarchical nesting, NOT 3 independent engines

**This REVISES §1.1a's engine-ownership design below** — that section's "each `ImbalanceScreen*
.cpp` owns its own local `ImbalanceBarEngine` instance" claim is now superseded for `IS1`/`IS2`
specifically (`IS3` is unaffected: it remains the one real threshold-crossing engine).

**Origin**: real empirical evidence (2026-09-07, `tools/observation_vector/
imbalance_screen1_hurst_eval.cpp`) showed `EnableAdaptiveThreshold()` run independently converges
toward a FINER natural scale (580,356 bars) than today's shipped fixed-700 baseline (108,691
bars) — proving 3 independent adaptive engines applied to the same raw tick stream would NOT
produce a deliberate macro/intermediate/micro split; each just converges toward the same
underlying "natural" per-tick pattern, which is circular. Two independent literature reviews were
consulted (Gemini CLI, read-only, hit a transient tool-search error mid-run and did not complete;
Gemini web — no repo access, no code changes possible — completed a thorough, well-cited review;
both are consistent in spirit with what's below).

**Two candidate architectures were evaluated, one confirmed institutionally sound, one rejected**:

- **REJECTED — "derive-one-then-scale" (3 independent flat-threshold engines, one adaptively-
  derived baseline `b*` scaled by fixed multiples `5b*`/`25b*` for the other two)**: order-flow
  imbalance `θ_T = Σb_k·v_k` is a stochastic drift-diffusion process — hitting threshold `b*` some
  number of times in a row does NOT equate to hitting `5b*` on the SAME raw stream at the same
  tick. Three engines thresholding the same stream independently produce bar boundaries that
  drift relative to each other with no structural relationship — `IS1` could close mid-way through
  an `IS2`/`IS3` bar, destroying any clean multi-resolution sequence representation
  `[S1,S2,S3]_t` for downstream HMM/Transformer consumption without ad hoc interpolation.
- **CONFIRMED SOUND — hierarchical nesting ("bars-of-bars")**: run the ONE real adaptive
  `ImbalanceBarEngine` (`EnableAdaptiveThreshold`) at `IS3` (the finest resolution) only. Construct
  `IS2` bars by aggregating a target count `K2` of already-completed `IS3` bars; construct `IS1`
  bars by aggregating a target count `K1` of already-completed `IS2` bars. This guarantees EXACT
  boundary nesting (every `IS1` bar contains exactly `K1×K2` `IS3` bars, no drift, no partial
  overlap) — the same nesting property calendar bars already have for free (240min = 4×60min =
  16×15min exactly), and directly mirrors formal multiresolution-analysis theory's own nested
  approximation-space requirement (Mallat 1989; Daubechies 1992; Gençay, Selçuk & Whitcher 2001:
  $V_j \subset V_{j-1}$, violated by non-aligned boundaries). Also directly explains and resolves
  the empirical finding above: there is no "multi-estimator instability" because there is only
  ONE estimator now, not three converging independently to the same thing.
- **Noted, not adopted as the core mechanism**: a third option (uniform information-time sampling
  at `IS3` alone, then a wavelet/filter-bank decomposition — Mallat 1989-style DWT — applied
  across that single uniform series to extract multi-scale features) is mathematically elegant
  for FEATURE extraction and grounded in subordination theory (Clark 1973; Ané & Geman 2000: an
  information-time-sampled return series is closer to i.i.d. Gaussian, a precondition classical
  linear wavelet tools assume) — but hierarchical nesting remains superior for producing discrete,
  human/chart-interpretable multi-bar OHLCV representations (matching this system's own Elder-
  Raschke Triple Screen heritage of actual bars, not just derived features). Worth revisiting for
  feature-engineering purposes later, not the bar-construction mechanism itself.

**The `25:5:1` ratio's own grounding, also resolved**: Elder's original "roughly 4-6x per screen"
rule (Elder 1986/1993) has NO formal market-microstructure justification in calendar time — it was
a heuristic for human chart inspection. But translated into information time, a fixed `m≈4-5`
scale-separation ratio IS independently grounded by multifractal cascade literature (Mandelbrot
1997; Calvet & Fisher 2002) as a genuine separation of orthogonal "frequency bands" of market
activity (institutional execution horizons vs. local market-maker inventory cycles vs. immediate
aggressor/sweep activity) — a real, independent justification for keeping a similar ratio, not
merely inherited from Elder's own unrelated calendar-time heuristic.

**Concrete architecture, IMPLEMENTED 2026-09-07 (`include/ImbalanceClockManager.h`,
15/15 native tests pass)**: one `ImbalanceBarEngine` with `EnableAdaptiveThreshold()` at `IS3`;
`ImbalanceClockManager` (§1.2b) aggregates `IS3`'s completed-bar log returns in groups of `K2` to
form each `IS2` bar's return (exact telescoping sum, not an approximation), and `IS2`'s in groups
of `K1` to form each `IS1` bar's return — using `RingBuffer` per this repo's own DOD/fixed-capacity
convention (the reviewed pseudocode's `std::vector`-buffer sketch was not carried over). Scoped to
LOG RETURNS ONLY, not full OHLCV bars — the 4 dims already placed onto IS1/IS2/IS3 all consume a
return series; a full OHLCV struct is deferred until/unless a future dim needs it.

**`K1`/`K2` RESOLVED 2026-09-07 — empirically validated at 4/4, not the literature-plausible 5/5**
(`tools/observation_vector/imbalance_clock_manager_ratio_eval.cpp`, real 471.9M MES ticks, matching
this repo's own standing discipline of letting real data adjudicate between literature-plausible
candidates, e.g. `fractal_dim`'s Politis-White block-length precedent). Criterion: subordination
theory (Clark 1973; Ané & Geman 2000) predicts lag-1 return autocorrelation should trend toward
zero at coarser (more heavily-aggregated) scales. Real results: `K=4` — IS3=0.197, IS2=0.035,
IS1=0.135; `K=5` — IS3=0.197 (same engine, unaffected by K), IS2=0.087, IS1=0.253. **Neither
candidate shows a fully monotonic decorrelation trend** (IS1 ticks back up from IS2 in both cases,
a real, honestly-flagged caveat, not glossed over) — but `K=4` is unambiguously better on every
axis: lower autocorrelation at both IS2 and IS1, and IS1 still ends up below IS3's raw level
(0.135 < 0.197). `K=5`'s IS1 autocorrelation (0.253) is actually HIGHER than IS3's raw level —
actively reversing the predicted trend, not merely underperforming it, a strong reason to reject 5
as the default rather than a marginal call. `K=4` also happens to match this repo's own existing
calendar-clock ratio exactly (`TS1:TS2:TS3` = 240:60:15min = a clean 4:1 hop). `ImbalanceClockManager`
(§1.2b) now defaults to `4`/`4`. Sample sizes (IS1: 36,272 bars at K=4, 23,214 at K=5) are real but
from a single pass, not bootstrapped/confidence-interval-tested — a reasonable basis for a default,
not a claim of statistical certainty.
- §1.2b (below) resolves where the aggregator lives and how it's driven — read that before §1.1a.

### 1.2b Holder placement, tick-feed ownership, and cross-screen ordering, RESOLVED 2026-09-07

**Holder is a new singleton, mirroring `ContextManager`/`IndicatorManager`'s existing pattern** —
not bundled into `ImbalanceBarEngine` itself. `ImbalanceBarEngine` stays exactly as-is (pure,
non-singleton, unit-testable in isolation, no ACSIL/cross-chart awareness, no aggregation
responsibility). **IMPLEMENTED 2026-09-07**: `ImbalanceClockManager` (`include/
ImbalanceClockManager.h`, `static Instance()`, 15/15 native tests pass — `tests/cpp/
test_imbalance_clock_manager.cpp`) privately owns one real `ImbalanceBarEngine` (the `IS3`
adaptive threshold-crossing engine) plus the `K2`/`K1` bars-of-bars return-aggregation buffers
(`RingBuffer<float, 500>`), and exposes `OnTick(barIndex, askVolume, bidVolume, price)` (mutator,
IS1-exclusive per the producer discipline below), `ConfigureIs3Threshold()`/
`EnableIs3AdaptiveThreshold()` (IS3 engine configuration pass-through), and `GetIs1Returns()`/
`GetIs2Returns()`/`GetIs3Returns()` plus `GetIs1CompletedBarCount()`/`GetIs2CompletedBarCount()`/
`GetIs3CompletedBarCount()` (read-only accessors) — not yet wired into any `ImbalanceScreen*.cpp`
(those files don't exist yet, §1.1a), but the holder itself is real, tested, production-shaped
code, not a design sketch.

Rejected alternative: bundling threshold-crossing + aggregation into `ImbalanceBarEngine` itself
(a single God-class singleton). Rejected because it conflates two different responsibilities (pure
accumulator vs. cross-chart shared state), grows `Reset()`'s cascade-clear surface area for
backtest replay, and repeats this repo's own `IndicatorManager` scope-creep precedent (the ~44-
member heterogeneous store now being migrated away from, per
`docs/superpowers/specs/2026-08-04-indicator-manager-dod-soa-design.md`) rather than learning from
it.

**Cross-screen calculation ordering uses `sc.CalculationPrecedence`** (confirmed directly from
Sierra Chart's own ACSIL docs, `ACSIL_Members_Variables_And_Arrays.html`: default `STD_PREC_LEVEL`
calculates first; `LOW_PREC_LEVEL` calculates after all `STD_PREC_LEVEL` studies; `VERY_LOW_PREC_LEVEL`
calculates after both). Sierra Chart is single-threaded (all chart study-function calls are
serialized on one thread — no race conditions possible, only call-order/discipline concerns), and
this repo already relies on this exact mechanism for real cross-study ordering guarantees
(`SCStudies.cpp`/`BackTesterStudy.cpp`/`EventDataCollectorStudy.cpp` all set `LOW_PREC_LEVEL`;
`TripleScreen3.cpp:587-590`'s own comment reasons about the resulting ordering explicitly).

**`ImbalanceScreen1.cpp` (`IS1`) is BOTH the highest-precedence screen AND the tick-feed producer
— these are made the same fact by design, not two independently-enforced ones:**

**SUPERSEDED TWICE, SAME DAY, 2026-09-07 — read `1.2c` (below) for the design that actually
shipped.** History, for the record (both intermediate attempts left real lessons):
1. ~~Collapsed IS2/IS3 onto one precedence tier, freeing a tier for the collector~~ — wrong,
   discarded a real near-term requirement (IS3 will genuinely need to cross-read IS2's computed
   dim once more indicators land) instead of resolving the actual conflict.
2. ~~Kept the full STD/LOW/VERY_LOW 3-way split for IS1/IS2/IS3, and gave the collector a
   cascade-generation-stamp freshness check instead of a 4th precedence tier~~ — correct as far as
   it went, but still left 3 separate ACSIL studies computing their own dims independently, each
   needing its own precedence-tier coordination for correctness. Operator feedback (2026-09-07):
   why coordinate 3 racing studies at all, when the cascade already visits every screen's data in
   the right order on its own? Superseded by centralizing computation instead of distributing it
   — see `1.2c`.

### 1.2c Centralized dim computation, driven directly by the cascade — RESOLVED AND IMPLEMENTED 2026-09-07

**IMPLEMENTED 2026-09-07** (`include/ImbalanceContextManager.h`, `src/ImbalanceScreen1/2/3.cpp`,
`src/ImbalanceEventDataCollectorStudy.cpp` — 13/13 native tests pass,
`tests/cpp/test_imbalance_context_manager.cpp`, rewritten for this design; `./build_dll.sh
--no-clean` clean). Also removed the now-dead `ImbalanceClockManager::GetCascadeGeneration()`/
`m_cascadeGeneration` mechanism (§1.2b) entirely from `include/ImbalanceClockManager.h` — no longer
referenced anywhere once `ImbalanceContextManager::Update()` made staleness structurally
impossible instead of merely detectable.

**The `ImbalanceContextManager` is called directly by the cascade (from `IS1`'s own tick handler,
immediately after `ImbalanceClockManager::OnTick()`), and computes ALL 4 dims itself, in one
deterministic, sequential function** — `hurst_exponent` (`IS1`'s returns) → `recurrence_rate`
(`IS2`'s returns) → `skewness_idx` + `taleb_kurtosis` (`IS3`'s returns), in that literal statement
order. This replaces BOTH prior designs above, not just the collector's freshness mechanism:

- **`ImbalanceScreen1/2/3.cpp` no longer compute anything.** They become pure, precedence-agnostic
  DISPLAY studies — each just reads whatever dim(s) it wants from `ImbalanceContextManager`'s
  already-fully-computed vector and puts it on its own `Subgraph`. No cross-screen ordering
  dependency exists among them anymore, because no computation happens in them anymore.
- **Perfect same-tick freshness becomes structural, not probabilistic.** The cascade-generation-
  stamp mechanism (`1.2b`) is no longer needed at all — there is no window where "some dims are
  this tick's, some are last tick's" can even occur, since all 4 are computed in one function, one
  call, sequential statements, with no other code path able to interleave. This is strictly
  stronger than the generation-stamp check (which only detected staleness after the fact) — it
  makes staleness structurally impossible instead of detectable.
- **The precedence-tier problem dissolves.** Only `IS1` needs `STD_PREC_LEVEL` (to run first and
  drive the cascade + computation). `IS2`, `IS3`, and `ImbalanceEventDataCollectorStudy.cpp` are
  now ALL pure readers with no ordering dependency on each other — they can share one single lower
  tier (`LOW_PREC_LEVEL`, all three), or even not set `CalculationPrecedence` at all, since nothing
  about their own correctness depends on relative order among themselves anymore. `VERY_LOW_PREC_LEVEL`
  isn't needed by anything in this design.
- **Real future cross-reads (e.g. a future Gang-indicator on `IS3` referencing `IS2`'s computed
  dim) are trivially satisfied for free** — `ImbalanceContextManager`'s own internal computation
  order already computes `IS2` before `IS3`; a future `IS3` dim can simply read the
  already-computed `recurrence_rate` from the SAME struct/array it's about to write into, no
  precedence-tier coordination ever required, no matter how many more dims get added later.
- **Generalizes to `ImbalanceIndicatorManager` (§1.3) the same way**, once that's built: it too
  would be invoked directly from the cascade (from `IS1`'s tick handler, alongside
  `ImbalanceContextManager`), computing whatever activity-clock-native indicators it owns in one
  deterministic pass — not scattered across per-screen files. Not built now (§1.3 remains a
  separate, later design pass) — recorded here as the established pattern to follow when it is.

### 1.2d Cache/pointer-chasing discipline, carry-forward, and individual compute functions — RESOLVED AND IMPLEMENTED 2026-09-07

**IMPLEMENTED 2026-09-07** alongside §1.2c (same commit/session) — `ComputeImbalanceHurstExponent`,
`ComputeImbalanceRecurrenceRate`, `ComputeImbalanceSkewnessKurtosis`/`ImbalanceSkewKurt`, and
`ApplyWithCarryForward<Index>()` all live in `include/ImbalanceContextManager.h` exactly as
designed below — one deviation from the illustrative signature: `ComputeImbalanceRecurrenceRate`
takes an explicit `completedBarCount` parameter (not shown in the original sketch) so it can detect
"has a new IS2 bar closed" without a hidden singleton read, keeping the function's own "no hidden
state" contract intact; `ImbalanceContextManager::Update()` passes
`ImbalanceClockManager::Instance().GetIs2CompletedBarCount()` in.

**Standing rule for this initiative, stated explicitly, not just implied**: no pointer/heap
ownership and no indirect (function-pointer/vtable) dispatch for any state or compute logic this
component owns. Concretely:
- Any owned engine/state (e.g. `RecurrenceRateEngine`) is a **direct value member**, never
  `unique_ptr`/heap-allocated — a pointer there would be a genuine, avoidable cache-miss on every
  tick for zero benefit.
- Compute functions are called **directly, by name** — never through a generic function-pointer
  dispatch table (a `kImbalanceDimLayout`-style descriptor table, mirroring `IndicatorLayout.h`,
  was considered and rejected: the 4 dims' compute signatures are genuinely heterogeneous —
  `recurrence_rate` needs engine + bar-count-cache state, the others don't — forcing a uniform
  function-pointer signature would either lose type safety or block inlining for no real benefit).
- At this scale (4 dims today, plausibly ~18 once more dims are case-by-case included per §1.5),
  `m_values`/`m_lastValid` (below) fit in 2-3 cache lines regardless of array layout choice — the
  classic AoS-vs-SoA micro-optimization genuinely does not apply here; being honest about that,
  rather than manufacturing a layout "win" that isn't real, matters more than the layout itself.

**Individual compute functions, one per dim, take explicit array+count inputs and return a value —
no hidden singleton reads, no side effects**, mirroring this repo's own existing math headers
(`DfaHurstExponent.h`, `RobustMoments.h`, `RecurrenceRateEngine.h`) exactly:
```cpp
float ComputeImbalanceHurstExponent(const float* returns, std::size_t count);
float ComputeImbalanceRecurrenceRate(const float* returns, std::size_t count,
                                      RecurrenceRateEngine& engine, std::size_t& lastBarCount);
struct ImbalanceSkewKurt { float skewness; float kurtosis; };
ImbalanceSkewKurt ComputeImbalanceSkewnessKurtosis(const float* returns, std::size_t count);
```
Only `ImbalanceContextManager::Update()` itself touches `ImbalanceClockManager::Instance()` to pull
the 3 returns buffers — every compute function stays fully unit-testable with synthetic arrays.

**Carry-forward is centralized and generic, not duplicated 4x** (the RobustMoments.h/
DfaHurstExponent.h documented contract — NaN on a degenerate window, caller must carry-forward —
is IDENTICAL across all 4 dims, so this is a real DRY win only possible once computation is
centralized): a parallel `m_lastValid` array plus one compile-time-devirtualized template, same
idiom as `SetDim<Index>()`:
```cpp
std::array<float, N> m_values;     // current computed value per dim
std::array<float, N> m_lastValid;  // carry-forward fallback per dim

template <ImbalanceObsIndex Index>
void ApplyWithCarryForward(float raw) {
    if (std::isfinite(raw)) { m_values[Index] = raw; m_lastValid[Index] = raw; }
    else                    { m_values[Index] = m_lastValid[Index]; }
}
```
`recurrence_rate`'s own extra state (`RecurrenceRateEngine` + a "rebuild the O(n²) matrix only on a
new bar" cache) stays a dedicated member, not generalized into the shared arrays — a genuinely
different-in-kind concern (performance caching, not NaN-handling), matching this repo's own
`IndicatorManager` hybrid-architecture lesson: keep the generic part generic, keep real
heterogeneity concrete and named, don't force a false-generic abstraction over it.

### 1.2e Emission ownership: mode-gated, inside `ImbalanceContextManager` — RESOLVED 2026-09-07

**`ImbalanceContextManager` owns deciding what happens to the computed vector, gated by a mode
fixed once at initialization — not a separate collector file deciding externally.** This is MORE
consistent with the real, already-existing precedent than an earlier draft of this doc assumed:
`ContextManager::EmitTrainingContext()` (the `.context` write) and `EmitLiveContext()` (the ZMQ
send) are both `ContextManager`'s OWN private methods, invoked internally from
`ContextManager::CheckAndTriggerHMM()` — the calling study (`EventDataCollectorStudy.cpp`) only
decides WHEN to invoke the trigger, never what happens to the result.

**Init-time-fixed mode, not a per-call bool** (the calendar-clock precedent passes
`isDataCollection` per-call to `CheckAndTriggerHMM(now_us, true, ...)`, because `SCStudies.cpp`
(live) and `EventDataCollectorStudy.cpp` (collection) CAN coexist there — the imbalance clock's own
§1.8 mutual-exclusivity ruling means collection and live never run simultaneously, making an
init-time-fixed mode the more correct model for this system specifically, not merely a
simplification borrowed without justification):
```cpp
enum class ImbalanceMode { Live, DataCollection };
void Configure(ImbalanceMode mode);       // called once, e.g. from IS1's SetDefaults
void Update(uint64_t timestamp_us);       // always computes all 4 dims; internally emits per m_mode
```
`Update()` stays ACSIL-independent (`timestamp_us` passed in by the caller, e.g.
`sc.GetCurrentDateTime().ToUNIXTimeInMicroseconds()` from `ImbalanceScreen1.cpp`) — the manager
itself never touches `SCStudyInterfaceRef`.

**Real fork, NOT YET RESOLVED, deferred on purpose (operator directive 2026-09-07): decide
`ImbalanceEventDataCollectorStudy.cpp`'s fate only once `ImbalanceIndicatorManager`'s own
update/compute design (§1.3) is established.** If `Update()` internally emits, `IS1` alone could
drive everything (cascade + dim computation + conditional write), making the standalone collector
file potentially redundant — OR the collector file might still earn its keep as the future home for
Lock-A-E-style readiness gates (§1.6) that shouldn't bloat `ImbalanceContextManager` itself. Explicitly
not resolved here; `src/ImbalanceEventDataCollectorStudy.cpp` (already implemented, 2026-09-07)
stays as-is until this is decided.

**Revised precedence table, IMPLEMENTED 2026-09-07**:

| Screen | Precedence | Role |
|---|---|---|
| `ImbalanceScreen1.cpp` (IS1) | `STD_PREC_LEVEL` (default) | Calls `ImbalanceClockManager::Instance().OnTick(sc)` (bar cascade), THEN `ImbalanceContextManager::Instance().Update()` (computes all 4 dims in order), THEN reads its own dim for display |
| `ImbalanceScreen2.cpp` (IS2) | `LOW_PREC_LEVEL` | Pure display: reads `recurrence_rate` from `ImbalanceContextManager` |
| `ImbalanceScreen3.cpp` (IS3) | `LOW_PREC_LEVEL` | Pure display: reads `skewness_idx`/`taleb_kurtosis` from `ImbalanceContextManager` |
| `ImbalanceEventDataCollectorStudy.cpp` | `VERY_LOW_PREC_LEVEL` (kept defensively last, operator preference 2026-09-07 -- not strictly required for correctness under §1.2c, but no cost either) | Present role, per §1.2e: unclear whether this file still has a job once `ImbalanceContextManager::Update()` can emit internally — fork not yet resolved, deferred until §1.3's `ImbalanceIndicatorManager` design lands |

`AllDimsReady()` keeps its name and IMPLEMENTS the simplification exactly as designed: "has
`Update()` been called at least once" (a single monotonic `bool m_updated`, not a per-dim
generation-stamp array) — since freshness is now structural, not per-dim-tracked. `SetDim<Index>()`
was removed entirely (no longer needed — nothing outside `ImbalanceContextManager::Update()` writes
dims anymore); replaced by a read-only `GetDim<Index>()` for the pure-display screens.

**Rejected alternative, and why**: a `friend`-restricted/capability-token `OnRawTick()` on the
holder was considered, to structurally prevent `IS2`/`IS3` from accidentally becoming producers —
motivated by a real risk (precedence order and producer identity are two independent facts a
future editor could confuse) but rejected as unneeded ceremony once producer identity and
first-precedence were unified onto the same screen (`IS1`): the confusion this would have guarded
against ("the screen that runs first should be the one driving the tick feed") is simply now
*correct*, not a trap — with only two other developers (Claude, Gemini) and the user working this
codebase, a compiler-enforced guardrail for a no-longer-possible mistake is exactly the kind of
unrequested abstraction this repo's own engineering discipline rejects.

### 1.3 `ImbalanceIndicatorManager` (new) — IMPLEMENTED 2026-09-07 (first candidate only)

**IMPLEMENTED 2026-09-07** (`include/ImbalanceIndicatorManager.h` -- 14/14 native tests pass,
`tests/cpp/test_imbalance_indicator_manager.cpp`; wired into `src/ImbalanceScreen1.cpp`'s tick
handler right after `ImbalanceContextManager::Update()`; displayed on `src/ImbalanceScreen2.cpp`'s
new Subgraph 1/2; `./build_dll.sh --no-clean` clean). Only §1.3a's Gang-MACD/Phase-Coherence
general-purpose 3-state classification shipped -- its divergence/TRAP sub-case did NOT (see §1.3a's
own updated status below). The pattern below is confirmed correct in practice, not just designed:

**UPDATED 2026-09-07: a real first candidate now exists — see §1.3a.** `Work Rate`/`Y_imb` (this
session's FIRST activity-clock indicator candidate) was closed out and retargeted AWAY from an
indicator/observation-vector role, to the risk-gating doc's §2.4 as a liquidity-gate candidate
instead. Force Index-on-the-imbalance-clock (a SECOND candidate) was also declined. **A THIRD
candidate — a Gemini-reframed, entropy-native MACD/Impulse-System replacement (§1.3a) — was worked
through this session and adopted**, pending one real, confirmed prerequisite gap (an
activity-clock-native Shannon entropy measure, §1.3b) that does not yet exist anywhere in this
codebase. Building `ImbalanceIndicatorManager` itself still waits on that prerequisite — the
"producer before consumer" discipline still applies, just against a real, named target now instead
of an empty category.

**Pattern locked in now, so `ImbalanceEventDataCollectorStudy.cpp`'s design (§1.2e's open fork)
can eventually be decided against it** — mirrors `IndicatorPackedState` (the packed-array READ
side `IndicatorManager` is migrating TOWARD), explicitly NOT `IndicatorStore`/`BaseIndicator`/
`Indicator<T>` (the OOP WRITE side it's migrating AWAY from, per
`docs/superpowers/specs/2026-08-04-indicator-manager-dod-soa-design.md`'s own retrospective — real
crash, real ~44-member scope creep, real virtual-dispatch cost). Task 6's `Macd` extraction (the
only indicator among ~44 that has actually completed that migration) is the template to start from,
not the other 43. Concretely: real trading indicators pair an `Int8` classification (bullish/
bearish/neutral) with a `Float32` companion quality/confidence score (matching `IndicatorState`'s
real schema shape), so the packed arrays are:
```cpp
std::array<int8_t, N> m_signals;
std::array<float, N>  m_quality;
```
with an `ImbalanceIndicatorKey`-style enum indexing both (same idiom as `ImbalanceObsIndex`), one
free function per indicator returning `{signal, quality}` together (mirroring §1.2d's
`ImbalanceSkewKurt`-style paired-return convention), and a single `Update()` orchestrator —
identical shape to `ImbalanceContextManager::Update()`, invoked from the SAME cascade call site
(`IS1`'s tick handler), just writing into signal+quality slots instead of one float per dim.

### 1.3a First real candidate: Gang-MACD / Phase Coherence indicator — GENERAL-PURPOSE 3-STATE SIGNAL IMPLEMENTED 2026-09-07, DIVERGENCE/TRAP SUB-CASE NOT YET WIRED

**IMPLEMENTED 2026-09-07** (`include/ImbalanceIndicatorManager.h`'s `ComputeGangMacdPhaseCoherence()`
+ `ImbalanceIndicatorManager::Update()`): `dP/dτ` is the latest completed `IS2` bar's own log
return (return = log(P_end/P_start) for that bar IS the discrete phase velocity by construction --
no separate price-level tracking needed); `dH_norm/dτ` is the first difference of normalized
entropy (`H_norm = 1 - shannon_efficiency`, read from `ImbalanceContextManager::GetShannonEfficiency()`,
§1.3b) across consecutive `IS2` bar closes, `0.0` (mapping to the spec's own `<= 0` branch, not an
approximation) on any tick where no new bar has closed. Quality = `|dP/dτ|`, per the design's own
"not finalized, secondary to signal logic" framing -- the simplest non-invented choice, not a final
answer. **NOT implemented**: the divergence sub-case (Thermodynamic Exhaustion -> `TRAP_LONG`) --
requires cross-referencing the native TRAP/`StructureTest` framework (`CLAUDE.md`), a genuinely
separate, larger integration deferred as its own follow-on, not attempted in this pass.

**Origin, not to be confused with two declined candidates.** Session explored, in order: (1) Force
Index-on-imbalance-clock (`ΔP×√V`, `docs/superpowers/specs/2026-09-04-indicator-gang-statistical-
reformulation-initiative.md` §1) — **declined** by operator directive, not pursued further; (2) a
first MACD framing (robustify EMA with median/MAD, later a cross-scale `IS1`-vs-`IS3` drift-
difference proposal) — **both superseded**, the median/MAD framing on the operator's own correct
objection (recency-weighting matters, a plain windowed median throws it away for no demonstrated
benefit — price itself doesn't have Force Index's multiplicative-heavy-tail mechanism, and the
calendar-clock MACD fragility claim was never empirically validated, only assumed by analogy); (3)
**adopted**: a Gemini literature-consult's own "Imbalance Clock Information Physics" reframing
(`lbrnet/logs/rc_gemini.log`, starting line 7515), which reframes MACD/Force Index/3-10 Oscillator/
Chandelier Exit as physical operators (Phase Velocity, Kinetic Energy, Thermodynamic Dissipation) on
the imbalance clock τ, and Elder's Impulse System as a "Phase Coherence State."

**Design, as agreed**:
- **Signal** (`Int8`, 3-state, entropy checked FIRST — overrides price direction, not a bolt-on
  filter on top of it):
  ```
  dH_norm/dτ > 0                      -> BLUE  (turbulent/dissipation, 0)
  dH_norm/dτ ≤ 0  and  dP/dτ > 0      -> GREEN (coherent bullish, +1)
  dH_norm/dτ ≤ 0  and  dP/dτ < 0      -> RED   (coherent bearish, -1)
  ```
  Simpler than an earlier, since-dropped proposal (macro drift AND cross-scale `IS1`/`IS3`
  agreement, separately gated by entropy) — Gemini's 2-variable framing (price slope + entropy
  trend) folds momentum and regime-quality into the same decision tree rather than treating entropy
  as a secondary modifier.
- **Quality** (`Float32`): magnitude of `dP/dτ` or a combined coherence-strength score — exact
  formula not yet finalized, secondary to the signal logic above.
- **Divergence, exact structural definition (Gemini's own formulation)**:
  $$P(\tau_2) > P(\tau_1) \ \text{while}\ v_{phase}(\tau_2) < v_{phase}(\tau_1) \ \text{and}\ dH_{norm}/d\tau > 0$$
  — explicitly named by Gemini as "Thermodynamic Exhaustion...a structural `TRAP_LONG`," not a
  standalone divergence flag.
- **Placement, agreed 2026-09-07**: lives in `ImbalanceIndicatorManager` as a real indicator (not
  folded entirely into TRAP's own internals) AND is separately read by the native TRAP framework as
  an additional anticipatory input — matching TRAP's existing governance (`CLAUDE.md`: native
  `StructureTest` floor stays authoritative; a model/indicator signal may LEAD, never SUPPRESS the
  floor). The divergence sub-case specifically feeds TRAP; the broader 3-state classification is the
  general-purpose indicator (mirrors how calendar-clock MACD-Histogram divergence feeds
  `Scoring.cpp` while the same histogram also feeds Impulse's bar-coloring — one construct, two
  consumers).
- **Honest, unresolved caveat, flagged critically not silently accepted**: Gemini's own Force Index
  reframing in the same consult, $W_\tau = \theta_\tau \cdot Y_{imb}(\tau)$, appears to algebraically
  reduce to plain `ΔP` if $Y_{imb} = \Delta P/\theta$ (Track 2's already-tested formula) — a possible
  redundancy/tautology, structurally similar to the exact question `Y_imb` was already tested and
  closed out for. Not yet resolved, not this candidate's blocker (unrelated to MACD/Impulse), but
  should not be silently trusted either if Force Index-on-imbalance-clock is ever revisited.

### 1.3b Prerequisite gap, CONFIRMED 2026-09-07: no activity-clock-native Shannon entropy exists yet

**IMPLEMENTED 2026-09-07** (`include/ImbalanceContextManager.h` -- `MindfulTrader::InformationEngine
m_entropyEngine` member, `GetShannonFlowEntropy()`/`GetShannonEfficiency()`/`BuildRiskGateContext()`;
`include/LBRFileManager.h`/`.cpp` -- `LogImbalanceContext()` now takes an optional
`const MTS::Schema::ImbalanceRiskGateContextT*`, mirroring `LogContext()`'s existing convention;
`src/ImbalanceEventDataCollectorStudy.cpp` now builds and passes it. 21/21 native tests pass,
`tests/cpp/test_imbalance_context_manager.cpp`; `./build_dll.sh --no-clean` clean.) All design
decisions below shipped exactly as resolved, with one implementation-level addition not previously
called out: `kImbalanceShannonMaxEntropyBits` (= log2(10) bits) is a deliberate, commented
DUPLICATE of `ContextManager.h`'s own `kShannonMaxEntropyBits` constant, not a shared include --
including `ContextManager.h` would pull in `sierrachart.h` and break this header's ACSIL
independence.

**Checked directly, not assumed** — `docs/superpowers/specs/2026-08-26-activity-clock-tail-risk-
and-decay-spec.md` §4b and §5b both state explicitly, independently: "its feed (`InformationEngine`,
via `UpdateMarketPhysics()`) is already tick-native, so the staleness argument that motivated
kurtosis doesn't apply here. Whether the entropy *estimator itself* (not its feed cadence) would
still benefit from activity-clock windowing is a different, still-open question — **not examined**."

**What exists today** (`include/InformationEngine.h`, `shannon_flow_entropy`/`shannon_efficiency`,
Miller-Madow-bias-corrected, `NUM_BINS=10` SAX discretization) is a *third*, distinct clock from
either calendar bars or imbalance bars: a continuous streaming engine fed via
`ContextManager::UpdateMarketPhysics(logReturn)`, called on every raw tick-level price CHANGE
(`EventDataCollectorStudy.cpp`'s `sc.Close[sc.Index] != s_lastPhysicsPrice` guard), never gated to
any bar-close boundary at all — calendar or imbalance. That's a genuinely different clock than what
`dH_norm/d\tau` needs: entropy sampled AT imbalance-bar boundaries (τ = completed imbalance-bar
index), the exact same treatment `hurst_exponent`/`skewness_idx`/`recurrence_rate`/`taleb_kurtosis`
already received on their own path to `IS1`/`IS2`/`IS3`.

**Proposed design, reuse not reinvent (matching this repo's own established precedent for every
prior activity-clock dim)**: reuse `InformationEngine`'s existing `AddObservation()`/
`GetShannonEntropy()` machinery verbatim (same Miller-Madow correction, same SAX binning — nothing
about the estimator itself needs to change), but feed it completed `IS`-level returns (via
`ImbalanceClockManager::GetIs1Returns()`/`GetIs2Returns()`/`GetIs3Returns()`) instead of continuous
tick-level price changes — the same "same formula, new clock" pattern already used for
`fast_hurst_exponent`/`fast_taleb_kurtosis`.

**Not yet decided, real open questions:**
- ~~Which `IS` level(s) should this activity-clock entropy be computed at?~~ **RESOLVED
  2026-09-07: `IS2`.** Checked directly, not assumed: `TripleScreen2.cpp` (`scsf_Screen2_MACD`,
  `IndicatorKey::INTERM_MACD`/`INTERM_MACD_DIVERGENCE` — "INTERM" = intermediate = TS2, confirmed
  via direct grep) is where calendar-clock MACD, MACD-Histogram divergence detection, AND the
  Impulse System coloring all already live — real, existing screen-ownership precedent, not a
  borrowed analogy. Since §1.3a's Gang-MACD/Phase-Coherence indicator computes `dP/dτ` (a MACD/
  phase-velocity analog) and needs `dH_norm/dτ` on the SAME `τ` (same denominator, so the two
  derivatives are commensurable), the entropy engine must sample at the same imbalance-bar cadence
  the indicator itself will use — `IS2`, matching where `dP/dτ` will be measured, not `IS1`/`IS3`.
- ~~A brand-new engine instance, or reuse `ImbalanceContextManager`'s own state?~~ **RESOLVED
  2026-09-07: a new, dedicated `MindfulTrader::InformationEngine` instance, placed the same way
  `RecurrenceRateEngine` already is** — a private member of `ImbalanceContextManager`, not a
  separate component/singleton. Matches §1.2d's own established precedent exactly: `recurrence_rate`
  already needed its own dedicated engine + bar-count cache (a genuinely different-in-kind concern
  from the 3 stateless pure-function dims) rather than forcing a false-generic abstraction — entropy
  is the same shape of exception, not a new kind of one.
- **Feed cadence — RESOLVED 2026-09-07, and this is the one genuinely new wrinkle relative to every
  prior activity-clock dim**: `InformationEngine::AddObservation()` is inherently INCREMENTAL (an
  EMA-based rolling sigma estimate + ring-buffer histograms that advance their own head index on
  every call) — unlike `DfaHurstExponent`/`RobustMoments`/`ComputeImbalanceRecurrenceRate`, which
  all recompute from scratch over a freshly-drained snapshot array every time they're called. Naively
  calling `GetIs2Returns(100, ...)` and feeding all 100 returns into `AddObservation()` every tick
  would re-insert the same historical returns repeatedly, corrupting the ring buffer/histogram
  state (each call is treated as a genuinely NEW sample). The fix: feed exactly ONE observation per
  NEWLY completed `IS2` bar — same `completedBarCount != lastFedBarCount` change-detection already
  used for `recurrence_rate` (§1.2d), but call `GetIs2Returns(1, out)` (the single most recent
  completed return) instead of a 100-wide snapshot, and call `AddObservation()` exactly once per
  genuinely new bar close — mirroring `ContextManager::UpdateMarketPhysics()`'s own existing
  "exactly one call per genuine underlying event" discipline (its own tick-level price-change guard
  in `EventDataCollectorStudy.cpp`), just re-grounded on `IS2` bar closes instead of tick-level price
  changes.
- **Output routing — RESOLVED 2026-09-07**: `shannon_flow_entropy`/`shannon_efficiency` feed
  `ImbalanceRiskGateContext` (§1.5a), NOT `ImbalanceObservationData`'s 4-dim vector — matches where
  these two fields were already scoped into the schema. `shannon_efficiency` is derived the same way
  `ContextManager.cpp` already does it (`1 - H/Hmax`, `Hmax = log2(NUM_BINS) ≈ 3.322` bits) — no new
  formula, same reuse-not-reinvent discipline as the entropy estimator itself.
- **`dH_norm/dτ` itself is NOT this entropy engine's job** — the engine only needs to expose
  `GetShannonEntropy()` (or a normalized `H/Hmax` variant) per `IS2` bar close; computing the
  first difference across bars (the actual derivative §1.3a's indicator consumes) is the Gang-MACD/
  Phase-Coherence indicator's own responsibility, inside `ImbalanceIndicatorManager` (§1.3, not yet
  built) — keeps the entropy engine a single-responsibility state holder, not a derivative
  calculator, matching this repo's own "keep the generic part generic, keep real heterogeneity
  concrete" lesson (§1.2d).
- **Not yet empirically validated on real imbalance-bar data** — same discipline as every other
  dim this session: literature-grounded reuse of a proven estimator is not itself a validation that
  it produces a meaningful signal on THIS clock, at whatever window size gets chosen.

**Still NOT YET IMPLEMENTED** (this section resolves the DESIGN, not the code) — real next step is
adding an `MindfulTrader::InformationEngine m_entropyEngine` member (+ `std::size_t
m_entropyLastBarCount`) to `ImbalanceContextManager`, feeding it from the `IS2` branch of `Update()`
(alongside `recurrence_rate`'s own bar-close-gated compute), and exposing the result for whatever
populates `ImbalanceRiskGateContext` — blocked on `ImbalanceIndicatorManager`'s own build (§1.3)
being the actual consumer that would prove this design correct, per this initiative's own
"producer before consumer" discipline stated at the top of §1.3.

**This is the actual next concrete step for §1.3a's Gang-MACD/Phase-Coherence candidate** — not
further design brainstorming on the indicator itself, which is otherwise agreed.


**RESOLVED 2026-09-06: share the EXISTING `IndicatorPackedState`/`IndicatorLayout` arrays — no
separate `ImbalanceIndicatorPackedState`.** Unlike §1.5's `ObservationData` question, this is NOT
the same kind of coupling risk: the packed array is a purely in-PROCESS runtime cache (`IndicatorKey`
-enum-indexed float/int8 arrays), not itself a wire contract — indicator values get copied OUT into
whichever wire struct is appropriate (`ObservationData` vs. `ImbalanceObservationData`, already
independent per §1.5) at serialization time, so sharing the cache underneath doesn't re-couple the
two clock domains' actual wire schemas. Two considerations make sharing clearly correct here, where
they didn't for §1.5:

- **Mutual exclusivity (§1.8) removes the main correctness risk.** `SCStudies.cpp` and
  `SCImbalanceStudies.cpp` are never both live in the same session — only one side's `IndicatorKey`
  entries are ever actually populated with real values at a time; the other side's slots simply sit
  at their default/zero state for that session. There is no scenario where both sets of values are
  needed simultaneously in one running process, so there is nothing for a shared array to get wrong.
- **Reuses a proven, already-DOD-hardened mechanism** rather than duplicating the hot-path
  devirtualized read code (`CheckTrigger`, `PopulateIndicatorState`, `GetTrainingEventT`,
  `EventSerializer`) a second time — a real, ongoing maintenance cost this repo's own permanent-
  hybrid-architecture framing (`CLAUDE.md`) already went out of its way to avoid duplicating once;
  doubling it for something explicitly TRANSITIONAL (§0) would be the wrong trade.

**One real implementation detail this decision creates, not yet designed**: new `IndicatorKey`
enum entries for imbalance-only indicators need to be clearly grouped/namespaced (e.g. a contiguous
range) so that any code which iterates "all keys" for calendar-clock-specific purposes (HUD display
loops, warm-up/readiness checks, dirty-mask default state) can trivially skip the imbalance-only
range when running under `SCStudies.cpp`, and vice versa under `SCImbalanceStudies.cpp` — not
designed here, but now a concrete, scoped follow-on rather than an open architecture question.

**Adjacent gap noticed while reasoning through this, not yet addressed anywhere in this doc**:
does the offline comparison workflow (§1.6's scope note) need an `ImbalanceBackTesterStudy.cpp`
counterpart to `BackTesterStudy.cpp` (which drives its own PositionManager/RiskManager orchestration
directly, independent of `SCStudies.cpp`), or can the existing `BackTesterStudy.cpp` be parameterized
to run against `ImbalanceScreen1/2/3.cpp`-produced indicators instead? **Lower priority than it
looks, per §0a**: `BackTesterStudy.cpp` itself has never been run, ever — designing a counterpart
to (or a parameterization of) code that has never been exercised even once on the system it
already targets would be building on an unverified foundation. Not designed, not even scoped —
flagged here so it isn't lost, but §0a's validation must happen first regardless of which way this
question resolves.

### 1.4 `ImbalanceContextManager` (new)

Mirrors `ContextManager`'s role (assembling `ObservationData`/`LocalRiskContext`/triggering HMM
inference) but reading from 1.3 to assemble the new `ImbalanceMarketObservation`/
`ImbalanceTrainingEvent` FlatBuffer tables (siblings of `MarketObservation`/`TrainingEvent`,
`mts_schema.fbs`, not yet added — see §1.5).

**RESOLVED 2026-09-06 (operator directive): TWO distinct HMMs during the transition, not one
unified model.** One legacy calendar-clock HMM (today's model, unchanged, trained on
`ObservationData`/`MarketObservation`) runs alongside one new activity-clock HMM (trained on
whatever `ImbalanceMarketObservation` turns out to contain, §1.5) — genuinely parallel, not a
fallback/primary split. Rationale:

- **Enables the actual comparison this migration exists to make.** The point of this whole
  initiative is to validate whether the imbalance clock outperforms the calendar clock before
  cutting over (per §0's own "migration target, not permanent addition" framing already applied to
  every other twin construct this session — Force Index Track 2, `hurst_exponent`/
  `fast_hurst_exponent`). A single unified model trained from day one would make it impossible to
  attribute any performance change to the clock domain vs. the retrain itself.
- **Avoids forcing §1.5's still-open question early.** A single HMM requires a single, fixed
  observation-vector shape — committing to that now would silently pre-decide "does
  `ImbalanceMarketObservation` reuse `ObservationData`'s shape or not" before that question has
  actually been reasoned through. Two independent models let each clock domain's vector evolve on
  its own schedule.
- **Matches this session's own established validate-before-merging discipline** — the same posture
  already used for `ImbalanceBarEngine`'s own extension (offline tooling before touching the
  production engine) and `Y_imb`'s closure (real-data-tested before any wiring decision), scaled up
  to model architecture.
- **Real, acknowledged cost, not free**: this duplicates training compute and retraining cadence
  for as long as the transition lasts, and `HMMClient` needs day-one model-routing awareness (see
  §2's updated bullet below) — a genuine new engineering task, not a config toggle. This is
  explicitly TRANSITIONAL: once the activity-clock HMM is validated and cut over, the legacy
  calendar-clock HMM gets deleted outright per this repo's own standing pre-production rule
  (`CLAUDE.md`: "the default is deletion, not preservation") — not kept as a permanent second model.
- **Not yet resolved by this decision, RETIRED 2026-09-06**: whether the two HMMs share ZMQ port
  5561 (model-selector field) or each gets a dedicated port. **Operator clarification: `SCStudies
  .cpp` and `SCImbalanceStudies.cpp` (§1.8) are mutually exclusive — never instantiated together,
  ever; §1.8 is a sequential REPLACEMENT for `SCStudies.cpp`, not a concurrent twin.** This means
  live inference is NEVER concurrent between the two models — whichever chartbook/study is active,
  Python's HMM server is configured to serve just that one model, the same sequential
  swap-which-artifact-is-loaded pattern `BackTesterStudy.cpp`'s `IN_MODEL_ARTIFACT_PATH` input is
  CODED to use for offline comparison runs (per §0a: coded, never actually run — this reasoning
  still holds regardless, since it's about the ABSENCE of any need for live dual-model routing, not
  a claim that the artifact-path mechanism itself is proven). No live routing mechanism (port or
  selector field) is needed at all — this whole question is retired, not merely re-leaned.

### 1.5 Schema: `ImbalanceMarketObservation` / `ImbalanceTrainingEvent`

New FlatBuffer tables in `schema/mts_schema.fbs`, siblings of (not replacements for)
`MarketObservation`/`TrainingEvent`. A real schema-versioning event — this repo's own
`WIRE_SCHEMA_VERSION` is held fixed at 240 through the current observation-vector campaign until it
ships to `lbrnet` (`mts_schema.fbs`'s own versioning note) — adding two new top-level tables needs
its own version-bump decision and `regenerate_schema.sh` run, not a casual field add.

**RESOLVED 2026-09-06: a distinct new struct (working name `ImbalanceObservationData`), not a
literal reuse of `ObservationData`'s type — but architecturally identical in PATTERN.** Two
considerations pull in different directions, and the resolution takes the stronger point from
each rather than picking one side wholesale:

- **For reusing the pattern (not the type)**: this repo already has a direct, proven precedent for
  "activity-clock version of a dim lives as another field in the SAME flat struct" —
  `fast_taleb_kurtosis` was added as `ObservationData`'s 17th field directly, in place, not via a
  side table (`skewness_idx` was later repointed to source from the activity-clock buffer the same
  way, no schema change). A flat, contract-constant-driven float struct (`kObservationDim`-style,
  with a generated field-name array) is exactly the shape every piece of downstream tooling
  (`context_reader.h`, `context_to_parquet.cpp`, `context_validate.cpp`, `FeatureScaler.h`) is
  already built generically against — reusing that PATTERN lets an `ImbalanceFeatureScaler`/
  imbalance `context_reader.h` equivalent be a near-direct adaptation of proven, already-hardened
  code, not a from-scratch design.
- **Against reusing the literal `ObservationData` TYPE**: §1.4's own two-HMM rationale already
  established that the two clock domains' vectors must be free to "evolve on its own schedule" —
  sharing one literal struct type would silently couple them again: an imbalance-clock-only field
  added to a shared `ObservationData` would leak an unused/meaningless field into the calendar-
  clock `MarketObservation` table (and vice versa), and any future calendar-clock schema change
  (e.g. this session's own `vol_convexity` removal, 19D→18D) would force a version-bump decision
  on the imbalance side too, even though nothing about it actually changed. This is precisely the
  coupling the two-HMM decision was written to avoid — a shared struct type would reintroduce it
  at the schema layer even after avoiding it at the model layer.
- **Net decision**: a new, structurally independent `ImbalanceObservationData` struct + its own
  `kImbalanceObservationDim` contract constant + its own generated field-name array, built by
  copying `ObservationData`'s existing 19-field starting point (or whatever the current calendar-
  clock count is at the time this is implemented) as the initial content — not because the two
  must stay identical, but because that starting point is a reasonable, already-validated seed,
  free to diverge immediately afterward.

**RESOLVED 2026-09-06 (operator directive): dim inclusion is CASE-BY-CASE, not a blanket "recompute
everything natively" or "start narrow" default.** Each of `ObservationData`'s current fields gets
its own explicit include/exclude decision for `ImbalanceObservationData`, the same discipline
already used for the calendar-clock vector's own Gaussian-moment-audit/elite-feature-set-curation
process — no dim is assumed to belong here just because it exists on the calendar-clock side.

**Naming convention, RESOLVED 2026-09-06: drop the `fast_` prefix inside the new struct.** The
prefix exists on the calendar-clock side specifically to distinguish an activity-clock variant
living ALONGSIDE its calendar-clock counterpart in the SAME shared `ObservationData` struct
(per `docs/superpowers/specs/2026-09-06-observation-vector-gang-statistical-reformulation-
initiative.md`) — inside `ImbalanceObservationData`, that distinction is meaningless (EVERY field
is activity-clock-native by construction, there is no calendar-clock sibling to distinguish from
in the same struct), so carrying the prefix forward would be pure noise. Concretely, of the 5 dims
already on the activity clock today: `fast_hurst_exponent` → `hurst_exponent`, `fast_taleb_kurtosis`
→ `taleb_kurtosis`, `fast_mean_rev_z` → `mean_rev_z` (all three drop the prefix); `skewness_idx`/
`recurrence_rate` are unchanged (no prefix to drop). This naming rule applies to whichever dims the
case-by-case process above actually selects, not just these 5 — any calendar-clock `fast_*` field
that gets included drops the prefix in its `ImbalanceObservationData` name.

### 1.5a Schema: `ImbalanceRiskGateContext` — RESOLVED 2026-09-07

A new sibling FlatBuffer table, `ImbalanceRiskGateContext`, embedded as a field inside
`ImbalanceMarketObservation` (`risk_gate_context: ImbalanceRiskGateContext;`, alongside the
existing `observation: ImbalanceObservationData;` field). Mirrors the calendar-clock precedent
(`MarketObservation.risk_gate_context: RiskGateContext`) in PATTERN — a distinct struct, not a
literal reuse of `RiskGateContext`'s type, for the same two-clock-domain independence reasons
§1.5 already established for `ImbalanceObservationData` vs. `ObservationData`.

**Field selection, RESOLVED (operator directive: "make it as comprehensive as practical now, so
yes add them. We will grow it as we go"): comprehensive but grounded, not a blanket copy of all 17
`RiskGateContext` fields.** Only dims with a REAL activity-clock version that already exists or is
actively being designed were included:

```flatbuffers
table ImbalanceRiskGateContext {
  shannon_flow_entropy  : float = 0.0;
  shannon_efficiency    : float = 0.5;
  hurst_exponent        : float = 0.5;
  skewness_idx          : float = 0.0;
  taleb_kurtosis        : float = 1.23;
  is_valid              : bool  = false;
  snapshot_timestamp_us : long  = 0;
}
```

- `hurst_exponent` (IS1), `skewness_idx`/`taleb_kurtosis` (IS3) — already computed dims, already
  present in `ImbalanceObservationData` (§1.5).
- `shannon_flow_entropy`/`shannon_efficiency` — not yet computed; the entropy engine itself is
  still a prerequisite gap per §1.3b (no activity-clock-native Shannon entropy exists yet). Added
  to the schema now per the operator's "grow it as we go" comprehensiveness directive, ahead of the
  engine's own implementation — the field exists in the wire contract before the producer does,
  same relationship `taleb_kurtosis`/`skewness_idx` already have to their own not-yet-centralized
  compute path (§1.2c/§1.2d not yet implemented in `ImbalanceContextManager`).
- `is_valid`/`snapshot_timestamp_us` — generic bookkeeping, mirrors `RiskGateContext`'s own fields
  of the same name.
- **Deliberately EXCLUDED** (no imbalance-clock analog designed yet): `elder_chandelier_atr`,
  `pareto_tail_alpha`, `amihud_illiquidity`, `spread_stress`, `fractal_dim`, `mean_rev_z`,
  `raschke_burst`, `fisher_info`, `regime_duration`, `amihud_percentile`.
- **Naming**: `skewness_idx`, matching `ImbalanceObservationData`'s own field name — not
  `RiskGateContext`'s inconsistent alternate name `taleb_skewness`.

No `WIRE_SCHEMA_VERSION` bump required — purely additive new tables, not a modification of the
existing 18D `ObservationData`/`MarketObservation`. Regenerated via `regenerate_schema.sh
--cpp-only` and build-verified clean (`./build_dll.sh --no-clean`) 2026-09-07. A pre-existing,
unrelated dead-code build failure (`m_readyMask`, an unused private field left over from the
superseded §1.2b bitmask-readiness design) was caught and fixed by the same build pass — see
§1.2b's own "superseded" note.

**Not yet done**: no C++ code populates this table yet — needs a new method on
`ImbalanceContextManager` (or wherever §1.2c's centralized `Update()` lands) to build an
`MTS::Schema::ImbalanceRiskGateContext` instance, and a corresponding `LBRFileManager` emission
path. Blocked on the same §1.2c/§1.2d/§1.3b prerequisites already blocking `ImbalanceObservationData`
population.

### 1.6 `ImbalanceEventDataCollectorStudy.cpp` (new)

Mirrors `EventDataCollectorStudy.cpp`'s role exactly (arm/disarm ACSIL study, 5-lock readiness
gate, `HasSignificantChange()`-triggered writes) but for the imbalance clock — the concrete
data-collection component behind the offline HMM/Transformer comparison workflow (operator
discussion, 2026-09-06): writes `.imbalance.context`/`.imbalance.alpha` sibling files, following
the exact naming convention `LBRFileManager::Open(path, symbol)` already establishes for
`.context`/`.alpha` (`path + ".context"`/`path + ".alpha"`, `src/LBRFileManager.cpp`).

- **Depends on** §1.1 (`ImbalanceScreen1/2/3.cpp`, in place of `TripleScreen1/2/3.cpp`), §1.3
  (`ImbalanceIndicatorManager`, in place of `IndicatorManager`), §1.4 (`ImbalanceContextManager`,
  in place of `ContextManager`, including whichever of the two parallel HMMs it triggers), and
  §1.5's schema decision (what `ImbalanceMarketObservation`/`ImbalanceTrainingEvent` actually
  contain) — cannot be built before those resolve.
- **RESOLVED 2026-09-06: extend `LBRFileManager` with new sibling methods** (`LogImbalanceContext`/
  `LogImbalanceAlpha`), not a dedicated sibling class. One `Open(path, symbol)` call producing all
  four streams (`.context`/`.alpha`/`.imbalance.context`/`.imbalance.alpha`) together keeps them
  trivially aligned to the same collection run and symbol by construction — a dedicated sibling
  class would have to duplicate `Open`'s own file-open/magic-header/FileMetadata-write logic and
  reintroduce exactly the alignment risk (two independently-opened files silently drifting to
  different runs) this decision avoids for free. Matches §1.3's `ImbalanceIndicatorManager`
  resolution in spirit: reuse a proven, already-hardened mechanism rather than duplicate it, for
  something explicitly transitional (§0). Real follow-on, not yet designed: `LogImbalanceContext`/
  `LogImbalanceAlpha`'s exact signatures depend on §1.5's `ImbalanceObservationData` struct
  actually being defined (schema work, not yet done) — this decision fixes WHERE the writer lives,
  not its field-level shape.
- **RESOLVED 2026-09-06: Lock D/E gate on events/bars-since-last-update on the imbalance clock,
  NOT wall-clock age — but the existing calendar-based Market-Closed Gate (session/day-of-week
  check, `EventDataCollectorStudy.cpp`'s own earlier, separate check) stays unchanged either way.**
  These are two genuinely separate concerns that were conflated in the open-question framing: (1)
  "is the market known to be closed right now" is a calendar/session fact regardless of which bar-
  construction scheme downstream code uses — keep it as-is, unmodified, ahead of Lock D/E in both
  collectors; (2) "has the upstream screen (IS1/IS2) unexpectedly stopped producing bars while the
  market IS open" is what Lock D/E itself measures, and THAT concept should be imbalance-clock-
  native (events/bars since IS1/IS2 last updated, not wall-clock hours) — a fixed wall-clock
  threshold is actively wrong here, since imbalance bars form at variable real-time rates by
  construction (the whole point of this migration). **Not yet decided, a real follow-on**: the
  actual numeric threshold (analogous to today's 6h/3h) needs empirical derivation against real
  tick data once `ImbalanceBarEngine`'s multi-frame extension (§1.2) exists — not guessed, matching
  this repo's own `fractal_dim` block-length-derivation precedent, not invented here.
- **Feeds whichever HMM §1.4's two-model decision assigns to the imbalance clock** — no live
  routing mechanism needed at all (§1.4's ZMQ-routing question is retired, not just leaning toward
  a specific answer).
- **Scope note**: this doc records the component and its dependencies only — the actual offline
  comparison workflow (train both HMMs/Transformers on their respective `.context`/`.alpha` vs.
  `.imbalance.context`/`.imbalance.alpha` artifacts, run both through `BackTesterStudy.cpp`'s
  Phase 2/3 harness via its `IN_MODEL_ARTIFACT_PATH`/`IN_TRANSFORMER_ARTIFACT_PATH` inputs, compare
  `.btst` `RunSummary` results under a pre-registered walk-forward split before any live cutover
  decision) is a separate, later step — not designed in detail here. **Per §0a: this entire
  workflow depends on `BackTesterStudy.cpp` actually working, which has never been verified even
  once on the calendar-clock system it already exists for** — that verification is a prerequisite
  to this workflow, not a detail within it.

### 1.7 Live production wiring — `SCStudies.cpp` (deferred, explicit sequencing constraint)

`SCStudies.cpp` is the main LIVE ACSIL entry point (`CLAUDE.md`'s Data Flow diagram) — calls
`TripleScreen1/2/3`, `IndicatorManager::UpdateBarContext()`, `ContextManager`, `PositionManager::
Update()`, `RiskManager::Evaluate()`, `TradeSignalManager::GenerateSignals()`, `EventSerializer::
PublishEvent()` every tick. **Nothing in §1.1-1.6 touches it, by design** — `ImbalanceScreen1/2/3
.cpp` and `ImbalanceEventDataCollectorStudy.cpp` are separate ACSIL study files driven by REPLAY,
matching `EventDataCollectorStudy.cpp`'s own existing replay-only collection role. This section
exists to make that omission an explicit decision, not an oversight.

- **Explicit sequencing constraint**: `SCStudies.cpp` must NOT gain live imbalance-clock trading
  logic until AFTER §1.6's scope-note workflow (offline HMM/Transformer comparison, pre-registered
  walk-forward split) favors the imbalance-clock model. Wiring live trading logic ahead of that
  validation would violate the exact discipline this whole initiative is built on — validate before
  merging, per `Y_imb`'s own closure precedent (real-data-tested and found wanting before any
  wiring was attempted) — and would make the walk-forward/pre-registered-criterion caveat from the
  2026-09-06 operator discussion meaningless in practice.
- **Once validated**: cutover is a discrete SWAP, not a merge — activate `SCImbalanceStudies.cpp`
  (§1.8)'s chartbook for live trading and stop using `SCStudies.cpp`'s, per the operator's own
  mutual-exclusivity clarification (§1.8). `SCStudies.cpp` then gets deleted outright per this
  repo's own standing pre-production rule (`CLAUDE.md`: "the default is deletion, not
  preservation") — it is never modified in place to add imbalance-clock calls alongside its
  existing ones.
- ~~Whether an interim "shadow mode" is worth building~~ — **RETIRED 2026-09-06**: shadow mode
  requires both studies live simultaneously (one driving, one observing); the operator's mutual-
  exclusivity clarification (§1.8) rules this out entirely, not just as a design preference.
  Validation is OFFLINE-ONLY, via §1.6's collection workflow and the backtested comparison in its
  scope note — there is no live parallel-run comparison mode at any point before cutover.

### 1.8 `SCImbalanceStudies.cpp` (new) — the eventual replacement for `SCStudies.cpp`, not a concurrent twin

**Operator clarification, 2026-09-06**: `SCStudies.cpp` and `SCImbalanceStudies.cpp` are mutually
exclusive — never instantiated together, ever. `SCImbalanceStudies.cpp` is structurally the
imbalance-clock counterpart of `SCStudies.cpp` (which defines `scsf_MindfulTrader` — the
execution/orchestration study calling `PositionManager`/`RiskManager`/`TradeSignalManager`/
`EventSerializer` — distinct from `TripleScreen1/2/3.cpp`'s per-timeframe indicator computation
role, which §1.1's `ImbalanceScreen1/2/3.cpp` mirrors instead), built and validated OFFLINE (§1.6's
collection workflow + the backtested comparison in its scope note) while `SCStudies.cpp` keeps
running live, completely unaffected — then activated at cutover by a discrete SWAP (start using
`SCImbalanceStudies.cpp`'s chartbook for live trading, stop using `SCStudies.cpp`'s), never a
concurrent run of both. `SCStudies.cpp` is deleted outright once cutover is confirmed, per this
repo's standing pre-production rule.

**Consequence of mutual exclusivity, not concurrency**: because the two are never instantiated
together, there is no concurrent-process, shared-singleton, or live-dual-model-routing concern to
design around — `TransportStream`/`HMMClient`/`PositionManager`/`RiskManager` are reused
SEQUENTIALLY (whichever study is active owns them for that period), not shared under concurrent
load. This is what retires §1.4's live-routing question and §1.7's shadow-mode question above —
neither scenario they were designed to address can actually occur.

- **Optional, not required**: if a live-forward-test/paper-trading period on real feed is ever
  wanted before fully committing to cutover (still sequential — `SCStudies.cpp` would be
  deactivated first), this repo already has a working isolation precedent for exactly that kind of
  test: `BackTesterStudy.cpp`'s `IN_ISOLATION_MODE` (`dedicated_sim_account`/`scoped_clear`). Not
  currently planned, just noted as available if the operator wants it.

## 2. Existing singletons that likely do NOT need an "Imbalance" twin (tentative, not fully verified)

Reasoned from each singleton's actual role (`CLAUDE.md`'s Key Singletons table) — these are
consumers or transport layers, not clock-specific producers, so they should keep working unchanged
regardless of which context/indicator manager feeds them:

- **`PositionManager`** — consumes `LocalRiskContext`/`ObservationData` for gating; doesn't itself
  compute anything clock-specific. Continues reading from whichever context manager is authoritative
  during/after the transition.
- **`RiskManager`** — same reasoning; consumes `RiskGateContext`, doesn't produce clock-specific data.
- **`TransportStream`/`HMMClient`/`SystemOrchestrator`/`TradeExecutionServer`** — protocol/transport
  layers, serialize and send whatever `MarketObservation`/`ImbalanceMarketObservation` gets built;
  largely clock-agnostic. **`HMMClient` needs NO live model-routing awareness, RETIRED 2026-09-06**:
  `SCStudies.cpp`/`SCImbalanceStudies.cpp` (§1.8) are mutually exclusive, never instantiated
  together, so `HMMClient` is only ever serving ONE model at a time in practice — whichever study
  is active. No port or selector-field decision is needed (see §1.4's retired bullet).

**This list is tentative** — flagged as a starting position, not a verified conclusion; each of
these should get its own explicit check once `ImbalanceContextManager`/`ImbalanceIndicatorManager`
are actually being designed in detail, not assumed clean now.

## 3. Cross-references

- `src/EventDataCollectorStudy.cpp` / `include/LBRFileManager.h` — the existing calendar-clock
  data collector and its `.context`/`.alpha` writer that §1.6's `ImbalanceEventDataCollectorStudy
  .cpp` mirrors; ground truth for the `.imbalance.context`/`.imbalance.alpha` naming convention.
- `src/BackTesterStudy.cpp` — its `IN_MODEL_ARTIFACT_PATH`/`IN_TRANSFORMER_ARTIFACT_PATH` inputs
  are the mechanism §1.6's own scope note assumes for the eventual offline HMM/Transformer
  comparison, once both `.context`/`.alpha` and `.imbalance.context`/`.imbalance.alpha` exist —
  **but see §0a: this file has never been run, ever, so this is a dependency on unverified code,
  not a proven mechanism** — must be validated on the calendar-clock system first.
- `docs/superpowers/specs/2026-09-06-imbalance-work-rate-spec.md` (Track 2, RESOLVED/retired as
  an alpha candidate, retargeted to the risk-gating initiative) — Force Index Track 1 (calendar-
  clock hardening) was abandoned outright 2026-09-07 (operator directive: not worth further token
  spend) and its spec deleted, not just closed — do not resurrect without a fresh operator ask.
  The first concrete indicator this architecture would eventually host, once `ImbalanceScreen2.cpp`
  (Force Index's own Screen 2 role) exists.
- `docs/superpowers/specs/2026-09-06-observation-vector-gang-statistical-reformulation-initiative.md`
  — the 5 dims already living on `ImbalanceBarEngine` today (via `ContextManager`'s activity-clock
  block, a transitional shortcut per §1.1 above), the starting inventory for whatever
  `ImbalanceContextManager` eventually assembles.
- `docs/superpowers/specs/2026-09-06-labeling-data-augmentation-gang-statistical-reformulation-
  initiative.md` §2.1/§1a — the adaptive-threshold gap (§1.2 above) and the wider possible
  Triple-Barrier replacement question, both relevant to how far this migration ultimately reaches.
- `docs/superpowers/specs/2026-09-06-risk-gating-gang-statistical-reformulation-initiative.md` —
  candidate risk-gate signals (`Δ_HS`/`D_KL`) that would need to read from whichever
  context manager (`ContextManager` or `ImbalanceContextManager`) ends up authoritative.

## 4. Next steps (not started)

0. **TOP PRIORITY, surfaced 2026-09-06 (§0a) — outranks everything below**: run `BackTesterStudy
   .cpp` (Phase 2 and/or 3) for the first time ever, on the EXISTING calendar-clock system, and
   confirm it actually produces a usable `.btst`/`RunSummary`. Every other item in this list that
   references the offline comparison workflow (items 5-6 indirectly, §1.6's scope note, §1.3's
   `ImbalanceBackTesterStudy.cpp` gap) assumes this works — that assumption is currently unverified.
   Blocks nothing else structurally, but should be done before investing further in the
   comparison-workflow side of this migration.
1. ~~Resolve §1.4's "one HMM or two during transition" question~~ — **RESOLVED 2026-09-06: two.**
   See §1.4 for the decision (its ZMQ-routing follow-on is separately retired, item 4 below).
2. ~~Resolve §1.3's "shared vs. separate packed array" question~~ — **RESOLVED 2026-09-06: share
   the existing `IndicatorPackedState`/`IndicatorLayout` arrays.** See §1.3 for the decision and
   its two real follow-ons (`IndicatorKey` range namespacing; the newly-noticed possible
   `ImbalanceBackTesterStudy.cpp` gap).
3. ~~Resolve §1.5's "reuse `ObservationData` shape vs. new struct" question~~ — **RESOLVED
   2026-09-06: new distinct struct (`ImbalanceObservationData`), reusing the tooling PATTERN not
   the literal type; dim inclusion is case-by-case, `fast_` prefix dropped inside the new struct.**
   See §1.5 for the full decision.
4. ~~Resolve §1.4's ZMQ-routing follow-on~~ — **RETIRED 2026-09-06**: `SCStudies.cpp`/
   `SCImbalanceStudies.cpp` mutual exclusivity (§1.8, operator clarification) means live inference
   is never concurrent between the two models — no port/selector-field decision is needed at all.
5. ~~Design §1.6's Lock D/E imbalance-clock-native equivalent~~ — **RESOLVED 2026-09-06:
   events/bars-since-last-update on the imbalance clock, calendar Market-Closed Gate unchanged.**
   See §1.6 for the decision; the numeric threshold itself is a separate, still-open empirical-
   derivation follow-on (blocked on §1.2's multi-frame `ImbalanceBarEngine` extension existing).
6. ~~Decide §1.6's writer question~~ — **RESOLVED 2026-09-06: extend `LBRFileManager`** with
   `LogImbalanceContext`/`LogImbalanceAlpha`, not a dedicated sibling class. See §1.6 for the
   decision; exact method signatures remain blocked on §1.5's struct actually being defined.
7. ~~Decide §1.7's interim "shadow mode" question~~ — **RETIRED 2026-09-06**: mutual exclusivity
   (§1.8) rules out any concurrent live operation, so shadow mode cannot exist in this
   architecture at all. Validation is offline-only, per §1.6's workflow.
8. ~~Scope a real implementation plan for `ImbalanceScreen1.cpp`~~ — **SCOPED 2026-09-07, see
   §1.1a**: structural template, engine-instance placement, `IndicatorKey` namespacing, and
   tick-level integration point all resolved there. ~~Confirm or revise the candidate dim-
   placement proposal~~ — **RESOLVED 2026-09-07: all 4 dims placed** (`hurst_exponent`→`IS1`,
   `recurrence_rate`→`IS2`, `skewness_idx`+`taleb_kurtosis`→`IS3`), grounded in each dim's own
   calendar-clock sibling's real screen ownership (file:line citations in §1.1a), not guessed.
   **`src/ImbalanceScreen1.cpp` IMPLEMENTED 2026-09-07** (added to `CMakeLists.txt`,
   `./build_dll.sh --no-clean` clean): drives `ImbalanceClockManager::Instance().OnTick()` as the
   sole producer (`STD_PREC_LEVEL`), reads `GetIs1Returns()`, computes `hurst_exponent` via
   `DfaHurstExponent` with the same carry-forward convention as `ContextManager.cpp`'s
   `fast_hurst_exponent`, and displays it via its own `Subgraph`. Deliberately NOT yet wired into
   `IndicatorManager`/`IndicatorKey` or `ImbalanceObservationData` (§1.3/§1.5, separate design
   passes) — this is the smallest real end-to-end slice (producer + cascade + one dim), not the
   final wiring.
9. Two real follow-ons from §1.2's 2026-09-07 implementation, not yet done: (a) ~~decide where the
   3 `IS1`/`IS2`/`IS3` `ImbalanceBarEngine` instances actually live~~ — **SUPERSEDED 2026-09-07 by
   §1.2b, not "inline per-screen" as this item originally said**: only `IS3` has a real engine,
   privately owned by the new `ImbalanceClockManager` singleton holder — IMPLEMENTED and tested
   (`include/ImbalanceClockManager.h`, 15/15 native tests); (b) empirically derive
   `EnableAdaptiveThreshold`'s `α` (currently the source brainstorm's own `0.01`, not independently
   re-derived) once real multi-frame data exists to calibrate against — still open, `K1`/`K2` were
   validated 2026-09-07 (see below) but `α` itself was not.
10. ~~Same design pass needed for `ImbalanceScreen2.cpp`/`ImbalanceScreen3.cpp`'s own dim
    assignments~~ — **RESOLVED 2026-09-07 as part of item 8**: all 4 live activity-clock dims now
    have a placed screen. ~~Still open: `ImbalanceScreen2.cpp`/`ImbalanceScreen3.cpp`'s own FILE
    structure/implementation~~ — **IMPLEMENTED 2026-09-07** (`src/ImbalanceScreen2.cpp`,
    `src/ImbalanceScreen3.cpp`, added to `CMakeLists.txt`, `./build_dll.sh --no-clean` clean): both
    are pure readers (`GetIs2Returns()`/`GetIs3Returns()`, `LOW_PREC_LEVEL`/`VERY_LOW_PREC_LEVEL`,
    never call `OnTick()`, per §1.2b's producer discipline). IS2 computes `recurrence_rate` via
    `RecurrenceRateEngine`/`SelectEpsilonForTargetRecurrenceRate`, cached-until-new-bar exactly
    mirroring `ContextManager.cpp`'s own convention. IS3 computes `skewness_idx`+`taleb_kurtosis`
    via `BowleySkewness`/`MoorsKurtosis` (`RobustMoments.h`), carry-forward-on-non-finite guarded
    from the start (the exact NaN bug class found or the calendar-clock side 2026-09-04). Still
    open: whatever NEW Gang-statistical indicators (Work Rate's successor, Gang-MACD, Gang-Impulse)
    eventually land on any of the 3 screens beyond these 4 pre-existing dims.
11. Now legitimate to scope in detail (per the operator's own question, 2026-09-07): with all 4
    dims placed, `ImbalanceObservationData`'s actual field list can finally be drafted (4 fields
    seeded from these dims, per §1.5's case-by-case inclusion — still not automatically "all
    4 dims = the whole vector," that inclusion decision is separate from placement), which in turn
    unblocks scoping `ImbalanceContextManager.cpp` for real (previously assessed as premature,
    2026-09-07, precisely because this dim list didn't exist yet).
12. ~~Real gap: `EnableAdaptiveThreshold()` doesn't by itself produce the 25:5:1 ratio~~ —
    **RESOLVED 2026-09-07 via §1.2a, following a thorough two-source literature review (Gemini
    web, full citations; Gemini CLI attempted but hit a transient tool error mid-run):
    hierarchical nesting ("bars-of-bars"), not 3 independent engines.** One real adaptive
    `ImbalanceBarEngine` at `IS3` only; `IS2`/`IS1` are aggregations of `K2`/`K1` already-completed
    finer bars, guaranteeing exact boundary nesting (grounded in Mallat 1989/Daubechies 1992
    multiresolution-analysis theory) — the "derive-one-then-scale via 3 independent engines"
    alternative was explicitly considered and rejected (stochastic boundary drift, no structural
    relationship between the 3 engines' bar closes). The `25:5:1` ratio itself is now independently
    grounded (not just inherited from Elder's own calendar-time heuristic) via multifractal
    cascade literature (Mandelbrot 1997; Calvet & Fisher 2002)'s `m≈4-5` scale-separation
convention (though real-data validation, below, ultimately favored the lower end of that range,
`4`, not `5`). **§1.2b (added 2026-09-07) further resolves where the aggregator lives (a new
`ImbalanceClockManager` singleton, mirroring `ContextManager`/`IndicatorManager`'s own pattern,
NOT bundled into `ImbalanceBarEngine` itself) and how cross-screen ordering/staleness is
handled (`sc.CalculationPrecedence`, confirmed from Sierra Chart's own docs; `IS1` is both
highest-precedence and the tick-feed producer by design, guaranteeing zero staleness for
`IS2`/`IS3`'s reads, not just tolerated one-tick staleness).**

**`K1`/`K2` empirically validated 2026-09-07 at `4`/`4`, not `5`/`5`** — real 471.9M-tick MES
validation (`tools/observation_vector/imbalance_clock_manager_ratio_eval.cpp`) found `K=4` gives
lower lag-1 return autocorrelation than `K=5` at both IS2 (0.035 vs 0.087) and IS1 (0.135 vs
0.253); `K=5`'s IS1 autocorrelation was even higher than IS3's own raw level, actively reversing
the theoretically-predicted decorrelation trend. `K=4` also matches this repo's own existing
calendar-clock ratio (240:60:15min = 4:1). Honest caveat: neither candidate showed a fully
monotonic IS3→IS2→IS1 decorrelation trend (IS1 ticks back up from IS2 in both cases) — this is a
real, single-pass finding favoring `4` over `5`, not a fully-settled theoretical confirmation.
`ImbalanceClockManager` (below) now defaults to `4`/`4`, implemented and native-tested (15/15
pass, `include/ImbalanceClockManager.h` + `tests/cpp/test_imbalance_clock_manager.cpp`), not just
designed. Remaining real follow-on: revisit the reviewed pseudocode's `std::vector`-buffer pattern
question — already moot, the shipped implementation uses `RingBuffer` per this repo's own DOD
convention, not `std::vector`.

## 5. Candidate idea, not yet designed (migrated from the 2026-08-29 brainstorm doc §5.5, that
doc removed 2026-09-09)

**`CalculateMarketSpeed()`'s True-Range tempo proxy** (feeds the already-live `AdaptiveWindowParams`
adaptive-windowing mechanism, calendar-clock side) could plausibly be replaced or augmented by
`ImbalanceBarEngine`/`ImbalanceClockManager`'s own bar-formation rate — the rate at which
imbalance bars complete is itself a direct, real-time activity measure, potentially better than a
True-Range-based tempo proxy. Not yet designed (needs its own pass — how the rate is normalized,
which screen/window feeds it, whether it's additive or a replacement), logged here so it isn't
lost with the source doc's removal.

