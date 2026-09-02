#!/usr/bin/env python3
"""Offline dependence-correction calibration for the two candidate-
validation tools' bootstrap/hit-rate CIs (docs/superpowers/specs/
2026-08-30-bootstrap-dependence-correction-design.md): both
tools/observation_vector/drift_location_eval.cpp's ComputeHitRate and
tools/observation_vector/jump_ratio_eval.cpp's ComputeBootstrapMedianGapCI treat individual
per-tick forward-return signals as i.i.d., but real forward returns
overlap heavily (a 240-minute forward return shares most of its
underlying tick data with thousands of adjacent signals), understating
every reported CI's width.

Method: the design effect (DEFF, Kish 1965: ratio of true to naive-i.i.d.
variance), computed on BOUNDED {0,1} INDICATOR series (hit/miss for
drift_location; below-median for jump_ratio), not on the raw
|forward_return| values. This is deliberate, not an approximation:
ComputeHitRate's hit-rate is itself a mean of a 0/1 indicator, so a
Newey-West (1987) long-run/short-run variance ratio on that indicator is
exactly the right generalization for a mean. ComputeBootstrapMedianGapCI
tests a MEDIAN; per the Bahadur (1966) representation and Sen (1968)'s
dependent-quantile theorem, a sample quantile's asymptotic variance under
dependence is governed by the long-run variance of the below-quantile
indicator process I(X<=q) -- the unobserved marginal density at the
quantile cancels out of the DEFF ratio entirely (verified algebraically:
DEFF_quantile = LRV(I(X<=q)) / Var_iid(I(X<=q)), independent of the
density term), so applying the SAME indicator-transform-then-Newey-West
recipe used for the hit-rate case is the correct generalization for a
median too, per Babu (1986) and Politis (2001)'s treatment of dependent
quantile resampling via HAC estimators on the indicator transform. This
also has the practical benefit that a bounded indicator series is
estimator-stable regardless of how heavy the underlying return
distribution's tails are.

Two independent DEFF estimates are computed and the larger is used --
see this task's own plan-document design note for why (NeweyWest's
Bartlett kernel can underestimate variance for a bounded-overlap MA(L)
process unless its bandwidth comfortably exceeds L; Geyer's (1992)
self-truncating tau_int estimator doesn't have that failure mode, so
taking the max is a conservative cross-check, not an arbitrary tie-break).

Real tick density (verified directly against the production file, not
assumed): 38,547,467 ticks over a 1,685,015.4-minute span = 22.8766
ticks/minute (wall-clock average, including off-hours). Bandwidth for
each horizon is 5x the average tick-count the horizon spans, as a safety
margin against intraday tick-density variability (this is an average,
not a worst-case measurement -- 5x is a deliberate cushion, not derived
from a measured peak).

Data: CSVs exported by tools/observation_vector/drift_location_eval.cpp and
tools/observation_vector/jump_ratio_eval.cpp's --export-signals-dir flag (see that flag's
own doc comment for the exact population each series represents) --
run those tools first if the expected files below don't exist.

Real runtime cost, measured before this script was written (see plan
Task 6's own design note): ~20-30 minutes total across all 12 NeweyWest
calls. This is expected, not a hang -- budget for it.

Usage (mts env):
    source /home/rcruz/anaconda3/etc/profile.d/conda.sh && mamba activate mts
    python3 tools/observation_vector/block_length_and_variance_inflation.py
"""
from pathlib import Path

import numpy as np
import pandas as pd
from arch.covariance.kernel import NeweyWest
from statsmodels.tsa.stattools import acf

DRIFT_LOCATION_DIR = Path("/tmp/drift_location_signals")
JUMP_RATIO_DIR = Path("/tmp/jump_ratio_signals")
HORIZONS = [30, 60, 120, 240]

# Real measured average tick density (verified against the production
# file directly -- see this module's own docstring for the derivation).
TICKS_PER_MINUTE = 22.8766
BANDWIDTH_SAFETY_MULTIPLE = 5


def bandwidth_for_horizon(horizon_minutes: int) -> int:
    """Bandwidth (in signal-index lags) for NeweyWest, sized to
    comfortably exceed the tick-count length of this horizon's overlap
    window -- see this task's design note on why the default automatic
    bandwidth is unsafe for this specific bounded-MA(L) dependence
    structure."""
    return int(np.ceil(BANDWIDTH_SAFETY_MULTIPLE * TICKS_PER_MINUTE * horizon_minutes))


def geyer_tau_int(x: np.ndarray, max_lag: int) -> tuple[float, float]:
    """Geyer (1992) initial monotone sequence estimator of the integrated
    autocorrelation time tau_int (standard normalization:
    tau_int = 1 + 2*sum_{k=1}^{K} rho(k)), and the implied
    N_eff = n/tau_int. Self-truncating: sums adjacent autocorrelation
    pairs Gamma_m = rho[2m] + rho[2m+1] until the running sum stops being
    positive and monotone-decreasing, rather than requiring a hand-chosen
    cutoff. Verified against two known cases before being used here (an
    earlier draft of this function used an incorrectly doubled
    normalization, N_eff = n/(2*tau_int) -- caught by the first of these
    two checks, an i.i.d. sanity test, before this script was dispatched):
      - i.i.d. data (no dependence): theoretical tau_int=1.0 exactly;
        measured 1.0114 at n=300,000.
      - AR(1), phi=0.7: theoretical tau_int=(1+phi)/(1-phi)=5.6667;
        measured 5.6438 at n=2,000,000 (~0.4% error)."""
    x = np.asarray(x, dtype=np.float64)
    n = len(x)
    rho = acf(x, nlags=max_lag, fft=True)
    m_max = (max_lag - 1) // 2
    gammas = [rho[2 * m] + rho[2 * m + 1] for m in range(m_max + 1)]
    big_m = 0
    for m in range(1, len(gammas)):
        if gammas[m] > 0 and gammas[m] <= gammas[m - 1]:
            big_m = m
        else:
            break
    tau_int = -1.0 + 2.0 * sum(gammas[: big_m + 1])
    n_eff = n / tau_int if tau_int > 0 else float(n)
    return tau_int, n_eff


def report_deff(label: str, series: np.ndarray, horizon_minutes: int) -> float:
    """Prints both DEFF estimates (Newey-West with an explicit,
    horizon-sized bandwidth; Geyer's self-tuning tau_int) and the final
    chosen value (the max of the two) for one indicator series. Returns
    the final DEFF."""
    series = series.astype(np.float64)
    bandwidth = bandwidth_for_horizon(horizon_minutes)
    cov = NeweyWest(series, bandwidth=bandwidth).cov
    long_run = float(cov.long_run[0, 0])
    short_run = float(cov.short_run[0, 0])
    deff_nw = long_run / short_run
    tau_int, n_eff = geyer_tau_int(series, max_lag=bandwidth)
    # DEFF = N/N_eff = tau_int directly under the standard normalization
    # (tau_int=1 + 2*sum rho(k)), NOT 2*tau_int -- an earlier draft used
    # the doubled form, caught by an i.i.d. sanity check (see
    # geyer_tau_int's own docstring) before this script was dispatched.
    deff_geyer = tau_int
    deff_final = max(deff_nw, deff_geyer)
    print(f"  {label:<38} n={len(series):>9} bandwidth={bandwidth:>7}  "
          f"DEFF_NW={deff_nw:8.4f}  DEFF_Geyer={deff_geyer:8.4f}  "
          f"-> final={deff_final:8.4f}  sqrt(final)={deff_final ** 0.5:7.4f}")
    if abs(deff_nw - deff_geyer) / max(deff_nw, deff_geyer) > 0.5:
        print(f"    NOTE: the two DEFF estimates disagree by >50% -- worth a closer look "
              f"before trusting this horizon's number, per CLAUDE_BRIEF_116/117's own "
              f"sanity-check rule.")
    return deff_final


def main() -> None:
    print("=== drift_location (ComputeHitRate: hit/miss indicator) ===")
    drift_deff: dict[int, float] = {}
    for h in HORIZONS:
        path = DRIFT_LOCATION_DIR / f"drift_location_hitmiss_h{h}.csv"
        series = pd.read_csv(path)["hit"].to_numpy()
        drift_deff[h] = report_deff(f"h={h}min", series, h)

    print("\n=== jump_ratio (ComputeBootstrapMedianGapCI: below-median indicator) ===")
    jump_deff: dict[int, float] = {}
    for h in HORIZONS:
        top = pd.read_csv(JUMP_RATIO_DIR / f"jump_ratio_top_belowmedian_h{h}.csv")["below_median"].to_numpy()
        bottom = pd.read_csv(JUMP_RATIO_DIR / f"jump_ratio_bottom_belowmedian_h{h}.csv")["below_median"].to_numpy()
        top_deff = report_deff(f"h={h}min top decile", top, h)
        bottom_deff = report_deff(f"h={h}min bottom decile", bottom, h)
        # Simple average of the two groups' final DEFFs (deliberate
        # simplification, not a rigorous variance-weighted combination --
        # both groups are comparable size and share the same
        # horizon-driven overlap mechanism, per the design spec's own
        # §4.1 note).
        jump_deff[h] = (top_deff + bottom_deff) / 2.0
        print(f"    -> combined (simple average of top/bottom): "
              f"DEFF={jump_deff[h]:.4f}  sqrt(DEFF)={jump_deff[h] ** 0.5:.4f}")

    print("\n=== C++ constants to embed (Task 7) ===")
    print("drift_location_eval.cpp's VarianceInflationFor():")
    for h in HORIZONS:
        print(f"        {{{h}, {drift_deff[h]:.6f}}},")
    print("jump_ratio_eval.cpp's VarianceInflationFor():")
    for h in HORIZONS:
        print(f"        {{{h}, {jump_deff[h]:.6f}}},")


if __name__ == "__main__":
    main()
