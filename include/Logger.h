#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <chrono>
#include <memory>

class Logger {
public:
    static Logger& getInstance();
    void log(const std::string& message);

private:
    Logger();
    ~Logger();
    Logger(const Logger&) = delete;
    Logger& operator=(const Logger&) = delete;

    std::ofstream m_logFile;
    std::mutex m_mutex;
};
