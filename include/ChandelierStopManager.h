#pragma once

#include "sierrachart.h"
#include <map>
#include <set>
#include <mutex>

/**
 * @brief Manages Chandelier trailing stops (3×ATR volatility-adjusted stops)
 *
 * Implements Alexander Elder's Chandelier Exit system:
 * - Long: Stop = Highest High (since entry) - (3 × ATR14)
 * - Short: Stop = Lowest Low (since entry) + (3 × ATR14)
 *
 * Key Features:
 * - One-way movement only (stops move favorably, never against)
 * - Event-driven: UpdateStop() fires on every tick (intra-bar)
 * - Climax bar detection uses barHasClosed to count completed bars
 * - Pattern-based activation (trend continuation patterns only)
 * - Automatic breakeven lock once profitable
 *
 * Usage Pattern:
 * 1. InitializeStop() on trade entry
 * 2. ActivateTrailing() after T1 fills (for 3+ contracts)
 * 3. UpdateStop() every tick (pass barHasClosed for bar-counting logic)
 * 4. ShouldStopOut() to check exit signal
 * 5. RemoveStop() when position closes
 */
class ChandelierStopManager {
public:
    struct StopInfo {
        double stopPrice;           // Current stop loss price
        double highestHigh;         // Highest high since entry (for longs)
        double lowestLow;           // Lowest low since entry (for shorts)
        double entryPrice;          // Original entry price
        double initialStopPrice;    // Original stop loss
        bool isLong;                // Position direction
        bool isActive;              // Whether trailing is active
        int entryBarIndex;          // Bar index when trade entered
        double atrMultiplier;       // ATR multiplier (default 3.0)
        double baseMultiplier;      // Base multiplier before profit adjustments

        // Vol-Expansion Tightening (Elite enhancement)
        bool isClimaxTightening;         // True when tightened due to climax bar
        int normalBarsCounter;           // Count consecutive COMPLETED normal bars for reset
        bool wasClimaxBarInProgress;     // True if any tick in the forming bar exceeded climax threshold
        double peakClimaxRatio;          // Highest range/ATR10 ratio seen during forming bar (proportional response)

        StopInfo()
            : stopPrice(0), highestHigh(0), lowestLow(0),
              entryPrice(0), initialStopPrice(0),
              isLong(true), isActive(false),
              entryBarIndex(-1), atrMultiplier(3.0), baseMultiplier(3.0),
              isClimaxTightening(false), normalBarsCounter(0),
              wasClimaxBarInProgress(false), peakClimaxRatio(0.0) {}
    };

    /**
     * @brief Get singleton instance
     */
    static ChandelierStopManager& getInstance();

    /**
     * @brief Initialize stop tracking for a new position
     *
     * @param positionID Unique position identifier (Trade ID or order ID)
     * @param isLong True for long positions, false for short
     * @param entryPrice Position entry price
     * @param entryBarIndex Bar index when position was entered
     * @param initialStop Initial stop loss price
     * @param atrMultiplier ATR multiplier (default 3.0, can use 2.5 or 3.5)
     */
    void InitializeStop(int positionID, bool isLong, double entryPrice,
                       int entryBarIndex, double initialStop,
                       double atrMultiplier = 3.0);

    /**
     * @brief Update trailing stop (called every tick, event-driven)
     *
     * Recalculates stop based on:
     * - Highest high since entry (longs) or lowest low (shorts)
     * - Current ATR value
     * - One-way movement rule
     * - Vol-expansion tightening (climax bar detection)
     * - Dynamic multiplier adjustment based on profit level
     *
     * Stop price is recalculated on every tick for responsiveness.
     * Climax bar counting uses barHasClosed to track completed bars
     * (not ticks), so the 3-bar reset window works correctly.
     *
     * @param positionID Position to update
     * @param currentHigh Running high of forming bar
     * @param currentLow Running low of forming bar
     * @param atr Current ATR value (typically ATR14)
     * @param atr10 10-period ATR for climax detection (optional, use atr if not provided)
     * @param currentPrice Current market price (for profit-based multiplier adjustment)
     * @param barHasClosed True when the previous bar just closed (sc.GetBarHasClosedStatus())
     * @return true if stop was updated, false otherwise
     */
    bool UpdateStop(int positionID, double currentHigh, double currentLow, double atr, double atr10 = 0.0, double currentPrice = 0.0, bool barHasClosed = false);

    /**
     * @brief Get current stop price for a position
     *
     * @param positionID Position to query
     * @return Current stop price, or 0.0 if position not found
     */
    double GetStopPrice(int positionID) const;

    /**
     * @brief Check if position should be stopped out
     *
     * @param positionID Position to check
     * @param currentPrice Current market price
     * @return true if price has hit the stop
     */
    bool ShouldStopOut(int positionID, double currentPrice) const;

    /**
     * @brief Remove stop tracking when position closes
     *
     * @param positionID Position to remove
     */
    void RemoveStop(int positionID);

    /**
     * @brief Activate trailing stop (typically after T1 fills)
     *
     * Before activation: Uses initial stop
     * After activation: Uses Chandelier formula
     *
     * @param positionID Position to activate trailing for
     */
    void ActivateTrailing(int positionID);

    /**
     * @brief Check if trailing is active for a position
     *
     * @param positionID Position to check
     * @return true if trailing is active
     */
    bool IsTrailingActive(int positionID) const;

    /**
     * @brief Force a tighter stop while preserving existing trailing state
     *
     * One-way rule still applies:
     * - Long: only accepts higher stop prices
     * - Short: only accepts lower stop prices
     *
     * @param positionID Position to update
     * @param tighterStop Proposed tighter stop price
     * @param newBaseMultiplier Optional new base ATR multiplier for subsequent updates
     * @return true if stop was tightened, false otherwise
     */
    bool ForceTightenStop(int positionID, double tighterStop, double newBaseMultiplier = 0.0);

    /**
     * @brief Check if pattern should use Chandelier trailing
     *
     * Trend continuation patterns use trailing stops (let winners run)
     * Mean reversion patterns use fixed targets (quick scalps)
     *
     * @param patternEnum Pattern enum string (e.g., "HOLY_GRAIL_CONTINUATION")
     * @return true if pattern should trail with Chandelier
     */
    static bool ShouldUseTrailingStop(RaschkeTacticalTrigger patternTrigger);  // Type-safe enum-based check

    /**
     * @brief Get detailed stop info for debugging
     *
     * @param positionID Position to query
     * @return StopInfo struct with all details
     */
    StopInfo GetStopInfo(int positionID) const;

    /**
     * @brief Clear all stops (emergency cleanup)
     */
    void ClearAllStops();

private:
    ChandelierStopManager() = default;
    ~ChandelierStopManager() = default;

    // Prevent copying
    ChandelierStopManager(const ChandelierStopManager&) = delete;
    ChandelierStopManager& operator=(const ChandelierStopManager&) = delete;

    std::map<int, StopInfo> stops_;
    std::set<int> breakevenLogged_;   // Track which positions have logged breakeven
    mutable std::mutex mutex_;

    /**
     * @brief Calculate dynamic ATR multiplier based on profit level
     *
     * Smooth exponential decay eliminates discrete cliff-edges:
     *   m(R) = floor + (base - floor) * exp(-lambda * max(0, R - onset))
     *
     * - Below 2R: base multiplier (let trade breathe)
     * - Above 2R: continuous decay toward floor (2.0×ATR)
     * - Half-life ≈ 2R beyond onset
     *
     * @param currentProfit Current unrealized P&L in points
     * @param initialRisk Original risk in points (entry - initial stop)
     * @param baseMultiplier Base multiplier before adjustment
     * @return Adjusted multiplier (smooth, no discontinuities)
     */
    static double GetDynamicMultiplier(double currentProfit, double initialRisk, double baseMultiplier);

    /**
     * @brief Calculate new stop price using Chandelier formula
     *
     * Uses wick-filtered info.highestHigh / info.lowestLow as the trailing anchor.
     *
     * @param info Current stop info (contains wick-filtered extremes)
     * @param atr Current ATR value
     * @param overrideMultiplier Optional multiplier override (for climax tightening)
     * @return New stop price
     */
    double CalculateChandelierStop(const StopInfo& info,
                                   double atr,
                                   double overrideMultiplier = 0.0) const;
};
