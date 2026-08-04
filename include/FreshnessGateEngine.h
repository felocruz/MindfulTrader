// FreshnessGateEngine.h — pure, header-only freshness/age comparison for the
// TS1/TS2 readiness gates (docs/superpowers/plans/2026-08-04-phase1-hardening.md
// Task 3; lbrnet/logs/rc_gemini.log GEMINI_BRIEF_082 §1.3).
//
// SCOPE: the actual "is this timestamp too old" arithmetic, extracted so it's
// independently testable. Does NOT decide whether a weekend-grace bypass
// SHOULD apply -- that's real ACSIL day-of-week/session-time detection living
// in StudyHelperFunctions.h's IsPostWeekendReopenGracePeriod(), which cannot be
// unit-tested the same way (no Sierra Chart types allowed in this header). The
// caller (ContextManager::AreTs1DimsReady/AreTs2StructuralDimsReady) combines
// both: it calls IsFresh() with bypassCheck already resolved by the caller
// chain, so this file has no Sierra Chart dependency at all.
//
// When bypassCheck is true, the caller has already determined we are within
// the post-weekend-reopen grace window (e.g. Sunday, at or after session open,
// within the same number of hours as the readiness max-age budget) -- in that
// specific window, TS1/TS2's slowly-evolving regime dims (Hurst exponent,
// tail index, log-variance-ratio, recurrence rate, fractal dimension) are
// still a reasonable approximation of the current regime, since the market
// was CLOSED for the entire gap and could not have produced new information
// to be stale relative to. bypassCheck does not affect any other failure mode
// (never-written, non-finite dims, out-of-contract values) -- those are
// checked by the caller before IsFresh() is ever reached.

#pragma once

#include <cstdint>

namespace fge {

inline bool IsFresh(uint64_t nowUs, uint64_t lastWriteUs, uint64_t maxAgeUs, bool bypassCheck) {
    if (bypassCheck) {
        return true;
    }

    if (nowUs >= lastWriteUs) {
        return (nowUs - lastWriteUs) <= maxAgeUs;
    }

    // Defensive for replay seeks or chart timeline jumps.
    return (lastWriteUs - nowUs) <= maxAgeUs;
}

}  // namespace fge
