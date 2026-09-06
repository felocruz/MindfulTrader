// atr_robustness_comparison.cpp -- first empirical validation pass for the
// robust-ATR spec (docs/superpowers/specs/2026-09-05-robust-atr-reformulation-spec.md,
// v3): compares Wilder's EMA-of-True-Range against a recursive Huber-in-
// log-space filter with a CUSUM-triggered adaptive learning rate, on real
// MES tick data resampled into bars.
//
// This is a FIRST-PASS validation tool, not a finished/tuned instrument --
// per that spec's own honesty framing (v3 is a synthesis of independently-
// grounded techniques, empirically unvalidated until this runs), the
// specific parameters below (k=1.345 Huber constant, referenceOffset/
// cusumThreshold, alpha_fast/alpha_slow) are first-pass defaults from the
// spec's own design, not yet fitted -- this tool exists to measure whether
// they're even in a sane ballpark, and to report the raw comparison data a
// human can use to judge that.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   -Iinclude tools/observation_vector/atr_robustness_comparison.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/atr_robustness_comparison
// Usage: ./tools/bin/atr_robustness_comparison \
//   --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet \
//   [--bar-minutes 15] [--atr-period 10] [--max-rss-mb 4096]

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
constexpr double kHuberK = 1.345;            // Huber's standard 95%-Gaussian-efficiency constant
constexpr double kMadConsistency = 1.4826;   // standard Gaussian-consistent MAD scale constant
constexpr double kSigmaFloor = 1e-4;           // guards div-by-zero on a degenerate/flat opening run
constexpr double kTrFloor = 1e-6;              // guards log(0) on a zero-range bar

// ---------------------------------------------------------------------
// Wilder's classic EMA-of-True-Range -- the baseline being compared against.
// ---------------------------------------------------------------------
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

// ---------------------------------------------------------------------
// v3 design (robust-atr-reformulation-spec.md sect. 3): recursive,
// Huber-robustified filter in log(True Range) space, with a CUSUM-triggered
// fast/slow learning rate and a Jensen's-inequality back-transform
// correction. No fixed window -- O(1) state (mu, sigma, cusumS), per
// section 3.4's Data-Oriented Design requirement.
// ---------------------------------------------------------------------
struct RobustAtrFilter {
    int period = 10;               // sets alpha_slow = 1/period, same effective memory as Wilder
    double alphaSlow = 0.1;
    double alphaFast = 0.5;        // 2/(3+1), matches CLAUDE_BRIEF_135_REPLY's proposed fast rate
    double cusumReferenceOffset = 0.5;  // CLI-configurable -- see file header, first-pass default
    double cusumThreshold = 4.0;        // CLI-configurable -- see file header, first-pass default
    double mu = 0.0;                // running location estimate of log(TR)
    double sigma = 0.0;             // running robust dispersion estimate of log(TR) innovations
    double cusumUp = 0.0;           // evidence of a sustained shift AWAY from the current (adapting) mu
    double cusumDown = 0.0;         // evidence of sustained RECOVERY toward baseline, vs. a FROZEN peak
    double frozenPeakMu = 0.0;      // mu snapshot at the moment the elevated state was entered
    bool inElevatedState = false;   // true from the moment upward fast-track fires until recovery confirmed
    bool seeded = false;
    bool fastTrackedLastUpdate = false;  // for reporting: was this bar updated at the fast rate?

    explicit RobustAtrFilter(int period_) : period(period_), alphaSlow(1.0 / period_) {}

    // Returns the back-transformed ATR_robust value (price units) for this bar.
    double Update(double tr) {
        const double logTr = std::log(std::max(tr, kTrFloor));
        if (!seeded) {
            mu = logTr;
            sigma = 0.0;  // will floor to kSigmaFloor below until real dispersion accumulates
            seeded = true;
            fastTrackedLastUpdate = false;
            return std::exp(mu);
        }

        const double sigmaForStd = std::max(sigma, kSigmaFloor);
        const double e = (logTr - mu) / sigmaForStd;
        const double absE = std::fabs(e);

        // Upward evidence: sustained deviation from the CURRENT (continuously-adapting) mu.
        // Catches sudden jumps well (a jump produces one huge |e| before mu can react), but
        // structurally under-detects a gradual multi-bar drift, since mu partially absorbs it
        // bar-by-bar before any single bar's gap grows large -- see the recovery mechanism below
        // for why a SECOND, frozen-reference test is needed, not just this one.
        cusumUp = std::max(0.0, cusumUp + absE - cusumReferenceOffset);
        const bool upTrigger = cusumUp > cusumThreshold;
        if (upTrigger && !inElevatedState) {
            inElevatedState = true;
            frozenPeakMu = mu;  // starting point only -- tracked forward below as the episode's true peak
            cusumDown = 0.0;
        }

        const double alphaForThisBar = upTrigger ? alphaFast : alphaSlow;
        const double psi = std::clamp(e, -kHuberK, kHuberK);
        const double muPrev = mu;
        mu = mu + alphaForThisBar * psi * sigmaForStd;

        // Recovery evidence: sustained decline relative to the FROZEN peak (not the moving mu) --
        // this is what lets a real, gradual multi-bar grind back to baseline accumulate properly,
        // since the reference no longer moves out from under the accumulating statistic. The peak
        // itself is tracked forward (max so far) while still elevated, so it reflects the episode's
        // actual highest point once the initial jump's own upward push has fully landed, not just
        // the pre-jump starting level.
        bool downTrigger = false;
        if (inElevatedState) {
            frozenPeakMu = std::max(frozenPeakMu, mu);
            const double recoverySignal = (frozenPeakMu - logTr) / sigmaForStd;  // positive once below the peak
            cusumDown = std::max(0.0, cusumDown + recoverySignal - cusumReferenceOffset);
            downTrigger = cusumDown > cusumThreshold;
            if (downTrigger) {
                inElevatedState = false;
                cusumUp = 0.0;
                cusumDown = 0.0;
            }
        }

        const bool fastTrack = upTrigger || downTrigger;
        fastTrackedLastUpdate = fastTrack;
        // Recovery is detected AFTER this bar's location update above (it needed the fresh mu to
        // track the running peak) -- if downTrigger just fired, re-apply this bar's update at the
        // fast rate so the recovery itself isn't left one bar behind its own detection.
        if (downTrigger) {
            mu = muPrev + alphaFast * psi * sigmaForStd;
        }

        // Robust scale update (recursive MAD-like recursion, per
        // CLAUDE_BRIEF_135_REPLY's specified formula), using the PRE-update mu
        // (matches the spec's own step ordering: sigma reacts to how far
        // today's reading was from yesterday's estimate, not today's own).
        // FIX #2 (found empirically, real-data trace): using `fastTrack` (upTrigger OR
        // downTrigger) here was WRONG -- it let sigma react at the fast rate the INSTANT the
        // jump itself hit (upTrigger fires on the very first extreme bar), which spiked sigma
        // to 1.89 and inflated the Jensen term by ~5.9x, producing RobustATR=338 on a bar where
        // real TR=363 -- nearly defeating the entire point of the Huber clip. Sigma should stay
        // conservative while evidence is still only ACCUMULATING (upTrigger alone), and only
        // snap to the fast rate once recovery is CONFIRMED (downTrigger) -- asymmetric on
        // purpose, unlike mu's own alpha selection above.
        const double sigmaAlpha = downTrigger ? alphaFast : alphaSlow;
        sigma = sigma + sigmaAlpha * (kMadConsistency * std::fabs(logTr - muPrev) - sigma);
        sigma = std::max(sigma, 0.0);

        // Jensen's-inequality correction (spec sect. 3.1a): recover an
        // arithmetic-mean-like center, not a median-like one, to preserve
        // Wilder's original conservative (tail-pulled) semantics.
        return std::exp(mu + 0.5 * sigma * sigma);
    }
};

struct SeriesStats {
    std::vector<double> values;

    void Record(double v) {
        if (std::isfinite(v)) values.push_back(v);
    }

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
        "usage: %s --ticks-parquet PATH [--bar-minutes 15] [--atr-period 10] [--max-rss-mb 4096]\n"
        "  [--cusum-offset 0.5] [--cusum-threshold 4.0] [--alpha-fast 0.5]\n",
        argv0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticksPath;
    int barMinutes = 15;
    int atrPeriod = 10;
    std::size_t maxRssMB = 0;  // 0 = no budget check
    double cusumOffset = 0.5;
    double cusumThreshold = 4.0;
    double alphaFast = 0.5;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--ticks-parquet") ticksPath = next("--ticks-parquet");
        else if (arg == "--bar-minutes") barMinutes = std::stoi(next("--bar-minutes"));
        else if (arg == "--atr-period") atrPeriod = std::stoi(next("--atr-period"));
        else if (arg == "--max-rss-mb") maxRssMB = std::stoull(next("--max-rss-mb"));
        else if (arg == "--cusum-offset") cusumOffset = std::stod(next("--cusum-offset"));
        else if (arg == "--cusum-threshold") cusumThreshold = std::stod(next("--cusum-threshold"));
        else if (arg == "--alpha-fast") alphaFast = std::stod(next("--alpha-fast"));
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); PrintUsage(argv[0]); return 1; }
    }
    if (ticksPath.empty()) {
        PrintUsage(argv[0]);
        return 1;
    }

    ToolProgressLogger progress("atr_robustness_comparison");
    progress.SetScope("bar-minutes=" + std::to_string(barMinutes) + " atr-period=" + std::to_string(atrPeriod) +
                       " cusum-offset=" + std::to_string(cusumOffset) + " cusum-threshold=" + std::to_string(cusumThreshold) +
                       " alpha-fast=" + std::to_string(alphaFast));
    progress.Log("streaming from " + ticksPath);
    constexpr std::size_t kProgressEveryNTicks = 5'000'000;

    WilderAtr wilder;
    wilder.period = atrPeriod;
    RobustAtrFilter robust(atrPeriod);
    robust.cusumReferenceOffset = cusumOffset;
    robust.cusumThreshold = cusumThreshold;
    robust.alphaFast = alphaFast;

    const long long barUs = static_cast<long long>(barMinutes) * 60LL * 1'000'000LL;

    long long curBucket = -1;
    double curHigh = -1e30, curLow = 1e30, curClose = 0.0;
    double prevClose = 0.0;
    bool havePrevClose = false;

    SeriesStats wilderStats, robustStats, ratioStats;
    std::size_t barsClosed = 0;
    std::size_t ticksProcessed = 0;

    // Track the single largest real True Range bar seen, plus both filters'
    // trajectories for the ~15 bars following it -- the concrete
    // "contamination-window behavior" evidence the validation plan asks for.
    struct TraceEntry { double tr, wilderAtr, robustAtr, muOnly, sigmaVal; bool fastTracked; };
    double largestTr = -1.0;
    std::size_t largestTrBarIndex = 0;
    std::vector<TraceEntry> allBars;  // full history retained -- bounded by real bar count (~tens of thousands), not tick count

    try {
        StreamTicksParquet(ticksPath, [&](std::int64_t ts, double price) {
            ++ticksProcessed;
            if (ticksProcessed % kProgressEveryNTicks == 0) {
                progress.LogProgress(ticksProcessed, kTotalTicksEstimate);
                if (maxRssMB > 0) progress.CheckMemoryBudget(maxRssMB);
            }
            if (price <= 0.0) return;
            const long long bucket = ts / barUs;

            if (curBucket == -1) {
                curBucket = bucket;
                curHigh = curLow = curClose = price;
                return;
            }

            if (bucket != curBucket) {
                const double barRange = curHigh - curLow;
                double tr = barRange;
                if (havePrevClose) {
                    tr = std::max({barRange, std::fabs(curHigh - prevClose), std::fabs(curLow - prevClose)});
                }

                const double wilderVal = wilder.Update(tr);
                const double robustVal = robust.Update(tr);
                ++barsClosed;

                if (wilder.seeded && robust.seeded && barsClosed > 1) {
                    wilderStats.Record(wilderVal);
                    robustStats.Record(robustVal);
                    if (wilderVal > 1e-9) ratioStats.Record(robustVal / wilderVal);
                }

                allBars.push_back({tr, wilderVal, robustVal, std::exp(robust.mu), robust.sigma, robust.fastTrackedLastUpdate});
                if (tr > largestTr) {
                    largestTr = tr;
                    largestTrBarIndex = allBars.size() - 1;
                }

                prevClose = curClose;
                havePrevClose = true;
                curBucket = bucket;
                curHigh = curLow = price;
            }

            curClose = price;
            curHigh = std::max(curHigh, price);
            curLow = std::min(curLow, price);
        });
    } catch (const std::exception& e) {
        progress.Log(std::string("aborted: ") + e.what());
        return 1;
    }

    progress.Log("streaming done (" + std::to_string(ticksProcessed) + " ticks, " +
                 std::to_string(barsClosed) + " bars closed), writing final report");

    char line[512];
    std::snprintf(line, sizeof(line), "=== ATR Robustness Comparison: Wilder EMA vs. Huber-in-log-space/CUSUM filter ===");
    std::puts(line); progress.Log(line);
    std::snprintf(line, sizeof(line), "bar-minutes=%d atr-period=%d cusum-offset=%.3f cusum-threshold=%.3f alpha-fast=%.3f bars-closed=%zu",
                  barMinutes, atrPeriod, cusumOffset, cusumThreshold, alphaFast, barsClosed);
    std::puts(line); progress.Log(line);

    std::size_t fastTrackedBars = 0;
    for (const auto& b : allBars) if (b.fastTracked) ++fastTrackedBars;
    std::snprintf(line, sizeof(line), "fast-track engaged on %zu / %zu bars (%.4f%%)",
                  fastTrackedBars, allBars.size(), 100.0 * static_cast<double>(fastTrackedBars) / static_cast<double>(std::max<std::size_t>(allBars.size(), 1)));
    std::puts(line); progress.Log(line);

    std::snprintf(line, sizeof(line),
        "Wilder ATR   : mean=%.4f  p1=%.4f  p50=%.4f  p99=%.4f",
        wilderStats.Mean(), wilderStats.Percentile(0.01), wilderStats.Percentile(0.50), wilderStats.Percentile(0.99));
    std::puts(line); progress.Log(line);
    std::snprintf(line, sizeof(line),
        "Robust filter: mean=%.4f  p1=%.4f  p50=%.4f  p99=%.4f",
        robustStats.Mean(), robustStats.Percentile(0.01), robustStats.Percentile(0.50), robustStats.Percentile(0.99));
    std::puts(line); progress.Log(line);
    std::snprintf(line, sizeof(line),
        "R-Scale Distortion Ratio (robust/wilder): mean=%.4f  p1=%.4f  p50=%.4f  p99=%.4f",
        ratioStats.Mean(), ratioStats.Percentile(0.01), ratioStats.Percentile(0.50), ratioStats.Percentile(0.99));
    std::puts(line); progress.Log(line);

    // Contamination-window trace: the single largest real True Range bar in
    // the whole history, and both filters' trajectories for the next 15 bars.
    if (largestTrBarIndex < allBars.size()) {
        std::snprintf(line, sizeof(line),
            "\n=== Contamination-window trace: largest real TR in the dataset (TR=%.4f, bar #%zu) ===",
            largestTr, largestTrBarIndex);
        std::puts(line); progress.Log(line);
        std::snprintf(line, sizeof(line), "%6s  %10s  %12s  %12s  %10s  %10s  %10s", "offset", "TR", "WilderATR", "RobustATR", "exp(mu)", "sigma", "fastTrack");
        std::puts(line); progress.Log(line);
        const std::size_t start = (largestTrBarIndex >= 2) ? largestTrBarIndex - 2 : 0;
        const std::size_t end = std::min(allBars.size(), largestTrBarIndex + 41);
        for (std::size_t i = start; i < end; ++i) {
            const auto& b = allBars[i];
            const long long offset = static_cast<long long>(i) - static_cast<long long>(largestTrBarIndex);
            std::snprintf(line, sizeof(line), "%6lld  %10.4f  %12.4f  %12.4f  %10.4f  %10.4f  %10s",
                          offset, b.tr, b.wilderAtr, b.robustAtr, b.muOnly, b.sigmaVal, b.fastTracked ? "YES" : "no");
            std::puts(line); progress.Log(line);
        }
    }

    return 0;
}
