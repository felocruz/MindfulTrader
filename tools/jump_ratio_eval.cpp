// tools/jump_ratio_eval.cpp
// Offline, model-independent prototype validator for the §5.1 jump/bipower-
// variation ratio observation-vector candidate (docs/superpowers/specs/
// 2026-08-29-hmm-fat-tail-observation-vector-brainstorm.md). Magnitude test
// (top/bottom decile bootstrap gap in |forward return|), not directional
// hit-rate -- jump_ratio is non-negative by construction, so there's no sign
// to test a forward return's sign against.
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
        "[--top-decile 0.10] [--report-json PATH]\n", argv0);
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
    const double idx = p / 100.0 * static_cast<double>(values.size() - 1);
    const std::size_t lo_idx = static_cast<std::size_t>(std::floor(idx));
    const std::size_t hi_idx = static_cast<std::size_t>(std::ceil(idx));
    if (lo_idx == hi_idx) return values[lo_idx];
    const double frac = idx - static_cast<double>(lo_idx);
    return values[lo_idx] * (1.0 - frac) + values[hi_idx] * frac;
}

}  // namespace

int main(int argc, char** argv) {
    std::string ticks_path, report_json_path;
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
    std::printf("\n=== Predictive power (magnitude): does high jump_ratio predict larger |forward return|? ===\n");
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
        // n_boot=200, not market_test_stats.h's default of 2000: at this
        // tool's real decile-group scale (~10% of 38.5M signals, so top_fwd/
        // bottom_fwd are each several million elements), even the fast
        // weighted-bootstrap path measures ~342ms/resample -- 2000 resamples
        // would cost ~11.4min PER horizon (~46min for all 4), confirmed by a
        // real benchmark, not an estimate. At n in the millions the
        // per-resample statistic is already extremely tightly concentrated,
        // so 200 replicates still gives a stable 2.5th/97.5th percentile
        // read for this decide-if-the-CI-excludes-0 test.
        const auto result = ComputeBootstrapMeanGapCI(top_fwd, bottom_fwd, /*n_boot=*/200);
        const bool survives = (result.ci_lo > 0.0) || (result.ci_hi < 0.0);
        std::printf("  %4dmin: n_top=%-7zu n_bot=%-7zu gap=%+.6f 95%%CI=[%+.6f,%+.6f] (%s)\n",
                    h, top_fwd.size(), bottom_fwd.size(), result.gap, result.ci_lo, result.ci_hi,
                    survives ? "SURVIVES (CI excludes 0)" : "does not survive (CI includes 0)");
        if (json_out.is_open()) {
            json_out << "    {\"horizon_minutes\": " << h << ", \"n_top\": " << top_fwd.size()
                      << ", \"n_bot\": " << bottom_fwd.size() << ", \"gap\": " << result.gap
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
