#include "IndicatorPackedState.h"

#include <cstdio>

using namespace mts;

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
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

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
