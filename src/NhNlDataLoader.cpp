#include "NhNlDataLoader.h"
#include "Logger.h"
#include <fstream>
#include <sstream>
#include <vector>

namespace {
constexpr const char* kNhNlPrimaryCsvPath = "C:/Trading/data/NH_NL.csv";
}

const char* NhNlDataLoader::PrimaryCsvPath()
{
    return kNhNlPrimaryCsvPath;
}

NhNlDataLoader::NhNlDataLoader()
    : m_isLoaded(false)
    , m_weeklyProvided(false)
{
}

NhNlDataLoader::~NhNlDataLoader()
{
}

bool NhNlDataLoader::LoadDefaultPath(SCStudyInterfaceRef sc)
{
    return LoadFromFile(SCString(PrimaryCsvPath()), sc);
}

bool NhNlDataLoader::LoadFromFile(const SCString& csvPath, SCStudyInterfaceRef sc)
{
    m_dataMap.clear();
    m_isLoaded = false;
    m_weeklyProvided = false;

    std::ifstream file(csvPath.GetChars());
    if (!file.is_open()) {
        SCString msg;
        msg.Format("NhNlDataLoader: Failed to open file: %s", csvPath.GetChars());
        Logger::getInstance().log(msg.GetChars());
        return false;
    }

    std::string headerLine;
    if (!std::getline(file, headerLine)) {
        Logger::getInstance().log("NhNlDataLoader: File is empty");
        file.close();
        return false;
    }

    int lineCount = 0;
    int errorCount = 0;
    std::string line;

    while (std::getline(file, line)) {
        ++lineCount;

        if (line.empty() || line[0] == '#') {
            continue;
        }

        int dateKey;
        NhNlData data;
        SCString scLine(line.c_str());

        if (ParseCsvLine(scLine, dateKey, data)) {
            m_dataMap[dateKey] = data;
        } else {
            ++errorCount;
            if (errorCount <= 5) {
                SCString msg;
                msg.Format("NhNlDataLoader: Parse error on line %d: %s", lineCount, line.c_str());
                Logger::getInstance().log(msg.GetChars());
            }
        }
    }

    file.close();

    // Calculate weekly sums only when not explicitly provided
    if (!m_dataMap.empty()) {
        if (!m_weeklyProvided) {
            CalculateWeeklySums();
        }
        m_isLoaded = true;

        // Log success
        SCString msg;
        msg.Format("NhNlDataLoader: Loaded %d dates from %s (%d parse errors)",
                   GetDataCount(), csvPath.GetChars(), errorCount);
        Logger::getInstance().log(msg.GetChars());

        // Log date range
        SCDateTime firstDate, lastDate;
        if (GetDateRange(firstDate, lastDate)) {
            SCString rangeMsg;
            rangeMsg.Format("NhNlDataLoader: Date range: %s to %s",
                          sc.DateTimeToString(firstDate, FLAG_DT_COMPLETE_DATE).GetChars(),
                          sc.DateTimeToString(lastDate, FLAG_DT_COMPLETE_DATE).GetChars());
            Logger::getInstance().log(rangeMsg.GetChars());
        }

        return true;
    } else {
        Logger::getInstance().log("NhNlDataLoader: No valid data loaded");
        return false;
    }
}

bool NhNlDataLoader::ParseCsvLine(const SCString& line, int& outDate, NhNlData& outData)
{
    // Canonical format: date,nh_nl_daily,nh_nl_weekly,sp500_close

    // Create non-const copy for Tokenize (method is not const)
    SCString lineCopy = line;
    std::vector<char*> tokens;
    lineCopy.Tokenize(",", tokens);

    if (tokens.size() != 4) {
        return false;
    }

    try {
        // Parse date (YYYY-MM-DD format)
        SCString dateStr(tokens[0]);
        std::vector<char*> dateParts;
        dateStr.Tokenize("-", dateParts);

        if (dateParts.size() != 3) {
            return false;
        }

        int year = atoi(dateParts[0]);
        int month = atoi(dateParts[1]);
        int day = atoi(dateParts[2]);

        // Create date key (YYYYMMDD)
        outDate = year * 10000 + month * 100 + day;

        outData.nh_nl_daily = atoi(tokens[1]);
        outData.nh_nl_weekly = atoi(tokens[2]);
        outData.sp500_close = atof(tokens[3]);
        m_weeklyProvided = true;

        return true;

    } catch (...) {
        return false;
    }
}

void NhNlDataLoader::CalculateWeeklySums()
{
    // Calculate 7-day rolling sum for each date
    // This requires iterating in chronological order

    std::vector<int> sortedDates;
    sortedDates.reserve(m_dataMap.size());

    for (const auto& pair : m_dataMap) {
        sortedDates.push_back(pair.first);
    }

    // Map is already sorted by key (date), so we can iterate directly
    for (size_t i = 0; i < sortedDates.size(); ++i) {
        int currentDate = sortedDates[i];
        int weeklySum = 0;

        // Sum previous 7 days (including current day)
        for (int j = static_cast<int>(i); j >= 0 && j > static_cast<int>(i) - 7; --j) {
            int priorDate = sortedDates[j];
            weeklySum += m_dataMap[priorDate].nh_nl_daily;
        }

        m_dataMap[currentDate].nh_nl_weekly = weeklySum;
    }
}

NhNlData NhNlDataLoader::GetDataForDate(const SCDateTime& dateTime) const
{
    if (!m_isLoaded) {
        return NhNlData();  // Return zeros
    }

    int dateKey = DateTimeToDateKey(dateTime);

    auto it = m_dataMap.find(dateKey);
    if (it != m_dataMap.end()) {
        return it->second;
    }

    // Date not found - return zeros
    // This is normal for weekends, holidays, etc.
    return NhNlData();
}

bool NhNlDataLoader::TryGetDataForDate(
    const SCDateTime& dateTime,
    NhNlData& outData
) const
{
    outData = NhNlData();

    if (!m_isLoaded || m_dataMap.empty()) {
        return false;
    }

    const int dateKey = DateTimeToDateKey(dateTime);

    auto it = m_dataMap.find(dateKey);
    if (it != m_dataMap.end()) {
        outData = it->second;
        return true;
    }

    return false;
}

bool NhNlDataLoader::GetDateRange(SCDateTime& firstDate, SCDateTime& lastDate) const
{
    if (!m_isLoaded || m_dataMap.empty()) {
        return false;
    }

    // Get first and last keys
    int firstKey = m_dataMap.begin()->first;
    int lastKey = m_dataMap.rbegin()->first;

    // Convert to SCDateTime
    int firstYear = firstKey / 10000;
    int firstMonth = (firstKey / 100) % 100;
    int firstDay = firstKey % 100;

    int lastYear = lastKey / 10000;
    int lastMonth = (lastKey / 100) % 100;
    int lastDay = lastKey % 100;

    firstDate = SCDateTime(firstYear, firstMonth, firstDay, 0, 0, 0);
    lastDate = SCDateTime(lastYear, lastMonth, lastDay, 0, 0, 0);

    return true;
}

int NhNlDataLoader::DateTimeToDateKey(const SCDateTime& dateTime) const
{
    int year = 0;
    int month = 0;
    int day = 0;
    dateTime.GetDateYMD(year, month, day);

    return year * 10000 + month * 100 + day;
}

SCDateTime NhNlDataLoader::DateKeyToDateTime(int dateKey) const
{
    const int year = dateKey / 10000;
    const int month = (dateKey / 100) % 100;
    const int day = dateKey % 100;
    return SCDateTime(year, month, day, 0, 0, 0);
}
