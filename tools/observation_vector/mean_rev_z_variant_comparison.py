#!/usr/bin/env python3
"""mean_rev_z_variant_comparison.py -- empirical additive-vs-replace decision
for mean_rev_z/fast_mean_rev_z (docs/superpowers/plans/2026-08-28-activity-
clock-mean-rev-hurst-recurrence.md Task 5, per the sibling's revised verdict
in SCRATCHPAD.md: correlation alone does not decide this -- feed each
variant through Scoring.cpp:305's actual gate condition (score > 2.0) and
measure forward-return/hit-rate on the resulting signals independently).

Uses polars (columnar, lazy) for parquet I/O and numpy for the C++ driver's
raw binary export -- NOT pandas/.to_csv() (a 38.5M-row CSV round-trip is the
wrong tool for this data volume; contiguous binary arrays are what the C++
driver actually wants to fread(), not text it has to parse).

Pipeline:
1. Export real MES 1-second ticks (mes_continuous_ticks.parquet) and 15-min
   bars (mes_ripple_15m.parquet) to raw binary for the C++ driver
   (tools/observation_vector/mean_rev_z_variant_comparison.cpp, which uses the REAL
   ActivityClockMeanRevZ header + a faithful CalculateMeanReversionSpeed
   port to generate gate-crossing signals for both variants).
2. For each signal, look up the price `horizon_minutes` later in the full
   1-second series and compute the forward log-return.
3. Classify a "hit" as forward return having the OPPOSITE sign of the
   signal's own signed deviation (price was stretched one way, hit = it
   moved back the other way -- the literal definition of "reversion").
4. Report N signals, hit rate, mean/median forward return, per variant --
   plus the raw correlation between the two score series as a secondary
   diagnostic (not the deciding test, per the sibling's correction).

Usage: mamba run -n mts python tools/observation_vector/mean_rev_z_variant_comparison.py \
           [--horizon-minutes 60] [--imbalance-threshold 15.0] [--calibrate-only]
"""

import argparse
import math
import struct
import subprocess
import tempfile
from pathlib import Path

import numpy as np
import polars as pl


WORKSPACE = Path("/home/rcruz/devel/VSCode")
TICKS_PARQUET = WORKSPACE / "lbrnet/data/raw/mes_continuous_ticks.parquet"
BARS_15M_PARQUET = WORKSPACE / "lbrnet/data/raw/mes_ripple_15m.parquet"
MINDFULTRADER = WORKSPACE / "MindfulTrader"
DRIVER_SRC = MINDFULTRADER / "tools/observation_vector/mean_rev_z_variant_comparison.cpp"
DRIVER_BIN = MINDFULTRADER / "tools/bin/mean_rev_z_variant_comparison"


def build_driver():
    DRIVER_BIN.parent.mkdir(parents=True, exist_ok=True)
    print(
        f"building driver: g++ -O2 -std=c++17 -I{MINDFULTRADER / 'include'} {DRIVER_SRC} -o {DRIVER_BIN}"
    )
    subprocess.run(
        [
            "g++",
            "-O2",
            "-std=c++17",
            "-I",
            str(MINDFULTRADER / "include"),
            str(DRIVER_SRC),
            "-o",
            str(DRIVER_BIN),
        ],
        check=True,
    )


def write_binary(path: Path, timestamp_us: np.ndarray, *float_cols: np.ndarray):
    """[int64 count][int64 timestamp_us x count][float32 col x count]..."""
    count = timestamp_us.shape[0]
    with open(path, "wb") as f:
        f.write(struct.pack("<q", count))
        f.write(timestamp_us.astype("<i8", copy=False).tobytes())
        for col in float_cols:
            f.write(col.astype("<f4", copy=False).tobytes())


def export_binaries(scratch_dir: Path):
    ticks = (
        pl.scan_parquet(TICKS_PARQUET)
        .select("timestamp_us", "close", "bid_volume", "ask_volume")
        .sort("timestamp_us")
        .collect()
    )
    ticks_bin = scratch_dir / "ticks_1s.bin"
    write_binary(
        ticks_bin,
        ticks["timestamp_us"].to_numpy(),
        ticks["close"].to_numpy(),
        ticks["bid_volume"].to_numpy(),
        ticks["ask_volume"].to_numpy(),
    )
    print(f"exported {len(ticks)} 1-second bars to {ticks_bin}")

    bars = (
        pl.scan_parquet(BARS_15M_PARQUET)
        .select("timestamp_us", "close")
        .sort("timestamp_us")
        .collect()
    )
    bars_bin = scratch_dir / "bars_15m.bin"
    write_binary(bars_bin, bars["timestamp_us"].to_numpy(), bars["close"].to_numpy())
    print(f"exported {len(bars)} 15-min bars to {bars_bin}")

    return ticks, ticks_bin, bars_bin


def compute_forward_returns(
    signals: pl.DataFrame, ticks: pl.DataFrame, horizon_minutes: int
) -> pl.DataFrame:
    horizon_us = horizon_minutes * 60 * 1_000_000
    ts = ticks["timestamp_us"].to_numpy()
    px = ticks["close"].to_numpy()

    sig_ts = signals["timestamp_us"].to_numpy()
    sig_price = signals["price"].to_numpy()
    target_ts = sig_ts + horizon_us
    idx = np.searchsorted(ts, target_ts)

    valid = idx < len(ts)
    # Guard against a huge gap (overnight/weekend) swamping the horizon.
    gap_ok = np.zeros_like(valid)
    gap_ok[valid] = (ts[idx[valid]] - sig_ts[valid]) <= horizon_us * 3
    valid &= gap_ok

    forward_return = np.full(len(signals), np.nan)
    forward_return[valid] = np.log(px[idx[valid]] / sig_price[valid])

    return signals.with_columns(pl.Series("forward_return", forward_return)).filter(
        pl.col("forward_return").is_not_nan()
    )


def wilson_ci(k: int, n: int, z: float = 1.96) -> tuple:
    """Wilson score interval -- correct coverage near p=0.5 even at these n, unlike a naive p +/- 1.96*se."""
    phat = k / n
    denom = 1 + z * z / n
    center = (phat + z * z / (2 * n)) / denom
    half = (z / denom) * np.sqrt(phat * (1 - phat) / n + z * z / (4 * n * n))
    return center - half, center + half


def report(signals: pl.DataFrame, variant: str):
    sub = signals.filter(pl.col("variant") == variant)
    if len(sub) == 0:
        print(f"  {variant}: 0 signals with a valid forward return")
        return
    fwd = sub["forward_return"].to_numpy()
    dev = sub["signed_deviation"].to_numpy()
    # "Hit" = forward return moved opposite to the signal's own stretch direction.
    hits = np.sign(fwd) == -np.sign(dev)
    n = len(hits)
    k = int(hits.sum())
    hit_rate = k / n
    # Two-sided z-test against the null hit_rate = 0.5 (pure chance).
    se_null = np.sqrt(0.25 / n)
    z = (hit_rate - 0.5) / se_null
    p_value = float(2 * (1 - 0.5 * (1 + math.erf(abs(z) / math.sqrt(2)))))
    lo, hi = wilson_ci(k, n)
    print(
        f"  {variant}: n={n}  hit_rate={hit_rate:.4f}  95%CI=[{lo:.4f}, {hi:.4f}]  "
        f"z={z:+.2f}  p={p_value:.3f} (vs null=0.5)  "
        f"mean_fwd_ret={np.mean(fwd):.6f}  median_fwd_ret={np.median(fwd):.6f}  "
        f"std_fwd_ret={np.std(fwd):.6f}"
    )


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--horizon-minutes",
        type=int,
        default=60,
        help="Forward-return horizon in minutes, applied identically to both variants",
    )
    parser.add_argument(
        "--imbalance-threshold",
        type=float,
        default=None,
        help="Override the activity-clock imbalance threshold (calibrate bar-formation rate first)",
    )
    parser.add_argument(
        "--calibrate-only",
        action="store_true",
        help="Just run the driver and print bar-formation/signal counts, skip forward-return stats",
    )
    parser.add_argument(
        "--reuse-binaries",
        type=str,
        default=None,
        help="Path to a scratch dir with already-exported ticks_1s.bin/bars_15m.bin (skip re-export)",
    )
    args = parser.parse_args()

    build_driver()

    scratch_ctx = tempfile.TemporaryDirectory(prefix="mean_rev_z_comparison_")
    scratch_dir = Path(args.reuse_binaries) if args.reuse_binaries else Path(scratch_ctx.name)

    if args.reuse_binaries and (scratch_dir / "ticks_1s.bin").exists():
        print(f"reusing existing binaries in {scratch_dir}")
        ticks = (
            pl.scan_parquet(TICKS_PARQUET)
            .select("timestamp_us", "close")
            .sort("timestamp_us")
            .collect()
        )
        ticks_bin = scratch_dir / "ticks_1s.bin"
        bars_bin = scratch_dir / "bars_15m.bin"
    else:
        scratch_dir.mkdir(parents=True, exist_ok=True)
        ticks, ticks_bin, bars_bin = export_binaries(scratch_dir)

    signals_csv = scratch_dir / "signals.csv"
    cmd = [str(DRIVER_BIN), str(ticks_bin), str(bars_bin), str(signals_csv)]
    if args.imbalance_threshold is not None:
        cmd.append(str(args.imbalance_threshold))
    print(f"running driver: {' '.join(cmd)}")
    subprocess.run(cmd, check=True)

    if args.calibrate_only:
        if not args.reuse_binaries:
            scratch_ctx.cleanup()
        return

    signals = pl.read_csv(signals_csv)
    print(f"\ntotal signals (both variants, before forward-return filtering): {len(signals)}")

    signals = compute_forward_returns(signals, ticks, args.horizon_minutes)
    print(f"signals with a valid {args.horizon_minutes}-min forward return: {len(signals)}\n")

    print(f"=== Forward-return / hit-rate report (horizon={args.horizon_minutes}min) ===")
    report(signals, "time_bar")
    report(signals, "activity_clock")

    print(
        "\n[note] no same-timestamp score correlation computed here -- the two variants fire "
        "signals on different native clocks (15-min bars vs imbalance bars), so a naive "
        "paired correlation would require resampling one onto the other's grid, which this "
        "script deliberately does not do implicitly. The forward-return/hit-rate comparison "
        "above is the decision-relevant test per the sibling's verdict."
    )

    if not args.reuse_binaries:
        scratch_ctx.cleanup()


if __name__ == "__main__":
    main()
