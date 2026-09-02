// tools/observation_vector/drift_location_eval.cpp
// Offline, model-independent prototype validator for the §5.0 drift/location
// observation-vector candidate. Reads lbrnet/data/raw/mes_continuous_ticks.parquet
// DIRECTLY via Arrow -- the first C++ tool in this codebase to read Parquet
// (every prior tool, e.g. context_to_parquet.cpp, only writes). This removes
// the old mean_rev_z_variant_comparison.py pattern's Python round-trip
// (polars-read -> custom-binary-export -> C++-read -> CSV -> Python-scores).
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   tools/observation_vector/drift_location_eval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/drift_location_eval
#include "market_data_io.h"
#include "drift_location_stats.h"

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
        "[--report-json PATH] [--export-signals-dir DIR]\n", argv0);
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

}  // namespace

int main(int argc, char** argv) {
    std::string ticks_path, report_json_path, export_signals_dir;
    std::size_t window = 100;
    std::string horizons_csv = "30,60,120,240";

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--ticks-parquet") ticks_path = next("--ticks-parquet");
        else if (arg == "--window") window = std::stoull(next("--window"));
        else if (arg == "--horizons") horizons_csv = next("--horizons");
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
    const auto z = ComputeDriftZScore(log_returns, window);
    // z[] is indexed against log_returns (length n-1), which is itself offset
    // by 1 from series.close/timestamp_us -- signal at return-index i corresponds
    // to price-series index i+1 (the price AFTER the return that produced z[i]).
    std::vector<std::int64_t> signal_ts;
    std::vector<double> signal_price, signal_z;
    for (std::size_t i = 0; i < z.size(); ++i) {
        if (std::isnan(z[i])) continue;
        signal_ts.push_back(series.timestamp_us[i + 1]);
        signal_price.push_back(series.close[i + 1]);
        signal_z.push_back(z[i]);
    }
    std::printf("%zu non-warmup drift/location signals (window=%zu)\n", signal_ts.size(), window);
    std::fflush(stdout);

    const auto horizons = ParseHorizons(horizons_csv);
    const double bonferroni_alpha = 0.05 / static_cast<double>(horizons.size());

    std::printf("\n=== Predictive power (directional, continuation): forward-return sign vs. drift z-score sign ===\n");
    std::ofstream json_out;
    if (!report_json_path.empty()) {
        json_out.open(report_json_path);
        json_out << "{\n  \"window\": " << window << ",\n  \"horizons\": [\n";
    }
    for (std::size_t h_idx = 0; h_idx < horizons.size(); ++h_idx) {
        const int h = horizons[h_idx];
        std::fprintf(stderr, "[progress] computing horizon=%dmin (%zu/%zu)...\n", h, h_idx + 1, horizons.size());
        const auto fwd = ComputeForwardReturns(signal_ts, signal_price, series.timestamp_us, series.close, h);
        const auto result = ComputeHitRate(fwd, signal_z);
        if (!export_signals_dir.empty()) {
            std::ofstream sig_csv(export_signals_dir + "/drift_location_hitmiss_h" + std::to_string(h) + ".csv");
            sig_csv << "hit\n";
            for (std::size_t i = 0; i < fwd.size(); ++i) {
                if (!std::isfinite(fwd[i])) continue;
                if (signal_z[i] == 0.0 || !std::isfinite(signal_z[i])) continue;
                sig_csv << (Sign(fwd[i]) == Sign(signal_z[i]) ? 1 : 0) << "\n";
            }
        }
        const bool survives = result.p_value < bonferroni_alpha;
        std::printf("  %4dmin: n=%-8zu hit_rate=%.4f 95%%CI=[%.4f,%.4f] p=%.4f (%s, alpha=%.5f for %zu tests)\n",
                    h, result.n, result.hit_rate, result.ci_lo, result.ci_hi, result.p_value,
                    survives ? "SURVIVES Bonferroni" : "does not survive Bonferroni",
                    bonferroni_alpha, horizons.size());
        if (json_out.is_open()) {
            json_out << "    {\"horizon_minutes\": " << h << ", \"n\": " << result.n
                      << ", \"hit_rate\": " << result.hit_rate
                      << ", \"ci_lo\": " << result.ci_lo << ", \"ci_hi\": " << result.ci_hi
                      << ", \"p_value\": " << result.p_value
                      << ", \"survives_bonferroni\": " << (survives ? "true" : "false") << "}"
                      << (h_idx + 1 < horizons.size() ? ",\n" : "\n");
        }
    }
    if (json_out.is_open()) {
        json_out << "  ]\n}\n";
        std::printf("\nWrote JSON report: %s\n", report_json_path.c_str());
    }
    return 0;
}
