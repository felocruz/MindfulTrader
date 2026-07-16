#pragma once

#include "sierrachart.h"
#include "ExecutionParams.h"
#include "KellyCalculator.h"
#include "RejectionLedger.h"
#include "TradeDecisionEngine.h"
#include "Result.h"
#include <atomic>
#include <cstddef>
#include <memory>

/**
 * @brief Singleton class responsible for all risk management decisions
 *
 * Implements Linda Raschke's risk management principles:
 * - 2% daily loss limit (hard lockout via sc.SetTradingIsDisabledForDay)
 * - 2% risk per trade (position sizing validation)
 * - 6% total portfolio exposure limit
 * - 2 consecutive losses → trading halt (prevents revenge trading)
 * - Drawdown-based position sizing reduction
 *
 * Integration points:
 * - Called by scsf_MindfulTrader orchestrator for continuous monitoring
 * - Called by PositionManager::OpenPosition() for pre-trade validation
 * - Updated by PositionManager::ClosePosition() to track consecutive losses
 */

// Helper enums for trade validation (shared with dialog, GUI, and RiskManager)
enum class TradeSetupType {
    ElderTripleScreen = 0,
    RaschkeTurtleSoup = 1,
    MomentumPinball = 2,
    Custom = 3
};
enum class EntryRuleType {
    Breakout = 0,
    Pullback = 1,
    Reversal = 2
};
enum class StopRuleType {
    ATR = 0,
    SwingPoint = 1,
    VolatilityBand = 2
};
enum class TargetRuleType {
    RiskReward = 0,
    SupportResistance = 1,
    Trailing = 2
};

// Trade validation input (from dialog or Python GUI)
struct TradeValidationParams {
    TradeSetupType setupType;
    EntryRuleType entryRule;
    StopRuleType stopRule;
    TargetRuleType targetRule;
    float riskBudgetPercent;
    float atr;
    float lastSwingHigh;
    float lastSwingLow;
    float currentPrice;
    int quantity;
    float modelConfidence; // ELITE: Model confidence (0.0-1.0)
};

// Trade validation result (output)
struct TradeValidationResult {
    bool allowed;
    float entryPrice;
    float stopPrice;
    float targetPrice;
    float riskPerTrade;
    float riskPct;
    float newTotalExposure;
    float newExposurePct;
    const char* reason;
};

struct LatencyGateResult {
    bool hardReject = false;
    bool degraded = false;
    float riskFactor = 1.0f;
    const char* reason = "OK";
};

/**
 * @brief Per-pattern performance tracking for adaptive quality thresholds
 *
 * Tracks wins, losses, expectancy for each pattern type.
 * After 20 trades, if expectancy < 0, pattern goes on "probation"
 * requiring quality score > 0.8 instead of base 0.6.
 */
struct PatternStats {
    std::string patternName;     // Pattern enum string (e.g., "HOLY_GRAIL")
    int tradeCount = 0;          // Total trades executed
    int wins = 0;                // Winning trades
    int losses = 0;              // Losing trades
    double totalPnL = 0.0;       // Cumulative P&L (R-multiples or dollars)
    double expectancy = 0.0;     // Average R per trade
    bool onProbation = false;    // True if underperforming
    int probationStreak = 0;     // Count of good trades since probation started
};

/**
 * @brief Singleton class responsible for all risk management decisions
 *
 * Implements Linda Raschke's risk management principles:
 * - 2% daily loss limit (hard lockout via sc.SetTradingIsDisabledForDay)
 * - 2% risk per trade (position sizing validation)
 * - 6% total portfolio exposure limit
 * - 2 consecutive losses → trading halt (prevents revenge trading)
 * - Drawdown-based position sizing reduction
 *
 * Integration points:
 * - Called by scsf_MindfulTrader orchestrator for continuous monitoring
 * - Called by PositionManager::OpenPosition() for pre-trade validation
 * - Updated by PositionManager::ClosePosition() to track consecutive losses
 */
class RiskManager {
public:
    static RiskManager& Instance();

    // Initialization and updates
    void Init(SCStudyInterfaceRef sc);
    void Update(SCStudyInterfaceRef sc);  // Call every bar for continuous monitoring

    // Pre-trade validation (call before submitting orders) - Elite Refactor #7: Result<T> error handling
    Result<void> ValidateOrder(SCStudyInterfaceRef sc, double entryPrice, double stopPrice, int& quantity,
                               long inferenceLatencyUs = -1,
                               long transformerLatencyUs = -1,
                               long regimeLatencyUs = -1);
    Result<int> CalculateSafePositionSize(SCStudyInterfaceRef sc, double entryPrice, double stopPrice, float modelConfidence = 0.5f);

    // Manual trade decision support - logs validation results for manual trading
    void LogTradeValidation(SCStudyInterfaceRef sc, double entryPrice, double stopPrice, int quantity, const char* tradeDescription);

    // === ELITE GAP 3: MODEL HEALTH SIGNAL STRICTNESS ===
    // Returns required confidence threshold based on current model health
    // HEALTHY: 60% (base), WARNING: 75% (stricter), SOFT_LOCKED: reject all
    float GetRequiredConfidenceThreshold(SCStudyInterfaceRef sc) const;

    // Post-trade updates (call when position opens/closes)
    void OnPositionOpened(SCStudyInterfaceRef sc);
    void OnTradeClose(SCStudyInterfaceRef sc, double pnl);

    // Cache refresh (call once per bar if position is open, otherwise no-op)
    void RefreshMetrics(SCStudyInterfaceRef sc);

    // State queries (lightweight - return cached values, no broker API calls)
    bool IsTradingHalted() const;
    bool IsTradingHalted(SCStudyInterfaceRef sc) const;  // Version with sc parameter
    int GetConsecutiveLosses(SCStudyInterfaceRef sc) const;
    double GetDailyPnL() const;                          // Returns cached daily P&L
    double GetUnrealizedPnL() const;                     // Returns cached unrealized P&L
    double GetRealizedPnL() const;                       // Returns cached realized P&L
    double GetAccountEquity(SCStudyInterfaceRef sc) const;  // IB account data (fallback to SC balance)
    double CalculateTotalExposure(SCStudyInterfaceRef sc) const;

    // Psychological metrics (for dashboard - hides dollar amounts)
    double GetNetTicksToday(SCStudyInterfaceRef sc) const;
    double GetTradeQualityScore(SCStudyInterfaceRef sc) const;

    // Emergency controls
    void EmergencyHalt(SCStudyInterfaceRef sc, const char* reason);
    void ResetDailyState(SCStudyInterfaceRef sc);  // Call at start of trading day

    // === ELITE v3.2: UNIFIED HARD GATE LAYER ===
    // Physics-based hard gates evaluated from LocalRiskContext.
    // Returns failure reason if ANY gate trips; success if all pass.
    // Three callers: Path 1 (bar update), Path 2 (HMM), Path 3 (Transformer).
    Result<void> EvaluateHardGates(const LocalRiskContext& ctx) const;

    // Layer 4: Reactive order monitoring (external order protection)
    void CancelAllWorkingOrders(SCStudyInterfaceRef sc, const char* reason);
    void FlattenPositionIfOpened(SCStudyInterfaceRef sc, const char* reason);
    void MonitorOrderViolations(SCStudyInterfaceRef sc);
    void MonitorStopOrderModifications(SCStudyInterfaceRef sc);

    // Top-level validation method
    bool IsTradeAllowed(SCStudyInterfaceRef sc, const TradeValidationParams& params, TradeValidationResult& result);

    // Market data setters from TripleScreen1 (for regime-adaptive position sizing)
    void SetATR14(float value) { m_atr14 = value; }
    void SetATR14Avg(float value) { m_atr14Avg = value; }

    // Institutional HMM sizing (source of truth for regime-based multiplier)
    float ComputeInstitutionalRiskMultiplier(int primaryState,
                                             const float* probabilities,
                                             size_t probabilityCount,
                                             float entropy,
                                             float transitionRisk,
                                             float dof,
                                             bool unknownPosture = false);
    int GetOrderAckTimeoutMs() const;
    int GetWorkingOrderTtlMs() const;
    uint64_t GetConnectivityFailClosedHoldUs() const;
    bool ShouldInjectScDisconnect() const;
    bool ShouldInjectIbDisconnect() const;
    bool ShouldInjectOrderAckTimeout() const;
    float GetTransitionRiskDefensiveThreshold() const;
    float GetTransitionRiskCriticalThreshold() const;
    float GetLiveToxicScoreThreshold() const;
    float GetLiveHostileScoreThreshold() const;
    float GetLiveForceFlatConfidenceCap() const;
    float GetTransitionDefensiveDistanceAtrMultiplier() const;
    float GetTransitionDefensiveForceTightenAtrProfile() const;
    float GetParetoTopStateRatioMax() const;
    float GetShannonMinTenureBars() const;
    float GetTalebSignalSigmaThreshold() const;

private:
    RiskManager() = default;
    ~RiskManager() = default;
    RiskManager(const RiskManager&) = delete;
    RiskManager& operator=(const RiskManager&) = delete;

    // Cached invariant values (captured once in Init(), never change during session)
    struct SessionInvariants {
        // Contract specifications (truly invariant)
        double tickSize = 0.0;
        double currencyPerTick = 0.0;
        std::string symbol;

        // Session start values (invariant until next trading day)
        double sessionStartBalance = 0.0;
        double maxDailyLoss = 0.0;        // 2% of session start
        double maxPortfolioHeat = 0.0;    // 6% of session start
        double maxDailyWin = 0.0;         // 5% of session start

        int sessionStartDate = 0;
        bool initialized = false;
    } m_invariants;

    // Cached P&L metrics (updated on events: position open/close, or RefreshMetrics())
    struct CachedMetrics {
        double dailyPnL = 0.0;           // Realized + Unrealized P&L today
        double unrealizedPnL = 0.0;      // Open position P&L
        double realizedPnL = 0.0;        // Closed trades P&L today
        double netTicks = 0.0;           // Cumulative net ticks today
        double qualityScore = 0.0;       // 0-100 trade quality score
        int lastUpdateBar = -1;          // Bar index of last update
        bool hasOpenPosition = false;    // True if position is open
        bool isValid = false;            // True if cache has been initialized
    } m_cache;

    // P-AER runtime-configurable execution parameters
    ExecutionParams m_execParams;

    // TDE shadow-mode state (PAER §9 Phase 1) — populated in ValidateOrder (terminal decision point)
    TradingClearance m_shadowClearance;
    ConvictionResult m_shadowConviction;
    RiskPriceResult  m_shadowRiskPrice;
    bool             m_shadowTdeReady = false;

    // Pattern probation system (adaptive quality thresholds)
    // Elite Refactor #5: O(1) hash-based lookups (avg case) instead of O(log n) tree
    std::unordered_map<std::string, PatternStats> m_patternStats;
    static constexpr int MIN_TRADES_FOR_PROBATION = 20;  // Require statistical sample
    static constexpr int PROBATION_RESET_STREAK = 10;    // Good trades to exit probation

    // ── Structural time constants (compiled, not tunable) ──
    static constexpr int TRADING_START_HOUR = 9;                     // 9:30 AM ET - avoid overnight gap chaos
    static constexpr int TRADING_START_MIN = 30;
    static constexpr int END_OF_DAY_CUTOFF_HOUR = 15;               // 3:30 PM ET (15:30)
    static constexpr int END_OF_DAY_CUTOFF_MIN = 30;
    // Pre-calculated minute values for efficient comparisons (Elite refactor #2)
    static constexpr int TRADING_START_MINUTES = TRADING_START_HOUR * 60 + TRADING_START_MIN;      // 570 minutes (9:30 AM)
    static constexpr int END_OF_DAY_CUTOFF_MINUTES = END_OF_DAY_CUTOFF_HOUR * 60 + END_OF_DAY_CUTOFF_MIN;  // 930 minutes (3:30 PM)

    // All 25 P-AER risk parameters now live in ExecutionParams (m_execParams),
    // loaded at runtime from C:/Trading/config/execution_params.json.
    // Phase 3 complete — constexpr originals removed 2026-03-31.

    // Validator methods (Chain of Responsibility + Result<T> - Elite Refactor #7)
    Result<void> ValidateMonthlyLimit(SCStudyInterfaceRef sc);
    Result<void> ValidateTradingHalt(SCStudyInterfaceRef sc);
    Result<void> ValidateTradingHours(SCStudyInterfaceRef sc);  // ⏰ Time-of-day windows + Golden Rule
    Result<void> ValidateCoolingPeriod(SCStudyInterfaceRef sc);
    Result<void> ValidateRapidFire(SCStudyInterfaceRef sc);
    Result<void> ValidateKurtosisEmergencyGate(SCStudyInterfaceRef sc);  // 🚨 Trend-only gate in dislocation
    Result<void> ValidatePatternQuality(SCStudyInterfaceRef sc);  // 🎯 Time-adjusted quality thresholds
    Result<void> ValidateCrossMarketCorrelation(SCStudyInterfaceRef sc);  // 🌐 ZN/DX divergence detection
    Result<void> ValidateRiskLimits(SCStudyInterfaceRef sc, double entryPrice, double stopPrice, int quantity);
    LatencyGateResult EvaluateLatencyGate(SCStudyInterfaceRef sc,
                                          long inferenceLatencyUs,
                                          long transformerLatencyUs,
                                          long regimeLatencyUs) const;

    // Helper methods (DRY refactoring)
    double EnsureMonthlyEquityTracking(SCStudyInterfaceRef sc);  // Returns monthly start equity (handles reset)

    // Internal validation methods
    bool CheckDailyLossLimit(SCStudyInterfaceRef sc);
    bool CheckMonthlyLossLimit(SCStudyInterfaceRef sc);  // Elder's 6% monthly kill-switch
    bool CheckDrawdownLimit(SCStudyInterfaceRef sc);
    void EnforceGoldenRule(SCStudyInterfaceRef sc);       // C14: End-of-day flatten losing positions
    bool Check6PercentRule(SCStudyInterfaceRef sc, double newRisk);
    double GetRiskMultiplier(SCStudyInterfaceRef sc) const;
    double GetAtrVolatilityMultiplier() const;  // ATR-based volatility adjustment (Elite feature)
    double CalculateOrderRisk(SCStudyInterfaceRef sc, double entryPrice, double stopPrice, int quantity) const;
    void RefreshKurtosisEmergencyState(SCStudyInterfaceRef sc);
    bool IsTrendContinuationTrigger(RaschkeTacticalTrigger trigger) const;

    // Pattern probation helpers
    void UpdatePatternStats(const std::string& patternEnum, double pnl, bool isWin);
    bool IsPatternOnProbation(const std::string& patternEnum) const;
    float GetMinQualityForPattern(const std::string& patternEnum) const;
    void SavePatternStatsToFile() const;
    void LoadPatternStatsFromFile();

private:
    // Trade statistics tracker (trade history for diagnostics / pattern probation)
    KellyCalculator m_kellyCalculator;

    // Market metrics for adaptive position sizing
    float m_atr14 = 0.0f;
    float m_atr14Avg = 0.0f;
    std::atomic<bool> m_tradingHalted{false};  // C1 fix: cross-thread halt flag (mirrored to SystemOrchestrator HUD)
    std::atomic<bool> m_kurtosisEmergencyActive{false};
    std::atomic<float> m_lastObservedKurtosis{3.0f};
};
