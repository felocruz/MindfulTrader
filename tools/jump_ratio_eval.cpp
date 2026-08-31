// tools/jump_ratio_eval.cpp
// Offline, model-independent prototype validator for the §5.1 jump/bipower-
// variation ratio observation-vector candidate (docs/superpowers/specs/
// 2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md). Magnitude test
// (top/bottom decile bootstrap gap in |forward return|), not directional
// hit-rate -- jump_ratio is non-negative by construction, so there's no sign
// to test a forward return's sign against.
//
// Uses ComputeBootstrapMedianGapCI (median, not mean, as the central-
// tendency statistic) -- this codebase's own established convention for
// fat-tailed data (FeatureScaler.h's RobustLocation(): "median and MAD x
// 1.4826, Taleb-consistent", applied to every observation-vector dim; see
// market_test_stats.h's ComputeBootstrapMedianGapCI for the full Kim & White
// 2004 rationale). An earlier version of this tool used the mean-based
// ComputeBootstrapMeanGapCI (matching dim_acceptance_eval.py's own
// predictive_power_magnitude() precedent) -- caught in review as defaulting
// to the weaker precedent instead of this repo's own harder-won standard.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   tools/jump_ratio_eval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/jump_ratio_eval
#include "drift_location_stats.h"  // for ComputeLogReturns
#include "jump_ratio_stats.h"
#include "market_data_io.h"
#include "market_test_stats.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace {

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --ticks-parquet PATH [--window N] [--horizons 30,60,120,240] "
        "[--top-decile 0.10] [--report-json PATH] [--export-signals-dir DIR]\n", argv0);
}

std::vector<int> ParseHorizons(const std::string& csv) {
    std::vector<int> out;
    std::stringstream ss(csv);
    std::string item;
    while (std::getline(ss, item, ',')) {
        out.push_back(std::stoi(item));
    }
    return out;
}

double Percentile(std::vector<double> values, double p) {
    std::sort(values.begin(), values.end());
    return PercentileFromSorted(values, p);
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticks_path, report_json_path, export_signals_dir;
    std::size_t window = 100;
    std::string horizons_csv = "30,60,120,240";
    double top_decile = 0.10;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--ticks-parquet") ticks_path = next("--ticks-parquet");
        else if (arg == "--window") window = std::stoull(next("--window"));
        else if (arg == "--horizons") horizons_csv = next("--horizons");
        else if (arg == "--top-decile") top_decile = std::stod(next("--top-decile"));
        else if (arg == "--report-json") report_json_path = next("--report-json");
        else if (arg == "--export-signals-dir") export_signals_dir = next("--export-signals-dir");
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); PrintUsage(argv[0]); return 1; }
    }
    if (ticks_path.empty()) {
        PrintUsage(argv[0]);
        return 1;
    }

    TickSeries series;
    try {
        series = ReadTicksParquet(ticks_path);
    } catch (const std::runtime_error& e) {
        std::fprintf(stderr, "❌ %s\n", e.what());
        return 1;
    }
    std::printf("Loaded %zu rows from %s\n", series.timestamp_us.size(), ticks_path.c_str());
    std::fflush(stdout);

    const auto log_returns = ComputeLogReturns(series.close);
    const auto jump_ratio = ComputeJumpRatio(log_returns, window);
    // jump_ratio[] is indexed against log_returns (length n-1), itself offset
    // by 1 from series.close/timestamp_us -- same convention as drift_location_eval.cpp.
    std::vector<std::int64_t> signal_ts;
    std::vector<double> signal_price, signal_jr;
    for (std::size_t i = 0; i < jump_ratio.size(); ++i) {
        if (std::isnan(jump_ratio[i])) continue;
        signal_ts.push_back(series.timestamp_us[i + 1]);
        signal_price.push_back(series.close[i + 1]);
        signal_jr.push_back(jump_ratio[i]);
    }
    std::printf("%zu non-warmup jump-ratio signals (window=%zu)\n", signal_ts.size(), window);
    std::fflush(stdout);

    const double hi_thresh = Percentile(signal_jr, 100.0 * (1.0 - top_decile));
    const double lo_thresh = Percentile(signal_jr, 100.0 * top_decile);

    const auto horizons = ParseHorizons(horizons_csv);
    std::printf("\n=== Predictive power (magnitude, median-based): does high jump_ratio predict larger |forward return|? ===\n");
    std::printf("NOTE: bootstrap CIs below treat per-tick signals as i.i.d.; real forward-return\n"
                 "signals overlap heavily (a 240min return spans thousands of adjacent ticks), so\n"
                 "true CI width is understated by an unquantified factor -- see market_test_stats.h's\n"
                 "ComputeBootstrapMedianGapCI comment for the known-limitation writeup.\n");
    std::ofstream json_out;
    if (!report_json_path.empty()) {
        json_out.open(report_json_path);
        json_out << "{\n  \"window\": " << window << ",\n  \"top_decile\": " << top_decile
                 << ",\n  \"horizons\": [\n";
    }
    for (std::size_t h_idx = 0; h_idx < horizons.size(); ++h_idx) {
        const int h = horizons[h_idx];
        std::fprintf(stderr, "[progress] computing horizon=%dmin (%zu/%zu)...\n", h, h_idx + 1, horizons.size());
        const auto fwd = ComputeForwardReturns(signal_ts, signal_price, series.timestamp_us, series.close, h);

        std::vector<double> top_fwd, bottom_fwd;
        for (std::size_t i = 0; i < fwd.size(); ++i) {
            if (!std::isfinite(fwd[i])) continue;
            if (signal_jr[i] >= hi_thresh) top_fwd.push_back(fwd[i]);
            else if (signal_jr[i] <= lo_thresh) bottom_fwd.push_back(fwd[i]);
        }
        if (top_fwd.size() < 30 || bottom_fwd.size() < 30) {
            std::printf("  %4dmin: too few samples in top/bottom decile (n_top=%zu, n_bot=%zu)\n",
                        h, top_fwd.size(), bottom_fwd.size());
            continue;
        }
        // n_boot=1000: the commonly-cited minimum replicate count for a
        // percentile bootstrap CI (Efron & Tibshirani 1993) -- an earlier
        // n_boot=200 was measured (5 seeds, n=25,000 synthetic data) to
        // produce a CI ~6.3% too narrow with +-13% run-to-run width variance
        // relative to 1000's ~2%; this run's own effect sizes are hundreds of
        // standard errors from zero, so that difference doesn't change any
        // verdict below, but 1000 is the honest choice for a tool meant to
        // also validate marginal future candidates. At this tool's real
        // decile-group scale (~10% of 38.5M signals, several million elements
        // per group), the weighted-median-bootstrap path measures
        // ~252ms/resample steady-state (re-benchmarked at the real shipped
        // group sizes, n_top=3842588/n_bot=3836836, after review found an
        // earlier ~458ms figure was inflated by amortizing the one-time sort
        // over too few resamples) -- ~1.08x ComputeBootstrapMeanGapCI's
        // ~233ms/resample at the same sizes, not the ~1.8x first claimed. At
        // n_boot=1000 that's ~4.2min/horizon, ~17min for all 4 -- matches the
        // real end-to-end run's own 15m36s wall time. No Bonferroni correction
        // across horizons here, unlike
        // drift_location_eval.cpp's directional test: this magnitude test
        // mirrors dim_acceptance_eval.py's own predictive_power_magnitude(),
        // which likewise doesn't apply one.
        const auto result = ComputeBootstrapMedianGapCI(top_fwd, bottom_fwd, /*n_boot=*/1000);
        const bool survives = (result.ci_lo > 0.0) || (result.ci_hi < 0.0);
        // Group medians, not just the gap: a high jump_ratio window (few
        // large moves among mostly-tiny ones) and a low jump_ratio window
        // (uniform-sized moves) differ in realized-volatility LEVEL by
        // construction, not only in jump content -- printing both medians
        // makes that potential confound visible rather than burying it
        // inside a single gap number.
        const double top_median = MedianAbs(top_fwd);
        const double bottom_median = MedianAbs(bottom_fwd);
        if (!export_signals_dir.empty()) {
            std::ofstream top_csv(export_signals_dir + "/jump_ratio_top_belowmedian_h" + std::to_string(h) + ".csv");
            top_csv << "below_median\n";
            for (double v : top_fwd) top_csv << (std::fabs(v) <= top_median ? 1 : 0) << "\n";
            std::ofstream bottom_csv(export_signals_dir + "/jump_ratio_bottom_belowmedian_h" + std::to_string(h) + ".csv");
            bottom_csv << "below_median\n";
            for (double v : bottom_fwd) bottom_csv << (std::fabs(v) <= bottom_median ? 1 : 0) << "\n";
        }
        std::printf("  %4dmin: n_top=%-7zu n_bot=%-7zu median|fwd|_top=%.6f median|fwd|_bot=%.6f "
                    "gap=%+.6f 95%%CI=[%+.6f,%+.6f] (%s)\n",
                    h, top_fwd.size(), bottom_fwd.size(), top_median, bottom_median,
                    result.gap, result.ci_lo, result.ci_hi,
                    survives ? "SURVIVES (CI excludes 0)" : "does not survive (CI includes 0)");
        if (json_out.is_open()) {
            json_out << "    {\"horizon_minutes\": " << h << ", \"n_top\": " << top_fwd.size()
                      << ", \"n_bot\": " << bottom_fwd.size()
                      << ", \"median_abs_fwd_top\": " << top_median
                      << ", \"median_abs_fwd_bottom\": " << bottom_median
                      << ", \"gap\": " << result.gap
                      << ", \"ci_lo\": " << result.ci_lo << ", \"ci_hi\": " << result.ci_hi
                      << ", \"survives\": " << (survives ? "true" : "false") << "}"
                      << (h_idx + 1 < horizons.size() ? ",\n" : "\n");
        }
    }
    if (json_out.is_open()) {
        json_out << "  ]\n}\n";
        std::printf("\nWrote JSON report: %s\n", report_json_path.c_str());
    }
    return 0;
}
