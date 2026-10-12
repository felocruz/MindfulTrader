//! regime_engine.rs -- the live, stateful HMM "producer" (plan Stage G4), ported from
//! `lbrnet/models/regime_engine.py`'s `RegimeEngine`/`RegimeState` -- the reference design spec
//! §5.1d names explicitly (NOT the superseded `regime_manager.py`, NOT C++'s current
//! `RegimeManager.cpp`/`MarketClimateIndicator`, both of which still depend on the broken
//! positional `HMMStateEnum` convention removed from Python 2026-09-21).
//!
//! Design, resolved this session (G0(a)):
//! - **Producer, not oracle.** `RegimeEngine` owns exactly one mutable-authority method,
//!   `step()`: it advances the Hamilton-filter belief by one (Mahalanobis-triggered, per
//!   `otg::ObservationTriggerGate` -- unchanged, see spec §5.2) observation and returns an owned,
//!   immutable `RegimeState` value. No other method reads or mutates live state.
//! - **Rules live on the value, not the producer.** Every "ask a question" method
//!   (`is_momentum_aligned`, `get_hmm_multiplier`, ...) is defined on `RegimeState` itself, so a
//!   caller holding any two snapshots -- from any thread, at any time -- can answer any rule
//!   question without touching the live `RegimeEngine` at all. This is what makes the design safe
//!   across the real planned topology (a tokio order-watcher thread, a Transformer-inference
//!   thread, a user-action/flatten thread, spec §8) without a lock around the engine for every
//!   single query -- only `step()` itself needs exclusive access.
//! - **No `RegimeSnapshot` wrapper type.** That C++ class existed solely because the HMM lived
//!   across a process boundary (Python, async ZMQ) and C++ needed a safe, opaque way to hold a
//!   point-in-time identity. Once the engine runs in-process and synchronously, the hazard class
//!   it defended against (stale/out-of-order/misinterpreted raw state) no longer exists --
//!   `RegimeState` is already `Copy` and cheap, so a caller just stores a copy of a prior
//!   `RegimeState` directly and calls `.is_hostile_change_from(&entry, is_long)` on the current
//!   one. `RegimeSnapshot` (`include/RegimeManager.h`) is retired in the C++-side consequence of
//!   this port (plan Stage G7), not reproduced here.
//! - **`state_id`/`profile_label` are canonical identity; `climate` is a legacy-shaped but still
//!   materially useful override-corrected projection** (P2.4/P2.4b fat-tail/momentum overrides +
//!   velocity-deadzone hysteresis + entropy-chaos override), per `RegimeState`'s own docstring in
//!   `regime_engine.py`: "Do not add new logic that depends on `climate` identity" for anything
//!   NEW, but existing consumers (pattern multipliers, risk gates) correctly read `climate` because
//!   it is empirically the more behaviorally-reliable signal today (corrected against live
//!   observation features the static per-state profile can't see).
//!
//! Every constant, table, and the override cascade below is ported 1:1 from
//! `lbrnet/models/regime_engine.py` / `lbrnet/core/scoring.py` and golden-parity-validated against
//! real invocations of the actual Python `RegimeEngine._classify_regime()` /
//! `_calculate_normalized_entropy()` / `_calculate_confidence()` (mts conda env, 2026-10-12) -- see
//! this module's own tests.
//!
//! Scope cuts, deliberate, not silently dropped:
//! - `cognitive_load`/`cognitive_stall`/`inference_latency_us` (wall-clock-derived, non-
//!   deterministic -- spec §5.5 item 2, already flagged as a drop candidate).
//! - `observation` passthrough on `RegimeState` (Python needed it because a caller could
//!   reconstruct a `RegimeState` from stored posteriors without the original observation; Rust's
//!   caller already holds the observation it just passed into `step()`).
//! - `hmm_probability_vector`/`_project_probs_to_hmm_enums` (a legacy fixed-4-slot projection onto
//!   the retiring `HMMStateEnum` -- itself part of the `TrainingEvent` K=4-hardcoding bug spec
//!   §5.1d already flags for a *wire-schema* fix, not an engine concern).
//! - Per-state duration *history* (`_regime_duration_history`, used only for offline diagnostics
//!   in Python) -- `duration_bars`/`duration_ratio` (what `RegimeState` actually exposes) are kept.

use crate::model::StudentTHmm;
use crate::registry::{MarketClimate, RegimeProfile};

// ---------------------------------------------------------------------------------------------
// Constants -- ported 1:1 from `regime_engine.py` module-level constants.
// ---------------------------------------------------------------------------------------------

/// Deadzone *enter* threshold for the smoothed burstiness ("velocity") signal.
const VELOCITY_EPSILON: f64 = 1e-5;
/// Deadzone *exit* threshold -- deliberately higher than the enter threshold (hysteresis, not a
/// single boundary) so the override doesn't chatter at the edge.
const VELOCITY_EPSILON_EXIT: f64 = 2e-5;
/// EMA smoothing factor applied to the raw burstiness signal before the deadzone check.
const VELOCITY_SMOOTHING_ALPHA: f64 = 0.20;
/// Normalized-entropy threshold above which climate is force-overridden to `ShannonChaos`
/// regardless of the manifest profile or the P2.4/P2.4b overrides.
const ENTROPY_CHAOS_THRESHOLD: f64 = 0.85;
/// Black Swan alert: `tail_weight` sustained below this for `TAIL_RISK_STREAK_COUNT` consecutive
/// steps is a Taleb fat-tail heads-up.
const TAIL_RISK_THRESHOLD: f64 = 0.51;
const TAIL_RISK_STREAK_COUNT: u32 = 3;
/// Derman variance-ratio drift: rolling observation-buffer length, breach threshold, and
/// consecutive-breach count before `model_drift_alert` fires.
const VARIANCE_RATIO_WINDOW: usize = 50;
const VARIANCE_RATIO_DRIFT_THRESHOLD: f64 = 2.0;
const VARIANCE_RATIO_DRIFT_COUNT: u32 = 5;

// ---------------------------------------------------------------------------------------------
// PatternType -- ported from `include/Scoring.h`'s `enum class PatternType` (mirrors the same 5
// Python string keys `HMM_MULTIPLIERS`/`CLIMATE_MULTIPLIERS` use: momentum_pinball, turtle_soup,
// elder_breakout, kangaroo_tail, nr7).
// ---------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum PatternType {
    KangarooTail,
    MomentumPinball,
    ElderBreakout,
    TurtleSoup,
    Nr7,
    Unknown,
}

/// Index of `MarketClimate` into a 5-element multiplier table -- `MarketClimate`'s own `#[repr(u8)]`
/// ids, matching `scoring.py`'s dict keys exactly (`GaussianStable=0` .. `CoiledSpring=4`).
fn climate_index(climate: MarketClimate) -> usize {
    climate as u8 as usize
}

/// `HMM_MULTIPLIERS` (`scoring.py`): pattern efficacy multiplier keyed by the STATIC per-state
/// profile archetype (`profile_label`). `PatternType::Unknown` and any pattern Python doesn't
/// define default to `1.0` (neutral), matching `get_hmm_multiplier`'s own `if pattern_name not in
/// HMM_MULTIPLIERS: return 1.0` fallback.
fn hmm_multiplier_table(pattern: PatternType) -> Option<[f64; 5]> {
    // Order: [GaussianStable, TalebianFragile, ShannonChaos, ParetoMomentum, CoiledSpring]
    match pattern {
        PatternType::MomentumPinball => Some([1.00, 0.85, 0.85, 0.85, 1.00]),
        PatternType::TurtleSoup => Some([0.95, 0.75, 0.75, 0.75, 0.95]),
        PatternType::ElderBreakout => Some([1.10, 1.35, 1.35, 1.35, 1.10]),
        PatternType::KangarooTail => Some([1.00, 1.10, 1.10, 1.10, 1.00]),
        PatternType::Nr7 => Some([1.30, 0.80, 0.80, 0.80, 1.30]),
        PatternType::Unknown => None,
    }
}

/// `CLIMATE_MULTIPLIERS` (`scoring.py`): pattern efficacy multiplier keyed by the LIVE,
/// override-corrected `climate` -- a genuinely different table from `HMM_MULTIPLIERS` above, not a
/// duplicate (see `RegimeState::get_climate_multiplier`'s own doc comment for why).
fn climate_multiplier_table(pattern: PatternType) -> Option<[f64; 5]> {
    match pattern {
        PatternType::MomentumPinball => Some([1.05, 0.90, 1.20, 0.70, 0.95]),
        PatternType::TurtleSoup => Some([1.10, 0.80, 1.25, 0.60, 0.85]),
        PatternType::ElderBreakout => Some([0.90, 1.15, 0.60, 1.40, 1.25]),
        PatternType::KangarooTail => Some([1.00, 1.20, 1.10, 1.05, 1.00]),
        PatternType::Nr7 => Some([1.00, 1.10, 0.80, 0.75, 1.45]),
        PatternType::Unknown => None,
    }
}

/// `CLIMATE_CONVICTION_BOOST` (`scoring.py`) -- TalebianFragile/ShannonChaos deliberately
/// unmapped -> neutral `1.0` default (ENTER actions in those climates are already forced to
/// STAND_ASIDE by the live hard-stop gate before this is ever consulted).
fn climate_conviction_boost_table() -> [f64; 5] {
    [1.0, 1.0, 1.0, 1.2, 0.85]
}

/// `CLIMATE_RISK_MULTIPLIER` (`scoring.py`) -- the `RiskStateUpdate` wire field's position-size
/// multiplier. TalebianFragile/ShannonChaos deliberately zero (hard stop).
fn climate_risk_multiplier_table() -> [f64; 5] {
    [1.0, 0.0, 0.0, 1.2, 0.85]
}

/// `CLIMATE_HALF_LIFE_SCALE` (`scoring.py`) -- dimensionless; the caller multiplies this by its
/// own base half-life (an agent-owned runtime parameter, not HMM domain knowledge).
fn climate_half_life_scale_table() -> [f64; 5] {
    [1.5, 0.75, 0.5, 0.5, 0.75]
}

// ---------------------------------------------------------------------------------------------
// StepContext -- the 3 named observation-vector values the P2.4/P2.4b override cascade needs.
// Deliberately NOT an index into `obs: &[f64; D]` -- `rust/observation_vector` already computes
// each of these as its own named function (`compute_burstiness_index` et al.); there is no
// generated dimension-index contract on the Rust side (and none is needed), since the caller
// assembling `StepContext` already has these as local named values before `obs` is ever flattened
// for `model.rs`'s uniform per-dimension math. (Decided 2026-10-12: as the observation-vector
// pipeline migrates further into Rust -- the ACSIL boundary narrows to wherever Sierra Chart's own
// C++-only APIs are unavoidable -- this becomes even cheaper: the caller computes these 3 values
// directly, no C++ round-trip at all.)
// ---------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, Default, PartialEq)]
pub struct StepContext {
    pub tail_index: f64,
    pub liq_fragility: f64,
    pub burstiness_index: f64,
}

// ---------------------------------------------------------------------------------------------
// RegimeState -- the per-step output value. Owned, immutable, `Copy` (no heap, fixed `K`) -- safe
// to hold across threads, store at trade entry, or pass to any rule method below.
// ---------------------------------------------------------------------------------------------

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct RegimeState<const K: usize> {
    // --- canonical identity (lbrnet's `RegimeState` reference design, 2026-09-22) ---
    /// Raw model index -- positional, opaque. Only for `probability_vector` indexing and as a
    /// cheap equality/logging token; never a branch target.
    pub state_id: usize,
    /// `registry[state_id]`'s STATIC archetype -- stable per retrain, the run-independent identity
    /// `get_hmm_multiplier` resolves against.
    pub profile_label: MarketClimate,
    pub profile_shannon_entropy: f64,
    pub profile_taleb_fragility: f64,
    pub profile_pareto_thrust: f64,

    // --- live, override-corrected projection (P2.4/P2.4b + velocity-deadzone + entropy-chaos) ---
    pub climate: MarketClimate,
    /// True only for the engine's very first `step()` call (burn-in) -- the wire-corruption
    /// defense this flag used to carry (`HMMClient`'s invalid-state-id check) no longer applies;
    /// there is no wire.
    pub unknown_posture: bool,

    // --- raw inference diagnostics ---
    pub probability_vector: [f64; K],
    /// Normalized Shannon entropy in [0.0, 1.0] (0.0 = certain, 1.0 = uniform/maximum uncertainty).
    pub entropy: f64,
    /// Confidence throttle in [0.0, 1.0]: `max_prob * (0.6 + 0.4 * (1 - entropy))`.
    pub confidence: f64,
    /// `1 - P(state_id -> state_id)`: probability the regime flips next bar.
    pub transition_risk: f64,
    pub dof: f64,
    pub mahalanobis_distance: f64,
    pub tail_weight: f64,
    /// `E[bars in current state] = 1 / transition_risk`.
    pub expected_duration: f64,

    // --- model-risk stateful (Derman variance-ratio drift, Amari FIM, duration tracking) ---
    pub variance_ratio: f64,
    pub model_drift_alert: bool,
    /// Static per-state constant (Fisher-information distance to the nearest rival state) --
    /// precomputed once at `RegimeEngine::new()`, not recomputed per step. Diagnostic/telemetry
    /// only, per the ported formula's own citation (`docs/ADR/live_agent_risk_gate_audit.md` §4).
    pub fim_distance_min: f64,
    pub fim_nearest_state: i32,
    pub black_swan_alert: bool,
    pub duration_bars: u32,
    pub duration_ratio: f64,
}

impl<const K: usize> RegimeState<K> {
    // --- directional alignment (new mapping onto the 5-way taxonomy, no Python equivalent --
    //     see this method's own doc comment) ---

    /// Whether this regime is momentum-aligned for a thesis in the given direction. Re-expresses
    /// the retired 4-way `HMMStateEnum` grouping (`{PARETO_MOMENTUM, GAUSSIAN_STABLE}`) on the
    /// 5-way `MarketClimate` taxonomy -- a genuinely NEW mapping decision (Python's `RegimeState`
    /// has no equivalent method; this was always a C++-only, `RegimeManager`-specific concept),
    /// not a literal port. Deliberately excludes `ShannonChaos` from both alignment sets (an
    /// improvement over the old scheme, which forced ambiguous/chaotic states into the
    /// mean-reversion-aligned bucket whether or not that was appropriate).
    pub fn is_momentum_aligned(&self, is_bullish: bool) -> bool {
        let in_momentum_set =
            matches!(self.climate, MarketClimate::ParetoMomentum | MarketClimate::GaussianStable);
        if !in_momentum_set {
            return false;
        }
        // Short-vs-ParetoMomentum hostility, same convention as the old HMMStateEnum scheme.
        !(!is_bullish && self.climate == MarketClimate::ParetoMomentum)
    }

    /// Mirror of `is_momentum_aligned` for mean-reversion theses. `TalebianFragile`/`CoiledSpring`
    /// are the nearest 5-way analogue of the old `GAUSSIAN_FRAGILE` bucket (`ShannonChaos`
    /// deliberately excluded -- see `is_momentum_aligned`'s doc comment).
    pub fn is_mean_reversion_aligned(&self, is_bullish: bool) -> bool {
        let in_mean_rev_set =
            matches!(self.climate, MarketClimate::CoiledSpring | MarketClimate::TalebianFragile);
        if !in_mean_rev_set {
            return false;
        }
        !(is_bullish && self.climate == MarketClimate::TalebianFragile)
    }

    pub fn requires_passive_execution(&self) -> bool {
        self.climate == MarketClimate::TalebianFragile
    }

    pub fn grants_momentum_boost(&self) -> bool {
        self.climate == MarketClimate::ParetoMomentum
    }

    /// Whether `current` (i.e. `self`) is a hostile regime change relative to a previously-stored
    /// `entry` snapshot, for a position opened in the given direction. Replaces C++'s
    /// `RegimeManager::IsHostileRegimeChange(RegimeSnapshot, bool)` -- no wrapper type needed, the
    /// caller just stores a copy of the `RegimeState` (or its `.climate`/`.profile_label`) from
    /// trade-entry time and calls this on the current one.
    pub fn is_hostile_change_from(&self, entry: &RegimeState<K>, is_long: bool) -> bool {
        if entry.climate == self.climate {
            return false;
        }
        (is_long
            && matches!(self.climate, MarketClimate::TalebianFragile | MarketClimate::ShannonChaos))
            || (!is_long && self.climate == MarketClimate::ParetoMomentum)
    }

    // --- State Manager API, ported from `regime_engine.py`'s `RegimeState` methods ---

    /// Pattern efficacy multiplier for this state's STATIC archetype (`HMM_MULTIPLIERS`).
    /// Resolves via `profile_label`, never `state_id` -- `profile_label` is the run-independent
    /// identity that survives a retrain; `state_id` is positional and can mean something
    /// different every retrain.
    pub fn get_hmm_multiplier(&self, pattern: PatternType) -> f64 {
        hmm_multiplier_table(pattern).map_or(1.0, |t| t[climate_index(self.profile_label)])
    }

    /// Pattern efficacy multiplier for this state's LIVE, override-corrected `climate`.
    /// Deliberately a DIFFERENT table from `get_hmm_multiplier` -- `climate` is corrected against
    /// live observation features (P2.4/P2.4b + entropy-chaos), so it is a more behaviorally
    /// reliable signal than the static profile alone, and the two tables were calibrated
    /// independently against that difference.
    pub fn get_climate_multiplier(&self, pattern: PatternType) -> f64 {
        climate_multiplier_table(pattern).map_or(1.0, |t| t[climate_index(self.climate)])
    }

    pub fn get_climate_conviction_boost(&self) -> f64 {
        climate_conviction_boost_table()[climate_index(self.climate)]
    }

    pub fn get_climate_risk_multiplier(&self) -> f64 {
        climate_risk_multiplier_table()[climate_index(self.climate)]
    }

    /// Dimensionless temporal half-life scale factor. The caller multiplies this by its own base
    /// half-life seconds -- this never returns an absolute duration.
    pub fn get_half_life_scale(&self) -> f64 {
        climate_half_life_scale_table()[climate_index(self.climate)]
    }

    pub fn is_conviction_amplifier_eligible(&self) -> bool {
        self.climate == MarketClimate::ParetoMomentum
    }

    /// Whether this regime triggers the LockC hard stop (`CLIMATE_HARD_STOP_ELIGIBLE`).
    pub fn is_hard_stop_climate(&self) -> bool {
        matches!(self.climate, MarketClimate::TalebianFragile | MarketClimate::ShannonChaos)
    }
}

// ---------------------------------------------------------------------------------------------
// RegimeEngine -- the sole mutable-authority producer. `step()` is the only method that mutates;
// everything else is a read of static, precomputed-at-construction data.
// ---------------------------------------------------------------------------------------------

#[derive(Debug)]
pub struct RegimeEngine<const D: usize, const K: usize> {
    model: StudentTHmm<D, K>,
    registry: Vec<RegimeProfile>,
    /// Static per-state Fisher-information distance to the nearest rival state, precomputed once
    /// here (depends only on the trained model's means/covars/dof, never the live observation).
    fim_table: Vec<(f64, i32)>,

    belief: [f64; K],
    has_stepped: bool,

    velocity_ema: f64,
    velocity_deadzone_active: bool,

    variance_ratio_buffer: [[f64; D]; VARIANCE_RATIO_WINDOW],
    variance_ratio_cursor: usize,
    variance_ratio_filled: usize,
    variance_drift_count: u32,

    tail_risk_streak: u32,

    current_regime_id: i32,
    current_regime_duration: u32,
}

/// Normalized Shannon entropy in [0.0, 1.0]: `H = -sum(p*ln(p)) / ln(K)`, clipped at `1e-10` to
/// avoid `ln(0)`. Ported from `_calculate_normalized_entropy`.
fn normalized_entropy<const K: usize>(probs: &[f64; K]) -> f64 {
    const EPSILON: f64 = 1e-10;
    let h_raw: f64 = probs.iter().map(|&p| {
        let pc = p.clamp(EPSILON, 1.0);
        -pc * pc.ln()
    }).sum();
    let h_max = (K as f64).ln();
    if h_max > 0.0 {
        h_raw / h_max
    } else {
        0.0
    }
}

/// Confidence throttle in [0.0, 1.0]. Ported from `_calculate_confidence`.
fn calculate_confidence(max_prob: f64, entropy_norm: f64) -> f64 {
    let entropy_factor = 1.0 - entropy_norm;
    (max_prob * (0.6 + 0.4 * entropy_factor)).clamp(0.0, 1.0)
}

/// Fisher-information distance between regime location parameters (diagonal-covariance Student-t
/// location FIM, scale/dof held fixed): `I(mu) = [(nu+D)/(nu+D+2)] * Sigma^-1`. Ported from
/// `_compute_fim_distance` -- static per-state constant, computed once at construction.
fn compute_fim_table<const D: usize, const K: usize>(model: &StudentTHmm<D, K>) -> Vec<(f64, i32)> {
    if K < 2 {
        return vec![(0.0, -1); K];
    }
    (0..K)
        .map(|k| {
            let fim_scale = (model.dof[k] + D as f64) / (model.dof[k] + D as f64 + 2.0);
            let mut min_dist = f64::INFINITY;
            let mut nearest: i32 = -1;
            for j in 0..K {
                if j == k {
                    continue;
                }
                let mut d_sq = 0.0;
                for d in 0..D {
                    let var = model.covars[k][d].max(1e-12);
                    let delta = model.means[k][d] - model.means[j][d];
                    d_sq += fim_scale * (delta * delta) / var;
                }
                let dist = d_sq.sqrt();
                if dist < min_dist {
                    min_dist = dist;
                    nearest = j as i32;
                }
            }
            (if min_dist.is_finite() { min_dist } else { 0.0 }, nearest)
        })
        .collect()
}

/// Allocation-free median of a fixed-size array: sorts `values` in place with `sort_unstable_by`
/// (pattern-defeating quicksort, no scratch-buffer allocation, unlike the stable `sort_by` the
/// shared `registry::median` helper uses for its own once-per-training-run offline-tooling
/// callers) -- required here since `compute_variance_ratio` runs inside `RegimeEngine::step()`,
/// this crate's one genuine hot path (plan §6.5: "no allocation in any `*_step` function").
fn median_inplace<const D: usize>(values: &mut [f64; D]) -> f64 {
    values.sort_unstable_by(f64::total_cmp);
    if D == 0 {
        return 0.0;
    }
    if D % 2 == 1 {
        values[D / 2]
    } else {
        0.5 * (values[D / 2 - 1] + values[D / 2])
    }
}

/// Derman variance ratio: median per-dimension ratio of realized (rolling-window) variance to the
/// winning state's learned covariance. ~1.0 means the live market matches the model; sustained
/// values above `VARIANCE_RATIO_DRIFT_THRESHOLD` signal stale covariance. Ported from
/// `_compute_variance_ratio`. Allocation-free: `buffer` is always exactly `VARIANCE_RATIO_WINDOW`
/// fixed-size rows (the caller only invokes this once the ring buffer is full).
fn compute_variance_ratio<const D: usize>(
    buffer: &[[f64; D]; VARIANCE_RATIO_WINDOW],
    model_var: &[f64; D],
) -> f64 {
    let w = VARIANCE_RATIO_WINDOW as f64;
    let mut mean = [0.0; D];
    for obs in buffer {
        for d in 0..D {
            mean[d] += obs[d];
        }
    }
    for m in &mut mean {
        *m /= w;
    }
    let mut ratios = [0.0; D];
    for obs in buffer {
        for d in 0..D {
            let diff = obs[d] - mean[d];
            ratios[d] += diff * diff;
        }
    }
    for d in 0..D {
        let realized_var = ratios[d] / w;
        let safe_model_var = model_var[d].max(1e-12);
        ratios[d] = realized_var / safe_model_var;
    }
    median_inplace(&mut ratios)
}


impl<const D: usize, const K: usize> RegimeEngine<D, K> {
    pub fn new(model: StudentTHmm<D, K>, registry: Vec<RegimeProfile>) -> Self {
        let fim_table = compute_fim_table(&model);
        let belief = model.startprob;
        Self {
            model,
            registry,
            fim_table,
            belief,
            has_stepped: false,
            velocity_ema: 0.0,
            velocity_deadzone_active: false,
            variance_ratio_buffer: [[0.0; D]; VARIANCE_RATIO_WINDOW],
            variance_ratio_cursor: 0,
            variance_ratio_filled: 0,
            variance_drift_count: 0,
            tail_risk_streak: 0,
            current_regime_id: -1,
            current_regime_duration: 0,
        }
    }

    pub fn is_burn_in_complete(&self) -> bool {
        self.has_stepped
    }

    /// Resets the Hamilton forward filter to the stationary prior -- call on session boundaries
    /// (e.g. a new trading day) to prevent stale belief from the previous session bleeding across
    /// the gap. Ported from `reset_belief()`'s exact scope: the loaded model/registry/`fim_table`
    /// are untouched (expensive to reload, and static/model-derived, not session state); every
    /// other piece of mutable runtime state resets to its just-constructed value.
    pub fn reset(&mut self) {
        self.belief = self.model.startprob;
        self.has_stepped = false;
        self.velocity_ema = 0.0;
        self.velocity_deadzone_active = false;
        self.variance_ratio_cursor = 0;
        self.variance_ratio_filled = 0;
        self.variance_drift_count = 0;
        self.tail_risk_streak = 0;
        self.current_regime_id = -1;
        self.current_regime_duration = 0;
    }

    /// Manifest-backed classifier + P2.4/P2.4b/velocity-deadzone/entropy-chaos override cascade.
    /// Ported verbatim (including the override PRIORITY ORDER: P2.4 -> P2.4b -> entropy/deadzone,
    /// the last of which wins over everything above it) from `_classify_regime`. Returns
    /// `(profile_label, profile_shannon_entropy, profile_taleb_fragility, profile_pareto_thrust,
    /// climate)`.
    #[allow(clippy::too_many_arguments)]
    fn classify_regime(
        &mut self,
        state_id: usize,
        entropy_norm: f64,
        ctx: &StepContext,
    ) -> (MarketClimate, f64, f64, f64, MarketClimate) {
        // `MarketClimate::handle_missing()`'s Python equivalent: fall back to ShannonChaos
        // (maximum entropy) to force a defensive posture, rather than panicking.
        let Some(profile) = self.registry.get(state_id) else {
            return (
                MarketClimate::ShannonChaos,
                0.0,
                0.0,
                0.0,
                MarketClimate::ShannonChaos,
            );
        };
        let (profile_label, pse, ptf, ppt) =
            (profile.market_climate, profile.shannon_entropy, profile.taleb_fragility, profile.pareto_thrust);
        let mut climate = profile.market_climate;

        // P2.4: fat-tail danger override -- forces TalebianFragile regardless of the static
        // profile, catching regime transitions the HMM's own state posterior is too slow to see.
        let tail_alpha = ctx.tail_index;
        let fragility = ctx.liq_fragility;
        if tail_alpha > 0.0 && tail_alpha < 1.5 && fragility > 3.0 {
            climate = MarketClimate::TalebianFragile;
        }

        // P2.4b: mirror-image momentum upgrade, only reachable if P2.4 didn't already fire and
        // the static profile under-classified a genuinely safe, bursty move as GaussianStable.
        let burstiness = ctx.burstiness_index;
        if climate == MarketClimate::GaussianStable
            && tail_alpha > 2.0
            && fragility < 1.0
            && burstiness > 2.0
        {
            climate = MarketClimate::ParetoMomentum;
        }

        // Velocity EMA + enter/exit hysteresis deadzone (microstructure-noise suppression).
        self.velocity_ema = (1.0 - VELOCITY_SMOOTHING_ALPHA) * self.velocity_ema
            + VELOCITY_SMOOTHING_ALPHA * burstiness;
        let abs_velocity = self.velocity_ema.abs();
        if self.velocity_deadzone_active {
            if abs_velocity >= VELOCITY_EPSILON_EXIT {
                self.velocity_deadzone_active = false;
            }
        } else if abs_velocity < VELOCITY_EPSILON {
            self.velocity_deadzone_active = true;
        }

        // Entropy-chaos / velocity-deadzone override -- applied LAST, wins over P2.4/P2.4b.
        if entropy_norm > ENTROPY_CHAOS_THRESHOLD || self.velocity_deadzone_active {
            climate = MarketClimate::ShannonChaos;
        }

        (profile_label, pse, ptf, ppt, climate)
    }

    /// Advances the Hamilton-filter belief by one Mahalanobis-triggered observation and returns
    /// the resulting, owned `RegimeState`. The only mutable-authority method on this type.
    pub fn step(&mut self, obs: &[f64; D], ctx: &StepContext) -> RegimeState<K> {
        self.belief = if self.has_stepped {
            self.model.forward_step(&self.belief, obs)
        } else {
            // First observation: emission update only, no transition -- matches
            // `StudentTHmm::predict`'s own first-observation handling.
            let s0: f64 = self.belief.iter().sum();
            let mut prior = self.belief;
            if s0 > 0.0 && s0.is_finite() {
                for p in &mut prior {
                    *p /= s0;
                }
            } else {
                prior = [1.0 / K as f64; K];
            }
            self.model.forward_step(&prior, obs)
        };
        let unknown_posture = !self.has_stepped;
        self.has_stepped = true;

        let entropy_norm = normalized_entropy(&self.belief);
        let state_id = self
            .belief
            .iter()
            .enumerate()
            .max_by(|a, b| a.1.total_cmp(b.1))
            .map(|(idx, _)| idx)
            .unwrap_or(0);
        let max_prob = self.belief[state_id];

        let (profile_label, profile_shannon_entropy, profile_taleb_fragility, profile_pareto_thrust, climate) =
            self.classify_regime(state_id, entropy_norm, ctx);
        let confidence = calculate_confidence(max_prob, entropy_norm);

        let prob_stay = self.model.transmat[state_id][state_id];
        let transition_risk = 1.0 - prob_stay;
        let expected_duration = 1.0 / transition_risk.max(1e-6);

        let dof = self.model.dof[state_id];
        let mahalanobis_distance = self.model.weighted_delta(state_id, obs);
        let tail_weight = self.model.compute_tail_weight(state_id, obs);

        self.variance_ratio_buffer[self.variance_ratio_cursor] = *obs;
        self.variance_ratio_cursor = (self.variance_ratio_cursor + 1) % VARIANCE_RATIO_WINDOW;
        self.variance_ratio_filled = (self.variance_ratio_filled + 1).min(VARIANCE_RATIO_WINDOW);
        let variance_ratio = if self.variance_ratio_filled < VARIANCE_RATIO_WINDOW {
            1.0
        } else {
            compute_variance_ratio(&self.variance_ratio_buffer, &self.model.covars[state_id])
        };
        if variance_ratio > VARIANCE_RATIO_DRIFT_THRESHOLD {
            self.variance_drift_count += 1;
        } else {
            self.variance_drift_count = self.variance_drift_count.saturating_sub(1);
        }
        let model_drift_alert = self.variance_drift_count >= VARIANCE_RATIO_DRIFT_COUNT;

        if tail_weight < TAIL_RISK_THRESHOLD {
            self.tail_risk_streak += 1;
        } else {
            self.tail_risk_streak = 0;
        }
        let black_swan_alert = self.tail_risk_streak >= TAIL_RISK_STREAK_COUNT;

        let (fim_distance_min, fim_nearest_state) = self.fim_table[state_id];

        if state_id as i32 == self.current_regime_id {
            self.current_regime_duration += 1;
        } else {
            self.current_regime_id = state_id as i32;
            self.current_regime_duration = 1;
        }
        let duration_bars = self.current_regime_duration;
        let duration_ratio = duration_bars as f64 / expected_duration.max(1.0);

        RegimeState {
            state_id,
            profile_label,
            profile_shannon_entropy,
            profile_taleb_fragility,
            profile_pareto_thrust,
            climate,
            unknown_posture,
            probability_vector: self.belief,
            entropy: entropy_norm,
            confidence,
            transition_risk,
            dof,
            mahalanobis_distance,
            tail_weight,
            expected_duration,
            variance_ratio,
            model_drift_alert,
            fim_distance_min,
            fim_nearest_state,
            black_swan_alert,
            duration_bars,
            duration_ratio,
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::registry::RegimeProfile;

    // ------------------------------------------------------------------------------------------
    // Golden-parity fixtures: generated 2026-10-12 against real `lbrnet.models.regime_engine`
    // execution (mts conda env), via a hand-constructed `RegimeEngine.__new__` instance with a
    // synthetic `_profiles_by_state` dict (real function, synthetic model data -- same pattern as
    // `registry.rs`'s own golden-parity test). See this session's transcript for the exact script.
    // ------------------------------------------------------------------------------------------

    fn check(name: &str, got: f64, want: f64, tol: f64) {
        assert!(
            (got - want).abs() < tol,
            "{name}: got {got}, want {want} (tol {tol})"
        );
    }

    #[test]
    fn golden_normalized_entropy() {
        check("uniform4", normalized_entropy(&[0.25, 0.25, 0.25, 0.25]), 1.0, 1e-9);
        check("certain", normalized_entropy(&[1.0, 0.0, 0.0, 0.0]), 4.982892142331044e-09, 1e-12);
        check(
            "0.7_0.1_0.1_0.1",
            normalized_entropy(&[0.7, 0.1, 0.1, 0.1]),
            0.6783898247235197,
            1e-12,
        );
        check(
            "0.4_0.3_0.2_0.1",
            normalized_entropy(&[0.4, 0.3, 0.2, 0.1]),
            0.9232196723355077,
            1e-12,
        );
    }

    #[test]
    fn golden_confidence() {
        check("case1", calculate_confidence(0.25, 1.0), 0.15, 1e-12);
        check("case2", calculate_confidence(1.0, 0.0), 1.0, 1e-12);
        check("case3", calculate_confidence(0.7, 0.3), 0.6159999999999999, 1e-9);
        check("case4", calculate_confidence(0.5, 0.5), 0.4, 1e-12);
    }

    /// Minimal `RegimeEngine<2, 4>` with a 2-state-only registry (unused states classify as
    /// "missing profile" per `classify_regime`'s own fallback) -- enough to exercise
    /// `classify_regime` directly without a real trained model, same hand-constructed-fixture
    /// pattern used throughout this crate.
    fn make_engine() -> RegimeEngine<2, 4> {
        // Distinct per-state means -- breaks emission-density ties so `step()`'s `argmax` reliably
        // selects a real, registry-covered state for a given observation (an earlier all-identical
        // fixture caused every observation to tie across all 4 states, and `max_by`'s "last
        // element wins a tie" semantics then always picked the one state NOT in a 2-entry
        // registry, silently skipping the velocity-EMA code path in every step()-based test).
        let model = StudentTHmm::<2, 4>::new_unweighted(
            [[0.0, 0.0], [5.0, 5.0], [-5.0, -5.0], [10.0, -10.0]],
            [[1.0; 2]; 4],
            [[0.25; 4]; 4],
            [0.25; 4],
            [10.0; 4],
            2.1,
            100.0,
        );
        let registry = vec![
            RegimeProfile {
                raw_id: 0,
                market_climate: MarketClimate::GaussianStable,
                shannon_entropy: 0.1,
                taleb_fragility: 0.1,
                pareto_thrust: 0.1,
                sample_share: 0.3,
                mean_tenure_bars: 10.0,
            },
            RegimeProfile {
                raw_id: 1,
                market_climate: MarketClimate::ParetoMomentum,
                shannon_entropy: 0.2,
                taleb_fragility: 0.2,
                pareto_thrust: 0.8,
                sample_share: 0.2,
                mean_tenure_bars: 5.0,
            },
            RegimeProfile {
                raw_id: 2,
                market_climate: MarketClimate::CoiledSpring,
                shannon_entropy: 0.1,
                taleb_fragility: 0.1,
                pareto_thrust: 0.1,
                sample_share: 0.3,
                mean_tenure_bars: 8.0,
            },
            RegimeProfile {
                raw_id: 3,
                market_climate: MarketClimate::TalebianFragile,
                shannon_entropy: 0.6,
                taleb_fragility: 0.9,
                pareto_thrust: 0.1,
                sample_share: 0.2,
                mean_tenure_bars: 4.0,
            },
        ];
        RegimeEngine::new(model, registry)
    }

    #[test]
    fn golden_classify_plain_pass_through() {
        let mut eng = make_engine();
        let entropy_norm = normalized_entropy(&[0.7, 0.1, 0.1, 0.1]);
        let ctx = StepContext { tail_index: 4.0, liq_fragility: 0.0, burstiness_index: 0.001 };
        let (_, _, _, _, climate) = eng.classify_regime(0, entropy_norm, &ctx);
        assert_eq!(climate, MarketClimate::GaussianStable);
        check("velocity_ema", eng.velocity_ema, 0.00020000000949949026, 1e-9);
        assert!(!eng.velocity_deadzone_active);
    }

    #[test]
    fn golden_classify_p2_4_override() {
        let mut eng = make_engine();
        let entropy_norm = normalized_entropy(&[0.7, 0.1, 0.1, 0.1]);
        let ctx = StepContext { tail_index: 1.0, liq_fragility: 5.0, burstiness_index: 0.001 };
        let (_, _, _, _, climate) = eng.classify_regime(0, entropy_norm, &ctx);
        assert_eq!(climate, MarketClimate::TalebianFragile);
    }

    #[test]
    fn golden_classify_p2_4b_override() {
        let mut eng = make_engine();
        let entropy_norm = normalized_entropy(&[0.7, 0.1, 0.1, 0.1]);
        let ctx = StepContext { tail_index: 3.0, liq_fragility: 0.5, burstiness_index: 3.0 };
        let (_, _, _, _, climate) = eng.classify_regime(0, entropy_norm, &ctx);
        assert_eq!(climate, MarketClimate::ParetoMomentum);
        check("velocity_ema", eng.velocity_ema, 0.6000000000000001, 1e-9);
    }

    #[test]
    fn golden_classify_entropy_chaos_override() {
        let mut eng = make_engine();
        let entropy_norm = normalized_entropy(&[0.3, 0.3, 0.2, 0.2]);
        let ctx = StepContext { tail_index: 4.0, liq_fragility: 0.0, burstiness_index: 0.001 };
        // State 1's static profile is ParetoMomentum -- entropy-chaos must override it anyway.
        let (_, _, _, _, climate) = eng.classify_regime(1, entropy_norm, &ctx);
        assert_eq!(climate, MarketClimate::ShannonChaos);
    }

    #[test]
    fn golden_classify_velocity_deadzone_enter_persist_exit() {
        let mut eng = make_engine();
        let entropy_norm = normalized_entropy(&[0.7, 0.1, 0.1, 0.1]);

        let ctx1 = StepContext { tail_index: 4.0, liq_fragility: 0.0, burstiness_index: 0.0 };
        let (_, _, _, _, c1) = eng.classify_regime(1, entropy_norm, &ctx1);
        assert_eq!(c1, MarketClimate::ShannonChaos, "step1: deadzone must enter and override");
        assert!(eng.velocity_deadzone_active);

        let ctx2 = StepContext { tail_index: 4.0, liq_fragility: 0.0, burstiness_index: 0.00001 };
        let (_, _, _, _, c2) = eng.classify_regime(1, entropy_norm, &ctx2);
        assert_eq!(c2, MarketClimate::ShannonChaos, "step2: still below exit threshold");
        assert!(eng.velocity_deadzone_active);

        let ctx3 = StepContext { tail_index: 4.0, liq_fragility: 0.0, burstiness_index: 1.0 };
        let (_, _, _, _, c3) = eng.classify_regime(1, entropy_norm, &ctx3);
        assert_eq!(c3, MarketClimate::ParetoMomentum, "step3: exits deadzone, resolves to profile");
        assert!(!eng.velocity_deadzone_active);
    }

    #[test]
    fn golden_classify_missing_profile_defaults_to_shannon_chaos() {
        let mut eng = make_engine();
        let entropy_norm = normalized_entropy(&[0.25, 0.25, 0.25, 0.25]);
        let ctx = StepContext::default();
        let (_, _, _, _, climate) = eng.classify_regime(9, entropy_norm, &ctx);
        assert_eq!(climate, MarketClimate::ShannonChaos);
    }

    // ------------------------------------------------------------------------------------------
    // Rule-method / multiplier-table tests (hand-verified directly against `scoring.py`'s own
    // dict literals, not a live Python run -- these are static table lookups, not algorithms).
    // ------------------------------------------------------------------------------------------

    fn state_with_climate(climate: MarketClimate, profile_label: MarketClimate) -> RegimeState<4> {
        RegimeState {
            state_id: 0,
            profile_label,
            profile_shannon_entropy: 0.0,
            profile_taleb_fragility: 0.0,
            profile_pareto_thrust: 0.0,
            climate,
            unknown_posture: false,
            probability_vector: [0.25; 4],
            entropy: 0.0,
            confidence: 1.0,
            transition_risk: 0.1,
            dof: 10.0,
            mahalanobis_distance: 0.0,
            tail_weight: 1.0,
            expected_duration: 10.0,
            variance_ratio: 1.0,
            model_drift_alert: false,
            fim_distance_min: 0.0,
            fim_nearest_state: -1,
            black_swan_alert: false,
            duration_bars: 1,
            duration_ratio: 0.1,
        }
    }

    #[test]
    fn hmm_multiplier_keys_on_profile_label_not_climate() {
        // profile_label=ParetoMomentum, climate=GaussianStable (post-override divergence) --
        // get_hmm_multiplier must read profile_label (0.85 for momentum_pinball), NOT climate.
        let s = state_with_climate(MarketClimate::GaussianStable, MarketClimate::ParetoMomentum);
        check("hmm_mult", s.get_hmm_multiplier(PatternType::MomentumPinball), 0.85, 1e-12);
        // get_climate_multiplier must read climate (1.05 for momentum_pinball @ GaussianStable).
        check("climate_mult", s.get_climate_multiplier(PatternType::MomentumPinball), 1.05, 1e-12);
    }

    #[test]
    fn unknown_pattern_defaults_neutral() {
        let s = state_with_climate(MarketClimate::ParetoMomentum, MarketClimate::ParetoMomentum);
        check("hmm", s.get_hmm_multiplier(PatternType::Unknown), 1.0, 1e-12);
        check("climate", s.get_climate_multiplier(PatternType::Unknown), 1.0, 1e-12);
    }

    #[test]
    fn climate_conviction_and_risk_and_half_life_tables() {
        let s = state_with_climate(MarketClimate::ParetoMomentum, MarketClimate::ParetoMomentum);
        check("conviction", s.get_climate_conviction_boost(), 1.2, 1e-12);
        check("risk", s.get_climate_risk_multiplier(), 1.2, 1e-12);
        check("half_life", s.get_half_life_scale(), 0.5, 1e-12);
        assert!(s.is_conviction_amplifier_eligible());
        assert!(!s.is_hard_stop_climate());

        let fragile = state_with_climate(MarketClimate::TalebianFragile, MarketClimate::TalebianFragile);
        check("risk_fragile", fragile.get_climate_risk_multiplier(), 0.0, 1e-12);
        assert!(fragile.is_hard_stop_climate());
    }

    #[test]
    fn momentum_and_mean_reversion_alignment() {
        let pareto = state_with_climate(MarketClimate::ParetoMomentum, MarketClimate::ParetoMomentum);
        assert!(pareto.is_momentum_aligned(true));
        assert!(!pareto.is_momentum_aligned(false), "short thesis hostile to PARETO_MOMENTUM");

        let stable = state_with_climate(MarketClimate::GaussianStable, MarketClimate::GaussianStable);
        assert!(stable.is_momentum_aligned(true));
        assert!(stable.is_momentum_aligned(false));

        let fragile = state_with_climate(MarketClimate::TalebianFragile, MarketClimate::TalebianFragile);
        assert!(fragile.is_mean_reversion_aligned(false));
        assert!(!fragile.is_mean_reversion_aligned(true), "long thesis hostile to TALEBIAN_FRAGILE");

        let chaos = state_with_climate(MarketClimate::ShannonChaos, MarketClimate::ShannonChaos);
        assert!(!chaos.is_momentum_aligned(true));
        assert!(!chaos.is_mean_reversion_aligned(true));
    }

    #[test]
    fn hostile_regime_change() {
        let entry = state_with_climate(MarketClimate::GaussianStable, MarketClimate::GaussianStable);
        let now_fragile = state_with_climate(MarketClimate::TalebianFragile, MarketClimate::TalebianFragile);
        let now_pareto = state_with_climate(MarketClimate::ParetoMomentum, MarketClimate::ParetoMomentum);
        let now_same = state_with_climate(MarketClimate::GaussianStable, MarketClimate::GaussianStable);

        assert!(now_fragile.is_hostile_change_from(&entry, true));
        assert!(!now_fragile.is_hostile_change_from(&entry, false));
        assert!(now_pareto.is_hostile_change_from(&entry, false));
        assert!(!now_pareto.is_hostile_change_from(&entry, true));
        assert!(!now_same.is_hostile_change_from(&entry, true), "no change at all is never hostile");
    }

    // ------------------------------------------------------------------------------------------
    // FIM distance / variance ratio / duration tracking -- hand-verified against the ported
    // formula's own closed-form arithmetic (not a live Python run: pure, already-cited math).
    // ------------------------------------------------------------------------------------------

    #[test]
    fn fim_table_nearest_rival_and_symmetry_sanity() {
        // 3 well-separated 1-D states: means 0, 10, 100; equal unit variance/dof.
        let model = StudentTHmm::<1, 3>::new_unweighted(
            [[0.0], [10.0], [100.0]],
            [[1.0], [1.0], [1.0]],
            [[1.0 / 3.0; 3]; 3],
            [1.0 / 3.0; 3],
            [10.0; 3],
            2.1,
            100.0,
        );
        let table = compute_fim_table(&model);
        // State 0's nearest rival is state 1 (distance 10), not state 2 (distance 100).
        assert_eq!(table[0].1, 1);
        // State 1's nearest rival could be either neighbor (10 vs 90) -- must be state 0.
        assert_eq!(table[1].1, 0);
        // State 2's nearest rival is state 1 (distance 90), not state 0 (distance 100).
        assert_eq!(table[2].1, 1);
        let fim_scale = (10.0 + 1.0) / (10.0 + 1.0 + 2.0);
        check("dist01", table[0].0, (fim_scale * 100.0f64).sqrt(), 1e-9);
    }

    #[test]
    fn variance_ratio_matches_model_covariance_is_one() {
        // Buffer with zero variance in dim 0, matching model_var exactly in dim 1 -> ratio 1.0
        // isn't exactly representative (median of [0, 1] = 0.5), so build an alternating
        // 50-row buffer (matching VARIANCE_RATIO_WINDOW) that produces a clean, hand-checkable
        // median instead.
        let mut buffer = [[0.0; 2]; VARIANCE_RATIO_WINDOW];
        for (i, row) in buffer.iter_mut().enumerate() {
            *row = if i % 2 == 0 { [1.0, 1.0] } else { [3.0, 1.0] };
        }
        // dim0: values alternate {1,3}, mean=2, var=1.0 -- matches model_var[0]=1.0 -> ratio 1.0
        // dim1: constant 1.0, var=0.0 -- model_var[1]=1.0 -> ratio 0.0
        let model_var = [1.0, 1.0];
        let vr = compute_variance_ratio(&buffer, &model_var);
        check("median_of_1_and_0", vr, 0.5, 1e-9);
    }

    #[test]
    fn duration_tracking_via_full_step() {
        let mut eng = make_engine();
        let ctx = StepContext { tail_index: 4.0, liq_fragility: 0.0, burstiness_index: 0.001 };
        let obs = [0.0, 0.0];
        let s1 = eng.step(&obs, &ctx);
        assert!(s1.unknown_posture, "first step is burn-in");
        assert_eq!(s1.duration_bars, 1);
        let s2 = eng.step(&obs, &ctx);
        assert!(!s2.unknown_posture);
        if s2.state_id == s1.state_id {
            assert_eq!(s2.duration_bars, 2);
        } else {
            assert_eq!(s2.duration_bars, 1);
        }
    }

    #[test]
    fn is_burn_in_complete_reflects_step_count() {
        let mut eng = make_engine();
        assert!(!eng.is_burn_in_complete());
        let ctx = StepContext::default();
        eng.step(&[0.0, 0.0], &ctx);
        assert!(eng.is_burn_in_complete());
    }

    #[test]
    fn variance_ratio_ring_buffer_wraps_without_growing_and_stays_1_until_full() {
        let mut eng = make_engine();
        let ctx = StepContext { tail_index: 4.0, liq_fragility: 0.0, burstiness_index: 0.001 };
        // Fewer than VARIANCE_RATIO_WINDOW steps: variance_ratio must stay the neutral 1.0
        // default (Python's own "not enough samples yet" behavior), never computed early.
        for i in 0..VARIANCE_RATIO_WINDOW - 1 {
            let obs = [i as f64, 0.0];
            let s = eng.step(&obs, &ctx);
            check(&format!("step{i}_neutral"), s.variance_ratio, 1.0, 1e-12);
        }
        // The window-filling step and beyond: ring buffer wraps (cursor resets to 0), never
        // grows past VARIANCE_RATIO_WINDOW rows -- exercised by running well past one full cycle.
        for i in 0..VARIANCE_RATIO_WINDOW * 2 {
            let obs = [(i % 7) as f64, 0.0];
            let s = eng.step(&obs, &ctx);
            assert!(s.variance_ratio.is_finite(), "step {i}: variance_ratio must stay finite");
        }
        assert_eq!(eng.variance_ratio_filled, VARIANCE_RATIO_WINDOW, "filled count caps at the window size, never grows past it");
    }

    #[test]
    fn reset_restores_belief_and_model_risk_state_but_keeps_registry() {
        let mut eng = make_engine();
        let ctx = StepContext { tail_index: 1.0, liq_fragility: 5.0, burstiness_index: 3.0 };
        for i in 0..10 {
            eng.step(&[i as f64, 0.0], &ctx);
        }
        assert!(eng.is_burn_in_complete());
        assert!(eng.velocity_ema.abs() > 1e-9, "precondition: velocity EMA must have moved before reset");
        assert_eq!(eng.registry.len(), 4, "precondition: registry populated before reset");

        eng.reset();

        assert!(!eng.is_burn_in_complete(), "reset must re-arm burn-in");
        // Exact equality is correct here, not a numerically-derived comparison needing
        // tolerance: `reset()` directly re-assigns `self.belief = self.model.startprob`, a plain
        // copy, not a computed value.
        #[allow(clippy::float_cmp)]
        let belief_restored = eng.belief == eng.model.startprob;
        assert!(belief_restored, "reset must restore the stationary prior");
        check("velocity_ema_reset", eng.velocity_ema, 0.0, 1e-12);
        assert!(!eng.velocity_deadzone_active);
        assert_eq!(eng.variance_ratio_filled, 0);
        assert_eq!(eng.variance_ratio_cursor, 0);
        assert_eq!(eng.variance_drift_count, 0);
        assert_eq!(eng.tail_risk_streak, 0);
        assert_eq!(eng.current_regime_id, -1);
        assert_eq!(eng.current_regime_duration, 0);
        // The loaded model/registry/fim_table are untouched by reset (expensive to reload, and
        // model-derived rather than session state) -- same scope as Python's `reset_belief()`.
        assert_eq!(eng.registry.len(), 4, "registry must survive reset");
    }
}
