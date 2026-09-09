// test_candidate_trigger_gate.cpp — native characterization tests for
// CandidateTriggerGate (dim-selection spec, tools/market_data_replay/ only).
// Build & run: g++ -std=c++17 -Iinclude -Iinclude/generated \
//   -Itools/market_data_replay tools/market_data_replay/test_candidate_trigger_gate.cpp \
//   -o /tmp/ctg_test && /tmp/ctg_test

#include "CandidateTriggerGate.h"

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

    {
        CandidateTriggerGate gate;
        std::array<float, kCandidateDimCount> obs{};
        obs.fill(1.0f);
        auto m = gate.ComputeTriggerDecisionMetrics(obs);
        check("below_warmup_never_significant", !m.significant_change);
        check("below_warmup_zero_distance", m.mahalanobis_distance == 0.0f);
    }
    {
        CandidateTriggerGate gate;
        std::array<float, kCandidateDimCount> obs{};
        obs.fill(1.0f);
        for (int i = 0; i < 45; ++i) {
            gate.PushObservation(obs);
        }
        auto quiet = gate.ComputeTriggerDecisionMetrics(obs);
        check("quiet_flat_history_not_significant", !quiet.significant_change);

        std::array<float, kCandidateDimCount> jump{};
        jump.fill(1000.0f);
        auto m = gate.ComputeTriggerDecisionMetrics(jump);
        check("large_jump_after_flat_history_is_significant", m.significant_change);
        check("large_jump_has_nonzero_distance", m.mahalanobis_distance > 0.0f);
    }
    {
        CandidateTriggerGate gate;
        check("no_baseline_initially", !gate.HasBaseline());
        std::array<float, kCandidateDimCount> obs{};
        gate.SetBaseline(obs);
        check("has_baseline_after_set", gate.HasBaseline());
        gate.Reset();
        check("no_baseline_after_reset", !gate.HasBaseline());
        check("sample_count_zero_after_reset", gate.SampleCount() == 0);
    }

    if (g_failures == 0) std::printf("ALL PASS\n");
    else std::printf("%d FAILURE(S)\n", g_failures);
    return g_failures == 0 ? 0 : 1;
}
