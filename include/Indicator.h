#pragma once

#include "sierrachart.h" // Use Sierra Chart's main header
#include <string>
#include <string_view> // C++17
#include <vector>
#include "generated/mts_schema_generated.h"

// Use a constexpr function for compile-time color evaluation
constexpr int CreateRGB(int r, int g, int b) {
    return ((r & 0xff) | ((g & 0xff) << 8) | ((b & 0xff) << 16));
}

// Impulse System colors
const int GREEN{ CreateRGB(0, 255, 0) };
const int RED{ CreateRGB(255, 0, 0) };
const int BLUE{ CreateRGB(0, 128, 255) };

#include "IndicatorKey.h"  // IndicatorKey enum (extracted so it's ACSIL-independent; see IndicatorKey.h)
/**
 * Linda Raschke Tactical Entry Triggers (Screen 3, 5-min bars)
 *
 * These are precise entry confirmation signals when structural setup (RaschkeStrategySetup)
 * aligns with directional trend (Screen 1) and timing context (Screen 2).
 *
 * Philosophy: Enter when amateurs trapped (Turtle Soup), momentum precedes price (Momentum Pinball),
 * breakouts work in trends (Elder Breakout), compression releases (NR7 Breakout).
 *
 * Source: Linda Raschke "Street Smarts" (1995) + Elder "Come Into My Trading Room" (2002)
 * Range: 0-18 (mirrors Python rc_enums.RaschkeTacticalTrigger exactly)
 */
enum class RaschkeTacticalTrigger : int8_t
{
    NONE = 0,
    KANGAROO_TAIL_BUY = 1,
    KANGAROO_TAIL_SELL = 2,
    TURTLE_SOUP_BUY = 3,
    TURTLE_SOUP_SELL = 4,
    MOMENTUM_PINBALL_BUY = 5,
    MOMENTUM_PINBALL_SELL = 6,
    ELDER_BREAKOUT_BUY = 7,
    ELDER_BREAKOUT_SELL = 8,
    NR7_BREAKOUT_BUY = 9,
    NR7_BREAKOUT_SELL = 10,
    ITR_BREAKOUT_BUY = 11,
    ITR_BREAKOUT_SELL = 12,
    ITR_FADE_BUY = 13,
    ITR_FADE_SELL = 14,
    RSI_FAILURE_SWING_BUY = 15,
    RSI_FAILURE_SWING_SELL = 16,
    STOCHASTIC_POP_BUY = 17,
    STOCHASTIC_POP_SELL = 18
};

/**
 * Linda Raschke Strategy Setups (Screen 2, 15-min bars)
 *
 * Structural patterns that define trading opportunity context. While Screen 1 defines
 * the trend direction, these setups define WHEN to wait for entries.
 *
 * Philosophy: Volatility compression → expansion, pullbacks in trends,
 * failed breakouts = stop hunts, divergence signals reversal.
 *
 * Source: Linda Raschke "Street Smarts" (1995), "Reminiscences of a Stock Operator" (Livermore)
 * Range: 0-21 (mirrors Python rc_enums.RaschkeStrategySetup exactly)
 */
enum class RaschkeStrategySetup : int8_t
{
    NONE = 0,
    THREE_BAR_TRIANGLE = 1,
    NR4 = 2,
    NR7 = 3,
    IDNR4 = 4,
    WHIPLASH = 7,
    GHOST = 8,
    TWO_B_REVERSAL = 9,
    ANTI = 10,
    HOLY_GRAIL_CONTINUATION = 12,
    HOLY_GRAIL_BUY = 13,
    HOLY_GRAIL_SELL = 14,
    SLINGSHOT = 15,
    FIRST_CROSS = 16,
    BREAD_AND_BUTTER = 17,
    DOUBLE_REPO = 18,
    DOUBLE_REPO_FAILURE = 19,
    FLIP = 20,
    NR4_NR7_VOLUME_SPIKE = 21
};

// Helper: Get enum name for logging
inline const char* getRaschkeTacticalTriggerName(RaschkeTacticalTrigger trigger) {
    switch (trigger) {
        case RaschkeTacticalTrigger::NONE: return "NONE";
        case RaschkeTacticalTrigger::KANGAROO_TAIL_BUY: return "KANGAROO_TAIL_BUY";
        case RaschkeTacticalTrigger::KANGAROO_TAIL_SELL: return "KANGAROO_TAIL_SELL";
        case RaschkeTacticalTrigger::TURTLE_SOUP_BUY: return "TURTLE_SOUP_BUY";
        case RaschkeTacticalTrigger::TURTLE_SOUP_SELL: return "TURTLE_SOUP_SELL";
        case RaschkeTacticalTrigger::MOMENTUM_PINBALL_BUY: return "MOMENTUM_PINBALL_BUY";
        case RaschkeTacticalTrigger::MOMENTUM_PINBALL_SELL: return "MOMENTUM_PINBALL_SELL";
        case RaschkeTacticalTrigger::ELDER_BREAKOUT_BUY: return "ELDER_BREAKOUT_BUY";
        case RaschkeTacticalTrigger::ELDER_BREAKOUT_SELL: return "ELDER_BREAKOUT_SELL";
        case RaschkeTacticalTrigger::NR7_BREAKOUT_BUY: return "NR7_BREAKOUT_BUY";
        case RaschkeTacticalTrigger::NR7_BREAKOUT_SELL: return "NR7_BREAKOUT_SELL";
        case RaschkeTacticalTrigger::ITR_BREAKOUT_BUY: return "ITR_BREAKOUT_BUY";
        case RaschkeTacticalTrigger::ITR_BREAKOUT_SELL: return "ITR_BREAKOUT_SELL";
        case RaschkeTacticalTrigger::ITR_FADE_BUY: return "ITR_FADE_BUY";
        case RaschkeTacticalTrigger::ITR_FADE_SELL: return "ITR_FADE_SELL";
        case RaschkeTacticalTrigger::RSI_FAILURE_SWING_BUY: return "RSI_FAILURE_SWING_BUY";
        case RaschkeTacticalTrigger::RSI_FAILURE_SWING_SELL: return "RSI_FAILURE_SWING_SELL";
        case RaschkeTacticalTrigger::STOCHASTIC_POP_BUY: return "STOCHASTIC_POP_BUY";
        case RaschkeTacticalTrigger::STOCHASTIC_POP_SELL: return "STOCHASTIC_POP_SELL";
        default: return "UNKNOWN";
    }
}

// Define the TradeSideEnum here for use by multiple components
enum class TradeSideEnum : int8_t
{
    FLAT = 0,
    LONG = 1,
    SHORT = 2
};

// TradeActionEnum - ML model predictions for position actions
// Mirrors Python GUI's TradeAction enum exactly
enum class TradeActionEnum : int
{
    STAND_ASIDE = 0,
    ENTER_LONG  = 1,
    ENTER_SHORT = 2,
    HOLD_LONG   = 3,
    HOLD_SHORT  = 4,
    EXIT_LONG   = 5,
    EXIT_SHORT  = 6,
    TRAP_LONG   = 7,
    TRAP_SHORT  = 8
};

// Helper functions for TradeActionEnum
inline const char* TradeActionToString(TradeActionEnum action) {
    switch (action) {
        case TradeActionEnum::STAND_ASIDE: return "STAND_ASIDE";
        case TradeActionEnum::ENTER_LONG: return "ENTER_LONG";
        case TradeActionEnum::ENTER_SHORT: return "ENTER_SHORT";
        case TradeActionEnum::HOLD_LONG: return "HOLD_LONG";
        case TradeActionEnum::HOLD_SHORT: return "HOLD_SHORT";
        case TradeActionEnum::EXIT_LONG: return "EXIT_LONG";
        case TradeActionEnum::EXIT_SHORT: return "EXIT_SHORT";
        case TradeActionEnum::TRAP_LONG: return "TRAP_LONG";
        case TradeActionEnum::TRAP_SHORT: return "TRAP_SHORT";
        default: return "UNKNOWN";
    }
}

inline bool IsEntryAction(TradeActionEnum action) {
    return action == TradeActionEnum::ENTER_LONG ||
           action == TradeActionEnum::ENTER_SHORT;
}

inline bool IsExitAction(TradeActionEnum action) {
    return action == TradeActionEnum::EXIT_LONG ||
           action == TradeActionEnum::EXIT_SHORT;
}

inline bool IsHoldAction(TradeActionEnum action) {
    return action == TradeActionEnum::HOLD_LONG ||
           action == TradeActionEnum::HOLD_SHORT;
}

// TradingEventType - Events published on port 5555 (MindfulSocketZMQ PUB)
enum class TradingEventType : int
{
    BAR_CLOSED = 0,          // New bar completed
    ENTRY_FILLED = 1,        // Entry order filled
    POSITION_UPDATE = 2,     // Position state changed
    STOP_HIT = 3,           // Stop loss triggered
    TARGET_HIT = 4,         // Profit target hit
    PARTIAL_FILL = 5,       // Scale-in/out partial fill
    ORDER_CANCELLED = 6,    // Order cancelled (staleness, manual, etc.)
    TRAILING_STOP_MOVED = 7 // Stop moved to new price
};

inline const char* TradingEventTypeToString(TradingEventType eventType) {
    switch (eventType) {
        case TradingEventType::BAR_CLOSED: return "BAR_CLOSED";
        case TradingEventType::ENTRY_FILLED: return "ENTRY_FILLED";
        case TradingEventType::POSITION_UPDATE: return "POSITION_UPDATE";
        case TradingEventType::STOP_HIT: return "STOP_HIT";
        case TradingEventType::TARGET_HIT: return "TARGET_HIT";
        case TradingEventType::PARTIAL_FILL: return "PARTIAL_FILL";
        case TradingEventType::ORDER_CANCELLED: return "ORDER_CANCELLED";
        case TradingEventType::TRAILING_STOP_MOVED: return "TRAILING_STOP_MOVED";
        default: return "UNKNOWN";
    }
}

// ImpulseEnum (extracted so it's ACSIL-independent; see IndicatorComputations.h) —
// same rationale as MacdEnum's extraction below (indicator-manager-dod-soa
// plan, Task 7): ComputeImpulse's return type must be the real ImpulseEnum.

enum class EmaEnum : int8_t
{
    UNDEFINED = 0,
    FLAT = 1,
    INC = 2,
    DEC = 3
};

// MacdEnum (extracted so it's ACSIL-independent; see IndicatorComputations.h) —
// same rationale as IndicatorKey.h's extraction above: ComputeMacd's free
// function needs the real enum type, and that header must stay includable
// with no sierrachart.h on the path (indicator-manager-dod-soa plan, Task 6).
#include "IndicatorComputations.h"

/*
  Represents the signal interpretation of the 13-period Daily 13-period Force Index (FI)
  based on Alexander Elder's methodology.
*/
enum class FI13Enum : int8_t
{
    // Neutral or unclear state
    UNCLEAR = 0,  // No strong signal, or conflicting indications from FI.

    // Trend Confirmation Signals (FI confirms price action and momentum)
    BULLISH_TREND_CONFIRMED = 1, // FI is positive and generally rising, confirming an uptrend.
    // Indicates strong buying pressure.

    BEARISH_TREND_CONFIRMED = -1, // FI is negative and generally falling, confirming a downtrend.
    // Indicates strong selling pressure.

    // Divergence Signals (These are the most powerful signals according to Elder)
    BULLISH_DIVERGENCE = 2, // Price makes a lower low, but FI makes a higher low.
                            // Indicates selling pressure is waning; a potential reversal up is near.

    BEARISH_DIVERGENCE = -2, // Price makes a higher high, but FI makes a lower high.
                             // Indicates buying pressure is weakening; a potential reversal down is near.

    // Zero Line Crossover Signals (Changes in control between bulls and bears)
    BULLISH_CROSSOVER = 3, // FI crosses above the zero line.
                           // Indicates bulls are gaining control; potential new uptrend or resumption of uptrend.

    BEARISH_CROSSOVER = -3, // FI crosses below the zero line.
                           // Indicates bears are gaining control; potential new downtrend or resumption of downtrend.

    BULLISH_PUMP = 4,
    BEARISH_DUMP = -4
};

enum class FI2Enum : int8_t
{
    NEUTRAL_OR_TREND_ALIGNED = 0,
    PULLBACK_FOR_LONG = 1,
    RALLY_FOR_SHORT = -1,
    SIGNAL_UP = 2,
    SIGNAL_DOWN = -2
};

enum class StochasticEnum : int8_t
{
    UNDEFINED = 0,
    NORMAL = 1,
    OVER_BOUGHT = 2,
    OVER_SOLD = 3,
    BULLISH_DIVERGENCE = 4,
    BEARISH_DIVERGENCE = 5
};

enum class DivergenceEnum : int8_t
{
    NONE = 0,
    PENDING_BEARISH_DIVERGENCE = 1, // Price made a high, MACD made a high, now waiting for MACD to cross below zero.
    BEARISH_DIVERGENCE = 2,         // Confirmed Elder Bearish Divergence (price higher high, MACD lower high after zero cross).
    PENDING_BULLISH_DIVERGENCE = 3, // Price made a low, MACD made a low, now waiting for MACD to cross above zero.
    BULLISH_DIVERGENCE = 4,          // Confirmed Elder Bullish Divergence (price lower low, MACD higher low after zero cross).
    BULLISH_DIVERGENCE_DOUBLE_BOTTOM = 5, // Price Equal Low, MACD Higher Low
    BEARISH_DIVERGENCE_DOUBLE_TOP = 6,    // Price Equal High, MACD Lower High
    HIDDEN_BULLISH_DIVERGENCE = 7,   // Price Higher Low, MACD Lower Low
    HIDDEN_BEARISH_DIVERGENCE = 8    // Price Lower High, MACD Higher High
};

/**
 * MACDDivergenceEnum: State machine for Dr. Alexander Elder's MACD-Histogram divergence detection.
 * Based on "Two Roads Diverged: Trading Divergences" (2012-2014)
 *
 * Elder's Critical Rules:
 * 1. MACD-H MUST cross zero line between first and second extremes (non-negotiable)
 * 2. Price extremes must be "discreet" = separated by meaningful rally/decline (>1.5× ATR)
 * 3. Bullish: Price lower low + MACD-H higher low (less negative) after zero cross UP
 * 4. Bearish: Price higher high + MACD-H lower high (less positive) after zero cross DOWN
 * 5. Pattern exists ≠ Trade signal. Signal = uptick/downtick from second extreme
 * 6. Professionals re-enter on pattern renewal after small stop-outs (Elder's GE example)
 *
 * Quote: "MACD-Histogram has to cross above the zero line before sinking to its second bottom.
 *         If there is no crossover, then there is no divergence." — Elder
 *
 * Quote: "The bear is getting older and weaker — but the bear is still in charge!"
 *         (When MACD-H makes higher lows without crossing zero)
 */
enum class MACDDivergenceEnum : int8_t
{
    NONE = 0,                              // No divergence pattern active

    // Bullish divergence states (positive values)
    SEARCHING_FIRST_TROUGH = 1,            // Looking for first MACD-H low (below zero)
    WAITING_ZERO_CROSS_UP = 2,             // First trough found, waiting for zero cross UP ("break bear's back")
    WAITING_SECOND_TROUGH = 3,             // Zero crossed up, looking for second trough with divergence
    BULLISH_DIVERGENCE_PATTERN = 4,        // Pattern formed (price lower low + MACD higher low), waiting for uptick
    BULLISH_DIVERGENCE_BUY_SIGNAL = 5,     // Elder buy signal: upticked from second trough (still below zero)

    // Bearish divergence states (negative values)
    SEARCHING_FIRST_PEAK = -1,             // Looking for first MACD-H high (above zero)
    WAITING_ZERO_CROSS_DOWN = -2,          // First peak found, waiting for zero cross DOWN ("break bull's back")
    WAITING_SECOND_PEAK = -3,              // Zero crossed down, looking for second peak with divergence
    BEARISH_DIVERGENCE_PATTERN = -4,       // Pattern formed (price higher high + MACD lower high), waiting for downtick
    BEARISH_DIVERGENCE_SELL_SIGNAL = -5    // Elder sell signal: downticked from second peak (still above zero)
};

enum class PriceActionEnum : int8_t
{
    FUBK = 0,
    HIT_UPPER_CHANNEL = 1,
    ABOVE_VALUE = 2,
    IN_VALUE_ZONE = 3,
    BELOW_VALUE = 4,
    HIT_LOWER_CHANNEL = 5,
    FDBK = 6,
    NONE = 7
};

// ===== DUPLICATE REMOVED =====
// RaschkeStrategySetup already defined at line 151 with int8_t type
// Keeping the primary definition (int8_t) for consistency with other enums

// ===== DEPRECATED: RaschkeTacticalTrigger definition moved to line ~116 =====
// This duplicate was for backward compatibility. Use the primary definition above.
// DO NOT use this - it's only kept for reference. Delete if compilation succeeds.

// KangarooTailEnum/TurtleSoupEnum/MomentumPinballEnum/ElderBreakoutEnum/NR7Enum
// (extracted so they're ACSIL-independent; see IndicatorComputations.h) — same
// rationale as MacdEnum/ImpulseEnum/VolumeEnum's extraction elsewhere in this
// file (indicator-manager-dod-soa plan, Task 8): DetectKangarooTail/
// DetectTurtleSoup/DetectMomentumPinball/DetectElderBreakout/DetectNR7's return
// types must be the real enums, and that header must stay includable with no
// sierrachart.h on the path.

enum class RSI : int8_t
{
    UNDEFINED = 0,          // RSI undefined
    NORMAL = 1,             // RSI is in a normal range
    OVERBOUGHT = 2,         // RSI is in the overbought region (above 70)
    OVERSOLD = 3,           // RSI is in the oversold region (below 30)
    BULLISH_DIVERGENCE = 4, // RSI shows bullish divergence with price
    BEARISH_DIVERGENCE = 5  // RSI shows bearish divergence with price
};

// VolumeEnum (extracted so it's ACSIL-independent; see IndicatorComputations.h) —
// same rationale as MacdEnum/ImpulseEnum's extraction above (indicator-manager-
// dod-soa plan, Task 7): ComputeVolumeClassification's return type must be the
// real VolumeEnum.

enum class StructureTest : int8_t
{
    NONE = 0, // No price action near the previous bar high or low boundary.

    // BULLISH REVERSAL / FAILURE TESTS (Suggests Long Opportunity)
    FAILED_LOW_CLOSE_INSIDE = 1, // Price penetrates previous low but closes inside. Bear trap reversal signal.
    FAILED_LOW_STRONG_REVERSAL = 2, // Price breaks previous low, closes significantly higher. Strong bullish reversal signal (Turtle Soup potential).

    // BEARISH REVERSAL / FAILURE TESTS (Suggests Short Opportunity)
    FAILED_HIGH_CLOSE_INSIDE = 3, // Price penetrates previous high but closes inside. Bull trap reversal signal.
    FAILED_HIGH_STRONG_REVERSAL = 4, // Price breaks previous high, closes significantly lower. Strong bearish reversal signal (Turtle Soup potential).

    // CONTINUATION / DECISIVE ACTION
    DECISIVE_BREAKOUT_HIGH = 5, // Price closes decisively above the previous bar high. Strong bullish continuation signal.
    DECISIVE_BREAKDOWN_LOW = 6, // Price closes decisively below the previous bar low. Strong bearish continuation signal.

    // CONSOLIDATION / EXPANSION
    INSIDE_BAR = 7, // Current bar's range is completely within the previous bar's range.
    OUTSIDE_BAR = 8 // Current bar's range completely engulfs the previous bar's range.
};

enum class ATRProximityEnum : int8_t
{
    LOW_VOLATILITY = 0,
    HIGH_MOVE = 1,
    EXTREME_VOLATILITY = 2,
    EXTREME_LOW = 3,
    EXTREME_HIGH = 4
};

enum class EmaProximity : int8_t
{
    NONE = -1,
    ABOVE_STRONG = 0,
    ABOVE_TOUCH = 1,
    CROSS_ABOVE = 2,
    AT_EMA = 3,
    CROSS_BELOW = 4,
    BELOW_TOUCH = 5,
    BELOW_STRONG = 6,
    PRICE_ABOVE_EMA = 7,
    PRICE_BELOW_EMA = 8
};

enum class PriceMetrics : int8_t
{
    NORMAL = 0,
    STRONG_BULLISH = 1,
    STRONG_BEARISH = 2
};

enum class MarketSymbol : int8_t
{
    MES = 1,
    MGC = 2,
    MCL = 3,
    MNQ = 4,
    M2K = 5
};


// Enum definition for the Daily Price Action Bias.
// The integer values must match exactly with the Python `DailyBiasEnum` in `rc_enums.py`.
enum class DailyBiasEnum : int8_t {
    PHYSICS_VETO_RANDOM_WALK = 0,    // Hurst ~0.5 (Mandelbrot: Do Not Touch)
    PHYSICS_VETO_HIGH_ENTROPY = 3,   // Shannon: Signal too noisy (Wait)
    VALUE_AREA_ROTATION = 4,         // Raschke: Inside 70% VA (Fade Edges)

    BULLISH_TREND_PERSISTENT = 1,    // Raschke Breakout + Hurst > 0.5 (Go With)
    BEARISH_TREND_PERSISTENT = -1,   // Raschke Breakdown + Hurst > 0.5 (Go With)

    BULLISH_MEAN_REVERSION = 2,      // Raschke 80% Rule (Bounce off Low/VAL)
    BEARISH_MEAN_REVERSION = -2,     // Raschke 80% Rule (Reject off High/VAH)

    BULLISH_VOLATILITY_TRAP = 5,     // Breakout Failed + Low Hurst (Taleb Trap)
    BEARISH_VOLATILITY_TRAP = -5     // Breakdown Failed + Low Hurst (Taleb Trap)
};

/**
 * Categorizes the current trading session based on time of day.
 * Helps identify high-quality entry windows vs low-quality sessions.
 *
 * Based on Linda Raschke's session quality framework:
 * - Opening Hour (9:30-10:30): High volatility, trend establishment
 * - Sweet Spot (10:30-12:00): Cleanest trends, best follow-through
 * - Lunch Dead Zone (12:00-14:00): Avoid entries, choppy action
 * - Afternoon Session (14:00-15:00): Second chance setups
 * - Final Hour (15:00-16:00): Avoid new entries, close intraday positions
 *
 * Calculated by C++ execution layer from bar datetime.
 */
enum class TimeOfDayEnum : int8_t {
    // === Globex Session Windows ===
    ASIAN_SESSION = 0,          // 18:00-03:00 ET - Globex overnight trading
    LONDON_WINDOW = 1,          // 03:00-04:00 ET - European open influence
    LONDON_TO_PREMARKET = 2,    // 04:00-08:30 ET - Pre-US session positioning
    PRE_MARKET_HOOK = 3,        // 08:30-09:00 ET - Economic data reaction window

    // === Regular Trading Hours ===
    PRE_MARKET = 4,             // 09:00-09:30 ET - Pre-market positioning
    OPENING_HOUR = 5,           // 09:30-10:30 ET - High volatility, trend establishment
    SWEET_SPOT = 6,             // 10:30-12:00 ET - Cleanest trends, best entries
    LUNCH_DEAD_ZONE = 7,        // 12:00-14:00 ET - Avoid entries, choppy action
    AFTERNOON_SESSION = 8,      // 14:00-15:00 ET - Second chance setups
    FINAL_HOUR = 9,             // 15:00-15:45 ET - Avoid new entries unless strong setup
    PM_RUN_ENTRY = 10,          // 15:45-16:00 ET - Late-day entry, must profit immediately
    AFTER_HOURS = 11,           // 16:00-18:00 ET - Low liquidity, position squaring

    // === Overnight Hold State ===
    OVERNIGHT_HOLD = 12         // Position held overnight (set when carrying position through close)
};

/**
 * Overnight Exit Type classification for positions carried through market close.
 * Based on Linda Raschke and George Taylor's overnight management methodology.
 *
 * Determines how to exit overnight positions based on next morning's price action:
 * - Gap exits (windfall)
 * - First reaction exits (flat/adverse open)
 * - Objective point exits (Taylor targets)
 * - Momentum failure exits (3-10 oscillator crossed during Globex)
 * - Scratch exits (didn't meet Golden Rule)
 * - Hold continuation (target not yet hit)
 *
 * Calculated during Globex session or first 90 minutes of regular trading.
 */
enum class OvernightExitTypeEnum : int8_t {
    NO_OVERNIGHT_POSITION = 0,      // Flat, no overnight position

    // === Overnight Entry Validation ===
    STRONG_CLOSE_QUALIFIED = 1,     // Position in profit, closed top/bottom 25% of range
    FAILED_GOLDEN_RULE = 2,         // Not in profit OR weak close → MUST scratch

    // === Next Morning Exit Types ===
    GAP_EXIT = 3,                   // Gap in favor (windfall) → exit 09:30-09:45 ET
    FIRST_REACTION_EXIT = 4,        // Flat/adverse open → exit on first bounce (09:30-10:00 ET)
    OBJECTIVE_POINT_EXIT = 5,       // Taylor target hit (prev day H/L) → exit at liquidity window
    MOMENTUM_FAILURE_EXIT = 6,      // 3-10 Oscillator crossed during Globex → exit pre-market
    SCRATCH_EXIT = 7,               // Immediate exit (didn't meet Golden Rule criteria)

    // === Hold Continuation ===
    HOLD_FOR_TARGET = 8,            // Strong position, target not yet hit, continue holding
    TRAILING_STOP_EXIT = 9          // Stop moved to profit, let it run or get stopped
};

/**
 * HMMStateEnum mirrors Python rc_enums.HMMStateEnum for cross-language parity.
 *
 * IDs are a fixed canonical convention, not a raw EM output: EM state indices
 * are arbitrary per-fit (label-switching) and are NOT guaranteed to match
 * this convention on their own. The Python training pipeline's state-alignment
 * step (lbrnet/docs/architecture/HMM_STATE_ALIGNMENT_SPEC.md) reorders each
 * freshly-fit model's raw arrays before saving so that raw_id matches these
 * ids by construction -- this side never runs independent HMM inference, it
 * only deserialises the already-resolved hmm_state_id Python sends over the
 * wire (HMMClient::HandleBinaryResponse -> MutableHmmState()).
 * No UNKNOWN member — use HMM_NO_PRIOR constexpr for uninitialised state.
 *
 * Identified by RULE, not by fixed numeric centroids (any specific centroid
 * values are retrain-specific and go stale the moment the feature pipeline
 * or training data changes -- this bit us once already: an earlier version
 * of this comment cited z-score-era centroid values from before the
 * redundant Python-side normalization step was removed, 2026-08-02):
 *   COILED_SPRING:    quiet (low vol_convexity/relative_range) AND fleeting
 *                     (low tenure/occupancy) -- rare, short-lived compression
 *   GAUSSIAN_STABLE:  quiet AND persistent (high tenure/occupancy) -- the
 *                     common, long-lived baseline regime
 *   GAUSSIAN_FRAGILE: DOF < 4.0 (Student-t kurtosis-undefined threshold,
 *                     Kotz & Nadarajah 2004) OR low-tenure/high-entropy
 *                     chaos -- the "doesn't look like a clean archetype"
 *                     bucket; also absorbs what MarketClimate labels
 *                     SHANNON_CHAOS below, since HMMStateEnum has no chaos
 *                     slot of its own
 *   PARETO_MOMENTUM:  high burstiness AND positive relative_range --
 *                     directional thrust
 * See lbrnet.models.regime_registry.compute_hmmstate_affinity() (Python) for
 * the exact, executable form of these rules.
 */
enum class HMMStateEnum : int8_t {
    COILED_SPRING    = 0,
    GAUSSIAN_STABLE  = 1,
    GAUSSIAN_FRAGILE = 2,
    PARETO_MOMENTUM  = 3
};

/// Pipeline sentinel: HMM inference not yet received. Not a trained model state.
constexpr HMMStateEnum HMM_NO_PRIOR = static_cast<HMMStateEnum>(-1);

enum class MarketClimate : int8_t {
        GAUSSIAN_STABLE = 0,
        TALEBIAN_FRAGILE = 1,
        SHANNON_CHAOS = 2,
        PARETO_MOMENTUM = 3,
        COILED_SPRING = 4
};


/**
 * NH-NL (New Highs - New Lows) market breadth signal.
 * Based on Dr. Alexander Elder's methodology from "The New High - New Low Index" (2nd Edition, 2014).
 *
 * Elder's Definition: NH-NL = (Number of New 365-day Highs) - (Number of New 365-day Lows)
 * Data: NYSE + AMEX + NASDAQ combined (actual count from C:/Trading/data/NH_NL.csv)
 *
 * Elder's Analogy: "Visualize all stocks as soldiers attacking a hill. New Highs are officers
 * leading uphill. New Lows are officers deserting downhill. When more run down than up, the attack fails."
 *
 * Elder's Key Thresholds (from book):
 * - Daily: +100/-100 confirmation band (filters whipsaws)
 * - Weekly: +2500 = bull market confirmation ("solid ground, hold longs")
 * - Weekly: -4000 = extreme panic spike ("mass capitulation, buy signal")
 * - Weekly: -1500 = mini-spike in bull markets ("high-quality opportunity")
 *
 * Critical Rule for Divergences: NH-NL MUST cross zero line between price peaks/troughs
 *
 * Priority: Extremes → Zero-Cross Divergences → Breadth Participation → Confirmations
 *
 * Values match Python NhNlSignal enum in rc_enums.py for consistency.
 */
enum class NhNlSignalEnum : int8_t {
    UNCLEAR = 0,                  // Daily NH-NL between +100 and -100 (Elder: neutral zone)
    BULLISH_CONFIRMATION = 1,     // Daily > +100 OR Weekly > +2500 (Elder: bull market confirmed)
    BEARISH_CONFIRMATION = -1,    // Daily < -100 (Elder: bears in control)
    BEARISH_DIVERGENCE = -2,      // S&P higher high + NH-NL lower high after zero cross (Elder: sell signal)
    BULLISH_DIVERGENCE = 2,       // S&P lower low + NH-NL higher low after zero cross (Elder: buy signal)
    EXTREME_LOWS_BOUNCE = 3,      // Weekly < -4000 then rises (Elder: panic capitulation, trend lasts ~1 year)
    EXTREME_HIGHS_PEAK = -3,      // S&P new high but weekly cannot reach +2500 (Elder: narrow rally warning)
    NARROWING_RALLY = 4,          // Price rising but fewer stocks participating (Elder: weak rally will fail)
    BROADENING_RALLY = 5,         // Price rising with increasing participation (Elder: healthy trend)
    NARROWING_DECLINE = 6,        // Price falling but breadth improving (Elder: selling exhaustion)
    BROADENING_DECLINE = -4       // Price falling with worsening breadth (Elder: strong downtrend)
};

/**
 * Oscillator310CrossEnum: Cross states for Linda Raschke's 3-10 Oscillator (EMA(3) - EMA(16))
 * - NEUTRAL: No cross detected
 * - BULLISH_CROSS: Fast line crossed above slow line (bullish signal)
 * - BEARISH_CROSS: Fast line crossed below slow line (bearish signal)
 */
// Oscillator310CrossEnum (extracted so it's ACSIL-independent; see
// IndicatorComputations.h) — same rationale as MacdEnum/ImpulseEnum/VolumeEnum's
// extraction above (indicator-manager-dod-soa plan, Task 7):
// ComputeOscillator310Cross's return type must be the real Oscillator310CrossEnum.


// A non-templated base class for polymorphism
class BaseIndicator
{
public:
    virtual ~BaseIndicator() = default;
    virtual void Reset() = 0;
    virtual bool IsDirty() const = 0;  // Stage A: Change tracking (for piggyback)
    virtual bool ShouldTrigger() const = 0;  // Stage B: Strategic trigger logic
    virtual int intValue() const = 0;
    virtual IndicatorKey Key() const = 0;
    virtual void SetDirtyMaskPointer(uint64_t* maskPtr) = 0;

    // Extract int8_t value and clear dirty flag (atomic operation for FlatBuffer serialization)
    virtual int8_t ExtractInt8AndClearDirty() = 0;

    // Training: Set named field in wide table for research/training data
    virtual void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const = 0;
};

template <typename T>
class Indicator : public BaseIndicator
{
protected:
    IndicatorKey m_key;
    T m_defaultValue;
    T m_value;
    T m_prevValue;
    uint64_t* m_dirty_mask_ptr = nullptr;
    // Dual-write target during the DOD/SoA migration (indicator-manager-dod-soa
    // plan, Task 4): nullptr = not yet wired. Written alongside m_value inside
    // Update() below; nothing reads through this pointer yet.
    int8_t* m_packedSlotI8 = nullptr;
    // Task 9 fix (Finding 2): companion "previous" slot in m_packed's Int8
    // block. Task 4's raw-pointer dual-write only ever overwrote *current*
    // (m_packedSlotI8) — it never shifted the old value into *prev* the way
    // IndicatorPackedState::SetI8() does internally, so m_packed.GetPrevI8()
    // silently stayed 0 forever for every indicator updated through this path
    // (i.e. everything except LONG_MACD/INTERM_MACD, which go through
    // IndicatorManager::SetValue<Key>() instead). nullptr = not wired.
    int8_t* m_packedPrevSlotI8 = nullptr;

    uint64_t KeyBit() const {
        return 1ULL << static_cast<uint64_t>(m_key);
    }

public:
    Indicator(IndicatorKey key_, const T& defaultValue_)
        : m_key{ key_ }
        , m_defaultValue{ defaultValue_ }
        , m_value{ defaultValue_ }
        , m_prevValue{ defaultValue_ }
    {
    }
    virtual ~Indicator() = default;

    void Reset() override {
        m_prevValue = m_defaultValue;
        m_value = m_defaultValue;
        if (m_dirty_mask_ptr) {
            *m_dirty_mask_ptr &= ~KeyBit();
        }
    }
    bool IsDirty() const override {
        return m_dirty_mask_ptr && ((*m_dirty_mask_ptr & KeyBit()) != 0ULL);
    }
    bool ShouldTrigger() const override { return false; }  // Stage B: Passive by default (no trigger)

    void SetDirtyMaskPointer(uint64_t* maskPtr) override {
        m_dirty_mask_ptr = maskPtr;
    }

    // Dual-write target during Phase II migration (indicator-manager-dod-soa
    // plan, Task 4). nullptr = not yet wired into IndicatorManager::m_packed.
    void SetPackedSlotPointer(int8_t* slot) { m_packedSlotI8 = slot; }
    // Task 9 fix (Finding 2): wires the companion prev-slot (see
    // m_packedPrevSlotI8 above). Must be wired to the SAME position's
    // RawPrevI8Pointer() as SetPackedSlotPointer()'s RawI8Pointer() call —
    // IndicatorManager's constructor wires both together for every Int8-block
    // key.
    void SetPackedPrevSlotPointer(int8_t* slot) { m_packedPrevSlotI8 = slot; }

    void Update(const T& newValue) {

        if (newValue != m_value) {
            m_prevValue = m_value;  // Always sync - clears dirty state naturally
            m_value = newValue;
            if (m_dirty_mask_ptr) {
                *m_dirty_mask_ptr |= KeyBit();
            }
            if (m_packedSlotI8) {
                // Task 9 fix (Finding 2): shift current into prev BEFORE
                // overwriting current, mirroring IndicatorPackedState::SetI8's
                // own m_prevI8[pos] = m_currentI8[pos] ordering exactly — this
                // is the read-then-write step a bare pointer store to
                // *m_packedSlotI8 alone could never perform.
                if (m_packedPrevSlotI8) {
                    *m_packedPrevSlotI8 = *m_packedSlotI8;
                }
                *m_packedSlotI8 = static_cast<int8_t>(newValue);
            }
        }
    }

    IndicatorKey Key() const override { return m_key; }
    int intValue() const override { return static_cast<int>(m_value); }
    T Value() const { return m_value; }

    // Extract int8_t value and clear dirty flag (atomic FlatBuffer serialization)
    int8_t ExtractInt8AndClearDirty() override {
        int8_t value = static_cast<int8_t>(m_value);
        if (m_dirty_mask_ptr) {
            *m_dirty_mask_ptr &= ~KeyBit();
        }
        return value;
    }

    // FlatBuffer serialization - TrainingEvent (wide table, 60+ fields)
    // Uses Object API (TrainingEventT) - plain struct assignments ("Elite" pattern)
    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const override
    {
        // Map JsonKey() to TrainingEvent field using helper function
        MapIndicatorKeyToTrainingEvent(Key(), intValue(), event);
        // Clear dirty bit after extraction so the latch resets for next real change.
        // m_dirty_mask_ptr is a non-const pointer — writing through it from const is valid.
        if (m_dirty_mask_ptr) {
            *m_dirty_mask_ptr &= ~KeyBit();
        }
    }
};

// Helper function for field mapping (defined in IndicatorMappings_generated.h)
// Auto-generated by scripts/generate_flatbuffer_mappings.py from schema
inline void MapIndicatorKeyToTrainingEvent(
    IndicatorKey key,
    int intValue,
    MTS::Training::TrainingEventT& event
)
{
    // Handle top-level fields (HMM, Climate) which are NOT in the struct
    if (key == IndicatorKey::HMM_STATE) {
        event.hmm_state = static_cast<int8_t>(intValue);
        return;
    }
    if (key == IndicatorKey::MARKET_CLIMATE) {
        event.market_climate = static_cast<int8_t>(intValue);
        return;
    }

    // Ensure the zero-copy struct is allocated
    if (!event.indicators) {
        event.indicators = std::make_unique<MTS::Schema::IndicatorState>();
    }
    auto& ind = *event.indicators;

    switch (key) {
        case IndicatorKey::LONG_MACD: ind.mutate_long_macd(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::LONG_FI13_SIGNAL: ind.mutate_long_fi13_signal(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::LONG_MACD_DIVERGENCE: ind.mutate_long_macd_divergence(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::LONG_IMP: ind.mutate_long_imp(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::INTERM_STOCHASTIC: ind.mutate_interm_stochastic(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::RASCHKE_STRATEGY_SETUP: ind.mutate_raschke_strategy_setup(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::RASCHKE_TACTICAL_TRIGGER: ind.mutate_raschke_tactical_trigger(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::RSI: ind.mutate_rsi(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::INTERM_FI2_SIGNAL: ind.mutate_interm_fi2_signal(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::EMA_PROXIMITY: ind.mutate_ema_proximity(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::PRICE_METRICS: ind.mutate_price_metrics(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::INTERM_MACD_DIVERGENCE: ind.mutate_interm_macd_divergence(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::INTERM_IMP: ind.mutate_interm_imp(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::INTERM_MACD: ind.mutate_interm_macd(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::STRUCTURE_TEST: ind.mutate_structure_test(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::VOLUME_SIGNAL: ind.mutate_volume_signal(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::ATR_PROXIMITY: ind.mutate_atr_proximity(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::DAILY_BIAS:
            ind.mutate_daily_bias(static_cast<int8_t>(intValue));
            ind.mutate_daily_bias_enum(static_cast<int8_t>(intValue));
            break;
        case IndicatorKey::KANGAROO_TAIL: ind.mutate_kangaroo_tail(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::TURTLE_SOUP: ind.mutate_turtle_soup(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::MOMENTUM_PINBALL: ind.mutate_momentum_pinball(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::ELDER_BREAKOUT: ind.mutate_elder_breakout(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::NR7: ind.mutate_nr7(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::NH_NL_SIGNAL: ind.mutate_nh_nl_signal(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::OSCILLATOR_310: ind.mutate_oscillator_310(static_cast<int8_t>(intValue)); break;
        case IndicatorKey::TIME_OF_DAY: ind.mutate_time_of_day(static_cast<int8_t>(intValue)); break;
        default: break;
    }
}

class Impulse : public Indicator<ImpulseEnum>
{
public:
    Impulse(IndicatorKey key_) : Indicator(key_, ImpulseEnum::UNDEFINED) { }
    void SetFromColor(int color, int prevColor, float maDiff, float macdDiff, float atr);

    // v5.0: Impulse color change is always a trigger (critical state)
    bool ShouldTrigger() const override { return IsDirty(); }

    // v5.2 Institutional-grade metrics (derived from raw Impulse state)
    uint8_t RunLength()      const { return m_runLength; }
    float   Magnitude()      const { return m_magnitude; }
    float   Fatigue()        const { return m_fatigue; }
    float   TransitionRate() const { return m_transitionRate; }

    // FlatBuffer serialization — export enum + run_length (only Python-consumed field)
    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const override {
        Indicator<ImpulseEnum>::AddToTrainingEventFB(event);
        if (!event.indicators) event.indicators = std::make_unique<MTS::Schema::IndicatorState>();
        event.indicators->mutate_impulse_run_length(static_cast<int8_t>(m_runLength));
    }

private:
    uint8_t  m_runLength      = 0;      // Consecutive bars in same color bucket
    float    m_magnitude      = 0.0f;   // ATR-normalized momentum strength [-1, +1]
    float    m_fatigue        = 0.0f;   // Δ(magnitude): positive = accelerating, negative = fading
    float    m_transitionRate = 0.0f;   // Fraction of recent bars with color change [0, 1]

    // Running state (magnitude/runLength/colorHistory carried between ticks),
    // extracted to ImpulseState (IndicatorComputations.h) so ComputeImpulse()
    // can be a pure free function taking explicit state (indicator-manager-dod-soa
    // plan, Task 7 — same treatment as Macd::m_macdState, Task 6).
    ImpulseState m_impulseState;
};

class Macd : public Indicator<MacdEnum>
{
public:
    Macd(IndicatorKey key_) : Indicator(key_, MacdEnum::AT_ZERO) { }

    // indicator-manager-dod-soa plan, Task 6: the actual classification/
    // z-score computation now lives in the pure, standalone-testable
    // ComputeMacd()/MacdIsX() free functions (IndicatorComputations.h).
    // SetFromChart stays as the ACSIL-only adapter — it still owns m_value/
    // m_prevMacdValue/m_zScore/the dirty-mask-driving Update() call, since
    // several other call sites (TripleScreen2/3.cpp, StudyHelperFunctions.cpp,
    // EventSerializer.cpp, BackTesterStudy.cpp) read MacdValue()/ZScore()/
    // Value()/ShouldTrigger() from this same object independently of the
    // SetFromChart producer call site — verified by grep during the Task 6
    // audit, not assumed. Returns the MacdResult so the producer call site can
    // additionally exercise IndicatorManager::SetValue<Key>() (the packed-array
    // proof of pattern) without disturbing any of those other readers.
    MacdResult SetFromChart(SCSubgraphRef MACD_Diff, int Index);
    double MacdValue() const { return m_macdValue; }
    double PreviousMacdValue() const { return m_prevMacdValue; }

    /// Robust z-score of MACD histogram (median/MAD, Taleb fat-tail safe).
    float ZScore() const { return m_zScore; }

    // v5.4: Serialize enum + robust z-score magnitude (mirrors FI2Signal pattern)
    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const override {
        Indicator<MacdEnum>::AddToTrainingEventFB(event);
        event.interm_macd_norm = m_zScore;
    }

private:
    double m_macdValue = 0.0;
    double m_prevMacdValue = 0.0;
    float m_zScore{0.0f};

    // Running z-score history, extracted to MacdState (IndicatorComputations.h)
    // so ComputeMacd() can be a pure free function taking explicit state.
    MacdState m_macdState;
};

class Stochastic : public Indicator<StochasticEnum>
{
public:
    Stochastic(IndicatorKey key_) : Indicator(key_, StochasticEnum::UNDEFINED) { }

    // v5.0 Tier 2: Trigger only on threshold crossing (entering/exiting overbought/oversold)
    bool ShouldTrigger() const override {
        // Crossing into extreme zones
        bool enteringOverbought = (m_prevValue != StochasticEnum::OVER_BOUGHT && m_value == StochasticEnum::OVER_BOUGHT);
        bool enteringOversold = (m_prevValue != StochasticEnum::OVER_SOLD && m_value == StochasticEnum::OVER_SOLD);

        // Crossing out of extreme zones back to normal
        bool exitingOverbought = (m_prevValue == StochasticEnum::OVER_BOUGHT && m_value != StochasticEnum::OVER_BOUGHT);
        bool exitingOversold = (m_prevValue == StochasticEnum::OVER_SOLD && m_value != StochasticEnum::OVER_SOLD);

        // Divergence detection always triggers (high signal)
        bool divergence = (m_value == StochasticEnum::BULLISH_DIVERGENCE || m_value == StochasticEnum::BEARISH_DIVERGENCE);

        return enteringOverbought || enteringOversold || exitingOverbought || exitingOversold || divergence;
    }
};

class Divergence : public Indicator<DivergenceEnum>
{
public:
    Divergence(IndicatorKey key_) : Indicator(key_, DivergenceEnum::NONE) { }
};

/**
 * MACDDivergence: Indicator wrapper for Dr. Alexander Elder's MACD-Histogram divergence.
 * Uses MACDDivergenceEnum state machine for proper divergence tracking.
 *
 * This class wraps the MACDDivergenceEnum values and manages dirty state tracking
 * for the IndicatorManager payload system. The actual divergence detection logic
 * lives in DetectElderMACDDivergence() (StudyHelperFunctions.cpp).
 *
 * Note: State tracking (peaks, troughs, zero crosses) is handled by MACDDivergenceState
 * struct in StudyHelperFunctions.h, not stored in this indicator class.
 */
class MACDDivergence : public Indicator<MACDDivergenceEnum>
{
public:
    MACDDivergence(IndicatorKey key_) : Indicator(key_, MACDDivergenceEnum::NONE) { }
};

class Side : public Indicator<TradeSideEnum>
{
public:
    Side(IndicatorKey key_) : Indicator(key_, TradeSideEnum::FLAT) {}

    // v5.0: Position side change is always a trigger (critical state)
    bool ShouldTrigger() const override { return IsDirty(); }
};

class IntermediateMarketAction : public Indicator<PriceActionEnum>
{
protected:
    struct MarketData {
        float ema = 0.0f;
        float fastEma = 0.0f;
        float upperChan = 0.0f;
        float lowerChan = 0.0f;

        bool operator!=(const MarketData& other) const {
            constexpr float THRESHOLD = 0.25f;

            // Early returns avoid unnecessary abs() calls (called every bar)
            if (std::abs(ema - other.ema) > THRESHOLD) [[likely]] return true;
            if (std::abs(fastEma - other.fastEma) > THRESHOLD) return true;
            if (std::abs(upperChan - other.upperChan) > THRESHOLD) return true;
            return std::abs(lowerChan - other.lowerChan) > THRESHOLD;
        }
    };
    MarketData m_data;
    MarketData m_prevData;

    // Structural swing tracking (Option B: numeric 60-min pivots)
    // Confirmed swing pivots from sc.IsSwingHigh/Low with SWING_LENGTH=5
    float m_swingHigh = 0.0f;
    float m_swingLow  = 0.0f;
    float m_prevSwingHigh = 0.0f;
    float m_prevSwingLow  = 0.0f;

    // Keltner band rejection detection
    // True when price touched band within last 3 bars then reversed away
    bool m_upperRejected = false;
    bool m_lowerRejected = false;

public:
    IntermediateMarketAction(IndicatorKey key_);
    virtual ~IntermediateMarketAction() = default;

    void Reset() override;

    void setFastEma(float ema_, float topBand_, float bottomBand_);
    void setEma(float ema_);

    // Structural swing setters (called from TS2 KeltnerChannel study)
    void updateSwingHigh(float price);
    void updateSwingLow(float price);
    void setKeltnerRejection(bool upperRejected, bool lowerRejected);

    float Channel() const { return m_data.upperChan - m_data.lowerChan; }
    float ema() const { return m_data.ema; }
    float fastEma() const { return m_data.fastEma; }
    float upperChan() const { return m_data.upperChan; }
    float lowerChan() const { return m_data.lowerChan; }

    float prevEma() const { return m_prevData.ema; }
    float prevFastEma() const { return m_prevData.fastEma; }
    float prevUpperChan() const { return m_prevData.upperChan; }
    float prevLowerChan() const { return m_prevData.lowerChan; }
    float atr() const { return (m_data.upperChan - m_data.ema) / 2.0f; }

    // Structural level accessors (for PositionManager / target calculation)
    float swingHigh() const { return m_swingHigh; }
    float swingLow() const { return m_swingLow; }
    float prevSwingHigh() const { return m_prevSwingHigh; }
    float prevSwingLow() const { return m_prevSwingLow; }
    bool upperRejected() const { return m_upperRejected; }
    bool lowerRejected() const { return m_lowerRejected; }
};

class FI13Signal : public Indicator<FI13Enum>
{
public:
    FI13Signal(IndicatorKey key_);
    void setFromChart(double forceValue);

    float ZScore() const { return m_zScore; }

    // Companion Float32-block dual-write target (indicator-manager-dod-soa
    // plan, Task 4). Brings the base template's int8 overload into scope
    // alongside this float one. Written inside setFromChart() (Indicator.cpp),
    // where m_zScore is actually computed.
    using Indicator<FI13Enum>::SetPackedSlotPointer;
    void SetPackedSlotPointer(float* slot) { m_packedSlotF32 = slot; }

    // v5.6: Serialize enum + robust z-score of FI-13 EMA magnitude
    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const override {
        Indicator<FI13Enum>::AddToTrainingEventFB(event);
        if (!event.indicators) event.indicators = std::make_unique<MTS::Schema::IndicatorState>();
        event.indicators->mutate_long_fi13_norm(m_zScore);
        event.long_fi13_norm = m_zScore;
    }

private:
    // Robust z-score state: median/MAD (Taleb-consistent, outlier-resistant)
    // 50-bar lookback for 240-min TS1 ≈ 6 weeks of daily trend
    static constexpr int kLookback = 50;
    static constexpr double kMADConsistency = 1.4826;

    std::array<double, kLookback> m_forceHistory{};
    int   m_historyCount{0};
    int   m_historyIdx{0};
    float m_zScore{0.0f};
    float* m_packedSlotF32 = nullptr;
};

class FI2Signal : public Indicator<FI2Enum>
{
public:
    FI2Signal(IndicatorKey key_);
    void setFromChart(const double value, MacdEnum longMacd);

    float ZScore() const { return m_zScore; }

    // Companion Float32-block dual-write target (indicator-manager-dod-soa
    // plan, Task 4). Brings the base template's int8 overload into scope
    // alongside this float one. Written inside setFromChart() (Indicator.cpp),
    // where m_zScore is actually computed.
    using Indicator<FI2Enum>::SetPackedSlotPointer;
    void SetPackedSlotPointer(float* slot) { m_packedSlotF32 = slot; }

    // v5.3: Serialize enum + robust z-score magnitude
    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const override {
        Indicator<FI2Enum>::AddToTrainingEventFB(event);
        if (!event.indicators) event.indicators = std::make_unique<MTS::Schema::IndicatorState>();
        event.indicators->mutate_interm_fi2_norm(m_zScore);
    }

private:
    double m_force{0.0};
    const double m_threshold{0.0001};

    // Robust z-score state: median/MAD (Taleb-consistent, outlier-resistant)
    // 50-bar lookback for 60-min TS2 ≈ 3 trading days
    static constexpr int kLookback = 50;
    // Consistency factor: MAD * 1.4826 ≈ σ for Gaussian data,
    // but MAD doesn't inflate on fat-tail events.
    static constexpr double kMADConsistency = 1.4826;

    std::array<double, kLookback> m_forceHistory{};
    int   m_historyCount{0};
    int   m_historyIdx{0};
    float m_zScore{0.0f};
    float* m_packedSlotF32 = nullptr;
};

class RaschkeStrategyIndicator : public Indicator<RaschkeStrategySetup>
{
public:
    RaschkeStrategyIndicator(IndicatorKey key_) : Indicator(key_, RaschkeStrategySetup::NONE) { }
};

class RaschkeTacticalIndicator : public Indicator<RaschkeTacticalTrigger>
{
public:
    RaschkeTacticalIndicator(IndicatorKey key_) : Indicator(key_, RaschkeTacticalTrigger::NONE) { }
};

// Working-state exception (indicator-manager-dod-soa plan, Task 8; design spec
// §3.4): only the published enum (packed Int8) and m_qualityScore (packed
// Float32) are read by any live caller outside this class (confirmed via a
// full-repo grep of every getter below at Task 8 time — none are called from
// outside KangarooTail itself). m_tailToBodyRatio/m_tailToATR/m_closePosition/
// m_atSupportLevel/m_atResistanceLevel are set via SetMetrics()/SetContext()
// from TripleScreen3.cpp but never read back; they stay as plain member
// fields here rather than moving into the packed arrays or a dedicated
// compute-function struct (there is no compute function that consumes them —
// DetectKangarooTail in IndicatorComputations.h only produces the enum +
// quality/ratio/position outputs; the support/resistance context is computed
// independently in TripleScreen3.cpp and passed in via SetContext()).
class KangarooTail : public Indicator<KangarooTailEnum>
{
public:
    KangarooTail(IndicatorKey key_) : Indicator(key_, KangarooTailEnum::NONE) { }

    // v5.0: Trigger on pattern entry OR exit (prevents ghost patterns)
    bool ShouldTrigger() const override {
        bool entered = (m_prevValue == KangarooTailEnum::NONE && m_value != KangarooTailEnum::NONE);
        bool exited = (m_prevValue != KangarooTailEnum::NONE && m_value == KangarooTailEnum::NONE);
        return entered || exited;
    }

    // Pattern detection with context
    float TailToBodyRatio() const { return m_tailToBodyRatio; }
    float TailToATR() const { return m_tailToATR; }
    float ClosePosition() const { return m_closePosition; }  // 0.0-1.0 where close is in bar range
    float QualityScore() const { return m_qualityScore; }

    bool AtSupportLevel() const { return m_atSupportLevel; }
    bool AtResistanceLevel() const { return m_atResistanceLevel; }

    // Companion Float32-block dual-write target (indicator-manager-dod-soa
    // plan, Task 4). Brings the base template's int8 overload into scope
    // alongside this float one.
    using Indicator<KangarooTailEnum>::SetPackedSlotPointer;
    void SetPackedSlotPointer(float* slot) { m_packedSlotF32 = slot; }

    void SetMetrics(float tailToBody, float tailToATR, float closePos, float quality) {
        m_tailToBodyRatio = tailToBody;
        m_tailToATR = tailToATR;
        m_closePosition = closePos;
        m_qualityScore = quality;
        if (m_packedSlotF32) { *m_packedSlotF32 = m_qualityScore; }
    }

    void SetContext(bool atSupport, bool atResistance) {
        m_atSupportLevel = atSupport;
        m_atResistanceLevel = atResistance;
    }

    // FlatBuffer serialization - Export quality score to TrainingEvent
    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const override
    {
        // Map JsonKey() to TrainingEvent field using helper function
        MapIndicatorKeyToTrainingEvent(Key(), intValue(), event);
        // Export quality score to FlatBuffer field
        if (!event.indicators) event.indicators = std::make_unique<MTS::Schema::IndicatorState>();
        event.indicators->mutate_kangaroo_tail_quality(m_qualityScore);
    }

private:
    float m_tailToBodyRatio = 0.0f;
    float m_tailToATR = 0.0f;
    float m_closePosition = 0.5f;  // 0=low, 1=high
    float m_qualityScore = 0.0f;
    bool m_atSupportLevel = false;
    bool m_atResistanceLevel = false;
    float* m_packedSlotF32 = nullptr;
};

// Working-state exception (indicator-manager-dod-soa plan, Task 8; design spec
// §3.4): only the published enum and m_qualityScore are read by any live
// caller outside this class (confirmed via full-repo grep). m_penetrationDistance/
// m_closeDistance/m_closePosition/m_atDailyHigh/m_atDailyLow/m_hurst/
// m_screenAligned are set via SetMetrics()/SetContext() but never read back;
// same rationale as KangarooTail above — no compute function consumes them as
// a parameter, so no struct is introduced for them.
class TurtleSoup : public Indicator<TurtleSoupEnum>
{
public:
    TurtleSoup(IndicatorKey key_) : Indicator(key_, TurtleSoupEnum::NONE) { }

    // v5.0: Trigger on pattern entry OR exit (prevents ghost patterns)
    bool ShouldTrigger() const override {
        bool entered = (m_prevValue == TurtleSoupEnum::NONE && m_value != TurtleSoupEnum::NONE);
        bool exited = (m_prevValue != TurtleSoupEnum::NONE && m_value == TurtleSoupEnum::NONE);
        return entered || exited;
    }

    // Pattern detection with context
    float PenetrationDistance() const { return m_penetrationDistance; }  // ATR multiple of false breakout
    float CloseDistance() const { return m_closeDistance; }  // ATR multiple of close-back-inside distance
    float ClosePosition() const { return m_closePosition; }  // 0.0-1.0 where close is in bar range
    float QualityScore() const { return m_qualityScore; }  // 0.0-1.0 overall pattern quality

    bool AtDailyHigh() const { return m_atDailyHigh; }  // Near previous day high (resistance)
    bool AtDailyLow() const { return m_atDailyLow; }  // Near previous day low (support)
    float Hurst() const { return m_hurst; }  // Hurst exponent at pattern formation
    bool ScreenAligned() const { return m_screenAligned; }  // Screen1 regime matches pattern direction

    // Companion Float32-block dual-write target (indicator-manager-dod-soa
    // plan, Task 4). Brings the base template's int8 overload into scope
    // alongside this float one.
    using Indicator<TurtleSoupEnum>::SetPackedSlotPointer;
    void SetPackedSlotPointer(float* slot) { m_packedSlotF32 = slot; }

    void SetMetrics(float penetrationDist, float closeDist, float closePos, float quality) {
        m_penetrationDistance = penetrationDist;
        m_closeDistance = closeDist;
        m_closePosition = closePos;
        m_qualityScore = quality;
        if (m_packedSlotF32) { *m_packedSlotF32 = m_qualityScore; }
    }

    void SetContext(bool atDailyHigh, bool atDailyLow, float hurst, bool screenAligned) {
        m_atDailyHigh = atDailyHigh;
        m_atDailyLow = atDailyLow;
        m_hurst = hurst;
        m_screenAligned = screenAligned;
    }

    // FlatBuffer serialization - Export quality score to TrainingEvent
    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const override {
        // Export enum value using base class mapper
        Indicator<TurtleSoupEnum>::AddToTrainingEventFB(event);
        // Export quality score to FlatBuffer field
        if (!event.indicators) event.indicators = std::make_unique<MTS::Schema::IndicatorState>();
        event.indicators->mutate_turtle_soup_quality(m_qualityScore);
    }

private:
    float m_penetrationDistance = 0.0f;  // How far price broke beyond 4-day extreme (× ATR)
    float m_closeDistance = 0.0f;  // How far close came back inside range (× ATR)
    float m_closePosition = 0.5f;  // 0=low, 1=high (where in bar range close occurred)
    float m_qualityScore = 0.0f;  // 0.0-1.0 combined quality metric
    bool m_atDailyHigh = false;  // Pattern at resistance (bearish context)
    bool m_atDailyLow = false;  // Pattern at support (bullish context)
    float m_hurst = 0.0f;  // Hurst exponent (>0.55 = persistent, avoid soup in strong trend)
    bool m_screenAligned = false;  // Screen1 regime supports pattern direction
    float* m_packedSlotF32 = nullptr;
};

// Working-state exception (indicator-manager-dod-soa plan, Task 8; design spec
// §3.4): only the published enum and m_qualityScore are read by any live
// caller outside this class (confirmed via full-repo grep). m_rsiDelta/
// m_stochDepth/m_impulseJustChanged/m_volumeSpike/m_hasFI2Pullback/
// m_macdHRising/m_screenAligned are set via SetMetrics()/SetContext() but
// never read back; same rationale as KangarooTail above.
class MomentumPinball : public Indicator<MomentumPinballEnum>
{
public:
    MomentumPinball(IndicatorKey key_) : Indicator(key_, MomentumPinballEnum::NONE) { }

    // v5.0: Trigger on pattern entry OR exit (prevents ghost patterns)
    bool ShouldTrigger() const override {
        bool entered = (m_prevValue == MomentumPinballEnum::NONE && m_value != MomentumPinballEnum::NONE);
        bool exited = (m_prevValue != MomentumPinballEnum::NONE && m_value == MomentumPinballEnum::NONE);
        return entered || exited;
    }

    // Pattern metrics
    float RSIDelta() const { return m_rsiDelta; }  // RSI3 - RSI10 (positive = bullish momentum)
    float StochDepth() const { return m_stochDepth; }  // How deep into oversold/overbought (0-100)
    bool ImpulseJustChanged() const { return m_impulseJustChanged; }  // Impulse color changed this bar
    float VolumeSpike() const { return m_volumeSpike; }  // Volume / AvgVolume (1.0 = average)
    float QualityScore() const { return m_qualityScore; }  // 0.0-1.0 overall pattern quality

    // Context
    bool HasFI2Pullback() const { return m_hasFI2Pullback; }  // FI2 shows pullback/rally
    bool MacdHRising() const { return m_macdHRising; }  // MACD-H rising (bullish) or falling (bearish)
    bool ScreenAligned() const { return m_screenAligned; }  // Screen1 regime matches pattern direction

    // Companion Float32-block dual-write target (indicator-manager-dod-soa
    // plan, Task 4). Brings the base template's int8 overload into scope
    // alongside this float one.
    using Indicator<MomentumPinballEnum>::SetPackedSlotPointer;
    void SetPackedSlotPointer(float* slot) { m_packedSlotF32 = slot; }

    void SetMetrics(float rsiDelta, float stochDepth, bool impulseJustChanged, float volumeSpike, float quality) {
        m_rsiDelta = rsiDelta;
        m_stochDepth = stochDepth;
        m_impulseJustChanged = impulseJustChanged;
        m_volumeSpike = volumeSpike;
        m_qualityScore = quality;
        if (m_packedSlotF32) { *m_packedSlotF32 = m_qualityScore; }
    }

    void SetContext(bool hasFI2Pullback, bool macdHRising, bool screenAligned) {
        m_hasFI2Pullback = hasFI2Pullback;
        m_macdHRising = macdHRising;
        m_screenAligned = screenAligned;
    }

    // FlatBuffer serialization - Export quality score to TrainingEvent
    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const override {
        // Export enum value using base class mapper
        Indicator<MomentumPinballEnum>::AddToTrainingEventFB(event);
        // Export quality score to FlatBuffer field
        if (!event.indicators) event.indicators = std::make_unique<MTS::Schema::IndicatorState>();
        event.indicators->mutate_momentum_pinball_quality(m_qualityScore);
    }

private:
    float m_rsiDelta = 0.0f;  // RSI3 - RSI10 (strength of momentum shift)
    float m_stochDepth = 50.0f;  // Stochastic value (how extreme)
    bool m_impulseJustChanged = false;  // Did Impulse change color this bar?
    float m_volumeSpike = 1.0f;  // Current volume relative to average
    float m_qualityScore = 0.0f;  // 0.0-1.0 combined quality metric
    bool m_hasFI2Pullback = false;  // FI2 shows pullback (bullish) or rally (bearish)
    bool m_macdHRising = false;  // MACD-H momentum direction
    bool m_screenAligned = false;  // Screen1 regime supports pattern direction
    float* m_packedSlotF32 = nullptr;
};

// Working-state exception (indicator-manager-dod-soa plan, Task 8; design spec
// §3.4): only the published enum and m_qualityScore are read by any live
// caller outside this class (confirmed via full-repo grep). m_breakoutDistance/
// m_hurst/m_volumeSpike/m_consolidationBars/m_isGap/m_channelSqueeze/
// m_impulseAligned/m_screenAligned are set via SetMetrics()/SetContext() but
// never read back; same rationale as KangarooTail above.
class ElderBreakout : public Indicator<ElderBreakoutEnum>
{
public:
    ElderBreakout(IndicatorKey key_) : Indicator(key_, ElderBreakoutEnum::NONE) { }

    // v5.0: Trigger on pattern entry OR exit (prevents ghost patterns)
    bool ShouldTrigger() const override {
        bool entered = (m_prevValue == ElderBreakoutEnum::NONE && m_value != ElderBreakoutEnum::NONE);
        bool exited = (m_prevValue != ElderBreakoutEnum::NONE && m_value == ElderBreakoutEnum::NONE);
        return entered || exited;
    }

    // Pattern metrics
    float BreakoutDistance() const { return m_breakoutDistance; }  // Distance beyond band (× ATR)
    float Hurst() const { return m_hurst; }  // Trend persistence (higher = stronger persistence)
    float VolumeSpike() const { return m_volumeSpike; }  // Volume / AvgVolume (1.0 = average)
    int ConsolidationBars() const { return m_consolidationBars; }  // Bars spent consolidating before breakout
    bool IsGap() const { return m_isGap; }  // Did price gap beyond the band?
    float QualityScore() const { return m_qualityScore; }  // 0.0-1.0 overall pattern quality

    // Context
    bool ChannelSqueeze() const { return m_channelSqueeze; }  // Keltner bands narrowing (ATR compression)
    bool ImpulseAligned() const { return m_impulseAligned; }  // Impulse color matches breakout direction
    bool ScreenAligned() const { return m_screenAligned; }  // Screen1 regime matches breakout direction

    // Companion Float32-block dual-write target (indicator-manager-dod-soa
    // plan, Task 4). Brings the base template's int8 overload into scope
    // alongside this float one.
    using Indicator<ElderBreakoutEnum>::SetPackedSlotPointer;
    void SetPackedSlotPointer(float* slot) { m_packedSlotF32 = slot; }

    void SetMetrics(float breakoutDist, float hurst, float volumeSpike, int consolidationBars, bool isGap, float quality) {
        m_breakoutDistance = breakoutDist;
        m_hurst = hurst;
        m_volumeSpike = volumeSpike;
        m_consolidationBars = consolidationBars;
        m_isGap = isGap;
        m_qualityScore = quality;
        if (m_packedSlotF32) { *m_packedSlotF32 = m_qualityScore; }
    }

    void SetContext(bool channelSqueeze, bool impulseAligned, bool screenAligned) {
        m_channelSqueeze = channelSqueeze;
        m_impulseAligned = impulseAligned;
        m_screenAligned = screenAligned;
    }

    // FlatBuffer serialization - Export quality score to TrainingEvent
    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const override {
        // Export enum value using base class mapper
        Indicator<ElderBreakoutEnum>::AddToTrainingEventFB(event);
        // Export quality score to FlatBuffer field
        if (!event.indicators) event.indicators = std::make_unique<MTS::Schema::IndicatorState>();
        event.indicators->mutate_elder_breakout_quality(m_qualityScore);
    }

private:
    float m_breakoutDistance = 0.0f;  // Distance beyond Keltner band (× ATR)
    float m_hurst = 0.0f;  // Hurst exponent at breakout (>0.55 = persistent, >0.65 = strong)
    float m_volumeSpike = 1.0f;  // Current volume relative to 10-bar average
    int m_consolidationBars = 0;  // Number of bars spent near band before breakout
    bool m_isGap = false;  // True if price gapped beyond band
    float m_qualityScore = 0.0f;  // 0.0-1.0 combined quality metric
    bool m_channelSqueeze = false;  // Keltner bands narrowing (ATR declining)
    bool m_impulseAligned = false;  // Impulse color supports breakout direction
    bool m_screenAligned = false;  // Screen1 regime supports breakout direction
    float* m_packedSlotF32 = nullptr;
};

// Working-state exception (indicator-manager-dod-soa plan, Task 8; design spec
// §3.4): only the published enum and m_qualityScore are read by any live
// caller outside this class (confirmed via full-repo grep). m_currentRange/
// m_avg7BarRange/m_rangePercentile/m_volumeSpike/m_consolidationBars/
// m_volumeDecline/m_impulseAligned/m_screenAligned are set via SetMetrics()/
// SetContext() but never read back; same rationale as KangarooTail above.
class NR7 : public Indicator<NR7Enum>
{
private:
    // Pattern metrics
    float m_currentRange;           // Current bar range (high - low)
    float m_avg7BarRange;           // Average range over past 7 bars
    float m_rangePercentile;        // Current range as % of 7-bar average (0.0-1.0)
    float m_volumeSpike;            // Current volume / average volume
    int m_consolidationBars;        // Bars spent in tight range
    float m_qualityScore;           // 0.0-1.0 quality score

    // Context factors
    bool m_volumeDecline;           // Volume drying up (compression signal)?
    bool m_impulseAligned;          // Impulse positioned to break out?
    bool m_screenAligned;           // Screen1 regime predicts direction?

    // Companion Float32-block dual-write target (indicator-manager-dod-soa
    // plan, Task 4).
    float* m_packedSlotF32 = nullptr;

public:
    NR7(IndicatorKey key_)
        : Indicator(key_, NR7Enum::NONE),
          m_currentRange(0.0f), m_avg7BarRange(0.0f),
          m_rangePercentile(1.0f), m_volumeSpike(1.0f),
          m_consolidationBars(0), m_qualityScore(0.0f),
          m_volumeDecline(false), m_impulseAligned(false),
          m_screenAligned(false) {}

    // Brings the base template's int8 overload into scope alongside this float one.
    using Indicator<NR7Enum>::SetPackedSlotPointer;
    void SetPackedSlotPointer(float* slot) { m_packedSlotF32 = slot; }

    // v5.0: Trigger on pattern entry OR exit (prevents ghost patterns)
    bool ShouldTrigger() const override {
        bool entered = (m_prevValue == NR7Enum::NONE && m_value != NR7Enum::NONE);
        bool exited = (m_prevValue != NR7Enum::NONE && m_value == NR7Enum::NONE);
        return entered || exited;
    }

    // Getters
    float CurrentRange() const { return m_currentRange; }
    float Avg7BarRange() const { return m_avg7BarRange; }
    float RangePercentile() const { return m_rangePercentile; }
    float VolumeSpike() const { return m_volumeSpike; }
    int ConsolidationBars() const { return m_consolidationBars; }
    float QualityScore() const { return m_qualityScore; }
    bool VolumeDecline() const { return m_volumeDecline; }
    bool ImpulseAligned() const { return m_impulseAligned; }
    bool ScreenAligned() const { return m_screenAligned; }

    // Setters
    void SetMetrics(float currentRange, float avg7BarRange, float rangePercentile,
                   float volumeSpike, int consolidationBars, float qualityScore) {
        m_currentRange = currentRange;
        m_avg7BarRange = avg7BarRange;
        m_rangePercentile = rangePercentile;
        m_volumeSpike = volumeSpike;
        m_consolidationBars = consolidationBars;
        m_qualityScore = qualityScore;
        if (m_packedSlotF32) { *m_packedSlotF32 = m_qualityScore; }
    }

    void SetContext(bool volumeDecline, bool impulseAligned, bool screenAligned) {
        m_volumeDecline = volumeDecline;
        m_impulseAligned = impulseAligned;
        m_screenAligned = screenAligned;
    }

    // FlatBuffer serialization - Export quality score to TrainingEvent
    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const override {
        // Export enum value using base class mapper
        Indicator<NR7Enum>::AddToTrainingEventFB(event);
        // Export quality score to FlatBuffer field
        if (!event.indicators) event.indicators = std::make_unique<MTS::Schema::IndicatorState>();
        event.indicators->mutate_nr7_quality(m_qualityScore);
    }
};

class RSIIndicator : public Indicator<RSI>
{
public:
    RSIIndicator(IndicatorKey key_) : Indicator(key_, RSI::NORMAL) { }

    // v5.0 Tier 2: Trigger only on threshold crossing (entering/exiting overbought/oversold)
    bool ShouldTrigger() const override {
        // Crossing into extreme zones (>70 or <30)
        bool enteringOverbought = (m_prevValue != RSI::OVERBOUGHT && m_value == RSI::OVERBOUGHT);
        bool enteringOversold = (m_prevValue != RSI::OVERSOLD && m_value == RSI::OVERSOLD);

        // Crossing out of extreme zones back to normal
        bool exitingOverbought = (m_prevValue == RSI::OVERBOUGHT && m_value != RSI::OVERBOUGHT);
        bool exitingOversold = (m_prevValue == RSI::OVERSOLD && m_value != RSI::OVERSOLD);

        // Divergence detection always triggers (high signal)
        bool divergence = (m_value == RSI::BULLISH_DIVERGENCE || m_value == RSI::BEARISH_DIVERGENCE);

        return enteringOverbought || enteringOversold || exitingOverbought || exitingOversold || divergence;
    }
};

class VolumeIndicator : public Indicator<VolumeEnum>
{
public:
    VolumeIndicator(IndicatorKey key_) : Indicator(key_, VolumeEnum::NORMAL) { }

    // v5.7: Log-volume robust z-score (Taleb-consistent, Mandelbrot log-domain)
    // + self-classification using robust thresholds (replaces Gaussian GetVolumeEnum)
    // + order-flow imbalance: (askVol - bidVol) / totalVol — orthogonal to magnitude
    //
    // Per-tick: refresh order-flow imbalance + self-classify against the current
    // (per-bar, session-aware) z-score baseline.
    void UpdateVolume(float bidVolume, float askVolume);

    // Per closed 15-min bar: push the completed bar's total volume into its SESSION
    // pool (RTH vs overnight) and recompute the robust log-vol z-score. RTH and
    // overnight volume distributions differ materially (intraday U-shape / MDH), so a
    // pooled baseline is bimodal and miscalibrates the z. Deseasonalized, once-per-bar
    // (mirrors the Amihud session-pool design). Call from the TS3 once-per-bar guard.
    void SampleBarVolume(float completedBarVolume, bool isRTH);

    float GetVolumeRatio() const { return m_volumeState.volumeZScore; }
    float GetVolumeImbalance() const { return m_volumeImbalance; }
    // Total traded contracts (bid+ask) behind the current imbalance. The imbalance is a
    // sample proportion (noise ~ 1/sqrt(N)); callers gate on this so a thin book can't
    // produce a spurious ±band breach (min-volume guard).
    float GetLastTotalVolume() const { return m_lastTotalVolume; }

    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const override {
        Indicator<VolumeEnum>::AddToTrainingEventFB(event);
        event.volume_ratio_percent = m_volumeState.volumeZScore;
        event.volume_imbalance = m_volumeImbalance;
    }

private:
    // Running state (session-segregated log-vol history + z-score), extracted
    // to VolumeState (IndicatorComputations.h) so ComputeVolumeBarSample()/
    // ComputeVolumeClassification() can be pure free functions taking explicit
    // state (indicator-manager-dod-soa plan, Task 7 — same treatment as
    // Macd::m_macdState, Task 6).
    VolumeState m_volumeState;
    float m_volumeImbalance = 0.0f;  // (askVol - bidVol) / totalVol, bounded [-1, +1]
    float m_lastTotalVolume = 0.0f;  // bid+ask contracts behind the current imbalance
};

// Session VWAP: cumulative Σ(TP×Vol)/Σ(Vol) with daily reset.
// Provides institutional anchor level + normalized distance for entry quality.
enum class VwapPositionEnum : int8_t {
    BELOW_FAR = 0,      // price < VWAP − 1 ATR (discount zone)
    BELOW_NEAR = 1,     // price < VWAP (mild discount)
    AT_VWAP = 2,        // price ≈ VWAP (within 0.25 ATR)
    ABOVE_NEAR = 3,     // price > VWAP (mild premium)
    ABOVE_FAR = 4       // price > VWAP + 1 ATR (premium zone)
};

class VwapIndicator : public Indicator<VwapPositionEnum>
{
public:
    VwapIndicator(IndicatorKey key_) : Indicator(key_, VwapPositionEnum::AT_VWAP) {}

    /// Call every bar with typical price, bar volume, and current ATR.
    /// Handles session reset internally when newSession is true.
    void UpdateVwap(float typicalPrice, float barVolume, float atr, bool newSession);

    float GetVwapPrice() const { return m_vwapPrice; }
    float GetDistanceNorm() const { return m_distanceNorm; }  // (close − VWAP) / ATR

    // VWAP is for trade-execution only — no FlatBuffer schema field exists.
    // No-op override prevents useless MapIndicatorKeyToTrainingEvent dispatch.
    void AddToTrainingEventFB(MTS::Training::TrainingEventT& /*event*/) const override {}

private:
    double m_cumPriceVol = 0.0;   // Σ(TP × Vol) since session open
    double m_cumVol = 0.0;        // Σ(Vol) since session open
    float  m_vwapPrice = 0.0f;    // Current session VWAP level
    float  m_distanceNorm = 0.0f; // (price − VWAP) / ATR, signed
};

class StructureTestIndicator : public Indicator<StructureTest>
{
public:
    StructureTestIndicator(IndicatorKey key_) : Indicator(key_, StructureTest::NONE) { }

    // Completed-bar structural test (evaluated on sc.Index-1) — the deterministic,
    // model-independent parity anchor for the native TRAP floor. Distinct from the
    // intra-bar Value() that feeds the model's observation vector.
    void SetCompletedValue(StructureTest v) { m_completedValue = v; }
    StructureTest CompletedValue() const { return m_completedValue; }
private:
    StructureTest m_completedValue = StructureTest::NONE;
};

class ATRProximityIndicator : public Indicator<ATRProximityEnum>
{
public:
    ATRProximityIndicator(IndicatorKey key_) : Indicator(key_, ATRProximityEnum::LOW_VOLATILITY) { }

    // Elite v2.0: ATRProximity owns ATR(10) - single source of truth for volatility metrics
    void SetATR10(float atr10) { m_atr10 = atr10; }
    float GetATR10() const { return m_atr10; }

    // v5.0 Tier 2: Trigger on entering/exiting ATR channel bounds or volatility extremes
    bool ShouldTrigger() const override {
        // Entering extreme volatility zones
        bool enteringExtreme = (m_prevValue == ATRProximityEnum::LOW_VOLATILITY &&
                               (m_value == ATRProximityEnum::EXTREME_VOLATILITY ||
                                m_value == ATRProximityEnum::EXTREME_LOW ||
                                m_value == ATRProximityEnum::EXTREME_HIGH));

        // Exiting extreme back to normal
        bool exitingExtreme = ((m_prevValue == ATRProximityEnum::EXTREME_VOLATILITY ||
                                m_prevValue == ATRProximityEnum::EXTREME_LOW ||
                                m_prevValue == ATRProximityEnum::EXTREME_HIGH) &&
                               m_value == ATRProximityEnum::LOW_VOLATILITY);

        // High move state change (channel breach)
        bool channelBreach = (m_prevValue != ATRProximityEnum::HIGH_MOVE && m_value == ATRProximityEnum::HIGH_MOVE) ||
                            (m_prevValue == ATRProximityEnum::HIGH_MOVE && m_value != ATRProximityEnum::HIGH_MOVE);

        return enteringExtreme || exitingExtreme || channelBreach;
    }

    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const override {
        Indicator<ATRProximityEnum>::AddToTrainingEventFB(event);
        event.atr_10 = m_atr10;
    }

private:
    float m_atr10 = 0.0f;  // Cached ATR(10) from Screen3 Keltner calculation
};

class EmaProximityIndicator : public Indicator<EmaProximity>
{
public:
    EmaProximityIndicator(IndicatorKey key_) : Indicator(key_, EmaProximity::ABOVE_STRONG) { }

    // v5.0 Tier 2: Trigger on price crossing EMA-13 line (bullish/bearish crossovers)
    bool ShouldTrigger() const override {
        // Explicit crossover states (CROSS_ABOVE, CROSS_BELOW)
        bool crossover = (m_value == EmaProximity::CROSS_ABOVE || m_value == EmaProximity::CROSS_BELOW);

        // Transitioning from above to below (or vice versa) through AT_EMA
        bool crossingUp = (m_prevValue == EmaProximity::BELOW_TOUCH ||
                          m_prevValue == EmaProximity::BELOW_STRONG ||
                          m_prevValue == EmaProximity::PRICE_BELOW_EMA) &&
                         (m_value == EmaProximity::ABOVE_TOUCH ||
                          m_value == EmaProximity::ABOVE_STRONG ||
                          m_value == EmaProximity::PRICE_ABOVE_EMA);

        bool crossingDown = (m_prevValue == EmaProximity::ABOVE_TOUCH ||
                            m_prevValue == EmaProximity::ABOVE_STRONG ||
                            m_prevValue == EmaProximity::PRICE_ABOVE_EMA) &&
                           (m_value == EmaProximity::BELOW_TOUCH ||
                            m_value == EmaProximity::BELOW_STRONG ||
                            m_value == EmaProximity::PRICE_BELOW_EMA);

        return crossover || crossingUp || crossingDown;
    }
};

class PriceMetricsIndicator : public Indicator<PriceMetrics>
{
public:
    PriceMetricsIndicator(IndicatorKey key_) : Indicator(key_, PriceMetrics::NORMAL) { }
    void SetOHLC(float open, float high, float low, float close);

    // v5.5: Where close sits within bar range [0,1] — tactical micro-pressure signal
    float GetClosePercentile() const { return m_closePercentile; }

    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const override {
        Indicator<PriceMetrics>::AddToTrainingEventFB(event);
        event.close_percentile = m_closePercentile;
    }

private:
    float m_open = 0.0f;
    float m_high = 0.0f;
    float m_low = 0.0f;
    float m_close = 0.0f;
    float m_closePercentile = 0.0f;  // (close - low) / (high - low)
};

class MarketSymbolIndicator : public Indicator<MarketSymbol>
{
public:
    MarketSymbolIndicator(IndicatorKey key_) : Indicator(key_, MarketSymbol::MES) { }
};

class DailyBiasIndicator : public Indicator<DailyBiasEnum> {
public:
    DailyBiasIndicator(IndicatorKey key) : Indicator(key, DailyBiasEnum::PHYSICS_VETO_RANDOM_WALK) {}
};

// Elite v2.6: Hurst Exponent (Market Physics Memory)
// Defined locally to decouple from schema dependencies while maintaining int8 compatibility
enum class HurstExponentEnum : int8_t {
    RANDOM_WALK = 0,             // H ~ 0.5 (Efficient Market)
    ANTI_PERSISTENT_STRONG = 1,  // H < 0.25 (Aggressive Mean Reversion)
    ANTI_PERSISTENT_WEAK = 2,    // H < 0.40 (Weak Mean Reversion)
    PERSISTENT_WEAK = 3,         // H > 0.60 (Weak Trend)
    PERSISTENT_STRONG = 4        // H > 0.75 (Strong Trend)
};

class HurstExponentIndicator : public Indicator<HurstExponentEnum> {
public:
    HurstExponentIndicator(IndicatorKey key) : Indicator(key, HurstExponentEnum::RANDOM_WALK) {}


    // Elite v2.6: Hurst Exponent (Market Physics Memory)
    // Institutional-grade DFA implementation with persistent buffers
    // Defined locally to decouple from schema dependencies while maintaining int8 compatibility
    float Calculate(SCStudyInterfaceRef sc, SCFloatArrayRef InputData, int length, int minScale);

    // Institutional-Grade Regime Classification
    // Converts raw R/S number into actionable market physics state
    void SetFromFloat(float hurstValue) {
        HurstExponentEnum state;

        if (hurstValue < 0.40f) {
            // Mean Reverting (Pink Noise) - Price covers less distance than random walk
            if (hurstValue < 0.25f) state = HurstExponentEnum::ANTI_PERSISTENT_STRONG;
            else                    state = HurstExponentEnum::ANTI_PERSISTENT_WEAK;
        }
        else if (hurstValue > 0.60f) {
            // Persistent (Black Noise) - Price covers more distance than random walk (Trending)
            if (hurstValue > 0.75f) state = HurstExponentEnum::PERSISTENT_STRONG;
            else                    state = HurstExponentEnum::PERSISTENT_WEAK;
        }
        else {
            // Random Walk (White Noise) - 0.50 center
            state = HurstExponentEnum::RANDOM_WALK;
        }

        Update(state);
    }

private:
    // Persistent buffers for DFA calculation (avoids recurring allocation)
    std::vector<double> m_logReturns;
    std::vector<double> m_profile;
    std::vector<double> m_logScales;
    std::vector<double> m_logFluctuations;
};

class TimeOfDayIndicator : public Indicator<TimeOfDayEnum> {
public:
    TimeOfDayIndicator(IndicatorKey key) : Indicator(key, TimeOfDayEnum::OVERNIGHT_HOLD) {}

    /**
     * Compute TimeOfDayEnum from SCDateTime.
     * Uses market hours: 9:30 AM - 4:00 PM ET (standard US equity market hours).
     * Extended to include Globex session windows for overnight management.
     */
    void SetFromDateTime(const SCDateTime& dateTime, bool hasOpenPosition = false);
};

class OvernightExitIndicator : public Indicator<OvernightExitTypeEnum> {
public:
    OvernightExitIndicator(IndicatorKey key) : Indicator(key, OvernightExitTypeEnum::NO_OVERNIGHT_POSITION) {}

    /**
     * Determine overnight exit type based on next morning's price action.
     * Called during Globex or first 90 minutes of regular session.
     *
     * @param overnightEntry Entry price of overnight position
     * @param prevDayHigh Previous day's high (Taylor "Objective Point")
     * @param prevDayLow Previous day's low (Taylor "Objective Point")
     * @param openPrice Today's open price
     * @param currentPrice Current price
     * @param isLong Position direction
     * @param threeLineOscillator 3-period minus 16-period moving average (Raschke 3-10 Oscillator)
     * @param threeLineOscPrev Previous bar's oscillator value
     * @return OvernightExitTypeEnum indicating exit action
     */
    void SetFromOvernightContext(
        float overnightEntry,
        float prevDayHigh,
        float prevDayLow,
        float openPrice,
        float currentPrice,
        bool isLong,
        float threeLineOscillator,
        float threeLineOscPrev
    );

    /**
     * Check if current price has hit Taylor "Objective Point" target.
     * Buy Day: Target is previous day's high
     * Sell Day: Target is previous day's low
     */
    static bool HasHitObjectivePoint(float currentPrice, float prevDayHigh, float prevDayLow, bool isLong);

    /**
     * Check for gap (windfall) in favor of position.
     * Gap threshold: > 0.5% for "windfall" classification
     */
    static bool IsGapInFavor(float overnightEntry, float openPrice, bool isLong);
};

/**
 * HmmStateIndicator
 * Specialized instance of the Template for the HMM Regime IDs.
 * Key: "hmm_state"
 *
 * Single source of truth for ALL Student-t HMM diagnostics on the C++ side.
 * Written by HMMClient worker thread, read by PositionManager/RiskManager on
 * the main ACSIL thread.  All mutable fields are std::atomic<float> to
 * eliminate the latent data race on cross-thread reads.
 */
class HmmStateIndicator : public Indicator<HMMStateEnum> {
public:
    HmmStateIndicator(IndicatorKey key)
        : Indicator(key, HMM_NO_PRIOR)
    {}

    // Non-copyable / non-movable (atomic members are not trivially copyable).
    HmmStateIndicator(const HmmStateIndicator&) = delete;
    HmmStateIndicator& operator=(const HmmStateIndicator&) = delete;
    HmmStateIndicator(HmmStateIndicator&&) = delete;
    HmmStateIndicator& operator=(HmmStateIndicator&&) = delete;

    /// Full update from HMMClient::HandleBinaryResponse (worker thread).
    void SetState(int stateId, float riskMultiplier, float transitionRisk,
                  float dof, float mahalanobis, float tailWeight,
                  float expectedDuration, float entropy = 0.0f) {
        Update(static_cast<HMMStateEnum>(stateId));
        m_risk_multiplier.store(riskMultiplier, std::memory_order_relaxed);
        m_transition_risk.store(transitionRisk, std::memory_order_relaxed);
        m_dof.store(dof, std::memory_order_relaxed);
        m_mahalanobis.store(mahalanobis, std::memory_order_relaxed);
        m_tail_weight.store(tailWeight, std::memory_order_relaxed);
        m_expected_duration.store(expectedDuration, std::memory_order_relaxed);
        m_entropy.store(entropy, std::memory_order_relaxed);
    }

    // Explicit trigger logic: Any change in HMM state is a significant event
    bool ShouldTrigger() const override {
        return IsDirty();
    }

    // --- Thread-safe getters (all relaxed-order, fine for non-critical sizing) ---
    float RiskMultiplier() const { return m_risk_multiplier.load(std::memory_order_relaxed); }
    float TransitionRisk() const { return m_transition_risk.load(std::memory_order_relaxed); }
    float Dof() const { return m_dof.load(std::memory_order_relaxed); }
    float Mahalanobis() const { return m_mahalanobis.load(std::memory_order_relaxed); }
    float TailWeight() const { return m_tail_weight.load(std::memory_order_relaxed); }
    float ExpectedDuration() const { return m_expected_duration.load(std::memory_order_relaxed); }
    float Entropy() const { return m_entropy.load(std::memory_order_relaxed); }

    // =====================================================================
    // Domain methods — encapsulate Taleb/Elder/Shannon sizing & gating logic
    // so consumers call intent-revealing APIs instead of reimplementing math.
    // =====================================================================

    /// DOF-aware ATR stop scale (Taleb t-quantile approximation).
    /// Returns multiplier ∈ [1.0, 1.5].
    /// Safe-closed: 1.5 when no HMM data (most conservative).
    /// Gaussian: 1.0 when dof ≥ 30.
    double DofStopScale() const {
        const float dof = Dof();
        if (dof < 1e-6f) return 1.5;                        // No data → max widening
        if (dof <= 2.0f + 1e-6f || dof >= 30.0f) return 1.0; // Undefined or Gaussian
        return std::min(1.5, std::sqrt((static_cast<double>(dof) + 1.0) /
                                       (static_cast<double>(dof) - 1.0)));
    }

    /// Mahalanobis emergency flatten test.
    /// Returns true when current observation is an outlier that warrants immediate exit.
    /// Threshold is DOF-scaled: lower DOF → more lenient (fat distributions produce
    /// naturally higher Mahalanobis distances).
    bool IsOutlierEmergency() const {
        const float mahal = Mahalanobis();
        if (mahal <= 0.0f) return false;
        const float dof = Dof();
        const double threshold = (dof > 2.0f + 1e-6f)
            ? 8.0 + 4.0 / (static_cast<double>(dof) - 1.0)
            : 12.0;
        return mahal > static_cast<float>(threshold);
    }

    /// Mahalanobis sigmoid cap for position sizing (Taleb+Elder).
    /// Returns multiplier ∈ (0, 1].
    /// mahal < 4.0 → 1.0 (no penalty); mahal > 8.0 → ≈0.08.
    double MahalanobisSizingCap() const {
        const float mahal = Mahalanobis();
        if (mahal <= 4.0f) return 1.0;
        return 1.0 / (1.0 + std::exp(0.5 * (static_cast<double>(mahal) - 5.0)));
    }

    /// Tail weight discount for position sizing (Taleb).
    /// Returns multiplier ∈ [0.5, 1.0].
    /// tail_weight ≥ 0.70 → 1.0; tail_weight = 0 → 0.5.
    double TailWeightDiscount() const {
        const float tw = TailWeight();
        if (tw >= 0.7f || tw < 0.0f) return 1.0;
        return 0.5 + 0.5 * (static_cast<double>(tw) / 0.7);
    }

    /// Expected-duration sizing factor for position sizing (RiskManager).
    /// Returns multiplier ∈ [0.70, 1.0].
    /// Ephemeral regimes (<5 bars) → 0.70×; long-lived (≥20 bars) → 1.0×.
    double SizingDurationFactor() const {
        const float dur = ExpectedDuration();
        if (dur <= 0.0f || dur >= 20.0f) return 1.0;
        return 0.70 + 0.30 * std::clamp(
            (static_cast<double>(dur) - 5.0) / 15.0, 0.0, 1.0);
    }

    /// Expected-duration holding stability factor (PositionManagerPatterns).
    /// Returns multiplier ∈ [0.75, 1.0].
    /// Short regimes (<5 bars) → 0.75×; long regimes (≥15 bars) → 1.0×.
    double HoldingStabilityFactor() const {
        const float dur = ExpectedDuration();
        if (dur <= 0.0f || dur >= 15.0f) return 1.0;
        return 0.75 + 0.25 * std::clamp(
            (static_cast<double>(dur) - 5.0) / 10.0, 0.0, 1.0);
    }

    /// Model kurtosis from Student-t DOF: 3*(dof-2)/(dof-4).
    /// Returns 0 if DOF ≤ 4 (kurtosis undefined / infinite).
    float ModelKurtosis() const {
        const float dof = Dof();
        if (dof <= 4.0f + 1e-6f) return 0.0f;
        return 3.0f * (dof - 2.0f) / (dof - 4.0f);
    }

    /// Duration-decay factor for trailing stop tightening (Gap 4 — Pareto).
    /// As bars_held approaches expected_duration, the stop should tighten
    /// proactively — you are approaching the regime boundary.
    /// Returns multiplier ∈ [0.60, 1.0] applied to Chandelier ATR distance.
    ///   ratio < 0.5  → 1.0  (early in regime, full room)
    ///   ratio 0.5–0.8 → linear tighten to 0.85
    ///   ratio 0.8–1.0 → tighten to 0.70
    ///   ratio > 1.0   → 0.60 (overstayed — regime likely to flip)
    double DurationDecayFactor(int barsHeld) const {
        const float dur = ExpectedDuration();
        if (dur <= 0.0f || barsHeld <= 0) return 1.0;
        const double ratio = static_cast<double>(barsHeld) / static_cast<double>(dur);
        if (ratio < 0.5) return 1.0;
        if (ratio < 0.8) return 1.0 - 0.15 * ((ratio - 0.5) / 0.3);   // 1.0 → 0.85
        if (ratio < 1.0) return 0.85 - 0.15 * ((ratio - 0.8) / 0.2);  // 0.85 → 0.70
        return 0.60;
    }

    /// DOF-adaptive confidence threshold (Taleb — fat-tail uncertainty scaling).
    /// Maps a base confidence threshold upward when DOF is low (fat tails =
    /// model uncertainty is fundamentally wider, demand more evidence).
    ///
    /// Formula: threshold = base + (1 - base) × penalty
    ///   penalty = clamp(1 / (DOF - 1), 0, 0.5)
    ///
    ///   DOF = 3  → penalty = 0.50, base 0.80 → 0.90
    ///   DOF = 5  → penalty = 0.25, base 0.80 → 0.85
    ///   DOF = 10 → penalty = 0.11, base 0.80 → 0.822
    ///   DOF = 30 → penalty = 0.03, base 0.80 → 0.807
    ///   No HMM   → returns base + 0.05 (conservative default)
    ///
    /// Returns value in [base, min(base + headroom/2, 0.95)].
    float DofConfidenceThreshold(float base) const {
        const float dof = Dof();
        if (dof < 1e-6f) return std::min(base + 0.05f, 0.95f);  // No HMM data
        if (dof >= 30.0f) return base;                            // Gaussian
        if (dof <= 1.0f + 1e-6f) return std::min(base + (1.0f - base) * 0.5f, 0.95f);  // Degenerate

        const float penalty = std::clamp(1.0f / (dof - 1.0f), 0.0f, 0.5f);
        return std::min(base + (1.0f - base) * penalty, 0.95f);
    }

private:
    std::atomic<float> m_risk_multiplier{0.5f};
    std::atomic<float> m_transition_risk{1.0f};
    std::atomic<float> m_dof{0.0f};
    std::atomic<float> m_mahalanobis{0.0f};
    std::atomic<float> m_tail_weight{0.0f};
    std::atomic<float> m_expected_duration{0.0f};
    std::atomic<float> m_entropy{0.0f};
};


/**
 * PredictionState
 * Persistent Transformer prediction state — NOT a trigger-capable indicator.
 * Key: PREDICTION_STATE
 *
 * Single source of truth for the latest ML model prediction on the C++ side.
 * Written by TradeExecutionServer when Python sends ModelPrediction (port 5559).
 * Read by PositionManager, RiskManager, and trade management logic on main
 * ACSIL thread.
 *
 * Safety invariant: Action defaults to STAND_ASIDE at construction and Reset().
 * Callers MUST set action to STAND_ASIDE when orders are prohibited,
 * and to EXIT_LONG/EXIT_SHORT for forced immediate exits.
 *
 * Does NOT participate in dirty-bit event emission (SetDirtyMaskPointer is a
 * no-op).  This class is a passive read-only state repository for trade
 * management — not an event source.
 *
 * All mutable fields are std::atomic<> with relaxed ordering
 * (same cross-thread pattern as HmmStateIndicator).
 */
class PredictionState : public Indicator<TradeActionEnum> {
public:
    PredictionState(IndicatorKey key)
        : Indicator(key, TradeActionEnum::STAND_ASIDE)
    {}

    // Non-copyable / non-movable (atomic members).
    PredictionState(const PredictionState&) = delete;
    PredictionState& operator=(const PredictionState&) = delete;
    PredictionState(PredictionState&&) = delete;
    PredictionState& operator=(PredictionState&&) = delete;

    // Never triggers event emission — passive state holder.
    bool ShouldTrigger() const override { return false; }

    // Block dirty-mask injection so prediction changes never fire events.
    void SetDirtyMaskPointer(uint64_t* /*maskPtr*/) override { /* intentional no-op */ }

    /// Full update from TradeExecutionServer::HandlePythonPrediction.
    void SetPrediction(TradeActionEnum action, float confidence,
                       float thesisStrength, float actionEntropy,
                       float top2Margin, uint64_t timestampUs,
                       uint64_t sequenceId, int64_t inferenceLatencyUs,
                       int64_t transformerLatencyUs, int64_t regimeLatencyUs,
                       bool modelReady) {
        Update(action);
        m_confidence.store(confidence, std::memory_order_relaxed);
        m_thesis_strength.store(thesisStrength, std::memory_order_relaxed);
        m_action_entropy.store(actionEntropy, std::memory_order_relaxed);
        m_top2_margin.store(top2Margin, std::memory_order_relaxed);
        m_timestamp_us.store(timestampUs, std::memory_order_relaxed);
        m_sequence_id.store(sequenceId, std::memory_order_relaxed);
        m_inference_latency_us.store(inferenceLatencyUs, std::memory_order_relaxed);
        m_transformer_latency_us.store(transformerLatencyUs, std::memory_order_relaxed);
        m_regime_latency_us.store(regimeLatencyUs, std::memory_order_relaxed);
        m_model_ready.store(modelReady, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------
    // Safety resets
    // -----------------------------------------------------------------

    /// Force STAND_ASIDE — call when orders are prohibited (risk breach,
    /// connection loss, model staleness, etc.).  Zeroes confidence to
    /// prevent stale reads from influencing sizing.
    void ForceStandAside() {
        Update(TradeActionEnum::STAND_ASIDE);
        m_confidence.store(0.0f, std::memory_order_relaxed);
        m_model_ready.store(false, std::memory_order_relaxed);
    }

    /// Force immediate directional exit — call when a position must be
    /// closed NOW (Mahalanobis outlier, hard drawdown limit, etc.).
    /// Sets confidence to 1.0 so sizing logic treats this as decisive.
    void ForceExit(bool isLong) {
        Update(isLong ? TradeActionEnum::EXIT_LONG
                      : TradeActionEnum::EXIT_SHORT);
        m_confidence.store(1.0f, std::memory_order_relaxed);
        m_model_ready.store(true, std::memory_order_relaxed);
    }

    // -----------------------------------------------------------------
    // Thread-safe getters (relaxed order — non-critical sizing reads)
    // -----------------------------------------------------------------
    TradeActionEnum Action()      const { return Value(); }
    const char*     ActionName()  const { return TradeActionToString(Value()); }
    float    Confidence()         const { return m_confidence.load(std::memory_order_relaxed); }
    float    ThesisStrength()     const { return m_thesis_strength.load(std::memory_order_relaxed); }
    float    ActionEntropy()      const { return m_action_entropy.load(std::memory_order_relaxed); }
    float    Top2Margin()         const { return m_top2_margin.load(std::memory_order_relaxed); }
    uint64_t TimestampUs()        const { return m_timestamp_us.load(std::memory_order_relaxed); }
    uint64_t SequenceId()         const { return m_sequence_id.load(std::memory_order_relaxed); }
    int64_t  InferenceLatencyUs() const { return m_inference_latency_us.load(std::memory_order_relaxed); }
    int64_t  TransformerLatencyUs() const { return m_transformer_latency_us.load(std::memory_order_relaxed); }
    int64_t  RegimeLatencyUs()    const { return m_regime_latency_us.load(std::memory_order_relaxed); }
    bool     ModelReady()         const { return m_model_ready.load(std::memory_order_relaxed); }

    // -----------------------------------------------------------------
    // Domain queries
    // -----------------------------------------------------------------

    /// True if action is ENTER_LONG or ENTER_SHORT.
    bool IsEntry() const { return IsEntryAction(Value()); }

    /// True if action is EXIT_LONG or EXIT_SHORT.
    bool IsExit() const { return IsExitAction(Value()); }

    /// True if action is HOLD_LONG or HOLD_SHORT.
    bool IsHold() const { return IsHoldAction(Value()); }

    /// Low entropy = model is decisive (single dominant action).
    bool IsDecisive() const { return ActionEntropy() < 1.0f; }

    /// Confidence above a caller-specified threshold.
    bool IsConfident(float threshold = 0.60f) const {
        return Confidence() >= threshold;
    }

    /// Staleness check.  Returns true if prediction arrived within
    /// maxAgeUs of the supplied wall-clock timestamp.
    bool IsFresh(uint64_t nowUs, uint64_t maxAgeUs) const {
        const uint64_t ts = TimestampUs();
        if (ts == 0) return false;   // Never received a prediction
        return (nowUs - ts) <= maxAgeUs;
    }

    /// Shannon information-decay discount (Gap 2).
    /// Predictions lose value exponentially as they age — channel capacity
    /// degrades with delay.  Returns multiplier ∈ [0.0, 1.0].
    ///   Age < 5s   → 1.0  (fresh)
    ///   5–15s      → linear 1.0 → 0.70
    ///   15–30s     → linear 0.70 → 0.40
    ///   > 30s      → 0.0  (treat as STAND_ASIDE)
    ///
    /// NOTE: This is the WALL-CLOCK backstop for position management (stop scaling,
    /// risk sizing).  For entry-freshness, use SemanticFreshnessDiscount() which
    /// measures event-count divergence — the correct staleness metric for an
    /// event-driven system where silence IS information.
    double FreshnessDiscount(uint64_t nowUs) const {
        const uint64_t ts = TimestampUs();
        if (ts == 0 || nowUs <= ts) return 0.0;
        const double ageSec = static_cast<double>(nowUs - ts) / 1e6;
        if (ageSec < 5.0)  return 1.0;
        if (ageSec < 15.0) return 1.0 - 0.30 * ((ageSec - 5.0) / 10.0);   // 1.0 → 0.70
        if (ageSec < 30.0) return 0.70 - 0.30 * ((ageSec - 15.0) / 15.0); // 0.70 → 0.40
        return 0.0;
    }

    /// Event-count semantic freshness — the correct staleness metric for
    /// an event-driven architecture.
    ///
    /// A prediction is semantically valid until the market says something
    /// new (an indicator fires).  During quiet periods, eventsSince stays
    /// at 0 and the prediction remains fully fresh — silence IS information.
    /// During volatile regime shifts, many indicators fire and eventsSince
    /// ramps quickly.
    ///
    /// Returns multiplier ∈ [0.0, 1.0]:
    ///   0 events → 1.00  (nothing changed — prediction fully current)
    ///   1 event  → 0.90  (minor update — still mostly valid)
    ///   2 events → 0.75  (two signals the model hasn't seen)
    ///   3 events → 0.50  (significant divergence)
    ///   4 events → 0.25  (model is seriously behind)
    ///   5+ events→ 0.00  (world has moved on — treat as STAND_ASIDE)
    double SemanticFreshnessDiscount(uint64_t currentGlobalSequenceId) const {
        const uint64_t predSeq = SequenceId();
        if (predSeq == 0 || currentGlobalSequenceId < predSeq) return 0.0;
        const uint64_t eventsSince = currentGlobalSequenceId - predSeq;
        switch (eventsSince) {
            case 0:  return 1.00;
            case 1:  return 0.90;
            case 2:  return 0.75;
            case 3:  return 0.50;
            case 4:  return 0.25;
            default: return 0.00;
        }
    }

private:
    std::atomic<float>    m_confidence{0.0f};
    std::atomic<float>    m_thesis_strength{0.0f};
    std::atomic<float>    m_action_entropy{0.0f};
    std::atomic<float>    m_top2_margin{0.0f};
    std::atomic<uint64_t> m_timestamp_us{0};
    std::atomic<uint64_t> m_sequence_id{0};
    std::atomic<int64_t>  m_inference_latency_us{0};
    std::atomic<int64_t>  m_transformer_latency_us{0};
    std::atomic<int64_t>  m_regime_latency_us{0};
    std::atomic<bool>     m_model_ready{false};
};


// Forward declaration — full definition in ContextManager.h (available via PCH)
struct LocalRiskContext;

/**
 * MarketClimateIndicator
 * Specialized instance of the Template for Macro Market Climate IDs.
 * Key: "market_climate"
 *
 * Elite v3.2: Reads from LocalRiskContext (single source of truth from ContextManager)
 * instead of receiving raw physics params from TripleScreen3.
 */
class MarketClimateIndicator : public Indicator<MarketClimate> {
private:
    int m_stateDuration;

public:
    MarketClimateIndicator(IndicatorKey key)
        : Indicator(key, MarketClimate::GAUSSIAN_STABLE)
        , m_stateDuration(0)
    {}

    /// Elite v3.2: Classify climate from unified LocalRiskContext
    void UpdateContext(const LocalRiskContext& ctx, HMMStateEnum hmmState);

    void SetClimate(int climateId) {
        Update(static_cast<MarketClimate>(climateId));
    }

    int GetRegimeDuration() const { return m_stateDuration; }

    // Usually passive: Climate shifts are slower and shouldn't
    // necessarily trigger a new trade signal on their own.
    bool ShouldTrigger() const override {
        return false;
    }
};

class NhNlSignalIndicator : public Indicator<NhNlSignalEnum> {
private:
    float m_dailyValue;

public:
    NhNlSignalIndicator(IndicatorKey key)
        : Indicator(key, NhNlSignalEnum::UNCLEAR)
        , m_dailyValue(0.0f)
    {}

    void SetDailyValue(float val) {
        m_dailyValue = val;
    }

    float GetDailyValue() const {
        return m_dailyValue;
    }

    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const override {
        Indicator<NhNlSignalEnum>::AddToTrainingEventFB(event);
        event.nh_nl_daily = m_dailyValue;
    }
};

// Oscillator310: Linda Raschke's 3-10 oscillator (actually 3-16)
// Managed by IndicatorManager for cross-study access (overnight exits, divergence detection)
class Oscillator310 : public Indicator<Oscillator310CrossEnum> {
private:
    float m_fastLine;      // EMA(3) - EMA(16)
    float m_slowLine;      // SMA(16, fastLine)
    float m_prevFastLine;
    float m_prevSlowLine;

public:
    Oscillator310(IndicatorKey key_)
        : Indicator(key_, Oscillator310CrossEnum::NEUTRAL)
        , m_fastLine(0.0f)
        , m_slowLine(0.0f)
        , m_prevFastLine(0.0f)
        , m_prevSlowLine(0.0f)
    {}

    // indicator-manager-dod-soa plan, Task 7: the actual crossover detection
    // is ComputeOscillator310Cross() (IndicatorComputations.h), a pure,
    // stateless free function; this method stays as the ACSIL-only adapter
    // owning the fastLine/slowLine/prev history.
    void UpdateOscillator(float fastLine, float slowLine) {
        m_prevFastLine = m_fastLine;
        m_prevSlowLine = m_slowLine;
        m_fastLine = fastLine;
        m_slowLine = slowLine;

        Update(ComputeOscillator310Cross(m_fastLine, m_slowLine, m_prevFastLine, m_prevSlowLine));
    }

    float FastLine() const { return m_fastLine; }
    float SlowLine() const { return m_slowLine; }
    float PrevFastLine() const { return m_prevFastLine; }
    float PrevSlowLine() const { return m_prevSlowLine; }
};

// ============================================================================
// CROSS-MARKET CORRELATION INDICATORS
// ============================================================================

// Correlation strength enum for interpretation
enum class CorrelationStrengthEnum : int8_t {
    STRONG_NEGATIVE = -2,   // < -0.7 (strongly inverse)
    WEAK_NEGATIVE = -1,     // -0.7 to -0.3 (moderately inverse)
    NEUTRAL = 0,            // -0.3 to 0.3 (no clear relationship)
    WEAK_POSITIVE = 1,      // 0.3 to 0.7 (moderately aligned)
    STRONG_POSITIVE = 2     // > 0.7 (strongly aligned)
};

// TrendEnum for cross-market instruments
enum class CrossMarketTrendEnum : int8_t {
    DOWN = -1,
    FLAT = 0,
    UP = 1
};

// Correlation indicator - stores floating-point correlation value
// Unlike other indicators which store enums, this stores the raw correlation coefficient
class CorrelationIndicator : public BaseIndicator {
private:
    IndicatorKey m_key;
    uint64_t* m_dirty_mask_ptr;
    float m_correlation;      // Raw correlation value (-1.0 to 1.0)
    float m_prevCorrelation;
    float* m_packedSlotF32 = nullptr;

public:
    CorrelationIndicator(IndicatorKey key_)
        : m_key(key_)
        , m_dirty_mask_ptr(nullptr)
        , m_correlation(0.0f)
        , m_prevCorrelation(0.0f)
    {}

    void Reset() override {
        m_prevCorrelation = 0.0f;
        m_correlation = 0.0f;
        if (m_dirty_mask_ptr) {
            *m_dirty_mask_ptr &= ~(1ULL << static_cast<uint64_t>(m_key));
        }
    }

    bool IsDirty() const override {
        return m_dirty_mask_ptr && ((*m_dirty_mask_ptr & (1ULL << static_cast<uint64_t>(m_key))) != 0ULL);
    }

    void SetDirtyMaskPointer(uint64_t* maskPtr) override {
        m_dirty_mask_ptr = maskPtr;
    }

    // v5.0: Correlations are passive (Tier 3) - never trigger, only piggyback
    bool ShouldTrigger() const override { return false; }

    // Companion Float32-block dual-write target (indicator-manager-dod-soa
    // plan, Task 4): nullptr = not yet wired.
    void SetPackedSlotPointer(float* slot) { m_packedSlotF32 = slot; }

    void Update(float newCorrelation) {
        if (newCorrelation != m_correlation) {
            m_prevCorrelation = m_correlation;  // Always sync - clears dirty state naturally
            m_correlation = newCorrelation;
            if (m_dirty_mask_ptr) {
                *m_dirty_mask_ptr |= (1ULL << static_cast<uint64_t>(m_key));
            }
            if (m_packedSlotF32) {
                *m_packedSlotF32 = m_correlation;
            }
        }
    }

    IndicatorKey Key() const override { return m_key; }

    float Value() const { return m_correlation; }

    // Convert to enum for categorical interpretation
    CorrelationStrengthEnum GetStrength() const {
        if (m_correlation < -0.7f) return CorrelationStrengthEnum::STRONG_NEGATIVE;
        if (m_correlation < -0.3f) return CorrelationStrengthEnum::WEAK_NEGATIVE;
        if (m_correlation > 0.7f) return CorrelationStrengthEnum::STRONG_POSITIVE;
        if (m_correlation > 0.3f) return CorrelationStrengthEnum::WEAK_POSITIVE;
        return CorrelationStrengthEnum::NEUTRAL;
    }

    int intValue() const override {
        return static_cast<int>(GetStrength());
    }

    // Extract int8_t value and clear dirty flag (correlation strength enum)
    int8_t ExtractInt8AndClearDirty() override {
        int8_t value = static_cast<int8_t>(GetStrength());
        m_prevCorrelation = m_correlation;  // Sync state and clear dirty flag
        if (m_dirty_mask_ptr) {
            *m_dirty_mask_ptr &= ~(1ULL << static_cast<uint64_t>(m_key));
        }
        return value;
    }

    // FlatBuffer serialization - TrainingEvent (wide table)
    void AddToTrainingEventFB(MTS::Training::TrainingEventT& event) const override {
        // Map correlation fields (only 2 base correlations + 4 derivatives in schema)
        IndicatorKey k = Key();
        if (!event.indicators) event.indicators = std::make_unique<MTS::Schema::IndicatorState>();
        if (k == IndicatorKey::CORR_ES_ZN) event.indicators->mutate_corr_es_zn(m_correlation);
        else if (k == IndicatorKey::CORR_ES_DX) event.indicators->mutate_corr_es_dx(m_correlation);
        // Derivatives (delta, accel) computed from these base correlations
        // Not directly exported - Python calculates them
    }
};

// Cross-market trend indicator (ZN, DX trend direction)
class CrossMarketTrend : public Indicator<CrossMarketTrendEnum> {
public:
    CrossMarketTrend(IndicatorKey key_)
        : Indicator(key_, CrossMarketTrendEnum::FLAT)
    {}
};
