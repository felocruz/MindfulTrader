# Sierra Chart Data Feed & Package Selection for Live ES Trading

**Status**: Decision implemented 2026-08-04: Switched to Package 11 to unlock real Volume Profile features.

## Decision: Package 11 + Denali (CME, no market depth) + Interactive Brokers for execution only

**Do not use Interactive Brokers as the market-data source.** Sierra Chart's own documentation states IB's bid/ask-volume data is inaccurate "100% occurrence all the time" for exactly the kind of order-flow studies this codebase runs, and IB's Time & Sales completeness is explicitly not guaranteed. That would silently corrupt the two live signals this ACSIL code actually reads:
- `StudyHelperFunctions.cpp:3459-3493` — `sc.GetTimeAndSales()`, bid/ask trade classification for order-flow-asymmetry.
- `TripleScreen3.cpp:624-625` — per-bar `sc.BidVolume`/`sc.AskVolume`.

IB also has limited historical intraday data, conflicting with `EventDataCollectorStudy.cpp`'s hard gate requiring ≥365 days of intraday history at startup.

**The fix is a named, Sierra-Chart-documented configuration, not a workaround**: use Denali (Sierra Chart's own feed) for market data; use IB purely as the trade/order-routing service ("Denali data feed with Interactive Brokers execution").

| Component | Choice | Approx. cost |
|---|---|---|
| Sierra Chart package | **Package 11** (Integrated Standard Plus / Volume Profile) | $46/mo |
| Market data | **Denali, CME, without market depth** | $6-13.50/mo (non-professional, requires a live funded futures account) |
| Execution | **Interactive Brokers**, configured as trade service only | existing IB commissions |

**Why Package 11**: We switched to Package 11 to unlock Sierra Chart's real Volume Profile study. This allows us to replace the existing proxies (TPO Value Area Proxy and close-price histogram Point of Control) with real Volume Profile POC, VAH, and VAL via the `sc.GetStudyArrayUsingID` pattern already used elsewhere in the codebase. This represents a significant upgrade to live market profile feature parity without requiring Package 12/MBO.

Package 12 + MBO specifically was evaluated and **not recommended**: the literature on MBO's incremental value over aggregated order-book data (arXiv:2102.08811) shows it's modest/orthogonal, realized mainly via ensembling, at horizons (tick/sub-second) far shorter than this system's regime dwell times (~200-400s, from the HMM's own calibrated temporal half-life). MBO's message rate would also seriously strain this project's already-tight compute budget.

### Setup steps (not yet executed)

1. TWS/IB Gateway: File → Global Configuration → API → Settings — enable "ActiveX and Socket Clients," port 7496 (live) / 7497 (paper), disable "Read-Only API," Component Exchange Separator = `/`.
2. Sierra Chart: Global Settings → Data/Trade Service Settings → select Interactive Brokers as the *trading* service, `127.0.0.1:7496`, unique Instance Client ID, enable "Connect On Program Startup" / "Reconnect On Failure." Denali continues to supply chart data automatically for subscribed exchanges.
3. **Verify at setup time, don't trust this doc blindly**: symbol mapping differs between Denali's native Sierra Chart symbol and IB's own ES format (`ES-YYYYMM-GLOBEX`, e.g. `ES-202509-GLOBEX`). Sierra Chart has a per-symbol trade-symbol-override field for this dual-source case — confirm the exact current UI path against Sierra Chart's setup wizard/support board, not this note.
4. Confirm a recent Sierra Chart build (IB-integration issues were flagged by Sierra Chart's own team pre-version-2480, ~2023 — almost certainly moot by now, worth a one-line version check before going live).

## Separate finding: genuine low-hanging fruit in the existing code, independent of any package/feed decision above

`MindfulTrader/src` already has **Market-Profile-labeled features implemented as admitted proxies**, not the real thing:

- `StudyHelperFunctions.cpp:1514-1550` (`CalculateDailyBias()`) — comment says **"Calculate TPO Value Area Proxy (Central 70% of Range)"**; computes `VAL = prevDayLow + range*0.15`, `VAH = prevDayLow + range*0.85` — a naive range split, not a real Value Area (which should be the price band containing 70% of *traded volume*).
- `StructureEngine.cpp:60-84` (`GetRecurrenceRate()`) — comment says **"Use a simple histogram to find the Point of Control (Mode)"**, but bins the last N *closing prices*, not volume — a price-recurrence proxy, not real POC (the price level with the most volume traded).
- `docs/GUI_INDICATOR_REFERENCE.md:416-422` — `daily_bias` is explicitly documented as "(Market Profile context)" — the naming/intent is already Market-Profile-native, the math underneath just isn't.

**Package 11 ($46/mo — does not need Package 12/MBO)** unlocks Sierra Chart's real Volume Profile study. The codebase already has the exact mechanism to consume a built-in study's output without reimplementing the math: `sc.GetStudyArrayUsingID(Input_XStudy.GetStudyID(), subgraphIndex, DestArray)`, already used for MACD/Keltner/cross-market closes (`TripleScreen1.cpp:208,221,864`; `TripleScreen2.cpp:180,193,208`; `CrossMarketStudies.cpp:164,319`). Pulling real POC/VAH/VAL into `IndicatorManager::SyncFeatureVector()` (`IndicatorManager.cpp:542-622`) or `ContextManager`'s observation-population code is the same pattern, same effort, pointed at a different study ID. Use **Volume Profile (real traded volume)**, not literal TPO letter-counting — Volume Profile is the standard choice over TPO for a liquid, high-volume instrument like ES.

**Bonus**: `docs/GUI_INDICATOR_REFERENCE.md:424` already flags `daily_bias` as **"⚠️ NOT found in raw data ... may be missing from C++ transmission"** — a known, separate transmission gap worth fixing in the same pass since it's the field these proxies feed.

**Not low-hanging, evaluated and set aside**: Numbers Bars (different bar-construction scheme entirely, would require re-deriving the whole feature pipeline's bar-cadence assumptions); Market Depth Historical Graph (needs a separate depth-data subscription the code doesn't otherwise need, same timeframe-mismatch concern as MBO above).

## Not yet done

- ~~Package 11 has been subscribed to/active, but the Volume Profile replacement...~~ **Scoped and implemented** — `docs/superpowers/plans/2026-08-04-volume-profile-daily-bias.md`. Two corrections the plan made to this doc's original research: (1) `StructureEngine.cpp`'s `GetRecurrenceRate()` (the second proxy this doc flagged) turned out to be dead code with zero live call sites — only `CalculateDailyBias()`'s Value Area proxy was actually swapped. (2) The swap did **not** end up needing Package 11's chart-visible Volume Profile study at all — ACSIL exposes `sc.VolumeAtPriceForBars`/`sc.MaintainVolumeAtPriceData` directly, letting the plan aggregate the previous day's real Value Area from TS3's own bars without adding any study to the chart.
