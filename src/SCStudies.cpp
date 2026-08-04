#include "MindfulTrader_Precompiled.h"
#include "AIConnectionMonitor.h"
#include "transport/AIHeartbeatMonitor.h"
#include "SystemOrchestrator.h"

SCDLLName("Mindful Trader - Version 2.0 Devel")

// SCStudies-specific constants
namespace {
    // Menu and UI persistent IDs
    constexpr int MENU_ITEM_EXISTS_ID = 10;
    constexpr int IDX_INPUT_DEBUG_MTS = 13;
    constexpr int IDX_INPUT_ENABLE_REAL_VOLUME_PROFILE_DAILY_BIAS = 14;
    constexpr int VALIDATE_TRADE_MENU_ID = 22;
    constexpr int RISK_DASHBOARD_MENU_ID = 23;
    constexpr int STRUCT_BAR_INDEX_ID = 40; // Elite v3.0: Track last structure update

    // Risk Dashboard Thresholds (visualization only, not business logic)
    constexpr double DAILY_LOSS_LIMIT = 0.02;       // 2% max daily loss (for budget calculation)
    constexpr double BUDGET_BONUS_CAP = 5.0;        // 5% bonus cap for profitability display

    // Dashboard Color Thresholds
    constexpr double BUDGET_PRISTINE_THRESHOLD = 95.0;   // Green above this
    constexpr double BUDGET_CAUTION_THRESHOLD = 80.0;    // Yellow above this, red below
    constexpr double QUALITY_ELITE_THRESHOLD = 80.0;     // Green above this
    constexpr double QUALITY_GOOD_THRESHOLD = 60.0;      // Gold above this, orange below
}

// === AI CONNECTION MONITORING (Port 5559) ===
// AIConnectionMonitor and AIHeartbeatMonitor both use singleton pattern via Instance()
// GUI connection is coordinated by Python SystemOrchestrator via CONFIG_REQ/CONFIG_ACK on port 5560

SCSFExport scsf_MindfulTrader(SCStudyInterfaceRef sc)
{
    if (sc.SetDefaults)
    {
        sc.GraphName = "Mindful Trading System";
        sc.StudyDescription = "Mindful Trading System controller";
        // Runs on the TS3 (15-minute) chart — inherits chart's native timeframe.
        // All ContextManager updates (physics, structure, HMM) operate in 15-min context.
        sc.FreeDLL = 0;
        sc.CalculationPrecedence = LOW_PREC_LEVEL;
        sc.AutoLoop = 1;
        sc.GraphRegion = 1;  // Separate region below price chart

        sc.Input[IDX_INPUT_DEBUG_MTS].Name = "Debug MTS";
        sc.Input[IDX_INPUT_DEBUG_MTS].SetYesNo(0);

        // Train/live parity gate (docs/superpowers/plans/2026-08-04-volume-profile-daily-bias.md
        // final-review fix wave): OFF by default. The currently-deployed HMM was fitted on the old
        // range-split proxy's daily_bias semantics; do not enable until a coordinated retrain on the
        // real Value Area's distribution has shipped on the Python side.
        sc.Input[IDX_INPUT_ENABLE_REAL_VOLUME_PROFILE_DAILY_BIAS].Name =
            "Enable Real Volume Profile Daily Bias (EXPERIMENTAL - requires HMM retrain, see plan doc)";
        sc.Input[IDX_INPUT_ENABLE_REAL_VOLUME_PROFILE_DAILY_BIAS].SetYesNo(0);

        // Chart-level flag required for VolumeAtPriceForBars aggregation in IndicatorManager::UpdateDailyCache.
        // Runs on the same TS3 15-minute chart as this study (see comment above), so setting it here is
        // equivalent to setting it in any other study on this chart.
        sc.MaintainVolumeAtPriceData = 1;

        // Psychological Dashboard - No Dollar Amounts Visible
        // All in same region (1) below price chart
        sc.Subgraph[0].Name = "Risk Budget Remaining (%)";
        sc.Subgraph[0].DrawStyle = DRAWSTYLE_LINE;
        sc.Subgraph[0].PrimaryColor = RGB(0, 255, 0);  // Green = safe
        sc.Subgraph[0].LineWidth = 2;
        sc.Subgraph[0].DrawZeros = false;
        sc.Subgraph[0].GraphicalDisplacement = 0;

        sc.Subgraph[1].Name = "Net Ticks Today";
        sc.Subgraph[1].DrawStyle = DRAWSTYLE_BAR;
        sc.Subgraph[1].PrimaryColor = RGB(100, 149, 237);  // Cornflower blue
        sc.Subgraph[1].SecondaryColor = RGB(220, 20, 60);  // Crimson for negative
        sc.Subgraph[1].DrawZeros = true;
        sc.Subgraph[1].GraphicalDisplacement = 0;

        sc.Subgraph[2].Name = "Trade Quality Score (0-100)";
        sc.Subgraph[2].DrawStyle = DRAWSTYLE_LINE;
        sc.Subgraph[2].PrimaryColor = RGB(255, 215, 0);  // Gold
        sc.Subgraph[2].LineWidth = 2;
        sc.Subgraph[2].DrawZeros = false;
        sc.Subgraph[2].GraphicalDisplacement = 0;

        // NN Trade Signal Dashboard
        sc.Subgraph[3].Name = "NN Signal (-1=SHORT, 0=HOLD, 1=LONG)";
        sc.Subgraph[3].DrawStyle = DRAWSTYLE_STAIR_STEP;
        sc.Subgraph[3].PrimaryColor = RGB(255, 255, 255);  // White
        sc.Subgraph[3].LineWidth = 3;
        sc.Subgraph[3].DrawZeros = true;
        sc.Subgraph[3].GraphicalDisplacement = 0;

        sc.Subgraph[4].Name = "Entry Price";
        sc.Subgraph[4].DrawStyle = DRAWSTYLE_DASH;
        sc.Subgraph[4].PrimaryColor = RGB(0, 255, 255);  // Cyan
        sc.Subgraph[4].LineWidth = 2;
        sc.Subgraph[4].DrawZeros = false;
        sc.Subgraph[4].GraphicalDisplacement = 0;

        sc.Subgraph[5].Name = "Stop Price";
        sc.Subgraph[5].DrawStyle = DRAWSTYLE_DASH;
        sc.Subgraph[5].PrimaryColor = RGB(255, 0, 0);  // Red
        sc.Subgraph[5].LineWidth = 2;
        sc.Subgraph[5].DrawZeros = false;
        sc.Subgraph[5].GraphicalDisplacement = 0;

        sc.Subgraph[6].Name = "Target Price";
        sc.Subgraph[6].DrawStyle = DRAWSTYLE_DASH;
        sc.Subgraph[6].PrimaryColor = RGB(0, 255, 0);  // Green
        sc.Subgraph[6].LineWidth = 2;
        sc.Subgraph[6].DrawZeros = false;
        sc.Subgraph[6].GraphicalDisplacement = 0;

        sc.Subgraph[7].Name = "Validation Status (0=REJECT, 1=ALLOWED)";
        sc.Subgraph[7].DrawStyle = DRAWSTYLE_STAIR_STEP;
        sc.Subgraph[7].PrimaryColor = RGB(255, 165, 0);  // Orange
        sc.Subgraph[7].LineWidth = 2;
        sc.Subgraph[7].DrawZeros = true;
        sc.Subgraph[7].GraphicalDisplacement = 0;

        sc.AllowMultipleEntriesInSameDirection = false;
        sc.MaximumPositionAllowed = 100;
        sc.SupportReversals = false;
        sc.AllowOppositeEntryWithOpposingPositionOrOrders = false;
        sc.SupportAttachedOrdersForTrading = true;
        sc.AllowEntryWithWorkingOrders = false;
        sc.AllowOnlyOneTradePerBar = true;
        sc.MaintainTradeStatisticsAndTradesData = true;

        return;
    }

    // Dashboard subgraphs have no warmup requirement, but skip bar 0 for clean display.
    sc.DataStartIndex = 1;

    // This block runs only once on initial chart load or full recalculation for GUI initialization
    if (sc.UpdateStartIndex == 0)
    {
        // One-time initialization for all menu items
        if (sc.GetPersistentInt(MENU_ITEM_EXISTS_ID) == 0) {
            // Risk Manager menu items (GUI connection now controlled by Python)
            sc.SetPersistentInt(VALIDATE_TRADE_MENU_ID, sc.AddACSChartShortcutMenuItem(sc.ChartNumber, "✓ Validate Trade Idea"));
            sc.SetPersistentInt(RISK_DASHBOARD_MENU_ID, sc.AddACSChartShortcutMenuItem(sc.ChartNumber, "📊 Show Risk Status"));

            sc.SetPersistentInt(MENU_ITEM_EXISTS_ID, 1); // Mark all menu items as created
        }
        const auto requestQueue = std::make_shared<ThreadSafeQueue<TradeRequest>>();
        const auto replyQueue = std::make_shared<ThreadSafeQueue<TradeReply>>();

        // Elite v2.4: Removed all queue infrastructure - uses TransportStream directly
        PositionManager::Instance().Init(sc, requestQueue, replyQueue);

        // Initialize RiskManager with cached invariants (contract specs, session start balance)
        RiskManager::Instance().Init(sc);

        // Elite v3.0: Reset ContextManager (StructureEngine, etc.)
        ContextManager::Instance().Reset();
        sc.SetPersistentInt(STRUCT_BAR_INDEX_ID, -1);

        // HMMClient lifecycle is owned by SystemOrchestrator to avoid split init/shutdown ownership.

        // === INITIALIZE AI HEARTBEAT MONITORING (Port 5559) ===
        AIHeartbeatMonitor::Instance().Init(AIConnectionMonitor::Instance());
        AIHeartbeatMonitor::Instance().Start();  // Start SUB socket listener for AI heartbeats
        Logger::getInstance().log("[SCStudies] AIHeartbeatMonitor started");

        // === INITIALIZE SYSTEM ORCHESTRATOR (Port 5560 Master Controller) ===
        try {
            Logger::getInstance().log("[SCStudies] SystemOrchestrator::Initialize()...");
            if (SystemOrchestrator::Instance().Initialize()) {
                Logger::getInstance().log("SystemOrchestrator initialized on port 5560");
                // Watchdog thread already started by Initialize()
            } else {
                Logger::getInstance().log("❌ SystemOrchestrator failed to initialize");
            }
        } catch (const std::exception& e) {
            Logger::getInstance().log("[SCStudies] EXCEPTION initializing SystemOrchestrator: " + std::string(e.what()));
        } catch (...) {
            Logger::getInstance().log("[SCStudies] UNKNOWN EXCEPTION initializing SystemOrchestrator");
        }

        // Dedicated trade RPC endpoint (port 5558) - separate from 5560 control-plane.
        try {
            if (TradeExecutionServer::Instance().Initialize()) {
                Logger::getInstance().log("✅ TradeExecutionServer initialized on port 5558");
            } else {
                Logger::getInstance().log("❌ TradeExecutionServer failed to initialize on port 5558");
            }
        } catch (const std::exception& e) {
            Logger::getInstance().log("[SCStudies] EXCEPTION initializing TradeExecutionServer: " + std::string(e.what()));
        } catch (...) {
            Logger::getInstance().log("[SCStudies] UNKNOWN EXCEPTION initializing TradeExecutionServer");
        }

        // Note: Python-side orchestrator initializes via CONFIG_REQ/CONFIG_ACK
        // on the control channel (legacy REQUEST_INIT_SEQUENCE path removed).
    }

    if (sc.LastCallToFunction)
    {
        // === GENERATE TCA REPORT (Step 1.7) ===
        AIConnectionMonitor::Instance().GenerateTCAReport(sc);
        AIConnectionMonitor::Instance().GenerateSessionSummary(sc);

        // Elite: Clear HUD drawings before shutdown
        for (int lineNum = 10001; lineNum <= 10005; ++lineNum) {
            sc.DeleteACSChartDrawing(sc.ChartNumber, TOOL_DELETE_CHARTDRAWING, lineNum);
        }

        // Elite: Shutdown order matters - dependents first, providers last
        AIHeartbeatMonitor::Instance().Shutdown();  // Depends on SystemOrchestrator
        SystemOrchestrator::Instance().Shutdown();  // Provider of heartbeat state

        // Elite v2.4: All messaging now uses centralized infrastructure
        TransportStream::Instance().Shutdown();  // Centralized PUB socket (replaces MindfulSocketZMQ)
        sc.RemoveACSChartShortcutMenuItem(sc.ChartNumber, sc.GetPersistentInt(VALIDATE_TRADE_MENU_ID));
        sc.RemoveACSChartShortcutMenuItem(sc.ChartNumber, sc.GetPersistentInt(RISK_DASHBOARD_MENU_ID));
        TradeExecutionServer::Instance().Shutdown();
        return;
    }

    if (sc.MenuEventID != 0)
    {
        if (sc.MenuEventID == sc.GetPersistentInt(VALIDATE_TRADE_MENU_ID)) {
            // Validate Trade - prompts for entry/stop/quantity
            Logger::getInstance().log("=== TRADE VALIDATOR ===");
            Logger::getInstance().log("Enter trade details in format: ENTRY,STOP,QTY");
            Logger::getInstance().log("Example: 4500.25,4495.00,2");
            Logger::getInstance().log("Then check log for validation result");

            // For now, use current price as example
            const double currentPrice = sc.Close[sc.Index];
            const double exampleStop = currentPrice - (5.0 * sc.TickSize); // 5 ticks below
            constexpr int exampleQty = 1;

            RiskManager::Instance().LogTradeValidation(sc, currentPrice, exampleStop, exampleQty, "Example Long Trade");
        }
        else if (sc.MenuEventID == sc.GetPersistentInt(RISK_DASHBOARD_MENU_ID)) {
            // Display current risk status
            constexpr double kMinDenominator = 1e-9;
            const double dailyPnL = RiskManager::Instance().GetDailyPnL();
            const double accountEquity = RiskManager::Instance().GetAccountEquity(sc);
            const double totalExposure = RiskManager::Instance().CalculateTotalExposure(sc);
            const int consecutiveLosses = RiskManager::Instance().GetConsecutiveLosses(sc);
            const bool isHalted = RiskManager::Instance().IsTradingHalted(sc);

            auto& log = Logger::getInstance();
            log.log("========== RISK DASHBOARD ==========");

            SCString msg;
            msg.Format("Account Equity: $%.2f", accountEquity);
            log.log(msg.GetChars());

            const double dailyPnlPct = (std::abs(accountEquity) > kMinDenominator)
                ? ((dailyPnL / accountEquity) * 100.0)
                : 0.0;
            msg.Format("Daily P&L: $%.2f (%.2f%%)", dailyPnL, dailyPnlPct);
            log.log(msg.GetChars());

            const double exposurePct = (std::abs(accountEquity) > kMinDenominator)
                ? ((totalExposure / accountEquity) * 100.0)
                : 0.0;
            msg.Format("Current Exposure: $%.2f (%.2f%%)", totalExposure, exposurePct);
            log.log(msg.GetChars());

            msg.Format("Consecutive Losses: %d / %d max", consecutiveLosses, 2);
            log.log(msg.GetChars());

            msg.Format("Trading Status: %s", isHalted ? "HALTED" : "ACTIVE");
            log.log(msg.GetChars());

            // Risk limits status
            double maxDailyLoss = accountEquity * 0.02;
            double remainingDailyLoss = maxDailyLoss + dailyPnL;
            msg.Format("Daily Loss Limit: $%.2f remaining of $%.2f", remainingDailyLoss, maxDailyLoss);
            log.log(msg.GetChars());

            double maxExposure = accountEquity * 0.06;
            double remainingExposure = maxExposure - totalExposure;
            msg.Format("Portfolio Heat Limit: $%.2f remaining of $%.2f", remainingExposure, maxExposure);
            log.log(msg.GetChars());

            log.log("====================================");
        }
    }

    // --- Production Trading Logic ---
    try {
        // === CONTEXT & PHYSICS UPDATE (Must run on history to warm up engines) ===
        // Tick-driven: feed log returns to InformationEngine (LZ) and
        // TailRiskEngine (Hill) on every price change.
        // During historical load: one return per completed bar.
        // During live: one return per tick-level price movement.
        {
            static float s_lastPhysicsPrice = 0.0f;
            if (sc.Index == 0) s_lastPhysicsPrice = 0.0f;

            float currentPrice = sc.Close[sc.Index];
            if (currentPrice != s_lastPhysicsPrice) {
                if (s_lastPhysicsPrice > 0.0001f && currentPrice > 0.0001f) {
                    float logReturn = std::log(currentPrice / s_lastPhysicsPrice);
                    ContextManager::Instance().UpdateMarketPhysics(logReturn);
                }
                s_lastPhysicsPrice = currentPrice;
            }
        }

        // Update TimeOfDay indicator based on current bar time (needed for history)
        const auto timeOfDayIndicator = IndicatorManager::Instance().GetIndicator<TimeOfDayIndicator>(IndicatorKey::TIME_OF_DAY);
        if (timeOfDayIndicator) {
            timeOfDayIndicator->SetFromDateTime(sc.BaseDateTimeIn[sc.Index]);
        }

        // Update bar-level indicator context (daily cache, warmup, temporal counters).
        // Must run during history to prime DailyHighLowLoader and CheckWarmupStatus.
        // Mirrors EventDataCollectorStudy for path consistency.
        IndicatorManager::Instance().SetRealVolumeProfileDailyBiasEnabled(
            sc.Input[IDX_INPUT_ENABLE_REAL_VOLUME_PROFILE_DAILY_BIAS].GetYesNo() != 0);
        IndicatorManager::Instance().UpdateBarContext(sc);

        // === FULL RECALCULATION / HISTORICAL DOWNLOAD GUARD ===
        // During Sierra Chart historical load or data download backfill:
        // skip trading logic, event publishing, and ordering.
        // Warm-up (physics, indicators, TimeOfDay) runs above this gate.
        if (sc.IsFullRecalculation || sc.DownloadingHistoricalData) {
            return;
        }

        // === ELITE GAP 6: HANDSHAKE BLOCKING - Prevent trading until Python ready ===
        // Keep warmup updates above this gate so context features do not cold-start.
        static bool bootstrapSnapshotPublished = false;
        if (!SystemOrchestrator::Instance().IsHandshakeComplete()) {
            // Re-arm bootstrap snapshot for reconnect cycles.
            bootstrapSnapshotPublished = false;
            return;  // Block trading/execution/event publishing until handshake completes.
        }

        // === ELITE: CHECK HANDSHAKE STATUS (NON-BLOCKING) ===
        // SystemOrchestrator runs handshake in background thread
        // Main thread just checks atomic flag - NEVER blocks
        static bool handshakeLogged = false;
        if (!handshakeLogged) {
            Logger::getInstance().log("✅ Python handshake complete - live trading ENABLED");
            handshakeLogged = true;
        }

        if (!bootstrapSnapshotPublished) {
            IndicatorManager::Instance().PublishEventSnapshot(sc);
            PositionManager::Instance().PublishSnapshot(sc);
            Logger::getInstance().log("✅ Post-handshake bootstrap snapshot published (full state)");
            bootstrapSnapshotPublished = true;
        }

        // === TRADING MODE (Live Trading - IsFullRecalculation == 0) ===

        // Elite: Heartbeat watchdog (check for stale connections)
        SystemOrchestrator::Instance().CheckLiveness();

        // === AI CONNECTION HEALTH CHECK (runs every update) ===
        AIConnectionMonitor::AIHealthStatus aiHealth = AIConnectionMonitor::Instance().CheckSystemIntegrity(sc);

        // Track purge state with persistent flag
        static bool lastWasDisconnected = false;

        if (aiHealth == AIConnectionMonitor::DISCONNECTED) {
            // AI disconnected - EMERGENCY FLATTEN (Elite GAP 5)
            if (!lastWasDisconnected) {
                PositionManager::Instance().EmergencyFlattenPosition(sc, "AI DISCONNECTED");
                RiskManager::Instance().EmergencyHalt(sc, "AI system disconnected");
                lastWasDisconnected = true;
            }
            // No new AI signals will be accepted until CONNECTED again
            return;
        } else {
            // AI reconnected or still connected - reset purge flag
            if (lastWasDisconnected) {
                Logger::getInstance().log("AI RECONNECTED - Resuming normal operations");
                lastWasDisconnected = false;

                // === ELITE REFINEMENT 2: POSITION SYNC ON RECONNECT ===
                // Send explicit position state to GUI to prevent stale position display
                PositionManager::Instance().PublishPositionSync(sc);
            }
        }

        // === MODEL HEALTH STATUS CHECK (Step 1.6 - runs every bar, cached 60s) ===
        AIConnectionMonitor::ModelHealthStatus modelHealth = AIConnectionMonitor::Instance().CheckModelHealthStatus(sc);

        // Track model health state transitions
        static AIConnectionMonitor::ModelHealthStatus lastModelHealth = AIConnectionMonitor::ModelHealthStatus::HEALTHY;

        if (modelHealth == AIConnectionMonitor::ModelHealthStatus::SOFT_LOCKED) {
            // Model performance critically degraded - EMERGENCY FLATTEN (Elite GAP 5)
            if (lastModelHealth != AIConnectionMonitor::ModelHealthStatus::SOFT_LOCKED) {
                PositionManager::Instance().EmergencyFlattenPosition(sc, "MODEL SOFT_LOCKED");
                RiskManager::Instance().EmergencyHalt(sc, "Model performance critically degraded");
            }
            // No new AI signals will be accepted until model health improves
            lastModelHealth = modelHealth;
            return;
        } else if (modelHealth == AIConnectionMonitor::ModelHealthStatus::WARNING) {
            // Model degraded - continue but with stricter criteria (handled in signal acceptance)
            lastModelHealth = modelHealth;
        } else {
            // Model HEALTHY - track state change
            lastModelHealth = modelHealth;
        }

        // Risk management continuous monitoring (CRITICAL - runs every bar)
        RiskManager::Instance().Update(sc);

        // === CONTEXT MANAGER: Update Structure (Elite v3.0) ===
        // Must be called BEFORE CheckAndTriggerHMM to ensure structure metrics are current
        int lastStructIdx = sc.GetPersistentInt(STRUCT_BAR_INDEX_ID);
        bool isStructureNewBar = (sc.Index != lastStructIdx);
        ContextManager::Instance().UpdatePriceStructure(
            sc,
            sc.High[sc.Index],
            sc.Low[sc.Index],
            sc.Close[sc.Index],
            isStructureNewBar
        );
        if (isStructureNewBar) {
            sc.SetPersistentInt(STRUCT_BAR_INDEX_ID, sc.Index);
        }

        // === CONTEXT MANAGER: Update HMM every bar ===
        uint64_t now_us = sc.GetCurrentDateTime().ToUNIXTimeInMicroseconds();
        ContextManager::Instance().CheckAndTriggerHMM(now_us, false, -1.0f, IsPostWeekendReopenGracePeriod(sc));

        // PositionManager gates manual intents against LocalRiskContext freshness.
        // Run it after current-tick ContextManager updates so manual orders do not
        // evaluate against the previous tick's snapshot timestamp.
        PositionManager::Instance().Update(sc);

        const auto sideIndicator = IndicatorManager::Instance().GetIndicator<Side>(IndicatorKey::SIDE);
        if (sideIndicator) {
            sideIndicator->Update(PositionManager::Instance().GetTradeSide());
        }

        // === Elite v2.4 EVENT-DRIVEN: Publish FlatBuffer Event on indicator change ===
        // Note: Connection now controlled by Python via CONFIG_REQ/CONFIG_ACK
        // C++ publishes events continuously (port 5555)
        bool eventPublished = IndicatorManager::Instance().PublishEventOnChange(sc);

        if (eventPublished) {
            // Event sent - also publish position snapshot for complete state
            PositionManager::Instance().PublishSnapshot(sc);
        }

        // ====== UPDATE MARKET CONTEXT FOR TRADE EXECUTION SERVER ======
        // TradeExecutionServer calculates ATR and swing points internally
        TradeExecutionServer::Instance().UpdateMarketContext(sc);

        // ====== PSYCHOLOGICAL DASHBOARD UPDATE (runs every bar) ======
        // Refresh RiskManager cache once per bar (only queries broker if position is open)
        RiskManager::Instance().RefreshMetrics(sc);

        // Calculate metrics but hide dollar amounts from trader
        // Use cached values (no broker API calls)

        // 1. Risk Budget Remaining (%) - starts at 100%, decreases with losses
        constexpr double kMinDenominator = 1e-9;
        const double dailyPnL = RiskManager::Instance().GetDailyPnL();
        const double accountEquity = RiskManager::Instance().GetAccountEquity(sc);
        const double maxDailyLoss = accountEquity * DAILY_LOSS_LIMIT;
        double budgetRemaining = 100.0;
        const bool hasValidLossBudget = (std::abs(maxDailyLoss) > kMinDenominator);

        if (dailyPnL < 0) [[likely]] {
            // Consumed some of our 2% daily allowance
            const double lossConsumed = fabs(dailyPnL);
            budgetRemaining = hasValidLossBudget
                ? (100.0 * (1.0 - (lossConsumed / maxDailyLoss)))
                : 0.0;
            if (budgetRemaining < 0) budgetRemaining = 0.0;
        } else if (dailyPnL > 0) {
            // Small bonus for being profitable (caps at 105%)
            budgetRemaining = hasValidLossBudget
                ? (100.0 + (BUDGET_BONUS_CAP * std::min(dailyPnL / maxDailyLoss, 1.0)))
                : 100.0;
        }

        // Color-code based on budget remaining
        if (budgetRemaining >= BUDGET_PRISTINE_THRESHOLD) {
            sc.Subgraph[0].PrimaryColor = RGB(0, 255, 0);  // Green - pristine
        } else if (budgetRemaining >= BUDGET_CAUTION_THRESHOLD) {
            sc.Subgraph[0].PrimaryColor = RGB(255, 255, 0);  // Yellow - caution
        } else {
            sc.Subgraph[0].PrimaryColor = RGB(255, 0, 0);  // Red - danger
        }
        sc.Subgraph[0][sc.Index] = static_cast<float>(budgetRemaining);

        // 2. Net Ticks Today - pure performance metric, no $ signs
        const double netTicks = RiskManager::Instance().GetNetTicksToday(sc);
        sc.Subgraph[1][sc.Index] = static_cast<float>(netTicks);

        // 3. Trade Quality Score - rewards execution over outcome
        const double qualityScore = RiskManager::Instance().GetTradeQualityScore(sc);

        // Color-code quality
        if (qualityScore >= QUALITY_ELITE_THRESHOLD) {
            sc.Subgraph[2].PrimaryColor = RGB(0, 255, 0);  // Green - elite
        } else if (qualityScore >= QUALITY_GOOD_THRESHOLD) {
            sc.Subgraph[2].PrimaryColor = RGB(255, 215, 0);  // Gold - good
        } else {
            sc.Subgraph[2].PrimaryColor = RGB(255, 165, 0);  // Orange - needs work
        }
        sc.Subgraph[2][sc.Index] = static_cast<float>(qualityScore);

        // ====== NN TRADE SIGNAL DASHBOARD UPDATE ======
        // Display latest trade signal from Python NN and validation result

        if (TradeSignalManager::Instance().HasFreshSignal()) {
            const TradeValidationParams& signal = TradeSignalManager::Instance().GetTradeSignal();
            TradeValidationResult result;

            // === LAYER 1: MODEL HEALTH FILTER (Step 1.6) ===
            // Reject all signals if model is SOFT_LOCKED
            if (modelHealth == AIConnectionMonitor::ModelHealthStatus::SOFT_LOCKED) {
                result.allowed = false;
                result.reason = "Model SOFT_LOCKED (alpha slippage >=30%)";
                TradeSignalManager::Instance().SetValidationResult(result);
                Logger::getInstance().log("NN Signal REJECTED: Model SOFT_LOCKED");
                TradeSignalManager::Instance().MarkSignalProcessed();
                return;
            }

            // TODO: When confidence is added to TradeValidationParams:
            // if (modelHealth == ModelHealthStatus::WARNING) {
            //     if (signal.confidence < 0.70f) {
            //         result.reason = "Model WARNING - confidence too low (<0.70)";
            //         TradeSignalManager::Instance().SetValidationResult(result);
            //         TradeSignalManager::Instance().MarkSignalProcessed();
            //         return;
            //     }
            // }

            // === LAYER 2: RISK MANAGER VALIDATION ===
            // Validate the signal using RiskManager
            const bool isAllowed = RiskManager::Instance().IsTradeAllowed(sc, signal, result);
            TradeSignalManager::Instance().SetValidationResult(result);

            // Determine NN signal direction from entry rule
            // Note: Python LiveAgent should include explicit TradeActionEnum in signal
            const float nnSignal = (signal.entryRule == EntryRuleType::Breakout) ? 1.0f :
                                   (signal.entryRule == EntryRuleType::Pullback) ? -1.0f :
                                   0.0f;  // HOLD by default

            sc.Subgraph[3][sc.Index] = nnSignal;
            sc.Subgraph[4][sc.Index] = result.entryPrice;
            sc.Subgraph[5][sc.Index] = result.stopPrice;
            sc.Subgraph[6][sc.Index] = result.targetPrice;
            sc.Subgraph[7][sc.Index] = isAllowed ? 1.0f : 0.0f;

            // Log validation result to user-facing message log
            if (isAllowed) {
                Logger::getInstance().log("NN Signal VALIDATED");
            } else {
                Logger::getInstance().log("NN Signal REJECTED: " + std::string(result.reason));
            }

            TradeSignalManager::Instance().MarkSignalProcessed();
        } else {
            // No fresh signal - maintain last values or zero out
            if (sc.Index > 0) {
                sc.Subgraph[3][sc.Index] = sc.Subgraph[3][sc.Index - 1];
                sc.Subgraph[4][sc.Index] = sc.Subgraph[4][sc.Index - 1];
                sc.Subgraph[5][sc.Index] = sc.Subgraph[5][sc.Index - 1];
                sc.Subgraph[6][sc.Index] = sc.Subgraph[6][sc.Index - 1];
                sc.Subgraph[7][sc.Index] = sc.Subgraph[7][sc.Index - 1];
            } else {
                sc.Subgraph[3][sc.Index] = 0.0f;
                sc.Subgraph[4][sc.Index] = 0.0f;
                sc.Subgraph[5][sc.Index] = 0.0f;
                sc.Subgraph[6][sc.Index] = 0.0f;
                sc.Subgraph[7][sc.Index] = 0.0f;
            }
        }

        // Elite: Draw institutional HUD (lock-free atomic reads)
        SystemOrchestrator::Instance().DrawHUD(sc);
    }
    catch (const std::exception& e) {
        Logger::getInstance().log("Exception in scsf_MindfulTrader: " + std::string(e.what()));
    }
    catch (...)
    {
        Logger::getInstance().log("Unknown exception in scsf_MindfulTrader");
    }
}

