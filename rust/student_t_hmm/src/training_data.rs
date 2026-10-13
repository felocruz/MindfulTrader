//! Training-data preparation and model initialization for offline training (`rust/tools`
//! `hmm_tool train`): everything between "rows read from `.context.parquet`" and `train::fit`.
//! Pure -- no file I/O.
//!
//! Ported from the 2026-09-18 production run's Python (`lbrnet/lbrnet/scripts/
//! train_student_t_hmm.py`, `lbrnet/lbrnet/models/student_t_hmm.py`), rule for rule:
//! 1. `drop_flat_runs` -- the "dead-physics" filter (`_project_and_filter_dead_physics`): a row is
//!    flat when no input moved more than `FLAT_EPS` from the previous row; once a flat run reaches
//!    `FLAT_RUN_DROP_THRESHOLD` rows, the rest of it is dropped (a frozen feed, not a market).
//! 2. `trim` -- `--warmup-trim 500` / `--tail-trim 200` rows off the whole filtered series.
//! 3. `session_lengths` -- a gap of more than `SESSION_GAP_SECONDS` between consecutive rows starts
//!    a new, independent sequence (`_compute_session_lengths_from_timestamps`).
//! 4. `initial_model` -- k-means++ means, every state's variance = the global variance, a 0.9
//!    sticky diagonal, uniform start, one starting dof (`StudentTHmm._ensure_initialized`).
//! 5. `effective_covar_eta` -- `max(eta, min_share * N)` (same function).
//!
//! Not ported, deliberately: the Python's `--jitter` (1e-4 Gaussian noise added to every row "to
//! stabilize zero-variance features" -- the problem that was the constant `fast_mean_rev_z`, now
//! excluded from the input instead, `input.rs`); and crash oversampling (off in the production
//! run, and keyed on the removed `vol_convexity`). The k-means++ draws use `init::SplitMix64`, not
//! numpy's generator, so a fit is reproducible here but not bit-identical to Python's.

use crate::init::{kmeans_plusplus_seed_indices, SplitMix64};
use crate::model::StudentTHmm;
use crate::train::EmPriors;

pub const FLAT_EPS: f64 = 1e-7;
pub const FLAT_RUN_DROP_THRESHOLD: usize = 64;
pub const WARMUP_TRIM: usize = 500;
pub const TAIL_TRIM: usize = 200;
pub const SESSION_GAP_SECONDS: i64 = 3600;

/// The 2026-09-18 production run's training configuration (its manifest's `training_config`).
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct TrainingConfig {
    pub n_iter: usize,
    pub min_iter: usize,
    pub tol: f64,
    pub update_dof: bool,
    pub initial_dof: f64,
    pub dof_min: f64,
    pub dof_max: f64,
    pub mean_prior_kappa: f64,
    pub covar_prior_eta: f64,
    pub covar_prior_min_share: f64,
    pub trans_prior_alpha: f64,
    pub sticky_diag_alpha: f64,
    pub min_covar: f64,
}

impl TrainingConfig {
    pub fn production_20260918() -> Self {
        Self {
            n_iter: 20,
            min_iter: 0,
            tol: 0.01,
            update_dof: true,
            initial_dof: 30.0,
            dof_min: crate::model::DEFAULT_DOF_MIN,
            dof_max: crate::model::DEFAULT_DOF_MAX,
            mean_prior_kappa: 16.0,
            covar_prior_eta: 16.0,
            covar_prior_min_share: 0.03,
            trans_prior_alpha: 1.0,
            sticky_diag_alpha: 2.0,
            min_covar: 1e-12,
        }
    }

    /// The EM priors for a training set of `n_rows`.
    pub fn priors(&self, n_rows: usize) -> EmPriors {
        EmPriors {
            mean_prior_kappa: self.mean_prior_kappa,
            covar_prior_eta_effective: effective_covar_eta(self.covar_prior_eta, self.covar_prior_min_share, n_rows),
            trans_prior_alpha: self.trans_prior_alpha,
            sticky_diag_alpha: self.sticky_diag_alpha,
            min_covar: self.min_covar,
        }
    }
}

/// Keep-mask of the dead-physics filter (rule 1). `true` = keep.
pub fn flat_run_keep_mask<const D: usize>(rows: &[[f64; D]]) -> Vec<bool> {
    let mut keep = Vec::with_capacity(rows.len());
    let mut run = 0usize;
    for (i, row) in rows.iter().enumerate() {
        let flat = i > 0 && row.iter().zip(&rows[i - 1]).all(|(a, b)| (a - b).abs() <= FLAT_EPS);
        run = if flat { run + 1 } else { 0 };
        keep.push(run < FLAT_RUN_DROP_THRESHOLD);
    }
    keep
}

/// Applies the dead-physics filter to rows and their timestamps; returns how many were dropped.
pub fn drop_flat_runs<const D: usize>(rows: &mut Vec<[f64; D]>, timestamps_us: &mut Vec<i64>) -> usize {
    assert_eq!(rows.len(), timestamps_us.len(), "one timestamp per row");
    let keep = flat_run_keep_mask(rows);
    let before = rows.len();
    let mut it = keep.iter();
    rows.retain(|_| *it.next().unwrap_or(&true));
    let mut it = keep.iter();
    timestamps_us.retain(|_| *it.next().unwrap_or(&true));
    before - rows.len()
}

/// Drops `warmup` rows from the front and `tail` from the back (rule 2). Leaves both empty when
/// there are not more than `warmup + tail` rows.
pub fn trim<const D: usize>(rows: &mut Vec<[f64; D]>, timestamps_us: &mut Vec<i64>, warmup: usize, tail: usize) {
    assert_eq!(rows.len(), timestamps_us.len(), "one timestamp per row");
    let n = rows.len();
    if n <= warmup + tail {
        rows.clear();
        timestamps_us.clear();
        return;
    }
    rows.truncate(n - tail);
    timestamps_us.truncate(n - tail);
    rows.drain(..warmup);
    timestamps_us.drain(..warmup);
}

/// Lengths of the independent sequences (rule 3): a gap strictly greater than `gap_seconds`
/// starts a new one.
pub fn session_lengths(timestamps_us: &[i64], gap_seconds: i64) -> Vec<usize> {
    let mut lengths = Vec::new();
    let mut current = 0usize;
    for (i, &t) in timestamps_us.iter().enumerate() {
        if i > 0 && t - timestamps_us[i - 1] > gap_seconds * 1_000_000 {
            lengths.push(current);
            current = 0;
        }
        current += 1;
    }
    if current > 0 {
        lengths.push(current);
    }
    lengths
}

/// Splits `rows` into consecutive slices of the given lengths.
pub fn segments<'a, const D: usize>(rows: &'a [[f64; D]], lengths: &[usize]) -> Vec<&'a [[f64; D]]> {
    let mut out = Vec::with_capacity(lengths.len());
    let mut start = 0;
    for &len in lengths {
        out.push(&rows[start..start + len]);
        start += len;
    }
    out
}

/// Population mean and variance (ddof 0) per feature, variance floored at `min_covar` -- the MAP
/// priors' global statistics.
pub fn global_mean_var<const D: usize>(rows: &[[f64; D]], min_covar: f64) -> ([f64; D], [f64; D]) {
    let n = rows.len().max(1) as f64;
    let mut mean = [0.0; D];
    for row in rows {
        for j in 0..D {
            mean[j] += row[j];
        }
    }
    mean.iter_mut().for_each(|m| *m /= n);
    let mut var = [0.0; D];
    for row in rows {
        for j in 0..D {
            let d = row[j] - mean[j];
            var[j] += d * d;
        }
    }
    var.iter_mut().for_each(|v| *v = (*v / n).max(min_covar));
    (mean, var)
}

/// `max(eta, min_share * n)` when `eta > 0`, else `eta` (rule 5).
pub fn effective_covar_eta(eta: f64, min_share: f64, n_rows: usize) -> f64 {
    if eta > 0.0 { eta.max(min_share * n_rows as f64) } else { eta }
}

/// The starting model (rule 4). `None` when there are fewer rows than states.
pub fn initial_model<const D: usize, const K: usize>(
    rows: &[[f64; D]],
    global_var: &[f64; D],
    config: &TrainingConfig,
    feature_weights: [f64; D],
    rng: &mut SplitMix64,
) -> Option<StudentTHmm<D, K>> {
    let seeds = kmeans_plusplus_seed_indices(rows, K, rng);
    if seeds.len() < K {
        return None;
    }
    let means: [[f64; D]; K] = std::array::from_fn(|k| rows[seeds[k]]);
    let off_diagonal = 0.1 / (K.max(2) - 1) as f64;
    let transmat: [[f64; K]; K] = std::array::from_fn(|i| std::array::from_fn(|j| if i == j { 0.9 } else { off_diagonal }));
    Some(StudentTHmm::new(
        means,
        [*global_var; K],
        transmat,
        [1.0 / K as f64; K],
        [config.initial_dof; K],
        config.dof_min,
        config.dof_max,
        feature_weights,
    ))
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn flat_runs_keep_their_first_63_rows() {
        // Row 0 is never flat; rows 1..=100 repeat it exactly: a 100-row flat run.
        let rows = vec![[1.0, 2.0]; 101];
        let keep = flat_run_keep_mask(&rows);
        assert!(keep[..64].iter().all(|&k| k), "row 0 plus flat rows 1..=63 are kept");
        assert!(keep[64..].iter().all(|&k| !k), "the 64th flat row onwards is dropped");
        // A real move resets the run: rows 1..=49 flat, 50 moves, 51..=100 flat again (50 rows).
        let mut moved = rows.clone();
        for r in moved.iter_mut().skip(50) {
            *r = [1.0, 2.0 + 1e-6];
        }
        assert!(flat_run_keep_mask(&moved).iter().all(|&k| k), "no run reaches 64 rows");
    }

    #[test]
    fn the_flat_threshold_is_inclusive_of_eps() {
        // Alternating 0 / eps: every step is exactly eps, which counts as flat (`<= flat_eps`).
        let at_eps: Vec<[f64; 1]> = (0..70).map(|i| [if i % 2 == 0 { 0.0 } else { FLAT_EPS }]).collect();
        let keep = flat_run_keep_mask(&at_eps);
        assert!(keep[63] && !keep[64]);
        // Steps of 2 eps are moves.
        let above: Vec<[f64; 1]> = (0..70).map(|i| [if i % 2 == 0 { 0.0 } else { 2.0 * FLAT_EPS }]).collect();
        assert!(flat_run_keep_mask(&above).iter().all(|&k| k));
    }

    #[test]
    fn drop_flat_runs_keeps_rows_and_timestamps_aligned() {
        let mut rows = vec![[5.0]; 70]; // rows 1..=69 flat: the last 6 are dropped
        let mut ts: Vec<i64> = (0..70).collect();
        assert_eq!(drop_flat_runs(&mut rows, &mut ts), 6);
        assert_eq!((rows.len(), ts.len(), *ts.last().expect("rows left")), (64, 64, 63));
    }

    #[test]
    fn trim_cuts_both_ends_or_everything() {
        let mut rows: Vec<[f64; 1]> = (0..1000).map(|i| [i as f64]).collect();
        let mut ts: Vec<i64> = (0..1000).collect();
        trim(&mut rows, &mut ts, WARMUP_TRIM, TAIL_TRIM);
        assert_eq!((rows.len(), ts[0], *ts.last().expect("rows left")), (300, 500, 799));
        let mut few = vec![[0.0]; 700];
        let mut few_ts = vec![0; 700];
        trim(&mut few, &mut few_ts, WARMUP_TRIM, TAIL_TRIM);
        assert!(few.is_empty() && few_ts.is_empty());
    }

    #[test]
    fn sessions_split_on_gaps_strictly_longer_than_an_hour() {
        let s = 1_000_000;
        let ts = [0, s, 2 * s, 2 * s + 3600 * s, 2 * s + 7201 * s, 2 * s + 7202 * s];
        assert_eq!(session_lengths(&ts, SESSION_GAP_SECONDS), vec![4, 2], "exactly one hour is not a break");
        assert!(session_lengths(&[], SESSION_GAP_SECONDS).is_empty());
        let rows = [[0.0]; 6];
        let segs = segments(&rows, &[4, 2]);
        assert_eq!((segs[0].len(), segs[1].len()), (4, 2));
    }

    #[test]
    fn global_stats_are_population_and_floored() {
        let rows = [[1.0, 5.0], [3.0, 5.0]];
        let (m, v) = global_mean_var(&rows, 1e-12);
        assert_eq!((m[0].to_bits(), v[0].to_bits()), (2.0f64.to_bits(), 1.0f64.to_bits()));
        assert_eq!(v[1].to_bits(), 1e-12f64.to_bits(), "a constant feature sits on the floor");
    }

    #[test]
    fn effective_eta_scales_with_the_training_set() {
        assert_eq!(effective_covar_eta(16.0, 0.03, 100).to_bits(), 16.0f64.to_bits());
        assert!((effective_covar_eta(16.0, 0.03, 21_966_483) - 658_994.49).abs() < 1e-6);
        assert_eq!(effective_covar_eta(0.0, 0.03, 1_000_000).to_bits(), 0.0f64.to_bits(), "a disabled prior stays disabled");
    }

    #[test]
    fn initial_model_matches_the_python_initialization() {
        let rows: Vec<[f64; 2]> = (0..50).map(|i| [i as f64, (i % 7) as f64]).collect();
        let (_, var) = global_mean_var(&rows, 1e-12);
        let cfg = TrainingConfig::production_20260918();
        let mut rng = SplitMix64::new(42);
        let m: StudentTHmm<2, 4> = initial_model(&rows, &var, &cfg, [1.8, 1.0], &mut rng).expect("enough rows");
        assert!(m.means.iter().all(|mu| rows.contains(mu)), "means are data points (k-means++)");
        assert!(m.covars.iter().all(|c| c.iter().zip(&var).all(|(a, b)| a.to_bits() == b.to_bits())));
        assert_eq!(m.transmat[0][0].to_bits(), 0.9f64.to_bits());
        assert!((m.transmat[0][1] - 0.1 / 3.0).abs() < 1e-15);
        assert!(m.transmat.iter().all(|r| (r.iter().sum::<f64>() - 1.0).abs() < 1e-12));
        assert!(m.startprob.iter().all(|p| (p - 0.25).abs() < 1e-15));
        assert!(m.dof.iter().all(|d| d.to_bits() == 30.0f64.to_bits()));
        assert_eq!(m.feature_weights.map(f64::to_bits), [1.8f64.to_bits(), 1.0f64.to_bits()]);
        // Same seed, same model.
        let again: StudentTHmm<2, 4> = initial_model(&rows, &var, &cfg, [1.8, 1.0], &mut SplitMix64::new(42)).expect("enough rows");
        assert!(m == again, "same seed, same model");
        assert!(initial_model::<2, 4>(&rows[..3], &var, &cfg, [1.0, 1.0], &mut rng).is_none());
    }
}
