# tools/ — Standalone Analysis Tools

Standalone, natively-tested C++/Python utilities for data ingestion, calibration, and
diagnostics — **never** added to `CMakeLists.txt`/`build_dll.sh`. See `CLAUDE.md`/
`.github/copilot-instructions.md` for the top-level directives this section assumes:

- Build via bare `mamba run -n mts g++ -std=c++17 ...` (Arrow/Parquet tools also need
  `$(mamba run -n mts pkg-config --cflags/--libs arrow parquet)` plus
  `-Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib`). Never wire these into the DLL build.
- Compiled binaries go in `tools/bin/` (gitignored) — never next to source.
- Every tool that can run more than a few seconds must route all output through
  `ToolProgressLogger::Log()` (never bare `printf`/`puts`) — auto-archives to
  `tools/output/<toolName>_<timestamp>.txt` (gitignored, never overwritten) and appends a row
  to `tools/RECALIBRATION_LEDGER.md` (git-tracked).
- **Check `tools/RECALIBRATION_LEDGER.md`'s PENDING REVIEW rows before launching a new heavy
  recalibration pass** — a prior run may already answer the question.
- Native tests use a `check(name, bool)` + `g_failures` + final `ALL PASS`/`N FAILURE(S)`
  harness — no GoogleTest/CMake.

## Subfolders

### `scid_processing/` — `.scid` tick decode, mirror-sync, parquet export

The real-tick data-ingestion pipeline: decodes Sierra Chart's raw `.scid` binary tick format,
mirrors `.scid` files from the live Sierra Chart directory into a local copy, and writes the
canonical `lbrnet/data/raw/mes_ticks.parquet` (true per-tick data, no aggregation).

- `scid_reader.h` — raw `.scid` record decode
- `scid_contract_windows.h` — derives each MES contract's active date-window from filenames
- `scid_mirror_sync.h` — syncs `.scid` files from `/mnt/c/SierraChart2/Data` → `lbrnet/data/scid/`
  (new/grown/shrunk-file handling, atomic rename, rollover trim; read-only against the live dir)
- `scid_part_action.h` — per-contract incremental decode state (resume vs. full rebuild)
- `scid_to_ticks_parquet.cpp` — the CLI tying it together. Defaults:
  `--data-dir lbrnet/data/scid`, `--data-dir-live /mnt/c/SierraChart2/Data`,
  `--output lbrnet/data/raw/mes_ticks.parquet`. Flags: `--sync-only`, `--no-sync`,
  `--full-rebuild`. Default (no flags) = sync mirror, then incremental decode.
- `test_scid_*.cpp` — native tests for the above
- Design reference: `docs/superpowers/specs/2026-09-02-scid-tick-parquet-cpp-tool-spec.md`

### `context_pipeline/` — `.context` training-cache file I/O

- `context_reader.h` / `context_cache_key.h` — read/parse `.context` files, schema-version gating
- `context_to_parquet.cpp` — converts `.context` files to parquet
- `context_validate.cpp` / `context_validate_stats.h` — validation/sanity-check pass over a
  `.context` file
- `test_context_*.cpp` — native tests

### `market_data_replay/` — offline `.context`/`.alpha` generators + dim-selection exporter (no Sierra Chart needed)

Reconstructs TS1/TS2/TS3 bars, the full 18D observation vector, and (since the
2026-09-16 `.alpha` initiative) all 17 `PRIMARY_TRIGGER_MASK` pattern-detector
indicators directly from `lbrnet/data/raw/mes_ticks.parquet` — built so
training-cache generation doesn't require a live/replay Sierra Chart session.
**Two separate CLI entry points**, not one — do not conflate them:

- `MarketDataReplayEngine.h` — the shared reconstruction engine (tick aggregation → observation
  dims → 17 pattern detectors → trigger gates), reused unmodified by both CLIs below.
- `CandidateObservationDims.h` / `CandidateTriggerGate.h` — dimension/gate definitions
- `ContextFileWriter.h` / `AlphaFileWriter.h` — standalone, byte-faithful `.context`/`.alpha`
  writers (can't link `LBRFileManager.cpp` directly — pulls in `windows.h` transitively)
- `MarketDataReplayContext.cpp` — CLI driver for real, byte-compatible `.context`+`.alpha` file
  pairs (two independent triggers: Mahalanobis gate for `.context`, `PRIMARY_TRIGGER_MASK`
  dirty-bit + Locks A/B/D/E for `.alpha` — never unified). This is the tool that replaces a real
  Sierra-Chart-collected `.context`/`.alpha` pair.
- `MarketDataReplay.cpp` — a SEPARATE CLI driver for the dim-selection research pipeline
  (`docs/superpowers/specs/2026-09-09-market-data-replay-dim-selection-spec.md`): streams the same
  engine's output directly to flat Parquet (all 18 dims as columns, one row per Mahalanobis-gated
  tick) for `lbrnet`'s dim-IN/OUT calibration work — NOT a `.context`/`.alpha` writer, does not use
  `ContextFileWriter.h`/`AlphaFileWriter.h` at all (this reflects a real 2026-09-09 pivot away from
  `.context` output for that specific research use case; corrected here 2026-09-17 after this
  entry was found stale, still describing `MarketDataReplay.cpp` as "the CLI driver" singular).
- `test_*.cpp` — native tests
- Design reference (`.context`/`.alpha`): `docs/superpowers/specs/2026-09-16-market-data-replay-alpha-generator-spec.md`,
  plan: `docs/superpowers/plans/2026-09-16-market-data-replay-alpha-generator-implementation.md`
- Design reference (original `.context`-only generator, superseded by the spec above):
  `docs/superpowers/specs/2026-09-08-offline-context-generator-spec.md`,
  plan: `docs/superpowers/plans/2026-09-08-market-data-replay-implementation.md`
- Design reference (dim-selection Parquet exporter): `docs/superpowers/specs/2026-09-09-market-data-replay-dim-selection-spec.md`


### `observation_vector/` — HMM observation-vector dimension calibration/eval

The largest and most heterogeneous subfolder — grouped here by function, not alphabetically:

**Evals** (read-only measurement/validation against real tick data, not calibration):
`FeatureSaliencyEval.cpp`, `ImbalanceEntropyDivergenceEval.cpp`, `drift_location_eval.cpp`,
`imbalance_clock_manager_ratio_eval.cpp`, `imbalance_screen1_hurst_eval.cpp`,
`imbalance_work_rate_eval.cpp`, `jump_ratio_eval.cpp`, `relative_range_reformulation_eval.cpp`,
`volatility_dim_redundancy_eval.cpp`, `whole_vector_redundancy_eval.cpp`, `dim_acceptance_eval.py`

**Recalibration** (derives/updates production threshold constants):
`observation_vector_recalibration.cpp`, `amihud_liqfragility_recalibration.cpp`/`.py`,
`burstiness_recalibration.cpp`, `rqa_recurrence_calibration.cpp`,
`fractal_dim_threshold_migration.cpp`/`.py`, `mean_rev_z_variant_comparison.cpp`/`.py`

**Diagnostics / Monte Carlo** (methodology validation, not per-dim calibration):
`activity_clock_bv_comparison.cpp`, `analyze_kurtosis_threshold_migration.py`,
`block_length_and_variance_inflation.py`, `build_paired_kurtosis_sample.py`,
`dfa_bias_montecarlo.py`, `dfa_vs_mfdfa_q1_montecarlo.py`, `hill_intraday_seasonality.cpp`/`.py`,
`window_autocorrelation_diagnostic.py`

**Shared headers** (stats/IO helpers, no `main()`): `FeatureSaliencyEM.h`,
`drift_location_stats.h`, `jump_ratio_stats.h`, `market_test_stats.h`,
`streaming_correlation_matrix.h`, `volatility_dim_stats.h`, `market_data_io.h`

**Tests**: `test_drift_location_stats.cpp`, `test_feature_saliency_em.cpp`,
`test_jump_ratio_stats.cpp`, `test_market_test_stats_median.cpp`,
`test_streaming_correlation_matrix.cpp`, `test_volatility_dim_stats.cpp`

Design references: `docs/superpowers/specs/2026-08-12-gang-literature-grounding-spec.md` (the
living per-dim literature-grounding reference), `docs/superpowers/specs/2026-08-31-elite-feature-set-curation-initiative.md`, `docs/superpowers/specs/2026-09-07-feature-saliency-em-fitter-spec.md`.

### `visualization/` — Python chart-comparison scripts

`imbalance_vs_time_bars.py` (real imbalance-vs-time-bar chart comparisons on real MES ticks),
`imbalance_triple_screen_first_6mo.py`. See
`docs/superpowers/specs/2026-09-04-technical-analysis-gang-statistical-reformulation-initiative.md`.
