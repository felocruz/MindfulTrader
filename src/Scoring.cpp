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

double Scoring::CalculateEliteCompositeMultiplier(const double* multipliers, size_t count) const {
    if (multipliers == nullptr || count == 0) {
        return 1.0;
    }

    // Separate positive (bonuses > 1.0) and negative (penalties < 1.0)
    double bonus_product = 1.0;
    double penalty_sum = 0.0;
    int penalty_count = 0;

    for (size_t i = 0; i < count; ++i) {
        const double m = multipliers[i];
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
// "The Gang" Rules Implementation (continuous, not discrete tiers):
// 1. Taleb (Kurtosis): fragility penalty via sigmoid 1/(1+exp(steepness*(kurtosis-
//    center))), gated on kurtosis > gate, with NO artificial floor. gate/center/
//    steepness are all re-derived for the Moors-kurtosis scale (Task 7) -- see
//    the call site below. Steepness was NOT left at the old 0.5: the Moors
//    distribution is ~20x more compressed (std 0.37 vs old 7.4), so the old
//    slope value would have made this control nearly inert on the new scale
//    (empirically verified: unchanged 0.5 would drop the fraction of real
//    samples reaching a strong risk-off penalty <0.1 from 13.6% to 0.056%,
//    a >200x reduction). Re-derived empirically from the real 57,256-sample
//    paired distribution to reproduce the SAME real-world trigger frequency
//    (13.6% of samples land below penalty 0.1) -- see call site.
// 2. Shannon (Entropy, in BITS): thresholds are normalized H/Hmax ratios scaled by
//    kShannonMaxEntropyBits (=log2(NUM_BINS)). High entropy boosts mean-reversion and
//    cuts trend (hard-zero for patterns in neither bucket); low entropy boosts trend.
// 3. Hurst: quality > 0.70 => trend/breakout boost.
// Plus session-aware Amihud, spread-stress, Taleb-cliff, Fisher, and mean-rev-Z gates.
//
// Returns: multiplier unbounded below (can reach 0.0 = hard kill); no fixed [0.6,1.5] range.
// ============================================================================
double Scoring::GetDeepContextMultiplier(PatternType pattern, const LocalRiskContext& ctx) const {
    double multiplier = 1.0;

    // --- 1. FRAGILITY PENALTY (Taleb Kurtosis) — No artificial floor ---
    // gate=2.5 (old) -> P30.5 -> 1.3248 (new); center=6.0 (old) -> P70.6 -> 1.6414
    // (new), percentile-matched on real MES data -- see
    // tools/analyze_kurtosis_threshold_migration.py, run 2026-08-13 (Task 7,
    // .superpowers/sdd/2026-08-13-observation-vector-institutional-elevation/
    // task-7-report.md).
    // Steepness (9.7409, was 0.5) derived empirically, not percentile-matched
    // (slope isn't a point threshold): solved in closed form for the value
    // that makes penalty(new_kurtosis)<0.1 fire on the SAME real-sample
    // fraction (13.6%) that penalty(old_kurtosis)<0.1 fired on with the old
    // 0.5 steepness -- i.e. same real-world "strong risk-off" trigger rate,
    // just re-expressed on the new scale. Cross-checked against the
    // closed-form variance-ratio approximation steepness_new ≈ steepness_old
    // * sqrt(Var(old)/Var(new)) = 0.5 * sqrt(7.4066^2/0.3739^2) ≈ 9.91 --
    // within 2% of the empirically-derived value, corroborating it.
    if (ctx.talebKurtosis > 1.3248f) {
        const double fragilityPenalty = 1.0 / (1.0 + std::exp(9.7409 * (static_cast<double>(ctx.talebKurtosis) - 1.6414)));
        multiplier *= fragilityPenalty;
    }

    // --- 2. ENTROPY ADJUSTMENT (Shannon) ---
    // shannonFlowEntropy is in BITS; thresholds below are normalized H/Hmax ratios
    // scaled by kShannonMaxEntropyBits = log2(10) (Finding 18).
    // ENTROPY SCALE NOTE (2026-08-13, final-review Finding 7): GetShannonEntropy()
    // now subtracts a Miller-Madow bias term, shifting entropy DOWN ~4% at the
    // steady-state window (N=50), up to ~20% during warmup (N=10). These three
    // bands were evaluated and deliberately NOT re-derived: the shift is an order
    // of magnitude smaller than the kurtosis 3.0 -> 1.233 change handled just
    // above by the fragility sigmoid (which DID get a full empirical
    // re-derivation, Task 7, including its slope). Full rationale at
    // InformationEngine.h's GetShannonEntropy() doc comment.
    bool isMeanReversionPattern = (pattern == PatternType::TurtleSoup || pattern == PatternType::KangarooTail);
    bool isTrendPattern = (pattern == PatternType::ElderBreakout || pattern == PatternType::MomentumPinball);
    // NR7 (Crabel 1990) is a volatility-compression breakout-anticipation setup whose payoff is a
    // DIRECTIONAL range expansion. Like the trend/breakout patterns it needs informative
    // (low-entropy) order flow to follow through -- balanced (high-entropy) flow yields the classic
    // failed-breakout-in-chop -- so its flow-entropy and trend-quality response belongs to the
    // directional family. Classifying it EXPLICITLY (Finding 9) stops it falling into the Unknown
    // catch-all below, which previously hard-zeroed NR7 at high entropy and silently annihilated
    // every other NR7 multiplier. Coherent with the encoded NR7 thesis (HMM/climate tables:
    // GAUSSIAN_STABLE/COILED_SPRING favored, SHANNON_CHAOS penalized).
    bool isDirectionalPattern = isTrendPattern || (pattern == PatternType::NR7);

    if (ctx.shannonFlowEntropy > 0.80f * kShannonMaxEntropyBits) {
        if (isMeanReversionPattern) multiplier *= 1.25;     // chaos favors fading extremes
        else if (isDirectionalPattern) multiplier *= 0.70;  // chaos kills breakouts (incl. NR7 expansion)
        else multiplier *= 0.0;                             // Unknown/aggregate path: conservative floor (backstopped by the 0.90 hard veto)
    } else if (ctx.shannonFlowEntropy > 0.60f * kShannonMaxEntropyBits) {
        if (!isMeanReversionPattern) multiplier *= 0.50;
    } else if (ctx.shannonFlowEntropy < 0.45f * kShannonMaxEntropyBits) {
        if (isDirectionalPattern) multiplier *= 1.20;       // order favors persistence & clean breakouts
        else if (isMeanReversionPattern) multiplier *= 0.80;
        else multiplier *= 1.10;
    }

    // --- 3. TREND QUALITY (Hurst persistence) ---
    if ((isDirectionalPattern || pattern == PatternType::Unknown) && ctx.hurstExponent > 0.70f) {
        multiplier *= 1.15;
    }

    // --- 4. REGIME DURATION (Lindy Effect) ---
    if (ctx.regimeDuration > 20 && ctx.regimeDuration < 100) {
        multiplier *= 1.05;
    } else if (ctx.regimeDuration > 150) {
        multiplier *= 0.95;
    }

    // --- 5. AMIHUD TOXICITY GATE (session-aware rolling percentile; Layer B) ---
    // Gate on the stationary rolling percentile of Amihud illiquidity (not the raw,
    // non-stationary value). Mirrors the hard-gate canon: kill at p90, halve at p75.
    if (ctx.amihudPercentile > 0.90f) {
        multiplier = 0.0; // hard kill — toxic flow
    } else if (ctx.amihudPercentile > 0.75f) {
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

    if (ctx.shannonFlowEntropy > 0.85f * kShannonMaxEntropyBits) {
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
            indicatorKey == IndicatorKey::SHORT_MKT_ACTION ||
            indicatorKey == IndicatorKey::INTERM_MKT_ACTION) {
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
