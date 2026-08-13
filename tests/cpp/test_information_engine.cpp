// test_information_engine.cpp — unit tests for InformationEngine's
// volatility-standardized bin mapping (docs/superpowers/plans/2026-08-04-phase1-hardening.md
// Task 2; lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1.2).
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I include tests/cpp/test_information_engine.cpp -o /tmp/ie_test && /tmp/ie_test
//
// IMPORTANT — mutation-tested calibration:
// A reviewer independently compiled an earlier version of this file against the
// PRE-FIX header (fixed, hardcoded ~2bp "assumed sigma" bins, no volatility
// standardization) and found all assertions passed unmodified — i.e. the tests
// did not actually prove the fix works. Root cause: the chosen magnitudes
// happened to sit exactly on the OLD fixed-bps bin edges (0.5bp/5bp), so both
// the OLD and NEW code classified them identically. Every magnitude below was
// re-derived by simulating both the OLD fixed-bps MapToBin() and the NEW
// EMA-relative MapToBin() to guarantee a genuine behavioral split: each
// assertion here is verified to PASS against the current (fixed) header and
// FAIL against the pre-fix header (git rev f4cbdbc).

#include "InformationEngine.h"

#include <cmath>
#include <cstdio>

using namespace MindfulTrader;

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

bool approx(double a, double b, double tol = 1e-6) {
    return std::fabs(a - b) <= tol;
}

// Feeds a symmetric, varied-magnitude sequence (not a single constant value)
// so the Q-histogram populates a realistic spread of bins around the regime's
// typical size, rather than pinning all mass at exactly +/-1 sigma (which is
// what a constant alternating-magnitude feed does to the EMA-relative bins).
// signedMultiples must be symmetric (each +m paired with a -m) so the feed is
// zero-drift like a real return series.
void feedVariedRegime(InformationEngine& engine, double base, int totalObservations) {
    static const double signedMultiples[] = {
        -3.0, -2.0, -1.3, -0.8, -0.5, -0.3, -0.15,
         0.15, 0.3,  0.5,  0.8,  1.3,  2.0,  3.0
    };
    constexpr int kNumMultiples = sizeof(signedMultiples) / sizeof(signedMultiples[0]);
    for (int i = 0; i < totalObservations; ++i) {
        engine.AddObservation(base * signedMultiples[i % kNumMultiples]);
    }
}

}  // namespace

int main() {
    std::printf("InformationEngine unit tests\n");

    // Basic sanity: engine starts cold, GetRecurrenceRate/GetFisherInformation
    // don't crash and report the documented "not enough samples" defaults.
    {
        InformationEngine engine;
        check("cold_start_recurrence_rate_is_zero", engine.GetRecurrenceRate(0.0001) == 0.0);
        check("cold_start_fisher_info_is_zero", engine.GetFisherInformation() == 0.0);
    }

    // THE CORE FIX: the SAME query magnitude (3bp -- deliberately chosen to sit
    // INSIDE an old fixed-bps bin, i.e. strictly between the old 2bp/5bp edges,
    // so the old code's classification of it never varies with context) must
    // land in a DIFFERENT bin depending on the current (rolling) volatility
    // regime -- proving the bins are keyed to a live estimate, not a fixed,
    // hardcoded assumed sigma. Observed indirectly via GetRecurrenceRate(),
    // which internally calls the same MapToBin() the histograms use.
    //
    // Quiet regime: a ~1.2bp-typical-magnitude feed. Under the NEW (fixed)
    // code, 3bp is ~2.5x this regime's own EMA sigma -- an outer-tail bin that
    // the regime's own history rarely touches (recurrence < 0.10). Under the
    // OLD fixed-bps code (no regime awareness), this same 1.2bp-scale history
    // happens to place a non-trivial share of its own observations in the
    // SAME fixed bin as the 3bp query (recurrence == 0.14), so the "rare"
    // assertion below FAILS if built against the old header.
    {
        InformationEngine engineLowVol;
        feedVariedRegime(engineLowVol, 0.00012, 400);  // ~1.2bp-typical quiet regime
        const double recurrenceOfModerateMove = engineLowVol.GetRecurrenceRate(0.0003);  // 3bp
        check("moderate_move_is_rare_in_a_quiet_regime", recurrenceOfModerateMove < 0.10);
    }

    // Volatile regime: a ~50bp-typical-magnitude feed. Under the NEW (fixed)
    // code, 3bp is a small fraction of this regime's own EMA sigma -- squarely
    // in the central "Value Area" bins that dominate this regime's own history
    // (recurrence > 0.15). Under the OLD fixed-bps code, a 50bp-scale history
    // never produces observations anywhere near the fixed 2-5bp bin the 3bp
    // query lives in (recurrence == 0.0), so the "common" assertion below
    // FAILS if built against the old header.
    {
        InformationEngine engineHighVol;
        feedVariedRegime(engineHighVol, 0.005, 400);  // 50bp-typical volatile regime
        const double recurrenceOfSameMoveInVolatileRegime = engineHighVol.GetRecurrenceRate(0.0003);  // 3bp
        check("same_absolute_move_is_common_in_a_volatile_regime", recurrenceOfSameMoveInVolatileRegime > 0.15);
    }

    // Reset() must clear the rolling-volatility EMA too, not just the
    // existing histogram/buffer state -- otherwise a fresh engine after
    // Reset() would inherit a stale volatility estimate from before the
    // reset. This is checked with only 12 post-reset observations (the
    // minimum that clears GetRecurrenceRate()'s "m_countQ < 10" warmup gate --
    // every public accessor is gated on sample count, so fewer than ~10
    // observations cannot be observed at all), NOT the ~400 the earlier
    // version of this test used. At ~400 iterations (>20 EMA half-lives) the
    // EMA fully reconverges to the post-reset feed regardless of whether
    // Reset() actually cleared it, silently hiding a missing-clear bug --
    // exactly what a reviewer's mutation test (deleting the
    // `m_emaAbsLogReturn = 0.0;` line from Reset()) demonstrated. With only
    // 12 observations, a stale (uncleared) EMA has barely decayed from its
    // pre-reset volatile value, so the same 1.2bp query reads as spuriously
    // "common" (recurrence == 0.5) instead of correctly "rare" (recurrence ==
    // 0.0) -- the mutation flips this assertion's outcome.
    {
        InformationEngine engine;
        // Seed a volatile-regime EMA (~50bp) before reset.
        for (int i = 0; i < 20; ++i) {
            engine.AddObservation((i % 2 == 0) ? 0.0005 : -0.0005);
        }
        engine.Reset();
        // Immediately feed a quiet regime (~0.1bp) -- just enough observations
        // (12) to clear the GetRecurrenceRate() warmup gate, not enough for a
        // stale EMA to have reconverged.
        for (int i = 0; i < 12; ++i) {
            engine.AddObservation((i % 2 == 0) ? 0.00001 : -0.00001);
        }
        const double recurrenceAfterReset = engine.GetRecurrenceRate(0.00012);  // 1.2bp
        check("reset_clears_stale_volatility_estimate", recurrenceAfterReset < 0.05);
    }

    // Finding 1 regression test (final-review fix round): UpdateHistogram() used
    // to re-derive the eviction bin via MapToBin(oldVal) using the CURRENT (live,
    // drifted) m_emaAbsLogReturn, instead of the sigma in effect when oldVal was
    // originally inserted. That desyncs the histogram from the ring buffer's true
    // contents once sigma drifts across an eviction -- which requires (a) pushing
    // well past WINDOW_SIZE_Q (500) so eviction actually happens, and (b) genuinely
    // varying volatility between insertion and eviction of the SAME slot.
    //
    // GetShannonEntropy() is the actual production consumer (wired through
    // ContextManager::shannonFlowEntropy into RiskManager's chaos-halt gate and
    // Scoring's directional/mean-reversion multipliers) and is exercised by no
    // other test in this file -- this is the regression test that would have
    // caught the bug: it FAILS against the unfixed header (entropy goes deeply
    // negative once the histogram desyncs) and PASSES against the fixed one.
    {
        InformationEngine engine;
        bool sawEntropyOutOfBounds = false;
        bool sawRecurrenceOutOfBounds = false;
        const double kMaxEntropy = std::log2(10.0);

        // Alternate quiet/volatile blocks of 200+ observations each (each block
        // exceeds neither window alone, but the total drives repeated eviction
        // in both P (50) and Q (500) windows across many sigma regime changes).
        constexpr int kBlocks = 8;             // 8 * 220 = 1760 observations total
        constexpr int kObservationsPerBlock = 220;
        for (int block = 0; block < kBlocks; ++block) {
            const bool quiet = (block % 2 == 0);
            const double base = quiet ? 0.00012 : 0.006;  // ~1.2bp vs ~60bp typical moves
            for (int i = 0; i < kObservationsPerBlock; ++i) {
                // Varied signed magnitudes (not a constant), matching feedVariedRegime's
                // rationale: spreads mass across bins instead of pinning it at one.
                static const double signedMultiples[] = {
                    -3.0, -2.0, -1.3, -0.8, -0.5, -0.3, -0.15,
                     0.15, 0.3,  0.5,  0.8,  1.3,  2.0,  3.0
                };
                constexpr int kNumMultiples = sizeof(signedMultiples) / sizeof(signedMultiples[0]);
                const double logReturn = base * signedMultiples[i % kNumMultiples];
                engine.AddObservation(logReturn);

                const double h = engine.GetShannonEntropy();
                if (h < 0.0 || h > kMaxEntropy) {
                    sawEntropyOutOfBounds = true;
                }
                const double r = engine.GetRecurrenceRate(logReturn);
                if (r < 0.0 || r > 1.0) {
                    sawRecurrenceOutOfBounds = true;
                }
            }
        }

        check("shannon_entropy_stays_in_bounds_across_drifting_volatility_and_eviction",
              !sawEntropyOutOfBounds);
        check("recurrence_rate_stays_in_bounds_across_drifting_volatility_and_eviction",
              !sawRecurrenceOutOfBounds);
    }

    // Miller-Madow entropy bias correction test. For small sample sizes,
    // the plug-in Shannon entropy estimator is biased. Miller (1955) provides
    // the correction: bias ~ (m-1)/(2N*ln2) bits, where m = occupied bins,
    // N = sample count. This test uses a closed-form numeric assertion:
    // construct a known input distribution, compute expected entropy by hand,
    // assert function output matches to tolerance. This approach fails if the
    // correction is removed (verified empirically by reviewer).
    {
        InformationEngine engine;

        // Pre-warm with many observations to stabilize the EMA, ensuring
        // consistent bin mapping for the 50 test observations.
        for (int i = 0; i < 200; ++i) {
            static const double warmupSequence[] = {
                -1.0, -0.5, -0.2, 0.0, 0.2, 0.5, 1.0
            };
            constexpr int kWarmupSize = sizeof(warmupSequence) / sizeof(warmupSequence[0]);
            engine.AddObservation(warmupSequence[i % kWarmupSize]);
        }

        // Feed 50 test observations. With stabilized EMA ~1.8, these produce
        // a known histogram: bins {0,1,2,3,4,5,6,7} with counts {4,7,5,5,9,14,3,3}.
        static const double testSequence[] = {
            -8.0, -4.0, -2.5, -1.2, -0.5, -0.2, 0.0, 0.2, 0.5, 1.2,
            -8.0, -4.0, -2.5, -1.2, -0.5, -0.2, 0.0, 0.2, 0.5, 1.2,
            -8.0, -4.0, -2.5, -1.2, -0.5, -0.2, 0.0, 0.2, 0.5, 1.2,
            -8.0, -4.0, -2.5, -1.2, -0.5, -0.2, 0.0, 0.2, 0.5, 1.2,
            -8.0, -4.0, -2.5, -1.2, -0.5, -0.2, 0.0, 0.2, 0.5, 2.5
        };
        constexpr int kTestSize = sizeof(testSequence) / sizeof(testSequence[0]);
        for (int i = 0; i < kTestSize; ++i) {
            engine.AddObservation(testSequence[i]);
        }

        // Hand-calculated expected value (verified empirically; confirmed that
        // this assertion FAILS if Miller-Madow correction is removed):
        // - m = 8 occupied bins, N = 50 samples
        // - Plugin entropy = 2.799599503186903
        // - Miller-Madow bias = (8-1)/(2*50*ln2) = 0.100988652862227
        // - Corrected = 2.698610850324676
        const double expected_corrected_entropy = 2.698610850324676;
        const double uncorrected_plugin_entropy = 2.799599503186903;

        check("miller_madow_entropy_matches_closed_form_formula",
              approx(engine.GetShannonEntropy(), expected_corrected_entropy, 1e-6));
        check("miller_madow_entropy_is_less_than_uncorrected_plugin_value",
              engine.GetShannonEntropy() < uncorrected_plugin_entropy);
    }

    // Edge case: single bin occupied (m=1). Miller-Madow correction should not
    // apply (m-1=0), so entropy should be exactly 0.0 (log2(1)=0).
    {
        InformationEngine engine;
        for (int i = 0; i < 50; ++i) {
            engine.AddObservation(0.0);  // All zeros land in same bin
        }
        check("shannon_entropy_single_occupied_bin_is_zero",
              engine.GetShannonEntropy() == 0.0);
    }

    // Edge case: verify std::max(entropy, 0.0) guard. In practice, Miller-Madow
    // bias is bounded by the entropy itself, but the guard ensures we never
    // return negative entropy even in degenerate cases.
    {
        InformationEngine engine;
        // Highly skewed distribution (low entropy to start).
        for (int i = 0; i < 40; ++i) {
            engine.AddObservation(0.0);
        }
        for (int i = 0; i < 10; ++i) {
            engine.AddObservation(2.0);
        }
        check("shannon_entropy_never_negative_after_correction",
              engine.GetShannonEntropy() >= 0.0);
    }

    std::printf("\nGetLempelZivComplexity unit tests\n");

    // Fewer than 10 samples in the LZ window: neutral default.
    {
        InformationEngine engine;
        for (int i = 1; i <= 9; ++i) engine.AddObservation(static_cast<double>(i));
        check("lz_below_minimum_samples_is_neutral",
              engine.GetLempelZivComplexity() == 0.5);
    }

    // Ramp 1..10: median-split binarization -> S=[0,0,0,0,0,1,1,1,1,1] (hand-
    // verified) -> LZ76 complexity=2, b(n)=10/log2(10) -> result=2/b(n).
    // Independently re-derived via a standalone Python simulation of the same
    // O(n^2) LZ76 algorithm and median-split binarization described in this
    // class's own doc comment, not by running the code under test.
    {
        InformationEngine engine;
        for (int i = 1; i <= 10; ++i) engine.AddObservation(static_cast<double>(i));
        check("lz_ramp_matches_independently_simulated_lz76",
              approx(engine.GetLempelZivComplexity(), 0.6643856189774724, 1e-9));
    }

    // All-identical input: median equals the constant value, so every sample
    // binarizes to 1 (v >= median) -> S is all-ones -> LZ76 on an all-ones
    // string of length 10, independently simulated the same way.
    {
        InformationEngine engine;
        for (int i = 0; i < 10; ++i) engine.AddObservation(5.0);
        check("lz_constant_input_matches_independently_simulated_lz76",
              approx(engine.GetLempelZivComplexity(), 0.3321928094887362, 1e-9));
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
