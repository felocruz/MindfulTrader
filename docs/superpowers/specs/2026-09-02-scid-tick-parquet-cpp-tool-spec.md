# C++ Tick-Level `.scid` Decoder + Mirror Sync (replaces `mes_continuous.py`)

> Status: **SPEC, not yet implemented.** Written from `MindfulTrader`, 2026-09-02.

## 1. Problem statement

`lbrnet/data/raw/mes_continuous_ticks.parquet` is named "ticks" but is not tick data — it is
a front-month-rolled, **1-second-bar** aggregation, built by `lbrnet/data/mes_continuous.py`'s
`decode_scid_ticks()` + `aggregate_to_1s_bars()` (invoked via
`lbrnet/scripts/build_mes_continuous_bars.py`). This is documented in `lbrnet`'s own
`docs/superpowers/specs/2026-08-08-mes-continuous-bars-design.md` and independently
re-flagged as a "red herring" filename in `lbrnet/scratchpad.md:4485`. The predecessor
`lbrnet/scripts/preprocess_scid_data.py` (pre-Denali legacy, zero live callers — verified,
only comment references remain) also never produced true tick output; it deduped by
`unique(timestamp_us)` but was superseded before this gap was ever addressed.

Verified directly against real data (this session, both `/mnt/c/SierraChart2/Data/MES[HMUZ]##-CME.scid`
and its byte-identical local mirror `lbrnet/data/scid/`, MD5-matched on first 50MB of a sampled
file): raw `.scid` records have genuine per-trade granularity — `NumTrades == 1` on every record,
timestamp deltas down to 1µs, fully irregular spacing. The existing `mes_continuous_ticks.parquet`
loses this: its `timestamp_us` deltas are exclusively multiples of 1,000,000µs (1-second bar
boundaries). Anything currently reading that file for "tick-accurate" analysis (e.g.
`docs/ADR/phase2_triple_barrier_migration_spec.md`, `tools/observation_vector/drift_location_eval.cpp`,
`build_directional_alpha.py`'s `PolarsTickReader`) is working from second-resolution bars, not
ticks, despite the filename and multiple docs' own "tick-accurate" claims.

**Primary consumer driving this migration**: `MindfulTrader/tools/` already hosts a family of
offline evaluation/calibration scripts for this system's 16D observation-vector dimensions
feeding the Student-t HMM, all currently reading `mes_continuous_ticks.parquet` as their real
MES market-data source — `drift_location_eval.cpp`, `burstiness_recalibration.cpp`,
`mean_rev_z_variant_comparison.cpp`/`.py`, `window_autocorrelation_diagnostic.py`,
`amihud_liqfragility_recalibration.py`, `dim_acceptance_eval.py`. Several of these already flag
the 1-second aggregation as a known analysis limitation in their own comments (e.g.
`mean_rev_z_variant_comparison.cpp:19`: "bars from mes_continuous_ticks.parquet's 1-second
aggregated..."; `window_autocorrelation_diagnostic.py:23`: "re-resampled from the 1-second
mes_continuous_ticks.parquet"). These tools are the concrete, near-term beneficiaries of true
tick-level data — several observation-vector dims already under literature-grounded
"activity-clock" treatment (per `CLAUDE.md`'s North Star notes: `fast_taleb_kurtosis`,
`skewness_idx` shipped; `mean_rev_z`/`hurst_exponent`/`recurrence_rate` decided, not yet
implemented) fundamentally require per-tick event-time indexing, not fixed 1-second clock time,
to be evaluated correctly at all.

**Also a likely `lbrnet`-side consumer, not just `MindfulTrader/tools/`**: `lbrnet`'s own
labeling and backtesting code reads the same file today — `lbrnet/scripts/build_directional_alpha.py`
(training-label generation via `PolarsTickReader`, explicitly for "lossless, tick-accurate
lookahead scans", per `GEMINI.md:296`), `lbrnet/scripts/build_turtle_soup_dataset.py`,
`lbrnet/data/multiscale_bars.py` (derives its 15m/60m/240m bars from this as the "rawest
available price source"), and `backtest/generate_oracle.py` /
`backtest/run_phase2.py` / `backtest/run_phase2_verdict.py`. These are out of scope to edit
from this repo (see §8's handoff note) but are named here since they are real, existing
consumers whose "tick-accurate" claims are currently false for the same reason as the
`MindfulTrader/tools/` consumers above.

This spec covers building a native C++ tool, `tools/scid_processing/scid_to_ticks_parquet.cpp`, that:
1. Emits one row per raw `.scid` trade record — no aggregation of any kind.
2. Refreshes the local `lbrnet/data/scid/` mirror from the live Sierra Chart directory
   (`/mnt/c/SierraChart2/Data`) when needed, incrementally.
3. Once implemented and verified, retires the Python decode/aggregate/sync code path this
   spec supersedes (§8).


## 2. Reused algorithm (ported from `lbrnet/data/mes_continuous.py`, not reinvented)

The existing Python module already solves every non-aggregation part of this problem
correctly and has empirically-verified edge-case handling. Port these pieces to C++
byte-for-byte equivalent, do not redesign them:

- **Contract discovery & front-month windowing** (`discover_contracts()`): filenames
  matching `MES[HMUZ]\d{2}-CME\.scid`; each contract's active window ends
  `ROLLOVER_DAYS_BEFORE_EXPIRY=4` calendar days before the 3rd Friday of its contract
  month. **This is Sierra Chart's own configured rollover rule for this exact symbol, not an
  assumed generic CME convention** — `lbrnet`'s
  `docs/superpowers/specs/2026-08-08-mes-continuous-bars-design.md` traces it to two
  independent sources: the user directly reading Sierra Chart's own Symbol Settings tooltip
  for MES, and Sierra Chart's own `SymbolSettings.scdataallservices.xml` config file
  (`rollover-method=Method3`, `rollover-input-1=4`, `rollover-input-2=3` for `MES?##-CME`).
  Windows are validated non-overlapping and the quarterly H/M/U/Z cycle must have no gaps
  (raise on a missing contract, not silently produce a gap in the continuous series).
- **SC epoch conversion**: `raw_timestamp - SC_EPOCH_OFFSET_US` where
  `SC_EPOCH_OFFSET_US = 2209161600000000` (1899-12-30 → 1970-01-01 in µs) converts each
  record's `SCDateTimeMS` to POSIX microseconds.
  `s_IntradayRecord`/`s_IntradayFileHeader` layout is already available in this repo at
  `sierra_chart_dependencies/IntradayRecord.h` (`HEADER_SIZE=56`, 40-byte records, matching
  `SCID_DTYPE` in the Python module field-for-field).
- **Window slicing via binary search on the already-sorted timestamp stride**
  (`np.memmap` + `np.searchsorted` in the Python original): only the requested
  `[active_start_us, active_end_us)` window is decoded per contract, not the whole
  multi-year file.
- **Benign out-of-order jitter resequencing**: real exchange feed-handler jitter produces
  occasional backwards timestamp jumps under `_MAX_BENIGN_TIMESTAMP_JITTER_US = 1_000_000`
  (1 second) — verified empirically (8 backwards jumps out of 29.79M ticks on real
  `MESU26-CME.scid`, all under 1ms). Resequence (stable sort) jitter under this bound; raise
  a hard error for anything exceeding it, since that indicates corruption/misalignment, not
  benign reordering.
- **Ask/bid/spread derivation**: `ask_price = high`, `bid_price = low`, `spread = high - low`
  — empirically verified (not a guess) that `high`/`low` are the ask/bid price at the moment
  of each trade for this data, never a price range, across 50,000 sampled real records
  (`close == high` exactly when `ask_volume > 0 ∧ bid_volume == 0`, and symmetric for
  `low`/`bid_volume`). `trade_side = ask_volume > 0 ? 1 : (bid_volume > 0 ? -1 : 0)`.
- **Non-overlap + sanity guards**: reject a contract whose window starts at or before the
  previous contract's last timestamp; reject any derived negative spread.

## 3. What changes vs. the Python original (no aggregation)

`aggregate_to_1s_bars()` is dropped entirely — there is no bar concept in this tool's
output. Each decoded, resequenced tick is written as its own output row using the *tick*
schema below, not the *bar* schema (`BAR_COLUMN_ORDER`) `mes_continuous.py` currently
produces.

### Output schema — `lbrnet/data/raw/mes_ticks.parquet` (new filename, not overwriting the
existing bar file — see §8 for the sunset plan of the old name)

| Column | Type | Source |
|---|---|---|
| `timestamp_us` | int64 | `raw_timestamp - SC_EPOCH_OFFSET_US` |
| `trade_price` | float64 | `close` |
| `ask_price` | float64 | `high` |
| `bid_price` | float64 | `low` |
| `spread` | float64 | `high - low` |
| `volume` | int64 | `volume` |
| `bid_volume` | int64 | `bid_volume` |
| `ask_volume` | int64 | `ask_volume` |
| `trade_side` | int64 | derived, see §2 |
| `contract` | string | e.g. `"U26"`, stamped per contract window like the bar file's `contract` column |

This is exactly `decode_scid_ticks()`'s own in-memory column set (already tick-granular
internally, before `aggregate_to_1s_bars()` collapses it) plus the `contract` label the bar
path currently only attaches after aggregation.

## 4. DOD (data-oriented design) requirements — mandatory, not optional

Per this repo's established convention (`tools/context_to_parquet.cpp`'s SoA pattern, this
project's global hot-path discipline): **columnar (SoA) construction throughout, chunked
writes bounding peak memory regardless of file size.**

- One `arrow::Int64Builder`/`DoubleBuilder`/`StringBuilder` per output column — never a
  row-of-structs intermediate, matching `ChunkBuffers` in `context_to_parquet.cpp`.
- Decode via a **direct-mmap read view over the raw `.scid` bytes**, reinterpreting the
  40-byte record stride in place (equivalent to `mes_continuous.py`'s `np.memmap` +
  `SCID_DTYPE`), not a read-into-`std::vector<s_IntradayRecord>` copy — avoid paging in or
  copying bytes outside the requested `[start_us, end_us)` window at all. Use
  `std::lower_bound` (or hand-rolled binary search) over the mmap'd timestamp stride for the
  window-slice step — the direct C++ analog of `np.searchsorted` on the memmap. Never copy
  the contract's whole active window (already only ~1/8 of a multi-year file, per
  `mes_continuous.py`'s own measurement) into any `std::vector` before slicing.
- Fixed-size record batches (`kChunkRows`, matching `context_to_parquet.cpp`'s
  `2'000'000`-row convention) via `NewBufferedRowGroup()` + `WriteRecordBatch()` — process
  and flush one chunk at a time per contract; never accumulate a whole contract's ticks (up
  to ~30M+ per contract) in one builder before writing.
- **Chunk buffer reuse, not per-chunk reallocation**: the accumulation buffer passed across
  chunk boundaries must follow `context_to_parquet.cpp`'s exact `ChunkBuffers::Reserve(n)`/
  `Clear()` pattern — `Reserve(kChunkRows)` once, `Clear()` (not destroy-and-reconstruct)
  between chunks. A fresh heap-allocated chunk struct per callback invocation is exactly the
  allocation churn this pattern exists to avoid, and matters more here than in
  `context_to_parquet.cpp` given `.scid` contracts run ~30M+ ticks vs. that tool's typical
  input size.
- **`madvise()` sequential-access hint on the mmap'd window being decoded** — this codebase
  has no existing `madvise` usage, but contract files are 1-2GB and decoded as one long
  sequential scan per contract; call `madvise(MADV_SEQUENTIAL)` on the `[start_us, end_us)`
  byte range immediately after the binary-search slice is known (before the decode loop
  starts), and `madvise(MADV_DONTNEED)` on that same range once a contract's decode
  completes, so page cache pressure from one multi-GB contract doesn't linger while the next
  contract is processed.
- Final cross-contract merge is a **k-way streaming merge on already-sorted-per-contract
  parts**, not an in-memory `sort()` over the full combined series — each contract's part is
  already chronologically ordered and non-overlapping by construction (§2), so the merge is
  a linear concatenation in contract order, not a genuine multi-way sort. Do not load two
  contracts' full tick data into RAM simultaneously.
- No heap allocation inside the per-record derivation loop (`ask_price`/`bid_price`/
  `spread`/`trade_side` computation) — these are pure scalar transforms on primitive fields
  already resident in the mmap view; write directly into the chunk's builder columns.
- **Cross-contract parallel decode**: the ~13 discovered contract windows are fully
  independent (non-overlapping by construction, §2) and each writes its own part file — decode
  them concurrently, one `std::thread` per contract capped at
  `std::thread::hardware_concurrency()` (this codebase's own established threading primitive,
  used throughout `src/` for worker threads; there is no prior parallel-batch-processing
  precedent in `tools/`, but the primitive itself is not foreign). This composes cleanly with
  the chunked-memory discipline above: each thread's peak memory stays bounded to one
  `kChunkRows`-sized buffer, so N-way contract parallelism costs N times that small fixed
  footprint, not N times a contract's multi-GB file size. This is the single largest speed
  lever available, since wall-clock time is dominated by decoding ~13 independent multi-GB
  files sequentially otherwise.
  **Deliberately not done**: intra-contract per-record parallelism (splitting one contract's
  derivation loop across threads). That workload is I/O/page-fault-bound on one memory-mapped
  multi-GB file, not CPU-bound — the per-record math is a handful of scalar float ops: adding
  threads there would add real synchronization/complexity for no measurable win. Documented
  here as a considered-and-rejected option, not an oversight.
- **Zero-copy kernel-side mirror copy**: the mirror-sync copy step (§5) must use
  `std::filesystem::copy_file` (which on Linux glibc already dispatches to `copy_file_range`/
  `sendfile`, a kernel-side copy with no userspace buffer round-trip) rather than a hand-rolled
  read-into-buffer/write loop — the latter would silently double the memory traffic for every
  byte of a multi-GB contract file.
- **Explicit Parquet `WriterProperties`, not library defaults**: `context_to_parquet.cpp`
  opens its writer via `parquet::arrow::FileWriter::Open(*schema, arrow::default_memory_pool(),
  sink)` with no explicit properties, acceptable there given its output size. This tool's
  output is expected to be far larger (no aggregation, likely 10-30x more rows than the
  existing 38.5M-row bar file) — set an explicit `parquet::WriterProperties::Builder`
  specifying `zstd` compression at an explicit fast level (`compression_level(1)`, not the
  library's higher-ratio default) — this file is an incrementally-regenerated cache, not a
  cold-archival artifact, so write throughput matters more than ratio — and an explicit
  `parquet::ArrowWriterProperties` enabling dictionary encoding for the low-cardinality
  `contract` column (a handful of distinct contract labels across hundreds of millions of
  rows — dictionary encoding is a large, near-free win there). Row groups stay aligned to
  `kChunkRows` (one `WriteRecordBatch` per
  `NewBufferedRowGroup`), which also gives downstream readers row-group-level min/max
  `timestamp_us` statistics for free predicate pushdown on time-bounded reads, since output
  order is chronological by construction.

## 5. Mirror sync — `lbrnet/data/scid/` refresh from `/mnt/c/SierraChart2/Data`

Port the (currently unimplemented) design in `lbrnet`'s own
`docs/superpowers/specs/2026-08-18-mes-scid-incremental-sync-design.md` to C++, folded into
this same tool rather than a separate script (per this project's "reuse existing lifecycle,
don't create parallel paths" convention). Reuse its already-verified logic:

- For every `MES[HMUZ]\d{2}-CME\.scid` file discovered under the live directory:
  - Not present in the mirror → copy in (new contract, full decode later).
  - Present, `(size, mtime)` unchanged → skip (untouched).
  - Present, size **grew** → copy in (overwrite); this contract becomes a delta-decode
    candidate (resume from the mirror's own previous max `timestamp_us` + 1, not from
    scratch — reuses `decode_scid_ticks`'s arbitrary-`start_us` capability, ported per §2).
  - Present, size **shrank** or header bytes changed → do **not** treat as a delta; force a
    full re-decode of that contract's whole active window and log a warning (the shrink
    design's own explicit callout: this signals a re-exported/replaced source file, not an
    append).
- Copy is never in-place: write to `{name}.tmp` in the mirror directory, then atomically
  rename over the existing file (`std::filesystem::rename`, same filesystem — POSIX rename
  is atomic). Sierra Chart may be actively appending to the live file mid-copy; an
  interrupted in-place copy must never leave a torn file indistinguishable from real
  corruption.
- Rollover trim: when a **new** contract's window retroactively closes a sibling's
  previously-open (`active_end_us = None`) window, the sibling's mirror-side already-decoded
  part must be trimmed to the new boundary, not left overrun — same as the sync design's own
  edge case.
- Mirror sync is **read-only** against `/mnt/c/SierraChart2/Data` — never write to the live
  Sierra Chart directory.
- CLI surface: `--data-dir-live` (default `/mnt/c/SierraChart2/Data`, read-only) and
  `--data-dir` (default `lbrnet/data/scid`, the local mirror actually decoded from) as
  distinct flags, matching the sync design's own orchestration change.

## 6. CLI surface

```
tools/bin/scid_to_ticks_parquet \
  --data-dir lbrnet/data/scid \
  --data-dir-live /mnt/c/SierraChart2/Data \
  --output lbrnet/data/raw/mes_ticks.parquet \
  [--sync-only] [--no-sync] [--full-rebuild]
```

- `--sync-only`: run the mirror refresh (§5) and exit, no decode/write.
- `--no-sync`: decode directly from the existing mirror, skip the live-directory check
  entirely (useful when `/mnt/c/SierraChart2/Data` isn't mounted, e.g. native Linux CI).
- `--full-rebuild`: ignore any existing per-contract incremental state and reprocess every
  contract's whole active window from scratch.
- Default (no flags): sync, then incremental decode — the common case.

## 7. Testing

Follow this repo's native-test convention (see `tests/cpp/`), not a Python test:

- Contract discovery: quarterly cycle validation (raise on missing contract), non-overlap
  guard, correct roll-date math for a handful of known contract months.
- Record decode: byte-for-byte fixture (small synthetic `.scid` file, header + N records)
  decodes to the exact expected tick rows including `ask_price`/`bid_price`/`spread`/
  `trade_side` derivation.
- Benign jitter resequencing: synthetic fixture with a few sub-1s backwards jumps resolves
  to sorted output; a jump exceeding the bound raises.
- Mirror sync: new file copied in; unchanged file skipped; grown file triggers delta-decode
  resume from the correct offset; shrunk/header-changed file triggers full reprocess;
  simulated interrupted copy leaves the prior mirror file untouched.
- End-to-end: two small real contract fixtures decode to tick-for-tick identical output
  whether run as one full pass or as a sync + incremental-append across two invocations.

## 8. Sunset plan for the Python code this replaces

**Once the C++ tool is implemented, built, and its output verified against the existing
Python pipeline's decode step (tick-for-tick match on `decode_scid_ticks()`'s own columns,
pre-aggregation) — remove the superseded Python:**

- `lbrnet/scripts/preprocess_scid_data.py` — already dead (zero live callers, confirmed this
  session; only comment references remain). Delete outright, independent of this spec's
  completion.
- `lbrnet/data/mes_continuous.py` — delete once the C++ tool's tick-decode output is
  verified equivalent. Note: if any consumer still needs 1-second **bars** (not ticks) after
  this migration, that aggregation must be re-homed (e.g. a small downstream Polars
  `group_by_dynamic` step reading the new tick parquet, or a future C++ addition) before
  deleting `aggregate_to_1s_bars()` — do not delete bar-producing capability without first
  confirming no remaining consumer needs it.
- `lbrnet/scripts/build_mes_continuous_bars.py` — the orchestration CLI wrapper; delete
  alongside `mes_continuous.py`.
- `tests/test_mes_continuous.py` — delete or replace with the C++ tool's native tests
  (§7), whichever this repo's test-migration convention prefers; do not leave dangling
  tests importing a deleted module.
- `lbrnet`'s own `docs/superpowers/specs/2026-08-18-mes-scid-incremental-sync-design.md` —
  mark superseded by this spec (its Python-side sync design is subsumed by §5 here); do not
  implement it in Python.
- `lbrnet`'s labeling/backtesting consumers of `mes_continuous_ticks.parquet` —
  `lbrnet/scripts/build_directional_alpha.py`, `lbrnet/scripts/build_turtle_soup_dataset.py`,
  `lbrnet/data/multiscale_bars.py`, `backtest/generate_oracle.py`, `backtest/run_phase2.py`,
  `backtest/run_phase2_verdict.py` — must be repointed to the new tick file by a `lbrnet`-side
  session as part of the same handoff as the deletions above; not performed here. Note that
  `multiscale_bars.py`'s 15m/60m/240m bar derivation is exactly the kind of fixed-clock
  resample step called out below (a genuine downstream consumer that still needs bars, now
  reading from true ticks instead of the old 1-second file).
- `MindfulTrader/tools/` consumers reading `mes_continuous_ticks.parquet` as their real
  market-data source — `drift_location_eval.cpp`, `burstiness_recalibration.cpp`,
  `mean_rev_z_variant_comparison.cpp`/`.py`, `window_autocorrelation_diagnostic.py`,
  `amihud_liqfragility_recalibration.py`, `dim_acceptance_eval.py` — are not deleted (they're
  `MindfulTrader`-owned, this repo's own analysis tools) but must be repointed to the new
  `mes_ticks.parquet`. Each must be checked individually: a tool computing a genuinely
  tick-native statistic (e.g. activity-clock dims) should switch to raw per-tick indexing;
  a tool that still legitimately wants fixed-clock bars (e.g. anything comparing against the
  HMM's own 1s/15m/60m/240m training cadence) must gain its own explicit resample step
  reading the new tick file, rather than silently keep assuming its input is already
  1-second bars.
- Update every doc/spec cross-referencing `mes_continuous_ticks.parquet` as a "tick" source
  (`docs/ADR/phase2_triple_barrier_migration_spec.md`,
  `docs/superpowers/specs/2026-08-19-predator-fusion-tier2-physics-signal-spec.md`,
  `lbrnet/scripts/build_directional_alpha.py`'s help text, `GEMINI.md:296`, etc.) to point at
  the new `mes_ticks.parquet` (true ticks) or the retained bar file (if still produced
  downstream) as appropriate — do not leave stale references implying the old file was ever
  genuine tick data.
- This is a cross-repo change (`MindfulTrader` owns the new C++ tool; `lbrnet` owns the
  deletions) — do not perform the `lbrnet`-side deletions from a `MindfulTrader` session;
  hand off explicitly, matching this project's existing cross-repo handoff convention (see
  `docs/superpowers/specs/2026-08-26-activity-clock-lbrnet-handoff.md` for the pattern).

## 9. Explicitly out of scope

- Any change to the roll-date calendar rule, contract discovery regex, or ask/bid
  derivation logic — all reused exactly as empirically verified in the Python original.
- Scheduling/automation of the mirror refresh (cron, systemd timer) — this spec covers the
  mechanism only, matching the deferred scope of the sync design it ports.
- Re-deriving 1-second (or any other) bars from the new tick file — deferred to whichever
  future consumer actually needs bars again post-migration (§8).
