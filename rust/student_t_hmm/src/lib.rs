//! student_t_hmm -- the Student-t HMM core (spec
//! docs/superpowers/specs/2026-10-09-mindfultrader-rust-migration-spec.md §5). Zero Sierra/zmq/
//! pyo3 dependency (infrastructure guide §2.1 "pure core, thin edges") -- `rust/ffi` wraps this
//! for the DLL's `hmm_step`, `rust/py` wraps it for `lbrnet` (spec §5.4a), a standalone tool
//! wraps it for offline training (plan Stage G).

pub mod artifact;
pub mod audit;
pub mod gate_metrics;
pub mod init;
pub mod model;
pub mod regime_engine;
pub mod registry;
pub mod train;
