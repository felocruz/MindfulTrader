# Drift/Location Offline Prototype Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a standalone, native C++/Arrow tool (`tools/drift_location_eval.cpp`) that computes the §5.0 "drift/location" candidate (a volatility-normalized realized-return z-score) directly from real historical MES tick data and tests it against real forward market returns — entirely independent of the trained HMM model, with no Python sibling and no schema/wire-format change.

**Architecture:** Two files, split by the same pure-math/thin-glue precedent already used for `context_validate_stats.h`/`context_validate.cpp`. `tools/drift_location_stats.h` holds every pure numeric function (log returns, rolling z-score, forward returns, Wilson CI, hit-rate/Bonferroni test) — no Arrow dependency, fully natively testable with hand-verified reference values. `tools/drift_location_eval.cpp` holds the CLI and the one genuinely new capability this tool introduces to the codebase: reading a Parquet file **directly** in C++ via `parquet::arrow::FileReader` (every existing tool only *writes* Parquet; nothing reads one yet). This eliminates the old `mean_rev_z_variant_comparison.py` pattern's Python round-trip (polars read → custom binary export → C++ read → CSV → Python scores) entirely.

**Tech Stack:** C++17, Apache Arrow/Parquet 22.0.0 C++ (via the `mamba run -n mts` environment). Native tests via bare `g++` + hand-rolled `check(name, bool)` (this codebase's real convention).

**Spec:** `docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md` §5.0 (drift/location candidate definition and rationale). This plan implements only the offline prototype step of §6.0's Phase 1 — it does **not** touch `mts_schema.fbs`, does **not** wire anything into `ObservationData`, and does **not** compute or consume any HMM state label. Cross-state-ratio validation against `hmm_model.pkl` is explicitly out of scope for this plan (the model was trained on data with multiple confirmed invalidities — bar-gated `amihud_illiquidity`/`liq_fragility`, a `FeatureScaler` dedupe-corruption bug affecting 31-71% of samples across 6 dims, and a `fast_hurst_exponent`-insertion index-shift miscalibration of 3 dims — using its state decode as ground truth right now would be circular). This tool validates against real forward market returns only.

## Global Constraints

- **No Python sibling, no CSV round-trip.** `mes_continuous_ticks.parquet` is read directly in C++ via Arrow.
- **RPATH required on every Arrow/Parquet-linked build** — confirmed necessary: the already-built `context_to_parquet` binary fails in a clean shell (`env -i`) with `error while loading shared libraries: libparquet.so.2200`; it only worked before because the interactive shell already had the `mts` env's `LD_LIBRARY_PATH` exported. Every build command in this plan includes `-Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib`. Task 4 also retrofits this onto `context_to_parquet.cpp`'s documented build command, which has the identical gap.
- **No parallel Arrow installation.** Confirmed via `dpkg`/`apt list` that no system Arrow package exists. None gets installed — this project's standing rule is one environment (`mts`) for every native and Python dependency, no exceptions.
- **Never touches `build_dll.sh` or `CMakeLists.txt`** — standalone tool under `tools/`, matching every other tool in this directory.
- **Statistical formulas are ported verbatim from `tools/dim_acceptance_eval.py`**, not reinvented: `compute_forward_returns()` (lines 189-200), `wilson_ci()` (lines 171-178), and `predictive_power_directional()`'s same-sign hit-rate test (lines 203-231, `hits = sign(fwd) == sign(candidate)` — a **continuation** hypothesis, correct for a drift/momentum candidate). Do not use `mean_rev_z_variant_comparison.py`'s *negated*-sign test — that's specific to its own mean-reversion candidate and is the wrong hypothesis here.
- **Default horizons: 30, 60, 120, 240 minutes** (matches `dim_acceptance_eval.py`'s own default) with Bonferroni correction across them (`alpha = 0.05 / n_horizons`).
- **Default rolling window: 100 bars**, exposed as a CLI flag (not hardcoded) so it can be swept without recompiling.

---

## Task 1: `tools/drift_location_stats.h` — log returns + rolling z-score

**Files:**
- Create: `tools/drift_location_stats.h`
- Create: `tools/test_drift_location_stats.cpp`

**Interfaces:**
- Produces: `std::vector<double> ComputeLogReturns(const std::vector<double>& prices)`; `std::vector<double> ComputeDriftZScore(const std::vector<double>& log_returns, std::size_t window)` (returns a vector the same length as `log_returns`; the first `window - 1` entries are `NaN` — not enough data yet; a zero-variance window returns `0.0`, not `NaN`/`Inf`, avoiding downstream propagation).

- [ ] **Step 1: Write the failing test**

```cpp
// tools/test_drift_location_stats.cpp
// Build & run: mamba run -n mts g++ -std=c++17 tools/test_drift_location_stats.cpp \
//   -o /tmp/drift_location_stats_test && /tmp/drift_location_stats_test
#include "drift_location_stats.h"
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
        // prices = [1, 2, 4] -> log returns = [log(2), log(2)] (doubling each step)
        std::vector<double> prices = {1.0, 2.0, 4.0};
        auto returns = ComputeLogReturns(prices);
        check("2 returns from 3 prices", returns.size() == 2);
        check("both returns equal log(2)", close(returns[0], std::log(2.0)) && close(returns[1], std::log(2.0)));
    }
    {
        check("empty vector for <2 prices", ComputeLogReturns({1.0}).empty());
    }
    {
        // Hand-computed rolling z-score, window=2, over returns = [1, 3, 2, 4]:
        //   i=1: window=[1,3] mean=2 var=(1+9)/2-4=1 std=1 z=2.0
        //   i=2: window=[3,2] mean=2.5 var=(9+4)/2-6.25=0.25 std=0.5 z=5.0
        //   i=3: window=[2,4] mean=3 var=(4+16)/2-9=1 std=1 z=3.0
        std::vector<double> returns = {1.0, 3.0, 2.0, 4.0};
        auto z = ComputeDriftZScore(returns, 2);
        check("z has same length as input", z.size() == 4);
        check("z[0] is NaN (warmup)", std::isnan(z[0]));
        check("z[1] == 2.0", close(z[1], 2.0));
        check("z[2] == 5.0", close(z[2], 5.0));
        check("z[3] == 3.0", close(z[3], 3.0));
    }
    {
        // Constant returns -> zero variance -> z defined as 0.0, not NaN/Inf.
        std::vector<double> constant_returns = {0.5, 0.5, 0.5};
        auto z = ComputeDriftZScore(constant_returns, 2);
        check("zero-variance window yields z=0.0, not NaN/Inf", close(z[1], 0.0) && close(z[2], 0.0));
    }
    {
        check("window larger than input yields all-NaN",
              std::isnan(ComputeDriftZScore({1.0, 2.0}, 5)[0]));
    }

    std::printf(g_failures == 0 ? "ALL PASS\n" : "%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
```

- [ ] **Step 2: Run test to verify it fails**

Run: `mamba run -n mts g++ -std=c++17 tools/test_drift_location_stats.cpp -o /tmp/drift_location_stats_test`
Expected: FAIL — `drift_location_stats.h` does not exist, compile error.

- [ ] **Step 3: Write the implementation**

```cpp
// tools/drift_location_stats.h
// Pure numeric functions for the §5.0 drift/location offline prototype
// (docs/superpowers/specs/2026-08-29-hmm-fat-tail-observation-vector-
// brainstorm.md). No Arrow dependency -- fully natively testable. Mirrors
// tools/context_validate_stats.h's pure-math/thin-glue split for the same
// reason: keep CLI/Arrow concerns out of the natively-tested layer.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
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

// Volatility-normalized rolling return z-score: z[i] = mean(w) / std(w), where
// w is the `window`-wide trailing slice of log_returns ending at i. O(n) via a
// sliding sum/sum-of-squares accumulator, not O(n*window) -- DOD discipline,
// matters at this tool's real 38.5M-row scale.
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

- [ ] **Step 4: Run test to verify it passes**

Run: `mamba run -n mts g++ -std=c++17 tools/test_drift_location_stats.cpp -o /tmp/drift_location_stats_test && /tmp/drift_location_stats_test`
Expected: `ALL PASS` (8 checks)

- [ ] **Step 5: Commit**

```bash
git add tools/drift_location_stats.h tools/test_drift_location_stats.cpp
git commit -m "feat: add log-return + rolling drift z-score computation for §5.0 prototype"
```

---

## Task 2: Forward returns + Wilson CI + hit-rate/Bonferroni test

**Files:**
- Modify: `tools/drift_location_stats.h`
- Modify: `tools/test_drift_location_stats.cpp`

**Interfaces:**
- Produces: `std::vector<double> ComputeForwardReturns(const std::vector<std::int64_t>& signal_ts, const std::vector<double>& signal_price, const std::vector<std::int64_t>& timestamps, const std::vector<double>& prices, int horizon_minutes)`; `struct WilsonInterval { double lo; double hi; }; WilsonInterval ComputeWilsonCI(std::size_t k, std::size_t n, double z = 1.96)`; `int Sign(double x)`; `struct HitRateResult { std::size_t n = 0; std::size_t k = 0; double hit_rate = 0.0; double ci_lo = 0.0; double ci_hi = 0.0; double z_stat = 0.0; double p_value = 1.0; }; HitRateResult ComputeHitRate(const std::vector<double>& forward_returns, const std::vector<double>& candidate_values)`.

- [ ] **Step 1: Extend the test**

Add to `tools/test_drift_location_stats.cpp`, before the final `std::printf`:

```cpp
    {
        // timestamps in microseconds: t=0, t=60s, t=120s; prices 100, 110, 121.
        // Signal at t=0, price=100, horizon=1 minute (60s) -> target_ts=60_000_000,
        // exact match at index 1 -> fwd = log(110/100).
        std::vector<std::int64_t> timestamps = {0, 60'000'000, 120'000'000};
        std::vector<double> prices = {100.0, 110.0, 121.0};
        auto fwd = ComputeForwardReturns({0}, {100.0}, timestamps, prices, /*horizon_minutes=*/1);
        check("forward return at exact horizon match", close(fwd[0], std::log(110.0 / 100.0)));

        // Horizon far beyond available data -> no valid target -> NaN.
        auto fwd_oob = ComputeForwardReturns({0}, {100.0}, timestamps, prices, /*horizon_minutes=*/1000);
        check("forward return out of bounds is NaN", std::isnan(fwd_oob[0]));

        // Gap guard: signal at t=0 but next available timestamp is way beyond
        // 3x the horizon -- matches dim_acceptance_eval.py's own gap_ok rule.
        std::vector<std::int64_t> gappy_ts = {0, 10'000'000'000};  // huge gap
        std::vector<double> gappy_px = {100.0, 999.0};
        auto fwd_gap = ComputeForwardReturns({0}, {100.0}, gappy_ts, gappy_px, /*horizon_minutes=*/1);
        check("forward return beyond 3x horizon gap is NaN (guard)", std::isnan(fwd_gap[0]));
    }
    {
        // Reference values computed directly from tools/dim_acceptance_eval.py's
        // own wilson_ci() (verified via a real mamba run -n mts python3 invocation,
        // not hand-arithmetic): wilson_ci(50, 100) = (0.40382982859014716, 0.5961701714098528)
        auto ci = ComputeWilsonCI(50, 100);
        check("WilsonCI(50,100) lo matches Python reference", close(ci.lo, 0.40382982859014716, 1e-9));
        check("WilsonCI(50,100) hi matches Python reference", close(ci.hi, 0.5961701714098528, 1e-9));

        // wilson_ci(70, 100) = (0.6041496156718804, 0.7810524613377188)
        auto ci2 = ComputeWilsonCI(70, 100);
        check("WilsonCI(70,100) lo matches Python reference", close(ci2.lo, 0.6041496156718804, 1e-9));
        check("WilsonCI(70,100) hi matches Python reference", close(ci2.hi, 0.7810524613377188, 1e-9));
    }
    {
        check("Sign(5.0) == 1", Sign(5.0) == 1);
        check("Sign(-5.0) == -1", Sign(-5.0) == -1);
        check("Sign(0.0) == 0", Sign(0.0) == 0);
    }
    {
        // 70 hits out of 100, all candidate values nonzero and finite.
        std::vector<double> forward_returns(100, 1.0);   // all positive forward returns
        std::vector<double> candidate_values(100, 1.0);  // all positive candidate -> hit
        for (int i = 0; i < 30; ++i) candidate_values[i] = -1.0;  // 30 misses (opposite sign)
        auto result = ComputeHitRate(forward_returns, candidate_values);
        check("n == 100", result.n == 100);
        check("k == 70", result.k == 70);
        check("hit_rate == 0.70", close(result.hit_rate, 0.70));
        // Reference values from a real mamba run -n mts python3 computation:
        // hit_rate=0.70, n=100 -> z=3.999999999999999, p=6.334248366623996e-05
        check("z_stat matches Python reference", close(result.z_stat, 3.999999999999999, 1e-6));
        check("p_value matches Python reference", close(result.p_value, 6.334248366623996e-05, 1e-9));
    }
    {
        // Zero candidate values are excluded from n (matches dim_acceptance_eval.py's
        // `mask = np.isfinite(fwd) & (cand_val != 0)`).
        std::vector<double> forward_returns = {1.0, 1.0, 1.0};
        std::vector<double> candidate_values = {1.0, 0.0, 1.0};
        auto result = ComputeHitRate(forward_returns, candidate_values);
        check("zero candidate values excluded from n", result.n == 2);
    }
```

- [ ] **Step 2: Run test to verify it fails**

Run: `mamba run -n mts g++ -std=c++17 tools/test_drift_location_stats.cpp -o /tmp/drift_location_stats_test`
Expected: FAIL — `ComputeForwardReturns`/`ComputeWilsonCI`/`Sign`/`ComputeHitRate`/`WilsonInterval`/`HitRateResult` undeclared.

- [ ] **Step 3: Write the implementation**

Add to `tools/drift_location_stats.h`:

```cpp
// Mirrors tools/dim_acceptance_eval.py's compute_forward_returns() semantics
// exactly (lines 189-200: forward log-return from signal_price at signal_ts to
// the first available price at or after signal_ts + horizon, rejected if that
// target is beyond the series or the actual gap exceeds 3x the horizon), but
// as an O(n) two-pointer merge instead of Python's O(n log n) np.searchsorted.
// REQUIRES both signal_ts and timestamps to be sorted ascending (true here:
// signal_ts is a filtered subsequence of timestamps, verified sorted via a
// real Polars is_sorted() check against the actual production Parquet file).
// This isn't a micro-optimization -- an earlier binary-search-per-signal
// version was measured taking 100+ seconds (timed out) on the real 38.5M-row
// series across 4 horizons: each independent std::lower_bound probe jumps
// unpredictably through a ~308MB array that doesn't fit in cache, thrashing
// it on every level of the search. A single forward-only pointer never
// revisits memory, so this stays cache-friendly at this tool's real scale.
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
        // idx only ever advances across the whole loop: target_ts is
        // non-decreasing in i (signal_ts is sorted, horizon_us is constant),
        // so the first timestamps[idx] >= target_ts is also non-decreasing.
        while (idx < m && timestamps[idx] < target_ts) {
            ++idx;
        }
        if (idx >= m) {
            break;  // every later target_ts is >= this one -- none can match either.
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

// Mirrors tools/dim_acceptance_eval.py's wilson_ci() exactly (lines 171-178).
inline WilsonInterval ComputeWilsonCI(std::size_t k, std::size_t n, double z = 1.96) {
    const double nd = static_cast<double>(n);
    const double phat = static_cast<double>(k) / nd;
    const double denom = 1.0 + z * z / nd;
    const double center = (phat + z * z / (2.0 * nd)) / denom;
    const double half = (z / denom) * std::sqrt(phat * (1.0 - phat) / nd + z * z / (4.0 * nd * nd));
    return {center - half, center + half};
}

// Matches numpy's np.sign() exactly: -1, 0, or +1.
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

// Mirrors tools/dim_acceptance_eval.py's predictive_power_directional() hit-rate
// test exactly (lines 203-231): SAME-SIGN (continuation) hypothesis, the correct
// one for a drift/momentum candidate -- NOT mean_rev_z_variant_comparison.py's
// negated-sign reversion test, which is specific to that different candidate.
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

- [ ] **Step 4: Run test to verify it passes**

Run: `mamba run -n mts g++ -std=c++17 tools/test_drift_location_stats.cpp -o /tmp/drift_location_stats_test && /tmp/drift_location_stats_test`
Expected: `ALL PASS` (21 checks)

- [ ] **Step 5: Commit**

```bash
git add tools/drift_location_stats.h tools/test_drift_location_stats.cpp
git commit -m "feat: add forward-return/Wilson-CI/hit-rate functions, verified against real Python reference values"
```

---

## Task 3: Arrow Parquet reader utility

**Files:**
- Create: `tools/drift_location_eval.cpp` (skeleton: reader utility + smoke-test `main()` only — CLI/statistics wiring comes in Task 4)

**Interfaces:**
- Produces: `struct TickSeries { std::vector<std::int64_t> timestamp_us; std::vector<double> close; }`; `TickSeries ReadTicksParquet(const std::string& path)` (throws `std::runtime_error` on any Arrow/Parquet error).

- [ ] **Step 1: Write the implementation directly (no separate failing-test step — this task's own `main()` IS its smoke test against real data, matching this repo's precedent for tools whose only meaningful test is against the real file, e.g. Task 9's e2e verification in the converter plan)**

```cpp
// tools/drift_location_eval.cpp
// Offline, model-independent prototype validator for the §5.0 drift/location
// observation-vector candidate. Reads lbrnet/data/raw/mes_continuous_ticks.parquet
// DIRECTLY via Arrow -- the first C++ tool in this codebase to read Parquet
// (every prior tool, e.g. context_to_parquet.cpp, only writes). This removes
// the old mean_rev_z_variant_comparison.py pattern's Python round-trip
// (polars-read -> custom-binary-export -> C++-read -> CSV -> Python-scores).
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   tools/drift_location_eval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/drift_location_eval
#include "drift_location_stats.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <cstdio>
#include <stdexcept>
#include <string>
#include <vector>

struct TickSeries {
    std::vector<std::int64_t> timestamp_us;
    std::vector<double> close;
};

namespace {

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

}  // namespace

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
    // reading the full table via a bare ReadTable(&table) was measured at 5+
    // minutes with zero output on this 38.5M-row file (killed as unreasonably
    // slow); projected, it's 7.5s. Indices are resolved from the real schema,
    // never hardcoded, so a future column reorder in the source Parquet can't
    // silently read the wrong data.
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
    series.timestamp_us = FlattenColumn<arrow::Int64Array, std::int64_t>(ts_col);
    series.close = FlattenColumn<arrow::DoubleArray, double>(close_col);
    return series;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        std::fprintf(stderr, "usage: %s <path-to-mes_continuous_ticks.parquet>\n", argv[0]);
        return 1;
    }
    try {
        TickSeries series = ReadTicksParquet(argv[1]);
        std::printf("✅ read %zu rows, timestamp range [%lld, %lld], first close=%.4f, last close=%.4f\n",
                    series.timestamp_us.size(),
                    static_cast<long long>(series.timestamp_us.front()),
                    static_cast<long long>(series.timestamp_us.back()),
                    series.close.front(), series.close.back());
    } catch (const std::runtime_error& e) {
        std::fprintf(stderr, "❌ %s\n", e.what());
        return 1;
    }
    return 0;
}
```

- [ ] **Step 2: Build it**

Run:
```bash
mamba run -n mts g++ -O2 -std=c++17 \
  $(mamba run -n mts pkg-config --cflags arrow parquet) \
  tools/drift_location_eval.cpp \
  $(mamba run -n mts pkg-config --libs arrow parquet) \
  -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
  -o tools/drift_location_eval
```
Expected: compiles cleanly. Fix any real compile errors surfaced here (this is the first real compilation of the Arrow-read code path — treat it as the actual verification gate, not a formality).

- [ ] **Step 3: Smoke-test against the real file**

Run: `./tools/drift_location_eval /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_continuous_ticks.parquet`
Expected: `✅ read 38547467 rows, timestamp range [...], first close=..., last close=...` — confirm the row count is exactly `38547467` (verified earlier this session via a direct `polars` scan) and that `first close`/`last close` are plausible MES futures prices (roughly 3000-8000 range, not zero/NaN/garbage). Actual verified result: `✅ read 38547467 rows, timestamp range [1685916000000000, 1787016924000000], first close=4330.0000, last close=7765.5000` in 7.5s (real).

- [ ] **Step 4: Verify runtime portability (the RPATH fix actually works)**

Run: `env -i HOME="$HOME" PATH="/usr/bin:/bin" ./tools/drift_location_eval /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_continuous_ticks.parquet`
Expected: succeeds identically in a genuinely clean environment (no mamba activation) — this is the actual test of the RPATH fix; if this fails with a `libparquet.so` error, the `-Wl,-rpath,...` flag was dropped or misapplied.

- [ ] **Step 5: Commit**

```bash
git add tools/drift_location_eval.cpp
git commit -m "feat: add Arrow Parquet reader (first direct Parquet read in this codebase), RPATH-portable"
```

---

## Task 4: CLI wiring, end-to-end run, and the `context_to_parquet.cpp` RPATH retrofit

**Files:**
- Modify: `tools/drift_location_eval.cpp` (replace the smoke-test `main()` with the real CLI)
- Modify: `docs/superpowers/plans/2026-08-30-context-to-parquet-cpp-arrow-converter.md` (Task 9's documented build command — add the RPATH flag as a small addendum, since the same portability gap was confirmed there too)

**Interfaces:**
- Consumes: everything in `tools/drift_location_stats.h` (Tasks 1-2) and `ReadTicksParquet`/`TickSeries` (Task 3).
- Produces: the final `tools/drift_location_eval` CLI: `--ticks-parquet PATH [--window N] [--horizons 30,60,120,240] [--report-json PATH]`.

- [ ] **Step 1: Replace `main()` with the real CLI**

```cpp
// Replace the Task 3 smoke-test main() with this. Add near the top of the
// file, alongside the existing #includes:
#include <cstdlib>
#include <fstream>
#include <sstream>

namespace {

// No JsonEscape() here (unlike context_to_parquet.cpp/context_validate.cpp) --
// this tool's JSON report is purely numeric, no string fields to escape.
// A pre-fix draft defined one anyway and never called it; removed as dead
// code once the -Wunused-function warning caught it during real compilation.
void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --ticks-parquet PATH [--window N] [--horizons 30,60,120,240] "
        "[--report-json PATH]\n", argv0);
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

}  // namespace

int main(int argc, char** argv) {
    std::string ticks_path, report_json_path;
    std::size_t window = 100;
    std::string horizons_csv = "30,60,120,240";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--ticks-parquet") ticks_path = next("--ticks-parquet");
        else if (arg == "--window") window = std::stoull(next("--window"));
        else if (arg == "--horizons") horizons_csv = next("--horizons");
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

    const auto log_returns = ComputeLogReturns(series.close);
    const auto z = ComputeDriftZScore(log_returns, window);
    // z[] is indexed against log_returns (length n-1), which is itself offset
    // by 1 from series.close/timestamp_us -- signal at return-index i corresponds
    // to price-series index i+1 (the price AFTER the return that produced z[i]).
    std::vector<std::int64_t> signal_ts;
    std::vector<double> signal_price, signal_z;
    for (std::size_t i = 0; i < z.size(); ++i) {
        if (std::isnan(z[i])) continue;
        signal_ts.push_back(series.timestamp_us[i + 1]);
        signal_price.push_back(series.close[i + 1]);
        signal_z.push_back(z[i]);
    }
    std::printf("%zu non-warmup drift/location signals (window=%zu)\n", signal_ts.size(), window);

    const auto horizons = ParseHorizons(horizons_csv);
    const double bonferroni_alpha = 0.05 / static_cast<double>(horizons.size());

    std::printf("\n=== Predictive power (directional, continuation): forward-return sign vs. drift z-score sign ===\n");
    std::ofstream json_out;
    if (!report_json_path.empty()) {
        json_out.open(report_json_path);
        json_out << "{\n  \"window\": " << window << ",\n  \"horizons\": [\n";
    }
    for (std::size_t h_idx = 0; h_idx < horizons.size(); ++h_idx) {
        const int h = horizons[h_idx];
        const auto fwd = ComputeForwardReturns(signal_ts, signal_price, series.timestamp_us, series.close, h);
        const auto result = ComputeHitRate(fwd, signal_z);
        const bool survives = result.p_value < bonferroni_alpha;
        std::printf("  %4dmin: n=%-8zu hit_rate=%.4f 95%%CI=[%.4f,%.4f] p=%.4f (%s, alpha=%.5f for %zu tests)\n",
                    h, result.n, result.hit_rate, result.ci_lo, result.ci_hi, result.p_value,
                    survives ? "SURVIVES Bonferroni" : "does not survive Bonferroni",
                    bonferroni_alpha, horizons.size());
        if (json_out.is_open()) {
            json_out << "    {\"horizon_minutes\": " << h << ", \"n\": " << result.n
                      << ", \"hit_rate\": " << result.hit_rate
                      << ", \"ci_lo\": " << result.ci_lo << ", \"ci_hi\": " << result.ci_hi
                      << ", \"p_value\": " << result.p_value
                      << ", \"survives_bonferroni\": " << (survives ? "true" : "false") << "}"
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

- [ ] **Step 2: Build it**

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

- [ ] **Step 3: Run end-to-end against the real file**

Run: `./tools/drift_location_eval --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_continuous_ticks.parquet --report-json /tmp/drift_location_report.json`

Expected: prints `Loaded 38547467 rows...`, a non-zero signal count, then a 4-row table (one per default horizon) with **no `nan`/`inf` in any field**, `n` values in the tens of millions minus warmup/gap exclusions, `hit_rate` strictly between 0 and 1, and `95%CI` bounds that bracket `hit_rate`. Read the actual numbers — do not just check the process exited 0. If `hit_rate` is suspiciously exactly `0.5000` or `n` is `0`, that's a real bug (likely an indexing-offset error between `z[]` and the price series), not a null result to accept at face value.

- [ ] **Step 4: Verify the JSON report**

```bash
mamba run -n mts python3 -c "
import json
report = json.load(open('/tmp/drift_location_report.json'))
assert report['window'] == 100
assert len(report['horizons']) == 4
for h in report['horizons']:
    assert 0.0 < h['hit_rate'] < 1.0, h
    assert h['n'] > 0, h
    assert h['ci_lo'] <= h['hit_rate'] <= h['ci_hi'], h
print('✅ drift_location_eval JSON report verified:', report['horizons'])
"
```
Expected: prints the verification line and the actual per-horizon results — read them, this is the actual prototype answer this whole plan exists to produce.

- [ ] **Step 5: Retrofit the RPATH fix onto `context_to_parquet.cpp`'s documented build command**

In `docs/superpowers/plans/2026-08-30-context-to-parquet-cpp-arrow-converter.md`, find Task 9's documented build command (the `mamba run -n mts g++ -O2 -std=c++17 -Iinclude $(...) tools/context_to_parquet.cpp $(...) -o tools/context_to_parquet` block) and add `-Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib` before `-o tools/context_to_parquet`, with a one-line note that this was confirmed necessary via a clean-shell (`env -i`) test on 2026-08-30, matching this plan's own Task 3/Global Constraints finding. Then actually rebuild the real binary with the fix:

```bash
mamba run -n mts g++ -O2 -std=c++17 -Iinclude \
  $(mamba run -n mts pkg-config --cflags arrow parquet) \
  tools/context_to_parquet.cpp \
  $(mamba run -n mts pkg-config --libs arrow parquet) \
  -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
  -o tools/context_to_parquet
env -i HOME="$HOME" PATH="/usr/bin:/bin" ./tools/context_to_parquet --input /tmp/e2e_test.context --output /tmp/rpath_check.parquet --mode unbounded
```
Expected: the clean-shell run succeeds (previously it would have failed with `libparquet.so.2200` missing, matching this plan's own Task 3 Step 4 finding for the new tool).

- [ ] **Step 6: Commit**

```bash
git add tools/drift_location_eval.cpp docs/superpowers/plans/2026-08-30-context-to-parquet-cpp-arrow-converter.md
git commit -m "feat: complete drift_location_eval CLI, verified end-to-end against real MES data; retrofit RPATH onto context_to_parquet's build docs"
```

---

## Self-Review Notes (completed during authoring)

- **Spec coverage**: §5.0's exact formula (z = mean(log_returns)/std(log_returns) over a rolling window) — Task 1. Model-independence (no HMM state label anywhere) — enforced throughout, explicitly called out in Global Constraints. Statistical methodology ported from `dim_acceptance_eval.py`, not reinvented — Task 2, with reference values independently verified via a real `mamba run -n mts python3` execution of the actual Python formulas (not hand-arithmetic). Arrow-native Parquet read, no Python sibling — Task 3. RPATH portability, including retrofitting the same fix onto the earlier converter tool — Task 3 Step 4 (new tool) and Task 4 Step 5 (retrofit).
- **Placeholder scan**: no `TBD`/`TODO`; every step has complete, real code.
- **Type consistency**: `TickSeries`/`ReadTicksParquet` (Task 3) are consumed identically in Task 4's `main()`. `ComputeDriftZScore`/`ComputeForwardReturns`/`ComputeHitRate`/`WilsonInterval`/`HitRateResult` (Tasks 1-2) are consumed with matching signatures in Task 4 — no drift between definition and use.
