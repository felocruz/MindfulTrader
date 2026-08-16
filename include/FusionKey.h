// FusionKey.h — bit-index enum for the Predator applicability-mask dispatch
// (mirrors IndicatorKey.h's convention: one enum value per bit position).
//
// This is a SEPARATE bit-space from IndicatorKey/IndicatorManager's own dirty-mask —
// analogous pattern, different concept (which fusion FUNCTIONS are applicable this tick,
// not which INDICATORS are dirty). Do not conflate the two enums or their masks.

#pragma once

#include <cstdint>

enum class FusionKey : uint8_t {
    TAU_STAR = 0,                  // exit-side: TRAP anticipatory threshold (Task 3)
    TURTLE_SOUP_OPTION_A = 1,      // entry-side: Turtle Soup geometric heuristic (Task 4)
    TURTLE_SOUP_OPTION_B = 2,      // entry-side: Turtle Soup classifier (Task 6, not yet wired live)
    MAX_FUSION_KEYS = 3
};
