#pragma once

#include <string_view> // For std::string_view

// Input indices (potentially used across multiple studies)
const int IDX_INPUT_LONG_FI13_CHART_STUDY_LINK(14);
const int IDX_INPUT_LONG_FI13_SUBGRAPH_ID(15);
const int IDX_INPUT_INTERM_STOCHASTICS_CHART_STUDY_LINK(16);
const int IDX_INPUT_INTERM_STOCHASTICS_SUBGRAPH_ID(17);
const int IDX_INPUT_STRATEGY(20);

// Constants for JSON keys (shared serialization protocol)
const std::string_view SYMBOL_KEY{ "symbol" };
const std::string_view SIDE_KEY{ "side" };
const std::string_view DATE_KEY{ "date" };
const std::string_view LAST_KEY{ "last" };

// Position specific
const std::string_view POSITION{ "position" };
const std::string_view ORDER_ID_KEY{ "order_id" };
const std::string_view SIZE_KEY{ "size" };
const std::string_view TRADE_STATUS_KEY{ "trade_status" };
const std::string_view STOP_KEY{ "stop" };
const std::string_view TARGET_KEY{ "target" };
const std::string_view RISK_REWARD_KEY{ "risk_reward" };
const std::string_view ENTRY_DATE_KEY{ "entry_date" };
const std::string_view ENTRY_PRICE_KEY{ "entry_price" };
const std::string_view ENTRY_HIGH_KEY{ "entry_high" };
const std::string_view ENTRY_LOW_KEY{ "entry_low" };
const std::string_view ENTRY_GRADE_KEY{ "entry_grade" };
const std::string_view EXIT_DATE_KEY{ "exit_date" };
const std::string_view EXIT_PRICE_KEY{ "exit_price" };
const std::string_view EXIT_HIGH_KEY{ "exit_high" };
const std::string_view EXIT_LOW_KEY{ "exit_low" };
const std::string_view EXIT_GRADE_KEY{ "exit_grade" };
const std::string_view CHANNEL_KEY{ "channel" };
const std::string_view TRADE_GRADE_KEY{ "trade_grade" };
const std::string_view PNL_KEY{ "pnl" };

// Constant used when determining when a price move triggers sending a position update to the GUI
const int POSITION_UPDATE_TICK_THRESHOLD{ 4 };

// Subgraph indices for temporary calculations in pattern detection
// These are used by StudyHelperFunctions.cpp pattern detection and must be allocated
// in any study that calls DetectRaschkeStrategySetup or DetectRaschkeTacticalTrigger
namespace SubgraphIndex {
    const int TEMP_STOCH_K = 50;      // ANTI pattern: Stochastic %K calculation
    const int TEMP_STOCH_D = 51;      // ANTI pattern: Stochastic %D (stored in Arrays[0] of TEMP_STOCH_K)
    // Note: TEMP_ROC (52) and TEMP_PINBALL (53) no longer needed - FLIP now uses MomentumPinball indicator from IndicatorManager
}

// Enum for GUI connection status (used by multiple files)
enum class ConnectionStatus {
    Disconnected = 0,
    Connected = 1
};

// ============================================================================
// PERSISTENT VARIABLE INDICES
// ============================================================================
// Sierra Chart persistent variables (GetPersistentInt/Float) are per-study.
// Each study must use unique indices to avoid data corruption.
// These constants provide named indices for all studies and helper functions.

// TripleScreen1 Study: Weekly Timeframe with MACD Divergence
namespace PersistentVar_TripleScreen1 {
    // Pointer indices
    const int MACD_DIVERGENCE_STATE = 1;  // Pointer to MACDDivergenceState structure
}

// TripleScreen2 Study: Daily Timeframe with MACD Divergence
namespace PersistentVar_TripleScreen2 {
    // Pointer indices
    const int MACD_DIVERGENCE_STATE = 1;  // Pointer to MACDDivergenceState structure
}

// StudyHelperFunctions adaptive calculator state (per-study instance)
namespace PersistentVar_AdaptiveCalculators {
    const int STATE_PTR = 20;  // Pointer to AdaptiveCalculatorsState structure
    const int WINDOW_STATE_PTR = 21;  // Pointer to AdaptiveWindowParams structure
    const int LAST_OBS_UPDATE_INDEX = 22;  // Last bar index processed by UpdateObservationVectorSubgraphs
    const int FISHER_WINDOW_STATE_PTR = 23;  // Independent AdaptiveWindowParams for Fisher Information
    const int HURST_LAST_VALID_VALUE = 24;   // Last finite Hurst value for graceful degradation
    const int HURST_HAS_LAST_VALID = 25;     // 1 when HURST_LAST_VALID_VALUE has been initialized
    const int LAST_AMIHUD_SAMPLE_INDEX = 26; // Last bar index pushed into the Amihud percentile pool (once per 15-min bar)
    const int LAST_VALUE_AREA_SAMPLE_INDEX = 27; // Last bar index a daily Value Area aggregation was run for
}

// TripleScreen3 Study: Turtle Soup Pattern Detection
namespace PersistentVar_TurtleSoup {
    // Integer indices
    const int HIGHEST_INDEX = 1;        // Index of highest high in lookback period
    const int LOWEST_INDEX = 2;         // Index of lowest low in lookback period
    const int LAST_PROCESSED_BAR = 3;   // Last bar processed (for data collection)
    
    // Float indices
    const int CURRENT_HIGHEST = 1;      // Value of highest high in lookback period
    const int CURRENT_LOWEST = 2;       // Value of lowest low in lookback period
    
    // Note: Daily high/low cache moved to IndicatorManager (central cache)
}

// StudyHelperFunctions: DetectElderInitialBreakout (called from multiple studies)
// NOTE: These are used in the context of the CALLING study's persistent variables
// Each calling study must allocate these indices or pass them as parameters
namespace PersistentVar_ElderBreakout {
    // Integer indices (recommend starting at 10 to avoid collision with study's own vars)
    const int ITR_DAY = 10;              // Current day being tracked
    const int ITR_ESTABLISHED = 11;      // Flag: has ITR been established today?
    const int HAD_BREAKOUT_ABOVE = 12;   // Flag: breakout above ITR occurred
    const int HAD_BREAKDOWN_BELOW = 13;  // Flag: breakdown below ITR occurred
    
    // Float indices (recommend starting at 10 to avoid collision)
    const int ITR_HIGH = 10;             // Intraday Trading Range high
    const int ITR_LOW = 11;              // Intraday Trading Range low
}

// StudyHelperFunctions: Position tracking (DetectEntryExit, etc.)
namespace PersistentVar_Position {
    const int ENTRY_INDEX_ID = 100;      // Bar index of entry
    const int ENTRY_SIGNAL_ID = 101;     // Entry signal type
    const int POSITION_STATE_ID = 102;   // Current position state
}

// StudyHelperFunctions: Visualization (TrackPriceLineValue for drawing)
// Note: Uses LineNumber parameter directly as persistent index (range: 200-299)
namespace PersistentVar_Visualization {
    const int BASE_LINE_VALUE = 200;     // Base index for line tracking (200-299)
    // Usage: sc.GetPersistentFloat(BASE_LINE_VALUE + LineNumber)
}

// Risk Management (reserve high indices 500-599 for risk/compliance)
namespace PersistentVar_Risk {
    // Daily trade limits (Elder: max 3-5 trades per day)
    const int TRADES_EXECUTED_TODAY = 500;
    const int CONSECUTIVE_LOSSES = 501;
    const int TRADING_HALTED = 502;
    const int LAST_LOSS_TIMESTAMP = 503;  // Use GetPersistentInt64
    
    // Order action rate limiting (prevent order spam)
    const int LAST_ORDER_ACTION_TIMESTAMP = 504;  // Use GetPersistentInt64
    const int ORDER_ACTION_COUNT_LAST_MINUTE = 505;
    
    // Stop loss enforcement
    const int ORIGINAL_STOP_PRICE_ID = 506;  // Use GetPersistentInt64 (for price precision)
    const int STOP_VIOLATION_COUNT_ID = 507;
    
    // Position size tracking
    const int LAST_POSITION_QTY_ID = 508;
}
