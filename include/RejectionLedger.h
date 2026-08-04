#pragma once

#include <string>
#include <fstream>
#include <mutex>
#include <ctime>
#include <chrono>
#include "nlohmann/json.hpp"
#include "Logger.h"

// ═══════════════════════════════════════════════════════════════════════════════
// RejectionLedger — Structured trade-rejection telemetry (JSONL)
//
// PAER Phase 1: Every gate failure in ValidateOrder() emits a structured
// JSON record to C:/Trading/logs/rejection_ledger.jsonl.
//
// Design:
//   - Always-on (sim and live are identical from ACSIL's perspective)
//   - Writes ONLY on rejections (failure path, not hot path)
//   - ~500 bytes per record, 10-50 per session → negligible I/O
//   - Thread-safe via mutex (same pattern as Logger)
//   - Records are self-contained: identity + what was rejected + which gate +
//     metric evidence + full market snapshot
//
// The Rejection Ledger is the denominator dataset for counterfactual analysis.
// Without it, AER tuning is blind.
// ═══════════════════════════════════════════════════════════════════════════════

// ─── Sizing Bucket Decomposition (PAER §8.4) ───
// Groups the 17-factor sizing chain into semantic buckets so the counterfactual
// analyzer can answer "WHY was the size X?" not just "the size was X."
// Each bucket is the product of its constituent factors (1.0 = no effect).
struct SizingBuckets {
    double model    = 1.0;  // paretoMultiplier × convictionScale × thesisMultiplier × entropyDiscount
    double regime   = 1.0;  // hmmRisk × hmmDuration × mahalanobisCap × tailWeightDiscount
    double market   = 1.0;  // deepContextMultiplier × atrVolatility
    double safety   = 1.0;  // kurtosisCeiling × tripleConfirmZero × consecutiveLossHalt × hillGate
    double timing   = 1.0;  // freshnessDiscount × sessionWindDown
    double reward   = 1.0;  // cascadeBoost × hillOpportunityBoost
    double final_rm = 1.0;  // full riskMultiplier (product of all)

    nlohmann::json ToJson() const {
        return {
            {"model",    model},
            {"regime",   regime},
            {"market",   market},
            {"safety",   safety},
            {"timing",   timing},
            {"reward",   reward},
            {"final",    final_rm}
        };
    }
};

struct RejectionRecord {
    // ─── Identity ───
    uint64_t    timestampUs     = 0;
    std::string timestampHuman;
    std::string paramSetId;
    std::string sessionDate;

    // ─── What Was Rejected ───
    std::string action;             // e.g. "ENTER_LONG", "ENTER_SHORT"
    std::string patternEnum;        // e.g. "ELDER_BREAKOUT_BUY"
    float       modelConfidence = 0.0f;
    double      entryPrice      = 0.0;
    double      stopPrice       = 0.0;
    int         proposedQuantity = 0;
    double      orderRiskDollars = 0.0;

    // ─── Which Gate Rejected ───
    std::string gate;               // e.g. "MONTHLY_LIMIT", "LATENCY_GATE"
    int         gateOrdinal     = 0;     // Position in ValidateOrder chain (1-10)
    std::string reasonCode;         // e.g. "ELDER_6PCT_RULE"
    int         reasonId        = 0;     // Numeric code from Result<void>::Failure
    std::string validator;          // e.g. "ValidateMonthlyLimit"

    // ─── Metric Evidence (actual vs threshold) ───
    std::string metricName;         // e.g. "monthlyLossPct"
    double      metricValue     = 0.0;
    double      thresholdValue  = 0.0;
    std::string thresholdParam;     // e.g. "elder_6pct_rule_frac"
    double      margin          = 0.0;   // |metricValue - thresholdValue|

    // ─── Market Snapshot (from LocalRiskContext) ───
    struct ContextSnapshot {
        float amihudIlliquidity = 0.0f;
        float spreadStress      = 0.0f;
        float shannonFlowEntropy = 0.0f;
        float shannonEfficiency = 0.0f;
        float talebKurtosis     = 0.0f;
        float talebSkewness     = 0.0f;
        float elderChandelierATR = 0.0f;
        float paretoTailAlpha   = 0.0f;
        float hurstExponent     = 0.0f;
        float raschkeBurst      = 0.0f;
        float fisherInfo        = 0.0f;
        int   regimeDuration    = 0;

        // Account/session state
        double dailyPnL         = 0.0;
        double accountEquity    = 0.0;
        int    tradesToday      = 0;
        int    consecutiveLosses = 0;
        int    currentTimeMinutes = 0;
    } context;

    // ─── Sizing Decomposition (PAER §8.4) ───
    // Only populated for SILENT_ZERO_SIZING and sizing-path records.
    SizingBuckets sizingBuckets;

    nlohmann::json ToJson() const {
        nlohmann::json j;

        // Identity
        j["timestamp_us"]     = timestampUs;
        j["timestamp_human"]  = timestampHuman;
        j["param_set_id"]     = paramSetId;
        j["session_date"]     = sessionDate;

        // What was rejected
        j["action"]            = action;
        j["pattern_enum"]      = patternEnum;
        j["model_confidence"]  = modelConfidence;
        j["entry_price"]       = entryPrice;
        j["stop_price"]        = stopPrice;
        j["proposed_quantity"]  = proposedQuantity;
        j["order_risk_dollars"] = orderRiskDollars;

        // Which gate rejected
        j["gate"]              = gate;
        j["gate_ordinal"]      = gateOrdinal;
        j["reason_code"]       = reasonCode;
        j["reason_id"]         = reasonId;
        j["validator"]         = validator;

        // Metric evidence
        j["metric_name"]       = metricName;
        j["metric_value"]      = metricValue;
        j["threshold_value"]   = thresholdValue;
        j["threshold_param"]   = thresholdParam;
        j["margin"]            = margin;

        // Market snapshot
        j["context"] = {
            {"amihud_illiquidity", context.amihudIlliquidity},
            {"spread_stress",       context.spreadStress},
            {"shannon_flow_entropy", context.shannonFlowEntropy},
            {"shannon_efficiency",  context.shannonEfficiency},
            {"taleb_kurtosis",      context.talebKurtosis},
            {"taleb_skewness",      context.talebSkewness},
            {"elder_chandelier_atr", context.elderChandelierATR},
            {"pareto_tail_alpha",   context.paretoTailAlpha},
            {"hurst_exponent",      context.hurstExponent},
            {"raschke_burst",       context.raschkeBurst},
            {"fisher_info",         context.fisherInfo},
            {"regime_duration",     context.regimeDuration},
            {"daily_pnl",           context.dailyPnL},
            {"account_equity",      context.accountEquity},
            {"trades_today",        context.tradesToday},
            {"consecutive_losses",  context.consecutiveLosses},
            {"current_time_minutes", context.currentTimeMinutes}
        };

        // Sizing decomposition (only meaningful for sizing-path records)
        if (sizingBuckets.final_rm != 1.0 || gate == "SILENT_ZERO_SIZING") {
            j["sizing_buckets"] = sizingBuckets.ToJson();
        }

        return j;
    }
};

class RejectionLedger {
public:
    static RejectionLedger& Instance() {
        static RejectionLedger instance;
        return instance;
    }

    // Call once from RiskManager::Init()
    void Init(const std::string& path = kDefaultPath) {
        std::lock_guard<std::mutex> lock(m_mutex);

        if (m_initialized) return;

        m_filePath = path;
        m_stream.open(path, std::ios::app);  // Append mode — preserves previous session data

        if (m_stream.is_open()) {
            m_initialized = true;
            Logger::getInstance().log(
                "RejectionLedger: Initialized at " + path);
        } else {
            Logger::getInstance().log(
                "RejectionLedger: FAILED to open " + path + " — rejections will be logged only");
        }
    }

    // Primary write method — thread-safe, append one JSONL line
    void Record(const RejectionRecord& record) {
        std::lock_guard<std::mutex> lock(m_mutex);
        ++m_recordCount;

        if (!m_stream.is_open()) return;

        try {
            m_stream << record.ToJson().dump() << '\n';
            m_stream.flush();
        } catch (const std::exception& e) {
            Logger::getInstance().log(
                "RejectionLedger: Write failed: " + std::string(e.what()));
        }
    }

    // Session summary for log — call at end of day or on halt
    uint64_t GetRecordCount() const { return m_recordCount; }
    bool IsActive() const { return m_initialized && m_stream.is_open(); }

    // Helpers for building records
    static std::string NowHumanTimestamp() {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            now.time_since_epoch()) % 1000;

        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &time_t_now);
#else
        localtime_r(&time_t_now, &tm_buf);
#endif
        char buf[32];
        std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S", &tm_buf);

        char result[40];
        std::snprintf(result, sizeof(result), "%s.%03d", buf, static_cast<int>(ms.count()));
        return std::string(result);
    }

    static std::string TodaySessionDate() {
        auto now = std::chrono::system_clock::now();
        auto time_t_now = std::chrono::system_clock::to_time_t(now);

        std::tm tm_buf{};
#ifdef _WIN32
        localtime_s(&tm_buf, &time_t_now);
#else
        localtime_r(&time_t_now, &tm_buf);
#endif
        char buf[16];
        std::strftime(buf, sizeof(buf), "%Y-%m-%d", &tm_buf);
        return std::string(buf);
    }

    static uint64_t NowMicroseconds() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

private:
    RejectionLedger() = default;
    ~RejectionLedger() {
        if (m_stream.is_open()) {
            m_stream.flush();
            m_stream.close();
        }
    }
    RejectionLedger(const RejectionLedger&) = delete;
    RejectionLedger& operator=(const RejectionLedger&) = delete;

    static constexpr const char* kDefaultPath = "C:/Trading/logs/rejection_ledger.jsonl";

    std::ofstream m_stream;
    std::mutex m_mutex;
    std::string m_filePath;
    bool m_initialized = false;
    uint64_t m_recordCount = 0;
};
