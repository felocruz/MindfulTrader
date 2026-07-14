#pragma once

#include <string>

class ConfigManager {
public:
    // Provides global access to the singleton instance
    static ConfigManager& Instance();

    // Delete copy constructor and assignment operator to prevent duplication
    ConfigManager(const ConfigManager&) = delete;
    void operator=(const ConfigManager&) = delete;

    // Getters that will trigger a lazy load on first call
    double GetPreviousDayHigh();
    double GetPreviousDayLow();

private:
    // Private constructor for the singleton pattern
    ConfigManager() = default;

    // Loads configuration data from the file
    void LoadConfig();

    bool m_isLoaded = false;
    double m_previousDayHigh = 0.0;
    double m_previousDayLow = 0.0;
    const std::string m_configPath = "C:/Trading/sc_config.json";
};
