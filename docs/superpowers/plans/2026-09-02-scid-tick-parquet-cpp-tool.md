# C++ Tick-Level `.scid` Decoder + Mirror Sync Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Replace `lbrnet`'s Python tick-decode/aggregate/sync pipeline
(`lbrnet/data/mes_continuous.py` + `lbrnet/scripts/build_mes_continuous_bars.py`) with a
standalone, natively-tested C++/Arrow tool, `tools/scid_processing/scid_to_ticks_parquet.cpp`, that emits one
row per raw `.scid` trade record (no 1-second, or any, aggregation) and keeps the local
`lbrnet/data/scid/` mirror in sync with the live Sierra Chart directory
(`/mnt/c/SierraChart2/Data`) incrementally. This directly fixes a real, confirmed-live data
defect: `lbrnet/data/raw/mes_continuous_ticks.parquet` — read by this repo's own 16D
observation-vector calibration tools (`drift_location_eval.cpp`, `burstiness_recalibration.cpp`,
`mean_rev_z_variant_comparison.cpp`/`.py`, `window_autocorrelation_diagnostic.py`,
`amihud_liqfragility_recalibration.py`, `dim_acceptance_eval.py`) and by `lbrnet`'s labeling/
backtesting code — is named "ticks" but is actually 1-second bars, verified directly this
session against real `.scid` data.

**Architecture:** Four independently-shippable phases. **Phase A** (Tasks 1-3) is the pure,
ACSIL-independent decode/discovery layer: `tools/scid_processing/scid_reader.h` (mmap'd tick decode, SC-epoch
conversion, benign-jitter resequencing, ask/bid/spread/trade_side derivation — ported
byte-for-byte from `mes_continuous.py`'s `decode_scid_ticks()`) and
`tools/scid_processing/scid_contract_windows.h` (contract discovery, roll-date math, quarterly-cycle
validation — ported from `discover_contracts()`). Both are natively testable with zero
external dependencies (no Arrow, no `sierrachart.h`). **Phase B** (Task 4) is the mirror sync
layer, `tools/scid_processing/scid_mirror_sync.h`, porting `lbrnet`'s own (currently unimplemented)
`docs/superpowers/specs/2026-08-18-mes-scid-incremental-sync-design.md` into C++: grow/shrink/
new/unchanged detection, atomic copy-then-rename, rollover trim. **Phase C** (Tasks 5-6) is
the CLI tool itself, `tools/scid_processing/scid_to_ticks_parquet.cpp` — wires Phases A+B together, writes
chunked columnar Parquet via Arrow (matching `tools/context_to_parquet.cpp`'s established SoA
pattern), and persists per-contract parts for incremental resume. **Phase D** (Tasks 7-9) is
the migration: repoint this repo's own `tools/` consumers at the new tick file, extend
`tools/observation_vector/market_data_io.h` for the new schema, and produce a `lbrnet`-side handoff doc (this
repo does not edit `lbrnet/` directly, matching the existing cross-repo convention).

**Tech Stack:** C++17, Apache Arrow/Parquet C++ (via `mamba run -n mts`, `pkg-config --cflags/
--libs arrow parquet`), `std::filesystem` (mirror sync file ops), POSIX `mmap` (tick decode).
Native tests via bare `g++` + the existing `check(name, bool)` helper convention (see
`tools/test_context_reader.cpp`) — no GoogleTest/CMake. Never added to `CMakeLists.txt` or
`build_dll.sh` (standalone `tools/` binary, matching `context_to_parquet.cpp`'s precedent).

**Spec:** `docs/superpowers/specs/2026-09-02-scid-tick-parquet-cpp-tool-spec.md` (full spec,
all sections). This plan implements §2 (Phase A), §5 (Phase B), §3/§4/§6 (Phase C), and the
`MindfulTrader/tools/`-owned half of §8 (Phase D). The `lbrnet`-owned half of §8 (deleting
`mes_continuous.py`/`build_mes_continuous_bars.py`/`preprocess_scid_data.py`/
`test_mes_continuous.py`, repointing `build_directional_alpha.py`/`build_turtle_soup_dataset.py`/
`multiscale_bars.py`/`backtest/*.py`) is explicitly out of scope for this plan — Task 9
produces the handoff doc, no task edits anything under `lbrnet/`.

## Global Constraints

- **`lbrnet`-side deletions/repointing are explicitly out of scope for this plan** (spec §8) —
  no task modifies anything under `lbrnet/`. Task 9 (the handoff doc) is the only
  `lbrnet`-facing deliverable, matching `docs/superpowers/specs/2026-08-30-context-converter-
  lbrnet-handoff.md`'s existing precedent.
- **Reuse the Python original's verified algorithms exactly, do not redesign them**: roll-date
  math, contract discovery/validation, SC-epoch offset, benign-jitter threshold
  (`1_000_000`µs), ask/bid/spread/trade_side derivation. Every constant and formula in Tasks
  1-2 must trace to a specific line/behavior in `lbrnet/data/mes_continuous.py`, not be
  independently re-derived.
- **DOD discipline throughout** (spec §4, this codebase's established convention per
  `tools/context_to_parquet.cpp`, `CLAUDE.md` Performance Rules):
  - Decode via direct `mmap` read view over raw `.scid` bytes, reinterpreted in place — never
    copy a whole contract's active window into a `std::vector<s_IntradayRecord>` first.
  - Binary search (`std::lower_bound`) over the mmap'd timestamp stride for window slicing —
    the C++ analog of `np.searchsorted`.
  - Columnar (SoA) Arrow builders, one per output column — never a row-of-structs
    intermediate (matches `ChunkBuffers` in `context_to_parquet.cpp`).
  - Fixed-size chunked writes (`kChunkRows = 2'000'000`, matching `context_to_parquet.cpp`'s
    convention) via `NewBufferedRowGroup()` + `WriteRecordBatch()` — never accumulate a whole
    contract's ticks (up to ~30M+) in one builder.
  - No heap allocation inside the per-record derivation loop.
- **Standalone tool, `tools/` directory only** — bare `g++` build via the `mamba run -n mts`
  environment. Never added to `CMakeLists.txt`/`build_dll.sh`.
- Output schema is exactly spec §3's table (`timestamp_us`, `trade_price`, `ask_price`,
  `bid_price`, `spread`, `volume`, `bid_volume`, `ask_volume`, `trade_side`, `contract`),
  written to a **new** file, `lbrnet/data/raw/mes_ticks.parquet` — never overwrite the
  existing `mes_continuous_ticks.parquet` (bar file) from this tool.
- Full native test pass (`g++ ... && ./a.out` exits 0, all `check()` calls print `PASS`) is
  required before a task is considered done — no task is "mostly working."

---

## Task 1: `tools/scid_processing/scid_reader.h` — mmap tick decode, epoch conversion, jitter resequencing, ask/bid derivation

**Files:**
- Create: `tools/scid_processing/scid_reader.h`
- Create: `tools/scid_processing/test_scid_reader.cpp`

**Interfaces:**
- `struct ScidRecordView` — POD overlay matching `s_IntradayRecord`'s 40-byte layout
  (`sierra_chart_dependencies/IntradayRecord.h`): `int64_t raw_timestamp; float open, high,
  low, close; uint32_t trades, volume, bid_volume, ask_volume;`. `static_assert(sizeof(ScidRecordView)
  == 40)`.
- `constexpr int64_t kScEpochOffsetUs = 2209161600000000;` (matches `mes_continuous.py`'s
  `SC_EPOCH_OFFSET_US`).
- `constexpr int64_t kMaxBenignTimestampJitterUs = 1'000'000;` (matches
  `_MAX_BENIGN_TIMESTAMP_JITTER_US`).
- `struct DecodedTickChunk { std::vector<int64_t> timestamp_us; std::vector<double>
  trade_price, ask_price, bid_price, spread; std::vector<int64_t> volume, bid_volume,
  ask_volume, trade_side; void Reserve(std::size_t n); void Clear(); std::size_t Rows() const;
  }` — one chunk's worth of decoded, resequenced ticks. `Reserve`/`Clear` follow
  `context_to_parquet.cpp`'s `ChunkBuffers` pattern exactly (`reserve()` each vector once,
  `clear()` — never destroy/reconstruct — between chunks): the caller owns one
  `DecodedTickChunk` instance for a whole contract's decode and passes it by mutable
  reference across every `on_chunk` invocation, avoiding per-chunk heap churn across a
  contract's potentially hundreds of chunks.
- `class ScidFileView` — owns the `mmap`'d file (RAII: `mmap`/`munmap` in ctor/dtor,
  non-copyable). Constructor takes the file path, validates `HEADER_SIZE=56` +
  `sizeof(ScidRecordView)=40` record stride against the file size (mirrors the Python
  original's `np.memmap(..., shape=(n_records,))` explicit-shape requirement — reject a file
  whose trailing bytes don't form a whole record, same defect the Python docstring already
  flags as previously silently truncated by `np.fromfile`).
- `std::size_t ScidFileView::LowerBoundUs(int64_t target_posix_us) const` — binary search over
  the mmap'd `raw_timestamp` stride for `target_posix_us + kScEpochOffsetUs`, returning the
  first record index `>=` that value (the C++ analog of `np.searchsorted(..., side="left")`).
- `void ScidFileView::AdviseRange(std::size_t lo, std::size_t hi, int advice) const` — thin
  wrapper over `madvise()` on the byte range spanning records `[lo, hi)`. `DecodeScidTicks`
  calls this with `MADV_SEQUENTIAL` on the resolved `[lo, hi)` range before its decode loop
  starts, and `MADV_DONTNEED` on the same range once the loop finishes — this codebase has no
  prior `madvise` usage, but contract files are 1-2GB scanned sequentially, exactly the case
  `madvise` optimizes for.
- `DecodeScidTicks(const ScidFileView& file, int64_t start_us, std::optional<int64_t> end_us,
  std::size_t chunk_rows, DecodedTickChunk& chunk, const std::function<void(const
  DecodedTickChunk&)>& on_chunk)` — decodes `[start_us, end_us)`, resequencing benign jitter
  (stable sort within the whole window — matches the Python original's whole-window
  `np.argsort(kind="stable")`, not a per-chunk sort, since a jitter pair could straddle a
  chunk boundary), filling the caller-owned `chunk` (already `Reserve(chunk_rows)`'d) and
  invoking `on_chunk(chunk)` once per `chunk_rows`-sized batch, calling `chunk.Clear()`
  immediately after each `on_chunk` returns. Throws `std::runtime_error` if any backwards
  jump exceeds `kMaxBenignTimestampJitterUs`, with a message naming the file and worst-jump
  magnitude (matches the Python original's error message shape).

**Steps:**

- [x] Step 1: Write `tools/scid_processing/test_scid_reader.cpp` first (failing): a helper that writes a
  synthetic 56-byte header + N 40-byte records to a temp file, then asserts:
  - `ScidFileView` opens successfully and record count matches `(file_size - 56) / 40`.
  - A file whose trailing bytes are not a whole multiple of 40 (post-header) throws.
  - `LowerBoundUs` returns the correct index for a target timestamp exactly matching one
    record, one strictly between two records, one before the first, one after the last.
  - `DecodeScidTicks` on an all-in-order fixture returns ticks in the exact input order with
    `ask_price=high`, `bid_price=low`, `spread=high-low`, `trade_side` per the `ask_volume>0
    ? 1 : (bid_volume>0 ? -1 : 0)` rule, `timestamp_us = raw_timestamp - kScEpochOffsetUs`.
  - A fixture with one sub-1s backwards jump resequences correctly (stable, preserves
    relative order of any exact-timestamp ties).
  - A fixture with a jump exceeding `kMaxBenignTimestampJitterUs` throws
    `std::runtime_error`.
  - A fixture spanning 3 chunks (`chunk_rows` small, e.g. 2, with 7 total records) invokes
    `on_chunk` exactly 4 times with the correct per-chunk row counts and correct
    concatenated order, and the **same** `DecodedTickChunk` instance/address is reused across
    all 4 invocations (assert on `&chunk` identity) — the concrete regression check for the
    Reserve/Clear reuse contract, not just a correctness check.
  - `AdviseRange` is at minimum callable without error on a real mapped range (a real
    `madvise` return-value assertion is platform-dependent noise; the meaningful check is
    that decode still produces correct output when the advise calls are wired in, not the
    kernel's actual paging behavior).
- [x] Step 2: Implement `tools/scid_processing/scid_reader.h` to make the tests pass. Build/run:
  `mamba run -n mts g++ -std=c++17 -Iinclude tools/scid_processing/test_scid_reader.cpp -o /tmp/test_scid_reader
  && /tmp/test_scid_reader` — all `check()` calls must print `PASS`. **DONE** — committed
  `e082969`, all 19 checks pass.

## Task 2: `tools/scid_processing/scid_contract_windows.h` — contract discovery, roll-date math, quarterly-cycle validation

**Files:**
- Create: `tools/scid_processing/scid_contract_windows.h`
- Create: `tools/scid_processing/test_scid_contract_windows.cpp`

**Interfaces:**
- `constexpr int kRolloverDaysBeforeExpiry = 4;`
- `std::chrono::year_month_day ThirdFriday(int year, int month)` — 3rd Friday of the month
  (matches `mes_continuous.py`'s `third_friday()`).
- `std::chrono::year_month_day ContractActiveEnd(char month_code, int year)` — 3rd Friday minus
  `kRolloverDaysBeforeExpiry` days. `month_code` in `{'H','M','U','Z'}` mapping to
  `{3,6,9,12}` (matches `CONTRACT_MONTH_CODES`).
- `std::optional<std::pair<char,int>> ParseContractFilename(std::string_view name)` — matches
  filenames `MES[HMUZ]\d{2}-CME\.scid` (matches `_CONTRACT_FILENAME_RE`/
  `parse_contract_filename`), returns `(month_code, year)` or `nullopt`.
- `struct ContractWindow { std::string path; char month_code; int year; int64_t
  active_start_us; std::optional<int64_t> active_end_us; };`
- `std::vector<ContractWindow> DiscoverContracts(const std::string& data_dir)` — globs
  `MES*-CME.scid` in `data_dir`, parses/sorts by `active_end` date, validates the quarterly
  H/M/U/Z cycle has no gaps (throws `std::invalid_argument` naming the missing contract,
  matching the Python original's error message), validates no duplicate `active_end` date,
  and returns non-overlapping windows with the most-recent contract's `active_end_us =
  nullopt` (open-ended). Throws `std::runtime_error` if zero contracts are found.

**Steps:**

- [x] Step 1: Write `tools/scid_processing/test_scid_contract_windows.cpp` first (failing):
  - `ThirdFriday`/`ContractActiveEnd` against a handful of known real contract months (verify
    against `mes_continuous.py`'s own docstring/known values — e.g. compute independently for
    2024/2025 H/M/U/Z months and hand-check against a calendar). This roll rule (4 calendar
    days before the 3rd Friday) is Sierra Chart's own configured rollover for this exact
    symbol — traced in `lbrnet`'s `docs/superpowers/specs/2026-08-08-mes-continuous-bars-
    design.md` to both the Sierra Chart Symbol Settings UI tooltip and
    `SymbolSettings.scdataallservices.xml`'s stored `rollover-method=Method3`/
    `rollover-input-1=4`/`rollover-input-2=3` for `MES?##-CME` — not an assumed generic CME
    last-trading-day convention independently re-derived here.
  - `ParseContractFilename` accepts `"MESU23-CME.scid"` → `('U', 2023)`, rejects
    `"MES-202409-CME-USD.scid"` (the old-convention stub filename — must NOT match, this is
    the exact contamination case flagged in this session's earlier data investigation).
  - `DiscoverContracts` on a synthetic directory with a full H/M/U/Z/H/M/U/Z... sequence
    (empty placeholder files, only filenames matter) returns correctly non-overlapping windows
    with only the last window's `active_end_us` being `nullopt`.
  - `DiscoverContracts` on a directory missing one contract in the middle of the cycle throws,
    naming the missing contract.
  - `DiscoverContracts` on an empty directory (or one with only non-matching filenames)
    throws.
- [x] Step 2: Implement `tools/scid_processing/scid_contract_windows.h` to make the tests pass. Build/run:
  `mamba run -n mts g++ -std=c++17 -Iinclude tools/scid_processing/test_scid_contract_windows.cpp -o
  /tmp/test_scid_contract_windows && /tmp/test_scid_contract_windows`. **DONE** — committed
  `2fb7f60`, all 22 checks pass. Used Howard Hinnant civil-calendar day-count algorithms
  instead of `std::chrono` calendar types (those are C++20-only; this repo's `tools/` baseline
  is C++17).

## Task 3: End-to-end decode integration check (Phase A close-out)

**Files:**
- Create: `tools/scid_processing/test_scid_decode_integration.cpp`

**Interfaces:** none new — exercises Tasks 1+2 together against a small **real** contract file
slice.

**Steps:**

- [ ] Step 1: Write a test that opens a small byte-range copy of a real contract file (e.g.
  the first ~200KB of `lbrnet/data/scid/MESU23-CME.scid`, copied to a fixture path once —
  do not commit multi-GB fixtures) via `ScidFileView`, calls `DiscoverContracts` against a
  temp dir containing just that one renamed/trimmed fixture file, and decodes its whole
  window via `DecodeScidTicks`. Assert: row count matches the fixture's own record count;
  first/last `timestamp_us` match hand-computed values from the raw bytes (cross-check
  against this session's own earlier manual `struct.unpack` verification); `ask_price >=
  bid_price` (i.e. `spread >= 0`) for every row (matches the Python original's own runtime
  guard, spec §2's non-negative-spread sanity check — verify it as a test assertion here,
  not just a runtime `throw`).
- [x] Step 2: Confirm build/run passes: `mamba run -n mts g++ -std=c++17 -Iinclude
  tools/scid_processing/test_scid_decode_integration.cpp -o /tmp/test_scid_decode_integration &&
  /tmp/test_scid_decode_integration`. **DONE** — committed `9d8afc8`, all 7 checks pass.

## Task 4: `tools/scid_processing/scid_mirror_sync.h` — mirror refresh from live Sierra Chart directory

**Files:**
- Create: `tools/scid_processing/scid_mirror_sync.h`
- Create: `tools/scid_processing/test_scid_mirror_sync.cpp`

**Interfaces:**
- `enum class MirrorSyncAction { kNew, kUnchanged, kGrown, kShrunkOrChanged };`
- `struct MirrorSyncResult { std::string contract_filename; MirrorSyncAction action; };`
- `std::vector<MirrorSyncResult> SyncScidMirror(const std::string& live_dir, const
  std::string& mirror_dir)` — for every `MES[HMUZ]\d{2}-CME\.scid` file in `live_dir`
  (reusing `ParseContractFilename` from Task 2 to filter):
  - Not present in `mirror_dir` → copy in (atomic: write to `{name}.tmp`, then
    `std::filesystem::rename` over the final name), record `kNew`.
  - Present, `(size, mtime)` identical → record `kUnchanged`, no copy.
  - Present, live size **greater** than mirror size and mirror is a byte-for-byte prefix of
    live (checked by comparing the mirror's own trailing header/first-record bytes against
    the live file's corresponding bytes — cheap prefix check, not a full re-read) → atomic
    copy-overwrite, record `kGrown`.
  - Present, live size **less than** mirror size, or the leading `HEADER_SIZE` bytes differ →
    atomic copy-overwrite, record `kShrunkOrChanged`, log a warning (this signals a
    re-exported/replaced file, per spec §5 — the caller must treat this as "force full
    reprocess," not delta-decode).
  - Never writes to `live_dir` — read-only against it.
- Return value only reports what changed; it does not itself trigger any decode — that's the
  CLI orchestration layer's job (Task 5).
- Copy mechanism: `std::filesystem::copy_file` (glibc dispatches this to `copy_file_range`/
  `sendfile` on Linux — a kernel-side copy with no userspace buffer round-trip), never a
  hand-rolled `read()`/`write()` loop through a userspace buffer, which would double the
  memory traffic for every byte of a multi-GB contract file.

**Steps:**

- [ ] Step 1: Write `tools/scid_processing/test_scid_mirror_sync.cpp` first (failing):
  - New file in `live_dir`, absent from `mirror_dir` → copied in, `kNew`, byte-identical
    afterward.
  - Unchanged file (same size/mtime) → `kUnchanged`, mirror file's mtime/content untouched
    (verify by writing a sentinel byte pattern to the mirror copy first — if a real copy
    happened it would be overwritten, breaking the sentinel check).
  - Grown file (mirror is a byte-prefix of a larger live file) → `kGrown`, mirror now matches
    live exactly.
  - Shrunk file → `kShrunkOrChanged`, mirror now matches the (smaller) live file, one warning
    logged (capture via a redirected `stderr` or an injectable logger, whichever this
    codebase's convention supports; a simple `stderr` capture is fine if no logger
    abstraction exists yet).
  - Header-bytes-changed-but-same-size file → `kShrunkOrChanged`.
  - Simulated interrupted copy (kill mid-write to `.tmp`, i.e. manually create a partial
    `.tmp` file before calling `SyncScidMirror`) → the prior good mirror file is untouched,
    and the sync either cleans up or overwrites the stale `.tmp` (both acceptable; the
    invariant under test is that the *real* filename's content is never torn).
  - Non-matching filenames (e.g. the old `MES-202409-CME-USD.scid` stub pattern) in
    `live_dir` are ignored entirely — never copied.
- [ ] Step 2: Implement `tools/scid_processing/scid_mirror_sync.h` to make the tests pass. Build/run:
  `mamba run -n mts g++ -std=c++17 -Iinclude tools/scid_processing/test_scid_mirror_sync.cpp -o
  /tmp/test_scid_mirror_sync && /tmp/test_scid_mirror_sync`.

## Task 5: `tools/scid_processing/scid_to_ticks_parquet.cpp` — CLI, chunked Arrow writer, per-contract parts

**Files:**
- Create: `tools/scid_processing/scid_to_ticks_parquet.cpp`

**Interfaces:**
- CLI flags (matches spec §6): `--data-dir` (default `lbrnet/data/scid`), `--data-dir-live`
  (default `/mnt/c/SierraChart2/Data`), `--output` (default
  `lbrnet/data/raw/mes_ticks.parquet`), `--sync-only`, `--no-sync`, `--full-rebuild`.
- Per-contract parts directory: `{data-dir}/.parts/{contract_label}.parquet` (mirrors the
  `lbrnet` sync design's own `data/scid/.parts/` convention exactly, so a future `lbrnet`
  session porting incremental resume can recognize the layout) — gitignored, never committed.
- Orchestration, matching the reused design 1:1:
  1. Unless `--no-sync`: call `SyncScidMirror(data-dir-live, data-dir)` (Task 4). If
     `--sync-only`, print a summary and exit 0 here.
  2. Call `DiscoverContracts(data-dir)` (Task 2).
  3. For each window: if `--full-rebuild` or no existing part or that contract was
     `kNew`/`kShrunkOrChanged` this run → decode its whole active window (Task 1) and (re)write
     its part from scratch. If that contract was `kGrown` → read the existing part's own max
     `timestamp_us`, resume `DecodeScidTicks` from `max_ts + 1`, append the new chunk(s) to the
     existing part (atomic rewrite: write combined `{part}.tmp`, sort by `timestamp_us` if the
     delta could contain jitter spanning the old/new boundary — reuse Task 1's resequencing
     bound — then rename over the part). If `kUnchanged` and this contract's window closed
     this run (a later contract just appeared) → trim any rows `>= active_end_us` from the
     existing part (rollover trim, spec §5) via the same atomic-rewrite pattern; otherwise skip
     entirely.
  4. Final merge: since each part is already sorted and contracts are non-overlapping by
     construction, stream all parts in contract order directly into the final
     `parquet::arrow::FileWriter` (chunked, `kChunkRows`-sized batches per part) — a linear
     concatenation, not a merge-sort.
- **Step 3 runs contracts concurrently**, not sequentially: one `std::thread` per contract
  needing decode this run, capped at `std::thread::hardware_concurrency()` (a simple work
  queue if more contracts need decode than available threads). Each thread decodes its own
  window into its own `{contract_label}.parquet` part using its own `DecodedTickChunk`
  instance (Task 1) — no shared mutable state between threads, so no locking is needed on
  the hot path. Step 4 (final merge) is a join point: it starts only after every thread from
  Step 3 has completed. Step 1 (mirror sync) and Step 2 (contract discovery) stay
  single-threaded — they're fast, whole-directory operations, not per-contract work.
- Every Arrow builder column matches spec §3's schema exactly; `contract` column is the
  `{month_code}{year%100:02d}` label (e.g. `"U26"`), stamped per-part.
- **Explicit `parquet::WriterProperties`/`parquet::ArrowWriterProperties`, not library
  defaults**: build the writer via a `parquet::WriterProperties::Builder` with `.compression
  (parquet::Compression::ZSTD)`, and a `parquet::ArrowWriterProperties::Builder` with
  dictionary encoding enabled for the `contract` column — pass both into
  `parquet::arrow::FileWriter::Open(...)`'s properties arguments. Row groups stay aligned to
  `kChunkRows` (one `WriteRecordBatch` per `NewBufferedRowGroup`), which gives downstream
  readers row-group-level `timestamp_us` min/max statistics for free predicate pushdown on
  time-bounded reads.
- Chunk buffers reused across the whole run, not per-chunk: the same `DecodedTickChunk`
  (Task 1) and Arrow-builder scratch state used per contract is `Reserve(kChunkRows)`'d once
  and `Clear()`'d between chunks, matching `context_to_parquet.cpp`'s `ChunkBuffers` pattern.
- Print a summary on completion: total rows, per-contract row counts, `ts_min`/`ts_max`,
  matching `BuildSummary`'s existing fields in `mes_continuous.py` (same shape, so any
  downstream log-scraping stays compatible).

**Steps:**

- [ ] Step 1: Implement CLI argument parsing (mirror `context_to_parquet.cpp`'s existing
  arg-parsing style) and the orchestration steps above.
- [ ] Step 2: Build: `mamba run -n mts g++ -O2 -std=c++17 -Iinclude
  $(mamba run -n mts pkg-config --cflags arrow parquet) tools/scid_processing/scid_to_ticks_parquet.cpp
  $(mamba run -n mts pkg-config --libs arrow parquet) -o tools/bin/scid_to_ticks_parquet`.
- [ ] Step 3: Smoke-run against the real local mirror (`lbrnet/data/scid/`) with
  `--full-rebuild` on a **time-bounded** slice first if the full run is too slow for
  iteration (e.g. add a temporary `--max-contracts N` debug flag, or just let one contract
  run to completion first) — confirm output row count roughly matches
  expectations (should be far larger than `mes_continuous_ticks.parquet`'s 38.5M rows, since
  no aggregation occurs).
- [ ] Step 4: Verify output against Task 3's decode-integration expectations: read a sample
  of the output parquet back (e.g. via a small Python `polars.read_parquet(..., n_rows=...)`
  one-off check, or a small C++ read using `market_data_io.h`'s existing Arrow-reading
  pattern) and confirm `timestamp_us` deltas are **not** exclusively 1-second multiples
  (the concrete, falsifiable check that this migration actually fixed the granularity defect).

## Task 6: Native tests for the mirror-sync + incremental-resume orchestration (end-to-end)

**Files:**
- Create: `tools/scid_processing/test_scid_to_ticks_parquet_e2e.cpp`

**Interfaces:** none new — exercises Task 5's orchestration logic directly (factor the
per-contract decision logic in Task 5 into a small testable free function, e.g.
`DecidePartAction(MirrorSyncAction, bool has_existing_part, bool full_rebuild) ->
PartAction{kFullDecode, kResumeAppend, kTrim, kSkip}`, so this test doesn't need to shell out
to the built binary).

**Steps:**

- [ ] Step 1: Write tests for `DecidePartAction` covering every combination in Task 5's
  orchestration table (new/grown/shrunk/unchanged × has-part/no-part × full-rebuild flag).
- [ ] Step 2: Two-small-real-contract-fixture end-to-end test (matching spec §7's own
  requirement): full build once, then simulate a delta (copy a few more real records onto one
  fixture, matching the byte layout Task 1 already validates) and confirm the incremental
  result is tick-for-tick identical to a from-scratch full rebuild of the updated fixture.
- [ ] Step 3: Build/run and confirm all `check()` calls pass.

## Task 7: `tools/observation_vector/market_data_io.h` — read the new tick schema

**Files:**
- Modify: `tools/observation_vector/market_data_io.h`

**Interfaces:**
- `ReadTicksParquet()` currently projects only `timestamp_us`/`close` — the new tick file has
  no `close` column (spec §3's schema uses `trade_price`). Update `TickSeries`/
  `ReadTicksParquet()` to read `trade_price` as the price column when present, falling back
  to `close` (for callers still pointed at the legacy `mes_continuous_ticks.parquet` bar
  file during the migration window) — resolve the column name dynamically via
  `schema->GetFieldIndex()`, trying `"trade_price"` first, `"close"` second, throwing only if
  neither exists. Do not silently rename `TickSeries::close` — keep it as the struct's
  field name to avoid a churn-only rename across every existing call site (`drift_location_eval.cpp`,
  `jump_ratio_eval.cpp`), even though its logical meaning is now "the trade/close price from
  whichever schema was read."

**Steps:**

- [ ] Step 1: Update `ReadTicksParquet()`'s column-resolution logic as above.
- [ ] Step 2: Confirm existing native callers (`tools/observation_vector/drift_location_eval.cpp`,
  `tools/observation_vector/jump_ratio_eval.cpp` — check both for a `ReadTicksParquet` call site) still build
  unchanged: `mamba run -n mts g++ -O2 -std=c++17 $(mamba run -n mts pkg-config --cflags arrow
  parquet) tools/observation_vector/drift_location_eval.cpp $(mamba run -n mts pkg-config --libs arrow parquet)
  -o /tmp/drift_location_eval_smoke`.
- [ ] Step 3: Smoke-test `ReadTicksParquet()` against both the old bar file (still works,
  falls back to `close`) and the new tick file (finds `trade_price`) — a small ad hoc test or
  extend an existing test file, whichever is less churn.

## Task 8: Repoint `MindfulTrader/tools/` consumers at the new tick file

**Files:**
- Modify: `tools/observation_vector/drift_location_eval.cpp`, `tools/observation_vector/burstiness_recalibration.cpp`,
  `tools/observation_vector/mean_rev_z_variant_comparison.cpp`, `tools/observation_vector/mean_rev_z_variant_comparison.py`,
  `tools/observation_vector/window_autocorrelation_diagnostic.py`, `tools/observation_vector/amihud_liqfragility_recalibration.py`,
  `tools/observation_vector/dim_acceptance_eval.py` (only the ones whose default path constant literally needs
  to change — verify each file's actual current constant before editing, per spec §8's
  per-tool judgment note).

**Interfaces:** no new interfaces — each file's `TICKS_PARQUET`/`CONTINUOUS_TICKS`/
`--ticks-parquet` default constant changes from `lbrnet/data/raw/mes_continuous_ticks.parquet`
to `lbrnet/data/raw/mes_ticks.parquet`.

**Steps:**

- [ ] Step 1: For each listed tool, read its actual current usage of the ticks file (some
  compute genuinely tick-native statistics that benefit directly from the switch; others —
  per spec §8 — may assume fixed 1-second spacing internally, e.g. treating consecutive rows
  as exactly 1 second apart for a rate calculation) and classify: **(a) switch path only**
  (tool already treats `timestamp_us` deltas generically) or **(b) needs an explicit resample
  step added first** (tool currently assumes uniform 1s spacing). Do not blanket-switch a
  type-(b) tool's path without adding its resample step — that would silently corrupt its
  output, not fix it.
- [ ] Step 2: Apply the path switch (type-(a) tools) or add the resample step + path switch
  together (type-(b) tools, if any are found) in the same edit.
- [ ] Step 3: Re-run each tool once against real data (or its existing offline validation
  path, e.g. `--report-json`) and confirm it still produces sane, non-empty output — this is
  a smoke check, not a full recalibration re-run.

## Task 9: `lbrnet`-side handoff doc

**Files:**
- Create: `docs/superpowers/specs/2026-09-02-scid-ticks-lbrnet-handoff.md` (in
  `MindfulTrader`, matching `docs/superpowers/specs/2026-08-30-context-converter-lbrnet-
  handoff.md`'s existing precedent and location convention)

**Interfaces:** none — documentation only.

**Steps:**

- [ ] Step 1: Write the handoff doc covering: what shipped (this plan's tool, its CLI, its
  output schema/path), the build command, and the explicit `lbrnet`-side action list from
  spec §8 — delete `lbrnet/scripts/preprocess_scid_data.py` (already dead, zero live callers,
  independent of this migration), delete `lbrnet/data/mes_continuous.py` +
  `lbrnet/scripts/build_mes_continuous_bars.py` + `tests/test_mes_continuous.py` once this
  tool's tick-decode output is verified equivalent (tick-for-tick match against
  `decode_scid_ticks()`'s own pre-aggregation columns — name the exact comparison procedure),
  mark `docs/superpowers/specs/2026-08-18-mes-scid-incremental-sync-design.md` superseded,
  and repoint `build_directional_alpha.py`/`build_turtle_soup_dataset.py`/
  `multiscale_bars.py`/`backtest/generate_oracle.py`/`backtest/run_phase2.py`/
  `backtest/run_phase2_verdict.py` — noting `multiscale_bars.py`'s 15m/60m/240m bar
  derivation now needs its own resample step reading the new tick file (it can no longer
  assume its input is already 1-second bars).
- [ ] Step 2: State plainly, per this doc's own established convention: **do not begin the
  `lbrnet`-side work from a `MindfulTrader` session** — this doc exists so a `lbrnet`-rooted
  session has everything it needs without re-deriving it.
