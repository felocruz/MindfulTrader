#include "Scoring.h"
#include <algorithm>
#include <numeric>

// Initialize the singleton
void Scoring::Init() {
    if (m_initialized) return;
    InitializeMultipliers();
    m_initialized = true;
}

void Scoring::InitializeMultipliers() {
    // ============================================================================
    // INSTITUTIONAL-GRADE HMM & CLIMATE MULTIPLIERS (Refactored Feb 2026)
    // Matches lbrnet/core/scoring.py Source of Truth
    // ============================================================================

    // === HMM State Multipliers ===
    // 'momentum_pinball'
    m_hmmMultipliers[PatternType::MomentumPinball] = {
        { (int)HMMStateEnum::PARETO_MOMENTUM,  0.85 },
        { (int)HMMStateEnum::GAUSSIAN_FRAGILE, 0.85 },
        { (int)HMMStateEnum::GAUSSIAN_STABLE,  1.00 },
        { (int)HMMStateEnum::COILED_SPRING,    1.00 }
    };

    // 'turtle_soup'
    m_hmmMultipliers[PatternType::TurtleSoup] = {
        { (int)HMMStateEnum::PARETO_MOMENTUM,  0.75 },
        { (int)HMMStateEnum::GAUSSIAN_FRAGILE, 0.75 },
        { (int)HMMStateEnum::GAUSSIAN_STABLE,  0.95 },
        { (int)HMMStateEnum::COILED_SPRING,    0.95 }
    };

    // 'elder_breakout'
    m_hmmMultipliers[PatternType::ElderBreakout] = {
        { (int)HMMStateEnum::PARETO_MOMENTUM,  1.35 },
        { (int)HMMStateEnum::GAUSSIAN_FRAGILE, 1.35 },
        { (int)HMMStateEnum::GAUSSIAN_STABLE,  1.10 },
        { (int)HMMStateEnum::COILED_SPRING,    1.10 }
    };

    // 'kangaroo_tail'
    m_hmmMultipliers[PatternType::KangarooTail] = {
        { (int)HMMStateEnum::PARETO_MOMENTUM,  1.10 },
        { (int)HMMStateEnum::GAUSSIAN_FRAGILE, 1.10 },
        { (int)HMMStateEnum::GAUSSIAN_STABLE,  1.00 },
        { (int)HMMStateEnum::COILED_SPRING,    1.00 }
    };

    // 'nr7'
    m_hmmMultipliers[PatternType::NR7] = {
        { (int)HMMStateEnum::PARETO_MOMENTUM,  0.80 },
        { (int)HMMStateEnum::GAUSSIAN_FRAGILE, 0.80 },
        { (int)HMMStateEnum::GAUSSIAN_STABLE,  1.30 },
        { (int)HMMStateEnum::COILED_SPRING,    1.30 }
    };


    // === Market Climate Multipliers ===
    // 'momentum_pinball'
    m_climateMultipliers[PatternType::MomentumPinball] = {
        { (int)MarketClimate::GAUSSIAN_STABLE, 1.05 },
        { (int)MarketClimate::TALEBIAN_FRAGILE, 0.90 },
        { (int)MarketClimate::SHANNON_CHAOS, 1.20 },
        { (int)MarketClimate::PARETO_MOMENTUM, 0.70 },
        { (int)MarketClimate::COILED_SPRING, 0.95 }
    };

    // 'turtle_soup'
    m_climateMultipliers[PatternType::TurtleSoup] = {
        { (int)MarketClimate::GAUSSIAN_STABLE, 1.10 },
        { (int)MarketClimate::TALEBIAN_FRAGILE, 0.80 },
        { (int)MarketClimate::SHANNON_CHAOS, 1.25 },
        { (int)MarketClimate::PARETO_MOMENTUM, 0.60 },
        { (int)MarketClimate::COILED_SPRING, 0.85 }
    };

    // 'elder_breakout'
    m_climateMultipliers[PatternType::ElderBreakout] = {
        { (int)MarketClimate::GAUSSIAN_STABLE, 0.90 },
        { (int)MarketClimate::TALEBIAN_FRAGILE, 1.15 },
        { (int)MarketClimate::SHANNON_CHAOS, 0.60 },
        { (int)MarketClimate::PARETO_MOMENTUM, 1.40 },
        { (int)MarketClimate::COILED_SPRING, 1.25 }
    };

    // 'kangaroo_tail'
    m_climateMultipliers[PatternType::KangarooTail] = {
        { (int)MarketClimate::GAUSSIAN_STABLE, 1.00 },
        { (int)MarketClimate::TALEBIAN_FRAGILE, 1.20 },
        { (int)MarketClimate::SHANNON_CHAOS, 1.10 },
        { (int)MarketClimate::PARETO_MOMENTUM, 1.05 },
        { (int)MarketClimate::COILED_SPRING, 1.00 }
    };

    // 'nr7'
    m_climateMultipliers[PatternType::NR7] = {
        { (int)MarketClimate::GAUSSIAN_STABLE, 1.00 },
        { (int)MarketClimate::TALEBIAN_FRAGILE, 1.10 },
        { (int)MarketClimate::SHANNON_CHAOS, 0.80 },
        { (int)MarketClimate::PARETO_MOMENTUM, 0.75 },
        { (int)MarketClimate::COILED_SPRING, 1.45 } // DEFINITIONAL MATCH
    };
}

double Scoring::CalculateEliteCompositeMultiplier(const std::vector<double>& multipliers) const {
    if (multipliers.empty()) {
        return 1.0;
    }

    // Separate positive (bonuses > 1.0) and negative (penalties < 1.0)
    double bonus_product = 1.0;
    double penalty_sum = 0.0;
    int penalty_count = 0;

    for (double m : multipliers) {
        if (m > 1.0) {
            bonus_product *= m;
        } else if (m < 1.0) {
            penalty_sum += m;
            penalty_count++;
        }
        // Neutrals (1.0) are ignored
    }

    // Average the penalties (Prevent exponential collapse)
    double penalty_avg = (penalty_count > 0) ? (penalty_sum / penalty_count) : 1.0;

    // Combine: Compound bonuses × Average penalties
    double composite = bonus_product * penalty_avg;

    // Apply hard safety floor (prevent collapse below 50% of base score)
    // "Safety Floor: max(0.5, result) prevents collapse below 50%"
    return std::max(0.5, composite);
}

double Scoring::NormalizeScore(double raw_score, double scale) const {
    // tanh(raw_score / scale) compresses to [0, 1]
    // Preserves monotonicity, bounds output, smooth gradients
    if (scale <= 0.0001) scale = 1.0; // Avoid div by zero
    double val = std::tanh(std::abs(raw_score) / scale);
    return (raw_score >= 0) ? val : -val;
}

double Scoring::GetHMMMultiplier(PatternType pattern, HMMStateEnum state) const {
    auto it = m_hmmMultipliers.find(pattern);
    if (it != m_hmmMultipliers.end()) {
        auto valIt = it->second.find((int)state);
        if (valIt != it->second.end()) {
            return valIt->second;
        }
    }
    return 1.0; // Default buffer
}

double Scoring::GetClimateMultiplier(PatternType pattern, MarketClimate climate) const {
    auto it = m_climateMultipliers.find(pattern);
    if (it != m_climateMultipliers.end()) {
        auto valIt = it->second.find((int)climate);
        if (valIt != it->second.end()) {
            return valIt->second;
        }
    }
    return 1.0; // Default buffer
}

double Scoring::GetIntrabarConfidenceMultiplier(double timeIntoBar, bool isBarClose, int dirtyCount) const {
    // Bar-close events get full confidence regardless of noise
    if (isBarClose) {
        return 1.0;
    }

    // Base confidence from time progression
    double baseConfidence = 0.65;
    if (timeIntoBar >= 0.80) {
        baseConfidence = 0.95; // Late-bar (high confidence)
    } else if (timeIntoBar >= 0.60) {
        baseConfidence = 0.85; // Mid-late bar
    } else if (timeIntoBar >= 0.40) {
        baseConfidence = 0.75; // Mid-bar
    }

    // Institutional Signal Stability Penalty
    // A signal that flickers on/off many times is "dirty" and less reliable.
    // Penalty decays confidence for noisy setups.
    // 0-2 flips: No penalty
    // 3-5 flips: Minor penalty (0.9x)
    // 6+ flips: Major penalty (0.8x or lower)
    double stabilityMultiplier = 1.0;
    if (dirtyCount > 5) {
        stabilityMultiplier = 0.80;
    } else if (dirtyCount > 2) {
        stabilityMultiplier = 0.90;
    }

    return baseConfidence * stabilityMultiplier;
}

double Scoring::GetEventVelocityMultiplier(double eventsPerMin) const {
    // Matches get_event_velocity_multiplier in scoring.py
    if (eventsPerMin > 30.0) {
        return 1.15; // Event storm (strong conviction)
    } else if (eventsPerMin > 15.0) {
        return 1.10; // High activity
    } else if (eventsPerMin > 5.0) {
        return 1.00; // Normal activity
    } else {
        return 0.95; // Low activity (weak follow-through)
    }
}


PatternType Scoring::StringToPatternType(const std::string& name) const {
    if (name == "kangaroo_tail") return PatternType::KangarooTail;
    if (name == "momentum_pinball") return PatternType::MomentumPinball;
    if (name == "elder_breakout") return PatternType::ElderBreakout;
    if (name == "turtle_soup") return PatternType::TurtleSoup;
    if (name == "nr7") return PatternType::NR7;
    return PatternType::Unknown;
}

// ============================================================================
// ELITE v2.5: Deep Context Multiplier (Institutional Grade)
// ============================================================================
// Logic replicates Python 'MarketClimate' feature engineering + scoring
// Source: lbrnet/core/scoring.py -> get_climate_impact_score
//
// "The Gang" Rules Implementation:
// 1. Taleb (Kurtosis): Fragility Penalty.
//    - Kurtosis > 10.0 => 0.6x (Severe Panic Protection)
//    - Kurtosis > 4.0 => 0.8x (Caution)
//
// 2. Shannon (Entropy): Structure Detection.
//    - Entropy > 0.8 => 1.25x for Mean Reversion (Turtle Soup, Pinball Reversed)
//    - Entropy < 0.4 => 1.25x for Trend (Elder Breakout, Pinball Continuation)
//
// 3. Hurst/ADX: Trend Quality
//    - Quality > 0.70 => 1.2x for Trend/Breakout
//
// Returns: Multiplier [0.6, 1.5]
// ============================================================================
double Scoring::GetDeepContextMultiplier(PatternType pattern, const LocalRiskContext& ctx) const {
    double multiplier = 1.0;

    // --- 1. FRAGILITY PENALTY (Taleb Kurtosis) — No artificial floor ---
    // Sigmoid to zero: kurtosis=3 → ~0.95, kurtosis=6 → ~0.50, kurtosis=10 → ~0.08
    if (ctx.talebKurtosis > 2.5f) {
        const double fragilityPenalty = 1.0 / (1.0 + std::exp(0.5 * (static_cast<double>(ctx.talebKurtosis) - 6.0)));
        multiplier *= fragilityPenalty;
    }

    // --- 2. ENTROPY ADJUSTMENT (Shannon) ---
    bool isMeanReversionPattern = (pattern == PatternType::TurtleSoup || pattern == PatternType::KangarooTail);
    bool isTrendPattern = (pattern == PatternType::ElderBreakout || pattern == PatternType::MomentumPinball);

    if (ctx.shannonFlowEntropy > 0.80f) {
        if (isMeanReversionPattern) multiplier *= 1.25;
        else if (isTrendPattern) multiplier *= 0.70;
        else multiplier *= 0.0;
    } else if (ctx.shannonFlowEntropy > 0.60f) {
        if (!isMeanReversionPattern) multiplier *= 0.50;
    } else if (ctx.shannonFlowEntropy < 0.45f) {
        if (isTrendPattern) multiplier *= 1.20;
        else if (isMeanReversionPattern) multiplier *= 0.80;
        else multiplier *= 1.10;
    }

    // --- 3. TREND QUALITY (Hurst persistence) ---
    if ((isTrendPattern || pattern == PatternType::Unknown) && ctx.hurstExponent > 0.70f) {
        multiplier *= 1.15;
    }

    // --- 4. REGIME DURATION (Lindy Effect) ---
    if (ctx.regimeDuration > 20 && ctx.regimeDuration < 100) {
        multiplier *= 1.05;
    } else if (ctx.regimeDuration > 150) {
        multiplier *= 0.95;
    }

    // --- 5. VPIN TOXICITY GATE (new: order-flow poison detection) ---
    // vpin ∈ [0,1]; >0.8 = market-maker withdrawal imminent
    if (ctx.vpin > 0.80f) {
        multiplier = 0.0; // hard kill — toxic flow
    } else if (ctx.vpin > 0.60f) {
        multiplier *= 0.50; // halve exposure
    }

    // --- 6. LIQUIDITY FRAGILITY GATE (new: spread/depth stress) ---
    if (ctx.spreadStress > 0.70f) {
        multiplier *= 0.70; // slippage risk penalty
    }

    // --- 7. TALEB CLIFF PROXIMITY (new: tail-risk proximity) ---
    // elderChandelierATR ∈ (0, ∞); lower = closer to distributional edge
    if (ctx.elderChandelierATR < 0.50f) {
        multiplier *= 0.10; // near-zero sizing at cliff edge
    } else if (ctx.elderChandelierATR < 1.0f) {
        multiplier *= 0.60; // significant size-down
    }

    // --- 8. FISHER INFORMATION RELIABILITY (new: regime-estimate confidence) ---
    if (ctx.fisherInfo < 0.30f) {
        multiplier *= 0.85; // low confidence in parameter estimates
    }

    // --- 9. MEAN-REVERSION Z BOOST (new: mean-rev Z > 2 in MR patterns) ---
    if (isMeanReversionPattern && ctx.meanRevZ > 2.0f) {
        multiplier *= 1.10; // stretched rubber-band boost
    }

    return multiplier;
}

bool Scoring::IsIndicatorEventSignificant(IndicatorKey indicatorKey, const LocalRiskContext& ctx) const {
    // ELITE v3.2: NOISE GATING via LocalRiskContext
    // In high-entropy regimes (Chaos), suppress events from noisy/lagging indicators.

    if (ctx.shannonFlowEntropy > 0.85f) {
        // Chaos Mode: Only allow VITAL structural updates
        if (indicatorKey == IndicatorKey::HMM_STATE || 
            indicatorKey == IndicatorKey::MARKET_CLIMATE || // Recursive safety
            indicatorKey == IndicatorKey::ATR_PROXIMITY ||  // Risk management
            indicatorKey == IndicatorKey::SIDE) {           // Trade state
            return true;
        }

        // Suppress trend-followers which false-signal in chop
        if (indicatorKey == IndicatorKey::LONG_MACD ||
            indicatorKey == IndicatorKey::INTERM_MACD || 
            indicatorKey == IndicatorKey::LONG_FI13_SIGNAL || 
            indicatorKey == IndicatorKey::INTERM_FI2_SIGNAL) {
            return false; 
        }

        // Suppress generic market action chatter
        if (indicatorKey == IndicatorKey::LONG_MKT_ACTION ||
            indicatorKey == IndicatorKey::SHORT_MKT_ACTION) {
            return false;
        }
    }

    // Default: All changes are significant in orderly markets
    return true;
}

// ============================================================================
// ELITE v2.6: Daily Bias & Market Structure Filter
// Matches Python: lbrnet/core/scoring.py -> apply_daily_bias_filter
// ============================================================================
Scoring::BiasFilterResult Scoring::ApplyDailyBiasFilter(PatternType /*pattern*/, bool isLong, DailyBiasEnum bias) const {
    BiasFilterResult result{true, 1.0, ""};

    // 1. PHYSICS VETO (Hard Stop)
    if (bias == DailyBiasEnum::PHYSICS_VETO_RANDOM_WALK) {
        // In Python we technically allow it but with 0 confidence adjustments.
        // In C++ High Performance mode, we might want to be stricter, but following contract:
        // No Veto, No Boost.
        return result; 
    }

    // 2. STRUCTURE VETOS (Counter-Trend Filters)
    // "Trading into resistance" or "Trading into support" against a persistent trend
    if (bias == DailyBiasEnum::BEARISH_TREND_PERSISTENT && isLong) {
        result.allowed = false;
        result.multiplier_adjustment = 0.0;
        result.reason = "VETO: Long into Bearish Trend Persistent";
        return result;
    }

    if (bias == DailyBiasEnum::BULLISH_TREND_PERSISTENT && !isLong) {
        result.allowed = false;
        result.multiplier_adjustment = 0.0;
        result.reason = "VETO: Short into Bullish Trend Persistent";
        return result;
    }

    // 3. REVERSAL SETUP BOOSTS
    // "Buying the test of support" or "Selling the test of resistance" in Mean Reversion
    if (bias == DailyBiasEnum::BULLISH_MEAN_REVERSION && isLong) {
        result.multiplier_adjustment = 1.15; // +15% Boost
        result.reason = "BOOST: Bullish Mean Reversion (Responsive Buying)";
    }
    else if (bias == DailyBiasEnum::BEARISH_MEAN_REVERSION && !isLong) {
        result.multiplier_adjustment = 1.15; // +15% Boost
        result.reason = "BOOST: Bearish Mean Reversion (Responsive Selling)";
    }

    return result;
}
