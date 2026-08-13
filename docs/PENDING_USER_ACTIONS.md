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

## 6. Dim 11 (`amihud_illiquidity`) — RESOLVED (2026-08-12), fix implemented (commit `b7d3f05`), pending the batched deploy

**Update (2026-08-12):** the empirical re-verification below has concluded — via a live
`ContextManager::ObservationStaleness ALERT` grep of `/mnt/c/Trading/logs/MindfulTrader.log` against
the currently-deployed DLL (`/mnt/c/SierraChart2/Data/MindfulTrader.dll`, built 09:02:32, confirmed to
already include the pre-existing `AMIHUD_ABSOLUTE_FLOOR` fix) — dim 11 is **still** persistently
zero-collapsing: 49 alerts, always exactly `0.0`, max stale run 2,477 samples. The existing floor fix
is confirmed insufficient — item 1 below is the answer.

**Also newly found, while root-causing dim 7's separate once-per-bar timing bug (see
`docs/superpowers/plans/2026-08-12-observation-vector-incremental-accumulators.md`'s "Related finding"
section):** `CalculateAmihudIlliquidity` reads `sc.Volume[sc.Index]` for the current (i=0) term —
`sc.Volume` accumulates throughout the still-forming bar exactly like `sc.AskVolume`/`sc.BidVolume`
did for dim 7. Because this function is called from `UpdateObservationVectorSubgraphs`'s once-per-bar
gate (fires only on the bar's first tick), the current bar's volume is read while still near-zero,
tripping `if (vol < 1.0) continue;` (`StudyHelperFunctions.cpp:3060`) and silently dropping the
current bar's term from the sum on essentially every call. This doesn't by itself explain the
exact-zero-collapse pattern (that's the `FeatureScaler` median mechanism, unchanged) but it is a
related, additional bias worth fixing in the same pass — the current-bar volume should be read at
whatever point is most complete for that bar, not always at its first tick.

**Implemented as (commit `b7d3f05`):**

1. Carry-forward: `CalculateAmihudIlliquidity`'s degenerate branch (previously `if (count < 2) return
   0.0f;`) now calls `cfc::ComputeAmihudIlliquidity(sum, count, lastValidAmihud)`, per the pattern in
   `docs/superpowers/specs/2026-08-12-featurescaler-sentinel-collapse-hardening.md` D1 — the last
   valid value is persisted via `sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::AMIHUD_LAST_VALID_VALUE)`
   instead of collapsing to a fixed `0.0f` sentinel.
2. Current-bar volume timing: the summation loop changed from `for (int i = 0; i < lookback_n; ++i)`
   to `for (int i = 1; i <= lookback_n; ++i)`, excluding the live current bar (i=0) from the window
   entirely rather than merely deferring its read — the window is now built exclusively from
   closed/historical bars, mirroring dim 7's and dim 12's once-per-bar timing fixes.

**Update (2026-08-12, fix round 1):** dim 12's fragility_raw is `0.65 * range_signal + 0.35 *
(range_signal * thinness)`. The `9506dd5` commit above fixed only the `thinness` term's volume read;
`range_signal` — the dominant, 0.65-weighted term — still computed `barRange = sc.High[sc.Index] -
sc.Low[sc.Index]`, reading the live still-forming bar. Since this function is called at the bar's first
tick (same once-per-bar gate as dims 4/7/11), `High == Low` at that moment and `barRange` collapsed to
its `0.00001f` floor on essentially every live call, pinning `fragility_raw` to roughly `[0.0064,
0.0099]` regardless of actual market conditions. Fixed in commit `f6f112d`
(`fix(microstructure): closed-bar-only range read for liq_fragility dominant term (dim 12)`) —
`barRange` now reads `sc.High[sc.Index - 1] - sc.Low[sc.Index - 1]`, mirroring the pattern already used
for the volume term six lines below.

**Also decided and deliberately deferred (2026-08-12):** `CalculateRealizedVarianceRatio` (dim 3,
`correction_action`) still reads `sc.Close[sc.Index]` (the live current bar) as its `i=0` term every
tick. This was reviewed and NOT changed to closed-bar-only in this plan, because unlike
`sc.Volume`/`sc.BaseData[SC_HIGH/LOW]` (accumulators that are genuinely incomplete early in a bar),
`sc.Close` mid-bar is simply "the current best-known price" — a legitimate real-time quantity, not a
partially-formed one. If this needs to change, it should be its own future decision, not bundled here.

See section 8 below for the batched-deploy tracking and re-verification plan covering this fix
alongside dims 1/2/3/4/7/8/11.

---

## 7. Dim 9 (`tail_index`) long stale runs — investigated, CLOSED (2026-08-12): organic, no bug

`ContextManager::ObservationStaleness ALERT` showed 56 alerts for dim 9 (`tail_index`, Hill estimator
alpha from `TailRiskEngine`), max stale run 10,141 samples, values varying (not a single frozen
sentinel like dims 1/7/11). Investigated per
`docs/superpowers/plans/2026-08-12-remaining-observation-vector-dims.md` Task 5.

**Conclusion: organic, not the Tasks 1-4 once-per-bar timing bug family and not a warmup-reset
artifact.** `TailRiskEngine::AddObservation` (feeding dim 9) is only called from
`EventDataCollectorStudy.cpp` when `sc.Close[sc.Index]` actually changes
(`currentPrice != s_lastPhysicsPrice`), but the staleness-telemetry counter increments on every
`CheckAndTriggerHMM` call regardless of whether a new price print occurred. Long stale runs correspond
to real stretches of the replayed historical data with no new price observation (thin-liquidity/quiet
windows), not a computation defect: `GetHillAlpha()` is a pure function of the circular buffer's
contents, so an unfed buffer produces a bit-identical result on every call, which is correct. Only one
`Context reset generation=` occurred in the analyzed log (well before nearly all 56 alerts), ruling out
warmup-reset churn as the driver. Corroborating evidence: dim 9 goes stale in the same telemetry windows
as several structurally unrelated dims (dim1, dim2, dim7, dim8, dim11, dim14, fed by different code
paths), consistent with a shared "no new price data" cause rather than a defect isolated to
`TailRiskEngine`. No code change made or required.

---

## 8. Batched deploy of dims 1/2/3/4/7/8/11/12 fixes + post-deploy re-verification

**Bundled commits:**

- Dims 1/2/8 (`burstiness_index`, `relative_range`, `fisher_info` carry-forward instead of a fixed
  `0.0f` sentinel): `e6b4003`
- Dim 7 (`micro_asymmetry` computed every tick instead of once per bar): `90b17fc`
- Dim 3 (`correction_action` carry-forward via the existing burstiness formula): `241b4c0`
- Dim 4 (`vol_convexity` closed-bar-only window): `008b421`
- Dim 11 (`amihud_illiquidity` carry-forward + closed-bar-only window): `b7d3f05`
- Dim 12 (`liq_fragility`): thinness/volume term `9506dd5`, dominant range_signal term `f6f112d`

**Deploy steps:**

1. Build: `./build_dll.sh --no-clean` (must succeed with 0 errors).
2. Copy the resulting `bin/MindfulTrader.dll` to the currently-deployed DLL path,
   `/mnt/c/SierraChart2/Data/MindfulTrader.dll` (same path referenced in this section's history above).
3. Restart Sierra Chart (or reload the study instance) so the new DLL is actually picked up.
4. Run a historical replay through `BackTesterStudy` (or a live session) long enough to accumulate
   several thousand `ContextManager::ObservationStaleness ALERT` samples.
5. Grep `/mnt/c/Trading/logs/MindfulTrader.log` for `ObservationStaleness ALERT` and tabulate, per dim,
   the alert count, max stale run, and whether values vary or sit at a single frozen sentinel — the
   same method used to verify dim 11 above.

**Per-dim acceptance criteria:**

- **Dims 4, 11, 12 (now bar-quantized / closed-bar-only):** these dims recompute once per closed bar,
  not continuously. The correct post-deploy signature is "value changes at bar boundaries and is
  non-zero/non-degenerate" — NOT "low stale-run count." A bar-quantized value legitimately shows a
  longer stale run within a single bar's ticks than before (it is correctly frozen for the whole bar by
  design), and that is not a regression.
  - **Dim 4 specifically: its alert COUNT is expected to INCREASE from its pre-fix baseline of 9
    alerts.** A bit-identical value across a whole bar's ticks is now correct behavior, not a bug — do
    not misread a higher post-deploy count as new breakage.
  - Dim 11: confirm it no longer collapses to exactly `0.0` (pre-fix: 49 alerts, always exactly `0.0`,
    max stale run 2,477). Post-fix it should show non-zero, plausible values that change at bar
    boundaries.
  - Dim 12: confirm `fragility_raw` is no longer pinned to roughly `[0.0064, 0.0099]` — it should range
    meaningfully wider (design intent: ~0.0-0.35 normal, ~0.4-0.8 stressed) and change at bar
    boundaries.
- **Dim 3 (`correction_action`):** expect the exact-zero-collapse pattern to be gone — carry-forward is
  now active on the degenerate branch, so the value should hold the last valid reading instead of
  resetting to `0.0`.
- **Dims 1/2/7/8:** confirm no `stale_run` matching a single frozen sentinel value the way dims 1/7/11
  did pre-fix — carry-forward should be visibly holding the last valid physics reading, not a fixed
  `0.0`.

---

## 9. Carry-forward hardening for dims 10/13/14/15 + StatisticalContext.relRange + FeatureScaler D2 diagnostic (2026-08-12)

**Bundled commits:**

- Dim 10 (`skewness_idx` carry-forward on degenerate variance): `e4a57d8`
- Dim 13 (`recurrence_rate` carry-forward on flat price window): `17a1a3c`
- Dim 14 (`fractal_dim` carry-forward on flat price window): `470d7ec`
- Dim 15 (`mean_rev_z` carry-forward on degenerate std-dev): `9f83127`
- StatisticalContext.relRange (carry-forward on ATR edge case): `9679f4c`
- FeatureScaler dominance diagnostic (new `ALERT` for single-value ratios on adaptive dims): `97540e3`

**Deploy steps:**

1. Build: `./build_dll.sh --no-clean` (must succeed with 0 errors).
2. Copy the resulting `bin/MindfulTrader.dll` to the currently-deployed DLL path,
   `/mnt/c/SierraChart2/Data/MindfulTrader.dll`.
3. Restart Sierra Chart (or reload the study instance) so the new DLL is actually picked up.
4. Run a historical replay through `BackTesterStudy` (or a live session) long enough to accumulate
   several thousand `ContextManager::ObservationStaleness ALERT` samples.
5. Grep `/mnt/c/Trading/logs/MindfulTrader.log` for `ObservationStaleness ALERT` and `FeatureScaler dominance ALERT` and tabulate per dim, the alert count, max stale run, and whether values vary or sit at a single frozen sentinel.

**Per-dim acceptance criteria:**

- **Dims 10, 13, 14, 15 (carry-forward degenerate branches):** confirm `ObservationStaleness` alerts no longer show a single frozen sentinel value (e.g., `0.0`); carried values should match the last valid physics reading and change at legitimate degenerate-run boundaries, not remain frozen indefinitely.
- **StatisticalContext.relRange:** confirm it no longer collapses to a single frozen sentinel on ATR edge cases; carried values should be non-zero and plausible.
- **FeatureScaler dominance ALERT (new diagnostic, Task 6):** this line should be checked **first** for dims 1/2 to investigate the still-open zero-ratio problem documented in `docs/superpowers/plans/2026-08-12-statistical-context-relrange-sentinel-gap.md` Problem 2. The new diagnostic logs ratio threshold and whether it fired; if it does fire for dims 1/2, that is the leading indicator for why the carry-forward safety net is being triggered. Do not attempt to fix Problem 2 in this deploy cycle — the diagnostic output will guide the next investigation.

---

*Generated 2026-08-04, alongside the Volume Profile Value Area feature (`docs/superpowers/plans/2026-08-04-volume-profile-daily-bias.md`, merged to `master` at `8251fb7`). Section 5 added 2026-08-04 alongside the Phase 1 hardening branch (`docs/superpowers/plans/2026-08-04-phase1-hardening.md`). Section 6 added 2026-08-12 alongside the tick-native micro-asymmetry fix (`docs/superpowers/plans/2026-08-12-tick-native-toxicity-illiquidity.md`), Section 6 updated 2026-08-12 alongside this plan (`docs/superpowers/plans/2026-08-12-remaining-observation-vector-dims.md`) to mark the fix implemented. Section 6 updated again and Section 8 added 2026-08-12 alongside this plan's fix round 1 (dim 12's dominant-term fix), restoring a concrete batched-deploy verification recipe and recording dim 3's live-current-bar read as a deliberate, reviewed deferral; Section 7 updated to remove its dangling gitignored `.superpowers/sdd/**` report reference. Section 9 added 2026-08-12 alongside the carry-forward completion batch (`docs/superpowers/sdd/2026-08-12-observation-vector-carry-forward-completion/task-7-brief.md`), recording all five fixes and the diagnostic as pending deploy + re-verification.*
