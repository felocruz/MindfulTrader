// activity_clock_bv_comparison.cpp -- first empirical test of the decisive pivot recorded in
// docs/superpowers/specs/2026-09-05-activity-clock-triple-barrier-reformulation-spec.md:
// compares Wilder's calendar-clock ATR against a jump-robust scale reference (Bipower Variation,
// Barndorff-Nielsen & Shephard 2004/2006) computed on the imbalance/activity clock
// (ImbalanceBarEngine), running BOTH side by side over the same real tick stream so their
// behavior around the same real historical events can be compared directly.
//
// Per Gemini's CLAUDE_BRIEF_137_REPLY (independent review of the earlier Huber-in-log-space/CUSUM
// ATR-robustification attempt, which failed 3 real empirical tests): "Option B... resolves both
// jump-robustness and fast reactivity natively... without requiring manual CUSUM state machines."
// This tool tests that claim directly, rather than assuming it.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   -Iinclude tools/observation_vector/activity_clock_bv_comparison.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/activity_clock_bv_comparison
// Usage: ./tools/bin/activity_clock_bv_comparison \
//   --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet \
//   [--bar-minutes 15] [--atr-period 10] [--imbalance-threshold 50] [--bv-window 20] [--max-rss-mb 4096]

#include "ImbalanceBarEngine.h"
#include "market_data_io.h"
#include "../ToolProgressLogger.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kTotalTicksEstimate = 471'930'891;

// Wilder's classic EMA-of-True-Range -- the calendar-clock baseline, unchanged from
// atr_robustness_comparison.cpp (kept in that file, reused here for the side-by-side comparison).
struct WilderAtr {
    int period = 10;
    double value = 0.0;
    bool seeded = false;

    double Update(double tr) {
        if (!seeded) {
            value = tr;
            seeded = true;
        } else {
            value = value + (tr - value) / static_cast<double>(period);
        }
        return value;
    }
};

// Recursive (EWMA) bipower variation -- replaces a fixed-N rolling window of returns with an
// exponentially-decaying one, avoiding the discrete window-exit discontinuity a fixed window
// suffers when a large historical return ages out (an old jump vanishes from the estimate in
// one step instead of decaying smoothly). lambda = 2/(N+1) matches the fixed window's own N as
// an "effective" EWMA span, so --bv-window remains a like-for-like knob.
struct EwmaBipowerVariation {
    double lambda = 2.0 / 21.0;
    double bv = 0.0;
    bool seeded = false;
    double prevAbsReturn = -1.0;  // sentinel: no prior return observed yet

    void Update(double r) {
        const double absR = std::fabs(r);
        if (prevAbsReturn >= 0.0) {
            constexpr double kHalfPi = 1.5707963267948966;
            const double instantaneous = kHalfPi * prevAbsReturn * absR;
            if (!seeded) { bv = instantaneous; seeded = true; }
            else { bv = bv + lambda * (instantaneous - bv); }
        }
        prevAbsReturn = absR;
    }
};

struct SeriesStats {
    std::vector<double> values;
    void Record(double v) { if (std::isfinite(v)) values.push_back(v); }
    double Percentile(double p) const {
        if (values.empty()) return 0.0;
        std::vector<double> sorted = values;
        std::sort(sorted.begin(), sorted.end());
        const double idx = p * static_cast<double>(sorted.size() - 1);
        const std::size_t lo = static_cast<std::size_t>(std::floor(idx));
        const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
        const double frac = idx - static_cast<double>(lo);
        return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
    }
    double Mean() const {
        if (values.empty()) return 0.0;
        double sum = 0.0;
        for (double v : values) sum += v;
        return sum / static_cast<double>(values.size());
    }
};

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --ticks-parquet PATH [--bar-minutes 15] [--atr-period 10]\n"
        "  [--imbalance-threshold 50] [--bv-window 20] [--max-rss-mb 4096]\n"
        "  [--sweep-thresholds 50,200,1000,5000,20000]  (single-pass calibration mode)\n",
        argv0);
}

// Single-pass threshold calibration: sweeps N candidate thresholds over ONE stream of the
// tick file, avoiding a re-read per candidate (the dominant cost). DOD-conscious, purpose-built
// for this narrow question (bar count + average duration only) rather than reusing the full
// ImbalanceBarEngine:
//   - Struct-of-Arrays, not array-of-structs: each per-candidate field (cumulative imbalance,
//     bar count, last-bar-close timestamp) is its own flat, contiguous array, so the inner loop
//     over candidates walks small, cache-friendly arrays rather than striding through full
//     ImbalanceBarEngine objects (each of which carries a 500-capacity RingBuffer<float> this
//     calibration question never needs -- allocating/touching that per candidate is pure waste
//     here).
//   - No barIndex/previous-ask-bid bookkeeping: the established calling convention elsewhere in
//     this codebase (mean_rev_z_variant_comparison.cpp) already always passes a monotonically
//     distinct barIndex per tick, which makes ImbalanceBarEngine::OnTick's own "reset if barIndex
//     changed" branch fire on every single call -- i.e. that machinery degenerates to a pure
//     per-tick pass-through in every real use. Skipped entirely here; the per-tick signed delta
//     is just askVolume-bidVolume directly, computed once and reused across all candidates.
int RunThresholdSweep(const std::string& ticksPath, const std::vector<float>& thresholds) {
    ToolProgressLogger progress("activity_clock_bv_comparison_sweep");
    progress.SetScope("sweep, " + std::to_string(thresholds.size()) + " candidates, DOD/SoA");
    progress.Log("streaming from " + ticksPath);
    constexpr std::size_t kProgressEveryNTicks = 5'000'000;

    const std::size_t k = thresholds.size();
    std::vector<float> cumulativeImbalance(k, 0.0f);
    std::vector<std::size_t> barCount(k, 0);
    std::vector<std::int64_t> lastBarCloseTs(k, -1);
    std::int64_t firstTs = -1;
    std::size_t ticksProcessed = 0;

    try {
        StreamTicksFullParquet(ticksPath, [&](std::int64_t ts, double price, std::int64_t /*volume*/,
                                               std::int64_t askVolume, std::int64_t bidVolume, bool /*isNewContract*/) {
            ++ticksProcessed;
            if (ticksProcessed % kProgressEveryNTicks == 0) progress.LogProgress(ticksProcessed, kTotalTicksEstimate);
            if (price <= 0.0) return;
            if (firstTs < 0) firstTs = ts;

            const float signedDelta = static_cast<float>(askVolume - bidVolume);  // computed ONCE, reused across all candidates
            for (std::size_t i = 0; i < k; ++i) {
                cumulativeImbalance[i] += signedDelta;
                if (std::fabs(cumulativeImbalance[i]) >= thresholds[i]) {
                    cumulativeImbalance[i] = 0.0f;
                    ++barCount[i];
                    lastBarCloseTs[i] = ts;
                }
            }
        });
    } catch (const std::exception& e) {
        progress.Log(std::string("aborted: ") + e.what());
        return 1;
    }

    progress.Log("streaming done (" + std::to_string(ticksProcessed) + " ticks), writing sweep report");
    char line[256];
    std::snprintf(line, sizeof(line), "%12s  %14s  %16s", "threshold", "bar-count", "avg-duration-sec");
    std::puts(line); progress.Log(line);
    for (std::size_t i = 0; i < k; ++i) {
        const double totalSpanSec = (lastBarCloseTs[i] > firstTs) ? static_cast<double>(lastBarCloseTs[i] - firstTs) / 1e6 : 0.0;
        const double avgDurationSec = (barCount[i] > 0) ? totalSpanSec / static_cast<double>(barCount[i]) : 0.0;
        std::snprintf(line, sizeof(line), "%12.1f  %14zu  %16.4f", thresholds[i], barCount[i], avgDurationSec);
        std::puts(line); progress.Log(line);
    }
    return 0;
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticksPath;
    int barMinutes = 15;
    int atrPeriod = 10;
    float imbalanceThreshold = 50.0f;
    int bvWindow = 20;
    std::size_t maxRssMB = 0;
    std::string sweepThresholdsCsv;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--ticks-parquet") ticksPath = next("--ticks-parquet");
        else if (arg == "--bar-minutes") barMinutes = std::stoi(next("--bar-minutes"));
        else if (arg == "--atr-period") atrPeriod = std::stoi(next("--atr-period"));
        else if (arg == "--imbalance-threshold") imbalanceThreshold = std::stof(next("--imbalance-threshold"));
        else if (arg == "--bv-window") bvWindow = std::stoi(next("--bv-window"));
        else if (arg == "--max-rss-mb") maxRssMB = std::stoull(next("--max-rss-mb"));
        else if (arg == "--sweep-thresholds") sweepThresholdsCsv = next("--sweep-thresholds");
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); PrintUsage(argv[0]); return 1; }
    }
    if (ticksPath.empty()) { PrintUsage(argv[0]); return 1; }

    if (!sweepThresholdsCsv.empty()) {
        std::vector<float> thresholds;
        std::size_t pos = 0;
        while (pos < sweepThresholdsCsv.size()) {
            const std::size_t comma = sweepThresholdsCsv.find(',', pos);
            const std::string token = sweepThresholdsCsv.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos);
            thresholds.push_back(std::stof(token));
            if (comma == std::string::npos) break;
            pos = comma + 1;
        }
        return RunThresholdSweep(ticksPath, thresholds);
    }

    ToolProgressLogger progress("activity_clock_bv_comparison");
    progress.SetScope("bar-minutes=" + std::to_string(barMinutes) + " atr-period=" + std::to_string(atrPeriod) +
                       " imbalance-threshold=" + std::to_string(imbalanceThreshold) + " bv-window=" + std::to_string(bvWindow));
    progress.Log("streaming from " + ticksPath);
    constexpr std::size_t kProgressEveryNTicks = 5'000'000;

    // --- Calendar clock: Wilder ATR on bar-minutes bars ---
    WilderAtr wilder;
    wilder.period = atrPeriod;
    const long long barUs = static_cast<long long>(barMinutes) * 60LL * 1'000'000LL;
    long long curBucket = -1;
    double curHigh = -1e30, curLow = 1e30, curClose = 0.0;
    double prevClose = 0.0;
    bool havePrevClose = false;
    double latestWilderAtr = 0.0;

    // --- Activity clock: ImbalanceBarEngine -> EWMA BV-derived scale (points, via sqrt(BV)*price) ---
    ImbalanceBarEngine engine;
    engine.SetImbalanceThreshold(imbalanceThreshold);
    EwmaBipowerVariation ewmaBv;
    ewmaBv.lambda = 2.0 / (static_cast<double>(bvWindow) + 1.0);
    std::size_t completedBefore = 0;
    long long tickIndex = 0;

    struct JointSample {
        std::int64_t ts; double price;
        double wilderAtrCompleted;  // stale until the calendar bar actually closes (old behavior)
        double wilderAtrIntrabar;   // TR-so-far on the still-forming calendar bar, Wilder-blended
        double bvScale; std::size_t imbBarCount;
    };
    std::vector<JointSample> joint;  // one entry per completed imbalance bar

    SeriesStats wilderAtBvClose, wilderAtBvCloseIntrabar, bvScaleStats, ratioStats, ratioStatsIntrabar;
    std::size_t ticksProcessed = 0;
    double largestTr = -1.0;
    std::size_t largestTrJointIndex = 0;

    try {
        StreamTicksFullParquet(ticksPath, [&](std::int64_t ts, double price, std::int64_t /*volume*/,
                                               std::int64_t askVolume, std::int64_t bidVolume, bool /*isNewContract*/) {
            ++ticksProcessed;
            if (ticksProcessed % kProgressEveryNTicks == 0) {
                progress.LogProgress(ticksProcessed, kTotalTicksEstimate);
                if (maxRssMB > 0) progress.CheckMemoryBudget(maxRssMB);
            }
            if (price <= 0.0) return;

            // Calendar clock update.
            const long long bucket = ts / barUs;
            if (curBucket == -1) {
                curBucket = bucket;
                curHigh = curLow = curClose = price;
            } else if (bucket != curBucket) {
                const double barRange = curHigh - curLow;
                double tr = barRange;
                if (havePrevClose) {
                    tr = std::max({barRange, std::fabs(curHigh - prevClose), std::fabs(curLow - prevClose)});
                }
                latestWilderAtr = wilder.Update(tr);
                if (tr > largestTr) largestTr = tr;  // tracked for the trace below
                prevClose = curClose;
                havePrevClose = true;
                curBucket = bucket;
                curHigh = curLow = price;
            }
            curClose = price;
            curHigh = std::max(curHigh, price);
            curLow = std::min(curLow, price);

            // Intra-bar Wilder preview: "what would ATR read right now" using the still-forming
            // bar's TR-so-far. Production's sc.ATR() already behaves this way (AutoLoop=1
            // re-evaluates every tick against the live-updating current-bar array slot) -- this
            // offline tool previously only updated latestWilderAtr at calendar-bucket close, which
            // is NOT what production does, so it was an unfair handicap in the earlier comparison.
            double wilderAtrIntrabar = latestWilderAtr;
            if (wilder.seeded) {
                const double barRangeSoFar = curHigh - curLow;
                double trSoFar = barRangeSoFar;
                if (havePrevClose) {
                    trSoFar = std::max({barRangeSoFar, std::fabs(curHigh - prevClose), std::fabs(curLow - prevClose)});
                }
                wilderAtrIntrabar = wilder.value + (trSoFar - wilder.value) / static_cast<double>(wilder.period);
            }

            // Activity clock update -- same barIndex-always-different pattern already validated
            // in mean_rev_z_variant_comparison.cpp (degenerates OnTick's internal delta to a
            // direct pass-through of this tick's own raw ask/bid volume, which is what this
            // dataset's per-tick ask_volume/bid_volume columns already represent).
            engine.OnTickWithPrice(static_cast<int>(tickIndex++), static_cast<float>(askVolume),
                                    static_cast<float>(bidVolume), static_cast<float>(price));
            const std::size_t completedNow = engine.GetCompletedBarCount();
            if (completedNow != completedBefore) {
                float rawReturn[1];
                if (engine.GetImbalanceBarReturns(1, rawReturn) == 1) {
                    ewmaBv.Update(static_cast<double>(rawReturn[0]));
                }
                completedBefore = completedNow;

                if (ewmaBv.seeded && wilder.seeded) {
                    const double bvScale = std::sqrt(std::max(ewmaBv.bv, 0.0)) * price;  // percentage-vol -> price points

                    joint.push_back({ts, price, latestWilderAtr, wilderAtrIntrabar, bvScale, completedNow});
                    wilderAtBvClose.Record(latestWilderAtr);
                    wilderAtBvCloseIntrabar.Record(wilderAtrIntrabar);
                    bvScaleStats.Record(bvScale);
                    if (latestWilderAtr > 1e-9) ratioStats.Record(bvScale / latestWilderAtr);
                    if (wilderAtrIntrabar > 1e-9) ratioStatsIntrabar.Record(bvScale / wilderAtrIntrabar);
                }
            }
        });
    } catch (const std::exception& e) {
        progress.Log(std::string("aborted: ") + e.what());
        return 1;
    }

    progress.Log("streaming done (" + std::to_string(ticksProcessed) + " ticks, " +
                 std::to_string(joint.size()) + " joint imbalance-bar/ATR samples), writing final report");

    char line[512];
    std::snprintf(line, sizeof(line), "=== Activity-Clock Bipower-Variation vs. Calendar-Clock Wilder ATR ===");
    std::puts(line); progress.Log(line);
    std::snprintf(line, sizeof(line), "bar-minutes=%d atr-period=%d imbalance-threshold=%.1f bv-window=%d joint-samples=%zu",
                  barMinutes, atrPeriod, imbalanceThreshold, bvWindow, joint.size());
    std::puts(line); progress.Log(line);
    std::snprintf(line, sizeof(line), "Wilder ATR completed-bar-only:        mean=%.4f p1=%.4f p50=%.4f p99=%.4f",
                  wilderAtBvClose.Mean(), wilderAtBvClose.Percentile(0.01), wilderAtBvClose.Percentile(0.50), wilderAtBvClose.Percentile(0.99));
    std::puts(line); progress.Log(line);
    std::snprintf(line, sizeof(line), "Wilder ATR intra-bar preview:         mean=%.4f p1=%.4f p50=%.4f p99=%.4f",
                  wilderAtBvCloseIntrabar.Mean(), wilderAtBvCloseIntrabar.Percentile(0.01), wilderAtBvCloseIntrabar.Percentile(0.50), wilderAtBvCloseIntrabar.Percentile(0.99));
    std::puts(line); progress.Log(line);
    std::snprintf(line, sizeof(line), "BV-derived scale (points, EWMA):      mean=%.4f p1=%.4f p50=%.4f p99=%.4f",
                  bvScaleStats.Mean(), bvScaleStats.Percentile(0.01), bvScaleStats.Percentile(0.50), bvScaleStats.Percentile(0.99));
    std::puts(line); progress.Log(line);
    std::snprintf(line, sizeof(line), "Ratio (BV-scale/Wilder completed):    mean=%.4f p1=%.4f p50=%.4f p99=%.4f",
                  ratioStats.Mean(), ratioStats.Percentile(0.01), ratioStats.Percentile(0.50), ratioStats.Percentile(0.99));
    std::puts(line); progress.Log(line);
    std::snprintf(line, sizeof(line), "Ratio (BV-scale/Wilder intra-bar):    mean=%.4f p1=%.4f p50=%.4f p99=%.4f",
                  ratioStatsIntrabar.Mean(), ratioStatsIntrabar.Percentile(0.01), ratioStatsIntrabar.Percentile(0.50), ratioStatsIntrabar.Percentile(0.99));
    std::puts(line); progress.Log(line);

    // Find the joint sample closest to the largest real True Range bar's own timestamp isn't
    // directly tracked here (calendar bar boundary times differ from imbalance bar close times) --
    // instead, find the largest single-imbalance-bar-to-bar jump in bvScale itself as the activity
    // clock's own "largest event" analogue, and trace both series around it.
    double largestBvJump = -1.0;
    std::size_t largestBvJumpIndex = 0;
    for (std::size_t i = 1; i < joint.size(); ++i) {
        const double jump = std::fabs(joint[i].bvScale - joint[i - 1].bvScale);
        if (jump > largestBvJump) { largestBvJump = jump; largestBvJumpIndex = i; }
    }
    if (largestBvJumpIndex < joint.size()) {
        std::snprintf(line, sizeof(line), "\n=== Trace around the largest single-step BV-scale jump (index %zu) ===", largestBvJumpIndex);
        std::puts(line); progress.Log(line);
        std::snprintf(line, sizeof(line), "%6s  %14s  %12s  %12s  %12s  %10s", "offset", "price", "WilderCompl", "WilderIntra", "BVscale", "imbBarCt");
        std::puts(line); progress.Log(line);
        const std::size_t start = (largestBvJumpIndex >= 5) ? largestBvJumpIndex - 5 : 0;
        const std::size_t end = std::min(joint.size(), largestBvJumpIndex + 16);
        for (std::size_t i = start; i < end; ++i) {
            const long long offset = static_cast<long long>(i) - static_cast<long long>(largestBvJumpIndex);
            std::snprintf(line, sizeof(line), "%6lld  %14.4f  %12.4f  %12.4f  %12.4f  %10zu",
                          offset, joint[i].price, joint[i].wilderAtrCompleted, joint[i].wilderAtrIntrabar, joint[i].bvScale, joint[i].imbBarCount);
            std::puts(line); progress.Log(line);
        }
    }

    return 0;
}
