//! execution -- the execution layer (spec
//! docs/superpowers/specs/2026-10-09-mindfultrader-rust-migration-spec.md §8).
//! Pure business-logic modules live here with zero Sierra/zmq/pyo3/ibapi dependency, same
//! "pure core, thin edges" discipline as observation_vector. Broker-connectivity modules
//! (ibkr_session, order_watcher, signal_feed, intent_client, position_feed) are added later and
//! carry their own dependencies (ibapi, tokio, transport) -- never pulled into this core.

pub mod order_gate;
pub mod stop_manager;
pub mod trap_exit;
