#include "MindfulTrader_Precompiled.h"
#include "ContextManager.h"

/*==========================================================================*/
// TripleScreen3-specific constants
namespace {
    // Elder Breakout Detection Constants
    constexpr int ELDER_CONSOLIDATION_LOOKBACK = 5;
    constexpr int ELDER_MIN_CONSOLIDATION_BARS = 3;

    // Persistent Variable Keys
    constexpr int SG_LAST_PROCESSED_INDEX_ID = 200;

    // Subgraph Indices - Named constants for all sc.Subgraph[] array indices
    // This replaces magic numbers throughout the study for better maintainability
    enum SubgraphIndex {
        SG_KELTNER_AVERAGE = 0,
        SG_TOP_BAND = 1,
        SG_BOTTOM_BAND = 2,
        SG_ATR_TEMP = 3,                // ATR for bands, patterns, and downstream indicators
        SG_VWAP = 4,                     // Session VWAP (institutional anchor level)
        SG_VOLUME_SMA = 5,
        SG_RETIRED_VOLUME_STDDEV = 6,  // Retired v5.7 — never bound
        SG_AVG_RANGE = 7,
        SG_AVG_VOLUME = 8,
        SG_HIGHEST_HIGH_20 = 9,
        SG_LOWEST_LOW_20 = 10,
        SG_RSI3 = 11,
        SG_RSI10 = 12,
        SG_STOCH_K = 13,
        SG_STOCH_D = 14,
        SG_KANGAROO_TAIL_BULLISH = 15,
        SG_KANGAROO_TAIL_BEARISH = 16,
        SG_MOMENTUM_PINBALL_BULLISH = 17,
        SG_MOMENTUM_PINBALL_BEARISH = 18,
        SG_PREV_RSI3 = 19,
        SG_PREV_RSI10 = 20,
        SG_PREV_IMPULSE_COLOR = 21,
        SG_ELDER_BREAKOUT_BULLISH = 22,
        SG_ELDER_BREAKOUT_BEARISH = 23,
        SG_PREV_ATR = 24,
        SG_NR7_MARKER = 25,
        SG_NR7_BREAKOUT_BULLISH = 26,
        SG_NR7_BREAKOUT_BEARISH = 27,
        SG_EMA3 = 28,                    // 3-period EMA for 3-16 overnight oscillator (calculated on 15-min chart)
        SG_EMA16 = 29,                   // 16-period EMA for 3-16 overnight oscillator (calculated on 15-min chart)
        SG_RETIRED_ADX = 30,            // Retired — Hurst replaces ADX
        SG_ATR_AVG = 31,                 // 20-period SMA of ATR for intraday regime
        SG_TURTLE_SOUP_BULLISH = 32,     // Turtle Soup Bullish (Green Arrow)
        SG_TURTLE_SOUP_BEARISH = 33,     // Turtle Soup Bearish (Red Arrow)

        // === ELITE ObservationData support metrics - Quadrant II (Information) ===
        SG_PATH_EFFICIENCY_SNR = 34,    // Path directness SNR (20-bar efficiency ratio squared)
        SG_HURST_EXPONENT = 35,          // Rescaled Range H (0.0-2.0, persistence detector)
        SG_MICRO_ASYMMETRY = 36,         // Rolling (AskVol-BidVol)/VolumeSMA (20 bars)

        // === ELITE ObservationData support metrics - Quadrant III (Fragility) ===
        SG_REALIZED_KURTOSIS = 37,       // Excess 4th moment (fat tail detector, >3.0 = risk)
        SG_SKEWNESS_IDX = 38,            // 3rd moment (directional panic: +1 = retail, -1 = accumulation)
        SG_AMIHUD_ILLIQUIDITY = 39,     // Amihud Illiquidity |r_t|/V_t (adverse selection proxy)
        SG_LIQ_FRAGILITY = 40,           // Bid-Ask spread volatility stress index
        SG_EMA13_ANCHOR = 41,            // EMA(13) for structural anchor distance (15-min directional smoothing = 3.25 hours)

        // === PHASE C: Hidden Rolling Buffers for O(1) Institutional Performance ===
        // These subgraphs store intermediate calculations for native SC functions (sc.SimpleMovingAverage, etc.)
        // This architecture eliminates manual loops: O(n) -> O(1) per tick during high-volume regimes
        SG_LOG_RET = 42,                 // Log returns for moments (Kurtosis/Skewness)
        SG_MEAN_RET = 43,                // Mean of log returns (SMA output)
        SG_STD_DEV = 44,                 // StdDev of log returns (used for normalization)
        SG_M4_BUFFER = 45,               // 4th power of deviations: (x - mean)^4
        SG_M4_AVG = 46,                  // Moving average of M4 (4th moment)
        SG_M3_BUFFER = 47,               // 3rd power of deviations: (x - mean)^3 (for skewness)
        SG_M3_AVG = 48,                  // Moving average of M3 (3rd moment)
        SG_CUMUL_DEV = 49,               // Cumulative deviations for Hurst R/S analysis
        SG_ABS_IMBAL = 50,               // Absolute (AskVol - BidVol) for Amihud illiquidity
        SG_IMBAL_AVG = 51,               // Moving average of imbalance (Amihud 20-bar window)
        SG_ASYM_BUFFER = 52,             // Micro Asymmetry: (AskVol - BidVol) / VolSMA
        SG_ASYM_AVG = 53                 // Moving average of asymmetry
    };
}



/*==========================================================================*/
/*
 * scsf_Screen3_KeltnerChannel
 *
 * Elder Triple Screen - Screen 3: Short-term Keltner Channel
 *
 * Intraday (short-term) volatility envelope and trade entry signals.
 *
 * Configuration:
 * - 10-period EMA centerline (default, user configurable)
 * - ATR-based bands (default 10-period, asymmetric multipliers)
 * - 14-period average for additional smoothing
 * - Volume analysis (SMA + StdDev for volume confirmation)
 * - 20-period Highest/Lowest for breakout detection
 *
 * Features:
 * - Elder breakout detection (consolidation → expansion)
 * - Volume spike detection (> 2 StdDev)
 * - Average range calculation (price volatility proxy)
 * - Keltner band breakout signals
 *
 * Architecture Note:
 * - EMA3 (SG_EMA3) and EMA16 (SG_EMA16) are calculated on the same 15-min chart as this study
 * - They are exported via Subgraphs 28-29 for DataCollectorStudy to read via same-chart study reference
 * - This is NOT a cross-chart reference to TripleScreen1 (240-min) - these are 15-min EMAs for the 3-16 oscillator
 * - DataCollectorStudy must reference the correct study number (TripleScreen3's actual study ID in Sierra Chart GUI)
 *
 * References:
 * - Elder, Alexander. "Come Into My Trading Room" (2002), Chapter 9
 * - Official Sierra Chart ATR pattern from Studies7.cpp:3840-3920
 */
SCSFExport scsf_Screen3_KeltnerChannel(SCStudyInterfaceRef sc)
{
    // Using named constants from SubgraphIndex enum instead of magic numbers
    SCSubgraphRef Subgraph_KeltnerAverage = sc.Subgraph[SG_KELTNER_AVERAGE];
    SCSubgraphRef Subgraph_TopBand = sc.Subgraph[SG_TOP_BAND];
    SCSubgraphRef Subgraph_BottomBand = sc.Subgraph[SG_BOTTOM_BAND];
    SCSubgraphRef Subgraph_AtrTemp3 = sc.Subgraph[SG_ATR_TEMP];

    SCSubgraphRef Subgraph_VolumeSma = sc.Subgraph[SG_VOLUME_SMA];
    // VolumeStdDev removed in v5.7 — VolumeIndicator uses robust log-vol median/MAD internally
    SCSubgraphRef Subgraph_AvgRange = sc.Subgraph[SG_AVG_RANGE];
    SCSubgraphRef Subgraph_AvgVolume = sc.Subgraph[SG_AVG_VOLUME];
    SCSubgraphRef Subgraph_HighestHigh20Period = sc.Subgraph[SG_HIGHEST_HIGH_20];
    SCSubgraphRef Subgraph_LowestLow20Period = sc.Subgraph[SG_LOWEST_LOW_20];
    SCSubgraphRef Subgraph_RSI3 = sc.Subgraph[SG_RSI3];
    SCSubgraphRef Subgraph_RSI10 = sc.Subgraph[SG_RSI10];
    SCSubgraphRef Subgraph_StochK = sc.Subgraph[SG_STOCH_K];
    SCSubgraphRef Subgraph_StochD = sc.Subgraph[SG_STOCH_D];
    SCSubgraphRef Subgraph_KangarooTailBullish = sc.Subgraph[SG_KANGAROO_TAIL_BULLISH];
    SCSubgraphRef Subgraph_KangarooTailBearish = sc.Subgraph[SG_KANGAROO_TAIL_BEARISH];
    SCSubgraphRef Subgraph_MomentumPinballBullish = sc.Subgraph[SG_MOMENTUM_PINBALL_BULLISH];
    SCSubgraphRef Subgraph_MomentumPinballBearish = sc.Subgraph[SG_MOMENTUM_PINBALL_BEARISH];
    SCSubgraphRef Subgraph_PrevRSI3 = sc.Subgraph[SG_PREV_RSI3];
    SCSubgraphRef Subgraph_PrevRSI10 = sc.Subgraph[SG_PREV_RSI10];
    SCSubgraphRef Subgraph_PrevImpulseColor = sc.Subgraph[SG_PREV_IMPULSE_COLOR];
    SCSubgraphRef Subgraph_ElderBreakoutBullish = sc.Subgraph[SG_ELDER_BREAKOUT_BULLISH];
    SCSubgraphRef Subgraph_ElderBreakoutBearish = sc.Subgraph[SG_ELDER_BREAKOUT_BEARISH];
    SCSubgraphRef Subgraph_PrevATR = sc.Subgraph[SG_PREV_ATR];
    SCSubgraphRef Subgraph_NR7Marker = sc.Subgraph[SG_NR7_MARKER];
    SCSubgraphRef Subgraph_NR7BreakoutBullish = sc.Subgraph[SG_NR7_BREAKOUT_BULLISH];
    SCSubgraphRef Subgraph_NR7BreakoutBearish = sc.Subgraph[SG_NR7_BREAKOUT_BEARISH];
    SCSubgraphRef Subgraph_EMA3 = sc.Subgraph[SG_EMA3];
    SCSubgraphRef Subgraph_EMA16 = sc.Subgraph[SG_EMA16];
    SCSubgraphRef Subgraph_RetiredADX = sc.Subgraph[SG_RETIRED_ADX];
    SCSubgraphRef Subgraph_ATRAvg = sc.Subgraph[SG_ATR_AVG];
    SCSubgraphRef Subgraph_TurtleSoupBullish = sc.Subgraph[SG_TURTLE_SOUP_BULLISH];
    SCSubgraphRef Subgraph_TurtleSoupBearish = sc.Subgraph[SG_TURTLE_SOUP_BEARISH];
    SCSubgraphRef Subgraph_Vwap = sc.Subgraph[SG_VWAP];
    SCFloatArrayRef Array_VwapCumPriceVol = Subgraph_Vwap.Arrays[0];
    SCFloatArrayRef Array_VwapCumVol = Subgraph_Vwap.Arrays[1];

    // === ELITE ObservationData support metric references ===
    SCSubgraphRef Subgraph_PathEfficiencySNR = sc.Subgraph[SG_PATH_EFFICIENCY_SNR];
    SCSubgraphRef Subgraph_HurstExponent = sc.Subgraph[SG_HURST_EXPONENT];
    SCSubgraphRef Subgraph_MicroAsymmetry = sc.Subgraph[SG_MICRO_ASYMMETRY];
    SCSubgraphRef Subgraph_RealizedKurtosis = sc.Subgraph[SG_REALIZED_KURTOSIS];
    SCSubgraphRef Subgraph_SkewnessIdx = sc.Subgraph[SG_SKEWNESS_IDX];
    SCSubgraphRef Subgraph_AmihudIlliquidity = sc.Subgraph[SG_AMIHUD_ILLIQUIDITY];
    SCSubgraphRef Subgraph_LiqFragility = sc.Subgraph[SG_LIQ_FRAGILITY];
    SCSubgraphRef Subgraph_EMA13Anchor = sc.Subgraph[SG_EMA13_ANCHOR];

    // === PHASE C: Hidden Rolling Buffers (Institutional O(1) Performance) ===
    SCSubgraphRef Subgraph_LogRet = sc.Subgraph[SG_LOG_RET];
    SCSubgraphRef Subgraph_MeanRet = sc.Subgraph[SG_MEAN_RET];
    SCSubgraphRef Subgraph_StdDevRet = sc.Subgraph[SG_STD_DEV];
    SCSubgraphRef Subgraph_M4Buffer = sc.Subgraph[SG_M4_BUFFER];
    SCSubgraphRef Subgraph_M4Avg = sc.Subgraph[SG_M4_AVG];
    SCSubgraphRef Subgraph_M3Buffer = sc.Subgraph[SG_M3_BUFFER];
    SCSubgraphRef Subgraph_M3Avg = sc.Subgraph[SG_M3_AVG];
    SCSubgraphRef Subgraph_CumulDev = sc.Subgraph[SG_CUMUL_DEV];
    SCSubgraphRef Subgraph_AbsImbal = sc.Subgraph[SG_ABS_IMBAL];
    SCSubgraphRef Subgraph_ImbalAvg = sc.Subgraph[SG_IMBAL_AVG];
    SCSubgraphRef Subgraph_AsymBuffer = sc.Subgraph[SG_ASYM_BUFFER];
    SCSubgraphRef Subgraph_AsymAvg = sc.Subgraph[SG_ASYM_AVG];

    SCInputRef Input_Data = sc.Input[0];
    SCInputRef Input_KeltnerMALength = sc.Input[3];
    SCInputRef Input_BandMultiplier = sc.Input[5];  // Single multiplier for symmetric bands (Elder's approach)
    SCInputRef Input_KeltnerMAType = sc.Input[7];
    SCInputRef Input_AvgLength = sc.Input[11];

    if (sc.SetDefaults)
    {
        sc.GraphName = "Screen 3 - Keltner Channel";

        sc.GraphRegion = 0;
        sc.ValueFormat = 3;
        sc.DrawZeros = 0;
        sc.FreeDLL = 0;
        // ENABLE INTRA-BAR UPDATES FOR PHYSICS (Recurrence Rate / Fractal Dim)
        sc.AutoLoop = 1;

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

        Subgraph_VolumeSma.Name = "Volume SMA";
        Subgraph_VolumeSma.DrawStyle = DRAWSTYLE_IGNORE;

        // VolumeStdDev init removed in v5.7 (no longer needed)

        Subgraph_AvgRange.Name = "Avg Range";
        Subgraph_AvgRange.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_AvgVolume.Name = "Avg Volume";
        Subgraph_AvgVolume.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_HighestHigh20Period.Name = "20 Period Highest High";
        Subgraph_HighestHigh20Period.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_LowestLow20Period.Name = "20 Period Lowest Low";
        Subgraph_LowestLow20Period.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_RetiredADX.Name = "ADX [RETIRED]";
        Subgraph_RetiredADX.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_RetiredADX.DrawZeros = false;

        Subgraph_ATRAvg.Name = "ATR Avg(20)";
        Subgraph_ATRAvg.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_ATRAvg.DrawZeros = false;

        Subgraph_TurtleSoupBullish.Name = "Turtle Soup Bullish";
        Subgraph_TurtleSoupBullish.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_TurtleSoupBullish.DrawZeros = false;

        Subgraph_TurtleSoupBearish.Name = "Turtle Soup Bearish";
        Subgraph_TurtleSoupBearish.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_TurtleSoupBearish.DrawZeros = false;

        Subgraph_Vwap.Name = "Session VWAP";
        Subgraph_Vwap.DrawStyle = DRAWSTYLE_DASH;
        Subgraph_Vwap.LineWidth = 2;
        Subgraph_Vwap.PrimaryColor = RGB(0, 191, 255);  // Deep sky blue
        Subgraph_Vwap.DrawZeros = false;

        Subgraph_KangarooTailBullish.Name = "Kangaroo Tail Bullish";
        Subgraph_KangarooTailBullish.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_KangarooTailBullish.PrimaryColor = RGB(0, 200, 0);
        Subgraph_KangarooTailBullish.LineWidth = 2;
        Subgraph_KangarooTailBullish.DrawZeros = false;

        Subgraph_KangarooTailBearish.Name = "Kangaroo Tail Bearish";
        Subgraph_KangarooTailBearish.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_KangarooTailBearish.PrimaryColor = RGB(200, 0, 0);
        Subgraph_KangarooTailBearish.LineWidth = 2;
        Subgraph_KangarooTailBearish.DrawZeros = false;

        Subgraph_MomentumPinballBullish.Name = "Momentum Pinball Bullish";
        Subgraph_MomentumPinballBullish.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_MomentumPinballBullish.PrimaryColor = RGB(0, 180, 255);
        Subgraph_MomentumPinballBullish.LineWidth = 2;
        Subgraph_MomentumPinballBullish.DrawZeros = false;

        Subgraph_MomentumPinballBearish.Name = "Momentum Pinball Bearish";
        Subgraph_MomentumPinballBearish.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_MomentumPinballBearish.PrimaryColor = RGB(255, 130, 0);
        Subgraph_MomentumPinballBearish.LineWidth = 2;
        Subgraph_MomentumPinballBearish.DrawZeros = false;

        Subgraph_ElderBreakoutBullish.Name = "Elder Breakout Bullish";
        Subgraph_ElderBreakoutBullish.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_ElderBreakoutBullish.PrimaryColor = RGB(0, 255, 0);
        Subgraph_ElderBreakoutBullish.LineWidth = 2;
        Subgraph_ElderBreakoutBullish.DrawZeros = false;

        Subgraph_ElderBreakoutBearish.Name = "Elder Breakout Bearish";
        Subgraph_ElderBreakoutBearish.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_ElderBreakoutBearish.PrimaryColor = RGB(255, 0, 0);
        Subgraph_ElderBreakoutBearish.LineWidth = 2;
        Subgraph_ElderBreakoutBearish.DrawZeros = false;

        Subgraph_NR7Marker.Name = "NR7 Marker";
        Subgraph_NR7Marker.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_NR7Marker.PrimaryColor = RGB(0, 255, 255);
        Subgraph_NR7Marker.LineWidth = 3;
        Subgraph_NR7Marker.DrawZeros = false;

        Subgraph_NR7BreakoutBullish.Name = "NR7 Breakout Bullish";
        Subgraph_NR7BreakoutBullish.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_NR7BreakoutBullish.PrimaryColor = RGB(0, 220, 0);
        Subgraph_NR7BreakoutBullish.LineWidth = 2;
        Subgraph_NR7BreakoutBullish.DrawZeros = false;

        Subgraph_NR7BreakoutBearish.Name = "NR7 Breakout Bearish";
        Subgraph_NR7BreakoutBearish.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_NR7BreakoutBearish.PrimaryColor = RGB(220, 0, 0);
        Subgraph_NR7BreakoutBearish.LineWidth = 2;
        Subgraph_NR7BreakoutBearish.DrawZeros = false;

        Subgraph_PrevRSI3.Name = "Internal: Prev RSI3";
        Subgraph_PrevRSI3.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_PrevRSI3.DrawZeros = false;

        Subgraph_PrevRSI10.Name = "Internal: Prev RSI10";
        Subgraph_PrevRSI10.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_PrevRSI10.DrawZeros = false;

        Subgraph_PrevImpulseColor.Name = "Internal: Prev Impulse Color";
        Subgraph_PrevImpulseColor.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_PrevImpulseColor.DrawZeros = false;

        Subgraph_PrevATR.Name = "Internal: Prev ATR";
        Subgraph_PrevATR.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_PrevATR.DrawZeros = false;

        // === ELITE ObservationData support metrics - Quadrant II (Information) ===
        // These subgraphs are computation-only for HMM/FlatBuffer export
        // Not drawn on chart, but critical for HMM state machine

        Subgraph_PathEfficiencySNR.Name = "Path Efficiency SNR";
        Subgraph_PathEfficiencySNR.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_PathEfficiencySNR.DrawZeros = false;

        Subgraph_HurstExponent.Name = "Hurst Exponent (R/S)";
        Subgraph_HurstExponent.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_HurstExponent.DrawZeros = false;

        Subgraph_MicroAsymmetry.Name = "Micro Asymmetry (Vol Delta)";
        Subgraph_MicroAsymmetry.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_MicroAsymmetry.DrawZeros = false;

        // === ELITE ObservationData support metrics - Quadrant III (Fragility) ===

        Subgraph_RealizedKurtosis.Name = "Realized Kurtosis (4th Moment)";
        Subgraph_RealizedKurtosis.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_RealizedKurtosis.DrawZeros = false;

        Subgraph_SkewnessIdx.Name = "Skewness Index (3rd Moment)";
        Subgraph_SkewnessIdx.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_SkewnessIdx.DrawZeros = false;

        Subgraph_AmihudIlliquidity.Name = "Amihud Illiquidity (|r|/V)";
        Subgraph_AmihudIlliquidity.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_AmihudIlliquidity.DrawZeros = false;

        Subgraph_LiqFragility.Name = "Liquidity Fragility (Spread Stress)";
        Subgraph_LiqFragility.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_LiqFragility.DrawZeros = false;

        Subgraph_EMA13Anchor.Name = "EMA13 (Anchor Distance)";
        Subgraph_EMA13Anchor.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_EMA13Anchor.DrawZeros = false;

        // === PHASE C: Hidden Rolling Buffers (allocated but not displayed) ===
        // These subgraphs are internal computation buffers for native SC functions
        // Allocated here to pre-allocate space; SC manages the sliding window logic
        // CRITICAL: Use DRAWSTYLE_IGNORE to prevent these small values (0.001) from wrecking
        // the main price chart scaling (5000+) when GraphRegion=0.
        Subgraph_LogRet.Name = "Internal: Log Returns";
        Subgraph_LogRet.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_MeanRet.Name = "Internal: Mean Log Return";
        Subgraph_MeanRet.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_StdDevRet.Name = "Internal: StdDev Log Return";
        Subgraph_StdDevRet.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_M4Buffer.Name = "Internal: 4th Moment Buffer";
        Subgraph_M4Buffer.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_M4Avg.Name = "Internal: 4th Moment Average";
        Subgraph_M4Avg.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_M3Buffer.Name = "Internal: 3rd Moment Buffer";
        Subgraph_M3Buffer.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_M3Avg.Name = "Internal: 3rd Moment Average";
        Subgraph_M3Avg.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_CumulDev.Name = "Internal: Cumulative Deviations";
        Subgraph_CumulDev.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_AbsImbal.Name = "Internal: Absolute Imbalance";
        Subgraph_AbsImbal.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_ImbalAvg.Name = "Internal: Imbalance Average";
        Subgraph_ImbalAvg.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_AsymBuffer.Name = "Internal: Asymmetry Buffer";
        Subgraph_AsymBuffer.DrawStyle = DRAWSTYLE_IGNORE;

        Subgraph_AsymAvg.Name = "Internal: Asymmetry Average";
        Subgraph_AsymAvg.DrawStyle = DRAWSTYLE_IGNORE;


    // EMA3 and EMA16 for 3-10 oscillator (computation only, exported to DataCollector)
        Subgraph_EMA3.Name = "EMA3 (Oscillator)";
        Subgraph_EMA3.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_EMA3.DrawZeros = false;

        Subgraph_EMA16.Name = "EMA16 (Oscillator)";
        Subgraph_EMA16.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_EMA16.DrawZeros = false;

        // RSI subgraphs (computation only)
        Subgraph_RSI3.Name = "RSI3";
        Subgraph_RSI3.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_RSI3.DrawZeros = false;

        Subgraph_RSI10.Name = "RSI10";
        Subgraph_RSI10.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_RSI10.DrawZeros = false;

        // Stochastic subgraphs (computation only)
        Subgraph_StochK.Name = "Stochastic K";
        Subgraph_StochK.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_StochK.DrawZeros = false;

        Subgraph_StochD.Name = "Stochastic D";
        Subgraph_StochD.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_StochD.DrawZeros = false;

        // ATR temporary subgraph (computation only)
        Subgraph_AtrTemp3.Name = "ATR Temp";
        Subgraph_AtrTemp3.DrawStyle = DRAWSTYLE_IGNORE;
        Subgraph_AtrTemp3.DrawZeros = false;

        Input_Data.Name = "Input Data";
        Input_Data.SetInputDataIndex(SC_OHLC_AVG);

        Input_KeltnerMALength.Name = "Keltner Mov Avg Length";
        Input_KeltnerMALength.SetInt(10);
        Input_KeltnerMALength.SetIntLimits(1, MAX_STUDY_LENGTH);

        Input_BandMultiplier.Name = "Band Multiplier (ATR)";
        Input_BandMultiplier.SetFloat(2.0f);  // Elder's standard (symmetric: same for top & bottom)
        Input_BandMultiplier.SetFloatLimits(0.1f, 10.0f);

        Input_KeltnerMAType.Name = "Keltner Mov Avg Type (Center line)";
        Input_KeltnerMAType.SetMovAvgType(MOVAVGTYPE_EXPONENTIAL);

        Input_AvgLength.Name = "Avg Length";
        Input_AvgLength.SetInt(14);
        Input_AvgLength.SetIntLimits(1, MAX_STUDY_LENGTH);


        // DON'T initialize persistent variables here - they're managed by pattern detection functions
        // Turtle Soup detection in StudyHelperFunctions.cpp manages its own persistent vars

        return;
    }

    if (sc.LastCallToFunction) {
        CleanupAdaptiveCalculators(sc);
        return;
    }

    if (sc.IsFullRecalculation && sc.UpdateStartIndex == 0) {
        ResetAdaptiveCalculators(sc);
    }

    // DataStartIndex calculation: Use Screen2's working approach (MUST be outside SetDefaults)
    sc.DataStartIndex = std::max(Input_KeltnerMALength.GetInt(), 10);

    // ========================================================================
    // INSTITUTIONAL PHYSICS ENGINE INTEGRATION (Live & Historical)
    // ========================================================================
    // 1. Update Market Physics (Log Returns -> Entropy, Lempel-Ziv, VolZScore)
    // ONLY executed once per bar (on bar close) to preserve time-series integrity.
    int& lastPhysicsIndex = sc.GetPersistentInt(SG_LAST_PROCESSED_INDEX_ID);
    if (sc.Index > lastPhysicsIndex) {
        if (sc.Index >= 2) {
            // SCStudies is the single owner for UpdateMarketPhysics() in live runtime.
            // Keep Screen3 focused on local study metrics to avoid duplicate feed paths.

            // Populate Ripple context (TS3 local dynamics only).
            float atr = Subgraph_AtrTemp3[sc.Index - 1];
            float relRange = (atr > 0.00001f) ? ((sc.High[sc.Index - 1] - sc.Low[sc.Index - 1]) / atr) : 0.0f;

            StatisticalContext ctx;
            ctx.volatility = atr;  // Cache ATR proxy for downstream normalization (distance-to-cliff, etc.)
            ctx.relRange = relRange;
            ctx.velocity = 0.0f;
            ctx.lastUpdated = sc.BaseDateTimeIn[sc.Index - 1];
            ContextManager::Instance().SetRippleContext(std::move(ctx));
        }
        lastPhysicsIndex = sc.Index;
    }

    // Price-structure ownership remains in SCStudies to avoid duplicate feeds.

    // Clear one-shot marker subgraphs each bar to avoid stale artifacts.
    Subgraph_KangarooTailBullish[sc.Index] = 0.0f;
    Subgraph_KangarooTailBearish[sc.Index] = 0.0f;
    Subgraph_MomentumPinballBullish[sc.Index] = 0.0f;
    Subgraph_MomentumPinballBearish[sc.Index] = 0.0f;
    Subgraph_ElderBreakoutBullish[sc.Index] = 0.0f;
    Subgraph_ElderBreakoutBearish[sc.Index] = 0.0f;
    Subgraph_NR7Marker[sc.Index] = 0.0f;
    Subgraph_NR7BreakoutBullish[sc.Index] = 0.0f;
    Subgraph_NR7BreakoutBearish[sc.Index] = 0.0f;

    // === Keltner Channel and Pattern Detection Code ===
    // Use built-in functions but keep ATR visible for downstream indicators
    sc.MovingAverage(sc.BaseDataIn[static_cast<int>(Input_Data.GetInputDataIndex())],
                     Subgraph_KeltnerAverage,
                     Input_KeltnerMAType.GetMovAvgType(),
                     Input_KeltnerMALength.GetInt());

    // ATR(10) Wilder's smoothing — institutional standard (hardcoded, not configurable)
    sc.ATR(sc.BaseDataIn, Subgraph_AtrTemp3, 10, MOVAVGTYPE_WILDERS);
    // Baseline ATR regime reference for liquidity fragility adaptation.
    sc.SimpleMovAvg(Subgraph_AtrTemp3, Subgraph_ATRAvg, 20);
    // Removed duplicate: Subgraph_AtrTemp4 = Subgraph_AtrTemp3 (same calculation)

    const float multiplier = Input_BandMultiplier.GetFloat();
    Subgraph_TopBand[sc.Index] = Subgraph_KeltnerAverage[sc.Index] + (Subgraph_AtrTemp3[sc.Index] * multiplier);
    Subgraph_BottomBand[sc.Index] = Subgraph_KeltnerAverage[sc.Index] - (Subgraph_AtrTemp3[sc.Index] * multiplier);

    // Calculate 20-period Highest High and Lowest Low for support/resistance
    sc.Highest(sc.High, Subgraph_HighestHigh20Period, 20);
    sc.Lowest(sc.Low, Subgraph_LowestLow20Period, 20);

    // === Session VWAP: Σ(TP×Vol)/Σ(Vol) with daily reset ===
    {
        const float tp = (sc.High[sc.Index] + sc.Low[sc.Index] + sc.Close[sc.Index]) / 3.0f;
        const float vol = static_cast<float>(sc.Volume[sc.Index]);

        // Detect new trading session (handles Globex/RTH correctly for ES)
        bool newSession = (sc.Index == 0);
        if (sc.Index > 0) {
            SCDateTime prevDay = sc.GetTradingDayStartDateTimeOfBar(sc.BaseDateTimeIn[sc.Index - 1]);
            SCDateTime currDay = sc.GetTradingDayStartDateTimeOfBar(sc.BaseDateTimeIn[sc.Index]);
            newSession = (currDay != prevDay);
        }

        if (newSession) {
            Array_VwapCumPriceVol[sc.Index] = tp * vol;
            Array_VwapCumVol[sc.Index] = vol;
        } else {
            Array_VwapCumPriceVol[sc.Index] = Array_VwapCumPriceVol[sc.Index - 1] + tp * vol;
            Array_VwapCumVol[sc.Index] = Array_VwapCumVol[sc.Index - 1] + vol;
        }

        const float cumVol = Array_VwapCumVol[sc.Index];
        const float vwapPrice = (cumVol > 0.0f)
            ? (Array_VwapCumPriceVol[sc.Index] / cumVol)
            : tp;
        Subgraph_Vwap[sc.Index] = vwapPrice;

        // Update VwapIndicator in IndicatorManager
        const float atr = Subgraph_AtrTemp3[sc.Index];
        auto* vwapInd = IndicatorManager::Instance().GetIndicator<VwapIndicator>(IndicatorKey::VWAP);
        if (vwapInd) {
            vwapInd->UpdateVwap(tp, vol, atr, newSession);
        }
    }

    // ADX computation retired — Hurst exponent provides superior persistence measurement.
    // Subgraph_RetiredADX slot preserved as DRAWSTYLE_IGNORE to avoid index shifts.

    // Cache singleton references — avoid 26+ singleton lookups per tick
    auto& indMgr = IndicatorManager::Instance();
    auto& infMgr = InferenceManager::Instance();

    // Update indicators with new high/low values
    const auto shortPriceAction = indMgr.GetIndicator<ShortMarketAction>(IndicatorKey::SHORT_MKT_ACTION);

    // Get cached previous day high/low from IndicatorManager (updated centrally in UpdateBarContext)
    const float prevDayHigh = indMgr.GetCachedPrevDayHigh();
    const float prevDayLow = indMgr.GetCachedPrevDayLow();

    if (shortPriceAction) {
        // Ensure sufficient historical data is available before accessing sc.High[sc.Index - 2], etc.
        if (sc.Index >= 3) {
            // Event-driven path: update every call using ONLY completed bars
            // (sc.Index - 1, -2, -3), so values are stable intra-bar.
            const float maxHigh = std::max({sc.High[sc.Index - 1], sc.High[sc.Index - 2], sc.High[sc.Index - 3]});
            const float minLow = std::min({sc.Low[sc.Index - 1], sc.Low[sc.Index - 2], sc.Low[sc.Index - 3]});

            // Set previous bar high/low and 3-bar extremes
            shortPriceAction->SetPriceValues(sc.Index, sc.High[sc.Index - 1],
                                             sc.Low[sc.Index - 1], maxHigh, minLow);

            // Set previous trading day's session high/low for training data export
            shortPriceAction->SetPrevDayHighLow(prevDayHigh, prevDayLow);
        }
    }

    const auto structureTest = indMgr.GetIndicator<StructureTestIndicator>(IndicatorKey::STRUCTURE_TEST);
    if (structureTest) {
        structureTest->Update(DetectStructure(sc, prevDayHigh, prevDayLow,
            Subgraph_AtrTemp3[sc.Index], Subgraph_HighestHigh20Period[sc.Index], Subgraph_LowestLow20Period[sc.Index]));
    }

    sc.SimpleMovAvg(sc.Volume, Subgraph_VolumeSma, 21);
    // StdDeviation removed in v5.7 — VolumeIndicator uses robust log-vol median/MAD

    const auto volumeIndicator = indMgr.GetIndicator<VolumeIndicator>(IndicatorKey::VOLUME_SIGNAL);
    if (volumeIndicator) {
        // v5.7: VolumeIndicator self-classifies using robust z-score thresholds
        // + computes order-flow imbalance from bid/ask split
        // (replaces Gaussian GetVolumeEnum — thresholds are now fat-tail stable)
        volumeIndicator->UpdateVolume(static_cast<float>(sc.Volume[sc.Index]),
                                      static_cast<float>(sc.BidVolume[sc.Index]),
                                      static_cast<float>(sc.AskVolume[sc.Index]));
    }

    const auto atrProximity = indMgr.GetIndicator<ATRProximityIndicator>(IndicatorKey::ATR_PROXIMITY);
    if (atrProximity) {
        atrProximity->Update(DetectATRProximity(sc, Subgraph_AtrTemp3[sc.Index]));

        // Elite v2.0: Cache ATR(10) for transformer rel_range (architectural owner of volatility metrics)
        atrProximity->SetATR10(Subgraph_AtrTemp3[sc.Index]);
    }

    // Gap 1: Feed PositionManager so it avoids redundant per-tick TR loops
    PositionManager::Instance().SetCachedATR10(Subgraph_AtrTemp3[sc.Index]);

    SCFloatArrayRef barRange = Subgraph_AvgRange.Arrays[0];
    barRange[sc.Index] = sc.High[sc.Index] - sc.Low[sc.Index];
    sc.SimpleMovAvg(barRange, Subgraph_AvgRange, Input_AvgLength.GetInt());
    sc.SimpleMovAvg(sc.Volume, Subgraph_AvgVolume, Input_AvgLength.GetInt());

    const auto priceMetrics = indMgr.GetIndicator<PriceMetricsIndicator>(IndicatorKey::PRICE_METRICS);
    if (priceMetrics) {
        priceMetrics->SetOHLC(sc.Open[sc.Index], sc.High[sc.Index], sc.Low[sc.Index], sc.Close[sc.Index]);
        priceMetrics->Update(DeterminePriceMetric(sc, Subgraph_AvgRange[sc.Index], Subgraph_AvgVolume[sc.Index]));
    }

    // === ELITE v2.5: Observation Vector Update (Shared with TurtleSoup) ===
    // Cross-timeframe coherence from LONG/INTERM MACD momentum drives adaptive lookback.
    float coherence_score = 0.5f;  // neutral fallback
    const auto longMacdIndicator = indMgr.GetIndicator<Macd>(IndicatorKey::LONG_MACD);
    const auto intermMacdIndicator = indMgr.GetIndicator<Macd>(IndicatorKey::INTERM_MACD);
    if (longMacdIndicator && intermMacdIndicator) {
        const float tf1_hist = static_cast<float>(longMacdIndicator->MacdValue());
        const float tf2_hist = static_cast<float>(intermMacdIndicator->MacdValue());
        const float max_observed = std::max({0.01f, std::abs(tf1_hist), std::abs(tf2_hist)});
        coherence_score = CheckCoherence(tf1_hist, tf2_hist, max_observed);
    }
    const int observation_window_n = CalculateAdaptiveObservationWindow(sc, coherence_score);
    UpdateObservationVectorSubgraphs(sc, observation_window_n,
        Subgraph_PathEfficiencySNR, Subgraph_HurstExponent,
        Subgraph_MicroAsymmetry, Subgraph_RealizedKurtosis,
        Subgraph_SkewnessIdx, Subgraph_AmihudIlliquidity,
        Subgraph_LiqFragility, Subgraph_AtrTemp3, Subgraph_VolumeSma);

    sc.RSI(sc.Close, Subgraph_RSI3, MOVAVGTYPE_SIMPLE, 3);
    sc.RSI(sc.Close, Subgraph_RSI10, MOVAVGTYPE_SIMPLE, 10);
    sc.Stochastic(sc.BaseDataIn, Subgraph_StochK, 3, 3, 3, MOVAVGTYPE_SIMPLE);
    Subgraph_StochD[sc.Index] = Subgraph_StochK.Arrays[1][sc.Index]; // Extract %D from the calculation

    const auto raschkeTacticalIndicator = indMgr.GetIndicator<RaschkeTacticalIndicator>(IndicatorKey::RASCHKE_TACTICAL_TRIGGER);
    if (raschkeTacticalIndicator) {
        raschkeTacticalIndicator->Update(DetectRaschkeTacticalTrigger(sc, Subgraph_RSI3[sc.Index], Subgraph_RSI10[sc.Index], Subgraph_StochK[sc.Index]));
    }

    const auto dailyBiasIndicator = indMgr.GetIndicator<DailyBiasIndicator>(IndicatorKey::DAILY_BIAS);
    if (dailyBiasIndicator) {
        // Elite v2.5: Inject Physics Veto (Hurst/Entropy) into Bias to filter Random Walk
        // This stops directional bias when information content is low (Shannon/Mandelbrot)
        dailyBiasIndicator->Update(CalculateDailyBias(
            sc.Close[sc.Index],
            prevDayHigh,
            prevDayLow,
            Subgraph_HurstExponent[sc.Index],
            Subgraph_PathEfficiencySNR[sc.Index]
        ));


    }

    // --- Time of Day Session Detection ---
    // Compute session window from current bar timestamp (critical for session filtering gates)
    const auto timeOfDayIndicator = indMgr.GetIndicator<TimeOfDayIndicator>(IndicatorKey::TIME_OF_DAY);
    if (timeOfDayIndicator) {
        // Use the centralized TimeOfDay logic in Indicator.cpp
        // This ensures identical session definitions across all studies and fixes
        // minute-level boundary bugs present in the old manual logic.
        timeOfDayIndicator->SetFromDateTime(sc.BaseDateTimeIn[sc.Index], false);
    }

    // --- Kangaroo Tail Detection (Elder/Raschke) ---
    // Detect long-shadow reversal bars for precise entry timing
    constexpr float SUPPORT_RESISTANCE_THRESHOLD = 0.5f;

    float tailToBodyRatio = 0.0f;
    float tailToATR = 0.0f;
    float closePosition = 0.0f;
    float qualityScore = 0.0f;


    // === INSTITUTIONAL-GRADE CLIMATE ASSESSMENT (HYBRID ARCHITECTURE) ===
    // Elite v3.2: Gang metrics already in ContextManager from BuildObservationVector.
    // ADX retired — Hurst exponent provides superior trend persistence measurement.

    // Check HMM State for "Deep Context" (Pattern Recognition)
    HMMStateEnum currentHmmState = HMM_NO_PRIOR;
    auto* hmmIndicator = infMgr.HmmState();
    if (hmmIndicator) {
        currentHmmState = hmmIndicator->Value();
    }

    // === INSTITUTIONAL REFACTOR: Centralized Logic in Indicator ===
    // MarketClimateIndicator reads from ContextManager's LocalRiskContext (single source of truth)
    MarketClimate currentClimate = MarketClimate::GAUSSIAN_STABLE;
    auto* climateIndicator = infMgr.MutableClimate();
    if (climateIndicator) {
        const auto& riskCtx = ContextManager::Instance().GetLocalRiskContext();
        climateIndicator->UpdateContext(riskCtx, currentHmmState);
    }

    // ========================================================================
    // CANONICAL OBSERVATIONDATA VECTOR - SCREEN 3 (RIPPLE)
    // Calculated on 15-min bars (Ripple)
    // ========================================================================

    // Canonical source: use adaptive-window subgraph outputs computed by
    // UpdateObservationVectorSubgraphs(...) to avoid duplicate fixed-window paths.
    const float skewness = Subgraph_SkewnessIdx[sc.Index];
    const float vpin = Subgraph_AmihudIlliquidity[sc.Index];
    const float liqFragility = Subgraph_LiqFragility[sc.Index];
    const float microAsymmetry = Subgraph_MicroAsymmetry[sc.Index];

    // These two are not yet persisted by UpdateObservationVectorSubgraphs.
    // Keep them on the same adaptive window used by the canonical updater.
    const float meanRevZ = CalculateMeanReversionSpeed(sc, observation_window_n);
    const float volConvexity = CalculateVolConvexity(sc, observation_window_n);

    // Update Central Observation Store
    // Note: Mutating canonical ObservationData fields owned by Screen 3
    auto* obs = ContextManager::Instance().GetMutableObservation();
    if (obs) {
        obs->mutate_skewness_idx(skewness);
        obs->mutate_vpin_toxicity(vpin);
        obs->mutate_liq_fragility(liqFragility);
        obs->mutate_mean_rev_z(meanRevZ);
        obs->mutate_vol_convexity(volConvexity);       // Also Q1 for safety
        obs->mutate_micro_asymmetry(microAsymmetry);
    }
    if (climateIndicator) {
        currentClimate = climateIndicator->Value();
    }

    // Legacy logic regarding "isFragile" / "isChaos" moved to Indicator::UpdateContext
    // Variable 'isFragile' 'isChaos' removed from here as they are internal to the indicator now.

    // === PRO DUCT IVE: Kangaroo Tail (Raw Sensor) ===
    // Pattern Logic: Rejection of levels.
    // Filtering: None (Orchestrator handles HMM/Climate filtering)

    KangarooTailEnum tailEnum = DetectKangarooTail(
        sc.Open[sc.Index],
        sc.High[sc.Index],
        sc.Low[sc.Index],
        sc.Close[sc.Index],
        Subgraph_AtrTemp3[sc.Index],  // Reuse existing ATR calculation
        tailToBodyRatio,
        tailToATR,
        closePosition,
        qualityScore
    );

    // Check support/resistance context (data already loaded above)
    const float threshold = SUPPORT_RESISTANCE_THRESHOLD * Subgraph_AtrTemp3[sc.Index];
    const bool atSupportLevel = (prevDayLow > 0.0f && std::abs(sc.Low[sc.Index] - prevDayLow) <= threshold);
    const bool atResistanceLevel = (prevDayHigh > 0.0f && std::abs(sc.High[sc.Index] - prevDayHigh) <= threshold);

    // Update KangarooTail indicator in IndicatorManager
    const auto kangarooTail = indMgr.GetIndicator<KangarooTail>(IndicatorKey::KANGAROO_TAIL);
    if (kangarooTail) {
        kangarooTail->Update(tailEnum);
        kangarooTail->SetMetrics(tailToBodyRatio, tailToATR, closePosition, qualityScore);
        kangarooTail->SetContext(atSupportLevel, atResistanceLevel);
    }

    // Update RaschkeTacticalTrigger if high-quality tail detected
    // Note: raschkeTacticalIndicator already retrieved above, reuse it
    constexpr float KANGAROO_QUALITY_THRESHOLD = 0.6f;
    if (raschkeTacticalIndicator && qualityScore >= KANGAROO_QUALITY_THRESHOLD) {
        // Only trigger on STRONG or EXTREME tails with good context
        if (tailEnum == KangarooTailEnum::BULLISH_STRONG || tailEnum == KangarooTailEnum::BULLISH_EXTREME) {
            if (atSupportLevel) {  // Must be at support
                raschkeTacticalIndicator->Update(RaschkeTacticalTrigger::KANGAROO_TAIL_BUY);
            }
        }
        else if (tailEnum == KangarooTailEnum::BEARISH_STRONG || tailEnum == KangarooTailEnum::BEARISH_EXTREME) {
            if (atResistanceLevel) {  // Must be at resistance
                raschkeTacticalIndicator->Update(RaschkeTacticalTrigger::KANGAROO_TAIL_SELL);
            }
        }
    }

    // Visualize only high-quality patterns (STRONG/EXTREME with quality ≥threshold at key levels)
    // WEAK patterns are tracked for ML training but not visualized to reduce chart clutter
    if (qualityScore >= KANGAROO_QUALITY_THRESHOLD) {
        if ((tailEnum == KangarooTailEnum::BULLISH_STRONG || tailEnum == KangarooTailEnum::BULLISH_EXTREME) && atSupportLevel) {
            Subgraph_KangarooTailBullish[sc.Index] = sc.Low[sc.Index];  // Green dot at low
        }
        else if ((tailEnum == KangarooTailEnum::BEARISH_STRONG || tailEnum == KangarooTailEnum::BEARISH_EXTREME) && atResistanceLevel) {
            Subgraph_KangarooTailBearish[sc.Index] = sc.High[sc.Index];  // Red dot at high
        }
    }

    // --- Momentum Pinball Detection ---
    // Pattern: RSI3 crosses RSI10 + Stochastic extreme = early reversal signal
    // Filtering: None (Orchestrator handles HMM/Climate filtering)

    if (sc.Index >= 1) {  // Need previous bar for cross detection
        float rsiDelta = 0.0f;
        float stochDepth = 0.0f;
        bool impulseJustChanged = false;
        float volumeSpike = 0.0f;
        float pinballQuality = 0.0f;

        // Get current Impulse color
        int currentImpulseColor = 0;  // 0 = BLUE (neutral)
        const auto impulseIndicator = indMgr.GetIndicator<Impulse>(IndicatorKey::INTERM_IMP);
        if (impulseIndicator) {
            const ImpulseEnum impulseEnum = impulseIndicator->Value();
            if (impulseEnum == ImpulseEnum::GREEN) currentImpulseColor = 1;
            else if (impulseEnum == ImpulseEnum::RED) currentImpulseColor = -1;
        }

        // Get previous Impulse color from stored subgraph
        const int prevImpulseColor = static_cast<int>(Subgraph_PrevImpulseColor[sc.Index - 1]);

        // Detect MomentumPinball pattern ("The Gang" Physics Overlay)
        MomentumPinballEnum pinballEnum = MomentumPinballEnum::NONE;

        pinballEnum = DetectMomentumPinball(
            Subgraph_RSI3[sc.Index],           // current RSI3
            Subgraph_RSI10[sc.Index],          // current RSI10
            Subgraph_PrevRSI3[sc.Index - 1],   // previous RSI3
            Subgraph_PrevRSI10[sc.Index - 1],  // previous RSI10
            Subgraph_StochK[sc.Index],         // current Stochastic %K
            currentImpulseColor,               // current Impulse color
            prevImpulseColor,                  // previous Impulse color
            sc.Volume[sc.Index],               // current volume
            Subgraph_AvgVolume[sc.Index],      // average volume
            rsiDelta,                          // OUT: RSI3 - RSI10
            stochDepth,                        // OUT: Stochastic value
            impulseJustChanged,                // OUT: Impulse changed?
            volumeSpike,                       // OUT: Volume / AvgVolume
            pinballQuality                     // OUT: 0.0-1.0 quality
        );

        // TS3 alignment refinement: trend-pattern quality depends on persistence and
        // local slope coherence. Keep this as quality shaping (not hard veto).
        if (pinballEnum != MomentumPinballEnum::NONE && sc.Index >= 1) {
            float ts3Hurst = Subgraph_HurstExponent[sc.Index];
            if (!std::isfinite(ts3Hurst) || ts3Hurst <= 0.0f) {
                ts3Hurst = 0.5f;  // Neutral persistence fallback
            }

            const float slope = sc.Close[sc.Index] - sc.Close[sc.Index - 1];
            const bool bullishSignal =
                (pinballEnum == MomentumPinballEnum::BULLISH_WEAK ||
                 pinballEnum == MomentumPinballEnum::BULLISH_STRONG ||
                 pinballEnum == MomentumPinballEnum::BULLISH_EXTREME);
            const bool bearishSignal =
                (pinballEnum == MomentumPinballEnum::BEARISH_WEAK ||
                 pinballEnum == MomentumPinballEnum::BEARISH_STRONG ||
                 pinballEnum == MomentumPinballEnum::BEARISH_EXTREME);
            const bool slopeAligned =
                (bullishSignal && slope > 0.0f) || (bearishSignal && slope < 0.0f);

            float alignmentMult = 1.0f;
            if (ts3Hurst >= 0.65f && slopeAligned) {
                alignmentMult = 1.10f;
            } else if (ts3Hurst >= 0.70f && !slopeAligned) {
                alignmentMult = 0.75f;
            } else if (ts3Hurst < 0.55f) {
                alignmentMult = 0.85f;
            }

            pinballQuality *= alignmentMult;
        }

        // Update MomentumPinball indicator in IndicatorManager
        const auto pinballIndicator = indMgr.GetIndicator<MomentumPinball>(IndicatorKey::MOMENTUM_PINBALL);
        if (pinballIndicator) {
            pinballQuality = std::min(1.0f, std::max(0.0f, pinballQuality));
            pinballIndicator->Update(pinballEnum);
            pinballIndicator->SetMetrics(rsiDelta, stochDepth, impulseJustChanged, volumeSpike, pinballQuality);
        }

        // Store current values for next bar
        Subgraph_PrevRSI3[sc.Index] = Subgraph_RSI3[sc.Index];
        Subgraph_PrevRSI10[sc.Index] = Subgraph_RSI10[sc.Index];
        Subgraph_PrevImpulseColor[sc.Index] = static_cast<float>(currentImpulseColor);

        // Visualize non-neutral raw patterns.
        if (pinballEnum != MomentumPinballEnum::NONE) {
            // Logic removed: Context bonus calculations (MACD, FI2, HMM)
            // Logic removed: RaschkeTacticalTrigger updates (moved to Python/Orchestrator)

            // Visualize raw patterns based on inherent quality
            if (pinballQuality >= 0.5f) { // Lowered threshold for raw sensor visibility
                if (pinballEnum == MomentumPinballEnum::BULLISH_STRONG ||
                    pinballEnum == MomentumPinballEnum::BULLISH_EXTREME) {
                    Subgraph_MomentumPinballBullish[sc.Index] = sc.Low[sc.Index];
                }
                else if (pinballEnum == MomentumPinballEnum::BEARISH_STRONG ||
                         pinballEnum == MomentumPinballEnum::BEARISH_EXTREME) {
                    Subgraph_MomentumPinballBearish[sc.Index] = sc.High[sc.Index];
                }
            }
        }
    }

    // --- Elder Breakout Detection (Keltner Channel breakout = volatility expansion) ---
    // Pattern: Close beyond Keltner band after consolidation
    // Reuses Keltner bands already calculated above (Subgraph_TopBand, Subgraph_BottomBand, Subgraph_AtrTemp4)
    if (sc.Index >= ELDER_CONSOLIDATION_LOOKBACK) {
        float breakoutDistance = 0.0f;
        float hurstValue = 0.0f;
        float breakoutVolumeSpike = 0.0f;
        int consolidationBars = 0;
        bool isGap = false;
        float breakoutQuality = 0.0f;

        // Use Hurst exponent for trend persistence (replaces ADX).
        float currentHurst = Subgraph_HurstExponent[sc.Index];
        if (currentHurst <= 0.0f) {
            currentHurst = 0.50f;  // Neutral persistence default
        }

        // Detect consolidation: Count bars where close stayed near bands
        // For simplicity, use heuristic: if previous 5 bars had close within 1× ATR of upper/lower band
        float atr = Subgraph_AtrTemp3[sc.Index];
        float upperBand = Subgraph_TopBand[sc.Index];
        float lowerBand = Subgraph_BottomBand[sc.Index];
        float close = sc.Close[sc.Index];

        // Simple consolidation detection (last 5 bars near current band)
        if (atr > 0.0f) [[likely]] {
            const int startIdx = sc.Index - ELDER_CONSOLIDATION_LOOKBACK;

            // Count bars near upper/lower band with straight loops (no closure overhead)
            int nearUpperCount = 0;
            int nearLowerCount = 0;
            for (int idx = startIdx + 1; idx <= sc.Index; ++idx) {
                const float prevATR = Subgraph_AtrTemp3[idx];
                if (prevATR > 0.0f) {
                    if (std::abs(sc.Close[idx] - Subgraph_TopBand[idx]) <= prevATR)
                        ++nearUpperCount;
                    if (std::abs(sc.Close[idx] - Subgraph_BottomBand[idx]) <= prevATR)
                        ++nearLowerCount;
                }
            }

            // If close was near upper band for 3+ bars, that's consolidation at resistance
            if (nearUpperCount >= ELDER_MIN_CONSOLIDATION_BARS) {
                consolidationBars = nearUpperCount;
            }
            // If close was near lower band for 3+ bars, that's consolidation at support
            else if (nearLowerCount >= ELDER_MIN_CONSOLIDATION_BARS) {
                consolidationBars = nearLowerCount;
            }
        }

        // Detect gap: if open is beyond band (not just close)
        const float open = sc.Open[sc.Index];
        isGap = (open > upperBand || open < lowerBand);

        // Detect Elder Breakout pattern (Elite Physics Overlay Applied)
        ElderBreakoutEnum breakoutEnum = ElderBreakoutEnum::NONE;

        // Climate Filter:
        // Logic removed - Orchestrator/Python is decision authority

        bool allowBreakout = true;

        if (allowBreakout) {
             breakoutEnum = DetectElderBreakout(
                close, upperBand, lowerBand,
                atr, currentHurst,
                sc.Volume[sc.Index], Subgraph_AvgVolume[sc.Index],
                consolidationBars, isGap,
                breakoutDistance, hurstValue, breakoutVolumeSpike,
                consolidationBars, isGap, breakoutQuality
            );

            // --- ELITE ADAPTATION: Breakout Physics ---
            // Logic removed - Orchestrator/Python handles climate multipliers

            // Clip quality to [0.0, 1.0]
            breakoutQuality = std::min(1.0f, std::max(0.0f, breakoutQuality));

            // TS3 alignment refinement for trend breakouts:
            // reward persistent + aligned slope, penalize persistence divergence.
            if (breakoutEnum != ElderBreakoutEnum::NONE && sc.Index >= 1) {
                const float slope = sc.Close[sc.Index] - sc.Close[sc.Index - 1];
                const bool bullishBreakout =
                    (breakoutEnum == ElderBreakoutEnum::BULLISH_WEAK ||
                     breakoutEnum == ElderBreakoutEnum::BULLISH_STRONG ||
                     breakoutEnum == ElderBreakoutEnum::BULLISH_EXTREME);
                const bool bearishBreakout =
                    (breakoutEnum == ElderBreakoutEnum::BEARISH_WEAK ||
                     breakoutEnum == ElderBreakoutEnum::BEARISH_STRONG ||
                     breakoutEnum == ElderBreakoutEnum::BEARISH_EXTREME);
                const bool slopeAligned =
                    (bullishBreakout && slope > 0.0f) || (bearishBreakout && slope < 0.0f);

                float alignmentMult = 1.0f;
                if (currentHurst >= 0.65f && slopeAligned) {
                    alignmentMult = 1.12f;
                } else if (currentHurst >= 0.70f && !slopeAligned) {
                    alignmentMult = 0.70f;
                } else if (currentHurst < 0.55f) {
                    alignmentMult = 0.80f;
                }

                breakoutQuality *= alignmentMult;
                breakoutQuality = std::min(1.0f, std::max(0.0f, breakoutQuality));
            }

        } else {
             // Log suppression if needed
             // "Breakout suppressed by Mean Reversion Regime (Hurst < 0.4)"
        }

        // Store current ATR for next bar (squeeze detection)
        Subgraph_PrevATR[sc.Index] = atr;

        // Update ElderBreakout indicator with metrics
        const auto elderBreakoutIndicator = indMgr.GetIndicator<ElderBreakout>(IndicatorKey::ELDER_BREAKOUT);
        if (elderBreakoutIndicator && breakoutEnum != ElderBreakoutEnum::NONE) {
            elderBreakoutIndicator->Update(breakoutEnum);
            // Note: SetMetrics() called later with enhanced quality after context calculation

            // Context 1: Channel Squeeze (+0.2)
            // ATR declining = bands narrowing = compression before expansion
            bool channelSqueeze = false;
            if (sc.Index >= 5) {
                float avgPrevATR = 0.0f;
                for (int i = 1; i <= 5; i++) {
                    avgPrevATR += Subgraph_AtrTemp3[sc.Index - i];
                }
                avgPrevATR /= 5.0f;

                if (atr < avgPrevATR * 0.9f) {  // ATR declined 10%+ = squeeze
                    channelSqueeze = true;
                }
            }

            // Context 2: Impulse Aligned (+0.1)
            bool impulseAligned = false;
            auto impulseIndicator = indMgr.GetIndicator<Impulse>(IndicatorKey::INTERM_IMP);
            if (impulseIndicator) {
                ImpulseEnum impulseEnum = impulseIndicator->Value();
                bool impulseGreen = (impulseEnum == ImpulseEnum::GREEN);
                bool impulseRed = (impulseEnum == ImpulseEnum::RED);

                if ((breakoutEnum == ElderBreakoutEnum::BULLISH_WEAK ||
                     breakoutEnum == ElderBreakoutEnum::BULLISH_STRONG ||
                     breakoutEnum == ElderBreakoutEnum::BULLISH_EXTREME) && impulseGreen) {
                    impulseAligned = true;
                }
                else if ((breakoutEnum == ElderBreakoutEnum::BEARISH_WEAK ||
                          breakoutEnum == ElderBreakoutEnum::BEARISH_STRONG ||
                          breakoutEnum == ElderBreakoutEnum::BEARISH_EXTREME) && impulseRed) {
                    impulseAligned = true;
                }
            }

            // Context 3: HMM State Alignment (reuses cached hmmIndicator from climate assessment)
            bool screenAligned = false;
            if (hmmIndicator) {
                // HMM states are non-directional; momentum regimes support breakouts in either direction.
                bool screen1Bullish = (currentHmmState == HMMStateEnum::GAUSSIAN_STABLE || currentHmmState == HMMStateEnum::PARETO_MOMENTUM);
                bool screen1Bearish = (currentHmmState == HMMStateEnum::GAUSSIAN_STABLE || currentHmmState == HMMStateEnum::PARETO_MOMENTUM);

                if ((breakoutEnum == ElderBreakoutEnum::BULLISH_WEAK ||
                     breakoutEnum == ElderBreakoutEnum::BULLISH_STRONG ||
                     breakoutEnum == ElderBreakoutEnum::BULLISH_EXTREME) && screen1Bullish) {
                    screenAligned = true;
                }
                else if ((breakoutEnum == ElderBreakoutEnum::BEARISH_WEAK ||
                          breakoutEnum == ElderBreakoutEnum::BEARISH_STRONG ||
                          breakoutEnum == ElderBreakoutEnum::BEARISH_EXTREME) && screen1Bearish) {
                    screenAligned = true;
                }
            }

            // Update context in indicator (Pure Data - No Scoring)
            elderBreakoutIndicator->SetContext(channelSqueeze, impulseAligned, screenAligned);

            // Update indicator with raw quality score (Physics only)
            elderBreakoutIndicator->SetMetrics(breakoutDistance, hurstValue, breakoutVolumeSpike, consolidationBars, isGap, breakoutQuality);

            // Forward ALL breakouts to RaschkeTacticalTrigger
            // The Orchestrator/Scoring engine will filter them based on HMM/Climate/Score.
            if (raschkeTacticalIndicator) {
                if (breakoutEnum == ElderBreakoutEnum::BULLISH_STRONG ||
                    breakoutEnum == ElderBreakoutEnum::BULLISH_EXTREME) {
                    raschkeTacticalIndicator->Update(RaschkeTacticalTrigger::ELDER_BREAKOUT_BUY);
                }
                else if (breakoutEnum == ElderBreakoutEnum::BEARISH_STRONG ||
                         breakoutEnum == ElderBreakoutEnum::BEARISH_EXTREME) {
                    raschkeTacticalIndicator->Update(RaschkeTacticalTrigger::ELDER_BREAKOUT_SELL);
                }
            }

            // Visualize high-quality patterns (Physics Check only)
            // Visualization threshold remains to reduce chart clutter, but logic is decoupled
            if (breakoutQuality >= 0.5f) {
                if (breakoutEnum == ElderBreakoutEnum::BULLISH_STRONG ||
                    breakoutEnum == ElderBreakoutEnum::BULLISH_EXTREME) {
                    Subgraph_ElderBreakoutBullish[sc.Index] = close;  // Triangle at breakout close
                }
                else if (breakoutEnum == ElderBreakoutEnum::BEARISH_STRONG ||
                         breakoutEnum == ElderBreakoutEnum::BEARISH_EXTREME) {
                    Subgraph_ElderBreakoutBearish[sc.Index] = close;  // Triangle at breakout close
                }
            }
        }
    }  // End if (sc.Index >= ELDER_CONSOLIDATION_LOOKBACK)

    // ============================================================================
    // TURTLE SOUP DETECTION (Consolidated from scsf_Screen3_TurtleSoup)
    // ============================================================================

    // Institutional timing contract:
    // - Process ONCE per closed bar
    // - Evaluate the just-closed bar against prior-window extremes (exclude signal bar)

    constexpr int TURTLE_SOUP_LENGTH = 20;
    constexpr int TURTLE_SOUP_MIN_SEPARATION = 4;

    // Use persistent variable to track last processed bar index
    // Reuse specific ID or just use a unique integer for this study instance
    int& lastProcessedBarTS = sc.GetPersistentInt(101); // 101 for Turtle Soup Logic

    const int signalBarIndex = sc.Index - 1;

    // Only process if we have a closed bar and haven't processed it yet
    // Note: In AutoLoop during history, sc.Index iterates.
    // For live updates, we want to run this check on every tick but only EXECUTE logic once per closed bar.

    bool runTurtleSoup = false;
    if (signalBarIndex >= TURTLE_SOUP_LENGTH &&
        sc.GetBarHasClosedStatus(signalBarIndex) == BHCS_BAR_HAS_CLOSED &&
        lastProcessedBarTS != signalBarIndex) {
        runTurtleSoup = true;
    }

    if (runTurtleSoup) {
        lastProcessedBarTS = signalBarIndex;

        // Retrieve context variables
        float atr = Subgraph_AtrTemp3[signalBarIndex];

        // Turtle Soup must use PREVIOUS lookback window (exclude signal bar):
        // false breakout requires signal bar to break prior extreme, then close back inside.
        const int prevWindowLastIndex = signalBarIndex - 1;

        // Calculate prior window extremes manually since Subgraph_HighestHigh20Period includes sc.Index
        // Subgraph_HighestHigh20Period[i] covers [i-19, i]
        // We need [prevWindowLastIndex-19, prevWindowLastIndex] which is exactly Subgraph_HighestHigh20Period[prevWindowLastIndex]

        const float prevHighest = Subgraph_HighestHigh20Period[prevWindowLastIndex];
        const float prevLowest = Subgraph_LowestLow20Period[prevWindowLastIndex];

        // Define signal bar components
        const float high = sc.High[signalBarIndex];
        const float low = sc.Low[signalBarIndex];
        const float close = sc.Close[signalBarIndex];
        const float open = sc.Open[signalBarIndex];

        // Calculate ADX for regime context
        const float hurst = Subgraph_HurstExponent[signalBarIndex];

        // Export prior-window extremes to training data (used by Turtle Soup)
        if (shortPriceAction) {
            shortPriceAction->SetPrevFourBarExtremes(prevHighest, prevLowest);
        }

        // Detect Turtle Soup pattern (Raw Sensor)
        float penetrationDistance = 0.0f;
        float closeDistance = 0.0f;
        float closePosition = 0.5f;
        float qualityScore = 0.0f;

        TurtleSoupEnum soupEnum = DetectTurtleSoup(
            open, high, low, close,
            prevHighest,  // prior-window high
            prevLowest,   // prior-window low
            atr,
            penetrationDistance,
            closeDistance,
            closePosition,
            qualityScore
        );

        // Logic removed: HMM/MarketClimate filtering and quality adjustment
        // Orchestrator/Python handles regime-based filtering

        // Enforce Street Smarts separation guidance (3-4+ bars separation)
        int priorHighIndex = prevWindowLastIndex;
        int priorLowIndex = prevWindowLastIndex;

        { // Scope for loop variables
           float scanHigh = -FLT_MAX;
           float scanLow = FLT_MAX;
           // Recalculate priorHighIndex/priorLowIndex
           for (int i = prevWindowLastIndex - 1; i >= prevWindowLastIndex - TURTLE_SOUP_LENGTH + 1; --i) {
                float h = sc.High[i];
                float l = sc.Low[i];
                if (h >= scanHigh) { scanHigh = h; priorHighIndex = i; }
                if (l <= scanLow) { scanLow = l; priorLowIndex = i; }
           }
        }

        const int barsSincePriorHigh = signalBarIndex - priorHighIndex;
        const int barsSincePriorLow = signalBarIndex - priorLowIndex;

        // Apply separation filter
        if ((soupEnum == TurtleSoupEnum::BULLISH_WEAK ||
             soupEnum == TurtleSoupEnum::BULLISH_STRONG ||
             soupEnum == TurtleSoupEnum::BULLISH_EXTREME) && barsSincePriorLow < TURTLE_SOUP_MIN_SEPARATION) {
            soupEnum = TurtleSoupEnum::NONE;
        }

        if ((soupEnum == TurtleSoupEnum::BEARISH_WEAK ||
             soupEnum == TurtleSoupEnum::BEARISH_STRONG ||
             soupEnum == TurtleSoupEnum::BEARISH_EXTREME) && barsSincePriorHigh < TURTLE_SOUP_MIN_SEPARATION) {
            soupEnum = TurtleSoupEnum::NONE;
        }

        // Calculate daily levels context early
        float threshold = 0.5f * atr;
        bool atDailyHigh = (prevDayHigh > 0.0f && std::abs(prevHighest - prevDayHigh) <= threshold);
        bool atDailyLow = (prevDayLow > 0.0f && std::abs(prevLowest - prevDayLow) <= threshold);

        // Update TurtleSoup indicator
        auto soupIndicator = indMgr.GetIndicator<TurtleSoup>(IndicatorKey::TURTLE_SOUP);
        if (soupIndicator) {
            soupIndicator->Update(soupEnum);
            if (soupEnum != TurtleSoupEnum::NONE) {
                soupIndicator->SetMetrics(penetrationDistance, closeDistance, closePosition, qualityScore);
            }

            // Context: HMM State Alignment (reuses cached hmmIndicator from climate assessment)
            bool screenAligned = false;
            if (hmmIndicator) {
                if ((soupEnum == TurtleSoupEnum::BULLISH_STRONG || soupEnum == TurtleSoupEnum::BULLISH_EXTREME) &&
                    (currentHmmState == HMMStateEnum::GAUSSIAN_STABLE || currentHmmState == HMMStateEnum::PARETO_MOMENTUM)) {
                    screenAligned = true;
                }
                else if ((soupEnum == TurtleSoupEnum::BEARISH_STRONG || soupEnum == TurtleSoupEnum::BEARISH_EXTREME) &&
                         (currentHmmState == HMMStateEnum::GAUSSIAN_FRAGILE || currentHmmState == HMMStateEnum::COILED_SPRING)) {
                    screenAligned = true;
                }
            }

            soupIndicator->SetContext(atDailyHigh, atDailyLow, hurst, screenAligned);

            // Forward RAW quality score (Physics only)
            soupIndicator->SetMetrics(penetrationDistance, closeDistance, closePosition, qualityScore);

             // Forward ALL Turtle Soup signals to RaschkeTacticalTrigger
             // Orchestrator/Scoring engine applies filters.
            if (raschkeTacticalIndicator) {
                if ((soupEnum == TurtleSoupEnum::BULLISH_STRONG || soupEnum == TurtleSoupEnum::BULLISH_EXTREME) && atDailyLow) {
                    raschkeTacticalIndicator->Update(RaschkeTacticalTrigger::TURTLE_SOUP_BUY);
                }
                else if ((soupEnum == TurtleSoupEnum::BEARISH_STRONG || soupEnum == TurtleSoupEnum::BEARISH_EXTREME) && atDailyHigh) {
                    raschkeTacticalIndicator->Update(RaschkeTacticalTrigger::TURTLE_SOUP_SELL);
                }
            }
        }
    } // End runTurtleSoup

    // === CONTEXT MANAGER: Register normalized anchors (CRITICAL FIX - MOVED FROM TURTLE SOUP) ===
    // This ensures physics engine runs even if Turtle Soup logic didn't trigger a signal,
    // but relies on signalBarIndex >= 3

    if (signalBarIndex >= 3) {
        NormalizedAnchors anchors;
        float last = static_cast<float>(sc.Close[signalBarIndex]);
        float atr_norm = Subgraph_AtrTemp3[signalBarIndex];
        if (atr_norm < 0.01f) atr_norm = 1.0f;

        // Daily levels
        anchors.distDayHigh = (prevDayHigh > 0.0f) ? ((last - prevDayHigh) / atr_norm) : 0.0f;
        anchors.distDayLow = (prevDayLow > 0.0f) ? ((last - prevDayLow) / atr_norm) : 0.0f;

        // 4-bar swing extremes
        float fourBarHigh = sc.High[signalBarIndex];
        float fourBarLow = sc.Low[signalBarIndex];
        for (int i = 1; i <= 3; ++i) {
            if (signalBarIndex >= i) {
                fourBarHigh = std::max(fourBarHigh, static_cast<float>(sc.High[signalBarIndex - i]));
                fourBarLow = std::min(fourBarLow, static_cast<float>(sc.Low[signalBarIndex - i]));
            }
        }
        anchors.distFourBarHigh = (last - fourBarHigh) / atr_norm;
        anchors.distFourBarLow = (last - fourBarLow) / atr_norm;

        // EMA13 distance
        sc.ExponentialMovAvg(sc.Close, Subgraph_EMA13Anchor, 13);
        float ema13 = Subgraph_EMA13Anchor[signalBarIndex];
        anchors.distEma13 = (ema13 > 0.0f) ? ((last - ema13) / atr_norm) : 0.0f;

        float volumeSMA21 = Subgraph_VolumeSma[signalBarIndex];
        float currentVolume = static_cast<float>(sc.Volume[signalBarIndex]);
        if (volumeSMA21 > 0.0f && currentVolume > 0.0f) {
            anchors.logRelativeVolume = std::log(currentVolume / volumeSMA21);
        } else {
            anchors.logRelativeVolume = 0.0f;
        }

        // Pack Observation Vector from calculated subgraphs
        anchors.pathEfficiencySNR = Subgraph_PathEfficiencySNR[signalBarIndex];
        anchors.hurstExponent = Subgraph_HurstExponent[signalBarIndex];
        anchors.microAsymmetry = Subgraph_MicroAsymmetry[signalBarIndex];
        anchors.realizedKurtosis = Subgraph_RealizedKurtosis[signalBarIndex];
        anchors.skewnessIdx = Subgraph_SkewnessIdx[signalBarIndex];
        anchors.vpin = Subgraph_AmihudIlliquidity[signalBarIndex];
        anchors.spreadStress = Subgraph_LiqFragility[signalBarIndex];

        const auto nhNlIndicator = indMgr.GetIndicator<NhNlSignalIndicator>(IndicatorKey::NH_NL_SIGNAL);
        anchors.nhNlDaily = nhNlIndicator ? nhNlIndicator->GetDailyValue() : 0.0f;

        const auto dailyBiasInd = indMgr.GetIndicator<DailyBiasIndicator>(IndicatorKey::DAILY_BIAS);
        anchors.dailyBias = dailyBiasInd ? static_cast<float>(dailyBiasInd->Value()) : 0.0f;

        anchors.lastUpdated = sc.BaseDateTimeIn[signalBarIndex];

        constexpr float ANCHOR_ATR_CLAMP = 15.0f;
        anchors.distDayHigh = std::clamp(anchors.distDayHigh, -ANCHOR_ATR_CLAMP, ANCHOR_ATR_CLAMP);
        anchors.distDayLow = std::clamp(anchors.distDayLow, -ANCHOR_ATR_CLAMP, ANCHOR_ATR_CLAMP);
        anchors.distFourBarHigh = std::clamp(anchors.distFourBarHigh, -ANCHOR_ATR_CLAMP, ANCHOR_ATR_CLAMP);
        anchors.distFourBarLow = std::clamp(anchors.distFourBarLow, -ANCHOR_ATR_CLAMP, ANCHOR_ATR_CLAMP);
        anchors.distEma13 = std::clamp(anchors.distEma13, -ANCHOR_ATR_CLAMP, ANCHOR_ATR_CLAMP);

        ContextManager::Instance().SetNormalizedAnchors(std::move(anchors));
    }



    // ============================================================================
    // NR7 (Narrow Range 7) Detection - Linda Raschke compression pattern
    // Simplified implementation based on Sierra Chart's scsf_NarrowRangeBar
    // ============================================================================

    // Detect NR7 compression bar (narrowest range in 7 bars)
    // Then watch for breakout on subsequent bars
    constexpr int NR7_LOOKBACK = 7;

    if (sc.Index >= NR7_LOOKBACK) {
        // Calculate current bar's range
        const float currentRange = sc.High[sc.Index] - sc.Low[sc.Index];

        // Check if current bar has the narrowest range in last 7 bars
        bool isNR7 = true;

        // === ELITE PHYSICS FILTER: NR7 (Mean Reversion/Compression) ===
        // Pattern: Volatility Compression -> Expansion.
        // Climate Filter:
        // - SHANNON_CHAOS: BLOCK (Range contraction in noise is meaningless).
        // - COILED_SPRING: BOOST (Highest probability setup).
        // - PARETO_MOMENTUM: ALLOW (Continuation pattern).
        // - TALEBIAN_FRAGILE: ALLOW (Explosive potential).

        bool allowNR7 = true;

        if (currentClimate == MarketClimate::SHANNON_CHAOS) {
             allowNR7 = false;
        }

        if (allowNR7) {
            for (int i = 1; i <= NR7_LOOKBACK; i++) {
                const float priorRange = sc.High[sc.Index - i] - sc.Low[sc.Index - i];

                // If any prior bar has a smaller or equal range, current bar is NOT NR7
                if (!sc.FormattedEvaluate(priorRange, sc.BaseGraphValueFormat, GREATER_OPERATOR, currentRange, sc.BaseGraphValueFormat)) {
                    isNR7 = false;
                    break;
                }
            }
        } else {
            isNR7 = false; // Blocked by Physics
        }

        if (isNR7) {
            auto* const nr7Indicator = indMgr.GetIndicator<NR7>(IndicatorKey::NR7);
            if (nr7Indicator) {
                // Calculate basic metrics for indicator
                float avg7BarRange = 0.0f;
                for (int i = 0; i < NR7_LOOKBACK; i++) {
                    avg7BarRange += sc.High[sc.Index - i] - sc.Low[sc.Index - i];
                }
                avg7BarRange /= NR7_LOOKBACK;

                const float rangePercentile = (avg7BarRange > 0.0f) ? (currentRange / avg7BarRange) : 1.0f;

                // Volume spike calculation — reuse precomputed Subgraph_AvgVolume (20-period SMA)
                const float avgVolume = Subgraph_AvgVolume[sc.Index];
                const float volumeSpike = (avgVolume > 0.0f) ? (static_cast<float>(sc.Volume[sc.Index]) / avgVolume) : 1.0f;

                // Simple quality score: lower range percentile + lower volume = better compression
                const float qualityScore = (1.0f - rangePercentile) * 0.7f + (volumeSpike < 0.8f ? 0.3f : 0.0f);

                // Update indicator with STRONG classification (simplified from WEAK/STRONG/EXTREME)
                nr7Indicator->Update(NR7Enum::STRONG);
                nr7Indicator->SetMetrics(currentRange, avg7BarRange, rangePercentile, volumeSpike, 0, qualityScore);

                // Calculate context (same factors as other patterns)
                bool channelSqueeze = false;
                bool impulseAligned = false;
                bool screenAligned = false;

                // Context: Impulse alignment
                auto* impulseIndicator = indMgr.GetIndicator<Impulse>(IndicatorKey::INTERM_IMP);
                if (impulseIndicator) {
                    ImpulseEnum impulseState = impulseIndicator->Value();
                    impulseAligned = (impulseState == ImpulseEnum::GREEN || impulseState == ImpulseEnum::RED);
                }

                        // Context: HMM state alignment (reuses cached hmmIndicator)
                if (hmmIndicator) {
                    screenAligned = (currentHmmState == HMMStateEnum::GAUSSIAN_STABLE ||
                                    currentHmmState == HMMStateEnum::PARETO_MOMENTUM ||
                                    currentHmmState == HMMStateEnum::COILED_SPRING ||
                                    currentHmmState == HMMStateEnum::GAUSSIAN_FRAGILE);
                }

                nr7Indicator->SetContext(channelSqueeze, impulseAligned, screenAligned);

                // Visualize NR7 compression with green dot at bar low
                Subgraph_NR7Marker[sc.Index] = sc.Low[sc.Index];
            }
        }

        // Check for breakout AFTER NR7 bar (next 1-3 bars)
        // Look back to see if we had NR7 in last 3 bars, then check for breakout
        for (int lookback = 1; lookback <= 3; lookback++) {
            const int nr7BarIdx = sc.Index - lookback;

            if (nr7BarIdx < 0) continue;  // Safety check

            // Check if that bar was marked as NR7
            if (Subgraph_NR7Marker[nr7BarIdx] > 0.0f) {
                const float nr7High = sc.High[nr7BarIdx];
                const float nr7Low = sc.Low[nr7BarIdx];
                const float breakoutClose = sc.Close[sc.Index];

                // Bullish breakout: close above NR7 high
                if (breakoutClose > nr7High) {
                    Subgraph_NR7BreakoutBullish[sc.Index] = breakoutClose;

                    if (raschkeTacticalIndicator) {
                        raschkeTacticalIndicator->Update(RaschkeTacticalTrigger::NR7_BREAKOUT_BUY);
                    }
                    break;  // Only mark first breakout
                }
                // Bearish breakout: close below NR7 low
                else if (breakoutClose < nr7Low) {
                    Subgraph_NR7BreakoutBearish[sc.Index] = breakoutClose;

                    if (raschkeTacticalIndicator) {
                        raschkeTacticalIndicator->Update(RaschkeTacticalTrigger::NR7_BREAKOUT_SELL);
                    }
                    break;  // Only mark first breakout
                }
            }
        }
    }

    // ============================================================================
    // 3-10 OSCILLATOR CALCULATION (Raschke - actually 3-16 EMA difference)
    // Used for overnight momentum failure detection during Globex session
    // ============================================================================

    // Use sc.MACD to calculate EMA(3) - EMA(16) and SMA(16) signal line
    // Subgraph_EMA3 = MACD line (fast line), Subgraph_EMA16 = Signal line (slow line)
    sc.MACD(sc.Close, Subgraph_EMA3, sc.Index, 3, 16, 16, MOVAVGTYPE_EXPONENTIAL);

    const float fastLine = Subgraph_EMA3[sc.Index];
    const float slowLine = Subgraph_EMA3.Arrays[1][sc.Index];  // Signal line from MACD

    // Update IndicatorManager with oscillator values
    auto* osc310 = indMgr.GetIndicator<Oscillator310>(IndicatorKey::OSCILLATOR_310);
    if (osc310) {
        osc310->UpdateOscillator(fastLine, slowLine);
    }

    // Legacy variable for overnight exit logic (use indicator values for consistency)
    const float threeLineOsc = fastLine;
    const float threeLineOscPrev = osc310 ? osc310->PrevFastLine() : 0.0f;

    // ============================================================================
    // OVERNIGHT EXIT EVALUATION (Next Morning Logic)
    // Only evaluate during Globex or early RTH if holding overnight position
    // ============================================================================

    // Check if we have an overnight position
    const TradeSideEnum tradeSide = PositionManager::Instance().GetTradeSide();
    const bool hasOvernightPosition = (tradeSide != TradeSideEnum::FLAT);

    if (hasOvernightPosition) {
        // Get current time classification
        const auto timeOfDayIndicator = indMgr.GetIndicator<TimeOfDayIndicator>(IndicatorKey::TIME_OF_DAY);
        TimeOfDayEnum timeWindow = timeOfDayIndicator ? timeOfDayIndicator->Value() : TimeOfDayEnum::OVERNIGHT_HOLD;

        // Only evaluate during Globex and early RTH (not during regular trading hours)
        const bool isGlobexOrEarlyRTH = (
            timeWindow == TimeOfDayEnum::ASIAN_SESSION ||
            timeWindow == TimeOfDayEnum::LONDON_WINDOW ||
            timeWindow == TimeOfDayEnum::LONDON_TO_PREMARKET ||
            timeWindow == TimeOfDayEnum::PRE_MARKET_HOOK ||
            timeWindow == TimeOfDayEnum::PRE_MARKET ||
            timeWindow == TimeOfDayEnum::OPENING_HOUR
        );

        if (isGlobexOrEarlyRTH) {
            const Trade& openTrade = PositionManager::Instance().GetOpenTrade();
            const float overnightEntry = static_cast<float>(openTrade.GetEntryPrice());

            // Get previous day's high/low (Taylor objective points)
            const SCDateTime previousDay = sc.BaseDateTimeIn[sc.Index] - 1.0;
            const DailyHighLowData dailyData = DailyHighLowLoader::Instance().GetDataForDate(previousDay);
            const float prevDayHigh = static_cast<float>(dailyData.prevDayHigh);
            const float prevDayLow = static_cast<float>(dailyData.prevDayLow);

            // Get session open price (9:30 AM RTH open)
            // For now, approximate as first bar of OPENING_HOUR window
            const float openPrice = sc.Open[sc.Index];  // Simplified - should cache actual 9:30 open
            const float currentPrice = sc.Close[sc.Index];

            const bool isLong = (tradeSide == TradeSideEnum::LONG);

            // Evaluate overnight exit type
            auto overnightExitIndicator = indMgr.GetIndicator<OvernightExitIndicator>(IndicatorKey::OVERNIGHT_EXIT);
            if (overnightExitIndicator) {
                overnightExitIndicator->SetFromOvernightContext(
                    overnightEntry,
                    prevDayHigh,
                    prevDayLow,
                    openPrice,
                    currentPrice,
                    isLong,
                    threeLineOsc,
                    threeLineOscPrev
                );

                // The exit type is now available for TradeExecutionServer to act upon
                // OvernightExitTypeEnum exitType = overnightExitIndicator->Value();
                // TradeExecutionServer will check this and execute appropriate exit
            }
        }
    }
}

// scsf_Screen3_TurtleSoup removed — logic consolidated into scsf_Screen3_KeltnerChannel.
