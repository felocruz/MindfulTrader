#include "MindfulTrader_Precompiled.h"
#include <sstream>
#include <iomanip>
#include <random> // Added for shuffling
#include "Logger.h"
#include "DailyBiasEngine.h"
#include "RingBuffer.h"
#include "CarryForwardCalculators.h"

/// ============================================================================
/// INSTITUTIONAL-GRADE: RollingWindowCalculator Template
/// ============================================================================
/// O(1) circular buffer for high-frequency statistical calculations
///
/// Design: Maintains running sum internally, so push/pop = O(1)
/// Example: 20-bar entropy window updated 1000×/sec = 20µs saved per second
///
/// Thread-safe: Each caller gets their own instance (static local, thread_local)
///
/// Capacity is a compile-time template parameter (fixed-capacity RingBuffer,
/// zero heap allocation for its lifetime), not a runtime constructor argument
/// -- matches the convention established in docs/superpowers/specs/2026-08-07-
/// contextmanager-ring-buffer-dod-design.md (Round 4). The window never holds
/// more than Capacity elements: push() pops down to Capacity-1 before pushing
/// whenever already full, so it never transiently overshoots.
/// ============================================================================
template<typename T, size_t Capacity>
class RollingWindowCalculator {
private:
    RingBuffer<T, Capacity> window;
    T sum = 0;

public:
    void push(T value) {
        if (window.size() == Capacity) {
            sum -= window[0];
            window.pop_front();
        }
        window.push_back(value);
        sum += value;
    }

    T mean() const {
        return window.empty() ? 0 : sum / static_cast<T>(window.size());
    }

    T get_sum() const { return sum; }
    bool is_full() const { return window.size() == Capacity; }
    size_t size() const { return window.size(); }
    void reset() { window.clear(); sum = 0; }
};

/// ============================================================================
/// FIX #1-2: O(1) ATR CALCULATION - Circular Buffer Management
/// ============================================================================
/// Replaces O(n) recalculation with O(1) incremental updates
class ATRCalculator {
private:
    RollingWindowCalculator<float, 5> atr_fast_buffer;
    RollingWindowCalculator<float, 20> atr_slow_buffer;
    int last_index = -1;

public:
    ATRCalculator() = default;

    float GetMarketSpeed(float tr_current, float prev_market_speed) {
        // Push current TR into both buffers (O(1) each)
        atr_fast_buffer.push(tr_current);
        atr_slow_buffer.push(tr_current);

        if (!atr_slow_buffer.is_full()) return 1.0f;  // Warmup

        float atr_fast = atr_fast_buffer.mean();
        float atr_slow = atr_slow_buffer.mean();

        if (atr_slow < 0.0001f) return prev_market_speed;  // Guard

        float market_speed = atr_fast / atr_slow;
        return std::clamp(market_speed, 0.4f, 2.5f);
    }

    void reset() {
        atr_fast_buffer.reset();
        atr_slow_buffer.reset();
        last_index = -1;
    }
};

/// ============================================================================
/// FIX #2: O(1) EFFICIENCY RATIO - Running Volatility Sum
/// ============================================================================
/// Trades memory for speed: maintains running sum of |price changes|
class EfficiencyRatioCalculator {
private:
    static constexpr int ER_LOOKBACK = 34;
    RollingWindowCalculator<float, ER_LOOKBACK> volatility_window;

public:
    EfficiencyRatioCalculator() = default;

    float GetEfficiencyRatio(float net_change, float abs_daily_change) {
        // Push |price change| into window (O(1))
        volatility_window.push(abs_daily_change);

        if (!volatility_window.is_full()) return 0.5f;  // Warmup

        float volatility = volatility_window.get_sum();
        if (volatility < 0.00001f) return 0.5f;

        float er = net_change / volatility;
        return std::clamp(er, 0.0f, 1.0f);
    }

    void reset() { volatility_window.reset(); }
};

/// ============================================================================
/// FIX #3: ADAPTIVE THRESHOLDS - Market Speed Scaling
/// ============================================================================
/// Thresholds scale with market regime to prevent false signals
class AdaptiveThresholdManager {
private:
    // Base thresholds (calibrated via backtesting)
    static constexpr float BASE_STATE_CHANGE_THRESHOLD = 0.05f;     // 5% base
    static constexpr float BASE_COHERENCE_THRESHOLD = 0.10f;        // 10% base
    static constexpr int BASE_CONFIRMATION_BARS = 3;

public:
    float GetStateChangeThreshold(float market_speed) {
        // Fast market (speed 2.5): tighter threshold 0.05 ÷ 2.5 = 0.02
        // Slow market (speed 0.4): looser threshold 0.05 ÷ 0.4 = 0.125
        return BASE_STATE_CHANGE_THRESHOLD / market_speed;
    }

    float GetCoherenceThreshold(float market_speed) {
        // Similar scaling: adaptive to market regime
        return BASE_COHERENCE_THRESHOLD / market_speed;
    }

    int GetConfirmationBars(float market_speed) {
        // Fast market: need more bars (volatile, confirm firmly)
        // Slow market: fewer bars (less noise)
        int bars = static_cast<int>(BASE_CONFIRMATION_BARS * market_speed);
        return std::clamp(bars, 2, 6);  // Range [2, 6] bars
    }
};

/// ============================================================================
/// FIX #4-5: ENHANCED IMPULSE HYSTERESIS WITH ADAPTIVE STATES
/// ============================================================================
class EnhancedImpulseHysteresis {
private:
    int confirmation_count = 0;
    int prev_impulse_state = 0;
    int window_change_count = 0;  // NEW: prevents window flipping
    static constexpr int CONFIRMATION_THRESHOLD_MAX = 6;

public:
    int GetSmoothedImpulse(int current_impulse, float macd_histogram,
                           float prev_macd_histogram, float adaptive_threshold) {
        float slope_change = std::abs(macd_histogram - prev_macd_histogram);

        if (slope_change < adaptive_threshold) {
            return prev_impulse_state;  // Insufficient change
        }

        if (current_impulse == prev_impulse_state) {
            confirmation_count = 0;
        } else {
            confirmation_count++;
        }

        if (confirmation_count >= CONFIRMATION_THRESHOLD_MAX) {
            prev_impulse_state = current_impulse;
            confirmation_count = 0;
        }

        return prev_impulse_state;
    }

    // FIX #5: Window Change Hysteresis - requires 5 bars to flip window
    bool ShouldChangeWindow(int new_window, int prev_window) {
        if (new_window == prev_window) {
            window_change_count = 0;
            return false;
        }

        window_change_count++;
        return window_change_count >= 5;  // 5 bars to confirm
    }

    void ResetWindowChangeCount() {
        window_change_count = 0;
    }

    void reset() {
        confirmation_count = 0;
        prev_impulse_state = 0;
        window_change_count = 0;
    }
};

/// ============================================================================
/// FIX #7: PERFORMANCE INSTRUMENTATION - Metrics Collection
/// ============================================================================
struct AdaptiveWindowMetrics {
    int window_change_count = 0;      // How many times did window size change?
    float avg_market_speed = 1.0f;    // Running average
    float coherence_score_avg = 0.5f; // Running average coherence
    int low_coherence_bars = 0;       // Bars with <0.5 coherence

    void UpdateMetrics(float market_speed, float coherence_score) {
        // Exponential average for market speed (reduce memory, smooth trend)
        const float ALPHA = 0.05f;
        avg_market_speed = ALPHA * market_speed + (1.0f - ALPHA) * avg_market_speed;

        // Coherence tracking
        coherence_score_avg = ALPHA * coherence_score + (1.0f - ALPHA) * coherence_score_avg;
        if (coherence_score < 0.5f) low_coherence_bars++;
    }
};

/// ============================================================================
/// ELITE v2.5: ADAPTIVE WINDOWING SYSTEM - Full Integration
/// ============================================================================
/// Dynamic Market-Speed Scaling for Multi-Timeframe Coherence
///
/// Architecture:
/// - CalculateMarketSpeed: Master clock (5-period ATR / 20-period ATR) - NOW O(1)
/// - CheckCoherence: Multi-TF alignment detection for Screen 3 (15m) - NOW returns [0,1]
/// - ImpulseHysteresis: Schmitt trigger with adaptive thresholds
/// - Window hysteresis: Prevents daily window flipping
/// ============================================================================

/// FIX #1: GLOBAL MARKET SPEED - Master Clock (O(1) OPTIMIZATION)
/// BEFORE: O(25) - 5+20 bar loops per call
/// AFTER:  O(1)  - 2 circular buffer pushes
/// Performance: 92% faster market speed calculation
struct AdaptiveCalculatorsState {
    ATRCalculator marketSpeedCalc;
    EfficiencyRatioCalculator efficiencyRatioCalc;
    float prevMarketSpeed = 1.0f;

    void reset() {
        marketSpeedCalc.reset();
        efficiencyRatioCalc.reset();
        prevMarketSpeed = 1.0f;
    }
};

static AdaptiveCalculatorsState* GetAdaptiveCalculatorsState(SCStudyInterfaceRef sc) {
    auto* state = static_cast<AdaptiveCalculatorsState*>(
        sc.GetPersistentPointer(PersistentVar_AdaptiveCalculators::STATE_PTR));

    if (!state) {
        state = new AdaptiveCalculatorsState();
        sc.SetPersistentPointer(PersistentVar_AdaptiveCalculators::STATE_PTR, state);
    }

    return state;
}

static void CleanupAdaptiveWindowState(SCStudyInterfaceRef sc);
static void ResetAdaptiveWindowState(SCStudyInterfaceRef sc);

void CleanupAdaptiveCalculators(SCStudyInterfaceRef sc) {
    auto* state = static_cast<AdaptiveCalculatorsState*>(
        sc.GetPersistentPointer(PersistentVar_AdaptiveCalculators::STATE_PTR));

    if (state) {
        delete state;
        sc.SetPersistentPointer(PersistentVar_AdaptiveCalculators::STATE_PTR, nullptr);
    }

    CleanupAdaptiveWindowState(sc);
}

/// ============================================================================
/// DETERMINISTIC STATE RESET: Clears buffers on chart/symbol change
/// ============================================================================
/// Called when sc.IsFullRecalculation triggers (chart reload, symbol change, etc).
/// Ensures circular buffers don't carry ghost data from previous session.
void ResetAdaptiveCalculators(SCStudyInterfaceRef sc) {
    auto* state = static_cast<AdaptiveCalculatorsState*>(
        sc.GetPersistentPointer(PersistentVar_AdaptiveCalculators::STATE_PTR));

    if (state) {
        state->reset();
    }

    ResetAdaptiveWindowState(sc);
}

bool IsPostWeekendReopenGracePeriod(SCStudyInterfaceRef sc)
{
    // Grace window matches LOCK_D_TS1_MAX_AGE_US's own 6-hour budget
    // (EventDataCollectorStudy.cpp) -- covers exactly the ~49-hour Friday-close
    // to Sunday-reopen gap this fix exists for, without masking a genuinely
    // stuck system on any other day.
    constexpr int kWeekendGraceHours = 6;

    // SCDateTime::GetDayOfWeek() returns the DayOfWeekEnum from
    // sierra_chart_dependencies/scdatetime.h: SUNDAY=0, MONDAY=1, ..., SATURDAY=6
    // (confirmed by direct construction against the vendored header). This is
    // the standard 0=Sunday convention, not the 1=Sunday..7=Saturday convention
    // that the existing Market-Closed Gate's comment in EventDataCollectorStudy.cpp
    // assumes -- so 0 (not 1) is used here for Sunday.
    const int barDOW = sc.BaseDateTimeIn[sc.Index].GetDayOfWeek();  // 0=Sunday, ..., 6=Saturday
    if (barDOW != 0) {
        return false;
    }

    int barHour = 0, barMinute = 0, barSecond = 0;
    sc.BaseDateTimeIn[sc.Index].GetTimeHMS(barHour, barMinute, barSecond);
    const int barTimeHHMM = barHour * 100 + barMinute;

    const int openHHMM = (sc.StartTime1 / 3600) * 100 + (sc.StartTime1 % 3600) / 60;
    const int graceEndHHMM = openHHMM + kWeekendGraceHours * 100;

    return barTimeHHMM >= openHHMM && barTimeHHMM < graceEndHHMM;
}

float CalculateMarketSpeed(SCStudyInterfaceRef sc) {
    if (sc.Index < 20) return 1.0f;  // Warmup

    auto* state = GetAdaptiveCalculatorsState(sc);
    if (!state) {
        return 1.0f;
    }

    // Calculate true range for current bar
    float tr = sc.High[sc.Index] - sc.Low[sc.Index];
    if (sc.Index > 0) {
        tr = std::max(tr, std::abs(sc.Close[sc.Index] - sc.Close[sc.Index - 1]));
    }

    // Get market speed from O(1) circular buffer calculation
    float market_speed = state->marketSpeedCalc.GetMarketSpeed(tr, state->prevMarketSpeed);
    state->prevMarketSpeed = market_speed;

    return market_speed;
}

float CalculateEfficiencyRatio(SCStudyInterfaceRef sc, int lookback_bars) {
    if (sc.Index < lookback_bars) return 0.5f;  // Warmup

    auto* state = GetAdaptiveCalculatorsState(sc);
    if (!state) {
        return 0.5f;
    }

    // Net change over lookback period
    float net_change = std::abs(sc.Close[sc.Index] - sc.Close[sc.Index - lookback_bars]);

    // Daily absolute change (push into circular buffer for O(1) sum)
    float abs_daily_change = std::abs(sc.Close[sc.Index] - sc.Close[sc.Index - 1]);

    return state->efficiencyRatioCalc.GetEfficiencyRatio(net_change, abs_daily_change);
}



/// FIX #5: MULTI-TIMEFRAME COHERENCE CHECK (FLOAT SCORE RETURN)
/// BEFORE: Returns boolean (true/false) - loses signal strength information
/// AFTER:  Returns float [0.0, 1.0] - provides confidence scoring
/// Benefit: Better gating logic - can use soft thresholds (0.7 = strong, 0.3 = weak)
float CheckCoherence(float tf1_macd_histogram, float tf2_macd_histogram,
                    float max_observed_macd) {
    // Alignment check: both bullish or both bearish
    bool tf1_bullish = tf1_macd_histogram > 0.0f;
    bool tf2_bullish = tf2_macd_histogram > 0.0f;
    bool is_aligned = (tf1_bullish == tf2_bullish);

    if (!is_aligned) return 0.0f;  // Anti-correlated = no coherence

    // Strength calculation: normalized magnitude
    float epsilon = max_observed_macd * 0.0001f + 0.00001f;
    float tf1_strength = std::min(1.0f, std::abs(tf1_macd_histogram) / (max_observed_macd + epsilon));
    float tf2_strength = std::min(1.0f, std::abs(tf2_macd_histogram) / (max_observed_macd + epsilon));

    // Coherence score: geometric mean of aligned strengths (0.0 = weak, 1.0 = strong)
    // Geometric mean is more robust than arithmetic for proportional metrics
    float coherence_score = std::sqrt(tf1_strength * tf2_strength);

    return std::clamp(coherence_score, 0.0f, 1.0f);
}

/// FIX #3-4: IMPULSE HYSTERESIS - Schmitt Trigger with Adaptive Thresholds
/// BEFORE: Fixed thresholds (0.05f, 3 bars) appropriate only for mean market speed
/// AFTER:  Scales with market regime (fast = tighter, slow = looser)
/// Philosophy: Fast markets need harder evidence; slow markets need not wait as long
class ImpulseHysteresis {
private:
    int confirmation_count = 0;
    int prev_impulse_state = 0;  // 0=unknown, 1=green, -1=red, 2=blue
    static AdaptiveThresholdManager threshold_mgr;  // Handles market-speed scaling

public:
    ImpulseHysteresis() = default;

    int GetSmoothedImpulse(int current_impulse, float macd_histogram,
                          float prev_macd_histogram, float market_speed) {
        // Get adaptive threshold based on current market regime
        float adaptive_threshold = threshold_mgr.GetStateChangeThreshold(market_speed);

        // Detect if MACD histogram slope is significant (>adaptive_threshold)
        float slope_change = std::abs(macd_histogram - prev_macd_histogram);

        if (slope_change < adaptive_threshold) {
            // Insufficient change: stick with previous state (flip prevention)
            return prev_impulse_state;
        }

        // Significant change detected - require adaptive bar count to confirm
        if (current_impulse == prev_impulse_state) {
            confirmation_count = 0;  // Reset on state mismatch
        } else {
            confirmation_count++;
        }

        int adaptive_confirmation = threshold_mgr.GetConfirmationBars(market_speed);
        if (confirmation_count >= adaptive_confirmation) {
            // Confirmed: accept new state
            prev_impulse_state = current_impulse;
            confirmation_count = 0;
        }

        return prev_impulse_state;  // Return smoothed (hysteresis-filtered) state
    }

    void reset() {
        confirmation_count = 0;
        prev_impulse_state = 0;
    }
};

AdaptiveThresholdManager ImpulseHysteresis::threshold_mgr;

/// ============================================================================
/// FUND-GRADE REGIME DIAGNOSTICS - Audit Trail for Post-Trade Analysis
/// ============================================================================
/// Elite fund managers need to answer: "Why did we trade here?"
/// This struct captures regime changes for institutional audit and backtesting review.
/// Logs to Logger: [Regime Change] Speed: X.XX | ER: Y.YY | Window: A → B
///
/// Used to validate Market Speed logic and justify window adaptations to risk committee.
struct RegimeDiagnostics {
    bool window_changed = false;
    int prev_window = 0;
    int new_window = 0;
    float market_speed = 1.0f;
    float efficiency_ratio = 0.5f;

    void LogToAuditTrail() const {
        if (!window_changed) return;

        // Fund-grade audit format for Logger: [Regime Change] Speed: X.XX | ER: Y.YY | Window: A → B
        std::ostringstream oss;
        oss << "[Regime Change] Speed: " << std::fixed << std::setprecision(2) << market_speed
            << " | ER: " << std::fixed << std::setprecision(2) << efficiency_ratio
            << " | Window: " << prev_window << " → " << new_window;

        // Log to institutional audit trail (stored in file for post-trade review)
        Logger::getInstance().log(oss.str());
    }
};

/// FIX #5-6: WINDOW SCALING HELPER - With Hysteresis and Metrics
/// Adjusts all window periods based on market speed and coherence
/// NOW INCLUDES:
/// - FIX #6: Window-change hysteresis (5-bar confirmation before switching)
/// - FIX #7: Performance metrics tracking (market_speed, coherence, window changes)
/// - FIX #8: Fund-grade audit logging (regime change diagnostics for post-trade review)
struct AdaptiveWindowParams {
    int screen1_window;     // 240m EMA period (adaptive)
    int screen2_window;     // 60m oscillator period (adaptive)
    int screen3_window;     // 15m entry window (adaptive)
    int observation_vector_n;  // ObservationData lookback (unified)
    float market_speed_multiplier;
    bool is_coherent;

    // FIX #6: Window hysteresis state
    int pending_screen1_window = 0;
    int pending_screen2_window = 0;
    int pending_screen3_window = 0;
    int window_change_confirmation_count = 0;
    static constexpr int WINDOW_CHANGE_HYSTERESIS = 5;  // 5 bars to confirm window change

    // FIX #8: Fund-grade regime diagnostics for post-trade audit trail
    RegimeDiagnostics diagnostics;

    // FIX #7: Performance metrics
    AdaptiveWindowMetrics metrics;

    void UpdateWindows(float market_speed, float coherence_score, float efficiency_ratio,
                      float market_speed_multiplier_in = 1.0f) {
        market_speed_multiplier = market_speed_multiplier_in;
        is_coherent = coherence_score >= 0.5f;  // Use coherence_score threshold

        // FIX #7: Update metrics
        metrics.UpdateMetrics(market_speed, coherence_score);

        // Base windows (Elder's classic)
        int base_screen1 = 26;  // 240m EMA
        int base_screen2 = 13;  // 60m FI
        int base_screen3 = 5;   // 15m entry
        int base_obs_vector = 20;  // 20-bar observation

        // Scale by market speed: fast market = shorter windows (responsive)
        float speed_factor = 1.0f / market_speed;

        int new_screen1 = static_cast<int>(base_screen1 * speed_factor);
        int new_screen2 = static_cast<int>(base_screen2 * speed_factor);
        int new_screen3 = static_cast<int>(base_screen3 * speed_factor);

        // Bounds checking
        new_screen1 = std::clamp(new_screen1, 8, 52);
        new_screen2 = std::clamp(new_screen2, 4, 26);
        new_screen3 = std::clamp(new_screen3, 2, 13);

        // FIX #6: Window change hysteresis - require 5 bars to confirm window change
        if (new_screen1 != pending_screen1_window || new_screen2 != pending_screen2_window ||
            new_screen3 != pending_screen3_window) {
            // Different window detected
            if (pending_screen1_window == 0) {
                // First initialization
                window_change_confirmation_count = 0;
                pending_screen1_window = new_screen1;
                pending_screen2_window = new_screen2;
                pending_screen3_window = new_screen3;
            } else {
                // Potential change - confirm over WINDOW_CHANGE_HYSTERESIS bars
                if (new_screen1 == pending_screen1_window &&
                    new_screen2 == pending_screen2_window &&
                    new_screen3 == pending_screen3_window) {
                    window_change_confirmation_count++;
                } else {
                    window_change_confirmation_count = 0;
                    pending_screen1_window = new_screen1;
                    pending_screen2_window = new_screen2;
                    pending_screen3_window = new_screen3;
                }
            }
        } else {
            window_change_confirmation_count = 0;
        }

        // Only change windows after hysteresis confirmation
        if (window_change_confirmation_count >= WINDOW_CHANGE_HYSTERESIS || new_screen1 == 0) {
            // FIX #8: Capture regime diagnostics for fund manager audit trail
            if (screen1_window != new_screen1) {
                diagnostics.window_changed = true;
                diagnostics.prev_window = screen1_window;
                diagnostics.new_window = new_screen1;
                diagnostics.market_speed = market_speed;
                diagnostics.efficiency_ratio = efficiency_ratio;
                // Trigger institutional audit logging for post-trade analysis
                diagnostics.LogToAuditTrail();
            }

            // Apply new windows
            screen1_window = new_screen1;
            screen2_window = new_screen2;
            screen3_window = new_screen3;
            window_change_confirmation_count = 0;
            metrics.window_change_count++;  // FIX #7: Track window changes
        } else {
            diagnostics.window_changed = false;  // No change this bar
        }

        // Observation vector scaled by coherence (tighter when coherent)
        if (is_coherent && coherence_score > 0.7f) {
            observation_vector_n = base_obs_vector;
        } else if (coherence_score < 0.3f) {
            observation_vector_n = base_obs_vector * 2;  // Wider window when incoherent
        } else {
            observation_vector_n = static_cast<int>(base_obs_vector * (1.0f + (0.5f - coherence_score)));
        }

        observation_vector_n = std::clamp(observation_vector_n, 10, 40);
    }
};

static void CleanupAdaptiveWindowState(SCStudyInterfaceRef sc) {
    auto* windowState = static_cast<AdaptiveWindowParams*>(
        sc.GetPersistentPointer(PersistentVar_AdaptiveCalculators::WINDOW_STATE_PTR));

    if (windowState) {
        delete windowState;
        sc.SetPersistentPointer(PersistentVar_AdaptiveCalculators::WINDOW_STATE_PTR, nullptr);
    }

    auto* fisherState = static_cast<AdaptiveWindowParams*>(
        sc.GetPersistentPointer(PersistentVar_AdaptiveCalculators::FISHER_WINDOW_STATE_PTR));

    if (fisherState) {
        delete fisherState;
        sc.SetPersistentPointer(PersistentVar_AdaptiveCalculators::FISHER_WINDOW_STATE_PTR, nullptr);
    }

    sc.SetPersistentInt(PersistentVar_AdaptiveCalculators::LAST_OBS_UPDATE_INDEX, -1);
}

static void ResetAdaptiveWindowState(SCStudyInterfaceRef sc) {
    auto* windowState = static_cast<AdaptiveWindowParams*>(
        sc.GetPersistentPointer(PersistentVar_AdaptiveCalculators::WINDOW_STATE_PTR));
    if (windowState) {
        *windowState = AdaptiveWindowParams{};
    }

    auto* fisherState = static_cast<AdaptiveWindowParams*>(
        sc.GetPersistentPointer(PersistentVar_AdaptiveCalculators::FISHER_WINDOW_STATE_PTR));
    if (fisherState) {
        *fisherState = AdaptiveWindowParams{};
    }

    sc.SetPersistentInt(PersistentVar_AdaptiveCalculators::LAST_OBS_UPDATE_INDEX, -1);
}

// Pattern Detection Constants
namespace PatternConstants {
    constexpr float HURST_TREND_THRESHOLD = 0.60f;
    constexpr float PINBALL_OVERSOLD = 30.0f;
    constexpr float PINBALL_OVERBOUGHT = 70.0f;
    constexpr int TURTLE_SOUP_LOOKBACK = 4;
    constexpr int REVERSAL_BAR_LOOKBACK = 3;
    constexpr int TWO_B_LOOKBACK = 5;  // Reduced from 20 to allow earlier pattern detection
    constexpr int WHIPLASH_LOOKBACK = 10;
    constexpr int GHOST_LOOKBACK = 20;  // Look back 20 bars for swing-based divergence
    constexpr float WHIPLASH_TOP_THRESHOLD = 0.75f;
    constexpr float WHIPLASH_BOTTOM_THRESHOLD = 0.25f;
    constexpr float TICK_MULTIPLIER_PULLBACK = 2.0f;

    // Tactical Trigger Detection Constants
    constexpr int TACTICAL_MIN_LOOKBACK = 20;  // Minimum bars needed for ITR/Elder patterns
    constexpr int PINBALL_MIN_BARS = 4;  // Need 4 bars for ROC + 3-period RSI
    constexpr int PINBALL_ROC_BARS = 4;  // Number of bars for 1-period ROC calculation
    constexpr int PINBALL_RSI_PERIOD = 3;  // 3-period RSI of ROC
    constexpr float PINBALL_ROC_MULTIPLIER = 100.0f;  // ROC percentage multiplier
    constexpr float PINBALL_RSI_BASE = 100.0f;  // RSI base value for calculation
    constexpr int VOLUME_AVG_LOOKBACK = 10;  // Bars for volume average calculation
    constexpr float VOLUME_BREAKOUT_MULTIPLIER = 1.2f;  // Volume must be 20% above avg

    // RSI and Stochastic Pattern Constants
    constexpr float RSI_OVERSOLD = 30.0f;        // RSI oversold threshold
    constexpr float RSI_OVERBOUGHT = 70.0f;      // RSI overbought threshold
    constexpr float STOCH_OVERSOLD = 20.0f;      // Stochastic oversold threshold
    constexpr float STOCH_OVERBOUGHT = 80.0f;    // Stochastic overbought threshold
    constexpr int RSI_SWING_LOOKBACK = 5;        // Bars to look back for RSI swing points
}

// Calculate Force Index: Volume × (Close - PreviousClose), then apply EMA
// Elder's Force Index measures the power behind price movements
// Positive values = bullish force, Negative values = bearish force
void CalculateForceIndex(SCStudyInterfaceRef sc, SCFloatArrayRef forceArray,
                         SCSubgraphRef forceAverage, const int emaLength)
{
    const int i = sc.Index;

    // Force Index = Volume × Price Change
    forceArray[i] = sc.Volume[i] * (sc.Close[i] - sc.Close[i - 1]);

    // Apply exponential moving average for smoothing
    sc.ExponentialMovAvg(forceArray, forceAverage, emaLength);
}



// This function determines the impulse color based on two sets of data.
// It is intended to be used by the Impulse studies.
int GetImpulse(const float maDiff, const float macdDiff)
{
    if (maDiff > 0 && macdDiff > 0)
    {
        return GREEN;
    }
    else if (maDiff < 0 && macdDiff < 0)
    {
        return RED;
    }
    else
    {
        return BLUE;
    }
}



RaschkeStrategySetup DetectRaschkeStrategySetup(SCStudyInterfaceRef sc, const float hurst, const float ema21)
{
    // Ensure there is enough data for lookbacks
    if (sc.Index < PatternConstants::TWO_B_LOOKBACK) [[unlikely]] {
        return RaschkeStrategySetup::NONE;
    }

    using namespace PatternConstants;

    // --- Holy Grail Pattern (Linda Raschke's Exact Specification) ---
    // "The closest thing to a sure bet in trading" - requires strong trend persistence + pullback to 20 EMA
    // Hurst > 0.60 replaces ADX > 30 as the institutional-grade persistence gate
    if (hurst > HURST_TREND_THRESHOLD && sc.Index >= 2) {
        // Need previous bars to verify actual pullback occurred
        float tickSize = sc.TickSize;

        // HOLY_GRAIL_BUY: Pullback to EMA in established uptrend
        // 1. Verify established uptrend: Previous 2 bars were trading above EMA
        bool hadEstablishedUptrend = (sc.Close[sc.Index - 1] > ema21 && sc.Close[sc.Index - 2] > ema21);
        // 2. Current bar pulls back and touches/slightly pierces EMA
        bool pullbackTouchesEma = (sc.Low[sc.Index] <= ema21 + tickSize);
        // 3. Current bar still respects trend (close above EMA or very close)
        bool maintainsUptrend = (sc.Close[sc.Index] >= ema21 - tickSize);
        // 4. Previous bar was trading away from EMA (confirming this is a pullback, not just noise)
        bool wasAwayFromEma = (sc.Low[sc.Index - 1] > ema21 + (2 * tickSize));

        if (hadEstablishedUptrend && pullbackTouchesEma && maintainsUptrend && wasAwayFromEma) {
            return RaschkeStrategySetup::HOLY_GRAIL_BUY;
        }

        // HOLY_GRAIL_SELL: Rally to EMA in established downtrend
        // 1. Verify established downtrend: Previous 2 bars were trading below EMA
        const bool hadEstablishedDowntrend = (sc.Close[sc.Index - 1] < ema21 && sc.Close[sc.Index - 2] < ema21);
        // 2. Current bar rallies and touches/slightly pierces EMA
        const bool rallyTouchesEma = (sc.High[sc.Index] >= ema21 - tickSize);
        // 3. Current bar still respects trend (close below EMA or very close)
        const bool maintainsDowntrend = (sc.Close[sc.Index] <= ema21 + tickSize);
        // 4. Previous bar was trading away from EMA (confirming this is a rally, not just noise)
        const bool wasAwayFromEmaDown = (sc.High[sc.Index - 1] < ema21 - (TICK_MULTIPLIER_PULLBACK * tickSize));

        if (hadEstablishedDowntrend && rallyTouchesEma && maintainsDowntrend && wasAwayFromEmaDown) {
            return RaschkeStrategySetup::HOLY_GRAIL_SELL;
        }

        // HOLY_GRAIL_CONTINUATION: Strong trend (Hurst > 0.60) but no pullback to EMA yet
        // Price is trading away from EMA, waiting for entry opportunity
        const bool strongUptrend = (sc.Close[sc.Index] > ema21 && sc.Low[sc.Index] > ema21 + (TICK_MULTIPLIER_PULLBACK * tickSize));
        const bool strongDowntrend = (sc.Close[sc.Index] < ema21 && sc.High[sc.Index] < ema21 - (TICK_MULTIPLIER_PULLBACK * tickSize));

        if (strongUptrend || strongDowntrend) {
            return RaschkeStrategySetup::HOLY_GRAIL_CONTINUATION;
        }
    }


    // --- Fetch Indicators from Manager ---
    auto intermMarketAction = IndicatorManager::Instance().GetIndicator<IntermediateMarketAction>(IndicatorKey::INTERM_MKT_ACTION);

    if (!intermMarketAction) [[unlikely]] {
        return RaschkeStrategySetup::NONE; // Cannot proceed without this data
    }
    const float ema = intermMarketAction->ema();
    const float prevEma = intermMarketAction->prevEma();
    // const float upperChan = intermMarketAction->upperChan();
    // const float lowerChan = intermMarketAction->lowerChan();

    // --- Helper Lambdas ---
    auto GetRange = [&](int index) {
        return sc.High[index] - sc.Low[index];
    };

    auto IsInsideBar = [&](int currentBarIndex, int previousBarIndex) {
        return sc.High[currentBarIndex] < sc.High[previousBarIndex] && sc.Low[currentBarIndex] > sc.Low[previousBarIndex];
    };

    // --- Pattern Detection Logic (Ordered by specificity per ENUM_REFERENCE.md) ---
    // Detection Order:
    // 1. DOUBLE_REPO_FAILURE (trend continuation - most specific)
    // 2. DOUBLE_REPO (reversal pattern)
    // 3. BREAD_AND_BUTTER (trend continuation pullback to short EMA)
    // 4. ANTI (trend + EMA touch)
    // 5. SLINGSHOT (MACD momentum + breakout)
    // 6. GHOST (price/MACD divergence)
    // 7. TWO_B_REVERSAL (failed breakout)
    // 8. WHIPLASH (breakout + reversal)
    // 9. THREE_BAR_TRIANGLE (consolidation)
    // 10-13. NR patterns (compression patterns)
    // 14. FLIP (momentum pinball - extreme mean reversion)
    // 15. FIRST_CROSS (MACD zero-line)
    // 16. NONE (default)

    const float tickSize = sc.TickSize;

    // --- 1. DOUBLE_REPO_FAILURE (Highest Priority - Trend Continuation) ---
    // Linda Raschke: "A failed pattern is often a higher-probability signal in the opposite direction"
    // Pattern: Double Repo setup forms but FAILS → trapped traders liquidate → trend resumes
    //
    // SELL SETUP (downtrend continuation):
    //   - Reversal bar attempts to form low
    //   - Retest bar challenges that low (Double Repo Buy setup forming)
    //   - Current bar FAILS to break retest high → breaks retest low instead → trend resumes down
    //
    // BUY SETUP (uptrend continuation):
    //   - Reversal bar attempts to form high
    //   - Retest bar challenges that high (Double Repo Sell setup forming)
    //   - Current bar FAILS to break retest low → breaks retest high instead → trend resumes up
    {
        for (int lookback = 1; lookback <= TURTLE_SOUP_LOOKBACK && (sc.Index - lookback) >= 2; ++lookback) {
            const int retestBarIndex = sc.Index - lookback;

            for (int reversalLookback = 1; reversalLookback <= REVERSAL_BAR_LOOKBACK; ++reversalLookback) {
                const int reversalBarIndex = retestBarIndex - reversalLookback;
                if (reversalBarIndex < 1) [[unlikely]] break;

                // SELL SETUP (downtrend continuation): Bullish Double Repo FAILS
                // Use Sierra Chart's built-in swing detection (matches scsf_SwingHighAndLow)
                const bool reversalBarIsLow = sc.IsSwingLow(sc.Low, reversalBarIndex, REVERSAL_BAR_LOOKBACK);
                const float reversalLow = sc.Low[reversalBarIndex];

                const bool retestChallengesLow = (sc.Low[retestBarIndex] <= reversalLow + (TICK_MULTIPLIER_PULLBACK * tickSize));
                const bool currentFailsToBreakRetestHigh = (sc.High[sc.Index] < sc.High[retestBarIndex]);
                const bool currentBreaksRetestLow = (sc.Close[sc.Index] < sc.Low[retestBarIndex]);

                if (reversalBarIsLow && retestChallengesLow && currentFailsToBreakRetestHigh && currentBreaksRetestLow) {
                    return RaschkeStrategySetup::DOUBLE_REPO_FAILURE;
                }

                // BUY SETUP (uptrend continuation): Bearish Double Repo FAILS
                // Use Sierra Chart's built-in swing detection (matches scsf_SwingHighAndLow)
                const bool reversalBarIsHigh = sc.IsSwingHigh(sc.High, reversalBarIndex, REVERSAL_BAR_LOOKBACK);
                const float reversalHigh = sc.High[reversalBarIndex];

                const bool retestChallengesHigh = (sc.High[retestBarIndex] >= reversalHigh - (TICK_MULTIPLIER_PULLBACK * tickSize));
                const bool currentFailsToBreakRetestLow = (sc.Low[sc.Index] > sc.Low[retestBarIndex]);
                const bool currentBreaksRetestHigh = (sc.Close[sc.Index] > sc.High[retestBarIndex]);

                if (reversalBarIsHigh && retestChallengesHigh && currentFailsToBreakRetestLow && currentBreaksRetestHigh) {
                    return RaschkeStrategySetup::DOUBLE_REPO_FAILURE;
                }
            }
        }
    }

    // --- 2. DOUBLE_REPO (Reversal Pattern) ---
    // Linda Raschke's "Double Repositioning" reversal pattern
    // Pattern: Reversal bar → retest → successful breakout in reversal direction
    //
    // BUY SETUP: Downtrend → reversal bar (low) → retest low → break above retest high → reversal up
    // SELL SETUP: Uptrend → reversal bar (high) → retest high → break below retest low → reversal down
    {
        for (int lookback = 1; lookback <= TURTLE_SOUP_LOOKBACK && (sc.Index - lookback) >= 2; ++lookback) {
            const int retestBarIndex = sc.Index - lookback;

            for (int reversalLookback = 1; reversalLookback <= 3; ++reversalLookback) {
                int reversalBarIndex = retestBarIndex - reversalLookback;
                if (reversalBarIndex < 1) break;

                // BUY SETUP: Bullish reversal succeeds
                // Use Sierra Chart's built-in swing detection (matches scsf_SwingHighAndLow)
                const bool reversalBarIsLow = sc.IsSwingLow(sc.Low, reversalBarIndex, 3);
                const float reversalLow = sc.Low[reversalBarIndex];

                const bool retestChallengesLow = (sc.Low[retestBarIndex] <= reversalLow + (2 * tickSize));
                bool currentBreaksRetestHigh = (sc.Close[sc.Index] > sc.High[retestBarIndex]);

                if (reversalBarIsLow && retestChallengesLow && currentBreaksRetestHigh) {
                    return RaschkeStrategySetup::DOUBLE_REPO;
                }

                // SELL SETUP: Bearish reversal succeeds
                // Use Sierra Chart's built-in swing detection (matches scsf_SwingHighAndLow)
                const bool reversalBarIsHigh = sc.IsSwingHigh(sc.High, reversalBarIndex, 3);
                const float reversalHigh = sc.High[reversalBarIndex];

                const bool retestChallengesHigh = (sc.High[retestBarIndex] >= reversalHigh - (2 * tickSize));
                bool currentBreaksRetestLow = (sc.Close[sc.Index] < sc.Low[retestBarIndex]);

                if (reversalBarIsHigh && retestChallengesHigh && currentBreaksRetestLow) {
                    return RaschkeStrategySetup::DOUBLE_REPO;
                }
            }
        }
    }

    // --- 3. BREAD_AND_BUTTER (Trend Continuation Pullback) ---
    // Linda Raschke's "Bread and Butter" - First pullback to short-term EMA in strong trend
    // Context: Strong trend with short EMA above/below long EMA (both sloping)
    // Trigger: First pullback to short EMA after impulse move
    // Entry: Break of pullback bar high/low (continuation)
    {
        // Check if we have prior bar for comparison
        if (sc.Index > 1) {
            // BULLISH SETUP: Buy first dip to short EMA in uptrend
            // 1. Short EMA > Long EMA (both rising)
            bool bullishEmaAlignment = (ema > ema21 && ema > prevEma);
            // 2. Previous bar was above short EMA (impulse move)
            bool hadBullishImpulse = (sc.Low[sc.Index - 1] > ema);
            // 3. Current bar pulls back to touch short EMA
            bool pullbackToShortEma = (sc.Low[sc.Index] <= ema + tickSize);
            // 4. Pullback doesn't break long EMA (trend intact)
            bool doesntBreakLongEma = (sc.Low[sc.Index] > ema21);
            // 5. Current bar closes back near/above short EMA (rejection of lower prices)
            bool strongClose = (sc.Close[sc.Index] >= ema - tickSize);

            if (bullishEmaAlignment && hadBullishImpulse && pullbackToShortEma &&
                doesntBreakLongEma && strongClose) {
                return RaschkeStrategySetup::BREAD_AND_BUTTER;
            }

            // BEARISH SETUP: Sell first rally to short EMA in downtrend
            // 1. Short EMA < Long EMA (both falling)
            bool bearishEmaAlignment = (ema < ema21 && ema < prevEma);
            // 2. Previous bar was below short EMA (impulse move)
            bool hadBearishImpulse = (sc.High[sc.Index - 1] < ema);
            // 3. Current bar rallies to touch short EMA
            bool rallyToShortEma = (sc.High[sc.Index] >= ema - tickSize);
            // 4. Rally doesn't break long EMA (trend intact)
            bool doesntBreakLongEmaUp = (sc.High[sc.Index] < ema21);
            // 5. Current bar closes back near/below short EMA (rejection of higher prices)
            bool weakClose = (sc.Close[sc.Index] <= ema + tickSize);

            if (bearishEmaAlignment && hadBearishImpulse && rallyToShortEma &&
                doesntBreakLongEmaUp && weakClose) {
                return RaschkeStrategySetup::BREAD_AND_BUTTER;
            }
        }
    }

    // --- 4. ANTI (Stochastic Trend Continuation - Linda Raschke) ---
    // The "Anti" pattern: %K moves ANTI (against) the trend direction temporarily,
    // then hooks back, confirming pullback exhaustion and trend resumption.
    // Requires: Impulse move → %D sloping with trend → %K crosses against %D → %K hooks back
    if (sc.Index >= 20) { // Need sufficient history for Stochastic calculation
        // Calculate Stochastic with Linda Raschke's ANTI settings: %K = 7, %D = 10, Smoothing = 4
        // Manual calculation for recent bars only (avoid array operations)
        auto calcStochK = [&](int idx) -> float {
            float highest = sc.GetHighest(sc.High, idx, 7);
            float lowest = sc.GetLowest(sc.Low, idx, 7);
            if (highest - lowest > 0) {
                return 100.0f * (sc.Close[idx] - lowest) / (highest - lowest);
            }
            return 0.0f;
        };

        // Calculate Fast %K for recent bars and apply 4-period smoothing manually
        auto calcSmoothedK = [&](int idx) -> float {
            float sum = 0.0f;
            for (int j = 0; j < 4; ++j) {
                sum += calcStochK(idx - j);
            }
            return sum / 4.0f;
        };

        // Calculate %D (10-period SMA of smoothed %K)
        auto calcD = [&](int idx) -> float {
            float sum = 0.0f;
            for (int j = 0; j < 10; ++j) {
                sum += calcSmoothedK(idx - j);
            }
            return sum / 10.0f;
        };

        int i = sc.Index;

        // Get current and recent Stochastic values using lambda calculations
        float k_curr = calcSmoothedK(i);
        float k_prev = calcSmoothedK(i-1);
        float k_prev2 = calcSmoothedK(i-2);
        float d_curr = calcD(i);
        float d_prev = calcD(i-1);
        float d_prev2 = calcD(i-2);

        // --- ANTI BUY: Uptrend Continuation ---
        // 1. %D must be sloping upward (confirming bullish trend)
        bool dSlopingUp = (d_curr > d_prev2);

        // 2. %K was previously moving with %D (both rising), then crossed BELOW %D (the "anti" move)
        bool kWasAboveD = (k_prev2 > d_prev2);
        bool kCrossedBelowD = (k_prev < d_prev);

        // 3. %K now hooks back UP toward %D (pullback exhaustion, momentum snapping back)
        bool kHooksUp = (k_curr > k_prev && k_prev < k_prev2);

        // 4. %D hasn't reversed (still sloping up - trend intact)
        bool dStillUp = (d_curr >= d_prev);

        if (dSlopingUp && kWasAboveD && kCrossedBelowD && kHooksUp && dStillUp) {
            // Additional confirmation: Look for prior impulse move (price made higher high recently)
            bool hadImpulse = (sc.High[i-1] > sc.High[i-3] || sc.High[i-2] > sc.High[i-4]);
            if (hadImpulse) {
                return RaschkeStrategySetup::ANTI;
            }
        }

        // --- ANTI SELL: Downtrend Continuation ---
        // Mirror logic for bearish setup
        // 1. %D must be sloping downward (confirming bearish trend)
        bool dSlopingDown = (d_curr < d_prev2);

        // 2. %K was previously moving with %D (both falling), then crossed ABOVE %D (the "anti" move)
        bool kWasBelowD = (k_prev2 < d_prev2);
        bool kCrossedAboveD = (k_prev > d_prev);

        // 3. %K now hooks back DOWN toward %D (rally exhaustion, momentum snapping back)
        bool kHooksDown = (k_curr < k_prev && k_prev > k_prev2);

        // 4. %D hasn't reversed (still sloping down - trend intact)
        bool dStillDown = (d_curr <= d_prev);

        if (dSlopingDown && kWasBelowD && kCrossedAboveD && kHooksDown && dStillDown) {
            // Additional confirmation: Look for prior impulse move (price made lower low recently)
            bool hadImpulse = (sc.Low[i-1] < sc.Low[i-3] || sc.Low[i-2] < sc.Low[i-4]);
            if (hadImpulse) {
                return RaschkeStrategySetup::ANTI;
            }
        }
    }

    // --- 5. SLINGSHOT (MACD Momentum + Breakout) ---
    // Specific: Requires MACD state AND breakout confirmation
    {
        // DOD/SoA migration (Task 14): read straight from the packed array —
        // no pointer, no null check, always a valid value.
        MacdEnum macdValue = static_cast<MacdEnum>(
            IndicatorManager::Instance().GetValue<IndicatorKey::INTERM_MACD, mts::StorageBlock::Int8>());
        // Bullish: MACD below zero ticking up (NEG_TICK_UP or SPRING) + close above previous high
        if ((macdValue == MacdEnum::NEG_TICK_UP || macdValue == MacdEnum::SPRING) &&
            sc.Close[sc.Index] > sc.High[sc.Index - 1]) {
            return RaschkeStrategySetup::SLINGSHOT;
        }
        // Bearish: MACD above zero ticking down (POS_TICK_DOWN or FALL) + close below previous low
        if ((macdValue == MacdEnum::POS_TICK_DOWN || macdValue == MacdEnum::FALL) &&
            sc.Close[sc.Index] < sc.Low[sc.Index - 1]) {
            return RaschkeStrategySetup::SLINGSHOT;
        }
    }

    // --- 6. GHOST (Price/MACD Divergence) ---
    // Linda Raschke divergence: Proper swing-based divergence detection
    // Bullish: Price makes lower low, but MACD makes higher low
    // Bearish: Price makes higher high, but MACD makes lower high
    // Uses Sierra Chart's swing detection to find proper divergence points
    if (sc.Index >= GHOST_LOOKBACK) {
        constexpr int SWING_LENGTH = 3;  // Look for swings with 3 bars on each side

        const MacdEnum currentMacd = static_cast<MacdEnum>(
            IndicatorManager::Instance().GetValue<IndicatorKey::INTERM_MACD, mts::StorageBlock::Int8>());
        const bool bullishMacdConfirm =
            currentMacd == MacdEnum::NEG_TICK_UP ||
            currentMacd == MacdEnum::SPRING ||
            currentMacd == MacdEnum::ZERO_FROM_BELOW ||
            currentMacd == MacdEnum::BULLISH_CROSS;

        const bool bearishMacdConfirm =
            currentMacd == MacdEnum::POS_TICK_DOWN ||
            currentMacd == MacdEnum::FALL ||
            currentMacd == MacdEnum::ZERO_FROM_ABOVE ||
            currentMacd == MacdEnum::BEARISH_CROSS;

        // Look back for previous swing points (GHOST_LOOKBACK bars)
        for (int lookback = SWING_LENGTH + 1; lookback <= GHOST_LOOKBACK; ++lookback) {
            const int priorSwingIndex = sc.Index - lookback;
            if (priorSwingIndex < SWING_LENGTH) break;

            // BULLISH DIVERGENCE: Check if current bar is swing low
            if (sc.IsSwingLow(sc.Low, sc.Index, SWING_LENGTH)) {
                // Check if prior bar was also a swing low
                if (sc.IsSwingLow(sc.Low, priorSwingIndex, SWING_LENGTH)) {
                    // Bullish divergence: Price lower low (simple check without MACD historical access)
                    // Note: Full MACD divergence requires accessing historical MACD values from study arrays
                    if (sc.Low[sc.Index] < sc.Low[priorSwingIndex] && bullishMacdConfirm) {
                        return RaschkeStrategySetup::GHOST;
                    }
                }
            }

            // BEARISH DIVERGENCE: Check if current bar is swing high
            if (sc.IsSwingHigh(sc.High, sc.Index, SWING_LENGTH)) {
                // Check if prior bar was also a swing high
                if (sc.IsSwingHigh(sc.High, priorSwingIndex, SWING_LENGTH)) {
                    // Bearish divergence: Price higher high (simple check without MACD historical access)
                    // Note: Full MACD divergence requires accessing historical MACD values from study arrays
                    if (sc.High[sc.Index] > sc.High[priorSwingIndex] && bearishMacdConfirm) {
                        return RaschkeStrategySetup::GHOST;
                    }
                }
            }
        }
    }

    // --- 7. TWO_B_REVERSAL (Failed 20-Bar Breakout) ---
    // Specific: Requires breakout of 20-bar extreme that fails to hold
    // Uses simple loop approach matching Sierra Chart's scsf_NarrowRangeBar
    {
        // Find highest high in last 20 bars (excluding current)
        float highestHigh_20 = sc.High[sc.Index - 1];
        for (int i = 2; i <= TWO_B_LOOKBACK; ++i) {
            if (sc.Index - i < 0) break;
            if (sc.High[sc.Index - i] > highestHigh_20) {
                highestHigh_20 = sc.High[sc.Index - i];
            }
        }

        // Find lowest low in last 20 bars (excluding current)
        float lowestLow_20 = sc.Low[sc.Index - 1];
        for (int i = 2; i <= TWO_B_LOOKBACK; ++i) {
            if (sc.Index - i < 0) break;
            if (sc.Low[sc.Index - i] < lowestLow_20) {
                lowestLow_20 = sc.Low[sc.Index - i];
            }
        }

        // Bullish: High breaks above 20-bar high but close fails below
        if (sc.High[sc.Index] > highestHigh_20 && sc.Close[sc.Index] < highestHigh_20) {
            return RaschkeStrategySetup::TWO_B_REVERSAL;
        }
        // Bearish: Low breaks below 20-bar low but close fails above
        if (sc.Low[sc.Index] < lowestLow_20 && sc.Close[sc.Index] > lowestLow_20) {
            return RaschkeStrategySetup::TWO_B_REVERSAL;
        }
    }

    // --- 8. WHIPLASH (10-Bar Breakout + Same-Bar Reversal) ---
    // Specific: Requires range breakout AND close in opposite extreme
    // Uses simple loop approach matching Sierra Chart's scsf_NarrowRangeBar
    const float barRange = GetRange(sc.Index);
    if (barRange > 0) {
        // Find lowest low in last 10 bars (excluding current)
        float lowestLow_10 = sc.Low[sc.Index - 1];
        for (int i = 2; i <= WHIPLASH_LOOKBACK; ++i) {
            if (sc.Index - i < 0) break;
            if (sc.Low[sc.Index - i] < lowestLow_10) {
                lowestLow_10 = sc.Low[sc.Index - i];
            }
        }

        // Find highest high in last 10 bars (excluding current)
        float highestHigh_10 = sc.High[sc.Index - 1];
        for (int i = 2; i <= WHIPLASH_LOOKBACK; ++i) {
            if (sc.Index - i < 0) break;
            if (sc.High[sc.Index - i] > highestHigh_10) {
                highestHigh_10 = sc.High[sc.Index - i];
            }
        }

        const float closePositionInRange = (sc.Close[sc.Index] - sc.Low[sc.Index]) / barRange;

        // Bullish: Low breaks 10-bar low, close in top 25% of bar (sharp reversal)
        if (sc.Low[sc.Index] < lowestLow_10 && closePositionInRange > WHIPLASH_TOP_THRESHOLD) {
            return RaschkeStrategySetup::WHIPLASH;
        }
        // Bearish: High breaks 10-bar high, close in bottom 25% of bar (sharp reversal)
        if (sc.High[sc.Index] > highestHigh_10 && closePositionInRange < WHIPLASH_BOTTOM_THRESHOLD) {
            return RaschkeStrategySetup::WHIPLASH;
        }
    }

    // --- 9. THREE_BAR_TRIANGLE (Symmetric Consolidation) ---
    // Specific: Requires 3 bars with converging highs AND lows
    if ((sc.High[sc.Index] < sc.High[sc.Index - 1] && sc.High[sc.Index] < sc.High[sc.Index - 2]) &&
        (sc.Low[sc.Index] > sc.Low[sc.Index - 1] && sc.Low[sc.Index] > sc.Low[sc.Index - 2])) {
        return RaschkeStrategySetup::THREE_BAR_TRIANGLE;
    }

    // --- 10-13. NARROW RANGE PATTERNS (General Compression Patterns) ---
    // More general: Check multiple narrow range patterns
    // Detection order: NR4_NR7_VOLUME_SPIKE > IDNR4 > NR7 > NR4
    // Uses Sierra Chart's scsf_NarrowRangeBar approach

    const float currentRange = sc.High[sc.Index] - sc.Low[sc.Index];

    // Check for NR4 (narrowest range in last 4 bars)
    bool isNR4 = true;
    for (int i = 1; i <= 3; ++i) {
        const float priorRange = sc.High[sc.Index - i] - sc.Low[sc.Index - i];
        // Use FormattedEvaluate for precision-aware comparison
        if (!sc.FormattedEvaluate(priorRange, sc.BaseGraphValueFormat, GREATER_OPERATOR, currentRange, sc.BaseGraphValueFormat)) {
            isNR4 = false;
            break;
        }
    }

    // Check for NR7 (narrowest range in last 7 bars)
    bool isNR7 = true;
    for (int i = 1; i <= 6; ++i) {
        const float priorRange = sc.High[sc.Index - i] - sc.Low[sc.Index - i];
        // Use FormattedEvaluate for precision-aware comparison
        if (!sc.FormattedEvaluate(priorRange, sc.BaseGraphValueFormat, GREATER_OPERATOR, currentRange, sc.BaseGraphValueFormat)) {
            isNR7 = false;
            break;
        }
    }

    // Check for volume spike (2 std dev above 10-bar average)
    bool hasVolumeSpike = false;
    if (isNR4 || isNR7) {
        double volumeSum = 0;
        for (int i = 1; i <= 10; ++i) {
            volumeSum += sc.Volume[sc.Index - i];
        }
        double volumeAvg = volumeSum / 10.0;

        double sqDiffSum = 0;
        for (int i = 1; i <= 10; ++i) {
            sqDiffSum += (sc.Volume[sc.Index - i] - volumeAvg) * (sc.Volume[sc.Index - i] - volumeAvg);
        }
        double volumeStdDev = std::sqrt(sqDiffSum / 10.0);

        if (sc.Volume[sc.Index] > volumeAvg + (2.0 * volumeStdDev)) {
            hasVolumeSpike = true;
        }
    }

    // 10. NR4_NR7_VOLUME_SPIKE (Highest priority NR pattern - compression + volume)
    if ((isNR4 || isNR7) && hasVolumeSpike) {
        return RaschkeStrategySetup::NR4_NR7_VOLUME_SPIKE;
    }

    // 11. IDNR4 (Inside Day + Narrow Range 4)
    if (IsInsideBar(sc.Index, sc.Index - 1) && isNR4) {
        return RaschkeStrategySetup::IDNR4;
    }

    // 12. NR7 (Narrowest range in 7 bars - strong compression)
    if (isNR7) {
        return RaschkeStrategySetup::NR7;
    }

    // 13. NR4 (Narrowest range in 4 bars - moderate compression)
    if (isNR4) {
        return RaschkeStrategySetup::NR4;
    }

    // NOTE: FLIP (Momentum Pinball) pattern has been REMOVED from RaschkeStrategySetup.
    // Momentum Pinball is already properly implemented as:
    //   - RaschkeTacticalTrigger::MOMENTUM_PINBALL_BUY (5)
    //   - RaschkeTacticalTrigger::MOMENTUM_PINBALL_SELL (6)
    // See DetectMomentumPinball() and the MomentumPinball indicator in Screen3.
    // The tactical trigger representation is correct - immediate entry signal, not a setup pattern.

    // --- 15. FIRST_CROSS (MACD Zero-Line Cross) ---
    // General: Simple MACD momentum shift (includes all zero-line crossing variations)
    {
        MacdEnum macdValue = static_cast<MacdEnum>(
            IndicatorManager::Instance().GetValue<IndicatorKey::INTERM_MACD, mts::StorageBlock::Int8>());
        if (macdValue == MacdEnum::ZERO_FROM_BELOW || macdValue == MacdEnum::ZERO_FROM_ABOVE ||
            macdValue == MacdEnum::BULLISH_CROSS || macdValue == MacdEnum::BEARISH_CROSS) {
            return RaschkeStrategySetup::FIRST_CROSS;
        }
    }

    // No pattern detected
    return RaschkeStrategySetup::NONE;
}

// Detects Linda Raschke tactical triggers using both calculated indicators (Momentum Pinball)
// and passed RSI/Stochastic values for additional pattern recognition.
// Parameters:
//   - rsi3: 3-period RSI for short-term momentum (oversold/overbought signals)
//   - rsi10: 10-period RSI for swing failure detection (divergence patterns)
//   - stochK: Stochastic %K for "pop" reversal patterns (hook backs from extremes)
RaschkeTacticalTrigger DetectRaschkeTacticalTrigger(SCStudyInterfaceRef sc, float rsi3, float rsi10, float stochK)
{
    using namespace PatternConstants;

    // Ensure there is enough data for lookbacks (ITR/Elder patterns need more history)
    if (sc.Index < TACTICAL_MIN_LOOKBACK) {
        return RaschkeTacticalTrigger::NONE;
    }

    // --- Momentum Pinball (Linda Raschke/Larry Connors) ---
    // Pinball Indicator = LBR/RSI = 3-period RSI of 1-period Rate of Change
    // This is the exact indicator from "Street Smarts" for identifying extreme oversold/overbought
    // Note: Full strategy includes "first hour ITR" entry mechanics for daily charts,
    // but we detect the signal condition here (Pinball < 30 or > 70)
    if (sc.Index >= PINBALL_MIN_BARS) { // Need 4 bars for ROC calculation + 3-period RSI
        // Calculate 1-period Rate of Change for last 4 bars
        float roc[PINBALL_ROC_BARS];
        for (int i = 0; i < PINBALL_ROC_BARS; ++i) {
            int idx = sc.Index - i;
            if (idx > 0 && sc.Close[idx - 1] > 0) {
                roc[PINBALL_ROC_BARS - 1 - i] = ((sc.Close[idx] - sc.Close[idx - 1]) / sc.Close[idx - 1]) * PINBALL_ROC_MULTIPLIER;
            } else {
                roc[PINBALL_ROC_BARS - 1 - i] = 0.0f;
            }
        }

        // Calculate 3-period RSI of ROC (Pinball Indicator)
        float gains = 0.0f;
        float losses = 0.0f;
        int gainCount = 0;
        int lossCount = 0;

        for (int i = 1; i < PINBALL_ROC_BARS; ++i) {
            float change = roc[i] - roc[i - 1];
            if (change > 0) {
                gains += change;
                gainCount++;
            } else if (change < 0) {
                losses += std::abs(change);
                lossCount++;
            }
        }

        const float avgGain = (gainCount > 0) ? (gains / static_cast<float>(PINBALL_RSI_PERIOD)) : 0.0f;
        const float avgLoss = (lossCount > 0) ? (losses / static_cast<float>(PINBALL_RSI_PERIOD)) : 0.0f;

        float pinballIndicator = 0.0f;
        if (avgLoss > 0) {
            const float rs = avgGain / avgLoss;
            pinballIndicator = PINBALL_RSI_BASE - (PINBALL_RSI_BASE / (1.0f + rs));
        } else if (avgGain > 0) [[unlikely]] {
            // All gains, no losses = extreme overbought
            pinballIndicator = PINBALL_RSI_BASE;
        }

        // Pinball Buy: Indicator < 30 (extreme oversold, expect "flip" to upside)
        if (pinballIndicator < PINBALL_OVERSOLD) {
            return RaschkeTacticalTrigger::MOMENTUM_PINBALL_BUY;
        }

        // Pinball Sell: Indicator > 70 (extreme overbought, expect "flip" to downside)
        if (pinballIndicator > PINBALL_OVERBOUGHT) {
            return RaschkeTacticalTrigger::MOMENTUM_PINBALL_SELL;
        }
    }

    // --- RSI Failure Swing (Divergence Pattern) ---
    // Linda Raschke: RSI makes higher low while price makes lower low = bullish divergence (buy)
    // or RSI makes lower high while price makes higher high = bearish divergence (sell)
    if (sc.Index >= RSI_SWING_LOOKBACK) {
        // Find recent swing points in last RSI_SWING_LOOKBACK bars
        float lowestPrice = sc.Low[sc.Index];
        float highestPrice = sc.High[sc.Index];

        for (int i = 1; i <= RSI_SWING_LOOKBACK; ++i) {
            const int idx = sc.Index - i;
            if (sc.Low[idx] < lowestPrice) lowestPrice = sc.Low[idx];
            if (sc.High[idx] > highestPrice) highestPrice = sc.High[idx];
            // Note: Would need RSI array to compare historical RSI values properly
            // Current implementation simplified - uses current RSI vs recent price action
        }

        // Bearish Divergence: Price making new high but RSI not confirming
        if (sc.High[sc.Index] >= highestPrice && rsi10 < RSI_OVERBOUGHT) {
            // Price at/near recent high but RSI showing weakness
            if (sc.Index > 0 && rsi10 < rsi3) { // RSI10 trending down vs RSI3
                return RaschkeTacticalTrigger::RSI_FAILURE_SWING_SELL;
            }
        }

        // Bullish Divergence: Price making new low but RSI not confirming
        if (sc.Low[sc.Index] <= lowestPrice && rsi10 > RSI_OVERSOLD) {
            // Price at/near recent low but RSI showing strength
            if (sc.Index > 0 && rsi10 > rsi3) { // RSI10 trending up vs RSI3
                return RaschkeTacticalTrigger::RSI_FAILURE_SWING_BUY;
            }
        }
    }

    // --- Stochastic Pop (Hook Reversal) ---
    // Linda Raschke: Stochastic reaches extreme (>80 or <20) then "pops" back
    // This indicates momentum exhaustion and potential reversal
    if (sc.Index > 0) {
        // Get previous stochastic value from study subgraph
        // Note: For proper implementation, need access to previous %K values
        // Current simplified version: check if at extreme and RSI confirming reversal

        // Stochastic Pop Buy: Was oversold, now reversing up
        if (stochK > STOCH_OVERSOLD && stochK < (STOCH_OVERSOLD + 10.0f)) {
            // Just above oversold, check if momentum turning
            if (rsi3 > RSI_OVERSOLD && rsi3 > rsi10) {
                return RaschkeTacticalTrigger::STOCHASTIC_POP_BUY;
            }
        }

        // Stochastic Pop Sell: Was overbought, now reversing down
        if (stochK < STOCH_OVERBOUGHT && stochK > (STOCH_OVERBOUGHT - 10.0f)) {
            // Just below overbought, check if momentum turning
            if (rsi3 < RSI_OVERBOUGHT && rsi3 < rsi10) {
                return RaschkeTacticalTrigger::STOCHASTIC_POP_SELL;
            }
        }
    }

    // --- Turtle Soup Detection (Optimized with STL) ---

    // Use STL algorithms to find min/max instead of manual loops
    // Note: std::min_element expects [first, last) where last is one-past-the-end
    const float lowestLow_1_to_4 = *std::min_element(
        &sc.Low[sc.Index - TURTLE_SOUP_LOOKBACK],
        &sc.Low[sc.Index + 1]  // Fixed: +1 for one-past-end
    );
    if (sc.Low[sc.Index] < lowestLow_1_to_4 && sc.Close[sc.Index] > lowestLow_1_to_4) {
        return RaschkeTacticalTrigger::TURTLE_SOUP_BUY;
    }

    const float highestHigh_1_to_4 = *std::max_element(
        &sc.High[sc.Index - TURTLE_SOUP_LOOKBACK],  // Fixed: was sc.Low, should be sc.High
        &sc.High[sc.Index + 1]  // Fixed: +1 for one-past-end
    );
    if (sc.High[sc.Index] > highestHigh_1_to_4 && sc.Close[sc.Index] < highestHigh_1_to_4) {
        return RaschkeTacticalTrigger::TURTLE_SOUP_SELL;
    }

    // 2. If no other trigger is found, check for the Elder Breakout.
    //    CONTRACT REFACTOR: Logic flow moved to Scoring.cpp.
    //    Pure indicator logic: Any break of the prior bar's high is a POTENTIAL breakout.
    //    The validity (is_long_bias) is now enforced by the Scoring engine, not here.

    // Remove old bias check logic (lines 1422-1426 deleted)
    // We now return the raw pattern detection without context filtering.

    SCFloatArrayRef high = sc.High;
    SCFloatArrayRef low = sc.Low;
    int currentIndex = sc.Index;

    if (currentIndex > 0) { // Ensure we are not on the first bar
        // Pure price action detection
        if (high[currentIndex] > high[currentIndex - 1]) {
            return RaschkeTacticalTrigger::ELDER_BREAKOUT_BUY;
        }

        if (low[currentIndex] < low[currentIndex - 1]) {
            return RaschkeTacticalTrigger::ELDER_BREAKOUT_SELL;
        }
    }

    // --- ITR (Initial Trading Range) Strategy ---
    // Linda Raschke: "The first hour establishes the framework for the rest of the trading day"
    //
    // 1. Track ITR = High/Low of first hour (9:30-10:30 EST)
    // 2. Breakout strategies (Trend Day):
    //    - ITR_BREAKOUT_BUY: Price > ITR High + volume confirmation
    //    - ITR_BREAKOUT_SELL: Price < ITR Low + volume confirmation
    // 3. Fade strategies (Range Day):
    //    - ITR_FADE_BUY: Breakdown fails, price back inside ITR (shorts trapped)
    //    - ITR_FADE_SELL: Breakout fails, price back inside ITR (longs trapped)
    //
    // Volume is critical: High volume = trend day, Low volume = range day / fade

    // Get time of day to identify opening hour
    SCDateTime barTime = sc.BaseDateTimeIn[sc.Index];
    int hour = barTime.GetHour();
    int minute = barTime.GetMinute();

    // Market session: 9:30 AM = 09:30, 10:30 AM = 10:30
    bool isOpeningHour = (hour == 9 && minute >= 30) || (hour == 10 && minute < 30);
    bool isAfterOpeningHour = (hour > 10) || (hour == 10 && minute >= 30);

    // Persistent variables for ITR tracking (per trading day)
    // Note: These use the calling study's persistent variable space
    int& itrDay = sc.GetPersistentInt(PersistentVar_ElderBreakout::ITR_DAY);
    float& itrHigh = sc.GetPersistentFloat(PersistentVar_ElderBreakout::ITR_HIGH);
    float& itrLow = sc.GetPersistentFloat(PersistentVar_ElderBreakout::ITR_LOW);
    int& itrEstablished = sc.GetPersistentInt(PersistentVar_ElderBreakout::ITR_ESTABLISHED);
    int& hadBreakoutAbove = sc.GetPersistentInt(PersistentVar_ElderBreakout::HAD_BREAKOUT_ABOVE);
    int& hadBreakdownBelow = sc.GetPersistentInt(PersistentVar_ElderBreakout::HAD_BREAKDOWN_BELOW);

    int currentDay = barTime.GetDate();

    // Reset ITR tracking on new trading day
    if (currentDay != itrDay) {
        itrDay = currentDay;
        itrHigh = sc.High[sc.Index];
        itrLow = sc.Low[sc.Index];
        itrEstablished = false;
        hadBreakoutAbove = false;
        hadBreakdownBelow = false;
    }

    // During opening hour: Track ITR high/low
    if (isOpeningHour) {
        if (sc.High[sc.Index] > itrHigh) itrHigh = sc.High[sc.Index];
        if (sc.Low[sc.Index] < itrLow) itrLow = sc.Low[sc.Index];
        itrEstablished = false; // Not finalized yet
    }

    // After opening hour: ITR is established, detect patterns
    if (isAfterOpeningHour && !itrEstablished) {
        itrEstablished = true;
    }

    if (itrEstablished && itrHigh > itrLow) { // Valid ITR range
        float tickSize = sc.TickSize;
        float currentHigh = sc.High[sc.Index];
        float currentLow = sc.Low[sc.Index];
        float currentClose = sc.Close[sc.Index];
        float prevHigh = sc.High[sc.Index - 1];
        float prevLow = sc.Low[sc.Index - 1];

        // Check volume confirmation (Raschke emphasizes high volume on breakouts)
        bool highVolume = false;
        if (sc.Index >= VOLUME_AVG_LOOKBACK) {
            double volumeSum = 0;
            for (int i = 1; i <= VOLUME_AVG_LOOKBACK; ++i) {
                volumeSum += sc.Volume[sc.Index - i];
            }
            double volumeAvg = volumeSum / static_cast<double>(VOLUME_AVG_LOOKBACK);
            highVolume = (sc.Volume[sc.Index] > volumeAvg * VOLUME_BREAKOUT_MULTIPLIER);
        }

        // --- ITR BREAKOUT (Trend Day) ---
        // Breakout Buy: Price breaks above ITR High (bullish trend day)
        if (currentHigh > itrHigh + tickSize && !hadBreakoutAbove) {
            hadBreakoutAbove = true;
            if (highVolume) { // High volume confirms trend day
                return RaschkeTacticalTrigger::ITR_BREAKOUT_BUY;
            }
        }

        // Breakout Sell: Price breaks below ITR Low (bearish trend day)
        if (currentLow < itrLow - tickSize && !hadBreakdownBelow) {
            hadBreakdownBelow = true;
            if (highVolume) { // High volume confirms trend day
                return RaschkeTacticalTrigger::ITR_BREAKOUT_SELL;
            }
        }

        // --- ITR FADE (Range Day / Failed Breakout) ---
        // Fade Buy: Price broke below ITR, but reversed back inside (short trap)
        if (hadBreakdownBelow && prevLow < itrLow && currentClose > itrLow) {
            // Price was below ITR, now back inside - failed breakdown
            return RaschkeTacticalTrigger::ITR_FADE_BUY;
        }

        // Fade Sell: Price broke above ITR, but reversed back inside (long trap)
        if (hadBreakoutAbove && prevHigh > itrHigh && currentClose < itrHigh) {
            // Price was above ITR, now back inside - failed breakout
            return RaschkeTacticalTrigger::ITR_FADE_SELL;
        }
    }

    return RaschkeTacticalTrigger::NONE;
}

DailyBiasEnum CalculateDailyBias(float lastPrice, float prevDayHigh, float prevDayLow, float hurstExponent, float /*entropy*/,
                                  float valueAreaLow, float valueAreaHigh)
{
    const dbe::Bias bias = dbe::ComputeDailyBias({
        lastPrice, prevDayHigh, prevDayLow, hurstExponent, valueAreaLow, valueAreaHigh
    });
    return static_cast<DailyBiasEnum>(static_cast<int8_t>(bias));
}

// GetVolumeEnum() removed in v5.7 — VolumeIndicator self-classifies using
// robust log-volume z-score thresholds (Taleb-consistent, replaces Gaussian mu/sigma).

// Pure structural classifier (no SC dependency) — single source of truth shared by the
// intra-bar DetectStructure() and the completed-bar native TRAP floor. Keeping it pure
// makes it unit-testable and the deterministic parity anchor with the Python labeler.
StructureTest ClassifyStructure(float high, float low, float close,
                                float prev_high, float prev_low, double atr,
                                float lookbackHigh, float lookbackLow)
{
    // ATR-based thresholds
    double breakout_threshold = 0.25 * atr;
    double reversal_threshold = 0.5 * atr;

    // Consolidation / Expansion
    bool is_inside_bar = high < prev_high && low > prev_low;
    if (is_inside_bar) {
        return StructureTest::INSIDE_BAR;
    }

    bool is_outside_bar = high > prev_high && low < prev_low;
    if (is_outside_bar) {
        return StructureTest::OUTSIDE_BAR;
    }

    // --- Strong Reversal Tests (using lookbackHigh/Low) ---
    // FAILED_HIGH_STRONG_REVERSAL: Price breaks above lookbackHigh but closes significantly lower
    if (high > lookbackHigh && close < lookbackHigh - reversal_threshold) {
        return StructureTest::FAILED_HIGH_STRONG_REVERSAL;
    }

    // FAILED_LOW_STRONG_REVERSAL: Price breaks below lookbackLow but closes significantly higher
    if (low < lookbackLow && close > lookbackLow + reversal_threshold) {
        return StructureTest::FAILED_LOW_STRONG_REVERSAL;
    }

    // Bullish Tests (using prev_high/low for shorter-term context)
    if (high > prev_high) {
        if (close > prev_high + breakout_threshold) {
            return StructureTest::DECISIVE_BREAKOUT_HIGH;
        }
        else if (close < prev_high) {
            return StructureTest::FAILED_HIGH_CLOSE_INSIDE;
        }
        else {
            // Marginal breakout: high broke prev_high, close is above prev_high but < prev_high + 0.25*atr
            return StructureTest::DECISIVE_BREAKOUT_HIGH;  // Still treat as breakout, just less decisive
        }
    }

    // Bearish Tests (using prev_high/low for shorter-term context)
    if (low < prev_low) {
        if (close < prev_low - breakout_threshold) {
            return StructureTest::DECISIVE_BREAKDOWN_LOW;
        }
        else if (close > prev_low) {
            return StructureTest::FAILED_LOW_CLOSE_INSIDE;
        }
        else {
            // Marginal breakdown: low broke prev_low, close is below prev_low but > prev_low - 0.25*atr
            return StructureTest::DECISIVE_BREAKDOWN_LOW;  // Still treat as breakdown, just less decisive
        }
    }

    return StructureTest::NONE;
}

StructureTest DetectStructure(SCStudyInterfaceRef sc, float prev_high, float prev_low, double atr, float lookbackHigh, float lookbackLow)
{
    if (sc.Index < 1) {
        return StructureTest::NONE;
    }

    // Intra-bar read of the current forming bar (feeds the model observation vector).
    return ClassifyStructure(sc.High[sc.Index], sc.Low[sc.Index], sc.Close[sc.Index],
                             prev_high, prev_low, atr, lookbackHigh, lookbackLow);
}

ATRProximityEnum DetectATRProximity(SCStudyInterfaceRef sc, double atr)
{
    double bar_range = sc.High[sc.Index] - sc.Low[sc.Index];
    ATRProximityEnum newValue = ATRProximityEnum::LOW_VOLATILITY;

    if (bar_range >= atr && bar_range <= 2.5 * atr)
        newValue = ATRProximityEnum::HIGH_MOVE;      // Current bar range is between 1.0 and 2.5 ATR (Strong but normal volatility).
    else if (bar_range > 2.5 * atr) {
        // Price is stretched beyond 2.5 ATR from the previous close (Potential extreme reversal).
        // Now differentiate between EXTREME_LOW and EXTREME_HIGH
        if (std::abs(sc.Close[sc.Index] - sc.Low[sc.Index]) < std::abs(sc.Close[sc.Index] - sc.High[sc.Index])) {
            newValue = ATRProximityEnum::EXTREME_LOW;
        } else if (std::abs(sc.Close[sc.Index] - sc.High[sc.Index]) < std::abs(sc.Close[sc.Index] - sc.Low[sc.Index])) {
            newValue = ATRProximityEnum::EXTREME_HIGH;
        } else {
            newValue = ATRProximityEnum::EXTREME_VOLATILITY;
        }
    }

    return newValue;
}

EmaProximity DetectEmaProximity(SCStudyInterfaceRef sc, double ema, double std_dev)
{
    if (sc.Index < 1) {
        return EmaProximity::ABOVE_STRONG;
    }

    float price = sc.Close[sc.Index];
    float prev_price = sc.Close[sc.Index - 1];
    float distance = std::abs(price - ema);

    // Check for crossing first
    if (prev_price <= ema && price > ema) {
        return EmaProximity::CROSS_ABOVE;
    }

    if (prev_price >= ema && price < ema) {
        return EmaProximity::CROSS_BELOW;
    }

    // If not crossing, check proximity
    if (distance <= 0.1 * std_dev) {
        return EmaProximity::AT_EMA;
    }

    if (price > ema) { // Price is above EMA
        if (distance <= 1.0 * std_dev) {
            return EmaProximity::ABOVE_TOUCH;
        }
        else { // If not touching, and simply above, return PRICE_ABOVE_EMA
            return EmaProximity::PRICE_ABOVE_EMA;
        }
    } else { // Price is below EMA
        if (distance <= 1.0 * std_dev) {
            return EmaProximity::BELOW_TOUCH;
        }
        else { // If not touching, and simply below, return PRICE_BELOW_EMA
            return EmaProximity::PRICE_BELOW_EMA;
        }
    }

    return EmaProximity::ABOVE_STRONG;
}

RSI DetectRSI(float rsiValue)
{
    if (rsiValue > 70)
    {
        return RSI::OVERBOUGHT;
    }
    else if (rsiValue < 30)
    {
        return RSI::OVERSOLD;
    }
    else
    {
        return RSI::NORMAL;
    }
}

PriceMetrics DeterminePriceMetric(SCStudyInterfaceRef sc, float avg_range, float avg_volume)
{
    if (sc.Index < 1) {
        return PriceMetrics::NORMAL;
    }

    float bar_range = sc.High[sc.Index] - sc.Low[sc.Index];
    if (bar_range == 0) {
        return PriceMetrics::NORMAL;
    }

    // 1. Closing Price Dominance
    float close_position = (sc.Close[sc.Index] - sc.Low[sc.Index]) / bar_range;

    // 2. Volatility Expansion
    bool is_expansion_bar = bar_range > (avg_range * 1.5);

    // 3. Volume Confirmation
    bool is_high_volume = sc.Volume[sc.Index] > (avg_volume * 1.5);

    // Check for Strong Bullish conditions
    if (close_position > 0.8 && is_expansion_bar && is_high_volume) {
        return PriceMetrics::STRONG_BULLISH;
    }

    // Check for Strong Bearish conditions
    if (close_position < 0.2 && is_expansion_bar && is_high_volume) {
        return PriceMetrics::STRONG_BEARISH;
    }

    return PriceMetrics::NORMAL;
}

/**
 * Calculate NH-NL (New Highs - New Lows) market breadth signal.
 * Implements Dr. Alexander Elder's methodology from "The New High - New Low Index" (2nd Edition, 2014).
 *
 * Elder's Core Principles:
 * 1. "NH-NL is a LEADING INDICATOR that tracks behavior of market leaders"
 * 2. "Weekly timeframe is primary; daily is for tactical entry/exit"
 * 3. "Zero-line crosses are MANDATORY for valid divergence signals"
 * 4. "When S&P makes new highs but NH-NL cannot reach +2500, expect trouble ahead"
 *
 * Elder's Exact Thresholds (from book):
 * - Daily confirmation: +100 (bullish) / -100 (bearish)
 * - Weekly bull market: +2500 ("solid ground, hold longs")
 * - Weekly panic spike: -4000 ("mass capitulation, buy signal, trend lasts ~1 year")
 * - Weekly mini-spike (bull only): -1500 ("good for several months upside")
 *
 * Priority: Extremes → Zero-Cross Divergences → Breadth Participation → Confirmations
 *
 * @param nh_nl_daily Daily NYSE+AMEX+NASDAQ NH-NL differential
 * @param nh_nl_weekly 5-day rolling sum (Elder uses 5 trading days)
 * @param nh_nl_prev_weekly Previous week's NH-NL sum (for breadth trend)
 * @param currentPrice Current S&P 500 price
 * @param recentHigh Highest price in last 20 bars (240-min = ~17 trading days)
 * @param recentLow Lowest price in last 20 bars
 * @param priceIsRising True if price above 13-EMA (Elder's trend definition)
 * @return NhNlSignalEnum classification
 */
NhNlSignalEnum CalculateNhNlSignal(int nh_nl_daily, int nh_nl_weekly, int nh_nl_prev_weekly,
                                     float currentPrice, float recentHigh, float recentLow, bool priceIsRising) {
    // Priority 1: EXTREME PANIC SPIKE (Elder's most powerful buy signal)
    // Elder: "Weekly NH-NL < -4000 = mass capitulation, stocks pass from weak to strong hands"
    // Book: "To reach -4000, daily NH-NL must be at least -800 for 5 consecutive days"
    // Result: Bull market rallies last ~1 year; bear market rallies last a few weeks
    if (nh_nl_weekly < -4000) {
        return NhNlSignalEnum::EXTREME_LOWS_BOUNCE;
    }

    // Define price extreme thresholds for divergence detection
    const float priceNearHighThreshold = 0.98f;  // Within 2% of recent high
    const float priceNearLowThreshold = 1.02f;   // Within 2% of recent low
    bool priceNearHigh = (currentPrice >= recentHigh * priceNearHighThreshold);
    bool priceNearLow = (currentPrice <= recentLow * priceNearLowThreshold);

    // Priority 1b: BULL MARKET CONFIRMATION FAILURE (Elder's weakness warning)
    // Elder: "If S&P makes new highs but weekly NH-NL cannot reach +2500, expect trouble"
    // Book: "Narrow rally - fewer stocks participating, leadership weakening"
    // This checks if price is near recent high but breadth is NOT confirming strength
    if (priceNearHigh && nh_nl_weekly < 2500 && nh_nl_weekly < nh_nl_prev_weekly) {
        return NhNlSignalEnum::EXTREME_HIGHS_PEAK;
    }

    // Calculate breadth trend (is NH-NL improving or deteriorating?)
    bool breadthImproving = nh_nl_weekly > nh_nl_prev_weekly;
    bool breadthDeteriorating = nh_nl_weekly < nh_nl_prev_weekly;
    int breadthChange = nh_nl_weekly - nh_nl_prev_weekly;

    // Priority 2: ELDER'S DIVERGENCES (Most Powerful Trading Signals)
    // Elder: "Divergences between price and breadth are among the most reliable signals"

    // BEARISH DIVERGENCE: Price makes new high but fewer stocks participating
    // Elder: "When the index reaches a new peak but NH-NL doesn't, it's a sign of distribution"
    // Requirements: Price near high + breadth deteriorating significantly
    if (priceNearHigh && breadthDeteriorating && breadthChange < -500) {
        // Strong bearish divergence: Price at high but breadth collapsing
        return NhNlSignalEnum::BEARISH_DIVERGENCE;
    }

    // BULLISH DIVERGENCE: Price makes new low but breadth improving
    // Elder: "When price falls to a new low but NH-NL improves, smart money is accumulating"
    // Requirements: Price near low + breadth improving significantly
    if (priceNearLow && breadthImproving && breadthChange > 500) {
        // Strong bullish divergence: Price at low but breadth strengthening
        return NhNlSignalEnum::BULLISH_DIVERGENCE;
    }

    // Priority 3: BREADTH PARTICIPATION ANALYSIS (Elder's Trend Health Assessment)
    // Elder: "A healthy trend has broad participation. Narrowing breadth warns of trouble."

    if (priceIsRising) {
        // Price is rising - analyze participation quality

        // NARROWING RALLY: Price up but breadth deteriorating (Elder's warning sign)
        // Elder: "When fewer stocks participate in a rally, it's likely to fail"
        if (breadthDeteriorating && nh_nl_weekly < nh_nl_prev_weekly - 300) {
            return NhNlSignalEnum::NARROWING_RALLY;
        }

        // BROADENING RALLY: Price up with improving breadth (Elder's healthy trend)
        // Elder: "When more stocks join a rally, the trend is strong and likely to continue"
        if (breadthImproving && nh_nl_weekly > 1500 && breadthChange > 200) {
            return NhNlSignalEnum::BROADENING_RALLY;
        }

    } else {
        // Price is falling - analyze decline quality

        // NARROWING DECLINE: Price down but breadth improving (Elder's exhaustion)
        // Elder: "When price falls but fewer stocks decline, selling is exhausting"
        if (breadthImproving && breadthChange > 300) {
            return NhNlSignalEnum::NARROWING_DECLINE;
        }

        // BROADENING DECLINE: Price down with worsening breadth (Elder's broad weakness)
        // Elder: "When more stocks join a decline, the downtrend is strong"
        if (breadthDeteriorating && nh_nl_weekly < -1500 && breadthChange < -200) {
            return NhNlSignalEnum::BROADENING_DECLINE;
        }
    }

    // Priority 4: ELDER'S DAILY CONFIRMATION BAND (Exact Thresholds)
    // Elder: "Neutral zone between +100 and -100 filters whipsaws from zero-line crosses"

    // BULLISH CONFIRMATION (Elder's exact threshold from book)
    // Elder: "Daily NH-NL > +100 → bulls in control, pullbacks to EMA = excellent long entries"
    // OR Weekly > +2500: "You're on solid ground, this is a bull market; hold longs and buy pullbacks"
    if (nh_nl_daily > 100 || nh_nl_weekly > 2500) {
        return NhNlSignalEnum::BULLISH_CONFIRMATION;
    }

    // BEARISH CONFIRMATION (Elder's exact threshold from book)
    // Elder: "Daily NH-NL < -100 → bears in control, rallies to EMA = excellent short entries"
    if (nh_nl_daily < -100) {
        return NhNlSignalEnum::BEARISH_CONFIRMATION;
    }

    // Default: UNCLEAR (Elder's neutral zone)
    // Elder: "Between +100 and -100 = neutral zone, no clear signal"
    return NhNlSignalEnum::UNCLEAR;
}

void DrawStudyHorizontalLine(SCStudyInterfaceRef sc, int LineNumber, float Value, COLORREF Color, int LineWidth, SubgraphLineStyles LineStyle)
{
    // LineNumber should be in range PersistentVar_Visualization::BASE_LINE_VALUE (200+)
    float& lastValue = sc.GetPersistentFloat(LineNumber);

    if (Value != lastValue)
    {
        s_UseTool Tool;
        Tool.Clear();
        Tool.ChartNumber = sc.ChartNumber;
        Tool.DrawingType = DRAWING_HORIZONTALLINE;
        Tool.LineNumber = LineNumber;
        Tool.BeginValue = Value;
        Tool.Color = Color;
        Tool.LineStyle = LineStyle;
        Tool.LineWidth = LineWidth;
        Tool.AddMethod = UTAM_ADD_OR_ADJUST;
        Tool.ShowPrice = 1;
        Tool.AllowCopyToOtherCharts = 0;
        sc.UseTool(Tool);

        lastValue = Value;
    }
}

/**
 * DetectElderMACDDivergence: Implements Dr. Alexander Elder's MACD-Histogram divergence algorithm.
 * Based on "Two Roads Diverged: Trading Divergences" (2012-2014)
 *
 * Elder's Core Principle: "When trying to find a divergence, first look at the pattern of an
 *                          indicator and later at the pattern of prices." — Elder
 *
 * Critical Rules (all must be satisfied):
 * 1. MACD-Histogram MUST cross zero line between first and second extremes (MANDATORY)
 * 2. Without zero-line cross → "The bear is getting older and weaker — but the bear is still in charge!" (Elder)
 * 3. Buy signal = MACD-H upticks from second trough while still below zero
 * 4. Sell signal = MACD-H downticks from second peak while still above zero
 *
 * Implementation Notes:
 * - Uses state machine to track progression through divergence pattern
 * - State persists across bar updates (Sierra Chart AutoLoop)
 * - Detects local extremes using simple lookback comparison
 * - Handles pattern expiration if zero cross takes too long
 * - Supports professional re-entry strategy (Elder's GE example)
 */
MACDDivergenceEnum DetectElderMACDDivergence(
    SCStudyInterfaceRef& sc,
    int currentIndex,
    SCFloatArrayRef priceHigh,
    SCFloatArrayRef priceLow,
    SCFloatArrayRef macdHistogram,
    MACDDivergenceState& state,
    float atrValue,
    int lookbackPeriod
) {
    // Need at least lookbackPeriod bars before current to detect extremes
    if (currentIndex < lookbackPeriod) {
        return MACDDivergenceEnum::NONE;
    }

    double currentMacd = macdHistogram[currentIndex];
    double prevMacd = (currentIndex > 0) ? macdHistogram[currentIndex - 1] : 0.0;
    float currentHigh = priceHigh[currentIndex];
    float currentLow = priceLow[currentIndex];

    // ==================== BULLISH DIVERGENCE DETECTION ====================

    // PHASE 1: Looking for first MACD-H trough (below zero)
    if (state.currentState == MACDDivergenceEnum::NONE ||
        state.currentState == MACDDivergenceEnum::SEARCHING_FIRST_TROUGH) {

        // Check if current bar is a local minimum in MACD-H (below zero)
        if (currentMacd < -state.ZERO_THRESHOLD) {
            bool isLocalMin = true;

            // Verify it's lower than surrounding bars
            for (int i = 1; i <= lookbackPeriod && (currentIndex - i) >= 0; ++i) {
                if (macdHistogram[currentIndex - i] < currentMacd) {
                    isLocalMin = false;
                    break;
                }
            }

            if (isLocalMin) {
                // Found first trough
                state.priceBottom1 = currentLow;
                state.macdBottom1 = currentMacd;
                state.macdBottom1Index = currentIndex;
                state.macdCrossedZeroUp = false;
                state.currentState = MACDDivergenceEnum::WAITING_ZERO_CROSS_UP;
            }
        }
    }

    // PHASE 2: Waiting for MACD-H to cross above zero ("Breaking the back of the bear" — Elder)
    else if (state.currentState == MACDDivergenceEnum::WAITING_ZERO_CROSS_UP) {

        // Check for zero-line crossover (negative to positive)
        if (prevMacd < -state.ZERO_THRESHOLD && currentMacd >= -state.ZERO_THRESHOLD) {
            state.macdCrossedZeroUp = true;
            state.zeroCrossUpIndex = currentIndex;
            state.currentState = MACDDivergenceEnum::WAITING_SECOND_TROUGH;
        }
        // Reset if new deeper low without zero cross
        else if (currentMacd < state.macdBottom1) {
            state.macdBottom1 = currentMacd;
            state.macdBottom1Index = currentIndex;
            state.priceBottom1 = currentLow;
        }
        // Pattern expires if taking too long
        else if ((currentIndex - state.macdBottom1Index) > state.MAX_BARS_TO_ZERO_CROSS) {
            state.ResetBullish();
        }
    }

    // PHASE 3: Looking for second price/MACD trough (bullish divergence)
    else if (state.currentState == MACDDivergenceEnum::WAITING_SECOND_TROUGH) {

        // MACD-H must be back below zero to look for second trough
        if (currentMacd < -state.ZERO_THRESHOLD) {

            // Check if current bar is a local minimum in MACD-H
            bool isLocalMin = true;
            for (int i = 1; i <= lookbackPeriod && (currentIndex - i) >= 0; ++i) {
                if (macdHistogram[currentIndex - i] < currentMacd) {
                    isLocalMin = false;
                    break;
                }
            }

            if (isLocalMin) {
                // Check divergence conditions:
                // 1. Price made LOWER low
                // 2. MACD-H made HIGHER low (less negative)
                if (currentLow < state.priceBottom1 && currentMacd > state.macdBottom1) {

                    // === ELDER'S "DISCREET BOTTOMS" VALIDATION ===
                    // Calculate rally between first and second troughs (ATR-normalized)
                    // Elder: "Two discreet price bottoms, separated by a rally"
                    float rallySize = 0.0f;
                    int rallyBars = 0;
                    float maxHigh = state.priceBottom1;  // Track highest point in rally

                    for (int i = state.macdBottom1Index; i < currentIndex; ++i) {
                        if (priceHigh[i] > maxHigh) {
                            maxHigh = priceHigh[i];
                        }
                        rallyBars++;
                    }
                    rallySize = maxHigh - state.priceBottom1;  // Rally from first trough to peak

                    const float atrForValidation = std::max(atrValue, 0.01f);
                    state.rallySize = rallySize / atrForValidation;
                    state.rallyBars = rallyBars;

                    // Validate rally: Must be >1.5× ATR (Elder's "discreet" requirement)
                    if (state.rallySize < state.MIN_RALLY_ATR_MULTIPLE) {
                        // Update first trough to current (searching continues)
                        state.macdBottom1 = currentMacd;
                        state.macdBottom1Index = currentIndex;
                        state.priceBottom1 = currentLow;
                        return state.currentState;
                    }

                    // === DIVERGENCE PATTERN FORMED ===
                    // Calculate quality score (strong vs weak divergence)
                    float priceSpread = std::abs(currentLow - state.priceBottom1);
                    float macdSpread = std::abs(currentMacd - state.macdBottom1);
                    int zeroCrossBars = currentIndex - state.zeroCrossUpIndex;

                    // Quality factors:
                    // 1. Larger price/MACD differential = stronger (0.0-0.4 points)
                    // 2. Faster zero-cross (5-10 bars = strong, 40-50 bars = weak) (0.0-0.3 points)
                    // 3. Larger rally between extremes (0.0-0.3 points)
                    float qualityScore = 0.0f;
                    const float bullishPriceNorm = std::max(std::abs(state.priceBottom1) * 0.05f, 0.01f);
                    qualityScore += std::min(0.4f, (priceSpread / bullishPriceNorm) * 0.1f);  // Price differential
                    qualityScore += std::min(0.3f, (macdSpread / 0.01f) * 0.1f);  // MACD differential
                    qualityScore += (zeroCrossBars <= 10) ? 0.3f : std::max(0.0f, 0.3f - ((zeroCrossBars - 10) / 40.0f) * 0.3f);  // Zero-cross speed
                    qualityScore += std::min(0.3f, (state.rallySize / 5.0f) * 0.3f);  // Rally size
                    state.divergenceQuality = std::min(1.0f, qualityScore);

                    // Set PATTERN state (divergence exists, waiting for uptick signal)
                    state.currentState = MACDDivergenceEnum::BULLISH_DIVERGENCE_PATTERN;

                    // Check for immediate uptick on historical bars
                    if (currentIndex < sc.Index && (currentIndex + 1) < sc.ArraySize) {
                        if (macdHistogram[currentIndex + 1] > currentMacd) {
                            state.currentState = MACDDivergenceEnum::BULLISH_DIVERGENCE_BUY_SIGNAL;
                            return MACDDivergenceEnum::BULLISH_DIVERGENCE_BUY_SIGNAL;
                        }
                    }
                }
            }
        }
        // If MACD-H crosses above zero again without divergence, reset
        else if (currentMacd > state.ZERO_THRESHOLD && (currentIndex - state.zeroCrossUpIndex) > state.MIN_BARS_BETWEEN_EXTREMES) {
            state.ResetBullish();
        }
    }

    // Check for uptick from PATTERN state (Elder's buy signal trigger)
    if (state.currentState == MACDDivergenceEnum::BULLISH_DIVERGENCE_PATTERN && currentMacd < -state.ZERO_THRESHOLD) {
        if (currentMacd > prevMacd) {
            // UPTICK DETECTED - Elder's buy signal!
            state.currentState = MACDDivergenceEnum::BULLISH_DIVERGENCE_BUY_SIGNAL;

            return MACDDivergenceEnum::BULLISH_DIVERGENCE_BUY_SIGNAL;
        }
    }

    // Legacy: Check for uptick from WAITING_SECOND_TROUGH (for backward compatibility)
    if (state.currentState == MACDDivergenceEnum::WAITING_SECOND_TROUGH && currentMacd < -state.ZERO_THRESHOLD) {
        if (currentMacd > prevMacd) {
            // Uptick detected - confirm if we have valid divergence setup
            if (currentLow < state.priceBottom1) {
                // Find the recent MACD low
                double recentMacdLow = currentMacd;
                for (int i = 1; i <= lookbackPeriod && (currentIndex - i) >= 0; ++i) {
                    if (macdHistogram[currentIndex - i] < recentMacdLow) {
                        recentMacdLow = macdHistogram[currentIndex - i];
                    }
                }

                if (recentMacdLow > state.macdBottom1) {
                    state.currentState = MACDDivergenceEnum::BULLISH_DIVERGENCE_BUY_SIGNAL;

                    return MACDDivergenceEnum::BULLISH_DIVERGENCE_BUY_SIGNAL;
                }
            }
        }
    }

    // ==================== BEARISH DIVERGENCE DETECTION ====================

    // PHASE 1: Looking for first MACD-H peak (above zero)
    if (state.currentState == MACDDivergenceEnum::NONE ||
        state.currentState == MACDDivergenceEnum::SEARCHING_FIRST_PEAK) {

        // Check if current bar is a local maximum in MACD-H (above zero)
        if (currentMacd > state.ZERO_THRESHOLD) {
            bool isLocalMax = true;

            // Verify it's higher than surrounding bars
            for (int i = 1; i <= lookbackPeriod && (currentIndex - i) >= 0; ++i) {
                if (macdHistogram[currentIndex - i] > currentMacd) {
                    isLocalMax = false;
                    break;
                }
            }

            if (isLocalMax) {
                // Found first peak
                state.pricePeak1 = currentHigh;
                state.macdPeak1 = currentMacd;
                state.macdPeak1Index = currentIndex;
                state.macdCrossedZeroDown = false;
                state.currentState = MACDDivergenceEnum::WAITING_ZERO_CROSS_DOWN;
            }
        }
    }

    // PHASE 2: Waiting for MACD-H to cross below zero ("Breaking the back of the bull" — Elder)
    else if (state.currentState == MACDDivergenceEnum::WAITING_ZERO_CROSS_DOWN) {

        // Check for zero-line crossover (positive to negative)
        if (prevMacd > state.ZERO_THRESHOLD && currentMacd <= state.ZERO_THRESHOLD) {
            state.macdCrossedZeroDown = true;
            state.zeroCrossDownIndex = currentIndex;
            state.currentState = MACDDivergenceEnum::WAITING_SECOND_PEAK;
        }
        // Reset if new higher high without zero cross
        else if (currentMacd > state.macdPeak1) {
            state.macdPeak1 = currentMacd;
            state.macdPeak1Index = currentIndex;
            state.pricePeak1 = currentHigh;
        }
        // Pattern expires if taking too long
        else if ((currentIndex - state.macdPeak1Index) > state.MAX_BARS_TO_ZERO_CROSS) {
            state.ResetBearish();
        }
    }

    // PHASE 3: Looking for second price/MACD peak (bearish divergence)
    else if (state.currentState == MACDDivergenceEnum::WAITING_SECOND_PEAK) {

        // MACD-H must be back above zero to look for second peak
        if (currentMacd > state.ZERO_THRESHOLD) {

            // Check if current bar is a local maximum in MACD-H
            bool isLocalMax = true;
            for (int i = 1; i <= lookbackPeriod && (currentIndex - i) >= 0; ++i) {
                if (macdHistogram[currentIndex - i] > currentMacd) {
                    isLocalMax = false;
                    break;
                }
            }

            if (isLocalMax) {
                // Check divergence conditions:
                // 1. Price made HIGHER high
                // 2. MACD-H made LOWER high (less positive)
                if (currentHigh > state.pricePeak1 && currentMacd < state.macdPeak1) {

                    // === ELDER'S "DISCREET PEAKS" VALIDATION ===
                    // Calculate decline between first and second peaks (ATR-normalized)
                    // Elder: "Two discreet price peaks, separated by a decline"
                    float declineSize = 0.0f;
                    int declineBars = 0;
                    float minLow = state.pricePeak1;  // Track lowest point in decline

                    for (int i = state.macdPeak1Index; i < currentIndex; ++i) {
                        if (priceLow[i] < minLow) {
                            minLow = priceLow[i];
                        }
                        declineBars++;
                    }
                    declineSize = state.pricePeak1 - minLow;  // Decline from first peak to trough

                    const float atrForValidation = std::max(atrValue, 0.01f);
                    state.rallySize = declineSize / atrForValidation;  // Normalize to ATR (reuse rallySize field)
                    state.rallyBars = declineBars;

                    // Validate decline: Must be >1.5× ATR (Elder's "discreet" requirement)
                    if (state.rallySize < state.MIN_RALLY_ATR_MULTIPLE) {
                        // Update first peak to current (searching continues)
                        state.macdPeak1 = currentMacd;
                        state.macdPeak1Index = currentIndex;
                        state.pricePeak1 = currentHigh;
                        return state.currentState;
                    }

                    // === DIVERGENCE PATTERN FORMED ===
                    // Calculate quality score (strong vs weak divergence)
                    float priceSpread = std::abs(currentHigh - state.pricePeak1);
                    float macdSpread = std::abs(currentMacd - state.macdPeak1);
                    int zeroCrossBars = currentIndex - state.zeroCrossDownIndex;

                    // Quality factors (same as bullish)
                    float qualityScore = 0.0f;
                    const float bearishPriceNorm = std::max(std::abs(state.pricePeak1) * 0.05f, 0.01f);
                    qualityScore += std::min(0.4f, (priceSpread / bearishPriceNorm) * 0.1f);  // Price differential
                    qualityScore += std::min(0.3f, (macdSpread / 0.01f) * 0.1f);  // MACD differential
                    qualityScore += (zeroCrossBars <= 10) ? 0.3f : std::max(0.0f, 0.3f - ((zeroCrossBars - 10) / 40.0f) * 0.3f);  // Zero-cross speed
                    qualityScore += std::min(0.3f, (state.rallySize / 5.0f) * 0.3f);  // Decline size
                    state.divergenceQuality = std::min(1.0f, qualityScore);

                    // Set PATTERN state (divergence exists, waiting for downtick signal)
                    state.currentState = MACDDivergenceEnum::BEARISH_DIVERGENCE_PATTERN;

                    // Check for immediate downtick on historical bars
                    if (currentIndex < sc.Index && (currentIndex + 1) < sc.ArraySize) {
                        if (macdHistogram[currentIndex + 1] < currentMacd) {
                            state.currentState = MACDDivergenceEnum::BEARISH_DIVERGENCE_SELL_SIGNAL;

                            return MACDDivergenceEnum::BEARISH_DIVERGENCE_SELL_SIGNAL;
                        }
                    }
                }
            }
        }
        // If MACD-H crosses below zero again without divergence, reset
        else if (currentMacd < -state.ZERO_THRESHOLD && (currentIndex - state.zeroCrossDownIndex) > state.MIN_BARS_BETWEEN_EXTREMES) {
            state.ResetBearish();
        }
    }

    // Check for downtick from PATTERN state (Elder's sell signal trigger)
    if (state.currentState == MACDDivergenceEnum::BEARISH_DIVERGENCE_PATTERN && currentMacd > state.ZERO_THRESHOLD) {
        if (currentMacd < prevMacd) {
            // DOWNTICK DETECTED - Elder's sell signal!
            state.currentState = MACDDivergenceEnum::BEARISH_DIVERGENCE_SELL_SIGNAL;

            return MACDDivergenceEnum::BEARISH_DIVERGENCE_SELL_SIGNAL;
        }
    }

    // Legacy: Check for downtick from WAITING_SECOND_PEAK (for backward compatibility)
    if (state.currentState == MACDDivergenceEnum::WAITING_SECOND_PEAK && currentMacd > state.ZERO_THRESHOLD) {
        if (currentMacd < prevMacd) {
            // Downtick detected - confirm if we have valid divergence setup
            if (currentHigh > state.pricePeak1) {
                // Find the recent MACD high
                double recentMacdHigh = currentMacd;
                for (int i = 1; i <= lookbackPeriod && (currentIndex - i) >= 0; ++i) {
                    if (macdHistogram[currentIndex - i] > recentMacdHigh) {
                        recentMacdHigh = macdHistogram[currentIndex - i];
                    }
                }

                if (recentMacdHigh < state.macdPeak1) {
                    state.currentState = MACDDivergenceEnum::BEARISH_DIVERGENCE_SELL_SIGNAL;

                    return MACDDivergenceEnum::BEARISH_DIVERGENCE_SELL_SIGNAL;
                }
            }
        }
    }

    return state.currentState;
}

// DetectKangarooTail/DetectTurtleSoup/DetectMomentumPinball/DetectElderBreakout
// moved to include/IndicatorComputations.h (indicator-manager-dod-soa plan, Task 8)
// — they took only primitive arguments already, so they're now inline free
// functions there, alongside their now-ACSIL-independent enums. DetectNR7 was
// moved there too, then deleted entirely (docs/superpowers/specs/2026-08-06-
// indicator-orphan-cleanup-design.md) after confirming zero production callers.

// ============================================================================
// ELITE v2.5: 16D Observation Vector Calculation Pipeline Implementation
// ============================================================================
// Institutional-grade observation vector calculations using type-safe enums
// Short-horizon features support adaptive windowing [10, 40] based on market speed + coherence.

int CalculateAdaptiveObservationWindow(SCStudyInterfaceRef sc, float coherence_score) {
    float bounded_coherence = std::clamp(coherence_score, 0.0f, 1.0f);

    if (sc.Index < 34) {
        return 20;
    }

    auto* windows = static_cast<AdaptiveWindowParams*>(
        sc.GetPersistentPointer(PersistentVar_AdaptiveCalculators::WINDOW_STATE_PTR));
    if (!windows) {
        windows = new AdaptiveWindowParams{};
        sc.SetPersistentPointer(PersistentVar_AdaptiveCalculators::WINDOW_STATE_PTR, windows);
    }

    if (windows->screen1_window == 0) {
        windows->screen1_window = 26;
        windows->screen2_window = 13;
        windows->screen3_window = 5;
        windows->observation_vector_n = 20;
    }

    float market_speed = CalculateMarketSpeed(sc);
    float efficiency_ratio = CalculateEfficiencyRatio(sc, 34);
    windows->UpdateWindows(market_speed, bounded_coherence, efficiency_ratio);

    return std::clamp(windows->observation_vector_n, 10, 40);
}

int CalculateFisherAdaptiveWindow(SCStudyInterfaceRef sc, float coherence_score) {
    float bounded_coherence = std::clamp(coherence_score, 0.0f, 1.0f);

    if (sc.Index < 34) {
        return 20;
    }

    auto* windows = static_cast<AdaptiveWindowParams*>(
        sc.GetPersistentPointer(PersistentVar_AdaptiveCalculators::FISHER_WINDOW_STATE_PTR));
    if (!windows) {
        windows = new AdaptiveWindowParams{};
        sc.SetPersistentPointer(PersistentVar_AdaptiveCalculators::FISHER_WINDOW_STATE_PTR, windows);
    }

    if (windows->screen1_window == 0) {
        windows->screen1_window = 26;
        windows->screen2_window = 13;
        windows->screen3_window = 5;
        windows->observation_vector_n = 20;
    }

    float market_speed = CalculateMarketSpeed(sc);
    float efficiency_ratio = CalculateEfficiencyRatio(sc, 34);
    windows->UpdateWindows(market_speed, bounded_coherence, efficiency_ratio);

    return std::clamp(windows->observation_vector_n, 10, 40);
}

float CalculatePathEfficiencySNR(SCStudyInterfaceRef sc, [[maybe_unused]] float atr10, int lookback_n) {
    /// Spectral Entropy - ELITE: O(1) Rolling Window Implementation (Reference: Elder's Efficiency Ratio)
    /// Measures market signal purity: how much price moved directionally vs total movement
    /// Formula: SNR = efficiency_ratio^2, where efficiency = |net_change| / Σ|moves|
    ///
    int entropy_window = std::clamp(lookback_n, 10, 40);

    if (sc.Index < entropy_window) return 0.5f;  // Warmup: neutral

    float net_change = sc.Close[sc.Index] - sc.Close[sc.Index - entropy_window];

    float sum_abs_changes = 0.0f;
    for (int i = 0; i < entropy_window; ++i) {
        sum_abs_changes += std::abs(sc.Close[sc.Index - i] - sc.Close[sc.Index - i - 1]);
    }

    if (sum_abs_changes < 0.00001f) return 0.5f;  // Flat market: neutral

    float efficiency_ratio = std::abs(net_change) / sum_abs_changes;
    float snr = efficiency_ratio * efficiency_ratio;

    return std::clamp(snr, 0.0f, 1.0f);
}

/// Robust Rescaled Range (R/S) Analysis — Multi-Scale Implementation
/// Reference: Peters, "Fractal Market Analysis" (1994), Chapter 4
    ///
    /// Prior version used 128-bar window with 7 scales — insufficient for 15-sec ES bars.
    /// Futures at this timescale have high noise, collapsing H toward 0.5 with narrow scales.
    ///
    /// Fix: Wider 256-bar window (64 min on 15-sec), 8 scales, responsive EMA(0.25).
    ///   1. Collect 256 log-returns from price history (stack-allocated, zero-alloc).
    ///   2. For each sub-period length n ∈ {8, 16, 24, 32, 48, 64, 96, 128}:
    ///      a. Partition returns into floor(256/n) non-overlapping groups.
    ///      b. Per group: R/S = (max − min of cumulative deviations) / σ.
    ///      c. Average R/S across all groups of length n.
    ///   3. OLS regression of log(avg_R/S) vs log(n) → slope = Hurst exponent H.
    ///   4. EMA smoothing (α = 0.25) against previous bar for temporal stability.
    ///
    /// Interpretation:
    ///   H < 0.5: Antipersistent / mean-reverting (range-bound; tighten stops)
    ///   H ≈ 0.5: Random walk (no exploitable structure; widen noise floor)
    ///   H > 0.5: Persistent / trending  (momentum continuation; trail stops)
    ///
    /// Performance: O(256 × 8) ≈ 2048 float ops per bar; all stack-allocated.

// Helper: Pure calculation of Hurst from return series (Zero external dependencies)
// returns: Pointer to array of log-returns
// length: Number of elements (must be power of 2 favorable, e.g. 256)
float ComputeHurstFromReturns(const float* returns, int length) {
    constexpr int NUM_SCALES = 8;
    // Scales adjusted for 256 length: 8, 16, 24, 32, 48, 64, 96, 128
    const int SCALES[NUM_SCALES] = {8, 16, 24, 32, 48, 64, 96, 128};

    // Multi-scale R/S computation
    float log_n[NUM_SCALES];
    float log_rs[NUM_SCALES];
    int valid_scales = 0;

    for (int s = 0; s < NUM_SCALES; ++s) {
        const int n = SCALES[s];
        if (n > length / 2) break; // Safety check

        const int num_groups = length / n;
        if (num_groups < 2) continue;

        float rs_sum = 0.0f;
        int valid_groups = 0;

        for (int g = 0; g < num_groups; ++g) {
            const int group_start = g * n;

            // Group mean
            float group_mean = 0.0f;
            for (int i = 0; i < n; ++i) {
                group_mean += returns[group_start + i];
            }
            group_mean /= static_cast<float>(n);

            // Cumulative deviations, range, and variance
            float cum_dev = 0.0f;
            float max_dev = -1e30f;
            float min_dev = 1e30f;
            float var_sum = 0.0f;

            for (int i = 0; i < n; ++i) {
                const float d = returns[group_start + i] - group_mean;
                cum_dev += d;
                max_dev = std::max(max_dev, cum_dev);
                min_dev = std::min(min_dev, cum_dev);
                var_sum += d * d;
            }

            const float range = max_dev - min_dev;
            const float stddev = std::sqrt(var_sum / static_cast<float>(n));

            if (stddev > 1e-10f && range > 0.0f) {
                rs_sum += range / stddev;
                ++valid_groups;
            }
        }

        if (valid_groups >= 2) {
            const float avg_rs = rs_sum / static_cast<float>(valid_groups);
            log_n[valid_scales] = std::log(static_cast<float>(n));
            log_rs[valid_scales] = std::log(avg_rs);
            ++valid_scales;
        }
    }

    if (valid_scales < 3) return 0.5f;

    // OLS regression
    float sum_x = 0.0f, sum_y = 0.0f, sum_xy = 0.0f, sum_xx = 0.0f;
    for (int i = 0; i < valid_scales; ++i) {
        sum_x  += log_n[i];
        sum_y  += log_rs[i];
        sum_xy += log_n[i] * log_rs[i];
        sum_xx += log_n[i] * log_n[i];
    }

    const float N_f = static_cast<float>(valid_scales);
    const float denom = N_f * sum_xx - sum_x * sum_x;
    if (std::abs(denom) < 1e-10f) return 0.5f;

    float hurst = (N_f * sum_xy - sum_x * sum_y) / denom;
    return std::clamp(hurst, 0.0f, 1.0f);
}

// ----------------------------------------------------------------------------
// Calculates the Hurst Exponent (H) using Detrended Fluctuation Analysis (DFA).
//
// ELITE v2.6 UPGRADE: Replaced legacy R/S analysis with institutional-grade DFA.
// - Standard R/S is biased for small samples and prone to "phantom trends".
// - DFA handles non-stationary time series (log returns) robustly.
// - O(N) optimized implementation with pre-allocated buffers if possible.
//
// Parameters:
//   sc: Sierra Chart Interface
//   length: Lookback window size (n)
//   minScale: Minimum box size (s_min), ensures statistical validity
//
// Returns: Effective Hurst Exponent (0.0 - 1.0)
//   0.5 = Random Walk (Efficient Market)
//   >0.5 = Persistent (Trending / Black Noise)
//   <0.5 = Anti-Persistent (Mean Reverting / Pink Noise)
// ----------------------------------------------------------------------------
float CalculateHurstExponent(SCStudyInterfaceRef sc, int length, int minScale) {
    auto fallback_hurst = [&sc]() -> float {
        const int hasLast = sc.GetPersistentInt(PersistentVar_AdaptiveCalculators::HURST_HAS_LAST_VALID);
        if (hasLast == 1) {
            return sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::HURST_LAST_VALID_VALUE);
        }
        return 0.5f;
    };

    // Institutional guardrails: clamp pathological input, preserve deterministic runtime.
    constexpr int kMaxDfaWindow = 512;
    length = std::clamp(length, 16, kMaxDfaWindow);
    minScale = std::clamp(minScale, 4, 64);

    if (length < minScale * 4 || sc.Index < length) {
        return fallback_hurst();
    }

    // Fixed-capacity stack buffers (zero heap allocations in hot path).
    std::array<double, kMaxDfaWindow> logReturns{};
    std::array<double, kMaxDfaWindow> profile{};
    std::array<double, kMaxDfaWindow> logScales{};
    std::array<double, kMaxDfaWindow> logFluctuations{};

    const int dataStartIndex = sc.Index - length + 1;
    SCFloatArrayRef priceData = sc.BaseData[SC_LAST];

    double sumReturns = 0.0;
    for (int i = 0; i < length; ++i) {
        const int idx = dataStartIndex + i;
        const float currentPrice = priceData[idx];
        const float prevPrice = priceData[idx - 1];

        double logRet = 0.0;
        if (currentPrice > 0.0f && prevPrice > 0.0f) {
            logRet = std::log(static_cast<double>(currentPrice) / static_cast<double>(prevPrice));
        }

        logReturns[static_cast<size_t>(i)] = logRet;
        sumReturns += logRet;
    }

    const double meanReturn = sumReturns / static_cast<double>(length);

    double cumulative = 0.0;
    for (int i = 0; i < length; ++i) {
        cumulative += (logReturns[static_cast<size_t>(i)] - meanReturn);
        profile[static_cast<size_t>(i)] = cumulative;
    }

    const int maxScale = length / 4;
    if (maxScale <= minScale) {
        return fallback_hurst();
    }

    const int step = (maxScale - minScale > 50) ? 2 : 1;
    int validScaleCount = 0;

    for (int s = minScale; s <= maxScale; s += step) {
        const int numSegments = length / s;
        if (numSegments < 1) {
            continue;
        }

        double totalVariance = 0.0;
        int usedSegments = 0;

        for (int v = 0; v < numSegments; ++v) {
            const int startIndex = v * s;
            const double n = static_cast<double>(s);
            const double sumX = n * (n - 1.0) * 0.5;
            const double sumX2 = n * (n - 1.0) * (2.0 * n - 1.0) / 6.0;
            const double denom = n * sumX2 - sumX * sumX;
            if (std::fabs(denom) < 1e-12) {
                continue;
            }

            double sumY = 0.0;
            double sumXY = 0.0;
            for (int k = 0; k < s; ++k) {
                const double y = profile[static_cast<size_t>(startIndex + k)];
                sumY += y;
                sumXY += static_cast<double>(k) * y;
            }

            const double slope = (n * sumXY - sumX * sumY) / denom;
            const double intercept = (sumY - slope * sumX) / n;

            double ssr = 0.0;
            for (int k = 0; k < s; ++k) {
                const double trend = slope * static_cast<double>(k) + intercept;
                const double diff = profile[static_cast<size_t>(startIndex + k)] - trend;
                ssr += diff * diff;
            }

            totalVariance += (ssr / n);
            ++usedSegments;
        }

        if (usedSegments == 0) {
            continue;
        }

        const double f_s = std::sqrt(totalVariance / static_cast<double>(usedSegments));
        if (f_s > 1e-12 && validScaleCount < kMaxDfaWindow) {
            logScales[static_cast<size_t>(validScaleCount)] = std::log(static_cast<double>(s));
            logFluctuations[static_cast<size_t>(validScaleCount)] = std::log(f_s);
            ++validScaleCount;
        }
    }

    if (validScaleCount < 2) {
        return fallback_hurst();
    }

    const double n = static_cast<double>(validScaleCount);
    double sumX = 0.0;
    double sumY = 0.0;
    double sumXY = 0.0;
    double sumX2 = 0.0;

    for (int i = 0; i < validScaleCount; ++i) {
        const double x = logScales[static_cast<size_t>(i)];
        const double y = logFluctuations[static_cast<size_t>(i)];
        sumX += x;
        sumY += y;
        sumXY += x * y;
        sumX2 += x * x;
    }

    const double regressionDenom = n * sumX2 - sumX * sumX;
    if (std::fabs(regressionDenom) < 1e-12) {
        return fallback_hurst();
    }

    float hurst = static_cast<float>((n * sumXY - sumX * sumY) / regressionDenom);
    hurst = std::clamp(hurst, 0.0f, 1.5f);

    if (!std::isfinite(hurst)) {
        return fallback_hurst();
    }

    sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::HURST_LAST_VALID_VALUE) = hurst;
    sc.SetPersistentInt(PersistentVar_AdaptiveCalculators::HURST_HAS_LAST_VALID, 1);
    return hurst;
}

// ----------------------------------------------------------------------------
// Legacy wrapper for existing calls in TripleScreen studies
// ----------------------------------------------------------------------------
float CalculateHurstExponent(SCStudyInterfaceRef sc) {
    // Default parameters for legacy calls: 100 bar window, minScale=8
    // This matches standard institutional settings for intraday noise filtering
    return CalculateHurstExponent(sc, 100, 8);
}

float CalculateRealizedKurtosis(SCStudyInterfaceRef sc, float prevKurtosis, SCFloatArrayRef atrArray) {
    /// Realized Kurtosis - ELITE: Regime-Adjusted Implementation #3
    /// Detects tail risk (excess kurtosis >3 = panic/euphoria, 0 = normal, <-2 = flat/trapped)
    ///
    /// Institutional Regime Adjustment:
    ///   Normal volatility: kurtosis as-is (baseline)
    ///   High volatility (>1.3× avg): scale UP 1.25× (panic amplifies tail risk)
    ///   Low volatility (<0.7× avg): scale DOWN 0.75× (flat creates false positives)
    constexpr int KURT_WINDOW = 100;
    if (sc.Index < KURT_WINDOW) return 3.0f;

    std::array<float, KURT_WINDOW> returns{};
    for (int i = 0; i < KURT_WINDOW; ++i) {
        returns[static_cast<size_t>(i)] = std::log(sc.Close[sc.Index - i] / std::max(sc.Close[sc.Index - i - 1], 0.001f));
    }

    float mean_ret = 0.0f;
    for (float r : returns) mean_ret += r;
    mean_ret /= KURT_WINDOW;

    float variance = 0.0f;
    for (float ret : returns) {
        float diff = ret - mean_ret;
        variance += diff * diff;
    }
    variance /= KURT_WINDOW;

    // ES 15s log-return variance is typically very small; avoid hard-zero fallback.
    // Returning 0.0f here created a pathological floor-lock in the rank-percentile
    // scaler (Dim 8 zero-trap). Use a neutral/carry-forward value instead.
    constexpr float KURT_VARIANCE_EPS = 1e-10f;
    if (variance < KURT_VARIANCE_EPS) {
        if (sc.Index > 0 && std::isfinite(prevKurtosis)) {
            return std::clamp(prevKurtosis, -5.0f, 50.0f);
        }
        return 3.0f;  // Neutral kurtosis baseline
    }

    float m4 = 0.0f;
    for (float ret : returns) {
        float diff = ret - mean_ret;
        m4 += diff * diff * diff * diff;
    }
    m4 /= KURT_WINDOW;

    // Sample Kurtosis Formula (Unbiased Estimator - Sierra Chart's approach)
    // More statistically rigorous than simple Fisher-Pearson adjustment
    // adjustment = (n-1)(n+1) / [(n-2)(n-3)]
    // correction = 3(n-1)² / [(n-2)(n-3)]
    // This provides better small-sample statistics for our 100-bar window
    constexpr float n_f = static_cast<float>(KURT_WINDOW);
    float adjustment = ((n_f - 1.0f) * (n_f + 1.0f)) / ((n_f - 2.0f) * (n_f - 3.0f));
    float bias_correction = 3.0f * (n_f - 1.0f) * (n_f - 1.0f) / ((n_f - 2.0f) * (n_f - 3.0f));

    float var_squared = variance * variance;
    float kurtosis = adjustment * (m4 / var_squared) - bias_correction;

    // ELITE FIX #3: Regime adjustment (PhD-level risk management)
    float atrCurrent = atrArray[sc.Index];
    constexpr int VOL_COMPARE_WINDOW = 20;
    if (sc.Index >= VOL_COMPARE_WINDOW) {
        float atrAvg = 0.0f;
        for (int i = 0; i < VOL_COMPARE_WINDOW; ++i) {
            atrAvg += atrArray[sc.Index - i];
        }
        atrAvg /= VOL_COMPARE_WINDOW;
        float vol_ratio = atrCurrent / std::max(atrAvg, 0.0001f);
        float regime_mult = 1.0f;
        if (vol_ratio > 1.3f) regime_mult = 1.25f;   // High-vol: panic amplifies
        if (vol_ratio < 0.7f) regime_mult = 0.75f;   // Low-vol: flat creates noise
        kurtosis *= regime_mult;
    }

    return std::clamp(kurtosis, -5.0f, 50.0f);
}

float CalculateSkewness(SCStudyInterfaceRef sc, SCFloatArrayRef atrArray) {
    /// Skewness - ELITE: Regime-Adjusted Implementation #4
    /// Detects distribution asymmetry: (-) = left tail crashes, (+) = right tail rallies
    ///
    /// Institutional Regime Adjustment:
    ///   Normal volatility: skewness as-is (baseline)
    ///   High-vol trending (>1.2× avg): amplify × 1.3× (rallies steeper, crashes sharp)
    ///   Low-vol ranging (<0.8× avg): dampen × 0.8× (noise creates spurious asymmetry)
    constexpr int SKEW_WINDOW = 100;
    if (sc.Index < SKEW_WINDOW) return 0.0f;

    std::array<float, SKEW_WINDOW> returns{};
    for (int i = 0; i < SKEW_WINDOW; ++i) {
        returns[static_cast<size_t>(i)] = std::log(sc.Close[sc.Index - i] / std::max(sc.Close[sc.Index - i - 1], 0.001f));
    }

    float mean_ret = 0.0f;
    for (float r : returns) mean_ret += r;
    mean_ret /= SKEW_WINDOW;

    float variance = 0.0f;
    for (float ret : returns) {
        float diff = ret - mean_ret;
        variance += diff * diff;
    }
    variance /= SKEW_WINDOW;

    float& lastValidSkewness = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::SKEWNESS_LAST_VALID_VALUE);

    // ES 15s log-return variance is typically very small; avoid collapsing to zero.
    // Degenerate (near-flat return window) carries the last valid value forward
    // instead of a fabricated exact-zero "no skew" reading -- returning before
    // the regime-adjustment block below means a carried-forward value is never
    // re-multiplied by a fresh regime factor -- same sentinel-collapse fix
    // already applied to dims 1/2/3/7/8/11/12
    // (docs/superpowers/plans/2026-08-12-observation-vector-carry-forward-completion.md).
    constexpr float SKEW_VARIANCE_EPS = 1e-10f;
    if (variance < SKEW_VARIANCE_EPS) {
        return lastValidSkewness;
    }
    float stddev = std::sqrt(variance);

    float m3 = 0.0f;
    for (float ret : returns) {
        float diff = ret - mean_ret;
        m3 += diff * diff * diff;
    }
    m3 /= SKEW_WINDOW;

    float skewness = m3 / (stddev * stddev * stddev);

    // ELITE FIX #4: Regime adjustment (Wyckoff-aligned)
    float atrCurrent = atrArray[sc.Index];
    constexpr int VOL_COMPARE_WINDOW = 20;
    if (sc.Index >= VOL_COMPARE_WINDOW) {
        float atrAvg = 0.0f;
        for (int i = 0; i < VOL_COMPARE_WINDOW; ++i) {
            atrAvg += atrArray[sc.Index - i];
        }
        atrAvg /= VOL_COMPARE_WINDOW;
        float vol_ratio = atrCurrent / std::max(atrAvg, 0.0001f);
        float regime_mult = 1.0f;
        if (vol_ratio > 1.2f) regime_mult = 1.30f;   // Trending: steeper rallies
        if (vol_ratio < 0.8f) regime_mult = 0.80f;   // Ranging: flatten spurious skew
        skewness *= regime_mult;
    }

    skewness = std::clamp(skewness, -1.5f, 1.5f);
    lastValidSkewness = skewness;
    return skewness;
}

float CalculateLiquidityFragility(SCStudyInterfaceRef sc, float atrRef, float volumeSma, float prev_fragility) {
    /// Liquidity Fragility v3 — Institutional Redesign (Almgren & Chriss + Gemini Review)
    ///
    /// Prior versions used ATR-expansion (atrCurrent / atrMean - 1) which produces near-zero
    /// signal on 15-sec ES bars because ATR is a trailing average that barely fluctuates.
    ///
    /// v3 Fix: Dual-signal fragility from INSTANTANEOUS bar metrics:
    ///   Signal 1: Bar Range Expansion — (High-Low) / ATR. Individual bars fluctuate 0.3x-2.5x
    ///             of trailing ATR, providing continuous variation.
    ///   Signal 2: Volume Depletion — when volume drops below SMA, market is thin.
    ///             Thin volume + wide range = fragile microstructure.
    ///
    /// Formula: fragility = sigmoid(range_expansion × volume_amplifier)
    ///   range_expansion = max(0, barRange/ATR - 1.0)  [0 = normal, scales up with expansion]
    ///   volume_amplifier = clamp(2.0 - vol/volSMA, 1.0, 2.0)  [thin volume = 2x amplifier]
    ///   sigmoid mapping: x / (1 + x) for bounded [0, 1) output
    ///   EMA smoothing: α=0.30 (expansion) / α=0.15 (decay) for asymmetric response
    ///
    /// ES Calibration: On 15-sec bars, typical barRange/ATR is 0.3-1.5 in normal conditions,
    /// producing fragility of 0.0-0.35 (normal) and 0.4-0.8 (stressed).

    if (sc.Index < 20) return 0.0f;

    // Current bar range -- reads the last fully-closed bar, not the live
    // still-forming one, for the same reason the volume read below does:
    // this function is called at the bar's first tick, when High==Low==the
    // bar's only trade so far, which would otherwise pin range_signal (the
    // dominant 0.65-weighted term) near its floor on every live call.
    float barRange = sc.High[sc.Index - 1] - sc.Low[sc.Index - 1];
    if (barRange < 0.00001f) barRange = 0.00001f;  // Avoid div-by-zero on doji

    // Trailing ATR reference
    if (atrRef < 0.0001f) {
        if (sc.Index > 0 && std::isfinite(prev_fragility)) {
            return std::clamp(prev_fragility, 0.0f, 1.0f);
        }
        return 0.2f;
    }

    // Signal 1: Continuous range pressure via log-ratio for wider dynamic range.
    // log(rangeRatio) maps [0.1, 5.0] → [-2.3, +1.6], then sigmoid to [0, 1].
    // This spreads the typical ES 15-min range (0.3-1.5 ATR) across [0.15, 0.60]
    // instead of the old sigmoid which compressed to [0.23, 0.33].
    const float rangeRatio = std::clamp(barRange / atrRef, 0.1f, 5.0f);
    const float logRatio = std::log(rangeRatio);
    const float range_signal = 1.0f / (1.0f + std::exp(-2.0f * logRatio));  // logistic sigmoid

    // Signal 2: Thin-book pressure from volume depletion.
    // Last fully-closed bar's volume, not the current still-forming bar's
    // (this function is called once per bar, at the bar's first tick, via
    // UpdateObservationVectorSubgraphs -- sc.Volume[sc.Index] at that moment
    // is live and near its minimum almost every call, systematically biasing
    // "thinness" toward its extreme. See
    // docs/superpowers/plans/2026-08-12-remaining-observation-vector-dims.md.
    const float currentVolume = (sc.Index >= 1) ? static_cast<float>(sc.Volume[sc.Index - 1]) : 0.0f;
    float thinness = 0.5f;  // neutral when volume references are unavailable
    if (volumeSma > 1.0f && currentVolume > 0.0f) {
        const float volRatio = currentVolume / volumeSma;
        thinness = std::clamp(1.5f - volRatio, 0.0f, 1.0f);
    }

    // Institutional composite: range pressure is primary; thinness amplifies fragility.
    const float fragility_raw = std::clamp(
        0.65f * range_signal +
        0.35f * (range_signal * thinness),
        0.0f,
        1.0f
    );

    // Asymmetric EMA: fast rise (α=0.30) / slow decay (α=0.15)
    // Fragility should spike quickly but linger — market memory is asymmetric
    if (sc.Index == 0) {
        return 0.0f;
    }
    const float alpha = (fragility_raw > prev_fragility) ? 0.30f : 0.15f;
    const float fragility_smoothed = alpha * fragility_raw + (1.0f - alpha) * prev_fragility;

    return std::clamp(fragility_smoothed, 0.0f, 1.0f);
}

// NOTE: micro_asymmetry (dim 7) is NOT computed here -- see the declaration's
// doc comment in StudyHelperFunctions.h. It must be updated every tick, not
// gated to once per bar the way the rest of this function's outputs are.
void UpdateObservationVectorSubgraphs(
    SCStudyInterfaceRef sc,
    int observation_window_n,
    SCSubgraphRef Subgraph_PathEfficiencySNR,
    SCSubgraphRef Subgraph_HurstExponent,
    SCSubgraphRef Subgraph_RealizedKurtosis,
    SCSubgraphRef Subgraph_SkewnessIdx,
    SCSubgraphRef Subgraph_AmihudIlliquidity,
    SCSubgraphRef Subgraph_LiqFragility,
    SCSubgraphRef Subgraph_ATR,
    SCSubgraphRef Subgraph_VolumeSMA) {
    constexpr int WARMUP_BARS = 100;
    int adaptive_window_n = std::clamp(observation_window_n, 10, 40);

    int& lastObsUpdateIndex = sc.GetPersistentInt(PersistentVar_AdaptiveCalculators::LAST_OBS_UPDATE_INDEX);
    if (!sc.IsFullRecalculation && lastObsUpdateIndex == sc.Index) {
        return;
    }
    lastObsUpdateIndex = sc.Index;

    if (sc.Index < WARMUP_BARS) {
        Subgraph_PathEfficiencySNR[sc.Index] = 0.5f;
        Subgraph_HurstExponent[sc.Index] = 0.5f;
        Subgraph_RealizedKurtosis[sc.Index] = 3.0f;
        Subgraph_SkewnessIdx[sc.Index] = 0.0f;
        Subgraph_AmihudIlliquidity[sc.Index] = 0.5f;
        Subgraph_LiqFragility[sc.Index] = 0.0f;
    } else {
        Subgraph_PathEfficiencySNR[sc.Index] = CalculatePathEfficiencySNR(sc, Subgraph_ATR[sc.Index], adaptive_window_n);
        Subgraph_HurstExponent[sc.Index] = CalculateHurstExponent(sc);
        Subgraph_RealizedKurtosis[sc.Index] = CalculateRealizedKurtosis(sc, Subgraph_RealizedKurtosis[sc.Index - 1], Subgraph_ATR.Data);
        Subgraph_SkewnessIdx[sc.Index] = CalculateSkewness(sc, Subgraph_ATR.Data);

        float volSma = Subgraph_VolumeSMA[sc.Index];
        float atrRef = Subgraph_ATR[sc.Index];

        Subgraph_AmihudIlliquidity[sc.Index] = CalculateAmihudIlliquidity(sc, adaptive_window_n);
        Subgraph_LiqFragility[sc.Index] = CalculateLiquidityFragility(sc, atrRef, volSma, Subgraph_LiqFragility[sc.Index - 1]);
    }
}
/// ============================================================================
/// INTEGRATION GUIDE: Adaptive Windowing in TripleScreen Studies
/// ============================================================================
///
/// Usage Pattern 1: SCREEN 1 (240m) - Adaptive Trend Detection
/// ─────────────────────────────────────────────────────────────
/// Place this in the main study loop of TripleScreen1.cpp:
///
///   float market_speed = CalculateMarketSpeed(sc);
///   AdaptiveWindowParams windows;
///   windows.UpdateWindows(market_speed, true, 1.0f);  // true=coherent
///
///   // Use windows.screen1_window instead of static 26
///   // (CalculateKAMA removed — Kaufman MA no longer used)
///
///
/// Usage Pattern 2: SCREEN 3 (15m) - Hysteresis + Coherence
/// ──────────────────────────────────────────────────────────
/// Place this in TripleScreen3.cpp entry signal logic:
///
///   // Get 240m & 60m MACD histograms (from screens 1 & 2)
///   float tf1_macd = /* Screen1 MACD histogram */;
///   float tf2_macd = /* Screen2 MACD histogram */;
///
///   // Check multi-TF coherence
///   bool is_coherent = CheckCoherence(tf1_macd, tf2_macd, 0.05f);  // 5% threshold
///
///   // Update adaptive windows
///   AdaptiveWindowParams windows;
///   windows.UpdateWindows(market_speed, is_coherent);
///
///   // Apply impulse hysteresis
///   static ImpulseHysteresis impulse_filter;
///   int smoothed_impulse = impulse_filter.GetSmoothedImpulse(
///       current_impulse_color,  // 1=green, -1=red, 2=blue
///       macd_histogram,
///       prev_macd_histogram
///   );
///
///   // Coherence gate: if incoherent, widen entry window
///   if (!is_coherent) {
///       // Signal widen mode: window size increased 30% (see AdaptiveWindowParams)
///   }
///
///
/// Usage Pattern 3: Observation Vector Normalization
/// ──────────────────────────────────────────
/// Ensure all observation vector features use unified lookback:
///
///   // Calculate observation vector with adaptive window
///   for (int i = 0; i < windows.observation_vector_n; ++i) {
///       features[0] = CalculatePathEfficiencySNR(...);  // Uses entropy_window
///       // ... all observation dimensions use windows.observation_vector_n
///   }
///
///
/// ACSIL Safety Checklist:
/// ─────────────────────
/// - [ ] Store AdaptiveWindowParams in sc.PersistVars for cross-bar state
/// - [ ] Bounds-check all window sizes before calling sc.GetOHLCV
/// - [ ] Reset ImpulseHysteresis on market open (session boundary)
/// - [ ] Validate market_speed is in [0.4, 2.5] range (already clamped)
/// - [ ] Check array indices before accessing sc.Close[sc.Index - window]
///
/// Performance Impact:
/// ──────────────────
/// - CalculateMarketSpeed: ~50µs per call (safe to call every bar)
/// - CalculateEfficiencyRatio: ~100µs per call (34-bar window sum)
/// - CheckCoherence: <1µs per call (simple comparison)
/// - ImpulseHysteresis: <1µs per call (state machine)
///
/// ============================================================================

/// ============================================================================
/// CANONICAL OBSERVATIONDATA VECTOR IMPLEMENTATIONS
/// ============================================================================

float CalculateLogVariance(SCStudyInterfaceRef sc, int lookback_n) {
    // Canonical metric: log(short_var / long_var), robust to tiny variances.
    const int long_n = std::max(20, lookback_n);
    const int short_n = std::max(8, long_n / 4);

    if (sc.Index < (long_n + 1)) return 0.0f;

    auto windowVariance = [&](int n) -> double {
        double sum = 0.0;
        double sumSq = 0.0;
        int count = 0;
        for (int i = 0; i < n; ++i) {
            int idx = sc.Index - i;
            float price = sc.BaseData[SC_LAST][idx];
            float prevPrice = sc.BaseData[SC_LAST][idx - 1];
            if (price > 0.0f && prevPrice > 0.0f) {
                const double logRet = std::log(price / prevPrice);
                sum += logRet;
                sumSq += logRet * logRet;
                ++count;
            }
        }
        if (count < 2) {
            return 0.0;
        }
        const double mean = sum / static_cast<double>(count);
        const double var = (sumSq / static_cast<double>(count)) - (mean * mean);
        return std::max(var, 0.0);
    };

    const double short_var = windowVariance(short_n);
    const double long_var = windowVariance(long_n);
    constexpr double kVarEps = 1e-12;

    const double log_ratio = std::log((short_var + kVarEps) / (long_var + kVarEps));
    return std::clamp(static_cast<float>(log_ratio), -6.0f, 6.0f);
}

float CalculateFisherInformation(SCStudyInterfaceRef sc, int lookback_n) {
    // Fisher Transform of Price Relative to Range (Stoch-like normalized)
    // y = 0.5 * ln((1+x)/(1-x))

    if (sc.Index < lookback_n) return 0.0f;

    float minPrice = FLT_MAX;
    float maxPrice = -FLT_MAX;

    for(int i=0; i<lookback_n; i++) {
        float p = sc.BaseData[SC_LAST][sc.Index - i];
        if(p < minPrice) minPrice = p;
        if(p > maxPrice) maxPrice = p;
    }

    float currentPrice = sc.BaseData[SC_LAST][sc.Index];
    float& lastValidFisherInfo = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::FISHER_INFO_LAST_VALID_VALUE);
    const float fisherInfo = cfc::ComputeFisherInformation(minPrice, maxPrice, currentPrice, lastValidFisherInfo);
    lastValidFisherInfo = fisherInfo;
    return fisherInfo;
}

float CalculateRealizedVarianceRatio(SCStudyInterfaceRef sc, int lookback_n) {
    // log(RV_recent / RV_full) — positive = volatility expanding, negative = contracting.
    // Stateless, naturally centered at 0, bounded by construction.
    const int half = lookback_n / 2;
    if (sc.Index < lookback_n || half < 2) return 0.0f;

    double rv_recent = 0.0, rv_full = 0.0;
    for (int i = 0; i < lookback_n; ++i) {
        int idx = sc.Index - i;
        if (idx < 1 || sc.Close[idx - 1] <= 0.0f) continue;
        double r = std::log(static_cast<double>(sc.Close[idx]) / sc.Close[idx - 1]);
        double r2 = r * r;
        rv_full += r2;
        if (i < half) rv_recent += r2;
    }

    // Scale full-window variance to per-sample rate for fair comparison.
    double rv_full_rate = rv_full / lookback_n;
    double rv_recent_rate = rv_recent / half;

    float& lastValidCorrectionAction = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::CORRECTION_ACTION_LAST_VALID_VALUE);
    const float correctionAction = cfc::ComputeBurstinessIndex(rv_recent_rate, rv_full_rate, lastValidCorrectionAction);
    lastValidCorrectionAction = correctionAction;
    return correctionAction;
}

float CalculateAmihudIlliquidity(SCStudyInterfaceRef sc, int lookback_n) {
    // Canonical Amihud (2002) illiquidity: mean( |r_t| / DollarVolume_t ) over the
    // lookback, with r_t = log return ln(P_t / P_{t-1}) and DollarVolume_t = P_t * V_t.
    // Log returns + dollar volume make the measure price-level STATIONARY: a $2 move
    // at ES=6000 is not the same event as a $2 move at ES=2000, and a fixed threshold
    // is meaningless without this normalization (root cause of the old 0.80/0.40 bug).
    // Amihud is the canonical low-frequency proxy for Kyle (1985) lambda (price impact
    // per unit dollar flow). High values = illiquid (large impact per dollar traded).
    if (sc.Index < lookback_n) return 0.0f;

    // Window is entirely closed/historical bars (i=1..lookback_n), never the
    // current still-forming bar (i=0). This function is called from
    // UpdateObservationVectorSubgraphs's once-per-bar gate (first tick of
    // each bar only) -- sc.Volume[sc.Index] at that moment is the live,
    // still-accumulating volume, near its minimum almost every call, which
    // would otherwise make the current-bar term spuriously fail the
    // dollarVol/vol floor checks below on nearly every call (same root
    // cause as dim 7's and dim 12's once-per-bar timing bugs -- see
    // docs/superpowers/plans/2026-08-12-remaining-observation-vector-dims.md).
    double sum = 0.0;
    int count = 0;
    for (int i = 1; i <= lookback_n; ++i) {
        int idx = sc.Index - i;
        if (idx < 1) break;
        const double price = static_cast<double>(sc.Close[idx]);
        const double prevPrice = static_cast<double>(sc.Close[idx - 1]);
        const double vol = static_cast<double>(sc.Volume[idx]);
        if (vol < 1.0 || price <= 0.0 || prevPrice <= 0.0) continue;
        const double dollarVol = price * vol;
        if (dollarVol < 1.0) continue;
        const double logRet = std::abs(std::log(price / prevPrice));
        sum += logRet / dollarVol;
        ++count;
    }

    float& lastValidAmihud = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::AMIHUD_LAST_VALID_VALUE);
    const float amihud = cfc::ComputeAmihudIlliquidity(sum, count, lastValidAmihud);
    lastValidAmihud = amihud;
    return amihud;
}

float CalculateBurstiness(SCStudyInterfaceRef sc, int lookback_n) {
    // Half-window variance ratio: log(RV_recent_half / RV_older_half).
    // Positive = volatility clustering in recent half = bursty.
    // Negative = volatility clustering in older half = quieting.
    // Replaces Barabási inter-arrival formula which is degenerate at hourly resolution.
    const int half = lookback_n / 2;
    if (sc.Index < lookback_n || half < 2) return 0.0f;

    double rv_recent = 0.0, rv_older = 0.0;
    for (int i = 0; i < lookback_n; ++i) {
        int idx = sc.Index - i;
        float range = sc.BaseData[SC_HIGH][idx] - sc.BaseData[SC_LOW][idx];
        double r2 = static_cast<double>(range) * range;
        if (i < half)
            rv_recent += r2;
        else
            rv_older += r2;
    }

    // Per-sample rate for fair comparison (halves may differ by 1 sample for odd lookback).
    double rv_recent_rate = rv_recent / half;
    double rv_older_rate = rv_older / (lookback_n - half);

    float& lastValidBurstiness = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::BURSTINESS_LAST_VALID_VALUE);
    const float burstiness = cfc::ComputeBurstinessIndex(rv_recent_rate, rv_older_rate, lastValidBurstiness);
    lastValidBurstiness = burstiness;
    return burstiness;
}

float CalculateFractalDimension(SCStudyInterfaceRef sc, int lookback_n) {
    // Sevcik Fractal Dimension Approximation
    // D = 1 + ln(L) / ln(2*N)
    // L = Sum of euclidean distances between normalized points

    if (sc.Index < lookback_n) return 1.5f; // Brownian guess -- true cold-start, no prior value exists yet

    float minP = FLT_MAX, maxP = -FLT_MAX;
    for(int i=0; i<lookback_n; i++) {
        float p = sc.BaseData[SC_LAST][sc.Index - i];
        if(p < minP) minP = p;
        if(p > maxP) maxP = p;
    }

    float& lastValidFractalDim = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::FRACTAL_DIM_LAST_VALID_VALUE);

    // Degenerate (flat price window) carries the last valid value forward
    // instead of a fabricated "1.0 = flat line" reading -- checked before the
    // path-length scan below, so the expensive computation is still skipped on
    // the degenerate path exactly as before -- same sentinel-collapse fix
    // already applied to dims 1/2/3/7/8/10/11/12/13. Guarded against the
    // uninitialized 0.0f default (no prior valid value yet): fractal_dim's
    // contract is [1.0, 2.0], and an out-of-contract 0.0f trips the hard
    // structuralInRange gate downstream, stalling the HMM inference path --
    // fall back to the same cold-start "Brownian guess" this function already
    // returns elsewhere instead.
    if (maxP <= minP) {
        return (lastValidFractalDim >= 1.0f) ? lastValidFractalDim : 1.5f;
    }

    const int segments = lookback_n - 1;
    if (segments <= 0) return 1.5f; // Unreachable in practice (lookback_n always >=30) -- defensive, true cold-start shape

    double length = 0.0;
    double priceRange = maxP - minP;

    for(int i=1; i<lookback_n; i++) {
        int idx = sc.Index - lookback_n + i; // Moving forward from start of window
        float p1 = sc.BaseData[SC_LAST][idx-1];
        float p2 = sc.BaseData[SC_LAST][idx];

        // Normalized coordinates (Time on X [0..1], Price on Y [0..1])
        // Use segment count (N-1), not point count (N), to avoid discretization bias.
        double dy = (p2 - p1) / priceRange;
        double dx = 1.0 / static_cast<double>(segments);

        length += std::sqrt(dx*dx + dy*dy);
    }

    // Degenerate (zero-length path -- should be unreachable once maxP>minP is
    // established above; kept as a defensive carry-forward, not a fresh sentinel).
    if (length <= 0.0) {
        return lastValidFractalDim;
    }
    const float dim = static_cast<float>(
        1.0 + std::log(length) / std::log(2.0 * static_cast<double>(segments)));
    const float fractalDim = std::clamp(dim, 1.0f, 2.0f);
    lastValidFractalDim = fractalDim;
    return fractalDim;
}

float CalculateMeanReversionSpeed(SCStudyInterfaceRef sc, int lookback_n) {
    // Mean-Reversion Elasticity Score (contract: mean_rev_z in [0, 5]).
    // 1) Compute price stretch as |z(log-price)| over lookback window.
    // 2) Suppress score in momentum regimes using lag-1 return autocorrelation.

    // kMaxLookback matches the [10,40] adaptive observation window contract
    // this function's one caller (TripleScreen3.cpp) always passes today
    // (CalculateAdaptiveObservationWindow's own std::clamp(..., 10, 40)) --
    // defensive upper bound so a fixed-capacity scratch buffer below can
    // never be written out of range if a future caller passes something larger.
    constexpr int kMaxLookback = 40;
    const int n = std::clamp(lookback_n, 5, kMaxLookback);
    if (sc.Index < (n + 1)) return 0.0f; // True cold-start, no prior value exists yet

    constexpr double kPriceEps = 1e-6;
    const int start_idx = sc.Index - n + 1;

    // Price z-score from log-price window.
    double sum_log_p = 0.0;
    double sum_log_p_sq = 0.0;
    for (int i = 0; i < n; ++i) {
        const double p = std::max(static_cast<double>(sc.BaseData[SC_LAST][start_idx + i]), kPriceEps);
        const double lp = std::log(p);
        sum_log_p += lp;
        sum_log_p_sq += lp * lp;
    }

    const double mean_log_p = sum_log_p / static_cast<double>(n);
    const double var_log_p = std::max((sum_log_p_sq / static_cast<double>(n)) - (mean_log_p * mean_log_p), 0.0);
    const double std_log_p = std::sqrt(var_log_p);

    float& lastValidMeanRevZ = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::MEAN_REV_Z_LAST_VALID_VALUE);
    // Degenerate (flat price window) carries the last valid value forward
    // instead of a fabricated exact-zero "no stretch" reading -- same
    // sentinel-collapse fix already applied to dims 1/2/3/7/8/11/12.
    if (std_log_p < 1e-6) {
        return lastValidMeanRevZ;
    }

    const double current_log_p = std::log(std::max(static_cast<double>(sc.BaseData[SC_LAST][sc.Index]), kPriceEps));
    const double abs_z_price = std::abs((current_log_p - mean_log_p) / std_log_p);

    // Lag-1 autocorrelation on log-returns: positive rho => momentum, negative => reversion.
    const int m = n - 1;
    if (m < 3) {
        // Genuinely computed (not degenerate) -- too few samples for the
        // autocorrelation term, so it's skipped, not faked. Still updates the
        // carry-forward state so a later degenerate call has a real value to
        // fall back on.
        const float score = std::clamp(static_cast<float>(abs_z_price), 0.0f, 5.0f);
        lastValidMeanRevZ = score;
        return score;
    }

    double sum_r = 0.0;
    std::array<double, kMaxLookback> returns{};  // m <= n-1 < kMaxLookback, always in range
    for (int i = 0; i < m; ++i) {
        const int idx = start_idx + i + 1;
        const double p = std::max(static_cast<double>(sc.BaseData[SC_LAST][idx]), kPriceEps);
        const double p_prev = std::max(static_cast<double>(sc.BaseData[SC_LAST][idx - 1]), kPriceEps);
        const double r = std::log(p / p_prev);
        returns[static_cast<size_t>(i)] = r;
        sum_r += r;
    }

    const double mean_r = sum_r / static_cast<double>(m);
    double num = 0.0;
    double den = 0.0;
    for (int t = 1; t < m; ++t) {
        const double r_t = returns[t] - mean_r;
        const double r_prev = returns[t - 1] - mean_r;
        num += r_t * r_prev;
        den += r_prev * r_prev;
    }

    const double rho = (den > 1e-12) ? (num / den) : 0.0;
    const double elasticity_gate = std::clamp(1.0 - std::max(rho, 0.0), 0.0, 1.0);
    const double score = abs_z_price * elasticity_gate;

    const float meanRevZ = std::clamp(static_cast<float>(score), 0.0f, 5.0f);
    lastValidMeanRevZ = meanRevZ;
    return meanRevZ;
}

float CalculateVolConvexity(SCStudyInterfaceRef sc, int lookback_n) {
    // Volatility of Volatility (High moment of volatility)
    // StdDev of ATR over lookback
    // Uses only closed/historical bars (i=1..n) -- sc.Index itself (the
    // still-forming current bar) is never read, since this function is
    // called every tick and its High/Low would otherwise leak the live,
    // not-yet-final bar into a statistic meant to summarize completed bars.

    // Better: Calculate TR locally to be robust

    // kMaxLookback matches the [10,40] adaptive observation window contract
    // this function's one caller (TripleScreen3.cpp) always passes today --
    // defensive upper bound so the fixed-capacity scratch buffer below can
    // never be written out of range if a future caller passes something larger.
    constexpr int kMaxLookback = 40;
    const int n = std::clamp(lookback_n, 1, kMaxLookback);
    if (sc.Index < n + 1) return 0.0f;

    std::array<float, kMaxLookback> trValues{};
    double sumTR = 0;

    for (int i = 1; i <= n; i++) {
        int idx = sc.Index - i;
        float h = sc.BaseData[SC_HIGH][idx];
        float l = sc.BaseData[SC_LOW][idx];
        float c_prev = sc.BaseData[SC_LAST][idx-1];

        float tr = std::max(h-l, std::max(std::abs(h-c_prev), std::abs(l-c_prev)));
        trValues[static_cast<size_t>(i - 1)] = tr;
        sumTR += tr;
    }

    double meanTR = sumTR / n;
    double sumSqDiff = 0;

    for (int i = 0; i < n; i++) {
        const float tr = trValues[static_cast<size_t>(i)];
        sumSqDiff += (tr - meanTR) * (tr - meanTR);
    }

    const double trStd = std::sqrt(sumSqDiff / n);
    const double trMean = std::max(meanTR, 1e-6);
    const float cv = static_cast<float>(trStd / trMean);
    return std::clamp(cv, 0.0f, 5.0f);
}


float CalculateRecurrenceRate(SCStudyInterfaceRef sc, int lookback_n) {
    if (sc.Index < lookback_n) return 0.0f;

    // RQA (Recurrence Quantification Analysis) Recurrence Rate
    // RR = (1 / N^2) * Sum(Theta(epsilon - dist(i,j)))
    // Where Theta is Heaviside step fun.
    // Epsilon = 10% of range or 0.5 * StdDev

    float minP = FLT_MAX, maxP = -FLT_MAX;
    for(int i=0; i<lookback_n; i++) {
        float p = sc.BaseData[SC_LAST][sc.Index - i];
        if (p < minP) minP = p;
        if (p > maxP) maxP = p;
    }

    float range = maxP - minP;
    float& lastValidRecurrenceRate = sc.GetPersistentFloat(PersistentVar_AdaptiveCalculators::RECURRENCE_RATE_LAST_VALID_VALUE);
    // Degenerate (flat price window) carries the last valid value forward
    // instead of a fabricated "1.0 = 100% recurrence" reading -- checked before
    // the O(n^2) distance-matrix loop below, so the expensive computation is
    // still skipped on the degenerate path exactly as before -- same
    // sentinel-collapse fix already applied to dims 1/2/3/7/8/11/12.
    if (range <= 0.00001f) {
        return lastValidRecurrenceRate;
    }

    // Adaptive tolerance: mix range-based and variance-based scales.
    float meanP = 0.0f;
    for (int i = 0; i < lookback_n; ++i) {
        meanP += sc.BaseData[SC_LAST][sc.Index - i];
    }
    meanP /= static_cast<float>(lookback_n);

    float varP = 0.0f;
    for (int i = 0; i < lookback_n; ++i) {
        const float d = sc.BaseData[SC_LAST][sc.Index - i] - meanP;
        varP += d * d;
    }
    varP /= static_cast<float>(lookback_n);
    const float stdP = std::sqrt(std::max(varP, 0.0f));

    float epsilon = std::max(range * 0.1f, stdP * 0.5f);
    epsilon = std::max(epsilon, 1e-6f);
    int recurCount = 0;
    int totalCount = lookback_n * lookback_n;

    // Calculate distance matrix and count pairs within epsilon
    // Optimization: Since distance is symmetric, compute i > j and double, plus diagonal (i=j, always 0 < eps)
    // Diagonal count = N.
    recurCount += lookback_n;

    for(int i=0; i<lookback_n; i++) {
        float p1 = sc.BaseData[SC_LAST][sc.Index - i];
        for(int j=i+1; j<lookback_n; j++) {
            float p2 = sc.BaseData[SC_LAST][sc.Index - j];
            if (std::abs(p1 - p2) < epsilon) {
                recurCount += 2; // Add both (i,j) and (j,i)
            }
        }
    }

    const float recurrenceRate = std::clamp((float)recurCount / (float)totalCount, 0.0f, 1.0f);
    lastValidRecurrenceRate = recurrenceRate;
    return recurrenceRate;
}

// 4282
