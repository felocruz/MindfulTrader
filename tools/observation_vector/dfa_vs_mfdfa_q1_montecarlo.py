#!/usr/bin/env python3
"""Monte Carlo comparison of this codebase's production DFA (q=2, RMS-based
fluctuation function) against a q=1 MFDFA variant (mean absolute deviation,
not mean SQUARED deviation), at production's exact window (length=100,
minScale=8), to give a real, data-driven answer to the ledger's open
ambiguous decision (2026-08-31 elite-feature-set-curation-initiative.md row 7):
"DFA's RMS/q=2 fluctuation function assumes finite second moment; a q=1
MFDFA variant would be more fat-tail-consistent, but standard DFA isn't
wrong -- a real tradeoff to weigh, not a mechanical fix."

Extends tools/observation_vector/dfa_bias_montecarlo.py's existing
methodology (Kristoufek 2010, exact Davies-Harte fGn simulation) rather than
replacing it -- that script's pure-Gaussian-fGn baseline is reproduced here
for both estimators, THEN a second, outlier-CONTAMINATED fGn scenario is
added (a small fraction of increments replaced by heavy-tailed Student-t(3)
spikes) to directly test the literature's claim that q=1 is more
fat-tail-robust than q=2 -- empirically, on this codebase's own exact
algorithm, not by assuming the general MFDFA literature claim transfers
here unmeasured.

Requires: pip install fbm numpy scipy (same env as dfa_bias_montecarlo.py).
"""

import numpy as np
from fbm import FBM


# ----------------------------------------------------------------------------
# fGn generator (identical to dfa_bias_montecarlo.py)
# ----------------------------------------------------------------------------


def simulate_fgn(n: int, hurst: float, rng: np.random.Generator) -> np.ndarray:
    seed = int(rng.integers(0, 2**31 - 1))
    np.random.seed(seed)
    f = FBM(n=n, hurst=hurst, length=1, method="daviesharte")
    return f.fgn()


def contaminate_with_fat_tail_spikes(
    fgn: np.ndarray,
    rng: np.random.Generator,
    contamination_frac: float = 0.05,
    spike_scale_mult: float = 8.0,
) -> np.ndarray:
    """Replace a small fraction of increments with heavy-tailed (Student-t,
    df=3) spikes scaled to spike_scale_mult times the series' own std -- a
    crude but standard way to simulate real financial fat-tail contamination
    (isolated jump ticks/news spikes) on top of an otherwise well-behaved
    long-memory process."""
    contaminated = fgn.copy()
    n = len(fgn)
    n_spikes = max(1, int(round(n * contamination_frac)))
    spike_idx = rng.choice(n, size=n_spikes, replace=False)
    base_std = np.std(fgn)
    spikes = rng.standard_t(df=3, size=n_spikes) * base_std * spike_scale_mult
    contaminated[spike_idx] = spikes
    return contaminated


# ----------------------------------------------------------------------------
# Generalized MFDFA fluctuation function -- q=2 reduces to production's exact
# DFA (verified bit-for-bit against dfa_bias_montecarlo.py's dfa() below);
# q=1 replaces "RMS of per-segment RMS" with "mean of per-segment RMS"
# (Kantelhardt et al. 2002's F_q(s) generalization).
# ----------------------------------------------------------------------------


def mfdfa(log_returns: np.ndarray, q: float, min_scale: int = 8) -> float:
    length = len(log_returns)
    mean_return = np.mean(log_returns)
    profile = np.cumsum(log_returns - mean_return)

    max_scale = length // 4
    if max_scale <= min_scale:
        return 0.5

    step = 2 if (max_scale - min_scale) > 50 else 1

    log_scales = []
    log_flucts = []

    for s in range(min_scale, max_scale + 1, step):
        num_segments = length // s
        if num_segments < 1:
            continue

        n = float(s)
        sum_x = n * (n - 1.0) * 0.5
        sum_x2 = n * (n - 1.0) * (2.0 * n - 1.0) / 6.0
        denom = n * sum_x2 - sum_x * sum_x
        if abs(denom) < 1e-12:
            continue

        seg_f2 = []  # per-segment F^2(s,v) = mean squared residual
        for v in range(num_segments):
            start = v * s
            chunk = profile[start : start + s]
            k = np.arange(s, dtype=np.float64)

            sum_y = np.sum(chunk)
            sum_xy = np.sum(k * chunk)

            slope = (n * sum_xy - sum_x * sum_y) / denom
            intercept = (sum_y - slope * sum_x) / n

            trend = slope * k + intercept
            diff = chunk - trend
            ssr = np.sum(diff * diff)
            seg_f2.append(ssr / n)

        if not seg_f2:
            continue
        seg_f2 = np.array(seg_f2)

        # F_q(s): q=2 is production's exact sqrt(mean(F^2)) (RMS-of-RMS);
        # q=1 is mean(sqrt(F^2)) (mean-of-RMS, one fewer squaring operation
        # applied to the already-robust per-segment RMS values -- outlier
        # segments contribute linearly, not quadratically, to F_q).
        if q == 2:
            f_q = np.sqrt(np.mean(seg_f2))
        elif q == 1:
            f_q = np.mean(np.sqrt(seg_f2))
        else:
            # General q, for completeness (Kantelhardt et al. 2002 eq. 4) --
            # not exercised by this script's two scenarios but kept correct.
            f_q = (np.mean(seg_f2 ** (q / 2.0))) ** (1.0 / q)

        if f_q > 1e-12:
            log_scales.append(np.log(s))
            log_flucts.append(np.log(f_q))

    if len(log_scales) < 2:
        return 0.5

    x = np.array(log_scales)
    y = np.array(log_flucts)
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


def run_scenario(
    label: str,
    true_hursts,
    n_trials: int,
    n_samples: int,
    min_scale: int,
    contaminate: bool,
    rng: np.random.Generator,
):
    print(f"\n=== {label} ===")
    print(
        f"{'true H':>8} {'q2 mean':>9} {'q2 bias':>8} {'q2 std':>8} "
        f"{'q1 mean':>9} {'q1 bias':>8} {'q1 std':>8}"
    )
    for true_hurst in true_hursts:
        q2_estimates = []
        q1_estimates = []
        for _ in range(n_trials):
            fgn = simulate_fgn(n_samples, true_hurst, rng)
            if contaminate:
                fgn = contaminate_with_fat_tail_spikes(fgn, rng)
            q2_estimates.append(mfdfa(fgn, q=2, min_scale=min_scale))
            q1_estimates.append(mfdfa(fgn, q=1, min_scale=min_scale))
        q2_estimates = np.array(q2_estimates)
        q1_estimates = np.array(q1_estimates)
        print(
            f"{true_hurst:8.2f} "
            f"{q2_estimates.mean():9.4f} {q2_estimates.mean() - true_hurst:+8.4f} {q2_estimates.std():8.4f}  "
            f"{q1_estimates.mean():9.4f} {q1_estimates.mean() - true_hurst:+8.4f} {q1_estimates.std():8.4f}"
        )


def main():
    rng = np.random.default_rng(2026)
    n_trials = 200
    n_samples = 100
    min_scale = 8
    true_hursts = [0.3, 0.4, 0.5, 0.6, 0.7]

    print(
        f"DFA(q=2) vs MFDFA(q=1) Monte Carlo -- N={n_samples}, minScale={min_scale}, "
        f"trials={n_trials} per true-H value"
    )

    run_scenario(
        "Scenario A: pure Gaussian fGn (Kristoufek 2010 baseline, matches "
        "dfa_bias_montecarlo.py exactly)",
        true_hursts,
        n_trials,
        n_samples,
        min_scale,
        contaminate=False,
        rng=rng,
    )

    run_scenario(
        "Scenario B: fGn + 5% Student-t(3) outlier contamination (real fat-tail-jump simulation)",
        true_hursts,
        n_trials,
        n_samples,
        min_scale,
        contaminate=True,
        rng=rng,
    )


if __name__ == "__main__":
    main()
