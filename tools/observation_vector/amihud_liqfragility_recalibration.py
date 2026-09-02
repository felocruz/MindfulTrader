#!/usr/bin/env python3
"""amihud_liqfragility_recalibration.py -- exports real MES 1-second bars
(high, low, close, volume) to raw binary for tools/
amihud_liqfragility_recalibration.cpp, which replays the REAL, now-live-
reactive CalculateAmihudIlliquidity/CalculateLiquidityFragility logic
(2026-08-29, docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-
vector-brainstorm.md §1.11) against it, feeds the resulting raw values
through the REAL FeatureScaler (include/FeatureScaler.h) via
UpdateAndNormalize(), and reports exceedance rates against the current
(pre-recalibration) winsorization bounds for dims 12/13.

Required follow-on named explicitly (not optional) in SCRATCHPAD.md -- the
old bounds were calibrated against the bar-gated (lagging) distribution,
which no longer describes what these two dims measure.

Usage: mamba run -n mts python tools/observation_vector/amihud_liqfragility_recalibration.py \
           [--live-bar-min-volume 10]
"""
import argparse
import struct
import subprocess
from pathlib import Path

import numpy as np
import polars as pl

WORKSPACE = Path("/home/rcruz/devel/VSCode")
TICKS_PARQUET = WORKSPACE / "lbrnet/data/raw/mes_continuous_ticks.parquet"
MINDFULTRADER = WORKSPACE / "MindfulTrader"
DRIVER_SRC = MINDFULTRADER / "tools/observation_vector/amihud_liqfragility_recalibration.cpp"
DRIVER_BIN = MINDFULTRADER / "tools/amihud_liqfragility_recalibration"
VCPKG_JSON_INCLUDE = "/mnt/c/Users/rcruz/vcpkg/installed/x64-windows/include"
BINARY_PATH = Path("/tmp/amihud_liqfrag_ticks.bin")


def build_driver():
    cmd = [
        "g++", "-O2", "-std=c++17",
        "-I", str(MINDFULTRADER / "include"),
        "-I", VCPKG_JSON_INCLUDE,
        str(DRIVER_SRC), "-o", str(DRIVER_BIN),
    ]
    print(f"building driver: {' '.join(cmd)}")
    subprocess.run(cmd, check=True)


def write_binary(path: Path, timestamp_us: np.ndarray, *float_cols: np.ndarray):
    """[int64 count][int64 timestamp_us x count][float32 col x count]..."""
    count = timestamp_us.shape[0]
    with open(path, "wb") as f:
        f.write(struct.pack("<q", count))
        f.write(timestamp_us.astype("<i8", copy=False).tobytes())
        for col in float_cols:
            f.write(col.astype("<f4", copy=False).tobytes())


def export_binary():
    if BINARY_PATH.exists():
        print(f"reusing existing export at {BINARY_PATH}")
        return
    ticks = (
        pl.scan_parquet(TICKS_PARQUET)
        .select("timestamp_us", "high", "low", "close", "volume")
        .sort("timestamp_us")
        .collect()
    )
    write_binary(
        BINARY_PATH,
        ticks["timestamp_us"].to_numpy(),
        ticks["high"].to_numpy(),
        ticks["low"].to_numpy(),
        ticks["close"].to_numpy(),
        ticks["volume"].to_numpy(),
    )
    print(f"exported {len(ticks)} 1-second bars to {BINARY_PATH}")


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--live-bar-min-volume", type=float, default=10.0,
                         help="kLiveBarMinVolume guard candidate to evaluate")
    args = parser.parse_args()

    export_binary()
    build_driver()

    cmd = [str(DRIVER_BIN), str(BINARY_PATH), str(args.live_bar_min_volume)]
    print(f"running driver: {' '.join(cmd)}")
    subprocess.run(cmd, check=True)


if __name__ == "__main__":
    main()
