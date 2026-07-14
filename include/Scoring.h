#pragma once
#include "MindfulTrader_Precompiled.h"
#include <map>
#include <string>
#include <vector>
#include <cmath>
#include "Indicator.h"

// Forward declaration — full definition in ContextManager.h (available via PCH)
struct LocalRiskContext;

// Pattern Types Enum for type-safe lookups (mirrors Python string keys)
enum class PatternType {
    KangarooTail,
    MomentumPinball,
    ElderBreakout,
    TurtleSoup,
    NR7,
    Unknown
};

class Scoring {
public:
    static Scoring& Instance() {
        static Scoring instance;
        // Ensure multipliers are initialized on first access
        if (!instance.m_initialized) {
            instance.Init();
        }
        return instance;
    }

    // Initialize/Load multipliers (if needed)
    void Init();

    // === Core Scoring Algorithms ===

    // Elite multiplicative penalty protection: Compound bonuses, average penalties
    double CalculateEliteCompositeMultiplier(const std::vector<double>& multipliers) const;

    // Tanh normalization to prevent gradient explosion
    double NormalizeScore(double raw_score, double scale = 100.0) const;

    // === Context Multipliers ===

    // Get win rate-based multiplier for pattern in given HMM state
    double GetHMMMultiplier(PatternType pattern, HMMStateEnum state) const;

    // Get multiplier for pattern in given Market Climate
    double GetClimateMultiplier(PatternType pattern, MarketClimate climate) const;
    
    // ELITE v3.2: Deep Context Multiplier (Institutional Grade)
    // Uses LocalRiskContext (ContextManager single source of truth) to apply
    // continuous, non-linear scoring adjustments including:
    // Shannon entropy, Taleb fragility, Hurst trend quality, VPIN toxicity,
    // Taleb cliff proximity, Fisher info reliability
    double GetDeepContextMultiplier(PatternType pattern, const LocalRiskContext& ctx) const;

    // ELITE v2.6: Daily Bias & Market Structure Filter
    // Applies strict institutional bias rules (Veto vs. Boost)
    // Returns: Filtered Action (e.g. STAND_ASIDE if vetoed) and Adjusted Multiplier
    // Note: Mirrors Python's apply_daily_bias_filter for consistency
    struct BiasFilterResult {
        bool allowed;
        double multiplier_adjustment;
        std::string reason;
    };
    BiasFilterResult ApplyDailyBiasFilter(PatternType pattern, bool isLong, DailyBiasEnum bias) const;

    // Filter indicator noise in high-entropy regimes
    // Returns TRUE if the change is significant enough to publish an event
    bool IsIndicatorEventSignificant(IndicatorKey indicatorKey, const LocalRiskContext& ctx) const;

    // Phase 1: Event-driven inference utilities
    // dirtyCount: Measures signal flicker/instability within the bar (fewer is better)
    double GetIntrabarConfidenceMultiplier(double timeIntoBar, bool isBarClose, int dirtyCount) const;
    double GetEventVelocityMultiplier(double eventsPerMin) const;

    // Helper to convert string to PatternType (if needed for bridging)
    PatternType StringToPatternType(const std::string& name) const;

private:
    Scoring() = default;
    ~Scoring() = default;
    Scoring(const Scoring&) = delete;
    Scoring& operator=(const Scoring&) = delete;

    // Multiplier Maps
    // Outer key: PatternType, Inner key: HMMStateEnum (as int) -> Multiplier
    std::map<PatternType, std::map<int, double>> m_hmmMultipliers;

    // Outer key: PatternType, Inner key: MarketClimate (as int) -> Multiplier
    std::map<PatternType, std::map<int, double>> m_climateMultipliers;

    void InitializeMultipliers();

    bool m_initialized = false;
};
