#include "messaging/EventSerializerV2.h"
#include "IndicatorManager.h"
#include "Logger.h"
#include <chrono>
#include <algorithm>
#include <cmath>

// Static singleton
static EventSerializerV2* g_eventSerializerV2 = nullptr;

EventSerializerV2& EventSerializerV2::Instance() {
    if (g_eventSerializerV2 == nullptr) {
        g_eventSerializerV2 = new EventSerializerV2();
    }
    return *g_eventSerializerV2;
}

EventSerializerV2::EventSerializerV2()
    : m_fbb(std::make_unique<::flatbuffers::FlatBufferBuilder>(2048)) {
    m_featuresBuffer.reserve(84);
    m_observationBuffer.reserve(10);
}

EventSerializerV2::~EventSerializerV2() {
    m_fbb.reset();
}

std::vector<uint8_t> EventSerializer::SerializeEvent(
    const IndicatorManager& manager,
    int32_t bar_index,
    uint64_t timestamp_us,
    uint64_t sequence_id
) {
    std::vector<uint8_t> result;

    try {
        auto start_time = std::chrono::steady_clock::now();

        // Clear state for fresh serialization
        m_fbb->Clear();
        m_changedKeysOffsets.clear();
        m_featuresBuffer.clear();
        m_observationBuffer.clear();

        // === SECTION 0: Handshake & Metadata ===

        // === SECTION 1-11: Populate all 84 Event fields ===

        // Section 0: Temporal & Identity
        // (populated below in builder)

        // Sections 1-4: Indicators
        PopulateIndicatorEnums(/* builder will be created after we know all offsets */,  manager);

        // Sections 5-7: Context
        PopulateContextFields(/* builder */, manager);

        // Sections 8-10: Price & Execution
        // (populated below)

        // Section 11: Features
        PopulateFeatureVectors(/* builder */, manager);

        // Normalized Fields
        PopulateNormalizedFields(/* builder */, manager);

        // === Build Event table ===
        // Due to FlatBuffers construction order, we need to build bottom-up

        // 1. Create vector fields first (features, observation, changed_keys)
        std::vector<float> features_vec = m_featuresBuffer;
        auto features_offset = m_fbb->CreateVector(features_vec);

        std::vector<float> observation_vec = m_observationBuffer;
        if (observation_vec.size() < 10) {
            observation_vec.resize(10, 0.0f);
        }
        auto observation_offset = m_fbb->CreateVector(observation_vec);

        // Create changed_keys vector
        auto changed_keys_offset = m_fbb->CreateVector(m_changedKeysOffsets);

        // 2. Start building Event
        MTS::Schema::EventBuilder event_builder(*m_fbb);

        // Section 0: Handshake & Identity
        event_builder.add_sequence_id(sequence_id);

        // Section 1: Temporal & Reference
        event_builder.add_bar_index(bar_index);
        event_builder.add_timestamp_us(static_cast<int64_t>(timestamp_us));
        event_builder.add_is_event_driven(true);

        // Physics & Velocity
        event_builder.add_delta_t_log(0.0f);  // TODO: compute from indicatorManager
        event_builder.add_tau_100_log(0.0f);
        event_builder.add_event_velocity(0.0f);

        // HMM & Climate
        event_builder.add_hmm_state(0);
        event_builder.add_market_climate(0);

        // === Indicator Enums (31 fields, int8) ===
        // These need to come from IndicatorManager indicator values
        // Placeholder: set all to 0
        event_builder.add_long_macd(0);
        event_builder.add_long_FI13_signal(0);
        event_builder.add_long_macd_divergence(0);
        event_builder.add_long_imp(0);
        event_builder.add_long_mkt_action(0);
        event_builder.add_interm_stochastic(0);
        event_builder.add_raschke_strategy_setup(0);
        event_builder.add_raschke_tactical_trigger(0);
        event_builder.add_rsi(0);
        event_builder.add_interm_FI2_signal(0);
        event_builder.add_ema_proximity(0);
        event_builder.add_price_metrics(0);
        event_builder.add_interm_macd(0);
        event_builder.add_interm_macd_divergence(0);
        event_builder.add_interm_imp(0);
        event_builder.add_interm_mkt_action(0);
        event_builder.add_structure_test(0);
        event_builder.add_volume_signal(0);
        event_builder.add_atr_proximity(0);
        event_builder.add_daily_bias(0);
        event_builder.add_short_mkt_action(0);
        event_builder.add_time_of_day(0);
        event_builder.add_kangaroo_tail(0);
        event_builder.add_turtle_soup(0);
        event_builder.add_momentum_pinball(0);
        event_builder.add_elder_breakout(0);
        event_builder.add_nr7(0);
        event_builder.add_nh_nl_signal(0);

        // Quality Scores (5 fields)
        event_builder.add_kangaroo_tail_quality(0.75f);
        event_builder.add_turtle_soup_quality(0.75f);
        event_builder.add_momentum_pinball_quality(0.75f);
        event_builder.add_elder_breakout_quality(0.75f);
        event_builder.add_nr7_quality(0.75f);

        // Section 5-7: Market Context (oscillators & correlations)
        event_builder.add_oscillator_310(0);
        event_builder.add_corr_es_zn(0.0f);
        event_builder.add_corr_es_dx(0.0f);
        event_builder.add_zn_trend(0);
        event_builder.add_dx_trend(0);
        event_builder.add_corr_es_zn_delta(0.0f);
        event_builder.add_corr_es_zn_accel(0.0f);
        event_builder.add_corr_es_dx_delta(0.0f);
        event_builder.add_corr_es_dx_accel(0.0f);

        // Absolute Levels
        event_builder.add_prev_high(0.0f);
        event_builder.add_prev_low(0.0f);
        event_builder.add_prev_day_high(0.0f);
        event_builder.add_prev_day_low(0.0f);
        event_builder.add_prev_four_bar_high(0.0f);
        event_builder.add_prev_four_bar_low(0.0f);

        // Section 8-10: Execution & Price
        event_builder.add_side(0);
        event_builder.add_market_symbol(0);
        event_builder.add_overnight_exit(0);
        event_builder.add_open(0.0f);
        event_builder.add_high(0.0f);
        event_builder.add_low(0.0f);
        event_builder.add_close(0.0f);
        event_builder.add_atr_10(0.0f);
        event_builder.add_volume(0);

        // v5.5: Read bar-context floats from their owning indicators
        const auto* priceMetrics = manager.GetIndicator<PriceMetricsIndicator>(IndicatorKey::PRICE_METRICS);
        event_builder.add_close_percentile(priceMetrics ? priceMetrics->GetClosePercentile() : 0.0f);

        const auto* volumeInd = manager.GetIndicator<VolumeIndicator>(IndicatorKey::VOLUME_SIGNAL);
        event_builder.add_volume_ratio_percent(volumeInd ? volumeInd->GetVolumeRatio() : 0.0f);
        event_builder.add_volume_imbalance(volumeInd ? volumeInd->GetVolumeImbalance() : 0.0f);

        // Section 11: Feature Vectors
        event_builder.add_features(features_offset);
        event_builder.add_observation(observation_offset);
        event_builder.add_changed_keys(changed_keys_offset);

        // Normalized Features
        event_builder.add_volatility(0.0f);
        event_builder.add_efficiency(0.0f);
        event_builder.add_rel_range(0.0f);
        event_builder.add_velocity(0.0f);
        event_builder.add_regime_tenure(0);

        // Normalized Distances
        event_builder.add_dist_day_high(0.0f);
        event_builder.add_dist_day_low(0.0f);
        event_builder.add_dist_four_bar_high(0.0f);
        event_builder.add_dist_four_bar_low(0.0f);
        event_builder.add_dist_ema_13(0.0f);

        // Normalized Features (continued)
        event_builder.add_time_of_day_norm(0.0f);
        event_builder.add_bar_completion_pct(1.0f);
        event_builder.add_long_fi13_norm(
            [&]() -> float {
                const auto* fi13 = manager.GetIndicator<FI13Signal>(IndicatorKey::LONG_FI13_SIGNAL);
                return fi13 ? fi13->ZScore() : 0.0f;
            }());
        event_builder.add_interm_fi2_norm(0.0f);
        event_builder.add_long_macd_norm(0.0f);
        event_builder.add_interm_macd_norm(0.0f);
        event_builder.add_impulse_color(0.0f);
        event_builder.add_daily_bias_encoded(0.0f);
        event_builder.add_corr_velocity(0.0f);

        auto event_offset = event_builder.Finish();
        m_fbb->Finish(event_offset);

        // Extract buffer
        auto buf_ptr = m_fbb->GetBufferPointer();
        auto buf_size = m_fbb->GetSize();
        result.assign(buf_ptr, buf_ptr + buf_size);

        // Update statistics
        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

        m_lastSerializationTimeUs.store(static_cast<int32_t>(duration.count()));
        m_lastEventSizeBytes.store(static_cast<int32_t>(result.size()));
        m_eventCount.fetch_add(1);

        return result;

    } catch (const std::exception& e) {
        Logger::getInstance().log(std::string("EventSerializer::SerializeEvent exception: ") + e.what());
        return result;
    }
}

void EventSerializer::PopulateIndicatorEnums(
    MTS::Schema::EventBuilder& builder,
    const IndicatorManager& manager
) {
    // TODO: Extract indicator enum values from manager
    // Example: manager.GetIndicator("long_macd")->GetEnumValue()
    // For now, placeholder implementation (all zeros)
}

void EventSerializer::PopulateContextFields(
    MTS::Schema::EventBuilder& builder,
    const IndicatorManager& manager
) {
    // TODO: Extract context from ContextManager
    // Example: ContextManager::Instance().GetStatisticalContext().volatility
}

void EventSerializer::PopulateFeatureVectors(
    MTS::Schema::EventBuilder& builder,
    const IndicatorManager& manager
) {
    // TODO: Build feature vector from all indicators
    m_featuresBuffer.resize(84, 0.0f);

    // First 10 elements go to observation vector
    for (int i = 0; i < std::min(10, static_cast<int>(m_featuresBuffer.size())); ++i) {
        m_observationBuffer.push_back(m_featuresBuffer[i]);
    }
}

void EventSerializer::PopulateNormalizedFields(
    MTS::Schema::EventBuilder& builder,
    const IndicatorManager& manager
) {
    // TODO: Compute normalized fields from raw data
    // Example: dist_day_high = current_price - daily_high
}

bool EventSerializer::ValidateEventBinary(const std::vector<uint8_t>& binary) const {
    // Basic validation: check size and presence of FlatBuffer magic bytes
    if (binary.empty() || binary.size() > 65536) {
        return false;
    }

    // Try to access as Event (will throw if invalid)
    try {
        if (binary.size() < sizeof(uint32_t)) {
            return false;
        }

        // FlatBuffer uses offset at position 4-7 to find root table
        const auto* event = MTS::Schema::GetEvent(binary.data());
        if (!event) {
            return false;
        }

        // Verify sequence_id is reasonable (non-zero)
        uint64_t seq = event->sequence_id();
        if (seq == 0) {
            return false;  // Sequence should never be 0
        }

        return true;
    } catch (...) {
        return false;
    }
}
