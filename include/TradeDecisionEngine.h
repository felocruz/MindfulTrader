#pragma once

#include <string>
#include <cmath>
#include <algorithm>
#include <fstream>
#include <mutex>
#include "nlohmann/json.hpp"
#include "Logger.h"
#include "RejectionLedger.h"  // Reuse timestamp helpers + ContextSnapshot

// ═══════════════════════════════════════════════════════════════════════════════
// Trade Decision Engine (TDE) — Shadow-Mode Observation Layer
//
// PAER §9 Phase 1: Computes shadow metrics on every trade opportunity
// WITHOUT changing execution behavior.  The legacy ValidateOrder chain
// remains the sole decision-maker.  TDE observes in parallel.
//
// Components:
//   Layer 1: TradingClearance — 8 env booleans (did each gate pass?)
//   Layer 2: ConvictionScore  — weighted signal quality scalar [0, 1]
//   Layer 3: RiskPrice        — dollar-denominated risk per contract
//   Layer 4: TradeDecision    — synthesis (would TDE have agreed?)
//   Layer 5: OpportunityLedger — JSONL record of every opportunity
//
// All metrics are SHADOW-ONLY.  They are logged for offline calibration.
// No execution logic reads these values.
// ═══════════════════════════════════════════════════════════════════════════════

// ─── Layer 1: TradingClearance (CAN we trade?) ──────────────────────────────
// Every gate evaluates even if an upstream gate fails.
// This is the key difference from the short-circuit legacy chain.

struct TradingClearance {
    bool connectivity   = false;
    bool latency        = false;
    bool timeWindow     = false;
    bool tradingHalt    = false;
    bool coolingPeriod  = false;
    bool rapidFire      = false;
    bool monthlyLimit   = false;
    bool entriesAllowed = false;

    bool pass() const {
        return connectivity && latency && timeWindow && tradingHalt
            && coolingPeriod && rapidFire && monthlyLimit && entriesAllowed;
    }

    int failCount() const {
        int n = 0;
        if (!connectivity)   n++;
        if (!latency)        n++;
        if (!timeWindow)     n++;
        if (!tradingHalt)    n++;
        if (!coolingPeriod)  n++;
        if (!rapidFire)      n++;
        if (!monthlyLimit)   n++;
        if (!entriesAllowed) n++;
        return n;
    }

    std::string firstFailure() const {
        if (!connectivity)   return "connectivity";
        if (!latency)        return "latency";
        if (!timeWindow)     return "time_window";
        if (!tradingHalt)    return "trading_halt";
        if (!coolingPeriod)  return "cooling_period";
        if (!rapidFire)      return "rapid_fire";
        if (!monthlyLimit)   return "monthly_limit";
        if (!entriesAllowed) return "entries_allowed";
        return "";
    }

    nlohmann::json ToJson() const {
        return {
            {"connectivity",    connectivity},
            {"latency",         latency},
            {"time_window",     timeWindow},
            {"trading_halt",    tradingHalt},
            {"cooling_period",  coolingPeriod},
            {"rapid_fire",      rapidFire},
            {"monthly_limit",   monthlyLimit},
            {"entries_allowed", entriesAllowed},
            {"pass",            pass()}
        };
    }
};

// ─── Layer 2: ConvictionScore (SHOULD we trade?) ────────────────────────────
// Weighted scalar from 8 signal inputs.  Hard vetoes zero the score.

struct ConvictionInputs {
    float modelConfidence     = 0.0f;   // c₁  [0, 1]
    float actionEntropy       = 0.0f;   // raw entropy (converted to c₂ internally)
    float top2Margin          = 0.0f;   // c₃  raw margin
    float thesisStrength      = 0.0f;   // c₄  [0, 1]
    float biasAlignment       = 0.0f;   // c₅  1.0 if aligned, 0.0 if not
    float impulseAlignment    = 0.0f;   // c₆  1.0 if aligned, 0.0 if not
    float patternQuality      = 0.0f;   // c₇  [0, 1]
    float predictionFreshness = 0.0f;   // c₈  [0, 1] from FreshnessDiscount
    bool  isPhysicsVetoRandomWalk = false;  // hard veto
};

struct ConvictionResult {
    float score           = 0.0f;   // final conviction [0, 1]
    bool  hardVetoActive  = false;
    std::string hardVetoReason;

    // Component breakdown for logging
    struct Components {
        float c1_confidence = 0.0f;
        float c2_entropy    = 0.0f;
        float c3_top2margin = 0.0f;
        float c4_thesis     = 0.0f;
        float c5_bias       = 0.0f;
        float c6_impulse    = 0.0f;
        float c7_pattern    = 0.0f;
        float c8_freshness  = 0.0f;
    } components;

    nlohmann::json ToJson() const {
        return {
            {"conviction",       score},
            {"hard_veto_active", hardVetoActive},
            {"hard_veto_reason", hardVetoReason},
            {"components", {
                {"c1_confidence", components.c1_confidence},
                {"c2_entropy",    components.c2_entropy},
                {"c3_top2margin", components.c3_top2margin},
                {"c4_thesis",     components.c4_thesis},
                {"c5_bias",       components.c5_bias},
                {"c6_impulse",    components.c6_impulse},
                {"c7_pattern",    components.c7_pattern},
                {"c8_freshness",  components.c8_freshness}
            }}
        };
    }
};

inline ConvictionResult ComputeConviction(const ConvictionInputs& in) {
    ConvictionResult r;

    // Hard vetoes (PAER §9.4): these zero conviction regardless of weights
    if (in.actionEntropy > 2.0f) {
        r.hardVetoActive = true;
        r.hardVetoReason = "entropy>" + std::to_string(in.actionEntropy);
        r.score = 0.0f;
        return r;
    }
    if (in.predictionFreshness <= 0.0f) {
        r.hardVetoActive = true;
        r.hardVetoReason = "stale_prediction";
        r.score = 0.0f;
        return r;
    }
    if (in.isPhysicsVetoRandomWalk) {
        r.hardVetoActive = true;
        r.hardVetoReason = "physics_veto_random_walk";
        r.score = 0.0f;
        return r;
    }

    // Normalize inputs to [0, 1]
    static constexpr float kLnOf9 = 2.1972f;  // ln(9)

    r.components.c1_confidence = std::clamp(in.modelConfidence, 0.0f, 1.0f);
    r.components.c2_entropy    = std::clamp(1.0f - (in.actionEntropy / kLnOf9), 0.0f, 1.0f);
    r.components.c3_top2margin = std::clamp(in.top2Margin / 0.50f, 0.0f, 1.0f);
    r.components.c4_thesis     = std::clamp(in.thesisStrength, 0.0f, 1.0f);
    r.components.c5_bias       = std::clamp(in.biasAlignment, 0.0f, 1.0f);
    r.components.c6_impulse    = std::clamp(in.impulseAlignment, 0.0f, 1.0f);
    r.components.c7_pattern    = std::clamp(in.patternQuality, 0.0f, 1.0f);
    r.components.c8_freshness  = std::clamp(in.predictionFreshness, 0.0f, 1.0f);

    // PAER §9.4 weights (tunable in Phase 2 from P&L correlation)
    static constexpr float w1 = 0.25f;  // modelConfidence
    static constexpr float w2 = 0.20f;  // entropy (inverted)
    static constexpr float w3 = 0.15f;  // top2Margin
    static constexpr float w4 = 0.15f;  // thesisStrength
    static constexpr float w5 = 0.10f;  // biasAlignment
    static constexpr float w6 = 0.05f;  // impulseAlignment
    static constexpr float w7 = 0.05f;  // patternQuality
    static constexpr float w8 = 0.05f;  // predictionFreshness

    r.score = w1 * r.components.c1_confidence
            + w2 * r.components.c2_entropy
            + w3 * r.components.c3_top2margin
            + w4 * r.components.c4_thesis
            + w5 * r.components.c5_bias
            + w6 * r.components.c6_impulse
            + w7 * r.components.c7_pattern
            + w8 * r.components.c8_freshness;

    return r;
}

// ─── Layer 3: RiskPrice (HOW MUCH?) ─────────────────────────────────────────
// Dollar-denominated risk per contract.  Replaces multiplicative shrinkage
// with an additive premium model.

struct RiskPriceInputs {
    // Physical base
    double stopDistanceTicks  = 0.0;
    double currencyPerTick    = 0.0;

    // Premium drivers
    float  atrRatio           = 1.0f;   // current_ATR / historical_ATR
    float  vpin               = 0.0f;
    float  spreadStress       = 0.0f;
    float  shannonFlowEntropy = 0.0f;
    float  talebKurtosis      = 0.0f;
    float  paretoTailAlpha    = 4.0f;
    float  mahalanobis        = 0.0f;
    float  hmmTransitionRisk  = 0.0f;
    float  hmmEntropy         = 0.0f;
    float  drawdownPct        = 0.0f;   // |current equity - high_water| / high_water

    // v5.7: Directional order-flow toxicity (Taleb-consistent volume decomposition)
    float  volumeImbalance    = 0.0f;   // (askVol-bidVol)/totalVol, [-1,+1]
    float  volumeZScore       = 0.0f;   // robust log-vol z-score (magnitude)
    bool   isLong             = true;   // entry direction (for directional premium)
};

struct RiskPriceResult {
    double baseRiskPerContract    = 0.0;
    double volatilityPremium      = 1.0;
    double microstructurePremium  = 1.0;
    double regimeUncertaintyPremium = 1.0;
    double tailRiskPremium        = 1.0;
    double directionalToxicityPremium = 1.0;
    double drawdownSurcharge      = 1.0;
    double riskPricePerContract   = 0.0;   // product of all

    nlohmann::json ToJson() const {
        return {
            {"base_risk_per_contract",     baseRiskPerContract},
            {"volatility_premium",         volatilityPremium},
            {"microstructure_premium",     microstructurePremium},
            {"regime_uncertainty_premium", regimeUncertaintyPremium},
            {"tail_risk_premium",          tailRiskPremium},
            {"directional_toxicity_premium", directionalToxicityPremium},
            {"drawdown_surcharge",         drawdownSurcharge},
            {"risk_price_per_contract",    riskPricePerContract}
        };
    }
};

inline RiskPriceResult ComputeRiskPrice(const RiskPriceInputs& in) {
    RiskPriceResult r;

    // Base: physical risk per contract
    r.baseRiskPerContract = in.stopDistanceTicks * in.currencyPerTick;
    if (r.baseRiskPerContract <= 0.0) {
        r.riskPricePerContract = 0.0;
        return r;
    }

    // 1. Volatility regime premium: higher ATR → higher cost
    //    ratio 1.0 → 1.0×; ratio 2.0 → 1.5×; ratio 3.0 → 2.0×
    r.volatilityPremium = std::max(1.0, 1.0 + 0.5 * (static_cast<double>(in.atrRatio) - 1.0));

    // 2. Microstructure premium: max of VPIN, spread stress, entropy signals
    //    Each normalized to [0,1] → premium [1.0, 2.0]
    {
        const double vpinStress   = std::clamp(static_cast<double>(in.vpin) / 0.80, 0.0, 1.0);
        const double spreadFactor = std::clamp(static_cast<double>(in.spreadStress) / 0.85, 0.0, 1.0);
        const double entropyFactor = std::clamp(static_cast<double>(in.shannonFlowEntropy) / 0.90, 0.0, 1.0);
        const double worstMicro = std::max({vpinStress, spreadFactor, entropyFactor});
        r.microstructurePremium = 1.0 + worstMicro;
    }

    // 3. Regime uncertainty premium: transition risk + HMM entropy
    //    Low transition risk (< 0.15) → 1.0×; high (> 0.50) → 1.5×
    {
        const double transRisk = std::clamp(static_cast<double>(in.hmmTransitionRisk) / 0.50, 0.0, 1.0);
        r.regimeUncertaintyPremium = 1.0 + 0.5 * transRisk;
    }

    // 4. Tail risk premium: composite of Hill α, excess kurtosis, Mahalanobis
    //    Fat tails (low α, high kurtosis, high Mahalanobis) → expensive.
    //    Calibrated to fat-tailed futures (Student-t DOF≈5), not Gaussian.
    {
        // Hill α (Pareto tail index): α ≥ 4 → 0.0; α = 1.5 (near-Cauchy) → 1.0
        double hillPenalty = 0.0;
        if (in.paretoTailAlpha > 0.0f && in.paretoTailAlpha < 4.0f) {
            hillPenalty = std::clamp((4.0 - static_cast<double>(in.paretoTailAlpha)) / 2.5, 0.0, 1.0);
        }
        // Excess kurtosis (0 = Gaussian, 6 = Student-t DOF≈5).
        // Ambient ES/NQ excess kurtosis ≈ 3–6 during normal fat-tailed trading.
        // Zero penalty at 6.0 (ambient baseline); full penalty at 15.0 (crisis).
        // Layered with ExecutionParams crisis gate (κ > 5 restricts trading);
        // this premium adds continuous cost for trades that survive the gate.
        const double kurtosisPenalty = std::clamp(
            (static_cast<double>(in.talebKurtosis) - 6.0) / 9.0, 0.0, 1.0);
        // Robust Mahalanobis (16D observation vector, median/MAD estimator).
        // E[d] ≈ √p = √16 = 4.0 for typical observations.
        // Consistent with MahalanobisSizingCap() which also anchors at 4.0;
        // wire_mahal > 6.0 danger threshold sits at midpoint (penalty ≈ 0.5).
        const double mahalPenalty = std::clamp(
            (static_cast<double>(in.mahalanobis) - 4.0) / 4.0, 0.0, 1.0);

        const double worstTail = std::max({hillPenalty, kurtosisPenalty, mahalPenalty});
        r.tailRiskPremium = 1.0 + worstTail;
    }

    // 5. Drawdown surcharge: deeper drawdown → costlier risk
    //    0% DD → 1.0×; 10% DD → 1.5×; 25% DD → 2.5×
    r.drawdownSurcharge = 1.0 + 6.0 * static_cast<double>(in.drawdownPct);

    // 6. Directional toxicity premium: entering against institutional flow at elevated volume
    //    Fires only when volume is elevated (|z| > 1.0) AND flow is against entry direction.
    //    directionalToxicity = max(0, -sign(direction) × imbalance) × clamp(|volZ|/2, 0, 1)
    //    Premium range: 1.0 (no toxicity) → 1.5 (max adverse flow at extreme volume)
    {
        const float dirSign = in.isLong ? 1.0f : -1.0f;
        const float adverseFlow = std::max(0.0f, -dirSign * in.volumeImbalance);
        const float volIntensity = std::clamp(std::fabs(in.volumeZScore) / 2.0f, 0.0f, 1.0f);
        const double toxicity = static_cast<double>(adverseFlow) * static_cast<double>(volIntensity);
        r.directionalToxicityPremium = 1.0 + 0.5 * toxicity;
    }

    // Final: multiply all premiums
    r.riskPricePerContract = r.baseRiskPerContract
                           * r.volatilityPremium
                           * r.microstructurePremium
                           * r.regimeUncertaintyPremium
                           * r.tailRiskPremium
                           * r.directionalToxicityPremium
                           * r.drawdownSurcharge;

    return r;
}

// ─── Layer 4: TradeDecision (synthesis) ─────────────────────────────────────

struct TradeDecision {
    // Layer outputs
    TradingClearance clearance;
    ConvictionResult conviction;
    RiskPriceResult  riskPrice;

    // Sizing
    double dailyRiskBudget = 0.0;
    int    rawSize         = 0;
    int    finalSize       = 0;

    // Composite shadow decision
    bool        shadowExecute = false;
    std::string denyLayer;
    std::string denyDetail;

    void Synthesize(float convictionFloor, int positionCap) {
        if (!clearance.pass()) {
            shadowExecute = false;
            denyLayer = "CLEARANCE";
            denyDetail = clearance.firstFailure();
            return;
        }
        if (conviction.hardVetoActive || conviction.score < convictionFloor) {
            shadowExecute = false;
            denyLayer = "CONVICTION";
            if (conviction.hardVetoActive) {
                denyDetail = "hard_veto:" + conviction.hardVetoReason;
            } else {
                denyDetail = "score=" + std::to_string(conviction.score) +
                             "<floor=" + std::to_string(convictionFloor);
            }
            return;
        }

        // Size from risk price
        if (riskPrice.riskPricePerContract > 0.0 && dailyRiskBudget > 0.0) {
            rawSize = static_cast<int>(
                dailyRiskBudget / riskPrice.riskPricePerContract
                * static_cast<double>(conviction.score));
        }
        finalSize = std::min(rawSize, positionCap);

        if (finalSize < 1) {
            shadowExecute = false;
            denyLayer = "SIZING";
            denyDetail = "risk_price=$" + std::to_string(riskPrice.riskPricePerContract) +
                         ">budget=$" + std::to_string(dailyRiskBudget);
            return;
        }

        shadowExecute = true;
    }

    nlohmann::json ToJson() const {
        return {
            {"shadow_execute", shadowExecute},
            {"deny_layer",     denyLayer},
            {"deny_detail",    denyDetail},
            {"daily_risk_budget", dailyRiskBudget},
            {"raw_size",       rawSize},
            {"final_size",     finalSize}
        };
    }
};

// ─── Layer 5: OpportunityLedger ─────────────────────────────────────────────
// Records EVERY opportunity (executed or not) with full TDE diagnostics.
// Extends the RejectionLedger pattern to capture the denominator.

struct OpportunityRecord {
    // Identity (shared with RejectionRecord)
    uint64_t    timestampUs     = 0;
    std::string timestampHuman;
    std::string paramSetId;
    std::string sessionDate;
    std::string symbol;

    // Trade proposal
    std::string action;
    std::string patternEnum;
    float       modelConfidence = 0.0f;
    double      entryPrice      = 0.0;
    double      stopPrice       = 0.0;
    int         proposedQuantity = 0;

    // TDE layers
    TradingClearance clearance;
    ConvictionResult  conviction;
    RiskPriceResult   riskPrice;
    TradeDecision     decision;

    // Legacy chain result (what actually happened)
    bool        legacyExecuted  = false;
    std::string legacyDenyGate;
    int         legacyDenyCode  = 0;
    int         legacyFinalSize = 0;

    // Market snapshot (reuse from RejectionLedger)
    RejectionRecord::ContextSnapshot context;

    nlohmann::json ToJson() const {
        nlohmann::json j;

        // Identity
        j["timestamp_us"]      = timestampUs;
        j["timestamp_human"]   = timestampHuman;
        j["param_set_id"]      = paramSetId;
        j["session_date"]      = sessionDate;
        j["symbol"]            = symbol;

        // Trade proposal
        j["action"]            = action;
        j["pattern_enum"]      = patternEnum;
        j["model_confidence"]  = modelConfidence;
        j["entry_price"]       = entryPrice;
        j["stop_price"]        = stopPrice;
        j["proposed_quantity"]  = proposedQuantity;

        // TDE layers
        j["layer1_clearance"]  = clearance.ToJson();
        j["layer2_conviction"] = conviction.ToJson();
        j["layer3_risk_price"] = riskPrice.ToJson();
        j["layer4_decision"]   = decision.ToJson();

        // Legacy comparison
        j["legacy"] = {
            {"executed",    legacyExecuted},
            {"deny_gate",   legacyDenyGate},
            {"deny_code",   legacyDenyCode},
            {"final_size",  legacyFinalSize}
        };

        // Market snapshot
        j["context"] = {
            {"vpin",                 context.vpin},
            {"spread_stress",        context.spreadStress},
            {"shannon_flow_entropy", context.shannonFlowEntropy},
            {"shannon_efficiency",   context.shannonEfficiency},
            {"taleb_kurtosis",       context.talebKurtosis},
            {"taleb_skewness",       context.talebSkewness},
            {"elder_chandelier_atr", context.elderChandelierATR},
            {"pareto_tail_alpha",    context.paretoTailAlpha},
            {"hurst_exponent",       context.hurstExponent},
            {"raschke_burst",        context.raschkeBurst},
            {"fisher_info",          context.fisherInfo},
            {"regime_duration",      context.regimeDuration},
            {"daily_pnl",            context.dailyPnL},
            {"account_equity",       context.accountEquity},
            {"trades_today",         context.tradesToday},
            {"consecutive_losses",   context.consecutiveLosses},
            {"current_time_minutes", context.currentTimeMinutes}
        };

        return j;
    }
};

class OpportunityLedger {
public:
    static OpportunityLedger& Instance() {
        static OpportunityLedger instance;
        return instance;
    }

    void Init(const std::string& path = kDefaultPath) {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_initialized) return;

        m_filePath = path;
        m_stream.open(path, std::ios::app);

        if (m_stream.is_open()) {
            m_initialized = true;
            Logger::getInstance().log("OpportunityLedger: Initialized at " + path);
        } else {
            Logger::getInstance().log("OpportunityLedger: FAILED to open " + path);
        }
    }

    void Record(const OpportunityRecord& record) {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_recordCount;

        if (!m_stream.is_open()) return;

        try {
            m_stream << record.ToJson().dump() << '\n';
            m_stream.flush();
        } catch (const std::exception& e) {
            Logger::getInstance().log(
                "OpportunityLedger: Write failed: " + std::string(e.what()));
        }
    }

    uint64_t GetRecordCount() const { return m_recordCount; }
    bool IsActive() const { return m_initialized && m_stream.is_open(); }

private:
    OpportunityLedger() = default;
    ~OpportunityLedger() {
        if (m_stream.is_open()) {
            m_stream.flush();
            m_stream.close();
        }
    }
    OpportunityLedger(const OpportunityLedger&) = delete;
    OpportunityLedger& operator=(const OpportunityLedger&) = delete;

    static constexpr const char* kDefaultPath = "C:/Trading/logs/opportunity_ledger.jsonl";

    std::ofstream m_stream;
    std::mutex m_mutex;
    std::string m_filePath;
    bool m_initialized = false;
    uint64_t m_recordCount = 0;
};
