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
#include "fixtures_dim1_raw.h"
#include "fixtures_dim3_raw.h"
#include "fixtures_dim9_raw.h"
#include "fixtures_dim0_raw.h"
#include "fixtures_dim7_raw.h"
#include "fixtures_dim6_raw.h"
#include "fixtures_dim8_raw.h"
#include "fixtures_dim12_raw.h"

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

    // Varying adaptive SOFTLOGZ dim on the GENERIC (non-shrinkage) path --
    // dim 8, not dim 0 or dim 6: both of those gained SHRINKAGE_SCALE_MIN>0
    // entries (2026-08-14 generalization, dim0 first, dim6 in the Task 3
    // follow-up pass), so they now return 0.0f before Calibrate() ever fires
    // (same bootstrap-guard behavior dim3 already had) rather than an early
    // median/MAD z-score -- correct, deliberate, and covered by the
    // dedicated dim0 check below. dim8 has no shrinkage entry (its own
    // audit found a clean, uncontaminated scale, only needing a wider
    // winsor bound -- DIM_WINSOR_SIGMA_OVERRIDE[8]=20.0, which doesn't
    // affect this test since z=0.899 never approaches even the old default
    // of 6.0), so it still exercises the plain generic-path formula this
    // test is actually about. Sequence 1..10 (window size 500 for dim8 >>
    // 10 samples, so no eviction -- the buffer holds all 10 values).
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
            obs[8] = static_cast<float>(i);
            result = fs.UpdateAndNormalize(obs);
        }
        check("varying_adaptive_dim_matches_independently_computed_zscore",
              approx(result[8], 0.6414965f, 1e-3f));
    }

    // dim0's new shrinkage bootstrap-guard: before Calibrate() ever fires
    // (sampleCount < RANK_WINDOW), macroScaleEwma[0] sits at its
    // zero-initialized default, so the shrinkage branch must return a safe
    // 0.0f rather than dividing by an unseeded anchor -- same guard dim3
    // already relies on, now exercised for dim0 too.
    {
        FeatureScaler fs;
        std::array<float, FeatureScaler::N_DIMS> result{};
        for (int i = 1; i <= 10; ++i) {
            auto obs = MakeObs(0.0f);
            obs[0] = static_cast<float>(i);
            result = fs.UpdateAndNormalize(obs);
        }
        check("dim0_shrinkage_bootstrap_guard_returns_zero_before_calibrate",
              result[0] == 0.0f);
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

    // --- TestDim3RealDataScaleStaysBounded ---
    // Real tick-level replica of dim3 (correction_action) against MES continuous
    // ticks, over the actual live-collection replay window (2023-09-01..10-20).
    // Matched live telemetry to within ~1pp at the 6-sigma rail-hit rate
    // (logs/rc_gemini.log CLAUDE_BRIEF_097). Contains the real tick sequence
    // that produced an internal z of -43,330 under the pre-shrinkage floor-gate
    // mechanism (DIM3_FIXTURE_BLOWUP_LOCAL_IDX). Uses lastRawZ directly rather
    // than matching a hardcoded rail VALUE -- dim3 has its own dedicated
    // DIM3_WIDE_WINSOR_SIGMA=500 (NOT dim1's 25 -- see that constant's doc
    // comment for why sharing it was wrong), so the rail constant itself is
    // dim-specific; testing the true pre-clamp z is both more precise and
    // independent of which bound is currently configured.
    {
        FeatureScaler fs;
        size_t hits6 = 0, hitsRail = 0;
        float maxAbsZ = 0.0f;
        for (size_t i = 0; i < DIM3_FIXTURE_N; ++i) {
            auto obs = MakeObs(0.0f);
            obs[3] = DIM3_FIXTURE_RAW[i];
            fs.UpdateAndNormalize(obs);
            const float absZ = std::fabs(fs.lastRawZ[3]);
            if (absZ >= 6.0f) ++hits6;
            if (absZ >= FeatureScaler::DIM_WINSOR_SIGMA_OVERRIDE[3]) ++hitsRail;
            maxAbsZ = std::max(maxAbsZ, absZ);
        }
        const double rate6 = static_cast<double>(hits6) / static_cast<double>(DIM3_FIXTURE_N);
        std::printf("  [info] dim3 real-data |z|>=6 rate: %.4f%%  |z|>=%.0f rate: %zu  max|z|=%.2f\n",
                    rate6 * 100.0, FeatureScaler::DIM_WINSOR_SIGMA_OVERRIDE[3], hitsRail, maxAbsZ);
        // Baseline (pre-shrinkage, binary floor-gate mechanism) measured ~9%
        // |z|>=6 rate and an unbounded max|z| (43,330 on the known worst
        // event) on this exact adversarial fixture via an independent Python
        // replica. D2 (shrinkage) brought the rate down; D4 (the GARCH-omega
        // floor) bounded the known worst event's magnitude. Assert max|z|
        // stays bounded and sane (not unbounded/exploding) rather than
        // re-asserting a specific rate, since the rate alone doesn't
        // distinguish "fixed" from "differently broken" (this is exactly the
        // gap that let the bootstrap-guard bug hide during D2's development,
        // and the gap that let the corrupted-diagnostic-data bug hide during
        // D3's original, wrong, 25-sigma sizing).
        check("dim3: real-data max|z| stays bounded (no unbounded blowup)", maxAbsZ < 1000.0);
        check("dim3: real-data |z|>=6 rate stays well below the pre-fix ~9% baseline", rate6 < 0.05);
    }

    // --- TestDim1WideWinsorTailTapersCleanly ---
    // Real tick-level replica of dim1 (burstiness_index) against MES continuous
    // ticks, over the actual live-collection replay window (2023-09-01..10-20).
    // Matched live telemetry within ~0.2pp at the 6-sigma rail-hit rate
    // (logs/rc_gemini.log CLAUDE_BRIEF_097). Unlike dim3, dim1's tail tapers
    // cleanly -- confirmed via lastRawZ (the true pre-winsorization z; the
    // rail VALUE itself changes with WIDE_STATE_WINSOR_SIGMA, so checking the
    // rate of |z|>=25 directly is the precise, fixture-independent assertion).
    {
        FeatureScaler fs;
        size_t hits6 = 0, hitsRail = 0;
        for (size_t i = 0; i < DIM1_FIXTURE_N; ++i) {
            auto obs = MakeObs(0.0f);
            obs[1] = DIM1_FIXTURE_RAW[i];
            fs.UpdateAndNormalize(obs);
            const float absZ = std::fabs(fs.lastRawZ[1]);
            if (absZ >= 6.0f) ++hits6;
            if (absZ >= FeatureScaler::DIM_WINSOR_SIGMA_OVERRIDE[1]) ++hitsRail;
        }
        const double rate6 = static_cast<double>(hits6) / static_cast<double>(DIM1_FIXTURE_N);
        const double rateRail = static_cast<double>(hitsRail) / static_cast<double>(DIM1_FIXTURE_N);
        std::printf("  [info] dim1 real-data |z|>=6 rate: %.4f%%  |z|>=%.0f rate: %.4f%%\n",
                    rate6 * 100.0, FeatureScaler::DIM_WINSOR_SIGMA_OVERRIDE[1], rateRail * 100.0);
        // Independently measured via Python replica on the same real data:
        // |z|>=6 ~7.63%. WIDE_STATE_WINSOR_SIGMA=45 sits past dim1's GPD-fitted
        // theoretical endpoint (~43.87, bootstrap p95=44.52, max draw=45.12 --
        // see that constant's doc comment), so the rate at the rail should be
        // at or near zero -- confirms the bound is set at the actual wall, not
        // an arbitrary intermediate point still being tested against.
        check("dim1: |z|>=WIDE_STATE_WINSOR_SIGMA rate is far below |z|>=6 rate (tail tapers, not just shifts)",
              rateRail < 0.01 && rateRail < rate6 / 5.0);
    }

    // --- TestShrinkageGeneralization: dim9/dim0/dim7 no longer blow up on
    // real data, same pattern D2/D4 proved for dim3 ---
    // 2026-08-14: tracing dim9/dim0/dim7's real-data max|z| events (SAME
    // methodology D4 used for dim3) found the identical scale-collapse
    // signature -- a modest raw deviation landing on a local MAD that had
    // collapsed to near its own series' p1 -- independently on all three,
    // not a dim3-specific quirk. Before this generalization: dim9 raw moved
    // only 1.4->8.0 but produced z=1963 (local MAD 0.00336, far below its
    // own p1=0.0249); dim0 raw moved ~0.25 but produced z=-176 (local MAD
    // 0.00144); dim7 raw moved to a bounded [-1,1] extreme but produced
    // z=438 with local MAD consistently below its own p1. Fixtures are the
    // same real tick-level replica window as fixtures_dim3_raw.h
    // ([1000000:1225000] of the 2023-09-01..10-20 series). Assert bounded
    // max|z| (well below the pre-fix magnitudes above), same "sane, not
    // just differently broken" bar dim3's test uses -- not a specific rate,
    // since D2's own history shows the rate alone can hide a still-broken
    // bootstrap case.
    {
        FeatureScaler fs;
        double maxAbsZ = 0.0;
        for (size_t i = 0; i < DIM9_FIXTURE_N; ++i) {
            auto obs = MakeObs(0.0f);
            obs[9] = DIM9_FIXTURE_RAW[i];
            fs.UpdateAndNormalize(obs);
            maxAbsZ = std::max(maxAbsZ, static_cast<double>(std::fabs(fs.lastRawZ[9])));
        }
        std::printf("  [info] dim9 real-data max|z| after shrinkage: %.2f (pre-fix: 1963.12)\n", maxAbsZ);
        check("dim9: shrinkage bounds real-data max|z| (was 1963, scale-collapse artifact)",
              maxAbsZ < 150.0);
    }
    {
        FeatureScaler fs;
        double maxAbsZ = 0.0;
        for (size_t i = 0; i < DIM0_FIXTURE_N; ++i) {
            auto obs = MakeObs(0.0f);
            obs[0] = DIM0_FIXTURE_RAW[i];
            fs.UpdateAndNormalize(obs);
            maxAbsZ = std::max(maxAbsZ, static_cast<double>(std::fabs(fs.lastRawZ[0])));
        }
        std::printf("  [info] dim0 real-data max|z| after shrinkage: %.2f (pre-fix: 175.93)\n", maxAbsZ);
        check("dim0: shrinkage bounds real-data max|z| (was -176, scale-collapse artifact)",
              maxAbsZ < 150.0);
    }
    {
        FeatureScaler fs;
        double maxAbsZ = 0.0;
        for (size_t i = 0; i < DIM7_FIXTURE_N; ++i) {
            auto obs = MakeObs(0.0f);
            obs[7] = DIM7_FIXTURE_RAW[i];
            fs.UpdateAndNormalize(obs);
            maxAbsZ = std::max(maxAbsZ, static_cast<double>(std::fabs(fs.lastRawZ[7])));
        }
        std::printf("  [info] dim7 real-data max|z| after shrinkage: %.2f (pre-fix: 437.69)\n", maxAbsZ);
        check("dim7: shrinkage bounds real-data max|z| (was 438, scale-collapse artifact)",
              maxAbsZ < 150.0);
    }

    // --- dim6 (hurst_exponent): Task 3, same scale-collapse signature found
    // via a tick-level DFA replica (sampled every 20 ticks, TS1/240min).
    // Approximate formula (see SHRINKAGE_SCALE_MIN's doc comment) -- assert
    // shrinkage keeps real-data max|z| bounded and sane, not a tight number.
    {
        FeatureScaler fs;
        double maxAbsZ = 0.0;
        for (size_t i = 0; i < DIM6_FIXTURE_N; ++i) {
            auto obs = MakeObs(0.0f);
            obs[6] = DIM6_FIXTURE_RAW[i];
            fs.UpdateAndNormalize(obs);
            maxAbsZ = std::max(maxAbsZ, static_cast<double>(std::fabs(fs.lastRawZ[6])));
        }
        std::printf("  [info] dim6 real-data max|z| after shrinkage: %.2f\n", maxAbsZ);
        check("dim6: shrinkage keeps real-data max|z| bounded (no unbounded blowup)",
              maxAbsZ < 1000.0);
    }

    // --- dim8 (fisher_info): Task 3, clean scale-collapse trace (no
    // shrinkage needed) -- verify the new DIM_WINSOR_SIGMA_OVERRIDE[8]=20.0
    // rail rate is far below the |z|>=6 rate, same "tail tapers, not just
    // shifts" check dim1's test uses.
    {
        FeatureScaler fs;
        size_t hits6 = 0, hitsRail = 0;
        for (size_t i = 0; i < DIM8_FIXTURE_N; ++i) {
            auto obs = MakeObs(0.0f);
            obs[8] = DIM8_FIXTURE_RAW[i];
            fs.UpdateAndNormalize(obs);
            const float absZ = std::fabs(fs.lastRawZ[8]);
            if (absZ >= 6.0f) ++hits6;
            if (absZ >= FeatureScaler::DIM_WINSOR_SIGMA_OVERRIDE[8]) ++hitsRail;
        }
        const double rate6 = static_cast<double>(hits6) / static_cast<double>(DIM8_FIXTURE_N);
        const double rateRail = static_cast<double>(hitsRail) / static_cast<double>(DIM8_FIXTURE_N);
        std::printf("  [info] dim8 real-data |z|>=6 rate: %.4f%%  |z|>=%.0f rate: %.4f%%\n",
                    rate6 * 100.0, FeatureScaler::DIM_WINSOR_SIGMA_OVERRIDE[8], rateRail * 100.0);
        check("dim8: |z|>=DIM_WINSOR_SIGMA_OVERRIDE rate is far below |z|>=6 rate (tail tapers, not just shifts)",
              rateRail < 0.01 && rateRail < rate6 / 5.0);
    }

    // dim6's new shrinkage bootstrap-guard, same pattern as dim0's.
    {
        FeatureScaler fs;
        std::array<float, FeatureScaler::N_DIMS> result{};
        for (int i = 1; i <= 10; ++i) {
            auto obs = MakeObs(0.0f);
            obs[6] = static_cast<float>(i);
            result = fs.UpdateAndNormalize(obs);
        }
        check("dim6_shrinkage_bootstrap_guard_returns_zero_before_calibrate",
              result[6] == 0.0f);
    }

    // --- dim12 (liq_fragility): Task 4, LOGZ path, first dim to use the new
    // LOGZ_WINSOR_SIGMA_OVERRIDE mechanism. Unlike SOFTLOGZ, the LOGZ path's
    // public result[] IS the clamped z directly (no further log1p transform
    // hides it), so the rail-hit rate is observable straight from result[]
    // without a lastRawZ-style diagnostic. Real bar-close replica (confirmed
    // bar-gated/historical-only, no live-bar undersampling risk), 15-min
    // bars aggregated from the full multi-year tick history.
    {
        FeatureScaler fs;
        size_t hits6 = 0, hitsRail = 0;
        for (size_t i = 0; i < DIM12_FIXTURE_N; ++i) {
            auto obs = MakeObs(0.0f);
            obs[12] = DIM12_FIXTURE_RAW[i];
            const auto result = fs.UpdateAndNormalize(obs);
            const float absZ = std::fabs(result[12]);
            if (absZ >= 6.0f) ++hits6;
            if (absZ >= FeatureScaler::LOGZ_WINSOR_SIGMA_OVERRIDE[12]) ++hitsRail;
        }
        const double rate6 = static_cast<double>(hits6) / static_cast<double>(DIM12_FIXTURE_N);
        const double rateRail = static_cast<double>(hitsRail) / static_cast<double>(DIM12_FIXTURE_N);
        std::printf("  [info] dim12 real-data |z|>=6 rate: %.4f%%  |z|>=%.0f rate: %.4f%%\n",
                    rate6 * 100.0, FeatureScaler::LOGZ_WINSOR_SIGMA_OVERRIDE[12], rateRail * 100.0);
        check("dim12: |z|>=LOGZ_WINSOR_SIGMA_OVERRIDE rate is far below |z|>=6 rate (tail tapers, not just shifts)",
              rateRail < 0.05 && rateRail < rate6 / 2.0);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
