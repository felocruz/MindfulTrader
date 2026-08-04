// test_information_engine.cpp — unit tests for InformationEngine's
// volatility-standardized bin mapping (docs/superpowers/plans/2026-08-04-phase1-hardening.md
// Task 2; lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1.2).
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I include tests/cpp/test_information_engine.cpp -o /tmp/ie_test && /tmp/ie_test

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

    // THE CORE FIX: the same absolute log-return magnitude must land in a
    // DIFFERENT bin depending on the current (rolling) volatility regime --
    // proving the bins are no longer keyed to a fixed, hardcoded assumed sigma.
    // We can't reach into private state directly, so we observe this through
    // GetRecurrenceRate(), which internally calls the same MapToBin() the
    // histograms use: feed a long run of LOW-volatility returns to build a
    // low-volatility baseline, then check that a MODERATE return now reads as
    // "rare" (low recurrence) -- under the OLD fixed-sigma logic this same
    // absolute magnitude would already be well inside the common bins for any
    // regime, low or high volatility alike.
    {
        InformationEngine engineLowVol;
        // Quiet regime: tiny returns, ~0.00001 (1bp) in magnitude, alternating sign.
        for (int i = 0; i < 400; ++i) {
            engineLowVol.AddObservation((i % 2 == 0) ? 0.00001 : -0.00001);
        }
        // A 0.0005 (50bp) return is 50x this regime's typical magnitude -- should
        // now be a RARE event (low recurrence) in a properly volatility-standardized
        // scheme, not just another "common" bin entry as the old fixed ~2bp-sigma
        // assumption would have made moderately-sized moves look like nothing unusual
        // relative to a HIGH-volatility regime's own typical range.
        const double recurrenceOfModerateMove = engineLowVol.GetRecurrenceRate(0.0005);
        check("moderate_move_is_rare_in_a_quiet_regime", recurrenceOfModerateMove < 0.05);
    }

    {
        InformationEngine engineHighVol;
        // Volatile regime: much larger returns, ~0.0005 (50bp) in magnitude, alternating sign.
        for (int i = 0; i < 400; ++i) {
            engineHighVol.AddObservation((i % 2 == 0) ? 0.0005 : -0.0005);
        }
        // The SAME 0.0005 (50bp) move that was rare in the quiet regime should now
        // be COMMON (high recurrence) in a regime where it's the typical magnitude --
        // this is the volatility-regime-independence the fix is meant to restore.
        const double recurrenceOfSameMoveInVolatileRegime = engineHighVol.GetRecurrenceRate(0.0005);
        check("same_absolute_move_is_common_in_a_volatile_regime", recurrenceOfSameMoveInVolatileRegime > 0.3);
    }

    // Reset() must clear the new rolling-volatility state too, not just the
    // existing histogram/buffer state -- otherwise a fresh engine after Reset()
    // would inherit a stale volatility estimate from before the reset.
    {
        InformationEngine engine;
        for (int i = 0; i < 400; ++i) {
            engine.AddObservation((i % 2 == 0) ? 0.0005 : -0.0005);  // build a high-vol estimate
        }
        engine.Reset();
        for (int i = 0; i < 400; ++i) {
            engine.AddObservation((i % 2 == 0) ? 0.00001 : -0.00001);  // now feed a quiet regime
        }
        const double recurrenceAfterReset = engine.GetRecurrenceRate(0.0005);
        check("reset_clears_stale_volatility_estimate", recurrenceAfterReset < 0.05);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
