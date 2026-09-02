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

// Faithful reference port of CalculateMeanReversionSpeed (StudyHelperFunctions.cpp:3247),
// operating on a chronological in-memory price window instead of sc.BaseData. Returns
// both the clamped score and the SIGNED (current - mean) deviation before abs(), needed
// here to classify forward-return direction (the production function only returns the
// abs-valued, elasticity-gated score).
struct TimeBarResult { float score; float signedDeviation; };

TimeBarResult TimeBarMeanRevZ(const std::vector<float>& prices, int endIdxInclusive, int n) {
    n = std::clamp(n, 5, 40);
    const int start_idx = endIdxInclusive - n + 1;
    if (start_idx < 0) return {0.0f, 0.0f};

    constexpr double kPriceEps = 1e-6;
    double sum_log_p = 0.0, sum_log_p_sq = 0.0;
    for (int i = 0; i < n; ++i) {
        const double p = std::max(static_cast<double>(prices[static_cast<std::size_t>(start_idx + i)]), kPriceEps);
        const double lp = std::log(p);
        sum_log_p += lp;
        sum_log_p_sq += lp * lp;
    }
    const double mean_log_p = sum_log_p / n;
    const double var_log_p = std::max((sum_log_p_sq / n) - (mean_log_p * mean_log_p), 0.0);
    const double std_log_p = std::sqrt(var_log_p);
    if (std_log_p < 1e-6) return {0.0f, 0.0f};

    const double current_log_p = std::log(std::max(static_cast<double>(prices[static_cast<std::size_t>(endIdxInclusive)]), kPriceEps));
    const double signedDeviation = (current_log_p - mean_log_p) / std_log_p;
    const double abs_z_price = std::fabs(signedDeviation);

    const int m = n - 1;
    if (m < 3) {
        return {std::clamp(static_cast<float>(abs_z_price), 0.0f, 5.0f), static_cast<float>(signedDeviation)};
    }

    std::vector<double> returns(static_cast<std::size_t>(m));
    double sum_r = 0.0;
    for (int i = 0; i < m; ++i) {
        const int idx = start_idx + i + 1;
        const double p = std::max(static_cast<double>(prices[static_cast<std::size_t>(idx)]), kPriceEps);
        const double p_prev = std::max(static_cast<double>(prices[static_cast<std::size_t>(idx - 1)]), kPriceEps);
        const double r = std::log(p / p_prev);
        returns[static_cast<std::size_t>(i)] = r;
        sum_r += r;
    }
    const double mean_r = sum_r / m;
    double num = 0.0, den = 0.0;
    for (int t = 1; t < m; ++t) {
        const double r_t = returns[static_cast<std::size_t>(t)] - mean_r;
        const double r_prev = returns[static_cast<std::size_t>(t - 1)] - mean_r;
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
                        // does internally (cumsum z-score), since the header only returns
                        // the final abs-valued/elasticity-gated score.
                        double cumsum = 0.0, sum = 0.0, sumSq = 0.0;
                        for (int k = 0; k < kReturnBufferN; ++k) {
                            cumsum += static_cast<double>(window[static_cast<std::size_t>(k)]);
                            sum += cumsum;
                            sumSq += cumsum * cumsum;
                        }
                        const double count = static_cast<double>(kReturnBufferN + 1);
                        const double mean = sum / count;
                        const double var = std::max((sumSq / count) - (mean * mean), 0.0);
                        const double stdDev = std::sqrt(var);
                        const double signedDeviation = (stdDev > 1e-9) ? (cumsum - mean) / stdDev : 0.0;

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
