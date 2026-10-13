//! The engine: one value owns all state (spec §6.2.2 principle 5). This is the ingestion core --
//! events in, per-clock windows kept -- on which the dims, the scaler and the gate are built.
//!
//! **Event order** (the rule both feeders follow, so live and offline see one sequence): a bar
//! closed by a trade is processed BEFORE that trade. Live, Sierra creates the new bar with the
//! trade that opens it, so the adapter calls `on_bars` (which sees the newly closed bar) and then
//! `on_trades`; offline, `OfflineFeeder` splits the trade stream at every bar close.
//!
//! **Time** only moves forward: a trade older than the last one, or closed bars older than the last
//! closed bar of their timeframe, is an error ([`EngineError::TimeWentBackwards`]); the caller resets
//! (`ResetReason::TimeWentBackwards`) and replays. Never absorbed silently (spec §6.2.5).

use crate::activity::{ActivityBar, ImbalanceBarBuilder};
use crate::clock::{EventTime, SessionCalendar, Timeframe};
use crate::input::{Bar, BarColumns, InputError, Trade, TradeColumns};
use crate::ring::WindowRing;

/// Closed bars kept per timeframe (the longest window any estimator reads, plus slack).
pub const BAR_WINDOW: usize = 512;
/// Closed activity bars kept.
pub const ACTIVITY_WINDOW: usize = 512;

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct EngineConfig {
    pub calendar: SessionCalendar,
    /// Activity-clock imbalance threshold, contracts of signed aggressor volume.
    pub activity_threshold: u64,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum ResetReason {
    /// Sierra full recalculation (chart load, settings or symbol change, replay start).
    Recalculation,
    /// An event older than the last one arrived.
    TimeWentBackwards,
    /// A new contract (offline: a roll in the tick file).
    NewContract,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum EngineError {
    Input(InputError),
    /// `live_index` beyond the bars passed.
    LiveIndexOutOfRange { live_index: usize, len: usize },
    /// Closed bars not in strictly increasing start time.
    BarsOutOfOrder { timeframe: Timeframe, index: usize },
    TimeWentBackwards { last: EventTime, got: EventTime },
}

impl std::fmt::Display for EngineError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            EngineError::Input(e) => write!(f, "{e}"),
            EngineError::LiveIndexOutOfRange { live_index, len } => write!(f, "live index {live_index} beyond {len} bars"),
            EngineError::BarsOutOfOrder { timeframe, index } => write!(f, "{timeframe:?} bar {index} does not start after the bar before it"),
            EngineError::TimeWentBackwards { last, got } => write!(f, "time went backwards: last {} us, got {} us", last.0, got.0),
        }
    }
}

impl std::error::Error for EngineError {}

impl From<InputError> for EngineError {
    fn from(e: InputError) -> Self {
        EngineError::Input(e)
    }
}

/// The closed bars of one timeframe, as rolling columns.
#[derive(Debug, Clone, Default)]
pub struct BarStore {
    pub time_us: WindowRing<i64, BAR_WINDOW>,
    pub open: WindowRing<f32, BAR_WINDOW>,
    pub high: WindowRing<f32, BAR_WINDOW>,
    pub low: WindowRing<f32, BAR_WINDOW>,
    pub close: WindowRing<f32, BAR_WINDOW>,
    pub volume: WindowRing<f32, BAR_WINDOW>,
    pub bid_volume: WindowRing<f32, BAR_WINDOW>,
    pub ask_volume: WindowRing<f32, BAR_WINDOW>,
    pub num_trades: WindowRing<f32, BAR_WINDOW>,
    /// Closed bars seen since the last reset (not capped by the window).
    pub closed_count: u64,
    /// The forming bar as last reported (informational: dims use closed bars, B23 rule 1).
    pub forming: Option<Bar>,
    /// The engine's trade count when the newest bar was taken (the event-order record).
    pub trades_at_last_close: Option<u64>,
}

impl BarStore {
    fn push(&mut self, b: &Bar) {
        self.time_us.push(b.start.0);
        self.open.push(b.open);
        self.high.push(b.high);
        self.low.push(b.low);
        self.close.push(b.close);
        self.volume.push(b.volume);
        self.bid_volume.push(b.bid_volume);
        self.ask_volume.push(b.ask_volume);
        self.num_trades.push(b.num_trades);
        self.closed_count += 1;
    }

    pub fn last_start(&self) -> Option<EventTime> {
        self.time_us.newest().map(EventTime)
    }

    fn clear(&mut self) {
        *self = BarStore::default();
    }
}

/// Closed activity bars, as rolling columns.
#[derive(Debug, Clone, Default)]
pub struct ActivityStore {
    pub first_time_us: WindowRing<i64, ACTIVITY_WINDOW>,
    pub last_time_us: WindowRing<i64, ACTIVITY_WINDOW>,
    pub open: WindowRing<f64, ACTIVITY_WINDOW>,
    pub high: WindowRing<f64, ACTIVITY_WINDOW>,
    pub low: WindowRing<f64, ACTIVITY_WINDOW>,
    pub close: WindowRing<f64, ACTIVITY_WINDOW>,
    pub buy_volume: WindowRing<u64, ACTIVITY_WINDOW>,
    pub sell_volume: WindowRing<u64, ACTIVITY_WINDOW>,
    pub trades: WindowRing<u32, ACTIVITY_WINDOW>,
    pub imbalance: WindowRing<i64, ACTIVITY_WINDOW>,
    pub closed_count: u64,
}

impl ActivityStore {
    fn push(&mut self, b: &ActivityBar) {
        self.first_time_us.push(b.first_time.0);
        self.last_time_us.push(b.last_time.0);
        self.open.push(b.open);
        self.high.push(b.high);
        self.low.push(b.low);
        self.close.push(b.close);
        self.buy_volume.push(b.buy_volume);
        self.sell_volume.push(b.sell_volume);
        self.trades.push(b.trades);
        self.imbalance.push(b.imbalance);
        self.closed_count += 1;
    }

    fn clear(&mut self) {
        *self = ActivityStore::default();
    }
}

/// What one `on_bars` call added.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BarUpdate {
    pub timeframe: Timeframe,
    pub new_closed: usize,
}

#[derive(Debug, Clone)]
pub struct Engine {
    config: EngineConfig,
    last_trade_time: Option<EventTime>,
    trades_seen: u64,
    bars: [BarStore; 3],
    activity_builder: ImbalanceBarBuilder,
    activity: ActivityStore,
}

impl Engine {
    pub fn new(config: EngineConfig) -> Self {
        Self {
            config,
            last_trade_time: None,
            trades_seen: 0,
            bars: Default::default(),
            activity_builder: ImbalanceBarBuilder::new(config.activity_threshold),
            activity: ActivityStore::default(),
        }
    }

    pub fn config(&self) -> &EngineConfig {
        &self.config
    }

    pub fn trades_seen(&self) -> u64 {
        self.trades_seen
    }

    pub fn bars(&self, timeframe: Timeframe) -> &BarStore {
        &self.bars[timeframe.index()]
    }

    pub fn activity(&self) -> &ActivityStore {
        &self.activity
    }

    /// Drops all state; the caller then replays history.
    pub fn reset(&mut self, _reason: ResetReason) {
        self.last_trade_time = None;
        self.trades_seen = 0;
        for b in &mut self.bars {
            b.clear();
        }
        self.activity_builder.clear();
        self.activity.clear();
    }

    /// One trade. Returns the activity bar it closed, if any.
    #[inline]
    pub fn on_trade(&mut self, t: &Trade) -> Result<Option<ActivityBar>, EngineError> {
        if let Some(last) = self.last_trade_time
            && t.time < last
        {
            return Err(EngineError::TimeWentBackwards { last, got: t.time });
        }
        self.last_trade_time = Some(t.time);
        self.trades_seen += 1;
        let closed = self.activity_builder.on_trade(t);
        if let Some(bar) = &closed {
            self.activity.push(bar);
        }
        Ok(closed)
    }

    /// A batch of trades (live: a time-and-sales slice; offline: a decoded chunk). Returns the
    /// number of activity bars closed.
    pub fn on_trades(&mut self, trades: &TradeColumns<'_>) -> Result<usize, EngineError> {
        let mut closed = 0;
        for t in trades.iter() {
            closed += usize::from(self.on_trade(&t)?.is_some());
        }
        Ok(closed)
    }

    /// The bars of one timeframe: `0..live_index` closed, `live_index` (if < len) forming. Only
    /// closed bars newer than the last one seen are taken, so the live adapter can pass Sierra's whole
    /// arrays every call and a recalculation's full history arrives as one call.
    pub fn on_bars(&mut self, timeframe: Timeframe, bars: &BarColumns<'_>, live_index: usize) -> Result<BarUpdate, EngineError> {
        let len = bars.validate()?;
        if live_index > len {
            return Err(EngineError::LiveIndexOutOfRange { live_index, len });
        }
        let trades_seen = self.trades_seen;
        let store = &mut self.bars[timeframe.index()];
        let closed = &bars.time_us[..live_index];
        if let Some(i) = closed.windows(2).position(|w| w[1] <= w[0]) {
            return Err(EngineError::BarsOutOfOrder { timeframe, index: i + 1 });
        }
        let first_new = match store.last_start() {
            Some(last) => {
                if let Some(&newest) = closed.last()
                    && newest < last.0
                {
                    return Err(EngineError::TimeWentBackwards { last, got: EventTime(newest) });
                }
                closed.partition_point(|&t| t <= last.0)
            }
            None => 0,
        };
        for i in first_new..live_index {
            store.push(&bars.bar(i));
        }
        if live_index > first_new {
            store.trades_at_last_close = Some(trades_seen);
        }
        store.forming = (live_index < len).then(|| bars.bar(live_index));
        Ok(BarUpdate { timeframe, new_closed: live_index - first_new })
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn config() -> EngineConfig {
        EngineConfig { calendar: SessionCalendar::default(), activity_threshold: 5 }
    }

    struct Cols {
        t: Vec<i64>,
        f: Vec<f32>,
    }

    impl Cols {
        fn new(times: &[i64]) -> Self {
            Self { t: times.to_vec(), f: times.iter().map(|&x| x as f32).collect() }
        }
        fn view(&self) -> BarColumns<'_> {
            let f = &self.f;
            BarColumns { time_us: &self.t, open: f, high: f, low: f, close: f, volume: f, bid_volume: f, ask_volume: f, num_trades: f }
        }
    }

    #[test]
    fn on_bars_takes_only_new_closed_bars_when_given_the_whole_history_each_call() {
        let mut e = Engine::new(config());
        let c = Cols::new(&[10, 20, 30]);
        // Two closed, one forming.
        assert_eq!(e.on_bars(Timeframe::Ts3, &c.view(), 2), Ok(BarUpdate { timeframe: Timeframe::Ts3, new_closed: 2 }));
        assert_eq!(e.bars(Timeframe::Ts3).forming.map(|b| b.start), Some(EventTime(30)));
        // Same arrays again (a forming-bar update): nothing new.
        assert_eq!(e.on_bars(Timeframe::Ts3, &c.view(), 2).map(|u| u.new_closed), Ok(0));
        // A bar closed and another forms.
        let c2 = Cols::new(&[10, 20, 30, 40]);
        assert_eq!(e.on_bars(Timeframe::Ts3, &c2.view(), 3).map(|u| u.new_closed), Ok(1));
        let s = e.bars(Timeframe::Ts3);
        assert_eq!((s.closed_count, s.time_us.as_slice()), (3, &[10i64, 20, 30][..]));
        assert_eq!(e.bars(Timeframe::Ts1).closed_count, 0);
    }

    #[test]
    fn on_bars_rejects_bad_input_and_backwards_time() {
        let mut e = Engine::new(config());
        let c = Cols::new(&[10, 20, 30]);
        assert_eq!(e.on_bars(Timeframe::Ts2, &c.view(), 4), Err(EngineError::LiveIndexOutOfRange { live_index: 4, len: 3 }));
        let unordered = Cols::new(&[10, 30, 20]);
        assert_eq!(e.on_bars(Timeframe::Ts2, &unordered.view(), 3), Err(EngineError::BarsOutOfOrder { timeframe: Timeframe::Ts2, index: 2 }));
        e.on_bars(Timeframe::Ts2, &c.view(), 3).expect("valid");
        let older = Cols::new(&[1, 2]);
        assert_eq!(e.on_bars(Timeframe::Ts2, &older.view(), 2), Err(EngineError::TimeWentBackwards { last: EventTime(30), got: EventTime(2) }));
        e.reset(ResetReason::TimeWentBackwards);
        assert_eq!(e.on_bars(Timeframe::Ts2, &older.view(), 2).map(|u| u.new_closed), Ok(2));
    }

    #[test]
    fn trades_feed_the_activity_clock_and_time_only_moves_forward() {
        let mut e = Engine::new(config());
        let t = [1i64, 2, 2, 3];
        let p = [100.0, 100.25, 100.5, 100.25];
        let buy = [3u32, 2, 0, 1];
        let sell = [0u32, 0, 1, 0];
        let cols = TradeColumns::new(&t, &p, &buy, &sell).expect("equal lengths");
        assert_eq!(e.on_trades(&cols), Ok(1)); // +3, +5 -> closes on the 2nd trade
        assert_eq!(e.trades_seen(), 4);
        assert_eq!(e.activity().closed_count, 1);
        assert_eq!(e.activity().imbalance.as_slice(), &[5]);
        let late = Trade { time: EventTime(2), price: 1.0, buy_volume: 1, sell_volume: 0 };
        assert_eq!(e.on_trade(&late), Err(EngineError::TimeWentBackwards { last: EventTime(3), got: EventTime(2) }));
        e.reset(ResetReason::NewContract);
        assert_eq!((e.trades_seen(), e.activity().closed_count), (0, 0));
        assert!(e.on_trade(&late).is_ok());
    }
}
