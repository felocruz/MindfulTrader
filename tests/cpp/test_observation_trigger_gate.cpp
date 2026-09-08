// test_observation_trigger_gate.cpp — characterization tests for
// ObservationTriggerGate, the pure replica of ContextManager's Mahalanobis
// significant-change gate. No Sierra Chart/ACSIL dependency.
//
// Build & run natively:
//   g++ -std=c++17 -I include -I include/generated tests/cpp/test_observation_trigger_gate.cpp -o /tmp/otg_test && /tmp/otg_test

#include "ObservationTriggerGate.h"

#include <cstdio>
#include <limits>

namespace {

int g_failures = 0;

void check(const char* name, bool ok) {
    if (ok) {
        std::printf("  PASS  %s\n", name);
    } else {
        ++g_failures;
        std::printf("  FAIL  %s\n", name);
    }
}

std::array<float, otg::kObservationDim> MakeObs(float fillValue) {
    std::array<float, otg::kObservationDim> obs;
    obs.fill(fillValue);
    return obs;
}

}  // namespace

int main() {
    std::printf("ObservationTriggerGate unit tests\n");

    // Below warmup (kMinSamples=40): no trigger decision possible yet.
    {
        otg::ObservationTriggerGate gate;
        for (int i = 0; i < 39; ++i) {
            gate.PushObservation(MakeObs(0.0f));
        }
        const auto metrics = gate.ComputeTriggerDecisionMetrics(MakeObs(5.0f), 5.0f);
        check("below_warmup_never_significant", !metrics.significant_change);
        check("below_warmup_zero_distance", metrics.mahalanobis_distance == 0.0f);
    }

    // Stable, flat history: a moderate deviation should be flagged significant
    // via the geometry channel once warmed up (MAD collapses to a small floor
    // on identical inputs, so even a modest jump produces a large z-score).
    {
        otg::ObservationTriggerGate gate;
        for (int i = 0; i < 40; ++i) {
            gate.PushObservation(MakeObs(0.0f));
        }
        const auto metrics = gate.ComputeTriggerDecisionMetrics(MakeObs(5.0f), 5.0f);
        check("large_jump_after_flat_history_is_significant", metrics.significant_change);
        check("large_jump_has_nonzero_distance", metrics.mahalanobis_distance > 0.0f);
    }

    // No deviation at all after warmup: not significant.
    {
        otg::ObservationTriggerGate gate;
        for (int i = 0; i < 40; ++i) {
            gate.PushObservation(MakeObs(3.0f));
        }
        const auto metrics = gate.ComputeTriggerDecisionMetrics(MakeObs(3.0f), 5.0f);
        check("zero_deviation_is_not_significant", !metrics.significant_change);
    }

    // IsEnergyObservationDim: exactly the 5 documented energy dims.
    {
        check("log_scale_ratio_is_energy",
              otg::IsEnergyObservationDim(MTS::Schema::Contract::kObsLogScaleRatio));
        check("relative_range_is_energy",
              otg::IsEnergyObservationDim(MTS::Schema::Contract::kObsRelativeRange));
        check("tail_index_is_energy",
              otg::IsEnergyObservationDim(MTS::Schema::Contract::kObsTailIndex));
        check("liq_fragility_is_energy",
              otg::IsEnergyObservationDim(MTS::Schema::Contract::kObsLiqFragility));
        check("fast_taleb_kurtosis_is_energy",
              otg::IsEnergyObservationDim(MTS::Schema::Contract::kObsFastTalebKurtosis));
        check("hurst_exponent_is_not_energy",
              !otg::IsEnergyObservationDim(MTS::Schema::Contract::kObsHurstExponent));
    }

    // SanitizeObservationVector: non-finite -> 0.0, then clamps.
    {
        auto obs = MakeObs(0.0f);
        obs[static_cast<size_t>(MTS::Schema::Contract::kObsHurstExponent)] =
            std::numeric_limits<float>::quiet_NaN();
        obs[static_cast<size_t>(MTS::Schema::Contract::kObsRelativeRange)] = 999.0f;  // above 25.0 upper bound
        uint64_t nonFinite = 0, clamped = 0;
        const auto sanitized = otg::SanitizeObservationVector(obs, nonFinite, clamped);
        check("nan_dim_sanitized_to_zero",
              sanitized[static_cast<size_t>(MTS::Schema::Contract::kObsHurstExponent)] == 0.0f);
        check("nonfinite_counter_incremented", nonFinite == 1);
        check("out_of_range_dim_clamped_to_upper_bound",
              sanitized[static_cast<size_t>(MTS::Schema::Contract::kObsRelativeRange)] == 25.0f);
        check("clamped_counter_incremented_at_least_once", clamped >= 1);
    }

    // GetAdaptiveNoiseFloor: monotonic decrease with velocity (conservative at
    // low velocity, reactive at high velocity).
    {
        const float lowFloor = otg::ObservationTriggerGate::GetAdaptiveNoiseFloor(1.0f);
        const float midFloor = otg::ObservationTriggerGate::GetAdaptiveNoiseFloor(5.5f);
        const float highFloor = otg::ObservationTriggerGate::GetAdaptiveNoiseFloor(10.0f);
        check("noise_floor_low_velocity_is_conservative", lowFloor > midFloor);
        check("noise_floor_high_velocity_is_reactive", highFloor < midFloor);
    }

    // ComputeL1DistanceFromBaseline: zero before a baseline is set, real after.
    {
        otg::ObservationTriggerGate gate;
        check("no_baseline_l1_is_zero", gate.ComputeL1DistanceFromBaseline(MakeObs(1.0f)) == 0.0f);
        gate.SetBaseline(MakeObs(0.0f));
        check("has_baseline_after_set", gate.HasBaseline());
        const float l1 = gate.ComputeL1DistanceFromBaseline(MakeObs(1.0f));
        check("l1_distance_matches_expected", l1 == static_cast<float>(otg::kObservationDim));
    }

    // Reset(): warmed-up gate returns to below-warmup behavior; baseline clears.
    {
        otg::ObservationTriggerGate gate;
        for (int i = 0; i < 40; ++i) {
            gate.PushObservation(MakeObs(3.0f));
        }
        gate.SetBaseline(MakeObs(3.0f));
        check("pre_reset_is_warmed_up", gate.SampleCount() == 40);
        check("pre_reset_has_baseline", gate.HasBaseline());

        gate.Reset();
        check("post_reset_sample_count_is_zero", gate.SampleCount() == 0);
        check("post_reset_has_no_baseline", !gate.HasBaseline());
        const auto metrics = gate.ComputeTriggerDecisionMetrics(MakeObs(3.0f), 5.0f);
        check("post_reset_never_significant_until_rewarmed", !metrics.significant_change);
    }

    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
