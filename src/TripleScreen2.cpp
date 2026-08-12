#include "MindfulTrader_Precompiled.h"
#include "CarryForwardCalculators.h"

/*==========================================================================*/
// Helper Functions (Pure Logic)
//
// By separating pure logic from the Sierra Chart API calls,
// these functions become easier to test and understand.

/*--------------------------------------------------------------------------*/
/*
 * GetPriceAction
 *
 * Analyzes price position relative to EMAs and Keltner Channels
 * to determine the intermediate timeframe market state.
 *
 * Identifies Linda Raschke price action patterns:
 * - FUBO/FDBO: Failed breakouts (reversal signals)
 * - FUBK/FDBK: Failed breakdowns (continuation signals)
 * - Above/below value zone tracking
 */
PriceActionEnum GetPriceAction(SCStudyInterfaceRef sc, float last, float ema, float fastEma, float upperChan, float lowerChan) {
    // Determine the value zone, defined by the area between the two EMAs.
    const float lowerEma = std::min(ema, fastEma);
    const float higherEma = std::max(ema, fastEma);

    if (last > higherEma) {
        // Price is above the value zone, indicating bullish sentiment.

        // Check for a "Fake-out Pullback" (FUBK). This pattern identifies a 3-bar swing high,
        // where the middle bar has the highest high. This can signal a potential pullback
        // or reversal, offering a buying opportunity in an uptrend.
        // Bounds check required for 3-bar pattern
        if (sc.Index >= 2 && (sc.High[sc.Index] < sc.High[sc.Index - 1]) && (sc.High[sc.Index - 2] < sc.High[sc.Index - 1])) {
            return PriceActionEnum::FUBK;
        }
        if (last >= upperChan) {
            return PriceActionEnum::HIT_UPPER_CHANNEL;
        }
        return PriceActionEnum::ABOVE_VALUE;
    }
    else if (last < lowerEma) {
        // Price is below the value zone, indicating bearish sentiment.

        // Check for a "Fake-out Dropback" (FDBK). This pattern identifies a 3-bar swing low,
        // where the middle bar has the lowest low. This can signal a potential rally
        // or reversal, offering a shorting opportunity in a downtrend.
        // Bounds check required for 3-bar pattern
        if (sc.Index >= 2 && (sc.Low[sc.Index] > sc.Low[sc.Index - 1]) && (sc.Low[sc.Index - 2] > sc.Low[sc.Index - 1])) {
            return PriceActionEnum::FDBK;
        }
        if (last <= lowerChan) {
            return PriceActionEnum::HIT_LOWER_CHANNEL;
        }
        return PriceActionEnum::BELOW_VALUE;
    }

    // Price is within the value zone (between the EMAs).
    return PriceActionEnum::IN_VALUE_ZONE;
}

/*==========================================================================*/
/*
 * scsf_Screen2_Impulse
 *
 * Elder Triple Screen - Screen 2: Daily Impulse System
 *
 * Intermediate timeframe (daily) trend analysis using:
 * - 13-period EMA (trend direction)
 * - 21-period EMA (value zone definition)
 * - Keltner Channels (volatility envelope)
 * - 14-period ADX (trend strength) — RETIRED March 2026, Hurst exponent used instead
 *
 * Price action patterns:
 * - FUBO/FDBO: Failed breakouts (reversals)
 * - FUBK/FDBK: Failed breakdowns (continuations)
 * - Channel hits and value zone position
 *
 * References external MACD and Keltner studies via study IDs.
 */
// --- Sierra Chart Study Functions ---

SCSFExport scsf_Screen2_Impulse(SCStudyInterfaceRef sc)
{
    // Using enums for subgraphs improves readability over magic numbers.
    enum { SG_IMPULSE_EMA, SG_COLOR_BAR, SG_ATR,
            // Reserved hidden slots (unused in canonical schema)
           SG_REL_RANGE, SG_BURSTINESS, SG_LEMPEL_ZIV, SG_LOG_EXPANSION };

    SCSubgraphRef Subgraph_ImpulseEMA = sc.Subgraph[SG_IMPULSE_EMA];
    SCSubgraphRef Subgraph_ColorBar = sc.Subgraph[SG_COLOR_BAR];
    SCSubgraphRef Subgraph_ATR = sc.Subgraph[SG_ATR];
    SCFloatArrayRef Array_ImpulseTrueRange = Subgraph_ATR.Arrays[0];
    SCFloatArrayRef Array_ImpulseATR = Subgraph_ATR.Arrays[1];

    // Reserved hidden subgraphs for deterministic Sierra slot indexing.
    // Runtime metric values are no longer written to these slots.
    SCSubgraphRef Subgraph_RelRange = sc.Subgraph[SG_REL_RANGE];
    SCSubgraphRef Subgraph_Burstiness = sc.Subgraph[SG_BURSTINESS];
    SCSubgraphRef Subgraph_LempelZiv = sc.Subgraph[SG_LEMPEL_ZIV];
    SCSubgraphRef Subgraph_LogExpansion = sc.Subgraph[SG_LOG_EXPANSION];

    enum { IN_INPUT_DATA, IN_MACD_STUDY, IN_KELTNER_STUDY };
    SCInputRef Input_MACDStudy = sc.Input[IN_MACD_STUDY];
    SCInputRef Input_KeltnerStudy = sc.Input[IN_KELTNER_STUDY];

    if (sc.SetDefaults) {
        sc.GraphName = "Screen 2 - Impulse System";
        sc.ValueFormat = VALUEFORMAT_INHERITED;
        sc.FreeDLL = 0;
        sc.GraphRegion = 0;
        sc.AutoLoop = 1;
        sc.GraphDrawType = GDT_CUSTOM;
        sc.DrawZeros = 0;

        Subgraph_ImpulseEMA.Name = "Impulse EMA";
        Subgraph_ImpulseEMA.DrawStyle = DRAWSTYLE_LINE;

        Subgraph_ColorBar.Name = "Color Bar";
        Subgraph_ColorBar.DrawStyle = DRAWSTYLE_COLORBAR;
        Subgraph_ColorBar.DrawZeros = false;

        Subgraph_ATR.Name = "ATR(14)";
        Subgraph_ATR.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_ATR.DrawZeros = false;

        // Configure new metric subgraphs to be ignored
        Subgraph_RelRange.Name = "Internal: Relative Range";
        Subgraph_RelRange.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_RelRange.DrawZeros = false;

        Subgraph_Burstiness.Name = "Internal: Burstiness";
        Subgraph_Burstiness.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_Burstiness.DrawZeros = false;

        Subgraph_LempelZiv.Name = "Internal: Lempel-Ziv";
        Subgraph_LempelZiv.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_LempelZiv.DrawZeros = false;

        Subgraph_LogExpansion.Name = "Internal: Log Expansion";
        Subgraph_LogExpansion.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_LogExpansion.DrawZeros = false;

        sc.Input[IN_INPUT_DATA].Name = "Input Data";
        sc.Input[IN_INPUT_DATA].SetInputDataIndex(SC_LAST);

        Input_MACDStudy.Name = "MACD Study to Reference";
        Input_MACDStudy.SetStudyID(0);

        Input_KeltnerStudy.Name = "Keltner Channel Study to Reference";
        Input_KeltnerStudy.SetStudyID(0);

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

    // Get the MACD subgraph data from the referenced study
    SCFloatArray MACDSubgraphArray;
    SCFloatArray MACDDiffSubgraphArray;

    static uint64_t s_macdMainRefFailCount = 0;
    static uint64_t s_macdDiffRefFailCount = 0;
    static uint64_t s_keltnerRefFailCount = 0;
    static uint64_t s_arraySizeMismatchCount = 0;
    static uint64_t s_indexGateCount = 0;
    static uint64_t s_ts2StructuralCommitCount = 0;

    const auto shouldSampleSparse = [](uint64_t count) {
        return count == 1 || (count % 16384) == 0;
    };

    if (!sc.GetStudyArrayUsingID(Input_MACDStudy.GetStudyID(), 0, MACDSubgraphArray))
    {
        ++s_macdMainRefFailCount;
        if (shouldSampleSparse(s_macdMainRefFailCount)) {
            Logger::getInstance().log(
                "TS2 GateReject: MACD main ref unresolved"
                "(study_id=" + std::to_string(Input_MACDStudy.GetStudyID()) +
                ", rejects=" + std::to_string(s_macdMainRefFailCount) + ")"
            );
        }
        return;
    }

    if (!sc.GetStudyArrayUsingID(Input_MACDStudy.GetStudyID(), 2, MACDDiffSubgraphArray))
    {
        ++s_macdDiffRefFailCount;
        if (shouldSampleSparse(s_macdDiffRefFailCount)) {
            Logger::getInstance().log(
                "TS2 GateReject: MACD diff ref unresolved"
                "(study_id=" + std::to_string(Input_MACDStudy.GetStudyID()) +
                ", rejects=" + std::to_string(s_macdDiffRefFailCount) + ")"
            );
        }
        return;
    }

    // Get the Keltner EMA subgraph data from the referenced study
    SCFloatArray KeltnerEMASubgraphArray;
    if (!sc.GetStudyArrayUsingID(Input_KeltnerStudy.GetStudyID(), 0, KeltnerEMASubgraphArray))
    {
        ++s_keltnerRefFailCount;
        if (shouldSampleSparse(s_keltnerRefFailCount)) {
            Logger::getInstance().log(
                "TS2 GateReject: Keltner EMA ref unresolved"
                "(study_id=" + std::to_string(Input_KeltnerStudy.GetStudyID()) +
                ", rejects=" + std::to_string(s_keltnerRefFailCount) + ")"
            );
        }
        return;
    }

    // Validate study array sizes match current chart (Sierra Chart pattern)
    if (MACDSubgraphArray.GetArraySize() < sc.ArraySize ||
        MACDDiffSubgraphArray.GetArraySize() < sc.ArraySize ||
        KeltnerEMASubgraphArray.GetArraySize() < sc.ArraySize)
    {
        ++s_arraySizeMismatchCount;
        if (shouldSampleSparse(s_arraySizeMismatchCount)) {
            Logger::getInstance().log(
                "TS2 GateReject: referenced array size mismatch "
                "(macd=" + std::to_string(MACDSubgraphArray.GetArraySize()) +
                ", macd_diff=" + std::to_string(MACDDiffSubgraphArray.GetArraySize()) +
                ", keltner=" + std::to_string(KeltnerEMASubgraphArray.GetArraySize()) +
                ", chart=" + std::to_string(sc.ArraySize) +
                ", rejects=" + std::to_string(s_arraySizeMismatchCount) + ")"
            );
        }
        return;
    }

    // Set the Impulse EMA equal to the Keltner EMA
    Subgraph_ImpulseEMA[sc.Index] = KeltnerEMASubgraphArray[sc.Index];

    // Calculate ATR for Relative Range (Screen 2 volatility normalization)
    sc.ATR(sc.BaseDataIn, Array_ImpulseTrueRange, Array_ImpulseATR, 14, MOVAVGTYPE_SIMPLE);

    // Bounds check before accessing sc.Index - 1
    if (sc.Index < 1) {
        ++s_indexGateCount;
        if (shouldSampleSparse(s_indexGateCount)) {
            Logger::getInstance().log(
                "TS2 GateReject: index warmup gate "
                "(index=" + std::to_string(sc.Index) +
                ", rejects=" + std::to_string(s_indexGateCount) + ")"
            );
        }
        return;
    }

    Subgraph_ColorBar[sc.Index] = 1;
    const float maDiff = Subgraph_ImpulseEMA[sc.Index] - Subgraph_ImpulseEMA[sc.Index - 1];
    const float macdDiff = MACDDiffSubgraphArray[sc.Index] - MACDDiffSubgraphArray[sc.Index - 1];
    Subgraph_ColorBar.DataColor[sc.Index] = GetImpulse(maDiff, macdDiff);

    // ========================================================================
    // CANONICAL OBSERVATIONDATA VECTOR - SCREEN 2 (WAVE)
    // Calculated on 60-min bars (Wave), persists for ripple bars
    // ========================================================================

    // 0. Relative Range (Volatility Normalization) - Q1
    float atr = Array_ImpulseATR[sc.Index];
    float& lastValidRelRange = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::RELATIVE_RANGE_LAST_VALID_VALUE);
    float relRange = cfc::ComputeRelativeRange(sc.High[sc.Index], sc.Low[sc.Index], atr, lastValidRelRange);
    lastValidRelRange = relRange;
    // Push directly to ContextManager.

    // Keep all Screen 2 short-horizon features on one adaptive lookback.
    const int observation_window_n = CalculateAdaptiveObservationWindow(sc, 0.5f);

    // Slow structural features need a minimum lookback for statistical validity.
    const int slow_window_n = std::max(30, observation_window_n);

    // 1. Burstiness Index (Clustering of Volatility)
    float burstiness = CalculateBurstiness(sc, observation_window_n);

    // 2. Lempel-Ziv Complexity — sourced from InformationEngine (event-driven).
    //    IE is the unconditional authority; no bar-based CalculateLempelZiv here.

    // 3. Fractal Dimension (Roughness)
    float fractalDim = CalculateFractalDimension(sc, slow_window_n);

    // 4. Recurrence Rate (Topological Stability, RQA)
    float recurrenceRate = CalculateRecurrenceRate(sc, slow_window_n);

    // 5. Realized Variance Ratio (Volatility Expansion/Contraction)
    float realizedVarRatio = CalculateRealizedVarianceRatio(sc, observation_window_n);

    const bool structuralFinite = std::isfinite(fractalDim) && std::isfinite(recurrenceRate);
    const bool structuralInRange =
        recurrenceRate >= 0.0f && recurrenceRate <= 1.0f &&
        fractalDim >= 1.0f && fractalDim <= 2.0f;

    // Update Central Observation Store
    // Physics metrics are injected directly into the ML context layer.
    auto* obs = ContextManager::Instance().GetMutableObservation();
    if (obs) {
        static uint64_t s_ts2StructuralRejectCount = 0;
        static bool s_loggedFirstStructuralCommit = false;

        obs->mutate_relative_range(relRange);
        obs->mutate_burstiness_index(burstiness);
        // lempel_ziv: sourced from InformationEngine in BuildObservationVector()

        if (structuralFinite && structuralInRange) {
            obs->mutate_fractal_dim(fractalDim);
            obs->mutate_recurrence_rate(recurrenceRate);
            ContextManager::Instance().MarkTs2StructuralDimsFresh(
                sc.GetCurrentDateTime().ToUNIXTimeInMicroseconds());
            ++s_ts2StructuralCommitCount;
            if (!s_loggedFirstStructuralCommit || (s_ts2StructuralCommitCount % 65536) == 0) {
                s_loggedFirstStructuralCommit = true;
                Logger::getInstance().log(
                    "TS2 StructuralObs commit"
                    "(commits=" + std::to_string(s_ts2StructuralCommitCount) + ")"
                );
            }
        } else {
            ++s_ts2StructuralRejectCount;
            if (shouldSampleSparse(s_ts2StructuralRejectCount)) {
                Logger::getInstance().log(
                    "TS2 StructuralObs reject: invalid structural dims "
                    "(fractal=" + std::to_string(fractalDim) +
                    ", recurrence=" + std::to_string(recurrenceRate) +
                    ", rejects=" + std::to_string(s_ts2StructuralRejectCount) + ")"
                );
            }
        }

        obs->mutate_correction_action(realizedVarRatio);
    }

    // Update indicators with new impulse value (v5.2: pass macdDiff + ATR for derived metrics)
    auto intermImpulse = IndicatorManager::Instance().GetIndicator<Impulse>(IndicatorKey::INTERM_IMP);
    if (intermImpulse) intermImpulse->SetFromColor(Subgraph_ColorBar.DataColor[sc.Index], Subgraph_ColorBar.DataColor[sc.Index - 1], maDiff, macdDiff, Array_ImpulseATR[sc.Index]);
}

/*==========================================================================*/
/*
 * scsf_Screen2_MACD
 *
 * Standard MACD indicator (12, 26, 9) for intermediate timeframe.
 * Used as reference study for Screen2 Impulse calculations.
 *
 * Outputs:
 * - Subgraph 0: MACD line (fast EMA - slow EMA)
 * - Subgraph 1: Signal line (EMA of MACD)
 * - Subgraph 2: Histogram (MACD - Signal)
 *
 * Official Sierra Chart implementation pattern from Studies7.cpp
 */
SCSFExport scsf_Screen2_MACD(SCStudyInterfaceRef sc)
{
    const int RSI_LENGTH = 2; // The key short-term length
    const unsigned int MA_TYPE = MOVAVGTYPE_WILDERS; // The correct smoothing method for RSI

    SCSubgraphRef Subgraph_MACD = sc.Subgraph[0];
    SCSubgraphRef Subgraph_MovAvgOfMACD = sc.Subgraph[1];
    SCSubgraphRef Subgraph_MACDDiff = sc.Subgraph[2];
    SCSubgraphRef Subgraph_RefLine = sc.Subgraph[3];

    SCSubgraphRef Subgraph_RSI = sc.Subgraph[4];
    SCSubgraphRef Subgraph_ATR = sc.Subgraph[5];

    SCInputRef Input_InputData = sc.Input[0];
    SCInputRef Input_FastLength = sc.Input[3];
    SCInputRef Input_SlowLength = sc.Input[4];
    SCInputRef Input_MACDLength = sc.Input[5];
    SCInputRef Input_MovingAverageType = sc.Input[6];

    if (sc.SetDefaults)
    {
        sc.GraphName = "Screen 2 - MACD";
        sc.FreeDLL = 0;
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

        Subgraph_RSI.DrawStyle = DRAWSTYLE_HIDDEN;

        Subgraph_ATR.Name = "ATR Temp";
        Subgraph_ATR.DrawStyle = DRAWSTYLE_HIDDEN;

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
        CleanupAdaptiveCalculators(sc);

        auto* pDivergenceState = static_cast<MACDDivergenceState*>(
            sc.GetPersistentPointer(PersistentVar_TripleScreen2::MACD_DIVERGENCE_STATE));
        if (pDivergenceState) {
            delete pDivergenceState;
            sc.SetPersistentPointer(PersistentVar_TripleScreen2::MACD_DIVERGENCE_STATE, nullptr);
        }
        return;
    }

    // DataStartIndex: 2 bars minimum for exponential MACD
    // Official pattern from Studies7.cpp (MACD studies)
    sc.DataStartIndex = 2;

    sc.MACD(sc.BaseDataIn[Input_InputData.GetInputDataIndex()], Subgraph_MACD, sc.Index, Input_FastLength.GetInt(), Input_SlowLength.GetInt(), Input_MACDLength.GetInt(), Input_MovingAverageType.GetInt());

    Subgraph_MovAvgOfMACD[sc.Index] = Subgraph_MACD.Arrays[2][sc.Index];
    Subgraph_MACDDiff[sc.Index] = Subgraph_MACD.Arrays[3][sc.Index];
    Subgraph_RefLine[sc.Index] = 0;
    sc.ATR(sc.BaseDataIn, Subgraph_ATR, 14, MOVAVGTYPE_WILDERS);

    auto& indMgr2 = IndicatorManager::Instance();

    // Update INTERM_MACD in IndicatorManager for use by Screen3
    auto intermMacd = indMgr2.GetIndicator<Macd>(IndicatorKey::INTERM_MACD);
    if (intermMacd) {
        // indicator-manager-dod-soa plan, Task 6 proof of pattern: SetFromChart
        // still owns the legacy object's other state (MacdValue/ZScore/Value,
        // ShouldTrigger dirty-mask) for its other readers (TripleScreen3.cpp,
        // StudyHelperFunctions.cpp, EventSerializer.cpp, BackTesterStudy.cpp);
        // the packed array is additionally written directly here via
        // SetValue<Key>(). INTERM_MACD has TWO kIndicatorLayout rows (Int8
        // signal @13, Float32 interm_macd_norm companion @8 — confirmed via
        // include/IndicatorLayout.h, Task 2's audit), so the single-Key
        // convenience form's static_assert would reject it; the explicit
        // (Key, Block) form is required here.
        const MacdResult result = intermMacd->SetFromChart(Subgraph_MACDDiff, sc.Index);
        indMgr2.SetValue<IndicatorKey::INTERM_MACD, mts::StorageBlock::Int8>(result.signal);
    }

    // Elder MACD Divergence Detection (15-min timeframe)
    // Get persistent state for divergence tracking (independent from 240-min)
    MACDDivergenceState* pDivergenceState = (MACDDivergenceState*)sc.GetPersistentPointer(PersistentVar_TripleScreen2::MACD_DIVERGENCE_STATE);
    if (!pDivergenceState) {
        pDivergenceState = new MACDDivergenceState();
        sc.SetPersistentPointer(PersistentVar_TripleScreen2::MACD_DIVERGENCE_STATE, pDivergenceState);
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
    auto macdDivergence = indMgr2.GetIndicator<MACDDivergence>(
        IndicatorKey::INTERM_MACD_DIVERGENCE);
    if (macdDivergence) {
        macdDivergence->Update(divergence);
    }

    auto rsiIndicator = indMgr2.GetIndicator<RSIIndicator>(IndicatorKey::RSI);
    if (rsiIndicator) {
        sc.RSI(sc.BaseDataIn[Input_InputData.GetInputDataIndex()], Subgraph_RSI, MA_TYPE, RSI_LENGTH);
        rsiIndicator->Update(DetectRSI(Subgraph_RSI[sc.Index]));
    }
}

/*==========================================================================*/
/*
 * scsf_Screen2_KeltnerChannel
 *
 * Keltner Channels for intermediate timeframe volatility envelope.
 *
 * Configuration:
 * - 13-period EMA centerline (matches Screen2 Impulse EMA)
 * - 21-period EMA for value zone definition
 * - ATR-based bands (default 14-period, user configurable multipliers)
 * - ADX for trend strength — RETIRED March 2026, Hurst exponent used instead
 *
 * Used by Screen2 Impulse to identify:
 * - Channel breakouts/breakdowns
 * - Value zone position
 * - Volatility expansion/contraction
 *
 * Official Sierra Chart ATR pattern from Studies7.cpp:3840-3920
 */
SCSFExport scsf_Screen2_KeltnerChannel(SCStudyInterfaceRef sc)
{
    // Subgraphs 0-12: Existing Keltner/Context Logic
    SCSubgraphRef Subgraph_KeltnerAverage = sc.Subgraph[0];
    SCSubgraphRef Subgraph_TopBand = sc.Subgraph[1];
    SCSubgraphRef Subgraph_BottomBand = sc.Subgraph[2];
    SCSubgraphRef Subgraph_AtrTemp3 = sc.Subgraph[3];  // ATR for symmetric band calculation
    SCSubgraphRef Subgraph_InternalUnused = sc.Subgraph[4]; // Reserved internal slot (must stay non-visual)
    SCSubgraphRef Subgraph_EMA21 = sc.Subgraph[5];
    SCSubgraphRef Subgraph_EmaStdDev = sc.Subgraph[6];
    SCSubgraphRef Subgraph_ADX = sc.Subgraph[7];
    SCSubgraphRef Subgraph_EMA3 = sc.Subgraph[8];      // 3-period EMA for Raschke oscillator
    SCSubgraphRef Subgraph_EMA16 = sc.Subgraph[9];     // 16-period EMA for Raschke oscillator
    SCSubgraphRef Subgraph_Oscillator310 = sc.Subgraph[10]; // 3-16 Oscillator (EMA3 - EMA16)
    SCFloatArrayRef Array_AtrTrueRange = Subgraph_AtrTemp3.Arrays[0];
    SCFloatArrayRef Array_AtrKeltner = Subgraph_AtrTemp3.Arrays[1];
    // ADX arrays retired — Subgraph_ADX slot preserved for index stability
    SCFloatArrayRef Array_EMA3 = Subgraph_EMA3.Arrays[1];
    SCFloatArrayRef Array_EMA16 = Subgraph_EMA16.Arrays[1];
    SCFloatArrayRef Array_Oscillator310 = Subgraph_Oscillator310.Arrays[1];
    SCFloatArrayRef Array_EmaStdDev = Subgraph_EmaStdDev.Arrays[0];

    // Persistent internal arrays for StatisticalContext computation.
    // These are intentionally stored in Arrays[n] instead of Subgraph[x][Index]
    // to keep Region 0 autoscaling tied to price-level outputs only.
    SCFloatArrayRef Array_LogReturns = sc.Subgraph[11].Arrays[1];     // Internal: Log return buffer
    SCFloatArrayRef Array_Volatility20 = sc.Subgraph[11].Arrays[2];   // Internal: Volatility result
    SCFloatArrayRef Array_AbsChanges = sc.Subgraph[12].Arrays[1];     // Internal: Abs changes buffer
    SCFloatArrayRef Array_Efficiency20 = sc.Subgraph[12].Arrays[2];   // Internal: Efficiency result

    // Subgraph 14: Wave-scale Hurst for Indicator framework (Shannon, 60m persistence)
    SCSubgraphRef Subgraph_HurstExponent = sc.Subgraph[14];

    SCInputRef Input_Data = sc.Input[0];
    SCInputRef Input_TrueRangeAvgLength = sc.Input[4];
    SCInputRef Input_BandMultiplier = sc.Input[5];  // Single multiplier for symmetric bands (Elder's approach)
    SCInputRef Input_ATR_MAType = sc.Input[8];

    if (sc.SetDefaults)
    {
        sc.GraphName = "Screen 2 - Keltner Channel & Strategic Climate";

        sc.FreeDLL = 0;
        sc.GraphRegion = 0;
        sc.ValueFormat = 3;
        sc.AutoLoop = 1;
        sc.DrawZeros = 0;

        Subgraph_KeltnerAverage.Name = "Keltner Average";
        Subgraph_KeltnerAverage.DrawStyle = DRAWSTYLE_LINE;
        Subgraph_KeltnerAverage.PrimaryColor = RGB(0, 255, 0);
        Subgraph_KeltnerAverage.DrawZeros = true;

        Subgraph_TopBand.Name = "Top";
        Subgraph_TopBand.DrawStyle = DRAWSTYLE_LINE;
        Subgraph_TopBand.PrimaryColor = RGB(255, 0, 255);
        Subgraph_TopBand.DrawZeros = true;

        Subgraph_BottomBand.Name = "Bottom";
        Subgraph_BottomBand.DrawStyle = DRAWSTYLE_LINE;
        Subgraph_BottomBand.PrimaryColor = RGB(255, 255, 0);
        Subgraph_BottomBand.DrawZeros = true;

        Subgraph_AtrTemp3.Name = "Internal: ATR Temp";
        Subgraph_AtrTemp3.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_AtrTemp3.DrawZeros = false;

        Subgraph_InternalUnused.Name = "Internal: Reserved";
        Subgraph_InternalUnused.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_InternalUnused.DrawZeros = false;

        Subgraph_EMA21.Name = "21-Period EMA";
        Subgraph_EMA21.DrawStyle = DRAWSTYLE_LINE;
        Subgraph_EMA21.PrimaryColor = RGB(0, 0, 255);
        Subgraph_EMA21.DrawZeros = true;

        Subgraph_EmaStdDev.Name = "Internal: EMA StdDev";
        Subgraph_EmaStdDev.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_EmaStdDev.DrawZeros = false;

        Subgraph_ADX.Name = "ADX [RETIRED]";
        Subgraph_ADX.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_ADX.DrawZeros = false;

        Subgraph_EMA3.Name = "3-Period EMA";
        Subgraph_EMA3.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_EMA3.DrawZeros = false;

        Subgraph_EMA16.Name = "16-Period EMA";
        Subgraph_EMA16.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_EMA16.DrawZeros = false;

        Subgraph_Oscillator310.Name = "3-16 Oscillator";
        Subgraph_Oscillator310.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_Oscillator310.DrawZeros = false;

        // Note: Arrays don't have .Name/.DrawStyle - these belong to parent Subgraph[11]/[12]
        sc.Subgraph[11].Name = "Volatility (20-period StdDev)";
        sc.Subgraph[11].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[11].DrawZeros = false;

        sc.Subgraph[12].Name = "Efficiency (20-period)";
        sc.Subgraph[12].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[12].DrawZeros = false;

        // Wave-scale Hurst — Shannon persistence for Indicator framework
        Subgraph_HurstExponent.Name = "Hurst Exponent (Wave)";
        Subgraph_HurstExponent.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_HurstExponent.DrawZeros = false;

        Input_Data.Name = "Input Data";
        Input_Data.SetInputDataIndex(SC_OHLC_AVG);

        Input_TrueRangeAvgLength.Name = "True Range Avg Length";
        Input_TrueRangeAvgLength.SetInt(13);
        Input_TrueRangeAvgLength.SetIntLimits(1, MAX_STUDY_LENGTH);

        Input_BandMultiplier.Name = "Band Multiplier (ATR)";
        Input_BandMultiplier.SetFloat(2.0f);  // Elder's standard (symmetric: same for top & bottom)
        Input_BandMultiplier.SetFloatLimits(0.1f, 10.0f);

        Input_ATR_MAType.Name = "ATR Mov Avg Type";
        Input_ATR_MAType.SetMovAvgType(MOVAVGTYPE_WILDERS);

        return;
    }

    sc.DataStartIndex = std::max(14, Input_TrueRangeAvgLength.GetInt());

    // Calculate Keltner Channel manually (Elder's methodology with symmetric 2.0 ATR bands)
    // Fixed 13-period EMA center line
    sc.ExponentialMovAvg(sc.BaseDataIn[static_cast<int>(Input_Data.GetInputDataIndex())], Subgraph_KeltnerAverage, 13);

    // Calculate the 21-period EMA for value zone
    sc.ExponentialMovAvg(sc.BaseDataIn[static_cast<int>(Input_Data.GetInputDataIndex())], Subgraph_EMA21, 21);

    // ADX computation retired — Hurst exponent provides superior persistence measurement.
    // Subgraph_ADX slot and Array_Adx* arrays preserved to avoid index shifts.

    // Calculate ATR once for both bands (Elder's symmetric approach with 2.0 multiplier default)
    sc.ATR(sc.BaseDataIn, Array_AtrTrueRange, Array_AtrKeltner,
        Input_TrueRangeAvgLength.GetInt(), Input_ATR_MAType.GetMovAvgType());

    const float multiplier = Input_BandMultiplier.GetFloat();
    Subgraph_TopBand[sc.Index] = Subgraph_KeltnerAverage[sc.Index] + (Array_AtrKeltner[sc.Index] * multiplier);
    Subgraph_BottomBand[sc.Index] = Subgraph_KeltnerAverage[sc.Index] - (Array_AtrKeltner[sc.Index] * multiplier);

    // Calculate Linda Raschke's 3-16 Oscillator (used for velocity calculation in StatisticalContext)
    // Formula: 3-period EMA minus 16-period EMA
    // This oscillator captures short-term momentum relative to intermediate trend
    sc.ExponentialMovAvg(sc.BaseDataIn[static_cast<int>(Input_Data.GetInputDataIndex())], Array_EMA3, 3);
    sc.ExponentialMovAvg(sc.BaseDataIn[static_cast<int>(Input_Data.GetInputDataIndex())], Array_EMA16, 16);
    Array_Oscillator310[sc.Index] = Array_EMA3[sc.Index] - Array_EMA16[sc.Index];

    auto& indMgr = IndicatorManager::Instance();

    // Update Oscillator310 indicator with EMA fast/slow lines for training data export
    auto* osc310 = indMgr.GetIndicator<Oscillator310>(IndicatorKey::OSCILLATOR_310);
    if (osc310) {
        osc310->UpdateOscillator(Array_EMA3[sc.Index], Array_EMA16[sc.Index]);
    }

    // ========================================================================
    // WAVE-SCALE HURST (Shannon Persistence → Indicator Framework)
    // ========================================================================
    // 60m Hurst feeds HurstExponentIndicator for trade-decision regime classification.
    // Observation vector dim 6 (macro Hurst) is owned by TS1 at 240m scale.
    {
        float hurst = (sc.Index < 100) ? 0.5f : CalculateHurstExponent(sc);
        Subgraph_HurstExponent.Arrays[1][sc.Index] = hurst;

        if (sc.Index >= 100) {
            auto* hurstInd = indMgr.GetIndicator<HurstExponentIndicator>(IndicatorKey::HURST_EXPONENT);
            if (hurstInd) {
                hurstInd->SetFromFloat(hurst);
            }
        }
    }

    // Elite: Compute StatisticalContext using optimized rolling calculations (O(1) per bar)
    // Only compute after sufficient warmup period (20 bars minimum)
    if (sc.Index >= 20) {
        StatisticalContext ctx;

        // 1. Volatility: Use Sierra Chart's optimized StdDeviation for log returns
        //    Elite approach: Calculate log returns incrementally, then use built-in StdDev
        if (sc.Index >= 1 && sc.Close[sc.Index] > 0.0f && sc.Close[sc.Index - 1] > 0.0f) {
            Array_LogReturns[sc.Index] = std::log(sc.Close[sc.Index] / sc.Close[sc.Index - 1]);

            // Use Sierra Chart's optimized rolling standard deviation (internally cached)
            sc.StdDeviation(Array_LogReturns, Array_Volatility20, 20);
            ctx.volatility = Array_Volatility20[sc.Index];
        } else {
            ctx.volatility = 0.0f;
        }

        // 2. Efficiency: Use rolling sums stored in subgraph arrays (incremental calculation)
        //    Elite approach: Store absolute changes, compute efficiency from stored values
        if (sc.Index >= 1) {
            Array_AbsChanges[sc.Index] = std::abs(sc.Close[sc.Index] - sc.Close[sc.Index - 1]);
        } else {
            Array_AbsChanges[sc.Index] = 0.0f;
        }

        // Calculate net change over 20-period window
        float netChange = std::abs(sc.Close[sc.Index] - sc.Close[sc.Index - 20]);

        // Sum absolute changes over 20-period window (Sierra Chart handles this efficiently)
        float sumAbsChanges = 0.0f;
        for (int i = 0; i < 20; ++i) {
            sumAbsChanges += Array_AbsChanges[sc.Index - i];
        }

        if (sumAbsChanges > 0.0001f) {
            ctx.efficiency = std::min(1.0f, netChange / sumAbsChanges);
            Array_Efficiency20[sc.Index] = ctx.efficiency;
        } else {
            ctx.efficiency = 0.0f;
            Array_Efficiency20[sc.Index] = 0.0f;
        }

        // 3. Relative Range: Current bar range normalized by ATR (already O(1))
        if (Array_AtrKeltner[sc.Index] > 0.0f) {
            float barRange = sc.High[sc.Index] - sc.Low[sc.Index];
            ctx.relRange = barRange / Array_AtrKeltner[sc.Index];
        } else {
            ctx.relRange = 0.0f;
        }

        // 4. Velocity: Change in oscillator_310 (momentum acceleration)
        if (sc.Index >= 1) {
            ctx.velocity = Array_Oscillator310[sc.Index] - Array_Oscillator310[sc.Index - 1];
        } else {
            ctx.velocity = 0.0f;
        }

        // Volatility is already computed above through sc.StdDeviation on Array_LogReturns.
        // Keep a single source of truth and do not recompute here.

        // Tenure tracking is managed centrally by ContextManager::SetWaveContext.

        // Timestamp
        ctx.lastUpdated = sc.BaseDateTimeIn[sc.Index];

        // Update ContextManager (triggers HMM auto-update if needed)
        ContextManager::Instance().SetWaveContext(std::move(ctx));

    }

    auto intermPriceAction = indMgr.GetIndicator<IntermediateMarketAction>(IndicatorKey::INTERM_MKT_ACTION);
    if (intermPriceAction) {
        intermPriceAction->setFastEma(Subgraph_KeltnerAverage[sc.Index], Subgraph_TopBand[sc.Index], Subgraph_BottomBand[sc.Index]);

        intermPriceAction->setEma(Subgraph_EMA21[sc.Index]);

        PriceActionEnum action = GetPriceAction(sc, sc.Close[sc.Index], Subgraph_EMA21[sc.Index], Subgraph_KeltnerAverage[sc.Index],
                                                    Subgraph_TopBand[sc.Index], Subgraph_BottomBand[sc.Index]);
        intermPriceAction->Update(action);

        // ================================================================
        // STRUCTURAL SWING DETECTION (Option B — numeric 60-min pivots)
        // ================================================================
        // sc.IsSwingHigh/Low confirms a pivot SWING_LENGTH bars after it forms.
        // SWING_LENGTH=5 → 5 hours on each side = significant structural level.
        // Detection runs on the confirmed bar (sc.Index - SWING_LENGTH), not current bar.
        constexpr int SWING_LENGTH = 5;
        if (sc.Index >= SWING_LENGTH * 2) {
            const int pivotIndex = sc.Index - SWING_LENGTH;
            if (sc.IsSwingHigh(sc.High, pivotIndex, SWING_LENGTH)) {
                intermPriceAction->updateSwingHigh(sc.High[pivotIndex]);
            }
            if (sc.IsSwingLow(sc.Low, pivotIndex, SWING_LENGTH)) {
                intermPriceAction->updateSwingLow(sc.Low[pivotIndex]);
            }
        }

        // ================================================================
        // KELTNER BAND REJECTION DETECTION
        // ================================================================
        // Upper rejection: price touched upper band within last 3 bars,
        //   then current bar closes below the band midpoint (fastEma).
        // Lower rejection: mirror logic for lower band.
        // This distinguishes "defended resistance" from "breakout zone".
        constexpr int REJECTION_LOOKBACK = 3;
        bool touchedUpper = false;
        bool touchedLower = false;
        if (sc.Index >= REJECTION_LOOKBACK) {
            for (int k = 0; k < REJECTION_LOOKBACK; ++k) {
                const int bi = sc.Index - k;
                if (sc.High[bi] >= Subgraph_TopBand[bi]) touchedUpper = true;
                if (sc.Low[bi] <= Subgraph_BottomBand[bi]) touchedLower = true;
            }
        }
        const bool upperRejected = touchedUpper && (sc.Close[sc.Index] < Subgraph_KeltnerAverage[sc.Index]);
        const bool lowerRejected = touchedLower && (sc.Close[sc.Index] > Subgraph_KeltnerAverage[sc.Index]);
        intermPriceAction->setKeltnerRejection(upperRejected, lowerRejected);
    }

    auto priceMetrics = indMgr.GetIndicator<PriceMetricsIndicator>(IndicatorKey::PRICE_METRICS);
    if (priceMetrics) {
        priceMetrics->SetOHLC(sc.Open[sc.Index], sc.High[sc.Index], sc.Low[sc.Index], sc.Close[sc.Index]);
    }

    auto raschkeStrategyIndicator = indMgr.GetIndicator<RaschkeStrategyIndicator>(IndicatorKey::RASCHKE_STRATEGY_SETUP);
    if (raschkeStrategyIndicator) {
        raschkeStrategyIndicator->Update(DetectRaschkeStrategySetup(sc, Subgraph_HurstExponent.Arrays[1][sc.Index], Subgraph_EMA21[sc.Index]));
    }

    sc.StdDeviation(Subgraph_KeltnerAverage, Array_EmaStdDev, 13);
    auto emaProximity = indMgr.GetIndicator<EmaProximityIndicator>(IndicatorKey::EMA_PROXIMITY);
    if (emaProximity) {
        emaProximity->Update(DetectEmaProximity(sc, Subgraph_KeltnerAverage[sc.Index], Array_EmaStdDev[sc.Index]));
    }
}

/*==========================================================================*/
/*
 * scsf_Screen2_ForceIndexAverage
 *
 * Elder Force Index (FI-2) - intermediate timeframe confirmation indicator.
 *
 * Calculates Force Index: Volume × (Close - Close[1])
 * Smoothed with configurable EMA period (default 2) for sensitive detection.
 * Shorter period than FI-13 to catch intermediate trend changes earlier.
 *
 * Outputs:
 * - Subgraph 0: EMA-smoothed Force Index
 *
 * References:
 * - Elder, Alexander. "Come Into My Trading Room" (2002), Chapter 7
 * - Official implementation matches Studies6.cpp:2015-2100
 */
SCSFExport scsf_Screen2_ForceIndex(SCStudyInterfaceRef sc)
{
    // Section 1 - Set the configuration variables

    SCSubgraphRef Subgraph_ForceAverage = sc.Subgraph[0];

    SCFloatArrayRef Array_ForceIndex = sc.Subgraph[0].Arrays[0];

    SCInputRef Input_AverageLength = sc.Input[0];

    if (sc.SetDefaults)
    {
        // Set the configuration and defaults

        sc.GraphName = "Screen 2 - 2-Period FI";

        sc.StudyDescription = "";

        sc.FreeDLL = 0;
        sc.AutoLoop = 1;
        sc.ValueFormat = 2;
        sc.GraphRegion = 1;

        Subgraph_ForceAverage.Name = "Force Average";
        Subgraph_ForceAverage.DrawStyle = DRAWSTYLE_LINE;
        Subgraph_ForceAverage.PrimaryColor = RGB(0, 255, 0);
        Subgraph_ForceAverage.DrawZeros = true;

        Input_AverageLength.Name = "Moving Average Length";
        Input_AverageLength.SetInt(2);  // Elder's FI-2: 2-period EMA for sensitive detection
        Input_AverageLength.SetIntLimits(1, MAX_STUDY_LENGTH);

        return;
    }

    // DataStartIndex: minimum bars required for EMA smoothing
    // Official pattern from Studies6.cpp:2048
    sc.DataStartIndex = Input_AverageLength.GetInt();

    // Calculate Force Index using helper function
    // Formula from Elder: Force Index = Volume × (Close[i] - Close[i-1])
    // Then apply EMA smoothing (2-period for sensitive, 13-period for strategic)
    if (sc.Index >= 1) {
        CalculateForceIndex(sc, Array_ForceIndex, Subgraph_ForceAverage, Input_AverageLength.GetInt());
    }

    auto& indFI = IndicatorManager::Instance();

    // DOD/SoA migration (Task 14): read straight from the packed array — no
    // pointer, no null check, always a valid value (previously defaulted to
    // MacdEnum::AT_ZERO if the leaf object was null; packed reads can't be
    // null, so that fallback is gone).
    MacdEnum macd = static_cast<MacdEnum>(indFI.GetValue<IndicatorKey::LONG_MACD>());

    auto intermFI2Signal = indFI.GetIndicator<FI2Signal>(IndicatorKey::INTERM_FI2_SIGNAL);
    if (intermFI2Signal) {
        intermFI2Signal->setFromChart(Subgraph_ForceAverage[sc.Index], macd);
    }
}

/*==========================================================================*/
/*
 * scsf_Screen2_KeltnerChannel
 *
 * Elder Triple Screen - Keltner Channel for intermediate timeframe
 *
 * Volatility-based envelope around 13-period EMA:
 * - Center line: 13-period EMA (matches Elder's impulse system)
 * - Bands: ATR-based (default 14-period ATR × 2.0 multipliers)
 * - Asymmetric: Top and bottom multipliers are independently configurable
 *
 * Used for:
 * - Identifying overbought/oversold conditions
 * - Detecting failed breakouts (FUBO/FDBO patterns)
 * - Value zone boundaries with 21-period EMA
 *
 * References:
 * - Elder, Alexander. "Come Into My Trading Room" (2002)
 * - Official pattern from Studies7.cpp:3840-3920 (manual ATR calculation)
 */
SCSFExport scsf_Screen2_StochasticCrossover(SCStudyInterfaceRef sc)
{
    SCSubgraphRef Subgraph_Buy = sc.Subgraph[0];
    SCSubgraphRef Subgraph_Sell = sc.Subgraph[1];
    SCSubgraphRef Subgraph_FastD = sc.Subgraph[2];
    SCFloatArrayRef Subgraph_SlowK = Subgraph_FastD.Arrays[0];
    SCFloatArrayRef Subgraph_SlowD = Subgraph_FastD.Arrays[1];

    // Divergence detection subgraphs
    SCSubgraphRef Subgraph_BullishDivergence = sc.Subgraph[3];
    SCSubgraphRef Subgraph_BearishDivergence = sc.Subgraph[4];
    SCFloatArrayRef Array_PriceInTicks = sc.Subgraph[5].Arrays[0];
    SCFloatArrayRef Array_StochasticInPoints = sc.Subgraph[5].Arrays[1];

    SCInputRef Input_FastK = sc.Input[0];
    SCInputRef Input_FastD = sc.Input[1];
    SCInputRef Input_SlowD = sc.Input[2];
    SCInputRef Input_Line1 = sc.Input[3];
    SCInputRef Input_Line2 = sc.Input[4];
    SCInputRef Input_UseBuySell = sc.Input[5];
    SCInputRef Input_ArrowOffsetPercentage = sc.Input[6];
    SCInputRef Input_EnableDivergence = sc.Input[7];
    SCInputRef Input_DivergenceLength = sc.Input[8];
    SCInputRef Input_DivergenceThreshold = sc.Input[9];
    SCInputRef Input_OppositeSlopeDivergenceThreshold = sc.Input[10];

    if (sc.SetDefaults)
    {
        // Set the configuration and defaults

        sc.GraphName = "Screen 2 - Stochastic Crossover System";
        sc.StudyDescription = "Stochastic Crossover Study System.";
        sc.FreeDLL = 0;

        Subgraph_Buy.Name = "Buy";
        Subgraph_Buy.PrimaryColor = RGB_COLOR(0, 255, 0);	// green
        Subgraph_Buy.DrawStyle = DRAWSTYLE_ARROW_UP;
        Subgraph_Buy.LineWidth = 2;	//Width of arrow

        Subgraph_Sell.Name = "Sell";
        Subgraph_Sell.DrawStyle = DRAWSTYLE_ARROW_DOWN;
        Subgraph_Sell.PrimaryColor = RGB_COLOR(255, 0, 0);	// red
        Subgraph_Sell.LineWidth = 2; //Width of arrow

        Input_FastK.Name = "Fast %K Length";
        Input_FastK.SetInt(10);
        Input_FastK.SetIntLimits(1, MAX_STUDY_LENGTH);

        Input_FastD.Name = "Fast %D Length";
        Input_FastD.SetInt(3);
        Input_FastD.SetIntLimits(1, MAX_STUDY_LENGTH);

        Input_SlowD.Name = "Slow %D Length";
        Input_SlowD.SetInt(3);
        Input_SlowD.SetIntLimits(1, MAX_STUDY_LENGTH);

        Input_Line1.Name = "Line 1 Value";
        Input_Line1.SetFloat(70);
        Input_Line1.SetFloatLimits(0, 100);

        Input_Line2.Name = "Line 2 Value";
        Input_Line2.SetFloat(30);
        Input_Line2.SetFloatLimits(0, 100);

        Input_UseBuySell.Name = "Use Buy/Sell Lines";
        Input_UseBuySell.SetYesNo(true);

        Input_ArrowOffsetPercentage.Name = "Arrow Offset Percentage";
        Input_ArrowOffsetPercentage.SetInt(3);
        Input_ArrowOffsetPercentage.SetIntLimits(0, 100);

        Input_EnableDivergence.Name = "Enable Divergence Detection";
        Input_EnableDivergence.SetYesNo(0); // Disabled by default - enable for ML training if needed

        Input_DivergenceLength.Name = "Divergence Detection Length";
        Input_DivergenceLength.SetInt(10);
        Input_DivergenceLength.SetIntLimits(5, 50);

        Input_DivergenceThreshold.Name = "Divergence Angle Threshold (Degrees)";
        Input_DivergenceThreshold.SetFloat(45.0f);
        Input_DivergenceThreshold.SetFloatLimits(10, 90);

        Input_OppositeSlopeDivergenceThreshold.Name = "Opposite Slope Divergence Threshold (Degrees)";
        Input_OppositeSlopeDivergenceThreshold.SetFloat(10.0f);
        Input_OppositeSlopeDivergenceThreshold.SetFloatLimits(0, 90);

        Subgraph_BullishDivergence.Name = "Bullish Divergence";
        Subgraph_BullishDivergence.DrawStyle = DRAWSTYLE_ARROW_UP;
        Subgraph_BullishDivergence.PrimaryColor = RGB(0, 200, 255); // Cyan (different from crossover green)
        Subgraph_BullishDivergence.LineWidth = 4;
        Subgraph_BullishDivergence.DrawZeros = false;

        Subgraph_BearishDivergence.Name = "Bearish Divergence";
        Subgraph_BearishDivergence.DrawStyle = DRAWSTYLE_ARROW_DOWN;
        Subgraph_BearishDivergence.PrimaryColor = RGB(255, 100, 0); // Orange (different from crossover red)
        Subgraph_BearishDivergence.LineWidth = 4;
        Subgraph_BearishDivergence.DrawZeros = false;

        sc.GraphRegion = 0; //Main chart region
        sc.DrawZeros = 0;
        sc.AutoLoop = 1;

        return;
    }

    // Do data processing

    // Calculate the stochastic (matches Sierra Chart sample)
    sc.Stochastic(sc.BaseDataIn, Subgraph_FastD, Input_FastK.GetInt(), Input_FastD.GetInt(), Input_SlowD.GetInt(), MOVAVGTYPE_SIMPLE);

    int Index = sc.Index;

    /* Position arrows slightly offset from bar high/low for appearance */
    float Offset = (sc.High[Index] - sc.Low[Index]) * (Input_ArrowOffsetPercentage.GetInt() * 0.01f);

    StochasticEnum stochastic = StochasticEnum::NORMAL;

    // --- Divergence Detection (angle-based, Sierra Chart method) ---
    if (Input_EnableDivergence.GetYesNo())
    {
        // Calculate price slope using linear regression
        double PriceSlope = 0;
        double PriceYIntercept = 0;
        Array_PriceInTicks[Index] = sc.Close[Index] / sc.TickSize;
        sc.CalculateRegressionStatistics(Array_PriceInTicks, PriceSlope, PriceYIntercept, Input_DivergenceLength.GetInt());

        // Calculate Stochastic slope using linear regression
        double StochasticSlope = 0;
        double StochasticYIntercept = 0;
        Array_StochasticInPoints[Index] = Subgraph_SlowK[Index];
        sc.CalculateRegressionStatistics(Array_StochasticInPoints, StochasticSlope, StochasticYIntercept, Input_DivergenceLength.GetInt());

        // Convert slopes to angles
        double PriceAngle = sc.SlopeToAngleInDegrees(PriceSlope);
        double StochasticAngle = sc.SlopeToAngleInDegrees(StochasticSlope);
        double AngleDifference = PriceAngle - StochasticAngle;

        float ThresholdDegrees = Input_DivergenceThreshold.GetFloat();
        float OppositeSlopeThresholdDegrees = Input_OppositeSlopeDivergenceThreshold.GetFloat();

        // Detect divergence (matches Sierra Chart scsf_DivergenceDetector logic exactly)
        if ((fabs(AngleDifference) >= ThresholdDegrees)
            || (PriceAngle > 0 && StochasticAngle < 0 && fabs(AngleDifference) >= OppositeSlopeThresholdDegrees)
            || (PriceAngle < 0 && StochasticAngle > 0 && fabs(AngleDifference) >= OppositeSlopeThresholdDegrees))
        {
            // Bullish divergence: Stochastic angle > Price angle
            if (StochasticAngle > PriceAngle)
            {
                Subgraph_BullishDivergence[Index] = sc.Low[Index] - (Offset * 1.5f);
                Subgraph_BearishDivergence[Index] = 0;
                stochastic = StochasticEnum::BULLISH_DIVERGENCE;
            }
            // Bearish divergence: Price angle > Stochastic angle
            else
            {
                Subgraph_BullishDivergence[Index] = 0;
                Subgraph_BearishDivergence[Index] = sc.High[Index] + (Offset * 1.5f);
                stochastic = StochasticEnum::BEARISH_DIVERGENCE;
            }
        }
        else
        {
            Subgraph_BullishDivergence[Index] = 0;
            Subgraph_BearishDivergence[Index] = 0;
        }
    }

    // --- Crossover Detection (only if no divergence detected) ---
    if (stochastic != StochasticEnum::BULLISH_DIVERGENCE && stochastic != StochasticEnum::BEARISH_DIVERGENCE)
    {
        const int crossResult = sc.CrossOver(Subgraph_SlowK, Subgraph_SlowD, Index);

        /* If Slow %k crosses Slow %d from the bottom AND %k is below 30 */
        if (crossResult == CROSS_FROM_BOTTOM &&
            (!Input_UseBuySell.GetYesNo() || Subgraph_SlowK[Index] < Input_Line2.GetFloat()))
        {
            Subgraph_Buy[Index] = sc.Low[Index] - Offset;
            Subgraph_Sell[Index] = 0;

            stochastic = StochasticEnum::OVER_SOLD;
        }
        /* If Slow %k crosses Slow %d from the top AND %k is over 70 */
        else if (crossResult == CROSS_FROM_TOP &&
            (!Input_UseBuySell.GetYesNo() || Subgraph_SlowK[Index] > Input_Line1.GetFloat()))
        {
            Subgraph_Sell[Index] = sc.High[Index] + Offset;
            Subgraph_Buy[Index] = 0;

            stochastic = StochasticEnum::OVER_BOUGHT;
        }
        else
        {
            Subgraph_Buy[Index] = 0;
            Subgraph_Sell[Index] = 0;
        }
    }
    else
    {
        // Divergence detected - clear crossover arrows (divergence arrows shown instead)
        Subgraph_Buy[Index] = 0;
        Subgraph_Sell[Index] = 0;
    }

    // Update IndicatorManager with final stochastic state (divergence takes priority)
    auto intermStochastic = IndicatorManager::Instance().GetIndicator<Stochastic>(IndicatorKey::INTERM_STOCHASTIC);
    if (intermStochastic) {
        intermStochastic->Update(stochastic);
    }
}
