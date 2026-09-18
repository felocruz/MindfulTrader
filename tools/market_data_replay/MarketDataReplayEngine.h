// MarketDataReplayEngine.h — reusable per-tick pipeline core for the offline
// .context generator (Task 1: TickBarAggregator wiring only, no dim math yet).
// No parquet/Arrow/CLI dependency -- testable on synthetic tick sequences alone
// (docs/superpowers/plans/2026-09-08-market-data-replay-implementation.md).
//
// Does NOT include ContextManager.h/ActivityClockManager.h -- both transitively
// #include "sierrachart.h" and are not includable in this standalone build
// (docs/superpowers/specs/2026-09-08-offline-context-generator-spec.md §3b).

#pragma once

#include "TickBarAggregator.h"
#include "generated/mts_schema_generated.h"
#include "generated/mts_schema_contract_generated.h"
#include "generated/training_shared_writers_generated.h"
#include "BipowerVariation.h"
#include "BoundedBarRing.h"
#include "CandidateObservationDims.h"
#include "CandidateTriggerGate.h"
#include "CarryForwardCalculators.h"
#include "DailyBiasEngine.h"
#include "DfaHurstExponent.h"
#include "ActivityClockMeanReversion.h"
#include "EventVelocityEngine.h"
#include "FeatureScaler.h"
#include "ImbalanceBarEngine.h"
#include "InformationEngine.h"
#include "LiquidityFragilityEngine.h"
#include "MeanReversionCalculator.h"
#include "ObservationTriggerGate.h"
#include "OrderFlowAsymmetryEngine.h"
#include "RecurrenceRateEngine.h"
#include "RingBuffer.h"
#include "RobustMoments.h"
#include "RQAEpsilonSelector.h"
#include "SevcikFractalDimension.h"
#include "TailRiskEngine.h"

// Task 7 (spec §3 item 2): compute primitives for the 5 already-audited
// PRIMARY_TRIGGER_MASK pattern detectors, and the real (ACSIL-independent)
// IndicatorKey enum for genuine dirty-bit parity with production's own
// PRIMARY_TRIGGER_MASK bit positions (src/IndicatorManager.cpp).
#include "EmaEngine.h"
#include "IndicatorComputations.h"
#include "IndicatorKey.h"
#include "RsiEngine.h"
#include "StochasticEngine.h"
#include "WilderAtrEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>
#include <string>

class MarketDataReplayEngine {
public:
    MarketDataReplayEngine()
        : m_ts1(kTs1BarPeriodSeconds, kCmeEsSessionStartSecondsET,
                [this](const tba::Bar& bar) { OnTs1BarClose(bar); }),
          m_ts2(kTs2BarPeriodSeconds, kCmeEsSessionStartSecondsET,
                [this](const tba::Bar& bar) { OnTs2BarClose(bar); }),
          m_ts3(kTs3BarPeriodSeconds, kCmeEsSessionStartSecondsET,
                [this](const tba::Bar& bar) { OnTs3BarClose(bar); }),
          m_dailyBars(kDailyBarPeriodSeconds, kCmeEsSessionStartSecondsET,
                [this](const tba::Bar& bar) { OnDailyBarClose(bar); }),
          m_nr7Ring(kNr7LookbackBars),
          m_turtleSoupRing(kTurtleSoupLookbackBars),
          m_ts2PatternRing(kTs2PatternRingCapacity),
          m_atr10(10),
          m_rsi3(3, RsiSmoothing::SIMPLE),
          m_rsi10(10, RsiSmoothing::SIMPLE),
          m_stoch(3, 3, 3),
          m_ts2Stoch(10, 3, 3),
          m_ts3KeltnerEma10(10),
          m_ts1ImpulseEma13(13),
          m_ts1Macd(12, 26, 9),
          m_ts2ImpulseEma13(13),
          m_ts2Macd(12, 26, 9),
          m_ts2Ema21(21),
          m_ts2AntiStoch(7, 4, 10),
          m_rsiTop(2, RsiSmoothing::WILDERS),
          m_avgVolume14(14) {
        // Hurst's true "no data yet" neutral value is 0.5 (random walk), not
        // ObservationData's zero-initialized 0.0 -- matches CalculateHurstExponent's
        // own fallback (StudyHelperFunctions.cpp:2401). fractal_dim's true
        // cold-start "Brownian guess" is 1.5 (SevcikFractalDimension's own
        // documented fallback, StudyHelperFunctions.cpp:2954) -- 0.0 isn't even
        // inside fractal_dim's valid [1.0,2.0] contract, so leaving it at
        // FlatBuffers' zero-init would be a real correctness bug, not a
        // harmless default. amihud_illiquidity's own cold-start default is
        // also 0.5, not 0.0 (TripleScreen3.cpp:708's own warm-up branch) --
        // same class of fix. Every other TS1/TS2/TS3 dim's true degenerate/
        // neutral value already IS 0.0, so only these three need correcting.
        m_obs.mutate_hurst_exponent(0.5f);
        m_obs.mutate_fractal_dim(1.5f);
        m_obs.mutate_amihud_illiquidity(0.5f);
        // fast_taleb_kurtosis's true cold-start neutral value is 1.23f
        // (ContextManager.cpp's own carry-forward seed), not FlatBuffers'
        // zero-init -- same class of fix as the three above. fast_hurst_
        // exponent shares hurst_exponent's own 0.5f (random-walk) neutral.
        m_obs.mutate_fast_taleb_kurtosis(1.23f);
        m_obs.mutate_fast_hurst_exponent(0.5f);
        // fast_mean_rev_z: wired 2026-09-17 (explicit operator authorization, see
        // ContextManager.cpp's own comment at the live call site for the full trail) --
        // true cold-start neutral value is 0.0f (no deviation), matches FlatBuffers' zero-init.
        m_obs.mutate_fast_mean_rev_z(0.0f);
    }

    // Returns true iff this tick's observation is a significant change and
    // should be written to `.context`.
    bool OnTick(int64_t timestampUs, double price, int64_t volume,
                int64_t askVolume, int64_t bidVolume) {
        // Task 11 (spec §3f): zero/negative price is degenerate input (a bad
        // tick), skipped entirely rather than fed to any engine/rolling
        // window -- matches whole_vector_redundancy_eval.cpp's own
        // `if (price <= 0.0) return;` precedent.
        if (price <= 0.0) return false;

        // Task 11 (spec §3f): the input is ALREADY resequenced upstream
        // (scid_to_ticks_parquet.cpp's own kMaxBenignTimestampJitterUs
        // handling) -- any remaining backwards timestamp here is data
        // corruption, not benign jitter to silently reorder again. Hard
        // error (unlike the skip-and-continue price guard above), since a
        // silent reorder could corrupt every rolling window's chronological
        // assumption.
        if (m_hasLastTimestamp && timestampUs < m_lastTimestampUs) {
            throw std::runtime_error(
                "MarketDataReplayEngine::OnTick: timestamp went backwards (" +
                std::to_string(timestampUs) + " < " + std::to_string(m_lastTimestampUs) +
                ") -- input must already be resequenced upstream");
        }
        m_lastTimestampUs = timestampUs;
        m_hasLastTimestamp = true;

        // Operator directive, 2026-09-16: any dim NOT in mdr::kCandidateDims is
        // a decided-OUT dim -- forced to a hard 0.0f sentinel every tick
        // (unconditionally, even during warmup), never left at its own
        // cold-start/neutral default. Runs first so every real per-dim compute
        // guard below can simply skip an OUT dim's math entirely (the actual
        // "stop computing" savings) without needing its own else-branch.
        ApplyOutDimZeroing();

        m_ts1.OnTick(timestampUs, price, volume, askVolume, bidVolume);
        m_ts2JustClosedThisTick = false;
        m_ts2.OnTick(timestampUs, price, volume, askVolume, bidVolume);
        m_ts3JustClosedThisTick = false;
        m_ts3.OnTick(timestampUs, price, volume, askVolume, bidVolume);
        m_dailyBars.OnTick(timestampUs, price, volume, askVolume, bidVolume);

        // Live-forming TS1 bar tracking: TS1's dims are all close-based (no
        // high/low), so the live bar's own "close" is simply the current
        // tick's price -- no reset-on-new-bar bookkeeping needed at all,
        // unlike TS2/TS3 below (which need high/low).
        ComputeTs1LiveDims(price);

        // Live-forming TS2 bar tracking (mirrors TS3's own pattern below) --
        // relative_range/fractal_dim/log_scale_expansion_ratio/regime tenure
        // are all genuinely per-tick-reactive in production (spec §2a):
        // confirmed via TripleScreen2.cpp's unconditional per-tick block and
        // ContextManager::SetWaveContext()'s own per-TICK (not per-bar)
        // previous-value commit for regime tenure specifically.
        if (m_ts2JustClosedThisTick || !m_hasTs2LiveBar) {
            m_ts2LiveHigh = price;
            m_ts2LiveLow = price;
            m_ts2LiveClose = price;
            m_hasTs2LiveBar = true;
        } else {
            m_ts2LiveHigh = std::max(m_ts2LiveHigh, price);
            m_ts2LiveLow = std::min(m_ts2LiveLow, price);
            m_ts2LiveClose = price;
        }
        ComputeTs2LiveDims();

        // Live-forming TS3 bar tracking (mirrors sc.High/Low/Volume/AskVolume/
        // BidVolume for the STILL-FORMING bar) -- amihud_illiquidity/
        // liq_fragility/micro_asymmetry are all documented every-tick-reactive
        // in production (TripleScreen3.cpp's own 2026-08-29 change), not
        // bar-gated, so this tool must track the live bar itself:
        // TickBarAggregator only exposes COMPLETED bars via its callback.
        if (m_ts3JustClosedThisTick || !m_hasTs3LiveBar) {
            m_ts3LiveOpen = price;
            m_ts3LiveHigh = price;
            m_ts3LiveLow = price;
            m_ts3LiveClose = price;
            m_ts3LiveVolume = volume;
            m_ts3LiveAskVolume = askVolume;
            m_ts3LiveBidVolume = bidVolume;
            m_hasTs3LiveBar = true;
        } else {
            m_ts3LiveHigh = std::max(m_ts3LiveHigh, price);
            m_ts3LiveLow = std::min(m_ts3LiveLow, price);
            m_ts3LiveClose = price;
            m_ts3LiveVolume += volume;
            m_ts3LiveAskVolume += askVolume;
            m_ts3LiveBidVolume += bidVolume;
        }

        ComputeTs3LiveDims();

        // Activity-clock dims (skewness_idx, fast_taleb_kurtosis,
        // fast_hurst_exponent, recurrence_rate): mirrors ActivityClockManager::
        // Update(sc)'s exact real body (src/ActivityClockManager.cpp:17) --
        // OnTickWithPrice(barIndex, askVolume, bidVolume, price) every tick,
        // one call per tick, own ImbalanceBarEngine instance (bypassing
        // ActivityClockManager itself, which #includes sierrachart.h). barIndex
        // is a plain ever-incrementing per-tick counter: production's own
        // sc.AskVolume[sc.Index]/BidVolume[sc.Index] are this study's PER-TICK
        // (not cumulative-per-bar) footprint volumes -- confirmed this session,
        // a direct 1:1 mapping to mes_ticks.parquet's own per-tick columns --
        // so ImbalanceBarEngine::OnTick()'s "barIndex != m_lastBarIndex" reset
        // fires every tick by construction, exactly matching production.
        ComputeActivityClockDims(static_cast<float>(price), askVolume, bidVolume);

        // burstiness_index: tick-level, always fresh, no bar/price-change gate
        // (matches ContextManager::GetRaschkeBurst()'s own tick-level cadence).
        if (mdr::IsCandidateDim(MTS::Schema::Contract::kObsBurstinessIndex)) {
            m_tickTimestamps.push_back(static_cast<uint64_t>(timestampUs));
            if (m_tickTimestamps.size() == kTickTimestampWindow) m_tickTimestamps.pop_front();
            m_obs.mutate_burstiness_index(eve::CalculateBurstinessIndex(m_tickTimestamps));
        }

        // tail_index/lempel_ziv: BOTH fed only on a genuine price CHANGE --
        // confirmed this session (src/ContextManager.cpp:165-166, both calls
        // live inside UpdateMarketPhysics()'s own price-change gate, not
        // "every tick" for TailRiskEngine as an earlier draft of this plan
        // assumed).
        if (!m_hasLastPrice || price != m_lastPrice) {
            if (m_hasLastPrice && m_lastPrice > 0.0) {
                const double logReturn = std::log(price / m_lastPrice);
                // m_tailRiskEngine.AddObservation() stays unconditional --
                // AllDimsReady()'s GetSampleCount()>=50 gate depends on it
                // regardless of tail_index's own IN/OUT status.
                m_tailRiskEngine.AddObservation(logReturn);
                if (mdr::IsCandidateDim(MTS::Schema::Contract::kObsLempelZiv)) {
                    m_infoEngine.AddObservation(logReturn);
                    m_obs.mutate_lempel_ziv(static_cast<float>(m_infoEngine.GetLempelZivComplexity()));
                }
            }
            m_lastPrice = price;
            m_hasLastPrice = true;
        }

        // tail_index: TailRiskEngine is the unconditional sole authority
        // (ContextManager.cpp's own dim 9 block) -- GetHillAlpha() is an O(1)
        // read of the EWMA already advanced by AddObservation() above, so
        // it's safe to evaluate every tick, not just on a price change.
        if (mdr::IsCandidateDim(MTS::Schema::Contract::kObsTailIndex)) {
            if (m_tailRiskEngine.GetSampleCount() >= 50) {
                const float alpha = static_cast<float>(m_tailRiskEngine.GetHillAlpha());
                m_obs.mutate_tail_index((std::isfinite(alpha) && alpha >= 1.1f && alpha <= 8.0f) ? alpha : 2.5f);
            } else {
                m_obs.mutate_tail_index(0.0f);  // Warmup: TRE not yet primed
            }
        }

        return ComputeShouldEmit(timestampUs);
    }

    // Must be called once after the last tick, or the final in-progress bar
    // across all 3 timeframes is silently dropped (spec §3f).
    // Deliberately does NOT flush m_dailyBars: prevDayHigh/prevDayLow must
    // only ever reflect a genuinely COMPLETED trading day (production's own
    // `sc.GetOHLCForDate`-based semantic, IndicatorManager::UpdateDailyCache)
    // -- flushing the still-forming final day here would wrongly promote a
    // partial session's range to "yesterday" at the very end of a replay run.
    void Flush() {
        m_ts1.Flush();
        m_ts2.Flush();
        m_ts3.Flush();
    }

    const MTS::Schema::ObservationData& GetObservation() const { return m_obs; }

    // SystemState's regime-tenure field (Task 4b) -- the tick loop passes this
    // into LBRFileManager::LogContext()'s bars_since_last_update parameter.
    float GetBarsSinceLastUpdate() const { return m_regimeTenure; }

    // Test-only accessors (also reusable for the Logging Policy's progress lines).
    int GetTs1BarsClosed() const { return m_ts1BarsClosed; }
    int GetTs2BarsClosed() const { return m_ts2BarsClosed; }
    int GetTs3BarsClosed() const { return m_ts3BarsClosed; }

    // Task 11: the live (still-forming) TS3 bar's own OHLCV -- production's
    // real GetTrainingEventT() reads sc.Open/High/Low/Close/Volume[sc.Index]
    // for whichever bar its own attached chart is currently on; TS3 is the
    // finest/most-granular timeframe this engine tracks and the one all 17
    // PRIMARY_TRIGGER_MASK detectors are evaluated against (Task 7), so this
    // engine's own TS3 live bar is the direct equivalent. bar_index is this
    // engine's own TS3 bar-close counter, the closest available proxy for
    // sc.Index (both are simple monotonic per-timeframe bar counters).
    float GetTs3LiveOpen() const { return static_cast<float>(m_ts3LiveOpen); }
    float GetTs3LiveHigh() const { return static_cast<float>(m_ts3LiveHigh); }
    float GetTs3LiveLow() const { return static_cast<float>(m_ts3LiveLow); }
    float GetTs3LiveClose() const { return static_cast<float>(m_ts3LiveClose); }
    int64_t GetTs3LiveVolume() const { return m_ts3LiveVolume; }

    // Task 11: Lock A's own real readiness source (FeatureScaler.warmedUp,
    // spec §3 item 1's resolved trace) -- public forwarding accessor.
    bool IsFeatureScalerWarmedUp() const { return m_featureScaler.warmedUp; }
    // Task 11: Locks D/E's own documented offline substitute (this tool's
    // existing per-timeframe bar-count sufficiency check) -- public
    // forwarding accessor for AllDimsReady(), which is otherwise private.
    bool IsAllDimsReady() const { return AllDimsReady(); }

    // Diagnostic accessor (calibration investigation, 2026-09-09) -- mirrors
    // ContextManager::GetLastTriggerDiagnostics()'s own precedent. Only valid
    // after a call to OnTick() that reached the trigger-decision step (i.e.
    // AllDimsReady()+FeatureScaler both warmed up); default-constructed
    // (significant_change=false) otherwise.
    const mdr::CandidateTriggerMetrics& GetLastTriggerMetrics() const { return m_lastTriggerMetrics; }

    // Diagnostic accessor (double-normalization investigation, 2026-09-15) --
    // the FeatureScaler-scaled candidate vector BEFORE CandidateTriggerGate's
    // own separate rolling median/MAD re-normalization. Lets an investigation
    // test distance metrics computed directly from FeatureScaler's output,
    // without going through the gate's own window at all.
    const std::array<float, mdr::kCandidateDimCount>& GetLastScaledCandidateObs() const {
        return m_lastScaledCandidateObs;
    }

    // Task 5 (spec §3 item 2 feasibility assessment): bounded multi-bar
    // history for the PRIMARY_TRIGGER_MASK pattern detectors assembled in
    // Task 7 -- NR7's 7-bar range comparison and TurtleSoup's 20-bar prior-
    // window extremes (both confirmed real requirements, see
    // BoundedBarRing.h's own header for the exact source citations).
    const BoundedBarRing& GetNr7Ring() const { return m_nr7Ring; }
    const BoundedBarRing& GetTurtleSoupRing() const { return m_turtleSoupRing; }

    // Task 7 (spec §3 item 2): the 5 already-audited PRIMARY_TRIGGER_MASK
    // pattern classifications, updated once per completed TS3 bar (matching
    // production's own once-per-bar cadence for all 5).
    KangarooTailEnum GetKangarooTailResult() const { return m_lastKangarooTail; }
    TurtleSoupEnum GetTurtleSoupResult() const { return m_lastTurtleSoup; }
    MomentumPinballEnum GetMomentumPinballResult() const { return m_lastMomentumPinball; }
    ElderBreakoutEnum GetElderBreakoutResult() const { return m_lastElderBreakout; }
    // Diagnostic accessors (test verification that each pattern's own quality score,
    // computed by its Detect*() call, actually makes it onto the wire -- see
    // mutate_*_quality() call sites below).
    float GetKangarooTailQuality() const { return m_lastKangarooTailQuality; }
    float GetTurtleSoupQuality() const { return m_lastTurtleSoupQuality; }
    float GetMomentumPinballQuality() const { return m_lastMomentumPinballQuality; }
    float GetElderBreakoutQuality() const { return m_lastElderBreakoutQuality; }
    float GetNr7Quality() const { return m_lastNr7Quality; }
    // Diagnostic accessor (audit-fix verification): the TS3-local 100-bar
    // DFA(minScale=8) Hurst actually fed into DetectElderBreakout -- distinct
    // from GetObservation().hurst_exponent()'s canonical TS1-based dim.
    float GetElderBreakoutTs3Hurst() const { return m_lastElderBreakoutTs3Hurst; }
    NR7Enum GetNr7Result() const { return m_lastNr7; }
    RaschkeTacticalTrigger GetRaschkeTacticalTriggerResult() const { return m_lastRaschkeTactical; }
    RaschkeStrategySetup GetRaschkeStrategySetupResult() const { return m_lastRaschkeStrategy; }
    // Diagnostic accessor: TS2's own 100-bar DFA(minScale=8) Hurst (60-minute
    // bars) fed into RASCHKE_STRATEGY_SETUP's HOLY_GRAIL patterns -- a THIRD,
    // independent instance of the same fixed-window formula already applied
    // to TS1 (canonical hurst_exponent) and TS3 (ElderBreakout's own local Hurst).
    float GetTs2Hurst() const { return m_lastTs2Hurst; }
    RSI GetRsiTopResult() const { return m_lastRsiTop; }
    StochasticEnum GetIntermStochasticResult() const { return m_lastIntermStochastic; }
    EmaProximity GetEmaProximityResult() const { return m_lastEmaProximity; }
    VolumeEnum GetVolumeSignalResult() const { return m_lastVolumeSignal; }
    dbe::Bias GetDailyBiasResult() const { return m_lastDailyBias; }

    // IndicatorKey::SIDE -- real production value is `PositionManager::
    // GetTradeSide()` (execution-layer state: FLAT=0/LONG=1/SHORT=2,
    // include/Indicator.h's TradeSideEnum), NOT a market-observation signal at
    // all. This offline replay tool has no execution/position state to read
    // (by design -- it only replays historical ticks, it never trades) --
    // by convention, always FLAT (0). The dirty bit for this key therefore
    // never fires past construction (no computed value ever changes it),
    // which faithfully matches what a real, permanently-flat account would
    // also produce in production.
    int8_t GetSideResult() const { return 0; }  // TradeSideEnum::FLAT
    ATRProximityEnum GetAtrProximityResult() const { return m_lastAtrProximity; }
    StructureTest GetStructureTestResult() const { return m_lastStructureTest; }
    int GetLongImpulseColor() const { return m_currentTs1ImpulseColor; }
    int GetIntermImpulseColor() const { return m_currentImpulseColor; }

    // Task 9 (spec §3 item 3): pooled `TrainingEventT` assembly -- mirrors
    // production's real `IndicatorManager::GetTrainingEventT()`'s own "Tier 2"
    // pooled-scratch-object pattern (spec §11 item 1): `m_trainingEventScratch`
    // and its nested `indicators`/`observation` are allocated ONCE (first
    // call) and reused on every subsequent call, never freshly constructed
    // per emission. Every field is unconditionally (re-)written on every call
    // -- in-scope fields with this engine's own real computed values,
    // out-of-scope fields (HMM/regime/AsymmetryContext/dist_*/features --
    // spec §3 item 3's own disposition table) with their documented sentinel
    // -- so no stale value from a prior call is ever visible to the caller
    // (spec §11 item 3's own pooling-hazard warning).
    //
    // barIndex/timestampUs/open/high/low/close/volume are the 4 real
    // extract-to-pure substitutions (spec §3 item 3): the caller (the CLI
    // driver, Task 11) already tracks these from its own tick-processing
    // loop, exactly like production's own `sc.Index`/`sc.BaseDateTimeIn[...]`/
    // `sc.Open/High/Low/Close/Volume[sc.Index]` reads.
    //
    // Non-const return (unlike BuildRiskGateContextT's const one): the
    // pooled object must remain mutable so AlphaFileWriter::LogAlpha() can
    // overwrite sequence_id on it directly (matches production's own
    // LogAlphaUnlocked(TrainingEventT&) contract).
    MTS::Training::TrainingEventT& BuildTrainingEventT(
        int barIndex, int64_t timestampUs,
        float open, float high, float low, float close, int64_t volume) {
        auto& event = m_trainingEventScratch;
        if (!event.indicators) {
            event.indicators = std::make_unique<MTS::Schema::IndicatorState>();
        }
        if (!event.observation) {
            event.observation = std::make_unique<MTS::Schema::ObservationData>();
        }

        event.bar_index = barIndex;
        event.timestamp_us = timestampUs;
        event.open = open;
        event.high = high;
        event.low = low;
        event.close = close;
        event.volume = volume;

        // 17 PRIMARY_TRIGGER_MASK IndicatorState fields (schema/mts_schema.fbs:221),
        // this engine's own already-computed Task 7 results.
        auto& ind = *event.indicators;
        ind.mutate_kangaroo_tail(static_cast<int8_t>(m_lastKangarooTail));
        ind.mutate_kangaroo_tail_quality(m_lastKangarooTailQuality);
        ind.mutate_turtle_soup(static_cast<int8_t>(m_lastTurtleSoup));
        ind.mutate_turtle_soup_quality(m_lastTurtleSoupQuality);
        ind.mutate_momentum_pinball(static_cast<int8_t>(m_lastMomentumPinball));
        ind.mutate_momentum_pinball_quality(m_lastMomentumPinballQuality);
        ind.mutate_elder_breakout(static_cast<int8_t>(m_lastElderBreakout));
        ind.mutate_elder_breakout_quality(m_lastElderBreakoutQuality);
        ind.mutate_nr7(static_cast<int8_t>(m_lastNr7));
        ind.mutate_nr7_quality(m_lastNr7Quality);
        ind.mutate_rsi(static_cast<int8_t>(m_lastRsiTop));
        ind.mutate_interm_stochastic(static_cast<int8_t>(m_lastIntermStochastic));
        ind.mutate_atr_proximity(static_cast<int8_t>(m_lastAtrProximity));
        ind.mutate_ema_proximity(static_cast<int8_t>(m_lastEmaProximity));
        ind.mutate_raschke_strategy_setup(static_cast<int8_t>(m_lastRaschkeStrategy));
        ind.mutate_raschke_tactical_trigger(static_cast<int8_t>(m_lastRaschkeTactical));
        ind.mutate_structure_test(static_cast<int8_t>(m_lastStructureTest));
        ind.mutate_volume_signal(static_cast<int8_t>(m_lastVolumeSignal));
        ind.mutate_daily_bias(static_cast<int8_t>(m_lastDailyBias));
        // long_imp/interm_imp -- KNOWN GAP (2026-09-17, discovered during this
        // task): production's real value is the full ImpulseEnum from
        // ComputeImpulse() (IndicatorComputations.h), needing a dedicated
        // "Impulse ATR" per timeframe (TripleScreen1.cpp's Subgraph_ATR /
        // TripleScreen2.cpp's Array_ImpulseATR -- neither is this engine's
        // existing atr10/relative-range ATR) that this engine does not yet
        // track. This engine only tracks the raw 3-color bucket
        // (GetLongImpulseColor()/GetIntermImpulseColor()), not the refined
        // 8-state enum the wire field actually expects -- using the raw color
        // directly would silently misrepresent the field, not just omit it.
        // UNDEFINED is ComputeImpulse's own documented "not enough data"
        // sentinel -- the honest choice here, not a guessed color mapping.
        // Tracked in the plan doc as a follow-up, not silently left unstated.
        ind.mutate_long_imp(static_cast<int8_t>(ImpulseEnum::UNDEFINED));
        ind.mutate_interm_imp(static_cast<int8_t>(ImpulseEnum::UNDEFINED));
        // impulse_run_length: same gap as long_imp/interm_imp above (both
        // come from the same ComputeImpulse() call this engine doesn't yet
        // make) -- 0 is ComputeImpulse's own genuine cold-start value.
        ind.mutate_impulse_run_length(0);

        // event.observation: direct reuse of this engine's own 18D vector --
        // genuinely the same value production's ContextManager::
        // AddToTrainingEventFB reads (spec §3 item 3's own confirmed finding).
        *event.observation = m_obs;

        // WriteTrainingRootSharedFields-equivalent (schema/mts_schema.fbs's
        // TrainingEvent "Absolute Levels"/execution-context shared fields).
        mts::schema_contract::shared_writers::WriteTrainingRootSharedFields(
            event,
            mts::schema_contract::shared_writers::TrainingRootSharedSlice{
                GetSideResult(),
                0,  // market_symbol: out of scope (MarketSymbolIndicator not ported)
                0,  // overnight_exit: out of scope (OvernightExitTypeEnum not ported)
                0.0f,  // nh_nl_daily: out of scope (NhNlSignal not ported)
                m_prevTs3BarHigh,
                m_prevTs3BarLow,
                m_prevDayHigh,
                m_prevDayLow,
                m_prevFourBarHigh,
                m_prevFourBarLow,
                0.0f,  // close_percentile: out of scope (PriceMetricsIndicator not ported)
                0.0f,  // volume_ratio_percent: out of scope (VolumeIndicator's ratio not ported)
                0.0f,  // volume_imbalance: out of scope (VolumeIndicator's imbalance not ported)
            });

        // model_confidence: spec §5 item 4's resolved decision -- 0.0f, but NOT
        // because production also always returns 0.0f (that was fixed 2026-09-17,
        // EventDataCollectorStudy.cpp now reads InferenceManager::Instance()
        // .Prediction()->Confidence()). This tool still has no value to give here
        // because it runs no live Transformer inference at replay time -- same
        // structural reason as the HMM/regime fields below, not a special case.
        event.model_confidence = 0.0f;

        // Everything else (HMM/regime fields, asymmetry_context, dist_*,
        // volatility/efficiency/rel_range/velocity/regime_tenure, features,
        // Section 12-15 outcome/label fields) is explicitly out of scope for
        // this PRIMARY_TRIGGER_MASK-only initiative (spec §3 item 3's own
        // disposition table) -- left at TrainingEventT's own zero-init
        // defaults, which the pooled object already starts with and this
        // method never touches, so nothing here can carry forward a stale
        // value from a prior call either. lbrnet's own posterior-injection
        // pass (mirroring materialize_hmm_features.py) is the intended fix
        // for these downstream, not a C++ change here -- see
        // HMM_REGIME_MANAGER_COORDINATION.md Entries 9/11.
        return event;
    }

    // Task 10 (spec §5 item 5): RiskGateContextT-equivalent, mirroring
    // ContextManager::BuildRiskGateContext()'s real 17-field copy exactly
    // (src/ContextManager.cpp:851). Pooled the same way as BuildTrainingEventT()
    // (spec §11 item 1) -- a plain member, never freshly constructed per call.
    // 6 fields are direct m_obs/m_featureScaler reads (this engine already
    // retains production's own "raw pre-scaling" values there, confirmed by
    // direct trace -- see this task's own plan-doc scope-correction note);
    // fractal_dim is a genuine gap (documented sentinel, not a wrong-window
    // value); everything else is LocalRiskContext.h's own documented default,
    // explicitly assigned (never left at RiskGateContextT's own generated
    // defaults, which mismatch LocalRiskContext.h's for raschke_burst).
    const MTS::Schema::RiskGateContextT& BuildRiskGateContextT(int64_t timestampUs) {
        auto& rgc = m_riskGateContextScratch;

        rgc.hurst_exponent = m_obs.hurst_exponent();
        rgc.mean_rev_z = m_obs.mean_rev_z();
        rgc.fisher_info = m_obs.fisher_info();
        rgc.amihud_illiquidity = m_obs.amihud_illiquidity();
        rgc.spread_stress = m_obs.liq_fragility();
        rgc.pareto_tail_alpha = m_obs.tail_index();
        rgc.is_valid = m_featureScaler.warmedUp;
        rgc.snapshot_timestamp_us = timestampUs;

        // Genuine gap (2026-09-17): production's real fractalDim is a
        // SEPARATE short-window Sevcik computation (m_fractalDimShortRaw),
        // decoupled from the 400-bar HMM-bound window this engine's own
        // fractal_dim dim already uses -- using m_obs.fractal_dim() here
        // would silently substitute the wrong window. Left at
        // LocalRiskContext.h's own documented neutral instead.
        rgc.fractal_dim = 1.5f;

        // Out of scope (StructureEngine/TailRiskEngine/MarketClimateIndicator/
        // Layer-B rolling-percentile subsystems, none replicated here) --
        // LocalRiskContext.h's own documented defaults, explicit, never left
        // to RiskGateContextT's own generated member initializers (which
        // mismatch for raschke_burst: wire default 1.0f vs. the semantically
        // correct Poisson-neutral 0.0f).
        rgc.shannon_flow_entropy = 0.0f;
        rgc.shannon_efficiency = 0.5f;
        rgc.taleb_kurtosis = 0.0f;
        rgc.taleb_skewness = 0.0f;
        rgc.elder_chandelier_atr = 0.0f;
        // vol_convexity (restored 2026-09-18, RiskGateContext-only): CalculateVolConvexity()
        // is ACSIL-coupled (sc.BaseData reads), not yet ported to this offline engine -- same
        // "out of scope" treatment as the other unreplicated subsystems above.
        rgc.vol_convexity = 0.0f;
        rgc.raschke_burst = 0.0f;
        rgc.regime_duration = 0;
        rgc.amihud_percentile = 0.5f;

        return rgc;
    }

    // Real IndicatorKey bit positions (matches production's
    // PRIMARY_TRIGGER_MASK convention, src/IndicatorManager.cpp) — set when
    // a pattern's classification changed on the most recently completed TS3
    // bar. Consumer clears via ConsumePatternDirtyMask().
    uint64_t ConsumePatternDirtyMask() {
        const uint64_t mask = m_patternDirtyMask;
        m_patternDirtyMask = 0;
        return mask;
    }

private:
    // Matches EventDataCollectorStudy.cpp's CME_ES_SESSION_START_SECS.
    static constexpr int kCmeEsSessionStartSecondsET = 18 * 3600;
    static constexpr int64_t kTs1BarPeriodSeconds = 240 * 60;
    static constexpr int64_t kTs2BarPeriodSeconds = 60 * 60;
    static constexpr int64_t kTs3BarPeriodSeconds = 15 * 60;
    // Task 7 audit fix (STRUCTURE_TEST): production's real "prev_high"/
    // "prev_low" for ClassifyStructure are prevDayHigh/prevDayLow -- the
    // PREVIOUS COMPLETED TRADING DAY's own high/low (confirmed via
    // src/TripleScreen3.cpp:614's real call site + IndicatorManager::
    // UpdateDailyCache's sc.GetOHLCForDate walk-back), NOT the immediately-
    // prior 15-min bar's own high/low, which an earlier pass in this same
    // session wrongly assumed. A 24h TickBarAggregator anchored to the same
    // 18:00 ET session start already trusted for TS1/TS2/TS3 reproduces
    // "trading day" boundaries (not calendar day) without needing
    // sc.GetTradingDayDate()'s exact ACSIL semantics -- ticks never flow
    // across a weekend/holiday close, so the aggregator naturally emits one
    // bar per real trading session.
    static constexpr int64_t kDailyBarPeriodSeconds = 24 * 60 * 60;
    static constexpr size_t kTickTimestampWindow = 100;  // matches production's own window

    // Task 5: NR7 needs the last 7 completed TS3 bars' own ranges
    // (`sc.High[sc.Index-i]-sc.Low[sc.Index-i]` for i in 1..7); TurtleSoup
    // needs a 20-bar prior-window highest-high/lowest-low (confirmed via
    // `sc.Highest(sc.High,...,20)`/`sc.Lowest(sc.Low,...,20)`,
    // `TripleScreen3.cpp:539-540` -- NOT "4 days" despite the real, pre-
    // existing, misleadingly-named `SetPrevFourBarExtremes()`).
    static constexpr int kNr7LookbackBars = 7;
    static constexpr int kTurtleSoupLookbackBars = 20;

    // RASCHKE_STRATEGY_SETUP (TS2, 60-minute bars): 13 sub-patterns
    // (src/StudyHelperFunctions.cpp DetectRaschkeStrategySetup, PatternConstants
    // namespace, values confirmed by direct grep, not assumed). GHOST's own
    // swing-confirmed divergence lookback is the deepest real dependency:
    // priorSwingIndex as far back as (sc.Index-SWING_LENGTH)-GHOST_LOOKBACK,
    // needing SWING_LENGTH more bars beyond THAT to confirm its own swing
    // status -- 20+3+3=26 bars behind "today", so a 30-bar ring (comfortably
    // inside BoundedBarRing's own 32-bar max) covers every sub-pattern with
    // headroom (Gemini-reviewed sizing, CLAUDE_BRIEF_147).
    static constexpr int kTs2PatternRingCapacity = 30;
    static constexpr int kGhostSwingLength = 3;         // SWING_LENGTH (GHOST + DOUBLE_REPO*, all 3)
    static constexpr int kGhostLookback = 20;           // GHOST_LOOKBACK
    static constexpr int kDoubleRepoRetestLookback = 4; // PatternConstants::TURTLE_SOUP_LOOKBACK
                                                         // (a DIFFERENT constant, same name, from
                                                         // this codebase's own RASCHKE_STRATEGY_SETUP
                                                         // -- not to be confused with TS3's
                                                         // kTurtleSoupLookbackBars=20 above).
    static constexpr int kDoubleRepoReversalLookback = 3;  // REVERSAL_BAR_LOOKBACK
    static constexpr float kHurstTrendThreshold = 0.60f;   // HURST_TREND_THRESHOLD
    static constexpr float kTickMultiplierPullback = 2.0f; // TICK_MULTIPLIER_PULLBACK
    static constexpr int kTwoBLookback = 5;             // TWO_B_LOOKBACK (real value 5, despite
                                                         // that section's own stale "20-bar" comments
                                                         // in the live source -- Gemini-confirmed
                                                         // cosmetic-only, CLAUDE_BRIEF_147 Q4).
    static constexpr int kWhiplashLookback = 10;        // WHIPLASH_LOOKBACK
    static constexpr float kWhiplashTopThreshold = 0.75f;
    static constexpr float kWhiplashBottomThreshold = 0.25f;
    static constexpr int kTs2HurstWindowN = 100;        // same fixed DFA(100,8) formula as
    static constexpr size_t kTs2HurstRetained = static_cast<size_t>(kTs2HurstWindowN) + 1;  // TS3's
    static constexpr size_t kTs2HurstCapacity = kTs2HurstRetained + 1;  // ElderBreakout Hurst, TS2-scaled

    // Standard MES exchange tick size (0.25 index points) -- no per-instrument
    // config was found in this repo to read instead; matches production's own
    // sc.TickSize read (shared by ITR breakout and HOLY_GRAIL's pullback checks).
    static constexpr float kMesTickSize = 0.25f;


    // Task 7: Elder Breakout's real "consolidation bars" heuristic
    // (TripleScreen3.cpp's ELDER_CONSOLIDATION_LOOKBACK/ELDER_MIN_CONSOLIDATION_BARS)
    // counts, over the trailing 5 closed bars (including the just-closed one),
    // how many had their OWN close within 1x their OWN concurrent ATR(10) of
    // their OWN concurrent Keltner band -- not a single rolling flag, so the
    // per-bar close/topBand/bottomBand/atr history must be retained.
    static constexpr size_t kElderConsolidationLookback = 5;
    static constexpr int kElderMinConsolidationBars = 3;
    static constexpr size_t kElderConsolidationCapacity = kElderConsolidationLookback + 1;

    // VOLUME_SIGNAL's real session routing: RTH = IndicatorKey::TIME_OF_DAY's
    // OPENING_HOUR..PM_RUN_ENTRY band (src/TripleScreen3.cpp), i.e. 09:30-16:00
    // ET (confirmed via TimeOfDayEnum's own real value comments, Indicator.h).
    static bool IsRthSession(int64_t timestampUs) {
        const int64_t utcSeconds = timestampUs / 1'000'000LL;
        const int offsetSeconds = ete::GetEasternUtcOffsetSeconds(utcSeconds);
        int64_t etSecondsOfDay = (utcSeconds + offsetSeconds) % 86400;
        if (etSecondsOfDay < 0) etSecondsOfDay += 86400;
        constexpr int64_t kRthStartSeconds = 9 * 3600 + 30 * 60;   // 09:30 ET
        constexpr int64_t kRthEndSeconds = 16 * 3600;              // 16:00 ET
        return etSecondsOfDay >= kRthStartSeconds && etSecondsOfDay < kRthEndSeconds;
    }

    static constexpr uint64_t IndicatorKeyBit(IndicatorKey key) {
        return 1ULL << static_cast<uint64_t>(key);
    }

    // ITR's real per-day reset uses barTime.GetDate() (a plain ET calendar
    // date), NOT a trading-day boundary -- this coarse "day number" (ET
    // seconds-since-epoch floor-divided by 86400) is a faithful proxy for
    // change-detection purposes without needing full Y/M/D decoding.
    static void GetEasternTimeParts(int64_t timestampUs, int& hour, int& minute, int64_t& dayNumber) {
        const int64_t utcSeconds = timestampUs / 1'000'000LL;
        const int offsetSeconds = ete::GetEasternUtcOffsetSeconds(utcSeconds);
        const int64_t etSeconds = utcSeconds + offsetSeconds;
        int64_t etSecondsOfDay = etSeconds % 86400;
        if (etSecondsOfDay < 0) etSecondsOfDay += 86400;
        hour = static_cast<int>(etSecondsOfDay / 3600);
        minute = static_cast<int>((etSecondsOfDay % 3600) / 60);
        dayNumber = (etSeconds >= 0) ? (etSeconds / 86400) : ((etSeconds - 86399) / 86400);
    }

    // Sierra Chart's real IsSwingHigh/IsSwingLow (sierrachart.h, verbatim):
    // bar at Index qualifies only if STRICTLY greater/less than ALL of its
    // own Length neighbors on BOTH sides -- a symmetric, look-ahead-required
    // definition (Gemini-reviewed replication strategy, CLAUDE_BRIEF_147 Q1).
    // Evaluated directly against m_ts2PatternRing's own chronological
    // HighAt/LowAt (0=oldest); returns false (not a swing, matching the "skip
    // if not yet evaluable" resolution to CLAUDE_BRIEF_147 Q2/148) whenever
    // either flank would read outside [0, Count()-1] -- i.e. this can never
    // read a not-yet-closed future bar, unlike the live code's own real
    // edge-case gap for small lookback combinations.
    bool IsSwingHighAt(int ringIndex, int length) const {
        if (ringIndex - length < 0 || ringIndex + length > m_ts2PatternRing.Count() - 1) return false;
        const float v = m_ts2PatternRing.HighAt(ringIndex);
        for (int offset = 1; offset <= length; ++offset) {
            if (v <= m_ts2PatternRing.HighAt(ringIndex - offset)) return false;
            if (v <= m_ts2PatternRing.HighAt(ringIndex + offset)) return false;
        }
        return true;
    }
    bool IsSwingLowAt(int ringIndex, int length) const {
        if (ringIndex - length < 0 || ringIndex + length > m_ts2PatternRing.Count() - 1) return false;
        const float v = m_ts2PatternRing.LowAt(ringIndex);
        for (int offset = 1; offset <= length; ++offset) {
            if (v >= m_ts2PatternRing.LowAt(ringIndex - offset)) return false;
            if (v >= m_ts2PatternRing.LowAt(ringIndex + offset)) return false;
        }
        return true;
    }

    // TS1 dims (log_scale_ratio, hurst_exponent, fisher_info) all use a FIXED
    // 100-bar window here -- a deliberate, documented simplification vs.
    // the existing codebase's adaptive macro_window_n/fisher_window_n
    // (StudyHelperFunctions.cpp's CalculateAdaptiveObservationWindow/
    // CalculateFisherAdaptiveWindow, both ACSIL-coupled, not yet ported).
    // Matches tools/observation_vector/whole_vector_redundancy_eval.cpp's own
    // documented precedent for this exact simplification. hurst_exponent's
    // 100-bar/minScale=8 pair specifically matches CalculateHurstExponent(sc)'s
    // own "legacy... standard institutional settings" fixed default
    // (StudyHelperFunctions.cpp:2436-2440), not an invented number.
    static constexpr int kTs1WindowN = 100;
    static constexpr size_t kTs1CloseWindow = kTs1WindowN;          // CLOSED closes held; the live
                                                                     // close is appended as the
                                                                     // (N+1)th point (Task 6b)
    static constexpr size_t kTs1CloseCapacity = kTs1CloseWindow + 1;  // + headroom for push-then-pop

    // Operator directive, 2026-09-16: forces every currently-OUT dim (any dim
    // not in mdr::kCandidateDims) to a hard 0.0f sentinel, unconditionally,
    // every tick -- called first thing in OnTick() so it applies even before
    // any per-timeframe warmup completes. fast_mean_rev_z (dim 17) is IN
    // kCandidateDims (never OUT) and is now genuinely computed (2026-09-17,
    // explicit operator authorization) in the shared imbalance-bar-return
    // block below, same as its 4 siblings -- no entry needed here either way.
    void ApplyOutDimZeroing() {
        using namespace MTS::Schema::Contract;
        if (!mdr::IsCandidateDim(kObsLogScaleRatio)) m_obs.mutate_log_scale_ratio(0.0f);
        if (!mdr::IsCandidateDim(kObsBurstinessIndex)) m_obs.mutate_burstiness_index(0.0f);
        if (!mdr::IsCandidateDim(kObsRelativeRange)) m_obs.mutate_relative_range(0.0f);
        if (!mdr::IsCandidateDim(kObsLogScaleExpansionRatio)) m_obs.mutate_log_scale_expansion_ratio(0.0f);
        if (!mdr::IsCandidateDim(kObsLempelZiv)) m_obs.mutate_lempel_ziv(0.0f);
        if (!mdr::IsCandidateDim(kObsHurstExponent)) m_obs.mutate_hurst_exponent(0.0f);
        if (!mdr::IsCandidateDim(kObsMicroAsymmetry)) m_obs.mutate_micro_asymmetry(0.0f);
        if (!mdr::IsCandidateDim(kObsFisherInfo)) m_obs.mutate_fisher_info(0.0f);
        if (!mdr::IsCandidateDim(kObsFastHurstExponent)) m_obs.mutate_fast_hurst_exponent(0.0f);
        if (!mdr::IsCandidateDim(kObsTailIndex)) m_obs.mutate_tail_index(0.0f);
        if (!mdr::IsCandidateDim(kObsSkewnessIdx)) m_obs.mutate_skewness_idx(0.0f);
        if (!mdr::IsCandidateDim(kObsAmihudIlliquidity)) m_obs.mutate_amihud_illiquidity(0.0f);
        if (!mdr::IsCandidateDim(kObsLiqFragility)) m_obs.mutate_liq_fragility(0.0f);
        if (!mdr::IsCandidateDim(kObsFastTalebKurtosis)) m_obs.mutate_fast_taleb_kurtosis(0.0f);
        if (!mdr::IsCandidateDim(kObsRecurrenceRate)) m_obs.mutate_recurrence_rate(0.0f);
        if (!mdr::IsCandidateDim(kObsFractalDim)) m_obs.mutate_fractal_dim(0.0f);
        if (!mdr::IsCandidateDim(kObsMeanRevZ)) m_obs.mutate_mean_rev_z(0.0f);
    }

    // Task 7 audit fix: feeds STRUCTURE_TEST's real prevDayHigh/prevDayLow
    // inputs (see kDailyBarPeriodSeconds's own comment for the full citation).
    // Fires once per completed CME trading session (18:00 ET session-start-
    // aligned 24h bar), i.e. exactly the cadence `IndicatorManager::
    // UpdateDailyCache`'s own "update cache only when trading day changes"
    // gate uses in production.
    void OnDailyBarClose(const tba::Bar& bar) {
        m_prevDayHigh = static_cast<float>(bar.high);
        m_prevDayLow = static_cast<float>(bar.low);
        m_hasPrevDayRange = true;
    }

    void OnTs1BarClose(const tba::Bar& bar) {
        ++m_ts1BarsClosed;
        m_ts1Closes.push_back(static_cast<float>(bar.close));
        // FIX (found while designing Task 4, re-deriving this same pattern more
        // carefully): must pop only once the window is EXCEEDED (size >
        // kTs1CloseWindow), not when size merely equals capacity -- the RingBuffer
        // header's own documented convention ("Capacity = maxWindowSize + 1" for
        // this exact push-then-pop shape). The original `size() == kTs1CloseWindow`
        // check popped one element too early, so the buffer could never actually
        // stabilize at kTs1CloseWindow elements -- ComputeTs1Dims()'s warm-up guard
        // below would never pass, and TS1 dims would silently stay at their
        // cold-start defaults forever. Caught by hand-tracing the push/pop
        // sequence, not by the (too-weak) tests -- see the strengthened Task 3
        // test below.
        if (m_ts1Closes.size() > kTs1CloseWindow) m_ts1Closes.pop_front();

        // Task 7 (spec §3 item 2): LONG_IMP -- TS1's own Elder Impulse color.
        // Real source (src/TripleScreen1.cpp:249,287-298 + scsf_Screen1_MACD's
        // real (12,26,9) EMA-on-close defaults, a repo-owned custom study,
        // same pattern as TS2's own scsf_Screen2_MACD): maDiff = EMA(13 of
        // CLOSE, NOT OHLC-avg -- confirmed Input_InputData defaults to
        // SC_LAST here, unlike TS2/TS3's Keltner-style EMAs) delta;
        // macdDiff = MACD(12,26,9) histogram delta.
        const float ts1Close = static_cast<float>(bar.close);
        const float impulseEmaValue = m_ts1ImpulseEma13.OnValue(ts1Close);
        const float macdHistogram = m_ts1Macd.OnValue(ts1Close);
        if (m_hasPrevTs1ImpulseEma && m_hasPrevTs1MacdHistogram) {
            const float maDiff = impulseEmaValue - m_prevTs1ImpulseEmaValue;
            const float macdDiff = macdHistogram - m_prevTs1MacdHistogram;
            m_currentTs1ImpulseColor = GetImpulseColorLike(maDiff, macdDiff);
        }
        m_prevTs1ImpulseEmaValue = impulseEmaValue;
        m_hasPrevTs1ImpulseEma = true;
        m_prevTs1MacdHistogram = macdHistogram;
        m_hasPrevTs1MacdHistogram = true;
        if (m_currentTs1ImpulseColor != m_lastEmittedTs1ImpulseColor) {
            m_patternDirtyMask |= IndicatorKeyBit(IndicatorKey::LONG_IMP);
        }
        m_lastEmittedTs1ImpulseColor = m_currentTs1ImpulseColor;
        // No longer calls the dim math here (Task 6b) -- ComputeTs1LiveDims()
        // runs every tick from OnTick() instead, matching the existing
        // per-tick reactivity in CalculateHurstExponent/TripleScreen1.cpp
        // (spec §2a). This callback only advances the closed-bar history.
    }

    // Task 6b: called every tick, not just at bar close -- CalculateHurstExponent/
    // CalculateFisherInformation/CalculateLogScaleRatio recompute EVERY TICK
    // from a window ending at the current, still-forming bar (confirmed
    // against CalculateHurstExponent's real ACSIL indexing, spec §2a). `price`
    // is the live/forming bar's own evolving close (TS1 has no high/low
    // dependency, so no separate live-bar-tracking state is needed beyond
    // this parameter).
    void ComputeTs1LiveDims(double price) {
        const size_t closedSz = m_ts1Closes.size();
        if (closedSz < kTs1WindowN) return;  // not warmed up yet -- carry forward (do nothing)

        // Combined [closed...][live] window: kTs1WindowN+1 prices total,
        // matching CalculateHurstExponent's own sc.BaseData[SC_LAST]
        // [sc.Index-length+1 .. sc.Index] indexing, where the window's final
        // point is always the live/forming bar, not the last CLOSED bar.
        std::array<float, kTs1WindowN + 1> prices{};
        for (size_t i = 0; i < static_cast<size_t>(kTs1WindowN); ++i) {
            prices[i] = m_ts1Closes[closedSz - static_cast<size_t>(kTs1WindowN) + i];
        }
        prices[static_cast<size_t>(kTs1WindowN)] = static_cast<float>(price);
        const size_t sz = static_cast<size_t>(kTs1WindowN) + 1;

        // log_scale_ratio: log(short_BV / long_BV), Barndorff-Nielsen & Shephard
        // bipower variation -- ports CalculateLogScaleRatio's own formula
        // (StudyHelperFunctions.cpp:2771) onto this tool's own close buffer.
        if (mdr::IsCandidateDim(MTS::Schema::Contract::kObsLogScaleRatio)) {
            constexpr int kShortN = 25;  // matches CalculateLogScaleRatio's own max(8, long_n/4) at long_n=100
            auto windowBv = [&prices, sz](int windowN) -> double {
                double logReturns[kTs1WindowN];
                int count = 0;
                for (int i = 0; i < windowN; ++i) {
                    const size_t idx = sz - static_cast<size_t>(windowN) + static_cast<size_t>(i);
                    if (idx == 0) continue;
                    const float p = prices[idx];
                    const float prevPrice = prices[idx - 1];
                    if (p > 0.0f && prevPrice > 0.0f) {
                        logReturns[count++] = std::log(static_cast<double>(p) / prevPrice);
                    }
                }
                return ComputeBipowerVariation(logReturns, count);
            };
            const double shortBv = windowBv(kShortN);
            const double longBv = windowBv(kTs1WindowN);
            constexpr double kVarEps = 1e-12;
            const double logRatio = std::log((shortBv + kVarEps) / (longBv + kVarEps));
            m_obs.mutate_log_scale_ratio(std::clamp(static_cast<float>(logRatio), -6.0f, 6.0f));
        }

        // hurst_exponent: DfaHurstExponent over the fixed 100-bar/minScale=8
        // window (StudyHelperFunctions.cpp:2393-2409's own carry-forward policy
        // ported: NaN -> leave last valid value / 0.5 default in place).
        if (mdr::IsCandidateDim(MTS::Schema::Contract::kObsHurstExponent)) {
            std::array<float, kTs1WindowN> logReturns{};
            for (int i = 0; i < kTs1WindowN; ++i) {
                const size_t idx = sz - static_cast<size_t>(kTs1WindowN) + static_cast<size_t>(i);
                const float p = prices[idx];
                const float prevPrice = prices[idx - 1];
                logReturns[static_cast<size_t>(i)] = (p > 0.0f && prevPrice > 0.0f)
                    ? static_cast<float>(std::log(static_cast<double>(p) / prevPrice)) : 0.0f;
            }
            const float hurst = DfaHurstExponent(logReturns.data(), kTs1WindowN, /*minScale=*/8);
            if (std::isfinite(hurst)) {
                m_obs.mutate_hurst_exponent(hurst);
            }
        }

        // fisher_info: Fisher transform over the same fixed 100-bar window's
        // raw closes (StudyHelperFunctions.cpp:2817-2829's own formula).
        if (mdr::IsCandidateDim(MTS::Schema::Contract::kObsFisherInfo)) {
            float minPrice = std::numeric_limits<float>::max();
            float maxPrice = -std::numeric_limits<float>::max();
            for (int i = 0; i < kTs1WindowN; ++i) {
                const float p = prices[sz - 1 - static_cast<size_t>(i)];
                minPrice = std::min(minPrice, p);
                maxPrice = std::max(maxPrice, p);
            }
            const float currentPrice = prices[sz - 1];
            const float fisherInfo =
                cfc::ComputeFisherInformation(minPrice, maxPrice, currentPrice, m_lastValidFisherInfo);
            m_lastValidFisherInfo = fisherInfo;
            m_obs.mutate_fisher_info(fisherInfo);
        }
    }

    // TS2 dims (relative_range, log_scale_expansion_ratio, fractal_dim). Each
    // warms up on its own timeline (matches the existing codebase's own
    // per-dim independence) -- log_scale_expansion_ratio needs only 21
    // closes (20 closed + 1 live, Task 6b), fractal_dim needs the full 401
    // (400 closed + 1 live).
    static constexpr int kTs2AtrPeriod = 14;                          // matches sc.ATR(...,14,MOVAVGTYPE_SIMPLE);
                                                                       // total window incl. the live bar
    static constexpr size_t kTs2AtrClosedWindow = kTs2AtrPeriod - 1;  // 13 CLOSED true ranges held
    static constexpr size_t kTs2AtrCapacity = kTs2AtrClosedWindow + 1;  // + headroom for push-then-pop
    static constexpr int kTs2ObsWindowN = 20;                        // CalculateAdaptiveObservationWindow's
                                                                      // own seed value (clamp range [10,40]),
                                                                      // a real sanctioned default, not invented
    static constexpr int kTs2FractalWindowN = 400;                   // TripleScreen2.cpp's kFractalDimHmmWindow
    static constexpr size_t kTs2CloseWindow = kTs2FractalWindowN;    // CLOSED closes held; the live close
                                                                      // is appended as the (N+1)th point
    static constexpr size_t kTs2CloseCapacity = kTs2CloseWindow + 1;   // + headroom for push-then-pop
    static constexpr size_t kTs2EmaStdDevWindow = 13;  // sc.StdDeviation(Subgraph_KeltnerAverage,...,13)
    static constexpr size_t kTs2EmaHistCapacity = kTs2EmaStdDevWindow + 1;  // + headroom

    void OnTs2BarClose(const tba::Bar& bar) {
        ++m_ts2BarsClosed;
        m_ts2JustClosedThisTick = true;
        const bool hadPrevTs2Close = m_hasTs2PrevClose;
        const float prevTs2CloseForEmaProximity = m_ts2PrevClose;  // captured before overwrite below
        // RASCHKE_STRATEGY_SETUP's BREAD_AND_BUTTER needs the PRIOR bar's own
        // impulseEmaValue (its "prevEma") -- captured here, before this same
        // function's own INTERM_IMP block overwrites m_prevTs2ImpulseEmaValue
        // with today's value further down.
        const float prevImpulseEmaForStrategy = m_prevTs2ImpulseEmaValue;
        const bool hasPrevImpulseEmaForStrategy = m_hasPrevTs2ImpulseEma;

        float trueRange;
        if (m_hasTs2PrevClose) {
            const float highLow = static_cast<float>(bar.high - bar.low);
            const float highPrevClose = std::fabs(static_cast<float>(bar.high) - m_ts2PrevClose);
            const float lowPrevClose = std::fabs(static_cast<float>(bar.low) - m_ts2PrevClose);
            trueRange = std::max({highLow, highPrevClose, lowPrevClose});
        } else {
            trueRange = static_cast<float>(bar.high - bar.low);
        }
        m_ts2TrueRanges.push_back(trueRange);
        if (m_ts2TrueRanges.size() > kTs2AtrClosedWindow) m_ts2TrueRanges.pop_front();

        m_ts2Closes.push_back(static_cast<float>(bar.close));
        if (m_ts2Closes.size() > kTs2CloseWindow) m_ts2Closes.pop_front();
        m_ts2PrevClose = static_cast<float>(bar.close);
        m_hasTs2PrevClose = true;

        // Task 7 (spec §3 item 2): INTERM_IMP -- TS2's own Elder Impulse
        // color, read cross-timeframe by TS3's MomentumPinball. Real source
        // (src/TripleScreen2.cpp:242,261-263,700 + scsf_Screen2_MACD's own
        // real (12,26,9) EMA-on-close defaults, a repo-owned custom study,
        // not a third-party chart reference): maDiff = EMA(13 of OHLC-avg)
        // delta; macdDiff = MACD(12,26,9) histogram delta.
        const float ts2OhlcAvg = static_cast<float>((bar.open + bar.high + bar.low + bar.close) / 4.0);
        const float impulseEmaValue = m_ts2ImpulseEma13.OnValue(ts2OhlcAvg);
        const float macdHistogram = m_ts2Macd.OnValue(static_cast<float>(bar.close));
        if (m_hasPrevTs2ImpulseEma && m_hasPrevTs2MacdHistogram) {
            const float maDiff = impulseEmaValue - m_prevTs2ImpulseEmaValue;
            const float macdDiff = macdHistogram - m_prevTs2MacdHistogram;
            m_currentImpulseColor = GetImpulseColorLike(maDiff, macdDiff);
        }
        m_prevTs2ImpulseEmaValue = impulseEmaValue;
        m_hasPrevTs2ImpulseEma = true;
        m_prevTs2MacdHistogram = macdHistogram;
        m_hasPrevTs2MacdHistogram = true;
        if (m_currentImpulseColor != m_lastEmittedTs2ImpulseColor) {
            m_patternDirtyMask |= IndicatorKeyBit(IndicatorKey::INTERM_IMP);
        }
        m_lastEmittedTs2ImpulseColor = m_currentImpulseColor;

        // Task 7 (spec §3 item 2): IndicatorKey::RSI -- a top-level TS2 RSI,
        // period=2 WILDERS (real config confirmed at the top of
        // scsf_Screen2_MACD, src/TripleScreen2.cpp:385-386 -- a DIFFERENT
        // config from MomentumPinball's own rsi3/rsi10 SIMPLE on TS3).
        const float rsiTopValue = m_rsiTop.OnClose(static_cast<float>(bar.close));
        const RSI rsiTopResult = DetectRSI(rsiTopValue);
        if (rsiTopResult != m_lastRsiTop) {
            m_patternDirtyMask |= IndicatorKeyBit(IndicatorKey::RSI);
        }
        m_lastRsiTop = rsiTopResult;

        // Task 7 (spec §3 item 2): IndicatorKey::INTERM_STOCHASTIC -- TS2's own
        // Stochastic(10,3,3) SIMPLE crossover classification
        // (src/TripleScreen2.cpp scsf_Screen2_StochasticCrossover) -- a
        // DIFFERENT config from TS3's own (3,3,3) MomentumPinball stochastic.
        // Divergence detection (a separate, disabled-by-default branch) is
        // NOT ported, matching this repo's own default config.
        m_ts2Stoch.OnBar(static_cast<float>(bar.high), static_cast<float>(bar.low), static_cast<float>(bar.close));
        const float ts2StochRawK = m_ts2Stoch.RawK();
        const float ts2StochFastD = m_ts2Stoch.FastD();
        if (m_hasPrevTs2Stoch) {
            const StochasticEnum result = ClassifyStochasticCrossover(
                ts2StochRawK, ts2StochFastD, m_prevTs2StochRawK, m_prevTs2StochFastD);
            if (result != m_lastIntermStochastic) {
                m_patternDirtyMask |= IndicatorKeyBit(IndicatorKey::INTERM_STOCHASTIC);
            }
            m_lastIntermStochastic = result;
        }
        m_prevTs2StochRawK = ts2StochRawK;
        m_prevTs2StochFastD = ts2StochFastD;
        m_hasPrevTs2Stoch = true;

        // Task 7 (spec §3 item 2): IndicatorKey::EMA_PROXIMITY -- TS2's own
        // Keltner EMA(13)-of-OHLC-avg proximity classification
        // (src/TripleScreen2.cpp scsf_Screen2_KeltnerChannel). `ema` reuses
        // `impulseEmaValue` above (confirmed identical formula: EMA(13) of
        // SC_OHLC_AVG). `stdDev` is a rolling 13-bar POPULATION stddev of
        // that EMA line's OWN values (`sc.StdDeviation(Subgraph_KeltnerAverage,
        // ..., 13)`) -- population vs. sample denominator is unconfirmable
        // from the vendored ACSIL headers (closed-source engine internals),
        // flagged for Task 12's empirical validation, not silently assumed.
        m_ts2EmaHist.push_back(impulseEmaValue);
        if (m_ts2EmaHist.size() > kTs2EmaStdDevWindow) m_ts2EmaHist.pop_front();
        if (hadPrevTs2Close && m_ts2EmaHist.size() >= kTs2EmaStdDevWindow) {
            float sum = 0.0f;
            const size_t sz = m_ts2EmaHist.size();
            for (size_t i = 0; i < sz; ++i) sum += m_ts2EmaHist[i];
            const float mean = sum / static_cast<float>(sz);
            float sqSum = 0.0f;
            for (size_t i = 0; i < sz; ++i) {
                const float d = m_ts2EmaHist[i] - mean;
                sqSum += d * d;
            }
            const float stdDev = std::sqrt(sqSum / static_cast<float>(sz));
            const EmaProximity result = ClassifyEmaProximity(
                static_cast<float>(bar.close), prevTs2CloseForEmaProximity, impulseEmaValue, stdDev);
            if (result != m_lastEmaProximity) {
                m_patternDirtyMask |= IndicatorKeyBit(IndicatorKey::EMA_PROXIMITY);
            }
            m_lastEmaProximity = result;
        }

        // --- RASCHKE_STRATEGY_SETUP (13-pattern cascade, TS2 60-minute bars).
        // Single monolithic block mirroring production's own early-return
        // cascade order exactly (Gemini-endorsed structure, CLAUDE_BRIEF_147
        // Q5: 1:1 structural correspondence with src/StudyHelperFunctions.cpp's
        // DetectRaschkeStrategySetup makes this trivial to audit/diff, vs.
        // splitting into free functions that would need boilerplate to halt
        // the cascade). Gated on the pattern ring being full (30 bars) as a
        // single, deliberately conservative blanket warm-up gate -- simpler
        // than replicating each of the 13 sub-patterns' own smaller individual
        // thresholds (smallest real gate is TWO_B_LOOKBACK=5), a documented
        // simplification, not a correctness gap (every pattern is still
        // evaluated correctly once the ring fills; only the ~25-bar ramp-up
        // window differs from production's own earlier per-pattern availability). ---

        // TS2-local 100-bar DFA(minScale=8) Hurst (HOLY_GRAIL's `hurst` input)
        // -- mirrors ElderBreakout's own TS3-local Hurst fix exactly, just on
        // TS2's 60-minute closes (src/TripleScreen2.cpp:730-738's own
        // `CalculateHurstExponent(sc)` zero-arg overload, fixed (100,8)).
        m_ts2HurstCloses.push_back(static_cast<float>(bar.close));
        if (m_ts2HurstCloses.size() > kTs2HurstRetained) m_ts2HurstCloses.pop_front();
        if (m_ts2HurstCloses.size() >= kTs2HurstRetained) {
            std::array<float, kTs2HurstWindowN> hurstLogReturns{};
            const size_t hSz = m_ts2HurstCloses.size();
            for (int i = 0; i < kTs2HurstWindowN; ++i) {
                const size_t idx = hSz - kTs2HurstRetained + static_cast<size_t>(i) + 1;
                const float p = m_ts2HurstCloses[idx];
                const float prevP = m_ts2HurstCloses[idx - 1];
                hurstLogReturns[static_cast<size_t>(i)] = (p > 0.0f && prevP > 0.0f)
                    ? std::log(p / prevP) : 0.0f;
            }
            const float hurst = DfaHurstExponent(hurstLogReturns.data(), kTs2HurstWindowN, /*minScale=*/8);
            if (std::isfinite(hurst)) m_lastTs2Hurst = hurst;
        }

        // ema21 (HOLY_GRAIL/BREAD_AND_BUTTER's `ema21` parameter).
        const float ema21 = m_ts2Ema21.OnValue(static_cast<float>(bar.close));

        // Advance the 30-bar pattern ring (POST-update state used throughout
        // this cascade -- unlike TurtleSoup's PRE-update read, GHOST's own
        // fixed swing anchor at cur-SWING_LENGTH needs TODAY's own bar as its
        // forward confirmation flank, so "today" must already be IN the ring).
        m_ts2PatternRing.OnBarClose(static_cast<float>(bar.high), static_cast<float>(bar.low));
        m_ts2PatternCloses.push_back(static_cast<float>(bar.close));
        if (m_ts2PatternCloses.size() > static_cast<size_t>(kTs2PatternRingCapacity)) m_ts2PatternCloses.pop_front();
        m_ts2PatternVolumes.push_back(static_cast<float>(bar.volume));
        if (m_ts2PatternVolumes.size() > static_cast<size_t>(kTs2PatternRingCapacity)) m_ts2PatternVolumes.pop_front();

        // ANTI's own Stochastic(7,4,10) SIMPLE -- a DIFFERENT config from
        // both TS3's MomentumPinball (3,3,3) and TS2's own INTERM_STOCHASTIC
        // (10,3,3). Production's own k/d naming is confusing: "k" is FastD
        // (the 4-smoothed raw K), "d" is SlowD (10-period SMA of that) --
        // preserved here via m_antiKHist/m_antiDHist's own doc comments, not
        // renamed, to keep this port auditable line-for-line against source.
        m_ts2AntiStoch.OnBar(static_cast<float>(bar.high), static_cast<float>(bar.low), static_cast<float>(bar.close));
        m_antiKHist.push_back(m_ts2AntiStoch.FastD());
        if (m_antiKHist.size() > 3) m_antiKHist.pop_front();
        m_antiDHist.push_back(m_ts2AntiStoch.SlowD());
        if (m_antiDHist.size() > 3) m_antiDHist.pop_front();

        // RASCHKE_STRATEGY_SETUP's own independent MacdEnum classification
        // (SLINGSHOT/GHOST/FIRST_CROSS) -- ComputeMacd() is already pure
        // (IndicatorComputations.h), fed macdHistogram (already computed
        // above for INTERM_IMP) through a dedicated 2-deep diffPrev1/
        // diffPrev2 shift register (Macd::SetFromChart's own real state
        // machine, src/Indicator.cpp:29-44).
        {
            const int barsAvailable = std::min(m_ts2StrategyMacdBarsSeen, 2);
            const MacdResult stratMacdResult = ComputeMacd(
                m_ts2StrategyMacdState, macdHistogram,
                m_ts2StrategyMacdDiffPrev1, m_ts2StrategyMacdDiffPrev2, barsAvailable);
            m_currentTs2StrategyMacd = stratMacdResult.signal;
            m_ts2StrategyMacdDiffPrev2 = m_ts2StrategyMacdDiffPrev1;
            m_ts2StrategyMacdDiffPrev1 = macdHistogram;
            if (m_ts2StrategyMacdBarsSeen < 2) ++m_ts2StrategyMacdBarsSeen;
        }

        if (m_ts2PatternRing.IsFull()) {
            const int cur = m_ts2PatternRing.Count() - 1;
            const float curHigh = m_ts2PatternRing.HighAt(cur);
            const float curLow = m_ts2PatternRing.LowAt(cur);
            const float curClose = m_ts2PatternCloses[static_cast<size_t>(cur)];
            RaschkeStrategySetup strategyResult = RaschkeStrategySetup::NONE;

            // --- Holy Grail (Linda Raschke's exact specification) ---
            if (m_lastTs2Hurst > kHurstTrendThreshold && cur >= 2) {
                const float close1 = m_ts2PatternCloses[static_cast<size_t>(cur - 1)];
                const float close2 = m_ts2PatternCloses[static_cast<size_t>(cur - 2)];
                const float low1 = m_ts2PatternRing.LowAt(cur - 1);
                const float high1 = m_ts2PatternRing.HighAt(cur - 1);

                const bool hadEstablishedUptrend = (close1 > ema21 && close2 > ema21);
                const bool pullbackTouchesEma = (curLow <= ema21 + kMesTickSize);
                const bool maintainsUptrend = (curClose >= ema21 - kMesTickSize);
                const bool wasAwayFromEma = (low1 > ema21 + (kTickMultiplierPullback * kMesTickSize));
                if (hadEstablishedUptrend && pullbackTouchesEma && maintainsUptrend && wasAwayFromEma) {
                    strategyResult = RaschkeStrategySetup::HOLY_GRAIL_BUY;
                }

                if (strategyResult == RaschkeStrategySetup::NONE) {
                    const bool hadEstablishedDowntrend = (close1 < ema21 && close2 < ema21);
                    const bool rallyTouchesEma = (curHigh >= ema21 - kMesTickSize);
                    const bool maintainsDowntrend = (curClose <= ema21 + kMesTickSize);
                    const bool wasAwayFromEmaDown = (high1 < ema21 - (kTickMultiplierPullback * kMesTickSize));
                    if (hadEstablishedDowntrend && rallyTouchesEma && maintainsDowntrend && wasAwayFromEmaDown) {
                        strategyResult = RaschkeStrategySetup::HOLY_GRAIL_SELL;
                    }
                }

                if (strategyResult == RaschkeStrategySetup::NONE) {
                    const bool strongUptrend = (curClose > ema21 && curLow > ema21 + (kTickMultiplierPullback * kMesTickSize));
                    const bool strongDowntrend = (curClose < ema21 && curHigh < ema21 - (kTickMultiplierPullback * kMesTickSize));
                    if (strongUptrend || strongDowntrend) {
                        strategyResult = RaschkeStrategySetup::HOLY_GRAIL_CONTINUATION;
                    }
                }
            }

            // --- Double Repo Failure (trend continuation, highest priority) ---
            if (strategyResult == RaschkeStrategySetup::NONE) {
                for (int lookback = 1; lookback <= kDoubleRepoRetestLookback && strategyResult == RaschkeStrategySetup::NONE; ++lookback) {
                    const int retestBarIndex = cur - lookback;
                    if (retestBarIndex < 0) break;
                    for (int reversalLookback = 1; reversalLookback <= kDoubleRepoReversalLookback; ++reversalLookback) {
                        const int reversalBarIndex = retestBarIndex - reversalLookback;
                        if (reversalBarIndex < 0) break;

                        const bool reversalBarIsLow = IsSwingLowAt(reversalBarIndex, kDoubleRepoReversalLookback);
                        const float reversalLow = m_ts2PatternRing.LowAt(reversalBarIndex);
                        const bool retestChallengesLow = (m_ts2PatternRing.LowAt(retestBarIndex) <= reversalLow + (kTickMultiplierPullback * kMesTickSize));
                        const bool currentFailsToBreakRetestHigh = (curHigh < m_ts2PatternRing.HighAt(retestBarIndex));
                        const bool currentBreaksRetestLow = (curClose < m_ts2PatternRing.LowAt(retestBarIndex));
                        if (reversalBarIsLow && retestChallengesLow && currentFailsToBreakRetestHigh && currentBreaksRetestLow) {
                            strategyResult = RaschkeStrategySetup::DOUBLE_REPO_FAILURE;
                            break;
                        }

                        const bool reversalBarIsHigh = IsSwingHighAt(reversalBarIndex, kDoubleRepoReversalLookback);
                        const float reversalHigh = m_ts2PatternRing.HighAt(reversalBarIndex);
                        const bool retestChallengesHigh = (m_ts2PatternRing.HighAt(retestBarIndex) >= reversalHigh - (kTickMultiplierPullback * kMesTickSize));
                        const bool currentFailsToBreakRetestLow = (curLow > m_ts2PatternRing.LowAt(retestBarIndex));
                        const bool currentBreaksRetestHigh = (curClose > m_ts2PatternRing.HighAt(retestBarIndex));
                        if (reversalBarIsHigh && retestChallengesHigh && currentFailsToBreakRetestLow && currentBreaksRetestHigh) {
                            strategyResult = RaschkeStrategySetup::DOUBLE_REPO_FAILURE;
                            break;
                        }
                    }
                }
            }

            // --- Double Repo (reversal pattern) ---
            if (strategyResult == RaschkeStrategySetup::NONE) {
                for (int lookback = 1; lookback <= kDoubleRepoRetestLookback && strategyResult == RaschkeStrategySetup::NONE; ++lookback) {
                    const int retestBarIndex = cur - lookback;
                    if (retestBarIndex < 0) break;
                    for (int reversalLookback = 1; reversalLookback <= 3; ++reversalLookback) {
                        const int reversalBarIndex = retestBarIndex - reversalLookback;
                        if (reversalBarIndex < 0) break;

                        const bool reversalBarIsLow = IsSwingLowAt(reversalBarIndex, 3);
                        const float reversalLow = m_ts2PatternRing.LowAt(reversalBarIndex);
                        const bool retestChallengesLow = (m_ts2PatternRing.LowAt(retestBarIndex) <= reversalLow + (2.0f * kMesTickSize));
                        const bool currentBreaksRetestHigh = (curClose > m_ts2PatternRing.HighAt(retestBarIndex));
                        if (reversalBarIsLow && retestChallengesLow && currentBreaksRetestHigh) {
                            strategyResult = RaschkeStrategySetup::DOUBLE_REPO;
                            break;
                        }

                        const bool reversalBarIsHigh = IsSwingHighAt(reversalBarIndex, 3);
                        const float reversalHigh = m_ts2PatternRing.HighAt(reversalBarIndex);
                        const bool retestChallengesHigh = (m_ts2PatternRing.HighAt(retestBarIndex) >= reversalHigh - (2.0f * kMesTickSize));
                        const bool currentBreaksRetestLow = (curClose < m_ts2PatternRing.LowAt(retestBarIndex));
                        if (reversalBarIsHigh && retestChallengesHigh && currentBreaksRetestLow) {
                            strategyResult = RaschkeStrategySetup::DOUBLE_REPO;
                            break;
                        }
                    }
                }
            }

            // --- Bread and Butter (first pullback to short EMA in a trend) ---
            // FIXED (2026-09-16, Gemini-confirmed CLAUDE_BRIEF_147 Q3): `ema`
            // is `impulseEmaValue` (TS2's own Keltner EMA(13)), NOT a second
            // copy of ema21 -- the live wiring bug that made this permanently
            // unreachable in production is fixed at its own source
            // (src/TripleScreen2.cpp's setEma() call), and this port uses the
            // CORRECT short-EMA series from the start.
            if (strategyResult == RaschkeStrategySetup::NONE && cur >= 1 && hasPrevImpulseEmaForStrategy) {
                const float prevLow = m_ts2PatternRing.LowAt(cur - 1);
                const float prevHigh = m_ts2PatternRing.HighAt(cur - 1);

                const bool bullishEmaAlignment = (impulseEmaValue > ema21 && impulseEmaValue > prevImpulseEmaForStrategy);
                const bool hadBullishImpulse = (prevLow > impulseEmaValue);
                const bool pullbackToShortEma = (curLow <= impulseEmaValue + kMesTickSize);
                const bool doesntBreakLongEma = (curLow > ema21);
                const bool strongClose = (curClose >= impulseEmaValue - kMesTickSize);
                if (bullishEmaAlignment && hadBullishImpulse && pullbackToShortEma && doesntBreakLongEma && strongClose) {
                    strategyResult = RaschkeStrategySetup::BREAD_AND_BUTTER;
                }

                if (strategyResult == RaschkeStrategySetup::NONE) {
                    const bool bearishEmaAlignment = (impulseEmaValue < ema21 && impulseEmaValue < prevImpulseEmaForStrategy);
                    const bool hadBearishImpulse = (prevHigh < impulseEmaValue);
                    const bool rallyToShortEma = (curHigh >= impulseEmaValue - kMesTickSize);
                    const bool doesntBreakLongEmaUp = (curHigh < ema21);
                    const bool weakClose = (curClose <= impulseEmaValue + kMesTickSize);
                    if (bearishEmaAlignment && hadBearishImpulse && rallyToShortEma && doesntBreakLongEmaUp && weakClose) {
                        strategyResult = RaschkeStrategySetup::BREAD_AND_BUTTER;
                    }
                }
            }

            // --- Anti (stochastic trend continuation) ---
            if (strategyResult == RaschkeStrategySetup::NONE && m_antiKHist.size() >= 3 && cur >= 4) {
                const float kCurr = m_antiKHist[2], kPrev = m_antiKHist[1], kPrev2 = m_antiKHist[0];
                const float dCurr = m_antiDHist[2], dPrev = m_antiDHist[1], dPrev2 = m_antiDHist[0];

                const bool dSlopingUp = (dCurr > dPrev2);
                const bool kWasAboveD = (kPrev2 > dPrev2);
                const bool kCrossedBelowD = (kPrev < dPrev);
                const bool kHooksUp = (kCurr > kPrev && kPrev < kPrev2);
                const bool dStillUp = (dCurr >= dPrev);
                if (dSlopingUp && kWasAboveD && kCrossedBelowD && kHooksUp && dStillUp) {
                    const bool hadImpulse = (m_ts2PatternRing.HighAt(cur - 1) > m_ts2PatternRing.HighAt(cur - 3) ||
                                              m_ts2PatternRing.HighAt(cur - 2) > m_ts2PatternRing.HighAt(cur - 4));
                    if (hadImpulse) strategyResult = RaschkeStrategySetup::ANTI;
                }

                if (strategyResult == RaschkeStrategySetup::NONE) {
                    const bool dSlopingDown = (dCurr < dPrev2);
                    const bool kWasBelowD = (kPrev2 < dPrev2);
                    const bool kCrossedAboveD = (kPrev > dPrev);
                    const bool kHooksDown = (kCurr < kPrev && kPrev > kPrev2);
                    const bool dStillDown = (dCurr <= dPrev);
                    if (dSlopingDown && kWasBelowD && kCrossedAboveD && kHooksDown && dStillDown) {
                        const bool hadImpulse = (m_ts2PatternRing.LowAt(cur - 1) < m_ts2PatternRing.LowAt(cur - 3) ||
                                                  m_ts2PatternRing.LowAt(cur - 2) < m_ts2PatternRing.LowAt(cur - 4));
                        if (hadImpulse) strategyResult = RaschkeStrategySetup::ANTI;
                    }
                }
            }

            // --- Slingshot (MACD momentum + breakout) ---
            if (strategyResult == RaschkeStrategySetup::NONE && cur >= 1) {
                const float prevHigh = m_ts2PatternRing.HighAt(cur - 1);
                const float prevLow = m_ts2PatternRing.LowAt(cur - 1);
                if ((m_currentTs2StrategyMacd == MacdEnum::NEG_TICK_UP || m_currentTs2StrategyMacd == MacdEnum::SPRING) &&
                    curClose > prevHigh) {
                    strategyResult = RaschkeStrategySetup::SLINGSHOT;
                } else if ((m_currentTs2StrategyMacd == MacdEnum::POS_TICK_DOWN || m_currentTs2StrategyMacd == MacdEnum::FALL) &&
                           curClose < prevLow) {
                    strategyResult = RaschkeStrategySetup::SLINGSHOT;
                }
            }

            // --- Ghost (price/MACD divergence) ---
            // FIXED (2026-09-16, Gemini-confirmed CLAUDE_BRIEF_148): production's
            // real code checks IsSwingLow/High(*, sc.Index, 3) -- the CURRENT
            // bar, which structurally can never be confirmed as a swing point
            // at its own moment of closing (needs 3 bars that don't exist yet
            // in ANY real execution context), making GHOST permanently dead
            // code in production today. This port (and the matching live fix,
            // src/StudyHelperFunctions.cpp) instead anchors the "current" swing
            // at cur-SWING_LENGTH, the most recent bar that CAN legitimately be
            // confirmed right now (today's own bar closing is exactly what
            // newly confirms it). This is a genuine behavior change from
            // production's previously-dead pattern -- flagged for downstream
            // empirical validation via Task 12, not assumed safe.
            if (strategyResult == RaschkeStrategySetup::NONE && cur >= kGhostLookback) {
                const int currentSwingIndex = cur - kGhostSwingLength;
                const bool bullishMacdConfirm =
                    m_currentTs2StrategyMacd == MacdEnum::NEG_TICK_UP ||
                    m_currentTs2StrategyMacd == MacdEnum::SPRING ||
                    m_currentTs2StrategyMacd == MacdEnum::ZERO_FROM_BELOW ||
                    m_currentTs2StrategyMacd == MacdEnum::BULLISH_CROSS;
                const bool bearishMacdConfirm =
                    m_currentTs2StrategyMacd == MacdEnum::POS_TICK_DOWN ||
                    m_currentTs2StrategyMacd == MacdEnum::FALL ||
                    m_currentTs2StrategyMacd == MacdEnum::ZERO_FROM_ABOVE ||
                    m_currentTs2StrategyMacd == MacdEnum::BEARISH_CROSS;

                if (currentSwingIndex >= kGhostSwingLength) {
                    for (int lookback = kGhostSwingLength + 1; lookback <= kGhostLookback && strategyResult == RaschkeStrategySetup::NONE; ++lookback) {
                        const int priorSwingIndex = currentSwingIndex - lookback;
                        if (priorSwingIndex < kGhostSwingLength) break;

                        if (IsSwingLowAt(currentSwingIndex, kGhostSwingLength) && IsSwingLowAt(priorSwingIndex, kGhostSwingLength)) {
                            if (m_ts2PatternRing.LowAt(currentSwingIndex) < m_ts2PatternRing.LowAt(priorSwingIndex) && bullishMacdConfirm) {
                                strategyResult = RaschkeStrategySetup::GHOST;
                                break;
                            }
                        }
                        if (IsSwingHighAt(currentSwingIndex, kGhostSwingLength) && IsSwingHighAt(priorSwingIndex, kGhostSwingLength)) {
                            if (m_ts2PatternRing.HighAt(currentSwingIndex) > m_ts2PatternRing.HighAt(priorSwingIndex) && bearishMacdConfirm) {
                                strategyResult = RaschkeStrategySetup::GHOST;
                                break;
                            }
                        }
                    }
                }
            }

            // --- Two-B Reversal (failed 5-bar breakout -- PatternConstants::
            // TWO_B_LOOKBACK's real value is 5, not the 20 its own stale
            // in-source comments claim; Gemini-confirmed cosmetic-only,
            // CLAUDE_BRIEF_147 Q4 -- this port matches the REAL constant). ---
            if (strategyResult == RaschkeStrategySetup::NONE && cur >= kTwoBLookback) {
                float highestHighPrior = m_ts2PatternRing.HighAt(cur - 1);
                float lowestLowPrior = m_ts2PatternRing.LowAt(cur - 1);
                for (int i = 2; i <= kTwoBLookback; ++i) {
                    highestHighPrior = std::max(highestHighPrior, m_ts2PatternRing.HighAt(cur - i));
                    lowestLowPrior = std::min(lowestLowPrior, m_ts2PatternRing.LowAt(cur - i));
                }
                if (curHigh > highestHighPrior && curClose < highestHighPrior) {
                    strategyResult = RaschkeStrategySetup::TWO_B_REVERSAL;
                } else if (curLow < lowestLowPrior && curClose > lowestLowPrior) {
                    strategyResult = RaschkeStrategySetup::TWO_B_REVERSAL;
                }
            }

            // --- Whiplash (10-bar breakout + same-bar reversal) ---
            if (strategyResult == RaschkeStrategySetup::NONE && cur >= kWhiplashLookback) {
                const float barRange = curHigh - curLow;
                if (barRange > 0.0f) {
                    float lowestLow10 = m_ts2PatternRing.LowAt(cur - 1);
                    float highestHigh10 = m_ts2PatternRing.HighAt(cur - 1);
                    for (int i = 2; i <= kWhiplashLookback; ++i) {
                        lowestLow10 = std::min(lowestLow10, m_ts2PatternRing.LowAt(cur - i));
                        highestHigh10 = std::max(highestHigh10, m_ts2PatternRing.HighAt(cur - i));
                    }
                    const float closePositionInRange = (curClose - curLow) / barRange;
                    if (curLow < lowestLow10 && closePositionInRange > kWhiplashTopThreshold) {
                        strategyResult = RaschkeStrategySetup::WHIPLASH;
                    } else if (curHigh > highestHigh10 && closePositionInRange < kWhiplashBottomThreshold) {
                        strategyResult = RaschkeStrategySetup::WHIPLASH;
                    }
                }
            }

            // --- Three Bar Triangle (symmetric consolidation) ---
            if (strategyResult == RaschkeStrategySetup::NONE && cur >= 2) {
                if ((curHigh < m_ts2PatternRing.HighAt(cur - 1) && curHigh < m_ts2PatternRing.HighAt(cur - 2)) &&
                    (curLow > m_ts2PatternRing.LowAt(cur - 1) && curLow > m_ts2PatternRing.LowAt(cur - 2))) {
                    strategyResult = RaschkeStrategySetup::THREE_BAR_TRIANGLE;
                }
            }

            // --- Narrow Range patterns (NR4/NR7/IDNR4/NR4_NR7_VOLUME_SPIKE) ---
            if (strategyResult == RaschkeStrategySetup::NONE && cur >= 10) {
                const float curRange = curHigh - curLow;
                bool isNR4 = true;
                for (int i = 1; i <= 3; ++i) {
                    if (!(m_ts2PatternRing.RangeAt(cur - i) > curRange)) { isNR4 = false; break; }
                }
                bool isNR7 = true;
                for (int i = 1; i <= 6; ++i) {
                    if (!(m_ts2PatternRing.RangeAt(cur - i) > curRange)) { isNR7 = false; break; }
                }
                bool hasVolumeSpike = false;
                if (isNR4 || isNR7) {
                    double volumeSum = 0.0;
                    for (int i = 1; i <= 10; ++i) volumeSum += m_ts2PatternVolumes[static_cast<size_t>(cur - i)];
                    const double volumeAvg = volumeSum / 10.0;
                    double sqDiffSum = 0.0;
                    for (int i = 1; i <= 10; ++i) {
                        const double d = m_ts2PatternVolumes[static_cast<size_t>(cur - i)] - volumeAvg;
                        sqDiffSum += d * d;
                    }
                    const double volumeStdDev = std::sqrt(sqDiffSum / 10.0);
                    if (m_ts2PatternVolumes[static_cast<size_t>(cur)] > volumeAvg + (2.0 * volumeStdDev)) {
                        hasVolumeSpike = true;
                    }
                }
                const bool isInsideBar = (curHigh < m_ts2PatternRing.HighAt(cur - 1) && curLow > m_ts2PatternRing.LowAt(cur - 1));

                if ((isNR4 || isNR7) && hasVolumeSpike) {
                    strategyResult = RaschkeStrategySetup::NR4_NR7_VOLUME_SPIKE;
                } else if (isInsideBar && isNR4) {
                    strategyResult = RaschkeStrategySetup::IDNR4;
                } else if (isNR7) {
                    strategyResult = RaschkeStrategySetup::NR7;
                } else if (isNR4) {
                    strategyResult = RaschkeStrategySetup::NR4;
                }
            }

            // --- First Cross (MACD zero-line cross) ---
            if (strategyResult == RaschkeStrategySetup::NONE) {
                if (m_currentTs2StrategyMacd == MacdEnum::ZERO_FROM_BELOW || m_currentTs2StrategyMacd == MacdEnum::ZERO_FROM_ABOVE ||
                    m_currentTs2StrategyMacd == MacdEnum::BULLISH_CROSS || m_currentTs2StrategyMacd == MacdEnum::BEARISH_CROSS) {
                    strategyResult = RaschkeStrategySetup::FIRST_CROSS;
                }
            }

            if (strategyResult != m_lastRaschkeStrategy) {
                m_patternDirtyMask |= IndicatorKeyBit(IndicatorKey::RASCHKE_STRATEGY_SETUP);
            }
            m_lastRaschkeStrategy = strategyResult;
        }

        // No longer calls the dim math here (Task 6b) -- ComputeTs2LiveDims()
        // runs every tick from OnTick() instead. This callback only advances
        // the closed-bar history.
    }

    // Task 6b: called every tick. relative_range/log_scale_expansion_ratio/
    // fractal_dim/regime tenure are all genuinely per-tick-reactive in the
    // existing codebase (TripleScreen2.cpp's unconditional per-tick block;
    // ContextManager::SetWaveContext()'s own per-TICK, not per-bar, previous-
    // value commit for regime tenure specifically -- confirmed by reading its
    // real source, spec §2a).
    void ComputeTs2LiveDims() {
        // relative_range: (high-low)/ATR14 using the LIVE bar's own high/low;
        // ATR = 14-bar SMA of True Range where the most recent of the 14 is
        // the live bar's own still-evolving True Range (StudyHelperFunctions.cpp/
        // TripleScreen2.cpp:245's own formula).
        if (mdr::IsCandidateDim(MTS::Schema::Contract::kObsRelativeRange)) {
            float liveTrueRange;
            if (m_hasTs2PrevClose) {
                const float highLow = static_cast<float>(m_ts2LiveHigh - m_ts2LiveLow);
                const float highPrevClose = std::fabs(static_cast<float>(m_ts2LiveHigh) - m_ts2PrevClose);
                const float lowPrevClose = std::fabs(static_cast<float>(m_ts2LiveLow) - m_ts2PrevClose);
                liveTrueRange = std::max({highLow, highPrevClose, lowPrevClose});
            } else {
                liveTrueRange = static_cast<float>(m_ts2LiveHigh - m_ts2LiveLow);
            }
            float sumTr = liveTrueRange;
            size_t trCount = 1;
            for (size_t i = 0; i < m_ts2TrueRanges.size(); ++i) {
                sumTr += m_ts2TrueRanges[i];
                ++trCount;
            }
            const float atr = sumTr / static_cast<float>(trCount);

            const float relRange = cfc::ComputeRelativeRange(
                static_cast<float>(m_ts2LiveHigh), static_cast<float>(m_ts2LiveLow), atr, m_lastValidRelRange);
            m_lastValidRelRange = relRange;
            m_obs.mutate_relative_range(relRange);
        }

        const size_t closedSz = m_ts2Closes.size();

        // log_scale_expansion_ratio + bars_since_last_update: both need
        // kTs2ObsWindowN+1=21 total prices = 20 closed + 1 live, ported from
        // CalculateLogScaleExpansionRatio (StudyHelperFunctions.cpp:2854-2882)
        // and TripleScreen2.cpp:751-817/ContextManager::SetWaveContext's own
        // REGIME_CHANGE_THRESHOLD respectively.
        if (closedSz >= static_cast<size_t>(kTs2ObsWindowN)) {
            std::array<float, kTs2ObsWindowN + 1> prices{};
            for (size_t i = 0; i < static_cast<size_t>(kTs2ObsWindowN); ++i) {
                prices[i] = m_ts2Closes[closedSz - static_cast<size_t>(kTs2ObsWindowN) + i];
            }
            prices[static_cast<size_t>(kTs2ObsWindowN)] = static_cast<float>(m_ts2LiveClose);
            const size_t sz = static_cast<size_t>(kTs2ObsWindowN) + 1;

            // log_scale_expansion_ratio: log(recent BV rate / full BV rate),
            // same Barndorff-Nielsen & Shephard bipower-variation construct as
            // TS1's log_scale_ratio, over TS2's own 20-price window
            // (StudyHelperFunctions.cpp:2854-2882's own formula).
            if (mdr::IsCandidateDim(MTS::Schema::Contract::kObsLogScaleExpansionRatio)) {
                constexpr int kHalfWindow = kTs2ObsWindowN / 2;
                auto windowBv = [&prices, sz](int windowN) -> double {
                    double logReturns[kTs2ObsWindowN];
                    int count = 0;
                    for (int i = 0; i < windowN; ++i) {
                        const size_t idx = sz - static_cast<size_t>(windowN) + static_cast<size_t>(i);
                        if (idx == 0) continue;
                        const float p = prices[idx];
                        const float prevPrice = prices[idx - 1];
                        if (p > 0.0f && prevPrice > 0.0f) {
                            logReturns[count++] = std::log(static_cast<double>(p) / prevPrice);
                        }
                    }
                    return ComputeBipowerVariation(logReturns, count);
                };
                const double bvFull = windowBv(kTs2ObsWindowN);
                const double bvRecent = windowBv(kHalfWindow);
                const double bvFullRate = bvFull / kTs2ObsWindowN;
                const double bvRecentRate = bvRecent / kHalfWindow;
                const float logScaleExpansionRatio = cfc::ComputeBurstinessIndex(
                    bvRecentRate, bvFullRate, m_lastValidLogScaleExpansionRatio, -10.0f, 6.0f);
                m_lastValidLogScaleExpansionRatio = logScaleExpansionRatio;
                m_obs.mutate_log_scale_expansion_ratio(logScaleExpansionRatio);
            }

            double sumRet = 0.0;
            double retLogReturns[kTs2ObsWindowN];
            int retCount = 0;
            for (int i = 0; i < kTs2ObsWindowN; ++i) {
                const size_t idx = sz - static_cast<size_t>(kTs2ObsWindowN) + static_cast<size_t>(i);
                if (idx == 0) continue;
                const float price = prices[idx];
                const float prevPrice = prices[idx - 1];
                if (price > 0.0f && prevPrice > 0.0f) {
                    const double r = std::log(static_cast<double>(price) / prevPrice);
                    retLogReturns[retCount++] = r;
                    sumRet += r;
                }
            }
            float volatility = 0.0f;
            if (retCount > 1) {
                const double mean = sumRet / retCount;
                double sumSq = 0.0;
                for (int i = 0; i < retCount; ++i) {
                    const double d = retLogReturns[i] - mean;
                    sumSq += d * d;
                }
                volatility = static_cast<float>(std::sqrt(sumSq / (retCount - 1)));
            }

            const float netChange =
                std::fabs(prices[sz - 1] - prices[sz - 1 - static_cast<size_t>(kTs2ObsWindowN)]);
            float sumAbsChanges = 0.0f;
            for (int i = 0; i < kTs2ObsWindowN; ++i) {
                const size_t idx = sz - static_cast<size_t>(kTs2ObsWindowN) + static_cast<size_t>(i);
                sumAbsChanges += std::fabs(prices[idx] - prices[idx - 1]);
            }
            float efficiency = 0.0f;
            if (sumAbsChanges > 0.0001f) {
                efficiency = std::min(1.0f, netChange / sumAbsChanges);
            }

            constexpr float kRegimeChangeThreshold = 0.15f;  // ContextManager.h:469
            if (m_hasTs2PrevRegimeStats) {
                const float effChange =
                    std::fabs(efficiency - m_prevTs2Efficiency) / std::max(m_prevTs2Efficiency, 0.1f);
                const float volChange =
                    std::fabs(volatility - m_prevTs2Volatility) / std::max(m_prevTs2Volatility, 0.0001f);
                if (effChange > kRegimeChangeThreshold || volChange > kRegimeChangeThreshold) {
                    m_regimeTenure = 1.0f;
                } else {
                    m_regimeTenure += 1.0f;
                }
            } else {
                m_regimeTenure = 1.0f;
                m_hasTs2PrevRegimeStats = true;
            }
            m_prevTs2Efficiency = efficiency;
            m_prevTs2Volatility = volatility;
        }

        // fractal_dim: Sevcik (1998), needs kTs2FractalWindowN+1=401 total
        // prices = 400 closed + 1 live (TripleScreen2.cpp's own kFractalDimHmmWindow).
        if (mdr::IsCandidateDim(MTS::Schema::Contract::kObsFractalDim) &&
            closedSz >= static_cast<size_t>(kTs2FractalWindowN)) {
            std::array<float, kTs2FractalWindowN + 1> fractalPrices{};
            for (size_t i = 0; i < static_cast<size_t>(kTs2FractalWindowN); ++i) {
                fractalPrices[i] = m_ts2Closes[closedSz - static_cast<size_t>(kTs2FractalWindowN) + i];
            }
            fractalPrices[static_cast<size_t>(kTs2FractalWindowN)] = static_cast<float>(m_ts2LiveClose);
            const float dim = SevcikFractalDimension(fractalPrices.data(), kTs2FractalWindowN);
            if (std::isfinite(dim)) {
                m_lastValidFractalDim = dim;
                m_obs.mutate_fractal_dim(dim);
            } else if (m_lastValidFractalDim >= 1.0f) {
                m_obs.mutate_fractal_dim(m_lastValidFractalDim);
            }
            // else: leave m_obs.fractal_dim() at whatever it currently holds
            // (the 1.5f cold-start default set at construction, if no valid
            // reading has ever landed).
        }
    }

    // TS3 dims (amihud_illiquidity, liq_fragility, mean_rev_z, micro_asymmetry).
    // amihud_illiquidity/liq_fragility/micro_asymmetry are all documented
    // every-tick-reactive in the existing codebase (TripleScreen3.cpp's own
    // 2026-08-29 change, StudyHelperFunctions.cpp:2608-2656). mean_rev_z
    // moves to the same per-tick cadence in Task 6b (spec §2a correction --
    // it was wrongly assumed bar-close cadence in Task 5).
    static constexpr int kTs3ObsWindowN = 20;         // amihud/mean_rev_z window, same
                                                       // sanctioned fixed default as Task 4/4b
    static constexpr int kTs3LiqFragWindowN = 30;     // lfe::kWindow (LiquidityFragilityEngine.h)
    static constexpr size_t kTs3ClosedBarWindow = kTs3LiqFragWindowN;       // the larger of the two
    static constexpr size_t kTs3ClosedBarCapacity = kTs3ClosedBarWindow + 1;  // + headroom

    // Audit fix (ElderBreakout hurst): production's real ElderBreakout reads
    // `Subgraph_HurstExponent[sc.Index]`, computed by `UpdateObservationVectorSubgraphs`
    // -> `CalculateHurstExponent(sc)` -> the FIXED (100, minScale=8) DFA overload
    // (the adaptive `observation_window_n` it's passed is only consumed by
    // CalculatePathEfficiencySNR in that same function, NOT by Hurst -- confirmed
    // via direct read, not assumed). Since that call happens from WITHIN TS3
    // (src/TripleScreen3.cpp), it's a 100-bar DFA Hurst over TS3's OWN 15-min
    // closes -- the SAME formula as this engine's canonical TS1 hurst_exponent
    // dim, but a genuinely DIFFERENT price series (TS1 240m bars). Sanity-
    // checked with Gemini (read-only consult, lbrnet/logs/rc_gemini.log
    // CLAUDE_BRIEF_145/_REPLY) before implementing.
    static constexpr int kTs3HurstWindowN = 100;
    // 100 log-returns need 101 retained closes (matches ComputeTs1LiveDims's
    // own "closed+live = window+1 prices" pattern for the identical DFA(100,8)
    // formula) -- + 1 more for RingBuffer's own push-then-pop headroom.
    static constexpr size_t kTs3HurstRetained = static_cast<size_t>(kTs3HurstWindowN) + 1;
    static constexpr size_t kTs3HurstCapacity = kTs3HurstRetained + 1;

    void OnTs3BarClose(const tba::Bar& bar) {
        ++m_ts3BarsClosed;
        m_ts3JustClosedThisTick = true;

        const float high = static_cast<float>(bar.high);
        const float low = static_cast<float>(bar.low);
        const float close = static_cast<float>(bar.close);
        const float open = static_cast<float>(bar.open);
        const float volume = static_cast<float>(bar.volume);
        const float ohlcAvg = static_cast<float>((bar.open + bar.high + bar.low + bar.close) / 4.0);

        // Task 7 (spec §3 item 2): TurtleSoup's fourDayHigh/fourDayLow and
        // NR7's 7 prior ranges are read from the rings' PRE-update state
        // (production's own "PREVIOUS lookback window, excluding the
        // currently-closing bar" semantic — BoundedBarRing.h's own header
        // citation) -- must run before OnBarClose() pushes today's bar in.
        const bool turtleSoupWindowReady = m_turtleSoupRing.IsFull();
        const float fourDayHigh = turtleSoupWindowReady ? m_turtleSoupRing.HighestHigh() : 0.0f;
        const float fourDayLow = turtleSoupWindowReady ? m_turtleSoupRing.LowestLow() : 0.0f;
        // Task 9 (spec §3 item 3): retained for prev_four_bar_high/low (TrainingEvent's
        // real `m_shortTermExtremes.prevFourBarHigh/Low`, GetTickCompanionValues()) --
        // otherwise identical to the transient fourDayHigh/fourDayLow locals above.
        if (turtleSoupWindowReady) {
            m_prevFourBarHigh = fourDayHigh;
            m_prevFourBarLow = fourDayLow;
        }

        // Audit finding (2026-09-16, RASCHKE_TACTICAL_TRIGGER investigation):
        // TurtleSoup's real call site also enforces a "Street Smarts
        // separation" filter (src/TripleScreen3.cpp ~line 1179-1230) this
        // engine's own earlier Task 7 Step 1 wiring missed -- resets the
        // pattern to NONE if the prior window's own high/low (whichever
        // side the signal needs) occurred too recently (<4 bars ago) within
        // that SAME 20-bar prior window, EXCLUDING its own newest bar
        // (ring index kTurtleSoupLookbackBars-1). Ties favor the OLDER bar
        // (real loop's `>=`/`<=` semantics scanning newest-excluded-down-to-
        // oldest). Must also run on the PRE-update ring, same timing as
        // fourDayHigh/fourDayLow above.
        int priorHighRingIdx = kTurtleSoupLookbackBars - 2;
        int priorLowRingIdx = kTurtleSoupLookbackBars - 2;
        if (turtleSoupWindowReady) {
            float scanHigh = -std::numeric_limits<float>::max();
            float scanLow = std::numeric_limits<float>::max();
            for (int ringIdx = kTurtleSoupLookbackBars - 2; ringIdx >= 0; --ringIdx) {
                const float h = m_turtleSoupRing.HighAt(ringIdx);
                const float l = m_turtleSoupRing.LowAt(ringIdx);
                if (h >= scanHigh) { scanHigh = h; priorHighRingIdx = ringIdx; }
                if (l <= scanLow) { scanLow = l; priorLowRingIdx = ringIdx; }
            }
        }
        const int barsSincePriorHigh = kTurtleSoupLookbackBars - priorHighRingIdx;
        const int barsSincePriorLow = kTurtleSoupLookbackBars - priorLowRingIdx;

        float nr7PriorRanges[kNr7LookbackBars];
        const bool nr7WindowReady = m_nr7Ring.IsFull();
        if (nr7WindowReady) {
            for (int i = 0; i < kNr7LookbackBars; ++i) nr7PriorRanges[i] = m_nr7Ring.RangeAt(i);
        }

        // RASCHKE_TACTICAL_TRIGGER: RSI-swing's own 5-bar high/low lookback
        // and NR7-breakout's own 3-bar marker history must also be read in
        // their PRE-update state (same "prior window, excluding today"
        // timing as fourDayHigh/nr7PriorRanges above).
        const size_t rsiSwingCountPre = m_rsiSwingHighs.size();
        float rsiSwingHighsPre[kRsiSwingLookbackBars];
        float rsiSwingLowsPre[kRsiSwingLookbackBars];
        for (size_t i = 0; i < rsiSwingCountPre; ++i) {
            rsiSwingHighsPre[i] = m_rsiSwingHighs[i];
            rsiSwingLowsPre[i] = m_rsiSwingLows[i];
        }
        const size_t nr7MarkerCountPre = m_nr7MarkerFlags.size();
        uint8_t nr7MarkerFlagsPre[kNr7MarkerLookback];
        float nr7MarkerHighsPre[kNr7MarkerLookback];
        float nr7MarkerLowsPre[kNr7MarkerLookback];
        for (size_t i = 0; i < nr7MarkerCountPre; ++i) {
            nr7MarkerFlagsPre[i] = m_nr7MarkerFlags[i];
            nr7MarkerHighsPre[i] = m_nr7MarkerHighs[i];
            nr7MarkerLowsPre[i] = m_nr7MarkerLows[i];
        }
        const float prevTs3BarHighPre = m_prevTs3BarHigh;
        const float prevTs3BarLowPre = m_prevTs3BarLow;
        const bool hasPrevTs3BarPre = m_hasPrevTs3Bar;


        // Now advance the bounded pattern-detector history rings (Task 5's
        // wiring) -- after reading their pre-update state above.
        m_nr7Ring.OnBarClose(high, low);
        m_turtleSoupRing.OnBarClose(high, low);

        // RSI-swing ring: push today's own high/low, evicting the oldest if
        // the logical 5-bar window is now over capacity (RingBuffer's own
        // push_back-then-conditionally-pop_front convention).
        m_rsiSwingHighs.push_back(high);
        m_rsiSwingLows.push_back(low);
        if (m_rsiSwingHighs.size() > static_cast<size_t>(kRsiSwingLookbackBars)) {
            m_rsiSwingHighs.pop_front();
            m_rsiSwingLows.pop_front();
        }

        // Task 7: shared per-bar compute engines, all fed exactly once per
        // completed TS3 bar (matches production's own once-per-bar cadence
        // for ATR/RSI/Stochastic/Keltner/AvgVolume on this timeframe).
        const float atr10 = m_atr10.OnBar(high, low, close);
        const float rsi3 = m_rsi3.OnClose(close);
        const float rsi10 = m_rsi10.OnClose(close);
        m_stoch.OnBar(high, low, close);
        const float stochFastK = m_stoch.RawK();  // Subgraph_StochK = Arrays[0] = FastK = RawK, not FastD
        const float keltnerCenter = m_ts3KeltnerEma10.OnValue(ohlcAvg);
        m_avgVolume14.Push(volume);
        const float avgVolume14 = m_avgVolume14.Value();

        // --- Kangaroo Tail (no cross-bar history needed) ---
        {
            float tailToBodyRatio = 0.0f, tailToATR = 0.0f, closePosition = 0.0f, quality = 0.0f;
            const KangarooTailEnum result = DetectKangarooTail(
                open, high, low, close, atr10,
                tailToBodyRatio, tailToATR, closePosition, quality);
            if (result != m_lastKangarooTail) {
                m_patternDirtyMask |= IndicatorKeyBit(IndicatorKey::KANGAROO_TAIL);
            }
            m_lastKangarooTail = result;
            m_lastKangarooTailQuality = quality;
        }

        // --- Turtle Soup (needs the PRE-update 20-bar prior window) ---
        if (turtleSoupWindowReady) {
            float penetrationDistance = 0.0f, closeDistance = 0.0f, closePosition = 0.0f, quality = 0.0f;
            TurtleSoupEnum result = DetectTurtleSoup(
                open, high, low, close, fourDayHigh, fourDayLow, atr10,
                penetrationDistance, closeDistance, closePosition, quality);
            // Street Smarts separation filter (see priorHighRingIdx/priorLowRingIdx's
            // own comment above) -- TURTLE_SOUP_MIN_SEPARATION=4 (src/TripleScreen3.cpp).
            constexpr int kTurtleSoupMinSeparation = 4;
            const bool isBullish = (result == TurtleSoupEnum::BULLISH_WEAK ||
                                    result == TurtleSoupEnum::BULLISH_STRONG ||
                                    result == TurtleSoupEnum::BULLISH_EXTREME);
            const bool isBearish = (result == TurtleSoupEnum::BEARISH_WEAK ||
                                    result == TurtleSoupEnum::BEARISH_STRONG ||
                                    result == TurtleSoupEnum::BEARISH_EXTREME);
            if (isBullish && barsSincePriorLow < kTurtleSoupMinSeparation) result = TurtleSoupEnum::NONE;
            if (isBearish && barsSincePriorHigh < kTurtleSoupMinSeparation) result = TurtleSoupEnum::NONE;
            if (result != m_lastTurtleSoup) {
                m_patternDirtyMask |= IndicatorKeyBit(IndicatorKey::TURTLE_SOUP);
            }
            m_lastTurtleSoup = result;
            // Raw quality, from BEFORE the separation-filter override above -- matches
            // production's own "Forward RAW quality score (Physics only)" unconditional
            // SetMetrics() call (src/TripleScreen3.cpp), not gated on the filtered enum.
            m_lastTurtleSoupQuality = quality;
        }

        // --- Momentum Pinball (needs previous-bar RSI3/RSI10 + cross-
        // timeframe INTERM_IMP snapshot) ---
        if (m_hasPrevRsi) {
            float rsiDelta = 0.0f, stochDepth = 0.0f, volumeSpike = 0.0f, quality = 0.0f;
            bool impulseJustChanged = false;
            const MomentumPinballEnum result = DetectMomentumPinball(
                rsi3, rsi10, m_prevRsi3, m_prevRsi10,
                stochFastK, m_currentImpulseColor, m_ts3PrevImpulseColorSnapshot,
                static_cast<double>(volume), static_cast<double>(avgVolume14),
                rsiDelta, stochDepth, impulseJustChanged, volumeSpike, quality);
            if (result != m_lastMomentumPinball) {
                m_patternDirtyMask |= IndicatorKeyBit(IndicatorKey::MOMENTUM_PINBALL);
            }
            m_lastMomentumPinball = result;
            m_lastMomentumPinballQuality = quality;
        }
        m_prevRsi3 = rsi3;
        m_prevRsi10 = rsi10;
        m_hasPrevRsi = true;
        m_ts3PrevImpulseColorSnapshot = m_currentImpulseColor;

        // --- Elder Breakout (Keltner bands + 5-bar consolidation history,
        // which DOES include today's own just-computed band/atr/close --
        // production's own loop bound is `idx <= sc.Index`, TripleScreen3.cpp) ---
        {
            const float multiplier = 2.0f;  // Input_BandMultiplier's real default (Elder's standard)
            const float topBand = keltnerCenter + atr10 * multiplier;
            const float bottomBand = keltnerCenter - atr10 * multiplier;

            m_ts3ElderCloseHist.push_back(close);
            if (m_ts3ElderCloseHist.size() > kElderConsolidationLookback) m_ts3ElderCloseHist.pop_front();
            m_ts3ElderTopBandHist.push_back(topBand);
            if (m_ts3ElderTopBandHist.size() > kElderConsolidationLookback) m_ts3ElderTopBandHist.pop_front();
            m_ts3ElderBottomBandHist.push_back(bottomBand);
            if (m_ts3ElderBottomBandHist.size() > kElderConsolidationLookback) m_ts3ElderBottomBandHist.pop_front();
            m_ts3ElderAtrHist.push_back(atr10);
            if (m_ts3ElderAtrHist.size() > kElderConsolidationLookback) m_ts3ElderAtrHist.pop_front();

            int nearUpperCount = 0, nearLowerCount = 0;
            const size_t histSz = m_ts3ElderCloseHist.size();
            for (size_t i = 0; i < histSz; ++i) {
                const float priorAtr = m_ts3ElderAtrHist[i];
                if (priorAtr <= 0.0f) continue;
                if (std::fabs(m_ts3ElderCloseHist[i] - m_ts3ElderTopBandHist[i]) <= priorAtr) ++nearUpperCount;
                if (std::fabs(m_ts3ElderCloseHist[i] - m_ts3ElderBottomBandHist[i]) <= priorAtr) ++nearLowerCount;
            }
            int consolidationBars = 0;
            if (nearUpperCount >= kElderMinConsolidationBars) consolidationBars = nearUpperCount;
            else if (nearLowerCount >= kElderMinConsolidationBars) consolidationBars = nearLowerCount;

            const bool isGap = (open > topBand || open < bottomBand);
            // FIXED (audit finding, sanity-checked with Gemini -- see
            // kTs3HurstWindowN's own comment): production's real ElderBreakout
            // hurst is a TS3-LOCAL 100-bar DFA(minScale=8) Hurst over TS3's OWN
            // closes, NOT the canonical TS1-based hurst_exponent dim. Push
            // THIS bar's own close first (production's real window includes
            // the current/just-closed bar as its newest point).
            m_ts3HurstCloses.push_back(close);
            if (m_ts3HurstCloses.size() > kTs3HurstRetained) m_ts3HurstCloses.pop_front();
            float currentHurst = 0.5f;  // matches TripleScreen3.cpp's own neutral fallback
            if (m_ts3HurstCloses.size() >= kTs3HurstRetained) {
                std::array<float, kTs3HurstWindowN> hurstLogReturns{};
                const size_t hSz = m_ts3HurstCloses.size();
                for (int i = 0; i < kTs3HurstWindowN; ++i) {
                    const size_t idx = hSz - kTs3HurstRetained + static_cast<size_t>(i) + 1;
                    const float p = m_ts3HurstCloses[idx];
                    const float prevP = m_ts3HurstCloses[idx - 1];
                    hurstLogReturns[static_cast<size_t>(i)] = (p > 0.0f && prevP > 0.0f)
                        ? std::log(p / prevP) : 0.0f;
                }
                const float hurst = DfaHurstExponent(hurstLogReturns.data(), kTs3HurstWindowN, /*minScale=*/8);
                if (std::isfinite(hurst)) currentHurst = hurst;
            }
            if (currentHurst <= 0.0f) currentHurst = 0.5f;
            m_lastElderBreakoutTs3Hurst = currentHurst;

            if (atr10 > 0.0f && topBand > 0.0f && bottomBand > 0.0f) {
                float breakoutDistance = 0.0f, hurstOut = 0.0f, volumeSpike = 0.0f, quality = 0.0f;
                int consolidationBarsOut = 0;
                bool isGapOut = false;
                const ElderBreakoutEnum result = DetectElderBreakout(
                    close, topBand, bottomBand, atr10, currentHurst,
                    static_cast<double>(volume), static_cast<double>(avgVolume14),
                    consolidationBars, isGap,
                    breakoutDistance, hurstOut, volumeSpike, consolidationBarsOut, isGapOut, quality);
                if (result != m_lastElderBreakout) {
                    m_patternDirtyMask |= IndicatorKeyBit(IndicatorKey::ELDER_BREAKOUT);
                }
                m_lastElderBreakout = result;
                m_lastElderBreakoutQuality = quality;
            }
        }

        // --- NR7 (needs the PRE-update 7-bar prior window) ---
        if (nr7WindowReady) {
            const float currentRange = high - low;
            float avg7BarRange = 0.0f, rangePercentile = 0.0f, volumeSpike = 0.0f, quality = 0.0f;
            const bool isChaosClimateBlocked = false;  // climate gate not ported to the replay tool (Task 12 scope)
            const NR7Enum result = DetectNR7(
                currentRange, nr7PriorRanges, kNr7LookbackBars, isChaosClimateBlocked,
                volume, avgVolume14,
                avg7BarRange, rangePercentile, volumeSpike, quality);
            if (result != m_lastNr7) {
                m_patternDirtyMask |= IndicatorKeyBit(IndicatorKey::NR7);
            }
            m_lastNr7 = result;
            m_lastNr7Quality = quality;
        }

        // --- RASCHKE_TACTICAL_TRIGGER (5-writer, last-write-wins field --
        // see IndicatorComputations.h's own RaschkeTacticalTrigger doc
        // comment for the full precedence account). Applied in production's
        // real ascending line-number order (src/TripleScreen3.cpp): base
        // DetectRaschkeTacticalTrigger-equivalent (lowest precedence) ->
        // KangarooTail's gate -> ElderBreakout's gate -> TurtleSoup's gate ->
        // NR7-breakout's gate (highest precedence). Each later stage can
        // overwrite an earlier one's result -- only the LAST one whose own
        // condition is true wins. ---
        {
            RaschkeTacticalTrigger tactical = m_lastRaschkeTactical;
            bool tacticalDirty = false;
            const auto setTactical = [&](RaschkeTacticalTrigger v) {
                if (v != tactical) tacticalDirty = true;
                tactical = v;
            };

            if (m_ts3BarsClosed >= kTacticalMinLookback) {
                // Base classifier starts fresh each bar (production's own
                // DetectRaschkeTacticalTrigger returns NONE by default, then
                // the 4 later writers may or may not overwrite it).
                RaschkeTacticalTrigger base = RaschkeTacticalTrigger::NONE;

                // RSI Failure Swing (Divergence): highest/lowest across
                // {current bar} UNION {prior kRsiSwingLookbackBars bars}.
                float highestPrice = high;
                float lowestPrice = low;
                for (size_t i = 0; i < rsiSwingCountPre; ++i) {
                    highestPrice = std::max(highestPrice, rsiSwingHighsPre[i]);
                    lowestPrice = std::min(lowestPrice, rsiSwingLowsPre[i]);
                }
                if (high >= highestPrice && rsi10 < kRsiOverbought && rsi10 < rsi3) {
                    base = RaschkeTacticalTrigger::RSI_FAILURE_SWING_SELL;
                }
                if (low <= lowestPrice && rsi10 > kRsiOversold && rsi10 > rsi3) {
                    base = RaschkeTacticalTrigger::RSI_FAILURE_SWING_BUY;
                }

                // Stochastic Pop (Hook Reversal).
                if (stochFastK > kStochOversold && stochFastK < (kStochOversold + 10.0f) &&
                    rsi3 > kRsiOversold && rsi3 > rsi10) {
                    base = RaschkeTacticalTrigger::STOCHASTIC_POP_BUY;
                }
                if (stochFastK < kStochOverbought && stochFastK > (kStochOverbought - 10.0f) &&
                    rsi3 < kRsiOverbought && rsi3 < rsi10) {
                    base = RaschkeTacticalTrigger::STOCHASTIC_POP_SELL;
                }

                // ITR (Initial Trading Range): per-ET-calendar-day 09:30-10:30
                // high/low tracking (production's own barTime.GetDate() reset,
                // a plain calendar date, not a trading-day boundary).
                int hour = 0, minute = 0;
                int64_t dayNumber = 0;
                GetEasternTimeParts(bar.closeTimeUs, hour, minute, dayNumber);
                const bool isOpeningHour = (hour == 9 && minute >= 30) || (hour == 10 && minute < 30);
                const bool isAfterOpeningHour = (hour > 10) || (hour == 10 && minute >= 30);

                if (!m_hasItrDay || dayNumber != m_itrDayNumber) {
                    m_itrDayNumber = dayNumber;
                    m_hasItrDay = true;
                    m_itrHigh = high;
                    m_itrLow = low;
                    m_itrEstablished = false;
                    m_hadBreakoutAbove = false;
                    m_hadBreakdownBelow = false;
                }
                if (isOpeningHour) {
                    if (high > m_itrHigh) m_itrHigh = high;
                    if (low < m_itrLow) m_itrLow = low;
                    m_itrEstablished = false;
                }
                if (isAfterOpeningHour && !m_itrEstablished) {
                    m_itrEstablished = true;
                }

                if (m_itrEstablished && m_itrHigh > m_itrLow && hasPrevTs3BarPre) {
                    // Volume confirmation over the prior kVolumeAvgLookback
                    // CLOSED bars (m_ts3ClosedVolumes's own PRE-update state --
                    // its own push_back doesn't run until this function's end).
                    bool highVolume = false;
                    const size_t volSz = m_ts3ClosedVolumes.size();
                    if (volSz >= static_cast<size_t>(kVolumeAvgLookback)) {
                        double volumeSum = 0.0;
                        for (size_t i = 0; i < static_cast<size_t>(kVolumeAvgLookback); ++i) {
                            volumeSum += m_ts3ClosedVolumes[volSz - 1 - i];
                        }
                        const double volumeAvg = volumeSum / static_cast<double>(kVolumeAvgLookback);
                        highVolume = (static_cast<double>(volume) > volumeAvg * kVolumeBreakoutMultiplier);
                    }

                    // Standard MES exchange tick size (0.25 index points) --
                    // no per-instrument config was found in this repo to read
                    // instead; matches production's own sc.TickSize read.
                    if (high > m_itrHigh + kMesTickSize && !m_hadBreakoutAbove) {
                        m_hadBreakoutAbove = true;
                        if (highVolume) base = RaschkeTacticalTrigger::ITR_BREAKOUT_BUY;
                    }
                    if (low < m_itrLow - kMesTickSize && !m_hadBreakdownBelow) {
                        m_hadBreakdownBelow = true;
                        if (highVolume) base = RaschkeTacticalTrigger::ITR_BREAKOUT_SELL;
                    }
                    if (m_hadBreakdownBelow && prevTs3BarLowPre < m_itrLow && close > m_itrLow) {
                        base = RaschkeTacticalTrigger::ITR_FADE_BUY;
                    }
                    if (m_hadBreakoutAbove && prevTs3BarHighPre > m_itrHigh && close < m_itrHigh) {
                        base = RaschkeTacticalTrigger::ITR_FADE_SELL;
                    }
                }

                setTactical(base);

                // KangarooTail's own gate (2nd writer, src/TripleScreen3.cpp ~885).
                const float srThreshold = kSupportResistanceThreshold * atr10;
                const bool atSupportLevel = (m_prevDayLow > 0.0f && std::abs(low - m_prevDayLow) <= srThreshold);
                const bool atResistanceLevel = (m_prevDayHigh > 0.0f && std::abs(high - m_prevDayHigh) <= srThreshold);
                if (m_lastKangarooTailQuality >= kKangarooQualityThreshold) {
                    if ((m_lastKangarooTail == KangarooTailEnum::BULLISH_STRONG ||
                         m_lastKangarooTail == KangarooTailEnum::BULLISH_EXTREME) && atSupportLevel) {
                        setTactical(RaschkeTacticalTrigger::KANGAROO_TAIL_BUY);
                    } else if ((m_lastKangarooTail == KangarooTailEnum::BEARISH_STRONG ||
                                m_lastKangarooTail == KangarooTailEnum::BEARISH_EXTREME) && atResistanceLevel) {
                        setTactical(RaschkeTacticalTrigger::KANGAROO_TAIL_SELL);
                    }
                }

                // ElderBreakout's own gate (3rd writer, ~1130) -- STRONG/EXTREME
                // only, no support/resistance condition.
                if (m_lastElderBreakout == ElderBreakoutEnum::BULLISH_STRONG ||
                    m_lastElderBreakout == ElderBreakoutEnum::BULLISH_EXTREME) {
                    setTactical(RaschkeTacticalTrigger::ELDER_BREAKOUT_BUY);
                } else if (m_lastElderBreakout == ElderBreakoutEnum::BEARISH_STRONG ||
                           m_lastElderBreakout == ElderBreakoutEnum::BEARISH_EXTREME) {
                    setTactical(RaschkeTacticalTrigger::ELDER_BREAKOUT_SELL);
                }

                // TurtleSoup's own gate (4th writer, ~1310) -- STRONG/EXTREME
                // AND at the corresponding daily extreme (already-separation-
                // filtered m_lastTurtleSoup from this same bar).
                if (turtleSoupWindowReady) {
                    const float dailyThreshold = 0.5f * atr10;
                    const bool atDailyHighNow = (m_prevDayHigh > 0.0f && std::abs(fourDayHigh - m_prevDayHigh) <= dailyThreshold);
                    const bool atDailyLowNow = (m_prevDayLow > 0.0f && std::abs(fourDayLow - m_prevDayLow) <= dailyThreshold);
                    if ((m_lastTurtleSoup == TurtleSoupEnum::BULLISH_STRONG ||
                         m_lastTurtleSoup == TurtleSoupEnum::BULLISH_EXTREME) && atDailyLowNow) {
                        setTactical(RaschkeTacticalTrigger::TURTLE_SOUP_BUY);
                    } else if ((m_lastTurtleSoup == TurtleSoupEnum::BEARISH_STRONG ||
                                m_lastTurtleSoup == TurtleSoupEnum::BEARISH_EXTREME) && atDailyHighNow) {
                        setTactical(RaschkeTacticalTrigger::TURTLE_SOUP_SELL);
                    }
                }

                // NR7-breakout's own gate (5th writer, highest precedence,
                // ~1490) -- was any of the last 3 bars marked NR7, and does
                // today's close break above/below THAT bar's own high/low
                // (first/most-recent match wins, production's own `break`).
                for (int lookback = 1; lookback <= kNr7MarkerLookback; ++lookback) {
                    const size_t idxFromNewest = static_cast<size_t>(lookback);
                    if (idxFromNewest > nr7MarkerCountPre) continue;
                    const size_t idx = nr7MarkerCountPre - idxFromNewest;
                    if (nr7MarkerFlagsPre[idx] == 0) continue;
                    const float nr7High = nr7MarkerHighsPre[idx];
                    const float nr7Low = nr7MarkerLowsPre[idx];
                    if (close > nr7High) {
                        setTactical(RaschkeTacticalTrigger::NR7_BREAKOUT_BUY);
                        break;
                    } else if (close < nr7Low) {
                        setTactical(RaschkeTacticalTrigger::NR7_BREAKOUT_SELL);
                        break;
                    }
                }
            }

            if (tacticalDirty) {
                m_patternDirtyMask |= IndicatorKeyBit(IndicatorKey::RASCHKE_TACTICAL_TRIGGER);
            }
            m_lastRaschkeTactical = tactical;

            // Advance the NR7 marker ring (this bar's own NR7 status) and the
            // immediately-prior-bar high/low, for FUTURE bars' own checks above.
            m_nr7MarkerFlags.push_back(m_lastNr7 != NR7Enum::NONE ? 1 : 0);
            m_nr7MarkerHighs.push_back(high);
            m_nr7MarkerLows.push_back(low);
            if (m_nr7MarkerFlags.size() > static_cast<size_t>(kNr7MarkerLookback)) {
                m_nr7MarkerFlags.pop_front();
                m_nr7MarkerHighs.pop_front();
                m_nr7MarkerLows.pop_front();
            }
            m_prevTs3BarHigh = high;
            m_prevTs3BarLow = low;
            m_hasPrevTs3Bar = true;
        }

        {
            const ATRProximityEnum result = ClassifyATRProximity(high, low, close, atr10);
            if (result != m_lastAtrProximity) {
                m_patternDirtyMask |= IndicatorKeyBit(IndicatorKey::ATR_PROXIMITY);
            }
            m_lastAtrProximity = result;
        }

        // --- Structure Test (needs prevDayHigh/prevDayLow -- the PREVIOUS
        // COMPLETED TRADING DAY's own high/low, NOT the immediately-prior
        // 15-min bar's own high/low, an assumption this same implementation
        // pass initially got wrong before an audit caught it against the
        // real call site, src/TripleScreen3.cpp:614 -- plus the 20-bar
        // lookback window INCLUDING today, via m_turtleSoupRing's now-POST-
        // update state -- unlike TurtleSoup's own "previous window" read
        // earlier in this function, sc.Highest/Lowest(...,20) is inclusive
        // of the current bar). ---
        if (m_hasPrevDayRange) {
            const float lookbackHigh = m_turtleSoupRing.HighestHigh();
            const float lookbackLow = m_turtleSoupRing.LowestLow();
            const StructureTest result = ClassifyStructure(
                high, low, close, m_prevDayHigh, m_prevDayLow,
                static_cast<double>(atr10), lookbackHigh, lookbackLow);
            if (result != m_lastStructureTest) {
                m_patternDirtyMask |= IndicatorKeyBit(IndicatorKey::STRUCTURE_TEST);
            }
            m_lastStructureTest = result;
        }

        // --- Volume Signal (session-aware robust log-volume z-score +
        // order-flow imbalance, src/StudyHelperFunctions.cpp/Indicator.h's
        // VolumeIndicator, already ACSIL-free -- ComputeVolumeBarSample/
        // ComputeVolumeClassification reused directly, no extraction needed).
        // Production samples the bar-magnitude baseline once per closed bar
        // and self-classifies per-tick using the LIVE forming bar's own
        // bid/ask volume; this engine's own bar-close-only cadence
        // (established throughout Task 7) uses the just-closed bar's own
        // aggregated bid/ask volume as its per-bar proxy for that per-tick read. ---
        {
            const bool isRth = IsRthSession(bar.closeTimeUs);
            ComputeVolumeBarSample(m_volumeState, static_cast<float>(bar.volume), isRth);
            const VolumeClassification vc = ComputeVolumeClassification(
                m_volumeState.volumeZScore, static_cast<float>(bar.bidVolume), static_cast<float>(bar.askVolume));
            if (vc.signal != m_lastVolumeSignal) {
                m_patternDirtyMask |= IndicatorKeyBit(IndicatorKey::VOLUME_SIGNAL);
            }
            m_lastVolumeSignal = vc.signal;
        }

        // --- Daily Bias (Raschke 80%-Rule/gap-analysis classification,
        // include/DailyBiasEngine.h's already-pure dbe::ComputeDailyBias() --
        // zero extraction needed). Real call site (src/TripleScreen3.cpp)
        // passes the SAME TS3-local Hurst as ElderBreakout
        // (m_lastElderBreakoutTs3Hurst) and its own real prevDayHigh/
        // prevDayLow (m_dailyBars). `entropy`/PathEfficiencySNR is NOT used
        // at all by the real CalculateDailyBias() wrapper (its own
        // `float /*entropy*/` commented-out parameter name, confirmed by
        // direct read) -- not a gap, the real function simply ignores it.
        // valueAreaLow/valueAreaHigh: this engine has no real Volume Profile
        // Value Area data -- passing 0.0f for both is NOT an invented
        // approximation, it is dbe::ComputeDailyBias()'s own documented,
        // designed sentinel for "fall back to the naive 15%/85% range-split
        // proxy this engine replaces" (DailyBiasEngine.h's own header
        // comment) -- a real, intentional fallback path, not a shortcut. ---
        {
            const dbe::Bias result = dbe::ComputeDailyBias({
                close, m_prevDayHigh, m_prevDayLow, m_lastElderBreakoutTs3Hurst,
                0.0f, 0.0f});
            if (result != m_lastDailyBias) {
                m_patternDirtyMask |= IndicatorKeyBit(IndicatorKey::DAILY_BIAS);
            }
            m_lastDailyBias = result;
        }

        m_ts3ClosedCloses.push_back(static_cast<float>(bar.close));
        if (m_ts3ClosedCloses.size() > kTs3ClosedBarWindow) m_ts3ClosedCloses.pop_front();
        m_ts3ClosedVolumes.push_back(static_cast<float>(bar.volume));
        if (m_ts3ClosedVolumes.size() > kTs3ClosedBarWindow) m_ts3ClosedVolumes.pop_front();
        m_ts3ClosedRanges.push_back(static_cast<float>(bar.high - bar.low));
        if (m_ts3ClosedRanges.size() > kTs3ClosedBarWindow) m_ts3ClosedRanges.pop_front();
        // No longer computes mean_rev_z here (Task 6b) -- ComputeTs3LiveDims()
        // runs every tick from OnTick() instead. This callback only advances
        // the closed-bar history.
    }

    void ComputeTs3LiveDims() {
        // micro_asymmetry: order-flow asymmetry from the live bar's own
        // cumulative ask/bid volume (TripleScreen3.cpp's per-tick cadence,
        // ofae::ComputeMicroAsymmetry).
        if (mdr::IsCandidateDim(MTS::Schema::Contract::kObsMicroAsymmetry)) {
            const float askVol = m_hasTs3LiveBar ? static_cast<float>(m_ts3LiveAskVolume) : 0.0f;
            const float bidVol = m_hasTs3LiveBar ? static_cast<float>(m_ts3LiveBidVolume) : 0.0f;
            const float microAsymmetry =
                ofae::ComputeMicroAsymmetry(askVol, bidVol, m_lastValidMicroAsymmetry);
            m_lastValidMicroAsymmetry = microAsymmetry;
            m_obs.mutate_micro_asymmetry(microAsymmetry);
        }

        // amihud_illiquidity: historical closed-bar sum + live still-forming-
        // bar term (StudyHelperFunctions.cpp:2608-2656's own formula).
        if (mdr::IsCandidateDim(MTS::Schema::Contract::kObsAmihudIlliquidity)) {
            const size_t sz = m_ts3ClosedCloses.size();
            if (sz >= static_cast<size_t>(kTs3ObsWindowN + 1)) {
                double sumLogRatio = 0.0;
                int count = 0;
                constexpr double kRatioEps = 1e-20;
                for (int i = 0; i < kTs3ObsWindowN; ++i) {
                    const size_t idx = sz - static_cast<size_t>(kTs3ObsWindowN) + static_cast<size_t>(i);
                    if (idx == 0) continue;
                    const double price = m_ts3ClosedCloses[idx];
                    const double prevPrice = m_ts3ClosedCloses[idx - 1];
                    const double vol = m_ts3ClosedVolumes[idx];
                    if (vol < 1.0 || price <= 0.0 || prevPrice <= 0.0) continue;
                    const double dollarVol = price * vol;
                    if (dollarVol < 1.0) continue;
                    const double logRet = std::fabs(std::log(price / prevPrice));
                    const double ratio = logRet / std::sqrt(dollarVol);
                    sumLogRatio += std::log(ratio + kRatioEps);
                    ++count;
                }
                constexpr double kLiveBarMinVolume = 50.0;
                if (m_hasTs3LiveBar && static_cast<double>(m_ts3LiveVolume) >= kLiveBarMinVolume) {
                    const double livePrice = m_ts3LiveClose;
                    const double prevClose = m_ts3ClosedCloses.back();
                    if (livePrice > 0.0 && prevClose > 0.0) {
                        const double dollarVol = livePrice * static_cast<double>(m_ts3LiveVolume);
                        if (dollarVol >= 1.0) {
                            const double logRet = std::fabs(std::log(livePrice / prevClose));
                            const double ratio = logRet / std::sqrt(dollarVol);
                            sumLogRatio += std::log(ratio + kRatioEps);
                            ++count;
                        }
                    }
                }
                const float amihud = cfc::ComputeAmihudIlliquidity(sumLogRatio, count, m_lastValidAmihud);
                m_lastValidAmihud = amihud;
                m_obs.mutate_amihud_illiquidity(amihud);
            }
        }

        // liq_fragility: F_raw = eta_t/ScaleRef_W, median-based, 30-closed-bar
        // window + live still-forming bar (StudyHelperFunctions.cpp:2608-2645).
        if (mdr::IsCandidateDim(MTS::Schema::Contract::kObsLiqFragility)) {
            const size_t sz = m_ts3ClosedRanges.size();
            if (sz >= static_cast<size_t>(kTs3LiqFragWindowN)) {
                std::array<float, kTs3LiqFragWindowN> rangeWindow{};
                std::array<float, kTs3LiqFragWindowN> sqrtVolWindow{};
                for (int i = 0; i < kTs3LiqFragWindowN; ++i) {
                    const size_t idx = sz - static_cast<size_t>(kTs3LiqFragWindowN) + static_cast<size_t>(i);
                    rangeWindow[static_cast<size_t>(i)] = m_ts3ClosedRanges[idx];
                    sqrtVolWindow[static_cast<size_t>(i)] = std::sqrt(std::max(m_ts3ClosedVolumes[idx], 0.0f));
                }
                const float liveRange = m_hasTs3LiveBar
                    ? static_cast<float>(m_ts3LiveHigh - m_ts3LiveLow) : 0.0f;
                const float liveVol = m_hasTs3LiveBar ? static_cast<float>(m_ts3LiveVolume) : 0.0f;
                const float fragility = lfe::ComputeLiquidityFragility(
                    rangeWindow.data(), sqrtVolWindow.data(), liveRange, liveVol, m_lastValidLiqFragility);
                m_lastValidLiqFragility = fragility;
                m_obs.mutate_liq_fragility(fragility);
            }
        }

        // mean_rev_z: median/MAD price-stretch z-score + lag-1 autocorrelation,
        // needs kTs3ObsWindowN=20 total prices = 19 closed + 1 live (Task 6b --
        // spec §2a correction; TripleScreen3.cpp:801's real call site has no
        // bar-close gate, unlike Task 5's original assumption).
        if (mdr::IsCandidateDim(MTS::Schema::Contract::kObsMeanRevZ)) {
            const size_t closedSz = m_ts3ClosedCloses.size();
            if (closedSz >= static_cast<size_t>(kTs3ObsWindowN - 1)) {
                std::array<float, kTs3ObsWindowN> prices{};
                for (size_t i = 0; i < static_cast<size_t>(kTs3ObsWindowN - 1); ++i) {
                    prices[i] = m_ts3ClosedCloses[closedSz - static_cast<size_t>(kTs3ObsWindowN - 1) + i];
                }
                prices[static_cast<size_t>(kTs3ObsWindowN - 1)] =
                    m_hasTs3LiveBar ? static_cast<float>(m_ts3LiveClose) : prices[static_cast<size_t>(kTs3ObsWindowN - 2)];
                const float meanRevZ =
                    mrc::ComputeMeanReversionZ(prices.data(), kTs3ObsWindowN, m_lastValidMeanRevZ);
                m_lastValidMeanRevZ = meanRevZ;
                m_obs.mutate_mean_rev_z(meanRevZ);
            }
        }
    }

    // Activity-clock dims (skewness_idx, fast_taleb_kurtosis,
    // fast_hurst_exponent, recurrence_rate) -- mirrors ContextManager.cpp's
    // own inline block (lines ~432-507), reading the SAME 100-bar imbalance-
    // bar-return buffer shape ActivityClockManager's ImbalanceBarEngine
    // produces. Per the standing calendar-clock/imbalance-clock boundary rule,
    // this reuses ONLY ImbalanceBarEngine.h's pure math (bypassing
    // ActivityClockManager, which #includes sierrachart.h) -- it must NOT
    // read from ImbalanceContextManager/ImbalanceClockManager, a genuinely
    // separate, not-yet-cut-over system.
    void ComputeActivityClockDims(float price, int64_t askVolume, int64_t bidVolume) {
        m_imbalanceEngine.OnTickWithPrice(m_imbalanceTickIndex++,
                                           static_cast<float>(askVolume),
                                           static_cast<float>(bidVolume), price);

        // skewness_idx/fast_taleb_kurtosis/fast_hurst_exponent/recurrence_rate:
        // all four read the same 100-bar imbalance-bar-return buffer, mirroring
        // ContextManager.cpp's own inline block (lines ~432-507). Each is
        // independently guarded on its own IN/OUT status (operator directive,
        // 2026-09-16) -- an OUT dim skips its real math entirely here (the
        // actual "stop computing" savings; ApplyOutDimZeroing() already forced
        // its output to 0.0f this tick).
        using MTS::Schema::Contract::kObsFastTalebKurtosis;
        using MTS::Schema::Contract::kObsSkewnessIdx;
        using MTS::Schema::Contract::kObsFastHurstExponent;
        using MTS::Schema::Contract::kObsRecurrenceRate;
        using MTS::Schema::Contract::kObsFastMeanRevZ;
        if (!mdr::IsCandidateDim(kObsFastTalebKurtosis) && !mdr::IsCandidateDim(kObsSkewnessIdx) &&
            !mdr::IsCandidateDim(kObsFastHurstExponent) && !mdr::IsCandidateDim(kObsRecurrenceRate) &&
            !mdr::IsCandidateDim(kObsFastMeanRevZ)) {
            return;  // all 5 OUT -- skip the imbalance-bar-return fetch entirely
        }

        std::array<float, 100> returnsArray{};
        const std::size_t count = m_imbalanceEngine.GetImbalanceBarReturns(100, returnsArray.data());
        if (count >= 100) {
            if (mdr::IsCandidateDim(kObsFastTalebKurtosis)) {
                const float moorsKurtosisRaw = MoorsKurtosis(returnsArray);
                m_lastValidFastTalebKurtosis =
                    std::isfinite(moorsKurtosisRaw) ? moorsKurtosisRaw : m_lastValidFastTalebKurtosis;
                m_obs.mutate_fast_taleb_kurtosis(m_lastValidFastTalebKurtosis);
            }

            if (mdr::IsCandidateDim(kObsSkewnessIdx)) {
                const float bowleySkewnessRaw = BowleySkewness(returnsArray);
                m_lastValidSkewnessIdx =
                    std::isfinite(bowleySkewnessRaw) ? bowleySkewnessRaw : m_lastValidSkewnessIdx;
                m_obs.mutate_skewness_idx(m_lastValidSkewnessIdx);
            }

            if (mdr::IsCandidateDim(kObsFastHurstExponent)) {
                const float fastHurstRaw = DfaHurstExponent(returnsArray.data(), 100, 8);
                m_lastValidFastHurst = std::isfinite(fastHurstRaw) ? fastHurstRaw : m_lastValidFastHurst;
                m_obs.mutate_fast_hurst_exponent(m_lastValidFastHurst);
            }

            if (mdr::IsCandidateDim(kObsFastMeanRevZ)) {
                // wired 2026-09-17, explicit operator authorization -- see
                // ContextManager.cpp's own comment at the live call site for the full trail.
                const float fastMeanRevZRaw = ActivityClockMeanRevZ(returnsArray.data(), 100);
                m_lastValidFastMeanRevZ = std::isfinite(fastMeanRevZRaw) ? fastMeanRevZRaw : m_lastValidFastMeanRevZ;
                m_obs.mutate_fast_mean_rev_z(m_lastValidFastMeanRevZ);
            }

            if (mdr::IsCandidateDim(kObsRecurrenceRate)) {
                // recurrence_rate: only rebuild the O(n^2) closed-bar window when a
                // NEW imbalance bar has actually closed (RecurrenceRateEngine's own
                // caching contract), not every tick.
                const std::size_t completedBarCount = m_imbalanceEngine.GetCompletedBarCount();
                if (completedBarCount != m_lastRecurrenceBarCount) {
                    const float epsilon = static_cast<float>(
                        SelectEpsilonForTargetRecurrenceRate(returnsArray.data(), 100, 0.05));
                    m_recurrenceEngine.RebuildClosedBarWindow(returnsArray.data(), 99, epsilon);
                    m_cachedRecurrenceRate = m_recurrenceEngine.ComputeRate(returnsArray[99], epsilon);
                    m_lastRecurrenceBarCount = completedBarCount;
                }
                m_obs.mutate_recurrence_rate(m_cachedRecurrenceRate);
            }
        } else {
            // Warmup: neutral defaults, matching ContextManager.cpp's own
            // warmup branch and this engine's constructor cold-start values --
            // only for dims still IN (an OUT dim's 0.0f from ApplyOutDimZeroing()
            // must not be overwritten here).
            if (mdr::IsCandidateDim(kObsFastTalebKurtosis)) {
                m_obs.mutate_fast_taleb_kurtosis(m_lastValidFastTalebKurtosis);
            }
            if (mdr::IsCandidateDim(kObsSkewnessIdx)) {
                m_obs.mutate_skewness_idx(m_lastValidSkewnessIdx);
            }
            if (mdr::IsCandidateDim(kObsFastHurstExponent)) {
                m_obs.mutate_fast_hurst_exponent(m_lastValidFastHurst);
            }
            if (mdr::IsCandidateDim(kObsFastMeanRevZ)) {
                m_obs.mutate_fast_mean_rev_z(m_lastValidFastMeanRevZ);
            }
            if (mdr::IsCandidateDim(kObsRecurrenceRate)) {
                m_obs.mutate_recurrence_rate(m_cachedRecurrenceRate);
            }
        }
    }

    // Task 10 (spec §3a open Q2): every dim's OWN warm-up requirement must be
    // satisfied before ANY emission, not just FeatureScaler's 500-TICK
    // counter (Task 8). FeatureScaler warms up on tick COUNT; TS1/TS2's
    // bar-based dims need elapsed SESSION TIME (TS1's 100 240-minute bars
    // alone need ~16+ days) -- on a real feed, FeatureScaler can warm up
    // while every bar-based dim is still frozen at its cold-start default,
    // which would silently emit placeholder-masquerading-as-real values.
    // Checks the single LARGEST per-timeframe requirement (TS2's fractal_dim
    // window dominates TS2's own smaller ATR/obs-window needs; TS3's
    // liq_fragility window dominates its own smaller amihud/mean_rev_z
    // needs) -- sufficient because these buffers are shared and fill in
    // lockstep, not independently gated per dim.
    bool AllDimsReady() const {
        return m_ts1Closes.size() >= static_cast<size_t>(kTs1WindowN) &&
               m_ts2Closes.size() >= static_cast<size_t>(kTs2FractalWindowN) &&
               m_ts3ClosedRanges.size() >= static_cast<size_t>(kTs3LiqFragWindowN) &&
               m_imbalanceEngine.GetCompletedBarCount() >= 100 &&
               m_tailRiskEngine.GetSampleCount() >= 50;
    }

    // Task 8: the real collection-mode change gate. Quality-over-quantity
    // correction, 2026-09-08 (docs/superpowers/specs/2026-09-08-context-
    // emission-gate-quality-over-quantity-spec.md): ContextManager.cpp's
    // ShouldTriggerHMM() now shares ONE Mahalanobis significant-change
    // standard across both the live-trading and data-collection paths (was
    // previously a separate any-dim-moved-by-epsilon mechanism for data
    // collection specifically) -- this replicates that same unified gate,
    // not the old mechanism this task originally planned before the
    // correction. Mirrors CheckAndTriggerHMM's real per-call sequence:
    // Sanitize -> validate(raw) -> FeatureScaler.UpdateAndNormalize ->
    // warmup gate -> validate(scaled) -> PushObservation ->
    // ComputeTriggerDecisionMetrics -> ShouldTriggerHMM -> SetBaseline.
    bool ComputeShouldEmit(int64_t timestampUs) {
        if (!AllDimsReady()) return false;

        auto rawObs = MTS::Schema::Contract::ToObservationArray(m_obs);

        uint64_t nonFiniteCount = 0;
        uint64_t clampedCount = 0;
        rawObs = otg::SanitizeObservationVector(rawObs, nonFiniteCount, clampedCount);
        for (float v : rawObs) {
            if (!std::isfinite(v)) return false;
        }

        const auto currentObs = m_featureScaler.UpdateAndNormalize(rawObs);
        if (!m_featureScaler.warmedUp) return false;  // FeatureScaler::RANK_WINDOW=500 samples
        for (float v : currentObs) {
            if (!std::isfinite(v)) return false;
        }

    // Task 13/17/§3c (docs/superpowers/specs/2026-09-09-market-data-replay-dim-
    // selection-spec.md): the significant-change decision runs over ONLY the
    // current candidate dims (CandidateObservationDims.h), extracted from the
    // full 18D scaled vector -- FeatureScaler itself still scales all 18 (its
    // positional calibration arrays are sized for that, out of scope to
    // change here). §3c: the gate now computes its distance DIRECTLY from
    // this already-scaled vector, with no second rolling-window
    // re-normalization of its own (see CandidateTriggerGate.h's own header
    // comment for why the earlier §3a/§3b design was empirically wrong).
    // Deliberately NOT the shared include/ObservationTriggerGate.h -- that
    // header, and live ContextManager.cpp, stay untouched until dim selection
    // concludes (spec §2/§4).
    std::array<float, mdr::kCandidateDimCount> candidateObs{};
    for (std::size_t i = 0; i < mdr::kCandidateDimCount; ++i) {
        candidateObs[i] = currentObs[mdr::kCandidateDims[i]];
    }
    m_lastScaledCandidateObs = candidateObs;

    const mdr::CandidateTriggerMetrics triggerMetrics =
        m_candidateTriggerGate.ComputeTriggerDecisionMetrics(candidateObs);
    m_lastTriggerMetrics = triggerMetrics;

    const bool shouldTrigger =
        !m_candidateTriggerGate.HasBaseline() || triggerMetrics.significant_change;

    if (shouldTrigger) {
        m_candidateTriggerGate.SetBaseline(candidateObs);
    }
    return shouldTrigger;
}

    tba::TickBarAggregator m_ts1;
    tba::TickBarAggregator m_ts2;
    tba::TickBarAggregator m_ts3;
    tba::TickBarAggregator m_dailyBars;  // feeds m_prevDayHigh/m_prevDayLow (STRUCTURE_TEST)

    MTS::Schema::ObservationData m_obs{};

    int m_ts1BarsClosed = 0;
    int m_ts2BarsClosed = 0;
    int m_ts3BarsClosed = 0;

    RingBuffer<uint64_t, kTickTimestampWindow> m_tickTimestamps;
    MindfulTrader::TailRiskEngine m_tailRiskEngine;
    MindfulTrader::InformationEngine m_infoEngine;
    double m_lastPrice = 0.0;
    bool m_hasLastPrice = false;

    RingBuffer<float, kTs1CloseCapacity> m_ts1Closes;
    float m_lastValidFisherInfo = 0.0f;

    RingBuffer<float, kTs2AtrCapacity> m_ts2TrueRanges;
    RingBuffer<float, kTs2CloseCapacity> m_ts2Closes;
    RingBuffer<float, kTs2EmaHistCapacity> m_ts2EmaHist;  // feeds EMA_PROXIMITY's rolling stddev
    float m_lastValidRelRange = 0.0f;
    float m_lastValidLogScaleExpansionRatio = 0.0f;
    float m_lastValidFractalDim = 1.5f;
    float m_ts2PrevClose = 0.0f;
    bool m_hasTs2PrevClose = false;

    float m_regimeTenure = 0.0f;
    float m_prevTs2Efficiency = 0.0f;
    float m_prevTs2Volatility = 0.0f;
    bool m_hasTs2PrevRegimeStats = false;

    // Live-forming TS2 bar state (Task 6b), same rationale as TS3's own
    // m_ts3Live* members below -- TickBarAggregator only exposes COMPLETED
    // bars via its callback, so this tool tracks the live bar itself for
    // relative_range/log_scale_expansion_ratio/fractal_dim/regime tenure's
    // real per-tick reactivity.
    bool m_ts2JustClosedThisTick = false;
    bool m_hasTs2LiveBar = false;
    double m_ts2LiveHigh = 0.0;
    double m_ts2LiveLow = 0.0;
    double m_ts2LiveClose = 0.0;

    // Live-forming TS3 bar state (mirrors sc.High/Low/Volume/AskVolume/
    // BidVolume for the STILL-FORMING bar -- TickBarAggregator only exposes
    // completed bars via its callback, so this tool tracks the live bar
    // itself for amihud_illiquidity/liq_fragility/micro_asymmetry's
    // documented every-tick reactivity, an already-existing production
    // design, not an invented one).
    bool m_ts3JustClosedThisTick = false;
    bool m_hasTs3LiveBar = false;
    double m_ts3LiveOpen = 0.0;
    double m_ts3LiveHigh = 0.0;
    double m_ts3LiveLow = 0.0;
    double m_ts3LiveClose = 0.0;
    int64_t m_ts3LiveVolume = 0;
    int64_t m_ts3LiveAskVolume = 0;
    int64_t m_ts3LiveBidVolume = 0;

    RingBuffer<float, kTs3ClosedBarCapacity> m_ts3ClosedCloses;
    RingBuffer<float, kTs3ClosedBarCapacity> m_ts3ClosedVolumes;
    RingBuffer<float, kTs3ClosedBarCapacity> m_ts3ClosedRanges;
    RingBuffer<float, kTs3HurstCapacity> m_ts3HurstCloses;  // ElderBreakout's real TS3-local Hurst window

    // Task 5: bounded pattern-detector history rings (see constructor +
    // OnTs3BarClose() for wiring, GetNr7Ring()/GetTurtleSoupRing() for access).
    BoundedBarRing m_nr7Ring;
    BoundedBarRing m_turtleSoupRing;
    // RASCHKE_STRATEGY_SETUP (TS2): 30-bar high/low ring, sized for GHOST's
    // own deepest swing-confirmed divergence lookback (see kTs2PatternRingCapacity's
    // own comment). Parallel close/volume rings kept in lockstep (pushed the
    // same tick, same count) since BoundedBarRing itself only holds high/low.
    BoundedBarRing m_ts2PatternRing;
    RingBuffer<float, kTs2PatternRingCapacity + 1> m_ts2PatternCloses;
    RingBuffer<float, kTs2PatternRingCapacity + 1> m_ts2PatternVolumes;

    // Task 7 (spec §3 item 2): compute engines shared by the 5 already-
    // audited PRIMARY_TRIGGER_MASK detectors, fed on TS3 (or TS2, where
    // noted) bar close. Periods/smoothing confirmed against real source --
    // see each engine's own header for citations.
    WilderAtr m_atr10;               // TS3 ATR(10) Wilder -- Subgraph_AtrTemp3
    RsiEngine m_rsi3;                 // TS3 RSI(3) SIMPLE ("Cutler's RSI")
    RsiEngine m_rsi10;                // TS3 RSI(10) SIMPLE
    StochasticEngine m_stoch;         // TS3 Stochastic(3,3,3) SIMPLE
    StochasticEngine m_ts2Stoch;      // TS2 Stochastic(10,3,3) SIMPLE -- feeds INTERM_STOCHASTIC
    Ema m_ts3KeltnerEma10;            // TS3 Keltner center: EMA(10) of OHLC-avg
    Ema m_ts1ImpulseEma13;            // TS1 Impulse EMA(13) of CLOSE (not OHLC-avg -- feeds LONG_IMP)
    MacdEngine m_ts1Macd;             // TS1 Impulse MACD(12,26,9) on close -- feeds LONG_IMP (scsf_Screen1_MACD)
    Ema m_ts2ImpulseEma13;            // TS2 Impulse EMA(13) of OHLC-avg -- feeds INTERM_IMP
    MacdEngine m_ts2Macd;             // TS2 Impulse MACD(12,26,9) on close -- feeds INTERM_IMP
    Ema m_ts2Ema21;                   // TS2 21-period EMA of close -- RASCHKE_STRATEGY_SETUP's ema21
    StochasticEngine m_ts2AntiStoch;  // TS2 Stochastic(7,4,10) SIMPLE -- ANTI pattern only
    RsiEngine m_rsiTop;                // TS2 RSI(2) WILDERS -- IndicatorKey::RSI (distinct from MomentumPinball's rsi3/rsi10)
    SmaWindow m_avgVolume14;          // TS3 14-bar SMA of volume (Subgraph_AvgVolume)

    float m_prevRsi3 = 0.0f;
    float m_prevRsi10 = 0.0f;
    bool m_hasPrevRsi = false;

    // INTERM_IMP (TS2's own Impulse color) is read by MomentumPinball (a TS3
    // pattern) as a genuine cross-timeframe input -- production stores its
    // own per-TS3-bar snapshot (Subgraph_PrevImpulseColor) rather than
    // reading TS2's bar cadence directly, since TS2 may or may not have
    // closed a new bar between two TS3 closes.
    int m_currentImpulseColor = kImpulseBlue;
    int m_ts3PrevImpulseColorSnapshot = kImpulseBlue;
    int m_lastEmittedTs2ImpulseColor = kImpulseBlue;
    float m_prevTs2ImpulseEmaValue = 0.0f;
    bool m_hasPrevTs2ImpulseEma = false;
    float m_prevTs2MacdHistogram = 0.0f;
    bool m_hasPrevTs2MacdHistogram = false;

    int m_currentTs1ImpulseColor = kImpulseBlue;
    int m_lastEmittedTs1ImpulseColor = kImpulseBlue;
    float m_prevTs1ImpulseEmaValue = 0.0f;
    bool m_hasPrevTs1ImpulseEma = false;
    float m_prevTs1MacdHistogram = 0.0f;
    bool m_hasPrevTs1MacdHistogram = false;

    RSI m_lastRsiTop = RSI::UNDEFINED;

    // INTERM_STOCHASTIC: TS2's own Stochastic(10,3,3) crossover classification
    // (src/TripleScreen2.cpp scsf_Screen2_StochasticCrossover) -- needs the
    // PREVIOUS bar's RawK/FastD ("SlowK"/"SlowD" in that study's own, real,
    // misleading local naming) to detect a crossover.
    float m_prevTs2StochRawK = 0.0f;
    float m_prevTs2StochFastD = 0.0f;
    bool m_hasPrevTs2Stoch = false;
    StochasticEnum m_lastIntermStochastic = StochasticEnum::UNDEFINED;
    EmaProximity m_lastEmaProximity = EmaProximity::NONE;
    VolumeState m_volumeState{};
    VolumeEnum m_lastVolumeSignal = VolumeEnum::NORMAL;
    dbe::Bias m_lastDailyBias = dbe::Bias::PHYSICS_VETO_RANDOM_WALK;

    // Task 9 (spec §3 item 3): pooled TrainingEventT scratch object -- see
    // BuildTrainingEventT()'s own comment for the "never construct fresh per
    // emission" DOD rationale (spec §11 item 1).
    MTS::Training::TrainingEventT m_trainingEventScratch;

    // Task 10 (spec §5 item 5): pooled RiskGateContextT scratch object, same
    // DOD rationale as m_trainingEventScratch above. RiskGateContextT is a
    // NativeTable with no nested unique_ptr fields, so unlike TrainingEventT
    // there's no separate nested-allocation concern -- the struct itself is
    // reused, never freshly constructed per call.
    MTS::Schema::RiskGateContextT m_riskGateContextScratch;

    // Elder Breakout's per-bar consolidation history (close/topBand/
    // bottomBand/atr, one entry per completed TS3 bar).
    RingBuffer<float, kElderConsolidationCapacity> m_ts3ElderCloseHist;
    RingBuffer<float, kElderConsolidationCapacity> m_ts3ElderTopBandHist;
    RingBuffer<float, kElderConsolidationCapacity> m_ts3ElderBottomBandHist;
    RingBuffer<float, kElderConsolidationCapacity> m_ts3ElderAtrHist;

    KangarooTailEnum m_lastKangarooTail = KangarooTailEnum::NONE;
    TurtleSoupEnum m_lastTurtleSoup = TurtleSoupEnum::NONE;
    MomentumPinballEnum m_lastMomentumPinball = MomentumPinballEnum::NONE;
    ElderBreakoutEnum m_lastElderBreakout = ElderBreakoutEnum::NONE;
    float m_lastElderBreakoutTs3Hurst = 0.5f;
    NR7Enum m_lastNr7 = NR7Enum::NONE;

    // RASCHKE_TACTICAL_TRIGGER (src/TripleScreen3.cpp's 5-writer, last-write-
    // wins field -- see IndicatorComputations.h's own RaschkeTacticalTrigger
    // doc comment for the precedence order). State for the lowest-precedence
    // base classifier (DetectRaschkeTacticalTrigger's real RSI-swing/
    // Stochastic-pop/ITR logic, src/StudyHelperFunctions.cpp).
    static constexpr int kRsiSwingLookbackBars = 5;      // PatternConstants::RSI_SWING_LOOKBACK
    static constexpr int kTacticalMinLookback = 20;      // PatternConstants::TACTICAL_MIN_LOOKBACK
    static constexpr int kVolumeAvgLookback = 10;        // PatternConstants::VOLUME_AVG_LOOKBACK
    static constexpr float kVolumeBreakoutMultiplier = 1.2f;  // PatternConstants::VOLUME_BREAKOUT_MULTIPLIER
    static constexpr float kRsiOversold = 30.0f;
    static constexpr float kRsiOverbought = 70.0f;
    static constexpr float kStochOversold = 20.0f;
    static constexpr float kStochOverbought = 80.0f;
    static constexpr int kTurtleSoupMinSeparation = 4;   // TURTLE_SOUP_MIN_SEPARATION
    static constexpr float kSupportResistanceThreshold = 0.5f;  // SUPPORT_RESISTANCE_THRESHOLD
    static constexpr float kKangarooQualityThreshold = 0.6f;    // KANGAROO_QUALITY_THRESHOLD

    RingBuffer<float, kRsiSwingLookbackBars + 1> m_rsiSwingHighs;
    RingBuffer<float, kRsiSwingLookbackBars + 1> m_rsiSwingLows;

    // ITR (Initial Trading Range) per-trading-day persistent state, reset on
    // ET calendar-date change (production's own `barTime.GetDate()` --
    // deliberately calendar-date, not sc.GetTradingDayDate()).
    int64_t m_itrDayNumber = -1;
    bool m_hasItrDay = false;
    float m_itrHigh = 0.0f;
    float m_itrLow = 0.0f;
    bool m_itrEstablished = false;
    bool m_hadBreakoutAbove = false;
    bool m_hadBreakdownBelow = false;

    // Immediately-prior TS3 bar's own high/low (NOT prevDayHigh/Low) --
    // ITR fade's own prevHigh/prevLow inputs.
    float m_prevTs3BarHigh = 0.0f;
    float m_prevTs3BarLow = 0.0f;
    bool m_hasPrevTs3Bar = false;

    // TurtleSoup's own 20-bar prior-window extremes, retained (Task 9) for
    // TrainingEvent's prev_four_bar_high/low (production's own, confusingly-
    // named m_shortTermExtremes.prevFourBarHigh/Low -- a real 20-bar window,
    // not "four bars", per BoundedBarRing.h's own citation).
    float m_prevFourBarHigh = 0.0f;
    float m_prevFourBarLow = 0.0f;

    // NR7 breakout visualization's own 3-bar "was any of the last 3 bars
    // NR7" marker history (src/TripleScreen3.cpp's Subgraph_NR7Marker scan).
    static constexpr int kNr7MarkerLookback = 3;
    RingBuffer<uint8_t, kNr7MarkerLookback + 1> m_nr7MarkerFlags;
    RingBuffer<float, kNr7MarkerLookback + 1> m_nr7MarkerHighs;
    RingBuffer<float, kNr7MarkerLookback + 1> m_nr7MarkerLows;

    RaschkeTacticalTrigger m_lastRaschkeTactical = RaschkeTacticalTrigger::NONE;
    float m_lastKangarooTailQuality = 0.0f;  // KangarooTail's own qualityScore, needed by its RASCHKE_TACTICAL_TRIGGER gate
    // Real bug found 2026-09-18 (lbrnet session): these 4 were already computed by their own
    // Detect*() calls below but discarded -- only the enum got serialized, never the quality
    // score IndicatorState's own schema has a field for. Added alongside the existing
    // KangarooTail precedent, same "raw quality, unaffected by any later enum override" semantics
    // as production's own SetMetrics() forwarding (src/TripleScreen3.cpp).
    float m_lastTurtleSoupQuality = 0.0f;
    float m_lastMomentumPinballQuality = 0.0f;
    float m_lastElderBreakoutQuality = 0.0f;
    float m_lastNr7Quality = 0.0f;

    // RASCHKE_STRATEGY_SETUP (TS2): TS2-local 100-bar DFA(minScale=8) Hurst
    // (mirrors TS3's own m_ts3HurstCloses/m_lastElderBreakoutTs3Hurst pattern,
    // just fed TS2's own 60-minute closes instead of TS3's 15-minute ones).
    RingBuffer<float, kTs2HurstCapacity> m_ts2HurstCloses;
    float m_lastTs2Hurst = 0.5f;

    // RASCHKE_STRATEGY_SETUP's own independent MACD-diff history for
    // ComputeMacd() (SLINGSHOT/GHOST/FIRST_CROSS's shared MacdEnum classifier)
    // -- deliberately separate from INTERM_IMP's own m_prevTs2MacdHistogram
    // (a 1-deep tracker; ComputeMacd needs a genuine 2-deep diffPrev1/
    // diffPrev2 shift register, mirroring Macd::SetFromChart's own real state
    // machine, src/Indicator.cpp:29-44). Both read the SAME underlying
    // m_ts2Macd histogram value, just retained differently.
    MacdState m_ts2StrategyMacdState;
    float m_ts2StrategyMacdDiffPrev1 = 0.0f;
    float m_ts2StrategyMacdDiffPrev2 = 0.0f;
    int m_ts2StrategyMacdBarsSeen = 0;
    MacdEnum m_currentTs2StrategyMacd = MacdEnum::AT_ZERO;

    // ANTI's own Stochastic(7,4,10) needs 3 historical (FastD, SlowD) pairs
    // (production's own k_curr/k_prev/k_prev2, d_curr/d_prev/d_prev2, each
    // freshly recomputed from raw OHLC history in the real code -- an
    // incremental engine fed once per TS2 close is mathematically identical,
    // just retaining the last 3 outputs instead of recomputing from scratch).
    RingBuffer<float, 4> m_antiKHist;  // FastD history (production's own confusingly-named "k")
    RingBuffer<float, 4> m_antiDHist;  // SlowD history (production's own "d")

    RaschkeStrategySetup m_lastRaschkeStrategy = RaschkeStrategySetup::NONE;

    ATRProximityEnum m_lastAtrProximity = ATRProximityEnum::LOW_VOLATILITY;
    StructureTest m_lastStructureTest = StructureTest::NONE;
    float m_prevDayHigh = 0.0f;  // fed by OnDailyBarClose(), STRUCTURE_TEST's real prev_high
    float m_prevDayLow = 0.0f;   // fed by OnDailyBarClose(), STRUCTURE_TEST's real prev_low
    bool m_hasPrevDayRange = false;
    uint64_t m_patternDirtyMask = 0;

    float m_lastValidAmihud = 0.5f;
    float m_lastValidLiqFragility = 0.0f;
    float m_lastValidMeanRevZ = 0.0f;
    float m_lastValidMicroAsymmetry = 0.0f;

    ImbalanceBarEngine m_imbalanceEngine;
    int m_imbalanceTickIndex = 0;
    float m_lastValidFastTalebKurtosis = 1.23f;
    float m_lastValidSkewnessIdx = 0.0f;
    float m_lastValidFastHurst = 0.5f;
    float m_lastValidFastMeanRevZ = 0.0f;
    RecurrenceRateEngine m_recurrenceEngine;
    std::size_t m_lastRecurrenceBarCount = 0;
    float m_cachedRecurrenceRate = 0.0f;

    // Task 8/13: FeatureScaler still scales all 18 dims (its calibration
    // arrays are sized for that); the emission decision itself (Task 13) runs
    // over only the current candidate dims via m_candidateTriggerGate.
    FeatureScaler m_featureScaler;
    mdr::CandidateTriggerGate m_candidateTriggerGate;

    // Task 11 (spec §3f): backwards-timestamp hard-error guard.
    int64_t m_lastTimestampUs = 0;
    bool m_hasLastTimestamp = false;

    // Diagnostic-only (calibration investigation, 2026-09-09) -- not read by
    // any production logic, only GetLastTriggerMetrics().
    mdr::CandidateTriggerMetrics m_lastTriggerMetrics{};

    // Diagnostic-only (double-normalization investigation, 2026-09-15) -- not
    // read by any production logic, only GetLastScaledCandidateObs().
    std::array<float, mdr::kCandidateDimCount> m_lastScaledCandidateObs{};
};
