// RingBuffer.h — pure, header-only fixed-capacity rolling window, replacing
// std::deque<T> at the two call sites (FeatureScaler, StructureEngine) that
// run on every tick and previously incurred ongoing chunk allocation/
// deallocation churn as their windows slid (docs/superpowers/specs/
// 2026-08-07-contextmanager-ring-buffer-dod-design.md).
//
// SCOPE: backing storage is a std::array<T, Capacity> member — zero heap
// allocation for the buffer's entire lifetime, matching the ring-buffer
// convention already established by InformationEngine (std::array) and
// TailRiskEngine (std::vector resized once at construction) in this same
// 16D-observation-vector pipeline.
//
// API surface deliberately mirrors std::deque<T> at exactly the operations
// the two call sites use: push_back/pop_front/back()/size()/empty()/clear()/
// operator[](logical index, 0=oldest)/forward iteration. This keeps the
// call-site code unchanged — only the container type swaps.
//
// Capacity must include headroom for the existing "push_back, then
// conditionally pop_front if over window size" call shape, which transiently
// holds one more element than the logical window between those two
// statements — callers size Capacity = maxWindowSize + 1.

#pragma once

#include <array>
#include <cassert>
#include <cstddef>
#include <iterator>

template <typename T, size_t Capacity>
class RingBuffer {
public:
    void push_back(const T& v) {
        assert(m_count < Capacity && "RingBuffer::push_back at full capacity — caller must pop_front first");
        m_data[(m_head + m_count) % Capacity] = v;
        ++m_count;
    }

    void pop_front() {
        if (m_count == 0) return;
        m_head = (m_head + 1) % Capacity;
        --m_count;
    }

    T& back() {
        assert(m_count > 0 && "RingBuffer::back() on empty buffer");
        return m_data[(m_head + m_count - 1) % Capacity];
    }

    const T& back() const {
        assert(m_count > 0 && "RingBuffer::back() on empty buffer");
        return m_data[(m_head + m_count - 1) % Capacity];
    }

    size_t size() const { return m_count; }
    bool empty() const { return m_count == 0; }

    void clear() {
        m_head = 0;
        m_count = 0;
    }

    T& operator[](size_t logicalIndex) {
        assert(logicalIndex < m_count && "RingBuffer::operator[] out of range");
        return m_data[(m_head + logicalIndex) % Capacity];
    }

    const T& operator[](size_t logicalIndex) const {
        assert(logicalIndex < m_count && "RingBuffer::operator[] out of range");
        return m_data[(m_head + logicalIndex) % Capacity];
    }

    // Forward iterator, oldest -> newest. Supports std::accumulate/
    // std::min_element/std::max_element and range-based for.
    class ConstIterator {
    public:
        // Standard iterator typedefs — required by std::iterator_traits for
        // std::accumulate/std::min_element/std::max_element to accept this
        // iterator (LegacyForwardIterator concept).
        using iterator_category = std::forward_iterator_tag;
        using value_type = T;
        using difference_type = std::ptrdiff_t;
        using pointer = const T*;
        using reference = const T&;

        ConstIterator(const RingBuffer* buf, size_t logicalIndex) : m_buf(buf), m_idx(logicalIndex) {}
        const T& operator*() const { return (*m_buf)[m_idx]; }
        ConstIterator& operator++() { ++m_idx; return *this; }
        bool operator!=(const ConstIterator& other) const { return m_idx != other.m_idx; }
        bool operator==(const ConstIterator& other) const { return m_idx == other.m_idx; }
    private:
        const RingBuffer* m_buf;
        size_t m_idx;
    };

    ConstIterator begin() const { return ConstIterator(this, 0); }
    ConstIterator end() const { return ConstIterator(this, m_count); }

private:
    std::array<T, Capacity> m_data{};
    size_t m_head = 0;
    size_t m_count = 0;
};
