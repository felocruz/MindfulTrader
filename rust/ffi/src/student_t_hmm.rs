//! C-ABI opaque-handle wrapper around `student_t_hmm::regime_engine::RegimeEngine` --
//! `hmm_init`/`hmm_step`/`hmm_reset`/`hmm_shutdown`, the init/step/reset/shutdown lifecycle spec
//! §5.2/§6.5 names (the first stateful resource in this crate -- every `observation_vector`
//! wrapper is a one-shot pure function, never holding state across calls).
//!
//! Monomorphized to the production shape (`PRODUCTION_D`=18, `PRODUCTION_K`=4,
//! `student_t_hmm::model`) -- the opaque handle hides a boxed
//! `RegimeEngine<PRODUCTION_D, PRODUCTION_K>`, never exposed to C++ as anything but a pointer.
//!
//! `f64` inside the model, `f32` on the wire (plan §6.5) -- every floating-point field on the
//! exported `RegimeState` struct is downcast from the engine's internal `f64` at this exact
//! boundary, nowhere else.

use std::ffi::{c_char, CStr};
use std::panic::{catch_unwind, AssertUnwindSafe};
use std::path::Path;

use student_t_hmm::model::{PRODUCTION_D, PRODUCTION_K};
use student_t_hmm::regime_engine::{RegimeEngine, StepContext};
use student_t_hmm::registry::MarketClimate as CoreMarketClimate;

use crate::Status;

type ProdEngine = RegimeEngine<PRODUCTION_D, PRODUCTION_K>;

/// A LOCAL, literal copy of `PRODUCTION_K` for use in `#[repr(C)]` struct field array sizes.
/// cbindgen's `parse_deps = false` cannot see `student_t_hmm::model::PRODUCTION_K`'s value (it
/// lives in a dependency crate) -- and, confirmed by inspecting the actual generated header,
/// cbindgen does not evaluate ANY named Rust `const` into a C++ array size either way, even a
/// local one; it only emits a literal integer token verbatim. `RegimeState::probability_vector`'s
/// field type below must therefore be the literal `[f32; 4]`, not `[f32; HMM_K]` -- the named
/// constant still exists so the compile-time assertion below has something to check: a future
/// `PRODUCTION_K` change that forgets to update the literal fails the Rust build immediately,
/// not silently producing a stale generated header with the wrong array size.
const HMM_K: usize = 4;
const _: () = assert!(HMM_K == PRODUCTION_K, "HMM_K (and RegimeState::probability_vector's literal [f32; 4]) must track student_t_hmm::model::PRODUCTION_K");

fn guard(f: impl FnOnce() -> Status) -> Status {
    catch_unwind(AssertUnwindSafe(f)).unwrap_or(Status::Panicked)
}

/// Opaque handle to a live `RegimeEngine` -- a zero-sized marker type, standard C-ABI idiom for
/// "callers only ever see a pointer to this, never its layout." The real allocation behind it is
/// `Box<ProdEngine>`; `hmm_init` creates it, `hmm_shutdown` is the only thing that frees it.
#[repr(C)]
#[derive(Debug)]
pub struct HmmHandle {
    _private: [u8; 0],
}

/// C-ABI mirror of `student_t_hmm::registry::MarketClimate` -- duplicated here, not re-exported,
/// because cbindgen's `parse_deps = false` (`cbindgen.toml`) only scans this crate; a type from a
/// path dependency is invisible to the generated header regardless of visibility. The `From`
/// impl below uses an EXHAUSTIVE match, not a bare `as u8` cast, so this copy cannot silently drift
/// out of sync with the authoritative enum -- a new variant there fails to compile here instead of
/// producing a wrong id (the exact misaligned-enum/array hazard this repo has already been bitten
/// by twice).
#[repr(u8)]
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub enum MarketClimate {
    #[default]
    GaussianStable = 0,
    TalebianFragile = 1,
    ShannonChaos = 2,
    ParetoMomentum = 3,
    CoiledSpring = 4,
}

impl From<CoreMarketClimate> for MarketClimate {
    fn from(value: CoreMarketClimate) -> Self {
        match value {
            CoreMarketClimate::GaussianStable => MarketClimate::GaussianStable,
            CoreMarketClimate::TalebianFragile => MarketClimate::TalebianFragile,
            CoreMarketClimate::ShannonChaos => MarketClimate::ShannonChaos,
            CoreMarketClimate::ParetoMomentum => MarketClimate::ParetoMomentum,
            CoreMarketClimate::CoiledSpring => MarketClimate::CoiledSpring,
        }
    }
}

/// C-ABI mirror of `student_t_hmm::regime_engine::RegimeState<PRODUCTION_K>`. Plain data, no
/// methods -- the rule API (`is_momentum_aligned`, `get_hmm_multiplier`, ...) is not exposed here;
/// that is Stage G7's C++-side cutover work (re-keying `RegimeManager.cpp` onto `MarketClimate`),
/// not this wiring task's scope.
#[repr(C)]
#[derive(Debug, Clone, Copy, Default)]
pub struct RegimeState {
    pub state_id: u8,
    pub profile_label: MarketClimate,
    pub profile_shannon_entropy: f32,
    pub profile_taleb_fragility: f32,
    pub profile_pareto_thrust: f32,
    pub climate: MarketClimate,
    pub unknown_posture: bool,
    pub probability_vector: [f32; 4], // HMM_K literal -- see this module's own const _ assertion
    pub entropy: f32,
    pub confidence: f32,
    pub transition_risk: f32,
    pub dof: f32,
    pub mahalanobis_distance: f32,
    pub tail_weight: f32,
    pub expected_duration: f32,
    pub variance_ratio: f32,
    pub model_drift_alert: bool,
    pub fim_distance_min: f32,
    pub fim_nearest_state: i32,
    pub black_swan_alert: bool,
    pub duration_bars: u32,
    pub duration_ratio: f32,
}

impl From<student_t_hmm::regime_engine::RegimeState<PRODUCTION_K>> for RegimeState {
    fn from(s: student_t_hmm::regime_engine::RegimeState<PRODUCTION_K>) -> Self {
        let mut probability_vector = [0.0f32; HMM_K];
        for (dst, src) in probability_vector.iter_mut().zip(s.probability_vector) {
            *dst = src as f32;
        }
        Self {
            state_id: s.state_id as u8,
            profile_label: s.profile_label.into(),
            profile_shannon_entropy: s.profile_shannon_entropy as f32,
            profile_taleb_fragility: s.profile_taleb_fragility as f32,
            profile_pareto_thrust: s.profile_pareto_thrust as f32,
            climate: s.climate.into(),
            unknown_posture: s.unknown_posture,
            probability_vector,
            entropy: s.entropy as f32,
            confidence: s.confidence as f32,
            transition_risk: s.transition_risk as f32,
            dof: s.dof as f32,
            mahalanobis_distance: s.mahalanobis_distance as f32,
            tail_weight: s.tail_weight as f32,
            expected_duration: s.expected_duration as f32,
            variance_ratio: s.variance_ratio as f32,
            model_drift_alert: s.model_drift_alert,
            fim_distance_min: s.fim_distance_min as f32,
            fim_nearest_state: s.fim_nearest_state,
            black_swan_alert: s.black_swan_alert,
            duration_bars: s.duration_bars,
            duration_ratio: s.duration_ratio as f32,
        }
    }
}

/// Loads a trained model + registry manifest (`student_t_hmm::artifact::load`'s JSON format) and
/// returns a live, stateful opaque handle. Call once per `sc.LastCallToFunction`-adjacent study
/// load (spec §6.5) -- never per tick, never per step.
///
/// # Safety
/// `path` must be a valid, NUL-terminated, UTF-8 C string. `out_handle` must be non-null and valid
/// for writes of one `*mut HmmHandle`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hmm_init(path: *const c_char, out_handle: *mut *mut HmmHandle) -> Status {
    guard(|| {
        if path.is_null() || out_handle.is_null() {
            return Status::BadArg;
        }
        let path_str = match unsafe { CStr::from_ptr(path) }.to_str() {
            Ok(s) => s,
            Err(_) => return Status::BadArg,
        };
        let (model, registry) =
            match student_t_hmm::artifact::load::<PRODUCTION_D, PRODUCTION_K>(Path::new(path_str)) {
                Ok(pair) => pair,
                Err(_) => return Status::Error,
            };
        let engine: Box<ProdEngine> = Box::new(RegimeEngine::new(model, registry));
        unsafe {
            *out_handle = Box::into_raw(engine) as *mut HmmHandle;
        }
        Status::Ok
    })
}

/// Advances the Hamilton-filter belief by one Mahalanobis-triggered observation (the existing
/// `otg::ObservationTriggerGate` decision, unchanged -- see spec §5.2) and writes the resulting
/// `RegimeState` to `out_state`. One call per trigger, never per tick; allocation-free.
///
/// # Safety
/// `handle` must be a valid, non-null handle from `hmm_init`, not yet passed to `hmm_shutdown`.
/// `obs` must be valid for reads of exactly `obs_len` contiguous `f64` elements. `out_state` must
/// be non-null and valid for writes of one `RegimeState`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hmm_step(
    handle: *mut HmmHandle,
    obs: *const f64,
    obs_len: usize,
    tail_index: f64,
    liq_fragility: f64,
    burstiness_index: f64,
    out_state: *mut RegimeState,
) -> Status {
    guard(|| {
        if handle.is_null() {
            return Status::NotInitialized;
        }
        if obs.is_null() || obs_len != PRODUCTION_D || out_state.is_null() {
            return Status::BadArg;
        }
        let engine = unsafe { &mut *(handle as *mut ProdEngine) };
        let obs_slice = unsafe { std::slice::from_raw_parts(obs, obs_len) };
        let mut obs_arr = [0.0f64; PRODUCTION_D];
        obs_arr.copy_from_slice(obs_slice);
        let ctx = StepContext { tail_index, liq_fragility, burstiness_index };
        let state = engine.step(&obs_arr, &ctx);
        unsafe {
            *out_state = state.into();
        }
        Status::Ok
    })
}

/// Resets the Hamilton-filter belief to the stationary prior and clears runtime model-risk state
/// (velocity EMA/deadzone, variance-ratio buffer, tail-risk/duration tracking) -- for a session
/// boundary (e.g. a new trading day), so stale belief from the previous session doesn't bleed
/// across the gap. The loaded model/registry survive (`RegimeEngine::reset`'s own doc comment);
/// does not reload from disk.
///
/// # Safety
/// `handle` must be a valid, non-null handle from `hmm_init`, not yet passed to `hmm_shutdown`.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hmm_reset(handle: *mut HmmHandle) -> Status {
    guard(|| {
        if handle.is_null() {
            return Status::NotInitialized;
        }
        let engine = unsafe { &mut *(handle as *mut ProdEngine) };
        engine.reset();
        Status::Ok
    })
}

/// Frees a handle created by `hmm_init`. Call explicitly on `sc.LastCallToFunction` (spec §6.5) --
/// never in a static destructor or `DLL_PROCESS_DETACH`. After this call, `handle` is dangling and
/// must never be used again (not re-passed to `hmm_step`/`hmm_reset`/`hmm_shutdown`).
///
/// # Safety
/// `handle` must be a valid, non-null handle from `hmm_init`, not already passed to this function.
#[unsafe(no_mangle)]
pub unsafe extern "C" fn hmm_shutdown(handle: *mut HmmHandle) -> Status {
    guard(|| {
        if handle.is_null() {
            return Status::NotInitialized;
        }
        unsafe {
            drop(Box::from_raw(handle as *mut ProdEngine));
        }
        Status::Ok
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::ffi::CString;
    use student_t_hmm::model::StudentTHmm;
    use student_t_hmm::registry::RegimeProfile;

    /// Writes a tiny, valid production-shaped (`D`=`PRODUCTION_D`, `K`=`PRODUCTION_K`) artifact to
    /// a unique temp-dir path and returns that path -- real `artifact::save`/`load`, not a
    /// hand-rolled fixture file, so this test exercises the exact round trip `hmm_init` performs.
    fn write_test_artifact(tag: &str) -> std::path::PathBuf {
        let mut means = [[0.0; PRODUCTION_D]; PRODUCTION_K];
        for (k, row) in means.iter_mut().enumerate() {
            row[0] = k as f64 * 5.0; // distinct per-state means -- avoids emission-density ties
        }
        let model = StudentTHmm::<PRODUCTION_D, PRODUCTION_K>::new_unweighted(
            means,
            [[1.0; PRODUCTION_D]; PRODUCTION_K],
            [[0.25; PRODUCTION_K]; PRODUCTION_K],
            [0.25; PRODUCTION_K],
            [10.0; PRODUCTION_K],
            2.1,
            100.0,
        );
        let registry: Vec<RegimeProfile> = (0..PRODUCTION_K)
            .map(|k| RegimeProfile {
                raw_id: k,
                market_climate: CoreMarketClimate::from_id(k as u8),
                shannon_entropy: 0.1,
                taleb_fragility: 0.1,
                pareto_thrust: 0.1,
                sample_share: 1.0 / PRODUCTION_K as f64,
                mean_tenure_bars: 10.0,
            })
            .collect();
        let path = std::env::temp_dir().join(format!(
            "mindfultrader_ffi_hmm_test_{tag}_{}.json",
            std::process::id()
        ));
        student_t_hmm::artifact::save(&path, &model, &registry).expect("save test artifact");
        path
    }

    #[test]
    fn init_step_reset_shutdown_round_trip() {
        let path = write_test_artifact("round_trip");
        let path_c = CString::new(path.to_str().expect("test path is UTF-8")).expect("test path has no interior NUL");

        let mut handle: *mut HmmHandle = std::ptr::null_mut();
        let status = unsafe { hmm_init(path_c.as_ptr(), &mut handle) };
        assert!(matches!(status, Status::Ok), "hmm_init: {status:?}");
        assert!(!handle.is_null());

        let obs = [0.0f64; PRODUCTION_D];
        let mut state = RegimeState::default();
        let status = unsafe {
            hmm_step(handle, obs.as_ptr(), obs.len(), 4.0, 0.0, 0.001, &mut state)
        };
        assert!(matches!(status, Status::Ok), "hmm_step: {status:?}");
        assert!(state.unknown_posture, "first step is burn-in");
        assert!((state.probability_vector.iter().sum::<f32>() - 1.0).abs() < 1e-5);

        let status = unsafe {
            hmm_step(handle, obs.as_ptr(), obs.len(), 4.0, 0.0, 0.001, &mut state)
        };
        assert!(matches!(status, Status::Ok));
        assert!(!state.unknown_posture, "second step is no longer burn-in");

        let status = unsafe { hmm_reset(handle) };
        assert!(matches!(status, Status::Ok), "hmm_reset: {status:?}");

        let status = unsafe {
            hmm_step(handle, obs.as_ptr(), obs.len(), 4.0, 0.0, 0.001, &mut state)
        };
        assert!(matches!(status, Status::Ok));
        assert!(state.unknown_posture, "post-reset step is burn-in again");

        let status = unsafe { hmm_shutdown(handle) };
        assert!(matches!(status, Status::Ok), "hmm_shutdown: {status:?}");

        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn hmm_init_rejects_null_and_missing_paths() {
        let mut handle: *mut HmmHandle = std::ptr::null_mut();
        assert!(matches!(
            unsafe { hmm_init(std::ptr::null(), &mut handle) },
            Status::BadArg
        ));
        assert!(handle.is_null());

        let missing = CString::new("/nonexistent/mindfultrader_hmm_artifact.json").expect("literal has no interior NUL");
        assert!(matches!(
            unsafe { hmm_init(missing.as_ptr(), &mut handle) },
            Status::Error
        ));
    }

    #[test]
    fn hmm_step_rejects_null_handle_and_wrong_length_obs() {
        let path = write_test_artifact("bad_args");
        let path_c = CString::new(path.to_str().expect("test path is UTF-8")).expect("test path has no interior NUL");
        let mut handle: *mut HmmHandle = std::ptr::null_mut();
        unsafe { hmm_init(path_c.as_ptr(), &mut handle) };

        let mut state = RegimeState::default();
        let short_obs = [0.0f64; 3];
        assert!(matches!(
            unsafe {
                hmm_step(std::ptr::null_mut(), short_obs.as_ptr(), short_obs.len(), 0.0, 0.0, 0.0, &mut state)
            },
            Status::NotInitialized
        ));
        assert!(matches!(
            unsafe {
                hmm_step(handle, short_obs.as_ptr(), short_obs.len(), 0.0, 0.0, 0.0, &mut state)
            },
            Status::BadArg
        ));

        unsafe { hmm_shutdown(handle) };
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn hmm_reset_and_shutdown_reject_null_handle() {
        assert!(matches!(unsafe { hmm_reset(std::ptr::null_mut()) }, Status::NotInitialized));
        assert!(matches!(unsafe { hmm_shutdown(std::ptr::null_mut()) }, Status::NotInitialized));
    }

    #[test]
    fn market_climate_conversion_matches_every_core_variant() {
        assert_eq!(MarketClimate::from(CoreMarketClimate::GaussianStable), MarketClimate::GaussianStable);
        assert_eq!(MarketClimate::from(CoreMarketClimate::TalebianFragile), MarketClimate::TalebianFragile);
        assert_eq!(MarketClimate::from(CoreMarketClimate::ShannonChaos), MarketClimate::ShannonChaos);
        assert_eq!(MarketClimate::from(CoreMarketClimate::ParetoMomentum), MarketClimate::ParetoMomentum);
        assert_eq!(MarketClimate::from(CoreMarketClimate::CoiledSpring), MarketClimate::CoiledSpring);
    }
}
