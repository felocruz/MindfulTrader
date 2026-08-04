// test_event_velocity_engine.cpp — unit tests for the EMA-based, uncapped event
// velocity estimator (docs/superpowers/plans/2026-08-04-phase1-hardening.md Task 1;
// lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1).
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I include tests/cpp/test_event_velocity_engine.cpp -o /tmp/eve_test && /tmp/eve_test

#include "EventVelocityEngine.h"

#include <cmath>
#include <cstdio>

using namespace eve;

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

bool approx(float a, float b, float relTol = 0.05f) {
    return std::fabs(a - b) <= relTol * std::max(1.0f, std::fabs(b));
}

}  // namespace

int main() {
    std::printf("EventVelocityEngine unit tests\n");

    // First-ever event: no prior interval to measure, velocity is 0.
    {
        VelocityState state;
        const float v = UpdateAndGetVelocity(state, /*nowUs=*/1'000'000, /*tauUs=*/2'000'000.0);
        check("first_event_returns_zero_velocity", v == 0.0f);
    }

    // Steady 100/sec arrival rate (10ms apart) should converge to ~100 events/sec,
    // uncapped -- this is the core fix: the old deque-count formula could never
    // exceed EVENT_VELOCITY_MAX(100)/EVENT_VELOCITY_WINDOW_SEC(2) = 50.0.
    {
        VelocityState state;
        uint64_t t = 0;
        float v = 0.0f;
        for (int i = 0; i < 500; ++i) {
            t += 10'000;  // 10ms = 100 events/sec
            v = UpdateAndGetVelocity(state, t, /*tauUs=*/2'000'000.0);
        }
        check("converges_to_100_events_per_sec_uncapped", approx(v, 100.0f));
        check("genuinely_exceeds_the_old_50_cap", v > 50.0f);
    }

    // Steady 1000/sec (news-spike rate, 1ms apart) -- must not be capped either.
    {
        VelocityState state;
        uint64_t t = 0;
        float v = 0.0f;
        for (int i = 0; i < 2000; ++i) {
            t += 1'000;  // 1ms = 1000 events/sec
            v = UpdateAndGetVelocity(state, t, /*tauUs=*/2'000'000.0);
        }
        check("converges_to_1000_events_per_sec_during_a_news_spike", approx(v, 1000.0f, 0.1f));
    }

    // Non-monotonic timestamp (replay seek/duplicate tick): must not crash or
    // produce a negative/garbage value; should hold the prior estimate steady.
    {
        VelocityState state;
        UpdateAndGetVelocity(state, 1'000'000, 2'000'000.0);
        const float vBefore = UpdateAndGetVelocity(state, 1'010'000, 2'000'000.0);
        const float vAfter = UpdateAndGetVelocity(state, 1'005'000, 2'000'000.0);  // time went backwards
        check("non_monotonic_timestamp_holds_prior_estimate", vAfter == vBefore);
    }

    // Anchor-rewind persistence: a backward jump must not advance state.lastEventUs.
    // If it did, the *next* legitimate forward tick would measure its interval
    // against the earlier, out-of-order timestamp instead of the last known-good
    // forward one, inflating that interval and distorting the EMA for one cycle.
    // Use a small tauUs so a stale anchor's distorted interval measurably moves
    // the EMA/velocity (with tauUs=2s the effect would be too small to detect).
    {
        VelocityState state;
        const double tauUs = 10'000.0;
        UpdateAndGetVelocity(state, 1'000'000, tauUs);   // seed: no interval yet
        UpdateAndGetVelocity(state, 1'010'000, tauUs);   // dt=10ms -> ema seeded at 10000us (~100/sec)
        UpdateAndGetVelocity(state, 1'005'000, tauUs);   // backward jump: must NOT move the anchor

        check("anchor_not_rewound_by_backward_jump", state.lastEventUs == 1'010'000);

        // Next forward tick, 10ms after the last known-good anchor (1,010,000).
        // If the anchor had been incorrectly rewound to 1,005,000, this tick's
        // measured interval would be inflated to 15ms instead of 10ms, pulling
        // the reported velocity down from ~100/sec toward ~72/sec.
        const float v = UpdateAndGetVelocity(state, 1'020'000, tauUs);
        check("velocity_after_backward_jump_uses_last_known_good_anchor", approx(v, 100.0f, 0.1f));
    }

    // Quiet market (10 seconds between events) -- velocity should be very low,
    // not zero (there IS a real, if slow, arrival rate) and not NaN/inf.
    // Note: the first call's nowUs must be non-zero -- 0 is this engine's
    // "uninitialized" sentinel (the same 0-means-never-written convention
    // ContextManager::AreTs1DimsReady already uses for m_ts1MacroLastWriteUs),
    // so a first call with nowUs=0 would be indistinguishable from cold-start
    // and short-circuit before any interval is measured.
    {
        VelocityState state;
        UpdateAndGetVelocity(state, 1'000'000, 2'000'000.0);
        const float v = UpdateAndGetVelocity(state, 11'000'000, 2'000'000.0);
        check("quiet_market_gives_low_finite_velocity", std::isfinite(v) && v > 0.0f && v < 1.0f);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
