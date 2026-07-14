#include "MindfulTrader_Precompiled.h"

// --- Impulse ---
void Impulse::SetFromColor(const int color, const int prevColor, const float maDiff, const float macdDiff, const float atr)
{
    ImpulseEnum newValue = ImpulseEnum::UNDEFINED;

    switch (color)
    {
    case GREEN:
        newValue = (prevColor == BLUE) ? ImpulseEnum::BLUE_TO_GREEN : ImpulseEnum::GREEN;
        break;
    case RED:
        newValue = (prevColor == BLUE) ? ImpulseEnum::BLUE_TO_RED : ImpulseEnum::RED;
        break;
    case BLUE:
        if (prevColor == GREEN) {
            // v5.3 Event-driven split: maDiff polarity known at this tick,
            // no need to wait for bar close.
            newValue = (maDiff > 0.0f) ? ImpulseEnum::GREEN_TO_BLUE_BULL
                                       : ImpulseEnum::GREEN_TO_BLUE_BEAR;
        } else if (prevColor == RED) {
            newValue = (maDiff < 0.0f) ? ImpulseEnum::RED_TO_BLUE_BEAR
                                       : ImpulseEnum::RED_TO_BLUE_BULL;
        } else {
            // v5.1 Blue Bug Fix: Determine blue polarity from EMA direction.
            if (maDiff > 0.0f)
                newValue = ImpulseEnum::BLUE_BULL;
            else if (maDiff < 0.0f)
                newValue = ImpulseEnum::BLUE_BEAR;
            else
                newValue = ImpulseEnum::BLUE;  // True neutral (maDiff exactly 0, rare)
        }
        break;
    }

    // --- v5.2 Institutional-grade derived metrics ---

    // 1. Magnitude: ATR-normalized momentum strength, clamped to [-1, +1].
    //    Combines EMA slope and MACD-H slope into one continuous signal.
    m_prevMagnitude = m_magnitude;
    if (atr > 0.00001f) {
        float maComponent  = maDiff / atr;
        float macdComponent = macdDiff / atr;
        m_magnitude = std::clamp((maComponent + macdComponent) * 0.5f, -1.0f, 1.0f);
    } else {
        m_magnitude = 0.0f;
    }

    // 2. Fatigue: Δ(magnitude).  Positive = accelerating, negative = fading.
    m_fatigue = m_magnitude - m_prevMagnitude;

    // 3. Run length: consecutive bars in the same color bucket.
    //    We bucket by raw color (GREEN/RED/BLUE), not by the refined enum.
    bool sameColor = (color == prevColor);
    if (sameColor && m_runLength < 255) {
        ++m_runLength;
    } else if (!sameColor) {
        m_runLength = 1;
    }

    // 4. Transition rate: fraction of recent 16 bars with a color change.
    //    Shift history left, set bit-0 if color changed this bar.
    m_colorHistory = static_cast<uint16_t>((m_colorHistory << 1) | (sameColor ? 0u : 1u));
    // popcount via compiler intrinsic for the 16-bit window
    m_transitionRate = static_cast<float>(__builtin_popcount(m_colorHistory)) / 16.0f;

    Update(newValue);
}

// --- Macd ---
void Macd::SetFromChart(SCSubgraphRef MACD_Diff, int Index)
{
    m_prevMacdValue = m_macdValue;
    m_macdValue = MACD_Diff[Index];

    // ── Robust z-score (median/MAD) — Taleb fat-tail safe ──
    // Mirrors FI2Signal::setFromChart normalization.
    // MACD histogram is unbounded (Extremistan) → standard z is forbidden.
    m_macdHistory[m_historyIdx] = m_macdValue;
    m_historyIdx = (m_historyIdx + 1) % kLookback;
    if (m_historyCount < kLookback) ++m_historyCount;

    if (m_historyCount >= 5) {
        std::array<double, kLookback> scratch;
        std::copy_n(m_macdHistory.begin(), m_historyCount, scratch.begin());
        const int n = m_historyCount;
        const int mid = n / 2;

        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
        double median = scratch[mid];

        for (int i = 0; i < n; ++i)
            scratch[i] = std::abs(m_macdHistory[i] - median);
        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
        double mad = scratch[mid];

        double denom = mad * kMADConsistency;
        m_zScore = (denom > 1e-12)
            ? static_cast<float>((m_macdValue - median) / denom)
            : 0.0f;
    }

    MacdEnum newValue = MacdEnum::AT_ZERO;

    // Check for zero-line crossings (highest priority)
    // Check specific patterns first (ZERO_FROM_BELOW/ABOVE require 3 bars + momentum)
    // Then evaluate simple crosses (BULLISH/BEARISH_CROSS require 2 bars only)
    if (Index >= 2) {
        // ZERO_FROM_BELOW: Crossing to positive with momentum (3-bar pattern)
        if (IsZeroFromBelow(MACD_Diff, Index)) {
            newValue = MacdEnum::ZERO_FROM_BELOW;
        }
        // ZERO_FROM_ABOVE: Crossing to negative with momentum (3-bar pattern)
        else if (IsZeroFromAbove(MACD_Diff, Index)) {
            newValue = MacdEnum::ZERO_FROM_ABOVE;
        }
        // Bullish cross: Simple cross from negative to positive (2-bar pattern)
        else if (MACD_Diff[Index] > 0.0 && MACD_Diff[Index - 1] <= 0.0) {
            newValue = MacdEnum::BULLISH_CROSS;
        }
        // Bearish cross: Simple cross from positive to negative (2-bar pattern)
        else if (MACD_Diff[Index] < 0.0 && MACD_Diff[Index - 1] >= 0.0) {
            newValue = MacdEnum::BEARISH_CROSS;
        }
        // MACD is positive
        else if (MACD_Diff[Index] > 0.0) {
            if (IsSummer(MACD_Diff, Index))
                newValue = MacdEnum::SUMMER;
            else if (IsFall(MACD_Diff, Index))
                newValue = MacdEnum::FALL;
            else if (IsPositiveTickDown(MACD_Diff, Index))
                newValue = MacdEnum::POS_TICK_DOWN;
            else if (MACD_Diff[Index] > MACD_Diff[Index - 1])
                newValue = MacdEnum::SUMMER;  // Positive and rising (simple case)
            else if (MACD_Diff[Index] < MACD_Diff[Index - 1])
                newValue = MacdEnum::FALL;     // Positive and falling (simple case)
            else
                newValue = MacdEnum::POSITIVE_FLAT;  // Above zero, consolidating
        }
        // MACD is negative
        else if (MACD_Diff[Index] < 0.0) {
            if (IsWinter(MACD_Diff, Index))
                newValue = MacdEnum::WINTER;
            else if (IsSpring(MACD_Diff, Index))
                newValue = MacdEnum::SPRING;
            else if (IsNegativeTickUp(MACD_Diff, Index))
                newValue = MacdEnum::NEG_TICK_UP;
            else if (MACD_Diff[Index] < MACD_Diff[Index - 1])
                newValue = MacdEnum::WINTER;   // Negative and falling (simple case)
            else if (MACD_Diff[Index] > MACD_Diff[Index - 1])
                newValue = MacdEnum::SPRING;    // Negative and rising (simple case)
            else
                newValue = MacdEnum::NEGATIVE_FLAT;  // Below zero, consolidating
        }
        // MACD is exactly zero (no additional pattern - already checked crosses above)
        else {
            newValue = MacdEnum::AT_ZERO;
        }
    }
    // Index < 2: use simplified 2-bar cross detection while history is building
    else if (Index >= 1) {
        // Simple 2-bar crosses when we don't have 3 bars of history yet
        if (MACD_Diff[Index] > 0.0 && MACD_Diff[Index - 1] <= 0.0) {
            newValue = MacdEnum::BULLISH_CROSS;
        }
        else if (MACD_Diff[Index] < 0.0 && MACD_Diff[Index - 1] >= 0.0) {
            newValue = MacdEnum::BEARISH_CROSS;
        }
    }

    Update(newValue);
}

bool Macd::IsSpring(SCSubgraphRef MACD_Diff, int Index) const {
    return ((MACD_Diff[Index] < 0.0) && (MACD_Diff[Index - 1] < 0.0) && (MACD_Diff[Index - 2] < 0.0) &&
        (MACD_Diff[Index] > MACD_Diff[Index - 1]) &&
        (MACD_Diff[Index - 1] >= MACD_Diff[Index - 2]));
}

bool Macd::IsSummer(SCSubgraphRef MACD_Diff, int Index) const {
    return ((MACD_Diff[Index] > 0.0) && (MACD_Diff[Index - 1] > 0.0) && (MACD_Diff[Index - 2] > 0.0) &&
        (MACD_Diff[Index] > MACD_Diff[Index - 1]) &&
        (MACD_Diff[Index - 1] >= MACD_Diff[Index - 2]));
}

bool Macd::IsFall(SCSubgraphRef MACD_Diff, int Index) const {
    return ((MACD_Diff[Index] > 0.0) && (MACD_Diff[Index - 1] > 0.0) && (MACD_Diff[Index - 2] > 0.0) &&
        (MACD_Diff[Index] < MACD_Diff[Index - 1]) &&
        (MACD_Diff[Index - 1] <= MACD_Diff[Index - 2]));
}

bool Macd::IsWinter(SCSubgraphRef MACD_Diff, int Index) const {
    return ((MACD_Diff[Index] < 0.0) && (MACD_Diff[Index - 1] < 0.0) && (MACD_Diff[Index - 2] < 0.0) &&
        (MACD_Diff[Index] < MACD_Diff[Index - 1]) &&
        (MACD_Diff[Index - 1] <= MACD_Diff[Index - 2]));
}

bool Macd::IsPositiveTickDown(SCSubgraphRef MACD_Diff, int Index) const {
    return ((MACD_Diff[Index] > 0.0) && (MACD_Diff[Index - 1] > 0.0) && (MACD_Diff[Index - 2] > 0.0) &&
        (MACD_Diff[Index] < MACD_Diff[Index - 1]) &&
        (MACD_Diff[Index - 1] > MACD_Diff[Index - 2]));
}

bool Macd::IsNegativeTickUp(SCSubgraphRef MACD_Diff, int Index) const {
    return ((MACD_Diff[Index] < 0.0) && (MACD_Diff[Index - 1] < 0.0) && (MACD_Diff[Index - 2] < 0.0) &&
        (MACD_Diff[Index] > MACD_Diff[Index - 1]) &&
        (MACD_Diff[Index - 1] < MACD_Diff[Index - 2]));
}

bool Macd::IsZeroFromBelow(SCSubgraphRef MACD_Diff, int Index) const {
    return ((MACD_Diff[Index] >= 0.0) && (MACD_Diff[Index - 1] < 0.0) && (MACD_Diff[Index - 2] < 0.0) &&
        (MACD_Diff[Index] > MACD_Diff[Index - 1]) &&
        (MACD_Diff[Index - 1] >= MACD_Diff[Index - 2]));
}

bool Macd::IsZeroFromAbove(SCSubgraphRef MACD_Diff, int Index) const {
    return ((MACD_Diff[Index] <= 0.0) && (MACD_Diff[Index - 1] > 0.0) && (MACD_Diff[Index - 2] > 0.0) &&
        (MACD_Diff[Index] < MACD_Diff[Index - 1]) &&
        (MACD_Diff[Index - 1] <= MACD_Diff[Index - 2]));
}

//
// FI13Signal — v5.6: Robust z-score of 13-EMA Force Index (TS1 trend force)
//

FI13Signal::FI13Signal(IndicatorKey key_)
    : Indicator(key_, FI13Enum::UNCLEAR)
{
}

void FI13Signal::setFromChart(double forceValue)
{
    // Store in circular buffer (log-space not needed: FI is already signed)
    m_forceHistory[m_historyIdx] = forceValue;
    m_historyIdx = (m_historyIdx + 1) % kLookback;
    if (m_historyCount < kLookback) ++m_historyCount;

    // Compute robust z-score (need ≥5 samples for stable MAD)
    if (m_historyCount >= 5) {
        // 1. Copy live portion into scratch for nth_element (mutates array)
        std::array<double, kLookback> scratch;
        std::copy_n(m_forceHistory.begin(), m_historyCount, scratch.begin());
        const int n = m_historyCount;
        const int mid = n / 2;

        // 2. Median via nth_element (O(n) average, no heap alloc)
        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
        double median = scratch[mid];

        // 3. MAD = median( |x_i - median| )
        for (int i = 0; i < n; ++i)
            scratch[i] = std::abs(m_forceHistory[i] - median);
        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
        double mad = scratch[mid];

        // 4. Robust z-score: (x - median) / (MAD * 1.4826)
        double denom = mad * kMADConsistency;
        m_zScore = (denom > 1e-12)
            ? static_cast<float>((forceValue - median) / denom)
            : 0.0f;
    }
}

//
// FI2Signal
//

FI2Signal::FI2Signal(IndicatorKey key_)
    : Indicator(key_, FI2Enum::NEUTRAL_OR_TREND_ALIGNED)
{
}

void FI2Signal::setFromChart(const double force, MacdEnum longMacd)
{
    double prev_force = m_force;
    m_force = force;

    // --- v5.3: Robust z-score via median / MAD (Taleb-consistent) ---
    // Store in circular buffer
    m_forceHistory[m_historyIdx] = m_force;
    m_historyIdx = (m_historyIdx + 1) % kLookback;
    if (m_historyCount < kLookback) ++m_historyCount;

    // Compute robust z-score (need ≥5 samples for stable MAD)
    if (m_historyCount >= 5) {
        // 1. Copy live portion into scratch for nth_element (mutates array)
        std::array<double, kLookback> scratch;
        std::copy_n(m_forceHistory.begin(), m_historyCount, scratch.begin());
        const int n = m_historyCount;
        const int mid = n / 2;

        // 2. Median via nth_element (O(n) average, no heap alloc)
        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
        double median = scratch[mid];

        // 3. MAD = median( |x_i - median| )
        for (int i = 0; i < n; ++i)
            scratch[i] = std::abs(m_forceHistory[i] - median);
        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
        double mad = scratch[mid];

        // 4. Robust z-score: (x - median) / (MAD * 1.4826)
        //    1.4826 makes MAD consistent with σ for Gaussian data,
        //    but MAD resists inflation from fat-tail spikes.
        double denom = mad * kMADConsistency;
        m_zScore = (denom > 1e-12)
            ? static_cast<float>((m_force - median) / denom)
            : 0.0f;
    }

    // --- Enum classification (unchanged) ---
    FI2Enum newValue = FI2Enum::NEUTRAL_OR_TREND_ALIGNED;

    switch (longMacd) {
    case MacdEnum::NEG_TICK_UP:
    case MacdEnum::SPRING:
    case MacdEnum::SUMMER:
    case MacdEnum::ZERO_FROM_BELOW:
    case MacdEnum::BULLISH_CROSS:
        if (m_force < m_threshold)
            newValue = FI2Enum::PULLBACK_FOR_LONG;
        else if (m_force > 0 && prev_force < 0)
            newValue = FI2Enum::SIGNAL_UP;
        break;

    case MacdEnum::POS_TICK_DOWN:
    case MacdEnum::FALL:
    case MacdEnum::ZERO_FROM_ABOVE:
    case MacdEnum::WINTER:
    case MacdEnum::BEARISH_CROSS:
        if (m_force > m_threshold)
            newValue = FI2Enum::RALLY_FOR_SHORT;
        else if (m_force < 0 && prev_force > 0)
            newValue = FI2Enum::SIGNAL_DOWN;
        break;

    case MacdEnum::AT_ZERO:
        // Neutral state - default value is already NEUTRAL_OR_TREND_ALIGNED
        break;

    case MacdEnum::POSITIVE_FLAT:
    case MacdEnum::NEGATIVE_FLAT:
        // Flat MACD-H slope = ambiguous by Elder's slope rule; no trade permitted.
        break;
    }

    Update(newValue);
}

//
// VwapIndicator — Session VWAP with daily reset + ATR-normalized distance
//

void VwapIndicator::UpdateVwap(float typicalPrice, float barVolume, float atr, bool newSession)
{
    if (newSession) {
        m_cumPriceVol = 0.0;
        m_cumVol = 0.0;
    }

    if (barVolume <= 0.0f) return;

    m_cumPriceVol += static_cast<double>(typicalPrice) * static_cast<double>(barVolume);
    m_cumVol += static_cast<double>(barVolume);

    if (m_cumVol > 0.0) {
        m_vwapPrice = static_cast<float>(m_cumPriceVol / m_cumVol);
    }

    // Classify position relative to VWAP using ATR bands
    const float dist = typicalPrice - m_vwapPrice;
    m_distanceNorm = (atr > 1e-6f) ? (dist / atr) : 0.0f;

    VwapPositionEnum pos;
    if (m_distanceNorm < -1.0f)       pos = VwapPositionEnum::BELOW_FAR;
    else if (m_distanceNorm < -0.25f) pos = VwapPositionEnum::BELOW_NEAR;
    else if (m_distanceNorm <= 0.25f) pos = VwapPositionEnum::AT_VWAP;
    else if (m_distanceNorm <= 1.0f)  pos = VwapPositionEnum::ABOVE_NEAR;
    else                              pos = VwapPositionEnum::ABOVE_FAR;

    Update(pos);
}

//
// VolumeIndicator — v5.7: Log-volume robust z-score + self-classification + order-flow imbalance
//

void VolumeIndicator::UpdateVolume(float rawVolume, float bidVolume, float askVolume)
{
    if (rawVolume <= 0.0f) return;  // Guard against zero/negative volume

    const double logVol = std::log(static_cast<double>(rawVolume));

    // Store in circular buffer
    m_logVolHistory[m_logVolIdx] = logVol;
    m_logVolIdx = (m_logVolIdx + 1) % kLookback;
    if (m_logVolCount < kLookback) ++m_logVolCount;

    // 1. Compute robust z-score (need ≥5 samples for stable MAD)
    if (m_logVolCount >= 5) {
        std::array<double, kLookback> scratch;
        std::copy_n(m_logVolHistory.begin(), m_logVolCount, scratch.begin());
        const int n = m_logVolCount;
        const int mid = n / 2;

        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
        double median = scratch[mid];

        for (int i = 0; i < n; ++i)
            scratch[i] = std::abs(m_logVolHistory[i] - median);
        std::nth_element(scratch.begin(), scratch.begin() + mid, scratch.begin() + n);
        double mad = scratch[mid];

        double denom = mad * kMADConsistency;
        m_volumeZScore = (denom > 1e-12)
            ? static_cast<float>((logVol - median) / denom)
            : 0.0f;
    }

    // 2. Self-classify VolumeEnum using robust z-score thresholds
    //    Replaces Gaussian GetVolumeEnum() — thresholds are stable under fat tails
    VolumeEnum classified = VolumeEnum::NORMAL;
    if (m_volumeZScore < -2.0f) {
        classified = VolumeEnum::VERY_LOW;
    } else if (m_volumeZScore < -1.0f) {
        classified = VolumeEnum::LOW;
    } else if (m_volumeZScore > 2.0f) {
        classified = VolumeEnum::VERY_HIGH;
    } else if (m_volumeZScore > 1.0f) {
        // Directional split: bid/ask imbalance at high volume = institutional activity
        if (askVolume > bidVolume) {
            classified = VolumeEnum::HIGH_BUY_VOLUME;
        } else if (bidVolume > askVolume) {
            classified = VolumeEnum::HIGH_SELL_VOLUME;
        } else {
            classified = VolumeEnum::HIGH;
        }
    }
    Update(classified);

    // 3. Order-flow imbalance: pure directional signal, orthogonal to magnitude
    //    Bounded [-1, +1] by construction — no normalization needed
    const float totalVol = bidVolume + askVolume;
    m_volumeImbalance = (totalVol > 0.0f)
        ? (askVolume - bidVolume) / totalVol
        : 0.0f;
}

//
// MarketAction
//

IntermediateMarketAction::IntermediateMarketAction(IndicatorKey key_)
    : Indicator(key_, PriceActionEnum::NONE)
{
}

void IntermediateMarketAction::Reset()
{
    Indicator<PriceActionEnum>::Reset();
    m_data = MarketData{};
    m_prevData = MarketData{};
    m_swingHigh = 0.0f;
    m_swingLow = 0.0f;
    m_prevSwingHigh = 0.0f;
    m_prevSwingLow = 0.0f;
    m_upperRejected = false;
    m_lowerRejected = false;
}

void IntermediateMarketAction::setFastEma(float ema_, float topBand_, float bottomBand_)
{
    m_prevData.fastEma = m_data.fastEma;
    m_data.fastEma = ema_;
    m_prevData.upperChan = m_data.upperChan;
    m_data.upperChan = topBand_;
    m_prevData.lowerChan = m_data.lowerChan;
    m_data.lowerChan = bottomBand_;
}

void IntermediateMarketAction::setEma(float ema_)
{
    m_prevData.ema = m_data.ema;
    m_data.ema = ema_;
}

void IntermediateMarketAction::updateSwingHigh(float price)
{
    if (price > 0.0f && price != m_swingHigh) {
        m_prevSwingHigh = m_swingHigh;
        m_swingHigh = price;
    }
}

void IntermediateMarketAction::updateSwingLow(float price)
{
    if (price > 0.0f && price != m_swingLow) {
        m_prevSwingLow = m_swingLow;
        m_swingLow = price;
    }
}

void IntermediateMarketAction::setKeltnerRejection(bool upperRejected, bool lowerRejected)
{
    m_upperRejected = upperRejected;
    m_lowerRejected = lowerRejected;
}

//
// ShortMarketAction
//

ShortMarketAction::ShortMarketAction(IndicatorKey key_)
    : Indicator(key_, PriceActionEnum::NONE)
{
}

void ShortMarketAction::SetPriceValues(int index, float high, float low, float maxHigh, float minLow)
{
    // Update the current index and high/low values
    m_prevHighLowIndex = m_priceData.highLowIndex;

    m_priceData.highLowIndex = index - 1; // Store data for the just-closed bar
    m_priceData.prevHigh = high; // This is sc.High[index - 1] from caller
    m_priceData.prevLow = low;   // This is sc.Low[index - 1] from caller
    m_priceData.maxHigh = maxHigh;
    m_priceData.minLow = minLow;
}

//
// PriceMetricsIndicator
//
void PriceMetricsIndicator::SetOHLC(float open, float high, float low, float close) {
    m_open = open;
    m_high = high;
    m_low = low;
    m_close = close;

    const float range = high - low;
    m_closePercentile = (range > 0.0f) ? (close - low) / range : 0.0f;
}

//
// TimeOfDayIndicator
//
void TimeOfDayIndicator::SetFromDateTime(const SCDateTime& dateTime, bool hasOpenPosition) {
    // Extract time components from SCDateTime
    int hour = 0, minute = 0, second = 0;
    dateTime.GetTimeHMS(hour, minute, second);

    // Convert to minutes since midnight for easier comparison
    int timeInMinutes = hour * 60 + minute;

    // === Globex / Overnight Session Boundaries (Eastern Time) ===
    const int ASIAN_START = 18 * 60;           // 18:00 (6:00 PM)
    const int LONDON_WINDOW_START = 3 * 60;    // 03:00 (3:00 AM)
    const int LONDON_WINDOW_END = 4 * 60;      // 04:00 (4:00 AM)
    const int PRE_MARKET_HOOK_START = 8 * 60 + 30;  // 08:30 (8:30 AM)

    // === Regular Trading Hours Boundaries (Eastern Time) ===
    const int PRE_MARKET_START = 9 * 60;       // 09:00 (9:00 AM)
    const int MARKET_OPEN = 9 * 60 + 30;       // 09:30 (9:30 AM)
    const int OPENING_HOUR_END = 10 * 60 + 30; // 10:30 (10:30 AM)
    const int SWEET_SPOT_END = 12 * 60;        // 12:00 (12:00 PM)
    const int LUNCH_END = 14 * 60;             // 14:00 (2:00 PM)
    const int AFTERNOON_END = 15 * 60;         // 15:00 (3:00 PM)
    const int FINAL_HOUR_END = 15 * 60 + 45;   // 15:45 (3:45 PM)
    const int MARKET_CLOSE = 16 * 60;          // 16:00 (4:00 PM)

    TimeOfDayEnum newValue = TimeOfDayEnum::OVERNIGHT_HOLD;

    // === Globex Session Classification (handles midnight rollover) ===
    // ASIAN_SESSION: 18:00-03:00 ET (crosses midnight)
    if (timeInMinutes >= ASIAN_START || timeInMinutes < LONDON_WINDOW_START) {
        // Special case: If holding overnight position during Globex, classify as OVERNIGHT_HOLD
        if (hasOpenPosition) {
            newValue = TimeOfDayEnum::OVERNIGHT_HOLD;
        } else {
            newValue = TimeOfDayEnum::ASIAN_SESSION;
        }
    }
    // LONDON_WINDOW: 03:00-04:00 ET (key entry window)
    else if (timeInMinutes >= LONDON_WINDOW_START && timeInMinutes < LONDON_WINDOW_END) {
        newValue = TimeOfDayEnum::LONDON_WINDOW;
    }
    // LONDON_TO_PREMARKET: 04:00-08:30 ET
    else if (timeInMinutes >= LONDON_WINDOW_END && timeInMinutes < PRE_MARKET_HOOK_START) {
        newValue = TimeOfDayEnum::LONDON_TO_PREMARKET;
    }
    // PRE_MARKET_HOOK: 08:30-09:00 ET (economic data reaction)
    else if (timeInMinutes >= PRE_MARKET_HOOK_START && timeInMinutes < PRE_MARKET_START) {
        newValue = TimeOfDayEnum::PRE_MARKET_HOOK;
    }
    // === Regular Trading Hours Classification ===
    // PRE_MARKET: 09:00-09:30 ET
    else if (timeInMinutes >= PRE_MARKET_START && timeInMinutes < MARKET_OPEN) {
        newValue = TimeOfDayEnum::PRE_MARKET;
    }
    // OPENING_HOUR: 09:30-10:30 ET
    else if (timeInMinutes >= MARKET_OPEN && timeInMinutes < OPENING_HOUR_END) {
        newValue = TimeOfDayEnum::OPENING_HOUR;
    }
    // SWEET_SPOT: 10:30-12:00 ET (best entry window)
    else if (timeInMinutes >= OPENING_HOUR_END && timeInMinutes < SWEET_SPOT_END) {
        newValue = TimeOfDayEnum::SWEET_SPOT;
    }
    // LUNCH_DEAD_ZONE: 12:00-14:00 ET
    else if (timeInMinutes >= SWEET_SPOT_END && timeInMinutes < LUNCH_END) {
        newValue = TimeOfDayEnum::LUNCH_DEAD_ZONE;
    }
    // AFTERNOON_SESSION: 14:00-15:00 ET
    else if (timeInMinutes >= LUNCH_END && timeInMinutes < AFTERNOON_END) {
        newValue = TimeOfDayEnum::AFTERNOON_SESSION;
    }
    // FINAL_HOUR: 15:00-15:45 ET
    else if (timeInMinutes >= AFTERNOON_END && timeInMinutes < FINAL_HOUR_END) {
        newValue = TimeOfDayEnum::FINAL_HOUR;
    }
    // PM_RUN_ENTRY: 15:45-16:00 ET (conditional entry window)
    else if (timeInMinutes >= FINAL_HOUR_END && timeInMinutes < MARKET_CLOSE) {
        newValue = TimeOfDayEnum::PM_RUN_ENTRY;
    }
    // AFTER_HOURS: 16:00-18:00 ET
    else if (timeInMinutes >= MARKET_CLOSE && timeInMinutes < ASIAN_START) {
        newValue = TimeOfDayEnum::AFTER_HOURS;
    }

    Update(newValue);
}

//
// OvernightExitIndicator
//

// Static helper: Check for gap (windfall) in favor of position
bool OvernightExitIndicator::IsGapInFavor(float overnightEntry, float openPrice, bool isLong) {
    // Invalid price inputs: skip gap classification rather than risking bad math.
    constexpr float MIN_VALID_PRICE = 1e-6f;
    if (overnightEntry <= MIN_VALID_PRICE || openPrice <= MIN_VALID_PRICE) {
        return false;
    }

    // Calculate gap as percentage of entry price
    float gapPercent = (openPrice - overnightEntry) / overnightEntry;

    if (isLong) {
        // LONG: Gap up > 0.5% is a windfall
        return gapPercent > 0.005f;
    } else {
        // SHORT: Gap down > 0.5% (absolute) is a windfall
        return gapPercent < -0.005f;
    }
}

// Static helper: Check if Taylor "Objective Point" target has been hit
bool OvernightExitIndicator::HasHitObjectivePoint(float currentPrice, float prevDayHigh, float prevDayLow, bool isLong) {
    if (isLong) {
        // LONG: Target is previous day's high (Buy Day objective)
        return currentPrice >= prevDayHigh;
    } else {
        // SHORT: Target is previous day's low (Sell Day objective)
        return currentPrice <= prevDayLow;
    }
}

void OvernightExitIndicator::SetFromOvernightContext(
    float overnightEntry,
    float prevDayHigh,
    float prevDayLow,
    float openPrice,
    float currentPrice,
    bool isLong,
    float threeLineOscillator,
    float threeLineOscPrev
) {
    // Step 1: Check for gap (windfall) - PRIORITY EXIT
    // Gap > 0.5% in favor → exit 09:30-09:45 ET
    if (IsGapInFavor(overnightEntry, openPrice, isLong)) {
        Update(OvernightExitTypeEnum::GAP_EXIT);
        return;
    }

    // Step 2: Check Taylor Objective Point (prev day H/L target)
    if (HasHitObjectivePoint(currentPrice, prevDayHigh, prevDayLow, isLong)) {
        Update(OvernightExitTypeEnum::OBJECTIVE_POINT_EXIT);
        return;
    }

    // Step 3: Check 3-10 Oscillator crossover (momentum failure)
    // This detects when fast EMA (3) crosses slow EMA (16) against position
    bool momentumFailure = false;
    if (isLong) {
        // LONG: Fast line crosses below slow line (bearish signal)
        // Oscillator goes from positive to zero/negative
        momentumFailure = (threeLineOscPrev > 0.0f) && (threeLineOscillator <= 0.0f);
    } else {
        // SHORT: Fast line crosses above slow line (bullish signal)
        // Oscillator goes from negative to zero/positive
        momentumFailure = (threeLineOscPrev < 0.0f) && (threeLineOscillator >= 0.0f);
    }

    if (momentumFailure) {
        Update(OvernightExitTypeEnum::MOMENTUM_FAILURE_EXIT);
        return;
    }

    // Step 4: Check flat or unfavorable open
    // If open didn't gap in our favor, exit on first bounce (09:30-10:00 ET)
    float openMove = openPrice - overnightEntry;
    bool flatOrUnfavorable = false;
    if (isLong) {
        flatOrUnfavorable = (openMove <= 0.0f);  // LONG: open flat or lower
    } else {
        flatOrUnfavorable = (openMove >= 0.0f);  // SHORT: open flat or higher
    }

    if (flatOrUnfavorable) {
        Update(OvernightExitTypeEnum::FIRST_REACTION_EXIT);
        return;
    }

    // No exit trigger met - continue holding for target
    Update(OvernightExitTypeEnum::HOLD_FOR_TARGET);
}


// --- MarketClimateIndicator ---
// Elite v3.2: Reads from unified LocalRiskContext (ContextManager single source of truth)
void MarketClimateIndicator::UpdateContext(const LocalRiskContext& ctx, HMMStateEnum hmmState) {
    MarketClimate newClimate = MarketClimate::GAUSSIAN_STABLE;

    const float kurtosis = ctx.talebKurtosis;
    const float entropy = ctx.shannonFlowEntropy;
    const float hurst = ctx.hurstExponent;

    // --- STEP 1: PHYSICS VETO (SAFETY FIRST) ---
    // "The Gang" Rules:
    // Taleb: Kurtosis > 4.0 = Fat Tail Risk. Stop fading moves.
    // Shannon: Entropy > 0.6 = Random Walk. Stop trend following.

    bool isFragile = (kurtosis > 4.0f);
    bool isChaos = (entropy > 0.6f);

    if (isFragile) {
        newClimate = MarketClimate::TALEBIAN_FRAGILE; // Fat Tails -> Risk Off (Hard Override)
    } else if (isChaos) {
        newClimate = MarketClimate::SHANNON_CHAOS;    // High Entropy -> Mean Rev Only (Hard Override)
    }
    else {
        // --- STEP 2: PHYSICS+HMM HYBRID CLASSIFICATION ---
        // If Physics isn't screaming "Danger", we look for structural opportunities.

        bool isPhysicsMomentum = (hurst > 0.6f && entropy < 0.45f); // Tighter entropy for trend
        bool isPhysicsCoil = (hurst < 0.4f && entropy < 0.4f);

        bool isHmmMomentum = (hmmState == HMMStateEnum::PARETO_MOMENTUM || hmmState == HMMStateEnum::GAUSSIAN_STABLE);
        bool isHmmMeanRev  = (hmmState == HMMStateEnum::COILED_SPRING || hmmState == HMMStateEnum::GAUSSIAN_FRAGILE);

        // HYBRID DECISION MATRIX:
        // 1. Pareto Momentum: Requires Physics OR (HMM + decent Physics)
        if (isPhysicsMomentum || (isHmmMomentum && entropy < 0.5f && hurst > 0.55f)) {
            newClimate = MarketClimate::PARETO_MOMENTUM;
        }
        // 2. Coiled Spring: Requires Physics OR (HMM + decent Physics)
        else if (isPhysicsCoil || (isHmmMeanRev && entropy < 0.5f)) {
            newClimate = MarketClimate::COILED_SPRING;
        }
    }

    // Update Duration Logic
    if (newClimate == Value()) {
        m_stateDuration++;
    } else {
        m_stateDuration = 0;
    }

    // Feed regime duration back to ContextManager's LocalRiskContext
    ContextManager::Instance().SetRegimeDuration(m_stateDuration);

    Update(newClimate);
}

// --- Hurst Exponent (DFA) ---
float HurstExponentIndicator::Calculate(SCStudyInterfaceRef sc, SCFloatArrayRef InputData, int length, int minScale)
{
    // Institutional-Grade Robustness: Ensure minimum data requirements
    if (sc.Index < length || length < minScale * 4) // Require at least 4 segments of minScale
        return std::numeric_limits<float>::quiet_NaN();

    // 1. Prepare/Resize Persistent Buffers (Zero-Allocation on Steady State)
    if (m_logReturns.size() != static_cast<size_t>(length)) {
        m_logReturns.resize(length);
        m_profile.resize(length);
        m_logScales.reserve(32);
        m_logFluctuations.reserve(32);
    }

    // 2. Extract Window & Calculate Log Returns
    // r_t = ln(P_t / P_{t-1})
    double sumReturns = 0.0;
    int dataStartIndex = sc.Index - length + 1;

    for (int i = 0; i < length; ++i) {
        int idx = dataStartIndex + i;
        float currentPrice = InputData[idx];
        float prevPrice = InputData[idx - 1]; // Safe since dataStartIndex >= 1 if sc.Index >= length

        float logRet = 0.0f;
        if (prevPrice > 0 && currentPrice > 0)
            logRet = logf(currentPrice / prevPrice);

        m_logReturns[i] = logRet;
        sumReturns += logRet;
    }

    double meanReturn = sumReturns / static_cast<double>(length);

    // 3. Integrate to obtain Profile Y (Cumulative Deviations)
    // Y[k] = sum(r[i] - mean)
    double currentCumSum = 0.0;
    for (int i = 0; i < length; ++i) {
        currentCumSum += (m_logReturns[i] - meanReturn);
        m_profile[i] = currentCumSum;
    }

    // 4. Perform DFA (Detrending & Fluctuation Analysis)
    m_logScales.clear();
    m_logFluctuations.clear();

    // Scale Logic: Step linearly (s = minScale, minScale+1, ... maxScale)
    // L/4 is max scale rule of thumb for statistical validity.
    int maxScale = length / 4;
    if (maxScale <= minScale) return 0.5f;

    // Optimization: Adjust step size for very large windows
    int step = (maxScale - minScale > 50) ? 2 : 1;

    for (int s = minScale; s <= maxScale; s += step) {
        int numSegments = length / s;
        if (numSegments < 1) continue;

        double totalVariance = 0.0;

        // Iterate segments
        for (int v = 0; v < numSegments; ++v) {
            int startIndex = v * s;

            // Fit Local Trend (OLS): y = mx + c (x is 0..s-1)
            double n = static_cast<double>(s);
            double sumX = n * (n - 1.0) * 0.5;
            double sumX2 = n * (n - 1.0) * (2.0 * n - 1.0) / 6.0;
            double denom = n * sumX2 - sumX * sumX;

            if (fabs(denom) < 1e-9) continue;

            double sumY = 0.0;
            double sumXY = 0.0;

            for (int k = 0; k < s; ++k) {
                double val = m_profile[startIndex + k];
                sumY += val;
                sumXY += k * val;
            }

            double slope = (n * sumXY - sumX * sumY) / denom;
            double intercept = (sumY - slope * sumX) / n;

            // Compute Segment Variance (Mean Squared Residuals)
            double ssr = 0.0;
            for (int k = 0; k < s; ++k) {
                double trend = slope * k + intercept;
                double diff = m_profile[startIndex + k] - trend;
                ssr += diff * diff;
            }
            totalVariance += (ssr / n);
        }

        // F(s) = sqrt( mean( F^2(s,v) ) )
        double F_s = sqrt(totalVariance / static_cast<double>(numSegments));

        if (F_s > 1e-12) {
            m_logScales.push_back(log(static_cast<double>(s)));
            m_logFluctuations.push_back(log(F_s));
        }
    }

    // 5. Calculate Hurst Exponent (Slope of Log-Log Plot)
    size_t N = m_logScales.size();
    if (N < 2) return 0.5f;

    double n = static_cast<double>(N);
    double sumX = 0.0, sumY = 0.0, sumXY = 0.0, sumX2 = 0.0;

    for (size_t i = 0; i < N; ++i) {
        sumX += m_logScales[i];
        sumY += m_logFluctuations[i];
        sumXY += m_logScales[i] * m_logFluctuations[i];
        sumX2 += m_logScales[i] * m_logScales[i];
    }

    double denom = n * sumX2 - sumX * sumX;
    if (fabs(denom) < 1e-9) return 0.5f;

    float H = static_cast<float>((n * sumXY - sumX * sumY) / denom);

    // Clamp result to reasonable physical bounds [0, 1.5]
    if (H < 0.0f) H = 0.0f;
    if (H > 1.5f) H = 1.5f;

    return H;
}

// Explicitly instantiate the templates that are used in the project
template class Indicator<ImpulseEnum>;
template class Indicator<EmaEnum>;
template class Indicator<MacdEnum>;
template class Indicator<StochasticEnum>;
template class Indicator<FI13Enum>;
template class Indicator<FI2Enum>;
template class Indicator<DivergenceEnum>;
template class Indicator<TradeSideEnum>;
template class Indicator<PriceActionEnum>;
template class Indicator<RaschkeStrategySetup>;
template class Indicator<RaschkeTacticalTrigger>;
template class Indicator<DailyBiasEnum>;
template class Indicator<VolumeEnum>;
template class Indicator<StructureTest>;
template class Indicator<ATRProximityEnum>;
template class Indicator<EmaProximity>;
template class Indicator<RSI>;
template class Indicator<PriceMetrics>;
template class Indicator<MarketSymbol>;
template class Indicator<TimeOfDayEnum>;
template class Indicator<OvernightExitTypeEnum>;
template class Indicator<KangarooTailEnum>;
template class Indicator<TurtleSoupEnum>;
template class Indicator<MomentumPinballEnum>;
template class Indicator<ElderBreakoutEnum>;
template class Indicator<NR7Enum>;
template class Indicator<Oscillator310CrossEnum>;
template class Indicator<MarketClimate>;
