#include "MindfulTrader_Precompiled.h"
#include "ConfigManager.h"
#include <fstream>
#include "nlohmann/json.hpp"

ConfigManager& ConfigManager::Instance()
{
    static ConfigManager instance;
    return instance;
}

void ConfigManager::LoadConfig()
{
    std::ifstream configFile(m_configPath);
    if (!configFile.is_open()) {
        Logger::getInstance().log("ERROR: ConfigManager - Could not open config file: " + m_configPath);
        m_isLoaded = true; // Mark as loaded to prevent re-attempts
        return;
    }

    try {
        nlohmann::json configJson;
        configFile >> configJson;

        m_previousDayHigh = configJson.value("previous_day_high", 0.0);
        m_previousDayLow = configJson.value("previous_day_low", 0.0);

        Logger::getInstance().log("ConfigManager - Configuration loaded successfully from " + m_configPath);
    }
    catch (const nlohmann::json::parse_error& e) {
        Logger::getInstance().log("ERROR: ConfigManager - JSON parse error: " + std::string(e.what()));
    }
    catch (const std::exception& e) {
        Logger::getInstance().log("ERROR: ConfigManager - An unexpected error occurred: " + std::string(e.what()));
    }

    m_isLoaded = true; // Mark as loaded even if parsing fails to prevent re-attempts
}

double ConfigManager::GetPreviousDayHigh()
{
    if (!m_isLoaded) {
        LoadConfig();
    }
    return m_previousDayHigh;
}

double ConfigManager::GetPreviousDayLow()
{
    if (!m_isLoaded) {
        LoadConfig();
    }
    return m_previousDayLow;
}
