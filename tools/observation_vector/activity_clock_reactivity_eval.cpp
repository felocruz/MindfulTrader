// activity_clock_reactivity_eval.cpp -- systematic (not N=1 anecdotal) reactivity comparison
// between activity-clock Bipower Variation (BV) and calendar-clock Wilder ATR, following up
// activity_clock_bv_comparison.cpp's single-largest-jump trace (tools/RECALIBRATION_LEDGER.md
// 2026-09-18 15:22 row; docs/superpowers/specs/2026-09-05-activity-clock-triple-barrier-
// reformulation-spec.md). That one trace found Wilder's own intra-bar preview (matching live
// sc.ATR()'s every-tick re-evaluation, the fair baseline) reacted the SAME calendar bar as a real
// price shock, while BV's comparable jump landed one imbalance-bar later -- not decisive on its
// own. This tool answers the same question over the top-K largest real price shocks in the
// dataset, using an event-study design (reaction-fraction-of-eventual-move at fixed time offsets
// after each event), not a single eyeballed trace.
//
// Event definition: top-K calendar bars by True Range (bar-minutes, same convention as the
// sibling tool), independently of either estimator -- avoids circularity (picking events based on
// one estimator's own reaction would bias the comparison toward that estimator). A minimum
// bar-index gap between selected events enforces temporal independence (no two events from the
// same volatility cluster).
//
// Two-pass design: Pass 1 streams once to compute every calendar bar's TR and select the top-K
// independent events; Pass 2 streams again, maintaining both estimators live (as in the sibling
// tool) and sampling each at fixed wall-clock offsets after every selected event.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   -Iinclude tools/observation_vector/activity_clock_reactivity_eval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/activity_clock_reactivity_eval
// Usage: ./tools/bin/activity_clock_reactivity_eval \
//   --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet \
//   [--bar-minutes 15] [--atr-period 10] [--imbalance-threshold 700] [--bv-window 20] \
//   [--top-k 30] [--min-gap-bars 20] [--target-sec 1800] [--max-rss-mb 4096]

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
constexpr std::size_t kProgressEveryNTicks = 5'000'000;

// Fixed reaction-sampling offsets, seconds after each event's bar-close timestamp.
const std::vector<int> kOffsetsSec = {0, 15, 30, 60, 120, 300, 600};

// Wilder's classic EMA-of-True-Range -- calendar-clock baseline, same construction as the
// sibling activity_clock_bv_comparison.cpp (duplicated per this codebase's own established
// precedent of not sharing these small structs across standalone tools).
struct WilderAtr {
    int period = 10;
    double value = 0.0;
    bool seeded = false;
    double Update(double tr) {
        if (!seeded) { value = tr; seeded = true; }
        else { value = value + (tr - value) / static_cast<double>(period); }
        return value;
    }
};

// Recursive (EWMA) bipower variation -- identical construction to the sibling tool.
struct EwmaBipowerVariation {
    double lambda = 2.0 / 21.0;
    double bv = 0.0;
    bool seeded = false;
    double prevAbsReturn = -1.0;
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

struct BarTr {
    std::int64_t closeTs = 0;
    double tr = 0.0;
};

struct Event {
    std::int64_t ts = 0;
    double tr = 0.0;
    bool baselineSet = false;
    double baselineBv = 0.0;
    double baselineWilder = 0.0;
    std::size_t nextOffsetIdx = 0;
    std::vector<double> sampledBv;      // aligned with kOffsetsSec
    std::vector<double> sampledWilder;  // aligned with kOffsetsSec
    bool targetSet = false;
    double targetBv = 0.0;
    double targetWilder = 0.0;
};

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --ticks-parquet PATH [--bar-minutes 15] [--atr-period 10]\n"
        "  [--imbalance-threshold 700] [--bv-window 20] [--top-k 30] [--min-gap-bars 20]\n"
        "  [--target-sec 1800] [--max-rss-mb 4096]\n",
        argv0);
}

// Pass 1: compute every calendar bar's True Range + close timestamp (cheap -- a handful of bars
// per 15 minutes over the whole dataset's real span, trivially fits in memory).
std::vector<BarTr> ComputeBarTrs(const std::string& ticksPath, int barMinutes, ToolProgressLogger& progress) {
    std::vector<BarTr> bars;
    const long long barUs = static_cast<long long>(barMinutes) * 60LL * 1'000'000LL;
    long long curBucket = -1;
    double curHigh = -1e30, curLow = 1e30, curClose = 0.0, prevClose = 0.0;
    bool havePrevClose = false;
    std::size_t ticksProcessed = 0;

    StreamTicksFullParquet(ticksPath, [&](std::int64_t ts, double price, std::int64_t /*volume*/,
                                           std::int64_t /*askVolume*/, std::int64_t /*bidVolume*/, bool /*isNewContract*/) {
        ++ticksProcessed;
        if (ticksProcessed % kProgressEveryNTicks == 0) progress.LogProgress(ticksProcessed, kTotalTicksEstimate);
        if (price <= 0.0) return;

        const long long bucket = ts / barUs;
        if (curBucket == -1) {
            curBucket = bucket;
            curHigh = curLow = curClose = price;
        } else if (bucket != curBucket) {
            const double barRange = curHigh - curLow;
            double tr = barRange;
            if (havePrevClose) tr = std::max({barRange, std::fabs(curHigh - prevClose), std::fabs(curLow - prevClose)});
            bars.push_back({ts, tr});
            prevClose = curClose;
            havePrevClose = true;
            curBucket = bucket;
            curHigh = curLow = price;
        }
        curClose = price;
        curHigh = std::max(curHigh, price);
        curLow = std::min(curLow, price);
    });

    progress.Log("pass 1 done: " + std::to_string(ticksProcessed) + " ticks, " +
                 std::to_string(bars.size()) + " calendar bars");
    return bars;
}

// Greedy top-K selection by TR, enforcing a minimum bar-index gap between selected events so no
// two events come from the same volatility cluster (temporal independence, not just distinctness).
std::vector<Event> SelectTopKEvents(const std::vector<BarTr>& bars, int topK, int minGapBars) {
    std::vector<std::size_t> order(bars.size());
    for (std::size_t i = 0; i < order.size(); ++i) order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) { return bars[a].tr > bars[b].tr; });

    std::vector<std::size_t> selected;
    for (std::size_t idx : order) {
        if (static_cast<int>(selected.size()) >= topK) break;
        bool tooClose = false;
        for (std::size_t s : selected) {
            const long long gap = std::llabs(static_cast<long long>(idx) - static_cast<long long>(s));
            if (gap < minGapBars) { tooClose = true; break; }
        }
        if (!tooClose) selected.push_back(idx);
    }
    std::sort(selected.begin(), selected.end());  // chronological order for pass 2

    std::vector<Event> events;
    events.reserve(selected.size());
    for (std::size_t idx : selected) {
        Event e;
        e.ts = bars[idx].closeTs;
        e.tr = bars[idx].tr;
        e.sampledBv.assign(kOffsetsSec.size(), 0.0);
        e.sampledWilder.assign(kOffsetsSec.size(), 0.0);
        events.push_back(e);
    }
    return events;
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticksPath;
    int barMinutes = 15;
    int atrPeriod = 10;
    float imbalanceThreshold = 700.0f;
    int bvWindow = 20;
    int topK = 30;
    int minGapBars = 20;
    int targetSec = 1800;
    std::size_t maxRssMB = 0;

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
        else if (arg == "--top-k") topK = std::stoi(next("--top-k"));
        else if (arg == "--min-gap-bars") minGapBars = std::stoi(next("--min-gap-bars"));
        else if (arg == "--target-sec") targetSec = std::stoi(next("--target-sec"));
        else if (arg == "--max-rss-mb") maxRssMB = std::stoull(next("--max-rss-mb"));
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); PrintUsage(argv[0]); return 1; }
    }
    if (ticksPath.empty()) { PrintUsage(argv[0]); return 1; }

    ToolProgressLogger progress("activity_clock_reactivity_eval");
    progress.SetScope("bar-minutes=" + std::to_string(barMinutes) + " atr-period=" + std::to_string(atrPeriod) +
                       " imbalance-threshold=" + std::to_string(imbalanceThreshold) + " bv-window=" + std::to_string(bvWindow) +
                       " top-k=" + std::to_string(topK) + " min-gap-bars=" + std::to_string(minGapBars) +
                       " target-sec=" + std::to_string(targetSec));

    progress.Log("=== Pass 1: computing calendar bar TRs to select independent jump events ===");
    const std::vector<BarTr> bars = ComputeBarTrs(ticksPath, barMinutes, progress);
    std::vector<Event> events = SelectTopKEvents(bars, topK, minGapBars);
    progress.Log("selected " + std::to_string(events.size()) + " independent top-TR events (requested top-k=" +
                 std::to_string(topK) + ")");
    for (const auto& e : events) {
        char line[128];
        std::snprintf(line, sizeof(line), "  event ts=%lld tr=%.4f", static_cast<long long>(e.ts), e.tr);
        progress.Log(line);
    }

    progress.Log("=== Pass 2: streaming both estimators live, sampling at each event ===");
    WilderAtr wilder;
    wilder.period = atrPeriod;
    const long long barUs = static_cast<long long>(barMinutes) * 60LL * 1'000'000LL;
    long long curBucket = -1;
    double curHigh = -1e30, curLow = 1e30, curClose = 0.0, prevClose = 0.0;
    bool havePrevClose = false;
    double latestWilderAtr = 0.0;

    ImbalanceBarEngine engine;
    engine.SetImbalanceThreshold(imbalanceThreshold);
    EwmaBipowerVariation ewmaBv;
    ewmaBv.lambda = 2.0 / (static_cast<double>(bvWindow) + 1.0);
    std::size_t completedBefore = 0;
    long long tickIndex = 0;

    double prevBvScale = 0.0, prevWilderIntrabar = 0.0;
    std::size_t curEvent = 0;
    std::size_t ticksProcessed = 0;

    StreamTicksFullParquet(ticksPath, [&](std::int64_t ts, double price, std::int64_t /*volume*/,
                                           std::int64_t askVolume, std::int64_t bidVolume, bool /*isNewContract*/) {
        ++ticksProcessed;
        if (ticksProcessed % kProgressEveryNTicks == 0) {
            progress.LogProgress(ticksProcessed, kTotalTicksEstimate);
            if (maxRssMB > 0) progress.CheckMemoryBudget(maxRssMB);
        }
        if (price <= 0.0) return;

        // Calendar clock (Wilder), identical construction to the sibling tool.
        const long long bucket = ts / barUs;
        if (curBucket == -1) {
            curBucket = bucket;
            curHigh = curLow = curClose = price;
        } else if (bucket != curBucket) {
            const double barRange = curHigh - curLow;
            double tr = barRange;
            if (havePrevClose) tr = std::max({barRange, std::fabs(curHigh - prevClose), std::fabs(curLow - prevClose)});
            latestWilderAtr = wilder.Update(tr);
            prevClose = curClose;
            havePrevClose = true;
            curBucket = bucket;
            curHigh = curLow = price;
        }
        curClose = price;
        curHigh = std::max(curHigh, price);
        curLow = std::min(curLow, price);

        double wilderIntrabar = latestWilderAtr;
        if (wilder.seeded) {
            const double barRangeSoFar = curHigh - curLow;
            double trSoFar = barRangeSoFar;
            if (havePrevClose) trSoFar = std::max({barRangeSoFar, std::fabs(curHigh - prevClose), std::fabs(curLow - prevClose)});
            wilderIntrabar = wilder.value + (trSoFar - wilder.value) / static_cast<double>(wilder.period);
        }

        // Activity clock (BV), identical construction to the sibling tool.
        engine.OnTickWithPrice(static_cast<int>(tickIndex++), static_cast<float>(askVolume),
                                static_cast<float>(bidVolume), static_cast<float>(price));
        const std::size_t completedNow = engine.GetCompletedBarCount();
        if (completedNow != completedBefore) {
            float rawReturn[1];
            if (engine.GetImbalanceBarReturns(1, rawReturn) == 1) ewmaBv.Update(static_cast<double>(rawReturn[0]));
            completedBefore = completedNow;
        }
        const double bvScale = ewmaBv.seeded ? std::sqrt(std::max(ewmaBv.bv, 0.0)) * price : 0.0;

        // Event sampling: process events strictly in chronological order (non-overlapping windows
        // guaranteed by min-gap-bars), using the tick BEFORE the event crossed as the baseline.
        while (curEvent < events.size()) {
            Event& e = events[curEvent];
            if (ts < e.ts) break;  // haven't reached this event yet
            if (!e.baselineSet) {
                e.baselineBv = prevBvScale;
                e.baselineWilder = prevWilderIntrabar;
                e.baselineSet = true;
            }
            bool advanced = false;
            while (e.nextOffsetIdx < kOffsetsSec.size() && ts >= e.ts + static_cast<std::int64_t>(kOffsetsSec[e.nextOffsetIdx]) * 1'000'000LL) {
                e.sampledBv[e.nextOffsetIdx] = bvScale;
                e.sampledWilder[e.nextOffsetIdx] = wilderIntrabar;
                ++e.nextOffsetIdx;
                advanced = true;
            }
            if (!e.targetSet && ts >= e.ts + static_cast<std::int64_t>(targetSec) * 1'000'000LL) {
                e.targetBv = bvScale;
                e.targetWilder = wilderIntrabar;
                e.targetSet = true;
                ++curEvent;  // this event is fully processed, move to the next
                continue;
            }
            if (!advanced) break;  // nothing more to do for this event on this tick
        }

        prevBvScale = bvScale;
        prevWilderIntrabar = wilderIntrabar;
    });

    progress.Log("pass 2 done: " + std::to_string(ticksProcessed) + " ticks");

    // Some trailing events near the end of the dataset may never reach --target-sec; drop them
    // from the aggregate (report how many, don't silently reduce the denominator unexplained).
    std::size_t usable = 0;
    for (const auto& e : events) if (e.targetSet) ++usable;
    progress.Log("=== Reaction-fraction-of-eventual-move, averaged over " + std::to_string(usable) +
                 "/" + std::to_string(events.size()) + " events with a captured target ===");

    char line[256];
    std::snprintf(line, sizeof(line), "%10s  %14s  %10s  %14s  %10s", "offset_s", "mean_frac_BV", "n_BV", "mean_frac_Wilder", "n_Wilder");
    std::puts(line); progress.Log(line);

    for (std::size_t k = 0; k < kOffsetsSec.size(); ++k) {
        double sumBv = 0.0, sumWilder = 0.0;
        std::size_t nBv = 0, nWilder = 0;
        for (const auto& e : events) {
            if (!e.targetSet || !e.baselineSet || e.nextOffsetIdx <= k) continue;
            const double denomBv = e.targetBv - e.baselineBv;
            const double denomWilder = e.targetWilder - e.baselineWilder;
            if (std::fabs(denomBv) > 1e-9) { sumBv += (e.sampledBv[k] - e.baselineBv) / denomBv; ++nBv; }
            if (std::fabs(denomWilder) > 1e-9) { sumWilder += (e.sampledWilder[k] - e.baselineWilder) / denomWilder; ++nWilder; }
        }
        const double meanBv = nBv > 0 ? sumBv / static_cast<double>(nBv) : 0.0;
        const double meanWilder = nWilder > 0 ? sumWilder / static_cast<double>(nWilder) : 0.0;
        std::snprintf(line, sizeof(line), "%10d  %14.4f  %10zu  %14.4f  %10zu",
                      kOffsetsSec[k], meanBv, nBv, meanWilder, nWilder);
        std::puts(line); progress.Log(line);
    }

    progress.Log("=== Per-event detail (baseline / target, both estimators) ===");
    std::snprintf(line, sizeof(line), "%18s  %10s  %10s  %10s  %10s  %10s", "event_ts", "tr", "base_BV", "target_BV", "base_Wldr", "target_Wldr");
    std::puts(line); progress.Log(line);
    for (const auto& e : events) {
        std::snprintf(line, sizeof(line), "%18lld  %10.4f  %10.4f  %10.4f  %10.4f  %10.4f",
                      static_cast<long long>(e.ts), e.tr, e.baselineBv, e.targetBv, e.baselineWilder, e.targetWilder);
        std::puts(line); progress.Log(line);
    }

    return 0;
}
