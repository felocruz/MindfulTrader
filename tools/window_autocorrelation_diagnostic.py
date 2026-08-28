#!/usr/bin/env python3
"""Autocorrelation-time diagnostic for the observation-vector-institutional-
hardening spec's window-widening question (docs/superpowers/specs/2026-08-25-
observation-vector-institutional-hardening-spec.md Section 5/8): how many
bars should recurrence_rate/fractal_dim (TS2, 60min) and mean_rev_z's outer
z-score + inner rho (TS3, 15min) span, derived from real data rather than
the spec's own unvalidated ~150/~600-bar proposals (sized only by analogy to
the HMM's unrelated ~6.1-day regime tenure)?

Method: Politis & White (2004) / Patton, Politis & White (2009) automatic
optimal block-length selection (arch.bootstrap.optimal_block_length) --
the same algorithm this project already depends on (arch>=8.0) for the HMM
gate-threshold calibration's own --window-rows derivation
(docs/superpowers/specs/2026-08-24-hmm-gate-threshold-calibration-
institutional-grade-spec.md). Applied here to log-returns (optimal_block_
length assumes weak stationarity -- not valid on raw non-stationary price
levels), at each cadence, both raw and with the ~12 contract-roll-affected
bars flagged for a sensitivity check (see below).

Data: lbrnet/data/raw/mes_wave_60m.parquet (TS2, 60min) and
mes_ripple_15m.parquet (TS3, 15min) -- already-built continuous-contract bar
series (real MES data, 2023-06-04..2026-08-18), used directly rather than
re-resampled from the 1-second mes_continuous_ticks.parquet, per direct
inspection: the ~12 real contract-roll price jumps (unadjusted splice,
confirmed via mes_continuous_ticks.parquet's own `contract` column, ~0.7-1.2%
each) land inside a single 60-min bar's High-Low range rather than as an
isolated between-bar discontinuity, so contamination is mild at this
cadence -- still flagged explicitly below, not ignored.

Usage (mts env):
    source /home/rcruz/anaconda3/etc/profile.d/conda.sh && mamba activate mts
    python3 tools/window_autocorrelation_diagnostic.py
"""
import sys
from pathlib import Path

import numpy as np
import pandas as pd
from arch.bootstrap import optimal_block_length

WAVE_60M = Path("/home/rcruz/devel/VSCode/lbrnet/data/raw/mes_wave_60m.parquet")
RIPPLE_15M = Path("/home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ripple_15m.parquet")
CONTINUOUS_TICKS = Path("/home/rcruz/devel/VSCode/lbrnet/data/raw/mes_continuous_ticks.parquet")

# Reference point cited by the spec: this HMM's own fitted mean regime
# tenure (production model, 2026-08-25 sign-off run) is ~589 bars at
# TS3/15min, ~6.1 real days. Printed alongside the measured values below for
# direct comparison -- NOT used as an input to the derivation itself.
HMM_MEAN_REGIME_TENURE_BARS_TS3 = 588.8


def load_roll_timestamps() -> list[int]:
    """The ~12 real (unadjusted) contract-roll timestamps, identified directly
    from mes_continuous_ticks.parquet's own `contract` column."""
    ticks = pd.read_parquet(CONTINUOUS_TICKS, columns=["timestamp_us", "contract"])
    ticks = ticks.sort_values("timestamp_us").reset_index(drop=True)
    changed = ticks["contract"] != ticks["contract"].shift(1)
    roll_idxs = ticks.index[changed].tolist()[1:]
    return ticks.loc[roll_idxs, "timestamp_us"].tolist()


def load_bars(path: Path) -> pd.DataFrame:
    df = pd.read_parquet(path).sort_values("timestamp_us").reset_index(drop=True)
    assert df["timestamp_us"].is_monotonic_increasing, f"{path}: timestamps not sorted"
    return df


def flag_roll_bars(df: pd.DataFrame, roll_timestamps: list[int]) -> np.ndarray:
    """Boolean mask, True for the single bar spanning each real contract-roll
    timestamp (the bar whose [prior_ts, this_ts) window contains the roll)."""
    ts = df["timestamp_us"].to_numpy()
    mask = np.zeros(len(df), dtype=bool)
    for roll_ts in roll_timestamps:
        idx = int(np.searchsorted(ts, roll_ts))
        idx = min(idx, len(df) - 1)
        mask[idx] = True
    return mask


def report_block_length(label: str, series: np.ndarray) -> None:
    result = optimal_block_length(series)
    # optimal_block_length returns a DataFrame with columns
    # ['stationary', 'circular'] for the single input series (row 0).
    stationary = float(result["stationary"].iloc[0])
    circular = float(result["circular"].iloc[0])
    print(f"  {label:<45} n={len(series):>7}  "
          f"stationary={stationary:>8.2f} bars  circular={circular:>8.2f} bars")


def main() -> None:
    roll_timestamps = load_roll_timestamps()
    print(f"Identified {len(roll_timestamps)} real contract-roll timestamps "
          f"(unadjusted splice, from mes_continuous_ticks.parquet).\n")

    for label, path, screen in [
        ("TS2 (60min) -- recurrence_rate/fractal_dim", WAVE_60M, "TS2"),
        ("TS3 (15min) -- mean_rev_z", RIPPLE_15M, "TS3"),
    ]:
        print(f"=== {label} ===")
        df = load_bars(path)
        roll_mask = flag_roll_bars(df, roll_timestamps)
        print(f"  {len(df)} bars total, {roll_mask.sum()} flagged as roll-affected "
              f"({roll_mask.sum() / len(df) * 100:.4f}% of bars)")

        close = df["close"].to_numpy(dtype=np.float64)
        log_ret = np.diff(np.log(close))
        # roll_mask is aligned to bars; a return uses bar[i-1]->bar[i], so a
        # roll-affected return is one where EITHER endpoint bar was flagged.
        ret_roll_mask = roll_mask[1:] | roll_mask[:-1]

        report_block_length("log-returns (raw, includes roll bars)", log_ret)
        report_block_length("log-returns (roll-affected bars excluded)",
                             log_ret[~ret_roll_mask])
        report_block_length("|log-returns| (volatility clustering, raw)",
                             np.abs(log_ret))
        report_block_length("|log-returns| (roll-affected bars excluded)",
                             np.abs(log_ret[~ret_roll_mask]))
        print()

    print(f"Reference point (NOT an input to the above, printed for comparison): "
          f"this HMM's own fitted mean regime tenure is "
          f"~{HMM_MEAN_REGIME_TENURE_BARS_TS3:.1f} bars at TS3/15min "
          f"(~{HMM_MEAN_REGIME_TENURE_BARS_TS3 * 15 / 60 / 24:.2f} real days).")
    print(f"Spec's own proposed (unvalidated) targets: recurrence_rate/fractal_dim "
          f"~150 bars (TS2), mean_rev_z ~600 bars (TS3).")


if __name__ == "__main__":
    main()
