// fractal_dim_threshold_migration.cpp -- generates a paired (fractal_dim@30,
// fractal_dim@400) sample from real MES TS2/60min bar data, for percentile-
// matching PositionManager.cpp's live fractal_dim gate thresholds (1.6/1.3)
// against the new 400-bar window, mirroring Task 7's kurtosis/skewness
// threshold-migration methodology (analyze_kurtosis_threshold_migration.py).
//
// #includes the real, unmodified include/SevcikFractalDimension.h (not a
// reimplementation) -- same "exact port, drive with real data" precedent as
// tools/hill_intraday_seasonality.cpp.
//
// Build: g++ -O2 -std=c++17 -Iinclude tools/fractal_dim_threshold_migration.cpp -o fractal_dim_threshold_migration
// Usage: ./fractal_dim_threshold_migration closes.csv paired_out.csv
//        closes.csv: header "timestamp_us,close", one row per 60-min bar, chronological.

#include "SevcikFractalDimension.h"

#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {
constexpr int kOldWindow = 30;   // TripleScreen2.cpp's current std::max(30, observation_window_n) floor
constexpr int kNewWindow = 400;  // measured circular block length (spec Section 5b), rounded
}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <closes.csv> <paired_out.csv>\n";
        return 1;
    }
    std::ifstream in(argv[1]);
    if (!in) { std::cerr << "cannot open " << argv[1] << "\n"; return 1; }
    std::ofstream out(argv[2]);
    if (!out) { std::cerr << "cannot open " << argv[2] << "\n"; return 1; }
    out << "timestamp_us,fractal_dim_30,fractal_dim_400\n";

    std::vector<long long> timestamps;
    std::vector<float> closes;
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string tsField, closeField;
        if (!std::getline(ss, tsField, ',')) continue;
        if (!std::getline(ss, closeField, ',')) continue;
        timestamps.push_back(std::atoll(tsField.c_str()));
        closes.push_back(static_cast<float>(std::atof(closeField.c_str())));
    }
    std::cerr << "bars read: " << closes.size() << "\n";

    const int n = static_cast<int>(closes.size());
    std::vector<float> window(kNewWindow + 1);
    std::size_t rowsWritten = 0;
    for (int idx = kNewWindow; idx < n; ++idx) {
        // prices[0..lookback_n] chronological: prices[i] = sc.Index - lookback_n + i.
        // "sc.Index" here is `idx`.
        for (int i = 0; i <= kOldWindow; ++i) {
            window[static_cast<std::size_t>(i)] = closes[static_cast<std::size_t>(idx - kOldWindow + i)];
        }
        const float fd30 = SevcikFractalDimension(window.data(), kOldWindow);

        for (int i = 0; i <= kNewWindow; ++i) {
            window[static_cast<std::size_t>(i)] = closes[static_cast<std::size_t>(idx - kNewWindow + i)];
        }
        const float fd400 = SevcikFractalDimension(window.data(), kNewWindow);

        if (std::isnan(fd30) || std::isnan(fd400)) continue;  // degenerate window, skip (matches carry-forward semantics -- not a fresh sample)
        out << timestamps[static_cast<std::size_t>(idx)] << "," << fd30 << "," << fd400 << "\n";
        ++rowsWritten;
    }
    std::cerr << "paired rows written: " << rowsWritten << "\n";
    return 0;
}
