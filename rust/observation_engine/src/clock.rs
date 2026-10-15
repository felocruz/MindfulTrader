//! Time: the engine's event clock, the chart timeframes, and the CME Globex session calendar that
//! anchors time bars.
//!
//! There is no wall clock anywhere in the engine (spec §6.2.2 principle 6): time is the timestamp of
//! the event being processed, Unix microseconds UTC. Bars are anchored to the trading session, not
//! to UTC multiples: MES trades Sunday-Friday from 18:00 to 17:00 US Eastern time, so a 240-minute
//! bar starts at 18:00, 22:00, 02:00, 06:00, 10:00 and 14:00 ET, and the last one is cut short by the
//! 17:00 close. US Eastern daylight time follows the rules in force since 2007.

/// Microseconds per second / minute / day.
pub const US_PER_SECOND: i64 = 1_000_000;
pub const US_PER_MINUTE: i64 = 60 * US_PER_SECOND;
const SECONDS_PER_DAY: i64 = 86_400;

/// An event timestamp: Unix microseconds, UTC.
#[derive(Debug, Clone, Copy, PartialEq, Eq, PartialOrd, Ord, Hash, Default)]
pub struct EventTime(pub i64);

impl EventTime {
    pub fn micros(self) -> i64 {
        self.0
    }
}

/// Days from Sierra Chart's epoch (1899-12-30) to the Unix epoch.
const SC_EPOCH_TO_UNIX_DAYS: i64 = 25_569;

/// Sierra Chart `SCDateTime` (an `int64_t` of microseconds since 1899-12-30, in whatever time zone
/// the chart is set to) -> Unix microseconds in that same zone. The FFI adapter must hand the engine
/// UTC: it either runs the chart in UTC or removes the chart's offset before calling this.
pub fn scdatetime_to_unix_us(sc_us: i64) -> i64 {
    sc_us - SC_EPOCH_TO_UNIX_DAYS * SECONDS_PER_DAY * US_PER_SECOND
}

/// The three Triple Screen chart timeframes (minutes, not seconds).
#[derive(Debug, Clone, Copy, PartialEq, Eq, Hash)]
pub enum Timeframe {
    /// Screen 1: 240-minute bars.
    Ts1,
    /// Screen 2: 60-minute bars.
    Ts2,
    /// Screen 3: 15-minute bars.
    Ts3,
}

impl Timeframe {
    pub const ALL: [Timeframe; 3] = [Timeframe::Ts1, Timeframe::Ts2, Timeframe::Ts3];

    pub fn minutes(self) -> i64 {
        match self {
            Timeframe::Ts1 => 240,
            Timeframe::Ts2 => 60,
            Timeframe::Ts3 => 15,
        }
    }

    pub fn period_us(self) -> i64 {
        self.minutes() * US_PER_MINUTE
    }

    /// Dense index (0..3) for per-timeframe arrays.
    pub fn index(self) -> usize {
        match self {
            Timeframe::Ts1 => 0,
            Timeframe::Ts2 => 1,
            Timeframe::Ts3 => 2,
        }
    }
}

// --- civil-date arithmetic (Hinnant, "chrono-Compatible Low-Level Date Algorithms") ---------------

/// (year, month 1-12, day 1-31) of a day count since 1970-01-01.
fn civil_from_days(days: i64) -> (i64, u32, u32) {
    let z = days + 719_468;
    let era = z.div_euclid(146_097);
    let doe = z - era * 146_097;
    let yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
    let doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    let mp = (5 * doy + 2) / 153;
    let d = (doy - (153 * mp + 2) / 5 + 1) as u32;
    let m = (if mp < 10 { mp + 3 } else { mp - 9 }) as u32;
    (yoe + era * 400 + i64::from(m <= 2), m, d)
}

/// Day count since 1970-01-01 of a civil date.
fn days_from_civil(y: i64, m: u32, d: u32) -> i64 {
    let y = if m <= 2 { y - 1 } else { y };
    let era = y.div_euclid(400);
    let yoe = y - era * 400;
    let m = i64::from(m);
    let doy = (153 * (if m > 2 { m - 3 } else { m + 9 }) + 2) / 5 + i64::from(d) - 1;
    let doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    era * 146_097 + doe - 719_468
}

/// Unix microseconds of a UTC calendar date's midnight.
pub fn utc_midnight_us(year: i64, month: u32, day: u32) -> i64 {
    days_from_civil(year, month, day) * SECONDS_PER_DAY * US_PER_SECOND
}

/// Day of week, 0 = Sunday.
fn weekday(days: i64) -> i64 {
    (days + 4).rem_euclid(7)
}

/// Day of month of the n-th (1-based) Sunday of a month.
fn nth_sunday(y: i64, m: u32, n: u32) -> u32 {
    let first = days_from_civil(y, m, 1);
    1 + ((7 - weekday(first)) % 7) as u32 + 7 * (n - 1)
}

/// Is US daylight time in effect at the END of this local date (i.e. at any time after 02:00)?
/// DST runs from the second Sunday of March to the first Sunday of November (since 2007).
fn is_dst_local_date(y: i64, m: u32, d: u32) -> bool {
    match m {
        4..=10 => true,
        3 => d >= nth_sunday(y, 3, 2),
        11 => d < nth_sunday(y, 11, 1),
        _ => false,
    }
}

/// US Eastern offset from UTC, in seconds, at a UTC instant.
fn eastern_offset_seconds(utc_seconds: i64) -> i64 {
    let days = utc_seconds.div_euclid(SECONDS_PER_DAY);
    let (y, _, _) = civil_from_days(days);
    // Switches happen at 02:00 local: 07:00 UTC in March (from EST), 06:00 UTC in November (from EDT).
    let start = (days_from_civil(y, 3, nth_sunday(y, 3, 2)) * SECONDS_PER_DAY) + 7 * 3600;
    let end = (days_from_civil(y, 11, nth_sunday(y, 11, 1)) * SECONDS_PER_DAY) + 6 * 3600;
    if (start..end).contains(&utc_seconds) { -4 * 3600 } else { -5 * 3600 }
}

/// UTC microseconds of a US Eastern local date and minute-of-day. Valid for minutes outside the
/// 01:00-03:00 switch window (the session calendar only asks for 17:00 and 18:00).
fn eastern_to_utc_us(local_days: i64, minute_of_day: i64) -> i64 {
    let (y, m, d) = civil_from_days(local_days);
    let offset = if is_dst_local_date(y, m, d) { -4 * 3600 } else { -5 * 3600 };
    (local_days * SECONDS_PER_DAY + minute_of_day * 60 - offset) * US_PER_SECOND
}

/// How a trading session differs from the regular CME Globex equity session on its trade date.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub enum SessionChange {
    /// No session for this trade date.
    Closed,
    /// The session ends early, at this US Eastern minute of the trade date.
    EarlyClose { minute: i64 },
}

/// A one-off change to the rules (a day of mourning, a shortened Good Friday on a jobs-report day):
/// events no rule can predict. `trade_date` is (year, month, day).
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SessionException {
    pub trade_date: (i64, u32, u32),
    pub change: SessionChange,
}

/// Known one-off changes for CME Globex equity futures (Eastern times; CME publishes Central).
/// Each verified two ways: the exchange's announcement and when MES actually stopped trading.
pub const CME_EQUITY_EXCEPTIONS: &[SessionException] = &[
    // Good Friday on a jobs-report day: equities open until 08:15 CT.
    SessionException { trade_date: (2023, 4, 7), change: SessionChange::EarlyClose { minute: 9 * 60 + 15 } },
    // National day of mourning (President Carter): equities open until 08:30 CT (CME release
    // 2024-12-30); MES last traded 09:29:59 ET.
    SessionException { trade_date: (2025, 1, 9), change: SessionChange::EarlyClose { minute: 9 * 60 + 30 } },
    // Good Friday on a jobs-report day: MES last traded 09:14:59 ET.
    SessionException { trade_date: (2026, 4, 3), change: SessionChange::EarlyClose { minute: 9 * 60 + 15 } },
];

/// Easter Sunday as a day count (anonymous Gregorian algorithm, Meeus/Jones/Butcher).
fn easter(y: i64) -> i64 {
    let a = y % 19;
    let b = y / 100;
    let c = y % 100;
    let d = b / 4;
    let e = b % 4;
    let f = (b + 8) / 25;
    let g = (b - f + 1) / 3;
    let h = (19 * a + b - d - g + 15) % 30;
    let i = c / 4;
    let k = c % 4;
    let l = (32 + 2 * e + 2 * i - h - k) % 7;
    let m = (a + 11 * h + 22 * l) / 451;
    let month = (h + l - 7 * m + 114) / 31;
    let day = (h + l - 7 * m + 114) % 31 + 1;
    days_from_civil(y, month as u32, day as u32)
}

/// Day count of the n-th (1-based) given weekday (0 = Sunday) of a month.
fn nth_weekday(y: i64, m: u32, weekday_wanted: i64, n: i64) -> i64 {
    let first = days_from_civil(y, m, 1);
    first + (weekday_wanted - weekday(first)).rem_euclid(7) + 7 * (n - 1)
}

/// Day count of the last given weekday of a month.
fn last_weekday(y: i64, m: u32, weekday_wanted: i64) -> i64 {
    let next_first = if m == 12 { days_from_civil(y + 1, 1, 1) } else { days_from_civil(y, m + 1, 1) };
    let last = next_first - 1;
    last - (weekday(last) - weekday_wanted).rem_euclid(7)
}

/// A fixed date moved to the nearest weekday (Saturday -> Friday, Sunday -> Monday).
fn nearest_workday(day: i64) -> i64 {
    match weekday(day) {
        6 => day - 1,
        0 => day + 1,
        _ => day,
    }
}

/// The CME Globex equity futures holiday rules, for any year (they project forward without
/// change). Times are US Eastern; CME publishes them in Central (+1 h here). Rules as in the
/// maintained `pandas_market_calendars` CME Globex Equities calendar (MIT; "Source
/// https://www.cmegroup.com/tools-information/holiday-calendar.html"), except Good Friday -- that
/// calendar marks every Good Friday since 2023 as an 08:15 CT close, but MES did not trade at all on
/// Good Friday 2024 or 2025; the rule here is "closed", and the shortened ones (jobs-report days,
/// which follow the BLS release calendar, not a rule) are in `CME_EQUITY_EXCEPTIONS`.
/// - Closed: New Year's Day (Sunday -> Monday; none when it falls on a Saturday), Good Friday,
///   Christmas (nearest weekday).
/// - Early close 13:00 ET (12:00 CT): Martin Luther King Jr. Day, Presidents Day, Memorial Day,
///   Juneteenth (nearest weekday), Independence Day (nearest weekday), Labor Day, Thanksgiving.
/// - Early close 13:15 ET (12:15 CT): the Friday after Thanksgiving; Christmas Eve when it falls
///   Monday to Thursday.
pub fn cme_equity_holiday_rule(trade_day: i64) -> Option<SessionChange> {
    let (y, m, d) = civil_from_days(trade_day);
    const EARLY: SessionChange = SessionChange::EarlyClose { minute: 13 * 60 };
    const EARLY_LATE: SessionChange = SessionChange::EarlyClose { minute: 13 * 60 + 15 };
    let new_year = days_from_civil(y, 1, 1);
    let closed = (weekday(new_year) != 6 && trade_day == if weekday(new_year) == 0 { new_year + 1 } else { new_year })
        || trade_day == easter(y) - 2
        || trade_day == nearest_workday(days_from_civil(y, 12, 25));
    if closed {
        return Some(SessionChange::Closed);
    }
    let thanksgiving = nth_weekday(y, 11, 4, 4);
    let early = trade_day == nth_weekday(y, 1, 1, 3) // MLK: 3rd Monday of January
        || trade_day == nth_weekday(y, 2, 1, 3) // Presidents Day: 3rd Monday of February
        || trade_day == last_weekday(y, 5, 1) // Memorial Day: last Monday of May
        || trade_day == nearest_workday(days_from_civil(y, 6, 19))
        || trade_day == nearest_workday(days_from_civil(y, 7, 4))
        || trade_day == nth_weekday(y, 9, 1, 1) // Labor Day: 1st Monday of September
        || trade_day == thanksgiving;
    if early {
        return Some(EARLY);
    }
    if trade_day == thanksgiving + 1 || (m == 12 && d == 24 && (1..=4).contains(&weekday(trade_day))) {
        return Some(EARLY_LATE);
    }
    None
}

/// Session times in US Eastern minutes of the day.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct SessionCalendar {
    /// Session open (CME Globex: 18:00 ET = 1080).
    pub open_minute: i64,
    /// Session close on the following day (CME Globex: 17:00 ET = 1020).
    pub close_minute: i64,
    /// Apply the exchange holiday rules and exceptions to the open-market time (the step floor).
    /// Bar spans never use them (bars exist only where trades are).
    pub holidays: bool,
    /// One-off changes, applied before the rules.
    pub exceptions: &'static [SessionException],
}

impl Default for SessionCalendar {
    fn default() -> Self {
        Self { open_minute: 18 * 60, close_minute: 17 * 60, holidays: true, exceptions: CME_EQUITY_EXCEPTIONS }
    }
}

/// The half-open UTC interval `[start, end)` of one bar.
#[derive(Debug, Clone, Copy, PartialEq, Eq)]
pub struct BarSpan {
    pub start: EventTime,
    pub end: EventTime,
}

impl BarSpan {
    pub fn contains(&self, t: EventTime) -> bool {
        self.start <= t && t < self.end
    }
}

impl SessionCalendar {
    /// The session `[open, close)` containing (or, during the daily halt, following) `t`.
    pub fn session(&self, t: EventTime) -> BarSpan {
        let utc_s = t.0.div_euclid(US_PER_SECOND);
        let local_s = utc_s + eastern_offset_seconds(utc_s);
        let local_days = local_s.div_euclid(SECONDS_PER_DAY);
        let local_minute = local_s.rem_euclid(SECONDS_PER_DAY) / 60;
        // A session opening at `open_minute` on day D runs to `close_minute` on day D+1.
        let open_day = if local_minute >= self.open_minute { local_days } else { local_days - 1 };
        let mut start = eastern_to_utc_us(open_day, self.open_minute);
        let mut end = eastern_to_utc_us(open_day + 1, self.close_minute);
        if t.0 >= end {
            // In the halt between close and the next open: the next session.
            start = eastern_to_utc_us(open_day + 1, self.open_minute);
            end = eastern_to_utc_us(open_day + 2, self.close_minute);
        }
        BarSpan { start: EventTime(start), end: EventTime(end) }
    }

    /// The change to the session that opens on local day `open_day` (its trade date is the next
    /// day): weekends (CME Globex equity futures trade Sunday 18:00 to Friday 17:00 ET, so no
    /// session opens Friday or Saturday), then the one-off exceptions, then the holiday rules.
    fn session_change(&self, open_day: i64) -> Option<SessionChange> {
        if matches!(weekday(open_day), 5 | 6) {
            return Some(SessionChange::Closed);
        }
        if !self.holidays {
            return None;
        }
        let trade_day = open_day + 1;
        let date = civil_from_days(trade_day);
        if let Some(e) = self.exceptions.iter().find(|e| e.trade_date == date) {
            return Some(e.change);
        }
        cme_equity_holiday_rule(trade_day)
    }

    /// The local day on which the session containing (or following) `t` opens, and its span.
    fn session_on(&self, t: EventTime) -> (i64, BarSpan) {
        let utc_s = t.0.div_euclid(US_PER_SECOND);
        let local_s = utc_s + eastern_offset_seconds(utc_s);
        let local_days = local_s.div_euclid(SECONDS_PER_DAY);
        let local_minute = local_s.rem_euclid(SECONDS_PER_DAY) / 60;
        let mut open_day = if local_minute >= self.open_minute { local_days } else { local_days - 1 };
        if t.0 >= eastern_to_utc_us(open_day + 1, self.close_minute) {
            open_day += 1;
        }
        let span = BarSpan { start: EventTime(eastern_to_utc_us(open_day, self.open_minute)), end: EventTime(eastern_to_utc_us(open_day + 1, self.close_minute)) };
        (open_day, span)
    }

    /// The first trading session that ends after `t`: the one containing `t`, or the next to open
    /// (skipping the daily halt, weekends and holidays; an early close shortens the session).
    pub fn trading_session_from(&self, t: EventTime) -> BarSpan {
        let (mut open_day, _) = self.session_on(t);
        loop {
            let start = EventTime(eastern_to_utc_us(open_day, self.open_minute));
            let end = match self.session_change(open_day) {
                Some(SessionChange::Closed) => None,
                Some(SessionChange::EarlyClose { minute }) => Some(EventTime(eastern_to_utc_us(open_day + 1, minute))),
                None => Some(EventTime(eastern_to_utc_us(open_day + 1, self.close_minute))),
            };
            if let Some(end) = end
                && end > t
            {
                return BarSpan { start, end };
            }
            open_day += 1;
        }
    }

    /// Is the market open at `t`?
    pub fn is_open(&self, t: EventTime) -> bool {
        self.trading_session_from(t).contains(t)
    }

    /// Open (trading) time in `[a, b)`, microseconds.
    pub fn open_time_between(&self, a: EventTime, b: EventTime) -> i64 {
        let mut total = 0;
        let mut t = a;
        while t < b {
            let session = self.trading_session_from(t);
            if session.start >= b {
                break;
            }
            let from = session.start.max(t);
            let to = session.end.min(b);
            total += to.0 - from.0;
            t = session.end;
        }
        total
    }

    /// The instant at which `duration_us` of open time has elapsed after `from` (closures skipped).
    pub fn add_open_time(&self, from: EventTime, duration_us: i64) -> EventTime {
        let mut remaining = duration_us;
        let mut t = from;
        loop {
            let session = self.trading_session_from(t);
            let start = session.start.max(t);
            let available = session.end.0 - start.0;
            if remaining <= available {
                return EventTime(start.0 + remaining);
            }
            remaining -= available;
            t = session.end;
        }
    }

    /// The bar of `timeframe` containing `t`: session open + k * period, cut at the session close.
    pub fn bar_span(&self, timeframe: Timeframe, t: EventTime) -> BarSpan {
        let session = self.session(t);
        let period = timeframe.period_us();
        let k = (t.0 - session.start.0).max(0) / period;
        let start = session.start.0 + k * period;
        BarSpan { start: EventTime(start), end: EventTime((start + period).min(session.end.0)) }
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn utc(y: i64, m: u32, d: u32, h: i64, min: i64) -> EventTime {
        EventTime((days_from_civil(y, m, d) * SECONDS_PER_DAY + h * 3600 + min * 60) * US_PER_SECOND)
    }

    #[test]
    fn civil_round_trip() {
        for days in [-1000, 0, 19_000, 19_723, 20_000, 25_000] {
            let (y, m, d) = civil_from_days(days);
            assert_eq!(days_from_civil(y, m, d), days);
        }
        assert_eq!(civil_from_days(0), (1970, 1, 1));
        assert_eq!(weekday(0), 4); // 1970-01-01 was a Thursday
        assert_eq!(utc_midnight_us(1970, 1, 2), 86_400 * US_PER_SECOND);
        assert_eq!(utc_midnight_us(2024, 7, 15), 1_721_001_600_000_000);
    }

    #[test]
    fn dst_switch_dates() {
        assert_eq!(nth_sunday(2024, 3, 2), 10);
        assert_eq!(nth_sunday(2024, 11, 1), 3);
        assert_eq!(nth_sunday(2025, 3, 2), 9);
        assert_eq!(nth_sunday(2025, 11, 1), 2);
        // 2024-03-10 06:59 UTC is still EST; 07:00 UTC is EDT.
        assert_eq!(eastern_offset_seconds(utc(2024, 3, 10, 6, 59).0 / US_PER_SECOND), -5 * 3600);
        assert_eq!(eastern_offset_seconds(utc(2024, 3, 10, 7, 0).0 / US_PER_SECOND), -4 * 3600);
        assert_eq!(eastern_offset_seconds(utc(2024, 11, 3, 5, 59).0 / US_PER_SECOND), -4 * 3600);
        assert_eq!(eastern_offset_seconds(utc(2024, 11, 3, 6, 0).0 / US_PER_SECOND), -5 * 3600);
    }

    #[test]
    fn sierra_epoch_conversion() {
        // 1970-01-01 00:00 is 25569 days after 1899-12-30.
        assert_eq!(scdatetime_to_unix_us(25_569 * 86_400 * 1_000_000), 0);
        assert_eq!(scdatetime_to_unix_us(25_569 * 86_400 * 1_000_000 + 1_234_567), 1_234_567);
    }

    #[test]
    fn session_opens_at_18_et_in_summer_and_winter() {
        let cal = SessionCalendar::default();
        // Summer (EDT, UTC-4): Monday 2024-07-15 10:00 ET = 14:00 UTC; session opened Sunday 18:00 ET = 22:00 UTC.
        let s = cal.session(utc(2024, 7, 15, 14, 0));
        assert_eq!(s.start, utc(2024, 7, 14, 22, 0));
        assert_eq!(s.end, utc(2024, 7, 15, 21, 0)); // 17:00 ET
        // Winter (EST, UTC-5): 2024-01-16 10:00 ET = 15:00 UTC; session opened 2024-01-15 18:00 ET = 23:00 UTC.
        let w = cal.session(utc(2024, 1, 16, 15, 0));
        assert_eq!(w.start, utc(2024, 1, 15, 23, 0));
        assert_eq!(w.end, utc(2024, 1, 16, 22, 0));
        // 18:30 ET belongs to the session that just opened.
        assert_eq!(cal.session(utc(2024, 7, 15, 22, 30)).start, utc(2024, 7, 15, 22, 0));
        // During the 17:00-18:00 halt: the next session.
        assert_eq!(cal.session(utc(2024, 7, 15, 21, 30)).start, utc(2024, 7, 15, 22, 0));
    }

    #[test]
    fn session_across_dst_switch_weekends() {
        let cal = SessionCalendar::default();
        // Sunday 2024-03-10 opens at 18:00 EDT (22:00 UTC); the previous Friday closed at 17:00 EST (22:00 UTC).
        assert_eq!(cal.session(utc(2024, 3, 11, 14, 0)).start, utc(2024, 3, 10, 22, 0));
        assert_eq!(cal.session(utc(2024, 3, 8, 15, 0)).end, utc(2024, 3, 8, 22, 0));
        // Sunday 2024-11-03 opens at 18:00 EST (23:00 UTC).
        assert_eq!(cal.session(utc(2024, 11, 4, 15, 0)).start, utc(2024, 11, 3, 23, 0));
    }

    #[test]
    fn bar_spans_are_session_anchored() {
        let cal = SessionCalendar::default();
        // Summer session 2024-07-14 22:00 UTC (18:00 ET) .. 2024-07-15 21:00 UTC (17:00 ET).
        let starts_ts1: Vec<EventTime> = [(14, 22), (15, 2), (15, 6), (15, 10), (15, 14), (15, 18)]
            .iter()
            .map(|&(d, h)| cal.bar_span(Timeframe::Ts1, utc(2024, 7, d, h, 30)).start)
            .collect();
        assert_eq!(
            starts_ts1,
            vec![utc(2024, 7, 14, 22, 0), utc(2024, 7, 15, 2, 0), utc(2024, 7, 15, 6, 0), utc(2024, 7, 15, 10, 0), utc(2024, 7, 15, 14, 0), utc(2024, 7, 15, 18, 0)]
        );
        // The last 240-minute bar (14:00-17:00 ET) is cut at the close: 3 hours.
        let last = cal.bar_span(Timeframe::Ts1, utc(2024, 7, 15, 20, 0));
        assert_eq!(last, BarSpan { start: utc(2024, 7, 15, 18, 0), end: utc(2024, 7, 15, 21, 0) });
        // 15-minute bars are on the quarter hour; 60-minute on the hour.
        let b = cal.bar_span(Timeframe::Ts3, utc(2024, 7, 15, 14, 37));
        assert_eq!(b, BarSpan { start: utc(2024, 7, 15, 14, 30), end: utc(2024, 7, 15, 14, 45) });
        let h = cal.bar_span(Timeframe::Ts2, utc(2024, 7, 15, 14, 37));
        assert_eq!(h, BarSpan { start: utc(2024, 7, 15, 14, 0), end: utc(2024, 7, 15, 15, 0) });
        assert!(h.contains(utc(2024, 7, 15, 14, 59)));
        assert!(!h.contains(utc(2024, 7, 15, 15, 0)));
    }

    #[test]
    fn weekends_and_the_daily_halt_are_closed() {
        let cal = SessionCalendar::default();
        // July 2024 (EDT, UTC-4): Fri 12th, Sat 13th, Sun 14th, Mon 15th.
        assert!(cal.is_open(utc(2024, 7, 12, 20, 59))); // Fri 16:59 ET
        assert!(!cal.is_open(utc(2024, 7, 12, 21, 0))); // Fri 17:00 ET: weekly close
        assert!(!cal.is_open(utc(2024, 7, 13, 23, 0))); // Sat 19:00 ET
        assert!(!cal.is_open(utc(2024, 7, 14, 21, 59))); // Sun 17:59 ET
        assert!(cal.is_open(utc(2024, 7, 14, 22, 0))); // Sun 18:00 ET: weekly open
        assert!(!cal.is_open(utc(2024, 7, 15, 21, 30))); // Mon 17:30 ET: daily halt
        assert!(cal.is_open(utc(2024, 7, 15, 22, 0))); // Mon 18:00 ET
        // Friday 16:00 ET to Monday 10:00 ET: 1 h Friday + Sunday 18:00 to Monday 10:00 (16 h).
        let open = cal.open_time_between(utc(2024, 7, 12, 20, 0), utc(2024, 7, 15, 14, 0));
        assert_eq!(open, 17 * 3600 * US_PER_SECOND);
        // 90 open minutes after Friday 16:00 ET: 60 on Friday, then 30 after the Sunday open.
        assert_eq!(cal.add_open_time(utc(2024, 7, 12, 20, 0), 90 * US_PER_MINUTE), utc(2024, 7, 14, 22, 30));
        // Through the daily halt: Monday 16:50 ET + 20 open minutes = Monday 18:10 ET.
        assert_eq!(cal.add_open_time(utc(2024, 7, 15, 20, 50), 20 * US_PER_MINUTE), utc(2024, 7, 15, 22, 10));
        // A week of open time: 5 sessions x 23 h.
        assert_eq!(cal.open_time_between(utc(2024, 7, 14, 22, 0), utc(2024, 7, 21, 22, 0)), 5 * 23 * 3600 * US_PER_SECOND);
    }

    fn rule(y: i64, m: u32, d: u32) -> Option<SessionChange> {
        cme_equity_holiday_rule(days_from_civil(y, m, d))
    }

    const EARLY_1300: Option<SessionChange> = Some(SessionChange::EarlyClose { minute: 13 * 60 });
    const EARLY_1315: Option<SessionChange> = Some(SessionChange::EarlyClose { minute: 13 * 60 + 15 });
    const CLOSED: Option<SessionChange> = Some(SessionChange::Closed);

    #[test]
    fn easter_dates() {
        for (y, m, d) in [(2023, 4, 9), (2024, 3, 31), (2025, 4, 20), (2026, 4, 5), (2027, 3, 28), (2038, 4, 25)] {
            assert_eq!(civil_from_days(easter(y)), (y, m, d));
        }
    }

    /// 2024: every rule on its known date, and ordinary days unaffected.
    #[test]
    fn holiday_rules_2024() {
        assert_eq!(rule(2024, 1, 1), CLOSED); // New Year (Monday)
        assert_eq!(rule(2024, 1, 15), EARLY_1300); // MLK
        assert_eq!(rule(2024, 2, 19), EARLY_1300); // Presidents Day
        assert_eq!(rule(2024, 3, 29), CLOSED); // Good Friday
        assert_eq!(rule(2024, 5, 27), EARLY_1300); // Memorial Day
        assert_eq!(rule(2024, 6, 19), EARLY_1300); // Juneteenth (Wednesday)
        assert_eq!(rule(2024, 7, 4), EARLY_1300); // Independence Day (Thursday)
        assert_eq!(rule(2024, 9, 2), EARLY_1300); // Labor Day
        assert_eq!(rule(2024, 11, 28), EARLY_1300); // Thanksgiving
        assert_eq!(rule(2024, 11, 29), EARLY_1315); // the Friday after
        assert_eq!(rule(2024, 12, 24), EARLY_1315); // Christmas Eve (Tuesday)
        assert_eq!(rule(2024, 12, 25), CLOSED); // Christmas
        for (m, d) in [(3, 28), (7, 3), (7, 5), (12, 23), (12, 26)] {
            assert_eq!(rule(2024, m, d), None, "2024-{m}-{d}");
        }
    }

    /// The rules project forward with no change: 2027 weekend observances and Easter.
    #[test]
    fn holiday_rules_project_to_2027() {
        assert_eq!(rule(2027, 1, 1), CLOSED); // Friday
        assert_eq!(rule(2027, 3, 26), CLOSED); // Good Friday (Easter March 28)
        assert_eq!(rule(2027, 6, 18), EARLY_1300); // Juneteenth on a Saturday -> Friday
        assert_eq!(rule(2027, 7, 5), EARLY_1300); // July 4th on a Sunday -> Monday
        assert_eq!(rule(2027, 11, 25), EARLY_1300); // Thanksgiving
        assert_eq!(rule(2027, 12, 24), CLOSED); // Christmas on a Saturday -> Friday closed
        assert_eq!(rule(2028, 1, 1), None); // New Year on a Saturday: not observed
        assert_eq!(rule(2023, 1, 2), CLOSED); // New Year on a Sunday -> Monday
    }

    #[test]
    fn holidays_and_exceptions_shape_the_open_time() {
        let cal = SessionCalendar::default();
        // Christmas 2024 (Wednesday): Christmas Eve closes 13:15 ET, nothing until Dec 25 18:00 ET.
        assert!(cal.is_open(utc(2024, 12, 24, 18, 14))); // 13:14 ET
        assert!(!cal.is_open(utc(2024, 12, 24, 18, 15))); // 13:15 ET
        assert!(!cal.is_open(utc(2024, 12, 25, 15, 0))); // Christmas Day
        assert!(cal.is_open(utc(2024, 12, 25, 23, 0))); // 18:00 ET, trade date Dec 26
        // Good Friday 2024: no session; Thursday closes normally at 17:00 ET.
        assert!(cal.is_open(utc(2024, 3, 28, 20, 59)));
        assert!(!cal.is_open(utc(2024, 3, 29, 14, 0)));
        // Good Friday 2026 (exception): open until 09:15 ET.
        assert!(cal.is_open(utc(2026, 4, 3, 13, 14)));
        assert!(!cal.is_open(utc(2026, 4, 3, 13, 15)));
        // Day of mourning 2025-01-09: open until 09:30 ET (EST).
        assert!(cal.is_open(utc(2025, 1, 9, 14, 29)));
        assert!(!cal.is_open(utc(2025, 1, 9, 14, 30)));
        // MLK 2024: 13:00 ET close, reopen 18:00 ET.
        assert!(!cal.is_open(utc(2024, 1, 15, 18, 0)));
        assert!(cal.is_open(utc(2024, 1, 15, 23, 0)));
        // Open time across Christmas: Dec 24 12:00 ET -> Dec 26 12:00 ET = 1.25 h + 18 h.
        let open = cal.open_time_between(utc(2024, 12, 24, 17, 0), utc(2024, 12, 26, 17, 0));
        assert_eq!(open, (75 + 18 * 60) * US_PER_MINUTE);
        // Holidays can be switched off (bars never use them).
        let plain = SessionCalendar { holidays: false, ..cal };
        assert!(plain.is_open(utc(2024, 12, 25, 15, 0)));
    }
}
