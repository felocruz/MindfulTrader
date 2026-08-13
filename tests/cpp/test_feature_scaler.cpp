// test_feature_scaler.cpp — characterization tests for FeatureScaler,
// captured against the CURRENT std::deque-backed implementation as a golden
// baseline before the RingBuffer swap
// (docs/superpowers/specs/2026-08-07-contextmanager-ring-buffer-dod-design.md).
// Expected values below are independently computed (not just "whatever the
// current code happens to output") — re-run unchanged after the swap to
// confirm bit-identical behavior.
//
// Build & run natively (no Sierra Chart deps — FeatureScaler.h was extracted
// from ContextManager.h specifically to make this possible):
//   g++ -std=c++17 -I include -I include/generated tests/cpp/test_feature_scaler.cpp -o /tmp/fs_test && /tmp/fs_test

#include "FeatureScaler.h"

#include <cmath>
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

bool approx(float a, float b, float tol = 1e-4f) {
    return std::fabs(a - b) <= tol;
}

std::array<float, FeatureScaler::N_DIMS> MakeObs(float fillValue) {
    std::array<float, FeatureScaler::N_DIMS> obs;
    obs.fill(fillValue);
    return obs;
}

}  // namespace

int main() {
    std::printf("FeatureScaler unit tests\n");

    // First-ever sample: unconditional all-zero return, regardless of input.
    {
        FeatureScaler fs;
        auto obs = MakeObs(0.0f);
        obs[0] = 123.0f;
        const auto result = fs.UpdateAndNormalize(obs);
        bool allZero = true;
        for (float v : result) if (v != 0.0f) allZero = false;
        check("cold_start_first_sample_is_all_zero", allZero);
        check("cold_start_not_warmed_up", !fs.warmedUp);
        check("cold_start_sample_count_is_one", fs.sampleCount == 1);
    }

    // Constant input on an adaptive SOFTLOGZ dim (dim 0): MAD is exactly 0
    // (all identical values), which is below even the ABSOLUTE_FLOOR, so the
    // carry-forward branch fires every call. Since hasValidScaled never
    // becomes true (no live signal ever clears the floor), the dim reports
    // 0.0f forever — the "carry-forward death spiral" documented in
    // ContextManager.h/FeatureScaler.h.
    {
        FeatureScaler fs;
        float lastDim0 = -1.0f;
        for (int i = 0; i < 20; ++i) {
            const auto result = fs.UpdateAndNormalize(MakeObs(5.0f));
            lastDim0 = result[0];
        }
        check("constant_input_adaptive_dim_stays_zero_forever", lastDim0 == 0.0f);
    }

    // Static-scaled dims (5=LZ, 13=RECURRENCE, 14=FRACTAL) depend ONLY on the
    // current raw input, not on history -- any call after the first (which is
    // always all-zero) gives the exact analytic value immediately.
    {
        FeatureScaler fs;
        fs.UpdateAndNormalize(MakeObs(0.0f));  // sample 1: discarded (always zero)
        auto obs = MakeObs(0.0f);
        obs[5] = 1.0f;     // LZ_STATIC_CENTER=0.5, LZ_STATIC_SCALE=0.25 -> z=2.0
        // Probe value moved from 0.6 to 0.09 alongside the 2026-08-13
        // re-derivation of the recurrence constants (final-review Finding 1):
        // 0.6 is unreachable for the post-Task-3 RQA statistic, whose real
        // 60-min MES range is ~0.033-0.15, so 0.09 is an upper-tail-but-real
        // probe. z = (0.09-0.0467)/0.0099 = 4.373738 (float32).
        obs[13] = 0.09f;   // RECURRENCE_STATIC_CENTER=0.0467, SCALE=0.0099 -> z=4.3737...
        obs[14] = 1.5f;    // FRACTAL_STATIC_CENTER=1.289, SCALE=0.101 -> z=2.0891...
        const auto result = fs.UpdateAndNormalize(obs);
        // Independently computed via Python: math.copysign(math.log1p(abs(z)), z)
        check("static_lz_dim_exact_value", approx(result[5], 1.0986122886681096f));
        check("static_recurrence_dim_exact_value", approx(result[13], 1.6815237207713019f));
        check("static_fractal_dim_exact_value", approx(result[14], 1.127882670968223f));
    }

    // Varying adaptive SOFTLOGZ dim (dim 0), sequence 1..10 (window size 500
    // >> 10 samples, so no eviction -- the buffer holds all 10 values).
    // This implementation's "median"/"MAD" use std::nth_element at index
    // n/2 (NOT the textbook averaged-middle-two for even n), independently
    // re-derived: sorted [1..10], nth_element(mid=5) -> median=6;
    // abs-deviations sorted [0,1,1,2,2,3,3,4,4,5], nth_element(mid=5) -> 3,
    // madScale = 3*1.4826 = 4.4478; current=10 (last pushed);
    // z=(10-6)/4.4478=0.899321; result=copysign(log1p(0.899321),+) = 0.641496.
    {
        FeatureScaler fs;
        std::array<float, FeatureScaler::N_DIMS> result{};
        for (int i = 1; i <= 10; ++i) {
            auto obs = MakeObs(0.0f);
            obs[0] = static_cast<float>(i);
            result = fs.UpdateAndNormalize(obs);
        }
        check("varying_adaptive_dim_matches_independently_computed_zscore",
              approx(result[0], 0.6414965f, 1e-3f));
    }

    // Warmup boundary: warmedUp flips to true exactly at sampleCount ==
    // RANK_WINDOW (500), not one sample before or after.
    {
        FeatureScaler fs;
        for (int i = 0; i < static_cast<int>(FeatureScaler::RANK_WINDOW) - 1; ++i) {
            fs.UpdateAndNormalize(MakeObs(static_cast<float>(i % 7)));
        }
        check("warmup_not_yet_at_rank_window_minus_one", !fs.warmedUp);
        fs.UpdateAndNormalize(MakeObs(3.0f));
        check("warmup_flips_true_exactly_at_rank_window", fs.warmedUp);
        check("warmup_calibrated_after_flip", fs.calibrated);
    }

    // Reset() clears sample count and warmup state back to cold-start.
    {
        FeatureScaler fs;
        for (int i = 0; i < 50; ++i) {
            fs.UpdateAndNormalize(MakeObs(static_cast<float>(i)));
        }
        fs.Reset();
        check("reset_clears_sample_count", fs.sampleCount == 0);
        check("reset_clears_warmed_up", !fs.warmedUp);
        check("reset_clears_calibrated", !fs.calibrated);
    }

    // --- ComputeValueDominance (D2 sentinel-collapse diagnostic) ---
    {
        RingBuffer<float, FeatureScaler::RANK_WINDOW + 1> buf;
        for (int i = 0; i < 10; ++i) buf.push_back(1.0f);
        check("dominance_all_identical_is_one",
              approx(FeatureScaler::ComputeValueDominance(buf), 1.0f, 1e-6f));
    }
    {
        RingBuffer<float, FeatureScaler::RANK_WINDOW + 1> buf;
        for (int i = 0; i < 10; ++i) buf.push_back(static_cast<float>(i));
        check("dominance_all_distinct_is_one_over_n",
              approx(FeatureScaler::ComputeValueDominance(buf), 0.1f, 1e-6f));
    }
    {
        RingBuffer<float, FeatureScaler::RANK_WINDOW + 1> buf;
        for (int i = 0; i < 6; ++i) buf.push_back(0.0f);
        for (int i = 0; i < 4; ++i) buf.push_back(static_cast<float>(i + 1));
        check("dominance_repeated_sentinel_matches_expected_ratio",
              approx(FeatureScaler::ComputeValueDominance(buf), 0.6f, 1e-6f));
    }
    {
        RingBuffer<float, FeatureScaler::RANK_WINDOW + 1> buf;
        check("dominance_empty_buffer_is_zero",
              approx(FeatureScaler::ComputeValueDominance(buf), 0.0f, 1e-6f));
    }

    // Calibrate()/Recalibrate() wiring: 500 identical raw observations fill
    // every rolling buffer with one repeated value, triggering Calibrate()
    // at sampleCount == RANK_WINDOW (per the existing warmup-boundary test
    // above). dominanceRatio should come out ~1.0 for both a SOFTLOGZ dim
    // (dim 0) and a LOGZ dim (dim 2) -- not just the pure ComputeValueDominance
    // function in isolation, but the actual field FeatureScaler populates.
    {
        FeatureScaler fs;
        std::array<float, FeatureScaler::N_DIMS> result{};
        for (int i = 0; i < static_cast<int>(FeatureScaler::RANK_WINDOW); ++i) {
            result = fs.UpdateAndNormalize(MakeObs(5.0f));
        }
        check("calibrate_wiring_softlogz_dim_dominance_is_one",
              approx(fs.dominanceRatio[0], 1.0f, 1e-6f));
        check("calibrate_wiring_logz_dim_dominance_is_one",
              approx(fs.dominanceRatio[2], 1.0f, 1e-6f));
        (void)result;
    }

    // --- TestDedupeAtIngestion ---
    {
        FeatureScaler fs;
        // Warm up dim 1 (burstiness_index, SOFTLOGZ) with 500 varying samples so the
        // rolling window and warmup gate are both satisfied before the repeat run.
        for (int i = 0; i < 500; ++i) {
            auto obs = MakeObs(0.0f);
            obs[1] = 0.01f * static_cast<float>(i % 37) - 0.18f;  // varying, never exactly repeats
            fs.UpdateAndNormalize(obs);
        }
        // Now inject a carry-forward run: the same raw value 300 times in a row,
        // simulating a degenerate-input calculator holding its last valid reading.
        std::array<float, FeatureScaler::N_DIMS> result{};
        for (int i = 0; i < 300; ++i) {
            auto obs = MakeObs(0.0f);
            obs[1] = 0.42f;
            result = fs.UpdateAndNormalize(obs);
        }
        // Pre-fix: the window fills with 0.42f repeats, median converges to 0.42f,
        // z collapses to exactly 0. Post-fix: the window stops growing after the
        // first 0.42f push, so the held value's z reflects its true distance from
        // the pre-repeat distribution -- must be nonzero.
        check("dedupe: held value gets nonzero z after long repeat run", std::fabs(result[1]) > 1e-4f);
    }

    // --- TestDedupeLowersDominance ---
    {
        FeatureScaler fs;
        for (int i = 0; i < 500; ++i) {
            auto obs = MakeObs(0.0f);
            obs[2] = 0.01f * static_cast<float>(i % 41) - 0.2f;
            fs.UpdateAndNormalize(obs);
        }
        // Inject the same repeat-collapse signature the hardening spec's D2 diagnostic
        // was built to catch: a single value dominating >30% of the window.
        for (int i = 0; i < 250; ++i) {
            auto obs = MakeObs(0.0f);
            obs[2] = 0.75f;
            fs.UpdateAndNormalize(obs);
        }
        // Force a recalibration so dominanceRatio[] is recomputed.
        fs.Recalibrate();
        check("dedupe: dim2 dominanceRatio stays below the D2 ALERT threshold (0.30)",
              fs.dominanceRatio[2] < 0.30f);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
