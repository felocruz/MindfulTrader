//! observation_engine -- the one observation pipeline, live and offline (rust-migration spec §6.2,
//! decision #43).
//!
//! Trades and bars go in as borrowed column views; the engine owns every window and all state; the
//! per-dim formulas come from `observation_vector`. Live, the `rust/ffi` adapter passes Sierra
//! Chart's own arrays; offline, [`offline::OfflineFeeder`] builds the same bar views from a trade
//! stream and feeds the engine in the same event order -- one engine, two feeders, so training data
//! and live observations come from the same code.
//!
//! Built so far (the foundation, independent of the open operator decisions D2-D6):
//! - [`clock`]: event time, timeframes, the CME session calendar (18:00 ET anchor, US DST);
//! - [`input`]: `Trade`, `TradeColumns`, `BarColumns` (Sierra's layout), `Bar`;
//! - [`ring`]: `WindowRing` (the last n values always one contiguous slice), `SortedWindow`
//!   (incremental rolling median/MAD);
//! - [`bars`]: session-anchored time bars from trades; [`activity`]: imbalance bars;
//! - [`engine`]: ingestion, event order, monotonic time, per-clock rolling stores;
//! - [`offline`]: the feeder that turns a trade stream into engine events;
//! - [`status`]: `DimStatus` instead of sentinel values.
//!
//! Next, after D2-D6: the dims (B12-B24), the frame, the scaler and the gate.

pub mod activity;
pub mod bars;
pub mod clock;
pub mod engine;
pub mod input;
pub mod offline;
pub mod ring;
pub mod status;
