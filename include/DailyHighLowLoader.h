#pragma once

#include "sierrachart.h"
#include <map>
#include <string>

/**
 * @brief Data structure for a single day's high/low values
 * 
 * Represents the daily session high and low for a specific trading date.
 * Used for daily bias calculation and structure detection.
 * NOTE: When retrieved via GetDataForDate(previous_day), these fields represent
 * the PREVIOUS day's high and low values - naming updated to clarify this intent.
 */
struct DailyHighLowData {
    double prevDayHigh;    // Previous day's session high (from CSV column 2)
    double prevDayLow;     // Previous day's session low (from CSV column 3)
    
    DailyHighLowData() 
        : prevDayHigh(0.0), prevDayLow(0.0) {}
    
    DailyHighLowData(double h, double l)
        : prevDayHigh(h), prevDayLow(l) {}
};

// Forward declaration for logging
class Logger;

/**
 * @brief Singleton that loads and provides access to daily high/low historical data from CSV
 * 
 * This class reads the daily_high_low.csv file at initialization and provides
 * fast date-based lookups for daily bias and structure calculations. The CSV is 
 * expected to have the format:
 * 
 * date,high,low
 * 2024-01-02,6125.50,6050.25
 * 2024-01-03,6150.75,6075.00
 * ...
 * 
 * Usage:
 * @code
 * // Data is automatically loaded from C:/Trading/data/daily_high_low.csv
 * DailyHighLowData data = DailyHighLowLoader::Instance().GetDataForDate(sc.BaseDateTimeIn[sc.Index] - 1.0);
 * double prevHigh = data.prevDayHigh;
 * double prevLow = data.prevDayLow;
 * @endcode
 */
class DailyHighLowLoader {
public:
    static DailyHighLowLoader& Instance();
    DailyHighLowLoader(const DailyHighLowLoader&) = delete;
    DailyHighLowLoader& operator=(const DailyHighLowLoader&) = delete;
    
    /**
     * @brief Get high/low for a specific date
     * @param date SCDateTime object (uses date component only, ignores time)
     * @return DailyHighLowData structure (returns 0.0, 0.0 if date not found)
     */
    DailyHighLowData GetDataForDate(const SCDateTime& date) const;
    
    /**
     * @brief Check if data was loaded successfully
     * @return true if CSV loaded and contains data
     */
    bool IsLoaded() const { return m_isLoaded; }
    
    /**
     * @brief Get number of dates loaded
     * @return Count of trading days in dataset
     */
    int GetDataCount() const { return static_cast<int>(m_dataMap.size()); }
    
    /**
     * @brief Get the most recent (latest) date's high/low data
     * @return DailyHighLowData for most recent date (0.0, 0.0 if no data loaded)
     */
    DailyHighLowData GetMostRecentData() const;
    
private:
    DailyHighLowLoader();
    ~DailyHighLowLoader() = default;
    
    /**
     * @brief Load CSV file and parse data (called automatically in constructor)
     * @return true if file loaded successfully
     */
    bool LoadFromFile();
    
    /**
     * @brief Parse a single CSV line into date key and data
     * @param line CSV line (format: "2024-01-02,6125.50,6050.25")
     * @param outDateKey Output: date as YYYYMMDD integer key
     * @param outData Output: parsed high/low data
     * @return true if parse successful
     */
    bool ParseCsvLine(const SCString& line, int& outDateKey, DailyHighLowData& outData);
    
    /**
     * @brief Convert SCDateTime to YYYYMMDD integer key
     * @param date SCDateTime object
     * @return Date as integer (e.g., 20240102)
     */
    int DateToKey(const SCDateTime& date) const;
    
    std::map<int, DailyHighLowData> m_dataMap;  // Key: YYYYMMDD, Value: high/low
    bool m_isLoaded = false;
};
