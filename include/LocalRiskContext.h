#pragma once

// LocalRiskContext.h — ACSIL-independent home for LocalRiskContext, extracted from
// ContextManager.h so headers composing it (e.g. PredatorContext.h) stay includable
// with just `-I include` (no sierrachart.h on the path) — same rationale as
// MacdEnum's extraction from Indicator.h into IndicatorComputations.h.

#include <cstdint>

/// Elite v3.2: Unified Local Risk Context
/// Single struct exposing all locally-computed Gang (Shannon/Taleb/Pareto) intelligence
/// to RiskManager, PositionManager, and Scoring without Python round-trip.
/// Replaces the fragmented ClimateMetrics ↔ InstitutionalMetrics ↔ NormalizedAnchors paths.
struct LocalRiskContext {
    // Shannon (Information Theory) — from InformationEngine
    float shannonFlowEntropy = 0.0f;   // bits — flow entropy (disambiguated from schema-level Shannon)
    float shannonEfficiency = 0.5f;    // 1 - H/Hmax — signal-to-noise ratio

    // Taleb (Tail Risk) — from NormalizedAnchors + TailRiskEngine
    float talebKurtosis = 0.0f;        // Moors (1988) octile kurtosis (1.233 = N(0,1) neutral, clamped [0,5])
    float talebSkewness = 0.0f;        // Bowley (1920) quartile skewness (0 = symmetric, bounded [-1,+1])
    float elderChandelierATR = 0.0f;   // Elder's Chandelier: (price - stop) / ATR distance
    float paretoTailAlpha = 4.0f;      // Pareto tail index via Hill estimator (4.0 = safe default)

    // Pareto (Structure/Flow) — from StructureEngine + ObservationData
    float amihudIlliquidity = 0.0f;    // Amihud illiquidity (canonical: mean |log-ret| / dollar-volume; formerly 'vpin')
    float amihudPercentile = 0.5f;     // Layer B: session-aware rolling percentile [0,1] of amihudIlliquidity — the actual gate input (p90 normal / p75 fat-tail)
    float spreadStress = 0.0f;         // spread-stress fragility
    float hurstExponent = 0.5f;        // persistence (>0.5 trending, <0.5 mean-reverting)
    float fractalDim = 1.5f;           // roughness 1.0-2.0
    float meanRevZ = 0.0f;             // distance from mean in sigma

    // Raschke (Event Dynamics)
    float raschkeBurst = 1.0f;         // CV of inter-arrival times (clustering)

    // Fisher (Observation Reliability)
    float fisherInfo = 0.0f;           // sharpness of current estimate

    // Regime Duration (from MarketClimateIndicator)
    int regimeDuration = 0;            // bars in current climate state

    bool isValid = false;              // true after warmup
    uint64_t snapshotTimestampUs = 0;  // D.6: Wall-clock time (system_clock, µs) when context was last refreshed
};
