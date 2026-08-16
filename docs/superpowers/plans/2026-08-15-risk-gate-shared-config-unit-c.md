# Shared Calibration Config (Unit C) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give both live config files (`execution_params.json`, `hmm_regime_risk_policy.json`) real git history via a new `MindfulTrader/config/` folder, move `FeatureScaler.h`'s hardcoded winsorization-bound constants into that shared, versioned, provenance-tracked config (loaded once at startup, never per-tick), and add a script that pushes the git-tracked config to the live deployment path.

**Architecture:** `config/` becomes the source of truth; `/mnt/c/Trading/config/` becomes a deployment target populated by a new push script. `FeatureScaler.h`'s four `static constexpr` arrays become `static` (non-`constexpr`) members with the same compiled-default values, overwritten in place by a new `FeatureScaler::LoadConfig()` static method — this exactly mirrors the existing lazy-load-once pattern `RiskManager.cpp`'s `GetHMMRiskPolicy()` already uses for the same class of problem, so every existing call site (`FeatureScaler::STATE_WINSOR_SIGMA`, unqualified internal use, array indexing) keeps working unchanged. `LoadConfig()` is invoked exactly once, from `ContextManager`'s constructor (a Meyer's singleton — constructed exactly once via `ContextManager::Instance()`'s function-local static).

**Tech Stack:** C++17 (`nlohmann::json`, already used by `ExecutionParams`/`RiskManager`'s own config loaders), Python 3 (stdlib only — `os`, `shutil`, `datetime`, `pathlib`, `json`).

**Spec:** `docs/superpowers/specs/2026-08-15-risk-gate-context-cpp-coevolution.md` (Unit C).

## Global Constraints

- **Load-once, not per-tick.** `FeatureScaler` is a declared hot-path class (every TS1/TS2/TS3 tick, per this repo's own `CLAUDE.md` Performance Rules). The config load must happen exactly once, at `ContextManager` construction, matching `GetHMMRiskPolicy()`'s existing `static bool loaded` idiom (`src/RiskManager.cpp:105-115`) — never a per-tick file read or per-tick branch beyond what already exists.
- **Preserve every existing call site's syntax.** ~20 call sites (`FeatureScaler.h` internal, `tests/cpp/test_feature_scaler.cpp` external) reference these constants as `FeatureScaler::CONST_NAME` or unqualified from inside member functions. The fix keeps them `static` class members (dropping only `constexpr`, since a value must be assignable after load) — do not convert to instance members or accessor-function calls.
- **Do not touch `tools/rqa_recurrence_calibration.cpp:180`'s hardcoded `std::clamp(z, -6.0, 6.0)` literal** — it's an independent duplicate constant in a standalone calibration tool, not wired to `FeatureScaler::STATE_WINSOR_SIGMA`, and reconciling it is out of scope for this plan (note it in Task 2's commit message as a known, unaddressed duplicate).
- **`config/` is the source of truth; `/mnt/c/Trading/config/` is a deployment target.** Never write the reverse direction (live path → git) as part of this plan's push script.
- **Do not modify or reimplement `promote_to_cpp_config.py`** (`../lbrnet/lbrnet/scripts/promote_to_cpp_config.py`) — it is a separate script in a separate repo, explicitly staying as-is per the spec's Non-Goals. This plan's new push script is additive and lives entirely in `MindfulTrader/scripts/`; it may replicate (copy, not import) `promote_to_cpp_config.py`'s `_write_json_atomic`/`_backup_file` helper logic, since cross-repo Python imports are not appropriate here.
- **Do not build the lbrnet-side `empirical_gate_thresholds` sync script** (spec design point 5 — "run from lbrnet after `calibrate_hmm_gate_thresholds.py` produces a fresh file"). That script lives in the lbrnet repo and is a separate session's work — out of scope here.
- **No CI gate, no forced-acknowledgment blocking build** — mismatches get a loud log line only, per this repo's established "loudly logged, never silent" convention. Never a blocked build or approval step.
- **Versioning:** `execution_params.json` is already `"1.0.2"`-format compliant — bump to `"1.1.0"` (adding a new optional section is a minor addition). `hmm_regime_risk_policy.json` converts from `"2026-06-11.v3"` to `"1.0.0"` (first version under the new unified scheme — there is no direct numeric predecessor to increment from, so this plan starts the new scheme at `1.0.0`, keeping `empirical_gate_thresholds.generated_at_utc` as the separate calibration-freshness timestamp it already is).

---

### Task 1: Create the git-tracked `config/` folder with unified-version, sectioned-ownership JSON files

**Files:**
- Create: `config/execution_params.json` (copied from live `/mnt/c/Trading/config/execution_params.json`, then augmented)
- Create: `config/hmm_regime_risk_policy.json` (full literal replacement, content given below)

**Interfaces:**
- Produces: `config/execution_params.json`'s new `featurescaler_winsorization` section (top-level key `featurescaler_winsorization`, fields `state_winsor_sigma` (float) and `dims` (array of 16 objects, each `{index, name, dim_winsor_sigma_override, logz_winsor_sigma_override, shrinkage_scale_min, note}`)) — Task 2's `FeatureScaler::LoadConfig()` reads exactly this shape.

- [x] **Step 1: Create the `config/` directory and copy the live `execution_params.json` as a starting point**

```bash
mkdir -p config
cp /mnt/c/Trading/config/execution_params.json config/execution_params.json
```

- [x] **Step 2: Additively merge the new top-level keys into `config/execution_params.json` via `jq`** (additive merge — every existing key, including `trap_config`'s full nested structure, is preserved untouched; only the keys below are added/overwritten)

```bash
jq '. + {
  "_version": "1.1.0",
  "_owner": "manual/MindfulTrader",
  "_generated_by": "manual/MindfulTrader",
  "featurescaler_winsorization": {
    "_owner": "manual/MindfulTrader",
    "_generated_by": "manual/MindfulTrader (docs/superpowers/specs/2026-08-15-risk-gate-context-cpp-coevolution.md Unit C; values carried forward verbatim from include/FeatureScaler.h compiled defaults, zero behavior change on first load)",
    "_updated": "2026-08-15",
    "state_winsor_sigma": 6.0,
    "dims": [
      {"index": 0,  "name": "log_variance_ratio", "dim_winsor_sigma_override": 262.0, "logz_winsor_sigma_override": 0.0,  "shrinkage_scale_min": 0.000389, "note": "GPD-derived on corrected z, Frechet (xi=+0.2533), p=1/N return level; shrinkage: confirmed collapse signature (z=-176 traced)"},
      {"index": 1,  "name": "burstiness_index",   "dim_winsor_sigma_override": 45.0,  "logz_winsor_sigma_override": 0.0,  "shrinkage_scale_min": 0.0,      "note": "GPD+bootstrap derived (D7); shrinkage disabled -- tail decays cleanly under generic path + wide bound"},
      {"index": 2,  "name": "relative_range",     "dim_winsor_sigma_override": 0.0,   "logz_winsor_sigma_override": 0.0,  "shrinkage_scale_min": 0.0,      "note": "audited, closed clean (0 exceedances on 18,761 real bars); shrinkage disabled -- LOGZ, 0.035% clip rate, no evidence of need"},
      {"index": 3,  "name": "correction_action",  "dim_winsor_sigma_override": 4587.0,"logz_winsor_sigma_override": 0.0,  "shrinkage_scale_min": 0.00007,  "note": "GPD-derived, p=1/N return level (D7); shrinkage: original D4 derivation"},
      {"index": 4,  "name": "vol_convexity",      "dim_winsor_sigma_override": 0.0,   "logz_winsor_sigma_override": 12.0, "shrinkage_scale_min": 0.211521, "note": "LOGZ dim, GPD-derived on corrected (shrinkage-blended) z, real margin above theoretical wall; first LOGZ dim to use the shrinkage mechanism"},
      {"index": 5,  "name": "lempel_ziv",         "dim_winsor_sigma_override": 0.0,   "logz_winsor_sigma_override": 0.0,  "shrinkage_scale_min": 0.0,      "note": "static scaler, not applicable -- no winsorization/shrinkage override"},
      {"index": 6,  "name": "hurst_exponent",     "dim_winsor_sigma_override": 345.0, "logz_winsor_sigma_override": 0.0,  "shrinkage_scale_min": 0.000145, "note": "GPD-derived, p=1/N return level, genuine Frechet tail confirmed via tail-conditional noise decomposition; shrinkage: exact-formula-derived"},
      {"index": 7,  "name": "micro_asymmetry",    "dim_winsor_sigma_override": 36.0,  "logz_winsor_sigma_override": 0.0,  "shrinkage_scale_min": 0.00733,  "note": "GPD-derived on corrected z, p=1/N return level; shrinkage: confirmed collapse signature, z=438 traced"},
      {"index": 8,  "name": "fisher_info",        "dim_winsor_sigma_override": 20.0,  "logz_winsor_sigma_override": 0.0,  "shrinkage_scale_min": 0.0,      "note": "GPD-derived, margin above a smaller-sample fit; audited 2026-08-14, clean, no shrinkage needed"},
      {"index": 9,  "name": "tail_index",         "dim_winsor_sigma_override": 10.0,  "logz_winsor_sigma_override": 0.0,  "shrinkage_scale_min": 0.0438,   "note": "GPD-derived on corrected z, Weibull (xi=-0.3259), just past the theoretical wall; shrinkage: confirmed collapse signature, z=1963 traced"},
      {"index": 10, "name": "skewness_idx",       "dim_winsor_sigma_override": 0.0,   "logz_winsor_sigma_override": 0.0,  "shrinkage_scale_min": 0.0,      "note": "audited, negligible clip rate, no action needed; shrinkage disabled per ruthless-simplicity (D8 precedent)"},
      {"index": 11, "name": "amihud_illiquidity", "dim_winsor_sigma_override": 0.0,   "logz_winsor_sigma_override": 0.0,  "shrinkage_scale_min": 0.0,      "note": "audited, 0.000% production clip rate, no action needed; has its own dedicated AMIHUD_ABSOLUTE_FLOOR fix instead of shrinkage"},
      {"index": 12, "name": "liq_fragility",      "dim_winsor_sigma_override": 0.0,   "logz_winsor_sigma_override": 12.0, "shrinkage_scale_min": 0.0,      "note": "LOGZ dim, GPD-derived, clean trace, real margin above the theoretical wall; shrinkage disabled -- resolved-healthy, no evidence of need"},
      {"index": 13, "name": "recurrence_rate",    "dim_winsor_sigma_override": 0.0,   "logz_winsor_sigma_override": 0.0,  "shrinkage_scale_min": 0.0,      "note": "static scaler, not applicable"},
      {"index": 14, "name": "fractal_dim",        "dim_winsor_sigma_override": 0.0,   "logz_winsor_sigma_override": 0.0,  "shrinkage_scale_min": 0.0,      "note": "static scaler, not applicable"},
      {"index": 15, "name": "mean_rev_z",         "dim_winsor_sigma_override": 0.0,   "logz_winsor_sigma_override": 0.0,  "shrinkage_scale_min": 0.0,      "note": "audited, 0.028% clip rate, only 14 exceedances, negligible, no action needed"}
    ]
  }
}' config/execution_params.json > /tmp/execution_params_merged.json && mv /tmp/execution_params_merged.json config/execution_params.json
```

- [x] **Step 3: Verify the merge preserved every pre-existing key untouched**

```bash
diff <(jq 'del(._version, ._owner, ._generated_by, .featurescaler_winsorization)' config/execution_params.json) \
     <(jq '.' /mnt/c/Trading/config/execution_params.json)
```
Expected: no output (the two are structurally identical once the new keys are removed from the merged copy — confirms nothing pre-existing was altered). Note `_version` intentionally differs (`1.0.2` → `1.1.0`), which is why it's excluded from this comparison.

- [x] **Step 4: Create `config/hmm_regime_risk_policy.json`** (full literal content — converts `policy_version` to the unified semver scheme, adds `_owner`/`_generated_by` at root and to the `empirical_gate_thresholds` section; every other key/value carried forward byte-identical from the live file)

```json
{
  "policy_id": "hmm_regime_risk_policy_prod",
  "policy_version": "1.0.0",
  "_owner": "manual/MindfulTrader",
  "_generated_by": "manual/MindfulTrader",
  "_version_scheme_note": "Converted from date+v-suffix scheme (was '2026-06-11.v3') to unified semver 2026-08-15, per docs/superpowers/specs/2026-08-15-risk-gate-context-cpp-coevolution.md Unit C. Starting at 1.0.0 -- no direct numeric predecessor to increment from.",
  "state_weights": { "COILED_SPRING": 1.00, "GAUSSIAN_STABLE": 1.20, "GAUSSIAN_FRAGILE": 0.90, "PARETO_MOMENTUM": 0.75 },
  "entropy_penalty": 0.50,
  "transition_penalty": 0.25,
  "confidence_floor": 0.20,
  "transition_floor": 0.70,
  "min_multiplier": 0.00,
  "max_multiplier": 1.50,
  "empirical_gate_thresholds": {
    "_owner": "lbrnet/calibrate_hmm_gate_thresholds.py",
    "_generated_by": "lbrnet/calibrate_hmm_gate_thresholds.py",
    "generated_at_utc": "2026-04-15T13:01:33Z",
    "pareto_top_state_ratio_max": 0.5,
    "shannon_min_tenure_bars": 143.5184571838369,
    "taleb_signal_sigma_threshold": 1.8401,
    "_taleb_signal_sigma_threshold_note": "percentile-matched to Moors-kurtosis scale, rescaled 2026-08-13 (was 9.697616023284109). Compiled C++ default synced to this exact value in src/RiskManager.cpp:74 per docs/superpowers/plans/2026-08-15-risk-gate-audit-unit-b.md Task 2.",
    "window_count_used": 34
  }
}
```

- [x] **Step 5: Commit**

```bash
git add config/execution_params.json config/hmm_regime_risk_policy.json
git commit -m "feat: add git-tracked config/ folder — shared, versioned, sectioned-ownership calibration config (Unit C)"
```

---

### Task 2: Make `FeatureScaler.h` load winsorization bounds from config at startup

**Files:**
- Modify: `include/FeatureScaler.h` (constants at lines 169, 231, 270, 400 per current file state — re-read the file first, line numbers may have shifted since this plan was written)
- Modify: `include/ContextManager.h:285` (constructor declaration)
- Modify: `src/ContextManager.cpp` (constructor definition, add `#include "FeatureScaler.h"` if not already present via another include)
- Test: `tests/cpp/test_feature_scaler.cpp` (extend with new config-load tests)

**Interfaces:**
- Consumes: `config/execution_params.json`'s `featurescaler_winsorization` section (Task 1).
- Produces: `FeatureScaler::LoadConfig(const std::string& path = kConfigPathWindows)`, `FeatureScaler::configLoadStatus` (enum `ConfigLoadStatus { DEFAULTS, LOADED_FROM_FILE, FILE_MISSING, PARSE_FAILED }`) — callable by any future config-status diagnostics; not currently consumed elsewhere.

- [x] **Step 1: Convert the four `constexpr` members to loadable `static` members**

In `include/FeatureScaler.h`, change:
```cpp
static constexpr float STATE_WINSOR_SIGMA = 6.0f;           ///< 6-sigma winsorization before Soft-Log map (default; see DIM_WIDE_WINSOR_INDEX)
```
to:
```cpp
static inline float STATE_WINSOR_SIGMA = 6.0f;              ///< 6-sigma winsorization before Soft-Log map (default; see DIM_WIDE_WINSOR_INDEX). Compiled default; overwritten in place by LoadConfig() if config/execution_params.json is present.
```
Change (line ~169, `SHRINKAGE_SCALE_MIN`):
```cpp
inline static constexpr std::array<float, N_DIMS> SHRINKAGE_SCALE_MIN = {
```
to:
```cpp
inline static std::array<float, N_DIMS> SHRINKAGE_SCALE_MIN = {  // compiled defaults; overwritten in place by LoadConfig()
```
(keep every existing per-dim value/comment inside the braces unchanged). Apply the identical `constexpr` → (drop, keep `inline static`) change to `LOGZ_WINSOR_SIGMA_OVERRIDE` (line ~270) and `DIM_WINSOR_SIGMA_OVERRIDE` (line ~400) — keep every existing value/comment.

- [x] **Step 2: Add the config-load method and status enum**

Add includes near the top of `include/FeatureScaler.h` (after the existing `#include` block):
```cpp
#include <fstream>
#include <string>
#include <nlohmann/json.hpp>
#include "Logger.h"
```
Add inside the `FeatureScaler` struct, near the four constants:
```cpp
    enum class ConfigLoadStatus : uint8_t { DEFAULTS, LOADED_FROM_FILE, FILE_MISSING, PARSE_FAILED };
    static inline ConfigLoadStatus configLoadStatus = ConfigLoadStatus::DEFAULTS;
    static constexpr const char* kConfigPathWindows = "C:/Trading/config/execution_params.json";

    /// Loads STATE_WINSOR_SIGMA/DIM_WINSOR_SIGMA_OVERRIDE/LOGZ_WINSOR_SIGMA_OVERRIDE/
    /// SHRINKAGE_SCALE_MIN from config/execution_params.json's "featurescaler_winsorization"
    /// section, called exactly once from ContextManager's constructor (never per-tick).
    /// Fail-closed: parses into locals first and only commits on full success, so a
    /// mid-parse exception never leaves the live static arrays partially overwritten;
    /// on any failure the compiled defaults above remain in effect and the failure is
    /// loudly logged (never silent), matching this repo's established config-load convention
    /// (ExecutionParams::LoadFromFile, RiskManager.cpp's GetHMMRiskPolicy).
    static void LoadConfig(const std::string& path = kConfigPathWindows) {
        std::ifstream in(path);
        if (!in.is_open()) {
            configLoadStatus = ConfigLoadStatus::FILE_MISSING;
            Logger::getInstance().log(
                "FeatureScaler: config FILE_MISSING at " + path + " -- using compiled defaults");
            return;
        }
        try {
            const std::string payload((std::istreambuf_iterator<char>(in)),
                                       std::istreambuf_iterator<char>());
            nlohmann::json j = nlohmann::json::parse(payload);

            if (!j.contains("featurescaler_winsorization") ||
                !j["featurescaler_winsorization"].is_object()) {
                configLoadStatus = ConfigLoadStatus::PARSE_FAILED;
                Logger::getInstance().log(
                    "FeatureScaler: 'featurescaler_winsorization' section missing at " + path +
                    " -- using compiled defaults");
                return;
            }
            const auto& fw = j["featurescaler_winsorization"];

            float newStateWinsorSigma = STATE_WINSOR_SIGMA;
            std::array<float, N_DIMS> newDimWinsor = DIM_WINSOR_SIGMA_OVERRIDE;
            std::array<float, N_DIMS> newLogzWinsor = LOGZ_WINSOR_SIGMA_OVERRIDE;
            std::array<float, N_DIMS> newShrinkageMin = SHRINKAGE_SCALE_MIN;

            newStateWinsorSigma = fw.value("state_winsor_sigma", newStateWinsorSigma);
            if (fw.contains("dims") && fw["dims"].is_array()) {
                for (const auto& d : fw["dims"]) {
                    const size_t idx = d.value("index", N_DIMS);
                    if (idx >= N_DIMS) { continue; }
                    newDimWinsor[idx] = d.value("dim_winsor_sigma_override", newDimWinsor[idx]);
                    newLogzWinsor[idx] = d.value("logz_winsor_sigma_override", newLogzWinsor[idx]);
                    newShrinkageMin[idx] = d.value("shrinkage_scale_min", newShrinkageMin[idx]);
                }
            }

            STATE_WINSOR_SIGMA = newStateWinsorSigma;
            DIM_WINSOR_SIGMA_OVERRIDE = newDimWinsor;
            LOGZ_WINSOR_SIGMA_OVERRIDE = newLogzWinsor;
            SHRINKAGE_SCALE_MIN = newShrinkageMin;
            configLoadStatus = ConfigLoadStatus::LOADED_FROM_FILE;
        } catch (const std::exception& e) {
            configLoadStatus = ConfigLoadStatus::PARSE_FAILED;
            Logger::getInstance().log(
                std::string("FeatureScaler: config PARSE_FAILED at ") + path + " (" + e.what() +
                ") -- using compiled defaults");
        }
    }
```

- [x] **Step 3: Wire `ContextManager`'s constructor to call `LoadConfig()` exactly once**

In `include/ContextManager.h:285`, change:
```cpp
    ContextManager() = default;
```
to:
```cpp
    ContextManager();
```
In `src/ContextManager.cpp`, add the out-of-line definition near the `Instance()` definition (this file already includes `FeatureScaler.h` transitively via `ContextManager.h`'s `m_featureScaler` member, so no new include is needed — confirm this by checking the top of `ContextManager.cpp` for `#include "FeatureScaler.h"` or `#include "ContextManager.h"` and add the former explicitly if it is not already pulled in):
```cpp
ContextManager::ContextManager() {
    FeatureScaler::LoadConfig();
}
```

- [x] **Step 4: Write the failing test first (extend `tests/cpp/test_feature_scaler.cpp`)**

Add a new bracketed test block, following the file's existing hand-rolled `check()`/`approx()` style. This needs a temporary JSON fixture file on disk — write it via `std::ofstream` at the top of the block so the test is self-contained:
```cpp
{
    // FeatureScaler::LoadConfig() overrides compiled defaults from a config file.
    const std::string path = "/tmp/test_featurescaler_config.json";
    {
        std::ofstream out(path);
        out << R"({
  "featurescaler_winsorization": {
    "state_winsor_sigma": 7.5,
    "dims": [
      {"index": 9, "dim_winsor_sigma_override": 99.0, "logz_winsor_sigma_override": 0.0, "shrinkage_scale_min": 0.0438}
    ]
  }
})";
    }
    FeatureScaler::LoadConfig(path);
    check("config_load_status_is_loaded_from_file",
          FeatureScaler::configLoadStatus == FeatureScaler::ConfigLoadStatus::LOADED_FROM_FILE);
    check("config_overrides_state_winsor_sigma", FeatureScaler::STATE_WINSOR_SIGMA == 7.5f);
    check("config_overrides_dim9_winsor_override",
          FeatureScaler::DIM_WINSOR_SIGMA_OVERRIDE[9] == 99.0f);
    check("config_leaves_untouched_dims_at_compiled_default",
          FeatureScaler::DIM_WINSOR_SIGMA_OVERRIDE[0] == 262.0f);
    std::remove(path.c_str());
}
{
    // FeatureScaler::LoadConfig() falls back to compiled defaults, loudly, on a missing file.
    FeatureScaler::LoadConfig("/tmp/does_not_exist_featurescaler_config.json");
    check("config_load_status_is_file_missing",
          FeatureScaler::configLoadStatus == FeatureScaler::ConfigLoadStatus::FILE_MISSING);
    check("missing_config_keeps_compiled_default_state_winsor_sigma",
          FeatureScaler::STATE_WINSOR_SIGMA == 6.0f);
}
```
Add `#include <fstream>` and `#include <cstdio>` to the test file's includes if not already present.

- [x] **Step 5: Run the test to verify it fails (before Steps 1-3 land) or passes (after)**

Since Steps 1-3 (implementation) and Step 4 (test) are being written together in this task, run after all of Steps 1-4 are in place:
```bash
g++ -std=c++17 -I include -I include/generated -I /mnt/c/Users/rcruz/vcpkg/installed/x64-windows/include tests/cpp/test_feature_scaler.cpp -o /tmp/fs_test && /tmp/fs_test
```
Expected: all checks pass, including the two new ones and all pre-existing ones (the pre-existing `FeatureScaler::DIM_WINSOR_SIGMA_OVERRIDE[9] == 10.0f` check at line ~387 runs *before* `LoadConfig()` is ever called in `main()`'s execution order — confirm by placing the new config-load test block at the *end* of `main()`, after every pre-existing block, so it does not mutate shared static state out from under earlier assertions).
Note the extended `-I` flag versus the file's original documented command — `nlohmann/json.hpp` is a vcpkg dependency, only available via that path; update the file's own header-comment build command to include it, since Step 4 introduces the first `test_feature_scaler.cpp` dependency on this library.

- [x] **Step 6: Full build verification**

```bash
./build_dll.sh --no-clean
```
Expected: build succeeds.

- [x] **Step 7: Commit**

```bash
git add include/FeatureScaler.h include/ContextManager.h src/ContextManager.cpp tests/cpp/test_feature_scaler.cpp
git commit -m "feat: FeatureScaler winsorization bounds load from config/execution_params.json at startup"
```

---

### Task 3: New script — push `config/*.json` to the live deployment path

**Files:**
- Create: `scripts/promote_config_to_live.py`
- Test: `tests/python/test_promote_config_to_live.py`

**Interfaces:**
- Consumes: `config/execution_params.json`, `config/hmm_regime_risk_policy.json` (Task 1).
- Produces: overwrites `/mnt/c/Trading/config/execution_params.json` and `/mnt/c/Trading/config/hmm_regime_risk_policy.json` when run — atomic write + timestamped backup, full-file replace (this is the one-directional git→live promotion path; it is not a sectioned read-modify-write, since the git-tracked file is already the fully-assembled canonical content by the time this script runs).

- [x] **Step 1: Write the script**

```python
#!/usr/bin/env python3
"""Push MindfulTrader/config/*.json to the live Sierra Chart config path.

config/ (this repo, git-tracked) is the source of truth; /mnt/c/Trading/config/
is a deployment target. Run manually after editing config/*.json — not wired
into any build step, matching this repo's other un-wired scripts/ entries
(check_nh_nl_freshness.py, refresh_sierra_chart_dependencies.sh).
"""
import argparse
import json
import os
import shutil
import sys
from datetime import datetime, timezone
from pathlib import Path

REPO_CONFIG_DIR = Path(__file__).resolve().parent.parent / "config"
LIVE_CONFIG_DIR = Path("/mnt/c/Trading/config")
CONFIG_FILES = ["execution_params.json", "hmm_regime_risk_policy.json"]


def _utc_stamp() -> str:
    return datetime.now(timezone.utc).strftime("%Y%m%dT%H%M%SZ")


def _backup_file(path: Path) -> Path:
    backup = path.with_suffix(path.suffix + f".bak.{_utc_stamp()}")
    shutil.copy2(path, backup)
    fd = os.open(backup, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)
    return backup


def _write_json_atomic(path: Path, payload: dict) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    tmp = path.with_suffix(path.suffix + f".tmp.{_utc_stamp()}")
    tmp.write_text(json.dumps(payload, indent=2) + "\n", encoding="utf-8")

    fd = os.open(tmp, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)

    os.replace(tmp, path)
    dir_fd = os.open(path.parent, os.O_RDONLY)
    try:
        os.fsync(dir_fd)
    finally:
        os.close(dir_fd)


def promote(repo_dir: Path, live_dir: Path) -> list[Path]:
    """Push every file in CONFIG_FILES from repo_dir to live_dir. Returns backups created."""
    backups = []
    for name in CONFIG_FILES:
        src = repo_dir / name
        dst = live_dir / name
        payload = json.loads(src.read_text(encoding="utf-8"))
        if dst.exists():
            backups.append(_backup_file(dst))
        _write_json_atomic(dst, payload)
        print(f"Promoted {src} -> {dst}")
    return backups


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--repo-dir", type=Path, default=REPO_CONFIG_DIR)
    parser.add_argument("--live-dir", type=Path, default=LIVE_CONFIG_DIR)
    args = parser.parse_args()
    promote(args.repo_dir, args.live_dir)
    return 0


if __name__ == "__main__":
    sys.exit(main())
```

- [x] **Step 2: Write the failing test first**

```python
"""Test scripts/promote_config_to_live.py's atomic-write + backup behavior."""
import json
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent.parent.parent / "scripts"))
import promote_config_to_live as promote_mod  # noqa: E402


def test_promote_writes_content_byte_for_byte(tmp_path):
    repo_dir = tmp_path / "config"
    live_dir = tmp_path / "live"
    repo_dir.mkdir()
    live_dir.mkdir()
    payload = {"_version": "1.1.0", "key": "value"}
    (repo_dir / "execution_params.json").write_text(json.dumps(payload))
    (repo_dir / "hmm_regime_risk_policy.json").write_text(json.dumps({"policy_version": "1.0.0"}))

    promote_mod.promote(repo_dir, live_dir)

    written = json.loads((live_dir / "execution_params.json").read_text())
    assert written == payload


def test_promote_backs_up_existing_live_file(tmp_path):
    repo_dir = tmp_path / "config"
    live_dir = tmp_path / "live"
    repo_dir.mkdir()
    live_dir.mkdir()
    (repo_dir / "execution_params.json").write_text(json.dumps({"_version": "1.1.0"}))
    (repo_dir / "hmm_regime_risk_policy.json").write_text(json.dumps({"policy_version": "1.0.0"}))
    (live_dir / "execution_params.json").write_text(json.dumps({"_version": "old"}))
    (live_dir / "hmm_regime_risk_policy.json").write_text(json.dumps({"policy_version": "old"}))

    backups = promote_mod.promote(repo_dir, live_dir)

    assert len(backups) == 2
    for backup in backups:
        assert backup.exists()
        assert ".bak." in backup.name


if __name__ == "__main__":
    import pytest
    sys.exit(pytest.main([__file__, "-v"]))
```

- [x] **Step 3: Run test to verify it fails**

```bash
python3 -m pytest tests/python/test_promote_config_to_live.py -v
```
Expected: FAIL — `promote_config_to_live` module not found (script doesn't exist yet if Steps are run in strict order; if Step 1 already landed, skip to Step 4 — TDD ordering here is Step 1 written, then test added and confirmed to pass against it, since this is a self-contained script with no separate red/green split worth enforcing beyond "test exists and is meaningful").

- [x] **Step 4: Run test to verify it passes**

```bash
python3 -m pytest tests/python/test_promote_config_to_live.py -v
```
Expected: 2 passed.

- [x] **Step 5: Commit**

```bash
git add scripts/promote_config_to_live.py tests/python/test_promote_config_to_live.py
git commit -m "feat: add script to push git-tracked config/ to live Sierra Chart config path"
```

---

## Self-Review Notes (for whoever executes this plan)

- **Spec coverage:** Design points 1 (new `config/` folder) → Task 1. Point 2 (unified versioning) → Task 1 Steps 2/4. Point 3 (FeatureScaler section with provenance) → Task 1 Step 2. Point 4 (sectioned `_owner`/`_generated_by`) → Task 1 Steps 2/4. Point 5 (lbrnet-side sync script) → explicitly out of scope, Global Constraints. Point 6 (push script) → Task 3. Point 7 (loud-log, no CI gate) → Task 2 Step 2's `LoadConfig()`.
- **Task ordering:** Task 1 must land before Task 2 (Task 2's test fixture mirrors Task 1's JSON shape, and `FeatureScaler::LoadConfig()`'s default path assumes the config file will eventually exist at the live path via Task 3). Task 3 depends on Task 1's files existing but not on Task 2. Task 2 and Task 3 are otherwise independent of each other and can run in parallel across two subagents if desired.
- **Type/name consistency check performed:** `ConfigLoadStatus` (Task 2) is a new, separate enum from `ExecutionParams::LoadStatus`/`HMMRiskPolicy::LoadStatus` (different classes, same naming convention, deliberately not unified into one shared type — each config-bearing class owns its own status enum in the existing codebase, no change to that pattern here). `featurescaler_winsorization.dims[].index` matches `FeatureScaler::N_DIMS` (16) exactly — Task 1's JSON has 16 entries, Task 2's loader guards `idx >= N_DIMS`.
- **Known accepted gap:** `tools/rqa_recurrence_calibration.cpp:180`'s hardcoded `6.0`/`-6.0` literal duplicate is not wired to the new config and is explicitly left alone (Global Constraints) — do not "helpfully" fix it as part of this plan; it's out of scope and not requested.
