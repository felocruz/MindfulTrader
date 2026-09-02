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

/// Burstiness Index: robust coefficient of variation (MAD/median, not
/// StdDev/Mean) of inter-arrival times (IATs) over a rolling window of event
/// timestamps (Raschke flow dynamics). Was a plain CV (StdDev/Mean) --
/// replaced 2026-08-31, same Gaussian-moment-construct fix already applied to
/// log_scale_ratio/log_scale_expansion_ratio (Kim & White 2004: moment
/// statistics are least reliable exactly under the fat-tailed conditions they
/// exist to detect -- this repo's own established FeatureScaler.h
/// RobustLocation() convention, applied here to the raw dim's own
/// construction instead of only the downstream scaling layer).
///
/// Consistency constant is NOT the standard 1.4826 (that's calibrated for
/// MAD-to-sigma consistency under a NORMAL distribution, the wrong reference
/// here) -- it's derived from the Exponential distribution specifically,
/// since inter-arrival times under a Poisson process are Exponential-
/// distributed, and this function's own semantic requires ==1.0 average out
/// under a true Poisson process (matching what it replaces): for X~Exp(rate),
/// median(X)=ln(2)/rate, and MAD(X) (median absolute deviation from that
/// median) solves sinh(d)=1/2, i.e. MAD=arcsinh(1/2)/rate. The ratio
/// median/MAD = ln(2)/arcsinh(1/2) ~= 1.440420 is scale-invariant (the rate
/// cancels), so multiplying MAD/median by this constant reproduces exactly
/// 1.0 for a genuine Poisson arrival process.
///
/// BOUNDED via Goh & Barabási (2008)'s burstiness parameter transform,
/// B=(x-1)/(x+1) applied to the robust ratio above (not their original
/// mean/std CV, which would regress the fat-tail-robust fix just described --
/// this is the same bounding *device*, applied to this codebase's own
/// already-robust estimator). REQUIRED, not optional polish: verified
/// directly against real per-tick MES data (2026-09-02,
/// tools/observation_vector/burstiness_recalibration.cpp against
/// lbrnet/data/raw/mes_ticks.parquet, 471.9M rows) that the UNBOUNDED ratio
/// diverges catastrophically at real tick density -- the median inter-arrival
/// time in a 100-tick window collapses to the timestamp field's own minimum
/// representable resolution (1us) in 73.9% of real windows, and the plain
/// ratio's division-by-near-zero there produced |z| values up to 2.7 million
/// after FeatureScaler's own downstream shrinkage compounded it. Goh-Barabási's
/// transform is exactly the literature-standard fix for this failure mode:
/// it was originally designed to bound the same mean/std CV against the same
/// kind of blowup when inter-event times cluster near zero.
///
/// Range is now exactly [-1, +1]: -1 = perfectly regular spacing (MAD=0),
/// 0 = Poisson-neutral (also the warmup/degenerate-input default -- this
/// changed from the pre-2026-09-02 unbounded scale's neutral point of 1.0),
/// +1 = maximally bursty (approached, never reached exactly, as the window's
/// median inter-arrival time -> 0).
///
/// Extracted from ContextManager::CalculateBurstinessIndex()
/// (docs/superpowers/specs/2026-08-07-contextmanager-ring-buffer-dod-design.md
/// §3.3, Round 2) so it's independently unit-testable -- it has zero ACSIL
/// dependency and was only ever unreachable by a standalone test because it
/// lived as a ContextManager member function, behind sierrachart.h. Templated
/// on Capacity purely so a unit test can exercise every code path (empty,
/// below-minimum, exact-minimum) at a small capacity without needing
/// production's full 100-timestamp window.
template <size_t Capacity>
float CalculateBurstinessIndex(const RingBuffer<uint64_t, Capacity>& timestamps) {
    if (timestamps.size() < 4) return 0.0f;  // Need samples for variance -- Poisson-neutral default

    // Convert timestamps to IATs -- stack-allocated, sized to Capacity (one
    // more than the max possible IAT count, matching the original's own
    // sized-to-max-window convention).
    std::array<float, Capacity> iats{};
    size_t iatCount = 0;

    uint64_t prev = 0;
    bool first = true;
    // Iterate from oldest to newest.
    for (const auto& ts : timestamps) {
        if (first) {
            prev = ts;
            first = false;
            continue;
        }
        // IAT in milliseconds for numerical stability.
        float iat_ms = static_cast<float>(ts - prev) / 1000.0f;
        if (iat_ms < 0.001f) iat_ms = 0.001f;  // Clamp zero IATs
        iats[iatCount++] = iat_ms;
        prev = ts;
    }

    if (iatCount == 0) return 0.0f;  // Poisson-neutral default

    // Poisson-neutral consistency constant: ln(2)/arcsinh(1/2), derived above.
    constexpr float kExponentialConsistency = 1.4404199f;

    // Median IAT -- nth_element at index n/2, matching this repo's own
    // established median/MAD convention (FeatureScaler.h's RobustLocation()):
    // NOT the textbook averaged-middle-two for even n.
    std::array<float, Capacity> sorted = iats;
    const size_t mid = iatCount / 2;
    std::nth_element(sorted.begin(), sorted.begin() + mid, sorted.begin() + iatCount);
    const float median = sorted[mid];

    if (median < 0.0001f) return 0.0f;  // Avoid division by zero -- Poisson-neutral default

    // MAD: median of |iat - median|, same nth_element convention.
    std::array<float, Capacity> absDev{};
    for (size_t i = 0; i < iatCount; ++i) absDev[i] = std::fabs(iats[i] - median);
    std::nth_element(absDev.begin(), absDev.begin() + mid, absDev.begin() + iatCount);
    const float mad = absDev[mid];

    const float robustCv = (mad / median) * kExponentialConsistency;
    // Goh & Barabási (2008) bounded transform, see this function's own doc
    // comment for why: robustCv is always >= 0, so (robustCv + 1) is always
    // >= 1 -- no new division-by-zero risk introduced here.
    return (robustCv - 1.0f) / (robustCv + 1.0f);
}

}  // namespace eve
