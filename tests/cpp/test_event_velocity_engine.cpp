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

    // Fewer than kMinSamples(20) timestamps: neutral Poisson default.
    {
        RingBuffer<uint64_t, 16> ts;
        for (uint64_t t = 0; t < 10'000'000; t += 1'000'000) ts.push_back(t);
        check("burstiness_below_minimum_samples_is_neutral",
              CalculateBurstinessIndex(ts) == 0.0f);
    }

    // Perfectly regular spacing (n=20, evenly spaced): counts land exactly 2
    // per bin across all 10 bins (verified via real python3 execution -- the
    // bin-index arithmetic isn't obviously uniform by inspection for n=20/
    // kBins=10) -> MAD=0 -> idc=0 -> Goh-Barabási B=(0-1)/(0+1)=-1.0 exactly.
    {
        RingBuffer<uint64_t, 24> ts;
        for (uint64_t t = 0; t < 20'000'000; t += 1'000'000) ts.push_back(t);
        check("burstiness_regular_spacing_is_minimum",
              CalculateBurstinessIndex(ts) == -1.0f);
    }

    // Every tick in the exact same instant (span=0): maximal burstiness by
    // definition, not "insufficient data" -- this is the real-world case that
    // broke the old IAT-based formula (median IAT collapsing to the
    // timestamp's own minimum resolution); this formula defines it explicitly
    // rather than letting it fall out of a division.
    {
        RingBuffer<uint64_t, 24> ts;
        for (int i = 0; i < 20; ++i) ts.push_back(5'000'000);
        check("burstiness_all_same_instant_is_maximum", CalculateBurstinessIndex(ts) == 1.0f);
    }

    // Extreme clustering: 19 of 20 ticks at t=0, 1 tick far later -- median
    // bin count is 0 (majority-empty bins), which this formula treats as
    // maximal burstiness (not neutral -- an empty-bin-majority IS the
    // clustering signal, not an absence of data).
    {
        RingBuffer<uint64_t, 24> ts;
        for (int i = 0; i < 19; ++i) ts.push_back(0);
        ts.push_back(100'000'000);
        check("burstiness_extreme_clustering_is_maximum", CalculateBurstinessIndex(ts) == 1.0f);
    }

    // Irregular/moderately-clustered spacing (n=20, a mix of tight sub-100us
    // gaps and much longer gaps -- deliberately not adversarial/degenerate).
    // Bin counts=[4,0,4,0,3,0,4,1,2,2], median=2, MAD=2, idc=(1.58113883*2)^2/2
    // = 4.99999998..., B=(idc-1)/(idc+1)=0.66666666... (verified via real
    // python3 execution against this exact algorithm, not hand arithmetic).
    {
        RingBuffer<uint64_t, 24> ts;
        for (uint64_t t : {0ULL, 50'000ULL, 100'000ULL, 300'000ULL, 1'100'000ULL, 1'150'000ULL,
                           1'200'000ULL, 1'250'000ULL, 2'150'000ULL, 2'250'000ULL, 2'350'000ULL,
                           3'050'000ULL, 3'100'000ULL, 3'150'000ULL, 3'200'000ULL, 3'800'000ULL,
                           4'000'000ULL, 4'050'000ULL, 4'950'000ULL, 5'000'000ULL}) {
            ts.push_back(t);
        }
        check("burstiness_irregular_spacing_matches_verified_idc",
              approx(CalculateBurstinessIndex(ts), 0.6666667f, 1e-4f));
    }

    // Poisson-neutral property: average burstiness across many INDEPENDENT
    // 100-tick windows (matching production's Capacity=100), each drawn from
    // a real Exponential(mean=1s)-distributed IAT sequence (a genuine
    // homogeneous Poisson process), must be close to 0. A SINGLE window's
    // reading has real, expected small-sample noise (only kBins=10 data
    // points feed its own median/MAD) -- averaging many independent windows
    // is the correct way to demonstrate the Poisson-neutral property, not a
    // single giant buffer (this measures a genuinely different, per-window
    // statistic than the old IAT-based formula did). Verified via real
    // python3 execution (not hand-picked): mean over 2000 such windows =
    // 0.0419, well within this test's tolerance.
    {
        std::mt19937 rng(42);
        std::exponential_distribution<double> exp_dist(1.0 / 1'000'000.0);  // mean 1s, in us
        double sum = 0.0;
        constexpr int kTrials = 2000;
        for (int trial = 0; trial < kTrials; ++trial) {
            RingBuffer<uint64_t, 100> ts;
            uint64_t t = 0;
            ts.push_back(t);
            for (int i = 0; i < 99; ++i) {
                t += static_cast<uint64_t>(exp_dist(rng)) + 1;  // +1: avoid a zero-span artifact
                ts.push_back(t);
            }
            sum += CalculateBurstinessIndex(ts);
        }
        const float meanB = static_cast<float>(sum / kTrials);
        std::printf("  [info] mean IDC-based burstiness over %d independent real Poisson-process windows: %.4f\n",
                    kTrials, meanB);
        check("burstiness_converges_to_zero_for_a_real_poisson_process", approx(meanB, 0.0f, 0.1f));
    }

    // Capacity is independent of the production EVENT_VELOCITY_MAX=100 --
    // exercise a capacity just above kMinSamples(20) to confirm the template
    // parameter is a genuine size, not a hardcoded assumption leaking in from
    // ContextManager.
    {
        RingBuffer<uint64_t, 21> ts;
        for (uint64_t t = 0; t < 20'000'000; t += 1'000'000) ts.push_back(t);
        check("burstiness_works_at_small_template_capacity",
              CalculateBurstinessIndex(ts) == -1.0f);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
