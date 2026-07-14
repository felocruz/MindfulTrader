#include "ChandelierStopManager.h"
#include "Logger.h"
#include "IndicatorManager.h"
#include <algorithm>
#include <cmath>


// Singleton instance
ChandelierStopManager& ChandelierStopManager::getInstance() {
    static ChandelierStopManager instance;
    return instance;
}

void ChandelierStopManager::InitializeStop(int positionID, bool isLong,
                                          double entryPrice, int entryBarIndex,
                                          double initialStop, double atrMultiplier) {
    std::lock_guard<std::mutex> lock(mutex_);

    StopInfo info;
    info.stopPrice = initialStop;
    info.highestHigh = isLong ? entryPrice : 0.0;
    info.lowestLow = isLong ? 0.0 : entryPrice;
    info.entryPrice = entryPrice;
    info.initialStopPrice = initialStop;
    info.isLong = isLong;
    info.isActive = false;  // Not trailing yet
    info.entryBarIndex = entryBarIndex;
    info.atrMultiplier = atrMultiplier;
    info.baseMultiplier = atrMultiplier;  // Store base for dynamic adjustment

    stops_[positionID] = info;

    SCString logMsg;
    logMsg.Format("Chandelier: Initialized for position %d | Direction: %s | Entry: %.2f | Initial Stop: %.2f | ATR Multiplier: %.2fx",
        positionID, isLong ? "LONG" : "SHORT", entryPrice, initialStop, atrMultiplier);
    Logger::getInstance().log(logMsg.GetChars());
}

bool ChandelierStopManager::UpdateStop(int positionID,
                                      double currentHigh, double currentLow,
                                      double atr, double atr10, double currentPrice,
                                      bool barHasClosed) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = stops_.find(positionID);
    if (it == stops_.end()) {
        return false;  // Position not tracked
    }

    StopInfo& info = it->second;

    // If trailing not active yet, use initial stop
    if (!info.isActive) {
        return false;
    }

    // === VOL-EXPANSION TIGHTENING (Elite Enhancement) ===
    // Detect "Climax Bars": range > 2× the 10-period ATR
    // During vertical moves, tighten stop from 3×ATR to 1.5×ATR to lock in gains
    //
    // Two-phase approach:
    //   Phase 1 (every tick): Detect climax intra-bar and activate tightening immediately.
    //           Track wasClimaxBarInProgress so we know the completed bar's character.
    //   Phase 2 (bar close):  Count completed normal bars for the 3-bar reset window.
    //           barHasClosed refers to the PREVIOUS bar closing, so currentRange at
    //           that moment is the new (tiny) bar — we use the flag instead.
    if (atr10 <= 0.0) {
        SCString logMsg;
        logMsg.Format("Chandelier: invalid ATR10 for position %d (atr10=%.4f)", positionID, atr10);
        Logger::getInstance().log(logMsg.GetChars());
        return false;
    }

    const double currentRange = currentHigh - currentLow;
    const double atr10Value = atr10;
    const double climaxThreshold = atr10Value * 2.0;

    const bool isClimaxBar = (currentRange > climaxThreshold);

    // Phase 1: Intra-bar climax detection — react immediately to vertical moves
    // Track the worst (highest) climax ratio seen during this forming bar so the
    // proportional multiplier reflects the peak intensity, not just the latest tick.
    const double climaxRatio = currentRange / std::max(atr10Value, 1e-9);  // range-to-ATR10 ratio

    if (isClimaxBar) {
        info.wasClimaxBarInProgress = true;
        info.peakClimaxRatio = std::max(info.peakClimaxRatio, climaxRatio);

        if (!info.isClimaxTightening) {
            info.isClimaxTightening = true;
            info.normalBarsCounter = 0;

            SCString logMsg;
            logMsg.Format("Chandelier: CLIMAX BAR detected for position %d | Range: %.2f > %.2f | Climax ratio: %.2fx ATR10",
                positionID, currentRange, climaxThreshold, climaxRatio);
            Logger::getInstance().log(logMsg.GetChars());
        }
    }

    // Phase 2: On bar close, evaluate the COMPLETED bar for normal-bar counting.
    // barHasClosed means the previous bar just closed — wasClimaxBarInProgress
    // tells us whether that completed bar ever exceeded the climax threshold.
    if (barHasClosed && info.isClimaxTightening) {
        if (info.wasClimaxBarInProgress) {
            // Completed bar was a climax bar — reset normal-bar counter
            info.normalBarsCounter = 0;
        } else {
            // Completed bar was normal — count it
            info.normalBarsCounter++;

            if (info.normalBarsCounter >= 3) {
                info.isClimaxTightening = false;
                info.normalBarsCounter = 0;

                SCString logMsg;
                logMsg.Format("Chandelier: Climax tightening RESET for position %d | 3 normal bars completed | Returning to base ATR multiplier",
                    positionID);
                Logger::getInstance().log(logMsg.GetChars());
            }
        }
        info.wasClimaxBarInProgress = false;  // Reset for the new forming bar
        info.peakClimaxRatio = 0.0;            // Reset peak intensity for new bar
    }

    // === OUTLIER WICK FILTER ===
    // Guard against phantom wicks (1-tick quote spikes) inflating the trailing anchor.
    // Only accept a new extreme if it's within a reasonable envelope of the current
    // close price. A wick beyond wickGuard × ATR from close is too far from where
    // the market actually trades and would cause the stop to trail from a phantom level.
    constexpr double wickGuard = 1.5;  // max distance from close, in ATR units
    const double wickEnvelope = wickGuard * atr;

    // Update highest high (for longs) or lowest low (for shorts)
    if (info.isLong) {
        // Accept new high only if it's within wickGuard ATRs of the close
        const double candidateHigh = currentHigh;
        if (candidateHigh > info.highestHigh) {
            if (currentPrice > 0.0 && (candidateHigh - currentPrice) > wickEnvelope) {
                // Wick is too far from close — cap to close + envelope
                info.highestHigh = std::max(info.highestHigh, currentPrice + wickEnvelope);
            } else {
                info.highestHigh = candidateHigh;
            }
        }
    } else {
        const double candidateLow = currentLow;
        if (info.lowestLow == 0.0 || candidateLow < info.lowestLow) {
            if (currentPrice > 0.0 && (currentPrice - candidateLow) > wickEnvelope) {
                // Wick is too far from close — cap to close - envelope
                info.lowestLow = (info.lowestLow == 0.0)
                    ? (currentPrice - wickEnvelope)
                    : std::min(info.lowestLow, currentPrice - wickEnvelope);
            } else {
                info.lowestLow = candidateLow;
            }
        }
    }

    // Calculate current profit and apply dynamic multiplier
    // Use provided currentPrice if available, otherwise infer from direction
    double priceForCalc = (currentPrice > 0.0) ? currentPrice : (info.isLong ? currentHigh : currentLow);
    double currentProfit = info.isLong ?
        (priceForCalc - info.entryPrice) :
        (info.entryPrice - priceForCalc);
    double initialRisk = std::abs(info.entryPrice - info.initialStopPrice);

    // Get dynamic multiplier based on profit level
    double dynamicMultiplier = GetDynamicMultiplier(currentProfit, initialRisk, info.baseMultiplier);

    // Single HMM lookup for both flush-floor and climax-suppression logic
    const auto* hmmInd = InferenceManager::Instance().HmmState();
    const bool isKineticFlush = hmmInd && (hmmInd->Value() == HMMStateEnum::PARETO_MOMENTUM);

    // P3.2: During PARETO_MOMENTUM, floor the dynamic multiplier at 3.5×ATR
    // so tightening thresholds don't choke big runners prematurely.
    if (isKineticFlush && dynamicMultiplier < 3.5) {
        dynamicMultiplier = 3.5;
    }

    // Apply climax tightening if active (overrides dynamic multiplier)
    // P1.6: Regime-aware climax override — During PARETO_MOMENTUM, suppress
    // climax tightening because the large range IS the directional move itself.
    // Tightening stops during a flush causes premature exit from trending trades.
    //
    // Proportional climax response: the tighter the multiplier, the more extreme
    // the climax was.  m_climax = max(floor, base - k * (ratio - threshold_ratio))
    // where ratio = peak range / ATR10.  At ratio=2 (barely climax) → small tighten.
    // At ratio=4 (blow-off) → tighten to floor (1.0).
    double effectiveMultiplier = dynamicMultiplier;
    if (info.isClimaxTightening) {
        if (isKineticFlush) {
            // Suppress climax tightening — keep dynamic multiplier to let trend run
            effectiveMultiplier = dynamicMultiplier;
        } else {
            // Proportional: how far above the 2.0 threshold is the peak ratio?
            constexpr double climaxFloor = 1.0;          // tightest possible multiplier
            constexpr double climaxThresholdRatio = 2.0;  // ratio at which climax starts
            constexpr double climaxK = 0.75;              // tightening rate per unit of excess ratio
            const double excessRatio = std::max(0.0, info.peakClimaxRatio - climaxThresholdRatio);
            effectiveMultiplier = std::max(climaxFloor,
                                          dynamicMultiplier - climaxK * excessRatio);
        }
    }

    // Update stored multiplier for reference
    info.atrMultiplier = effectiveMultiplier;

    // Calculate new stop using Chandelier formula (with dynamic multiplier)
    double newStop = CalculateChandelierStop(info, atr, effectiveMultiplier);

    // One-way movement: stop can only move favorably
    bool stopUpdated = false;
    if (info.isLong) {
        // Long: stop can only move UP
        if (newStop > info.stopPrice) {
            info.stopPrice = newStop;
            stopUpdated = true;
        }
    } else {
        // Short: stop can only move DOWN
        if (newStop < info.stopPrice || info.stopPrice == info.initialStopPrice) {
            info.stopPrice = newStop;
            stopUpdated = true;
        }
    }

    // Check for breakeven lock (only log once when crossing breakeven)
    if (info.isLong && info.stopPrice >= info.entryPrice) {
        if (breakevenLogged_.find(positionID) == breakevenLogged_.end()) {
            breakevenLogged_.insert(positionID);
            SCString logMsg;
            logMsg.Format("Chandelier: BREAKEVEN LOCKED for position %d | Entry: %.2f | Stop: %.2f | Guaranteed minimum profit: %.2f pts",
                positionID, info.entryPrice, info.stopPrice, info.stopPrice - info.entryPrice);
            Logger::getInstance().log(logMsg.GetChars());
        }
    } else if (!info.isLong && info.stopPrice <= info.entryPrice) {
        if (breakevenLogged_.find(positionID) == breakevenLogged_.end()) {
            breakevenLogged_.insert(positionID);
            SCString logMsg2;
            logMsg2.Format("Chandelier: BREAKEVEN LOCKED for position %d | Entry: %.2f | Stop: %.2f | Guaranteed minimum profit: %.2f pts",
                positionID, info.entryPrice, info.stopPrice, info.entryPrice - info.stopPrice);
            Logger::getInstance().log(logMsg2.GetChars());
        }
    }

    return stopUpdated;
}

double ChandelierStopManager::GetStopPrice(int positionID) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = stops_.find(positionID);
    if (it == stops_.end()) {
        return 0.0;
    }

    return it->second.stopPrice;
}

bool ChandelierStopManager::ShouldStopOut(int positionID, double currentPrice) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = stops_.find(positionID);
    if (it == stops_.end()) {
        return false;
    }

    const StopInfo& info = it->second;

    if (info.isLong) {
        // Long: stop out if price falls below stop
        return currentPrice <= info.stopPrice;
    } else {
        // Short: stop out if price rises above stop
        return currentPrice >= info.stopPrice;
    }
}

void ChandelierStopManager::RemoveStop(int positionID) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = stops_.find(positionID);
    if (it != stops_.end()) {
        SCString logMsg;
        logMsg.Format("Chandelier: Removed tracking for position %d", positionID);
        Logger::getInstance().log(logMsg.GetChars());

        stops_.erase(it);
        breakevenLogged_.erase(positionID);
    }
}

void ChandelierStopManager::ActivateTrailing(int positionID) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = stops_.find(positionID);
    if (it == stops_.end()) {
        SCString logMsg;
        logMsg.Format("Chandelier: WARNING - Cannot activate trailing for unknown position %d", positionID);
        Logger::getInstance().log(logMsg.GetChars());
        return;
    }

    StopInfo& info = it->second;
    if (!info.isActive) {
        info.isActive = true;

        SCString logMsg2;
        logMsg2.Format("Chandelier: ACTIVATED trailing for position %d | Entry: %.2f | Initial Stop: %.2f | Current Extreme: %.2f | Will trail with %.2fx ATR",
            positionID, info.entryPrice, info.initialStopPrice, info.isLong ? info.highestHigh : info.lowestLow, info.atrMultiplier);
        Logger::getInstance().log(logMsg2.GetChars());
    }
}

bool ChandelierStopManager::IsTrailingActive(int positionID) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = stops_.find(positionID);
    if (it == stops_.end()) {
        return false;
    }

    return it->second.isActive;
}

bool ChandelierStopManager::ForceTightenStop(int positionID, double tighterStop, double newBaseMultiplier) {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = stops_.find(positionID);
    if (it == stops_.end()) {
        return false;
    }

    StopInfo& info = it->second;
    const double currentStop = info.stopPrice;

    bool tightened = false;
    if (info.isLong) {
        if (tighterStop > currentStop) {
            info.stopPrice = tighterStop;
            tightened = true;
        }
    } else {
        if (tighterStop < currentStop) {
            info.stopPrice = tighterStop;
            tightened = true;
        }
    }

    if (!tightened) {
        return false;
    }

    if (newBaseMultiplier > 0.0) {
        info.baseMultiplier = newBaseMultiplier;
        info.atrMultiplier = newBaseMultiplier;
    }

    SCString logMsg;
    if (newBaseMultiplier > 0.0) {
        logMsg.Format("Chandelier: FORCE TIGHTEN for position %d | Old Stop: %.2f -> New Stop: %.2f | New Base Multiplier: %.2fx",
            positionID, currentStop, info.stopPrice, newBaseMultiplier);
    } else {
        logMsg.Format("Chandelier: FORCE TIGHTEN for position %d | Old Stop: %.2f -> New Stop: %.2f",
            positionID, currentStop, info.stopPrice);
    }
    Logger::getInstance().log(logMsg.GetChars());

    return true;
}

ChandelierStopManager::StopInfo ChandelierStopManager::GetStopInfo(int positionID) const {
    std::lock_guard<std::mutex> lock(mutex_);

    auto it = stops_.find(positionID);
    if (it == stops_.end()) {
        return StopInfo();  // Return empty info
    }

    return it->second;
}

void ChandelierStopManager::ClearAllStops() {
    std::lock_guard<std::mutex> lock(mutex_);

    size_t count = stops_.size();
    stops_.clear();
    breakevenLogged_.clear();

    SCString logMsg;
    logMsg.Format("Chandelier: Cleared all stops (%zu positions)", count);
    Logger::getInstance().log(logMsg.GetChars());
}

double ChandelierStopManager::CalculateChandelierStop(const StopInfo& info,
                                                      double atr,
                                                      double overrideMultiplier) const {
    // Use override multiplier if provided (for climax tightening), otherwise use stored multiplier
    double effectiveMultiplier = (overrideMultiplier > 0.0) ? overrideMultiplier : info.atrMultiplier;

    if (info.isLong) {
        // Long: Stop = Highest High - (multiplier × ATR)
        // Use info.highestHigh directly — it is already wick-filtered in UpdateStop().
        // Do NOT re-introduce raw currentHigh here; that bypasses the outlier guard.
        return info.highestHigh - (effectiveMultiplier * atr);
    } else {
        // Short: Stop = Lowest Low + (multiplier × ATR)
        // Use info.lowestLow directly — already wick-filtered.
        return info.lowestLow + (effectiveMultiplier * atr);
    }
}

bool ChandelierStopManager::ShouldUseTrailingStop(RaschkeTacticalTrigger patternTrigger) {
    // Only trail on trend continuation patterns (from RaschkeTacticalTrigger enum)
    // Skip trailing stops on mean-reversion patterns (quick scalps with fixed targets)

    switch (patternTrigger) {
        // Trend continuation patterns that benefit from trailing stops
        case RaschkeTacticalTrigger::ELDER_BREAKOUT_BUY:
        case RaschkeTacticalTrigger::ELDER_BREAKOUT_SELL:
        case RaschkeTacticalTrigger::ITR_BREAKOUT_BUY:
        case RaschkeTacticalTrigger::ITR_BREAKOUT_SELL:
            return true;

        // Mean-reversion / scalar patterns (fixed targets, no trailing)
        case RaschkeTacticalTrigger::MOMENTUM_PINBALL_BUY:
        case RaschkeTacticalTrigger::MOMENTUM_PINBALL_SELL:
        case RaschkeTacticalTrigger::TURTLE_SOUP_BUY:
        case RaschkeTacticalTrigger::TURTLE_SOUP_SELL:
        case RaschkeTacticalTrigger::KANGAROO_TAIL_BUY:
        case RaschkeTacticalTrigger::KANGAROO_TAIL_SELL:
        case RaschkeTacticalTrigger::STOCHASTIC_POP_BUY:
        case RaschkeTacticalTrigger::STOCHASTIC_POP_SELL:
            return false;

        case RaschkeTacticalTrigger::NONE:
        default:
            return false;  // Conservative default - no trailing
    }
}

double ChandelierStopManager::GetDynamicMultiplier(double profit, double risk, double baseMultiplier) {
    // Calculate R-multiple (profit in terms of initial risk)
    if (risk <= 0.0) {
        return baseMultiplier;  // No adjustment if risk unknown
    }

    double rMultiple = profit / risk;

    // Smooth exponential decay: as profit grows, ATR multiplier tightens continuously.
    //   m(R) = floor + (base - floor) * exp(-lambda * max(0, R - onset))
    //
    // Parameters:
    //   floor  = 2.0  (tightest multiplier at very large R)
    //   onset  = 2.0  (R below this keeps base multiplier — let trade breathe)
    //   lambda = 0.35 (decay rate — half-life ≈ 2R beyond onset)
    //
    // This eliminates the discrete 3.0→2.5→2.0 cliff-edges that caused stop
    // chatter at the 4R and 6R boundaries.
    constexpr double floor  = 2.0;
    constexpr double onset  = 2.0;
    constexpr double lambda = 0.35;

    if (rMultiple <= onset) {
        return baseMultiplier;
    }

    return floor + (baseMultiplier - floor) * std::exp(-lambda * (rMultiple - onset));
}
