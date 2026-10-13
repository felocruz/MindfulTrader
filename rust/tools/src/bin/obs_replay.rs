//! obs_replay -- streams the raw tick file through the Rust observation engine (spec §6.2,
//! decision #43). Today it runs the foundation (session-anchored time bars, the activity clock,
//! the engine's event order) and reports what it built and how fast; it becomes the
//! `.context.parquet` generator that replaces `tools/market_data_replay` once the dims, the frame and
//! the emission are built on the engine.
//!
//! A contract roll in the file starts a new segment: the engine and feeder are reset, so no bar,
//! window or return spans two contracts.
//!
//! `cargo run --release -p tools --bin obs_replay -- --help`

use std::path::{Path, PathBuf};
use std::time::Instant;

use clap::Parser;
use observation_engine::clock::{SessionCalendar, Timeframe, utc_midnight_us};
use observation_engine::engine::{Engine, EngineConfig, ResetReason};
use observation_engine::offline::OfflineFeeder;
use tools::report::Report;
use tools::tick_stream::{self, StreamOptions};

#[derive(Parser)]
#[command(name = "obs_replay", about = "Stream raw ticks through the Rust observation engine (MindfulTrader)")]
struct Cli {
    /// Raw tick file (scid_to_ticks_parquet.cpp's output).
    #[arg(long, default_value = "../lbrnet/data/raw/mes_ticks.parquet")]
    input: PathBuf,
    /// First UTC date to replay, YYYY-MM-DD (row groups before it are not read).
    #[arg(long)]
    from: Option<String>,
    /// UTC date to stop before, YYYY-MM-DD.
    #[arg(long)]
    to: Option<String>,
    /// Stop after N trades (0 = all) -- for smoke runs.
    #[arg(long, default_value_t = 0)]
    max_rows: usize,
    /// Activity-clock imbalance threshold, contracts (50 = today's production value).
    #[arg(long, default_value_t = 50)]
    activity_threshold: u64,
}

fn repo_root() -> PathBuf {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..");
    root.canonicalize().unwrap_or(root)
}

fn resolve(p: &Path) -> PathBuf {
    let path = if p.is_absolute() { p.to_path_buf() } else { repo_root().join(p) };
    path.canonicalize().unwrap_or(path)
}

fn parse_date(s: &str) -> Result<i64, String> {
    let parts: Vec<&str> = s.split('-').collect();
    let bad = || format!("'{s}' is not a YYYY-MM-DD date");
    if parts.len() != 3 {
        return Err(bad());
    }
    let y: i64 = parts[0].parse().map_err(|_| bad())?;
    let m: u32 = parts[1].parse().map_err(|_| bad())?;
    let d: u32 = parts[2].parse().map_err(|_| bad())?;
    if !(1..=12).contains(&m) || !(1..=31).contains(&d) {
        return Err(bad());
    }
    Ok(utc_midnight_us(y, m, d))
}

fn main() {
    let cli = Cli::parse();
    if let Err(e) = run(&cli) {
        eprintln!("obs_replay: {e}");
        std::process::exit(1);
    }
}

#[derive(Debug, Default)]
struct Segment {
    contract: String,
    trades: u64,
    bars: [u64; 3],
    activity: u64,
}

fn run(cli: &Cli) -> Result<(), String> {
    let input = resolve(&cli.input);
    let time_range = match (&cli.from, &cli.to) {
        (None, None) => None,
        (from, to) => Some((from.as_deref().map(parse_date).transpose()?.unwrap_or(i64::MIN), to.as_deref().map(parse_date).transpose()?.unwrap_or(i64::MAX))),
    };
    let scope = format!(
        "foundation replay of {} (from {:?} to {:?}, max_rows {}, activity threshold {})",
        input.display(),
        cli.from,
        cli.to,
        cli.max_rows,
        cli.activity_threshold
    );
    let mut r = Report::open(&repo_root(), "obs_replay", &scope).map_err(|e| format!("cannot open the report: {e}"))?;
    r.line(format!("obs_replay -- {scope}"));

    let calendar = SessionCalendar::default();
    let config = EngineConfig { calendar, activity_threshold: cli.activity_threshold };
    let mut engine = Engine::new(config);
    let mut feeder = OfflineFeeder::new(calendar);
    let t0 = Instant::now();
    let stream = tick_stream::open(&input, StreamOptions { time_range, max_rows: cli.max_rows })?;
    r.line(format!("row groups: reading {} of {}", stream.row_groups_read, stream.row_groups_total));

    let mut segments: Vec<Segment> = Vec::new();
    for chunk in stream {
        let chunk = chunk?;
        for k in 0..chunk.runs.len() {
            let run = &chunk.runs[k];
            if run.new_segment || segments.is_empty() {
                engine.reset(ResetReason::NewContract);
                feeder.clear();
                segments.push(Segment { contract: run.contract.clone(), ..Segment::default() });
            }
            let stats = feeder.feed(&mut engine, &chunk.run_trades(k)).map_err(|e| format!("engine: {e}"))?;
            if let Some(seg) = segments.last_mut() {
                seg.trades += stats.trades as u64;
                for (total, n) in seg.bars.iter_mut().zip(stats.closed_bars) {
                    *total += n as u64;
                }
                seg.activity += stats.activity_bars as u64;
            }
        }
    }
    let secs = t0.elapsed().as_secs_f64();

    r.line("segment  contract       trades   TS1 bars   TS2 bars   TS3 bars  activity bars");
    let mut total = Segment { contract: "total".into(), ..Segment::default() };
    for (i, s) in segments.iter().enumerate() {
        r.line(format!("{i:7}  {:8} {:12} {:10} {:10} {:10} {:14}", s.contract, s.trades, s.bars[0], s.bars[1], s.bars[2], s.activity));
        total.trades += s.trades;
        for k in 0..3 {
            total.bars[k] += s.bars[k];
        }
        total.activity += s.activity;
    }
    r.line(format!("  total  {:8} {:12} {:10} {:10} {:10} {:14}", "", total.trades, total.bars[0], total.bars[1], total.bars[2], total.activity));
    let tfs = Timeframe::ALL.map(|tf| format!("{:?} {} min", tf, tf.minutes()));
    r.line(format!("timeframes: {}", tfs.join(", ")));
    r.line(format!("elapsed {secs:.1}s -- {:.1} M trades/s (decode thread pipelined with the engine)", total.trades as f64 / secs.max(1e-9) / 1e6));
    let archive = r.archive_path().to_string();
    r.finish().map_err(|e| format!("cannot finish the report: {e}"))?;
    println!("archived: {archive}");
    Ok(())
}
