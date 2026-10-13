//! Offline time bars: the trade stream turned into the same `BarColumns` view Sierra Chart lends the
//! live engine (spec §6.2.3 `TickBarBuilder`), so both feeders hand the engine one shape.
//!
//! A bar is the session-anchored span of its timeframe (`clock::SessionCalendar::bar_span`). Like
//! Sierra, a bar exists only if it has a trade, and a bar CLOSES when the first trade outside its span
//! arrives -- that trade opens the next bar. The builder holds the closed bars not yet handed to the
//! engine plus the forming bar, as columns; `drain_closed` keeps the allocation.

use crate::clock::{BarSpan, EventTime, SessionCalendar, Timeframe};
use crate::input::{BarColumns, Trade};

#[derive(Debug, Clone)]
pub struct TimeBarBuilder {
    timeframe: Timeframe,
    calendar: SessionCalendar,
    time_us: Vec<i64>,
    open: Vec<f32>,
    high: Vec<f32>,
    low: Vec<f32>,
    close: Vec<f32>,
    volume: Vec<f32>,
    bid_volume: Vec<f32>,
    ask_volume: Vec<f32>,
    num_trades: Vec<f32>,
    /// The forming bar's span, if one is forming (it is then the last row).
    forming: Option<BarSpan>,
}

impl TimeBarBuilder {
    pub fn new(timeframe: Timeframe, calendar: SessionCalendar) -> Self {
        Self {
            timeframe,
            calendar,
            time_us: Vec::new(),
            open: Vec::new(),
            high: Vec::new(),
            low: Vec::new(),
            close: Vec::new(),
            volume: Vec::new(),
            bid_volume: Vec::new(),
            ask_volume: Vec::new(),
            num_trades: Vec::new(),
            forming: None,
        }
    }

    pub fn timeframe(&self) -> Timeframe {
        self.timeframe
    }

    /// Would a trade at `t` close the forming bar?
    #[inline]
    pub fn closes_at(&self, t: EventTime) -> bool {
        self.forming.is_some_and(|span| !span.contains(t))
    }

    /// Ends the forming bar (it becomes the newest closed bar). No-op if none is forming.
    pub fn close_forming(&mut self) {
        self.forming = None;
    }

    /// Adds a trade to the forming bar, opening a new bar if none is forming. The caller closes the
    /// forming bar first when `closes_at(trade.time)` (the engine's event order: a bar closed by a
    /// trade is processed before that trade).
    pub fn apply(&mut self, trade: &Trade) {
        let p = trade.price as f32;
        let (bv, av) = (trade.sell_volume as f32, trade.buy_volume as f32);
        match self.forming {
            Some(span) if span.contains(trade.time) => {
                let i = self.time_us.len() - 1;
                self.high[i] = self.high[i].max(p);
                self.low[i] = self.low[i].min(p);
                self.close[i] = p;
                self.volume[i] += bv + av;
                self.bid_volume[i] += bv;
                self.ask_volume[i] += av;
                self.num_trades[i] += 1.0;
            }
            _ => {
                let span = self.calendar.bar_span(self.timeframe, trade.time);
                self.time_us.push(span.start.0);
                self.open.push(p);
                self.high.push(p);
                self.low.push(p);
                self.close.push(p);
                self.volume.push(bv + av);
                self.bid_volume.push(bv);
                self.ask_volume.push(av);
                self.num_trades.push(1.0);
                self.forming = Some(span);
            }
        }
    }

    /// Closed bars not yet drained, then the forming bar (if any). `live_index()` is the forming
    /// bar's index, or `len()` when none is forming.
    pub fn view(&self) -> BarColumns<'_> {
        BarColumns {
            time_us: &self.time_us,
            open: &self.open,
            high: &self.high,
            low: &self.low,
            close: &self.close,
            volume: &self.volume,
            bid_volume: &self.bid_volume,
            ask_volume: &self.ask_volume,
            num_trades: &self.num_trades,
        }
    }

    pub fn live_index(&self) -> usize {
        if self.forming.is_some() { self.time_us.len() - 1 } else { self.time_us.len() }
    }

    /// Forgets the closed bars (already handed to the engine), keeping the forming one.
    pub fn drain_closed(&mut self) {
        let keep = usize::from(self.forming.is_some());
        let cut = self.time_us.len() - keep;
        self.time_us.drain(..cut);
        self.open.drain(..cut);
        self.high.drain(..cut);
        self.low.drain(..cut);
        self.close.drain(..cut);
        self.volume.drain(..cut);
        self.bid_volume.drain(..cut);
        self.ask_volume.drain(..cut);
        self.num_trades.drain(..cut);
    }

    /// Drops everything (a reset).
    pub fn clear(&mut self) {
        self.time_us.clear();
        self.open.clear();
        self.high.clear();
        self.low.clear();
        self.close.clear();
        self.volume.clear();
        self.bid_volume.clear();
        self.ask_volume.clear();
        self.num_trades.clear();
        self.forming = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::clock::US_PER_MINUTE;

    /// 2024-07-15 14:00 UTC (10:00 ET), a Monday.
    const T0: i64 = 1_721_052_000_000_000;

    fn trade(minute: i64, price: f64, buy: u32, sell: u32) -> Trade {
        Trade { time: EventTime(T0 + minute * US_PER_MINUTE), price, buy_volume: buy, sell_volume: sell }
    }

    #[test]
    fn builds_ohlcv_and_closes_on_the_first_trade_outside_the_span() {
        let mut b = TimeBarBuilder::new(Timeframe::Ts3, SessionCalendar::default());
        let trades = [trade(1, 100.0, 2, 0), trade(5, 101.0, 0, 3), trade(9, 99.5, 1, 0), trade(14, 100.25, 0, 1), trade(15, 100.5, 4, 0)];
        for t in &trades[..4] {
            assert!(!b.closes_at(t.time));
            b.apply(t);
        }
        assert_eq!(b.live_index(), 0);
        assert!(b.closes_at(trades[4].time)); // minute 15 starts the next quarter hour
        b.close_forming();
        let v = b.view();
        assert_eq!((v.len(), b.live_index()), (1, 1));
        let bar = v.bar(0);
        assert_eq!(bar.start, EventTime(T0));
        let ohlc = [bar.open, bar.high, bar.low, bar.close].map(f32::to_bits);
        assert_eq!(ohlc, [100.0f32, 101.0, 99.5, 100.25].map(f32::to_bits));
        let vols = [bar.volume, bar.bid_volume, bar.ask_volume, bar.num_trades].map(f32::to_bits);
        assert_eq!(vols, [7.0f32, 4.0, 3.0, 4.0].map(f32::to_bits));
        b.drain_closed();
        assert!(b.view().is_empty());
        b.apply(&trades[4]);
        assert_eq!((b.view().len(), b.live_index()), (1, 0));
        assert_eq!(b.view().bar(0).start, EventTime(T0 + 15 * US_PER_MINUTE));
    }

    #[test]
    fn no_bar_without_a_trade_and_drain_keeps_the_forming_bar() {
        let mut b = TimeBarBuilder::new(Timeframe::Ts3, SessionCalendar::default());
        b.apply(&trade(0, 10.0, 1, 0));
        // Next trade 50 minutes later: the empty quarters in between produce no bars.
        let next = trade(50, 11.0, 1, 0);
        assert!(b.closes_at(next.time));
        b.close_forming();
        b.apply(&next);
        let v = b.view();
        assert_eq!(v.time_us, &[T0, T0 + 45 * US_PER_MINUTE]);
        assert_eq!(b.live_index(), 1);
        b.drain_closed();
        assert_eq!(b.view().time_us, &[T0 + 45 * US_PER_MINUTE]);
        assert_eq!(b.live_index(), 0);
        b.clear();
        assert!(b.view().is_empty());
        assert_eq!(b.live_index(), 0);
    }
}
