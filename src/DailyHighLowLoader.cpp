#include "MindfulTrader_Precompiled.h"
#include "Logger.h"
#include <ctime>
#include <iomanip>

DailyHighLowLoader& DailyHighLowLoader::Instance()
{
    static DailyHighLowLoader singletonInstance;
    return singletonInstance;
}

DailyHighLowLoader::DailyHighLowLoader()
    : m_isLoaded(false)
{
    // Auto-load from hardcoded path
    LoadFromFile();
}

bool DailyHighLowLoader::LoadFromFile()
{
    m_dataMap.clear();
    m_isLoaded = false;

    constexpr const char* kDailyHighLowPath = "C:/Trading/data/daily_high_low.csv";
    std::ifstream file(kDailyHighLowPath);

    // DIAGNOSTIC: File not found - log error via Logger
    if (!file.is_open()) {
        Logger::getInstance().log("[CRITICAL] DailyHighLowLoader: CSV file not found at C:/Trading/data/daily_high_low.csv. Dimensions 6-7 will be ZERO.");
        return false;
    }

    // Read header line (skip it)
    std::string headerLine;
    if (!std::getline(file, headerLine)) {
        file.close();
        return false;
    }

    // Read data lines
    std::string line;
    int loadedCount = 0;
    int skippedCount = 0;

    while (std::getline(file, line)) {
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#') {
            continue;
        }

        // Parse line
        int dateKey;
        DailyHighLowData data;
        SCString scLine(line.c_str());

        if (ParseCsvLine(scLine, dateKey, data)) {
            m_dataMap[dateKey] = data;
            loadedCount++;
        } else {
            skippedCount++;
        }
    }

    file.close();

    // INSTITUTIONAL VALIDATION: Verify we loaded meaningful data
    m_isLoaded = (m_dataMap.size() > 0);

    if (m_isLoaded) {
        // Log SUCCESS via Logger (log once on init)
        std::string msg = "[INFO] DailyHighLowLoader: Loaded " + std::to_string(loadedCount)
                        + " dates from " + std::string(kDailyHighLowPath) + " (skipped " + std::to_string(skippedCount) + " lines)";
        Logger::getInstance().log(msg);
    } else {
        // FAIL: CSV loaded but no valid lines parsed
        Logger::getInstance().log("[CRITICAL] DailyHighLowLoader: CSV found but no valid data lines parsed. Check format.");
    }

    return m_isLoaded;
}

bool DailyHighLowLoader::ParseCsvLine(const SCString& line, int& outDateKey, DailyHighLowData& outData)
{
    // Expected format: "2024-01-02,6125.50,6050.25"

    // Create non-const copy for Tokenize (method is not const)
    SCString lineCopy = line;
    std::vector<char*> tokens;
    lineCopy.Tokenize(",", tokens);

    if (tokens.size() != 3) {
        return false;
    }

    try {
        // Parse date: "2024-01-02" -> 20240102
        SCString dateStr(tokens[0]);
        std::vector<char*> dateParts;
        dateStr.Tokenize("-", dateParts);

        if (dateParts.size() != 3) {
            return false;
        }

        int year = atoi(dateParts[0]);
        int month = atoi(dateParts[1]);
        int day = atoi(dateParts[2]);

        if (year < 2000 || year > 2100 || month < 1 || month > 12 || day < 1 || day > 31) {
            return false;
        }

        outDateKey = year * 10000 + month * 100 + day;

        // Parse high and low
        outData.prevDayHigh = atof(tokens[1]);
        outData.prevDayLow = atof(tokens[2]);

        // Sanity check: high should be >= low and both should be positive
        if (outData.prevDayHigh < outData.prevDayLow || outData.prevDayHigh <= 0.0 || outData.prevDayLow <= 0.0) {
            return false;
        }

        return true;

    } catch (...) {
        return false;
    }
}

int DailyHighLowLoader::DateToKey(const SCDateTime& date) const
{
    int year, month, day;
    date.GetDateYMD(year, month, day);
    return year * 10000 + month * 100 + day;
}

DailyHighLowData DailyHighLowLoader::GetDataForDate(const SCDateTime& date) const
{
    int dateKey = DateToKey(date);

    // Lookup date
    auto it = m_dataMap.find(dateKey);
    if (it != m_dataMap.end()) {
        return it->second;
    }

    return DailyHighLowData();  // Return zero if not found
}

DailyHighLowData DailyHighLowLoader::GetMostRecentData() const
{
    if (m_dataMap.empty()) {
        return DailyHighLowData();
    }

    // Map is sorted by key (date), so last element is most recent
    return m_dataMap.rbegin()->second;
}
