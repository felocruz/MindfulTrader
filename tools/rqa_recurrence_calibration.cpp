// Final-review Finding 1 driver (2026-08-13): empirically re-derives
// FeatureScaler.h's RECURRENCE_STATIC_CENTER / RECURRENCE_STATIC_SCALE against
// the CORRECTED RQA recurrence-rate distribution.
//
// Why this exists: the shipped constants (center 0.329, scale 0.083) were
// calibrated on 2026-07-23 against the OLD epsilon heuristic
// (max(range*0.1, std*0.5)). Task 3 replaced that with a fixed-target-
// recurrence-rate selector and this review corrected its line-of-identity
// accounting (Finding 4) and its recalibration cadence (Finding 3), so the
// dim's raw distribution moved and the old center/scale would map production
// values to a near-constant z (the exact dimension-collapse pathology the
// initiative exists to remove).
//
// What it replicates, verbatim, from src/StudyHelperFunctions.cpp's
// CalculateRecurrenceRate (post-fix):
//   * n = 30. TripleScreen2.cpp:281 sets slow_window_n = max(30,
//     CalculateAdaptiveObservationWindow(sc, 0.5f)); with the hardcoded
//     coherence of 0.5f, AdaptiveWindowParams::UpdateWindows takes the middle
//     branch and yields observation_vector_n = 20*(1+(0.5-0.5)) = 20 on every
//     bar, so slow_window_n is deterministically 30 in production.
//   * degenerate (range <= 1e-5) windows carry the last valid value forward.
//   * epsilon selected by SelectEpsilonForTargetRecurrenceRate (the real
//     shipped header) at the full-matrix target 0.05, recalibrated on BAR
//     advancement every 200 bars and held fixed in between.
//   * RR measured over the full n*n matrix with the line of identity included.
//
// Input is 60-minute MES bars, because CalculateRecurrenceRate is only called
// from TripleScreen2 (Screen 2 = 60-minute bars per CLAUDE.md), resampled from
// lbrnet/data/raw/mes_ripple_15m.parquet.
//
// Build: g++ -O2 -std=c++17 -Iinclude tools/rqa_recurrence_calibration.cpp -o rqa_recurrence_calibration
// Usage: ./rqa_recurrence_calibration closes.csv [rr_out.csv]
//        closes.csv: header "timestamp_us,close", one row per bar, chronological.

#include "RQAEpsilonSelector.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr int kLookbackN = 30;
constexpr int kRecalibrationBars = 200;
constexpr double kTargetRR = 0.05;

double Quantile(std::vector<double> v, double q) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    const double pos = q * static_cast<double>(v.size() - 1);
    const size_t lo = static_cast<size_t>(std::floor(pos));
    const size_t hi = std::min(lo + 1, v.size() - 1);
    const double frac = pos - static_cast<double>(lo);
    return v[lo] * (1.0 - frac) + v[hi] * frac;
}

// FeatureScaler's ToSoftLogZ, copied so the driver can confirm the new
// constants produce a non-degenerate scaled output.
double SoftLogZ(double z) {
    return (z >= 0.0) ? std::log1p(z) : -std::log1p(-z);
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 2) {
        std::cerr << "usage: " << argv[0] << " <closes.csv> [rr_out.csv]\n";
        return 1;
    }
    std::ifstream in(argv[1]);
    if (!in) { std::cerr << "cannot open " << argv[1] << "\n"; return 1; }

    std::vector<float> closes;
    std::vector<long long> stamps;
    std::string line;
    std::getline(in, line);  // header
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        std::istringstream ss(line);
        std::string tsField, closeField;
        if (!std::getline(ss, tsField, ',')) continue;
        if (!std::getline(ss, closeField, ',')) continue;
        stamps.push_back(std::atoll(tsField.c_str()));
        closes.push_back(static_cast<float>(std::atof(closeField.c_str())));
    }
    std::cerr << "bars read: " << closes.size() << "\n";

    std::ofstream out;
    const bool writeCsv = (argc >= 3);
    if (writeCsv) { out.open(argv[2]); out << "timestamp_us,recurrence_rate\n"; }

    float calibratedEpsilon = 0.0f;
    int lastCalibrationBarIndex = 0;
    float lastValidRR = 0.0f;
    size_t degenerateCount = 0, recalibrations = 0;
    std::vector<double> rrSeries;
    rrSeries.reserve(closes.size());

    for (int idx = kLookbackN; idx < static_cast<int>(closes.size()); ++idx) {
        float minP = closes[static_cast<size_t>(idx)], maxP = minP;
        for (int i = 0; i < kLookbackN; ++i) {
            const float p = closes[static_cast<size_t>(idx - i)];
            if (p < minP) minP = p;
            if (p > maxP) maxP = p;
        }
        if ((maxP - minP) <= 0.00001f) {
            ++degenerateCount;
            rrSeries.push_back(lastValidRR);
            if (writeCsv) out << stamps[static_cast<size_t>(idx)] << "," << lastValidRR << "\n";
            continue;
        }

        if (calibratedEpsilon <= 0.0f || lastCalibrationBarIndex <= 0 ||
            (idx - lastCalibrationBarIndex) >= kRecalibrationBars) {
            std::array<float, kLookbackN> windowPrices{};
            for (int i = 0; i < kLookbackN; ++i) {
                windowPrices[static_cast<size_t>(i)] = closes[static_cast<size_t>(idx - i)];
            }
            calibratedEpsilon = static_cast<float>(
                SelectEpsilonForTargetRecurrenceRate(windowPrices.data(), kLookbackN, kTargetRR));
            lastCalibrationBarIndex = idx;
            ++recalibrations;
        }

        const float epsilon = calibratedEpsilon;
        int recurCount = kLookbackN;
        for (int i = 0; i < kLookbackN; ++i) {
            const float p1 = closes[static_cast<size_t>(idx - i)];
            for (int j = i + 1; j < kLookbackN; ++j) {
                const float p2 = closes[static_cast<size_t>(idx - j)];
                if (std::fabs(p1 - p2) < epsilon) recurCount += 2;
            }
        }
        const float rr = std::clamp(static_cast<float>(recurCount) /
                                    static_cast<float>(kLookbackN * kLookbackN), 0.0f, 1.0f);
        lastValidRR = rr;
        rrSeries.push_back(rr);
        if (writeCsv) out << stamps[static_cast<size_t>(idx)] << "," << rr << "\n";
    }

    // --- Robust location/scale, matching FeatureScaler's own convention -----
    const double median = Quantile(rrSeries, 0.5);
    std::vector<double> absDev;
    absDev.reserve(rrSeries.size());
    for (double v : rrSeries) absDev.push_back(std::fabs(v - median));
    const double mad = Quantile(absDev, 0.5);
    const double scale = 1.4826 * mad;

    std::printf("samples                : %zu\n", rrSeries.size());
    std::printf("degenerate carry-fwd   : %zu\n", degenerateCount);
    std::printf("epsilon recalibrations : %zu\n", recalibrations);
    std::printf("min / max              : %.6f / %.6f\n",
                *std::min_element(rrSeries.begin(), rrSeries.end()),
                *std::max_element(rrSeries.begin(), rrSeries.end()));
    std::printf("p01 p05 p25 p50 p75 p95 p99 : %.4f %.4f %.4f %.4f %.4f %.4f %.4f\n",
                Quantile(rrSeries, 0.01), Quantile(rrSeries, 0.05), Quantile(rrSeries, 0.25),
                median, Quantile(rrSeries, 0.75), Quantile(rrSeries, 0.95), Quantile(rrSeries, 0.99));
    std::printf("distinct values        : %zu\n", [&] {
        std::vector<double> s = rrSeries;
        std::sort(s.begin(), s.end());
        return static_cast<size_t>(std::distance(s.begin(), std::unique(s.begin(), s.end())));
    }());
    std::printf("MEDIAN (center)        : %.6f\n", median);
    std::printf("MAD                    : %.6f\n", mad);
    std::printf("1.4826*MAD (scale)     : %.6f\n", scale);

    // --- Degeneracy check on the resulting scaled output --------------------
    auto report = [&](const char* label, double center, double sc) {
        std::vector<double> zs;
        zs.reserve(rrSeries.size());
        for (double v : rrSeries) {
            double z = (v - center) / std::max(sc, 1e-6);
            z = std::clamp(z, -6.0, 6.0);  // FeatureScaler STATE_WINSOR_SIGMA
            zs.push_back(SoftLogZ(z));
        }
        std::vector<double> s = zs;
        std::sort(s.begin(), s.end());
        const size_t distinct =
            static_cast<size_t>(std::distance(s.begin(), std::unique(s.begin(), s.end())));
        std::printf("[%s] center=%.4f scale=%.4f -> softlogz p01=%.4f p50=%.4f p99=%.4f "
                    "span(p01..p99)=%.4f min=%.4f max=%.4f distinct=%zu\n",
                    label, center, sc, Quantile(zs, 0.01), Quantile(zs, 0.5), Quantile(zs, 0.99),
                    Quantile(zs, 0.99) - Quantile(zs, 0.01), s.front(), s.back(), distinct);
    };
    report("OLD 2026-07-23", 0.329, 0.083);
    report("NEW re-derived ", median, scale);

    return 0;
}
