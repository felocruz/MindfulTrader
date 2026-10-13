//! Reads HMM training rows from a `.context.parquet` file (the offline replay generator's output,
//! e.g. `lbrnet/data/raw/offline_replay_full_20260918.context.parquet`): the model's input columns
//! by name, in model order (`student_t_hmm::input`), plus `timestamp_us`. Only those columns are
//! decoded. Refuses -- naming column and row -- a missing column, a null, or a non-finite value.

use std::fs::File;
use std::path::Path;

use arrow_array::{Array, Float32Array, Int64Array, RecordBatch};
use parquet::arrow::ProjectionMask;
use parquet::arrow::arrow_reader::ParquetRecordBatchReaderBuilder;

pub const TIMESTAMP_COLUMN: &str = "timestamp_us";

/// Rows in file order, with one timestamp per row.
#[derive(Debug)]
pub struct ContextRows<const D: usize> {
    pub rows: Vec<[f64; D]>,
    pub timestamps_us: Vec<i64>,
}

/// Reads `fields` (Float32 columns) and `timestamp_us` (Int64). `max_rows` > 0 keeps only the
/// first `max_rows` rows (a contiguous head, so sequences stay intact).
pub fn read<const D: usize>(path: &Path, fields: &[&str; D], max_rows: usize) -> Result<ContextRows<D>, String> {
    let file = File::open(path).map_err(|e| format!("cannot open {}: {e}", path.display()))?;
    let builder = ParquetRecordBatchReaderBuilder::try_new(file).map_err(|e| format!("not a parquet file: {e}"))?;
    let total = usize::try_from(builder.metadata().file_metadata().num_rows()).unwrap_or(0);
    let schema = builder.parquet_schema();
    let mut leaves = Vec::with_capacity(D + 1);
    for name in fields.iter().chain(std::iter::once(&TIMESTAMP_COLUMN)) {
        let idx = schema
            .columns()
            .iter()
            .position(|c| c.name() == *name)
            .ok_or_else(|| format!("column '{name}' is missing from {}", path.display()))?;
        leaves.push(idx);
    }
    let mask = ProjectionMask::leaves(schema, leaves);
    let reader = builder
        .with_projection(mask)
        .with_batch_size(1 << 16)
        .build()
        .map_err(|e| format!("cannot build the parquet reader: {e}"))?;

    let want = if max_rows > 0 { max_rows.min(total) } else { total };
    let mut out = ContextRows { rows: Vec::with_capacity(want), timestamps_us: Vec::with_capacity(want) };
    for batch in reader {
        let batch = batch.map_err(|e| format!("parquet read failed: {e}"))?;
        append_batch(&batch, fields, &mut out, want)?;
        if out.rows.len() >= want {
            break;
        }
    }
    Ok(out)
}

fn append_batch<const D: usize>(batch: &RecordBatch, fields: &[&str; D], out: &mut ContextRows<D>, want: usize) -> Result<(), String> {
    let base = out.rows.len();
    let n = batch.num_rows().min(want - base);
    let ts = batch
        .column_by_name(TIMESTAMP_COLUMN)
        .and_then(|c| c.as_any().downcast_ref::<Int64Array>())
        .ok_or_else(|| format!("column '{TIMESTAMP_COLUMN}' is not Int64"))?;
    let mut cols = Vec::with_capacity(D);
    for name in fields {
        let col = batch
            .column_by_name(name)
            .and_then(|c| c.as_any().downcast_ref::<Float32Array>())
            .ok_or_else(|| format!("column '{name}' is not Float32"))?;
        cols.push(col);
    }
    for i in 0..n {
        if ts.is_null(i) {
            return Err(format!("null in '{TIMESTAMP_COLUMN}' at row {}", base + i));
        }
        let mut row = [0.0f64; D];
        for (j, col) in cols.iter().enumerate() {
            let v = f64::from(col.value(i));
            if col.is_null(i) || !v.is_finite() {
                return Err(format!("null or non-finite value in '{}' at row {}; refusing to train", fields[j], base + i));
            }
            row[j] = v;
        }
        out.rows.push(row);
        out.timestamps_us.push(ts.value(i));
    }
    Ok(())
}

#[cfg(test)]
mod tests {
    #![allow(clippy::unwrap_used)]
    use super::*;
    use std::sync::Arc;

    use arrow_array::ArrayRef;
    use parquet::arrow::ArrowWriter;

    fn write(path: &Path, cols: Vec<(&str, ArrayRef)>) {
        let batch = RecordBatch::try_from_iter(cols).unwrap();
        let mut w = ArrowWriter::try_new(File::create(path).unwrap(), batch.schema(), None).unwrap();
        w.write(&batch).unwrap();
        w.close().unwrap();
    }

    fn tmp(name: &str) -> std::path::PathBuf {
        std::env::temp_dir().join(format!("tools_ctx_{name}_{}.parquet", std::process::id()))
    }

    #[test]
    fn reads_named_columns_in_the_requested_order_and_ignores_the_rest() {
        let p = tmp("order");
        write(
            &p,
            vec![
                ("b", Arc::new(Float32Array::from(vec![10.0, 20.0, 30.0])) as ArrayRef),
                ("unused", Arc::new(Float32Array::from(vec![f32::NAN; 3])) as ArrayRef),
                ("a", Arc::new(Float32Array::from(vec![1.0, 2.0, 3.0])) as ArrayRef),
                (TIMESTAMP_COLUMN, Arc::new(Int64Array::from(vec![7, 8, 9])) as ArrayRef),
            ],
        );
        let got = read(&p, &["a", "b"], 0).unwrap();
        assert_eq!(got.rows.len(), 3);
        assert_eq!(got.rows[1].map(f64::to_bits), [2.0f64.to_bits(), 20.0f64.to_bits()], "requested order, not file order");
        assert_eq!(got.timestamps_us, vec![7, 8, 9]);
        let head = read(&p, &["a", "b"], 2).unwrap();
        assert_eq!(head.timestamps_us, vec![7, 8], "max_rows keeps a contiguous head");
        std::fs::remove_file(&p).unwrap();
    }

    #[test]
    fn refuses_missing_columns_and_non_finite_values() {
        let p = tmp("bad");
        write(
            &p,
            vec![
                ("a", Arc::new(Float32Array::from(vec![1.0, f32::INFINITY])) as ArrayRef),
                (TIMESTAMP_COLUMN, Arc::new(Int64Array::from(vec![1, 2])) as ArrayRef),
            ],
        );
        assert!(read(&p, &["missing"], 0).unwrap_err().contains("'missing' is missing"));
        assert!(read(&p, &["a"], 0).unwrap_err().contains("'a' at row 1"));
        std::fs::remove_file(&p).unwrap();
    }
}
