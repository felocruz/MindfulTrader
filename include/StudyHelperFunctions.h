#pragma once

#include "sierrachart.h"
#include "Indicator.h"

// This function determines the impulse color based on two sets of data.
// It is intended to be used by the Impulse studies.
int GetImpulse(float maDiff, float macdDiff);


RaschkeStrategySetup DetectRaschkeStrategySetup(SCStudyInterfaceRef sc, float hurst, float ema21);
RaschkeTacticalTrigger DetectRaschkeTacticalTrigger(SCStudyInterfaceRef sc, float rsi3, float rsi10, float stochK);
DailyBiasEnum CalculateDailyBias(float lastPrice, float prevDayHigh, float prevDayLow, float hurstExponent, float entropy,
                                  float valueAreaLow = 0.0f, float valueAreaHigh = 0.0f);
// GetVolumeEnum() removed in v5.7 — VolumeIndicator self-classifies via robust z-score
StructureTest ClassifyStructure(float high, float low, float close,
                                float prev_high, float prev_low, double atr,
                                float lookbackHigh, float lookbackLow);
StructureTest DetectStructure(SCStudyInterfaceRef sc, float prev_high, float prev_low, double atr, float lookbackHigh, float lookbackLow);

// Done
ATRProximityEnum DetectATRProximity(SCStudyInterfaceRef sc, double atr);

// Done
EmaProximity DetectEmaProximity(SCStudyInterfaceRef sc, double ema, double std_dev);

// Done
RSI DetectRSI(float rsiValue);

PriceMetrics DeterminePriceMetric(SCStudyInterfaceRef sc, float avg_range, float avg_volume);


// Calculate Force Index (Volume × Price Change) and apply EMA smoothing
// Force Index = Volume[i] × (Close[i] - Close[i-1])
// Used by TripleScreen1 (FI-13) and TripleScreen2 (FI-2) for Elder's Force Index indicator
void CalculateForceIndex(SCStudyInterfaceRef sc, SCFloatArrayRef forceArray,
                         SCSubgraphRef forceAverage, int emaLength);

// Multi-timeframe coherence score [0,1] from two MACD histogram values.
// 0.0 = anti-aligned, 1.0 = strongly aligned directional momentum.
float CheckCoherence(float tf1_macd_histogram, float tf2_macd_histogram,
                     float max_observed_macd = 0.01f);

// Calculate NH-NL market breadth signal using Alexander Elder's methodology
// Detects divergences, breadth participation changes, and sentiment extremes
// Should be called from 240-min Screen1 Impulse study
// @param nh_nl_daily Daily NYSE NH-NL differential (new highs - new lows)
// @param nh_nl_weekly 7-day rolling sum of NH-NL differential for trend confirmation
// @param nh_nl_prev_weekly Previous week's NH-NL sum (for breadth trend analysis)
// @param currentPrice Current S&P price (for divergence detection)
// @param recentHigh Highest price in last 20 bars (for bearish divergence)
// @param recentLow Lowest price in last 20 bars (for bullish divergence)

/**
 * MACDDivergenceState: Persistent state for Elder's MACD-Histogram divergence detection.
 * Must be stored in study persistent data to survive across bar updates (Sierra Chart AutoLoop).
 *
 * Elder's Rules:
 * - "MACD-Histogram has to cross above the zero line before sinking to its second bottom.
 *    If there is no crossover, then there is no divergence." — Elder
 * - "MACD-H gives a buy signal when it ticks up from its second bottom. It doesn't have
 *    to cross above the centerline for the second time." — Elder
 * - "Two discreet price bottoms, separated by a rally" — Price extremes must be separated by
 *    meaningful price movement (>1.5× ATR)
 */
struct MACDDivergenceState {
    // Bullish divergence tracking
    float priceBottom1 = 0.0f;        // First price trough
    double macdBottom1 = 0.0;         // First MACD-H trough (negative value)
    int macdBottom1Index = -1;        // Bar index of first trough
    bool macdCrossedZeroUp = false;   // "Breaking the back of the bear" — Elder
    int zeroCrossUpIndex = -1;        // Bar index where MACD-H crossed above zero

    // Bearish divergence tracking
    float pricePeak1 = 0.0f;          // First price peak
    double macdPeak1 = 0.0;           // First MACD-H peak (positive value)
    int macdPeak1Index = -1;          // Bar index of first peak
    bool macdCrossedZeroDown = false; // "Breaking the back of the bull" — Elder
    int zeroCrossDownIndex = -1;      // Bar index where MACD-H crossed below zero

    // Rally/decline validation (Elder's "discreet" requirement)
    float rallySize = 0.0f;           // ATR-normalized rally between troughs (bullish) or decline between peaks (bearish)
    int rallyBars = 0;                // Number of bars in rally/decline

    // Quality assessment (strong vs weak divergences)
    float divergenceQuality = 0.0f;   // Quality score 0.0-1.0 (0.7+ = strong, <0.5 = weak)

    // Triple confirmation (Elder's "bullish trifecta")
    bool falseBreakoutConfirmation = false;  // Price broke support/resistance then recovered (Turtle Soup)
    bool macdLinesDivergence = false;        // MACD Lines also showing divergence (rare, "especially strong" — Elder)

    // Current state
    MACDDivergenceEnum currentState = MACDDivergenceEnum::NONE;

    // Configuration
    const int MIN_BARS_BETWEEN_EXTREMES = 5;  // Minimum bars for valid pattern
    const int MAX_BARS_TO_ZERO_CROSS = 50;    // Max bars to wait for zero cross (pattern expires)
    const double ZERO_THRESHOLD = 0.00001;    // Floating point tolerance for zero comparison
    const float MIN_RALLY_ATR_MULTIPLE = 1.5f; // Elder's "discreet" requirement: rally must be >1.5× ATR

    // Reset bullish tracking
    void ResetBullish() {
        priceBottom1 = 0.0f;
        macdBottom1 = 0.0;
        macdBottom1Index = -1;
        macdCrossedZeroUp = false;
        zeroCrossUpIndex = -1;
        rallySize = 0.0f;
        rallyBars = 0;
        divergenceQuality = 0.0f;
        falseBreakoutConfirmation = false;
        macdLinesDivergence = false;
        if (currentState >= MACDDivergenceEnum::SEARCHING_FIRST_TROUGH &&
            currentState <= MACDDivergenceEnum::BULLISH_DIVERGENCE_BUY_SIGNAL) {
            currentState = MACDDivergenceEnum::NONE;
        }
    }

    // Reset bearish tracking
    void ResetBearish() {
        pricePeak1 = 0.0f;
        macdPeak1 = 0.0;
        macdPeak1Index = -1;
        macdCrossedZeroDown = false;
        zeroCrossDownIndex = -1;
        rallySize = 0.0f;
        rallyBars = 0;
        divergenceQuality = 0.0f;
        falseBreakoutConfirmation = false;
        macdLinesDivergence = false;
        if (currentState <= MACDDivergenceEnum::SEARCHING_FIRST_PEAK &&
            currentState >= MACDDivergenceEnum::BEARISH_DIVERGENCE_SELL_SIGNAL) {
            currentState = MACDDivergenceEnum::NONE;
        }
    }
};

/**
 * DetectElderMACDDivergence: Implements Dr. Alexander Elder's MACD-Histogram divergence algorithm.
 * Based on "Two Roads Diverged: Trading Divergences" (2012-2014)
 *
 * Elder's Key Quote: "When trying to find a divergence, first look at the pattern of an
 *                     indicator and later at the pattern of prices." — Elder
 *
 * Critical Requirements:
 * 1. Zero-line crossover is MANDATORY between first and second extremes
 * 2. Buy signal = uptick from second trough (MACD-H still below zero)
 * 3. Sell signal = downtick from second peak (MACD-H still above zero)
 *
 * @param sc Sierra Chart study interface
 * @param currentIndex Current bar index (sc.Index)
 * @param priceHigh Price high array
 * @param priceLow Price low array
 * @param macdHistogram MACD-Histogram values (from MACD study)
 * @param state Persistent divergence state (must survive across bar updates)
 * @param lookbackPeriod How many bars to look back for local extremes (default 3)
 * @return Current divergence state
 */
MACDDivergenceEnum DetectElderMACDDivergence(
    SCStudyInterfaceRef& sc,
    int currentIndex,
    SCFloatArrayRef priceHigh,
    SCFloatArrayRef priceLow,
    SCFloatArrayRef macdHistogram,
    MACDDivergenceState& state,
    float atrValue = 0.0f,
    int lookbackPeriod = 3
);

/**
 * DetectKangarooTail: Implements Elder's Kangaroo tail price-action pattern.
 * Based on "Trading for a Living" and "Come Into My Trading Room"
 *
 * Elder's Definition: Long shadow (tail) showing aggressive rejection of price extreme.
 * - Bullish: Long lower tail, close near high (buyers rejected lower prices)
 * - Bearish: Long upper tail, close near low (sellers rejected higher prices)
 *
 * Elder's Criteria:
 * - Tail ≥ 2× body size (2.5-4× = strong, >4× = extreme)
 * - Tail ≥ 0.5× ATR (must be meaningful vs volatility)
 * - Close in upper 75% (bullish) or lower 25% (bearish) of bar range
 *
 * Most powerful when:
 * - At support/resistance levels
 * - After MACD divergence
 * - With Screen1/Screen2 alignment
 * - High volume (if available)
 *
 * @param open Bar open price
 * @param high Bar high price
 * @param low Bar low price
 * @param close Bar close price
 * @param atr Current ATR (14-period recommended)
 * @param tailToBodyRatio OUT: Calculated tail-to-body ratio
 * @param tailToATR OUT: Calculated tail-to-ATR ratio
 * @param closePosition OUT: Where close is in bar range (0.0-1.0)
 * @param qualityScore OUT: Overall quality score (0.0-1.0)
 * @return KangarooTailEnum state
 */
KangarooTailEnum DetectKangarooTail(
    float open,
    float high,
    float low,
    float close,
    float atr,
    float& tailToBodyRatio,
    float& tailToATR,
    float& closePosition,
    float& qualityScore
);

/**
 * DetectMomentumPinball: Identify Linda Raschke's momentum-based mean-reversion pattern.
 *
 * Theory: "When momentum shifts and price is at an extreme, the bounce is coming."
 * RSI3 crossing RSI10 shows momentum change. Stochastic extreme shows price stretched.
 * Combination = early reversal signal ("catch the pinball bounce").
 *
 * Raschke's Criteria:
 * - RSI Cross: RSI3 must cross RSI10 (fresh momentum shift)
 * - Stochastic Extreme: <20 (oversold bullish) or >80 (overbought bearish)
 * - Strength Classification:
 *   * WEAK: Marginal cross (RSI delta 2-5), stoch barely extreme (15-20 or 80-85)
 *   * STRONG: Strong cross (RSI delta ≥5), stoch deep (10-15 or 85-90), FI2 aligned
 *   * EXTREME: Fresh Impulse change + very deep stoch (<10 or >90) + volume spike ≥1.5×
 *
 * Best Context (Quality Multipliers):
 * - After FI2 pullback/rally (+0.2): Price already pulled back, ready to bounce
 * - MACD-H rising/falling (+0.1): Histogram confirms momentum shift
 * - Screen1 aligned (+0.1): Bullish regime for bullish pinball, bearish for bearish
 *
 * Quote: "The Pinball is my go-to counter-trend play. RSI tells me momentum shifted,
 *         Stochastic tells me price is stretched. That's when I enter." — Linda Raschke
 *
 * Integration:
 * - Screen3 (15-min): PRIMARY USE — Precise entry timing for mean-reversion
 * - Works best in ranging markets (Hurst < 0.55)
 * - Fails in strong trends (Hurst > 0.65, real momentum, not bounce)
 *
 * @param rsi3 Current 3-period RSI (faster, more sensitive)
 * @param rsi10 Current 10-period RSI (slower, smoother)
 * @param prevRSI3 Previous bar RSI3 (to detect cross)
 * @param prevRSI10 Previous bar RSI10 (to detect cross)
 * @param stochK Current Stochastic %K value (0-100)
 * @param impulseColor Current Impulse color (GREEN/RED/BLUE)
 * @param prevImpulseColor Previous bar Impulse color (to detect change)
 * @param volume Current bar volume
 * @param avgVolume Average volume (21-period SMA)
 * @param[out] rsiDelta RSI3 - RSI10 (momentum strength)
 * @param[out] stochDepth Stochastic value (extremity measure)
 * @param[out] impulseJustChanged Did Impulse change color this bar?
 * @param[out] volumeSpike Volume / AvgVolume ratio
 * @param[out] qualityScore 0.0-1.0 overall pattern quality
 * @return MomentumPinballEnum (NONE, BULLISH_WEAK/STRONG/EXTREME, BEARISH_WEAK/STRONG/EXTREME)
 */
MomentumPinballEnum DetectMomentumPinball(
    float rsi3, float rsi10, float prevRSI3, float prevRSI10,
    float stochK, int impulseColor, int prevImpulseColor,
    double volume, double avgVolume,
    float& rsiDelta, float& stochDepth, bool& impulseJustChanged,
    float& volumeSpike, float& qualityScore
);

/**
 * DetectElderBreakout: Identify Dr. Elder/Linda Raschke Keltner Channel breakout pattern.
 *
 * Theory: "Volatility expansion from compression. When price breaks the channel after squeezing,
 *         it signals a directional move has begun." — Elder/Raschke
 *
 * Elder Breakout = Close beyond Keltner Channel band (20-EMA ± 2.5× ATR)
 * This indicates price breaking out of normal volatility range into expansion.
 *
 * Bullish: Close above upper Keltner band
 * - Price breaks above normal volatility envelope (upside expansion)
 * - Most powerful after 5+ bars consolidating AT or NEAR upper band
 * - "Channel squeeze then breakout" pattern
 *
 * Bearish: Close below lower Keltner band
 * - Price breaks below normal volatility envelope (downside expansion)
 * - Most powerful after consolidation at lower band
 *
 * Strength Classification:
 * - WEAK: Barely beyond band (0.1-0.5× ATR), marginal breakout
 * - STRONG: Clear breakout (>0.5× ATR) + volume 1.5× avg + Hurst >0.55
 * - EXTREME: Large breakout (>1× ATR or gap) + 2-3× volume + after 5+ bar consolidation
 *
 * Best Context (Quality Multipliers):
 * - Channel squeeze (+0.2): ATR declining = compression before expansion
 * - Impulse aligned (+0.1): Impulse color (GREEN/RED) matches breakout direction
 * - Screen1 aligned (+0.1): Weekly trend supports breakout direction
 *
 * Quote: "The best breakouts come from the tightest ranges. Look for the squeeze,
 *         then catch the expansion." — Linda Raschke
 *
 * Integration:
 * - Screen3 (15-min): PRIMARY USE — Intraday channel breakouts
 * - Works best when Hurst rising (trend emerging)
 * - Fails in extreme trending (Hurst > 0.80, overextended)
 *
 * @param close Current bar close price
 * @param upperBand Upper Keltner band value
 * @param lowerBand Lower Keltner band value
 * @param atr Current ATR (Average True Range)
 * @param hurst Current Hurst exponent (trend persistence, 0.0-1.0)
 * @param volume Current bar volume
 * @param avgVolume Average volume (21-period SMA)
 * @param consolidationBars Number of bars spent near band before breakout
 * @param isGap Did price gap beyond the band?
 * @param[out] breakoutDistance Distance beyond band (× ATR)
 * @param[out] hurstOut Hurst value (for context)
 * @param[out] volumeSpike Volume / AvgVolume ratio
 * @param[out] consolidationBarsOut Number of consolidation bars detected
 * @param[out] isGapOut Gap flag
 * @param[out] qualityScore 0.0-1.0 overall pattern quality
 * @return ElderBreakoutEnum (NONE, BULLISH_WEAK/STRONG/EXTREME, BEARISH_WEAK/STRONG/EXTREME)
 */
ElderBreakoutEnum DetectElderBreakout(
    float close, float upperBand, float lowerBand,
    float atr, float hurst,
    double volume, double avgVolume,
    int consolidationBars, bool isGap,
    float& breakoutDistance, float& hurstOut, float& volumeSpike,
    int& consolidationBarsOut, bool& isGapOut, float& qualityScore
);

/**
 * DetectTurtleSoup: Implements Linda Raschke's false breakout pattern (Turtle Soup).
 *
 * Pattern: Price breaks beyond recent extreme (4-day high/low), then closes back inside range.
 * This is a "stop hunt" where professionals trap amateur breakout traders.
 *
 * Bullish Turtle Soup:
 * - Price breaks below 4-day low (amateur stops triggered)
 * - Price closes back above 4-day low (professionals enter long)
 * - "The turtles (breakout traders) become soup for the sharks" — Raschke
 *
 * Bearish Turtle Soup:
 * - Price breaks above 4-day high (amateur stops triggered)
 * - Price closes back below 4-day high (professionals enter short)
 *
 * Strength Classification:
 * - WEAK: Penetration 0.1-0.3× ATR, close 0.1-0.3× ATR inside (marginal stop hunt)
 * - STRONG: Penetration 0.3-0.5× ATR, close ≥40% through bar range (strong rejection)
 * - EXTREME: Penetration >0.5× ATR, close ≥80% through bar range (panic reversal)
 *
 * @param open Current bar open price
 * @param high Current bar high price
 * @param low Current bar low price
 * @param close Current bar close price
 * @param fourDayHigh Highest high of previous 4 bars (lookback 1-4)
 * @param fourDayLow Lowest low of previous 4 bars (lookback 1-4)
 * @param atr Current ATR(14) value for volatility context
 * @param[out] penetrationDistance How far price penetrated beyond 4-day extreme (× ATR)
 * @param[out] closeDistance How far close came back inside range (× ATR)
 * @param[out] closePosition Close position in bar range (0.0=low, 1.0=high)
 * @param[out] qualityScore Overall pattern quality (0.0-1.0)
 * @return TurtleSoupEnum state
 */
TurtleSoupEnum DetectTurtleSoup(
    float open,
    float high,
    float low,
    float close,
    float fourDayHigh,
    float fourDayLow,
    float atr,
    float& penetrationDistance,
    float& closeDistance,
    float& closePosition,
    float& qualityScore
);

// @param priceIsRising True if price is in uptrend (EMA or slope analysis)
NhNlSignalEnum CalculateNhNlSignal(int nh_nl_daily, int nh_nl_weekly, int nh_nl_prev_weekly,
                                     float currentPrice, float recentHigh, float recentLow, bool priceIsRising);

/**
 * DetectNR7: Linda Raschke's Narrow Range 7 compression pattern.
 *
 * Theory: "When price consolidates in a tight range, the spring coils tighter.
 *         When it breaks, the expansion follows." — Linda Raschke
 *
 * NR7 = Bar where range (high - low) is smallest over past 7 bars.
 * Compression signal; breakout follows (like release of coiled spring).
 *
 * Strength Classification:
 * - WEAK: Range 95-100% of 7-bar average (barely narrowest, low quality)
 * - STRONG: Range 85-95% + volume declining (good compression, tradeable)
 * - EXTREME: Range <80% + volume very low + consolidation (nuclear setup, best trades)
 *
 * Quality Scoring:
 * - Base: 0.15 (WEAK), 0.25 (STRONG), 0.4 (EXTREME)
 * - Distance bonus: 0.05-0.2 based on percentile
 * - Volume bonus: 0.05-0.15 based on decline
 * - Consolidation bonus: 0.05-0.15 based on bars
 * - Total capped at 1.0
 *
 * Context Enhancement:
 * - Volume decline (+0.2): Volume drying up confirms compression
 * - Impulse aligned (+0.1): Impulse ready to confirm breakout direction
 * - Screen1 aligned (+0.1): Weekly trend predicts breakout direction
 *
 * Integration with RaschkeTacticalTrigger:
 * - NR7Enum = Pattern detection (WEAK/STRONG/EXTREME granularity)
 * - RaschkeTacticalTrigger = Entry decision (NR7_BREAKOUT_BUY/SELL action)
 * - Entry logic: if (nr7 == STRONG/EXTREME + quality ≥0.6) → breakout trigger
 **/


/**
 * DrawStudyHorizontalLine: Draw a horizontal line on the chart at a specified price level.
 *
 * Used for daily high/low levels, support/resistance, etc.
 *
 * @param sc Sierra Chart study interface
 * @param LineNumber Unique line identifier (recommended: 1000+ to avoid conflicts)
 * @param Value Price level to draw line at
 * @param Color RGB color value (e.g., RGB(255, 0, 0) for red)
 * @param LineWidth Line thickness (1-5 typical)
 * @param LineStyle LINESTYLE_SOLID, LINESTYLE_DASH, etc.
 */
void DrawStudyHorizontalLine(SCStudyInterfaceRef sc, int LineNumber, float Value,
                              COLORREF Color, int LineWidth, SubgraphLineStyles LineStyle);

/**
 * DetectNR7: Linda Raschke's Narrow Range 7 compression pattern.
 *
 * Pattern: Current bar range is the narrowest in the last 7 bars.
 * Theory: Compression precedes expansion. NR7 identifies coiled springs before big moves.
 *
 * Linda Raschke Quote:
 * "NR7 is the compression pattern I use most. It's selective enough to filter
 *  whipsaws but common enough to trade regularly." — Linda Raschke
 *
 * Raschke's Strength Classification:
 * - WEAK: Range 95-100% of 7-bar average (barely narrowest)
 * - STRONG: Range 85-95% + volume declining (clear compression)
 * - EXTREME: Range <80% + volume dry-up (maximum compression + 3+ bar consolidation)
 *
 * Integration with RaschkeTacticalTrigger:
 * - NR7Enum = Pattern detection (WEAK/STRONG/EXTREME granularity)
 * - RaschkeTacticalTrigger = Entry decision (NR7_BREAKOUT_BUY/SELL action)
 * - Entry logic: if (nr7 == STRONG/EXTREME + quality ≥0.6) → breakout trigger
 *
 * Quote: "NR7 is the compression pattern I use most. It's selective enough to filter
 *        whipsaws but common enough to trade regularly." — Linda Raschke
 *
 * @param currentHigh Current bar high
 * @param currentLow Current bar low
 * @param rangeArray Array of 7 most recent bar ranges
 * @param volume Current bar volume
 * @param avgVolume Average volume (21-period SMA)
 * @param consolidationBars Number of bars spent in tight range
 * @param[out] currentRange Bar range (high - low)
 * @param[out] avg7BarRange 7-bar average range
 * @param[out] rangePercentile Current range / 7-bar average (0.0-1.0+)
 * @param[out] volumeSpike volume / avgVolume ratio
 * @param[out] qualityScore 0.0-1.0 overall pattern quality
 * @return NR7Enum (NONE, WEAK, STRONG, EXTREME)
 */
NR7Enum DetectNR7(
    float currentHigh, float currentLow,
    const std::vector<float>& rangeArray,
    double volume, double avgVolume,
    int consolidationBars,
    float& currentRange, float& avg7BarRange, float& rangePercentile,
    float& volumeSpike, float& qualityScore
);

// ============================================================================
// ELITE v2.5: 16D Observation Vector Calculation Pipeline
// ============================================================================
// Institutional-grade shared functions for all observation dimensions
// Called from both TripleScreen3_KeltnerChannel and TripleScreen3_TurtleSoup

/**
 * Observation Vector Helper Functions
 *
 * Subgraph layout is owned by each TripleScreen study (TS1/TS2/TS3).
 * Helper functions accept injected SCSubgraphRef parameters — no index assumptions.
 */

/// Calculate adaptive observation lookback window using market speed + cross-timeframe coherence.
/// Returns bounded lookback [10, 40] with hysteresis-driven regime transitions.
int CalculateAdaptiveObservationWindow(SCStudyInterfaceRef sc, float coherence_score);

/// Independent adaptive window for Fisher Information — own persistent state to avoid
/// clobbering the shared AdaptiveWindowParams used by macro/screen windows.
int CalculateFisherAdaptiveWindow(SCStudyInterfaceRef sc, float coherence_score);

/// Update all observation vector subgraphs.
/// Caller (TS3) owns subgraph layout and injects refs — this function is index-agnostic.
void UpdateObservationVectorSubgraphs(
    SCStudyInterfaceRef sc,
    int observation_window_n,
    SCSubgraphRef Subgraph_PathEfficiencySNR,
    SCSubgraphRef Subgraph_HurstExponent,
    SCSubgraphRef Subgraph_MicroAsymmetry,
    SCSubgraphRef Subgraph_RealizedKurtosis,
    SCSubgraphRef Subgraph_SkewnessIdx,
    SCSubgraphRef Subgraph_AmihudIlliquidity,
    SCSubgraphRef Subgraph_LiqFragility,
    SCSubgraphRef Subgraph_ATR,
    SCSubgraphRef Subgraph_VolumeSMA);

/// Path Efficiency SNR (Index 5): Signal-to-Noise Ratio via efficiency ratio squared
float CalculatePathEfficiencySNR(SCStudyInterfaceRef sc, float atr10, int lookback_n = 20);

/// Hurst Exponent (Index 6): Rescaled Range Analysis for persistence detection
float CalculateHurstExponent(SCStudyInterfaceRef sc);

/// TRUE Microstructure Asymmetry from Time and Sales tick data (Institutional-Grade Order Flow)
/// Primary method: Measures real buyer/seller pressure at market makers' surfaces
/// Infers informed trading flow: informed traders come first, retail follows 10-500ms later
float CalculateMicroAsymmetryFromTimeAndSales(SCStudyInterfaceRef sc);

/// Microstructure Asymmetry (Index 7): Dual-mode - T&S priority, fallback to price proxy
/// PRIORITY 1: Time and Sales tick-by-tick (10-100ms informed flow latency)
/// FALLBACK: Price-action signed volume (5s bar-closure latency)
float CalculateMicroAsymmetry(SCStudyInterfaceRef sc, float volume_sma_20, int lookback_n = 20);

/// Realized Kurtosis: 4th moment with bias correction (fat tail detector)
/// prevKurtosis: carry-forward value from previous bar (for low-variance fallback)
float CalculateRealizedKurtosis(SCStudyInterfaceRef sc, float prevKurtosis, SCFloatArrayRef atrArray);

/// Skewness: 3rd moment directional bias
float CalculateSkewness(SCStudyInterfaceRef sc, SCFloatArrayRef atrArray);

/// Liquidity Fragility (Index 11): Bid-Ask spread volatility stress
float CalculateLiquidityFragility(SCStudyInterfaceRef sc, float atrRef, float volumeSma, float prev_fragility);

// ============================================================================
// CANONICAL OBSERVATIONDATA ADDITIONS (Screen 1 - Macro)
// ============================================================================

/// Log-Variance: Variance of log-returns (Macro Volatility)
/// Lookback: ~100-500 bars (Tide)
float CalculateLogVariance(SCStudyInterfaceRef sc, int lookback_n);

/// Fisher Information: Regime change detection
/// Lookback: ~100-500 bars (Tide)
float CalculateFisherInformation(SCStudyInterfaceRef sc, int lookback_n);

// ============================================================================
// CANONICAL OBSERVATIONDATA ADDITIONS (Screen 2 - Wave)
// ============================================================================

/// Realized Variance Ratio: log(RV_recent / RV_full) — volatility expansion/contraction.
/// Lookback: ~20-100 bars (Wave)
float CalculateRealizedVarianceRatio(SCStudyInterfaceRef sc, int lookback_n);

/// Amihud Illiquidity: mean(|r_t| / V_t) — price impact per unit volume.
/// Lookback: ~20-40 bars (Ripple)
float CalculateAmihudIlliquidity(SCStudyInterfaceRef sc, int lookback_n);

/// Burstiness: Inter-arrival time variance (clustering)
/// Lookback: ~20-100 bars (Wave)
float CalculateBurstiness(SCStudyInterfaceRef sc, int lookback_n);

/// Fractal Dimension: Box-counting or similar dimensionality
/// Lookback: ~20-100 bars (Wave)
float CalculateFractalDimension(SCStudyInterfaceRef sc, int lookback_n); // Often D = 2 - H, but explicitly

// ============================================================================
// CANONICAL OBSERVATIONDATA ADDITIONS (Screen 3 - Ripple)
// ============================================================================

/// Mean Reversion Speed: Ornstein-Uhlenbeck theta
/// Lookback: ~5-20 bars (Ripple)
float CalculateMeanReversionSpeed(SCStudyInterfaceRef sc, int lookback_n);

/// Volatility Convexity: Curvature of realized volatility (smile proxy)
/// Lookback: ~5-20 bars (Ripple)
float CalculateVolConvexity(SCStudyInterfaceRef sc, int lookback_n);

/// Recurrence Rate (Index 14): Box-Counting Recurrence (Topological Stability)
/// Function: RQA Recurrence Rate (RR) - % of phase space points within epsilon
/// Lookback: ~20-100 bars (Wave/Ripple)
float CalculateRecurrenceRate(SCStudyInterfaceRef sc, int lookback_n);

/// ============================================================================
/// DETERMINISTIC RESET: Called on sc.IsFullRecalculation to clear buffer state

/// ============================================================================
/// Resets per-study ATRCalculator and EfficiencyRatioCalculator instances.
/// MUST be called when chart reloads or symbol changes to ensure circular buffers
/// don't carry "ghost data" from previous session that would pollute observation features.
/// Sierra Chart Pattern: Call from study's main loop when sc.IsFullRecalculation && sc.UpdateStartIndex == 0
void ResetAdaptiveCalculators(SCStudyInterfaceRef sc);

/// Explicitly frees per-study adaptive calculator state.
/// Call from sc.LastCallToFunction paths to avoid persistent pointer leaks.
void CleanupAdaptiveCalculators(SCStudyInterfaceRef sc);
float CalculateHurstExponent(SCStudyInterfaceRef sc, int length, int minScale);
