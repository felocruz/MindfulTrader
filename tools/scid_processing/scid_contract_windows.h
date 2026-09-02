// tools/tick_pipeline/scid_contract_windows.h
// Contract discovery, roll-date math, quarterly-cycle validation. Ported
// byte-for-byte from lbrnet/data/mes_continuous.py's third_friday()/
// contract_active_end()/parse_contract_filename()/discover_contracts() --
// see docs/superpowers/specs/2026-09-02-scid-tick-parquet-cpp-tool-spec.md §2.
//
// The 4-calendar-days-before-3rd-Friday roll rule is Sierra Chart's own
// configured rollover for the MES?##-CME symbol (SymbolSettings.
// scdataallservices.xml: rollover-method=Method3, rollover-input-1=4,
// rollover-input-2=3), not an independently-derived generic CME convention
// -- see lbrnet's docs/superpowers/specs/2026-08-08-mes-continuous-bars-
// design.md for the dual verification (Symbol Settings UI tooltip + the
// XML config file).
//
// Date math uses Howard Hinnant's civil-calendar day-count algorithms
// (http://howardhinnant.github.io/date_algorithms.html, public domain)
// rather than std::chrono's calendar types, which require C++20 --
// this repo's tools/ baseline is C++17.
#ifndef TOOLS_SCID_CONTRACT_WINDOWS_H
#define TOOLS_SCID_CONTRACT_WINDOWS_H

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <optional>
#include <regex>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace scid {

constexpr int kRolloverDaysBeforeExpiry = 4;

// Days since 1970-01-01 for the proleptic Gregorian civil date (y, m, d).
inline std::int64_t DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= (m <= 2) ? 1 : 0;
    const std::int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

// Inverse of DaysFromCivil.
inline void CivilFromDays(std::int64_t z, int* y, unsigned* m, unsigned* d) {
    z += 719468;
    const std::int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = static_cast<unsigned>(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const std::int64_t y_ = static_cast<std::int64_t>(yoe) + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp + (mp < 10 ? 3 : static_cast<unsigned>(-9));
    *y = static_cast<int>(y_ + (*m <= 2 ? 1 : 0));
}

// Day-of-week for a day count, "C" encoding: Sunday=0 .. Saturday=6.
inline unsigned WeekdayFromDays(std::int64_t z) {
    return static_cast<unsigned>(z >= -4 ? (z + 4) % 7 : (z + 5) % 7 + 6);
}

struct CivilDate {
    int year;
    unsigned month;
    unsigned day;
    bool operator==(const CivilDate& other) const {
        return year == other.year && month == other.month && day == other.day;
    }
};

inline std::int64_t CivilDateDays(const CivilDate& d) { return DaysFromCivil(d.year, d.month, d.day); }

inline CivilDate CivilDateFromDays(std::int64_t z) {
    CivilDate d{};
    CivilFromDays(z, &d.year, &d.month, &d.day);
    return d;
}

// Matches mes_continuous.py's CONTRACT_MONTH_CODES.
inline int MonthCodeToMonth(char month_code) {
    switch (month_code) {
        case 'H': return 3;
        case 'M': return 6;
        case 'U': return 9;
        case 'Z': return 12;
        default: throw std::invalid_argument(std::string("unknown contract month code: ") + month_code);
    }
}

// Matches mes_continuous.py's _MONTH_CYCLE index-after-this-one lookup.
inline int NextCycleIndex(char month_code) {
    static const std::string kCycle = "HMUZ";
    const auto pos = kCycle.find(month_code);
    if (pos == std::string::npos) {
        throw std::invalid_argument(std::string("unknown contract month code: ") + month_code);
    }
    return static_cast<int>((pos + 1) % 4);
}

// 3rd Friday of the given month (matches mes_continuous.py's third_friday()):
// first_friday_offset = (4 - first_of_month.weekday()) % 7 in Python's
// Monday=0..Sunday=6 weekday encoding, then +14 days.
inline CivilDate ThirdFriday(int year, int month) {
    const std::int64_t first_of_month = DaysFromCivil(year, static_cast<unsigned>(month), 1);
    const unsigned c_weekday = WeekdayFromDays(first_of_month);          // Sunday=0..Saturday=6
    const int python_weekday = (static_cast<int>(c_weekday) + 6) % 7;    // Monday=0..Sunday=6
    const int first_friday_offset = ((4 - python_weekday) % 7 + 7) % 7;  // Python's non-negative %
    return CivilDateFromDays(first_of_month + first_friday_offset + 14);
}

// 3rd Friday minus kRolloverDaysBeforeExpiry days (mes_continuous.py's contract_active_end()).
inline CivilDate ContractActiveEnd(char month_code, int year) {
    const int month = MonthCodeToMonth(month_code);
    const std::int64_t third = CivilDateDays(ThirdFriday(year, month));
    return CivilDateFromDays(third - kRolloverDaysBeforeExpiry);
}

// Matches mes_continuous.py's _CONTRACT_FILENAME_RE: ^MES([HMUZ])(\d{2})-CME\.scid$
inline std::optional<std::pair<char, int>> ParseContractFilename(std::string_view name) {
    static const std::regex kRe(R"(^MES([HMUZ])(\d{2})-CME\.scid$)");
    std::match_results<std::string_view::const_iterator> m;
    if (!std::regex_match(name.begin(), name.end(), m, kRe)) {
        return std::nullopt;
    }
    const char month_code = m[1].str()[0];
    const int year = 2000 + std::stoi(m[2].str());
    return std::make_pair(month_code, year);
}

// UTC-midnight POSIX microseconds for a civil date (matches mes_continuous.py's _date_to_posix_us()).
inline std::int64_t DateToPosixUs(const CivilDate& d) {
    return DaysFromCivil(d.year, d.month, d.day) * 86400LL * 1'000'000LL;
}

struct ContractWindow {
    std::string path;
    char month_code;
    int year;
    std::int64_t active_start_us;
    std::optional<std::int64_t> active_end_us;
};

inline std::string ContractLabel(char month_code, int year) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%c%02d", month_code, year % 100);
    return std::string(buf);
}

// Globs MES*-CME.scid in data_dir, parses/sorts by active_end date, validates
// the quarterly H/M/U/Z cycle has no gaps and no duplicate active_end dates,
// and returns non-overlapping windows with the most-recent contract's
// active_end_us = nullopt (open-ended). Matches mes_continuous.py's
// discover_contracts() exactly.
inline std::vector<ContractWindow> DiscoverContracts(const std::string& data_dir) {
    struct Candidate {
        CivilDate active_end;
        char month_code;
        int year;
        std::string path;
    };
    std::vector<Candidate> candidates;
    for (const auto& entry : std::filesystem::directory_iterator(data_dir)) {
        if (!entry.is_regular_file()) continue;
        const std::string name = entry.path().filename().string();
        const auto parsed = ParseContractFilename(name);
        if (!parsed.has_value()) continue;
        const auto [month_code, year] = *parsed;
        candidates.push_back({ContractActiveEnd(month_code, year), month_code, year, entry.path().string()});
    }

    if (candidates.empty()) {
        throw std::runtime_error("No MES[HMUZ]##-CME.scid contract files found in " + data_dir);
    }

    std::sort(candidates.begin(), candidates.end(), [](const Candidate& a, const Candidate& b) {
        return CivilDateDays(a.active_end) < CivilDateDays(b.active_end);
    });

    for (std::size_t i = 1; i < candidates.size(); ++i) {
        const Candidate& prev = candidates[i - 1];
        const Candidate& cur = candidates[i];
        if (cur.active_end == prev.active_end) {
            throw std::invalid_argument("Duplicate contract active_end date for " +
                                         std::filesystem::path(cur.path).filename().string());
        }
        const int expected_idx = NextCycleIndex(prev.month_code);
        static const std::string kCycle = "HMUZ";
        const char expected_month = kCycle[static_cast<std::size_t>(expected_idx)];
        const int expected_year = expected_idx == 0 ? prev.year + 1 : prev.year;
        if (cur.month_code != expected_month || cur.year != expected_year) {
            throw std::invalid_argument(
                "Missing contract between " + ContractLabel(prev.month_code, prev.year) + " and " +
                ContractLabel(cur.month_code, cur.year) + " -- expected " +
                ContractLabel(expected_month, expected_year) + " next in the quarterly H/M/U/Z cycle");
        }
    }

    std::vector<ContractWindow> windows;
    windows.reserve(candidates.size());
    std::optional<std::int64_t> prev_end_us;
    for (std::size_t i = 0; i < candidates.size(); ++i) {
        const Candidate& c = candidates[i];
        const std::int64_t active_end_us = DateToPosixUs(c.active_end);
        const bool is_last = i + 1 == candidates.size();
        windows.push_back(ContractWindow{
            c.path, c.month_code, c.year, prev_end_us.value_or(0),
            is_last ? std::nullopt : std::optional<std::int64_t>(active_end_us)});
        prev_end_us = active_end_us;
    }
    return windows;
}

}  // namespace scid

#endif  // TOOLS_SCID_CONTRACT_WINDOWS_H
