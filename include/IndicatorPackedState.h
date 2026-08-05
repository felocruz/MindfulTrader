#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace mts {

// Pure, header-only, key-agnostic packed storage for all published indicator
// values. N_I8/N_F32 are the exact block sizes from IndicatorLayout.h's audit
// (docs/superpowers/plans/2026-08-05-indicator-manager-dod-soa.md, Task 2/3).
// Position-based: the caller (IndicatorManager) maps an IndicatorKey to a
// position via kIndicatorLayout and calls these by position — this class knows
// nothing about IndicatorKey at all.
//
// m_prevI8/m_prevF32 are NOT a dirty-bit convenience that could be dropped —
// they are read by ShouldTrigger()-style entered/exited transition logic for
// at least 9 indicator families (see the design spec, §3.1). Do not remove them.
template <size_t N_I8, size_t N_F32>
class IndicatorPackedState {
public:
    int8_t GetI8(size_t pos) const { return m_currentI8[pos]; }
    int8_t GetPrevI8(size_t pos) const { return m_prevI8[pos]; }
    float GetF32(size_t pos) const { return m_currentF32[pos]; }
    float GetPrevF32(size_t pos) const { return m_prevF32[pos]; }

    void SetI8(size_t pos, int8_t value, uint64_t keyBit) {
        if (value != m_currentI8[pos]) {
            m_prevI8[pos] = m_currentI8[pos];
            m_currentI8[pos] = value;
            m_dirtyMask |= keyBit;
        }
    }

    void SetF32(size_t pos, float value, uint64_t keyBit) {
        if (value != m_currentF32[pos]) {
            m_prevF32[pos] = m_currentF32[pos];
            m_currentF32[pos] = value;
            m_dirtyMask |= keyBit;
        }
    }

    bool IsDirty(uint64_t keyBit) const { return (m_dirtyMask & keyBit) != 0; }
    uint64_t DirtyMask() const { return m_dirtyMask; }
    void ClearDirtyMask() { m_dirtyMask = 0; }

    void Reset(const std::array<int8_t, N_I8>& defaultsI8,
               const std::array<float, N_F32>& defaultsF32) {
        m_currentI8 = defaultsI8;
        m_prevI8 = defaultsI8;
        m_currentF32 = defaultsF32;
        m_prevF32 = defaultsF32;
        m_dirtyMask = 0;
    }

private:
    alignas(64) std::array<int8_t, N_I8>  m_currentI8{};
    alignas(64) std::array<int8_t, N_I8>  m_prevI8{};
    alignas(64) std::array<float,  N_F32> m_currentF32{};
    alignas(64) std::array<float,  N_F32> m_prevF32{};
    uint64_t m_dirtyMask = 0;
};

}  // namespace mts
