//! C-ABI wrappers around `mts_observation_vector`'s pure functions. Each function takes a raw
//! `(ptr, len)` pair (the only shape that crosses a C ABI cleanly) and returns `f32::NAN` on a
//! caught panic or a null/empty input, rather than using `MtsStatus` -- a NaN sentinel is the
//! existing convention these same functions already use for "degenerate input" (see each pure
//! function's own doc comment), so callers already have to handle it.

use std::panic::{catch_unwind, AssertUnwindSafe};

fn guard_f32(f: impl FnOnce() -> f32) -> f32 {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(f32::NAN)
}

fn guard_f64(f: impl FnOnce() -> f64) -> f64 {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(f64::NAN)
}

/// Builds a `&[f32]` from a C pointer + length, or `None` if the input is unusable (null pointer,
/// or `len == 0`, which every caller here already treats identically to "degenerate -> NaN").
unsafe fn slice_or_none<'a>(ptr: *const f32, len: usize) -> Option<&'a [f32]> {
    if ptr.is_null() || len == 0 {
        None
    } else {
        Some(unsafe { std::slice::from_raw_parts(ptr, len) })
    }
}

/// `f64` counterpart of `slice_or_none`. `ComputeBipowerVariation`'s C++ original takes `double`,
/// not `float` -- ports a mixed-precision codebase as-is, not "cleaned up" to one type.
unsafe fn slice_or_none_f64<'a>(ptr: *const f64, len: usize) -> Option<&'a [f64]> {
    if ptr.is_null() || len == 0 {
        None
    } else {
        Some(unsafe { std::slice::from_raw_parts(ptr, len) })
    }
}

/// Port of SevcikFractalDimension.h. `prices` must point to `len` chronologically-ordered points
/// (`prices[len-1]` = the live/current bar); see `mts_observation_vector::sevcik_fractal_dimension`'s
/// own doc comment for the exact windowing convention this replicates.
#[unsafe(no_mangle)]
pub extern "C" fn mts_observation_vector_sevcik_fractal_dimension(prices: *const f32, len: usize) -> f32 {
    guard_f32(|| match unsafe { slice_or_none(prices, len) } {
        Some(s) => mts_observation_vector::sevcik_fractal_dimension(s),
        None => f32::NAN,
    })
}

/// Port of RobustMoments.h's `BowleySkewness`. `returns` points to `len` log-return values.
#[unsafe(no_mangle)]
pub extern "C" fn mts_observation_vector_bowley_skewness(returns: *const f32, len: usize) -> f32 {
    guard_f32(|| match unsafe { slice_or_none(returns, len) } {
        Some(s) => mts_observation_vector::bowley_skewness(s),
        None => f32::NAN,
    })
}

/// Port of RobustMoments.h's `MoorsKurtosis`. `returns` points to `len` log-return values.
#[unsafe(no_mangle)]
pub extern "C" fn mts_observation_vector_moors_kurtosis(returns: *const f32, len: usize) -> f32 {
    guard_f32(|| match unsafe { slice_or_none(returns, len) } {
        Some(s) => mts_observation_vector::moors_kurtosis(s),
        None => f32::NAN,
    })
}

/// Port of BipowerVariation.h's `ComputeBipowerVariation`. `returns` points to `len` return
/// values (`double`, matching the C++ original -- not every dim in this codebase uses `float`).
/// Unlike the other wrappers here, `None` (null/empty input) maps to `0.0`, not NaN -- matching
/// `compute_bipower_variation`'s own "n < 2 -> 0.0" convention (BV has no NaN-returning case at
/// all; a degenerate window is a genuinely zero-variance neutral reading, not "unknown").
#[unsafe(no_mangle)]
pub extern "C" fn mts_observation_vector_compute_bipower_variation(returns: *const f64, len: usize) -> f64 {
    guard_f64(|| match unsafe { slice_or_none_f64(returns, len) } {
        Some(s) => mts_observation_vector::compute_bipower_variation(s),
        None => 0.0,
    })
}
