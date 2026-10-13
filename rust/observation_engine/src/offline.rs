//! The offline feeder: a trade stream in, the engine fed exactly as live Sierra would feed it.
//!
//! It builds the three timeframes' bars from the trades (`TimeBarBuilder`) and splits each trade
//! batch at every bar close, so the engine sees the event order the live adapter produces (engine
//! module doc): trades up to the close, then the closed bars, then the trade that opened the new
//! bar. Batches are borrowed column slices end to end -- nothing is copied per trade.
//!
//! Forming-bar updates are not sent: no dim reads the forming bar (B23 rule 1), so offline skips the
//! per-trade bar views that live Sierra produces anyway.

use crate::bars::TimeBarBuilder;
use crate::clock::{SessionCalendar, Timeframe};
use crate::engine::{Engine, EngineError};
use crate::input::TradeColumns;

#[derive(Debug, Clone)]
pub struct OfflineFeeder {
    builders: [TimeBarBuilder; 3],
}

/// Counts from one `feed` call.
#[derive(Debug, Clone, Copy, Default, PartialEq, Eq)]
pub struct FeedStats {
    pub trades: usize,
    pub closed_bars: [usize; 3],
    pub activity_bars: usize,
}

impl OfflineFeeder {
    pub fn new(calendar: SessionCalendar) -> Self {
        Self { builders: Timeframe::ALL.map(|tf| TimeBarBuilder::new(tf, calendar)) }
    }

    /// Feeds one batch (time non-decreasing, one contract).
    pub fn feed(&mut self, engine: &mut Engine, trades: &TradeColumns<'_>) -> Result<FeedStats, EngineError> {
        let mut stats = FeedStats { trades: trades.len(), ..FeedStats::default() };
        let mut start = 0;
        for i in 0..trades.len() {
            let trade = trades.get(i);
            if self.builders.iter().any(|b| b.closes_at(trade.time)) {
                stats.activity_bars += engine.on_trades(&trades.slice(start..i))?;
                start = i;
                for b in &mut self.builders {
                    if b.closes_at(trade.time) {
                        b.close_forming();
                        let update = engine.on_bars(b.timeframe(), &b.view(), b.live_index())?;
                        stats.closed_bars[b.timeframe().index()] += update.new_closed;
                        b.drain_closed();
                    }
                }
            }
            for b in &mut self.builders {
                b.apply(&trade);
            }
        }
        stats.activity_bars += engine.on_trades(&trades.slice(start..trades.len()))?;
        Ok(stats)
    }

    /// Drops the forming bars (with an engine reset: a new contract, a recalculation).
    pub fn clear(&mut self) {
        for b in &mut self.builders {
            b.clear();
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::clock::{EventTime, US_PER_MINUTE};
    use crate::engine::EngineConfig;

    /// 2024-07-15 14:00 UTC (10:00 ET), a Monday.
    const T0: i64 = 1_721_052_000_000_000;

    #[test]
    fn bars_close_before_the_trade_that_opens_the_next_bar() {
        let mut engine = Engine::new(EngineConfig { calendar: SessionCalendar::default(), activity_threshold: 1_000_000 });
        let mut feeder = OfflineFeeder::new(SessionCalendar::default());
        // One trade a minute for 2 hours (10:00-12:00 ET), split into odd-sized batches.
        let n = 120usize;
        let t: Vec<i64> = (0..n as i64).map(|m| T0 + m * US_PER_MINUTE).collect();
        let p: Vec<f64> = (0..n).map(|i| 5000.0 + (i % 7) as f64 * 0.25).collect();
        let buy = vec![1u32; n];
        let sell = vec![0u32; n];
        let all = TradeColumns::new(&t, &p, &buy, &sell).expect("equal lengths");
        let mut total = FeedStats::default();
        for range in [0..7, 7..50, 50..51, 51..120] {
            let s = feeder.feed(&mut engine, &all.slice(range)).expect("feed");
            total.trades += s.trades;
            for k in 0..3 {
                total.closed_bars[k] += s.closed_bars[k];
            }
        }
        assert_eq!(total.trades, n);
        assert_eq!(engine.trades_seen(), n as u64);
        // 10:00-12:00 ET: 7 closed quarter hours (the 8th is forming), 1 closed hour, no 240-minute
        // bar closed (10:00-14:00 ET is still forming).
        assert_eq!(total.closed_bars, [0, 1, 7]);
        let ts3 = engine.bars(Timeframe::Ts3);
        assert_eq!(ts3.time_us.as_slice(), &(0..7).map(|k| T0 + k * 15 * US_PER_MINUTE).collect::<Vec<_>>()[..]);
        // Each closed quarter hour holds exactly its 15 trades -- none from the next bar leaked in.
        assert!(ts3.num_trades.as_slice().iter().all(|&x| x.to_bits() == 15.0f32.to_bits()));
        // The 7th quarter hour closed on trade 105 (10:105 min): the engine had seen 105 trades.
        assert_eq!(ts3.trades_at_last_close, Some(105));
        assert_eq!(engine.bars(Timeframe::Ts2).volume.as_slice().iter().map(|v| v.to_bits()).collect::<Vec<_>>(), vec![60.0f32.to_bits()]);
        assert_eq!(engine.bars(Timeframe::Ts3).last_start(), Some(EventTime(T0 + 6 * 15 * US_PER_MINUTE)));
    }

    #[test]
    fn when_a_bar_closes_the_engine_has_not_yet_seen_the_opening_trade() {
        // Trades at 10:14 and 10:15 ET: the 10:00 bar closes on the 10:15 trade. At that moment the
        // engine must have seen exactly 1 trade.
        let mut engine = Engine::new(EngineConfig { calendar: SessionCalendar::default(), activity_threshold: 1_000_000 });
        let mut feeder = OfflineFeeder::new(SessionCalendar::default());
        let t = [T0 + 14 * US_PER_MINUTE, T0 + 15 * US_PER_MINUTE];
        let p = [1.0, 2.0];
        let v = [1u32, 1];
        let z = [0u32, 0];
        let cols = TradeColumns::new(&t, &p, &v, &z).expect("equal lengths");
        feeder.feed(&mut engine, &cols.slice(0..1)).expect("feed");
        assert_eq!(engine.bars(Timeframe::Ts3).closed_count, 0);
        feeder.feed(&mut engine, &cols.slice(1..2)).expect("feed");
        assert_eq!(engine.bars(Timeframe::Ts3).closed_count, 1);
        assert_eq!(engine.bars(Timeframe::Ts3).trades_at_last_close, Some(1));
        assert_eq!(engine.bars(Timeframe::Ts3).close.as_slice().iter().map(|x| x.to_bits()).collect::<Vec<_>>(), vec![1.0f32.to_bits()]);
        assert_eq!(engine.trades_seen(), 2);
    }
}
