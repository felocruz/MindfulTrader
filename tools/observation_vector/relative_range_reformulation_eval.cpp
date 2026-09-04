// relative_range_reformulation_eval.cpp -- validation-first comparison for
// relative_range (observation-vector dim 2): current production formula
// (High-Low)/ATR14-SMA vs. a proposed median-range reference (same
// ATR-dependency fix already applied to liq_fragility, Rousseeuw & Croux
// 1993 50%-breakdown-point robust scale). Real MES ticks, both formulas fed
// through the REAL FeatureScaler (dim 2 only, every other dim left at its
// neutral default -- same single-dim-in-isolation convention as
// burstiness_recalibration.cpp/amihud_liqfragility_recalibration.cpp).
//
// Production ATR formula VERIFIED against source before writing this tool
// (TripleScreen2.cpp): sc.ATR(..., Array_ImpulseATR, 14, MOVAVGTYPE_SIMPLE)
// -- True Range with a 14-period SIMPLE moving average, NOT Wilder smoothing
// and NOT period 10 (that combination belongs to a different ATR array used
// by Keltner bands/liq_fragility) -- a prior assumption this session, corrected
// here before building anything on it.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   -Iinclude tools/observation_vector/relative_range_reformulation_eval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/relative_range_reformulation_eval
// Usage: ./tools/bin/relative_range_reformulation_eval \
//   --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet

#include "FeatureScaler.h"
#include "RingBuffer.h"
#include "market_data_io.h"
#include "../ToolProgressLogger.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kTotalTicksEstimate = 471'930'891;
constexpr int kAtrPeriod = 14;        // verified: Array_ImpulseATR's real period
constexpr int kMedianRangeWindow = 30; // matches liq_fragility's own ScaleRef_W precedent

struct Stats {
    std::size_t n = 0;
    double sumAbsZ = 0.0;
    float maxAbsZ = 0.0f;
    std::size_t hitsAt6 = 0;

    void Record(float z) {
        if (!std::isfinite(z)) return;
        ++n;
        const float az = std::fabs(z);
        sumAbsZ += az;
        if (az > maxAbsZ) maxAbsZ = az;
        if (az >= 6.0f) ++hitsAt6;
    }

    void Report(const char* name, ToolProgressLogger& logger) const {
        char line[300];
        if (n == 0) {
            std::snprintf(line, sizeof(line), "%s: NO SAMPLES", name);
        } else {
            std::snprintf(line, sizeof(line), "%s: n=%zu mean|z|=%.4f max|z|=%.2f rate-at-bound(6.0)=%.4f%%",
                          name, n, sumAbsZ / static_cast<double>(n), maxAbsZ,
                          100.0 * static_cast<double>(hitsAt6) / static_cast<double>(n));
        }
        std::puts(line);
        logger.Log(line);
    }
};

}  // namespace

int main(int argc, char** argv) {
    std::string ticksPath;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--ticks-parquet") == 0 && i + 1 < argc) {
            ticksPath = argv[++i];
        }
    }
    if (ticksPath.empty()) {
        std::fprintf(stderr, "usage: %s --ticks-parquet PATH\n", argv[0]);
        return 1;
    }

    FeatureScaler fsOld, fsNew;  // two independent instances -- dim2 only, never cross-contaminate
    ToolProgressLogger progress("relative_range_reformulation_eval");
    progress.Log("streaming from " + ticksPath);
    constexpr std::size_t kProgressEveryNTicks = 5'000'000;
    constexpr long long kBarUs = 60LL * 60 * 1'000'000;  // TS2 = 60-min bars, verified

    Stats statsOld, statsNew;

    long long curBucket = -1;
    float curOpen = 0.0f, curHigh = -1e30f, curLow = 1e30f, curClose = 0.0f;
    float prevClose = 0.0f;
    bool havePrevClose = false;

    RingBuffer<float, kAtrPeriod> trueRangeHist;
    RingBuffer<float, kMedianRangeWindow> rangeHist;

    std::size_t ticksProcessed = 0;

    try {
        StreamTicksParquet(ticksPath, [&](std::int64_t ts, double price) {
            ++ticksProcessed;
            if (ticksProcessed % kProgressEveryNTicks == 0) {
                progress.LogProgress(ticksProcessed, kTotalTicksEstimate);
            }
            if (price <= 0.0) return;
            const float p = static_cast<float>(price);
            const long long bucket = ts / kBarUs;

            if (curBucket == -1) {
                curBucket = bucket;
                curOpen = curHigh = curLow = curClose = p;
                return;
            }

            if (bucket != curBucket) {
                // Close the bar: compute True Range, update ATR history, compute both ratios.
                const float barRange = curHigh - curLow;
                float trueRange = barRange;
                if (havePrevClose) {
                    trueRange = std::max({barRange, std::fabs(curHigh - prevClose), std::fabs(curLow - prevClose)});
                }
                if (trueRangeHist.size() == kAtrPeriod) trueRangeHist.pop_front();
                trueRangeHist.push_back(trueRange);
                if (rangeHist.size() == kMedianRangeWindow) rangeHist.pop_front();
                rangeHist.push_back(barRange);

                if (trueRangeHist.size() == kAtrPeriod) {
                    double sumTr = 0.0;
                    for (std::size_t i = 0; i < trueRangeHist.size(); ++i) sumTr += trueRangeHist[i];
                    const float atr14Sma = static_cast<float>(sumTr / kAtrPeriod);
                    if (atr14Sma > 0.00001f) {
                        const float relRangeOld = barRange / atr14Sma;
                        std::array<float, FeatureScaler::N_DIMS> obs{};
                        obs[MTS::Schema::Contract::kObsRelativeRange] = relRangeOld;
                        fsOld.UpdateAndNormalize(obs);
                        if (fsOld.warmedUp) statsOld.Record(fsOld.lastRawZ[MTS::Schema::Contract::kObsRelativeRange]);
                    }
                }

                if (rangeHist.size() == kMedianRangeWindow) {
                    std::array<float, kMedianRangeWindow> sorted{};
                    for (std::size_t i = 0; i < rangeHist.size(); ++i) sorted[i] = rangeHist[i];
                    std::nth_element(sorted.begin(), sorted.begin() + kMedianRangeWindow / 2, sorted.end());
                    const float medianRange = sorted[kMedianRangeWindow / 2];
                    if (medianRange > 0.00001f) {
                        const float relRangeNew = barRange / medianRange;
                        std::array<float, FeatureScaler::N_DIMS> obs{};
                        obs[MTS::Schema::Contract::kObsRelativeRange] = relRangeNew;
                        fsNew.UpdateAndNormalize(obs);
                        if (fsNew.warmedUp) statsNew.Record(fsNew.lastRawZ[MTS::Schema::Contract::kObsRelativeRange]);
                    }
                }

                prevClose = curClose;
                havePrevClose = true;
                curBucket = bucket;
                curOpen = curHigh = curLow = p;
            }

            curClose = p;
            curHigh = std::max(curHigh, p);
            curLow = std::min(curLow, p);
        });
    } catch (const std::exception& e) {
        progress.Log(std::string("aborted: ") + e.what());
        return 1;
    }

    progress.Log("streaming done (" + std::to_string(ticksProcessed) + " ticks), writing final report");
    statsOld.Report("relative_range OLD (ATR14-SMA)         ", progress);
    statsNew.Report("relative_range NEW (median-range, W=30)", progress);
    return 0;
}
