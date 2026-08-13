#!/usr/bin/env python3
"""Task 13 (Unit 6b) spike: does the Hill tail-index estimator (dim 9,
include/TailRiskEngine.h, post-Task-4: Hill-plot stability-region k-selection +
EWMA-smoothed output) vary systematically by session time-of-day on real MES
15-min bar data?

Supersedes task-13-brief.md's Step 1 (.context-file reading via lbrnet
tooling) -- that data source is no longer the recommended path. Real, clean
15-min MES bar data was found at
lbrnet/data/raw/mes_ripple_15m.parquet (75,085 rows, columns
open/high/low/close/timestamp_us/bar_type, real market data mid-2023 onward)
and is used directly instead. Step 2/3/4 (analysis question, output format,
Changelog recording) still apply.

Pipeline (for exact reproducibility from scratch):
  1. Export parquet timestamp_us/close to CSV, chronological.
  2. Feed those closes through the REAL, unmodified production
     MindfulTrader::TailRiskEngine (tools/hill_intraday_seasonality.cpp,
     which #includes include/TailRiskEngine.h directly -- not a
     reimplementation) to get one (timestamp_us, alpha) row per bar, driven
     exactly the way src/SCStudies.cpp:287-306 documents for "historical
     load" mode (one log-return AddObservation() per completed bar) and
     read/clamped exactly as ContextManager.cpp:512-519 does.
  3. Convert timestamp_us (verified genuine UTC microseconds -- see the
     DST/maintenance-break sanity check below) to America/New_York wall time
     and bucket into five session-time-of-day windows.
  4. Compare per-bucket mean/std, and run a Kruskal-Wallis test (non-
     parametric, appropriate given alpha's visibly non-Gaussian/skewed
     distribution) for "do the bucket distributions differ beyond noise."

Timestamp verification performed before trusting any of this (see task-13
report for detail): timestamp_us/1e6 converted to America/New_York wall time
reproduces two independently-known CME/Globex facts with no free parameters
-- (a) hour=17 (5-6pm ET) is completely absent from the bar-count-by-hour
histogram, matching the CME daily maintenance break exactly; (b) weekday
counts show zero Saturday bars and a reduced Sunday count (evening-only
open), matching the Sun 6pm ET - Fri 5pm ET futures week exactly. Both are
real, checkable market-structure facts, not something a naive UTC-offset
guess would reproduce by chance.

Usage (mamba mts env):
    source /home/rcruz/anaconda3/etc/profile.d/conda.sh && mamba activate mts
    cd /home/rcruz/devel/VSCode/MindfulTrader/tools
    g++ -O2 -std=c++17 -I../include hill_intraday_seasonality.cpp -o hill_intraday_seasonality
    python3 hill_intraday_seasonality.py
"""
import subprocess
import sys
from pathlib import Path

import numpy as np
import pandas as pd

PARQUET_PATH = Path("/home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ripple_15m.parquet")
TOOLS_DIR = Path(__file__).resolve().parent
DRIVER_SRC = TOOLS_DIR / "hill_intraday_seasonality.cpp"
DRIVER_BIN = TOOLS_DIR / "hill_intraday_seasonality"
INCLUDE_DIR = TOOLS_DIR.parent / "include"
SCRATCH_DIR = Path("/tmp/claude-1000/-home-rcruz-devel-VSCode-MindfulTrader/"
                    "ad99fc40-0731-4b5e-8b28-7baafc05d38c/scratchpad")
CLOSES_CSV = SCRATCH_DIR / "closes.csv"
ALPHAS_CSV = SCRATCH_DIR / "alphas.csv"

# Session time-of-day buckets, in US/Eastern wall-clock hour-of-day (float,
# half-open [start, end)). CME/Globex convention for ES/MES-family futures:
#   overnight   18:00-02:59  Globex open through the Asia session
#   pre_open    03:00-08:29  London/European session ahead of the US day
#   us_open     08:30-10:29  brackets the 09:30 NYSE cash open (and the 08:30
#                             ET economic-data-release ramp-up that already
#                             moves ES/MES before the cash open)
#   midday      10:30-13:59  post-open settling through early afternoon
#   close       14:00-16:59  last RTH hour (16:00 CME equity-index close) and
#                             the settlement window into the 17:00 maintenance
#                             break
BUCKET_BOUNDS = [
    (18.0, 27.0, "overnight"),   # wraps past midnight, handled via +24 below
    (3.0, 8.5, "pre_open"),
    (8.5, 10.5, "us_open"),
    (10.5, 14.0, "midday"),
    (14.0, 17.0, "close"),
]


def time_bucket(hour_float: float) -> str:
    for lo, hi, name in BUCKET_BOUNDS:
        if lo <= hour_float < hi:
            return name
        if lo <= hour_float + 24.0 < hi:  # overnight wrap
            return name
    return "unassigned"


def export_closes_csv() -> None:
    SCRATCH_DIR.mkdir(parents=True, exist_ok=True)
    df = pd.read_parquet(PARQUET_PATH)
    df = df.sort_values("timestamp_us").reset_index(drop=True)
    assert df["timestamp_us"].is_monotonic_increasing, "timestamps not sorted/unique"
    df[["timestamp_us", "close"]].to_csv(CLOSES_CSV, index=False)
    print(f"exported {len(df)} bars to {CLOSES_CSV}", file=sys.stderr)


def build_driver_if_needed() -> None:
    if DRIVER_BIN.exists() and DRIVER_BIN.stat().st_mtime > DRIVER_SRC.stat().st_mtime:
        return
    cmd = ["g++", "-O2", "-std=c++17", f"-I{INCLUDE_DIR}", str(DRIVER_SRC), "-o", str(DRIVER_BIN)]
    print(f"building driver: {' '.join(cmd)}", file=sys.stderr)
    subprocess.run(cmd, check=True)


def run_driver() -> None:
    cmd = [str(DRIVER_BIN), str(CLOSES_CSV), str(ALPHAS_CSV)]
    print(f"running driver: {' '.join(cmd)}", file=sys.stderr)
    subprocess.run(cmd, check=True)


def verify_timestamp_semantics(dt_et: pd.Series) -> None:
    """Independent sanity check that timestamp_us really is UTC microseconds
    and that America/New_York conversion is correct, using two known,
    checkable CME/Globex facts that a wrong offset/unit would not reproduce."""
    hour_counts = dt_et.dt.hour.value_counts()
    assert 17 not in hour_counts.index, (
        "Expected the 17:00-18:00 ET CME maintenance-break hour to be absent "
        "from the bar histogram -- timestamp/timezone interpretation may be wrong."
    )
    weekday_counts = dt_et.dt.dayofweek.value_counts()
    assert weekday_counts.get(5, 0) == 0, (
        "Expected zero Saturday (weekday=5) bars for a CME futures product -- "
        "timestamp/timezone interpretation may be wrong."
    )
    print("timestamp/timezone sanity check passed: no 17:00-18:00 ET bars "
          "(CME maintenance break), zero Saturday bars.", file=sys.stderr)


def main() -> None:
    if not CLOSES_CSV.exists():
        export_closes_csv()
    build_driver_if_needed()
    if not ALPHAS_CSV.exists() or ALPHAS_CSV.stat().st_mtime < DRIVER_BIN.stat().st_mtime:
        run_driver()

    df = pd.read_csv(ALPHAS_CSV)
    dt_utc = pd.to_datetime(df["timestamp_us"], unit="us", utc=True)
    dt_et = dt_utc.dt.tz_convert("America/New_York")
    verify_timestamp_semantics(dt_et)

    hour_float = dt_et.dt.hour + dt_et.dt.minute / 60.0
    df["bucket"] = hour_float.apply(time_bucket)

    print(f"\n{len(df)} alpha samples, {df['bucket'].eq('unassigned').sum()} unassigned\n")
    print(f"{'bucket':<12} {'n':>8} {'mean':>10} {'std':>10} {'median':>10}")
    groups = []
    order = ["overnight", "pre_open", "us_open", "midday", "close"]
    for bucket in order:
        vals = df.loc[df["bucket"] == bucket, "alpha"].to_numpy()
        groups.append(vals)
        print(f"{bucket:<12} {len(vals):>8} {vals.mean():>10.4f} {vals.std():>10.4f} "
              f"{np.median(vals):>10.4f}")

    grand_mean = df["alpha"].mean()
    grand_std = df["alpha"].std()
    print(f"\n{'grand':<12} {len(df):>8} {grand_mean:>10.4f} {grand_std:>10.4f} "
          f"{df['alpha'].median():>10.4f}")

    # Effect-size framing: how far apart are bucket means relative to the
    # pooled within-bucket spread (a rough ANOVA-style eta-squared), plus a
    # non-parametric Kruskal-Wallis test for "these are not the same
    # distribution" (robust to alpha's skew -- see .describe() in the
    # investigation, min~1.3, max~8.0, heavy right tail).
    try:
        from scipy import stats
        h_stat, p_value = stats.kruskal(*groups)
        print(f"\nKruskal-Wallis H={h_stat:.2f}, p={p_value:.3e} "
              f"(H0: all buckets drawn from the same distribution)")
    except ImportError:
        print("\n(scipy not available -- skipping Kruskal-Wallis test)", file=sys.stderr)

    bucket_means = np.array([g.mean() for g in groups])
    max_gap = bucket_means.max() - bucket_means.min()
    print(f"max bucket-mean gap: {max_gap:.4f} alpha units "
          f"({max_gap / grand_std:.2f} pooled-SD units, "
          f"{max_gap / grand_mean * 100:.1f}% of grand mean)")

    # Critical caveat: the alpha series is a 500-sample-window Hill estimate
    # further EWMA-smoothed (Task 4, alpha=0.2), so consecutive readings are
    # NOT independent draws -- Kruskal-Wallis's p-value above assumes iid
    # samples within each group and is invalid as reported. Measure the
    # actual serial autocorrelation and re-run the test on a thinned series
    # sampled at roughly the decorrelation lag, which is the honest test.
    alpha_arr = df["alpha"].to_numpy()
    lag1 = np.corrcoef(alpha_arr[:-1], alpha_arr[1:])[0, 1]
    print(f"\nlag-1 autocorrelation of the alpha series: {lag1:.4f} "
          f"(near 1.0 -- consecutive readings are highly non-independent; "
          f"the Kruskal-Wallis p-value above is NOT valid as reported)")

    decorrelation_lag = None
    for lag in range(50, 500, 25):
        r = np.corrcoef(alpha_arr[:-lag], alpha_arr[lag:])[0, 1]
        if r < 0.2:
            decorrelation_lag = lag
            break
    if decorrelation_lag is not None:
        thinned = df.iloc[::decorrelation_lag]
        thinned_groups = [thinned.loc[thinned["bucket"] == b, "alpha"].to_numpy() for b in order]
        print(f"decorrelation lag (first lag with autocorr < 0.2): {decorrelation_lag} bars "
              f"-> thinned to {len(thinned)} approximately-independent samples")
        try:
            from scipy import stats
            h2, p2 = stats.kruskal(*thinned_groups)
            print(f"thinned (honest) Kruskal-Wallis H={h2:.2f}, p={p2:.3f}")
        except ImportError:
            pass


if __name__ == "__main__":
    main()
