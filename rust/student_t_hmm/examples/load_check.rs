//! load_check -- smoke-checks `artifact::load` against a REAL (not synthetic) trained-model
//! manifest: `fixtures/hmm_model_20260918_manifest.json`, the first and only real artifact this
//! crate has ever produced so far. Converted once (2026-10-12, hmm_tool `import-legacy`, since
//! removed -- see the manifest's own provenance note below) from the real
//! `lbrnet/models/hmm_model.pkl` (a 2026-09-18 training run, D=17/K=4 -- the production input
//! shape, `fast_mean_rev_z` excluded; see `input.rs`) plus its
//! paired `ModelManifest_candidate_20260918_full.json`'s already-computed `MarketClimate`
//! classification (real `build_regime_registry()` output from the real decoded Viterbi path over
//! 21.9M real training rows -- not re-derived, carried forward as-is).
//!
//! `cargo run -p student_t_hmm --example load_check`

fn main() {
    let path = std::path::Path::new(concat!(
        env!("CARGO_MANIFEST_DIR"),
        "/fixtures/hmm_model_20260918_manifest.json"
    ));
    let (model, profiles) = student_t_hmm::artifact::load::<{ student_t_hmm::model::PRODUCTION_D }, { student_t_hmm::model::PRODUCTION_K }>(path).expect("load failed");
    println!("Loaded model: K={}, dof={:?}", profiles.len(), model.dof);
    for p in &profiles {
        println!("state {}: {:?} sample_share={:.3}", p.raw_id, p.market_climate, p.sample_share);
    }
}
