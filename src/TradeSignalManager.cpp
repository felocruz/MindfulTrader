#include "MindfulTrader_Precompiled.h"
#include "TradeSignalManager.h"

TradeSignalManager& TradeSignalManager::Instance() {
    static TradeSignalManager instance;
    return instance;
}

void TradeSignalManager::SetTradeSignal(const TradeValidationParams& params) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_currentSignal = params;
    m_hasFreshSignal = true;
}

const TradeValidationParams& TradeSignalManager::GetTradeSignal() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_currentSignal;
}

const TradeValidationResult& TradeSignalManager::GetValidationResult() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_validationResult;
}

void TradeSignalManager::SetValidationResult(const TradeValidationResult& result) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_validationResult = result;
}

bool TradeSignalManager::HasFreshSignal() const {
    std::lock_guard<std::mutex> lock(m_mutex);
    return m_hasFreshSignal;
}

void TradeSignalManager::MarkSignalProcessed() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hasFreshSignal = false;
}

void TradeSignalManager::ClearSignal() {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_hasFreshSignal = false;
    m_validationResult.allowed = false;
}
