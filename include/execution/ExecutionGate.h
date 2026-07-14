#pragma once

#include <cstdint>

class ExecutionGate {
public:
    enum class Outcome : uint8_t {
        Allow = 0,
        Deny = 1,
        Ignore = 2,
    };

    enum class EntrySource : uint8_t {
        Automatic = 0,
        Manual = 1,
    };

    enum class ReasonCode : uint8_t {
        Allow = 0,
        ContextStale = 1,
        EntryDisabledGlobal = 2,
        HardSafetyViolation = 3,
        IbDisconnected = 4,
        IntentSuperseded = 9,
        ModelNotReady = 10,
        OrderAckTimeout = 11,
        PolicyEvalErrorFailClosed = 15,
        PredictionStale = 16,
        RiskDailyLimit = 18,
        ScDisconnected = 19,
        HmmRegimeGateParetoBreach = 30,
        HmmRegimeGateShannonBreach = 31,
        HmmRegimeGateTalebBreach = 32,
    };

    struct GateContext {
        EntrySource source{EntrySource::Automatic};
        bool isEntryAction{false};

        bool connectivityFailClosed{false};
        ReasonCode connectivityReason{ReasonCode::ScDisconnected};

        bool modelReady{true};
        bool predictionFresh{true};
        bool contextFresh{true};
        bool allowNewEntries{true};
        bool hardGatesPass{true};

        bool isFlat{true};
        bool hasPendingEntryOrder{false};
        bool tradingHalted{false};

        // Empirical HMM regime gate inputs (wired from LocalRiskContext + RiskPolicy).
        bool hmmMetricsValid{false};
        float paretoTopStateRatio{0.0f};
        float shannonTenureBars{0.0f};
        float talebSignalSigma{0.0f};

        float paretoTopStateRatioMax{0.0f};
        float shannonMinTenureBars{0.0f};
        float talebSignalSigmaThreshold{0.0f};
    };

    struct GateDecision {
        Outcome outcome{Outcome::Allow};
        ReasonCode reason{ReasonCode::Allow};
    };

    static GateDecision EvaluateEntry(const GateContext& ctx);
};
