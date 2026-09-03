// EventVelocityEngine.h — pure, header-only EMA-based event-arrival-rate
// estimator (docs/superpowers/plans/2026-08-04-phase1-hardening.md Task 1;
// lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1).
//
// SCOPE: replaces a windowed-count velocity formula that was mathematically
// capped at (deque capacity / window seconds) regardless of true event rate --
// during high-intensity events (CPI, FOMC, US open) ES tick rates can exceed
// 500-1000/sec, but the old formula could never report above 50.0. This
// computes an exponential moving average of inter-arrival TIME instead of
// counting arrivals in a fixed window, giving an O(1), continuously reactive
// velocity with constant memory and no loop overhead -- uncapped by design
// (no fixed ceiling like the old formula's 50/sec), though in practice still
// bounded by the resolution of the timestamp source (nowUs comes from a
// millisecond-resolution clock in production, i.e. a practical ceiling around
// 1000 events/sec). No Sierra Chart types, natively unit-testable
// (tests/cpp/test_event_velocity_engine.cpp).
//
// tauUs is the EMA's time constant, in microseconds -- pass the same window
// the old formula used (2 seconds) to preserve its intended responsiveness
// while removing its cap.

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include "RingBuffer.h"

namespace eve {

struct VelocityState {
    uint64_t lastEventUs = 0;
    double emaIntervalUs = 0.0;
};

inline float UpdateAndGetVelocity(VelocityState& state, uint64_t nowUs, double tauUs) {
    if (state.lastEventUs == 0) {
        state.lastEventUs = nowUs;
        return 0.0f;  // first event ever: no interval to measure yet
    }

    const double dtUs = static_cast<double>(nowUs) - static_cast<double>(state.lastEventUs);

    if (dtUs <= 0.0) {
        // Non-monotonic timestamp (replay seek/duplicate tick) -- hold the
        // prior estimate rather than corrupt it with a negative/zero interval.
        // Deliberately do NOT advance state.lastEventUs here: doing so would
        // rewind the anchor to this earlier, out-of-order nowUs, inflating the
        // *next* legitimate forward-moving tick's dtUs and distorting the EMA
        // for one cycle. Leave the last known-good forward timestamp in place.
        return state.emaIntervalUs > 0.0 ? static_cast<float>(1'000'000.0 / state.emaIntervalUs) : 0.0f;
    }

    state.lastEventUs = nowUs;

    // A session break (e.g. a weekend or overnight halt) produces a dtUs many
    // orders of magnitude larger than tauUs. Folding that gap into the EMA as
    // if it were a real inter-arrival interval would snap emaIntervalUs to the
    // huge value (alpha~=1) and then take ~14 time constants of *wall-clock*
    // ticks after resumption to decay back down (Finding 3, final-review fix
    // round). Treat it as a cold start instead: reset emaIntervalUs to 0 so
    // the NEXT interval seeds fresh via the branch above, rather than smoothing
    // the gap itself into the estimate.
    constexpr double kSessionBreakMultiple = 100.0;
    if (dtUs > kSessionBreakMultiple * tauUs) {
        state.emaIntervalUs = 0.0;
        return 0.0f;
    }

    if (state.emaIntervalUs <= 0.0) {
        state.emaIntervalUs = dtUs;  // seed on the first real interval
    } else {
        const double alpha = 1.0 - std::exp(-dtUs / tauUs);
        state.emaIntervalUs = alpha * dtUs + (1.0 - alpha) * state.emaIntervalUs;
    }

    return static_cast<float>(1'000'000.0 / state.emaIntervalUs);
}

/// Burstiness Index: robust Index of Dispersion for Counts (IDC) over K=10
/// fixed-width TIME sub-windows spanning the rolling event-timestamp window
/// (Raschke flow dynamics).
///
/// REDESIGNED 2026-09-02 (2nd revision -- see git history for the intermediate
/// IAT-median/MAD + Goh-Barabási-only attempt, which FAILED real-data
/// validation, see below). Both revisions were reviewed with Gemini
/// (lbrnet/logs/rc_gemini.log CLAUDE_BRIEF_121/122): the diagnosed root cause
/// was that ANY statistic built from inter-arrival TIMES (gaps between
/// consecutive tick timestamps) is structurally tied/degenerate at real tick
/// density -- 73.9% of real 100-tick windows have a median inter-arrival time
/// that collapses to the timestamp field's own minimum resolution (1us), and
/// a bounding transform applied afterward (Goh-Barabási) cannot repair that:
/// it just relabels the same point-mass to a different constant, still
/// collapsing FeatureScaler's own online local-MAD z-scoring the same way
/// (verified: post-transform real-data recalibration was *worse*, not
/// better -- mean|z| roughly doubled).
///
/// THIS revision abandons inter-arrival TIMES entirely and instead measures
/// dispersion of tick COUNTS across K=10 fixed-width time sub-bins spanning
/// the window (Daley & Vere-Jones 2003's Index of Dispersion for Counts, the
/// standard point-process-literature burstiness measure for exactly this
/// class of problem -- a count is always a well-defined non-negative integer
/// regardless of how many raw ticks share the same microsecond timestamp, so
/// the same-timestamp-tie degeneracy cannot occur structurally). Bin width is
/// span/K (NOT a fixed absolute width like "10ms") so it self-scales to
/// whatever the local tick rate actually is, avoiding a mirror-image
/// degeneracy at low tick density that a fixed absolute bin width would
/// introduce (near-empty bins => near-zero mean count => the same kind of
/// blowup on the opposite end).
///
/// Real-data validation (2026-09-02, same recalibration methodology as the
/// failed attempt): a fast Python sanity check across 287,234 real windows
/// (5 row groups spanning the full mes_ticks.parquet date range) found only
/// 0.18% of real windows land exactly at the -1.0 floor and 0.56% at the +1.0
/// ceiling (0.74% total at either extreme) -- no dominant point mass, versus
/// the failed revision's ~74% collapse to one exact value.
///
/// FULL real-data recalibration then confirmed this holds at true production
/// scale (tools/observation_vector/burstiness_recalibration.cpp, all
/// 471,930,891 real ticks, ~1hr runtime, RSS ~3.2GB):
///   mean|z|=1.1356  max|z|=49.28  p50=0.682 p90=2.541 p99=7.959 p99.9=15.870
///   |z|>=6 rate=1.9605% (vs the failed Goh-Barabási-alone attempt's 73.78%,
///   and the original unbounded formula's 73.77% -- both were genuinely
///   broken; this is a normal, sane winsorization clip rate, not a defense
///   mechanism firing on the majority of real data). corr(localMAD,|z|)=
///   -0.0959, still negative but small in magnitude, no longer the dominant
///   collapse signature it was before (was -0.0509/-0.0780 alongside
///   catastrophic |z| magnitudes in the two failed attempts).
///
/// Consistency constant 1.58113883f = sqrt(10)/2, NOT the standard normal
/// constant 1.4826: under the null hypothesis of a homogeneous (non-bursty)
/// arrival process, each of the K=10 bin counts is Poisson(lambda=N/K=10)-
/// distributed (Gemini's derivation, lbrnet/logs/rc_gemini.log
/// CLAUDE_BRIEF_122, independently cross-checked here via exact PMF
/// computation and 5M-sample simulation: Poisson(10)'s true median=10,
/// MAD=2.0 exactly, sigma=sqrt(10), so sigma/MAD=sqrt(10)/2=1.58113883, NOT
/// 1.4826 which is the wrong reference distribution -- that's calibrated for
/// continuous Normal data, not small-integer Poisson counts). A tempting
/// alternative -- deriving sigma/MAD from the *exact* Binomial(N=100,p=0.1)
/// marginal instead of the Poisson(10) approximation, since N is fixed here,
/// not Poisson-distributed -- gives a cleaner-looking sigma/MAD=1.5 exactly,
/// but was verified EMPIRICALLY WORSE: direct Monte Carlo simulation of this
/// exact algorithm (500,000 trials, matching this repo's own
/// nth_element(mid=K/2) median convention, not the textbook averaged-middle-
/// two) shows Gemini's Poisson-based 1.58113883 centers the empirical
/// Poisson-neutral point at median B=0.00000 (essentially exact), while the
/// theoretically-tempting Binomial-based 1.5 gives median B=-0.05263 (worse)
/// and the standard 1.4826 gives -0.06426 (worst) -- the cross-bin negative
/// correlation inherent to a single multinomial draw (bin counts sum to a
/// fixed N) makes the naive single-variable Binomial derivation less accurate
/// than the Poisson approximation for THIS specific across-bin median/MAD
/// statistic. Verified via real execution, not analytic assumption alone.
///
/// Degenerate defaults: span==0 (every tick in the exact same instant) or
/// median count==0 (majority-empty bins) both return +1.0 (maximally bursty
/// by definition -- extreme clustering, not "insufficient data"). Fewer than
/// kMinSamples timestamps returns 0.0 (Poisson-neutral default, insufficient
/// data to assess).
///
/// Range is exactly [-1, +1] via the same Goh & Barabási (2008) bounded
/// transform device as the failed revision (that part of the diagnosis was
/// correct -- boundedness itself is still desirable, it just wasn't
/// sufficient on its own): -1 = perfectly regular (uniform counts across
/// bins, MAD=0), 0 = Poisson-neutral, +1 = maximally bursty.
///
/// Extracted from ContextManager::CalculateBurstinessIndex()
/// (docs/superpowers/specs/2026-08-07-contextmanager-ring-buffer-dod-design.md
/// §3.3, Round 2) so it's independently unit-testable -- it has zero ACSIL
/// dependency and was only ever unreachable by a standalone test because it
/// lived as a ContextManager member function, behind sierrachart.h. Templated
/// on Capacity purely so a unit test can exercise every code path at a small
/// capacity without needing production's full 100-timestamp window.
template <size_t Capacity>
float CalculateBurstinessIndex(const RingBuffer<uint64_t, Capacity>& timestamps) {
    constexpr size_t kBins = 10;
    constexpr size_t kMinSamples = 20;  // avg >=2 ticks/bin -- below this, a 10-bin count estimate is too noisy to trust
    const size_t n = timestamps.size();
    if (n < kMinSamples) return 0.0f;  // Poisson-neutral default -- insufficient data

    const uint64_t first = timestamps[0];
    const uint64_t last = timestamps.back();
    const uint64_t span = last - first;
    if (span == 0) return 1.0f;  // every tick in the exact same instant -- maximal burstiness by definition

    std::array<uint32_t, kBins> counts{};
    for (size_t i = 0; i < n; ++i) {
        const uint64_t dt = timestamps[i] - first;
        size_t bin = static_cast<size_t>((static_cast<double>(dt) / static_cast<double>(span)) * kBins);
        if (bin >= kBins) bin = kBins - 1;
        ++counts[bin];
    }

    std::array<float, kBins> vals{};
    for (size_t i = 0; i < kBins; ++i) vals[i] = static_cast<float>(counts[i]);

    // Median count -- nth_element at index kBins/2, matching this repo's own
    // established median/MAD convention (FeatureScaler.h's RobustLocation()):
    // NOT the textbook averaged-middle-two for even n.
    std::array<float, kBins> sorted = vals;
    constexpr size_t mid = kBins / 2;
    std::nth_element(sorted.begin(), sorted.begin() + mid, sorted.end());
    const float median = sorted[mid];
    if (median <= 0.0f) return 1.0f;  // majority-empty bins -- extreme clustering, maximal burstiness

    // MAD: median of |count - median|, same nth_element convention.
    std::array<float, kBins> absDev{};
    for (size_t i = 0; i < kBins; ++i) absDev[i] = std::fabs(vals[i] - median);
    std::nth_element(absDev.begin(), absDev.begin() + mid, absDev.end());
    const float mad = absDev[mid];

    // Poisson(N/kBins)-consistency scale factor, see this function's own doc
    // comment for the derivation and why it beats the seemingly-more-exact
    // Binomial-derived alternative in practice.
    constexpr float kPoissonConsistency = 1.58113883f;
    const float idc = (kPoissonConsistency * mad) * (kPoissonConsistency * mad) / median;
    // Goh & Barabási (2008) bounded transform: idc is always >= 0, so
    // (idc + 1) is always >= 1 -- no new division-by-zero risk introduced here.
    return (idc - 1.0f) / (idc + 1.0f);
}

}  // namespace eve
