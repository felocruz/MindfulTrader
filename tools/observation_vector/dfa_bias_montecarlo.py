#!/usr/bin/env python3
"""Monte Carlo DFA bias/variance characterization at N~100, following
Kristoufek (2010)'s methodology: simulate fractional Gaussian noise (fGn) at
known Hurst values via the exact Davies-Harte spectral method (`fbm` package),
run THIS codebase's DFA estimator (mirrored parameter-for-parameter from
src/StudyHelperFunctions.cpp::CalculateHurstExponent, length=100, minScale=8)
against each, and see whether a stable bias-correction curve emerges.

Output-only spike -- does NOT modify production code. See Task 12 / Unit 6a,
docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md.

Why fGn (not the fBm path) is what gets simulated:
CalculateHurstExponent's input is a length-100 array of *log returns*
(increments), which it then internally cumsum-demeans into a "profile" (the
fBm-like path) before running the scaling analysis. So the correct object to
draw from the simulator is the increment process -- fGn with target Hurst H
-- not the already-integrated fBm path. (An earlier draft of this script,
per the Task 12 brief, fed FBM(...).fbm() -- the path -- into a dfa() that
then cumsum'd it *again*. That double-integrates the series and estimates
something other than the target Hurst exponent; it is deliberately not what
this script does.)

Requires: pip install fbm numpy   (installed into the `mts` mamba/conda env
for this spike; not otherwise a build or runtime dependency of this repo.)
"""
import numpy as np
from fbm import FBM

# ----------------------------------------------------------------------------
# fBm/fGn generator
# ----------------------------------------------------------------------------


def simulate_fgn(n: int, hurst: float, rng: np.random.Generator) -> np.ndarray:
    """n increments of fractional Gaussian noise with the given Hurst exponent,
    via the exact Davies-Harte (spectral) method. This is the "logReturns"-
    equivalent input CalculateHurstExponent actually consumes.

    Note: the installed `fbm` package (0.3.0) draws its Gaussian innovations
    from the legacy global `np.random` state, not a passed-in Generator, so
    reproducibility here is driven by seeding that global state from our own
    `rng` before each draw (rather than passing `rng` through directly)."""
    seed = int(rng.integers(0, 2**31 - 1))
    np.random.seed(seed)
    f = FBM(n=n, hurst=hurst, length=1, method="daviesharte")
    return f.fgn()


# ----------------------------------------------------------------------------
# DFA estimator -- mirrors src/StudyHelperFunctions.cpp::CalculateHurstExponent
# parameter-for-parameter (length=100, minScale=8, maxScale=length//4=25,
# step=1 since maxScale-minScale=17<=50, dense integer scale sweep -- NOT a
# log-spaced subsample of scales, all-forward non-overlapping segments only,
# closed-form per-segment OLS detrend, closed-form final log-log regression,
# clamp to [0, 1.5]). No production code is imported or modified; this is a
# faithful Python re-derivation of the same algorithm for simulation purposes.
# ----------------------------------------------------------------------------


def dfa(log_returns: np.ndarray, min_scale: int = 8) -> float:
    length = len(log_returns)
    mean_return = np.mean(log_returns)
    profile = np.cumsum(log_returns - mean_return)

    max_scale = length // 4
    if max_scale <= min_scale:
        return 0.5  # fallback_hurst() equivalent

    step = 2 if (max_scale - min_scale) > 50 else 1

    log_scales = []
    log_fluctuations = []

    for s in range(min_scale, max_scale + 1, step):
        num_segments = length // s
        if num_segments < 1:
            continue

        total_variance = 0.0
        used_segments = 0
        n = float(s)
        sum_x = n * (n - 1.0) * 0.5
        sum_x2 = n * (n - 1.0) * (2.0 * n - 1.0) / 6.0
        denom = n * sum_x2 - sum_x * sum_x
        if abs(denom) < 1e-12:
            continue

        for v in range(num_segments):
            start = v * s
            chunk = profile[start:start + s]
            k = np.arange(s, dtype=np.float64)

            sum_y = np.sum(chunk)
            sum_xy = np.sum(k * chunk)

            slope = (n * sum_xy - sum_x * sum_y) / denom
            intercept = (sum_y - slope * sum_x) / n

            trend = slope * k + intercept
            diff = chunk - trend
            ssr = np.sum(diff * diff)

            total_variance += ssr / n
            used_segments += 1

        if used_segments == 0:
            continue

        f_s = np.sqrt(total_variance / used_segments)
        if f_s > 1e-12:
            log_scales.append(np.log(s))
            log_fluctuations.append(np.log(f_s))

    if len(log_scales) < 2:
        return 0.5  # fallback_hurst() equivalent

    x = np.array(log_scales)
    y = np.array(log_fluctuations)
    n = float(len(x))
    sum_x = np.sum(x)
    sum_y = np.sum(y)
    sum_xy = np.sum(x * y)
    sum_x2 = np.sum(x * x)

    regression_denom = n * sum_x2 - sum_x * sum_x
    if abs(regression_denom) < 1e-12:
        return 0.5

    hurst = (n * sum_xy - sum_x * sum_y) / regression_denom
    return float(np.clip(hurst, 0.0, 1.5))


# ----------------------------------------------------------------------------
# Monte Carlo driver
# ----------------------------------------------------------------------------


def main():
    rng = np.random.default_rng(2026)
    n_trials = 200
    n_samples = 100
    min_scale = 8

    print(f"DFA bias/variance Monte Carlo -- N={n_samples}, minScale={min_scale}, "
          f"trials={n_trials} per true-H value")
    print(f"{'true H':>8} {'mean est':>10} {'bias':>8} {'std':>8} {'95% CI':>20}")

    results = []
    for true_hurst in [0.3, 0.4, 0.5, 0.6, 0.7]:
        estimates = np.array([
            dfa(simulate_fgn(n_samples, true_hurst, rng), min_scale=min_scale)
            for _ in range(n_trials)
        ])
        mean_est = np.mean(estimates)
        bias = mean_est - true_hurst
        std = np.std(estimates)
        ci_lo, ci_hi = np.percentile(estimates, [2.5, 97.5])
        results.append((true_hurst, mean_est, bias, std, ci_lo, ci_hi))
        print(f"{true_hurst:8.2f} {mean_est:10.4f} {bias:+8.4f} {std:8.4f} "
              f"[{ci_lo:.3f}, {ci_hi:.3f}]")

    return results


if __name__ == "__main__":
    main()
