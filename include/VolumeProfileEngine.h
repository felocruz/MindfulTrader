// VolumeProfileEngine.h — pure, header-only Point-of-Control / Value-Area
// computation from a price->volume histogram (docs/ADR/sierra_chart_data_feed_setup.md;
// docs/superpowers/plans/2026-08-04-volume-profile-daily-bias.md;
// design reviewed lbrnet/logs/rc_gemini.log GEMINI_REVIEW_077).
//
// SCOPE: given an already-accumulated set of (price, volume) pairs — e.g. one
// trading day's worth of TS3's own sc.VolumeAtPriceForBars, summed across all
// of that day's 15-min bars by the ACSIL caller — compute the Point of
// Control (the price level with the most volume) and the Value Area bounds
// via the standard 2-row Market Profile expansion: compare the sum of the
// next two ticks above VAH against the sum of the next two ticks below VAL,
// expand by two ticks toward the winning side, expand both sides on an exact
// tie, until ValueAreaPercentage of total volume is captured. Represented as
// a dense, tick-indexed array (not a map) so an untraded tick is a real
// zero-volume position the 2-row lookahead can see past, not a skipped node.
// No Sierra Chart types, natively unit-testable
// (tests/cpp/test_volume_profile_engine.cpp).

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace vpe {

struct PriceVolume {
    int32_t priceInTicks;
    double volume;
};

struct ValueArea {
    int32_t pocPriceInTicks = 0;
    int32_t valueAreaHighInTicks = 0;
    int32_t valueAreaLowInTicks = 0;
    bool valid = false;  // false if input was empty or carried no real volume
};

inline ValueArea ComputeValueArea(const std::vector<PriceVolume>& levels, double valueAreaPercentage = 70.0) {
    ValueArea result;
    if (levels.empty()) return result;

    int32_t minPrice = levels.front().priceInTicks;
    int32_t maxPrice = levels.front().priceInTicks;
    for (const auto& lv : levels) {
        minPrice = std::min(minPrice, lv.priceInTicks);
        maxPrice = std::max(maxPrice, lv.priceInTicks);
    }

    const size_t numTicks = static_cast<size_t>(maxPrice - minPrice) + 1;
    std::vector<double> volumeByOffset(numTicks, 0.0);
    double totalVolume = 0.0;
    for (const auto& lv : levels) {
        volumeByOffset[static_cast<size_t>(lv.priceInTicks - minPrice)] += lv.volume;
        totalVolume += lv.volume;
    }
    if (totalVolume <= 0.0) return result;

    // POC = the offset with the most volume. Ties resolve to the lowest
    // price (first offset) encountered during the scan, for determinism.
    size_t pocOffset = 0;
    for (size_t i = 1; i < numTicks; ++i) {
        if (volumeByOffset[i] > volumeByOffset[pocOffset]) pocOffset = i;
    }

    size_t lowOffset = pocOffset;
    size_t highOffset = pocOffset;
    double captured = volumeByOffset[pocOffset];
    const double targetVolume = totalVolume * (valueAreaPercentage / 100.0);

    auto sumAbove = [&](size_t fromOffset) {
        double sum = 0.0;
        for (size_t i = fromOffset + 1; i <= fromOffset + 2 && i < numTicks; ++i) {
            sum += volumeByOffset[i];
        }
        return sum;
    };
    auto sumBelow = [&](size_t fromOffset) {
        double sum = 0.0;
        for (size_t i = 1; i <= 2 && fromOffset >= i; ++i) {
            sum += volumeByOffset[fromOffset - i];
        }
        return sum;
    };

    while (captured < targetVolume) {
        const bool haveAbove = highOffset + 1 < numTicks;
        const bool haveBelow = lowOffset >= 1;
        if (!haveAbove && !haveBelow) break;  // exhausted both sides of the distribution

        const double aboveVolume = haveAbove ? sumAbove(highOffset) : 0.0;
        const double belowVolume = haveBelow ? sumBelow(lowOffset) : 0.0;

        if (haveAbove && (!haveBelow || aboveVolume > belowVolume)) {
            highOffset = std::min(highOffset + 2, numTicks - 1);
            captured += aboveVolume;
        } else if (haveBelow && (!haveAbove || belowVolume > aboveVolume)) {
            lowOffset -= std::min<size_t>(2, lowOffset);
            captured += belowVolume;
        } else {
            // Exact tie with both sides available: expand both, per the
            // standard convention (maintains distribution symmetry).
            highOffset = std::min(highOffset + 2, numTicks - 1);
            lowOffset -= std::min<size_t>(2, lowOffset);
            captured += aboveVolume + belowVolume;
        }
    }

    result.pocPriceInTicks = minPrice + static_cast<int32_t>(pocOffset);
    result.valueAreaHighInTicks = minPrice + static_cast<int32_t>(highOffset);
    result.valueAreaLowInTicks = minPrice + static_cast<int32_t>(lowOffset);
    result.valid = true;
    return result;
}

}  // namespace vpe
