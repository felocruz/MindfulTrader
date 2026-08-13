#!/usr/bin/env python3
"""Maps each old-kurtosis threshold constant to its Moors-kurtosis equivalent
via percentile-matching on a paired (old, new) sample collected per Task 7
Step 1 (see tools/build_paired_kurtosis_sample.py -- this environment could
not run a live .btst/Sierra Chart replay, so the paired sample is built from
real historical MES .scid intraday price data instead; see that script's
docstring and .superpowers/sdd/2026-08-13-observation-vector-institutional-
elevation/task-7-report.md for the full data-provenance writeup).

Run after collecting the paired CSV; prints the mapped thresholds -- does
not modify any C++ files itself (that's a manual follow-up commit once the
mapping is reviewed).

NOTE on OLD_THRESHOLDS vs. the task-7 brief: the brief's draft dict included
a "cascade_gate_high (RiskManager.cpp:1756)": 2.5 entry. That line's `rk`
variable is `ContextManager::GetCachedHillAlpha()` -- a Hill tail-index
estimator, a completely different statistic from Taleb kurtosis and NOT
touched by Task 6's Moors-kurtosis swap. It is deliberately excluded here.
Only RiskManager.cpp:1758's `lrc.talebKurtosis < 3.0f` (kept below as
"cascade_gate_low") is a genuine kurtosis threshold at that call site.
"""
import sys
import csv
import numpy as np

OLD_THRESHOLDS = {
    "isFragile (Indicator.cpp:508)": 4.0,
    "scoring_sigmoid_gate (Scoring.cpp:201)": 2.5,
    "scoring_sigmoid_center (Scoring.cpp:200)": 6.0,
    "talebKurtosisCrisisExit (ExecutionParams.h:59)": 3.0,
    "talebKurtosisCrisisEnter (ExecutionParams.h:58)": 5.0,
    "talebKurtosisHaltThreshold (ExecutionParams.h:64)": 15.0,
    "fatTail_literal (RiskManager.cpp:805)": 8.0,
    "cascade_gate_low (RiskManager.cpp:1758, lrc.talebKurtosis)": 3.0,
}


def main(csv_path: str) -> None:
    old_vals, new_vals = [], []
    with open(csv_path, newline="") as f:
        for row in csv.DictReader(f):
            old_vals.append(float(row["old_kurtosis"]))
            new_vals.append(float(row["moors_kurtosis"]))
    old_arr = np.array(old_vals)
    new_arr = np.array(new_vals)

    print(f"Paired sample size: {len(old_arr)}")
    print(f"old_kurtosis:   mean={old_arr.mean():.3f} median={np.median(old_arr):.3f} "
          f"p90={np.percentile(old_arr,90):.3f} p99={np.percentile(old_arr,99):.3f}")
    print(f"moors_kurtosis: mean={new_arr.mean():.3f} median={np.median(new_arr):.3f} "
          f"p90={np.percentile(new_arr,90):.3f} p99={np.percentile(new_arr,99):.3f}")
    print()
    for name, old_threshold in OLD_THRESHOLDS.items():
        percentile = float((old_arr <= old_threshold).mean() * 100.0)
        mapped = float(np.percentile(new_arr, percentile))
        print(f"{name}: old={old_threshold} (P{percentile:.1f}) -> new={mapped:.4f}")


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print("Usage: analyze_kurtosis_threshold_migration.py <paired_sample.csv>")
        sys.exit(1)
    main(sys.argv[1])
