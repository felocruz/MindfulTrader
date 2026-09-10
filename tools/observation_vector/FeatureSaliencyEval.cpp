// FeatureSaliencyEval.cpp -- CLI driver for Feature Saliency EM (Task 7,
// docs/superpowers/plans/2026-09-09-feature-saliency-em-fitter-implementation.md).
// Reads mes_candidates.parquet's 10 candidate-dim columns (tools/market_data_
// replay/'s own dim-selection pipeline output), fits FeatureSaliencyEM.h
// (Task 1-6, pure numerical core, no I/O), and reports per-dim saliency.
//
// Usage:
//   tools/bin/feature_saliency_eval \
//     --input /home/rcruz/devel/VSCode/lbrnet/data/raw/mes_candidates.parquet \
//     [--k 4] [--max-observations 500000] [--max-iterations 500] [--seed 13] \
//     [--first-n]  (smoke-test mode: stop after --max-observations rows, no full-file scan --
//                   NOT statistically representative, for code-path validation only)
//
// Build: mamba run -n mts g++ -O2 -std=c++17 -Iinclude \
//   $(mamba run -n mts pkg-config --cflags arrow parquet) \
//   tools/observation_vector/FeatureSaliencyEval.cpp \
//   $(mamba run -n mts pkg-config --libs arrow parquet) \
//   -Wl,-rpath,/home/rcruz/anaconda3/envs/mts/lib \
//   -o tools/bin/feature_saliency_eval

#include "FeatureSaliencyEM.h"
#include "../market_data_replay/CandidateObservationDims.h"
#include "../ToolProgressLogger.h"

#include <arrow/api.h>
#include <arrow/io/file.h>
#include <parquet/arrow/reader.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <random>
#include <string>
#include <vector>

namespace {

constexpr std::size_t kD = mdr::kCandidateDimCount;

// Bounded-memory reservoir sample (Algorithm R, Vitter 1985) over the
// Parquet file's rows -- reads the whole file exactly once regardless of
// its size, but never holds more than `cap` observations in memory at any
// time. Deterministic given `seed`, per spec §7/Task 7 Step 2 ("deterministic
// subsampling via the same seed, not arbitrary truncation").
class ReservoirSampler {
public:
    ReservoirSampler(std::size_t cap, std::uint64_t seed) : m_cap(cap), m_rng(seed) {
        m_reservoir.reserve(cap);
    }

    void Offer(const fsem::Observation<kD>& x) {
        if (m_reservoir.size() < m_cap) {
            m_reservoir.push_back(x);
        } else {
            std::uniform_int_distribution<std::uint64_t> dist(0, m_seen);
            const std::uint64_t j = dist(m_rng);
            if (j < m_cap) m_reservoir[static_cast<std::size_t>(j)] = x;
        }
        ++m_seen;
    }

    const std::vector<fsem::Observation<kD>>& Reservoir() const { return m_reservoir; }
    std::uint64_t SeenCount() const { return m_seen; }

private:
    std::size_t m_cap;
    std::mt19937_64 m_rng;
    std::vector<fsem::Observation<kD>> m_reservoir;
    std::uint64_t m_seen = 0;
};

}  // namespace

int main(int argc, char** argv) {
    std::string inputPath;
    std::size_t k = 4;  // PRODUCTION_TRIAGE.md row 1's decided production target
    std::size_t maxObservations = 500'000;  // bounded default, spec §7 -- never unbounded
    int maxIterations = 500;
    std::uint64_t seed = 13;
    bool firstNOnly = false;  // smoke-test mode: stop after maxObservations rows, no full-file scan

    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--input") == 0 && i + 1 < argc) {
            inputPath = argv[++i];
        } else if (std::strcmp(argv[i], "--k") == 0 && i + 1 < argc) {
            k = static_cast<std::size_t>(std::strtoul(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--max-observations") == 0 && i + 1 < argc) {
            maxObservations = static_cast<std::size_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--max-iterations") == 0 && i + 1 < argc) {
            maxIterations = std::atoi(argv[++i]);
        } else if (std::strcmp(argv[i], "--seed") == 0 && i + 1 < argc) {
            seed = static_cast<std::uint64_t>(std::strtoull(argv[++i], nullptr, 10));
        } else if (std::strcmp(argv[i], "--first-n") == 0) {
            firstNOnly = true;
        }
    }
    if (inputPath.empty()) {
        std::fprintf(stderr,
                      "usage: %s --input PATH.parquet [--k 4] [--max-observations 500000] "
                      "[--max-iterations 500] [--seed 13] [--first-n]\n",
                      argv[0]);
        return 1;
    }

    ToolProgressLogger progress("feature_saliency_eval");
    progress.Log("input=" + inputPath + " k=" + std::to_string(k) +
                 " max-observations=" + std::to_string(maxObservations) +
                 " max-iterations=" + std::to_string(maxIterations) +
                 " seed=" + std::to_string(seed) + " D=" + std::to_string(kD));

    auto infileResult = arrow::io::ReadableFile::Open(inputPath);
    if (!infileResult.ok()) {
        progress.Log("FATAL: cannot open " + inputPath + ": " + infileResult.status().ToString());
        return 1;
    }
    parquet::arrow::FileReaderBuilder builder;
    auto openStatus = builder.Open(*infileResult);
    if (!openStatus.ok()) {
        progress.Log("FATAL: FileReaderBuilder::Open: " + openStatus.ToString());
        return 1;
    }
    std::unique_ptr<parquet::arrow::FileReader> reader;
    auto buildStatus = builder.Build(&reader);
    if (!buildStatus.ok()) {
        progress.Log("FATAL: FileReaderBuilder::Build: " + buildStatus.ToString());
        return 1;
    }

    std::shared_ptr<arrow::Schema> schema;
    auto schemaStatus = reader->GetSchema(&schema);
    if (!schemaStatus.ok()) {
        progress.Log("FATAL: GetSchema: " + schemaStatus.ToString());
        return 1;
    }
    // Column projection, resolved from the real schema (never hardcoded) --
    // same convention as market_data_io.h's own reader. Order matches
    // mdr::kCandidateDims so batch->column(i) maps directly to candidate dim i.
    std::vector<int> columnIndices(kD);
    for (std::size_t i = 0; i < kD; ++i) {
        const char* name = MTS::Schema::Contract::kObservationFieldNames[mdr::kCandidateDims[i]];
        const int idx = schema->GetFieldIndex(name);
        if (idx < 0) {
            progress.Log(std::string("FATAL: missing column '") + name + "' in " + inputPath);
            return 1;
        }
        columnIndices[static_cast<std::size_t>(i)] = idx;
    }

    std::vector<int> rowGroups(static_cast<std::size_t>(reader->num_row_groups()));
    std::iota(rowGroups.begin(), rowGroups.end(), 0);

    auto batchReaderResult = reader->GetRecordBatchReader(rowGroups, columnIndices);
    if (!batchReaderResult.ok()) {
        progress.Log("FATAL: GetRecordBatchReader: " + batchReaderResult.status().ToString());
        return 1;
    }
    auto batchReader = std::move(*batchReaderResult);

    ReservoirSampler sampler(maxObservations, seed);
    constexpr std::size_t kProgressEveryNRows = 5'000'000;
    std::size_t rowsSeen = 0;
    while (true) {
        std::shared_ptr<arrow::RecordBatch> batch;
        auto nextStatus = batchReader->ReadNext(&batch);
        if (!nextStatus.ok()) {
            progress.Log("FATAL: ReadNext: " + nextStatus.ToString());
            return 1;
        }
        if (batch == nullptr) break;
        std::array<const float*, kD> cols{};
        for (std::size_t j = 0; j < kD; ++j) {
            auto arr = std::static_pointer_cast<arrow::FloatArray>(batch->column(static_cast<int>(j)));
            cols[j] = arr->raw_values();
        }
        for (std::int64_t i = 0; i < batch->num_rows(); ++i) {
            fsem::Observation<kD> x{};
            for (std::size_t j = 0; j < kD; ++j) x[j] = static_cast<double>(cols[j][i]);
            sampler.Offer(x);
            ++rowsSeen;
            if (rowsSeen % kProgressEveryNRows == 0) progress.LogProgress(rowsSeen, 0);
            if (firstNOnly && rowsSeen >= maxObservations) break;
        }
        if (firstNOnly && rowsSeen >= maxObservations) break;
    }
    progress.Log("read " + std::to_string(rowsSeen) + " rows, reservoir holds " +
                 std::to_string(sampler.Reservoir().size()) + " observations");

    if (sampler.Reservoir().empty()) {
        progress.Log("FATAL: no observations read -- nothing to fit");
        return 1;
    }

    progress.Log("fitting FeatureSaliencyEM (K=" + std::to_string(k) + ")...");
    const auto result = fsem::FitFeatureSaliencyEM<kD>(
        sampler.Reservoir(), k, maxIterations, /*tol=*/1e-6, seed);
    progress.Log("converged after " + std::to_string(result.iterations) + " iterations, final "
                 "log-likelihood=" + std::to_string(result.logLikelihood));

    std::vector<std::size_t> order(kD);
    std::iota(order.begin(), order.end(), 0);
    std::sort(order.begin(), order.end(), [&](std::size_t a, std::size_t b) {
        return result.params.phi[a] > result.params.phi[b];
    });

    progress.Log("--- per-dim saliency (phi), sorted descending ---");
    for (std::size_t rank = 0; rank < kD; ++rank) {
        const std::size_t j = order[rank];
        const char* name = MTS::Schema::Contract::kObservationFieldNames[mdr::kCandidateDims[j]];
        char line[256];
        std::snprintf(line, sizeof(line), "  %-24s phi=%.4f", name, result.params.phi[j]);
        progress.Log(line);
    }

    progress.Log("--- per-state mean/variance for each dim (context) ---");
    for (std::size_t j = 0; j < kD; ++j) {
        const char* name = MTS::Schema::Contract::kObservationFieldNames[mdr::kCandidateDims[j]];
        std::string line = std::string("  ") + name + ":";
        for (std::size_t s = 0; s < result.params.K(); ++s) {
            char stateBuf[80];
            std::snprintf(stateBuf, sizeof(stateBuf), " state%zu[mu=%.4f,var=%.4f]",
                          s, result.params.stateMean[s][j], result.params.stateVar[s][j]);
            line += stateBuf;
        }
        progress.Log(line);
    }

    return 0;
}
