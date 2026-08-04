#include "MindfulTrader_Precompiled.h"
#include "generated/backtest_schema_generated.h"

#include <algorithm>
#include <chrono>
#include <deque>
#include <filesystem>
#include <iomanip>
#include <mutex>
#include <random>
#include <sstream>

// ============================================================================
// BackTesterStudy.cpp - EVENT-DRIVEN Backtesting Framework
// Three-phase backtesting framework for Transformer model validation
// ============================================================================
//
// PURPOSE:
// Validate the complete trading system (Transformer → RiskManager → Execution)
// in a production-identical environment using Sierra Chart's backtesting engine.
// Collect comprehensive trade data to improve model calibration and risk settings.
//
// EVENT-DRIVEN ARCHITECTURE:
// - No longer processes on bar close - reacts to indicator-change events only
// - Same as live trading: publishes FlatBuffer events via PublishEventOnChange()
// - Trades entered/exited in real-time as indicators update (sub-bar latency)
// - Timestamp precision: microseconds (same as live EventDataCollectorStudy)
//
// ARCHITECTURE:
// - Phase 1: Data Export (use scsf_EventDataCollector instead - not implemented here)
// - Phase 2: Pure Neural Network - Baseline model accuracy without risk filters
// - Phase 3: Risk-Managed - Full system with RiskManager validation layer
//
// DATA FLOW (Phase 2 & 3):
// 1. Update indicators via IndicatorManager
// 2. Check for significant indicator changes (HasSignificantChange)
// 3. If changed: Publish FlatBuffer event to Python via PublishEventOnChange()
// 4. Python Transformer generates predictions, sends TradeRequests
// 5. PositionManager processes requests:
//    - Phase 2: Direct order submission
//    - Phase 3: RiskManager validation first, then order submission
// 6. Sierra Chart executes trades via backtesting engine
// 7. BackTester monitors position state and records entry/exit data
//
// OUTPUT FILES:
// - backtest_run.btst: FlatBuffer binary (all frames: manifest, trades, decisions, acks, summary)
// - manifest.json: Human-readable run metadata (written at start and finalized on shutdown)
//
// INTEGRATION POINTS:
// - IndicatorManager: Provides 50+ technical indicators + PublishEventOnChange()
// - PositionManager: Handles order execution and position tracking
// - RiskManager: Validates trades (Phase 3 only)
// - TransportStream: ZMQ communication with Python (Elite v2.4 - centralized)
// - Trade class: Tracks MAE/MFE, P&L, and pattern context
//
// ELITE PROTOCOL INTEGRATION:
// - Pre-Flight Check: Validates Python readiness before starting backtest
// - PREDICTION_ACK: Tracks model acceptance/rejection rates via callback
// - POSITION_SYNC: Ensures state consistency (tested in Task 1.4)
// - Event-Driven: Zero-latency position updates (not bar-based)
//
// ERROR HANDLING:
// - All I/O operations wrapped in try-catch blocks
// - Graceful degradation: Failures don't halt backtest
// - Comprehensive logging for diagnostics
// ============================================================================

namespace {
    // BackTester persistent IDs
    constexpr int BT_PHASE_ID = 50;
    constexpr int BT_MENU_EXISTS_ID = 51;
    constexpr int BT_FILE_OPENED_FLAG_ID = 52;
    constexpr int BT_TRADE_COUNT_ID = 53;
    constexpr int BT_CURRENT_TRADE_ID = 54;
    constexpr int BT_DISCONNECT_START_BAR_ID = 55;
    constexpr int BT_IS_DISCONNECTED_ID = 56;
    constexpr int BT_EMERGENCY_FLATTEN_TRIGGERED_ID = 57;
    constexpr int BT_HMM_CLIENT_INITIALIZED_ID = 58;
    constexpr int BT_STRUCTURE_LAST_BAR_ID = 59;

    // Input indices
    enum InputIndex : int {
        IN_PHASE = 0,
        IN_DEBUG = 1,
        IN_ENABLE_WALK_FORWARD = 2,
        IN_TRAINING_WINDOW_BARS = 3,
        IN_TEST_WINDOW_BARS = 4,
        IN_STEP_SIZE_BARS = 5,
        // Failure testing inputs
        IN_EMERGENCY_FLATTEN_BAR = 6,
        IN_SIMULATE_DISCONNECT_BAR = 7,
        IN_DISCONNECT_DURATION_BARS = 8,
        // Lineage and artifact inputs (fill before each run)
        IN_GIT_SHA_MINDFULTRADER = 9,
        IN_GIT_SHA_LBRNET = 10,
        IN_TRANSFORMER_ARTIFACT_PATH = 11,
        IN_MODEL_ARTIFACT_PATH = 12,
        IN_RISK_PARAM_PROFILE = 13,
        IN_EXECUTION_PARAM_PROFILE = 14,
        // Replay isolation
        IN_ISOLATION_MODE = 15,
        IN_PRE_RUN_CLEAR_APPLIED = 16,
        // Timeframe stack chart numbers (BackTesterStudy runs on TS3)
        IN_TS1_CHART_NUMBER = 17,
        IN_TS2_CHART_NUMBER = 18,
        // Output path (override if SC is not installed to the default location)
        IN_OUTPUT_BASE_DIR = 19
    };

    // Subgraph indices
    enum SubgraphIndex : int {
        SG_NN_SIGNAL = 0,
        SG_ENTRY_PRICE = 1,
        SG_STOP_PRICE = 2,
        SG_TARGET_PRICE = 3,
        SG_TRADE_GRADE = 4,
        SG_PNL_TICKS = 5,
        SG_PNL_DOLLARS = 6
    };

    uint64_t GetReplaySafeNowUs(SCStudyInterfaceRef sc)
    {
        return sc.GetCurrentDateTime().ToUNIXTimeInMicroseconds();
    }
}

static std::mutex g_predictionAckBufferMutex;
static std::deque<PositionManager::PredictionAckEvent> g_predictionAckBuffer;
static std::ofstream g_backtestBinaryStream;
static bool g_binaryStreamFailed = false;
static constexpr float kNaN = std::numeric_limits<float>::quiet_NaN();

struct BacktestRunContext {
    std::string runId;
    std::string runFolder;
    std::string simAccount;
    int phase{0};
    bool dataRangeStartCaptured{false};
    uint64_t startedAtUsUtc{0};
    uint64_t completedAtUsUtc{0};
    uint64_t dataRangeStartUsUtc{0};
    uint64_t dataRangeEndUsUtc{0};
    uint64_t tradeSeq{0};
    uint64_t ackSeq{0};
    uint64_t decisionSeq{0};
    uint64_t totalPredictions{0};
    uint64_t acceptedPredictions{0};
    uint64_t rejectedPredictions{0};
    uint64_t winningTrades{0};
    double totalPnlDollars{0.0};
    double grossProfitDollars{0.0};
    double grossLossDollarsAbs{0.0};
    double cumulativeEquity{0.0};
    double peakEquity{0.0};
    double maxDrawdown{0.0};
    std::vector<double> inferenceLatencyMs;
    std::vector<double> tradePnls;       // per-trade realized P&L for sharpe_like computation
    std::vector<float>  tradeMaeTicks;
    std::vector<float>  tradeMfeTicks;
    std::vector<float>  tradeRealizedRrrValues;
    std::vector<int32_t> tradeDurationBars;
    int32_t currentWalkForwardWindowId{0};
};

static BacktestRunContext g_runContext;

constexpr const char* kBacktestSchemaName = "mts_backtest_artifact";
constexpr int kBacktestSchemaVersion = 1;
constexpr const char* kBacktestOutputBaseDirDefault = "C:/SierraChart2/Data/backtests";

std::string BuildRunId()
{
    const auto now = std::chrono::system_clock::now();
    const auto nowUs = std::chrono::duration_cast<std::chrono::microseconds>(
        now.time_since_epoch()).count();
    const std::time_t nowTime = std::chrono::system_clock::to_time_t(now);
    std::tm utcTm{};
#ifdef _WIN32
    gmtime_s(&utcTm, &nowTime);
#else
    gmtime_r(&nowTime, &utcTm);
#endif

    std::ostringstream timePart;
    timePart << "bt_" << std::put_time(&utcTm, "%Y%m%dT%H%M%SZ");

    std::random_device rd;
    std::mt19937_64 rng(rd() ^ static_cast<uint64_t>(nowUs));
    const uint32_t nonce = static_cast<uint32_t>(rng() & 0xFFFFFFFFu);
    std::ostringstream noncePart;
    noncePart << std::hex << std::setw(8) << std::setfill('0') << nonce;

    return timePart.str() + "_" + noncePart.str();
}

std::string BuildUtcTimestampString(uint64_t epochUs)
{
    const std::time_t sec = static_cast<std::time_t>(epochUs / 1000000ULL);
    std::tm utcTm{};
#ifdef _WIN32
    gmtime_s(&utcTm, &sec);
#else
    gmtime_r(&sec, &utcTm);
#endif
    std::ostringstream out;
    out << std::put_time(&utcTm, "%Y-%m-%dT%H:%M:%SZ");
    return out.str();
}

// Expects a pre-sorted non-empty vector; callers that need one sort for multiple percentiles use this.
static double ComputePercentileFromSorted(const std::vector<double>& sorted, double percentile)
{
    const double rank = percentile * static_cast<double>(sorted.size() - 1);
    const auto lowerIdx = static_cast<size_t>(std::floor(rank));
    const auto upperIdx = static_cast<size_t>(std::ceil(rank));
    if (lowerIdx == upperIdx) {
        return sorted[lowerIdx];
    }
    const double frac = rank - static_cast<double>(lowerIdx);
    return sorted[lowerIdx] + (sorted[upperIdx] - sorted[lowerIdx]) * frac;
}

double ComputePercentile(std::vector<double> values, double percentile)
{
    if (values.empty()) {
        return 0.0;
    }
    std::sort(values.begin(), values.end());
    return ComputePercentileFromSorted(values, percentile);
}

// Per-trade Sharpe: mean(PnL) / std(PnL). Not annualized (no daily granularity in replay).
// Labeled "trade_level_not_annualized" in summary. Returns 0.0 when stddev is degenerate.
static double ComputeTradeLevelSharpe(const std::vector<double>& pnls)
{
    if (pnls.size() < 2) {
        return 0.0;
    }
    double sum = 0.0;
    for (double v : pnls) sum += v;
    const double mean = sum / static_cast<double>(pnls.size());
    double sqSum = 0.0;
    for (double v : pnls) sqSum += (v - mean) * (v - mean);
    const double stdDev = std::sqrt(sqSum / static_cast<double>(pnls.size() - 1));
    return (stdDev < 1e-9) ? 0.0 : (mean / stdDev);
}

void ResetRunContext()
{
    g_runContext = BacktestRunContext();
}

std::string CurrentSimAccount(SCStudyInterfaceRef sc)
{
    const char* accountChars = sc.SelectedTradeAccount.GetChars();
    if (accountChars == nullptr || accountChars[0] == '\0') {
        return "SIM";
    }
    return std::string(accountChars);
}

std::string BuildExecutionKey(int parentInternalOrderId)
{
    return g_runContext.runId + "::" + std::to_string(parentInternalOrderId);
}

bool EnsureRunFolderExists(const std::string& runFolder)
{
    try {
        std::filesystem::create_directories(runFolder);
        return true;
    } catch (const std::exception& e) {
        Logger::getInstance().log("BackTester: Failed to create run folder: " + std::string(e.what()));
        return false;
    }
}

bool WriteManifest(SCStudyInterfaceRef sc, const char* status)
{
    try {
        const std::string manifestPath = g_runContext.runFolder + "/manifest.json";
        std::ofstream manifest(manifestPath, std::ios::out | std::ios::trunc);
        if (!manifest.is_open()) {
            Logger::getInstance().log("BackTester: Could not open manifest file for write");
            return false;
        }

        nlohmann::json manifestJson;
        manifestJson["schema_name"] = kBacktestSchemaName;
        manifestJson["schema_version"] = kBacktestSchemaVersion;
        manifestJson["run_id"] = g_runContext.runId;
        manifestJson["phase"] = g_runContext.phase;
        manifestJson["status"] = status;
        manifestJson["symbol"] = std::string(sc.Symbol.GetChars());
        manifestJson["sim_account"] = g_runContext.simAccount;
        manifestJson["started_at_us_utc"] = g_runContext.startedAtUsUtc;
        manifestJson["started_at_utc"] = BuildUtcTimestampString(g_runContext.startedAtUsUtc);
        if (g_runContext.completedAtUsUtc > 0) {
            manifestJson["completed_at_us_utc"] = g_runContext.completedAtUsUtc;
            manifestJson["completed_at_utc"] = BuildUtcTimestampString(g_runContext.completedAtUsUtc);
        }

        // Git identity — filled in via SC inputs before each run
        {
            const char* gitMT  = sc.Input[IN_GIT_SHA_MINDFULTRADER].GetString();
            const char* gitLBR = sc.Input[IN_GIT_SHA_LBRNET].GetString();
            manifestJson["git_sha_mindfultrader"] = (gitMT  && gitMT[0])  ? nlohmann::json(std::string(gitMT))  : nullptr;
            manifestJson["git_sha_lbrnet"]        = (gitLBR && gitLBR[0]) ? nlohmann::json(std::string(gitLBR)) : nullptr;
        }

        // Data range covered by the replay
        manifestJson["data_range_start_us_utc"] = g_runContext.dataRangeStartUsUtc;
        manifestJson["data_range_start_utc"] = (g_runContext.dataRangeStartUsUtc > 0)
            ? BuildUtcTimestampString(g_runContext.dataRangeStartUsUtc) : "";
        manifestJson["data_range_end_us_utc"] = g_runContext.dataRangeEndUsUtc;
        manifestJson["data_range_end_utc"] = (g_runContext.dataRangeEndUsUtc > 0)
            ? BuildUtcTimestampString(g_runContext.dataRangeEndUsUtc) : "";

        // Model artifacts — paths set in SC inputs before each run
        {
            const char* tfPath    = sc.Input[IN_TRANSFORMER_ARTIFACT_PATH].GetString();
            const char* modelPath = sc.Input[IN_MODEL_ARTIFACT_PATH].GetString();
            manifestJson["transformer_artifact"] = (tfPath    && tfPath[0])    ? nlohmann::json(std::string(tfPath))    : nullptr;
            manifestJson["model_artifact"]       = (modelPath && modelPath[0]) ? nlohmann::json(std::string(modelPath)) : nullptr;
        }

        // Parameter profile names — human-readable identifiers for versioned configs
        {
            const char* riskProf = sc.Input[IN_RISK_PARAM_PROFILE].GetString();
            const char* execProf = sc.Input[IN_EXECUTION_PARAM_PROFILE].GetString();
            manifestJson["risk_param_profile"]      = (riskProf && riskProf[0]) ? nlohmann::json(std::string(riskProf)) : nullptr;
            manifestJson["execution_param_profile"] = (execProf && execProf[0]) ? nlohmann::json(std::string(execProf)) : nullptr;
        }

        // Replay isolation (spec appendix §3)
        {
            const int isolIdx = sc.Input[IN_ISOLATION_MODE].GetIndex();
            manifestJson["isolation_mode"]         = (isolIdx == 1) ? "scoped_clear" : "dedicated_sim_account";
            manifestJson["pre_run_clear_applied"]  = (sc.Input[IN_PRE_RUN_CLEAR_APPLIED].GetYesNo() == 1);
        }

        // Timeframe stack — TS3 settings from sc; TS1/TS2 chart numbers from inputs
        manifestJson["timeframe_stack"] = {
            {"ts3_chart_number",    sc.ChartNumber},
            {"ts3_seconds_per_bar", sc.SecondsPerBar},
            {"ts3_symbol",          std::string(sc.Symbol.GetChars())},
            {"ts1_chart_number",    sc.Input[IN_TS1_CHART_NUMBER].GetInt()},
            {"ts2_chart_number",    sc.Input[IN_TS2_CHART_NUMBER].GetInt()}
        };

        manifestJson["replay_mode"] = "sierra_replay";
        manifestJson["protocol_reuse_checks"] = {
            {"indicator_event_publish_path", true},
            {"position_manager_update_path", true},
            {"transport_stream_runtime", true}
        };
        manifestJson["artifacts"] = {
            {"binary", "backtest_run.btst"}
        };

        manifest << manifestJson.dump(2) << std::endl;
        manifest.close();
        return true;
    } catch (const std::exception& e) {
        Logger::getInstance().log("BackTester: WriteManifest exception: " + std::string(e.what()));
        return false;
    }
}

// Entry context: captures state at trade open, consumed when exit is recorded.
struct TradeEntryContext {
    int tradeID;
    int entryBarIndex;

    // FlatBuffer capture at entry (read-only — intValue() preserves dirty flags)
    uint64_t entryTimestampUs{0};
    int32_t  walkForwardWindowId{0};
    int8_t   hmmStateAtEntry{0};
    int8_t   marketClimateAtEntry{0};
    MTS::Schema::IndicatorState indicatorState{};
    bool     hasEntryIndicators{false};
    float    volumeRatioAtEntry{0.0f};
    float    volumeImbalanceAtEntry{0.0f};
    float    closePercentileAtEntry{0.0f};
    float    atr10PriceAtEntry{0.0f};
    // Risk-at-inception: captured once at entry for correct R:R denominator regardless of trailing stops.
    float    initialStopPrice{0.0f};
    float    initialTargetPrice{0.0f};
    uint64_t decisionId{0};

    TradeEntryContext() : tradeID(0), entryBarIndex(0) {}
};

static TradeEntryContext g_entryContext;

void BufferPredictionAckEvent(const PositionManager::PredictionAckEvent& ackEvent)
{
    std::lock_guard<std::mutex> lock(g_predictionAckBufferMutex);
    g_predictionAckBuffer.push_back(ackEvent);
}

bool TryPopBufferedPredictionAckEvent(PositionManager::PredictionAckEvent& out)
{
    std::lock_guard<std::mutex> lock(g_predictionAckBufferMutex);
    if (g_predictionAckBuffer.empty()) {
        return false;
    }

    out = g_predictionAckBuffer.front();
    g_predictionAckBuffer.pop_front();
    return true;
}

// Forward declarations
static void WriteRunManifestFb(SCStudyInterfaceRef sc);
static void WriteRunSummaryFb(SCStudyInterfaceRef sc);
static void WriteDecisionEventFb(SCStudyInterfaceRef sc, uint64_t decisionId,
    bool publishAttempted, bool publishSucceeded, bool isDisconnected, int phase);
void RunTradingPhase(SCStudyInterfaceRef sc, int phase);
void RecordTradeEntry(SCStudyInterfaceRef sc, int tradeID, int entryBarIndex);
void RecordTradeExit(SCStudyInterfaceRef sc, int currentPhase);
void RecordPredictionAckEvent(SCStudyInterfaceRef sc, const PositionManager::PredictionAckEvent& ackEvent);

// Walk-forward optimization helpers
struct WalkForwardWindow {
    int windowId;
    int trainingStart;
    int trainingEnd;
    int testStart;
    int testEnd;
    bool isInTestPeriod;
};

WalkForwardWindow CalculateWalkForwardWindow(int currentBar, int trainingSize,
                                              int testSize, int stepSize);
bool ShouldTradeInWalkForward(SCStudyInterfaceRef sc, int currentBar,
                              WalkForwardWindow& outWindow);

// Failure testing helpers (Elite Protocol validation)
void CheckEmergencyFlatten(SCStudyInterfaceRef sc, int currentBar);
bool IsDisconnected(SCStudyInterfaceRef sc);
void SimulateDisconnect(SCStudyInterfaceRef sc, int currentBar);
void SimulateReconnect(SCStudyInterfaceRef sc, int currentBar);

// ============================================================================
// scsf_BackTester - Phase 2/3: Pure NN & Risk-Managed Backtesting
// ============================================================================

SCSFExport scsf_BackTester(SCStudyInterfaceRef sc)
{
    if (sc.SetDefaults)
    {
        sc.GraphName = "BackTester - Transformer Validation";
        sc.StudyDescription = "Three-phase backtesting: (1) Data Export, (2) Pure NN, (3) Risk-Managed";
        sc.CalculationPrecedence = LOW_PREC_LEVEL;
        sc.AutoLoop = 1;
        sc.GraphRegion = 1;

        // Phase selector input
        sc.Input[IN_PHASE].Name = "Backtest Phase";
        sc.Input[IN_PHASE].SetCustomInputStrings("Phase 1: Data Export;Phase 2: Pure NN;Phase 3: Risk-Managed");
        sc.Input[IN_PHASE].SetCustomInputIndex(0);  // Default to Phase 1

        sc.Input[IN_DEBUG].Name = "Debug BackTester";
        sc.Input[IN_DEBUG].SetYesNo(0);

        // Walk-forward optimization inputs (for time-series cross-validation)
        sc.Input[IN_ENABLE_WALK_FORWARD].Name = "Enable Walk-Forward";
        sc.Input[IN_ENABLE_WALK_FORWARD].SetYesNo(0);

        sc.Input[IN_TRAINING_WINDOW_BARS].Name = "Training Window (bars)";
        sc.Input[IN_TRAINING_WINDOW_BARS].SetInt(500);  // ~2 weeks of 5-min bars
        sc.Input[IN_TRAINING_WINDOW_BARS].SetIntLimits(50, 5000);

        sc.Input[IN_TEST_WINDOW_BARS].Name = "Test Window (bars)";
        sc.Input[IN_TEST_WINDOW_BARS].SetInt(250);  // ~1 week of 5-min bars
        sc.Input[IN_TEST_WINDOW_BARS].SetIntLimits(10, 2000);

        sc.Input[IN_STEP_SIZE_BARS].Name = "Step Size (bars)";
        sc.Input[IN_STEP_SIZE_BARS].SetInt(250);  // Walk forward by 1 week
        sc.Input[IN_STEP_SIZE_BARS].SetIntLimits(10, 1000);

        // Failure testing inputs (Elite Protocol validation)
        sc.Input[IN_EMERGENCY_FLATTEN_BAR].Name = "Emergency Flatten At Bar (0=disabled)";
        sc.Input[IN_EMERGENCY_FLATTEN_BAR].SetInt(0);
        sc.Input[IN_EMERGENCY_FLATTEN_BAR].SetIntLimits(0, 100000);

        sc.Input[IN_SIMULATE_DISCONNECT_BAR].Name = "Simulate Disconnect At Bar (0=disabled)";
        sc.Input[IN_SIMULATE_DISCONNECT_BAR].SetInt(0);
        sc.Input[IN_SIMULATE_DISCONNECT_BAR].SetIntLimits(0, 100000);

        sc.Input[IN_DISCONNECT_DURATION_BARS].Name = "Disconnect Duration (bars)";
        sc.Input[IN_DISCONNECT_DURATION_BARS].SetInt(10);
        sc.Input[IN_DISCONNECT_DURATION_BARS].SetIntLimits(1, 1000);

        // --- Artifact lineage inputs (fill before each run) ---
        sc.Input[IN_GIT_SHA_MINDFULTRADER].Name = "Git SHA (MindfulTrader)";
        sc.Input[IN_GIT_SHA_MINDFULTRADER].SetString("");

        sc.Input[IN_GIT_SHA_LBRNET].Name = "Git SHA (lbrnet)";
        sc.Input[IN_GIT_SHA_LBRNET].SetString("");

        sc.Input[IN_TRANSFORMER_ARTIFACT_PATH].Name = "Transformer Artifact Path";
        sc.Input[IN_TRANSFORMER_ARTIFACT_PATH].SetString("");

        sc.Input[IN_MODEL_ARTIFACT_PATH].Name = "Model Artifact Path (HMM)";
        sc.Input[IN_MODEL_ARTIFACT_PATH].SetString("models/hmm_model.pkl");

        sc.Input[IN_RISK_PARAM_PROFILE].Name = "Risk Param Profile Name";
        sc.Input[IN_RISK_PARAM_PROFILE].SetString("default_v1");

        sc.Input[IN_EXECUTION_PARAM_PROFILE].Name = "Execution Param Profile Name";
        sc.Input[IN_EXECUTION_PARAM_PROFILE].SetString("default_v1");

        // --- Replay isolation ---
        sc.Input[IN_ISOLATION_MODE].Name = "Replay Isolation Mode";
        sc.Input[IN_ISOLATION_MODE].SetCustomInputStrings("dedicated_sim_account;scoped_clear");
        sc.Input[IN_ISOLATION_MODE].SetCustomInputIndex(0);

        sc.Input[IN_PRE_RUN_CLEAR_APPLIED].Name = "Pre-Run Trade Data Clear Applied";
        sc.Input[IN_PRE_RUN_CLEAR_APPLIED].SetYesNo(0);

        // --- Timeframe stack chart numbers (BackTesterStudy runs on TS3) ---
        sc.Input[IN_TS1_CHART_NUMBER].Name = "TS1 Chart Number (240-min)";
        sc.Input[IN_TS1_CHART_NUMBER].SetInt(1);
        sc.Input[IN_TS1_CHART_NUMBER].SetIntLimits(1, 999);

        sc.Input[IN_TS2_CHART_NUMBER].Name = "TS2 Chart Number (60-min)";
        sc.Input[IN_TS2_CHART_NUMBER].SetInt(2);
        sc.Input[IN_TS2_CHART_NUMBER].SetIntLimits(1, 999);

        // --- Output path (override if Sierra Chart is not at the default install path) ---
        sc.Input[IN_OUTPUT_BASE_DIR].Name = "Output Base Directory";
        sc.Input[IN_OUTPUT_BASE_DIR].SetString(kBacktestOutputBaseDirDefault);

        // Visualization subgraphs
        sc.Subgraph[SG_NN_SIGNAL].Name = "NN Signal (-1=SHORT, 0=HOLD, 1=LONG)";
        sc.Subgraph[SG_NN_SIGNAL].DrawStyle = DRAWSTYLE_STAIR_STEP;
        sc.Subgraph[SG_NN_SIGNAL].PrimaryColor = RGB(255, 255, 255);
        sc.Subgraph[SG_NN_SIGNAL].LineWidth = 3;
        sc.Subgraph[SG_NN_SIGNAL].DrawZeros = true;

        sc.Subgraph[SG_ENTRY_PRICE].Name = "Entry Price";
        sc.Subgraph[SG_ENTRY_PRICE].DrawStyle = DRAWSTYLE_DASH;
        sc.Subgraph[SG_ENTRY_PRICE].PrimaryColor = RGB(0, 255, 255);
        sc.Subgraph[SG_ENTRY_PRICE].LineWidth = 2;
        sc.Subgraph[SG_ENTRY_PRICE].DrawZeros = false;

        sc.Subgraph[SG_STOP_PRICE].Name = "Stop Price";
        sc.Subgraph[SG_STOP_PRICE].DrawStyle = DRAWSTYLE_DASH;
        sc.Subgraph[SG_STOP_PRICE].PrimaryColor = RGB(255, 0, 0);
        sc.Subgraph[SG_STOP_PRICE].LineWidth = 2;
        sc.Subgraph[SG_STOP_PRICE].DrawZeros = false;

        sc.Subgraph[SG_TARGET_PRICE].Name = "Target Price";
        sc.Subgraph[SG_TARGET_PRICE].DrawStyle = DRAWSTYLE_DASH;
        sc.Subgraph[SG_TARGET_PRICE].PrimaryColor = RGB(0, 255, 0);
        sc.Subgraph[SG_TARGET_PRICE].LineWidth = 2;
        sc.Subgraph[SG_TARGET_PRICE].DrawZeros = false;

        sc.Subgraph[SG_TRADE_GRADE].Name = "Risk Validation (0=REJECT, 1=ALLOWED)";
        sc.Subgraph[SG_TRADE_GRADE].DrawStyle = DRAWSTYLE_STAIR_STEP;
        sc.Subgraph[SG_TRADE_GRADE].PrimaryColor = RGB(255, 165, 0);
        sc.Subgraph[SG_TRADE_GRADE].LineWidth = 2;
        sc.Subgraph[SG_TRADE_GRADE].DrawZeros = true;

        // Sierra Chart backtesting settings
        sc.AllowMultipleEntriesInSameDirection = false;
        sc.MaximumPositionAllowed = 1;
        sc.SupportReversals = false;
        sc.AllowOppositeEntryWithOpposingPositionOrOrders = false;
        sc.SupportAttachedOrdersForTrading = true;
        sc.AllowEntryWithWorkingOrders = false;
        sc.AllowOnlyOneTradePerBar = true;
        sc.MaintainTradeStatisticsAndTradesData = true;

        return;
    }

    // One-time initialization
    if (sc.UpdateStartIndex == 0)
    {
        {
            std::lock_guard<std::mutex> lock(g_predictionAckBufferMutex);
            g_predictionAckBuffer.clear();
        }

        if (sc.GetPersistentInt(BT_MENU_EXISTS_ID) == 0) {
            sc.SetPersistentInt(BT_MENU_EXISTS_ID, 1);
            sc.SetPersistentInt(BT_FILE_OPENED_FLAG_ID, 0);
            sc.SetPersistentInt(BT_TRADE_COUNT_ID, 0);
            sc.SetPersistentInt(BT_CURRENT_TRADE_ID, 0);
            sc.SetPersistentInt(BT_DISCONNECT_START_BAR_ID, 0);
            sc.SetPersistentInt(BT_IS_DISCONNECTED_ID, 0);
            sc.SetPersistentInt(BT_EMERGENCY_FLATTEN_TRIGGERED_ID, 0);
            sc.SetPersistentInt(BT_HMM_CLIENT_INITIALIZED_ID, 0);
            sc.SetPersistentInt(BT_STRUCTURE_LAST_BAR_ID, -1);
        }

        // Get current phase from input
        int phase = sc.Input[IN_PHASE].GetIndex() + 1;  // 1, 2, or 3
        sc.SetPersistentInt(BT_PHASE_ID, phase);

        Logger::getInstance().log("BackTester initialized in Phase " + std::to_string(phase));

        if (phase >= 2) {
            // ===================================================================
            // PRODUCTION-IDENTICAL INITIALIZATION (same as live trading)
            // ===================================================================

            // Create communication queues
            const auto requestQueue = std::make_shared<ThreadSafeQueue<TradeRequest>>();
            const auto replyQueue = std::make_shared<ThreadSafeQueue<TradeReply>>();

            // Initialize PositionManager (handles trade execution)
            // Elite v2.4: Removed SocketMessage queue (deprecated MindfulSocketZMQ infrastructure)
            PositionManager::Instance().Init(sc, requestQueue, replyQueue);
            PositionManager::Instance().SetPredictionAckSink(BufferPredictionAckEvent);

            // Reset ContextManager physics/structure state at replay epoch boundary.
            ContextManager::Instance().Reset();

            // Initialize RiskManager contract invariants (session balance, ATR baseline).
            // Required for Phase 3 correctness; harmless in Phase 2.
            RiskManager::Instance().Init(sc);

            // Elite v2.4: All ZMQ communication uses centralized TransportStream
            // (TransportStream::Instance().Emit() replaces MindfulSocketZMQ queue management)

            // ===================================================================
            // ELITE: SystemOrchestrator handshake (prevents silent corruption)
            // ===================================================================
            if (!SystemOrchestrator::Instance().Initialize()) {
                Logger::getInstance().log("FATAL: SystemOrchestrator initialization failed");
                return;
            }

            // Wait for Python handshake (timeout 5 seconds)
            const int maxWaitMs = 5000;
            const int pollIntervalMs = 100;
            int elapsedMs = 0;
            bool pythonReady = false;

            while (elapsedMs < maxWaitMs) {
                if (SystemOrchestrator::Instance().IsHandshakeComplete()) {
                    pythonReady = true;
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(pollIntervalMs));
                elapsedMs += pollIntervalMs;
            }

            if (!pythonReady) {
                Logger::getInstance().log("FATAL: Python handshake timeout (5s) - START PYTHON FIRST");
                return;  // Abort backtest
            }

            Logger::getInstance().log("BackTester: Python handshake complete");

            // === INITIALIZE HMM CLIENT (Live parity for MARKET_OBSERVATION + SYSTEM_STATE) ===
            if (sc.GetPersistentInt(BT_HMM_CLIENT_INITIALIZED_ID) == 0) {
                HMMClient::Instance().Init();
                sc.SetPersistentInt(BT_HMM_CLIENT_INITIALIZED_ID, 1);
                Logger::getInstance().log("BackTester: HMMClient initialized (port 5561)");
            }

            // TradeExecutionServer: binds REP on port 5558.
            // Python backtest_server sends ModelPrediction via REQ → PositionManager consumes via
            // ProcessPendingPrediction().  Must initialize here; live SCStudies.cpp does it earlier.
            if (!TradeExecutionServer::Instance().Initialize()) {
                Logger::getInstance().log("FATAL: TradeExecutionServer initialization failed (port 5558)");
                return;
            }
            Logger::getInstance().log("BackTester: TradeExecutionServer initialized (REP on port 5558)");
        }

        // Open data collection files if in replay mode
        if (sc.IsReplayRunning() && phase >= 2) {
            bool fileAlreadyOpened = (sc.GetPersistentInt(BT_FILE_OPENED_FLAG_ID) == 1);
            if (!fileAlreadyOpened) {
                ResetRunContext();
                g_binaryStreamFailed = false;
                g_runContext.phase = phase;
                g_runContext.startedAtUsUtc = GetReplaySafeNowUs(sc);
                g_runContext.runId = BuildRunId();
                g_runContext.simAccount = CurrentSimAccount(sc);
                {
                    const char* baseDir = sc.Input[IN_OUTPUT_BASE_DIR].GetString();
                    const std::string base = (baseDir && baseDir[0]) ? baseDir : kBacktestOutputBaseDirDefault;
                    g_runContext.runFolder = base + "/" + g_runContext.runId;
                }

                if (!EnsureRunFolderExists(g_runContext.runFolder)) {
                    return;
                }

                const std::string binaryPath = g_runContext.runFolder + "/backtest_run.btst";

                Logger::getInstance().log("BackTester: Opening binary stream");
                g_backtestBinaryStream.open(binaryPath, std::ios::out | std::ios::binary | std::ios::trunc);
                if (g_backtestBinaryStream.is_open()) {
                    Logger::getInstance().log("BackTester: Binary stream opened: " + binaryPath);
                    sc.SetPersistentInt(BT_FILE_OPENED_FLAG_ID, 1);

                    // Reset trade tracking
                    g_entryContext = TradeEntryContext();
                    sc.SetPersistentInt(BT_TRADE_COUNT_ID, 0);
                    sc.SetPersistentInt(BT_CURRENT_TRADE_ID, 0);

                    // Manifest starts in running status and is finalized on shutdown.
                    WriteManifest(sc, "running");
                    WriteRunManifestFb(sc);
                } else {
                    Logger::getInstance().log("BackTester: ERROR - Could not open binary stream: " + binaryPath);
                }
            }
        }
    }

    if (sc.LastCallToFunction)
    {
        if (sc.GetPersistentInt(BT_HMM_CLIENT_INITIALIZED_ID) == 1) {
            HMMClient::Instance().Shutdown();
            sc.SetPersistentInt(BT_HMM_CLIENT_INITIALIZED_ID, 0);
        }

        // Shutdown ZMQ connections
        int currentPhase = sc.GetPersistentInt(BT_PHASE_ID);
        if (currentPhase >= 2) {
            // Elite v2.4: All messaging now uses centralized TransportStream
            TransportStream::Instance().Shutdown();
            PositionManager::Instance().SetPredictionAckSink(PositionManager::PredictionAckSink());
            {
                std::lock_guard<std::mutex> lock(g_predictionAckBufferMutex);
                g_predictionAckBuffer.clear();
            }
            Logger::getInstance().log("BackTester: ZMQ connections closed");
        }

        // Write summary and close binary stream
        if (g_backtestBinaryStream.is_open()) {
            g_runContext.completedAtUsUtc = GetReplaySafeNowUs(sc);

            // Capture replay data range end from last processed bar
            if (sc.ArraySize > 0) {
                const int lastIdx = sc.Index;  // sc.Index is the final bar in LastCallToFunction
                SCDateTime lastBarDt(sc.BaseDateTimeIn.DateAt(lastIdx), sc.BaseDateTimeIn.TimeAt(lastIdx));
                g_runContext.dataRangeEndUsUtc = lastBarDt.ToUNIXTimeInMicroseconds();
            }

            WriteRunSummaryFb(sc);
            WriteManifest(sc, "completed");

            Logger::getInstance().log("BackTester: Closing binary stream");
            g_backtestBinaryStream.flush();
            g_backtestBinaryStream.close();

            int totalTrades = sc.GetPersistentInt(BT_TRADE_COUNT_ID);
            Logger::getInstance().log("BackTester: Completed backtest with " +
                std::to_string(totalTrades) + " trades");

            sc.SetPersistentInt(BT_FILE_OPENED_FLAG_ID, 0);
        }
        return;
    }

    // Safety: BackTester ONLY runs during replay
    if (!sc.IsReplayRunning()) {
        return;
    }

    // Get current phase
    int currentPhase = sc.GetPersistentInt(BT_PHASE_ID);

    // =======================================================================
    // PHASE 1: DATA EXPORT (Use scsf_DataCollector instead)
    // =======================================================================
    if (currentPhase == 1) {
        static bool s_phase1Warned = false;
        if (!s_phase1Warned) {
            Logger::getInstance().log("BackTester Phase 1: Use scsf_DataCollector study instead!");
            s_phase1Warned = true;
        }
        return;
    }

    // =======================================================================
    // PHASE 2: PURE NEURAL NETWORK BACKTESTING (Event-Driven)
    // =======================================================================
    // Tests Transformer model accuracy WITHOUT RiskManager filtering.
    // Provides baseline performance to compare against Phase 3.
    //
    // EVENT-DRIVEN FLOW:
    // 1. Update all 50+ technical indicators via IndicatorManager
    // 2. Check for significant indicator changes (HasSignificantChange)
    // 3. If changed: Publish FlatBuffer event to Python via PublishEventOnChange()
    // 4. Python Transformer analyzes indicators, generates predictions
    // 5. Python sends TradeRequest to PositionManager queue
    // 6. PositionManager.Update() processes queue, submits orders to Sierra Chart
    // 7. Detect entry/exit via position state changes
    // 8. Record trade data (entry context + exit analytics)
    //
    // PREDICTION_ACK CALLBACK:
    // PositionManager invokes callback for each ACCEPTED/REJECTED prediction,
    // allowing us to track model calibration (confidence vs acceptance rate).
    //
    // DATA COLLECTION:
    // - Entry: Capture indicator snapshot only (Trade class handles execution)
    // - Exit: Query Trade object for P&L, MAE/MFE, combine with entry indicators
    // =======================================================================
    else if (currentPhase == 2) {
        RunTradingPhase(sc, 2);
    }

    // =======================================================================
    // =======================================================================
    // PHASE 3: RISK-MANAGED BACKTESTING (Event-Driven)
    // =======================================================================
    // Tests COMPLETE system: Transformer → RiskManager → Execution.
    // Validates that risk filters improve performance vs Phase 2 baseline.
    //
    // EVENT-DRIVEN FLOW:
    // Same as Phase 2, but with RiskManager validation:
    // 1. Update all indicators via IndicatorManager
    // 2. Check for significant indicator changes (HasSignificantChange)
    // 3. If changed: Publish FlatBuffer event to Python via PublishEventOnChange()
    // 4. Python Transformer generates predictions
    // 5. Python sends TradeRequest to PositionManager queue
    // 6. RiskManager.Update() checks trading halts (daily loss, max trades, etc.)
    // 7. PositionManager.Update() processes queue:
    //    - Validates order via RiskManager (ATR spikes, volatility, max position)
    //    - If REJECTED: Record to backtest_blocked_trades.jsonl
    //    - If ACCEPTED: Submit order to Sierra Chart
    // 8. Detect entry/exit via position state changes
    // 9. Record trade data (same as Phase 2)
    //
    // KEY DIFFERENCES FROM PHASE 2:
    // - RiskManager.Update() called before PositionManager.Update()
    // - Trading halted if daily loss limit exceeded
    // - Individual trades validated (ATR spike, time-of-day, max position)
    // - Blocked trades recorded separately for analysis
    //
    // ANALYSIS:
    // Compare Phase 3 metrics vs Phase 2 to validate risk improvements:
    // - Win rate, profit factor, max drawdown
    // - Trade frequency (should be lower due to filtering)
    // - Average trade quality (should be higher)
    // =======================================================================
    else if (currentPhase == 3) {
        RunTradingPhase(sc, 3);
    }
}

// ============================================================================
// RunTradingPhase — shared Phase 2/3 per-bar execution loop
// Phase 3 gates PositionManager behind RiskManager; Phase 2 does not.
// ============================================================================
void RunTradingPhase(SCStudyInterfaceRef sc, int phase)
{
    const std::string phaseStr = "Phase " + std::to_string(phase);
    try {
        // Capture actual replay start bar on first processed bar (not dataset bar 0).
        if (!g_runContext.dataRangeStartCaptured) {
            SCDateTime barDt(sc.BaseDateTimeIn.DateAt(sc.Index), sc.BaseDateTimeIn.TimeAt(sc.Index));
            g_runContext.dataRangeStartUsUtc = barDt.ToUNIXTimeInMicroseconds();
            g_runContext.dataRangeStartCaptured = true;
        }

        SimulateDisconnect(sc, sc.Index);
        SimulateReconnect(sc, sc.Index);
        CheckEmergencyFlatten(sc, sc.Index);

        // Physics engine warm-up: LZ/Hill need log-returns every bar to build rolling windows.
        if (sc.Index > 0) {
            const float curr = sc.Close[sc.Index];
            const float prev = sc.Close[sc.Index - 1];
            if (prev > 1e-4f && curr > 1e-4f) {
                ContextManager::Instance().UpdateMarketPhysics(std::log(curr / prev));
            }
        }

        // Set TimeOfDay before UpdateBarContext so all session-based gates read current state.
        {
            auto* tod = IndicatorManager::Instance().GetIndicator<TimeOfDayIndicator>(IndicatorKey::TIME_OF_DAY);
            if (tod) tod->SetFromDateTime(sc.BaseDateTimeIn[sc.Index]);
        }

        WalkForwardWindow currentWindow;
        if (!ShouldTradeInWalkForward(sc, sc.Index, currentWindow)) {
            IndicatorManager::Instance().UpdateBarContext(sc);
            {
                const bool isNewBar = (sc.Index != sc.GetPersistentInt(BT_STRUCTURE_LAST_BAR_ID));
                ContextManager::Instance().UpdatePriceStructure(sc, sc.High[sc.Index], sc.Low[sc.Index], sc.Close[sc.Index], isNewBar);
                if (isNewBar) sc.SetPersistentInt(BT_STRUCTURE_LAST_BAR_ID, sc.Index);
            }
            ContextManager::Instance().CheckAndTriggerHMM(GetReplaySafeNowUs(sc), false, -1.0f,
                                                            IsPostWeekendReopenGracePeriod(sc));
            return;
        }

        g_runContext.currentWalkForwardWindowId = currentWindow.windowId;
        IndicatorManager::Instance().UpdateBarContext(sc);

        bool tradingHalted = false;
        if (phase == 3) {
            RiskManager::Instance().Update(sc);
            tradingHalted = RiskManager::Instance().IsTradingHalted(sc);
        }

        // Elite v3.0: structure metrics (fractal/recurrence) feed HMM features — must precede CheckAndTriggerHMM.
        // Runs unconditionally so feature engines stay warm even on risk-halted days.
        {
            const bool isNewBar = (sc.Index != sc.GetPersistentInt(BT_STRUCTURE_LAST_BAR_ID));
            ContextManager::Instance().UpdatePriceStructure(sc, sc.High[sc.Index], sc.Low[sc.Index], sc.Close[sc.Index], isNewBar);
            if (isNewBar) sc.SetPersistentInt(BT_STRUCTURE_LAST_BAR_ID, sc.Index);
        }
        ContextManager::Instance().CheckAndTriggerHMM(GetReplaySafeNowUs(sc), false, -1.0f,
                                                        IsPostWeekendReopenGracePeriod(sc));

        if (tradingHalted) return;

        const TradeSideEnum sideBeforeUpdate = PositionManager::Instance().GetTradeSide();
        const bool wasInPosition = (sideBeforeUpdate != TradeSideEnum::FLAT);

        PositionManager::Instance().Update(sc);

        PositionManager::PredictionAckEvent ackEvent;
        while (TryPopBufferedPredictionAckEvent(ackEvent)) {
            RecordPredictionAckEvent(sc, ackEvent);
        }

        const TradeSideEnum sideAfterUpdate = PositionManager::Instance().GetTradeSide();
        const bool isInPosition = (sideAfterUpdate != TradeSideEnum::FLAT);

        if (isInPosition && !wasInPosition) {
            int tradeID = sc.GetPersistentInt(BT_CURRENT_TRADE_ID) + 1;
            sc.SetPersistentInt(BT_CURRENT_TRADE_ID, tradeID);
            RecordTradeEntry(sc, tradeID, sc.Index);
            const Trade& trade = PositionManager::Instance().GetOpenTrade();
            if (tradeID == 1 || tradeID % 10 == 0) {
                const std::string side = (trade.GetSide() == TradeSideEnum::LONG) ? "LONG" : "SHORT";
                const std::string suffix = (phase == 3) ? " (RiskMgr OK)" : "";
                Logger::getInstance().log(phaseStr + ": Trade #" + std::to_string(tradeID) + " entered " + side + suffix);
            }
            PositionManager::Instance().PublishSnapshot(sc);
        }

        if (!isInPosition && wasInPosition && g_entryContext.tradeID > 0) {
            const int closingTradeID = g_entryContext.tradeID;  // capture before RecordTradeExit clears context
            RecordTradeExit(sc, phase);
            if (closingTradeID % 10 == 0) {
                Logger::getInstance().log(phaseStr + ": Trade #" + std::to_string(closingTradeID) + " closed");
            }
            PositionManager::Instance().PublishSnapshot(sc);
        }

        {
            s_SCPositionData positionData;
            sc.GetTradePosition(positionData);
            TradeSideEnum currentSide = TradeSideEnum::FLAT;
            if (positionData.PositionQuantity > 0)       currentSide = TradeSideEnum::LONG;
            else if (positionData.PositionQuantity < 0)  currentSide = TradeSideEnum::SHORT;
            const auto sideIndicator = IndicatorManager::Instance().GetIndicator<Side>(IndicatorKey::SIDE);
            if (sideIndicator) sideIndicator->Update(currentSide);
        }

        if (IndicatorManager::Instance().HasSignificantChange()) {
            const bool disconnected = IsDisconnected(sc);
            const bool publishAttempted = !disconnected;
            bool publishSucceeded = false;
            if (!disconnected && IndicatorManager::Instance().PublishEventOnChange(sc)) {
                publishSucceeded = true;
            } else if (disconnected) {
                Logger::getInstance().log(phaseStr + ": Skipping event publish (simulated disconnect)");
            }
            const uint64_t decisionId = ++g_runContext.decisionSeq;
            WriteDecisionEventFb(sc, decisionId, publishAttempted, publishSucceeded, disconnected, phase);
        }
    }
    catch (const std::exception& e) {
        Logger::getInstance().log("BackTester " + phaseStr + " Exception: " + std::string(e.what()));
    }
}

// ============================================================================
// Walk-Forward Optimization Helpers
// ============================================================================

// Calculate which walk-forward window the current bar belongs to
// Returns window ID and whether we're in training vs test period
WalkForwardWindow CalculateWalkForwardWindow(int currentBar, int trainingSize,
                                              int testSize, int stepSize) {
    // Rolling walk-forward with step = stepSize.
    // Window i: train [i*step, i*step+trainSize-1], test [i*step+trainSize, i*step+trainSize+testSize-1].
    // Test periods are non-overlapping as long as stepSize >= testSize.
    // Scan from the most recently started window downward to find the window
    // whose test period contains currentBar; if none, assign to the training
    // period of the latest started window.
    WalkForwardWindow window{};
    const int latestWindowId = currentBar / stepSize;
    for (int i = latestWindowId; i >= 0; --i) {
        const int tStart = i * stepSize + trainingSize;
        const int tEnd   = tStart + testSize - 1;
        if (currentBar >= tStart && currentBar <= tEnd) {
            window.windowId      = i;
            window.trainingStart = i * stepSize;
            window.trainingEnd   = tStart - 1;
            window.testStart     = tStart;
            window.testEnd       = tEnd;
            window.isInTestPeriod = true;
            return window;
        }
    }
    // Not in any test period: bar is in the training region of the latest window.
    window.windowId      = latestWindowId;
    window.trainingStart = latestWindowId * stepSize;
    window.trainingEnd   = window.trainingStart + trainingSize - 1;
    window.testStart     = window.trainingEnd + 1;
    window.testEnd       = window.testStart + testSize - 1;
    window.isInTestPeriod = false;
    return window;
}

// Check if we should trade based on walk-forward settings
// Returns true if trading allowed, false if in training period
bool ShouldTradeInWalkForward(SCStudyInterfaceRef sc, int currentBar,
                              WalkForwardWindow& outWindow) {
    // Check if walk-forward is enabled
    bool walkForwardEnabled = sc.Input[IN_ENABLE_WALK_FORWARD].GetYesNo();
    if (!walkForwardEnabled) {
        // Walk-forward disabled - always allow trading
        outWindow.windowId = 0;
        outWindow.isInTestPeriod = true;
        return true;
    }

    // Get walk-forward parameters
    int trainingSize = sc.Input[IN_TRAINING_WINDOW_BARS].GetInt();
    int testSize = sc.Input[IN_TEST_WINDOW_BARS].GetInt();
    int stepSize = sc.Input[IN_STEP_SIZE_BARS].GetInt();

    // Require minimum training data before starting
    if (currentBar < trainingSize) {
        outWindow.windowId = 0;
        outWindow.isInTestPeriod = false;
        return false;  // Still in initial training period
    }

    // Calculate current window
    outWindow = CalculateWalkForwardWindow(currentBar, trainingSize, testSize, stepSize);

    // Only trade during test periods
    return outWindow.isInTestPeriod;
}

// ============================================================================
// Failure Testing Helpers (Elite Protocol Validation)
// ============================================================================

// Check if Emergency Flatten should be triggered at current bar
void CheckEmergencyFlatten(SCStudyInterfaceRef sc, int currentBar) {
    int triggerBar = sc.Input[IN_EMERGENCY_FLATTEN_BAR].GetInt();

    // Check if enabled and not already triggered
    if (triggerBar == 0 || currentBar != triggerBar) {
        return;
    }

    // Check if already triggered (persistent flag)
    if (sc.GetPersistentInt(BT_EMERGENCY_FLATTEN_TRIGGERED_ID) != 0) {
        return;
    }

    // Trigger Emergency Flatten
    Logger::getInstance().log("========================================");
    Logger::getInstance().log("FAILURE TEST: Emergency Flatten triggered at bar " + std::to_string(currentBar));
    Logger::getInstance().log("========================================");

    std::string reason = "Failure test at bar " + std::to_string(currentBar);
    PositionManager::Instance().EmergencyFlattenPosition(sc, reason.c_str());

    // Mark as triggered
    sc.SetPersistentInt(BT_EMERGENCY_FLATTEN_TRIGGERED_ID, 1);

    Logger::getInstance().log("Emergency Flatten completed - all positions closed");
}

// Check if we're in a simulated disconnect period
bool IsDisconnected(SCStudyInterfaceRef sc) {
    int triggerBar = sc.Input[IN_SIMULATE_DISCONNECT_BAR].GetInt();

    // Check if disconnect simulation is disabled
    if (triggerBar == 0) {
        return false;
    }

    // Check persistent disconnect state
    return (sc.GetPersistentInt(BT_IS_DISCONNECTED_ID) != 0);
}

// Simulate disconnect at specified bar
void SimulateDisconnect(SCStudyInterfaceRef sc, int currentBar) {
    int triggerBar = sc.Input[IN_SIMULATE_DISCONNECT_BAR].GetInt();

    // Check if we should start disconnect
    if (triggerBar == 0 || currentBar != triggerBar) {
        return;
    }

    // Check if already disconnected
    if (sc.GetPersistentInt(BT_IS_DISCONNECTED_ID) != 0) {
        return;
    }

    // Start disconnect
    Logger::getInstance().log("========================================");
    Logger::getInstance().log("FAILURE TEST: Simulating disconnect at bar " + std::to_string(currentBar));
    Logger::getInstance().log("========================================");

    sc.SetPersistentInt(BT_IS_DISCONNECTED_ID, 1);
    sc.SetPersistentInt(BT_DISCONNECT_START_BAR_ID, currentBar);

    int duration = sc.Input[IN_DISCONNECT_DURATION_BARS].GetInt();
    Logger::getInstance().log("Disconnect will last " + std::to_string(duration) + " bars");
}

// Simulate reconnect after disconnect duration
void SimulateReconnect(SCStudyInterfaceRef sc, int currentBar) {
    // Check if we're disconnected
    if (sc.GetPersistentInt(BT_IS_DISCONNECTED_ID) == 0) {
        return;
    }

    int disconnectStartBar = sc.GetPersistentInt(BT_DISCONNECT_START_BAR_ID);
    int duration = sc.Input[IN_DISCONNECT_DURATION_BARS].GetInt();
    int reconnectBar = disconnectStartBar + duration;

    // Check if it's time to reconnect
    if (currentBar < reconnectBar) {
        return;
    }

    // Reconnect
    Logger::getInstance().log("========================================");
    Logger::getInstance().log("FAILURE TEST: Simulating reconnect at bar " + std::to_string(currentBar));
    Logger::getInstance().log("========================================");

    // Send POSITION_SYNC message (Elite Protocol REFINEMENT 2)
    Logger::getInstance().log("Sending POSITION_SYNC to Python for state reconciliation");
    PositionManager::Instance().PublishPositionSync(sc);

    // Clear disconnect state
    sc.SetPersistentInt(BT_IS_DISCONNECTED_ID, 0);
    sc.SetPersistentInt(BT_DISCONNECT_START_BAR_ID, 0);

    Logger::getInstance().log("Reconnect completed - POSITION_SYNC sent");
}

// Determine how a trade exited based on price proximity to stop/target.
// 3-tick tolerance covers rounding, fill slippage, and gap-through on trailed stops.
static std::string InferExitReason(const Trade& trade, double tickSize) {
    // Deterministic exit paths (e.g. time barrier) tag the reason explicitly;
    // honor it before falling back to price inference.
    if (!trade.GetExitReasonTag().empty()) {
        return trade.GetExitReasonTag();
    }

    const double exitPrice  = trade.GetExitPrice();
    const double stopPrice  = trade.GetStop();
    const double targetPrice = trade.GetTarget();
    const double tolerance  = tickSize * 3.0;

    if (stopPrice > 1e-6 && std::fabs(exitPrice - stopPrice) <= tolerance) {
        return "STOP_HIT";
    }
    if (targetPrice > 1e-6 && std::fabs(exitPrice - targetPrice) <= tolerance) {
        return "TARGET_HIT";
    }
    return "MANUAL_OR_TIME_STOP";
}

// ============================================================================
// FlatBuffer binary output helpers
// Each Write*Fb function writes one size-prefixed BacktestFrame to g_backtestBinaryStream.
// ============================================================================

static void WriteFbFrame(flatbuffers::FlatBufferBuilder& fbb)
{
    if (g_binaryStreamFailed) return;
    g_backtestBinaryStream.write(
        reinterpret_cast<const char*>(fbb.GetBufferPointer()),
        static_cast<std::streamsize>(fbb.GetSize()));
    if (!g_backtestBinaryStream.good()) {
        g_binaryStreamFailed = true;
        Logger::getInstance().log(
            "BackTester ERROR: Binary stream write failed (disk full or I/O error). Aborting further writes.");
    }
}

static MTS::Backtest::ExitReason MapExitReason(const std::string& reason)
{
    if (reason == "TARGET_HIT") return MTS::Backtest::ExitReason_PROFIT_TARGET;
    if (reason == "STOP_HIT")   return MTS::Backtest::ExitReason_STOP_LOSS;
    if (reason == "TIME_STOP")  return MTS::Backtest::ExitReason_TIME_STOP;
    // TODO(schema): add ExitReason_TRAP (+ REGIME_INVALIDATION) to backtest_schema.fbs
    // and regenerate, so native TRAP exits are attributable in .btst for the F_0.25
    // deploy-gate measurement. Until then TRAP maps to MANUAL (the explicit "TRAP"
    // exit-reason tag is still preserved on the Trade object).
    if (reason == "TRAP")       return MTS::Backtest::ExitReason_MANUAL;
    return MTS::Backtest::ExitReason_MANUAL;
}

static MTS::Backtest::TradeSide MapTradeSide(TradeSideEnum side)
{
    switch (side) {
        case TradeSideEnum::LONG:  return MTS::Backtest::TradeSide_LONG;
        case TradeSideEnum::SHORT: return MTS::Backtest::TradeSide_SHORT;
        default:                   return MTS::Backtest::TradeSide_FLAT;
    }
}

// Read-only indicator snapshot for FlatBuffer TradeRecord.
// Uses intValue() throughout — never ExtractInt8AndClearDirty() — so dirty flags are preserved.
static MTS::Schema::IndicatorState BuildEntryIndicatorState()
{
    MTS::Schema::IndicatorState state{};
    auto& im = IndicatorManager::Instance();

    // Helper: safely read int8 from typed pointer
    auto i8 = [](const auto* p) -> int8_t {
        return p ? static_cast<int8_t>(p->intValue()) : int8_t{0};
    };

    // Int8 signal indicators
    state.mutate_long_macd(i8(im.GetIndicator<Macd>(IndicatorKey::LONG_MACD)));
    state.mutate_long_fi13_signal(i8(im.GetIndicator<FI13Signal>(IndicatorKey::LONG_FI13_SIGNAL)));
    state.mutate_long_macd_divergence(i8(im.GetIndicator<MACDDivergence>(IndicatorKey::LONG_MACD_DIVERGENCE)));
    state.mutate_long_imp(i8(im.GetIndicator<Impulse>(IndicatorKey::LONG_IMP)));
    state.mutate_interm_stochastic(i8(im.GetIndicator<Stochastic>(IndicatorKey::INTERM_STOCHASTIC)));
    state.mutate_raschke_strategy_setup(i8(im.GetIndicator<RaschkeStrategyIndicator>(IndicatorKey::RASCHKE_STRATEGY_SETUP)));
    state.mutate_raschke_tactical_trigger(i8(im.GetIndicator<RaschkeTacticalIndicator>(IndicatorKey::RASCHKE_TACTICAL_TRIGGER)));
    state.mutate_rsi(i8(im.GetIndicator<RSIIndicator>(IndicatorKey::RSI)));
    state.mutate_interm_fi2_signal(i8(im.GetIndicator<FI2Signal>(IndicatorKey::INTERM_FI2_SIGNAL)));
    state.mutate_ema_proximity(i8(im.GetIndicator<EmaProximityIndicator>(IndicatorKey::EMA_PROXIMITY)));
    state.mutate_price_metrics(i8(im.GetIndicator<PriceMetricsIndicator>(IndicatorKey::PRICE_METRICS)));
    state.mutate_interm_macd(i8(im.GetIndicator<Macd>(IndicatorKey::INTERM_MACD)));
    state.mutate_interm_macd_divergence(i8(im.GetIndicator<MACDDivergence>(IndicatorKey::INTERM_MACD_DIVERGENCE)));
    state.mutate_interm_imp(i8(im.GetIndicator<Impulse>(IndicatorKey::INTERM_IMP)));
    state.mutate_structure_test(i8(im.GetIndicator<StructureTestIndicator>(IndicatorKey::STRUCTURE_TEST)));
    state.mutate_volume_signal(i8(im.GetIndicator<VolumeIndicator>(IndicatorKey::VOLUME_SIGNAL)));
    state.mutate_atr_proximity(i8(im.GetIndicator<ATRProximityIndicator>(IndicatorKey::ATR_PROXIMITY)));
    {
        const int8_t bias = i8(im.GetIndicator<DailyBiasIndicator>(IndicatorKey::DAILY_BIAS));
        state.mutate_daily_bias(bias);
        state.mutate_daily_bias_enum(bias);
    }
    state.mutate_time_of_day(i8(im.GetIndicator<TimeOfDayIndicator>(IndicatorKey::TIME_OF_DAY)));
    state.mutate_kangaroo_tail(i8(im.GetIndicator<KangarooTail>(IndicatorKey::KANGAROO_TAIL)));
    state.mutate_turtle_soup(i8(im.GetIndicator<TurtleSoup>(IndicatorKey::TURTLE_SOUP)));
    state.mutate_momentum_pinball(i8(im.GetIndicator<MomentumPinball>(IndicatorKey::MOMENTUM_PINBALL)));
    state.mutate_elder_breakout(i8(im.GetIndicator<ElderBreakout>(IndicatorKey::ELDER_BREAKOUT)));
    state.mutate_nr7(i8(im.GetIndicator<NR7>(IndicatorKey::NR7)));
    state.mutate_nh_nl_signal(i8(im.GetIndicator<NhNlSignalIndicator>(IndicatorKey::NH_NL_SIGNAL)));
    state.mutate_oscillator_310(i8(im.GetIndicator<Oscillator310>(IndicatorKey::OSCILLATOR_310)));
    state.mutate_zn_trend(i8(im.GetIndicator<CrossMarketTrend>(IndicatorKey::ZN_TREND)));
    state.mutate_dx_trend(i8(im.GetIndicator<CrossMarketTrend>(IndicatorKey::DX_TREND)));

    // Quality scores (float)
    { const auto* p = im.GetIndicator<KangarooTail>(IndicatorKey::KANGAROO_TAIL);    state.mutate_kangaroo_tail_quality(p ? p->QualityScore() : 0.0f); }
    { const auto* p = im.GetIndicator<TurtleSoup>(IndicatorKey::TURTLE_SOUP);        state.mutate_turtle_soup_quality(p ? p->QualityScore() : 0.0f); }
    { const auto* p = im.GetIndicator<MomentumPinball>(IndicatorKey::MOMENTUM_PINBALL); state.mutate_momentum_pinball_quality(p ? p->QualityScore() : 0.0f); }
    { const auto* p = im.GetIndicator<ElderBreakout>(IndicatorKey::ELDER_BREAKOUT);  state.mutate_elder_breakout_quality(p ? p->QualityScore() : 0.0f); }
    { const auto* p = im.GetIndicator<NR7>(IndicatorKey::NR7);                       state.mutate_nr7_quality(p ? p->QualityScore() : 0.0f); }

    // Normalized (Z-score) floats
    { const auto* p = im.GetIndicator<FI2Signal>(IndicatorKey::INTERM_FI2_SIGNAL); state.mutate_interm_fi2_norm(p ? p->ZScore() : 0.0f); }
    { const auto* p = im.GetIndicator<Macd>(IndicatorKey::INTERM_MACD);            state.mutate_interm_macd_norm(p ? p->ZScore() : 0.0f); }
    { const auto* p = im.GetIndicator<FI13Signal>(IndicatorKey::LONG_FI13_SIGNAL); state.mutate_long_fi13_norm(p ? p->ZScore() : 0.0f); }

    // Impulse run length
    { const auto* p = im.GetIndicator<Impulse>(IndicatorKey::INTERM_IMP); state.mutate_impulse_run_length(p ? static_cast<int8_t>(p->RunLength()) : int8_t{0}); }

    // Cross-market correlations (float)
    { const auto* p = im.GetIndicator<CorrelationIndicator>(IndicatorKey::CORR_ES_ZN);       state.mutate_corr_es_zn(p ? p->Value() : 0.0f); }
    { const auto* p = im.GetIndicator<CorrelationIndicator>(IndicatorKey::CORR_ES_DX);       state.mutate_corr_es_dx(p ? p->Value() : 0.0f); }
    { const auto* p = im.GetIndicator<CorrelationIndicator>(IndicatorKey::CORR_ES_ZN_DELTA); state.mutate_corr_es_zn_delta(p ? p->Value() : 0.0f); }
    { const auto* p = im.GetIndicator<CorrelationIndicator>(IndicatorKey::CORR_ES_ZN_ACCEL); state.mutate_corr_es_zn_accel(p ? p->Value() : 0.0f); }
    { const auto* p = im.GetIndicator<CorrelationIndicator>(IndicatorKey::CORR_ES_DX_DELTA); state.mutate_corr_es_dx_delta(p ? p->Value() : 0.0f); }
    { const auto* p = im.GetIndicator<CorrelationIndicator>(IndicatorKey::CORR_ES_DX_ACCEL); state.mutate_corr_es_dx_accel(p ? p->Value() : 0.0f); }

    return state;
}

static void WriteRunManifestFb(SCStudyInterfaceRef sc)
{
    if (!g_backtestBinaryStream.is_open()) return;
    try {
        flatbuffers::FlatBufferBuilder fbb(1024);

        const std::string tfStack = nlohmann::json{
            {"ts1", sc.Input[IN_TS1_CHART_NUMBER].GetInt()},
            {"ts2", sc.Input[IN_TS2_CHART_NUMBER].GetInt()},
            {"ts3", sc.ChartNumber},
            {"ts3_seconds_per_bar", sc.SecondsPerBar},
            {"ts3_symbol", std::string(sc.Symbol.GetChars())}
        }.dump();

        const int isolIdx = sc.Input[IN_ISOLATION_MODE].GetIndex();
        const char* isoStr = (isolIdx == 1) ? "scoped_clear" : "dedicated_sim_account";

        const char* gitMT  = sc.Input[IN_GIT_SHA_MINDFULTRADER].GetString();
        const char* gitLBR = sc.Input[IN_GIT_SHA_LBRNET].GetString();
        const char* tfPath = sc.Input[IN_TRANSFORMER_ARTIFACT_PATH].GetString();
        const char* mdPath = sc.Input[IN_MODEL_ARTIFACT_PATH].GetString();
        const char* riskP  = sc.Input[IN_RISK_PARAM_PROFILE].GetString();
        const char* execP  = sc.Input[IN_EXECUTION_PARAM_PROFILE].GetString();

        auto manifestOff = MTS::Backtest::CreateRunManifestDirect(
            fbb,
            static_cast<uint16_t>(kBacktestSchemaVersion),
            g_runContext.runId.c_str(),
            static_cast<int64_t>(g_runContext.startedAtUsUtc),
            sc.Symbol.GetChars(),
            tfStack.c_str(),
            static_cast<int8_t>(g_runContext.phase),
            MTS::Schema::BacktestReplayMode_ACCURATE_TRADING_SYSTEM_BACK_TEST,
            1.0f,    // replay_speed
            false,   // skip_empty_periods
            false,   // capture_live_events
            nullptr, // data_range_start (not known at manifest time)
            nullptr, // data_range_end
            sc.Input[IN_ENABLE_WALK_FORWARD].GetYesNo() ? sc.Input[IN_TRAINING_WINDOW_BARS].GetInt() : 0,
            sc.Input[IN_ENABLE_WALK_FORWARD].GetYesNo() ? sc.Input[IN_TEST_WINDOW_BARS].GetInt()     : 0,
            sc.Input[IN_ENABLE_WALK_FORWARD].GetYesNo() ? sc.Input[IN_STEP_SIZE_BARS].GetInt()       : 0,
            g_runContext.simAccount.c_str(),
            isoStr,
            (sc.Input[IN_PRE_RUN_CLEAR_APPLIED].GetYesNo() == 1),
            (gitMT  && gitMT[0])  ? gitMT  : nullptr,
            (gitLBR && gitLBR[0]) ? gitLBR : nullptr,
            (tfPath && tfPath[0]) ? tfPath : nullptr,
            nullptr, // transformer_weights_sha256
            nullptr, // feature_contract_hash
            (mdPath && mdPath[0]) ? mdPath : nullptr,
            (riskP  && riskP[0])  ? riskP  : nullptr,
            (execP  && execP[0])  ? execP  : nullptr,
            nullptr, // dataset_path
            nullptr  // dataset_sha256
        );

        auto frameOff = MTS::Backtest::CreateBacktestFrameDirect(
            fbb,
            static_cast<int64_t>(g_runContext.startedAtUsUtc),
            0,
            g_runContext.runId.c_str(),
            MTS::Backtest::BacktestRecord_RunManifest,
            manifestOff.Union());

        fbb.FinishSizePrefixed(frameOff);
        WriteFbFrame(fbb);
    } catch (const std::exception& e) {
        Logger::getInstance().log("WriteRunManifestFb exception: " + std::string(e.what()));
    }
}

static void WriteTradeRecordFb(SCStudyInterfaceRef sc, const Trade& trade, int currentPhase)
{
    if (!g_backtestBinaryStream.is_open()) return;
    try {
        flatbuffers::FlatBufferBuilder fbb(1024);

        SCDateTime exitTime(sc.BaseDateTimeIn.DateAt(sc.Index), sc.BaseDateTimeIn.TimeAt(sc.Index));
        const int64_t exitTimestampUs = static_cast<int64_t>(exitTime.ToUNIXTimeInMicroseconds());

        const std::string execKey = BuildExecutionKey(trade.GetParentOrderId());
        const MTS::Backtest::TradeSide side = MapTradeSide(trade.GetSide());
        const MTS::Backtest::ExitReason exitReason = MapExitReason(InferExitReason(trade, sc.TickSize));

        const float entryPriceF  = static_cast<float>(trade.GetEntryPrice());
        const float exitPriceF   = static_cast<float>(trade.GetExitPrice());
        // Store risk-at-inception prices; these are correct even when stops trailed during the trade.
        const float stopPriceF   = g_entryContext.initialStopPrice;
        const float targetPriceF = g_entryContext.initialTargetPrice;
        const float realizedPnl  = static_cast<float>(trade.GetRealizedPnL());

        const double rawPnl = (trade.GetSide() == TradeSideEnum::LONG)
            ? (trade.GetExitPrice() - trade.GetEntryPrice())
            : (trade.GetEntryPrice() - trade.GetExitPrice());
        const float pnlTicks = static_cast<float>(rawPnl / sc.TickSize);
        const float maeTicks = static_cast<float>(trade.GetMAETicks());
        const float mfeTicks = static_cast<float>(trade.GetMFETicks());

        // R:R denominator is the initial stop risk, not the exit-time (trailed) stop distance.
        const float stopDist    = static_cast<float>(std::fabs(trade.GetEntryPrice() - static_cast<double>(stopPriceF)));
        const float tgtDist     = static_cast<float>(std::fabs(static_cast<double>(targetPriceF) - trade.GetEntryPrice()));
        const float stopDistTks = (sc.TickSize > 0.0) ? static_cast<float>(stopDist / sc.TickSize) : 0.0f;
        const float tgtDistTks  = (sc.TickSize > 0.0) ? static_cast<float>(tgtDist / sc.TickSize) : 0.0f;
        const float theorRRR    = (stopDist > 0.0f) ? (tgtDist / stopDist) : 0.0f;
        const float realRRR     = (stopDist > 0.0f) ? static_cast<float>(rawPnl / static_cast<double>(stopDist)) : 0.0f;

        float atr10 = g_entryContext.atr10PriceAtEntry;
        float stopDistAtr = kNaN, maeAtr = kNaN, mfeAtr = kNaN;
        if (atr10 > static_cast<float>(sc.TickSize)) {
            stopDistAtr = stopDist / atr10;
            maeAtr      = (maeTicks * static_cast<float>(sc.TickSize)) / atr10;
            mfeAtr      = (mfeTicks * static_cast<float>(sc.TickSize)) / atr10;
        } else {
            atr10 = kNaN;
        }

        MTS::Schema::PatternType patternType = MTS::Schema::PatternType_UNDEFINED;
        {
            const int pid = trade.GetPatternId();
            if (pid >= static_cast<int>(MTS::Schema::PatternType_MIN) &&
                pid <= static_cast<int>(MTS::Schema::PatternType_MAX)) {
                patternType = static_cast<MTS::Schema::PatternType>(pid);
            }
        }

        const int32_t barsHeld = sc.Index - g_entryContext.entryBarIndex;

        auto tradeOff = MTS::Backtest::CreateTradeRecordDirect(
            fbb,
            static_cast<uint16_t>(kBacktestSchemaVersion),
            g_runContext.runId.c_str(),
            static_cast<uint64_t>(g_runContext.tradeSeq),
            execKey.c_str(),
            static_cast<int64_t>(g_entryContext.entryTimestampUs),
            exitTimestampUs,
            g_entryContext.entryBarIndex,
            sc.Index,
            barsHeld,
            static_cast<int8_t>(currentPhase),
            g_entryContext.walkForwardWindowId,
            side,
            static_cast<float>(trade.GetSize()),
            entryPriceF,
            exitPriceF,
            stopPriceF,
            targetPriceF,
            exitReason,
            (realizedPnl > 0.0f),
            realizedPnl,
            pnlTicks,
            maeTicks,
            mfeTicks,
            g_runContext.simAccount.c_str(),
            trade.GetParentOrderId(),
            trade.GetStopInternalOrderID(),
            trade.GetTargetInternalOrderID(),
            stopDistTks,
            tgtDistTks,
            realRRR,
            theorRRR,
            atr10,
            stopDistAtr,
            maeAtr,
            mfeAtr,
            patternType,
            static_cast<uint8_t>(trade.GetPatternId()),
            trade.GetPatternName().c_str(),
            static_cast<float>(trade.GetConfidence()),
            g_entryContext.hmmStateAtEntry,
            g_entryContext.marketClimateAtEntry,
            0.0f, 0.0f, 0.0f,  // original_target1/2/3 (not tracked)
            static_cast<int16_t>(trade.GetEntryGrade()),
            static_cast<int16_t>(trade.GetExitGrade()),
            static_cast<int16_t>(trade.GetTradeGrade()),
            g_entryContext.hasEntryIndicators,
            g_entryContext.hasEntryIndicators ? &g_entryContext.indicatorState : nullptr,
            g_entryContext.volumeRatioAtEntry,
            g_entryContext.volumeImbalanceAtEntry,
            g_entryContext.closePercentileAtEntry,
            g_entryContext.atr10PriceAtEntry,
            g_entryContext.decisionId
        );

        auto frameOff = MTS::Backtest::CreateBacktestFrameDirect(
            fbb,
            exitTimestampUs,
            sc.Index,
            g_runContext.runId.c_str(),
            MTS::Backtest::BacktestRecord_TradeRecord,
            tradeOff.Union());

        fbb.FinishSizePrefixed(frameOff);
        WriteFbFrame(fbb);
    } catch (const std::exception& e) {
        Logger::getInstance().log("WriteTradeRecordFb exception: " + std::string(e.what()));
    }
}

static void WriteDecisionEventFb(
    SCStudyInterfaceRef sc,
    uint64_t decisionId,
    bool publishAttempted,
    bool publishSucceeded,
    bool isDisconnected,
    int phase)
{
    if (!g_backtestBinaryStream.is_open()) return;
    try {
        flatbuffers::FlatBufferBuilder fbb(512);

        const int64_t timestampUs = static_cast<int64_t>(GetReplaySafeNowUs(sc));

        const auto* hmmInd     = InferenceManager::Instance().HmmState();
        const auto* climateInd = InferenceManager::Instance().MarketClimate();
        const int8_t hmmState     = hmmInd     ? static_cast<int8_t>(hmmInd->intValue())     : int8_t{0};
        const int8_t marketClimate = climateInd ? static_cast<int8_t>(climateInd->intValue()) : int8_t{0};

        auto decisionOff = MTS::Backtest::CreateDecisionEventDirect(
            fbb,
            static_cast<uint16_t>(kBacktestSchemaVersion),
            g_runContext.runId.c_str(),
            decisionId,
            timestampUs,
            sc.Index,
            static_cast<int8_t>(phase),
            g_runContext.currentWalkForwardWindowId,
            0,     // model_action (not available at decision point without ack)
            0.0f,  // model_confidence
            0.0f,  // action_entropy
            0.0f,  // top2_margin
            kNaN,  // model_latency_ms
            kNaN,  // transformer_latency_ms
            kNaN,  // regime_latency_ms
            publishAttempted,
            publishSucceeded,
            isDisconnected,
            hmmState,
            marketClimate,
            0.0f,  // regime_entropy
            0.0f   // regime_transition_risk
        );

        auto frameOff = MTS::Backtest::CreateBacktestFrameDirect(
            fbb,
            timestampUs,
            sc.Index,
            g_runContext.runId.c_str(),
            MTS::Backtest::BacktestRecord_DecisionEvent,
            decisionOff.Union());

        fbb.FinishSizePrefixed(frameOff);
        WriteFbFrame(fbb);
    } catch (const std::exception& e) {
        Logger::getInstance().log("WriteDecisionEventFb exception: " + std::string(e.what()));
    }
}

static void WritePredictionAckFb(SCStudyInterfaceRef sc, const PositionManager::PredictionAckEvent& ackEvent)
{
    if (!g_backtestBinaryStream.is_open()) return;
    try {
        flatbuffers::FlatBufferBuilder fbb(512);

        const int64_t timestampUs = static_cast<int64_t>(GetReplaySafeNowUs(sc));

        const MTS::Backtest::PredictionAckStatus status = ackEvent.accepted
            ? MTS::Backtest::PredictionAckStatus_ACCEPTED
            : MTS::Backtest::PredictionAckStatus_REJECTED;

        const MTS::Schema::ReasonCode rejectCode =
            static_cast<MTS::Schema::ReasonCode>(static_cast<uint8_t>(ackEvent.reasonCode));

        const int8_t action = static_cast<int8_t>(static_cast<int>(ackEvent.action));

        // Phase 3 only: RiskManager was active this run. Phase 2 has no risk gate context.
        // nlohmann::json handles NaN/inf from GetDailyPnL() via explicit finite check.
        std::string riskGateJson = "{}";
        if (g_runContext.phase == 3) {
            const double dailyPnl = RiskManager::Instance().GetDailyPnL();
            riskGateJson = nlohmann::json{
                {"trading_halted",     RiskManager::Instance().IsTradingHalted(sc)},
                {"consecutive_losses", RiskManager::Instance().GetConsecutiveLosses(sc)},
                {"daily_pnl",          std::isfinite(dailyPnl) ? nlohmann::json(dailyPnl) : nlohmann::json(nullptr)}
            }.dump();
        }

        auto ackOff = MTS::Backtest::CreatePredictionAckDirect(
            fbb,
            static_cast<uint16_t>(kBacktestSchemaVersion),
            g_runContext.runId.c_str(),
            g_runContext.ackSeq,   // already incremented before this call
            ackEvent.sequenceId,   // decision_id this ack corresponds to
            timestampUs,
            status,
            rejectCode,
            action,
            ackEvent.confidence,
            riskGateJson.c_str()
        );

        auto frameOff = MTS::Backtest::CreateBacktestFrameDirect(
            fbb,
            timestampUs,
            sc.Index,
            g_runContext.runId.c_str(),
            MTS::Backtest::BacktestRecord_PredictionAck,
            ackOff.Union());

        fbb.FinishSizePrefixed(frameOff);
        WriteFbFrame(fbb);
    } catch (const std::exception& e) {
        Logger::getInstance().log("WritePredictionAckFb exception: " + std::string(e.what()));
    }
}

static void WriteRunSummaryFb(SCStudyInterfaceRef sc)
{
    if (!g_backtestBinaryStream.is_open()) return;
    try {
        flatbuffers::FlatBufferBuilder fbb(512);

        const int64_t completedAtUs = static_cast<int64_t>(g_runContext.completedAtUsUtc);
        const uint32_t tradeCnt     = static_cast<uint32_t>(g_runContext.tradeSeq);
        const double acceptRate = (g_runContext.totalPredictions > 0)
            ? static_cast<double>(g_runContext.acceptedPredictions) / static_cast<double>(g_runContext.totalPredictions)
            : 0.0;
        const double winRate = (tradeCnt > 0)
            ? static_cast<double>(g_runContext.winningTrades) / static_cast<double>(tradeCnt)
            : 0.0;
        const float profitFactor = (g_runContext.grossLossDollarsAbs > 0.0)
            ? static_cast<float>(g_runContext.grossProfitDollars / g_runContext.grossLossDollarsAbs)
            : kNaN;
        const float expectancy = (tradeCnt > 0)
            ? static_cast<float>(g_runContext.totalPnlDollars / static_cast<double>(tradeCnt))
            : 0.0f;
        const bool hasSharpe = (g_runContext.tradePnls.size() >= 2);
        const float sharpeLike = hasSharpe
            ? static_cast<float>(ComputeTradeLevelSharpe(g_runContext.tradePnls))
            : kNaN;

        // Averages over per-trade tracking vectors
        auto avgF = [](const std::vector<float>& v) -> float {
            if (v.empty()) return 0.0f;
            float s = 0.0f;
            for (float x : v) s += x;
            return s / static_cast<float>(v.size());
        };
        auto avgI = [](const std::vector<int32_t>& v) -> float {
            if (v.empty()) return 0.0f;
            float s = 0.0f;
            for (int32_t x : v) s += static_cast<float>(x);
            return s / static_cast<float>(v.size());
        };

        float p50Lat = 0.0f, p95Lat = 0.0f, p99Lat = 0.0f;
        if (!g_runContext.inferenceLatencyMs.empty()) {
            auto sortedLat = g_runContext.inferenceLatencyMs;
            std::sort(sortedLat.begin(), sortedLat.end());
            p50Lat = static_cast<float>(ComputePercentileFromSorted(sortedLat, 0.50));
            p95Lat = static_cast<float>(ComputePercentileFromSorted(sortedLat, 0.95));
            p99Lat = static_cast<float>(ComputePercentileFromSorted(sortedLat, 0.99));
        }

        auto summaryOff = MTS::Backtest::CreateRunSummaryDirect(
            fbb,
            static_cast<uint16_t>(kBacktestSchemaVersion),
            g_runContext.runId.c_str(),
            completedAtUs,
            static_cast<uint64_t>(g_runContext.decisionSeq),
            static_cast<uint32_t>(g_runContext.totalPredictions),
            static_cast<uint32_t>(g_runContext.acceptedPredictions),
            static_cast<uint32_t>(g_runContext.rejectedPredictions),
            static_cast<float>(acceptRate),
            tradeCnt,
            static_cast<uint32_t>(g_runContext.winningTrades),
            static_cast<float>(winRate),
            static_cast<float>(g_runContext.totalPnlDollars),
            static_cast<float>(g_runContext.grossProfitDollars),
            static_cast<float>(g_runContext.grossLossDollarsAbs),
            profitFactor,
            expectancy,
            static_cast<float>(g_runContext.maxDrawdown),
            static_cast<float>(g_runContext.peakEquity),
            static_cast<float>(g_runContext.cumulativeEquity),
            sharpeLike,
            avgI(g_runContext.tradeDurationBars),
            avgF(g_runContext.tradeMaeTicks),
            avgF(g_runContext.tradeMfeTicks),
            avgF(g_runContext.tradeRealizedRrrValues),
            p50Lat,
            p95Lat,
            p99Lat,
            kNaN, kNaN, kNaN   // e2e latency (not measured in backtest)
        );

        auto frameOff = MTS::Backtest::CreateBacktestFrameDirect(
            fbb,
            completedAtUs,
            sc.Index,
            g_runContext.runId.c_str(),
            MTS::Backtest::BacktestRecord_RunSummary,
            summaryOff.Union());

        fbb.FinishSizePrefixed(frameOff);
        WriteFbFrame(fbb);
    } catch (const std::exception& e) {
        Logger::getInstance().log("WriteRunSummaryFb exception: " + std::string(e.what()));
    }
}

// ============================================================================
// Record prediction ACK/REJECT for model calibration analysis
void RecordPredictionAckEvent(SCStudyInterfaceRef sc, const PositionManager::PredictionAckEvent& ackEvent) {
    ++g_runContext.ackSeq;
    ++g_runContext.totalPredictions;
    if (ackEvent.accepted) {
        ++g_runContext.acceptedPredictions;
        g_entryContext.decisionId = ackEvent.sequenceId;
    } else {
        ++g_runContext.rejectedPredictions;
    }
    if (ackEvent.inferenceLatencyUs >= 0) {
        g_runContext.inferenceLatencyMs.push_back(
            static_cast<double>(ackEvent.inferenceLatencyUs) / 1000.0);
    }
    WritePredictionAckFb(sc, ackEvent);
}

// Record trade entry context (FlatBuffer capture, all read-only).
void RecordTradeEntry(SCStudyInterfaceRef sc, int tradeID, int entryBarIndex)
{
    g_entryContext.tradeID       = tradeID;
    g_entryContext.entryBarIndex = entryBarIndex;

    // FlatBuffer-specific fields captured once at entry, used later at exit
    SCDateTime entryTime(sc.BaseDateTimeIn.DateAt(entryBarIndex), sc.BaseDateTimeIn.TimeAt(entryBarIndex));
    g_entryContext.entryTimestampUs    = entryTime.ToUNIXTimeInMicroseconds();
    g_entryContext.walkForwardWindowId = g_runContext.currentWalkForwardWindowId;

    // HMM state and market climate at entry (from InferenceManager — same source as live path)
    const auto* hmmInd     = InferenceManager::Instance().HmmState();
    const auto* climateInd = InferenceManager::Instance().MarketClimate();
    g_entryContext.hmmStateAtEntry     = hmmInd     ? static_cast<int8_t>(hmmInd->intValue())     : int8_t{0};
    g_entryContext.marketClimateAtEntry = climateInd ? static_cast<int8_t>(climateInd->intValue()) : int8_t{0};

    // Risk-at-inception: initial stop/target locked here so R:R is correct even after trailing.
    {
        const Trade& openTrade = PositionManager::Instance().GetOpenTrade();
        g_entryContext.initialStopPrice   = static_cast<float>(openTrade.GetStop());
        g_entryContext.initialTargetPrice = static_cast<float>(openTrade.GetTarget());
    }

    // Full indicator state (read-only — intValue() preserves dirty flags)
    g_entryContext.indicatorState     = BuildEntryIndicatorState();
    g_entryContext.hasEntryIndicators = true;

    // Continuous indicator fields stored separately for TradeRecord scalar columns
    auto& im = IndicatorManager::Instance();
    const auto* vol = im.GetIndicator<VolumeIndicator>(IndicatorKey::VOLUME_SIGNAL);
    g_entryContext.volumeRatioAtEntry     = vol ? vol->GetVolumeRatio()     : 0.0f;
    g_entryContext.volumeImbalanceAtEntry = vol ? vol->GetVolumeImbalance() : 0.0f;

    const auto* pm = im.GetIndicator<PriceMetricsIndicator>(IndicatorKey::PRICE_METRICS);
    g_entryContext.closePercentileAtEntry = pm ? pm->GetClosePercentile() : 0.0f;

    const auto* atr = im.GetIndicator<ATRProximityIndicator>(IndicatorKey::ATR_PROXIMITY);
    g_entryContext.atr10PriceAtEntry = (atr && atr->GetATR10() > 0.0f) ? atr->GetATR10() : kNaN;
}

// Record trade exit — writes FlatBuffer TradeRecord and updates run aggregates.
void RecordTradeExit(SCStudyInterfaceRef sc, int currentPhase)
{
    if (g_entryContext.tradeID == 0) {
        Logger::getInstance().log("RecordTradeExit: No active trade to close");
        return;
    }

    try {
        const Trade& trade = PositionManager::Instance().GetLastClosedTrade();

        ++g_runContext.tradeSeq;
        const bool streamWasGood = !g_binaryStreamFailed;
        WriteTradeRecordFb(sc, trade, currentPhase);
        if (streamWasGood && g_binaryStreamFailed) {
            // WriteFbFrame failed during this write — roll back so tradeSeq stays in sync with written frames.
            --g_runContext.tradeSeq;
        }

        // Accumulate aggregates only for frames that were actually written.
        // Once g_binaryStreamFailed is set, all future writes abort via WriteFbFrame's early return.
        // Skipping aggregates here keeps summary metrics in sync with the written trade count.
        if (!g_binaryStreamFailed) {
            // Increment trade counter
            int tradeCount = sc.GetPersistentInt(BT_TRADE_COUNT_ID);
            sc.SetPersistentInt(BT_TRADE_COUNT_ID, tradeCount + 1);

            // Update run aggregates for summary metrics
            const double realizedPnl = trade.GetRealizedPnL();
            g_runContext.totalPnlDollars += realizedPnl;
            g_runContext.tradePnls.push_back(realizedPnl);  // accumulate for sharpe_like
            if (realizedPnl > 0.0) {
                g_runContext.grossProfitDollars += realizedPnl;
                g_runContext.winningTrades += 1;
            } else if (realizedPnl < 0.0) {
                g_runContext.grossLossDollarsAbs += std::abs(realizedPnl);
            }
            g_runContext.cumulativeEquity += realizedPnl;
            g_runContext.peakEquity = std::max(g_runContext.peakEquity, g_runContext.cumulativeEquity);
            const double drawdown = g_runContext.peakEquity - g_runContext.cumulativeEquity;
            g_runContext.maxDrawdown = std::max(g_runContext.maxDrawdown, drawdown);

            // Accumulate per-trade tracking vectors for RunSummary averages
            g_runContext.tradeMaeTicks.push_back(static_cast<float>(trade.GetMAETicks()));
            g_runContext.tradeMfeTicks.push_back(static_cast<float>(trade.GetMFETicks()));
            g_runContext.tradeDurationBars.push_back(sc.Index - g_entryContext.entryBarIndex);
            {
                const double rStop = std::fabs(trade.GetEntryPrice() - static_cast<double>(g_entryContext.initialStopPrice));
                const double rPnl  = (trade.GetSide() == TradeSideEnum::LONG)
                    ? (trade.GetExitPrice() - trade.GetEntryPrice())
                    : (trade.GetEntryPrice() - trade.GetExitPrice());
                g_runContext.tradeRealizedRrrValues.push_back((rStop > 0.0) ? static_cast<float>(rPnl / rStop) : 0.0f);
            }

            // Log progress every 10 trades (post-increment value).
            const int newTradeCount = tradeCount + 1;
            if (newTradeCount % 10 == 0) {
                Logger::getInstance().log("BackTester: Recorded " + std::to_string(newTradeCount) + " trades");
            }

            // Periodic flush: protect against data loss on crash (every 50 trades).
            if (g_runContext.tradeSeq % 50 == 0 && g_backtestBinaryStream.is_open()) {
                g_backtestBinaryStream.flush();
            }
        }

    } catch (const std::exception& e) {
        Logger::getInstance().log("RecordTradeExit CRITICAL Exception: " + std::string(e.what()));
        Logger::getInstance().log("Failed to record trade #" + std::to_string(g_entryContext.tradeID));
    }

    // Always clear entry context, even on error
    g_entryContext = TradeEntryContext();
}
