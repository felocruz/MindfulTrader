//! The HMM's input contract: which `ObservationData` fields the model consumes, in what order.
//!
//! The wire schema (`ObservationData`, `../schema/mts_schema.fbs`) stays 18D. The HMM consumes 17
//! of those fields: `fast_mean_rev_z` (schema index 17) is retired -- a constant 0.0 since
//! 2026-10-13 (spec decision #38), and already excluded from the 2026-09-18 production training
//! run (`lbrnet/models/hmm_utils.py` `_EXCLUDED_FIELD_NAMES`). A constant input carries no
//! information and is not neutral: the Student-t emission counts it in the true dimension `D`
//! (the `(nu + D)` terms), so keeping it would bias the degrees-of-freedom estimates (spec
//! decision #41).
//!
//! Every boundary that turns an 18D schema vector into model input -- live `hmm_step` (`rust/ffi`)
//! and offline training (`rust/tools` `hmm_tool train`) -- goes through `project` /
//! `to_model_units`, so the two can never disagree on the column set or the units.
//!
//! The values are the RAW `ObservationData` values (as `.context.parquet` stores them), not
//! `FeatureScaler` output. One unit change applies on the way in (`MODEL_UNITS`):
//! `amihud_illiquidity`'s raw scale is ~1e-8 (variance ~1.7e-15 over the 2026-09-18 training
//! file), below the model's absolute variance floor (`model::MIN_COVAR` = 1e-12) -- the floor, not
//! the data, would set its variance. A diagonal Student-t HMM is exactly equivariant to rescaling
//! one input (means x c, variances x c^2) except where that floor binds, so expressing it in units
//! of 1e-8 changes nothing but the floor's grip. (The 2026-09-18 Python run instead added 1e-4
//! jitter to every row, 2,500x amihud's own spread: that model's amihud variances are ~1e-8, the
//! jitter's, i.e. the dimension was noise.)

use crate::model::PRODUCTION_D;

/// `ObservationData`'s field count (the wire schema; unchanged by the HMM's exclusions).
pub const OBSERVATION_D: usize = 18;

/// `ObservationData`'s fields in schema order (`mts_schema.fbs`, also the `.context.parquet`
/// column names).
pub const OBSERVATION_FIELDS: [&str; OBSERVATION_D] = [
    "log_scale_ratio",
    "burstiness_index",
    "relative_range",
    "log_scale_expansion_ratio",
    "lempel_ziv",
    "hurst_exponent",
    "micro_asymmetry",
    "fisher_info",
    "fast_hurst_exponent",
    "tail_index",
    "skewness_idx",
    "amihud_illiquidity",
    "liq_fragility",
    "fast_taleb_kurtosis",
    "recurrence_rate",
    "fractal_dim",
    "mean_rev_z",
    "fast_mean_rev_z",
];

/// Schema fields the HMM does not consume.
pub const EXCLUDED_FIELDS: [&str; OBSERVATION_D - PRODUCTION_D] = ["fast_mean_rev_z"];

/// Schema index of each model input, in model order (schema order minus the exclusions).
pub const HMM_SOURCE_INDICES: [usize; PRODUCTION_D] = [0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16];

/// The model's input fields, in model order.
pub fn hmm_input_fields() -> [&'static str; PRODUCTION_D] {
    HMM_SOURCE_INDICES.map(|i| OBSERVATION_FIELDS[i])
}

/// Model index of a model input field, if the model consumes it.
pub fn model_index(field: &str) -> Option<usize> {
    hmm_input_fields().iter().position(|f| *f == field)
}

/// Multiplier from raw `ObservationData` units to model units, per model input (see module doc).
pub const MODEL_UNITS: [f64; PRODUCTION_D] = [1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1.0, 1e8, 1.0, 1.0, 1.0, 1.0, 1.0];

/// Raw values of the model's input fields (model order) -> model units.
pub fn to_model_units(raw: &[f64; PRODUCTION_D]) -> [f64; PRODUCTION_D] {
    std::array::from_fn(|j| raw[j] * MODEL_UNITS[j])
}

/// One raw 18D schema vector -> the model's input.
pub fn project(observation: &[f64; OBSERVATION_D]) -> [f64; PRODUCTION_D] {
    to_model_units(&HMM_SOURCE_INDICES.map(|i| observation[i]))
}

/// Up-weighted ("bullet") fields of the 2026-09-18 production training run
/// (`train_student_t_hmm.py` `_BULLET_MODEL_DIMS`: the strongest cross-state differentiators);
/// every other field is weighted `MAP_WEIGHT`.
pub const BULLET_FIELDS: [&str; 4] = ["log_scale_ratio", "relative_range", "mean_rev_z", "burstiness_index"];
/// The production run's `bullet_weight` / `map_weight`.
pub const BULLET_WEIGHT: f64 = 1.8;
pub const MAP_WEIGHT: f64 = 1.0;

/// Per-feature weights for the model input, given bullet/map weights.
pub fn feature_weights(bullet_weight: f64, map_weight: f64) -> [f64; PRODUCTION_D] {
    hmm_input_fields().map(|f| if BULLET_FIELDS.contains(&f) { bullet_weight } else { map_weight })
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn source_indices_are_the_schema_minus_the_exclusions() {
        let expected: Vec<usize> = (0..OBSERVATION_D).filter(|&i| !EXCLUDED_FIELDS.contains(&OBSERVATION_FIELDS[i])).collect();
        assert_eq!(HMM_SOURCE_INDICES.to_vec(), expected);
        assert!(!hmm_input_fields().contains(&"fast_mean_rev_z"));
    }

    #[test]
    fn model_order_matches_the_production_manifest() {
        // lbrnet/models/ModelManifest_candidate_20260918_full.json metadata.feature_order.
        let manifest_order = [
            "log_scale_ratio", "burstiness_index", "relative_range", "log_scale_expansion_ratio", "lempel_ziv",
            "hurst_exponent", "micro_asymmetry", "fisher_info", "fast_hurst_exponent", "tail_index", "skewness_idx",
            "amihud_illiquidity", "liq_fragility", "fast_taleb_kurtosis", "recurrence_rate", "fractal_dim", "mean_rev_z",
        ];
        assert_eq!(hmm_input_fields(), manifest_order);
    }

    #[test]
    fn projection_drops_only_the_excluded_field_and_rescales_only_amihud() {
        let obs: [f64; OBSERVATION_D] = std::array::from_fn(|i| i as f64);
        let x = project(&obs);
        let amihud = model_index("amihud_illiquidity").expect("model input");
        let expected: Vec<f64> = (0..17).map(|i| if i == amihud { i as f64 * 1e8 } else { i as f64 }).collect();
        assert_eq!(x.map(f64::to_bits).to_vec(), expected.iter().map(|v| v.to_bits()).collect::<Vec<_>>());
        assert_eq!(MODEL_UNITS.iter().filter(|u| u.to_bits() != 1.0f64.to_bits()).count(), 1);
        // The training path (columns read by name, then units) agrees with the live path.
        assert_eq!(to_model_units(&HMM_SOURCE_INDICES.map(|i| obs[i])).map(f64::to_bits), x.map(f64::to_bits));
        assert_eq!(model_index("mean_rev_z"), Some(16));
        assert_eq!(model_index("fast_mean_rev_z"), None);
    }

    #[test]
    fn bullet_weights_land_on_the_named_fields() {
        let w = feature_weights(BULLET_WEIGHT, MAP_WEIGHT);
        for f in BULLET_FIELDS {
            assert_eq!(w[model_index(f).expect("bullet field is a model input")].to_bits(), BULLET_WEIGHT.to_bits());
        }
        assert_eq!(w.iter().filter(|&&v| v.to_bits() == BULLET_WEIGHT.to_bits()).count(), 4);
    }
}
