// tools/context_pipeline/context_validate.cpp
// Consolidates lbrnet/scripts/validate_lbr_file.py's .context-specific checks
// (MO/SS sequence integrity, per-dim NaN/Inf detection, zero/one-trap detection)
// and lbrnet/scripts/context_preflight.py's checks (per-dim distribution stats,
// inter-feature correlation, constant/chronic-zero/saturation gates) into one
// C++ tool built directly on tools/context_pipeline/context_reader.h -- both Python scripts
// already operated on exactly the same raw MO/SS data, and context_preflight.py
// itself already described its role as "supplements validate_lbr_file.py".
// No Arrow/Parquet dependency -- this tool only reports, never writes a file.
//
// Build: mamba run -n mts g++ -O2 -std=c++17 -Iinclude \
//   tools/context_pipeline/context_validate.cpp -o tools/context_validate
#include "context_reader.h"
#include "context_validate_stats.h"
#include "generated/mts_schema_contract_generated.h"

#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <vector>

namespace {

struct Accumulator {
    std::array<std::vector<float>, MTS::Schema::Contract::kObservationDim> columns;
};

void AppendPair(const PairedRecord& rec, void* user_data) {
    auto* acc = static_cast<Accumulator*>(user_data);
    const auto* obs = rec.observation.observation;
    const std::array<float, MTS::Schema::Contract::kObservationDim> values = {
        obs->log_scale_ratio(), obs->burstiness_index(), obs->relative_range(),
        obs->log_scale_expansion_ratio(), obs->vol_convexity(), obs->lempel_ziv(),
        obs->hurst_exponent(), obs->micro_asymmetry(), obs->fisher_info(),
        obs->fast_hurst_exponent(), obs->tail_index(), obs->skewness_idx(),
        obs->amihud_illiquidity(), obs->liq_fragility(), obs->fast_taleb_kurtosis(),
        obs->recurrence_rate(), obs->fractal_dim(), obs->mean_rev_z(), obs->fast_mean_rev_z(),
    };
    for (std::size_t i = 0; i < values.size(); ++i) {
        acc->columns[i].push_back(values[i]);
    }
}

std::string JsonEscape(const std::string& s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back(c);
    }
    return out;
}

void PrintUsage(const char* argv0) {
    std::fprintf(stderr,
        "usage: %s --input PATH [--mode head|tail] [--max-pairs N] [--min-pairs N] "
        "[--constant-ratio-threshold F] [--saturation-threshold F] "
        "[--chronic-zero-threshold F] [--check-zero-trap] [--check-nulls] "
        "[--check-sequence-integrity] [--corr-threshold F] [--report-json PATH] "
        "[--no-strict]\n", argv0);
}

}  // namespace

int main(int argc, char** argv) {
    std::string input_path, mode = "head", report_json_path;
    std::size_t max_pairs = 50000, min_pairs = 5000;
    double constant_ratio_threshold = 0.995, saturation_threshold = 0.98;
    double chronic_zero_threshold = 0.15, corr_threshold = 0.8;
    bool check_zero_trap = false, check_nulls = false, check_sequence_integrity = false;
    bool strict = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto next = [&](const char* flag) -> std::string {
            if (i + 1 >= argc) { std::fprintf(stderr, "missing value for %s\n", flag); std::exit(1); }
            return argv[++i];
        };
        if (arg == "--input") input_path = next("--input");
        else if (arg == "--mode") mode = next("--mode");
        else if (arg == "--max-pairs") max_pairs = std::stoull(next("--max-pairs"));
        else if (arg == "--min-pairs") min_pairs = std::stoull(next("--min-pairs"));
        else if (arg == "--constant-ratio-threshold") constant_ratio_threshold = std::stod(next("--constant-ratio-threshold"));
        else if (arg == "--saturation-threshold") saturation_threshold = std::stod(next("--saturation-threshold"));
        else if (arg == "--chronic-zero-threshold") chronic_zero_threshold = std::stod(next("--chronic-zero-threshold"));
        else if (arg == "--corr-threshold") corr_threshold = std::stod(next("--corr-threshold"));
        else if (arg == "--check-zero-trap") check_zero_trap = true;
        else if (arg == "--check-nulls") check_nulls = true;
        else if (arg == "--check-sequence-integrity") check_sequence_integrity = true;
        else if (arg == "--report-json") report_json_path = next("--report-json");
        else if (arg == "--no-strict") strict = false;
        else { std::fprintf(stderr, "unknown argument: %s\n", arg.c_str()); PrintUsage(argv[0]); return 1; }
    }
    if (input_path.empty()) { PrintUsage(argv[0]); return 1; }

    ContextFileHandle handle;
    try {
        handle = OpenContextFile(input_path);
    } catch (const std::runtime_error& e) {
        std::fprintf(stderr, "❌ %s\n", e.what());
        return 1;
    }

    Accumulator acc;
    ReadCounters counters = (mode == "tail")
        ? ReadBoundedTail(handle, max_pairs, AppendPair, &acc)
        : ReadBoundedHead(handle, max_pairs, AppendPair, &acc);
    CloseContextFile(handle);

    const std::vector<std::vector<float>> columns(acc.columns.begin(), acc.columns.end());
    const std::size_t rows_sampled = columns.empty() ? 0 : columns[0].size();
    const auto dim_stats = ComputeDimStats(columns);
    const auto corr = ComputeCorrelationMatrix(columns);
    const auto top_corr = TopCorrelationPairs(corr, 10);
    const auto high_corr = PairsAboveThreshold(corr, corr_threshold);

    std::vector<std::string> violations;
    std::vector<std::string> warnings;

    // --- context_preflight.py-derived gates ---
    if (rows_sampled < min_pairs) {
        violations.push_back(
            "Insufficient aligned pairs: sampled=" + std::to_string(rows_sampled) +
            " < min_pairs=" + std::to_string(min_pairs));
    }
    for (std::size_t i = 0; i < dim_stats.size(); ++i) {
        const auto& s = dim_stats[i];
        const std::string name = MTS::Schema::Contract::kObservationFieldNames[i];
        if (s.constant_ratio >= constant_ratio_threshold) {
            violations.push_back(
                "Dim " + std::to_string(i) + " (" + name + ") appears constant/trapped "
                "(constant_ratio=" + std::to_string(s.constant_ratio) + ")");
        }
        if (s.zero_ratio >= chronic_zero_threshold) {
            violations.push_back(
                "Dim " + std::to_string(i) + " (" + name + ") chronic zero_ratio=" +
                std::to_string(s.zero_ratio) + " >= chronic_zero_threshold=" +
                std::to_string(chronic_zero_threshold));
        }
        if (s.zero_ratio >= saturation_threshold) {
            warnings.push_back("Dim " + std::to_string(i) + " (" + name + ") zero saturation=" +
                                std::to_string(s.zero_ratio));
        }
        if (s.one_ratio >= saturation_threshold) {
            warnings.push_back("Dim " + std::to_string(i) + " (" + name + ") one saturation=" +
                                std::to_string(s.one_ratio));
        }
    }
    if (!high_corr.empty()) {
        warnings.push_back(
            "High redundancy: " + std::to_string(high_corr.size()) +
            " feature pairs exceed abs(corr)>" + std::to_string(corr_threshold));
    }

    // --- validate_lbr_file.py-derived gates ---
    if (check_nulls) {
        for (std::size_t i = 0; i < dim_stats.size(); ++i) {
            if (dim_stats[i].nan_inf_count > 0) {
                violations.push_back(
                    "Dim " + std::to_string(i) + " has " +
                    std::to_string(dim_stats[i].nan_inf_count) + " NaN/Inf values");
            }
        }
    }
    if (check_zero_trap) {
        for (std::size_t i = 0; i < dim_stats.size(); ++i) {
            const auto& s = dim_stats[i];
            if (s.zero_ratio >= 0.98) {
                violations.push_back(
                    "Dim " + std::to_string(i) + " zero-trap detected (zero_ratio=" +
                    std::to_string(s.zero_ratio) + ")");
            }
            if (s.one_ratio >= 0.98) {
                violations.push_back(
                    "Dim " + std::to_string(i) + " one-trap detected (one_ratio=" +
                    std::to_string(s.one_ratio) + ")");
            }
        }
    }
    if (check_sequence_integrity) {
        if (counters.sequence_mismatches > 0) {
            violations.push_back("Sequence mismatches detected: " + std::to_string(counters.sequence_mismatches));
        }
        if (counters.sequence_regressions > 0) {
            violations.push_back("Sequence regressions detected: " + std::to_string(counters.sequence_regressions));
        }
        if (counters.unpaired_market_records > 0 || counters.unpaired_system_records > 0) {
            violations.push_back(
                "Unpaired MO/SS records detected (market=" +
                std::to_string(counters.unpaired_market_records) + ", system=" +
                std::to_string(counters.unpaired_system_records) + ")");
        }
    }

    const bool passed = violations.empty();

    std::printf("========================================================================\n");
    std::printf("Context Validate Report\n");
    std::printf("========================================================================\n");
    std::printf("Input: %s\n", input_path.c_str());
    std::printf("Rows sampled: %zu\n", rows_sampled);
    std::printf("Aligned pairs: %zu, sequence mismatches: %zu\n",
                counters.aligned_pairs, counters.sequence_mismatches);
    std::printf("Status: %s\n", passed ? "PASS" : "FAIL");
    if (!top_corr.empty()) {
        std::printf("Top abs-correlation pair: %s/%s = %.4f\n",
                    MTS::Schema::Contract::kObservationFieldNames[top_corr[0].dim_a],
                    MTS::Schema::Contract::kObservationFieldNames[top_corr[0].dim_b],
                    top_corr[0].corr);
    }
    if (!violations.empty()) {
        std::printf("\nViolations\n");
        for (const auto& v : violations) std::printf("  - %s\n", v.c_str());
    }
    if (!warnings.empty()) {
        std::printf("\nWarnings\n");
        for (const auto& w : warnings) std::printf("  - %s\n", w.c_str());
    }
    std::printf("========================================================================\n");

    if (!report_json_path.empty()) {
        std::ofstream out(report_json_path);
        out << "{\n";
        out << "  \"input\": \"" << JsonEscape(input_path) << "\",\n";
        out << "  \"rows_sampled\": " << rows_sampled << ",\n";
        out << "  \"status\": \"" << (passed ? "PASS" : "FAIL") << "\",\n";
        out << "  \"violations\": [";
        for (std::size_t i = 0; i < violations.size(); ++i) {
            out << (i ? ", " : "") << "\"" << JsonEscape(violations[i]) << "\"";
        }
        out << "],\n";
        out << "  \"warnings\": [";
        for (std::size_t i = 0; i < warnings.size(); ++i) {
            out << (i ? ", " : "") << "\"" << JsonEscape(warnings[i]) << "\"";
        }
        out << "]\n";
        out << "}\n";
        std::printf("Wrote JSON report: %s\n", report_json_path.c_str());
    }

    if (strict && !passed) {
        return 2;
    }
    return 0;
}
