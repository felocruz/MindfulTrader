#pragma once

#include "generated/mts_schema_generated.h"

#include <cstdint>

namespace mts::schema_contract::shared_writers {

struct TrainingRootSharedSlice {
    int8_t side;
    int8_t market_symbol;
    int8_t overnight_exit;
    float nh_nl_daily;
    float prev_high;
    float prev_low;
    float prev_day_high;
    float prev_day_low;
    float prev_four_bar_high;
    float prev_four_bar_low;
    float close_percentile;
    float volume_ratio_percent;
    float volume_imbalance;
};

inline constexpr TrainingRootSharedSlice kDefaultTrainingRootSharedSlice{
    0, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

inline void WriteTrainingRootSharedFields(
    MTS::Training::TrainingEventT& event,
    const TrainingRootSharedSlice& values) {
    event.side = values.side;
    event.market_symbol = values.market_symbol;
    event.overnight_exit = values.overnight_exit;
    event.nh_nl_daily = values.nh_nl_daily;
    event.prev_high = values.prev_high;
    event.prev_low = values.prev_low;
    event.prev_day_high = values.prev_day_high;
    event.prev_day_low = values.prev_day_low;
    event.prev_four_bar_high = values.prev_four_bar_high;
    event.prev_four_bar_low = values.prev_four_bar_low;
    event.close_percentile = values.close_percentile;
    event.volume_ratio_percent = values.volume_ratio_percent;
    event.volume_imbalance = values.volume_imbalance;
}

}  // namespace mts::schema_contract::shared_writers
