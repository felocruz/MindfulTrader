// BarKeyedLogThrottle.h -- edge-triggered, exponential-backoff log throttle
// keyed on a BAR INDEX (e.g. sc.Index), not wall-clock time and not raw
// tick/event count.
//
// Why bar index, not wall-clock: this codebase runs both live and at
// variable-speed historical replay (a single session can compress ~12 real
// calendar days into a few real minutes, or run 1:1). Wall-clock-based
// throttling (once per N real seconds) is WRONG here -- it fires at wildly
// different *market-time* rates depending on replay speed, and misbehaves
// identically during a paused/slow live session. A bar close is a fixed
// unit of market time regardless of how fast ticks arrive, true in both
// live and replay.
//
// Why bar index, not raw tick/event count: tick density varies with volume
// (quiet pre-market vs. a news-driven burst), so "every N ticks" is noisy --
// it under-throttles during high-volume periods and over-throttles during
// quiet ones. This codebase already had one confirmed bug from conflating
// these clocks (`regime_tenure` counting ticks instead of bars, despite its
// own doc comments -- see CLAUDE.md).
//
// This is a pure state-transition function, not a class -- callers own all
// state storage, so it works identically whether that storage is a plain
// struct member (ContextManager.cpp style) or ACSIL persistent ints via
// sc.GetPersistentInt/SetPersistentInt (EventDataCollectorStudy.cpp style).

#pragma once

#include <algorithm>
#include <cstdint>

namespace mts::log_throttle {

enum class BarKeyedEvent {
    kNone,       // no log-worthy transition this call
    kEntered,    // condition just became blocked -- log once, with full context
    kHeartbeat,  // still blocked, backoff interval elapsed -- log a reminder
    kCleared,    // condition just became unblocked -- log once, with run summary
};

// Caller-owned state (persist across calls):
//   wasBlocked        -- true if blocked on the previous call
//   enteredAtBar       -- bar index at which the CURRENT blocked run started
//   lastHeartbeatBar   -- bar index of the most recent kEntered/kHeartbeat event
//   nextIntervalBars   -- bars until the next kHeartbeat is due (doubles each time,
//                         capped at maxIntervalBars -- so a persistently-stuck
//                         condition tapers toward silence instead of spamming
//                         linearly with tick volume forever)
//   runBlockCount      -- cumulative blocked calls in the CURRENT run only
//                         (resets to 0 on kCleared) -- NOT the same as a
//                         whole-session cumulative counter, which callers
//                         should track separately if they need one.
inline BarKeyedEvent UpdateBarKeyedLock(
    bool blocked, int64_t barIndex,
    bool& wasBlocked, int64_t& enteredAtBar, int64_t& lastHeartbeatBar,
    int64_t& nextIntervalBars, int64_t& runBlockCount,
    int64_t initialIntervalBars = 1, int64_t maxIntervalBars = 256) {
    if (blocked) {
        ++runBlockCount;
        if (!wasBlocked) {
            wasBlocked = true;
            enteredAtBar = barIndex;
            lastHeartbeatBar = barIndex;
            nextIntervalBars = initialIntervalBars;
            return BarKeyedEvent::kEntered;
        }
        if (barIndex - lastHeartbeatBar >= nextIntervalBars) {
            lastHeartbeatBar = barIndex;
            nextIntervalBars = std::min(nextIntervalBars * 2, maxIntervalBars);
            return BarKeyedEvent::kHeartbeat;
        }
        return BarKeyedEvent::kNone;
    }
    if (wasBlocked) {
        wasBlocked = false;
        return BarKeyedEvent::kCleared;
    }
    runBlockCount = 0;
    return BarKeyedEvent::kNone;
}

}  // namespace mts::log_throttle
