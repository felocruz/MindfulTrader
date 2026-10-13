//! The activity clock: imbalance bars from trades (spec §6.2.3). Signed aggressor volume (buy minus
//! sell) accumulates trade by trade; a bar closes on the trade that brings |sum| to the threshold,
//! and the next trade opens the next bar. A trade is never split across bars.
//!
//! The builder records what happened -- prices, volumes, the imbalance, the time span -- and leaves
//! every derived quantity (which return, which window) to the dims.

use crate::clock::EventTime;
use crate::input::Trade;

/// One closed imbalance bar.
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct ActivityBar {
    /// Time of the first and of the closing trade.
    pub first_time: EventTime,
    pub last_time: EventTime,
    pub open: f64,
    pub high: f64,
    pub low: f64,
    pub close: f64,
    pub buy_volume: u64,
    pub sell_volume: u64,
    pub trades: u32,
    /// Signed aggressor volume at the close: |imbalance| >= threshold.
    pub imbalance: i64,
}

/// Builds imbalance bars with a fixed threshold (contracts of signed aggressor volume).
#[derive(Debug, Clone)]
pub struct ImbalanceBarBuilder {
    threshold: u64,
    forming: Option<ActivityBar>,
}

impl ImbalanceBarBuilder {
    /// `threshold` must be positive.
    pub fn new(threshold: u64) -> Self {
        Self { threshold: threshold.max(1), forming: None }
    }

    pub fn threshold(&self) -> u64 {
        self.threshold
    }

    /// The bar forming now, if any.
    pub fn forming(&self) -> Option<&ActivityBar> {
        self.forming.as_ref()
    }

    /// Adds a trade; returns the bar it closed, if it closed one.
    #[inline]
    pub fn on_trade(&mut self, t: &Trade) -> Option<ActivityBar> {
        let bar = self.forming.get_or_insert(ActivityBar {
            first_time: t.time,
            last_time: t.time,
            open: t.price,
            high: t.price,
            low: t.price,
            close: t.price,
            ..ActivityBar::default()
        });
        bar.last_time = t.time;
        bar.high = bar.high.max(t.price);
        bar.low = bar.low.min(t.price);
        bar.close = t.price;
        bar.buy_volume += u64::from(t.buy_volume);
        bar.sell_volume += u64::from(t.sell_volume);
        bar.trades += 1;
        bar.imbalance += t.signed_volume();
        if bar.imbalance.unsigned_abs() >= self.threshold { self.forming.take() } else { None }
    }

    pub fn clear(&mut self) {
        self.forming = None;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn t(us: i64, price: f64, buy: u32, sell: u32) -> Trade {
        Trade { time: EventTime(us), price, buy_volume: buy, sell_volume: sell }
    }

    #[test]
    fn closes_when_the_signed_sum_reaches_the_threshold_either_way() {
        let mut b = ImbalanceBarBuilder::new(5);
        assert_eq!(b.on_trade(&t(1, 100.0, 3, 0)), None);
        assert_eq!(b.on_trade(&t(2, 100.25, 0, 1)), None); // +2
        let bar = b.on_trade(&t(3, 100.5, 3, 0)).expect("+5 closes"); // +5
        assert_eq!((bar.first_time, bar.last_time, bar.trades, bar.imbalance), (EventTime(1), EventTime(3), 3, 5));
        assert_eq!((bar.buy_volume, bar.sell_volume), (6, 1));
        assert_eq!([bar.open, bar.high, bar.low, bar.close].map(f64::to_bits), [100.0f64, 100.5, 100.0, 100.5].map(f64::to_bits));
        assert!(b.forming().is_none());
        // The next trade opens a new bar; a sell run closes it at -5 (one large trade is not split).
        assert_eq!(b.on_trade(&t(4, 100.25, 0, 2)), None);
        let bar = b.on_trade(&t(5, 99.75, 0, 7)).expect("-9 closes");
        assert_eq!((bar.imbalance, bar.open.to_bits(), bar.low.to_bits()), (-9, 100.25f64.to_bits(), 99.75f64.to_bits()));
    }

    #[test]
    fn balanced_flow_never_closes_and_clear_drops_the_forming_bar() {
        let mut b = ImbalanceBarBuilder::new(3);
        for i in 0..100 {
            let (buy, sell) = if i % 2 == 0 { (2, 0) } else { (0, 2) };
            assert_eq!(b.on_trade(&t(i, 50.0, buy, sell)), None);
        }
        assert_eq!(b.forming().map(|f| f.trades), Some(100));
        b.clear();
        assert!(b.forming().is_none());
        assert_eq!(ImbalanceBarBuilder::new(0).threshold(), 1);
    }
}
