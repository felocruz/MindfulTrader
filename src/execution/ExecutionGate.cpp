#include "MindfulTrader_Precompiled.h"
#include "execution/ExecutionGate.h"

#include <cassert>

namespace {
ExecutionGate::GateDecision AllowDecision() {
    return ExecutionGate::GateDecision{ExecutionGate::Outcome::Allow, ExecutionGate::ReasonCode::Allow};
}

ExecutionGate::GateDecision IgnoreDecision() {
    return ExecutionGate::GateDecision{ExecutionGate::Outcome::Ignore, ExecutionGate::ReasonCode::Allow};
}

ExecutionGate::GateDecision DenyDecision(ExecutionGate::ReasonCode reason) {
    return ExecutionGate::GateDecision{ExecutionGate::Outcome::Deny, reason};
}

ExecutionGate::GateDecision EvaluateEmpiricalRegimeGates(const ExecutionGate::GateContext& ctx) {
    if (!ctx.hmmMetricsValid) {
        return AllowDecision();
    }

    if (ctx.paretoTopStateRatioMax > 0.0f &&
        ctx.paretoTopStateRatio > ctx.paretoTopStateRatioMax) {
        return DenyDecision(ExecutionGate::ReasonCode::HmmRegimeGateParetoBreach);
    }

    if (ctx.shannonMinTenureBars > 0.0f &&
        ctx.shannonTenureBars < ctx.shannonMinTenureBars) {
        return DenyDecision(ExecutionGate::ReasonCode::HmmRegimeGateShannonBreach);
    }

    if (ctx.talebSignalSigmaThreshold > 0.0f &&
        ctx.talebSignalSigma > ctx.talebSignalSigmaThreshold) {
        return DenyDecision(ExecutionGate::ReasonCode::HmmRegimeGateTalebBreach);
    }

    return AllowDecision();
}

ExecutionGate::GateDecision EvaluateAutomatic(const ExecutionGate::GateContext& ctx) {
    if (ctx.connectivityFailClosed) {
        return DenyDecision(ctx.connectivityReason);
    }
    if (!ctx.modelReady) {
        return DenyDecision(ExecutionGate::ReasonCode::ModelNotReady);
    }
    if (!ctx.predictionFresh) {
        return DenyDecision(ExecutionGate::ReasonCode::PredictionStale);
    }
    if (!ctx.contextFresh) {
        return DenyDecision(ExecutionGate::ReasonCode::ContextStale);
    }
    if (!ctx.allowNewEntries) {
        return DenyDecision(ExecutionGate::ReasonCode::EntryDisabledGlobal);
    }
    if (!ctx.hardGatesPass) {
        return DenyDecision(ExecutionGate::ReasonCode::HardSafetyViolation);
    }
    if (!ctx.isFlat) {
        // Existing behavior parity: automatic entry silently no-ops when already in position.
        return IgnoreDecision();
    }
    if (ctx.hasPendingEntryOrder) {
        return DenyDecision(ExecutionGate::ReasonCode::IntentSuperseded);
    }
    if (ctx.tradingHalted) {
        return DenyDecision(ExecutionGate::ReasonCode::RiskDailyLimit);
    }
    const auto regimeDecision = EvaluateEmpiricalRegimeGates(ctx);
    if (regimeDecision.outcome == ExecutionGate::Outcome::Deny) {
        return regimeDecision;
    }
    return AllowDecision();
}

ExecutionGate::GateDecision EvaluateManual(const ExecutionGate::GateContext& ctx) {
    // NOTE (intentional): manual/discretionary entries deliberately do NOT gate on
    // modelReady / predictionFresh (unlike EvaluateAutomatic). A human-in-the-loop
    // override must remain available precisely when the ML model is down or stale;
    // requiring model availability would defeat the failsafe. Every hard-safety and
    // risk gate below (connectivity, context freshness, hard gates, flat, pending,
    // halt, empirical regime) still applies unchanged.
    if (ctx.connectivityFailClosed) {
        return DenyDecision(ctx.connectivityReason);
    }
    if (!ctx.allowNewEntries) {
        return DenyDecision(ExecutionGate::ReasonCode::EntryDisabledGlobal);
    }
    if (!ctx.contextFresh) {
        return DenyDecision(ExecutionGate::ReasonCode::ContextStale);
    }
    if (!ctx.hardGatesPass) {
        return DenyDecision(ExecutionGate::ReasonCode::HardSafetyViolation);
    }
    if (!ctx.isFlat) {
        return DenyDecision(ExecutionGate::ReasonCode::PolicyEvalErrorFailClosed);
    }
    if (ctx.hasPendingEntryOrder) {
        return DenyDecision(ExecutionGate::ReasonCode::IntentSuperseded);
    }
    if (ctx.tradingHalted) {
        return DenyDecision(ExecutionGate::ReasonCode::RiskDailyLimit);
    }
    const auto regimeDecision = EvaluateEmpiricalRegimeGates(ctx);
    if (regimeDecision.outcome == ExecutionGate::Outcome::Deny) {
        return regimeDecision;
    }
    return AllowDecision();
}

#ifndef NDEBUG
void RunExecutionGateSelfCheck() {
    ExecutionGate::GateContext ctx;
    ctx.isEntryAction = true;
    ctx.source = ExecutionGate::EntrySource::Automatic;

    // Connectivity is highest precedence.
    ctx.connectivityFailClosed = true;
    ctx.modelReady = false;
    auto decision = ExecutionGate::EvaluateEntry(ctx);
    assert(decision.outcome == ExecutionGate::Outcome::Deny);
    assert(decision.reason == ExecutionGate::ReasonCode::ScDisconnected);

    // Automatic non-flat is ignore, not deny.
    ctx = {};
    ctx.isEntryAction = true;
    ctx.source = ExecutionGate::EntrySource::Automatic;
    ctx.isFlat = false;
    decision = ExecutionGate::EvaluateEntry(ctx);
    assert(decision.outcome == ExecutionGate::Outcome::Ignore);

    // Manual non-flat is deterministic deny.
    ctx = {};
    ctx.isEntryAction = true;
    ctx.source = ExecutionGate::EntrySource::Manual;
    ctx.isFlat = false;
    decision = ExecutionGate::EvaluateEntry(ctx);
    assert(decision.outcome == ExecutionGate::Outcome::Deny);
    assert(decision.reason == ExecutionGate::ReasonCode::PolicyEvalErrorFailClosed);

    // Trading halted denies both sources once preconditions pass.
    ctx = {};
    ctx.isEntryAction = true;
    ctx.source = ExecutionGate::EntrySource::Automatic;
    ctx.tradingHalted = true;
    decision = ExecutionGate::EvaluateEntry(ctx);
    assert(decision.outcome == ExecutionGate::Outcome::Deny);
    assert(decision.reason == ExecutionGate::ReasonCode::RiskDailyLimit);
}
#endif
}

ExecutionGate::GateDecision ExecutionGate::EvaluateEntry(const GateContext& ctx) {
#ifndef NDEBUG
    static const bool kExecutionGateValidated = []() {
        RunExecutionGateSelfCheck();
        return true;
    }();
    (void)kExecutionGateValidated;
#endif

    if (!ctx.isEntryAction) {
        return AllowDecision();
    }

    return (ctx.source == EntrySource::Manual)
        ? EvaluateManual(ctx)
        : EvaluateAutomatic(ctx);
}
