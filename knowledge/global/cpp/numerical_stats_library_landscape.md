---
domain: cpp/numerical_computing
intent: what C++ library should I reach for when a task needs NumPy/SciPy/Pandas-style numerical, statistical, or optimization functionality that isn't in the standard library?
scope: global
tags: [eigen, boost-math, statslib, nlopt, ceres, arrow, duckdb, fftw, nanobind, optimization, statistics]
source_files: []
last_verified: 2026-08-30
dependencies: [candidate_validation_methodology]
---

# C++ library landscape for numerical/statistical work (Python stack replacement)

## Why This Exists

This codebase's tooling (`tools/*.cpp`) has been moving deliberately toward standalone native C++ for
offline analysis work that used to be Python (e.g. `drift_location_eval.cpp`/`jump_ratio_eval.cpp`
replacing the old `mean_rev_z_variant_comparison.py` Python-round-trip pattern), for real, measured
performance reasons (5+ min → 7.5s Parquet reads, 100+s → 2min forward-return computation). As that
trend continues, more numerical/statistical needs will come up that the C++ standard library doesn't
cover. This chunk is a map of what exists, sourced from a teammate's (Gemini) research pass, assessed
against this codebase's actual current and near-future needs rather than accepted at face value.

## The Invariant / Contract

**Match the library to a real, already-identified need — don't adopt one speculatively.** As of this
writing, none of the libraries below are actually required by any in-flight work; each entry states
the concrete future scenario where it would apply, given by name (a specific §-numbered candidate or
decision in the brainstorm doc), not a generic "might be useful someday."

## How It Works

| Python / SciPy component | C++ replacement | This codebase's current fit |
|---|---|---|
| `numpy.ndarray` & matrix ops | **Eigen 3** (header-only, SIMD, industry standard) or **xtensor** (NumPy-syntax) | Not currently used or needed — no matrix/linear-algebra operation exists in this codebase's tooling yet. Would be the natural foundation if any of the rows below get adopted (Boost.Math/StatsLib/NLopt/Ceres all compose with it). |
| `scipy.stats` distributions, CDFs, quantile functions | **Boost.Math** (Normal/Student-t/Gamma/Beta, digamma, log-gamma, Brent's method root finder) | Not currently needed — every candidate validated so far (`tools/observation_vector/drift_location_eval.cpp`, `tools/observation_vector/jump_ratio_eval.cpp`) uses either a Normal-approximation z-test (`std::erf` suffices) or a bootstrap percentile CI (no closed-form distribution needed). Relevant if a **future** candidate's test statistic follows a non-Normal distribution (e.g. Student-t) requiring a quantile function. |
| `scipy.optimize` (L-BFGS, Nelder-Mead, non-linear least squares) | **NLopt** (lightweight, direct callbacks) or **Ceres Solver** (non-linear least squares, automatic differentiation) | **Concretely relevant**: brainstorm doc §5.2 (self-exciting jump clustering / Hawkes intensity, §9 row 23, `CANDIDATE-DEFERRED`) is deferred specifically because it "requires MLE fitting of excitation/decay parameters, not a closed-form statistic." If that candidate is ever implemented natively rather than via a Python MLE round-trip, this is the tool choice — NLopt for a simpler direct-callback fit, Ceres if the problem is naturally a nonlinear-least-squares formulation. |
| `pandas`/`polars` DataFrames | **Apache Arrow C++** (columnar, zero-copy with PyArrow) or **DuckDB C++ API** (embedded SQL on Arrow buffers) | Arrow C++ is **already adopted** (`tools/observation_vector/market_data_io.h`'s `ReadTicksParquet`, `tools/context_pipeline/context_to_parquet.cpp`) — this row confirms the existing choice matches the wider ecosystem's own standard, not a new option. DuckDB is unused and not currently needed (existing tools' filtering/grouping needs are simple enough for hand-written loops); would be worth revisiting only if a future tool needs genuine multi-table joins or complex aggregation that hand-written C++ starts to strain against. |
| `scipy.fft` / spectral density estimation | **PocketFFT** (the exact engine inside NumPy) or **FFTW** | Not currently used. This is the actual computational core underneath Politis & White (2004) block-length selection and Newey-West long-run-variance estimation (`arch.bootstrap.optimal_block_length`, `arch.covariance.kernel.NeweyWest`) — the calibration this codebase currently does entirely in Python (`tools/observation_vector/block_length_and_variance_inflation.py`, 2026-08-30 decision: reuse the validated Python library rather than reimplement Politis-White in C++, since it's a one-time offline calibration step, not a hot path). If that decision is ever revisited toward a native port, FFTW/PocketFFT is what would make the spectral-density piece tractable — not needed now. |

**Migration strategy considered and NOT adopted**: `nanobind` (lightweight `pybind11` successor,
zero-copy NumPy/PyTorch tensor sharing into C++) represents embedding Python bindings *into* C++ and
sharing memory across the boundary — a fundamentally different strategy from this codebase's actual,
validated pattern of **standalone native C++ tools that read Parquet directly and never round-trip
through Python at all** (the whole point of `tools/observation_vector/drift_location_eval.cpp`/`tools/observation_vector/jump_ratio_eval.cpp`
existing, and the source of two real, measured performance wins this session: unprojected vs.
projected Arrow reads, `std::lower_bound` vs. two-pointer forward-return matching). Recorded here as
considered-and-rejected-by-existing-precedent, not silently dropped from consideration.

## Failure Modes

- **Adopting a library because it exists, not because a real need does.** Every row above states its
  concrete trigger condition; treat "we could use X" as insufficient justification on its own.
- **Reaching for `nanobind`/Python-embedding as a default "make it faster" move.** This codebase's own
  measured evidence favors full native ports over embedding — don't reintroduce a Python round-trip
  the standalone-tool pattern was built specifically to eliminate.
- **Pulling in Eigen/Armadillo/StatsLib as a bundle "just in case."** These compose with each other,
  so once one is genuinely needed (e.g. Boost.Math for a future candidate), evaluate whether that
  need alone justifies the dependency weight before adding the others speculatively.

## References

- `lbrnet/logs/rc_gemini.log` (~line 3595, 2026-08-30) — the source research pass this chunk assesses.
- [candidate_validation_methodology.md](candidate_validation_methodology.md) — the actual current
  offline-validation pattern this chunk's "current fit" column is evaluated against.
- `docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md` §5.2, §9 row 23
  (Hawkes intensity, the concrete NLopt/Ceres trigger condition).
- `docs/superpowers/plans/2026-08-30-bootstrap-dependence-correction.md` (the 2026-08-30 decision to
  keep Newey-West/block-length calibration in Python, the concrete FFTW/PocketFFT trigger condition).
