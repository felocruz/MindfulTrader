#pragma once

#include "generated/mts_schema_generated.h"

#include <cstdint>

namespace mts::schema_contract::shared_writers {

struct EventRootSharedSlice {
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

inline constexpr EventRootSharedSlice kDefaultEventRootSharedSlice{
    0, 0, 0, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

inline void WriteEventRootSharedFields(
    MTS::Schema::EventBuilder& builder,
    const EventRootSharedSlice& values) {
    builder.add_side(values.side);
    builder.add_market_symbol(values.market_symbol);
    builder.add_overnight_exit(values.overnight_exit);
    builder.add_nh_nl_daily(values.nh_nl_daily);
    builder.add_prev_high(values.prev_high);
    builder.add_prev_low(values.prev_low);
    builder.add_prev_day_high(values.prev_day_high);
    builder.add_prev_day_low(values.prev_day_low);
    builder.add_prev_four_bar_high(values.prev_four_bar_high);
    builder.add_prev_four_bar_low(values.prev_four_bar_low);
    builder.add_close_percentile(values.close_percentile);
    builder.add_volume_ratio_percent(values.volume_ratio_percent);
    builder.add_volume_imbalance(values.volume_imbalance);
}

}  // namespace mts::schema_contract::shared_writers
