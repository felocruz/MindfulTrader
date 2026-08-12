#include "MindfulTrader_Precompiled.h"
#include "LBRFileManager.h"
#include "TradeSignalManager.h" // ELITE: Access to model confidence
#include "Logger.h"

// Persistent IDs
namespace {
    constexpr int EDC_EXPORT_ARMED_FLAG_ID = 50;
    constexpr int EDC_ARM_MENU_ID = 51;
    constexpr int EDC_DISARM_MENU_ID = 52;
    [[maybe_unused]] constexpr int EDC_FILE_OPENED_FLAG_ID = 53;
    constexpr int EDC_EVENT_COUNT_ID = 54;
    constexpr int EDC_LOCK_A_BLOCK_COUNT_ID = 55;  // Observation saturation not ready
    constexpr int EDC_LOCK_B_BLOCK_COUNT_ID = 56;  // Indicator warm-up not ready
    constexpr int EDC_LAST_TELEMETRY_EVENT_COUNT_ID = 57;
    constexpr int EDC_ALPHA_COLLECTION_ACTIVE_ID = 58;
    constexpr int EDC_LOCK_C_BLOCK_COUNT_ID = 59;      // Regime certainty not stable
    constexpr int EDC_LOCK_C_STABLE_COUNT_ID = 60;     // Consecutive stable samples
    constexpr int EDC_LOCK_C_UNSTABLE_COUNT_ID = 61;   // Consecutive unstable samples while active
    constexpr int EDC_LOCK_C_READY_FLAG_ID = 62;       // Hysteresis latch
    constexpr int EDC_LOCK_D_BLOCK_COUNT_ID = 70;      // TS1 freshness lock not ready
    constexpr int EDC_LOCK_D_STALE_STREAK_ID = 71;     // Consecutive stale TS1 checks
    constexpr int EDC_LOCK_E_BLOCK_COUNT_ID = 72;      // TS2 structural freshness lock not ready

    // Lock C hysteresis thresholds (Schmidt Trigger on rank-percentile scaled values)
    // Post-FeatureScaler [0,1] rank thresholds — distribution-agnostic.
    // Enter: dim is below median of recent history (regime is certain / tails are calm)
    // Exit:  dim has risen well above median (uncertainty / fat tails growing)
    constexpr float LOCK_C_ENTROPY_ENTER = 0.50f;
    constexpr float LOCK_C_ENTROPY_EXIT = 0.65f;
    constexpr float LOCK_C_KURTOSIS_ENTER = 0.50f;
    constexpr float LOCK_C_KURTOSIS_EXIT = 0.65f;
    constexpr int LOCK_C_STABLE_REQUIRED = 20;
    constexpr int LOCK_C_UNSTABLE_REQUIRED = 8;

    // Lock D: TS1 macro freshness guard.
    // TS1 updates on 240-min chart; allow one bar plus replay jitter before stale.
    constexpr uint64_t LOCK_D_TS1_MAX_AGE_US = 6ULL * 60ULL * 60ULL * 1000000ULL; // 6h
    constexpr int LOCK_D_CIRCUIT_BREAKER_STALE = 200;

    // Lock E: TS2 structural ownership guard (fractal/recurrence freshness).
    // TS2 updates on 60-min chart; allow one bar plus replay jitter before stale.
    constexpr uint64_t LOCK_E_TS2_MAX_AGE_US = 3ULL * 60ULL * 60ULL * 1000000ULL; // 3h

    // Synthetic velocity EMA state
    constexpr int EDC_VELOCITY_EMA_ID = 63;         // Persistent float: 5-period EMA of trade rate
    constexpr int EDC_VELOCITY_EMA_INIT_ID = 64;    // Persistent int:   EMA initialized flag
    constexpr float VELOCITY_EMA_ALPHA = 2.0f / (5.0f + 1.0f);  // 5-period EMA: α = 0.333

    // Structure tracking (mirrors SCStudies STRUCT_BAR_INDEX_ID)
    constexpr int EDC_STRUCT_BAR_INDEX_ID = 65;

    // Preflight tracking
    constexpr int EDC_PREFLIGHT_PASSED_ID = 66;    // 1 if preflight passed this session

    // Diagnostic counters (logged on disarm)
    constexpr int EDC_SIGNIFICANT_COUNT_ID = 67;   // HasSignificantChange() == true
    constexpr int EDC_NULL_OBS_SKIP_COUNT_ID = 68;  // observation||asymmetry null → write skipped
    constexpr int EDC_NULL_EVENT_SKIP_COUNT_ID = 69; // GetTrainingEventT returned null

    // Expected bar period for TS3 (15 min = 900 sec)
    constexpr int EXPECTED_SECONDS_PER_BAR = 900;

    // CME Globex ES session times — Eastern Time (NYC).
    // Full session: 18:00 ET (Sun–Thu) → 17:00 ET (Mon–Fri).
    // Daily maintenance halt 17:00–18:00 ET has no data (natural gap).
    // SCDateTime TimeValue is seconds since midnight.
    constexpr int CME_ES_SESSION_START_SECS = 18 * 3600;  // 18:00 ET = 64800
    constexpr int CME_ES_SESSION_END_SECS   = 17 * 3600;  // 17:00 ET = 61200

    // Minimum historical days required for warm-up and data collection.
    // Physics engines, FeatureScaler rank buffers, and indicators all need
    // a ramp-up period.  365 days covers most replay scenarios and prevents
    // accidental data loss from "Delete All Data And Download" with a tiny
    // Days to Load value.
    constexpr int MIN_DAYS_TO_LOAD = 365;
}

static std::string g_exportFilename;

namespace {

// Crash breadcrumb: 4-byte file overwritten in-place each step.
// Survives SIGSEGV — read after restart to find last successful step:
//   10 = HasSignificantChange true
//   20 = GetTrainingEventT OK
//   30 = AddToTrainingEventFB done
//   40 = Metadata + gap done
//   50 = LogSynchronizedEvent entered
//   60 = LogSynchronizedEvent returned
//   70 = Full write cycle OK
static constexpr const char* BREADCRUMB_PATH = "C:/SierraChart2/Data/edc_breadcrumb.bin";

inline void WriteBreadcrumb(int step) {
    FILE* f = fopen(BREADCRUMB_PATH, "wb");
    if (f) {
        fwrite(&step, sizeof(step), 1, f);
        fclose(f);
    }
}

inline int ReadBreadcrumb() {
    int step = -1;
    FILE* f = fopen(BREADCRUMB_PATH, "rb");
    if (f) {
        fread(&step, sizeof(step), 1, f);
        fclose(f);
    }
    return step;
}

uint64_t GetReplaySafeNowUs(SCStudyInterfaceRef sc)
{
    return sc.GetCurrentDateTime().ToUNIXTimeInMicroseconds();
}

// ---------------------------------------------------------------------------
// PreflightValidation: Runs on arm to catch SC misconfiguration early.
// Returns true if all checks pass.  Logs warnings but DOES NOT block arming
// for soft failures (operator may intentionally override).
// Returns false + logs ERROR only for hard failures that would corrupt data.
// ---------------------------------------------------------------------------
bool PreflightValidation(SCStudyInterfaceRef sc)
{
    bool hardPass = true;

    // --- HARD GATE: Chart must be Intraday ---
    if (sc.ChartDataType != INTRADAY_DATA) {
        Logger::getInstance().log(
            "EDC PREFLIGHT ERROR: Chart is not Intraday. "
            "Data collection requires an Intraday chart.");
        hardPass = false;
    }

    // --- SOFT CHECK: IDSTU should be 1 Tick (value 0) for max fidelity ---
    if (sc.IntradayDataStorageTimeUnit != 0) {
        Logger::getInstance().log(
            "EDC PREFLIGHT WARNING: Intraday Data Storage Time Unit is "
            + std::to_string(sc.IntradayDataStorageTimeUnit)
            + " seconds (expected 0 = 1 Tick). "
            "Price resolution may be reduced.");
    }

    // --- SOFT CHECK: Bar period should be 15 min (TS3) ---
    if (sc.SecondsPerBar != EXPECTED_SECONDS_PER_BAR) {
        Logger::getInstance().log(
            "EDC PREFLIGHT WARNING: SecondsPerBar="
            + std::to_string(sc.SecondsPerBar)
            + " (expected " + std::to_string(EXPECTED_SECONDS_PER_BAR)
            + " for TS3 15-min chart).");
    }

    // --- SOFT CHECK: Replay state ---
    // Operator workflow supports arm-then-run replay. Keep this non-blocking.
    if (sc.ReplayStatus == REPLAY_RUNNING) {
        Logger::getInstance().log(
            "EDC PREFLIGHT: Replay is running. "
            "CUI=" + std::to_string(sc.ChartUpdateIntervalInMilliseconds) + "ms.");
    } else if (sc.ReplayStatus == REPLAY_PAUSED) {
        Logger::getInstance().log(
            "EDC PREFLIGHT: Replay is paused. Resume replay to begin collection.");
    } else {
        Logger::getInstance().log(
            "EDC PREFLIGHT: No replay detected (live or stopped). "
            "CUI=" + std::to_string(sc.ChartUpdateIntervalInMilliseconds) + "ms.");
    }

    // --- REPLAY-READY GATE: keep the chart/session checks observable but non-blocking ---
    // The supported operator flow is arm-then-run replay. We intentionally avoid
    // mutating Start/End or UseSecondStartEndTimes here because those settings can
    // trigger a chart reload and reset TS1 warm-up. The preflight remains logged for
    // operator troubleshooting but must not prevent a valid arm action.
    if (sc.StartTime1 != CME_ES_SESSION_START_SECS ||
        sc.EndTime1 != CME_ES_SESSION_END_SECS ||
        sc.UseSecondStartEndTimes != 0) {
        Logger::getInstance().log(
            "EDC PREFLIGHT WARNING: Session misconfigured. Expected Start=18:00 ET, End=17:00 ET, "
            "Use Evening Session=No. Configure chart sessions first; EDC will not mutate sessions on arm.");
    }

    // --- REPLAY-READY GATE: keep days-to-load visibility without blocking arm ---
    if (sc.DaysToLoadInChart < MIN_DAYS_TO_LOAD) {
        Logger::getInstance().log(
            "EDC PREFLIGHT WARNING: DaysToLoad="
            + std::to_string(sc.DaysToLoadInChart)
            + " (< minimum " + std::to_string(MIN_DAYS_TO_LOAD)
            + "). Data quality will be weaker until replay has sufficient history.");
    }

    // --- OWNERSHIP GATE: log state but do not hard block arm-then-run replay ---
    {
        auto& cm = ContextManager::Instance();
        const uint64_t now_us = GetReplaySafeNowUs(sc);
        const bool weekendGrace = IsPostWeekendReopenGracePeriod(sc);
        const bool ts1Ready = cm.AreTs1DimsReady(now_us, LOCK_D_TS1_MAX_AGE_US, weekendGrace);
        const bool ts2Ready = cm.AreTs2StructuralDimsReady(now_us, LOCK_E_TS2_MAX_AGE_US, weekendGrace);
        const auto& obs = cm.GetObservationData();

        Logger::getInstance().log(
            "EDC PREFLIGHT OWNERSHIP: "
            "ts1_ready=" + std::to_string(ts1Ready ? 1 : 0) +
            " ts1_quality_ready=" + std::to_string(cm.HasTs1QualityReadyAfterReset() ? 1 : 0) +
            " ts1_age_us=" + std::to_string(cm.GetTs1MacroAgeUs(now_us)) +
            " ts1_last_write_us=" + std::to_string(cm.GetTs1MacroLastWriteUs()) +
            " ts1_dim0=" + std::to_string(obs.log_variance_ratio()) +
            " ts1_dim6=" + std::to_string(obs.hurst_exponent()) +
            " ts1_dim8=" + std::to_string(obs.fisher_info()) +
            " ts2_ready=" + std::to_string(ts2Ready ? 1 : 0) +
            " ts2_age_us=" + std::to_string(cm.GetTs2StructuralAgeUs(now_us)) +
            " ts2_last_write_us=" + std::to_string(cm.GetTs2StructuralLastWriteUs()) +
            " ts2_dim13=" + std::to_string(obs.recurrence_rate()) +
            " ts2_dim14=" + std::to_string(obs.fractal_dim()) +
            " weekend_grace=" + std::to_string(weekendGrace ? 1 : 0)
        );

        if (!ts1Ready) {
            Logger::getInstance().log(
                "EDC PREFLIGHT WARNING: TS1 macro ownership not ready. "
                "Replay can still arm; quality-qualified TS1 writes will become available as the chart warms."
            );
        }

        if (!ts2Ready) {
            Logger::getInstance().log(
                "EDC PREFLIGHT WARNING: TS2 structural ownership not ready. "
                "Replay can still arm; TS2 structural writes will populate once the producer refreshes."
            );
        }
    }

    if (hardPass) {
        Logger::getInstance().log(
            "EDC PREFLIGHT: All checks passed. "
            "IDSTU=" + std::to_string(sc.IntradayDataStorageTimeUnit)
            + ", BarPeriod=" + std::to_string(sc.SecondsPerBar) + "s"
            + ", CUI=" + std::to_string(sc.ChartUpdateIntervalInMilliseconds) + "ms");
    }

    return hardPass;
}
}

SCSFExport scsf_EventDataCollector(SCStudyInterfaceRef sc)
{
    if (sc.SetDefaults)
    {
        sc.GraphName = "Event Data Collector - Elite v2.3";
        sc.StudyDescription = "⚡ DUAL-STREAM: Context Snapshots + Indicator Events";
        // Runs on the TS3 (15-minute) chart — inherits chart's native timeframe.
        // All ContextManager updates (physics, structure, HMM) operate in 15-min context.
        // sc.FreeDLL removed — no effect since SC v1836.
        sc.CalculationPrecedence = LOW_PREC_LEVEL;
        sc.AutoLoop = 1;
        sc.GraphRegion = 0;

        sc.Subgraph[0].Name = "Event Count";
        sc.Subgraph[0].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[1].Name = "Log Return";
        sc.Subgraph[1].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[2].Name = "Volatility";
        sc.Subgraph[2].DrawStyle = DRAWSTYLE_IGNORE;
        sc.Subgraph[3].Name = "ATR";
        sc.Subgraph[3].DrawStyle = DRAWSTYLE_IGNORE;
        return;
    }

    // Initialize context menu items
    int& r_ArmMenuID = sc.GetPersistentInt(EDC_ARM_MENU_ID);
    int& r_DisarmMenuID = sc.GetPersistentInt(EDC_DISARM_MENU_ID);

    if (r_ArmMenuID <= 0)
    {
        r_ArmMenuID = sc.AddACSChartShortcutMenuItem(sc.ChartNumber, "⚡ Arm Event Export");
    }

    if (r_DisarmMenuID <= 0)
    {
        r_DisarmMenuID = sc.AddACSChartShortcutMenuItem(sc.ChartNumber, "⚡ Disarm Event Export");
    }

    // Update menu state based on armed flag
    bool isArmed = sc.GetPersistentInt(EDC_EXPORT_ARMED_FLAG_ID) == 1;
    sc.SetACSChartShortcutMenuItemEnabled(sc.ChartNumber, r_ArmMenuID, isArmed ? 0 : 1);
    sc.SetACSChartShortcutMenuItemEnabled(sc.ChartNumber, r_DisarmMenuID, isArmed ? 1 : 0);

    // --- Cleanup on study removal ---
    if (sc.LastCallToFunction)
    {
        LBRFileManager::Instance().Close();

        // Remove menu items when study is removed
        if (r_ArmMenuID > 0)
            sc.RemoveACSChartShortcutMenuItem(sc.ChartNumber, r_ArmMenuID);

        if (r_DisarmMenuID > 0)
            sc.RemoveACSChartShortcutMenuItem(sc.ChartNumber, r_DisarmMenuID);

        return;
    }

    // --- Handle right-click context menu events ---
    if (sc.MenuEventID != 0 && sc.MenuEventID == r_ArmMenuID)
    {
        // Arm the export
        // --- Preflight validation before arming ---
        if (!PreflightValidation(sc)) {
            sc.AddMessageToLog("EDC: Preflight FAILED — export NOT armed. Check SC Log.", 1);
            return;
        }
        sc.SetPersistentInt(EDC_PREFLIGHT_PASSED_ID, 1);

        Logger::getInstance().log(
            "EDC: Session/Days preflight passed. No chart/session mutation on arm (reload-safe)."
        );

        sc.SetPersistentInt(EDC_EXPORT_ARMED_FLAG_ID, 1);

        // Generate new filename with timestamp
        time_t now = time(nullptr);
        char buffer[256];
        strftime(buffer, sizeof(buffer), "C:/SierraChart2/Data/event_data_%Y%m%d_%H%M%S", localtime(&now));
        g_exportFilename = buffer;

        // Open the .lbr file streams (creates .alpha and .context files)
        if (!LBRFileManager::Instance().Open(g_exportFilename, "ES"))
        {
            Logger::getInstance().log("ERROR: Failed to open LBR file streams: " + g_exportFilename);
            sc.SetPersistentInt(EDC_EXPORT_ARMED_FLAG_ID, 0);
            return;
        }

        Logger::getInstance().log("EventDataCollector: Export armed - collecting to: " + g_exportFilename + ".alpha/.context");
        sc.SetPersistentInt(EDC_LOCK_A_BLOCK_COUNT_ID, 0);
        sc.SetPersistentInt(EDC_LOCK_B_BLOCK_COUNT_ID, 0);
        sc.SetPersistentInt(EDC_LOCK_C_BLOCK_COUNT_ID, 0);
        sc.SetPersistentInt(EDC_LOCK_C_STABLE_COUNT_ID, 0);
        sc.SetPersistentInt(EDC_LOCK_C_UNSTABLE_COUNT_ID, 0);
        sc.SetPersistentInt(EDC_LOCK_C_READY_FLAG_ID, 0);
        sc.SetPersistentInt(EDC_LOCK_D_BLOCK_COUNT_ID, 0);
        sc.SetPersistentInt(EDC_LOCK_D_STALE_STREAK_ID, 0);
        sc.SetPersistentInt(EDC_LOCK_E_BLOCK_COUNT_ID, 0);
        sc.SetPersistentInt(EDC_LAST_TELEMETRY_EVENT_COUNT_ID, 0);
        sc.SetPersistentInt(EDC_ALPHA_COLLECTION_ACTIVE_ID, 0);
        sc.SetPersistentInt(EDC_SIGNIFICANT_COUNT_ID, 0);
        sc.SetPersistentInt(EDC_NULL_OBS_SKIP_COUNT_ID, 0);
        sc.SetPersistentInt(EDC_NULL_EVENT_SKIP_COUNT_ID, 0);
        sc.GetPersistentFloat(EDC_VELOCITY_EMA_ID) = 0.0f;
        sc.SetPersistentInt(EDC_VELOCITY_EMA_INIT_ID, 0);

        // Hard reset: chart-derived pre-arm state is invalid for this collection epoch.
        auto& cm = ContextManager::Instance();
        cm.Reset(GetReplaySafeNowUs(sc));
        Logger::getInstance().log(
            "EventDataCollector: Context reset generation=" +
            std::to_string(cm.GetResetGeneration()) +
            " instance=" + std::to_string(reinterpret_cast<uint64_t>(&cm))
        );
        sc.SetPersistentInt(EDC_STRUCT_BAR_INDEX_ID, -1);

        return;
    }
    else if (sc.MenuEventID != 0 && sc.MenuEventID == r_DisarmMenuID)
    {
        // Disarm the export
        sc.SetPersistentInt(EDC_EXPORT_ARMED_FLAG_ID, 0);

        // Finalize current output files immediately on disarm.
        LBRFileManager::Instance().Close();

        int eventCount = sc.GetPersistentInt(EDC_EVENT_COUNT_ID);
        int lockABlocks = sc.GetPersistentInt(EDC_LOCK_A_BLOCK_COUNT_ID);
        int lockBBlocks = sc.GetPersistentInt(EDC_LOCK_B_BLOCK_COUNT_ID);
        int lockCBlocks = sc.GetPersistentInt(EDC_LOCK_C_BLOCK_COUNT_ID);
        int lockDBlocks = sc.GetPersistentInt(EDC_LOCK_D_BLOCK_COUNT_ID);
        int lockEBlocks = sc.GetPersistentInt(EDC_LOCK_E_BLOCK_COUNT_ID);
        Logger::getInstance().log("EventDataCollector: Export disarmed - " + std::to_string(eventCount) + " events collected");
        Logger::getInstance().log(
            "EventDataCollector: Lock telemetry - LockA blocks=" + std::to_string(lockABlocks) +
            ", LockB blocks=" + std::to_string(lockBBlocks) +
            ", LockC blocks=" + std::to_string(lockCBlocks) +
            ", LockD blocks=" + std::to_string(lockDBlocks) +
            ", LockE blocks=" + std::to_string(lockEBlocks)
        );

        sc.GraphName = "Event Data Collector - Elite v2.3";

        // --- Diagnostic dump ---
        int breadcrumbStep = ReadBreadcrumb();
        int significantCount = sc.GetPersistentInt(EDC_SIGNIFICANT_COUNT_ID);
        int nullObsSkips = sc.GetPersistentInt(EDC_NULL_OBS_SKIP_COUNT_ID);
        int nullEventSkips = sc.GetPersistentInt(EDC_NULL_EVENT_SKIP_COUNT_ID);
        Logger::getInstance().log(
            "EDC DIAG: breadcrumb=" + std::to_string(breadcrumbStep)
            + " significantChanges=" + std::to_string(significantCount)
            + " nullEventSkips=" + std::to_string(nullEventSkips)
            + " nullObsSkips=" + std::to_string(nullObsSkips)
            + " eventsWritten=" + std::to_string(eventCount)
        );
        if (nullObsSkips > 0) {
            Logger::getInstance().log(
                "EDC DIAG: " + std::to_string(nullObsSkips)
                + " events BLOCKED by null observation/asymmetry_context gate"
            );
        }
        if (breadcrumbStep >= 0 && breadcrumbStep < 70) {
            Logger::getInstance().log(
                "EDC DIAG: Last breadcrumb=" + std::to_string(breadcrumbStep)
                + " — incomplete write cycle (70=OK). Possible crash site."
            );
        }

        sc.SetPersistentInt(EDC_EVENT_COUNT_ID, 0);
        sc.SetPersistentInt(EDC_ALPHA_COLLECTION_ACTIVE_ID, 0);
        sc.SetPersistentInt(EDC_SIGNIFICANT_COUNT_ID, 0);
        sc.SetPersistentInt(EDC_NULL_OBS_SKIP_COUNT_ID, 0);
        sc.SetPersistentInt(EDC_NULL_EVENT_SKIP_COUNT_ID, 0);
        return;
    }

    // ============================================================================
    // Main Processing
    // ============================================================================

    if (sc.GetPersistentInt(EDC_EXPORT_ARMED_FLAG_ID) == 1)
    {
        try {
            // 0. Market-Closed Gate: Skip only when CME Globex is shut.
            // Overnight sessions ARE kept — only the weekend closure is skipped.
            // During replay over weekends, bars produce stale indicator values
            // that pollute the rank buffers with identical entries.
            //
            // Timezone-agnostic: derives open/close from the chart's own
            // session times (sc.StartTime1 / sc.EndTime1) so the gate stays
            // correct regardless of whether the chart is in ET, CT, etc.
            {
                int barDOW = sc.BaseDateTimeIn[sc.Index].GetDayOfWeek();
                int barHour = 0, barMinute = 0, barSecond = 0;
                sc.BaseDateTimeIn[sc.Index].GetTimeHMS(barHour, barMinute, barSecond);
                int barTimeHHMM = barHour * 100 + barMinute;

                const int openHHMM  = (sc.StartTime1 / 3600) * 100
                                    + (sc.StartTime1 % 3600) / 60;
                const int closeHHMM = (sc.EndTime1 / 3600) * 100
                                    + (sc.EndTime1 % 3600) / 60;

                // Sierra Chart DOW: 0=Sunday, 1=Monday, ..., 5=Friday, 6=Saturday
                bool isMarketClosed = (barDOW == 6)                        // Saturday: always closed
                    || (barDOW == 0 && barTimeHHMM < openHHMM)             // Sunday before session open
                    || (barDOW == 5 && barTimeHHMM >= closeHHMM);          // Friday at/after session close
                if (isMarketClosed) {
                    return;  // Silently skip — no stale data in .context
                }
            }

            // 1. Get replay-safe timestamp
            // Use Sierra's canonical current-time abstraction:
            // - Replay: replay timeline (deterministic)
            // - Live:   system clock time
            // This preserves intra-bar ordering when replay updates occur
            // within a 15-minute bar and improves timestamp_us fidelity.
            uint64_t now_us = GetReplaySafeNowUs(sc);

            // 1b. Synthetic Event Velocity (telemetry/adaptive thresholds)
            // During replay, real-time tick timestamps don't exist — historical
            // data is bar-clamped.  Derive trade-rate from the bar's own stats:
            //   raw_rate = NumberOfTrades / SecondsPerBar  (trades per second)
            // Then smooth with a 5-period EMA to avoid stair-stepping and feed
            // velocity-sensitive logic in ContextManager.
            // Canonical ObservationData dim1 remains burstiness by contract.
            float syntheticVelocity = 0.0f;
            if (sc.SecondsPerBar > 0) {
                float rawRate = static_cast<float>(sc.NumberOfTrades[sc.Index])
                              / static_cast<float>(sc.SecondsPerBar);

                float& emaState = sc.GetPersistentFloat(EDC_VELOCITY_EMA_ID);
                int& emaInit = sc.GetPersistentInt(EDC_VELOCITY_EMA_INIT_ID);

                if (emaInit == 0) {
                    emaState = rawRate;    // Seed EMA with first sample
                    emaInit = 1;
                } else {
                    emaState += VELOCITY_EMA_ALPHA * (rawRate - emaState);
                }
                syntheticVelocity = emaState;
            }

            // 2. Update Raw Indicators
            IndicatorManager::Instance().UpdateBarContext(sc);

            // 3. Feed Market Physics engines (Hill, Shannon, LZ) on every
            //    tick-level price change.  Identical pattern to SCStudies.
            //    During historical replay: one return per completed bar.
            //    During live/replay ticks: one return per price change.
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

            // 3b. Update Price Structure (mirrors SCStudies)
            //     Must be called BEFORE CheckAndTriggerHMM to ensure
            //     structure metrics (fractal dim, elder impulse) are current.
            {
                int lastStructIdx = sc.GetPersistentInt(EDC_STRUCT_BAR_INDEX_ID);
                bool isStructureNewBar = (sc.Index != lastStructIdx);
                ContextManager::Instance().UpdatePriceStructure(
                    sc,
                    sc.High[sc.Index],
                    sc.Low[sc.Index],
                    sc.Close[sc.Index],
                    isStructureNewBar
                );
                if (isStructureNewBar) {
                    sc.SetPersistentInt(EDC_STRUCT_BAR_INDEX_ID, sc.Index);
                }
            }

            // 4. Trigger ContextManager Logic
            // This handles Scenario A: Always logging context for data collection
            // And Scenario B: Logging specific HMM training events on Mahalanobis shift
            // Synthetic velocity is injected so velocity-sensitive trigger logic and
            // log_event_velocity remain replay-safe (timestamp velocity flatlines in replay).
            // ObservationData dim1 is burstiness, not raw event velocity.
            ContextManager::Instance().CheckAndTriggerHMM(now_us, true, syntheticVelocity,
                                                            IsPostWeekendReopenGracePeriod(sc));

            const size_t saturationSamples = ContextManager::Instance().GetObservationSampleCount();
            const size_t saturationRequired = ContextManager::GetObservationSaturationRequired();
            const bool lockAReady = ContextManager::Instance().IsObservationSaturated();
            const bool lockBReady = lockAReady ? IndicatorManager::Instance().IsWarmedUp() : false;

            float lockCEntropy = 1.0f;
            float lockCKurtosis = 1.0f;
            bool lockCReady = false;
            if (lockAReady && lockBReady) {
                // Read post-FeatureScaler rank-percentile values [0,1]
                // LockC gates on the SCALED signal, not raw sensors.
                auto scaledObs = ContextManager::Instance().GetLatestScaledObservation();
                lockCEntropy = scaledObs[ContextManager::OBS_LEMPEL_ZIV];
                lockCKurtosis = scaledObs[ContextManager::OBS_TAIL_INDEX];

                int stableCount = sc.GetPersistentInt(EDC_LOCK_C_STABLE_COUNT_ID);
                int unstableCount = sc.GetPersistentInt(EDC_LOCK_C_UNSTABLE_COUNT_ID);
                const bool latchReady = sc.GetPersistentInt(EDC_LOCK_C_READY_FLAG_ID) == 1;

                const bool stableSample =
                    std::isfinite(lockCEntropy) &&
                    std::isfinite(lockCKurtosis) &&
                    (lockCEntropy <= LOCK_C_ENTROPY_ENTER) &&
                    (lockCKurtosis <= LOCK_C_KURTOSIS_ENTER);

                const bool unstableSample =
                    (!std::isfinite(lockCEntropy)) ||
                    (!std::isfinite(lockCKurtosis)) ||
                    (lockCEntropy >= LOCK_C_ENTROPY_EXIT) ||
                    (lockCKurtosis >= LOCK_C_KURTOSIS_EXIT);

                if (!latchReady) {
                    stableCount = stableSample ? (stableCount + 1) : 0;
                    unstableCount = 0;
                    if (stableCount >= LOCK_C_STABLE_REQUIRED) {
                        sc.SetPersistentInt(EDC_LOCK_C_READY_FLAG_ID, 1);
                    }
                } else {
                    unstableCount = unstableSample ? (unstableCount + 1) : 0;
                    stableCount = stableSample ? std::min(stableCount + 1, LOCK_C_STABLE_REQUIRED) : stableCount;
                    if (unstableCount >= LOCK_C_UNSTABLE_REQUIRED) {
                        sc.SetPersistentInt(EDC_LOCK_C_READY_FLAG_ID, 0);
                        stableCount = 0;
                        unstableCount = 0;
                    }
                }

                sc.SetPersistentInt(EDC_LOCK_C_STABLE_COUNT_ID, stableCount);
                sc.SetPersistentInt(EDC_LOCK_C_UNSTABLE_COUNT_ID, unstableCount);
                const bool newLockCReady = sc.GetPersistentInt(EDC_LOCK_C_READY_FLAG_ID) == 1;
                if (newLockCReady != latchReady) {
                    static int lockCTransitionCount = 0;
                    ++lockCTransitionCount;
                    if (lockCTransitionCount <= 5 || (lockCTransitionCount % 50) == 0) {
                        Logger::getInstance().log(
                            std::string("EventDataCollector: LockC ") +
                            (newLockCReady ? "ACQUIRED" : "RELEASED") +
                            " entropy=" + std::to_string(lockCEntropy) +
                            ", kurtosis=" + std::to_string(lockCKurtosis) +
                            " (transition #" + std::to_string(lockCTransitionCount) + ")"
                        );
                    }
                }
                lockCReady = newLockCReady;
            }

            const bool lockDReady = (lockAReady && lockBReady)
                ? ContextManager::Instance().AreTs1DimsReady(now_us, LOCK_D_TS1_MAX_AGE_US)
                : false;
            const uint64_t ts1LastWriteUs = ContextManager::Instance().GetTs1MacroLastWriteUs();
            const uint64_t ts1AgeUs = ContextManager::Instance().GetTs1MacroAgeUs(now_us);
            const bool lockEReady = (lockAReady && lockBReady)
                ? ContextManager::Instance().AreTs2StructuralDimsReady(now_us, LOCK_E_TS2_MAX_AGE_US)
                : false;
            const uint64_t ts2LastWriteUs = ContextManager::Instance().GetTs2StructuralLastWriteUs();
            const uint64_t ts2AgeUs = ContextManager::Instance().GetTs2StructuralAgeUs(now_us);

            {
                const int eventCount = sc.GetPersistentInt(EDC_EVENT_COUNT_ID);
                const int lockABlocks = sc.GetPersistentInt(EDC_LOCK_A_BLOCK_COUNT_ID);
                const int lockBBlocks = sc.GetPersistentInt(EDC_LOCK_B_BLOCK_COUNT_ID);
                const int lockCBlocks = sc.GetPersistentInt(EDC_LOCK_C_BLOCK_COUNT_ID);
                const int lockDBlocks = sc.GetPersistentInt(EDC_LOCK_D_BLOCK_COUNT_ID);
                const int lockEBlocks = sc.GetPersistentInt(EDC_LOCK_E_BLOCK_COUNT_ID);
                const int lockCStable = sc.GetPersistentInt(EDC_LOCK_C_STABLE_COUNT_ID);
                const bool alphaActive = lockAReady && lockBReady && lockDReady && lockEReady;

                SCString hudTitle;
                hudTitle.Format(
                    "Event Data Collector - Elite v2.3 | A %llu/%llu %s | B %s | D %s (age=%llus) | E %s (age=%llus) | C %s (%d/%d e=%.2f k=%.2f) | alpha %s | ev %d | blkA %d blkB %d blkC %d blkD %d blkE %d",
                    static_cast<unsigned long long>(saturationSamples),
                    static_cast<unsigned long long>(saturationRequired),
                    lockAReady ? "READY" : "WARMUP",
                    lockBReady ? "READY" : "WARMUP",
                    lockDReady ? "READY" : "WARMUP",
                    static_cast<unsigned long long>(ts1AgeUs / 1000000ULL),
                    lockEReady ? "READY" : "WARMUP",
                    static_cast<unsigned long long>(ts2AgeUs / 1000000ULL),
                    lockCReady ? "READY" : "WARMUP",
                    lockCStable,
                    LOCK_C_STABLE_REQUIRED,
                    lockCEntropy,
                    lockCKurtosis,
                    alphaActive ? "ON" : "HOLD",
                    eventCount,
                    lockABlocks,
                    lockBBlocks,
                    lockCBlocks,
                    lockDBlocks,
                    lockEBlocks
                );
                sc.GraphName = hudTitle;
            }

            // === HISTORICAL DOWNLOAD GUARD (replay-safe) ===
            // During the initial data download phase of a replay session,
            // allow warm-up above (indicators, physics, HMM, HUD) to prime
            // the observation scaler and indicator state.  Skip lock-gate
            // counting and event writing until replay playback begins.
            // NOTE: sc.DownloadingHistoricalData is deprecated since SC v2395.
            //       Use sc.ChartIsDownloadingHistoricalData(ChartNumber) instead.
            if (sc.ChartIsDownloadingHistoricalData(sc.ChartNumber))
                return;

            // Unified collection gate (Double-Lock for .alpha):
            //   Lock A: observation stream reached event-driven saturation readiness
            //   Lock B: indicator stack passed warm-up validation
            //   Lock C: regime certainty (Schmidt trigger) — TELEMETRY ONLY during
            //           data collection.  LockC is NOT enforced here because the
            //           data-collector must capture ALL market regimes for System
            //           Identification training.  The Schmidt trigger still runs
            //           above for HUD display and will be enforced by the live-
            //           inference path (SCStudies.cpp) when deployed.
            // .context collection is handled inside CheckAndTriggerHMM() and is allowed
            // to mature independently from Lock B.
            if (!lockAReady) {
                int lockABlocks = sc.GetPersistentInt(EDC_LOCK_A_BLOCK_COUNT_ID) + 1;
                sc.SetPersistentInt(EDC_LOCK_A_BLOCK_COUNT_ID, lockABlocks);

                if ((lockABlocks % 250) == 0) {
                    Logger::getInstance().log(
                        "EventDataCollector: LockA waiting (observation saturation) samples=" +
                        std::to_string(saturationSamples) + "/" + std::to_string(saturationRequired)
                    );
                }
                return;
            }

            if (!lockBReady) {
                int lockBBlocks = sc.GetPersistentInt(EDC_LOCK_B_BLOCK_COUNT_ID) + 1;
                sc.SetPersistentInt(EDC_LOCK_B_BLOCK_COUNT_ID, lockBBlocks);

                if ((lockBBlocks % 250) == 0) {
                    Logger::getInstance().log(
                        "EventDataCollector: LockB waiting (indicator warm-up not complete)"
                    );
                }
                return;
            }

            if (!lockDReady) {
                int lockDBlocks = sc.GetPersistentInt(EDC_LOCK_D_BLOCK_COUNT_ID) + 1;
                sc.SetPersistentInt(EDC_LOCK_D_BLOCK_COUNT_ID, lockDBlocks);

                int staleStreak = sc.GetPersistentInt(EDC_LOCK_D_STALE_STREAK_ID) + 1;
                sc.SetPersistentInt(EDC_LOCK_D_STALE_STREAK_ID, staleStreak);

                if ((lockDBlocks % 250) == 0) {
                    Logger::getInstance().log(
                        "EventDataCollector: LockD waiting (TS1 macro dims stale/unready) age_us=" +
                        std::to_string(ts1AgeUs) +
                        " last_write_us=" + std::to_string(ts1LastWriteUs)
                    );
                }

                if (staleStreak == LOCK_D_CIRCUIT_BREAKER_STALE ||
                    (staleStreak > LOCK_D_CIRCUIT_BREAKER_STALE &&
                     (staleStreak % LOCK_D_CIRCUIT_BREAKER_STALE) == 0)) {
                    Logger::getInstance().log(
                        "EventDataCollector: LockD circuit-breaker ACTIVE (consecutive stale checks=" +
                        std::to_string(staleStreak) +
                        "). Investigate TS1 writer/inputs before collecting."
                    );
                }
                return;
            }
            sc.SetPersistentInt(EDC_LOCK_D_STALE_STREAK_ID, 0);

            if (!lockEReady) {
                int lockEBlocks = sc.GetPersistentInt(EDC_LOCK_E_BLOCK_COUNT_ID) + 1;
                sc.SetPersistentInt(EDC_LOCK_E_BLOCK_COUNT_ID, lockEBlocks);

                if ((lockEBlocks % 250) == 0) {
                    Logger::getInstance().log(
                        "EventDataCollector: LockE waiting (TS2 structural dims stale/unready) age_us=" +
                        std::to_string(ts2AgeUs) +
                        " last_write_us=" + std::to_string(ts2LastWriteUs)
                    );
                }
                return;
            }

            // LockC pass-through: track blocks for telemetry but do NOT gate.
            if (!lockCReady) {
                int lockCBlocks = sc.GetPersistentInt(EDC_LOCK_C_BLOCK_COUNT_ID) + 1;
                sc.SetPersistentInt(EDC_LOCK_C_BLOCK_COUNT_ID, lockCBlocks);
                // No return — fall through to alpha collection
            }

            // Log one-time lock transition marker when alpha path first opens.
            if (sc.GetPersistentInt(EDC_ALPHA_COLLECTION_ACTIVE_ID) == 0) {
                sc.SetPersistentInt(EDC_ALPHA_COLLECTION_ACTIVE_ID, 1);
                sc.SetPersistentInt(EDC_LAST_TELEMETRY_EVENT_COUNT_ID, sc.GetPersistentInt(EDC_EVENT_COUNT_ID));
                const std::string lockCStatus = lockCReady ? "ready" : "telemetry-warmup";
                Logger::getInstance().log(
                    "EventDataCollector: Alpha collection active (LockA=ready, LockB=ready, LockD=ready, LockE=ready, LockC=" +
                    lockCStatus + ")"
                );
            }

            // 4. Log Significant Indicator Changes (Wide-Table Alpha Stream)
            const bool significantChange = IndicatorManager::Instance().HasSignificantChange();
            if (significantChange)
            {
                WriteBreadcrumb(10);  // HasSignificantChange=true
                sc.SetPersistentInt(EDC_SIGNIFICANT_COUNT_ID,
                    sc.GetPersistentInt(EDC_SIGNIFICANT_COUNT_ID) + 1);

                auto eventT = IndicatorManager::Instance().GetTrainingEventT(sc);
                if (!eventT) {
                    sc.SetPersistentInt(EDC_NULL_EVENT_SKIP_COUNT_ID,
                        sc.GetPersistentInt(EDC_NULL_EVENT_SKIP_COUNT_ID) + 1);
                    return;
                }

                WriteBreadcrumb(20);  // GetTrainingEventT OK

                WriteBreadcrumb(30);  // TrainingEvent populated

                // Log Model Confidence (if fresh signal exists)
                if (TradeSignalManager::Instance().HasFreshSignal()) {
                    eventT->model_confidence = TradeSignalManager::Instance().GetTradeSignal().modelConfidence;
                } else {
                    eventT->model_confidence = 0.0f;
                }

                eventT->timestamp_us = now_us;
                eventT->event_type_code = MTS::Training::EventTypeCode_IndicatorChange;

                WriteBreadcrumb(40);  // Metadata + gap done

                // Sequence-Locked Stitcher: write MO+SS+TrainingEvent with shared sequence_id
                const float stitched_regime_tenure = static_cast<float>(eventT->regime_tenure);
                if (eventT->observation && eventT->asymmetry_context) {
                    WriteBreadcrumb(50);  // Entering LogSynchronizedEvent
                    LBRFileManager::Instance().LogSynchronizedEvent(
                        *eventT,
                        *eventT->observation,
                        *eventT->asymmetry_context,
                        now_us,
                        stitched_regime_tenure
                    );
                    WriteBreadcrumb(60);  // LogSynchronizedEvent returned
                } else {
                    WriteBreadcrumb(45);  // observation/asymmetry null — write skipped
                    sc.SetPersistentInt(EDC_NULL_OBS_SKIP_COUNT_ID,
                        sc.GetPersistentInt(EDC_NULL_OBS_SKIP_COUNT_ID) + 1);
                }

                // Clear dirty mask after consuming (matches live path in PublishEventOnChange)
                IndicatorManager::Instance().ClearDirtyMask();

                // Update UI Subgraph
                int count = sc.GetPersistentInt(EDC_EVENT_COUNT_ID) + 1;
                sc.SetPersistentInt(EDC_EVENT_COUNT_ID, count);
                sc.Subgraph[0][sc.Index] = static_cast<float>(count);

                WriteBreadcrumb(70);  // Full cycle OK
            }
        }
        catch (const std::exception& e) {
            Logger::getInstance().log("EventDataCollector: Exception - " + std::string(e.what()));
        }
    }
}
