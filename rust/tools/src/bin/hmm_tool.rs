//! hmm_tool -- offline Student-t HMM tooling (spec §5.2, decision #41).
//!
//! `hmm_tool train` trains the production-shape model (D=17, K=4) on a `.context.parquet` file:
//! read the model's input columns by name -> refuse bad data -> the 2026-09-18 production run's
//! preparation rules (`student_t_hmm::training_data`) -> seeded k-means++ restarts, each fit with
//! the production EM configuration -> keep the best TRAINING fit (a holdout, if any, never picks)
//! -> Viterbi decode + regime classification -> `artifact::save`. Everything is reported through
//! an archived transcript plus a ledger row (`tools/output/`, `tools/RECALIBRATION_LEDGER.md`).
//!
//! Patterns borrowed from Atratus's `atratus_tool train` (never a dependency): column-by-name
//! loading, refusing non-finite data and near-floor-variance features, seeded restarts chosen by
//! training likelihood with BIC reported, and an optional holdout behind an embargo.
//!
//! `cargo run --release -p tools --bin hmm_tool -- train --help`

use std::path::{Path, PathBuf};
use std::time::Instant;

use clap::{Parser, Subcommand};
use student_t_hmm::gate_metrics::{dof_boundary_check, state_occupancy_balance_check, taleb_gate_pass, TALEB_KURTOSIS_UNDEFINED_DOF};
use student_t_hmm::init::SplitMix64;
use student_t_hmm::input::{feature_weights, hmm_input_fields, model_index, to_model_units, BULLET_WEIGHT, MAP_WEIGHT};
use student_t_hmm::model::{StudentTHmm, MIN_COVAR, PRODUCTION_D, PRODUCTION_K};
use student_t_hmm::registry::{classify_states, ClassifyConfig};
use student_t_hmm::train::{fit, forward_backward, FitResult};
use student_t_hmm::training_data::{
    drop_flat_runs, global_mean_var, initial_model, segments, session_lengths, trim, TrainingConfig, SESSION_GAP_SECONDS, TAIL_TRIM,
    WARMUP_TRIM,
};
use tools::context_parquet;
use tools::report::Report;

const D: usize = PRODUCTION_D;
const K: usize = PRODUCTION_K;
type Model = StudentTHmm<D, K>;

/// A feature whose variance is within 100x of the model's variance floor is flattened out of the
/// model by the floor, not the data (Atratus's guard): refuse it rather than train on it.
const NEAR_FLOOR_VARIANCE: f64 = 100.0 * MIN_COVAR;

#[derive(Parser)]
#[command(name = "hmm_tool", about = "Offline Student-t HMM tooling (MindfulTrader)")]
struct Cli {
    #[command(subcommand)]
    command: Commands,
}

#[derive(Subcommand)]
enum Commands {
    /// Train the production-shape Student-t HMM on a .context.parquet file.
    Train {
        /// Input .context.parquet (the offline replay generator's output).
        #[arg(long, default_value = "../lbrnet/data/raw/offline_replay_full_20260918.context.parquet")]
        input: PathBuf,
        /// Output model manifest (JSON). Default: tools/output/hmm_model_<archive stamp>.json.
        #[arg(long)]
        output: Option<PathBuf>,
        /// Independent EM fits from different k-means++ seeds; the best TRAINING fit is kept.
        #[arg(long, default_value_t = 1)]
        restarts: usize,
        /// Seed of the first restart (restart r uses seed + r).
        #[arg(long, default_value_t = 42)]
        seed: u64,
        /// EM iterations (production run: 20).
        #[arg(long, default_value_t = 20)]
        n_iter: usize,
        /// Fraction of sessions (the latest) held out for an out-of-sample log-likelihood; 0 = none
        /// (production run). One session between train and holdout is dropped as an embargo.
        #[arg(long, default_value_t = 0.0)]
        holdout: f64,
        /// Use only the first N rows (0 = all) -- for smoke runs.
        #[arg(long, default_value_t = 0)]
        max_rows: usize,
    },
}

/// Path arguments are relative to the MindfulTrader checkout, wherever cargo was run from.
fn repo_root() -> PathBuf {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    root.canonicalize().unwrap_or(root)
}

fn resolve(p: &Path) -> PathBuf {
    let path = if p.is_absolute() { p.to_path_buf() } else { repo_root().join(p) };
    path.canonicalize().unwrap_or(path)
}

fn main() {
    let cli = Cli::parse();
    let Commands::Train { input, output, restarts, seed, n_iter, holdout, max_rows } = cli.command;
    if let Err(e) = train(&resolve(&input), output.map(|o| resolve(&o)), restarts, seed, n_iter, holdout, max_rows) {
        eprintln!("hmm_tool train: {e}");
        std::process::exit(1);
    }
}

#[allow(clippy::too_many_arguments)]
fn train(input: &Path, output: Option<PathBuf>, restarts: usize, seed: u64, n_iter: usize, holdout: f64, max_rows: usize) -> Result<(), String> {
    if restarts == 0 || !(0.0..0.5).contains(&holdout) {
        return Err("--restarts must be >= 1 and --holdout in [0, 0.5)".into());
    }
    let file_name = input.file_name().map(|f| f.to_string_lossy().into_owned()).unwrap_or_default();
    let scope = format!("input={file_name} K={K} D={D} restarts={restarts} seed={seed} n_iter={n_iter} holdout={holdout} max_rows={max_rows}");
    let mut r = Report::open(&repo_root(), "hmm_tool_train", &scope).map_err(|e| format!("cannot open the report archive: {e}"))?;
    let output = output.unwrap_or_else(|| repo_root().join(r.archive_path().replace("hmm_tool_train_", "hmm_model_").replace(".txt", ".json")));
    let config = TrainingConfig { n_iter, ..TrainingConfig::production_20260918() };
    let t0 = Instant::now();

    r.line("=== hmm_tool train ===");
    r.line(format!("input   {}", input.display()));
    r.line(format!("output  {}", output.display()));
    r.line(format!("archive {}", r.archive_path()));
    r.line(format!("config  {config:?}"));
    r.line(format!("fields  {:?}", hmm_input_fields()));

    // 1. Read.
    let fields = hmm_input_fields();
    let data = context_parquet::read::<D>(input, &fields, max_rows)?;
    let (mut rows, mut ts) = (data.rows, data.timestamps_us);
    rows.iter_mut().for_each(|row| *row = to_model_units(row)); // same units as live hmm_step
    r.line(format!("[1] read {} rows x {D} in {:.1}s (model units: input::MODEL_UNITS)", rows.len(), t0.elapsed().as_secs_f64()));
    if let Some(i) = ts.windows(2).position(|w| w[1] < w[0]) {
        return Err(format!("timestamps go backwards at row {}; the file must be in time order", i + 1));
    }

    // 2. Refuse near-floor-variance features.
    let (_, raw_var) = global_mean_var(&rows, 0.0);
    for (j, v) in raw_var.iter().enumerate() {
        if *v <= NEAR_FLOOR_VARIANCE {
            return Err(format!("feature '{}' has variance {v:.3e}, within 100x of the model's variance floor ({MIN_COVAR:e})", fields[j]));
        }
    }

    // 3. Production preparation rules.
    let dropped = drop_flat_runs(&mut rows, &mut ts);
    trim(&mut rows, &mut ts, WARMUP_TRIM, TAIL_TRIM);
    let lengths = session_lengths(&ts, SESSION_GAP_SECONDS);
    r.line(format!(
        "[2] dead-physics filter dropped {dropped} rows; trimmed {WARMUP_TRIM}+{TAIL_TRIM}; {} rows in {} sessions (gap > {SESSION_GAP_SECONDS}s)",
        rows.len(),
        lengths.len()
    ));
    if rows.len() < 2000 {
        return Err(format!("only {} rows after preparation (minimum 2000)", rows.len()));
    }

    // 4. Optional session holdout behind a one-session embargo.
    let all_segments = segments(&rows, &lengths);
    let n_holdout = if holdout > 0.0 { ((lengths.len() as f64 * holdout).ceil() as usize).max(1) } else { 0 };
    let n_train_sessions = if n_holdout > 0 { lengths.len().saturating_sub(n_holdout + 1) } else { lengths.len() };
    if n_train_sessions == 0 {
        return Err("no training sessions left after the holdout".into());
    }
    let train_segs: Vec<&[[f64; D]]> = all_segments[..n_train_sessions].to_vec();
    let holdout_segs: Vec<&[[f64; D]]> = if n_holdout > 0 { all_segments[lengths.len() - n_holdout..].to_vec() } else { Vec::new() };
    let train_rows: Vec<[f64; D]> = train_segs.iter().flat_map(|s| s.iter().copied()).collect();
    let n_train = train_rows.len();
    r.line(format!(
        "[3] train: {n_train} rows / {n_train_sessions} sessions; holdout: {} rows / {n_holdout} sessions",
        holdout_segs.iter().map(|s| s.len()).sum::<usize>()
    ));

    // 5. Priors and global statistics from the training rows.
    let (global_mean, global_var) = global_mean_var(&train_rows, config.min_covar);
    let priors = config.priors(n_train);
    r.line(format!("    priors {priors:?}"));
    let weights = feature_weights(BULLET_WEIGHT, MAP_WEIGHT);

    // 6. Restarts.
    let mut best: Option<(u64, Model, FitResult)> = None;
    for k in 0..restarts {
        let s = seed + k as u64;
        let t = Instant::now();
        let mut model = initial_model::<D, K>(&train_rows, &global_var, &config, weights, &mut SplitMix64::new(s))
            .ok_or("fewer training rows than states")?;
        let res = fit(&mut model, &train_segs, &global_mean, &global_var, &priors, config.update_dof, config.n_iter, config.min_iter, config.tol);
        r.line(format!(
            "[4] restart {}/{restarts} seed {s}: best MAP LL {:.4} (raw {:.4}) at iter {}, {} iters, converged={}, dof={:?}, {:.1}s",
            k + 1,
            res.best_train_map_ll,
            res.best_train_raw_ll,
            res.best_train_iter,
            res.iterations_run,
            res.converged,
            model.dof.map(|d| (d * 100.0).round() / 100.0),
            t.elapsed().as_secs_f64()
        ));
        r.line(format!("    MAP LL history {:?}", res.map_ll_history.iter().map(|v| v.round()).collect::<Vec<_>>()));
        if best.as_ref().is_none_or(|(_, _, b)| res.best_train_map_ll > b.best_train_map_ll) {
            best = Some((s, model, res));
        }
    }
    let Some((best_seed, model, res)) = best else { return Err("no fit".into()) };

    // 7. Model selection and out-of-sample numbers.
    let n_params = (K - 1) + K * (K - 1) + 2 * K * D + K;
    let bic = -2.0 * res.best_train_raw_ll + n_params as f64 * (n_train as f64).ln();
    r.line(format!("[5] kept seed {best_seed}: raw train LL {:.4} ({:.6}/row), BIC {bic:.1} ({n_params} params)", res.best_train_raw_ll, res.best_train_raw_ll / n_train as f64));
    if !holdout_segs.is_empty() {
        let n_h: usize = holdout_segs.iter().map(|s| s.len()).sum();
        let ll: f64 = holdout_segs.iter().map(|s| forward_backward(&model, s).3).sum();
        r.line(format!("    holdout LL {ll:.4} ({:.6}/row, train {:.6}/row)", ll / n_h as f64, res.best_train_raw_ll / n_train as f64));
    }

    // 8. Viterbi decode + classification.
    let states: Vec<i32> = train_segs.iter().flat_map(|s| model.decode(s)).map(|s| s as i32).collect();
    let by_state = |field: &str| -> Result<Vec<f64>, String> {
        let j = model_index(field).ok_or_else(|| format!("'{field}' is not a model input"))?;
        Ok(model.means.iter().map(|m| m[j]).collect())
    };
    let dof = model.dof.to_vec();
    let class_cfg = ClassifyConfig { dof_min: config.dof_min, dof_max: config.dof_max, ..ClassifyConfig::default() };
    let profiles = classify_states(&states, &dof, &by_state("log_scale_ratio")?, &by_state("relative_range")?, &by_state("lempel_ziv")?, K, &class_cfg);
    r.line("[6] states:");
    for p in &profiles {
        r.line(format!(
            "    state {}: {:<16} dof {:>7.3}  self-transition {:.4}  share {:.4}  mean tenure {:.1} rows",
            p.raw_id,
            p.market_climate.label(),
            dof[p.raw_id],
            model.transmat[p.raw_id][p.raw_id],
            p.sample_share,
            p.mean_tenure_bars
        ));
    }

    // 9. Sign-off diagnostics.
    let shares: Vec<f64> = profiles.iter().map(|p| p.sample_share).collect();
    let boundary = dof_boundary_check(&dof, config.dof_min, config.dof_max, class_cfg.epsilon);
    let balance = state_occupancy_balance_check(&shares, class_cfg.min_share_floor);
    let taleb = taleb_gate_pass(&dof, &shares, config.dof_min, config.dof_max, class_cfg.epsilon, class_cfg.min_share_floor);
    r.line(format!("[7] dof boundary: no state pinned = {} {:?}", boundary.no_state_pinned_at_bounds, boundary.pinned_states));
    r.line(format!("    occupancy balance: {balance:?}"));
    r.line(format!(
        "    fat-tail (Taleb) gate: pass = {} -- needs an eligible state with dof < {TALEB_KURTOSIS_UNDEFINED_DOF}; lowest eligible dof {:.3}",
        taleb.pass, taleb.dof_min_eligible
    ));

    // 10. Save.
    let sha = student_t_hmm::artifact::save(&output, &model, &profiles).map_err(|e| format!("cannot save the model: {e:?}"))?;
    r.line(format!("[8] saved {} (model_sha256 {sha})", output.display()));
    r.line(format!("=== done in {:.1}s ===", t0.elapsed().as_secs_f64()));
    r.finish().map_err(|e| format!("cannot append the ledger row: {e}"))
}
