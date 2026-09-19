// Singleton holder driving the IS3 -> IS2 -> IS1 hierarchical bars-of-bars cascade
// (docs/superpowers/specs/2026-09-18-predator-sniper-execution-architecture.md §1.2a/§1.2b).
// Owns the one real ImbalanceBarEngine (IS3, adaptive threshold); IS2/IS1 "bars" are aggregates
// of K2/K1 completed finer-level log returns, not independent engines -- rejected alternative
// (3 independent adaptive engines) suffers stochastic boundary drift with no structural
// relationship between bar closes (§1.2a).
//
// Scope: aggregation is over LOG RETURNS ONLY, not full OHLCV bars. The 4 activity-clock dims
// already placed onto IS1/IS2/IS3 (hurst_exponent, recurrence_rate, skewness_idx,
// taleb_kurtosis) all consume a return series, not OHLCV -- summing K consecutive log returns
// is an exact, not approximate, way to get the coarser bar's own log return
// (log(P_end/P_start) = sum of log(P_i/P_i-1)). A full OHLCV ImbalanceBar struct is deferred
// until/unless a future dim actually needs open/high/low/volume at the aggregated level.
//
// Producer discipline (§1.2b): only ImbalanceScreen1.cpp (IS1, highest sc.CalculationPrecedence,
// STD_PREC_LEVEL) calls OnTick(). ImbalanceScreen2.cpp/ImbalanceScreen3.cpp are pure readers.
#pragma once

#include "ImbalanceBarEngine.h"
#include "RingBuffer.h"

#include <cstddef>
#include <cstdint>

class ImbalanceClockManager {
public:
    static constexpr std::size_t kAggregateBufferCapacity = 500;

    static ImbalanceClockManager& Instance() {
        static ImbalanceClockManager instance;
        return instance;
    }

    void Reset() {
        m_is3Engine.Reset();
        m_is2Returns.clear();
        m_is1Returns.clear();
        m_is2AccumReturn = 0.0f;
        m_is1AccumReturn = 0.0f;
        m_is2AccumCount = 0;
        m_is1AccumCount = 0;
        m_is2CompletedCount = 0;
        m_is1CompletedCount = 0;
    }

    // K2 = number of completed IS3 bars aggregated into one IS2 bar; K1 = number of completed
    // IS2 bars aggregated into one IS1 bar. Default 4/4 is real-data validated (2026-09-07,
    // tools/observation_vector/imbalance_clock_manager_ratio_eval.cpp), not merely literature-
    // plausible -- see the member declarations below for the specific numbers.
    void SetAggregationCounts(std::size_t k2, std::size_t k1) {
        m_k2 = k2 > 0 ? k2 : 1;
        m_k1 = k1 > 0 ? k1 : 1;
    }

    void ConfigureIs3Threshold(float threshold) { m_is3Engine.SetImbalanceThreshold(threshold); }
    void EnableIs3AdaptiveThreshold(float alpha = 0.01f) { m_is3Engine.EnableAdaptiveThreshold(alpha); }

    // IS1-exclusive producer entry point (§1.2b) -- feeds the raw tick into the IS3 engine and
    // drains every IS3 bar that completed on this exact tick through the K2/K1 cascade in one
    // pass. IS2Screen.cpp/IS3Screen.cpp never call this; they only read.
    void OnTick(int barIndex, float askVolume, float bidVolume, float price) {
        const std::size_t before = m_is3Engine.GetCompletedBarCount();
        m_is3Engine.OnTickWithPrice(barIndex, askVolume, bidVolume, price);
        const std::size_t after = m_is3Engine.GetCompletedBarCount();
        std::size_t newBars = after - before;
        if (newBars == 0) return;

        // Realistically always 1 (one bar close per tick at most), but never assume -- cap to a
        // small fixed buffer rather than allocate, matching this codebase's no-heap-in-hot-path rule.
        constexpr std::size_t kMaxDrainPerTick = 8;
        if (newBars > kMaxDrainPerTick) newBars = kMaxDrainPerTick;
        float drained[kMaxDrainPerTick];
        m_is3Engine.GetImbalanceBarReturns(newBars, drained);
        for (std::size_t i = 0; i < newBars; ++i) {
            FeedIs3Return(drained[i]);
        }
    }

    std::size_t GetIs3CompletedBarCount() const { return m_is3Engine.GetCompletedBarCount(); }
    std::size_t GetIs2CompletedBarCount() const { return m_is2CompletedCount; }
    std::size_t GetIs1CompletedBarCount() const { return m_is1CompletedCount; }

    std::size_t GetIs3Returns(std::size_t n, float* out) const {
        return m_is3Engine.GetImbalanceBarReturns(n, out);
    }
    std::size_t GetIs2Returns(std::size_t n, float* out) const {
        return DrainRingBuffer(m_is2Returns, n, out);
    }
    std::size_t GetIs1Returns(std::size_t n, float* out) const {
        return DrainRingBuffer(m_is1Returns, n, out);
    }

private:
    static std::size_t DrainRingBuffer(const RingBuffer<float, kAggregateBufferCapacity>& buf,
                                        std::size_t n, float* out) {
        const std::size_t available = buf.size();
        const std::size_t count = n < available ? n : available;
        const std::size_t start = available - count;
        for (std::size_t i = 0; i < count; ++i) {
            out[i] = buf[start + i];
        }
        return count;
    }

    void FeedIs3Return(float is3Return) {
        m_is2AccumReturn += is3Return;
        ++m_is2AccumCount;
        if (m_is2AccumCount >= m_k2) {
            PushIs2Return(m_is2AccumReturn);
            m_is2AccumReturn = 0.0f;
            m_is2AccumCount = 0;
        }
    }

    void PushIs2Return(float is2Return) {
        if (m_is2Returns.size() == kAggregateBufferCapacity) m_is2Returns.pop_front();
        m_is2Returns.push_back(is2Return);
        ++m_is2CompletedCount;

        m_is1AccumReturn += is2Return;
        ++m_is1AccumCount;
        if (m_is1AccumCount >= m_k1) {
            PushIs1Return(m_is1AccumReturn);
            m_is1AccumReturn = 0.0f;
            m_is1AccumCount = 0;
        }
    }

    void PushIs1Return(float is1Return) {
        if (m_is1Returns.size() == kAggregateBufferCapacity) m_is1Returns.pop_front();
        m_is1Returns.push_back(is1Return);
        ++m_is1CompletedCount;
    }

    ImbalanceBarEngine m_is3Engine;

    // K2=K1=4, not the literature-plausible 5, per real-data validation (2026-09-07,
    // tools/observation_vector/imbalance_clock_manager_ratio_eval.cpp against 471.9M real MES
    // ticks): K=4 gave lower lag-1 return autocorrelation at both IS2 (0.035 vs 0.087) and IS1
    // (0.135 vs 0.253) than K=5 -- K=5's IS1 autocorrelation (0.253) was even HIGHER than IS3's
    // raw level (0.197), actively reversing the predicted decorrelation trend, not just
    // underperforming it. Also matches this repo's own existing calendar-clock ratio exactly
    // (TS1:TS2:TS3 = 240:60:15min = 4:1 at each hop). See architecture spec §1.2a/§1.2b.
    std::size_t m_k2 = 4;
    std::size_t m_k1 = 4;

    float m_is2AccumReturn = 0.0f;
    float m_is1AccumReturn = 0.0f;
    std::size_t m_is2AccumCount = 0;
    std::size_t m_is1AccumCount = 0;
    std::size_t m_is2CompletedCount = 0;
    std::size_t m_is1CompletedCount = 0;

    RingBuffer<float, kAggregateBufferCapacity> m_is2Returns;
    RingBuffer<float, kAggregateBufferCapacity> m_is1Returns;
};
