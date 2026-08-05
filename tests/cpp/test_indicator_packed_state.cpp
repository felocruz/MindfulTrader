#include "IndicatorPackedState.h"
#include "IndicatorLayout.h"
#include "IndicatorComputations.h"

#include <cstdio>

using namespace mts;

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}

// ----------------------------------------------------------------------------
// Task 9 fix regression test (Findings 1 + 2, review round 2026-08-05).
//
// Indicator<T> itself (include/Indicator.h) cannot be instantiated in this
// Linux/WSL test binary -- it transitively includes sierrachart.h, ACSIL's
// Windows-only header (pulls in <windows.h>). FakeIndicator below is a
// line-for-line mirror of Indicator<T>::Update()'s real raw-pointer
// dual-write, AFTER the Finding 2 fix -- it is wired against the REAL
// IndicatorPackedState (RawI8Pointer/RawPrevI8Pointer), not a mock of that
// class, so this exercises the actual dependency the fix relies on. The full
// template's compilation is separately verified by ./build_dll.sh (the
// mandatory cross-compile step), which recompiles the real Indicator.h.
struct FakeIndicator {
    int8_t* packedSlot = nullptr;
    int8_t* packedPrevSlot = nullptr;  // nullptr reproduces the PRE-FIX bug
    int8_t value = 0;
    bool dirty = false;  // mirrors m_dirty_mask_ptr's bit -- CheckTrigger(index)
                          // in production is only ever invoked when this is set.

    void Update(int8_t newValue) {
        dirty = false;
        if (newValue != value) {
            value = newValue;
            dirty = true;
            if (packedSlot) {
                // Mirrors Indicator<T>::Update()'s fixed ordering exactly:
                // shift current into prev BEFORE overwriting current.
                if (packedPrevSlot) {
                    *packedPrevSlot = *packedSlot;
                }
                *packedSlot = newValue;
            }
        }
    }
};

// Mirrors IndicatorManager.cpp's anonymous-namespace EnteredOrExitedNone,
// used by CheckTrigger's KangarooTail/TurtleSoup/MomentumPinball/
// ElderBreakout/NR7 cases.
template <typename Enum>
bool EnteredOrExitedNone(int8_t cur, int8_t prev, Enum noneValue) {
    const int8_t none = static_cast<int8_t>(noneValue);
    const bool entered = (prev == none && cur != none);
    const bool exited = (prev != none && cur == none);
    return entered || exited;
}
}  // namespace

int main() {
    std::printf("IndicatorPackedState tests\n");

    {
        IndicatorPackedState<4, 2> state;
        check("cold_start_i8_is_zero", state.GetI8(0) == 0);
        check("cold_start_f32_is_zero", state.GetF32(0) == 0.0f);
        check("cold_start_dirty_mask_is_zero", state.DirtyMask() == 0);
    }

    // Setting a value that actually changes sets the dirty bit AND updates prev.
    {
        IndicatorPackedState<4, 2> state;
        constexpr uint64_t kBit = 1ULL << 3;
        state.SetI8(0, 5, kBit);
        check("set_i8_updates_current", state.GetI8(0) == 5);
        check("set_i8_sets_dirty_bit", (state.DirtyMask() & kBit) != 0);
        check("set_i8_updates_prev_to_old_value", state.GetPrevI8(0) == 0);
    }

    // Setting the SAME value again does not re-flip the dirty bit's meaning of
    // "changed since last clear" — but it must not corrupt prev either.
    {
        IndicatorPackedState<4, 2> state;
        constexpr uint64_t kBit = 1ULL << 3;
        state.SetI8(0, 5, kBit);
        state.ClearDirtyMask();
        state.SetI8(0, 5, kBit);  // same value again
        check("setting_same_value_again_does_not_set_dirty_bit", (state.DirtyMask() & kBit) == 0);
        check("prev_still_reflects_last_real_change", state.GetPrevI8(0) == 0);
    }

    // Float path mirrors the int8 path.
    {
        IndicatorPackedState<4, 2> state;
        constexpr uint64_t kBit = 1ULL << 7;
        state.SetF32(1, 3.5f, kBit);
        check("set_f32_updates_current", state.GetF32(1) == 3.5f);
        check("set_f32_sets_dirty_bit", (state.DirtyMask() & kBit) != 0);
        check("set_f32_updates_prev_to_old_value", state.GetPrevF32(1) == 0.0f);
    }

    // Reset restores compile-time defaults and clears dirty state for touched keys.
    {
        IndicatorPackedState<4, 2> state;
        constexpr uint64_t kBit = 1ULL << 3;
        state.SetI8(0, 5, kBit);
        std::array<int8_t, 4> defaultsI8 = {9, 0, 0, 0};
        std::array<float, 2> defaultsF32 = {0.0f, 0.0f};
        state.Reset(defaultsI8, defaultsF32);
        check("reset_restores_i8_default", state.GetI8(0) == 9);
        check("reset_restores_i8_prev_to_default_too", state.GetPrevI8(0) == 9);
        check("reset_clears_dirty_mask", state.DirtyMask() == 0);
    }

    // RawI8Pointer/RawF32Pointer (Task 4 dual-write wiring) must alias the
    // same storage as the Get/Set-by-position accessors.
    {
        IndicatorPackedState<4, 2> state;
        *state.RawI8Pointer(0) = 7;
        check("raw_i8_pointer_aliases_current_storage", state.GetI8(0) == 7);
    }
    {
        IndicatorPackedState<4, 2> state;
        *state.RawF32Pointer(1) = 2.5f;
        check("raw_f32_pointer_aliases_current_storage", state.GetF32(1) == 2.5f);
    }

    // ------------------------------------------------------------------
    // Finding 1: UniqueDescriptorFor(KANGAROO_TAIL) must resolve to NotPacked
    // (this IS the bug's root cause -- CheckTrigger's pre-fix code used
    // .position off of this NotPacked result without checking .block, silently
    // getting position 0, which belongs to LONG_MACD). DescriptorFor with an
    // explicit block is the fix, and must resolve to the real row (position 18
    // per kIndicatorLayout).
    {
        constexpr auto broken = UniqueDescriptorFor(IndicatorKey::KANGAROO_TAIL);
        check("finding1_unique_descriptor_for_two_row_key_is_notpacked",
              broken.block == StorageBlock::NotPacked);
        constexpr auto fixed = DescriptorFor(IndicatorKey::KANGAROO_TAIL, StorageBlock::Int8);
        static_assert(fixed.block == StorageBlock::Int8, "KANGAROO_TAIL must resolve to a real Int8 row");
        check("finding1_descriptor_for_explicit_block_resolves_to_real_row", fixed.position == 18);
        check("finding1_real_row_is_not_long_macds_position_0", fixed.position != 0);
    }

    // ------------------------------------------------------------------
    // Finding 2: the raw-pointer dual-write path (Indicator<T>::Update(),
    // Task 4) never shifted current into prev before this fix, so
    // GetPrevI8() silently stayed 0 (the array's zero-init default) forever.
    // For KangarooTailEnum (NONE == 0), that means "entered NONE" fires on
    // EVERY tick the pattern stays active, not just on the real transition --
    // reproduced below, then shown fixed.
    {
        using KT = KangarooTailEnum;
        constexpr size_t pos = DescriptorFor(IndicatorKey::KANGAROO_TAIL, StorageBlock::Int8).position;

        // --- PRE-FIX reproduction: prev pointer never wired (Task 4 as it
        // shipped) -> GetPrevI8 stuck at 0 forever. ---
        {
            IndicatorPackedState<kIndicatorLayoutI8Count, kIndicatorLayoutF32Count> state;
            FakeIndicator kangarooTail;
            kangarooTail.packedSlot = state.RawI8Pointer(pos);
            // packedPrevSlot intentionally left nullptr -- this is the bug.

            kangarooTail.Update(static_cast<int8_t>(KT::BULLISH_STRONG));  // NONE -> BULLISH_STRONG (real entry)
            check("finding2_prefix_first_transition_fires_entered",
                  EnteredOrExitedNone(state.GetI8(pos), state.GetPrevI8(pos), KT::NONE));

            kangarooTail.Update(static_cast<int8_t>(KT::BULLISH_EXTREME));  // BULLISH_STRONG -> BULLISH_EXTREME (still active, NOT a NONE boundary)
            // BUG: because prev never advanced past 0 (== NONE), this
            // interior transition still reads as "entered" -- a false
            // positive on every tick the pattern intensifies/stays active.
            check("finding2_prefix_bug_reproduced_spurious_entered_on_interior_transition",
                  EnteredOrExitedNone(state.GetI8(pos), state.GetPrevI8(pos), KT::NONE));
        }

        // --- POST-FIX: prev pointer wired via RawPrevI8Pointer, mirroring
        // IndicatorManager's real constructor wiring for all 26 Int8-block
        // keys. ---
        {
            IndicatorPackedState<kIndicatorLayoutI8Count, kIndicatorLayoutF32Count> state;
            FakeIndicator kangarooTail;
            kangarooTail.packedSlot = state.RawI8Pointer(pos);
            kangarooTail.packedPrevSlot = state.RawPrevI8Pointer(pos);

            kangarooTail.Update(static_cast<int8_t>(KT::BULLISH_STRONG));  // NONE -> BULLISH_STRONG (real entry)
            check("finding2_postfix_real_entry_transition_fires",
                  EnteredOrExitedNone(state.GetI8(pos), state.GetPrevI8(pos), KT::NONE));

            kangarooTail.Update(static_cast<int8_t>(KT::BULLISH_EXTREME));  // BULLISH_STRONG -> BULLISH_EXTREME (interior, NOT a NONE boundary)
            check("finding2_postfix_interior_transition_does_not_fire",
                  !EnteredOrExitedNone(state.GetI8(pos), state.GetPrevI8(pos), KT::NONE));

            kangarooTail.Update(static_cast<int8_t>(KT::NONE));  // BULLISH_EXTREME -> NONE (real exit)
            check("finding2_postfix_real_exit_transition_fires",
                  EnteredOrExitedNone(state.GetI8(pos), state.GetPrevI8(pos), KT::NONE));

            // Re-sending the same value must not corrupt prev (Update()'s
            // change guard skips the whole dual-write when nothing changed),
            // AND must not leave the dirty bit set -- CheckTrigger(index) is
            // only ever invoked by HasSignificantChange() when dirty, so a
            // cleared dirty bit here means production would never even
            // re-evaluate EnteredOrExitedNone for this tick.
            kangarooTail.Update(static_cast<int8_t>(KT::NONE));
            check("finding2_postfix_repeat_same_value_clears_dirty_so_checktrigger_never_runs",
                  !kangarooTail.dirty);
        }
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
