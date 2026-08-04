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

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
