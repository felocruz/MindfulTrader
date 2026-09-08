// EasternTimeOffset.h — pure, header-only US Eastern Time DST-aware UTC
// offset calculator. No Sierra Chart/ACSIL dependency, no C++20 <chrono>
// calendar types (this repo's baseline is C++17) -- uses Howard Hinnant's
// civil-calendar day-count algorithm (public domain,
// http://howardhinnant.github.io/date_algorithms.html), matching this
// repo's own established convention for date math outside C++20
// (/memories/repo/cpp_tools_conventions.md).
//
// Exists because tick data (tools/scid_processing/scid_reader.h's
// timestamp_us) is stored as UTC epoch microseconds, but Sierra Chart's
// real CME ES/MES session boundary (18:00 ET, EventDataCollectorStudy.cpp's
// CME_ES_SESSION_START_SECS) is defined in Eastern Time -- any tool that
// reconstructs Sierra Chart's own bar boundaries from raw UTC ticks needs a
// correct ET conversion, DST included.
//
// Valid 2007-onward (Energy Policy Act of 2005 DST rule: starts 2nd Sunday
// in March, ends 1st Sunday in November, both at 2:00 AM local) -- covers
// this project's entire real tick data range (2023-2026).

#pragma once

#include <cstdint>

namespace ete {

// Days since 1970-01-01 for a given proleptic-Gregorian civil date.
inline constexpr int64_t DaysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= (m <= 2) ? 1 : 0;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? 0u : 12u) - 3) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<int64_t>(doe) - 719468;
}

// Inverse of DaysFromCivil: civil (year, month, day) for a given day count
// since 1970-01-01.
inline void CivilFromDays(int64_t z, int64_t& y, unsigned& m, unsigned& d) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t year = static_cast<int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp + (mp < 10 ? 3 : -9);
    y = year + ((m <= 2) ? 1 : 0);
}

// UTC instant (epoch seconds) of the Nth Sunday of (year, month) at 2:00 AM
// local time, where localUtcOffsetHours is the offset in effect just BEFORE
// the transition (e.g. -5 for the March EST->EDT transition, -4 for the
// November EDT->EST transition).
inline int64_t NthSundayTransitionUtcSeconds(int64_t year, unsigned month, int nth,
                                              int localUtcOffsetHoursBeforeTransition) {
    const int64_t firstDay = DaysFromCivil(year, month, 1);
    // 1970-01-01 (day 0) was a Thursday; 0=Sunday..6=Saturday convention.
    const int64_t firstWeekday = ((firstDay % 7) + 7 + 4) % 7;
    const int64_t firstSunday = (firstWeekday == 0) ? 1 : (8 - firstWeekday);
    const unsigned sundayDate = static_cast<unsigned>(firstSunday + 7 * (nth - 1));
    const int64_t days = DaysFromCivil(year, month, sundayDate);
    return days * 86400 + (2 - localUtcOffsetHoursBeforeTransition) * 3600;
}

// Returns the US Eastern Time UTC offset in seconds for a given UTC epoch
// second: -18000 (EST, UTC-5) or -14400 (EDT, UTC-4).
inline int GetEasternUtcOffsetSeconds(int64_t utcEpochSeconds) {
    constexpr int64_t kSecPerDay = 86400;
    const int64_t days = (utcEpochSeconds >= 0)
        ? utcEpochSeconds / kSecPerDay
        : (utcEpochSeconds - kSecPerDay + 1) / kSecPerDay;
    int64_t year;
    unsigned month, day;
    CivilFromDays(days, year, month, day);

    const int64_t marchTransitionUtc = NthSundayTransitionUtcSeconds(year, 3, 2, -5);
    const int64_t novTransitionUtc = NthSundayTransitionUtcSeconds(year, 11, 1, -4);

    const bool isEdt = (utcEpochSeconds >= marchTransitionUtc) && (utcEpochSeconds < novTransitionUtc);
    return isEdt ? -14400 : -18000;
}

}  // namespace ete
