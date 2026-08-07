#include "MindfulTrader_Precompiled.h"

/*==========================================================================*/
// Helper function prototypes
EmaEnum GetEmaEnum(float ema, float prevEma);
FI13Enum GetFI_13(float force, float prevForce, float ema_delta, float low_delta, float high_delta, float forceStdDev);

namespace {
enum class NhNlLogMode : int {
    OFF = 0,
    ERRORS_ONLY = 1,
    VERBOSE = 2,
};

// Temporary operational switch for NH-NL logging during long backfills.
// OFF: disable all NH-NL logs from this study.
// ERRORS_ONLY: keep loader-failure warnings only.
// VERBOSE: emit diagnostics and stale/missing telemetry.
constexpr NhNlLogMode kNhNlLogMode = NhNlLogMode::ERRORS_ONLY;

inline bool ShouldLogNhNlErrorsOnly() {
    return kNhNlLogMode != NhNlLogMode::OFF;
}

inline bool ShouldLogNhNlVerbose() {
    return kNhNlLogMode == NhNlLogMode::VERBOSE;
}

enum class Ts1MacroObsLogMode : int {
    OFF = 0,
    IMPORTANT_ONLY = 1,
    VERBOSE = 2,
};

// TS1 macro-observation diagnostics were used during WS-09 investigation.
// Keep disabled by default to avoid polluting runtime logs.
constexpr Ts1MacroObsLogMode kTs1MacroObsLogMode = Ts1MacroObsLogMode::OFF;
constexpr uint64_t kTs1MacroDigestEveryWrites = 500;
constexpr uint64_t kTs1MacroStaleWarnEveryWrites = 2000;
constexpr uint64_t kTs1MacroNonFiniteWarnEveryWrites = 1000;
constexpr float kTs1MacroChangeEps = 1e-6f;
constexpr uint64_t kTs1EarlyReturnLogEvery = 2000;

inline bool ShouldLogTs1MacroImportant() {
    return kTs1MacroObsLogMode != Ts1MacroObsLogMode::OFF;
}

inline bool ShouldLogTs1MacroVerbose() {
    return kTs1MacroObsLogMode == Ts1MacroObsLogMode::VERBOSE;
}

inline bool IsMacroValueChanged(float prev, float current) {
    return std::fabs(current - prev) > kTs1MacroChangeEps;
}

inline bool ShouldLogTs1EarlyReturn(uint64_t count) {
    return count <= 5 || (count % kTs1EarlyReturnLogEvery) == 0;
}
}  // namespace

/*==========================================================================*/
/*
 * scsf_Screen1_Impulse
 *
 * Elder Triple Screen - Screen 1: Impulse System
 *
 * Analyzes trend direction using 13-period EMA of bar-to-bar MACD deltas.
 * Colors bars green (bullish impulse) or red (bearish impulse).
 * Includes market regime detection (ADX/ATR) for data collection.
 *
 * References:
 * - Elder, Alexander. "Come Into My Trading Room" (2002)
 * - Official Sierra Chart MACD pattern from Studies7.cpp
 */
SCSFExport scsf_Screen1_Impulse(SCStudyInterfaceRef sc)
{
    SCSubgraphRef Subgraph_ImpulseEMA = sc.Subgraph[0];
    SCSubgraphRef Subgraph_ColorBar = sc.Subgraph[1];
    SCSubgraphRef Subgraph_ADX = sc.Subgraph[2];          // ADX(14) for market regime
    SCSubgraphRef Subgraph_ATR = sc.Subgraph[3];          // ATR(14) for market regime
    SCSubgraphRef Subgraph_ATRAvg = sc.Subgraph[4];       // 20-period SMA of ATR
    SCSubgraphRef Subgraph_ColorBarExport = sc.Subgraph[5];  // ColorBar duplicate for cross-chart access

    SCInputRef Input_InputData = sc.Input[0];
    SCInputRef Input_MACDStudy = sc.Input[1];

    if (sc.SetDefaults)
    {
        sc.GraphName = "Screen 1 - Impulse System";
        sc.ValueFormat = VALUEFORMAT_INHERITED;
        sc.FreeDLL = 0;
        sc.GraphRegion = 0;
        sc.AutoLoop = 1;
        sc.GraphDrawType = GDT_CUSTOM;

        Subgraph_ImpulseEMA.Name = "Impulse EMA";
        Subgraph_ImpulseEMA.DrawStyle = DRAWSTYLE_LINE;

        Subgraph_ColorBar.Name = "Color Bar";
        Subgraph_ColorBar.DrawStyle = DRAWSTYLE_COLORBAR;
        Subgraph_ColorBar.DrawZeros = false;

        // ColorBar duplicate for cross-chart study reference (COLORBAR not selectable in cross-chart dialogs)
        Subgraph_ColorBarExport.Name = "Color Bar (Export)";
        Subgraph_ColorBarExport.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_ColorBarExport.DrawZeros = false;

        // Hidden subgraphs for market regime calculation
        Subgraph_ADX.Name = "ADX [RETIRED]";
        Subgraph_ADX.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_ATR.Name = "ATR(14)";
        Subgraph_ATR.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_ATRAvg.Name = "ATR 20-Median";
        Subgraph_ATRAvg.DrawStyle = DRAWSTYLE_IGNORE;

        Input_InputData.Name = "Input Data";
        Input_InputData.SetInputDataIndex(SC_LAST);

        Input_MACDStudy.Name = "MACD Study to Reference";
        Input_MACDStudy.SetStudyID(0); // This is a required setting for referencing other studies

        // Draw horizontal lines for previous day high/low (visual cue)
        if (DailyHighLowLoader::Instance().IsLoaded()) {
            DailyHighLowData dailyData = DailyHighLowLoader::Instance().GetMostRecentData();
            if (dailyData.prevDayHigh > 0.0 && dailyData.prevDayLow > 0.0) {
                DrawStudyHorizontalLine(sc, 1000, static_cast<float>(dailyData.prevDayHigh), RGB(56, 189, 248), 2, LINESTYLE_DASH);
                DrawStudyHorizontalLine(sc, 1001, static_cast<float>(dailyData.prevDayLow), RGB(56, 189, 248), 2, LINESTYLE_DASH);
            }
        }

        return;
    }

    // ========================================================================
    // DETERMINISTIC RESET: Clear buffers on chart reload or symbol change
    // ========================================================================
    // This ensures circular buffers don't carry "ghost data" from previous
    // session that would pollute observation features used for training.
    // Called on sc.IsFullRecalculation (chart reload, symbol change, parameters modified).
    if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0) {
        ResetAdaptiveCalculators(sc);
    }

    // Set minimum bars required (MUST be outside SetDefaults)
    // Requirements: ADX(14) + ATR(14) + 20-period SMA of ATR
    sc.DataStartIndex = 20;

    // Initialize NH-NL data loader (persistent with bounded retry)
    static NhNlDataLoader* s_nhNlLoader = nullptr;
    static int s_lastNhNlLoadRetryDateKey = 0;
    static bool s_loggedNhNlRecovery = false;
    if (s_nhNlLoader == nullptr) {
        s_nhNlLoader = new NhNlDataLoader();
        if (!s_nhNlLoader->LoadDefaultPath(sc)) {
            if (ShouldLogNhNlErrorsOnly()) {
                Logger::getInstance().log("Warning: NH-NL data not loaded. EXTREME_DISLOCATION detection disabled.");
            }
        }
    }

    // Institutional resiliency: if initial load failed, retry once per trading day.
    // Without this, a transient startup failure can keep NH-NL at zero for the entire session.
    if (s_nhNlLoader != nullptr && !s_nhNlLoader->IsLoaded() && sc.Index >= sc.DataStartIndex) {
        int ry = 0, rm = 0, rd = 0;
        sc.BaseDateTimeIn[sc.Index].GetDateYMD(ry, rm, rd);
        const int retryDateKey = ry * 10000 + rm * 100 + rd;

        if (retryDateKey != s_lastNhNlLoadRetryDateKey) {
            s_lastNhNlLoadRetryDateKey = retryDateKey;
            if (s_nhNlLoader->LoadDefaultPath(sc)) {
                if (!s_loggedNhNlRecovery) {
                    if (ShouldLogNhNlVerbose()) {
                        Logger::getInstance().log("NH-NL loader recovered after initial failure; NH-NL signals re-enabled.");
                    }
                    s_loggedNhNlRecovery = true;
                }
            }
        }
    }

    // DailyHighLowLoader auto-loads from C:/Trading/data/daily_high_low.csv

    // Get the MACD subgraph data from the referenced study
    SCFloatArray MACDSubgraphArray;
    SCFloatArray MACDDiffSubgraphArray;

    static uint64_t s_ts1MissingMacdMainCount = 0;
    static uint64_t s_ts1MissingMacdDiffCount = 0;
    static uint64_t s_ts1MacdArraySizeMismatchCount = 0;
    static uint64_t s_ts1EarlyIndexGateCount = 0;

    const int macdStudyId = Input_MACDStudy.GetStudyID();
    static uint64_t s_ts1EmptyMacdStudyRefCount = 0;
    if (macdStudyId <= 0) {
        ++s_ts1EmptyMacdStudyRefCount;
        if (ShouldLogTs1MacroImportant() && ShouldLogTs1EarlyReturn(s_ts1EmptyMacdStudyRefCount)) {
            Logger::getInstance().log(
                "TS1 MacroObs early-return: MACD Study to Reference is empty/unset (study_id<=0); count=" +
                std::to_string(s_ts1EmptyMacdStudyRefCount)
            );
        }
        return;
    }

    if (!sc.GetStudyArrayUsingID(macdStudyId, 0, MACDSubgraphArray))
    {
        ++s_ts1MissingMacdMainCount;
        if (ShouldLogTs1MacroImportant() && ShouldLogTs1EarlyReturn(s_ts1MissingMacdMainCount)) {
            Logger::getInstance().log(
                "TS1 MacroObs early-return: missing MACD main array; count=" +
                std::to_string(s_ts1MissingMacdMainCount) +
                " study_id=" + std::to_string(macdStudyId)
            );
        }
        return;
    }

    if (!sc.GetStudyArrayUsingID(macdStudyId, 2, MACDDiffSubgraphArray))
    {
        ++s_ts1MissingMacdDiffCount;
        if (ShouldLogTs1MacroImportant() && ShouldLogTs1EarlyReturn(s_ts1MissingMacdDiffCount)) {
            Logger::getInstance().log(
                "TS1 MacroObs early-return: missing MACD diff array; count=" +
                std::to_string(s_ts1MissingMacdDiffCount) +
                " study_id=" + std::to_string(macdStudyId)
            );
        }
        return;
    }

    // Validate study array sizes match current chart (Sierra Chart pattern)
    if (MACDSubgraphArray.GetArraySize() < sc.ArraySize || MACDDiffSubgraphArray.GetArraySize() < sc.ArraySize)
    {
        ++s_ts1MacdArraySizeMismatchCount;
        if (ShouldLogTs1MacroImportant() && ShouldLogTs1EarlyReturn(s_ts1MacdArraySizeMismatchCount)) {
            Logger::getInstance().log(
                "TS1 MacroObs early-return: MACD array size mismatch; count=" +
                std::to_string(s_ts1MacdArraySizeMismatchCount) +
                " main_size=" + std::to_string(MACDSubgraphArray.GetArraySize()) +
                " diff_size=" + std::to_string(MACDDiffSubgraphArray.GetArraySize()) +
                " chart_size=" + std::to_string(sc.ArraySize)
            );
        }
        return;
    }

    sc.ExponentialMovAvg(sc.BaseDataIn[Input_InputData.GetInputDataIndex()], Subgraph_ImpulseEMA, 13);

    // Calculate ADX(14) and ATR(14) for market regime detection
    // ADX computation retired — Hurst exponent (TS2) provides superior persistence measurement.
    // Subgraph_ADX slot preserved as DRAWSTYLE_IGNORE to avoid index shifts.
    sc.ATR(sc.BaseDataIn, Subgraph_ATR, 14, MOVAVGTYPE_WILDERS);
    sc.MovingMedian(Subgraph_ATR, Subgraph_ATRAvg, 20);  // Median: robust to fat-tail ATR spikes

    // Provide ATR values to RiskManager for volatility-based position sizing.
    // ATR14Avg uses the 20-bar Moving Median (Sierra Chart built-in) instead
    // of SMA, so a single 10× ATR event doesn't inflate the denominator and
    // distort sizing for the next 20 bars.
    RiskManager::Instance().SetATR14(Subgraph_ATR[sc.Index]);
    RiskManager::Instance().SetATR14Avg(Subgraph_ATRAvg[sc.Index]);

    // Gap 1: Feed PositionManager so it avoids redundant per-tick TR loops
    PositionManager::Instance().SetCachedATR14(Subgraph_ATR[sc.Index]);

    // Update global ADX indicator state (Regime Awareness) - DISABLED
    // auto adxIndicator = IndicatorManager::Instance().GetIndicator<AdxIndicator>(IndicatorKey::ADX_14);
    // if (adxIndicator) {
    //     adxIndicator->Update(Subgraph_ADX[sc.Index]);
    // }

    // Bounds check before accessing sc.Index - 1 (Sierra Chart pattern)
    if (sc.Index < 1)
    {
        ++s_ts1EarlyIndexGateCount;
        if (ShouldLogTs1MacroImportant() && ShouldLogTs1EarlyReturn(s_ts1EarlyIndexGateCount)) {
            Logger::getInstance().log(
                "TS1 MacroObs early-return: index gate (sc.Index < 1); count=" +
                std::to_string(s_ts1EarlyIndexGateCount)
            );
        }
        return;
    }

    Subgraph_ColorBar[sc.Index] = 1;
    const float maDiff = Subgraph_ImpulseEMA[sc.Index] - Subgraph_ImpulseEMA[sc.Index - 1];
    const float macdDiff = MACDDiffSubgraphArray[sc.Index] - MACDDiffSubgraphArray[sc.Index - 1];
    Subgraph_ColorBar.DataColor[sc.Index] = GetImpulse(maDiff, macdDiff);

    // Copy ColorBar data to export subgraph for cross-chart access
    Subgraph_ColorBarExport[sc.Index] = static_cast<float>(Subgraph_ColorBar.DataColor[sc.Index]);

    auto& indMgr = IndicatorManager::Instance();

    // Update indicators with new impulse value (v5.2: pass macdDiff + ATR for derived metrics)
    auto longImpulse = indMgr.GetIndicator<Impulse>(IndicatorKey::LONG_IMP);
    if (longImpulse) longImpulse->SetFromColor(Subgraph_ColorBar.DataColor[sc.Index], Subgraph_ColorBar.DataColor[sc.Index - 1], maDiff, macdDiff, Subgraph_ATR[sc.Index]);

    // Calculate and update market regime (240-min timeframe)
    // Event-driven: update on every tick so downstream HMM consumers always
    // see fresh observation values.  The underlying calculations use adaptive
    // lookback windows and are numerically stable across intra-bar updates.
    if (sc.Index >= 20) {
        int barIndex = sc.Index;

        // CRITICAL: Use PREVIOUS trading day's NH-NL (1-day lag for realistic trading)
        // Elder: "NH-NL is calculated end-of-day. Today's trading uses yesterday's NH-NL."
        // Prevents lookahead bias in training data and matches live trading conditions
        SCDateTime previousDay = sc.BaseDateTimeIn[barIndex] - 1.0;

        static int s_lastNhNlStaleWarnKey = 0;
        static int s_lastNhNlDiagnosticLogKey = 0;
        static bool s_loggedNhNlReplaySuppression = false;
        static bool s_loggedNhNlPreCoverage = false;

        int nh_nl_weekly = 0;
        int nh_nl_daily = 0;
        int nh_nl_prev_weekly = 0;

        NhNlData nhNlData;
        bool hasNhNlData = false;
        const bool isReplay = sc.IsReplayRunning();

        if (s_nhNlLoader != nullptr && s_nhNlLoader->IsLoaded()) {
            SCDateTime firstDate;
            SCDateTime lastDate;
            const bool hasDateRange = s_nhNlLoader->GetDateRange(firstDate, lastDate);
            const bool targetBeforeCoverage =
                hasDateRange && (previousDay.GetDate() < firstDate.GetDate());

            if (!targetBeforeCoverage) {
                hasNhNlData = s_nhNlLoader->TryGetDataForDate(previousDay, nhNlData);
            } else if (isReplay && !s_loggedNhNlPreCoverage) {
                s_loggedNhNlPreCoverage = true;
                SCString preCoverageMsg;
                preCoverageMsg.Format(
                    "NH-NL pre-coverage segment detected in replay: target dates precede CSV start %s. "
                    "Suppressing per-day NH-NL stale warnings until coverage begins.",
                    sc.DateTimeToString(firstDate, FLAG_DT_COMPLETE_DATE).GetChars()
                );
                if (ShouldLogNhNlVerbose()) {
                    Logger::getInstance().log(preCoverageMsg.GetChars());
                }
            }

            if (hasNhNlData) {
                nh_nl_daily = nhNlData.nh_nl_daily;   // Daily signal source
                nh_nl_weekly = nhNlData.nh_nl_weekly; // Weekly context from loader (provided or derived)

                int logYear, logMonth, logDay, logHour, logMinute, logSecond;
                previousDay.GetDateTimeYMDHMS(logYear, logMonth, logDay, logHour, logMinute, logSecond);
                const int diagnosticLogKey = logYear * 10000 + logMonth * 100 + logDay;
                if (ShouldLogNhNlVerbose() && !isReplay && diagnosticLogKey != s_lastNhNlDiagnosticLogKey) {
                    s_lastNhNlDiagnosticLogKey = diagnosticLogKey;

                    SCString diag;
                    diag.Format(
                        "NH-NL DIAGNOSTIC: target_date=%s ymd=%04d-%02d-%02d key=%d nh_nl_daily=%d nh_nl_weekly=%d",
                        sc.DateTimeToString(previousDay, FLAG_DT_COMPLETE_DATE).GetChars(),
                        logYear,
                        logMonth,
                        logDay,
                        diagnosticLogKey,
                        nh_nl_daily,
                        nh_nl_weekly
                    );
                    Logger::getInstance().log(diag.GetChars());
                }
            } else {
                if (isReplay) {
                    if (ShouldLogNhNlVerbose() && !s_loggedNhNlReplaySuppression) {
                        s_loggedNhNlReplaySuppression = true;
                        Logger::getInstance().log(
                            "NH-NL stale/missing per-day logs suppressed during replay/backfill "
                            "to prevent log spam while collecting long-range context data."
                        );
                    }
                    // Replay/backfill path intentionally suppresses per-day stale warnings.
                } else {
                    int warnDateKey = 0;
                    int wy, wm, wd, wh, wmin, ws;
                    previousDay.GetDateTimeYMDHMS(wy, wm, wd, wh, wmin, ws);
                    warnDateKey = wy * 10000 + wm * 100 + wd;

                    if (ShouldLogNhNlVerbose() && warnDateKey != s_lastNhNlStaleWarnKey) {
                        s_lastNhNlStaleWarnKey = warnDateKey;
                        SCDateTime firstDate, lastDate;
                        SCString msg;
                        if (s_nhNlLoader->GetDateRange(firstDate, lastDate)) {
                            msg.Format(
                                "NH-NL missing for target date %s ymd=%04d-%02d-%02d key=%d (exact date required). "
                                "CSV range=%s to %s. Check C:/Trading/data/NH_NL.csv.",
                                sc.DateTimeToString(previousDay, FLAG_DT_COMPLETE_DATE).GetChars(),
                                wy,
                                wm,
                                wd,
                                warnDateKey,
                                sc.DateTimeToString(firstDate, FLAG_DT_COMPLETE_DATE).GetChars(),
                                sc.DateTimeToString(lastDate, FLAG_DT_COMPLETE_DATE).GetChars()
                            );
                        } else {
                            msg.Format(
                                "NH-NL missing for target date %s ymd=%04d-%02d-%02d key=%d (exact date required).",
                                sc.DateTimeToString(previousDay, FLAG_DT_COMPLETE_DATE).GetChars(),
                                wy,
                                wm,
                                wd,
                                warnDateKey
                            );
                        }
                        Logger::getInstance().log(msg.GetChars());
                    }
                }
            }
        }

        // ADX retired from risk/scoring pipeline (Hurst provides superior trend persistence).
        // Subgraph computation retained for chart overlay only.

        // Calculate and update NH-NL signal using Alexander Elder's methodology
        // Elder's approach: Compare price extremes with breadth trends to detect divergences
        // This provides powerful reversal signals and trend health assessment
        // CRITICAL: Use previousDay (already calculated above) for all NH-NL lookups
        if (hasNhNlData && barIndex >= 1) {
            // Previous-week context using strict exact-date lookup.
            SCDateTime twoDaysBack = sc.BaseDateTimeIn[barIndex] - 2.0;
            NhNlData prevNhNlData;
            if (s_nhNlLoader->TryGetDataForDate(twoDaysBack, prevNhNlData)) {
                nh_nl_prev_weekly = prevNhNlData.nh_nl_weekly;
            }
        }

        // Calculate price data for Elder's divergence detection
        float currentPrice = sc.Close[barIndex];
        float recentHigh = sc.Close[barIndex];
        float recentLow = sc.Close[barIndex];

        // Find recent 20-bar high and low for divergence context
        int lookbackBars = std::min(20, barIndex);
        for (int i = 0; i < lookbackBars; i++) {
            int idx = barIndex - i;
            if (sc.High[idx] > recentHigh) recentHigh = sc.High[idx];
            if (sc.Low[idx] < recentLow) recentLow = sc.Low[idx];
        }

        // Determine price trend using 240-min Impulse EMA
        // Elder: "Use the EMA to determine the trend direction"
        bool priceIsRising = (currentPrice > Subgraph_ImpulseEMA[barIndex]);

        auto nhNlSignalIndicator = indMgr.GetIndicator<NhNlSignalIndicator>(IndicatorKey::NH_NL_SIGNAL);
        if (nhNlSignalIndicator && hasNhNlData) {
            // Calculate NH-NL signal with Elder's full methodology only when source data is available.
            NhNlSignalEnum nhNlSignal = CalculateNhNlSignal(nh_nl_daily, nh_nl_weekly, nh_nl_prev_weekly,
                                                            currentPrice, recentHigh, recentLow, priceIsRising);

            // Update NH-NL signal indicator (automatic export via IndicatorManager)
            nhNlSignalIndicator->Update(nhNlSignal);
            nhNlSignalIndicator->SetDailyValue(static_cast<float>(nh_nl_daily)); // Raw daily index (Regime Awareness)
        }

        // ========================================================================
        // CANONICAL OBSERVATIONDATA VECTOR - SCREEN 1 (MACRO / TIDE)
        // Calculated on 240-min bars (Tide), persists for intermediate/ripple bars
        // ========================================================================

        // Keep TS1 macro features on adaptive windows anchored to the shared
        // observation-window engine used by TS2/TS3.
        const int observation_window_n = CalculateAdaptiveObservationWindow(sc, 0.5f);
        const int macro_window_n = std::clamp(observation_window_n * 5, 50, 200);

        // Fisher gets its own independent adaptive window (coherence 0.3 = more
        // responsive to regime shifts) to avoid mechanical correlation with Hurst.
        const int fisher_obs_window_n = CalculateFisherAdaptiveWindow(sc, 0.3f);
        const int fisher_window_n = std::clamp(fisher_obs_window_n * 3, 30, 120);

        // 1. Log-Variance Ratio (Vol of Log-Returns) - adaptive macro window
        float logVariance = CalculateLogVariance(sc, macro_window_n);

        // 2. Hurst Exponent (Persistence) - adaptive macro window, min scale 8.
        // Graceful degradation: when history is insufficient after replay/chart reload,
        // carry forward last valid Hurst value instead of emitting NaN.
        const float hurstRaw = CalculateHurstExponent(sc, macro_window_n, 8);
        static float s_lastValidHurst = 0.5f;
        static bool s_hasLastValidHurst = false;
        const bool hasSufficientHurstHistory = (sc.Index + 1) >= macro_window_n;

        float hurst = hurstRaw;
        if (std::isfinite(hurstRaw)) {
            s_lastValidHurst = hurstRaw;
            s_hasLastValidHurst = true;
        } else if (!hasSufficientHurstHistory) {
            hurst = s_hasLastValidHurst ? s_lastValidHurst : 0.5f;
        }

        // 3. Hill Estimator — sourced from TailRiskEngine (event-driven).
        //    TRE is the unconditional authority; no bar-based CalculateHillEstimator here.

        // 4. Fisher Information (Regime Reliability) - adaptive intermediate window
        float fisherInfo = CalculateFisherInformation(sc, fisher_window_n);

        // Update Central Observation Store (Thread-safe context)
        auto* obs = ContextManager::Instance().GetMutableObservation();
        if (obs) {
            static bool s_loggedMacroWriterReady = false;
            static uint64_t s_macroWriteCount = 0;
            static uint64_t s_dim0ChangeCount = 0;
            static uint64_t s_dim6ChangeCount = 0;
            static uint64_t s_dim8ChangeCount = 0;
            static uint64_t s_dim0StaleRun = 0;
            static uint64_t s_dim6StaleRun = 0;
            static uint64_t s_dim8StaleRun = 0;
            static float s_prevDim0 = 0.0f;
            static float s_prevDim6 = 0.0f;
            static float s_prevDim8 = 0.0f;
            static bool s_prevMacroValid = false;

            ++s_macroWriteCount;

            if (!s_loggedMacroWriterReady && ShouldLogTs1MacroImportant()) {
                s_loggedMacroWriterReady = true;
                Logger::getInstance().log(
                    "TS1 MacroObs Writer active for ObservationData dims 0/6/8"
                );
            }

            const bool dim0Finite = std::isfinite(logVariance);
            const bool dim6Finite = std::isfinite(hurst);
            const bool dim8Finite = std::isfinite(fisherInfo);
            const float dim9TailIndex = obs->tail_index();
            const bool dim9Finite = std::isfinite(dim9TailIndex);
            const bool allTs1DimsFinite = dim0Finite && dim6Finite && dim8Finite && dim9Finite;

            if (!allTs1DimsFinite &&
                ShouldLogTs1MacroImportant() &&
                (s_macroWriteCount <= 5 || (s_macroWriteCount % kTs1MacroNonFiniteWarnEveryWrites) == 0)) {
                Logger::getInstance().log(
                    "TS1 MacroObs non-finite guard at write=" + std::to_string(s_macroWriteCount) +
                    " finite(dim0=" + std::to_string(static_cast<int>(dim0Finite)) +
                    ",dim6=" + std::to_string(static_cast<int>(dim6Finite)) +
                    ",dim8=" + std::to_string(static_cast<int>(dim8Finite)) +
                    ",dim9=" + std::to_string(static_cast<int>(dim9Finite)) + ")"
                );
            }

            if (s_prevMacroValid) {
                if (IsMacroValueChanged(s_prevDim0, logVariance)) {
                    ++s_dim0ChangeCount;
                    s_dim0StaleRun = 0;
                } else {
                    ++s_dim0StaleRun;
                }

                if (IsMacroValueChanged(s_prevDim6, hurst)) {
                    ++s_dim6ChangeCount;
                    s_dim6StaleRun = 0;
                } else {
                    ++s_dim6StaleRun;
                }

                if (IsMacroValueChanged(s_prevDim8, fisherInfo)) {
                    ++s_dim8ChangeCount;
                    s_dim8StaleRun = 0;
                } else {
                    ++s_dim8StaleRun;
                }
            }

            s_prevDim0 = logVariance;
            s_prevDim6 = hurst;
            s_prevDim8 = fisherInfo;
            s_prevMacroValid = true;

            if (ShouldLogTs1MacroImportant()) {
                const auto maybeLogStale = [&](uint64_t staleRun, const char* dimName) {
                    if (staleRun >= kTs1MacroStaleWarnEveryWrites &&
                        ((staleRun % kTs1MacroStaleWarnEveryWrites) == 0)) {
                        Logger::getInstance().log(
                            "TS1 MacroObs stale dim=" + std::string(dimName) +
                            " stale_run=" + std::to_string(staleRun) +
                            " writes=" + std::to_string(s_macroWriteCount)
                        );
                    }
                };
                maybeLogStale(s_dim0StaleRun, "dim0_log_variance_ratio");
                maybeLogStale(s_dim6StaleRun, "dim6_hurst_exponent");
                maybeLogStale(s_dim8StaleRun, "dim8_fisher_info");

                if ((s_macroWriteCount % kTs1MacroDigestEveryWrites) == 0) {
                    Logger::getInstance().log(
                        "TS1 MacroObs digest writes=" + std::to_string(s_macroWriteCount) +
                        " values(dim0=" + std::to_string(logVariance) +
                        ",dim6=" + std::to_string(hurst) +
                        ",dim8=" + std::to_string(fisherInfo) + ")" +
                        " changes(dim0=" + std::to_string(s_dim0ChangeCount) +
                        ",dim6=" + std::to_string(s_dim6ChangeCount) +
                        ",dim8=" + std::to_string(s_dim8ChangeCount) + ")" +
                        " stale(dim0=" + std::to_string(s_dim0StaleRun) +
                        ",dim6=" + std::to_string(s_dim6StaleRun) +
                        ",dim8=" + std::to_string(s_dim8StaleRun) + ")"
                    );
                }
            }

            if (ShouldLogTs1MacroVerbose() && s_macroWriteCount <= 10) {
                Logger::getInstance().log(
                    "TS1 MacroObs write=" + std::to_string(s_macroWriteCount) +
                    " dim0=" + std::to_string(logVariance) +
                    " dim6=" + std::to_string(hurst) +
                    " dim8=" + std::to_string(fisherInfo)
                );
            }

            static uint64_t s_macroCommitCount = 0;
            static uint64_t s_macroSkipCount = 0;

            if (allTs1DimsFinite) {
                obs->mutate_log_variance_ratio(logVariance);
                obs->mutate_hurst_exponent(hurst);
                // tail_index: sourced from TailRiskEngine in BuildObservationVector()
                obs->mutate_fisher_info(fisherInfo);
                ContextManager::Instance().MarkTs1MacroDimsFresh(
                    sc.GetCurrentDateTime().ToUNIXTimeInMicroseconds());
                ++s_macroCommitCount;
            } else {
                ++s_macroSkipCount;
            }

            if (ShouldLogTs1MacroImportant() &&
                (s_macroWriteCount <= 5 || (s_macroWriteCount % kTs1MacroDigestEveryWrites) == 0)) {
                Logger::getInstance().log(
                    "TS1 MacroObs commit digest writes=" + std::to_string(s_macroWriteCount) +
                    " committed=" + std::to_string(s_macroCommitCount) +
                    " skipped=" + std::to_string(s_macroSkipCount)
                );
            }
        }
    }
}

/*==========================================================================*/
/*
 * scsf_Screen1_MACD
 *
 * Standard MACD indicator (12, 26, 9) with configurable parameters.
 * Used as reference study for Impulse System delta calculations.
 *
 * Outputs:
 * - Subgraph 0: MACD line (fast EMA - slow EMA)
 * - Subgraph 1: Signal line (EMA of MACD)
 * - Subgraph 2: Histogram (MACD - Signal)
 *
 * Official Sierra Chart implementation pattern from Studies7.cpp
 */
SCSFExport scsf_Screen1_MACD(SCStudyInterfaceRef sc)
{
    SCSubgraphRef Subgraph_MACD = sc.Subgraph[0];
    SCSubgraphRef Subgraph_MovAvgOfMACD = sc.Subgraph[1];
    SCSubgraphRef Subgraph_MACDDiff = sc.Subgraph[2];
    SCSubgraphRef Subgraph_RefLine = sc.Subgraph[3];

    SCInputRef Input_InputData = sc.Input[0];
    SCInputRef Input_FastLength = sc.Input[3];
    SCInputRef Input_SlowLength = sc.Input[4];
    SCInputRef Input_MACDLength = sc.Input[5];
    SCInputRef Input_MovingAverageType = sc.Input[6];

    if (sc.SetDefaults)
    {
        sc.GraphName = "Screen 1 - MACD";
        sc.AutoLoop = 1;

        sc.GraphRegion = 2;
        sc.ValueFormat = 3;

        Subgraph_MACD.Name = "MACD";
        Subgraph_MACD.DrawStyle = DRAWSTYLE_LINE;
        Subgraph_MACD.DrawZeros = true;
        Subgraph_MACD.PrimaryColor = RGB(0, 255, 0);

        Subgraph_MovAvgOfMACD.Name = "MA of MACD";
        Subgraph_MovAvgOfMACD.DrawStyle = DRAWSTYLE_LINE;
        Subgraph_MovAvgOfMACD.DrawZeros = true;
        Subgraph_MovAvgOfMACD.PrimaryColor = RGB(255, 0, 255);

        Subgraph_MACDDiff.Name = "MACD Diff";
        Subgraph_MACDDiff.DrawStyle = DRAWSTYLE_BAR;
        Subgraph_MACDDiff.DrawZeros = true;
        Subgraph_MACDDiff.PrimaryColor = RGB(255, 255, 0);

        Subgraph_RefLine.Name = "Line";
        Subgraph_RefLine.DrawStyle = DRAWSTYLE_LINE;
        Subgraph_RefLine.DrawZeros = true;
        Subgraph_RefLine.PrimaryColor = RGB(255, 127, 0);

        Input_InputData.Name = "Input Data";
        Input_InputData.SetInputDataIndex(SC_LAST);

        Input_FastLength.Name = "Fast Moving Average Length";
        Input_FastLength.SetInt(12);
        Input_FastLength.SetIntLimits(1, MAX_STUDY_LENGTH);

        Input_SlowLength.Name = "Slow Moving Average Length";
        Input_SlowLength.SetInt(26);
        Input_SlowLength.SetIntLimits(1, MAX_STUDY_LENGTH);

        Input_MACDLength.Name = "MACD Moving Average Length";
        Input_MACDLength.SetInt(9);
        Input_MACDLength.SetIntLimits(1, MAX_STUDY_LENGTH);

        Input_MovingAverageType.Name = "Moving Average Type";
        Input_MovingAverageType.SetMovAvgType(MOVAVGTYPE_EXPONENTIAL);

        return;
    }

    // Validate MACD parameters (official Sierra Chart pattern)
    if (Input_FastLength.GetInt() >= Input_SlowLength.GetInt()) {
        return;  // Invalid: fast must be < slow
    }

    if (sc.LastCallToFunction) {
        auto* pDivergenceState = static_cast<MACDDivergenceState*>(
            sc.GetPersistentPointer(PersistentVar_TripleScreen1::MACD_DIVERGENCE_STATE));
        if (pDivergenceState) {
            delete pDivergenceState;
            sc.SetPersistentPointer(PersistentVar_TripleScreen1::MACD_DIVERGENCE_STATE, nullptr);
        }
        return;
    }

    // DataStartIndex calculation depends on MA type (official MACD pattern)
    // Exponential: 2 bars minimum, Simple: sum of all lengths
    // From Studies7.cpp MACD implementations
    if (Input_MovingAverageType.GetMovAvgType() == MOVAVGTYPE_EXPONENTIAL)
        sc.DataStartIndex = 2;
    else
        sc.DataStartIndex = Input_MACDLength.GetInt() + std::max(Input_FastLength.GetInt(), Input_SlowLength.GetInt());

    sc.MACD(sc.BaseDataIn[Input_InputData.GetInputDataIndex()], Subgraph_MACD, sc.Index, Input_FastLength.GetInt(), Input_SlowLength.GetInt(), Input_MACDLength.GetInt(), Input_MovingAverageType.GetInt());

    Subgraph_MovAvgOfMACD[sc.Index] = Subgraph_MACD.Arrays[2][sc.Index];
    Subgraph_MACDDiff[sc.Index] = Subgraph_MACD.Arrays[3][sc.Index];
    Subgraph_RefLine[sc.Index] = 0;
    SCSubgraphRef Subgraph_ATR = sc.Subgraph[4];
    sc.ATR(sc.BaseDataIn, Subgraph_ATR, 14, MOVAVGTYPE_WILDERS);

    auto& indMacd = IndicatorManager::Instance();

    auto longMacd = indMacd.GetIndicator<Macd>(IndicatorKey::LONG_MACD);
    if (longMacd) {
        // indicator-manager-dod-soa plan, Task 6 proof of pattern: SetFromChart
        // still owns the legacy object's other state (MacdValue/ZScore/Value,
        // ShouldTrigger dirty-mask) for its other readers (TripleScreen3.cpp,
        // getScreen1EntryText); the packed array is additionally written
        // directly here via SetValue<Key>() to exercise the new accessor.
        const MacdResult result = longMacd->SetFromChart(Subgraph_MACDDiff, sc.Index);
        indMacd.SetValue<IndicatorKey::LONG_MACD>(result.signal);
    }

    // Elder MACD Divergence Detection (240-min timeframe)
    // Get persistent state for divergence tracking
    MACDDivergenceState* pDivergenceState = (MACDDivergenceState*)sc.GetPersistentPointer(PersistentVar_TripleScreen1::MACD_DIVERGENCE_STATE);
    if (!pDivergenceState) {
        pDivergenceState = new MACDDivergenceState();
        sc.SetPersistentPointer(PersistentVar_TripleScreen1::MACD_DIVERGENCE_STATE, pDivergenceState);
    }

    // Reset divergence state on full recalculation (official Sierra Chart pattern)
    if (sc.UpdateStartIndex == 0) {
        pDivergenceState->priceBottom1 = 0.0f;
        pDivergenceState->macdBottom1 = 0.0;
        pDivergenceState->macdBottom1Index = -1;
        pDivergenceState->macdCrossedZeroUp = false;
        pDivergenceState->zeroCrossUpIndex = -1;
        pDivergenceState->pricePeak1 = 0.0f;
        pDivergenceState->macdPeak1 = 0.0;
        pDivergenceState->macdPeak1Index = -1;
        pDivergenceState->macdCrossedZeroDown = false;
        pDivergenceState->zeroCrossDownIndex = -1;
        pDivergenceState->rallySize = 0.0f;
        pDivergenceState->rallyBars = 0;
        pDivergenceState->divergenceQuality = 0.0f;
        pDivergenceState->falseBreakoutConfirmation = false;
        pDivergenceState->macdLinesDivergence = false;
        pDivergenceState->currentState = MACDDivergenceEnum::NONE;
    }

    // Detect Elder divergences using MACD-Histogram
    MACDDivergenceEnum divergence = DetectElderMACDDivergence(
        sc, sc.Index, sc.High, sc.Low, Subgraph_MACDDiff, *pDivergenceState, Subgraph_ATR[sc.Index], 3);

    // Update IndicatorManager with divergence state
    auto macdDivergence = indMacd.GetIndicator<MACDDivergence>(
        IndicatorKey::LONG_MACD_DIVERGENCE);
    if (macdDivergence) {
        macdDivergence->Update(divergence);
    }
}

/*==========================================================================*/
/*
 * scsf_Screen1_ForceIndexAverage
 *
 * Elder Triple Screen - Force Index (FI-13)
 *
 * Calculates Force Index: Volume × (Close - Close[1])
 * Smoothed with 13-period EMA to filter out minor fluctuations.
 * Used to confirm signals from Impulse System.
 *
 * Outputs:
 * - Subgraph 0: 13-period EMA of Force Index
 * - Subgraph 1: Standard deviation of Force Index (for volatility)
 *
 * References:
 * - Elder, Alexander. "Come Into My Trading Room" (2002), Chapter 7
 * - Official implementation matches Studies6.cpp:2015-2100
 */
SCSFExport scsf_Screen1_ForceIndexAverage(SCStudyInterfaceRef sc)
{
    // Section 1 - Set the configuration variables
    SCSubgraphRef Subgraph_ForceAverage = sc.Subgraph[0];
    SCSubgraphRef Subgraph_ForceStdDev = sc.Subgraph[1];
    SCFloatArrayRef Array_ForceIndex = sc.Subgraph[0].Arrays[0];

    SCInputRef Input_ImpulseStudy = sc.Input[0];

    if (sc.SetDefaults)
    {
        // Set the configuration and defaults
        sc.GraphName = "Screen 1 - 13-Period FI";
        sc.StudyDescription = "";
        sc.AutoLoop = 1;

        sc.ValueFormat = 2;
        sc.GraphRegion = 1;

        Subgraph_ForceAverage.Name = "Force Average";
        Subgraph_ForceAverage.DrawStyle = DRAWSTYLE_LINE;
        Subgraph_ForceAverage.PrimaryColor = RGB(0, 255, 0);
        Subgraph_ForceAverage.DrawZeros = true;

        Subgraph_ForceStdDev.Name = "Force StdDev";
        Subgraph_ForceStdDev.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_ForceStdDev.DrawZeros = false;

        // New input to reference the Impulse study.
        Input_ImpulseStudy.Name = "Impulse Study to Reference";
        Input_ImpulseStudy.SetStudyID(0);

        return;
    }

    // DataStartIndex: 13 bars minimum for Force Index 13-period EMA
    // Official pattern from Studies6.cpp:2048
    sc.DataStartIndex = 13;

    // Calculate Force Index with 13-period EMA (Elder's FI-13)
    // Formula: Volume × (Close - Close[i-1]), then 13-period EMA
    // Official implementation matches Studies6.cpp:2054-2056
    if (sc.Index >= 1) {
        CalculateForceIndex(sc, Array_ForceIndex, Subgraph_ForceAverage, 13);
    }
    sc.StdDeviation(Array_ForceIndex, Subgraph_ForceStdDev, 13);

    // Get the EMA subgraph data from the referenced Impulse study.
    SCFloatArray ImpulseEMASubgraphArray;
    if (!sc.GetStudyArrayUsingID(Input_ImpulseStudy.GetStudyID(), 0, ImpulseEMASubgraphArray))
    {
        Logger::getInstance().log("Failed to retrieve Impulse EMA from FI-13 study"); // Changed from sc.AddMessageToLog
        return;
    }

    // Validate study array size matches current chart
    if (ImpulseEMASubgraphArray.GetArraySize() < sc.ArraySize)
    {
        return;
    }

    // Bounds check before accessing sc.Index - 1
    if (sc.Index < 1)
        return;

    float ema_delta = ImpulseEMASubgraphArray[sc.Index] - ImpulseEMASubgraphArray[sc.Index - 1];
    float low_delta = sc.Low[sc.Index] - sc.Low[sc.Index - 1];
    float high_delta = sc.High[sc.Index] - sc.High[sc.Index - 1];

    auto longFI13Signal = IndicatorManager::Instance().GetIndicator<FI13Signal>(IndicatorKey::LONG_FI13_SIGNAL);
    if (longFI13Signal) {
        longFI13Signal->Update(GetFI_13(Subgraph_ForceAverage[sc.Index], Subgraph_ForceAverage[sc.Index - 1], ema_delta, low_delta, high_delta, Subgraph_ForceStdDev[sc.Index]));

        // v5.6: Feed raw FI-13 EMA value for robust z-score (Taleb-consistent magnitude)
        longFI13Signal->setFromChart(static_cast<double>(Subgraph_ForceAverage[sc.Index]));
    }
}


////// Helper functions

EmaEnum GetEmaEnum(float ema, float prevEma)
{
    EmaEnum emaEnum = EmaEnum::FLAT;

    float delta = ema - prevEma;

    if (delta > 0.0)
        emaEnum = EmaEnum::INC;
    else if (delta < 0.0)
        emaEnum = EmaEnum::DEC;

    return emaEnum;
}

FI13Enum GetFI_13(float force, float prevForce, float ema_delta, float low_delta, float high_delta, float forceStdDev)
{
    const float pumpDumpThreshold = 2.0f; // Threshold multiplier
    FI13Enum newValue{ FI13Enum::UNCLEAR };

    // Check for Pump/Dump first, as it's an extreme condition
    if (force > (pumpDumpThreshold * forceStdDev)) {
        return FI13Enum::BULLISH_PUMP;
    }
    if (force < -(pumpDumpThreshold * forceStdDev)) {
        return FI13Enum::BEARISH_DUMP;
    }

    float force_delta = force - prevForce;

    if (force > 0.0) {
        if (ema_delta > 0.0 && force_delta >= 0.0)
            newValue = FI13Enum::BULLISH_TREND_CONFIRMED;
        else if (high_delta > 0.0 && force_delta < 0.0)
            newValue = FI13Enum::BEARISH_DIVERGENCE;
        else if (prevForce <= 0)
            newValue = FI13Enum::BULLISH_CROSSOVER;
    }
    else if (force < 0.0) {
        if (ema_delta < 0.0 && force_delta <= 0.0)
            newValue = FI13Enum::BEARISH_TREND_CONFIRMED;
        else if (low_delta < 0.0 && force_delta > 0.0)
            newValue = FI13Enum::BULLISH_DIVERGENCE;
        else if (prevForce >= 0)
            newValue = FI13Enum::BEARISH_CROSSOVER;
    }

    return newValue;
}
