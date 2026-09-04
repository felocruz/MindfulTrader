// mean_rev_z_variant_comparison.cpp -- empirical additive-vs-replace decision
// tool for mean_rev_z/fast_mean_rev_z, per the sibling's revised verdict
// (SCRATCHPAD.md, 2026-08-28): correlation alone is not the deciding test:
// feed each variant through Scoring.cpp:305's actual gate condition
// (score > 2.0f) and measure forward-return/hit-rate on the resulting
// signals, independently per variant, on real MES history.
//
// Uses the REAL, unmodified include/ActivityClockMeanReversion.h
// (ActivityClockMeanRevZ) for the activity-clock side -- same "exact port,
// drive with real data" precedent as tools/observation_vector/fractal_dim_threshold_migration.cpp.
// The time-bar side replicates CalculateMeanReversionSpeed's exact math
// (StudyHelperFunctions.cpp:3247) as a faithful reference port, the same
// technique tests/cpp/test_dfa_hurst_exponent.cpp's BruteForceHurst uses --
// the original function is ACSIL-embedded (takes SCStudyInterfaceRef) and
// cannot be #included standalone.
//
// Known, explicitly-flagged approximation: real Sierra Chart tick-level
// signed order flow is not available offline; this tool builds imbalance
// bars from mes_continuous_ticks.parquet's 1-second aggregated
// bid_volume/ask_volume columns instead (confirmed 1-second bars, not
// per-trade ticks, earlier this project). Each 1-second row is fed to
// ImbalanceBarEngine as its own atomic "tick" (a fresh barIndex per row
// forces its delta computation to use that row's own signed volume
// directly) -- this preserves real net per-second order-flow imbalance and
// its cumulative accumulation across seconds, the core AFML imbalance-bar
// mechanism, but loses true intra-second granularity. The engine's
// production imbalance threshold (50.0f) was calibrated for real
// tick-level cumulative signed volume between chart bars, not 1-second
// aggregates -- this tool empirically picks its own threshold (see
// kDefaultImbalanceThreshold below) to produce a reasonable bar-formation
// rate on this coarser data, not the live production value.
//
// I/O is raw binary (contiguous arrays, no text parsing) -- fed by
// tools/observation_vector/mean_rev_z_variant_comparison.py via polars/numpy, not pandas/CSV
// (38.5M-row CSV round-trips were the wrong tool for this data volume).
// Format: [int64 count][int64 timestamp_us x count][float32 field2 x count]
// [float32 field3 x count (ticks file only)][float32 field4 x count (ticks only)].
//
// Build: g++ -O2 -std=c++17 -Iinclude tools/observation_vector/mean_rev_z_variant_comparison.cpp -o mean_rev_z_variant_comparison
// Usage: ./mean_rev_z_variant_comparison ticks_1s.bin bars_15m.bin signals_out.csv [imbalance_threshold]
//   ticks_1s.bin: count, timestamp_us[], close[], bid_volume[], ask_volume[]
//   bars_15m.bin: count, timestamp_us[], close[]
//   signals_out.csv: one row per gate-crossing signal (either variant), for
//                     the companion Python script to compute forward returns
//                     against the full 1-second price series and report
//                     hit-rate/forward-return statistics per variant.
//   imbalance_threshold: optional, defaults to kDefaultImbalanceThreshold below --
//                        pass a candidate value to calibrate the activity
//                        clock's bar-formation rate before trusting results.

#include "ActivityClockMeanReversion.h"
#include "ImbalanceBarEngine.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

constexpr int kReturnBufferN = 100;         // matches ContextManager.cpp's shared 100-return fetch
constexpr float kGateThreshold = 2.0f;      // Scoring.cpp:305's real threshold
constexpr float kDefaultImbalanceThreshold = 15.0f; // fallback if not passed on the command line

struct Tick {
    long long timestamp_us;
    float close;
    float bid_volume;
    float ask_volume;
};

struct Bar15m {
    long long timestamp_us;
    float close;
};

template <typename T>
std::vector<T> ReadArray(std::ifstream& in, std::int64_t count) {
    std::vector<T> out(static_cast<std::size_t>(count));
    in.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(count * static_cast<std::int64_t>(sizeof(T))));
    return out;
}

std::vector<Tick> LoadTicks(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::int64_t count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    const auto ts = ReadArray<std::int64_t>(in, count);
    const auto close = ReadArray<float>(in, count);
    const auto bid = ReadArray<float>(in, count);
    const auto ask = ReadArray<float>(in, count);

    std::vector<Tick> out(static_cast<std::size_t>(count));
    for (std::int64_t i = 0; i < count; ++i) {
        out[static_cast<std::size_t>(i)] = {ts[static_cast<std::size_t>(i)], close[static_cast<std::size_t>(i)],
                                             bid[static_cast<std::size_t>(i)], ask[static_cast<std::size_t>(i)]};
    }
    return out;
}

std::vector<Bar15m> LoadBars15m(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    std::int64_t count = 0;
    in.read(reinterpret_cast<char*>(&count), sizeof(count));
    const auto ts = ReadArray<std::int64_t>(in, count);
    const auto close = ReadArray<float>(in, count);

    std::vector<Bar15m> out(static_cast<std::size_t>(count));
    for (std::int64_t i = 0; i < count; ++i) {
        out[static_cast<std::size_t>(i)] = {ts[static_cast<std::size_t>(i)], close[static_cast<std::size_t>(i)]};
    }
    return out;
}

// Faithful reference port of CalculateMeanReversionSpeed (StudyHelperFunctions.cpp:3033),
// operating on a chronological in-memory price window instead of sc.BaseData. Returns
// both the clamped score and the SIGNED (current - median) deviation before abs(), needed
// here to classify forward-return direction (the production function only returns the
// abs-valued, elasticity-gated score).
//
// FIXED 2026-09-04: this port was stale, still using mean/std -- CalculateMeanReversionSpeed
// was reformulated to median/MAD (Kim & White 2004) 2026-08-31/committed 2026-09-02, but this
// tool was never updated to match, silently invalidating its own "empirically null" verdict
// (that result was measuring the OLD, already-abandoned formula). Re-ported verbatim against
// the current real function before re-running.
struct TimeBarResult { float score; float signedDeviation; };

TimeBarResult TimeBarMeanRevZ(const std::vector<float>& prices, int endIdxInclusive, int n) {
    n = std::clamp(n, 5, 40);
    const int start_idx = endIdxInclusive - n + 1;
    if (start_idx < 0) return {0.0f, 0.0f};

    constexpr double kPriceEps = 1e-6;
    constexpr double kMadConsistency = 1.4826;
    std::array<double, 40> log_prices{};
    for (int i = 0; i < n; ++i) {
        const double p = std::max(static_cast<double>(prices[static_cast<std::size_t>(start_idx + i)]), kPriceEps);
        log_prices[static_cast<std::size_t>(i)] = std::log(p);
    }

    std::array<double, 40> scratch{};
    std::copy_n(log_prices.begin(), n, scratch.begin());
    const int priceMid = n / 2;
    std::nth_element(scratch.begin(), scratch.begin() + priceMid, scratch.begin() + n);
    const double median_log_p = scratch[static_cast<std::size_t>(priceMid)];

    for (int i = 0; i < n; ++i) scratch[static_cast<std::size_t>(i)] = std::fabs(log_prices[static_cast<std::size_t>(i)] - median_log_p);
    std::nth_element(scratch.begin(), scratch.begin() + priceMid, scratch.begin() + n);
    const double mad_log_p = scratch[static_cast<std::size_t>(priceMid)];
    const double scale_log_p = mad_log_p * kMadConsistency;
    if (scale_log_p < 1e-6) return {0.0f, 0.0f};

    const double current_log_p = std::log(std::max(static_cast<double>(prices[static_cast<std::size_t>(endIdxInclusive)]), kPriceEps));
    const double signedDeviation = (current_log_p - median_log_p) / scale_log_p;
    const double abs_z_price = std::fabs(signedDeviation);

    const int m = n - 1;
    if (m < 3) {
        return {std::clamp(static_cast<float>(abs_z_price), 0.0f, 5.0f), static_cast<float>(signedDeviation)};
    }

    std::array<double, 40> returns{};
    for (int i = 0; i < m; ++i) {
        const int idx = start_idx + i + 1;
        const double p = std::max(static_cast<double>(prices[static_cast<std::size_t>(idx)]), kPriceEps);
        const double p_prev = std::max(static_cast<double>(prices[static_cast<std::size_t>(idx - 1)]), kPriceEps);
        returns[static_cast<std::size_t>(i)] = std::log(p / p_prev);
    }
    std::array<double, 40> returnScratch{};
    std::copy_n(returns.begin(), m, returnScratch.begin());
    const int retMid = m / 2;
    std::nth_element(returnScratch.begin(), returnScratch.begin() + retMid, returnScratch.begin() + m);
    const double medianR = returnScratch[static_cast<std::size_t>(retMid)];

    double num = 0.0, den = 0.0;
    for (int t = 1; t < m; ++t) {
        const double r_t = returns[static_cast<std::size_t>(t)] - medianR;
        const double r_prev = returns[static_cast<std::size_t>(t - 1)] - medianR;
        num += r_t * r_prev;
        den += r_prev * r_prev;
    }
    const double rho = (den > 1e-12) ? (num / den) : 0.0;
    const double elasticity_gate = std::clamp(1.0 - std::max(rho, 0.0), 0.0, 1.0);
    const double score = abs_z_price * elasticity_gate;
    return {std::clamp(static_cast<float>(score), 0.0f, 5.0f), static_cast<float>(signedDeviation)};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 4 && argc != 5) {
        std::cerr << "usage: " << argv[0] << " <ticks_1s.bin> <bars_15m.bin> <signals_out.csv> [imbalance_threshold]\n";
        return 1;
    }
    const float imbalanceThreshold = (argc == 5) ? static_cast<float>(std::atof(argv[4])) : kDefaultImbalanceThreshold;
    const auto ticks = LoadTicks(argv[1]);
    const auto bars15m = LoadBars15m(argv[2]);
    std::cerr << "ticks (1s bars): " << ticks.size() << ", 15m bars: " << bars15m.size() << "\n";

    std::ofstream out(argv[3]);
    out << "timestamp_us,price,score,signed_deviation,variant\n";

    // --- Time-bar variant (15-min, CalculateMeanReversionSpeed's own math) ---
    // Uses n=20 (fixed representative window, mid of the [10,40] adaptive
    // observation_window_n range this dim actually runs on in production --
    // a single fixed value here since the adaptive-window derivation itself
    // is orthogonal to this specific measurement).
    {
        std::vector<float> prices(bars15m.size());
        for (std::size_t i = 0; i < bars15m.size(); ++i) prices[i] = bars15m[i].close;
        std::size_t signalCount = 0;
        for (int i = 20; i < static_cast<int>(bars15m.size()); ++i) {
            const auto r = TimeBarMeanRevZ(prices, i, 20);
            if (r.score > kGateThreshold) {
                out << bars15m[static_cast<std::size_t>(i)].timestamp_us << ","
                    << bars15m[static_cast<std::size_t>(i)].close << ","
                    << r.score << "," << r.signedDeviation << ",time_bar\n";
                ++signalCount;
            }
        }
        std::cerr << "time_bar signals (score > " << kGateThreshold << "): " << signalCount << "\n";
    }

    // --- Activity-clock variant (imbalance bars from 1s aggregate ticks) ---
    {
        ImbalanceBarEngine engine;
        engine.SetImbalanceThresholdForTesting(imbalanceThreshold);
        std::deque<float> returnBuffer;  // rolling last-100 returns, chronological (oldest first)
        std::size_t completedBefore = 0;
        std::size_t signalCount = 0;

        for (int i = 0; i < static_cast<int>(ticks.size()); ++i) {
            engine.OnTickWithPrice(/*barIndex=*/i, ticks[static_cast<std::size_t>(i)].ask_volume,
                                    ticks[static_cast<std::size_t>(i)].bid_volume,
                                    ticks[static_cast<std::size_t>(i)].close);
            const std::size_t completedNow = engine.GetCompletedBarCount();
            if (completedNow != completedBefore) {
                // A new imbalance bar just closed. Fetch its return (the most recent
                // element of the engine's own buffer) and append to our rolling window.
                float rawReturns[ImbalanceBarEngine::kImbalanceBarBufferCapacity];
                const std::size_t n = engine.GetImbalanceBarReturns(1, rawReturns);
                if (n == 1) {
                    returnBuffer.push_back(rawReturns[0]);
                    if (returnBuffer.size() > kReturnBufferN) returnBuffer.pop_front();
                }
                completedBefore = completedNow;

                if (returnBuffer.size() == kReturnBufferN) {
                    std::vector<float> window(returnBuffer.begin(), returnBuffer.end());
                    const float score = ActivityClockMeanRevZ(window.data(), kReturnBufferN);
                    if (std::isfinite(score) && score > kGateThreshold) {
                        // Recompute the signed deviation the same way ActivityClockMeanRevZ
                        // does internally (cumulative-path MEDIAN/MAD z-score, Kim & White 2004
                        // -- FIXED 2026-09-04, this previously used a stale mean/std
                        // recomputation even after the header itself was reformulated
                        // 2026-09-02), since the header only returns the final abs-valued/
                        // elasticity-gated score.
                        constexpr double kMadConsistency = 1.4826;
                        std::array<double, kReturnBufferN + 1> path{};
                        double cumsum = 0.0;
                        path[0] = 0.0;
                        for (int k = 0; k < kReturnBufferN; ++k) {
                            cumsum += static_cast<double>(window[static_cast<std::size_t>(k)]);
                            path[static_cast<std::size_t>(k + 1)] = cumsum;
                        }
                        constexpr int count = kReturnBufferN + 1;
                        constexpr int pathMid = count / 2;
                        std::array<double, count> scratch = path;
                        std::nth_element(scratch.begin(), scratch.begin() + pathMid, scratch.begin() + count);
                        const double medianPath = scratch[static_cast<std::size_t>(pathMid)];
                        for (int k = 0; k < count; ++k) scratch[static_cast<std::size_t>(k)] = std::fabs(path[static_cast<std::size_t>(k)] - medianPath);
                        std::nth_element(scratch.begin(), scratch.begin() + pathMid, scratch.begin() + count);
                        const double madPath = scratch[static_cast<std::size_t>(pathMid)];
                        const double scalePath = madPath * kMadConsistency;
                        const double signedDeviation = (scalePath > 1e-9) ? (cumsum - medianPath) / scalePath : 0.0;

                        out << ticks[static_cast<std::size_t>(i)].timestamp_us << ","
                            << ticks[static_cast<std::size_t>(i)].close << ","
                            << score << "," << signedDeviation << ",activity_clock\n";
                        ++signalCount;
                    }
                }
            }
        }
        std::cerr << "activity_clock imbalance bars formed: " << completedBefore << "\n";
        std::cerr << "activity_clock signals (score > " << kGateThreshold << "): " << signalCount << "\n";
    }

    return 0;
}
