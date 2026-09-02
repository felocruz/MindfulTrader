// test_event_velocity_engine.cpp — unit tests for the EMA-based, uncapped event
// velocity estimator (docs/superpowers/plans/2026-08-04-phase1-hardening.md Task 1;
// lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1).
//
// Build & run natively (no Sierra Chart deps, header-only core):
//   g++ -std=c++17 -I include tests/cpp/test_event_velocity_engine.cpp -o /tmp/eve_test && /tmp/eve_test

#include "EventVelocityEngine.h"

#include <cmath>
#include <cstdio>
#include <random>

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

    // Finding 3 regression test (final-review fix round): a large gap (e.g. a
    // weekend) used to get folded directly into emaIntervalUs (alpha~=1 when
    // dtUs >> tauUs), then take ~14 time constants of wall-clock ticks after
    // resumption to decay back down to the true rate. This matters now because
    // a separate weekend-aware freshness gate task on this branch newly lets
    // HMM inference proceed through the Sunday-reopen window, making this lag
    // live-reachable during one of the most information-dense moments of the
    // week. The fix re-seeds (treats the gap like a cold start) instead of
    // smoothing it in, so velocity should recover within 1-2 ticks of resumption.
    {
        VelocityState state;
        const double tauUs = 2'000'000.0;  // 2s, the production value

        // Seed a normal 10ms/100-per-sec cadence before the gap.
        uint64_t t = 0;
        for (int i = 0; i < 50; ++i) {
            t += 10'000;
            UpdateAndGetVelocity(state, t, tauUs);
        }

        // A 49-hour weekend gap (in microseconds).
        const uint64_t kFortyNineHoursUs = 49ULL * 60ULL * 60ULL * 1'000'000ULL;
        t += kFortyNineHoursUs;
        const float vDuringGap = UpdateAndGetVelocity(state, t, tauUs);
        check("velocity_report_during_gap_is_not_a_stale_spike", vDuringGap == 0.0f);

        // Steady 100/sec resumes. Velocity should recover to ~100/sec within the
        // first 1-2 ticks of resumption, not tens of seconds of wall-clock ticks.
        t += 10'000;
        const float vFirstTickAfterResumption = UpdateAndGetVelocity(state, t, tauUs);
        check("velocity_recovers_within_one_tick_of_resumption_after_a_weekend_gap",
              approx(vFirstTickAfterResumption, 100.0f, 0.1f));

        t += 10'000;
        const float vSecondTickAfterResumption = UpdateAndGetVelocity(state, t, tauUs);
        check("velocity_stays_correct_on_the_second_tick_after_resumption",
              approx(vSecondTickAfterResumption, 100.0f, 0.1f));
    }

    std::printf("\nCalculateBurstinessIndex unit tests\n");

    // Fewer than 4 timestamps: neutral Poisson default (0.0 in the bounded
    // [-1,1] Goh-Barabási scale -- was 1.0 in the pre-2026-09-02 unbounded scale).
    {
        RingBuffer<uint64_t, 8> ts;
        ts.push_back(0);
        ts.push_back(1000);
        ts.push_back(2000);
        check("burstiness_below_minimum_samples_is_neutral",
              CalculateBurstinessIndex(ts) == 0.0f);
    }

    // Perfectly regular spacing: IATs are all identical -> MAD=0 -> robust
    // ratio=0.0 -> Goh-Barabási B=(0-1)/(0+1)=-1.0 (the scale's own minimum,
    // matching Goh & Barabási 2008's "perfectly regular process" endpoint).
    {
        RingBuffer<uint64_t, 8> ts;
        for (uint64_t t = 0; t <= 4'000'000; t += 1'000'000) ts.push_back(t);
        check("burstiness_regular_spacing_is_minimum",
              CalculateBurstinessIndex(ts) == -1.0f);
    }

    // Irregular spacing: iats_ms=[500,1500,200,2800]. Hand-computed via this
    // repo's own nth_element(mid=n/2) median/MAD convention (NOT the textbook
    // averaged-middle-two for even n -- see FeatureScaler.h's RobustLocation()
    // for the same convention): sorted=[200,500,1500,2800], mid=4/2=2 ->
    // median=1500. abs devs from 1500: [1300,1000,0,1300], sorted=
    // [0,1000,1300,1300], mid=2 -> MAD=1300. robust_cv =
    // (1300/1500)*1.4404199 = 1.2483639. Goh-Barabási B=(1.2483639-1)/
    // (1.2483639+1) = 0.1104643 (verified via real python3 execution, not
    // hand arithmetic).
    {
        RingBuffer<uint64_t, 8> ts;
        for (uint64_t t : {0ULL, 500'000ULL, 2'000'000ULL, 2'200'000ULL, 5'000'000ULL}) {
            ts.push_back(t);
        }
        check("burstiness_irregular_spacing_matches_hand_computed_robust_cv",
              approx(CalculateBurstinessIndex(ts), 0.1104643f, 1e-4f));
    }

    // Poisson-neutral property: a real Exponential(rate)-distributed IAT
    // sample must converge toward the scale's Poisson-neutral point -- 0.0 in
    // the bounded [-1,1] Goh-Barabási scale (was 1.0 pre-2026-09-02, before
    // the robust ratio was bounded via B=(x-1)/(x+1)) -- this is exactly why
    // the consistency constant is derived from the Exponential distribution
    // instead of reusing the standard normal-consistency 1.4826. Not a
    // hand-picked pass -- a real synthetic verification, large n to average
    // out small-sample MAD/median noise. Tolerance verified against this
    // exact seed's real printed output before being set (not guessed).
    {
        RingBuffer<uint64_t, 4096> ts;
        std::mt19937 rng(42);
        std::exponential_distribution<double> exp_dist(1.0 / 1'000'000.0);  // mean 1s, in us
        uint64_t t = 0;
        ts.push_back(t);
        for (int i = 0; i < 4095; ++i) {
            t += static_cast<uint64_t>(exp_dist(rng)) + 1;  // +1: avoid a zero-IAT clamp artifact
            ts.push_back(t);
        }
        const float b = CalculateBurstinessIndex(ts);
        std::printf("  [info] robust burstiness (Goh-Barabasi bounded) on a real Exponential(mean=1s) sample: %.4f\n", b);
        check("burstiness_converges_to_zero_for_a_real_poisson_process", approx(b, 0.0f, 0.05f));
    }

    // Capacity is independent of the production EVENT_VELOCITY_MAX=100 --
    // exercise a tiny capacity (4) to confirm the template parameter is a
    // genuine size, not a hardcoded assumption leaking in from ContextManager.
    {
        RingBuffer<uint64_t, 4> ts;
        ts.push_back(0);
        ts.push_back(1'000'000);
        ts.push_back(2'000'000);
        ts.push_back(3'000'000);
        check("burstiness_works_at_small_template_capacity",
              CalculateBurstinessIndex(ts) == -1.0f);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
