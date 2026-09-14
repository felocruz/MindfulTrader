# Sierra Chart Setup — Centralized Settings Reference

**This is the single source of truth for current Sierra Chart settings this project depends on.**
Update this doc, not a copy elsewhere, whenever a setting changes or is re-verified on a machine.

- **Why these choices were made** (package/feed rationale): `docs/ADR/sierra_chart_data_feed_setup.md`
- **Install + chartbook restore steps**: `docs/NEW_MACHINE_WSL_SETUP.md` §12/§12a
- **Training-data-export replay procedure** (its own workflow, not duplicated here):
  `docs/TRAINING_DATA_EXPORT.md`
- **Per-machine bring-up coordination log**: `docs/PUGET_SETUP_COORDINATION.md`

---

## 0. Time Zone — Eastern Time (America/New_York), required

**Hard requirement, not optional.** `src/Indicator.cpp` ("Regular Trading Hours Boundaries (Eastern
Time)", `MARKET_OPEN = 9*60+30`, `MARKET_CLOSE = 16*60`, etc.) and
`src/EventDataCollectorStudy.cpp` ("CME Globex ES session times — Eastern Time (NYC)",
`CME_ES_SESSION_START_SECS = 18*3600`) both hardcode literal Eastern-Time hour-of-day boundaries
with **no timezone conversion logic** — they read `sc.BaseDateTimeIn`'s hour/minute directly. This
only produces correct `TimeOfDayEnum` classification, RTH boundaries, and CME Globex session-gap
detection if Sierra Chart's own Time Zone setting is Eastern Time. Set via Global Settings → Time
Zone → **Eastern Time**. Found missing from this doc and from the live Puget install 2026-09-14
(the fresh install defaulted to UTC) — was previously an implicit, undocumented assumption carried
over tacitly from the old machine's own setup.

---

## 1. Package, data feed, and execution setup

**Decision** (full rationale in the ADR): Sierra Chart **Package 11** (Integrated Standard Plus /
Volume Profile) + **Denali** for market data (CME, no market depth) + **Interactive Brokers** for
execution only. Do not use IB as the market-data source — Sierra Chart's own documentation states
IB's bid/ask-volume data is inaccurate for order-flow studies this codebase relies on
(`sc.GetTimeAndSales()`, per-bar `sc.BidVolume`/`sc.AskVolume`).

### Setup steps

1. **TWS/IB Gateway**: File → Global Configuration → API → Settings
   - Enable "ActiveX and Socket Clients"
   - Port `7496` (live) / `7497` (paper)
   - Disable "Read-Only API"
   - Component Exchange Separator = `/`
2. **Sierra Chart**: Global Settings → Data/Trade Service Settings
   - Select Interactive Brokers as the **trading** service (Denali continues to supply chart data
     automatically for subscribed exchanges — do not select IB for market data)
   - `127.0.0.1:7496`
   - Unique Instance Client ID
   - Enable "Connect On Program Startup" / "Reconnect On Failure"
3. **Symbol mapping** — verify at setup time, don't trust this doc blindly: Denali's native Sierra
   Chart symbol vs. IB's own ES format (`ES-YYYYMM-GLOBEX`, e.g. `ES-202509-GLOBEX`) differ. Sierra
   Chart has a per-symbol trade-symbol-override field for this dual-source case — confirm the exact
   current UI path against Sierra Chart's own setup wizard/support board, not this note.
4. **Confirm a recent Sierra Chart build** — a pre-version-2480 IB-integration issue was flagged by
   Sierra Chart's own team (~2023); almost certainly moot now, worth a one-line version check before
   going live regardless.

---

## 2. Volume Profile / Intraday Data Storage prerequisites

1. **`Intraday Data Storage Time Unit` must be `1 Tick`.** Global Settings → Data/Trade Service
   Settings. If it's coarser, `sc.VolumeAtPriceForBars` will be empty or inaccurate. Sierra Chart
   may need to redownload/rebuild historical intraday data at the new granularity — budget time for
   that if so.
2. **Re-add or "Reset Instance to Defaults" on the MindfulTrader studies on any chart where they're
   already added.** `sc.MaintainVolumeAtPriceData = 1` is only applied inside each study's
   `SetDefaults` block, which doesn't re-run automatically for an already-configured study instance
   — an existing chart won't pick up this setting until the study is re-added or explicitly reset.
   Applies to both `Mindful Trading System` (`scsf_MindfulTrader`) and `Screen 3 - Keltner Channel`
   (`scsf_Screen3_KeltnerChannel`) on the TS3 (15-minute) chart.

---

## 3. Chartbook restore

Covered in full in `docs/NEW_MACHINE_WSL_SETUP.md` §12a — not duplicated here. Summary: the 10
custom `.Cht` files + `ChartbookSharingSettings.config` are a `gh` release asset
(`sierrachart-chartbooks-20260910`), not git-tracked; restore via `gh release download` + `tar -xzvf`
into the Sierra Chart `Data/` directory. Sierra Chart's own stock sample chartbooks reinstall
automatically and don't need transferring.

---

## 4. Per-machine verification status

Sierra Chart settings are local application state, not something git/config-file transfer carries
over — a fresh install starts every row unverified regardless of what a prior machine had confirmed.

| Setting | Old machine (retired) | Puget (current) |
|---|---|---|
| Time Zone = Eastern Time (§0) | Confirmed set (implicit, undocumented) | **Fixed and confirmed** — log now shows `-04:00:00 (EST-05EDT+01,...)`, correct EDT offset (2026-09-14) |
| Package 11 subscription active | Confirmed active | Not yet re-verified |
| Denali (CME, no depth) feed connected | Confirmed active | **Confirmed live** — `MESZ26-CME.scid` updating in real time (Entry 14) |
| IB Gateway API settings (§1 step 1) | Confirmed configured | TWS running — API settings (port, Read-Only API, ActiveX/Socket Clients) not yet individually confirmed |
| Sierra Chart IB trading-service config (§1 step 2) | Confirmed configured | Not yet done — see above |
| Symbol mapping override (§1 step 3) | Confirmed working | Not yet re-verified |
| Sierra Chart build version (§1 step 4) | Confirmed post-2480 | **Confirmed** — build 2949 (Entry 14) |
| `Intraday Data Storage Time Unit` = 1 Tick (§2.1) | Confirmed set | **Confirmed set** — UI dropdown directly checked, reads "1 Tick"; log's raw enum value `0` = "1 Tick" (mapping now known) (2026-09-14) |
| MindfulTrader studies reset-to-defaults (§2.2) | Confirmed done | Not yet re-verified — new DLL/chartbook, needs redoing regardless |
| Chartbook restore (§3) | N/A (original install) | **Done** — `docs/PUGET_SETUP_COORDINATION.md` Entry 12 |
| `/mnt/c/Trading/config/*.json` live config | Confirmed present | **Done** — `docs/PUGET_SETUP_COORDINATION.md` Entry 12 |
| `/mnt/c/Trading/logs/` directory exists | Confirmed present | **Done** — Entry 14; was missing, `Logger.cpp` silently fails without it (see Entry 14 for the finding) |
| `/mnt/c/Trading/data/{daily_high_low,NH_NL}.csv` fresh | Confirmed current | **Done** — refreshed through 2026-09-11 (Entry 14) |
| MindfulTrader chartbook opened/study attached | N/A (always running) | Not yet confirmed — `MindfulTrader.log` not yet written |

Update this table (not a new copy of it) as each row is confirmed on the current machine.
