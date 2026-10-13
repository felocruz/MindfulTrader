//! The live counterpart of `sierra_sim`: a broker-neutral `OrderRequest` -> the IBKR orders to
//! send, as plain data (no `ibapi` types -- this crate's core stays broker-free; the future
//! `ibkr_session` copies these fields onto `ibapi::orders::Order`).
//!
//! Evidence-bound (spec §8.7 D3, W0e 2026-10-13, paper account, MES futures): only combinations
//! IBKR actually accepted are produced. Adaptive with LMT or MKT; TWAP, VWAP, PctVol and DarkIce
//! with LMT only (W0e never asked about them with MKT); TWAP without `strategyType` (that
//! attribute was rejected `[443]` on MES). Anything else is `NotModelled` rather than a guess.
//! Emergency flattens are always a plain market order -- no algo between us and flat.
//!
//! Brackets follow D3a: parent (not transmitted), target `LMT` child (not transmitted), stop `STP`
//! child (transmitted -- releasing the whole tree atomically). Prices are snapped to the tick
//! grid with `sierra_sim`'s exact rules (limits against us, stops triggering later), so live and
//! replay round identically.

use crate::broker::{BrokerError, Entry, ExecutionStyle, OrderRequest, Side};
use crate::position_state::{ContractSpec, OrderRole};
use crate::sierra_sim::{round_against, round_stop_late};

/// IBKR `algoParams` as (tag, value) pairs.
pub type AlgoParams = Vec<(&'static str, String)>;

#[derive(Debug, Clone, PartialEq)]
pub struct IbkrOrder {
    pub action: Side,
    pub quantity: i64,
    /// IBKR order type: "MKT", "LMT" or "STP".
    pub order_type: &'static str,
    pub limit_price: Option<f64>,
    /// Stop trigger for "STP".
    pub aux_price: Option<f64>,
    /// IBKR `algoStrategy`; empty for no algo.
    pub algo_strategy: &'static str,
    pub algo_params: AlgoParams,
    /// Index of the parent within the plan, for bracket children.
    pub parent_index: Option<usize>,
    pub transmit: bool,
}

/// Parent first, then target, then stop.
#[derive(Debug, Clone, PartialEq)]
pub struct IbkrPlan {
    pub orders: Vec<IbkrOrder>,
}

pub fn translate(req: &OrderRequest, spec: ContractSpec) -> Result<IbkrPlan, BrokerError> {
    req.validate()?;
    let tick = spec.tick_size();
    let has_bracket = req.bracket.is_some();

    let (order_type, limit_price) = match req.entry {
        Entry::Market => ("MKT", None),
        Entry::Limit { price } => ("LMT", Some(round_against(req.side, price, tick))),
    };
    let (algo_strategy, algo_params) = if req.role == OrderRole::Flatten {
        ("", Vec::new())
    } else {
        algo(req.style, req.entry)?
    };
    let (order_type, limit_price) = if req.role == OrderRole::Flatten { ("MKT", None) } else { (order_type, limit_price) };

    let mut orders = vec![IbkrOrder {
        action: req.side,
        quantity: req.quantity,
        order_type,
        limit_price,
        aux_price: None,
        algo_strategy,
        algo_params,
        parent_index: None,
        transmit: !has_bracket,
    }];
    if let Some(b) = req.bracket {
        let exit = req.side.opposite();
        let child = |order_type, limit_price, aux_price, transmit| IbkrOrder {
            action: exit,
            quantity: req.quantity,
            order_type,
            limit_price,
            aux_price,
            algo_strategy: "",
            algo_params: Vec::new(),
            parent_index: Some(0),
            transmit,
        };
        orders.push(child("LMT", Some(round_against(exit, b.target, tick)), None, false));
        orders.push(child("STP", None, Some(round_stop_late(exit, b.stop, tick)), true));
    }
    Ok(IbkrPlan { orders })
}

fn algo(style: ExecutionStyle, entry: Entry) -> Result<(&'static str, AlgoParams), BrokerError> {
    let limit_only = |name: &str| -> Result<(), BrokerError> {
        match entry {
            Entry::Limit { .. } => Ok(()),
            Entry::Market => Err(BrokerError::NotModelled(format!("{name} with a market entry was not confirmed on MES by W0e (spec §8.7 D3)"))),
        }
    };
    let pct = |name: &str, v: f64| -> Result<String, BrokerError> {
        if v > 0.0 && v <= 0.5 {
            Ok(v.to_string())
        } else {
            Err(BrokerError::InvalidRequest(format!("{name} must be in (0, 0.5] (IBKR's 1-50% range)")))
        }
    };
    Ok(match style {
        ExecutionStyle::Direct => ("", Vec::new()),
        ExecutionStyle::Adaptive(p) => ("Adaptive", vec![("adaptivePriority", p.ibkr_name().to_string())]),
        ExecutionStyle::Twap => {
            limit_only("TWAP")?;
            ("Twap", vec![("allowPastEndTime", "1".into())])
        }
        ExecutionStyle::Vwap { max_pct_vol } => {
            limit_only("VWAP")?;
            ("Vwap", vec![("maxPctVol", pct("maxPctVol", max_pct_vol)?), ("noTakeLiq", "0".into()), ("allowPastEndTime", "1".into())])
        }
        ExecutionStyle::PctVol { pct_vol } => {
            limit_only("PctVol")?;
            ("PctVol", vec![("pctVol", pct("pctVol", pct_vol)?), ("noTakeLiq", "0".into())])
        }
        ExecutionStyle::DarkIce { display_size } => {
            limit_only("DarkIce")?;
            if display_size == 0 {
                return Err(BrokerError::InvalidRequest("DarkIce display size must be at least 1".into()));
            }
            ("DarkIce", vec![("displaySize", display_size.to_string()), ("allowPastEndTime", "1".into())])
        }
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::broker::{AdaptivePriority, Bracket};

    fn mes() -> ContractSpec {
        ContractSpec::new(0.25, 5.0).expect("valid")
    }

    fn req(side: Side, entry: Entry, style: ExecutionStyle) -> OrderRequest {
        OrderRequest { role: OrderRole::Entry, side, quantity: 1, entry, style, bracket: None, decision_id: None }
    }

    fn params(o: &IbkrOrder) -> Vec<(&str, &str)> {
        o.algo_params.iter().map(|(k, v)| (*k, v.as_str())).collect()
    }

    #[test]
    fn w0e_accepted_combinations_translate_exactly() {
        let lmt = Entry::Limit { price: 7808.0 };
        type Case<'a> = (ExecutionStyle, Entry, &'a str, &'a str, Vec<(&'a str, &'a str)>);
        let cases: [Case; 6] = [
            (ExecutionStyle::Adaptive(AdaptivePriority::Urgent), lmt, "LMT", "Adaptive", vec![("adaptivePriority", "Urgent")]),
            (ExecutionStyle::Adaptive(AdaptivePriority::Normal), Entry::Market, "MKT", "Adaptive", vec![("adaptivePriority", "Normal")]),
            (ExecutionStyle::Twap, lmt, "LMT", "Twap", vec![("allowPastEndTime", "1")]),
            (ExecutionStyle::Vwap { max_pct_vol: 0.1 }, lmt, "LMT", "Vwap", vec![("maxPctVol", "0.1"), ("noTakeLiq", "0"), ("allowPastEndTime", "1")]),
            (ExecutionStyle::PctVol { pct_vol: 0.1 }, lmt, "LMT", "PctVol", vec![("pctVol", "0.1"), ("noTakeLiq", "0")]),
            (ExecutionStyle::DarkIce { display_size: 1 }, lmt, "LMT", "DarkIce", vec![("displaySize", "1"), ("allowPastEndTime", "1")]),
        ];
        for (style, entry, order_type, strategy, expected) in cases {
            let plan = translate(&req(Side::Buy, entry, style), mes()).expect("translates");
            let o = &plan.orders[0];
            assert_eq!((o.order_type, o.algo_strategy, o.transmit), (order_type, strategy, true));
            assert_eq!(params(o), expected);
        }
    }

    #[test]
    fn rejected_strategies_cannot_be_expressed() {
        // BalanceImpactRisk / ArrivalPx were [439] on MES: ExecutionStyle has no variant for them,
        // and TWAP never carries the [443] `strategyType` attribute.
        let plan = translate(&req(Side::Buy, Entry::Limit { price: 1.0 }, ExecutionStyle::Twap), mes()).expect("ok");
        assert!(plan.orders[0].algo_params.iter().all(|(k, _)| *k != "strategyType"));
    }

    #[test]
    fn unconfirmed_market_slicers_are_not_modelled() {
        for style in [ExecutionStyle::Twap, ExecutionStyle::Vwap { max_pct_vol: 0.1 }, ExecutionStyle::PctVol { pct_vol: 0.1 }, ExecutionStyle::DarkIce { display_size: 1 }] {
            assert!(matches!(translate(&req(Side::Buy, Entry::Market, style), mes()), Err(BrokerError::NotModelled(_))));
        }
    }

    #[test]
    fn invalid_algo_parameters_are_refused() {
        let lmt = Entry::Limit { price: 1.0 };
        assert!(matches!(translate(&req(Side::Buy, lmt, ExecutionStyle::PctVol { pct_vol: 0.0 }), mes()), Err(BrokerError::InvalidRequest(_))));
        assert!(matches!(translate(&req(Side::Buy, lmt, ExecutionStyle::Vwap { max_pct_vol: 0.9 }), mes()), Err(BrokerError::InvalidRequest(_))));
        assert!(matches!(translate(&req(Side::Buy, lmt, ExecutionStyle::DarkIce { display_size: 0 }), mes()), Err(BrokerError::InvalidRequest(_))));
    }

    #[test]
    fn flatten_is_always_plain_market() {
        let r = OrderRequest { role: OrderRole::Flatten, ..req(Side::Sell, Entry::Limit { price: 7858.0 }, ExecutionStyle::Adaptive(AdaptivePriority::Patient)) };
        let o = &translate(&r, mes()).expect("ok").orders[0];
        assert_eq!((o.order_type, o.limit_price, o.algo_strategy), ("MKT", None, ""));
    }

    #[test]
    fn bracket_is_an_atomic_three_order_tree() {
        let r = OrderRequest {
            bracket: Some(Bracket { target: 7862.10, stop: 7854.10 }),
            ..req(Side::Buy, Entry::Limit { price: 7858.0 }, ExecutionStyle::Adaptive(AdaptivePriority::Normal))
        };
        let plan = translate(&r, mes()).expect("ok");
        let [parent, target, stop] = plan.orders.as_slice() else { panic!("three orders expected") };
        assert_eq!((parent.transmit, target.transmit, stop.transmit), (false, false, true), "only the last child transmits");
        assert_eq!((target.parent_index, stop.parent_index), (Some(0), Some(0)));
        assert_eq!((target.action, target.order_type, stop.action, stop.order_type), (Side::Sell, "LMT", Side::Sell, "STP"));
        assert!(target.limit_price.is_some_and(|p| (p - 7862.25).abs() < 1e-9), "sell target rounds up");
        assert!(stop.aux_price.is_some_and(|p| (p - 7854.0).abs() < 1e-9), "sell stop rounds down: triggers later");
        assert!(target.algo_strategy.is_empty() && stop.algo_strategy.is_empty(), "children are plain orders (D3a)");
    }
}
