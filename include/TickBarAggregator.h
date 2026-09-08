// TickBarAggregator.h — pure, header-only, generic tick-to-bar aggregator.
// No Sierra Chart/ACSIL dependency -- reusable by any offline tool (the
// training-data generator, a future event generator, etc.), not tied to one
// tool's own source file.
//
// Reproduces Sierra Chart's real IBPT_DAYS_MINS_SECS bar-boundary convention
// for this project's CME ES/MES data: bar boundaries fall at
// sessionStartSecondsET + N*barPeriodSeconds, for integer N, in Eastern Time
// (DST-aware via EasternTimeOffset.h) -- matching the session-start constant
// EventDataCollectorStudy.cpp already trusts in production
// (CME_ES_SESSION_START_SECS = 18:00 ET), not a newly-invented assumption.
// Bar construction ITSELF (deciding exactly when Sierra Chart closes a bar)
// happens inside Sierra Chart's own closed-source charting engine
// (sierrachart.h's GetBarPeriodParameters/GetTradingDayStartDateTimeOfBar are
// opaque function-pointer calls into that engine, not code in this repo) --
// this is a faithful, natively-tested replication of the documented/observed
// convention, not literally shared code, since no such code exists in this
// repository to share.
//
// DST-transition edge case: US DST transitions occur at 2:00 AM ET on a
// Sunday, which for CME ES/MES falls within the weekly session close (the
// session does not reopen until Sunday 18:00 ET) -- so a transition instant
// never falls within an active trading session for this instrument, and no
// tick stream this aggregator processes can straddle one.

#pragma once

#include <algorithm>
#include <cstdint>
#include <functional>
#include <limits>

#include "EasternTimeOffset.h"

namespace tba {

struct Bar {
    int64_t openTimeUs = 0;
    int64_t closeTimeUs = 0;
    double open = 0.0;
    double high = 0.0;
    double low = 0.0;
    double close = 0.0;
    int64_t volume = 0;
    int64_t askVolume = 0;
    int64_t bidVolume = 0;
    int64_t numTrades = 0;
};

class TickBarAggregator {
public:
    TickBarAggregator(int64_t barPeriodSeconds, int sessionStartSecondsET,
                       std::function<void(const Bar&)> onBarClose)
        : m_barPeriodUs(barPeriodSeconds * 1'000'000LL),
          m_sessionStartSecondsET(sessionStartSecondsET),
          m_onBarClose(std::move(onBarClose)) {}

    // askVolume/bidVolume may be 0 if unavailable -- per-tick aggressor
    // classification is the caller's own responsibility.
    void OnTick(int64_t timestampUs, double price, int64_t volume,
                int64_t askVolume, int64_t bidVolume) {
        const int64_t bucket = BucketForTimestamp(timestampUs);
        if (!m_haveBar) {
            StartBar(bucket, timestampUs, price);
        } else if (bucket != m_currentBucket) {
            EmitCurrentBar();
            StartBar(bucket, timestampUs, price);
        }
        m_bar.high = std::max(m_bar.high, price);
        m_bar.low = std::min(m_bar.low, price);
        m_bar.close = price;
        m_bar.closeTimeUs = timestampUs;
        m_bar.volume += volume;
        m_bar.askVolume += askVolume;
        m_bar.bidVolume += bidVolume;
        ++m_bar.numTrades;
    }

    // Call once after the last tick to flush any still-open bar.
    void Flush() {
        if (m_haveBar) {
            EmitCurrentBar();
            m_haveBar = false;
        }
    }

private:
    // ET offset only changes twice a year (DST transitions) -- recomputing
    // GetEasternUtcOffsetSeconds()'s full civil-calendar math on every tick
    // would be wasted work at real tick volumes (471.9M+ ticks); cache it per
    // UTC calendar day instead, invalidated only when the day changes.
    int64_t BucketForTimestamp(int64_t timestampUs) {
        const int64_t utcSeconds = timestampUs / 1'000'000LL;
        const int64_t utcDay = FloorDiv(utcSeconds, 86400LL);
        if (utcDay != m_cachedOffsetDay) {
            m_cachedOffsetSeconds = ete::GetEasternUtcOffsetSeconds(utcSeconds);
            m_cachedOffsetDay = utcDay;
        }
        const int64_t etUs = timestampUs + static_cast<int64_t>(m_cachedOffsetSeconds) * 1'000'000LL;
        const int64_t phaseUs = static_cast<int64_t>(m_sessionStartSecondsET) * 1'000'000LL;
        return FloorDiv(etUs - phaseUs, m_barPeriodUs);
    }

    static int64_t FloorDiv(int64_t a, int64_t b) {
        const int64_t q = a / b;
        const int64_t r = a % b;
        return (r != 0 && ((r < 0) != (b < 0))) ? q - 1 : q;
    }

    void StartBar(int64_t bucket, int64_t timestampUs, double price) {
        m_currentBucket = bucket;
        m_bar = Bar{};
        m_bar.openTimeUs = timestampUs;
        m_bar.closeTimeUs = timestampUs;
        m_bar.open = price;
        m_bar.high = price;
        m_bar.low = price;
        m_bar.close = price;
        m_haveBar = true;
    }

    void EmitCurrentBar() {
        if (m_onBarClose) m_onBarClose(m_bar);
    }

    int64_t m_barPeriodUs;
    int m_sessionStartSecondsET;
    std::function<void(const Bar&)> m_onBarClose;
    bool m_haveBar = false;
    int64_t m_currentBucket = 0;
    Bar m_bar;
    int64_t m_cachedOffsetDay = std::numeric_limits<int64_t>::min();
    int m_cachedOffsetSeconds = 0;
};

}  // namespace tba
