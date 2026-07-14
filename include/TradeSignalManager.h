#pragma once

#include "RiskManager.h"
#include <mutex>

/**
 * @brief Singleton class to store the latest trade signal from Python NN
 * 
 * Stores trade decision, setup parameters, and validation results
 * for display on the dashboard and execution by PositionManager.
 */
class TradeSignalManager {
public:
    static TradeSignalManager& Instance();

    // Update trade signal from Python NN
    void SetTradeSignal(const TradeValidationParams& params);
    
    // Get current trade signal
    const TradeValidationParams& GetTradeSignal() const;
    
    // Get validation result (computed from RiskManager)
    const TradeValidationResult& GetValidationResult() const;
    void SetValidationResult(const TradeValidationResult& result);
    
    // Check if signal is fresh (updated this bar)
    bool HasFreshSignal() const;
    void MarkSignalProcessed();
    
    // Clear signal (e.g., when position opened)
    void ClearSignal();

private:
    TradeSignalManager() = default;
    ~TradeSignalManager() = default;
    TradeSignalManager(const TradeSignalManager&) = delete;
    TradeSignalManager& operator=(const TradeSignalManager&) = delete;

    mutable std::mutex m_mutex;
    TradeValidationParams m_currentSignal;
    TradeValidationResult m_validationResult;
    bool m_hasFreshSignal = false;
};
