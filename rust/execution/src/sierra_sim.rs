//! Spec §8.7 D2's IBKR-algo -> Sierra-simulator order mapping, as one pure function. The Sierra
//! replay is a deliberately PESSIMISTIC broker (operator, 2026-10-13): every ambiguity resolves
//! against us, so a system that holds up here should do no worse live under IBKR's algos.
//!
//! `translate` returns exactly the Sierra orders the backtest relay must place -- the C++ relay
//! study places them verbatim and decides nothing -- plus, per leg, the instructions the
//! backtest-only report overlay applies to that leg's broker-reported fills (`Overlay`). Sierra's
//! own fills stay the source of truth (D2 "Results"); the overlay only produces the separate
//! "pessimistic P&L" view.
//!
//! The mapping (D2 table):
//! - Adaptive Patient / Normal -> `LIMIT` at the passive touch (buy at bid, sell at ask), or at
//!   the caller's limit if that is more passive; filled only on trade-through. (MES's one-tick
//!   spread has no on-grid midpoint, so Normal's price improvement is not modelled.)
//! - Adaptive Urgent, plain market entries, flattens -> `MARKET`, overlay +1 tick adverse.
//! - Plain limit -> `LIMIT` at the caller's price (rounded against us), trade-through.
//! - Bracket target -> attached `LIMIT`, trade-through; bracket stop -> attached
//!   `STOP_WITH_LAST_TRIGGERING`, overlay +1 tick adverse.
//! - TWAP / VWAP / PctVol / DarkIce -> `NotModelled`: slicing and size-hiding algos have nothing
//!   to do at 1 to a few MES contracts; emulation as spaced child limits is deferred (D2).
//!
//! Amends (`amend`) re-derive the leg exactly as a new leg at the amended price would be built --
//! same rounding, same trade-through placement and overlay -- so an amended order is no less
//! pessimistic than a fresh one.
//!
//! Trade-through: on a tick grid a buy limit at P is traded through exactly when a trade prints
//! at <= P - 1 tick, which is exactly when a touch-filled buy limit at P - 1 tick fills. So under
//! `LimitFillSemantics::Touch` the limit is placed one tick better and the overlay books it at P;
//! under `TradeThrough` it is placed at P unchanged (offsetting it too would double-count).

use crate::broker::{AdaptivePriority, Amend, BrokerError, Entry, ExecutionStyle, OrderRequest, Side};
use crate::position_state::{ContractSpec, OrderRole};

/// How Sierra's trade simulator fills resting limits in replay -- to be measured by D2's spike,
/// never assumed, so there is deliberately no default.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum LimitFillSemantics {
    /// Fills when a trade prints at the limit price.
    Touch,
    /// Fills only when a trade prints beyond the limit price.
    TradeThrough,
}

/// Best bid / ask at decision time.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Quote {
    pub bid: f64,
    pub ask: f64,
}

/// The Sierra order types this mapping uses, with their `SCT_ORDERTYPE_*` ids from the vendored
/// `sierra_chart_dependencies/scconstants.h`.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SierraOrderType {
    Market,
    Limit,
    StopWithLastTriggering,
}

impl SierraOrderType {
    pub fn sct_order_type(self) -> i32 {
        match self {
            Self::Market => 0,                  // SCT_ORDERTYPE_MARKET
            Self::Limit => 1,                   // SCT_ORDERTYPE_LIMIT
            Self::StopWithLastTriggering => 25, // SCT_ORDERTYPE_STOP_WITH_LAST_TRIGGERING
        }
    }
}

/// How the pessimistic report overlay treats this leg's broker-reported fills.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Overlay {
    /// Book the fill at this price instead of the broker's (the trade-through emulation).
    pub book_at: Option<f64>,
    /// Then move the booked price this many ticks against us.
    pub adverse_ticks: u32,
}

impl Overlay {
    const NONE: Self = Self { book_at: None, adverse_ticks: 0 };
    const SLIP_ONE: Self = Self { book_at: None, adverse_ticks: 1 };
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SierraLeg {
    pub order_type: SierraOrderType,
    pub side: Side,
    pub quantity: i64,
    /// Limit price or stop trigger; `None` for market orders.
    pub price: Option<f64>,
    pub overlay: Overlay,
}

/// The parent order plus its attached OCO bracket legs, if any.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct SierraPlan {
    pub parent: SierraLeg,
    pub target: Option<SierraLeg>,
    pub stop: Option<SierraLeg>,
}

/// Translate one broker-neutral request into the Sierra orders the replay relay places.
pub fn translate(req: &OrderRequest, quote: Quote, spec: ContractSpec, fills: LimitFillSemantics) -> Result<SierraPlan, BrokerError> {
    req.validate()?;
    let tick = spec.tick_size();
    if !(quote.bid.is_finite() && quote.ask.is_finite() && quote.bid > 0.0 && quote.bid < quote.ask) {
        return Err(BrokerError::InvalidRequest("quote must have 0 < bid < ask".into()));
    }

    let market = |side| SierraLeg { order_type: SierraOrderType::Market, side, quantity: req.quantity, price: None, overlay: Overlay::SLIP_ONE };
    let parent = match (req.role, req.style, req.entry) {
        (OrderRole::Flatten, _, _) => market(req.side),
        (_, ExecutionStyle::Twap | ExecutionStyle::Vwap { .. } | ExecutionStyle::PctVol { .. } | ExecutionStyle::DarkIce { .. }, _) => {
            return Err(BrokerError::NotModelled(
                "slicing/size-hiding algos are not modelled in the Sierra replay at this size (spec §8.7 D2)".into(),
            ));
        }
        (_, ExecutionStyle::Adaptive(AdaptivePriority::Urgent), _) | (_, ExecutionStyle::Direct, Entry::Market) => market(req.side),
        (_, ExecutionStyle::Adaptive(_), entry) => {
            let touch = match req.side {
                Side::Buy => quote.bid,
                Side::Sell => quote.ask,
            };
            let price = match entry {
                Entry::Market => touch,
                Entry::Limit { price } => more_passive(req.side, round_against(req.side, price, tick), touch),
            };
            limit_leg(req.side, req.quantity, price, tick, fills)
        }
        (_, ExecutionStyle::Direct, Entry::Limit { price }) => limit_leg(req.side, req.quantity, round_against(req.side, price, tick), tick, fills),
    };

    let (target, stop) = match req.bracket {
        None => (None, None),
        Some(b) => {
            let exit = req.side.opposite();
            let target = limit_leg(exit, req.quantity, round_against(exit, b.target, tick), tick, fills);
            let stop = SierraLeg {
                order_type: SierraOrderType::StopWithLastTriggering,
                side: exit,
                quantity: req.quantity,
                price: Some(round_stop_late(exit, b.stop, tick)),
                overlay: Overlay::SLIP_ONE,
            };
            (Some(target), Some(stop))
        }
    };
    Ok(SierraPlan { parent, target, stop })
}

/// The leg the relay re-sends through Sierra's `ModifyOrder`: `working` with the amend applied.
pub fn amend(working: &SierraLeg, change: &Amend, spec: ContractSpec, fills: LimitFillSemantics) -> Result<SierraLeg, BrokerError> {
    change.validate()?;
    let tick = spec.tick_size();
    let quantity = change.quantity.unwrap_or(working.quantity);
    let Some(p) = change.price else {
        return Ok(SierraLeg { quantity, ..*working });
    };
    match working.order_type {
        SierraOrderType::Limit => Ok(limit_leg(working.side, quantity, round_against(working.side, p, tick), tick, fills)),
        SierraOrderType::StopWithLastTriggering => Ok(SierraLeg { quantity, price: Some(round_stop_late(working.side, p, tick)), ..*working }),
        SierraOrderType::Market => Err(BrokerError::InvalidRequest("a market order has no price to amend".into())),
    }
}

/// A resting limit filled only on trade-through (see module doc).
fn limit_leg(side: Side, quantity: i64, price: f64, tick: f64, fills: LimitFillSemantics) -> SierraLeg {
    let (placed, overlay) = match fills {
        LimitFillSemantics::TradeThrough => (price, Overlay::NONE),
        LimitFillSemantics::Touch => (price - side.sign() as f64 * tick, Overlay { book_at: Some(price), adverse_ticks: 0 }),
    };
    SierraLeg { order_type: SierraOrderType::Limit, side, quantity, price: Some(placed), overlay }
}

/// Snap a limit price to the grid in the direction that is worse for us (buy down, sell up).
pub(crate) fn round_against(side: Side, price: f64, tick: f64) -> f64 {
    let steps = price / tick;
    let snapped = match side {
        Side::Buy => (steps + 1e-9).floor(),
        Side::Sell => (steps - 1e-9).ceil(),
    };
    snapped * tick
}

/// Snap a stop trigger so it triggers later (a sell stop lower, a buy stop higher). Note this is
/// the OPPOSITE rounding direction from a limit on the same side: a lower sell limit is better
/// for us, but a lower sell stop is worse (it triggers later, after a bigger loss).
pub(crate) fn round_stop_late(side: Side, price: f64, tick: f64) -> f64 {
    round_against(side.opposite(), price, tick)
}

/// The less aggressive of two limit prices for `side` (lower for a buy, higher for a sell).
fn more_passive(side: Side, a: f64, b: f64) -> f64 {
    match side {
        Side::Buy => a.min(b),
        Side::Sell => a.max(b),
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::broker::Bracket;

    const Q: Quote = Quote { bid: 7858.00, ask: 7858.25 };

    fn mes() -> ContractSpec {
        ContractSpec::new(0.25, 5.0).expect("valid")
    }

    fn req(side: Side, entry: Entry, style: ExecutionStyle) -> OrderRequest {
        OrderRequest { role: OrderRole::Entry, side, quantity: 1, entry, style, bracket: None, decision_id: Some(9) }
    }

    fn plan(r: &OrderRequest, f: LimitFillSemantics) -> SierraPlan {
        translate(r, Q, mes(), f).expect("translates")
    }

    fn close(a: Option<f64>, b: f64) -> bool {
        a.is_some_and(|a| (a - b).abs() < 1e-9)
    }

    #[test]
    fn adaptive_patient_and_normal_rest_at_the_passive_touch() {
        for p in [AdaptivePriority::Patient, AdaptivePriority::Normal] {
            let buy = plan(&req(Side::Buy, Entry::Market, ExecutionStyle::Adaptive(p)), LimitFillSemantics::TradeThrough);
            assert_eq!(buy.parent.order_type, SierraOrderType::Limit);
            assert!(close(buy.parent.price, 7858.00), "buy rests at the bid");
            let sell = plan(&req(Side::Sell, Entry::Market, ExecutionStyle::Adaptive(p)), LimitFillSemantics::TradeThrough);
            assert!(close(sell.parent.price, 7858.25), "sell rests at the ask");
            assert_eq!(sell.parent.overlay, Overlay::NONE);
        }
    }

    #[test]
    fn touch_semantics_place_one_tick_better_and_book_at_the_intended_price() {
        let buy = plan(&req(Side::Buy, Entry::Market, ExecutionStyle::Adaptive(AdaptivePriority::Patient)), LimitFillSemantics::Touch);
        assert!(close(buy.parent.price, 7857.75));
        assert!(close(buy.parent.overlay.book_at, 7858.00));
        let sell = plan(&req(Side::Sell, Entry::Market, ExecutionStyle::Adaptive(AdaptivePriority::Patient)), LimitFillSemantics::Touch);
        assert!(close(sell.parent.price, 7858.50));
        assert!(close(sell.parent.overlay.book_at, 7858.25));
    }

    #[test]
    fn an_adaptive_limit_uses_the_more_passive_of_limit_and_touch() {
        let aggressive = req(Side::Buy, Entry::Limit { price: 7860.0 }, ExecutionStyle::Adaptive(AdaptivePriority::Normal));
        assert!(close(plan(&aggressive, LimitFillSemantics::TradeThrough).parent.price, 7858.00), "capped at the bid");
        let passive = req(Side::Buy, Entry::Limit { price: 7855.0 }, ExecutionStyle::Adaptive(AdaptivePriority::Normal));
        assert!(close(plan(&passive, LimitFillSemantics::TradeThrough).parent.price, 7855.00), "the caller's lower limit wins");
    }

    #[test]
    fn urgent_market_and_flatten_go_to_market_with_one_tick_slippage() {
        for r in [
            req(Side::Buy, Entry::Limit { price: 7859.0 }, ExecutionStyle::Adaptive(AdaptivePriority::Urgent)),
            req(Side::Sell, Entry::Market, ExecutionStyle::Direct),
            OrderRequest { role: OrderRole::Flatten, ..req(Side::Sell, Entry::Limit { price: 7858.0 }, ExecutionStyle::Adaptive(AdaptivePriority::Patient)) },
        ] {
            let p = plan(&r, LimitFillSemantics::Touch);
            assert_eq!((p.parent.order_type, p.parent.price), (SierraOrderType::Market, None));
            assert_eq!(p.parent.overlay, Overlay { book_at: None, adverse_ticks: 1 });
        }
    }

    #[test]
    fn plain_limits_round_against_us() {
        let buy = req(Side::Buy, Entry::Limit { price: 7858.10 }, ExecutionStyle::Direct);
        assert!(close(plan(&buy, LimitFillSemantics::TradeThrough).parent.price, 7858.00));
        let sell = req(Side::Sell, Entry::Limit { price: 7858.10 }, ExecutionStyle::Direct);
        assert!(close(plan(&sell, LimitFillSemantics::TradeThrough).parent.price, 7858.25));
        let on_grid = req(Side::Buy, Entry::Limit { price: 7858.25 }, ExecutionStyle::Direct);
        assert!(close(plan(&on_grid, LimitFillSemantics::TradeThrough).parent.price, 7858.25), "on-grid prices are untouched");
    }

    #[test]
    fn long_bracket_target_trades_through_and_stop_slips() {
        let r = OrderRequest {
            bracket: Some(Bracket { target: 7862.10, stop: 7854.10 }),
            ..req(Side::Buy, Entry::Market, ExecutionStyle::Adaptive(AdaptivePriority::Normal))
        };
        let p = plan(&r, LimitFillSemantics::Touch);
        let t = p.target.expect("target leg");
        assert_eq!((t.order_type, t.side, t.quantity), (SierraOrderType::Limit, Side::Sell, 1));
        assert!(close(Some(t.price.unwrap_or(f64::NAN)), 7862.50), "sell target rounds up to 7862.25, then +1 tick for touch");
        assert!(close(t.overlay.book_at, 7862.25));
        let s = p.stop.expect("stop leg");
        assert_eq!((s.order_type, s.side), (SierraOrderType::StopWithLastTriggering, Side::Sell));
        assert!(close(s.price, 7854.00), "sell stop rounds down: triggers later");
        assert_eq!(s.overlay, Overlay { book_at: None, adverse_ticks: 1 });
    }

    #[test]
    fn short_bracket_mirrors_the_long_one() {
        let r = OrderRequest {
            bracket: Some(Bracket { target: 7854.10, stop: 7862.10 }),
            ..req(Side::Sell, Entry::Market, ExecutionStyle::Direct)
        };
        let p = plan(&r, LimitFillSemantics::TradeThrough);
        let (t, s) = (p.target.expect("target"), p.stop.expect("stop"));
        assert_eq!((t.side, s.side), (Side::Buy, Side::Buy));
        assert!(close(t.price, 7854.00), "buy target rounds down");
        assert!(close(s.price, 7862.25), "buy stop rounds up: triggers later");
    }

    #[test]
    fn amended_legs_are_as_pessimistic_as_new_ones() {
        let r = OrderRequest {
            bracket: Some(Bracket { target: 7862.0, stop: 7854.0 }),
            ..req(Side::Buy, Entry::Market, ExecutionStyle::Adaptive(AdaptivePriority::Patient))
        };
        let p = plan(&r, LimitFillSemantics::Touch);
        let a = |price, quantity| Amend { order_id: 1, quantity, price };

        let t = amend(&p.target.expect("target"), &a(Some(7863.10), None), mes(), LimitFillSemantics::Touch).expect("ok");
        assert!(close(t.price, 7863.50), "sell target rounds up to 7863.25, then +1 tick for touch");
        assert!(close(t.overlay.book_at, 7863.25));

        let s = amend(&p.stop.expect("stop"), &a(Some(7855.10), Some(2)), mes(), LimitFillSemantics::Touch).expect("ok");
        assert!(close(s.price, 7855.00), "sell stop rounds down: triggers later");
        assert_eq!((s.quantity, s.overlay), (2, Overlay::SLIP_ONE));

        let parent = amend(&p.parent, &a(None, Some(3)), mes(), LimitFillSemantics::Touch).expect("qty only");
        assert_eq!((parent.quantity, parent.price), (3, p.parent.price), "quantity-only keeps the placed price");

        let mkt = plan(&req(Side::Buy, Entry::Market, ExecutionStyle::Direct), LimitFillSemantics::Touch).parent;
        assert!(matches!(amend(&mkt, &a(Some(7858.0), None), mes(), LimitFillSemantics::Touch), Err(BrokerError::InvalidRequest(_))));
    }

    #[test]
    fn slicing_algos_are_not_modelled() {
        for style in [
            ExecutionStyle::Twap,
            ExecutionStyle::Vwap { max_pct_vol: 0.1 },
            ExecutionStyle::PctVol { pct_vol: 0.1 },
            ExecutionStyle::DarkIce { display_size: 1 },
        ] {
            let r = req(Side::Buy, Entry::Limit { price: 7858.0 }, style);
            assert!(matches!(translate(&r, Q, mes(), LimitFillSemantics::Touch), Err(BrokerError::NotModelled(_))));
        }
    }

    #[test]
    fn bad_quotes_and_bad_requests_are_refused() {
        let r = req(Side::Buy, Entry::Market, ExecutionStyle::Direct);
        for q in [Quote { bid: 7858.25, ask: 7858.0 }, Quote { bid: f64::NAN, ask: 1.0 }, Quote { bid: 0.0, ask: 1.0 }] {
            assert!(matches!(translate(&r, q, mes(), LimitFillSemantics::Touch), Err(BrokerError::InvalidRequest(_))));
        }
        let zero = OrderRequest { quantity: 0, ..r };
        assert!(matches!(translate(&zero, Q, mes(), LimitFillSemantics::Touch), Err(BrokerError::InvalidRequest(_))));
    }

    #[test]
    fn sct_ids_match_the_vendored_header() {
        // sierra_chart_dependencies/scconstants.h:1281/1282/1305
        assert_eq!(SierraOrderType::Market.sct_order_type(), 0);
        assert_eq!(SierraOrderType::Limit.sct_order_type(), 1);
        assert_eq!(SierraOrderType::StopWithLastTriggering.sct_order_type(), 25);
    }
}
