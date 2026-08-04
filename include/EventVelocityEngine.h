// EventVelocityEngine.h — pure, header-only EMA-based event-arrival-rate
// estimator (docs/superpowers/plans/2026-08-04-phase1-hardening.md Task 1;
// lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1).
//
// SCOPE: replaces a windowed-count velocity formula that was mathematically
// capped at (deque capacity / window seconds) regardless of true event rate --
// during high-intensity events (CPI, FOMC, US open) ES tick rates can exceed
// 500-1000/sec, but the old formula could never report above 50.0. This
// computes an exponential moving average of inter-arrival TIME instead of
// counting arrivals in a fixed window, giving an O(1), uncapped, continuously
// reactive velocity with constant memory and no loop overhead. No Sierra Chart
// types, natively unit-testable (tests/cpp/test_event_velocity_engine.cpp).
//
// tauUs is the EMA's time constant, in microseconds -- pass the same window
// the old formula used (2 seconds) to preserve its intended responsiveness
// while removing its cap.

#pragma once

#include <cmath>
#include <cstdint>

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

    if (state.emaIntervalUs <= 0.0) {
        state.emaIntervalUs = dtUs;  // seed on the first real interval
    } else {
        const double alpha = 1.0 - std::exp(-dtUs / tauUs);
        state.emaIntervalUs = alpha * dtUs + (1.0 - alpha) * state.emaIntervalUs;
    }

    return static_cast<float>(1'000'000.0 / state.emaIntervalUs);
}

}  // namespace eve
