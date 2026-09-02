// Task 13 (Unit 6b) spike driver: feeds the real production MindfulTrader::TailRiskEngine
// (include/TailRiskEngine.h, post-Task-4: Hill-plot stability-region k-selection +
// EWMA-smoothed output) with real MES 15-min-bar close-to-close log returns, and emits
// one (timestamp_us, alpha) row per bar so a downstream Python script can bucket by
// session time-of-day.
//
// Driving convention matches src/SCStudies.cpp:287-306's documented "historical load"
// mode verbatim: "During historical load: one return per completed bar" -- AddObservation()
// is called once per bar using std::log(currentClose / previousClose), skipping any
// non-positive/unchanged-price bar exactly as SCStudies.cpp does. GetHillAlpha() is read
// after every AddObservation() once ContextManager.cpp:512's warmup gate
// (GetSampleCount() >= 50) is satisfied, and clamped identically to
// ContextManager.cpp:513-516 (finite && 1.1 <= alpha <= 8.0, else neutral 2.5) so the
// output series matches exactly what the live pipeline would write to dim 9 (tail_index).
//
// No production files are modified; this only #includes the shipped header.
//
// Build:   g++ -O2 -std=c++17 -I../include hill_intraday_seasonality.cpp -o hill_intraday_seasonality
// Usage:   ./hill_intraday_seasonality closes.csv alphas_out.csv
//          closes.csv: header "timestamp_us,close", one row per 15-min bar, chronological.
//          alphas_out.csv: written as "timestamp_us,alpha".

#include "TailRiskEngine.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

int main(int argc, char** argv) {
    if (argc != 3) {
        std::cerr << "usage: " << argv[0] << " <closes.csv> <alphas_out.csv>\n";
        return 1;
    }

    std::ifstream in(argv[1]);
    if (!in) {
        std::cerr << "cannot open input " << argv[1] << "\n";
        return 1;
    }
    std::ofstream out(argv[2]);
    if (!out) {
        std::cerr << "cannot open output " << argv[2] << "\n";
        return 1;
    }
    out << "timestamp_us,alpha\n";

    MindfulTrader::TailRiskEngine tailRiskEngine;  // defaults: windowSize=500, tailPercent=0.05,
                                                    // identical to ContextManager.h:391's
                                                    // default-constructed member.

    std::string line;
    std::getline(in, line);  // header

    long long lastTimestampUs = 0;
    double lastPrice = 0.0;
    bool havePrev = false;
    size_t rowsRead = 0;
    size_t alphasWritten = 0;

    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string tsField, closeField;
        if (!std::getline(ss, tsField, ',')) continue;
        if (!std::getline(ss, closeField, ',')) continue;

        long long timestampUs = std::atoll(tsField.c_str());
        double close = std::atof(closeField.c_str());
        ++rowsRead;

        // Mirrors SCStudies.cpp:298-305 exactly: only push a return when price actually
        // changed and both prices are sane positive numbers.
        if (havePrev && close != lastPrice) {
            if (lastPrice > 0.0001 && close > 0.0001) {
                double logReturn = std::log(close / lastPrice);
                tailRiskEngine.AddObservation(static_cast<float>(logReturn));

                // Mirrors ContextManager.cpp:512-519's warmup gate and clamp exactly.
                if (tailRiskEngine.GetSampleCount() >= 50) {
                    double alpha = tailRiskEngine.GetHillAlpha();
                    double clamped = (std::isfinite(alpha) && alpha >= 1.1 && alpha <= 8.0)
                                          ? alpha
                                          : 2.5;
                    out << timestampUs << "," << clamped << "\n";
                    ++alphasWritten;
                }
            }
        }
        lastPrice = close;
        lastTimestampUs = timestampUs;
        havePrev = true;
    }

    std::cerr << "rows read: " << rowsRead << ", alpha samples written: " << alphasWritten
              << ", last timestamp_us: " << lastTimestampUs << "\n";
    return 0;
}
