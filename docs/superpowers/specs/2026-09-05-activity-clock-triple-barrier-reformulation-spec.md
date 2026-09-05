# Activity-Clock, Jump-Robust Triple-Barrier Reformulation — Spec

**Status: SPEC — decisive design direction, NOT implemented. Opened 2026-09-05 (operator directive:
"we are looking for institutional, elite, alternatives... let go of the past... even if that means
doing away with what currently exists"). This spec supersedes ATR's role in the Triple-Barrier exit
engine (`tbe::ComputeBarriers()`/`BuildBarrierInputs()`) OUTRIGHT — not a robustified ATR variant, not
a parallel/shadow-run fallback, not a hedge. `docs/superpowers/specs/2026-09-05-robust-atr-
reformulation-spec.md` remains the governing spec ONLY for ATR's 4 peripheral, non-barrier consumers
(`ATRProximityEnum`/`EmaProximity`, Trade Grade Keltner Channels, VWAP distance normalization, Elder
Breakout distance-beyond-band) — none of which set money-at-risk stop/target width.**

## 0. Origin and decisive framing

This began as a narrower question (`docs/superpowers/specs/2026-09-05-robust-atr-reformulation-
spec.md`: "how do we make ATR itself more robust for Triple-Barrier width"). A deeper question
surfaced during that work: Wilder's ATR was never actually the volatility reference López de Prado's
own Triple-Barrier Method uses (his reference implementation, AFML Ch. 3 `getDailyVol()`, computes an
EWMA standard deviation of close-to-close returns — no ATR, no True Range, at all) — this system's own
substitution of ATR in its place was made without any documented justification anywhere in this
repo's design history (checked, found nothing).

**The operator's explicit correction, recorded verbatim in spirit**: this repo's own standing rule
already states the default is deletion/replacement, not preservation, pre-production (`CLAUDE.md`).
Robustifying ATR in place — treating it as the thing to protect and improve — was the wrong instinct
for a component this central. The right question is what the Triple-Barrier's own scale reference
*should be* if designed from the Gang's own toolkit today, with no obligation to keep anything that
currently exists. This spec answers that decisively.

## 1. The decisive replacement

**Scale reference for ALL stop/target width in the Triple-Barrier engine**: a jump-robust realized-
volatility estimator — **Bipower Variation** (Barndorff-Nielsen & Shephard, 2004, "Power and Bipower
Variation with Stochastic Volatility and Jumps," *Journal of Financial Econometrics* 2(1):1-37; and
2006, "Econometrics of Testing for Jumps in Financial Economics Using Bipower Variation," *Journal of
Financial Econometrics* 4(1):1-30) as the baseline, with **MedRV** (Andersen, Dobrev & Schaumburg,
2012, "Jump-Robust Volatility Estimation Using Nearest Neighbor Truncation," *Journal of Econometrics*
169(1):75-93) as the more elite candidate if empirical validation favors its added robustness to
consecutive/clustered jumps — **computed natively on this system's own activity/imbalance clock**
(`ImbalanceBarEngine`, fixed-imbalance triggering on cumulative `|ask_vol - bid_vol|`), not calendar
time.

This is not a new idea grafted onto this system — bipower variation is **already implemented**
(`include/BipowerVariation.h`), **already real-data-validated** against ~38.5M real MES tick rows
(`CalculateLogScaleRatio()`'s existing `log(short_BV/long_BV)` use), just never wired into barrier
width. And it is not a new idea grafted onto López de Prado's own framework either: his Ch. 2 argues
information-driven/activity bars are the statistically correct sampling clock; his Ch. 3
`getDailyVol()` still samples calendar days. Running the barrier-width volatility estimator on the
imbalance clock is applying the book's own Ch. 2 logic to its own Ch. 3 formula — completing a
synthesis the book itself never makes, not an outside substitution.

**ATR (Wilder's original, or the robustified v3 filter from the sibling spec) is REMOVED from this
path entirely.** No dual-path, no shadow comparison against it as a safety net — per this system's
own pre-production standing rule, the correct posture is to build the better thing and validate it on
its own terms, not to keep the old thing running alongside it "just in case."

## 2. The vertical (time) barrier moves to the same clock — full internal coherence, not mixed

**Decisive, not hedged**: if the stop/target width is measured on the activity clock, the vertical
barrier must be too — `max_bars` becomes **max imbalance-bars elapsed**, not wall-clock minutes or a
15-minute-bar count. Running one barrier on an activity clock while the other two stay on calendar
time would leave the barrier system internally incoherent (the "race" the three barriers run would no
longer be measured in a single consistent unit) — a real defect this spec explicitly closes, not an
open question left for later. `TripleBarrierExitManager`'s entry-latched bracket already stores
`maxBars` as a plain integer count; the only change is which clock increments it.

## 3. What remains genuinely open (sent to Gemini for independent stress-test, `CLAUDE_BRIEF_136`,
not yet answered)

These are real unresolved design questions — flagged honestly, not glossed over, but none of them
change the decisive call in §1/§2:

1. **Bipower variation vs. MedRV**: bipower variation can still be contaminated by two consecutive
   jumps (it uses products of adjacent absolute returns); MedRV's rolling-median-of-triplets
   construction is more robust to that specific case. Bipower variation is the validated baseline
   already in this system; MedRV is the candidate upgrade, pending empirical comparison on real data,
   not literature grounding alone.
2. **Price-point vs. percentage-return barriers**: López de Prado's own barriers are evaluated as
   cumulative path RETURN from entry crossing a threshold, not a fixed price-point distance the way
   `entry ± N×ATR`/`entry ± N×BV` currently work in this system. Whether this distinction matters for
   a single-instrument futures book (vs. AFML's own cross-sectional equity framing) is not yet
   resolved — does not block §1/§2's decisive scale-and-clock replacement, but is a live question for
   how the replacement scale is actually applied to compute a price level.
3. **Circularity/redundancy check**: this system's HMM observation vector already derives
   `log_scale_ratio`/`log_scale_expansion_ratio` from the same short/long bipower variation this spec
   proposes reusing for barrier width. Whether an execution-layer parameter directly sharing a
   primitive with a model-training input creates any train/live coherence issue (distinct from the
   already-resolved question of whether two MODEL INPUTS would be redundant with each other) is
   explicitly flagged for review, not assumed safe.
4. **Microstructure-noise robustness** (a distinct contamination source from jumps — bid-ask bounce,
   stale ticks): realized kernels (Barndorff-Nielsen, Hansen, Lunde & Shephard, 2008, *Econometrica*)
   address this specifically; not yet decided whether this system's tick data quality warrants that
   additional layer on top of jump-robustness alone.

## 4. Validation plan (empirical, required before cutover — decisive design, not decisive parameters)

Being decisive about the *design direction* does not mean skipping empirical validation of its
*parameters* — same standing discipline as every other reformulation this session:

1. Build the activity-clock bipower-variation (and MedRV, if pursued) computation over real MES tick
   data, compare its jump-suppression and regime-tracking behavior against real historical gap/halt
   events and genuine sustained volatility expansions — same methodology already used for the
   sibling ATR spec's own validation plan, applied to this estimator instead.
2. Re-derive every per-pattern `stop_mult`/`target_r_mult` constant against the new scale directly
   from real data (empirical percentile-matching, this repo's own established method) — not a
   theoretical correction factor.
3. Re-derive `max_bars` per regime/tier in imbalance-bar-count terms, calibrated against real data,
   not carried over numerically from the old calendar-bar-count values (the two units are not
   comparable 1:1).
4. Confirm the 7 existing golden-vector Triple-Barrier parity fixtures either get regenerated against
   the new scale/clock, or are explicitly retired if they no longer represent a meaningful parity
   target under the new design.

## 5. References

- Barndorff-Nielsen, O.E. & Shephard, N. (2004), "Power and Bipower Variation with Stochastic
  Volatility and Jumps," *Journal of Financial Econometrics* 2(1):1-37.
- Barndorff-Nielsen, O.E. & Shephard, N. (2006), "Econometrics of Testing for Jumps in Financial
  Economics Using Bipower Variation," *Journal of Financial Econometrics* 4(1):1-30.
- Andersen, T.G., Dobrev, D. & Schaumburg, E. (2012), "Jump-Robust Volatility Estimation Using
  Nearest Neighbor Truncation," *Journal of Econometrics* 169(1):75-93 — MedRV, the candidate upgrade
  (§3 item 1).
- Barndorff-Nielsen, O.E., Hansen, P.R., Lunde, A. & Shephard, N. (2008), realized kernels,
  *Econometrica* — microstructure-noise robustness, a distinct open question (§3 item 4).
- López de Prado, M. (2018), *Advances in Financial Machine Learning*, Ch. 2 (information-driven
  bars) and Ch. 3 (Triple-Barrier Method, `getDailyVol()`) — the two chapters this spec synthesizes.
- `docs/superpowers/specs/2026-09-05-robust-atr-reformulation-spec.md` — the superseded-in-scope
  predecessor spec; retained for the 4 peripheral ATR consumers this spec does not cover.
- `docs/superpowers/specs/2026-09-03-trade-execution-risk-management-curation-initiative.md` §2 item
  5 — the founding ATR finding this thread traces back to.
- `lbrnet/logs/rc_gemini.log` `CLAUDE_BRIEF_136` — the independent stress-test request for §3's open
  questions, sent but not yet replied to as of this spec's authoring.
