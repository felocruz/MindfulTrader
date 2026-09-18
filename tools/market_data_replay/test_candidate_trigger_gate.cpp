// test_candidate_trigger_gate.cpp — native characterization tests for
// CandidateTriggerGate (dim-selection spec §3c, tools/market_data_replay/
// only). The gate is now a stateless direct chi-squared distance over
// FeatureScaler's own already-scaled candidate vector -- no rolling window,
// no per-dim warm-up concept of its own (see CandidateTriggerGate.h's header
// comment for why the earlier rolling-window design was replaced).
// Build & run: g++ -std=c++17 -Iinclude -Iinclude/generated \
//   -Itools/market_data_replay tools/market_data_replay/test_candidate_trigger_gate.cpp \
//   -o /tmp/ctg_test && /tmp/ctg_test

#include "CandidateTriggerGate.h"

#include <cmath>
#include <cstdio>

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) std::printf("  PASS  %s\n", name);
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}
}  // namespace

int main() {
    using mdr::kCandidateDimCount;
    using mdr::CandidateTriggerGate;

    // All-zero input (FeatureScaler's own scaled output at exactly its
    // center) is never significant -- distance is exactly 0.
    {
        CandidateTriggerGate gate;
        std::array<float, kCandidateDimCount> obs{};
        obs.fill(0.0f);
        auto m = gate.ComputeTriggerDecisionMetrics(obs);
        check("all_zero_is_not_significant", !m.significant_change);
        check("all_zero_distance_is_zero", m.mahalanobis_distance == 0.0f);
    }

    // A value comfortably below the chi-squared(kCandidateDimCount) threshold
    // (kBaseEpsilon) uniformly across all dims is not significant: distance =
    // sqrt(kCandidateDimCount * 0.5^2), well under kBaseEpsilon.
    {
        CandidateTriggerGate gate;
        std::array<float, kCandidateDimCount> obs{};
        obs.fill(0.5f);
        auto m = gate.ComputeTriggerDecisionMetrics(obs);
        check("small_uniform_z_not_significant", !m.significant_change);
    }

    // A value that pushes distance right at/above kBaseEpsilon IS
    // significant: distance = sqrt(kCandidateDimCount * 1.5^2).
    {
        CandidateTriggerGate gate;
        std::array<float, kCandidateDimCount> obs{};
        obs.fill(1.5f);
        auto m = gate.ComputeTriggerDecisionMetrics(obs);
        check("large_uniform_z_is_significant", m.significant_change);
        check("large_uniform_z_has_expected_distance",
              std::abs(m.mahalanobis_distance -
                        std::sqrt(static_cast<float>(kCandidateDimCount) * 1.5f * 1.5f)) < 1e-3f);
    }

    // A single dim spiking hard while the rest stay near zero must still be
    // detected (real regime shifts can be concentrated in one or two dims,
    // e.g. amihud_illiquidity/liq_fragility).
    {
        CandidateTriggerGate gate;
        std::array<float, kCandidateDimCount> obs{};
        obs.fill(0.0f);
        obs[6] = 10.0f;  // amihud_illiquidity index, arbitrary choice here
        auto m = gate.ComputeTriggerDecisionMetrics(obs);
        check("single_dim_spike_is_significant", m.significant_change);
    }

    // perDimZ must literally equal the input vector (no re-normalization) and
    // sum-of-squares to mahalanobis_distance^2.
    {
        CandidateTriggerGate gate;
        std::array<float, kCandidateDimCount> obs{};
        for (std::size_t i = 0; i < kCandidateDimCount; ++i) obs[i] = static_cast<float>(i) * 0.3f;
        auto m = gate.ComputeTriggerDecisionMetrics(obs);
        bool matches = true;
        float sumSq = 0.0f;
        for (std::size_t i = 0; i < kCandidateDimCount; ++i) {
            if (m.perDimZ[i] != obs[i]) matches = false;
            sumSq += m.perDimZ[i] * m.perDimZ[i];
        }
        check("per_dim_z_equals_input_directly", matches);
        check("per_dim_z_sums_to_total_distance_squared",
              std::abs(sumSq - m.mahalanobis_distance * m.mahalanobis_distance) < 1e-3f);
    }

    // Baseline/reset semantics: unrelated to the distance computation itself,
    // just the "force emit on the very first post-warmup tick" flag.
    {
        CandidateTriggerGate gate;
        std::array<float, kCandidateDimCount> obs{};
        check("no_baseline_initially", !gate.HasBaseline());
        gate.SetBaseline(obs);
        check("has_baseline_after_set", gate.HasBaseline());
        gate.Reset();
        check("no_baseline_after_reset", !gate.HasBaseline());
    }

    if (g_failures == 0) std::printf("ALL PASS\n");
    else std::printf("%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}


