//! C-ABI for MindfulTrader.dll. Rules (plan §2/§6.5, W0s): every entry point is
//! catch_unwind-guarded and returns a status; state is created by an explicit init and destroyed by
//! an explicit shutdown, which the study calls on `sc.LastCallToFunction` -- never in a static
//! destructor or DLL_PROCESS_DETACH; no thread outlives shutdown; no allocation inside a `*_step`
//! hot-path function. See docs/superpowers/plans/2026-10-09-mindfultrader-rust-migration-plan.md.

use std::panic::{catch_unwind, AssertUnwindSafe};

#[repr(C)]
#[derive(Debug)]
pub enum Status {
    Ok = 0,
    BadArg = 1,
    NotInitialized = 2,
    Panicked = 3,
    Error = 4,
}

/// Runs `f`, converting any Rust panic into `Status::Panicked` instead of letting it unwind across
/// the FFI boundary (Rust's unwind-across-`extern "C"` behavior was undefined before 1.71 / RFC 2945;
/// this guard makes the question moot regardless of edition/ABI by never letting a panic reach the
/// boundary at all).
#[allow(dead_code)] // first real caller lands with the first feature (hmm/obs/transport)
fn guard(f: impl FnOnce() -> Status) -> Status {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(Status::Panicked)
}

/// The study checks this against its own compiled-in expectation and refuses to run on a mismatch,
/// rather than risk a struct-layout skew between what C++ expects and what this crate provides.
#[unsafe(no_mangle)]
pub extern "C" fn abi_version() -> u32 {
    1
}

#[cfg(feature = "observation_vector")]
pub mod observation_vector; // observation_vector_sevcik_fractal_dimension / _bowley_skewness / _moors_kurtosis
                            // -- FFI symbol names dropped the mts_ prefix 2026-10-11 (operator
                            // ruling), same as the crate/directory naming pass; the cbindgen
                            // `namespace = "mts"` in cbindgen.toml already namespaces every symbol
                            // for C++ callers (`mts::observation_vector_*`), making a second,
                            // redundant mts_ prefix on the symbol name itself pointless.

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn abi_version_is_stable() {
        assert_eq!(abi_version(), 1);
    }

    #[test]
    fn guard_converts_panic_to_status() {
        let status = guard(|| panic!("deliberate test panic"));
        assert!(matches!(status, Status::Panicked));
    }

    #[test]
    fn guard_passes_through_ok_status() {
        let status = guard(|| Status::Ok);
        assert!(matches!(status, Status::Ok));
    }
}
