#include "MindfulTrader_Precompiled.h"
#include "Scoring.h"
#include "TradeExecutionServer.h"
#include "SystemOrchestrator.h"

// RiskManager-specific persistent Int IDs
namespace {
    constexpr double kMinRiskDenominator = 1e-6;

    constexpr int RISK_CONSECUTIVE_LOSSES_ID = 104;       // Tracks consecutive losing trades
    constexpr int RISK_PEAK_EQUITY_ID = 105;              // Peak account equity for drawdown calculation
    constexpr int RISK_TRADING_HALTED_ID = 106;           // Trading halt flag (0=normal, 1=halted)
    constexpr int RISK_SESSION_START_BALANCE_ID = 107;    // Session starting balance for daily P&L calculation
    constexpr int RISK_NET_TICKS_TODAY_ID = 108;          // Net ticks gained/lost today (psychological metric)
    constexpr int RISK_LAST_RESET_DATE_ID = 109;          // Last date risk state was reset (prevents stale halt flags)
    constexpr int RISK_TRADES_EXECUTED_TODAY_ID = 110;    // Count of trades executed today (overtrading prevention)
    constexpr int RISK_LAST_LOSS_TIMESTAMP_ID = 111;      // Timestamp of last losing trade (cooling-off period)
    constexpr int RISK_LAST_POSITION_QTY_ID = 112;        // Track position qty to detect changes (Layer 4)
    constexpr int RISK_ORDER_ACTION_COUNT_LAST_MINUTE = 113;  // Count rapid-fire order actions
    constexpr int RISK_LAST_ORDER_ACTION_TIMESTAMP = 114; // Timestamp of last order action
    constexpr int RISK_ORIGINAL_STOP_PRICE_ID = 115;      // Original stop price (prevent manipulation)
    constexpr int RISK_STOP_VIOLATION_COUNT_ID = 116;     // Count of stop manipulation attempts
    constexpr int RISK_MONTHLY_START_EQUITY_ID = 117;     // Monthly starting equity (Elder's 6% rule)
    constexpr int RISK_MONTHLY_RESET_DATE_ID = 118;       // Last month reset occurred (YYYYMM format)
    constexpr int RISK_LAST_TCA_REPORT_DATE_ID = 119;     // Last session date for end-of-day TCA report

    struct HMMRiskPolicy {
        enum class LoadStatus {
            DEFAULTS,
            LOADED_FROM_FILE,
            FILE_MISSING,
            PARSE_FAILED
        };

        std::string policy_id = "default_hmm_risk_policy";
        std::string policy_version = "v1";
        std::string policy_hash = "builtin_defaults";
        LoadStatus load_status = LoadStatus::DEFAULTS;
        bool strict_mode = false;
        float strict_failure_cap = 0.20f;

        float state_weights[4] = {
            1.00f,  // COILED_SPRING
            1.20f,  // GAUSSIAN_STABLE
            0.90f,  // GAUSSIAN_FRAGILE
            0.75f   // PARETO_MOMENTUM
        };
        float entropy_penalty = 0.50f;
        float transition_penalty = 0.25f;
        float confidence_floor = 0.20f;
        float transition_floor = 0.70f;
        float transition_defensive_threshold = 0.30f;
        float transition_critical_threshold = 0.60f;
        float transition_defensive_distance_atr_multiplier = 1.50f;
        float transition_defensive_force_tighten_atr_profile = 2.00f;
        float max_multiplier = 1.50f;
        float min_multiplier = 0.00f;
        int connectivity_fail_closed_hold_ms = 5000;
        int order_ack_timeout_ms = 1200;
        int working_order_ttl_ms = 5000;
        bool inject_sc_disconnect = false;
        bool inject_ib_disconnect = false;
        bool inject_order_ack_timeout = false;

        // Empirical HMM gate thresholds (calibrated from lbrnet)
        double pareto_top_state_ratio_max = 0.25;
        double shannon_min_tenure_bars = 109.158494;
        double taleb_signal_sigma_threshold = 9.636797;
    };

    constexpr const char* kRiskPolicyPathWindows = "C:/Trading/config/hmm_regime_risk_policy.json";

    float ClampFloat(float value, float min_value, float max_value) {
        return std::max(min_value, std::min(value, max_value));
    }

    bool IsDailyLossLimitBreached(double dailyPnL, double accountEquity, double lossPct) {
        if (!std::isfinite(dailyPnL) || !std::isfinite(accountEquity)) {
            return false;
        }
        if (accountEquity <= kMinRiskDenominator || lossPct <= 0.0) {
            return false;
        }
        const double maxDailyLoss = accountEquity * lossPct;
        if (maxDailyLoss <= kMinRiskDenominator) {
            return false;
        }
        // Strict inequality avoids false halts when both values are effectively zero.
        return dailyPnL < -maxDailyLoss;
    }

    std::string HashPolicyPayload(const std::string& payload) {
        const size_t hash_value = std::hash<std::string>{}(payload);
        std::ostringstream oss;
        oss << std::hex << std::setw(sizeof(size_t) * 2) << std::setfill('0') << hash_value;
        return oss.str();
    }

    const HMMRiskPolicy& GetHMMRiskPolicy() {
        static HMMRiskPolicy policy;
        static bool loaded = false;

        if (loaded) {
            return policy;
        }
        loaded = true;

        std::ifstream in(kRiskPolicyPathWindows);
        if (!in.is_open()) {
            policy.load_status = HMMRiskPolicy::LoadStatus::FILE_MISSING;
            Logger::getInstance().log(
                std::string("RiskManager: Using default HMM risk policy (file not found at ") +
                kRiskPolicyPathWindows + ")"
            );
            Logger::getInstance().log(
                std::string("RiskManager: Policy metadata | policy_id=") + policy.policy_id +
                " | version=" + policy.policy_version +
                " | hash=" + policy.policy_hash +
                " | connectivity_fail_closed_hold_ms=" + std::to_string(policy.connectivity_fail_closed_hold_ms) +
                " | order_ack_timeout_ms=" + std::to_string(policy.order_ack_timeout_ms) +
                " | inject_sc_disconnect=" + (policy.inject_sc_disconnect ? "true" : "false") +
                " | inject_ib_disconnect=" + (policy.inject_ib_disconnect ? "true" : "false") +
                " | inject_order_ack_timeout=" + (policy.inject_order_ack_timeout ? "true" : "false")
            );
            return policy;
        }

        try {
            const std::string payload((std::istreambuf_iterator<char>(in)),
                                      std::istreambuf_iterator<char>());
            nlohmann::json j = nlohmann::json::parse(payload);

            policy.policy_id = j.value("policy_id", policy.policy_id);
            policy.policy_version = j.value("policy_version", policy.policy_version);
            policy.policy_hash = HashPolicyPayload(payload);
            policy.strict_mode = j.value("strict_mode", policy.strict_mode);
            policy.strict_failure_cap = j.value("strict_failure_cap", policy.strict_failure_cap);

            if (j.contains("state_weights") && j["state_weights"].is_object()) {
                const auto& w = j["state_weights"];
                policy.state_weights[0] = w.value("COILED_SPRING",    policy.state_weights[0]);
                policy.state_weights[1] = w.value("GAUSSIAN_STABLE",  policy.state_weights[1]);
                policy.state_weights[2] = w.value("GAUSSIAN_FRAGILE", policy.state_weights[2]);
                policy.state_weights[3] = w.value("PARETO_MOMENTUM",  policy.state_weights[3]);
            }

            policy.entropy_penalty = j.value("entropy_penalty", policy.entropy_penalty);
            policy.transition_penalty = j.value("transition_penalty", policy.transition_penalty);
            policy.confidence_floor = j.value("confidence_floor", policy.confidence_floor);
            policy.transition_floor = j.value("transition_floor", policy.transition_floor);
            policy.transition_defensive_threshold = j.value("transition_defensive_threshold", policy.transition_defensive_threshold);
            policy.transition_critical_threshold = j.value("transition_critical_threshold", policy.transition_critical_threshold);
            // Guardrail: critical must be meaningfully above defensive
            if (policy.transition_critical_threshold < policy.transition_defensive_threshold + 0.10f) {
                policy.transition_critical_threshold = policy.transition_defensive_threshold + 0.10f;
            }
            policy.transition_defensive_distance_atr_multiplier = j.value(
                "transition_defensive_distance_atr_multiplier",
                policy.transition_defensive_distance_atr_multiplier);
            policy.transition_defensive_force_tighten_atr_profile = j.value(
                "transition_defensive_force_tighten_atr_profile",
                policy.transition_defensive_force_tighten_atr_profile);
            policy.max_multiplier = j.value("max_multiplier", policy.max_multiplier);
            policy.min_multiplier = j.value("min_multiplier", policy.min_multiplier);
            policy.connectivity_fail_closed_hold_ms = j.value(
                "connectivity_fail_closed_hold_ms",
                policy.connectivity_fail_closed_hold_ms);
            policy.order_ack_timeout_ms = j.value(
                "order_ack_timeout_ms",
                policy.order_ack_timeout_ms);
            policy.working_order_ttl_ms = j.value(
                "working_order_ttl_ms",
                policy.working_order_ttl_ms);
            policy.inject_sc_disconnect = j.value(
                "inject_sc_disconnect",
                policy.inject_sc_disconnect);
            policy.inject_ib_disconnect = j.value(
                "inject_ib_disconnect",
                policy.inject_ib_disconnect);
            policy.inject_order_ack_timeout = j.value(
                "inject_order_ack_timeout",
                policy.inject_order_ack_timeout);

            // Parse empirical gate thresholds if present
            if (j.contains("empirical_gate_thresholds") && j["empirical_gate_thresholds"].is_object()) {
                const auto& gates = j["empirical_gate_thresholds"];
                policy.pareto_top_state_ratio_max = gates.value("pareto_top_state_ratio_max", policy.pareto_top_state_ratio_max);
                policy.shannon_min_tenure_bars = gates.value("shannon_min_tenure_bars", policy.shannon_min_tenure_bars);
                policy.taleb_signal_sigma_threshold = gates.value("taleb_signal_sigma_threshold", policy.taleb_signal_sigma_threshold);
            }

            policy.load_status = HMMRiskPolicy::LoadStatus::LOADED_FROM_FILE;

            Logger::getInstance().log(
                std::string("RiskManager: Loaded HMM risk policy from ") + kRiskPolicyPathWindows +
                " | policy_id=" + policy.policy_id +
                " | version=" + policy.policy_version +
                " | hash=" + policy.policy_hash +
                " | connectivity_fail_closed_hold_ms=" + std::to_string(policy.connectivity_fail_closed_hold_ms) +
                " | order_ack_timeout_ms=" + std::to_string(policy.order_ack_timeout_ms) +
                " | inject_sc_disconnect=" + (policy.inject_sc_disconnect ? "true" : "false") +
                " | inject_ib_disconnect=" + (policy.inject_ib_disconnect ? "true" : "false") +
                " | inject_order_ack_timeout=" + (policy.inject_order_ack_timeout ? "true" : "false") +
                " | strict_mode=" + (policy.strict_mode ? "true" : "false")
            );
            Logger::getInstance().log(
                std::string("RiskManager: HMM empirical gate thresholds") +
                " | pareto_ratio=" + std::to_string(policy.pareto_top_state_ratio_max) +
                " | shannon_tenure_bars=" + std::to_string(policy.shannon_min_tenure_bars) +
                " | taleb_sigma=" + std::to_string(policy.taleb_signal_sigma_threshold)
            );
        } catch (const std::exception& e) {
            policy.load_status = HMMRiskPolicy::LoadStatus::PARSE_FAILED;
            Logger::getInstance().log(
                std::string("RiskManager: Failed to parse HMM risk policy at ") +
                kRiskPolicyPathWindows + " (" + e.what() + ")"
            );
        }

        return policy;
    }
}

RiskManager& RiskManager::Instance() {
    static RiskManager instance;
    return instance;
}

float RiskManager::ComputeInstitutionalRiskMultiplier(int primaryState,
                                                      const float* probabilities,
                                                      size_t probabilityCount,
                                                      float entropy,
                                                      float transitionRisk,
                                                      float dof,
                                                      bool unknownPosture) {
    const HMMRiskPolicy& policy = GetHMMRiskPolicy();

    if (policy.strict_mode && policy.load_status != HMMRiskPolicy::LoadStatus::LOADED_FROM_FILE) {
        static bool strict_guard_logged = false;
        if (!strict_guard_logged) {
            Logger::getInstance().log(
                "RiskManager: STRICT MODE active with unavailable/invalid policy; applying strict_failure_cap"
            );
            strict_guard_logged = true;
        }
        return ClampFloat(policy.strict_failure_cap, policy.min_multiplier, policy.max_multiplier);
    }

    int clamped_state = std::max(0, std::min(primaryState, 3));
    float expected_multiplier = policy.state_weights[clamped_state];

    if (unknownPosture) {
        return ClampFloat(policy.strict_failure_cap, policy.min_multiplier, policy.max_multiplier);
    }

    if (!unknownPosture && probabilities != nullptr && probabilityCount > 0) {
        float weighted_sum = 0.0f;
        float probability_sum = 0.0f;

        const size_t n = std::min(probabilityCount, static_cast<size_t>(4));
        for (size_t i = 0; i < n; ++i) {
            const float p = probabilities[i];
            weighted_sum += p * policy.state_weights[i];
            probability_sum += p;
        }

        if (probability_sum > 1e-6f) {
            expected_multiplier = weighted_sum / probability_sum;
        }
    }

    const float safe_entropy = ClampFloat(entropy, 0.0f, 2.0f);
    const float safe_transition = ClampFloat(transitionRisk, 0.0f, 1.0f);

    const float confidence_scalar = ClampFloat(1.0f - (safe_entropy * policy.entropy_penalty),
                                               policy.confidence_floor,
                                               1.00f);
    const float transition_scalar = ClampFloat(1.0f - (safe_transition * policy.transition_penalty),
                                               policy.transition_floor,
                                               1.00f);

    // Model-implied kurtosis from Student-t DOF (Gap 6 closure)
    // κ = 3(ν-2)/(ν-4) for ν > 4; undefined/extreme for ν ≤ 4
    float model_kurtosis_factor = 1.0f;
    if (dof > 4.0f + 1e-6f) {
        const float model_kurtosis = 3.0f * (dof - 2.0f) / (dof - 4.0f);
        // Continuous sigmoid penalty centered at kurtosis=4.0:
        // At kurtosis=3 (normal): factor ≈ 1.0 (no penalty)
        // At kurtosis=6: factor ≈ 0.73
        // At kurtosis=10: factor ≈ 0.55
        // At kurtosis=20: factor ≈ 0.50 (floor)
        model_kurtosis_factor = 0.50f + 0.50f / (1.0f + std::exp(1.5f * (model_kurtosis - 4.0f)));
    } else if (dof > 1e-6f) {
        // DOF ≤ 4: extremely fat tails — apply minimum factor
        model_kurtosis_factor = 0.50f;
    } else {
        // P1.1 SAFE-CLOSED: dof ≈ 0 means no HMM data available.
        // Without regime intelligence, refuse to trade (safe-closed default).
        model_kurtosis_factor = 0.0f;
    }

    float multiplier = ClampFloat(expected_multiplier * confidence_scalar * transition_scalar * model_kurtosis_factor,
                                  policy.min_multiplier,
                                  policy.max_multiplier);

    return multiplier;
}

float RiskManager::GetTransitionRiskDefensiveThreshold() const {
    const HMMRiskPolicy& policy = GetHMMRiskPolicy();
    return ClampFloat(policy.transition_defensive_threshold, 0.0f, 1.0f);
}

float RiskManager::GetTransitionRiskCriticalThreshold() const {
    const HMMRiskPolicy& policy = GetHMMRiskPolicy();
    return ClampFloat(policy.transition_critical_threshold, 0.0f, 1.0f);
}

float RiskManager::GetLiveToxicScoreThreshold() const {
    return ClampFloat(m_execParams.liveToxicScoreThreshold, 0.0f, 1.0f);
}

float RiskManager::GetLiveHostileScoreThreshold() const {
    return ClampFloat(m_execParams.liveHostileScoreThreshold, 0.0f, 1.0f);
}

float RiskManager::GetLiveForceFlatConfidenceCap() const {
    return ClampFloat(m_execParams.liveForceFlatConfidenceCap, 0.0f, 1.0f);
}

int RiskManager::GetOrderAckTimeoutMs() const {
    const HMMRiskPolicy& policy = GetHMMRiskPolicy();
    return std::max(100, std::min(policy.order_ack_timeout_ms, 60000));
}

int RiskManager::GetWorkingOrderTtlMs() const {
    const HMMRiskPolicy& policy = GetHMMRiskPolicy();
    return std::max(500, std::min(policy.working_order_ttl_ms, 300000));
}

uint64_t RiskManager::GetConnectivityFailClosedHoldUs() const {
    const HMMRiskPolicy& policy = GetHMMRiskPolicy();
    const int holdMs = std::max(100, std::min(policy.connectivity_fail_closed_hold_ms, 300000));
    return static_cast<uint64_t>(holdMs) * 1000ULL;
}

bool RiskManager::ShouldInjectScDisconnect() const {
    return GetHMMRiskPolicy().inject_sc_disconnect;
}

bool RiskManager::ShouldInjectIbDisconnect() const {
    return GetHMMRiskPolicy().inject_ib_disconnect;
}

bool RiskManager::ShouldInjectOrderAckTimeout() const {
    return GetHMMRiskPolicy().inject_order_ack_timeout;
}

float RiskManager::GetTransitionDefensiveDistanceAtrMultiplier() const {
    const HMMRiskPolicy& policy = GetHMMRiskPolicy();
    return ClampFloat(policy.transition_defensive_distance_atr_multiplier, 0.10f, 10.0f);
}

float RiskManager::GetTransitionDefensiveForceTightenAtrProfile() const {
    const HMMRiskPolicy& policy = GetHMMRiskPolicy();
    return ClampFloat(policy.transition_defensive_force_tighten_atr_profile, 0.10f, 10.0f);
}

float RiskManager::GetParetoTopStateRatioMax() const {
    const HMMRiskPolicy& policy = GetHMMRiskPolicy();
    return ClampFloat(static_cast<float>(policy.pareto_top_state_ratio_max), 0.0f, 1.0f);
}

float RiskManager::GetShannonMinTenureBars() const {
    const HMMRiskPolicy& policy = GetHMMRiskPolicy();
    return std::max(0.0f, static_cast<float>(policy.shannon_min_tenure_bars));
}

float RiskManager::GetTalebSignalSigmaThreshold() const {
    const HMMRiskPolicy& policy = GetHMMRiskPolicy();
    return std::max(0.0f, static_cast<float>(policy.taleb_signal_sigma_threshold));
}

void RiskManager::Init(SCStudyInterfaceRef sc) {
    // Check if we need to reset daily state on chartbook open
    // This handles the case where Sierra Chart was closed with an active halt flag
    const int lastResetDate = sc.GetPersistentInt(RISK_LAST_RESET_DATE_ID);
    const int currentDate = sc.CurrentSystemDateTime.GetDate();

    if (lastResetDate != currentDate) {
        // New trading day - reset all daily state
        ResetDailyState(sc);
        sc.SetPersistentInt(RISK_LAST_RESET_DATE_ID, currentDate);

        // === ELITE GAP 7: VERIFY RESET AFTER CHARTBOOK OPEN ===
        if (IsTradingHalted()) {
            Logger::getInstance().log("⛔ CRITICAL: Daily reset failed - manually clear halt flag");
        }
    } else {
        // Same day - sync atomic from persistent storage (SC may have restarted mid-halt)
        const int haltFlag = sc.GetPersistentInt(RISK_TRADING_HALTED_ID);
        if (haltFlag != 0) [[unlikely]] {
            m_tradingHalted.store(true, std::memory_order_release);
            SystemOrchestrator::Instance().SetEmergencyHalt(true);
            Logger::getInstance().log("RiskManager: WARNING - Halt flag was active from previous session, atomic synced");
        }
    }

    // === LOAD P-AER EXECUTION PARAMETERS ===
        m_execParams.LoadFromFile();
        if (m_execParams.loadStatus == ExecutionParams::LoadStatus::FILE_MISSING ||
                m_execParams.loadStatus == ExecutionParams::LoadStatus::PARSE_FAILED) {
                const std::string msg =
                        m_execParams.loadStatus == ExecutionParams::LoadStatus::FILE_MISSING
                        ? std::string("FATAL: execution_params.json not found at ") + ExecutionParams::kDefaultPath +
                            " — this file is mandatory and must not be absent"
                        : std::string("FATAL: execution_params.json parse failed at ") + ExecutionParams::kDefaultPath +
                            " — fix the JSON before restarting";
                Logger::getInstance().log(msg);
                sc.AddMessageToLog(msg.c_str(), 1);
                return;
        }

    // === INIT REJECTION LEDGER ===
    RejectionLedger::Instance().Init();

    // === INIT OPPORTUNITY LEDGER (TDE Phase 1) ===
    OpportunityLedger::Instance().Init();

    // === CACHE INVARIANT VALUES (captured once, never queried again) ===

    // 1. Contract specifications (truly invariant)
    m_invariants.tickSize = sc.TickSize;
    m_invariants.currencyPerTick = sc.CurrencyValuePerTick;
    m_invariants.symbol = sc.GetChartSymbol(sc.ChartNumber);
    m_invariants.sessionStartDate = currentDate;

    // 2. Session start equity and calculated limits
    const double currentEquity = GetAccountEquity(sc);
    const double storedPeakEquity = sc.GetPersistentDouble(RISK_PEAK_EQUITY_ID);

    // If stored peak is zero or less than current, update it
    if (storedPeakEquity <= 0.0 || currentEquity > storedPeakEquity) [[unlikely]] {
        sc.SetPersistentDouble(RISK_PEAK_EQUITY_ID, currentEquity);
    }

    // Initialize session start balance for daily P&L tracking
    double sessionStartBalance = sc.GetPersistentDouble(RISK_SESSION_START_BALANCE_ID);
    if (sessionStartBalance <= 0.0) {
        sessionStartBalance = currentEquity;
        sc.SetPersistentDouble(RISK_SESSION_START_BALANCE_ID, currentEquity);
    }

    // Cache session start and computed limits (never changes until next day)
    m_invariants.sessionStartBalance = sessionStartBalance;
    m_invariants.maxDailyLoss = sessionStartBalance * m_execParams.elder2PctRuleFrac;
    m_invariants.maxPortfolioHeat = sessionStartBalance * m_execParams.elderPortfolioHeatFrac;
    m_invariants.maxDailyWin = sessionStartBalance * m_execParams.maxDailyWinPct;
    m_invariants.initialized = true;

    // 3. Initialize cached metrics with current state
    RefreshMetrics(sc);

    // 4. Load pattern stats from persistent storage
    LoadPatternStatsFromFile();

    // 5. Deterministic institutional sizing self-check (integration sanity marker)
    {
        const float test_probs[4] = {0.00f, 1.00f, 0.00f, 0.00f};
        const float test_entropy = 0.20f;
        const float test_transition_risk = 0.10f;
        const float baseline_multiplier = ComputeInstitutionalRiskMultiplier(
            1,  // GAUSSIAN_STABLE primary
            test_probs,
            4,
            test_entropy,
            test_transition_risk,
            0.0f,
            false);  // No HMM DOF during init self-check

        std::ostringstream oss;
        oss << std::fixed << std::setprecision(6)
            << "RiskManager: HMM sizing self-check"
            << " | case=GAUSSIAN_STABLE_certainty"
            << " | entropy=" << test_entropy
            << " | transition_risk=" << test_transition_risk
            << " | multiplier=" << baseline_multiplier;
        Logger::getInstance().log(oss.str());
    }

    // Initialization complete (logging reduced to avoid spam)
}

void RiskManager::OnPositionOpened(SCStudyInterfaceRef sc) {
    // Called by PositionManager when a new position is opened
    // Mark cache as having open position for future RefreshMetrics() calls
    m_cache.hasOpenPosition = true;
    RefreshMetrics(sc);
}

void RiskManager::RefreshMetrics(SCStudyInterfaceRef sc) {
    // Refresh cached P&L metrics (called once per bar if position open, or on events)
    // This is the ONLY method that queries broker APIs for P&L data

    if (!m_invariants.initialized) {
        Logger::getInstance().log("RiskManager: RefreshMetrics called before Init(), skipping");
        return;
    }

    // Query Sierra Chart position data (only once per bar or on events)
    s_SCPositionData positionData;
    const int result = sc.GetTradePosition(positionData);

    if (result == SCTRADING_ORDER_ERROR) {
        // No position data available - calculate from balance delta
        const double currentBalance = GetAccountEquity(sc);
        m_cache.dailyPnL = currentBalance - m_invariants.sessionStartBalance;
        m_cache.unrealizedPnL = 0.0;
        m_cache.realizedPnL = m_cache.dailyPnL;
        m_cache.hasOpenPosition = false;
    } else {
        // Position data available - use broker's calculations
        m_cache.dailyPnL = positionData.DailyProfitLoss;
        m_cache.unrealizedPnL = positionData.OpenProfitLoss;
        m_cache.realizedPnL = m_cache.dailyPnL - m_cache.unrealizedPnL;
        m_cache.hasOpenPosition = (positionData.PositionQuantity != 0);
    }

    // Update auxiliary metrics
    m_cache.netTicks = sc.GetPersistentDouble(RISK_NET_TICKS_TODAY_ID);

    // Quality score from current trade
    const Trade& currentTrade = PositionManager::Instance().GetTrade();
    const int tradeGrade = currentTrade.GetTradeGrade();
    m_cache.qualityScore = (tradeGrade > 0) ? static_cast<double>(tradeGrade) : 70.0;

    m_cache.lastUpdateBar = sc.Index;
    m_cache.isValid = true;
}

void RiskManager::Update(SCStudyInterfaceRef sc) {
    // Continuous monitoring called every bar

    // Keep kurtosis emergency state current even when no trades are attempted.
    RefreshKurtosisEmergencyState(sc);

    // Check 0: Monthly loss limit (ELDER'S ULTIMATE CAREER INSURANCE - checked first)
    if (!CheckMonthlyLossLimit(sc)) {
        return; // Trading halted for remainder of month
    }

    // Check 1: Daily loss limit (CRITICAL - highest priority)
    if (!CheckDailyLossLimit(sc)) {
        return; // Trading already halted
    }

    // Check 2: Drawdown limit
    if (!CheckDrawdownLimit(sc)) {
        return; // Trading halted due to excessive drawdown
    }

    // LAYER 4: Reactive order monitoring (external order protection)
    MonitorOrderViolations(sc);
    MonitorStopOrderModifications(sc);

    // C14 fix: Golden Rule — continuously enforce end-of-day flatten (not just on new predictions)
    EnforceGoldenRule(sc);

    // End-of-day TCA summary (once per session day).
    const SCDateTime now = sc.CurrentSystemDateTime;
    const int currentMinutes = now.GetHour() * 60 + now.GetMinute();
    const int currentDate = now.GetDate();
    const int lastTcaReportDate = sc.GetPersistentInt(RISK_LAST_TCA_REPORT_DATE_ID);
    static constexpr int TCA_REPORT_TRIGGER_MINUTES = 15 * 60 + 59;  // 15:59 ET
    if (currentMinutes >= TCA_REPORT_TRIGGER_MINUTES && currentDate != lastTcaReportDate) {
        AIConnectionMonitor::Instance().GenerateTCAReport(sc);
        sc.SetPersistentInt(RISK_LAST_TCA_REPORT_DATE_ID, currentDate);
    }

    // Update peak equity if current equity is higher
    const double currentEquity = GetAccountEquity(sc);
    const double peakEquity = sc.GetPersistentDouble(RISK_PEAK_EQUITY_ID);
    if (currentEquity > peakEquity) [[unlikely]] {
        sc.SetPersistentDouble(RISK_PEAK_EQUITY_ID, currentEquity);
    }
}

// ===========================================================================================
// HELPER METHODS (DRY Refactoring - Single Source of Truth)
// ===========================================================================================

double RiskManager::EnsureMonthlyEquityTracking(SCStudyInterfaceRef sc) {
    // Ensures monthly equity tracking is initialized and current
    // Returns the monthly start equity (resets if new month detected)
    const double currentEquity = GetAccountEquity(sc);
    const SCDateTime now = sc.CurrentSystemDateTime;
    const int currentYearMonth = (now.GetYear() * 100) + now.GetMonth();  // YYYYMM format

    double monthlyStartEquity = sc.GetPersistentDouble(RISK_MONTHLY_START_EQUITY_ID);
    const int lastMonthlyReset = sc.GetPersistentInt(RISK_MONTHLY_RESET_DATE_ID);

    // Check if new month - reset tracking
    if (lastMonthlyReset != currentYearMonth) {
        monthlyStartEquity = currentEquity;
        sc.SetPersistentDouble(RISK_MONTHLY_START_EQUITY_ID, currentEquity);
        sc.SetPersistentInt(RISK_MONTHLY_RESET_DATE_ID, currentYearMonth);
        Logger::getInstance().log("Monthly equity reset: $" + std::to_string(currentEquity) +
            " (Month: " + std::to_string(currentYearMonth) + ")");
        return monthlyStartEquity;
    }

    // Initialize if first time
    if (monthlyStartEquity <= 0.0) [[unlikely]] {
        monthlyStartEquity = currentEquity;
        sc.SetPersistentDouble(RISK_MONTHLY_START_EQUITY_ID, currentEquity);
        sc.SetPersistentInt(RISK_MONTHLY_RESET_DATE_ID, currentYearMonth);
    }

    return monthlyStartEquity;
}

// ===========================================================================================
// VALIDATOR CHAIN IMPLEMENTATION (Elite Refactor - Chain of Responsibility Pattern)
// Each validator is a self-contained check that can be tested independently
// ===========================================================================================

Result<void> RiskManager::ValidateMonthlyLimit(SCStudyInterfaceRef sc) {
    // ELDER'S 6% MONTHLY RULE - Career Insurance (highest priority protection)
    const double currentEquity = GetAccountEquity(sc);
    const double monthlyStartEquity = EnsureMonthlyEquityTracking(sc);  // DRY: Single source of truth

    // Check if monthly drawdown limit exceeded
    const double maxMonthlyLoss = monthlyStartEquity * m_execParams.elder6PctRuleFrac;
    if (currentEquity < (monthlyStartEquity - maxMonthlyLoss)) [[unlikely]] {
        const double monthlyLoss = monthlyStartEquity - currentEquity;
        const double monthlyLossPct = (monthlyLoss / monthlyStartEquity) * 100.0;

        SCString msg;
        msg.Format("⛔ ELDER 6%% RULE: Monthly drawdown limit exceeded! Loss: -$%.2f (-%.2f%%). See you next month.",
                   monthlyLoss, monthlyLossPct);
        Logger::getInstance().log(msg.GetChars());

        EmergencyHalt(sc, "ELDER 6% RULE: Monthly drawdown limit reached. Trading suspended until next month.");
        return Result<void>::Failure("Monthly 6% loss limit exceeded", 1001);
    }

    return Result<void>::Success();
}

Result<void> RiskManager::ValidateTradingHalt(SCStudyInterfaceRef sc) {
    // Belt-and-suspenders: check both atomic flag and persistent storage
    if (IsTradingHalted() || IsTradingHalted(sc)) {
        const int consecutiveLosses = sc.GetPersistentInt(RISK_CONSECUTIVE_LOSSES_ID);
        const double dailyPnL = GetDailyPnL();
        const double accountEquity = GetAccountEquity(sc);
        const double dailyPnLPct = (accountEquity > 0.0) ? (dailyPnL / accountEquity) * 100.0 : 0.0;
        SCString msg;
        msg.Format("Trading is halted | consecutive_losses=%d | daily_pnl_pct=%.2f%% | equity=$%.2f",
                   consecutiveLosses, dailyPnLPct, accountEquity);
        return Result<void>::Failure(msg.GetChars(), 1002);
    }
    return Result<void>::Success();
}

Result<void> RiskManager::ValidateTradingHours(SCStudyInterfaceRef sc) {
    // Time-of-day trading windows (avoid overnight gap chaos and late-day fatigue)
    const SCDateTime now = sc.CurrentSystemDateTime;
    const int hour = now.GetHour();
    const int minute = now.GetMinute();
    const int currentMinutes = hour * 60 + minute;

    s_SCPositionData position;
    sc.GetTradePosition(position);
    const bool isNewEntry = (position.PositionQuantity == 0);
    const bool hasPosition = (position.PositionQuantity != 0);

    // Check if before market open (9:30 AM)
    if (currentMinutes < TRADING_START_MINUTES && isNewEntry) [[unlikely]] {
        SCString msg;
        msg.Format("Before market open (9:30 AM) | current_time=%02d:%02d", hour, minute);
        return Result<void>::Failure(msg.GetChars(), 1003);
    }

    // Final Hour (3:45-4:00 PM) - Golden Rule: Must flatten losing positions
    static constexpr int GOLDEN_RULE_CUTOFF = 15 * 60 + 45;  // 3:45 PM (945 minutes)
    if (currentMinutes >= GOLDEN_RULE_CUTOFF && hasPosition) {
        // Check if position is losing
        const double unrealizedPnL = GetUnrealizedPnL();
        if (unrealizedPnL < 0) {
            // Force flatten - don't carry loser overnight
            PositionManager::Instance().EmergencyFlattenPosition(sc,
                "Golden Rule: Flatten losing position before 4:00 PM");
            Logger::getInstance().log("⚠️ GOLDEN RULE TRIGGERED: Flattening losing position before close");
        }
    }

    // No new entries after 3:00 PM (position squaring noise)
    static constexpr int NEAR_CLOSE_CUTOFF = 15 * 60;  // 3:00 PM (900 minutes)
    if (currentMinutes >= NEAR_CLOSE_CUTOFF && isNewEntry) [[unlikely]] {
        SCString msg;
        msg.Format("No new entries after 3:00 PM (near close) | current_time=%02d:%02d", hour, minute);
        return Result<void>::Failure(msg.GetChars(), 1004);
    }

    // Check if after market close (3:30 PM) - now superseded by 3:00 PM
    if (currentMinutes >= END_OF_DAY_CUTOFF_MINUTES && isNewEntry) [[unlikely]] {
        SCString msg;
        msg.Format("After market close (3:30 PM) | current_time=%02d:%02d", hour, minute);
        return Result<void>::Failure(msg.GetChars(), 1004);
    }

    return Result<void>::Success();
}

Result<void> RiskManager::ValidateCoolingPeriod(SCStudyInterfaceRef sc) {
    // Cooling-off period after loss (prevents revenge trading)
    const int64_t lastLossTime = sc.GetPersistentInt64(RISK_LAST_LOSS_TIMESTAMP_ID);
    if (lastLossTime > 0) [[unlikely]] {
        SCDateTime lastLoss;
        lastLoss = lastLossTime;

        const SCDateTime now = sc.CurrentSystemDateTime;
        const int minutesSinceLoss = (now - lastLoss).GetTimeInSeconds() / 60;

        if (minutesSinceLoss < m_execParams.elderRevengeCooloffMin) [[unlikely]] {
            const int minutesRemaining = m_execParams.elderRevengeCooloffMin - minutesSinceLoss;
            SCString msg;
            msg.Format("Cooling-off period active | elapsed=%d min | remaining=%d min | cooloff=%d min",
                       minutesSinceLoss, minutesRemaining, m_execParams.elderRevengeCooloffMin);
            return Result<void>::Failure(msg.GetChars(), 1005);
        }
    }
    return Result<void>::Success();
}

Result<void> RiskManager::ValidateRapidFire(SCStudyInterfaceRef sc) {
    // Rapid-fire order placement (panic trading detection)
    SCDateTime currentTime = sc.CurrentSystemDateTime;
    int64_t lastActionTime = sc.GetPersistentInt64(RISK_LAST_ORDER_ACTION_TIMESTAMP);

    if (lastActionTime > 0) {
        SCDateTime lastAction;
        lastAction = lastActionTime;

        int secondsSinceLastAction = (currentTime - lastAction).GetTimeInSeconds();

        if (secondsSinceLastAction < 60) {
            // Within 60-second window
            int actionCount = sc.GetPersistentInt(RISK_ORDER_ACTION_COUNT_LAST_MINUTE);
            actionCount++;
            sc.SetPersistentInt(RISK_ORDER_ACTION_COUNT_LAST_MINUTE, actionCount);

            if (actionCount >= 3) {
                // 3rd action within 60 seconds = panic trading
                SCString msg;
                msg.Format("Rapid-fire order placement detected - panic trading | actions=%d in %ds (limit=3 per 60s)",
                           actionCount, secondsSinceLastAction);
                EmergencyHalt(sc, msg.GetChars());
                return Result<void>::Failure(msg.GetChars(), 1006);
            }
        } else {
            // More than 60 seconds - reset counter
            sc.SetPersistentInt(RISK_ORDER_ACTION_COUNT_LAST_MINUTE, 1);
        }
    } else {
        // First action
        sc.SetPersistentInt(RISK_ORDER_ACTION_COUNT_LAST_MINUTE, 1);
    }

    // Record this action timestamp
    sc.SetPersistentInt64(RISK_LAST_ORDER_ACTION_TIMESTAMP, static_cast<int64_t>(currentTime.GetAsDouble()));

    return Result<void>::Success();
}

// ============================================================================
// ELITE v3.2: UNIFIED HARD GATE LAYER
// Physics-based hard gates from LocalRiskContext — single evaluation point
// for all three paths (bar update, HMM, Transformer).
//
// Gate ordering: cheapest checks first, fatal checks before partial penalties.
// Returns: Success if all gates pass, Failure with reason string if any trips.
// ============================================================================
Result<void> RiskManager::EvaluateHardGates(const LocalRiskContext& ctx) const {
    if (!ctx.isValid) {
        return Result<void>::Failure(
            "HARD_GATE: context not valid"
            " | isValid=0"
            " | snapshot_us=" + std::to_string(ctx.snapshotTimestampUs) +
            " | shannon_entropy=" + std::to_string(ctx.shannonFlowEntropy) +
            " | amihud=" + std::to_string(ctx.amihudIlliquidity) +
            " | taleb_cliff=" + std::to_string(ctx.elderChandelierATR) +
            " | taleb_kurtosis=" + std::to_string(ctx.talebKurtosis) +
            " | liq_fragility=" + std::to_string(ctx.spreadStress)
        );
    }
    // === GAP 26: REGIME-AWARE AMIHUD HARD GATE (Taleb — fat-tail tightening) ===
    // Layer B: gate on a SESSION-AWARE ROLLING PERCENTILE of raw Amihud illiquidity,
    // not the raw value against a fixed constant (the raw value is non-stationary in
    // price/volume regime, so a fixed 0.80/0.40 was meaningless across regimes).
    // In crash regimes (DOF ≤ 4 / kurtosis > 8) tighten the veto from p90 → p75.
    {
        const auto* hmmGate = InferenceManager::Instance().HmmState();
        const float gateDof = hmmGate ? hmmGate->Dof() : 30.0f;
        const bool fatTail = (gateDof <= 4.0f) || (ctx.talebKurtosis > 8.0f);
        const float amihudPctThreshold = fatTail ? 0.75f : 0.90f;
        if (ctx.amihudPercentile > amihudPctThreshold) {
            return Result<void>::Failure(
                "HARD_GATE: Amihud illiquidity toxicity critical"
                " | amihud_pct=" + std::to_string(ctx.amihudPercentile) +
                " threshold=" + std::to_string(amihudPctThreshold) +
                " | amihud_raw=" + std::to_string(ctx.amihudIlliquidity) +
                " | dof=" + std::to_string(gateDof) +
                " | fat_tail=" + std::string(fatTail ? "true" : "false"));
        }
    }
    if (ctx.elderChandelierATR < 0.50f) {
        return Result<void>::Failure(
            "HARD_GATE: Taleb cliff proximity critical"
            " | taleb_cliff=" + std::to_string(ctx.elderChandelierATR) +
            " threshold=0.50");
    }
    if (ctx.shannonFlowEntropy > m_execParams.shannonEntropyHaltFrac * kShannonMaxEntropyBits) {
        return Result<void>::Failure(
            "HARD_GATE: Shannon entropy critical"
            " | shannon_entropy=" + std::to_string(ctx.shannonFlowEntropy) +
            " threshold=" + std::to_string(m_execParams.shannonEntropyHaltFrac * kShannonMaxEntropyBits));
    }
    if (ctx.talebKurtosis > m_execParams.talebKurtosisHaltThreshold) {
        return Result<void>::Failure(
            "HARD_GATE: Taleb kurtosis critical"
            " | taleb_kurtosis=" + std::to_string(ctx.talebKurtosis) +
            " threshold=" + std::to_string(m_execParams.talebKurtosisHaltThreshold));
    }
    if (ctx.spreadStress > 0.85f) {
        return Result<void>::Failure(
            "HARD_GATE: liquidity fragility critical"
            " | liq_fragility=" + std::to_string(ctx.spreadStress) +
            " threshold=0.85");
    }
    return Result<void>::Success();
}

void RiskManager::RefreshKurtosisEmergencyState([[maybe_unused]] SCStudyInterfaceRef sc) {
    const auto& localCtx = ContextManager::Instance().GetLocalRiskContext();
    if (!localCtx.isValid) {
        return;
    }

    const float kurtosis = localCtx.talebKurtosis;
    if (!std::isfinite(kurtosis)) {
        return;
    }

    m_lastObservedKurtosis.store(kurtosis, std::memory_order_relaxed);

    const bool wasActive = m_kurtosisEmergencyActive.load(std::memory_order_relaxed);
    bool nowActive = wasActive;

    if (!wasActive && kurtosis > m_execParams.talebKurtosisCrisisEnter) {
        nowActive = true;
    } else if (wasActive && kurtosis < m_execParams.talebKurtosisCrisisExit) {
        nowActive = false;
    }

    if (nowActive == wasActive) {
        return;
    }

    m_kurtosisEmergencyActive.store(nowActive, std::memory_order_relaxed);

    SCString msg;
    msg.Format(
        "🚨 KURTOSIS EMERGENCY %s: realized_kurtosis=%.2f (enter>%.1f, exit<%.1f) | posture=%s",
        nowActive ? "ENGAGED" : "CLEARED",
        kurtosis,
        m_execParams.talebKurtosisCrisisEnter,
        m_execParams.talebKurtosisCrisisExit,
        nowActive ? "TREND_ONLY" : "NORMAL"
    );
    Logger::getInstance().log(msg.GetChars());
}

bool RiskManager::IsTrendContinuationTrigger(RaschkeTacticalTrigger trigger) const {
    switch (trigger) {
        case RaschkeTacticalTrigger::ELDER_BREAKOUT_BUY:
        case RaschkeTacticalTrigger::ELDER_BREAKOUT_SELL:
        case RaschkeTacticalTrigger::NR7_BREAKOUT_BUY:
        case RaschkeTacticalTrigger::NR7_BREAKOUT_SELL:
        case RaschkeTacticalTrigger::ITR_BREAKOUT_BUY:
        case RaschkeTacticalTrigger::ITR_BREAKOUT_SELL:
            return true;
        default:
            return false;
    }
}

Result<void> RiskManager::ValidateKurtosisEmergencyGate(SCStudyInterfaceRef sc) {
    RefreshKurtosisEmergencyState(sc);

    if (!m_kurtosisEmergencyActive.load(std::memory_order_relaxed)) {
        return Result<void>::Success();
    }

    // DOD/SoA migration (Task 7 fix round): read straight from the packed
    // array — no pointer, no null check, always a valid value.
    const RaschkeTacticalTrigger trigger = static_cast<RaschkeTacticalTrigger>(
        IndicatorManager::Instance().GetValue<IndicatorKey::RASCHKE_TACTICAL_TRIGGER>());
    if (!IsTrendContinuationTrigger(trigger)) {
        SCString msg;
        msg.Format(
            "Kurtosis emergency gate active: trigger %s blocked (realized_kurtosis=%.2f, TREND_ONLY)",
            getRaschkeTacticalTriggerName(trigger),
            m_lastObservedKurtosis.load(std::memory_order_relaxed)
        );
        Logger::getInstance().log(msg.GetChars());
        return Result<void>::Failure(msg.GetChars(), 1026);
    }

    return Result<void>::Success();
}

Result<void> RiskManager::ValidatePatternQuality(SCStudyInterfaceRef sc) {
    // Pattern quality minimum (filter weak setups) - time-of-day aware
    const Trade& currentTrade = PositionManager::Instance().GetTrade();
    const std::string patternEnum = currentTrade.GetPatternEnum();

    // Use AI confidence score from Trade object as pattern quality
    float patternQuality = currentTrade.GetConfidence();

    // If confidence not set, assume neutral quality
    if (patternQuality <= 0.0f) {
        patternQuality = 0.7f;  // Default: above minimum threshold
    }

    // Get base quality threshold (probation-aware)
    float minQualityThreshold = GetMinQualityForPattern(patternEnum);

    // Time-of-day adjustments (Linda Raschke's session quality rules)
    const SCDateTime now = sc.CurrentSystemDateTime;
    const int currentMinutes = now.GetHour() * 60 + now.GetMinute();

    // Opening Hour (9:30-10:30 AM): Choppy price action, need confirmation
    static constexpr int OPENING_HOUR_END = 10 * 60 + 30;  // 10:30 AM (630 minutes)
    if (currentMinutes >= TRADING_START_MINUTES && currentMinutes < OPENING_HOUR_END) {
        minQualityThreshold = std::max(minQualityThreshold, m_execParams.raschkeOpeningQualityFloor);
        Logger::getInstance().log("⏰ Opening Hour: Quality threshold raised to 0.70");
    }

    // Lunch Dead Zone (12:00-2:00 PM): Low volume, false breakouts common
    static constexpr int LUNCH_START = 12 * 60;      // 12:00 PM (720 minutes)
    static constexpr int LUNCH_END = 14 * 60;        // 2:00 PM (840 minutes)
    if (currentMinutes >= LUNCH_START && currentMinutes < LUNCH_END) {
        minQualityThreshold = std::max(minQualityThreshold, m_execParams.raschkeLunchDeadzoneFloor);
        Logger::getInstance().log("🍽️ Lunch Dead Zone: Quality threshold raised to 0.80 (STRICT)");
    }

    // Afternoon Session (2:00-3:00 PM): Second-chance setups, but time running out
    static constexpr int AFTERNOON_START = 14 * 60;  // 2:00 PM (840 minutes)
    static constexpr int AFTERNOON_END = 15 * 60;    // 3:00 PM (900 minutes)
    if (currentMinutes >= AFTERNOON_START && currentMinutes < AFTERNOON_END) {
        minQualityThreshold = std::max(minQualityThreshold, m_execParams.raschkeAfternoonQualityFloor);
        Logger::getInstance().log("🌆 Afternoon: Quality threshold raised to 0.65");
    }

    // Sweet Spot (10:30 AM-12:00 PM): Best time to trade, use base thresholds
    // (No adjustment needed - base threshold already optimal)

    if (patternQuality < minQualityThreshold) [[unlikely]] {
        SCString msg;
        msg.Format("Pattern quality %.2f below time-adjusted threshold %.2f",
                   patternQuality, minQualityThreshold);
        return Result<void>::Failure(msg.GetChars(), 1007);
    }

    return Result<void>::Success();
}

Result<void> RiskManager::ValidateCrossMarketCorrelation(SCStudyInterfaceRef sc) {
    // Cross-market divergence detection - reject trades when ZN/DX show warning signals
    // Uses rolling 20-bar correlation from hidden 60-min charts (CrossMarketStudies)
    (void)sc;  // Unused - accesses IndicatorManager singleton, not sc data

    // Get current trade direction
    const Trade& currentTrade = PositionManager::Instance().GetTrade();
    const TradeSideEnum tradeSide = currentTrade.GetSide();

    if (tradeSide == TradeSideEnum::FLAT) {
        return Result<void>::Success();  // No position to validate
    }

    // Fetch correlation indicators from IndicatorManager
    auto* znTrend = IndicatorManager::Instance().GetIndicator<CrossMarketTrend>(
        IndicatorKey::ZN_TREND);
    auto* dxTrend = IndicatorManager::Instance().GetIndicator<CrossMarketTrend>(
        IndicatorKey::DX_TREND);

    // If correlation data not available yet (hidden charts not running), allow trade
    if (!znTrend || !dxTrend) {
        return Result<void>::Success();
    }

    // DOD/SoA migration (Task 8): read straight from the packed array — no
    // pointer, no null check, always a valid value. CORR_ES_ZN/CORR_ES_DX are
    // single-row (Float32-only), so the single-key GetValue<Key>() form
    // resolves unambiguously.
    const float esZnCorr = IndicatorManager::Instance().GetValue<IndicatorKey::CORR_ES_ZN>();
    const float esDxCorr = IndicatorManager::Instance().GetValue<IndicatorKey::CORR_ES_DX>();
    const CrossMarketTrendEnum znTrendDir = znTrend->Value();
    const CrossMarketTrendEnum dxTrendDir = dxTrend->Value();

    // === RULE 1: Strong Positive ES-ZN Correlation (Unusual Risk-Off Warning) ===
    // Normal: ES-ZN negatively correlated (bonds up when stocks down)
    // Warning: ES-ZN strongly positive (> 0.7) = both moving together = instability
    if (esZnCorr > 0.7f) {
        SCString msg;
        msg.Format("Cross-market warning: ES-ZN correlation %.2f (risk-off signal)", esZnCorr);
        Logger::getInstance().log(msg.GetChars());
        return Result<void>::Failure(msg.GetChars(), 1020);
    }

    // === RULE 2: Bearish Divergence (ZN Up + ES Long) ===
    // If going LONG ES but ZN trending up strongly = bonds seeking safety = bearish for stocks
    if (tradeSide == TradeSideEnum::LONG &&
        znTrendDir == CrossMarketTrendEnum::UP &&
        esZnCorr < -0.5f) {
        SCString msg;
        msg.Format("Cross-market warning: ZN uptrend (%.2f corr) conflicts with ES long", esZnCorr);
        Logger::getInstance().log(msg.GetChars());
        return Result<void>::Failure(msg.GetChars(), 1021);
    }

    // === RULE 3: Bullish Divergence (ZN Down + ES Short) ===
    // If going SHORT ES but ZN trending down = bonds selling off = risk-on for stocks
    if (tradeSide == TradeSideEnum::SHORT &&
        znTrendDir == CrossMarketTrendEnum::DOWN &&
        esZnCorr < -0.5f) {
        SCString msg;
        msg.Format("Cross-market warning: ZN downtrend (%.2f corr) conflicts with ES short", esZnCorr);
        Logger::getInstance().log(msg.GetChars());
        return Result<void>::Failure(msg.GetChars(), 1022);
    }

    // === RULE 4: Dollar Strength Conflicts ===
    // Strong dollar (DX up) = typically bearish for ES
    if (tradeSide == TradeSideEnum::LONG &&
        dxTrendDir == CrossMarketTrendEnum::UP &&
        esDxCorr < -0.5f) {
        SCString msg;
        msg.Format("Cross-market warning: DX uptrend (%.2f corr) bearish for ES long", esDxCorr);
        Logger::getInstance().log(msg.GetChars());
        return Result<void>::Failure(msg.GetChars(), 1023);
    }

    // Weak dollar (DX down) = typically bullish for ES
    if (tradeSide == TradeSideEnum::SHORT &&
        dxTrendDir == CrossMarketTrendEnum::DOWN &&
        esDxCorr < -0.5f) {
        SCString msg;
        msg.Format("Cross-market warning: DX downtrend (%.2f corr) bullish for ES short", esDxCorr);
        Logger::getInstance().log(msg.GetChars());
        return Result<void>::Failure(msg.GetChars(), 1024);
    }

    // All cross-market checks passed
    return Result<void>::Success();
}

Result<void> RiskManager::ValidateRiskLimits(SCStudyInterfaceRef sc, double entryPrice, double stopPrice, int quantity) {
    // Validate all risk-based limits: daily loss, daily win, max trades, and trade risk

    // Check 1: Max trades per day
    const int tradesExecutedToday = sc.GetPersistentInt(RISK_TRADES_EXECUTED_TODAY_ID);
    if (tradesExecutedToday >= m_execParams.maxTradesPerDay) [[unlikely]] {
        SCString msg;
        msg.Format("Daily trade limit reached | trades_today=%d | limit=%d",
                   tradesExecutedToday, m_execParams.maxTradesPerDay);
        EmergencyHalt(sc, msg.GetChars());
        return Result<void>::Failure(msg.GetChars(), 1008);
    }

    // Check 2: Daily loss limit
    const double dailyPnL = GetDailyPnL();
    const double accountEquity = GetAccountEquity(sc);

    if (IsDailyLossLimitBreached(dailyPnL, accountEquity, m_execParams.elder2PctRuleFrac)) [[unlikely]] {
        const double maxDailyLoss = accountEquity * m_execParams.elder2PctRuleFrac;
        SCString msg;
        msg.Format("Daily loss limit exceeded | daily_pnl=$%.2f | max_loss=$%.2f (%.1f%%) | equity=$%.2f",
                   dailyPnL, -maxDailyLoss, m_execParams.elder2PctRuleFrac * 100.0, accountEquity);
        EmergencyHalt(sc, msg.GetChars());
        return Result<void>::Failure(msg.GetChars(), 1009);
    }

    if (dailyPnL >= accountEquity * m_execParams.maxDailyWinPct) {
        const double winGoal = accountEquity * m_execParams.maxDailyWinPct;
        SCString msg;
        msg.Format("Daily win goal reached | daily_pnl=$%.2f | win_goal=$%.2f (+%.1f%%) | equity=$%.2f",
                   dailyPnL, winGoal, m_execParams.maxDailyWinPct * 100.0, accountEquity);
        EmergencyHalt(sc, msg.GetChars());
        return Result<void>::Failure(msg.GetChars(), 1010);
    }

    // Check 4: 2% rule per trade
    const double orderRisk = CalculateOrderRisk(sc, entryPrice, stopPrice, quantity);
    const double maxRiskPerTrade = accountEquity * m_execParams.maxRiskPerTradeFrac;

    if (orderRisk > maxRiskPerTrade) [[unlikely]] {
        SCString msg;
        msg.Format("Risk per trade exceeds 2%% limit | order_risk=$%.2f | max_risk=$%.2f (%.1f%%) | entry=%.2f | stop=%.2f | qty=%d",
                   orderRisk, maxRiskPerTrade, m_execParams.maxRiskPerTradeFrac * 100.0, entryPrice, stopPrice, quantity);
        return Result<void>::Failure(msg.GetChars(), 1011);
    }

    // Check 5: 6% portfolio heat rule
    if (!Check6PercentRule(sc, orderRisk)) {
        const double totalExposure = CalculateTotalExposure(sc);
        const double maxExposure = accountEquity * m_execParams.elderPortfolioHeatFrac;
        SCString heatMsg;
        heatMsg.Format("Portfolio heat exceeds 6%% limit | exposure=$%.2f | new_risk=$%.2f | max=$%.2f",
                       totalExposure, orderRisk, maxExposure);
        return Result<void>::Failure(heatMsg.GetChars(), 1012);
    }

    return Result<void>::Success();
}

LatencyGateResult RiskManager::EvaluateLatencyGate(
    SCStudyInterfaceRef sc,
    long inferenceLatencyUs,
    long transformerLatencyUs,
    long regimeLatencyUs
) const {
    (void)sc;
    (void)inferenceLatencyUs;
    (void)transformerLatencyUs;

    LatencyGateResult result;

    if (regimeLatencyUs < 0) {
        result.reason = "Latency telemetry unavailable";
        return result;
    }

    if (regimeLatencyUs <= m_execParams.regimeLatencyOptimalUs) {
        result.reason = "Latency optimal";
        return result;
    }

    if (regimeLatencyUs > m_execParams.regimeLatencyBreakerUs) {
        result.hardReject = true;
        result.degraded = true;
        result.riskFactor = 0.0f;
        result.reason = "Regime latency breaker tripped";
        return result;
    }

    const float normalized = static_cast<float>(regimeLatencyUs - m_execParams.regimeLatencyOptimalUs) /
                             static_cast<float>(m_execParams.regimeLatencyBreakerUs - m_execParams.regimeLatencyOptimalUs);
    const float clamped = std::max(0.0f, std::min(1.0f, normalized));
    result.degraded = true;
    result.riskFactor = 1.0f - (clamped * (1.0f - m_execParams.regimeLatencyMinFactor));
    result.reason = "Regime latency degraded";
    return result;
}

Result<void> RiskManager::ValidateOrder(
    SCStudyInterfaceRef sc,
    double entryPrice,
    double stopPrice,
    int& quantity,
    long inferenceLatencyUs,
    long transformerLatencyUs,
    long regimeLatencyUs
) {
    // Pre-trade validation gate - Elite Refactor #7: Result<T> error handling
    // Each validator returns Result<void> with detailed error information

    // === P-AER: BUILD BASE REJECTION RECORD ===
    RejectionRecord baseRec;
    baseRec.timestampUs     = RejectionLedger::NowMicroseconds();
    baseRec.timestampHuman  = RejectionLedger::NowHumanTimestamp();
    baseRec.paramSetId      = m_execParams.paramSetId;
    baseRec.sessionDate     = RejectionLedger::TodaySessionDate();
    baseRec.entryPrice      = entryPrice;
    baseRec.stopPrice       = stopPrice;
    baseRec.proposedQuantity = quantity;

    // Trade proposal details
    const Trade& currentTrade = PositionManager::Instance().GetTrade();
    baseRec.patternEnum     = currentTrade.GetPatternEnum();
    baseRec.modelConfidence = currentTrade.GetConfidence();
    baseRec.action          = (currentTrade.GetSide() == TradeSideEnum::LONG)  ? "ENTER_LONG" :
                              (currentTrade.GetSide() == TradeSideEnum::SHORT) ? "ENTER_SHORT" : "FLAT";

    // Market snapshot from LocalRiskContext
    const auto& lrc = ContextManager::Instance().GetLocalRiskContext();
    baseRec.context.amihudIlliquidity  = lrc.amihudIlliquidity;
    baseRec.context.spreadStress       = lrc.spreadStress;
    baseRec.context.shannonFlowEntropy = lrc.shannonFlowEntropy;
    baseRec.context.shannonEfficiency  = lrc.shannonEfficiency;
    baseRec.context.talebKurtosis      = lrc.talebKurtosis;
    baseRec.context.talebSkewness      = lrc.talebSkewness;
    baseRec.context.elderChandelierATR = lrc.elderChandelierATR;
    baseRec.context.paretoTailAlpha    = lrc.paretoTailAlpha;
    baseRec.context.hurstExponent      = lrc.hurstExponent;
    baseRec.context.raschkeBurst       = lrc.raschkeBurst;
    baseRec.context.fisherInfo         = lrc.fisherInfo;
    baseRec.context.regimeDuration     = lrc.regimeDuration;

    // Account state snapshot
    baseRec.context.dailyPnL           = GetDailyPnL();
    baseRec.context.accountEquity      = GetAccountEquity(sc);
    baseRec.context.tradesToday        = sc.GetPersistentInt(RISK_TRADES_EXECUTED_TODAY_ID);
    baseRec.context.consecutiveLosses  = sc.GetPersistentInt(RISK_CONSECUTIVE_LOSSES_ID);
    const SCDateTime valNow            = sc.CurrentSystemDateTime;
    baseRec.context.currentTimeMinutes = valNow.GetHour() * 60 + valNow.GetMinute();

    // Helper: emit rejection record on gate failure
    auto emitRejection = [&](const Result<void>& result, const char* gate,
                             int ordinal, const char* validator) {
        RejectionRecord rec = baseRec;
        rec.gate       = gate;
        rec.gateOrdinal = ordinal;
        rec.validator  = validator;
        rec.reasonId   = result.errorCode();
        rec.reasonCode = result.error();
        RejectionLedger::Instance().Record(rec);
    };

    // P-AER §7.5: diagnostic mode evaluates ALL gates, records full-chain verdicts
    const bool diag = m_execParams.diagnosticMode;
    std::optional<Result<void>> firstFailure;

    // Execute validator chain - propagate first error
    auto latencyGate = EvaluateLatencyGate(sc, inferenceLatencyUs, transformerLatencyUs, regimeLatencyUs);
    if (latencyGate.hardReject) {
        SCString msg;
        msg.Format(
            "⚠️ LATENCY BREAKER: regime=%ldus, transformer=%ldus, total=%ldus | posture=SHANNON_CHAOS",
            regimeLatencyUs, transformerLatencyUs, inferenceLatencyUs
        );
        Logger::getInstance().log(msg.GetChars());

        if (!m_execParams.latencyGateObserveOnly) {
            auto latencyFailure = Result<void>::Failure("Latency breaker tripped (>5ms regime latency)", 1013);
            emitRejection(latencyFailure, "LATENCY_BREAKER", 1, "EvaluateLatencyGate");
            if (!diag) return latencyFailure;
            if (!firstFailure) firstFailure = latencyFailure;
        }

        // Shadow mode: don't reject, but force defensive floor size.
        latencyGate.riskFactor = m_execParams.regimeLatencyMinFactor;
    }

    if (latencyGate.degraded && quantity > 0) {
        int scaledQty = static_cast<int>(std::floor(static_cast<double>(quantity) * latencyGate.riskFactor));
        scaledQty = std::max(1, scaledQty);

        if (scaledQty < quantity) {
            SCString msg;
            msg.Format(
                "RiskManager: Latency penalty applied (%s): qty %d -> %d (factor=%.2f)",
                latencyGate.reason,
                quantity,
                scaledQty,
                latencyGate.riskFactor
            );
            Logger::getInstance().log(msg.GetChars());
            quantity = scaledQty;
        }
    }

    auto monthlyResult = ValidateMonthlyLimit(sc);
    if (!monthlyResult) { emitRejection(monthlyResult, "MONTHLY_LIMIT", 2, "ValidateMonthlyLimit"); if (!diag) return monthlyResult; if (!firstFailure) firstFailure = monthlyResult; }

    auto haltResult = ValidateTradingHalt(sc);
    if (!haltResult) { emitRejection(haltResult, "TRADING_HALT", 3, "ValidateTradingHalt"); if (!diag) return haltResult; if (!firstFailure) firstFailure = haltResult; }

    auto hoursResult = ValidateTradingHours(sc);
    if (!hoursResult) { emitRejection(hoursResult, "TRADING_HOURS", 4, "ValidateTradingHours"); if (!diag) return hoursResult; if (!firstFailure) firstFailure = hoursResult; }

    auto coolingResult = ValidateCoolingPeriod(sc);
    if (!coolingResult) { emitRejection(coolingResult, "COOLING_PERIOD", 5, "ValidateCoolingPeriod"); if (!diag) return coolingResult; if (!firstFailure) firstFailure = coolingResult; }

    auto rapidFireResult = ValidateRapidFire(sc);
    if (!rapidFireResult) { emitRejection(rapidFireResult, "RAPID_FIRE", 6, "ValidateRapidFire"); if (!diag) return rapidFireResult; if (!firstFailure) firstFailure = rapidFireResult; }

    auto kurtosisEmergencyResult = ValidateKurtosisEmergencyGate(sc);
    if (!kurtosisEmergencyResult) { emitRejection(kurtosisEmergencyResult, "KURTOSIS_EMERGENCY", 7, "ValidateKurtosisEmergencyGate"); if (!diag) return kurtosisEmergencyResult; if (!firstFailure) firstFailure = kurtosisEmergencyResult; }

    auto qualityResult = ValidatePatternQuality(sc);
    if (!qualityResult) { emitRejection(qualityResult, "PATTERN_QUALITY", 8, "ValidatePatternQuality"); if (!diag) return qualityResult; if (!firstFailure) firstFailure = qualityResult; }

    auto correlationResult = ValidateCrossMarketCorrelation(sc);
    if (!correlationResult) { emitRejection(correlationResult, "CROSS_MARKET", 9, "ValidateCrossMarketCorrelation"); if (!diag) return correlationResult; if (!firstFailure) firstFailure = correlationResult; }

    auto riskResult = ValidateRiskLimits(sc, entryPrice, stopPrice, quantity);
    if (!riskResult) { emitRejection(riskResult, "RISK_LIMITS", 10, "ValidateRiskLimits"); if (!diag) return riskResult; if (!firstFailure) firstFailure = riskResult; }

    // ═══════════════════════════════════════════════════════════════════════
    // TDE SHADOW MODE (PAER §9 Phase 1) — observation only, no behavioral change.
    // All 5 TDE layers computed here at the terminal decision point.
    // The legacy chain above remains the sole decision-maker.
    //
    // NOTE: When diag is false and a gate short-circuits, this block only runs
    // for opportunities that pass ALL gates (the denominator for win-rate).
    // Rejected opportunities are captured by the RejectionLedger.
    // Phase 2 will force full-chain evaluation for complete TDE coverage.
    // ═══════════════════════════════════════════════════════════════════════
    if (m_execParams.tdeShadowEnabled) {
        // Layer 1: TradingClearance — map existing gate results to 8 booleans
        TradingClearance clearance;
        clearance.connectivity   = !AIConnectionMonitor::Instance().IsTransportDegraded();
        clearance.latency        = !latencyGate.hardReject;
        clearance.timeWindow     = hoursResult.isOk();
        clearance.tradingHalt    = haltResult.isOk();
        clearance.coolingPeriod  = coolingResult.isOk();
        clearance.rapidFire      = rapidFireResult.isOk();
        clearance.monthlyLimit   = monthlyResult.isOk();
        clearance.entriesAllowed = true;  // No operator switch yet — always true

        // Layer 2: ConvictionScore — weighted signal quality scalar
        const auto* pred = InferenceManager::Instance().Prediction();
        ConvictionInputs convIn;
        convIn.modelConfidence     = currentTrade.GetConfidence();
        convIn.actionEntropy       = pred ? pred->ActionEntropy() : 2.2f;
        convIn.top2Margin          = pred ? pred->Top2Margin() : 0.0f;
        convIn.thesisStrength      = pred ? pred->ThesisStrength() : 0.0f;
        convIn.predictionFreshness = pred ? static_cast<float>(pred->SemanticFreshnessDiscount(
            IndicatorManager::Instance().GlobalSequenceId())) : 1.0f;
        // biasAlignment, impulseAlignment, patternQuality default to 0.0 (not yet wired)
        const auto convResult = ComputeConviction(convIn);

        // Layer 3: RiskPrice — dollar-denominated risk with additive premiums
        // (PAER §9.6: replaces multiplicative shrinkage with structured pricing)
        RiskPriceInputs rpIn;
        rpIn.stopDistanceTicks  = fabs(entryPrice - stopPrice) / sc.TickSize;
        rpIn.currencyPerTick    = sc.CurrencyValuePerTick;
        rpIn.atrRatio           = (m_atr14Avg > 0.0f) ? (m_atr14 / m_atr14Avg) : 1.0f;
        rpIn.amihudPercentile   = lrc.amihudPercentile;
        rpIn.spreadStress       = lrc.spreadStress;
        rpIn.shannonFlowEntropy = lrc.shannonFlowEntropy;
        rpIn.talebKurtosis      = lrc.talebKurtosis;
        rpIn.paretoTailAlpha    = lrc.paretoTailAlpha;
        auto* hmmShadow = InferenceManager::Instance().HmmState();
        rpIn.mahalanobis        = hmmShadow ? hmmShadow->Mahalanobis() : 0.0f;
        rpIn.hmmTransitionRisk  = hmmShadow ? hmmShadow->TransitionRisk() : 0.0f;
        rpIn.hmmEntropy         = hmmShadow ? hmmShadow->Entropy() : 0.0f;
        const double peakEq = sc.GetPersistentDouble(RISK_PEAK_EQUITY_ID);
        const double eqNow  = baseRec.context.accountEquity;
        rpIn.drawdownPct = (peakEq > 0.0 && eqNow < peakEq)
                         ? static_cast<float>((peakEq - eqNow) / peakEq) : 0.0f;
        const auto* volInd = IndicatorManager::Instance().GetIndicator<VolumeIndicator>(IndicatorKey::VOLUME_SIGNAL);
        rpIn.volumeImbalance = volInd ? volInd->GetVolumeImbalance() : 0.0f;
        rpIn.volumeZScore    = volInd ? volInd->GetVolumeRatio() : 0.0f;
        rpIn.isLong          = (currentTrade.GetSide() == TradeSideEnum::LONG);
        const auto rpResult = ComputeRiskPrice(rpIn);

        // Layer 4: TradeDecision — synthesis (would TDE have agreed?)
        TradeDecision decision;
        decision.clearance      = clearance;
        decision.conviction     = convResult;
        decision.riskPrice      = rpResult;
        decision.dailyRiskBudget = eqNow * m_execParams.maxRiskPerTradeFrac;
        decision.Synthesize(m_execParams.tdeConvictionFloor, m_execParams.tdePositionCap);

        // Layer 5: OpportunityRecord — full JSONL record for offline calibration
        OpportunityRecord opp;
        opp.timestampUs      = baseRec.timestampUs;
        opp.timestampHuman   = baseRec.timestampHuman;
        opp.paramSetId       = baseRec.paramSetId;
        opp.sessionDate      = baseRec.sessionDate;
        opp.symbol           = m_invariants.symbol;
        opp.action           = baseRec.action;
        opp.patternEnum      = baseRec.patternEnum;
        opp.modelConfidence  = baseRec.modelConfidence;
        opp.entryPrice       = entryPrice;
        opp.stopPrice        = stopPrice;
        opp.proposedQuantity = quantity;
        opp.clearance        = clearance;
        opp.conviction       = convResult;
        opp.riskPrice        = rpResult;
        opp.decision         = decision;
        opp.legacyExecuted   = !firstFailure.has_value();
        opp.legacyDenyGate   = firstFailure ? firstFailure->error() : "";
        opp.legacyDenyCode   = firstFailure ? firstFailure->errorCode() : 0;
        opp.legacyFinalSize  = quantity;
        opp.context          = baseRec.context;
        OpportunityLedger::Instance().Record(opp);

        // Stash shadow results for downstream inspection / debugging
        m_shadowClearance  = clearance;
        m_shadowConviction = convResult;
        m_shadowRiskPrice  = rpResult;
        m_shadowTdeReady   = true;
    }

    // In diagnostic mode, return first failure after evaluating all gates
    if (firstFailure) return *firstFailure;

    // All checks passed
    return Result<void>::Success();
}

void RiskManager::LogTradeValidation(SCStudyInterfaceRef sc, double entryPrice, double stopPrice, int quantity, const char* tradeDescription) {
    // Comprehensive pre-trade validation logging for manual trading decisions (SC logs only)
    SCString logMsg;
    logMsg.Format("========== TRADE VALIDATION: %s ==========", tradeDescription);
    Logger::getInstance().log(logMsg.GetChars());

    // Current risk state (use cached daily P&L)
    const double dailyPnL = GetDailyPnL();
    const double accountEquity = GetAccountEquity(sc);
    const double totalExposure = CalculateTotalExposure(sc);
    const int consecutiveLosses = sc.GetPersistentInt(RISK_CONSECUTIVE_LOSSES_ID);
    const bool isHalted = IsTradingHalted(sc);

    // Show psychological metrics (budget %, ticks) instead of raw dollars
    const double maxDailyLoss = accountEquity * 0.02;
    const double budgetRemaining = (dailyPnL < 0) ? 100.0 * (1.0 - (fabs(dailyPnL) / maxDailyLoss)) : 100.0;
    const double netTicks = GetNetTicksToday(sc);

    logMsg.Format("Risk Budget: %.1f%% | Net Ticks Today: %.1f | Exposure: %.1f%% of account",
                  budgetRemaining, netTicks, (totalExposure/accountEquity)*100.0);
    Logger::getInstance().log(logMsg.GetChars());

    logMsg.Format("Consecutive Losses: %d | Trading Halted: %s",
                  consecutiveLosses, isHalted ? "YES" : "NO");
    Logger::getInstance().log(logMsg.GetChars());

    // Trade-specific risk calculation
    const double orderRisk = CalculateOrderRisk(sc, entryPrice, stopPrice, quantity);
    const double riskPct = (orderRisk / accountEquity) * 100.0;
    const double newTotalExposure = totalExposure + orderRisk;
    const double newExposurePct = (newTotalExposure / accountEquity) * 100.0;

    logMsg.Format("Proposed Trade: Entry=%.2f | Stop=%.2f | Qty=%d", entryPrice, stopPrice, quantity);
    Logger::getInstance().log(logMsg.GetChars());

    logMsg.Format("Order Risk: $%.2f (%.2f%% of account)", orderRisk, riskPct);
    Logger::getInstance().log(logMsg.GetChars());

    logMsg.Format("New Total Exposure: $%.2f (%.2f%% of account)", newTotalExposure, newExposurePct);
    Logger::getInstance().log(logMsg.GetChars());

    // Validation result
    int adjustedQuantity = quantity;
    const auto validationResult = ValidateOrder(sc, entryPrice, stopPrice, adjustedQuantity);
    const bool wouldPass = validationResult.isOk();

    if (wouldPass) [[likely]] {
        Logger::getInstance().log(">>> VALIDATION: SAFE TO TRADE");
    } else {
        Logger::getInstance().log(">>> VALIDATION: REJECT - RISK LIMITS EXCEEDED");

        // Specific failure reasons
        if (isHalted) {
            Logger::getInstance().log("   Reason: Trading is HALTED");
        }
        if (dailyPnL <= -(accountEquity * m_execParams.elder2PctRuleFrac)) {
            Logger::getInstance().log("   Reason: Daily loss limit reached");
        }
        if (riskPct > m_execParams.maxRiskPerTradeFrac * 100.0) {
            Logger::getInstance().log("   Reason: Exceeds 2% per-trade risk limit");
        }
        if (newExposurePct > m_execParams.elderPortfolioHeatFrac * 100.0) {
            Logger::getInstance().log("   Reason: Exceeds 6% portfolio heat limit");
        }
    }

    Logger::getInstance().log("=================================================");
}

Result<int> RiskManager::CalculateSafePositionSize(SCStudyInterfaceRef sc, double entryPrice, double stopPrice, float modelConfidence) {
    RefreshKurtosisEmergencyState(sc);

    // ─────────────────────────────────────────────────────────────────────
    // SUBSUMED-PARAMETER INVENTORY (Phase 2 consolidation candidates)
    //
    // The TDE RiskPrice layer (computed in ValidateOrder) now captures the
    // SAME risk factors that the ad-hoc multiplier chain below applies
    // individually.  In Phase 2 these legacy multipliers can be replaced
    // by the single RiskPrice-derived size, eliminating double-counting.
    //
    //  Legacy multiplier              → TDE layer / premium
    //  ─────────────────────────────────────────────────────────
    //  paretoMultiplier (sigmoid)     → ConvictionScore c1 (confidence)
    //  convictionScale  (top2Margin)  → ConvictionScore c3
    //  thesisMultiplier               → ConvictionScore c4
    //  entropyDiscount                → ConvictionScore c2
    //  FreshnessDiscount              → ConvictionScore c8
    //  GetAtrVolatilityMultiplier()   → RiskPrice volatilityPremium
    //  DeepContextMultiplier (Amihud…) → RiskPrice microstructurePremium
    //  HMM RiskMultiplier             → RiskPrice regimeUncertaintyPremium
    //  HMM SizingDurationFactor       → RiskPrice regimeUncertaintyPremium
    //  MahalanobisSizingCap           → RiskPrice tailRiskPremium
    //  TailWeightDiscount             → RiskPrice tailRiskPremium
    //  Hill alpha gate                → RiskPrice tailRiskPremium
    //  Triple-confirmed zero          → TradingClearance + deny layer
    //  Session wind-down              → (not yet in TDE — Phase 2 addition)
    //  Consecutive-loss halt          → TradingClearance (future)
    //  Cascade boost                  → ConvictionScore (by design)
    // ─────────────────────────────────────────────────────────────────────

    const double accountEquity = GetAccountEquity(sc);
    const double maxRisk = accountEquity * m_execParams.maxRiskPerTradeFrac;

    // Physical Risk (C++ Domain)
    const double stopDistance = fabs(entryPrice - stopPrice);
    const double riskPerContract = (stopDistance / sc.TickSize) * sc.CurrencyValuePerTick;
    if (riskPerContract <= 0.0) [[unlikely]] return Result<int>::Failure("Invalid stop distance", 2001);

    int baseSize = static_cast<int>(maxRisk / riskPerContract);

    // Direct, zero-logic multiplication.
    // The Brain has already factored in Taleb, Shannon, and Pareto.
    double riskMultiplier = 1.0;

    // PAER §8.4: Sizing bucket decomposition — track per-bucket products
    SizingBuckets buckets;

    // === ELITE: PARETO CONFIDENCE SIZING — Continuous Sigmoid (Gap 4 closure) ===
    // Smooth sigmoid: 0.5x at low confidence → 3.0x at very high confidence
    // Midpoint at confidence=0.75 (1.0x), matches existing risk posture at operating points
    // f(0.50) ≈ 0.5, f(0.75) ≈ 1.0, f(0.90) ≈ 1.7, f(0.95) ≈ 2.2, f(0.99) ≈ 2.8
    const double paretoMultiplier = 0.5 + 2.5 / (1.0 + std::exp(-12.0 * (static_cast<double>(modelConfidence) - 0.75)));

    // === ELITE: TOP2_MARGIN CONVICTION SHARPENING (Proposal 1 — Shannon) ===
    // Sharpen Pareto output by the gap between best and runner-up softmax probs.
    // At top2_margin ≥ 0.30, full pass-through; near 0 → halved.
    const auto* pred = InferenceManager::Instance().Prediction();
    const float top2m = pred ? pred->Top2Margin() : 0.0f;
    const double convictionScale = 0.5 + 0.5 * std::clamp(static_cast<double>(top2m) / 0.3, 0.0, 1.0);

    // === GAP 13: THESIS STRENGTH → SIZING MULTIPLIER (Transformer confidence quality) ===
    // thesisStrength is the Transformer's internal measure of how strongly the model's
    // feature activations support the predicted direction.  Range [0, 1].
    // Full sizing at thesisStrength ≥ 0.80; scales down to 0.70× at thesisStrength=0.
    const float thesisStr = pred ? pred->ThesisStrength() : 0.5f;
    const double thesisMultiplier = 0.70 + 0.30 * std::clamp(static_cast<double>(thesisStr) / 0.80, 0.0, 1.0);

    // === ELITE: ENTROPY SIZING DISCOUNT (Shannon — continuous uncertainty penalty) ===
    // §8.2: Continuous curve replaces the old hard gate at 1.8 (cliff removed).
    // Entropy in [1.0, 2.0] → continuous discount from 1.0× down to 0.0×.
    // Below 1.0 → no penalty (model is decisive). Max entropy ln(9) ≈ 2.197.
    // TDE hard veto at 2.0 is the backstop — anything above is already vetoed.
    const float actionEntropy = pred ? pred->ActionEntropy() : 1.5f;
    double entropyDiscount = 1.0;
    if (actionEntropy > 1.0f) {
        entropyDiscount = std::max(0.0, 1.0 - std::clamp(
            (static_cast<double>(actionEntropy) - 1.0) / 1.0, 0.0, 1.0));
    }

    const double effectiveParetoMultiplier = paretoMultiplier * convictionScale * entropyDiscount * thesisMultiplier;

    // Scale the multiplier by sharpened Pareto factor
    riskMultiplier *= effectiveParetoMultiplier;
    buckets.model = effectiveParetoMultiplier;

    // --- HMM REGIME RISK FACTORS (single source of truth: HmmStateIndicator) ---
    auto* hmmInd = InferenceManager::Instance().HmmState();
    if (hmmInd) {
        const double hmmRisk = hmmInd->RiskMultiplier();
        riskMultiplier *= hmmRisk;
        buckets.regime *= hmmRisk;
    }

    // === REGIME STABILITY: EXPECTED DURATION SIZING FACTOR ===
    // Logic lives in HmmStateIndicator::SizingDurationFactor().
    if (hmmInd) {
        const double hmmDur = hmmInd->SizingDurationFactor();
        riskMultiplier *= hmmDur;
        buckets.regime *= hmmDur;
    }

    // --- ELITE v3.2: DEEP CONTEXT via LocalRiskContext (single source of truth) ---
    const auto& localCtx = ContextManager::Instance().GetLocalRiskContext();
    if (localCtx.isValid) {
        const double deepCtx = Scoring::Instance().GetDeepContextMultiplier(PatternType::Unknown, localCtx);
        riskMultiplier *= deepCtx;
        buckets.market *= deepCtx;
    }

    // Execution Overrides (C++ Safety Domain)
    const double atrVol = GetAtrVolatilityMultiplier();
    riskMultiplier *= atrVol;
    buckets.market *= atrVol;

    // Ghost Protocol v1: DEGRADEDFLIGHT posture for transport stalls.
    // Compute health may be fine, but stale transport can still distort fills.
    if (AIConnectionMonitor::Instance().IsTransportDegraded()) {
        riskMultiplier *= 0.50;
        buckets.safety *= 0.50;
    }

    // === CONTINUOUS DRAWDOWN THROTTLE (CPPI-style equity-curve de-risking) ===
    // Perold-Sharpe (1988) CPPI cushion / Grossman-Zhou (1993) drawdown control:
    // scale per-trade risk down linearly as equity falls from peak toward the hard
    // drawdownHalt floor, rather than running full size right up to the cliff. The
    // equity curve is an axis independent of microstructure/regime, so this factor
    // compounds multiplicatively with the sensors above (same doctrine as dual-tail).
    // The 25% drawdownHalt inside GetRiskMultiplier remains the non-negotiable backstop.
    const double drawdownThrottle = GetRiskMultiplier(sc);
    if (drawdownThrottle < 1.0) {
        riskMultiplier *= drawdownThrottle;
        buckets.safety *= drawdownThrottle;
        Logger::getInstance().log(
            "RiskManager: DRAWDOWN THROTTLE active: multiplier=" + std::to_string(drawdownThrottle) +
            " (continuous CPPI-style de-risking toward hard halt)");
    }

    if (m_kurtosisEmergencyActive.load(std::memory_order_relaxed)) {
        const double cap = static_cast<double>(m_execParams.talebKurtosisCrisisCeiling);
        if (riskMultiplier > cap && riskMultiplier > 1e-9) {
            buckets.safety *= cap / riskMultiplier;
        }
        riskMultiplier = std::min(riskMultiplier, cap);
    }

    // Gap 6: Log model vs realized kurtosis divergence for monitoring
    const float wire_dof = hmmInd ? hmmInd->Dof() : 0.0f;
    const float model_kurt = hmmInd ? hmmInd->ModelKurtosis() : 0.0f;
    if (model_kurt > 0.0f) {
        const float realized_kurt = m_lastObservedKurtosis.load(std::memory_order_relaxed);
        if (std::isfinite(realized_kurt) && realized_kurt > model_kurt * 1.5f) {
            Logger::getInstance().log(
                "RiskManager: KURTOSIS DIVERGENCE: realized=" + std::to_string(realized_kurt) +
                " > 1.5x model=" + std::to_string(model_kurt) +
                " (dof=" + std::to_string(wire_dof) + ") — realized override active");
        }
    }

    // === ELITE: MAHALANOBIS OUTLIER GATE (Proposal 3 — Taleb+Elder) ===
    // Logic lives in HmmStateIndicator::MahalanobisSizingCap().
    const float wire_mahal = hmmInd ? hmmInd->Mahalanobis() : 0.0f;
    if (hmmInd) {
        const double mahalCap = hmmInd->MahalanobisSizingCap();
        riskMultiplier *= mahalCap;
        buckets.regime *= mahalCap;
    }

    // === ELITE: TAIL WEIGHT DISCOUNT (Proposal 6 — Taleb) ===
    // Logic lives in HmmStateIndicator::TailWeightDiscount().
    const float wire_tailWeight = hmmInd ? hmmInd->TailWeight() : 0.0f;
    if (hmmInd) {
        const double tailDisc = hmmInd->TailWeightDiscount();
        riskMultiplier *= tailDisc;
        buckets.regime *= tailDisc;
    }

    // === P3.4: HILL ALPHA OPPORTUNITY SIGNAL (Taleb Convexity) ===
    // When alpha < 1.5 (fat tails) PLUS PARETO_MOMENTUM + strong directional conviction,
    // the fat tails are directional opportunity — PARETO override instead of P1.4 gate.
    // This inverts the defensive posture into convex sizing for high-conviction flushes.
    const float paretoTailAlpha = ContextManager::Instance().GetCachedHillAlpha();

    // === GAP 1 FIX: DOF ↔ HILL ALPHA COHERENCE MONITOR ===
    // TailRiskEngine (Hill α): univariate, non-parametric, 1D |log returns|
    // Student-t HMM (DOF): multivariate, parametric, 16D observation vector
    // They measure different dimensionalities but both detect fat tails.
    // Log warning when one says danger and the other doesn't.
    if (paretoTailAlpha > 0.0f && wire_dof > 1e-6f) {
        const bool hill_danger = (paretoTailAlpha < 1.5f);
        const bool dof_danger = (wire_dof < 4.0f);  // DOF < 4 → infinite kurtosis
        if (hill_danger && !dof_danger && wire_dof > 8.0f) {
            Logger::getInstance().log(
                "RiskManager: TAIL COHERENCE DIVERGENCE: Hill α=" + std::to_string(paretoTailAlpha) +
                " (fat-tail DANGER) but DOF=" + std::to_string(wire_dof) +
                " (near-Gaussian) — univariate detects tails HMM misses");
        } else if (!hill_danger && paretoTailAlpha > 3.0f && dof_danger) {
            Logger::getInstance().log(
                "RiskManager: TAIL COHERENCE DIVERGENCE: Hill α=" + std::to_string(paretoTailAlpha) +
                " (thin-tail SAFE) but DOF=" + std::to_string(wire_dof) +
                " (fat-tail DANGER) — HMM detects multivariate tails Hill misses");
        }
    }
    bool hillOpportunityActive = false;
    if (paretoTailAlpha > 0.0f && paretoTailAlpha < 1.5f && top2m > 0.30f) {
        if (hmmInd && hmmInd->Value() == HMMStateEnum::PARETO_MOMENTUM) {
            // Fat-tail opportunity: directional flush with strong conviction.
            // Boost sizing 1.5-2.0× via linear ramp on top2_margin [0.30, 0.50].
            double opportunityBoost = 1.5 + 0.5 * std::clamp((static_cast<double>(top2m) - 0.30) / 0.20, 0.0, 1.0);
            riskMultiplier *= opportunityBoost;
            buckets.reward *= opportunityBoost;
            hillOpportunityActive = true;
        }
    }

    // === P1.4: HILL ALPHA DIRECT TRADING GATE (Taleb) ===
    // Hill alpha < 2.0 → fat tails. Direct sizing gate, not just HMM input feature.
    // alpha < 1.5 → 0.25× (extreme fat tails, near-infinite variance).
    // alpha < 1.2 → 0.0× (infinite variance territory — stand aside entirely).
    // alpha [1.5, 2.0) → continuous sigmoid ramp from 0.25× to 1.0×.
    // P3.4: Skip this gate when opportunity signal is active (PARETO_MOMENTUM + conviction).
    //
    // GAP 4 NOTE (INTENTIONAL DESIGN): When BOTH tail_weight discount AND this gate fire,
    // sizing compounds multiplicatively (e.g., 0.5× tail_weight × 0.25× hillGate = 0.125×).
    // This is CORRECT Talebian layered defense — two independent sensors confirming fat tails
    // SHOULD stack. TailRiskEngine is non-parametric/univariate; HMM tail_weight is
    // parametric/multivariate. Independent confirmation → compounding is conservative by design.
    const bool tailWeightFired = (wire_tailWeight < 0.7f && wire_tailWeight >= 0.0f);
    bool hillAlphaZeroed = false;
    if (!hillOpportunityActive && paretoTailAlpha > 0.0f && paretoTailAlpha < 2.0f) {
        double hillGate;
        if (paretoTailAlpha < 1.2f) {
            hillGate = 0.0;  // Infinite variance — refuse to trade
            hillAlphaZeroed = true;
        } else if (paretoTailAlpha < 1.5f) {
            hillGate = 0.25;  // Near-infinite variance — minimal exposure
        } else {
            // Continuous sigmoid: 1.5 → 0.25, 2.0 → ~1.0
            hillGate = 0.25 + 0.75 / (1.0 + std::exp(-6.0 * (static_cast<double>(paretoTailAlpha) - 1.75)));
        }
        riskMultiplier *= hillGate;
        buckets.safety *= hillGate;

        // GAP 4: Monitor when both tail sensors compound (institutional observability)
        if (tailWeightFired) {
            Logger::getInstance().log(
                "RiskManager: DUAL-TAIL COMPOUND: tailWeight=" + std::to_string(wire_tailWeight) +
                " + hillGate=" + std::to_string(hillGate) +
                " (α=" + std::to_string(paretoTailAlpha) +
                ") — layered Talebian defense active");
        }
    }

    // === P1.3: TRIPLE-CONFIRMED ZERO SIZING (Taleb) ===
    // When multiple independent danger signals confirm, allow sizing to reach zero.
    // Triple confirmation: Mahalanobis + kurtosis emergency + Hill alpha danger
    // P3.4: Hill alpha is NOT danger when opportunity signal overrides it.
    const bool mahalDanger = (wire_mahal > 6.0f);
    const bool kurtosisDanger = m_kurtosisEmergencyActive.load(std::memory_order_relaxed);
    const bool hillDanger = (!hillOpportunityActive && paretoTailAlpha > 0.0f && paretoTailAlpha < 1.5f);
    const int dangerCount = static_cast<int>(mahalDanger) + static_cast<int>(kurtosisDanger) + static_cast<int>(hillDanger);
    if (dangerCount >= 2) {
        // Double-confirmed: force zero sizing (was previously floored at 1 contract)
        riskMultiplier = 0.0;
        buckets.safety = 0.0;
    }
    const bool tripleConfirmZeroed = (dangerCount >= 2);

    // === GAP 6: CONVICTION CASCADE BOOST (Pareto — 80/20 elite trade sizing) ===
    // The top 20% of setups (where Shannon, Taleb, Pareto, and HMM all confirm)
    // generate 80% of P&L.  When multiple independent conviction signals align,
    // apply a modest cascade boost — these are the trades you SIZE UP on.
    // Only applies when no danger signals are active (riskMultiplier > 0).
    if (riskMultiplier > 0.01 && !hillOpportunityActive && dangerCount == 0) {
        int cascadeCount = 0;
        if (actionEntropy < 0.6f)          cascadeCount++;  // Shannon: decisive model
        if (top2m > 0.35f)                 cascadeCount++;  // Shannon: clear winner
        if (modelConfidence > 0.90f)       cascadeCount++;  // Pareto: strong conviction
        if (hmmInd && hmmInd->TransitionRisk() < 0.15f)
                                           cascadeCount++;  // HMM: stable regime
        const float rk = ContextManager::Instance().GetCachedHillAlpha();
        if (rk > 2.5f || rk <= 0.0f)      cascadeCount++;  // Taleb: finite variance (or no data)
        const auto& lrc = ContextManager::Instance().GetLocalRiskContext();
        if (lrc.isValid && lrc.talebKurtosis < 3.0f)
                                           cascadeCount++;  // Taleb: normal tails

        if (cascadeCount >= 5) {
            // 5+ of 6 conviction signals confirm → 1.25× boost (capped below by abs limit)
            riskMultiplier *= 1.25;
            buckets.reward *= 1.25;
        }
    }

    // === GAP 2: PREDICTION FRESHNESS DISCOUNT (Semantic + Shannon backstop) ===
    // Primary: event-count semantic freshness (correct for event-driven architecture).
    // Backstop: wall-clock Shannon decay (catches stalled pipeline / hung inference).
    {
        const uint64_t nowUs = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
        const double semanticFreshness = pred ? pred->SemanticFreshnessDiscount(
            IndicatorManager::Instance().GlobalSequenceId()) : 1.0;
        const double wallClockFreshness = pred ? pred->FreshnessDiscount(nowUs) : 1.0;
        const double freshness = std::min(semanticFreshness, wallClockFreshness);
        if (freshness < 1.0) {
            riskMultiplier *= freshness;
            buckets.timing *= freshness;
        }
    }

    // === GAP 16: SESSION WIND-DOWN SIZING (Elder — time-of-day awareness) ===
    // §8.7 Realignment: Wind-down moved from 3:30-4:00 PM (dead — overlapped
    // END_OF_DAY_CUTOFF) to 2:30-3:00 PM so it applies BEFORE the no-new-entry
    // cutoff at 3:00 PM. Curve: 2:30 PM → 1.0×, 3:00 PM → 0.50×.
    {
        SCDateTime currentDT = sc.CurrentSystemDateTime;
        int currentMinutes = currentDT.GetTimeInSeconds() / 60;
        // SC times are exchange-local (CT for CME). 870 min = 14:30, 900 min = 15:00.
        if (currentMinutes >= 870 && currentMinutes <= 900) {
            const double sessionDecay = std::max(0.50, 1.0 - static_cast<double>(currentMinutes - 870) / 60.0);
            riskMultiplier *= sessionDecay;
            buckets.timing *= sessionDecay;
        }
    }

    const int consecutiveLosses = sc.GetPersistentInt(RISK_CONSECUTIVE_LOSSES_ID);
    const bool lossStreakZeroed = (consecutiveLosses >= 2);
    if (lossStreakZeroed) {
        riskMultiplier = 0.0;
        buckets.safety = 0.0;
    }

    buckets.final_rm = riskMultiplier;

    int adjustedSize = static_cast<int>(baseSize * riskMultiplier);
    // P1.3: Allow zero sizing when riskMultiplier is zero (no more 1-contract floor)
    if (adjustedSize < 1 && riskMultiplier > 0.01) adjustedSize = 1;

    // §8.4: Sizing bucket decomposition log (observational — no math change)
    if (riskMultiplier < 0.99 || adjustedSize == 0) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "SIZING: model=%.3f regime=%.3f market=%.3f safety=%.3f timing=%.3f reward=%.3f -> final=%.4f base=%d adj=%d",
            buckets.model, buckets.regime, buckets.market,
            buckets.safety, buckets.timing, buckets.reward,
            buckets.final_rm, baseSize, adjustedSize);
        Logger::getInstance().log(buf);
    }

    // §8.5 + P-AER: Record zero-sizing events with named rejection codes.
    // Each zero-forcing path gets its own gate name and reasonId for counterfactual analysis.
    if (adjustedSize == 0) {
        // Build a shared base record (identity + market snapshot + buckets)
        auto makeBaseRecord = [&]() {
            RejectionRecord rec;
            rec.timestampUs     = RejectionLedger::NowMicroseconds();
            rec.timestampHuman  = RejectionLedger::NowHumanTimestamp();
            rec.paramSetId      = m_execParams.paramSetId;
            rec.sessionDate     = RejectionLedger::TodaySessionDate();
            rec.entryPrice      = entryPrice;
            rec.stopPrice       = stopPrice;
            rec.modelConfidence = modelConfidence;
            rec.gateOrdinal     = 0;
            rec.validator       = "CalculateSafePositionSize";
            rec.metricName      = "risk_multiplier";
            rec.metricValue     = riskMultiplier;
            rec.thresholdValue  = 0.01;
            rec.thresholdParam  = "min_nonzero_multiplier";
            rec.margin          = riskMultiplier;
            rec.sizingBuckets   = buckets;

            rec.context.amihudIlliquidity  = localCtx.amihudIlliquidity;
            rec.context.spreadStress       = localCtx.spreadStress;
            rec.context.shannonFlowEntropy = localCtx.shannonFlowEntropy;
            rec.context.shannonEfficiency  = localCtx.shannonEfficiency;
            rec.context.talebKurtosis      = localCtx.talebKurtosis;
            rec.context.talebSkewness      = localCtx.talebSkewness;
            rec.context.elderChandelierATR = localCtx.elderChandelierATR;
            rec.context.paretoTailAlpha    = localCtx.paretoTailAlpha;
            rec.context.hurstExponent      = localCtx.hurstExponent;
            rec.context.raschkeBurst       = localCtx.raschkeBurst;
            rec.context.fisherInfo         = localCtx.fisherInfo;
            rec.context.regimeDuration     = localCtx.regimeDuration;
            rec.context.dailyPnL           = GetDailyPnL();
            rec.context.accountEquity      = accountEquity;
            rec.context.consecutiveLosses  = consecutiveLosses;
            return rec;
        };

        if (hillAlphaZeroed) {
            auto rec = makeBaseRecord();
            rec.gate       = "HILL_ALPHA_INFINITE_VARIANCE";
            rec.reasonId   = 2001;
            rec.reasonCode = "hill_alpha=" + std::to_string(paretoTailAlpha) + " < 1.2 (infinite variance)";
            rec.metricName = "pareto_tail_alpha";
            rec.metricValue = static_cast<double>(paretoTailAlpha);
            rec.thresholdValue = 1.2;
            rec.thresholdParam = "hill_alpha_infinite_variance_gate";
            RejectionLedger::Instance().Record(rec);
        }
        if (tripleConfirmZeroed) {
            auto rec = makeBaseRecord();
            rec.gate       = "TRIPLE_CONFIRM_ZERO";
            rec.reasonId   = 2002;
            rec.reasonCode = "danger_count=" + std::to_string(dangerCount)
                + " (mahal=" + std::to_string(mahalDanger)
                + " kurtosis=" + std::to_string(kurtosisDanger)
                + " hill=" + std::to_string(hillDanger) + ")";
            rec.metricName = "danger_count";
            rec.metricValue = dangerCount;
            rec.thresholdValue = 2.0;
            rec.thresholdParam = "triple_confirm_min_danger_signals";
            RejectionLedger::Instance().Record(rec);
        }
        if (lossStreakZeroed) {
            auto rec = makeBaseRecord();
            rec.gate       = "CONSECUTIVE_LOSS_HALT";
            rec.reasonId   = 2003;
            rec.reasonCode = "consecutive_losses=" + std::to_string(consecutiveLosses) + " >= 2";
            rec.metricName = "consecutive_losses";
            rec.metricValue = static_cast<double>(consecutiveLosses);
            rec.thresholdValue = 2.0;
            rec.thresholdParam = "max_consecutive_losses";
            RejectionLedger::Instance().Record(rec);
        }
        // Fallback: if none of the known zero-forcers are responsible, emit generic record
        if (!hillAlphaZeroed && !tripleConfirmZeroed && !lossStreakZeroed) {
            auto rec = makeBaseRecord();
            rec.gate       = "SILENT_ZERO_SIZING";
            rec.reasonId   = 2000;
            rec.reasonCode = "risk_multiplier=" + std::to_string(riskMultiplier);
            RejectionLedger::Instance().Record(rec);
        }
    }

    return Result<int>::Success(adjustedSize);
}

double RiskManager::GetAtrVolatilityMultiplier() const {
    // Continuous ATR volatility scaling (Gap 4 closure)
    // Linear interpolation: ratio 1.0 → mult 1.0; ratio 3.0 → mult 0.5
    // Below 1.0: no boost (capped at 1.0); above 4.0: floored at 0.35
    if (m_atr14Avg <= 0.0f || m_atr14 <= 0.0f) {
        return 1.0;
    }

    const double atrRatio = static_cast<double>(m_atr14) / static_cast<double>(m_atr14Avg);

    if (atrRatio <= 1.0) {
        return 1.0;  // Normal/low volatility — no adjustment
    }

    // P1.5: Exponential decay — no hard floor. Extreme vol → near-zero multiplier.
    // ratio=2.0 → 0.75, ratio=3.0 → 0.50, ratio=4.0 → 0.25, ratio=6.0 → 0.05
    const double mult = std::exp(-0.35 * (atrRatio - 1.0));
    return std::max(mult, 0.01);  // Numerical safety only (never truly zero)
}

void RiskManager::OnTradeClose(SCStudyInterfaceRef sc, double pnl) {
    // Increment daily trade counter (prevents overtrading)
    int tradesExecutedToday = sc.GetPersistentInt(RISK_TRADES_EXECUTED_TODAY_ID);
    sc.SetPersistentInt(RISK_TRADES_EXECUTED_TODAY_ID, tradesExecutedToday + 1);

    // Update consecutive loss counter
    int consecutiveLosses = sc.GetPersistentInt(RISK_CONSECUTIVE_LOSSES_ID);

    // Convert P&L to ticks for psychological tracking
    const double tickValue = sc.CurrencyValuePerTick;
    const double ticksPnL = (tickValue > 0) ? (pnl / tickValue) : 0.0;
    const double currentNetTicks = sc.GetPersistentDouble(RISK_NET_TICKS_TODAY_ID);
    sc.SetPersistentDouble(RISK_NET_TICKS_TODAY_ID, currentNetTicks + ticksPnL);

    // === PATTERN PROBATION SYSTEM: Update per-pattern statistics ===
    const Trade& closedTrade = PositionManager::Instance().GetTrade();
    const std::string patternEnum = closedTrade.GetPatternEnum();

    if (!patternEnum.empty()) {
        const bool isWin = (pnl > 0.0);
        UpdatePatternStats(patternEnum, pnl, isWin);

        // Save pattern stats to file after each trade
        SavePatternStatsToFile();
    }

    // === TRADE STATISTICS: Track trade results for diagnostics / pattern probation ===
    // Calculate initial risk from entry/stop/quantity
    const double entryPrice = closedTrade.GetEntryPrice();
    const double stopPrice = closedTrade.GetStop();
    const int quantity = static_cast<int>(closedTrade.GetSize());

    double initialRisk = 0.0;
    if (entryPrice > 0 && stopPrice > 0 && quantity > 0) {
        const double stopDistance = std::abs(entryPrice - stopPrice);
        initialRisk = (stopDistance / sc.TickSize) * sc.CurrencyValuePerTick * quantity;
    }

    m_kellyCalculator.RecordTrade(pnl, initialRisk);

    if (m_kellyCalculator.GetTradeCount() >= 10) {
        Logger::getInstance().log("Trade Stats: WinRate=" +
            std::to_string(m_kellyCalculator.GetWinRate() * 100) +
            "%, AvgWin=" + std::to_string(m_kellyCalculator.GetAvgWin()) +
            "R, AvgLoss=" + std::to_string(m_kellyCalculator.GetAvgLoss()) + "R");
    }

    if (pnl < 0.0) {
        // Loss - increment counter and record timestamp for cooling-off
        consecutiveLosses++;
        sc.SetPersistentInt(RISK_CONSECUTIVE_LOSSES_ID, consecutiveLosses);

        // Record timestamp of loss for cooling-off period
        SCDateTime now = sc.CurrentSystemDateTime;
        sc.SetPersistentInt64(RISK_LAST_LOSS_TIMESTAMP_ID, static_cast<int64_t>(now.GetAsDouble()));

        SCString msg;
        msg.Format("Loss recorded - %d-minute cooling-off period activated", m_execParams.elderRevengeCooloffMin);
        Logger::getInstance().log(msg.GetChars());

        Logger::getInstance().log("Trade closed: " + std::to_string(ticksPnL) + " ticks" +
            " - Consecutive losses: " + std::to_string(consecutiveLosses));

        // Check if we hit the consecutive loss limit
        if (consecutiveLosses >= m_execParams.elderLossStreakBreaker) {
            EmergencyHalt(sc, "2 consecutive losses - preventing revenge trading");
        }
    } else {
        // Win - reset counter
        if (consecutiveLosses > 0) {
            Logger::getInstance().log("Trade closed: +" + std::to_string(ticksPnL) + " ticks" +
                " - Resetting consecutive loss counter from " + std::to_string(consecutiveLosses));
        } else {
            Logger::getInstance().log("Trade closed: +" + std::to_string(ticksPnL) + " ticks");
        }
        sc.SetPersistentInt(RISK_CONSECUTIVE_LOSSES_ID, 0);
    }
}

bool RiskManager::IsTradingHalted() const {
    return m_tradingHalted.load(std::memory_order_acquire);
}

bool RiskManager::IsTradingHalted(SCStudyInterfaceRef sc) const {
    // Check our persistent halt flag
    return sc.GetPersistentInt(RISK_TRADING_HALTED_ID) != 0;
}

int RiskManager::GetConsecutiveLosses(SCStudyInterfaceRef sc) const {
    return sc.GetPersistentInt(RISK_CONSECUTIVE_LOSSES_ID);
}

double RiskManager::GetDailyPnL() const {
    // Return cached value (no broker API call)
    // Cache is updated by RefreshMetrics() once per bar or on trade events
    return m_cache.isValid ? m_cache.dailyPnL : 0.0;
}

double RiskManager::GetUnrealizedPnL() const {
    // Return cached unrealized P&L for open position
    return m_cache.isValid ? m_cache.unrealizedPnL : 0.0;
}

double RiskManager::GetRealizedPnL() const {
    // Return cached realized P&L (closed trades today)
    return m_cache.isValid ? m_cache.realizedPnL : 0.0;
}

double RiskManager::GetAccountEquity(SCStudyInterfaceRef sc) const {
    n_ACSIL::s_TradeAccountDataFields tradeAccountDataFields;
    if (sc.GetTradeAccountData(tradeAccountDataFields, sc.SelectedTradeAccount)) {
        // Prefer account net liquidation / equity when available.
        if (std::isfinite(tradeAccountDataFields.m_AccountValue) &&
            tradeAccountDataFields.m_AccountValue > 0.0) {
            return tradeAccountDataFields.m_AccountValue;
        }

        // Fallback to current cash balance from broker account payload.
        if (std::isfinite(tradeAccountDataFields.m_CurrentCashBalance) &&
            tradeAccountDataFields.m_CurrentCashBalance > 0.0) {
            return tradeAccountDataFields.m_CurrentCashBalance;
        }
    }

    // Final fallback for startup/transient account-data unavailability.
    return sc.TradeServiceAccountBalance;
}

double RiskManager::GetNetTicksToday(SCStudyInterfaceRef sc) const {
    // Returns cached net ticks (updated by RefreshMetrics)
    (void)sc;  // Keep signature for compatibility
    return m_cache.isValid ? m_cache.netTicks : 0.0;
}

double RiskManager::GetTradeQualityScore(SCStudyInterfaceRef sc) const {
    // Returns cached quality score (updated by RefreshMetrics)
    (void)sc;
    if (m_cache.isValid) {
        return m_cache.qualityScore;
    }

    static bool cache_miss_logged = false;
    if (!cache_miss_logged) {
        Logger::getInstance().log(
            "RiskManager::GetTradeQualityScore: cache unavailable, returning strict zero score"
        );
        cache_miss_logged = true;
    }
    return 0.0;
}

// === ELITE GAP 3: MODEL HEALTH SIGNAL STRICTNESS (DOF-Adaptive) ===
float RiskManager::GetRequiredConfidenceThreshold(SCStudyInterfaceRef sc) const {
    // Returns required confidence threshold based on model health + DOF regime.
    // DOF-adaptive: fat-tail regimes push the base thresholds upward.
    // HEALTHY:     DOF-adaptive from 0.60 base (Gaussian → 0.60, DOF=3 → 0.80)
    // WARNING:     DOF-adaptive from 0.75 base (Gaussian → 0.75, DOF=3 → 0.875)
    // SOFT_LOCKED: 1.00 (reject all — not DOF-scaled)

    constexpr float BASE_HEALTHY  = 0.60f;
    constexpr float BASE_WARNING  = 0.75f;
    constexpr float LOCKED_THRESHOLD = 1.00f;

    auto modelHealth = AIConnectionMonitor::Instance().CheckModelHealthStatus(sc);
    const bool transportDegraded = AIConnectionMonitor::Instance().IsTransportDegraded();

    // DOF-adaptive scaling via HmmStateIndicator
    const auto* hmmInd = InferenceManager::Instance().HmmState();

    float base = BASE_HEALTHY;
    switch (modelHealth) {
        case AIConnectionMonitor::ModelHealthStatus::HEALTHY:
            base = transportDegraded ? BASE_WARNING : BASE_HEALTHY;
            break;

        case AIConnectionMonitor::ModelHealthStatus::WARNING:
            base = BASE_WARNING;
            break;

        case AIConnectionMonitor::ModelHealthStatus::SOFT_LOCKED:
            return LOCKED_THRESHOLD;  // Emergency halt — no DOF scaling

        default:
            base = transportDegraded ? BASE_WARNING : BASE_HEALTHY;
            break;
    }

    return hmmInd ? hmmInd->DofConfidenceThreshold(base) : std::min(base + 0.05f, 0.95f);
}

void RiskManager::EmergencyHalt(SCStudyInterfaceRef sc, const char* reason) {
    // THE LOCKOUT - Cancel all working orders FIRST, then disable order submission
    // C2 fix: Cancel before disabling — prevents pending limits from filling after halt
    CancelAllWorkingOrders(sc, reason);

    sc.SendOrdersToTradeService = 0;  // Disable order submission

    SCString msg;
    msg.Format("🛑 EMERGENCY HALT: %s - Order submission disabled", reason);

    Logger::getInstance().log(msg.GetChars());

    // Set our own halt flag for tracking (persistent + atomic)
    sc.SetPersistentInt(RISK_TRADING_HALTED_ID, 1);
    m_tradingHalted.store(true, std::memory_order_release);

    // Cross-wire to SystemOrchestrator so HUD Subgraph 4 lights up
    SystemOrchestrator::Instance().SetEmergencyHalt(true);

    // Play LOUD audible alert (repeat 3 times) - CANNOT BE IGNORED
    for (int i = 0; i < 3; i++) {
        sc.PlaySound(9);  // Alert sound 9 = loud alert
    }
}

void RiskManager::ResetDailyState(SCStudyInterfaceRef sc) {
    // === ELITE GAP 7: DAILY RESET VERIFICATION LOGGING ===
    const int oldConsecutiveLosses = sc.GetPersistentInt(RISK_CONSECUTIVE_LOSSES_ID);
    const int oldHaltFlag = sc.GetPersistentInt(RISK_TRADING_HALTED_ID);
    const int currentDate = sc.CurrentSystemDateTime.GetDate();
    const int lastResetDate = sc.GetPersistentInt(RISK_LAST_RESET_DATE_ID);

    Logger::getInstance().log(SCString().Format(
        "🔄 NEW TRADING DAY: Date transition %d → %d",
        lastResetDate, currentDate).GetChars());

    // Reset consecutive losses at start of new trading day
    sc.SetPersistentInt(RISK_CONSECUTIVE_LOSSES_ID, 0);

    // Reset trading halt flag - allow trading on new day
    sc.SetPersistentInt(RISK_TRADING_HALTED_ID, 0);
    m_tradingHalted.store(false, std::memory_order_release);
    SystemOrchestrator::Instance().SetEmergencyHalt(false);
    sc.SendOrdersToTradeService = 1;  // Re-enable order submission

    // Reset session start balance to current balance for new day
    double currentBalance = GetAccountEquity(sc);
    sc.SetPersistentDouble(RISK_SESSION_START_BALANCE_ID, currentBalance);

    // Reset psychological metrics
    sc.SetPersistentDouble(RISK_NET_TICKS_TODAY_ID, 0.0);

    // Reset daily trade counter (overtrading prevention)
    sc.SetPersistentInt(RISK_TRADES_EXECUTED_TODAY_ID, 0);

    // Clear cooling-off timestamp (start fresh)
    sc.SetPersistentInt64(RISK_LAST_LOSS_TIMESTAMP_ID, 0);

    // Record the reset date to prevent re-triggering on same day
    sc.SetPersistentInt(RISK_LAST_RESET_DATE_ID, currentDate);

    // C16 fix: Re-check monthly halt immediately after clearing daily flags.
    // This prevents a one-bar window where monthly halt is cleared but not yet re-evaluated.
    if (!CheckMonthlyLossLimit(sc)) {
        Logger::getInstance().log("🛑 Monthly loss halt re-engaged after daily reset");
        return;
    }

    // === ELITE GAP 7: VERIFY RESET WORKED ===
    const bool stillHalted = (sc.GetPersistentInt(RISK_TRADING_HALTED_ID) != 0);
    const int newConsecutiveLosses = sc.GetPersistentInt(RISK_CONSECUTIVE_LOSSES_ID);

    if (stillHalted) {
        Logger::getInstance().log("⛔ ERROR: Daily reset FAILED - trading still halted!");
    } else if (newConsecutiveLosses != 0) {
        Logger::getInstance().log("⚠️ WARNING: Consecutive losses not reset properly");
    } else {
        Logger::getInstance().log(SCString().Format(
            "✅ Daily reset SUCCESS | Balance: $%.2f | ConLosses: %d→0 | Halted: %d→0",
            currentBalance, oldConsecutiveLosses, oldHaltFlag).GetChars());
    }
}

// Private methods

void RiskManager::EnforceGoldenRule(SCStudyInterfaceRef sc) {
    // C14 fix: Continuously enforce end-of-day flatten for losing positions.
    // Previously this only ran during ValidateTradingHours() (new-prediction path).
    // Must run every Update() cycle to catch positions held without new predictions.
    static constexpr int GOLDEN_RULE_CUTOFF = 15 * 60 + 45;  // 3:45 PM (945 minutes)

    const SCDateTime now = sc.CurrentSystemDateTime;
    const int currentMinutes = now.GetHour() * 60 + now.GetMinute();

    if (currentMinutes < GOLDEN_RULE_CUTOFF) {
        return;  // Not yet 3:45 PM
    }

    s_SCPositionData position;
    sc.GetTradePosition(position);

    if (position.PositionQuantity == 0) {
        return;  // No position to protect
    }

    const double unrealizedPnL = position.OpenProfitLoss;
    if (unrealizedPnL < 0) {
        PositionManager::Instance().EmergencyFlattenPosition(sc,
            "Golden Rule: Flatten losing position before 4:00 PM");
        Logger::getInstance().log("⚠️ GOLDEN RULE ENFORCED: Flattened losing position before close");
    }
}

bool RiskManager::CheckDailyLossLimit(SCStudyInterfaceRef sc) {
    // Check if already halted - if so, return false immediately without logging
    if (sc.GetPersistentInt(RISK_TRADING_HALTED_ID) != 0) {
        return false; // Already halted, no need to re-trigger
    }

    const double dailyPnL = GetDailyPnL();  // Use cached value (no broker API call)
    const double accountEquity = GetAccountEquity(sc);
    const double maxDailyLoss = accountEquity * m_execParams.elder2PctRuleFrac;

    if (accountEquity <= kMinRiskDenominator) {
        static bool warned_zero_equity = false;
        if (!warned_zero_equity) {
            Logger::getInstance().log(
                "RiskManager::CheckDailyLossLimit: skipping check due to zero/uninitialized account equity"
            );
            warned_zero_equity = true;
        }
        return true;
    }

    if (IsDailyLossLimitBreached(dailyPnL, accountEquity, m_execParams.elder2PctRuleFrac)) [[unlikely]] {
        // Only log when FIRST triggered (halt flag not yet set)
        SCString detailMsg;
        detailMsg.Format("HALT TRIGGER: DailyP&L=$%.2f exceeds MaxLoss=$%.2f (%.2f%% of $%.2f)",
            dailyPnL, maxDailyLoss, (dailyPnL/accountEquity)*100, accountEquity);
        Logger::getInstance().log(detailMsg.GetChars());

        EmergencyHalt(sc, "2% daily loss limit exceeded");
        return false;
    }

    return true;
}

bool RiskManager::CheckDrawdownLimit(SCStudyInterfaceRef sc) {
    const double currentEquity = GetAccountEquity(sc);
    const double peakEquity = sc.GetPersistentDouble(RISK_PEAK_EQUITY_ID);

    if (peakEquity <= 0.0) [[unlikely]] {
        return true; // No peak equity tracked yet
    }

    const double drawdown = (peakEquity - currentEquity) / peakEquity;

    if (drawdown >= m_execParams.drawdownHalt) [[unlikely]] {
        EmergencyHalt(sc, "25% drawdown - trading halted");
        return false;
    }

    return true;
}

bool RiskManager::CheckMonthlyLossLimit(SCStudyInterfaceRef sc) {
    // Elder's 6% Rule: The ultimate career insurance kill-switch
    // Check if already halted - prevent re-triggering
    if (sc.GetPersistentInt(RISK_TRADING_HALTED_ID) != 0) {
        return false;
    }

    const double currentEquity = GetAccountEquity(sc);
    const double monthlyStartEquity = EnsureMonthlyEquityTracking(sc);  // DRY: Single source of truth

    // Check if 6% monthly loss limit exceeded
    const double maxMonthlyLoss = monthlyStartEquity * m_execParams.elder6PctRuleFrac;
    if (currentEquity < (monthlyStartEquity - maxMonthlyLoss)) [[unlikely]] {
        const double monthlyLoss = monthlyStartEquity - currentEquity;
        const double monthlyLossPct = (monthlyLoss / monthlyStartEquity) * 100.0;

        SCString detailMsg;
        detailMsg.Format("ELDER 6%% RULE TRIGGERED: MonthlyLoss=$%.2f (-%.2f%%) exceeds MaxLoss=$%.2f. Current=$%.2f, Start=$%.2f",
            monthlyLoss, monthlyLossPct, maxMonthlyLoss, currentEquity, monthlyStartEquity);
        Logger::getInstance().log(detailMsg.GetChars());

        EmergencyHalt(sc, "ELDER 6% RULE: Monthly drawdown limit reached. See you next month.");
        return false;
    }

    return true;
}

bool RiskManager::Check6PercentRule(SCStudyInterfaceRef sc, double newRisk) {
    const double totalExposure = CalculateTotalExposure(sc);
    const double accountEquity = GetAccountEquity(sc);
    const double maxExposure = accountEquity * m_execParams.elderPortfolioHeatFrac;

    if (totalExposure + newRisk > maxExposure) [[unlikely]] {
        Logger::getInstance().log("6% rule violation - Total exposure: $" +
            std::to_string(totalExposure) + " + New risk: $" +
            std::to_string(newRisk) + " > Limit: $" + std::to_string(maxExposure));
        return false;
    }

    return true;
}

double RiskManager::GetRiskMultiplier(SCStudyInterfaceRef sc) const {
    // Continuous drawdown scaling (Gap 4 closure)
    // Linear ramp: 0% DD → 1.0x, 10% DD → 0.60x, 15% DD → 0.40x, 25% DD → 0.0x (halt)
    // drawdownHalt (25%) remains a HARD STOP — non-negotiable
    const double currentEquity = GetAccountEquity(sc);
    const double peakEquity = sc.GetPersistentDouble(RISK_PEAK_EQUITY_ID);

    if (peakEquity <= 0.0) [[unlikely]] {
        return 1.0;
    }

    const double drawdown = (peakEquity - currentEquity) / peakEquity;

    if (drawdown >= m_execParams.drawdownHalt) [[unlikely]] {
        return 0.0;  // Hard halt — non-negotiable
    }

    if (drawdown <= 0.0) {
        return 1.0;  // No drawdown
    }

    // Linear interpolation from 0% → 1.0x down to drawdownHalt → 0.0x
    return std::max(1.0 - (drawdown / m_execParams.drawdownHalt), 0.0);
}

double RiskManager::CalculateOrderRisk(SCStudyInterfaceRef sc, double entryPrice, double stopPrice, int quantity) const {
    const double stopDistance = fabs(entryPrice - stopPrice);
    const double riskPerContract = (stopDistance / sc.TickSize) * sc.CurrencyValuePerTick;
    return riskPerContract * quantity;
}

double RiskManager::CalculateTotalExposure(SCStudyInterfaceRef sc) const {
    // C17 fix: Calculate actual risk from stop distance, not OpenProfitLoss proxy.

    s_SCPositionData PositionData;
    if (!sc.GetTradePosition(PositionData)) {
        return 0.0;
    }

    if (PositionData.PositionQuantity == 0) {
        return 0.0;
    }

    // Primary: Use actual stop distance from PositionManager's open trade
    const Trade& openTrade = PositionManager::Instance().GetTrade();
    const double stopPrice = openTrade.GetStop();
    const double currentPrice = sc.Close[sc.Index];
    const int quantity = abs(PositionData.PositionQuantity);

    if (stopPrice > 0.0 && currentPrice > 0.0 && sc.TickSize > 0.0) {
        const double stopDistance = fabs(currentPrice - stopPrice);
        return (stopDistance / sc.TickSize) * sc.CurrencyValuePerTick * quantity;
    }

    static bool missing_stop_context_logged = false;
    if (!missing_stop_context_logged) {
        Logger::getInstance().log(
            "RiskManager::CalculateTotalExposure: missing stop/tick context, returning 0.0 (fail-closed)"
        );
        missing_stop_context_logged = true;
    }
    return 0.0;
}


// Top-level validation method
bool RiskManager::IsTradeAllowed(SCStudyInterfaceRef sc, const TradeValidationParams& params, TradeValidationResult& result) {
    // Calculate entry price
    float entry = params.currentPrice;
    switch (params.entryRule) {
        case EntryRuleType::Breakout: entry = params.lastSwingHigh; break;
        case EntryRuleType::Pullback: entry = params.lastSwingLow; break;
        case EntryRuleType::Reversal: entry = params.currentPrice; break;
        default: entry = params.currentPrice; break;
    }
    // Calculate stop price
    float stop = entry;
    switch (params.stopRule) {
        case StopRuleType::ATR: stop = entry - 1.5f * params.atr; break;
        case StopRuleType::SwingPoint: stop = params.lastSwingLow; break;
        case StopRuleType::VolatilityBand: stop = entry - 2.0f * params.atr; break;
        default: stop = entry - params.atr; break;
    }
    // Calculate target price
    float target = entry;
    switch (params.targetRule) {
        case TargetRuleType::RiskReward: target = entry + 2.0f * (entry - stop); break;
        case TargetRuleType::SupportResistance: target = params.lastSwingHigh; break;
        case TargetRuleType::Trailing: target = entry + params.atr; break;
        default: target = entry + params.atr; break;
    }
    // Calculate risk per trade
    float riskPerTrade = entry - stop;
    double accountEquity = GetAccountEquity(sc);
    float orderRisk = CalculateOrderRisk(sc, entry, stop, params.quantity);
    float riskPct = (orderRisk / accountEquity) * 100.0f;
    double totalExposure = CalculateTotalExposure(sc);
    float newTotalExposure = totalExposure + orderRisk;
    float newExposurePct = (newTotalExposure / accountEquity) * 100.0f;
    // Validation logic
    result.entryPrice = entry;
    result.stopPrice = stop;
    result.targetPrice = target;
    result.riskPerTrade = riskPerTrade;
    result.riskPct = riskPct;
    result.newTotalExposure = newTotalExposure;
    result.newExposurePct = newExposurePct;
    result.allowed = true;
    result.reason = "";
    // Check trading halt
    if (IsTradingHalted(sc)) {
        result.allowed = false;
        result.reason = "Trading is halted";
        return false;
    }

    // --- ELITE v3.2: REGIME FILTER via LocalRiskContext ---
    // Prevent entry into toxic environments regardless of setup quality
    const auto& regimeCtx = ContextManager::Instance().GetLocalRiskContext();
    if (regimeCtx.isValid) {
        // 1. TALEB (Fragility): Flash Crash Block
        if (regimeCtx.talebKurtosis > m_execParams.talebKurtosisHaltThreshold) {
            result.allowed = false;
            result.reason = "MARKET FRAGILITY CRITICAL (Taleb Crash Risk)";
            return false;
        }

        // 2. SHANNON (Entropy): Chaos Block
        if (regimeCtx.shannonFlowEntropy > m_execParams.shannonEntropyHaltFrac * kShannonMaxEntropyBits) {
            result.allowed = false;
            result.reason = "MARKET ENTROPY CRITICAL (Shannon Chaos)";
            return false;
        }
    }

    // Daily loss limit (use cached value)
    const double dailyPnL = GetDailyPnL();
    if (IsDailyLossLimitBreached(dailyPnL, accountEquity, m_execParams.elder2PctRuleFrac)) {
        result.allowed = false;
        result.reason = "Daily loss limit reached";
        return false;
    }
    // 2% rule per trade
    double maxRiskPerTrade = accountEquity * m_execParams.maxRiskPerTradeFrac;
    if (orderRisk > maxRiskPerTrade) {
        result.allowed = false;
        result.reason = "Exceeds 2% per-trade risk limit";
        return false;
    }
    // 6% portfolio heat rule
    if (!Check6PercentRule(sc, orderRisk)) {
        result.allowed = false;
        result.reason = "Exceeds 6% portfolio heat limit";
        return false;
    }
    // All checks passed
    result.allowed = true;
    result.reason = "Trade allowed";
    return true;
}

// ============================================================================
// LAYER 4: REACTIVE ORDER MONITORING (External Order Protection)
// ============================================================================

void RiskManager::CancelAllWorkingOrders(SCStudyInterfaceRef sc, const char* reason) {
    // Cancel all pending orders (protection against external order placement)
    int cancelCount = 0;
    s_SCTradeOrder order;

    for (int i = 0; i < 1000; i++) {
        if (sc.GetOrderByIndex(i, order) != SCTRADING_ORDER_ERROR) {
            if (order.OrderStatusCode == SCT_OSC_OPEN) {
                // Working order found - cancel it
                sc.CancelOrder(order.InternalOrderID);
                cancelCount++;
            }
        } else {
            break;  // No more orders
        }
    }

    if (cancelCount > 0) {
        SCString msg;
        msg.Format("CANCELLED %d WORKING ORDERS: %s", cancelCount, reason);
        Logger::getInstance().log(msg.GetChars());

        // Play alert 3x
        for (int i = 0; i < 3; i++) {
            sc.PlaySound(9);
        }
    }
}

void RiskManager::FlattenPositionIfOpened(SCStudyInterfaceRef sc, const char* reason) {
    s_SCPositionData position;
    if (sc.GetTradePosition(position) && position.PositionQuantity != 0) {
        // Check if this is a NEW position (opened during violation)
        // Track last known position to detect changes
        int lastKnownQty = sc.GetPersistentInt(RISK_LAST_POSITION_QTY_ID);

        if (position.PositionQuantity != lastKnownQty) {
            // Position changed - trader opened new position during halt
            s_SCNewOrder flattenOrder;
            flattenOrder.OrderQuantity = abs(position.PositionQuantity);
            flattenOrder.OrderType = SCT_ORDERTYPE_MARKET;

            if (position.PositionQuantity > 0) {
                sc.SellExit(flattenOrder);  // Close long
            } else {
                sc.BuyExit(flattenOrder);   // Close short
            }

            SCString msg;
            msg.Format("EMERGENCY FLATTEN: %s - Position opened during lockout!", reason);
            Logger::getInstance().log(msg.GetChars());

            // LOUD ALARM - 5 repetitions
            for (int i = 0; i < 5; i++) {
                sc.PlaySound(9);
            }
        }

        // Update tracked position
        sc.SetPersistentInt(RISK_LAST_POSITION_QTY_ID, position.PositionQuantity);
    } else {
        // No position - reset tracking
        sc.SetPersistentInt(RISK_LAST_POSITION_QTY_ID, 0);
    }
}

void RiskManager::MonitorOrderViolations(SCStudyInterfaceRef sc) {
    // Check all violation conditions and enforce continuously
    bool isHalted = (sc.GetPersistentInt(RISK_TRADING_HALTED_ID) != 0);

    if (isHalted) {
        // Trading halted - cancel any working orders and flatten new positions
        CancelAllWorkingOrders(sc, "Trading halted - order monitoring");
        FlattenPositionIfOpened(sc, "Trading halted - no new positions allowed");
    }

    // Additional checks for cooling-off even if not halted
    int64_t lastLossTime = sc.GetPersistentInt64(RISK_LAST_LOSS_TIMESTAMP_ID);
    if (lastLossTime > 0) {
        SCDateTime lastLoss;
        lastLoss = lastLossTime;
        SCDateTime now = sc.CurrentSystemDateTime;
        int minutesSinceLoss = (now - lastLoss).GetTimeInSeconds() / 60;

        if (minutesSinceLoss < m_execParams.elderRevengeCooloffMin) {
            // Cooling-off active but not halted (1 loss only)
            CancelAllWorkingOrders(sc, "Cooling-off period active");
            FlattenPositionIfOpened(sc, "Cooling-off period violation");
        }
    }
}

void RiskManager::MonitorStopOrderModifications(SCStudyInterfaceRef sc) {
    // SCENARIO 8: Stop Loss Manipulation Monitoring (CRITICAL)
    s_SCTradeOrder stopOrder;

    // Get current stop orders
    if (sc.GetNearestStopOrder(stopOrder) == 0) {
        // Stop order exists
        int64_t originalStopPrice = sc.GetPersistentInt64(RISK_ORIGINAL_STOP_PRICE_ID);

        if (originalStopPrice == 0) {
            // First time seeing this stop - record it as original
            int64_t stopPriceInt = (int64_t)(stopOrder.Price1 * 100);  // Store as int (cents)
            sc.SetPersistentInt64(RISK_ORIGINAL_STOP_PRICE_ID, stopPriceInt);
            Logger::getInstance().log("Original stop recorded: " + std::to_string(stopOrder.Price1));
        } else {
            // Stop exists - check if it was modified
            double originalStop = (double)originalStopPrice / 100.0;
            double currentStop = stopOrder.Price1;

            // Get position direction
            s_SCPositionData position;
            sc.GetTradePosition(position);

            bool stopMovedWorse = false;

            if (position.PositionQuantity > 0) {
                // Long position - stop should be below entry
                // Moving stop DOWN (further away) = WORSE
                if (currentStop < originalStop - sc.TickSize) {  // C19: instrument-aware tolerance
                    stopMovedWorse = true;
                }
            } else if (position.PositionQuantity < 0) {
                // Short position - stop should be above entry
                // Moving stop UP (further away) = WORSE
                if (currentStop > originalStop + sc.TickSize) {
                    stopMovedWorse = true;
                }
            }

            if (stopMovedWorse) {
                // CRITICAL VIOLATION - Stop moved to increase risk
                SCString msg;
                msg.Format("🚨🚨🚨 STOP VIOLATION: Stop moved from %.2f to %.2f (INCREASING RISK)",
                           originalStop, currentStop);
                Logger::getInstance().log(msg.GetChars());

                // Cancel the modified stop order
                sc.CancelOrder(stopOrder.InternalOrderID);

                // Replace with original stop
                s_SCNewOrder newStopOrder;
                newStopOrder.OrderType = SCT_ORDERTYPE_STOP;
                newStopOrder.Price1 = originalStop;
                newStopOrder.OrderQuantity = abs(position.PositionQuantity);

                if (position.PositionQuantity > 0) {
                    sc.SellExit(newStopOrder);  // Restore long stop
                } else {
                    sc.BuyExit(newStopOrder);   // Restore short stop
                }

                msg.Format("Stop restored to original: %.2f - STOP MANIPULATION BLOCKED", originalStop);
                Logger::getInstance().log(msg.GetChars());

                // LOUD ALARM - 5 repetitions (critical psychological violation)
                for (int i = 0; i < 5; i++) {
                    sc.PlaySound(9);
                }

                // After 2 violations, flatten position entirely
                int violationCount = sc.GetPersistentInt(RISK_STOP_VIOLATION_COUNT_ID);
                violationCount++;
                sc.SetPersistentInt(RISK_STOP_VIOLATION_COUNT_ID, violationCount);

                if (violationCount >= 2) {
                    msg = "2ND STOP VIOLATION - POSITION FLATTENED FOR YOUR PROTECTION";
                    Logger::getInstance().log(msg.GetChars());

                    // Flatten position immediately
                    s_SCNewOrder flattenOrder;
                    flattenOrder.OrderQuantity = abs(position.PositionQuantity);
                    flattenOrder.OrderType = SCT_ORDERTYPE_MARKET;

                    if (position.PositionQuantity > 0) {
                        sc.SellExit(flattenOrder);
                    } else {
                        sc.BuyExit(flattenOrder);
                    }

                    EmergencyHalt(sc, "Repeated stop manipulation - protecting capital");
                }
            }

            // Allow stops to move CLOSER (trailing) - update original
            if (!stopMovedWorse) {
                if (position.PositionQuantity > 0 && currentStop > originalStop + sc.TickSize) {
                    // Long stop moved UP (closer/better) - allowed, update original
                    int64_t stopPriceInt = (int64_t)(currentStop * 100);
                    sc.SetPersistentInt64(RISK_ORIGINAL_STOP_PRICE_ID, stopPriceInt);
                } else if (position.PositionQuantity < 0 && currentStop < originalStop - sc.TickSize) {
                    // Short stop moved DOWN (closer/better) - allowed, update original
                    int64_t stopPriceInt = (int64_t)(currentStop * 100);
                    sc.SetPersistentInt64(RISK_ORIGINAL_STOP_PRICE_ID, stopPriceInt);
                }
            }
        }
    } else {
        // No stop order - clear tracking
        sc.SetPersistentInt64(RISK_ORIGINAL_STOP_PRICE_ID, 0);
        sc.SetPersistentInt(RISK_STOP_VIOLATION_COUNT_ID, 0);
    }
}

// =====================================================================
// Pattern Probation System Implementation
// =====================================================================

void RiskManager::UpdatePatternStats(const std::string& patternEnum, double pnl, bool isWin) {
    // Get or create pattern stats
    PatternStats& stats = m_patternStats[patternEnum];

    if (stats.patternName.empty()) {
        stats.patternName = patternEnum;
    }

    // Update statistics
    stats.tradeCount++;
    stats.totalPnL += pnl;

    if (isWin) {
        stats.wins++;

        // If on probation and this is a win, increment streak
        if (stats.onProbation) {
            stats.probationStreak++;

            // Exit probation after 10 consecutive good trades
            if (stats.probationStreak >= PROBATION_RESET_STREAK) {
                stats.onProbation = false;
                stats.probationStreak = 0;

                Logger::getInstance().log("Pattern " + patternEnum +
                    " REMOVED from probation after " + std::to_string(PROBATION_RESET_STREAK) +
                    " good trades");
            }
        }
    } else {
        stats.losses++;

        // Reset probation streak on loss
        if (stats.onProbation) {
            stats.probationStreak = 0;
        }
    }

    // Calculate expectancy (average P&L per trade)
    stats.expectancy = stats.totalPnL / stats.tradeCount;

    // Check if pattern should go on probation
    if (stats.tradeCount >= MIN_TRADES_FOR_PROBATION && !stats.onProbation) {
        if (stats.expectancy < 0.0) {
            // Negative expectancy → probation
            stats.onProbation = true;
            stats.probationStreak = 0;

            std::ostringstream oss;
            oss << std::fixed << std::setprecision(2)
                << "⚠️ Pattern " << patternEnum << " placed on PROBATION | "
                << "Trades: " << stats.tradeCount << " | "
                << "Wins: " << stats.wins << " | "
                << "Losses: " << stats.losses << " | "
                << "Expectancy: $" << stats.expectancy << " | "
                << "Quality threshold raised to " << m_execParams.probationQualityFloor;
            Logger::getInstance().log(oss.str());
        }
    }

    // Log pattern stats update
    std::ostringstream oss;
    oss << std::fixed << std::setprecision(2)
        << "Pattern Stats: " << patternEnum << " | "
        << "Trades: " << stats.tradeCount << " | "
        << "W/L: " << stats.wins << "/" << stats.losses << " | "
        << "Expectancy: $" << stats.expectancy
        << (stats.onProbation ? " | ON PROBATION" : "");
    Logger::getInstance().log(oss.str());
}

bool RiskManager::IsPatternOnProbation(const std::string& patternEnum) const {
    auto it = m_patternStats.find(patternEnum);
    if (it != m_patternStats.end()) {
        return it->second.onProbation;
    }
    return false;  // Unknown pattern, not on probation
}

float RiskManager::GetMinQualityForPattern(const std::string& patternEnum) const {
    if (IsPatternOnProbation(patternEnum)) {
        return m_execParams.probationQualityFloor;  // 0.8 for underperformers
    }
    return m_execParams.minPatternQualityBase;  // 0.6 for normal patterns
}

void RiskManager::SavePatternStatsToFile() const {
    if (m_patternStats.empty()) {
        return;  // Nothing to save
    }

    // Create filename with symbol (e.g., PatternStats_ESH25.csv)
    std::string filename = "PatternStats_" + m_invariants.symbol + ".csv";
    std::ofstream file(filename);
    if (!file.is_open()) {
        Logger::getInstance().log("ERROR: Cannot save pattern stats to " + filename);
        return;
    }

    // CSV header
    file << "Pattern,Trades,Wins,Losses,TotalPnL,Expectancy,OnProbation,ProbationStreak\n";

    // Write each pattern's stats
    for (const auto& [pattern, stats] : m_patternStats) {
        file << pattern << ","
             << stats.tradeCount << ","
             << stats.wins << ","
             << stats.losses << ","
             << std::fixed << std::setprecision(2) << stats.totalPnL << ","
             << stats.expectancy << ","
             << (stats.onProbation ? 1 : 0) << ","
             << stats.probationStreak << "\n";
    }

    file.close();
    Logger::getInstance().log("Pattern stats saved: " + std::to_string(m_patternStats.size()) + " patterns to " + filename);
}

void RiskManager::LoadPatternStatsFromFile() {
    // Create filename with symbol (e.g., PatternStats_ESH25.csv)
    std::string filename = "PatternStats_" + m_invariants.symbol + ".csv";
    std::ifstream file(filename);

    if (!file.is_open()) {
        Logger::getInstance().log("Pattern stats file not found (first run or no history): " + filename);
        return;  // Not an error - first run or symbol change
    }

    std::string line;
    std::getline(file, line);  // Skip header

    int loadedCount = 0;
    while (std::getline(file, line)) {
        if (line.empty()) continue;

        std::stringstream ss(line);
        std::string pattern;
        PatternStats stats;

        // Parse CSV: Pattern,Trades,Wins,Losses,TotalPnL,Expectancy,OnProbation,ProbationStreak
        std::getline(ss, pattern, ',');
        ss >> stats.tradeCount; ss.ignore();
        ss >> stats.wins; ss.ignore();
        ss >> stats.losses; ss.ignore();
        ss >> stats.totalPnL; ss.ignore();
        ss >> stats.expectancy; ss.ignore();

        int probation;
        ss >> probation; ss.ignore();
        ss >> stats.probationStreak;

        stats.patternName = pattern;
        stats.onProbation = (probation == 1);

        m_patternStats[pattern] = stats;
        loadedCount++;
    }

    file.close();
    Logger::getInstance().log("Pattern stats loaded: " + std::to_string(loadedCount) + " patterns from " + filename);
}

// 1622
