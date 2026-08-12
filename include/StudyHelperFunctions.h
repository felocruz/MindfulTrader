#pragma once

#include "sierrachart.h"
#include "Indicator.h"

// This function determines the impulse color based on two sets of data.
// It is intended to be used by the Impulse studies.
int GetImpulse(float maDiff, float macdDiff);

/// True if the current bar falls within the post-weekend-reopen grace window:
/// Sunday, at or after the session open time, within kWeekendGraceHours of it.
/// Reuses the same Sierra Chart day-of-week convention and session-time-derivation
/// pattern already established in EventDataCollectorStudy.cpp's Market-Closed Gate,
/// so it stays correct regardless of chart timezone (ET, CT, etc.) the same way
/// that gate does. See docs/superpowers/plans/2026-08-04-phase1-hardening.md Task 3.
///
/// NOTE: SCDateTime::GetDayOfWeek() returns the DayOfWeekEnum from
/// sierra_chart_dependencies/scdatetime.h, which is SUNDAY=0, MONDAY=1, ...,
/// SATURDAY=6 (verified by direct construction against the vendored header --
/// NOT the 1=Sunday..7=Saturday convention assumed by the Market-Closed Gate's
/// own comment in EventDataCollectorStudy.cpp, which appears to be mislabeled).
bool IsPostWeekendReopenGracePeriod(SCStudyInterfaceRef sc);


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

// DetectKangarooTail/DetectMomentumPinball/DetectElderBreakout/DetectTurtleSoup moved
// to include/IndicatorComputations.h (indicator-manager-dod-soa plan, Task 8) — they
// took only primitive arguments already, so they're now inline free functions there,
// alongside their now-ACSIL-independent enums (KangarooTailEnum/MomentumPinballEnum/
// ElderBreakoutEnum/TurtleSoupEnum).

// @param priceIsRising True if price is in uptrend (EMA or slope analysis)
NhNlSignalEnum CalculateNhNlSignal(int nh_nl_daily, int nh_nl_weekly, int nh_nl_prev_weekly,
                                     float currentPrice, float recentHigh, float recentLow, bool priceIsRising);

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

// DetectNR7 moved to include/IndicatorComputations.h (indicator-manager-dod-soa plan,
// Task 8), then deleted entirely (docs/superpowers/specs/2026-08-06-indicator-orphan-
// cleanup-design.md) after confirming zero production callers -- TripleScreen3.cpp has
// always had its own separate inline NR7 detection. NR7Enum itself remains live (real
// production consumer via the NR7 indicator class).

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
