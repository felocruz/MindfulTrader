#!/usr/bin/env python3
"""Builds a paired (old_kurtosis, moors_kurtosis) sample from REAL historical
MES intraday price data, for Task 7 (empirical threshold re-validation for
the new kurtosis scale -- see .superpowers/sdd/2026-08-13-observation-vector-
institutional-elevation/task-7-brief.md).

Why this exists instead of a live .btst/backtest replay: this environment
cannot run a live Sierra Chart backtest (no Windows Sierra Chart process
available from WSL). Real historical price data IS available locally as
Sierra Chart .scid files under /mnt/c/SierraChart2/Data/ -- for the MES
contract chain these are 30-second time-based OHLCV bars (verified by
inspecting raw records: uniform 30,000,000us spacing, real trade counts/
volumes during active sessions, real ES-consistent price levels). This
script aggregates those into 15-minute bars -- matching the timeframe
CalculateRealizedKurtosis() actually reads from (TripleScreen3.cpp's 15-
minute chart; see IMPORTANT note below) -- and replicates BOTH the
pre-Task-6 moment-based kurtosis formula and the post-Task-6 Moors octile
formula, plus the shared ATR-regime multiplier and clamp, on IDENTICAL
100-bar rolling return windows. That gives a genuine paired sample without
fabricating any numbers.

IMPORTANT correction vs. the task brief: the brief's Step 1 describes
CalculateRealizedKurtosis as operating on "15-second bars, per the
function's own comments." That in-code comment ("ES 15s log-return
variance...") is misleading. TripleScreen3.cpp -- the only caller of
UpdateObservationVectorSubgraphs / CalculateRealizedKurtosis -- explicitly
documents itself as running on the 15-MINUTE chart (see its own comments:
"calculated on 15-min chart", SG_EMA3/SG_EMA16 "15-min EMAs"). CLAUDE.md's
"Chart Timeframes" section flags exactly this minutes-vs-seconds confusion
as a known trap. sc.Close[] in CalculateRealizedKurtosis is therefore the
15-minute bar close array, not a 15-second one. This script builds 15-
minute bars accordingly.

Usage:
    conda activate mts
    python3 tools/build_paired_kurtosis_sample.py \
        --data-dir /mnt/c/SierraChart2/Data --pattern "MES-*-CME-USD.scid" \
        --out /tmp/paired_kurtosis_sample.csv
"""
import argparse
import glob
import os
import struct
import sys

import numpy as np
import pandas as pd

KURT_WINDOW = 100
KURT_VARIANCE_EPS = 1e-10
VOL_COMPARE_WINDOW = 20
ATR_PERIOD = 10
BAR_SECONDS = 900  # 15 minutes -- see IMPORTANT note above

# .scid format (Sierra Chart Intraday Data): 56-byte header, then fixed-size
# records: int64 SCDateTime(us since 1899-12-30), float32 O,H,L,C,
# int32 NumTrades, TotalVolume, BidVolume, AskVolume.
SCID_RECORD_DTYPE = np.dtype([
    ("dt", "<i8"), ("o", "<f4"), ("h", "<f4"), ("l", "<f4"), ("c", "<f4"),
    ("numtrades", "<i4"), ("totalvolume", "<i4"), ("bidvolume", "<i4"), ("askvolume", "<i4"),
])


def read_scid(path: str) -> np.ndarray:
    with open(path, "rb") as f:
        header = f.read(56)
        if header[:4] != b"SCID":
            raise ValueError(f"{path}: not a .scid file (bad magic)")
        header_size, record_size = struct.unpack_from("<II", header, 4)
        if record_size != SCID_RECORD_DTYPE.itemsize:
            raise ValueError(f"{path}: unexpected record size {record_size}")
        f.seek(header_size)
        return np.fromfile(f, dtype=SCID_RECORD_DTYPE)


def aggregate_to_bars(records: np.ndarray, bar_seconds: int) -> pd.DataFrame:
    """30s (or whatever native granularity) records -> OHLCV bars of bar_seconds."""
    if len(records) == 0:
        return pd.DataFrame(columns=["bucket", "open", "high", "low", "close", "volume"])
    df = pd.DataFrame({
        "dt_us": records["dt"],
        "o": records["o"], "h": records["h"], "l": records["l"], "c": records["c"],
        "v": records["totalvolume"],
    })
    bucket_us = bar_seconds * 1_000_000
    df["bucket"] = df["dt_us"] // bucket_us
    bars = df.groupby("bucket").agg(
        open=("o", "first"), high=("h", "max"), low=("l", "min"),
        close=("c", "last"), volume=("v", "sum"),
    ).reset_index()
    return bars


def empirical_quantile(sorted_arr: np.ndarray, p: float) -> float:
    """R Type-7 linear-interpolation quantile, matching RobustMoments.h's
    EmpiricalQuantile() over a pre-sorted window."""
    n = len(sorted_arr)
    idx = p * (n - 1)
    lo = int(np.floor(idx))
    hi = int(np.ceil(idx))
    if lo == hi:
        return float(sorted_arr[lo])
    frac = idx - lo
    return float(sorted_arr[lo] + frac * (sorted_arr[hi] - sorted_arr[lo]))


def moors_kurtosis(returns_sorted: np.ndarray) -> float:
    q1_8 = empirical_quantile(returns_sorted, 1.0 / 8.0)
    q2_8 = empirical_quantile(returns_sorted, 2.0 / 8.0)
    q3_8 = empirical_quantile(returns_sorted, 3.0 / 8.0)
    q5_8 = empirical_quantile(returns_sorted, 5.0 / 8.0)
    q6_8 = empirical_quantile(returns_sorted, 6.0 / 8.0)
    q7_8 = empirical_quantile(returns_sorted, 7.0 / 8.0)
    denom = q6_8 - q2_8
    if abs(denom) < 1e-10:
        return float("nan")
    return (q7_8 - q5_8 + q3_8 - q1_8) / denom


def old_moment_kurtosis(returns: np.ndarray, mean_ret: float, variance: float) -> float:
    """Pre-Task-6 formula, reconstructed verbatim from git show 298b9e0~1 --
    src/StudyHelperFunctions.cpp (Sierra Chart's biased-then-corrected
    sample-kurtosis estimator over the fixed 100-bar window)."""
    n = float(KURT_WINDOW)
    diffs = returns - mean_ret
    m4 = float(np.mean(diffs ** 4))
    adjustment = ((n - 1.0) * (n + 1.0)) / ((n - 2.0) * (n - 3.0))
    bias_correction = 3.0 * (n - 1.0) * (n - 1.0) / ((n - 2.0) * (n - 3.0))
    var_squared = variance * variance
    return adjustment * (m4 / var_squared) - bias_correction


def wilders_atr(bars: pd.DataFrame, period: int) -> np.ndarray:
    high = bars["high"].to_numpy(dtype=np.float64)
    low = bars["low"].to_numpy(dtype=np.float64)
    close = bars["close"].to_numpy(dtype=np.float64)
    n = len(bars)
    tr = np.empty(n)
    tr[0] = high[0] - low[0]
    for i in range(1, n):
        tr[i] = max(high[i] - low[i], abs(high[i] - close[i - 1]), abs(low[i] - close[i - 1]))
    atr = np.empty(n)
    atr[0] = tr[0]
    for i in range(1, n):
        atr[i] = atr[i - 1] + (tr[i] - atr[i - 1]) / period
    return atr


def process_contract(path: str) -> pd.DataFrame:
    records = read_scid(path)
    if len(records) < 1000:
        return pd.DataFrame()
    bars = aggregate_to_bars(records, BAR_SECONDS)
    if len(bars) < KURT_WINDOW + VOL_COMPARE_WINDOW + 2:
        return pd.DataFrame()

    closes = bars["close"].to_numpy(dtype=np.float32)
    atr = wilders_atr(bars, ATR_PERIOD)

    rows = []
    prev_old = np.nan
    prev_new = np.nan
    n_bars = len(bars)
    for idx in range(KURT_WINDOW, n_bars):
        # returns[i] = log(Close[idx-i] / Close[idx-i-1]) for i in 0..99, matching
        # CalculateRealizedKurtosis's std::array fill order exactly.
        window_closes = closes[idx - KURT_WINDOW: idx + 1].astype(np.float64)
        prevs = np.maximum(window_closes[:-1], 0.001)
        curs = window_closes[1:]
        returns = np.log(curs / prevs)[::-1]  # i=0 -> most recent, matching C++ loop
        mean_ret = float(np.mean(returns))
        variance = float(np.mean((returns - mean_ret) ** 2))
        if variance < KURT_VARIANCE_EPS:
            continue  # matches production's carry-forward branch -- not a fresh sample

        old_k = old_moment_kurtosis(returns, mean_ret, variance)
        sorted_returns = np.sort(returns)
        new_k = moors_kurtosis(sorted_returns)
        if np.isnan(new_k):
            continue

        # Shared ATR-regime multiplier (identical mechanism, applied to both
        # formulas' output, matching CalculateRealizedKurtosis lines 2683-2700).
        if idx >= VOL_COMPARE_WINDOW:
            atr_avg = float(np.mean(atr[idx - VOL_COMPARE_WINDOW + 1: idx + 1]))
            vol_ratio = atr[idx] / max(atr_avg, 0.0001)
            regime_mult = 1.0
            if vol_ratio > 1.3:
                regime_mult = 1.25
            if vol_ratio < 0.7:
                regime_mult = 0.75
            old_k *= regime_mult
            new_k *= regime_mult

        old_k = float(np.clip(old_k, -5.0, 50.0))
        new_k = float(np.clip(new_k, -5.0, 50.0))
        prev_old, prev_new = old_k, new_k

        rows.append({
            "contract": os.path.basename(path),
            "bar_index": idx,
            "old_kurtosis": old_k,
            "moors_kurtosis": new_k,
        })
    return pd.DataFrame(rows)


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--data-dir", default="/mnt/c/SierraChart2/Data")
    ap.add_argument("--pattern", default="MES-*-CME-USD.scid")
    ap.add_argument("--out", default="/tmp/paired_kurtosis_sample.csv")
    ap.add_argument("--min-bytes", type=int, default=1_000_000,
                     help="skip .scid files smaller than this (empty/placeholder contracts)")
    args = ap.parse_args()

    paths = sorted(glob.glob(os.path.join(args.data_dir, args.pattern)))
    paths = [p for p in paths if os.path.getsize(p) >= args.min_bytes]
    if not paths:
        print(f"No .scid files >= {args.min_bytes} bytes matching {args.pattern} in {args.data_dir}", file=sys.stderr)
        sys.exit(1)

    print(f"Processing {len(paths)} contract files:", file=sys.stderr)
    frames = []
    for p in paths:
        print(f"  {p} ({os.path.getsize(p):,} bytes)", file=sys.stderr)
        df = process_contract(p)
        print(f"    -> {len(df)} paired samples", file=sys.stderr)
        frames.append(df)

    full = pd.concat(frames, ignore_index=True) if frames else pd.DataFrame()
    full.to_csv(args.out, index=False)
    print(f"Wrote {len(full)} paired samples to {args.out}", file=sys.stderr)


if __name__ == "__main__":
    main()
