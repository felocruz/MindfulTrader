#include "Logger.h"
#include <iostream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <cstring>

Logger& Logger::getInstance() {
    static Logger instance;
    return instance;
}

Logger::Logger() {
    try {
        const std::string logPath = "C:/Trading/logs/MindfulTrader.log";
        const std::string backupPath = "C:/Trading/logs/MindfulTrader.log.bak";

        // Remove old backup
        std::remove(backupPath.c_str());
        
        // Rename current log to backup
        std::rename(logPath.c_str(), backupPath.c_str());

        // Open new log file
        m_logFile.open(logPath, std::ios_base::app);
    }
    catch (...) {
        // Silently fail - don't crash Sierra Chart
    }
}

Logger::~Logger() {
    try {
        if (m_logFile.is_open()) {
            m_logFile.close();
        }
    }
    catch (...) {
        // Silently fail on shutdown
    }
}

void Logger::log(const std::string& message) {
    try {
        std::lock_guard<std::mutex> lock(m_mutex);
        
        if (!m_logFile.is_open()) {
            return;  // Silently return if file not open
        }

        auto now = std::chrono::system_clock::now();
        auto in_time_t = std::chrono::system_clock::to_time_t(now);
        
        std::tm buf = {};
#ifdef _WIN32
        localtime_s(&buf, &in_time_t);
#else
        localtime_r(&in_time_t, &buf);
#endif
        
        // Use simple string formatting to avoid std::put_time issues
        char timeStr[32];
        strftime(timeStr, sizeof(timeStr), "%Y-%m-%d %H:%M:%S", &buf);
        
        m_logFile << timeStr << " " << message << std::endl;
        
        // Flush to disk (important for crash diagnostics)
        if (m_logFile.good()) {
            m_logFile.flush();
        }
    }
    catch (...) {
        // Catch everything - never throw from Logger
        // Silent failure is better than crashing Sierra Chart
    }
}
