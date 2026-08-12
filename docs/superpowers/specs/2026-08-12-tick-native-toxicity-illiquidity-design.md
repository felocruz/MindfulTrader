# Design: Tick-Native Fix for Dims 7 (`micro_asymmetry`) & 11 (`amihud_illiquidity`)

Date: 2026-08-12
Status: DESIGN APPROVED, not yet implemented.
Scope: `TripleScreen3.cpp`, `StudyHelperFunctions.cpp`, `FeatureScaler.h`. C++ side only —
a Python-side (`lbrnet`) refactor is an explicit follow-on, out of scope here.

Supersedes/finalizes: the dim-7 amendment already made to
`docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md`'s D1, and the
recommendation in `docs/superpowers/plans/2026-08-12-denali-data-feed-proxy-audit.md`. Does **not**
touch that spec's dims 1/2/8 fixes (ATR/burstiness/flat-price carry-forward) — those are unrelated
to tick data and remain as speced there.

## Motivation

Prompted by "we now have valuable tick-level data + the DLL isn't in production yet, so we can
restructure freely" — this triggered a brainstorm on whether to redesign the whole microstructure
quadrant (dims 8-11: `fisher_info`, `tail_index`, `skewness`, `amihud_illiquidity`) around the
newly-confirmed-reliable tick data (see the Denali proxy audit: genuine per-tick bid/ask price and
volume classification, going back years, was near-absent before the 2026-08-06/07 Denali backfill
and is now fully present).

That brainstorm surfaced a settled, well-reasoned prior decision
(`docs/ADR/liquidity_toxicity_gate_decision.md`, 2026-07-14; `docs/ADR/amihud_gate_percentile_spec.md`,
2026-07-15) that already rejected VPIN (Andersen & Bondarenko 2014: VPIN's Bulk Volume Classification
is a noisy proxy for trade direction, valuable only when real trade classification is unavailable —
which this system already had, and now has more robustly) and already established the correct
dual-axis architecture: **Amihud for illiquidity/price-impact** (dim 11), **OFI/T&S asymmetry for
toxicity/adverse-selection** (dim 7). Amihud's formula and risk-gate percentile normalization were
already fixed and offline-validated (2026-08-04). Dims 8-10 have no data-availability defect at all
and are untouched by this design. **Scope narrowed accordingly**: fix dims 7 and 11 only, using the
already-settled architecture, not a quadrant redesign.

## Architecture

No `mts_schema.fbs` changes. Dims 7 and 11 keep their existing names, positions, and semantics in
`ObservationData` — only their internal computation changes. No new per-tick accumulator struct is
introduced: Sierra Chart already maintains `sc.BidVolume`/`sc.AskVolume` as tick-accumulated,
per-bar arrays (confirmed reliable at the `.scid` byte level across all sampled eras), and
`VolumeIndicator` already reads them successfully every bar in the same replay sessions where the
hand-rolled Time & Sales scan fails ~45% of the time. Building a second, redundant accumulator would
duplicate work Sierra Chart already does correctly.

### Dim 7 — `micro_asymmetry`

Replace the call to `CalculateMicroAsymmetry`/`CalculateMicroAsymmetryFromTimeAndSales`
(`StudyHelperFunctions.cpp:2637-2727`) at its one call site (`TripleScreen3.cpp:758`, feeding
`obs->mutate_micro_asymmetry(microAsymmetry)` at line 774) with:

```
micro_asymmetry = (AskVolume - BidVolume) / (AskVolume + BidVolume)
```

read from `sc.AskVolume[sc.Index]`/`sc.BidVolume[sc.Index]` — the same arrays and same bar index
`VolumeIndicator::UpdateVolume()` already reads at `TripleScreen3.cpp:637-638`. This mirrors
`VolumeIndicator`'s working pattern exactly, so no new reliability question is introduced.

`CalculateMicroAsymmetryFromTimeAndSales`/`CalculateMicroAsymmetry` are deleted, not kept as a
log-only cross-check — they have no live purpose once the primary path no longer calls them, and
"dual-mode" dead code masquerading as active is exactly the kind of unreviewed claim (the never-built
"Phase 2" this investigation found) this codebase should not accumulate more of.

**Backstop for the true no-trade bar** (`AskVolume == BidVolume == 0`, confirmed by the `.scid`
audit to be essentially never observed intraday for ES/MES, but free to guard): carry the last valid
value forward via a `sc.GetPersistentFloat` slot, seeded to `0.0f` (true neutral) only before any
valid bar has ever been observed — same shape as the existing Hurst/`CalculateRealizedKurtosis`
carry-forward pattern already in this file.

### Dim 11 — `amihud_illiquidity`

**No calculator change up front.** `CalculateAmihudIlliquidity` (`StudyHelperFunctions.cpp:3149-3180`)
already computes the canonical Amihud formula correctly (fixed 2026-08-04) and was offline-validated
as 100% finite with zero degenerate zeros across 61,078 bars
(`docs/ADR/amihud_gate_percentile_spec.md` §5.1). Its `count < 2` degenerate branch is real but, per
that validation, essentially never fires in practice — it is not the dominant source of dim 11's
exported zero-ratio.

The exported zero-collapse (§2 of the 2026-08-12 sentinel-collapse audit: ~40% zero, flat across
288 hours) is isolated to `FeatureScaler`'s MAD-floor handling of Amihud's naturally tiny raw
magnitude (median ~2.1e-11 per the same validation) — a mechanism `FeatureScaler.h` already
documents (`DIM_AMIHUD_INDEX`/`AMIHUD_ABSOLUTE_FLOOR` comment) and has a partial fix for.

**Action: re-verify before writing more code.** Collect a fresh `.context` file against the
current `FeatureScaler.h` (confirming the `AMIHUD_ABSOLUTE_FLOOR` fix is actually in the DLL that
produces it — the file this investigation diagnosed from may predate that fix reaching a deployed
build) and re-run the octile zero-ratio analysis on dim 11 specifically.
- If the existing floor fix already resolves it: no C++ change needed for dim 11 at all — close
  this out as a stale-diagnosis artifact.
- If it doesn't: apply the carry-forward pattern from the sentinel-collapse spec's D1 (last-valid
  carry-forward on `madScale < minScaleFloor` with no prior valid value) as originally speced —
  that mechanism is unchanged by anything learned in this design.

## Data Flow

Both dims remain computed once per 15-minute bar close, inside `TripleScreen3.cpp`'s existing
observation-vector block (`CANONICAL OBSERVATIONDATA VECTOR - SCREEN 3` section). No new state,
no new per-tick hook, no change to `EventDataCollectorStudy.cpp` or `ContextManager.cpp`'s
`BuildObservationVector()`/`SanitizeObservationVector()`/`FeatureScaler::UpdateAndNormalize()`
pipeline (dim 11's fix, if needed at all, lives entirely inside `FeatureScaler.h`).

## Testing

Per `superpowers:test-driven-development` — write these failing first:

1. **Dim 7 unit test**: given known `AskVolume`/`BidVolume` pairs (including the both-zero
   no-trade case), assert `micro_asymmetry` equals the OFI formula's output, and equals the
   carried-forward last-valid value on the no-trade case (not `0.0`, except before any valid bar).
2. **Dim 7 regression test**: confirm `CalculateMicroAsymmetryFromTimeAndSales`/
   `CalculateMicroAsymmetry` and their one call site are fully removed (grep-based or compile-based
   assertion) — prevents silent reintroduction.
3. **Dim 11 empirical re-verification** (not a unit test — a data check): re-run
   `context_preflight.py`'s octile zero-ratio analysis (from the sentinel-collapse audit) against a
   freshly-collected `.context` file. This is the gate for whether any dim-11 code change happens
   at all.
4. **`context_preflight.py` chronic-zero threshold** (D3 from the sentinel-collapse spec, unaffected
   by this design, still to be implemented there): confirms dim 7's zero-ratio drop is caught/held
   by the gate going forward.

## Explicitly Out of Scope

- Dims 8/9/10 (`fisher_info`/`tail_index`/`skewness`) — no defect found, not touched.
- VPIN or any BVC-based toxicity proxy — deliberately rejected, reasoning strengthened (not
  weakened) by the new tick data; not revisited.
- `mts_schema.fbs` changes — none needed for this fix.
- A genuine per-tick DOD accumulator struct — unnecessary; Sierra Chart's existing
  `sc.BidVolume`/`sc.AskVolume` arrays already provide this.
- Python (`lbrnet`) refactor — deferred to its own spec once this C++ side is stable, since
  redesigning Python against a moving C++ output would be wasted work.
