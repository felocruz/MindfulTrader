//! The engine: one value owns all state (spec §6.2.2 principle 5). Trades in -- live from the records
//! the adapter reads out of Sierra Chart's intraday file, offline from the tick file (§6.2.8 D2) --
//! and the engine builds every clock itself, with the same code in both (D3): the 240/60/15-minute
//! time bars, the imbalance (activity) bars, and the regime model's steps (D4 ruling 2).
//!
//! **Order.** Trades are processed in sequence order (the record's position in its stream); time
//! never decreases, and the sequence strictly increases. Many trades share a timestamp, so the
//! sequence -- not the time -- is the official order, identical live and offline.
//!
//! **Event order within a time advance.** Before a trade at time t is applied, everything that
//! happened before t is processed in time order: each time bar whose span ended at or before t closes
//! (a bar is complete when time reaches the end of its span), and each step floor that elapsed fires;
//! at equal times a bar close comes before a floor step, so an input built on that bar is new at that
//! step. Then the trade is applied (it may open a new bar, close an activity bar, and fire a trade or
//! volume step at its own time). `on_time` performs the same advance without a trade (live: a timer
//! call with no new records; offline: the end of a segment).
//!
//! **Steps.** A step fires when, since the previous step, `trades` trades OR `volume` contracts have
//! arrived (at the trade that reaches either) or `floor_us` of OPEN market time has elapsed with
//! neither (operator, 2026-10-14: the floor pauses while the market is closed -- weekends, the daily
//! halt). The first step after a closure therefore covers it; each step reports its open and closed
//! time separately so the regime model can give closed time its own learned weight (information
//! arrives mostly while trading: French & Roll 1986). The step clock starts at the first event the
//! engine sees. The amounts come
//! from a theoretical rule still to be ruled -- they are configuration, never tuned on data.
//!
//! **Time** only moves forward: an older trade, or a non-increasing sequence, is an error
//! ([`EngineError::TimeWentBackwards`], [`EngineError::SequenceOutOfOrder`]); the caller resets and
//! replays. Never absorbed silently (spec §6.2.5).

use crate::activity::{ActivityBar, ImbalanceBarBuilder};
use crate::bars::TimeBarBuilder;
use crate::clock::{EventTime, SessionCalendar, Timeframe};
use crate::input::{Bar, InputError, Trade, TradeColumns};
use crate::ring::WindowRing;

/// Closed bars kept per timeframe (the longest window any estimator reads, plus slack).
pub const BAR_WINDOW: usize = 512;
/// Closed activity bars kept.
pub const ACTIVITY_WINDOW: usize = 512;

/// Longest allowed step floor: one week (the open-time walk is per session).
pub const MAX_STEP_FLOOR_US: i64 = 7 * 24 * 3600 * 1_000_000;

/// The regime model's step trigger (whichever comes first since the previous step).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StepTrigger {
    /// Trades per step (`u32::MAX` disables this criterion).
    pub trades: u32,
    /// Contracts per step (`u64::MAX` disables).
    pub volume: u64,
    /// Longest OPEN market time between steps, microseconds (0 < floor <= one week).
    pub floor_us: i64,
}

#[derive(Debug, Clone, Copy, PartialEq)]
pub struct EngineConfig {
    pub calendar: SessionCalendar,
    /// Activity-clock imbalance threshold, contracts of signed aggressor volume.
    pub activity_threshold: u64,
    pub step_trigger: StepTrigger,
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
    TimeWentBackwards { last: EventTime, got: EventTime },
    /// A trade's sequence is not greater than the previous trade's.
    SequenceOutOfOrder { last: u64, got: u64 },
    /// The step floor must be positive and at most one week.
    InvalidStepFloor(i64),
}

impl std::fmt::Display for EngineError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            EngineError::Input(e) => write!(f, "{e}"),
            EngineError::TimeWentBackwards { last, got } => write!(f, "time went backwards: last {} us, got {} us", last.0, got.0),
            EngineError::SequenceOutOfOrder { last, got } => write!(f, "trade sequence out of order: last {last}, got {got}"),
            EngineError::InvalidStepFloor(us) => write!(f, "step floor {us} us is not in (0, one week]"),
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
    /// When the newest bar closed (the end of its span).
    pub last_close_time: Option<EventTime>,
    /// The engine's trade count when the newest bar closed (the event-order record).
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

/// Why a step fired.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum StepReason {
    Trades,
    Volume,
    Floor,
}

/// One regime-model step.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct StepEvent {
    pub time: EventTime,
    /// Time since the previous step (the first step: since the first event).
    pub elapsed_us: i64,
    /// The part of `elapsed_us` while the market was open, and while it was closed.
    pub elapsed_open_us: i64,
    pub elapsed_closed_us: i64,
    /// Trades and contracts since the previous step.
    pub trades: u32,
    pub volume: u64,
    pub reason: StepReason,
}

/// Counts from one batch.
#[derive(Debug, Clone, Copy, PartialEq, Eq, Default)]
pub struct BatchStats {
    pub trades: usize,
    pub closed_bars: [usize; 3],
    pub activity_bars: usize,
    pub steps: usize,
}

#[derive(Debug, Clone)]
pub struct Engine {
    config: EngineConfig,
    last_time: Option<EventTime>,
    last_sequence: Option<u64>,
    trades_seen: u64,
    builders: [TimeBarBuilder; 3],
    bars: [BarStore; 3],
    activity_builder: ImbalanceBarBuilder,
    activity: ActivityStore,
    last_step: Option<EventTime>,
    /// When the floor step is due (open time `floor_us` after `last_step`).
    next_floor: Option<EventTime>,
    step_trades: u32,
    step_volume: u64,
    /// Fired steps not yet drained (capacity kept across drains).
    steps: Vec<StepEvent>,
}

impl Engine {
    pub fn new(config: EngineConfig) -> Result<Self, EngineError> {
        if config.step_trigger.floor_us <= 0 || config.step_trigger.floor_us > MAX_STEP_FLOOR_US {
            return Err(EngineError::InvalidStepFloor(config.step_trigger.floor_us));
        }
        Ok(Self {
            config,
            last_time: None,
            last_sequence: None,
            trades_seen: 0,
            builders: Timeframe::ALL.map(|tf| TimeBarBuilder::new(tf, config.calendar)),
            bars: Default::default(),
            activity_builder: ImbalanceBarBuilder::new(config.activity_threshold),
            activity: ActivityStore::default(),
            last_step: None,
            next_floor: None,
            step_trades: 0,
            step_volume: 0,
            steps: Vec::new(),
        })
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

    /// The bar still forming in a timeframe (informational; the regime inputs read closed bars
    /// unless the open forming-bar decision rules otherwise).
    pub fn forming_bar(&self, timeframe: Timeframe) -> Option<Bar> {
        self.builders[timeframe.index()].forming_bar()
    }

    pub fn activity(&self) -> &ActivityStore {
        &self.activity
    }

    /// Steps fired since the last drain, in time order.
    pub fn drain_steps(&mut self) -> std::vec::Drain<'_, StepEvent> {
        self.steps.drain(..)
    }

    /// Drops all state; the caller then replays history.
    pub fn reset(&mut self, _reason: ResetReason) {
        self.last_time = None;
        self.last_sequence = None;
        self.trades_seen = 0;
        for b in &mut self.builders {
            b.clear();
        }
        for b in &mut self.bars {
            b.clear();
        }
        self.activity_builder.clear();
        self.activity.clear();
        self.last_step = None;
        self.next_floor = None;
        self.step_trades = 0;
        self.step_volume = 0;
        self.steps.clear();
    }

    fn set_last_step(&mut self, t: EventTime) {
        self.last_step = Some(t);
        self.next_floor = Some(self.config.calendar.add_open_time(t, self.config.step_trigger.floor_us));
    }

    fn check_time(&mut self, t: EventTime) -> Result<(), EngineError> {
        if let Some(last) = self.last_time
            && t < last
        {
            return Err(EngineError::TimeWentBackwards { last, got: t });
        }
        self.last_time = Some(t);
        if self.last_step.is_none() {
            self.set_last_step(t);
        }
        Ok(())
    }

    fn fire_step(&mut self, time: EventTime, reason: StepReason) {
        let previous = self.last_step.unwrap_or(time);
        let elapsed_us = time.0 - previous.0;
        let elapsed_open_us = self.config.calendar.open_time_between(previous, time);
        self.steps.push(StepEvent {
            time,
            elapsed_us,
            elapsed_open_us,
            elapsed_closed_us: elapsed_us - elapsed_open_us,
            trades: self.step_trades,
            volume: self.step_volume,
            reason,
        });
        self.set_last_step(time);
        self.step_trades = 0;
        self.step_volume = 0;
    }

    /// Closes the bars and fires the floor steps that are due at or before `t`, in time order (a bar
    /// close first at equal times). Returns the bars closed per timeframe and the steps fired.
    fn advance(&mut self, t: EventTime) -> ([usize; 3], usize) {
        let mut closed = [0usize; 3];
        let mut steps = 0usize;
        loop {
            let next_bar = self
                .builders
                .iter()
                .enumerate()
                .filter_map(|(i, b)| b.forming_end().filter(|&end| end <= t).map(|end| (end, i)))
                .min_by_key(|&(end, i)| (end.0, i));
            let next_floor = self.next_floor.filter(|&f| f <= t);
            let bar = match (next_bar, next_floor) {
                (Some((end, i)), Some(f)) if end <= f => Some((end, i)),
                (Some((end, i)), None) => Some((end, i)),
                (_, Some(_)) => None,
                (None, None) => break,
            };
            if let Some((end, i)) = bar {
                let builder = &mut self.builders[i];
                builder.close_forming();
                let view = builder.view();
                let closed_bar = view.bar(view.len() - 1);
                builder.drain_closed();
                let store = &mut self.bars[i];
                store.push(&closed_bar);
                store.last_close_time = Some(end);
                store.trades_at_last_close = Some(self.trades_seen);
                closed[i] += 1;
            } else if let Some(f) = next_floor {
                self.fire_step(f, StepReason::Floor);
                steps += 1;
            }
        }
        (closed, steps)
    }

    /// Advances time to `now` without a trade (live timer call; offline end of a segment): closes the
    /// bars and fires the floor steps that are due.
    pub fn on_time(&mut self, now: EventTime) -> Result<BatchStats, EngineError> {
        self.check_time(now)?;
        let (closed_bars, steps) = self.advance(now);
        Ok(BatchStats { closed_bars, steps, ..BatchStats::default() })
    }

    /// One trade with its sequence number.
    pub fn on_trade(&mut self, t: &Trade, sequence: u64) -> Result<BatchStats, EngineError> {
        if let Some(last) = self.last_sequence
            && sequence <= last
        {
            return Err(EngineError::SequenceOutOfOrder { last, got: sequence });
        }
        self.check_time(t.time)?;
        self.last_sequence = Some(sequence);
        let (closed_bars, mut steps) = self.advance(t.time);
        for b in &mut self.builders {
            b.apply(t);
        }
        self.trades_seen += 1;
        let activity_closed = self.activity_builder.on_trade(t);
        if let Some(bar) = &activity_closed {
            self.activity.push(bar);
        }
        self.step_trades = self.step_trades.saturating_add(1);
        self.step_volume = self.step_volume.saturating_add(u64::from(t.volume()));
        let trigger = self.config.step_trigger;
        if self.step_trades >= trigger.trades {
            self.fire_step(t.time, StepReason::Trades);
            steps += 1;
        } else if self.step_volume >= trigger.volume {
            self.fire_step(t.time, StepReason::Volume);
            steps += 1;
        }
        Ok(BatchStats { trades: 1, closed_bars, activity_bars: usize::from(activity_closed.is_some()), steps })
    }

    /// A batch of trades in sequence order.
    pub fn on_trades(&mut self, trades: &TradeColumns<'_>) -> Result<BatchStats, EngineError> {
        let mut total = BatchStats::default();
        for i in 0..trades.len() {
            let s = self.on_trade(&trades.get(i), trades.sequence(i))?;
            total.trades += s.trades;
            for (t, n) in total.closed_bars.iter_mut().zip(s.closed_bars) {
                *t += n;
            }
            total.activity_bars += s.activity_bars;
            total.steps += s.steps;
        }
        Ok(total)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::clock::{US_PER_MINUTE, US_PER_SECOND};

    /// 2024-07-15 14:00 UTC (10:00 ET), a Monday.
    const T0: i64 = 1_721_052_000_000_000;

    const NO_STEPS: StepTrigger = StepTrigger { trades: u32::MAX, volume: u64::MAX, floor_us: MAX_STEP_FLOOR_US };

    fn engine(threshold: u64, trigger: StepTrigger) -> Engine {
        Engine::new(EngineConfig { calendar: SessionCalendar::default(), activity_threshold: threshold, step_trigger: trigger }).expect("valid config")
    }

    fn trade(time_us: i64, price: f64, buy: u32, sell: u32) -> Trade {
        Trade { time: EventTime(time_us), price, buy_volume: buy, sell_volume: sell }
    }

    #[test]
    fn builds_every_timeframe_from_trades_and_closes_bars_when_their_span_ends() {
        let mut e = engine(1_000_000, NO_STEPS);
        // One trade a minute for 2 hours (10:00-12:00 ET), in odd-sized batches.
        let n = 120usize;
        let t: Vec<i64> = (0..n as i64).map(|m| T0 + m * US_PER_MINUTE).collect();
        let p: Vec<f64> = (0..n).map(|i| 5000.0 + (i % 7) as f64 * 0.25).collect();
        let buy = vec![1u32; n];
        let sell = vec![0u32; n];
        let all = TradeColumns::new(&t, &p, &buy, &sell).expect("equal lengths");
        let mut closed = [0usize; 3];
        for range in [0..7, 7..50, 50..51, 51..120] {
            let s = e.on_trades(&all.slice(range)).expect("in order");
            for (c, n) in closed.iter_mut().zip(s.closed_bars) {
                *c += n;
            }
        }
        // 10:00-12:00 ET with the last trade at 11:59: 7 closed quarter hours, 1 closed hour.
        assert_eq!(closed, [0, 1, 7]);
        let ts3 = e.bars(Timeframe::Ts3);
        assert!(ts3.num_trades.as_slice().iter().all(|&x| x.to_bits() == 15.0f32.to_bits()));
        // The 7th quarter hour closed when time reached 11:45, i.e. on the trade at 11:45 (trade 105),
        // before that trade was applied.
        assert_eq!(ts3.trades_at_last_close, Some(105));
        assert_eq!(ts3.last_close_time, Some(EventTime(T0 + 105 * US_PER_MINUTE)));
        // No more trades: time reaching 12:00 closes the 8th quarter hour and the 2nd hour.
        let s = e.on_time(EventTime(T0 + 120 * US_PER_MINUTE)).expect("forward");
        assert_eq!(s.closed_bars, [0, 1, 1]);
        assert_eq!(e.bars(Timeframe::Ts2).volume.as_slice().iter().map(|v| v.to_bits()).collect::<Vec<_>>(), vec![60.0f32.to_bits(); 2]);
        assert!(e.forming_bar(Timeframe::Ts3).is_none());
        // The 240-minute bar opened at 10:00 ET spans 10:00-14:00 ET, so it is still forming.
        assert_eq!(e.bars(Timeframe::Ts1).closed_count, 0);
        assert!(e.forming_bar(Timeframe::Ts1).is_some());
    }

    #[test]
    fn step_trigger_fires_on_trades_volume_and_floor_in_time_order() {
        let s = US_PER_SECOND;
        let mut e = engine(1_000_000, StepTrigger { trades: 3, volume: 10, floor_us: 5 * s });
        // Step clock starts at T0. Trades at 1,2,3 s -> 3rd trade fires; trade at 4 s (size 12) fires
        // on volume; nothing until 9 s -> floor step at 9 s; trade at 9.5 s; floor at 14 s.
        e.on_time(EventTime(T0)).expect("start");
        let trades = [(s, 1u32), (2 * s, 1), (3 * s, 1), (4 * s, 12), (9 * s + s / 2, 1)];
        for (i, &(dt, size)) in trades.iter().enumerate() {
            e.on_trade(&trade(T0 + dt, 100.0, size, 0), i as u64).expect("in order");
        }
        e.on_time(EventTime(T0 + 15 * s)).expect("forward");
        let drained: Vec<StepEvent> = e.drain_steps().collect();
        assert!(drained.iter().all(|x| x.elapsed_open_us == x.elapsed_us && x.elapsed_closed_us == 0), "Monday morning: all open");
        let steps: Vec<(i64, StepReason, u32, u64, i64)> = drained.iter().map(|x| (x.time.0 - T0, x.reason, x.trades, x.volume, x.elapsed_us)).collect();
        assert_eq!(
            steps,
            vec![
                (3 * s, StepReason::Trades, 3, 3, 3 * s),
                (4 * s, StepReason::Volume, 1, 12, s),
                (9 * s, StepReason::Floor, 0, 0, 5 * s),
                (14 * s, StepReason::Floor, 1, 1, 5 * s),
            ]
        );
        assert_eq!(e.drain_steps().count(), 0);
    }

    /// A bar ending exactly when a floor step is due closes first, so an input built on it is new at
    /// that step.
    #[test]
    fn a_bar_close_comes_before_a_floor_step_at_the_same_time() {
        let mut e = engine(1_000_000, StepTrigger { trades: u32::MAX, volume: u64::MAX, floor_us: 15 * US_PER_MINUTE });
        e.on_time(EventTime(T0)).expect("start");
        e.on_trade(&trade(T0 + US_PER_MINUTE, 100.0, 1, 0), 0).expect("first");
        let s = e.on_time(EventTime(T0 + 15 * US_PER_MINUTE)).expect("forward");
        assert_eq!((s.closed_bars[Timeframe::Ts3.index()], s.steps), (1, 1));
        let step = e.drain_steps().next().expect("one step");
        let close = e.bars(Timeframe::Ts3).last_close_time.expect("closed");
        assert!(close <= step.time);
        assert_eq!(close, EventTime(T0 + 15 * US_PER_MINUTE));
    }

    #[test]
    fn sequence_orders_equal_timestamps_and_must_increase() {
        let mut e = engine(5, NO_STEPS);
        let t = [1i64, 2, 2, 3];
        let p = [100.0, 100.25, 100.5, 100.25];
        let buy = [3u32, 2, 0, 1];
        let sell = [0u32, 0, 1, 0];
        let cols = TradeColumns::new(&t, &p, &buy, &sell).expect("equal lengths").starting_at(10);
        let s = e.on_trades(&cols).expect("in order");
        assert_eq!((s.trades, s.activity_bars), (4, 1)); // +3, +5 -> closes on the 2nd trade
        assert_eq!(e.activity().imbalance.as_slice(), &[5]);
        // Same timestamp, repeated or older sequence: rejected.
        assert_eq!(e.on_trade(&trade(3, 1.0, 1, 0), 13), Err(EngineError::SequenceOutOfOrder { last: 13, got: 13 }));
        assert_eq!(e.on_trade(&trade(2, 1.0, 1, 0), 14), Err(EngineError::TimeWentBackwards { last: EventTime(3), got: EventTime(2) }));
        e.reset(ResetReason::NewContract);
        assert_eq!((e.trades_seen(), e.activity().closed_count), (0, 0));
        assert!(e.on_trade(&trade(2, 1.0, 1, 0), 0).is_ok());
    }

    #[test]
    fn invalid_step_floor_is_rejected() {
        let config = EngineConfig { calendar: SessionCalendar::default(), activity_threshold: 5, step_trigger: StepTrigger { trades: 1, volume: 1, floor_us: 0 } };
        assert_eq!(Engine::new(config).err(), Some(EngineError::InvalidStepFloor(0)));
        let long = EngineConfig { step_trigger: StepTrigger { trades: 1, volume: 1, floor_us: MAX_STEP_FLOOR_US + 1 }, ..config };
        assert_eq!(Engine::new(long).err(), Some(EngineError::InvalidStepFloor(MAX_STEP_FLOOR_US + 1)));
    }

    /// The floor counts open time only: a step 30 s before the Friday close is followed by the floor
    /// step 30 s after the Sunday open, covering the weekend as closed time.
    #[test]
    fn the_floor_pauses_while_the_market_is_closed() {
        // Friday 2024-07-12 16:59:30 ET = 20:59:30 UTC; Sunday 18:00 ET = 22:00 UTC on the 14th.
        let friday = 1_720_817_970_000_000i64;
        let sunday_open = 1_720_994_400_000_000i64;
        let mut e = engine(1_000_000, StepTrigger { trades: u32::MAX, volume: u64::MAX, floor_us: 60 * US_PER_SECOND });
        e.on_time(EventTime(friday)).expect("start");
        e.on_time(EventTime(sunday_open + 45 * US_PER_SECOND)).expect("forward");
        let steps: Vec<StepEvent> = e.drain_steps().collect();
        assert_eq!(steps.len(), 1, "{steps:?}");
        let s = steps[0];
        assert_eq!(s.time, EventTime(sunday_open + 30 * US_PER_SECOND));
        assert_eq!(s.elapsed_open_us, 60 * US_PER_SECOND);
        assert_eq!(s.elapsed_closed_us, (49 * 3600) * US_PER_SECOND); // Fri 17:00 -> Sun 18:00 ET
        assert_eq!(s.elapsed_us, s.elapsed_open_us + s.elapsed_closed_us);
    }
}
