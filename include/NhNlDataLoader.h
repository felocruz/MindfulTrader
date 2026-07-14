#pragma once

#include "sierrachart.h"
#include <map>
#include <string>

/**
 * @brief Data structure for a single day's NH-NL values
 *
 * Canonical runtime contract:
 * - nh_nl_daily  : daily NH-NL net breadth index (previous trading day at runtime)
 * - nh_nl_weekly : weekly NH-NL context (typically rolling 7-trading-day sum)
 * - sp500_close  : S&P 500 close for the same date (context/diagnostics)
 */
struct NhNlData {
    int nh_nl_daily;    // Daily NH-NL value (raw)
    int nh_nl_weekly;   // Weekly NH-NL context (provided or derived)
    double sp500_close; // S&P 500 close
    
    NhNlData()
        : nh_nl_daily(0), nh_nl_weekly(0), sp500_close(0.0) {}
};

/**
 * @brief Loads and provides access to NH-NL historical data from CSV
 * 
 * This class reads the NH_NL.csv file at initialization and provides
 * fast date-based lookups for market regime calculation.
 *
 * Canonical CSV format:
 * date,nh_nl_daily,nh_nl_weekly,sp500_close
 * 2024-01-02,227,1284,4769.83
 * 2024-01-03,173,1165,4728.11
 * ...
 * 
 * Usage:
 * @code
 * NhNlDataLoader loader;
 * if (loader.LoadFromFile("C:/Trading/data/NH_NL.csv", sc)) {
 *     NhNlData data = loader.GetDataForDate(sc.BaseDateTimeIn[sc.Index]);
 *     if (abs(data.nh_nl_weekly) > 4000) {
 *         // EXTREME_DISLOCATION detected
 *     }
 * }
 * @endcode
 */
class NhNlDataLoader {
public:
    NhNlDataLoader();
    ~NhNlDataLoader();

    // Canonical path defaults (kept in loader to avoid path policy leakage into studies)
    static const char* PrimaryCsvPath();
    
    /**
     * @brief Load NH-NL data from CSV file
     * 
    * @param csvPath Absolute path to NH_NL.csv
     * @param sc Sierra Chart study interface for logging
     * @return true if file loaded successfully, false otherwise
     */
    bool LoadFromFile(const SCString& csvPath, SCStudyInterfaceRef sc);

    /**
    * @brief Load NH-NL data from the canonical deployment path.
     */
    bool LoadDefaultPath(SCStudyInterfaceRef sc);
    
    /**
     * @brief Get NH-NL data for a specific date
     * 
     * @param date SCDateTime representing the date to lookup
     * @return NhNlData structure (returns zeros if date not found)
     */
    NhNlData GetDataForDate(const SCDateTime& date) const;

    /**
        * @brief Try exact-date NH-NL lookup.
     *
     * @param date Target date
     * @param outData Output data when lookup succeeds
     * @return true when exact date exists in loaded dataset; false otherwise
     */
    bool TryGetDataForDate(
        const SCDateTime& date,
        NhNlData& outData
    ) const;
    
    /**
     * @brief Check if data is loaded and available
     * 
     * @return true if CSV was loaded successfully
     */
    bool IsLoaded() const { return m_isLoaded; }
    
    /**
     * @brief Get number of dates loaded from CSV
     * 
     * @return Count of data entries
     */
    int GetDataCount() const { return static_cast<int>(m_dataMap.size()); }
    
    /**
     * @brief Get date range of loaded data
     * 
     * @param firstDate Output: earliest date in dataset
     * @param lastDate Output: latest date in dataset
     * @return true if data is loaded, false otherwise
     */
    bool GetDateRange(SCDateTime& firstDate, SCDateTime& lastDate) const;

private:
    /**
     * @brief Parse a single CSV line into NhNlData
     *
    * Supported format:
    * - Canonical: date,nh_nl_daily,nh_nl_weekly,sp500_close
     *
     * @param line CSV line to parse
     * @param outDate Output: parsed date key (YYYYMMDD)
     * @param outData Output: parsed NH-NL data
     * @return true if parse successful
     */
    bool ParseCsvLine(const SCString& line, int& outDate, NhNlData& outData);
    
    /**
     * @brief Calculate 7-day rolling sum for weekly NH-NL
     *
     * Called when weekly values are not explicitly provided in source CSV.
     */
    void CalculateWeeklySums();
    
    /**
     * @brief Convert SCDateTime to integer date key (YYYYMMDD)
     * 
     * @param dateTime SCDateTime to convert
     * @return Integer date in YYYYMMDD format (e.g., 20240102)
     */
    int DateTimeToDateKey(const SCDateTime& dateTime) const;

    /**
     * @brief Convert integer date key (YYYYMMDD) to SCDateTime
     */
    SCDateTime DateKeyToDateTime(int dateKey) const;
    
private:
    std::map<int, NhNlData> m_dataMap;  // Key: YYYYMMDD, Value: NH-NL data
    bool m_isLoaded;
    bool m_weeklyProvided;
    SCDateTime m_firstDate;
    SCDateTime m_lastDate;
};
