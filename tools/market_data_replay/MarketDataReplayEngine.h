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
#include "BipowerVariation.h"
#include "CarryForwardCalculators.h"
#include "DfaHurstExponent.h"
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
                [this](const tba::Bar& bar) { OnTs3BarClose(bar); }) {
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

        m_ts1.OnTick(timestampUs, price, volume, askVolume, bidVolume);
        m_ts2JustClosedThisTick = false;
        m_ts2.OnTick(timestampUs, price, volume, askVolume, bidVolume);
        m_ts3JustClosedThisTick = false;
        m_ts3.OnTick(timestampUs, price, volume, askVolume, bidVolume);

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

        // micro_asymmetry: must update every tick using the CURRENT bar's
        // cumulative ask/bid volume-so-far (StudyHelperFunctions.cpp's own
        // "must be updated every tick, not gated to once per bar" comment) --
        // not this tick's own incremental ask/bid volume, which is a
        // different quantity (production reads sc.AskVolume/BidVolume, which
        // are running per-bar totals, not per-tick deltas).
        {
            const float microAsym = ofae::ComputeMicroAsymmetry(
                static_cast<float>(m_ts3LiveAskVolume), static_cast<float>(m_ts3LiveBidVolume),
                m_lastValidMicroAsymmetry);
            m_lastValidMicroAsymmetry = microAsym;
            m_obs.mutate_micro_asymmetry(microAsym);
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
        m_tickTimestamps.push_back(static_cast<uint64_t>(timestampUs));
        if (m_tickTimestamps.size() == kTickTimestampWindow) m_tickTimestamps.pop_front();
        m_obs.mutate_burstiness_index(eve::CalculateBurstinessIndex(m_tickTimestamps));

        // tail_index/lempel_ziv: BOTH fed only on a genuine price CHANGE --
        // confirmed this session (src/ContextManager.cpp:165-166, both calls
        // live inside UpdateMarketPhysics()'s own price-change gate, not
        // "every tick" for TailRiskEngine as an earlier draft of this plan
        // assumed).
        if (!m_hasLastPrice || price != m_lastPrice) {
            if (m_hasLastPrice && m_lastPrice > 0.0) {
                const double logReturn = std::log(price / m_lastPrice);
                m_infoEngine.AddObservation(logReturn);
                m_tailRiskEngine.AddObservation(logReturn);
                m_obs.mutate_lempel_ziv(static_cast<float>(m_infoEngine.GetLempelZivComplexity()));
                // Matches ContextManager.cpp:410's own >=50-sample floor --
                // below it, tail_index carries its last valid value forward
                // (here: stays at ObservationData's zero-initialized default
                // until warmed up).
                if (m_tailRiskEngine.GetSampleCount() >= 50) {
                    m_obs.mutate_tail_index(static_cast<float>(m_tailRiskEngine.GetHillAlpha()));
                }
            }
            m_lastPrice = price;
            m_hasLastPrice = true;
        }

        return ComputeShouldEmit(timestampUs);
    }

    // Must be called once after the last tick, or the final in-progress bar
    // across all 3 timeframes is silently dropped (spec §3f).
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

private:
    // Matches EventDataCollectorStudy.cpp's CME_ES_SESSION_START_SECS.
    static constexpr int kCmeEsSessionStartSecondsET = 18 * 3600;
    static constexpr int64_t kTs1BarPeriodSeconds = 240 * 60;
    static constexpr int64_t kTs2BarPeriodSeconds = 60 * 60;
    static constexpr int64_t kTs3BarPeriodSeconds = 15 * 60;
    static constexpr size_t kTickTimestampWindow = 100;  // matches production's own window

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
        {
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
        {
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
        {
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

    void OnTs2BarClose(const tba::Bar& bar) {
        ++m_ts2BarsClosed;
        m_ts2JustClosedThisTick = true;

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
        {
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

            auto windowBv = [&prices, sz](int windowN) -> double {
                double logReturns[kTs2ObsWindowN];
                int count = 0;
                for (int i = 0; i < windowN; ++i) {
                    const size_t idx = sz - static_cast<size_t>(windowN) + static_cast<size_t>(i);
                    if (idx == 0) continue;
                    const float price = prices[idx];
                    const float prevPrice = prices[idx - 1];
                    if (price > 0.0f && prevPrice > 0.0f) {
                        logReturns[count++] = std::log(static_cast<double>(price) / prevPrice);
                    }
                }
                return ComputeBipowerVariation(logReturns, count);
            };
            constexpr int kHalfN = kTs2ObsWindowN / 2;
            const double bvFull = windowBv(kTs2ObsWindowN);
            const double bvRecent = windowBv(kHalfN);
            const double bvFullRate = bvFull / kTs2ObsWindowN;
            const double bvRecentRate = bvRecent / kHalfN;
            const float logScaleExpansionRatio = cfc::ComputeBurstinessIndex(
                bvRecentRate, bvFullRate, m_lastValidLogScaleExpansionRatio, -10.0f, 6.0f);
            m_lastValidLogScaleExpansionRatio = logScaleExpansionRatio;
            m_obs.mutate_log_scale_expansion_ratio(logScaleExpansionRatio);

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
        if (closedSz >= static_cast<size_t>(kTs2FractalWindowN)) {
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

    void OnTs3BarClose(const tba::Bar& bar) {
        ++m_ts3BarsClosed;
        m_ts3JustClosedThisTick = true;

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
        // amihud_illiquidity: historical closed-bar sum + live still-forming-
        // bar term (StudyHelperFunctions.cpp:2608-2656's own formula).
        {
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
        {
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
        {
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

        float rawReturns[ImbalanceBarEngine::kImbalanceBarBufferCapacity];
        const std::size_t count = m_imbalanceEngine.GetImbalanceBarReturns(100, rawReturns);
        if (count >= 100) {
            std::array<float, 100> returnsArray;
            std::copy(rawReturns, rawReturns + 100, returnsArray.begin());

            const float moorsKurtosisRaw = MoorsKurtosis(returnsArray);
            if (std::isfinite(moorsKurtosisRaw)) m_lastValidFastTalebKurtosis = moorsKurtosisRaw;
            m_obs.mutate_fast_taleb_kurtosis(m_lastValidFastTalebKurtosis);

            const float bowleySkewnessRaw = BowleySkewness(returnsArray);
            if (std::isfinite(bowleySkewnessRaw)) m_lastValidSkewnessIdx = bowleySkewnessRaw;
            m_obs.mutate_skewness_idx(m_lastValidSkewnessIdx);

            const float fastHurstRaw = DfaHurstExponent(returnsArray.data(), 100, 8);
            if (std::isfinite(fastHurstRaw)) m_lastValidFastHurst = fastHurstRaw;
            m_obs.mutate_fast_hurst_exponent(m_lastValidFastHurst);

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
        // else: not warmed up yet -- all 4 stay at their construction-time
        // defaults (1.23f/0.0f/0.5f/0.0f, FlatBuffers zero-init + Task 6's
        // own constructor seed below), matching ContextManager.cpp's own
        // warm-up branch exactly.
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

        m_triggerGate.PushObservation(currentObs);

        // Matches CalculateEventVelocity(now_us)'s real formula
        // (ContextManager.cpp:550-566) -- EMA of inter-arrival time,
        // tauUs = EVENT_VELOCITY_WINDOW_SEC(2) * 1e6 (ContextManager.h's own
        // constant, replicated directly since ContextManager.h itself isn't
        // includable here, spec §3b).
        constexpr double kEventVelocityWindowSec = 2.0;
        constexpr double kTauUs = kEventVelocityWindowSec * 1'000'000.0;
        const float eventVelocity = eve::UpdateAndGetVelocity(
            m_velocityState, static_cast<uint64_t>(timestampUs), kTauUs);

        const otg::TriggerDecisionMetrics triggerMetrics =
            m_triggerGate.ComputeTriggerDecisionMetrics(currentObs, eventVelocity);

        const bool shouldTrigger = !m_triggerGate.HasBaseline() || triggerMetrics.significant_change;
        if (shouldTrigger) {
            m_triggerGate.SetBaseline(currentObs);
        }
        return shouldTrigger;
    }

    tba::TickBarAggregator m_ts1;
    tba::TickBarAggregator m_ts2;
    tba::TickBarAggregator m_ts3;

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
    double m_ts3LiveHigh = 0.0;
    double m_ts3LiveLow = 0.0;
    double m_ts3LiveClose = 0.0;
    int64_t m_ts3LiveVolume = 0;
    int64_t m_ts3LiveAskVolume = 0;
    int64_t m_ts3LiveBidVolume = 0;

    RingBuffer<float, kTs3ClosedBarCapacity> m_ts3ClosedCloses;
    RingBuffer<float, kTs3ClosedBarCapacity> m_ts3ClosedVolumes;
    RingBuffer<float, kTs3ClosedBarCapacity> m_ts3ClosedRanges;
    float m_lastValidAmihud = 0.5f;
    float m_lastValidLiqFragility = 0.0f;
    float m_lastValidMeanRevZ = 0.0f;
    float m_lastValidMicroAsymmetry = 0.0f;

    ImbalanceBarEngine m_imbalanceEngine;
    int m_imbalanceTickIndex = 0;
    float m_lastValidFastTalebKurtosis = 1.23f;
    float m_lastValidSkewnessIdx = 0.0f;
    float m_lastValidFastHurst = 0.5f;
    RecurrenceRateEngine m_recurrenceEngine;
    std::size_t m_lastRecurrenceBarCount = 0;
    float m_cachedRecurrenceRate = 0.0f;

    // Task 8: the real (post-2026-09-08 correction) collection-mode gate --
    // FeatureScaler + the same Mahalanobis ObservationTriggerGate the
    // live-trading path uses, per ContextManager.cpp's now-unified
    // ShouldTriggerHMM().
    FeatureScaler m_featureScaler;
    otg::ObservationTriggerGate m_triggerGate;
    eve::VelocityState m_velocityState;

    // Task 11 (spec §3f): backwards-timestamp hard-error guard.
    int64_t m_lastTimestampUs = 0;
    bool m_hasLastTimestamp = false;
};
