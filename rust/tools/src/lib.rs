//! Shared pieces of the offline tools: the `.context.parquet` reader, the raw-tick stream and the archived report
//! (the Rust counterpart of `tools/ToolProgressLogger.h`).

pub mod context_parquet;
pub mod report;
pub mod tick_stream;
