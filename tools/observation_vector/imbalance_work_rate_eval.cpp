// imbalance_work_rate_eval.cpp -- empirical validation of the "Imbalance Yield" candidate signal
// (Y_imb = barReturn / theta_tau, docs/superpowers/specs/2026-09-06-imbalance-work-rate-spec.md
// section 2, corrected there from the source brainstorm's algebraically-degenerate
// "Work Rate" = theta*Y_imb = ΔP formula). Tests whether Y_imb's SIGN -- confirmed/efficient
// move (theta and price agree) vs absorbed/failed move (theta and price disagree) -- carries
// forward continuation/reversal information beyond naive momentum (sign of the bar's own
// return) alone, on real MES tick data.
//
// Methodology: naive-momentum hit rate (predict forward-K-bar direction = sign of the CURRENT
// bar's own return), measured separately on the "confirmed" subset (Y_imb>0) vs the "absorbed"
// subset (Y_imb<0). If Y_imb carries real information, these two subsets' hit rates should
// differ meaningfully (confirmed moves should continue more often than absorbed ones) --
// this is a genuinely new construct (docs/superpowers/specs/2026-09-06-imbalance-work-rate-spec.md
// section 1's own honesty flag: zero literature citation for this specific formula), so this is
// exactly the "does this carry information at all" bar, not a construction-correctness check.
//
// KNOWN, EXPLICITLY FLAGGED LIMITATION (same standing gap already documented in this repo's
// SCRATCHPAD.md for drift_location_eval.cpp/jump_ratio_eval.cpp): forward-K-bar windows for
// consecutive completed imbalance bars overlap heavily, so treating each bar's hit/miss as an
// independent Bernoulli trial for the Wilson CI below understates the true uncertainty. The
// reported CIs are therefore optimistic (narrower than reality) -- a block-bootstrap fix is a
// real follow-up, not done here, matching the same deferred status as those two sibling tools.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   -Iinclude tools/observation_vector/imbalance_work_rate_eval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/imbalance_work_rate_eval
// Usage: ./tools/bin/imbalance_work_rate_eval \
//   --ticks-parquet /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_ticks.parquet \
//   [--imbalance-threshold 50] [--horizons 1,5,20] [--max-rss-mb 4096]

#include "ImbalanceBarEngine.h"
#include "market_data_io.h"
#include "../ToolProgressLogger.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kTotalTicksEstimate = 471'930'891;

// 95% Wilson score interval for a proportion -- better-behaved than a normal
// approximation at small n or extreme p (Wilson 1927), no external dependency needed.
struct WilsonCI { double lower, upper; };
WilsonCI Wilson95(std::size_t hits, std::size_t n) {
    if (n == 0) return {0.0, 0.0};
    constexpr double z = 1.96;
    const double p = static_cast<double>(hits) / static_cast<double>(n);
    const double denom = 1.0 + (z * z) / static_cast<double>(n);
    const double center = (p + (z * z) / (2.0 * static_cast<double>(n))) / denom;
    const double margin = (z / denom) * std::sqrt(p * (1.0 - p) / static_cast<double>(n) +
                                                    (z * z) / (4.0 * static_cast<double>(n) * static_cast<double>(n)));
    return {center - margin, center + margin};
}

struct HitCounter {
    std::size_t hits = 0;
    std::size_t trials = 0;  // excludes ties (zero forward return)
    void Record(float predictedSign, float forwardReturn) {
        if (forwardReturn == 0.0f) return;  // tie, not scored
        ++trials;
        const bool predictedUp = predictedSign > 0.0f;
        const bool actualUp = forwardReturn > 0.0f;
        if (predictedUp == actualUp) ++hits;
    }
    double Rate() const { return trials > 0 ? static_cast<double>(hits) / static_cast<double>(trials) : 0.0; }
};

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --ticks-parquet PATH [--imbalance-threshold 50] [--horizons 1,5,20] [--max-rss-mb 4096]\n",
        argv0);
}

std::vector<int> ParseHorizons(const std::string& csv) {
    std::vector<int> out;
    std::size_t pos = 0;
    while (pos < csv.size()) {
        const std::size_t comma = csv.find(',', pos);
        out.push_back(std::stoi(csv.substr(pos, comma == std::string::npos ? std::string::npos : comma - pos)));
        if (comma == std::string::npos) break;
        pos = comma + 1;
    }
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticksPath;
    float imbalanceThreshold = 50.0f;
    std::string horizonsCsv = "1,5,20";
    std::size_t maxRssMB = 0;

    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--ticks-parquet") ticksPath = next("--ticks-parquet");
        else if (arg == "--imbalance-threshold") imbalanceThreshold = std::stof(next("--imbalance-threshold"));
        else if (arg == "--horizons") horizonsCsv = next("--horizons");
        else if (arg == "--max-rss-mb") maxRssMB = std::stoull(next("--max-rss-mb"));
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); PrintUsage(argv[0]); return 1; }
    }
    if (ticksPath.empty()) { PrintUsage(argv[0]); return 1; }
    const std::vector<int> horizons = ParseHorizons(horizonsCsv);

    ToolProgressLogger progress("imbalance_work_rate_eval");
    progress.SetScope("imbalance-threshold=" + std::to_string(imbalanceThreshold) + " horizons=" + horizonsCsv);
    progress.Log("streaming from " + ticksPath);
    constexpr std::size_t kProgressEveryNTicks = 5'000'000;

    // --- Pass 1: stream real ticks, close imbalance bars, retain every bar's own
    // return and signed imbalance magnitude (theta) in memory for the forward-looking
    // analysis in pass 2 below -- bars are orders of magnitude sparser than raw ticks,
    // so this is a modest, bounded amount of memory, not another 471.9M-row structure. ---
    ImbalanceBarEngine engine;
    engine.SetImbalanceThreshold(imbalanceThreshold);
    std::vector<float> barReturns;
    std::vector<float> barThetas;
    std::vector<std::int64_t> barCloseTimestampUs;  // for the artifact-vs-signal duration diagnostic below
    // Reserved, not grown unbounded -- real threshold-sweep data on this exact dataset
    // (tools/output/activity_clock_bv_comparison_sweep_20260906_063036.txt) measured
    // 73,877-137,002 completed bars across thresholds 600-900 at real tick density; 200K
    // gives headroom without over-allocating, avoiding push_back's reallocation-and-copy
    // churn during growth (this dominates DOD cost far more than any per-tick work below).
    constexpr std::size_t kExpectedBarCountHint = 200'000;
    barReturns.reserve(kExpectedBarCountHint);
    barThetas.reserve(kExpectedBarCountHint);
    barCloseTimestampUs.reserve(kExpectedBarCountHint);
    std::size_t completedBefore = 0;
    long long tickIndex = 0;
    std::size_t ticksProcessed = 0;

    try {
        StreamTicksFullParquet(ticksPath, [&](std::int64_t ts, double price, std::int64_t /*volume*/,
                                               std::int64_t askVolume, std::int64_t bidVolume, bool /*isNewContract*/) {
            ++ticksProcessed;
            if (ticksProcessed % kProgressEveryNTicks == 0) {
                progress.LogProgress(ticksProcessed, kTotalTicksEstimate);
                if (maxRssMB > 0) progress.CheckMemoryBudget(maxRssMB);
            }
            if (price <= 0.0) return;

            // Monotonically distinct barIndex per tick -- OnTick's own "reset if barIndex
            // changed" branch fires every call, degenerating to a direct per-tick signed-delta
            // pass-through, the same established convention as this repo's other imbalance-bar
            // tools (mean_rev_z_variant_comparison.cpp, activity_clock_bv_comparison.cpp).
            engine.OnTickWithPrice(static_cast<int>(tickIndex++), static_cast<float>(askVolume),
                                    static_cast<float>(bidVolume), static_cast<float>(price));

            const std::size_t completedNow = engine.GetCompletedBarCount();
            if (completedNow != completedBefore) {
                float ret[1];
                float theta[1];
                if (engine.GetImbalanceBarReturns(1, ret) == 1 && engine.GetImbalanceBarMagnitudes(1, theta) == 1) {
                    barReturns.push_back(ret[0]);
                    barThetas.push_back(theta[0]);
                    barCloseTimestampUs.push_back(ts);
                }
                completedBefore = completedNow;
            }
        });
    } catch (const std::exception& e) {
        progress.Log(std::string("aborted: ") + e.what());
        return 1;
    }

    progress.Log("streaming done (" + std::to_string(ticksProcessed) + " ticks, " +
                 std::to_string(barReturns.size()) + " imbalance bars closed), running forward-looking analysis");

    // --- Pass 2: in-memory, over the bar arrays only. For each horizon K, split bars into the
    // "confirmed" (Y_imb>0: theta and price direction agree) and "absorbed" (Y_imb<0: they
    // disagree) subsets, and measure the naive-momentum (predict forward direction = current
    // bar's own return sign) hit rate separately in each subset. ---
    const std::size_t n = barReturns.size();
    char line[256];
    std::snprintf(line, sizeof(line), "=== Imbalance Yield (Y_imb = barReturn/theta) forward continuation/reversal test ===");
    std::puts(line); progress.Log(line);
    std::snprintf(line, sizeof(line), "bars=%zu  imbalance-threshold=%.3f", n, imbalanceThreshold);
    std::puts(line); progress.Log(line);

    for (int K : horizons) {
        if (K < 1 || static_cast<std::size_t>(K) >= n) continue;
        HitCounter overall, confirmed, absorbed;

        for (std::size_t t = 0; t + static_cast<std::size_t>(K) < n; ++t) {
            const float theta = barThetas[t];
            if (theta == 0.0f) continue;  // guarded degenerate case, should not occur by construction
            const float yImb = barReturns[t] / theta;

            float fwd = 0.0f;
            for (std::size_t k = 1; k <= static_cast<std::size_t>(K); ++k) fwd += barReturns[t + k];

            const float predictedSign = barReturns[t];  // naive momentum: continue the current bar's own direction
            overall.Record(predictedSign, fwd);
            if (yImb > 0.0f) confirmed.Record(predictedSign, fwd);
            else if (yImb < 0.0f) absorbed.Record(predictedSign, fwd);
        }

        const auto ciOverall = Wilson95(overall.hits, overall.trials);
        const auto ciConfirmed = Wilson95(confirmed.hits, confirmed.trials);
        const auto ciAbsorbed = Wilson95(absorbed.hits, absorbed.trials);

        std::snprintf(line, sizeof(line), "\n--- horizon K=%d bars ---", K);
        std::puts(line); progress.Log(line);
        std::snprintf(line, sizeof(line), "  overall   : hit_rate=%.4f  n=%zu  95%% CI=[%.4f,%.4f]",
                      overall.Rate(), overall.trials, ciOverall.lower, ciOverall.upper);
        std::puts(line); progress.Log(line);
        std::snprintf(line, sizeof(line), "  confirmed (Y_imb>0): hit_rate=%.4f  n=%zu  95%% CI=[%.4f,%.4f]",
                      confirmed.Rate(), confirmed.trials, ciConfirmed.lower, ciConfirmed.upper);
        std::puts(line); progress.Log(line);
        std::snprintf(line, sizeof(line), "  absorbed  (Y_imb<0): hit_rate=%.4f  n=%zu  95%% CI=[%.4f,%.4f]",
                      absorbed.Rate(), absorbed.trials, ciAbsorbed.lower, ciAbsorbed.upper);
        std::puts(line); progress.Log(line);
    }

    std::puts("\n(reminder: reported CIs assume i.i.d. trials; overlapping forward windows mean");
    std::puts(" true uncertainty is wider than shown -- see this file's own header comment)");
    progress.Log("reminder: reported CIs assume i.i.d. trials -- overlapping forward windows mean "
                 "true uncertainty is wider than shown");

    // --- Pass 3: artifact-vs-signal diagnostics for the K=1 effect, per the independent Gemini
    // review of this tool's own K=1 result (confirmed 0.6457 vs absorbed 0.4038, both far from
    // 0.50) -- a single meta-order sweep spanning >1 imbalance-bar boundary would mechanically
    // produce that exact K=1-only pattern with zero real forward information. Two diagnostics:
    // (a) bin the K=1 test by inter-bar wall-clock gap -- if the edge concentrates in bar pairs
    // that close within milliseconds of each other, that is the artifact's fingerprint; (b) a
    // skip-1-bar test (predict bar t+2's own return, not t+1's) -- an artifact from a sweep
    // spanning ~2 bars would cliff-edge collapse here, whereas genuine information should decay
    // gracefully, not vanish outright. ---
    std::puts("\n=== Diagnostic: is the K=1 effect a meta-order/burst-splitting artifact? ===");
    progress.Log("=== Diagnostic: is the K=1 effect a meta-order/burst-splitting artifact? ===");

    struct DurationBucket { const char* label; std::int64_t maxGapUs; };
    const DurationBucket buckets[] = {
        {"<10ms", 10'000},
        {"10ms-100ms", 100'000},
        {"100ms-1s", 1'000'000},
        {">=1s", -1},  // sentinel: catch-all, checked last
    };
    constexpr std::size_t kNumBuckets = sizeof(buckets) / sizeof(buckets[0]);
    HitCounter bucketConfirmed[kNumBuckets];
    HitCounter bucketAbsorbed[kNumBuckets];

    for (std::size_t t = 0; t + 1 < n; ++t) {
        const float theta = barThetas[t];
        if (theta == 0.0f) continue;
        const float yImb = barReturns[t] / theta;
        const float predictedSign = barReturns[t];
        const float fwd1 = barReturns[t + 1];
        const std::int64_t gapUs = barCloseTimestampUs[t + 1] - barCloseTimestampUs[t];

        std::size_t b = kNumBuckets - 1;
        for (std::size_t i = 0; i + 1 < kNumBuckets; ++i) {
            if (gapUs < buckets[i].maxGapUs) { b = i; break; }
        }
        if (yImb > 0.0f) bucketConfirmed[b].Record(predictedSign, fwd1);
        else if (yImb < 0.0f) bucketAbsorbed[b].Record(predictedSign, fwd1);
    }

    std::puts("\n--- K=1 hit rate binned by inter-bar close gap (wall-clock) ---");
    progress.Log("K=1 hit rate binned by inter-bar close gap (wall-clock)");
    for (std::size_t i = 0; i < kNumBuckets; ++i) {
        const auto ciC = Wilson95(bucketConfirmed[i].hits, bucketConfirmed[i].trials);
        const auto ciA = Wilson95(bucketAbsorbed[i].hits, bucketAbsorbed[i].trials);
        std::snprintf(line, sizeof(line), "  gap %-11s confirmed: hit_rate=%.4f n=%zu CI=[%.4f,%.4f]",
                      buckets[i].label, bucketConfirmed[i].Rate(), bucketConfirmed[i].trials, ciC.lower, ciC.upper);
        std::puts(line); progress.Log(line);
        std::snprintf(line, sizeof(line), "  gap %-11s absorbed : hit_rate=%.4f n=%zu CI=[%.4f,%.4f]",
                      buckets[i].label, bucketAbsorbed[i].Rate(), bucketAbsorbed[i].trials, ciA.lower, ciA.upper);
        std::puts(line); progress.Log(line);
    }

    HitCounter skip1Confirmed, skip1Absorbed;
    for (std::size_t t = 0; t + 2 < n; ++t) {
        const float theta = barThetas[t];
        if (theta == 0.0f) continue;
        const float yImb = barReturns[t] / theta;
        const float predictedSign = barReturns[t];
        const float fwd2 = barReturns[t + 2];  // skip bar t+1 entirely, predict t+2's own return
        if (yImb > 0.0f) skip1Confirmed.Record(predictedSign, fwd2);
        else if (yImb < 0.0f) skip1Absorbed.Record(predictedSign, fwd2);
    }
    const auto ciSkip1C = Wilson95(skip1Confirmed.hits, skip1Confirmed.trials);
    const auto ciSkip1A = Wilson95(skip1Absorbed.hits, skip1Absorbed.trials);
    std::puts("\n--- skip-1-bar test (bar t predicts bar t+2, bar t+1 skipped entirely) ---");
    progress.Log("skip-1-bar test (bar t predicts bar t+2, bar t+1 skipped entirely)");
    std::snprintf(line, sizeof(line), "  confirmed: hit_rate=%.4f  n=%zu  95%% CI=[%.4f,%.4f]",
                  skip1Confirmed.Rate(), skip1Confirmed.trials, ciSkip1C.lower, ciSkip1C.upper);
    std::puts(line); progress.Log(line);
    std::snprintf(line, sizeof(line), "  absorbed : hit_rate=%.4f  n=%zu  95%% CI=[%.4f,%.4f]",
                  skip1Absorbed.Rate(), skip1Absorbed.trials, ciSkip1A.lower, ciSkip1A.upper);
    std::puts(line); progress.Log(line);

    return 0;
}
