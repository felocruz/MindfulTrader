//! Rust port of `include/execution/ExecutionGate.h` / `src/execution/ExecutionGate.cpp`.
//! Literal behavior-preserving port (spec §8.7 D4) -- same decision tree, same enum values, same
//! precedence order as the C++ original. Field names are snake_case (Rust convention); nothing
//! else about the logic changed. The C++ original's `RunExecutionGateSelfCheck()` (a runtime,
//! first-call-only debug assert) is reproduced here as ordinary `#[test]` functions instead --
//! that's test-harness mechanics, not business-logic behavior, so it's fair game for the
//! 2026-10-10 "Rust best-practice freedom for mechanics" ruling (spec §8.1) even though the gate
//! decisions themselves are not.

/// Mirrors `ExecutionGate::Outcome`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Outcome {
    Allow,
    Deny,
    Ignore,
}

/// Mirrors `ExecutionGate::EntrySource`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EntrySource {
    Automatic,
    Manual,
}

/// Mirrors `ExecutionGate::ReasonCode`. This is `ExecutionGate.h`'s own local enum (a narrower,
/// independently-named subset of the schema's `ReasonCode`, not the same type) -- ported as its
/// own distinct enum here too, not unified with `schema`'s `ReasonCode`. Unifying the two is a
/// separate, future decision, not part of this literal port.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ReasonCode {
    Allow,
    ContextStale,
    EntryDisabledGlobal,
    HardSafetyViolation,
    IbDisconnected,
    IntentSuperseded,
    ModelNotReady,
    OrderAckTimeout,
    PolicyEvalErrorFailClosed,
    PredictionStale,
    RiskDailyLimit,
    ScDisconnected,
    HmmRegimeGateParetoBreach,
    HmmRegimeGateShannonBreach,
    HmmRegimeGateTalebBreach,
}

/// Mirrors `ExecutionGate::GateContext`. Field defaults match the C++ struct's in-class
/// initializers; use `GateContext::default()` where C++ would default-construct.
#[derive(Debug, Clone, Copy)]
pub struct GateContext {
    pub source: EntrySource,
    pub is_entry_action: bool,

    pub connectivity_fail_closed: bool,
    pub connectivity_reason: ReasonCode,

    pub model_ready: bool,
    pub prediction_fresh: bool,
    pub context_fresh: bool,
    pub allow_new_entries: bool,
    pub hard_gates_pass: bool,

    pub is_flat: bool,
    pub has_pending_entry_order: bool,
    pub trading_halted: bool,

    // Empirical HMM regime gate inputs (wired from LocalRiskContext + RiskPolicy).
    pub hmm_metrics_valid: bool,
    // Hill (1975) tail-index proxy (1/alpha), NOT an HMM state-occupancy probability despite the
    // C++ field's pre-2026-09-04 name (paretoTopStateRatio) -- see
    // docs/superpowers/specs/2026-09-18-predator-sniper-execution-architecture.md §3a.
    pub hill_tail_index_proxy: f32,
    pub shannon_tenure_bars: f32,
    // Raw Moors octile kurtosis (same feature as RiskManager's gate 4), NOT a z-score/sigma
    // reading despite the C++ field's pre-2026-09-04 name (talebSignalSigma) -- see the same doc.
    pub taleb_kurtosis_entry_gate: f32,

    pub hill_tail_index_proxy_max: f32,
    pub shannon_min_tenure_bars: f32,
    pub taleb_kurtosis_entry_gate_threshold: f32,
}

impl Default for GateContext {
    fn default() -> Self {
        Self {
            source: EntrySource::Automatic,
            is_entry_action: false,
            connectivity_fail_closed: false,
            connectivity_reason: ReasonCode::ScDisconnected,
            model_ready: true,
            prediction_fresh: true,
            context_fresh: true,
            allow_new_entries: true,
            hard_gates_pass: true,
            is_flat: true,
            has_pending_entry_order: false,
            trading_halted: false,
            hmm_metrics_valid: false,
            hill_tail_index_proxy: 0.0,
            shannon_tenure_bars: 0.0,
            taleb_kurtosis_entry_gate: 0.0,
            hill_tail_index_proxy_max: 0.0,
            shannon_min_tenure_bars: 0.0,
            taleb_kurtosis_entry_gate_threshold: 0.0,
        }
    }
}

/// Mirrors `ExecutionGate::GateDecision`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct GateDecision {
    pub outcome: Outcome,
    pub reason: ReasonCode,
}

fn allow_decision() -> GateDecision {
    GateDecision { outcome: Outcome::Allow, reason: ReasonCode::Allow }
}

fn ignore_decision() -> GateDecision {
    GateDecision { outcome: Outcome::Ignore, reason: ReasonCode::Allow }
}

fn deny_decision(reason: ReasonCode) -> GateDecision {
    GateDecision { outcome: Outcome::Deny, reason }
}

fn evaluate_empirical_regime_gates(ctx: &GateContext) -> GateDecision {
    if !ctx.hmm_metrics_valid {
        return allow_decision();
    }

    if ctx.hill_tail_index_proxy_max > 0.0 && ctx.hill_tail_index_proxy > ctx.hill_tail_index_proxy_max {
        return deny_decision(ReasonCode::HmmRegimeGateParetoBreach);
    }

    if ctx.shannon_min_tenure_bars > 0.0 && ctx.shannon_tenure_bars < ctx.shannon_min_tenure_bars {
        return deny_decision(ReasonCode::HmmRegimeGateShannonBreach);
    }

    // Not a duplicate of RiskManager's gate-4 kurtosis hard gate (same raw ctx.talebKurtosis,
    // different threshold/scope, verdict 2026-09-04, trade-execution-risk-management-curation-
    // initiative.md §3a item 2): this only blocks NEW entry admission, independently calibrated
    // (lbrnet empirical HMM-regime study) from RiskManager's broader, continuously-enforced halt.
    if ctx.taleb_kurtosis_entry_gate_threshold > 0.0
        && ctx.taleb_kurtosis_entry_gate > ctx.taleb_kurtosis_entry_gate_threshold
    {
        return deny_decision(ReasonCode::HmmRegimeGateTalebBreach);
    }

    allow_decision()
}

fn evaluate_automatic(ctx: &GateContext) -> GateDecision {
    if ctx.connectivity_fail_closed {
        return deny_decision(ctx.connectivity_reason);
    }
    if !ctx.model_ready {
        return deny_decision(ReasonCode::ModelNotReady);
    }
    if !ctx.prediction_fresh {
        return deny_decision(ReasonCode::PredictionStale);
    }
    if !ctx.context_fresh {
        return deny_decision(ReasonCode::ContextStale);
    }
    if !ctx.allow_new_entries {
        return deny_decision(ReasonCode::EntryDisabledGlobal);
    }
    if !ctx.hard_gates_pass {
        return deny_decision(ReasonCode::HardSafetyViolation);
    }
    if !ctx.is_flat {
        // Existing behavior parity: automatic entry silently no-ops when already in position.
        return ignore_decision();
    }
    if ctx.has_pending_entry_order {
        return deny_decision(ReasonCode::IntentSuperseded);
    }
    if ctx.trading_halted {
        return deny_decision(ReasonCode::RiskDailyLimit);
    }
    let regime_decision = evaluate_empirical_regime_gates(ctx);
    if regime_decision.outcome == Outcome::Deny {
        return regime_decision;
    }
    allow_decision()
}

fn evaluate_manual(ctx: &GateContext) -> GateDecision {
    // NOTE (intentional): manual/discretionary entries deliberately do NOT gate on
    // model_ready / prediction_fresh (unlike evaluate_automatic). A human-in-the-loop override
    // must remain available precisely when the ML model is down or stale; requiring model
    // availability would defeat the failsafe. Every hard-safety and risk gate below
    // (connectivity, context freshness, hard gates, flat, pending, halt, empirical regime) still
    // applies unchanged.
    if ctx.connectivity_fail_closed {
        return deny_decision(ctx.connectivity_reason);
    }
    if !ctx.allow_new_entries {
        return deny_decision(ReasonCode::EntryDisabledGlobal);
    }
    if !ctx.context_fresh {
        return deny_decision(ReasonCode::ContextStale);
    }
    if !ctx.hard_gates_pass {
        return deny_decision(ReasonCode::HardSafetyViolation);
    }
    if !ctx.is_flat {
        return deny_decision(ReasonCode::PolicyEvalErrorFailClosed);
    }
    if ctx.has_pending_entry_order {
        return deny_decision(ReasonCode::IntentSuperseded);
    }
    if ctx.trading_halted {
        return deny_decision(ReasonCode::RiskDailyLimit);
    }
    let regime_decision = evaluate_empirical_regime_gates(ctx);
    if regime_decision.outcome == Outcome::Deny {
        return regime_decision;
    }
    allow_decision()
}

/// Mirrors `ExecutionGate::EvaluateEntry`.
pub fn evaluate_entry(ctx: &GateContext) -> GateDecision {
    if !ctx.is_entry_action {
        return allow_decision();
    }

    match ctx.source {
        EntrySource::Manual => evaluate_manual(ctx),
        EntrySource::Automatic => evaluate_automatic(ctx),
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    // Mirrors ExecutionGate.cpp's RunExecutionGateSelfCheck() cases 1:1.

    #[test]
    fn connectivity_is_highest_precedence() {
        let mut ctx = GateContext { is_entry_action: true, source: EntrySource::Automatic, ..Default::default() };
        ctx.connectivity_fail_closed = true;
        ctx.model_ready = false;
        let decision = evaluate_entry(&ctx);
        assert_eq!(decision.outcome, Outcome::Deny);
        assert_eq!(decision.reason, ReasonCode::ScDisconnected);
    }

    #[test]
    fn automatic_non_flat_is_ignore_not_deny() {
        let ctx = GateContext {
            is_entry_action: true,
            source: EntrySource::Automatic,
            is_flat: false,
            ..Default::default()
        };
        let decision = evaluate_entry(&ctx);
        assert_eq!(decision.outcome, Outcome::Ignore);
    }

    #[test]
    fn manual_non_flat_is_deterministic_deny() {
        let ctx = GateContext {
            is_entry_action: true,
            source: EntrySource::Manual,
            is_flat: false,
            ..Default::default()
        };
        let decision = evaluate_entry(&ctx);
        assert_eq!(decision.outcome, Outcome::Deny);
        assert_eq!(decision.reason, ReasonCode::PolicyEvalErrorFailClosed);
    }

    #[test]
    fn trading_halted_denies_both_sources_once_preconditions_pass() {
        let ctx = GateContext {
            is_entry_action: true,
            source: EntrySource::Automatic,
            trading_halted: true,
            ..Default::default()
        };
        let decision = evaluate_entry(&ctx);
        assert_eq!(decision.outcome, Outcome::Deny);
        assert_eq!(decision.reason, ReasonCode::RiskDailyLimit);
    }

    // Additional coverage beyond the C++ self-check (idiomatic #[test] granularity, not a
    // behavior change -- spec §8.1's mechanics-freedom ruling covers test-harness shape).

    #[test]
    fn non_entry_action_always_allows() {
        let ctx = GateContext { is_entry_action: false, ..Default::default() };
        assert_eq!(evaluate_entry(&ctx).outcome, Outcome::Allow);
    }

    #[test]
    fn manual_ignores_stale_model_and_prediction() {
        let ctx = GateContext {
            is_entry_action: true,
            source: EntrySource::Manual,
            model_ready: false,
            prediction_fresh: false,
            ..Default::default()
        };
        // Manual deliberately does not gate on model_ready/prediction_fresh.
        assert_eq!(evaluate_entry(&ctx).outcome, Outcome::Allow);
    }

    #[test]
    fn automatic_denies_on_stale_model() {
        let ctx = GateContext {
            is_entry_action: true,
            source: EntrySource::Automatic,
            model_ready: false,
            ..Default::default()
        };
        let decision = evaluate_entry(&ctx);
        assert_eq!(decision.outcome, Outcome::Deny);
        assert_eq!(decision.reason, ReasonCode::ModelNotReady);
    }

    #[test]
    fn automatic_denies_on_stale_prediction() {
        let ctx = GateContext {
            is_entry_action: true,
            source: EntrySource::Automatic,
            prediction_fresh: false,
            ..Default::default()
        };
        let decision = evaluate_entry(&ctx);
        assert_eq!(decision.outcome, Outcome::Deny);
        assert_eq!(decision.reason, ReasonCode::PredictionStale);
    }

    #[test]
    fn denies_on_stale_context() {
        let ctx = GateContext { is_entry_action: true, context_fresh: false, ..Default::default() };
        let decision = evaluate_entry(&ctx);
        assert_eq!(decision.outcome, Outcome::Deny);
        assert_eq!(decision.reason, ReasonCode::ContextStale);
    }

    #[test]
    fn denies_when_new_entries_globally_disabled() {
        let ctx = GateContext { is_entry_action: true, allow_new_entries: false, ..Default::default() };
        let decision = evaluate_entry(&ctx);
        assert_eq!(decision.outcome, Outcome::Deny);
        assert_eq!(decision.reason, ReasonCode::EntryDisabledGlobal);
    }

    #[test]
    fn denies_on_hard_safety_violation() {
        let ctx = GateContext { is_entry_action: true, hard_gates_pass: false, ..Default::default() };
        let decision = evaluate_entry(&ctx);
        assert_eq!(decision.outcome, Outcome::Deny);
        assert_eq!(decision.reason, ReasonCode::HardSafetyViolation);
    }

    #[test]
    fn denies_on_pending_entry_order() {
        let ctx = GateContext { is_entry_action: true, has_pending_entry_order: true, ..Default::default() };
        let decision = evaluate_entry(&ctx);
        assert_eq!(decision.outcome, Outcome::Deny);
        assert_eq!(decision.reason, ReasonCode::IntentSuperseded);
    }

    #[test]
    fn empirical_regime_gates_are_inert_until_hmm_metrics_valid() {
        let ctx = GateContext {
            is_entry_action: true,
            hmm_metrics_valid: false,
            hill_tail_index_proxy: 999.0,
            hill_tail_index_proxy_max: 1.0,
            ..Default::default()
        };
        // Breach values present but hmm_metrics_valid is false -- must still allow.
        assert_eq!(evaluate_entry(&ctx).outcome, Outcome::Allow);
    }

    #[test]
    fn denies_on_pareto_tail_index_breach() {
        let ctx = GateContext {
            is_entry_action: true,
            hmm_metrics_valid: true,
            hill_tail_index_proxy: 2.0,
            hill_tail_index_proxy_max: 1.0,
            ..Default::default()
        };
        let decision = evaluate_entry(&ctx);
        assert_eq!(decision.outcome, Outcome::Deny);
        assert_eq!(decision.reason, ReasonCode::HmmRegimeGateParetoBreach);
    }

    #[test]
    fn denies_on_shannon_tenure_breach() {
        let ctx = GateContext {
            is_entry_action: true,
            hmm_metrics_valid: true,
            shannon_tenure_bars: 1.0,
            shannon_min_tenure_bars: 5.0,
            ..Default::default()
        };
        let decision = evaluate_entry(&ctx);
        assert_eq!(decision.outcome, Outcome::Deny);
        assert_eq!(decision.reason, ReasonCode::HmmRegimeGateShannonBreach);
    }

    #[test]
    fn denies_on_taleb_kurtosis_entry_gate_breach() {
        let ctx = GateContext {
            is_entry_action: true,
            hmm_metrics_valid: true,
            taleb_kurtosis_entry_gate: 5.0,
            taleb_kurtosis_entry_gate_threshold: 1.0,
            ..Default::default()
        };
        let decision = evaluate_entry(&ctx);
        assert_eq!(decision.outcome, Outcome::Deny);
        assert_eq!(decision.reason, ReasonCode::HmmRegimeGateTalebBreach);
    }

    #[test]
    fn regime_gate_precedence_pareto_before_shannon_before_taleb() {
        // All three breach simultaneously -- Pareto must win, matching the C++ if-chain order.
        let ctx = GateContext {
            is_entry_action: true,
            hmm_metrics_valid: true,
            hill_tail_index_proxy: 2.0,
            hill_tail_index_proxy_max: 1.0,
            shannon_tenure_bars: 1.0,
            shannon_min_tenure_bars: 5.0,
            taleb_kurtosis_entry_gate: 5.0,
            taleb_kurtosis_entry_gate_threshold: 1.0,
            ..Default::default()
        };
        let decision = evaluate_entry(&ctx);
        assert_eq!(decision.reason, ReasonCode::HmmRegimeGateParetoBreach);
    }

    #[test]
    fn default_context_with_entry_action_allows() {
        let ctx = GateContext { is_entry_action: true, ..Default::default() };
        assert_eq!(evaluate_entry(&ctx).outcome, Outcome::Allow);
    }
}
