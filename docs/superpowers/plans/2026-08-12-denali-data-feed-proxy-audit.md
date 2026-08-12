# Audit: Denali Data-Feed Switch vs. Pre-Existing Limited-Data Proxies in the ContextManager Ecosystem

Date: 2026-08-12
Scope: `ContextManager` ecosystem (`ContextManager.cpp/.h`, `StudyHelperFunctions.cpp`,
`TripleScreen1/2/3.cpp`, `StructureEngine.cpp/.h`, `IndicatorManager.cpp`, `DailyBiasEngine.h`,
`VolumeProfileEngine.h`) + direct byte-level inspection of `/mnt/c/SierraChart2/Data/MES*-CME.scid`.

Trigger: Denali (Sierra Chart's own data feed, CME, no market depth) went live 2026-08-04 per
`docs/ADR/sierra_chart_data_feed_setup.md`, replacing IB as the market-data source (IB is now
execution-only). Task: find every place in the ContextManager ecosystem that was compensating for
data the prior feed lacked, check what Denali's `.scid` files actually contain now, and update the
sentinel-collapse fix spec (`docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md`)
if Denali changes the right fix.

## Part 1 — Proxy inventory (ContextManager ecosystem)

Delegated to a full-repo grep (no market-depth/MBO API is called anywhere — confirms the ADR's
"no-depth" decision is total). Four sites found:

1. **`DailyBiasEngine.h:12-58`** — the live successor to `StudyHelperFunctions.cpp`'s
   docstring-named "TPO Value Area Proxy." `haveRealValueArea` gates between a real
   `sc.VolumeAtPriceForBars`-derived Value Area and the naive 15%/85% range-split proxy. The real
   path is **already implemented** (`IndicatorManager.cpp:1405-1468`, via
   `include/VolumeProfileEngine.h`'s `vpe::ComputeValueArea`) but is currently **disabled**
   (`m_realVolumeProfileDailyBiasEnabled = false`, `IndicatorManager.h:97-102`) — not because the
   data is unavailable (`MaintainVolumeAtPriceData = 1` is already on globally, `SCStudies.cpp:60`,
   and per-TS3, `TripleScreen3.cpp:204`), but as a **deliberate train/live parity gate**: the
   deployed HMM was fit on the old proxy's semantics, and flipping it would silently shift feature
   distributions the model has never seen. This is orthogonal to the current investigation — no
   action needed here now, but flag for the next full HMM retrain: this proxy has a already-wired,
   already-real replacement waiting on a flag flip plus retrain.
2. **`StructureEngine.cpp:59-83` `GetRecurrenceRate()`** — close-price-histogram Point-of-Control
   proxy. Confirmed **dead code, zero live call sites** (per `docs/ADR/sierra_chart_data_feed_setup.md`'s
   own finding, re-verified here) — does not reach the 16D/8D vectors. No action.
3. **`StudyHelperFunctions.cpp:2637-2727` `CalculateMicroAsymmetryFromTimeAndSales`/
   `CalculateMicroAsymmetry`** — feeds dim 7 (`micro_asymmetry`), the dim identified in the prior
   sentinel-collapse audit (`docs/superpowers/plans/2026-08-12-observation-vector-sentinel-collapse-audit.md`)
   as ~45-50% exact-zero, flat across the whole replay window. This is the one proxy directly
   relevant to that open defect — see Part 3.
4. **`TripleScreen3.cpp:637-638`** — `sc.BidVolume[sc.Index]`/`sc.AskVolume[sc.Index]` (per-bar,
   already-aggregated bid/ask trade volume) feeding `VolumeIndicator::UpdateVolume()`'s order-flow
   imbalance. Not a proxy — this is the *real* signal, already in production use, and turns out to
   be the fix for finding 3 (Part 3).

## Part 2 — What Denali actually changed, verified at the byte level

Read the `.fbs`-free ground truth directly: `HEADER_SIZE=56`, `RECORD_SIZE=40`,
`<q 4f 4I>` (`DateTime, O, H, L, C, NumTrades, Volume, BidVolume, AskVolume`), using
`lbrnet/backtest/scid_reader.py`'s existing struct layout (no need to re-derive the format).

**Classification completeness, sampled 400k records each, three eras/contracts:**

| file | date range | both-zero | one-zero (i.e. cleanly classified) | both-nonzero |
|---|---|---|---|---|
| `MESU26-CME.scid` (current front month) | 2026-06-07..2026-08-12 | 0.00% | **100.00%** | 0.00% |
| `MESU25-CME.scid` | 2025-06-08..2025-09-19 | 0.00% | **100.00%** | 0.00% |
| `MESZ23-CME.scid` | 2023-09-03..2023-12-15 | 0.00% | **100.00%** | 0.00% |

Every single sampled record, in every era, has exactly one of `BidVolume`/`AskVolume` populated and
the other exactly zero — i.e. every `.scid` tick record was already cleanly aggressor-side
classified, in 2023 data just as much as in the current live file. No record has both sides zero
(unclassified) or both nonzero (split) in any era sampled.

**Density/granularity, same contract file (`MESU26-CME.scid`) straddling the 2026-08-04 switch:**

| window | records | records/min | trades/record | single-trade-records |
|---|---|---|---|---|
| Jul 7-8 2026 (pre-Denali) | 620,147 | 430.7 | 1.000 | 100.0% |
| Aug 7-8 2026 (post-Denali) | 520,062 | 361.2 | 1.000 | 100.0% |

`trades/record == 1.000` and `single-trade-records == 100%` in **both** windows — every `.scid`
record already represented exactly one trade, before and after the switch. The lower Aug 7-8 count
is ordinary day-to-day volume variation (also visible if you compare any two arbitrary July days to
each other), not a granularity change — there is no structural discontinuity at the switch date.

**CORRECTION (same day, before this doc's action items were acted on):** the comparison above is
between two *Denali-backfilled* files at different historical vintages, not a true pre-Denali vs.
post-Denali comparison — and that distinction matters enormously. `MESZ23-CME.scid` (new naming
convention, no `-USD` suffix) is **not** the original 2023-era recording; it is Denali's *retroactive
historical backfill*, written 2026-08-06 (confirmed via file mtime). The genuine pre-Denali files
use the old naming convention (`MES-YYYYMM-CME-USD.scid`, confirmed still present on disk) and are
**56-byte header-only stubs or a few-MB partial files** — i.e. essentially no usable historical tick
data existed before Denali. Cross-referenced against
`lbrnet/docs/superpowers/specs/2026-08-08-mes-continuous-bars-design.md` (a lbrnet-side design from
the same migration, which verified this independently and additionally found that at 1-tick
granularity Sierra Chart repurposes the `.scid` `high`/`low` fields as **ask price / bid price at
the moment of the trade** — not a price range — with `close` as the actual trade price).

**Corrected conclusion:** Denali *did* deliver a step-change — from near-zero historical tick
coverage to multi-year, fully tick-level, cleanly bid/ask-classified history, with real bid/ask
*price* (not just volume) recoverable per tick. The "no discontinuity" finding above still holds as
literally measured (Denali-backfilled 2023 data and current 2026 data are equally rich to each
other) — it just doesn't mean what it first appeared to mean. **Both things are true at once:**
(1) genuinely rich tick data is now available across the whole replay history where it was not
before, which is the open opportunity the rest of this document undersold, and (2) the specific
`CalculateMicroAsymmetryFromTimeAndSales` bug (Part 3) is independently a `sc.GetTimeAndSales()`
replay-mechanics reliability gap, not a data-richness gap — confirmed by the fact that dim 7's
~45% zero rate was measured (2026-08-12 sentinel-collapse audit) against a file collected *after*
the Aug 6 Denali backfill, i.e. against already-rich data, and the gap persisted anyway. The
`sc.BidVolume`/`sc.AskVolume` fix in Part 3 remains correct and necessary regardless. What changes
with this correction is the *scope of opportunity*: this is not just a one-dim bug fix, it's a
green light to redesign the whole tick-ingestion path around genuinely-available bid/ask price and
volume history — see the follow-on design conversation for the DOD/FlatBuffers tick-pipeline
redesign this correction motivated.

## Part 3 — Why dim 7 is ~45% zero anyway, and the actual fix

If the underlying `.scid` ticks were always cleanly classified, why does
`CalculateMicroAsymmetryFromTimeAndSales` see "no new BID/ASK records" on ~45% of its once-per-bar
calls? Because it doesn't read the classified `.scid` volume at all — it manually re-scans
`sc.GetTimeAndSales()`'s live rolling buffer with its own sequence-number bookkeeping
(`last_ts_sequence`, `StudyHelperFunctions.cpp:2639-2682`), which is a **fragile, redundant
re-implementation** of accounting Sierra Chart already does for you, and which is well known
(ADR, `docs/ADR/sierra_chart_data_feed_setup.md`) to be sensitive to feed/replay quirks — this is
exactly the kind of hand-rolled bid/ask bookkeeping the ADR was warning about for IB, and evidently
it is not perfectly reliable in this project's tick-replay path either, regardless of which vendor
feeds it.

Meanwhile, **`sc.BidVolume[sc.Index]`/`sc.AskVolume[sc.Index]`** — the same per-bar aggregated
fields this .scid audit just confirmed are always cleanly populated, and which `VolumeIndicator`
already consumes successfully every bar (`TripleScreen3.cpp:637-638`) in the identical replay
sessions where `CalculateMicroAsymmetry` fails — is sitting unused for this purpose. It is a
strictly more robust source for order-flow asymmetry than the manual T&S scan: same underlying
data, no manual sequence tracking, no dependency on `s_TimeAndSales::Type` semantics, and it is
*already proven reliable in this exact code path*.

**Revised fix for dim 7 (supersedes the carry-forward-only plan in the 2026-08-12 sentinel-collapse
spec's D1):** compute `micro_asymmetry` primarily from
`(sc.AskVolume[sc.Index] - sc.BidVolume[sc.Index]) / (sc.AskVolume[sc.Index] + sc.BidVolume[sc.Index])`,
matching `VolumeIndicator`'s already-working pattern, and keep the T&S-based
`CalculateMicroAsymmetryFromTimeAndSales` (if kept at all) as a secondary cross-check/log-only
diagnostic rather than the primary signal. Carry-forward (D1's general pattern) remains the correct
backstop for the rare true no-trade bar (`BidVolume == AskVolume == 0`), which the .scid audit shows
essentially never happens intraday for ES/MES but is cheap insurance for a completely closed
market/data gap.

Dims 1/2/11 are unaffected by this finding — they don't depend on T&S/bid-ask data at all
(`sc.Volume`, `sc.Close`, ATR-derived range), so the carry-forward fix already speced for them is
unchanged.

## Action Items

- Update `docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md` D1's dim-7
  bullet to the `sc.BidVolume`/`sc.AskVolume` primary fix (done, this pass).
- No change needed to dims 1/2/8/11 fixes.
- No action needed on the Value Area proxy (finding 1) or the dead POC proxy (finding 2) — tracked
  separately, not blocking.
- Flag for the ADR/data-feed doc: its framing ("Denali... unlocks real Volume Profile") is accurate
  for `VolumeAtPriceForBars`, but this audit found no evidence Denali changed `.scid`
  `BidVolume`/`AskVolume` classification — worth a one-line correction there so a future reader
  doesn't re-run this same investigation on the same false premise.
