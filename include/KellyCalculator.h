#pragma once

/**
 * @file KellyCalculator.h
 * @brief Trade statistics tracker (circular buffer of recent trades)
 *
 * Tracks win rate, average win/loss R-multiples, and trade count from the
 * last 30 closed trades.  Statistics are consumed by pattern probation and
 * diagnostic logging.
 *
 * Kelly fraction / Kelly multiplier computation was removed — the 30-trade
 * sample is too small for reliable f* estimation, and every signal Kelly
 * would provide is already captured by better-measured multipliers in
 * CalculateSafePositionSize (ATR decay, Hill α, kurtosis cap, etc.).
 */

class KellyCalculator {
public:
    KellyCalculator();

    /**
     * @brief Record a closed trade for statistics
     * @param pnl Trade P&L in dollars
     * @param initialRisk Initial risk (stop distance × contracts × tick value)
     */
    void RecordTrade(double pnl, double initialRisk);

    // Getters for diagnostics / pattern probation
    int GetTradeCount() const { return m_tradeCount; }
    double GetWinRate() const { return m_winRate; }
    double GetAvgWin() const { return m_avgWin; }
    double GetAvgLoss() const { return m_avgLoss; }

private:
    struct TradeResult {
        double pnl = 0.0;
        bool isWin = false;
        double rMultiple = 0.0;  // P&L / initial risk
    };

    static constexpr int TRADE_HISTORY_SIZE = 30;
    TradeResult m_tradeHistory[TRADE_HISTORY_SIZE];
    int m_tradeHistoryIndex = 0;
    int m_tradeCount = 0;

    double m_winRate = 0.5;
    double m_avgWin = 1.0;
    double m_avgLoss = 1.0;

    void RecalculateStatistics();
};
