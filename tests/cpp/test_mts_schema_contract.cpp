// test_mts_schema_contract.cpp — characterization tests for
// MakeObservationData/ToObservationArray/MakeAsymmetryContext/
// ToAsymmetryArray's memcpy-based conversions (mts_schema_contract_generated.h),
// verifying bit-exact field-order correctness against named accessors.
//
// Build & run natively:
//   g++ -std=c++17 -I include -I include/generated tests/cpp/test_mts_schema_contract.cpp -o /tmp/msc_test && /tmp/msc_test

#include "generated/mts_schema_contract_generated.h"

#include <cstdio>

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

}  // namespace

int main() {
    std::printf("mts_schema_contract memcpy-conversion unit tests\n");

    using namespace MTS::Schema::Contract;

    // ObservationData roundtrip: distinct values per index, verified via
    // named accessors (not just re-reading the array back).
    {
        ObservationArray values{};
        for (size_t i = 0; i < kObservationDim; ++i) {
            values[i] = static_cast<float>(i) * 1.5f + 0.25f;
        }
        const auto obs = MakeObservationData(values);
        check("obs_log_scale_ratio_matches_index_0",
              obs.log_scale_ratio() == values[kObsLogScaleRatio]);
        check("obs_burstiness_index_matches_index_1",
              obs.burstiness_index() == values[kObsBurstinessIndex]);
        check("obs_fast_mean_rev_z_matches_last_index",
              obs.fast_mean_rev_z() == values[kObsFastMeanRevZ]);

        const auto roundtrip = ToObservationArray(obs);
        bool allMatch = true;
        for (size_t i = 0; i < kObservationDim; ++i) {
            if (roundtrip[i] != values[i]) allMatch = false;
        }
        check("obs_full_roundtrip_bit_exact", allMatch);
    }

    // AsymmetryContext roundtrip.
    {
        AsymmetryArray values{};
        for (size_t i = 0; i < kAsymmetryDim; ++i) {
            values[i] = static_cast<float>(i) * 2.5f - 1.0f;
        }
        const auto ctx = MakeAsymmetryContext(values);
        check("asym_shannon_entropy_matches_index_0",
              ctx.shannon_entropy() == values[kAsymShannonEntropy]);
        check("asym_session_quality_score_matches_last_index",
              ctx.session_quality_score() == values[kAsymSessionQualityScore]);

        const auto roundtrip = ToAsymmetryArray(ctx);
        bool allMatch = true;
        for (size_t i = 0; i < kAsymmetryDim; ++i) {
            if (roundtrip[i] != values[i]) allMatch = false;
        }
        check("asym_full_roundtrip_bit_exact", allMatch);
    }

    std::printf("%s\n", g_failures == 0 ? "ALL PASS" : "FAILURES PRESENT");
    return g_failures == 0 ? 0 : 1;
}
