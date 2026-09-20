# Decision-Boundary Calibration Methodology Audit + Coordinated Tool — Spec

**Status**: design/brainstorm only, nothing implemented. Spun out of
`2026-09-19-meaningful-event-trigger-and-asymmetry-context-significance-spec.md` §4c (Trigger 3
threshold calibration) once reusing "already-calibrated" `AsymmetryContext`/`LocalRiskContext`
thresholds turned out to require verifying how each one was actually derived — the verification
itself became the more important finding.

## 1. Problem

While designing Trigger 3 (per-field `AsymmetryContext` magnitude significance), the plan was to
reuse existing, real-data-calibrated decision boundaries wherever a genuine quantity overlap
exists with already-gated `LocalRiskContext` fields. Checking each one's actual derivation (not
just its citation comment) found **three materially different methodologies coexisting under the
same "percentile-matched" framing**, plus several constants with no derivation on record at all.
This is a real, load-bearing inconsistency — this project's own standing rule (cited repeatedly
across recent specs, e.g. the kurtosis/burstiness/skewness migrations) requires thresholds to be
real-data-derived, not invented, yet roughly half of the *existing* production gates checked here
don't meet that bar themselves.

## 2. Evidence: per-field audit

| Field | Threshold(s) in production | How it was actually computed | Saved/reusable artifact? |
|---|---|---|---|
| `taleb_kurtosis` | `1.3248`/`1.6414`/`1.5650`/`1.3809`/`2.0064`/`1.7592` | Real percentile-matching: built a paired (old-moment-based, new-Moors) sample from real MES `.scid` data, mapped each old threshold to its new-scale percentile equivalent (Task 7, 2026-08-13) | **Yes** — `tools/observation_vector/analyze_kurtosis_threshold_migration.py` + `build_paired_kurtosis_sample.py`, git-tracked, rerunnable |
| `taleb_skewness` | `±0.1544` (`PositionManager.cpp` GAP 10) | Comment claims "percentile-matched to the Bowley quartile-skewness scale... same methodology as Task 7's kurtosis migration" | **No script found anywhere in `tools/`** — full-repo search for a skewness-migration script/CSV turned up nothing. The claim may be true but is not independently verifiable or rerunnable. |
| `raschke_burst` | `1/3`, `1/2` (`PositionManager.cpp` GAP 12) | Algebraic transform (`B=(x-1)/(x+1)`) of the **old**, pre-bounded-scale thresholds `2.0`/`3.0` — a scale re-expression, not a fresh real-data pass | The transform math itself is sound and traceable, but `2.0`/`3.0`'s own original derivation has **no citation anywhere** — likely a hand-picked constant from whenever the gate was first written, carried forward by algebra ever since |
| `shannon_entropy` halt frac (`0.90 × kShannonMaxEntropyBits`) | `RiskManager.cpp` | No percentile-matching citation. Comment explicitly states a later `GetShannonEntropy()` formula change (Miller-Madow bias correction) was "evaluated and **deliberately NOT re-derived**" | None |
| `shannon_entropy` bands (`0.80`/`0.60`/`0.45`) | `Scoring.cpp` `GetDeepContextMultiplier()` | No derivation citation of any kind | None |
| `taleb_cliff` (`elderChandelierATR < 0.50`) | `RiskManager.cpp` hard gate | No derivation citation of any kind | None |
| `roughness_ratio` | — | Never calibrated (new field, 2026-09-19 rename from `paretoRot`) | N/A |
| `session_quality_score` | — | Never calibrated (new continuous field, 2026-09-19 producer fix) | N/A |

Also confirmed via `tools/observation_vector/`'s directory listing: the pattern is a **new bespoke
script per migration event** (`fractal_dim_threshold_migration.py`, `mean_rev_z_variant_comparison.py`,
`hill_intraday_seasonality.py`, `amihud_liqfragility_recalibration.py`, etc.) — no shared, generic
"calibrate a decision boundary for dim X against real tick data" tool exists. The closest precedent
to a shared foundation is `market_test_stats.h` (a growing, git-tracked shared statistics-utility
header, extracted "on 2nd/3rd real use" per its own doc comment) — good convention to build on, but
it currently covers bootstrap/hypothesis-testing primitives, not threshold derivation itself.

## 3. Assessment

Three genuinely different methodologies currently coexist, silently, under one "percentile-matched"
label:
1. **Real, reproducible, git-tracked pipeline** (kurtosis only) — paired real-data sample, saved
   script, rerunnable, auditable.
2. **Claimed-but-unpreserved one-off pass** (skewness) — the method may have been sound at the
   time, but nothing survived that can be rerun or audited when the underlying formula next changes
   (exactly the kind of drift the kurtosis migration itself was triggered by).
3. **Algebraic carry-forward of an undocumented legacy constant** (burstiness) — mathematically
   valid as a scale transform, but only as trustworthy as whatever the original constant was, which
   is not on record.
4. **Hand-picked engineering constants with no data behind them at all** (entropy bands, cliff gate).

This is a real gap, not a documentation nicety: reusing category 2-4 thresholds as-is for Trigger 3
would launder ad hoc constants into a new mechanism rather than fix the underlying problem, and the
codebase has no standing capability to prevent this from recurring on the next dim or the next
formula change (which is exactly how the skewness gate's own citation became unverifiable in the
first place). A coordinated tool is warranted.

## 4. Goals for a coordinated calibration tool

1. **One generic, parameterized pipeline**, not a new bespoke script per event. Given: (a) a real
   tick-data source (e.g. `mes_ticks.parquet`, or a paired old/new-formula sample when migrating a
   scale), (b) a named quantity/dim, (c) a calibration mode, produce a threshold (or set of
   thresholds) plus a saved, git-tracked, rerunnable derivation artifact.
2. **Calibration modes needed** (cover every real case found in §2, not just Trigger 3's 2 new
   dims):
   - **Percentile-match against a legacy threshold** (kurtosis's precedent) — for scale-migration
     events.
   - **Empirical percentile at a target base/exceedance rate** — for a fresh gate with no legacy
     anchor (`roughness_ratio`, `session_quality_score`, and retroactively `taleb_cliff`/entropy
     bands).
   - **EVT/GPD tail derivation** — already this codebase's convention for `ObservationData`/
     `FeatureScaler` winsorization bounds (`docs/superpowers/specs/2026-08-12-gang-literature-
     grounding-spec.md`'s methodology); the tool should reuse that same POT/GPD fitting logic
     instead of re-implementing it a third time.
3. **Every run produces a citable artifact**: a saved script/config (git-tracked) plus a report
   (same `ToolProgressLogger` convention already standard for `tools/` executables, auto-archived
   to `tools/output/`, auto-logged to `tools/RECALIBRATION_LEDGER.md`) that the eventual C++ comment
   can cite by exact path — closing the exact gap that made the skewness threshold unverifiable.
4. **Reruns cheaply on formula change**: when a dim's underlying computation changes (e.g. another
   Miller-Madow-style correction), the tool should be re-invocable with the same inputs/mode to
   produce an updated, equally-documented threshold — not require a fresh one-off script.

## 5. Non-goals / scope boundaries

- **Not** a one-shot rewrite of every existing gate in this pass. Re-deriving `shannon_entropy`'s
  bands or `taleb_cliff`'s `0.50` touches live risk-gating behavior with its own blast radius and
  deserves its own review, separate from building the tool itself.
- **Not** re-litigating whether Trigger 3 should exist — that's the parent spec's concern. This
  spec only concerns how its thresholds (and, opportunistically, existing undocumented ones) get
  computed.
- **Not** assuming every ad hoc constant found in §2 is *wrong* — only that its correctness is
  currently unverifiable. Re-deriving may confirm the existing value (as happened for several
  kurtosis sites) or reveal a real miscalibration (as happened for `burstiness_index` twice, per
  `CLAUDE.md`'s own account) — the point is making that determination possible, not presupposing it.

## 6. Design sketch

- **Location**: `tools/observation_vector/` (existing home for this class of tool) or a new
  `tools/calibration/` if the generic tool's scope clearly outgrows the observation-vector-specific
  siblings already there — decide at implementation time based on how much of `market_test_stats.h`
  it ends up sharing.
- **Interface** (illustrative, not final): a C++ CLI (matching this repo's real-tick-data
  performance needs, not Python, per the `mes_ticks.parquet`-scale precedent) taking `--dim <name>`,
  `--mode {percentile-match|empirical-percentile|evt-gpd}`, `--ticks-parquet <path>`, and
  mode-specific parameters (legacy threshold value for percentile-match; target rate for
  empirical-percentile; POT threshold for evt-gpd). Shares `market_test_stats.h` for
  percentile/bootstrap primitives and reuses the existing GPD-fitting logic from the
  `FeatureScaler`/observation-vector recalibration tools rather than reimplementing it.
- **Output**: threshold value(s) printed via `ToolProgressLogger` (auto-archived +
  auto-ledgered), plus a suggested C++ comment block citing the exact tool invocation and report
  path, matching the existing best-documented examples (kurtosis's own comments) as the house style.

## 7. Retroactive audit queue (existing thresholds needing re-derivation or, at minimum, honest documentation of why not)

1. `taleb_skewness` (`±0.1544`) — highest priority: actively cited as calibrated but unverifiable.
   Either recover/rerun the original derivation or refit fresh.
2. `raschke_burst`'s underlying `2.0`/`3.0` legacy basis — determine if it was ever real-data-derived;
   if not, refit fresh on the current bounded scale directly (skip the algebraic-transform
   indirection).
3. `shannon_entropy` bands (`0.80`/`0.60`/`0.45`) and halt frac (`0.90`) — no citation at all;
   candidate for empirical-percentile-at-target-rate mode.
4. `taleb_cliff` (`elderChandelierATR < 0.50`) — no citation at all; same candidate mode.

## 8. Prospective queue (blocking Trigger 3, parent spec §4c)

1. `roughness_ratio` — no existing gate anywhere; needs fresh empirical-percentile or EVT/GPD
   derivation before Trigger 3 can cover it.
2. `session_quality_score` — no existing gate on the continuous score (only the categorical
   `TimeOfDayEnum` has session multipliers); same treatment needed.

## 9. Sequencing recommendation

1. Build the generic tool once (§6), validated against the one dim with a known-good real
   reference answer already on record (`taleb_kurtosis`) as a correctness check before trusting it
   for anything new.
2. Run it for the prospective queue (§8) first — this is what's actually blocking Trigger 3.
3. Run it for the retroactive queue (§7) as a separate, explicitly-scoped follow-up (each touches
   live risk-gating behavior and deserves its own review/sign-off, per §5's non-goals).
4. Update `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md` (the standing
   literature-grounding reference for every Shannon/Mandelbrot/Taleb/Pareto parameter in this repo)
   once any retroactive re-derivation lands, per that doc's own stated maintenance convention.

## 9a. Literature-grounding consult, 2026-09-19 (`CLAUDE_BRIEF_149`/`_REPLY`, `lbrnet/logs/rc_gemini.log`)

Real-name/methodology findings for both the prospective and retroactive queues, read-only Gemini
research consult (`--approval-mode plan`, no code touched):

- **`roughness_ratio` has a real name after all**: mathematically the reciprocal of Kaufman's
  Efficiency Ratio (*Smarter Trading*, 1995: `ER = Displacement/PathLength`), equivalently a
  **tortuosity index** in movement-ecology/random-walk literature — not Sevcik's fractal dimension,
  confirming the prior mislabeling finding. Kaufman's own trending-regime convention (`ER > 0.30`)
  inverts to `roughness_ratio < 3.33` — a literature-grounded reference point distinct from, and
  worth comparing against, this session's empirical 10%-exceedance threshold (3.4898).
- **EVT/GPD methodology correction**: standard practice (Coles 2001) sets decision thresholds
  directly from the fitted GPD's own quantiles/return levels at a target risk probability, not by
  layering a second arbitrary exceedance percentage on top of an already-fitted return level (what
  this plan's Task 6 did for `roughness_ratio`'s EVT-GPD mode). Worth a methodology revisit before
  Trigger 3 treats that 6.2597 figure as final — not yet done.
- **`session_quality_score`**: the underlying session ranking is real and literature-grounded
  (Admati & Pfleiderer 1988; Wood/McInish/Ord 1985 U-shaped intraday patterns; Raschke 1995), but
  Gemini was explicit that the numeric `[-1,+1]` encoding and any threshold on it (including this
  session's -0.8) has **no literature precedent at all** — an honest "we invented this" finding, not
  a gap to keep searching for.
- **Retroactive queue (§7)**: no universal literature bands exist for Shannon-entropy chaos
  thresholds or Chandelier-Exit early-warning distance (both remain engineering choices even after
  a fresh derivation). Bowley quartile skewness DOES have a generic robust-statistics convention
  (`|S_B|<0.10` negligible, `0.10-0.30` mild, `>0.30` moderate-to-strong) — the existing `0.1544`
  lands in "mild," consistent-but-not-validating (the convention doesn't pin an exact number).
  Goh-Barabási burstiness has no universal gating threshold either. Full detail:
  `CLAUDE_BRIEF_149_REPLY`.

## 10. Relationship to other specs

- Parent: `2026-09-19-meaningful-event-trigger-and-asymmetry-context-significance-spec.md` §4c —
  this spec exists to make that section's threshold-reuse claims verifiable/correct.
- Methodology precedent: `2026-08-12-gang-literature-grounding-spec.md` (EVT/GPD/percentile-matching
  conventions already established for `ObservationData`/`FeatureScaler`) — the new tool should be a
  generalization of that existing discipline, not a competing one.
- Statistical primitives precedent: `tools/observation_vector/market_test_stats.h` — reuse, extend
  in place per its own "extract on 2nd/3rd use" convention rather than duplicating percentile/
  bootstrap logic a third time.
