# Jump/Bipower-Variation Ratio Offline Prototype Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone, native C++/Arrow tool (`tools/jump_ratio_eval.cpp`) that computes the §5.1 jump/bipower-variation ratio candidate (Barndorff-Nielsen & Shephard 2004/2006) directly from real historical MES tick data and tests it against real forward market volatility — model-independent, no Python sibling, no schema change — while extracting the now-twice-needed generic infrastructure (Arrow Parquet reading, forward-return computation, Wilson CI/hit-rate test) out of the drift-location-specific files into shared headers, so a third candidate (§5.4, next in queue) doesn't have to duplicate it again either.

**Architecture:** Two new shared headers (`tools/market_data_io.h`, `tools/market_test_stats.h`) extracted verbatim from `tools/drift_location_eval.cpp`/`tools/drift_location_stats.h` — this is the second real use of that code, the right moment for extraction per the "wait for the second use" heuristic, not a bigger refactor than that. `tools/jump_ratio_stats.h` holds the candidate-specific math (realized variance, bipower variation, jump ratio) as an O(n) sliding-window computation, matching `ComputeDriftZScore`'s own precedent. `tools/jump_ratio_eval.cpp` is the CLI, built on the shared headers. The key design decision this plan makes explicit: **jump_ratio needs a different statistical test than drift/location did.** `RV`/`BV`/`jump_ratio` are all non-negative by construction (`jump_ratio = max(0, (RV-BV)/RV)`) — there is no sign to test a forward-return's sign against, so the same-sign hit-rate test doesn't apply. The correct test, confirmed by reading `tools/dim_acceptance_eval.py`'s `predictive_power_magnitude()` directly (lines 234-260) rather than assumed, is a **magnitude test**: does the top decile of jump_ratio values predict larger `|forward return|` than the bottom decile, via a bootstrap CI on the gap between the two groups' mean `|forward return|`.

**Tech Stack:** C++17, Apache Arrow/Parquet 22.0.0 C++ (via `mamba run -n mts`). Native tests via bare `g++` + hand-rolled `check(name, bool)`.

**Spec:** `docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md` §5.1 (candidate definition and literature grounding). Validates via the same model-independent methodology established in §10.7 for §5.0 — never cross-state-ratio against `hmm_model.pkl` (its training-data invalidities are still unresolved, per §5.0's own correction).

## Post-Implementation Note (all 4 tasks complete, this section added after the fact)

All 4 tasks shipped, reviewed (task-scoped + two whole-branch review rounds), and verified end to end against the real 38,547,467-row production file. One deliberate, real deviation from this plan's own Global Constraint below, found by direct user challenge after the first whole-branch review: **the magnitude test's central-tendency statistic is `median(|forward_return|)`, not `mean(|forward_return|)`** as Global Constraint line 16 (below) originally specified. `mean(|x|)` was ported verbatim from `dim_acceptance_eval.py`'s precedent, matching this plan's own stated methodology at the time — but this codebase has its own, later, harder-won standard for fat-tailed data: `FeatureScaler.h`'s `RobustLocation()` computes "median and MAD x 1.4826 (Taleb-consistent)" for every observation-vector dimension, precisely because Kim & White (2004) show moment-based statistics are least reliable exactly under the fat-tailed conditions they exist to detect (this codebase already made this same correction once before, replacing moment-based skewness/kurtosis with Bowley/Moors robust estimators, 2026-08-13). Using `mean(|x|)` here repeated that mistake in new code; caught and fixed post-hoc rather than baked into the original plan. See `tools/market_test_stats.h`'s `ComputeBootstrapMedianGapCI` (new, native-only infrastructure -- `dim_acceptance_eval.py` has no median counterpart to port) for the actual shipped implementation, and its own comment block for the full rationale, verification steps, and a documented known limitation (i.i.d. bootstrap resampling on heavily-overlapping forward-return signals -- shared by `drift_location_eval.cpp`'s test too, not unique to this candidate, and not yet fixed anywhere).

The Global Constraints below are the plan AS WRITTEN and describe what was originally intended; where they conflict with the above, the shipped code and this note are authoritative. In particular: line 16's `mean(|top|) - mean(|bottom|)` formula is superseded by the median-based one; line 24's Bonferroni correction was never implemented for this magnitude test (documented in `jump_ratio_eval.cpp`'s own comment as a deliberate deviation, matching `dim_acceptance_eval.py`'s own `predictive_power_magnitude()`, which also doesn't apply one).

## Global Constraints

- **Jump ratio formula**: `RV = sum(r_k^2)` over the window; `BV = (pi/2) * sum(|r_{k-1}| * |r_k|)` over the same window (Barndorff-Nielsen & Shephard's jump-robust continuous-variance estimator); `jump_ratio = max(0, (RV - BV) / RV)` when `RV > 0`, else `NaN`. The `max(0, ...)` floor is standard institutional practice (Andersen-Bollerslev-Diebold 2007) — finite-sample noise can make `BV > RV`, which would otherwise produce a nonsensical negative jump share.
- **Magnitude test, not directional hit-rate** — ported verbatim from `tools/dim_acceptance_eval.py`'s `predictive_power_magnitude()`/`bootstrap_mean_gap_ci()` (lines 179-186, 234-260): top/bottom decile split via percentile thresholds, bootstrap resampling (with replacement) of `|forward_return|` within each group, gap = `mean(|top|) - mean(|bottom|)`, 95% CI from the 2.5th/97.5th percentile of 2000 bootstrap gap draws.
- **Bootstrap CI is NOT bit-matched to Python.** Unlike Wilson CI/hit-rate (closed-form, verified bit-identical to Python's output), a bootstrap CI is an inherently stochastic estimate — matching numpy's PCG64 bitstream in C++ isn't practical or necessary. Native tests instead verify statistical *properties*: a zero-variance input (both groups constant-valued) collapses the bootstrap distribution to a single point, giving an exact, RNG-independent expected result (verified against a real Python run: `gap=lo=hi=9.0` for `top=[10.0]*50, bottom=[1.0]*50`).
- **Reference values verified against real Python execution, not hand arithmetic** — a hand-computed check for the RV/BV test case was caught wrong by an actual `mamba run -n mts python3` run before it went into a test assertion (real numbers: `RV=0.25029999999999997`, `BV=0.015865042900628457`, `jump_ratio=0.9366158893302898` for returns `[0.01, 0.01, 0.5, 0.01]`).
- **O(n) sliding-window computation**, not `O(n*window)` — matches `ComputeDriftZScore`'s precedent, matters at 38.5M-row scale.
- **Column-projected Arrow Parquet reads** (indices resolved via `Schema::GetFieldIndex`, never hardcoded) — an earlier unprojected `ReadTable()` on this exact file took 5+ minutes reading all 12 columns vs. 7.5s reading 2.
- **Forward-return computation is a single-pass two-pointer merge**, not per-signal `std::lower_bound` — the latter was measured hanging 100+ seconds across 4 horizons on this 38.5M-row file (cache-thrashing random access into a ~308MB sorted array); both series are confirmed monotonically sorted (real `pl.Series.is_sorted()` check).
- **RPATH required on every Arrow/Parquet-linked build**: `-Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib`.
- **Never touches `build_dll.sh`/`CMakeLists.txt`** — standalone tool under `tools/`.
- **Default horizons: 30, 60, 120, 240 minutes**, Bonferroni-corrected (`alpha = 0.05 / n_horizons`) — matches the drift/location precedent.
- **Real end-to-end verification against the actual `lbrnet/data/raw/mes_continuous_ticks.parquet`** is required before any task is done — read the actual numbers, don't just check for a clean exit code.

---

## Task 1: Extract `tools/market_data_io.h` and `tools/market_test_stats.h`

**Files:**
- Create: `tools/market_data_io.h` (from `tools/drift_location_eval.cpp`'s `TickSeries`/`ReadTicksParquet`/`FlattenColumn`)
- Create: `tools/market_test_stats.h` (from `tools/drift_location_stats.h`'s `WilsonInterval`/`ComputeWilsonCI`/`Sign`/`HitRateResult`/`ComputeHitRate`/`ComputeForwardReturns`)
- Modify: `tools/drift_location_eval.cpp` (use `market_data_io.h` instead of its own local copy)
- Modify: `tools/drift_location_stats.h` (use `market_test_stats.h` instead of its own local copy; keeps only `ComputeLogReturns`/`ComputeDriftZScore`)

**Interfaces:**
- Produces (unchanged signatures, just relocated): `struct TickSeries`, `TickSeries ReadTicksParquet(const std::string& path)`, `struct WilsonInterval`, `WilsonInterval ComputeWilsonCI(std::size_t k, std::size_t n, double z = 1.96)`, `int Sign(double x)`, `struct HitRateResult`, `HitRateResult ComputeHitRate(const std::vector<double>&, const std::vector<double>&)`, `std::vector<double> ComputeForwardReturns(...)`.

- [x] **Step 1: Create `tools/market_data_io.h`**

```cpp
// tools/market_data_io.h
// Shared Arrow Parquet reading utility for MES tick data, extracted from
// tools/drift_location_eval.cpp on its second real use (tools/jump_ratio_eval.cpp)
// -- the right moment for this extraction, not premature. This codebase's
// first direct Parquet read remains context-preserved in git history; every
// prior tool (context_to_parquet.cpp) only writes.
#pragma once

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

struct TickSeries {
    std::vector<std::int64_t> timestamp_us;
    std::vector<double> close;
};

namespace market_data_io_detail {

// Flattens a Table column's chunks into a contiguous std::vector -- a Table
// column is an arrow::ChunkedArray (may span multiple row-group chunks), not
// a flat array, so this can't just be a single raw_values() call.
template <typename ArrowArrayType, typename ValueType>
std::vector<ValueType> FlattenColumn(const std::shared_ptr<arrow::ChunkedArray>& col) {
    std::vector<ValueType> out;
    out.reserve(static_cast<std::size_t>(col->length()));
    for (const auto& chunk : col->chunks()) {
        auto typed = std::static_pointer_cast<ArrowArrayType>(chunk);
        const ValueType* raw = typed->raw_values();
        out.insert(out.end(), raw, raw + typed->length());
    }
    return out;
}

}  // namespace market_data_io_detail

inline TickSeries ReadTicksParquet(const std::string& path) {
    auto infile_result = arrow::io::ReadableFile::Open(path);
    if (!infile_result.ok()) {
        throw std::runtime_error("ReadTicksParquet: cannot open " + path + ": " +
                                  infile_result.status().ToString());
    }

    parquet::arrow::FileReaderBuilder builder;
    auto open_status = builder.Open(*infile_result);
    if (!open_status.ok()) {
        throw std::runtime_error("ReadTicksParquet: FileReaderBuilder::Open failed: " +
                                  open_status.ToString());
    }
    std::unique_ptr<parquet::arrow::FileReader> reader;
    auto build_status = builder.Build(&reader);
    if (!build_status.ok()) {
        throw std::runtime_error("ReadTicksParquet: FileReaderBuilder::Build failed: " +
                                  build_status.ToString());
    }

    // Column projection: read ONLY timestamp_us/close, not all 12 columns
    // (including a string `contract` column and several unneeded doubles) --
    // reading the full table was measured at 5+ minutes with zero output on
    // this 38.5M-row file; projected, it's 7.5s. Indices resolved from the
    // real schema, never hardcoded.
    std::shared_ptr<arrow::Schema> schema;
    auto schema_status = reader->GetSchema(&schema);
    if (!schema_status.ok()) {
        throw std::runtime_error("ReadTicksParquet: GetSchema failed: " + schema_status.ToString());
    }
    const int ts_idx = schema->GetFieldIndex("timestamp_us");
    const int close_idx = schema->GetFieldIndex("close");
    if (ts_idx < 0 || close_idx < 0) {
        throw std::runtime_error("ReadTicksParquet: missing timestamp_us or close column in " + path);
    }

    std::shared_ptr<arrow::Table> table;
    auto read_status = reader->ReadTable({ts_idx, close_idx}, &table);
    if (!read_status.ok()) {
        throw std::runtime_error("ReadTicksParquet: ReadTable failed: " + read_status.ToString());
    }

    auto ts_col = table->GetColumnByName("timestamp_us");
    auto close_col = table->GetColumnByName("close");
    if (!ts_col || !close_col) {
        throw std::runtime_error("ReadTicksParquet: missing timestamp_us or close column in projected table");
    }

    TickSeries series;
    series.timestamp_us = market_data_io_detail::FlattenColumn<arrow::Int64Array, std::int64_t>(ts_col);
    series.close = market_data_io_detail::FlattenColumn<arrow::DoubleArray, double>(close_col);
    return series;
}
```

- [x] **Step 2: Create `tools/market_test_stats.h`**

```cpp
// tools/market_test_stats.h
// Shared model-independent market-outcome testing utilities, extracted from
// tools/drift_location_stats.h on its second real use (tools/jump_ratio_eval.cpp).
// No Arrow dependency -- fully natively testable.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// Mirrors tools/dim_acceptance_eval.py's compute_forward_returns() semantics
// exactly, but as an O(n) two-pointer merge instead of Python's O(n log n)
// np.searchsorted. REQUIRES both signal_ts and timestamps sorted ascending.
inline std::vector<double> ComputeForwardReturns(
    const std::vector<std::int64_t>& signal_ts, const std::vector<double>& signal_price,
    const std::vector<std::int64_t>& timestamps, const std::vector<double>& prices,
    int horizon_minutes) {
    const std::int64_t horizon_us = static_cast<std::int64_t>(horizon_minutes) * 60 * 1'000'000LL;
    std::vector<double> fwd(signal_ts.size(), std::numeric_limits<double>::quiet_NaN());
    const std::size_t m = timestamps.size();
    std::size_t idx = 0;
    for (std::size_t i = 0; i < signal_ts.size(); ++i) {
        const std::int64_t target_ts = signal_ts[i] + horizon_us;
        while (idx < m && timestamps[idx] < target_ts) {
            ++idx;
        }
        if (idx >= m) {
            break;
        }
        if ((timestamps[idx] - signal_ts[i]) > horizon_us * 3) {
            continue;
        }
        fwd[i] = std::log(prices[idx] / signal_price[i]);
    }
    return fwd;
}

struct WilsonInterval {
    double lo;
    double hi;
};

inline WilsonInterval ComputeWilsonCI(std::size_t k, std::size_t n, double z = 1.96) {
    const double nd = static_cast<double>(n);
    const double phat = static_cast<double>(k) / nd;
    const double denom = 1.0 + z * z / nd;
    const double center = (phat + z * z / (2.0 * nd)) / denom;
    const double half = (z / denom) * std::sqrt(phat * (1.0 - phat) / nd + z * z / (4.0 * nd * nd));
    return {center - half, center + half};
}

inline int Sign(double x) {
    if (x > 0.0) return 1;
    if (x < 0.0) return -1;
    return 0;
}

struct HitRateResult {
    std::size_t n = 0;
    std::size_t k = 0;
    double hit_rate = 0.0;
    double ci_lo = 0.0;
    double ci_hi = 0.0;
    double z_stat = 0.0;
    double p_value = 1.0;
};

inline HitRateResult ComputeHitRate(
    const std::vector<double>& forward_returns, const std::vector<double>& candidate_values) {
    HitRateResult result;
    std::size_t n = 0, k = 0;
    for (std::size_t i = 0; i < forward_returns.size(); ++i) {
        if (!std::isfinite(forward_returns[i])) continue;
        if (candidate_values[i] == 0.0 || !std::isfinite(candidate_values[i])) continue;
        ++n;
        if (Sign(forward_returns[i]) == Sign(candidate_values[i])) {
            ++k;
        }
    }
    result.n = n;
    result.k = k;
    if (n == 0) {
        return result;
    }
    result.hit_rate = static_cast<double>(k) / static_cast<double>(n);
    const double se_null = std::sqrt(0.25 / static_cast<double>(n));
    result.z_stat = (result.hit_rate - 0.5) / se_null;
    result.p_value = 2.0 * (1.0 - 0.5 * (1.0 + std::erf(std::fabs(result.z_stat) / std::sqrt(2.0))));
    const auto ci = ComputeWilsonCI(k, n);
    result.ci_lo = ci.lo;
    result.ci_hi = ci.hi;
    return result;
}
```

- [x] **Step 3: Update `tools/drift_location_stats.h` to use the shared header**

Replace the `#pragma once` block's contents: remove `WilsonInterval`/`ComputeWilsonCI`/`Sign`/`HitRateResult`/`ComputeHitRate`/`ComputeForwardReturns` (now in `market_test_stats.h`), keep `ComputeLogReturns`/`ComputeDriftZScore`, and add `#include "market_test_stats.h"` near the top so anything that previously got these symbols from `drift_location_stats.h` still compiles unchanged:

```cpp
// tools/drift_location_stats.h
// Pure numeric functions for the §5.0 drift/location offline prototype.
// Forward-return/Wilson-CI/hit-rate functions moved to market_test_stats.h
// (2026-08-30, extracted on their second real use by jump_ratio_eval.cpp) --
// included here so existing callers of this header see no symbol change.
#pragma once

#include "market_test_stats.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

inline std::vector<double> ComputeLogReturns(const std::vector<double>& prices) {
    std::vector<double> returns;
    if (prices.size() < 2) {
        return returns;
    }
    returns.reserve(prices.size() - 1);
    for (std::size_t i = 1; i < prices.size(); ++i) {
        returns.push_back(std::log(prices[i] / prices[i - 1]));
    }
    return returns;
}

inline std::vector<double> ComputeDriftZScore(
    const std::vector<double>& log_returns, std::size_t window) {
    const std::size_t n = log_returns.size();
    std::vector<double> z(n, std::numeric_limits<double>::quiet_NaN());
    if (window == 0 || n < window) {
        return z;
    }

    double sum = 0.0, sum_sq = 0.0;
    for (std::size_t i = 0; i < window; ++i) {
        sum += log_returns[i];
        sum_sq += log_returns[i] * log_returns[i];
    }
    auto write_z = [&](std::size_t idx) {
        const double mean = sum / static_cast<double>(window);
        const double variance = sum_sq / static_cast<double>(window) - mean * mean;
        const double stddev = std::sqrt(std::max(0.0, variance));
        z[idx] = (stddev > 0.0) ? (mean / stddev) : 0.0;
    };
    write_z(window - 1);
    for (std::size_t i = window; i < n; ++i) {
        sum += log_returns[i] - log_returns[i - window];
        sum_sq += log_returns[i] * log_returns[i] - log_returns[i - window] * log_returns[i - window];
        write_z(i);
    }
    return z;
}
```

- [x] **Step 4: Update `tools/drift_location_eval.cpp` to use the shared header**

Replace the local `TickSeries`/`FlattenColumn`/`ReadTicksParquet` definitions (and their `#include <arrow/...>`/`#include <parquet/...>` lines) with:

```cpp
#include "market_data_io.h"
```

(Keep `#include "drift_location_stats.h"` as-is — it now transitively pulls in `market_test_stats.h`.)

- [x] **Step 5: Verify zero behavior change**

Run: `mamba run -n mts g++ -std=c++17 tools/test_drift_location_stats.cpp -o /tmp/drift_location_stats_test && /tmp/drift_location_stats_test`
Expected: `ALL PASS` (26 checks) — identical to before the extraction. If any check fails, the extraction changed behavior; stop and fix before proceeding.

Run:
```bash
mamba run -n mts g++ -O2 -std=c++17 \
  $(mamba run -n mts pkg-config --cflags arrow parquet) \
  tools/drift_location_eval.cpp \
  $(mamba run -n mts pkg-config --libs arrow parquet) \
  -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
  -o tools/drift_location_eval
```
Expected: compiles cleanly.

- [x] **Step 6: Commit**

```bash
git add tools/market_data_io.h tools/market_test_stats.h tools/drift_location_stats.h tools/drift_location_eval.cpp
git commit -m "refactor: extract market_data_io.h/market_test_stats.h from drift-location files (2nd use)"
```

---

## Task 2: `tools/jump_ratio_stats.h` — realized variance, bipower variation, jump ratio

**Files:**
- Create: `tools/jump_ratio_stats.h`
- Create: `tools/test_jump_ratio_stats.cpp`

**Interfaces:**
- Consumes: `ComputeLogReturns` is NOT reused here directly as a dependency (this header takes a log-returns vector as input, same convention as `ComputeDriftZScore`).
- Produces: `std::vector<double> ComputeJumpRatio(const std::vector<double>& log_returns, std::size_t window)` (returns a vector the same length as `log_returns`; the first `window - 1` entries are `NaN`; `RV == 0` within a window also yields `NaN` for that entry, since the ratio is undefined).

- [x] **Step 1: Write the failing test**

```cpp
// tools/test_jump_ratio_stats.cpp
// Build & run: mamba run -n mts g++ -std=c++17 tools/test_jump_ratio_stats.cpp \
//   -o /tmp/jump_ratio_stats_test && /tmp/jump_ratio_stats_test
#include "jump_ratio_stats.h"
#include <cmath>
#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
bool close(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }
}  // namespace

int main() {
    {
        // Reference values verified via a real mamba run -n mts python3 execution
        // (not hand arithmetic -- an initial hand computation was wrong and caught
        // before being embedded here): returns = [0.01, 0.01, 0.5, 0.01], window=4:
        //   RV = 0.01^2 + 0.01^2 + 0.5^2 + 0.01^2 = 0.25029999999999997
        //   BV = (pi/2) * (|0.01*0.01| + |0.01*0.5| + |0.5*0.01|) = 0.015865042900628457
        //   jump_ratio = max(0, (RV-BV)/RV) = 0.9366158893302898
        std::vector<double> returns = {0.01, 0.01, 0.5, 0.01};
        auto jr = ComputeJumpRatio(returns, 4);
        check("jump ratio has same length as input", jr.size() == 4);
        check("first 3 entries are NaN (warmup, window=4)",
              std::isnan(jr[0]) && std::isnan(jr[1]) && std::isnan(jr[2]));
        check("jr[3] matches Python reference (0.9366158893302898)",
              close(jr[3], 0.9366158893302898, 1e-9));
    }
    {
        // Pure diffusion (no jumps): all returns similar magnitude -> BV should be
        // close to RV -> jump_ratio close to 0 (or floored at 0 if BV slightly > RV
        // due to finite-sample noise, matching the max(0, ...) floor's whole purpose).
        std::vector<double> returns = {0.02, 0.02, 0.02, 0.02};
        auto jr = ComputeJumpRatio(returns, 4);
        check("pure-diffusion-like series has near-zero (or floored-zero) jump ratio",
              jr[3] >= 0.0 && jr[3] < 0.1);
    }
    {
        // RV == 0 (all-zero returns) -> undefined ratio -> NaN, not a divide-by-zero crash.
        std::vector<double> zero_returns = {0.0, 0.0, 0.0, 0.0};
        auto jr = ComputeJumpRatio(zero_returns, 4);
        check("all-zero returns yield NaN (RV=0, undefined ratio), not a crash",
              std::isnan(jr[3]));
    }
    {
        check("window larger than input yields all-NaN",
              std::isnan(ComputeJumpRatio({0.01, 0.02}, 5)[0]));
    }
    {
        // Sliding window sanity: a second window position over the same series
        // shifts by one element -- verify the incremental slide logic against
        // two independently-computed windows. returns = [0.1, 0.2, 0.05],
        // window=2. Reference values from a real mamba run -n mts python3
        // execution (not hand arithmetic -- an initial hand computation here
        // was off in the 7th decimal place, caught before being embedded):
        //   i=1: RV=0.05000000000000001 BV=0.031415926535897934 jump_ratio=0.37168146928204143
        //   i=2: RV=0.04250000000000001 BV=0.015707963267948967 jump_ratio=0.6304008642835538
        std::vector<double> returns = {0.1, 0.2, 0.05};
        auto jr = ComputeJumpRatio(returns, 2);
        check("sliding window i=1 matches Python reference", close(jr[1], 0.37168146928204143, 1e-9));
        check("sliding window i=2 matches Python reference", close(jr[2], 0.6304008642835538, 1e-9));
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [x] **Step 2: Run test to verify it fails**

Run: `mamba run -n mts g++ -std=c++17 tools/test_jump_ratio_stats.cpp -o /tmp/jump_ratio_stats_test`
Expected: FAIL — `jump_ratio_stats.h` does not exist, compile error.

- [x] **Step 3: Write the implementation**

```cpp
// tools/jump_ratio_stats.h
// Realized variance / bipower variation / jump ratio for the §5.1 jump-ratio
// offline prototype (docs/superpowers/specs/2026-08-29-hmm-fat-tail-
// observation-vector-brainstorm.md §5.1, Barndorff-Nielsen & Shephard
// 2004/2006). No Arrow dependency -- fully natively testable.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <vector>

// jump_ratio[i] = max(0, (RV[i] - BV[i]) / RV[i]) over the `window`-wide
// trailing slice of log_returns ending at i, where RV = sum(r^2) and
// BV = (pi/2) * sum(|r_{k-1}| * |r_k|) (Barndorff-Nielsen & Shephard's
// jump-robust continuous-variance estimator). The max(0, ...) floor is
// standard institutional practice (Andersen-Bollerslev-Diebold 2007) --
// finite-sample noise can make BV exceed RV, which would otherwise yield a
// nonsensical negative jump share. O(n) via sliding accumulators for RV's
// sum-of-squares and BV's sum-of-adjacent-products, matching
// drift_location_stats.h's ComputeDriftZScore precedent -- not O(n*window).
inline std::vector<double> ComputeJumpRatio(
    const std::vector<double>& log_returns, std::size_t window) {
    const std::size_t n = log_returns.size();
    std::vector<double> jump_ratio(n, std::numeric_limits<double>::quiet_NaN());
    if (window < 2 || n < window) {
        return jump_ratio;
    }

    constexpr double kHalfPi = 1.5707963267948966;  // pi/2

    auto compute_at = [&](std::size_t window_start) {
        const std::size_t window_end = window_start + window;  // exclusive
        double rv = 0.0;
        for (std::size_t k = window_start; k < window_end; ++k) {
            rv += log_returns[k] * log_returns[k];
        }
        double bv_sum = 0.0;
        for (std::size_t k = window_start + 1; k < window_end; ++k) {
            bv_sum += std::fabs(log_returns[k - 1]) * std::fabs(log_returns[k]);
        }
        const double bv = kHalfPi * bv_sum;
        if (rv <= 0.0) {
            return;  // leaves jump_ratio[window_end - 1] as NaN
        }
        jump_ratio[window_end - 1] = std::max(0.0, (rv - bv) / rv);
    };

    // First window computed directly (O(window)); every subsequent window
    // slides by one element via incremental sum updates (O(1) per step).
    compute_at(0);
    double rv = 0.0, bv_sum = 0.0;
    for (std::size_t k = 0; k < window; ++k) {
        rv += log_returns[k] * log_returns[k];
    }
    for (std::size_t k = 1; k < window; ++k) {
        bv_sum += std::fabs(log_returns[k - 1]) * std::fabs(log_returns[k]);
    }
    for (std::size_t window_start = 1; window_start + window <= n; ++window_start) {
        const std::size_t old_idx = window_start - 1;
        const std::size_t new_idx = window_start + window - 1;
        rv += log_returns[new_idx] * log_returns[new_idx] - log_returns[old_idx] * log_returns[old_idx];
        // BV's adjacent-pair sum: drop the pair that fell out of the window
        // (old_idx, old_idx+1), add the pair that entered (new_idx-1, new_idx).
        bv_sum -= std::fabs(log_returns[old_idx]) * std::fabs(log_returns[old_idx + 1]);
        bv_sum += std::fabs(log_returns[new_idx - 1]) * std::fabs(log_returns[new_idx]);
        const double bv = kHalfPi * bv_sum;
        if (rv > 0.0) {
            jump_ratio[new_idx] = std::max(0.0, (rv - bv) / rv);
        }
    }
    return jump_ratio;
}
```

- [x] **Step 4: Run test to verify it passes**

Run: `mamba run -n mts g++ -std=c++17 tools/test_jump_ratio_stats.cpp -o /tmp/jump_ratio_stats_test && /tmp/jump_ratio_stats_test`
Expected: `ALL PASS` (8 checks)

- [x] **Step 5: Commit**

```bash
git add tools/jump_ratio_stats.h tools/test_jump_ratio_stats.cpp
git commit -m "feat: add realized-variance/bipower-variation/jump-ratio computation for §5.1 prototype"
```

---

## Task 3: Magnitude/bootstrap-gap test in `market_test_stats.h`

**Files:**
- Modify: `tools/market_test_stats.h`
- Create: `tools/test_market_test_stats_magnitude.cpp`

**Interfaces:**
- Produces: `struct BootstrapGapResult { double gap; double ci_lo; double ci_hi; }`; `BootstrapGapResult ComputeBootstrapMeanGapCI(const std::vector<double>& top, const std::vector<double>& bottom, std::size_t n_boot = 2000, std::uint64_t seed = 0)`.

- [x] **Step 1: Write the failing test**

```cpp
// tools/test_market_test_stats_magnitude.cpp
// Build & run: mamba run -n mts g++ -std=c++17 tools/test_market_test_stats_magnitude.cpp \
//   -o /tmp/market_test_magnitude_test && /tmp/market_test_magnitude_test
#include "market_test_stats.h"
#include <cmath>
#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
bool close(double a, double b, double eps = 1e-9) { return std::fabs(a - b) < eps; }
}  // namespace

int main() {
    {
        // Zero-variance groups: every bootstrap resample of a constant-valued
        // array is that same constant, so the bootstrap distribution collapses
        // to a single point -- this is RNG-independent by construction, unlike
        // a general bootstrap CI, and matches a real mamba run -n mts python3
        // execution of the actual Python bootstrap_mean_gap_ci() exactly:
        // top=[10.0]*50, bottom=[1.0]*50 -> gap=lo=hi=9.0.
        std::vector<double> top(50, 10.0);
        std::vector<double> bottom(50, 1.0);
        auto result = ComputeBootstrapMeanGapCI(top, bottom);
        check("zero-variance gap matches Python reference exactly", close(result.gap, 9.0));
        check("zero-variance CI collapses to a point (lo == hi == gap)",
              close(result.ci_lo, 9.0) && close(result.ci_hi, 9.0));
    }
    {
        // Point estimate (not the CI, which is stochastic) must exactly match
        // mean(|top|) - mean(|bottom|) -- this part of the formula has no
        // randomness at all, matches tools/dim_acceptance_eval.py's own
        // bootstrap_mean_gap_ci() line: `float(np.mean(np.abs(top)) - np.mean(np.abs(bottom)))`.
        std::vector<double> top = {3.0, -5.0, 4.0};   // mean |x| = (3+5+4)/3 = 4.0
        std::vector<double> bottom = {1.0, -1.0, 2.0}; // mean |x| = (1+1+2)/3 = 4.0/3
        auto result = ComputeBootstrapMeanGapCI(top, bottom);
        check("point estimate is exactly mean(|top|) - mean(|bottom|)",
              close(result.gap, 4.0 - (4.0 / 3.0), 1e-9));
        check("CI bounds are finite and ordered (lo <= hi)",
              std::isfinite(result.ci_lo) && std::isfinite(result.ci_hi) && result.ci_lo <= result.ci_hi);
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [x] **Step 2: Run test to verify it fails**

Run: `mamba run -n mts g++ -std=c++17 tools/test_market_test_stats_magnitude.cpp -o /tmp/market_test_magnitude_test`
Expected: FAIL — `ComputeBootstrapMeanGapCI`/`BootstrapGapResult` undeclared.

- [x] **Step 3: Write the implementation**

Add to `tools/market_test_stats.h` (needs `<random>` added to the includes):

```cpp
#include <cstdint>
#include <random>
```

```cpp
struct BootstrapGapResult {
    double gap;
    double ci_lo;
    double ci_hi;
};

// Mirrors tools/dim_acceptance_eval.py's bootstrap_mean_gap_ci() methodology
// (lines 179-186): point estimate is the exact gap between the two groups'
// mean |value| (no randomness); the CI comes from n_boot resamples (with
// replacement) of each group independently, taking the 2.5th/97.5th
// percentile of the resampled gap distribution. NOT bit-matched to Python's
// PCG64 bitstream (impractical and unnecessary for a stochastic CI estimate;
// see this plan's Global Constraints) -- verified instead via the
// deterministic zero-variance case, where the bootstrap distribution
// collapses to a single point regardless of which PRNG algorithm is used.
inline BootstrapGapResult ComputeBootstrapMeanGapCI(
    const std::vector<double>& top, const std::vector<double>& bottom,
    std::size_t n_boot = 2000, std::uint64_t seed = 0) {
    auto mean_abs = [](const std::vector<double>& v) {
        double sum = 0.0;
        for (double x : v) sum += std::fabs(x);
        return sum / static_cast<double>(v.size());
    };
    const double point_gap = mean_abs(top) - mean_abs(bottom);

    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<std::size_t> top_dist(0, top.size() - 1);
    std::uniform_int_distribution<std::size_t> bottom_dist(0, bottom.size() - 1);

    std::vector<double> gaps(n_boot);
    std::vector<double> resampled_top(top.size());
    std::vector<double> resampled_bottom(bottom.size());
    for (std::size_t b = 0; b < n_boot; ++b) {
        for (std::size_t i = 0; i < top.size(); ++i) {
            resampled_top[i] = top[top_dist(rng)];
        }
        for (std::size_t i = 0; i < bottom.size(); ++i) {
            resampled_bottom[i] = bottom[bottom_dist(rng)];
        }
        gaps[b] = mean_abs(resampled_top) - mean_abs(resampled_bottom);
    }

    std::sort(gaps.begin(), gaps.end());
    auto percentile = [&](double p) {
        const double idx = p / 100.0 * static_cast<double>(gaps.size() - 1);
        const std::size_t lo_idx = static_cast<std::size_t>(std::floor(idx));
        const std::size_t hi_idx = static_cast<std::size_t>(std::ceil(idx));
        if (lo_idx == hi_idx) return gaps[lo_idx];
        const double frac = idx - static_cast<double>(lo_idx);
        return gaps[lo_idx] * (1.0 - frac) + gaps[hi_idx] * frac;
    };

    BootstrapGapResult result;
    result.gap = point_gap;
    result.ci_lo = percentile(2.5);
    result.ci_hi = percentile(97.5);
    return result;
}
```

- [x] **Step 4: Run test to verify it passes**

Run: `mamba run -n mts g++ -std=c++17 tools/test_market_test_stats_magnitude.cpp -o /tmp/market_test_magnitude_test && /tmp/market_test_magnitude_test`
Expected: `ALL PASS` (4 checks)

- [x] **Step 5: Re-verify Task 1's extraction is still unaffected**

Run: `mamba run -n mts g++ -std=c++17 tools/test_drift_location_stats.cpp -o /tmp/drift_location_stats_test && /tmp/drift_location_stats_test`
Expected: `ALL PASS` (26 checks) — adding `ComputeBootstrapMeanGapCI` to the shared header must not disturb anything drift/location already uses.

- [x] **Step 6: Commit**

```bash
git add tools/market_test_stats.h tools/test_market_test_stats_magnitude.cpp
git commit -m "feat: add bootstrap mean-gap CI (magnitude test) to market_test_stats.h"
```

---

## Task 4: `tools/jump_ratio_eval.cpp` — CLI and end-to-end run

**Files:**
- Create: `tools/jump_ratio_eval.cpp`

**Interfaces:**
- Consumes: `tools/market_data_io.h` (`TickSeries`, `ReadTicksParquet`), `tools/market_test_stats.h` (`ComputeForwardReturns`, `ComputeBootstrapMeanGapCI`, `BootstrapGapResult`), `tools/jump_ratio_stats.h` (`ComputeJumpRatio`). Also needs `ComputeLogReturns` — since it's still declared in `drift_location_stats.h` (Task 1 kept it there, it's not candidate-specific in name only by accident of history), include that header for it.
- Produces: `tools/jump_ratio_eval` executable, CLI `--ticks-parquet PATH [--window N] [--horizons 30,60,120,240] [--top-decile 0.10] [--report-json PATH]`.

- [x] **Step 1: Write the implementation**

```cpp
// tools/jump_ratio_eval.cpp
// Offline, model-independent prototype validator for the §5.1 jump/bipower-
// variation ratio observation-vector candidate (docs/superpowers/specs/
// 2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md). Magnitude test
// (top/bottom decile bootstrap gap in |forward return|), not directional
// hit-rate -- jump_ratio is non-negative by construction, so there's no sign
// to test a forward return's sign against.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   tools/jump_ratio_eval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/jump_ratio_eval
#include "drift_location_stats.h"  // for ComputeLogReturns
#include "jump_ratio_stats.h"
#include "market_data_io.h"
#include "market_test_stats.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --ticks-parquet PATH [--window N] [--horizons 30,60,120,240] "
        "[--top-decile 0.10] [--report-json PATH]\n", argv0);
}

std::vector<int> ParseHorizons(const std::string& csv) {
    std::vector<int> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        out.push_back(std::stoi(item));
    }
    return out;
}

double Percentile(std::vector<double> values, double p) {
    std::sort(values.begin(), values.end());
    const double idx = p / 100.0 * static_cast<double>(values.size() - 1);
    const std::size_t lo_idx = static_cast<std::size_t>(std::floor(idx));
    const std::size_t hi_idx = static_cast<std::size_t>(std::ceil(idx));
    if (lo_idx == hi_idx) return values[lo_idx];
    const double frac = idx - static_cast<double>(lo_idx);
    return values[lo_idx] * (1.0 - frac) + values[hi_idx] * frac;
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticks_path, report_json_path;
    std::size_t window = 100;
    std::string horizons_csv = "30,60,120,240";
    double top_decile = 0.10;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--ticks-parquet") ticks_path = next("--ticks-parquet");
        else if (arg == "--window") window = std::stoull(next("--window"));
        else if (arg == "--horizons") horizons_csv = next("--horizons");
        else if (arg == "--top-decile") top_decile = std::stod(next("--top-decile"));
        else if (arg == "--report-json") report_json_path = next("--report-json");
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); PrintUsage(argv[0]); return 1; }
    }
    if (ticks_path.empty()) {
        PrintUsage(argv[0]);
        return 1;
    }

    TickSeries series;
    try {
        series = ReadTicksParquet(ticks_path);
    } catch (const std::runtime_error& e) {
        std::fprintf(stderr, "❌ %s\n", e.what());
        return 1;
    }
    std::printf("Loaded %zu rows from %s\n", series.timestamp_us.size(), ticks_path.c_str());
    std::fflush(stdout);

    const auto log_returns = ComputeLogReturns(series.close);
    const auto jump_ratio = ComputeJumpRatio(log_returns, window);
    // jump_ratio[] is indexed against log_returns (length n-1), itself offset
    // by 1 from series.close/timestamp_us -- same convention as drift_location_eval.cpp.
    std::vector<std::int64_t> signal_ts;
    std::vector<double> signal_price, signal_jr;
    for (std::size_t i = 0; i < jump_ratio.size(); ++i) {
        if (std::isnan(jump_ratio[i])) continue;
        signal_ts.push_back(series.timestamp_us[i + 1]);
        signal_price.push_back(series.close[i + 1]);
        signal_jr.push_back(jump_ratio[i]);
    }
    std::printf("%zu non-warmup jump-ratio signals (window=%zu)\n", signal_ts.size(), window);
    std::fflush(stdout);

    const double hi_thresh = Percentile(signal_jr, 100.0 * (1.0 - top_decile));
    const double lo_thresh = Percentile(signal_jr, 100.0 * top_decile);

    const auto horizons = ParseHorizons(horizons_csv);
    std::printf("\n=== Predictive power (magnitude): does high jump_ratio predict larger |forward return|? ===\n");
    std::ofstream json_out;
    if (!report_json_path.empty()) {
        json_out.open(report_json_path);
        json_out << "{\n  \"window\": " << window << ",\n  \"top_decile\": " << top_decile
                 << ",\n  \"horizons\": [\n";
    }
    for (std::size_t h_idx = 0; h_idx < horizons.size(); ++h_idx) {
        const int h = horizons[h_idx];
        std::fprintf(stderr, "[progress] computing horizon=%dmin (%zu/%zu)...\n", h, h_idx + 1, horizons.size());
        const auto fwd = ComputeForwardReturns(signal_ts, signal_price, series.timestamp_us, series.close, h);

        std::vector<double> top_fwd, bottom_fwd;
        for (std::size_t i = 0; i < fwd.size(); ++i) {
            if (!std::isfinite(fwd[i])) continue;
            if (signal_jr[i] >= hi_thresh) top_fwd.push_back(fwd[i]);
            else if (signal_jr[i] <= lo_thresh) bottom_fwd.push_back(fwd[i]);
        }
        if (top_fwd.size() < 30 || bottom_fwd.size() < 30) {
            std::printf("  %4dmin: too few samples in top/bottom decile (n_top=%zu, n_bot=%zu)\n",
                        h, top_fwd.size(), bottom_fwd.size());
            continue;
        }
        const auto result = ComputeBootstrapMeanGapCI(top_fwd, bottom_fwd);
        const bool survives = (result.ci_lo > 0.0) || (result.ci_hi < 0.0);
        std::printf("  %4dmin: n_top=%-7zu n_bot=%-7zu gap=%+.6f 95%%CI=[%+.6f,%+.6f] (%s)\n",
                    h, top_fwd.size(), bottom_fwd.size(), result.gap, result.ci_lo, result.ci_hi,
                    survives ? "SURVIVES (CI excludes 0)" : "does not survive (CI includes 0)");
        if (json_out.is_open()) {
            json_out << "    {\"horizon_minutes\": " << h << ", \"n_top\": " << top_fwd.size()
                      << ", \"n_bot\": " << bottom_fwd.size() << ", \"gap\": " << result.gap
                      << ", \"ci_lo\": " << result.ci_lo << ", \"ci_hi\": " << result.ci_hi
                      << ", \"survives\": " << (survives ? "true" : "false") << "}"
                      << (h_idx + 1 < horizons.size() ? ",\n" : "\n");
        }
    }
    if (json_out.is_open()) {
        json_out << "  ]\n}\n";
        std::printf("\nWrote JSON report: %s\n", report_json_path.c_str());
    }
    return 0;
}
```

- [x] **Step 2: Build it**

Run:
```bash
mamba run -n mts g++ -O2 -std=c++17 \
  $(mamba run -n mts pkg-config --cflags arrow parquet) \
  tools/jump_ratio_eval.cpp \
  $(mamba run -n mts pkg-config --libs arrow parquet) \
  -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
  -o tools/jump_ratio_eval
```
Expected: compiles cleanly. Fix any real compile errors surfaced here.

- [x] **Step 3: Run end-to-end against the real file**

Run: `./tools/jump_ratio_eval --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_continuous_ticks.parquet --report-json /tmp/jump_ratio_report.json`

Expected: `Loaded 38547467 rows...`, a non-zero signal count, then a 4-row table (one per horizon) with **no `nan`/`inf`**, `n_top`/`n_bot` each well above the 30-sample floor, and a `gap` value with a finite, ordered CI. Read the actual numbers before declaring this done — if `gap` is exactly `0.0` or the CI is degenerate `[0,0]`, that indicates a real bug (e.g. an indexing error making top/bottom groups identical), not a null result to accept at face value.

- [x] **Step 4: Verify the JSON report**

```bash
mamba run -n mts python3 -c "
import json
report = json.load(open('/tmp/jump_ratio_report.json'))
assert report['window'] == 100
for h in report['horizons']:
    assert h['n_top'] >= 30 and h['n_bot'] >= 30, h
    assert h['ci_lo'] <= h['ci_hi'], h
print('✅ jump_ratio_eval JSON report verified:', report['horizons'])
"
```
Expected: prints the verification line and the actual per-horizon results — read them, this is the actual prototype answer this plan exists to produce.

- [x] **Step 5: Commit**

```bash
git add tools/jump_ratio_eval.cpp
git commit -m "feat: complete jump_ratio_eval CLI, verified end-to-end against real MES data"
```

---

## Self-Review Notes (completed during authoring)

- **Spec coverage**: §5.1's exact RV/BV/jump_ratio formula (Task 2), the magnitude-vs-directional test decision reasoned through explicitly and confirmed against the real `dim_acceptance_eval.py` source before finalizing (Task 3), model-independence maintained throughout (no HMM reference anywhere), the "second use → extract" refactor (Task 1) done before it was needed a third time. All Global Constraints (RPATH, column projection, two-pointer merge, real end-to-end verification) inherited directly from the drift-location precedent and re-applied here without rediscovery.
- **Placeholder scan**: no `TBD`/`TODO`; every step has complete, real code.
- **Type consistency**: `TickSeries`/`ReadTicksParquet` (Task 1) used identically in Task 4. `ComputeForwardReturns`/`ComputeBootstrapMeanGapCI`/`BootstrapGapResult` (Tasks 1/3) used identically in Task 4. `ComputeJumpRatio` (Task 2) used identically in Task 4. No signature drift between definition and use.
