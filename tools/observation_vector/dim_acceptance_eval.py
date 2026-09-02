#!/usr/bin/env python3
"""dim_acceptance_eval.py -- reusable pre-retrain acceptance eval for a
candidate observation-vector dimension (docs/superpowers/specs/2026-08-29-
hmm-fat-tail-observation-vector-brainstorm.md Sec 8/9: this is the concrete
mechanism that turns a ledger row from CANDIDATE/IN-UNMEASURED into IN/OUT
with real evidence, replacing one-off bespoke scripts per candidate -- same
rationale PRODUCTION_TRIAGE.md row 15 already named as a systemic problem
for lbrnet's 11+ dated calibration scripts).

Two tests, both usable BEFORE any HMM retrain (a cross-state discrimination
ratio -- the real post-retrain acceptance bar -- is structurally impossible
pre-retrain, since it needs the dim already in a fitted model; this script
does not fake a substitute for it):

1. REDUNDANCY -- Pearson correlation + upper-tail dependence (lambda_U,
   rank-transform + joint exceedance at q=0.90) against every one of the 16
   original observation-vector dims, using real production-exported data
   (lbrnet/data/raw/event_data.context.parquet, 56.9M rows). Same
   methodology as lbrnet/knowledge/global/training/hmm_feature_selection.md
   Sec 8 (max lambda_U found there: 0.276, "no severe tail-specific
   redundancy" -- reported here as a reference point, not a hard pass/fail
   cutoff; no literature-established threshold for this specific decision
   was found, so raw numbers are reported honestly rather than a manufactured
   cutoff).

   **KNOWN DATA-QUALITY CAVEAT, carried from this brainstorm doc's own Sec
   3/1.11 findings, not something to silently reuse as if clean**: several
   of these 16 comparison columns were confirmed BAR-GATED at export time
   (frozen for their whole bar duration, never reading the live/still-forming
   bar) -- `vol_convexity`, `amihud_illiquidity`, `liq_fragility` (all TS3,
   confirmed via source comment). A frozen comparison series structurally
   UNDERSTATES its true correlation/tail-dependence with any tick-native
   candidate (it literally cannot move during its frozen periods), so a low
   lambda_U against these three specifically is NOT strong evidence of real
   independence -- it's at least partly a measurement artifact. This script
   flags those rows in its own output rather than requiring the reader to
   remember which columns are compromised.

2. PREDICTIVE POWER -- one of two modes, chosen per candidate's own
   hypothesis (methodologically distinct, not interchangeable):
   - `directional` (reversion/continuation): does the candidate's sign
     predict forward-return sign? Mirrors mean_rev_z_variant_comparison.py's
     hit-rate/Wilson-CI methodology exactly.
   - `magnitude` (volatility/tail-risk forecast): does a high candidate
     reading predict LARGER forward-return magnitude, not direction? Correct
     mode for burstiness/jump-share/kurtosis-type candidates -- forcing
     these through a directional hit-rate test is a methodological mismatch.
     Bootstrap CI on the mean-|forward-return| gap between top/bottom decile
     candidate readings (not a t-test -- this project's whole premise is
     non-Gaussian returns, a normality-assuming test would be the wrong
     tool here).

Candidate input format: a 2-column Parquet file, `timestamp_us,value` --
decouples "compute a candidate" (its own C++/Python work, per-candidate)
from "evaluate a candidate" (this script, generic, reusable across all of
them). Parquet throughout, not CSV -- this is 56.9M/38.5M-row data, a
columnar format is the right tool, not a text round-trip.

Usage (mts env):
    mamba run -n mts python tools/observation_vector/dim_acceptance_eval.py \\
        --candidate-parquet <path> --candidate-name raschke_burst \\
        --test-mode magnitude --horizons 30,60,120,240 \\
        [--context-stride 38] [--top-decile 0.10]
"""
import argparse
import math
from pathlib import Path

import numpy as np
import polars as pl

WORKSPACE = Path("/home/rcruz/devel/VSCode")
CONTEXT_PARQUET = WORKSPACE / "lbrnet/data/raw/event_data.context.parquet"
TICKS_PARQUET = WORKSPACE / "lbrnet/data/raw/mes_continuous_ticks.parquet"

ORIGINAL_16_DIMS = [
    "log_variance_ratio", "burstiness_index", "relative_range", "correction_action",
    "vol_convexity", "lempel_ziv", "hurst_exponent", "micro_asymmetry", "fisher_info",
    "tail_index", "skewness_idx", "amihud_illiquidity", "liq_fragility",
    "recurrence_rate", "fractal_dim", "mean_rev_z",
]

REFERENCE_MAX_LAMBDA_U = 0.276  # hmm_feature_selection.md Sec 8, full-16D audit, for context only

# Confirmed via source comment (2026-08-14 full-institutional-coverage audit + this brainstorm
# doc's own Sec 1.11 grep pass) -- bar-gated, never reads the live/still-forming bar. A frozen
# comparison series structurally understates true correlation/tail-dependence with a tick-native
# candidate; treat low lambda_U against these specifically with extra skepticism, not as clean
# evidence of independence.
KNOWN_BAR_GATED_DIMS = {"vol_convexity", "amihud_illiquidity", "liq_fragility"}


def load_candidate(parquet_path: Path) -> pl.DataFrame:
    df = pl.read_parquet(parquet_path)
    return df.select("timestamp_us", "value").sort("timestamp_us").unique(subset=["timestamp_us"], keep="last")


def load_context_sample(stride: int) -> pl.DataFrame:
    cols = ["timestamp_us"] + ORIGINAL_16_DIMS
    df = (
        pl.scan_parquet(CONTEXT_PARQUET)
        .filter(pl.col("risk_gate_context_available"))
        .select(cols)
        .sort("timestamp_us")
        .with_row_index("row_idx")
        .filter(pl.col("row_idx") % stride == 0)
        .drop("row_idx")
        .unique(subset=["timestamp_us"], keep="last")
        .sort("timestamp_us")
        .collect()
    )
    print(f"context sample: {len(df)} rows (stride={stride}) from {CONTEXT_PARQUET.name}")
    return df


def empirical_upper_tail_dependence(x: np.ndarray, y: np.ndarray, q: float = 0.90) -> float:
    """Rank-transform both series to [0,1] (empirical CDF), estimate
    lambda_U = P(rank(y) > q | rank(x) > q) via joint/marginal exceedance
    counts -- same estimator hmm_feature_selection.md Sec 8 used."""
    n = len(x)
    rx = np.argsort(np.argsort(x)) / (n - 1)
    ry = np.argsort(np.argsort(y)) / (n - 1)
    x_exceed = rx > q
    n_exceed = x_exceed.sum()
    if n_exceed == 0:
        return float("nan")
    joint = (x_exceed & (ry > q)).sum()
    return float(joint / n_exceed)


def redundancy_check(candidate: pl.DataFrame, context: pl.DataFrame, candidate_name: str) -> None:
    joined = context.sort("timestamp_us").join_asof(
        candidate.sort("timestamp_us"), on="timestamp_us", strategy="nearest",
        tolerance="5m",
    ).drop_nulls(subset=["value"])
    print(f"\n=== Redundancy check: {candidate_name} vs. the original 16 dims "
          f"({len(joined)} aligned rows, asof-joined within 5min) ===")
    if len(joined) < 100:
        print("  too few aligned rows to trust a correlation/tail-dependence estimate -- "
              "check candidate Parquet's timestamp range overlaps the context parquet's")
        return
    cand = joined["value"].to_numpy()
    rows = []
    for dim in ORIGINAL_16_DIMS:
        other = joined[dim].to_numpy()
        mask = np.isfinite(cand) & np.isfinite(other)
        if mask.sum() < 100:
            continue
        corr = float(np.corrcoef(cand[mask], other[mask])[0, 1])
        lam = empirical_upper_tail_dependence(cand[mask], other[mask])
        rows.append((dim, corr, lam))
    rows.sort(key=lambda r: abs(r[1]), reverse=True)
    print(f"  {'dim':<20} {'pearson_r':>10} {'lambda_U':>10}")
    for dim, corr, lam in rows:
        flag = "  <- BAR-GATED, low values here are NOT clean evidence of independence" \
            if dim in KNOWN_BAR_GATED_DIMS else ""
        print(f"  {dim:<20} {corr:>+10.4f} {lam:>10.4f}{flag}")
    trustworthy = [r for r in rows if r[0] not in KNOWN_BAR_GATED_DIMS]
    max_abs_corr = max(abs(r[1]) for r in trustworthy)
    max_lam = max(r[2] for r in trustworthy if not math.isnan(r[2]))
    print(f"\n  Among non-bar-gated comparison dims only: max |pearson_r| = {max_abs_corr:.4f}   "
          f"max lambda_U = {max_lam:.4f}   "
          f"(reference: 2026-08-23 16D audit's own max lambda_U = {REFERENCE_MAX_LAMBDA_U:.4f})")
    print("  [no established institutional cutoff for 'too redundant' -- these are the raw "
          "numbers, judge against the reference point above, not a manufactured pass/fail line. "
          f"{len(rows) - len(trustworthy)} of {len(rows)} comparison dims are known bar-gated "
          "(see module docstring) -- their numbers are reported above but excluded from the "
          "summary max, not silently trusted.]")


def wilson_ci(k: int, n: int, z: float = 1.96) -> tuple:
    phat = k / n
    denom = 1 + z * z / n
    center = (phat + z * z / (2 * n)) / denom
    half = (z / denom) * np.sqrt(phat * (1 - phat) / n + z * z / (4 * n * n))
    return center - half, center + half


def bootstrap_mean_gap_ci(top: np.ndarray, bottom: np.ndarray, n_boot: int = 2000) -> tuple:
    rng = np.random.default_rng(0)
    gaps = np.empty(n_boot)
    for i in range(n_boot):
        t = rng.choice(top, size=len(top), replace=True)
        b = rng.choice(bottom, size=len(bottom), replace=True)
        gaps[i] = np.mean(np.abs(t)) - np.mean(np.abs(b))
    return float(np.mean(np.abs(top)) - np.mean(np.abs(bottom))), float(np.percentile(gaps, 2.5)), float(np.percentile(gaps, 97.5))


def compute_forward_returns(ts: np.ndarray, timestamps: np.ndarray, prices: np.ndarray,
                             signal_price: np.ndarray, horizon_minutes: int) -> np.ndarray:
    horizon_us = horizon_minutes * 60 * 1_000_000
    target_ts = ts + horizon_us
    idx = np.searchsorted(timestamps, target_ts)
    valid = idx < len(timestamps)
    gap_ok = np.zeros_like(valid)
    gap_ok[valid] = (timestamps[idx[valid]] - ts[valid]) <= horizon_us * 3
    valid &= gap_ok
    fwd = np.full(len(ts), np.nan)
    fwd[valid] = np.log(prices[idx[valid]] / signal_price[valid])
    return fwd


def predictive_power_directional(candidate: pl.DataFrame, ticks: pl.DataFrame, horizons: list) -> None:
    print(f"\n=== Predictive power (directional): forward-return sign vs. candidate sign ===")
    ts_all = ticks["timestamp_us"].to_numpy()
    px_all = ticks["close"].to_numpy()
    cand_ts = candidate["timestamp_us"].to_numpy()
    idx = np.searchsorted(ts_all, cand_ts)
    idx = np.clip(idx, 0, len(ts_all) - 1)
    cand_price = px_all[idx]
    cand_val = candidate["value"].to_numpy()

    n_tests = len(horizons)
    bonferroni_alpha = 0.05 / n_tests
    for h in horizons:
        fwd = compute_forward_returns(cand_ts, ts_all, px_all, cand_price, h)
        mask = np.isfinite(fwd) & (cand_val != 0)
        n = int(mask.sum())
        if n == 0:
            print(f"  {h:>4}min: 0 valid signals")
            continue
        hits = np.sign(fwd[mask]) == np.sign(cand_val[mask])
        k = int(hits.sum())
        hit_rate = k / n
        se_null = np.sqrt(0.25 / n)
        z = (hit_rate - 0.5) / se_null
        p = float(2 * (1 - 0.5 * (1 + math.erf(abs(z) / math.sqrt(2)))))
        lo, hi = wilson_ci(k, n)
        flag = "SURVIVES Bonferroni" if p < bonferroni_alpha else "does not survive Bonferroni"
        print(f"  {h:>4}min: n={n:<8} hit_rate={hit_rate:.4f} 95%CI=[{lo:.4f},{hi:.4f}] "
              f"p={p:.4f} ({flag}, alpha={bonferroni_alpha:.5f} for {n_tests} tests)")


def predictive_power_magnitude(candidate: pl.DataFrame, ticks: pl.DataFrame, horizons: list, top_decile: float) -> None:
    print(f"\n=== Predictive power (magnitude): does high {candidate.columns} predict larger |forward return|? ===")
    ts_all = ticks["timestamp_us"].to_numpy()
    px_all = ticks["close"].to_numpy()
    cand_ts = candidate["timestamp_us"].to_numpy()
    idx = np.searchsorted(ts_all, cand_ts)
    idx = np.clip(idx, 0, len(ts_all) - 1)
    cand_price = px_all[idx]
    cand_val = candidate["value"].to_numpy()

    hi_thresh = np.nanpercentile(cand_val, 100 * (1 - top_decile))
    lo_thresh = np.nanpercentile(cand_val, 100 * top_decile)
    for h in horizons:
        fwd = compute_forward_returns(cand_ts, ts_all, px_all, cand_price, h)
        valid = np.isfinite(fwd)
        top_mask = valid & (cand_val >= hi_thresh)
        bot_mask = valid & (cand_val <= lo_thresh)
        n_top, n_bot = int(top_mask.sum()), int(bot_mask.sum())
        if n_top < 30 or n_bot < 30:
            print(f"  {h:>4}min: too few samples in top/bottom decile (n_top={n_top}, n_bot={n_bot})")
            continue
        gap, lo, hi = bootstrap_mean_gap_ci(fwd[top_mask], fwd[bot_mask])
        survives = "SURVIVES (CI excludes 0)" if (lo > 0 or hi < 0) else "does not survive (CI includes 0)"
        print(f"  {h:>4}min: n_top={n_top:<7} n_bot={n_bot:<7} "
              f"mean|fwd_ret|_top={np.mean(np.abs(fwd[top_mask])):.6f} "
              f"mean|fwd_ret|_bot={np.mean(np.abs(fwd[bot_mask])):.6f} "
              f"gap={gap:+.6f} 95%CI=[{lo:+.6f},{hi:+.6f}] ({survives})")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--candidate-parquet", required=True, type=Path,
                         help="2-column Parquet file: timestamp_us,value")
    parser.add_argument("--candidate-name", required=True, type=str)
    parser.add_argument("--test-mode", choices=["directional", "magnitude"], required=True,
                         help="directional = reversion/continuation hit-rate; "
                              "magnitude = volatility/tail-risk forecast (burstiness/jump-share/kurtosis-type)")
    parser.add_argument("--horizons", type=str, default="30,60,120,240",
                         help="Comma-separated forward-return horizons in minutes")
    parser.add_argument("--context-stride", type=int, default=38,
                         help="Row stride for the 56.9M-row context parquet sample (38 matches the "
                              "original 16D redundancy audit's own sample rate)")
    parser.add_argument("--top-decile", type=float, default=0.10,
                         help="Top/bottom fraction used for magnitude-mode comparison")
    parser.add_argument("--skip-redundancy", action="store_true")
    parser.add_argument("--skip-predictive", action="store_true")
    args = parser.parse_args()

    horizons = [int(h) for h in args.horizons.split(",")]
    candidate = load_candidate(args.candidate_parquet)
    print(f"candidate '{args.candidate_name}': {len(candidate)} rows, "
          f"timestamp range [{candidate['timestamp_us'].min()}, {candidate['timestamp_us'].max()}]")

    if not args.skip_redundancy:
        context = load_context_sample(args.context_stride)
        redundancy_check(candidate, context, args.candidate_name)

    if not args.skip_predictive:
        ticks = pl.scan_parquet(TICKS_PARQUET).select("timestamp_us", "close").sort("timestamp_us").collect()
        if args.test_mode == "directional":
            predictive_power_directional(candidate, ticks, horizons)
        else:
            predictive_power_magnitude(candidate, ticks, horizons, args.top_decile)


if __name__ == "__main__":
    main()
