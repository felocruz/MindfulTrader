//! Working-entry-order decisions (spec §8.4 `order_watcher.rs`, §8.7 D3/D3a): the pure core of the
//! order watcher -- the future async shell (tokio, its own TWS client id) feeds it inputs and acts
//! on its `Decision`; nothing here does I/O or reads a clock.
//!
//! Scope: ORDER management of the unfilled entry only -- should this entry keep working? Where an
//! open position's stop and target sit is POSITION management (strategy: `stop_manager`,
//! `trap_exit`, the triple-barrier doctrine), not this module's concern; amending the orders that
//! implement them goes through `BrokerAdapter::amend`.
//!
//! Port of `PositionManager::ManageWorkingEntryOrder` (`src/PositionManager.cpp`) plus its
//! `IsQuoteChurnBreached` filter -- same cancel checks, same order, first match wins (spec §8.7
//! D4, behavior-preserving). The C++ never amends a working entry because Sierra's limit-chase
//! repriced by cancel + new; with IBKR algos the algo reprices its own child orders, so the
//! watcher only holds or cancels. That is a consequence of the algo choice, not a rule: raising
//! an Adaptive order's urgency instead of cancelling it is an open decision (spec §8.7 D3).
//!
//! Deliberate mechanics changes (spec §8.1 "freedom for mechanics"):
//! - **Two clocks, both supplied by the caller.** Market checks (intent expiry, quote churn) use
//!   market event time; liveness checks (working-order TTL, execution budget) use a steady local
//!   clock, so a stalled feed cannot stop them. The C++ mixed `sc.CurrentSystemDateTime` with a
//!   wall clock. In replay both are the replay clock (spec D2 A2). The shell must call `decide` on
//!   a timer as well as on market events, for the same reason.
//! - **An unreachable broker escalates; it is never dropped.** The C++ released the order
//!   (`ScDisconnected`) and stopped watching -- an orphaned live order. Here it is
//!   `Decision::Escalate`: the order's state is unknown, and the caller must reconcile it
//!   (`position_state`) on reconnect, or flatten.
//! - **A finished order is a fact, not a decision.** `Decision::Finished` only tells the caller to
//!   stop watching; the fill or cancel itself is recorded by `position_state`.
//! - **Budgets are configuration, not constants.** The C++ values (session budgets 800-1200 ms,
//!   regime caps 100-500 ms, ack timeout 1200 ms, working TTL 5000 ms) are `WatcherConfig::
//!   sierra_chase_defaults()`. They were tuned for Sierra's zero-latency limit-chase: the regime
//!   cap always wins, so an entry gets at most ~375-750 ms before an `OrderAckTimeout` cancel --
//!   far too short for an IBKR Adaptive order. Values for IBKR algos are an open operator decision
//!   (spec §8.7 D3), deliberately not chosen here.
//! - **"OrderAckTimeout" keeps its C++ name** although it measures time since submission against
//!   the execution budget, not time to acknowledgement.

use crate::position_state::{OrderStatus, TimestampUs};

/// Why the watcher cancels an order. Names mirror the C++ `ReasonCode` values the original emits.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Reason {
    IntentExpired,
    WorkingOrderTtlExceeded,
    ClimateShiftExit,
    OrderAckTimeout,
    SpreadBreach,
    ToxicFlowBreach,
    QuoteChurnBreach,
    PredictionDirectionBreach,
    PredictionStaleDecay,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Decision {
    /// Keep the order working.
    Hold,
    /// Cancel it at the broker; keep watching until the broker confirms (`Finished`).
    Cancel(Reason),
    /// The broker reports the order filled, cancelled or rejected: stop watching (C++
    /// `IntentCancelled`). `position_state` has already recorded what happened.
    Finished,
    /// The broker cannot be asked about the order, so its state is unknown (C++ `ScDisconnected`,
    /// which stopped watching). Hand it to reconciliation / the kill switch -- never drop it.
    Escalate,
}

/// `::MarketClimate` (the C++ enum `InferenceManager` caps budgets by). Local mirror; unify with
/// `student_t_hmm`'s registry climate when the HMM cutover (plan G7) connects the two crates.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum MarketClimate {
    GaussianStable,
    ParetoMomentum,
    CoiledSpring,
    ShannonChaos,
    TalebianFragile,
}

/// The C++ `TimeOfDayEnum` buckets (`include/IndicatorComputations.h`), as they affect entries.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Session {
    /// PRE_MARKET_HOOK, PRE_MARKET, OPENING_HOUR (08:30-10:30 ET).
    Opening,
    /// FINAL_HOUR, PM_RUN_ENTRY, AFTER_HOURS (15:00-18:00 ET).
    Closing,
    /// Everything else.
    Core,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WatcherConfig {
    pub order_ack_timeout_ms: i64,
    pub working_order_ttl_ms: i64,
    /// Execution budget per session (`GetSessionExecutionBudgetMs`).
    pub opening_budget_ms: i64,
    pub closing_budget_ms: i64,
    pub core_budget_ms: i64,
    /// Max spread in ticks per session (`GetSessionSpreadLimit`).
    pub opening_max_spread_ticks: f64,
    pub closing_max_spread_ticks: f64,
    pub core_max_spread_ticks: f64,
    /// Per-climate budget cap (`GetRegimeExecutionBudgetCap`), indexed by `climate_index`.
    pub climate_budget_cap_ms: [i64; 5],
}

impl WatcherConfig {
    /// The C++ values verbatim, including `RiskManager`'s clamps on the two policy timeouts
    /// (ack 100..=60000 ms, TTL 500..=300000 ms). Tuned for Sierra limit-chase -- see module doc.
    pub fn sierra_chase_defaults() -> Self {
        Self {
            order_ack_timeout_ms: 1200,
            working_order_ttl_ms: 5000,
            opening_budget_ms: 900,
            closing_budget_ms: 800,
            core_budget_ms: 1200,
            opening_max_spread_ticks: 1.5,
            closing_max_spread_ticks: 1.0,
            core_max_spread_ticks: 2.0,
            climate_budget_cap_ms: [500, 350, 400, 150, 100],
        }
    }

    /// Apply `RiskManager::GetOrderAckTimeoutMs`/`GetWorkingOrderTtlMs`'s clamps.
    pub fn clamped(mut self) -> Self {
        self.order_ack_timeout_ms = self.order_ack_timeout_ms.clamp(100, 60_000);
        self.working_order_ttl_ms = self.working_order_ttl_ms.clamp(500, 300_000);
        self
    }

    fn session_budget_ms(&self, s: Session) -> i64 {
        match s {
            Session::Opening => self.opening_budget_ms,
            Session::Closing => self.closing_budget_ms,
            Session::Core => self.core_budget_ms,
        }
    }

    fn max_spread_ticks(&self, s: Session) -> f64 {
        match s {
            Session::Opening => self.opening_max_spread_ticks,
            Session::Closing => self.closing_max_spread_ticks,
            Session::Core => self.core_max_spread_ticks,
        }
    }

    fn climate_cap_ms(&self, c: MarketClimate) -> i64 {
        self.climate_budget_cap_ms[climate_index(c)]
    }
}

fn climate_index(c: MarketClimate) -> usize {
    match c {
        MarketClimate::GaussianStable => 0,
        MarketClimate::ParetoMomentum => 1,
        MarketClimate::CoiledSpring => 2,
        MarketClimate::ShannonChaos => 3,
        MarketClimate::TalebianFragile => 4,
    }
}

/// Everything the decision needs, gathered by the caller at one moment.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct WatchInputs {
    /// Market event time (exchange timestamps live, the replay clock in replay).
    pub market_now: TimestampUs,
    /// A steady local clock (monotonic live, the replay clock in replay).
    pub local_now: TimestampUs,
    /// `None` when the broker could not be asked (the C++ order-lookup fault).
    pub order_status: Option<OrderStatus>,
    pub is_long: bool,
    /// When the order was sent, on the local clock.
    pub submitted_at_local: TimestampUs,
    /// The intent ticket's deadline, if it has one, in market time.
    pub intent_valid_until: Option<TimestampUs>,
    pub high_transition_risk: bool,
    /// HMM Fisher information, when the local risk context is valid.
    pub fisher_info: Option<f32>,
    pub session: Session,
    /// Current climate, when one is available (the C++ cap applies only then).
    pub climate: Option<MarketClimate>,
    /// Fault injection (`RiskManager::ShouldInjectOrderAckTimeout`).
    pub inject_ack_timeout: bool,
    pub bid: f64,
    pub ask: f64,
    pub tick_size: f64,
    /// TS3 volume imbalance from `VolumeIndicator` (negative = selling pressure).
    pub volume_imbalance: f64,
    /// Minutes since midnight for the churn limit (the caller decides the time zone; US Eastern
    /// is what the C++ thresholds were written for).
    pub minute_of_day: u32,
    pub prediction_direction_conflict: bool,
    /// min(semantic, wall-clock) prediction freshness; <= 0 means dead.
    pub prediction_freshness: f64,
}

/// `IsQuoteChurnBreached`'s state: an EMA of quote-change rate. Like the C++ member it lives
/// across orders (it is not reset per order) and is only updated when a decision reaches it.
#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct QuoteChurnFilter {
    prev: Option<(f64, f64, TimestampUs)>,
    ema_per_sec: f64,
}

impl QuoteChurnFilter {
    const ALPHA: f64 = 0.2;

    /// Feed one quote; true when the smoothed change rate exceeds the limit: 6/s from midnight
    /// through 09:45 and from 15:45 on, else 8/s -- the C++ rule exactly (`<= 09:45 || >= 15:45`).
    /// Note the C++ reads `sc.CurrentSystemDateTime`'s hour, i.e. the machine's local time zone,
    /// not necessarily exchange time; here the caller supplies `minute_of_day` explicitly.
    pub fn breached(&mut self, bid: f64, ask: f64, now: TimestampUs, minute_of_day: u32) -> bool {
        let Some((pb, pa, pt)) = self.prev else {
            self.prev = Some((bid, ask, now));
            return false;
        };
        let changed = (bid - pb).abs() > 1e-9 || (ask - pa).abs() > 1e-9;
        let delta_us = if now > pt { now - pt } else { 1 };
        let instantaneous = if changed { (1_000_000.0 / delta_us as f64).min(1000.0) } else { 0.0 };
        self.ema_per_sec = Self::ALPHA * instantaneous + (1.0 - Self::ALPHA) * self.ema_per_sec;
        self.prev = Some((bid, ask, now));
        let limit = if minute_of_day <= 9 * 60 + 45 || minute_of_day >= 15 * 60 + 45 { 6.0 } else { 8.0 };
        self.ema_per_sec > limit
    }

    pub fn rate_per_sec(&self) -> f64 {
        self.ema_per_sec
    }
}

/// Per-order watcher state: when the order was first seen working (local clock).
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct WorkingOrder {
    working_since: Option<TimestampUs>,
}

impl WorkingOrder {
    pub fn new() -> Self {
        Self::default()
    }

    /// One decision. Order of checks = the C++ original's.
    pub fn decide(&mut self, input: &WatchInputs, config: &WatcherConfig, churn: &mut QuoteChurnFilter) -> Decision {
        let Some(status) = input.order_status else {
            return Decision::Escalate;
        };
        if !matches!(status, OrderStatus::Submitted | OrderStatus::Acknowledged | OrderStatus::PartiallyFilled) {
            return Decision::Finished;
        }
        let local_now = input.local_now;
        let working_since = *self.working_since.get_or_insert(local_now);

        if input.intent_valid_until.is_some_and(|until| until > 0 && input.market_now > until) {
            return Decision::Cancel(Reason::IntentExpired);
        }
        if local_now - working_since > config.working_order_ttl_ms * 1000 {
            return Decision::Cancel(Reason::WorkingOrderTtlExceeded);
        }
        if input.high_transition_risk {
            return Decision::Cancel(Reason::ClimateShiftExit);
        }

        // Execution budget: session budget, capped by the ack timeout, Fisher-scaled, then capped
        // by the climate (the C++'s exact composition and order, integer truncation included).
        let fisher_scale: f32 = match input.fisher_info {
            Some(f) if f > 0.60 => 0.75,
            Some(f) if f < 0.20 => 1.50,
            _ => 1.0,
        };
        let mut budget_ms = config.session_budget_ms(input.session).min(config.order_ack_timeout_ms);
        budget_ms = (budget_ms as f32 * fisher_scale) as i64;
        if let Some(c) = input.climate {
            budget_ms = budget_ms.min(config.climate_cap_ms(c));
        }
        if local_now - input.submitted_at_local > budget_ms * 1000 || input.inject_ack_timeout {
            return Decision::Cancel(Reason::OrderAckTimeout);
        }

        if !(input.ask > input.bid && input.tick_size > 0.0) {
            return Decision::Hold; // invalid quote: skip the remaining checks this time
        }
        if (input.ask - input.bid) / input.tick_size > config.max_spread_ticks(input.session) {
            return Decision::Cancel(Reason::SpreadBreach);
        }
        if (input.is_long && input.volume_imbalance < -0.35) || (!input.is_long && input.volume_imbalance > 0.35) {
            return Decision::Cancel(Reason::ToxicFlowBreach);
        }
        if churn.breached(input.bid, input.ask, input.market_now, input.minute_of_day) {
            return Decision::Cancel(Reason::QuoteChurnBreach);
        }
        if input.prediction_direction_conflict {
            return Decision::Cancel(Reason::PredictionDirectionBreach);
        }
        if input.prediction_freshness <= 0.0 {
            return Decision::Cancel(Reason::PredictionStaleDecay);
        }
        Decision::Hold
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MS: TimestampUs = 1_000;
    const T0: TimestampUs = 1_000_000_000;

    fn calm() -> WatchInputs {
        WatchInputs {
            market_now: T0,
            local_now: T0,
            order_status: Some(OrderStatus::Acknowledged),
            is_long: true,
            submitted_at_local: T0,
            intent_valid_until: None,
            high_transition_risk: false,
            fisher_info: Some(0.4),
            session: Session::Core,
            climate: None,
            inject_ack_timeout: false,
            bid: 7858.00,
            ask: 7858.25,
            tick_size: 0.25,
            volume_imbalance: 0.0,
            minute_of_day: 11 * 60,
            prediction_direction_conflict: false,
            prediction_freshness: 1.0,
        }
    }

    fn decide(input: WatchInputs) -> Decision {
        WorkingOrder::new().decide(&input, &WatcherConfig::sierra_chase_defaults(), &mut QuoteChurnFilter::default())
    }

    #[test]
    fn a_calm_fresh_order_is_held() {
        assert_eq!(decide(calm()), Decision::Hold);
    }

    #[test]
    fn finished_orders_stop_and_unknown_orders_escalate() {
        for s in [OrderStatus::Filled, OrderStatus::Cancelled, OrderStatus::Rejected] {
            assert_eq!(decide(WatchInputs { order_status: Some(s), ..calm() }), Decision::Finished);
        }
        // Even an order that would otherwise be cancelled escalates: nothing is known about it.
        let unknown = WatchInputs { order_status: None, inject_ack_timeout: true, ..calm() };
        assert_eq!(decide(unknown), Decision::Escalate);
        assert_eq!(decide(WatchInputs { order_status: Some(OrderStatus::PartiallyFilled), ..calm() }), Decision::Hold);
    }

    #[test]
    fn intent_expiry_and_working_ttl() {
        let expired = WatchInputs { intent_valid_until: Some(T0 - 1), ..calm() };
        assert_eq!(decide(expired), Decision::Cancel(Reason::IntentExpired));
        assert_eq!(decide(WatchInputs { intent_valid_until: Some(0), ..calm() }), Decision::Hold, "0 = no deadline (C++ validUntilUs > 0 guard)");

        // TTL counts from the first time the watcher saw the order working, not from submission.
        let cfg = WatcherConfig { core_budget_ms: 60_000, order_ack_timeout_ms: 60_000, ..WatcherConfig::sierra_chase_defaults() };
        let mut w = WorkingOrder::new();
        let mut churn = QuoteChurnFilter::default();
        assert_eq!(w.decide(&calm(), &cfg, &mut churn), Decision::Hold);
        let at_ttl = WatchInputs { local_now: T0 + 5000 * MS, ..calm() };
        assert_eq!(w.decide(&at_ttl, &cfg, &mut churn), Decision::Hold, "exactly at the TTL is still allowed");
        let past = WatchInputs { local_now: T0 + 5000 * MS + 1, ..calm() };
        assert_eq!(w.decide(&past, &cfg, &mut churn), Decision::Cancel(Reason::WorkingOrderTtlExceeded));
    }

    #[test]
    fn liveness_runs_on_the_local_clock_and_market_checks_on_market_time() {
        // A stalled feed: market time frozen at T0, local clock 10 s on. The budget still fires.
        let stalled = WatchInputs { local_now: T0 + 10_000 * MS, ..calm() };
        assert_eq!(decide(stalled), Decision::Cancel(Reason::OrderAckTimeout));
        // Market time far ahead (e.g. replay fast-forward) does not consume the local budget...
        let ahead = WatchInputs { market_now: T0 + 10_000 * MS, ..calm() };
        assert_eq!(decide(ahead), Decision::Hold);
        // ...but it is what intent expiry is measured on.
        let expired = WatchInputs { market_now: T0 + 2, intent_valid_until: Some(T0 + 1), ..calm() };
        assert_eq!(decide(expired), Decision::Cancel(Reason::IntentExpired));
    }

    #[test]
    fn climate_shift_cancels_before_any_budget_check() {
        let i = WatchInputs { high_transition_risk: true, inject_ack_timeout: true, ..calm() };
        assert_eq!(decide(i), Decision::Cancel(Reason::ClimateShiftExit));
    }

    #[test]
    fn execution_budget_composition_matches_the_cpp() {
        let at = |ms: i64, f: Option<f32>, session: Session, climate: Option<MarketClimate>| {
            decide(WatchInputs { local_now: T0 + ms * MS, fisher_info: f, session, climate, ..calm() })
        };
        // Core: min(1200, ack 1200) = 1200, no Fisher scaling, no climate.
        assert_eq!(at(1200, Some(0.4), Session::Core, None), Decision::Hold);
        assert_eq!(at(1201, Some(0.4), Session::Core, None), Decision::Cancel(Reason::OrderAckTimeout));
        // Sharp Fisher: 1200 * 0.75 = 900.
        assert_eq!(at(901, Some(0.7), Session::Core, None), Decision::Cancel(Reason::OrderAckTimeout));
        // Fuzzy Fisher: 1200 * 1.5 = 1800 -- the ack-timeout cap applies BEFORE the scale.
        assert_eq!(at(1800, Some(0.1), Session::Core, None), Decision::Hold);
        // Closing session: 800.
        assert_eq!(at(801, None, Session::Closing, None), Decision::Cancel(Reason::OrderAckTimeout));
        // Climate cap wins: TalebianFragile 100 ms, even in Core with fuzzy Fisher.
        assert_eq!(at(100, Some(0.1), Session::Core, Some(MarketClimate::TalebianFragile)), Decision::Hold);
        assert_eq!(at(101, Some(0.1), Session::Core, Some(MarketClimate::TalebianFragile)), Decision::Cancel(Reason::OrderAckTimeout));
        // GaussianStable's 500 ms cap is below every session budget -- the finding in the module doc.
        assert_eq!(at(501, Some(0.4), Session::Core, Some(MarketClimate::GaussianStable)), Decision::Cancel(Reason::OrderAckTimeout));
        // Fault injection.
        assert_eq!(decide(WatchInputs { inject_ack_timeout: true, ..calm() }), Decision::Cancel(Reason::OrderAckTimeout));
    }

    #[test]
    fn invalid_quote_holds_and_skips_the_later_checks() {
        let i = WatchInputs { bid: 7858.25, ask: 7858.25, volume_imbalance: -1.0, prediction_freshness: 0.0, ..calm() };
        assert_eq!(decide(i), Decision::Hold);
    }

    #[test]
    fn spread_toxic_flow_direction_and_staleness() {
        // Core allows 2 ticks: a 0.50 spread is 2 ticks (held), 0.75 is 3 ticks (breach).
        assert_eq!(decide(WatchInputs { ask: 7858.50, ..calm() }), Decision::Hold, "exactly 2 ticks allowed");
        assert_eq!(decide(WatchInputs { ask: 7858.75, ..calm() }), Decision::Cancel(Reason::SpreadBreach));
        assert_eq!(decide(WatchInputs { ask: 7858.50, session: Session::Closing, ..calm() }), Decision::Cancel(Reason::SpreadBreach), "closing allows 1 tick");

        assert_eq!(decide(WatchInputs { volume_imbalance: -0.36, ..calm() }), Decision::Cancel(Reason::ToxicFlowBreach));
        assert_eq!(decide(WatchInputs { volume_imbalance: -0.35, ..calm() }), Decision::Hold, "boundary is strict");
        assert_eq!(decide(WatchInputs { is_long: false, volume_imbalance: 0.36, ..calm() }), Decision::Cancel(Reason::ToxicFlowBreach));
        assert_eq!(decide(WatchInputs { is_long: false, volume_imbalance: -0.9, ..calm() }), Decision::Hold, "selling pressure helps a short");

        assert_eq!(decide(WatchInputs { prediction_direction_conflict: true, ..calm() }), Decision::Cancel(Reason::PredictionDirectionBreach));
        assert_eq!(decide(WatchInputs { prediction_freshness: 0.0, ..calm() }), Decision::Cancel(Reason::PredictionStaleDecay));
    }

    #[test]
    fn quote_churn_ema() {
        let mut f = QuoteChurnFilter::default();
        assert!(!f.breached(1.0, 1.25, 0, 11 * 60), "first sample only seeds");
        // A change every 10 ms = 100/s instantaneous; EMA: 20, 36, 48.8 ... crosses 8/s at once.
        assert!(f.breached(1.25, 1.5, 10 * MS, 11 * 60));
        assert!((f.rate_per_sec() - 20.0).abs() < 1e-9);
        // Unchanged quotes decay it by 0.8 per sample.
        let mut slow = QuoteChurnFilter::default();
        slow.breached(1.0, 1.25, 0, 11 * 60);
        assert!(!slow.breached(1.25, 1.5, 1_000 * MS, 11 * 60), "1 change/s -> EMA 0.2/s");
        assert!(!slow.breached(1.25, 1.5, 2_000 * MS, 11 * 60));
        assert!((slow.rate_per_sec() - 0.16).abs() < 1e-9);
    }

    #[test]
    fn churn_limit_is_tighter_near_the_open_and_close() {
        // Drive the EMA to exactly 7/s: breaches at 6/s (open/close windows) but not at 8/s.
        let seeded = |minute| {
            let mut f = QuoteChurnFilter { prev: Some((1.0, 1.25, 0)), ema_per_sec: 7.0 / 0.8 };
            f.breached(1.0, 1.25, 1_000 * MS, minute) // unchanged quote: EMA -> 7.0
        };
        assert!(seeded(9 * 60 + 45), "09:45 counts as the open window");
        assert!(!seeded(9 * 60 + 46));
        assert!(!seeded(15 * 60 + 44));
        assert!(seeded(15 * 60 + 45));
    }

    #[test]
    fn churn_breach_cancels_through_decide() {
        let cfg = WatcherConfig::sierra_chase_defaults();
        let mut churn = QuoteChurnFilter::default();
        let mut w = WorkingOrder::new();
        assert_eq!(w.decide(&calm(), &cfg, &mut churn), Decision::Hold);
        let moved = WatchInputs { market_now: T0 + 10 * MS, bid: 7858.25, ask: 7858.50, ..calm() };
        assert_eq!(w.decide(&moved, &cfg, &mut churn), Decision::Cancel(Reason::QuoteChurnBreach));
    }

    #[test]
    fn config_clamps_match_risk_manager() {
        let c = WatcherConfig { order_ack_timeout_ms: 5, working_order_ttl_ms: 10_000_000, ..WatcherConfig::sierra_chase_defaults() }.clamped();
        assert_eq!((c.order_ack_timeout_ms, c.working_order_ttl_ms), (100, 300_000));
    }
}
