//! registry.rs -- post-training `MarketClimate` classification cascade: assigns each of a trained
//! model's K states to one of 5 archetypes, once per trained model, producing a per-state
//! `RegimeProfile`.
//!
//! Ported from `lbrnet/lbrnet/models/regime_registry.py`'s `build_regime_registry()` -- the real
//! owner of `MarketClimate` classification (confirmed, spec §5.1c/§5.1d: NOT `rc_enums.py`'s
//! deprecated lazy re-export shim, NOT the retired Hungarian-alignment/`HMMStateEnum` positional
//! scheme removed 2026-09-21). A one-time, post-training manifest-generation step -- confirmed
//! against `RegimeManager.cpp`, which only ever reads an already-computed climate value, never
//! re-derives it -- not a live per-tick classifier; that's `engine.rs`'s job (G4), a trivial array
//! lookup by `state_id` into whatever this module produces.
//!
//! Built almost entirely from functions already ported in `gate_metrics.rs`
//! (`mean_tenure_by_state`/`compute_pareto_thrust_shares`/`dof_fragility_score`/`taleb_gate_pass`),
//! per the plan's own note that this slice was "largely already prepared."
//!
//! **Deliberately out of scope for this module** (left for `artifact.rs`'s own manifest-writing
//! concern): the full `RegimeProfile.centroid` field (every feature's per-state mean, a pure
//! display/diagnostic value with no bearing on classification) and `RegimeRegistry`'s
//! `gang_validation`/`metadata` sign-off payload (a separate training-sign-off concern, not a
//! classification one). This module's `RegimeProfile` carries only the fields the classification
//! cascade itself produces and consumes.
//!
//! **One confirmed, deliberately-not-silently-resolved divergence from the Python original**: the
//! degenerate all-remaining-points-coincide case isn't relevant here (there is no RNG in this
//! module), but feature-dimension resolution (`log_scale_ratio`/`relative_range`/`lempel_ziv`
//! column indices) is handled the same way `compute_pareto_thrust_shares` already does --
//! `classify_states` takes the already-extracted per-state dimension arrays directly, not a full
//! `[[f64; D]; K]` means matrix plus a schema-derived index lookup. This crate has no dimension-
//! index convention of its own; the caller (whichever layer owns `OBSERVATION_FIELD_NAMES`) must
//! supply these three arrays already extracted.

use crate::gate_metrics::{compute_pareto_thrust_shares, dof_fragility_score, mean_tenure_by_state, taleb_gate_pass};

/// The HMM State Manager's own 5-archetype vocabulary -- ported from `regime_registry.py`'s
/// `MarketClimate` (relocated there 2026-09-22 from the now-deprecated `rc_enums.py`, confirmed
/// spec §5.1c). IDs match the Python original exactly (`GAUSSIAN_STABLE=0` through
/// `COILED_SPRING=4`) -- this ordering is a real wire/manifest contract, not incidental.
#[derive(Debug, Clone, Copy, PartialEq, Eq, serde::Serialize, serde::Deserialize)]
#[repr(u8)]
pub enum MarketClimate {
    GaussianStable = 0,
    TalebianFragile = 1,
    ShannonChaos = 2,
    ParetoMomentum = 3,
    CoiledSpring = 4,
}

impl MarketClimate {
    /// Matches Python's `MarketClimate.handle_missing()`: any out-of-range id falls back to
    /// `ShannonChaos` (maximum entropy) to force a defensive posture, rather than panicking.
    pub fn from_id(id: u8) -> Self {
        match id {
            0 => Self::GaussianStable,
            1 => Self::TalebianFragile,
            2 => Self::ShannonChaos,
            3 => Self::ParetoMomentum,
            4 => Self::CoiledSpring,
            _ => Self::ShannonChaos,
        }
    }

    pub fn label(self) -> &'static str {
        match self {
            Self::GaussianStable => "GAUSSIAN_STABLE",
            Self::TalebianFragile => "TALEBIAN_FRAGILE",
            Self::ShannonChaos => "SHANNON_CHAOS",
            Self::ParetoMomentum => "PARETO_MOMENTUM",
            Self::CoiledSpring => "COILED_SPRING",
        }
    }
}

/// One trained state's classification result -- the fields `build_regime_registry()`'s cascade
/// itself produces and consumes. Does NOT include Python's `centroid`/`alignment_cost` fields:
/// `centroid` is a pure display value (every feature's per-state mean) with no bearing on
/// classification, left to `artifact.rs`'s manifest-writing concern; `alignment_cost` is
/// confirmed dead in the Python original (always 0.0, kept only for older-manifest schema
/// compatibility) and not ported at all.
#[derive(Debug, Clone, Copy, PartialEq, serde::Serialize, serde::Deserialize)]
pub struct RegimeProfile {
    pub raw_id: usize,
    pub market_climate: MarketClimate,
    pub shannon_entropy: f64,
    pub taleb_fragility: f64,
    pub pareto_thrust: f64,
    pub sample_share: f64,
    pub mean_tenure_bars: f64,
}

fn clip01(value: f64) -> f64 {
    value.clamp(0.0, 1.0)
}

fn median(sorted_copy_source: &[f64]) -> f64 {
    if sorted_copy_source.is_empty() {
        return 0.0;
    }
    let mut xs: Vec<f64> = sorted_copy_source.to_vec();
    xs.sort_by(|a, b| a.partial_cmp(b).unwrap_or(std::cmp::Ordering::Equal));
    let n = xs.len();
    if n % 2 == 1 {
        xs[n / 2]
    } else {
        0.5 * (xs[n / 2 - 1] + xs[n / 2])
    }
}

fn std_dev(xs: &[f64], mean_override: f64) -> f64 {
    if xs.is_empty() {
        return 0.0;
    }
    let n = xs.len() as f64;
    let var = xs.iter().map(|&x| (x - mean_override).powi(2)).sum::<f64>() / n;
    var.sqrt()
}

/// Bounds/thresholds the classification cascade needs beyond the per-state arrays themselves --
/// mirrors `regime_registry.py`'s `HMMTrainingConfig`-derived defaults
/// (`pareto_top_state_ratio_max` is gang-validation-only, not used here; these three ARE).
#[derive(Debug, Clone, Copy)]
pub struct ClassifyConfig {
    pub dof_min: f64,
    pub dof_max: f64,
    /// `dof_boundary_check`/`taleb_gate_pass`'s pinning tolerance (Python default 0.05).
    pub epsilon: f64,
    /// `state_occupancy_balance_check`/`taleb_gate_pass`'s degenerate-state floor (Python default 0.03).
    pub min_share_floor: f64,
}

impl Default for ClassifyConfig {
    fn default() -> Self {
        Self {
            dof_min: crate::model::DEFAULT_DOF_MIN,
            dof_max: crate::model::DEFAULT_DOF_MAX,
            epsilon: crate::gate_metrics::DEFAULT_EPSILON,
            min_share_floor: crate::gate_metrics::DEFAULT_MIN_SHARE_FLOOR,
        }
    }
}

/// Classifies every one of `n_states` states into a `RegimeProfile`, ported verbatim from
/// `build_regime_registry()`'s priority-ordered if/elif cascade (lines ~487-527 as read
/// 2026-10-11), including the "classification with a reject option" (Chow 1970) fix: each branch
/// is tried in order, the first match wins, and a state matching none of them falls back to
/// `ShannonChaos` (max-entropy, most risk-averse) rather than defaulting to `GaussianStable`.
///
/// `states` is the model's decoded Viterbi state sequence (drives `sample_share`/`mean_tenure_bars`
/// via `gate_metrics.rs`'s already-ported helpers); `dof_by_state`/`log_scale_ratio_by_state`/
/// `relative_range_by_state`/`lempel_ziv_by_state` are each length `n_states`, one value per
/// state -- the per-state MEAN of that raw observation dimension (`means[raw_id, idx]` in the
/// Python original), already extracted by the caller.
// Mixed indexing across several same-length-`n_states` arrays in the loop below -- a loop index,
// not `.enumerate()`, matches this crate's own established convention (`model.rs`/`train.rs`).
#[allow(clippy::needless_range_loop)]
pub fn classify_states(
    states: &[i32],
    dof_by_state: &[f64],
    log_scale_ratio_by_state: &[f64],
    relative_range_by_state: &[f64],
    lempel_ziv_by_state: &[f64],
    n_states: usize,
    config: &ClassifyConfig,
) -> Vec<RegimeProfile> {
    let mut counts = vec![0.0f64; n_states];
    for &s in states {
        if s >= 0 && (s as usize) < n_states {
            counts[s as usize] += 1.0;
        }
    }
    let total_count = states.len().max(1) as f64;
    let sample_share: Vec<f64> = counts.iter().map(|&c| clip01(c / total_count)).collect();
    let tenure = mean_tenure_by_state(states, n_states);

    let median_logscale = median(log_scale_ratio_by_state);
    let median_abs_relrange = median(&relative_range_by_state.iter().map(|&v| v.abs()).collect::<Vec<f64>>());
    let median_lz = median(lempel_ziv_by_state);
    let std_lz = std_dev(lempel_ziv_by_state, median_lz);
    let safe_std_lz = if std_lz > 1e-9 { std_lz } else { 1.0 };

    let pareto_share = compute_pareto_thrust_shares(&counts, relative_range_by_state);
    let gate = taleb_gate_pass(dof_by_state, &sample_share, config.dof_min, config.dof_max, config.epsilon, config.min_share_floor);

    let mut profiles = Vec::with_capacity(n_states);
    for raw_id in 0..n_states {
        let shannon_score = clip01(tenure.get(raw_id).copied().unwrap_or(0.0) / 5.0);
        let taleb_score = dof_fragility_score(dof_by_state[raw_id]);
        let pareto_score = clip01(pareto_share[raw_id] / 0.70);

        let entropy_z = (lempel_ziv_by_state[raw_id] - median_lz) / safe_std_lz;
        let low_vol = log_scale_ratio_by_state[raw_id] < median_logscale;
        let low_range = relative_range_by_state[raw_id].abs() <= median_abs_relrange;

        let climate = if gate.fragile_states.contains(&raw_id) {
            MarketClimate::TalebianFragile
        } else if shannon_score < 0.35 || entropy_z > 1.0 {
            MarketClimate::ShannonChaos
        } else if pareto_score >= 0.50 && relative_range_by_state[raw_id] > 0.0 {
            MarketClimate::ParetoMomentum
        } else if low_vol && low_range {
            MarketClimate::CoiledSpring
        } else if low_vol || low_range {
            MarketClimate::GaussianStable
        } else {
            MarketClimate::ShannonChaos
        };

        profiles.push(RegimeProfile {
            raw_id,
            market_climate: climate,
            shannon_entropy: shannon_score,
            taleb_fragility: taleb_score,
            pareto_thrust: pareto_score,
            sample_share: sample_share[raw_id],
            mean_tenure_bars: tenure.get(raw_id).copied().unwrap_or(0.0),
        });
    }

    profiles
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;

    const EPS: f64 = 1e-9;

    fn assert_close(label: &str, got: f64, want: f64, eps: f64) {
        assert!(
            (got - want).abs() <= eps * want.abs().max(1.0),
            "{label}: got {got}, want {want} (diff {})",
            (got - want).abs()
        );
    }

    /// Golden-parity fixture: 6 states, one real invocation of the real Python
    /// `build_regime_registry()` (mts conda env, 2026-10-11) against a hand-constructed fake
    /// model object (real function, synthetic model data) -- deliberately engineered so each
    /// state lands in a DIFFERENT one of the cascade's 6 branches (5 climates + the reject-option
    /// fallback), not just the common/easy path.
    #[test]
    fn golden_parity_classify_states_all_six_branches() {
        let dof_by_state = [2.5, 50.0, 50.0, 50.0, 50.0, 50.0];
        let log_scale_ratio_by_state = [0.5, 0.4, 0.6, -0.5, -0.3, 0.55];
        let relative_range_by_state = [0.3, -0.1, 0.9, 0.05, 0.6, 0.55];
        let lempel_ziv_by_state = [0.2, 5.0, 0.3, 0.25, 0.25, 0.25];

        let states: Vec<i32> = {
            let runs: [(i32, usize); 10] =
                [(0, 20), (1, 2), (2, 15), (3, 30), (4, 18), (5, 10), (1, 1), (2, 5), (4, 3), (5, 2)];
            runs.iter().flat_map(|&(state, count)| std::iter::repeat_n(state, count)).collect()
        };

        let config = ClassifyConfig { dof_min: 2.1, dof_max: 100.0, epsilon: 0.05, min_share_floor: 0.03 };
        let profiles = classify_states(
            &states,
            &dof_by_state,
            &log_scale_ratio_by_state,
            &relative_range_by_state,
            &lempel_ziv_by_state,
            6,
            &config,
        );

        assert_eq!(profiles.len(), 6);

        let want: [(MarketClimate, f64, f64, f64, f64, f64); 6] = [
            (MarketClimate::TalebianFragile, 1.0, 0.375, 0.1904761904761905, 0.18867924528301888, 20.0),
            (MarketClimate::ShannonChaos, 0.3, 0.0, 0.009523809523809526, 0.02830188679245283, 1.5),
            (MarketClimate::ParetoMomentum, 1.0, 0.0, 0.5714285714285715, 0.18867924528301888, 10.0),
            (MarketClimate::CoiledSpring, 1.0, 0.0, 0.04761904761904762, 0.2830188679245283, 30.0),
            (MarketClimate::GaussianStable, 1.0, 0.0, 0.39999999999999997, 0.19811320754716982, 10.5),
            (MarketClimate::ShannonChaos, 1.0, 0.0, 0.20952380952380953, 0.11320754716981132, 6.0),
        ];

        for (raw_id, (want_climate, want_shannon, want_taleb, want_pareto, want_share, want_tenure)) in
            want.into_iter().enumerate()
        {
            let p = &profiles[raw_id];
            assert_eq!(p.market_climate, want_climate, "state {raw_id} climate mismatch: {:?}", p);
            assert_close(&format!("state{raw_id}.shannon_entropy"), p.shannon_entropy, want_shannon, EPS);
            assert_close(&format!("state{raw_id}.taleb_fragility"), p.taleb_fragility, want_taleb, EPS);
            assert_close(&format!("state{raw_id}.pareto_thrust"), p.pareto_thrust, want_pareto, EPS);
            assert_close(&format!("state{raw_id}.sample_share"), p.sample_share, want_share, EPS);
            assert_close(&format!("state{raw_id}.mean_tenure_bars"), p.mean_tenure_bars, want_tenure, EPS);
        }
    }

    #[test]
    fn market_climate_handle_missing_falls_back_to_shannon_chaos() {
        assert_eq!(MarketClimate::from_id(99), MarketClimate::ShannonChaos);
        assert_eq!(MarketClimate::from_id(255), MarketClimate::ShannonChaos);
    }

    #[test]
    fn market_climate_labels_match_python_names() {
        assert_eq!(MarketClimate::GaussianStable.label(), "GAUSSIAN_STABLE");
        assert_eq!(MarketClimate::TalebianFragile.label(), "TALEBIAN_FRAGILE");
        assert_eq!(MarketClimate::ShannonChaos.label(), "SHANNON_CHAOS");
        assert_eq!(MarketClimate::ParetoMomentum.label(), "PARETO_MOMENTUM");
        assert_eq!(MarketClimate::CoiledSpring.label(), "COILED_SPRING");
    }

    #[test]
    fn classify_states_empty_input_returns_empty() {
        let config = ClassifyConfig::default();
        let profiles = classify_states(&[], &[], &[], &[], &[], 0, &config);
        assert!(profiles.is_empty());
    }
}
