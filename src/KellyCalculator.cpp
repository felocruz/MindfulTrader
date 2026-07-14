#include "MindfulTrader_Precompiled.h"

KellyCalculator::KellyCalculator() {
    for (int i = 0; i < TRADE_HISTORY_SIZE; i++) {
        m_tradeHistory[i] = TradeResult{0.0, false, 0.0};
    }
}

void KellyCalculator::RecordTrade(double pnl, double initialRisk) {
    double rMultiple = (initialRisk > 0.0) ? (pnl / initialRisk) : 0.0;

    m_tradeHistory[m_tradeHistoryIndex].pnl = pnl;
    m_tradeHistory[m_tradeHistoryIndex].isWin = (pnl > 0.0);
    m_tradeHistory[m_tradeHistoryIndex].rMultiple = rMultiple;

    m_tradeHistoryIndex = (m_tradeHistoryIndex + 1) % TRADE_HISTORY_SIZE;
    if (m_tradeCount < TRADE_HISTORY_SIZE) {
        m_tradeCount++;
    }

    if (m_tradeCount >= 10) {
        RecalculateStatistics();
    }
}

void KellyCalculator::RecalculateStatistics() {
    int wins = 0;
    double totalWins = 0.0;
    double totalLosses = 0.0;
    int losses = 0;

    for (int i = 0; i < m_tradeCount; i++) {
        if (m_tradeHistory[i].isWin) {
            wins++;
            totalWins += m_tradeHistory[i].rMultiple;
        } else {
            losses++;
            totalLosses += std::abs(m_tradeHistory[i].rMultiple);
        }
    }

    m_winRate = static_cast<double>(wins) / m_tradeCount;
    m_avgWin = (wins > 0) ? (totalWins / wins) : 1.0;
    m_avgLoss = (losses > 0) ? (totalLosses / losses) : 1.0;
}
