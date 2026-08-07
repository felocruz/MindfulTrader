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

/// Burstiness Index: coefficient of variation (StdDev/Mean) of inter-arrival
/// times (IATs) over a rolling window of event timestamps (Raschke flow
/// dynamics). >1.0 = bursty/clustered arrivals, <1.0 = more regular than
/// Poisson, ==1.0 = neutral default (also returned during warmup/degenerate
/// input, matching a Poisson process's own CV).
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
    if (timestamps.size() < 4) return 1.0f;  // Need samples for variance

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

    if (iatCount == 0) return 1.0f;  // Default to Poisson

    // Calculate Mean IAT.
    float sum = 0.0f;
    for (size_t i = 0; i < iatCount; ++i) sum += iats[i];
    float mean = sum / static_cast<float>(iatCount);

    // Calculate StdDev IAT.
    float sumSq = 0.0f;
    for (size_t i = 0; i < iatCount; ++i) {
        const float d = iats[i] - mean;
        sumSq += d * d;
    }
    float stdDev = std::sqrt(sumSq / (iatCount > 1 ? static_cast<float>(iatCount - 1) : 1.0f));

    if (mean < 0.0001f) return 1.0f;  // Avoid division by zero

    return stdDev / mean;
}

}  // namespace eve
