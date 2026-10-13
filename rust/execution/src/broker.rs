//! The broker seam (spec §8.6/§8.7 D2): one broker-neutral order request, one `BrokerAdapter`
//! trait, two implementations -- IBKR live (`ibkr_orders` describes its orders) and Sierra Chart's
//! simulator in replay (`sierra_sim` describes its orders, deliberately pessimistic). The business
//! logic (`position_state`, `order_gate`, `trap_exit`, ...) only ever sees this module's types, so
//! backtest and live run the same logic.
//!
//! Adapters report everything they learn -- including their own submissions -- as
//! `position_state::Event`s through `poll_events`, so `PositionState` has exactly one input path.

use crate::position_state::{Event, OrderId, OrderRole};

/// Buy or sell.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum Side {
    Buy,
    Sell,
}

impl Side {
    /// +1 for a buy, -1 for a sell.
    pub fn sign(self) -> i64 {
        match self {
            Self::Buy => 1,
            Self::Sell => -1,
        }
    }

    pub fn opposite(self) -> Self {
        match self {
            Self::Buy => Self::Sell,
            Self::Sell => Self::Buy,
        }
    }
}

/// How the entry is priced.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum Entry {
    Market,
    /// Limit at `price` -- for an algo order, the worst price the algo may pay.
    Limit { price: f64 },
}

/// Adaptive's urgency (IBKR `adaptivePriority`).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum AdaptivePriority {
    Patient,
    Normal,
    Urgent,
}

impl AdaptivePriority {
    pub fn ibkr_name(self) -> &'static str {
        match self {
            Self::Patient => "Patient",
            Self::Normal => "Normal",
            Self::Urgent => "Urgent",
        }
    }
}

/// How the broker works the order. Only styles confirmed on MES futures by W0e (spec §8.7 D3)
/// exist here -- BalanceImpactRisk and ArrivalPx were rejected `[439]` and are deliberately absent.
#[derive(Debug, Clone, Copy, PartialEq)]
pub enum ExecutionStyle {
    /// A plain exchange order, no algo.
    Direct,
    Adaptive(AdaptivePriority),
    Twap,
    /// `max_pct_vol` as a fraction (0.1 = 10%).
    Vwap { max_pct_vol: f64 },
    /// `pct_vol` as a fraction.
    PctVol { pct_vol: f64 },
    DarkIce { display_size: u32 },
}

/// An immutable bracket attached at entry (spec §8.7 D3a): never modified after placement, only
/// cancelled.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Bracket {
    pub target: f64,
    pub stop: f64,
}

/// One broker-neutral order intent.
#[derive(Debug, Clone, PartialEq)]
pub struct OrderRequest {
    pub role: OrderRole,
    pub side: Side,
    /// Whole contracts, > 0.
    pub quantity: i64,
    pub entry: Entry,
    pub style: ExecutionStyle,
    pub bracket: Option<Bracket>,
    pub decision_id: Option<u64>,
}

impl OrderRequest {
    /// Shape checks every adapter relies on: positive quantity, finite prices, a bracket only on
    /// entries and on the correct sides of the entry.
    pub fn validate(&self) -> Result<(), BrokerError> {
        let bad = |why: &str| Err(BrokerError::InvalidRequest(why.into()));
        if self.quantity <= 0 {
            return bad("quantity must be a positive whole number of contracts");
        }
        if let Entry::Limit { price } = self.entry
            && !(price.is_finite() && price > 0.0)
        {
            return bad("limit price must be finite and positive");
        }
        if let Some(b) = self.bracket {
            if self.role != OrderRole::Entry {
                return bad("a bracket can only be attached to an entry");
            }
            if !(b.target.is_finite() && b.stop.is_finite()) {
                return bad("bracket prices must be finite");
            }
            let (above, below) = match self.side {
                Side::Buy => (b.target, b.stop),
                Side::Sell => (b.stop, b.target),
            };
            if above <= below {
                return bad("bracket target and stop are on the wrong sides");
            }
            if let Entry::Limit { price } = self.entry
                && !(below < price && price < above)
            {
                return bad("bracket must straddle the limit price");
            }
        }
        Ok(())
    }
}

/// Broker ids for what one `place` created.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct PlacedOrder {
    pub parent: OrderId,
    pub target: Option<OrderId>,
    pub stop: Option<OrderId>,
}

#[derive(Debug, Clone, PartialEq)]
pub enum BrokerError {
    /// The request itself is malformed; nothing was sent.
    InvalidRequest(String),
    /// This adapter cannot represent the request (e.g. a slicing algo in the Sierra simulator).
    NotModelled(String),
    /// The broker could not be reached; nothing is known to have been sent.
    Disconnected,
    /// The broker refused it (code, message) -- e.g. IBKR `[439]`.
    Rejected { code: i32, message: String },
}

/// The seam between execution logic and a broker of record. Implementations: IBKR live, the
/// Sierra-simulator relay in replay, and test doubles.
pub trait BrokerAdapter {
    /// Places the order and its attached bracket, if any. The adapter also queues an
    /// `Event::OrderSubmitted` per created order, so submissions reach `PositionState` through
    /// `poll_events` like everything else.
    fn place(&mut self, request: &OrderRequest) -> Result<PlacedOrder, BrokerError>;

    fn cancel(&mut self, order_id: OrderId) -> Result<(), BrokerError>;

    /// Cancel every working order and close the position at market (emergency path).
    fn flatten(&mut self) -> Result<(), BrokerError>;

    /// Moves every broker event received since the last call into `out`, in arrival order.
    fn poll_events(&mut self, out: &mut Vec<Event>);
}

#[cfg(test)]
mod tests {
    use super::*;

    fn entry(side: Side, entry: Entry, bracket: Option<Bracket>) -> OrderRequest {
        OrderRequest { role: OrderRole::Entry, side, quantity: 1, entry, style: ExecutionStyle::Direct, bracket, decision_id: Some(1) }
    }

    #[test]
    fn valid_long_and_short_brackets() {
        let long = entry(Side::Buy, Entry::Limit { price: 100.0 }, Some(Bracket { target: 102.0, stop: 98.0 }));
        let short = entry(Side::Sell, Entry::Limit { price: 100.0 }, Some(Bracket { target: 98.0, stop: 102.0 }));
        assert_eq!(long.validate(), Ok(()));
        assert_eq!(short.validate(), Ok(()));
        assert_eq!(entry(Side::Buy, Entry::Market, Some(Bracket { target: 102.0, stop: 98.0 })).validate(), Ok(()));
    }

    #[test]
    fn rejects_malformed_requests() {
        let inverted = entry(Side::Buy, Entry::Limit { price: 100.0 }, Some(Bracket { target: 98.0, stop: 102.0 }));
        assert!(matches!(inverted.validate(), Err(BrokerError::InvalidRequest(_))));
        let not_straddling = entry(Side::Buy, Entry::Limit { price: 103.0 }, Some(Bracket { target: 102.0, stop: 98.0 }));
        assert!(matches!(not_straddling.validate(), Err(BrokerError::InvalidRequest(_))));
        let zero = OrderRequest { quantity: 0, ..entry(Side::Buy, Entry::Market, None) };
        assert!(matches!(zero.validate(), Err(BrokerError::InvalidRequest(_))));
        let nan = entry(Side::Buy, Entry::Limit { price: f64::NAN }, None);
        assert!(matches!(nan.validate(), Err(BrokerError::InvalidRequest(_))));
        let bracket_on_exit = OrderRequest { role: OrderRole::Flatten, ..entry(Side::Sell, Entry::Market, Some(Bracket { target: 98.0, stop: 102.0 })) };
        assert!(matches!(bracket_on_exit.validate(), Err(BrokerError::InvalidRequest(_))));
    }

    /// The trait must stay object-safe: the service holds `Box<dyn BrokerAdapter>`.
    #[test]
    fn adapter_is_object_safe() {
        #[derive(Debug, Default)]
        struct Null(Vec<Event>);
        impl BrokerAdapter for Null {
            fn place(&mut self, r: &OrderRequest) -> Result<PlacedOrder, BrokerError> {
                r.validate()?;
                self.0.push(Event::OrderSubmitted { order_id: 1, role: r.role, signed_qty: r.side.sign() * r.quantity, decision_id: r.decision_id, at: 0 });
                Ok(PlacedOrder { parent: 1, target: None, stop: None })
            }
            fn cancel(&mut self, _: OrderId) -> Result<(), BrokerError> {
                Ok(())
            }
            fn flatten(&mut self) -> Result<(), BrokerError> {
                Ok(())
            }
            fn poll_events(&mut self, out: &mut Vec<Event>) {
                out.append(&mut self.0);
            }
        }
        let mut b: Box<dyn BrokerAdapter> = Box::new(Null::default());
        assert!(b.place(&entry(Side::Sell, Entry::Market, None)).is_ok());
        let mut out = Vec::new();
        b.poll_events(&mut out);
        assert!(matches!(out.as_slice(), [Event::OrderSubmitted { signed_qty: -1, .. }]));
    }
}
