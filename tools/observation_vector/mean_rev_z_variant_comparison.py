#!/usr/bin/env python3
"""mean_rev_z_variant_comparison.py -- empirical additive-vs-replace decision
for mean_rev_z/fast_mean_rev_z (docs/superpowers/plans/2026-08-28-activity-
clock-mean-rev-hurst-recurrence.md Task 5, per the sibling's revised verdict
in SCRATCHPAD.md: correlation alone does not decide this -- feed each
variant through Scoring.cpp:305's actual gate condition (score > 2.0) and
measure forward-return/hit-rate on the resulting signals independently).

Uses polars (columnar, lazy) for parquet I/O and numpy for the C++ driver's
raw binary export -- NOT pandas/.to_csv() (a multi-million-row CSV round-trip
is the wrong tool for this data volume; contiguous binary arrays are what the
C++ driver actually wants to fread(), not text it has to parse).

Pipeline:
1. Build 1-second and 15-minute bars from the raw tick file
   (lbrnet/data/raw/mes_ticks.parquet -- REBUILT 2026-10-13: the two
   pre-aggregated inputs this tool originally read, mes_continuous_ticks.parquet
   and mes_ripple_15m.parquet, no longer exist). Contracts are stitched with
   MULTIPLICATIVE back-adjustment at each roll (log-returns preserved exactly --
   both scores are log-return functions; additive adjustment would distort
   them). Every bar is stamped with its CLOSE time (bucket end), so a signal's
   timestamp is the earliest moment it was knowable -- no look-ahead overlap
   between a 15-min bar and its own forward window. Cached in --data-dir.
2. Run the C++ driver (tools/observation_vector/mean_rev_z_variant_comparison.cpp,
   REAL ActivityClockMeanRevZ header + a faithful CalculateMeanReversionSpeed
   port) to generate gate-crossing signals for both variants.
3. For each signal and each horizon, look up the price `horizon` later in the
   1-second series and compute the forward log-return.
4. "Hit" = forward return has the OPPOSITE sign of the signal's own signed
   deviation (stretched one way, moved back the other -- literal reversion).
5. Report per variant x horizon, three ways:
   - raw: every signal, zero-return ties counted as misses (the original
     definition, kept only for comparability with earlier runs; biased low,
     and its p-value assumes independence the signals do not have)
   - ties excluded: zero forward returns dropped
   - DECLUSTERED (primary): ties excluded AND only signals at least one
     horizon apart are kept, so forward windows never overlap -- consecutive
     signals from one stretch episode are not independent evidence.
     Bonferroni-corrected across every variant x horizon cell.

The full report is archived to tools/output/<tool>_<timestamp>.txt and a row
appended to tools/RECALIBRATION_LEDGER.md (the Python equivalent of
ToolProgressLogger's TOP-LEVEL DIRECTIVE, which only exists in C++).

Usage: mamba run -n mts python tools/observation_vector/mean_rev_z_variant_comparison.py \
           [--horizon-minutes 15 30 60 120 240] [--imbalance-threshold 900] \
           [--data-dir DIR] [--calibrate-only]
"""

import argparse
import datetime as dt
import math
import struct
import subprocess
import sys
from pathlib import Path

import numpy as np
import polars as pl


WORKSPACE = Path("/home/rcruz/devel/VSCode")
RAW_TICKS_PARQUET = WORKSPACE / "lbrnet/data/raw/mes_ticks.parquet"
MINDFULTRADER = WORKSPACE / "MindfulTrader"
DRIVER_SRC = MINDFULTRADER / "tools/observation_vector/mean_rev_z_variant_comparison.cpp"
DRIVER_BIN = MINDFULTRADER / "tools/bin/mean_rev_z_variant_comparison"
OUTPUT_DIR = MINDFULTRADER / "tools/output"
LEDGER = MINDFULTRADER / "tools/RECALIBRATION_LEDGER.md"
TOOL_NAME = "mean_rev_z_variant_comparison"

SECOND_US = 1_000_000
BAR_15M_US = 15 * 60 * SECOND_US
VARIANTS = ("time_bar", "activity_clock")


class Tee:
    """Mirror stdout into the archive file -- every result line lands in tools/output/."""

    def __init__(self, path: Path):
        self.file = open(path, "w")
        self.stdout = sys.stdout

    def write(self, s):
        self.stdout.write(s)
        self.file.write(s)

    def flush(self):
        self.stdout.flush()
        self.file.flush()


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


def read_binary_ts_close(path: Path) -> tuple:
    """Read back timestamp_us + the first float column (close) from write_binary's layout."""
    with open(path, "rb") as f:
        (count,) = struct.unpack("<q", f.read(8))
        ts = np.frombuffer(f.read(8 * count), dtype="<i8")
        close = np.frombuffer(f.read(4 * count), dtype="<f4").astype(np.float64)
    return ts, close


def back_adjusted_ticks() -> pl.LazyFrame:
    """Raw ticks with a multiplicative roll adjustment applied to trade_price.

    Contracts in mes_ticks.parquet are strictly sequential (no overlap, verified
    2026-10-13: 14 contracts, 13 switches, monotonic timestamps), so each roll
    ratio is first_price(new) / last_price(old). Every earlier contract is scaled
    by the cumulative product of the ratios after it, i.e. history is expressed
    in the newest contract's price level. Rolls fall on the Sunday-night
    boundary, so the one genuine weekend gap per roll is absorbed into the
    adjustment -- harmless here: forward windows spanning a weekend are already
    rejected by compute_forward_returns' gap guard.
    """
    ticks = pl.scan_parquet(RAW_TICKS_PARQUET)
    spans = (
        ticks.group_by("contract")
        .agg(
            pl.col("timestamp_us").min().alias("first_ts"),
            pl.col("trade_price").sort_by("timestamp_us").first().alias("p_first"),
            pl.col("trade_price").sort_by("timestamp_us").last().alias("p_last"),
        )
        .sort("first_ts")
        .collect()
    )
    ratios = spans["p_first"].to_numpy()[1:] / spans["p_last"].to_numpy()[:-1]
    # factor[i] = product of every roll ratio AFTER contract i; the newest contract stays 1.0.
    factors = np.append(np.cumprod(ratios[::-1])[::-1], 1.0)
    for name, r in zip(spans["contract"].to_list()[1:], ratios):
        print(f"  roll -> {name}: ratio {r:.6f}")
    factor_map = pl.DataFrame({"contract": spans["contract"], "adj": factors})
    return ticks.join(factor_map.lazy(), on="contract").with_columns(
        (pl.col("trade_price") * pl.col("adj")).alias("price")
    )


def prepare_inputs(data_dir: Path) -> tuple:
    ticks_bin = data_dir / "ticks_1s.bin"
    bars_bin = data_dir / "bars_15m.bin"
    if ticks_bin.exists() and bars_bin.exists():
        print(f"reusing cached bar binaries in {data_dir}")
        return ticks_bin, bars_bin

    data_dir.mkdir(parents=True, exist_ok=True)
    print(f"building bars from {RAW_TICKS_PARQUET} (multiplicative roll back-adjustment)")
    adj = back_adjusted_ticks()

    # Bucket by floor(ts / width) and stamp with the bucket END (close time).
    one_s = (
        adj.with_columns(((pl.col("timestamp_us") // SECOND_US + 1) * SECOND_US).alias("bar_ts"))
        .group_by("bar_ts")
        .agg(
            pl.col("price").sort_by("timestamp_us").last().alias("close"),
            pl.col("bid_volume").sum().cast(pl.Float64),
            pl.col("ask_volume").sum().cast(pl.Float64),
        )
        .sort("bar_ts")
        .collect()
    )
    write_binary(
        ticks_bin,
        one_s["bar_ts"].to_numpy(),
        one_s["close"].to_numpy(),
        one_s["bid_volume"].to_numpy(),
        one_s["ask_volume"].to_numpy(),
    )
    print(f"exported {len(one_s)} 1-second bars to {ticks_bin}")

    bars = (
        adj.with_columns(((pl.col("timestamp_us") // BAR_15M_US + 1) * BAR_15M_US).alias("bar_ts"))
        .group_by("bar_ts")
        .agg(pl.col("price").sort_by("timestamp_us").last().alias("close"))
        .sort("bar_ts")
        .collect()
    )
    write_binary(bars_bin, bars["bar_ts"].to_numpy(), bars["close"].to_numpy())
    print(f"exported {len(bars)} 15-min bars to {bars_bin}")
    return ticks_bin, bars_bin


def compute_forward_returns(
    signals: pl.DataFrame, ts: np.ndarray, px: np.ndarray, horizon_minutes: int
) -> pl.DataFrame:
    horizon_us = horizon_minutes * 60 * SECOND_US
    sig_ts = signals["timestamp_us"].to_numpy()
    # Entry price from the exact 1s series, NOT the CSV's "price" column: the driver
    # writes it at default ostream precision (6 significant digits), which rounds
    # back-adjusted prices and turns genuine zero-return ties into sign noise. Both
    # bar types are stamped at bucket END, so the last 1s bar at-or-before the
    # signal timestamp carries the exact close (identical bar for activity-clock
    # signals; the 15-min bar's final traded second for time-bar signals).
    entry_idx = np.searchsorted(ts, sig_ts, side="right") - 1
    sig_price = px[entry_idx]
    max_rel_err = np.max(np.abs(sig_price / signals["price"].to_numpy() - 1.0))
    if max_rel_err > 1e-5:
        raise ValueError(f"1s entry price disagrees with the driver's ({max_rel_err:.2e}) -- bar stamping mismatch")
    idx = np.searchsorted(ts, sig_ts + horizon_us)

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


def decluster(ts: np.ndarray, gap_us: int) -> np.ndarray:
    """Greedy keep-mask: keep a signal only if >= gap_us after the last KEPT one (ts sorted)."""
    keep = np.zeros(len(ts), dtype=bool)
    next_allowed = np.iinfo(np.int64).min
    for i, t in enumerate(ts):
        if t >= next_allowed:
            keep[i] = True
            next_allowed = t + gap_us
    return keep


def wilson_ci(k: int, n: int, z: float = 1.96) -> tuple:
    """Wilson score interval -- correct coverage near p=0.5 even at these n, unlike a naive p +/- 1.96*se."""
    phat = k / n
    denom = 1 + z * z / n
    center = (phat + z * z / (2 * n)) / denom
    half = (z / denom) * np.sqrt(phat * (1 - phat) / n + z * z / (4 * n * n))
    return center - half, center + half


def hit_stats(fwd: np.ndarray, dev: np.ndarray, count_ties_as_miss: bool) -> dict:
    if not count_ties_as_miss:
        nonzero = fwd != 0
        fwd, dev = fwd[nonzero], dev[nonzero]
    n = len(fwd)
    if n == 0:
        return {"n": 0}
    hits = np.sign(fwd) == -np.sign(dev)
    k = int(hits.sum())
    hit_rate = k / n
    # Two-sided z-test against the null hit_rate = 0.5 (pure chance).
    z = (hit_rate - 0.5) / np.sqrt(0.25 / n)
    p_value = float(2 * (1 - 0.5 * (1 + math.erf(abs(z) / math.sqrt(2)))))
    lo, hi = wilson_ci(k, n)
    # Mean forward return in the REVERSION direction (positive = reverted on average).
    rev_ret = -np.sign(dev) * fwd
    return {"n": n, "hit": hit_rate, "lo": lo, "hi": hi, "z": z, "p": p_value, "rev_bp": 1e4 * float(np.mean(rev_ret))}


def fmt(label: str, s: dict, extra: str = "") -> str:
    if s["n"] == 0:
        return f"    {label:<16} n=0"
    return (
        f"    {label:<16} n={s['n']:>9}  hit={s['hit']:.4f}  95%CI=[{s['lo']:.4f}, {s['hi']:.4f}]  "
        f"z={s['z']:+6.2f}  p={s['p']:.4f}{extra}  mean_reversion_ret={s['rev_bp']:+.2f}bp"
    )


def append_ledger(params: str, archive: Path):
    stamp = dt.datetime.now().strftime("%Y-%m-%d %H:%M")
    rel = archive.relative_to(MINDFULTRADER)
    with open(LEDGER, "a") as f:
        f.write(f"| {stamp} | {TOOL_NAME} | {params} | {rel} | PENDING REVIEW |\n")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--horizon-minutes",
        type=int,
        nargs="+",
        default=[15, 30, 60, 120, 240],
        help="Forward-return horizon(s) in minutes, applied identically to both variants",
    )
    parser.add_argument(
        "--imbalance-threshold",
        type=float,
        default=None,
        help="Activity-clock imbalance threshold -- calibrate bar-formation rate against the "
        "15-min bar count first (--calibrate-only); the driver's built-in default of 15 was "
        "found invalid 2026-08-28 (147x oversampled)",
    )
    parser.add_argument(
        "--calibrate-only",
        action="store_true",
        help="Just run the driver and print bar-formation/signal counts, skip forward-return stats",
    )
    parser.add_argument(
        "--data-dir",
        type=str,
        default=str(MINDFULTRADER / "tools/output/mean_rev_z_variant_comparison_data"),
        help="Where the derived 1s/15m bar binaries are cached between runs",
    )
    args = parser.parse_args()

    OUTPUT_DIR.mkdir(parents=True, exist_ok=True)
    archive = OUTPUT_DIR / f"{TOOL_NAME}_{dt.datetime.now().strftime('%Y%m%d_%H%M%S')}.txt"
    sys.stdout = Tee(archive)
    print(f"{TOOL_NAME}  args={vars(args)}")

    build_driver()
    data_dir = Path(args.data_dir)
    ticks_bin, bars_bin = prepare_inputs(data_dir)

    signals_csv = data_dir / f"signals_thr{args.imbalance_threshold}.csv"
    cmd = [str(DRIVER_BIN), str(ticks_bin), str(bars_bin), str(signals_csv)]
    if args.imbalance_threshold is not None:
        cmd.append(str(args.imbalance_threshold))
    print(f"running driver: {' '.join(cmd)}")
    sys.stdout.flush()
    driver = subprocess.run(cmd, check=True, capture_output=True, text=True)
    print(driver.stderr.rstrip())

    params = f"threshold={args.imbalance_threshold} horizons={args.horizon_minutes}"
    if args.calibrate_only:
        append_ledger(params + " (calibrate-only)", archive)
        return

    ts, px = read_binary_ts_close(ticks_bin)
    signals = pl.read_csv(signals_csv).sort("timestamp_us")
    print(f"\ntotal signals (both variants, before forward-return filtering): {len(signals)}")

    n_tests = len(VARIANTS) * len(args.horizon_minutes)
    alpha_bonf = 0.05 / n_tests
    print(
        f"\nPRIMARY test = declustered (non-overlapping forward windows, ties excluded); "
        f"Bonferroni over {n_tests} cells -> significant only if p < {alpha_bonf:.5f}"
    )

    for horizon in args.horizon_minutes:
        fr = compute_forward_returns(signals, ts, px, horizon)
        print(f"\n=== horizon = {horizon} min ===")
        for variant in VARIANTS:
            sub = fr.filter(pl.col("variant") == variant)
            fwd = sub["forward_return"].to_numpy()
            dev = sub["signed_deviation"].to_numpy()
            sts = sub["timestamp_us"].to_numpy()
            nonzero = fwd != 0
            keep = decluster(sts[nonzero], horizon * 60 * SECOND_US)
            primary = hit_stats(fwd[nonzero][keep], dev[nonzero][keep], count_ties_as_miss=False)
            ties = 1.0 - nonzero.mean() if len(fwd) else float("nan")
            verdict = ""
            if primary["n"]:
                verdict = "  ** SIGNIFICANT (Bonferroni) **" if primary["p"] < alpha_bonf else "  (not significant)"
            print(f"  {variant}  (zero-return ties: {ties:.2%})")
            print(fmt("raw", hit_stats(fwd, dev, count_ties_as_miss=True)))
            print(fmt("ties excluded", hit_stats(fwd, dev, count_ties_as_miss=False)))
            print(fmt("DECLUSTERED", primary, verdict))

    print(
        "\n[note] no same-timestamp score correlation computed here -- the two variants fire "
        "signals on different native clocks (15-min bars vs imbalance bars), so a naive "
        "paired correlation would require resampling one onto the other's grid, which this "
        "script deliberately does not do implicitly."
    )
    print(f"\narchived to {archive}")
    append_ledger(params, archive)


if __name__ == "__main__":
    main()
