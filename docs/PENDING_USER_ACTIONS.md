# Pending User Actions

Everything below requires hands-on Sierra Chart access or a decision only you can make — none of it is blocked on further coding. Grouped by what depends on what.

---

## 1. Sierra Chart data feed setup (Package 11 + Denali + IB) — not yet executed

From `docs/ADR/sierra_chart_data_feed_setup.md`, "Setup steps (not yet executed)":

1. **TWS/IB Gateway**: File → Global Configuration → API → Settings — enable "ActiveX and Socket Clients," port 7496 (live) / 7497 (paper), disable "Read-Only API," Component Exchange Separator = `/`.
2. **Sierra Chart**: Global Settings → Data/Trade Service Settings → select Interactive Brokers as the *trading* service, `127.0.0.1:7496`, unique Instance Client ID, enable "Connect On Program Startup" / "Reconnect On Failure." Denali continues to supply chart data automatically for subscribed exchanges.
3. **Verify symbol mapping at setup time, don't trust the doc blindly**: Denali's native Sierra Chart symbol vs. IB's own ES format (`ES-YYYYMM-GLOBEX`) differ. Confirm the exact current trade-symbol-override UI path against Sierra Chart's own setup wizard/support board.
4. **Confirm a recent Sierra Chart build** (a pre-version-2480 IB-integration issue was flagged by Sierra Chart's own team — almost certainly moot now, worth a one-line version check before going live).

---

## 2. Volume Profile Value Area — prerequisites (before the feature can be trusted at all)

1. **Confirm the Intraday Data Storage Time Unit setting.** Sierra Chart → Global Settings → Data/Trade Service Settings → `Intraday Data Storage Time Unit` must be **1 Tick**. If it's coarser, `sc.VolumeAtPriceForBars` will be empty or inaccurate. Note whether Sierra Chart needs to redownload/rebuild historical intraday data at the new granularity, and budget time for that if so.
2. **Re-add or "Reset Instance to Defaults" on the MindfulTrader studies on any chart where they're already added.** `sc.MaintainVolumeAtPriceData = 1` is only applied inside each study's `SetDefaults` block, which doesn't re-run automatically for an already-configured study instance — so an existing chart won't pick up this setting until you re-add the study or explicitly reset it to defaults. This applies to both `Mindful Trading System` (`scsf_MindfulTrader`) and `Screen 3 - Keltner Channel` (`scsf_Screen3_KeltnerChannel`) on the TS3 (15-minute) chart.

---

## 3. Volume Profile Value Area — empirical replay verification (before enabling the feature)

Several design decisions were made "safe-by-construction" (a wrong guess falls back to the old proxy, never produces a silently-wrong value) specifically because they couldn't be fully confirmed from Sierra Chart's SDK headers alone — only a real replay can close these out:

1. Run a short historical replay through `BackTesterStudy`.
2. Confirm `IndicatorManager::GetCachedValueAreaLow()`/`GetCachedValueAreaHigh()` return non-zero, plausible ES price levels once warmed up — not `0.0f`, not identical to `prevDayLow`/`prevDayHigh`.
3. **Specifically check a Monday's first RTH bars.** Confirm `prevDayHigh`/`prevDayLow` (and the Value Area, once enabled) resolve to **Friday's** actual values, not zero, not a stale day, not Thursday's.
4. Check the Sierra Chart log for `"Volume Profile Value Area aggregation found no valid RTH session within 7 days"` — this should appear only around genuine multi-day gaps (e.g. holidays), never on an ordinary Monday or weekday.
5. This closes out three specific residual uncertainties nothing but a live Sierra Chart instance can confirm: the `GetFirstIndexForDate`/`GetTradingDayDate` interaction, `GetOHLCForDate`'s exact date-vs-time interpretation, and whether `MaintainVolumeAtPriceData`/`VolumeAtPriceForBars` is truly chart-wide or per-study-interface (mitigated defensively either way, but worth confirming).

---

## 4. Train/live parity decision — before turning the feature on live

**Do not enable the "Enable Real Volume Profile Daily Bias (EXPERIMENTAL - requires HMM retrain, see plan doc)" input** (on the `Mindful Trading System` study) for live trading yet. It defaults to **No/off**, and it needs to stay off until one of:

- **(a) Coordinated retrain (recommended path, per `lbrnet/logs/rc_gemini.log` `GEMINI_REVIEW_079`):** run a historical replay with the flag turned **on** to generate a fresh `.context` file reflecting the real Value Area's distribution, retrain the HMM/Transformer on the `lbrnet` side against that data, then flip the C++ flag on live in one coordinated cutover.
- **(b) An explicit decision** to leave it off until the next already-scheduled retrain cycle absorbs it.

`daily_bias` is HMM observation dimension [18]; the currently-deployed model was fitted on the *old* proxy's semantics (71,218 observations). Enabling the flag without a corresponding retrain would silently diverge live inference from what the model was trained on.

---

## 5. Risk/Scoring threshold recalibration — after the Phase 1 hardening branch merges

`docs/superpowers/plans/2026-08-04-phase1-hardening.md`'s final whole-branch review found and fixed a Critical bug in `InformationEngine::MapToBin()`/`UpdateHistogram()`: volatility-standardizing the entropy bins (the branch's own Task 2) made `MapToBin()` time-varying, but the histogram eviction logic still re-derived bins from the live (drifted) sigma instead of the sigma at insertion time — desyncing histogram totals from window counts and driving `GetShannonEntropy()` negative in measured testing. This has been fixed (per-slot insertion-time bin recording), independently verified (0 desyncs across a 20k-observation stress test, vs. 19,947/20,000 before the fix), and a regression test now guards it.

**Consequence you need to act on:** fixing this bug legitimately shifted the Shannon entropy distribution the rest of the system was calibrated against — measured mean entropy on realistic ES-tick-scale synthetic data moved from **1.15 bits (pre-Task-2, broken-old-formula baseline) to 1.71 bits (post-fix, correct)**. Two places still hold thresholds tuned against the OLD (uncorrected) distribution:

1. **`shannonEntropyHaltFrac`** (`include/ExecutionParams.h`) — the RiskManager chaos-halt gate (`src/RiskManager.cpp`) compares live entropy against `shannonEntropyHaltFrac * log2(10)`. Never recalibrated against the new, correct distribution.
2. **`src/Scoring.cpp`**'s directional/mean-reversion multiplier fractions (currently `0.80/0.60/0.45/0.85` against `kShannonMaxEntropyBits`) — same issue.

**Recommended path:** run a historical replay through `BackTesterStudy` after this branch merges, collect a fresh entropy distribution under the corrected code, and compare the halt-gate and scoring-fraction thresholds against it before relying on either in live trading. Do not assume the old threshold values are still meaningful without this check — they were tuned against a bug.

---

## 6. Dim 11 (`amihud_illiquidity`) empirical re-verification — after this DLL is deployed

From `docs/superpowers/specs/2026-08-12-tick-native-toxicity-illiquidity-design.md`: dim 11's
FeatureScaler zero-collapse may already be resolved by the existing `AMIHUD_ABSOLUTE_FLOOR` fix in
`FeatureScaler.h` — the `.context` file this was diagnosed from may predate that fix reaching a
deployed build.

1. Deploy the DLL built by `docs/superpowers/plans/2026-08-12-tick-native-toxicity-illiquidity.md`'s
   Task 4 and run a historical replay through `EventDataCollectorStudy` to produce a fresh `.context`
   file.
2. Run `context_preflight.py`'s octile zero-ratio analysis (see
   `docs/superpowers/plans/2026-08-12-observation-vector-sentinel-collapse-audit.md` §2 for the
   method) against the fresh file, specifically for dim 11.
3. **If dim 11's zero-ratio now shows the same warmup-decay-then-flatten shape dim 0 already
   shows** (flattening to a low single-digit percentage, not a persistent ~40%): no further C++
   change needed — close this out.
4. **If it doesn't**: apply the carry-forward pattern from
   `docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md` D1
   (`CalculateAmihudIlliquidity`'s `count < 2` branch) as originally speced.

---

*Generated 2026-08-04, alongside the Volume Profile Value Area feature (`docs/superpowers/plans/2026-08-04-volume-profile-daily-bias.md`, merged to `master` at `8251fb7`). Section 5 added 2026-08-04 alongside the Phase 1 hardening branch (`docs/superpowers/plans/2026-08-04-phase1-hardening.md`). Section 6 added 2026-08-12 alongside the tick-native micro-asymmetry fix (`docs/superpowers/plans/2026-08-12-tick-native-toxicity-illiquidity.md`).*
