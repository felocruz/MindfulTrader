// tools/observation_vector/drift_location_eval.cpp
// Offline, model-independent prototype validator for the §5.0 drift/location
// observation-vector candidate. Reads a real MES tick-level parquet
// (tools/scid_processing/scid_to_ticks_parquet.cpp's output) DIRECTLY via
// Arrow -- the first C++ tool in this codebase to read Parquet (every prior
// tool, e.g. context_to_parquet.cpp, only writes). This removes the old
// mean_rev_z_variant_comparison.py pattern's Python round-trip (polars-read
// -> custom-binary-export -> C++-read -> CSV -> Python-scores).
//
// DOD, single-pass streaming architecture (rewritten 2026-09-02 once real
// per-tick data existed at this system's real 471.9M-row scale): the
// straightforward "materialize the whole series, then loop" version this
// tool originally shipped with peaks around 26GB at that row count (full
// timestamp/price arrays, a full per-signal copy of them, one full
// forward-return array per horizon) -- more than this machine's physical
// RAM, regardless of how lazily the Parquet file itself is read. Instead:
// StreamTicksParquet feeds ticks one Parquet row group at a time (bounded,
// tens of MB) into an inline rolling drift-z-score computation (a bounded
// ring buffer of the last `window` log-returns, mirroring
// ComputeDriftZScore's own incremental sum/sum_sq exactly) and
// StreamingHitRateAccumulator (a bounded per-horizon pending-signal FIFO,
// see market_test_stats.h's own comment for the full equivalence argument
// and test_drift_location_stats.cpp for the proof against the original
// batch functions on a synthetic fixture). Peak memory is now independent
// of total row count -- bounded by one row-group buffer plus a few
// thousand pending-signal entries per horizon.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   tools/observation_vector/drift_location_eval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/drift_location_eval
#include "market_data_io.h"
#include "drift_location_stats.h"

#include <cstdio>
#include <cstdlib>
#include <deque>
#include <fstream>
#include <memory>
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

    const auto horizons = ParseHorizons(horizons_csv);
    StreamingHitRateAccumulator acc(horizons);

    // One CSV ofstream per horizon, opened up front and written to
    // incrementally as each signal resolves (via Advance's on_resolved
    // callback) -- never materializes a forward-return array just to write
    // this file, matching this whole rewrite's own no-full-materialization
    // discipline.
    std::vector<std::unique_ptr<std::ofstream>> sig_csv(horizons.size());
    if (!export_signals_dir.empty()) {
        for (std::size_t h = 0; h < horizons.size(); ++h) {
            sig_csv[h] = std::make_unique<std::ofstream>(
                export_signals_dir + "/drift_location_hitmiss_h" + std::to_string(horizons[h]) + ".csv");
            *sig_csv[h] << "hit\n";
        }
    }
    const auto on_resolved = [&](std::size_t horizon_index, bool hit) {
        if (horizon_index < sig_csv.size() && sig_csv[horizon_index]) {
            *sig_csv[horizon_index] << (hit ? 1 : 0) << "\n";
        }
    };

    // Bounded rolling state for the drift z-score -- mirrors
    // ComputeDriftZScore's incremental sum/sum_sq exactly (see that
    // function's own comment), never materializing a full log_returns array.
    double prev_price = 0.0;
    bool has_prev = false;
    std::deque<double> ret_window;
    double sum = 0.0, sum_sq = 0.0;
    std::size_t returns_seen = 0;
    std::size_t n_signals = 0;
    std::size_t n_rows = 0;

    try {
        StreamTicksParquet(ticks_path, [&](std::int64_t ts, double price) {
            ++n_rows;
            acc.Advance(ts, price, on_resolved);
            if (has_prev) {
                const double log_ret = std::log(price / prev_price);
                ret_window.push_back(log_ret);
                sum += log_ret;
                sum_sq += log_ret * log_ret;
                ++returns_seen;
                if (ret_window.size() > window) {
                    const double oldest = ret_window.front();
                    ret_window.pop_front();
                    sum -= oldest;
                    sum_sq -= oldest * oldest;
                }
                if (returns_seen >= window) {
                    const double mean = sum / static_cast<double>(window);
                    const double variance = sum_sq / static_cast<double>(window) - mean * mean;
                    const double stddev = std::sqrt(std::max(0.0, variance));
                    const double z = (stddev > 0.0) ? (mean / stddev) : 0.0;
                    acc.PushSignal(ts, price, z);
                    ++n_signals;
                }
            }
            prev_price = price;
            has_prev = true;
        });
    } catch (const std::runtime_error& e) {
        std::fprintf(stderr, "❌ %s\n", e.what());
        return 1;
    }
    std::printf("Loaded %zu rows from %s\n", n_rows, ticks_path.c_str());
    std::printf("%zu non-warmup drift/location signals (window=%zu)\n", n_signals, window);
    std::fflush(stdout);

    const double bonferroni_alpha = 0.05 / static_cast<double>(horizons.size());

    std::printf("\n=== Predictive power (directional, continuation): forward-return sign vs. drift z-score sign ===\n");
    std::ofstream json_out;
    if (!report_json_path.empty()) {
        json_out.open(report_json_path);
        json_out << "{\n  \"window\": " << window << ",\n  \"horizons\": [\n";
    }
    for (std::size_t h_idx = 0; h_idx < horizons.size(); ++h_idx) {
        const int h = horizons[h_idx];
        const auto result = acc.Result(h_idx);
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

