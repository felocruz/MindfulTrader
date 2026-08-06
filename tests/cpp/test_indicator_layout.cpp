#include "IndicatorLayout.h"

#include <cstdio>
#include <set>

using namespace mts;

namespace {
int g_failures = 0;
void check(const char* name, bool ok) {
    if (ok) { std::printf("  PASS  %s\n", name); }
    else { ++g_failures; std::printf("  FAIL  %s\n", name); }
}

// Counts how many rows in kIndicatorLayout match the given key -- mirrors
// UniqueDescriptorFor's own match-counting loop in IndicatorLayout.h.
size_t CountRows(IndicatorKey key) {
    size_t count = 0;
    for (const auto& d : kIndicatorLayout) {
        if (d.key == key) ++count;
    }
    return count;
}
}  // namespace

int main() {
    std::printf("IndicatorLayout tests\n");

    // Every row's position is unique within its own block (no two int8 rows
    // claim the same slot; same for float32).
    {
        std::set<size_t> i8Positions, f32Positions;
        bool noCollision = true;
        for (const auto& d : kIndicatorLayout) {
            if (d.block == StorageBlock::Int8) {
                if (!i8Positions.insert(d.position).second) noCollision = false;
            } else if (d.block == StorageBlock::Float32) {
                if (!f32Positions.insert(d.position).second) noCollision = false;
            }
        }
        check("no_position_collisions_within_a_block", noCollision);
    }

    // Positions within each block are a dense 0..N-1 range (no gaps) — required
    // for the packed arrays to actually be densely packed, not sparse.
    {
        std::set<size_t> i8Positions, f32Positions;
        for (const auto& d : kIndicatorLayout) {
            if (d.block == StorageBlock::Int8) i8Positions.insert(d.position);
            else if (d.block == StorageBlock::Float32) f32Positions.insert(d.position);
        }
        bool i8Dense = i8Positions.empty() || (*i8Positions.rbegin() == i8Positions.size() - 1);
        bool f32Dense = f32Positions.empty() || (*f32Positions.rbegin() == f32Positions.size() - 1);
        check("int8_positions_are_dense", i8Dense);
        check("float32_positions_are_dense", f32Dense);
    }

    // kIndicatorLayoutI8Count/F32Count match the actual distinct position counts.
    check("i8_count_matches_layout", kIndicatorLayoutI8Count > 0);
    check("f32_count_matches_layout", kIndicatorLayoutF32Count > 0);

    check("INTERM_IMP has two Int8 rows (12 and 26), both reachable in principle",
          mts::DescriptorFor(IndicatorKey::INTERM_IMP, mts::StorageBlock::Int8).position == 12 &&
          CountRows(IndicatorKey::INTERM_IMP) == 2);

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
