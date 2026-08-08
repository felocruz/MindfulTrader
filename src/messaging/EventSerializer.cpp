#include "messaging/EventSerializer.h"
#include "IndicatorManager.h"
#include "ContextManager.h"
#include "Logger.h"
#include "generated/event_shared_writers_generated.h"
#include <chrono>
#include <cmath>
#include <algorithm>
#include <array>
#include <vector>

// Static singleton
static EventSerializer* g_eventSerializer = nullptr;

EventSerializer& EventSerializer::Instance() {
    if (g_eventSerializer == nullptr) {
        g_eventSerializer = new EventSerializer();
    }
    return *g_eventSerializer;
}

EventSerializer::EventSerializer()
    : m_fbb(std::make_unique<::flatbuffers::FlatBufferBuilder>(2048)) {}

EventSerializer::~EventSerializer() {
    m_fbb.reset();
}

std::vector<uint8_t> EventSerializer::SerializeEvent(
    const IndicatorManager& manager,
    int32_t bar_index,
    uint64_t timestamp_us,
    uint64_t sequence_id,
    const MTS::Schema::ObservationData* observation,
    const MTS::Schema::AsymmetryContext* asymmetry_context
) {
    std::vector<uint8_t> result;
    const uint8_t* buffer = nullptr;
    size_t size = 0;
    if (SerializeEventInPlace(manager, bar_index, timestamp_us, sequence_id, buffer, size, observation, asymmetry_context) && buffer && size > 0) {
        result.assign(buffer, buffer + size);
    }
    return result;
}

bool EventSerializer::SerializeEventInPlace(
    const IndicatorManager& manager,
    int32_t bar_index,
    uint64_t timestamp_us,
    uint64_t sequence_id,
    const uint8_t*& out_buffer,
    size_t& out_size,
    const MTS::Schema::ObservationData* observation,
    const MTS::Schema::AsymmetryContext* asymmetry_context
) {
    out_buffer = nullptr;
    out_size = 0;

    try {
        auto start_time = std::chrono::steady_clock::now();

        m_fbb->Clear();
        auto context = SnapshotContext(manager, timestamp_us);

        MTS::Schema::EventBuilder event_builder(*m_fbb);

        event_builder.add_sequence_id(sequence_id);
        event_builder.add_bar_index(bar_index);
        event_builder.add_timestamp_us(static_cast<int64_t>(timestamp_us));

        event_builder.add_delta_t_log(context.delta_t_log);
        event_builder.add_tau_100_log(context.tau_100_log);
        event_builder.add_log_event_velocity(context.log_event_velocity);

        event_builder.add_hmm_state(context.hmm_state);
        event_builder.add_market_climate(context.market_climate);

        event_builder.add_requires_inference(manager.CalculateRequiresInference());

        // Snapshot dirty mask for Python's authoritative change hints. Stale-
        // comment fix (indicator-manager-dod-soa plan, Task 9): this used to
        // need to run BEFORE PopulateIndicatorState, which cleared dirty bits
        // via ExtractInt8AndClearDirty(). Task 9 rewrote PopulateIndicatorState
        // to read m_packed directly with no dirty-clearing side effect, so the
        // ordering relative to the call below is no longer load-bearing.
        const uint64_t changed_mask = manager.GetDirtyMask();
        event_builder.add_changed_mask(changed_mask);

        // Zero-Copy IndicatorState
        MTS::Schema::IndicatorState indicators;
        manager.PopulateIndicatorState(indicators);

        // Export pattern quality scores from live indicator state.
        // DOD/SoA migration (Task 8): read straight from the packed Float32
        // array — no pointer, no null check, always a valid value. Each of
        // these keys is a two-row (Int8 + Float32) pair, so the explicit
        // (Key, Block) form is required.
        indicators.mutate_kangaroo_tail_quality(
            manager.GetValue<IndicatorKey::KANGAROO_TAIL, mts::StorageBlock::Float32>());
        indicators.mutate_turtle_soup_quality(
            manager.GetValue<IndicatorKey::TURTLE_SOUP, mts::StorageBlock::Float32>());
        indicators.mutate_momentum_pinball_quality(
            manager.GetValue<IndicatorKey::MOMENTUM_PINBALL, mts::StorageBlock::Float32>());
        indicators.mutate_elder_breakout_quality(
            manager.GetValue<IndicatorKey::ELDER_BREAKOUT, mts::StorageBlock::Float32>());
        indicators.mutate_nr7_quality(
            manager.GetValue<IndicatorKey::NR7, mts::StorageBlock::Float32>());

        // Context Fields (From SnapshotContext)
        indicators.mutate_corr_es_zn(context.corr_es_zn);
        indicators.mutate_corr_es_dx(context.corr_es_dx);
        indicators.mutate_zn_trend(context.zn_trend);
        indicators.mutate_dx_trend(context.dx_trend);
        indicators.mutate_corr_es_zn_delta(context.corr_es_zn_delta);
        indicators.mutate_corr_es_zn_accel(context.corr_es_zn_accel);
        indicators.mutate_corr_es_dx_delta(context.corr_es_dx_delta);
        indicators.mutate_corr_es_dx_accel(context.corr_es_dx_accel);

        // Robust z-score floats (Taleb fat-tail safe: median/MAD normalization).
        // DOD/SoA migration (Task 12): read straight from the packed array —
        // no pointers, no null checks, always valid values.
        indicators.mutate_interm_fi2_norm(manager.GetValue<IndicatorKey::INTERM_FI2_SIGNAL, mts::StorageBlock::Float32>());
        indicators.mutate_long_fi13_norm(manager.GetValue<IndicatorKey::LONG_FI13_SIGNAL, mts::StorageBlock::Float32>());
        // interm_macd_norm (INTERM_MACD Float32 position 8) and
        // impulse_run_length (INTERM_IMP Int8 position 26) are NOT migrated:
        // neither packed slot has a write side wired anywhere in the repo
        // (confirmed via AssertPackedStateParity's explicit skip of
        // {INTERM_IMP, 26} and the constructor's own "intentionally NOT
        // wired" comment for INTERM_MACD's Float32 companion). Reading them
        // via GetValue<>() would silently return 0 forever. Left on the
        // live leaf-object read, same as a NotPacked key, until a real write
        // side exists.
        const auto* intermMacd = manager.GetIndicator<Macd>(IndicatorKey::INTERM_MACD);
        indicators.mutate_interm_macd_norm(intermMacd ? intermMacd->ZScore() : 0.0f);
        const auto* intermImpulse = manager.GetIndicator<Impulse>(IndicatorKey::INTERM_IMP);
        indicators.mutate_impulse_run_length(static_cast<int8_t>(intermImpulse ? intermImpulse->RunLength() : 0));

        event_builder.add_indicators(&indicators);

        // Task 10 (indicator-manager-dod-soa plan): single canonical read of
        // every companion value shared with the training path, replacing this
        // function's own independent GetIndicator<T>()->GetX() calls.
        const auto companions = manager.GetTickCompanionValues();

        mts::schema_contract::shared_writers::WriteEventRootSharedFields(
            event_builder,
            mts::schema_contract::shared_writers::EventRootSharedSlice{
                companions.side,
                companions.marketSymbol,
                companions.overnightExit,
                companions.nhNlDaily,
                companions.prevHigh,
                companions.prevLow,
                companions.prevDayHigh,
                companions.prevDayLow,
                companions.prevFourBarHigh,
                companions.prevFourBarLow,
                companions.closePercentile,
                companions.volumeRatioPercent,
                companions.volumeImbalance});
        event_builder.add_open(0.0f);
        event_builder.add_high(0.0f);
        event_builder.add_low(0.0f);
        event_builder.add_close(0.0f);
        event_builder.add_atr_10(companions.atr10);
        event_builder.add_volume(0);

        // v5.5: Read bar-context floats from their owning indicators
        if (observation) {
            event_builder.add_observation(observation);
        }

        if (asymmetry_context) {
            event_builder.add_asymmetry_context(asymmetry_context);
        }

        auto event_offset = event_builder.Finish();
        m_fbb->Finish(event_offset);

        out_buffer = m_fbb->GetBufferPointer();
        out_size = m_fbb->GetSize();

        auto end_time = std::chrono::steady_clock::now();
        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time);

        m_lastSerializationTimeUs.store(static_cast<int32_t>(duration.count()));
        m_lastEventSizeBytes.store(static_cast<int32_t>(out_size));
        m_eventCount.fetch_add(1);

        return out_buffer != nullptr && out_size > 0;
    } catch (const std::exception& e) {
        Logger::getInstance().log(std::string("EventSerializer::SerializeEventInPlace exception: ") + e.what());
        out_buffer = nullptr;
        out_size = 0;
        return false;
    }
}

EventSerializer::ContextSnapshot EventSerializer::SnapshotContext(const IndicatorManager& manager, uint64_t timestamp_us) {
    // ===== ELITE TECHNIQUE: Context Manager Snapshot =====
    // Fetch all context fields ONCE and cache locally
    // Eliminates repeated ContextManager::Instance() calls
    // Result: 6× faster than 6+ separate singleton method calls

    ContextSnapshot snapshot{};

    try {
        const float eventVelocityPerSec = ContextManager::Instance().GetLastEventVelocityPerSec();
        snapshot.log_event_velocity = std::log1p(eventVelocityPerSec > 0.0f ? eventVelocityPerSec : 0.0f);
        UpdateTemporalPhysics(timestamp_us, snapshot.delta_t_log, snapshot.tau_100_log);

        // Fetch HMM State and Market Climate from InferenceManager
        const auto* hmmStateInd = InferenceManager::Instance().HmmState();
        snapshot.hmm_state = hmmStateInd ? hmmStateInd->intValue() : 0;

        const auto* climateInd = InferenceManager::Instance().MarketClimate();
        snapshot.market_climate = climateInd ? climateInd->intValue() : 0;


        // CORR_ES_ZN_DELTA/ACCEL and CORR_ES_DX_DELTA/ACCEL remain NotPacked
        // (Task 2 audit): "Not directly exported — Python calculates them" —
        // out of this task's scope, still pointer-based.
        const auto* znTrend = manager.GetIndicator<CrossMarketTrend>(IndicatorKey::ZN_TREND);
        const auto* dxTrend = manager.GetIndicator<CrossMarketTrend>(IndicatorKey::DX_TREND);
        const auto* corrEsZnDelta = manager.GetIndicator<CorrelationIndicator>(IndicatorKey::CORR_ES_ZN_DELTA);
        const auto* corrEsZnAccel = manager.GetIndicator<CorrelationIndicator>(IndicatorKey::CORR_ES_ZN_ACCEL);
        const auto* corrEsDxDelta = manager.GetIndicator<CorrelationIndicator>(IndicatorKey::CORR_ES_DX_DELTA);
        const auto* corrEsDxAccel = manager.GetIndicator<CorrelationIndicator>(IndicatorKey::CORR_ES_DX_ACCEL);

        // DOD/SoA migration (Task 7/8): read straight from the packed array —
        // no pointer, no null check, always a valid value. CORR_ES_ZN/
        // CORR_ES_DX are single-row (Float32-only, no Int8 companion), so the
        // single-key GetValue<Key>() form resolves unambiguously.
        snapshot.oscillator_310 = manager.GetValue<IndicatorKey::OSCILLATOR_310>();
        snapshot.corr_es_zn = manager.GetValue<IndicatorKey::CORR_ES_ZN>();
        snapshot.corr_es_dx = manager.GetValue<IndicatorKey::CORR_ES_DX>();
        snapshot.zn_trend = znTrend ? static_cast<int8_t>(znTrend->intValue()) : 0;
        snapshot.dx_trend = dxTrend ? static_cast<int8_t>(dxTrend->intValue()) : 0;
        snapshot.corr_es_zn_delta = corrEsZnDelta ? corrEsZnDelta->Value() : 0.0f;
        snapshot.corr_es_zn_accel = corrEsZnAccel ? corrEsZnAccel->Value() : 0.0f;
        snapshot.corr_es_dx_delta = corrEsDxDelta ? corrEsDxDelta->Value() : 0.0f;
        snapshot.corr_es_dx_accel = corrEsDxAccel ? corrEsDxAccel->Value() : 0.0f;

        // prev_high/prev_low/prev_day_high/prev_day_low/prev_four_bar_high/
        // prev_four_bar_low moved to IndicatorManager::GetTickCompanionValues()
        // (Task 10, indicator-manager-dod-soa plan) — SerializeEventInPlace reads
        // them from there now instead of this function duplicating the same
        // ShortMarketAction/daily-cache reads.

    } catch (const std::exception& e) {
        Logger::getInstance().log(std::string("SnapshotContext exception: ") + e.what());
    }

    return snapshot;
}

void EventSerializer::UpdateTemporalPhysics(uint64_t timestamp_us, float& out_delta_t_log, float& out_tau_100_log) {
    uint64_t delta_us = 0;
    if (m_lastEventTimestampUs > 0 && timestamp_us > m_lastEventTimestampUs) {
        delta_us = timestamp_us - m_lastEventTimestampUs;
        m_recentDeltaUs.push_back(delta_us);
        if (m_recentDeltaUs.size() > kTauWindowSize) {
            m_recentDeltaUs.pop_front();
        }
    }

    uint64_t tau_median_us = delta_us;
    if (!m_recentDeltaUs.empty()) {
        // Only the filled prefix (n, not the buffer's full kTauWindowSize
        // capacity) participates in the median — unfilled tail slots are
        // zero-initialized and would bias nth_element toward zero.
        const size_t n = m_recentDeltaUs.size();
        std::array<uint64_t, kTauWindowSize> scratch{};
        for (size_t i = 0; i < n; ++i) {
            scratch[i] = m_recentDeltaUs[i];
        }
        const size_t mid = n / 2;
        std::nth_element(scratch.begin(), scratch.begin() + static_cast<std::ptrdiff_t>(mid), scratch.begin() + static_cast<std::ptrdiff_t>(n));
        tau_median_us = scratch[mid];
    }

    out_delta_t_log = std::log1p(static_cast<float>(delta_us));
    out_tau_100_log = std::log1p(static_cast<float>(tau_median_us));
    m_lastEventTimestampUs = timestamp_us;
}

bool EventSerializer::ValidateEventBinary(const std::vector<uint8_t>& binary) const {
    // TODO: Implement FlatBuffer validation when generated Event::GetRootAsEvent is available
    // Basic check: validate size range
    if (binary.empty() || binary.size() > 65536) {
        return false;
    }
    return true;
}
// Force change Tue Mar  3 13:13:04 EST 2026
