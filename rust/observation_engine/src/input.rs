//! The engine's two inputs (spec §6.2.3): trades, and the bars of one timeframe.
//!
//! Both arrive as borrowed struct-of-arrays views, so neither feeder copies: offline, the views point
//! into decoded Arrow buffers; live, into Sierra Chart's own arrays (`c_ArrayWrapper<float>` is a
//! contiguous `float*`), valid for one call. The engine never keeps a view past the call.

use crate::clock::EventTime;

/// One trade: a match event with its aggressor volumes (CME MDP 3.0 tag 5797). Normally exactly one
/// side is non-zero; a record that merges several matches may carry both.
#[derive(Debug, Clone, Copy, PartialEq)]
pub struct Trade {
    pub time: EventTime,
    pub price: f64,
    /// Volume where the buyer was the aggressor (lifted the offer; Sierra "ask volume").
    pub buy_volume: u32,
    /// Volume where the seller was the aggressor (hit the bid; Sierra "bid volume").
    pub sell_volume: u32,
}

impl Trade {
    pub fn volume(&self) -> u32 {
        self.buy_volume + self.sell_volume
    }

    /// Signed aggressor volume: buy minus sell.
    pub fn signed_volume(&self) -> i64 {
        i64::from(self.buy_volume) - i64::from(self.sell_volume)
    }
}

/// A batch of trades as borrowed columns (equal lengths, time non-decreasing).
#[derive(Debug, Clone, Copy)]
pub struct TradeColumns<'a> {
    pub time_us: &'a [i64],
    pub price: &'a [f64],
    pub buy_volume: &'a [u32],
    pub sell_volume: &'a [u32],
}

impl<'a> TradeColumns<'a> {
    /// Checks the columns have one length.
    pub fn new(time_us: &'a [i64], price: &'a [f64], buy_volume: &'a [u32], sell_volume: &'a [u32]) -> Result<Self, InputError> {
        let n = time_us.len();
        if price.len() != n || buy_volume.len() != n || sell_volume.len() != n {
            return Err(InputError::ColumnLengths);
        }
        Ok(Self { time_us, price, buy_volume, sell_volume })
    }

    pub fn len(&self) -> usize {
        self.time_us.len()
    }

    pub fn is_empty(&self) -> bool {
        self.time_us.is_empty()
    }

    pub fn get(&self, i: usize) -> Trade {
        Trade { time: EventTime(self.time_us[i]), price: self.price[i], buy_volume: self.buy_volume[i], sell_volume: self.sell_volume[i] }
    }

    /// A sub-batch (no copy).
    pub fn slice(&self, range: std::ops::Range<usize>) -> TradeColumns<'a> {
        TradeColumns {
            time_us: &self.time_us[range.clone()],
            price: &self.price[range.clone()],
            buy_volume: &self.buy_volume[range.clone()],
            sell_volume: &self.sell_volume[range],
        }
    }

    pub fn iter(&self) -> impl Iterator<Item = Trade> + '_ {
        (0..self.len()).map(|i| self.get(i))
    }
}

/// The bars of one timeframe as borrowed columns, in Sierra Chart's layout: one `f32` array per
/// field, bar start times in Unix microseconds UTC. Indices `0..live_index` are closed bars; the bar
/// at `live_index` (if present) is the one still forming.
#[derive(Debug, Clone, Copy)]
pub struct BarColumns<'a> {
    pub time_us: &'a [i64],
    pub open: &'a [f32],
    pub high: &'a [f32],
    pub low: &'a [f32],
    pub close: &'a [f32],
    pub volume: &'a [f32],
    /// Seller-aggressor volume (Sierra `BidVolume`).
    pub bid_volume: &'a [f32],
    /// Buyer-aggressor volume (Sierra `AskVolume`).
    pub ask_volume: &'a [f32],
    pub num_trades: &'a [f32],
}

impl BarColumns<'_> {
    /// Checks every column has the length of `time_us`.
    pub fn validate(&self) -> Result<usize, InputError> {
        let n = self.time_us.len();
        let lens = [self.open.len(), self.high.len(), self.low.len(), self.close.len(), self.volume.len(), self.bid_volume.len(), self.ask_volume.len(), self.num_trades.len()];
        if lens.iter().any(|&l| l != n) {
            return Err(InputError::ColumnLengths);
        }
        Ok(n)
    }

    pub fn len(&self) -> usize {
        self.time_us.len()
    }

    pub fn is_empty(&self) -> bool {
        self.time_us.is_empty()
    }

    pub fn bar(&self, i: usize) -> Bar {
        Bar {
            start: EventTime(self.time_us[i]),
            open: self.open[i],
            high: self.high[i],
            low: self.low[i],
            close: self.close[i],
            volume: self.volume[i],
            bid_volume: self.bid_volume[i],
            ask_volume: self.ask_volume[i],
            num_trades: self.num_trades[i],
        }
    }
}

/// One bar, by value (the row form of `BarColumns`).
#[derive(Debug, Clone, Copy, PartialEq, Default)]
pub struct Bar {
    pub start: EventTime,
    pub open: f32,
    pub high: f32,
    pub low: f32,
    pub close: f32,
    pub volume: f32,
    pub bid_volume: f32,
    pub ask_volume: f32,
    pub num_trades: f32,
}

#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum InputError {
    /// Columns of one batch have different lengths.
    ColumnLengths,
}

impl std::fmt::Display for InputError {
    fn fmt(&self, f: &mut std::fmt::Formatter<'_>) -> std::fmt::Result {
        match self {
            InputError::ColumnLengths => write!(f, "input columns have different lengths"),
        }
    }
}

impl std::error::Error for InputError {}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn trade_columns_view_and_slice_without_copying() {
        let t = [1, 2, 3];
        let p = [10.0, 10.25, 10.5];
        let b = [1, 0, 2];
        let s = [0, 3, 0];
        let cols = TradeColumns::new(&t, &p, &b, &s).expect("equal lengths");
        assert_eq!(cols.len(), 3);
        let x = cols.get(1);
        assert_eq!((x.time, x.buy_volume, x.sell_volume, x.signed_volume(), x.volume()), (EventTime(2), 0, 3, -3, 3));
        let sub = cols.slice(1..3);
        assert_eq!(sub.len(), 2);
        assert!(std::ptr::eq(sub.price.as_ptr(), p[1..].as_ptr()));
        assert_eq!(TradeColumns::new(&t, &p[..2], &b, &s).err(), Some(InputError::ColumnLengths));
    }

    #[test]
    fn bar_columns_validate_lengths() {
        let t = [0i64, 1];
        let f = [1.0f32, 2.0];
        let short = [1.0f32];
        let ok = BarColumns { time_us: &t, open: &f, high: &f, low: &f, close: &f, volume: &f, bid_volume: &f, ask_volume: &f, num_trades: &f };
        assert_eq!(ok.validate(), Ok(2));
        assert_eq!(ok.bar(1).close.to_bits(), 2.0f32.to_bits());
        let bad = BarColumns { close: &short, ..ok };
        assert_eq!(bad.validate(), Err(InputError::ColumnLengths));
    }
}
