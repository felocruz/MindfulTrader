//! Streams the raw MES tick file (`lbrnet/data/raw/mes_ticks.parquet`, one row per trade, written by
//! `tools/scid_processing/scid_to_ticks_parquet.cpp`) into `observation_engine` trade batches.
//!
//! - **Projection:** only the five columns the engine needs (plus `contract`) are decoded.
//! - **Lazy row groups:** an optional time range skips every row group whose `timestamp_us`
//!   statistics fall outside it -- a training window or one contract costs only its own row groups.
//! - **Pipelined:** decoding runs on its own thread, a bounded channel ahead of the engine, so the
//!   engine computes while the next chunk decodes; memory stays bounded by the channel depth.
//! - **Zero copy where the types match:** times (`i64`) and prices (`f64`) are lent straight from the
//!   Arrow buffers; the two volume columns are validated into reused `u32` buffers (the file stores
//!   `i64`, the engine's trade volume is a contract count).
//!
//! Each chunk is split at contract changes and at any backwards timestamp into runs; the caller
//! decides what a new run means (offline replay resets the engine: a roll is a new instrument).

use std::fs::File;
use std::path::{Path, PathBuf};
use std::sync::mpsc::{Receiver, sync_channel};
use std::thread::JoinHandle;

use arrow_array::{Array, Float64Array, Int64Array, LargeStringArray, RecordBatch, StringArray, StringViewArray};
use observation_engine::input::TradeColumns;
use parquet::arrow::ProjectionMask;
use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;
use parquet::file::statistics::Statistics;

/// MES minimum price increment, index points.
pub const TICK_SIZE: f64 = 0.25;
const COLUMNS: [&str; 5] = ["timestamp_us", "trade_price", "bid_volume", "ask_volume", "contract"];
const BATCH_ROWS: usize = 1 << 17;
const CHANNEL_DEPTH: usize = 4;

/// One decoded chunk of trades.
#[derive(Debug)]
pub struct TickChunk {
    time_us: Int64Array,
    price: Float64Array,
    buy_volume: Vec<u32>,
    sell_volume: Vec<u32>,
    /// The chunk's runs, in order. A run continues the previous chunk's last run unless it starts a
    /// new segment (first trade of the stream, a contract change, or time going backwards).
    pub runs: Vec<Run>,
}

/// A contiguous stretch of one contract's trades within a chunk.
#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Run {
    pub start: usize,
    pub contract: String,
    pub new_segment: bool,
}

impl TickChunk {
    pub fn len(&self) -> usize {
        self.time_us.len()
    }

    pub fn is_empty(&self) -> bool {
        self.time_us.is_empty()
    }

    /// The trades of run `k`, as engine columns (times and prices borrowed from Arrow).
    pub fn run_trades(&self, k: usize) -> TradeColumns<'_> {
        let start = self.runs[k].start;
        let end = self.runs.get(k + 1).map_or(self.len(), |r| r.start);
        TradeColumns {
            time_us: &self.time_us.values()[start..end],
            price: &self.price.values()[start..end],
            buy_volume: &self.buy_volume[start..end],
            sell_volume: &self.sell_volume[start..end],
        }
    }
}

/// Options for opening the stream.
#[derive(Debug, Clone, Default)]
pub struct StreamOptions {
    /// Keep only row groups that may contain trades in `[from, to)` (Unix us); rows outside it in a
    /// kept group are dropped too.
    pub time_range: Option<(i64, i64)>,
    /// Stop after this many trades (0 = all).
    pub max_rows: usize,
}

/// A running decode thread and its channel.
#[derive(Debug)]
pub struct TickStream {
    rx: Receiver<Result<TickChunk, String>>,
    handle: Option<JoinHandle<()>>,
    pub row_groups_total: usize,
    pub row_groups_read: usize,
}

impl Iterator for TickStream {
    type Item = Result<TickChunk, String>;

    fn next(&mut self) -> Option<Self::Item> {
        match self.rx.recv() {
            Ok(item) => Some(item),
            Err(_) => {
                if let Some(h) = self.handle.take() {
                    let _ = h.join();
                }
                None
            }
        }
    }
}

fn row_group_may_overlap(stats: Option<&Statistics>, from: i64, to: i64) -> bool {
    match stats {
        Some(Statistics::Int64(s)) => match (s.min_opt(), s.max_opt()) {
            (Some(&lo), Some(&hi)) => hi >= from && lo < to,
            _ => true,
        },
        _ => true,
    }
}

/// Opens `path` and starts decoding on a background thread.
pub fn open(path: &Path, options: StreamOptions) -> Result<TickStream, String> {
    let file = File::open(path).map_err(|e| format!("cannot open {}: {e}", path.display()))?;
    let builder = ParquetRecordBatchReaderBuilder::try_new(file).map_err(|e| format!("not a parquet file: {e}"))?;
    let schema = builder.parquet_schema();
    let mut leaves = Vec::with_capacity(COLUMNS.len());
    for name in COLUMNS {
        let idx = schema
            .columns()
            .iter()
            .position(|c| c.name() == name)
            .ok_or_else(|| format!("column '{name}' is missing from {}", path.display()))?;
        leaves.push(idx);
    }
    let ts_leaf = leaves[0];
    let row_groups = builder.metadata().row_groups();
    let row_groups_total = row_groups.len();
    let selected: Vec<usize> = match options.time_range {
        Some((from, to)) => (0..row_groups_total).filter(|&i| row_group_may_overlap(row_groups[i].column(ts_leaf).statistics(), from, to)).collect(),
        None => (0..row_groups_total).collect(),
    };
    let row_groups_read = selected.len();
    let mask = ProjectionMask::leaves(schema, leaves);
    let reader = builder
        .with_projection(mask)
        .with_row_groups(selected)
        .with_batch_size(BATCH_ROWS)
        .build()
        .map_err(|e| format!("cannot build the parquet reader: {e}"))?;

    let (tx, rx) = sync_channel(CHANNEL_DEPTH);
    let path: PathBuf = path.to_path_buf();
    let handle = std::thread::Builder::new()
        .name("tick-decode".into())
        .spawn(move || {
            let mut state = SplitState::default();
            let mut sent = 0usize;
            for batch in reader {
                let chunk = batch
                    .map_err(|e| format!("parquet read failed in {}: {e}", path.display()))
                    .and_then(|b| to_chunk(&b, &options, &mut state, sent));
                let stop = match &chunk {
                    Ok(c) => {
                        sent += c.len();
                        options.max_rows > 0 && sent >= options.max_rows
                    }
                    Err(_) => true,
                };
                let empty = chunk.as_ref().is_ok_and(TickChunk::is_empty);
                if !empty && tx.send(chunk).is_err() {
                    return;
                }
                if stop {
                    return;
                }
            }
        })
        .map_err(|e| format!("cannot start the decode thread: {e}"))?;
    Ok(TickStream { rx, handle: Some(handle), row_groups_total, row_groups_read })
}

/// Run-splitting state carried across chunks.
#[derive(Debug, Default)]
struct SplitState {
    contract: String,
    last_time: Option<i64>,
}

fn contract_column(batch: &RecordBatch) -> Result<Vec<&str>, String> {
    let col = batch.column_by_name("contract").ok_or("column 'contract' is missing")?;
    let any = col.as_any();
    if let Some(a) = any.downcast_ref::<StringArray>() {
        return Ok((0..a.len()).map(|i| a.value(i)).collect());
    }
    if let Some(a) = any.downcast_ref::<LargeStringArray>() {
        return Ok((0..a.len()).map(|i| a.value(i)).collect());
    }
    if let Some(a) = any.downcast_ref::<StringViewArray>() {
        return Ok((0..a.len()).map(|i| a.value(i)).collect());
    }
    Err("column 'contract' is not a string column".into())
}

fn i64_column<'a>(batch: &'a RecordBatch, name: &str) -> Result<&'a Int64Array, String> {
    batch.column_by_name(name).and_then(|c| c.as_any().downcast_ref::<Int64Array>()).ok_or_else(|| format!("column '{name}' is not Int64"))
}

fn to_chunk(batch: &RecordBatch, options: &StreamOptions, state: &mut SplitState, already: usize) -> Result<TickChunk, String> {
    let ts = i64_column(batch, "timestamp_us")?;
    let px = batch
        .column_by_name("trade_price")
        .and_then(|c| c.as_any().downcast_ref::<Float64Array>())
        .ok_or("column 'trade_price' is not Float64")?;
    let bid = i64_column(batch, "bid_volume")?;
    let ask = i64_column(batch, "ask_volume")?;
    let contract = contract_column(batch)?;
    if ts.null_count() + px.null_count() + bid.null_count() + ask.null_count() > 0 {
        return Err(format!("null values in a required column near row {already}"));
    }
    // Rows to keep: the time range, then the row cap.
    let mut lo = 0usize;
    let mut hi = ts.len();
    if let Some((from, to)) = options.time_range {
        let v = ts.values();
        lo = v.partition_point(|&t| t < from).min(hi);
        hi = v.partition_point(|&t| t < to).max(lo);
        // Ticks are time-ordered within a contract; a range cut that a roll could straddle is still
        // exact because each contract's rows are contiguous and sorted.
    }
    if options.max_rows > 0 {
        hi = hi.min(lo + options.max_rows.saturating_sub(already));
    }
    let n = hi - lo;
    let time_us = ts.slice(lo, n);
    let price = px.slice(lo, n);
    let mut buy_volume = Vec::with_capacity(n);
    let mut sell_volume = Vec::with_capacity(n);
    for i in lo..hi {
        let p = px.value(i);
        let ticks = p / TICK_SIZE;
        if !(p > 0.0 && (ticks - ticks.round()).abs() < 1e-6) {
            return Err(format!("trade price {p} at row {} is not on the {TICK_SIZE} tick grid", already + i));
        }
        let (b, a) = (bid.value(i), ask.value(i));
        let conv = |v: i64| u32::try_from(v).map_err(|_| format!("volume {v} at row {} is negative or too large", already + i));
        sell_volume.push(conv(b)?);
        buy_volume.push(conv(a)?);
    }
    let mut runs = Vec::new();
    let tv = time_us.values();
    for (k, i) in (lo..hi).enumerate() {
        let c = contract[i];
        let new_segment = state.last_time.is_none_or(|last| tv[k] < last) || c != state.contract;
        if k == 0 || new_segment {
            runs.push(Run { start: k, contract: c.to_string(), new_segment });
            if new_segment {
                state.contract = c.to_string();
            }
        }
        state.last_time = Some(tv[k]);
    }
    Ok(TickChunk { time_us, price, buy_volume, sell_volume, runs })
}

#[cfg(test)]
mod tests {
    #![allow(clippy::unwrap_used)]
    use super::*;
    use arrow_array::ArrayRef;
    use parquet::arrow::ArrowWriter;
    use parquet::file::properties::WriterProperties;
    use std::sync::Arc;

    /// Writes ticks with 2-row row groups, so row-group skipping is observable.
    fn write(path: &Path, ts: Vec<i64>, px: Vec<f64>, bid: Vec<i64>, ask: Vec<i64>, contract: Vec<&str>) {
        let cols: Vec<(&str, ArrayRef)> = vec![
            ("timestamp_us", Arc::new(Int64Array::from(ts))),
            ("trade_price", Arc::new(Float64Array::from(px))),
            ("bid_volume", Arc::new(Int64Array::from(bid))),
            ("ask_volume", Arc::new(Int64Array::from(ask))),
            ("contract", Arc::new(StringArray::from(contract))),
        ];
        let batch = RecordBatch::try_from_iter(cols).unwrap();
        let props = WriterProperties::builder().set_max_row_group_row_count(Some(2)).build();
        let mut w = ArrowWriter::try_new(File::create(path).unwrap(), batch.schema(), Some(props)).unwrap();
        w.write(&batch).unwrap();
        w.close().unwrap();
    }

    fn tmp(name: &str) -> PathBuf {
        std::env::temp_dir().join(format!("tools_ticks_{name}_{}.parquet", std::process::id()))
    }

    fn collect(path: &Path, options: StreamOptions) -> (Vec<i64>, Vec<u32>, Vec<u32>, Vec<Run>, usize) {
        let stream = open(path, options).unwrap();
        let read = stream.row_groups_read;
        let (mut t, mut buy, mut sell, mut runs) = (vec![], vec![], vec![], vec![]);
        for chunk in stream {
            let chunk = chunk.unwrap();
            for k in 0..chunk.runs.len() {
                let cols = chunk.run_trades(k);
                t.extend_from_slice(cols.time_us);
                buy.extend_from_slice(cols.buy_volume);
                sell.extend_from_slice(cols.sell_volume);
            }
            runs.extend(chunk.runs.iter().cloned());
        }
        (t, buy, sell, runs, read)
    }

    #[test]
    fn streams_runs_split_at_rolls_with_aggressor_sides_mapped() {
        let p = tmp("runs");
        write(&p, vec![1, 2, 3, 10, 11, 12], vec![100.0, 100.25, 100.5, 200.0, 200.25, 200.0], vec![0, 2, 0, 1, 0, 0], vec![1, 0, 3, 0, 4, 2], vec!["U23", "U23", "U23", "Z23", "Z23", "Z23"]);
        let (t, buy, sell, runs, read) = collect(&p, StreamOptions::default());
        assert_eq!(t, vec![1, 2, 3, 10, 11, 12]);
        assert_eq!(buy, vec![1, 0, 3, 0, 4, 2], "ask volume = buyer aggressor");
        assert_eq!(sell, vec![0, 2, 0, 1, 0, 0], "bid volume = seller aggressor");
        assert_eq!(read, 3);
        let starts: Vec<(String, bool)> = runs.iter().map(|r| (r.contract.clone(), r.new_segment)).collect();
        assert_eq!(starts, vec![("U23".into(), true), ("Z23".into(), true)]);
        std::fs::remove_file(&p).unwrap();
    }

    #[test]
    fn time_range_skips_row_groups_and_trims_rows_and_max_rows_caps() {
        let p = tmp("range");
        write(&p, (0..8).map(|i| i * 10).collect(), vec![50.0; 8], vec![0; 8], vec![1; 8], vec!["H24"; 8]);
        let (t, _, _, runs, read) = collect(&p, StreamOptions { time_range: Some((25, 55)), max_rows: 0 });
        assert_eq!(t, vec![30, 40, 50]);
        assert_eq!(read, 2, "row groups [20,30] and [40,50]; [0,10] and [60,70] are skipped");
        assert!(runs[0].new_segment);
        let (t, ..) = collect(&p, StreamOptions { time_range: None, max_rows: 3 });
        assert_eq!(t, vec![0, 10, 20]);
        std::fs::remove_file(&p).unwrap();
    }

    #[test]
    fn backwards_time_starts_a_new_segment_and_off_grid_prices_are_refused() {
        let p = tmp("back");
        write(&p, vec![5, 6, 1, 2], vec![10.0, 10.25, 10.0, 10.5], vec![0; 4], vec![1; 4], vec!["M24"; 4]);
        let (_, _, _, runs, _) = collect(&p, StreamOptions::default());
        // Row groups are [5,6] and [1,2]: the second chunk starts a new segment (time went back).
        assert_eq!(runs.iter().map(|r| r.new_segment).collect::<Vec<_>>(), vec![true, true]);
        let q = tmp("grid");
        write(&q, vec![1, 2], vec![10.0, 10.1], vec![0, 0], vec![1, 1], vec!["M24"; 2]);
        let err = open(&q, StreamOptions::default()).unwrap().find_map(Result::err).unwrap();
        assert!(err.contains("not on the 0.25 tick grid"), "{err}");
        std::fs::remove_file(&p).unwrap();
        std::fs::remove_file(&q).unwrap();
    }
}
