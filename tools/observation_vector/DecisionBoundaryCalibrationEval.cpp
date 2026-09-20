// tools/observation_vector/DecisionBoundaryCalibrationEval.cpp -- CLI driver for
// DecisionBoundaryCalibration.h (Task 3, docs/superpowers/plans/2026-09-19-decision-boundary-
// calibration-tool-implementation.md). Generic across dims -- reads plain newline-delimited
// float files, not fused to any one dim's own data-sourcing mechanism (Tasks 4/5 supply those
// inputs for roughness_ratio/session_quality_score separately).
//
// Usage:
//   tools/bin/decision_boundary_calibration_eval --mode empirical-percentile \
//     --input values.txt --target-rate 0.10 [--upper-tail]
//
//   tools/bin/decision_boundary_calibration_eval --mode percentile-match \
//     --old-sample old_values.txt --new-sample new_values.txt --old-threshold 1.5
//
//   tools/bin/decision_boundary_calibration_eval --mode evt-gpd \
//     --input values.txt [--total-observed N]
//
// Build: g++ -O2 -std=c++17 -I tools/observation_vector \
//   tools/observation_vector/DecisionBoundaryCalibrationEval.cpp \
//   -o tools/bin/decision_boundary_calibration_eval

#include "DecisionBoundaryCalibration.h"
#include "../ToolProgressLogger.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <string>
#include <vector>

namespace {

// Reads one float per whitespace-separated token, any number of lines -- deliberately format-
// agnostic (newline- or space-delimited both work) since every real caller (Tasks 4/5) just
// dumps a plain value stream, not a structured table.
std::vector<double> LoadValues(const std::string& path) {
    std::vector<double> values;
    std::ifstream in(path);
    double v;
    while (in >> v) {
        values.push_back(v);
    }
    return values;
}

}  // namespace

int main(int argc, char** argv) {
    std::string mode;
    std::string inputPath;
    std::string oldSamplePath;
    std::string newSamplePath;
    double targetRate = 0.10;  // this repo's own ~10% base-rate precedent (CandidateTriggerGate::kBaseEpsilon)
    bool upperTail = true;
    double oldThreshold = 0.0;
    std::size_t totalObserved = 0;  // 0 = default to the input sample's own size

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--mode") == 0 && i + 1 < argc) {
            mode = argv[++i];
        } else if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            inputPath = argv[++i];
        } else if (std::strcmp(argv[i], "--old-sample") == 0 && i + 1 < argc) {
            oldSamplePath = argv[++i];
        } else if (std::strcmp(argv[i], "--new-sample") == 0 && i + 1 < argc) {
            newSamplePath = argv[++i];
        } else if (std::strcmp(argv[i], "--old-threshold") == 0 && i + 1 < argc) {
            oldThreshold = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--target-rate") == 0 && i + 1 < argc) {
            targetRate = std::atof(argv[++i]);
        } else if (std::strcmp(argv[i], "--upper-tail") == 0) {
            upperTail = true;
        } else if (std::strcmp(argv[i], "--lower-tail") == 0) {
            upperTail = false;
        } else if (std::strcmp(argv[i], "--total-observed") == 0 && i + 1 < argc) {
            totalObserved = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        }
    }

    if (mode != "empirical-percentile" && mode != "percentile-match" && mode != "evt-gpd") {
        std::fprintf(stderr,
                      "usage: %s --mode {empirical-percentile|percentile-match|evt-gpd} ...\n"
                      "  empirical-percentile: --input PATH --target-rate R [--upper-tail|--lower-tail]\n"
                      "  percentile-match:     --old-sample PATH --new-sample PATH --old-threshold V\n"
                      "  evt-gpd:               --input PATH [--total-observed N]\n",
                      argv[0]);
        return 1;
    }

    ToolProgressLogger logger("decision_boundary_calibration_eval");

    if (mode == "empirical-percentile") {
        std::vector<double> values = LoadValues(inputPath);
        if (values.empty()) {
            logger.Log("ERROR: --input produced zero values: " + inputPath);
            return 1;
        }
        std::sort(values.begin(), values.end());
        const double threshold = EmpiricalPercentileThreshold(values, targetRate, upperTail);
        char line[256];
        std::snprintf(line, sizeof(line),
                      "empirical-percentile: n=%zu target_rate=%.4f upper_tail=%d -> threshold=%.6f",
                      values.size(), targetRate, upperTail ? 1 : 0, threshold);
        logger.Log(line);
    } else if (mode == "percentile-match") {
        std::vector<double> oldSample = LoadValues(oldSamplePath);
        std::vector<double> newSample = LoadValues(newSamplePath);
        if (oldSample.empty() || newSample.empty()) {
            logger.Log("ERROR: --old-sample/--new-sample must both be non-empty");
            return 1;
        }
        const double mapped = PercentileMatchThreshold(oldSample, newSample, oldThreshold);
        char line[256];
        std::snprintf(line, sizeof(line),
                      "percentile-match: n_old=%zu n_new=%zu old_threshold=%.6f -> mapped=%.6f",
                      oldSample.size(), newSample.size(), oldThreshold, mapped);
        logger.Log(line);
    } else {  // evt-gpd
        std::vector<double> rawValues = LoadValues(inputPath);
        if (rawValues.empty()) {
            logger.Log("ERROR: --input produced zero values: " + inputPath);
            return 1;
        }
        std::vector<float> values(rawValues.begin(), rawValues.end());
        const std::size_t n = totalObserved != 0 ? totalObserved : values.size();
        const GPDFit fit = FitGPD(values, n);
        if (!fit.valid) {
            logger.Log("GPD fit SKIPPED: insufficient tail samples (n=" + std::to_string(values.size()) + ")");
            return 1;
        }
        char line[320];
        const char* domain = fit.xi > 0.0 ? "Frechet/unbounded" : (fit.xi < 0.0 ? "Weibull/bounded" : "Gumbel/borderline");
        std::snprintf(line, sizeof(line),
                      "evt-gpd: u=%.4f n_tail=%zu xi=%+.4f (%s) sigma=%.4f p=1/N return_level(N=%zu)=%.4f",
                      fit.u, fit.nTail, fit.xi, domain, fit.sigma, fit.totalObserved, fit.returnLevel);
        logger.Log(line);
    }

    return 0;
}
