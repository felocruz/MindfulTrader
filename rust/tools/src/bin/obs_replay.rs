//! obs_replay -- streams the raw tick file through the Rust observation engine (spec §6.2,
//! decision #43). The engine builds every clock from the trades (time bars, activity bars) and fires
//! the regime model's steps (spec §6.2.8 D4: N trades OR V contracts OR a time floor); this tool
//! reports what it built and how fast. It becomes the training-data generator that replaces
//! `tools/market_data_replay` once the dims are built on the engine.
//!
//! The step amounts come from a theoretical rule not yet ruled, so they are command-line settings
//! (trade and volume criteria off unless given); the counts reported are a check that the machinery
//! runs at full scale, not a tuning exercise.
//!
//! A contract roll in the file starts a new segment: the engine is reset, so no bar, window, return
//! or step spans two contracts.
//!
//! `cargo run --release -p tools --bin obs_replay -- --help`

use std::path::{Path, PathBuf};
use std::time::Instant;

use clap::Parser;
use observation_engine::clock::{SessionCalendar, Timeframe, utc_midnight_us};
use observation_engine::engine::{Engine, EngineConfig, ResetReason, StepReason, StepTrigger};
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
    /// Step after this many trades (omit = criterion off).
    #[arg(long)]
    step_trades: Option<u32>,
    /// Step after this many contracts (omit = criterion off).
    #[arg(long)]
    step_volume: Option<u64>,
    /// Longest time between steps, seconds.
    #[arg(long, default_value_t = 60.0)]
    step_floor_seconds: f64,
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
    /// Steps by reason: trades, volume, floor.
    steps: [u64; 3],
}

fn run(cli: &Cli) -> Result<(), String> {
    let input = resolve(&cli.input);
    let time_range = match (&cli.from, &cli.to) {
        (None, None) => None,
        (from, to) => Some((from.as_deref().map(parse_date).transpose()?.unwrap_or(i64::MIN), to.as_deref().map(parse_date).transpose()?.unwrap_or(i64::MAX))),
    };
    let trigger = StepTrigger {
        trades: cli.step_trades.unwrap_or(u32::MAX),
        volume: cli.step_volume.unwrap_or(u64::MAX),
        floor_us: (cli.step_floor_seconds * 1e6).round() as i64,
    };
    let scope = format!(
        "replay of {} (from {:?} to {:?}, max_rows {}, activity threshold {}, step trades {:?} / volume {:?} / floor {} s)",
        input.display(),
        cli.from,
        cli.to,
        cli.max_rows,
        cli.activity_threshold,
        cli.step_trades,
        cli.step_volume,
        cli.step_floor_seconds
    );
    let mut r = Report::open(&repo_root(), "obs_replay", &scope).map_err(|e| format!("cannot open the report: {e}"))?;
    r.line(format!("obs_replay -- {scope}"));

    let calendar = SessionCalendar::default();
    let config = EngineConfig { calendar, activity_threshold: cli.activity_threshold, step_trigger: trigger };
    let mut engine = Engine::new(config).map_err(|e| format!("engine config: {e}"))?;
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
                segments.push(Segment { contract: run.contract.clone(), ..Segment::default() });
            }
            let stats = engine.on_trades(&chunk.run_trades(k)).map_err(|e| format!("engine: {e}"))?;
            if let Some(seg) = segments.last_mut() {
                seg.trades += stats.trades as u64;
                for (total, n) in seg.bars.iter_mut().zip(stats.closed_bars) {
                    *total += n as u64;
                }
                seg.activity += stats.activity_bars as u64;
                for step in engine.drain_steps() {
                    let i = match step.reason {
                        StepReason::Trades => 0,
                        StepReason::Volume => 1,
                        StepReason::Floor => 2,
                    };
                    seg.steps[i] += 1;
                }
            }
        }
    }
    let secs = t0.elapsed().as_secs_f64();

    r.line("segment  contract       trades   TS1 bars   TS2 bars   TS3 bars  activity bars  steps:trades   volume    floor");
    let mut total = Segment { contract: "total".into(), ..Segment::default() };
    for (i, s) in segments.iter().enumerate() {
        r.line(format!(
            "{i:7}  {:8} {:12} {:10} {:10} {:10} {:14} {:13} {:8} {:8}",
            s.contract, s.trades, s.bars[0], s.bars[1], s.bars[2], s.activity, s.steps[0], s.steps[1], s.steps[2]
        ));
        total.trades += s.trades;
        for k in 0..3 {
            total.bars[k] += s.bars[k];
            total.steps[k] += s.steps[k];
        }
        total.activity += s.activity;
    }
    r.line(format!(
        "  total  {:8} {:12} {:10} {:10} {:10} {:14} {:13} {:8} {:8}",
        "", total.trades, total.bars[0], total.bars[1], total.bars[2], total.activity, total.steps[0], total.steps[1], total.steps[2]
    ));
    r.line("bars close when their span ends; the last bar of each segment and the floor steps after its last trade are not closed (no time advance past the last trade)");
    let tfs = Timeframe::ALL.map(|tf| format!("{:?} {} min", tf, tf.minutes()));
    r.line(format!("timeframes: {}", tfs.join(", ")));
    r.line(format!("elapsed {secs:.1}s -- {:.1} M trades/s (decode thread pipelined with the engine)", total.trades as f64 / secs.max(1e-9) / 1e6));
    let archive = r.archive_path().to_string();
    r.finish().map_err(|e| format!("cannot finish the report: {e}"))?;
    println!("archived: {archive}");
    Ok(())
}
