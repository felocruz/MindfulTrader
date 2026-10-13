//! W0e -- MES-futures IBKR algo what-if spike (spec §8.7 D3, §8.8; plan W0e).
//!
//! READ-ONLY. Asks this account's TWS which order types / IB Algos it would accept for one MES
//! futures contract, WITHOUT placing anything: every order is a what-if (IBKR checks product and
//! algo support, commission and margin, then answers; nothing reaches the exchange). Three guards,
//! each before the next network step:
//!   1. the logged-in account must be exactly one PAPER account (`DU`/`DF`) -- checked before any
//!      contract or order request (Atratus `is_paper_account` precedent);
//!   2. every order is built with `what_if = true`, and `ask_what_if` refuses one without it;
//!   3. nothing is ever modified or cancelled -- what-if orders never rest anywhere.
//!
//! Also answers W0e's secondary question empirically: does `ibapi` 5.0.0's stock 1s read timeout
//! survive WSL2 -> Windows TWS (Atratus had to vendor a 1s -> 20s patch on 4.2.0)?
//!
//! Usage (paper TWS, API enabled, 127.0.0.1 trusted):
//!   cargo run -p execution --example w0e_mes_algo_whatif -- [ADDR] [CLIENT_ID] [REF_PRICE]
//! Defaults: 127.0.0.1:7497 (paper TWS), client id 160. REF_PRICE overrides the historical lookup.

use std::time::{Duration, Instant};

use ibapi::client::blocking::Client;
use ibapi::contracts::{ContractDetails, TagValue};
use ibapi::orders::Order;
use ibapi::prelude::*;

const WHAT_IF_WAIT: Duration = Duration::from_secs(10);
/// Buy limit placed this many points under the reference price -- a resting order for sizing,
/// far enough below market that even a mistakenly transmitted order could not fill at once.
const LIMIT_OFFSET_POINTS: f64 = 50.0;

type Result<T> = std::result::Result<T, Box<dyn std::error::Error>>;

fn is_paper_account(account: &str) -> bool {
    account.starts_with("DU") || account.starts_with("DF")
}

/// One order variant to ask IBKR about.
struct Variant {
    label: &'static str,
    order_type: &'static str,
    algo: &'static str,
    params: &'static [(&'static str, &'static str)],
}

/// Plain LMT/MKT baselines first (if these fail, an algo rejection says nothing about the algo),
/// then Adaptive (spec D3's first choice), then the documented alternates.
const VARIANTS: &[Variant] = &[
    Variant { label: "LMT (baseline)", order_type: "LMT", algo: "", params: &[] },
    Variant { label: "MKT (baseline)", order_type: "MKT", algo: "", params: &[] },
    Variant { label: "LMT + Adaptive Urgent", order_type: "LMT", algo: "Adaptive", params: &[("adaptivePriority", "Urgent")] },
    Variant { label: "LMT + Adaptive Normal", order_type: "LMT", algo: "Adaptive", params: &[("adaptivePriority", "Normal")] },
    Variant { label: "LMT + Adaptive Patient", order_type: "LMT", algo: "Adaptive", params: &[("adaptivePriority", "Patient")] },
    Variant { label: "MKT + Adaptive Normal", order_type: "MKT", algo: "Adaptive", params: &[("adaptivePriority", "Normal")] },
    Variant {
        label: "LMT + TWAP (Marketable)",
        order_type: "LMT",
        algo: "Twap",
        params: &[("strategyType", "Marketable"), ("allowPastEndTime", "1")],
    },
    // The Marketable variant above is kept as the record of run 1's [443] (strategyType is not a
    // TWAP attribute on this product) -- these two separate "TWAP unsupported" from "bad params".
    Variant { label: "LMT + TWAP (no params)", order_type: "LMT", algo: "Twap", params: &[] },
    Variant { label: "LMT + TWAP (allowPastEndTime)", order_type: "LMT", algo: "Twap", params: &[("allowPastEndTime", "1")] },
    Variant {
        label: "LMT + VWAP (10% max vol)",
        order_type: "LMT",
        algo: "Vwap",
        params: &[("maxPctVol", "0.1"), ("noTakeLiq", "0"), ("allowPastEndTime", "1")],
    },
    Variant {
        label: "LMT + Balance Impact and Risk",
        order_type: "LMT",
        algo: "BalanceImpactRisk",
        params: &[("maxPctVol", "0.1"), ("riskAversion", "Neutral"), ("forceCompletion", "0")],
    },
    Variant {
        label: "LMT + Arrival Price",
        order_type: "LMT",
        algo: "ArrivalPx",
        params: &[("maxPctVol", "0.1"), ("riskAversion", "Neutral"), ("allowPastEndTime", "1"), ("forceCompletion", "0")],
    },
    // Round 2 (2026-10-13), after a teammate's research note claimed BalanceImpactRisk is
    // options-only and named Arrival Price / PctVol / Dark Ice as the futures alternatives:
    // Gemini's `ArrivalPrice` spelling (vs. the API docs' `ArrivalPx` above), plus the two new names.
    Variant {
        label: "LMT + ArrivalPrice (alt spelling)",
        order_type: "LMT",
        algo: "ArrivalPrice",
        params: &[("maxPctVol", "0.1"), ("riskAversion", "Neutral"), ("allowPastEndTime", "1"), ("forceCompletion", "0")],
    },
    Variant { label: "LMT + PctVol (10%)", order_type: "LMT", algo: "PctVol", params: &[("pctVol", "0.1"), ("noTakeLiq", "0")] },
    Variant { label: "LMT + DarkIce (display 1)", order_type: "LMT", algo: "DarkIce", params: &[("displaySize", "1"), ("allowPastEndTime", "1")] },
];

fn what_if_order(v: &Variant, account: &str, limit: f64) -> Order {
    let mut order = Order {
        action: Action::Buy,
        total_quantity: 1.0,
        order_type: v.order_type.to_string(),
        account: account.to_string(),
        what_if: true,
        ..Order::default()
    };
    if v.order_type == "LMT" {
        order.limit_price = Some(limit);
    }
    if !v.algo.is_empty() {
        order.algo_strategy = v.algo.to_string();
        order.algo_params = v.params.iter().map(|(tag, value)| TagValue { tag: (*tag).into(), value: (*value).into() }).collect();
    }
    order
}

fn main() -> Result<()> {
    let args: Vec<String> = std::env::args().collect();
    let addr = args.get(1).map_or("127.0.0.1:7497", String::as_str);
    let client_id: i32 = args.get(2).map_or(Ok(160), |s| s.parse())?;
    let ref_override: Option<f64> = args.get(3).map(|s| s.parse()).transpose()?;

    let started = Instant::now();
    let client = Client::connect(addr, client_id)?;
    println!("connected to {addr} as client {client_id} (server version {}) in {:?}", client.server_version(), started.elapsed());

    // Guard 1: exactly one paper account, before anything else touches the account.
    let accounts = client.managed_accounts()?;
    println!("managed accounts: {accounts:?}");
    let account = match accounts.as_slice() {
        [one] if is_paper_account(one) => one.clone(),
        [one] => return Err(format!("refusing: account {one:?} is not a paper account (DU/DF)").into()),
        _ => return Err(format!("refusing: expected exactly one managed account, got {}", accounts.len()).into()),
    };

    let contract = resolve_front_mes(&client)?;
    let ref_price = match ref_override {
        Some(p) => p,
        None => last_close(&client, &contract)?,
    };
    let tick = 0.25;
    let limit = ((ref_price - LIMIT_OFFSET_POINTS) / tick).round() * tick;
    println!("reference price {ref_price:.2} -> what-if BUY 1 limit {limit:.2} (market/algo variants ignore the limit where N/A)\n");

    let mut accepted = 0;
    for v in VARIANTS {
        let order = what_if_order(v, &account, limit);
        let line = ask_what_if(&client, &contract, &order)?;
        if line.starts_with("ACCEPTED") {
            accepted += 1;
        }
        println!("  {:32} {line}", v.label);
    }
    // The same note said to route algos to the `IBALGO` destination: ask once for ArrivalPx with
    // the contract's exchange set to IBALGO (everything above used the contract's own CME).
    if let Some(v) = VARIANTS.iter().find(|v| v.algo == "ArrivalPx") {
        let ibalgo = Contract { exchange: "IBALGO".into(), ..contract.clone() };
        let line = ask_what_if(&client, &ibalgo, &what_if_order(v, &account, limit))?;
        println!("  {:32} {line}", "LMT + ArrivalPx via IBALGO");
    }
    println!("\n{accepted}/{} variants accepted. Nothing was placed: every order was what-if.", VARIANTS.len());
    // Independent confirmation of guard 3: the account has no open orders afterwards.
    let open = client.all_open_orders()?;
    let mut resting = 0;
    while let Some(item) = open.next_timeout(Duration::from_secs(3)) {
        if let Ok(SubscriptionItem::Data(Orders::OrderData(d))) = item {
            resting += 1;
            println!("  OPEN ORDER FOUND: id {} {:?} {}", d.order_id, d.order.action, d.order.order_type);
        }
    }
    println!("open orders on the account after the run: {resting}");
    // Optional idle soak (W0E_SOAK_SECS): a 1-2s run cannot exercise the long-lived-connection
    // read-timeout failure Atratus hit on 4.2.0 over WSL2, so hold the session idle, then require
    // fresh round trips to still succeed.
    if let Some(secs) = std::env::var("W0E_SOAK_SECS").ok().and_then(|s| s.parse::<u64>().ok()) {
        println!("idle soak: {secs}s with no requests...");
        std::thread::sleep(Duration::from_secs(secs));
        let t = Instant::now();
        let accounts_after = client.managed_accounts()?;
        let details_after = client.contract_details(&contract)?;
        println!(
            "after soak: managed_accounts {accounts_after:?}, contract_details {} row(s), round trips took {:?}",
            details_after.len(),
            t.elapsed()
        );
    }
    println!("session stayed up {:?} on ibapi 5.0.0's stock 1s read timeout", started.elapsed());
    Ok(())
}

/// The listed MES future with the nearest expiry (by IBKR's own `real_expiration_date`, never a
/// calendar guess -- spec §8.4 mes_futures.rs: tick size and roll come from the exchange).
fn resolve_front_mes(client: &Client) -> Result<Contract> {
    let query = Contract::futures("MES").any_month().build();
    let mut details: Vec<ContractDetails> = client.contract_details(&query)?;
    details.sort_by(|a, b| a.real_expiration_date.cmp(&b.real_expiration_date));
    println!("MES listings (nearest first):");
    for d in details.iter().take(4) {
        println!(
            "  {} conId {} expires {} min_tick {} multiplier {}",
            d.contract.local_symbol, d.contract.contract_id, d.real_expiration_date, d.min_tick, d.contract.multiplier
        );
    }
    let front = details.into_iter().next().ok_or("no MES contracts returned")?;
    // Refer to it by conId + exchange only (Atratus `by_con_id` precedent): the details' own
    // `last_trade_date_or_contract_month` comes back in a form TWS rejects when echoed (10372).
    Ok(Contract {
        contract_id: front.contract.contract_id,
        security_type: SecurityType::Future,
        exchange: front.contract.exchange,
        ..Contract::default()
    })
}

fn last_close(client: &Client, contract: &Contract) -> Result<f64> {
    let data = client
        .historical_data(contract, HistoricalBarSize::Day)
        .what_to_show(HistoricalWhatToShow::Trades)
        .duration(5.days())
        .fetch()?;
    let bar = data.bars.last().ok_or("no historical bars -- pass REF_PRICE as the third argument")?;
    Ok(bar.close)
}

/// Sends one what-if order and returns IBKR's verdict. Guard 2 lives here.
fn ask_what_if(client: &Client, contract: &Contract, order: &Order) -> Result<String> {
    if !order.what_if {
        return Err("internal error: an order without the what-if flag -- refusing to send it".into());
    }
    let sub = client.place_order(client.next_order_id(), contract, order)?;
    let deadline = Instant::now() + WHAT_IF_WAIT;
    loop {
        let Some(item) = sub.next_timeout(deadline.saturating_duration_since(Instant::now())) else {
            return Ok(format!("no answer within {}s", WHAT_IF_WAIT.as_secs()));
        };
        match item {
            Ok(SubscriptionItem::Data(PlaceOrder::OpenOrder(d))) => {
                let st = d.order_state;
                let money = |v: Option<f64>| v.filter(|x| x.is_finite() && *x < 1e300).map_or("-".into(), |x| format!("${x:.2}"));
                let warn = if st.warning_text.is_empty() { String::new() } else { format!(" | warning: {}", st.warning_text) };
                return Ok(format!(
                    "ACCEPTED  commission {} | init margin change {}{warn}",
                    money(st.commission),
                    money(st.initial_margin_change)
                ));
            }
            Ok(SubscriptionItem::Notice(n)) if (2100..2200).contains(&n.code) => {} // farm/info notices
            Ok(SubscriptionItem::Notice(n)) => return Ok(format!("rejected  {}: {}", n.code, n.message)),
            Ok(_) => {}
            Err(e) => return Ok(format!("rejected  {e}")),
        }
    }
}
