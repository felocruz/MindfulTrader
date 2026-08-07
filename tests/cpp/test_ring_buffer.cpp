// test_ring_buffer.cpp — unit tests for RingBuffer<T, Capacity>
// (docs/superpowers/specs/2026-08-07-contextmanager-ring-buffer-dod-design.md)
//
// Build & run natively (no Sierra Chart deps, header-only):
//   g++ -std=c++17 -I include tests/cpp/test_ring_buffer.cpp -o /tmp/rb_test && /tmp/rb_test

#include "RingBuffer.h"

#include <algorithm>
#include <numeric>
#include <cstdio>

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

}  // namespace

int main() {
    std::printf("RingBuffer unit tests\n");

    // Empty buffer starts empty.
    {
        RingBuffer<float, 4> buf;
        check("empty_buffer_size_zero", buf.size() == 0);
        check("empty_buffer_is_empty", buf.empty());
    }

    // push_back increments size and is retrievable via operator[].
    {
        RingBuffer<float, 4> buf;
        buf.push_back(10.0f);
        buf.push_back(20.0f);
        buf.push_back(30.0f);
        check("push_back_size_tracks_count", buf.size() == 3);
        check("push_back_oldest_at_zero", buf[0] == 10.0f);
        check("push_back_middle_at_one", buf[1] == 20.0f);
        check("push_back_newest_at_last", buf[2] == 30.0f);
        check("back_returns_newest", buf.back() == 30.0f);
    }

    // pop_front evicts the oldest element; logical indices shift.
    {
        RingBuffer<float, 4> buf;
        buf.push_back(10.0f);
        buf.push_back(20.0f);
        buf.push_back(30.0f);
        buf.pop_front();
        check("pop_front_decrements_size", buf.size() == 2);
        check("pop_front_new_oldest_at_zero", buf[0] == 20.0f);
        check("pop_front_new_newest_at_one", buf[1] == 30.0f);
    }

    // Sliding window: push-then-conditionally-pop, matching FeatureScaler/
    // StructureEngine's exact call shape, with Capacity = windowSize + 1
    // headroom for the transient overshoot between the two statements.
    {
        constexpr size_t windowSize = 3;
        RingBuffer<float, windowSize + 1> buf;
        for (int i = 1; i <= 10; ++i) {
            buf.push_back(static_cast<float>(i));
            if (buf.size() > windowSize) {
                buf.pop_front();
            }
        }
        check("sliding_window_caps_at_window_size", buf.size() == windowSize);
        check("sliding_window_oldest_is_8", buf[0] == 8.0f);
        check("sliding_window_middle_is_9", buf[1] == 9.0f);
        check("sliding_window_newest_is_10", buf[2] == 10.0f);
    }

    // Wraparound past physical Capacity: many pushes/pops should cycle the
    // physical head index around multiple times without corrupting logical
    // ordering. Capacity=4, 100 total push/pop cycles.
    {
        RingBuffer<float, 4> buf;
        for (int i = 1; i <= 3; ++i) buf.push_back(static_cast<float>(i));  // prime: [1,2,3]
        for (int i = 4; i <= 103; ++i) {
            buf.push_back(static_cast<float>(i));
            buf.pop_front();
        }
        // After priming with [1,2,3] then 100 push+pop cycles (4..103), the
        // buffer holds the last 3 pushed values: [101,102,103].
        check("wraparound_size_stable", buf.size() == 3);
        check("wraparound_oldest_correct", buf[0] == 101.0f);
        check("wraparound_middle_correct", buf[1] == 102.0f);
        check("wraparound_newest_correct", buf[2] == 103.0f);
    }

    // back() is mutable — StructureEngine's isNewBar=false path overwrites
    // the last-pushed element in place rather than pushing a new one.
    {
        RingBuffer<float, 4> buf;
        buf.push_back(1.0f);
        buf.push_back(2.0f);
        buf.back() = 99.0f;
        check("back_mutation_updates_last_element", buf[1] == 99.0f);
        check("back_mutation_does_not_change_size", buf.size() == 2);
    }

    // clear() resets to empty and a subsequent push starts fresh at index 0.
    {
        RingBuffer<float, 4> buf;
        buf.push_back(1.0f);
        buf.push_back(2.0f);
        buf.clear();
        check("clear_resets_size", buf.size() == 0);
        buf.push_back(7.0f);
        check("push_after_clear_starts_at_zero", buf[0] == 7.0f);
    }

    // Forward iterator supports std::accumulate/min_element/max_element and
    // range-based for, oldest -> newest — the exact StructureEngine usage.
    {
        RingBuffer<float, 8> buf;
        for (float v : {5.0f, 1.0f, 9.0f, 3.0f}) buf.push_back(v);

        const float sum = std::accumulate(buf.begin(), buf.end(), 0.0f);
        check("iterator_accumulate_matches_sum", sum == 18.0f);

        const float minV = *std::min_element(buf.begin(), buf.end());
        const float maxV = *std::max_element(buf.begin(), buf.end());
        check("iterator_min_element_correct", minV == 1.0f);
        check("iterator_max_element_correct", maxV == 9.0f);

        float rangeForSum = 0.0f;
        for (float v : buf) rangeForSum += v;
        check("iterator_range_based_for_matches_sum", rangeForSum == 18.0f);

        // Iteration order is oldest -> newest, matching deque's begin()/end().
        int idx = 0;
        const float expected[] = {5.0f, 1.0f, 9.0f, 3.0f};
        bool orderCorrect = true;
        for (float v : buf) {
            if (v != expected[idx]) orderCorrect = false;
            ++idx;
        }
        check("iterator_order_is_oldest_to_newest", orderCorrect);
    }

    // Const buffer supports const iteration and const operator[]/back().
    {
        RingBuffer<float, 4> buf;
        buf.push_back(1.0f);
        buf.push_back(2.0f);
        const RingBuffer<float, 4>& constBuf = buf;
        check("const_operator_bracket", constBuf[0] == 1.0f);
        check("const_back", constBuf.back() == 2.0f);
        const float sum = std::accumulate(constBuf.begin(), constBuf.end(), 0.0f);
        check("const_iterator_accumulate", sum == 3.0f);
    }

    // pop_front on an empty buffer is a no-op, not a crash.
    {
        RingBuffer<float, 4> buf;
        buf.pop_front();
        check("pop_front_on_empty_is_noop", buf.size() == 0);
    }

    std::printf("\n%s (%d failure%s)\n", g_failures == 0 ? "ALL PASS" : "FAILURES",
                g_failures, g_failures == 1 ? "" : "s");
    return g_failures == 0 ? 0 : 1;
}
