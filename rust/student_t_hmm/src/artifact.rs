//! artifact.rs -- save/load a trained `StudentTHmm<D, K>` + its `registry.rs` classification as a
//! single, human-readable, Python-readable JSON manifest file.
//!
//! Replaces `models/hmm_model.pkl` (a pickle, unreadable from Rust and no longer the right
//! deployment format once training itself moves to Rust, decision #18).
//!
//! **Format resolved here (G0(c)/open question #14), 2026-10-11 -- confirms, then simplifies, the
//! spec's original "manifest + separate flat little-endian float64 weights file" proposal:**
//!
//! 1. **`bincode` was rejected, Python-readability stays a real requirement, not a stale one.**
//!    Checked directly, not assumed: `bincode` is Rust-only, which only stops mattering if NO
//!    Python consumer ever needs to read the trained model's raw parameters again. A grep across
//!    every `lbrnet` file that loads `models/hmm_model.pkl` found ~18 consumers; most ARE the HMM's
//!    own training/calibration/audit tooling decision #18 retires (`train_student_t_hmm.py`,
//!    `calibrate_hmm_gate_thresholds.py`, `smart_tuner.py`, `tune_risk_gate.py`,
//!    `audit_hmm_release.py`, `hmm_utils.py`, `replay_hmm_gate_compare.py`,
//!    `run_feasibility_cycle.py`) or pure file-ops with no read dependency on the format
//!    (`promote_live_models.py`) or already bypass the pickle entirely
//!    (`backtest/parquet_event_stream.py`'s own docstring: "Regime state is pre-built from stored
//!    HMM outputs in the parquet, bypassing HMM inference entirely"). But `train_event_transformer.py`
//!    and `tune_event_transformer.py` are confirmed REAL, ONGOING, non-retiring consumers --
//!    the Transformer itself stays Python/TensorFlow (decision #22), and `train_event_transformer.py`
//!    directly `joblib.load()`s the HMM artifact to compute the Kazemi et al. (2019)
//!    volatility-adjusted-time `sigma_eff` feature from the model's own covariances. Rejecting
//!    `bincode` for these two files alone is sufficient justification on its own.
//! 2. **Simplified further than the original proposal, 2026-10-11**: given `PRODUCTION_D=18`/
//!    `PRODUCTION_K=4`, the full parameter count (means + covars + transmat + startprob + dof +
//!    feature_weights) is on the order of 190 `f64`s -- a few KB as plain JSON numbers. A separate
//!    binary weights file with its own header format is complexity this tiny a payload doesn't
//!    earn; one JSON file (dims, weights, classification, calibration fields all together) is
//!    simpler, equally Python-readable (`json.load`, no `numpy.frombuffer` needed at all), and
//!    just as debuggable by hand. Revisit only if `D`/`K` ever grow by orders of magnitude (not
//!    expected -- `PRODUCTION_K` is production-locked per the runbook's own Smart Tuner V1 frozen
//!    contract, and `D`'s own growth is bounded by the observation-vector's own dimension debate).
//!
//! **Content-hash fingerprinting (decision #25's mandatory train -> audit -> calibrate chain)**:
//! `model_sha256` is a SHA-256 over a canonical byte serialization of ONLY the trained model's own
//! core parameters (dims, means, covars, transmat, startprob, dof, dof bounds, feature weights) --
//! never the classification/calibration fields layered on top, so recomputing it after calibration
//! lands still reproduces the same hash. A calibration artifact's own provenance record embeds
//! this exact value (not yet implemented here -- `calibration.rs`'s own concern once G0(e) lands),
//! giving the atomic-write-together provenance check decision #25 designed a concrete anchor to
//! compare against.

use crate::model::StudentTHmm;
use crate::registry::RegimeProfile;
use serde::{Deserialize, Serialize};
use sha2::{Digest, Sha256};
use std::fs;
use std::path::Path;

/// The trained model's own core parameters, in a plain (non-const-generic) shape `serde` can
/// derive for directly -- `model.rs`'s `StudentTHmm<D, K>` itself stays serde-free (its own header
/// comment's zero-hot-path-dependency guarantee); this is a deliberate data-transfer
/// representation, not a parallel "real" type. `Vec`/heap allocation here is fine: artifact I/O is
/// explicitly offline, once-per-training-run tooling, not hot-path code.
#[derive(Debug, Clone, Serialize, Deserialize)]
struct ModelParams {
    d: usize,
    k: usize,
    means: Vec<Vec<f64>>,
    covars: Vec<Vec<f64>>,
    transmat: Vec<Vec<f64>>,
    startprob: Vec<f64>,
    dof: Vec<f64>,
    dof_min: f64,
    dof_max: f64,
    feature_weights: Vec<f64>,
}

/// The full manifest -- model parameters, their content hash, and the `registry.rs`
/// classification cascade's own per-state output. Calibration-threshold/provenance fields
/// (decision #25) are deliberately NOT included yet -- `calibration.rs` (G0(e)) owns writing
/// those into this same manifest once it exists; adding an empty placeholder now would commit to
/// a shape before that module's own real needs are known.
#[derive(Debug, Clone, Serialize, Deserialize)]
pub struct ModelArtifact {
    model_sha256: String,
    params: ModelParams,
    profiles: Vec<RegimeProfile>,
}

/// Errors `save`/`load` can return -- deliberately NOT `panic!`/`unwrap()` on a dimension
/// mismatch (a genuinely likely operator error: loading a K=5 manifest against a K=4-compiled
/// binary) or a missing/corrupt file; this is offline tooling, but still must fail loudly and
/// specifically, not silently load garbage.
#[derive(Debug)]
pub enum ArtifactError {
    Io(std::io::Error),
    Json(serde_json::Error),
    /// Manifest's own recorded `d`/`k` don't match the caller's compile-time `D`/`K`.
    DimensionMismatch { manifest_d: usize, manifest_k: usize, expected_d: usize, expected_k: usize },
}

impl std::fmt::Display for ArtifactError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            Self::Io(e) => write!(f, "artifact I/O error: {e}"),
            Self::Json(e) => write!(f, "artifact JSON error: {e}"),
            Self::DimensionMismatch { manifest_d, manifest_k, expected_d, expected_k } => write!(
                f,
                "artifact dimension mismatch: manifest has D={manifest_d}/K={manifest_k}, caller expected D={expected_d}/K={expected_k}"
            ),
        }
    }
}

impl std::error::Error for ArtifactError {}

impl From<std::io::Error> for ArtifactError {
    fn from(e: std::io::Error) -> Self {
        Self::Io(e)
    }
}

impl From<serde_json::Error> for ArtifactError {
    fn from(e: serde_json::Error) -> Self {
        Self::Json(e)
    }
}

/// SHA-256 over a canonical (field-order-stable, since every field here is a `Vec`/scalar, never
/// a `HashMap`) JSON serialization of `params` alone -- the "model identity" decision #25's
/// provenance design anchors calibration artifacts to.
fn hash_params(params: &ModelParams) -> Result<String, ArtifactError> {
    let canonical = serde_json::to_vec(params)?;
    let mut hasher = Sha256::new();
    hasher.update(&canonical);
    Ok(format!("{:x}", hasher.finalize()))
}

/// Serializes `model` + `profiles` to a pretty-printed JSON manifest at `path`. Returns the
/// computed `model_sha256` so a caller (the eventual train -> audit -> calibrate orchestration,
/// decision #25) can embed it directly in a paired calibration-provenance record without
/// re-reading the file back to recompute it.
pub fn save<const D: usize, const K: usize>(
    path: &Path,
    model: &StudentTHmm<D, K>,
    profiles: &[RegimeProfile],
) -> Result<String, ArtifactError> {
    let params = ModelParams {
        d: D,
        k: K,
        means: model.means.iter().map(|row| row.to_vec()).collect(),
        covars: model.covars.iter().map(|row| row.to_vec()).collect(),
        transmat: model.transmat.iter().map(|row| row.to_vec()).collect(),
        startprob: model.startprob.to_vec(),
        dof: model.dof.to_vec(),
        dof_min: model.dof_min,
        dof_max: model.dof_max,
        feature_weights: model.feature_weights.to_vec(),
    };
    let model_sha256 = hash_params(&params)?;
    let artifact = ModelArtifact { model_sha256: model_sha256.clone(), params, profiles: profiles.to_vec() };

    let json = serde_json::to_string_pretty(&artifact)?;
    fs::write(path, json)?;
    Ok(model_sha256)
}

/// Loads a manifest from `path`, verifying its recorded `d`/`k` match the caller's compile-time
/// `D`/`K` before constructing the model -- a genuine, not-hypothetical failure mode (loading an
/// artifact trained at a different `D`/`K` than the binary was compiled for) this function refuses
/// outright rather than silently truncating/padding arrays to fit.
pub fn load<const D: usize, const K: usize>(path: &Path) -> Result<(StudentTHmm<D, K>, Vec<RegimeProfile>), ArtifactError> {
    let json = fs::read_to_string(path)?;
    let artifact: ModelArtifact = serde_json::from_str(&json)?;
    let p = &artifact.params;

    if p.d != D || p.k != K {
        return Err(ArtifactError::DimensionMismatch {
            manifest_d: p.d,
            manifest_k: p.k,
            expected_d: D,
            expected_k: K,
        });
    }

    let to_fixed_k_d = |rows: &[Vec<f64>]| -> [[f64; D]; K] {
        std::array::from_fn(|k| std::array::from_fn(|d| rows[k][d]))
    };
    let to_fixed_k_k = |rows: &[Vec<f64>]| -> [[f64; K]; K] {
        std::array::from_fn(|i| std::array::from_fn(|j| rows[i][j]))
    };
    let to_fixed_k = |xs: &[f64]| -> [f64; K] { std::array::from_fn(|i| xs[i]) };
    let to_fixed_d = |xs: &[f64]| -> [f64; D] { std::array::from_fn(|i| xs[i]) };

    let model = StudentTHmm::new(
        to_fixed_k_d(&p.means),
        to_fixed_k_d(&p.covars),
        to_fixed_k_k(&p.transmat),
        to_fixed_k(&p.startprob),
        to_fixed_k(&p.dof),
        p.dof_min,
        p.dof_max,
        to_fixed_d(&p.feature_weights),
    );

    Ok((model, artifact.profiles))
}

#[cfg(test)]
#[allow(clippy::unwrap_used)]
mod tests {
    use super::*;
    use crate::registry::MarketClimate;

    fn sample_model() -> StudentTHmm<2, 2> {
        StudentTHmm::new(
            [[0.1, 0.2], [3.0, 3.0]],
            [[0.05, 0.05], [0.06, 0.06]],
            [[0.9, 0.1], [0.2, 0.8]],
            [0.5, 0.5],
            [6.0, 7.0],
            2.1,
            100.0,
            [1.0, 1.4],
        )
    }

    fn sample_profiles() -> Vec<RegimeProfile> {
        vec![
            RegimeProfile {
                raw_id: 0,
                market_climate: MarketClimate::CoiledSpring,
                shannon_entropy: 0.8,
                taleb_fragility: 0.1,
                pareto_thrust: 0.05,
                sample_share: 0.4,
                mean_tenure_bars: 12.0,
            },
            RegimeProfile {
                raw_id: 1,
                market_climate: MarketClimate::ParetoMomentum,
                shannon_entropy: 0.6,
                taleb_fragility: 0.2,
                pareto_thrust: 0.9,
                sample_share: 0.6,
                mean_tenure_bars: 30.0,
            },
        ]
    }

    #[test]
    fn save_then_load_round_trips_exactly() {
        let dir = std::env::temp_dir().join(format!("artifact_rs_test_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("model_manifest.json");

        let model = sample_model();
        let profiles = sample_profiles();
        let hash = save::<2, 2>(&path, &model, &profiles).unwrap();
        assert_eq!(hash.len(), 64, "SHA-256 hex digest must be 64 chars");

        let (loaded_model, loaded_profiles) = load::<2, 2>(&path).unwrap();
        assert_eq!(loaded_model, model);
        assert_eq!(loaded_profiles, profiles);

        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn hash_is_deterministic_and_excludes_profiles() {
        let model = sample_model();
        let dir = std::env::temp_dir().join(format!("artifact_rs_test_hash_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path_a = dir.join("a.json");
        let path_b = dir.join("b.json");

        let hash_a = save::<2, 2>(&path_a, &model, &sample_profiles()).unwrap();
        // Different profiles (e.g. a re-classification after a threshold tweak), SAME model params
        // -- the content hash must be unchanged, since it anchors calibration provenance to the
        // MODEL's identity, not its downstream classification.
        let different_profiles = vec![RegimeProfile {
            raw_id: 0,
            market_climate: MarketClimate::ShannonChaos,
            shannon_entropy: 0.99,
            taleb_fragility: 0.0,
            pareto_thrust: 0.0,
            sample_share: 1.0,
            mean_tenure_bars: 1.0,
        }];
        let hash_b = save::<2, 2>(&path_b, &model, &different_profiles).unwrap();
        assert_eq!(hash_a, hash_b, "model_sha256 must depend only on model params, not profiles");

        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn load_rejects_dimension_mismatch() {
        let dir = std::env::temp_dir().join(format!("artifact_rs_test_dim_{}", std::process::id()));
        std::fs::create_dir_all(&dir).unwrap();
        let path = dir.join("model_manifest.json");

        let model = sample_model(); // D=2, K=2
        save::<2, 2>(&path, &model, &sample_profiles()).unwrap();

        // Attempt to load as D=3, K=2 -- must fail loudly, not silently truncate/pad.
        let result = load::<3, 2>(&path);
        assert!(matches!(result, Err(ArtifactError::DimensionMismatch { .. })));

        std::fs::remove_dir_all(&dir).ok();
    }

    #[test]
    fn load_missing_file_returns_io_error() {
        let result = load::<2, 2>(Path::new("/nonexistent/path/that/does/not/exist.json"));
        assert!(matches!(result, Err(ArtifactError::Io(_))));
    }
}
