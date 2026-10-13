//! Broker-mirror position state (spec docs/superpowers/specs/2026-10-09-mindfultrader-rust-migration-spec.md
//! §8.4, §8.7 D2 "Results"). NOT a port and NOT a ledger: the broker of record -- TWS via `ibapi`
//! live, Sierra's trade simulator in replay -- is the source of truth for position, average price,
//! P&L, commissions and fills. This module never recomputes those to serve them; it
//!
//! 1. **mirrors** the broker's last-reported position (`broker_position`),
//! 2. **annotates** what no broker knows: which decision (`decision_id`) each round trip came
//!    from (the `.btst` join key), its fills, MAE/MFE, and the orders sent but not yet resolved,
//! 3. **reconciles** the position implied by the broker's own fill stream against the broker's own
//!    position reports. The two legitimately disagree for a moment (a fill reported before the
//!    position update, or the reverse), so a mismatch only becomes a `PositionDiverged` alert once
//!    it outlasts a grace window -- and then the broker wins: the implied position is re-based to
//!    the broker's. A lasting divergence is the signature of an orphaned bracket leg, a double
//!    entry, or a manual order -- exactly the execution flaws the backtest tier exists to catch.
//! 4. **reports staleness** honestly: when the broker is disconnected or silent, callers get "last
//!    known as of T", never silently-served old values.
//!
//! Clock-free by construction (spec D2 A2): every event carries its own event timestamp and every
//! time-dependent query takes `now` from the caller, so replay time and live time are the same
//! code path. No `ibapi`/Sierra types appear here -- broker adapters translate into `Event`.
//!
//! Units: quantities are signed whole contracts (+long / -short); prices are in the contract's
//! price units; money fields are whatever the broker reported (currency), passed through verbatim.

use std::collections::{HashMap, HashSet};

/// Event time in microseconds since the Unix epoch (the repo's timing convention).
pub type TimestampUs = i64;

/// Broker-side order id (IBKR `orderId` / Sierra `InternalOrderID`).
pub type OrderId = i64;

/// Contract facts the mirror needs, taken from the broker's contract details (W0e read 0.25 and 5
/// for MES from IBKR) -- never hard-coded.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct ContractSpec {
    tick_size: f64,
    multiplier: f64,
}

impl ContractSpec {
    /// `None` unless both values are finite and positive.
    pub fn new(tick_size: f64, multiplier: f64) -> Option<Self> {
        (tick_size.is_finite() && tick_size > 0.0 && multiplier.is_finite() && multiplier > 0.0)
            .then_some(Self { tick_size, multiplier })
    }

    pub fn tick_size(&self) -> f64 {
        self.tick_size
    }

    pub fn multiplier(&self) -> f64 {
        self.multiplier
    }
}

/// What an order is for -- the system's own label, recorded at submission.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OrderRole {
    Entry,
    Target,
    Stop,
    Flatten,
    Other,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum OrderStatus {
    Submitted,
    Acknowledged,
    PartiallyFilled,
    Filled,
    Cancelled,
    Rejected,
}

impl OrderStatus {
    fn is_terminal(self) -> bool {
        matches!(self, Self::Filled | Self::Cancelled | Self::Rejected)
    }
}

/// One order this system sent, as the mirror tracks it.
#[derive(Debug, Clone, PartialEq)]
pub struct OrderRecord {
    pub order_id: OrderId,
    pub role: OrderRole,
    /// Signed ordered quantity (+buy / -sell).
    pub signed_qty: i64,
    /// Signed quantity filled so far, from broker fills.
    pub filled_qty: i64,
    pub decision_id: Option<u64>,
    pub status: OrderStatus,
    pub submitted_at: TimestampUs,
}

/// One execution as the broker reported it.
#[derive(Debug, Clone, PartialEq)]
pub struct Fill {
    /// Broker execution id (IBKR `execId`); a repeated id is the same execution, not a new one.
    pub exec_id: String,
    pub order_id: OrderId,
    /// Signed filled quantity (+bought / -sold).
    pub signed_qty: i64,
    pub price: f64,
    /// Broker-reported commission for this execution, if reported.
    pub commission: Option<f64>,
    /// Broker-reported realized P&L for this execution, if reported (IBKR's commission report
    /// carries one for closing executions). Passed through, never computed here.
    pub realized_pnl: Option<f64>,
    pub at: TimestampUs,
}

/// Everything the mirror learns, translated from `ibapi` or the Sierra relay by the adapter.
#[derive(Debug, Clone, PartialEq)]
pub enum Event {
    OrderSubmitted { order_id: OrderId, role: OrderRole, signed_qty: i64, decision_id: Option<u64>, at: TimestampUs },
    OrderAcknowledged { order_id: OrderId, at: TimestampUs },
    OrderRejected { order_id: OrderId, code: i32, message: String, at: TimestampUs },
    OrderCancelled { order_id: OrderId, at: TimestampUs },
    /// The broker confirmed an amend. Only the quantity matters here (it bounds over-fill
    /// detection); price changes are the broker's business.
    OrderAmended { order_id: OrderId, quantity: Option<i64>, at: TimestampUs },
    Fill(Fill),
    /// The broker's own position report -- the truth this module mirrors.
    BrokerPosition { quantity: i64, average_price: f64, realized_pnl: Option<f64>, unrealized_pnl: Option<f64>, at: TimestampUs },
    /// A market price observation, for MAE/MFE only.
    Mark { price: f64, at: TimestampUs },
    BrokerConnectivity { connected: bool, at: TimestampUs },
}

/// Something callers must know about. Returned from `apply`/`check`, never swallowed.
#[derive(Debug, Clone, PartialEq)]
pub enum Alert {
    /// A repeated execution id -- ignored (brokers resend executions on reconnect).
    DuplicateFill { exec_id: String },
    /// A fill for an order this system never submitted: a manual order or an orphaned leg.
    FillForUnknownOrder { order_id: OrderId, exec_id: String },
    /// Fills on one order add up to more than was ordered.
    OverFill { order_id: OrderId, ordered: i64, filled: i64 },
    OrderRejected { order_id: OrderId, code: i32, message: String },
    /// Fill-implied and broker-reported positions disagreed for longer than the grace window.
    /// The implied position has been re-based to the broker's (the broker wins).
    PositionDiverged { implied: i64, broker: i64, since: TimestampUs },
    /// The broker connection dropped while orders were working (raised once per disconnect).
    /// IBKR has no API cancel-on-disconnect, so these keep working at the broker unmanaged until
    /// reconnect; the operator decides whether to cancel by hand (spec §8.7 D3). `entries` are the
    /// unfilled entries -- the candidates for a manual cancel; `protective` are the target, stop
    /// and flatten orders reducing an open position, which must be left alone; `other` are orders
    /// of no known role.
    DisconnectedWithWorkingOrders { since: TimestampUs, entries: Vec<OrderId>, protective: Vec<OrderId>, other: Vec<OrderId> },
}

/// The broker's last-reported position.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct BrokerPosition {
    pub quantity: i64,
    pub average_price: f64,
    pub realized_pnl: Option<f64>,
    pub unrealized_pnl: Option<f64>,
    pub at: TimestampUs,
}

/// How far the mirror can be trusted right now.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum BrokerStatus {
    /// No broker report has ever arrived.
    NeverSeen,
    /// Connected and heard from within the freshness limit.
    Live,
    /// Connected, but silent for `age_us`.
    Stale { age_us: i64 },
    /// Disconnected since `since`; values are last known only.
    Disconnected { since: TimestampUs },
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Reconciliation {
    InSync,
    /// Disagreeing since `since`, still within the grace window.
    Pending { implied: i64, broker: i64, since: TimestampUs },
}

/// One round trip: flat -> position -> flat, as observed through broker fills.
#[derive(Debug, Clone, PartialEq)]
pub struct TradeRecord {
    /// `decision_id` of the order whose fill opened the trade (None for unknown/manual orders).
    pub decision_id: Option<u64>,
    /// +1 long / -1 short.
    pub direction: i8,
    pub opened_at: TimestampUs,
    pub closed_at: Option<TimestampUs>,
    /// The fills belonging to this trade, in arrival order (a flip's fill is split across the
    /// closing and the opening trade).
    pub fills: Vec<Fill>,
    /// Largest absolute position reached.
    pub max_quantity: i64,
    /// Max favorable / adverse excursion in ticks against the volume-weighted entry price of the
    /// position-increasing fills -- an annotation, not a P&L figure.
    pub mfe_ticks: f64,
    pub mae_ticks: f64,
    /// Sum of broker-reported per-fill realized P&L / commission -- `None` unless the broker
    /// reported a value for every fill in the trade (never filled in by computation).
    pub reported_realized_pnl: Option<f64>,
    pub reported_commission: Option<f64>,
}

impl TradeRecord {
    /// Volume-weighted price of the position-increasing fills.
    pub fn entry_price(&self) -> Option<f64> {
        vwap(self.fills.iter().filter(|f| f.signed_qty.signum() == i64::from(self.direction)))
    }

    /// Volume-weighted price of the position-reducing fills.
    pub fn exit_price(&self) -> Option<f64> {
        vwap(self.fills.iter().filter(|f| self.is_reducing(f)))
    }

    fn is_reducing(&self, f: &Fill) -> bool {
        f.signed_qty.signum() == -i64::from(self.direction)
    }
}

fn vwap<'a>(fills: impl Iterator<Item = &'a Fill>) -> Option<f64> {
    let (qty, notional) = fills.fold((0i64, 0.0f64), |(q, n), f| (q + f.signed_qty.abs(), n + f.price * f.signed_qty.abs() as f64));
    (qty > 0).then(|| notional / qty as f64)
}

/// Tunables. `grace_us` absorbs normal fill-vs-position-report lag; `freshness_us` bounds how long
/// a connected broker may stay silent before the mirror reports itself stale.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct Config {
    pub grace_us: i64,
    pub freshness_us: i64,
}

impl Default for Config {
    fn default() -> Self {
        Self { grace_us: 2_000_000, freshness_us: 30_000_000 }
    }
}

#[derive(Debug, Clone)]
pub struct PositionState {
    spec: ContractSpec,
    config: Config,
    broker: Option<BrokerPosition>,
    /// Position implied by broker fills; `None` until the first broker position report sets the
    /// baseline (so a session that starts with an existing position adopts it, not zero).
    implied: Option<i64>,
    mismatch_since: Option<TimestampUs>,
    connected: bool,
    disconnected_since: Option<TimestampUs>,
    last_broker_event_at: Option<TimestampUs>,
    orders: HashMap<OrderId, OrderRecord>,
    seen_exec_ids: HashSet<String>,
    open_trade: Option<TradeRecord>,
    closed_trades: Vec<TradeRecord>,
    last_mark: Option<f64>,
}

impl PositionState {
    pub fn new(spec: ContractSpec, config: Config) -> Self {
        Self {
            spec,
            config,
            broker: None,
            implied: None,
            mismatch_since: None,
            connected: true,
            disconnected_since: None,
            last_broker_event_at: None,
            orders: HashMap::new(),
            seen_exec_ids: HashSet::new(),
            open_trade: None,
            closed_trades: Vec::new(),
            last_mark: None,
        }
    }

    /// Apply one event; returns any alerts it raised.
    pub fn apply(&mut self, event: Event) -> Vec<Alert> {
        let mut alerts = Vec::new();
        match event {
            Event::OrderSubmitted { order_id, role, signed_qty, decision_id, at } => {
                self.orders.insert(
                    order_id,
                    OrderRecord { order_id, role, signed_qty, filled_qty: 0, decision_id, status: OrderStatus::Submitted, submitted_at: at },
                );
            }
            Event::OrderAcknowledged { order_id, at } => {
                self.heard_from_broker(at);
                if let Some(o) = self.orders.get_mut(&order_id).filter(|o| o.status == OrderStatus::Submitted) {
                    o.status = OrderStatus::Acknowledged;
                }
            }
            Event::OrderRejected { order_id, code, message, at } => {
                self.heard_from_broker(at);
                if let Some(o) = self.orders.get_mut(&order_id) {
                    o.status = OrderStatus::Rejected;
                }
                alerts.push(Alert::OrderRejected { order_id, code, message });
            }
            Event::OrderCancelled { order_id, at } => {
                self.heard_from_broker(at);
                if let Some(o) = self.orders.get_mut(&order_id).filter(|o| !o.status.is_terminal()) {
                    o.status = OrderStatus::Cancelled;
                }
            }
            Event::OrderAmended { order_id, quantity, at } => {
                self.heard_from_broker(at);
                if let (Some(q), Some(o)) = (quantity, self.orders.get_mut(&order_id).filter(|o| !o.status.is_terminal())) {
                    o.signed_qty = o.signed_qty.signum() * q.abs();
                    if o.filled_qty != 0 && o.filled_qty.abs() >= o.signed_qty.abs() {
                        o.status = OrderStatus::Filled; // cut down to what had already filled
                    }
                }
            }
            Event::Fill(fill) => self.apply_fill(fill, &mut alerts),
            Event::BrokerPosition { quantity, average_price, realized_pnl, unrealized_pnl, at } => {
                self.heard_from_broker(at);
                self.broker = Some(BrokerPosition { quantity, average_price, realized_pnl, unrealized_pnl, at });
                match self.implied {
                    None => self.implied = Some(quantity),
                    Some(implied) if implied == quantity => self.mismatch_since = None,
                    Some(_) => {
                        self.mismatch_since.get_or_insert(at);
                    }
                }
                self.check_into(at, &mut alerts);
            }
            Event::Mark { price, at: _ } => self.observe_price(price),
            Event::BrokerConnectivity { connected, at } => {
                self.connected = connected;
                if connected {
                    self.disconnected_since = None;
                    self.heard_from_broker(at);
                } else if self.disconnected_since.is_none() {
                    self.disconnected_since = Some(at);
                    let (mut entries, mut protective, mut other) = (Vec::new(), Vec::new(), Vec::new());
                    for o in self.working_orders() {
                        match o.role {
                            OrderRole::Entry => entries.push(o.order_id),
                            OrderRole::Target | OrderRole::Stop | OrderRole::Flatten => protective.push(o.order_id),
                            OrderRole::Other => other.push(o.order_id),
                        }
                    }
                    if !(entries.is_empty() && protective.is_empty() && other.is_empty()) {
                        for v in [&mut entries, &mut protective, &mut other] {
                            v.sort_unstable();
                        }
                        alerts.push(Alert::DisconnectedWithWorkingOrders { since: at, entries, protective, other });
                    }
                }
            }
        }
        alerts
    }

    /// Time-driven checks (the grace window): call with the current event time.
    pub fn check(&mut self, now: TimestampUs) -> Vec<Alert> {
        let mut alerts = Vec::new();
        self.check_into(now, &mut alerts);
        alerts
    }

    fn check_into(&mut self, now: TimestampUs, alerts: &mut Vec<Alert>) {
        let (Some(since), Some(implied), Some(broker)) = (self.mismatch_since, self.implied, self.broker) else {
            return;
        };
        if now - since > self.config.grace_us {
            alerts.push(Alert::PositionDiverged { implied, broker: broker.quantity, since });
            self.implied = Some(broker.quantity); // the broker wins
            self.mismatch_since = None;
        }
    }

    fn heard_from_broker(&mut self, at: TimestampUs) {
        self.last_broker_event_at = Some(self.last_broker_event_at.map_or(at, |t| t.max(at)));
    }

    fn apply_fill(&mut self, fill: Fill, alerts: &mut Vec<Alert>) {
        self.heard_from_broker(fill.at);
        if !self.seen_exec_ids.insert(fill.exec_id.clone()) {
            alerts.push(Alert::DuplicateFill { exec_id: fill.exec_id });
            return;
        }
        let decision_id = match self.orders.get_mut(&fill.order_id) {
            Some(o) => {
                o.filled_qty += fill.signed_qty;
                if o.filled_qty.abs() > o.signed_qty.abs() {
                    alerts.push(Alert::OverFill { order_id: o.order_id, ordered: o.signed_qty, filled: o.filled_qty });
                }
                o.status = if o.filled_qty.abs() >= o.signed_qty.abs() { OrderStatus::Filled } else { OrderStatus::PartiallyFilled };
                o.decision_id
            }
            None => {
                alerts.push(Alert::FillForUnknownOrder { order_id: fill.order_id, exec_id: fill.exec_id.clone() });
                None
            }
        };

        let before = self.implied.unwrap_or(0);
        let after = before + fill.signed_qty;
        if self.implied.is_some() {
            self.implied = Some(after);
            self.refresh_reconciliation(fill.at);
        }
        self.annotate_fill(before, after, fill, decision_id);
    }

    /// Fills move the implied position; a fill can either resolve or start a mismatch.
    fn refresh_reconciliation(&mut self, at: TimestampUs) {
        if let (Some(implied), Some(broker)) = (self.implied, self.broker) {
            if implied == broker.quantity {
                self.mismatch_since = None;
            } else {
                self.mismatch_since.get_or_insert(at);
            }
        }
    }

    fn annotate_fill(&mut self, before: i64, after: i64, fill: Fill, decision_id: Option<u64>) {
        self.observe_price(fill.price);
        let crosses_zero = before != 0 && after != 0 && before.signum() != after.signum();
        if crosses_zero {
            // Flip: one execution both closes the trade and opens the opposite one. Split it: the
            // flattening part closes the trade and keeps all of the broker's realized P&L (that is
            // where it was realized); the broker's commission is allocated by quantity -- a
            // documented allocation of the broker's own number, not a computed figure.
            let total = fill.signed_qty.abs() as f64;
            let close_share = before.abs() as f64 / total;
            let closing = Fill {
                signed_qty: -before,
                commission: fill.commission.map(|c| c * close_share),
                realized_pnl: fill.realized_pnl,
                ..fill.clone()
            };
            let opening = Fill { signed_qty: after, commission: fill.commission.map(|c| c * (1.0 - close_share)), realized_pnl: None, ..fill };
            self.push_fill(closing);
            self.close_trade(opening.at);
            self.start_trade(opening, decision_id, after);
            return;
        }
        if before == 0 && after != 0 {
            self.start_trade(fill, decision_id, after);
        } else if let Some(t) = self.open_trade.as_mut() {
            t.max_quantity = t.max_quantity.max(after.abs());
            let at = fill.at;
            self.push_fill(fill);
            if after == 0 {
                self.close_trade(at);
            }
        }
        // A fill while flat-and-staying-flat (before == after == 0) cannot happen: signed_qty != 0
        // moves the position. A zero-quantity fill is recorded nowhere.
    }

    fn start_trade(&mut self, fill: Fill, decision_id: Option<u64>, after: i64) {
        let opened_at = fill.at;
        self.open_trade = Some(TradeRecord {
            decision_id,
            direction: if after > 0 { 1 } else { -1 },
            opened_at,
            closed_at: None,
            fills: vec![fill],
            max_quantity: after.abs(),
            mfe_ticks: 0.0,
            mae_ticks: 0.0,
            reported_realized_pnl: None,
            reported_commission: None,
        });
    }

    fn push_fill(&mut self, fill: Fill) {
        if let Some(t) = self.open_trade.as_mut() {
            t.fills.push(fill);
        }
    }

    fn close_trade(&mut self, at: TimestampUs) {
        if let Some(mut t) = self.open_trade.take() {
            t.closed_at = Some(at);
            // Realized P&L exists only on position-reducing fills; `None` if any went unreported.
            t.reported_realized_pnl = t.fills.iter().filter(|f| t.is_reducing(f)).map(|f| f.realized_pnl).sum();
            t.reported_commission = t.fills.iter().map(|f| f.commission).sum();
            self.closed_trades.push(t);
        }
    }

    /// Track MAE/MFE for the open trade against its volume-weighted entry price.
    fn observe_price(&mut self, price: f64) {
        if !price.is_finite() {
            return;
        }
        self.last_mark = Some(price);
        let tick = self.spec.tick_size;
        if let Some(t) = self.open_trade.as_mut()
            && let Some(entry) = t.entry_price()
        {
            let favorable = (price - entry) * f64::from(t.direction) / tick;
            t.mfe_ticks = t.mfe_ticks.max(favorable);
            t.mae_ticks = t.mae_ticks.max(-favorable);
        }
    }

    // ----- queries -----

    /// The broker's last-reported position -- the truth. Check `broker_status` before acting on it.
    pub fn broker_position(&self) -> Option<BrokerPosition> {
        self.broker
    }

    /// Position implied by broker fills (the reconciliation counterpart, not the truth).
    pub fn implied_quantity(&self) -> Option<i64> {
        self.implied
    }

    pub fn reconciliation(&self) -> Reconciliation {
        match (self.mismatch_since, self.implied, self.broker) {
            (Some(since), Some(implied), Some(b)) => Reconciliation::Pending { implied, broker: b.quantity, since },
            _ => Reconciliation::InSync,
        }
    }

    pub fn broker_status(&self, now: TimestampUs) -> BrokerStatus {
        if let Some(since) = self.disconnected_since {
            return BrokerStatus::Disconnected { since };
        }
        match self.last_broker_event_at {
            None => BrokerStatus::NeverSeen,
            Some(t) if now - t > self.config.freshness_us => BrokerStatus::Stale { age_us: now - t },
            Some(_) => BrokerStatus::Live,
        }
    }

    pub fn order(&self, order_id: OrderId) -> Option<&OrderRecord> {
        self.orders.get(&order_id)
    }

    /// Orders sent and not yet filled, cancelled or rejected.
    pub fn working_orders(&self) -> impl Iterator<Item = &OrderRecord> {
        self.orders.values().filter(|o| !o.status.is_terminal())
    }

    pub fn open_trade(&self) -> Option<&TradeRecord> {
        self.open_trade.as_ref()
    }

    /// Completed round trips since the last call (drained).
    pub fn take_closed_trades(&mut self) -> Vec<TradeRecord> {
        std::mem::take(&mut self.closed_trades)
    }

    pub fn contract(&self) -> ContractSpec {
        self.spec
    }

    pub fn last_mark(&self) -> Option<f64> {
        self.last_mark
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    const MES_TICK: f64 = 0.25;
    const S: TimestampUs = 1_000_000; // one second in event-time microseconds

    fn close(a: f64, b: f64) -> bool {
        (a - b).abs() < 1e-9
    }

    fn mes() -> PositionState {
        let spec = ContractSpec::new(MES_TICK, 5.0).expect("valid MES spec");
        PositionState::new(spec, Config { grace_us: 2 * S, freshness_us: 30 * S })
    }

    fn flat_baseline(ps: &mut PositionState) {
        assert!(ps.apply(broker_pos(0, 0)).is_empty());
    }

    fn broker_pos(quantity: i64, at: TimestampUs) -> Event {
        Event::BrokerPosition { quantity, average_price: 0.0, realized_pnl: None, unrealized_pnl: None, at }
    }

    fn submit(order_id: OrderId, signed_qty: i64, decision_id: Option<u64>, at: TimestampUs) -> Event {
        Event::OrderSubmitted { order_id, role: OrderRole::Entry, signed_qty, decision_id, at }
    }

    fn fill(exec: &str, order_id: OrderId, signed_qty: i64, price: f64, at: TimestampUs) -> Fill {
        Fill { exec_id: exec.into(), order_id, signed_qty, price, commission: Some(0.61), realized_pnl: None, at }
    }

    #[test]
    fn contract_spec_rejects_non_positive_or_non_finite() {
        assert!(ContractSpec::new(0.25, 5.0).is_some());
        assert!(ContractSpec::new(0.0, 5.0).is_none());
        assert!(ContractSpec::new(0.25, -5.0).is_none());
        assert!(ContractSpec::new(f64::NAN, 5.0).is_none());
        assert!(ContractSpec::new(0.25, f64::INFINITY).is_none());
    }

    #[test]
    fn first_broker_report_sets_the_baseline_including_an_existing_position() {
        let mut ps = mes();
        assert_eq!(ps.implied_quantity(), None);
        ps.apply(broker_pos(2, 0));
        assert_eq!(ps.implied_quantity(), Some(2));
        assert_eq!(ps.broker_position().map(|b| b.quantity), Some(2));
        assert_eq!(ps.reconciliation(), Reconciliation::InSync);
    }

    #[test]
    fn fill_before_position_report_is_normal_lag_not_divergence() {
        let mut ps = mes();
        flat_baseline(&mut ps);
        ps.apply(submit(1, 1, Some(42), S));
        ps.apply(Event::OrderAcknowledged { order_id: 1, at: S });
        assert!(ps.apply(Event::Fill(fill("e1", 1, 1, 7858.0, S))).is_empty());
        assert_eq!(ps.reconciliation(), Reconciliation::Pending { implied: 1, broker: 0, since: S });
        assert!(ps.check(2 * S).is_empty(), "still inside the grace window");
        assert!(ps.apply(broker_pos(1, 2 * S)).is_empty());
        assert_eq!(ps.reconciliation(), Reconciliation::InSync);
    }

    #[test]
    fn position_report_before_fill_is_also_normal_lag() {
        let mut ps = mes();
        flat_baseline(&mut ps);
        ps.apply(submit(1, 1, None, S));
        assert!(ps.apply(broker_pos(1, S)).is_empty());
        assert!(matches!(ps.reconciliation(), Reconciliation::Pending { implied: 0, broker: 1, .. }));
        assert!(ps.apply(Event::Fill(fill("e1", 1, 1, 7858.0, S + 1))).is_empty());
        assert_eq!(ps.reconciliation(), Reconciliation::InSync);
    }

    #[test]
    fn lasting_divergence_alerts_once_and_the_broker_wins() {
        let mut ps = mes();
        flat_baseline(&mut ps);
        ps.apply(submit(1, 1, None, S));
        ps.apply(Event::Fill(fill("e1", 1, 1, 7858.0, S)));
        // The broker keeps reporting flat: e.g. our fill was busted, or a leg we don't know of.
        assert!(ps.apply(broker_pos(0, 2 * S)).is_empty(), "inside the grace window");
        assert!(ps.check(3 * S).is_empty(), "exactly at the grace edge (2s elapsed): not yet");
        let alerts = ps.check(3 * S + 1);
        assert_eq!(alerts, vec![Alert::PositionDiverged { implied: 1, broker: 0, since: S }]);
        assert_eq!(ps.implied_quantity(), Some(0), "re-based to the broker");
        assert_eq!(ps.reconciliation(), Reconciliation::InSync);
        assert!(ps.check(10 * S).is_empty(), "one alert per episode");
    }

    #[test]
    fn duplicate_execution_is_ignored_and_reported() {
        let mut ps = mes();
        flat_baseline(&mut ps);
        ps.apply(submit(1, 1, None, S));
        ps.apply(Event::Fill(fill("e1", 1, 1, 7858.0, S)));
        let alerts = ps.apply(Event::Fill(fill("e1", 1, 1, 7858.0, S)));
        assert_eq!(alerts, vec![Alert::DuplicateFill { exec_id: "e1".into() }]);
        assert_eq!(ps.implied_quantity(), Some(1));
        assert_eq!(ps.order(1).map(|o| o.filled_qty), Some(1));
    }

    #[test]
    fn fill_for_an_order_we_never_sent_is_flagged() {
        let mut ps = mes();
        flat_baseline(&mut ps);
        let alerts = ps.apply(Event::Fill(fill("m1", 99, -1, 7858.0, S)));
        assert_eq!(alerts, vec![Alert::FillForUnknownOrder { order_id: 99, exec_id: "m1".into() }]);
        assert_eq!(ps.implied_quantity(), Some(-1));
        let t = ps.open_trade().expect("the manual fill still opens a trade");
        assert_eq!((t.decision_id, t.direction), (None, -1));
    }

    #[test]
    fn long_round_trip_annotations() {
        let mut ps = mes();
        flat_baseline(&mut ps);
        ps.apply(submit(1, 1, Some(7), S));
        ps.apply(Event::Fill(fill("e1", 1, 1, 7858.0, S)));
        ps.apply(Event::Mark { price: 7860.0, at: 2 * S }); // +8 ticks
        ps.apply(Event::Mark { price: 7855.5, at: 3 * S }); // -10 ticks
        ps.apply(Event::OrderSubmitted { order_id: 2, role: OrderRole::Target, signed_qty: -1, decision_id: Some(7), at: 3 * S });
        let exit = Fill { realized_pnl: Some(5.03), ..fill("e2", 2, -1, 7859.25, 4 * S) };
        ps.apply(Event::Fill(exit));

        assert!(ps.open_trade().is_none());
        let trades = ps.take_closed_trades();
        assert_eq!(trades.len(), 1);
        let t = &trades[0];
        assert_eq!((t.decision_id, t.direction, t.opened_at, t.closed_at), (Some(7), 1, S, Some(4 * S)));
        assert_eq!(t.max_quantity, 1);
        assert!(t.entry_price().is_some_and(|p| close(p, 7858.0)));
        assert!(t.exit_price().is_some_and(|p| close(p, 7859.25)));
        assert!(close(t.mfe_ticks, 8.0), "mfe {}", t.mfe_ticks);
        assert!(close(t.mae_ticks, 10.0), "mae {}", t.mae_ticks);
        assert!(t.reported_realized_pnl.is_some_and(|v| close(v, 5.03)), "passed through, not computed");
        assert!(t.reported_commission.is_some_and(|v| close(v, 1.22)));
        assert!(ps.take_closed_trades().is_empty(), "drained");
    }

    #[test]
    fn short_trade_excursions_have_the_right_sign() {
        let mut ps = mes();
        flat_baseline(&mut ps);
        ps.apply(submit(1, -1, None, S));
        ps.apply(Event::Fill(fill("e1", 1, -1, 7858.0, S)));
        ps.apply(Event::Mark { price: 7857.0, at: 2 * S }); // favorable for a short: +4 ticks
        ps.apply(Event::Mark { price: 7859.0, at: 3 * S }); // adverse: 4 ticks
        let t = ps.open_trade().expect("open short");
        assert!(close(t.mfe_ticks, 4.0) && close(t.mae_ticks, 4.0), "mfe {} mae {}", t.mfe_ticks, t.mae_ticks);
    }

    #[test]
    fn unreported_realized_pnl_stays_unreported() {
        let mut ps = mes();
        flat_baseline(&mut ps);
        ps.apply(submit(1, 1, None, S));
        ps.apply(Event::Fill(fill("e1", 1, 1, 100.0, S)));
        ps.apply(submit(2, -1, None, 2 * S));
        ps.apply(Event::Fill(fill("e2", 2, -1, 101.0, 2 * S))); // no realized_pnl from the broker
        let t = &ps.take_closed_trades()[0];
        assert_eq!(t.reported_realized_pnl, None, "never filled in by computation");
    }

    #[test]
    fn scale_in_and_partial_exits() {
        let mut ps = mes();
        flat_baseline(&mut ps);
        ps.apply(submit(1, 1, Some(1), S));
        ps.apply(Event::Fill(fill("e1", 1, 1, 100.0, S)));
        ps.apply(submit(2, 1, Some(2), 2 * S));
        ps.apply(Event::Fill(fill("e2", 2, 1, 102.0, 2 * S)));
        let t = ps.open_trade().expect("open");
        assert_eq!((t.max_quantity, t.decision_id), (2, Some(1)), "the opening order's decision owns the trade");
        assert!(t.entry_price().is_some_and(|p| close(p, 101.0)));
        ps.apply(submit(3, -2, None, 3 * S));
        ps.apply(Event::Fill(Fill { realized_pnl: Some(10.0), ..fill("e3", 3, -1, 103.0, 3 * S) }));
        assert!(ps.open_trade().is_some(), "half closed");
        assert_eq!(ps.order(3).map(|o| o.status), Some(OrderStatus::PartiallyFilled));
        ps.apply(Event::Fill(Fill { realized_pnl: Some(15.0), ..fill("e4", 3, -1, 104.0, 4 * S) }));
        let t = &ps.take_closed_trades()[0];
        assert!(t.exit_price().is_some_and(|p| close(p, 103.5)));
        assert!(t.reported_realized_pnl.is_some_and(|v| close(v, 25.0)));
        assert_eq!(ps.order(3).map(|o| o.status), Some(OrderStatus::Filled));
    }

    #[test]
    fn flip_through_zero_splits_one_execution_across_two_trades() {
        let mut ps = mes();
        flat_baseline(&mut ps);
        ps.apply(submit(1, 1, Some(1), S));
        ps.apply(Event::Fill(fill("e1", 1, 1, 100.0, S)));
        ps.apply(submit(2, -3, Some(2), 2 * S));
        let flip = Fill { commission: Some(1.83), realized_pnl: Some(4.0), ..fill("e2", 2, -3, 101.0, 2 * S) };
        ps.apply(Event::Fill(flip));

        let closed = ps.take_closed_trades();
        assert_eq!(closed.len(), 1);
        let c = &closed[0];
        assert_eq!(c.fills.last().map(|f| f.signed_qty), Some(-1));
        assert!(c.reported_realized_pnl.is_some_and(|v| close(v, 4.0)), "realized belongs to the closing part");
        assert!(c.reported_commission.is_some_and(|v| close(v, 0.61 + 0.61)), "0.61 own + 1/3 of 1.83");

        let o = ps.open_trade().expect("new short");
        assert_eq!((o.direction, o.max_quantity, o.decision_id), (-1, 2, Some(2)));
        assert_eq!(o.fills[0].commission.map(|v| (v * 100.0).round()), Some(122.0), "2/3 of 1.83");
        assert_eq!(ps.implied_quantity(), Some(-2));
    }

    #[test]
    fn order_lifecycle_rejects_cancels_and_overfills() {
        let mut ps = mes();
        flat_baseline(&mut ps);
        ps.apply(submit(1, 1, None, S));
        ps.apply(submit(2, 1, None, S));
        ps.apply(submit(3, 1, None, S));
        assert_eq!(ps.working_orders().count(), 3);
        let alerts = ps.apply(Event::OrderRejected { order_id: 1, code: 439, message: "Algorithm definition not found".into(), at: S });
        assert_eq!(alerts, vec![Alert::OrderRejected { order_id: 1, code: 439, message: "Algorithm definition not found".into() }]);
        ps.apply(Event::OrderCancelled { order_id: 2, at: S });
        assert_eq!(ps.working_orders().count(), 1);
        ps.apply(Event::Fill(fill("e1", 3, 1, 100.0, S)));
        let alerts = ps.apply(Event::Fill(fill("e2", 3, 1, 100.0, S)));
        assert_eq!(alerts, vec![Alert::OverFill { order_id: 3, ordered: 1, filled: 2 }]);
        assert_eq!(ps.working_orders().count(), 0);
    }

    #[test]
    fn confirmed_amends_move_the_overfill_bound() {
        let mut ps = mes();
        flat_baseline(&mut ps);
        ps.apply(submit(1, 3, None, S));
        ps.apply(Event::OrderAmended { order_id: 1, quantity: Some(1), at: S });
        ps.apply(Event::Fill(fill("e1", 1, 1, 100.0, S)));
        assert_eq!(ps.working_orders().count(), 0, "amended down to 1: one fill completes it");
        let alerts = ps.apply(Event::Fill(fill("e2", 1, 1, 100.0, S)));
        assert_eq!(alerts, vec![Alert::OverFill { order_id: 1, ordered: 1, filled: 2 }]);

        // Raising a working sell keeps its sign; a price-only amend changes nothing here.
        ps.apply(submit(2, -1, None, S));
        ps.apply(Event::OrderAmended { order_id: 2, quantity: None, at: S });
        ps.apply(Event::OrderAmended { order_id: 2, quantity: Some(2), at: S });
        let alerts = ps.apply(Event::Fill(fill("e3", 2, -2, 100.0, S)));
        assert!(alerts.is_empty());

        // Cutting a partly filled order down to its filled quantity completes it.
        ps.apply(submit(3, 2, None, S));
        ps.apply(Event::Fill(fill("e4", 3, 1, 100.0, S)));
        ps.apply(Event::OrderAmended { order_id: 3, quantity: Some(1), at: S });
        assert_eq!(ps.working_orders().count(), 0);
    }

    #[test]
    fn broker_status_never_seen_live_stale_disconnected() {
        let mut ps = mes();
        assert_eq!(ps.broker_status(0), BrokerStatus::NeverSeen);
        ps.apply(broker_pos(0, 10 * S));
        assert_eq!(ps.broker_status(20 * S), BrokerStatus::Live);
        assert_eq!(ps.broker_status(41 * S), BrokerStatus::Stale { age_us: 31 * S });
        ps.apply(Event::BrokerConnectivity { connected: false, at: 42 * S });
        assert_eq!(ps.broker_status(50 * S), BrokerStatus::Disconnected { since: 42 * S });
        assert_eq!(ps.broker_position().map(|b| b.at), Some(10 * S), "last known value stays readable, with its timestamp");
        ps.apply(Event::BrokerConnectivity { connected: true, at: 60 * S });
        assert_eq!(ps.broker_status(61 * S), BrokerStatus::Live);
    }

    #[test]
    fn disconnect_with_working_orders_alerts_once_and_separates_protective_legs() {
        let mut ps = mes();
        flat_baseline(&mut ps);
        assert!(ps.apply(Event::BrokerConnectivity { connected: false, at: S }).is_empty(), "nothing working: no alert");
        ps.apply(Event::BrokerConnectivity { connected: true, at: 2 * S });

        ps.apply(submit(5, 1, Some(9), 3 * S));
        for (id, role) in [(6, OrderRole::Stop), (7, OrderRole::Target), (8, OrderRole::Flatten), (9, OrderRole::Other)] {
            ps.apply(Event::OrderSubmitted { order_id: id, role, signed_qty: -1, decision_id: Some(9), at: 3 * S });
        }
        let alerts = ps.apply(Event::BrokerConnectivity { connected: false, at: 4 * S });
        assert_eq!(alerts, vec![Alert::DisconnectedWithWorkingOrders { since: 4 * S, entries: vec![5], protective: vec![6, 7, 8], other: vec![9] }]);
        assert!(ps.apply(Event::BrokerConnectivity { connected: false, at: 5 * S }).is_empty(), "once per disconnect");
        assert_eq!(ps.broker_status(6 * S), BrokerStatus::Disconnected { since: 4 * S });
    }

    #[test]
    fn closing_every_trade_returns_to_flat_with_one_record_per_round_trip() {
        // Property-style: a scripted sequence of round trips of varying size and direction.
        let mut ps = mes();
        flat_baseline(&mut ps);
        let mut order: OrderId = 0;
        let mut exec = 0;
        let mut at = S;
        for (size, dir) in [(1i64, 1i64), (3, -1), (2, 1), (5, -1), (1, -1)] {
            for leg in [dir * size, -dir * size] {
                order += 1;
                exec += 1;
                at += S;
                ps.apply(submit(order, leg, Some(order as u64), at));
                ps.apply(Event::Fill(fill(&format!("e{exec}"), order, leg, 100.0, at)));
            }
        }
        assert_eq!(ps.implied_quantity(), Some(0));
        assert!(ps.open_trade().is_none());
        let trades = ps.take_closed_trades();
        assert_eq!(trades.len(), 5);
        assert!(trades.iter().all(|t| t.fills.iter().map(|f| f.signed_qty).sum::<i64>() == 0));
    }
}
