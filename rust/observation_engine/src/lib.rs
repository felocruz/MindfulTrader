//! observation_engine -- the one observation pipeline, live and offline (rust-migration spec §6.2,
//! decision #43).
//!
//! Trades go in as borrowed column views -- live, the records the `rust/ffi` adapter reads from
//! Sierra Chart's intraday file (spec §6.2.8 D2); offline, the tick file -- and the engine builds
//! every clock itself with the same code (D3): time bars, activity bars and the regime model's steps
//! (D4). Training data and live observations therefore come from the same code.
//!
//! Built so far (the foundation, independent of the open operator decisions D2-D6):
//! - [`clock`]: event time, timeframes, the CME session calendar (18:00 ET anchor, US DST);
//! - [`input`]: `Trade`, `TradeColumns` (with the stream sequence), `BarColumns`, `Bar`;
//! - [`ring`]: `WindowRing` (the last n values always one contiguous slice), `SortedWindow`
//!   (incremental rolling median/MAD);
//! - [`bars`]: session-anchored time bars from trades; [`activity`]: imbalance bars;
//! - [`engine`]: ingestion in sequence order, every clock built from trades, the step trigger,
//!   monotonic time, per-clock rolling stores;
//! - [`status`]: `DimStatus` instead of sentinel values.
//!
//! Next, after D2-D6: the dims (B12-B24), the frame, the scaler and the gate.

pub mod activity;
pub mod bars;
pub mod clock;
pub mod engine;
pub mod input;
pub mod ring;
pub mod status;
